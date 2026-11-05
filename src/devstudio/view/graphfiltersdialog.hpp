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

    explicit GraphFiltersDialog(QWidget *parent = nullptr);
    ~GraphFiltersDialog() override;

    void bind(GraphFiltersEdit *session);
    void sync();
    void reject() override;

    std::function<std::vector<GraphFilterFacts>()> rootFacts;
    std::function<Selection()> selection;
    std::function<void()> requestReady;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
