#pragma once

#include "../model/audioanalysistables.hpp"
#include "graphcanvasreadcapability.hpp"

#include <QDialog>

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

class QByteArray;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;
class QTimer;

namespace PelicanStudio {

class GraphCanvasModel;

// AB-1s, decision 1: what a sound's analysis is baked with. Features (RMS /
// Peak / Bands), how many bands (1..128, 8), the hop, the smoothing's attack
// and release, the channels (Mix / Left / Right), and where the table goes
// (assets/analysis/<the sound's name>.ktx2 unless changed). Named
// `pelican.audioBakeDialog` and `pelican.audioBakeDialog.<field>`: rms, peak,
// bands, bandCount, hop, attack, release, channels, out, bake, cancel.
class GraphCanvasBakeDialog final : public QDialog {
  public:
    GraphCanvasBakeDialog(const QString &sound, const QString &out, QWidget *parent = nullptr);
    [[nodiscard]] AudioBakeChoice choice() const;
    [[nodiscard]] QString out() const;

  private:
    void follow();
    QCheckBox *rms_ = nullptr;
    QCheckBox *peak_ = nullptr;
    QCheckBox *bands_ = nullptr;
    QSpinBox *band_count_ = nullptr;
    QDoubleSpinBox *hop_ = nullptr;
    QDoubleSpinBox *attack_ = nullptr;
    QDoubleSpinBox *release_ = nullptr;
    QComboBox *channels_ = nullptr;
    QLineEdit *out_ = nullptr;
    QPushButton *bake_ = nullptr;
};

// AB-1s: the Graph tab's side of a baked analysis (engine AB-1).
//   - decision 1: Bake analysis... opens the dialog and sends
//     bake_audio_analysis; while it runs the bake row says "Baking <sound>...
//     (n s)", when it is committed "Baked <out>: <features> × <n> samples",
//     a refusal one sentence (audioAnalysisRefusalDescription);
//   - decisions 2 and 3: the committed bake's row has Use in Graph and Use
//     in a shader, which the owner answers (`useInGraph`, `useInShader`),
//     and, after a shader use, the line to copy;
//   - decision 4: list_derived_assets, read when asked (the project opened,
//     the Graph read again), says which tables are stale; the stale row of
//     a node reading one says why and offers Re-bake (the manifest's own
//     settings). UX-13a, decision 3: nothing is edited after it -- the
//     rewritten table is a file the reload watches, and the engine reads it
//     again in a new generation (IA-3, "Picked up the change to <table>").
// The bake row is `pelican.graphCanvasBake` (`.line`, `.useInGraph`,
// `.useInShader`, `.shaderLine`, `.copy`), the stale row
// `pelican.graphCanvasStale` (`.line`, `.rebake`).
class GraphCanvasAudioAnalysis {
  public:
    struct Hooks {
        std::function<void()> notice;
        std::function<void()> staleChanged;
        std::function<void(const GraphCanvasAnalysisTable &table)> useInGraph;
        std::function<void(const GraphCanvasAnalysisTable &table)> useInShader;
    };
    GraphCanvasAudioAnalysis(GraphCanvasReadCapability &graph, GraphCanvasModel &model, QWidget &owner,
                             Hooks hooks);
    ~GraphCanvasAudioAnalysis();

    [[nodiscard]] QWidget *bakeRow() const noexcept { return bake_row_; }
    [[nodiscard]] QWidget *staleRow() const noexcept { return stale_row_; }
    void setProjectRoot(const std::filesystem::path &root) { project_root_ = root; }
    [[nodiscard]] bool available() const { return graph_.requestBakeAudioAnalysis != nullptr; }
    [[nodiscard]] bool running() const noexcept { return started_.has_value(); }

    // Decision 1: the dialog for `sound` (a project:// reference), answered
    // as it closes.
    void chooseBake(const std::string &sound);
    // bake_audio_analysis with these params ({asset, out, settings}).
    bool bake(const nlohmann::json &params);
    // Decision 4: Re-bake of the table `asset`, from its manifest.
    bool rebake(const std::string &asset);
    // Decision 4: list_derived_assets, once the engine answers.
    void readDerived();
    // The table's entry of the last list, or nullptr.
    [[nodiscard]] const GraphCanvasDerivedAsset *derived(std::string_view asset) const;
    // The node picked in the side fields: its stale row (hidden for a node
    // that reads no stale table).
    void showNode(const std::string &table_asset, bool editable);
    // Decision 3: the shader line to copy, under the bake row.
    void showShaderLine(const std::string &line, const std::string &notice);

    bool deliverResult(qint64 id, const QByteArray &bytes);
    bool deliverFailure(qint64 id, const QString &message);

  private:
    void requestTicket();
    void finished(const nlohmann::json &response);
    void say(bool error, const std::string &line);
    void tick();

    GraphCanvasReadCapability &graph_;
    GraphCanvasModel &model_;
    QWidget &owner_;
    Hooks hooks_;
    std::filesystem::path project_root_;

    QWidget *bake_row_ = nullptr;
    QLabel *bake_line_ = nullptr;
    QPushButton *use_in_graph_ = nullptr;
    QPushButton *use_in_shader_ = nullptr;
    QLineEdit *shader_line_ = nullptr;
    QPushButton *copy_ = nullptr;
    QWidget *stale_row_ = nullptr;
    QLabel *stale_line_ = nullptr;
    QPushButton *rebake_ = nullptr;
    QTimer *clock_ = nullptr;

    // The bake in flight: its request, its ticket, what it bakes, since when.
    qint64 bake_request_ = 0;
    qint64 ticket_request_ = 0;
    QString ticket_;
    std::string sound_;
    std::string out_;
    std::optional<std::chrono::steady_clock::time_point> started_;
    // The last committed bake's table (decisions 2 and 3).
    std::optional<GraphCanvasAnalysisTable> baked_;

    qint64 derived_request_ = 0;
    bool derived_again_ = false;
    std::vector<GraphCanvasDerivedAsset> derived_;
    std::string shown_asset_;
};

} // namespace PelicanStudio
