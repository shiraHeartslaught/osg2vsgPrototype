#include <ivevsg/FileDialog.h>

#include <nfd.h>

#include <chrono>
#include <cstdlib>
#include <mutex>
#include <utility>

namespace ivevsg
{
    std::vector<FileFilter> sceneFileFilters()
    {
        return {
            {"Scene databases", "ive,vsgb,vsgt,osgb,osgt,osg"},
            {"OpenSceneGraph", "ive,osgb,osgt,osg"},
            {"VulkanSceneGraph", "vsgb,vsgt"},
            {"3D models", "gltf,glb,obj,fbx,dae,3ds"},
        };
    }

    namespace
    {
        /// NFD wants one NFD_Init() per process and NFD_Quit() at the end.
        bool ensureInitialised()
        {
            static std::once_flag once;
            static bool ok = false;
            std::call_once(once, [] {
                ok = NFD_Init() == NFD_OKAY;
                if (ok) std::atexit([] { NFD_Quit(); });
            });
            return ok;
        }
    } // namespace

    FileDialogResult openFileDialog(const std::vector<FileFilter>& filters, const std::string& defaultDirectory)
    {
        FileDialogResult result;
        if (!ensureInitialised())
        {
            result.error = std::string("file dialog unavailable: ") + (NFD_GetError() ? NFD_GetError() : "NFD_Init failed");
            return result;
        }

        std::vector<nfdnfilteritem_t> items;
        items.reserve(filters.size());
        for (const FileFilter& filter : filters) items.push_back(nfdnfilteritem_t{filter.name.c_str(), filter.extensions.c_str()});

        nfdnchar_t* outPath = nullptr;
        const nfdresult_t status = NFD_OpenDialogN(&outPath, items.empty() ? nullptr : items.data(),
                                                   static_cast<nfdfiltersize_t>(items.size()),
                                                   defaultDirectory.empty() ? nullptr : defaultDirectory.c_str());
        switch (status)
        {
        case NFD_OKAY:
            result.accepted = true;
            result.path = outPath ? outPath : "";
            if (outPath) NFD_FreePathN(outPath);
            break;
        case NFD_CANCEL:
            break;
        default:
            result.error = NFD_GetError() ? NFD_GetError() : "file dialog failed";
            NFD_ClearError();
            break;
        }
        return result;
    }

    AsyncFileDialog::AsyncFileDialog(Opener opener) :
        _opener(opener ? std::move(opener) : Opener(&openFileDialog))
    {
    }

    AsyncFileDialog::~AsyncFileDialog()
    {
        if (_future.valid()) _future.wait();
    }

    bool AsyncFileDialog::open(std::vector<FileFilter> filters, std::string defaultDirectory)
    {
        if (busy()) return false;
        auto opener = _opener;
        _future = std::async(std::launch::async, [opener, filters = std::move(filters), defaultDirectory = std::move(defaultDirectory)]() {
            return opener(filters, defaultDirectory);
        });
        return true;
    }

    bool AsyncFileDialog::busy() const
    {
        return _future.valid() && _future.wait_for(std::chrono::seconds(0)) != std::future_status::ready;
    }

    std::optional<FileDialogResult> AsyncFileDialog::poll()
    {
        if (!_future.valid() || _future.wait_for(std::chrono::seconds(0)) != std::future_status::ready) return std::nullopt;
        return _future.get(); // get() leaves the future invalid, so the result is delivered once
    }
} // namespace ivevsg
