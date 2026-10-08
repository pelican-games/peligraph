# QD-1: scenario invariant findings

Original inventory dated 2026-10-05. Ordinary scenario assertions passed in that 84-case inventory. The matrix preserves observed failures even when a ticketed `known_red` mark lets CTest pass.

There are now **17 marks in 17 scenarios**: 17 Undo/Redo checks and 0 health checks. The original inventory had 49 marks in 46 scenarios; the October 5 follow-up resolved 16 marks, UD-1 resolved 2 more, UD-3 resolved 3 more, and the 2026-10-08/09 rechecks resolved 11 more. The 17 that remain fall into three classes: 13 are bundle B (engine restart inside the Undo walk / scene journal, v1 by user decision on 2026-10-08), 3 are documented limits D / E / F, and 1 (R24) was not measured because an ordinary step is unsteady. These are reproduction records, not a count of independent root causes. No reopen check is marked known red.


## UD-4 to UD-7 recheck (2026-10-08 to 2026-10-09)

Fixes on develop after the UD-1 + UD-2 + UD-3 recheck below:

- UD-4 (`5c0af0912`): an edit the engine accepts for a restart is checked as a start would check it before it commits.
- UD-5 (`2d732ae39`): another panel's confirmed edit no longer wipes the Graph tab's Undo/Redo, and Graph Undo/Redo is sent in time order (RT-D), refused before sending while a newer edit from another panel is on top. Its fix2 no longer makes an old Render branch a precondition of Redo, and a named click first brings an off-screen item into view.
- UD-6 (`f09ef4014`): an external reload that changes only images keeps the Graph and Render histories (bundle C), and the driver compares JSON documents by parsed content (bundle G).
- UD-7 (`05c198b80`): the driver answers the unsaved-scene question (`pelican.unsavedScenePrompt`) that a restart asks during the walk with Save, and records `answered_unsaved_scene_prompt: "save"`.

Results (bundle letters are those of `<作業ディレクトリ>/qd_triage_report.md`):

- Full run at `33b761ecd` on 2026-10-08 (`<build>/test_artifacts/studio_scenarios/<name>/report.json`): `gui_build_curve` (R17), `gui_build_ocean` (R22), `gui_build_ocean_compute` (R23), `gui_build_rt_from_blank` (R25) and `gui_graph_only_edits` (R27) are Undo green, resolved by RT-A to RT-D and UD-1 to UD-3.
- UD-6 runs: `gui_rt_procedural_callable` (R34), `gui_rt_two_rays_two_misses` (R35) and `gui_assets_watch_verify` (R13) are Undo green. In `gui_build_ltc` (R21) the render differences are gone and only the scene keys differ (bundle B).
- UD-5 landing check (`<作業ディレクトリ>/ud5_work/w1_final/<name>.json`): `graph_fold_decisions` (R10), `material_pass_authoring` (R4) and `material_loop_decompose` (R42) are Undo green. `gui_build_D` (R18), `gui_build_full` (R20), `gui_settings_choose` (R36), `sockets_fix_and_show` (R44) and `input_needs_shader` (R37) return to the start on Undo, and after Redo only `scene:(answer)` / `scene:(semantic)` differ (bundle B). `gui_rt_hit_groups_from_blank` (R33) differs only in the New material files and `list_materials` (limit F). `gui_build_optimize` (R24) stops at an unsteady ordinary step before the epilogue.
- `gui_build_C` (R16) is the same cause as bundle B (investigation). `gui_material_ray_query_from_blank` (R3, limits D and F) and `sockets_from_shader` (R45, limit E) are design limits.

The eleven green marks come off. Remaining: bundle B, 13 (R5, R6, R11, R14, R15, R16, R18, R19, R20, R21, R36, R37, R44); limits D / E / F, 3 (R3, R33, R45); not measured, 1 (R24). No comparison, exemption or pixel expectation was relaxed.

## UD-1 + UD-2 + UD-3 integration recheck (2026-10-08)

Integration executed the eleven prescribed scenarios with the UD-1 + UD-2 + UD-3 build (`<作業ディレクトリ>/ud3_work/gpu11.log`: 11/11 CTest passes, 538.74 s). Ordinary assertions, reopen and health are green in all eleven, and no Redo comparison differs. Undo/Redo is green in `graph_from_blank`, `frameplan_layout_then_edit`, `gui_frameplan_delegates`, `gui_graph_new_target` and `gui_graph_only_edits_2`; the last three were resolved by UD-3. The remaining six Undo checks (`add_primitive_object`, `deferred_loop_from_scratch`, `graph_from_blank_forward`, `gui_build_A`, `gui_build_B`, `gui_build_E`) are `known_red`. Their measured `undo_differs`, `undo.log` and `undo.notes` are quoted below.

UD-3 fixes the Graph history inverse: the palette entry that adds a forward material pass did not restore `material_route_default` when undone. One Undo now sends the pass removal and the restored route default (null removes a default that was absent) in a single `edit_graph_document`, and the engine restarts once.

