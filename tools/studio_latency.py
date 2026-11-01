#!/usr/bin/env python3
"""Measure real Studio operations through the scenario driver, stage by stage.

10-02 measured seven operations on the two-node fixture from the driver's side
only. LT-1 extends it: Graph-canvas operations (select, paced drag, multi
select, connect / disconnect, add / delete, inline value edit, undo / redo,
fold, definitions, zoom / pan / fit, tab switch, project open), graph sizes,
a weak-PC simulation (CPU affinity), the docks' size floors and a viewport
resize -- and, for every operation sample, the stages in between, from two
traces that are off unless asked for:

* Studio: PELICAN_STUDIO_OP_TRACE=1 installs the GuiTrace sink (optrace.hpp);
  the automation command `op_trace` writes what it kept.
* Engine: PELICAN_RPC_TRACE=<file> appends one line per RPC handled at a frame
  boundary (received / boundary / handler / written) and the editor's stages.

Both stamp steady_clock nanoseconds (QueryPerformanceCounter on Windows), the
clock the driver's perf_counter reads, so the three line up on one axis and an
RPC pairs with its Studio send/receive by id.

Example (build pelican_studio, pelican_player and pelican_gpu_lease_run):
  uv run tools/studio_latency.py --build-dir build --qt-bin <Qt>/bin --runs 3

Each run is a new directory under --out-root (never overwritten); each section
is one Studio launch, so one failing step costs its section only. Request-to-
verified-state, not display latency; no pixel comparison.
"""
from __future__ import annotations

import argparse
import collections
import ctypes
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import shutil
import statistics
import subprocess
import sys
import time
from typing import Any

CANVAS = "pelican.graphCanvasView"
C = CANVAS + "::"
NOTICE = "pelican.graphCanvasNotice"
SAVED = "Graph change saved."

# ---------------------------------------------------------------------------
# Scenario building


class Plan:
    def __init__(self, name: str, samples: int, warmup: int) -> None:
        self.name = name
        self.samples = samples
        self.warmup = warmup
        self.steps: list[dict] = []
        self.dumps = 0
        self.serials = 0

    @property
    def count(self) -> int:
        return self.samples + self.warmup

    def add(self, label: str, **command) -> None:
        self.steps.append({"label": label, **command})

    def raw(self, label: str, step: dict) -> None:
        self.steps.append({"label": label, **step})

    def op(self, operation: str, sample: int, commands: list[dict]) -> None:
        for phase, command in enumerate(commands):
            self.raw(f"latency/{operation}/{sample}/{phase}", command)

    def gate(self, operation: str, sample: int, commands: list[dict]) -> None:
        for phase, command in enumerate(commands):
            self.raw(f"gate/{operation}/{sample}/{phase}", command)

    def dump(self) -> None:
        self.dumps += 1
        self.add(f"optrace/{self.dumps}", cmd="op_trace",
                 path="{artifacts}/optrace_%03d.jsonl" % self.dumps, clear=True)

    def serial_before(self, operation: str, sample: int) -> str:
        self.serials += 1
        name = f"serial_{self.serials}"
        self.add(f"pre/{operation}/{sample}", expect="result", command={"cmd": "history"},
                 path="kinds.graph.undo.graph_serial", remember=name)
        return name

    def serial_after(self, operation: str, sample: int, name: str) -> list[dict]:
        # The flow gate: the undo entry changed, so the edit committed. Its
        # 200 ms retry clock is outside the measured group; the commit time
        # itself comes from the trace (graph_edit_committed).
        return [{"expect": "result", "command": {"cmd": "history"},
                 "path": "kinds.graph.undo.graph_serial", "op": "ne", "value_of": name,
                 "retry_ms": 60000},
                wait("pelican.graphCanvasAddNode", enabled=True)]

    def scenario(self, tab: str | None = "graph") -> dict:
        body = {"schema": "pelican.studio_scenario", "version": 1, "name": self.name,
                "probe_no_automation": False, "steps": self.steps,
                # A section makes up to a hundred edits to time them; undoing
                # them all at the end (and reopening) is not what it measures,
                # and the 11-01 small_edit went red there after every sample
                # had been taken. Health (Problems, logs) stays on.
                "invariants": {"undo": False, "reopen": False,
                               "reason": "a latency measurement, not an edit scenario"}}
        if tab:
            body["tab"] = tab
        return body


def wait(name: str, timeout_ms: int = 60000, **condition) -> dict:
    return {"cmd": "wait", "name": name, "timeout_ms": timeout_ms, **condition}


def setup_graph(p: Plan, first_node: str, show_value: bool = True) -> None:
    p.add("startup: engine has presented 60 frames", cmd="wait_engine", frames=60, timeout_ms=180000)
    p.add("setup: bring graph forward", cmd="action", name="pelican.view.renderTools.graph")
    p.add("setup: graph can edit", **wait("pelican.graphCanvasAddNode", 120000, enabled=True))
    if show_value:
        p.add("setup: show value nodes", cmd="click", name="pelican.graphCanvasShow.value")
    p.add("setup: graph ready", **wait(C + f"graph_node#{first_node}", 120000, visible=True))
    p.add("setup: fit graph", cmd="action", name="pelican.graphCanvas.fitView")
    p.add("record: layout_stats", expect="result",
          command={"cmd": "layout_stats", "name": CANVAS}, path="nodes", record=True)
    p.add("record: view_state", expect="result",
          command={"cmd": "view_state", "name": CANVAS}, path="items", record=True)


def ops_baseline(p: Plan) -> None:
    p.add("setup: outliner ready", **wait("pelican.outliner", visible=True))
    for i in range(p.count):
        p.op("automation_text_baseline", i, [{"cmd": "text", "name": NOTICE}])
        p.op("engine_status_roundtrip", i, [{"cmd": "rpc", "method": "get_status", "params": {}}])
        name, z = ("Camera", "5.0000") if i % 2 == 0 else ("Anchor", "0.0000")
        p.op("select_object_inspector_ready", i, [
            {"cmd": "select", "name": "pelican.outliner", "text": name},
            wait("pelican.inspectorSelection", contains=name + "\n"),
            wait("pelican.inspectorVector.transform.pos.2", text=z),
            wait("pelican.inspectorRefresh", enabled=True),
        ])
    p.dump()


def ops_select(p: Plan, nodes: tuple[str, str]) -> None:
    # nodes[0] is an instance (its details have a `definition` row), nodes[1]
    # is not: the row appearing / going is the details of the new selection.
    for i in range(p.count):
        node = nodes[i % 2]
        done = (wait("pelican.graphCanvasDetails::row#definition", 30000, visible=True) if i % 2 == 0
                else wait("pelican.graphCanvasDetails::row#definition", 30000, missing=True))
        p.op("graph_select_node", i, [{"cmd": "click", "name": C + f"graph_node_header#{node}"}, done])
    p.dump()


