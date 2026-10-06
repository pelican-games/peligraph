# Studio scenarios

One JSON file here is one end-to-end check of the Studio GUI: a real window, the
real engine embedded in it, and a driver that clicks and types the way a person
would. Each file becomes one CTest test, `studio_scenario_<name>`, in the GPU
lane.

    ctest --test-dir build -C Debug -R studio_scenario_graph_read -j1 --output-on-failure

The test copies `test/fixtures/studio_automation_graph` to
`build/test_artifacts/studio_scenarios/<name>/project` (and a second, untouched
copy beside it) and runs `tools/studio_drive.py` against the copy. A scenario
that needs a document of its own puts it in `test/fixtures/studio_<name>/` and
gets that copied instead -- `frameplan_edit_roundtrip` does, because it edits a
project pass and adding one to the shared fixture would change what every other
scenario is looking at. Everything
the run produced is in that directory: `report.json`, `studio.log`, and the
screenshots.

A scenario whose project runs game code (a behavior) has a game logic DLL
target named `pelican_studio_<name>_game_logic` in `test/CMakeLists.txt`; the
test copies the DLL into the run directory and Studio starts every engine
with `--game-logic` on it (`PELICAN_STUDIO_PLAYER_ARGUMENTS`).
`play_pause_step` does, for the object its behavior moves.

To run one by hand, without CTest:

    uv run tools/studio_drive.py \
      --studio build/dist/Debug/pelican_studio.exe \
      --player build/dist/Debug/pelican_player.exe \
      --scenario test/studio_scenarios/graph_read.json \
      --project <a copy of a project> \
      --var other_project=<a second copy> \
      --artifacts out/graph_read \
      --report out/graph_read/report.json

Qt has to be on `PATH` (Studio is not deployed with its own copy); the CTest
lane puts it there for you.

## Reading a report

`report.json` is written whether the run passed or failed. Read it in this
order:

* `verdict` (`"green"` / `"red"`) and `ok`, with `failed_steps` -- how many
  steps were not satisfied.
* `summary` -- one line per step, in order: index, `ok`/`FAIL`, duration, kind
  and label, and for a failure the reason.
* `failures` -- for each failed step, the request that was sent, the reply that
  came back, the reason in words, and `screenshot`: a picture of the window as
  it stood when the step failed.
* `steps` -- the same thing in full, for when the summary is not enough.
* `studio_log` -- Studio's and the engine's console output for the whole run.

The final `shutdown` step requires Studio to exit with code zero without forced
termination. A non-zero exit, a shutdown timeout, or a watchdog termination makes
the report red and the driver return non-zero even when all scenario steps pass.
`studio_exit_code` and `studio_shutdown_forced` record the outcome; `failures`
includes the shutdown reason.

## Writing a scenario

    {
      "schema": "pelican.studio_scenario", "version": 1,
      "name": "graph_read",
      "tab": "graph",                     // --tab, brought to the front at launch
      "probe_no_automation": true,        // first launch a Studio without the
                                          // flag and check no channel exists
      "viewport": [720, 450],             // optional; the engine viewport's size
                                          // in picture pixels (default 720x450),
                                          // null to leave it free
      "steps": [ ... ]
    }

The runner sends `viewport_size` right after connecting, before the first
step, so the engine viewport keeps the same size whatever panels open or
close and pixel points land on the same pixels from run to run (PX-2). The
pixel-reading scenarios were measured at the default; changing it means
measuring their points again.

A step is either a **command**, sent to Studio as-is:

    {"label": "click the gain pill", "cmd": "click",
     "name": "pelican.graphCanvasView::graph_value_pill#left/gain"}

optionally with `"expect_error": "automation.widget_missing@1"`, which turns the
step green only when the command is refused with exactly that code; or an
**expectation**, which issues the command it needs and compares:

| `expect`      | fields                             | passes when                                  |
|---------------|------------------------------------|----------------------------------------------|
| `rpc_equals`  | `method`, `params`, `path`, `value`| the engine's reply has `value` at `path`      |
| `text_equals` | `name`, `value`                    | the named thing reads exactly `value`         |
| `visible`     | `name`, `value` (default `true`)   | the named thing is/is not on screen           |
| `list_count`  | `pattern`, `value`                 | `list` matched exactly `value` things         |
| `result`      | `command`, `path`, and see below   | the command's own reply has what `path` says  |
| `screenshot`  | `name?`, `path?`, `mode?`          | always -- it only saves the picture           |
| `engine_windows` | `engines`, `min_hidden_samples?` | see below (needs `watch_engine_windows`)   |

`engine_windows` (EW-2) asks about the time between steps rather than about
one moment. A scenario with `"watch_engine_windows": true` has the driver list
the top-level windows of Studio's child processes from the launch on, many
times a second. The step passes when none of them was ever visible (nothing
showed outside Studio), when at least `engines` engines were seen loading --
their marked render window (EW-1) hidden and still top-level in at least
`min_hidden_samples` (default 3) samples -- and when the newest engine's window
is now on screen as a child of Studio's. `engine_window_embedded` uses it.

`path` walks the JSON reply: `nodes[1].parameters.gain`, and `$len` for the
length of a list or object (`definitions.$len`).

`rpc_equals` also takes `retry_ms`: it asks the same question again, a tenth of
a second apart, until the answer is the expected one or the time is up. Use it
for a reading the engine reaches on its own clock -- N-6's placement waits out
a 300 ms debounce before it is even sent, so the first reading after a gesture
can be one edit behind. Without `retry_ms` the step is a single reading.

`result` is the general one: it sends any command and asks a question about the
reply, which is how `view_state` and `menu_list` are checked.

    {"expect": "result", "command": {"cmd": "view_state", "name": "pelican.framePlanLogicalView"},
     "path": "scale", "remember": "before"}
    {"expect": "result", "command": {"cmd": "view_state", "name": "pelican.framePlanLogicalView"},
     "path": "scale", "op": "gt", "value_of": "before"}

* `value` is a literal; `value_of` is what an earlier step wrote down with
  `remember`. A step with neither compares nothing: it must `remember` the
  value for a later step, or say `"record": true` (a reading kept for the
  report's reader, such as a measurement); otherwise it fails (QD-1). A name
  remembered and never compared by a later step is a warning in the report.
* `op` is `eq` (the default), `ne`, `gt`, `ge`, `lt`, `le`, `contains`,
  `excludes` (the text does not hold the value -- how a scenario says a message
  never appeared), `has_key` or `lacks_key` (an object has / has no key of that
  name). QD-1 refuses, as a failed step, the forms that pass whatever happened:
  `contains` / `excludes` on an object (Python's `in` reads its keys -- ask
  `has_key` / `lacks_key`, or walk the path to the value), an `excludes` from a
  list of a value of a kind none of its elements is, `contains` / `excludes` of
  `""`, and `ne` between two kinds (null apart). `eq` tells a bool from a number:
  `true` is not `1`.
* `stable_for_frames: N` reads the value again once the engine has drawn N more
  frames and requires the same value: "and it stays so".
* `plus` adds a number to `value_of` (`"value_of": "frame", "plus": 1` is one
  step later), and `value_format` is a text made of what earlier steps
  remembered (`"Paused · frame {frame}"`) -- PM-3a's `play_pause_step` uses
  both.
* `retry_ms` asks again, a fifth of a second apart, until the answer
  satisfies the step or the time is up, as it does on `rpc_equals`. What the
  engine draws after a shader file is saved arrives on the engine's clock:
  `fullscreen_pass_from_template` asks for a `screen` screenshot until its
  `distinct_colors` says the new shader is on screen.
* A gesture is judged by which way a number moved, not by where it landed: the
  scale a view chooses is the view's business, and a scenario that hard-codes it
  is checking arithmetic instead of checking the gesture.

`{project}`, `{artifacts}` and any `--var NAME=VALUE` are substituted into every
string of a step, so a scenario can name the temp copy it was given.

The run stops at the first failed step.

## The automation channel

`pelican_studio --automation <name>` opens a `QLocalServer` on `<name>` (on
Windows, `\\.\pipe\<name>`) and speaks one JSON object per line:

    -> {"id": 4, "cmd": "click", "name": "pelican.graphCanvasRefresh"}
    <- {"id": 4, "ok": true, "result": {"point": [40, 12], ...}}
    <- {"id": 4, "ok": false, "error": {"code": "automation.widget_missing@1",
                                        "message": "..."}}

Without the flag the server object is never constructed, so the socket does not
exist. `probe_no_automation` is the check that this stays true.

Things are named by `objectName` (`grep setObjectName src/devstudio/view`). A
thing drawn inside a `QGraphicsView` -- a graph node, a socket, a value pill --
has no widget of its own and is named
`<view objectName>::<kind>#<node id>[/<port>]`, for example
`pelican.graphCanvasView::graph_value_pill#left/gain`. `list` publishes both
kinds of name, so a scenario can be written by reading a listing rather than by
reading the source.

A menu entry is a `QAction`, not a widget: it has no place on screen until the
menu is open. Actions are named the same way widgets are (`grep setObjectName
src/devstudio/view`), `list` publishes them with `"class": "QAction"`, and
`click` on one triggers it rather than pressing a point -- so a scenario reaches
`pelican.view.theme.dark` without driving a popup.

N-6 added two more drawn things to the graph canvas, named the same way. A
control point on one edge is `graph_reroute#<node>/<port>/<index>` -- the edge
is named by the *input* end it lands on, because that end is unique, and the
index counts the control points of that edge from left to right. A bundle is
`graph_tie#<index>`, counted in the order the document lists them. Both are
draggable items, so `drag` moves them; `click` picks one, and `click` with
`"modifiers": ["ctrl"]` adds to what is already picked, which is how the two
lines a bundle is made of are chosen. Both are drawn over the nodes, so a
gesture aimed at one reaches it even where it lies across a node -- a bundle
sits in the middle of the lines it carries, and that is often over a box. The canvas menu's two new entries are
named QActions like the others: `pelican.graphCanvas.tieTogether` and
`pelican.graphCanvas.untie`, both switched off until enough is picked.

The placement itself is document state, so a scenario reads it back with `rpc`:
`get_graph_instances` carries the root scope's under `layout` and each
definition's under `definitions.<name>.layout`, in the shape
`{"nodes": {...}, "reroutes": [...], "ties": [...]}`. `graph_reroute_tie` is
the example, and it is also where "a control point does not move when the node
it is near does" is checked against a live window.

N-8 made the Frame Plan canvas an editing surface, and its drawn things are
named the same way. A pass or task is `node#<pass name>`. A socket is
`node_socket#<pass name>/out:<resource>` or `.../in:<resource>` -- the side is
part of the name because one pass can read and write the same target. An
in-place band is `node_inline_field#<pass name>/<field>`, where the field is
`shader.vertex`, `shader.fragment` or `dispatch`, and the editor it opens is
the widget `pelican.framePlanInlineEditor`. A band takes the press that lands
on it, so a scenario that means "move this node" drags it by its heading,
`node_header#<pass name>`, the way a person does. A plain drag from an output socket
onto a pass connects; a plain drag off an input socket into open canvas cuts;
Shift-drag still opens the dialogs, as it always did. The canvas menu's two
insert entries are named QActions like the placement ones:
`pelican.framePlan.addFullscreenPass` and `pelican.framePlan.addComputeTask`,
both acting after the node that is picked. The placement the canvas keeps is
the `frame_plan` member of the same root `layout` the Graph tab writes, so
`get_graph_instances` carries it at `layout.frame_plan.<graph>.nodes.<pass>`;
`frameplan_edit_roundtrip` is the example.

VI-3a draws a fullscreen or raster pass's input sockets from its shader's
declarations (`get_frame_plan` `nodes[].pass_inputs`), and a drop connects by
binding number, so those sockets are named by binding:

| name | what it is |
|------|------------|
| `node_socket#<pass>/in@<binding>` | one declaration, or one connected member of the variadic group; empty or fed |
| `node_socket#<pass>/in:variadic` | the empty end of the variadic band, where a drop adds one input |
| `node_socket#<pass>/in:<resource>` | still answers: the socket that resource is connected to (not listed by `list`); an input no declaration takes keeps this as its own name |
| `node_row_label#<pass>/in@<binding>` | the row's text, `<declaration> ← <resource>` |
| `node_socket_band#<pass>` | the variadic band itself (drawing only) |
| `node_socket_warning#<pass>/in@<binding>` | the mark on a declaration named for another connected resource |

A drop on such a pass's body (its heading, say) adds nothing. A pass whose
shader declares no input at all, and a compute task, keep the N-8 drop onto the
node. After a reload the engine refused for a pass, the status line names the
binding it lost, beside `pelican.framePlan.reloadRefused.disconnect` and
`pelican.framePlan.reloadRefused.openShader`. `sockets_from_shader` is the
example.

VI-3b adds these:

| name | what it is |
|------|------------|
| `node_socket_fix#<pass>` | the Fix button beside the variadic band's end: Fix sockets |
| `pelican.framePlan.fixSockets` | the same, as a named menu entry acting on the picked node |
| `pelican.framePlan.makeSocketsVariadic` | the menu entry that puts back the line Fix sockets replaced (offered only when the shader holds that block) |
| `pelican.framePlan.shaderContract.usePassthrough` | Use engine://passthrough: the inputs and that fragment in one edit (it replaced `...makePassThrough`) |
| `pelican.fullscreenPass.fixSockets` | the Pass Editor's Fix sockets, beside the fragment |

A `click` on a connected socket of a pass that draws with
`engine://passthrough` writes `PELICAN_PASSTHROUGH_INPUT` into its
`shader.defines`; that socket's row label ends in ` (shown)`. A drop on a pass
whose shader declares no input is not sent: the status line says
`<pass>'s shader declares no inputs.` beside Use engine://passthrough and Open
the shader. `sockets_fix_and_show` is the example.

LP-4d1 draws each root `graph.loop` on the Graph tab as a zone: a frame with the
body of the branch its tab shows drawn inside, edited in that definition. The
frame is the loop's own `graph_node#<loop>` (a `click` on it lands on its
heading, not in its body), and a thing drawn inside the zone carries the body's
definition in front of its name -- a body node's id can be a root node's too.
Neither a definition name nor an id holds a slash, so the first one is where
they part. Opened with the loop's body (Open body), the canvas names the body's
nodes as it always did, without the definition.

