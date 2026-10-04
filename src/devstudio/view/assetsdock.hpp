#pragma once

#include "../model/assetsdockmodel.hpp"

#include <QByteArray>
#include <QJsonObject>
#include <QJsonValue>
#include <QString>
#include <QWidget>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

class QAction;
class QLabel;
class QPushButton;
class QTreeWidget;

namespace PelicanStudio {

// The tree's rows carry their project path and the key of their file.
inline constexpr int AssetLogicalPathRole = Qt::UserRole + 1;
inline constexpr int AssetPhysicalKeyRole = Qt::UserRole + 2;

// HW-2s / AV-1s. The Assets dock (beside Project): the asset folders as a
// tree, "Reload when saved outside" on every row (the Reload column's box,
// and the row's right-click), the line above the tree that says what is
// watched and what was read again last, and under it Track asset contents,
// the result of the contents check, Verify again and each store's Update
// manifest. AssetsDockSession holds what the engine said; this is how it is
// shown.
//
// The folders are followed with a QFileSystemWatcher (their folders, and the
// files of the watched ones), read again RescanDebounceMs after the last
// change. Nothing reads on a clock while nothing changes.
class AssetsDock final : public QWidget {
  public:
    struct Hooks {
        // Sends one editor request; the id its reply comes back under, or 0
        // with `error` saying why.
        std::function<qint64(const QString &method, const QJsonObject &params, QString *error)>
            send;
        // An edit's restart_required: the window's engine restart.
        std::function<void(const QString &reason)> restart;
        // The render documents the engine reads (RL-1s), for their rows'
        // tooltip: the watch does not apply them.
        std::function<std::vector<std::filesystem::path>()> render_documents;
        // PB-1s: what the contents check found, each time the dock shows it
        // (the Problems dock lists its warnings and errors).
        std::function<void(const std::vector<AssetStoreVerification> &stores)> checked;
        // MI-1s, decision 4: Remove from project... on a registered model's
        // row. The model whose file is the row at `logical` (as the project
        // names it, "assets/models/box.glb"), if any; whether it has material
        // bindings (`answered` once that is known); and unregister_model,
        // `said` with the one sentence that tells how it went.
        std::function<std::optional<std::string>(const std::string &logical)> model_at;
        std::function<void(const std::string &model, std::function<void(bool bound)> answered)>
            model_bindings;
        std::function<void(const std::string &model, std::function<void(const std::string &line)> said)>
            remove_model;
    };

    static constexpr int RescanDebounceMs = 250;
    // The files of watched folders followed one by one, at most.
    static constexpr int WatchedFileLimit = 512;

    explicit AssetsDock(Hooks hooks, QWidget *parent = nullptr);
    ~AssetsDock() override;

    // Another project (an empty root: none).
    void open(const std::filesystem::path &project_root);
    void engineAvailable();
    void engineUnavailable();

    // Every get_status result, whoever asked (the viewport's one parse).
    void observeStatus(qint64 request_id, const nlohmann::json &status);
    // The replies to what this dock sent. False for an id that is not its own.
    bool receiveResult(qint64 request_id, const QByteArray &result_json);
    bool receiveRefusal(qint64 request_id, int code, const QString &message,
                        const QJsonValue &data);
    bool receiveFailure(qint64 request_id, const QString &message);

    // Reads the folders again now (what the watcher does after its debounce).
    void rescan();
    // PB-1s, decision 3: the row of the file at `path` (on disk) selected and
    // scrolled to, its folders opened. False when the tree has no such row.
    bool revealFile(const std::filesystem::path &path);

    [[nodiscard]] AssetsDockSession &session() noexcept;
    [[nodiscard]] QTreeWidget *tree() const noexcept;
    [[nodiscard]] QAction *reloadAction() const noexcept;
    [[nodiscard]] QAction *undoAction() const noexcept;
    [[nodiscard]] QAction *removeModelAction() const noexcept;
    [[nodiscard]] QLabel *watchLine() const noexcept;
    [[nodiscard]] QLabel *noticeLine() const noexcept;
    [[nodiscard]] QLabel *verificationLine() const noexcept;
    [[nodiscard]] QPushButton *trackButton() const noexcept;
    [[nodiscard]] QPushButton *verifyButton() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
