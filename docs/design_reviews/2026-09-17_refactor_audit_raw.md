# リファクタ監査: 領域別の生報告(6 体)

日付: 2026-09-17(source `990b9eaa`)
判定と優先順位は [2026-09-17_refactor_audit.md](2026-09-17_refactor_audit.md)。本書は Opus subagent 6 体の報告を原文のまま並べたもの。
行番号・行数・重複行数は各 agent の実測。Fable が再確認したのは判定文書 §1 の 13 件だけ。

領域: 1 vkcore / renderer、2 renderingpass、3 communication / watch、4 loader / userpublic / ecs / persistence、5 shader / material / model / fullscreenpass、6 animation / gamelogic / phys / os / openxr / imgui / appflow / playback / top-level

---

## vkcore / renderer

監査対象: `src/core/vkcore/` と `src/core/renderer/`

---

### 1. `Renderer::renderLogicalFrame` は 1229 行の単一関数 — HIGH

**`src/core/vkcore/renderer.cpp:4840-6068`**

1 関数が 1229 行。内部に 13 個のラムダ（うち `record_acceleration_structures` = 4902-5054 の **153 行**、`prepare_view_family_states` = 5472-5588 の **117 行**、`capture_picking_model_instances`、`record_scene_as`、`publish_frame_generation` 等）と、第 1 階層のローカル変数だけで 57 個。全ラムダが `[&]` で同じスコープを掴むため、どの変数がどの時点で確定しているかがコードから読めない（例: `acceleration_structure_build` は 4886 行で宣言され、5054 行のラムダ内で `emplace` され、5063 行の別ラムダで参照され、5965 行で `commit()` される）。さらに 5661-5815（view-family 実行）と 5817-6006（sequential 実行）の 2 分岐は、`triggerUpdate` → `record_graph_deformation` → `record_scene_as` → `capture_picking_model_instances` → `prepareSecondaryViewFamilyDraws` → format 検証 → `prepare_view_family_states` → `executeRenderingPasses` → external depth → XR mirror → trace push という**同じ 90 行前後の列を 2 回書いている**（差は `view_index` vs `0`、`true` vs `false` のみ）。

**なぜ問題か**: フレーム記録の不変条件（AS の記録は 1 回、picking capture は resize 後、frame_resources の select 順）が全部この 1 スコープの暗黙の行順に埋まっている。片方の分岐だけ直す事故が構造的に起きる。

**方向**: ラムダ群をまず `LogicalFrameRecorder` クラスの private メソッドへ（`[&]` キャプチャをメンバに昇格）。そのうえで 2 分岐を `recordOneExecution(context, view_index, view_count, bool is_view_family)` に一本化し、`if` は `beginViewFamily/beginView` の取得部だけに残す。

---

### 2. `PreparedSceneState` が本体状態の手書きシャドーコピー — HIGH

**`src/core/renderer/polygoninstancecontainer.hpp:319-403`（メンバ）と `:405-441`（`PreparedSceneState` の 28 フィールド）**
**`polygoninstancecontainer.cpp:437-468`, `:491-534`, `:640-698` vs `:850-903`, `:1257-1284` + `:1326-1340` + `:1396-1413`**

`PreparedSceneState` は `PolygonInstanceContainer` の private メンバ 28 個を**そのまま複製した型**。そのため 1 フィールド追加すると最低 6 箇所を同期する必要がある: 構造体定義 / `snapshotPreparedScene`（31 行の designated-initializer 列挙）/ `publishPreparedScene`（28 行の `swap` 列挙）/ `publishPreparedSceneModel` と `publishModelInstance` の push_back と else の 4 ブロック / `prepareModelInstanceRebuild` の alias 20 行 + `next_*` コピー 15 行 + move-back 15 行。実際 `publishPreparedSceneModel`（640-698、55 行）と `publishModelInstance`（850-903、54 行）は **`prepared.` 接頭辞と assert を除いて行単位で同一**。さらに `prepareModelInstanceRebuild`（1246-1431）冒頭は `auto &model_instances_data = prepared.model_instances_data;` のようにメンバと**同名のローカル参照を 20 個作って意図的にシャドーイング**しており、alias し忘れたメンバに触ると静かに `this` 側を書き換える。

**なぜ問題か**: 同期漏れがコンパイルエラーにならない。publish 時に 1 本だけ古い配列が残ればフレーム間で slot index がずれ、GPU 側で別モデルのスキン行列を読む種類のバグになる。

**方向**: slot 並列配列（`model_instances_data` 〜 `morph_history_valid` の 16 本）を 1 個の `ModelInstanceSlotTable` 値型にまとめ、`PreparedSceneState` を `{ slots, draw_inventory, compiled_draw_queue, ray_*, overrides }` の 5〜6 フィールドに縮める。`snapshot`/`publish` は既定のコピーと `swap` で済み、列挙 3 箇所と `next_*` コピー 15 行が消える。**このディレクトリで最も費用対効果が高い改修。**

---

### 3. `PolygonInstanceContainer` は 109 メソッドの神クラス — HIGH

**`src/core/renderer/polygoninstancecontainer.hpp:305-733`（.cpp は 2991 行）**

1 クラスに public メソッド 109 個、データメンバ約 90 個。担当は model instance のライフサイクル / TRS / skinning palette / morph target / material override（相対・絶対の 2 系統）/ VRM / animation frame writer / ray tracing の scene plan・occurrence・declaration / draw queue 構築 / indirect buffer / view family culling / shadow draw / temporal history と、少なくとも 8 つの独立した関心事。さらに public に `*ForTesting` が **40 個**あり、テスト用アクセサが本番 API と同じ面に並んでいる。

**なぜ問題か**: 変更の影響範囲が事前に絞れない。ray tracing の修正が morph のテストを落とす類の結合が構造的に発生する。

**方向**: 所有の境界で分割する。まず `MaterialOverrideStore`（相対＋絶対 override とその GPU buffer、約 10 メソッド）と `ViewFamilyDrawPlanner`（`prepareViewFamilyDraws` 系、約 8 メソッド）を別クラスへ切り出すのが独立性が高く低リスク。`*ForTesting` は `friend struct PolygonInstanceContainerTestAccess` 1 個に集約。

---

### 4. `SwapchainFrameTarget::Impl` は 2419 行の一枚岩 — HIGH

**`src/core/vkcore/swapchainframetarget.cpp:469-2887`**

1 つの `struct Impl` に 25 メソッド・2419 行。内訳: `pumpMaintenance` 1604-1908（**305 行**）、`beginFrame` 1981-2262（**282 行**）、`buildEpoch` 963-1179（217 行）、`publishFrame` 2436-2636（201 行）、`workerMain` 1213-1364（152 行）。担当は swapchain 生成 / surface epoch と device-lost 復旧 / `std::jthread` ワーカーと mutex / フレーム取得・提出・publish / output transform copy / readback の 6 つ。`pumpMaintenance` は `completion` の 3 分岐（published / stale candidate / failure）がすべてインラインで、失敗分岐は `decision.action` の 5 段 `else if` が入れ子に入る。

**なぜ問題か**: WSI 復旧は本来デバッグが難しい領域で、そこが一番読めない形をしている。状態遷移の網羅性をレビューで確認できない。

**方向**: `pumpMaintenance` の 3 分岐を `publishCompletedEpoch()` / `deferStaleCandidate()` / `handlePreparationFailure()` の 3 メソッドに分ける（純粋な抽出で挙動不変、テストもそのまま通る）。さらに worker（`workerMain` / `worker_mutex` / `worker_request` / `worker_completion`）を `SwapchainBuildWorker` として分離すれば Impl は約 1500 行になる。

---

### 5. 2 つの reload 経路で publish/rollback ラムダが重複、しかも順序がずれている — HIGH

**`src/core/vkcore/renderer.cpp:3364-3428` vs `:3642-3705`（publish）、`:3430-3455` vs `:3707-3731`（rollback）**

`reloadRenderPipelineFromDisk`（3334-3605）と `applyRenderPipelineAuthoringCandidate`（3607-3851）が、トランザクションの `publish` / `rollback` ラムダを別々に持っている。publish は約 63 行がほぼ完全一致（`variants` の move、`flat/xr_rendering_pass_id` 代入、`publishRenderingConfigJson`、try/catch での `resetHistory`/`resetTemporalHistory`/`layout_tracker.reset()`、`render_pipeline_reload_state` 更新、`outcome` 構築、`completion` 呼び出し）。rollback も約 25 行が一致。**ただし `transaction->finalized = true;` の位置が片方は `outcome` 代入の前、もう片方は後**になっており、コピーが既にドリフトしている。

**なぜ問題か**: 20 行超 × 2 組の重複は片側だけ修正される。実際 3 番目の相違（`graph_runtime_transfer->commit()` が 3646 行にだけある）は意図的な差か写し漏れか、コードからは判別できない。

**方向**: `RenderPipelineTransaction` に `publish()` / `rollback()` をメソッドとして持たせ、差分（graph transfer の commit の有無、失敗メッセージ文字列）だけをフィールドかフックで渡す。

---

### 6. `executePlannedFrameGraph` — 952 行 / 引数 16 個 / 入れ子 10 段 — HIGH

**`src/core/vkcore/renderer.cpp:1444-2395`**（および `executeRenderingPasses` `:2397-2588`）

`executePlannedFrameGraph` は 1 関数で 952 行、パラメータ **16 個**（うち 3 個はデフォルト引数、`nlohmann::json *node_trace` は nullable な out-param、`std::function` が 2 個）。brace 深さは 1942 行で **11**（関数本体を 1 として実質 10 段のネスト）。直後の `executeRenderingPasses` はパラメータ **17 個**で、うち 13 個をそのまま `executePlannedFrameGraph` へ転送するだけ。

**なぜ問題か**: 引数が 16 個あると呼び出し側で位置を 1 つ間違えても型が通る（`view_index` と `logical_view_count` はどちらも `std::uint32_t`、`per_view_sort` と `bool` フラグ群も同様）。10 段ネストは barrier / scope の分岐条件を頭で追えない。

**方向**: 転送用の引数群を 1 つの `FrameGraphExecutionRequest` 構造体（designated initializer で呼ぶ）にまとめ、`executeRenderingPasses` は依存構築だけ行って request を渡す形にする。深い barrier 検証部（1900-1975 付近）は `validateFusedScopeBarrier(...)` に切り出す。

---

### 7. `prepareViewFamilyDraws` — 349 行 / ネスト 7 段 — MEDIUM

**`src/core/renderer/polygoninstancecontainer.cpp:2382-2730`**

1 メソッドで 349 行。`locally_sorted` 集合の構築、canonical queue の容量検証、family ごとのループ、view ごとのループ、draw item ごとのループ、frustum + clip plane 判定、indirect command の書き戻しが全部 1 本に入り、brace 深さは 2682 行で 8（実質 7 段）。

**なぜ問題か**: カリング条件（`intersectsZeroToOneClipFrustum` と `view.clip_plane` の組み合わせ）が最深部にあり、ここだけを単体で検証する手段がない。

**方向**: 「family/view ごとの visible 判定」を `cullDrawItemsForView(view, items, out_commands)` に抽出する。呼び出し側は容量検証とバッファ書き込みだけになり、カリング条件が単体テスト可能になる。

---

### 8. DebugDraw / DebugText / Gizmo が同一実装の 3 コピー — MEDIUM

**`src/core/renderer/debugdraw.cpp:42-105, 229-248` / `debugtext.cpp:133-215, 409-428` / `gizmo.cpp:674-729, 730-760, 813-830`**

3 クラスが同じ private ヘルパー集合を持つ: `ensureDevice` / `ensureDescriptorPool` / `ensureVertexCapacity` / `ensureDescriptorSet` / `updateDescriptorSet` / `registerPass` / `checkpointRegistrations` / `rollbackRegistrations` / `retireRegistrations`。実装も一致で、`retireRegistrations` は debugdraw.cpp:229-248 と debugtext.cpp:409-428 が **20 行完全一致**（クラス名すら現れない）。`ensureVertexCapacity` + `ensureDescriptorSet` + `updateDescriptorSet` は debugdraw:55-104 と gizmo:683-728 が約 **50 行**、変数名の別名化を除いて同一。`registerPass` の `GraphicsPipelineDesc` 組み立て（blend factor 4 つ、`eLineList`）も 16 行が同一。

**なぜ問題か**: 3 倍のメンテナンスコスト。descriptor pool のサイズや blend 設定を直すとき 3 箇所すべてを見つける必要がある。

**方向**: `LineOverlayPassRegistry`（device / descriptor pool / vertex buffer / `std::map<PassId, PipelineRecord>` / registration checkpoint を持つ）を 1 つ作り、3 クラスがそれを composition で持つ。各クラスに残るのは vertex の作り方（line / glyph / gizmo handle）だけ。**低リスクで効果が見える改修の第二候補。**

---

### 9. `createLogicalDevice` — 248 行 / out-param 3 + bool フラグ 2 — MEDIUM

**`src/core/vkcore/core.cpp:562-809`**

戻り値は `vk::UniqueDevice` だが、実際には `bool &memory_budget_enabled`、`VulkanRuntimeCapabilities &runtime_capabilities`、`std::vector<std::string> &enabled_device_extensions` の **3 つの出力引数**で結果を返す。加えて `bool headless` と `bool use_openxr = false` の 2 つのフラグ引数が本体の分岐を切り替える。`#if PELICAN_WITH_OPENXR` + `if (use_openxr) throw OpenXr::VulkanBootstrapError(...)` という同型の 3 箇所（572-578、584-590、651-655）が、通常の `std::runtime_error` と対で書かれている。

**なぜ問題か**: 途中で throw したとき out-param が「半分だけ書かれた」状態になり得る。呼び出し側は 3 個の変数を先に宣言してから渡す必要があり、初期化漏れをコンパイラが検出しない。

**方向**: `struct LogicalDeviceCreation { vk::UniqueDevice device; bool memory_budget_enabled; VulkanRuntimeCapabilities capabilities; std::vector<std::string> enabled_extensions; };` を返す。OpenXR の throw は `throwBootstrapError(use_openxr, message)` 1 関数にまとめる。

---

### 10. `Renderer` のエラー規約が 3 系統、テスト API が本番クラスに常駐 — MEDIUM

**`src/core/vkcore/renderer.hpp:145-191`（private state）/ `:192-257`（public API）**

同じクラスの中で 3 つのエラー規約が同居する: (a) `renderLogicalFrame` / `currentFramePlanJson` は例外（`std::runtime_error` と、制御フロー用の `OutputRelowerRequired` / `OutputTemporarilyUnavailable`）、(b) `applyRenderPipelineAuthoringCandidate` は結果構造体 `RenderPipelineAuthoringApplyResult{.error}`、(c) `reloadRenderPipelineFromDisk(std::string &error, ..., std::uint64_t *staged_generation)` は `bool` + 文字列 out-param + ポインタ out-param の `noexcept`。さらに public に `*ForTesting` が **12 個**、メンバに `execution_tracing_for_testing` と `last_execution_trace`（nlohmann::json）が本番オブジェクトの状態として常駐。クラスは他に 25 個の可変フィールド（temporal history / picking 結果 6 個 / XR variant / hot reload state / ray scene generation）を持つ。

**なぜ問題か**: 呼び出し側は関数ごとに失敗の受け取り方を調べ直す必要があり、`bool` を無視した戻り値が静かに通る。

**方向**: `reloadRenderPipelineFromDisk` を `applyRenderPipelineAuthoringCandidate` と同じ `RenderPipelineAuthoringApplyResult` 戻り値に揃える（finding 5 の統合と同時に行えば追加コストはほぼゼロ）。picking の 6 フィールドは `struct LastPickingFrame` に束ねる。

---

### 11. 中規模の逐語コピー 2 件 — MEDIUM

**`src/core/renderer/materialrender.cpp:49-88` vs `:362-398`** — `renderMaterialDraws` と `renderShadowDepthDraws` が、indirect buffer の選択（`secondary_view_index` → `viewFamilyDrawOffset` → `viewFamilyIndirectBuffer` への切り替え）と draw call の取得を **約 37 行**同一形で持つ。差は `getViewFamilyDrawCalls` に `phase, material_filter` を渡すか `std::nullopt` かだけ。

