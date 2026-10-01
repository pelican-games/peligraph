#include "rasterstateform.hpp"

#include "materialoutput.hpp"
#include "rasterpass.hpp"

#include <QComboBox>
#include <QFormLayout>
#include <QGridLayout>
#include <QLabel>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QVBoxLayout>

#include <algorithm>
#include <array>
#include <limits>
#include <tuple>
#include <utility>

namespace PelicanStudio {

using passform::Json;
using passform::text;

namespace {

std::vector<std::string> names(std::initializer_list<std::string_view> values) {
    std::vector<std::string> result;
    for (const auto value : values) result.emplace_back(value);
    return result;
}

std::vector<std::string> topologyNames() {
    using Pelican::RasterPrimitiveTopology;
    std::vector<std::string> result;
    for (const auto value :
         {RasterPrimitiveTopology::triangle_list,
          RasterPrimitiveTopology::triangle_strip,
          RasterPrimitiveTopology::line_list,
          RasterPrimitiveTopology::line_strip,
          RasterPrimitiveTopology::point_list})
        result.emplace_back(Pelican::rasterPrimitiveTopologyName(value));
    return result;
}

std::vector<std::string> cullNames() {
    using Pelican::RasterCullMode;
    std::vector<std::string> result;
    for (const auto value :
         {RasterCullMode::none, RasterCullMode::back, RasterCullMode::front})
        result.emplace_back(Pelican::rasterCullModeName(value));
    return result;
}

std::vector<std::string> frontFaceNames() {
    using Pelican::RasterFrontFace;
    std::vector<std::string> result;
    for (const auto value :
         {RasterFrontFace::counter_clockwise, RasterFrontFace::clockwise})
        result.emplace_back(Pelican::rasterFrontFaceName(value));
    return result;
}

std::vector<std::string> compareNames() {
    using Pelican::RasterDepthCompare;
    std::vector<std::string> result;
    for (const auto value :
         {RasterDepthCompare::less, RasterDepthCompare::less_equal,
          RasterDepthCompare::greater, RasterDepthCompare::greater_equal,
          RasterDepthCompare::equal, RasterDepthCompare::not_equal,
          RasterDepthCompare::always, RasterDepthCompare::never})
        result.emplace_back(Pelican::rasterDepthCompareName(value));
    return result;
}

// The convenience presets parseMaterialOutputBlendState reads; an explicit
// {color, alpha} equation is kept as a "custom" entry.
std::vector<std::string> blendNames() {
    return names({"opaque", "blend", "additive"});
}

// Every channel subset, in the engine's spelling ("rgba" ... "none").
std::vector<std::string> writeMaskNames() {
    std::vector<std::string> result;
    for (int mask = Pelican::materialOutputWriteRgba; mask >= 0; --mask)
        result.push_back(
            Pelican::materialOutputWriteMaskName(static_cast<std::uint8_t>(mask)));
    return result;
}

// Default / On / Off for a boolean key.
void fillBoolChoices(QComboBox &combo) {
    const QSignalBlocker blocker{&combo};
    combo.clear();
    combo.addItem(QObject::tr("Default"), QString{});
    combo.addItem(QObject::tr("On"), QStringLiteral("true"));
    combo.addItem(QObject::tr("Off"), QStringLiteral("false"));
}

void showBool(QComboBox &combo, const Json *value) {
    const QSignalBlocker blocker{&combo};
    if (value != nullptr && value->is_boolean()) {
        combo.setCurrentIndex(value->get<bool>() ? 1 : 2);
        return;
    }
    passform::showOptionalValue(combo, value);
}

std::optional<Json> boolValue(const QComboBox &combo) {
    if (combo.currentIndex() == 1) return Json(true);
    if (combo.currentIndex() == 2) return Json(false);
    return passform::optionalValue(combo);
}

const Json *member(const Json &object, const char *key) {
    if (!object.is_object()) return nullptr;
    const auto found = object.find(key);
    return found == object.end() ? nullptr : &*found;
}

void putOptional(Json &object, const char *key, std::optional<Json> value) {
    if (value)
        object[key] = std::move(*value);
    else
        object.erase(key);
}

QSpinBox *makeCount(QWidget *parent, const QString &name, int minimum) {
    auto *spin = new QSpinBox(parent);
    spin->setObjectName(name);
    spin->setRange(minimum, std::numeric_limits<int>::max());
    return spin;
}

} // namespace

struct RasterStateForm::ColorRow {
    QLabel *label = nullptr;
    QComboBox *blend = nullptr;
    QComboBox *write_mask = nullptr;
};

RasterStateForm::RasterStateForm(const QString &object_prefix, QWidget *parent)
    : PassForm(object_prefix, parent) {
    setObjectName(childName("rasterState"));
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    form_ = new QFormLayout;
    form_->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(form_);

    const auto combo = [this](const char *name, const QString &tip) {
        auto *box = new QComboBox(this);
        box->setObjectName(childName(name));
        box->setToolTip(tip);
        QObject::connect(box, qOverload<int>(&QComboBox::currentIndexChanged),
                         this, [this] { notifyChanged(); });
        return box;
    };
    topology_ = combo("rasterState.topology",
                      tr("raster_state.topology: how the vertices make primitives"));
    passform::fillOptionalChoices(*topology_, tr("Default (triangle_list)"),
                                  topologyNames());
    form_->addRow(tr("Topology"), topology_);
    cull_ = combo("rasterState.cull", tr("raster_state.cull"));
    passform::fillOptionalChoices(*cull_, tr("Default (none)"), cullNames());
    form_->addRow(tr("Cull"), cull_);
    front_face_ = combo("rasterState.frontFace",
                        tr("raster_state.front_face: the winding that faces the camera"));
    passform::fillOptionalChoices(*front_face_, tr("Default (counter_clockwise)"),
                                  frontFaceNames());
    // N-7: the shorter name; the key is on the combo's tooltip.
    form_->addRow(tr("Winding"), front_face_);
    depth_test_ = combo("rasterState.depthTest", tr("raster_state.depth_test"));
    fillBoolChoices(*depth_test_);
    form_->addRow(tr("Depth test"), depth_test_);
    depth_write_ = combo("rasterState.depthWrite", tr("raster_state.depth_write"));
    fillBoolChoices(*depth_write_);
    form_->addRow(tr("Depth write"), depth_write_);
    depth_compare_ = combo("rasterState.depthCompare", tr("raster_state.depth_compare"));
    passform::fillOptionalChoices(*depth_compare_, tr("Default (less)"),
                                  compareNames());
    form_->addRow(tr("Depth compare"), depth_compare_);

    colors_box_ = new QWidget(this);
    colors_box_->setObjectName(childName("rasterState.colorAttachments"));
    colors_grid_ = new QGridLayout(colors_box_);
    colors_grid_->setContentsMargins(0, 0, 0, 0);
    form_->addRow(tr("Blend"), colors_box_);

    draw_box_ = new QWidget(this);
    draw_box_->setObjectName(childName("draw"));
    auto *draw_form = new QFormLayout(draw_box_);
    draw_form->setContentsMargins(0, 0, 0, 0);
    operation_ = combo("draw.operation",
                       tr("draw.operation: direct draws vertices, indexed reads an "
                          "index buffer"));
    operation_->addItem(tr("Direct"), QStringLiteral("direct"));
    operation_->addItem(tr("Indexed"), QStringLiteral("indexed"));
    draw_form->addRow(tr("Draw"), operation_);
    index_buffer_ = new QComboBox(draw_box_);
    index_buffer_->setObjectName(childName("draw.indexBuffer"));
    index_buffer_->setEditable(true);
    index_buffer_->setInsertPolicy(QComboBox::NoInsert);
    index_buffer_->setToolTip(tr("draw.index_buffer: the buffer the indices are read from"));
    QObject::connect(index_buffer_, &QComboBox::currentTextChanged, this,
                     [this] { notifyChanged(); });
    draw_form->addRow(tr("Index buffer"), index_buffer_);
    const auto count = [this, draw_box = draw_box_, draw_form](
                           const char *name, const QString &label, int minimum) {
        auto *spin = makeCount(draw_box, childName(name), minimum);
        QObject::connect(spin, qOverload<int>(&QSpinBox::valueChanged), this,
                         [this] { notifyChanged(); });
        draw_form->addRow(label, spin);
        return spin;
    };
    vertex_count_ = count("draw.vertexCount", tr("Vertices"), 0);
    index_count_ = count("draw.indexCount", tr("Indices"), 0);
    instance_count_ = count("draw.instanceCount", tr("Instances"), 0);
    first_vertex_ = count("draw.firstVertex", tr("First vertex"), 0);
    first_index_ = count("draw.firstIndex", tr("First index"), 0);
    first_instance_ = count("draw.firstInstance", tr("First instance"), 0);
    QObject::connect(operation_, qOverload<int>(&QComboBox::currentIndexChanged),
                     this, [this] { showDrawOperation(); });
    layout->addWidget(draw_box_);
    reset();
}

RasterStateForm::~RasterStateForm() = default;

void RasterStateForm::reset() { load(nullptr, nullptr); }

void RasterStateForm::load(const Json &raster_state, const Json &draw) {
    const Loading loading{*this};
    state_ = raster_state.is_object() ? raster_state : Json::object();
    draw_ = draw.is_object() ? draw : Json(nullptr);
    colors_present_ = state_.contains("color_attachments");
    colors_ = colors_present_ && state_.at("color_attachments").is_array()
                  ? state_.at("color_attachments")
                  : Json::array();
    passform::showOptionalValue(*topology_, member(state_, "topology"));
    passform::showOptionalValue(*cull_, member(state_, "cull"));
    passform::showOptionalValue(*front_face_, member(state_, "front_face"));
    showBool(*depth_test_, member(state_, "depth_test"));
    showBool(*depth_write_, member(state_, "depth_write"));
    passform::showOptionalValue(*depth_compare_, member(state_, "depth_compare"));
    std::vector<std::string> labels;
    for (const auto &row : color_rows_) labels.push_back(row.label->text().toStdString());
    if (labels.empty())
        for (std::size_t index = 0; index < colors_.size(); ++index)
            labels.push_back(std::to_string(index));
    buildColorRows(labels);

    const Json drawn = draw_.is_object() ? draw_ : Json::object();
    {
        const QSignalBlocker blocker{operation_};
        operation_->setCurrentIndex(
            drawn.value("operation", std::string{"direct"}) == "indexed" ? 1 : 0);
    }
    {
        const QSignalBlocker blocker{index_buffer_};
        index_buffer_->setEditText(text(drawn.value("index_buffer", std::string{})));
    }
    for (const auto &[spin, key, fallback] :
         {std::tuple{vertex_count_, "vertex_count", 3},
          std::tuple{index_count_, "index_count", 0},
          std::tuple{instance_count_, "instance_count", 1},
          std::tuple{first_vertex_, "first_vertex", 0},
          std::tuple{first_index_, "first_index", 0},
          std::tuple{first_instance_, "first_instance", 0}}) {
        const QSignalBlocker blocker{spin};
        const auto found = drawn.find(key);
        spin->setValue(found != drawn.end() && found->is_number_integer()
                           ? found->get<int>()
                           : fallback);
    }
    showDrawOperation();
}

void RasterStateForm::setDepthShown(bool shown) {
    depth_shown_ = shown;
    for (auto *field : {depth_test_, depth_write_, depth_compare_})
        form_->setRowVisible(field, shown);
}

void RasterStateForm::setDrawShown(bool shown) {
    draw_shown_ = shown;
    draw_box_->setVisible(shown);
}

void RasterStateForm::setColorAttachments(const std::vector<std::string> &labels) {
    std::vector<std::string> current;
    for (const auto &row : color_rows_) current.push_back(row.label->text().toStdString());
    if (current == labels) return;
    // The rows say what the entries are now; rebuilt, each keeps its index.
    // An entry past the old rows (loaded before the rows were named) stays.
    Json entries = colorEntries();
    for (std::size_t index = entries.size(); index < colors_.size(); ++index)
        entries.push_back(colors_.at(index));
    colors_ = std::move(entries);
    const Loading loading{*this};
    buildColorRows(labels);
}

void RasterStateForm::setIndexBufferCandidates(const std::vector<std::string> &buffers) {
    const QSignalBlocker blocker{index_buffer_};
    const QString current = index_buffer_->currentText();
    index_buffer_->clear();
    for (const auto &buffer : buffers) index_buffer_->addItem(text(buffer));
    index_buffer_->setEditText(current);
}

void RasterStateForm::buildColorRows(const std::vector<std::string> &labels) {
    for (auto &row : color_rows_) {
        delete row.label;
        delete row.blend;
        delete row.write_mask;
    }
    color_rows_.clear();
    for (std::size_t index = 0; index < labels.size(); ++index) {
        const QString number = QString::number(index);
        ColorRow row;
        row.label = new QLabel(text(labels[index]), colors_box_);
        row.blend = new QComboBox(colors_box_);
        row.blend->setObjectName(childName("rasterState.blend.") + number);
        row.blend->setToolTip(tr("color_attachments[%1].blend").arg(index));
        passform::fillOptionalChoices(*row.blend, tr("Default (opaque)"), blendNames());
        row.write_mask = new QComboBox(colors_box_);
        row.write_mask->setObjectName(childName("rasterState.writeMask.") + number);
        row.write_mask->setToolTip(tr("color_attachments[%1].write_mask").arg(index));
        passform::fillOptionalChoices(*row.write_mask, tr("Default (rgba)"),
                                      writeMaskNames());
        const Json entry = index < colors_.size() ? colors_.at(index) : Json::object();
        passform::showOptionalValue(*row.blend, member(entry, "blend"));
        passform::showOptionalValue(*row.write_mask, member(entry, "write_mask"));
        const int line = static_cast<int>(index);
        colors_grid_->addWidget(row.label, line, 0);
        colors_grid_->addWidget(row.blend, line, 1);
        colors_grid_->addWidget(row.write_mask, line, 2);
        for (auto *box : {row.blend, row.write_mask})
            QObject::connect(box, qOverload<int>(&QComboBox::currentIndexChanged),
                             this, [this] { notifyChanged(); });
        color_rows_.push_back(row);
    }
    form_->setRowVisible(colors_box_, !color_rows_.empty());
}

Json RasterStateForm::colorEntries() const {
    Json entries = Json::array();
    for (std::size_t index = 0; index < color_rows_.size(); ++index) {
        Json entry = index < colors_.size() && colors_.at(index).is_object()
                         ? colors_.at(index)
                         : Json::object();
        putOptional(entry, "blend", passform::optionalValue(*color_rows_[index].blend));
        putOptional(entry, "write_mask",
                    passform::optionalValue(*color_rows_[index].write_mask));
        entries.push_back(std::move(entry));
    }
    return entries;
}

Json RasterStateForm::rasterState() const {
    Json state = state_;
    putOptional(state, "topology", passform::optionalValue(*topology_));
    putOptional(state, "cull", passform::optionalValue(*cull_));
    putOptional(state, "front_face", passform::optionalValue(*front_face_));
    if (depth_shown_) {
        putOptional(state, "depth_test", boolValue(*depth_test_));
        putOptional(state, "depth_write", boolValue(*depth_write_));
        putOptional(state, "depth_compare", passform::optionalValue(*depth_compare_));
    }
    const Json entries = colorEntries();
    const bool any = std::ranges::any_of(
        entries, [](const Json &entry) { return !entry.empty(); });
    if (colors_present_ || any)
        state["color_attachments"] = entries;
    else
        state.erase("color_attachments");
    return state.empty() ? Json(nullptr) : state;
}

void RasterStateForm::showDrawOperation() {
    const bool indexed = operation_->currentData().toString() == QStringLiteral("indexed");
    auto *draw_form = qobject_cast<QFormLayout *>(draw_box_->layout());
    draw_form->setRowVisible(index_buffer_, indexed);
    draw_form->setRowVisible(index_count_, indexed);
    draw_form->setRowVisible(first_index_, indexed);
    draw_form->setRowVisible(vertex_count_, !indexed);
    draw_form->setRowVisible(first_vertex_, !indexed);
    notifyChanged();
}

Json RasterStateForm::draw() const {
    if (!draw_shown_ && draw_.is_null()) return nullptr;
    const Json original = draw_.is_object() ? draw_ : Json::object();
    Json result = original;
    const bool indexed = operation_->currentData().toString() == QStringLiteral("indexed");
    if (indexed || original.contains("operation"))
        result["operation"] = indexed ? "indexed" : "direct";
    // A key is written when the declaration had it or its value is not the
    // engine's default, so a draw read and saved unchanged is the same JSON.
    const auto put = [&](QSpinBox *spin, const char *key, int fallback) {
        if (original.contains(key) || spin->value() != fallback)
            result[key] = spin->value();
    };
    if (indexed)
        for (const auto *key : {"vertex_count", "first_vertex"}) result.erase(key);
    else
        for (const auto *key : {"index_buffer", "index_count", "first_index"})
            result.erase(key);
    if (indexed) {
        result["index_buffer"] = index_buffer_->currentText().trimmed().toStdString();
        put(index_count_, "index_count", 0);
        put(first_index_, "first_index", 0);
    } else {
        put(vertex_count_, "vertex_count", 3);
        put(first_vertex_, "first_vertex", 0);
    }
    put(instance_count_, "instance_count", 1);
    put(first_instance_, "first_instance", 0);
    return result;
}

} // namespace PelicanStudio