def ops_view(p: Plan) -> None:
    p.add("setup: fit before zoom", cmd="action", name="pelican.graphCanvas.fitView")
    for i in range(p.count):
        p.add(f"pre/graph_zoom/{i}", expect="result", command={"cmd": "view_state", "name": CANVAS},
              path="scale", remember=f"zoom_{i}")
        p.op("graph_zoom", i, [
            {"cmd": "wheel", "name": CANVAS, "point": [120, 100], "delta": 120 if i % 2 == 0 else -120},
            {"expect": "result", "command": {"cmd": "view_state", "name": CANVAS},
             "path": "scale", "op": "ne", "value_of": f"zoom_{i}"},
        ])
    p.dump()
    for i in range(p.count):
        p.add(f"pre/graph_pan/{i}", expect="result", command={"cmd": "view_state", "name": CANVAS},
              path="center[0]", remember=f"pan_{i}")
        a, b = ([200, 150], [80, 150]) if i % 2 == 0 else ([80, 150], [200, 150])
        p.op("graph_pan", i, [
            {"cmd": "drag", "from": {"name": CANVAS, "point": a}, "to": {"name": CANVAS, "point": b},
             "button": "middle", "steps": 8},
            {"expect": "result", "command": {"cmd": "view_state", "name": CANVAS},
             "path": "center[0]", "op": "ne", "value_of": f"pan_{i}"},
        ])
    p.dump()
    for i in range(p.count):
        p.add(f"pre/graph_fit/{i}", cmd="wheel", name=CANVAS, point=[120, 100], delta=360)
        p.add(f"pre/graph_fit/{i}/read", expect="result", command={"cmd": "view_state", "name": CANVAS},
              path="scale", remember=f"fit_{i}")
        p.op("graph_fit", i, [
            {"cmd": "action", "name": "pelican.graphCanvas.fitView"},
            {"expect": "result", "command": {"cmd": "view_state", "name": CANVAS},
             "path": "scale", "op": "ne", "value_of": f"fit_{i}"},
        ])
    p.dump()


def ops_pill(p: Plan) -> None:
    pill = C + "graph_value_pill#left/gain"
    editor = C + "graph_inline_editor#left/gain"
    p.add("setup: fit before editing", cmd="action", name="pelican.graphCanvas.fitView")
    for i in range(p.count):
        value = 3.75 if i % 2 == 0 else 2.5
        serial = p.serial_before("graph_gain_commit", i)
        p.op("graph_inline_editor_open", i, [{"cmd": "click", "name": pill}, wait(editor, visible=True)])
        p.add(f"setup: type gain {i}", cmd="type", name=editor, text=str(value))
        p.op("graph_gain_commit", i, [
            {"cmd": "key", "name": editor, "sequence": "Return"},
            wait(editor, missing=True),
            {"expect": "rpc_equals", "method": "get_graph_instances", "params": {},
             "path": "nodes[1].parameters.gain", "value": value, "retry_ms": 60000},
            wait(pill, text=str(value)),
        ])
        p.gate("graph_gain_commit", i, p.serial_after("graph_gain_commit", i, serial))
    p.dump()


def ops_drag(p: Plan, node: str | None, intervals=(8, 16), pick: str | None = None,
             duration_ms: int = 1000, onward: bool = False, level: bool = False) -> None:
    for interval in intervals:
        operation = f"graph_drag_{interval}ms"
        for i in range(p.count):
            # drag_paced centres the node and refuses a path that leaves the
            # canvas. A large node (a loop zone, a pose definition) sits with
            # its grip near the left edge once centred, so there the node
            # walks right (onward) instead of going back and forth.
            if onward:
                delta = [100, 20] if i % 2 == 0 else [100, 0]
            elif level:
                # A grip near the top edge once centred (posegraph's cycle,
                # y = 19): back and forth on one line. Walking onward ran the
                # node into a folded one after ten samples, whose place is
                # never written, so the gate saw no edit.
                delta = [120, 0] if i % 2 == 0 else [-120, 0]
            else:
                delta = [120, 40] if i % 2 == 0 else [-120, -40]
            serial = p.serial_before(operation, i)
            command = {"cmd": "drag_paced", "name": CANVAS, "delta": delta,
                       "duration_ms": duration_ms, "interval_ms": interval, "hold_ms": 0}
            if pick:
                command["pick"] = pick
            else:
                command["node"] = node
            p.op(operation, i, [command])
            p.gate(operation, i, p.serial_after(operation, i, serial))
            p.add(f"dragtrace/{operation}/{i}", cmd="trace_dump", name=CANVAS, records=False)
        p.dump()


def ops_connect(p: Plan) -> None:
    edge = C + "graph_edge#clock->left"
    for i in range(p.count):
        serial = p.serial_before("graph_connect", i)
        p.op("graph_connect", i, [
            {"cmd": "drag", "from": {"name": C + "graph_socket#clock/out:value"},
             "to": {"name": C + "graph_socket#left/in:gain"}, "steps": 8},
            wait(edge, visible=True),
        ])
        p.gate("graph_connect", i, p.serial_after("graph_connect", i, serial))
        serial = p.serial_before("graph_disconnect", i)
        p.op("graph_disconnect", i, [
            {"cmd": "drag", "from": {"name": C + "graph_socket#left/in:gain"},
             "to": {"name": C + "graph_socket#left/in:gain", "offset": [-40, 0]}, "steps": 8},
            wait(edge, missing=True),
        ])
        p.gate("graph_disconnect", i, p.serial_after("graph_disconnect", i, serial))
    p.dump()


def ops_add_delete(p: Plan) -> None:
    node = C + "graph_node#render_fullscreen"
    # "Add fullscreen pass" adds two nodes: the pass and the image it writes
    # (render_fullscreen__color). Deleting only the pass leaves the image, and
    # the next add is refused as graph.node_duplicate@1 (the 11-01 run's red).
    # The image cannot go first (graph.output_referenced@1 while the pass
    # writes it), and a Ctrl+click does not add it to the pick; so the pass's
    # Delete is measured, and the image is deleted after it, outside the
    # sample, as a user would clear it.
    image = C + "graph_node#render_fullscreen__color"
    for i in range(p.count):
        serial = p.serial_before("graph_add_node", i)
        p.op("graph_add_node", i, [
            {"cmd": "action", "name": "pelican.graphCanvas.addFullscreenPass"},
            wait(node, visible=True),
        ])
        p.gate("graph_add_node", i, p.serial_after("graph_add_node", i, serial))
        p.add(f"setup: fit {i}", cmd="action", name="pelican.graphCanvas.fitView")
        p.add(f"setup: pick the added node {i}", cmd="click", name=C + "graph_node_header#render_fullscreen")
        serial = p.serial_before("graph_delete_node", i)
        p.op("graph_delete_node", i, [
            {"cmd": "key", "name": CANVAS, "sequence": "Delete"},
            wait(node, missing=True),
        ])
        p.gate("graph_delete_node", i, p.serial_after("graph_delete_node", i, serial))
        p.add(f"setup: pick its image {i}", cmd="click", name=C + "graph_node_header#render_fullscreen__color")
        serial = p.serial_before("image_cleanup", i)
        p.add(f"setup: delete its image {i}", cmd="key", name=CANVAS, sequence="Delete")
        p.add(f"setup: image gone {i}", **wait(image, 60000, missing=True))
        p.gate("image_cleanup", i, p.serial_after("image_cleanup", i, serial))
    p.dump()