**`src/core/renderer/camera.cpp:266-294` vs `:304-332`** — `prepareSceneCameras` の 2 オーバーロードが、`config` / `props` / `screen` 取得、`PreparedSceneState` の 10 フィールド初期化、`rebuild` ラムダ（orthoRH_ZO / perspectiveRH_ZO）、`rebuild()` 呼び出しまで **29 行完全一致**。

**なぜ問題か**: 投影行列の作り方や view-family バッファ選択規則を直すとき、2 箇所目が見落とされる。camera 側は特に危険で、片方だけ znear/zfar の扱いを変えると scene 有無で挙動が分かれる。

**方向**: materialrender は `struct ResolvedDrawSource { const BufferWrapper *buf; vk::DeviceSize offset_bias; std::span<const DrawCall> calls; }` を返す `resolveDrawSource(dependencies, phase, filter)` に抽出。camera は `makeDefaultPreparedSceneState()` を 1 本作り、2 オーバーロードは差分だけ書く。

---

### 12. `shared_ptr<const T>` への `const_cast` 書き込みと、説明のない再試行定数 — LOW

**`src/core/vkcore/renderer.cpp:4975-4978`**: `const_cast<RaySceneRowSnapshot &>(*ray_scene_rows).generation = generation;` — 直前に `std::make_shared<RaySceneRowSnapshot>(*ray_plan_publication->resolved_rows)` でコピーした直後の書き込みなので現状は安全だが、型は `shared_ptr<const ...>` として宣言されており「この時点ではまだ共有されていない」という不変条件がコードに表現されていない。今後この行より前に publish が入れば静かに共有オブジェクトを書き換える。→ 非 const の `shared_ptr<RaySceneRowSnapshot>` をローカルで作り、`generation` を入れてから const 版に代入する。

**`src/core/vkcore/swapchainframetarget.cpp:1706, 1836, 1843, 2158, 2579`**: `recovery->deferRetry(..., maintenance_tick, 1, ...)` と `..., maintenance_tick, 30)` の `1` と `30` が裸のリテラル。4 箇所の `30` は同じ意味（次の再試行までの tick 数）だが定数化されておらず、1 箇所だけ違う値に変えても誰も気づかない。→ `constexpr std::uint64_t kSwapchainRetryDelayTicks = 30;` / `kImmediateRetryDelayTicks = 1;` を導入。

なお `new`/`delete` は 2 箇所のみ（`renderpolicyregistry.cpp:487` の意図的リーク singleton と `swapchainrecovery.cpp:344` のテスト用フォールトスクリプト）で、Vulkan ハンドルは全面的に `vk::Unique*` / `BufferWrapper` / `DeletionQueue` 管理。**RAII の衛生は良好**。名前空間スコープの可変グローバルも 0 件。

---

## 全体所感

この 2 ディレクトリは「規律は高いが、規模の抑制だけが効いていない」状態です。TODO/FIXME が全体で **0 件**、可変グローバル 0 件、`const_cast` 1 件、`new` 2 件（どちらも意図的）、`deletionqueue.hpp` の retirement batch や `render_target_layout_tracker` / `swapchainrecovery` / `modelinstanceslots` / `viewfamily` / `projectionjitter` は素直に読める設計で、ここは手を入れる必要がありません。エラーメッセージも `pelican.ray_tracing.generation_rolled_back@1` のように ID 付きで整備されています。問題は一点に集中していて、**「1 フレームぶんの手順」と「1 コンテナぶんの状態」が、それぞれ 1 つの巨大なスコープに落ちている**こと。`renderLogicalFrame`(1229行) / `executePlannedFrameGraph`(952行) / `SwapchainFrameTarget::Impl`(2419行) / `PolygonInstanceContainer`(109メソッド) の 4 つで両ディレクトリの複雑さのほとんどを説明できます。

**最小リスクで最大の効果**は次の 2 つです。

1 つ目は **finding 2 の `ModelInstanceSlotTable` 抽出**。並列 vector 16 本を 1 個の値型にまとめるだけで、`PreparedSceneState` の 28 フィールド列挙が 5〜6 に減り、`snapshotPreparedScene` の 31 行・`publishPreparedScene` の 28 行・`prepareModelInstanceRebuild` の `next_*` コピー 50 行・`publishModelInstance` と `publishPreparedSceneModel` の 55 行重複が同時に消えます。純粋な機械的変換で挙動を変えず、しかも「フィールド追加時の同期漏れ」という現在最も高い潜在バグ源を型で塞げます。

2 つ目は **finding 5 の publish/rollback 統合**。すでに片方で `finalized` の代入順が食い違っている実害のある重複で、`RenderPipelineTransaction` にメソッドを移すだけの局所変更です。ここは finding 10（`reloadRenderPipelineFromDisk` の `bool` + out-param を結果構造体に揃える）と同じ場所を触るので、一度に片付けるのが得です。

`renderLogicalFrame` の分割は価値は最大ですが、フレーム記録の順序依存が暗黙なので単独では危険です。上の 2 つを先に済ませて `PolygonInstanceContainer` 側の状態遷移が型で表現されてから着手するのが安全な順番だと考えます。

---

## renderingpass

調査完了。`src/core/renderingpass/` (46 cpp / 30,623 行 + ヘッダ) を実際に読んで検証した結果を、重い順に 12 件。

---

### 1. 4 つの registry が同じ骨格を丸ごと写経している (約 1,200 行の冗長)
**HIGH** — `graphtransformregistry.cpp` / `subgraphreplacementregistry.cpp` / `renderstrategyregistry.cpp` / `passimplementationregistry.cpp`

正規化 (namespace 名・class 名・callback 名を置換、改行折り返しを無視) して突き合わせた結果:
- `graphtransformregistry.cpp:1307-1460` と `subgraphreplacementregistry.cpp:1326-1479` は **147 正規化行が完全一致**(差分ゼロ)。`renderstrategyregistry.cpp:855-1005` は 144 行中 131 行一致、`passimplementationregistry.cpp:678-830` は 147 行中 118 行一致(残りも折り返し差のみ)。内容は ctor / `registerProvider` / `unregisterProvider` / `snapshot` / `activateOwner` / `releaseOwner`。
- ABI ヘルパ群 `nextGeneration` / `validHandle` / `validByteRange` / `validProvider` / `abiSize` / `statusName` も 4 ファイルに同一実装(`graphtransformregistry.cpp:31-117`, `subgraphreplacementregistry.cpp:34-116`, `renderstrategyregistry.cpp:25-106`, `passimplementationregistry.cpp:25-88`)。約 70-90 行 × 4。
- `*RegistryState`(`ProviderSlot` / `mutex` / `providers` / `free_slots` / `retired_owner_generations` / `ownerIsRetired` / `retireOwner`)と `Snapshot::Impl` も同一(`graphtransformregistry.cpp:719-773`, `passimplementationregistry.cpp:254-297`, ほか 2 箇所)。
- provider 選択の `find_for_owner` ラムダ+フォールバック(約 30 行)も 4 箇所同一(`graphtransformregistry.cpp:1043-1077` ≡ `passimplementationregistry.cpp:442-472`)。
- ヘッダも同型: `*RegistrySnapshot` の 6 つの特殊メンバ宣言 + `operator bool`、`*Registry` の 5 メソッド、free function シングルトン、`*_internal::activateProviderOwner/releaseProviderOwner` が 4 ファイルで同じ順に並ぶ(`graphtransformregistry.hpp:63-125` ほか)。
- さらに **5 個目・6 個目**が存在する: `vulkannativescopeexecutor.cpp:96,552-580,724-800` と `src/core/renderer/renderpolicyregistry.cpp:22,291,342,368,458`。

**なぜ問題か**: ABI 検証の穴(`validProvider` の reserved チェック漏れなど)を直すとき 6 箇所を同じように直す必要があり、1 箇所忘れてもコンパイルは通る。owner retire のライフサイクル規則は再読込(hot reload)の安全性の中核なので、ここの分岐ズレは「特定の registry だけ古い provider が生き残る」形で出る。

**方向**: `registrationowner.hpp` の隣に `ProviderRegistry<Traits>` テンプレート(`ProviderSlot` の可変部は Traits の `resolve_fn` メンバポインタ 1 本)を置き、6 ファイルの lifecycle・ハンドル検証・statusName を移す。ABI 構造体の詰め替え(`abi_ports` 構築)だけ各ファイルに残す。段階移行なら「まず 4 つの `StableFingerprint` と ABI ヘルパだけ共通化」でもリスクほぼゼロで 400 行減る。

---

### 2. `registerRenderingPassConfigVariantsData` — 627 行・制御ネスト 7 段
**HIGH** — `renderingpassconfigregistration.cpp:676-1302`

1 関数で:トランザクション検証 → preview 分岐 → glTF/pose/deformed の model 検査 → audio → ray tracing project → GPU arena 確保 → variant ごとの program 登録 → attribute の staging buffer 作成と `executeOneTimeCmd` による GPU コピー → value graph 検証 → 世代間 buffer 引き継ぎ → publish。制御ネストは `for(1026) → for(1035) → if(1072) → if(1073) → for(1075) → for(1077) → if(1081)` で **7 段**。`bool include_preview = false` のデフォルト引数が 4 箇所(`744, 773, 958, 971`)で挙動を切り替える。

加えて `1079` で `const auto &dependencies = ...` が関数引数の `dependencies`(`std::vector<RenderingPassConfigRegistrationDependencies>`)を**シャドウ**している。同じループ内の `1041` / `1093` では外側の `dependencies[index]` を使っており、読み手が取り違えやすい。この区間だけ行幅が ~110 桁で、ファイル他所の ~60 桁と書式も不一致。

**なぜ問題か**: 登録は失敗時ロールバック(checkpoint/rollback)を伴うトランザクションなのに、成功経路と巻き戻し経路が 1 関数に混ざっている。新しい graph 資産(次の attribute 種)を足すたびにこの関数が伸びる。

**方向**: `prepare(検証+資産読み込み) → allocate(GPU arena) → registerVariants → publish` の 4 段に切り、`include_preview` はフラグでなく「preview を含む variant リスト」を呼び出し側で組み立てて渡す。シャドウしている `dependencies` は `scope_dependencies` に改名(これは 1 行で無害)。

---

### 3. `compileRenderingTargetPlans` — 495 行・引数 10 個
**HIGH** — `renderingsamplecount.cpp:1664-2158`

引数 10 個(うち `optional` 2 つ、`span` 3 つ)。本体は重複名チェック → planning constraint の索引化 → pin 索引化 → fragment 索引化 → graph ごとの plan 生成(`1806` からの 350 行ループ)→ sample/array_layer/format/representation の 4 種マージ → 4 種の assignment ベクタ生成。制御ネストは `for(1806) → for(1968) → if(2026) → if(2033)` で 4 段、designated-initializer の入れ子を含めると最大インデント 36 桁(`1886`)。

**なぜ問題か**: デバイス facts・policy・pin・physical fragment という 4 種類の入力源が 1 関数で交差しており、「どの入力がどの出力を決めたか」を追うのに全文読む必要がある。plan の fingerprint は再現性契約の一部なので、ここの副作用順序を崩す変更が静かに再現性を壊す。

**方向**: 引数を `RenderingTargetPlanRequest` 構造体 1 個にまとめ、`indexInputs()`(名前索引 4 つ)/ `planOneGraph()`(1806-1943 のループ本体)/ `mergeAssignments()` の 3 関数に分ける。`planOneGraph` はすでに 1 グラフ単位で閉じているので抽出リスクが低い。

---

### 4. `computetask.cpp` が 3,947 行の god file、`registerComputeTask` が 414 行
**HIGH** — `computetask.cpp` 全体、特に `:3006-3419` と `:466-808`

1 TU に:JSON パーサ(`parseFrameGraphBufferDefinitionsFromJson` `:1928-2140`, 213 行)、buffer コンテナ(`FrameGraphResourceContainer`)、compute task コンテナ、descriptor set 生成(`createDescriptorSet` `:2699-2924`, 226 行・引数 8 個)、ray tracing group layout 解決、dispatch 記録が同居。

- `registerComputeTask` (`:3006-3419`, **414 行**): 重複検出 → resource 解決 → indirect dispatch 検証 → ray group layout の virtual include → shader ロード → pipeline 生成 → dispatch group 導出 → frame×view の descriptor set 生成 → name_to_id 挿入とロールバック。ネスト `if(3149) → if(3160) → for(3197) → if(3203)` で 5 段。`:3171-3176` には **GLSL ソースが関数内に直書き**されている(no-op closest-hit shader)。
- `makeComputeResourceInterface` (`:466-808`, **343 行**): `for(476) → for(480) → if(680) → if(689) → if(691)` で 5 段、最大インデント 34 桁。

**なぜ問題か**: compute task の登録は ray tracing・reload・multiview の 3 つの機能が交差する場所で、ここを触る WP が高頻度に発生する。414 行 1 関数では「自分の変更がどの前提を壊したか」がレビューで見えない。

**方向**: まずファイルを `framegraphbuffercontainer.cpp` / `computetaskdefinitionjson.cpp` / `computetaskcontainer.cpp` の 3 つに割る(クラス境界がすでに存在するので機械的)。`registerComputeTask` からは `resolveRayTracingShaders()`(`:3149-3266`)と `createTaskDescriptorSets()`(`:3344-3369`)を抽出。埋め込み GLSL は `engine://` リソースに出す(隣の `engine://procedural_no_report` が既にその形)。

---

### 5. `registerRenderTarget` — 引数 18 個、しかも parameter object がすでに存在する
**HIGH** — `rendertargetcontainer.cpp:409-762` / `rendertargetcontainer.hpp:117-138`

引数 18 個(`name, base_extent, format_class, role, extent_scale, fixed_extent, format, usage, memUsage, history, history_clear_color, samples, mip_levels, array_layers, storage_mode, alias_group, alias_group_token, dimension`)、うち 9 個がデフォルト引数、bool フラグ 1 個。本体 354 行。

主要呼び出し側 `rendertargetconfigregistration.cpp:164-181` は **`RenderTargetDefinition` を 18 個の位置引数にバラして渡している**。つまり求める parameter object はすでに `rendertargetdefinition.hpp` に存在する。もう一方の呼び出し側 `src/core/vkcore/renderer_config.cpp:381-386` は 9 個だけ渡してあとはデフォルト。

**なぜ問題か**: 引数を 1 個足すと 9 箇所(テスト含む)の位置引数を数え直す。`bool history` と `std::uint32_t samples` の隣接など、型が近い引数が並んでおり取り違えがコンパイルで捕まらない。

**方向**: `registerRenderTarget(const RenderTargetDefinition &definition, vk::Extent2D base_extent, vma::MemoryUsage, std::optional<std::uint64_t> alias_group_token)` に置換し、`renderer_config.cpp` の XR mirror 呼び出しは `RenderTargetDefinition` をその場で組む。**このディレクトリで最も費用対効果が高い改修の 1 つ**(呼び出し側 3 + テスト数箇所のみ)。

---

### 6. `compilePassRenderingContract` — 452 行、color/depth の同型ブロックが 4 回
**MEDIUM-HIGH** — `renderingpassruntimecompiler.cpp:637-1088`

ネスト `if(730) → if(749) → for(755) → for(759) → if(766)` で 5 段。`749-827`(local_read 経路)と `828-917`(materialized 経路)は「色 attachment ループ → 深度 attachment の同じ検証」という構造を**それぞれ 2 回ずつ、計 4 回**書いている。`749-794` の color ブロックと `797-826` の depth ブロックは、`scopeColorAttachmentOperations` / `scopeDepthAttachmentOperations` と `map` / `optional` の違いを除けば同型の 30 行。

**なぜ問題か**: fused scope の load/store 契約は tile-local レンダリングの正しさそのもの。4 箇所のうち 1 箇所だけ条件を直す事故が起きやすい。

**方向**: `verifyScopeAttachmentChain(span<const PassDefinition*>, aspect, plan, metadata)` を 1 本作り、color は attachment index でループ、depth は単一要素 span として同じ関数に通す。

---

### 7. `materialPassRenderingBindings` — 227 行、最大インデント 62 桁、IIFE ラムダ 3 重
**MEDIUM** — `renderingpasscontainer.cpp:178-404`

本体はほぼ全部が `collect` ラムダ(`:185-392`, 208 行)。その中の `MaterialPassRenderingBinding` 集約初期化の `.shader_inputs` メンバが **177 行の即時実行ラムダ**(`:213-389`)で、その内側にさらに `input_attachment_index` ラムダが入る。最大インデント **62 桁**(`:335`)= 約 15 段。

