#pragma once

#include "../model/graphfoldgroups.hpp"
#include "graphcanvasreadcapability.hpp"

#include <QWidget>

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

class QAction;
class QByteArray;
class QCheckBox;
class QComboBox;
class QGraphicsItem;
class QLabel;
class QMenu;
class QMessageBox;
class QPushButton;
class QTimer;
class QVBoxLayout;

namespace PelicanStudio {

class GraphCanvasModel;
class GraphCanvasScene;

// GL-3a (design_graph_layers 3.4, 4 rules 3, 5, 6 and 8). A fold group is
// drawn over the root canvas: a pale frame round its nodes with a heading
// (its name, its state, and Fold / Open on the right), or -- folded, which is
// only how it is looked at -- one folded node in their place, drawn as GL-1b
// draws one, with a row for each line that crosses its edge.
//
// Every item drawn for a group carries the group here; the folded node is
// `graph_node#fold:<group>`.
inline constexpr int GraphCanvasFoldGroupRole = Qt::UserRole + 3190;
// A node or a line the folded group hides (shown again on the next draw).
inline constexpr int GraphCanvasFoldHiddenRole = Qt::UserRole + 3191;
// `graph_fold_group#<group>`: the frame; `graph_fold_heading#<group>` its
// heading band (a press picks the group, a right-click opens its menu);
// `graph_fold_state#<group>` the state's words; `graph_fold_mark#<group>`
// the orange mark of a stale group; `graph_fold_toggle#<group>` Fold / Open;
// `graph_fold_link#<group>/<row>` a line to a folded group's row.
inline constexpr auto GraphCanvasFoldGroupItem = "graph_fold_group";
inline constexpr auto GraphCanvasFoldHeadingItem = "graph_fold_heading";
inline constexpr auto GraphCanvasFoldStateItem = "graph_fold_state";
inline constexpr auto GraphCanvasFoldMarkItem = "graph_fold_mark";
inline constexpr auto GraphCanvasFoldToggleItem = "graph_fold_toggle";
inline constexpr auto GraphCanvasFoldLinkItem = "graph_fold_link";
// GL-4a: `graph_fold_verify#<group>`, Verify (Verify again on a stale group)
// on the heading.
inline constexpr auto GraphCanvasFoldVerifyItem = "graph_fold_verify";
inline constexpr qreal GraphCanvasFoldHeadingHeight = 26.0;
inline constexpr qreal GraphCanvasFoldPadding = 14.0;

// The id of a folded group's node on the canvas.
std::string graphFoldNodeId(const std::string &group);

// Decision 4: the Decisions section of the side fields, for the picked group.
// Each list has its rows (each with ×) and an Add row that is chosen, never
// typed: the node, a loop's element, a writer's port. Each change hands the
// group with its new decisions to `changed` (one fold_nodes).
class GraphCanvasFoldFields final : public QWidget {
  public:
    explicit GraphCanvasFoldFields(QWidget *parent = nullptr);
    // nullptr hides the section.
    void showGroup(const GraphFoldGroup *group, const std::vector<GraphCanvasNode> &root,
                   const std::map<std::string, std::int64_t, std::less<>> &loop_counts);
    void setEditable(bool editable);
    std::function<void(const GraphFoldGroup &edited)> changed;
    // GL-4a, decision 1: Verify over the Decisions, and Quick check beside
    // it. `verify` is pressed; `label` is "Verify" or "Verify again", and
    // `enabled` false while one runs.
    std::function<void()> verify;
    void setVerify(bool enabled, const QString &label, const QString &tooltip);
    [[nodiscard]] bool quickCheck() const noexcept { return quick_; }
    void setQuickCheck(bool quick);
    // Decision 5: of a verified group, the expanded names a decision's
    // subject landed on (empty: it matches nothing any more).
    std::function<std::vector<std::string>(GraphFoldField, const GraphFoldSubject &)> applied;

  private:
    struct AddRow {
        GraphFoldField field = GraphFoldField::serial;
        QComboBox *node = nullptr;
        QComboBox *element = nullptr;
        QComboBox *port = nullptr;
        QPushButton *add = nullptr;
        std::vector<GraphFoldNodeChoice> choices;
    };
    void rebuild();
    void followNode(AddRow &row);
    void add(AddRow &row);

    std::optional<GraphFoldGroup> group_;
    std::vector<GraphCanvasNode> root_;
    std::map<std::string, std::int64_t, std::less<>> loop_counts_;
    std::string shown_;
    bool editable_ = true;
    bool quick_ = false;
    bool verify_enabled_ = true;
    QString verify_label_;
    QString verify_tooltip_;
    QPushButton *verify_button_ = nullptr;
    QCheckBox *quick_box_ = nullptr;
    QVBoxLayout *layout_ = nullptr;
    QWidget *body_ = nullptr;
    std::vector<AddRow> rows_;
};

// What the Graph tab does with fold groups: reads them with every document
// the tab reads (get_fold_groups, then the root's decisions), draws them,
// answers the gestures on them, and writes them (fold_nodes, one ticket,
// RD-1's undo). The tab hands it its replies first.
class GraphCanvasFoldController {
  public:
    struct Hooks {
        // The tab's notice line, read again from the model.
        std::function<void()> notice;
        std::function<void()> dispatch;
        // The side fields, shown again.
        std::function<void()> details;
        // A fold_nodes committed: what the tab does after any render edit
        // of its own (the digest, a restart, the plan, the document), with
        // the line the notice says.
        std::function<void(const nlohmann::json &response, const std::string &line)> committed;
    };

