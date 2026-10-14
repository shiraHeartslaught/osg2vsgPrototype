#pragma once

#include <functional>
#include <future>
#include <optional>
#include <string>
#include <vector>

namespace ivevsg
{
    /// One entry of a file picker's type filter, e.g. {"Scene databases", "ive,vsgb,vsgt,osgb,osgt"}.
    struct FileFilter
    {
        std::string name;
        std::string extensions; // comma separated, no dots
    };

    struct FileDialogResult
    {
        bool accepted = false; // false on cancel (error stays empty) or failure (error set)
        std::string path;
        std::string error;

        explicit operator bool() const { return accepted; }
    };

    /// The filters the viewer offers when adding a layer: paged/scene databases first, then anything.
    std::vector<FileFilter> sceneFileFilters();

    /// Opens the platform's own file picker (on Linux through the xdg-desktop-portal, so KDE shows its
    /// dialog) and blocks until the user picks a file or cancels. Rendering pauses meanwhile, which is
    /// fine for a modal choice. defaultDirectory may be empty.
    FileDialogResult openFileDialog(const std::vector<FileFilter>& filters, const std::string& defaultDirectory = {});

    /// Runs a file picker on a worker thread so the viewer keeps rendering while it is open. open()
    /// starts it, busy() says whether it is still up, poll() hands back the result exactly once.
    /// The opener is injectable (defaults to openFileDialog) so the plumbing can be tested headless.
    class AsyncFileDialog
    {
    public:
        using Opener = std::function<FileDialogResult(const std::vector<FileFilter>&, const std::string&)>;

        explicit AsyncFileDialog(Opener opener = {});
        ~AsyncFileDialog();
        AsyncFileDialog(const AsyncFileDialog&) = delete;
        AsyncFileDialog& operator=(const AsyncFileDialog&) = delete;

        /// False (and nothing happens) while a dialog is already open.
        bool open(std::vector<FileFilter> filters, std::string defaultDirectory = {});
        bool busy() const;
        std::optional<FileDialogResult> poll();

    private:
        Opener _opener;
        std::future<FileDialogResult> _future;
    };
} // namespace ivevsg