**なぜ問題か**: 集約初期化の途中に 177 行のロジックがあるため、デバッガでのステップ実行もレビューでの差分も読めない。material pass の shader input 束縛はレンダリング結果に直結する箇所。

**方向**: `.shader_inputs` の IIFE を `static std::vector<MaterialPassShaderInputBinding> collectShaderInputs(const CompiledPass&)` として file-local 関数に出す。純粋な抽出で挙動は変わらない。

---

### 8. `FrameGraphNodeDefinition` を作る経路が 2 本あり、片方だけ壊れる
**MEDIUM** — `frameplanner.cpp:686-905` と `frameplanner.cpp:1239-1424`

`parseRenderNodeFromJson`(220 行)と `makeRenderNodeDefinition`(186 行)はどちらも `FrameGraphNodeDefinition` を組む。代入するフィールドを機械抽出して比べると **共通 17 / JSON 側のみ 4 (`after` `before` `snapshot_after` `name`) / PassDefinition 側のみ 1 (`resource_accesses`)**。どちらが正なのかコード上の根拠がない。

さらに「attachment を push_back → `load_op == eLoad` なら `appendUnique(node.reads)` + `appendReadFootprint(same_pixel)`」という **22 行のブロックが 4 回**登場(`:850-875`, `:878-903`, `:1307-1331`, `:1399-1423`)。

ついでに `frameplanner.cpp:1235-1237` の `passKind(const PassDefinition &)` は引数無名で常に `FramePlanNodeKind::render` を返す死んだ抽象。

**なぜ問題か**: `FrameGraphNodeDefinition` にフィールドを足すとき、2 本の経路のどちらかを忘れてもビルドは通り、「JSON から読んだときだけ効く/効かない」という再現困難な差になる。

**方向**: 共通部を `appendAttachment(node, resource, subresource, aspect, operations)` に括り出す(4→1)。その上で `makeRenderNodeDefinition` を `PassDefinition` → `FrameGraphNodeDefinition` の唯一の変換にし、JSON 経路は `PassDefinition` を経由させる(既に `passdefinitionjsonparser.cpp` が存在する)。

---

### 9. 共有ユーティリティがすでにあるのにローカル再実装が並ぶ
**MEDIUM** — 複数ファイル

- `StableFingerprint`(FNV-1a 64、`appendByte`/`appendUnsigned`/`appendString`/`value`)が `graphtransformregistry.cpp:184`, `subgraphreplacementregistry.cpp:182`, `renderstrategyregistry.cpp:228`, `passimplementationregistry.cpp:191` に 4 つ。**`src/project/stablefingerprint.hpp:18-44` の `StableFingerprint64` と本文が一字一句同じ**(整形のみ差)。4 ファイルとも `../../project/` 配下を既に include しているので使えるはず。
- `appendUnique` が 5 定義 / 4 ファイル(`computetask.cpp:359`, `frameplanner.cpp:38` と `:44`, `frameexecutionadapter.cpp:29`, `subgraphreplacementregistry.cpp:500`)。しかも `frameplanner` 版は `std::string value`(値渡し)、`computetask` 版は `const std::string&` とシグネチャが揃っていない。
- JSON の「必須文字列フィールド」ヘルパが 3 実装: `renderingpassjsonhelpers.cpp:273 parseStringField`、`computetask.cpp:48 requireString`、`frameplanner.cpp:274 requireString`。`parseStringList` / `parseOptionalStringList` も `computetask.cpp:329-357` と `frameplanner.cpp:282-310` に同内容(エラー文の接頭辞 `"Frame graph "` だけ違う)。

**なぜ問題か**: `renderingpassjsonhelpers.hpp` という共有ヘッダが存在しているのに使われていない = 共有の意図が壊れている。fingerprint は再現性契約の識別子なので、4 実装のどれかが将来ズレると「同じ入力なのに別 fingerprint」が起きうる。

**方向**: `StableFingerprint64` への差し替えは 4 ファイル各 35 行削除 + include 1 行、挙動不変を単体テストで確認できる。JSON ヘルパは `renderingpassjsonhelpers.hpp` に `requireString(json, field, context)` を 1 本足してローカル定義を消す。

---

### 10. `RenderTargetContainer` の image view API が組合せ爆発している
**MEDIUM** — `rendertargetcontainer.hpp:153-212`

`{image / attachment image} × {全体 / subresource / 単一 layer / layered} × {現フレーム / frame_index 明示}` の直積で **17 個のアクセサ**が並ぶ。全員が `bool history_read` を取り、subresource 系はさらに `bool array_view` を取る。

決定的なのは `getImageSubresourceView` / `getImageSubresourceViewForFrame` に **`bool array_view` 版と `ImageSubresourceViewDimension dimension` 版の両方**があること(`.hpp:167-187`)。実装(`rendertargetcontainer.cpp:1152-1181`)を読むと bool 版は単に `array_view ? two_d_array : two_d` に変換して enum 版に委譲しているだけで、enum の 3 つ目の値 `cube` を表現できない**劣化版シム**。移行が途中で止まっている。

**なぜ問題か**: 呼び出し側 39 箇所が 2 系統に分かれ、`cube` 対応を足すときにどちらの overload を使っているかで挙動が変わる。`bool array_view, bool history_read` が隣接しており引数取り違えがコンパイルで捕まらない。

**方向**: `bool array_view` 版 2 本を削除し、呼び出し側を `ImageSubresourceViewDimension` に統一(変換式が 1 行なので機械的)。次の段で `history_read` + `frame_index` を `RenderTargetFrameSelector` 1 個にまとめれば 17 → 6 程度に畳める。

---

### 11. DI 構造体があるのに同じクラスがグローバル module を直接掴む
**MEDIUM** — `computetask.hpp:88-107` vs `computetask.cpp` の `GET_MODULE` 21 箇所

`ComputeTaskRuntimeDependencies` は `render_target_container` と `frame_graph_resources` を明示的に注入する契約になっている。ところが同じ `ComputeTaskContainer` の実行経路は注入を無視してグローバルを引く:
- `dispatch()` — `computetask.cpp:3740` `GET_MODULE(RenderTargetContainer).historyFrameIndex()`、`:3760` `GET_MODULE(FrameGraphResourceContainer).buffer(...)`、`:3776`
- `bufferReadAfterWriteBarrier()` — `:3792`
- `rebindRenderTargets()` 周辺 — `:3453`

つまり `registerComputeTask` は注入されたコンテナで `FrameGraphBufferId` を解決し、`dispatch` はグローバルのコンテナで同じ id を引く。ディレクトリ全体で `GET_MODULE` は 59 箇所(`computetask.cpp` 21 / `rendertargetcontainer.cpp` 17)。

**なぜ問題か**: 現状は呼び出し側が常にグローバル module を渡しているので顕在化していない(`openxrmirrorsink.cpp:125` も `&GET_MODULE(...)`)。だが variant / reload の世代切り替えで別インスタンスを渡した瞬間、id は同じで中身が違うコンテナを引く。潜在バグであり、同時に「注入できるという見かけ」がテストを書く側を誤らせる。

**方向**: 注入するか globals を使うかどちらかに倒す。`dispatch` 系にもコンテナ参照を渡す(`FrameResources` にぶら下げるのが自然)か、逆に `ComputeTaskRuntimeDependencies` から 2 フィールドを消して「このクラスはグローバル module 前提」と明示する。後者のほうが今の実態に近く安全。

---

### 12. `renderingsamplecount` の名前と中身の乖離 + 4 本の並行ベクタ
**LOW-MEDIUM** — `renderingsamplecount.cpp` 全体 / `:2082-2155` / `:2259-2528`

ファイル名は「sample count」だが 2,548 行の実体は target plan コンパイラ全体で、主 export は `compileRenderingTargetPlans` / `applyRenderingTargetPlan`(`renderingsamplecount.hpp:161-193`)。

`RenderingSampleCountAssignment` / `RenderingTargetArrayLayerAssignment` / `RenderingTargetFormatAssignment` / `RenderingTargetRepresentationAssignment` は全て `{resource, 値 1 個}` の同型で、生成側 `:2082-2155` に **17 行ほぼ同一の reserve/loop/sort ブロックが 4 回**(差は member 名とデフォルト値だけ)、消費側 `applyRenderingTargetPlan`(270 行)は同じ 4 本を 4 つの `std::map` に詰め直して検証する。

なお本件はプロジェクト自身の `docs/design_reviews/2026-07-26_census_vocabulary.md:85,107` で既に指摘されており(「vector を4型に分ける理由を述べるコメントは読み取れない」)、未着手のまま残っている。

**方向**: `template <class T, class Member> void fillAssignments(...)` 1 本で 4 ブロックを畳む(74 行 → 約 20 行)。ファイル名は `rendertargetplancompiler.cpp` 相当にリネーム(include が 6 箇所程度なので低リスク)。

---

## 総評

**このディレクトリの本質的な問題は「複雑な設計が悪い」ことではなく、正しく設計された抽象が複製されている**ことだと思う。ABI provider registry、frame graph node、attachment operations、fingerprint、JSON ヘルパ — どれも設計としては筋が通っていて、レイヤ分離(logical graph → physical plan → Vulkan runtime)も docs の意図どおりに実装されている。問題は、新しい registry / 新しい node 経路を足すときに既存を **コピーして名前を置換する** 運用が定着していて、共通化の受け皿(`renderingpassjsonhelpers.hpp`, `src/project/stablefingerprint.hpp`, `RenderTargetDefinition`)がすでにあるのに使われていないこと。結果として「1 箇所直して 5 箇所忘れる」形の事故が構造的に仕込まれている。

**正直に言って良好な部分**も多い: エラー処理は一貫している(内部は `throw std::runtime_error` + 文脈文字列、ABI 境界のみ `Status` 返し、`noexcept` 境界で `std::terminate` — この 3 層の使い分けは崩れていない)。マジックナンバーはほぼ無く、定数は `constexpr` + `static_assert` で固められている(`computetask.cpp:33-46`)。`legacy_*` という名前が大量に出るが、これは死んだ互換分岐ではなく診断つきの意図的な lowering 経路(`logicalframegraphadapter.cpp:123-223`)。`frameexecutionadapter.cpp:576-669` の互換検証も現役で 3 箇所から呼ばれている。`vulkannativescopeexecutor.hpp` の型設計も整っている。dead code はほとんど見つからなかった。

**最小リスクで最大効果**の 2 つを挙げるなら:

1. **`registerRenderTarget` の parameter object 化**(所見 5)。受け皿の `RenderTargetDefinition` が既にあり、呼び出し側は本番 2 + テスト数箇所。18 引数 → 4 引数になり、以後 render target にフィールドを足す全 WP が軽くなる。半日仕事で回帰リスクはほぼゼロ。

2. **registry 骨格の共通化**(所見 1)を、まず **`StableFingerprint` と ABI ヘルパだけ**に絞って実施(所見 9 の一部と同じ作業)。`src/project/stablefingerprint.hpp` への差し替えは挙動不変を fingerprint の既存テストで即座に確認でき、それだけで約 400 行が消える。lifecycle 本体(147 行 × 4)のテンプレート化はその次の段に回すのが安全 — こちらは reload の owner 規則に触るので、所見で挙げた「削った分岐は誰もテストしない」問題(メモリの *集約改修の等価性*)がそのまま当てはまる。

逆に **今やるべきでない**のは所見 2 の `registerRenderingPassConfigVariantsData` の分割。627 行だが GPU 確保・トランザクション・ロールバックが絡み、分割の等価性を証明するコストが高い。先に周辺(所見 5・9)を片付けて、この関数が依存する語彙を減らしてから着手するほうが良い。

---

## communication / watch

監査対象: `src/core/communication/` + `src/core/watch/`

---

### 1. `editorjournal.cpp` 全体 — 1ファイルに7つの責務、匿名名前空間が4374行 / 118定義

**`src/core/communication/editorjournal.cpp:27-4400`（匿名名前空間）、`4688-5770`（`Impl`）**

ファイル6464行のうち **4374行（68%）が匿名名前空間で、そこに118個のトップレベル定義**（`awk 'NR>=28&&NR<=4399&&/^}/'` で計数）。中身は少なくとも7つの独立したクラスタに分かれる: JSON/文書ヘルパ(50-520)、コンポーネント・アドレス解決とスキーマ照会(519-1220)、prefabパラメータ/unpack(1222-1670)、`prepare*` 操作ファミリ(1689-2900、約1200行)、正規化/rebind/freeze/batch(2905-3660)、write-set/構造ドメイン重なり判定(3660-4110)、unpack整合性検証(4108-4400)。さらに `EditorEditCoordinator::Impl` は入れ子struct 6個 + **データメンバ27個** + メンバ関数23個で、セッション識別・編集チケット待ち行列・JOURNAL0/undo-redo・プレビューリース・ゲート epoch という**5つの独立した状態機械**を1つのstructに同居させている。

**なぜ問題か**: 匿名名前空間にあるため118個のヘルパは外部から単体テスト不能で、再利用もできない。27フィールドのImplはどのメソッドがどのクラスタを触るかがコードを全部読むまで分からず、プレビュー修正が undo スタックを壊すといった事故の温床になる。

**方向**: まず `prepare*` ファミリ(1689-2900)と unpack整合性検証(4108-4400)を `editorjournalops.cpp` / `editorjournalintegrity.cpp` に切り出す(どちらもImplに触らず、入力は `Json` + `AuthoringSceneDocument` だけなので機械的に分離可能)。次に `Impl` からプレビューリース関連8フィールドを `PreviewLeaseState` として抽出する。

**severity: high**

---

### 2. `commitPending` / `commitPendingPreview` — 531行の双子フレーム境界関数、成功後に結果を上書きする経路あり

**`src/core/communication/editorjournal.cpp:5234-5493`（260行）、`5499-5769`（271行）**

`commitPendingPreview` は260行・**最大ネスト7**（5314行目）。`request.kind` を `if (kind == open) {...continue;}` の連鎖で手動ディスパッチし、最後の abort は「どれでもなかった場合」のフォールスルーとして書かれている。`commitPending` は271行で、結果公開の5行定型

```
ticket.result = rejected(ticket.id, ticket.actor_id, ticket.base_revision, failure);
ticket.result["status"] = "failed";
results[ticket.id] = ticket.result;
completed.push_back(ticket.result);
```

が**同一関数内に6回**（`results[ticket.id] = ticket.result;` はファイル全体で9回）。末尾の `catch(EditFailure)` / `catch(std::exception)` / `catch(...)` は3ブロックとも本体7〜9行がほぼ同一。`kind==edit?"edit":kind==undo?"undo":"redo"` の三項連鎖も同関数内に3回。

**実バグ疑い**: `5730-5732` で「成功結果を `results`/`completed` に publish した直後」に `if (preview_lease) (void)forceAbort("base_revision_stale");` を呼ぶ。`forceAbort`(5198) は失敗時に `EditorPreviewAbortError`（`std::runtime_error` 派生）を**投げる**。これは同じ try の中なので `5739` の `catch(const std::exception&)` に捕まり、`results[ticket.id]` が failed で**上書きされ**、`completed` に2件目が push される。コミット済みの編集がクライアントには failed として届く。加えて reason 文字列 `"base_revision_stale"` はワイヤに出るが、実際の原因は「自分のコミットがプレビューのbase revisionを無効化した」ことで、状況と一致していない。

**方向**: `PreviewRequest::Kind` / `Ticket::Kind` ごとに `handleOpen/handleUpdate/handleCommit/handleAbort` に分割し、結果公開を `publishResult(ticket, json)` / `publishFailure(ticket, failure)` の2関数に集約する。`forceAbort` の呼び出しは成功publishの**前**に移すか、その場で `try/catch` で囲って成功結果を守る。

**severity: high**

---

### 3. `materializeEditorProjection` — 599行、最大ネスト9、同型引数3連続

**`src/core/communication/editorruntimefactory.cpp:1272-1870`**

このディレクトリで最悪の関数。599行、**最大ネスト深度9**（1618行付近: 関数 → try → for → if → std::visit ラムダ → `if constexpr` → for → find_if ラムダ → if）。引数8個で、うち

```
std::uint64_t expected_binding_epoch,
std::uint64_t expected_codec_generation,
std::uint64_t expected_behavior_generation,
```

