# v0 凍結一覧(pelican 0.1.0)

v1 計画 [docs/v1_development_plan.md](v1_development_plan.md) の「v0 で凍結するもの / 広く約束しないもの」の
5 行表を、実物の名前に展開した正本。**この文書が凍結の一覧であり、v1 計画の表は要約**。

読み方: §1 が「動かさないと約束するもの」、§2 が「二つの正本のうちどちらを残したか」、
§3 が「v0 では約束しないもの」、§4 が「約束の内側にあるが今日は穴になっているもの」、
§5 が「v0 に入った変更の SHA」、§6 が tag の手順。

## 1. 凍結するもの

### 1.1 文書 schema と version

project が持てる文書と、parser が受ける version。`schema` と `version` の組は**厳密一致**で、
知らない組は名前付きで拒否される。番号は文書ごとに独立で、op の意味・package・native ABI とは別の番号
(v1 計画の「schema version の分離」)。

| schema | version | parser | 備考 |
|---|---|---|---|
| `pelican.project` | 1 | `src/project/projectformat.cpp` | `project.json`。`engine_min_version` と `requires` を持つ |
| `pelican.scene` | 1〜2 | `src/project/sceneformat.cpp` | 書き出しは 2(`latest_scene_version`)。prefab は scene v2 を要求する |
| `pelican.graph` | 1 | `src/project/graphcomposition.cpp` | 共通グラフ。`definitions` / `nodes` / `outputs` と `graph.instance` / `graph.input` |
| `pelican.value_graph` | 1 | `src/core/userpublic/valuegraph.cpp` | **内部 IR + test 面**。外部契約ではない(§2 行 1) |
| `pelican.anim_graph` | 1 | `src/core/userpublic/animation/animgraph.cpp` | animation component が指す state machine 文書 |
| `pelican.render_feature` | 1 | `src/project/featurecompose.cpp` | |
| `pelican.render_feature_parameters` | 1 | `src/project/featurecompose.cpp` | |
| `pelican.render_feature_overlay` | 1 | `src/project/renderfeatureoverlay.cpp` | 起動時の一時合成。文書に書き戻さない(§2 行 11) |
| `pelican.render_component` | 1 | `src/project/rendercomponent.hpp` | export / import される部品 |
| `pelican.render_pipeline` | 1 | `src/project/renderpipeline.cpp` | preset(`hybrid_v1` など) |
| `pelican.package` | 1 | `src/project/packagemanifest.hpp` | §1.5 |
| `pelican.assets` | 1 | `src/project/assetsmanifest.cpp` | `assets.manifest.json` |
| `pelican.assets_cache` | 1 | `src/project/assetsmanifest.cpp` | 生成物 |
| `pelican.asset_data` | 1〜2 | `src/project/assetdataformat.cpp` | 現行は 2 |
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
| `pelican.ray_group_layout` | 1〜3 | `src/core/shader/raygrouplayout.cpp` | 出荷 raytracing_demo は 3 |
| `pelican.ray_bindings` | 1〜2 | `src/core/shader/raygrouplayout.cpp` | 出荷 raytracing_demo は 2 |
| `pelican.graph_catalog` | 1 | `src/project/graphmodule.cpp` | `pelican_cli graph-catalog` の出力(§1.3) |
| `pelican.project_catalog_smoke` | 1 | `test/run_project_catalog_smoke.cmake` | project ごとの smoke 閾値 |

### 1.2 RPC method 名

stdio JSON-RPC の枠組み(`jsonrpc` / `id` / `method` / `params`、名前付き error)が凍結対象で、
**params と戻り値の表は [docs/manual/10_tools.md](manual/10_tools.md) が正本**(ここでは重複させない)。

基盤 29 本(`src/core/communication/rpcserver.cpp` の登録):

`ack_modal_transform`, `cancel_modal_transform`, `capture`, `capture_gpu`, `get_frame_plan`, `get_gpu_timing`, `get_modal_transform`, `get_status`, `inject_event`, `inject_input`, `load_gltf`, `load_scene`, `pick_object`, `query_gizmo_drag_basis`, `query_gizmo_handle`, `raycast`, `reload_game_logic`, `render_frame`, `set_camera`, `set_gizmo`, `set_input_profile`, `set_seed`, `set_time`, `start_input_record`, `start_input_replay`, `step_frame`, `stop_input_record`, `stop_input_replay`, `update_transforms`

