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
| scene | [`normalizeSceneDataJson()`](../../src/project/sceneformat.cpp#L201) | [`SceneLoader::load()`](../../src/core/loader/scene.cpp#L215) |
| render feature | [`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L1611) | [`registerRenderingPassConfigData()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L133) |
| JSON-RPC | [`parseJsonRpcRequest()`](../../src/project/jsonrpc.cpp#L148) | [`RpcServer`](../../src/core/communication/rpcserver.cpp#L589) |
| asset manifest | [`parse/generate/verify`](../../src/project/assetsmanifest.hpp#L60) | [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L11) |
| material/surface | [`parseMaterialFormatJson()`](../../src/project/materialformat.cpp#L631)、[`parseSurfaceFormat()`](../../src/project/surfaceformat.cpp#L784) | runtime接続済み（WP116〜117, 122）。`.surface`は [`surfacecompiler`](../../src/core/shader/surfacecompiler.cpp) でGLSL/SPIR-V化されpipelineへ。`.material.json`は [`lowerMaterial()`](../../src/project/materiallowering.hpp#L82) → [`registerReloadableMaterialValuesFile()`](../../src/core/material/materialcontainer.hpp#L153) |

新形式を追加するときは、まず`src/project`へGPU非依存の受理/拒否規則を置き、次に`src/core`で実体化するのが既存パターンです。

## 3.2 ProjectSourceと設定の三段優先順位

[`ProjectSource`](../../src/core/loader/projectsrc.hpp#L7) は二種類の入力を保持します。

- `raw_data`: `PelicanCore(settings)`へ直接渡した上書きJSON
- `project_data`: playerが読んだ`project.json`全体

[`ProjectBasicConfig::ProjectBasicConfig()`](../../src/core/loader/basicconfig.cpp#L308) は次の三sourceを [`JsonLoader`](../../src/core/loader/basicconfig.cpp#L44) へ渡します。

1. `raw_data` — API/起動時の上書き
2. `project.json` の `basic_config`
3. `engine://default_config.json`

各fieldはこの順で最初に見つかった値が採用されます（[`JsonLoader::getVal()`](../../src/core/loader/basicconfig.cpp#L52)）。JSON object全体をdeep mergeするのではなく、必要fieldをpath単位で問い合わせる方式です。

構築時にwindow、framerate、seed、camera、各データファイル参照を値として取り込みます。その後、scene/assets/rendering/UI/input JSON本文はgetterの初回呼び出し時に読み、`mutable optional<string>`へcacheします（[`sceneDataJson()`以降](../../src/core/loader/basicconfig.cpp#L353)）。

## 3.3 PathResolver

宣言は [`pathresolver.hpp`](../../src/core/loader/pathresolver.hpp#L15)、中心実装は [`resolveRef()`](../../src/core/loader/pathresolver.cpp#L514) です。

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

shader、feature JSON、default config、debug fontなどは [`resources/CMakeLists.txt`](../../src/core/resources/CMakeLists.txt#L1) で`battery-embed`へ登録され、[`engineResource()`](../../src/core/loader/engineresources.cpp#L74) がIDを実データへ変換します。

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

[`prepareSceneBindings()`](../../src/core/loader/scene.cpp#L78) はcomponentを三系統へ分けます。

- `light`: ECSへ入れず`LightLoadEntry`へ
- `collider`: `ColliderComponent`として検証し、後で`PhysWorld`へ。`PELICAN_WITH_PHYSICS` OFFのビルドでは、colliderを含むsceneは明示エラーです（[scene.cpp](../../src/core/loader/scene.cpp#L244)）
- その他: `ComponentInfoManager`で文字列名から`ComponentId`へ

このためsceneの見た目はcomponent配列でも、現在のruntime実装ではlight/colliderがECS Chunkに保存されるわけではありません。

### 3. 旧sceneを破棄して新sceneを構築

[`SceneLoader::load()`](../../src/core/loader/scene.cpp#L215) は正規化と事前準備が成功した後、[`clearRuntimeScene()`](../../src/core/loader/scene.cpp#L374) を呼びます。

```text
object bindingをclear
→ PhysWorldをclear
→ ECS entityを全削除
→ PolygonInstanceContainerをclear
```

その後、light、camera、ECS object、colliderを作ります。ECS objectは [`GameObjects::createWithComponents()`](../../src/core/loader/scene.cpp#L261) のpopulate callback内でJSONを各Componentへロードします。

### 4. 名前binding

名前付きかつ`transform`を持つobjectだけが `object_bindings`へ入ります（[`bindObjectTransform()`](../../src/core/loader/scene.cpp#L383)）。RPC、camera controller、physicsはこの名前から世代付き`GameObjectId`を引き直します。

bindingは生ポインタを保持しません。アクセス時に`tryComponent<TransformComponent>()`でIDのgenerationとComponent存在を再検証します。

### 5. SceneLoaded event

ロード完了後に [`SceneLoaded`](../../src/core/userpublic/events.hpp#L9) をemitします（[`SceneLoader::load()`末尾](../../src/core/loader/scene.cpp#L348)）。フレーム末尾のpending loadからemitされた場合、eventは次フレーム冒頭に届きます。

## 3.6 即時loadと要求load

- `SceneLoader::load()`はその場で全sceneを置換。
- [`requestLoad()`](../../src/core/loader/scene.cpp#L351) は存在確認後、scene IDだけをpendingへ保存。
- [`applyPendingLoad()`](../../src/core/loader/scene.cpp#L360) はフレームのゲーム更新末尾で実際にload。

`GameContext::loadScene()`はrequest型です。RPCの`load_scene`は即時loadです。この違いは、ゲームSystem update中にECS全削除が起きないようにするためです。

## 3.7 ComponentのJSONロード

sceneのcomponent名から型への変換は [`ComponentInfoManager`](../../src/core/ecs/componentinfo.hpp#L34) が担います。

1. runtime登録時に `name → ComponentId` を保存。
2. `ComponentId → ComponentInfo`からJSON loader callbackを得る。
3. [`ComponentInfoManager::loadByJson()`](../../src/core/ecs/componentinfo.cpp#L25) が`JsonArchiveLoader`を作る。
4. Componentの`ref(ar)`を型消去callback越しに呼ぶ。

`ref()`を持たないComponentはscene JSONからロードできず、明示エラーになります。JSON archiveの対応型は [`jsonarchive.hpp`](../../src/core/userpublic/serialize/jsonarchive.hpp#L10) に固定されています。

## 3.8 Asset modelから描画instanceまで

[`ModelAssetContainer`](../../src/core/asset/model.hpp#L9) は`asset_data.json.models`を読み、拡張子に応じて [`GltfLoader::loadGltf()`](../../src/core/model/gltf.cpp#L2003) / [`loadGltfBinary()`](../../src/core/model/gltf.cpp#L1993) を呼び、名前から`ModelTemplate`へmapします。受理拡張子は`.glb`/`.gltf`に加え **`.vrm`** です（[asset/model.cpp](../../src/core/asset/model.cpp#L210)）。glTF scene fragmentの抽出には [`loadGltfBinarySceneNode()`](../../src/core/model/gltf.cpp#L1998) が使われます。

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

## 3.9 その他の純粋形式

### render feature

[`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L513) はfeature JSONを順番に読み、render target、buffer、pass、compute taskを追加し、限定的なoverrideを適用します。名前衝突、曖昧anchor、未知override fieldは即時エラーです。shader defineも重複排除して集約します。

### material / surface

[`parseMaterialFormatJson()`](../../src/project/materialformat.cpp#L631) はmaterial v1とoptional surface catalogの整合性を検証します。[`parseSurfaceFormat()`](../../src/project/surfaceformat.cpp#L784) はfront matter風の宣言部とshader codeを分け、parameter/texture/screen inputを構造化します。

これらはruntime接続済みです（WP116〜117, 122）。純粋層はparseと [`lowerMaterial()`](../../src/project/materiallowering.hpp#L82)（OpenPBR写像は [`openpbrmapping.hpp`](../../src/project/openpbrmapping.hpp)）まで、core層はsurface compile（[`surfacecompiler.cpp`](../../src/core/shader/surfacecompiler.cpp)）とvalues SSBO登録（[`registerReloadableMaterialValuesFile()`](../../src/core/material/materialcontainer.hpp#L153)）を担います。hot reloadは `MaterialValuesReloadHandler` / `TextureReloadHandler`（[src/core/material/](../../src/core/material)）です。

material bindingは **USD pathキー**を持ちます（[`materialformat.hpp`](../../src/project/materialformat.hpp#L45) の `usd_path`、[`modeltemplate.cpp`](../../src/core/model/modeltemplate.cpp#L61) で重複/未解決を検証）。これはU-USDレーン（WP119/124）の観測境界です。

### import manifest / import rules

[`parseImportManifestJson()`](../../src/project/importmanifest.cpp#L191) はDCC納品物のtool/source/outputとSHA-256、対応schemaを検証します。runtime playerではなく、[`pelican_cli import`](../../src/devcli/importcommand.cpp#L358) が主利用者です。加えて [`importrules.hpp`](../../src/project/importrules.hpp) がmatch/recipe/defaultsの3層優先を持つルールベースimport（`pelican_cli import --rules`、[importcommand.cpp](../../src/devcli/importcommand.cpp#L362)）を提供します。

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

次章では、sceneの中心にあるECSの実体をメモリ配置から追います。
