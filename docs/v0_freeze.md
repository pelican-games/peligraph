# v0 凍結一覧(pelican 0.1.0)

v1 計画 [docs/v1_development_plan.md](v1_development_plan.md) の「v0 で凍結するもの / 広く約束しないもの」の
5 行表を、実物の名前に展開した正本。**この文書が凍結の一覧であり、v1 計画の表は要約**。

読み方: §1 が「動かさないと約束するもの」、§2 が「二つの正本のうちどちらを残したか」、
§3 が「v0 では約束しないもの」、§4 が「解消済みの項目と現行構成で再検証するもの」、
§5 が「v0 に入った変更の SHA」、§6 が tag の手順。

## 1. 凍結するもの

### 1.1 文書 schema と version

project が持てる文書と、parser が受ける version。`schema` と `version` の組は**厳密一致**で、
知らない組は名前付きで拒否される。番号は文書ごとに独立で、op の意味・package・native ABI とは別の番号
(v1 計画の「schema version の分離」)。

| schema | version | parser | 備考 |
|---|---|---|---|
| `pelican.project` | 1 | `src/project/projectformat.cpp` | `project.json`。`engine_min_version` と `requires` を持つ |
| `pelican.scene` | 1〜2 | `src/project/sceneformat.cpp` | 保存は読み込んだ版を維持する。prefab は scene v2 を要求し、永続IDは両版で受理する |
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

**複合 undo は v0 では約束しない**(`### R-2:` の裁定)。scene journal の対象は
scene 編集 12 op で、Graph Canvas は graph 専用の基本 Undo/Redo を持つ（§4.1）。
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

tag は **release marker** であり、format 互換の正本は `src/project/projectformat.hpp` の
`current_engine_version`（`"0.1.0"`）。tag 名はこの定数に合わせる。