def ops_undo_redo(p: Plan) -> None:
    pill = C + "graph_value_pill#left/gain"
    editor = C + "graph_inline_editor#left/gain"
    # Two known values, so Undo's end is a Studio-side wait (25 ms poll) for
    # the first one rather than a driver-side "not 7.25" re-asked every 200 ms
    # (which quantised the 11-01 run's graph_undo to ~220 ms).
    for k, value in enumerate(("1.125", "7.25")):
        serial = p.serial_before("undo_setup", k)
        p.add(f"setup: open gain {k}", cmd="click", name=pill)
        p.add(f"setup: editor {k}", **wait(editor, visible=True))
        p.add(f"setup: type {k}", cmd="type", name=editor, text=value)
        p.add(f"setup: commit {k}", cmd="key", name=editor, sequence="Return")
        p.gate("undo_setup", k, p.serial_after("undo_setup", k, serial))
        p.add(f"setup: pill shows {value}", **wait(pill, text=value))
    for i in range(p.count):
        serial = p.serial_before("graph_undo", i)
        p.op("graph_undo", i, [
            {"cmd": "action", "name": "pelican.graphCanvas.undo"},
            wait(pill, text="1.125"),
        ])
        p.gate("graph_undo", i, p.serial_after("graph_undo", i, serial))
        serial = p.serial_before("graph_redo", i)
        p.op("graph_redo", i, [
            {"cmd": "action", "name": "pelican.graphCanvas.redo"},
            wait(pill, text="7.25"),
        ])
        p.gate("graph_redo", i, p.serial_after("graph_redo", i, serial))
    p.dump()


def ops_definition(p: Plan, node: str, definition: str, back_node: str) -> None:
    for i in range(p.count):
        p.op("graph_enter_definition", i, [
            {"cmd": "double_click", "name": C + f"graph_node#{node}"},
            wait("pelican.graphCanvasBreadcrumb", 30000, text=f"root / {definition}"),
        ])
        p.op("graph_leave_definition", i, [
            {"cmd": "select", "name": "pelican.graphCanvasScope", "text": "root"},
            wait(C + f"graph_node#{back_node}", 30000, visible=True),
        ])
    p.dump()


def ops_loop_body(p: Plan) -> None:
    for i in range(p.count):
        p.add(f"setup: pick loop {i}", cmd="click", name=C + "graph_node#loop")
        p.op("graph_enter_loop_body", i, [
            {"cmd": "click", "name": "pelican.graphCanvasLoop.openBody"},
            wait("pelican.graphCanvasBreadcrumb", 30000, text="root / loop_body"),
        ])
        p.op("graph_leave_loop_body", i, [
            {"cmd": "select", "name": "pelican.graphCanvasScope", "text": "root"},
            wait(C + "graph_node#loop", 30000, visible=True),
        ])
    p.dump()


def ops_multi(p: Plan, nodes: list[str]) -> None:
    for i in range(p.count):
        p.add(f"setup: fit {i}", cmd="action", name="pelican.graphCanvas.fitView")
        p.op("graph_multi_select_ctrl_click", i, [
            {"cmd": "click", "name": C + f"graph_node_header#{nodes[0]}"},
            *[{"cmd": "click", "name": C + f"graph_node_header#{n}", "modifiers": ["ctrl"]}
              for n in nodes[1:]],
        ])
        serial = p.serial_before("graph_multi_drag", i)
        p.add(f"pre/graph_multi_drag/{i}/y", expect="result",
              command={"cmd": "list", "pattern": C + f"graph_node#{nodes[-1]}"},
              path="widgets[0].scene[1]", remember=f"multi_y_{i}")
        dy = 40 if i % 2 == 0 else -40
        # A press on a node that is part of the selection keeps the selection:
        # every selected node moves with it (and DL-1's trace records the drag).
        p.op("graph_multi_drag", i, [
            {"cmd": "drag_paced", "name": CANVAS, "node": nodes[0], "delta": [60, dy],
             "duration_ms": 600, "interval_ms": 8, "hold_ms": 0},
        ])
        p.gate("graph_multi_drag", i, p.serial_after("graph_multi_drag", i, serial) + [
            {"expect": "result", "command": {"cmd": "list", "pattern": C + f"graph_node#{nodes[-1]}"},
             "path": "widgets[0].scene[1]", "op": "ne", "value_of": f"multi_y_{i}"}])
        p.add(f"dragtrace/graph_multi_drag/{i}", cmd="trace_dump", name=CANVAS, records=False)
    p.dump()
    for i in range(p.count):
        p.add(f"setup: fit {i}", cmd="action", name="pelican.graphCanvas.fitView")
        p.op("graph_rubber_band_select", i, [
            {"cmd": "drag", "from": {"name": CANVAS, "point": [4, 4]},
             "to": {"name": CANVAS, "point": [300, 220]}, "steps": 8},
        ])
        p.add(f"setup: clear selection {i}", cmd="click", name=CANVAS, point=[6, 6])
    p.dump()


def ops_fold(p: Plan) -> None:
    for i in range(p.count):
        p.add(f"setup: pick a {i}", cmd="click", name=C + "graph_node_header#render_fullscreen")
        p.add(f"setup: pick b {i}", cmd="click", name=C + "graph_node_header#render_fullscreen_2",
              modifiers=["ctrl"])
        p.op("graph_fold_into_group", i, [
            {"cmd": "action", "name": "pelican.graphCanvas.foldIntoGroup"},
            wait(NOTICE, 60000, contains="Folded"),
            wait(C + "graph_fold_group#fold_1", 60000, visible=True),
        ])
        p.add(f"setup: heading {i}", cmd="click", name=C + "graph_fold_heading#fold_1")
        p.op("graph_unfold_group", i, [
            {"cmd": "action", "name": "pelican.graphCanvas.unfoldGroup"},
            wait("pelican.graphCanvasFold.unfoldConfirm", 30000, visible=True),
            {"cmd": "click", "name": "pelican.graphCanvasFold.unfoldConfirm.ok"},
            wait(NOTICE, 60000, contains="Unfolded"),
        ])
        p.add(f"setup: settle {i}", **wait("pelican.graphCanvasAddNode", 60000, enabled=True))
    p.dump()


def ops_tab(p: Plan, node: str) -> None:
    for i in range(p.count):
        p.add(f"setup: other tab {i}", cmd="action", name="pelican.view.renderTools.frame_plan")
        p.op("graph_tab_switch_show", i, [
            {"cmd": "action", "name": "pelican.view.renderTools.graph"},
            wait(C + f"graph_node#{node}", 60000, visible=True),
        ])
    p.dump()


def ops_size_hints(p: Plan) -> None:
    p.add("size_hints/graph", cmd="size_hints")
    p.add("setup: frame plan tab", cmd="action", name="pelican.view.renderTools.frame_plan")
    p.add("size_hints/frame_plan", cmd="size_hints")
    p.add("setup: graph tab", cmd="action", name="pelican.view.renderTools.graph")


def ops_open_project(p: Plan, node: str, samples: int) -> None:
    for i in range(samples):
        target = "{other_project}" if i % 2 == 0 else "{project}"
        p.op("project_open_to_graph_ready", i, [
            {"cmd": "open_project", "dir": target},
            {"cmd": "wait_engine", "timeout_ms": 180000},
            wait(C + f"graph_node#{node}", 180000, visible=True),
            wait("pelican.graphCanvasAddNode", 180000, enabled=True),
        ])
        p.dump()


