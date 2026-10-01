#pragma once

#include "../model/graphcanvasmodel.hpp"

#include <QWidget>

#include <functional>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QRadioButton;
class QSpinBox;

namespace PelicanStudio {

// UX-2, decision 1: the fields of the selected render.image, beside the Graph
// tab's canvas above the details (as the Loop, Draw and View fields). Every
// value is picked, none typed as JSON:
//   - format: the engine's formats (get_render_authoring_context's
//     render_target_vocabulary), the depth ones for a depth type;
//   - type: the catalog's image types by name, and a combo per argument with
//     the values its schema allows;
//   - usage: a check per flag the engine names;
//   - size: a scale of the screen, or a fixed width and height; layers.
// Each change is one set_node_parameters of the node's whole parameters (one
// Undo step), the keys the fields do not show travelling as they were.
// Without the vocabulary (an engine before it) format and usage fall back to
// text. Named `pelican.graphCanvasImage.<field>`.
class GraphCanvasImageFields final : public QWidget {
  public:
    GraphCanvasImageFields(GraphCanvasModel &model, std::function<void()> submitted,
                           QWidget *parent = nullptr);
    // The node to show and the scope it stands in, or null to hide.
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    // Off while the model cannot take an edit, as the Draw fields (Q-18).
    void followEditable();

  private:
    void load(const nlohmann::json &parameters);
    void loadArguments(const nlohmann::json &parameters);
    void loadUsage(const nlohmann::json &parameters);
    // One set_node_parameters; a refused one shows the document again.
    void commit(const nlohmann::json &parameters);
    void commitFormat(const QString &format);
    void commitSemantic();
    void commitArgument(const std::string &name, QComboBox *combo);
    void commitUsage();
    void commitSize();
    [[nodiscard]] nlohmann::json parameters() const;

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string node_;
    std::string scope_;
    std::string shown_;
    QLabel *heading_ = nullptr;
    QWidget *fields_ = nullptr;
    QComboBox *format_ = nullptr;
    QLineEdit *format_line_ = nullptr;
    QComboBox *semantic_ = nullptr;
    QWidget *arguments_ = nullptr;
    QFormLayout *arguments_form_ = nullptr;
    std::vector<QComboBox *> argument_boxes_;
    QWidget *usage_ = nullptr;
    std::vector<std::pair<std::string, QCheckBox *>> usage_boxes_;
    QLineEdit *usage_text_ = nullptr;
    QRadioButton *size_scale_ = nullptr;
    QRadioButton *size_fixed_ = nullptr;
    QDoubleSpinBox *extent_scale_ = nullptr;
    QSpinBox *width_ = nullptr;
    QSpinBox *height_ = nullptr;
    QSpinBox *layers_ = nullptr;
    QLabel *note_ = nullptr;
};

// UX-2, decision 2: the shaders of the selected render.fullscreen or
// render.raster, picked from the Pass Editor's list (the project's shaders
// and the engine's, fullscreenShaderChoices), and its ports as they are
// declared -- a pass's input sockets follow its shader's declarations (VI-1),
// so they are connected on the canvas, not typed. Each pick is one
// set_node_parameters. Named `pelican.graphCanvasPass.<field>`.
class GraphCanvasPassFields final : public QWidget {
  public:
    GraphCanvasPassFields(GraphCanvasModel &model, std::function<void()> submitted,
                          QWidget *parent = nullptr);
    void showNode(const GraphCanvasNode *node, const std::string &scope);
    void followEditable();

  private:
    void load(const nlohmann::json &parameters);
    void commit(const char *stage, QComboBox *combo);

    GraphCanvasModel &model_;
    std::function<void()> submitted_;
    std::string node_;
    std::string scope_;
    std::string shown_;
    QLabel *heading_ = nullptr;
    QWidget *fields_ = nullptr;
    QFormLayout *form_ = nullptr;
    QComboBox *vertex_ = nullptr;
    QComboBox *fragment_ = nullptr;
    QLabel *ports_ = nullptr;
};

} // namespace PelicanStudio
