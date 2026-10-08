# 第12章 コード読解ガイド(技術解説書)

最終確認: 2026-10-07(develop d975ee9c0)

この章は他章と目的が違います。第1〜11章が「エンジンを**使う**ための説明書」なのに対し、この章は「エンジンの**中身を読む**ための地図」です。「このマニュアルはコードを正とする」とは、**設計文書と実装が食い違ったら実装の挙動を本文に書き、食い違いそのものは [第11章 §11.3](11_status.md) に台帳化する**、という本マニュアル全体の原則です([第0章](00_index.md))。より深いクラス単位の解説(ライフサイクル・インターフェース索引・落とし穴集)は [../source-code-guide/](../source-code-guide/README.md) —— アーキテクチャ / ランタイム / ロード / ECS / ゲームプレイ / 描画 / ツールと RPC の 7 章に、クラス索引(第8章)・落とし穴集(第9章)・背景知識(第10章)・レイトレ(第11章)が続く構成で、索引は同ディレクトリの README です。本章は入口・概観、詳細はそちらへ。すべての節にソースへの相対リンクを張ってあるので、エディタ(VS Code 等)や GitHub 上でクリックしながら読み進められます。

> **リンクの約束:** ファイルリンクはリファクタに強いのでそちらを基本とし、特に重要な箇所のみ行アンカー(`#L212` — GitHub で有効)を付けています。行番号は **2026-07-21 に再測定**したものです(WP138〜179 で大きく動いたため、この時点で追随できなかったアンカーはファイルリンクに落として関数名だけを残しています)。ずれていたら添えてある関数名で探してください。

## この章で学ぶこと

- リポジトリのどこに何があるか(ディレクトリ地図)
- 起動から 1 フレーム描画までのコードパス(追跡ルート付き)
- 各 JSON がどのパーサで読まれるか(データ→コード対応表)
- サブシステム別の「最初に開くべきファイル」
- コード全体に繰り返し現れる実装パターン(読むときの目印)

## 12.1 リポジトリ地図