| name | what it is |
|------|------------|
| `graph_zone#<loop>` | the inside of the frame, where the body is drawn |
| `graph_zone_label#<loop>/stage` | the zone's stage: `expand ×N`, `draw_loop N materials` or `loop` before the report |
| `graph_zone_label#<loop>/shared` | `shared (N users)` when another instance or loop runs the same definition |
| `graph_zone_tab#<loop>/<branch>` | one branch's tab (`if …` / `elif …` / `else`); a click shows that branch's body. None when the else is the only branch |
| `graph_socket#<loop>/<port>` | the loop's own sockets on the frame's edges: its inputs on the left, `p` and `p[]` on the right |
| `graph_socket#<definition>/<input>/value` | the inside end of a boundary input on the left edge: the body's `graph.input` `<input>` (drag from it to a body input) |
| `graph_zone_socket#<loop>/out:<output>` | the inside end of the body output `<output>` on the right edge (drawing only) |
| `graph_zone_link#<definition>/<loop>/out:<output>` | the line from the port that makes that output to it (drawing only) |
| `graph_node#<definition>/<id>` | a body node drawn in the zone; its heading, sockets, pills and edges follow the same rule (`graph_node_header#<definition>/<id>`, `graph_socket#<definition>/<id>/<port>`, ...) |

With the zone (or a node inside it) picked, `pelican.graphCanvas.addFullscreenPass`
and the palette add to its body; the canvas menu opened inside the frame does the
same. `loop_zone_edit` is the example.

LP-4d2 draws lines across the zone's edge. A root output dropped on the left
edge's empty slot (`graph_zone_socket#<loop>/in:+`) makes a loop input, named
after the port, and a `graph.input` of that name in every body the branches run
-- one edit; dropped on a body node's input inside the zone, the same edit also
connects that input. A body output dropped on the right edge's slot
(`graph_zone_socket#<loop>/out:+`) is declared by every body (`set_outputs`)
and handed on by the loop as `p`; when another branch's body has no node of
that id to give it from, nothing is sent and the notice says which body. A line
from a body output's inside end (`graph_zone_socket#<loop>/out:<output>`) to a
boundary input's inside end is a carry.

| name | what it is |
|------|------------|
| `graph_zone_socket#<loop>/in:+`, `.../out:+` | the empty slot at the end of the left and the right edge |
| `graph_zone_carry#<loop>/<from>/<to>` | one pair of the loop's carry: an arrow round the bottom of the frame (a click picks it, Delete removes it) |
| `graph_zone_carry_mark#<loop>/<from>/<to>` | the warning beside it when `<to>` has no loop input edge |
| `graph_zone_label#<loop>/expansion` | what the loop became, along the bottom: `×N copies`, `N materials` |
| `graph_zone_button#<loop>/frame_plan` | Show in Frame Plan: the Frame Plan comes forward with the loop's copies picked |
| `graph_edge_label#<node>/<port>` | the element a dashed line reads of another loop's `p[]` (`[element]`) |

A graph node can have an input and an output of one name (a fullscreen pass's
`color`), which `graph_socket#<id>/<port>` does not tell apart:
`graph_socket#<id>/in:<port>` and `.../out:<port>` do (resolving only; `list`
publishes the plain name). `loop_zone_wire` is the example.

A picked loop's fields are `pelican.graphCanvasLoop.<field>`, and a branch
table row's are `pelican.graphCanvasLoop.branch<i>.<column>` (`i` counts from
0; the last row is the else). LP-4f1 made a loop run over a view axis and took
the typing out of the fields while the count is known; these are the names it
added (combos are `select`ed by the label a person reads):

