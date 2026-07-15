# 第12章 コード読解ガイド(技術解説書)

対象: pelican2(2026-07-10 時点)/ このマニュアルはコードを正とする

この章は他章と目的が違います。第1〜11章が「エンジンを**使う**ための説明書」なのに対し、この章は「エンジンの**中身を読む**ための地図」です。すべての節にソースへの相対リンクを張ってあるので、エディタ(VS Code 等)や GitHub 上でクリックしながら読み進められます。

> **リンクの約束:** ファイルリンクはリファクタに強いのでそちらを基本とし、特に重要な箇所のみ行アンカー(`#L212` — GitHub で有効)を付けています。行番号は 2026-07-10 時点のものです。ずれていたら近くの関数名で探してください。

## この章で学ぶこと

- リポジトリのどこに何があるか(ディレクトリ地図)
- 起動から 1 フレーム描画までのコードパス(追跡ルート付き)
- 各 JSON がどのパーサで読まれるか(データ→コード対応表)
- サブシステム別の「最初に開くべきファイル」
- コード全体に繰り返し現れる実装パターン(読むときの目印)

## 12.1 リポジトリ地図

```
pelican2/
  src/
    core/        … エンジン本体(静的ライブラリ pelican_core)
    project/     … 純ロジック層 pelican_project(パーサ・検証。GPU/Vulkan 非依存)
    player/      … pelican_player.exe(main 1 ファイル)
    devcli/      … pelican_cli.exe(project init / import / dist-config)
    devstudio/   … pelican_studio.exe(Qt。現状は休眠骨組み)
  projects/example/ … 実例プロジェクト(全 JSON 形式の生きた見本)
  test/          … Catch2 単体 + golden + fixtures + run_*.cmake 結合テスト
  docs/          … 設計文書(design_*.md)・指示書・本マニュアル
  experiments/   … スパイク(spvlink 等。本体にリンクされない)
```

### src/core のサブモジュール一覧(下層 → 上層の順)

| ディレクトリ | 役割 | まず開くファイル |
|---|---|---|
| [os/](../../src/core/os) | ウィンドウ(GLFW)・入力スナップショット・アクション層 | [inputstate.hpp](../../src/core/os/inputstate.hpp), [actionmap.hpp](../../src/core/os/actionmap.hpp) |
| [vkcore/](../../src/core/vkcore) | Vulkan 低層(デバイス・スワップチェーン・RT・実行) | [core.hpp](../../src/core/vkcore/core.hpp), [renderer.hpp](../../src/core/vkcore/renderer.hpp) |
| [shader/](../../src/core/shader) | シェーダ基盤(コンパイル→リフレクション→ライブラリ→パイプライン) | [shaderlibrary.hpp](../../src/core/shader/shaderlibrary.hpp) |
| [renderingpass/](../../src/core/renderingpass) | rendering config のパーサ群 + フレームグラフ(プランナ/ランタイム) | [frameplanner.hpp](../../src/core/renderingpass/frameplanner.hpp) |
| [renderer/](../../src/core/renderer) | パス種別ごとの描画実装・カメラ・debug 描画・UI 描画 | [materialrender.hpp](../../src/core/renderer/materialrender.hpp), [camera.hpp](../../src/core/renderer/camera.hpp) |
| [fullscreenpass/](../../src/core/fullscreenpass) | fullscreen パスのコンテナ | [fullscreenpasscontainer.hpp](../../src/core/fullscreenpass/fullscreenpasscontainer.hpp) |
| [loader/](../../src/core/loader) | project.json・パス解決・シーンバインド・画像読み込み | [basicconfig.cpp](../../src/core/loader/basicconfig.cpp), [pathresolver.hpp](../../src/core/loader/pathresolver.hpp) |
| [ecs/](../../src/core/ecs) | ECS コア(chunk 実体は userpublic/details/ecs 側)+ エンジン内部システム | [core.hpp](../../src/core/ecs/core.hpp), [predefined/](../../src/core/ecs/predefined) |
| [userpublic/](../../src/core/userpublic) | **ゲームコードに公開される唯一の API 面**(facade) | [gamecontext.hpp](../../src/core/userpublic/gamecontext.hpp) |
| [appflow/](../../src/core/appflow) | フレームループ・時刻・フレームレート・終了処理 | [loop.cpp](../../src/core/appflow/loop.cpp) |
| [phys/](../../src/core/phys) | 物理クエリ(raycast/overlap)と PhysWorld | [physquery.hpp](../../src/core/phys/physquery.hpp) |
| [light/](../../src/core/light) / [material/](../../src/core/material) / [model/](../../src/core/model) / [asset/](../../src/core/asset) | ライト・マテリアル・glTF/VAT・モデル資産の各コンテナ | [gltf.cpp](../../src/core/model/gltf.cpp), [vatformat.hpp](../../src/core/model/vatformat.hpp) |
| [playback/](../../src/core/playback) | transform_seq / VAT の再生 | [seqplayer.cpp](../../src/core/playback/seqplayer.cpp) |
| [audio/](../../src/core/audio) | WAV 再生(miniaudio / null バックエンド) | [audio.cpp](../../src/core/audio/audio.cpp) |
| [communication/](../../src/core/communication) | stdio JSON-RPC サーバ | [rpcserver.cpp](../../src/core/communication/rpcserver.cpp) |
| [resources/](../../src/core/resources) | `engine://` 埋め込みリソースの実体(シェーダ・feature JSON・既定設定) | [features/](../../src/core/resources/features) |
| 直下 | モジュール機構・ハンドル・ログ・起動設定 | [container.hpp](../../src/core/container.hpp), [handle.hpp](../../src/core/handle.hpp), [launchconfig.hpp](../../src/core/launchconfig.hpp) |

