# 第3章 プロジェクトデータとロード

[索引へ戻る](README.md) / [前章](02_runtime_lifecycle.md)

## 3.1 純粋パーサとruntime bindingを分ける

データロードの基本形は二段です。

```text
JSON/テキスト
  ↓ src/project: 構文・schema・version・意味検証、正規化
Definition / Document / Result
  ↓ src/core: path・ECS・GPU・moduleへ結び付け
Runtime object
```

例:

| データ | 純粋層 | runtime層 |
|---|---|---|
| scene | [`normalizeSceneDataJson()`](../../src/project/sceneformat.cpp#L201) | [`SceneLoader::load()`](../../src/core/loader/scene.cpp#L261) |
| render feature | [`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L1505) | [`registerRenderingPassConfigData()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L133) |
| JSON-RPC | [`parseJsonRpcRequest()`](../../src/project/jsonrpc.cpp#L148) | [`RpcServer`](../../src/core/communication/rpcserver.hpp#L39) |
| asset manifest | [`parse/generate/verify`](../../src/project/assetsmanifest.hpp#L60) | [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L11) |
| material/surface | [`parseMaterialFormatJson()`](../../src/project/materialformat.cpp#L631)、[`parseSurfaceFormat()`](../../src/project/surfaceformat.cpp#L784) | runtime接続済み（WP116〜117, 122）。`.surface`は [`surfacecompiler`](../../src/core/shader/surfacecompiler.cpp) でGLSL/SPIR-V化されpipelineへ。`.material.json`は [`lowerMaterial()`](../../src/project/materiallowering.hpp#L82) → [`registerReloadableMaterialValuesFile()`](../../src/core/material/materialcontainer.hpp#L153) |

新形式を追加するときは、まず`src/project`へGPU非依存の受理/拒否規則を置き、次に`src/core`で実体化するのが既存パターンです。

## 3.2 ProjectSourceと設定の三段優先順位

[`ProjectSource`](../../src/core/loader/projectsrc.hpp#L7) は二種類の入力を保持します。

- `raw_data`: `PelicanCore(settings)`へ直接渡した上書きJSON
- `project_data`: playerが読んだ`project.json`全体

[`ProjectBasicConfig::ProjectBasicConfig()`](../../src/core/loader/basicconfig.cpp#L499) は次の三sourceを [`JsonLoader`](../../src/core/loader/basicconfig.cpp#L61) へ渡します。

1. `raw_data` — API/起動時の上書き
2. `project.json` の `basic_config`
3. `engine://default_config.json`

各fieldはこの順で最初に見つかった値が採用されます（[`JsonLoader::getVal()`](../../src/core/loader/basicconfig.cpp#L69)）。JSON object全体をdeep mergeするのではなく、必要fieldをpath単位で問い合わせる方式です。

構築時にwindow、framerate、seed、camera、各データファイル参照を値として取り込みます。その後、assets/rendering/UI/input JSON本文はgetterの初回呼び出し時に読み、`mutable optional<string>`へcacheします（[basicconfig.hpp](../../src/core/loader/basicconfig.hpp#L92)）。

### sceneだけはcacheではなくdocument

sceneは単なる文字列cacheから **[`AuthoringSceneDocument`](../../src/core/loader/authoringscenedocument.hpp#L81) へ格上げ**されました（WP149 / WP166）。保持しているのは次の四つです（[basicconfig.hpp#L86-L89](../../src/core/loader/basicconfig.hpp#L86)）。

```cpp
mutable std::optional<AuthoringSceneDocument> scene_document;
mutable std::optional<std::string> scene_baseline_digest;
mutable std::uint64_t next_scene_revision = 1;
mutable std::uint64_t next_authoring_object_id = 1;
```

そのため `sceneDataJson()` は**生ファイル文字列を返しません**（[basicconfig.cpp#L742](../../src/core/loader/basicconfig.cpp#L742)）。

```cpp
std::string ProjectBasicConfig::sceneDataJson() const {
    return sceneDocument().encodeSemantic();
}
```

返るのは正規化済みのsemantic encodeです。読み書きのAPIは次の通りです。

| API | 宣言 | 役割 |
|---|---|---|
| `sceneDocument()` | [basicconfig.cpp#L600](../../src/core/loader/basicconfig.cpp#L600) | 現在の改訂を取得（初回は遅延load） |
| `updateSceneDocument()` | [#L610](../../src/core/loader/basicconfig.cpp#L610) | scene v1バイト列で差し替え |
| `invalidateSceneDocument()` | [#L614](../../src/core/loader/basicconfig.cpp#L614) | 次回の再構築を強制 |
| `importSceneDocument()` | [#L619](../../src/core/loader/basicconfig.cpp#L619) | 外部由来のバイト列を取り込み、新しい `SceneRevision` を返す |
| `saveSceneDocument()` | [#L663](../../src/core/loader/basicconfig.cpp#L663) | ディスクへ書き戻し、`SceneSaveResult`（revision / digest / byte数）を返す |

保存の失敗は型付きです。[`SceneSaveErrorCode`](../../src/core/loader/basicconfig.hpp#L19) は `ExternalModification` / `Unavailable` / `IoFailure` の三つで、[`SceneSaveError`](../../src/core/loader/basicconfig.hpp#L25) が保持します。`scene_baseline_digest` と保存直前のディスク内容を突き合わせるため、**エディタの外でファイルが書き換わっていれば `ExternalModification` で拒否**します。

テスト用のフォールト注入点も宣言的に列挙されています。[`SceneSaveFaultPoint`](../../src/core/loader/basicconfig.hpp#L34) は `AfterEncode` / `AfterDiskDigest` / `AfterTemporaryWrite` / `AfterTemporaryValidation` / `AfterCachePrepare` / `BeforeReplace`、[`SceneImportFaultPoint`](../../src/core/loader/basicconfig.hpp#L43) は `AfterCandidatePrepare` / `AfterPublication` です。

> **設計決定:** 保存経路の中断点を実装内部の分岐ではなくenumとして公開することで、「どの中断点でもディスク上のsceneが壊れない」という性質をテストから網羅的に叩けます。列挙名がそのまま保存手順の段階名になっています。

## 3.3 PathResolver

宣言は [`pathresolver.hpp`](../../src/core/loader/pathresolver.hpp#L58)、中心実装は [`resolveRef()`](../../src/core/loader/pathresolver.cpp#L514) です。

### 返り値がpathだけではない理由

`ResolvedRef`は次のvariantです。

```cpp
std::variant<
    std::filesystem::path,
    EngineResourceId,
    ResolvedPathFragment,
    ResolvedEngineFragment
>
```

`engine://`はファイルシステム上のpathではなく、実行ファイルへ埋め込まれたresource IDです。また`#kind:name`や`#kind:/full/path`のfragmentを、コンテナ内要素のアドレスとして失わず伝えるためvariantになっています（[`parsePathReference()`](../../src/core/loader/pathresolver.cpp#L268)）。

### scheme別の処理

| 参照 | 解決先 | 保護 |
|---|---|---|
| `engine://id` | `EngineResourceId` | 登録IDだけを後段で許可 |
| `project://x` または相対`x` | project rootまたはasset store | root外escape拒否 |
| `user://x` | project名由来のuser root | user root外escape拒否 |
| CLI由来の絶対path | そのpath | `--allow-absolute-paths`必須 |
| project JSON内の絶対path | 不許可 | 常に拒否 |

`std::filesystem::weakly_canonical`後にroot包含判定を行うため、単純な`../`文字列検査より強い境界です（[`resolveRef()`のproject処理](../../src/core/loader/pathresolver.cpp#L565)）。

### asset store

[`PathResolver::setup()`](../../src/core/loader/pathresolver.cpp#L310) は`project.json.asset_stores`を読みます。

- 各storeは論理`mount`と実rootを持つ。
- mount同士の重なりを拒否。
- `.pelican/local.json`で実rootだけをローカル上書き可能。
- 上書きはprojectで宣言済みのstore名だけ。
- 実root同士の包含/重なりも拒否。
- optional manifestのpathはproject root内に制限。

相対参照がmount prefixに一致すると、project rootではなくstore rootへ付け替えます（[`asset store解決`](../../src/core/loader/pathresolver.cpp#L594)）。これにより、VCS上の論理pathは固定したまま、大容量assetの実配置を開発者ごとに変えられます。

### engine resource

shader、feature JSON、default config、debug fontなどは [`resources/CMakeLists.txt`](../../src/core/resources/CMakeLists.txt#L1) で`battery-embed`へ登録され、[`engineResource()`](../../src/core/loader/engineresources.cpp#L110) がIDを実データへ変換します。

resource追加には次の三箇所が必要です。

1. `b_embed(...)`または`embed_shader(...)`
2. `registered_ids`
3. `engineResource()`のdispatch

一つでも漏れるとビルドまたはruntime lookupが失敗します。これは[第9章](09_black_magic_and_gotchas.md)でも注意点として扱います。

## 3.4 asset manifestの起動検証

playerはcore起動前に [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L11) を呼びます。純粋な実作業は [`verifyAssetsManifest()`](../../src/project/assetsmanifest.cpp#L532) です。

- file size、SHA-256、missing/extra/case mismatch/unreadableを分類。
- `.pelican/assets-hash-cache.json`で再hashを減らす。
- `--strict-assets`なら通常warningの差異をerrorへ昇格。
- errorがあれば`PelicanCore::run()`へ入らず終了。

manifestの生成も同じ`src/project/assetsmanifest.cpp`にあり、`pelican_cli assets manifest`から利用されます。

## 3.5 Sceneのロード

sceneは実装を追う価値の高い、純粋層とruntime層の典型です。

### 1. envelopeと構文を正規化

[`normalizeSceneDataJson()`](../../src/project/sceneformat.cpp#L201) が次を検証します。

- `schema == "pelican.scene"`
- `version == 1`
- `scenes`がobject
- scene ID、object名、component名が`[a-zA-Z0-9_]`
- object名がscene内で一意
- 各objectに`components` arrayが存在
- `parent`（親子transform）が識別子であること。未知parent・曖昧parent・循環はエラー（[sceneformat.cpp](../../src/project/sceneformat.cpp#L103)、fixture: `scene_unknown_parent` / `scene_parent_cycle` / `scene_ambiguous_parent`）
- v1で禁止された旧`lights` fieldを拒否

この時点ではECS型やGPUには触れません。

### 2. runtime用にcomponentを分類

[`prepareSceneBindings()`](../../src/core/loader/scene.cpp#L91) はcomponentを四系統へ分けます。

- `light`: ECSへ入れず`LightLoadEntry`へ
- `collider`: `ColliderComponent`として検証し、後で`PhysWorld`へ。`PELICAN_WITH_PHYSICS` OFFのビルドでは、colliderを含むsceneは明示エラーです（[scene.cpp#L300](../../src/core/loader/scene.cpp#L300)）
- `behavior`: ECSへ入れず、[`BehaviorAttachmentArena`](../../src/core/gamelogic/behaviorarena.hpp#L109) へ（[scene.cpp#L155](../../src/core/loader/scene.cpp#L155) で特別扱い）
- その他: `ComponentInfoManager`で文字列名から`ComponentId`へ

このためsceneの見た目はcomponent配列でも、現在のruntime実装ではlight/collider/behaviorがECS Chunkに保存されるわけではありません。

#### behaviorコンポーネント ✅実装済み

behaviorはobject単位に束ねてから渡します（[scene.cpp#L281-L286](../../src/core/loader/scene.cpp#L281)）。

```cpp
const auto behavior_availability = game_logic_status.loaded
    ? BehaviorRegistryAvailability::active
    : BehaviorRegistryAvailability::dll_unavailable;
auto behavior_entries = prepareSceneBehaviorAttachments(objects, behavior_availability);
auto ecs_objects = prepareSceneBindings(objects, component_info_manager, light_entries,
                                        std::move(behavior_entries));
```

受理規則は [`prepareSceneBehaviorAttachments()`](../../src/core/gamelogic/behaviorarena.cpp#L70) にあります。

| 状況 | 結果 |
|---|---|
| `"type"` が無い / 文字列でない / 空文字列 | エラー `behavior on object '<name>' requires a non-empty string type` |
| DLLロード済みで、未登録の型名 | エラー `Unknown behavior type '<type>' on object '<name>'` |
| DLL不在で、未登録の型名 | warningを出して **pending** 扱いで続行 |

公開は `arena.publishSceneAttachments(std::move(bound_behaviors))`（[scene.cpp#L451](../../src/core/loader/scene.cpp#L451)）で、失敗した場合は [`clearRuntimeScene()`](../../src/core/loader/scene.cpp#L486) してからrethrowします（[同 #L454](../../src/core/loader/scene.cpp#L454)）。

> **設計決定:** game logic DLLが無い状態でsceneを開くのは、ツールや検証では正常なケースです。そこで「DLLが有るのに型名が引けない」ときだけfailにし、DLL不在は保留にしています。ロード可否がビルド構成に依存してぶれない、という線引きです。

実物のscene JSONは次の形です（[`test/run_physics_trigger_behavior.ps1`](../../test/run_physics_trigger_behavior.ps1) が書き出すfixtureからの引用）。

```json
{"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
  {"name":"TriggerZone","components":[
    {"name":"transform","pos":[0,0,0]},
    {"name":"collider","shape":"sphere","radius":1.0,"trigger":true},
    {"name":"behavior","type":"wp179_trigger_behavior"}]},
  {"name":"Target","components":[
    {"name":"transform","pos":[1,0,0]},
    {"name":"collider","shape":"sphere","radius":1.0}]}]}}}
```

### 3. 旧sceneを破棄して新sceneを構築

[`SceneLoader::load()`](../../src/core/loader/scene.cpp#L261) は正規化と事前準備が成功した後、[`clearRuntimeScene()`](../../src/core/loader/scene.cpp#L486) を呼びます。

```text
object bindingをclear
→ PhysWorldをclear
→ ECS entityを全削除
→ PolygonInstanceContainerをclear
```

その後、light、camera、ECS object、collider、behaviorを作ります。ECS objectは [`GameObjects::createWithComponents()`](../../src/core/loader/scene.cpp#L314) のpopulate callback内でJSONを各Componentへロードします（transient glTF経路は[同 #L642](../../src/core/loader/scene.cpp#L642)）。

### 4. authored local TRSのworld投影

ECS objectを作った後、`SceneLoaded` を配送する**前**に、親子transformのworld解決を済ませます（[scene.cpp#L357](../../src/core/loader/scene.cpp#L357)）。

```cpp
// Project authored local TRS to world before any SceneLoaded observer can
// query the runtime. This is the same recurrence as LocalTransformSystem.
```

これは `LocalTransformSystem` と同じ漸化式です。したがって `SceneLoaded` を受け取ったコードは、まだ1フレームも回っていない時点でも world transform を問い合わせられます。

### 5. 名前binding

名前付きかつ`transform`を持つobjectだけが `object_bindings`へ入ります（[`bindObjectTransform()`](../../src/core/loader/scene.cpp#L512)、呼び出しは [#L329](../../src/core/loader/scene.cpp#L329) / [#L424](../../src/core/loader/scene.cpp#L424)）。RPC、camera controller、physicsはこの名前から世代付き`GameObjectId`を引き直します。

bindingは生ポインタを保持しません。アクセス時に`tryComponent<TransformComponent>()`でIDのgenerationとComponent存在を再検証します。

### 6. SceneLoaded event

ロード完了後に [`SceneLoaded`](../../src/core/userpublic/events.hpp#L9) をemitします（[`SceneLoader::load()`末尾](../../src/core/loader/scene.cpp#L460)）。フレーム末尾のpending loadからemitされた場合、eventは次フレーム冒頭に届きます。

## 3.6 即時loadと要求load

- `SceneLoader::load()`はその場で全sceneを置換。
- [`requestLoad()`](../../src/core/loader/scene.cpp#L463) は存在確認後、scene IDだけをpendingへ保存。
- [`applyPendingLoad()`](../../src/core/loader/scene.cpp#L472) はフレームのゲーム更新末尾で実際にload。

`GameContext::loadScene()`はrequest型です。RPCの`load_scene`は即時loadです。この違いは、ゲームSystem update中にECS全削除が起きないようにするためです。

## 3.7 ComponentのJSONロード

sceneのcomponent名から型への変換は [`ComponentInfoManager`](../../src/core/ecs/componentinfo.hpp#L34) が担います。

1. runtime登録時に `name → ComponentId` を保存。
2. `ComponentId → ComponentInfo`からJSON loader callbackを得る。
3. [`ComponentInfoManager::loadByJson()`](../../src/core/ecs/componentinfo.cpp#L109) が`JsonArchiveLoader`を作る。
4. Componentの`ref(ar)`を型消去callback越しに呼ぶ。

`ref()`を持たないComponentはscene JSONからロードできず、明示エラーになります。JSON archiveの対応型は [`jsonarchive.hpp`](../../src/core/userpublic/serialize/jsonarchive.hpp#L10) に固定されています。

## 3.8 Asset modelから描画instanceまで

[`ModelAssetContainer`](../../src/core/asset/model.hpp#L21) は`asset_data.json.models`を読み、拡張子に応じて [`GltfLoader::loadGltf()`](../../src/core/model/gltf.cpp#L2003) / [`loadGltfBinary()`](../../src/core/model/gltf.cpp#L1993) を呼び、名前から`ModelTemplate`へmapします。受理拡張子は`.glb`/`.gltf`に加え **`.vrm`** です（[asset/model.cpp](../../src/core/asset/model.cpp#L210)）。glTF scene fragmentの抽出には [`loadGltfBinarySceneNode()`](../../src/core/model/gltf.cpp#L1998) が使われます。

これらの `loadGltf*()` は、現在は三段APIのラッパです（[`gltf.hpp`](../../src/core/model/gltf.hpp#L29)）。

| 段 | 関数 | 性質 |
|---|---|---|
| prepare | [`prepareGltf()`](../../src/core/model/gltf.cpp#L1923) / [`prepareGltfBinary()`](../../src/core/model/gltf.cpp#L1918) / [`prepareGltfBinarySceneNode()`](../../src/core/model/gltf.cpp#L1928) | ファイル読み取りとparseまで |
| inspect | [`inspect()`](../../src/core/model/gltf.cpp#L1978) | **副作用なし**の候補パス。fragment解決と検証だけ |
| commit | [`commit()`](../../src/core/model/gltf.cpp#L1939) | GPU資源を確保して `ModelTemplate` を作る |

glTFロードの大まかな変換は次です。

```text
tinygltf Model
→ accessor/bufferからCommonPolygonVertDataを構築
→ VertBufContainerの大きなGPU vertex/index poolへ追記
→ textureをMaterialContainerへ登録
→ materialごとにPrimitiveRefInfoをまとめる
→ ModelTemplate
```

執筆基準時点から要素が増えています。VRM semanticデコード（[`vrmsemantic.hpp`](../../src/core/model/vrmsemantic.hpp) — humanoid bone / expression / lookAt / firstPerson、WP111）、morph target（[`morphtarget.hpp`](../../src/core/model/morphtarget.hpp)、WP121）、skeletal animation（[`skeletalanimation.hpp`](../../src/core/model/skeletalanimation.hpp)）、KTX2テクスチャ（[`loader/ktx2.hpp`](../../src/core/loader/ktx2.hpp)、BC5/BC7 fixtureあり）、atlas asset（[`asset/atlasasset.hpp`](../../src/core/asset/atlasasset.hpp) + [`renderer/atlasassetresource.hpp`](../../src/core/renderer/atlasassetresource.hpp)）です。モデルのhot reload（HR2-G、WP110）に伴い、世代管理は [`MaterialContainer::releaseModelResources()`](../../src/core/material/materialcontainer.hpp#L144) が担います。

sceneの`SimpleModelViewComponent`は初期化時にdirtyになり、内部 [`SimpleModelViewUpdateSystem`](../../src/core/ecs/predefined/modelviewupdatesystem.cpp#L23) がmodel名からtemplateを引き、`PolygonInstanceContainer`へinstanceを置きます。

### `ModelInstanceId` はSlotMapハンドル（WP146）

`ModelInstanceId` は単なる整数の添字ではありません（[`modelinstance.hpp#L11-L19`](../../src/core/renderer/modelinstance.hpp#L11)）。

```cpp
struct ModelInstanceId {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
    std::uint64_t scene_epoch = 0;

    auto operator<=>(const ModelInstanceId &) const = default;
};

inline constexpr ModelInstanceId invalidModelInstanceId{};
```

文字列化は [`toString(id)`](../../src/core/renderer/modelinstance.hpp#L21) で `"index:generation@scene_epoch"` になります。ログやRPCの応答でこの形を見たらmodel instanceのハンドルです。

`PolygonInstanceContainer` 側はSlotMapとして `instance_generations` / `instance_alive` / `free_instance_indices` / `live_instance_count` / `scene_epoch` を持ちます（[polygoninstancecontainer.hpp#L241-L245](../../src/core/renderer/polygoninstancecontainer.hpp#L241)）。

> **設計決定:** slot identityは animation generation やmodel assetのcontent revisionから独立しています。`scene_epoch` が進むのは **model-instance slotが死んだとき**だけで、モデルのhot reloadでは進みません。これにより「アセットを差し替えても、生きているinstanceハンドルは有効なまま」という性質が保てます。

API面の主な変化は次の通りです。

| API | 位置 | 内容 |
|---|---|---|
| `removeModelInstance()` | [#L298](../../src/core/renderer/polygoninstancecontainer.hpp#L298) | 戻り値が `void` → **`bool`** |
| `preflightModelInstance()` | [#L292](../../src/core/renderer/polygoninstancecontainer.hpp#L292) | 確保前の容量チェック（const） |
| `stageModelInstance()` | [#L293](../../src/core/renderer/polygoninstancecontainer.hpp#L293) | [`StagedModelInstance`](../../src/core/renderer/polygoninstancecontainer.hpp#L204) を返す |
| `publishModelInstance()` | [#L296](../../src/core/renderer/polygoninstancecontainer.hpp#L296) | `noexcept` の単一公開点 |
| `prepareTrs()` / `publishPreparedTrs()` / `rollbackPreparedTrs()` | [#L310-L313](../../src/core/renderer/polygoninstancecontainer.hpp#L310) | TRS更新のprepare/publish |
| `isModelInstanceAlive()` | [#L317](../../src/core/renderer/polygoninstancecontainer.hpp#L317) | 生存判定 |
| `instanceCountForTesting()` | [#L345](../../src/core/renderer/polygoninstancecontainer.hpp#L345) | **live数**（`live_instance_count`）を返す |
| `slotCountForTesting()` | [#L346](../../src/core/renderer/polygoninstancecontainer.hpp#L346) | スロット総数 |

テストは [`test/modelinstance_slotmap_test.cpp`](../../test/modelinstance_slotmap_test.cpp)（GPUラベル）です。

### `load_gltf` はトランザクション（WP144）

RPCの `load_gltf` が使う [`SceneLoader::loadTransientGltf()`](../../src/core/loader/scene.cpp#L586) は、「**割り当てを全部公開の前に済ませ、公開点を1箇所に絞る**」形で書かれています。

1. 名前bindingのhash nodeを先に `extract()` して確保（[scene.cpp#L600-L607](../../src/core/loader/scene.cpp#L600)）。コメント通り、エンティティ生成後にこのnodeを差し込む操作は割り当てを伴わないため、トランザクションを分割できません。
2. `prepareGltf*()` → [`inspect()`](../../src/core/model/gltf.cpp#L1978) の副作用なし候補パス → [`preflightModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L292) で、**model固有のVulkan資源を1つも確保する前に**容量超過を拒否します。
3. [`commit()`](../../src/core/model/gltf.cpp#L1939) でGPU資源を確保し、`stageModelInstance()` でstagingします。
4. エンティティ生成がthrowしたら `releaseModelGpuResources()` して `transient_models.pop_back()` します。
5. 単一公開点（[scene.cpp#L652-L654](../../src/core/loader/scene.cpp#L652)）。

```cpp
// No operation below allocates: this is the single publication point for
// the entity's slot, draw commands, resources, and optional name.
instances.publishModelInstance(std::move(staged_instance));
```

`SceneLoader` には [`transient_models`](../../src/core/loader/scene.hpp#L32) / [`runtime_only_changes`](../../src/core/loader/scene.hpp#L35) / [`releaseTransientModels()`](../../src/core/loader/scene.hpp#L39) / [`hasRuntimeOnlyChanges()`](../../src/core/loader/scene.hpp#L54) が追加されています。`runtime_only_changes` は編集RPC側で「これはruntime専用の変更なので保存できない」と判定するために使います。

## 3.9 その他の純粋形式

### render feature

[`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L1505) はfeature JSONを順番に読み、render target、buffer、pass、compute taskを追加し、限定的なoverrideを適用します。名前衝突、曖昧anchor、未知override fieldは即時エラーです。shader defineも重複排除して集約します。

### material / surface

[`parseMaterialFormatJson()`](../../src/project/materialformat.cpp#L631) はmaterial v1とoptional surface catalogの整合性を検証します。[`parseSurfaceFormat()`](../../src/project/surfaceformat.cpp#L784) はfront matter風の宣言部とshader codeを分け、parameter/texture/screen inputを構造化します。

これらはruntime接続済みです（WP116〜117, 122）。純粋層はparseと [`lowerMaterial()`](../../src/project/materiallowering.hpp#L82)（OpenPBR写像は [`openpbrmapping.hpp`](../../src/project/openpbrmapping.hpp)）まで、core層はsurface compile（[`surfacecompiler.cpp`](../../src/core/shader/surfacecompiler.cpp)）とvalues SSBO登録（[`registerReloadableMaterialValuesFile()`](../../src/core/material/materialcontainer.hpp#L153)）を担います。hot reloadは `MaterialValuesReloadHandler` / `TextureReloadHandler`（[src/core/material/](../../src/core/material)）です。

material bindingは **USD pathキー**を持ちます（[`materialformat.hpp`](../../src/project/materialformat.hpp#L45) の `usd_path`、[`modeltemplate.cpp`](../../src/core/model/modeltemplate.cpp#L61) で重複/未解決を検証）。これはU-USDレーン（WP119/124）の観測境界です。

### import manifest / import rules

[`parseImportManifestJson()`](../../src/project/importmanifest.cpp#L249) はDCC納品物のtool/source/outputとSHA-256、対応schemaを検証します。runtime playerではなく、[`pelican_cli import`](../../src/devcli/importcommand.cpp#L358) が主利用者です。加えて [`importrules.hpp`](../../src/project/importrules.hpp) がmatch/recipe/defaultsの3層優先を持つルールベースimport（`pelican_cli import --rules`、[importcommand.cpp](../../src/devcli/importcommand.cpp#L362)）を提供します。

### JSON-RPC

[`parseJsonRpcRequest()`](../../src/project/jsonrpc.cpp#L148) はtransportに依存せずJSON-RPC 2.0 envelopeを検証します。batchとnotificationは現状不許可です。実メソッドdispatchだけがcore側にあります。

## 3.10 データロードを変更するときのチェック

1. schema/version gateを純粋層に置いたか。
2. pathを文字列連結せず`PathResolver`へ通したか。
3. project由来の絶対pathやroot escapeを許していないか。
4. parse成功前に旧runtime状態を壊していないか。
5. 型名/ID/名前bindingの寿命がscene遷移後も安全か。
6. valid/invalid fixtureとruntime binding testの両方を追加したか。
7. 形式パーサ実装とruntime接続済み範囲を混同していないか。
8. componentを増やすなら、§3.11 のcodec五点セットを揃えたか。

## 3.11 Component codec（authored ↔ canonical ↔ runtime） ✅実装済み

componentのJSONをどう受理し、どう書き戻し、どうruntimeへ当てるかの**正は [`componentcodec.hpp`](../../src/core/loader/componentcodec.hpp#L96) に移りました**（WP151）。scene loaderやエディタは、それぞれ独自にJSONを解釈するのではなく、このテーブルを引きます。

```cpp
struct ComponentCodec {
    using DecodeAuthored = ComponentCodecValue (*)(const nlohmann::json &);
    using EncodeCanonical = nlohmann::ordered_json (*)(const ComponentCodecValue &);
    using Schema = std::span<const StructFieldSchema> (*)();
    using RuntimeApply = void (*)(const ComponentCodecValue &, void *);
    using RuntimeProject = ComponentCodecValue (*)(const void *);

    std::string_view name;
    ComponentCodecRuntimeKind runtime_kind;
    ...
};
```

この五つ（decode / encode / schema / apply / project）が**全codecに必須**です。値の運搬は `ComponentCodecValue = std::any` です。

`runtime_apply` と `runtime_project` がどの型を指すかは [`ComponentCodecRuntimeKind`](../../src/core/loader/componentcodec.hpp#L19) が決めます。ヘッダのコメントが規範です — transform は `TransformCodecTarget`、その他のECS codecはそのcomponent自身、camera/light は `CameraCodecData` / `LightCodecData`、collider は `ColliderComponent` です。

現在の登録は7種です（[componentcodec.cpp#L856-L871](../../src/core/loader/componentcodec.cpp#L856)）。

| codec名 | `runtime_kind` |
|---|---|
| `transform` | `Ecs` |
| `simplemodelview` | `Ecs` |
| `camera` | `Camera` |
| `light` | `Light` |
| `collider` | `Collider` |
| `animation` | `Ecs` |
| `sprite_view` | `Ecs` |

transformだけは専用の投影関数 [`projectTransformRuntimeJson()`](../../src/core/loader/componentcodec.hpp#L125) を持ちます。

> **設計決定:** transformの「authored local TRS」と「表示専用のworld投影」を混同しない、という要件をコメントで明文化したうえで別関数に分けています。エディタが表示している値と、保存される値が同じものだと誤解しないための境界です。

未登録の名前を問い合わせても例外にはならず、[`componentCodecQueryMetadata()`](../../src/core/loader/componentcodec.hpp#L121) が [`ComponentCodecState::Missing`](../../src/core/loader/componentcodec.hpp#L26) を返します。エディタは「知らないcomponentは編集不可として表示する」ことができ、未知componentを含むsceneを開けなくなることがありません。

テストは [`test/componentcodec_test.cpp`](../../test/componentcodec_test.cpp)、fixtureは [`test/fixtures/component_codec/`](../../test/fixtures/component_codec) の `valid.json` / `invalid.json` です。

## 3.12 AuthoringSceneDocument ✅実装済み

§3.2 で触れたsceneのdocument化の中身です。宣言は [`authoringscenedocument.hpp`](../../src/core/loader/authoringscenedocument.hpp#L81) です。

### load とidentity

[`AuthoringSceneDocument::load(scene_v1_bytes, revision, first_authoring_object_id)`](../../src/core/loader/authoringscenedocument.hpp#L98) が、`SceneRevision` とセッション内で安定な [`AuthoringObjectId`](../../src/core/loader/authoringscenedocument.hpp#L24) を割り当てます。`AuthoringObjectId` は名前でも配列添字でもないため、objectをrenameしても並べ替えても、エディタ側の参照は切れません。

### query

[`query()`](../../src/core/loader/authoringscenedocument.hpp#L108) は三層のviewを返します。

| 型 | 内容 |
|---|---|
| [`AuthoringSceneView`](../../src/core/loader/authoringscenedocument.hpp#L46) | scene ID、authored JSON、object列 |
| [`AuthoringObjectView`](../../src/core/loader/authoringscenedocument.hpp#L34) | `AuthoringObjectId`、宣言順、name/parent、runtime entity ID、component列 |
| [`AuthoringComponentView`](../../src/core/loader/authoringscenedocument.hpp#L26) | 宣言順、authored JSON、`ComponentCodecQueryMetadata` |

componentごとに §3.11 のcodec metadataが付くので、「宣言としては存在するが、このビルドでは編集できない」状態をそのまま表現できます。

### stage と構造編集

改訂の作り方は二種類あります。

- [`stage(raw_document, revision)`](../../src/core/loader/authoringscenedocument.hpp#L114) — **オブジェクト宣言のidentityを固定したまま**の未公開改訂。component配列やparentエッジは変えられますが、object宣言のidentityは動きません。
- [`structuralStage()`](../../src/core/loader/authoringscenedocument.hpp#L116) → [`AuthoringSceneDocumentStage`](../../src/core/loader/authoringscenedocument.hpp#L130) — object配列そのものを触る唯一の経路。`insertObject` / `removeObject` / `restoreObject` / `renameObject` / `reorderObject` を持ち、`finish(revision) &&` で確定します。変更種別は [`AuthoringStructuralChangeKind`](../../src/core/loader/authoringscenedocument.hpp#L63) の `Insert` / `Remove` / `Restore` / `Rename` / `Reorder` です。

> **設計決定:** `removeObject()` は破棄ではなく [`AuthoringObjectClosure`](../../src/core/loader/authoringscenedocument.hpp#L54) を**返します**。closureは同じ `AuthoringObjectId` を、保存した宣言区間の中へ復元できる無損失な記録です。ヘッダのコメント通り、object配列の変更をこれらの操作だけに限ることで、metadataのidentity列とauthored JSONの並びがずれないことを型で保証しています。

fixtureは [`test/fixtures/authoring_scene/multi_scene_roundtrip.json`](../../test/fixtures/authoring_scene/multi_scene_roundtrip.json) です。

## 3.13 `.vrma`（VRM Animation）コンテナ ✅実装済み

WP176〜WP178で入った、アニメーションクリップの交換形式です。段は三つあります — decode（WP176） → retarget（WP177） → `AnimationSource` としての登録（WP178）。decodeとretargetが作る中間表現そのものは `AnimationSource` ではありませんが、retarget済みクリップはWP178でanim_graph/ABIへpublishされます（後述）。

### decode

入口は [`decodeVrmaAnimation()`](../../src/core/loader/vrmadecoder.hpp#L25) と [`loadVrmaAnimation()`](../../src/core/loader/vrmadecoder.hpp#L32) です。

- `container_name` は**エイリアスゲート**を兼ね、`.vrma` 拡張子が必須です。
- [`VrmaDecodeOptions`](../../src/core/loader/vrmadecoder.hpp#L13) の `import_profile` と `tool_version` は**必須のprovenance**です（`source_uri` は省略時にcontainer名で埋まります）。
- 正規に選ぶのは最初のアニメーションで、`#animation/0` として識別します。
- バイトは1度だけ読み、**同じバイト列をhashしてdecode**します。provenanceが実際にdecodeした内容と食い違わないための仕様です。

### データ型

[`vrmaanimation.hpp`](../../src/core/model/vrmaanimation.hpp) にあります。

| 型 | 位置 |
|---|---|
| [`VrmaClip`](../../src/core/model/vrmaanimation.hpp#L87) | クリップ本体 |
| [`VrmaBodyChannel`](../../src/core/model/vrmaanimation.hpp#L35) / [`VrmaExpressionChannel`](../../src/core/model/vrmaanimation.hpp#L42) / [`VrmaGazeChannel`](../../src/core/model/vrmaanimation.hpp#L49) | チャンネル |
| [`VrmaSourceRig`](../../src/core/model/vrmaanimation.hpp#L70) / [`VrmaClipMetadata`](../../src/core/model/vrmaanimation.hpp#L75) | 元リグとprovenance（`content_sha256` を含む） |

[`VrmaInterpolation`](../../src/core/model/vrmaanimation.hpp#L12) は `linear` / `step` / `cubic_spline` です。値はxyzw格納で、translationは `component_count == 3`、rotationは `4`。cubic splineの接線は `in_tangents` / `out_tangents` として別配列に保たれ、glTFのアニメーションデータを落としません。

> **設計決定:** ヘッダのコメントが規範です — VRMA-C0 は decode/storage のみを持ち、root motion delta も target-rig / application state も**意図的に存在しません**。hips translationは通常のbody channelのままです。

### retarget

[`retargetVrmaClip()`](../../src/core/animation/vrmaretarget.hpp#L124) が、target rigのローカル座標系へ写した中間クリップを作ります。

- プロファイルはバージョン付きです（[`vrmaRetargetProfileVersion = 1`](../../src/core/animation/vrmaretarget.hpp#L16)）。
- [`VrmaRootMotionPolicy`](../../src/core/animation/vrmaretarget.hpp#L21) は現在 `preserve_hips_translation` のみです。抽出モードを持たないのは、後のプロファイルがv1データを読み替えずに方針を足せるようにするための、意図的な空きスロットです。
- 出力 [`VrmaRetargetedClip`](../../src/core/animation/vrmaretarget.hpp#L108) は provenance（[`VrmaRetargetProvenance`](../../src/core/animation/vrmaretarget.hpp#L41) の `source_rig_sha256` / `target_rig_sha256` / `profile_version`）と、警告 [`VrmaRetargetWarningKind`](../../src/core/animation/vrmaretarget.hpp#L29)（`missing_optional_target_bone` / `missing_target_expression`）を持ちます。

> **設計決定:** これは**不変のtarget-rig-local中間クリップ**であり、この型自体は `AnimationSource` ではありません。`retargetVrmaClip()` は instance / graph / renderer / ABI のどこへもpublishせず、publishは次項のWP178 APIが担います。交換形式の取り込みと、実行時のアニメーション再生を別の層に保つための境界です。

> **注意:** [`vrmaretarget.hpp#L122`](../../src/core/animation/vrmaretarget.hpp#L122) のコメント（`This is not an AnimationSource and does not publish to an instance, graph, renderer, or ABI.`）はWP177時点のままで、WP178で入った登録経路を反映していません。ソース側のコメントが古い箇所です。

### `AnimationSource` としての登録（WP178） ✅実装済み

retarget済みクリップは、`anim_graph` と同じ名前付きClip/Cursor語彙へ登録できます。

| API | 位置 | 役割 |
|---|---|---|
| `AnimationServiceRuntime::registerVrmaSource()` | [animationservice.hpp#L34](../../src/core/animation/animationservice.hpp#L34) | `VrmaRetargetedClip` をobject名 + source名で登録 |
| `AnimationServiceRuntime::reloadVrmaSource()` | [同 #L37](../../src/core/animation/animationservice.hpp#L37) | 論理identityを保ったまま、そのsource assetのgenerationだけを進める |

ABI側では [`AnimationClipKindV1`](../../src/core/userpublic/animation/abi_v1.hpp#L96) が `skeletal_clip` / `vrma_retargeted_clip` を区別します。clip descriptorの**加算的な末尾**（[abi_v1.hpp#L171-L180](../../src/core/userpublic/animation/abi_v1.hpp#L171)）に `profile_version` / `asset_identity` / `asset_generation` / `source_rig_sha256` / `target_rig_sha256` が載り、[`CursorStatusV1`](../../src/core/userpublic/animation/animgraph.hpp#L66) にも同じprovenanceが出ます。

> **設計決定:** skeletal clipと別語彙を作らず、`AnimationClipKindV1` とprovenanceフィールドで区別しています。ゲームコードから見たClip/Cursorの扱いは`.vrma`由来かどうかで変わらず、reloadは論理identityを保ったままgenerationだけを進めます。ABIの追加は既存フィールドの後ろへの加算なので、古い呼び出し側は `reserved2` までで読み止められます。

テストは [`test/vrmadecoder_test.cpp`](../../test/vrmadecoder_test.cpp) / [`test/vrmaretarget_test.cpp`](../../test/vrmaretarget_test.cpp) / [`test/vrmasource_test.cpp`](../../test/vrmasource_test.cpp)（GPU）、fixtureは [`test/vrma_fixture_writer.cpp`](../../test/vrma_fixture_writer.cpp) + [`test/vrma_fixture.hpp`](../../test/vrma_fixture.hpp) が生成します。

次章では、sceneの中心にあるECSの実体をメモリ配置から追います。
