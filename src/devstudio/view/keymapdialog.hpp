#pragma once

#include "mainwindow.hpp"

#include <QDialog>
#include <QKeySequence>
#include <QList>
#include <QString>

#include <map>
#include <vector>

class QLabel;
class QPushButton;
class QKeySequenceEdit;
class QTableWidget;

namespace PelicanStudio {

// KM-2b: the settings dialog that edits `<config>/keymap.json`.  It never
// writes a handler's state directly: every edit goes through planKeymap and,
// once accepted, applyKeymapPlan, so the dialog and start-up share one rule.
class KeymapDialog : public QDialog {
    Q_OBJECT

  public:
    explicit KeymapDialog(MainWindow &window, QWidget *parent = nullptr);

  private:
    struct Row {
        QString id;
        int surface = 0;
        QString title;
        QList<QKeySequence> defaults;
        QList<QKeySequence> fixed_keys;
        bool fixed = false;
        int table_row = -1;
    };

    void buildRows();
    void buildTable();
    void rebuildKeysCell(int table_row);
    void refreshPlan();
    std::vector<KeymapEntry> overrides() const;
    void resetRow(int table_row);
    void clearRow(int table_row);
    void addBinding(int table_row);
    void removeBinding(int table_row, int binding_index);
    void handleRecorderFinished(int table_row, int binding_index,
                                QKeySequenceEdit *editor, bool draft);
    void showSaveError(const QString &message);
    bool saveAndApply();

    MainWindow &window_;
    QTableWidget *table_ = nullptr;
    QLabel *error_label_ = nullptr;
    QPushButton *ok_button_ = nullptr;
    std::vector<Row> rows_;
    std::map<QString, QList<QKeySequence>> pending_;
    std::map<QString, QKeySequenceEdit *> drafts_;
    KeymapPlan plan_;
    bool rebuilding_ = false;
};

} // namespace PelicanStudio