が**同じ型で3連続**。呼び出し側(2165, 2213)は位置引数で渡しているので、2つ入れ替えても型エラーにならず、ジェネレーション不一致の拒否が静かに誤動作する。ヘッダ(`editorruntimefactory.hpp:64`)で公開されているが呼び出し元は同一ファイル内の2箇所だけ。

**方向**: 3つの `expected_*` を `struct EditorProjectionExpectation { binding_epoch; codec_generation; behavior_generation; }` にまとめる（これだけで呼び違いが型で止まる）。本体は `std::visit` + `if constexpr` の各分岐を `applyAddComponent(...)` 等の自由関数に切り出せば、外側は50行程度のループになる。

**severity: high**

---

### 4. RenderConfig系メソッドが4層を素通しで通過、ガード付き委譲が63個

**`src/core/communication/editorcommandservice.cpp:1510-1600`（29個）、`2488-2620`（34個）、`editorcommandservice.hpp`、`renderconfigeditor.hpp`**

`RenderConfigEditorService::X` → `EditorCommandService::X` → `EditorCommandRpcAdapter::X` → `configureEditorRpcHandlers` の `setHandler("x", ...)` という4層。中間2層は**純粋な転送で、本体は3〜4行**:

```cpp
OrderedJson EditorCommandService::getGraphInstances(const Json &params) const {
    if (!render_config_editor_) throw std::logic_error("render config editor service is unavailable");
    return render_config_editor_->getGraphInstances(params);
}
```

`"render config editor service is unavailable"` が **29回**、`"render config editing requires a mutable editor service"` / `"editor RPC adapter is read-only"` が **34回**（実測 `grep -c`）。結果として `EditorCommandService` は公開メソッド70個超のファサードになり、ヘッダ528行のうち大半が転送宣言。RPCメソッドを1つ足すには4ファイルを編集する。

**方向**: `EditorCommandRpcAdapter` は `RenderConfigEditorService*` を直接持ち、render-config系は `EditorCommandService` を経由させない（`editorrpchandlers.cpp` の登録表を `editor_rpc` と `render_config_rpc` の2つに分ける）。これで29+34の委譲のうち片側が丸ごと消える。

**severity: high**

---

### 5. 操作種別ディスパッチが文字列／variant で6箇所に手展開されている

**`editorjournal.cpp:2905-2936`（`prepareRpcOperation`）、`3389-3522`（`commandFromCanonical`）、`4103-4106`、`4557-4565`；`editorprojectionplan.cpp:645, 697`；`editorruntimefactory.cpp:1315, 1432, 1554`**

`EditorCanonicalOperation` は**12択の variant**（`editorprojectionplan.hpp:249-255`）。それに対して:

- `prepareRpcOperation` は `if (op == "set_component_value") ... if (op == "insert_component_field") ...` の**12分岐の文字列連鎖**
- `commandFromCanonical`(134行) は**同じopを別のグルーピング**で再度文字列分岐。しかも4分岐の中で
  ```cpp
  const auto object_id = AuthoringObjectId{exactUnsigned(operation.at("object_id"), "journal object_id", true)};
  const auto object = requireObjectLocation(document, object_id);
  ```
  が**4回コピペ**されている。
- さらに `isCanonicalUnpackOperation` / `operationsUsePrefabParameters` がop名部分集合を独自に列挙
- `editorprojectionplan.cpp` と `editorruntimefactory.cpp` に `std::visit` + `if constexpr (std::same_as<Operation, X>)` の連鎖が計5箇所

**なぜ問題か**: 操作を1つ追加すると6箇所を手で直す必要があり、`if constexpr` 連鎖はどれか1つを書き忘れても**コンパイルが通って黙って無視される**（`else` がない）。op名文字列は `renderconfigeditor.cpp` / `editorrpchandlers.cpp` / `src/devstudio/` にも散在しており（`edit_graph_document` は6ファイル）、共有された定数がない。

**方向**: op名を `inline constexpr std::string_view` の集合として1ヘッダに集約し、`prepareRpcOperation` を `{name, preparer}` の静的テーブルに変換する。`if constexpr` 連鎖には最低限 `else static_assert` / `else` 分岐を置いて、未処理の alternative を検出可能にする。

**severity: high**

---

### 6. `RenderConfigEditorService` — 責務5種のgodクラス、add/remove で50行の完全重複

**`src/core/communication/renderconfigeditor.hpp:99-250`、`renderconfigeditor.cpp:2342-2549` / `2555-2682`**

1クラスで **公開メソッド35個 + データメンバ16個**。担当は (a)フィーチャカタログ、(b)パッケージ install/uninstall、(c)ValueGraph の値/カタログ/ノード、(d)compute task / ray callable / acceleration structure、(e)authored pass / preset pass、(f)チケット待ち行列と2相コミットトランザクション — 明らかに5つ以上の関心事。

重複は実測済み（15行シングルによる完全一致検出）:

- `1297-1320` と `2383-2406`: **24行が空白正規化後に完全一致**（リゾルバ有無チェック → digest形式 → gate → base digest CAS → ディスク再読込CAS）。3箇所目が `2567` 付近に近似形で存在。
- `2521-2547` と `2654-2680`: **26行が完全一致**（preflight → チケットenqueue → accepted応答）。差分は `.operation` の文字列1つだけ。

つまり `addAuthoredPass`(208行) と `removeAuthoredPass`(128行) は**約50行が同一**。

**方向**: 前半を `std::optional<OrderedJson> checkAuthoringPreconditions(params, method, base_digest)`（拒否JSONを返すか nullopt）、後半を `OrderedJson enqueueAuthoredPassTicket(operation, root, candidate_documents, reference, gate, base_digest)` に抽出すれば両関数が半減する。クラス分割は、まず (b)パッケージと (c)グラフを別サービスに出すのが一番低リスク。

**severity: high**

---

### 7. `finalizeJournal` / `finalizeRevertJournal` — 20行完全一致、`deduplicate` ラムダを二重定義

**`src/core/communication/editorjournal.cpp:5021-5086` / `5089-5155`**

シングル走査で **5056-5075 と 5117-5136 の20行が完全一致**。実体は `EditorJournalCommandRecord` の組み立て + 6行の累積処理（`stable_targets` / `read_set` / `write_set` / `structural_domain` / `forward_postconditions` / `commands`）と、その直後の

```cpp
const auto deduplicate = [](std::vector<std::string> &values) { ... };
deduplicate(record.stable_targets); deduplicate(record.read_set); deduplicate(record.write_set);
```

**8行のラムダ定義+呼び出しが両関数に丸ごとコピー**されている。先頭のレコードヘッダ代入6行も差分1行のみ。

**なぜ問題か**: JOURNAL0 のレコード形式にフィールドを1つ足すたびに2箇所を同期させる必要があり、片方だけ直すと undo/redo のレコードだけ壊れる — しかも undo 経路は通常フローより試験されにくい。

**方向**: `appendCommandRecord(EditorJournalRecord&, EditorJournalCommandRecord)` と `finalizeRecordSets(EditorJournalRecord&)` を `Impl` のメンバに抽出し、両関数から呼ぶ。`deduplicate` は匿名名前空間の自由関数にする。

**severity: medium**

---

### 8. `makeEditorRuntimeService` 538行 + `execute` / `execute_preview` の40行重複（fault_injector の差異つき）

**`src/core/communication/editorruntimefactory.cpp:1872-2409`、うち `2128-2231`**

1つの工場関数が538行。末尾の `return std::make_unique<EditorCommandService>(EditorCommandServiceDependencies{...})`（2113-2408、約300行）はラムダフィールドの壁。そのうち `.execute`（2137-2181）と `.execute_preview`（2183-2230）を空白正規化して diff すると、**改行位置の違いを除けば意味的な差は4点だけ**（`EditorProjectionMode::Committed` vs `Preview`、`ProjectBasicConfigProjectionTarget` vs `EphemeralEditorProjectionTarget`、`restore_committed` の有無、そして `fault_injector` を渡すかどうか）。plan → 失敗チェック → materialize → `catch(EditorProjectionPlanError)` の**約40行が実質コピー**。

**なぜ問題か**: 4点目の差異（preview 側は `fault_injector` を渡さない）が意図的なのかコピペ漏れなのか、このコードからは判別できない。plan/execute のエラー変換を直すときに片方だけ直る。

**方向**: `EditorProjectionResult runProjection(runtime, mode, target, request, fault_injector)` を1つ作り、2つのラムダをその3行呼び出しにする。工場関数本体は `buildQueryDependencies()` / `buildEditDependencies()` / `buildRenderConfigDependencies()` に割る。

**severity: medium**

---

### 9. `ReloadService::applyRequests` — 180行4パス、汎用participant抽象に shader/material の名前が直書き

**`src/core/watch/reloadservice.cpp:843-1022`、`60-131`**

180行、最大ネスト5。**インデックス整列した4本の並行ベクタ**（`results` / `claimants` / `shared_claims` / `consumed`）を、同じ request 列に対する**4つの逐次パス**（claim解決 → companion batch → shader/material特別扱い → 通常batch → 個別apply）が読み書きする。不変条件（「`consumed[i]` が立ったら以後どのパスも触らない」）はどこにも書かれておらず、各パスの先頭で `if (!results[index] || consumed[index] || claimants[index] == nullptr) continue;` を手で繰り返している（3回）。

3番目のパス(958-985)は

```cpp
if (claimants[index]->name == shaderReloadParticipantName) { ... }
else if (materials != nullptr && claimants[index]->name == materialReloadParticipantName && ...) { ... }
```

と**特定participant名を文字列で直接分岐**しており、「participantは対等に登録される」という設計が破れている。同じ漏れは `ReloadService` のフィールド（`pending_shader_runtime_result_`, `shader_cache_hits_`, `material_participant_source_`, `model_participant_source_`）とメソッド（`applyShaderReloadBatch`, `forceShaderReload`）にも表れている。

関連: `ReloadParticipant`(`reloadservice.hpp:95-131`) は **`std::function` メンバ10個 + `optional<RuntimeReloadParticipant>`** で、`enqueue` / `apply_batch` / `apply_with_companions` / `apply_authored_candidate` という**4通りのapply経路**が相互排他的に共存する。その結果 `registerParticipant`(60-131) は**72行・9本の検証ルール**という実行時バリデータになっている。

潜在的な危険: `claimants` は `participants_`（`std::vector<ReloadParticipant>`）の要素への**生ポインタ**。`apply_batch` / `apply_with_companions` コールバック中に `registerParticipant` / `unregisterParticipant` が呼ばれると再確保でダングリングする（`renderer.cpp:3003,3031` が両方を呼んでいる）。現状はセットアップ時のみなので発火しないが、不変条件が文書化されていない。

**方向**: `ReloadParticipant` を4経路の `std::variant` に分けると `registerParticipant` の72行検証の大半が型で消える。`applyRequests` は `struct RequestSlot { const ReloadRequest* req; std::string claimant_name; bool done; bool ok; }` の1本のベクタにし、`claimants` はインデックスで持つ。shader/material の特別扱いは「batch owner + companion」機構で表現できるはずなので、専用participantとして登録しなおす。

**severity: medium**

---

### 10. 同一の失敗が到達するまでに最大4つの表現を通る

**ディレクトリ横断（`editorjournal.cpp` の `editFailure` 110箇所、`renderconfigeditor.cpp` の `rejection()` 126箇所、`editorcommandservice.cpp` の `EditorCommandError` 56箇所、`editorrpchandlers.cpp:14-42`）**

このレイヤに共存するエラーチャネルを実測すると **8種類**:
`throw EditFailure`(独自) / `return rejection(code,msg,details)` の JSON封筒 / `throw EditorCommandError` / `throw JsonRpcHandlerError` / 素の `std::invalid_argument`+`runtime_error`+`logic_error`（計**321箇所**）/ `EditorProjectionResult{.status=Rejected,.error=...}` の戻り値 / `std::optional<std::string>` の失敗詳細（`unpackJournalIntegrityFailure`, `closureMismatchDetail`）/ `bool` + log-and-continue（`reloadservice.cpp`、`return false` 42箇所）。

たとえば「不正なコンポーネント値」は `EditFailure` → `rejected()` のJSON → `EditorCommandError` → `JsonRpcHandlerError` と**4回詰め替えられて**ワイヤに出る。各境界は手書きの catch 連鎖で、`editorjournal.cpp:5732` の例（項目2）のように詰め替えの一段が例外を漏らすと直前の成功結果が壊れる。素の `std::runtime_error` は境界で `applicationError` に落ちるため、コード名が付かないまま Studio に届く。

**方向**: 全部を1つにするのは大手術なので、まず境界を1つ減らす — `EditFailure` と `EditorCommandError` はどちらも `{code, message, payload}` の同型なので統合できる。合わせて `throw std::logic_error(...)` の「サービス利用不可」ガード（項目4の63箇所）をコード付きの型に置き換える。

**severity: medium**

---

### 11. `RpcServer` が汎用トランスポートでなくなっている（ドメイン知識の混入 + 応答全走査）

**`src/core/communication/rpcserver.cpp:1090-1165`**

`RpcServer::handleLine` は汎用 JSON-RPC ディスパッチャのはずが2つのドメイン知識を持つ:

1. `1097`: `if (isGameLogicReloadInProgress())` — `src/core/gamelogic/gamelogicreload.cpp:843` のプロセスグローバル atomic を直接読み、全メソッドを拒否する。トランスポートが別モジュールのグローバル状態に依存している。
2. `1109-1129`: ordered ハンドラの戻り値に対し、`"resolution_error"` というキーが**どこかに入っているか**を再帰ラムダで**結果JSON全体を走査**し、その結果でシリアライズ経路を切り替える。キー順序保存という表現上の都合をドメイン文字列で決めており、しかも大きな `get_components` / `scene_tree` 応答のたびに全ノードを歩く。

**なぜ問題か**: `RpcServer` は `rpcserver_stub.cpp` を持つ差し替え可能な部品のはずだが、グローバルとマジック文字列でエンジン本体に縛り付けられている。`resolution_error` の綴りを片方だけ変えると、キー順序が静かに崩れる（テストで気づきにくい）。

**方向**: (1) は `RpcServer` に `std::function<std::optional<std::string>()> reject_reason` を注入する形に変え、グローバル参照は `configureEngineRpcHandlers` 側に置く。(2) は `setOrderedHandler` に「常に順序保存」フラグを持たせるか、ハンドラ側が順序保存要求を返り値の型で表明する。

**severity: medium**

---

### 12. `importSceneSnapshot` v1/v2 — ゲート20行と結果構築20行がほぼ同一

**`src/core/communication/editorcommandservice.cpp:1041-1138` / `1141-1257`**

空白正規化して diff した結果:

- `1062-1082`（v1）と `1144-1163`（v2）: **21行中、意味的差分は `schema_version != 1` / `!= 2` の1行のみ**。version → size(64MiB) → sha256 digest の3ゲートが丸ごと重複。
- `1111-1131`（v1）と `1227-1245`（v2）: `ImportSceneSnapshotResult` の構築（`std::holds_alternative<CommittedWithEffectFailure>` の三項が2回入る20行）が**改行位置以外は同一**。
- `authoringSceneContains` によるシーン存在チェックも両方に同文。

差分はv2にだけ prefab closure 照合が入る点だけなのに、共通部が2重に書かれている。

**方向**: `validateSnapshotEnvelope(request)`（version番号を引数に取る）と `makeImportResult(imported, current_scene_id)` の2つを抽出すれば、v1/v2 は差分のみになる。

**severity: medium**

---

## 総評

このレイヤは**規律は高いが構造が追いついていない**という状態です。良い点は正直に挙げておきます: `editorrpchandlers.cpp`（264行）は `invokeEditorRpc` に例外→JSON-RPC変換を集約した60件の均質な登録表で、このディレクトリで最もよく分解されています。`windowedrpchost.cpp`(141行) はロック範囲・detachの理由・stopping の順序がコメント付きで明快。`renderconfigtransaction.cpp` の Windows `FileRenameInfoEx` フォールバック判定も理由が書かれています。**TODO/FIXME はディレクトリ全体で0件、死んだコードも見つかりません**（`Legacy*` は全て現役の拒否経路）。コメントは概ね「なぜ」を書いており、項目2で指摘した `"base_revision_stale"` 以外に矛盾は見つかりませんでした。