The six remaining reports differ only in `scene:(answer)` and `scene:(semantic)`; no file or render document differs. The walk presses Graph before scene. The Graph Undo that returns the pipeline to blank (no material pass) restarts the engine (UD-1's `material_passes_removed`) while a scene Undo such as Add Object is still pressable; `undo.notes` records `scene: Undo was pressable but unconsumed before the engine restart`. The restart drops the engine's scene journal, so the Inspector Undo reports `actor has no committed transaction to undo`; the scene content itself remains. This is a product limitation. Carrying the journal across the restart (v1; the user decided on 2026-10-08 not to change it in v0) is the planned fix; it applies to every engine restart, not only this Undo. The driver waits for Graph/Render histories to recover after the restart (`restart_history_ready: true` in all six), so a temporarily disabled button is not the cause. A pressable but unconsumed scene Undo is not exempted: the reports retain and compare its scene keys. No scenario mark, comparison or pixel expectation is relaxed.

## Evidence and interpretation

- Full run: 84 scenarios at `8c8752119`, 5212.34 s wall time; 38 CTest passes / 46 failures before the final inventory marks.
- Checker corrections at `0828834f5`: seven pilot cases and 23 affected follow-up cases were rerun. The matrix uses the latest executed result for each case. This is not a claim that all 84 ran together at the final revision.
- Final representative run at `f91453a6e`: 6/6 CTest passes in 327.25 s; four Undo and one health result remain explicitly `known_red`. No ordinary or reopen failure occurred. The 84 recorded outcomes also passed an offline policy audit; that audit is not a GUI rerun.
- Ordinary assertions, reopen and health are separate from Undo/Redo. `red` below means the invariant detected the recorded difference; `N/A` has a stated scope reason. The letter after `red` is the class used in Remaining findings (B, D, E, F); `not measured` means the epilogue did not run.
- Each case is reproducible with `ctest --test-dir <build> -C Debug -R "^studio_scenario_<scenario>$" -j1 --output-on-failure`. Reports are in `<build>/test_artifacts/studio_scenarios/<scenario>/report.json`; invariant steps immediately follow the ordinary scenario steps.
- The full-run and targeted-run reports/logs were archived before final mark verification. The integration report records their local locations.
- Studio CPU: 774/774. Python CI: 79/79 including the final incomplete-traversal guard. Nine isolated Python mutations each failed the relevant test and passed after restoration. This was not a mutation campaign across every C++ command. No golden was regenerated.

## Follow-up fixes (2026-10-05)

Production fixes cover project-scoped Material history, the Materials dock's missing binding inverse, per-entry procedural-record restoration, Graph Redo across confirmed Render restores, model-resource destruction order, and shader failure retirement. The checker separately corrects uncommitted inline editors and empty containers left by an already-exempt placement subtree. No golden was regenerated and no error was added to an allowlist.

Validation is targeted, not a second all-84 run: 125 Graph/Materials/Render-history CPU cases, 81 Python CI cases, engine procedural-record and shader regression cases, GPU late-model teardown and shader-retirement cases, and the dated GUI rechecks in the matrix. Earlier in the fix series the complete Studio CPU set passed 776/776. Local reports/logs are archived in `<作業ディレクトリ>/studio_fixes_final_evidence.zip`; intermediate diagnostic evidence is in `<作業ディレクトリ>/studio_fixes_mid_evidence.zip` and `<作業ディレクトリ>/studio_fixes_lifetime_evidence.zip`.

On 2026-10-05 the remaining marks were still executable reproductions. They included intentional route-contract refusals, the documented value-only Assets Undo, and Graph/Render ordering or missing-inverse cases that these fixes did not resolve; see the rechecks above for their current classification. A CTest pass with `known_red` is not an invariant pass.

## All 84 scenarios

| Scenario | Ordinary | Undo / Redo | Reopen | Health | Tickets | Evidence |
|---|---|---|---|---|---|---|
| add_primitive_object | pass | red (B) | green | green | QD1-R5 | integration 2026-10-08 14:12 |
| canvas_zoom_pan | pass | green | green | green |  | full run |
| deferred_loop_from_scratch | pass | red (B) | green | green | QD1-R6 | integration 2026-10-08 14:13 |
| engine_window_embedded | pass | green | green | green |  | full run |
| forward_loop_from_scratch | pass | green | green | green |  | full run |
| frameplan_edit_roundtrip | pass | green | green | green |  | full run |
| frameplan_layout_then_edit | pass | green | green | green |  | integration 2026-10-08 14:14 |
| fullscreen_pass_from_template | pass | green | green | green |  | fixes recheck 21:08 |
| graph_auto_layout | pass | green | green | green |  | fixes recheck 21:09 |
| graph_canvas_zoom_pan | pass | green | green | green |  | full run |
| graph_edit_roundtrip | pass | green | green | green |  | full run |
| graph_fold_decisions | pass | green | green | green |  | UD-5 landing 2026-10-08 23:46 |
| graph_folded_features | pass | green | green | green |  | full run |
| graph_from_blank | pass | green | green | green |  | integration 2026-10-08 14:15 |
| graph_from_blank_forward | pass | red (B) | green | green | QD1-R11 | integration 2026-10-08 14:15 |
| graph_layers_origin | pass | green | green | green |  | full run |
| graph_loop_expand | pass | green | green | green |  | final check |
| graph_observe_roundtrip | pass | green | green | green |  | full run |
| graph_palette_kinds | pass | green | green | green |  | fixes recheck 21:29 |
| graph_read | pass | green | green | green |  | full run |
| graph_reroute_tie | pass | green | green | green |  | full run |
| graph_template_bloom | pass | green | green | green |  | full run |
| gui_assets_watch_verify | pass | green | green | green |  | UD-6 integration 2026-10-08 |
| gui_build_A | pass | red (B) | green | green | QD1-R14 | integration 2026-10-08 14:16 |
| gui_build_audio_reactive | pass | green | green | green |  | follow-up |
| gui_build_B | pass | red (B) | green | green | QD1-R15 | integration 2026-10-08 14:17 |
| gui_build_C | pass | red (B) | green | green | QD1-R16 | full run 2026-10-08 19:54 |
| gui_build_curve | pass | green | green | green |  | full run 2026-10-08 19:59 |
| gui_build_D | pass | red (B) | green | green | QD1-R18 | UD-5 landing 2026-10-08 23:48 |
| gui_build_E | pass | red (B) | green | green | QD1-R19 | integration 2026-10-08 14:18 |
| gui_build_full | pass | red (B) | green | green | QD1-R20 | UD-5 landing 2026-10-08 23:50 |
| gui_build_ltc | pass | red (B) | green | green | QD1-R21 | UD-6 integration 2026-10-08 |
| gui_build_music | pass | green | green | green |  | full run |
| gui_build_ocean | pass | green | green | green |  | full run 2026-10-08 20:07 |
| gui_build_ocean_compute | pass | green | green | green |  | full run 2026-10-08 20:11 |
| gui_build_optimize | red (unsteady step) | not measured | not run | not run | QD1-R24 | UD-5 landing 2026-10-08 23:55 |
| gui_build_rt_from_blank | pass | green | green | green |  | full run 2026-10-08 20:20 |
| gui_folded_pass_edit | pass | green | green | green |  | full run |
| gui_frameplan_delegates | pass | green | green | green |  | integration 2026-10-08 14:19 |
| gui_graph_folded_edits | pass | green | green | green |  | full run |
| gui_graph_new_target | pass | green | green | green |  | integration 2026-10-08 14:19 |
| gui_graph_only_edits | pass | green | green | green |  | full run 2026-10-08 20:23 |
| gui_graph_only_edits_2 | pass | green | green | green |  | integration 2026-10-08 14:20 |
| gui_graph_order | pass | green | green | green |  | full run |
| gui_graph_task_delete | pass | green | green | green |  | full run |
| gui_lower_layer_pin_freeze | pass | green | green | green |  | pilot |
| gui_material_from_blank | pass | green | green | green |  | fixes recheck 21:31 |
| gui_material_ray_query_from_blank | pass | red (D, F) | green | green | QD1-R3 | full run 2026-10-08 20:29 |
| gui_material_repair | pass | green | green | green |  | fixes recheck 21:32 |
| gui_model_from_file | pass | green | green | green |  | full run |
| gui_node_parameters | pass | green | green | green |  | full run |
| gui_problems_window | pass | N/A | green | green |  | full run |
| gui_render_fallback | pass | green | green | green |  | full run |
| gui_rt_checks | pass | green | green | green |  | fixes recheck 21:33 |
| gui_rt_full_from_blank | pass | green | green | green |  | fixes recheck 21:34 |
| gui_rt_hit_groups_from_blank | pass | red (F) | green | green | QD1-R33 | UD-5 landing 2026-10-08 23:57 |
| gui_rt_layout_edits | pass | green | green | green |  | fixes recheck 21:35 |
| gui_rt_procedural_callable | pass | green | green | green |  | UD-6 integration 2026-10-08 |
| gui_rt_two_rays_two_misses | pass | green | green | green |  | UD-6 integration 2026-10-08 |
| gui_settings_choose | pass | red (B) | green | green | QD1-R36 | UD-5 landing 2026-10-08 23:59 |
| input_needs_shader | pass | red (B) | green | green | QD1-R37 | UD-5 landing 2026-10-09 00:02 |
| lighting_pass_template | pass | green | green | green |  | fixes recheck 21:17 |
| loop_view_axis | pass | green | green | green |  | full run |
| loop_zone_edit | pass | green | green | green |  | fixes recheck 21:19 |
| loop_zone_read | pass | green | green | green |  | fixes recheck 21:19 |
| loop_zone_wire | pass | green | green | green |  | fixes recheck 21:20 |
| material_loop_decompose | pass | green | green | green |  | UD-5 landing 2026-10-09 00:02 |
| material_pass_authoring | pass | green | green | green |  | UD-5 landing 2026-10-09 00:03 |
| menu_actions | pass | green | green | green |  | pilot |
| new_blank_project | pass | green | green | green |  | full run |
| outliner_edit_roundtrip | pass | green | green | green |  | full run |
| play_mode_roundtrip | pass | green | green | green |  | full run |
| play_pause_step | pass | green | green | green |  | full run |
| play_runtime_objects | pass | green | green | green |  | full run |
| render_pipeline_from_scratch | pass | green | green | green |  | full run |
| render_reload_external | pass | green | green | green |  | final check |
| render_target_authoring | pass | green | green | green |  | full run |
| render_undo_roundtrip | pass | green | green | green |  | fixes recheck 21:38 |
| rendering_settings_from_blank | pass | green | green | green |  | pilot |
| sockets_fix_and_show | pass | red (B) | green | green | QD1-R44 | UD-5 landing 2026-10-09 00:04 |
| sockets_from_shader | pass | red (E) | green | green | QD1-R45 | full run 2026-10-08 21:04 |
| theme_switch | pass | green | green | green |  | full run |
| view_select_field | pass | green | green | green |  | full run |
| whole_frame_first_steps | pass | green | green | green |  | full run |

## Remaining findings

The diff excerpts below come from the executed reports (expected start/end versus found). They are deliberately short; the report holds the remaining keys and first diff lines.

### QD1-R5: add_primitive_object / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** Integration measured this on 2026-10-08 (UD-1 + UD-2 + UD-3 build). Ordinary assertions, reopen and health are green; Undo is `known_red`, and Redo has no compared differences. The file and render documents return to the start; only the scene keys differ.

Graph is walked before scene. Graph Undo returns the pipeline to blank and restarts the engine while scene Undo is still unconsumed. The engine loses its scene journal, so the later scene press cannot restore the start; the scene content itself remains. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08). The scene keys remain compared and this finding remains open.

Reproduce through ordinary step 56, then the `undo` epilogue. Evidence: `<build>/test_artifacts/studio_scenarios/add_primitive_object/report.json`, started `2026-10-08T14:12:48`.

`undo.log`: 2 Graph Undo presses moved documents. The walk records `pid 15980 -> 44508`, `restart_history_ready: true` after 1.06 s for `graph`, `render`. The later scene press reports `Undo failed: method_unavailable - actor has no committed transaction to undo`.

`undo.notes`: `scene: Undo was pressable but unconsumed before the engine restart; scene keys remain compared`

`undo_differs` keys and first diff lines (expected start versus found):

`scene:(answer)` (changed):

```diff
@@ -17,3 +17,3 @@
      "algorithm": "sha256",
-     "hex": "65164c470fcd5c5fb8a3eebacbd207d6d08bd06a6afe4ef579577c4e4d48357e"
+     "hex": "1463cc6a501c574b51cd897cf040c473da8ec7a01d73b8f47ec8d29a7532e51b"
     },
```

