# QD-1: scenario invariant findings

Inventory dated 2026-10-05. Ordinary scenario assertions pass in all 84 cases. The matrix preserves observed failures even when a ticketed `known_red` mark lets CTest pass.

There are now **33 marks in 33 scenarios**: 33 Undo/Redo checks and 0 health checks. The original inventory had 49 marks in 46 scenarios; this follow-up resolved 16 marks. These are reproduction records, not a count of independent root causes. No reopen check is marked known red.

## Evidence and interpretation

- Full run: 84 scenarios at `8c8752119`, 5212.34 s wall time; 38 CTest passes / 46 failures before the final inventory marks.
- Checker corrections at `0828834f5`: seven pilot cases and 23 affected follow-up cases were rerun. The matrix uses the latest executed result for each case. This is not a claim that all 84 ran together at the final revision.
- Final representative run at `f91453a6e`: 6/6 CTest passes in 327.25 s; four Undo and one health result remain explicitly `known_red`. No ordinary or reopen failure occurred. The 84 recorded outcomes also passed an offline policy audit; that audit is not a GUI rerun.
- Ordinary assertions, reopen and health are separate from Undo/Redo. `red` below means the invariant detected the recorded difference; `N/A` has a stated scope reason.
- Each case is reproducible with `ctest --test-dir <build> -C Debug -R "^studio_scenario_<scenario>$" -j1 --output-on-failure`. Reports are in `<build>/test_artifacts/studio_scenarios/<scenario>/report.json`; invariant steps immediately follow the ordinary scenario steps.
- The full-run and targeted-run reports/logs were archived before final mark verification. The integration report records their local locations.
- Studio CPU: 774/774. Python CI: 79/79 including the final incomplete-traversal guard. Nine isolated Python mutations each failed the relevant test and passed after restoration. This was not a mutation campaign across every C++ command. No golden was regenerated.

## Follow-up fixes (2026-10-05)

Production fixes cover project-scoped Material history, the Materials dock's missing binding inverse, per-entry procedural-record restoration, Graph Redo across confirmed Render restores, model-resource destruction order, and shader failure retirement. The checker separately corrects uncommitted inline editors and empty containers left by an already-exempt placement subtree. No golden was regenerated and no error was added to an allowlist.

Validation is targeted, not a second all-84 run: 125 Graph/Materials/Render-history CPU cases, 81 Python CI cases, engine procedural-record and shader regression cases, GPU late-model teardown and shader-retirement cases, and the dated GUI rechecks in the matrix. Earlier in the fix series the complete Studio CPU set passed 776/776. Local reports/logs are archived in `<作業ディレクトリ>/studio_fixes_final_evidence.zip`; intermediate diagnostic evidence is in `<作業ディレクトリ>/studio_fixes_mid_evidence.zip` and `<作業ディレクトリ>/studio_fixes_lifetime_evidence.zip`.

Remaining marks are still executable reproductions. They include intentional route-contract refusals, the documented value-only Assets Undo, and Graph/Render ordering or missing-inverse cases that are not resolved by these fixes. A CTest pass with `known_red` is not an invariant pass.

## All 84 scenarios

