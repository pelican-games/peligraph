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
* `op` is `eq` (the default), `ne`, `gt`, `ge`, `lt`, `le` or `contains`.
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

`enabled` on `wait` is how a scenario waits for a panel to be ready rather than
guessing. A button that is on screen but switched off is the ordinary way a
panel says "not yet": `pelican.graphAttach` is there from the first frame and
only becomes usable once the Inspector has an owner and the attachment model
has a digest. A scenario that clicks it early does nothing and fails three
steps later, naming the wrong thing.

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
a drawn badge, `node_shader_error#<pass name>`. Under automation Studio does
not hand a shader to the desktop: "opening" one prints
`automation: shader source open <path>` to its log instead.

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
