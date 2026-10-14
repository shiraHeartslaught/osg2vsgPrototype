#include <ivevsg/AsyncLayerLoader.h>
#include <ivevsg/SceneLoader.h>

#include <vsg/all.h>

#include <chrono>

namespace ivevsg
{
    AsyncLayerLoader::AsyncLayerLoader(std::shared_ptr<const SceneLoader> loader, CompileFunction compile) :
        _loader(std::move(loader)),
        _compile(std::move(compile))
    {
    }

    AsyncLayerLoader::~AsyncLayerLoader()
    {
        std::vector<Job> jobs;
        {
            std::lock_guard<std::mutex> lock(_mutex);
            jobs.swap(_jobs);
        }
        for (auto& job : jobs)
        {
            if (job.future.valid()) job.future.wait();
        }
    }

    void AsyncLayerLoader::request(const std::string& path, const std::string& name)
    {
        std::string layerName = name.empty() ? vsg::simpleFilename(vsg::Path(path)).string() : name;
        auto loader = _loader;
        auto compile = _compile;

        auto future = std::async(std::launch::async, [loader, compile, path, layerName]() -> LayerLoadOutcome {
            const auto start = std::chrono::steady_clock::now();
            LayerLoadOutcome outcome;
            outcome.path = path;
            outcome.name = layerName;
            if (!loader)
            {
                outcome.error = "no loader";
            }
            else
            {
                const LoadResult loaded = loader->load(path);
                if (!loaded)
                {
                    outcome.error = loaded.error;
                }
                else
                {
                    outcome.node = loaded.node;
                    if (compile)
                    {
                        try
                        {
                            outcome.compileResult = compile(loaded.node);
                        }
                        catch (const vsg::Exception& e)
                        {
                            outcome.error = "compile failed: " + e.message;
                            outcome.node = {};
                        }
                    }
                }
            }
            outcome.seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
            return outcome;
        });

        std::lock_guard<std::mutex> lock(_mutex);
        _jobs.push_back(Job{path, layerName, std::move(future)});
    }

    std::vector<LayerLoadOutcome> AsyncLayerLoader::takeCompleted()
    {
        std::vector<LayerLoadOutcome> completed;
        std::lock_guard<std::mutex> lock(_mutex);
        for (auto it = _jobs.begin(); it != _jobs.end();)
        {
            if (it->future.valid() && it->future.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
            {
                completed.push_back(it->future.get());
                it = _jobs.erase(it);
            }
            else
            {
                ++it;
            }
        }
        return completed;
    }

    std::vector<std::string> AsyncLayerLoader::inFlight() const
    {
        std::vector<std::string> names;
        std::lock_guard<std::mutex> lock(_mutex);
        for (const auto& job : _jobs) names.push_back(job.name);
        return names;
    }

    std::size_t AsyncLayerLoader::pendingCount() const
    {
        std::lock_guard<std::mutex> lock(_mutex);
        return _jobs.size();
    }
} // namespace ivevsg