`scene:(semantic)` (changed):

```diff
@@ -50,2 +50,58 @@
      "name": "KeyLight"
+    },
+    {
+     "components": [
+      {
+       "name": "transform",
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R6: deferred_loop_from_scratch / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** Integration measured this on 2026-10-08 (UD-1 + UD-2 + UD-3 build). Ordinary assertions, reopen and health are green; Undo is `known_red`, and Redo has no compared differences. The file and render documents return to the start; only the scene keys differ.

Graph is walked before scene. Graph Undo returns the pipeline to blank and restarts the engine while scene Undo is still unconsumed. The engine loses its scene journal, so the later scene press cannot restore the start; the scene content itself remains. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08). The scene keys remain compared and this finding remains open.

Reproduce through ordinary step 98, then the `undo` epilogue. Evidence: `<build>/test_artifacts/studio_scenarios/deferred_loop_from_scratch/report.json`, started `2026-10-08T14:13:29`.

`undo.log`: 4 Graph Undo presses moved documents. The walk records `pid 18536 -> 51168`, `restart_history_ready: true` after 1.09 s for `graph`, `render`. The later scene press reports `Undo failed: method_unavailable - actor has no committed transaction to undo`.

`undo.notes`: `scene: Undo was pressable but unconsumed before the engine restart; scene keys remain compared`

`undo_differs` keys and first diff lines (expected start versus found):

`scene:(answer)` (changed):

```diff
@@ -17,3 +17,3 @@
      "algorithm": "sha256",
-     "hex": "65164c470fcd5c5fb8a3eebacbd207d6d08bd06a6afe4ef579577c4e4d48357e"
+     "hex": "88dedd002bb0b497516df1baeac1fdbf122cf7acc515b6d39d9ae9493eb1e348"
     },
```

`scene:(semantic)` (changed):

```diff
@@ -40,4 +40,4 @@
        "direction": [
-        -0.5,
-        -1.0,
+        0.5,
+        -1,
         -0.25
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R11: graph_from_blank_forward / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** Integration measured this on 2026-10-08 (UD-1 + UD-2 + UD-3 build). Ordinary assertions, reopen and health are green; Undo is `known_red`, and Redo has no compared differences. The file and render documents return to the start; only the scene keys differ.

Graph is walked before scene. Graph Undo returns the pipeline to blank and restarts the engine while scene Undo is still unconsumed. The engine loses its scene journal, so the later scene press cannot restore the start; the scene content itself remains. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08). The scene keys remain compared and this finding remains open.

Reproduce through ordinary step 69, then the `undo` epilogue. Evidence: `<build>/test_artifacts/studio_scenarios/graph_from_blank_forward/report.json`, started `2026-10-08T14:15:37`.

`undo.log`: 4 Graph Undo presses moved documents. The walk records `pid 11912 -> 52212`, `restart_history_ready: true` after 1.08 s for `graph`, `render`. The later scene press reports `Undo failed: method_unavailable - actor has no committed transaction to undo`.

`undo.notes`: `scene: Undo was pressable but unconsumed before the engine restart; scene keys remain compared`

`undo_differs` keys and first diff lines (expected start versus found):

`scene:(answer)` (changed):

```diff
@@ -17,3 +17,3 @@
      "algorithm": "sha256",
-     "hex": "65164c470fcd5c5fb8a3eebacbd207d6d08bd06a6afe4ef579577c4e4d48357e"
+     "hex": "7e5b4c472f22f14500c0ee1d440680f8f692dec4f98f00e9ef41ea2b8d678f44"
     },
```

`scene:(semantic)` (changed):

```diff
@@ -50,2 +50,30 @@
      "name": "KeyLight"
+    },
+    {
+     "components": [
+      {
+       "name": "transform",
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R14: gui_build_A / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** Integration measured this on 2026-10-08 (UD-1 + UD-2 + UD-3 build). Ordinary assertions, reopen and health are green; Undo is `known_red`, and Redo has no compared differences. The file and render documents return to the start; only the scene keys differ.

Graph is walked before scene. Graph Undo returns the pipeline to blank and restarts the engine while scene Undo is still unconsumed. The engine loses its scene journal, so the later scene press cannot restore the start; the scene content itself remains. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08). The scene keys remain compared and this finding remains open.

Reproduce through ordinary step 41, then the `undo` epilogue. Evidence: `<build>/test_artifacts/studio_scenarios/gui_build_A/report.json`, started `2026-10-08T14:16:28`.

`undo.log`: 2 Graph Undo presses moved documents. The walk records `pid 49572 -> 36504`, `restart_history_ready: true` after 1.08 s for `graph`, `render`. The later scene press reports `Undo failed: method_unavailable - actor has no committed transaction to undo`.

`undo.notes`: `scene: Undo was pressable but unconsumed before the engine restart; scene keys remain compared`

`undo_differs` keys and first diff lines (expected start versus found):

`scene:(answer)` (changed):

```diff
@@ -17,3 +17,3 @@
      "algorithm": "sha256",
-     "hex": "65164c470fcd5c5fb8a3eebacbd207d6d08bd06a6afe4ef579577c4e4d48357e"
+     "hex": "7e5b4c472f22f14500c0ee1d440680f8f692dec4f98f00e9ef41ea2b8d678f44"
     },
```

`scene:(semantic)` (changed):

```diff
@@ -50,2 +50,30 @@
      "name": "KeyLight"
+    },
+    {
+     "components": [
+      {
+       "name": "transform",
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R15: gui_build_B / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** Integration measured this on 2026-10-08 (UD-1 + UD-2 + UD-3 build). Ordinary assertions, reopen and health are green; Undo is `known_red`, and Redo has no compared differences. The file and render documents return to the start; only the scene keys differ.

Graph is walked before scene. Graph Undo returns the pipeline to blank and restarts the engine while scene Undo is still unconsumed. The engine loses its scene journal, so the later scene press cannot restore the start; the scene content itself remains. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08). The scene keys remain compared and this finding remains open.

Reproduce through ordinary step 69, then the `undo` epilogue. Evidence: `<build>/test_artifacts/studio_scenarios/gui_build_B/report.json`, started `2026-10-08T14:17:13`.

`undo.log`: 5 Graph Undo presses moved documents. The walk records `pid 35224 -> 46076`, `restart_history_ready: true` after 1.09 s for `graph`, `render`. The later scene press reports `Undo failed: method_unavailable - actor has no committed transaction to undo`.

`undo.notes`: `scene: Undo was pressable but unconsumed before the engine restart; scene keys remain compared`

`undo_differs` keys and first diff lines (expected start versus found):

`scene:(answer)` (changed):

```diff
@@ -17,3 +17,3 @@
      "algorithm": "sha256",
-     "hex": "65164c470fcd5c5fb8a3eebacbd207d6d08bd06a6afe4ef579577c4e4d48357e"
+     "hex": "7e5b4c472f22f14500c0ee1d440680f8f692dec4f98f00e9ef41ea2b8d678f44"
     },
```

`scene:(semantic)` (changed):

```diff
@@ -50,2 +50,30 @@
      "name": "KeyLight"
+    },
+    {
+     "components": [
+      {
+       "name": "transform",
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R16: gui_build_C / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** Full run at `33b761ecd` on 2026-10-08 (report started `2026-10-08T19:54:28`); the investigation (`<作業ディレクトリ>/qd_triage_report.md`, bundle B) found the same cause as R5 to R19. Ordinary assertions, reopen and health are green; the file and render documents return to the start and only the scene keys differ.

21 Graph Undo presses move documents. The last one returns the pipeline to blank and restarts the engine (`pid 50936 -> 39900`, `restart_history_ready: true` after 1.09 s) while the scene Undo for the Outliner's Cube (step 24) and the Inspector's intensity (step 32) is still unconsumed. The later scene press reports `Undo failed: method_unavailable - actor has no committed transaction to undo`; the scene content itself remains. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08).

`undo.notes`: `scene: Undo was pressable but unconsumed before the engine restart; scene keys remain compared`

Reproduce through ordinary step 164, then the `undo` epilogue.

`scene:(semantic)` (expected start versus found):

```diff
@@ -44,3 +44,3 @@
        ],
-       "intensity": 1.0,
+       "intensity": 6,
        "name": "light",
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R18: gui_build_D / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** UD-5 landing check on 2026-10-09 (`<作業ディレクトリ>/ud5_work/w1_final/gui_build_D.json`, started `2026-10-08T23:48:51`). Ordinary assertions, reopen and health are green. Undo returns every compared key to the start (`undo_differs` is empty).

The walk presses 6 Graph, 1 Render and 2 scene Undo. Scene Undo is pressed before the Graph Undo that removes the last material pass; that Graph Undo restarts the engine (`pid 48760 -> 38480`, the driver answers the unsaved-scene question with Save, `restart_history_ready: true` after 1.09 s). The restart drops the scene journal, including the entries already undone, so every scene Redo reports `Redo failed: method_unavailable - actor has no successful revert to redo`. After Redo only `scene:(answer)` and `scene:(semantic)` differ. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08).

