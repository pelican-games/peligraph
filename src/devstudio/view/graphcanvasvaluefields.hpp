#pragma once

#include "../model/graphcanvasmodel.hpp"
#include "graphcanvasrenderfields.hpp"

#include <QWidget>

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QPushButton;
class QTableWidget;
class QToolButton;

namespace PelicanStudio {

// UX-8, decision 4: the small curve over a value.curve's Keys table. A key
// is a point; a drag moves it (Shift: its time only, Ctrl: its value only),
// held between its neighbours, and letting go is one edit. Named
// `pelican.graphCanvasCurve.graph`.
class GraphCanvasCurveGraph final : public QWidget {
  public:
    using Commit = std::function<void(const std::vector<GraphCanvasCurveKey> &)>;
    explicit GraphCanvasCurveGraph(Commit commit, QWidget *parent = nullptr);
    void setKeys(std::vector<GraphCanvasCurveKey> keys, bool editable);
    [[nodiscard]] const std::vector<GraphCanvasCurveKey> &keys() const noexcept { return keys_; }
    // Where key `index` is drawn, in this widget's coordinates.
    [[nodiscard]] QPointF keyPoint(std::size_t index) const;
    [[nodiscard]] QSize sizeHint() const override;

  protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;

  private:
    struct Frame {
        double t0 = 0;
        double t1 = 1;
        double v0 = 0;
        double v1 = 1;
    };
    [[nodiscard]] Frame frameOf(const std::vector<GraphCanvasCurveKey> &keys) const;
    [[nodiscard]] QRectF plot() const;
    [[nodiscard]] QPointF pointOf(const Frame &frame, double time, double value) const;

    Commit commit_;
    std::vector<GraphCanvasCurveKey> keys_;
    bool editable_ = false;
    // While a key is held: the keys as the press found them, the frame the
    // drag is measured in (fixed, so the point does not run from the hand),
    // and which key.
    std::optional<std::size_t> dragging_;
    std::vector<GraphCanvasCurveKey> pressed_keys_;
    Frame pressed_frame_;
};

// UX-8, decisions 2, 4, 5 and 12 (b): the value side of the Graph tab's
// node fields, beside the Loop, Draw, Image and Pass fields.
//   - a value.curve: its Keys table -- time, value, interpolation, a row's
//     x -- and + to add one, under the small curve;
//   - a value.playback: Wrap, Duration, Source begin / end, Speed, Delay;
//   - a render.scalar: the buffer it writes, its 1 to 4 numbers, and the
//     offset Studio packs it at;
//   - a render.fullscreen / render.raster: its Buffers -- the buffer
//     bindings its shader declares, the named buffer ports and what feeds
//     them, and + Buffer port for a port the shader reads through the
//     resource-port header (which no reflection shows until the port is
//     there).
// Every change is one edit (one Undo step). Named `pelican.graphCanvasCurve.*`,
// `pelican.graphCanvasPlayback.*`, `pelican.graphCanvasScalar.*`,
// `pelican.graphCanvasBuffers.*`.
class GraphCanvasValueFields final : public QWidget {
  public:
    GraphCanvasValueFields(GraphCanvasModel &model, std::function<void()> submitted,
                           QWidget *parent = nullptr);
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    void followEditable();
    // Where a pass's shader is read for the ports + Buffer port offers.
    void setProjectRoot(const std::filesystem::path &root) { project_root_ = root; }
    [[nodiscard]] GraphCanvasCurveGraph *curveGraph() const noexcept { return graph_; }
    // UX-9, decision 2: whether a value given while the model was busy waits
    // to be sent (the owner's notice says "Saving" meanwhile), and the call
    // made when that changes.
    [[nodiscard]] bool valuePending() const noexcept { return held_->pending(); }
    void setPendingChanged(std::function<void()> changed) { held_->setChanged(std::move(changed)); }

  private:
    // UX-9, decision 2: one edit of this node, sent now or held for it while
    // a save is in flight; false when it was refused.
    bool send(std::function<bool()> edit);
    // Before the fields let go of the node: a key's time or value, a
    // playback's number or a scalar's component typed and not yet confirmed
    // is sent for it.
    void release();
    std::unique_ptr<GraphCanvasHeldEdits> held_;
    void loadCurve(bool force);
    void commitKeys(const std::vector<GraphCanvasCurveKey> &keys);
    void commitRow(int row);
    // `force`: a focused field is loaded too (another node is shown).
    void loadPlayback(bool force = false);
    void loadScalar(bool force = false);
    void commitScalar();
    void loadBuffers();
    void addBufferPort();
    void afterCommit(bool sent);

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::filesystem::path project_root_;
    std::string node_;
    std::string scope_;
    std::string op_;
    bool loading_ = false;

    QWidget *curve_ = nullptr;
    GraphCanvasCurveGraph *graph_ = nullptr;
    QTableWidget *keys_ = nullptr;
    QPushButton *add_key_ = nullptr;
    std::vector<GraphCanvasCurveKey> shown_keys_;

    QWidget *playback_ = nullptr;
    QComboBox *wrap_ = nullptr;
    std::vector<std::pair<std::string, QDoubleSpinBox *>> playback_numbers_;

    QWidget *scalar_ = nullptr;
    QComboBox *scalar_target_ = nullptr;
    QComboBox *scalar_components_ = nullptr;
    std::vector<QDoubleSpinBox *> scalar_values_;
    QLabel *scalar_offset_ = nullptr;

    QWidget *buffers_ = nullptr;
    QLabel *buffer_list_ = nullptr;
    QPushButton *buffer_add_ = nullptr;
    QWidget *buffer_row_ = nullptr;
    QComboBox *buffer_name_ = nullptr;
    QComboBox *buffer_source_ = nullptr;
    QPushButton *buffer_apply_ = nullptr;

    QLabel *note_ = nullptr;
};

// UX-8, decision 2: the names a pass's shader reads through the resource
// port header (`PELICAN_RESOURCE_PORT_<name>`, `pelican_load_<name>(`,
// `pelican_sample_<name>(`, `pelican_resource_<name>`; not the include's own
// name, nor the engine's pelican_sample_table), in the order the text names
// them, from the project's file a `project://shaders/`
// reference names; nothing for an engine shader or a file that is not there.
std::vector<std::string> shaderResourcePortNames(const std::filesystem::path &project_root,
                                                 const std::string &reference);

} // namespace PelicanStudio