エディタ拡張 60 本(`src/core/communication/editorrpchandlers.cpp` の登録。詳細は [docs/manual/13_editor.md](manual/13_editor.md)):

`abort_preview`, `add_authored_pass`, `add_compute_task`, `can_edit`, `can_preview`, `commit_preview`, `edit`, `edit_compute_task`, `edit_frame_node_order`, `edit_graph_document`, `edit_graph_instances`, `edit_preset_pass`, `edit_procedural_asset`, `edit_ray_callable_records`, `edit_region_replacement`, `edit_render_features`, `edit_scene_acceleration_structures`, `eval_preview`, `export_render_feature`, `export_scene_snapshot`, `fork_render_pipeline`, `get_component_add_candidates`, `get_components`, `get_compute_task_authoring`, `get_edit_result`, `get_frame_node_order`, `get_graph_catalog`, `get_graph_instances`, `get_graph_node_values`, `get_graph_values`, `get_preview_result`, `get_procedural_assets`, `get_ray_callable_records`, `get_ray_occurrences`, `get_render_authoring_context`, `get_render_features`, `get_scene_acceleration_structures`, `get_scene_revision`, `import_render_feature`, `import_scene_snapshot`, `install_package`, `list_assets`, `list_render_features`, `open_editor_session`, `open_preview`, `query_journal`, `redo`, `remove_authored_pass`, `remove_compute_task`, `render_preview`, `resolve_ray_occurrence`, `resume_editor_session`, `save_scene`, `scene_tree`, `set_prefab_parameter`, `undo`, `uninstall_package`, `unpack_prefab`, `unset_prefab_parameter`, `update_preview`

### 1.3 共通グラフ: module / op / 型 / sink

登録 module は 4 本(`pelican.value` / `pelican.audio` / `pelican.render` / `pelican.pose`)で、いずれも module version 1。
下の表は **`PELICAN_WITH_ANIMATION=ON` build の `pelican_cli graph-catalog` の出力**を展開したもの(op 27 本)。
`change` は parameter を変えたときの既定の扱い(`value` = 値だけ / `rebuild` = 作り直し)、
`ports: parameter_dependent` の op は port 一覧を宣言せず node 自身の `inputs` / `outputs` から決まる。
`?` の付いた入力は任意。

**`PELICAN_WITH_ANIMATION=OFF` build では `pelican.pose` module が無い**。
catalog は 3 module / 21 op になり、下表の `pose.*` 6 本(`pose.blend` / `pose.clip_sample` /
`pose.retarget` / `pose.skeletal` / `pose.state_machine` / `pose.transform`)だけが消える。
この関係は `test/run_devcli_graph_catalog.cmake` が「この表の op 集合 == ON の catalog、⊇ OFF の catalog」として検査する
(表の範囲は下の 2 つのコメント行で区切る。行を足すときは ON の catalog に載る op だけ)。

<!-- graph-op-table:begin -->
| op | module | domain | evaluation | change | sink | ports | inputs | outputs |
|---|---|---|---|---|---|---|---|---|
| `audio.pitch` | pelican.audio | values | direct | value | sink | declared | `value`, `voice`? | — |
| `audio.playback` | pelican.audio | values | replay | value | — | declared | `events`, `time` | `voice`: pelican.audio.voice@1 |
| `audio.volume` | pelican.audio | values | direct | value | sink | declared | `value`, `voice`? | — |
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
| `render.raster` | pelican.render | rendering | direct | rebuild | — | parameter_dependent | — | — |
| `render.scalar` | pelican.render | values | direct | rebuild | sink | declared | `value` | — |
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