Reproduce through ordinary step 99, then the `undo` epilogue.

`scene:(semantic)` after Redo (expected endpoint versus found):

```diff
@@ -50,58 +50,2 @@
      "name": "KeyLight"
-    },
-    {
-     "components": [
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R19: gui_build_E / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** Integration measured this on 2026-10-08 (UD-1 + UD-2 + UD-3 build). Ordinary assertions, reopen and health are green; Undo is `known_red`, and Redo has no compared differences. The file and render documents return to the start; only the scene keys differ.

Graph is walked before scene. Graph Undo returns the pipeline to blank and restarts the engine while scene Undo is still unconsumed. The engine loses its scene journal, so the later scene press cannot restore the start; the scene content itself remains. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08). The scene keys remain compared and this finding remains open.

Reproduce through ordinary step 69, then the `undo` epilogue. Evidence: `<build>/test_artifacts/studio_scenarios/gui_build_E/report.json`, started `2026-10-08T14:18:09`.

`undo.log`: 4 Graph Undo presses moved documents. The walk records `pid 49936 -> 49240`, `restart_history_ready: true` after 1.08 s for `graph`, `render`. The later scene press reports `Undo failed: method_unavailable - actor has no committed transaction to undo`.

`undo.notes`: `scene: Undo was pressable but unconsumed before the engine restart; scene keys remain compared`

`undo_differs` keys and first diff lines (expected start versus found):

`scene:(answer)` (changed):

```diff
@@ -17,3 +17,3 @@
      "algorithm": "sha256",
-     "hex": "65164c470fcd5c5fb8a3eebacbd207d6d08bd06a6afe4ef579577c4e4d48357e"
+     "hex": "db9635dd26647a75fb9d0f4d2012d4bf198b14639c52f331397c437fa99da9ad"
     },
```

`scene:(semantic)` (changed):

```diff
@@ -50,2 +50,58 @@
      "name": "KeyLight"
