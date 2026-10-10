# 第9章 マクロ・型消去・変更時の注意

最終確認: 2026-10-10

[索引へ戻る](README.md) / [前章](08_class_interface_index.md)

ここでいう「黒魔術」は、コードが不可解という意味ではありません。macro、template、型消去、static 初期化、GPU 非同期寿命など、実際の処理が呼び出し箇所から離れて見える実装を指します。この章は、変更時のチェックリストと本体の説明への索引です。

**この章の地図**

```mermaid
flowchart LR
    A["登録・型・寿命<br/>§9.1–9.8"] --> B["描画・shader・GPU<br/>§9.9–9.13"]
    B --> C["症状と変更チェック<br/>§9.14–9.16"]
    C --> D["編集・RPC・世代<br/>§9.17–9.21"]
    D --> E["領域別の難所索引<br/>§9.23–9.24"]
```
根拠（章構成）: `09_black_magic_and_gotchas.md:24` 以降の節見出し（矢印は読む順で、実行順ではありません）。

**この章の語。**

- **正本** — 変更の出発点として扱うデータで、そこから作る runtime や cache と区別します（[用語集](../manual/14_glossary.md#148-設計コード解説で頻出する用語)）。
- **publish / commit** — 候補の公開 / 取り消さない確定を指し、境界は transaction ごとに異なります（[scene](../manual/04_scene_ecs.md#scene-load-transaction)、[描画](../manual/06_rendering.md#render-generation)）。
- **lease** — 対象の寿命や一時的な操作権を保持するもので、GPU と preview の違いは[横断パターン](00_cross_cutting_patterns.md)から各所有節を引きます。

## 9.1 `DECLARE_MODULE` / `GET_MODULE`: 小さな service locator

初回生成・依存記録・公開・逆順破棄はひと組の契約で、freeze 後の新規生成と破棄中の参照保持に注意します。
[第2章 §2.2](02_runtime_lifecycle.md#22-declare_module--get_module-の正体) / [第2章 §2.3](02_runtime_lifecycle.md#23-明示teardownが必要な理由)。

## 9.2 typed handle は何を守り、何を守らないか

typed handle の型安全性、GPU handle の世代なし、ModelInstance の slot / scene 世代、animation ABI の世代・サイズ交渉は別々の契約です。
[第3章 §3.8](03_project_and_loading.md#38-asset-modelから描画instanceまで) / [第5章 §5.16](05_gameplay_and_services.md#516-姿勢を駆動する2つの様式-実装済みag-1a--ag-1b--ag-2)。

## 9.3 `PELICAN_REGISTER_EVENT` / `PELICAN_REGISTER_SYSTEM` / `PELICAN_REGISTER_BEHAVIOR`

登録は static 初期化と compile-time catalog の組合せで、event の宣言順・handler の第2引数・DLL アンロード前の payload 破棄を守ります。
[第5章 §5.3](05_gameplay_and_services.md#53-event-handlerを持つsystem) / [第5章 §5.4](05_gameplay_and_services.md#54-event-bus) / [第5章 §5.13](05_gameplay_and_services.md#513-object-behavior-実装済みwp155--beh0wp162--beh1wp167--beh2)。

## 9.4 `GameObjects::add()` の typestate builder

builder の型に Component ID と値の参照を蓄積し、`finish()` で runtime に渡すため、temporary を持つ builder を保存しないでください。
[第4章 §4.6](04_ecs_deep_dive.md#46-gameobjects-builderのテンプレートによる型の組み立て)。

## 9.5 Component type erasure と lifecycle callback

型消去 callback の noexcept 制約と登録時の一意性検査を守り、ID 宣言だけで runtime 登録が済んだとは考えないでください。
[第4章 §4.2](04_ecs_deep_dive.md#42-component-idとcomponentinfo) / [第4章 §4.15](04_ecs_deep_dive.md#415-ユーザー独自componentの現在地) / [第4章 §4.16](04_ecs_deep_dive.md#416-ecsを変更するときの不変条件)。

## 9.6 Archetype/SoA chunk と pointer lifetime

chunk の storage が再確保されなくても swap-delete で entity のアドレスは動くため、構造変更をまたぐ pointer は EntityId から再取得します。
[第4章 §4.4](04_ecs_deep_dive.md#44-archetypeとchunk) / [第4章 §4.7](04_ecs_deep_dive.md#47-削除は末尾要素での穴埋め) / [第4章 §4.5](04_ecs_deep_dive.md#45-entityの一括生成はtransaction)。

## 9.7 内部 ECS scheduler の並列性

read/write hazard は検出されますが、自動直列化は WARNING でしか分からず、同 level の開始順や raw write の version 更新に注意が必要です。
[第4章 §4.10](04_ecs_deep_dive.md#410-内部ecs-systemの登録) / [第4章 §4.11](04_ecs_deep_dive.md#411-変更検知) / [第4章 §4.12](04_ecs_deep_dive.md#412-依存グラフと並列実行-実装済み)。

## 9.8 Camera、light、collider はすべて同じ ECS component ではない

scene component は ECS chunk、light、physics、behavior に分岐し、camera の二重経路と画面の座標規約も追う必要があります。
[第3章 §3.5](03_project_and_loading.md#35-sceneのロード) / [第4章 §4.14](04_ecs_deep_dive.md#414-ecs-componentとして保存されないもの)。

## 9.9 Frame graph が保証するもの、しないもの

planner の node 順序と Vulkan の memory visibility は別の保証で、resource binding・stage/access・layout・queue ownership を併せて確認します。
[第6章 §6.3](06_rendering_vulkan_shader.md#63-frame-graph-planner-のアルゴリズム) / [第6章 §6.8](06_rendering_vulkan_shader.md#68-compute-task)。

## 9.10 Compute 設定の「parse 済み」と「実行済み」を区別する

parse できる設定がすべて実行されるとは限らず、dispatch の authority・buffer lifetime・queue capability の制限を確認します。
[第6章 §6.8](06_rendering_vulkan_shader.md#68-compute-task)。

## 9.11 Shader reflection と hot reload の境界

reflection は semantic を保証せず、reload / resize の rebind は対象が限られるため、set / binding / type / push layout を変える編集には個別の rebuild が要ります。
[第6章 §6.9](06_rendering_vulkan_shader.md#69-shader-referencecompilereflection) / [第6章 §6.10](06_rendering_vulkan_shader.md#610-pipelinefactory-と-hot-reload)。

## 9.12 GPU object は「C++で不要」になった時点では壊せない

GPU 資源の寿命は frame 数ではなく実 submission の lease が決めるため、完了まで lease を保持し、shutdown 前に idle と drain の順を守ります。
[第6章 §6.6](06_rendering_vulkan_shader.md#66-frame-target-window-と-headless-の共通インターフェース) / [第6章 §6.11](06_rendering_vulkan_shader.md#611-gpu-resource-の遅延破棄)。

## 9.13 宣言・schema はあるが、runtime が未完成または別経路のもの

ソースを読むときに「型がある = 利用可能」と誤解しやすい箇所です。

| 項目 | 調査時点の状態 | コード |
|---|---|---|
| `JsonArchiveLoader` | 実装済み。scene component/event JSON load に使用 | [`jsonarchive.cpp`](../../src/core/userpublic/serialize/jsonarchive.cpp#L6) |
| `JsonArchiveSaver` | `prop` 宣言のみで、この repository 内に定義なし | [`jsonarchive.hpp`](../../src/core/userpublic/serialize/jsonarchive.hpp#L32) |
| `BinaryArchive` | `prop` 宣言のみで、この repository 内に定義なし | [`binaryarchive.hpp`](../../src/core/userpublic/serialize/binaryarchive.hpp#L10) |
| pose action | **実装済み**(WP130/132)。pose は `poses` map から返り、未サンプルなら default `ActionPose`。flat 環境では pose サンプルが来ないので default が返る点に注意 | [`actionmap.cpp`](../../src/core/os/actionmap.cpp#L873) |
| `.surface` / material format | **runtime 接続済み**(WP116/117/122)。`.surface` は surfacecompiler で pipeline に、`.material.json` は lowering を経て `MaterialContainer` へ | [`surfacecompiler.hpp`](../../src/core/shader/surfacecompiler.hpp) / [`materiallowering.hpp`](../../src/project/materiallowering.hpp) |
| Studio project editor | D2、WP264 の viewport / Outliner 選択同期、WP266 の schema-driven property Inspector、WP275 の gizmo 操作まで実装済み。project は同じ child へ `--rpc --project` で渡り、identity は名前でなく `(scene_id, declaration_index)`。property edit / gizmo / live preview / undo / redo / save は公開 RPC を使い、terminal result まで成功扱いしない。picking / gizmo feature 無効は理由を警告し、自動有効化しない | [`SelectionModel`](../../src/devstudio/model/selection.hpp) / [`GizmoModel`](../../src/devstudio/model/gizmomodel.hpp) / [`InspectorModel`](../../src/devstudio/model/inspectormodel.hpp) / [`InspectorWidget`](../../src/devstudio/view/inspectorwidget.hpp) / [`EmbeddedViewport`](../../src/devstudio/viewport/embeddedviewport.hpp#L36) |
| swapchain capture | surface が TRANSFER_SRC を持てば windowed でも readback 実装済み。不可時のみ `capture unavailable_windowed` 例外 | [`swapchainframetarget.cpp`](../../src/core/vkcore/swapchainframetarget.cpp#L436) |
| frame graph levels | 計算/JSON 出力のみ。runtime は直列 node loop | [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1648) |
| custom Component public registration | ID macro はあるが安定 public boot hook なし。ただし登録解除 API と重複拒否は入った。解除と engine owner 登録は WP379a で host 専用へ | [`component/registerer.hpp`](../../src/core/userpublic/details/component/registerer.hpp#L78) / [`ecs/enginecomponentregistrar.hpp`](../../src/core/ecs/enginecomponentregistrar.hpp#L13) |
| behavior attachment | ✅実装済み(WP155 / 162 / 167) | [`behaviorarena.hpp`](../../src/core/gamelogic/behaviorarena.hpp#L213) |
| 物理 trigger event | ✅実装済み(WP179) | [`PhysWorld::updateTriggers()`](../../src/core/phys/physworld.cpp#L743) |
| 編集 RPC(query / snapshot / edit / undo / preview / journal) | ✅実装済み(WP153〜172) | [`editorcommandservice.hpp`](../../src/core/communication/editorcommandservice.hpp#L304) |
| ImGui inspector / asset browser | ✅実装済み(WP159 / 164 / 167)。ただし `--rpc` / headless / replay / golden / XR では無効 | [`inspector.hpp`](../../src/core/imgui/inspector.hpp#L167) |
| preview graph(第3 variant) | 🚧実装済みだが CPU 模式ラスタ(WP172)。隔離契約が本体で、見た目の忠実度は保証しない | [`previewgraph.hpp`](../../src/core/renderingpass/previewgraph.hpp#L16) |
| RenderDoc capture | 🚧受動のみ(WP140)。**エンジンは RenderDoc をロードしない** | [`renderdoccapture.hpp`](../../src/core/renderdoc/renderdoccapture.hpp#L66) |
| VRMA decode / retarget / AnimationSource | ✅実装済み(WP176 / 177 / 178) | [`vrmadecoder.hpp`](../../src/core/loader/vrmadecoder.hpp) / [`vrmaretarget.hpp`](../../src/core/animation/vrmaretarget.hpp) |
| `.vrma` の root motion 抽出(容器側) | 📐設計スロットのみ。`VrmaRootMotionPolicy` は `preserve_hips_translation` の 1 値だけ。**ただし root motion 自体は v0 で出荷済み**で、容器ではなく共通グラフ側の `pose.clip_sample{root_motion: "extract"}` が担います(AG-2)。抽出・pin・オブジェクトへの加算は §9.24 の難所「unwrapped time から作る root delta」 | [`VrmaRootMotionPolicy`](../../src/core/animation/vrmaretarget.hpp#L21) / [`poseClipSampleExtractsRootMotion()`](../../src/project/posegraphmodule.hpp#L45) / [`extractRootMotion()`](../../src/core/animation/posegraphruntime.cpp#L338) |
| authoring 側のオブジェクト宣言 identity | ✅実装済み(WP360)。raw viewとstageは [`AuthoringSceneAuthority`](../../src/core/loader/authoringsceneauthority.hpp#L35) に隔離し、runtime読者はidentityを保存した単一の [`ResolvedScene`](../../src/core/loader/resolvedscene.hpp#L268) を読む | [`SceneProjectionState`](../../src/core/loader/resolvedscene.hpp#L326) |

optional build feature には stub 実装もあります。たとえば SeqPlayer/VAT/RPC/audio/physics/renderdoc は build option により実装または disabled behavior が選ばれます。header が同じでも build artifact の能力は [`build_features.hpp`](../../src/core/build_features.hpp#L1) と各 `*_stub.cpp` を確認してください。

## 9.14 症状から読む場所を決める

| 症状 | 最初の確認 | 次の確認 |
|---|---|---|
| 起動中に module constructor 例外 | module initialization log、[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L103) | constructor 内の `GET_MODULE` 依存 chain |
| 起動後の `GET_MODULE` で例外 | `freezeCreation()` 後の新規 module 生成でないか | `prepareRuntimeModules()` / prepare フェーズへの依存先解決の移動 |
| entity が突然無効 | [`EntityId` generation](../../src/core/userpublic/details/ecs/entity.hpp#L12)、scene transition | remove/clear と stale handle test |
| component pointer の値が別 entity になる | [`swap-delete`](../../src/core/userpublic/details/ecs/chunk.cpp#L71) | pointer を structural mutation 越しに保持していないか |
| ECS system が動かない | matching component mask、force/version | explicit dependencies の cycle、empty chunk |
| ECS system が時々壊れる | 同 level の read/write conflict | `depends_list`、raw mutation、JobSystem race |
| ECS System が急に直列化された / 順序が変わった | ログの `ECS auto serialization: ...` WARNING | `read_indices` / `write_indices` の重なり、`predefined.cpp` の依存 |
| `--strict-assets` を付けた途端に起動しない | `ECS unordered component hazard between systems ...` | 明示 `depends_list` を足す |
| game system event が来ない | event macro が system macro より前に可視か | `onEvent` の完全な型 signature、event 短縮名 |
| behavior が動かない / `Unknown behavior type` | game DLL がロード済みか(`get_status.reload`) | scene の `behavior.type` と `PELICAN_REGISTER_BEHAVIOR` の stable name |
| behavior の `onEvent` が呼ばれない | event macro が behavior macro より前に可視か | 第2引数が `BehaviorContext&` か(`GameContext&` 版ではない) |
| `OverlapEnter` が 1 フレーム遅れる | `PhysicsTriggerSystem` は order `INT_MAX` | event は通常キュー経由なので配送は次フレーム |
| ライトが動かなくなった | engine 側の名前ベース自動アニメーションは撤去済み(§9.8) | `ctx.render().set*Light*()` をユーザーコードから毎フレーム呼ぶ |
| ライトが 1 個だけ描かれない | ログの `Light cap exceeded: ...` | `MAX_*_LIGHTS` |
| 絵が左右・上下に反転する / 面が裏から描かれて消える | 射影が `clipspace.hpp` の関数か(射影の外で y を反転していないか)、camera の前方が R·(0,0,−1) か(§9.8「画面は長く world の鏡像だった」) | mesh の pipeline の front face が dynamic か、`viewIsMirrored()`、raster pass の `clip_y` と `front_face`(画像の上で見た巻き方) |
| ModelInstance の描画が別モデルに化ける | `ModelInstanceId{index, generation, scene_epoch}` の generation | `isModelInstanceAlive()`、scene 再ロードによる epoch 進行 |
| `save_scene` が `RuntimeOnlyData` で失敗 | `load_gltf` で transient モデルを足していないか | `SceneLoader::hasRuntimeOnlyChanges()` |
| `edit` が `stale_revision` | `get_scene_revision` の `EditorWatchToken` | preview lease が `preview_epoch` を進めていないか |
| windowed で RPC 応答が来ない | フレームが進んでいるか | queue busy 応答(`-32000` / `reason:"busy"`)が来ていないか |
| RPC event payload が空 | event に `ref(JsonArchiveLoader&)` があるか | default-only JSON loader branch |
| frame graph の順が違う | [`currentFramePlanJson()`](../../src/core/vkcore/renderer.cpp#L4521) | reads/writes、after/before、declaration index |
| XR だけ表示が壊れる | `#xr` variant の feature 除外(`xr_excluded_features`) | `graph_variant_transition_trace`、XR feature policy |
| TAA の ghosting・再投影が乱れる | temporal reset のトリガ(set_time / camera 不連続 / resize / view 数 / variant 切替) | `resetTemporalHistory()`、previous object/skin/morph buffer |
| game DLL reload 後に状態が消える/残る | `RegistrationOwner` と DLL 内 static の寿命 | engine 側 System の function-local static(こちらは残る) |
| Vulkan validation の RAW error | buffer/image、stage/access、layout | `GENERAL→GENERAL` image case、queue family |
| shader reload 後だけ壊れる | compile log、reflection diff | descriptor/push layout を変更していないか |
| resize 後だけ texture が古い | target recreate と fullscreen rebind | 該当 pass が独自 descriptor を cache していないか |
| headless capture が真っ黒 | frame plan と execution trace | target layout、pass output、golden fixture |
| shutdown crash | wait-idle と deletion queue | ECS `deinit()` が既に壊れた GPU module を触っていないか |
| defer した GPU 資源が早すぎるタイミングで消える | `leaseForNextSubmission()` の lease を submission 完了まで保持しているか(§9.12) | `flushAll()` の前に wait-idle しているか |
| テストは緑なのに機能が壊れている | `ctest` の Skipped 件数(特に `-L gpu`) | テストが `catch (const std::exception &) → SKIP` で engine の例外を skip へ変えていないか(§9.20) |

## 9.15 大きな変更の安全チェックリスト

### Component/ECS を変更する

- Component ID は一意かつ64未満か。
- lifecycle callbacks は construct された object にだけ呼ばれるか。
- init failure の逆順 rollback が保たれるか。
- remove の swap entity(穴へ移した末尾 entity)に対して `id_table[...].ref.array_index` を更新するか(§9.6)。
- raw write 後に component version を更新するか。
- 並列 system の read/write conflict に明示 dependency があるか。
- empty chunk と zero entity batch を処理できるか。

### rendering resource/pass を変更する

- JSON definition、validation、runtime compilation、execution dispatch の4層を更新したか。
- resize 後の image view rebind があるか。
- hot reload 後の descriptor rebuild があるか。
- old GPU object を遅延破棄したか。submit 経路を足したなら submission lease を取って完了まで保持したか(§9.12)。
- buffer と image の両方に正しい stage/access barrier があるか。
- headless と swapchain の両 frame target で成立するか。
- flat と `#xr` の両 graph variant で成立するか。
- temporal history(history RT、object/skin/morph/override history)の reset 経路を更新したか。
- view 数変化時の FrameResources slot 再構成(device idle 待ち)を守ったか。
- plan fixture、execution trace、golden image を確認したか。
- `gpu` ラベルのテストを走らせ、**新しい skip が 1 件も出ていない**ことを確認したか(§9.20)。`ctest` の「0 失敗」だけでは足りません。

### public API/RPC を変更する

- `GameContext` から内部 module 型を漏らしていないか。
- frame boundary のどこで反映されるかを定義したか。
- error を invalid params と application error のどちらにするか決めたか。
- stdout へ protocol 外文字列を出していないか。
- 公開する関数/型に `PELICAN_API` を付けて DLL export したか。
- game DLL ABI(`gameLogicAbiVersion`)を壊していないか。
- pure parser test と actual player subprocess test の両方があるか。

## 9.16 logical frame と XR の不変条件(WP128〜135)

logical frame の全 view は同じ frame index・extent・format 契約を共有し、XR mirror の unavailable は drop として扱います。
[第6章 §6.4](06_rendering_vulkan_shader.md#64-logical-frame-renderlogicalframe-と-render-の1フレーム) / [第6章 §6.15](06_rendering_vulkan_shader.md#615-xr-composition-と-mirrorwp125133) / [第2章 §2.4](02_runtime_lifecycle.md#24-looprun-の五経路)。

## 9.17 編集 transaction の落とし穴(prepare / publish / CAS / ticket)

WP144〜WP172 で、変更のプロトコルがコードベース全体で統一されました。この節が第9章で最も重要な追加です。

以下は 3 層に分かれています。**(a) 1 回の編集をどう原子的に適用するか**(1〜2)、**(b) その編集をいつ受理してよいか**(3 の CAS / 4 の lease / 5 の gate)、**(c) どこで確定し、何を拒否・出力するか**(6〜8)です。(a) を束ねているのは [`EditorProjectionTransaction::commit(commands, adapters)`](../../src/core/loader/editorprojectiontransaction.hpp#L303) の 1 関数で、`base_revision` の照合 → command を staged document へ適用 → **全 adapter の `prepare()`** → **全 adapter の `publish()`** → document 公開 → 逆順に `finish()`、という並びです。

どこかで例外が出れば、そこまでに `prepare()` した adapter を**逆順に `rollback()`** して `Rejected` / `Failed` を返します([`EditorProjectionTransaction::commit()`](../../src/core/loader/editorprojectiontransaction.cpp#L897))。

### 1. prepare は throw してよいが、publish は絶対に失敗できない

[`ECSArchetypeMigrationAdapter`](../../src/core/ecs/archetypemigration.hpp#L45) のコメントが規範です。

> prepare() may throw but must not change published state. rollback() is invoked for
> every attempted prepare (including the one that threw). Publication must not fail.

同じ形が各所にあります。

| 場所 | API |
|---|---|
| ECS archetype | [`ECSArchetypeMigration::prepareAdd/prepareRemove`](../../src/core/ecs/archetypemigration.hpp#L121) → token の `publish()` / `rollback()` / `finish()`(全て `noexcept`) |
| ECS entity | [`ECSEntityMutation::prepareCreate/prepareDestroy`](../../src/core/ecs/archetypemigration.hpp#L109) |
| ECS 既存値 | `prepareComponentValue()` / `publishComponentValue()` / `rollbackComponentValue()` |
| model instance | `preflightModelInstance()` → `stageModelInstance()` → `publishModelInstance()` |
| behavior | [`PreparedBehaviorAttachmentEdits`](../../src/core/gamelogic/behaviorarena.hpp#L157) の `publish()` / `rollback()` / `finish()`(全て `noexcept`) |
| physics | [`PhysWorld::prepareBindings()`](../../src/core/phys/physworld.hpp#L87) → [`publishPrepared()`](../../src/core/phys/physworld.hpp#L94)(`noexcept`) |
| 編集投影 | [`EditorProjectionPublicationMode{StagedNoexcept, InverseToken}`](../../src/core/loader/editorprojectiontransaction.hpp#L39) |

`EditorProjectionPublicationMode` は「公開をどう戻せる形にしてあるか」の宣言です。`StagedNoexcept` は publish 時点に失敗要因が残らないよう prepare 側へ寄せ切る形、`InverseToken` は publish 後でも `rollback()` が完全な逆操作を復元できる形です。`publish()` / `rollback()` / `finish()` はどちらのモードでも `noexcept` で、`rollback()` は publish 前(prepared 状態を捨てる)・publish 後(逆トークンで戻す)のどちらの経路も allocation-free であることが要求されます([`EditorProjectionAdapter`](../../src/core/loader/editorprojectiontransaction.hpp#L185))。

> **設計決定:** 新しい adapter を足すときは **prepare / rollback / publish の三点セットを必ず作ってください**。「途中まで適用された状態」を許す実装を1つ混ぜるだけで、その adapter だけでなく、**同じ `commit()` が束ねる transaction 全体**(= 編集・reload・scene 遷移が共有するこのプロトコル)の原子性が崩れます。他の adapter が正しく rollback できても、その 1 つが戻らなければ transaction は半端な状態で終わるからです。

`load_gltf` の単一公開点がわかりやすい実例です([`instances.publishModelInstance()`](../../src/core/loader/scene.cpp#L2271))。

```cpp
// No operation below allocates: this is the single publication point for
// the entity's slot, draw commands, resources, and optional name.
instances.publishModelInstance(std::move(staged_instance));
```

名前バインディングも `unordered_map::node_type` を先に `extract()` して確保しておき、公開時に allocation が起きないようにしています。

### 2. 投影 adapter は 8 種で閉じている

[`EditorProjectionAdapterKind`](../../src/core/loader/editorprojectiontransaction.hpp#L25) は `EcsExistingValue` / `EcsArchetype` / `TransformClosure` / `RendererModel` / `Camera` / `Light` / `PhysWorld` / `BehaviorAttachment` の 8 種です。`archetypemigration.hpp` のコメントが境界を書いています。

> This is only a composition boundary. Light, collider, behavior, and other special
> attachments remain separate adapters owned by their respective projection WPs.

つまり ECS archetype 移行は「ECS の中だけ」を見ており、light / collider / behavior は別 adapter が責任を持ちます。

### 3. CAS は `SceneRevision` で行う。ただし revision だけでは足りない

編集の比較条件は[第13章の SceneRevision](../manual/13_editor.md#134-同時編集--actor-と-revision)と[条件付き params](../manual/13_editor.md#セッションと編集)を参照してください。watch トークンは [`EditorWatchToken{scene_revision, preview_epoch}`](../../src/core/communication/editorcommandservice.hpp#L222) の2要素なので、preview の変化も監視するクライアントは両方を保持します。

### 4. preview は lease(ticket)

`open_preview` → `update_preview` → `commit_preview` / `abort_preview` の流れです(lease は「開いている間その対象の編集を 1 人が占有する権利」、ticket はその lease 1 件を指す識別子です)。関連するエラーコードは [`EditorEditErrorCode`](../../src/core/communication/editorjournal.hpp#L50) の `preview_lease_conflict` / `preview_lease_busy` / `not_lease_owner` / `ticket_not_found` / `undo_conflict` です。composition root 用の緊急口として `EditorCommandService::forceAbortPreview(reason)` があります。

### 5. 編集ゲートは 5 ビット。「受理時に開いていた」は実行してよい理由にならない

[`EditorGateReason`](../../src/core/communication/editorjournal.hpp#L21) は `replay` / `golden` / `strict` / `reload_scene_transition` / `preview_lease_conflict` のビットフラグです。[`EditorGateObservation`](../../src/core/communication/editorjournal.hpp#L33) の `transition_epoch` のコメントが規範です。

> A counter the composition root advances when it observes a reload or
> scene transition (internal::EditorTransitionCounter), so an edit that
> straddles one is rejected even if the gate is open again.

behavior コールバック実行中 / DLL リロード中の追加ゲートは [`internal::applyBehaviorEditConcurrencyGate()`](../../src/core/communication/editorruntimefactory.hpp#L115) です。

その「遷移」は 2 段で数えます。

- reload service の [`transitionFacts()`](../../src/core/watch/reloadservice.hpp#L238) が、watcher の state と epoch、各 runtime participant の reload 結果(requested / attempted / applied / failed / error)を型付きの [`ReloadTransitionFacts`](../../src/core/watch/reloadtransition.hpp#L17) で返します。participant の `details` は**型として入る場所がありません**。
- editor runtime が 1 つだけ持つ [`internal::EditorTransitionCounter`](../../src/core/communication/editorruntimefactory.hpp#L127) が、現在の scene id とその facts を前回と比べ、違えば数を 1 進めます。これが `transition_epoch` です。hash ではないので衝突せず、元の値に戻る遷移(A→B→A)も遷移として数えます。

受理した編集を commit してよいかの判定は、journal・render config・procedural asset の 3 つの editor が同じ [`EditorGateEpoch`](../../src/core/communication/editorgate.hpp#L18) で行います。gate の観測(開閉・理由・`transition_epoch`)が前回と違えば epoch を進め、受理時の epoch と commit 時の epoch が違えば `gate_closed` です。閉じてから開き直した gate も変化として数えます。

`details` は participant の自己記述で、たとえば window を resize すると render pipeline の再 lowering が `details.generation` を進めます。以前は reload 状態の JSON 全体を hash していたため、受付と frame 境界の間に viewport の大きさが変わるだけで編集が `gate_closed` で捨てられ、v0 の GPU gate で `studio_scenario_graph_observe_roundtrip` が負荷次第で落ちていました(`ba82495f` で応急修正、上の形への一本化で構造ごと解消)。**participant に状態を足すときは、遷移なら `ReloadTransitionFacts` の側、観測値なら `details` の側に置いてください。**

### 6. 編集コミットはフレーム境界に 1 点だけ

[`invokeEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L148) が reload 公開の後・`freeze_events` の直前に走ります。フックは **単一所有**で、2 回目の [`installEditorCommitQueueHook()`](../../src/core/appflow/framephase.hpp#L40) は `false` を返します。未設置なら zero-state no-op で、モジュールを作りません。

### 7. 保存拒否の理由コード

[`EditorCommandErrorCode`](../../src/core/communication/editorcommandservice.hpp#L31) は 13 種です。特に注意すべきものを挙げます。

- **`RuntimeOnlyData`**: [`SceneLoader::hasRuntimeOnlyChanges()`](../../src/core/loader/scene.hpp#L269) が真のとき、つまり `load_gltf` で持ち込んだ transient モデルがあるときに出ます。「RPC で読み込んだモデルは保存できない」という意味です。
- `ExternalModification`: ディスク上の scene が外部で書き換わっていた。
- 上限は [`maxSceneSnapshotBytes = 64 MiB`](../../src/core/communication/editorcommandservice.hpp#L29)、JSON 整数の安全上限は [`maxExactEditorJsonInteger = 9007199254740991`](../../src/core/communication/editorcommandservice.hpp#L28)。

### 8. snapshot は「ファイル内容」ではなく semantic bytes

`export_scene_snapshot` の `semantic_scene_bytes` は **キー順が正規化された文字列** です。実物([`test/fixtures/editor_command_service/export_scene_snapshot_v1.json`](../../test/fixtures/editor_command_service/export_scene_snapshot_v1.json) の `expected_response`)を見ると `scenes` → `schema` → `version` の順になっています。

```json
{
  "schema_version": 2,
  "scene_revision": 42,
  "current_scene_id": "main",
  "semantic_scene_bytes": "{\"scenes\":{\"main\":{\"objects\":[]}},\"schema\":\"pelican.scene\",\"version\":2}",
  "digest": {"algorithm": "sha256", "hex": "e74c47dd1a3d08a001a0e9d5eeeaf1d2f0ed78d5c55ad94c1c55e5ee4904d06d"},
  "pending_ticket_ids": [],
  "preview_epoch": 7
}
```

(ファイル名は `_v1` のままですが、MG-2b から中身は SnapshotV2 の要求と scene v2 の文書です。実際の export の応答にはこのほか `prefab_closure` が付きます。)

外部ツールで digest を再計算するときは `AuthoringSceneDocument::encodeSemantic()` と同じ正規化が必要です。**ファイルを読んで sha256 を取っても一致しません。**

## 9.18 windowed RPC ホストの落とし穴

windowed RPC は frame boundary で dispatch するため、フレーム停止と queue の busy 応答を確認します。
[第2章 §2.4](02_runtime_lifecycle.md#24-looprun-の五経路)。

## 9.19 preview / `render_preview` の隔離

preview は専用の隔離実行系で、CPU 模式ラスタの絵を production renderer の忠実度と同一視しないでください。
[第6章 §6.19](06_rendering_vulkan_shader.md#619-preview-実行系previewexecutor-模式ラスタwp172)。

## 9.20 テストの `catch (const std::exception &) → SKIP` は、回帰を緑のまま隠す

engine の例外を broad catch で SKIP にすると回帰が緑に隠れるため、capability 検査と skip policy を確認します。
[第7章 §7.8](07_tools_rpc_tests.md#78-テスト構成-test-を実装の仕様書として読む)。

## 9.21 prepare / publish 世代のコードで踏む新しい罠

§9.17 までの落とし穴は「型と寿命」でしたが、scene load transaction・schema wire・登録 ABI の周りには**同じ形をした別の罠**が増えています。どれも「素直に書くと通ってしまう」ものです。

### `OptionalOverride` の absent と「0 を set した」は別

[第5章 §5.14](05_gameplay_and_services.md#optional-override-presence)。

### `$list.next_id` は「行の最大値より大きい」しか強制されない

[第5章 §5.14](05_gameplay_and_services.md#list-next-id-watermark)。

### phase trace は participant が記録する。executor に `record` 文を足さない

[第3章 §3.5](03_project_and_loading.md#scene-phase-trace-owner)。

### `ReleaseReport` は heap を使わない — 8 件 + `overflow_count`

[第3章 §3.8](03_project_and_loading.md#release-report-capacity)。

### effect sink の有効範囲は「early publish の前から flush まで」

scene load 中に game DLL のコールバック(component の JSON loader を含む)が `playSound()` や `saveData()` を呼ぶと、それは即時実行ではなく [`PreparedEffectTransaction`](../../src/core/loader/effecttransaction.hpp#L27) に溜まります。scope を張る位置が要点で、[`EffectTransactionScope`](../../src/core/loader/scene.cpp#L1761) は **early publish より前**、prepare が終わった直後に張られます。コメントが規範です — 「early publication が呼ぶ component JSON loader を含め、**すべての game-DLL コールバックが同じ transaction-local な effect を観測**しなければならない」。

したがって「publish の直前で張れば十分」という縮め方は誤りです。prepare 中の loader が本物の audio / persistence を直接叩き、rollback しても戻らない副作用が残ります。逆側の端は [`effect_transaction->flush(...)`](../../src/core/loader/scene.cpp#L1646) で、commit 後の noexcept tail の中にあります。

### `noexcept` の commit tail の中で文字列や JSON を作らない

[第3章 §3.5](03_project_and_loading.md#scene-noexcept-tail)。

### `createObject()` は基底の `GameContext` から呼んでも defer される

[第5章 §5.13](05_gameplay_and_services.md#behavior-deferred-create)。

### persistence の key は case-fold した canonical path — ただし Windows だけ

[第5章 §5.10](05_gameplay_and_services.md#persistence-canonical-key)。

### `sizeof` だけの ABI 凍結は、同サイズ field の並べ替えを通す

game DLL 境界の v2 record は「凍結」と書かれていますが、実際に固定しているのは**大きさだけ**です。

| record | 凍結 |
|---|---|
| [`ComponentLoaderInfo`](../../src/core/userpublic/details/component/registerer.hpp#L81) | `sizeof(FrozenComponentLoaderInfoV2Layout)` と一致 |
| [`BehaviorRegistration`](../../src/core/userpublic/details/behavior/registerer.hpp#L50) | `sizeof(FrozenBehaviorRegistrationV2Layout)` と一致 |
| [`EventTypeRegistration`](../../src/core/userpublic/details/event/registerer.hpp#L54) | `sizeof(FrozenEventTypeRegistrationV2Layout)` と一致 |

`BehaviorRegistration` の `create` と `destroy` のように**同じ大きさの隣接 field を入れ替えても `static_assert` は通ります**。古い DLL は入れ替え前の順で書き込み、host は入れ替え後の順で読むので、症状は「関数ポインタが 1 つずれて呼ばれる」という追いにくい形になります。台帳の裁定は `sizeof` + `alignof` + **全 field の `offsetof`** へ移すこと(WP382a-fix。起票済み・未着地)で、既に同じ形を採っている面もあります(例: [`offsetof(ProviderV2, shape_cast_all) == offsetof(ProviderV2, overlap_all) + sizeof(...)`](../../src/core/userpublic/physics/abi_v2.hpp#L170))。ここを触るときは「大きさが変わらないから安全」と読まないでください。

### raw import 診断は provider-only な旧 DLL の唯一の enforcement

[第2章 §2.11](02_runtime_lifecycle.md#provider-only-import-check)。

## 9.22 横断パターン(初読で 5 つ以上の領域に同じ形で現れるもの)

<a id="三段トランザクションprepare--publish--finish"></a>
<a id="noexcept-なコミットテール"></a>
<a id="rollback-は公開の逆順lifocleanup-と-discard-は別物"></a>
<a id="generation-による-stale-ハンドル検出"></a>
<a id="identity--canonical-key--lease-の三層"></a>
<a id="fault-injection-seam-と-mutation-id"></a>

本文は[横断パターン](00_cross_cutting_patterns.md)へ移しました。節内の各パターンは同ページで読めます。

## 9.23 仕様の難所索引(領域別)

「読んでいて詰まった場所から引く」ための逆引きです。領域ごとに、**★ 印(初読で最も助けが要ると報告された箇所)は 1 件ずつ**、それ以外は表 1 つにまとめてあります。表の「答え / 参照」が既存の難所ブロックを指しているものは、**そちらが本体**です。79 問の答えは結論 1 文と本体の節へのリンクです。

答えが `—` になっているものは、この索引の調査時点で**コードにも設計文書にも根拠が見つからなかったもの**です。推測は書いていません。

### 9.23.1 loader / scene transaction

#### [`SceneLoader::load()`](../../src/core/loader/scene.cpp#L1652) — commit までの 5 段の責務と、どこから不可逆か

**分からなくなること**: prepare / early_publish / publish / activate / commit_metadata のどこを越えると引き返せなくなるのか。

**答え**: 全体は retire → flush → finish を含む 8 段です。不可逆点は metadata の単一 swap です。 → [第3章 §3.5 の本文](03_project_and_loading.md#reading-note-001)。

**台帳**: manifest 側の語彙は WP375(inverse token)/ WP380(mutation id の同居)。

#### [manifest 配列](../../src/core/loader/sceneparticipantmanifest.cpp#L81) — この表は何の権威か

**分からなくなること**: 巨大な定数配列が何を数えた表で、行数がいくつなのか。

**答え**: manifest は観測可能なフィールドごとの分類権威で、public API の表とは別です。 → [第3章 §3.5 の本文](03_project_and_loading.md#reading-note-002)。

#### [`resolveParentOrder()`](../../src/core/loader/componentcodec.hpp#L116) — 巡回検査の fresh→active→done

**分からなくなること**: なぜ 3 値なのか、なぜ既に検証済みのはずの循環をもう一度見るのか。

**答え**: active と done を分ける DFS により、共有親と循環を区別します。 → [第3章 §3.5 の本文](03_project_and_loading.md#reading-note-003)。

#### [`ProjectBasicConfig::saveSceneDocument()`](../../src/core/loader/basicconfig.cpp#L935) — 複数の注入点と baseline 再検証

**分からなくなること**: baseline を取ったのに、置換直前でもう一度検証するのはなぜか。

**答え**: baseline は scene bytes と prefab read-set を一体で固定し、置換直前に再検証します。 → [第3章 §3.5 の本文](03_project_and_loading.md#reading-note-004)。

#### [`ProjectBasicConfig::publishSceneDocument()`](../../src/core/loader/basicconfig.cpp#L716) — revision と resolver generation が進む条件

**分からなくなること**: revision と resolver generation がいつ進むのか(prepare で進むのか、publish で進むのか)。

**答え**: revision と resolver generation は prepare では進まず、公開1回につき1進みます。 → [第3章 §3.5 の本文](03_project_and_loading.md#reading-note-005)。

#### [`PreparedEffectTransaction::errors_`](../../src/core/loader/effecttransaction.hpp#L142) — なぜ capacity + 1 か

**分からなくなること**: 末尾の 1 スロットが何のためにあるのか。

**答え**: 末尾の +1 は通常診断と独立した outbox 溢れ専用の予約枠です。 → [第3章 §3.5 の本文](03_project_and_loading.md#reading-note-006)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`rollback` ラムダ](../../src/core/loader/scene.cpp#L1736) | 失敗時の巻き戻し | 順序の原則と、何もしない参加者 | → §9.22「rollback は公開の逆順」 |
| [static_assert 群](../../src/core/loader/scene.cpp#L1084) | commit tail の `noexcept` 固定 | 20 個の表明が何を守るか | → §9.22「`noexcept` なコミットテール」 |
| [`PendingSceneTransactionScope`](../../src/core/loader/scene.cpp#L227) | thread_local + RAII の ambient context | なぜ引数で渡さないのか | early publication が呼ぶ**全 game-DLL callback**に同じ transaction-local な effect を観測させるため。`requestLoad()` は `active_pending_scene_transaction->loader == this` を見て live ではなく candidate へ書く |
| [run_nonce の初期化](../../src/core/loader/scene.cpp#L169) | `ticks ^ pointer`、0 を回避 | なぜ 0 を避けるのか | 0 は既定構築の `SceneAuthority{}`(= まだ何も commit していない)に予約されている |
| [`stagePreparedSceneModel()`](../../src/core/renderer/polygoninstancecontainer.cpp#L772) と publish の分離 | 容量検査 + 確保 / 挿入 | なぜ 2 つに割るのか | stage が `ModelInstanceId` を返す失敗しうる側で、loader は公開前にその id を `SimpleModelViewComponent` へ書く必要があるから |
| [`SceneLoadRequest`](../../src/core/loader/sceneload.hpp#L222) の Borrowed / Owned | 入口ごとの所有形 | 2 形態がある理由 | Borrowed は commit 済み projection の読み直し(metadata は複製されるだけ)、Owned は import root で未公開の候補を token ごと所有し、失敗時は token を捨てるだけ |
| [`MetadataCommitToken`](../../src/core/loader/sceneload.hpp#L195) | opaque pointer | なぜ中身を隠すのか | 呼び手が「document だけ」「projection だけ」を公開できないようにするため。唯一の commit は `SceneMetadataState` 全体の swap |
| [transform → localtransform 自動付与](../../src/core/loader/scene.cpp#L399) | `transform` を持つ全 object への補完(GP-1) | なぜ loader が component を足すのか、足した local は保存されるのか | authored なのは local TRS、runtime の読み手は world の `TransformComponent`。`LocalTransformSystem` は local を持つ object の world を local から毎フレーム作り直す。GP-1 から scene の `localtransform` 宣言が無い全 transform object に足す(以前は scene に `parent` が 1 つでもあるときの親子参加者だけで、parent の無い scene では `setLocalTransform` が `false` を返していた — 解消済み)。足した分は JSON を持たない `injected_local` で、値は投影段が authored の transform から書く。authored object の component ではないので**保存には出ない**。world を直接書く経路(`SceneLoader::applyObjectTransform`・editor の spawn)は local も書く。足さないのは editor の `add_component` で後から足した `transform`(次の load まで)と `load_gltf` の transient object |
| [`sceneLoadMutationEnabled()`](../../src/core/loader/sceneloadfault.cpp#L30) | SceneLoadRoot の 5 値 | 分類の意味 | 本番の入口は startup / rpc / pending / import_versioned の **4 種**で、manifest の `role = production_root` 行(4 行)と 1:1(`import_unversioned` は MG-2b で SnapshotV1 と一緒に撤去)。enum 先頭の `unspecified` だけは manifest 行も mutation id も持たない |
| [`create_object` ラムダ](../../src/core/loader/scene.cpp#L1255) | prepareCreate → publish → finish | 三段の不変式 | → §9.22「三段トランザクション」/ [第4章「生成transactionの三段構え」](04_ecs_deep_dive.md) |
| [`markComponentChanged`](../../src/core/loader/scene.cpp#L1342) | 親を書いた直後の版上げ | なぜ手動で呼ぶのか | `tryComponent<T>()` は生ポインタを返すだけで版を上げない(版を上げるのは [`setComponent()`](../../src/core/userpublic/details/ecs/coretemplate.hpp#L373) 側)。`local->parent` を直接書いた経路では手動で上げないと change detection が親リンクの変化を見落とす |
| [`loadColliderComponent()`](../../src/core/loader/scene.cpp#L287) | `runtime_codec` の null 検査 | なぜ `logic_error` なのか | resolver が具体 component に codec を必ず付ける契約なので、null は authoring の誤りではなく内部不整合。同関数の `ResolutionFailureException` 経路(= 作者の誤り)と行き先を分けている |
| [`loadTransientGltf()`](../../src/core/loader/scene.cpp#L2183) | 二重予約と `owns_resources` | なぜ 2 回予約するのか | ハッシュノードとバケット成長を**パース・GPU staging より前**に確保し(コメントが明言)、entity 生成後の公開が確保できない = transaction を割れないようにする。容量拒否は副作用の無い [`inspect()`(WP110)](../../src/core/loader/scene.cpp#L2222) + `preflightModelInstance` で先に行う |
| [`displayObjectName()`](../../src/core/loader/scene.cpp#L277) | 診断用の表示名 | 常に `<unnamed>` に見える | 空のときだけ `<unnamed>` を返す 1 行。identity ではなく**表示**なので、同一性の判定に使わない |
| [`scenePersistenceBaseline()`](../../src/core/loader/basicconfig.cpp#L361) | prefab closure と read-set | baseline が何の複合か | scene ファイルの digest + resolve が実際に読んだ prefab 集合(name/path/digest)。registry に無い prefab が closure にあれば `logic_error` |
| [`SceneMetadataState`](../../src/core/loader/basicconfig.hpp#L106) | 3 本の番号の役割分担 | revision / generation / object_id の違い | revision = authoring document の改訂、resolver generation = 解決結果の世代(読み手のキャッシュ無効化)、next_authoring_object_id = 宣言 ID の払い出し高水位(scene をまたいで再利用しない) |
| [`temporaryScenePath()`](../../src/core/loader/basicconfig.cpp#L428) | atomic カウンタ + nonce | 2 系統ある理由 | atomic は同一プロセス内の連続 save、steady_clock nonce は別プロセス・再起動・前回の残骸 tmp。保存先と同じ親ディレクトリに置くので `EXDEV` を踏まない |
| [`JsonLoader` の CLI / project / default 3 層](../../src/core/loader/basicconfig.cpp#L64) と [prefab_registry 構築](../../src/core/loader/basicconfig.cpp#L624) | 設定のフォールスルー | 初期化順序の根拠 | 優先順位は実装どおり(CLI > project > default)。**順序をその形にした理由はコードにも文書にも無く、未執筆** |
| [`importSceneDocument()`](../../src/core/loader/basicconfig.cpp#L792) | import のトランザクション意味論 | schema version 1/2 の扱い | **未執筆**(§9.23.10 の gate 順序と対で読む必要がある) |
| [`CallbackPhase`](../../src/core/loader/effecttransaction.hpp#L29) と [destroy 相で診断を積む catch(`playSound()` の例)](../../src/core/loader/effecttransaction.cpp#L163) | activate / destroy / flush | destroy でだけ再送出しない理由 | destroy の呼び出し元は `noexcept` かつ**既に commit 済みで rollback 不能**なので、throw は terminate か中途半端な破棄にしかならない。固定長診断へ落として `CommittedWithEffectFailure` として報告する |
| [`busValue()`](../../src/core/loader/effecttransaction.cpp#L36) / [`effectErrors`](../../src/core/loader/effecttransaction.cpp#L678) | const 版の copy、ErrorFields 分岐 | 何を分けているのか | **未執筆**(根拠がコードにない) |

### 9.23.2 resolver / componentcodec / prefab

#### [`validateOriginTransitionMap()`](../../src/core/loader/resolvedscene.cpp#L494) — gid / component_key / authored_index の三者

**分からなくなること**: 3 つの識別子が何を結び、この検査が何を守っているのか。

**答え**: 遷移表は prefab 生成物と authored 宣言の可逆な対応を保証します。 → [第3章 §3.12 の本文](03_project_and_loading.md#reading-note-007)。

**台帳**: 生成 ID のバイト定義は [prefab.hpp のコメント](../../src/project/prefab.hpp#L224)どおり WP361 v4。

#### [`ComponentCodec::completeDefaults()` の camera 補完](../../src/core/loader/componentcodec.cpp#L2264) — aspect を継承しない

**分からなくなること**: 省略されたフィールドが「既定から継承される」ものと「クリアされる」ものに分かれる基準。

**答え**: projection を明示して aspect を省略した場合は、viewport 由来なので継承しません。 → [第3章 §3.12 の本文](03_project_and_loading.md#reading-note-008)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [4 集合の照合](../../src/core/loader/resolvedscene.cpp#L645) | 全単射の検査 | なぜ 4 つも要るのか | `expected_*` は 2 つの snapshot から導いた真値、`actual_*` は map entries 側の実測。挿入の一意性 + 集合の完全一致で全単射を証明し、取りこぼし・重複・区間外 index へのエイリアスを排除する(これが `invertOriginTransitionMap` の健全性の前提) |
| [`resolverInputsFingerprint()`](../../src/core/loader/resolvedscene.cpp#L757) | resolver 入力の SHA256 | 何のための指紋か | camera 既定投影 + sprite 既定の正準 SHA256 を closure に載せ、[期待値比較](../../src/core/communication/editorcommandservice.cpp#L451)で不一致なら stale として再解決を強制する |
| [`resolveComponent()`](../../src/core/loader/resolvedscene.cpp#L212) | origin タグと遷移規則 | authoring / generated / prefab の別 | origin は型で保持され(generated は gid・prefab・instance_id・component_key・digest を持つ)、非 generated 経路に生成マーカーが混入したら拒否する |
| [behavior の canonicalize_params 呼び出し](../../src/core/loader/resolvedscene.cpp#L256) | behavior だけ別経路 | なぜコア codec を通さないのか | behavior はコア codec を持たず、**登録済み behavior の schema が受理の正**だから。[211 行のコメント](../../src/core/loader/resolvedscene.cpp#L256)どおり pre-WP360 の親 RPC 表現を再現するため、正準化結果を `source_json_exact["params"]` へ書き戻す |
| [`ResolvedSceneResolver::resolve()`](../../src/core/loader/resolvedscene.cpp#L780) | 4 フェーズ | 相の前提と依存 | 1a は codec を見ずに instance 検証、1b は provider 検証後に具体 JSON へ置換、2 は authored-first / generated-second で materialize し transform 個数・重複名・gid 一意性を**デコード前に**強制、3 でオブジェクト同一性を凍結、4 で宣言順に canonicalize。「構成規則の違反は runtime decode より前に落とす」ため 2 が 4 に先行する |
| [`name != "behavior"` の重複検査](../../src/core/loader/resolvedscene.cpp#L904) | 重複 component の拒否 | behavior だけ通る理由 | behavior は 1 オブジェクトに複数付けられる唯一の種別(`type` で区別)。gid 一意性は behavior にも等しく適用される。なおこの重複検査が走るのは **prefab instance の merged component 集合に対してだけ**で、素の object の重複はこの経路では見ていない |
| [`requireNonZeroQuat()`](../../src/core/loader/componentcodec.cpp#L440) | 四元数の検査 | なぜ正規化しないのか | 弾くのは長さゼロの退化四元数だけで(`OutOfRange` "must be a non-zero quaternion")、値には触れない。宣言 schema は成分ごとの range しか持てないので、値全体の規則であるこの検査だけが手書きで残る。正規化しない理由は関数のコメントに書かれた: decode → `encodeCanonical` のバイト厳密な往復を守るためで、正規化するのは world への合成側([`composeWorldTransform()`](../../src/core/loader/componentcodec.cpp#L2405)) |
| [`projectTransform()`](../../src/core/loader/componentcodec.cpp#L683) | 親からの逆変換 | 順序と epsilon の意味 | `applyTransform` の合成を厳密に逆順で解く(親並進を引く → 親回転の逆 → 親スケールで割る)。epsilon は親スケールの軸が 0 近傍のとき inf/NaN を静かに作らず `transform.parent.scale` の OutOfRange として名指しで落とすため |
| [camera.controller の二重指定](../../src/core/loader/componentcodec.cpp#L1058) | nested と非 nested | どちらが優先か | **優先順位は存在しません**。両方あれば `UnknownField` で拒否するので、`controller = nested_controller` の代入に到達するのは非 nested が無いときだけです(「nested 優先」は誤読) |
| [`decodeRayInstanceFlags()`](../../src/core/loader/componentcodec.cpp#L1718) | instance_flags の decode | 制約と排他の理由 | 「文字列配列」「既知 4 名のみ」「重複禁止」「`force_opaque` と `force_no_opaque` は排他」。最後は AS インスタンスフラグとして同時指定が矛盾するため。各失敗は `LegacyBranchDescriptor` 表に登録された `pelican_ray_instance.*` の安定 branch id を持つ(**その id を何に使うかを述べた記述はコードにない**) |
| [`decodeCollider()`](../../src/core/loader/componentcodec.cpp#L1418) | normalized にしてから宣言 schema | なぜ二段なのか | **向きが逆です**。受理の唯一の権威は codec の宣言 schema 側で、[`ColliderComponent::loadFromJsonArchive()`](../../src/core/userpublic/components/collider.cpp#L39)(= `ref()` 経路)は `requireComponentCodec("collider")` を引いてこの codec へ委譲します。前段の `normalized` が残るのは、`shape` 自体の既定(`sphere`)と shape 依存の既定値(sphere/capsule の radius、box の half_extents、capsule の half_height)が codec 側の正準既定だからで、加えて **その shape が読まない鍵を `erase` してから** schema へ渡します(`{"shape":"sphere","half_height":-1}` が受理されたままなのはこのため。宣言 range に見せると落ちる)。値全体の規則(厳密な正値、どの shape にどれが効くか)は JSON を通らない生成経路のために [`ColliderComponent::validate()`](../../src/core/userpublic/components/collider.cpp#L47) に残ります |
| [`decodeSchemaFieldsWithLegacyBranches()`](../../src/core/loader/componentcodec.cpp#L638) | schema の失敗を v1 の branch id へ訳す | 宣言化で拒否 id が変わらない理由 | `decodeSchemaFields` を try で包み、`StructFieldValidationError` の path と [`reasonOf()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L137) の `StructFieldErrorReason` を規則表([collider の表](../../src/core/loader/componentcodec.cpp#L1332))で引いて、旧手書きパーサと同じ `collider.*` / `sprite_view.*` の branch id で `legacyReject` する。表に無い理由は素通しで `<codec>.schema_validation` になるので、[`legacyRulesAreTotal()`](../../src/core/loader/componentcodec.cpp#L584) の `static_assert` が「その field の型と presence から到達しうる理由」を全部覆っているかを **compile 時に**要求する。shape のように規則が分岐する field は `discriminant` を持ち、[`legacyRuleDiscriminantsAgree()`](../../src/core/loader/componentcodec.cpp#L605) が「どの shape でも同じ理由集合を覆う」ことを課す |
| [`decodeCamera()`](../../src/core/loader/componentcodec.cpp#L946) / [`findCameraNumber()`](../../src/core/loader/componentcodec.cpp#L875) | nested / fallback / type 推論 | 既定値表が読めない | 個別規則はコードから読めるが、**既定値の一覧表としては未執筆**(camera defaults 表と対で図示が要る) |

### 9.23.3 ECS

#### [`State::publish()` の backfill](../../src/core/ecs/archetypemigration.cpp#L98) — swap-erase が id_table の参照をどう保つか

**分からなくなること**: 行を 2 回動かしているのに、どちらの entity の参照も壊れない理由。

**答え**: 最終行自身を移す場合の二重書き込みを避けて、backfill 後の参照を更新します。 → [第4章 §4.17 の本文](04_ecs_deep_dive.md#reading-note-009)。

#### [`populate` → `cb_init` の順序](../../src/core/ecs/archetypemigration.cpp#L562) — publish 前に init する理由

**分からなくなること**: ユーザー値の書き込み(populate)と型のライフサイクル初期化(`cb_init`)の順序と、なぜ公開前に済ませるのか。

**答え**: 失敗しうる populate と init を公開前に完了し、逆操作に必要な記録を残します。 → [第4章 §4.17 の本文](04_ecs_deep_dive.md#reading-note-010)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`rollbackPublished()`](../../src/core/ecs/archetypemigration.cpp#L229) | 新しい `MutationScope` を作る | なぜ作り直すのか | publish 末尾で `mutation.reset()` してスコープを手放しているから(「集約エディタトランザクションの残りにコア全体の構造ガードを持ち越さない」)。`mutation_active` が再入を `logic_error` で弾くので巻き戻し中も排他は保たれる |
| [`finishPublished()`](../../src/core/ecs/archetypemigration.cpp#L352) | 取り除いた component の `cb_deinit` | なぜ finish なのか | **remove 経路で退避した実体を deinit するのはここだけ**(add 側の候補は `discardPrepared()` / `rollbackPublished()` が対称に deinit する)。削除が最終確定するのは finish の時点だけで、それ以前は `rollbackPublished` が退避物を source chunk へ復活させる。prepare/publish で deinit すると巻き戻しに必要な状態を壊す |
| [`prepareContext()` 経由の adapter 登録](../../src/core/ecs/archetypemigration.cpp#L620) | 「prepare は throw してよいが状態を変えない」 | どう強制しているのか | adapter は prepare を呼ぶ**前**に `state->adapters` へ push されるので、prepare が投げても外側の `catch (...)` → `discardPrepared()` が逆順に全 adapter の `rollback()` を呼ぶ。`rollback()` / `publish()` は純粋仮想側で `noexcept` を要求 |
| [staging construct](../../src/core/ecs/archetypemigration.cpp#L125) | 既存 chunk への構築とプレースホルダ | tail capacity と SoA 配置の関係 | コメントどおり「prepare が live storage に触れずに失敗できるよう全 target component を staging に構築する。既存 chunk は raw tail capacity を既に持っているので、**新しく足した値だけを残し、entity 由来の component のプレースホルダは捨てる**」。SoA 配置との関係そのものは**未執筆** |
| [live / dead の判定](../../src/core/ecs/archetypemigration.cpp#L466) | `core.resolve(entity)` | live の定義 | **未執筆**(id_table の live フラグと free_indices の関係を書く必要がある) |
| [reusable chunk の逆順探索](../../src/core/ecs/archetypemigration.cpp#L594) / [生成側の同じ探索](../../src/core/ecs/archetypemigration.cpp#L1063) | 空き chunk の選択 | 逆順に探す理由 | **未執筆**(根拠がコードにない。移行側と生成側が同じ `rbegin` 走査で、"packing" という語も概念もコードには無い) |
| [destroy 時の staged_chunk への退避](../../src/core/ecs/archetypemigration.cpp#L1115) | 破棄前の退避 | なぜ退避するのか | rollback-after-destroy の逆操作を可能にするため(finish まで実体を捨てない。上の `finishPublished` と同じ原理)。実体は copy ではなく relocate(move) |
| [adapter の境界](../../src/core/ecs/archetypemigration.hpp#L41) | composition boundary | light / collider が別 adapter な理由 | コメントが "This is only a composition boundary. Light, collider, behavior, and other special attachments remain separate adapters owned by their respective projection WPs." と明記している |
| [`registerComponent()`](../../src/core/ecs/componentinfo.cpp#L25) | ComponentId == index | 添字と ID が同一である帰結 | → §9.5「Component ID は dense index でもある」 |
| [`recordDependencyLocked()`](../../src/core/container.hpp#L83) | 依存循環の実行時検出 | なぜ事前宣言できないのか | → §9.1 の難所「遅延生成の 49 行」(`construction_stack` と `recursive_mutex` の対) |
| [root の近道](../../src/core/ecs/predefined/localtransformsystem.cpp#L74) | `LocalTransformSystem` が root を毎フレーム local から作り直す | `TransformComponent` を直接書いたのに次のフレームで元に戻る | GP-1 から scene の transform object は全部 local を持つので、root も毎フレーム `world = compose(local)` で上書きされる(以前は親子の無い object には local が無く、world の直書きが残っていた)。world を動かすなら local を書く: 公開 API は `setLocalTransform`、エンジン内は [`SceneLoader::applyObjectTransform()`](../../src/core/loader/scene.cpp#L2138) のように world と一緒に local も書く(親があれば親の world で逆算)。test で world を直接書いていたものも同じ理由で次フレームに戻る |

### 9.23.4 renderer / frame

#### [`plan` と `target_plan` の照合](../../src/core/vkcore/renderer.cpp#L1853) — 2 つのプランが並ぶ理由

**分からなくなること**: 論理プランと物理プランのどちらが「正」なのか、`fused_rendering_scope` はいつ true になるのか。

**答え**: 論理 plan と任意の物理 target plan を照合し、scope の実行者を排他的に決めます。 → [第6章 §6.4 の本文](06_rendering_vulkan_shader.md#reading-note-011)。

#### ["The provider owns all ordering"](../../src/core/vkcore/renderer.cpp#L2121) — 責任分界

**分からなくなること**: エンジンが barrier を出す区間と出さない区間の境目。

**答え**: native scope 内部の順序は provider が所有し、engine は scope 境界の依存だけを処理します。 → [第6章 §6.4 の本文](06_rendering_vulkan_shader.md#reading-note-012)。

#### [fused scope の内部依存](../../src/core/vkcore/renderer.cpp#L2187) — なぜ dynamic rendering で実行できないか

**分からなくなること**: 同じスコープに入れたはずのノード間依存が、なぜエラーになるのか。

**答え**: fused scope 内では layout 遷移を出せず、local attachment read だけが例外です。 → [第6章 §6.4 の本文](06_rendering_vulkan_shader.md#reading-note-013)。

#### [`executePlannedFrameGraph()`](../../src/core/vkcore/renderer.cpp#L1648) — main family と他 family で schedule 構築が違う

**分からなくなること**: なぜ main family 以外では `view_index != 0` を捨てるのか。

**答え**: main family と view 非依存 family では記録回数が異なります。 → [第6章 §6.4 の本文](06_rendering_vulkan_shader.md#reading-note-014)。

#### [`transaction->generation == 0`](../../src/core/vkcore/renderer.cpp#L3872) — 番兵としての 0

**分からなくなること**: 世代 0 が「世代が 0 番」なのか「世代が無い」なのか。

**答え**: generation 0 は runtime が未公開であることを示す番兵です。 → [第6章 §6.4 の本文](06_rendering_vulkan_shader.md#reading-note-015)。

#### [`beginLogicalFrameTarget()`](../../src/core/vkcore/renderer.cpp#L6531) — 出力待ちで飛ばした frame は staged な世代を巻き戻さない

**分からなくなること**: frame が途中で終わったとき、staged な ray tracing の世代を巻き戻すのか残すのか。

**答え**: 出力 image 待ちの frame skip では staged 世代を次の frame に持ち越します。 → [第6章 §6.4 の本文](06_rendering_vulkan_shader.md#reading-note-016)。

#### [`buildLogicalFrameViewFamilySchedule()`](../../src/core/renderingpass/viewexecutionscheduler.hpp#L120) — multiview と sequential の差

**分からなくなること**: schedule に view の数(cardinality)が要る理由。

**答え**: multiview は一括記録、sequential は view ごとの展開なので cardinality の検証が要ります。 → [第6章 §6.4 の本文](06_rendering_vulkan_shader.md#reading-note-017)。

#### [`makeFrameDescriptorPoolPlan()`](../../src/core/renderer/frameresources.cpp#L28) — multiplier 3 / 6 の根拠

**分からなくなること**: 3 と 6 という定数が何を数えているのか。

**答え**: descriptor の係数は uniform 3個と storage 3個、および ray-query 用の二重セットから来ます。 → [第6章 §6.6 の本文](06_rendering_vulkan_shader.md#reading-note-018)。

#### [`GpuSubmissionLease`](../../src/core/vkcore/frametarget.hpp#L53) — 型消去リースが 2 種類ある

**分からなくなること**: `submission_lease_` と `target_epoch_lease_` が同じ型なのに別物である理由。

**答え**: submission の資源寿命と target epoch の記録中寿命は別軸です。 → [第6章 §6.11 の本文](06_rendering_vulkan_shader.md#reading-note-019)。

#### [`RetirementBatch`](../../src/core/vkcore/deletionqueue.hpp#L50) — なぜ weak_ptr で持つのか

**分からなくなること**: [`flushAll()`](../../src/core/vkcore/deletionqueue.cpp#L88) の `lock()` が失敗する条件。

**答え**: batch の所有者は current batch と lease で、queue の弱参照は寿命を延ばしません。 → [第6章 §6.11 の本文](06_rendering_vulkan_shader.md#reading-note-020)。

#### [`stagePreparedSceneModel()` の reuse 判定](../../src/core/renderer/polygoninstancecontainer.cpp#L772) — スロット再利用と世代

**分からなくなること**: 「再利用」と「新規確保」をどこで見分けているのか。

**答え**: 次の slot index が現在サイズに等しいかどうかで新規確保と再利用を区別します。 → [第3章 §3.8 の本文](03_project_and_loading.md#reading-note-021)。

#### [`uploadMaterialAbsoluteOverrides()`](../../src/core/renderer/polygoninstancecontainer.cpp#L2188) — instance_identity が 1-indexed

**分からなくなること**: なぜ添字を 1 つずらすのか。

**答え**: instance identity 0 は未設定なので、密配列には identity - 1 でアクセスします。 → [第6章 §6.4 の本文](06_rendering_vulkan_shader.md#reading-note-022)。

#### [phase × sort_view_index の直積](../../src/core/renderer/polygoninstancecontainer.cpp#L1296) — variants を事前展開する目的

**分からなくなること**: なぜフレーム中に選ぶのではなく、全組み合わせを先に作るのか。

**答え**: phase と view ごとのソート結果を事前に展開し、live inventory を変更せず選択します。 → [第6章 §6.4 の本文](06_rendering_vulkan_shader.md#reading-note-023)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [timing の first / last](../../src/core/vkcore/renderer.cpp#L1703) | GPU timing のクエリスロット | 2 つ持つ理由 | 1 つのノードが複数 schedule position に現れうる(view ごと繰り返し)ため、スロットは初出時に 1 つだけ割り当て、first を区間開始・last を計測終了に使って**繰り返し全体を 1 区間として囲む** |
| [`resolveFrameResolutionExtents()`](../../src/core/vkcore/renderer.cpp#L868) | render_extent と output_extent | 2 つある理由 | `render_source_resource` と `output_source_resource` を別々に解決し、内部レンダー解像度と最終出力解像度が異なるアップスケール構成を表す。両者はコンパイル済み plan と一致検証される |
| [jitter の 2 値](../../src/core/vkcore/renderer.cpp#L930) | `view_projection_jittered` と `jitter_ndc` | なぜ両方要るのか | 前者はラスタライズ時にサブピクセルオフセットを掛ける行列、後者はシェーダが速度ベクトルや TAA リプロジェクションでそのオフセットを打ち消すための生の量。→ [第6章「ジッタは w 倍で足す」](06_rendering_vulkan_shader.md) |
| [`windowOutputFactsChanged()`](../../src/core/vkcore/renderer.cpp#L2894) | fingerprint と compile_facts | 二重比較の理由 | `compile_fingerprint` は安価な一致判定用ハッシュで先に比較し、加えて実体も比較して**ハッシュ衝突と fingerprint 未包含フィールドの差分**を取りこぼさない二段確認 |
| [companion 3 点セット](../../src/core/vkcore/renderer.cpp#L3433) | companion_participants / claims / apply_with_companions | 3 つの役割 | companion が「一緒に取り込む他参加者(shader / material)」を宣言し、claims が所有リクエストを判定し、apply_with_companions が 1 回で処理する。グラフ・シェーダ・マテリアル値の変更が**同一トランザクション**として一括成否になることを保証(→ §9.23.12) |
| [`view_index == 0` の `triggerUpdate`](../../src/core/vkcore/renderer.cpp#L6594) | GPU 状態の凍結点 | なぜ 1 回だけか | コメント "Object, skin, morph, and material-override GPU state is frozen after target acquisition and before the first view records" のとおり、**同一論理フレームの全 view が同じインスタンス / 描画キュー状態を見る**ことを保証するため |
| [`LogicalFrameAbortGuard`](../../src/core/vkcore/renderer.cpp#L3176) | 2 つの RAII ガード | 何が違うのか | 前者は例外離脱時に必ず `abortLogicalFrame()`。後者は `generation != 0` のときだけ runtime を保持し、`complete()` されずに壊れたら rollback する |
| [record / end の分離](../../src/core/vkcore/renderer.cpp#L2275) | `latest_scene_color/depth` の追跡 | 役割分離の理由 | **未執筆**(根拠がコードにない) |
| [sprite anchor の layer 計算](../../src/core/vkcore/renderer.cpp#L2409) / [snapshot copy の layer](../../src/core/vkcore/renderer.cpp#L2554) | sequential layer の選択 | いつ view_index が層になるか | sequential 実行で、かつ対象の `array_layers` が view 数以上(snapshot copy 側は source が layered)のときだけ `scheduled.view_index` を層として使い、それ以外は 0。ただし swapchain 出力の color だけは `view_index` をそのまま層として引く。逆順スキャンの理由は[第6章「sprite anchor の逆順スキャン」](06_rendering_vulkan_shader.md) |
| [`per_view_sort` から `draw_sort_view_index` へ](../../src/core/vkcore/renderer.cpp#L2781) | multiview / sequential の選択 | 対応関係 | → 上記「phase × sort_view_index の直積」と対 |
| [jitter provider / pattern / phases](../../src/core/vkcore/renderer.cpp#L3049) | ジッタ設定の解決 | 3 つの関係 | → [第6章「ジッタは w 倍で足す」「epoch ペアが reset 信号」](06_rendering_vulkan_shader.md) |
| [`watched_sources`](../../src/core/vkcore/renderer.cpp#L3403) | 監視対象の集合 | 複数参照を入れる根拠 | **未執筆**(§9.23.12 の claim モデルと対で書く必要がある) |
| [`shader_oetf_unorm_fallback`](../../src/core/vkcore/renderer.cpp#L1223) | 出力変換のトレース | 分岐と paired_storage_edges | **未執筆**(§9.23.14 の format スコアリングと対) |
| [`native_scope_has_work`](../../src/core/vkcore/renderer.cpp#L1372) | 空スコープの判定 | 判定基準 | **未執筆**(根拠がコードにない) |
| [staged 世代の遷移](../../src/core/vkcore/renderer.cpp#L158) と [`after_publish`](../../src/core/vkcore/renderer.cpp#L3848) | finalized / ready / runtime_published | 対称性 | 上記「`transaction->generation == 0`」と同じ世代機構。個々の遷移表は**未執筆** |
| [`graph_variant_policy`](../../src/core/vkcore/renderer.cpp#L5716) | variant と view family の方針 | RenderGraphVariant との対応 | **未執筆**(根拠がコードにない) |
| [`configureExternalDepthSubmission()`](../../src/core/vkcore/renderer.cpp#L6514) | XR への depth 提出 | true でも export が null になりうる | **未執筆**(§9.23.14 の OpenXR depth submission と対) |
| [family / view ごとの jitter と reset epoch](../../src/core/vkcore/renderer.cpp#L4163) | トレース出力 | なぜ family/view ごとに追うのか | **未執筆**(根拠がコードにない) |
| [RT 無しの空 diagnostics](../../src/core/vkcore/renderer.cpp#L4784) | `ray_query_acceleration_structures` | 空になる条件 | **未執筆**(根拠がコードにない) |
| [`sequential_count < count` の拒否](../../src/core/renderer/frameresources.cpp#L116) | スロット数の下限 | 何を保証するか | in-flight frame ごとに sequential_count 個のスロットを確保するので、**main family の全 view が自分専用の frame/resolution UBO スロットを持つ**(view 同士の UBO 別名衝突の防止) |
| [`updateSceneDescriptors()`](../../src/core/renderer/frameresources.cpp#L312) | 全 set の無条件更新 | 差分検知しない理由 | 呼び出し側に同値比較ガードがあり、buffer identity が変わったときと `configureViewCount` の再構築時にしか来ないので、関数内で差分を取る意味がなく、全スロット書き直しの方が漏れがない |
| [multiview のスロット分離](../../src/core/renderer/frameresources.hpp#L117) | seq / multiview の別配列 | last_data が vector になる理由 | **未執筆**(上記スロット確保と対で書く必要がある) |
| [RT SBT のリース](../../src/core/renderer/frameresources.cpp#L386) / [RT descriptor bundle の publication](../../src/core/renderer/frameresources.cpp#L403) | 世代の保持と二重 bind | 何を指しているのか | **未執筆**(根拠がコードにない) |
| [`SubmissionLeaseBundle`](../../src/core/vkcore/deletionqueue.hpp#L67) | 入れ子リースの upstream | 完了順序 | **未執筆**(根拠がコードにない) |
| [`declaration_ordinal`](../../src/core/renderer/polygoninstancecontainer.cpp#L860) | 単調増加の宣言順 | 単調性が要る理由 | `DrawQueueBuilder` 側の[一意性検査](../../src/core/renderer/drawqueuebuilder.cpp#L504)(重複したら拒否)を満たし、`stableOrderKey` の**最後の要素=同点時の決定的なタイブレーク(宣言順)**になる |
| [`prepareSceneReplacement()`](../../src/core/renderer/polygoninstancecontainer.cpp#L712) / [`drainSceneRetirements()`](../../src/core/renderer/polygoninstancecontainer.cpp#L890) | 次フレーム回収 | いつ実際に解放されるか | **未執筆**(manifest の `next-frame-retirement-queue` 語彙と対で書く必要がある) |
| [`canRebuildModelInstances()`](../../src/core/renderer/polygoninstancecontainer.cpp#L1402) | 再構築前の容量計算 | 何を数えているか | **未執筆**(根拠がコードにない) |
| [world_bounds / history / `resetTemporalHistory()`](../../src/core/renderer/polygoninstancecontainer.cpp#L1380) | 前フレーム状態 | lazy 更新の条件 | **未執筆**(TAA / モーションベクタ用。→ [第6章「epoch ペアが reset 信号」](06_rendering_vulkan_shader.md)) |
| [VRM transaction の `noexcept` rollback ラムダ](../../src/core/renderer/polygoninstancecontainer.cpp#L2348) | `noexcept` な巻き戻し | なぜ投げないのか | → §9.22「三段トランザクション」「`noexcept` なコミットテール」 |
| [alpha override の優先順位](../../src/core/renderer/polygoninstancecontainer.cpp#L1645) | 絶対 / 相対の合成 | どちらが勝つか | **未執筆**(§9.23.5 の per-row 表と対) |

### 9.23.5 RT(acceleration structure・SBT・oracle)

#### [3 つの identity の組み立て](../../src/core/renderer/accelerationstructure.cpp#L674) — scene / geometry / geometry_match

**分からなくなること**: 似た文字列を 3 本も作る理由と、どれがソートキーでどれがガードなのか。

**答え**: scene identity は publish 順に依存しない authored / content 情報から作ります。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-024)。

#### [`row.q != canonical_index` の拒否](../../src/core/renderer/accelerationstructure.cpp#L805) — q と record offset

**分からなくなること**: `q` が何番なのか、SBT のレコード番号とどう繋がるのか。

**答え**: q は正準 scene row の連番で、hit record の先頭は q × ray type 数です。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-025)。

#### [`index >= (1u << 24u)` の上限](../../src/core/shader/raygrouplayout.cpp#L1756) — 24 ビットの出所

**分からなくなること**: なぜ 2^24 なのか(エンジン都合か Vulkan 都合か)。

**答え**: SBT record offset と custom index は別フィールドですが、いずれも24ビットに制限されます。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-026)。

#### [`resolveRaySceneRows()`](../../src/core/shader/raygrouplayout.cpp#L1536) — 同じ geometry_match_identity に別 binding を許さない

**分からなくなること**: 「曖昧」と判定される条件が何を意味するのか。

**答え**: 同じ物理ヒットに異なる binding が付くと選択不能なので拒否します。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-027)。

#### [profile モードの material_index](../../src/core/shader/raygrouplayout.cpp#L2200) — なぜ `material_index == q` になるか

**分からなくなること**: 通常モードとの表の作り方の違い。

**答え**: profile の alpha override は行固有なので、material table も1行1エントリになります。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-028)。

#### [硬コードされた頂点オフセット](../../src/core/shader/raygrouplayout.cpp#L1208) — `10u` / `3u` / `12u` の出所

**分からなくなること**: 生成される GLSL に埋まる裸の定数がどこから来たのか。

**答え**: 頂点 offset の定数は CommonVertStruct の byte offset を word 単位へ換算した値です。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-029)。

#### [`BlasVariantKey`](../../src/core/renderer/accelerationstructure.cpp#L927) — 一意性と再利用判定

**分からなくなること**: BLAS のキャッシュキーに transform が入らない理由。

**答え**: BLAS 入力は geometry の範囲と flags で決まり、transform は TLAS が所有します。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-030)。

#### [`descriptorSetLayoutKeysFor()` の set 数決定](../../src/core/shader/pipelinefactory.cpp#L487) — reflection と ray_layout の階層

**分からなくなること**: set の枚数を誰が決め、誰が上書きするのか。

**答え**: reflection を基底として、ray table を使う場合だけ set 3 を engine の契約で上書きします。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-031)。

#### [shader group の stage index と SBT](../../src/core/shader/pipelinefactory.cpp#L707) — regions への対応

**分からなくなること**: stage 配列・group 配列・SBT バッファ・`traceRaysKHR` の 4 つの並びの対応。

**答え**: any-hit は独立 group ではなく hit group に入り、その group 順に SBT を配置します。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-032)。

#### [`RayTracingOracleContract`](../../src/core/shader/raytracingoracle.hpp#L14) — 定数群の根拠

**分からなくなること**: 5° や 1e-3 といった閾値が経験則なのか契約なのか。

**答え**: oracle の定数は fixture subset の一意性と GPU 誤差の契約を表します。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-033)。

#### [`intersect()`](../../src/core/shader/raytracingoracle.cpp#L226) — double と区間演算を並走させる

**分からなくなること**: 同じ式を 2 回計算しているように見える理由。

**答え**: double の代表値と区間演算の真値包含を並走させて、両方の保証を得ます。 → [第6章 §6.20 の本文](06_rendering_vulkan_shader.md#reading-note-034)。

#### [`parse()`](../../src/core/shader/rtspirvinspection.cpp#L615) — SPIR-V 命令走査

**分からなくなること**: 走査そのものの読み方。

**答え**: SPIR-V の命令走査はモジュール構造の前提知識です。 → [第10章 §10.3 の本文](10_background_knowledge.md#spir-v-のバイナリ形式)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`classifyRayQueryGeometry()`](../../src/core/renderer/accelerationstructure.cpp#L383) | AS 対象の分類 | skin/morph/VAT の変形出力 | skin/morph/VAT は毎フレームの変形頂点から BLAS を構築する。不適格な三角形範囲は除外診断へ出す |
| [`primitiveCount == 0` の TLAS](../../src/core/renderer/accelerationstructure.cpp#L235) | 空 TLAS の確保 | なぜ確保が要るのか | コメントどおり「Vulkan requires a valid, 16-byte-aligned instances address even when primitiveCount is zero」。非アクティブ 1 件分を必ず確保・書き込み、build 時の primitiveCount は 0 のままにして miss 専用の有効な TLAS にする |
| [三角形 branch の primitive_count](../../src/core/renderer/accelerationstructure.cpp#L1495) | primitive 数の計算 | 3 で割ってよい前提 | 三角形リスト前提(→ [第10章](10_background_knowledge.md)) |
| [`validateRayProfileAccelerationStructureFlags()`](../../src/core/renderer/accelerationstructure.cpp#L531) | eOpaque / eForceOpaque | profile 時の制約 | 両者は「片方でも立つと any-hit が省略され得る」点で等価な不透明化指定。profile 有効時は any-hit でのアルファ判定が必須なので、BLAS geometry でも TLAS instance 行でも**どちらのビットも立っていないこと**を要求し、立っていれば `pelican.ray_profile_opaque_as@1` |
| [`lowerRayTracingTlasInstancePlan()`](../../src/core/renderer/accelerationstructure.cpp#L757) | ray_scene_rows の有無で分岐 | source_index の役割 | source_index は元の instance 配列への戻り参照。分岐の全体像は**未執筆** |
| [`geometryName()`](../../src/core/renderer/accelerationstructure.cpp#L49) | asset/geometry/mesh/primitive/node | 5 階層の意味 | **未執筆**(§9.23.17 の glTF 階層と対) |
| [`RaySceneRow`](../../src/core/shader/raygrouplayout.hpp#L353) | uint32 max 初期化 | 何を表す値か | 「未解決」の sentinel。詳細は**未執筆** |
| [`MaterialRowDocumentIdentity`](../../src/core/shader/raygrouplayout.hpp#L319) | document / asset / unavailable | 3 状態の使い分け | → [第11章「`MaterialRowIdentity`: gate と lookup が同じ値を見る」](11_ray_query_acceleration_structures.md) |
| [`resolveRaySceneRows()` の宣言](../../src/core/shader/raygrouplayout.hpp#L569) | 引数と optional bindings | 何が任意なのか | **未執筆**(根拠がコードにない) |
| [layout digest から AS 宣言を外すコメント](../../src/core/shader/raygrouplayout.cpp#L880) | ダイジェストの分離 | なぜ AS だけ外すのか | コメントどおり **AS 宣言はシェーダ / SBT の ABI に影響しない**ので、AS だけを変えたときにシェーダ・SBT のダイジェストが動いて不要な再コンパイル / 再構築が起きないよう構造的に分離する(AS 側は独立ダイジェスト) |
| [`source_by_handle` / `source_by_key`](../../src/core/shader/raygrouplayout.cpp#L1845) | 2 名前空間の同時照合 | 両方見る理由 | 前者は(handle, generation, canonical_key)という**ランタイムのマテリアルリース同一性**、後者は**内容由来の正準キー同一性**で、別々に崩れうる(同一世代が違うデータを持つ / 別世代が同じキーに衝突する)。→ §9.22「identity / canonical key / lease の三層」 |
| [`texture_identity`](../../src/core/shader/raygrouplayout.cpp#L1908) | profile_active での切替 | 参照先が変わる理由 | profile の表は base color 全体ではなく**アルファ専用 IR** を載せるので、テクスチャ参照も MASK のときだけ `alpha_texture_identity` を使い、非 MASK は空にする |
| [`validateRayProfileGeometryAttributes()`](../../src/core/shader/raygrouplayout.cpp#L2246) | UV0 / COLOR_0 の必須化 | なぜ profile だけ必須か | profile の any-hit はアルファをテクスチャサンプルと頂点カラーから計算するので、texture_index が有効なら TEXCOORD_0 が、`uses_vertex_color` なら COLOR_0 が BLAS ジオメトリ側に実在しなければならない(欠ければ `pelican.ray_geometry_attribute_missing@1`) |
| [`alpha_override_mask`](../../src/core/shader/raygrouplayout.cpp#L2070) | override を行に持つ | material 側でない理由 | override は**マテリアルの改変ではなく「そのインスタンス配置におけるアルファの上書き」**で、同じマテリアルを共有する他の行に波及させてはいけないため、行に持って per-row 表に焼き込む |
| [`normalized_*` フィールド](../../src/core/shader/raygrouplayout.cpp#L2034) | 誰が計算したか | 出所 | §9.23.17 の `normalized_alpha_ir`(glTF の material lowering)。[検証側](../../src/core/shader/raygrouplayout.cpp#L1872)は WP368a の authored reference 由来であることを要求する |
| [material selector の 2 形態(document_material / asset_material)](../../src/core/shader/raygrouplayout.cpp#L665) / [`selectorMatches()`](../../src/core/shader/raygrouplayout.cpp#L476) | document 形式と asset 形式 | 比較方法が違う理由 | **未執筆**(§9.23.7 の canonical key と対) |
| [engine フィールドの必須化](../../src/core/shader/raygrouplayout.cpp#L969) | profile 有効時 | material_index と geometry_index の両方が要る理由 | **未執筆**(根拠がコードにない) |
| [`validName()`](../../src/core/shader/raygrouplayout.cpp#L133) | 名前の文字集合 | どこ由来の制約か | 外部ツール契約 R7(`docs/external_tools_requirements.md`)と同系統。→ §9.23.11 |
| [`ePartiallyBound` を binding 1 だけに](../../src/core/shader/pipelinefactory.cpp#L190) | set 3(ray material)の binding flags | なぜ binding 1 だけか | binding 1 だけが `rayBindlessTextureCapacity` 個の bindless サンプラ配列で、使うスロットはシーン依存でまばら。全スロットに有効な descriptor を書く義務を外すため必要で、単一 descriptor の binding 0/2 には不要 |
| [`ray_query_frame_set` 必須化](../../src/core/shader/pipelinefactory.cpp#L1221) | frame set の TLAS binding | 無いと作成拒否になる理由 | RT パイプラインは frame set の TLAS binding を必ず参照する契約なので、reflection に現れなければ `pelican.ray_tracing.frame_tlas_required@1`(descriptor が無ければシーンをトレースできない)。この検査を通した後、ray table 使用時は続けて[bindless device 要件](../../src/core/shader/pipelinefactory.cpp#L1231)も検証する |
| [production branch observer](../../src/core/shader/pipelinefactory.cpp#L1714) | SBT 書き込み直前の通知 | 本番に観測点がある理由 | コメントどおり「removing the actual GPU write or routing different bytes makes its A/B test fail」。→ §9.22「fault injection seam」 |
| [`discard_new_layouts` ラムダ](../../src/core/shader/pipelinefactory.cpp#L1931) | descriptor layout キャッシュの巻き戻し | all-or-nothing の作り方 | 開始時のキー集合を控え、失敗時に**今回追加された分だけ**消す。→ [第6章「reload の swap は 3 回」](06_rendering_vulkan_shader.md) |
| [`allocate_table`](../../src/core/shader/pipelinefactory.cpp#L1535) | minimum_size より大きく確保 | 余分に取る理由 | **未執筆**(根拠がコードにない) |
| [`active_owner_pipelines` の検索](../../src/core/shader/pipelinefactory.cpp#L1525) / [hit table の pack](../../src/core/shader/pipelinefactory.cpp#L1662) | 線形探索と row snapshot | 意図 | **未執筆**(根拠がコードにない) |
| [`RayTracingHitGroupOccurrence::any_hit`](../../src/core/shader/raytracingvalidation.hpp#L37) | any_hit を optional に | 省略できる理由 | §9.23.6 の any_hit 自動生成と対。**未執筆** |
| [blend attachment 分岐](../../src/core/shader/pipelinefactory.cpp#L292) / [hot reload の差し替え](../../src/core/shader/pipelinefactory.cpp#L2086) | 生成と再構築 | 分岐の基準 | **未執筆**(根拠がコードにない) |
| [`directUint32Constant()`](../../src/core/shader/rtspirvinspection.cpp#L293) | OpConstant / OpConstantNull のみ許可 | 動的値を拒む理由 | 配列長や trace 呼び出しの sbtRecordOffset / missIndex は**静的検証で確定値が要る**ため。実行時に決まる命令結果を許すと SBT / インタフェース契約を静的に検証できない(`trace_operand_dynamic@1` / `interface_type_unsupported@1`) |
| [`canonicalType()`](../../src/core/shader/rtspirvinspection.cpp#L337) | `SpvOpTypeRuntimeArray` の拒否 | なぜ失敗にするのか | 要素数がコンパイル時に決まらず、payload / hit attribute のサイズとレイアウトを確定できないため |
| [`ScalarLayout`](../../src/core/shader/rtspirvinspection.cpp#L78) | boolean を 4 バイト扱い | 根拠 | SPIR-V の `OpTypeBool` には**メモリ表現が規定されていない**(bit_width も持たない)ので、ホスト / GLSL の慣行に合わせて 4 バイトに固定する。他のスカラーは bit_width/8 で、8 の倍数でなければ「byte-addressable でない」として拒否 |
| [hit-group pair の payload 正規化](../../src/core/shader/raytracingvalidation.cpp#L584) | closest-hit と any-hit の型合わせ | 「physical hit-group pair」とは | 同じ hit group の 2 段が持つ incoming ray payload 型を、**どの `OpTraceRayKHR` もその ray slot に届いていない段階で**先に一意な正準型として揃える(エンジン生成 any-hit の payload 省略のみ例外) |
| [`isTriangleHitAttribute()`](../../src/core/shader/raytracingvalidation.cpp#L183) | `vec2<f32>` 固定 | なぜ 2 成分か | Vulkan の組み込み三角形交差が重心座標 2 成分の 32bit float を渡す仕様だから |
| [恒真述語の既定構築](../../src/core/shader/raytracingoracle.cpp#L267) | 「常に true」を式で組む | 定数 true にしない理由 | 「値とその補数のビット和が全ビット 1」という形を各入力(primitive_id / instance_custom_index / record_u32)について組み、論理積するので、**既定でも全入力オペランドと bitwise / 比較 / 論理ノード経路が実際に評価される**(定数畳み込みで経路が消えない) |
| [fixture の `require()` 群](../../src/core/shader/raytracingoracle.cpp#L486) / [入射角](../../src/core/shader/raytracingoracle.cpp#L556) | 数値境界の検査 | 個々の境界値の由来 | 契約側の定数(上記 `RayTracingOracleContract`)に対応。**個別の値の導出は未執筆** |

### 9.23.6 shader / computetask

#### [`slot_index = group_index * ray_count + ray_index`](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L656) — 線形化の向き

**分からなくなること**: 2 次元表をどちら major で潰しているのか、その並びに意味があるのか。

**答え**: hit group を major とする線形化により、SBT の record 配置へ対応させます。 → [第6章 §6.8 の本文](06_rendering_vulkan_shader.md#reading-note-036)。

#### [layer_count の自動展開](../../src/core/renderingpass/computetaskcontainer.cpp#L355) — 条件と目的

**分からなくなること**: 省略時と明示時で意味が変わるのはなぜか。

**答え**: array port の省略 layer 数だけを view 数へ展開し、明示指定は一致を要求します。 → [第6章 §6.8 の本文](06_rendering_vulkan_shader.md#reading-note-037)。

#### [`sequential_layer`](../../src/core/renderingpass/computetaskcontainer.cpp#L1129) と [`shares_storage_layout`](../../src/core/renderingpass/computetaskcontainer.cpp#L1166)

**分からなくなること**: `base_array_layer += view_index` が要る構成と、sampled なのに `eGeneral` になる構成。

**答え**: sequential view の層選択と同一 image の storage layout 共有は別の判定です。 → [第6章 §6.8 の本文](06_rendering_vulkan_shader.md#reading-note-038)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`material_alpha_v1` の any_hit 自動生成](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L681) | 生成の条件 | なぜ勝手に足すのか | profile が要求するアルファ判定を全 group×ray に行き渡らせるため。authored な anyhit と衝突したら検出する(→ §9.23.5 の profile 制約) |
| [`traced_ray_type_indices` の構築](../../src/core/renderingpass/computetaskdefinitionjson.cpp#L512) | 追加と重複チェック | 何の集合か | **未執筆**(根拠がコードにない) |
| [`physical_view_count` の決定](../../src/core/renderingpass/computetaskcontainer.cpp#L278) | 論理 view との関係 | 2 つの view 数の別 | **未執筆**(§9.23.4 の view family と対) |
| [sampled 判定](../../src/core/renderingpass/computetaskcontainer.cpp#L1101) | descriptor type の対応 | 対応表 | Vulkan の descriptor 種別の前提知識(→ [第10章](10_background_knowledge.md)) |
| [`next_extra_binding`](../../src/core/renderingpass/computetaskcontainer.cpp#L149) | 同一 resource に複数 port | binding の割当規則 | **未執筆**(根拠がコードにない) |
| [layered view の subresource range](../../src/core/renderingpass/computetaskcontainer.cpp#L1209) | mip の省略 | 既定値の規約 | **未執筆**(根拠がコードにない) |
| [`descriptor_variant_count`](../../src/core/renderingpass/computetaskcontainer.cpp#L1520) と [(view × frame) の事前生成](../../src/core/renderingpass/computetaskcontainer.cpp#L1272) | 2 次元の事前確保 | 2 軸の意味 | → §9.23.7「variant 次元と parity 次元」(同じ 2 軸) |

### 9.23.7 material

#### [`descriptor_abi_changed`](../../src/core/material/materialgenerationplanner.cpp#L1580) — 全 descriptor 破棄と revision

**分からなくなること**: なぜ `waitIdle()` してから捨てるのか、revision は誰が読むのか。

**答え**: descriptor ABI 変更は GPU 完了を待って旧 set を破棄し、revision を進めます。 → [第6章 §6.10 の本文](06_rendering_vulkan_shader.md#reading-note-039)。

#### [variant 次元と parity 次元](../../src/core/material/materialdescriptorbuilder.cpp#L441) — 2 軸が直交する

**分からなくなること**: 「variant」と「parity」がどちらも 2 値の添字に見えて区別できない。

**答え**: view layer の variant と履歴 buffer の parity は直交する2軸です。 → [第6章 §6.10 の本文](06_rendering_vulkan_shader.md#reading-note-040)。

#### [`rayMaterialTableSource()` の canonical key 検証](../../src/core/material/materialcontainer.cpp#L1810) — `document:` / `asset:` プレフィックス

**分からなくなること**: プレフィックス付き文字列が何の識別なのか。

**答え**: canonical key の prefix は document / asset / unavailable の identity 区分に対応します。 → [第6章 §6.10 の本文](06_rendering_vulkan_shader.md#reading-note-041)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`ray_generation` / `ray_lease` の対](../../src/core/material/materialcontainer.cpp#L816) | 世代とリース | 2 つで何を保証するか | `ray_generation` は登録ごとに単調増加する数値 ID(枯渇時は例外)で同一 material id の再登録を区別し、`ray_lease`(型は `shared_ptr<const void>`、実体は `make_shared<const uint64_t>(ray_generation)`)は**その登録実体がまだ生きていることをポインタ同一性で証明**する。不一致なら [`pelican.ray_material_snapshot_stale@1`](../../src/core/material/materialcontainer.cpp#L1808) |
| [`input_attachment_index` と same_pixel](../../src/core/material/materialcontainer.cpp#L2130) | LogicalReadFootprint の same_pixel | なぜ同一ピクセルだけか | dynamic rendering local read の条件そのもの(→ §9.23.4 / [第6章](06_rendering_vulkan_shader.md))。詳細は**未執筆** |
| [pass variant 間の ABI 一致検証](../../src/core/material/materialgenerationplanner.cpp#L966) | 同じ shader input の検証 | 何を比べているか | **未執筆**(根拠がコードにない) |

### 9.23.8 vertex / model

#### [`++geometry_address_generation`(index 側)](../../src/core/model/vertbufcontainer.cpp#L344) — 再確保で世代を上げる理由

**分からなくなること**: バッファを太らせただけなのに、なぜ世代を進めるのか。

**答え**: buffer 再確保で device address が変わるため、BLAS cache の世代を無効化します。 → [第3章 §3.8 の本文](03_project_and_loading.md#reading-note-042)。

#### [`ReleaseReport`](../../src/core/model/modelresourcerelease.hpp#L36) — 固定容量 8 + overflow_count

**分からなくなること**: なぜ `vector` ではないのか。

**答え**: noexcept の解放経路では確保せず、固定容量の報告と溢れ件数を使います。 → [第3章 §3.8 の本文](03_project_and_loading.md#reading-note-043)。

#### [parse → validate → stage](../../src/core/asset/model.cpp#L721) — 3 段階と `sameRigLayout`

**分からなくなること**: どの段で GPU に触るのか、どの差分が `compatibility_revision` を進めるのか。

**答え**: parse と validate は live に触れず、stage で初めて GPU と revision を確定します。 → [第3章 §3.8 の本文](03_project_and_loading.md#reading-note-044)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [allocation id の払い出し](../../src/core/model/vertbufcontainer.cpp#L461) | 単調 ID | 再利用してはいけない理由 | 解放されたレンジは後続モデルに**同じ offset で再割り当てされうる**ので、offset を identity にすると古い BLAS キャッシュにエイリアスする。単調 ID(枯渇時は例外)なら「解放済みレンジが後のモデルのキャッシュ済み BLAS にエイリアスできない」ことが保証される |
| [`releaseGeometryNow()`](../../src/core/model/vertbufcontainer.cpp#L542) | `noexcept` な解放 | 例外を握って report に積む理由 | デストラクタ / DeletionQueue から呼ばれるので投げれば terminate。段ごとに try/catch して report に積むことで、metadata クリア失敗がそのレンジを保持し続けたり、**後続 primitive の独立したレンジ返却を妨げたり**しないようにしている |
| [morph metadata と delta](../../src/core/model/vertbufcontainer.cpp#L250) | 2 つの別バッファ | 住所空間の違い | metadata は vertex プール上の `vertex_offset + vertex` で直接索引される固定長配列(static/skinned で別)、delta は free-range で `delta_offset` を確保する共有プール。不変式は `metadata[vertex].delta_offset = delta_offset + vertex` と `delta[target*vertex_count + vertex]`。→ ✅MV-1(`f84a7d14`)から metadata は固定長(262,144 件)ではなく、対応する vertex buffer と同じ件数で作り、[`ensureVertexCapacity()` が vertex buffer と一緒に倍にする](../../src/core/model/vertbufcontainer.cpp#L367)(古い記録は写し、増えた分は 0、古い buffer は `deferOldBuffer()`)。2 つのうち固定の上限を持つのは delta の側(`maxMorphDeltaRecords` = 524,288)だけ |
| [`releaseRange()` の merge](../../src/core/model/vertbufcontainer.cpp#L121) | free range の併合 | `std::max` を使う理由 | 判定が `>=` で「隣接」だけでなく**重なり / 内包**も併合対象になるため、後続レンジが直前の併合済みレンジに完全に含まれる場合に末尾を縮めないよう両終端の max を取る |
| [`allocateRange()`](../../src/core/model/vertbufcontainer.cpp#L103) | free list + high water mark | 2 系統ある理由 | まず free list を first-fit で走査して解放済みレンジを再利用し(**リロード周回でバッファが際限なく伸びるのを防ぐ**)、合う穴が無いときだけ high water mark を進め、そこで初めて `ensure*Capacity` の再確保が起きる |
| [`content_revision` / `compatibility_revision`](../../src/core/model/modeltemplate.hpp#L181) | 2 本の版 | 分ける理由 | content は毎リロード必ず進む「中身が変わった」印、compatibility はリグレイアウトが変わったときだけ進む「バインドを張り直す必要がある」印。後者だけが staged resource の互換性キーとして公開されるので、**内容だけの更新で依存側の再解決を誘発しない** |
| [importer mapping の安定座標](../../src/core/model/modeltemplate.hpp#L84) | mesh/primitive/node index | 「regrouping を生き延びる」の意味 | ソース GLB 上の**安定座標**で、GPU 側の allocation offset とは意図的に分離されている。再グループ化で offset が変わっても、それらの座標で addressing されたインポータ由来のマテリアル割当はそのまま解決できる |
| [`publishBatch()`](../../src/core/asset/model.cpp#L532) / [`retireReloadPayload()`](../../src/core/asset/model.cpp#L899) | 公開と退役 | 逆にできない理由 | WP382c では renderer の rebuild を prepare し、participant の publish で template と instance を新世代へ張り替える。旧 payload は coordinator の退役経路から GPU resource の遅延解放へ渡す。旧 primitive レンジを公開前に解放すると、生きた draw の参照を壊しうる |
| [`DeferredRelease` の `acquire()`](../../src/core/model/vertbufcontainer.cpp#L604) | lease semantics | null のとき何が起きたか | §9.23.4 の `GpuSubmissionLease` と同じ(フェンス完了まで生かす券)。lease が null なら既に回収済み |
| [`deferOldBuffer()`](../../src/core/model/vertbufcontainer.cpp#L143) | 旧バッファの遅延破棄 | いつ drain されるか | DeletionQueue(submission fence 完了時)。→ §9.12 |
| [例外時の morph range](../../src/core/model/vertbufcontainer.cpp#L406) | 部分 cleanup | 解放されない範囲がある | **未執筆**(`vertex_allocated` フラグの cleanup 範囲を確かめる必要がある) |
| [morph metadata の空 vector 埋め](../../src/core/model/vertbufcontainer.cpp#L578) | ゼロクリア | なぜ空 vector か | **未執筆**(根拠がコードにない) |
| [`deferred_callbacks` の初期化位置](../../src/core/model/vertbufcontainer.cpp#L77) | メンバ初期化順 | なぜその位置か | **未執筆**(根拠がコードにない) |
| [`Impl` の所有関係](../../src/core/asset/model.cpp#L414) | PayloadTracker / CommitBatch / ModelPayload / PendingCandidate | 誰が誰を持つか | **未執筆**(3 段階 staging の実装詳細) |
| [`const void*` での payload 追跡](../../src/core/asset/model.cpp#L710) | 型を消した比較 | なぜ型を消すのか | **未執筆**(根拠がコードにない) |
| [`source_material_index` が optional](../../src/core/model/modeltemplate.hpp#L148) / [bounds](../../src/core/model/modeltemplate.hpp#L71) | 任意性と空間 | bounds は local か world か | **未執筆**。ただし「スキニングは後で現在のパレットから適用」とコメントされているので local space と読める |
| [material bindings のパス解決](../../src/core/asset/model.cpp#L165) | reference からの解決 | fragment を返しうるか | **未執筆**(根拠がコードにない) |

### 9.23.9 editor journal

#### [OriginTransitionMap の journal 側](../../src/core/communication/editorjournal.cpp#L1571) — forward / inverse の向き

**分からなくなること**: 同じ表を「展開」と「復元」の両方で使うとき、方向がどこで反転するのか。

**答え**: unpack と restore は同じ entries の direction を反転し、journal が方向を検証します。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-045)。

#### [`attachment_handle` / `attachment_seq` / witness](../../src/core/communication/editorjournalops.cpp#L351) — 3 識別子の主キー

**分からなくなること**: どれが主キーで、どれが照合用なのか。

**答え**: attachment handle、宣言位置の sequence、内容照合の witness は別の識別子です。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-046)。

#### [`sceneBehaviorAttachmentSeq()`](../../src/core/gamelogic/behaviorarena.cpp#L74) — 存在する関数

**分からなくなること**: journal 側から呼ばれているこの名前が、どこで定義されているのか。

**答え**: attachment sequence は宣言位置の2つの u32 を1つの u64 に詰めます。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-047)。

#### [`structuralDomainsOverlap()`](../../src/core/communication/editorjournal.cpp#L2868) — kind が競合検出でどう使われるか

**分からなくなること**: `kind` 文字列が判定にどう効くのか。

**答え**: structural domain は object・名前・宣言位置と、同 slot の異種操作の交差を判定します。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-048)。

#### [パス重複と domain 重複](../../src/core/communication/editorjournal.cpp#L2797) — 両方が要る理由

**分からなくなること**: JSON パスが重ならなければ安全ではないのか。

**答え**: path の祖先関係だけでは意味領域の衝突を拾えないため、domain overlap と OR で結びます。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-049)。

#### [`last_writers`](../../src/core/communication/editorjournal.cpp#L3433) — 「active writer history」の意味

**分からなくなること**: 履歴という名前なのに、undo で要素が消える理由。

**答え**: last writers は操作履歴ではなく現在有効な書き手のスタックです。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-050)。

#### [`final_authored_index` の連続性](../../src/core/communication/editorjournalintegrity.cpp#L211) — `max()` はスキップではない

**分からなくなること**: `numeric_limits<size_t>::max()` の明示チェックが「その値をスキップしてよい」と読めてしまう。

**答え**: authored index は連続であり、max の検査は加算の wraparound を防ぎます。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-051)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [gate epoch と transition_epoch](../../src/core/communication/editorjournal.cpp#L3519) | 2 つのエポック | 違い | `transition_epoch` は**ゲート提供側(runtime の `EditorTransitionCounter`)が数える外部の状態遷移カウンタ**、[`gate_tracker`](../../src/core/communication/editorjournal.cpp#L3440)(`EditorGateEpoch`)の epoch はその観測が変わるたびに進む**内部の単調エポック**。受理時に epoch を控え、[commit 時に食い違えば](../../src/core/communication/editorjournal.cpp#L4224)拒否する。transition の変化は必ず epoch を進めるので、比べるのは epoch だけでよい |
| [`requireRevertPreconditions()`](../../src/core/communication/editorjournal.cpp#L3662) | undo 前の 3 段検証 | 何を見ているか | → [第7章「undo前提の三段検証」](07_tools_rpc_tests.md) |
| [`Impl::preview` の reservation と lease](../../src/core/communication/editorjournal.cpp#L3411) | 予約と実効リース | 2 つある理由 | reservation は `open_preview` を受理した瞬間(まだ frame boundary 前)の**先行予約**、lease はプレビュー適用後の**実効リース**。[`requireNoLeaseOverlap()`](../../src/core/communication/editorjournal.cpp#L3641) が両者と新規編集の重複を見て `preview_lease_conflict`。状態機械は[第7章「preview leaseの状態機械」](07_tools_rpc_tests.md) |
| [unpack の単独 op 強制](../../src/core/communication/editorjournal.cpp#L2152) | batch の形の制限 | なぜ 1 command か | unpack は object subtree 全体を作り替え、その inverse を `origin_transition` 込みの forward/inverse ペアとして**受理時点で確定**させる必要があるため。他 op と混ぜるとこの単一ペアの整合検査が成立しない |
| [`prefabGeneratedId()`](../../src/project/prefab.cpp#L1060) | gid の生成 | アルゴリズム | ドメイン分離タグ `"pelican.prefab.gid.v1"` / `instance_id` / `component_key` を**長さ前置で連結**したバイト列の SHA-256 の先頭 16 バイトを hex 化し `gid_` を前置する([宣言側コメント](../../src/project/prefab.hpp#L224)= WP361 v4)。component_key は `component.id`、無ければ `component.name` |
| [`canonicalBehaviorComponent()`](../../src/core/communication/editorjournal.cpp#L271) | behavior だけ codec を通さない | 理由 | behavior attachment は登録済み codec を持つ通常 component ではなく `{name,type,params}` の固定形で、**型登録が DLL 由来で欠けている可能性がある**。未登録でも情報を落とさず保持し、「レジストリが生きている(名前拒否)」と「DLL 不在(pending attachment)」をコミット境界で区別するため |
| [子孫の降順ソート](../../src/core/communication/editorjournalops.cpp#L1134) | prepareDestroy の並べ替え | 降順の理由 | 子孫から順に remove を積む必要があり、降順で削除すれば**削除に伴う後続要素のインデックスずれが未処理分に波及しない**。同時に inverse(restore_objects)が逆順で元の宣言位置に挿し戻せる |
| [`field_path` の `/name` 禁止](../../src/core/communication/editorjournalops.cpp#L659) | 値編集の対象制限 | なぜ name だけ禁止か | `name` は **component のスロット同一性(と object 名前予約)を決める識別子**なので、値編集経路で書き換えられると slot / domain 計算が破綻する。detail も "field_path must be a JSON pointer and cannot change name" と明言 |
| [`prefabGeneratedMarkerRule`](../../src/core/communication/editorjournal.cpp#L1526) | マーカー削除と body 検証 | 何を消すのか | 上記 gid / OriginTransitionMap と一体。詳細は**未執筆** |
| [unpack の forward/inverse 整合](../../src/core/communication/editorjournal.cpp#L1596) | commit 時の structural_changes | 何を突き合わせるか | **未執筆**(根拠がコードにない) |
| [`behaviorAttachmentWitness()`](../../src/core/communication/editorjournal.cpp#L575) | witness の組み立て | generated と authored の分岐 | witness 自体は常に 4 フィールド(`authoring_object_id` / `authored_component_occurrence` / `type` / `origin`)。分岐するのは [`origin` の中身](../../src/core/communication/editorjournal.cpp#L549)で、authored は 2 フィールド(`kind` / `authoring_component_index`)、generated は 6 フィールド(`kind` / `stable_generated_id` / `prefab` / `instance_id` / `component_key` / `digest_sha256`) |
| [`expanded_components`](../../src/core/communication/editorjournalintegrity.cpp#L23) と [末尾からの削除](../../src/core/communication/editorjournal.cpp#L1460) | 展開区間の位置 | 逆操作が末尾削除でよい理由 | unpack は authored 配列の**末尾に展開区間を置く**ので、逆操作は末尾から authority.size() 分を削るだけでよい(→ final_authored_index の連続性) |
| [behavior edit の attachment_index](../../src/core/communication/editorjournalops.cpp#L72) | 必須引数 | なぜ必須か | 同一 object に複数の behavior が付くので index なしでは対象が決まらない(→ §9.23.2 の「behavior だけ重複可」) |
| [`closureJson()`](../../src/core/communication/editorjournal.cpp#L389) | previous/next による linked-list | 宣言順の復元 | → [第3章「closureがidentityを運ぶ」](03_project_and_loading.md) |
| [`resolvePrefabInstance` の context 引数](../../src/core/communication/editorjournal.cpp#L1249) | scene_id / parameter / json_pointer | 何を運ぶか | **未執筆**(根拠がコードにない) |
| [replay 時の witness 再検証](../../src/core/communication/editorjournal.cpp#L1923) | rebind | undo/redo で張り替わる理由 | **未執筆**(根拠がコードにない) |
| [`projection_state->revision() == source.revision()`](../../src/core/communication/editorjournal.cpp#L2164) | incremental update | 判定の意味 | **未執筆**(根拠がコードにない) |
| [`ReparentPreserve`](../../src/core/communication/editorjournalops.cpp#L1189) | Local / World | transform 計算への影響 | → [第3章「preserve-worldの逆算」](03_project_and_loading.md) |
| [behavior canonical 化の `old_component` 保持](../../src/core/communication/editorjournalops.cpp#L276) | retain する理由 | 何のために残すか | コメントどおり「`BehaviorParamsPolicy` の既定値は編集可能な authored view の一部なので、field pointer を解決する前に一時オブジェクトへ展開する。その間 `old_component` は inverse / raw 復元のために**逐語のまま**保持する」 |
| [`domainObjects` / `domainNames` / `domainIndices` の定義](../../src/core/communication/editorjournal.cpp#L2806) ほか(prefab_cas フラグ、generated_id と component_slot の排他、`operationsUsePrefabParameters`、`forceAbort` と preview_boundary、`normalizeBehaviorAttachmentIdentities`、`batchWriteSet` の集約、`coalesce_key`、`finalizeRevertJournal` の逆順再構築) | 競合判定の下請けと journal の細部 | 個々の規則 | **未執筆**(いずれも根拠がコードにない。エラーコードの `@1` は §9.23.18 の契約バージョン) |

### 9.23.10 editor command service

#### [5 つの PrefabError を dependency_failure に分類](../../src/core/communication/editorcommandservice.cpp#L1081) — 分類の基準

**分からなくなること**: 同じ `PrefabError` がどうして 2 つの外部エラーに分かれるのか。

**答え**: 受け側 project の入力不足を dependency failure とし、payload の意味違反とは分けます。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-052)。

#### [`trackPreviewTransition()`](../../src/core/communication/editorcommandservice.cpp#L1743) — PreviewWatch の erase 条件

**分からなくなること**: watch がいつ消えるのか(成功時なのか失敗時なのか)。

**答え**: accepted の transition だけを pending とし、terminal 応答や lease 不在で破棄します。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-053)。

#### [generated component の出力](../../src/core/communication/editorcommandservice.cpp#L728) — editable 判定

**分からなくなること**: 同じ component なのに、返る JSON の形が違う理由。

**答え**: generated component は編集元を持たず、behavior の編集可否には登録と canonical params が要ります。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-054)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`parsePrefabClosure()`](../../src/core/communication/editorcommandservice.cpp#L298) | closure を strict sorted 順に強制 | 順序を縛る理由 | closure は fingerprint / digest と突き合わせる**正準表現**なので、順序の自由度を残すと同一内容が別バイト列になり closure 一致判定が非決定的になる |
| [版を最初に断る行](../../src/core/communication/editorcommandservice.cpp#L467) / [gate 3](../../src/core/communication/editorcommandservice.cpp#L485) / [SNAPSHOT0 のコメント](../../src/core/communication/editorcommandservice.cpp#L512) | 5 ゲートの評価順 | 順序そのものが契約である理由 | **SNAPSHOT0 が定めた 5 ゲートの評価順(version → size → digest → parse/semantic → closure)がワイヤ契約**で、どの入力不正がどのコードで返るかの優先順位を版間で固定する。「digest の algorithm 検査は gate 3 であってリクエスト形状検査ではない」ため前倒しできない。MG-2b で SnapshotV1 の parser が消え、`schema_version` が 2 以外の要求は他の鍵を見る前に [版を断る関数 `rejectSnapshotVersion`](../../src/core/communication/editorcommandservice.cpp#L246) で `unsupported_snapshot_version` になる |
| [`parseGetComponentsRequest()`](../../src/core/communication/editorcommandservice.cpp#L204) | id と name の排他 | なぜちょうど一方か | 宣言 id と名前は**独立な参照系**で、両方指定は互いに矛盾しうる(片方が別オブジェクトを指す)、両方省略は対象未定。`has_value() == has_value()` で強制 |
| [`SceneRevision{1}` のダミー](../../src/core/communication/editorcommandservice.cpp#L1075) | import 検証用の使い捨て文書 | なぜ固定値でよいか | ここで作る `validated` は**ゲート 4 専用で公開されない**ため。実際に公開される revision は `import_scene_snapshot(...)` が返す `imported.revision` |
| [`ResolvedSceneResolver::resolve()` の 5 引数](../../src/core/communication/editorcommandservice.cpp#L1077) | defaults / registry / provider を別に渡す | 分ける理由 | **未執筆**(§9.23.2 の resolver 入力 fingerprint と対) |
| [runtime state と authored state の融合](../../src/core/communication/editorcommandservice.cpp#L686) | 2 つの状態の突き合わせ | size mismatch が `logic_error` な理由 | **未執筆**(runtime と authored の component 数は一致するのが不変式、という以上の根拠がコードにない) |
| [`closureMismatchDetail()`](../../src/core/communication/editorcommandservice.cpp#L404) | 最初のズレで停止 | なぜ全部見ないか | **未執筆**(エラー detail の決定性を狙ったものと読めるが、根拠となる記述がない) |
| [`render-config-edit-` プレフィックスの判定](../../src/core/communication/editorcommandservice.cpp#L1670) | ticket ルーティング | render config編集の共通ticketを既存結果照会へ送る | featureと名前付きASの編集が共用 |
| [revision token が `edit_` の preview_epoch を吸収する行](../../src/core/communication/editorcommandservice.cpp#L856) | `edit_` の未確定分 | なぜ吸収するのか | **未執筆**(根拠がコードにない) |
| [error detail のパス手組み(`root + "/scenes/" + index`)](../../src/core/communication/editorcommandservice.cpp#L323) | index 入り JSON pointer 風 | 手で組む理由 | **未執筆**(根拠がコードにない) |
| [`findScene()` の直後](../../src/core/communication/editorcommandservice.cpp#L1043) | scene 存在検査 | `selectScene()` も呼ぶのか | **呼びません**。ここは `findScene(current_scene) == nullptr` で `SceneNotFound` を投げるだけの存在検査で、選択の副作用はありません |
| [import reload 後の再 catch](../../src/core/communication/editorcommandservice.cpp#L1080) | `PrefabError` の再捕捉 | 二重に捕まえる理由 | **未執筆**(根拠がコードにない) |
| [`ordered_json` の扱い](../../src/core/communication/editorcommandservice.cpp#L629) | キー順序の保存 | いつ順序が意味を持つか | JSON ライブラリの前提知識(→ [第10章](10_background_knowledge.md))。使い分けの実例は §9.23.11 の `resolution_error` 行 |

### 9.23.11 RPC

#### [`gameLogicParticipantRpcStatus()` による participant 照会](../../src/core/communication/rpcserver.cpp#L1291) — 参加者名の正

**分からなくなること**: 参加者名の文字列が何なのか(`"game_logic_reload"` に見える資料がある)。

**答え**: game logic の participant 名は ReloadService の定数が正です。 → [第2章 §2.11 の本文](02_runtime_lifecycle.md#reading-note-055)。

#### [`SceneLoadResult` の受け取り](../../src/core/communication/rpcserver.cpp#L2056) — 状態機械

**分からなくなること**: 「失敗したのに committed:true」が起きる理由。

**答え**: pre-commit failure だけを例外に戻し、commit 後の effect failure は公開成功として返します。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-056)。

#### [`parseGizmoSelection()`](../../src/core/communication/rpcserver.cpp#L521) — 2 つの identity を受ける理由

**分からなくなること**: なぜギズモの選択だけ 2 形式あるのか。

**答え**: gizmo は永続する宣言 identity と runtime の世代付き identity を受け、混在を拒否します。 → [第7章 §7.7 の本文](07_tools_rpc_tests.md#reading-note-057)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [リロード中の一律拒否](../../src/core/communication/rpcserver.cpp#L1567) | `isGameLogicReloadInProgress()` | なぜ全メソッドか | DLL のシンボル・system 登録・behavior 実体が**差し替え途中**で、この状態でハンドラを走らせると無効化されたコードやハンドルを触るため。拒否は transport の中ではなく、[`configureEngineRpcHandlers()`](../../src/core/communication/rpcserver.cpp#L1555) が [`RpcServer::setRequestGate()`](../../src/core/communication/rpcserver.cpp#L1160) で差し込む述語になり、`RpcServer` は [parse 後・dispatch 前に gate を 1 回引く](../../src/core/communication/rpcserver.cpp#L1175)だけです |
| [before_apply / after_apply フックの登録](../../src/core/communication/rpcserver.cpp#L1575) | リロードの前後フック | 2 つで何を挟むか | `before_apply` でプレビューリースを強制中断して未確定編集をコミット済み状態に戻し、`after_apply` で `pending_transforms.clear()` により**旧 DLL 世代のエンティティを指す未フラッシュ transform を破棄**する |
| [`"resolution_error"` の再帰探索](../../src/core/communication/editorrpchandlers.cpp#L50) | 結果 JSON の走査 | 何を見ているか | どこかに `"resolution_error"` キーがあるかだけを見る。**含まれる場合はキー順序が意味を持つ**ため `nlohmann::json` への変換(順序喪失)を避け、`ordered_json` のまま手組みで dump する分岐に入る。走査は `RpcServer` から editor 側へ移り、`embedsResolutionError` は [`setOrderedHandler()` に渡す順序ポリシー](../../src/core/communication/editorrpchandlers.cpp#L88)として登録されます。transport は [ポリシーが真を返した応答だけ](../../src/core/communication/rpcserver.cpp#L1192)を ordered dump に回し、キー名は知りません |
| [`isR7Identifier()`](../../src/core/communication/rpcserver.cpp#L259) | "R7" の由来 | R7 とは何か | **外部ツール契約の要求番号 R7**(`docs/external_tools_requirements.md`)「オブジェクト / ジョイント名は `[a-zA-Z0-9_]` のみ」の実装。`docs/design_scene_format.md` と `docs/manual/04_scene_ecs.md` も同じ R7 を参照 |
| [`RpcServer::run()` の CRLF 除去](../../src/core/communication/rpcserver.cpp#L1233) | framing 契約 | なぜ `\r` を落とすか | 「1 行 = 1 JSON-RPC メッセージ、区切りは `\n`」で、CRLF 出力の相手から来た行末 `\r` を落とすことで Windows クライアントでも同じ契約が成立する。応答は常に `'\n'` 終端 + flush |
| [逆引き](../../src/core/communication/rpcserver.cpp#L418) / [名前からの変換](../../src/core/communication/rpcserver.cpp#L750) | model_instance → object_id → authoring_object_id | 橋渡しの規則 | 上記 2 つの identity の橋渡し。詳細は**未執筆** |
| [`ResolutionFailureException`](../../src/core/loader/resolutionresult.hpp#L177) | `failure()` の中身 | 何を表すか | **未執筆**(§9.23.2 の resolver 失敗形式と対) |
| [`kbd:` プレフィックス](../../src/core/communication/rpcserver.cpp#L641) | binding 表示 | modal transform との関係 | **未執筆**(根拠がコードにない) |
| [OpenXR inactive スキーマ](../../src/core/communication/rpcserver.cpp#L107) / [診断](../../src/core/communication/rpcserver.cpp#L1691) | 診断 JSON | スキーマの安定性契約 | **未執筆**(→ §9.23.18) |
| [frame index と continuous time](../../src/core/communication/rpcserver.cpp#L928) / [replay 開始](../../src/core/communication/rpcserver.cpp#L1986) | どちらが真か、設定順序 | 真の時刻源 | **未執筆**(根拠がコードにない) |

### 9.23.12 reload / watch

#### [`ReloadService::registerParticipant()`](../../src/core/watch/reloadservice.cpp#L151) — 型が言えないことだけを検査する

**分からなくなること**: 「両方あると不正」という検査がどこへ行ったのか。

**答え**: apply route の排他は variant で表し、登録時は型で表せない条件と companion 順序を検査します。 → [第6章 §6.10 の本文](06_rendering_vulkan_shader.md#reading-note-058)。

#### [`applyRenderPipelineCompanionReload()`](../../src/core/watch/reloadservice.cpp#L448) — 流れと早期 return

**分からなくなること**: 何もせずに戻る条件が読み取りづらい。

**答え**: shader と material の候補を1つの coordinated commit に束ね、実変更がないときだけ早期 return します。 → [第6章 §6.10 の本文](06_rendering_vulkan_shader.md#reading-note-059)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`applyRuntimeBoundary()`](../../src/core/watch/reloadservice.cpp#L277) | frame_start と render_start | 境界の使い分け | 境界ごとに参加者を絞って実行する二段構え。**ゲームロジック DLL のような論理状態の入れ替えは frame_start、シェーダ / パイプライン再構築は描画直前の render_start** |
| [`makeLogicalAssetId()`](../../src/core/watch/reloadtransaction.cpp#L19) | generation を上位 32bit・index を下位 32bit に詰めた 64bit | stale 検出の仕組み | [`liveRecord()`](../../src/core/watch/reloadtransaction.cpp#L77) は index でスロットを引いた後 generation 不一致なら stale。`destroyResource` がスロットを free list へ返し再利用時に `++generation` するので、古いハンドルは同じ index を指しても必ず検出される(飽和時は例外)。→ §9.22 |
| [`eraseEdges()`](../../src/core/watch/reloadtransaction.cpp#L90) と逆引き map | `AssetKey -> set<LogicalResourceRef>` | 何に使うのか | 依存元アセットから「それに依存している論理リソース」を引く。**ファイル変更時に再ロード対象を特定するため**で、publish/destroy のたびに同期し、空集合はエントリごと削除する |
| [`topologicalOrder()`](../../src/core/watch/reloadtransaction.cpp#L105) | DAG の必須性 | 循環したらどうなるか | 1 グループ内の actor は `after` で「先にコミットすべき対象」を宣言し、Kahn 法で処理順を決める。循環があると順序が決まらず `"reload transaction dependency cycle"`(重複ターゲットも先に弾く) |
| [`ResourceRegistry::publish()`](../../src/core/watch/reloadtransaction.cpp#L247) | revision と retire | 破棄のタイミング | content は publish のたびに +1、compatibility は互換性 / レイアウト / リグの変更を表し、stale handle 判定に使う slot generation とは**意図的に独立**。retire は旧 payload の `shared_ptr` を集めて返し、呼び出し側が公開後に受け取って GPU の DeletionQueue へ遅延破棄する(→ §9.23.8 の rebuild → release と同じ順序原則) |
| [thread_local な companion observer](../../src/core/watch/reloadservice.cpp#L25) | 入れ子と並行の安全化 | なぜ thread_local か | **未執筆**(コメントが無く、「ctor で退避・dtor で復元」以上の根拠がコードにない) |
| [built-in participant の lazy 登録](../../src/core/watch/reloadservice.cpp#L669) | 4 つの既定参加者 | ポインタ比較で更新判定する理由 | **未執筆**(根拠がコードにない) |
| [`applyRequests()`](../../src/core/watch/reloadservice.cpp#L1232) | claimants / owned / companion / consumed | 4 つの集合の役割 | **未執筆**(根拠がコードにない) |
| [`applyShaderReloadBatch()`](../../src/core/watch/reloadservice.cpp#L373) | prepared 空での早期 error | cache_hits/misses の意味 | **未執筆**(根拠がコードにない) |
| [authored candidate の completion callback](../../src/core/watch/reloadservice.cpp#L310) | 完了通知 | 誰が呼ぶか | **未執筆**(根拠がコードにない) |
| [`applyRequest()`](../../src/core/watch/reloadservice.cpp#L1040) | enqueue false と failed count | 同期の機構 | **未執筆**(根拠がコードにない) |
| [`runtimeStatusJson()`](../../src/core/watch/reloadservice.cpp#L1442) | describe callback | 誰が読むか | §9.23.11 の participant 照会が読む先。詳細は**未執筆** |

### 9.23.13 vkcore

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`createLogicalDevice()` の StructureChain](../../src/core/vkcore/core.cpp#L549) と [`unlink<T>()` の呼び出し](../../src/core/vkcore/core.cpp#L717) | feature 構造体の動的削除 | 削除の判定基準 | 全 feature 構造体を一度チェーンに組み、実行時に**「その機能が物理デバイスでサポートされ、かつ有効化選択されたか」**で使わないものだけ `unlink<T>()` する。未サポート機能の構造体を渡して device 作成が失敗するのを避けるため |
| [`pickQueues()`](../../src/core/vkcore/core.cpp#L228) | フォールバック優先順位 | 順序の理由 | 非 headless では ①graphics+present+compute を満たす単一 family、②graphics+present、③present だけ、の順。**1 family で済めばキュー跨ぎの所有権移転やセマフォ同期が不要**なので最優先 |
| [`selectVulkanPhysicalDevice()` の score](../../src/core/vkcore/devicefeaturepolicy.cpp#L26) | スコアの加点 | 1000 / 100 という値の根拠 | ✅DS-1 から `pickPhysicalDevice()` は候補を集めるだけで、選ぶのはこの関数。discrete GPU に +1000、headless か `graphic_queue == presentation_queue`(`unified_queue`)に +100 で、外付けの GPU が統合 GPU より先に選ばれる。同一 family なら swapchain image のキューファミリ所有権移転が要らず有利だから +100。Vulkan 1.3 未満と必須の拡張・機能の欠けは score ではなく `continue` で除外し、`--gpu <index|name>` はそれ以外の候補を外す(要件を欠く GPU は指定しても選ばない)。**数値そのものの根拠は未執筆** |
| [`VulkanManageCore()` の関数ポインタ初期化](../../src/core/vkcore/core.cpp#L931) | 拡張関数の条件付き解決 | null チェック後に throw する理由 | Vulkan 拡張の関数ポインタ解決という前提知識(→ [第10章](10_background_knowledge.md)) |

### 9.23.14 swapchain / OpenXR

#### [`replacement_anchor` の保存](../../src/core/vkcore/swapchainframetarget.cpp#L1146) — 成功時に退避する理由

**分からなくなること**: 作成に成功したのに、なぜ古いハンドルを持ち続けるのか。

**答え**: swapchain 作成成功後は旧 swapchain が退役するので、新 handle を先に retry anchor として保持します。 → [第6章 §6.6 の本文](06_rendering_vulkan_shader.md#reading-note-060)。

#### [`releaseAbandonedImage()`](../../src/core/vkcore/swapchainframetarget.cpp#L1293) — `vkReleaseSwapchainImagesEXT` をいつ呼ぶか

**分からなくなること**: 呼ぶ条件と対象がいずれも狭いこと。

**答え**: maintenance1 が有効で放棄済み acquire image がある場合だけ、cutover 前にその image を解放します。 → [第6章 §6.6 の本文](06_rendering_vulkan_shader.md#reading-note-061)。

#### [present 完了追跡の 2 系統](../../src/core/vkcore/swapchainframetarget.cpp#L556) — 証明と推論

**分からなくなること**: maintenance1 の有無で「完了した」の意味が変わること。

**答え**: present 完了は maintenance1 の fence で証明し、無い場合は後続 epoch の reacquire から推論します。 → [第6章 §6.6 の本文](06_rendering_vulkan_shader.md#reading-note-062)。

#### [`surfaceLossRequiresDeviceRebuild()`](../../src/core/vkcore/swapchainframetarget.cpp#L1540) — "unproven presentation"

**分からなくなること**: surface lost で device ごと作り直す条件。

**答え**: surface loss 後に present 完了を証明できない epoch が残ると device 再構築を要求します。 → [第6章 §6.6 の本文](06_rendering_vulkan_shader.md#reading-note-063)。

#### [2 種類の image view](../../src/core/openxr/openxrcompositiongraphics.cpp#L115) — 2D Array と層ごとの 2D

**分からなくなること**: 同じ image に 2 通りの view を作る理由。

**答え**: 一括 multiview と片目ごとの sequential 描画に対応するため、array view と layer view を持ちます。 → [第6章 §6.15 の本文](06_rendering_vulkan_shader.md#reading-note-064)。

#### [`Window::process()`](../../src/core/os/window.cpp#L374) — close flag が立たないまま窓が消える

**分からなくなること**: close を待つだけの main loop に、なぜ窓の存在確認があるのか。

**答え**: Studio の親窓破棄で close flag が立たない場合も、native window の実在を確認して終了します。 → [第2章 §2.4 の本文](02_runtime_lifecycle.md#reading-note-065)。

#### [depth だけ `eTransferDstOptimal` へ遷移させる barrier](../../src/core/openxr/openxrcompositiongraphics.cpp#L209) — color と違う遷移先

**分からなくなること**: depth attachment なのに transfer レイアウトへ遷移する理由。

**答え**: XR の depth は engine 側からコピーするため、transfer destination layout へ遷移します。 → [第6章 §6.15 の本文](06_rendering_vulkan_shader.md#reading-note-066)。

#### [depth extension の 4 値を書く行](../../src/core/openxr/openxrcompositiontarget.cpp#L915) — minDepth/maxDepth と nearZ/farZ

**分からなくなること**: 深度の範囲を表す値が 2 組あること。

**答え**: 深度値の値域と実際の view 空間距離を4値で結び、reprojection に使います。 → [第6章 §6.15 の本文](06_rendering_vulkan_shader.md#reading-note-067)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`pollRetiredEpochs()`](../../src/core/vkcore/swapchainframetarget.cpp#L1641) | quarantine と retire の分岐 | 隔離される条件 | GPU 完了済みだが present 未証明で surface が lost の場合、maintenance1 ありなら present fence が lost 後も完了を証明できるのでポーリング続行。無い場合は**「後続 reacquire は同一 surface 内でしか意味を持たない」ため証明が永久に得られず**、隔離して device teardown まで保持しつつリストからは外す |
| [`SwapchainEpoch` のメンバー宣言順](../../src/core/vkcore/swapchainframetarget.cpp#L490) | 逆順破棄の設計 | 宣言順が仕様である理由 | (a) `surface_epoch` リースを先頭に置いて全 swapchain 子オブジェクトが `VkSurfaceKHR` より先に壊れるようにし、(b) command_pool を command_buffers より前、(c) swapchain を image view / semaphore / fence より前に置く。**Vulkan の親子生存期間規則を宣言順だけで自動的に守る** |
| [present 後の注入](../../src/core/vkcore/swapchainframetarget.cpp#L2606) | 戻り値だけ差し替える | 本物を呼ぶ理由 | present は**エラーを返してもセマフォ待ちを実際にキューへ積み**、maintenance1 の present fence も既にシグナル対象になっている。本番と同じリソース lifetime 契約を保ったまま故障を注入するため。→ §9.22「fault injection seam」 |
| [`recovery_anchor` へ candidate を移す行](../../src/core/vkcore/swapchainframetarget.cpp#L1767) / [anchor 選択](../../src/core/vkcore/swapchainframetarget.cpp#L1797) | framebuffer.revision 不一致 | 捨てずに回す理由 | candidate は完成しているが framebuffer が変わって陳腐化した場合、破棄すると**「非退役 swapchain が 2 つ」になる / ウィンドウが掴まれたままになる**。candidate を `recovery_anchor` に移し、次の再構築の oldSwapchain として再利用しつつリトライを予約する |
| [format スコアリング](../../src/core/vkcore/swapchainframetarget.cpp#L264) | 20 / 10 / 0 の重み | 数値の根拠 | **未執筆**(スコア値の根拠コメントが無い)。実装は sRGB nonlinear の SRGB を 20、同 colorSpace の UNORM を 10、他 0 で `stable_sort` し、present mode も mailbox を先頭に安定ソートするだけ |
| [`chooseSwapchainImageCount()`](../../src/core/vkcore/swapchainframetarget.cpp#L176) / [composite alpha](../../src/core/vkcore/swapchainframetarget.cpp#L205) / [preTransform 固定](../../src/core/vkcore/swapchainframetarget.cpp#L339) | swapchain 作成パラメータ | ヒューリスティックの根拠 | image count は Vulkan の一般則(→ [第10章](10_background_knowledge.md))。**composite alpha の優先順と preTransform=current + clipped=VK_TRUE の固定化の根拠は未執筆** |
| [present fence の pNext](../../src/core/vkcore/swapchainframetarget.cpp#L2568) / [`present_wait_pending`](../../src/core/vkcore/swapchainframetarget.cpp#L2634) | 完了追跡の部品 | 個々の役割 | 上記「present 完了追跡の 2 系統」の一部。詳細は**未執筆** |
| [acquire の lock](../../src/core/vkcore/swapchainframetarget.cpp#L2170) / [`markSurfaceLost()`](../../src/core/vkcore/swapchainframetarget.cpp#L1498) / [`abandonFrameSerial()`](../../src/core/vkcore/swapchainframetarget.cpp#L2757) | surface_epoch まわり | 一体で何を守るか | 上記 2 項目と一体。詳細は**未執筆** |
| [bootstrap epoch](../../src/core/vkcore/swapchainframetarget.cpp#L884) / [BuildRequest](../../src/core/vkcore/swapchainframetarget.cpp#L660) / [nonblocking](../../src/core/vkcore/swapchainframetarget.cpp#L2135) | 初期化と要求の粒度 | 別枠にする理由 | **未執筆**(根拠がコードにない) |
| [XR swapchain の状態機械](../../src/core/openxr/openxrcompositiontarget.cpp#L399) / [reusable 判定](../../src/core/openxr/openxrcompositiontarget.cpp#L676) | idle → acquired → waited → submitted → released | 再利用できる状態 | `beginLogicalFrame` は idle / released のみ再利用可。[「wait 成功前の release は不正」](../../src/core/openxr/openxrcompositiontarget.cpp#L484)なので、acquired 止まりの image は release せず `teardown_required` を立てて世代ごと破棄する |
| [`beginView()`](../../src/core/openxr/openxrcompositiontarget.cpp#L711) / [endView の分岐](../../src/core/openxr/openxrcompositiontarget.cpp#L820) / [view_family の終端](../../src/core/openxr/openxrcompositiontarget.cpp#L818) | 順序強制と 2 経路 | sequential と multiview の同期差 | 上記 2 経路の排他性から従う。詳細は**未執筆** |
| [`FrameStage`](../../src/core/openxr/openxrcompositiontarget.cpp#L56) / [`ViewState` と `SwapchainFrameState`](../../src/core/openxr/openxrcompositiontarget.cpp#L123) | フレームの段と、それに直交する状態 | 有効な組み合わせ | フレームの段は bool の集合ではなく 7 値の enum です。遷移の合法表は [`frameStageMoveIsLegal()`](../../src/core/openxr/openxrcompositiontarget.cpp#L86) が持ち、違反は [`requireFrameStageMove()`](../../src/core/openxr/openxrcompositiontarget.cpp#L112) が投げます。bool として残るのは眼ごとの `ViewState` と、段に直交する `depth_submission_active` / `teardown_required` だけです |
| [command buffer slot](../../src/core/openxr/openxrcompositiongraphics.cpp#L73) / [`in_flight_frames_num`](../../src/core/openxr/openxrcompositiongraphics.cpp#L335) | 索引式と枚数 | 決定基準 | **未執筆**(根拠がコードにない) |
| [`queryViews()`](../../src/core/openxr/openxrcompositiontarget.cpp#L202) | 両眼で同じ extent を強制 | 揃える理由 | **未執筆**(max(recommended) と min(maxImageRect) の組み合わせという実装以上の根拠がない) |

### 9.23.15 physics

#### [`PhysWorld::updateTriggers()`](../../src/core/phys/physworld.cpp#L743) — 2-way merge と canonical pair

**分からなくなること**: Enter と Exit が同じフレームに混ざったときの発火順。

**答え**: canonical pair のソート済み集合を merge し、両 entity の self 視点へイベントを届けます。 → [第5章 §5.7 の本文](05_gameplay_and_services.md#reading-note-068)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [`prepareBindings()`](../../src/core/phys/physworld.cpp#L287) | 候補構築と二重チェック | なぜ全部ここで検査するか | **publish が `noexcept` になるよう失敗しうる検査を全部集める**(→ §9.22)。name 重複・collider の validate・transform 成分の存在確認を行い、id は未割り当てなら採番、既存 id が next 以上なら next を進める。上限一致で `overflow_error` にして **id の巻き戻し = 退役 collider とのエイリアス**を防ぐ |

### 9.23.16 game DLL / schema

#### [`reloadTransaction()`](../../src/core/gamelogic/gamelogicreload.cpp#L584) — previous_unloaded と rollback の分岐

**分からなくなること**: rollback の 2 分岐と、例外メッセージを退避してから unload する理由。

**答え**: 旧 DLL が unload 済みかを見て、再ロードと元の handle / owner の復帰を分けます。 → [第5章 §5.13 の本文](05_gameplay_and_services.md#reading-note-069)。

#### [`activation_delay`](../../src/core/gamelogic/behaviorarena.cpp#L574) — 1 フレーム遅らせる理由

**分からなくなること**: 生成直後に init が走らない理由と、失敗時の破棄順。

**答え**: attachment の init を1フレーム遅らせ、scene 公開完了後に活性化します。 → [第5章 §5.13 の本文](05_gameplay_and_services.md#reading-note-070)。

#### [`sameSceneBehaviorOrigin()`](../../src/core/gamelogic/behaviorarena.cpp#L491) — 許される遷移方向

**分からなくなること**: 既存 attachment を再利用してよい条件。

**答え**: origin の異種間再利用は遷移表が指定した片方向だけに許されます。 → [第5章 §5.13 の本文](05_gameplay_and_services.md#reading-note-071)。

#### [`diagnoseGameLogicImports()`](../../src/core/gamelogic/gamelogicimports.cpp#L559) — expected_epoch と host_schema_entrance

**分からなくなること**: 似た 2 つの版チェックが両方要る理由と、anchor が `extern "C"` である理由。

**答え**: SDK epoch と登録 schema entrance は別軸なので、ロード前に両方を照合します。 → [第2章 §2.11 の本文](02_runtime_lifecycle.md#reading-note-072)。

#### [`StructFieldRangeDescriptor`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L107) — "constexpr-in-incomplete-owner" 不変式

**分からなくなること**: 記述子が POD なのに、materialize 後は `std::variant` になる理由。

**答え**: 不完全な所有者型の中では variant を構築せず、POD descriptor の materialize を後へ遅らせます。 → [第5章 §5.14 の本文](05_gameplay_and_services.md#reading-note-073)。

**台帳**: コメントが名指しする WP71(MSVC 回避)。

#### [List の `$list` envelope](../../src/core/userpublic/details/schema/structfieldjson.hpp#L591) — row id の規約

**分からなくなること**: 素の配列ではなく封筒 + 行オブジェクトにした理由と、`id=0` / `next_id` の意味。

**答え**: 行は不変の row id で識別し、next id は既存最大値を超える必要があります。 → [第5章 §5.14 の本文](05_gameplay_and_services.md#reading-note-074)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [ロック検出と debounce のコメント](../../src/core/gamelogic/gamelogicreload.cpp#L774) | source_busy_retries | 失敗と pending の別 | ビルドは DLL をその場で truncate → 書き戻すことがあるので、まずサイズ / 更新時刻の同一サンプルを 2 回取って debounce し、次に `FILE_SHARE_READ` のみで開いてライタを排除してからシャドウコピーを取る。`ERROR_SHARING_VIOLATION` / `ERROR_LOCK_VIOLATION` は**失敗ではなく pending 扱い**で、retries が 3 回に達したときだけ `source_busy` |
| [`makeShadowCopy()`](../../src/core/gamelogic/gamelogicreload.cpp#L373) / [`sweepStaleShadowDirectories()`](../../src/core/gamelogic/gamelogicreload.cpp#L335) | シャドウコピーの規約 | ディレクトリ名の意味 | シャドウ先は `<source_dir>/.pelican-hot-reload/<PID>/` で、**PID をディレクトリ名にすることで同一 DLL を複数プロセスが同時にホットリロードしても衝突しない**。PDB があれば同名で併走コピーする(失敗は警告のみ)。掃除は「名前を PID としてパースでき、その PID が生存していない」ものだけ削除 |
| [`validateBehaviorReloadSources()`](../../src/core/gamelogic/behaviorreloadvalidation.cpp#L85) | schema_changed_without_version_bump | 何を比べているか | `schema_version` が同じなのに `params_schema_fingerprint` が違えば**「バージョンを上げずにスキーマを変えた」**として拒否(同一版で表現が変わると既存 canonical params の解釈が壊れる)。version が上がっている場合は authored の生 `source_params` を candidate の `canonicalize_params` に通す。指紋そのものは[第5章「スキーマの構造をハッシュ値で識別する」](05_gameplay_and_services.md) |
| [`sceneBehaviorAttachmentSeq()`](../../src/core/gamelogic/behaviorarena.cpp#L74) / [`refreshSceneAttachmentSequence()`](../../src/core/gamelogic/behaviorarena.cpp#L457) | seq の符号化と再計算 | scene_ordered のときだけ再計算する理由 | seq は「シーン内の出現順」をそのまま単調な 64bit キーにする符号化。**scene_ordered でない(ランタイム生成の)attachment は自分の識別子由来の seq を持ち、シーンの index 変化に追随してはいけない** |
| [attach 時の component_index シフト](../../src/core/gamelogic/behaviorarena.cpp#L1371) / [`reused` に載っているかの判定](../../src/core/gamelogic/behaviorarena.cpp#L1446) | 挿入 / 削除時の詰め替え | ずらさないと何が起きるか | component_index は同一 entity 内の並び順そのものなので、attach で挿入位置以降を +1、remove で以降を −1 してから seq を再計算しないと**実体とずれて衝突する**([公開時に seq 重複チェックがある](../../src/core/gamelogic/behaviorarena.cpp#L809))。所有権は「`reused` に載っている attachment のインスタンスは既存アリーナ側が持つ」モデル |
| [detached 配列との交換](../../src/core/gamelogic/behaviorarena.cpp#L474) / [`transferRuntime`](../../src/core/gamelogic/behaviorarena.cpp#L332) | publish/rollback の実体 | 所有権の移り方 | → §9.22「三段トランザクション」を behavior arena で実現したもの。詳細は**未執筆** |
| [5 種の編集の事前/事後条件](../../src/core/gamelogic/behaviorarena.cpp#L1308) / [pending/active の組み合わせ](../../src/core/gamelogic/behaviorarena.cpp#L942) | 条件表 | 各編集の前提 | 上記 identity 三層と activation_delay から従う。**個別の条件表は未執筆** |
| [`BehaviorCallbackScope`](../../src/core/gamelogic/behaviorarena.cpp#L23) | callback_owner の切り替え | 何を追跡するか | 「今どの DLL 世代のコードが走っているか」を追跡して、リロード時に古い owner のインスタンスを先に破棄できるようにするため。詳細は**未執筆** |
| [候補 DLL の検証](../../src/core/gamelogic/gamelogicreload.cpp#L466) / [Windows 固有の import 診断](../../src/core/gamelogic/gamelogicreload.cpp#L404) / [active の有無での分岐](../../src/core/gamelogic/gamelogicreload.cpp#L497) | ロード前の関門 | 入口の全体像 | 上記 `diagnoseGameLogicImports` と reloadTransaction の入口。詳細は**未執筆** |
| [`appendThunkImports()`](../../src/core/gamelogic/gamelogicimports.cpp#L306) | ordinal_flag と終端判定 | PE の読み方 | PE Import Table の仕様そのもの(→ [第10章](10_background_knowledge.md)) |
| [`recursive()`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L160) | Flags / List で true | v2/v3 分岐の判定 | v3 の記述子は所有型の再帰ツリーで、**Flags と List はフラットな V2 `StructFieldSchema` では表現できない唯一のタグ**(語彙と行 struct という子構造を持つ)。`recursive()` はそれらを部分木に含むかを返す |
| [`schemaNodeFingerprint()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L1042) | 指紋が JSON dump そのもの | hash にしない理由 | 用途は schema 同一性の厳密比較で、**正準 JSON dump は全順序かつ自己記述的なので、不一致時にどのフィールドが変わったかをそのまま diff でき、ハッシュ衝突を仮定する必要もない** |
| [Flags の文字列配列表現](../../src/core/userpublic/details/schema/structfieldjson.hpp#L417) | ビットを名前で書く | 数値にしない理由 | 語彙を持つタグの JSON 表現。数値ビットを直接書かせないことで語彙変更に耐える |
| [`Flags`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L194) | enum を unsigned storage へ | 変換の理由 | **未執筆**(flag ビット操作の正当性という以上の根拠がコードにない) |
| event 側の 3 分岐(`has_descriptor` / `recursive_descriptor` / `json_serializable`)と `EventSchemaState` | 登録時の分類 | 組み合わせ表 | **未執筆**(3 条件の組み合わせ表が要る) |

### 9.23.17 glTF

#### [`next_generated_material = -2`](../../src/core/model/gltf.cpp#L1273) — 負値が生成マテリアルを示す

**分からなくなること**: マテリアル ID に負値が現れる理由。

**答え**: 非負の source material index と -1 の未指定に衝突しないよう、生成 material は -2 から採番します。 → [第3章 §3.8 の本文](03_project_and_loading.md#reading-note-075)。

#### [`resolveFragmentCandidate()`](../../src/core/model/gltf.cpp#L1794) — full_path と alias

**分からなくなること**: 2 通りの指定が両方一致したときに、なぜエラーにするのか。

**答え**: alias は複数経路に一致しうるので、曖昧な場合は full path の指定を要求します。 → [第3章 §3.8 の本文](03_project_and_loading.md#reading-note-076)。

#### [VRM firstPerson auto の primitive 分割](../../src/core/model/gltf.cpp#L1073) — なぜ分けるしかないのか

**分からなくなること**: 三角形単位の可視性を、なぜ primitive を増やして表すのか。

**答え**: 可視性が primitive 単位なので、firstPerson の異なる三角群を別 primitive に分けます。 → [第3章 §3.8 の本文](03_project_and_loading.md#reading-note-077)。

#### [morph target layout の generation](../../src/core/model/gltf.cpp#L47) — 何を追跡する世代か

**分からなくなること**: GPU 割り当てとの関係。

**答え**: morph layout generation は GPU 割り当て順に依存しない不変モデル世代の identity です。 → [第3章 §3.8 の本文](03_project_and_loading.md#reading-note-078)。

#### [`geometry_allocation_id`](../../src/core/model/modeltemplate.hpp#L99) — 何を識別し、どこで使うか

**分からなくなること**: この ID が指す粒度。

**答え**: geometry allocation id は vertex / index の suballocation を識別し、BLAS cache key に使います。 → [第3章 §3.8 の本文](03_project_and_loading.md#reading-note-079)。

| 場所 | 何をしている所か | 何が分からなくなるか | 答え / 参照 |
|---|---|---|---|
| [palette 超過](../../src/core/model/gltf.cpp#L2240) | error でなく `skinned=false` | 落とさない理由 | [コメント](../../src/core/model/gltf.cpp#L2241)どおり**「v1 パレット超過のスキンを持つアセットも読み込めなければならない」**(WP38 以前は skin を丸ごと無視していた挙動とのパリティ)。明示的にそのスキンをアニメーションで使った場合だけ `selectSkin` が例外にする |
| [`transformVertexData()`](../../src/core/model/gltf.cpp#L708) | morph の有無で normal 正規化を分ける | 正規化しない場合がある理由 | morph がある場合、base normal/tangent を正規化すると**同じ normal_transform を掛けただけの morph delta と線形空間が食い違い、`normal + Σw·delta` の合成が壊れる**ため、非正規化のまま残して合成後にシェーダ側で正規化させる |
| [`selectSkin()`](../../src/core/model/gltf.cpp#L1609) | 複数 skin を 1 パレットへ | joint_offset の意味 | → [第3章「128枚の結合palette」](03_project_and_loading.md) |
| [`gltfSurface()`](../../src/core/model/gltf.cpp#L1366) | `engine://` surface の解決 | authoring 由来を受けない理由 | ローダは glTF のソース値を運ぶだけで、**routing とラッパ選択は project 側の lowering が行い**、その結果選ばれる `engine://surfaces/openpbr/*.surface` を組み込みリソースから解決・パース・キャッシュする。非 engine 参照は例外 |
| [`MaterialInfo::skinned_vert_shader`](../../src/core/material/material.hpp#L41) | skinned の頂点 shader | skin 付きの部品の material をどう作るか | ✅SK-1 から glTF の material の複製(旧 `skinnedMaterial()`)は無い。1 つの material が `skinned_vert_shader` を持ち、登録時に standard / skinned の両方の pipeline を作り、部品の `skinned` で描き分ける([第6章 §6.5](06_rendering_vulkan_shader.md#65-dynamic-rendering-と-pass-実行) の「頂点の mode は部品が決め、material は 1 つのまま」)。負の ID の派生は VAT だけ |
| [`authored_reference` があるときだけ IR を計算](../../src/core/model/gltf.cpp#L2098) | normalized_alpha_ir の条件 | 空のままにする理由 | **typed な project / path-resolver 経路だけが authored provenance を持ち、IR の identity は authored reference から作られる**ので、inspection や legacy 直接ロードでは identity を捏造せず空のままにする(→ §9.23.5 の profile 表) |
| [`selectLoad()`](../../src/core/model/gltf.cpp#L1828) | animation_only | mesh を読まない理由 | animation fragment は **CPU 側のクリップ / スキンデータのみ**を持ち、意図的に無関係な mesh・material・texture・ECS ボーンリソースを一切作らない(`roots` が空なので mesh 経路にも入らない) |
| [`meshMorphTargetCount()`](../../src/core/model/gltf.cpp#L1398) | 全 primitive で同数を要求 | 揃える理由 | `mesh.weights` / `node.weights` は **mesh 単位で 1 本の配列**で、`appendMorphDefaults` も mesh 単位で 1 つの `morph_weight_offset` ブロックしか確保しないので、primitive ごとに数が違うと 1 本の weight レンジを共有できない。→ [第3章「morphはnode単位で切る」](03_project_and_loading.md) |
| [morph accessor の sparse 拒否](../../src/core/model/gltf.cpp#L758) | morph accessor の制限 | 2 つの理由 | sparse は morph target v1 のデコード経路が未実装なので明示拒否。finite 検査は delta が GPU バッファと bounds ソースに入るため、**NaN/Inf が bounds とドローソートを汚染するのを入口で止める** |
| [`collectNodeOccurrence()`](../../src/core/model/gltf.cpp#L1662) | (node_index, full_path) で重複検出 | DAG を許す理由 | 同一 node が複数の親から参照される再利用を許しつつ、**同じ (node, path) の再訪だけを打ち切る**ため。パスが違えば別インスタンスとして記録され、同じパスでの再帰は 1 回で止まる |
| [`~LiveGltfResourceSink()`](../../src/core/model/gltf.cpp#L570) / [`finish()`](../../src/core/model/gltf.cpp#L557) | release と finish の排他 | `committed_` の意味 | → §9.22「三段トランザクション」(`committed_` が「もう解放してはいけない」印)。詳細は**未執筆** |
| [`firstPersonAnnotation()`](../../src/core/model/gltf.cpp#L652) / [`headRelatedNodes()`](../../src/core/model/gltf.cpp#L678) | 4 値とメモ化 | 分割の入力 | 上記 VRM 分割の入力。詳細は**未執筆** |
| [lowering → surface compilation → routing](../../src/core/model/gltf.cpp#L2024) / [named_materials の構築](../../src/core/model/gltf.cpp#L2460) / [VAT テクスチャ](../../src/core/model/gltf.cpp#L866) | 3 段の材質処理 | 段の切れ目 | **未執筆**(→ [第3章「逸脱がforwardへ落とす」](03_project_and_loading.md) が routing 側の一部を扱う) |

### 9.23.18 領域をまたぐ 3 つの規約

- **診断・トレース JSON は外部から読まれる前提で書かれています。** `basicconfig` の保存診断、`effecttransaction` の effect 診断、`renderer` のフレームトレース、`rpcserver` の status JSON はいずれも RPC / ログ越しに外部ツールが読む出力です。契約側の明文は `docs/external_tools_requirements.md` の **R8**(JSON-RPC 2.0 / NDJSON フレーミング)と **R10**(出力に schema 名 + version を埋め、互換を壊す変更は version を上げる)で、**個々の診断 JSON のどのキーまでが契約かを定めた記述は無く、未執筆**です。
- **エラーコードには `@1` が付きます。** `pelican.ray_row_count_limit@1`、`pelican.ray_profile_opaque_as@1`、`pelican.ray_tracing.frame_tlas_required@1`、editor 側の各コードがこの形です。`docs/design_error_contract.md` も `UserDataError` の `code` を「名前付き、`@1` 付き」と書いていますが、**この番号が何の版で、いつ上げるのかを定めた記述はコードにも文書にも無く、未執筆**です(「コード名の意味の版」と読むのは推測)。
- **表示用文字列は identity ではありません。** [`displayObjectName()`](../../src/core/loader/scene.cpp#L277) の `<unnamed>` のような値は診断出力の見た目で、同一性の判定には使いません。identity 側は §9.22「identity / canonical key / lease の三層」を参照してください。

なお **「機械判定できる subset を先に固定する」**(`docs/design_raytracing.md` §4 / §4-P の規範表。「決定論的適合部分集合」という語自体は文書にありません)という考え方 — GPU や数値の非決定性を避けるため、形状と数値範囲を先に制限してから比較する — は RT だけのものではありません。physics の tie-break(§9.23.15)、draw order の tie-break(§9.23.4 の `declaration_ordinal`)にも同じ発想が現れます。

## 9.24 v0 で足した機構の難所(共通グラフ・package・節 locator・オフライン時刻)

ここに並ぶ 18 件は、v0(AG / P / O / X / R 群、V0-ADD の RS-1、Studio の FX-4 / FX-8、v0 を閉じる段の RX-1 / VR-1 / DS-1f と test の揺れ)で入った機構のうち、**素朴に読むと逆の結論に着地する**箇所です。他章の `🧩 難所` ブロックと同じ役割なので、書式もそれに合わせて「素朴に読むと / 実際は / 不変条件 / 読む場所」の 4 つで書いてあります。**この節だけは実装が新しいぶん、既存の設計文書側に説明が無い**ものが多く、根拠はすべてコードとヘッダのコメントです。

### 難所 — byte 正確な節 locator と絶対 offset

**素朴に読むと**: `pelican.graph` root の `rendering` 節を編集するなら、JSON を parse して部分木を取り出し、書き換えて dump し直せばよい、と読めます。

**実際は**: 編集は最後まで **byte 列のまま**行われます。[`locateSectionMembers()`](../../src/project/renderconfigdocument.cpp#L309) は root のメンバを字句走査する [`locateObjectMembers()`](../../src/project/renderconfigdocument.cpp#L204) を 2 回使い、まず root で `rendering` の値スパンを見つけ、その位置から同じ走査を節の中へ入れ直します。要点は**入れ子にしても offset を相対化しないこと**で、ヘッダのコメントがそれを規範として書いています — 「同じ bytes の中に位置づけるので、offset は絶対のまま、文書 digest は file 全体を覆い続ける」([コメント](../../src/project/renderconfigdocument.cpp#L307))。

この性質があるので、節を編集しても CAS の単位([`renderConfigSourceDigest()`](../../src/project/renderconfigdocument.cpp#L431))は文書全体のままで変わりません。書き手は [`withSectionValue()`](../../src/project/renderconfigdocument.cpp#L755) 1 本で、graph root 自身のキー(と editor の `layout`)は [`withRootValue()`](../../src/project/renderconfigdocument.cpp#L714) へ落ちます(MG-2b で legacy root は読み手ごと無くなりました)。

読み手と書き手の非対称も意図です。[`renderingSectionOf()`](../../src/project/renderconfigsection.hpp#L36) は節が無ければ `RenderConfigSectionUnlocated` を投げ、[`renderingSectionOrEmpty()`](../../src/project/renderconfigsection.hpp#L49) は空 object を返します。**読みは節が無くても失敗せず、編集だけが `render_config_section_unlocated` という名前で拒否されます**([拒否の実例](../../src/core/communication/renderconfigeditor.cpp#L1718))。

**不変条件**: 節の中の offset を「節の先頭からの相対」に変えないこと(digest が節だけを覆う別物になり、CAS の意味が変わります)。読みの経路に `renderingSectionOf()` を使わないこと — 節を持たない graph root を読んだだけで例外になります。authored key の住所判定を各 caller が `if (graph)` で持たないこと。

**読む場所**: [`renderconfigsection.hpp`](../../src/project/renderconfigsection.hpp#L12) → [`locateSectionMembers()`](../../src/project/renderconfigdocument.cpp#L309) → [`withSectionValue()`](../../src/project/renderconfigdocument.cpp#L755)。

> **N-6 の例外**: `edit_graph_document` の op 列が `set_layout` だけのとき、editor は候補文書を `apply_candidate` に通さず(`Ticket::source_only`)、文書を書くだけで commit を終えます。「すべての commit は候補経路を通る」の唯一の例外で、置き場所は engine の挙動に触れないためです([経路](../../src/core/communication/renderconfigeditor.cpp#L3842))。

### 難所 — 排他 create の committed marker

対象は project 文書の transaction です。rendering config の編集のために作られ(G-3a)、MG-1 で `src/project/projectdocumenttransaction.*` へ移って `project migrate` の apply と共用になりました。ディレクトリ(`.pelican/render-authoring.transaction`)も形式も移設の前と同じなので、以下の規則は両方の書き手に効きます。

**素朴に読むと**: transaction が commit されたかどうかは `manifest.json` の `status` を読めば分かる、と読めます。

**実際は**: 決定は**別ファイルの存在**です。`manifest.json` は [`writeManifest()`](../../src/project/projectdocumenttransaction.cpp#L343) が**一度だけ**書いて二度と上書きせず、commit の印は [`publishCommittedMarker()`](../../src/project/projectdocumenttransaction.cpp#L376) が `committed.next` → `committed` の hard link で公開します。使う primitive は [`linkExclusive()`](../../src/project/projectdocumenttransaction.cpp#L219) で、「名前を公開し、既に取られていれば失敗する」を 1 操作で行います。rename にしなかった理由はコメントが規範で、**生きたファイルを置き換えないので、置き換え側が飽和した scanner の下で負け続けていた問い(宛先のハンドルを壊せるか)を訊かずに済む**からです。

marker は自分が属する manifest に digest で縛られます。[`transactionDecision()`](../../src/project/projectdocumenttransaction.cpp#L411) は marker を先に見て、記録された digest と現在の `manifest.json` の digest が食い違えば `render_authoring_manifest_mismatch@1` で[名前付きに落とします](../../src/project/projectdocumenttransaction.cpp#L421) — 既に durable な編集を黙って rollback しないためです。

旧形式(marker を持たず、`manifest.json` の `status == "committed"` で決定を書いていた形)は MG-2a から**読みません**。R-1 の裁定では読み取り互換を残していました(`docs/v0_freeze.md` §2 行 6。消すと legacy なディレクトリを持つ project が二度と commit できない wedge になる、という理由)。MG-2a は「黙った wedge を名前付きの停止に変える」([design_v0_production_gaps.md](../design_v0_production_gaps.md) §2.3 決定 3)を採り、2 つの読み手を同時に消しました。

今は [`rejectLegacyTransactionDirectory()`](../../src/project/projectdocumenttransaction.cpp#L401) が recovery の判定(`transactionDecision()` の先頭)と commit 前の guard の両方で呼ばれ、`render_config.legacy_transaction_dir@1`(error_fields は `path`)を投げます。prepared として rollback すれば commit 済みの編集を消し、committed として片付ければ知らない形式を黙って読むことになるので、どちらもせずディレクトリを残して開発者に渡します。edit の結果にもこの code と `path` が載ります(G-4 の carried refusal の仕組みに相乗り)。

掃除の順序も難所です。[`removeTransactionDirectory()`](../../src/project/projectdocumenttransaction.cpp#L586) は `manifest.json` を**明示的に先に**消します。ディレクトリの列挙順は照合順で `committed` が `manifest.json` より先に来るため、素直に `remove_all` へ任せると「marker だけ消えて prepared な manifest が残る」= 復旧が「rollback すべき transaction」と読む最悪の残骸ができるからです([順序のコメント](../../src/project/projectdocumenttransaction.cpp#L593))。

**不変条件**: marker が見えたら rollback 経路に入らないこと。旧形式の dir を rollback も片付けもせず、名前付きで止めること。marker の公開を rename 系に戻さないこと。`manifest.json` を後から書き換えないこと(digest の束縛が切れます)。掃除で `manifest.json` を最初に消す順序を変えないこと。

**読む場所**: [`linkExclusive()`](../../src/project/projectdocumenttransaction.cpp#L219) → [`publishCommittedMarker()`](../../src/project/projectdocumenttransaction.cpp#L376) → [`transactionDecision()`](../../src/project/projectdocumenttransaction.cpp#L411) → [commit 前の guard](../../src/project/projectdocumenttransaction.cpp#L705)。

### 難所 — unwrapped time から作る root delta(AG-2)

**素朴に読むと**: root motion は「今フレームの root 位置 − 前フレームの root 位置」で足りるように見えます。

**実際は**: clip の時刻は境界で折り返されるので、そのままの差分はループのたびにクリップ先頭へ飛び戻ります。[`extractRootMotion()`](../../src/core/animation/posegraphruntime.cpp#L338) は**折り返す前の時刻**(`unwrapped`)を受け取り、`R(u) = loops(u)·(root(end) − root(start)) + root(canonical(u))` を作ります([式のコメント](../../src/core/animation/posegraphruntime.cpp#L379))。`loops` は[非ループ clip では 0](../../src/core/animation/posegraphruntime.cpp#L385) で、この項があるおかげで「負の速度」も「1 フレームで複数ループ」も同じ算術に収まります。clip 定数(`root_start` と 1 ループ分の移動量)は binding ごとに 1 回だけサンプルし、**そのフレームの姿勢を一時的に使って測ってから元に戻します**([サンプル部](../../src/core/animation/posegraphruntime.cpp#L355))。

フレーム差分は `R(u) − R(u_prev)` で、`set_time` を観測したフレームは [`drop_root_previous`](../../src/core/animation/posegraphruntime.cpp#L392) によって前回値を捨てます — 時刻のカットは移動ではないからです。再バインドでも同じく状態を捨てます([rebind 時の reset](../../src/core/animation/posegraphruntime.cpp#L256))。最後に **pin**: 公開する姿勢の root translation を `root_start` へ戻し([pin](../../src/core/animation/posegraphruntime.cpp#L402))、移動はオブジェクトの transform 側からだけ出ていくようにします。回転の抽出は v0 にありません。

**不変条件**: `canonical` 済みの時刻から delta を作らないこと(ループごとに逆向きの跳ねが出ます)。pin を外したまま変位も足さないこと(二重に動きます)。`set_time` / 再バインドで前回値を残さないこと。

**読む場所**: [`canonicalTime()`](../../src/core/animation/posegraphruntime.cpp#L39) → [`extractRootMotion()`](../../src/core/animation/posegraphruntime.cpp#L338) → [変位をオブジェクトへ渡す所](../../src/core/animation/posegraphruntime.cpp#L823)。適用側は[第4章 §4.13](04_ecs_deep_dive.md)。

### 難所 — per-node の評価分類は `max()` でしか動かない

**素朴に読むと**: 「この op の評価分類」はモジュールが登録時に宣言した 1 値で、node ごとには変わらない、と読めます。

**実際は**: ports を authored parameters から作る factory は、**分類を引き上げることだけ**ができます。[`resolveOperation()`](../../src/project/graphmodule.cpp#L676) は factory の結果に対して `contract.evaluation = std::max(...)` を取り([該当行](../../src/project/graphmodule.cpp#L700))、`direct < replay < baked < recorded` の順で強い方を採ります。

理由は AG-2 で現物が出ました — root motion を抽出する `pose.clip_sample` は前フレームの変位を保持するので `replay` で、抽出しない同じ op は `direct` です。`max` なので**カタログの宣言より弱くはなれず**、catalog と実際の node が食い違いません。[`domain`](../../src/project/graphmodule.cpp#L691) / [`parameter_change`](../../src/project/graphmodule.cpp#L701) / [`parameter_changes`](../../src/project/graphmodule.cpp#L702) の 3 つは、factory の値を捨てて登録側の値へ戻します。

評価分類を実際に強制するのは値バックエンドで、状態を渡されずに replay 系の op を評価しようとすると [`graph.evaluation_requires_state@1` の送出](../../src/core/userpublic/valuegraph.cpp#L62) で拒否します。対象は `audio.playback` / `value.event_clock` / `value.trigger_clock` の 3 つ([拒否点](../../src/core/userpublic/valuegraph.cpp#L871))で、**op 名は authored 文書の綴り**で返ります(lowering がモジュール名前空間を落とすので、拒否を読む側がその事情を知らなくて済むように、という取り決めです)。

**不変条件**: factory で `evaluation` を下げないこと。分類の順序(`direct < replay < baked < recorded`)を [`graphcomposition.hpp`](../../src/project/graphcomposition.hpp#L267) の enum 順と別に持たないこと。拒否に lowering 後の綴りを載せないこと。

**読む場所**: [`GraphOperationRegistration`](../../src/project/graphmodule.hpp#L25) → [`resolveOperation()`](../../src/project/graphmodule.cpp#L676) → [`statelessEvaluationRejected()`](../../src/core/userpublic/valuegraph.cpp#L61)。

### 難所 — package の SDK は「転送ヘッダのディレクトリ」そのもの

**素朴に読むと**: package のコードが public SDK だけを使っていることは、レビューか命名規約で守るものに見えます。

**実際は**: **include path で守られています**。[`pelican_package_sdk_include_dir()`](../../src/player/CMakeLists.txt#L175) が build ディレクトリの下に、public SDK が公開するヘッダ 1 本につき 1 行だけの転送ヘッダ([`package_sdk_header.hpp.in`](../../src/player/package_sdk_header.hpp.in))を生成します。対象は `src/core/userpublic/**`、`src/core/ecs/**`、`handle.hpp`、`phys/physquery.hpp`。

[`pelican_package_sources()`](../../src/player/CMakeLists.txt#L219) が作る package target の include path には **`src/core` が入りません**([その省略に付いたコメント](../../src/player/CMakeLists.txt#L246))。したがってエンジン内部ヘッダを include した package コードは、規約違反ではなく**コンパイルエラー**になります。コピーではなく転送なので、実ヘッダは自分の相対 include を自分で解決でき、内容が腐りません。

呼ぶ順序も強制されます。[`pelican_packages()`](../../src/player/CMakeLists.txt#L276) は project の `packages/*/package.json` を名前順に調べ、`code/CMakeLists.txt` を持つものだけを `add_subdirectory` し、生成された OBJECT ライブラリを project の game logic DLL へ足します。DLL がまだ無ければ [`FATAL_ERROR`](../../src/player/CMakeLists.txt#L324) で「`pelican_game_sources(...)` の後に呼べ」と言います。中断した install が残した [`.tmp-` 付きディレクトリは読み飛ばし](../../src/player/CMakeLists.txt#L293)ます。

**不変条件**: package target の include path に `src/core` を足さないこと。転送ヘッダの生成対象を「SDK が公開するもの」以外へ広げないこと。`pelican_packages()` を `pelican_game_sources()` より前に置かないこと。

**読む場所**: [`package_sdk_header.hpp.in`](../../src/player/package_sdk_header.hpp.in) → [`pelican_package_sources()`](../../src/player/CMakeLists.txt#L219) → [`pelican_packages()`](../../src/player/CMakeLists.txt#L276)。

### 難所 — publish snapshot の `node_values` は「グラフを持った」ベクタ

**素朴に読むと**: 評価結果は名前付きの map で返る、あるいは名前を持たない値の配列を受け取って後からグラフに問い合わせればよい、と読めます。

**実際は**: [`PublishResult::node_values`](../../src/core/userpublic/valuegraph.hpp#L112) は**グラフ自身の node 順に並んだ `optional<double>` のベクタ**です。名前は載りません — 名前の解決は「読む側が 1 回だけ払う」もので、描画経路が毎フレーム払うものではない、という判断です。非活性スコープで飛ばされた node は 0 ではなく**不在**になります。

スナップショット側の [`PublishedInstanceInputs`](../../src/core/userpublic/valuegraph.hpp#L116) が重要で、**順序を決めたグラフを `shared_ptr` で保持します**([コメント](../../src/core/userpublic/valuegraph.hpp#L123))。借用ではないので、描かれた世代が差し替わった後に到着した読み手も、その世代の名前で値を読めます。公開の唯一の入口は [`publishGraphValues()`](../../src/core/loader/scenegraphruntime.cpp#L145) で、ここで `published = true` が立ちます。したがって **ベクタだけを持ち回る reader は、再 publish 後に別の展開へ index することになります**。

**不変条件**: `node_values` をグラフと切り離して保持しないこと。欠けている値を 0 で埋めないこと(「評価されなかった」と「0 だった」は別)。値を読むために再評価しないこと — 再評価は clock を進め、イベントを消費します([コメント](../../src/core/userpublic/valuegraph.hpp#L101))。

**読む場所**: [`PublishResult`](../../src/core/userpublic/valuegraph.hpp#L98) → [`PublishedInstanceInputs`](../../src/core/userpublic/valuegraph.hpp#L116) → [`publishGraphValues()`](../../src/core/loader/scenegraphruntime.cpp#L145)。

### 難所 — `$pose/` のスカラー橋と 2 つの EvaluationState

**素朴に読むと**: 1 つの文書なのだから評価状態も 1 つで、animation 側と rendering 側が同じ状態を共有すればよい、と読めます。

**実際は**: 同じ文書が 2 つの区画に分かれ、**同じフレームの中で 1 段ずれて**評価されます。[`GraphExecutionPartitions`](../../src/project/graphpartition.hpp#L7) の `animation` は values より 1 段早く走る、という順序の宣言です([宣言とコメント](../../src/project/graphpartition.hpp#L10))。ただしこれは runtime の受け渡しではありません。animation フェーズは values 区画を**自分の `EvaluationState` で自分で評価し直し**([`evaluateLowered()` の呼び出し](../../src/core/ecs/predefined/animationsystem.cpp#L408))、その出力から `$pose/<node>/<port>` という名前を拾います。

この名前は区画分割のときに values 側の出力として作られる**宣言上の名前**で([分割側](../../src/project/graphpartition.cpp#L384))、綴りは両側が [`graphAnimationScalarOutput()`](../../src/project/graphpartition.cpp#L461) の 1 箇所を呼びます(instance 入力側は同じ理由で [`graphInstanceInputOutput()`](../../src/project/graphpartition.cpp#L465) の `$value/<入力>`)。**供給は一方向**で、逆流はありません。

その帰結として、`SceneLoader` は 1 つの文書に対して評価状態を 2 つ持ちます — [`graphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L150) と [`poseGraphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L154)。共有すると、replay 系の op(`value.trigger_clock` など)が 1 フレームに 2 回進みます。2 つは必ず一緒に動きます: 世代が変わるときは[両方を移送](../../src/core/loader/scenegraphruntime.cpp#L377)し、owner が変わったスコープは[両方を reset](../../src/core/loader/scenegraphruntime.cpp#L100) します。片方だけ残すと、姿勢側の clock だけが巻き戻ります。

**不変条件**: 2 つの評価状態を 1 本にまとめないこと。移送と reset を片側だけで行わないこと。animation 側から values 側へ値を書き戻す経路を作らないこと。

**読む場所**: [`graphpartition.hpp`](../../src/project/graphpartition.hpp#L7) → [`poseGraphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L154) → [消費側](../../src/core/ecs/predefined/animationsystem.cpp#L406)。

### 難所 — `--start-time` は「量子化された frame 番号」でしか engine に入らない

**素朴に読むと**: 「この時刻から出力する」なので、engine には秒が渡ってどこかで seek する、と読めます。

**実際は**: 秒が frame 番号になるのは [`parseStartFrame()`](../../src/player/main.cpp#L330) の 1 箇所だけで、`llround(seconds * fps)` が量子化です([規範コメント](../../src/player/main.cpp#L316))。**engine はフレームの間の時刻を渡されません**。parse 自体は [`std::stod()`](../../src/player/main.cpp#L334) を呼びますが、**消費した文字数を突き合わせる**ので([検査](../../src/player/main.cpp#L338))、綴り間違い・非有限・負のすべてが同じ [`offline.time_invalid@1` のメッセージ](../../src/player/main.cpp#L340) になります(「そのようなフレームは無い」という 1 つの答えだから、という理由がコメントにあります)。

seek が無い代わりに置かれているのが **replay prologue** です。[`loop.cpp` の該当分岐](../../src/core/appflow/loop.cpp#L674)が `start_frames − 1` フレームを capture 無しで回し、`offline: start_time=… start_frame=… replay=… frames=…` を stderr に出してから通常の capture ループへ入ります。理由はコメントが規範で、**フレームの見た目を決める状態は時計だけからは到達できない**(グラフの clock は `render()` の中でしか進まず、project のシステムは自分で dt を積分する)からです。

**不変条件**: 量子化を `floor` などへ変えないこと(同じ時刻指定が別のフレームを指し、連番出力の byte 比較が壊れます)。replay フレームで capture 以外の処理を省かないこと。3 つの失敗を別々の名前に分けないこと。`llround` の前の範囲の検査([その条件](../../src/player/main.cpp#L348))を外さないこと —— 秒の上限 3600([`kMaxOfflineStartSeconds`](../../src/player/main.cpp#L328)、`--fps` によらない)が無いと巨大な時刻で warm-up が終わらず、`long long` を超える frame では `llround` の結果が処理系定義になります。`double` は `LLONG_MAX` を 2^63 に丸めるので、範囲の端も `>=` で断っています。

**読む場所**: [`parseStartFrame()`](../../src/player/main.cpp#L330) → [`--start-time` の宣言](../../src/player/main.cpp#L392) → [replay prologue](../../src/core/appflow/loop.cpp#L674)。

### 難所 — 解決できない owner は警告を出して非アクティブになる、警告は 1 回だけ

**素朴に読むと**: `graph.instance` が指すオブジェクトが見つからなければ、毎フレーム警告が出るかエラーになる、と読めます。

**実際は**: **静かに inactive になります**。[`inactiveGraphScopes()`](../../src/core/loader/scenegraphruntime.cpp#L449) は解決できなかったスコープを非活性集合に入れるだけで、何も言いません。これは描画経路で毎フレーム呼ばれるからで、オブジェクトが再公開されている最中の正常な状態と区別が付かないためです。代わりに、グラフ世代の同期時に [`reportUnresolvedGraphOwners()`](../../src/core/loader/scenegraphruntime.cpp#L60) が「このシーンで一度も解決したことのないスコープ」だけを 1 回警告します。2 度目を出さないのは [`graph_owner_reported_scopes`](../../src/core/loader/scenegraphruntime.cpp#L71) への `emplace` の戻り値で、両方のメモはシーンのロードでだけクリアされます([理由のコメント](../../src/core/loader/scenegraphruntime.cpp#L76))。

これが v0 で最も踏みやすい無音の失敗で、**グラフはロードされているのに何も起きない**という形で出ます。以前はこれに「`graph.instance.owner` の authoring object id が保存 → 再読込を跨ぐと別個体を指しうる」穴が重なっていましたが、保存が `authoring_object_id` と `next_authoring_object_id` を書くようになって解消しました(`efb30dad`、[`resolveAuthoringIdentities()` の読み取り](../../src/project/sceneformat.cpp#L45)。OL-1 が `duplicate` と挿入の後の保存・再読込を `test/sceneformat_test.cpp` の `[ol1]` で確かめています。`docs/v0_freeze.md` §4.1)。

**不変条件**: 描画経路側で警告を出さないこと。メモをフレーム境界でクリアしないこと。「解決できない」を評価時のエラーへ格上げしないこと(再公開中の正常状態を殺します)。

**読む場所**: [`graphOwnerObject()`](../../src/core/loader/scenegraphruntime.cpp#L41) → [`reportUnresolvedGraphOwners()`](../../src/core/loader/scenegraphruntime.cpp#L60) → [`inactiveGraphScopes()`](../../src/core/loader/scenegraphruntime.cpp#L449)。

### 難所 — `pelican.pose@1` は意図して不透明

**素朴に読むと**: port を流れる型なのだから、姿勢のバイト列(joint の TRS 配列)が流れている、と読めます。

**実際は**: 流れるのは **staged pose のハンドルと、それが作られた `PoseLayout` の世代・フレーム revision** です([型の説明](../../src/project/posegraphmodule.hpp#L11))。バイトでないからこそ、別の layout 世代で作られた 2 つの姿勢を `memcpy` で混ぜる代わりに、名前で `pose.layout_mismatch@1` として弾けます([宣言](../../src/core/animation/posegraph.hpp#L86))。判定の材料はトレースにも出ており、sink ごとに `layout_identity` / `layout_generation` が記録されます([`SinkTrace`](../../src/core/animation/posegraphruntime.hpp#L117))。`pelican.rig@1` は、op が clip と出力 layout を解決するための rig 同一性です。

契約(型と op)が `src/project` 側にあるのも同じ理由です — カタログ、エディタ、そして**アニメーションバックエンドを外した build** の 3 者が同じ宣言を見るためで、評価だけが `src/core/animation` の仕事です。

**不変条件**: `pelican.pose@1` に bytes の意味を与えないこと。layout 世代の比較を「等しいはず」として省かないこと。契約を評価側へ移さないこと(OFF build とカタログが食い違います)。

**読む場所**: [`posegraphmodule.hpp`](../../src/project/posegraphmodule.hpp#L7) → [`PoseGraph::raiseLayoutMismatch()`](../../src/core/animation/posegraph.hpp#L86) → [`SinkTrace`](../../src/core/animation/posegraphruntime.hpp#L117)。

### 難所 — 再生パラメータの既定値を本当に持つのは 2 関数だけ

**素朴に読むと**: `speed` / `start_time` / `loop` の既定値は、契約側と評価側の両方に書いてある(あるいは構造体の初期化子が正)と読めます。

**実際は**: project 層の 4 関数が入口ですが、**既定値そのものを持っているのは 2 つ**です — [`poseClipSampleLoop()`](../../src/project/posegraphmodule.hpp#L35) が `true`、[`poseClipSampleExtractsRootMotion()`](../../src/project/posegraphmodule.hpp#L45) が `false`。[`poseClipSampleNumber()`](../../src/project/posegraphmodule.hpp#L33) は `double fallback` を**引数で受け取る**ので、`speed = 1.0` / `start_time = 0.0` のリテラルは呼び出し側に書かれています([契約側](../../src/project/posegraphmodule.cpp#L166) と[評価側](../../src/core/animation/posegraph.cpp#L107) の 2 か所)。

[`poseClipSampleRootNode()`](../../src/project/posegraphmodule.hpp#L49) には既定値がなく、不在・負・過大はすべて `pose.root_node_unknown@1` です。アニメーションバックエンドも同じ関数を通るので、node が「カタログにとっての意味」と「評価器にとっての意味」を持ち分けることはできません。`PoseGraphNode` 側のメンバ初期化子は同じ値を持っていますが、コメントが「既定値は `posegraphmodule.hpp` にある。ここでは言い直さない」と明記しています([`PoseGraphNode` のコメント](../../src/core/animation/posegraph.hpp#L35))。

エラーの使い分けも意図的です。`root_motion` に `"none"` / `"extract"` 以外を書いたときは**素の `invalid_argument`** で、名前付きの拒否は「rig と clip とオブジェクトが揃って初めて分かるもの」に予約されています([コメントの規範](../../src/project/posegraphmodule.hpp#L36))。`pose.root_node_unknown@1` は後者で、範囲外も layout に無いのも同じ名前です。

**不変条件**: 既定値を評価側やスキーマ側で言い直さないこと。文書だけで決まる誤りに名前付きコードを与えないこと(逆に、rig / clip / オブジェクトを要する誤りを素の例外にしないこと)。

**読む場所**: [`posegraphmodule.hpp`](../../src/project/posegraphmodule.hpp#L28) → [`PoseGraphNode` の 3 メンバ](../../src/core/animation/posegraph.hpp#L37) → [rig 依存の拒否](../../src/core/animation/posegraph.hpp#L101)。

### 難所 — 物理の契約の拒否は最後に投げ、editor はそれだけを commit する(RS-1)

**素朴に読むと**: runtime が候補を断ったら editor は必ず巻き戻す、また [`MaterialGenerationPlanner::validateCompatibility()`](../../src/core/material/materialgenerationplanner.cpp#L1232) の検査は見つけた順に投げる、と読めます。

**実際は**: 物理の描画の契約(color / depth の format・sample 数・local read・attachment state)の違いだけは、見つけてもすぐには投げません。最初の 1 件を `std::optional<MaterialPhysicalContractChanged>` に取っておき([取っておく所](../../src/core/material/materialgenerationplanner.cpp#L1239))、受け皿の検査(`requireEveryMaterialServed()`)と、全 material・全 pass の output schema と `ray_query` の検査が通った後に投げます([投げる所](../../src/core/material/materialgenerationplanner.cpp#L1322))。

型は [`MaterialPhysicalContractChanged`](../../src/core/material/materialphysicalcontractchanged.hpp#L46)(`std::runtime_error` の派生。`info()` が `{material_id, material, pass, detail[]}`)で、code は [`materialPhysicalContractChangedCode`](../../src/core/material/materialphysicalcontractchanged.hpp#L13) です。**この順があるので、editor はこの型を見たら「これが唯一の拒否だった」と読めます。**

editor の側では、runtime の完了が [`completeCommitTransaction()`](../../src/core/communication/renderconfigeditor.cpp#L7924) に戻ったとき、staged でも committed でもなく source commit も呼ばれていない結果の例外を、[`physicalContractRestartReason()`](../../src/core/communication/renderconfigeditor.cpp#L217) が rethrow して型を見ます。

この型のときだけ、editor 自身が ticket の preset の CAS(`CommitTransaction::verify_preset`。[`commitPending()` で取っておく所](../../src/core/communication/renderconfigeditor.cpp#L8388))と複数ファイルの commit(`commitRenderConfigCandidateDocuments`)を行い、`applied.committed = true`、`published_generation` を runtime の今の世代にして、成功の経路へ合流します([合流する所](../../src/core/communication/renderconfigeditor.cpp#L7973))。

結果に `restart_required` と `restart_reason` を付けるのは最後の 1 か所で、G-4 の `material_route_default_changed` より先に見ます([結果に載せる所](../../src/core/communication/renderconfigeditor.cpp#L8193))。runtime に行く ticket はすべて [`commitPending()`](../../src/core/communication/renderconfigeditor.cpp#L8224) → `apply_candidate` → `completeCommitTransaction()` を通るので、この 1 か所が graph の編集・`add_authored_pass`・restore などのすべてに効きます。`set_layout` だけの ticket(`source_only`)はここを通りません。

live の material を見るのは authored の経路だけです。[`Renderer::applyRenderPipelineAuthoringCandidate()`](../../src/core/vkcore/renderer.cpp#L3948) は `validate_live_materials = true`、ディスクからの [`reloadRenderPipelineFromDisk()`](../../src/core/vkcore/renderer.cpp#L3722) は reload service がいるとき `false` で、material の組み直しは reload service の companion reload(`MaterialContainer::prepareRuntimeGenerationReload()`)に任せます。

検査は flat の variant の `validate_prepared_generation` の中にあり、XR の variant の検査と `before_publish`(publish の直前の準備と source commit)はその後です([variant ごとに検査を呼ぶ所](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1527))。だから RS-1 の経路ではそれらは走りません。

**同じ枝に乗る 2 つ目の型(UD-1)**: [`MaterialPassesRemoved`](../../src/core/material/materialpassesremoved.hpp#L12)(`std::runtime_error` の派生、code は [`materialPassesRemovedCode`](../../src/core/material/materialpassesremoved.hpp#L7))は、material pass を 1 つも持たない候補が公開中の世代の描いていた route を落とすときに、[`requireEveryMaterialServed()` の最後](../../src/core/material/materialgenerationplanner.cpp#L915)で投げます。`physicalContractRestartReason()` がこれも `{code: "material_passes_removed", message}` に変える([その catch](../../src/core/communication/renderconfigeditor.cpp#L229))ので、editor は RS-1 と同じく commit して `restart_required` を返します。

- 投げるのは `validateCompatibility()` の**最初**の `requireEveryMaterialServed()` の中で、物理の契約のように「最後」ではありません。それでも隠れる拒否が無いのは、material pass の無い候補では後ろの material ごとの検査(output schema・`ray_query`・物理の契約)が見る binding を `resolveLiveMaterialBindings()` が返さないからです(コードの読み)。material pass の無い候補に material ごとの検査を足すなら、この順を見直します。
- `requireEveryMaterialServed()` は [`prepareReload()`](../../src/core/material/materialgenerationplanner.cpp#L1326) でも呼ばれるので、ディスクからの読み直し(companion reload)でもこの型が投げられ、候補世代を捨てて旧世代を保ちます。そちらには restart を返す相手がいません。
- [`loadRenderGraphVariantsFromConfigData()`](../../src/core/vkcore/renderer_config.cpp#L628) は、この型を `std::exception` の枝より前で受けて INFO の log だけを出して投げ直します。設定の誤りの log(`Failed to load main rendering configuration`)として出さないためです。
- 起動時は公開済みの世代が無い(`FrameGraphRuntimeContainer::snapshot()` が null)ので、この型は投げられず、material は受け皿の無いまま登録されます。

**不変条件**: 物理の契約の拒否より後ろで新しい検査を走らせないこと —— RS-1 の経路はそこで止まるので、その検査が見るはずの誤りを抱えた候補が commit されます。足すなら `validateCompatibility()` の中で、`physical_change` を投げる前に。editor で型付きの拒否以外を commit に回さないこと。

**読む場所**: [`materialphysicalcontractchanged.hpp`](../../src/core/material/materialphysicalcontractchanged.hpp#L39) → [`validateCompatibility()`](../../src/core/material/materialgenerationplanner.cpp#L1232) → [`physicalContractRestartReason()`](../../src/core/communication/renderconfigeditor.cpp#L217) → [`completeCommitTransaction()` の RS-1 の枝](../../src/core/communication/renderconfigeditor.cpp#L7973)。利用者から見た結果の形は manual の第13章 §13.5。

### 難所 — Studio の widget の破棄では、Impl が QWidget の基底より先に消える(FX-4)

**素朴に読むと**: 子の widget の signal から親の lambda への接続は、QObject が破棄のときに自動で外すので、destructor は `= default` でよい、と読めます。

**実際は**: `std::unique_ptr<Impl>` のメンバは、派生クラスの destructor の本体が終わった直後、`QWidget::~QWidget()` より先に消えます。その後の `QWidget::~QWidget()` が窓を閉じて焦点を外すので、焦点を持っていた欄(`QLineEdit` など)が `editingFinished` を出し、消えた Impl や binding を捕まえた lambda が走ります。QObject の自動の切断はその後なので間に合いません。Inspector では、数値・文字の欄の binding(`NumericBinding` / `TextBinding`)を Impl が持っています。対策は destructor の最初で、子から自分への接続を全部外すことです([`InspectorWidget::~InspectorWidget()`](../../src/devstudio/view/inspectorwidget.cpp#L2719)。event filter も外す)。

[`GraphAttachmentWidget::~GraphAttachmentWidget()`](../../src/devstudio/view/graphattachmentwidget.cpp#L392)(入力 port の欄)と [`MaterialAssignWidget::~MaterialAssignWidget()`](../../src/devstudio/view/materialassignwidget.cpp#L340) も同じ形です。test は `test/devstudio_inspector_widget_test.cpp` にあり、焦点のある欄・書きかけの欄・書きかけの入力 port を持つ widget を壊して、遅れた編集が送られないことを見ます。

**不変条件**: 子の欄の signal を、Impl やその中の binding を捕まえた lambda に繋ぐ widget は、destructor を `= default` にしないこと。destructor の最初で `findChildren<QObject *>()` の各子から `this` への接続を外します。

**読む場所**: [`InspectorWidget::~InspectorWidget()`](../../src/devstudio/view/inspectorwidget.cpp#L2719) → [`GraphAttachmentWidget::~GraphAttachmentWidget()`](../../src/devstudio/view/graphattachmentwidget.cpp#L392)。

### 難所 — `setItemWidget` で行に置いた widget を `delete` で直接消すと、view の editor に残る(FX-8)

**素朴に読むと**: `QTreeWidget::setItemWidget()` で行に置いた widget は普通の子 widget なので、`delete` すれば view からも外れる、と読めます。

**実際は**: `setItemWidget` は view の index widget(view が editor として持つ一覧)に登録します。widget を `delete` しても、その一覧の項目は残ります。次に木を `clear()` すると選択が変わって view が layout をやり直し、一覧に残った解放済みの widget を読みます。Frame Plan では、Backends と Physical plan の木の先頭に置く帯(band)がこれに当たり、Pin の後に plan を読み直すと Studio が 0xC0000005 で落ちていました。今は帯ごとに置いた木を覚えておき([帯を覚える所](../../src/devstudio/view/frameplanwidget.cpp#L3537))、消す前に `removeItemWidget()` で木に手放させてから `delete` します([`dropBands()`](../../src/devstudio/view/frameplanwidget.cpp#L614))。木を作り直す前に帯を消す順([`populate()` の中で呼ぶ所](../../src/devstudio/view/frameplanwidget.cpp#L4205))は今までどおりです。

**不変条件**: `setItemWidget` で置いた widget は、`removeItemWidget()` で view に外させてから消すこと。木を `clear()` するより前に外しておくこと。

**読む場所**: [`dropBands()`](../../src/devstudio/view/frameplanwidget.cpp#L614) → [帯を覚える所](../../src/devstudio/view/frameplanwidget.cpp#L3537)。利用者から見た Frame Plan の帯は manual 第15章 §15.6 の「backend の pin と物理計画・再起動」。

### 難所 — 利用者のデータの例外は Play を止めるが、DLL の reload 中の onInit の例外は reload の失敗のまま(RX-1、`faf9a484b`)

**素朴に読むと**: RX-1 の guard は「frame の中で投げられた例外を捕まえて Problems に出す」ものなので、`try { … } catch (const std::exception &)` で全部を受ければよく、behavior の `onInit` が投げたときも同じ扱いでよい、と読めます。

**実際は**: 受けるのは印の付いた例外だけで、`onInit` には 2 つの扱いがあります。

- **印**: 利用者のデータの不正だけを [`UserDataError`](../../src/core/userpublic/userdataerror.hpp#L6)(`std::runtime_error` の派生)で投げ、[`guardUserData()`](../../src/core/watch/userdataguard.hpp#L31) はこの型だけを受けます。engine の内部の不変条件や GPU の失敗は元の型のまま guard を抜け、今までどおり致命の境界に届きます(`std::exception` まで広げると、壊れた engine を動かし続けることになる)。受けた scope は [`failUserData()`](../../src/core/watch/problemlist.cpp#L500) が Problems の `runtime.user_data@1` を 1 行にし、[`beginUserData()`](../../src/core/watch/problemlist.cpp#L490) が同じデータの版の間はその scope を飛ばし、版が変われば行を消してもう一度試します。
- **ゲームの DLL の callback**: DLL の中の例外は印を持てないので、[`guardUserCode()`](../../src/core/watch/userdataguard.hpp#L43) は callback の境界でだけ `std::exception` を利用者のコードの不正と見なし、behavior 名を付けた `UserDataError` に変え、[`requestUserCodeStop()`](../../src/core/watch/problemlist.cpp#L521)(Play 中だけ印を立てる)を呼びます。[`invokeUpdate()`](../../src/core/gamelogic/behaviorarena.cpp#L901)・event・登録 system がこれを通ります。止めるのは frame の境目で、[`finishUserCodeStop()`](../../src/core/watch/problemlist.cpp#L530)([`updateFrameState()` の中、game code を走らせる phase の後で呼ぶ所](../../src/core/appflow/framephase.cpp#L278))が、RPC のある engine では `set_play_state` と同じ Stop の処理(`rpcserver.cpp` が `user_code_stop_handler` に入れる)を、RPC の無い engine では `edit` への切り替え(`internal::changePlayState`)を走らせます。behavior の Problems は Stop の後も残り、直すか外すと消えます。
- **`onInit`(Play の開始)**: [`invokeInit()`](../../src/core/gamelogic/behaviorarena.cpp#L873) は Problems に記録して Stop を頼んでから、`UserDataError` を投げ直します。activation の transaction の逆順の破棄と遅延した変更の rollback をそのまま使うためです。`set_play_state` は rollback された遷移を受けて `edit` を返し([RPC の catch](../../src/core/communication/rpcserver.cpp#L2326))、起動時の失敗は scene を dormant で読み直して editor を使える状態で残します([起動の catch](../../src/core/userpublic/pelican_core.cpp#L184))。
- **`onInit`(DLL の reload の中)**: [`isGameLogicReloadInProgress()`](../../src/core/gamelogic/gamelogicreload.cpp#L887) が真なら、`invokeInit()` は [guard を通さずに元の例外を投げます](../../src/core/gamelogic/behaviorarena.cpp#L881)。候補の activation と rollback は reload の transaction の持ち物で(WP162 の契約)、reload は失敗し、旧い DLL の runtime が動き続けます。ここで Problems の行や Stop の要求を残すと、戻った runtime を止めてしまいます(RX-1 の fix の commit の注記)。

**不変条件**: guard が受けるのは `UserDataError` だけ(`std::exception` への変換は DLL の callback の境界だけ)。Stop は frame の境目で 1 回。reload の transaction の中の `onInit` は guard を通さない。

**読む場所**: [`userdataguard.hpp`](../../src/core/watch/userdataguard.hpp#L9) → [`invokeInit()`](../../src/core/gamelogic/behaviorarena.cpp#L873) → [`finishUserCodeStop()`](../../src/core/watch/problemlist.cpp#L530)。test は `rx1_userdata_test` の「RX-1 unmarked engine exceptions cross the guard unchanged」ほか、`game_logic_abi_test` の「RX-1 behavior DLL onInit reload failure rolls back without stopping Play」、`pb1_problems_test` の「RX-1 authored runtime failures recover through RPC while frames and editor survive」(GPU)。行番号は着地前の `agent/rx1` のものです。

### 難所 — render の編集の失敗は、GPU の準備に入っていても検証の拒否なら Problems に載せない(CP-1b、`507a57ed8`)

**素朴に読むと**: CP-1 の本題は「編集は検証を通ったのに、GPU 側で適用できなかった」失敗を Problems に出すことなので、`applyRenderPipelineAuthoringCandidate()` の catch に来た失敗は全部記録すればよい、あるいは「GPU の準備に入った後の失敗」を適用の失敗と見ればよい、と読めます。

**実際は**: どちらも、検証で断った編集の行を Problems に残します。CP-1 は前者の形で、`studio_scenario_gui_graph_order`(`frame_node_order.conflicts_dependency@1`)と `studio_scenario_gui_rt_checks`(`pelican.ray_tracing.shader_record_abi_mismatch@1`)が「dialog で断った後も 1 error」で赤になりました。後者も、shader record の ABI の検査のように GPU の準備の途中で走る検証があるので同じ赤になります(コードの注記)。編集を断ったときは engine の状態が変わらず、送った client が答えで理由を見ているので、一覧に残すと次の編集で古くなるだけです(PB-1 の「入らないもの」)。

- 編集の catch は [`recordRenderPipelineAuthoringFailure()`](../../src/core/vkcore/renderer.cpp#L3927) を通し、[例外の型と code で分けます](../../src/core/vkcore/renderer.cpp#L3936): `vk::SystemError`・`std::bad_alloc`・`compute.descriptor_*` だけを source `edit_rpc` で記録し、ほかは答えの `error` に残すだけで、既にある行を足しも置き換えもしません。
- 答えを返す相手のいない失敗(staged の後の rollback、file watch の読み直し、`reload_render_documents` の拒否、起動時)は、この判定を通らずに今までどおり記録します。

**不変条件**: 編集の RPC で Problems の行を足すのは「適用の資源の失敗」だけ。判定は「どの段で起きたか」ではなく「何の失敗か」で行う。

**読む場所**: [`recordRenderPipelineAuthoringFailure()`](../../src/core/vkcore/renderer.cpp#L3927) → [編集の catch](../../src/core/vkcore/renderer.cpp#L4091)。本文は [第6章 §6.8](06_rendering_vulkan_shader.md) の「descriptor pool は世代の task から決め、batch が持つ」。test は `reloadtransaction_test` の「CP1 RPC edit refusals do not add or replace Problems」。

### 難所 — draw item が push する source index と、RT の AS の行の identity は別の値(VR-1)

**素朴に読むと**: draw item は「描く material の glTF の source index」を 1 つ持てば足り、push constant と RT の加速構造の行の両方がそれを読めばよい、と読めます。

**実際は**: 2 つの値を別に持ちます([`DrawItemSnapshot`](../../src/core/renderer/drawqueuebuilder.hpp#L140))。

- **push と VRM の絶対上書きの鍵**: `pipeline_material_key.source_material_index` は、shader が `pelicanPush.sourceMaterialIndex` で VRM の表情の絶対上書き(VRM の `materialColorBinds` / `textureTransformBinds`)の record と照合する鍵です。✅VR-1(`a4a314550`)から [`makeDrawItemSnapshot()`](../../src/core/renderer/polygoninstancecontainer.cpp#L370) は、実際に描く material の `rayBindingIdentity()` の asset が、その model の authored な asset の参照と同じで index を持つときだけその index を入れ、それ以外(project material、別の asset の material、由来の分からない material)は `noSourceMaterialIndex` にします([決める所](../../src/core/renderer/polygoninstancecontainer.cpp#L390))。前は group(元の glTF の material)の index のままだったので、部品に project material を当てても VRM の表情が base color と UV を glTF の値で上書きしていました。RT の alpha / UV の絶対上書き([`rayQueryGeometryInstances()` の鍵](../../src/core/renderer/polygoninstancecontainer.cpp#L1673))も同じ鍵で引きます。
- **RT の AS の行の identity**: `ray_source_material_index` は登録された material の `source_material_index` をそのまま持ち、AS の行の asset / index の selector はこちらを使います([行に渡す所](../../src/core/renderer/polygoninstancecontainer.cpp#L1761))。VR-1 の最初の形は push 用の値を AS の行にも流用し、由来が model と違う material で行が消えていました(VR-1 の fix round の報告)。

**不変条件**: push の鍵を AS の行の identity に使わない(逆も)。push の鍵は「この model の asset の material を描くときだけ」その index。group の index に戻さない。

**読む場所**: [`DrawItemSnapshot::ray_source_material_index` の注記](../../src/core/renderer/drawqueuebuilder.hpp#L147) → [`makeDrawItemSnapshot()`](../../src/core/renderer/polygoninstancecontainer.cpp#L370) → [`rayQueryGeometryInstances()`](../../src/core/renderer/polygoninstancecontainer.cpp#L1645)。test は `headless_render_test` の「VR1 vrm effective material controls raster and RT absolute overrides」(GPU)。

### 難所 — GPU 時間を比べる step と symbol の全数検査は、gate の中でだけ揺れる(既知の間欠)

**素朴に読むと**: 2 回測った frame の GPU 時間の大小や、`ctest` の時間の上限で赤になったら、製品の後退を疑えばよい、と読めます。

**実際は**: v0 を閉じる段で、製品を変えずに揺れる test が 2 つ残っています。

- **`studio_scenario_gui_build_optimize`**: step「the frame's GPU time went up」(scenario の最後ではなく、この後に Features tab の step が続く)は、Multisampling を 4 にした後の `get_status` の `gpu_timing.logical_frame_averages[0].average_total_ms` が、Multisampling を変える前の `[count3]` の step で覚えた値(`ms_count3`)より大きいことを `op: gt` で見ます([その step](../../test/studio_scenarios/gui_build_optimize.json#L500)、[覚える step](../../test/studio_scenarios/gui_build_optimize.json#L459))。平均は engine の履歴の窓([`gpu_timing_history_capacity` = 120](../../src/core/vkcore/rendertiming.hpp#L18))の上で取るので、差が小さいと測り方の揺れで逆になります(RX-1 の W1 で 0.218 ms と 0.226 ms の比較が赤。統合側の記録)。NVIDIA の driver が起動の直後の shader を数秒から十数秒後に最適化した code へ差し替える、という統合側の観察がありますが、この step の揺れの原因かは未確認です(コードからは確かめられない)。scenario の側で幅を持たせるか比べ方を変えるのが残りの仕事です。
- **`wp380c_production_has_no_fault_setter_symbol`**: production の archive(`pelican_core` / `pelican_project`)の全部の symbol を `dumpbin` で調べる test で([定義](../../test/CMakeLists.txt#L2697))、通った回で 75〜150 秒ほどかかり(上限を 300 秒にした再実行で 203 秒の回もある)、`-j16` の全数の中では 120 秒の上限を越えることがあります(単独で 300 秒でも越えた回あり。どれも各 WP の報告の数字)。test の定義は LABELS だけで TIMEOUT を持たないので、上限は gate の `ctest --timeout` です。assertion の失敗は出ていません。

**不変条件**: この 2 つが赤のときは、単独で流し直してから製品を疑う。揺れを「既知」とするのは記録のある間だけで、記録は `docs/v0_freeze.md` §6.1 にあります。

**読む場所**: [`gui_build_optimize.json`](../../test/studio_scenarios/gui_build_optimize.json#L459) の `[count3]` の step、`[msaa4]` の step と比較の step、[`rendertiming.hpp`](../../src/core/vkcore/rendertiming.hpp#L18)。

### 難所 — player を GUI subsystem で build すると、PowerShell の `$LASTEXITCODE` が終了を待たない(DS-1f)

**素朴に読むと**: 出荷する player を窓だけのアプリ(Windows の GUI subsystem)にしても、test は今までどおり `& pelican_player …; $LASTEXITCODE` で終了コードを読める、と読めます。

**実際は**: PowerShell は GUI subsystem の exe を `&` で起動しても終わりを待たずに戻るので、終了コードが取れません(DS-1 で player を GUI にした直後、`run_physics_trigger_behavior.ps1` などの test が赤になった。DS-1f の仕様)。そこで subsystem は [`PELICAN_PLAYER_GUI_SUBSYSTEM`](../../src/player/CMakeLists.txt#L1)(既定 OFF = console)の option になり、ON のときだけ `WIN32_EXECUTABLE` と MSVC の `/ENTRY:mainCRTStartup` を付けます。開発と test の build は console のままで、`pelican_cli dist-config` が出す設定だけが ON にします([設定に書く行](../../src/devcli/distconfig.cpp#L932))。ON の build で終了コードを読むには `System.Diagnostics.Process` の `WaitForExit()` + `ExitCode` か `Start-Process -Wait -PassThru` を使います(`run_ds1f_player_startup.ps1` がこの形で、PE の header の subsystem も読んで確かめる)。

MessageBox も同じ理由で絞ってあります。[`shouldShowPlayerFatalDialog()`](../../src/core/playerdiagnostics.hpp#L16) は、`--headless` / `--rpc` / `--embedded`、`PELICAN_NO_MESSAGEBOX=1`、console がある、stdout か stderr が pipe・file・文字 device のどれかに繋がっている、のどれか 1 つでも当てはまれば出しません。出すのは何にも繋がっていない手起動の player だけで、headless の test が MessageBox で止まることはありません。

**不変条件**: 終了コードを読む script は subsystem に依らない形(`WaitForExit` + `ExitCode`)で書く。`PELICAN_PLAYER_GUI_SUBSYSTEM` の既定を ON にしない(既存の ps1 の test が待たなくなる)。MessageBox を出す条件に「繋がっていない」を残す。

**読む場所**: [`src/player/CMakeLists.txt`](../../src/player/CMakeLists.txt#L1) → [`shouldShowPlayerFatalDialog()`](../../src/core/playerdiagnostics.hpp#L16) → `test/run_ds1f_player_startup.ps1`(test `player_arguments_ds1f_startup`)。

---

Pelican の複雑さは、ECS と Vulkan そのものよりも「compile-time 型情報を runtime table へ落とす境界」と「CPU 上の寿命を frame/GPU 上の寿命へ写す境界」に集まっています。その2か所では、便利な macro や RAII の表面だけでなく、登録時刻、pointer の有効期間、barrier、破棄順まで追うのが安全です。
