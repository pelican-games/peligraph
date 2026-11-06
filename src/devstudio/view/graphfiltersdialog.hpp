#pragma once

#include "../model/graphfilters.hpp"
#include "../model/graphfiltersedit.hpp"

#include <QDialog>

#include <functional>
#include <memory>
#include <vector>

namespace PelicanStudio {

class GraphFiltersDialog : public QDialog {
  public:
    struct Selection {
        bool root = false;
        std::vector<std::string> node_ids;
    };

    // Read only. `document` belongs to the model and is used only during a sync.
    struct PresetFilters {
        QString name;
        const GraphFilters *document = nullptr;
    };

    explicit GraphFiltersDialog(QWidget *parent = nullptr);
    ~GraphFiltersDialog() override;

    void bind(GraphFiltersEdit *session);
    void sync();
    void reject() override;

    std::function<std::vector<GraphFilterFacts>()> rootFacts;
    std::function<Selection()> selection;
    std::function<void()> requestReady;
    std::function<QString(bool saving)> chooseFile;
    std::function<PresetFilters()> presetFilters;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