登録されている型(semantic)は 11: `pelican.value.scalar@1` / `pelican.audio.voice@1` /
`pelican.event.playback_control@1` / `pelican.render.*` 6 種(`storage_buffer` / `attribute` /
`color_signal` / `depth` / `legacy_opaque_image` / `legacy_opaque_resource`) /
`pelican.pose@1` / `pelican.rig@1`。値・姿勢・音の port を流れるのは前の 4 つで、
`render.*` の op は解決時に `pelican.render.*` を自分の port に載せる(上表の parameter_dependent)。
`pelican.rig@1` は登録だけで、どの port にも現れない(node の `parameters.rig` が指す同一性)。
`pelican.pose@1` は **handle と layout 世代**が流れるのであって bytes ではない。
sink は 5 本(`audio.pitch` / `audio.volume` / `render.scalar` / `pose.skeletal` / `pose.transform`)で、
pose の sink は **owner 単位の単一 writer**(二重 writer は `pose.sink_conflict@1`)。

### 1.4 ABI と版

| 面 | 版 | 置き場 |
|---|---|---|
| Animation ABI v1 | `abiVersionV1 = 1` / `descriptorVersionV1 = 1` | `src/core/userpublic/animation/abi_v1.hpp`([docs/animation_abi_v1.md](animation_abi_v1.md) が Frozen 宣言) |
| pose staging service | descriptor 1 / service 1 | `src/core/userpublic/animation/pose_staging_v1.hpp` |
| game logic SDK epoch | `PELICAN_GAME_LOGIC_ABI_VERSION = 2` | `src/core/userpublic/gamelogic.hpp` |
| engine / 文書互換 | `current_engine_version = "0.1.0"` | `src/project/projectformat.hpp` |
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

`current_engine_version` は project の `engine_min_version` を検査する側の版で、
**format 互換の正本はこの定数**。tag は release marker であって互換の正本ではない(§6)。

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

### 1.6 2 つの root と 2 つの装着様式

**rendering config の root は 2 つある**。どちらも v0 の契約:

- **legacy root** — `rendering` を最上位に持つ従来の render config。出荷 project の多くはこちら。
- **`pelican.graph` root** — 共通グラフ文書の `rendering` key の下に同じ render config が入る形。
  出荷では `example`(P-2a-4b で graph root 化)と `posegraph_demo` の 2 本がこの形。残り 4 本は legacy root のまま(`wrapLegacyRenderingConfig` が読み時に包む)。`graph.instance` を書けるのは graph root の下だけ。

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
| 6 `status == "committed"` の読み取り互換 | **維持(v0)** | 読み手は `renderconfigtransaction.cpp` の 2 箇所。消すと legacy dir のある project が**二度と commit できない wedge**になり、「prepared 扱いで rollback」は committed 済み編集を消す破壊操作。v1 で両読み手を同時に消す |
| 7 `withRootValue("nodes")` ×2 | **維持** | graph root key の書き手。N-2 が `definitions` を足す |
| 8 `compute_tasks` の root 分岐 | **維持** | 1 規則(locator)。graph root では lowering が forbid = 到達不能 |
| 9 `bloom_composite.frag` の `.spv` 非対称 / `shader_lab_*` family | **維持(記録)** | source-only は P-2a-2 の裁定。`shader_lab_*` は 8 本 family で、`engine_resources.json` は web ミラー test の正本なので触らない |
| 10 `docs/ci.md` の `pelican_golden_gpu` | **撤去(docs)** | code は `pelican_gpu` に統合済み |
| 11 overlay の semantic 流儀 vs authoring の byte 流儀 | **維持(記録)** | overlay は起動時の一時合成で文書に書き戻さない |

## 3. 約束しないもの

v1 計画の「v0 では約束しないもの」列と「やらない」節、および台帳 `### R-2:` の裁定。

| 領域 | v0 では約束しない |
|---|---|
| 文書・編集 | 保存の内部最適化、コンパイラの実装 |
| 共通グラフ | 全 domain の高度な最適化、全種類の完全な seek |
| パラメータ | 任意の式言語、双方向 bind |
| 描画 | 全 RT 機能の方式間互換、映画向け機能の網羅 |
| 拡張 | native ABI の永久互換、稼働中の任意 module の unload |

v0 の後に回すもの(v1 計画「やらない」): 双方向 bind / 任意の式言語 / layer・additive・IK・手付け /
2D ノード集 / 映像向け合成ノード / timeline 文書(時間軸は view)/ 物理 → graph 入力 /
native ABI の永久互換 / 任意 module の unload / マーケットプレイスと細分化された repo 群。