問題は一貫して**「同型の処理を型でなく手作業で並べている」**ことに集約されます。12択の操作 variant が6箇所で手展開され、30個のRPCが2層で63個の手書き委譲になり、チケット/プレビュー/リロードの各ループが結果公開の定型を5〜9回コピーしている。個々の行は正しいのに、追加のたびに触る箇所が増え続ける構造です。

**費用対効果が最も高い2つ**:

1. **`renderconfigeditor.cpp` の CAS前置き/チケット後置きの抽出**（項目6）。`1297-1320` と `2383-2406` の24行、`2521-2547` と `2654-2680` の26行は**空白正規化後に完全一致**していることを機械的に確認済みなので、抽出が等価であることを目視で検証できます。既存のテストがそのまま効き、`addAuthoredPass` / `removeAuthoredPass` / `queueRootEdit` が半減します。リスクほぼゼロ。

2. **`materializeEditorProjection` の `expected_*` 3引数を1構造体に束ねる**（項目3）。呼び出し元は同一ファイル内の2箇所だけ、変更は純粋に機械的で、見返りは「位置引数の取り違えが型で止まる」という実バグ予防。599行の分割はその後で構いません。

そのあと着手する価値が高いのは項目2（`commitPending` の結果公開ヘルパ抽出と `5732` の `forceAbort` 位置修正）です。これは重複除去ついでに**実際の後退バグを1つ潰せる**唯一の項目です。逆に項目1（`editorjournal.cpp` の分割）と項目9（`ReloadParticipant` の variant 化）は価値は最大ですが範囲が広く、上の3つが片付いてからにすべきです。

なお `renderconfigeditor.cpp` は100桁超の行が188本あるのに `editorjournal.cpp` は3本、`reloadservice.cpp` の `334-480` / `843-1022` は40桁前後で機械的に折り返されているなど、フォーマットがファイル間・ファイル内で不統一です（severity: low）。clang-format の一括適用は差分が大きくなるので、上記リファクタと同じコミットに混ぜないでください。

---

## loader / userpublic / ecs / persistence

# コード品質監査: `src/core/loader/` / `src/core/userpublic/`（+ `ecs/`, `persistence/`）

以下、実際に読んで行番号を確認した所見を重大度順に12件。

---

### 1. `SceneLoader::load` が 672 行の神関数 — HIGH

`C:/src/pelican2\src\core\loader\scene.cpp:546-1217`

単一関数で 672 行。冒頭 `:570-599` だけで 30 個以上のローカル候補変数（`*_candidate` 12 個、`*_published` bool フラグ 5 個、トークン配列 4 個）を宣言し、それを 4 つの `try` ブロックと 52 行の `rollback` ラムダ（`:760-812`）が共有する。フェーズは prepare / early_publish / publish / activate / commit / retire / flush / finish の 8 段で、各段の間に `sceneLoadMutationEnabled(...)` によるフォールト注入分岐が 12 箇所（`scene.cpp` 内）散在し、本筋のロジックと交互に並ぶ。ネストは `:918-956` の `project_transform` 再帰ラムダ内で 6 段に達する。

**なぜ問題か**: publish 済みフラグと rollback ラムダの対応が目視でしか保証されておらず、新しい参加者（renderer/procedural/light/…）を1つ足すたびに「候補変数・published フラグ・rollback 内の逆操作・`static_assert(noexcept(...))` 行」の4箇所を同期させる必要がある。1つ落とせばロールバック漏れ（リーク or 二重公開）になり、コンパイラは何も言わない。

**方向**: 各参加者を `prepare/publish/rollback/finish` を持つ小さな型（`editorprojectionadapters.hpp` の `EditorProjectionAdapter` と同型）に切り出し、`load` は「参加者のベクタを順に prepare → publish → commit、失敗時は逆順 rollback」だけを行う 60 行程度のドライバにする。フォールト注入は参加者のデコレータに寄せる。

---

### 2. コンポーネント定義が「宣言的スキーマ」と「手書き decode/encode」の二重管理 — HIGH

`src\core\loader\componentcodec.cpp:111-214`（スキーマ）, `:383-1298`（手書き codec）

`transform_schema` / `camera_schema` / `light_schema` / `collider_schema` / `sprite_schema` ほかがフィールド名・既定値・range を宣言的に持つ（`:111-196`）が、それらは `materializeSchema`（`:209-214`）でエディタ向けの `SchemaNode`/`fieldSchema()` を作るためだけに使われる。実際の decode/encode は同じフィールド名と既定値を手書きで再掲する。宣言的な `decodeStructObject` を使っているのは animation の `parameters`（`:998`）と ray override（`:1170`）だけ。

最悪例が collider。同じ10フィールドが**8箇所**に書かれている:
`componentcodec.cpp:158-172`（schema＋既定値）, `:932-935`（`requireClosed` の13名リスト）, `:936-950`（`normalized` への既定値詰め）, `:958-975`（`encodeCollider`）, `userpublic\components\collider.hpp:15-24`（構造体既定値）, `collider.hpp:31-43`（バイナリ archive の `ref`）, `userpublic\components\collider.cpp:135-172`（`loadFromJsonArchive`）, `collider.cpp:175-201`（`validate`）。`radius` の既定値 0.5 に至っては `collider.hpp:19` / `collider_schema:164` / `componentCodecCanonicalDefaults().collider_radius` の3つの出所がある。

さらに同一ファイル内に JSON デコード機構が**4種類**同居する: 手書き `json.find` + `readVec3/readFloat`（transform/light/camera/sprite/ray）、`JsonArchiveLoader` + `result.ref(archive)`（collider, `:952-954`）、宣言的 `decodeStructObject`（animation の一部）、`SchemaNode::fromFlat` の手組み（`rayInstanceSchema`, `:233-249`）。

**なぜ問題か**: フィールドを1つ足す/既定値を変えるとき、どこを直せば足りるかが人間の記憶にしかない。エディタが見せるスキーマとランタイムが実際に受け付ける JSON が静かにずれる。

**方向**: `decodeStructObject`/`encodeStructObject` を全コンポーネントの既定経路にし、手書き decode はクロスフィールド検証（light の spot 限定フィールド、camera の nested projection）だけの後処理に縮める。`requireClosed` のフィールド名リストはスキーマから生成する。

---

### 3. `decodeStructPolicyField` / `encodeStructPolicyField` の optional/非 optional 二重実装と、encode 側の検証欠落 — HIGH

`src\core\userpublic\details\schema\structfieldjson.hpp:281-576`（decode, 296行）, `:579-703`（encode, 125行）

`if constexpr (is_optional_v<Member>)` の枝が、直後の非 optional 枝をほぼ逐語的に複製している。flags のデコード本体は `:303-337` と `:393-425` にあり、型名（`Value`/`Member`）と代入先の違いを正規化して diff を取ると**29行が完全一致**。vec2/vec3/vec4/quat の枝（`:371-392` と `:556-572`）、スカラー枝（`:353-370` と `:526-555`）も同様。`decodeStructPolicyField` 単体で 296 行、`if constexpr` のネストは 5 段。

encode 側ではその複製がすでに乖離している: 非 optional の vec は各成分に `validateStructFloating` を掛ける（`:684-699`）のに、optional の vec はそのまま配列を返す（`:619-626`）。**つまり `std::optional<vec3>` フィールドは encode 時に range 検証を受けない**。

**なぜ問題か**: 実害が出ている型の複製。今後スカラー/enum 側に修正を入れても片方に入れ忘れる。

**方向**: `Member` から `optional` を剥がした `Value` に対して働く 1 個のヘルパ（`decodeScalarValue<Value>(json, schema, policy, path)` / `encodeScalarValue<Value>(...)`）を抽出し、optional 枝は「存在判定 → ヘルパ → `member = std::move(v)`」の 4 行にする。

---

### 4. `resolveComponent` の中に camera 専用ロジックが 115 行埋まっている — HIGH

`src\core\loader\resolvedscene.cpp:186-409`（関数全体 224行）, camera 特例 `:274-388`

この関数は本来 codec vtable に委譲する汎用コンポーネント解決器だが、`if (result.name == "camera")` で始まる 115 行のブロックが camera の投影既定値補完（perspective/orthographic 別、yfov/znear/zfar/xmag/ymag/aspect の継承規則、cross-field 検証、sprite サブオブジェクトの既定値）を直接持つ。ネストは `:274-388` 内で 5〜6 段。関数の引数は 9 個（`:186-193`）、うち 1 個は out 参照（`behavior_sources`）。もう1つの特例 `if (result.name == "behavior")` が `:209-248` にある。

loader 全体でコンポーネント名の文字列リテラル比較が散在する: `"camera"` 39回, `"transform"` 30回, `"light"` 24回, `"simplemodelview"` 22回（`loader/*.cpp,hpp` 内の出現数）。`ComponentCodec` は `EditorProjectionPolicy` という dispatch 用 enum を持っているのに、呼び出し側は文字列比較を続けている。

**なぜ問題か**: コンポーネントを1つ足す＝loader の6ファイルを grep して回る作業になる。camera の既定値規則を変えるとき、`decodeCamera`（`componentcodec.cpp:654-781`）と `resolveComponent` の両方を整合させる必要があり、どちらが権威か読んでも分からない。

**方向**: `ComponentCodec` に `completeDefaults(value, const ResolvedSceneDefaults&) -> ResolutionResult<>` を1つ足し、camera の 115 行をそこへ移す。`resolveComponent` は `codec->completeDefaults(...)` を呼ぶだけにする。

---

### 5. `GameContext` がユーザ向け API の万能クラスになっている — HIGH

`src\core\userpublic\gamecontext.hpp:26-142`

1クラスに**65個**の public メンバ関数。担当領域は入力アクション、時刻、ログ、オブジェクト生成/削除、ローカル変換、スプライト、ライト4種、物理クエリ8種、カメラ、レイトレーシング、乱数、オーディオ10種、レンダーバッファ、アニメーショングラフ、シーン遷移、デバッグ描画、イベント emit、設定 JSON、セーブデータ——18 領域。

エラー報告規約が 5 通り混在する: `[[nodiscard]] bool`（`removeObject`, `setLocalTransform`, ライト4種, `setRenderValues`, `setGraphParameter`）、`std::optional<T>`（`spriteView`, `loadData`, `soundState`）、`Animation::Status` enum（`setAnimationGraphParameter`, `forceAnimationGraphState`）、例外（`loadScene`、コメントに「invalid values throw」`:100`）、無報告 `void`（`setCamera`, `saveData`, `setBusVolume`）。

同じことをする API が重複している:
- `raycastClosest(ray)` は `optional<phys::ObjectRaycastHit>`、`raycastClosest(ray, filter)` は `optional<phys::RaycastQueryHit>` を返す（`gamecontext.cpp:227` / `:237`）。**同名オーバーロードで戻り値型が違う。**
- `overlapAll(shape)` は `vector<std::string>`（名前！）、`overlapAllHits(shape, filter)` は `vector<phys::OverlapHit>`（`gamecontext.cpp:248` / `:285`）。
- `setGraphParameter`（`bool` を返し `SceneLoader` の graph runtime へ, `gamecontext.cpp:428`）と `setAnimationGraphParameter`（`Animation::Status` を返し `AnimationSystem` へ, `:341`）が別サブシステムに行く。ユーザにはどちらを使うべきか判断材料がない。

実装側 `gamecontext.cpp`（554行, `GET_MODULE` 52回）は大半が転送で、`#if PELICAN_WITH_*` / `#else return {}; #endif` が 22 箇所。無効ビルドでは**黙って空を返す**。

**なぜ問題か**: これは公開 API 面なので、後方互換の縛りが付いた状態で固まる。今のうちに分割しないと直せなくなる。

**方向**: `ctx.physics()`, `ctx.audio()`, `ctx.animation()`, `ctx.save()` のような小さなファサードに分け、`GameContext` はそれらを返すだけにする。重複ペア（`overlapAll` / `raycastClosest` の型違いオーバーロード、`setGraphParameter` の二系統）はどちらか一方を `[[deprecated]]` にして一本化する。

---

### 6. world 変換の合成が 3 実装あり、回転の正規化が食い違っている — HIGH

`src\core\loader\componentcodec.cpp:402-424`（`applyTransform`）, `src\core\loader\editorpreviewprojection.cpp:152-158`（`compose`）, `src\core\loader\scene.cpp:918-956` / `editorpreviewprojection.cpp:292-323`（親子 DFS）

- `applyTransform`（ランタイム）: `target.world->rotation = parent_rotation * to_glm(data.rotation);` ——正規化なし。
- `compose`（プレビュー投影）: `.rotation = glm::normalize(parent.rotation * local.rotation)` ——正規化あり。

pos と scale の式は同一。つまり**非正規化クォータニオンが書かれたシーンで、プレビューとランタイムが異なる world 回転を出す**。

さらに親子 DFS（3色マーキング＋循環検出）が2実装ある: `scene.cpp:926-956` は `std::vector<std::uint8_t>` に 0/1/2 のマジックナンバー、`editorpreviewprojection.cpp:292-323` は `enum class Visit { fresh, active, done }`。`editorprojectionadapters.cpp:275-340` にも親解決＋`applyRuntime` による3つ目の経路がある。

**なぜ問題か**: 「web で開ける ⊆ pelican で開ける」の部分集合原則（メモリ参照）は、両者が同じ数式を使っていることに依存している。数式が2本ある時点で保証が形骸化する。

**方向**: `composeWorldTransform(parent, local)` を1つの自由関数として `componentcodec` 側に置き、`applyTransform` と `compose` の両方をそれに委譲させる（正規化の有無はそこで1回決める）。DFS も `resolveHierarchy(objects, get_parent, compose)` として1本化する。

---

### 7. Projection adapter 3 クラスがほぼ逐語的なコピペ — MEDIUM

`src\core\loader\editorprojectionadapters.cpp:1204-1255`（Camera）, `:1257-1309`（Light）, `:1311-1428`（Collider）

`CameraProjectionAdapter` と `LightProjectionAdapter` は 52 行 / 53 行で、型名を正規化して diff を取ると**差分は 13 行のみ**（残り約 40 行が完全一致）。実質的に違うのは `Impl` のフィールド型、`kind()` の enum 値、`name()` のプレフィックス文字列、`publicationMode()` の値、`prepare()` 本体の 2〜3 行だけ。`rollback()` / `finish()` / デストラクタ / `name()` は3クラスとも同一。`ColliderProjectionAdapter` も同じ `old_state / next_state / published` 三点セットを持つ。

同型のボイラープレートは `:590-604`, `:830-841`, `:951-961`, `:1060-1071` にもあり、6クラス × `~/kind/name/publicationMode` = 24 個の定型関数。

**なぜ問題か**: 新しい参加者を足すたびに 50 行の写経が発生し、rollback の1行を写し忘れても型は通る。

**方向**: `template <class Module, class Prepared, EditorProjectionAdapterKind Kind, EditorProjectionPublicationMode Mode> class SnapshotProjectionAdapter` を作り、`prepare` だけを policy（ラムダ / CRTP フック）で差し替える。Camera/Light は 5 行の派生になる。

---

### 8. `EditorProjectionTransaction::commit` が 244 行、catch 節が 3 重複 — MEDIUM

`src\core\loader\editorprojectiontransaction.cpp:893-1136`

`noexcept` な 244 行関数で、コマンド適用・staging・resolve・アダプタの重複名検証・prepare ループ・InverseToken の早期 publish・publish ループ・publication・finish を1本で行う。3つの catch 節（`:1061`, `:1083`, `:1109`）はいずれも 7 行の同じ前処理（`if (document_published && candidate_state) { publishProjectionState; document_published = false; }` ＋逆順 rollback ループ）で始まり、最後の2つは `transaction_failed` の判定・パス構築・status 決定まで**約 20 行が同一**。「injected inverse-token publish failure」を作るブロックも `:1000-1016` と `:1023-1039` に2回（各 16 行）現れる。

加えて `publishProjectionState` は swap セマンティクス（`editorprojectiontransaction.hpp:115-119` にコメントあり）で、「publish」という名前で rollback も兼ねる。catch 節を読んだとき「失敗したのに candidate をもう一度 publish している」と誤読しやすい。

**方向**: 前処理を `const auto unwind = [&]() noexcept { ... };` に括り出し、catch 3 つを `catch (const ResolutionFailureException&)` と `catch (...)`（`std::exception` からのメッセージ取得は `errorFromException` に任せる）の 2 つに縮める。swap 用 API は `swapProjectionState` に改名する。

