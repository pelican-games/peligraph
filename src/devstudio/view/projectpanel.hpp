#pragma once

#include "projectfiles.hpp"

#include <QWidget>

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

class QAction;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTreeWidget;

namespace PelicanStudio {

class ProjectPanel final : public QWidget {
  public:
    struct Counters {
        std::uint64_t scans_started = 0;
        std::uint64_t scans_completed = 0;
        std::uint64_t results_discarded = 0;
        std::uint64_t row_edits_applied = 0;
        std::uint64_t apply_turns = 0;
        std::uint64_t last_scan_us = 0;
        std::uint64_t last_diff_us = 0;
        std::uint64_t last_apply_us = 0;
        std::uint64_t last_filter_us = 0;
        std::uint64_t last_idle_us = 0;
        std::uint64_t visited_entries = 0;
        std::uint64_t json_bytes = 0;
    };
    using ScanFunction = std::function<ProjectListing(
        const std::filesystem::path &, const ProjectScanOptions &)>;

    explicit ProjectPanel(QWidget *parent = nullptr);
    ~ProjectPanel() override;

    void open(const std::filesystem::path &root);
    void refresh();

    [[nodiscard]] QTreeWidget *tree() const noexcept;
    [[nodiscard]] QLineEdit *searchBox() const noexcept;
    [[nodiscard]] QComboBox *kindFilter() const noexcept;
    [[nodiscard]] QPushButton *refreshButton() const noexcept;
    [[nodiscard]] QLabel *truncatedLine() const noexcept;
    [[nodiscard]] QLabel *noticeLine() const noexcept;
    [[nodiscard]] QAction *copyPathAction() const noexcept;
    [[nodiscard]] Counters counters() const noexcept;
    [[nodiscard]] bool idle() const noexcept;

    void setScanFunctionForTest(ScanFunction scan);
    void setApplyBudgetMsForTest(int milliseconds);

  protected:
    void showEvent(QShowEvent *event) override;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace PelicanStudio