**複合 undo は v0 では約束しない**(`### R-2:` の裁定)。journal / undo の対象は **scene 編集 12 op だけ**で
(`editorjournal.cpp`。他は `method_unavailable`)、render config 編集は **CAS + transaction** の
「失敗したら何も変わらない」が契約。文書単位の inverse は既にある
(`RenderConfigDocumentCommitReceipt` + `rollbackCommittedRenderConfigDocuments`、graph 編集は `nodes` 1 key)。
無いのは **scene journal と render config 編集を 1 本の undo stack に束ねる複合 undo** で、これは v1 計画の項目そのもの。
理由: v0 の 4 シナリオに要らない / editor journal の既知の弱点(WP372 系)を先に片付ける必要がある。

**代表ゲームの射程**: 代表は `projects/example` で、触るのは入力・ECS 生成と `setLocalTransform`・
VRM 表情・light animation まで。**物理 query・音・save/load・UI 操作・behavior のサンプルは v1**
(scene に collider 0、repo に音声 file 0、[docs/example_assets.md](example_assets.md) が新規 binary を禁じ、
`playSound` は `PELICAN_WITH_AUDIO=OFF` で throw = 「外す」と衝突する)。

## 4. v0 の既知の穴

約束の内側にあるが今日は塞がっていないもの。**行番号ではなく台帳の節名**で引く。

| 穴 | 記録している節 |
|---|---|
| `graph.instance.owner` の authoring object id — **解消**。scene 保存時に ID と次の採番値を永続化し、削除・再読込後も別個体へ付け替わらない | `sceneformat` / `AuthoringSceneDocument` の `[persistent-identity]` 回帰テスト |
| 加速構造 row transform の identity 比較が ±0 を別物とする(`accelerationstructure.cpp`) | 「未了項目と再開順」 |
| `frameIndex() == 0` の間は animation frame を公開できない(`setTime` だけで駆動する headless 呼び手) | 「未了項目と再開順」現況 |
| `SceneLoader::load` を 2 回呼ぶと hang | `### AG-1b:` |
| 放棄した scene transaction の teardown で hang | `### WP380e:` / 「未了項目と再開順」 |
| `looks_mutating` gate | `### WP386: RT3-2` |
| `PELICAN_WITH_RPC=OFF` build で落ちる test(`raytracing_rt3_1_production_test.cpp` の `#if`) | `### AG-1a:` / `### R-1:`(安い穴) |
| Studio が raytracing_demo を開けない(`renderer.cpp`) | `### WP386: RT3-2` |
| temp dir を共有する `renderconfigeditor` 6 件が並列で赤 — R-1 で per-process temp root(flake は再現せず。真の候補は `gpu_lease_run.cpp:94` が lease log を消さないこと: `%TEMP%` に 3358 個)| `### O-1:` / `### R-1:`(安い穴) |
| export の `.tmp-<pid>` が残る — **R-1 で閉じた**(次の export が掃除)| `### P-2a-3:` / `### R-1:`(安い穴) |
| `structfieldjson.hpp` の呼び手無し複製 — **R-1 で閉じた** | `### R-1:`(安い穴) |
| shader source の改行と cache key | `### WP386: RT3-2` |
| preset + graph root の `region_replacements` | `### P-2a-4a:` / `### P-2a-3:` |
| `--start-time` の `llround` overflow(≈1e17 s。今日は起きない) | `### O-1:` |
| `requires` を configure が読まず OFF build で DLL の link に失敗する — **解消**。configure と test sweep が同じ宣言表を使い、要求を満たさない sample target を作らない | `projectrequirements.def` / `pelican_project_requirements.cmake` |
| source tree の `dist_debug/` を別 build が上書きする — **解消**。実行ファイルは `<build>/dist/<config>/`、配布物は `cmake --install --component Runtime` でまとめる | `pelican_runtime_layout.cmake` / README |
| `pelican_studio` の終了時クラッシュ — **終了時に届く RPC 通知の経路を修正**。Windows で保存・project 切替後のアクセス違反を再現し、viewport 破棄前の通知切断で正常終了を確認。scenario runner も異常終了を失敗として扱う | `EmbeddedViewport::~EmbeddedViewport` / `[shutdown]` 回帰テスト |
| `test/gpu_lease_run.cpp:94` が GPU lease の log を消さず `%TEMP%` に `pelican_gpu_lease_v1.<pid>.log` が数千個積もる(R-1 の temp dir flake の真の候補)| `### R-1:` の完了記録 |
| N-3 のパンは scroll bar 経由なので、**描画が view に収まっている間は中ボタンドラッグが何も動かさない**(使い勝手の穴)| `### Q-1b:` の着地前の実測(2026-09-17)。**N-4 `af6e335f` で解消**(view が余白を持ち `centerOn` で動く)|
| `studio_scenario_graph_edit_roundtrip` が赤(帯の editor が ~500 ms で閉じる)| 根本は N-2 `02a3d285` の `FocusOut` 規則。この機械では導入 commit `06829720` から一度も緑でなかった(Q-1 の記録は別 tree)。`83645084` の全数 1947 はこのシナリオを含まない。**Q-3 `9be4113f` で修正**(環境依存の timing ではなく実欠陥) |