| Scenario | Ordinary | Undo / Redo | Reopen | Health | Tickets | Evidence |
|---|---|---|---|---|---|---|
| add_primitive_object | pass | red | green | green | QD1-R5 | follow-up |
| canvas_zoom_pan | pass | green | green | green |  | full run |
| deferred_loop_from_scratch | pass | red | green | green | QD1-R6 | follow-up |
| engine_window_embedded | pass | green | green | green |  | full run |
| forward_loop_from_scratch | pass | green | green | green |  | full run |
| frameplan_edit_roundtrip | pass | green | green | green |  | full run |
| frameplan_layout_then_edit | pass | red | green | green | QD1-R7 | full run |
| fullscreen_pass_from_template | pass | green | green | green |  | fixes recheck 21:08 |
| graph_auto_layout | pass | green | green | green |  | fixes recheck 21:09 |
| graph_canvas_zoom_pan | pass | green | green | green |  | full run |
| graph_edit_roundtrip | pass | green | green | green |  | full run |
| graph_fold_decisions | pass | red | green | green | QD1-R10 | fixes recheck 21:28 |
| graph_folded_features | pass | green | green | green |  | full run |
| graph_from_blank | pass | red | green | green | QD1-R1 | final check |
| graph_from_blank_forward | pass | red | green | green | QD1-R11 | follow-up |
| graph_layers_origin | pass | green | green | green |  | full run |
| graph_loop_expand | pass | green | green | green |  | final check |
| graph_observe_roundtrip | pass | green | green | green |  | full run |
| graph_palette_kinds | pass | green | green | green |  | fixes recheck 21:29 |
| graph_read | pass | green | green | green |  | full run |
| graph_reroute_tie | pass | green | green | green |  | full run |
| graph_template_bloom | pass | green | green | green |  | full run |
| gui_assets_watch_verify | pass | red | green | green | QD1-R13 | final check |
| gui_build_A | pass | red | green | green | QD1-R14 | follow-up |
| gui_build_audio_reactive | pass | green | green | green |  | follow-up |
| gui_build_B | pass | red | green | green | QD1-R15 | follow-up |
| gui_build_C | pass | red | green | green | QD1-R16 | follow-up |
| gui_build_curve | pass | red | green | green | QD1-R17 | full run |
| gui_build_D | pass | red | green | green | QD1-R18 | full run |
| gui_build_E | pass | red | green | green | QD1-R19 | follow-up |
| gui_build_full | pass | red | green | green | QD1-R20 | follow-up |
| gui_build_ltc | pass | red | green | green | QD1-R21 | full run |
| gui_build_music | pass | green | green | green |  | full run |
| gui_build_ocean | pass | red | green | green | QD1-R22 | full run |
| gui_build_ocean_compute | pass | red | green | green | QD1-R23 | full run |
| gui_build_optimize | pass | red | green | green | QD1-R24 | full run |
| gui_build_rt_from_blank | pass | red | green | green | QD1-R25 | follow-up |
| gui_folded_pass_edit | pass | green | green | green |  | full run |
| gui_frameplan_delegates | pass | red | green | green | QD1-R2 | full run |
| gui_graph_folded_edits | pass | green | green | green |  | full run |
| gui_graph_new_target | pass | red | green | green | QD1-R26 | follow-up |
| gui_graph_only_edits | pass | red | green | green | QD1-R27 | fixes recheck 21:30 |
| gui_graph_only_edits_2 | pass | red | green | green | QD1-R28 | follow-up |
| gui_graph_order | pass | green | green | green |  | full run |
| gui_graph_task_delete | pass | green | green | green |  | full run |
| gui_lower_layer_pin_freeze | pass | green | green | green |  | pilot |
| gui_material_from_blank | pass | green | green | green |  | fixes recheck 21:31 |
| gui_material_ray_query_from_blank | pass | red | green | green | QD1-R3 | follow-up |
| gui_material_repair | pass | green | green | green |  | fixes recheck 21:32 |
| gui_model_from_file | pass | green | green | green |  | full run |
| gui_node_parameters | pass | green | green | green |  | full run |
| gui_problems_window | pass | N/A | green | green |  | full run |
| gui_render_fallback | pass | green | green | green |  | full run |
| gui_rt_checks | pass | green | green | green |  | fixes recheck 21:33 |
| gui_rt_full_from_blank | pass | green | green | green |  | fixes recheck 21:34 |
| gui_rt_hit_groups_from_blank | pass | red | green | green | QD1-R33 | follow-up |
| gui_rt_layout_edits | pass | green | green | green |  | fixes recheck 21:35 |
| gui_rt_procedural_callable | pass | red | green | green | QD1-R34 | fixes recheck 21:36 |
| gui_rt_two_rays_two_misses | pass | red | green | green | QD1-R35 | fixes recheck 21:37 |
| gui_settings_choose | pass | red | green | green | QD1-R36 | fixes recheck 21:15 |
| input_needs_shader | pass | red | green | green | QD1-R37 | pilot |
| lighting_pass_template | pass | green | green | green |  | fixes recheck 21:17 |
| loop_view_axis | pass | green | green | green |  | full run |
| loop_zone_edit | pass | green | green | green |  | fixes recheck 21:19 |
| loop_zone_read | pass | green | green | green |  | fixes recheck 21:19 |
| loop_zone_wire | pass | green | green | green |  | fixes recheck 21:20 |
| material_loop_decompose | pass | red | green | green | QD1-R42 | full run |
| material_pass_authoring | pass | red | green | green | QD1-R4 | full run |
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
| sockets_fix_and_show | pass | red | green | green | QD1-R44 | full run |
| sockets_from_shader | pass | red | green | green | QD1-R45 | follow-up |
| theme_switch | pass | green | green | green |  | full run |
| view_select_field | pass | green | green | green |  | full run |
| whole_frame_first_steps | pass | green | green | green |  | full run |

