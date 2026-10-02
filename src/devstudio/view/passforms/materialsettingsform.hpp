#pragma once

#include "passform.hpp"

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

class QCheckBox;
class QComboBox;
class QGridLayout;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QSpinBox;
class QFormLayout;

namespace PelicanStudio {

class GraphCanvasTagPicker;

// UX-4a. A material pass's ten settings (edit_preset_pass's
// material_settings), as a form:
//   - material_contract: the contracts the engine knows
//   - material_filter: the tags a material must have any of / none of
//     (UX-11, decision 2: picked among the engine's material tags with the
//     Graph's Loop picker; a tag no material lists is typed in its
//     "+ new tag" line)
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
    // UX-12c, decision 3: the names a `kind` ("screen_inputs",
    // "surface_resources") row offers, as the engine lists them
    // (get_graph_catalog's material_pass_contracts, MP-1): {name,
    // source_type}. None given (an engine before MP-1): Studio's own table.
    void setResourceNameCandidates(const std::string &kind,
                                   std::vector<std::pair<std::string, std::string>> names);
    // The semantic type each resource offered to read carries, where known:
    // a row whose name wants another type does not offer that resource.
    void setResourceCandidateTypes(std::map<std::string, std::string> types);
    // The variant names other passes use.
    void setVariantCandidates(const std::vector<std::string> &variants);
    // UX-11, decision 2: the tags the Tags pickers offer (the engine's
    // material_tags); what the pass names stays offered, checked.
    void setTagCandidates(const std::vector<std::string> &tags);
    // UX-11, decision 1: the Graph shows its own Contract and Variant
    // (pick-only, beside the node's other fields), so it hides these two.
    // Hidden, they still come back from settings() as they were loaded.
    void setContractAndVariantShown(bool shown);

    // Each of the ten keys the pass has now; one left out is absent. Throws
    // when Other is not a JSON object of the keys it may hold.
    [[nodiscard]] nlohmann::json settings() const;

  private:
    void addOutputRow(const nlohmann::json &output, const nlohmann::json *state);
    void addResourceRow(const std::string &kind, const std::string &name,
                        const nlohmann::json &value);
    void renumber();
    void fillResourceNames(ResourceRow &row);
    void fillResourceReads(ResourceRow &row);
    [[nodiscard]] std::vector<std::string> resourceNamesOf(const std::string &kind) const;

    nlohmann::json original_ = nlohmann::json::object();
    std::vector<std::string> resource_candidates_;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> name_candidates_;
    std::map<std::string, std::string> candidate_types_;
    std::vector<std::string> tag_candidates_;
    QFormLayout *form_ = nullptr;

    QComboBox *contract_ = nullptr;
    GraphCanvasTagPicker *filter_include_ = nullptr;
    GraphCanvasTagPicker *filter_exclude_ = nullptr;
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
