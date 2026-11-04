#pragma once

// SDK-1b: the constants of the canvas drawing -- the data roles that make a
// rendered node inspectable, the kinds of the items drawn, the node geometry,
// and the small edge/reroute/tie shapes -- so a body (graphcanvasdrawing,
// graphcanvasstackbody) can name them without including the whole widget.
// graphcanvaswidget.hpp includes this, so every existing user still sees them.

#include "nodegraphics.hpp"

#include <QPointF>
#include <QtGlobal>

#include <string>
#include <vector>

namespace PelicanStudio {

// Data roles make the rendered canvas inspectable without coupling callers to
// private QGraphicsItem subclasses. Identity is the scope plus the node id,
// which is exactly the key a retained position is filed under.
inline constexpr int GraphCanvasOpRole = Qt::UserRole + 3120;
inline constexpr int GraphCanvasDomainRole = Qt::UserRole + 3121;
inline constexpr int GraphCanvasEvaluationRole = Qt::UserRole + 3122;
inline constexpr int GraphCanvasSinkRole = Qt::UserRole + 3123;
inline constexpr int GraphCanvasColumnRole = Qt::UserRole + 3124;
inline constexpr int GraphCanvasDefinitionRole = Qt::UserRole + 3125;
inline constexpr int GraphCanvasInputPortsRole = Qt::UserRole + 3126;
inline constexpr int GraphCanvasOutputPortsRole = Qt::UserRole + 3127;
inline constexpr int GraphCanvasDeclaredPortsRole = Qt::UserRole + 3128;
inline constexpr int GraphCanvasFromPortRole = Qt::UserRole + 3129;
inline constexpr int GraphCanvasToPortRole = Qt::UserRole + 3130;
inline constexpr int GraphCanvasPortRole = Qt::UserRole + 3131;
// "input" | "output" -- which side of the node the socket belongs to, so the
// same port name on both sides stays two distinct endpoints.
inline constexpr int GraphCanvasSocketDirectionRole = Qt::UserRole + 3132;
inline constexpr int GraphCanvasPortTypeRole = Qt::UserRole + 3133;
inline constexpr int GraphCanvasSocketShapeRole = Qt::UserRole + 3134;
inline constexpr int GraphCanvasLabelRole = Qt::UserRole + 3135;
inline constexpr int GraphCanvasValueTextRole = Qt::UserRole + 3136;
inline constexpr int GraphCanvasValueSourceRole = Qt::UserRole + 3137;
// True on the heading band and on everything drawn inside it. A double-click
// there renames the node; anywhere else on the node it descends an instance,
// and one flag keeps that split from depending on which child was hit.
inline constexpr int GraphCanvasHeadingRole = Qt::UserRole + 3138;
inline constexpr int GraphCanvasCollapsedRole = Qt::UserRole + 3139;
// Which control point of an edge, and which slot of a bundle. The automation
// surface builds `graph_reroute#<node>/<port>/<index>` and `graph_tie#<index>`
// out of the name and port roles; these two are what a reader of the item
// needs in order to say the same thing without parsing a string.
inline constexpr int GraphCanvasControlIndexRole = Qt::UserRole + 3140;
inline constexpr int GraphCanvasTieSlotRole = Qt::UserRole + 3141;
// LP-4c: a node the last refused edit pointed at carries the sentence the
// refusal read as (empty otherwise); it is drawn with a red rim.
inline constexpr int GraphCanvasRefusalRole = Qt::UserRole + 3142;
// LP-4d1: an item drawn inside a loop's zone on the root canvas -- a body
// node, its sockets, pills, edges and control points -- carries its
// definition here as well as in its scope. The automation surface puts it in
// front of the item's name (`graph_node#<definition>/<id>`), because a body
// node's id can be a root node's too; an item drawn in the scope on screen
// carries nothing, and keeps the name it always had.
inline constexpr int GraphCanvasZoneScopeRole = Qt::UserRole + 3143;
// A zone's tab: the branch it shows (an index into the loop's branches).
inline constexpr int GraphCanvasBranchRole = Qt::UserRole + 3144;
// LP-4d2. Which edge of its frame a thing on a zone's frame stands on, so the
// frame can follow its body during a drag without being drawn again
// (decision 7): GraphCanvasZoneAnchor below.
inline constexpr int GraphCanvasZoneAnchorRole = Qt::UserRole + 3145;
// Decision 4: the element a line into a loop reads of another loop's `p[]`
// ("element", "2"); empty on an ordinary edge. Such a line is dashed.
inline constexpr int GraphCanvasEdgeElementRole = Qt::UserRole + 3146;
// How tall the line along a zone's bottom is, on the carry arrows that run
// above it.
inline constexpr int GraphCanvasZoneFooterRole = Qt::UserRole + 3147;
// Q-19. An input socket drawn from a render node's shader declarations
// carries its key here -- "in@<binding>" or "in:variadic", which is also its
// port role, so its automation name is `graph_socket#<node>/in@0` -- and,
// when a numbered port is connected there, that port's name, which is what
// the document's edge names and what the edge is drawn to.
inline constexpr int GraphCanvasSocketKeyRole = Qt::UserRole + 3148;
inline constexpr int GraphCanvasBoundPortRole = Qt::UserRole + 3149;
// GL-1: true on a folded node (a feature, a pass no graph node made) and on
// everything drawn for it. Such a node is read only: nothing on it edits.
inline constexpr int GraphCanvasDerivedRole = Qt::UserRole + 3150;
// GL-1, decision 5: the domain a stub's hidden nodes are in.
inline constexpr int GraphCanvasStubDomainRole = Qt::UserRole + 3151;
// GL-1e: on an input socket of a rendering-section pass that takes lines, and
// on the line drawn to one: the index in the pass's `input` it stands for, or
// -1 for the empty `+ input` slot (whose port is "in:+").
inline constexpr int GraphCanvasPassInputRole = Qt::UserRole + 3152;
// NB-1 (design_node_bodies 2.2): on a node and its heading's triangle, the
// view it is drawn at ("open", "compact", "minimal"); on the triangle also
// the view a press on it goes to.
inline constexpr int GraphCanvasNodeViewRole = Qt::UserRole + 3153;
inline constexpr int GraphCanvasNextViewRole = Qt::UserRole + 3154;
// NB-2b (design_node_bodies 2.3): on each part of a layer row of a
// canvas.stack's body, the element of `layers` it stands for (0 the bottom),
// whether that layer is shown, whether the row is the picked one, and whether
// the stack is read only (an imported PSD's). The row's port role is the
// layer's input; the node's name role the stack.
inline constexpr int GraphCanvasLayerIndexRole = Qt::UserRole + 3155;
inline constexpr int GraphCanvasLayerVisibleRole = Qt::UserRole + 3156;
inline constexpr int GraphCanvasLayerPickedRole = Qt::UserRole + 3157;
inline constexpr int GraphCanvasLayerReadOnlyRole = Qt::UserRole + 3158;
// NB-2c: on a row's triangle (graph_stack_expand), whether the row is open.
inline constexpr int GraphCanvasLayerOpenRole = Qt::UserRole + 3161;
enum class GraphCanvasZoneAnchor : int {
    none = 0,
    left = 1,
    right = 2,
    bottom = 3,
    // A mark in the heading band that keeps to its right end.
    band_right = 4,
};
inline constexpr int GraphCanvasScopeRole = NodeGraphGraphRole;

inline constexpr auto GraphCanvasNodeItem = "graph_node";
inline constexpr auto GraphCanvasNodeLabelItem = "graph_node_label";
inline constexpr auto GraphCanvasNodeHeaderItem = "graph_node_header";
inline constexpr auto GraphCanvasSocketItem = "graph_socket";
inline constexpr auto GraphCanvasValuePillItem = "graph_value_pill";
// AG-3b: what the published generation left on an output port. It is a band
// and not a pill because it is not editable: a pill is a number the document
// stores and a click opens, this is a reading of what came out.
inline constexpr auto GraphCanvasValueBandItem = "graph_value_band";
inline constexpr auto GraphCanvasEdgeItem = "graph_edge";
inline constexpr auto GraphCanvasEdgeArrowItem = "graph_edge_arrow";
inline constexpr auto GraphCanvasBreadcrumbItem = "graph_breadcrumb";
// The fold triangle in the heading band. Its own kind, because a press on it
// must fold the node rather than start dragging it.
inline constexpr auto GraphCanvasCollapseItem = "graph_node_collapse";
// The line that follows the cursor between grabbing a socket and dropping on
// one. It is not an edge: nothing in the document answers for it yet.
inline constexpr auto GraphCanvasPendingEdgeItem = "graph_pending_edge";
// The in-place editor a pill or a heading opens.
inline constexpr auto GraphCanvasEditorItem = "graph_inline_editor";
// N-6: the two control points. A reroute is one dot on one edge; a tie is one
// bar several edges run through side by side. Both are absolute -- a node that
// moves does not carry them -- which is what makes a bend outlive a drag.
inline constexpr auto GraphCanvasRerouteItem = "graph_reroute";
inline constexpr auto GraphCanvasTieItem = "graph_tie";
// LP-4d1, the loop zone. The frame itself is the loop's `graph_node` (so it
// moves, is picked and is named as the loop always was); these are what it
// holds. `graph_zone#<loop>` is the inside of the frame, where the body is
// drawn; `graph_zone_tab#<loop>/<branch>` one branch's tab;
// `graph_zone_socket#<loop>/out:<port>` the inside end of one body output on
// the right edge (the outside ends are the loop's own sockets, and a body's
// boundary input is its graph.input's `value` socket on the left edge);
// `graph_zone_link` the line from the node that makes a body output to it;
// `graph_zone_label#<loop>` the zone's stage and `shared` marks.
inline constexpr auto GraphCanvasZoneItem = "graph_zone";
inline constexpr auto GraphCanvasZoneTabItem = "graph_zone_tab";
inline constexpr auto GraphCanvasZoneSocketItem = "graph_zone_socket";
inline constexpr auto GraphCanvasZoneLinkItem = "graph_zone_link";
inline constexpr auto GraphCanvasZoneLabelItem = "graph_zone_label";
// LP-4d2. `graph_zone_socket#<loop>/in:+` and `.../out:+` are the empty
// slots at the end of the two edges (decisions 1 and 2);
// `graph_zone_carry#<loop>/<from>/<to>` one pair of the loop's carry, an
// arrow round the bottom of the frame (picked, Delete removes it), with
// `graph_zone_carry_mark#...` beside it when `to` has no loop input edge;
// `graph_zone_label#<loop>/expansion` the line along the bottom (`×N
// copies`, `N materials`) and `graph_zone_button#<loop>/frame_plan` its Show
// in Frame Plan; `graph_edge_label#<node>/<port>` the element a dashed line
// reads.
inline constexpr auto GraphCanvasZoneCarryItem = "graph_zone_carry";
inline constexpr auto GraphCanvasZoneCarryMarkItem = "graph_zone_carry_mark";
inline constexpr auto GraphCanvasZoneButtonItem = "graph_zone_button";
inline constexpr auto GraphCanvasEdgeLabelItem = "graph_edge_label";
// GL-1. A folded node is a `graph_node` (named by its id: the pass's name, or
// `feature:<name>`) carrying GraphCanvasDerivedRole; its rows' ends are
// `graph_derived_socket#<id>/<resource>`; `graph_derived_edge#<from>-><to>`
// is a line the frame plan implies (dashed, read only);
// `graph_domain_stub#<node>/<port>` the cut end of a line to a node the
// domain filter hides; `graph_folded_button#show_decomposed` the button under
// a feature's breadcrumb.
inline constexpr auto GraphCanvasDerivedSocketItem = "graph_derived_socket";
inline constexpr auto GraphCanvasDerivedEdgeItem = "graph_derived_edge";
inline constexpr auto GraphCanvasDomainStubItem = "graph_domain_stub";
inline constexpr auto GraphCanvasFoldedButtonItem = "graph_folded_button";
// GL-1e. `graph_pass_input_edge#<from>-><pass>`: a line the rendering
// section's document names -- one item of a pass's `input` -- drawn solid from
// the node whose output is that resource. Picked, Delete takes it out.
inline constexpr auto GraphCanvasPassInputEdgeItem = "graph_pass_input_edge";
// GL-2b, decision 2. `graph_layer_band#<id>`: the line under a root node
// that says what its lower layers hold (`×3 copies · 2 barriers`).
inline constexpr auto GraphCanvasLayerBandItem = "graph_layer_band";
// NB-2b, a canvas.stack's body (design_node_bodies 2.3). Every part is named
// `<kind>#<stack>/<input>` by the layer's input, top row first on screen:
// `graph_stack_row` the row (a press picks it, a drag moves it), its
// `graph_stack_eye`, `graph_stack_thumb` (the empty 32 px frame NB-3 fills),
// `graph_stack_name` (a double-click renames), `graph_stack_blend`,
// `graph_stack_opacity` (a sideways drag sets it), `graph_stack_mask` and
// `graph_stack_enter` (`›`). Without a port:
// `graph_stack_composite#<stack>` (the 64 px frame), and on the band under
// the rows `graph_stack_add#<stack>` (+ ▾), `graph_stack_delete#<stack>`
// and `graph_stack_note#<stack>`. `graph_stack_drop_line` is the line a drag
// or a line being drawn says it will land on.
inline constexpr auto GraphCanvasStackRowItem = "graph_stack_row";
inline constexpr auto GraphCanvasStackEyeItem = "graph_stack_eye";
inline constexpr auto GraphCanvasStackThumbItem = "graph_stack_thumb";
inline constexpr auto GraphCanvasStackNameItem = "graph_stack_name";
inline constexpr auto GraphCanvasStackBlendItem = "graph_stack_blend";
inline constexpr auto GraphCanvasStackOpacityItem = "graph_stack_opacity";
inline constexpr auto GraphCanvasStackMaskItem = "graph_stack_mask";
inline constexpr auto GraphCanvasStackEnterItem = "graph_stack_enter";
inline constexpr auto GraphCanvasStackCompositeItem = "graph_stack_composite";
inline constexpr auto GraphCanvasStackAddItem = "graph_stack_add";
inline constexpr auto GraphCanvasStackDeleteItem = "graph_stack_delete";
inline constexpr auto GraphCanvasStackNoteItem = "graph_stack_note";
inline constexpr auto GraphCanvasStackDropLineItem = "graph_stack_drop_line";
// NB-5: the small mark on a stack's band while the rows it draws are a shape
// the engine has not answered for yet. Clicks go through it.
inline constexpr auto GraphCanvasStackPendingItem = "graph_stack_pending";
// NB-2c (design_node_bodies 2.3, a row opened): `graph_stack_expand` the
// small triangle at a row's left (a press opens or closes the row); on an
// open row's stage, `graph_stack_stage` its area, `graph_stack_opacity_field`
// the 0..100 % slider (a sideways drag sets it, a double-click types it) --
// or, while a line drives the opacity, `graph_stack_opacity_source`
// ("<- <the line's node>") -- and `graph_stack_mask_source` the mask's.
inline constexpr auto GraphCanvasStackExpandItem = "graph_stack_expand";
inline constexpr auto GraphCanvasStackStageItem = "graph_stack_stage";
inline constexpr auto GraphCanvasStackOpacityFieldItem = "graph_stack_opacity_field";
inline constexpr auto GraphCanvasStackOpacitySourceItem = "graph_stack_opacity_source";
inline constexpr auto GraphCanvasStackMaskSourceItem = "graph_stack_mask_source";
// NB-3 (design_node_bodies 2.4): a canvas source's or filter's card --
// `graph_card_thumb#<node>` its 64 px picture, `graph_card_value#<node>` a
// filter's one number beside the heading. The pictures' frames (the card's,
// a stack's composite and rows') carry GraphCanvasThumbnailDigestRole and
// GraphCanvasThumbnailStateRole (graphcanvasthumbnails.hpp).
inline constexpr auto GraphCanvasCardThumbItem = "graph_card_thumb";
inline constexpr auto GraphCanvasCardValueItem = "graph_card_value";
// SDK-2a: an open value.curve's fixed-size plot. `graph_curve_plot` is the
// curve itself, `graph_curve_keys` its key marks, `graph_curve_frame` the
// plot's frame. None takes a press: dragging one moves the node.
inline constexpr auto GraphCanvasCurvePlotItem = "graph_curve_plot";
inline constexpr auto GraphCanvasCurveKeysItem = "graph_curve_keys";
inline constexpr auto GraphCanvasCurveFrameItem = "graph_curve_frame";

// The node is a heading band over two socket sections, so its height follows
// its port count rather than a constant. The numbers are here so the scene
// test can name the same geometry the canvas draws.
// UX-5a: the least zoom Fit view goes to -- a node's words still readable in
// the bottom dock. Content bigger than that is looked at by the picked node.
inline constexpr qreal GraphCanvasFitFloor = 0.7;
inline constexpr qreal GraphCanvasNodeMinimumWidth = 196.0;
inline constexpr qreal GraphCanvasNodeMaximumWidth = 340.0;
inline constexpr qreal GraphCanvasHeaderHeight = 32.0;
inline constexpr qreal GraphCanvasSocketRowHeight = 21.0;
inline constexpr qreal GraphCanvasSectionPadding = 7.0;
// SDK-2a: the fixed plot under an open value.curve's rows, and its side
// margin. Only `open` adds the 72 px and the section padding; compact and
// minimal keep the default body's height and draw no plot.
inline constexpr qreal GraphCanvasCurvePlotHeight = 72.0;
inline constexpr qreal GraphCanvasCurvePlotMargin = 10.0;
inline constexpr qreal GraphCanvasSocketSize = 11.0;
inline constexpr qreal GraphCanvasNodeCornerRadius = 8.0;
// N-6. A reroute is drawn as a dot this wide. A tie is a bar of this width,
// carrying one edge per slot `GraphCanvasTieSpacing` apart; each edge crosses
// it level, entering and leaving `GraphCanvasTieReach` either side of its
// centre, which is what makes a bundle read as parallel lines rather than as
// several lines that happen to meet.
inline constexpr qreal GraphCanvasRerouteSize = 9.0;
inline constexpr qreal GraphCanvasTieWidth = 10.0;
inline constexpr qreal GraphCanvasTieReach = 9.0;
inline constexpr qreal GraphCanvasTieSpacing = 16.0;
inline constexpr qreal GraphCanvasTiePadding = 7.0;
// LP-4d1, the zone's frame. The body is drawn in the frame's own coordinates
// shifted by (GraphCanvasZoneContentLeft, the heading and its tabs, then
// GraphCanvasZonePadding): a body node at the definition's `layout.nodes`
// (x, y) is that far from the inside origin. The side columns carry the
// boundary sockets, an outside and an inside one per row.
inline constexpr qreal GraphCanvasZoneColumn = 96.0;
inline constexpr qreal GraphCanvasZoneContentLeft = 136.0;
inline constexpr qreal GraphCanvasZonePadding = 24.0;
inline constexpr qreal GraphCanvasZoneTabHeight = 24.0;
inline constexpr qreal GraphCanvasZoneMinimumWidth = 420.0;
inline constexpr qreal GraphCanvasZoneMinimumContentHeight = 96.0;
// LP-4d2: the line along the bottom of a reported zone, and the lanes its
// carry arrows run in above it.
inline constexpr qreal GraphCanvasZoneFooterHeight = 26.0;
inline constexpr qreal GraphCanvasZoneCarryLane = 7.0;

// N-6. One edge, named by the input end it lands on. That end is unique -- one
// port of one node takes at most one edge -- and it is how the document's
// `layout` names an edge too, so the canvas and the document agree by shape
// rather than by convention.
struct GraphCanvasEdgeKey {
    std::string node;
    std::string port;

    friend bool operator==(const GraphCanvasEdgeKey &,
                           const GraphCanvasEdgeKey &) = default;
    friend auto operator<=>(const GraphCanvasEdgeKey &,
                            const GraphCanvasEdgeKey &) = default;
};

// The control points on one edge, in the order the drawing meets them.
struct GraphCanvasReroute {
    GraphCanvasEdgeKey edge;
    std::vector<QPointF> points;
};

// Several edges carried through one place side by side. `position` is where
// slot 0 crosses; slot n is `GraphCanvasTieSpacing` below slot n-1, so the
// order of `edges` is the order of the lines.
struct GraphCanvasTie {
    QPointF position;
    std::vector<GraphCanvasEdgeKey> edges;
};

} // namespace PelicanStudio