---

### 9. Render ABI ヘッダ 5〜6 本が同じ骨格を複製し、`Status` の数値だけずれている — MEDIUM

`src\core\userpublic\render\draw_sort_abi_v1.hpp`, `graph_transform_abi_v1.hpp`, `pass_implementation_abi_v1.hpp`, `render_strategy_abi_v1.hpp`, `subgraph_replacement_abi_v1.hpp`, `animation\abi_v1.hpp`

6ファイルが `struct ApiV1 {...}`（12行、フィールドまで完全同一）を個別に宣言し、6ファイルが `template <class T> constexpr T descriptor() noexcept` を同文で宣言し、5ファイルが `abiVersionV1 / descriptorVersionV1 / providerVersionV1 / maximumProviderNameBytesV1` を同じ値で再宣言する。末尾の `static_assert(std::is_standard_layout_v<...>)` 群も定型。

危険なのは `enum class Status`。7箇所（上記＋`physics\query_types.hpp:19`）が**同じ名前のメンバを異なる数値で**定義している:

| ファイル | `invalid_argument` |
|---|---|
| `render\draw_sort_abi_v1.hpp:19` | **2** |
| `animation\abi_v1.hpp:55` | **2** |
| `physics\query_types.hpp:19` | **2** |
| `render\graph_transform_abi_v1.hpp:32` | **1** |
| `render\pass_implementation_abi_v1.hpp:22` | **1** |
| `render\render_strategy_abi_v1.hpp:27` | **1** |
| `render\subgraph_replacement_abi_v1.hpp:28` | **1** |

`draw_sort` だけが `buffer_too_small = 1` を挿入したため、以降が全部1ずれている。

**なぜ問題か**: ABI 境界（DLL をまたぐ数値）なので、どこかで `static_cast<std::uint32_t>` を挟んで転送すると静かに別のエラーになる。ユーザのプラグインコードも「同じ名前だから同じ値」と仮定しがち。

**方向**: `render\abi_common_v1.hpp` に `ApiV1`, `ProviderHandleV1`, `descriptor<T>()`, 共通 `Status` を1回だけ定義し、各サブシステムはそれを `using` する。数値が違って良い理由がないなら統一し、`draw_sort` の `buffer_too_small` は末尾に移す。

---

### 10. `publish()` / `rollbackPublished()` が手書きの逆操作で、保証が assert しかない — MEDIUM

`src\core\ecs\archetypemigration.cpp:93-227`（`publish`, 135行）, `:229-350`（`rollbackPublished`, 122行）, `:455-630`（`prepare`, 176行）

`publish` は component array の raw な `relocate_one` / `destroy_one` / `count` 増減を chunk 境界をまたいで手で行い、`rollbackPublished` がその完全な逆を手で書き直している（ループの向き、`source_last_row` の取り方、adapter への `publish(inverse_context)` の逆順呼び出しまで対応付け）。両者とも `noexcept`。整合性を守っているのは 20 個の `assert()` だけで、リリースビルド（NDEBUG）では全部消える。

**なぜ問題か**: ここが壊れると症状は ECS チャンクの静かなメモリ破壊で、原因の特定が極めて高コスト。`publish` を1行変えたら `rollbackPublished` の対応する1行を必ず変える、という規律がコードのどこにも強制されていない。

**方向**: 大規模な書き換えはリスクが高いので、まず「publish が行った操作を記録する小さな逆操作ログ（`{array_index, from, to}` の vector）」を publish 中に積み、rollback はそれを逆順に再生するだけにする。少なくとも `assert` を `NDEBUG` でも生きるチェック（`PELICAN_CHECK`）に格上げして、この関数に限り契約を実行時に保つ。

---

### 11. loader のエラー表現が6系統、暗黙コンテキストが thread_local 7個 — MEDIUM

`src\core\loader\*.cpp`, `src\core\userpublic\details\*`

`src\core\loader\*.cpp` の throw 内訳: `std::runtime_error` 89, `std::invalid_argument` 54, `std::logic_error` 27 ——計 **170 個の型なし例外**。これに対して型付きドメイン例外が 8 種（`SceneSaveError` 18, `PrefabError` 16, `ResolutionFailureException` 6, `EditorProjectionException` 3, `EditorPreviewProjectionError` 3, ほか）、結果型が `SceneLoadResult`（variant）/ `EditorProjectionResult` / `SceneSaveResult` / `SceneImportResult`、さらに `ResolutionResult<T>`（expected 相当）と `std::optional` と `[[nodiscard]] bool` が並走する。`SceneLoader::load` は最終的に `catch (...)` で全部を `PreCommitFailure{phase, exceptionMessage(error), error}` に潰す（`scene.cpp:600-601` ほか）ため、`throw std::runtime_error("scene not found: " + id)`（`:627`）の型情報は文字列になる。

暗黙コンテキストの thread_local グローバルが7個あり、それぞれ専用の RAII スコープを持つ: `componentcodec.cpp:35`（`codec_canonical_defaults`）, `:36`（`codec_catalog_override`）, `effecttransaction.cpp:18`, `scene.cpp:201`, `userpublic\details\event\registerer.cpp:241`, `userpublic\details\reload\registrationowner.cpp:11`, `userpublic\deterministicrng.cpp:17`。`SceneLoader::load` は `scene.cpp:752-757` で4つを同時にインストールする。

**なぜ問題か**: 呼び出し側は「この関数は投げるのか、false を返すのか、optional を返すのか」をシグネチャから判断できない。ambient scope はゲーム DLL のコールバックが読むため、コールスタックに現れない依存関係になる。

**方向**: エラー系統は増やさずに、まず「境界を跨ぐ失敗は `ResolutionResult` か結果 variant、内部不変条件違反だけ `std::logic_error`」という1行の規約を決め、`std::runtime_error` を新規に増やさない。thread_local は新規追加を止め、少なくとも各スコープ型に「入れ子を検出して assert する」共通基底を与える。

---

### 12. 死んだ検証コードと公開ヘッダの衛生 — LOW

`src\core\loader\componentcodec.cpp:1391-1402` / `:1464-1481`, `src\core\userpublic\gameobjects.hpp:9`, `gamecontext.hpp:20`, `basicconfig.hpp:70-201`

- `legacyBranchDescriptorsAreStrictlyOrdered()` は consteval で「ID が一意かつ昇順」を証明し、`static_assert` で強制している（`:1391-1403`）。にもかかわらず `componentCodecLegacyBranches()`（`:1464-1481`）が同じ配列に対して O(n²) の重複検査を静的初期化子で実行し、重複時に `throw std::logic_error` する。57 要素に対して絶対に発火しない 18 行。
- `gameobjects.hpp:9` が公開ヘッダで `#include <iostream>` を引くが、`gameobjects.hpp` / `gameobjects.cpp` に `std::cout` / `std::cerr` / `std::ostream` の使用は1つもない（grep で0件）。ユーザのすべての翻訳単位に iostream の静的初期化を持ち込んでいる。
- `gamecontext.hpp:20` が `<nlohmann/json.hpp>` を公開ヘッダで引き、`GameContext::Json = nlohmann::json`（`:126`）として型を公開している。サードパーティ依存がユーザ API に漏れている。
- `ProjectBasicConfig`（`basicconfig.hpp:70-201`）は window/framerate/seed/camera + シーン文書の権威 + revision/generation カウンタ + 永続化ベースライン + prefab registry + asset/rendering/ui/input の JSON キャッシュ、を1モジュールで持つ。`friend` 3 個、`mutable` メンバ 6 個（const メソッドが遅延パースで書き換える）、そして `setSceneSaveFaultForTesting` / `setSceneSaveHookForTesting` / `setSceneImportFaultForTesting` がプロダクションの public API に露出している。ついでに `initailCameraProperty()`（`:147`）は `initial` の綴り誤り。
- `animgraph.cpp:679` の `static TransitionV1 forced_transition;` はメンバ関数ローカルの可変 static を全エバリュエータで共有し、そのアドレスを返す。現状 `duration == 0.0` のため呼び出し元（`:791`）が即座に消費して保存しないので実害はないが、forced transition に duration を持たせた瞬間にダングリングになる。

---

## 総評

**loader は「トランザクション（prepare / publish / rollback / finish）」という一貫した設計思想を持っているが、その思想をコードで表現する語彙が足りていない。** `editorprojectionadapters.hpp` の `EditorProjectionAdapter` インタフェースはまさにその語彙なのに、`SceneLoader::load` はそれを使わず 672 行のベタ書きで同じことをやり、adapter 側は adapter 側でテンプレート化されずに 50 行 × 6 のコピペになっている。つまり抽象は既に存在していて、適用範囲が狭いだけ。ここが最大の改善余地。

**userpublic は「宣言的スキーマ」という正しい機構を持ちながら、実際の (de)serialization がそれを迂回している。** `structfieldschema.hpp` の consteval スキーマ DSL は設計として筋が良い（fixed-capacity で DLL の静的初期化に耐える理由もコメントで説明されている）。しかしその成果物は「エディタに見せるメタデータ」にしか使われず、ランタイムの decode は手書きのままなので、二重管理と乖離が生まれている。`GameContext` の 65 メソッド神クラスは公開 API なので、固まる前に手を入れるべき唯一の場所。

**良い部分は正直に**: `src\core\persistence\`（422行）は関数が全部 70 行未満で、責務が明確、監査で挙げるものが無い。`resolutionresult.hpp` の `ResolutionResult<T>` は move-only にした理由まで含めて設計意図が書かれており、良質。`animgraph.cpp` の `Status` 一貫返却（`evaluate` 内で 16 回同一イディオム）はエラー処理としてはこのリポジトリで最も規律がある。`archetypemigration.cpp` は難しいが、難しさの大半が本質的なもの。

**費用対効果が最も高い2つの改修**:

1. **`componentcodec` の decode/encode を宣言的スキーマ経由に寄せる（所見2・4）。** 機構（`decodeStructObject`/`encodeStructObject`）は既に存在し、animation が実証済み。1コンポーネントずつ移行でき、各ステップで既存の codec テスト（legacy_branch_id のコーパス）がそのまま回帰検出になる。移行が済めば所見4の camera 特例も自然に codec 側へ移り、loader 全体の文字列比較 130 箇所も削れる。リスクは低く、効果は loader と userpublic の両方に及ぶ。

2. **`SnapshotProjectionAdapter` テンプレートの導入（所見7）。** Camera/Light/Collider の 3 クラス約 220 行を 60 行に圧縮でき、挙動は完全に同一（差分 13 行が全部パラメータ化可能であることを diff で確認済み）。純粋に機械的でテストの書き換えも不要。そしてこれが済めば、所見1の `SceneLoader::load` を同じアダプタ列に載せ替える下地になる。

逆に**今は触るべきでない**のは `archetypemigration.cpp`（所見10）。ここは「規則を1箇所に集めると削った分岐を誰もテストしない」（集約改修の等価性）が最も刺さる場所で、変異テストの体制が整うまでは assert の格上げ程度に留めるのが安全。

---

## shader / material / model / fullscreenpass

### 1. `materialcontainer.cpp` is a god class — 5159 lines, ~10 unrelated responsibilities

`src/core/material/materialcontainer.hpp:77-464`, `materialcontainer.cpp:29-5159`

One `DECLARE_MODULE` owns: texture upload/registration (`registerTexture*`, `createTextureResource`), sampler caching (6 sampler arrays + a `std::map<MaterialSamplerKey,…>`), 3 descriptor pools, material registration + GPU SSBO packing, named variants, pass-input/screen-input descriptor construction, hot-reload plumbing for two asset kinds (delegating to two `unique_ptr` handler friends), render-graph generation validation and ABI re-planning, per-draw `bindResource`, and 14 `*ForTesting` accessors. The internal `InternalMaterialInfo` struct alone is 132 lines (hpp:113-245) with ~45 members. The `.cpp` contains 147 `throw` sites.

*Why it matters:* every one of those concerns recompiles and gets re-reviewed on any change; the file is the third-largest in the engine and there is no seam to test a piece in isolation.

*Direction:* the natural split is already visible in the private-method list — carve out `MaterialTextureStore` (textures + samplers + `texturereloadhandler` friendship), `MaterialDescriptorBuilder` (`buildScreenInputDescriptor` / `ensureScreenInputDescriptor` / `rebindScreenInputs` / the pools), and `MaterialGenerationPlanner` (`validateRuntimeGenerationCompatibility` / `prepareRuntimeGenerationReload` / `materialBindingsForGeneration` / `resolveGenerationShaderInputs`, all of which are already free `static` functions at 3099-3402). The remaining container keeps registration and `bindResource`.

**Severity: high**

---

### 2. `MaterialContainer::prepareRuntimeGenerationReload` — 540-line function with a 392-line lambda body

`src/core/material/materialcontainer.cpp:3498-4037`

The body is one `materials.forEach` lambda (3579-3970) that does eight distinct jobs in sequence: pass-binding lookup, cross-variant schema/rendering agreement checks, screen-input physical-define diffing (3681-3734), resource-port physical-define diffing (3736-3860), pipeline-desc diffing, capability validation, metadata-update accumulation, and then a 62-line commit closure (3974-4035). It carries five parallel local vectors (`current_physical_defines`, `candidate_physical_defines`, `candidate_pass_inputs`, `candidate_resource_interface`, `image_resource_indices`) that must stay index-aligned with `declared_screen_inputs` / `declared_resource_ports`. Brace depth reaches 7.

*Why it matters:* this runs inside the renderer publication hook and decides whether a live render-graph swap is safe. A misalignment between `image_resource_indices[position]` and `physical_inputs.at(declared_screen_inputs.size() + position)` (3795-3800) is a silent wrong-descriptor bug, and nothing local makes that invariant checkable.

*Direction:* extract `planScreenInputAbi(material, physical_inputs)` and `planResourcePortAbi(material, physical_inputs, image_resource_indices)`, each returning a small struct `{defines_before, defines_after, updated_bindings}`; the outer lambda then reads as lookup → two plans → diff → accumulate.

**Severity: high**

---

### 3. ~155 lines of descriptor-set construction duplicated between material and fullscreen paths

`src/core/material/materialcontainer.cpp:4370-4528` ↔ `src/core/fullscreenpass/fullscreenpasscontainer.cpp:653-826`

Both run `for (variant) for (parity < 2)`, allocate one descriptor set per (variant, parity), then run the *same five-branch image-view ladder* over the same four resolver calls — explicit subresource, cube, `getLayeredImageViewForFrame`, `getImageViewForFrame`, `getImageLayerViewForFrame` — then push a `DescriptorImageInfo` whose sampler and layout are chosen by an input-attachment test, record into `bound_image_views[parity]`, and call `updateDescriptorSets`. 65 of 158 whitespace-normalized lines match byte-for-byte; the cube branch (`materialcontainer.cpp:4450-4466` vs `fullscreenpasscontainer.cpp:724-742`) is identical modulo variable names. The owning structs are duplicated too: `ScreenInputDescriptor::DescriptorVariant` (`materialcontainer.hpp:166-170`) is character-identical to `InputTextureInfo::DescriptorVariant` (`fullscreenpasscontainer.hpp:33-37`).

*Why it matters:* this is the single most bug-prone code in the audited scope (layer indexing, history parity, local-read layout). Any fix to the ladder — a new view dimension, a layout correction — must be applied twice, and the two copies have *already* drifted: the fullscreen copy has a `separate_view_layers` concept (687-696) the material copy lacks.

*Direction:* extract a shared `PassInputDescriptorSetBuilder` taking a `std::span<const ResolvedPassInput>` (target, history, subresource, view_dimension, descriptor_dimension, sampler-or-local-read) plus the resolver, and return the `DescriptorVariant` vector. Both call sites keep only their own validation and their own mapping into that neutral input type.

**Severity: high**

---

### 4. `buildRayTracingTableSnapshot` — 456-line dual-mode function keyed on one bool

`src/core/shader/raygrouplayout.cpp:1794-2249`

`profile_active` (1799) selects between two entirely different table layouts and is threaded through three lambdas plus two ~115-line branches (1969-2003 ordinary, 2004-2120 profile) that share almost nothing but their epilogue. The function then also does texture dedup (1927-1954), geometry-table build + digest (2140-2180), scene-row index resolution (2182-2231), and a final identity digest (2236-2247) — six phases, one scope. At 2182-2218 the `else` branch body is indented at the *same level as its `if`*, so the block boundary is invisible while reading.

*Why it matters:* the two modes produce different GPU row structs (`RayMaterialTableRow` vs `RayProfileMaterialTableRow`) written by `memcpy`, and the correctness argument for each is buried inside a function you cannot hold in your head.

*Direction:* split into `buildOrdinaryMaterialTable(...)` and `buildProfileMaterialTable(...)` returning `{keys, bytes, digests, leases}`, plus `buildGeometryTable(...)` and `resolveSceneRowIndices(...)`; the top function becomes ~40 lines of sequencing. Fix the 2182-2218 indentation first, as a separate no-op commit.

**Severity: high**

---

### 5. Latent digest bug: `std::string + "\0"` appends nothing

`src/core/shader/raygrouplayout.cpp:2155`

```cpp
const auto prefix = std::to_string(source.geometry_allocation_id) + "\0";
```

`"\0"` is a `const char[2]` whose first byte is NUL, so `operator+(const std::string&, const char*)` does `strlen` → 0 and appends **nothing**. The intended NUL separator between the allocation id and the hashed row payload is absent. The adjacent material path at 1991-1993 does it correctly with the *character* `'\0'` via `ostringstream`, which is what makes the intent unambiguous.

*Why it matters:* `geometry_row_digests` feeds `geometry_digest_sha256`, which feeds the scene identity digest used for generation matching (`pipelinefactory.cpp:1616-1622`). Without the separator, id `"12"`+rowA and id `"1"`+`"2"`-prefixed-rowB are hashable to the same input — a low-probability but real digest collision, and the code claims a guarantee it does not provide.

*Direction:* `std::string prefix = std::to_string(id); prefix.push_back('\0');` — matching the 2102-2107 style already used in the same function. Worth a grep across the repo for the same idiom.

**Severity: medium** (correctness bug, low exploit probability)

---

### 6. `FullscreenPassContainer::setInputResourcesById` — 13 parameters and 5 parallel arrays

`src/core/fullscreenpass/fullscreenpasscontainer.cpp:347-839` (declaration `fullscreenpasscontainer.hpp:112-128`)

493-line function, 13 parameters, 7 of them defaulted. Five of the parameters are parallel `std::vector`s (`input_rt_history`, `input_sampling`, `input_views`, `input_subresources`, `input_view_dimensions`) that must each be either empty or exactly `input_rts.size()` — validated by five near-identical hand-written checks at 381-406, each with its own "… metadata is inconsistent" message, then five near-identical "empty → fill with default" expansions at 434-476. A near-twin overload `setInputResources` (hpp:97-111) takes the same 13 arguments with `std::string` buffers.

*Why it matters:* with 7 defaulted trailing parameters, a caller that omits one silently gets defaults for a *different* concept than intended, and the parallel-array invariant is enforced only at runtime. Reordering or inserting a parameter cannot be caught by the compiler.

*Direction:* replace the five arrays with one `std::vector<FullscreenPassInput>` struct (target, history, sampling, view, subresource, view_dimension, local_read) — the size invariant then holds by construction and all five checks and five expansions disappear. Then split the function: validation, variant-count derivation (536-651), descriptor build (652-826, which should become the shared builder from finding 3).

**Severity: high**

---

### 7. `InternalGltfLoader` — a 1754-line struct with 27 methods and ~32 mutable members

`src/core/model/gltf.cpp:892-2645`

A single anonymous-namespace struct holds the whole importer: texture registration, material lowering, accessor decoding (`getDataFromAccessor`, 249 lines at 640-888 in its sibling struct), skin/palette selection, animation clips, VRM first-person annotation, VAT extras, morph targets, collision-mesh extraction, occurrence identity, and the final `load()`. `loadMesh` alone (2093-2434, **342 lines**) handles collision meshes, morph targets, VAT, VRM auto-split, skin joint remapping, and occurrence-identity construction, with four `#if PELICAN_WITH_VAT` blocks interleaved (2236-2249, 2353-2356, 2378-2394, 2416-2429) — one of which (2353-2356) opens an `else if` chain inside a conditional-compilation block, so the chain's shape differs between build configurations.

