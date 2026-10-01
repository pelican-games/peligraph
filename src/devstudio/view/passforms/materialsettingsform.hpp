#pragma once

#include "passform.hpp"

#include <memory>
#include <string>
#include <vector>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;

namespace PelicanStudio {

// UX-4a. A material pass's ten settings (edit_preset_pass's
// material_settings), as a form:
//   - material_contract: the contracts the engine knows
//   - material_filter: the tags a material must have any of / none of
//     (written, commas between: the engine reports no tag list yet)
//   - material_variant: a name, offered from the variants other passes use
//   - material_range: on / off, start and count
//   - material_outputs and material_output_states: one row an output -- its
//     name, type, source, and the blend and write mask the pass gives it
//   - screen_inputs, surface_resources, material_resources: one row a
//     binding -- which of the three, its name, the resource it reads
//   - gpu_draw_source, and nothing else, stays JSON ("Other").
// What a setting held that the form does not show stays as it was.
class MaterialSettingsForm final : public PassForm {
    Q_OBJECT

    struct OutputRow;
    struct ResourceRow;

  public:
    explicit MaterialSettingsForm(const QString &object_prefix,
                                  QWidget *parent = nullptr);
    ~MaterialSettingsForm() override;

    // The ten keys, in the order edit_preset_pass names them.
    static const std::vector<std::string> &keys();

    // Reads the ten keys from a material pass declaration.
    void load(const nlohmann::json &declaration);
    void reset();
    // The resources a binding row offers to read.
    void setResourceCandidates(const std::vector<std::string> &resources);
    // The variant names other passes use.
    void setVariantCandidates(const std::vector<std::string> &variants);

    // Each of the ten keys the pass has now; one left out is absent. Throws
    // when Other is not a JSON object of the keys it may hold.
    [[nodiscard]] nlohmann::json settings() const;

  private:
    void addOutputRow(const nlohmann::json &output, const nlohmann::json *state);
    void addResourceRow(const std::string &kind, const std::string &name,
                        const nlohmann::json &value);
    void renumber();
    void fillResourceNames(ResourceRow &row);

    nlohmann::json original_ = nlohmann::json::object();
    std::vector<std::string> resource_candidates_;

    QComboBox *contract_ = nullptr;
    QLineEdit *filter_include_ = nullptr;
    QLineEdit *filter_exclude_ = nullptr;
    QComboBox *variant_ = nullptr;
    QCheckBox *range_ = nullptr;
    QSpinBox *range_start_ = nullptr;
    QSpinBox *range_count_ = nullptr;
    QLineEdit *outputs_name_ = nullptr;
    QWidget *outputs_box_ = nullptr;
    QGridLayout *outputs_grid_ = nullptr;
    QPushButton *add_output_ = nullptr;
    std::vector<std::unique_ptr<OutputRow>> output_rows_;
    QWidget *resources_box_ = nullptr;
    QGridLayout *resources_grid_ = nullptr;
    QPushButton *add_resource_ = nullptr;
    std::vector<std::unique_ptr<ResourceRow>> resource_rows_;
    QPlainTextEdit *other_ = nullptr;
    int next_line_ = 1;
};

} // namespace PelicanStudio