| name | what it is |
|------|------------|
| `pelican.graphCanvasLoop.over` | Over: `Count` or `View axis` |
| `pelican.graphCanvasLoop.viewFamily` | the view family a view-axis loop runs over, from the frame plan's families that have axes |
| `pelican.graphCanvasLoop.viewAxis` | the axis of that family: one element per view along it |
| `pelican.graphCanvasLoop.viewCount` | the count, which is the axis's length (read only): `×6 (face)` |
| `pelican.graphCanvasLoop.viewAxisNote` | the note under the View row: which families are listed, or why an axis cannot be chosen (one decided each frame) |
| `pelican.graphCanvasLoop.countTarget` | behind the count's ƒ: a render target of the frame plan |
| `pelican.graphCanvasLoop.insertMipLevels`, `.insertLayers` | put `mip_levels_of("<target>")` / `layers_of("<target>")` into the count's expression |
| `pelican.graphCanvasLoop.whereChecks` | Where's checks, one per element while the count is known (up to 64) |
| `pelican.graphCanvasLoop.whereValue<i>` | the check for element `i`; on a view-axis loop its text carries the axis's label (`2 (+Y)`) |
| `pelican.graphCanvasLoop.branch<i>.value` | `element =`'s element: a spin while the count is known |
| `pelican.graphCanvasLoop.branch<i>.checks`, `.check<j>` | `element in` / `not in`: a check per element, and the one for element `j` |
| `pelican.graphCanvasLoop.fields` | the form and the rows together, switched off while an edit's round trip is under way |
| `pelican.graphCanvasPerIteration.element` | the per-iteration dialog's Element: `element`, `element - 1`, `element + 1` or `Custom…` |
| `pelican.graphCanvasPerIteration.element.custom` | the expression Custom… takes |
| `pelican.graphCanvasView::graph_zone_label#<loop>/axis<i>` | a view-axis zone's label for element `i`, in the zone's heading (`+X` ... `-Z` for a cube's faces) |

A render node picked on the Graph tab shows its View field under the Loop
fields, `pelican.graphCanvasRender` (the Pass Editor's field, with this prefix):

| name | what it is |
|------|------------|
| `pelican.graphCanvasRender.heading` | `View (<family>)`, or `View (<family>) · in loop <loop>` in a loop's body |
| `pelican.graphCanvasRender.view` | the field |
| `pelican.graphCanvasRender.view.<axis>` | one axis: a combo of `All views`, its values and, on the axis of the loop the node's body runs over, `Each element (loop)`; a label for an axis decided each frame |
| `pelican.graphCanvasRender.view.<axis>.remove`, `.warning` | the Remove button of an axis the family no longer has, and the warning under a row |

With a view-axis loop's zone picked, `pelican.graphCanvas.addFullscreenPass`
puts the pass in the loop's family and makes its image at the size the frame
plan gives that family (`family_extent`, LP-4f3). `loop_view_axis` is the
example.

Q-19 draws a render node's inputs from its shader, as the Frame Plan does: once
the frame plan lists the pass's declarations (through `loop_origin` for a
loop's body), the node's input sockets are named by binding, and a line dropped
on one adds a numbered image port and its edge in one edit. A node the frame
plan has no declarations for keeps its port rows and their names. Inside a
zone the definition goes in front, as above (`graph_socket#<definition>/<id>/in@0`).
Rename definition… and Delete definition open a row under the canvas's
controls. `loop_zone_read` is the example.

| name | what it is |
|------|------------|
| `graph_socket#<id>/in@<binding>` | the socket of the declaration at that binding: a fixed input, or a member of the variadic group |
| `graph_socket#<id>/in:variadic` | the open end of the variadic group: a line dropped here is the next binding |
| `graph_socket#<id>/in:<port>` | resolving only: the declared socket the numbered port `<port>` is connected at (`list` publishes the `in@<binding>` name) |
| `graph_node_label#<id>/in:band` | the band drawn behind the variadic group's sockets (drawing only) |
| `pelican.graphCanvas.renameDefinition`, `.deleteDefinition` | the QActions on the breadcrumb, a zone's header and the Scope combo; fired by name they act on the definition on screen, else the one the picked zone shows. Off while there is no definition to act on |
| `pelican.graphCanvasDefinitionRow` | the row they open, hidden until then |
| `pelican.graphCanvasDefinitionCaption` | `Rename definition <name> to` / `Delete definition <name>? Nothing runs it.` |
| `pelican.graphCanvasDefinitionName` | the new name (Rename only; starts as the one Studio proposes, `<runner>_body` after the first node that runs it, or the old one when nothing does) |
| `pelican.graphCanvasDefinitionApply`, `pelican.graphCanvasDefinitionCancel` | `Rename` or `Delete`, and Cancel |
| `pelican.graphCanvasDefinitionLine` | the one line saying why the new name cannot be sent (empty, a slash, `root`, another definition's name). A definition something still runs opens no row: the Graph tab's notice says what runs it |

LP-4f2 replaces a feature with its shipped loop version:

| name | what it is |
|------|------------|
| `pelican.renderFeatures.replaceWithLoopVersion` | the QAction acting on the feature picked in Render Features (the row's right-click fires it too) |
| `pelican.renderFeatures.replaceWithLoop` | the button under the list that fires it; shown only for a feature a template decomposes |
| `pelican.renderFeatures.replaceConfirm` | the confirmation: what the template does, and that Undo takes it back |
| `pelican.renderFeatures.replaceConfirm.ok`, `.cancel` | its Replace and Cancel |
| `pelican.framePlan.replaceWithLoopVersion` | the Frame Plan's entry on a feature's group; Render Features comes forward and asks the same |

`graph_template_bloom` is the example.

GL-3a folds root nodes of the Graph tab into a fold group (`fold_nodes`) and
draws it over the canvas. Folded or open is only how the group is looked at:
the document's `fold_groups` does not change with it.

| name | what it is |
|------|------------|
| `pelican.graphCanvas.foldIntoGroup` | Fold into group…: the picked root nodes into one group, named by Studio (`fold_1`, `<loop>_fold`), without a question |
| `graph_fold_group#<group>` | the frame round the group's nodes (drawing only) |
| `graph_fold_heading#<group>` | its heading: a click picks the group, a right-click opens its menu |
| `graph_fold_state#<group>` | the state's words: `not verified — decisions are not applied yet`, or `changed since verified — runs the open form` beside `graph_fold_mark#<group>` |
| `graph_fold_toggle#<group>` | Fold / Open on the heading |
| `graph_node#fold:<group>` | the group drawn folded; its rows are `graph_derived_socket#fold:<group>/<node>.<port>` |
| `pelican.graphCanvas.foldAddSelected`, `.unfoldGroup`, `.toggleFoldGroup` | the heading menu's entries, acting on the picked group; Unfold asks in `pelican.graphCanvasFold.unfoldConfirm` (`.ok`, `.cancel`) |
| `pelican.graphCanvasFold.note`, `.state` | the Decisions section's first lines |
| `pelican.graphCanvasFold.<field>.node`, `.element`, `.port`, `.add` | the Add row of `serial`, `isolate` or `no_alias` |
| `pelican.graphCanvasFold.<field>.row<i>`, `.remove<i>` | one decision, and its × |
| `pelican.framePlan.planning.serial`, `.isolate`, `.noAlias` | the Frame Plan's right-click entries on the picked Passes / Resources row (`edit_target_planning`) |

`graph_fold_decisions` is the example.

The editor the canvas opens over a pill or a heading is a real widget carried by
a drawn item, and is named the same way:
`pelican.graphCanvasView::graph_inline_editor#left/gain`. `type`, `key` and
`text` reach through the item to the widget, so they do not depend on which
window the desktop happens to have activated.

| command        | fields                                                       |
|----------------|--------------------------------------------------------------|
| `list`         | `pattern?` (case-insensitive substring of the name); each entry carries `name`, `class`, `visible`, `enabled`, `geometry`, `text`, `shortcut` |
| `click`        | `name` \| `point`, plus `scene?`, `point?`, `button?`, `modifiers?` |
| `double_click` | the same                                                      |
| `drag`         | `from`, `to` (a name, or `{name, point?/scene?}`), `button?`, `modifiers?`, `steps?` |
| `wheel`        | `name` \| `point`, `delta` (a notch is 120, positive is forward), `delta_x?`, `scene?`, `point?`, `modifiers?` |
| `move`         | `name` \| `point`, `scene?`, `point?`, `modifiers?` -- a move with whatever this surface is holding down |
| `press`        | the same, plus `button?` -- and it stays down                 |
| `release`      | the same                                                      |
| `key`          | `sequence` (`"Return"`, `"Ctrl+A"`), `name?`, `route?` (`widget` \| `window`) |
| `type`         | `text`, `name?`, `select_all?` (default true)                 |
| `text`         | `name`                                                        |
| `select`       | `name`, and `text` (the row or entry as a person reads it) or `index`, `timeout_ms?` (default 5000: how long a widget or an entry that is not there yet is waited for) |
| `action`       | `name` \| `text`, `menu?` -- fires one menu or toolbar entry  |
| `menu_list`    | `name?` (scope to one widget), `pattern?` -- the entries, with `text`, `shortcut`, `enabled`, `checked`, `menu` |
| `view_state`   | `name` -- a QGraphicsView's `scale`, `transform`, `center`, `scroll`, `visible_scene`, `viewport` |
| `layout_stats` | `name` -- what the automatic placement achieved on the scene a QGraphicsView shows: `crossings`, `overlaps`, `nodes`, `reversed_edges` |
| `wait`         | `name`, one of `visible` / `enabled` / `text` / `contains` / `missing`, `timeout_ms?` |
| `wait_engine`  | `timeout_ms?`, `frames?` -- until the viewport answers editor RPC; with `frames`, until the engine has published that many frames (`get_status` `render.published_frames`) |
| `rpc`          | `method`, `params?`, `timeout_ms?` -- straight to the engine   |
| `open_project` | `dir`                                                         |
| `screenshot`   | `path`, `name?`, `mode?` (`widget` \| `screen`), `pixels?`    |
| `quit`         | --                                                            |

One example of each of the kinds Q-1b added:

    {"cmd": "wheel",  "name": "pelican.framePlanLogicalView", "point": [60, 50], "delta": 480}
    {"cmd": "move",   "name": "pelican.framePlanLogicalView", "point": [80, 60]}
    {"cmd": "press",  "name": "pelican.framePlanLogicalView", "point": [80, 60], "button": "middle"}
    {"cmd": "release","name": "pelican.framePlanLogicalView", "point": [160, 120], "button": "middle"}
    {"cmd": "drag",   "from": {"name": "pelican.graphCanvasView", "point": [200, 150]},
                      "to":   {"name": "pelican.graphCanvasView", "point": [80, 150]},
                      "button": "middle", "steps": 8}
    {"cmd": "action", "name": "pelican.gizmo.rotate"}
    {"cmd": "action", "text": "Ping", "menu": "More"}
    {"cmd": "menu_list", "pattern": "pelican.file."}
    {"cmd": "view_state", "name": "pelican.graphCanvasView"}
    {"cmd": "key", "name": "pelican.graphCanvasView", "sequence": "Ctrl+T", "route": "window"}

And the two AG-3b added:

    {"cmd": "select", "name": "pelican.outliner", "text": "Camera"}
    {"cmd": "wait",   "name": "pelican.graphAttach", "enabled": true, "timeout_ms": 60000}

`select` picks a row of a list or tree, or an entry of a combo box. For an item
view it is a real click: the row's own rectangle is asked for and the press
lands in it, so the selection model, its signals and everything watching them
run exactly as they do under a finger. For a combo box it cannot be -- the
entries live in a popup and a popup is a modal event loop, which a command that
owes a reply on this line cannot enter, the same reason `action` fires a menu
entry rather than opening the menu -- so it sets the current index, which emits
the combo's own `currentIndexChanged`, the signal a popup choice emits too.

Q-17: `select` on a switched-off list, tree or combo (itself, or the panel it
sits in) is refused with `automation.bad_request@1` and `the widget named "…"
is disabled`, as `click` is. A combo is read back once the panel has heard the
choice: when it then holds another entry -- a panel that refused the choice
puts its value back -- the step fails with `"…" did not take "…"`. A picker
that hands its entry on and goes back to its placeholder ("Pick...", no entry)
took the choice.

QA-1: many combos and lists are filled by an answer of the engine's
(`list_surfaces`, `get_material_bindings`, ...), which comes a few milliseconds
after the row they sit in is on screen, and `wait` has no way to say "until it
holds this entry". So `select` waits, with the event loop running, for a widget
that is not there yet or an entry or row it does not hold yet, up to
`timeout_ms` (5000 by default); the reply's `waited_ms` says how long. When none
comes the step fails with `"…" has no entry …` (a combo) or `"…" holds no row …`
(a list or tree), how long it waited and what the widget held then. A
switched-off widget is not waited for: it is refused at once, as above --
waiting for it to come on would let a choice through at a moment a person could
not have made it. No other command waits like this: what they need (a widget on
screen, switched on) is what `wait` already says, and a scenario says it there.

OL-1 named the rows themselves, for a gesture that starts or ends on one: `<list or tree>::row#<text>` is the row a person reads as `<text>`, among the rows the view shows (a folded branch's rows are not on screen, so they are not found). The view is scrolled to it first; a press lands in the middle of it. `drag` from one row to another is how `outliner_edit_roundtrip` moves an object under another, and `wait` with `missing` is how it sees a row go:

    {"cmd": "drag", "from": {"name": "pelican.outliner::row#Cube_copy1"},
                      "to":   {"name": "pelican.outliner::row#Floor"}, "steps": 8}
    {"cmd": "wait", "name": "pelican.outliner::row#Cube_copy1", "missing": true}

The Outliner's drag is its own press, move and release, not Qt's drag and drop (a `QDrag` runs a modal loop of the desktop's that nothing here can drive): let go over the middle half of a row, the object goes under it; on the quarter nearest a row's edge, on the scene's row or below the last row, it goes to the root; `"modifiers": ["alt"]` keeps its own transform. Duplicate and Delete are the QActions `pelican.outliner.duplicate` and `pelican.outliner.delete` (Ctrl+D and Del while the tree has the keyboard); Delete on an object with children asks first, in `pelican.outliner.deleteConfirm` with `.delete` and `.cancel`.

`enabled` on `wait` is how a scenario waits for a panel to be ready rather than
guessing. A button that is on screen but switched off is the ordinary way a
panel says "not yet": `pelican.graphAttach` is there from the first frame and
only becomes usable once the Inspector has an owner and the attachment model
has a digest. `click` and `double_click` on a switched-off widget (itself, or
the panel it sits in) are refused with `automation.bad_request@1` and
`the widget named "…" is disabled`, the way `action` refuses a disabled
action: Qt drops such a press without a word, so the step that pressed is the
one that fails, not a step later that looks for what the press would have done.
To show that a button is off, `wait` or `list` for `"enabled": false`; never
click it to see nothing happen.

Error codes: `automation.widget_missing@1`, `automation.bad_request@1`,
`automation.unknown_command@1`, `automation.engine_not_ready@1`,
`automation.rpc_failed@1`, `automation.wait_timeout@1`,
`automation.screenshot_failed@1`, `automation.project_failed@1`,
`automation.busy@1`, `automation.command_threw@1`.

Three things worth knowing before writing a scenario:

* `open_project` on the directory that is already open is a no-op -- that is
  what `EmbeddedViewport::openProject` does. To make the engine re-read a
  document from disk, open something else and come back, which is what
  `graph_edit_roundtrip` does. The command waits for the viewport to let go of
  the engine it had before it answers, and its result says
  `engine_restarting`: without that wait, the `wait_engine` after it would be
  answered by the engine that is on its way out.
* `text` on a value pill answers with the stored value the document holds
  (`"2.5"`), not the glyph the canvas draws (`"2.5 · stored"`): the pill files
  the value under a data role, and that is what a scenario asking for a value
  means. A `graph_value_band` (AG-3b, on an *output* row) answers the same way,
  and it is not a pill: a pill is a number the document stores and a click
  opens, a band is a reading of what the published generation produced, and
  clicking one opens nothing.
* `screenshot` in the default `widget` mode renders the Qt widget tree, which
  does not include the embedded engine window (a foreign child window). Use
  `"mode": "screen"` when the picture has to show what the engine drew; that
  mode needs the window to be unobstructed. The reply carries
  `distinct_colors`, the number of different colours in the picture, so a
  `result` step can ask whether the engine drew anything at all (a cleared
  viewport is one colour). `new_blank_project` is the example. With
  `"pixels": [[x, y], ...]` -- each a fraction of the picture's width and
  height -- the reply also carries `pixels`, the colour at each point as
  `"#rrggbb"`, so a `result` step can `remember` one and compare another
  against it: "the same colour down a stripe, a different one across it" is
  how `render_pipeline_from_scratch` tells stripes from a gradient without
  knowing what colour the output transform makes of them.
  MT-1s added `rgb`, the same points as `[r, g, b]` (0-255): `remember`
  `rgb[0][1]` and ask `rgb[0][0]` `gt` it with `plus`, and a step says
  "red above green" without knowing the shade (`gui_material_from_blank`).
* A dialog is filled in the way a panel is: it is shown with `open()`, not
  `exec()`, so its controls are named widgets like any other. File > New
  Project... is `pelican.newProjectDialog`, with `pelican.newProject.location`,
  `.name`, `.template` (a combo: `select` it by the label a person reads),
  `.notice` (why Create was refused) and `.create`.

## Files in the project copy

A third kind of step is done by the driver itself, not sent to Studio: a
`file` step reads or writes one file in the project copy, the way a person
does in an editor of their own between two gestures. The path has to be
inside `{project}` or `{artifacts}` -- the two directories the run owns; a
project a scenario makes with File > New Project lives under `{artifacts}`
(`render_pipeline_from_scratch` is the example). Anything else fails the step.

    {"file": "write",    "path": "{project}/shaders/x.frag", "text": "..."}
    {"file": "replace",  "path": "{project}/shaders/x.frag", "old": "a", "new": "b"}
    {"file": "contains", "path": "{project}/shaders/x.frag", "text": "...", "value": true}
    {"file": "exists",   "path": "{project}/shaders/x.frag", "value": true}

`replace` requires exactly one copy of `old`, so a step cannot quietly edit
the wrong place. `fullscreen_pass_from_template` is the example: it breaks and
repairs the shader the Pass Editor's "New..." wrote. Its fixture is
`test/fixtures/studio_fullscreen_pass_from_template`.

The Pass Editor's F-3 controls are named like the rest:
`pelican.fullscreenPass.newFragmentShader` (New...),
`pelican.fullscreenPass.shaderTemplateName`,
`pelican.fullscreenPass.createFragmentShader`,
`pelican.fullscreenPass.openExistingShader` and
`pelican.fullscreenPass.compileLog` (the log under the form, whose text is
`pelican.shaderCompileLog.text`). A node whose shader failed to reload carries
a drawn badge, `node_shader_error#<pass name>`. F-5: Studio asks the engine to
reload its shaders (`reload_shaders`) 200 ms after a write to a file a drawn
node reads; a refusal marks the node and, while the Pass Editor is open, puts
the log under its form. Under automation Studio does
not hand a shader to the desktop: "opening" one prints
`automation: shader source open <path>` to its log instead.

G-1's "Add material pass..." is the named QAction
`pelican.framePlan.addMaterialPass` (also the toolbar button
`pelican.framePlanAddMaterialPass`). Its form is `pelican.materialPassForm`:
`.name`, `.contract` (a combo: `select` it by the label a person reads),
one `.target.<slot>` combo and one `.state.<slot>` label per output, where the
slot is `color` for a forward contract, `albedo`, `normal`, `material`,
`world_position`, `emissive` for a G-buffer, and `depth`; `.createMissing`,
`.notice`, `.error` and `.ok`. `material_pass_authoring` is the example; its
fixture explains why it carries two material passes nobody reads.

G-5 named what a person answers and types on the way from a blank project to a
material loop. The Inspector's number fields are
`pelican.inspectorVector.<component>.<field>.<column>` (a vector, e.g.
`pelican.inspectorVector.transform.scale.2`) and
`pelican.inspectorNumeric.<component>.<field>` (one number); before G-5 every
vector's first column shared one name. Since Q-7 the Inspector swaps values into
its fields instead of rebuilding them after each commit, and a value typed while
it is busy is sent once it is idle; a scenario still waits for the field to be
enabled, types, presses Return, and waits for the field to hold the value
(`forward_loop_from_scratch` does it in that order), which is what a person
sees. Components with a list are still rebuilt on every commit. The unsaved-scene question is
`pelican.unsavedScenePrompt`, with `.save`, `.discard` and `.cancel`; it is also
asked before a restart the Frame Plan requests (`restart_required`), and Cancel
leaves the running engine alone.

BS-1s: Studio starts every engine with `--render-fallback`, so a project whose
rendering setup the engine refuses at start opens on the engine's one-colour
placeholder instead of a stopped viewport. The row above the viewport is
`pelican.renderFallbackNotice` (hidden while there is nothing to say), its
sentence `.text`, and its buttons `.reload` (Reload from disk), `.openGraph`,
`.openFile`, `.showDocument` and `.restart`. Show the document (a root that is
not JSON, render_document_syntax@1) opens `pelican.renderDocumentDialog`:
`.text` (the bytes on disk, the cursor where the parser stopped), `.notice`,
`.save` (restore_render_documents; off until the text is JSON) and `.cancel`.
Under automation Open file prints `automation:
shader source open <path>` to the log, as a shader does. A project that has to
be broken before Studio opens it is made by the scenario's `<name>.prepare.py`
(with `pelican_cli project init`) in `{artifacts}/incoming/`;
`gui_render_fallback` is the example.

PB-1s: the Problems dock (`pelican.problemsDock`, View > Problems is
`pelican.view.problems`, a tab beside the Engine Log) lists what is wrong
now -- the engine's `get_problems` (engine PB-1, read when
`get_status.problems.revision` moves) and the Assets dock's contents check.
The list is `pelican.problems.list`; its rows are found by their first
column, `row#Error` / `row#Warning`, or `select`ed by `index`, and a click on
one goes where it is fixed (the Graph's node, for a start's refusal and a
refused reload alike, or Show the document at the line). Beside it:
`.severity` (All / Errors / Warnings), `.filter`, `.count` ("1 error"),
`.detail` (the selected row: `<document>:<line>:<column> — <sentence>`) and
`.restart`. The status bar's `pelican.problemsCount` says the same count and
brings the dock forward; the placeholder's row ends "See Problems."
`gui_problems_window` is the example. RR-1s: the same row above the viewport
says when a refused reload's version is what the panels read
(get_graph_instances `live: false`): "Showing the saved version; the picture
is still the previous one — <the list's sentence>", or, while the file is
not JSON, "<file> is not valid JSON; the Graph shows the last version that
was."

MI-1s (engine MI-1): Add Object > Model from file... is the QAction
`pelican.outliner.addObject.modelFromFile`. It opens Qt's own file dialog,
`pelican.modelImportDialog`, whose file field is
`pelican.modelImportDialog.fileName` (type the path, press Return). A file
outside the project is copied into `assets/models` first; it is registered
with `register_model` and an object drawing it, named after the model, is
added as Add Object > Cube adds one. The Inspector's model field has
Import... beside it, `pelican.inspectorModelImport`, which opens the same
dialog and sets the field. A registration that waits for the engine's next
start says so under the Outliner, beside `pelican.outliner.restartEngine`.
The Assets dock's Remove from project... is `pelican.assets.removeModel`
(a registered model's row), and asks in `pelican.assets.removeModelConfirm`
with `.removeModelConfirmed` and `.removeModelCancel`. A file the scenario
needs from outside the project is copied by its `<name>.prepare.py` into
`{artifacts}/incoming/`; `gui_model_from_file` is the example.

FO-1s (engines FO-1 and EB-1): a refused node order is one sentence in
`pelican.frameNodeOrder.notice`, and the two rows it put the wrong way round
are red until a row moves (`gui_graph_order`). In the Materials dock a
document the engine did not load is a row that can be picked: its line is
`pelican.materials.broken`, with `pelican.materials.showDocument` (opens
`pelican.materials.documentDialog` with `.text`, `.notice`, `.save` and
`.cancel`; Save writes the file whole) and `pelican.materials.registerAgain`.
A material's row has the QActions `pelican.materials.delete` (asks in
`pelican.materials.deleteConfirm` with `.deleteConfirmed` / `.deleteCancel`)
and `pelican.materials.rename` (`pelican.materials.rename.name`, `.notice`,
`.ok`, `.cancel`). A refused delete or rename puts a button to each place the
material is used under the notice: `pelican.materials.uses.model#<model>`,
`.object#<name>`, `.document#<path>`. A model's bindings are the row
`pelican.materials.model` (`.heading`, `.notice`, `.choice`, `.close`).
`gui_material_repair` is the example.

MR-1s (engine EB-2): Save in `pelican.materials.documentDialog` has the engine
take the document in at once -- `register_material` for one its start refused,
the engine's own watch (`hot_reload`) for one it refused after start -- and the
window stays open, with the refusal in `.notice`, until the engine has it; the
dock then says `<name> is loaded again.` A document the watch refused after
start is marked on its row ("the file on disk does not load; the engine keeps
the last values") and in `pelican.materials.broken`, at the line. `wait_engine`
answers with `process_id`, the engine process that answered: `remember` it and
compare it later, and a step says no restart happened in between.
`gui_material_repair` is the example.

EB-6s (engine EB-6): a material written that the pass drawing it does not
declare enough for (`applied: "unusable"`) asks no restart: the dock's notice
says why, `pelican.materials.restartEngine` stays hidden, and the document's
row is picked with `pelican.materials.broken` and `pelican.materials.openPass`
(`gui_material_ray_query_from_blank`, bound before the pass is fixed). A
material's editor has `pelican.materials.reloadFromFile`: the engine reads the
document again from its file (`register_material`, `reloaded`), for a project
whose `hot_reload` does not watch materials (`gui_material_repair`). New
material... offers the surface's routings in `pelican.materials.new.routing`.
The Inspector's `pelican.inspectorMaterialMissing` says a bound name the engine
does not draw on Every <model> too ("<name> is not registered -- drawing
<material>"). A Problems row of `material.object_material_unresolved@1` picks
the object in the Outliner and shows its Material rows. A render document put
back outside Studio to the bytes the engine draws, after a refused reload, is
asked again (`gui_problems_window`).

## Gestures a canvas is driven with

`press`, `move` and `release` share one piece of state: which buttons this
surface is holding down. That is what makes a held gesture a gesture -- the
moves between a press and a release carry `buttons()`, which is the only thing a
panning view looks at. `drag` is the three of them in a row, and takes the same
`button`. A `move` with nothing held is a hover: it delivers a mouse move to the
widget, but it does not move the desktop's own cursor, so it does not raise
`enterEvent`, `underMouse()` or a tooltip.

Every pointer gesture -- `click`, `drag`, `wheel`, `move`, `press`, `release` --
is delivered to a scroll area's *viewport*, never to its frame. A
`QAbstractScrollArea` leaves alone a mouse or wheel event that arrives at the
frame, because on a real desktop such an event arrives at the viewport under the
cursor; from there the area's own filter carries it to
`QGraphicsView::mousePressEvent` and `::wheelEvent`, which is where a canvas
puts its rule. Naming the view and naming a drawn item inside it therefore reach
the same receiver, and a gesture aimed at a canvas by the view's own objectName
now lands (before Q-1b it was posted to the frame and silently did nothing).

`view_state` is the reading that answers what a zoom or a pan did: `scale` for
the first, `center` (the scene point in the middle of the viewport) for the
second. `center` answers however the view implements a pan -- scrollbars, a
translated transform, or a changed scene rect -- which is why it, not `scroll`,
is what a scenario should compare.

## A node's parameters

UX-7, decision 8. A scenario writes a graph node's parameter through its value
pill on the canvas, not through the details table: click
`pelican.graphCanvasView::graph_value_pill#<node>/<key>` (a wheel over it first
when the canvas is zoomed out), type into
`pelican.graphCanvasView::graph_inline_editor#<node>/<key>` and press Return.
That is one `set_node_parameters` of that key, the same edit a double-click in
the details table makes; the table's editor is the tree's own and has no name
the channel can reach, so the table is for reading. `gui_build_C`,
`graph_edit_roundtrip` and `graph_observe_roundtrip` write pills this way.

A key the node does not hold yet has no pill. A render node's are written by
its fields (`pelican.graphCanvasPass.*`, `pelican.graphCanvasImage.*`, the Loop
and Draw fields); the parameters render.scalar (`target`), value.constant
(`value`) and value.input (`name`) cannot be placed without are asked for when
the palette places them, in `pelican.graphCanvasRequired` (a field
`pelican.graphCanvasRequired.<parameter>` each, then
`pelican.graphCanvasRequired.ok`).

## Menus and shortcuts

A `QAction` is not a widget: it has no place on screen until a popup opens over
it, and `list` cannot see it. `menu_list` is the listing and `action` is the
gesture, and neither opens a popup -- a popup is a modal event loop, which a
command that owes a reply on this line cannot enter.

Entries are named the way widgets are (`grep setObjectName src/devstudio/view`):
`pelican.file.saveScene`, `pelican.gizmo.rotate`,
`pelican.view.panels.outputDock`, `pelican.framePlanLeaveGroup`. An entry that
opens a submenu carries the submenu's own objectName
(`pelican.layout.toolMenu`) and says `"submenu": true`. `action` also takes the
label a person reads (`"text": "Rotate"`), narrowed by `"menu"` when one label
appears in two menus; two matches with nothing to tell them apart is
`automation.bad_request@1`, and so is an entry the window has switched off.

Whether a *binding* is real is a different question from whether a handler
works, and `key` answers it with `"route": "window"`: keys sent that way go the
desktop's own way, through the shortcut map, which is the only route that can
fire a `QAction` by its shortcut. The default route delivers straight to the
named widget instead -- that is what lets `graph_edit_roundtrip` press Return
into an editor drawn inside a canvas without depending on which window the
machine decided to activate.

## Scenarios that are here but not registered

`pending/` is globbed by nothing: CTest registers `studio_scenarios/*.json` and
stops there. A scenario written against a change that has not landed yet waits
in `pending/` with a note saying what it is waiting for, and is moved up one
directory when that arrives. Nothing is waiting there at the moment.

## What a canvas says about its own placement

`layout_stats` reads what the automatic placement (N-4) achieved on the scene a
named view is showing: how many pairs of edges cross, how many pairs of nodes
overlap, how many nodes were placed, and how many edges a cycle forced to be
drawn backwards. The scene publishes these once per draw -- counting the
crossings is free, because the stage that minimises them counts them anyway --
so a scenario can ask a live window how tangled the drawing is instead of a
reader having to judge a screenshot. `graph_auto_layout` is the example.

The two entries of a canvas's own context menu are named QActions, so `action`
fires them without a popup: `pelican.graphCanvas.autoLayout`,
`pelican.graphCanvas.fitView`, `pelican.framePlan.autoLayout` and
`pelican.framePlan.fitView`.


## What every scenario is held to (QD-1)

Executed findings and the complete 84-case matrix are in [QD1_FINDINGS.md](QD1_FINDINGS.md).
There are 33 ticketed `known_red` checks in 33 scenarios:
33 Undo/Redo findings and 0 health findings. These marks retain
observed failures, including restoration gaps whose root cause is not isolated;
they are not a count of independent product defects. A mark never excuses an
exception or an incomplete history traversal in the checker.

After a scenario's last step passed, the driver checks three things the scenario
does not have to write (`tools/studio_invariants.py`). Each is one more step in
the report, of kind `invariant`, and the report's `invariants` holds what each
found.

1. **`undo`** -- the documents as they were when the engine first answered (after
   the scenario's first `wait_engine`, or again after one that finds another
   project or the same one opened again) come back when every Undo Studio has is
   pressed until no available history moves, and the documents as they were after
   the last step come back when the check's Undo sequence is redone. Studio's Undo is one per kind
   of document, each behind its own control, and the driver presses them by kind:

   | kind | Undo / Redo | what it holds |
   |---|---|---|
   | `graph` | `pelican.graphCanvas.undo` / `.redo` | the Graph tab's edits, and the render edits the tab sent, in the order sent |
   | `render` | `pelican.edit.undoRenderEdit` / `.redoRenderEdit` | the render edits (Frame Plan, Pass Editor, render panels) |
   | `materials` | `pelican.materials.undo` / -- | the Materials dock's writes |
   | `material_row` | `pelican.inspectorMaterialUndo` / -- | a Material row's Every <model> |
   | `assets` | `pelican.assets.undo` / -- | the Assets dock's declarations |
   | `scene` | `pelican.inspectorUndo` / `.inspectorRedo` | the engine's scene journal, the Inspector's session |

   Before walking history, the driver sends Escape to any open Graph inline
   editor and records its name in `closed_graph_drafts`. This cancels only an
   uncommitted text draft: Graph Undo deliberately stays disabled during text
   editing, and clicking a node's label can otherwise leave it disabled through
   sidebar edits. Failure to inspect or close an editor fails the check.
   Undo goes in that order, each kind until its control is off or a press moves
   nothing. A blocked kind is retried after another kind changes documents.
   Redo presses, last first, one Redo per Undo that moved something. Only if no
   Redo was already available at the ordinary endpoint does it drain remaining
   Redo; existing endpoint history must not carry the check past its snapshot.
   The scene journal distinguishes real Redo from always-enabled empty buttons.
   Both traversals are bounded; an incomplete traversal fails even if snapshots
   happen to match. Not compared, and said in the report when it is what
   differs: a scene file on disk (Save is not undone; the scene in memory is
   compared), the scene after the engine was started again (the journal lives in the
   engine), the Frame Plan's placement (`layout.frame_plan`, kept through Undo by
   RU-1, including empty ancestor containers left solely by removing that
   explicitly exempt subtree), and, after an Undo of a kind with no Redo, the Redo half. The documents
   are read with `project_state` (below): every file under the project but
   `.pelican/` and logs by sha256, and the engine's `export_scene_snapshot`,
   `get_render_documents`, `list_materials` and `get_project_settings`.
2. **`reopen`** -- File > Save Scene, then the project closed (another one opened)
   and opened again: the documents are what was saved, and nine points of the
   viewport (a 3 x 3 grid at 0.2 / 0.5 / 0.8) are within 3 of what they were. A
   picture must be stable in two readings half a second apart within 5 s. Animated
   scenarios must explicitly declare `pixels: false` with `pixels_reason`;
   missing pixels or an undeclared unstable picture fail the check.
3. **`health`** -- the engine still answers; Studio's log and the engine's
   (`<project>/.pelican/logs/player.log`) hold no `ERROR`, `LOG_ERROR`, `LOG_FATAL`, assert
   or fatal line that `log_allowlist.json` (here) or the scenario does not allow by
   pattern, with a reason; and Problems holds as many entries as the scenario
   declares (`"problems": N`, default 0) right after its last step. Studio's exit
   code is the `shutdown` step's, as before.

A scenario says what does not fit it, always with a reason:

    "invariants": {
      "undo": false, "undo_reason": "...",           // off
      "reopen": "known_red", "reopen_ticket": "QD1-4", // red for a listed bug
      "pixels": false, "pixels_reason": "...",       // reopen without the picture
      "problems": 1,
      "log_allow": [{"pattern": "<regex>", "reason": "..."}],
      "undo_exempt": [{"path": "file:shaders/*.frag", "reason": "..."},
                      {"path": "render:passes/main.json", "key": "a.b", "reason": "..."}]
    }

`reason` and `ticket` stand for every invariant the scenario turns off or marks.
An `undo_exempt` entry names a document (by its key in the report, a glob) or, with
`key`, a part of a JSON document that an operation writes outside every Undo by
design. **`known_red`** is for an executed finding on the list (the ticket is its
number there): the run stays green and the report and the driver's output say
`KNOWN RED invariant <name> (<ticket>)`; once the check passes, the step says the
mark can come off. `QD1_INVARIANTS=undo,health` in the environment runs only those.

Two commands serve this, and a scenario can use them too:

* `history` -- every Undo / Redo control by kind: `{kinds: {<kind>: {undo: {name,
  found, enabled, visible, text, tooltip}, redo: {...} | null}}}`. With `kind` and
  `step` (`undo` / `redo`) it presses that control the way it is pressed (whether
  or not its panel is on screen); switched off is refused, as `click` refuses it.
* `project_state` -- `{dir, engine_ready, engine_process_id, files: [{path, size,
  sha256}], rpc: {<method>: <answer> | {error}}}`; `brief: true` leaves out the
  files and the engine, `journal: false` leaves out `query_journal`.

A hidden widget still exists. Use `wait` with `missing: true` when an action
removes a widget or a graph item; `visible: false` / `enabled: false` require
the name to resolve and fail on a missing name. Keep a positive check of the
created item or replacement next to a disappearance check.

## 第13章・第10章から移した記述(日本語)

2026-10-06 に manual の第13章 §13.5「Studio の自動化面」と第10章 §10.7 の `--automation` の段落から移した原文です。
上の英語の節と同じことだけを書いた段落は移さず、上の節に無い事実を含む段落だけを、文を変えずに置きました(段落の中には上の節と重なる文も残っています)。
当時の記録を含みます。上の節と食い違うときはコードを確かめてください(例: 上の「Writing a scenario」の QD-1 は、下の GL-5g の段落が「常に通る」と書く形のいくつかを失敗にします)。

### 第13章 §13.5「Studio の自動化面」から

**これは「GUI の動作確認を AI だけで完結させる」ための面です**(Q-1、利用者指示 2026-09-17)。
人が窓を見て確かめる代わりに、シナリオが実物の窓を実物の engine 付きで動かし、AI が結果の report を読んで
「緑 / 赤と理由」を判定します。**人のレビューは使い勝手に使います。**

**flag が無い起動では server object が作られないので、socket が存在しません**
([宣言のコメント](../../src/devstudio/automation/automationserver.hpp#L30)) —— 出荷時の挙動は変わりません。
チャネルは**逐次**で、命令の実行中に次の行が来たら `automation.busy@1` で断ります
([判定](../../src/devstudio/automation/automationserver.cpp#L102))。認証はありません(flag を付けたときだけ開く口です)。

**命令は 23 個**です([一覧](../../src/devstudio/automation/automationcommands.cpp#L2567)):
`list` / `click` / `double_click` / `drag` / `wheel` / `move` / `press` / `release` / `key` / `type` / `text` / `select` /
`action` / `menu_list` / `view_state` / `layout_stats` / `wait` / `wait_engine` / `rpc` /
`open_project` / `screenshot` / `viewport_size` / `quit`。`rpc` は `EmbeddedViewport` 経由で engine へ送り、応答をそのまま返します。
この文書は長く 20 個と書いていましたが、一覧には `select`(AG-3b。下の段落)と `layout_stats`(名前を指した view の scene で自動配置が達成したものを読む。コードのコメントの読み)が既にあり、
✅PX-2(`28bc5ef9`)で `viewport_size`(下の「viewport の大きさを決める(✅PX-2)」)が増えました。
イベントは `QTest` の合成なので、**窓が前面にあるかどうかに依存しません**。拒否は名前付きで、
`automation.widget_missing@1` / `bad_request@1` / `unknown_command@1` / `engine_not_ready@1` / `rpc_failed@1` /
`wait_timeout@1` / `screenshot_failed@1` / `project_failed@1` / `busy@1` / `command_threw@1` の 10 種です
([定数](../../src/devstudio/automation/automationcommands.cpp#L243))。

**`wait_engine` は「今の」engine を待ちます(✅Q-9)。**Frame Plan の保存が engine の再起動を求めると(G-4)、Studio は再起動の
前に古い engine へ未保存の編集を問います(G-5)。その往復の間も古い engine は editor RPC に答えるので、以前の `wait_engine` は
その engine で通り、次の step が止まった engine に当たって `automation.engine_not_ready@1` で落ちていました。今は
**再起動は頼まれた時点で始まったと数え**、そこから新しい engine の process が最初の `get_status` に答えるまで、`wait_engine` は
通りません([wait_engine の判定](../../src/devstudio/automation/automationcommands.cpp#L1981)、
[再起動待ちを立てる所](../../src/devstudio/view/mainwindow.cpp#L2361))。Cancel などで再起動が見送られれば、すぐ今の engine で
通ります。再起動を待っている間に Studio が送る editor RPC は古い engine に届き(未保存の問いと Save はそこへ届く必要が
あるため)、engine が止まった後は既存の 1 行 `The engine viewport is not ready for editor RPC requests.` で断られます。
Stop ボタンや project の切り替えのような普通の停止は、process が終わるまで ready を落としません(`open_project` はその終わりを
待ちます)。
✅MR-1s(`5c26d492`)から、`wait_engine` の答えに **`process_id`**(答えた engine の process の id。`ready`・`waited_ms`・`frames` を付けたときの `published_frames` と並ぶ)が付きます([process_id を足す所](../../src/devstudio/automation/automationcommands.cpp#L2015))。
シナリオは `result` の `remember` で覚え、後の `wait_engine` の `process_id` を `value_of` と `op: eq` で比べて「その間に engine を起こし直していない」を言えます(`gui_material_repair`)。

**`list` の `checked`(✅SK-1、`67fd3d29`)**: `list` の widget の項目は `name` / `class` / `visible` / `enabled` / `geometry` / `text` / `shortcut` を返し、
check できるボタン(`QAbstractButton` で `isCheckable()` が真のもの。`QCheckBox` など)だけ `checked`(真偽)も返します
([`checked` を足す所](../../src/devstudio/automation/automationcommands.cpp#L967))。check box の `text` は caption(例 `Double sided`)なので、
印が付いているかはこちらで読みます(例 `{"cmd": "list", "pattern": "pelican.materials.doubleSided"}` の `widgets[0].checked`)。
check できない widget の項目には `checked` の鍵がありません。test は `devstudio_automation_input_test` の
[「SK-1: list says whether a check box is ticked」](../devstudio_automation_input_test.cpp#L627)(`QCheckBox` の false → true が `checked` に出て、
`QPushButton` の項目には無い)です。なお今の `list` は、名前の付いた `QAction` も `class` が `QAction` の項目(`visible` / `enabled` / `checked` /
`text`)で返します([`QAction` の項目](../../src/devstudio/automation/automationcommands.cpp#L998))。上の「`QAction` は `list` には出ません」は
Q-1b の着地時の記述で、今のコードとは違います(コードの読み)。発火は今も `action` です。

**シナリオは JSON で、`test/studio_scenarios/<名前>.json` に置きます**(書式は
[README](README.md#L61))。step は「命令をそのまま送るもの」か「期待」で、期待は
`rpc_equals`(engine の応答の `path` が `value`)/ `result`(任意の命令を送り、応答の `path` を記録するか `value` と比べる)/
`text_equals` / `visible` / `list_count` / `screenshot`(保存のみ)の 6 種です([判定](../../tools/studio_drive.py#L1031))。`{project}` / `{artifacts}` と `--var NAME=VALUE` は step のすべての文字列に差し込まれます。
`result` の比べ方 `op` は `eq`(既定)/ `ne` / `gt` / `ge` / `lt` / `le` / `contains` / **`excludes`**(✅F-7。文字列がその値を
含まない —— 「その文言は出なかった」の書き方。[`compare()`](../../tools/studio_drive.py#L563))です。`result` も **`retry_ms`**(✅F-5)を受け、
条件を満たすか時間が尽きるまで 0.2 秒おきに問い直します(`rpc_equals` は前から受けていました)。shader を保存した後に
engine が描くものは engine の時計で届くので、`fullscreen_pass_from_template` は `screen` の screenshot をこれで待ちます
([問い直し](../../tools/studio_drive.py#L1098))。
✅PM-3a から、`result` は `value_of` に数を足す **`plus`**(`"value_of": "frame", "plus": 1` は 1 歩後)と、覚えた値から比べる
文を作る **`value_format`**(`"Paused · frame {paused_frame}"`)も受けます([`plus` を足す所](../../tools/studio_drive.py#L1082)。
書式の正は [README](README.md#L142))。
3 つ目の種類の **`file` step**(✅F-3)は Studio へ送らず、driver 自身が run の持つ directory の中の file を 1 つ読み書きします ——
人が Studio の操作の合間に自分のエディタで file を直す、の代わりです。`write` / `replace`(`old` がちょうど 1 か所のときだけ)/
`contains` / `exists` と、✅IA-1s の `copy`(`from` の file を `path` へ byte のまま写して上書きする。`write` は UTF-8 の文字しか書けないので、人が外で焼き直した binary の file を project の file に上書きする代わり。[copy の所](../../tools/studio_drive.py#L909))の 5 つです。書ける範囲は **`{project}`(fixture の複製)と `{artifacts}`(その run の成果物の置き場)の
2 つ**で、File > New Project でシナリオが作った project は `{artifacts}` の下にあるので、そこにも届きます(✅F-4)。
それ以外の path(`..` で外へ出るものを含む)は step の失敗です([file step の範囲の検査](../../tools/studio_drive.py#L869))。`copy` は `from` もこの 2 つの中でなければ失敗です。
driver の stdout / stderr は、cp932 の console で出せない文字があっても止まりません。
`"probe_no_automation": true` を書くと、**先に flag 無しの Studio を起こして socket が無いことを確かめて**から本番を起動します。
最上位の **`"viewport": [w, h] | null`**(✅PX-2)は、driver が最初の step の前に `viewport_size` で固定する engine の viewport の大きさ(画の画素)です。
省くと 720x450、`null` なら固定しません(下の「viewport の大きさを決める(✅PX-2)」)。
最初の失敗で run は止まります。

**シナリオの確認が常に通る形(✅GL-5g、`cfa9859f`)**: シナリオを書く人向けの注意です。driver の比較は、書いた期待が何も確かめていなくても緑になる形があります。GL-5f の実装者が `gui_graph_only_edits_2` の 1 step で気づき、GL-5g の実装者が全シナリオを走査しました(78 本・6,541 step。cmd 4,656 / expect 1,784 / file 101。build に残っていた最新の report の「driver が実際に比べた値」と step の label で突き合わせた。走査の script は repo に入れていません)。本物の常に通る step は 6 本に 6 件あり、全部直しました。常に落ちる step は 0 件でした。直した後に赤になったものはありません(= 隠れていた不具合は無かった)。

| 常に通る形(driver の分岐。実装者が [`compare()`](../../tools/studio_drive.py#L563) と [`equal()`](../../tools/studio_drive.py#L528) などを読んだ結果) | 見つかった step と直し方 |
|---|---|
| `result` の `contains` / `excludes` を **object** に使う。Python の `in` なので object では**鍵**を見る。値の文字列を `excludes` すると常に真 | `gui_graph_only_edits_2` の「the document: no define」(`parameters.shader` に `excludes "PELICAN_PASSTHROUGH_INPUT"`)→ 同じ object に `excludes "defines"`(鍵が無い = define が 1 つも無い) |
| `remember` / `value_of` で、**同じ command・同じ path を時間を進めずに読み直して `eq`**(止めてあれば必ず同じ) | `play_pause_step` の「and no further」→ 間に「engine がその後 30 frame 描く」(`result.frame` に `ge` と `plus`、retry)を入れてから比べる |
| `result` に `value` / `value_of` / `value_format` が無く、`remember` だけで**後で誰も比べない**(path があれば通る) | `graph_reroute_tie`(drag の前に node の y を覚えて `ne`)、`gui_graph_only_edits`(notice の文を `contains "used by render_fullscreen_2"`)、`gui_build_curve`(何も繋がない時の黒と `eq`、retry)、`deferred_loop_from_scratch`(覚えた gradient の色に段 5 の後で `ne`) |

ほかにも次の形は常に通りえます(実装者の表。今のシナリオでは意図どおりに使われていたもの): `ne` で型の違う値を比べる、bool と数を `eq` で比べる(`True == 1` が通る)、`$len` や `distinct_colors` に `ge 0`、名前を書き間違えた `visible: false` / `wait` の `missing`、どの source にも現れない pattern の `list_count 0`、書き間違えた path の file の `contains: false` / `exists: false`。`screenshot` の期待は保存だけで、設計どおり常に通ります。

- **書くときの決まり**: 「無いこと」を言うときは、何が来れば落ちるかを先に考えます。object に `excludes` するなら鍵の名前を、文字列なら部分文字列を書きます。`remember` したら必ずどこかで `value_of` で比べます。「変わらない」を言うときは、間に時間を進める step(retry のある `result`・`wait`・`wait_engine`)を挟みます。開き直した後の絵は「背景と違う」ではなく、保存の前に覚えた点の色と `eq` で比べます(GL-5g が `gui_model_from_file` の 4 点と `render_pipeline_from_scratch` の段 6 の 2 点をこの形にした)。
- **見たが直さなかったもの**(実装者の報告): object の鍵を `contains` / `excludes` する 9 件は期待が鍵の名前で意図どおり(値が来れば落ちる)。比べない読み 80 件は label が「(read)」などの記録で、比較は別の step がする。`eq` の期待が前の操作より前の同じ読みで既に出ていた 86 件は round trip の「戻った」「残った」の確認。**未確認 1 件**: `input_needs_shader` の file の否定(`shaders/present_pass_through.frag` が書かれるとしたらその名前になる根拠が見つからなかった。driver に dir の一覧が無いので「増えていない」とは書けない)。
- **driver を直す案**(実装者の報告。`tools/` は触っていない): object への `contains` / `excludes` を失敗にして鍵には別の op を、比べない `result` で `remember` も無いものを失敗に・使われない `remember` を警告に、`eq` で bool と数を区別、`stable_for_frames` のような「時間が経っても変わらない」の形、`visible: false` は名前が解決しないとき失敗に、file の否定に「前に肯定を見た」を書ける形と dir の一覧、`list_count 0` の pattern が一度も一致しなければ警告、report の summary に比べた step の数。

**回し方**: [`tools/studio_drive.py`](../../tools/studio_drive.py#L1536)(Python 3.11以上、標準ライブラリだけ)が Studio を
`--automation --project --tab` で起動し、✅PX-2 から Studio とつないだ直後・最初の step の前に `viewport_size` で viewport を 720x450 に固定し
([固定する所](../../tools/studio_drive.py#L1647))、step を順に送り、`report.json` を書きます。ctest からは
`studio_scenario_<名前>` として GPU lane で走ります([登録](../CMakeLists.txt#L3673)) —— **実物の窓が要るので
`QT_QPA_PLATFORM=offscreen` は掛けません**。fixture の project は temp に 2 つ複製されるので、シナリオは
文書を書き換えてよく、「ディスクから読み直す」も試せます([複製](../run_studio_scenario.cmake#L30))。✅IA-1s から、シナリオの隣に `<名前>.prepare.py` があれば、ctest の口([prepare の script を走らせる所](../run_studio_scenario.cmake#L68))が driver を起こす前に`<名前>.prepare.py --out <run の directory>/incoming --cli <pelican_cli>` を走らせます。人が Studio を開く前に外で作る file の代わりで、シナリオは `{artifacts}/incoming/` の file を Studio で取り込むか、`file` step の `copy` で project に写すか、project として開きます(今 prepare を持つのは `gui_build_ltc`、✅AB-1s の `gui_build_audio_reactive`、✅HW-2s / AV-1s の `gui_assets_watch_verify`、✅BS-1s の `gui_render_fallback`、✅MI-1s の `gui_model_from_file` の 5 本。[第10章](../../docs/manual/10_tools.md) §10.7)。
CTest登録にはStudio targetと`PELICAN_PYTHON_TESTS=ON`（またはPythonが見つかった`AUTO`）が必要です。
`uv`は不要で、完全なGPU gateは全scenarioの登録・実行を必須にします([CI運用](../../docs/ci.md))。

**run ごとに空の設定 directory を使います(✅F-5)。**Studio は環境変数 **`PELICAN_STUDIO_CONFIG_DIR`** があれば、利用者の
設定 directory の代わりにそこへ dock の layout を読み書きします([置き場の決定](../../src/devstudio/view/mainwindow.cpp#L120))。
driver は run ごとに `{artifacts}/studio_config` を空にして作り、この変数で渡します([渡し方](../../tools/studio_drive.py#L1585))。
以前のシナリオは、前の run(や人が使った Studio)が利用者の設定 directory に残した dock の layout を復元し、自分の
layout をそこに残していました。テーマの選択はこの directory ではなく `QSettings` に残るので、この変数の影響を受けません。

**Studio を置く画面を選べます(✅OT-1)。**環境変数 **`PELICAN_STUDIO_SCREEN`** に画面の番号(`QGuiApplication::screens()` の順、0 から)か
画面の名前(`QScreen::name()`。`\\.\DISPLAY5` など)を書くと、Studio は起動した窓をその画面の使える範囲(`availableGeometry`)の左上へ動かします
([`placeOnRequestedScreen()`](../../src/devstudio/view/studioscreen.cpp#L30)、[呼ぶ所](../../src/devstudio/view/uimain.cpp#L35))。値は先に番号として読み、
範囲内の番号でなければ名前として探します。保存された窓の位置と大きさを戻した後に動かし、大きさは変えず、枠が画面に入らないときだけ縮めます
(最大化していれば、通常の位置を動かしてから最大化し直します)。どの画面にも当たらなければ stderr に 1 行
`PELICAN_STUDIO_SCREEN names no screen: <値>; screens: <名前, …>` を出し、窓は動かしません。未設定なら何もしません。
ctest の run(`cmake -E env`)と driver(`dict(os.environ)`)は Studio に環境をそのまま渡すので、`PELICAN_STUDIO_SCREEN=1 ctest -L studio_scenario` で
全部のシナリオを 2 枚目の(仮想の)モニタで走らせ、人が使っている画面を空けておけます。OT-1 の着地時に統合側が、100 % の主モニタの右に置いた
125 % の仮想モニタで 40 本全部を緑で通しました。

**シナリオは無音です(✅Q-11)。**AU-3 から player は audio の backend を起動時に選べます(`--audio-backend device|offline|null` と
環境変数 `PELICAN_AUDIO_BACKEND`。`null` は device を開きません)。Studio の QProcess は環境を継承するので、driver は Studio を起動する環境に
`PELICAN_AUDIO_BACKEND=null` を入れます([無音にする所](../../tools/studio_drive.py#L1576))。呼び出し側が既に値を設定していれば
(`device` / `offline` など)その値を尊重し、**空文字は未設定として扱って** `null` に置き換えます(player と同じ扱い)。シナリオ
`graph_read` は `get_status` の `audio.backend` が `null` で、`audio` に `fallback_reason` が無い(鍵が `available`・`backend`・`voices` の 3 つだけ)ことを確かめます。
engine の ✅CT-1(`ac8d836b`)から `audio` には `voices` が常に付くので(audio を外した build を除く。[第8章](../../docs/manual/08_gameplay.md) の「`get_status` の `audio`」)、`9ca2d3ce` でシナリオの `audio.$len` の期待を 2 から 3 にしました。この文書ではシナリオを走らせて確かめていません(未確認)。
Q-10 の時点では、埋め込んだ player は窓があるので実物の device を選び、シナリオの間も speaker から音が出ていました。

**report.json は AI が読む形です**([組み立て](../../tools/studio_drive.py#L1737)): `verdict`(`green` / `red`)と
`ok` / `failed_steps`、`summary`(step ごとに 1 行)、`failures`(送った命令・返ってきた応答・言葉での理由・
**失敗した瞬間のスクリーンショット**)、`steps`(全文)、`studio_log`(Studio と engine の console 出力)。
`studio_exit_code` と `studio_shutdown_forced` も記録します。✅PX-2 から `steps` には `viewport` の step(`the viewport is held at 720x450`)が入り、`steps_planned` もそれを数えます(`"viewport": null` のシナリオを除く)。最後の shutdown step は
**exit 0、強制終了なし**を要求し、異常終了・タイムアウトによる強制停止も report と driver の失敗になります。

**Play の間の `quit`(✅UX-7)**: 自動化面の `quit` は、閉じる前に MainWindow の [`stopPlayThen()`](../../src/devstudio/view/mainwindow.cpp#L2130) を通ります([渡す所](../../src/devstudio/view/uimain.cpp#L49))。Play の帯が Playing か Paused(か run がある)と言う間は、帯が送れる時に keep なしの Stop を送り(2 秒ごとに送り直します)、run が終わってから閉じます。15 秒たっても終わらなければ、待つのをやめて閉じる処理へ進みます(Stop に engine が答えないときに止まらないため。そのときに確認のダイアログが出るかは確かめていません。未確認)。UX-7 の前は、Play の間の `quit` が窓の close の「再生を終える確認」のダイアログで止まり、30 秒で強制終了になっていました(`gui_build_ocean` の間欠の失敗)。人が窓を閉じるときは今も確認が出ます。

**`get_status` の返事は 1 回だけ parse します(✅UX-7)**: viewport は、誰が送った `get_status` の返事でも、生の bytes の signal `engineStatusRead` の後に、1 回 parse した JSON を `engineStatusParsed(request_id, status)` で渡します([渡す所](../../src/devstudio/viewport/embeddedviewport.cpp#L834))。Play の帯・Outliner の runtime の行・`wait_engine`([`render.published_frames` を読む所](../../src/devstudio/automation/automationcommands.cpp#L1907))はこれを読みます。UX-7 の前はそれぞれが GUI の thread で parse していて、ループを 16 回展開した graph で約 2 MB の返事が GUI を止めていました(仕様の背景。engine の ✅ST-1 で返事自体も小さくなりました)。UX-6b の `PlanPerfHub` は生の bytes を受けて thread pool で parse します。

### 第10章 §10.7「`pelican_studio` の CLI」の `--automation` から

`--automation <名前>` は **GUI を外から駆動するための口**です(Q-1)。指定すると `QLocalServer`(Windows では
`\\.\pipe\<名前>`)が開き、**改行区切りの JSON 1 行 = 1 命令**で `list` / `click` / `double_click` / `drag` /
`wheel` / `move` / `press` / `release` / `key` / `type` / `text` / `select` / `action` / `menu_list` / `view_state` /
`layout_stats` / `wait` / `wait_engine` / `rpc` / `open_project` / `screenshot` / `viewport_size` / `quit` の **23 命令**を受けます
([一覧](../../src/devstudio/automation/automationcommands.cpp#L2567))。この節は長く 20 命令と書いていましたが、一覧には `select`(AG-3b)と
`layout_stats` が既にあり、✅PX-2(`28bc5ef9`)で `viewport_size` が増えました。**flag が無い起動では server object が
作られないので socket は存在しません**([構築](../../src/devstudio/view/uimain.cpp#L41)) —— 出荷時の挙動は
変わりません。listen できたときだけ `automation listening: <endpoint>` を標準出力に出します
([その 1 行](../../src/devstudio/view/uimain.cpp#L71))。開けなかったときは理由を出して **終了コード 3** です。

`wait_engine` は engine が editor RPC に答えた時点で返ります。engine は**最初の frame を出す前に** RPC に
答えるので、描画済みの frame が要る操作(`capture`、公開済みの frame plan)の前には `frames` を付けます。
`{"cmd": "wait_engine", "frames": 1}` は `get_status` の `render.published_frames` がその数に届くまで待ち、
応答に `published_frames` を足します([commandWaitEngine()](../../src/devstudio/automation/automationcommands.cpp#L1965))。
`published_frames` は renderer がその起動で公開した logical frame の数で、飛ばした frame は数えません。
✅MR-1s(`5c26d492`)から、答えに `process_id`(答えた engine の process の id)も付きます。シナリオは覚えておいて後で比べ、その間に engine が起こし直されていないことを言えます([第13章](../../docs/manual/13_editor.md) §13.5 の「Studio の自動化面」)。

**viewport の大きさ(✅PX-2、`28bc5ef9`)**: driver は Studio とつないだ直後・最初の step の前に
`{"cmd": "viewport_size", "width": 720, "height": 450}` を送り、engine の viewport(`pelican.viewportHost`)を画の画素で 720x450 に固定します
([`fix_viewport()`](../../tools/studio_drive.py#L1427)、既定の [`DEFAULT_VIEWPORT`](../../tools/studio_drive.py#L59))。
Studio は viewport に固定の大きさをかけ、窓を差の分だけ伸び縮みさせ、ドックの大きさはそのままにします。後でドックを開け閉めしても viewport の大きさは変わりません
([`commandViewportSize()`](../../src/devstudio/automation/automationcommands.cpp#L2053)。`{"free": true}` で外します)。
シナリオの最上位に `"viewport": [w, h]` と書けばその大きさ、`"viewport": null` なら固定しません(今は `whole_frame_first_steps` だけ)。
答えの大きさが要求から 1 画素を超えてずれるか断られたら、その run は step を送らずに赤です。画素を読むシナリオの点は 720x450 の絵で選んであるので、
**既定の大きさを変えると、画素を読む 33 本の点を全部測り直すことになります**。理由・`screen` の screenshot が 719x449 になる件・点の測り直しは
[第13章](../../docs/manual/13_editor.md) §13.5「Studio の自動化面」の「viewport の大きさを決める(✅PX-2)」です。

✅IA-1s で、シナリオが Studio の外の作業を代わりにする口が 2 つ増えました。

- **`file` step の `copy`**: `{"file": "copy", "path": "{project}/x", "from": "{artifacts}/y"}` は `from` の file を `path` へ byte のまま写します(あれば上書きします。[file step の copy](../../tools/studio_drive.py#L909))。`write` は UTF-8 の文字しか書けないので、人が外で焼き直した binary の file を project の file に上書きする、の代わりです。`path` と `from` のどちらも run の持つ 2 つの directory(`{project}` と `{artifacts}`)の中でなければ step の失敗です(ほかの `file` step と同じ範囲。[第13章](../../docs/manual/13_editor.md) §13.5「Studio の自動化面」)。
- **`<名前>.prepare.py`**: `test/studio_scenarios/<名前>.json` の隣に `<名前>.prepare.py` があると、ctest の `run_studio_scenario.cmake`([prepare の script を走らせる cmake の口](../run_studio_scenario.cmake#L68))は driver を起こす前に `<python> -B <名前>.prepare.py --out <run の directory>/incoming --cli <pelican_cli>` を走らせます(120 秒で打ち切り)。人が Studio を開く前に外で作る file(自分の script で書いて CLI で焼いた資産など)の代わりで、シナリオは `{artifacts}/incoming/` の file を Studio の Import で取り込みます(`{artifacts}` がその run の directory です)。`pelican_cli` は ctest の登録が target から `-DCLI=` で渡し([prepare に CLI を渡す登録](../CMakeLists.txt#L3706))、target が無いか file が無ければ、prepare を持つシナリオは失敗です。prepare が 0 以外で終わっても、その stderr を付けて失敗です。`tools/studio_drive.py` を直接回すときは prepare は走りません(ctest の cmake の口のため)。
  - 今 prepare を持つのは 5 本です: `gui_build_ltc`、✅AB-1s の `gui_build_audio_reactive`(音量が時間で変わる 1 秒の小さな WAV 2 つを書く。`--cli` は受けるが使わない)、
    ✅HW-2s / AV-1s の `gui_assets_watch_verify`(`pelican_cli image ktx2 --raw` で焼いた 4×4 の texture を書き、シナリオが `copy` で project に写す)、
    ✅BS-1s の `gui_render_fallback`(`pelican_cli project init --template blank` の project の root を壊して書き、シナリオがそれを開く)、
    ✅MI-1s の `gui_model_from_file`(repo の `test/fixtures/studio_material_pass_authoring/checker_box.glb` を写す。`--cli` は受けるが使わない)。[`gui_build_ltc.prepare.py`](gui_build_ltc.prepare.py) は LTC の表 2 枚と焼き直した 1 枚を raw の float で書き、`pelican_cli image ktx2 --format R32G32B32A32_SFLOAT --raw … --width 64 --height 64`(§10.4)で `.ktx2` にします。
  - 書式の正の [README](README.md) には、`copy` はまだ書かれていません(IA-1s の commit は README を変えていません)。prepare は、BS-1s と MI-1s の段落が
    `<name>.prepare.py` で `{artifacts}/incoming/` に置く例として触れるだけで、書式の節はありません。