*Why it matters:* nothing here is unit-testable without a whole glTF model and live Vulkan modules; `next_generated_material`, `skin_joint_offsets`, `tmp_material_primitives` etc. are cross-method mutable state with no documented ordering contract.

*Direction:* the accessor layer is the cheapest and safest extraction — `GltfAccessorReader` (546-890) is already a separate struct and has no dependency on the loader's mutable state; move it to its own TU with tests. Next, lift `loadMesh`'s per-primitive body into a free function taking `(primitive, skin context, vat context) -> PrimitiveBuildResult`, which also lets the `#if` blocks collapse to one.

**Severity: high**

---

### 8. `ensureScreenInputDescriptor` — 484 lines, two near-parallel validation ladders

`src/core/material/materialcontainer.cpp:4532-5015`

Two loops — pass inputs (4564-4708) and resource ports (4710-5008) — each resolve a binding, compute `input_attachment_index`, derive `view_dimension` from `pass.input_targets` / `pass.input_target_views` with the *same 12-line lookup* (4633-4645 vs 4809-4822, identical modulo the error text), then run a ladder of 6-9 sequential `if (…) throw` view-policy checks and build a `ScreenInputResource`. The second loop's checks alone span 4917-4978: five consecutive throws whose conditions are a hand-expanded cross-product of `binding->port.view` × `view_dimension`.

*Why it matters:* the view-policy compatibility rules are a small relation (declared view × physical view → allowed?) written as ~60 lines of imperative negations. Adding one view kind means auditing every one of those conditions, and the two loops will drift.

*Direction:* factor the shared `view_dimension` lookup into one helper, then replace the 4917-4978 ladder with a table: `constexpr` array of `{ShaderResourcePortView, allowed PassInputViewDimension mask, message}` and one loop. The pass-input loop's 4646-4667 checks fold into the same table.

**Severity: medium**

---

### 9. Ray-table descriptor contract (set 3, bindings 0/1/2, 1024 textures) is spelled in four unlinked places

`src/core/shader/raygrouplayout.cpp:1155-1161` · `src/core/shader/pipelinefactory.cpp:187-198, 1567, 1593-1609`

The generated GLSL hard-codes `layout(set = 3, binding = 0/1/2)` and `sampler2D pelican_ray_material_textures[1024]` as raw text (raygrouplayout.cpp:1155-1159). The C++ side writes the same set as a bare `descriptorSetLayout(*layout_pipeline, 3)` (pipelinefactory.cpp:1567) and the same bindings as bare `0`, `2`, `1` in `WriteDescriptorSet` literals (1595, 1599, 1605). Only the array size has a symbol, `rayBindlessTextureCapacity = 1024` (`raygrouplayout.hpp:394`) — and the GLSL does not use it. Meanwhile `pelican_sets.hpp:10` defines `PELICAN_SET_FREE = 3`, which is what this set actually is but is never referenced here. Related: `pelican_sets.hpp:12-31` is a flat list in which `PELICAN_RAY_QUERY_TLAS_BINDING = 6` and `PELICAN_MATERIAL_BUFFER_BINDING = 6` collide, and `PELICAN_LIGHT_UBO_BINDING = 2` collides with `PELICAN_SKIN_PALETTE_BINDING = 2`, with nothing in the file stating which set each belongs to.

*Why it matters:* changing `rayBindlessTextureCapacity` leaves the GLSL at 1024 and surfaces as a validation-layer descriptor-count error at pipeline creation, not a compile error. The `pelican_sets.hpp` collisions are safe only because of an unwritten set assignment.

*Direction:* build the GLSL header text from the constants (`source << "layout(set = " << PELICAN_SET_FREE << ", binding = " << rayMaterialTableBinding << …`), name the three bindings, and group `pelican_sets.hpp` into per-set nested namespaces or a comment block so the implicit set of each binding is stated.

**Severity: medium**

---

### 10. `validateRuntimeGenerationCompatibility` and `prepareRuntimeGenerationReload` duplicate ~35 lines including identical error strings

`src/core/material/materialcontainer.cpp:3410-3446` ↔ `3582-3602` and `3931-3943`

Both open with the same `materialBindingsForGeneration` call, the same empty-bindings + previous-generation fallback, and the same 11-line `throw std::runtime_error("render-pipeline candidate has no compatible pass for live material " …)` — character-for-character (3421-3431 vs 3590-3601). Both then construct the same 13-line `MaterialPipelineRenderingContract live{…}` (3434-3446 vs 3931-3943), identical.

*Why it matters:* these are the two halves of a check-then-act pair for a live render-graph swap. If one copy's acceptance rule is loosened and the other is not, the validator passes work the planner rejects (or worse, vice versa) at publication time.

*Direction:* extract `resolveLiveMaterialBindings(generation, previous, material_id, material) -> std::optional<std::vector<MaterialPassRenderingBinding>>` (nullopt = "legitimately unused by this graph") and `liveRenderingContract(material)`. Both functions then share one acceptance rule by construction.

**Severity: medium**

---

### 11. `registerMaterialVariants` hand-copies 14 `MaterialInfo` fields; the pass-walk loop is duplicated verbatim

`src/core/material/materialcontainer.cpp:2281-2303`, `2344-2366`, `2235-2255` ↔ `2407-2426`

The variant path reads 14 base fields into 14 locals (2281-2303) and then assigns those same 14 fields into each registration (2344-2366) — 46 lines of pure field shuffling with no compile-time link to `MaterialInfo`'s member list. Add a field to `MaterialInfo` and variants silently do not inherit it. Separately, the "walk every compiled pass, preferring the published snapshot and falling back to registered ids" block appears twice: `registerMaterial` 2235-2255 and `registerMaterialVariants` 2407-2426, ~20 lines each, differing only in the callback name, with the same `"Published render pipeline pass table is inconsistent"` string in both. `materialBindingsForGeneration` at 3106-3114 repeats the null-`program` check a third time.

*Why it matters:* the field copy is a silent-omission trap on a struct that is actively growing; the pass-walk triplication means a future change to snapshot precedence has three edit sites.

*Direction:* replace 2281-2366 with an explicit `inheritFromBase(MaterialInfo &variant, const InternalMaterialInfo &base)` helper placed next to `InternalMaterialInfo` so the two lists are reviewed together (or better, a `MaterialAppearance` sub-struct copied wholesale). Extract `forEachCompiledPass(RenderingPassContainer&, F&&)` for the walk.

**Severity: medium**

---

### 12. `generateShaderResourcePortInclude`: 4× copied stage-guard epilogue, and a mirrored "inactive" writer that can silently drift

`src/core/shader/shaderresourceinterface.cpp:663-806` and `441-553`

The 9-line epilogue

```cpp
if (guarded) { if (singleSurfaceStage(binding.expected_stages)) { stream << "#else\n"; writeInactiveStageAccessors(stream, binding); } stream << "#endif\n"; }
```

is copy-pasted at 691-699, 716-724, 761-769 and 797-805 — once per descriptor kind, inside a 144-line `for` loop whose four branches are otherwise `if (…) { … continue; }` blocks. Separately, `writeInactiveStageAccessors` (441-553) is a hand-maintained mirror of `writeSampledAccessors` / `writeInputAttachmentAccessors` / `writeStorageAccessors` / `writeBufferAccessors`: for every accessor the active writers emit, the inactive writer must emit a matching zero-returning stub with the same signature.

*Why it matters:* the mirror is the fragile part. Add an overload to `writeSampledAccessors` and forget the twin, and a two-stage surface compiles in one stage and fails to link in the other — a GLSL compile error surfacing far from the edit.

*Direction:* restructure the loop body as `declareBinding(kind)` + `writeAccessors(kind, active)` where one function emits either the real body or the stub from the *same* signature list, so the two can no longer disagree. That also collapses the four copied epilogues to one.

**Severity: medium**

---

## Overall

`src/core/shader/` is in better shape than its line count suggests. Error handling is uniformly exception-based with structured `pelican.*@1` diagnostic codes; there is **no mutable file-scope state anywhere** in the four directories; every Vulkan object is held in `vk::Unique*` or a wrapper, with no raw-handle lifetime management; and there are **zero TODO/FIXME/HACK markers**. `shaderreflection.cpp` (RAII `ReflectedModule`, focused functions), `raytracingvalidation.cpp`, `shaderbindingtable.cpp` and `src/spvlink/main.cpp` (107 lines, clean argument loop, single `try`) are genuinely fine and need nothing. On your specific question about reflection duplication between `rtspirvinspection` and `shaderresourceinterface`: **there is none** — `shaderresourceinterface.cpp` is pure GLSL text generation and never touches SPIR-V. The real observation is that the engine carries three independent word-level SPIR-V readers (`rtspirvinspection.cpp`, `spvlink.cpp`, and a magic-number check in `shadercompiler.cpp:386`) alongside the spirv-reflect-based `shaderreflection.cpp`, but they answer disjoint questions (trace-call operands, linkage decorations, descriptor sets) and merging them would be a large, risky change for little gain. Leave them.

The damage is concentrated in `src/core/material/materialcontainer.cpp` and `src/core/model/gltf.cpp`, which between them hold ~8100 lines and every function over 300 lines in scope.

**Best value for least risk, in order:**

1. **Extract the shared descriptor-set builder (finding 3).** It removes ~155 duplicated lines from the two highest-bug-density spots, it is mechanical, and the two copies have already drifted, so doing it also fixes a latent inconsistency. Both call sites have `*ForTesting` accessors for bound image views (`materialcontainer.hpp:456`, `fullscreenpasscontainer.hpp:133`), so the refactor is directly verifiable by existing tests.

2. **Convert `setInputResourcesById`'s five parallel arrays into one input struct (finding 6).** It deletes ten hand-written validation/expansion blocks, eliminates a 13-parameter signature with 7 defaults, and is compiler-checked end to end — the build will not link until every call site is converted correctly. It also makes step 1 easier, since the neutral input type is exactly what the shared builder wants.

Finding 5 (the `+ "\0"` bug) is a one-line fix that should go in independently and immediately. Finding 10 is the next-cheapest structural win: two extracted helpers, ~35 duplicated lines gone, and it closes a real check-then-act divergence risk.

One caveat on all line counts above: the repo has **no `.clang-format`**, and formatting varies within single files — 47% of `materialcontainer.cpp`'s non-blank lines are ≤40 characters while the same file has lines up to 152. That inflates the reported function lengths by roughly 1.5-2× relative to a consistently-formatted file. It is worth fixing, but a repo-wide reformat would destroy `git blame` across 15k+ lines; I would add the config and let it apply only to touched regions rather than doing a sweep.

---

## animation / gamelogic / phys / os / openxr / imgui / appflow / playback / top-level

## Findings (ranked, 12)

---

### 1. `src/core/animation/animationservice.cpp:47-1795` — one 1,749-line god class

`struct AnimationServiceRuntime::Impl` is a single class body spanning 1,749 of the file's 1,952 lines: 11 nested record structs (`ObjectRecord`, `SinkRecord`, `OwnerRecord`, `ArenaRecord`, `PoseRecord`, `CursorRecord`, `VrmaClipRecord`, `SourceRecord`, `PhaseRecord`, `BlockedCommit`, `StagedFrame`, lines 48-127), 26 data members including 14 `unordered_map`s (128-155), and 55 member functions. It is simultaneously the asset registry, the handle/generation table for 7 handle kinds, the pose arena allocator, the phase scheduler, the source-claim arbiter, and the full C-ABI shim surface (`apiAdvance`…`getService`, 1032-1794).
**Why it matters:** every animation change touches this one file and one lock; there is no seam at which to unit-test pose-arena allocation without also standing up sinks, sources, phases and clips.
**Direction:** split along the map groups that already exist — a `PoseArena` (arenas/poses/stale_poses), a `SinkRegistry` (sinks/instances/sources/blocked_commits), a `PhaseScheduler` (phases + `runPhases`) — and keep the `static Status api*` block as a thin translation layer that forwards to them.
**Severity: high**

---

### 2. `src/core/gamelogic/behaviorarena.cpp:278-451` vs `503-734` — the transaction protocol implemented twice

