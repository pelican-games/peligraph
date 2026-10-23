#include "renderdocumentwatcher.hpp"
#include "../model/renderdocumentreload.hpp"

#include <QFileInfo>
#include <QObject>

namespace PelicanStudio {

RenderDocumentWatcher::RenderDocumentWatcher(RenderDocumentReload &reload)
    : reload_{reload} {
    QObject::connect(&watcher_, &QFileSystemWatcher::fileChanged, &watcher_,
                     [this](const QString &path) {
                         // Replacement saves can leave a stale entry listed.
                         // Remove it explicitly and add the current file anew.
                         watcher_.removePath(path);
                         rewatch();
                         reload_.fileChanged();
                     });
    QObject::connect(&watcher_, &QFileSystemWatcher::directoryChanged, &watcher_,
                     [this](const QString &directory) {
                         // Keep the parent watched while a document is absent;
                         // its recreation must restore the file watch too.
                         for (const QString &path : watcher_.files())
                             if (QFileInfo(path).absolutePath() == directory)
                                 watcher_.removePath(path);
                         rewatch();
                         reload_.fileChanged();
                     });
}

void RenderDocumentWatcher::setFiles(
    const std::vector<std::filesystem::path> &files) {
    files_.clear();
    for (const auto &path : files) {
        const QString text =
            QFileInfo(QString::fromStdWString(path.wstring())).absoluteFilePath();
        if (!files_.contains(text)) files_.push_back(text);
    }
    rewatch();
}

void RenderDocumentWatcher::rewatch() {
    QStringList directories;
    for (const QString &path : files_) {
        const QString parent = QFileInfo(path).absolutePath();
        if (!directories.contains(parent)) directories.push_back(parent);
    }
    const QStringList watched = watcher_.files();
    for (const QString &path : watched)
        if (!files_.contains(path)) watcher_.removePath(path);
    for (const QString &path : files_)
        if (!watched.contains(path) && QFileInfo::exists(path))
            watcher_.addPath(path);

    const QStringList watched_directories = watcher_.directories();
    for (const QString &path : watched_directories)
        if (!directories.contains(path)) watcher_.removePath(path);
    for (const QString &path : directories)
        if (!watched_directories.contains(path) && QFileInfo(path).isDir())
            watcher_.addPath(path);
}

} // namespace PelicanStudio
