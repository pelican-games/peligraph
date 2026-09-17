# Studio scenarios

One JSON file here is one end-to-end check of the Studio GUI: a real window, the
real engine embedded in it, and a driver that clicks and types the way a person
would. Each file becomes one CTest test, `studio_scenario_<name>`, in the GPU
lane.

    ctest --test-dir build -C Debug -R studio_scenario_graph_read -j1 --output-on-failure

The test copies `test/fixtures/studio_automation_graph` to
`build/test_artifacts/studio_scenarios/<name>/project` (and a second, untouched
copy beside it) and runs `tools/studio_drive.py` against the copy. Everything
the run produced is in that directory: `report.json`, `studio.log`, and the
screenshots.

To run one by hand, without CTest:

    uv run tools/studio_drive.py \
      --studio dist_debug/pelican_studio.exe \
      --player build/src/player/Debug/pelican_player.exe \
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

`studio_exit_code` is recorded but is *not* part of the verdict: Studio is known
to leave a non-zero code on the way out (backlog), and the report says so in
`studio_exit_code_note` so a reader who has only the file is not left guessing.

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
| `screenshot`  | `name?`, `path?`, `mode?`          | always -- it only saves the picture           |

`path` walks the JSON reply: `nodes[1].parameters.gain`, and `$len` for the
length of a list or object (`definitions.$len`).

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

The editor the canvas opens over a pill or a heading is a real widget carried by
a drawn item, and is named the same way:
`pelican.graphCanvasView::graph_inline_editor#left/gain`. `type`, `key` and
`text` reach through the item to the widget, so they do not depend on which
window the desktop happens to have activated.

| command        | fields                                                       |
|----------------|--------------------------------------------------------------|
| `list`         | `pattern?` (case-insensitive substring of the name)           |
| `click`        | `name` \| `point`, plus `scene?`, `point?`, `button?`, `modifiers?` |
| `double_click` | the same                                                      |
| `drag`         | `from`, `to` (a name, or `{name, point?/scene?}`), `modifiers?`, `steps?` |
| `key`          | `sequence` (`"Return"`, `"Ctrl+A"`), `name?`                  |
| `type`         | `text`, `name?`, `select_all?` (default true)                 |
| `text`         | `name`                                                        |
| `wait`         | `name`, one of `visible` / `text` / `contains` / `missing`, `timeout_ms?` |
| `wait_engine`  | `timeout_ms?` -- until the viewport answers editor RPC         |
| `rpc`          | `method`, `params?`, `timeout_ms?` -- straight to the engine   |
| `open_project` | `dir`                                                         |
| `screenshot`   | `path`, `name?`, `mode?` (`widget` \| `screen`)               |
| `quit`         | --                                                            |

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
  means.
* `screenshot` in the default `widget` mode renders the Qt widget tree, which
  does not include the embedded engine window (a foreign child window). Use
  `"mode": "screen"` when the picture has to show what the engine drew; that
  mode needs the window to be unobstructed.