`PreparedBehaviorAttachmentEdits::Impl` and `PreparedBehaviorSceneReconciliation::Impl` are two copies of the same prepare/publish/rollback/finish machinery. Verified byte-identical spans: `ParamsUpdate` 279-287 ≡ 504-512 (9 lines), `transferRuntime` 300-313 ≡ 527-540 (14), `swapParamsAndActivation` 315-326 ≡ 551-562 (12), `releasePreparedParams` 328-335 ≡ 564-571 (8), `destroyDetached`+`~Impl` 337-355 ≡ 573-591 (19) — **62 lines verbatim**, plus ~130 more structurally parallel lines in `publish`/`rollback`/`finish`. The copies have already diverged in a way that matters: `Edits::publish` (394-403) applies *every* params update in a separate pass, while `Reconciliation::publish` (622-624) applies only updates whose handle is in `reused`; and in rollback, `Edits` (412-438) does all `swapParamsAndActivation` first then all `transferRuntime`, while `Reconciliation` (708-711) interleaves them per handle — and both functions mutate the same activation fields.
**Why it matters:** this is the rollback path of scene loading. A fix to one copy's ordering will not reach the other, and the divergence is invisible without diffing 200 lines.
**Direction:** extract one `AttachmentTransaction<Policy>` holding `detached`/`reused`/`params_updates`/`published`/`finished` and the five helpers; let the two users supply only the extra state (handle counters, `activated`, `deferred_base`) and the publish/rollback hooks.
**Severity: high**

---

### 3. `src/core/animation/posegraphruntime.cpp:536-797` — 262-line evaluator, and it throws away its own diagnostics

`PoseGraphRuntime::Impl::evaluate` is 262 lines, nesting depth 6, built from 6 stack lambdas (`openArena`, `acquire`, `poseInput`, `scalar`, `sameLayout`, `requireLayout`, 545-592) followed by one `switch` whose five cases are 40-75 lines each. It mixes two error channels: `return status` (~10 sites) and `throw std::runtime_error` (572, 580) / `PoseGraph::raiseLayoutMismatch` (589). The thrown detail is then destroyed: `phase()` at 346-353 catches `(...)`, stores only `attachment.failed = true`, and line 875 later throws the information-free `"pose graph evaluation failed for " + object_name`. The comment at 349-350 says *"the owner thread re-raises it on the next synchronize"* — it does not re-raise it, it raises a different exception. `framephase.cpp:188-192` then replaces even that with `"animation phase evaluation failed"`.
**Why it matters:** an author who mis-wires a pose port gets three generic messages instead of `"pose port is unconnected: blend1.b"`. Debugging graph authoring becomes guesswork.
**Direction:** give each `case` its own `Status evaluateBlend(...)`-style function; store `std::exception_ptr` (not a bool) in `Attachment` at line 351 and `std::rethrow_exception` it at 875.
**Severity: high**

---

### 4. `src/core/appflow/loop.cpp:343-618` — the frame is spelled three times in one 276-line function

`Loop::run` is 276 lines at depth 6, containing 12 preprocessor directive lines (6 conditional regions: `_WIN32`, `PELICAN_WITH_OPENXR` ×3, `PELICAN_WITH_RPC` ×2), 3 stack lambdas, and 3 unrelated loops: headless-RPC (392-398), headless-frames (406-462), interactive-with-embedded-XR (514-614). The advance/update/render/record sequence is written out three separate times (410-425, 546-580, 591-610), each with its own `Clock::now()` pairs and `recordCpuFrame` call. `finishLoopResources(...)` — which takes a trailing unexplained `bool` — is called from 3 different exit points (395, 460, 616). The comment at 402-405 explicitly states the intent ("One headless frame, spelled once") that the other two paths violate.
**Why it matters:** reordering a frame phase, or adding a new one, requires finding and editing three places; the XR path already skips `framerate_adjuster.wait()` and the flat path already skips `selectGraphVariant` — differences that are accidental rather than declared.
**Direction:** extract `runHeadless()`, `runInteractive()`, `runXrFrame()` as three functions over a shared `struct FrameStep { advance(); update(); render(); record(); }`, so the three loops differ only in what they pass in.
**Severity: high**

---

### 5. `src/core/animation/animationservice.cpp:789-852` (+ `756-787`) — arbitrary callbacks run under the service lock, and every exception becomes "out of memory"

`runPhases` takes `std::scoped_lock lock{mutex}` at 791 and then, still holding it, invokes registered client callbacks at line 827. Those callbacks re-enter the same service (`posegraphruntime.cpp` calls `begin_pose_frame`/`acquire_pose`/`blend_normal`, each of which does `std::scoped_lock lock{runtime->mutex}` at 1174, 1206, 1455 …, 40 lock sites in the file). That re-entrancy is the only reason `mutex` is a `std::recursive_mutex` (line 128) — the recursion is load-bearing but undocumented, so any future callback that takes a second lock, or that is invoked from a worker thread, deadlocks. Separately, the `catch (...)` at 846 and 870 maps *every* escaping exception to `Status::out_of_memory`, which is simply wrong for most of them. And `releaseOwner` (756-787) tombstones `phases`/`sources`/`cursors`/`owners` (`active = false`) but never erases them — grep confirms the only erases are the wholesale `clear()` in `reset()` (731-750) — so each game-DLL hot reload (`gamelogicreload.cpp:56`) permanently grows four maps.
**Why it matters:** an undeclared re-entrancy contract on a lock is the hardest class of bug to find later; the status mislabelling makes failures unattributable; the tombstone growth degrades the developer inner loop over a long session.
**Direction:** copy the sorted `ordered` phase list, release the lock, then invoke callbacks outside it (records are already tombstoned rather than erased, so the pointers stay valid); map exception types to statuses explicitly; add a compaction pass over the four maps in `releaseOwner`.
**Severity: high**

---

### 6. `src/core/openxr/openxrcompositiontarget.cpp:471-1302` — an 832-line `Impl` whose frame protocol is 12 loose booleans

`XrCompositionTarget::Impl` is 832 lines. Its frame state machine is encoded as independent flags: `frame_prepared`, `logical_frame_begun`, `logical_frame_submitted`, `view_family_begun`, `view_family_ended`, `view_family_submission_complete`, `depth_submission_active`, `teardown_required` (506-513), plus `view_begun`/`view_ended`/`submission_complete` per view (480-482) and `release_attempted` per swapchain (476) — 12 booleans for a protocol with perhaps six legal states. `resetFrameFlags()` (725-738) has to remember to clear seven of them, and `abortFrame()` (768-829) reads six to decide what to unwind. The same file also holds the 362-line `VulkanXrCompositionGraphics` (99-460).
**Why it matters:** nothing prevents an illegal combination; a missing line in `resetFrameFlags` produces a hang or a validation error one frame later, far from the cause. Every new step in the frame protocol adds another flag and another place to clear it.
**Direction:** replace the flags with one `enum class FrameStage { idle, prepared, begun, views_recorded, submitted }` plus a single transition function that rejects illegal moves; move `VulkanXrCompositionGraphics` to its own translation unit.
**Severity: medium**

---

### 7. `src/core/os/actionmap.cpp:180-262` ≡ `src/core/communication/rpcserver.cpp:778-861` — 82-line verbatim copy of the control-name parser

`keyboardControlToKey` and `injectedKeyboardKey` are the same function, copied. A normalized diff of the two 82-line bodies produced **zero real differences** (only the parameter name `control` vs `key`). Both contain the same alias tables (`"esc"`, `"left_ctrl"`, `"up"`…), the same `f1`–`f12` digit loop, the same `num0`–`num9` special case, and the same reliance on `KeyCode::A`/`Num0`/`F1` being contiguous enum ranges.
**Why it matters:** the RPC input-injection path and the binding-profile path must agree on key names or replays diverge from live input — and they can only agree by accident. Adding one key name silently fixes half the engine.
**Direction:** move the parser into `actionmap.hpp` as `std::optional<KeyCode> keyCodeFromControlName(std::string_view)` and have `rpcserver.cpp` call it (keeping only its `lowerAscii` pre-pass). A static sorted `string_view → KeyCode` table would also drop the 40-branch if-chain.
**Severity: medium**

---

### 8. `src/core/openxr/openxrmirrorsink.cpp:182-360` — 179-line function with hand-rolled unwinding, duplicated in both catch blocks

`tryPresent()` is 179 lines at depth 6. It tracks cleanup with four manual variables (`frame_token`, `rendering_begun`, `timing_begun`, `submission_lease`, 190-193) and has 8 exit points, each of which must remember `reportProgress()`. The two catch handlers at 329-343 and 344-358 contain the **same 14-line cleanup sequence twice**, differing only in the log message. Lines 259-261 construct a `ScopedCommandDebugLabel barrier_label` around an empty block — a no-op left behind from a removed barrier pass. The function also performs 11 separate `GET_MODULE(...)` lookups (189, 198, 229, 239, 243, 249, 291, 294, 299, 311-313).
**Why it matters:** hand-rolled unwinding with four flags is exactly the shape that leaks a swapchain image after the fifth edit; the duplicated handler will be fixed in one copy only.
**Direction:** wrap the frame token, the rendering scope and the GPU timing range in small RAII guards whose destructors do the abandon/end/cancel, then collapse both catches to one `catch (...)` that only logs and sets `disabled`. Delete 259-261.
**Severity: medium**

---

### 9. `src/core/gamelogic/gamelogicreload.cpp:56-65, 492-499, 560-567, 622-636` — the 7-subsystem provider list written out four times

The same fan-out over `render_policy_internal`, `render_pass_internal`, `render_subgraph_internal`, `render_graph_transform_internal`, `render_strategy_internal`, `vulkan_native_scope_internal` and (behind `#if PELICAN_WITH_PHYSICS`) `physics_internal` appears four times — once for release, three times for activation — each with its own `#if` guard, ~28 hand-maintained lines. The rollback copy at 622-636 is already structurally different: every call is individually wrapped in `if (activate_rollback_owner)` and the physics call sits in a second `#if` block.
**Why it matters:** adding an eighth provider registry means finding four lists; missing the rollback list produces a failure that only shows up when a reload *fails*, which is the least-tested path.
**Direction:** have each registry publish an `{activate, release}` pair into one host-side table (the `registrationownerhost.hpp` layer is the natural home), and replace all four sites with `forEachProviderRegistry(owner, &Registry::activate)`.
**Severity: medium**

---

### 10. `src/core/phys/physicsruntime.cpp:585-1047` — six query entry points sharing one uncaptured skeleton, four index-aligned parallel spans

`raycastMeshes` (585), `overlapMeshes` (655), `overlapMeshMeshes` (711), `raycastAll` (746), `overlapAll` (833), `shapeCastMeshes` (893) and `shapeCastAll` (971) all repeat: clear out-param → `try` → size checks → select → build query → `shared_lock` + `providerForCapabilityLocked` + `invokeProvider` → validate raw hits with a `std::vector<bool> seen` dedup → order → catch. The **identical 9-line catch triple** (`invalid_argument` / `bad_alloc` / `...` → clear + status) appears **7 times** (643, 699, 737, 821, 881, 959, 1036). The mesh variants thread **four index-aligned spans** (`meshes`, `identities`, `metadata`, `acceleration`) through 7-8 parameter signatures, and the "all sizes must match" invariant is re-checked in two places (`selectMeshes` 320-321 and again at `raycastMeshes` 595-596).
**Why it matters:** an 8-parameter signature with four spans that must be the same length is a caller-side bug waiting to happen; the 7 catch copies mean an error-mapping change lands in six of seven places.
**Direction:** introduce `struct MeshQuerySet { span meshes, identities, metadata, acceleration; }` with one validating factory, and a `template <class Query, class Hit> Status runProviderQuery(capability, query, validate, order, out)` that owns the lock/dispatch/validate/catch skeleton.
**Severity: medium**

---

### 11. `src/core/animation/vrmapplication.cpp:471-534` vs `536-616` — `setInputs` / `setTypedInputs` duplicated

The two ABI entry points share ~46 lines: the argument-validation predicate including the identical flag-mask expression (476-484 vs 541-550), the lock/model/stale-revision preamble (485-493 vs 551-559), the `InputState next` construction (495-501 vs 561-567), and the **16-line weight-validation loop (502-517 vs 571-586), which differs only in line wrapping**. Only the three asset fields (568-570) and the tail (588-612) are genuinely new.
**Why it matters:** the weight loop is the security-relevant part — it bounds-checks `weight.name_size`, `element_size` and `version` from a foreign DLL. Two copies means a hardening fix can miss one.
**Direction:** extract `Status readWeights(const ExpressionWeightV1 *, uint32_t, const SemanticView &, InputState &)` and a `Status validateCommonInput(const CommonFields &)`; let `setInputs` delegate to `setTypedInputs` with the asset fields zeroed.
**Severity: medium**

---

### 12. `src/core/playback/` — 19-line verbatim duplication, and "stub" files that copy non-stub code

`SeqPlayer::applyCameraOverride` (`seqplayer.cpp:308-326`) and `VatPlayer::applyCameraOverride` (`vatplayer.cpp:29-47`) are byte-identical over 19 lines (verified by diff modulo the class name). Worse, the build-variant files copy real logic: `vatplayer_stub.cpp:22-40` reproduces `vatplayer.cpp:60-78` exactly, and `seqplayer_stub.cpp:44-62` reproduces `seqplayer.cpp:362-380` — prepared-scene snapshot/publish boilerplate that has nothing to do with the disabled feature. `applyCameraOverride` also does not belong in a playback module at all: it reads `--camera` out of `EngineLaunchConfig` and pokes `Camera`, and runs twice if both `--play-seq` and `--play-vat` are given.
**Why it matters:** a `*_stub.cpp` that duplicates real code will silently drift from it, and the stub build is the one nobody runs locally.
**Direction:** move the camera override to a free `applyLaunchCameraOverride()` called once from startup; make the prepared-scene boilerplate a small CRTP/base helper so the stub files contain only the `throwBuildFeatureDisabled` constructor.
**Severity: low→medium**

---

**Minor, verified, not worth their own slot:** `profiler.cpp:14-46` is called every frame from `coretemplate.cpp:666-751` but its only success-path log is commented out (line 26) — when `logger` is null it instead writes `std::cout << … << std::endl` per zone per frame, and stdout is the JSON-RPC protocol channel (`loop.cpp:394`). `meshphysicsprovider.cpp:130-204` holds three ~24-line lambdas whose 10-line `ProviderShapeCastHitV2` translation (142-150 / 168-176 / 194-202) is verbatim ×3, while `toAbi` helpers already exist next door in `physicsruntime.cpp:164-221`. `openxrsession.cpp:459-484` and `501-528` are the same `endFrame` twice, differing only in `layerCount` 0 vs 1. `src/core/light/` (3 files) is the only tab-indented, `m_PascalCase`, brace-on-next-line island in all of `src/core`, and `LightContainer` exposes a five-deep `update()` overload ladder (hpp:52-68) over one private `updateImpl`. There are 219 `*ForTesting` entry points in production headers under `src/core`.

---

## Overall

The picture is uneven rather than uniformly bad. `src/core/ui/` is genuinely in good shape — 11 small files, no globals, one 109-line function, two `GET_MODULE` calls in total — and `src/core/audio/`, `src/core/asset/`, `src/core/os/actionmap.cpp` (its own internals, not the copy), `src/core/phys/physquery*.cpp` and `src/core/startup.cpp` are fine: decomposed into small named functions with consistent error handling. `src/player/main.cpp` is a readable startup sequence (`parseLaunchConfig` is 288 lines but it is a flat argparse table, not a tangle) and `main()` itself at 593-645 reads cleanly. The pain is concentrated in four places: the animation service, the behavior arena, the OpenXR composition/mirror pair, and `Loop::run`. The common shape is not spaghetti but *twinning* — the same protocol written twice (transactions, key parsers, input setters, endFrame, stub files) and long functions that inline a whole protocol instead of naming its steps.

The two refactors with the best value-to-risk ratio are **#7 (the 82-line key-name parser)** and **#2 (the twin behavior transactions)**. #7 is close to zero risk — move one function to a header, delete the copy, and a real correctness hazard (RPC-injected input diverging from live bindings) disappears. #2 is more work but is the highest-leverage: it is 62 lines of provably identical code guarding scene-load rollback, the divergence between the two copies is already real, and consolidating it is a pure structural move with the existing tests as the check. I would leave #1 (the animation god class) for last despite its rank — it is the largest maintenance burden but also the change most likely to break something while it is being made.

---

