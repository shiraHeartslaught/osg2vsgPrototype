#pragma once

#include <vsg/app/CompileManager.h>
#include <vsg/core/ref_ptr.h>
#include <vsg/nodes/Node.h>

#include <cstddef>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace ivevsg
{
    class SceneLoader;

    struct LayerLoadOutcome
    {
        std::string path;
        std::string name;
        vsg::ref_ptr<vsg::Node> node;
        vsg::CompileResult compileResult; // from the compile step, to hand to vsg::updateViewer() on the main thread
        std::string error;
        double seconds = 0.0;

        bool ok() const { return node.valid() && error.empty(); }
    };

    /// Loads layer files on worker threads so rendering never waits for disk reads, OSG conversion or
    /// GPU compilation. Each request runs read -> convert -> compile (the compile callback is expected
    /// to call the viewer's CompileManager, which is thread safe) on its own std::async thread; the
    /// main thread collects finished outcomes with takeCompleted() between frames and adds them to the
    /// scene after vsg::updateViewer(). The destructor waits for in-flight loads.
    class AsyncLayerLoader
    {
    public:
        using CompileFunction = std::function<vsg::CompileResult(vsg::ref_ptr<vsg::Node>)>;

        AsyncLayerLoader(std::shared_ptr<const SceneLoader> loader, CompileFunction compile = {});
        ~AsyncLayerLoader();
        AsyncLayerLoader(const AsyncLayerLoader&) = delete;
        AsyncLayerLoader& operator=(const AsyncLayerLoader&) = delete;

        /// Starts loading; name defaults to the file's simple name.
        void request(const std::string& path, const std::string& name = {});

        /// Finished loads, successful or not, removed from the queue. Never blocks.
        std::vector<LayerLoadOutcome> takeCompleted();

        /// Names of loads still running.
        std::vector<std::string> inFlight() const;
        std::size_t pendingCount() const;

    private:
        struct Job
        {
            std::string path;
            std::string name;
            std::future<LayerLoadOutcome> future;
        };

        std::shared_ptr<const SceneLoader> _loader;
        CompileFunction _compile;
        mutable std::mutex _mutex;
        std::vector<Job> _jobs;
    };
} // namespace ivevsg
