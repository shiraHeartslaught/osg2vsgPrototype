#include <ivevsg/AsyncLayerLoader.h>
#include <ivevsg/FileDialog.h>
#include <ivevsg/SceneLoader.h>

#include <gtest/gtest.h>
#include <vsg/all.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <thread>

namespace
{
    vsg::ref_ptr<vsg::Node> makeTriangleScene()
    {
        auto draw = vsg::VertexIndexDraw::create();
        draw->assignArrays(vsg::DataList{vsg::vec3Array::create({{0.0f, 0.0f, 0.0f}, {2.0f, 0.0f, 0.0f}, {0.0f, 3.0f, 0.0f}})});
        draw->assignIndices(vsg::ushortArray::create({0, 1, 2}));
        draw->indexCount = 3;
        draw->instanceCount = 1;
        auto group = vsg::Group::create();
        group->addChild(draw);
        return group;
    }

    template<typename Predicate>
    bool waitFor(Predicate predicate, std::chrono::milliseconds timeout = std::chrono::seconds(20))
    {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (!predicate())
        {
            if (std::chrono::steady_clock::now() > deadline) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        return true;
    }
} // namespace

TEST(AsyncLayerLoader, LoadsAndCompilesOffTheCallingThreadAndDeliversOnce)
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ivevsg_async_layer.vsgt";
    ASSERT_TRUE(vsg::write(makeTriangleScene(), vsg::Path(file.string())));

    auto loader = std::make_shared<ivevsg::SceneLoader>();
    std::atomic<int> compiles{0};
    const std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<bool> compiledOnWorker{false};
    ivevsg::AsyncLayerLoader async(loader, [&](vsg::ref_ptr<vsg::Node>) {
        ++compiles;
        compiledOnWorker = std::this_thread::get_id() != mainThread;
        return vsg::CompileResult{};
    });

    async.request(file.string());
    EXPECT_EQ(async.pendingCount(), 1u);

    std::vector<ivevsg::LayerLoadOutcome> done;
    ASSERT_TRUE(waitFor([&] { auto d = async.takeCompleted(); done.insert(done.end(), d.begin(), d.end()); return !done.empty(); }));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_TRUE(done[0].ok()) << done[0].error;
    EXPECT_EQ(done[0].name, "ivevsg_async_layer");
    EXPECT_EQ(done[0].path, file.string());
    EXPECT_TRUE(done[0].node);
    EXPECT_GE(done[0].seconds, 0.0);
    EXPECT_EQ(compiles.load(), 1);
    EXPECT_TRUE(compiledOnWorker.load());
    EXPECT_EQ(async.pendingCount(), 0u);
    EXPECT_TRUE(async.takeCompleted().empty()); // delivered once

    std::filesystem::remove(file);
}

TEST(AsyncLayerLoader, ReportsMissingFilesAsFailedOutcomes)
{
    auto loader = std::make_shared<ivevsg::SceneLoader>();
    ivevsg::AsyncLayerLoader async(loader);
    async.request("/nonexistent/layer.ive", "ghost");
    std::vector<ivevsg::LayerLoadOutcome> done;
    ASSERT_TRUE(waitFor([&] { auto d = async.takeCompleted(); done.insert(done.end(), d.begin(), d.end()); return !done.empty(); }));
    ASSERT_EQ(done.size(), 1u);
    EXPECT_FALSE(done[0].ok());
    EXPECT_EQ(done[0].name, "ghost");
    EXPECT_FALSE(done[0].error.empty());
    EXPECT_FALSE(done[0].node);
}

TEST(AsyncLayerLoader, ListsInFlightLoadsByName)
{
    const std::filesystem::path file = std::filesystem::temp_directory_path() / "ivevsg_async_inflight.vsgt";
    ASSERT_TRUE(vsg::write(makeTriangleScene(), vsg::Path(file.string())));

    auto loader = std::make_shared<ivevsg::SceneLoader>();
    std::atomic<bool> release{false};
    ivevsg::AsyncLayerLoader async(loader, [&](vsg::ref_ptr<vsg::Node>) {
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1)); // hold the job "in flight"
        return vsg::CompileResult{};
    });
    async.request(file.string(), "slow one");
    ASSERT_TRUE(waitFor([&] { return async.pendingCount() == 1; }));
    const std::vector<std::string> names = async.inFlight();
    ASSERT_EQ(names.size(), 1u);
    EXPECT_EQ(names[0], "slow one");
    EXPECT_TRUE(async.takeCompleted().empty()); // not finished yet, and this never blocks
    release = true;
    ASSERT_TRUE(waitFor([&] { return !async.takeCompleted().empty() || async.pendingCount() == 0; }));

    std::filesystem::remove(file);
}

TEST(AsyncFileDialog, RunsTheOpenerOffThreadAndDeliversTheResultOnce)
{
    std::atomic<bool> release{false};
    const std::thread::id mainThread = std::this_thread::get_id();
    std::atomic<bool> openedOnWorker{false};
    ivevsg::AsyncFileDialog dialog([&](const std::vector<ivevsg::FileFilter>& filters, const std::string& dir) {
        openedOnWorker = std::this_thread::get_id() != mainThread;
        while (!release) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ivevsg::FileDialogResult result;
        result.accepted = true;
        result.path = dir + "/picked." + (filters.empty() ? "none" : filters[0].extensions.substr(0, 3));
        return result;
    });

    EXPECT_FALSE(dialog.busy());
    EXPECT_FALSE(dialog.poll().has_value());
    EXPECT_TRUE(dialog.open(ivevsg::sceneFileFilters(), "/data"));
    EXPECT_TRUE(dialog.busy());
    EXPECT_FALSE(dialog.open({}, {})); // one at a time
    EXPECT_FALSE(dialog.poll().has_value()); // still open: never blocks, nothing yet

    release = true;
    std::optional<ivevsg::FileDialogResult> result;
    ASSERT_TRUE(waitFor([&] { result = dialog.poll(); return result.has_value(); }));
    EXPECT_TRUE(result->accepted);
    EXPECT_EQ(result->path, "/data/picked.ive");
    EXPECT_TRUE(openedOnWorker.load());
    EXPECT_FALSE(dialog.busy());
    EXPECT_FALSE(dialog.poll().has_value()); // delivered once
    EXPECT_TRUE(dialog.open({}, {}));         // and can be opened again
}

TEST(AsyncFileDialog, CancelAndErrorComeBackAsNonAcceptedResults)
{
    ivevsg::AsyncFileDialog cancelled([](const std::vector<ivevsg::FileFilter>&, const std::string&) { return ivevsg::FileDialogResult{}; });
    ASSERT_TRUE(cancelled.open({}, {}));
    std::optional<ivevsg::FileDialogResult> result;
    ASSERT_TRUE(waitFor([&] { result = cancelled.poll(); return result.has_value(); }));
    EXPECT_FALSE(result->accepted);
    EXPECT_TRUE(result->error.empty());

    ivevsg::AsyncFileDialog failed([](const std::vector<ivevsg::FileFilter>&, const std::string&) {
        ivevsg::FileDialogResult r;
        r.error = "portal unavailable";
        return r;
    });
    ASSERT_TRUE(failed.open({}, {}));
    ASSERT_TRUE(waitFor([&] { result = failed.poll(); return result.has_value(); }));
    EXPECT_FALSE(result->accepted);
    EXPECT_EQ(result->error, "portal unavailable");
}