+    },
+    {
+     "components": [
+      {
+       "name": "transform",
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R20: gui_build_full / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** UD-5 landing check on 2026-10-09 (`<作業ディレクトリ>/ud5_work/w1_final/gui_build_full.json`, started `2026-10-08T23:50:07`). Ordinary assertions, reopen and health are green. Undo returns every compared key to the start (`undo_differs` is empty).

The walk presses 32 Graph, 1 Render and 3 scene Undo. Scene Undo is pressed before the Graph Undo that removes the last material pass; that Graph Undo restarts the engine (`pid 55048 -> 54084`, the driver answers the unsaved-scene question with Save, `restart_history_ready: true` after 1.11 s). The restart drops the scene journal, including the entries already undone, so every scene Redo reports `Redo failed: method_unavailable - actor has no successful revert to redo`. After Redo only `scene:(answer)` and `scene:(semantic)` differ. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08).

Reproduce through ordinary step 284, then the `undo` epilogue.

`scene:(semantic)` after Redo (expected endpoint versus found):

```diff
@@ -44,3 +44,3 @@
        ],
-       "intensity": 6,
+       "intensity": 1.0,
        "name": "light",
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R21: gui_build_ltc / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** Before UD-6, the image-only external reload at step 189 (`ltc_2.ktx2` replaced outside Studio) cut the Graph and Render histories (bundle C), so the render documents never returned. UD-6 keeps both histories across such a reload. In the UD-6 integration run on 2026-10-08 the file and render documents return to the start and only `scene:(answer)` and `scene:(semantic)` differ: the Graph Undo that removes the last material pass restarts the engine and drops the scene journal. Reopen and health are green. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08).

Reproduce through ordinary step 192, then the `undo` epilogue. The report of that run was not kept; rerun the case for a current excerpt.

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R24: gui_build_optimize / undo
**Unsteady ordinary step; Undo not measured (2026-10-09).** In the UD-5 landing check (`<作業ディレクトリ>/ud5_work/w1_final/gui_build_optimize.json`, started `2026-10-08T23:55:04`) and on the primary build at `33b761ecd` before UD-5 (report started `2026-10-09T00:06:49`) the ordinary steps stop at report index 318, `the loop's Layers` (`Layers: render_fullscreen_4` instead of `Layers: loop`), right after the `pick the loop` click (index 317), whose effect varies with the canvas layout. The `undo` epilogue did not run, so this mark is neither confirmed nor cleared; it stays until a run reaches the epilogue. The step is listed in the engine backlog.

Reproduce through ordinary step 495, then the `undo` epilogue. The last measured Undo difference (2026-10-05) was in `file:passes/main_rendering_config.json` and `render:passes/main_rendering_config.json`.

### QD1-R3: gui_material_ray_query_from_blank / undo
**Limits D and F (documented design limits).** Full run at `33b761ecd` on 2026-10-08 (report started `2026-10-08T20:29:29`). Ordinary assertions, reopen and health are green.

D: step 57 creates a material whose surface queries the acceleration structure, and step 104 repairs the pass by turning Ray query on. Undoing that repair would leave the remaining material incompatible, so the engine refuses it twice (`Material pass 'render_material_pass' uses an acceleration structure but does not declare…`), and the Render `Undo Edit pass present` behind it reports `changed since`. The repair can be undone only after the material is removed or changed.

F: New material is not in Materials Undo by design (`test/devstudio_materials_test.cpp:701`; [README](README.md), the Materials dock section), so the material file, its registry entry and `list_materials` remain. Only the Material row assignment moves (1 press); a Material row Undo has no Redo, so the Redo half is not covered.

Reproduce through ordinary step 118, then the `undo` epilogue.

Undo differs: `file:assets/asset_data.json`, `file:materials/material.material.json` (added), `file:passes/main_rendering_config.json`, `list_materials:`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,2 +1,7 @@
 {
+ "materials": [
+  {
+   "path": "materials/material.material.json"
```

### QD1-R33: gui_rt_hit_groups_from_blank / undo
**Limit F (New material is outside Undo).** UD-5 landing check (`<作業ディレクトリ>/ud5_work/w1_final/gui_rt_hit_groups_from_blank.json`, started `2026-10-08T23:57:57`). Ordinary assertions, reopen and health are green.

Render (5 presses, including the ray layout edits; one restarts the engine, `pid 56228 -> 49896`), Material row (2) and Graph (1) Undo return the pass documents to the start. The remaining differences are the two materials created with New material at steps 33 and 39, which is not in Materials Undo by design (`test/devstudio_materials_test.cpp:701`; [README](README.md), the Materials dock section). A Material row Undo has no Redo, so the Redo half is not covered.

Reproduce through ordinary step 134, then the `undo` epilogue.

Undo differs: `file:assets/asset_data.json`, `file:materials/material.material.json` (added), `file:materials/material_2.material.json` (added), `list_materials:`.

```diff
@@ -1,2 +1,10 @@
 {
+ "materials": [
+  {
+   "path": "materials/material.material.json"
```

### QD1-R36: gui_settings_choose / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** UD-5 landing check on 2026-10-09 (`<作業ディレクトリ>/ud5_work/w1_final/gui_settings_choose.json`, started `2026-10-08T23:59:10`). Ordinary assertions, reopen and health are green. Undo returns every compared key to the start (`undo_differs` is empty).

The walk presses 7 Graph, 9 Render and 3 scene Undo. Scene Undo is pressed before the Graph Undo that removes the last material pass; that Graph Undo restarts the engine (`pid 49688 -> 38824`, the driver answers the unsaved-scene question with Save, `restart_history_ready: true` after 1.06 s). The restart drops the scene journal, including the entries already undone, so every scene Redo reports `Redo failed: method_unavailable - actor has no successful revert to redo`. After Redo only `scene:(answer)` and `scene:(semantic)` differ. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08).

Reproduce through ordinary step 206, then the `undo` epilogue.

`scene:(semantic)` after Redo (expected endpoint versus found):

```diff
@@ -40,5 +40,5 @@
        "direction": [
-        0.4364723549092421,
-        -0.8728369164014821,
-        -0.21828330391198672
+        -0.5,
+        -1.0,
+        -0.25
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R37: input_needs_shader / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** UD-5 landing check on 2026-10-09 (`<作業ディレクトリ>/ud5_work/w1_final/input_needs_shader.json`, started `2026-10-09T00:02:01`). Ordinary assertions, reopen and health are green. Undo returns every compared key to the start (`undo_differs` is empty).

The walk presses 2 Graph, 1 Render and 1 scene Undo. Scene Undo is pressed before the Graph Undo that removes the last material pass; that Graph Undo restarts the engine (`pid 56560 -> 50000`, the driver answers the unsaved-scene question with Save, `restart_history_ready: true` after 1.08 s). The restart drops the scene journal, including the entries already undone, so every scene Redo reports `Redo failed: method_unavailable - actor has no successful revert to redo`. After Redo only `scene:(answer)` and `scene:(semantic)` differ. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08).

Reproduce through ordinary step 71, then the `undo` epilogue.

`scene:(semantic)` after Redo (expected endpoint versus found):

```diff
@@ -50,30 +50,2 @@
      "name": "KeyLight"
-    },
-    {
-     "components": [
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R44: sockets_fix_and_show / undo
**Bundle B: engine restart inside the Undo walk / scene journal (v1, user decision 2026-10-08).** UD-5 landing check on 2026-10-09 (`<作業ディレクトリ>/ud5_work/w1_final/sockets_fix_and_show.json`, started `2026-10-09T00:04:49`). Ordinary assertions, reopen and health are green. Undo returns every compared key to the start (`undo_differs` is empty).

The walk presses 1 Graph, 4 Render and 1 scene Undo. Scene Undo is pressed before the Graph Undo that removes the last material pass; that Graph Undo restarts the engine (`pid 36972 -> 44652`, the driver answers the unsaved-scene question with Save, `restart_history_ready: true` after 1.06 s). The restart drops the scene journal, including the entries already undone, so every scene Redo reports `Redo failed: method_unavailable - actor has no successful revert to redo`. After Redo only `scene:(answer)` and `scene:(semantic)` differ. Carrying the journal across the restart is deferred to v1 (user decision, 2026-10-08).

Reproduce through ordinary step 96, then the `undo` epilogue.

`scene:(semantic)` after Redo (expected endpoint versus found):

```diff
@@ -50,30 +50,2 @@
      "name": "KeyLight"
-    },
-    {
-     "components": [
```

The ticket remains until the scene journal is carried across a restart (v1). No invariant exemption or pixel expectation was changed.

### QD1-R45: sockets_from_shader / undo
**Limit E (a connection made against an externally edited shader).** Full run at `33b761ecd` on 2026-10-08 (report started `2026-10-08T21:04:28`). Ordinary assertions, reopen and health are green.

Step 75 removes binding 1 from the shader in an external editor, and step 83's Disconnect brings the document in line with it. The top Render entry, `Undo Edit pass present`, would restore the connection to binding 1, which the shader no longer declares, so the engine refuses it twice (`Fullscreen pass input texture/local-read contract does not match shader reflection: …`); Graph Undo waits behind that newer Render edit (`A later render edit from another panel is on top.`). Only scene Undo/Redo moves (1 press each). The connection can be undone only after the shader's binding is restored.

Reproduce through ordinary step 94, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,4 +1,223 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
```

## Explicit coverage limits

- Undo is walked by kind (Graph, Render, Materials, Material row, Assets, Scene); there is no unified timestamped global history. A blocked kind is retried after another kind changes documents. Redo replays the successful Undo sequence in reverse. Existing Redo at the ordinary endpoint is preserved rather than drained beyond that endpoint.
- Materials, Material row and Assets do not expose Redo. If one of these histories moves, the report explicitly says the Redo half was not covered.
- Saved scene files, external-editor writes, newly copied/imported assets, `layout.frame_plan`, and scene journals lost across an engine restart have named exclusions in the report. The in-memory scene is still compared where its history survives.
- A project switch/reopen establishes a new baseline. `gui_problems_window` ends outside the captured project scope and reports Undo as not applicable.
- Save/reopen compares the state after the history walk. Where Redo is unavailable, this is not a claim that the original ordinary endpoint was restored.
- Animated cases explicitly disable only pixel equality, with a reason. The default is nine stable RGB samples, tolerance 3; document checks still run.
- Scoped scenario exemptions cover Update Manifest and deliberate external graph/audio reloads. Deliberate shader/material parse failures and contract refusals have narrow reasoned log patterns. Unexpected RT teardown and endpoint shader failures stay red.

## Checker/scenario repairs and runtime

- Object-key assertions use `has_key` / `lacks_key` (10 conversions). The observational frame-plan read declares `record: true`. Disappearance checks use `missing: true`; hidden/disabled require a real widget.
- Canvas zoom retains relative gesture checks without assuming initial scale 1. Refresh clicks wait for enabled controls. The menu-only scenario waits for its engine.
- Fixed automation viewports reserve space in the collapsible container, preventing the Inspector from contaminating sampled pixels after layout reset.
- The checker rejects missing/malformed pixels, failed Save, unsettled documents, undeclared unstable pictures and unknown invariant selections. Checker exceptions and incomplete history traversals cannot be accepted as known product findings.
- Hidden but enabled Material Undo is exercised. Previously blocked history is retried after document changes. Existing endpoint Redo is not consumed. These repairs removed false failures, while exposing a stale Material history after project reopen.
- Paired timings for 83 cases: baseline without epilogues 2574.36 s; full-run case durations 5103.49 s, **1.98x**. The 84-case full CTest wall time was 5212.34 s. Targeted reruns are additional validation, not a second full-run benchmark.
- During development, select affected scenarios with `-R`; keep the full serial suite for integration. If reducing reopen coverage to Blank-building cases is later desired, make it an explicit policy change. This change keeps default coverage.
- No new end-user text-entry field or production engine behavior is introduced. The complete non-Studio repository release gate was not rerun in this QD-1 validation.

## Resolved findings

### QD1-R8: fullscreen_pass_from_template / undo

**Resolved in the 2026-10-05 fixes.** Checker correction: removing the explicitly exempt layout.frame_plan subtree also removes its empty ancestor containers. Siblings and unrelated empty objects remain compared. Verified `fullscreen_pass_from_template` / `undo` green at 2026-10-05T21:08:29.

Original reproduction (retained as evidence):

**Serialization / empty metadata.** Undo leaves only an empty `layout` object after the explicitly exempt `layout.frame_plan` subtree is removed. This is a strict document-restoration difference, not evidence of a rendering failure.

Reproduce through ordinary step 79, then the `undo` epilogue.

Undo differs: `file:passes/main.json`, `render:passes/main.json`.

```diff
@@ -39,2 +39,3 @@
  },
+ "layout": {},
  "nodes": [
```

### QD1-R9: graph_auto_layout / undo

**Resolved in the 2026-10-05 fixes.** Checker correction: removing the explicitly exempt layout.frame_plan subtree also removes its empty ancestor containers. Siblings and unrelated empty objects remain compared. Verified `graph_auto_layout` / `undo` green at 2026-10-05T21:09:55.

Original reproduction (retained as evidence):

**Serialization / empty metadata.** Undo leaves only an empty `layout` object after the explicitly exempt `layout.frame_plan` subtree is removed. This is a strict document-restoration difference, not evidence of a rendering failure.

Reproduce through ordinary step 11, then the `undo` epilogue.

Undo differs: `file:passes/main.json`, `render:passes/main.json`.

```diff
@@ -39,2 +39,3 @@
  },
+ "layout": {},
  "nodes": [
```

### QD1-R12: graph_palette_kinds / undo

**Resolved in the 2026-10-05 fixes.** Graph Redo survives a confirmed Render-history restore whose exact digest and graph contents match; unrelated writes still invalidate history. Verified `graph_palette_kinds` / `undo` green at 2026-10-05T21:29:26.

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 61, then the `undo` epilogue.

Redo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -2,126 +2,3 @@
  "definitions": {},
- "nodes": [
-  {
-   "id": "ux3_scene_ref",
-   "op": "render.target_ref",
-   "outputs": [
-    "resource"
```

### QD1-R29: gui_material_from_blank / undo

**Resolved in the 2026-10-05 fixes.** Material assignment history and pending replies are cleared on project open; same-project engine restarts retain history. Verified `gui_material_from_blank` / `undo` green at 2026-10-05T21:31:16.

Original reproduction (retained as evidence):

**Studio / history lifetime.** The scenario reopens its original project after `project_control`. The Material row still exposes 3 old Undo entries; invoking them removes bindings that existed at the new start snapshot. `MaterialAssignSession::engineReady` and `engineGone` do not clear that history.

Reproduce through ordinary step 253, then the `undo` epilogue.

Undo differs: `file:assets/asset_data.json`, `file:materials/cube.bindings.json`, `list_materials:`.

```diff
@@ -9,9 +9,3 @@
  ],
- "models": [
-  {
-   "material_bindings": "materials/cube.bindings.json",
-   "name": "engine://primitives/cube",
-   "path": "engine://primitives/cube"
-  }
```

Redo coverage limit: material_row: a Material row's Undo (Every <model>) has no Redo.

### QD1-R30: gui_material_repair / undo

**Resolved in the 2026-10-05 fixes.** The Materials dock model-binding row now adopts its inverse into the visible dock history. The scenario verifies deletion Undo followed by binding Undo. Verified `gui_material_repair` / `undo` green at 2026-10-05T21:32:27.

Original reproduction (retained as evidence):

**Studio / binding history.** The Materials dock removes a binding during ordinary steps. The enabled Material-row Undo is exercised even with its parent hidden, but the binding and asset-data entry are not restored. Exact ownership of this missing inverse remains to be isolated.

Reproduce through ordinary step 205, then the `undo` epilogue.

Undo differs: `file:assets/asset_data.json`, `file:materials/cube.bindings.json`, `list_materials:`.

```diff
@@ -6,9 +6,3 @@
  ],
- "models": [
-  {
-   "material_bindings": "materials/cube.bindings.json",
-   "name": "engine://primitives/cube",
-   "path": "engine://primitives/cube"
-  }
```

### QD1-R31: gui_rt_checks / undo

**Resolved in the 2026-10-05 fixes.** Procedural record inverses belong to each history entry, not an output digest. Opaque JSON text preserves numeric types through Qt transport; repeated Undo/Redo and restart are covered. Verified `gui_rt_checks` / `undo` green at 2026-10-05T21:33:31.

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 125, then the `undo` epilogue.

Undo differs: `file:assets/asset_data.json`, `list_materials:`.

```diff
@@ -20,3 +20,3 @@
    "record": {
-    "value": 0.0
+    "value": 1
    }
```

### QD1-H1: gui_rt_checks / health

**Resolved in the 2026-10-05 fixes.** Resource owners are initialized before the model container, including an initially empty project with later model loads, so reverse destruction order keeps them alive during model release. Shader retirement also remaps failed reload-unit indices and removes only retired shaders from Problems, retaining surviving failure details. Verified `gui_rt_checks` / `health` green at 2026-10-05T21:33:31.

Original reproduction (retained as evidence):

**Engine teardown error.** `project_control/player.log` records `model container teardown release` with `stage=material` and `stage=texture`, both `code=owner_unavailable`. At the ordinary endpoint, Problems also contains `shader_reload_failed@1` for `project://shaders/start.rchit`; this is distinct from the intentionally rejected wrong_record shader. These errors are not allowlisted. The cases may share one underlying defect.

Reproduce through ordinary step 125, then the `health` epilogue.

Observed at the endpoint / project close: Problems holds 1, the scenario declares 0: error: shader_reload_failed@1: Shader compile failed: project://shaders/start.rchit / 2 error line(s) in the logs, the first: 19:23:07.550686969 [43732] modelresourcerelease.cpp:20  LOG_ERROR     pelican      model container teardown release: stage=material code=owner_unavailable index=1

### QD1-R32: gui_rt_full_from_blank / undo

**Resolved in the 2026-10-05 fixes.** Material assignment history and pending replies are cleared on project open; same-project engine restarts retain history. Verified `gui_rt_full_from_blank` / `undo` green at 2026-10-05T21:34:47.

Original reproduction (retained as evidence):

**Studio / history lifetime.** The scenario reopens its original project after `project_control`. The Material row still exposes 2 old Undo entries; invoking them removes bindings that existed at the new start snapshot. `MaterialAssignSession::engineReady` and `engineGone` do not clear that history.

Reproduce through ordinary step 249, then the `undo` epilogue.

Undo differs: `file:assets/asset_data.json`, `file:materials/cube.bindings.json`, `file:materials/sphere.bindings.json`, `list_materials:`.

```diff
@@ -9,14 +9,3 @@
  ],
- "models": [
-  {
-   "material_bindings": "materials/cube.bindings.json",
-   "name": "engine://primitives/cube",
-   "path": "engine://primitives/cube"
-  },
```

Redo coverage limit: material_row: a Material row's Undo (Every <model>) has no Redo.

### QD1-H2: gui_rt_layout_edits / health

**Resolved in the 2026-10-05 fixes.** Resource owners are initialized before the model container, including an initially empty project with later model loads, so reverse destruction order keeps them alive during model release. Verified `gui_rt_layout_edits` / `health` green at 2026-10-05T21:35:57.

Original reproduction (retained as evidence):

**Engine teardown error.** `project_control/player.log` records `model container teardown release` with `stage=material` and `stage=texture`, both `code=owner_unavailable`. These errors are not allowlisted. The cases may share one underlying defect.

Reproduce through ordinary step 82, then the `health` epilogue.

Observed at the endpoint / project close: 2 error line(s) in the logs, the first: 18:25:45.577283444 [38844] modelresourcerelease.cpp:20  LOG_ERROR     pelican      model container teardown release: stage=material code=owner_unavailable index=1

### QD1-H3: gui_rt_procedural_callable / health

**Resolved in the 2026-10-05 fixes.** Resource owners are initialized before the model container, including an initially empty project with later model loads, so reverse destruction order keeps them alive during model release. Verified `gui_rt_procedural_callable` / `health` green at 2026-10-05T21:36:45.

Original reproduction (retained as evidence):

**Engine teardown error.** `project_control/player.log` records `model container teardown release` with `stage=material` and `stage=texture`, both `code=owner_unavailable`. These errors are not allowlisted. The cases may share one underlying defect.

Reproduce through ordinary step 42, then the `health` epilogue.

Observed at the endpoint / project close: 2 error line(s) in the logs, the first: 18:26:17.196852342 [9584] modelresourcerelease.cpp:20  LOG_ERROR     pelican      model container teardown release: stage=material code=owner_unavailable index=1

### QD1-H4: gui_rt_two_rays_two_misses / health

**Resolved in the 2026-10-05 fixes.** Resource owners are initialized before the model container, including an initially empty project with later model loads, so reverse destruction order keeps them alive during model release. Verified `gui_rt_two_rays_two_misses` / `health` green at 2026-10-05T21:37:34.

Original reproduction (retained as evidence):

**Engine teardown error.** `project_control/player.log` records `model container teardown release` with `stage=material` and `stage=texture`, both `code=owner_unavailable`. These errors are not allowlisted. The cases may share one underlying defect.

Reproduce through ordinary step 48, then the `health` epilogue.

Observed at the endpoint / project close: 2 error line(s) in the logs, the first: 18:26:48.667943032 [4776] modelresourcerelease.cpp:20  LOG_ERROR     pelican      model container teardown release: stage=material code=owner_unavailable index=1

### QD1-R38: lighting_pass_template / undo

**Resolved in the 2026-10-05 fixes.** Checker correction: removing the explicitly exempt layout.frame_plan subtree also removes its empty ancestor containers. Siblings and unrelated empty objects remain compared. Verified `lighting_pass_template` / `undo` green at 2026-10-05T21:17:08.

Original reproduction (retained as evidence):

**Serialization / empty metadata.** Undo leaves only an empty `layout` object after the explicitly exempt `layout.frame_plan` subtree is removed. This is a strict document-restoration difference, not evidence of a rendering failure.

Reproduce through ordinary step 85, then the `undo` epilogue.

Undo differs: `file:passes/main.json`, `render:passes/main.json`.

```diff
@@ -39,2 +39,3 @@
  },
+ "layout": {},
  "nodes": [
```

### QD1-R39: loop_zone_edit / undo

**Resolved in the 2026-10-05 fixes.** Checker correction: removing the explicitly exempt layout.frame_plan subtree also removes its empty ancestor containers. Siblings and unrelated empty objects remain compared. Verified `loop_zone_edit` / `undo` green at 2026-10-05T21:19:05.

Original reproduction (retained as evidence):

**Serialization / empty metadata.** Undo leaves only an empty `layout` object after the explicitly exempt `layout.frame_plan` subtree is removed. This is a strict document-restoration difference, not evidence of a rendering failure.

Reproduce through ordinary step 66, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -2,2 +2,3 @@
  "definitions": {},
+ "layout": {},
  "nodes": [],
```

### QD1-R40: loop_zone_read / undo

**Resolved in the 2026-10-05 fixes.** Checker correction: removing the explicitly exempt layout.frame_plan subtree also removes its empty ancestor containers. Siblings and unrelated empty objects remain compared. Verified `loop_zone_read` / `undo` green at 2026-10-05T21:19:40.

Original reproduction (retained as evidence):

**Serialization / empty metadata.** Undo leaves only an empty `layout` object after the explicitly exempt `layout.frame_plan` subtree is removed. This is a strict document-restoration difference, not evidence of a rendering failure.

Reproduce through ordinary step 94, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -2,2 +2,3 @@
  "definitions": {},
+ "layout": {},
  "nodes": [],
```

### QD1-R41: loop_zone_wire / undo

**Resolved in the 2026-10-05 fixes.** Checker correction: removing the explicitly exempt layout.frame_plan subtree also removes its empty ancestor containers. Siblings and unrelated empty objects remain compared. Verified `loop_zone_wire` / `undo` green at 2026-10-05T21:20:41.

Original reproduction (retained as evidence):

**Serialization / empty metadata.** Undo leaves only an empty `layout` object after the explicitly exempt `layout.frame_plan` subtree is removed. This is a strict document-restoration difference, not evidence of a rendering failure.

Reproduce through ordinary step 77, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -2,2 +2,3 @@
  "definitions": {},
+ "layout": {},
  "nodes": [],
```

### QD1-R43: render_undo_roundtrip / undo

**Resolved in the 2026-10-05 fixes.** Graph history survives a confirmed Render-history restore whose exact digest and graph contents match, allowing the mixed Undo/Redo sequence to finish without losing its remaining entries. Verified `render_undo_roundtrip` / `undo` green at 2026-10-05T21:38:30.

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 136, then the `undo` epilogue.

Undo differs: `file:passes/main.json`, `render:passes/main.json`.

```diff
@@ -2,2 +2,7 @@
  "definitions": {},
+ "layout": {
+  "nodes": {},
+  "reroutes": [],
+  "ties": []
+ },
  "nodes": [],
```

Redo differs: `file:passes/main.json`, `render:passes/main.json`.

```diff
@@ -7,22 +7,3 @@
  },
- "nodes": [
-  {
-   "id": "render_target_ref",
-   "op": "render.target_ref",
-   "outputs": [
-    "resource"
```


### QD1-R1: graph_from_blank / undo

**Resolved (UD-1), 2026-10-08.** Removing the last material pass commits for an engine restart. The one prescribed scenario run verified Undo/Redo, reopen and health green.

Original reproduction (retained as evidence):

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the deferred_geometry route, which pelican.builtin.transparent (engine buil…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 48, then the `undo` epilogue.

Observed panel notices: `No pass draws the deferred_geometry route, which pelican.builtin.transparent (engine buil…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,4 +1,223 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "render_material_pass_draw": {
+   "nodes": [
+    {
```


### QD1-R7: frameplan_layout_then_edit / undo

**Resolved (UD-1), 2026-10-08.** Removing the last material pass commits for an engine restart. The one prescribed scenario run verified Undo/Redo, reopen and health green.

Original reproduction (retained as evidence):

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the deferred_geometry route, which pelican.builtin.transparent (engine buil…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 50, then the `undo` epilogue.

Observed panel notices: `No pass draws the deferred_geometry route, which pelican.builtin.transparent (engine buil…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,4 +1,228 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "render_material_pass_draw": {
+   "nodes": [
+    {
```


### QD1-R2: gui_frameplan_delegates / undo

**Resolved (UD-3), 2026-10-08.** Undoing the Graph palette entry that adds a forward material pass now restores `material_route_default` (null removes a default that was absent) in the same `edit_graph_document` as the pass removal, so the engine restarts once. The integration rerun (report started `2026-10-08T14:19:02`) verified Undo/Redo, reopen and health green.

Original reproduction (UD-1 + UD-2 build, retained as evidence):

**Studio / Graph history inverse(route default).** Integration measured this on 2026-10-08 (UD-1 + UD-2 build). Ordinary assertions, reopen and health are green; Undo is `known_red`, and Redo has no compared differences.

The palette addition sends `material_route_default: "forward"` with its pass in one Graph history entry, but the inverse sent only graph ops. Undo removed the pass and left the root default forward. The history was missing the root inverse.

Reproduce through ordinary step 69, then the `undo` epilogue. Evidence: `<build>/test_artifacts/studio_scenarios/gui_frameplan_delegates/report.json`, started `2026-10-08T13:45:32`.

`undo.log`: 2 Graph Undo presses moved documents. The walk records `pid 29068 -> 4744`, `restart_history_ready: true` after 1.08 s for `graph`, `render`. The later scene press reports `Undo failed: method_unavailable - actor has no committed transaction to undo`.

`undo.notes`: empty.

`undo_differs` keys and first diff lines (expected start versus found):

`file:passes/main_rendering_config.json` (changed):

```diff
@@ -5,2 +5,3 @@
  "rendering": {
+  "material_route_default": "forward",
   "rendering_passes": [
```

`render:passes/main_rendering_config.json` (changed):

```diff
@@ -5,2 +5,3 @@
  "rendering": {
+  "material_route_default": "forward",
   "rendering_passes": [
```


### QD1-R26: gui_graph_new_target / undo

**Resolved (UD-3), 2026-10-08.** Undoing the Graph palette entry that adds a forward material pass now restores `material_route_default` (null removes a default that was absent) in the same `edit_graph_document` as the pass removal, so the engine restarts once. The integration rerun (report started `2026-10-08T14:19:47`) verified Undo/Redo, reopen and health green.

Original reproduction (UD-1 + UD-2 build, retained as evidence):

**Studio / Graph history inverse(route default).** Integration measured this on 2026-10-08 (UD-1 + UD-2 build). Ordinary assertions, reopen and health are green; Undo is `known_red`, and Redo has no compared differences.

The palette addition sends `material_route_default: "forward"` with its pass in one Graph history entry, but the inverse sent only graph ops. Undo removed the pass and left the root default forward. The history was missing the root inverse.

The authored render-target fragment and its root `features` reference also remain. After the Graph restart, Render Undo refuses `Add render target render_target` because `passes/main_rendering_config.json changed since. Undo the later edit first, or refresh.` The remaining root default prevents the earlier target edit from passing CAS; this report does not establish a separate target inverse defect.

Reproduce through ordinary step 60, then the `undo` epilogue. Evidence: `<build>/test_artifacts/studio_scenarios/gui_graph_new_target/report.json`, started `2026-10-08T13:42:39`.

`undo.log`: 3 Graph Undo presses moved documents. The walk records `pid 43524 -> 45400`, `restart_history_ready: true` after 1.06 s for `graph`, `render`. The later graph press reports `Graph change saved.`. The later render press reports `"Add render target render_target" cannot be undone: passes/main_rendering_config.json changed since. Undo the later edit first, or refresh.`.

`undo.notes`: empty.

`undo_differs` keys and first diff lines (expected start versus found):

`file:passes/authoring/render-targets.json` (added):

```diff
@@ -0,0 +1,24 @@
+{
+  "schema": "pelican.render_feature",
+  "version": 1,
+  "name": "authored_render_targets",
+  "runtime_shader_compiler": "optional",
+  "pelican_editor_managed": {
+    "schema": "pelican.authored_render_fragment",
```

`file:passes/main_rendering_config.json` (changed):

```diff
@@ -5,2 +5,6 @@
  "rendering": {
+  "features": [
+   "project://passes/authoring/render-targets.json"
+  ],
+  "material_route_default": "forward",
   "rendering_passes": [
```

`render:passes/main_rendering_config.json` (changed):

```diff
@@ -5,2 +5,6 @@
  "rendering": {
+  "features": [
+   "project://passes/authoring/render-targets.json"
+  ],
+  "material_route_default": "forward",
   "rendering_passes": [
```

`render:project://passes/authoring/render-targets.json` (added):

```diff
@@ -0,0 +1,24 @@
+{
+ "name": "authored_render_targets",
+ "pelican_editor_managed": {
+  "id": "render-targets",
+  "owner": "pelican.devstudio",
+  "schema": "pelican.authored_render_fragment",
+  "version": 1
```


### QD1-R28: gui_graph_only_edits_2 / undo

**Resolved (UD-3), 2026-10-08.** Undoing the Graph palette entry that adds a forward material pass now restores `material_route_default` (null removes a default that was absent) in the same `edit_graph_document` as the pass removal, so the engine restarts once. The integration rerun (report started `2026-10-08T14:20:30`) verified Undo/Redo, reopen and health green.

Original reproduction (UD-1 + UD-2 build, retained as evidence):

**Studio / Graph history inverse(route default).** Integration measured this on 2026-10-08 (UD-1 + UD-2 build). Ordinary assertions, reopen and health are green; Undo is `known_red`, and Redo has no compared differences.

The palette addition sends `material_route_default: "forward"` with its pass in one Graph history entry, but the inverse sent only graph ops. Undo removed the pass and left the root default forward. The history was missing the root inverse.

Reproduce through ordinary step 103, then the `undo` epilogue. Evidence: `<build>/test_artifacts/studio_scenarios/gui_graph_only_edits_2/report.json`, started `2026-10-08T13:43:20`.

`undo.log`: 12 Graph Undo presses moved documents. The walk records `pid 41016 -> 42008`, `restart_history_ready: true` after 1.06 s for `graph`, `render`. The later scene press reports `Undo failed: method_unavailable - actor has no committed transaction to undo`.

`undo.notes`: empty.

`undo_differs` keys and first diff lines (expected start versus found):

`file:passes/main_rendering_config.json` (changed):

```diff
@@ -5,2 +5,3 @@
  "rendering": {
+  "material_route_default": "forward",
   "rendering_passes": [
```

`render:passes/main_rendering_config.json` (changed):

```diff
@@ -5,2 +5,3 @@
  "rendering": {
+  "material_route_default": "forward",
   "rendering_passes": [
```


### QD1-R10: graph_fold_decisions / undo

**Resolved (UD-5), 2026-10-09.** Graph Undo/Redo is no longer wiped by another panel's confirmed edit and is sent in time order with the Render edits. The UD-5 landing check (`<作業ディレクトリ>/ud5_work/w1_final/graph_fold_decisions.json`, started `2026-10-08T23:46:46`) verified Undo/Redo, reopen and health green (6 Render and 4 Graph Undo presses).

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 103, then the `undo` epilogue.

Observed panel notices: `Unfolded fold_1: the group and its decisions are gone; the nodes stay.`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -2,3 +2,163 @@
  "definitions": {},
- "nodes": [],
+ "layout": {
+  "nodes": {
+   "render_fullscreen": {
+    "x": 428,
+    "y": 0
```


### QD1-R13: gui_assets_watch_verify / undo

**Resolved (UD-6), 2026-10-08.** Assets Undo restores values, and the remaining difference was formatting and the resulting digest. The driver now compares JSON by parsed content and excludes `project_digest` only when `project.json` matches by content on both sides; the UD-6 integration run verified Undo green.

Original reproduction (retained as evidence):

**Documented value-based Undo / strict byte check.** After exempting the manifest update that is outside Assets Undo, `project.json` still differs by bytes while its canonical JSON diff is empty; `get_project_settings` also reports a changed project digest. The Assets Undo documentation in [manual 13](../../docs/manual/13_editor.md) explicitly says that it restores values and does not restore the original digest, because the engine formats JSON with two spaces. This record is therefore not evidence of a lost setting. The strict byte check remains marked until a narrowly scoped comparison policy is chosen; do not change production values or suppress the whole project document to make it pass.

Reproduce through ordinary step 46, then the `undo` epilogue.

Undo differs: `file:project.json`, `get_project_settings:`.

```diff
@@ -3,3 +3,3 @@
  "hot_reload": null,
- "project_digest": "1f89682d9c9d93f3eb3947905ef6308b468827a3cb9d02390ff15fd9fc58ea7f"
+ "project_digest": "6d686b445dff8e16d35888272f6cab79b08d5f0d2ca0b41a26d8cca55abbf0d6"
 }
```


### QD1-R17: gui_build_curve / undo

**Resolved (RT-A to RT-D, UD-1 to UD-3), 2026-10-08.** The full run at `33b761ecd` (report started `2026-10-08T19:59:32`) verified Undo/Redo, reopen and health green: 9 Graph Undo presses return the start and their Redo the endpoint.

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 94, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -2,3 +2,114 @@
  "definitions": {},
- "nodes": [],
+ "nodes": [
+  {
+   "id": "render_fullscreen__color",
+   "op": "render.image",
+   "outputs": [
```


### QD1-R22: gui_build_ocean / undo

**Resolved (RT-A to RT-D, UD-1 to UD-3), 2026-10-08.** The full run at `33b761ecd` (report started `2026-10-08T20:07:41`) verified Undo/Redo, reopen and health green (39 Graph Undo presses).

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 259, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,4 +1,634 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "loop_body": {
+   "nodes": [
+    {
```


### QD1-R23: gui_build_ocean_compute / undo

**Resolved (RT-A to RT-D, UD-1 to UD-3), 2026-10-08.** The full run at `33b761ecd` (report started `2026-10-08T20:11:49`) verified Undo/Redo, reopen and health green (37 Graph Undo presses).

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 268, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,4 +1,635 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "loop_body": {
+   "nodes": [
+    {
```


### QD1-R25: gui_build_rt_from_blank / undo

**Resolved (RT-A to RT-D, UD-1 to UD-3), 2026-10-08.** The full run at `33b761ecd` (report started `2026-10-08T20:20:31`) verified Undo/Redo, reopen and health green (4 Graph and 1 scene Undo presses).

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 79, then the `undo` epilogue.

Observed panel notices: `logical shadow graph node 'present' reads unknown resource 'render_compute__color' (rende…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -2,2 +2,3 @@
  "definitions": {},
+ "layout": {},
  "nodes": [],
```

Redo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -2,16 +2,3 @@
  "definitions": {},
- "layout": {
-  "nodes": {
-   "render_compute": {
-    "x": 428,
-    "y": 0
-   },
```


### QD1-R27: gui_graph_only_edits / undo

**Resolved (RT-A to RT-D, UD-1 to UD-3), 2026-10-08.** The full run at `33b761ecd` (report started `2026-10-08T20:23:51`) verified Undo/Redo, reopen and health green: 15 Graph Undo presses, one of which restarts the engine with `restart_history_ready: true`.

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 138, then the `undo` epilogue.

Undo differs: `file:passes/authoring/render-targets.json`, `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`, `render:project://passes/authoring/render-targets.json`.

```diff
@@ -0,0 +1,34 @@
+{
+  "schema": "pelican.render_feature",
+  "version": 1,
+  "name": "authored_render_targets",
+  "runtime_shader_compiler": "optional",
+  "pelican_editor_managed": {
+    "schema": "pelican.authored_render_fragment",
```

The last same-project restart is step 31; after-Undo also differs from that restart snapshot. Only scene history is excluded solely because of restart.


### QD1-R34: gui_rt_procedural_callable / undo

**Resolved (UD-6), 2026-10-08.** The values were already restored; only the file formatting differed. The driver now compares JSON documents by parsed content and reports a byte-only difference as formatting, and the UD-6 integration run verified Undo green.

Original reproduction (retained as evidence):

**Follow-up diagnosis:** the inline editor was the `trace/dispatch` value editor, not a lost history. After cancelling its uncommitted draft, four Graph inverses and their Redo restore all authored JSON values. The only remaining Undo difference is raw file identity; canonical JSON and the engine document agree. The strict byte check remains marked. The original semantic-difference excerpt below is historical.

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 42, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -60,2 +60,3 @@
     "ray_tracing": {
+     "callable": "project://shaders/trace_callable",
      "hit_shaders": {
@@ -63,4 +64,4 @@
        "ball": {
-        "closesthit": "project://shaders/start",
-        "intersection": "project://shaders/none"
```


### QD1-R35: gui_rt_two_rays_two_misses / undo

**Resolved (UD-6), 2026-10-08.** The values were already restored; only the file formatting differed. The driver now compares JSON documents by parsed content and reports a byte-only difference as formatting, and the UD-6 integration run verified Undo green.

Original reproduction (retained as evidence):

**Follow-up diagnosis:** the inline editor was the `trace/dispatch` value editor, not a lost history. After cancelling its uncommitted draft, four Graph inverses and their Redo restore all authored JSON values. The only remaining Undo difference is raw file identity; canonical JSON and the engine document agree. The strict byte check remains marked. The original semantic-difference excerpt below is historical.

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 48, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -65,2 +65,7 @@
        }
+      },
+      "shadow": {
+       "mesh": {
+        "closesthit": "project://shaders/trace_shadow_mesh"
+       }
       }
```


### QD1-R42: material_loop_decompose / undo

**Resolved (UD-5), 2026-10-09.** The Redo half that differed now returns the endpoint. The UD-5 landing check (`<作業ディレクトリ>/ud5_work/w1_final/material_loop_decompose.json`, started `2026-10-09T00:02:59`) verified Undo/Redo, reopen and health green (3 Graph and 1 Render presses each way).

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 46, then the `undo` epilogue.

Redo differs: `file:passes/main.json`, `render:passes/main.json`.

```diff
@@ -3,20 +3,2 @@
   "forward_opaque_draw": {
-   "nodes": [
-    {
-     "id": "step",
-     "op": "draw.material",
-     "outputs": [
-      "draws"
```


### QD1-R4: material_pass_authoring / undo

**Resolved (UD-5), 2026-10-09.** The UD-5 landing check (`<作業ディレクトリ>/ud5_work/w1_final/material_pass_authoring.json`, started `2026-10-09T00:03:29`) verified Undo/Redo, reopen and health green: 1 Render and 1 Graph Undo press, the Graph one restarting the engine with `restart_history_ready: true`.

Original reproduction (retained as evidence):

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 38, then the `undo` epilogue.

Undo differs: `file:passes/main.json`, `render:passes/main.json`.

```diff
@@ -1,6 +1,118 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "render_material_pass_draw": {
+   "nodes": [
+    {
```

The last same-project restart is step 15; after-Undo matches that restart snapshot. Only scene history is excluded solely because of restart.