    GraphCanvasFoldController(GraphCanvasReadCapability &graph, GraphCanvasModel &model,
                              GraphCanvasScene &scene, QWidget &owner, Hooks hooks);
    ~GraphCanvasFoldController();

    // A reply of this controller's (true), or none of its.
    bool deliverResult(qint64 id, const QByteArray &bytes);
    bool deliverFailure(qint64 id, const QString &message);
    // Each tick of the tab: read the groups again when the document moved.
    void follow();
    // Draw the groups over the canvas again (after a draw, after a drag).
    void redraw();
    void scheduleRedraw();
    // The side fields follow the picked group.
    void syncFields();
    // A node was picked on the canvas: the group lets go.
    void nodePicked();

    [[nodiscard]] GraphCanvasFoldFields *fields() const noexcept { return fields_; }
    [[nodiscard]] const std::vector<GraphFoldGroup> &groups() const noexcept { return groups_; }
    [[nodiscard]] const std::string &selectedGroup() const noexcept { return selected_; }
    [[nodiscard]] bool displayFolded(const std::string &group) const {
        return folded_display_.contains(group);
    }
    [[nodiscard]] bool busy() const noexcept { return edit_request_ != 0 || !ticket_.isEmpty(); }
    // GL-4a: the group a verify_fold is running for, or empty.
    [[nodiscard]] const std::string &verifying() const noexcept { return verifying_; }
    // The verifying group that holds this root node, or empty.
    [[nodiscard]] std::string verifyingGroupOf(const std::string &node) const;

    // Decision 2: pick a group (its heading, or its folded node).
    bool selectGroup(const std::string &group);
    // Decision 1: Fold into group… on what is picked.
    bool foldSelection();
    // Decision 5.
    bool addSelectedNodes(const std::string &group);
    bool removeNode(const std::string &group, const std::string &node);
    // Asks first (Cancel is the default); the answer sends folded null.
    void askUnfold(const std::string &group);
    bool unfold(const std::string &group);
    // Decision 2: folded or open, as it is looked at (session state; the
    // document's fold_groups does not change).
    bool setDisplayFolded(const std::string &group, bool folded);
    // Decision 4: the group's decisions written as `edited` holds them.
    bool writeDecisions(const GraphFoldGroup &edited);
    // GL-4a, decision 1: verify_fold for the group (Quick check: 2 frames
    // after 1 s), polled until it finishes.
    bool verify(const std::string &group);
    // Decision 2: while it runs, the side fields of a node of that group
    // take nothing (`fields` are greyed); the notice says why once per pick.
    void gateNodeFields(const std::vector<QWidget *> &fields);

    QAction *foldAction() const noexcept { return fold_action_; }

  private:
    [[nodiscard]] bool onRealRoot() const;
    [[nodiscard]] const GraphFoldGroup *find(const std::string &group) const;
    [[nodiscard]] std::vector<GraphFoldPick> picks() const;
    [[nodiscard]] std::map<std::string, std::int64_t, std::less<>> loopCounts() const;
    void say(bool error, const std::string &line);
    bool send(const std::string &group, std::vector<std::string> nodes, nlohmann::json folded,
              std::string line);
    void requestTicket();
    void refused(const nlohmann::json &response);
    void readGroups(const nlohmann::json &report);
    void readRoot(const nlohmann::json &response);
    void groupMenu(const std::string &group, const QPoint &screen);
    void requestVerifyTicket();
    void verifyFinished(const nlohmann::json &response);
    void tickVerify();
    // The scene's hooks.
    bool pressed(QGraphicsItem &item, Qt::MouseButton button);
    bool doubleClicked(QGraphicsItem &item);
    bool menu(QGraphicsItem &item, const QPoint &screen);
    void menuEntries(QMenu &menu);

    GraphCanvasReadCapability &graph_;
    GraphCanvasModel &model_;
    GraphCanvasScene &scene_;
    QWidget &owner_;
    Hooks hooks_;
    GraphCanvasFoldFields *fields_ = nullptr;
    QAction *fold_action_ = nullptr;
    QAction *add_selected_action_ = nullptr;
    QAction *unfold_action_ = nullptr;
    QAction *toggle_action_ = nullptr;
    QTimer *redraw_timer_ = nullptr;

    std::vector<GraphFoldGroup> groups_;
    std::set<std::string> folded_display_;
    std::string selected_;
    // The document digest the groups were last asked for, and the reads.
    std::string read_digest_;
    qint64 groups_request_ = 0;
    qint64 root_request_ = 0;
    bool reread_ = false;
    // The one fold_nodes in flight, its ticket, and what it was.
    qint64 edit_request_ = 0;
    qint64 ticket_request_ = 0;
    QString ticket_;
    std::string edit_line_;
    std::string edit_group_;
    bool edit_deletes_ = false;
    // Sent once more after a refusal for a moved document, from the
    // document read after it.
    std::function<bool()> retry_;
    std::optional<std::string> retry_after_;
    // GL-4a: the one verify_fold in flight, its ticket and since when.
    qint64 verify_request_ = 0;
    qint64 verify_ticket_request_ = 0;
    QString verify_ticket_;
    std::string verifying_;
    std::chrono::steady_clock::time_point verify_started_{};
    QTimer *verify_clock_ = nullptr;
    // Decision 3: a verify that found a difference (mismatch, unstable),
    // kept on the heading while the group's open form is the one it ran.
    struct VerifyMark {
        GraphFoldVerifyOutcome::Kind kind = GraphFoldVerifyOutcome::Kind::failed;
        std::string line;
        std::string digest;
    };
    std::map<std::string, VerifyMark> marks_;
    // The node whose fields were last said to wait (once per pick).
    std::string gated_node_;
};

} // namespace PelicanStudio
