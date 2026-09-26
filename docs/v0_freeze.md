# v0 形式の一覧と移行の約束(pelican 0.1.0)

**文書名 `v0_freeze.md`(旧称「v0 凍結一覧」)は歴史的な名残で、中身は v0 形式の一覧と移行の約束**
(2026-09-25、MG-1 `28bf3b40` の着地で書き直した。[設計 §1.4](design_v0_production_gaps.md)、2026-09-24 の利用者の意図)。

v1 計画 [docs/v1_development_plan.md](v1_development_plan.md) の「v0 で凍結するもの / 広く約束しないもの」の
5 行表を、実物の名前に展開した正本で、v1 計画の表は要約。v1 計画の「凍結するもの」は、この文書の §1
「v0 形式の一覧と移行の約束」の意味で読む(形を動かさない約束ではない。[設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24)。

読み方: §1 が「v0 形式の一覧と、後の版へ移せるという約束」(§1.2〜§1.4 は v0 時点の記録で、不変は約束しない)、
§2 が「二つの正本のうちどちらを残したか」、
§3 が「v0 では約束しないもの」、§4 が「解消済みの項目と現行構成で再検証するもの」、
§5 が「v0 に入った変更の SHA」、§6 が tag の手順(§1 の読み替えは [設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24)。

## 1. v0 形式の一覧と移行の約束

**約束: v0 形式の project は、後の版で `pelican_cli project migrate`(または Studio の移行画面)により開ける状態にできる。自動で直せない箇所は名前付きで列挙される。**

これは「形を動かさない」約束ではない。2026-09-24 に利用者は、v0 を「v0 以降でファイルフォーマットの変化はありえるが、
今後のファイルフォーマットへの変換などある程度の対応を可能とするべき最初のヴァージョン」とし、
「前のヴァージョンのプロジェクトをあとのヴァージョンのソフトに移行する仕組みは欲しい」と述べた。
旧 §1「凍結するもの」は形を動かさない約束と読めてこの意図とずれていたので、[設計 §1.4](design_v0_production_gaps.md) のとおり書き直した。

形式を変えるときの規則と、約束を支える仕組み([設計 §1.2](design_v0_production_gaps.md)、利用者の意図 2026-09-24):

- **文書の `version` を上げる WP は、同じ WP で移行手順を 1 本登録し、`test/fixtures/project_corpus/v0/` の移行 test を緑に保つ**(台帳 V0-ADD の運用の決めごと。2026-09-25、MG-1 `28bf3b40` から有効)。
- 移行の単位は engine の版ではなく**各文書の `(schema, version)`**。手順は 1 版ずつ(`from + 1 = to`)で、重複・版の飛び・循環は登録時に `project.migration_step_*@1` で拒否される。
- 自動で直せない箇所は、手順が `manual_items`(`{code, pointer, message}`)として返し、plan / apply の応答に文書ごとに載る。
- 仕組みの実体は MG-1(`28bf3b40`): 開く前の入口が各文書の `schema` / `version` を parser より前に生で読み、登録手順の起点に当たれば `project.migration_required@1` で止める。`plan`(dry-run)→ `apply`(`.pelican/migration/<時刻>/` へ退避し、1 つの transaction で書き換え、project.json の `saved_by_engine` を更新)。入口は CLI `pelican_cli project migrate` と RPC `plan_project_migration` / `apply_project_migration`。使い方は [manual 3 章 §3.13](manual/03_project_format.md) と [10 章 §10.4](manual/10_tools.md)。
- **v0 時点の状態**: production の手順は 0 本(v0 は後の版が移行元にする最初の版)。Studio の移行画面はまだ無く(GUI session の別 WP)、今日の入口は CLI と RPC。移行の検出が見るのは project root の下だけで、root の外に mount した asset store と `user://` 側(`pelican.settings`)には届かない。

§1.1 が約束の対象になる v0 形式の一覧で、§1.5 / §1.6 はその一部の形の補足。§1.2〜§1.4 は **v0 時点の一覧(記録)で、不変は約束しない**
([設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24)。

### 1.1 文書 schema と version

project が持てる文書と、v0 の parser が受ける version(= **v0 形式の一覧**)。各 parser が受けるのは表の version だけで、
番号は文書ごとに独立で、op の意味・package・native ABI とは別の番号(v1 計画の「schema version の分離」)。
engine が開く文書の `(schema, version)` は、**その engine に移行の手順が登録されていれば(その組が手順の起点なら)、parser より前に `project.migration_required@1` で止まり、`project migrate` で移せる。手順が無く parser の受ける version でもなければ、parser が名前付きで拒否する**。
後の版の engine は、この表の組を起点にする手順を持つことで v0 形式を開ける状態にする。v0 の engine は手順 0 本なので、表に無い組はすべて parser が拒否する
(MG-1。[設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24)。

| schema | version | parser | 備考 |
|---|---|---|---|
| `pelican.project` | 1 | `src/project/projectformat.cpp` | `project.json`。`engine_min_version` と `requires` を持つ。MG-1 で記録だけの `saved_by_engine` を足した(version は 1 のまま。MG-1 より前の player もこの key 入りで開けることを実物で確認) |
| `pelican.scene` | 2 | `src/project/sceneformat.cpp` | MG-2b で 1 の読み手を撤去し、2 以外は `pelican.scene.version_unsupported@1`(v1 は prefab キーを受けないことだけが違う版)。出荷 7 本と雛形・devcli の glTF / rules importer・import manifest の `pelican.scene` 出力は 2。SnapshotV1(scene v1 を運ぶ export / import)も撤去 |
| `pelican.graph` | 1 | `src/project/graphcomposition.cpp` | 共通グラフ。`definitions` / `nodes` / `outputs` と構造の node `graph.instance` / `graph.input` / `graph.loop`(LP-1a。catalog には載らない)。rendering config の root もこれだけ(§1.6) |
| `pelican.value_graph` | 1 | `src/core/userpublic/valuegraph.cpp` | **内部 IR + test 面**。外部契約ではない(§2 行 1) |
| `pelican.anim_graph` | 1 | `src/core/userpublic/animation/animgraph.cpp` | animation component が指す state machine 文書 |
| `pelican.render_feature` | 1 | `src/project/featurecompose.cpp` | rendering 節の `features[]` の instance は object 形で `pass_replacements`(LP-0d)を持てる |
| `pelican.render_feature_parameters` | 1 | `src/project/featurecompose.cpp` | |
| `pelican.render_feature_overlay` | 1 | `src/project/renderfeatureoverlay.cpp` | 起動時の一時合成。文書に書き戻さない(§2 行 11) |
| `pelican.render_component` | 1 | `src/project/rendercomponent.hpp` | export / import される部品 |
| `pelican.render_pipeline` | 1 | `src/project/renderpipeline.cpp` | preset(`hybrid_v1` など) |
| `pelican.package` | 1 | `src/project/packagemanifest.hpp` | §1.5 |
| `pelican.assets` | 1 | `src/project/assetsmanifest.cpp` | `assets.manifest.json` |
| `pelican.assets_cache` | 1 | `src/project/assetsmanifest.cpp` | 生成物 |
| `pelican.asset_data` | 2 | `src/project/assetdataformat.cpp` | MG-2b で 1 の読み手を撤去し、2 以外は `pelican.asset_data.version_unsupported@1`(v1 は `procedural_geometry` を受けないことだけが違う版)。出荷 7 本と雛形は 2 |
| `pelican.import` | 1 | `src/project/importmanifest.cpp` | |
| `pelican.import_rules` | 1 | `src/project/importrules.cpp` | |
| `pelican.material` | 1 | `src/project/materialformat.cpp` | |
| `pelican.material_bindings` | 1 | `src/project/materialformat.cpp` | |
| `pelican.material_outputs` | 1 | `src/project/materialoutput.cpp` | |
| `pelican.prefab` | 1 | `src/project/prefab.cpp` | scene v2 が前提 |
| `pelican.ui` | 1 | `src/core/ui/document.cpp` | |
| `pelican.atlas` | 1 | `src/core/asset/atlasasset.cpp` | |
| `pelican.input_actions` | 1 | `src/project/inputactionoverlay.cpp` | |
| `pelican.input_action_overlay` | 1 | `src/project/inputactionoverlay.cpp` | |
| `pelican.input_profile` | 1 | `src/core/os/actionmap.cpp` | |
| `pelican.transform_seq` | 1 | `src/core/playback/seqplayer.cpp` | camera bake の出力 |
| `pelican.settings` | 1 | `src/core/persistence/persistence.cpp` | `user://` 側 |
| `pelican.ray_group_layout` | 3 | `src/core/shader/raygrouplayout.cpp` | 出荷 raytracing_demo / pathtrace_demo は 3。MG-2a で 1〜2 の読み手を撤去し、3 以外は `pelican.ray_group_layout.version_unsupported@1` |
| `pelican.ray_bindings` | 2 | `src/core/shader/raygrouplayout.cpp` | 出荷 raytracing_demo / pathtrace_demo は 2。MG-2a で 1 の読み手を撤去し、2 以外は `pelican.ray_bindings.version_unsupported@1` |
| `pelican.graph_catalog` | 1 | `src/project/graphmodule.cpp` | `pelican_cli graph-catalog` の出力(§1.3) |
| `pelican.project_catalog_smoke` | 1 | `test/run_project_catalog_smoke.cmake` | project ごとの smoke 閾値 |
| `pelican.project_migration_backup` | 1 | `src/project/projectmigration.cpp` | `project migrate` の apply が退避先 `.pelican/migration/<時刻>/migration.json` に書く記録(MG-1)。`.` で始まるディレクトリの下なので、移行の検出は見ない |

**v0 の間に足した field(どれも足すだけなので版は上げていない。[design_graph_loops §8](design_graph_loops.md))**:
pass と compute task の JSON の `view_select`(LP-Va)、fullscreen / raster pass の `shader.defines`(VI-1)、
feature の instance の `pass_replacements`(LP-0d)、graph の render node と `graph.loop` の `insert` / `canonical_anchor`(LP-0b)と
`insert.declared`(LP-3b)、graph の render port の `history`(LP-0a)と `binding: "input"` / `input_index`(LP-1d)、
`render.material_pass` の `resolution_domain` / `inherit_bindings_from`(LP-Vb)。書き方は [manual 6 章 §6.18](manual/06_rendering.md)。
engine 付属の `engine://graphs/*.json`(分解版の雛形。`decomposes` を持つ `pelican.graph` 1)は engine が読まない雛形で、project が写して使う。

入力の収録 `pelican.input_seq`(GP-4 から v2 で書き、v2 と v1 を読む。`src/core/os/inputsequence.cpp`)は表に載せない。`--record-input` / `start_input_record` が呼び手の指定した場所に書く JSONL で、project が持つ文書ではないため、移行の約束の対象にせず手順も登録していない(版の読み分けは parser が持つ。[manual 7 章 §7.3](manual/07_input_ui.md))。

### 1.2 RPC method 名

**v0 時点の一覧(記録)。不変は約束しない**([設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24)。
method 名は後の版で足したり変えたりしうる。RPC は project の文書ではないので `project migrate` の対象でもない。

stdio JSON-RPC の枠組み(`jsonrpc` / `id` / `method` / `params`、名前付き error)の v0 時点の形と method 名を記録する。
**params と戻り値の表は [docs/manual/10_tools.md](manual/10_tools.md) が正本**(ここでは重複させない)。

MG-1 で足した 2 本(`src/core/communication/projectmigrationrpc.cpp` の登録。engine では `rpcserver.cpp` の `configureEngineRpcHandlers` から登録し、開く前の入口で止まった `--rpc` player ではこの 2 本だけの endpoint になる): `plan_project_migration`, `apply_project_migration`

基盤 30 本(`src/core/communication/rpcserver.cpp` の登録。`reload_shaders` は F-5 `7d3f3565` で追加):

`ack_modal_transform`, `cancel_modal_transform`, `capture`, `capture_gpu`, `get_frame_plan`, `get_gpu_timing`, `get_modal_transform`, `get_status`, `inject_event`, `inject_input`, `load_gltf`, `load_scene`, `pick_object`, `query_gizmo_drag_basis`, `query_gizmo_handle`, `raycast`, `reload_game_logic`, `reload_shaders`, `render_frame`, `set_camera`, `set_gizmo`, `set_input_profile`, `set_seed`, `set_time`, `start_input_record`, `start_input_replay`, `step_frame`, `stop_input_record`, `stop_input_replay`, `update_transforms`

エディタ拡張 65 本(`src/core/communication/editorrpchandlers.cpp` の登録。LP-3b / RD-1 の 3 本と、以前から一覧に漏れていた `edit_render_targets` / `set_material_route_default` を 2026-09-27 に足した。詳細は [docs/manual/13_editor.md](manual/13_editor.md)):

`abort_preview`, `add_authored_pass`, `add_compute_task`, `can_edit`, `can_preview`, `commit_preview`, `decompose_material_pass`, `edit`, `edit_compute_task`, `edit_frame_node_order`, `edit_graph_document`, `edit_graph_instances`, `edit_preset_pass`, `edit_procedural_asset`, `edit_ray_callable_records`, `edit_region_replacement`, `edit_render_features`, `edit_render_targets`, `edit_scene_acceleration_structures`, `eval_preview`, `export_render_feature`, `export_scene_snapshot`, `fork_render_pipeline`, `get_component_add_candidates`, `get_components`, `get_compute_task_authoring`, `get_edit_result`, `get_frame_node_order`, `get_graph_catalog`, `get_graph_instances`, `get_graph_node_values`, `get_graph_values`, `get_preview_result`, `get_procedural_assets`, `get_ray_callable_records`, `get_ray_occurrences`, `get_render_authoring_context`, `get_render_documents`, `get_render_features`, `get_scene_acceleration_structures`, `get_scene_revision`, `import_render_feature`, `import_scene_snapshot`, `install_package`, `list_assets`, `list_render_features`, `open_editor_session`, `open_preview`, `query_journal`, `redo`, `remove_authored_pass`, `remove_compute_task`, `render_preview`, `resolve_ray_occurrence`, `restore_render_documents`, `resume_editor_session`, `save_scene`, `scene_tree`, `set_material_route_default`, `set_prefab_parameter`, `undo`, `uninstall_package`, `unpack_prefab`, `unset_prefab_parameter`, `update_preview`

### 1.3 共通グラフ: module / op / 型 / sink

**v0 時点の一覧(記録)。不変は約束しない**([設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24)。
module / op / 型 / sink は後の版で足したり変えたりしうる。下の op 表は `test/run_devcli_graph_catalog.cmake` が catalog と突き合わせる記録で、
catalog が変われば表も同じ変更で直す。op を書いた文書(`pelican.graph` など)の書き方が変わる場合は、その文書の version を上げて移行手順を登録する(§1 の規則)。

登録 module は最大 5 本(`pelican.value` / `pelican.audio` / `pelican.render` / `pelican.pose` / `pelican.ui`)で、いずれも module version 1。
`pelican.value` と `pelican.render` と `pelican.ui`(GP-3、UI には build OFF の構成が無い)は常に登録され、`pelican.audio` は `PELICAN_WITH_AUDIO=ON`、`pelican.pose` は `PELICAN_WITH_ANIMATION=ON` の build でだけ登録される
(`pelican.audio` の条件付き登録は PG-1 `533d8d6c` から。登録の `#if` は `src/project/graphmodule.cpp`)。
下の表は **`PELICAN_WITH_ANIMATION=ON` かつ `PELICAN_WITH_AUDIO=ON` build の `pelican_cli graph-catalog` の出力**を展開したもの(5 module / op 36 本。LP-0a / LP-0e / LP-3a で `pelican.render` に 7 本を足した)。
`change` は parameter を変えたときの既定の扱い(`value` = 値だけ / `rebuild` = 作り直し)で、
parameter ごとに既定と違う扱い(catalog の `change.parameters`)があればかっこで添える(AU-1 `feca9dce` の時点で持つのは `audio.playback` だけ)。
`ports: parameter_dependent` の op は port 一覧を宣言せず node 自身の `inputs` / `outputs` から決まる。
`?` の付いた入力は任意。

**`PELICAN_WITH_ANIMATION=OFF` build では `pelican.pose` module が無い**。
catalog は 4 module / 30 op になり、下表の `pose.*` 6 本(`pose.blend` / `pose.clip_sample` /
`pose.retarget` / `pose.skeletal` / `pose.state_machine` / `pose.transform`)だけが消える。
この関係は `test/run_devcli_graph_catalog.cmake` が「この表の op 集合 == ON の catalog、⊇ OFF の catalog」として検査する
(表の範囲は下の 2 つのコメント行で区切る。行を足すときは ON の catalog に載る op だけ)。

**`PELICAN_WITH_AUDIO=OFF` build では `pelican.audio` module が無い**。下表の `audio.*` 3 本(`audio.pitch` / `audio.playback` / `audio.volume`)が消え、
catalog は 4 module / 33 op(animation も OFF なら 3 module / 27 op)になる。`audio.*` を書いた文書は `graph.operation_unregistered@1`。
`event.playback` とその出力型 `pelican.event.playback_control@1` は `pelican.value` に属するので残る(PG-1 で型を `pelican.value` へ移した)。
同じ test は `WITH_AUDIO` と `WITH_ANIMATION` の両方を受け取り、module 数(3 + audio + animation。GP-3 までは 2 が基数で、`pelican.ui` が常に加わって 3 になった)と、この表のうち
build から外した unit の op(`audio.*` / `pose.*`)だけが欠けてよいことを確かめる(`de84fddb`。AUDIO=OFF は catalog から
`pelican.audio` を抜いた出力で確認、修正前の script はそれを「expected 4 built-in modules, got 3」で落とす。数は GP-3 前のもの)。

<!-- graph-op-table:begin -->
| op | module | domain | evaluation | change | sink | ports | inputs | outputs |
|---|---|---|---|---|---|---|---|---|
| `audio.pitch` | pelican.audio | values | direct | value | sink | declared | `value`, `voice`? | — |
| `audio.playback` | pelican.audio | values | replay | value(`asset` / `bus` / `wrap` は rebuild) | — | declared | `delay`?, `events`?, `position`?, `source_begin`?, `source_end`?, `speed`?, `time` | `voice`: pelican.audio.voice@1 |
| `audio.volume` | pelican.audio | values | direct | value | sink | declared | `value`, `voice`? | — |
| `draw.material` | pelican.render | rendering | direct | rebuild | — | declared | `after`? | `draws`: pelican.render.draw_stream@1 |
| `draw.override` | pelican.render | rendering | direct | rebuild | — | declared | `after`? | `draws`: pelican.render.draw_stream@1 |
| `draw.variant` | pelican.render | rendering | direct | rebuild | — | declared | `after`? | `draws`: pelican.render.draw_stream@1 |
| `event.playback` | pelican.value | values | direct | value | — | declared | — | `events`: pelican.event.playback_control@1 |
| `pose.blend` | pelican.pose | animation | direct | value | — | declared | `a`, `b`, `weight` | `pose`: pelican.pose@1 |
| `pose.clip_sample` | pelican.pose | animation | direct | rebuild | — | declared | `time` | `pose`: pelican.pose@1 |
| `pose.retarget` | pelican.pose | animation | direct | rebuild | — | declared | `pose` | `pose`: pelican.pose@1 |
| `pose.skeletal` | pelican.pose | animation | direct | rebuild | sink | declared | `pose` | — |
| `pose.state_machine` | pelican.pose | animation | replay | rebuild | — | declared | — | `pose`: pelican.pose@1 |
| `pose.transform` | pelican.pose | animation | direct | rebuild | sink | declared | `pose` | — |
| `render.attribute` | pelican.render | rendering | direct | rebuild | — | parameter_dependent | — | — |
| `render.buffer` | pelican.render | rendering | direct | rebuild | — | parameter_dependent | — | — |
| `render.compute` | pelican.render | rendering | direct | rebuild | — | parameter_dependent | — | — |
| `render.fullscreen` | pelican.render | rendering | direct | rebuild | — | parameter_dependent | — | — |
| `render.image` | pelican.render | rendering | direct | rebuild | — | parameter_dependent | — | — |
| `render.material_pass` | pelican.render | rendering | direct | rebuild | — | parameter_dependent | — | — |
| `render.raster` | pelican.render | rendering | direct | rebuild | — | parameter_dependent | — | — |
| `render.scalar` | pelican.render | values | direct | rebuild | sink | declared | `value` | — |
| `render.shadow_depth` | pelican.render | rendering | direct | rebuild | — | declared | `depth` | — |
| `render.snapshot_copy` | pelican.render | rendering | direct | rebuild | — | declared | `destination`, `source` | — |
| `render.target_ref` | pelican.render | rendering | direct | rebuild | — | parameter_dependent | — | — |
| `ui.value` | pelican.ui | values | direct | rebuild | sink | declared | `value` | — |
| `ui.visible` | pelican.ui | values | direct | rebuild | sink | declared | `value` | — |
| `value.add` | pelican.value | values | direct | value | — | declared | `a`, `b` | `value`: pelican.value.scalar@1 |
| `value.constant` | pelican.value | values | direct | value | — | declared | — | `value`: pelican.value.scalar@1 |
| `value.curve` | pelican.value | values | direct | value | — | declared | `time` | `value`: pelican.value.scalar@1 |
| `value.event_clock` | pelican.value | values | replay | value | — | declared | `events`, `time` | `value`: pelican.value.scalar@1 |
| `value.input` | pelican.value | values | direct | value | — | declared | — | `value`: pelican.value.scalar@1 |
| `value.morph_weight` | pelican.value | values | direct | value | — | declared | — | `value`: pelican.value.scalar@1 |
| `value.multiply` | pelican.value | values | direct | value | — | declared | `a`, `b` | `value`: pelican.value.scalar@1 |
| `value.parameter` | pelican.value | values | direct | value | — | declared | — | `value`: pelican.value.scalar@1 |
| `value.playback` | pelican.value | values | direct | value | — | declared | `delay`?, `duration`?, `elapsed`, `sample_interval`?, `source_begin`?, `source_end`?, `speed`? | `value`: pelican.value.scalar@1 |
| `value.trigger_clock` | pelican.value | values | replay | value | — | declared | `paused`?, `reset`?, `time`, `trigger` | `value`: pelican.value.scalar@1 |
<!-- graph-op-table:end -->

catalog に module の型として載る型(semantic)は ON build で 12: `pelican.value` の `pelican.value.scalar@1` / `pelican.event.playback_control@1`、
`pelican.audio` の `pelican.audio.voice@1`、`pelican.render` の `pelican.render.*` 7 種(`storage_buffer` / `attribute` /
`color_signal` / `depth` / `legacy_opaque_image` / `legacy_opaque_resource` / LP-3a の `draw_stream`)、
`pelican.pose` の `pelican.pose@1` / `pelican.rig@1`。`pelican.ui` は型を持ち込まない(sink 2 本の入力 `value` は `pelican.value` の `pelican.value.scalar@1`)。AUDIO=OFF では `pelican.audio.voice@1` が、ANIMATION=OFF では pose の 2 つが catalog から消える
(`pelican.audio.voice@1` は内部の組み込み型表 `makeBuiltinLogicalTypeRegistry()` には OFF でも残る)。値・姿勢・音の port を流れるのは render 7 種と rig を除く 4 つ(scalar / playback_control / voice / pose)で、
`render.*` の op は解決時に `pelican.render.*` を自分の port に載せる(上表の parameter_dependent)。
`pelican.rig@1` は登録だけで、どの port にも現れない(node の `parameters.rig` が指す同一性)。
`pelican.pose@1` は **handle と layout 世代**が流れるのであって bytes ではない。
sink は ON build で 7 本(`audio.pitch` / `audio.volume` / `render.scalar` / `pose.skeletal` / `pose.transform` / `ui.value` / `ui.visible`。OFF にした module の sink はその分減る)で、
pose の sink は **owner 単位の単一 writer**(二重 writer は `pose.sink_conflict@1`)。`ui.*` の sink は owner に属さないので、単一 writer の単位は `(文書, path, 性質)`(二重 writer は準備時に `ui.sink_conflict@1`、facade との衝突は facade が同じ名前で失敗し、sink が勝つ)。

### 1.4 ABI と版

**v0 時点の一覧(記録)。不変は約束しない**([設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24。native ABI の永久互換を約束しないことは §3 にもある)。
native ABI は project の文書ではないので `project migrate` の対象ではない。ただし下の**世代の管理**(ABI / descriptor / service の版番号、
SDK の epoch、capability bit、`test/fixtures/userpublic_frozen/` の凍結 snapshot)は、それ自体が「形が変わったことを名前と番号で見分け、
古い世代を固定として残す」互換の仕組みなので、その説明はこの節に残す。ここでいう「凍結 snapshot」は、ある世代の形を test が固定したもの
(その世代の形は変えず、形を変えるときは新しい世代を足す)で、面そのものを今後変えないという約束ではない。

| 面 | 版 | 置き場 |
|---|---|---|
| Animation ABI v1 | `abiVersionV1 = 1` / `descriptorVersionV1 = 1` | `src/core/userpublic/animation/abi_v1.hpp`([docs/animation_abi_v1.md](animation_abi_v1.md) が Frozen 宣言) |
| pose staging service | descriptor 1 / service 1 | `src/core/userpublic/animation/pose_staging_v1.hpp` |
| game logic SDK epoch | `PELICAN_GAME_LOGIC_ABI_VERSION = 2` | `src/core/userpublic/gamelogic.hpp` |
| engine の版 | `pelicanEngineVersion = "0.1.0"`(`ProjectEnvelopeParseOptions::current_engine_version` の既定値。MG-1 で名前付きの定数にした) | `src/project/projectformat.hpp` |
| render provider ABI v1(5 面) | `abiVersionV1 = 1` / `descriptorVersionV1 = 1` | `src/core/userpublic/render/{draw_sort,pass_implementation,graph_transform,render_strategy,subgraph_replacement}_abi_v1.hpp` |
| game logic SDK 面(C-8 の facade 分割後) | v3 snapshot | `test/fixtures/userpublic_frozen/v3/` |

C-8 で `GameContext` を facade に分割した(`physics()` / `audio()` / `animation()` / `save()` / `render()`)ため、
game logic SDK 面は v2 snapshot と**互換ではない**(epoch は 2 のまま。epoch は loader の anchor であって
`GameContext` の形ではない)。分割後の正本は v3 snapshot で、v1 / v2 は WP381 / WP382a の
layout 固定用にそのまま残す。

render provider ABI の 5 面は各名前空間に `Status` を持ち、**その数値は game logic DLL 境界を渡る ABI**。
正本は [test/fixtures/userpublic_frozen/](../test/fixtures/userpublic_frozen/) の v1 / v2 snapshot で、
`RenderPolicy::Status` だけが `buffer_too_small = 1` を持つため残り 4 面から**意図的に 1 ずれている**
(相互に `static_cast` してはならない。値は `src/core/gamelogic/gamelogicreload.cpp` の `static_assert` が固定する)。

pose staging の capability bit(`PoseStagingServiceCapabilityBitsV1`)は 4 本。
**service version を上げずに entry を足す**ための印で、既に 1 で交渉した client を落とさない:

| bit | 値 | 意味 |
|---|---|---|
| `pose_staging_deferred_commit` | `1 << 0` | staged pose の commit を後段へ遅らせる |
| `pose_staging_mutable_local_pose` | `1 << 1` | staged local pose を書き換えられる |
| `pose_staging_source_node_mapping` | `1 << 2` | staged frame に対する glTF node → layout index |
| `pose_staging_rig_source_node_mapping` | `1 << 3` | rig に対する同じ写像 + clip の channel kind(AG-2) |

`pelicanEngineVersion` は project の `engine_min_version` を検査する側の版(新しすぎれば `project.engine_too_old@1`)で、
`project init` と `project migrate` の apply が project.json の `saved_by_engine` に記録する版でもある。
**文書形式の互換を決めるのはこの定数ではなく、各文書の `(schema, version)` と移行手順の登録表**(§1、MG-1)。
`saved_by_engine` も記録だけで、何の判定にも使わない。tag は release marker であって互換の正本ではない(§6)
([設計 §1.2-4 / §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24)。

### 1.5 package manifest の最小形

package = `package.json`(`pelican.package` v1)を持つ 1 ディレクトリ。install は
`<project>/packages/<namespace>/` への copy で、engine は project の外の package を解決しない。
最小形は名前空間と版、必要な engine / SDK、依存、提供する型・op・部品、説明:

```json
{
  "schema": "pelican.package",
  "version": 1,
  "namespace": "acme.glowkit",
  "package_version": "1.0.0",
  "description": "...",
  "requires": { "engine_min_version": "0.1.0", "sdk_abi": 2, "capabilities": [] },
  "dependencies": [],
  "provides": { "features": [], "graphs": [], "components": [], "code": false }
}
```

雛形は [test/fixtures/package_template/](../test/fixtures/package_template/)、
key ごとの規約は [docs/manual/03_project_format.md](manual/03_project_format.md)。
`provides` のパスは package 内の相対のみ(絶対・`..`・backslash・scheme は `package_path_invalid`)。
`sdk_abi` は install 時に `PELICAN_GAME_LOGIC_ABI_VERSION` と照合され、実際の gate は DLL を読むときに
loader が解決する epoch anchor。project.json に記録される `sha256` は install 済み byte 全体の digest で、
`package list` が再計算する。

### 1.6 rendering config の root と 2 つの装着様式

**rendering config の root は `pelican.graph` root だけ**(MG-2b):

- 共通グラフ文書の `rendering` key の下に render config が入る形。出荷 7 本と `project init` の雛形(standard / blank)はすべてこの形。
- `rendering` を最上位に直接持つ旧い形(legacy root)は、読み込み・feature overlay・ray tracing の設定・編集文書のどこでも
  `render_config.legacy_root@1` で拒否する。P-2a-4b から雛形の生成が使っていた包み関数 `wrapLegacyRenderingConfig` は撤去し(雛形は graph root を直接書く)、
  出荷の legacy root 5 本は graph root に書き換えた。
- graph root の `rendering` 節には `buffers` / `compute_tasks` を書けない(lowering が拒否する)。graph node の `render.buffer` /
  `render.compute` か task feature で書く。

**姿勢の装着様式も 2 つある**。owner 単位で**排他**(同じ owner に両方は付かない):

- **animation component** — `AnimationComponent` の `clip` XOR `graph`(`pelican.anim_graph` 文書)。
  出荷の scene.json に `animation` component を持つ project は無い。`animgraph_demo` は同じ状態機械を game code の `EvaluatorV1` から直接駆動し、`movement.anim_graph.json` 2 本(animgraph_demo / vrm_xr_demo)はどの scene / code からも参照されない。この様式の実物は test fixture と componentcodec の受理規則(`clip` XOR `graph`)にある。
  API は `setGraphParameter` / `forceGraphState`。
- **`graph.instance`** — 共通グラフの `pose.*` op を `pose.skeletal` / `pose.transform` sink へ繋ぐ形。
  出荷は posegraph_demo。

AG-1b で「一本化」は **v0 から撤回**され、v0 が約束するのは**二つの経路の parity**
(loop / speed / start_time、位相、状態移送)と owner 単位の排他。二重 writer は
`authority_conflict` / `pose.sink_conflict@1` で名前付きに落ちる。

## 2. 撤去 / 維持の表

v1 計画 R 群「表の各行を『撤去 / 維持(理由)』で閉じる」の裁定(台帳 `### R-1:` の転記)。
**code の撤去は 0 行**で、この表の実体は「維持の理由を凍結一覧に固定する」こと。

| 行 | 裁定 | 理由 |
|---|---|---|
| 1 `pelican.value_graph` 直書き parse 分岐 | **維持** | 直書き形は `pelican.graph` の lowering 結果の**真上位集合**(`element_count == 2` の render_buffer binding、`SoundBindings` key を target にする audio binding は lowering が作れない)で、bindings decode の名前付き検査 7 種はこの形の test でしか到達しない。production の `Graph` 構築点は `renderingpassconfigregistration.cpp` の 1 箇所で `pelican.graph` のみ。**`pelican.value_graph` は内部 IR + test 面であって外部契約ではない** |
| 2 `bindings` 配列 | **維持** | 1 と同じ(IR) |
| 3 `AnimationComponent::graph` + `pelican.anim_graph` 2 本 | **維持** | AG-1b で「一本化」を v0 から撤回、parity が v0 の約束。**2 つの装着様式は v0 の契約(owner 単位で排他)**(§1.6) |
| 4 `AnimationComponent::parameters` | **維持** | 3 と同じ |
| 5 `setGraphParameter` / `forceGraphState` | **維持** | 3 と同じ |
| 6 `status == "committed"` の読み取り互換 | **撤去(MG-2a、`8af3227d`)**。R-1 の「維持(v0)」を置き換えた | R-1 の理由(黙った wedge と、rollback による破壊)は残るので、黙って読むのをやめ、両読み手を同時に**名前付きの停止**に替えた: 旧形式の dir は `render_config.legacy_transaction_dir@1`(path 付き)で止まり、dir は消さずに残る。手での片付けは [manual 3 章 §3.1](manual/03_project_format.md)。v0 より古い形式は作り直す、という 2026-09-24 の裁定([設計 §2](design_v0_production_gaps.md)) |
| 7 `withRootValue("nodes")` ×2 | **維持** | graph root key の書き手。N-2 が `definitions` を足す |
| 8 `compute_tasks` の root 分岐 | **撤去(MG-3、`892e6305`)**。当初の裁定は維持 | 1 規則(locator)。graph root では lowering が forbid = 到達不能。**MG-2b で legacy root が無くなり、editor の root scope で `compute_tasks` / `buffers` を編集する経路は全体が到達不能になった**。MG-3 で editor の到達不能な 4 分岐(`getComputeTaskAuthoring` の root 探索、`changeComputeTask` の root 書き込み、`getGraphInstances` / `getGraphNodeValues` の legacy 分岐)を消した。root 由来の task は今も一覧に `editable: false` で残り、編集は `compute_task_not_editable`(engine 側の test で固定)。F-6 の root pass 編集は到達可能なので残した |
| 9 `bloom_composite.frag` の `.spv` 非対称 / `shader_lab_*` family | **維持(記録)** | source-only は P-2a-2 の裁定。`shader_lab_*` は 8 本 family で、`engine_resources.json` は web ミラー test の正本なので触らない |
| 10 `docs/ci.md` の `pelican_golden_gpu` | **撤去(docs)** | code は `pelican_gpu` に統合済み |
| 11 overlay の semantic 流儀 vs authoring の byte 流儀 | **維持(記録)** | overlay は起動時の一時合成で文書に書き戻さない |

## 3. 約束しないもの

v1 計画の「v0 では約束しないもの」列と「やらない」節、および台帳 `### R-2:` の裁定。

| 領域 | v0 では約束しない |
|---|---|
| 文書形式 | 形を v0 のまま保つこと。約束するのは後の版へ移せること(§1。[設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24) |
| RPC / graph の op / native ABI | 名前と形を v0 のまま保つこと(§1.2〜§1.4 は記録。[設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24) |
| 文書・編集 | 保存の内部最適化、コンパイラの実装 |
| 共通グラフ | 全 domain の高度な最適化、全種類の完全な seek |
| パラメータ | 任意の式言語、双方向 bind |
| 描画 | 全 RT 機能の方式間互換、映画向け機能の網羅 |
| 拡張 | native ABI の永久互換、稼働中の任意 module の unload |

v0 の後に回すもの(v1 計画「やらない」): 双方向 bind / 任意の式言語 / layer・additive・IK・手付け /
2D ノード集 / 映像向け合成ノード / timeline 文書(時間軸は view)/ 物理 → graph 入力 /
native ABI の永久互換 / 任意 module の unload / マーケットプレイスと細分化された repo 群。

**複合 undo は v0 では約束しない**(`### R-2:` の裁定)。scene journal の対象は
scene 編集 13 op(OL-1 で `duplicate` が加わった)で、Graph Canvas は graph 専用の基本 Undo/Redo を持つ（§4.1）。
graph の履歴は CAS で保存し、外部編集後は無効化する。render config 全体の編集は
CAS + transaction による失敗時の原状維持が契約で、文書単位の rollback は内部にある。
**scene journal と graph / render config を 1 本の履歴へ束ねる複合 Undo**は v1 の項目として残す。

**代表ゲームの射程**: 代表は `projects/example` で、触るのは入力・ECS 生成と `setLocalTransform`・
VRM 表情・light animation まで。**物理 query・音・save/load・UI 操作・behavior のサンプルは v1**
(scene に collider 0、repo に音声 file 0、[docs/example_assets.md](example_assets.md) が新規 binary を禁じ、
`playSound` は `PELICAN_WITH_AUDIO=OFF` で throw = 「外す」と衝突する)。

## 4. v0 の状態と再検証

2026-09-22 時点の実装と確認範囲。完了済みの項目を未実装リストへ戻さず、
以前のレビュー指摘は現在の release 構成で再現を判定する。
旧表と途中の検証記録は [履歴](history/v0_delivery_2026-09.md) に残した。

### 4.1 解消・確認したもの

| 項目 | 現在の状態と根拠 |
|---|---|
| `graph.instance.owner` の ID | scene 保存時に `authoring_object_id` と採番上限を永続化。先行 object の削除・順序変更後の保存/再読込でも owner が別個体へ変わらない。`efb30dad`、`sceneformat_test` の `[persistent-identity]` と prefab 回帰 |
| Graph Canvas の基本 Undo/Redo | graph 専用の履歴を CAS で適用し、外部変更後は破棄する。ノード・接続と配置の掃除を同じ transaction に含めて復元する。`0d41aeba`、Studio の `[graph-history]` と編集 op の `[graph-undo]` 回帰。scene と跨る複合 Undo は §3 の範囲外 |
| 未保存 scene の close/project 切替 | Save / Discard / Cancel を経由し、保存失敗時は切り替えない。`e2a5ab7b`、model/widget 回帰と実 Studio 操作 |
| Studio の終了 crash | Windows の保存・project 切替後のアクセス違反を再現し、viewport 破棄前に通知を切断して修正。`a4bfeec2`、`[shutdown]` 回帰。scenario も異常終了・強制終了を失敗とする |
| configure の `requires` 無視 | configure と test sweep が同じ `projectrequirements.def` を参照し、OFF build で非対応の sample DLL target を作らない。`be35b7de` |
| 複数 build による配布物の上書き | runtime は `<build>/dist/<config>/`、配布物は `cmake --install --component Runtime` で集約する。`be35b7de`。現行 Windows の Release build と絶対パスの prefix への Runtime install は成功。配布物の確認範囲は §6 |
| Studio が `raytracing_demo` を開けないという旧指摘 | 現行 Windows の Runtime stage で起動、`scene_tree` / `get_frame_plan`、capture、別projectへの切替と戻りによる再読込、正常終了を確認。RT の二球の画像も確認した。他の OS / GPU の保証ではない |
| 出荷 sample の Studio 起動 | `example` / `raytracing_demo` / `pathtrace_demo` / `sprite_demo` / `animgraph_demo` / `posegraph_demo` / `vrm_xr_demo` の 7 本を現行 Windows Runtime stage で起動・capture・別 project を介した再読込・正常終了まで確認。機能全組合せや GPU 全数とは区別する |
| 新規 project からの編集・保存 | CLI init → Studio で位置編集 → FileSaveScene → 完全終了 → Studio 再起動を確認。authoring ID が維持され、`get_components` の authoring/runtime 両方で保存位置が一致し、capture・正常終了まで通過 |
| Studio で開いた ray tracing project の起動直後 | shader cache が効いて起動が速いと、窓の出力より先に最初の frame が来る。出力待ちで飛ばした frame が staged な起動 generation を巻き戻し、次の frame が `Renderer logical frame requires a compiled render pipeline` で落ちていた(pathtrace_demo の開き直しで 5 回中 2 回)。飛ばした frame では巻き戻さず、公開前の live material 検査も「取り除く経路が無い」として受け入れる。`7cb39996` / `e5995d88`。開き直し 10 / 10 |
| Studio 終了時に player が空回りする | Studio が viewport の親窓を先に壊すと、埋め込み player は close flag が立たないまま surface を毎 frame 作り直し(`ErrorSurfaceLostKHR` 168〜172 行)、Studio に kill されるまで回っていた。main loop が Win32 の窓の実在を見て止まる。`62245846`。配布確認が終了時の player log で 3 件以下を検査し、修正を外すと赤 |
| player のログが 1 ファイルに混ざる | 全 player が Studio の cwd の `pelican.log` に追記していた。player に `--log-file`(起動ごとに切り詰め)を足し、Studio は `<project>/.pelican/logs/player.log` を渡す。既定の `pelican.log` 追記は維持。`a21b28e5`、`devstudio_viewport_test` と配布確認 |
| engine の準備完了の判定 | engine は最初の frame より前に RPC に答えるので、capture や frame plan を待つ側が名前付きの「まだ」エラーを再試行していた。`get_status` に `render.published_frames`、automation の `wait_engine` に `frames` を足した。Frame Plan dock は公開前の plan を「失敗」と表示せず上限付き(250 ms × 40)で問い直す。dock の失敗表示は `e5995d88` 以後の再現 6 回では出ておらず、上限付きの保険として入れた。`f4a5e2cd` |
| staged な ray tracing 世代の行き先 | 公開 / 次の frame へ持ち越し / 巻き戻しの 3 つを `StagedRuntimeRollbackGuard` にまとめ、`published()` / `keepForNextFrame()` で名指しする。`RenderTarget` に test seam を足し、出力に image の無い frame を 2 回飛ばしても staged の世代が残り次の frame で公開される GPU テストを追加。`keepForNextFrame()` を外すと赤。`f4a5e2cd` |
| indirect command の padding | `RenderCommand` は 34 バイトの field を 36 バイトに詰めており、末尾 2 バイトの padding にスタックの残りが入って、GPU へ上げるバイト列が実行ごとに変わっていた。`state_batched_v1` の byte 比較がこの build で 40 回中 34 回赤。padding を 0 初期化の名前付きメンバーにし、`has_unique_object_representations` を static_assert。GPU が読む stride と先頭の Vulkan command は不変。`47cbf3cf`。0 / 40 |
| 配布確認の手順 | scratch の script だった確認を `test/ci/run_distribution_check.py` に移し、出荷 sample を `projects/` から自動で数える。手順は [CI 運用](ci.md)。`053b1e0b` |
| editor の gate 判定の一本化 | journal・render config・procedural asset が同じ `EditorGateEpoch` で受理後の commit 可否を判定し、遷移は型付きの `ReloadTransitionFacts` を 1 つの counter が数える。閉じて開き直した gate を procedural asset が見逃す穴も閉じた。`f9b43662` |
| 同一 logical frame の再描画 | 通常動作中の Studio で `render_frame` を重ねた際、GPU timing の sample ID が重複して fatal になる不具合を修正。同じ logical frame の同じ範囲を再記録しない。全 GPU gate の結果は §6 で別に扱う |
| Release の runtime 診断 | file のみだったログ出力を file + stderr に戻し、Studio の Engine Log とテストの捕捉 stderr へ届くよう修正。JSON-RPC の stdout は維持 |
| 既に閉じた旧指摘 | export の `.tmp-<pid>` 掃除と `structfieldjson.hpp` 複製は R-1、canvas の余白/pan は N-4、inline editor の意図しない FocusOut は Q-3 で解消。着地記録は §5 / 実装台帳 |

### 4.2 現行構成で再検証するもの

以下は日付付き台帳から引き継いだ確認対象であり、この一覧だけで現 HEAD の再現済み不具合とはしない。
対象の操作・構成で確認し、再現時は blocker / v1 送りの判断を残す。全履歴の再試験は要求しない。

| 対象 | 元の記録・確認する境界 |
|---|---|
| SceneLoader の二重 load / 放棄 transaction の teardown | `AG-1b` / `WP380e`。同 process の load と終了時の寿命 |
| animation frame の公開 | `frameIndex() == 0` の `setTime` 呼び手、pose 側 snapshot、component_clip / component_graph の status。描画側の値だけで完了としない |
| RPC OFF と mutation 判定 | `AG-1a` / `R-1` の `raytracing_rt3_1_production_test`、`WP386` の `looks_mutating` gate |
| AS / shader の同一性 | `WP386` の signed-zero transform identity、shader source 改行と cache key |
| graph root と preset の組合せ | `P-2a-4a` / `P-2a-3` の `region_replacements`、Frame Plan fixture と現在の生成結果 |
| export / 一時ファイル | per-process temp root 修正後の並列 `renderconfigeditor`、GPU lease log の残留。古い `%TEMP%` 件数を現在の原因の証拠にしない |
| Debug 専用 WSI 故障注入 | `wsi_fault_window_player` は Release で注入 hook がなく明示 skip。2026-09-24 に Debug build(`47cbf3cf`)で実行し、注入 6 回・surface 作り直し 5 回・公開 2 回で通過 |
| Debug の production PDB 配線 | `crash_symbol_production_wiring` は実際の Debug Studio が残した symbol archive を読むため Debug 構成だけで登録する。2026-09-24 に Debug build で実行し、Studio の `47cbf3cf` の PDB が archive されていることを確認 |
| オフライン時刻の上限 | `O-1` の `--start-time` / `llround` 境界。入力範囲の拒否と通常 replay の区別 |

最終 revision の GPU / Studio gate と配布物の結果はrelease完了時に §6 へ記録する。部分確認や過去の全数成功で代用しない。

## 5. v0 で入ったもの

台帳の各節の完了記録から。順は着地順ではなく段の順。

| 段 | SHA |
|---|---|
| AG-0 登録契約と purge | `c9085d6f` |
| AG-1a pose module | `ee751752` |
| AG-1b pose graph 経路の parity と排他 | `129eb218` |
| AG-2 root motion | `37cc55ba` |
| AG-3a 定義内の値(engine 側) | `32d9a20b` |
| P-1 パラメータの供給元と由来 | `3611e390` |
| P-2a-1a anchor 明示と撤去 | `60e95f53` |
| P-2a-1b Bloom の feature 化 | `6e04c7c8` |
| P-2a-1c define 化と materialize 後の編集 | `e3a2d3d0` |
| P-2a-2 runtime 値(`binding: buffer`) | `340f8537` |
| P-2a-3 + P-2a-4a 共有部品の公開 / graph root の `rendering` 節 | `7e983ed9` |
| P-2a-4b 出荷 project の graph root | `e1c326c1`(+ Studio hotfix `d69a4226`)|
| P-2b posegraph_demo と `requires` | `400820f0` |
| O-1 オフライン出力(`--start-time`) | `fcbe1a98` |
| X-1 持ち出し(package) | `64eae566` |
| R1-2 committed marker | `cc95a732` |
| R-1 撤去 / 維持の表と安い穴 | `96d5e1d8` |
| N-1 共通グラフの読み取りキャンバス | `edafc41d` |
| N-1b 見出しとソケット欄 | `6c47a8a4` |
| N-2 canvas の編集 op | `f2eccbfd` |
| R-3 サンプル索引と本文書 | `be4c60a4` |
| Q-1 Studio automation surface | `c026002d` |
| N-3 Frame Plan の共有 node 描画 + zoom / pan / fit | `d8d7387d` |
| Q-1b 自動化面の pointer / menu / view_state | `cd9e243f` |
| N-5 Studio theme light / dark | `6a26eef3` |
| N-4 自動配置(層配置)とパン | `142a34fc` |
| AG-3b 観測の Studio 面(値の帯 / Live / Timeline / e2e) | `ce5e96c0` |
| N-7 パネルの文字を減らす | `434c4534` |
| N-6 配線の制御点と置き場所の永続化 | `5bd02967` |
| N-8 Frame Plan のノード編集 | `8e48473f` |
| Q-2 Studio の応答性(idle polling / parse 1 回 / canvas cache) | `bf2776fc` |
| Q-3 帯の inline editor の focus 規則(窓の瞬きで閉じない) | `c90fde90` |
| Q-4 project open の固まり(embed の待ち方)/ Frame Plan fill / Frame Plan editor 規則 | `06787ace` |
| scene owner ID の永続化 | `efb30dad` |
| 未保存 scene の終了・project 切替ガード | `e2a5ab7b` |
| Graph Canvas の基本 Undo/Redo と配置の同時復元 | `0d41aeba` |
| Studio 終了通知の遮断と scenario 異常終了判定 | `a4bfeec2` |
| build ごとの runtime 配置・Runtime install・構成 gate 整理 | `be35b7de` |
| Graph Canvas の描画・配置組み立ての責務分離 | `2d996200` |
| ビルド機能台帳と実装履歴の分離 | `af9c0b82` |
| Release の診断ログと PowerShell runner | `0bbcd249` |
| 同一 logical frame の再描画時の GPU timing 重複防止 | `3d7454b7` |
| Studio scenario の必須化・GPU 分類・構成別 gate | `86c42502` |
| 描画 fixture の終了手順と固定 cache hash 検査の整理 | `104b53ec` |
| viewport resize で受理済みの編集が捨てられる不具合 | `ba82495f` |
| path tracing の出荷 sample(PT-1a〜1c、camera の viewport 修正) | `05959a0f` |
| editor の gate 判定の一本化と型付きの遷移 counter | `f9b43662` |
| Studio で開いた ray tracing project の起動直後の fatal | `7cb39996` / `e5995d88` |
| Studio 終了時の player の空回り | `62245846` |
| player の project ごとのログ(`--log-file`) | `a21b28e5` |
| 公開 frame 数による準備完了・staged 世代の行き先と test seam・Frame Plan dock の問い直し | `f4a5e2cd` |
| 配布確認の repo 化(`run_distribution_check.py`) | `053b1e0b` |
| Studio の fatal テストを project ごとの log に合わせる | `777b4824` |
| `RenderCommand` の padding を無くす | `47cbf3cf` |
| LP-0a `render.target_ref` と合成の後の解決の段 | `64da2de8` |
| LP-Va view の軸と `view_select` | `c8e47325` |
| LP-0b graph の pass の置き場所(`insert` / `canonical_anchor`) | `637f1fd9` |
| LP-Va2 `get_frame_plan` の `view_axes`、`edit_preset_pass` の `view_select` | `0d816570` |
| LP-0c definition の編集 op | `85208610` |
| LP-1a `graph.loop` の芯 | `63f296de` |
| LP-2 material pass の描き方の列(draw_loop の engine) | `50f4ea8f` |
| LP-1b ループの式・carry・回ごとの入力・`p[]` | `6f0f6360` |
| LP-0d `pass_replacements` と変種で外れる feature への依存 | `efda0ad8` |
| LP-3a `render.material_pass`・`draw.*`・draw_loop | `e8482fc3` |
| LP-0e `render.shadow_depth` / `render.snapshot_copy` | `361145d9` |
| LP-3b `decompose_material_pass` と draw_loop の要素の paging | `69badca7` |
| LP-1c bloom / planar の分解版の出荷 | `04016274` |
| LP-1d graph の port の番号付きの入力 | `6b114a3c` |
| LP-1e 置き場所の無い graph の pass とループの既定の位置 | `b429f31c` |
| LP-Vb `view_axis` のループ、cube capture と影の cascade の分解版 | `66544252` |
| RD-1 render 文書の丸ごとの読み取りと CAS の書き戻し | `3a0f55b1` |
| draw-loop の表の登録簿の静的破棄の順序 | `cf40ec93` |
| TI-1 test の CRT assert を stderr と終了へ | `3483aba1` |
| RD-1b `restore_render_documents` の作成と削除 | `6c77d4f8` |

## 6. v0.1.0 tag の手順

2026-09-17 の engine 全数 1947 / 1947（`450f2866`）は履歴である。
その構成には現在の全 Studio scenario が含まれず、現行 release の gate 通過とは扱わない。
tag は利用者が v0 を閉じる判断をした時点で付ける。

2026-09-22 のローカル検証は Windows / MSVC、Qt 6.10.3、Python 3.12、Vulkan SDK 1.4.x の
`build-v0` / Release で実施した。Studio、Python tests、runtime shader compiler は ON。
CPU / GPU / Studio gate、配布 stage、Debug 専用の確認は、2026-09-24 に `47cbf3cf` で行った(Release build、
そこから作った Runtime install の `build-v0/stage`、同じ tree の Debug build)。以後の revision で tag を
付けるときは、その revision で同じ確認をやり直す。

| 確認範囲 | 結果と制約 |
|---|---|
| Release build / Runtime install | 成功。install prefix は絶対パスを使用 |
| CPU gate | 2026-09-24、`47cbf3cf`。1,793 件中 1,792 PASS、directory symlink 権限による allowlisted skip 1 件、失敗 0。`build-v0/test_artifacts/v0-cpu-final6/` の CTest と exact SKIP policy が根拠 |
| 配布 stage の出荷 sample | 2026-09-24、`47cbf3cf`。`test/ci/run_distribution_check.py` で 7 / 7。起動・最初の frame を待って capture → 別 project へ切替 → 元の project を再読込・capture → Frame Plan dock が失敗表示で止まっていない → 正常終了を確認。同じ project の open が no-op になる経路ではない。project ごとの player log があり、Studio 終了時に生きていた player の surface lost は 7 本とも 0 回。`build-v0/test_artifacts/distribution/run-20260924-003010-57332/report.json` |
| 配布 stage の新規 project | 2026-09-24、`47cbf3cf`。同じ確認の中で CLI init → 位置編集 → 保存 → 完全終了 → Studio 再起動で authoring ID と保存値の持続、capture、正常終了を確認 |
| 全 GPU gate | 2026-09-24、`47cbf3cf`。275 件中 274 PASS、Debug 専用 WSI の allowlisted skip 1 件、失敗 0。実窓 Studio scenario 10 件はすべて観測・通過し、exact SKIP policy も PASS。`build-v0/test_artifacts/v0-gpu-final6/` が根拠 |
| Studio CPU gate | 2026-09-24、`47cbf3cf`。compiler ON なので空の allowlist で実行し、263 件中 263 PASS、skip 0、exact SKIP policy PASS。`build-v0/test_artifacts/v0-studio-final6/` が根拠 |
| Debug 専用の確認 | 2026-09-24、`47cbf3cf` の Debug build。`wsi_fault_window_player`(注入 6 回・surface 作り直し 5 回・公開 2 回)と `crash_symbol_production_wiring`(Studio の `47cbf3cf` の PDB が archive 済み)がどちらも通過 |

GPU gate の経緯: 09-22 初回の失敗 4 件は、Studio への fatal ログ通知、WP366/WP384 の frame publication
fixture 終了、`physics_trigger_behavior_e2e`、`behavior_project_e2e`(初回ログは `v0-gpu-final/`)。
修正後の再実行では `studio_scenario_graph_observe_roundtrip` が 1 件失敗した(`v0-gpu-verified/`)。
Inspector から graph を装着する編集が、受付後の viewport resize で `gate_closed` として捨てられていた。
resize が render pipeline を再 lowering して reload 状態の `details.generation` を更新し、それが editor の
遷移 epoch の hash に入っていたためである。`ba82495f` で遷移 epoch を scene、watcher、各 participant の
reload 結果だけから作るよう修正し、`f9b43662` で gate 判定を 1 つの規則と型付きの counter にまとめた。

配布確認の経緯: `fc1a6cb5` の配布確認で、Studio から pathtrace_demo を開き直すと player が落ちる不具合が見つかった
(上の §4.1「Studio で開いた ray tracing project の起動直後」)。`7cb39996` で直したとき `get_frame_plan` も
公開前の staged な generation を返すように変えたが、公開前の generation を非公開とする WP384 の契約に反し、
全 GPU gate の WP366/WP384 frame publication test が検出した。`e5995d88` でその部分だけ取り消した。

v0 に足した 6 項目(`62245846`〜`053b1e0b`)の後、`50f1fbda` の確認で 2 件赤が出た。全 GPU gate の `devstudio_engine_failure_test` は player の log を player の横の `pelican.log` と固定しており、project ごとの log へ移した変更と食い違った(`777b4824` で修正。控えの run が同じファイルを切り詰めることも検査する)。CPU gate の `state_batched_v1` は `RenderCommand` の padding を比べていた(§4.1、`47cbf3cf`)。上の表はその後の `47cbf3cf` での最終確認である。

1. 対象 revision と構成を固定する。Studio ON、Python 3.11 以上と `PELICAN_PYTHON_TESTS=ON`、runtime shader compiler ON を使う。必要な機能の基準値は [ビルド機能台帳](build_features.md)、実行コマンドは [CI 運用](ci.md) を参照する。
2. 影響範囲の CPU 回帰と最終 GPU gate を通し、Studio CPU gate と実窓 scenario が未登録・skip になっていないことを確認する。`run_gpu_gate.py --engine-only` は限定検証用であり release 判定には使わない。古い件数を成功条件に固定しない。
3. Runtime install で作った配布物から、新規 project → 編集 → 保存 → 再起動の経路と出荷 sample を確認する(`test/ci/run_distribution_check.py` を GPU lease の下で実行。[CI 運用](ci.md))。v1 計画の「作る / 学ぶ / 出力する / 拡張して持ち出す」の 4 シナリオについて、確認した経路と残る制限を残す。
4. §4 の再検証結果と既知の制限、利用した revision・構成・実行範囲を記録し、文書・索引を commit する。
5. 利用者の release 判断後に `git tag -a v0.1.0 -m "pelican v0 (format 0.1.0)"` を実行する。

tag は **release marker** であり、tag 名は `src/project/projectformat.hpp` の
`pelicanEngineVersion`（`"0.1.0"`。`project init` / `project migrate` が `saved_by_engine` に書く版）に合わせる。
tag も定数も文書形式の互換の正本ではない。v0 形式の project を後の版で開けるようにするのは §1 の移行の約束と、v0 形式の実物を固定した
`test/fixtures/project_corpus/v0/` の corpus である([設計 §1.4](design_v0_production_gaps.md)、利用者の意図 2026-09-24)。
手順 5 の tag message の「format 0.1.0」は engine の版 0.1.0 で出した v0 形式を指し、形式を固定する宣言ではない。