## 12.2 レイヤ構造と依存規則

> **設計決定(レイヤ規則):** [vkcore/](../../src/core/vkcore) から [renderingpass/](../../src/core/renderingpass) 以上のレイヤへ新たな include を追加しない。下位層は「何が起きたか」をフラグ/イベントで上に伝えるだけで、「どう対応するか」(編成)は上位層が行う。実例: リサイズ時、[swapchainframetarget.cpp](../../src/core/vkcore/swapchainframetarget.cpp) はフラグを立てるだけで、RT の再生成は上位の [renderer.cpp](../../src/core/vkcore/renderer.cpp) が行う。

> **設計決定(純ロジック分離):** パース・検証・計画のコードは Vulkan・モジュール機構に依存しない plain クラスとして書き、GPU なしの単体テストを可能にする。その集積が [src/project/](../../src/project)(`pelican_project` ターゲット、WP44)で、依存は std + nlohmann_json のみ。[sceneformat.cpp](../../src/project/sceneformat.cpp)・[featurecompose.cpp](../../src/project/featurecompose.cpp)・[jsonrpc.cpp](../../src/project/jsonrpc.cpp)・[importmanifest.cpp](../../src/project/importmanifest.cpp)・[materialformat.cpp](../../src/project/materialformat.cpp) がここに住む。devstudio や外部ツールも将来この層だけをリンクして形式を扱える。

モジュール間の参照は 2 つの機構で行われます(→ §12.7):グローバル取得(`DECLARE_MODULE` / `GET_MODULE`)と、新しいコードで推奨される**依存構造体**([render_pass_dispatch.hpp](../../src/core/vkcore/render_pass_dispatch.hpp) の `XxxDependencies` が手本)。

## 12.3 起動から最初のフレームまで(追跡ルート)

デバッガでステップするならこの順に張ってください。