## Remaining findings

The diff excerpts below come from the executed reports (expected start/end versus found). They are deliberately short; the report holds the remaining keys and first diff lines.

### QD1-R5: add_primitive_object / undo

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 56, then the `undo` epilogue.

Observed panel notices: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

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

The last same-project restart is step 32; after-Undo matches that restart snapshot. Only scene history is excluded solely because of restart.

### QD1-R6: deferred_loop_from_scratch / undo

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the deferred_geometry route, which pelican.builtin.transparent (engine buil…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 98, then the `undo` epilogue.

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

### QD1-R10: graph_fold_decisions / undo

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

### QD1-R1: graph_from_blank / undo

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

### QD1-R11: graph_from_blank_forward / undo

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 69, then the `undo` epilogue.

Observed panel notices: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`; `logical shadow graph node 'present' reads unknown resource 'render_material_pass__color'…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

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

Redo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -112,60 +112,2 @@
    }
-  },
-  {
-   "id": "render_fullscreen__color",
-   "op": "render.image",
-   "outputs": [
-    "resource"
```

The last same-project restart is step 22; after-Undo matches that restart snapshot. Only scene history is excluded solely because of restart.

### QD1-R13: gui_assets_watch_verify / undo

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

### QD1-R14: gui_build_A / undo

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 41, then the `undo` epilogue.

Observed panel notices: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

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

The last same-project restart is step 21; after-Undo matches that restart snapshot. Only scene history is excluded solely because of restart.

### QD1-R15: gui_build_B / undo

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the deferred_geometry route, which pelican.builtin.transparent (engine buil…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 69, then the `undo` epilogue.

Observed panel notices: `No pass draws the deferred_geometry route, which pelican.builtin.transparent (engine buil…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,4 +1,224 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "render_material_pass_draw": {
+   "nodes": [
+    {
```

### QD1-R16: gui_build_C / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 164, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,6 +1,440 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "loop_body": {
+   "nodes": [
+    {
```

The last same-project restart is step 21; after-Undo also differs from that restart snapshot. Only scene history is excluded solely because of restart.

### QD1-R17: gui_build_curve / undo

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

### QD1-R18: gui_build_D / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 99, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,4 +1,328 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "render_material_pass_draw": {
+   "nodes": [
+    {
```

Redo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -261,9 +261,3 @@
    "op": "render.material_pass",
-   "outputs": [
-    "albedo",
-    "normal",
-    "material",
-    "worldpos",
-    "emissive"
```

### QD1-R19: gui_build_E / undo

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the deferred_geometry route, which pelican.builtin.transparent (engine buil…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 69, then the `undo` epilogue.

Observed panel notices: `No pass draws the deferred_geometry route, which pelican.builtin.transparent (engine buil…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,4 +1,224 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "render_material_pass_draw": {
+   "nodes": [
+    {
```

### QD1-R20: gui_build_full / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 284, then the `undo` epilogue.

Observed panel notices: `logical shadow graph node 'present' reads unknown resource 'render_fullscreen_3__color' (…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,6 +1,977 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "loop_body": {
+   "nodes": [
+    {
```

### QD1-R21: gui_build_ltc / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 192, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,4 +1,532 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "render_material_pass_draw": {
+   "nodes": [
+    {
```

### QD1-R22: gui_build_ocean / undo

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

### QD1-R24: gui_build_optimize / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 495, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,6 +1,1179 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "loop_body": {
+   "nodes": [
+    {
```

The last same-project restart is step 469; after-Undo also differs from that restart snapshot. Only scene history is excluded solely because of restart.

### QD1-R25: gui_build_rt_from_blank / undo

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

### QD1-R2: gui_frameplan_delegates / undo

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 69, then the `undo` epilogue.

Observed panel notices: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

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

The last same-project restart is step 30; after-Undo matches that restart snapshot. Only scene history is excluded solely because of restart.

### QD1-R26: gui_graph_new_target / undo

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 60, then the `undo` epilogue.

Observed panel notices: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`.

Undo differs: `file:passes/authoring/render-targets.json`, `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`, `render:project://passes/authoring/render-targets.json`.

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

The last same-project restart is step 41; after-Undo matches that restart snapshot. Only scene history is excluded solely because of restart.

### QD1-R27: gui_graph_only_edits / undo

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

### QD1-R28: gui_graph_only_edits_2 / undo

**Studio/engine / authoring contract.** The history walk encounters an engine route contract refusal: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`. Remaining document changes are listed below. Equality with a restart snapshot does not prove that the restart cleared the history. Decide intended rollback semantics before changing production.

Reproduce through ordinary step 103, then the `undo` epilogue.

Observed panel notices: `No pass draws the forward_opaque route, which pelican.builtin.transparent (engine built-i…`.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,6 +1,119 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "render_material_pass_draw": {
+   "nodes": [
+    {
```

The last same-project restart is step 31; after-Undo also differs from that restart snapshot. Only scene history is excluded solely because of restart.

### QD1-R3: gui_material_ray_query_from_blank / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 118, then the `undo` epilogue.

Observed panel notices: `Material pass 'render_material_pass' uses an acceleration structure but does not declare…`.

Undo differs: `file:assets/asset_data.json`, `file:materials/material.material.json`, `file:passes/main_rendering_config.json`, `list_materials:`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,2 +1,7 @@
 {
+ "materials": [
+  {
+   "path": "materials/material.material.json"
+  }
+ ],
  "models": [],
```

The last same-project restart is step 32; after-Undo also differs from that restart snapshot. Only scene history is excluded solely because of restart.

Redo coverage limit: material_row: a Material row's Undo (Every <model>) has no Redo.

### QD1-R33: gui_rt_hit_groups_from_blank / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 134, then the `undo` epilogue.

Undo differs: `file:assets/asset_data.json`, `file:materials/material.material.json`, `file:materials/material_2.material.json`, `file:passes/main_rendering_config.json`, `list_materials:`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,2 +1,10 @@
 {
+ "materials": [
+  {
+   "path": "materials/material.material.json"
+  },
+  {
+   "path": "materials/material_2.material.json"
```

The last same-project restart is step 83; after-Undo also differs from that restart snapshot. Only scene history is excluded solely because of restart.

Redo coverage limit: material_row: a Material row's Undo (Every <model>) has no Redo.

### QD1-R34: gui_rt_procedural_callable / undo

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

### QD1-R36: gui_settings_choose / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 206, then the `undo` epilogue.

Undo differs: `file:passes/main_rendering_config.json`, `render:passes/main_rendering_config.json`.

```diff
@@ -1,6 +1,665 @@
 {
- "definitions": {},
- "nodes": [],
+ "definitions": {
+  "render_material_pass_draw": {
+   "nodes": [
+    {
```

### QD1-R37: input_needs_shader / undo

**Studio/engine / history dependencies.** The Graph history is retried after other histories change documents, but restoration still leaves the root graph different. A notice references an unknown render resource. The remaining ordering/contract interaction is not isolated.

Reproduce through ordinary step 71, then the `undo` epilogue.

Observed panel notices: `logical shadow graph node 'present' reads unknown resource 'render_material_pass__albedo'…`.

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

### QD1-R42: material_loop_decompose / undo

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

### QD1-R44: sockets_fix_and_show / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 96, then the `undo` epilogue.

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

### QD1-R45: sockets_from_shader / undo

**Studio / history restoration, cause not isolated.** The available per-kind history walk does not restore the compared documents. This records an observed coverage or restoration gap; it does not establish an independent product defect or prove that an engine restart deleted history. A focused reproduction must distinguish history invalidation, shared Graph/Render ordering and a missing inverse.

Reproduce through ordinary step 94, then the `undo` epilogue.

Observed panel notices: `A shader the graph draws with did not reload: Fullscreen pass input texture/local-read co…`.

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
