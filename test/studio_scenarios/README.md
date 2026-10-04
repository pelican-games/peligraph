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
      "steps": [ ... ]
    }

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
  `remember`. With neither, the step only reads (and remembers), and passes.
* `op` is `eq` (the default), `ne`, `gt`, `ge`, `lt`, `le`, `contains` or `excludes` (the text does not hold the value -- how a scenario says a message never appeared).
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
| `select`       | `name`, and `text` (the row or entry as a person reads it) or `index` |
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