## 5. v0 で入ったもの

台帳の各節の完了記録から。順は着地順ではなく段の順。

| 段 | SHA |
|---|---|
| AG-0 登録契約と purge | `adb73793` |
| AG-1a pose module | `5bc79586` |
| AG-1b pose graph 経路の parity と排他 | `945ad8aa` |
| AG-2 root motion | `584afc15` |
| AG-3a 定義内の値(engine 側) | `9862ebdd` |
| P-1 パラメータの供給元と由来 | `b960edf7` |
| P-2a-1a anchor 明示と撤去 | `70982870` |
| P-2a-1b Bloom の feature 化 | `a1233ddd` |
| P-2a-1c define 化と materialize 後の編集 | `9e4081b4` |
| P-2a-2 runtime 値(`binding: buffer`) | `62f09ce0` |
| P-2a-3 + P-2a-4a 共有部品の公開 / graph root の `rendering` 節 | `5100be5f` |
| P-2a-4b 出荷 project の graph root | `d236822b`(+ Studio hotfix `2f2aadce`)|
| P-2b posegraph_demo と `requires` | `393f9d30` |
| O-1 オフライン出力(`--start-time`) | `405cd319` |
| X-1 持ち出し(package) | `3f72b71d` |
| R1-2 committed marker | `edf4bd43` |
| R-1 撤去 / 維持の表と安い穴 | `aa56a1af` |
| N-1 共通グラフの読み取りキャンバス | `2c6828e4` |
| N-1b 見出しとソケット欄 | `373f62ee` |
| N-2 canvas の編集 op | `02a3d285` |
| R-3 サンプル索引と本文書 | `979b40f7` |
| Q-1 Studio automation surface | `06829720` |
| N-3 Frame Plan の共有 node 描画 + zoom / pan / fit | `dd088c2a` |
| Q-1b 自動化面の pointer / menu / view_state | `026268ee` |
| N-5 Studio theme light / dark | `43f032b1` |
| N-4 自動配置(層配置)とパン | `af6e335f` |
| AG-3b 観測の Studio 面(値の帯 / Live / Timeline / e2e) | `5d64bd6e` |
| N-7 パネルの文字を減らす | `5865a6fd` |
| N-6 配線の制御点と置き場所の永続化 | `fd02e356` |
| N-8 Frame Plan のノード編集 | `6c71c5af` |
| Q-2 Studio の応答性(idle polling / parse 1 回 / canvas cache) | `cb76c164` |
| Q-3 帯の inline editor の focus 規則(窓の瞬きで閉じない) | `9be4113f` |
| Q-4 project open の固まり(embed の待ち方)/ Frame Plan fill / Frame Plan editor 規則 | `0547c0ac` |

## 6. v0.1.0 tag の手順(2026-09-17: engine 側の全数 1947 / 1947 緑 @ `83645084`。tag は Studio 側(N 系 / AG-3b)が入り利用者が v0 を閉じるまで打たない)

1. 全数(GPU 込み)を **1 回**通す。
2. `docs/v0_freeze.md` と索引の変更を commit する。
3. `git tag -a v0.1.0 -m "pelican v0 (format 0.1.0)"`。

tag は **release marker** であって、format 互換の正本は `src/project/projectformat.hpp` の
`current_engine_version`(= `"0.1.0"`)。tag 名はこの定数に合わせる。