1. **CLI パース** — [player/main.cpp#L212](../../src/player/main.cpp#L212) `parseLaunchConfig`。argparse で全引数を解釈し、[EngineLaunchConfig](../../src/core/launchconfig.hpp) に詰める。`--project` 省略時の暗黙探索は [main.cpp#L151](../../src/player/main.cpp#L151)(exe の祖先から `projects/example/project.json` を探す)
2. **コア生成** — [main.cpp#L348](../../src/player/main.cpp#L348) `main` → `Pelican::PelicanCore pl{...}`([pelican_core.cpp](../../src/core/userpublic/pelican_core.cpp))。モジュールコンテナに LaunchConfig を書き込み、[PathResolver](../../src/core/loader/pathresolver.hpp)`.setup()` と [ProjectSource](../../src/core/loader/projectsrc.hpp) にプロジェクト JSON を渡す
3. **設定合成** — [basicconfig.cpp#L228](../../src/core/loader/basicconfig.cpp#L228) `validateProjectJson`(schema/version の hard error ゲート)→ CLI > project.json > [default_config.json](../../src/core/resources/default_config.json) の 3 段合成
4. **ループ開始** — `pl.run()` → [loop.cpp#L122](../../src/core/appflow/loop.cpp#L122) `Loop::run`。ここがフレーム編成の唯一の場所で、通常 / headless / rpc の 3 経路に分岐する
5. **描画** — [renderer.cpp#L260](../../src/core/vkcore/renderer.cpp#L260) — ⚠現状は `has_compute` の場合のみフレームグラフのプラン実行で、compute を含まない config はレガシー経路 `executeLegacyRenderingPasses` を通る(統一が WP64、📐登録済み・未実装)
6. **終了処理** — [teardown.cpp](../../src/core/appflow/teardown.cpp) `RuntimeTeardownGuard` が例外経路でも waitIdle → 物理 → ECS → モデルの順に noexcept で掃除

## 12.4 1 フレームの流れ([loop.cpp#L198](../../src/core/appflow/loop.cpp#L198)。headless も同順)

```
①入力スナップショット確定    os/inputstate.cpp   … フレーム内不変・直列化可能
②アクション評価              os/actionmap.cpp    … actions.json のセットスタック
③時刻を進める                appflow/enginetime.cpp … realtime か fixed_step(1/fps)
④イベント配送                userpublic/details/event/ … 前フレームに emit された分(次フレーム頭配送)
⑤ECS 内部システム            ecs/predefined/     … transform 伝播・modelview 等
⑥ゲームシステム update       userpublic/details/system/ … order 昇順→名前辞書順(決定的)
⑦カメラコントローラ          userpublic/cameracontrollersystem.cpp
⑧描画                        vkcore/renderer.cpp → renderer/ 各パス実装
```

- ①: [inputstate.cpp](../../src/core/os/inputstate.cpp) — GLFW コールバックはキューに積むだけで、フレーム先頭で 1 回だけ `InputSnapshot` を確定します(決定性・記録可能性の土台)
- ⑥: ゲームコードの `update(GameContext&)` はここで呼ばれます。登録機構は [details/system/registerer.hpp](../../src/core/userpublic/details/system/registerer.hpp)
- ⑧: パス種別ごとの実装は [materialrender.cpp](../../src/core/renderer/materialrender.cpp)(gbuffer)・[fullscreenpassrenderer.cpp](../../src/core/renderer/fullscreenpassrenderer.cpp)・[shadowdepthpasscontainer.cpp](../../src/core/renderer/shadowdepthpasscontainer.cpp)・[debugdraw.cpp](../../src/core/renderer/debugdraw.cpp)・[debugtext.cpp](../../src/core/renderer/debugtext.cpp)・[uirenderer.cpp](../../src/core/renderer/uirenderer.cpp)

## 12.5 データ → コード対応表(JSON はどこで読まれるか)

| ファイル / 形式 | スキーマ解説 | パーサ(検証・hard error の実装) |
|---|---|---|
| `project.json` | [第3章](03_project_format.md) | [loader/basicconfig.cpp#L228](../../src/core/loader/basicconfig.cpp#L228) |
| パス参照(project:// / engine:// / user:// / #) | [第3章](03_project_format.md) | [loader/pathresolver.cpp](../../src/core/loader/pathresolver.cpp) |
| `*.scene.json`(pelican.scene v1) | [第4章](04_scene_ecs.md) | 純ロジック: [project/sceneformat.cpp#L47](../../src/project/sceneformat.cpp#L47) / バインド: [loader/scene.cpp](../../src/core/loader/scene.cpp) |
| `asset_data.json` | [第5章](05_assets.md) | [loader/basicconfig.cpp#L276](../../src/core/loader/basicconfig.cpp#L276)(パス書き換え) |
| rendering config | [第6章](06_rendering.md) | [renderingpass/](../../src/core/renderingpass) の *jsonparser 群(型分岐: [renderingpassjsonhelpers.cpp#L52](../../src/core/renderingpass/renderingpassjsonhelpers.cpp#L52)) |
| feature fragment(pelican.render_feature) | [第6章](06_rendering.md) | 純ロジック: [project/featurecompose.cpp](../../src/project/featurecompose.cpp) |
| シェーダ stem 参照 | [第6章](06_rendering.md) | [shader/shaderreference.cpp#L51](../../src/core/shader/shaderreference.cpp#L51)(拡張子 hard error はここ) |
| `input/actions.json`(pelican.input_actions) | [第7章](07_input_ui.md) | [os/actionmap.cpp#L436](../../src/core/os/actionmap.cpp#L436) |
| `ui/ui_overlay.json` | [第7章](07_input_ui.md) | [loader/basicconfig.cpp#L291](../../src/core/loader/basicconfig.cpp#L291) → [renderer/uicontainer.cpp](../../src/core/renderer/uicontainer.cpp) |
| collider コンポーネント | [第8章](08_gameplay.md) | [userpublic/components/collider.cpp#L62](../../src/core/userpublic/components/collider.cpp#L62) |
| camera コンポーネント / controller | [第8章](08_gameplay.md) | [renderer/camera.cpp#L218](../../src/core/renderer/camera.cpp#L218) / [#L284](../../src/core/renderer/camera.cpp#L284) |
| pelican.transform_seq(JSONL) | [第5章](05_assets.md) | [playback/seqplayer.cpp#L233](../../src/core/playback/seqplayer.cpp#L233) |
| pelican.vat(GLB extras) | [第5章](05_assets.md) | 純ロジック: [model/vatformat.cpp](../../src/core/model/vatformat.cpp) / 再生: [playback/vatplayer.cpp](../../src/core/playback/vatplayer.cpp) |
| pelican.material(M1・レンダラ未接続) | [第6章](06_rendering.md) | [project/materialformat.cpp](../../src/project/materialformat.cpp) |
| pelican.import(納品 manifest) | [第5章](05_assets.md) | [project/importmanifest.cpp#L86](../../src/project/importmanifest.cpp#L86)(利用者: [devcli/importcommand.cpp](../../src/devcli/importcommand.cpp)) |
| JSON-RPC エンベロープ | [第10章](10_tools.md) | 純ロジック: [project/jsonrpc.cpp](../../src/project/jsonrpc.cpp) / メソッド実装: [communication/rpcserver.cpp#L534](../../src/core/communication/rpcserver.cpp#L534) |
| pelican.frame_plan(出力専用) | [第6章](06_rendering.md) | 生成: [renderingpass/frameplanner.cpp](../../src/core/renderingpass/frameplanner.cpp) |

## 12.6 サブシステム別読解ガイド

### ECS(2 つの顔を持つ)

読み始め: [userpublic/details/ecs/entity.hpp#L14](../../src/core/userpublic/details/ecs/entity.hpp#L14)(世代付き `EntityId` の canonical 定義)→ [coretemplate.cpp#L93](../../src/core/userpublic/details/ecs/coretemplate.cpp#L93)(生成トランザクション)→ [chunk.cpp](../../src/core/userpublic/details/ecs/chunk.cpp)(アーキタイプ別チャンク格納)。

- WP62(R5-core)で世代付き ID・生成 transaction・teardown フェーズが入り、**フレームを跨ぐ生ポインタ保持は禁止**(ID を持ち resolve する)が規約になりました
- [ecs/](../../src/core/ecs) 直下([core.hpp](../../src/core/ecs/core.hpp)・[componentinfo.cpp](../../src/core/ecs/componentinfo.cpp))はエンジン内部視点、[userpublic/details/ecs/](../../src/core/userpublic/details/ecs) は実体テンプレート、という二重構造です。コンポーネントの型登録(scene JSON の `name` → C++ 型の対応)は [componentinfo.cpp](../../src/core/ecs/componentinfo.cpp) + [details/component/registerer.cpp](../../src/core/userpublic/details/component/registerer.cpp)
- エンジン内部システム(transform 伝播・modelview 更新)は [ecs/predefined/](../../src/core/ecs/predefined)。**ゲームシステム**(`PELICAN_REGISTER_SYSTEM`)とは別物です(→ [第8章](08_gameplay.md))

### シェーダ基盤(パイプラインが JSON から生えるまで)

読み順: [shadercompiler.cpp](../../src/core/shader/shadercompiler.cpp)(shaderc で GLSL→SPIR-V。**エラーを throw せず result 返却する唯一の例外**)→ [shaderreflection.cpp](../../src/core/shader/shaderreflection.cpp)(spirv-reflect で descriptor layout を自動生成)→ [shaderlibrary.cpp](../../src/core/shader/shaderlibrary.cpp)(stem 解決・ホットリロードのトランザクション = 失敗時旧版維持)→ [pipelinefactory.cpp](../../src/core/shader/pipelinefactory.cpp)(レイアウトハッシュ共有・`pipeline_cache.bin` 永続化)。

- set 規約(set 0 = frame / 1 = pass input / 2 = material / 3 = 自由枠)の正は [shader_contract.md](../shader_contract.md) と [pelican_sets.hpp](../../src/core/shader/pelican_sets.hpp) + [resources/shaders/include/pelican_sets.glsl](../../src/core/resources/shaders/include/pelican_sets.glsl)
- 旧パイプラインの破棄は即時ではなく [deletionqueue.cpp](../../src/core/vkcore/deletionqueue.cpp)(遅延破棄。in-flight フレームを守る)

### フレームグラフ(宣言 → 計画 → 実行)

読み順: [computetask.cpp](../../src/core/renderingpass/computetask.cpp)(`buffers` / `compute_tasks` の宣言)→ [frameplanner.cpp](../../src/core/renderingpass/frameplanner.cpp)(reads/writes からトポロジカルソート・レベル・バリアを導出。曖昧な writes-writes は hard error)→ [framegraphruntime.cpp](../../src/core/renderingpass/framegraphruntime.cpp)(プラン駆動実行)。

> **設計決定(三層):** ①ユーザーは依存(reads/writes)だけ宣言する ②順序・バリアは機械導出 ③どうしても必要な箇所だけ `after`/`before` で手詰め。順序を手書きさせないことでパス追加が局所変更になる([design_compute_task_graph.md](../design_compute_task_graph.md))。

⚠読むときの注意: [renderer.cpp#L260](../../src/core/vkcore/renderer.cpp#L260) の分岐により、プラン実行系に入るのは compute を含む config のみです(WP64 で一本化予定)。pure-render 構成の実挙動を追うときはレガシー経路を読んでください。

### 入力(4 層のうち実装は 2 層 + 注入)

[inputstate.cpp](../../src/core/os/inputstate.cpp)(L1: スナップショット)→ [actionmap.cpp](../../src/core/os/actionmap.cpp)(L2: アクション評価・セットスタック・エッジ検出)→ [gamecontext.cpp](../../src/core/userpublic/gamecontext.cpp) の `actionPressed` 系。rpc からの合成入力は [rpcserver.cpp](../../src/core/communication/rpcserver.cpp) の `inject_input` がキューに積み、次フレームのスナップショットに合流します。

### ゲーム API 面(facade)

ゲームコードが見てよいのは [userpublic/](../../src/core/userpublic) だけです。全 API の入口 = [gamecontext.hpp#L18](../../src/core/userpublic/gamecontext.hpp#L18)。オブジェクト生成ビルダーは [gameobjects.hpp](../../src/core/userpublic/gameobjects.hpp)(`GameObjects::add().addComponent<...>().finish()`)、システム/イベント登録マクロは [gamesystem.hpp](../../src/core/userpublic/gamesystem.hpp) と [events.hpp](../../src/core/userpublic/events.hpp)、決定的乱数(PCG32)は [deterministicrng.cpp](../../src/core/userpublic/deterministicrng.cpp)。

### RPC(外部ツールの操作面)

エンベロープ検証(JSON-RPC 2.0 / NDJSON)は純ロジック [project/jsonrpc.cpp](../../src/project/jsonrpc.cpp)、メソッド実装とエンジンへのバインドは [rpcserver.cpp#L534](../../src/core/communication/rpcserver.cpp#L534) の `runEngineRpcServer`。`RpcServer(istream, ostream) + setHandler` の汎用ディスパッチャ構造なので、メソッド追加はハンドラ登録 1 箇所です。OFF ビルド時のスタブは [rpcserver_stub.cpp](../../src/core/communication/rpcserver_stub.cpp)。

### ツール(devcli)

[devcli/main.cpp](../../src/devcli/main.cpp) がサブコマンド分岐、実装は [projectinit.cpp](../../src/devcli/projectinit.cpp)(雛形 15 ファイル生成)・[importcommand.cpp](../../src/devcli/importcommand.cpp)(sha256 照合 → asset_data.json 追記・冪等)・[distconfig.cpp](../../src/devcli/distconfig.cpp)(プロジェクト内容から PELICAN_WITH_* を導出。GLB の JSON チャンクを直接パースして VAT 有無を判定する箇所が読みどころ)。

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
| CLI 引数を足す | [player/main.cpp#L212](../../src/player/main.cpp#L212) + [launchconfig.hpp](../../src/core/launchconfig.hpp) | [第10章](10_tools.md) |
| scene に書ける新コンポーネント | [userpublic/components/](../../src/core/userpublic/components)(collider が手本)+ ComponentInfo 登録 | [第4章](04_scene_ecs.md) |
| ポストエフェクトを足す | feature fragment JSON + stem シェーダ(エンジンコード不要のことが多い) | [第6章](06_rendering.md) |
| compute パスを足す | config の `buffers`/`compute_tasks` + `.comp` stem(コード不要) | [第6章](06_rendering.md) |
| rpc メソッドを足す | [rpcserver.cpp#L534](../../src/core/communication/rpcserver.cpp#L534) にハンドラ追加 | [第10章](10_tools.md) |
| 新しい交換形式(JSON)を足す | [src/project/](../../src/project) に純ロジックパーサ + fixture + schema/version ゲート | [第1章](01_overview.md) 原則 |
| engine:// リソースを足す | 3+1 チェックリスト([adding_features.md](../adding_features.md)): b_embed / registered_ids / fixture / (web 鏡像) | [第9章](09_web.md) |
| ゲームの毎フレーム処理 | プロジェクトの `code/` に `PELICAN_REGISTER_SYSTEM`(エンジン側は触らない) | [第8章](08_gameplay.md) |

機能追加の正式レシピ集は [adding_features.md](../adding_features.md) にあります(本表はその読解入口です)。

## 12.9 テストから読む(テストは実行可能な仕様)

- **形式の仕様を知りたい** → [test/fixtures/](../../test/fixtures) の valid/invalid ペア + `expectations.json`(error_kind 付き)。パーサが何を受理し何を拒むかの正確な一覧です
- **描画の正解を知りたい** → [test/golden/](../../test/golden)(16 ケース。case.json + expected.png + tolerance.json)。`clear` が最小、`feature_compose` や `vat_playback` が応用
- **ツールの使い方の実例** → [test/](../../test) の `run_*.cmake`(player / pelican_cli を実際に子プロセス起動する結合テスト)。特に `run_rpc_headless.cmake` は RPC セッションの生きたサンプル
- テスト登録は `pelican_define_test(<name> [libs...])`([test/CMakeLists.txt](../../test/CMakeLists.txt))。GPU 必須テストはデバイス列挙失敗時 SKIP

## 関連文書

- [第1章 エンジン全体像](01_overview.md) — レイヤ規則・設計原則の利用者向け説明
- [第11章 実装状況と文書マップ](11_status.md) — WP 台帳(この章の ⚠注記の出典)
- [../adding_features.md](../adding_features.md) — 機能追加レシピ集(公式 cookbook)
- [../agent_operations.md](../agent_operations.md) — 開発体制・WP 運用
- [../shader_contract.md](../shader_contract.md) — シェーダ契約(実装から読み取った正)