def ops_resize(p: Plan) -> None:
    for i in range(p.count):
        size = (960, 600) if i % 2 == 0 else (720, 450)
        # Done for the user when the engine's window has the new size (the
        # viewport's status line reads it from Win32). The engine rebuilds its
        # swapchain and targets once the extent has held for 100 ms (WR-1), so
        # the gate waits 30 frames: that rebuild lands inside the sample's
        # window (engine_output_relower_*), outside its latency.
        p.op("viewport_resize", i, [
            {"cmd": "viewport_size", "width": size[0], "height": size[1]},
            wait("pelican.viewportStatus", 30000, contains=f"engine {size[0]}x{size[1]} px"),
        ])
        p.gate("viewport_resize", i, [{"cmd": "wait_engine", "frames": 30, "timeout_ms": 60000}])
    p.dump()


# ---------------------------------------------------------------------------
# Sections: one Studio launch each


def build_section(kind: str, samples: int, warmup: int, extra: dict) -> tuple[Plan, str]:
    p = Plan(f"lt1_{kind}", samples, warmup)
    if kind == "small_basic":
        p.add("startup: engine has presented 60 frames", cmd="wait_engine", frames=60, timeout_ms=180000)
        p.add("setup: graph ready", **wait(C + "graph_node#left", 120000, visible=True))
        ops_baseline(p)
        setup_graph(p, "left")
        p.add("setup: clock shown", **wait(C + "graph_node#clock", 60000, visible=True))
        ops_select(p, ("left", "clock"))
        ops_view(p)
        ops_pill(p)
        ops_drag(p, "left")
        ops_tab(p, "left")
        ops_size_hints(p)
        return p, "small"
    if kind == "small_edit":
        setup_graph(p, "left")
        p.add("setup: clock shown", **wait(C + "graph_node#clock", 60000, visible=True))
        ops_connect(p)
        ops_add_delete(p)
        ops_undo_redo(p)
        ops_definition(p, "left", "gain_curve", "left")
        return p, "small"
    if kind == "small_fold":
        setup_graph(p, "left")
        for name in ("render_fullscreen", "render_fullscreen_2"):
            serial = p.serial_before("fold_setup", 0)
            p.add(f"setup: add {name}", cmd="action", name="pelican.graphCanvas.addFullscreenPass")
            p.add(f"setup: {name} drawn", **wait(C + f"graph_node#{name}", 60000, visible=True))
            p.gate("fold_setup", 0, p.serial_after("fold_setup", 0, serial))
        ops_fold(p)
        return p, "small"
    if kind == "core":
        # D: the main operations (1, 2, 4, 7, 8) under an fps / affinity condition.
        setup_graph(p, "left")
        p.add("setup: clock shown", **wait(C + "graph_node#clock", 60000, visible=True))
        ops_select(p, ("left", "clock"))
        ops_pill(p)
        ops_drag(p, "left", intervals=(8,))
        ops_connect(p)
        ops_undo_redo(p)
        return p, "small"
    if kind == "project_open":
        # 11: its own launch -- after small_basic's sequence an open_project
        # once left the engine "not ready" for 180 s (see the LT-1 report).
        setup_graph(p, "left")
        ops_open_project(p, "left", max(3, min(8, samples // 2)))
        return p, "small"
    if kind == "docks":
        # E alone: what holds each dock up (small_basic reads it too, after
        # its operations; this reads it on a fresh window).
        setup_graph(p, "left")
        ops_size_hints(p)
        return p, "small"
    if kind == "resize":
        p.add("startup: engine has presented 60 frames", cmd="wait_engine", frames=60, timeout_ms=180000)
        ops_resize(p)
        return p, "small"
    if kind.startswith("scale"):
        n = extra["nodes"]
        setup_graph(p, "left")
        p.add("setup: chain shown", **wait(C + f"graph_node#n{n}", 60000, visible=True))
        ops_select(p, ("left", f"n{max(2, n // 2)}"))
        ops_drag(p, "n1", intervals=(8,))
        ops_pill(p)
        ops_view(p)
        ops_multi(p, [f"n{k}" for k in range(1, 6)])
        ops_tab(p, "left")
        return p, "scale"
    if kind == "example":
        setup_graph(p, "loop", show_value=False)
        ops_view(p)
        ops_drag(p, "loop", intervals=(8,), onward=True)
        ops_loop_body(p)
        ops_tab(p, "loop")
        return p, "example"
    if kind == "posegraph":
        setup_graph(p, "cycle")
        p.add("setup: blend_curve shown", **wait(C + "graph_node#blend_curve", 60000, visible=True))
        ops_select(p, ("hero_walk_run", "cycle"))
        # The view first, as before the drags (they move the cycle node).
        ops_view(p)
        ops_drag(p, "cycle", intervals=(8,), level=True)
        ops_definition(p, "hero_walk_run", "walk_run", "cycle")
        ops_tab(p, "cycle")
        return p, "posegraph"
    raise ValueError(kind)


def prepare_project(root: Path, kind: str, section_dir: Path, fps: float, extra: dict) -> None:
    def frame_rate(project: Path) -> None:
        path = project / "project.json"
        document = json.loads(path.read_text(encoding="utf-8"))
        document["basic_config"]["framerate"] = fps
        path.write_text(json.dumps(document, indent=2), encoding="utf-8")

    ignore = shutil.ignore_patterns(".pelican")
    for name in ("project", "project_control"):
        target = section_dir / name
        if kind in ("small", "scale"):
            shutil.copytree(root / "test/fixtures/studio_automation_graph", target, ignore=ignore)
        elif kind == "example":
            shutil.copytree(root / "projects/example", target, ignore=ignore)
            # The example's models and textures are not in git (.gitignore); a
            # checkout that has them is named with --example-assets (its
            # assets/models directory; the textures are its sibling).
            if extra.get("example_assets"):
                models = Path(extra["example_assets"])
                for source, folder in ((models, "models"), (models.parent / "textures", "textures")):
                    for item in source.glob("*.*"):
                        destination = target / "assets" / folder / item.name
                        if not destination.exists():
                            shutil.copy2(item, destination)
        elif kind == "posegraph":
            shutil.copytree(root / "projects/posegraph_demo", target, ignore=ignore)
        if kind == "scale":
            path = target / "passes/main.json"
            document = json.loads(path.read_text(encoding="utf-8"))
            previous = "clock"
            for k in range(1, extra["nodes"] + 1):
                document["nodes"].append({
                    "id": f"n{k}", "op": "value.add",
                    "inputs": {"a": {"node": previous, "port": "value"},
                               "b": {"node": "clock", "port": "value"}},
                    "outputs": ["value"]})
                previous = f"n{k}"
            path.write_text(json.dumps(document, indent=2) + "\n", encoding="utf-8")
        frame_rate(target)
    if kind == "example":
        # dl1_drag_example.prepare.py's anchored empty loop, in these copies only.
        subprocess.run([sys.executable, str(root / "test/studio_scenarios/dl1_drag_example.prepare.py"),
                        "--out", str(section_dir / "incoming"), "--cli", "unused"], check=True)


# ---------------------------------------------------------------------------
# CPU affinity (D). Children inherit the mask a process has when it creates
# them, so the lease, the driver, Studio and every engine it starts run on it.


def set_affinity(cores: int | None) -> dict:
    if sys.platform != "win32":
        return {"supported": False}
    kernel = ctypes.windll.kernel32
    kernel.GetCurrentProcess.restype = ctypes.c_void_p
    process = kernel.GetCurrentProcess()
    mask_process = ctypes.c_size_t()
    mask_system = ctypes.c_size_t()
    kernel.GetProcessAffinityMask(ctypes.c_void_p(process), ctypes.byref(mask_process),
                                  ctypes.byref(mask_system))
    system = mask_system.value
    if cores is None:
        wanted = system
    else:
        bits = [b for b in range(64) if system >> b & 1]
        # Every other logical processor first: whole physical cores on SMT parts.
        ordered = bits[::2] + bits[1::2]
        wanted = 0
        for b in ordered[:cores]:
            wanted |= 1 << b
    ok = kernel.SetProcessAffinityMask(ctypes.c_void_p(process), ctypes.c_size_t(wanted))
    return {"supported": True, "ok": bool(ok), "mask": hex(wanted), "system_mask": hex(system),
            "cores": bin(wanted).count("1")}


# ---------------------------------------------------------------------------
# Analysis


def load_jsonl(path: Path) -> list[dict]:
    rows = []
    if not path.is_file():
        return rows
    for line in path.read_text(encoding="utf-8", errors="replace").splitlines():
        line = line.strip()
        if line:
            try:
                rows.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return rows


def pct(values: list[float], p: float) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(len(ordered) * p) - 1)]


class Traces:
    def __init__(self, section_dir: Path) -> None:
        studio = []
        for path in sorted(section_dir.glob("optrace_*.jsonl")):
            for r in load_jsonl(path):
                studio.append((r["s"], r["b"] / 1e9, r["e"] / 1e9, r.get("x", 0), r.get("y", 0)))
        studio.sort(key=lambda r: r[1])
        self.studio = studio
        self.begins = [r[1] for r in studio]
        engine = load_jsonl(section_dir / "engine_trace.jsonl")
        self.rpc = [r for r in engine if r.get("kind") == "rpc"]
        self.spans = sorted([r for r in engine if r.get("kind") == "span"], key=lambda r: r["begin_ns"])
        # The engine numbers nothing itself: the id is Studio's. A restarted
        # engine keeps Studio's counter, so (id, nearest time) pairs.
        self.rpc_by_id: dict[str, list[dict]] = collections.defaultdict(list)
        for r in self.rpc:
            self.rpc_by_id[r["id"]].append(r)

    def window(self, start: float, end: float) -> list[tuple]:
        import bisect
        lo = bisect.bisect_left(self.begins, start)
        hi = bisect.bisect_right(self.begins, end)
        return self.studio[lo:hi]

    def engine_for(self, rid: float, sent: float) -> dict | None:
        candidates = self.rpc_by_id.get(str(int(rid)), [])
        best = None
        for r in candidates:
            received = r["received_ns"] / 1e9
            if received + 0.001 >= sent and (best is None or received < best["received_ns"] / 1e9):
                best = r
        return best


EDIT_METHODS = {"edit_graph_document", "fold_nodes", "edit_preset_pass", "edit_render_targets",
                "edit_target_planning"}


def sample_metrics(tr: Traces, t0: float, t_meas: float, t_end: float, operation: str,
                   extra: dict) -> dict[str, float]:
    m: dict[str, float] = {"driver_total_ms": (t_meas - t0) * 1000}
    rows = tr.window(t0 - 0.002, t_end)
    ms = lambda t: (t - t0) * 1000
    commands = [r for r in rows if r[0] == "automation_command"]
    if commands:
        m["studio_input_command_ms"] = (commands[0][2] - commands[0][1]) * 1000
    sends = [r for r in rows if r[0] == "rpc_send"]
    results = {int(r[3]): r for r in rows if r[0] == "rpc_result"}
    methods = []
    senders = []
    whos = [r for r in rows if r[3] == -1 and r[0]]
    frame_waits, handlers, pipes_in, pipes_out, reflect = [], [], [], [], []
    boundary_waits, batch_waits, batch_sizes = [], [], []
    edit_accept = None
    for s in sends:
        e = tr.engine_for(s[3], s[1])
        method = e["method"] if e else "?"
        methods.append(method)
        # Who asked: the innermost digest-subscriber span (renderconfigdigest.hpp,
        # named after the panel's class, detail -1) around the send, if any.
        holders = [w for w in whos if w[1] <= s[1] <= w[2]]
        senders.append(f"{method}<-{max(holders, key=lambda w: w[1])[0] if holders else '-'}")
        if not e:
            continue
        frame_waits.append((e["dispatched_ns"] - e["received_ns"]) / 1e6)
        boundary_waits.append(max(0.0, (e["boundary_ns"] - e["received_ns"]) / 1e6))
        batch_waits.append((e["dispatched_ns"] - e["boundary_ns"]) / 1e6)
        batch_sizes.append(e["batch_size"])
        handlers.append((e["handler_end_ns"] - e["handler_begin_ns"]) / 1e6)
        pipes_in.append((e["received_ns"] / 1e9 - s[1]) * 1000)
        res = results.get(int(s[3]))
        if res:
            pipes_out.append((res[1] - e["written_ns"] / 1e9) * 1000)
            reflect.append((res[2] - res[1]) * 1000)
            if method in EDIT_METHODS and edit_accept is None:
                edit_accept = res[1]
    m["rpc_count"] = len(sends)
    m["rpc_get_status_count"] = sum(1 for x in methods if x == "get_status")
    non_status = [x for x in methods if x != "get_status"]
    m["rpc_duplicate_non_status"] = len(non_status) - len(set(non_status))
    m["rpc_get_edit_result_count"] = sum(1 for x in methods if x == "get_edit_result")
    m["rpc_request_bytes"] = sum(s[4] for s in sends)
    m["rpc_response_bytes"] = sum(r[4] for r in results.values() if t0 - 0.002 <= r[1] <= t_end)
    if sends:
        m["first_rpc_send_ms"] = ms(sends[0][1])
    if frame_waits:
        m["engine_frame_wait_ms_sum"] = sum(frame_waits)
        m["engine_frame_wait_ms_max"] = max(frame_waits)
        m["engine_wait_for_boundary_ms_max"] = max(boundary_waits)
        m["engine_wait_for_boundary_ms_mean"] = sum(boundary_waits) / len(boundary_waits)
        m["engine_wait_in_batch_ms_max"] = max(batch_waits)
        m["engine_batch_size_max"] = max(batch_sizes)
        m["engine_handler_ms_sum"] = sum(handlers)
        m["engine_handler_ms_max"] = max(handlers)
        m["pipe_to_engine_ms_max"] = max(pipes_in)
    if pipes_out:
        m["pipe_to_studio_ms_max"] = max(pipes_out)
        m["studio_reply_handling_ms_sum"] = sum(reflect)
        m["studio_reply_handling_ms_max"] = max(reflect)
    if edit_accept is not None:
        m["edit_accept_reply_ms"] = ms(edit_accept)
    marks = {name: [r for r in rows if r[0] == name] for name in
             ("graph_edit_submitted", "graph_edit_committed", "graph_edit_still_pending")}
    if marks["graph_edit_submitted"]:
        m["edit_submitted_ms"] = ms(marks["graph_edit_submitted"][0][1])
    m["edit_still_pending_count"] = len(marks["graph_edit_still_pending"])
    committed = marks["graph_edit_committed"][0][1] if marks["graph_edit_committed"] else None
    if committed is not None:
        m["edit_committed_ms"] = ms(committed)
    repop = [r for r in rows if r[0] == "graph_scene_repopulate"]
    m["scene_repopulate_count"] = len(repop)
    if repop:
        m["scene_repopulate_ms_sum"] = sum((r[2] - r[1]) * 1000 for r in repop)
        m["scene_repopulate_ms_max"] = max((r[2] - r[1]) * 1000 for r in repop)
        items = [r[3] for r in rows if r[0] == "graph_scene_items"]
        if items:
            m["scene_items_max"] = max(items)
        # The rebuild's phases, from the marks repopulate() leaves at their
        # starts (graphcanvaswidget.cpp, graph_repop_mark 1..10): phase 0 is
        # closing the editor and emptying the scene, phase k runs from mark k.
        marks = [r for r in rows if r[0] == "graph_repop_mark"]
        phases: dict[int, float] = collections.defaultdict(float)
        for rp in repop:
            inside = sorted((r for r in marks if rp[1] <= r[1] <= rp[2]), key=lambda r: r[1])
            edges = [(0, rp[1])] + [(int(r[3]), r[1]) for r in inside] + [(None, rp[2])]
            for (k, t), (_, t_next) in zip(edges, edges[1:]):
                phases[k] += (t_next - t) * 1000
        for k, v in phases.items():
            m[f"scene_repop_phase{k}_ms_sum"] = v
    paints = [r for r in rows if r[0] == "graph_paint"]
    m["graph_paint_count"] = len(paints)
    m["graph_paint_full_count"] = sum(1 for r in paints if r[4] and r[3] >= 0.9 * r[4])
    if paints:
        m["graph_paint_ms_sum"] = sum((r[2] - r[1]) * 1000 for r in paints)
        m["graph_paint_ms_max"] = max((r[2] - r[1]) * 1000 for r in paints)
        inside = [r for r in paints if r[2] <= t_meas + 0.0005]
        if inside:
            m["last_paint_end_in_op_ms"] = ms(inside[-1][2])
        if committed is not None:
            after = [r for r in paints if r[2] >= committed]
            if after:
                m["paint_after_commit_ms"] = ms(after[0][2])
        if repop:
            after = [r for r in paints if r[1] >= repop[-1][2]]
            if after:
                m["paint_after_last_repopulate_ms"] = ms(after[0][2])
    for stage in ("graph_deliver_result", "graph_dispatch", "graph_poll", "graph_show_details",
                  "inspector_deliver_result"):
        spans = [r for r in rows if r[0] == stage]
        if spans:
            m[f"{stage}_ms_sum"] = sum((r[2] - r[1]) * 1000 for r in spans)
            m[f"{stage}_count"] = len(spans)
    notifies = [r for r in rows if r[0] == "qt_notify"]
    if notifies:
        m["ui_longest_event_ms"] = max((r[2] - r[1]) * 1000 for r in notifies)
        releases = [r for r in notifies if int(r[3]) == 3]
        if operation.startswith("graph_drag_") and releases:
            release = releases[-1][1]
            m["drag_release_offset_ms"] = ms(release)
            if committed is not None:
                m["drag_release_to_committed_ms"] = (committed - release) * 1000
    engine_spans = collections.defaultdict(list)
    for s in tr.spans:
        b = s["begin_ns"] / 1e9
        if t0 - 0.002 <= b <= t_end:
            engine_spans[s["stage"]].append((s["end_ns"] - s["begin_ns"]) / 1e6)
    for stage, values in engine_spans.items():
        m[f"engine_{stage}_ms_sum"] = sum(values)
        m[f"engine_{stage}_count"] = len(values)
    m.update(extra)
    m["_methods"] = methods  # type: ignore[assignment]
    m["_senders"] = senders  # type: ignore[assignment]
    return m


def analyze_section(section_dir: Path, warmup: int) -> dict:
    report_path = section_dir / "report.json"
    if not report_path.is_file():
        return {"ok": False, "error": "no report.json", "operations": {}}
    report = json.loads(report_path.read_text(encoding="utf-8"))
    tr = Traces(section_dir)
    groups: dict[tuple[str, int], dict[str, list]] = collections.defaultdict(
        lambda: {"latency": [], "gate": [], "dragtrace": []})
    for step in report["steps"]:
        label = step.get("label", "")
        parts = label.split("/")
        if parts[0] in ("latency", "gate") and len(parts) == 4:
            groups[(parts[1], int(parts[2]))][parts[0]].append(step)
        elif parts[0] == "dragtrace" and len(parts) == 3:
            groups[(parts[1], int(parts[2]))]["dragtrace"].append(step)
    samples: dict[str, list[dict]] = collections.defaultdict(list)
    failed: dict[str, int] = collections.Counter()
    first: dict[str, float] = {}
    offsets = []
    for (operation, index), g in sorted(groups.items(), key=lambda kv: kv[1]["latency"][0]["started_perf_s"]
                                        if kv[1]["latency"] else 0):
        if not g["latency"]:
            continue
        if not all(s["ok"] for s in g["latency"] + g["gate"]):
            failed[operation] += 1
            continue
        t0 = g["latency"][0]["started_perf_s"]
        t_meas = g["latency"][-1]["finished_perf_s"]
        t_end = (g["gate"] or g["latency"])[-1]["finished_perf_s"]
        extra: dict[str, float] = {}
        for d in g["dragtrace"]:
            if not d["ok"]:
                continue
            result = (d.get("response") or {}).get("result") or {}
            summary = result.get("summary", {})
            extra["dl_input_to_paint_p50_ms"] = summary.get("input_to_paint", {}).get("p50_ms", float("nan"))
            extra["dl_input_to_paint_p95_ms"] = summary.get("input_to_paint", {}).get("p95_ms", float("nan"))
            extra["dl_input_to_paint_max_ms"] = summary.get("input_to_paint", {}).get("max_ms", float("nan"))
            extra["dl_unpainted_moves"] = result.get("unpainted_moves", float("nan"))
            extra["dl_moves"] = result.get("moves", float("nan"))
            extra["dl_final_position_latency_ms"] = result.get("final_position_latency_ms", float("nan"))
            extra["dl_unpainted_tail_ms"] = result.get("unpainted_tail_ms", float("nan"))
        metrics = sample_metrics(tr, t0, t_meas, t_end, operation, extra)
        rows = tr.window(t0 - 0.002, t_meas)
        cmd = [r for r in rows if r[0] == "automation_command"]
        if cmd:
            offsets.append((cmd[0][1] - t0) * 1000)
        if index == 0:
            first[operation] = metrics["driver_total_ms"]
        if index < warmup:
            continue
        samples[operation].append(metrics)
    operations = {}
    for operation, ms_list in samples.items():
        keys = sorted({k for m in ms_list for k in m if not k.startswith("_")})
        stages = {}
        for k in keys:
            values = [m[k] for m in ms_list if k in m and isinstance(m[k], (int, float))
                      and not (isinstance(m[k], float) and math.isnan(m[k]))]
            if not values:
                continue
            stages[k] = {"n": len(values), "median": statistics.median(values),
                         "p95": pct(values, .95), "max": max(values), "min": min(values)}
        method_counts = collections.Counter(x for m in ms_list for x in m["_methods"])
        sender_counts = collections.Counter(x for m in ms_list for x in m.get("_senders", []))
        operations[operation] = {"samples": len(ms_list), "failed_samples": failed.get(operation, 0),
                                 "first_sample_driver_ms": first.get(operation),
                                 "stages": stages,
                                 "rpc_methods_per_sample": {k: v / len(ms_list) for k, v in
                                                            method_counts.most_common()},
                                 "rpc_senders_per_sample": {k: v / len(ms_list) for k, v in
                                                            sender_counts.most_common()}}
    for operation, n in failed.items():
        operations.setdefault(operation, {"samples": 0, "failed_samples": n, "stages": {}})
    extras = {}
    for step in report["steps"]:
        label = step.get("label", "")
        if label.startswith("size_hints/") and step["ok"]:
            extras[label] = (step.get("response") or {}).get("result")
        if label.startswith("record: ") and step["ok"]:
            extras[label] = step.get("response")
    return {"ok": report.get("ok"), "verdict": report.get("verdict"),
            "failures": [{"label": f.get("label"), "reason": f.get("reason") or f.get("detail")}
                         for f in report.get("failures", [])][:5],
            "clock_check_command_offset_ms": {
                "median": statistics.median(offsets) if offsets else None,
                "min": min(offsets) if offsets else None, "max": max(offsets) if offsets else None},
            "studio_records": len(tr.studio), "engine_rpc_records": len(tr.rpc),
            "engine_span_records": len(tr.spans),
            "operations": operations, "extras": extras}


def layout_nodes(section: dict) -> Any:
    for label, response in section.get("extras", {}).items():
        if label == "record: layout_stats" and isinstance(response, dict):
            # The driver records an `expect: result` reading as {path, value}.
            if "value" in response:
                return response["value"]
            result = response.get("result") or response
            return (result.get("result") if isinstance(result.get("result"), dict) else result).get("nodes")
    return None


def write_tables(run_dir: Path, sections: dict) -> None:
    rows = []
    for name, section in sections.items():
        meta = section["meta"]
        nodes = layout_nodes(section["analysis"])
        for operation, data in section["analysis"].get("operations", {}).items():
            for stage, s in data.get("stages", {}).items():
                rows.append({"run": run_dir.name, "section": name, "fps": meta["fps"],
                             "cores": meta["cores"], "graph": meta["graph"], "layout_nodes": nodes,
                             "operation": operation, "stage": stage, "n": s["n"],
                             "median": s["median"], "p95": s["p95"], "max": s["max"]})
    header = ["run", "section", "fps", "cores", "graph", "layout_nodes", "operation", "stage", "n",
              "median", "p95", "max"]
    with (run_dir / "operation_stages.csv").open("w", encoding="utf-8", newline="") as f:
        f.write(",".join(header) + "\n")
        for r in rows:
            f.write(",".join("" if r[h] is None else (f"{r[h]:.4f}" if isinstance(r[h], float) else str(r[h]))
                             for h in header) + "\n")
    (run_dir / "operation_stages.json").write_text(json.dumps(
        {name: {"meta": s["meta"], **s["analysis"]} for name, s in sections.items()}, indent=1),
        encoding="utf-8")


def aggregate(out_root: Path, run_dirs: list[Path]) -> Path:
    combined: dict[tuple, list[float]] = collections.defaultdict(list)
    p95s: dict[tuple, list[float]] = collections.defaultdict(list)
    maxes: dict[tuple, list[float]] = collections.defaultdict(list)
    for run in run_dirs:
        path = run / "operation_stages.csv"
        if not path.is_file():
            continue
        lines = path.read_text(encoding="utf-8").splitlines()
        header = lines[0].split(",")
        for line in lines[1:]:
            r = dict(zip(header, line.split(",")))
            key = (r["section"], r["fps"], r["cores"], r["graph"], r["layout_nodes"], r["operation"], r["stage"])
            combined[key].append(float(r["median"]))
            p95s[key].append(float(r["p95"]))
            maxes[key].append(float(r["max"]))
    stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
    target = out_root / f"aggregate-{stamp}"
    suffix = 1
    while target.exists():  # two aggregates in one second: never overwrite
        suffix += 1
        target = out_root / f"aggregate-{stamp}-{suffix}"
    target.mkdir(parents=True, exist_ok=False)
    with (target / "operation_stages_runs.csv").open("w", encoding="utf-8", newline="") as f:
        f.write("section,fps,cores,graph,layout_nodes,operation,stage,runs,median_of_medians,"
                "min_median,max_median,median_p95,max_max\n")
        for key, values in sorted(combined.items()):
            f.write(",".join(key) + f",{len(values)},{statistics.median(values):.4f},{min(values):.4f},"
                    f"{max(values):.4f},{statistics.median(p95s[key]):.4f},{max(maxes[key]):.4f}\n")
    (target / "runs.json").write_text(json.dumps([str(r) for r in run_dirs], indent=1), encoding="utf-8")
    return target


# ---------------------------------------------------------------------------
# Running


def machine_facts() -> dict:
    facts = {"platform": platform.platform(), "logical_processors": os.cpu_count(),
             "processor": platform.processor()}
    if sys.platform == "win32":
        for key, query in (("cpu", "Get-CimInstance Win32_Processor | Select-Object -Expand Name"),
                           ("gpu", "Get-CimInstance Win32_VideoController | Select-Object -Expand Name")):
            try:
                facts[key] = subprocess.run(["powershell", "-NoProfile", "-Command", query],
                                            capture_output=True, text=True, timeout=30).stdout.strip()
            except (OSError, subprocess.TimeoutExpired):
                pass
    return facts


def run_section(root: Path, args, run_dir: Path, name: str, kind: str, fps: float, cores: int | None,
                extra: dict) -> dict:
    section_dir = run_dir / name
    section_dir.mkdir(parents=True, exist_ok=False)
    plan, graph = build_section(kind, args.samples, args.warmup, extra)
    if graph == "example" and args.example_assets:
        extra = {**extra, "example_assets": str(args.example_assets)}
    plan_path = section_dir / "scenario.json"
    plan_path.write_text(json.dumps(plan.scenario(), indent=1), encoding="utf-8")
    prepare_project(root, graph, section_dir, fps, extra)
    build = args.build_dir.resolve()
    studio = build / "dist" / args.config / "pelican_studio.exe"
    player = build / "dist" / args.config / "pelican_player.exe"
    lease = build / "test" / "pelican_gpu_lease_run.exe"
    if not lease.is_file():
        lease = build / "test" / args.config / "pelican_gpu_lease_run.exe"
    env = dict(os.environ)
    env["PATH"] = str(args.qt_bin.resolve()) + os.pathsep + env["PATH"]
    env["PELICAN_STUDIO_SCREEN"] = env.get("PELICAN_STUDIO_SCREEN", "1")
    if not args.no_trace:
        env["PELICAN_STUDIO_OP_TRACE"] = "1"
        env["PELICAN_RPC_TRACE"] = str(section_dir / "engine_trace.jsonl")
    report_path = section_dir / "report.json"
    command = [str(lease), "--timeout-seconds", str(args.lease_timeout), "--", sys.executable,
               str(root / "tools/studio_drive.py"), "--studio", str(studio), "--player", str(player),
               "--scenario", str(plan_path), "--project", str(section_dir / "project"),
               "--var", f"other_project={section_dir / 'project_control'}",
               "--artifacts", str(section_dir), "--report", str(report_path)]
    affinity = set_affinity(cores)
    meta = {"section": name, "kind": kind, "graph": graph if not extra else f"{graph}{extra.get('nodes', '')}",
            "fps": fps, "cores": cores or "all", "affinity": affinity, "command": command,
            "started": datetime.datetime.now().isoformat(timespec="seconds")}
    started = time.perf_counter()
    try:
        with (section_dir / "driver.log").open("w", encoding="utf-8") as log:
            result = subprocess.run(command, env=env, stdout=log, stderr=subprocess.STDOUT,
                                    timeout=args.lease_timeout + 300)
        meta["returncode"] = result.returncode
    except subprocess.TimeoutExpired:
        meta["returncode"] = "timeout"
    finally:
        set_affinity(None)
    meta["seconds"] = time.perf_counter() - started
    (section_dir / "section.json").write_text(json.dumps(meta, indent=1), encoding="utf-8")
    analysis = analyze_section(section_dir, args.warmup)
    print(f"[{run_dir.name}] {name}: verdict={analysis.get('verdict')} "
          f"ops={len(analysis.get('operations', {}))} {meta['seconds']:.0f}s", flush=True)
    return {"meta": meta, "analysis": analysis}


SECTION_ORDER = ["small_basic", "small_edit", "core_fps30", "core_cores4", "core_cores2",
                 "scale10", "scale50", "scale150", "example", "posegraph", "resize", "project_open", "small_fold", "docks"]
DEBUG_SECTIONS: list[str] = []


def section_conditions(name: str) -> tuple[str, float, int | None, dict]:
    if name == "core_fps30":
        return "core", 30.0, None, {}
    if name == "core_cores4":
        return "core", 60.0, 4, {}
    if name == "core_cores2":
        return "core", 60.0, 2, {}
    if name.startswith("scale"):
        return name, 60.0, None, {"nodes": int(name[5:])}
    return name, 60.0, None, {}


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--build-dir", type=Path, default=Path("build"))
    parser.add_argument("--config", default="Release", choices=["Release", "Debug", "RelWithDebInfo"])
    parser.add_argument("--qt-bin", type=Path)
    parser.add_argument("--out-root", type=Path, default=Path("out/studio-latency-lt1"))
    parser.add_argument("--runs", type=int, default=1, help="separate launches of every section")
    parser.add_argument("--samples", type=int, default=20)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--sections", default="all", help="comma list of " + ", ".join(SECTION_ORDER))
    parser.add_argument("--fps", type=float, default=None, help="override every section's fps")
    parser.add_argument("--cpu-cores", type=int, default=None,
                        help="limit Studio and the engine to N logical processors for every section")
    parser.add_argument("--lease-timeout", type=int, default=3600)
    parser.add_argument("--example-assets", type=Path, default=None,
                        help="a directory holding projects/example/assets/models' untracked models")
    parser.add_argument("--no-trace", action="store_true",
                        help="leave both traces off (to compare the untraced timing)")
    parser.add_argument("--analyze", type=Path, nargs="*", help="re-analyse existing run directories")
    parser.add_argument("--aggregate", type=Path, nargs="*", help="combine existing run directories")
    args = parser.parse_args()
    root = Path(__file__).resolve().parents[1]
    out_root = args.out_root if args.out_root.is_absolute() else root / args.out_root
    if args.analyze:
        for run_dir in args.analyze:
            sections = {}
            for meta_path in sorted(run_dir.glob("*/section.json")):
                meta = json.loads(meta_path.read_text(encoding="utf-8"))
                sections[meta["section"]] = {"meta": meta,
                                             "analysis": analyze_section(meta_path.parent, args.warmup)}
            write_tables(run_dir, sections)
            print(run_dir)
        return 0
    if args.aggregate:
        print(aggregate(out_root, args.aggregate))
        return 0
    if args.qt_bin is None:
        parser.error("--qt-bin is required to run")
    names = SECTION_ORDER if args.sections == "all" else [s.strip() for s in args.sections.split(",")]
    for name in names:
        if name not in SECTION_ORDER + DEBUG_SECTIONS:
            parser.error(f"unknown section {name}")
    facts = machine_facts()
    revision = subprocess.check_output(["git", "rev-parse", "HEAD"], cwd=root, text=True).strip()
    diff = subprocess.run(["git", "diff", "HEAD"], cwd=root, capture_output=True).stdout
    run_dirs = []
    for run in range(args.runs):
        stamp = datetime.datetime.now().strftime("%Y%m%d-%H%M%S")
        run_dir = out_root / f"run-{stamp}-r{run + 1}"
        run_dir.mkdir(parents=True, exist_ok=False)  # never overwrite an earlier run
        build = args.build_dir.resolve()
        binaries = [build / "dist" / args.config / n for n in ("pelican_studio.exe", "pelican_player.exe")]
        environment = {"revision": revision, "working_tree_diff_sha256": hashlib.sha256(diff).hexdigest(),
                       "configuration": args.config, "samples": args.samples, "warmup": args.warmup,
                       "machine": facts,
                       "binaries": {str(p): hashlib.sha256(p.read_bytes()).hexdigest() for p in binaries},
                       "studio_screen": os.environ.get("PELICAN_STUDIO_SCREEN", "1"),
                       "limitations": [
                           "Request-to-verified-state and trace stamps, not display (present) latency",
                           "Studio stamps are GUI-thread spans; engine stamps are its RPC host and editor",
                           "Waits poll every 25 ms in Studio; serial gates poll every 200 ms (outside groups)",
                           "Shared workstation; other processes are not controlled",
                           "CPU limit is process affinity, not a slower clock or smaller cache"]}
        (run_dir / "environment.json").write_text(json.dumps(environment, indent=1), encoding="utf-8")
        sections = {}
        for name in names:
            kind, fps, cores, extra = section_conditions(name)
            if args.fps is not None:
                fps = args.fps
            if args.cpu_cores is not None:
                cores = args.cpu_cores
            sections[name] = run_section(root, args, run_dir, name, kind, fps, cores, extra)
            write_tables(run_dir, sections)
        run_dirs.append(run_dir)
        print(run_dir, flush=True)
    if len(run_dirs) > 1:
        print(aggregate(out_root, run_dirs))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
