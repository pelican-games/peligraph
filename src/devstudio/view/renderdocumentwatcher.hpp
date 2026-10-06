#pragma once

#include <QFileSystemWatcher>
#include <QStringList>

#include <filesystem>
#include <vector>

namespace PelicanStudio {

class RenderDocumentReload;

// The window's render documents and initial assets, independently of any
// panel. Reload policy (digests, debounce and retries) stays in the model.
class RenderDocumentWatcher final {
  public:
    explicit RenderDocumentWatcher(RenderDocumentReload &reload);
    RenderDocumentWatcher(const RenderDocumentWatcher &) = delete;
    RenderDocumentWatcher &operator=(const RenderDocumentWatcher &) = delete;

    void setFiles(const std::vector<std::filesystem::path> &files);

  private:
    void rewatch();

    RenderDocumentReload &reload_;
    QFileSystemWatcher watcher_;
    QStringList files_;
};

} // namespace PelicanStudio
