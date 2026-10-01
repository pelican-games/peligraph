#pragma once

#include "passform.hpp"

#include <string>
#include <vector>

class QComboBox;
class QFormLayout;
class QGridLayout;
class QLabel;
class QSpinBox;
class QWidget;

namespace PelicanStudio {

// UX-4a. A raster pass's `raster_state` and `draw`, as choices: topology,
// cull, front face, depth test / write / compare, and each colour
// attachment's blend and write mask; the draw's operation and its counts.
// Every choice has "Default", which leaves the key out, as the engine reads
// an absent key (rasterpass.cpp). The values are the engine's own names.
//
// A fullscreen pass takes the same raster_state without the depth fields and
// with one colour attachment entry, and has no draw: the owner says which
// rows are shown.
class RasterStateForm final : public PassForm {
    Q_OBJECT

    struct ColorRow;

  public:
    explicit RasterStateForm(const QString &object_prefix,
                             QWidget *parent = nullptr);
    ~RasterStateForm() override;

    // raster_state and draw as the declaration holds them (null: absent).
    void load(const nlohmann::json &raster_state, const nlohmann::json &draw);
    void reset();

    void setDepthShown(bool shown);
    void setDrawShown(bool shown);
    // One row per colour attachment entry, labelled. An entry that stays
    // keeps its values.
    void setColorAttachments(const std::vector<std::string> &labels);
    // The buffers an indexed draw can read its indices from.
    void setIndexBufferCandidates(const std::vector<std::string> &buffers);

    // null when every choice is Default and the state had nothing else.
    [[nodiscard]] nlohmann::json rasterState() const;
    // null when the draw is not shown and none was loaded.
    [[nodiscard]] nlohmann::json draw() const;

  private:
    void buildColorRows(const std::vector<std::string> &labels);
    nlohmann::json colorEntries() const;
    void showDrawOperation();

    nlohmann::json state_ = nlohmann::json::object();
    nlohmann::json draw_ = nullptr;
    // The colour attachment entries, by index, as the rows last held them;
    // whether the declaration had the key at all.
    nlohmann::json colors_ = nlohmann::json::array();
    bool colors_present_ = false;
    bool depth_shown_ = true;
    bool draw_shown_ = false;

    QFormLayout *form_ = nullptr;
    QComboBox *topology_ = nullptr;
    QComboBox *cull_ = nullptr;
    QComboBox *front_face_ = nullptr;
    QComboBox *depth_test_ = nullptr;
    QComboBox *depth_write_ = nullptr;
    QComboBox *depth_compare_ = nullptr;
    QWidget *colors_box_ = nullptr;
    QGridLayout *colors_grid_ = nullptr;
    std::vector<ColorRow> color_rows_;
    QWidget *draw_box_ = nullptr;
    QComboBox *operation_ = nullptr;
    QComboBox *index_buffer_ = nullptr;
    QSpinBox *vertex_count_ = nullptr;
    QSpinBox *index_count_ = nullptr;
    QSpinBox *instance_count_ = nullptr;
    QSpinBox *first_vertex_ = nullptr;
    QSpinBox *first_index_ = nullptr;
    QSpinBox *first_instance_ = nullptr;
};

} // namespace PelicanStudio