リポジトリの配置は[第1章 §1.3](01_overview.md#13-リポジトリの地図)、層ごとの役割は[§1.4](01_overview.md#14-レイヤ構造とレイヤ規則)を参照してください。

## 12.2 レイヤ構造と依存規則

レイヤ構造と依存規則は[第1章 §1.4](01_overview.md#14-レイヤ構造とレイヤ規則)を参照してください。

## 12.3 起動から最初のフレームまで(追跡ルート)

デバッガでステップするならこの順に張ってください。

1. **CLI パース** — [`parseLaunchConfig()`](../../src/player/main.cpp#L361) `parseLaunchConfig`。argparse で全引数を解釈し、[EngineLaunchConfig](../../src/core/launchconfig.hpp) に詰める。`--project` 省略時の暗黙探索は [`findExampleProjectNearExecutable()`](../../src/player/main.cpp#L255) `findExampleProjectNearExecutable`(exe の祖先から `projects/example/project.json` を探す)
2. **コア生成** — [`main.cpp` 内](../../src/player/main.cpp#L731) `main` → `Pelican::PelicanCore pl{...}`([pelican_core.cpp](../../src/core/userpublic/pelican_core.cpp))。モジュールコンテナに LaunchConfig を書き込み、解析済み`ProjectEnvelope`(project.json を検証して分解した構造体。`name` / `engine_min_version` / `basic_config` / `asset_stores` / `prefabs` を持つ — [projectformat.hpp](../../src/project/projectformat.hpp#L39))を[PathResolver](../../src/core/loader/pathresolver.hpp)`.setup()`へ、raw JSONを[ProjectSource](../../src/core/loader/projectsrc.hpp)へ渡す
3. **封筒検証と設定合成** — [`parseProjectEnvelopeText()`](../../src/project/projectformat.cpp#L311) がschema(`pelican.project` ちょうど)/version(`1` ちょうど)/engine minimum version(プロジェクトが要求するエンジン版)を検証し(いずれも不一致は起動を止める hard error。engine 版だけは `--ignore-engine-version` で WARN に降格)、[`projectBasicConfigSource()`](../../src/core/loader/basicconfig.cpp#L287) が結果を受けて CLI > project.json > [default_config.json](../../src/core/resources/default_config.json) の 3 段を合成
4. **ループ開始** — `pl.run()` → [`loop.cpp` 内](../../src/core/appflow/loop.cpp#L772) `Loop::run`。ここがフレーム編成の唯一の場所で、headless RPC / headless 固定フレーム / windowed の 3 分岐を選ぶ。windowed 内の通常 / XR と追加の RPC 層まで分類した読解ガイドの一覧は 5 項目です([読解ガイド第2章 §2.4](../source-code-guide/02_runtime_lifecycle.md#24-looprun-の五経路))。ウィンドウモードで `--rpc` が付いた場合は `WindowedRpcHost` を生成し、フレーム境界(`processFrameBoundary()`)でのみリクエストを処理します([windowedrpchost.cpp](../../src/core/communication/windowedrpchost.cpp))
5. **描画** — [renderer.cpp](../../src/core/vkcore/renderer.cpp) `executeRenderingPasses` → `executePlannedFrameGraph`。✅WP64 で一本化済み: 全構成が FramePlan(描画の前に確定させたノード列 + 依存レベル + バリアの計画 — [frameplanner.hpp](../../src/core/renderingpass/frameplanner.hpp#L168))順で実行される。ノード種別(`render` / `compute` / `anchor` / `snapshot_copy` / `output_transform`)の dispatch も同ファイル
6. **終了処理** — [teardown.cpp](../../src/core/appflow/teardown.cpp) `RuntimeTeardownGuard` が例外経路でも waitIdle → 物理 → ECS → モデルの順に noexcept で掃除

## 12.4 1 フレームの流れ(中核は [`framephase.cpp` 内](../../src/core/appflow/framephase.cpp#L143) `updateFrameState`。headless も同順)

フレーム全体の順序は[第1章のフレーム順の図](01_overview.md#frame-order)を参照してください。

- 時刻と各フェーズの順は[第1章のフレーム順](01_overview.md#frame-order)、三ドライバの呼出し経路は[読解ガイドの runtime lifecycle](../source-code-guide/02_runtime_lifecycle.md#frame-driver-paths)を参照してください。
- 入力:  [inputstate.cpp](../../src/core/os/inputstate.cpp) — GLFW コールバックはキューに積むだけで、フレーム先頭で 1 回だけ `InputSnapshot` を確定します(決定性・記録可能性の土台)
- ゲームシステム:  ゲームコードの `update(GameContext&)` はここで呼ばれます。登録機構は [details/system/registerer.hpp](../../src/core/userpublic/details/system/registerer.hpp)。**エンジン同梱のシステムも特権なしで同じ (order, 名前) 総順序に参加**します — オブジェクト behavior を回す `BehaviorSystem`(order = 50。[`behavior.hpp` 内](../../src/core/userpublic/behavior.hpp#L19)、実体は [gamelogic/behaviorarena.cpp](../../src/core/gamelogic/behaviorarena.cpp))/ カメラコントローラ `BuiltinCameraControllerSystem`(order = 10000。[userpublic/cameracontrollersystem.cpp](../../src/core/userpublic/cameracontrollersystem.cpp))/ トリガー判定 `PhysicsTriggerSystem`(order = `INT_MAX`。[phys/physworld.cpp](../../src/core/phys/physworld.cpp))。order がこれらより大きいユーザーシステムは、それぞれより後に走ります
- 描画:  パス種別ごとの実装は [materialrender.cpp](../../src/core/renderer/materialrender.cpp)(gbuffer)・[fullscreenpassrenderer.cpp](../../src/core/renderer/fullscreenpassrenderer.cpp)・[shadowdepthpasscontainer.cpp](../../src/core/renderer/shadowdepthpasscontainer.cpp)・[debugdraw.cpp](../../src/core/renderer/debugdraw.cpp)・[debugtext.cpp](../../src/core/renderer/debugtext.cpp)・[uirenderer.cpp](../../src/core/renderer/uirenderer.cpp)

## 12.5 データ → コード対応表(JSON はどこで読まれるか)

| ファイル / 形式 | スキーマ解説 | パーサ(検証・hard error の実装) |
|---|---|---|
| `project.json` | [第3章](03_project_format.md) | 純ロジック: [project/projectformat.cpp](../../src/project/projectformat.cpp) `parseProjectEnvelopeJson()` / engine adapter: [loader/basicconfig.cpp](../../src/core/loader/basicconfig.cpp) |
| パス参照(project:// / engine:// / user:// / #) | [第3章](03_project_format.md) | 純ロジック: [project/projectpathresolver.cpp](../../src/project/projectpathresolver.cpp) / engine resource・ログadapter: [loader/pathresolver.cpp](../../src/core/loader/pathresolver.cpp) |
| `*.scene.json`(pelican.scene v2。MG-2b から v2 だけ) | [第4章](04_scene_ecs.md) | 純ロジック(エンベロープ・識別子): [project/sceneformat.cpp](../../src/project/sceneformat.cpp) / **コンポーネント受理仕様の正**: [loader/componentcodec.cpp](../../src/core/loader/componentcodec.cpp) の codec テーブル / 振り分け・バインド: [loader/scene.cpp](../../src/core/loader/scene.cpp) |
| scene の `behavior` コンポーネント | [第4章](04_scene_ecs.md)・[第8章](08_gameplay.md) | [`prepareSceneBehaviorAttachments()`](../../src/core/gamelogic/behaviorarena.cpp#L95) `prepareSceneBehaviorAttachments()`(`scene.cpp` は `behavior` を ECS 経路から外すだけ) |
| `asset_data.json`(`pelican.asset_data` v2。MG-2b から v2 だけ) | [第5章](05_assets.md) | 純ロジック(エンベロープ・models/materials/textures): [project/assetdataformat.cpp](../../src/project/assetdataformat.cpp) / model 登録: [asset/model.cpp](../../src/core/asset/model.cpp) / project material 登録: [material/projectmaterialasset.cpp](../../src/core/material/projectmaterialasset.cpp) |
| `.vrma`(VRM Animation GLB) | [第5章](05_assets.md) | デコード: [loader/vrmadecoder.cpp](../../src/core/loader/vrmadecoder.cpp)(拡張子 `.vrma` が alias ゲート)/ リターゲット: [animation/vrmaretarget.cpp](../../src/core/animation/vrmaretarget.cpp)(版付き profile v1)/ 統合: [animation/animationservice.cpp](../../src/core/animation/animationservice.cpp) |
| rendering config | [第6章](06_rendering.md) | [renderingpass/](../../src/core/renderingpass) の *jsonparser 群(RT フォーマット 45 種: [renderingpassjsonhelpers.cpp](../../src/core/renderingpass/renderingpassjsonhelpers.cpp) `format_names` / パス種別 **14 種**: 一覧と鍵の所有は [project/passfieldownership.hpp](../../src/project/passfieldownership.hpp)。形の規則の正本は [project/passshapepolicy.cpp](../../src/project/passshapepolicy.cpp)) |
| feature fragment(pelican.render_feature) | [第6章](06_rendering.md) | 純ロジック: [project/featurecompose.cpp](../../src/project/featurecompose.cpp) |
| 共通グラフ文書(`pelican.graph` v1) | [第3章](03_project_format.md) | 純ロジック: [project/graphcomposition.cpp](../../src/project/graphcomposition.cpp)(`definitions` / `nodes` / `outputs` の展開と編集 op)→ [project/graphpartition.cpp](../../src/project/graphpartition.cpp)(`values` / `rendering` / `animation` への分割)。rendering config はこの文書の `rendering` メンバに入り、住所の判定は [project/renderconfigsection.hpp](../../src/project/renderconfigsection.hpp) だけが持つ |
| `pelican.value_graph` v1 | — | [userpublic/valuegraph.cpp](../../src/core/userpublic/valuegraph.cpp)。**内部 IR + テスト面であって外部契約ではない**(`pelican.graph` を lowering した結果と同じ形。本番で `Graph` を作る場所は 1 箇所で、入力は `pelican.graph` のみ) |
| モジュールカタログ(`pelican.graph_catalog` v1) | [第10章](10_tools.md) | [project/graphmodule.cpp](../../src/project/graphmodule.cpp) の `catalog()`(名前順。出力は `pelican_cli graph-catalog`) |
| package manifest(`pelican.package` v1) | [第3章](03_project_format.md) | 純ロジック: [project/packagemanifest.cpp](../../src/project/packagemanifest.cpp)(install は `<project>/packages/<namespace>/` への copy、`project.json` に `{namespace, package_version, sha256}` を記録)/ CLI: [devcli/packagecommand.cpp](../../src/devcli/packagecommand.cpp) |
| 持ち出し部品(`pelican.render_component` v1) | [第6章](06_rendering.md) | 純ロジック: [project/rendercomponent.cpp](../../src/project/rendercomponent.cpp)(materialize 済み feature 1 本 + host が満たす render target 契約。公開はディレクトリ rename)|
| シェーダ stem 参照 | [第6章](06_rendering.md) | [`makeShaderReference()`](../../src/core/shader/shaderreference.cpp#L100) `makeShaderReference`(拡張子 hard error はここ) |
| `input/actions.json`(pelican.input_actions) | [第7章](07_input_ui.md) | [os/actionmap.cpp](../../src/core/os/actionmap.cpp)(schema/version ゲートは `input_actions_schema` の照合箇所) |
| `ui/ui_overlay.json` | [第7章](07_input_ui.md) | [loader/basicconfig.cpp](../../src/core/loader/basicconfig.cpp) `uiConfigJson()` → [renderer/uicontainer.cpp](../../src/core/renderer/uicontainer.cpp) |
| collider コンポーネント | [第4章](04_scene_ecs.md)・[第8章](08_gameplay.md) | 受理の正: [`decodeCollider()`](../../src/core/loader/componentcodec.cpp#L1417) `decodeCollider`(**closed schema** = 列挙した鍵以外を拒否する schema。未知の鍵を黙って無視する open な受理はしない)。ランタイム型は [userpublic/components/collider.cpp](../../src/core/userpublic/components/collider.cpp) |
| camera コンポーネント / controller | [第4章](04_scene_ecs.md)・[第8章](08_gameplay.md) | 受理の正: [`decodeCamera()`](../../src/core/loader/componentcodec.cpp#L945) `decodeCamera`(closed schema。ネスト `perspective` / `orthographic` を含む)。controller の解釈は [renderer/camera.cpp](../../src/core/renderer/camera.cpp) `parseSceneCameraController` |
| pelican.transform_seq(JSONL) | [第5章](05_assets.md) | [playback/seqplayer.cpp](../../src/core/playback/seqplayer.cpp) `TransformSequence::fromJsonLines` |
| pelican.vat(GLB extras) | [第5章](05_assets.md) | 純ロジック: [model/vatformat.cpp](../../src/core/model/vatformat.cpp) / 再生: [playback/vatplayer.cpp](../../src/core/playback/vatplayer.cpp) |
| pelican.material / `.surface` | [第6章](06_rendering.md) | 純ロジック: [project/materialformat.cpp](../../src/project/materialformat.cpp)・[project/surfaceformat.cpp](../../src/project/surfaceformat.cpp)・[project/materiallowering.cpp](../../src/project/materiallowering.cpp) / 接続: [shader/surfacecompiler.cpp](../../src/core/shader/surfacecompiler.cpp)・[material/](../../src/core/material) |
| rendering config の `snapshots` / canonical anchor | [第6章](06_rendering.md) | [project/featurecompose.cpp](../../src/project/featurecompose.cpp) |
| pelican.import(納品 manifest) | [第5章](05_assets.md) | [project/importmanifest.cpp](../../src/project/importmanifest.cpp) `parseImportManifestJson`(利用者: [devcli/importcommand.cpp](../../src/devcli/importcommand.cpp)) |
| JSON-RPC エンベロープ | [第10章](10_tools.md) | 純ロジック: [project/jsonrpc.cpp](../../src/project/jsonrpc.cpp) / メソッド実装: [communication/rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1552) `configureEngineRpcHandlers`(`setHandler` 群はすべてこの関数の中。エディタ系メソッドの実体は [editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp) ほか) |
| pelican.frame_plan(出力専用) | [第6章](06_rendering.md) | 生成: [renderingpass/frameplanner.cpp](../../src/core/renderingpass/frameplanner.cpp) |
| `test/golden/inventory.json`(pelican.golden_inventory v1 = golden ケースの登録簿) | [第6章](06_rendering.md) | [test/golden_inventory.py](../../test/golden_inventory.py)(生成と検証)/ gate: [test/ci/test_golden_inventory.py](../../test/ci/test_golden_inventory.py)。**golden ケースは inventory への登録が必須** |

## 12.6 サブシステム別読解ガイド

### ECS（内部登録と公開テンプレートの分担）

読み始め: [`EntityId`](../../src/core/userpublic/details/ecs/entity.hpp#L12)(世代付き `EntityId` の canonical 定義)→ [`MutationScope::MutationScope()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L297) `MutationScope`(生成・変更トランザクションの単位)→ [chunk.cpp](../../src/core/userpublic/details/ecs/chunk.cpp)(アーキタイプ別チャンク格納)→ [ecs/archetypemigration.hpp](../../src/core/ecs/archetypemigration.hpp)(コンポーネント追加/削除でアーキタイプが変わるときの failure-atomic な移行。アダプタは `prepare` / `rollback` / `publish` の 3 メソッドで、`prepare` は throw してよいが公開状態を変えてはならず、`publish` は失敗できない、という契約)。

- WP62(R5-core)で世代付き ID・生成 transaction・teardown フェーズが入り、**フレームを跨ぐ生ポインタ保持は禁止**(ID を持ち resolve する)が規約になりました
- [ecs/](../../src/core/ecs) 直下([core.hpp](../../src/core/ecs/core.hpp)・[componentinfo.cpp](../../src/core/ecs/componentinfo.cpp))はエンジン内部視点、[userpublic/details/ecs/](../../src/core/userpublic/details/ecs) は実体テンプレート、という二重構造です。コンポーネントの型登録(scene JSON の `name` → C++ 型の対応)は [componentinfo.cpp](../../src/core/ecs/componentinfo.cpp) + [details/component/registerer.cpp](../../src/core/userpublic/details/component/registerer.cpp)
- エンジン内部システム(transform 伝播・modelview 更新)は [ecs/predefined/](../../src/core/ecs/predefined)。**ゲームシステム**(`PELICAN_REGISTER_SYSTEM`)とは別物です(→ [第8章](08_gameplay.md))

### シェーダ基盤（JSON からパイプラインを生成するまで）

読み順: [shadercompiler.cpp](../../src/core/shader/shadercompiler.cpp)(shaderc で GLSL→SPIR-V。**シェーダのコンパイル成否を結果オブジェクトで返す**)→ [shaderreflection.cpp](../../src/core/shader/shaderreflection.cpp)(spirv-reflect で descriptor layout を自動生成)→ [shaderlibrary.cpp](../../src/core/shader/shaderlibrary.cpp)(stem 解決・リロードの世代 swap)→ [pipelinefactory.cpp](../../src/core/shader/pipelinefactory.cpp)(レイアウトハッシュ共有・`pipeline_cache.bin` 永続化)。マテリアル系は [surfacecompiler.cpp](../../src/core/shader/surfacecompiler.cpp)(`.surface` → テンプレート逆 include 合成)と [spvlink.cpp](../../src/core/shader/spvlink.cpp)(experimental SPIR-V リンカ)。SPIR-V(Vulkan がシェーダとして受け取るバイナリ中間表現。GLSL / WGSL はこれへコンパイルされる)のディスクキャッシュ(WP82)は `<project>/.pelican/shader_cache/`。

ホットリロードのトランザクション(WP96/108 以降)は shaderlibrary 単独ではなく [src/core/watch/](../../src/core/watch)(FileWatcher / ContentDigest / ReloadService / ReloadGate)経由の単一経路です — シェーダに限らずテクスチャ・マテリアル値・モデルコンテナも同じ基盤で、失敗時は全体破棄・旧世代維持。

- set 規約(set 0 = frame / 1 = pass input / 2 = material / 3 = 自由枠)の正は [shader_contract.md](../shader_contract.md) と [pelican_sets.hpp](../../src/core/shader/pelican_sets.hpp) + [resources/shaders/include/pelican_sets.glsl](../../src/core/resources/shaders/include/pelican_sets.glsl)
- 旧パイプラインの破棄は即時ではなく [deletionqueue.cpp](../../src/core/vkcore/deletionqueue.cpp)(遅延破棄。in-flight フレームを守る)

### フレームグラフ(宣言 → 計画 → 実行)

読み順: [computetaskdefinitionjson.cpp](../../src/core/renderingpass/computetaskdefinitionjson.cpp)(`buffers` / `compute_tasks` の宣言の JSON パース。旧 computetask.cpp は宣言パース / [container](../../src/core/renderingpass/computetaskcontainer.cpp) / [buffer container](../../src/core/renderingpass/framegraphbuffercontainer.cpp) の 3 つに分かれ、`computetask.hpp` は傘の include になりました)→ [frameplanner.cpp](../../src/core/renderingpass/frameplanner.cpp)(reads/writes からトポロジカルソート・レベル・バリアを導出。曖昧な writes-writes は hard error)→ [framegraphruntime.cpp](../../src/core/renderingpass/framegraphruntime.cpp)(プラン駆動実行)。

> **設計決定(三層):** ①ユーザーは依存(reads/writes)だけ宣言する ②順序・バリアは機械導出 ③どうしても必要な箇所だけ `after`/`before` で手詰め。順序を手書きさせないことでパス追加が局所変更になる([design_compute_task_graph.md](../design_compute_task_graph.md))。

補足: canonical anchor ノード(`__anchor_sprite` など 8 個)・`snapshot_copy`・終端 `output_transform` は [featurecompose.cpp](../../src/project/featurecompose.cpp) の合成段階で実体化されます。実行系は WP64 で全構成プラン駆動に一本化済みです(レガシー経路は撤去)。

### 入力(4 層のうち実装は 2 層 + 注入)

[inputstate.cpp](../../src/core/os/inputstate.cpp)(L1: スナップショット)→ [actionmap.cpp](../../src/core/os/actionmap.cpp)(L2: アクション評価・セットスタック・エッジ検出)→ [gamecontext.cpp](../../src/core/userpublic/gamecontext.cpp) の `actionPressed` 系。rpc からの合成入力は [rpcserver.cpp](../../src/core/communication/rpcserver.cpp) の `inject_input` がキューに積み、次フレームのスナップショットに合流します。

### ゲーム向け公開 API（facade）

ゲームコードが見てよいのは [userpublic/](../../src/core/userpublic) だけです。全 API の入口 = [`GameContext`](../../src/core/userpublic/gamecontext.hpp#L284)。オブジェクト生成ビルダーは [gameobjects.hpp](../../src/core/userpublic/gameobjects.hpp)(`GameObjects::add().addComponent<...>().finish()`)、システム/イベント登録マクロは [gamesystem.hpp](../../src/core/userpublic/gamesystem.hpp) と [events.hpp](../../src/core/userpublic/events.hpp)、決定的乱数(PCG32)は [deterministicrng.cpp](../../src/core/userpublic/deterministicrng.cpp)。

オブジェクト単位の毎フレーム処理は [behavior.hpp](../../src/core/userpublic/behavior.hpp) の `Behavior` 基底 + `PELICAN_REGISTER_BEHAVIOR`([`PELICAN_REGISTER_BEHAVIOR()`](../../src/core/userpublic/details/behavior/registerer.hpp#L385))で、`BehaviorContext` は `GameContext` を継承して `self()` / `params<T>()` などを足したものです。パラメータの schema は [details/schema/structfieldschema.hpp](../../src/core/userpublic/details/schema/structfieldschema.hpp)(WP150 の共通 field descriptor。component codec と同じ機構を共有しています)。

### シーンロードのトランザクション(2026-09 の新しい住人)

シーンロード・結果・副作用の型と責務は[読解ガイド第8章 §8.4](../source-code-guide/08_class_interface_index.md#84-projectpathdata-formatloading)を参照してください。

### 宣言 schema(v2 と v3)

component / behavior / event の field descriptor は
[`details/schema/structfieldschema.hpp`](../../src/core/userpublic/details/schema/structfieldschema.hpp) が正です。
WP150 の flat な `StructFieldSchema` に加えて、WP382a で
[`SchemaNode`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L155)(`Struct` / `Leaf` /
[`Flags`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L194) /
[`List`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L222)、**深さ 2 まで**。
list の list は compile 時に拒否)が入りました。wire 側の `$list` エンベロープ(`v` / `next_id` / 行 id)の
decode / encode は [`structfieldjson.hpp`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L597) です。
`List` / `Flags` を含む宣言だけが v3 の登録入口を通り、v2 の record は `static_assert` で凍結されています。
**engine 同梱の component はまだ 1 つも `List` を宣言していません**(使っているのは
`test/fixtures/wp382a_schema/` の v3 DLL だけ)。

### ゲームロジック DLL のロード前診断

[`gamelogicimports.hpp`](../../src/core/gamelogic/gamelogicimports.hpp#L64) の
`readGameLogicImports()` / `diagnoseGameLogicImports()` は、**PE の top-level import /
delay-import directory だけを読みます**(image を map せず、`LoadLibrary` も呼ばず、
imported module も辿りません)。したがって古い DLL の static initializer は 1 行も走りません。
エラーコードは同ヘッダの
[`gameLogicSourceBusyErrorCode`](../../src/core/gamelogic/gamelogicimports.hpp#L54) 付近に並んでいます。
owner の払い出しと builtin 登録は game DLL のヘッダから外れ、
[`registrationownerhost.hpp`](../../src/core/registrationownerhost.hpp#L13) と
[`enginecomponentregistrar.hpp`](../../src/core/ecs/enginecomponentregistrar.hpp#L15) に移りました(✅WP379a)。

### RPC(外部ツールの操作面)

エンベロープ検証(JSON-RPC 2.0 / NDJSON)は純ロジック [project/jsonrpc.cpp](../../src/project/jsonrpc.cpp)、メソッド実装とエンジンへのバインドは [`rpcserver.cpp` 内](../../src/core/communication/rpcserver.cpp#L1552) の `configureEngineRpcHandlers`(`runEngineRpcServer` はこれを組み立てて回すだけの薄いエントリです)。`RpcServer(istream, ostream) + setHandler` の汎用ディスパッチャ構造なので、メソッド追加はハンドラ登録 1 箇所です。OFF ビルド時のスタブは [rpcserver_stub.cpp](../../src/core/communication/rpcserver_stub.cpp)。

- **エディタ系メソッドの実体は別ファイル**です。`rpcserver.cpp` の `setHandler` は薄い入口で、編集・undo/redo・ジャーナル・preview・アセット問い合わせは [editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp) / [editorjournal.cpp](../../src/core/communication/editorjournal.cpp) / [editorpreviewservice.cpp](../../src/core/communication/editorpreviewservice.cpp) / [editorassetqueryruntime.cpp](../../src/core/communication/editorassetqueryruntime.cpp) にあります(組み立ては [editorruntimefactory.cpp](../../src/core/communication/editorruntimefactory.cpp))
- **経路が 2 つある**点に注意してください。`--headless --rpc` は stdin を読み切るまでループを占有する blocking 経路、ウィンドウモードの `--rpc` は [windowedrpchost.cpp](../../src/core/communication/windowedrpchost.cpp) がリクエストを有界キューに積み、[loop.cpp](../../src/core/appflow/loop.cpp) が**フレーム境界で** `processFrameBoundary()` を呼んで捌く経路です(→ [第10章](10_tools.md))

### ツール(devcli)

[devcli/main.cpp](../../src/devcli/main.cpp) が **7 系統**のサブコマンド分岐(`assets` / `bake-camera` / `import` / `dist-config` / `project` / `dump-lowered-material` / `vrm`)、実装は [projectinit.cpp](../../src/devcli/projectinit.cpp)(`--template standard|blank` を読むだけ。雛形の中身は F-2 で `pelican_project` の [project/projecttemplate.cpp](../../src/project/projecttemplate.cpp) へ移り、正は [同ファイル](../../src/project/projecttemplate.cpp#L350) の `templateFiles()`。standard / blank とも **16 エントリ**(blank は VI-2 から `shaders/present.frag` を書かない)。Studio の File > New Project... も同じ関数を呼ぶ)・[importcommand.cpp](../../src/devcli/importcommand.cpp)(sha256 照合 → asset_data.json 追記・冪等)・[distconfig.cpp](../../src/devcli/distconfig.cpp)(プロジェクト内容から PELICAN_WITH_* を導出。GLB の JSON チャンクを直接パースして VAT 有無を判定する箇所が読みどころ)。外部ツールの子プロセス起動は [processrunner.cpp](../../src/devcli/processrunner.cpp) に集約されています(`import --rules` と `bake-camera` が利用)。

## 12.7 横断的な実装パターン(読むときの目印)

| パターン | 定義場所 | 意味 |
|---|---|---|
| `DECLARE_MODULE(Name)` / `GET_MODULE(Name)` | [container.hpp](../../src/core/container.hpp) | 遅延生成シングルトンのモジュール機構。`FastModuleContainer::get<T>()` が実体 |
| `XxxDependencies` 構造体 | [render_pass_dispatch.hpp](../../src/core/vkcore/render_pass_dispatch.hpp) | 新規コードの規約: モジュール間依存を関数引数の構造体で明示(隠れた GET_MODULE を避ける) |
| `PELICAN_DEFINE_HANDLE` | [handle.hpp](../../src/core/handle.hpp) | 型安全な ID 型の定義マクロ |
| `throw std::runtime_error(...)` 即時 | 全域 | fail-fast。**例外はシェーダコンパイルのみ result 返却**([shadercompiler.cpp](../../src/core/shader/shadercompiler.cpp)) |
| `*_stub.cpp` | [rpcserver_stub.cpp](../../src/core/communication/rpcserver_stub.cpp) 等 | `PELICAN_WITH_*=OFF` 時にリンクされる明確エラー版([build_features.hpp](../../src/core/build_features.hpp)) |
| `vk::UniqueXxx` / `ImageWrapper` / `BufferWrapper` | [vkcore/image.hpp](../../src/core/vkcore/image.hpp), [buf.hpp](../../src/core/vkcore/buf.hpp) | リソースは RAII ラッパ必須(vulkan.hpp。C API 直接使用は禁止) |
| `LOG_INFO(logger, ...)` | [log.hpp](../../src/core/log.hpp) | quill ログ。**rpc モードでは stdout に何も出さない**(stdout はプロトコル専用) |
| `b_embed(...)` | [resources/CMakeLists.txt](../../src/core/resources/CMakeLists.txt) | `engine://` リソースのバイナリ埋め込み。登録簿は [engineresources.cpp](../../src/core/loader/engineresources.cpp) |
| 純ロジック + fixture テスト | [src/project/](../../src/project) + [test/fixtures/](../../test/fixtures) | 交換形式はエンジン非依存でパースし、テストデータは fixture ディレクトリで web と共有 |

命名規約: 型 = CamelCase、関数 = lowerCamelCase、メンバ = snake_case、ファイル = 小文字連結。

## 12.8 「これをやりたいなら、ここを読む」早見表

| やりたいこと | 読む / 変える場所 | 参照章 |
|---|---|---|
| CLI 引数を足す | [`parseLaunchConfig()`](../../src/player/main.cpp#L361) + [launchconfig.hpp](../../src/core/launchconfig.hpp) | [第10章](10_tools.md) |
| scene に書ける新コンポーネント | [loader/componentcodec.cpp](../../src/core/loader/componentcodec.cpp) に codec 五つ組を足す(受理の正)+ [userpublic/components/](../../src/core/userpublic/components) のランタイム型 + ComponentInfo 登録 | [第4章](04_scene_ecs.md) |
| オブジェクトに毎フレーム処理を付ける | プロジェクトの `code/` に `PELICAN_REGISTER_BEHAVIOR` + scene 側に `behavior` コンポーネント(エンジン側は触らない) | [第4章](04_scene_ecs.md)・[第8章](08_gameplay.md) |
| ポストエフェクトを足す | feature fragment JSON + stem シェーダ(エンジンコード不要のことが多い) | [第6章](06_rendering.md) |
| compute パスを足す | config の `buffers`/`compute_tasks` + `.comp` stem(コード不要) | [第6章](06_rendering.md) |
| rpc メソッドを足す | [`rpcserver.cpp` 内](../../src/core/communication/rpcserver.cpp#L1552) `configureEngineRpcHandlers` にハンドラ追加 | [第10章](10_tools.md) |
| エディタ操作を足す | [communication/editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp) に操作を実装 → `rpcserver.cpp` の `setHandler` から呼ぶ(ImGui Inspector も同じ関数を通す) | [第10章](10_tools.md) |
| GPU デバッグラベルを付ける | [vkcore/debugutils.hpp](../../src/core/vkcore/debugutils.hpp) の `nameImage` / `nameImageView` / `nameBuffer` / `beginCommandLabel`(有効化は `--gpu-labels`。RT には `rt/<name>/surface/<n>/image` という規範名が [rendertargetcontainer.cpp](../../src/core/renderingpass/rendertargetcontainer.cpp) `nameRenderTargetSurfaces()` で自動的に付きます) | [第6章](06_rendering.md) |
| 新しい交換形式(JSON)を足す | [src/project/](../../src/project) に純ロジックパーサ + fixture + schema/version ゲート | [第1章](01_overview.md) 原則 |
| engine:// リソースを足す | 3+1 チェックリスト([adding_features.md](../adding_features.md)): b_embed / registered_ids / fixture / (web 鏡像) | [第9章](09_web.md) |
| ゲームの毎フレーム処理 | プロジェクトの `code/` に `PELICAN_REGISTER_SYSTEM`(エンジン側は触らない) | [第8章](08_gameplay.md) |
| オブジェクトに姿勢グラフを付けたい | rendering config を `pelican.graph` root にして(`schema` / `version` / `definitions` / `nodes` / `outputs` + `rendering`)、`graph.instance` と `pose.*` op を書く。見本は [projects/posegraph_demo/passes/main.json](../../projects/posegraph_demo/passes/main.json)。1 オブジェクトに animation component と姿勢 sink を同時に付けることはできない(`pose.sink_conflict@1`) | [第4章](04_scene_ecs.md)・[第6章](06_rendering.md) |
| 共通グラフに新しい op や型を足したい | [project/graphmodule.hpp](../../src/project/graphmodule.hpp) の `GraphModuleManifest` を 1 本作って登録する(型・op・sink はモジュールが持ち込む。op 名の特別扱いは無い)。登録すればカタログ(`pelican_cli graph-catalog`)とエディタに自動で出る | [第10章](10_tools.md) |
| package を作って配りたい | `package.json`(`pelican.package` v1)を持つディレクトリを作り、`pelican_cli package install <dir> --project <project>`。コードを含めるなら `code/CMakeLists.txt` で `pelican_package_sources(...)` を呼び、プロジェクト側は `pelican_game_sources(...)` の**後で** `pelican_packages()` を呼ぶ。雛形は [test/fixtures/package_template/](../../test/fixtures/package_template) | [第3章](03_project_format.md) |
| オフラインで連番画像を出したい | `pelican_player --headless --frames N --render-out <pattern>`。途中の時刻から始めるなら `--start-time <秒>`(`--headless` 必須。秒は 3600 まで。`llround(秒×fps)` でフレーム番号へ量子化され、そこまでは capture 無しで実際に回してから出力が始まる) | [第10章](10_tools.md) |

機能追加の正式レシピ集は [adding_features.md](../adding_features.md) にあります(本表はその読解入口です)。

## 12.9 テストから読む(テストは実行可能な仕様)

- **形式の仕様を知りたい** → [test/fixtures/](../../test/fixtures) の valid/invalid ペア + `expectations.json`(error_kind 付き)。パーサが何を受理し何を拒むかの正確な一覧です
- **描画の正解を知りたい** → [test/golden/](../../test/golden)(case.json + expected.png + tolerance.json。**件数は [第6章 §6.10](06_rendering.md) が正**)。`clear` が最小、`surface_toon` / `taa_*` 7 種 / `vrm_expression_*` / sprite 系 9 種が応用。expected.png は encoded-sRGB 契約(`test/golden/README.md`)
  - ⚠ **ディレクトリを置くだけでは通りません**。ケースは [test/golden/inventory.json](../../test/golden/inventory.json)(`pelican.golden_inventory` v1)への登録が必須で、未登録・ファイル欠落・ハッシュ不一致は GPU 不要の gate が落とします。エントリの実物:
    ```json
    { "name": "clear", "mode": "clear",
      "files": ["case.json", "expected.png", "tolerance.json"],
      "expected_png_sha256": "2d6f3715483b91e4444c11085fd13444ac343803574403008ade21672a299e78",
      "tolerance": true, "vat": "on_and_off", "traces": ["rgba8"] }
    ```
- **ツールの使い方の実例** → [test/](../../test) の `run_*.cmake` / `run_*.ps1`(player / pelican_cli を実際に子プロセス起動する結合テスト。現在 **54 本** = `.cmake` 50 + `.ps1` 4。うち `.cmake` 2 本(`run_project_code_smoke.cmake` / `run_package_template_smoke.cmake`)は `add_test` に登録されておらず、手で `cmake -P` するものです)。特に `run_rpc_headless.cmake` は RPC セッションの生きたサンプル、`run_physics_trigger_behavior.ps1` は scene の `behavior` コンポーネント + トリガーイベントの最小実例です
- **CI gate を読む** → [test/ci/run_cpu_gate.py](../../test/ci/run_cpu_gate.py)(CPU ゲートのローカル再現)/ [test/ci/test_golden_inventory.py](../../test/ci/test_golden_inventory.py)(golden 登録の gate)/ [test/contract_boundary_gate.py](../../test/contract_boundary_gate.py)(境界契約の機械 gate。依存の pin や ABI の offset/size をコードから独立に検査)。ワークフロー定義は [.github/workflows/](../../.github/workflows)、運用の正は [../ci.md](../ci.md)
- テスト登録は `pelican_define_test(<name> [libs...])` です([test/CMakeLists.txt](../../test/CMakeLists.txt))。Catch2 の case ごとに登録されるため、ctest の件数は構成によって変わります。GPU 必須テストはデバイス列挙失敗時に SKIP となり、`gpu` label のものは launcher の lease で直列化します([第10章](10_tools.md) §10.9)。過去の全数は[履歴](../history/manual_landing_notes_2026.md)を参照してください。

## 関連文書

- [第1章 エンジン全体像](01_overview.md) — レイヤ規則・設計原則の利用者向け説明
- [第11章 §11.1](11_status.md#111-wp-の台帳はどこにあるか) — WP 台帳・履歴への索引(この章の ⚠注記の出典を辿る入口)
- [../adding_features.md](../adding_features.md) — 機能追加レシピ集(公式 cookbook)
- [../agent_operations.md](../agent_operations.md) — 開発体制・WP 運用
- [../shader_contract.md](../shader_contract.md) — シェーダ契約(実装から読み取った正)
