# 第2章 起動・モジュール・1フレーム

[索引へ戻る](README.md) / [前章](01_architecture_and_build.md)

## 2.1 起動経路の全体

`pelican_player` の起動は、次の三段に分かれます。

```mermaid
sequenceDiagram
    participant Main as player/main.cpp
    participant Core as PelicanCore
    participant Modules as FastModuleContainer
    participant Scene as SceneLoader
    participant Loop as Loop

    Main->>Main: CLIをParsedLaunchConfigへ変換
    Main->>Core: PelicanCoreを生成（logger初期化）
    Main->>Modules: EngineLaunchConfig / PathResolver / ProjectSourceを設定
    Main->>Core: run()
    Core->>Modules: settings・保存設定・ECSを初期化
    Core->>Scene: default sceneをload
    Core->>Loop: run()
    Loop-->>Core: window終了 / headless完了 / RPC EOF
    Core->>Modules: GPU待機、runtime teardown
    Modules-->>Modules: 生成逆順でmodule破棄
```

### 段階A: `main()` でプロセス条件を確定

入口は [`src/player/main.cpp` の `main()`](../../src/player/main.cpp#L353) です。先に [`parseLaunchConfig()`](../../src/player/main.cpp#L212) が次を決めます。

- windowedかheadlessか
- RPCを使うか
- headlessの解像度・フレーム数・固定FPS
- project rootと`project.json`
- asset検証をstrictにするか
- render出力、frame plan dump
- sequence/VAT再生とcamera override

`--project` がなければ、実行ファイルの祖先から `projects/example/project.json` を探索します（[`configureImplicitProject()`](../../src/player/main.cpp#L151)）。明示projectなら、directoryまたは`project.json`そのものを受け付けます（[`configureExplicitProject()`](../../src/player/main.cpp#L169)）。

### 段階B: `run()` 前に共有moduleへ起動情報を注入

`main()` は [`PelicanCore`](../../src/core/userpublic/pelican_core.hpp#L7) を作った後、次の順でmoduleを設定します。

1. `EngineLaunchConfig`へCLI結果を代入。
2. `PathResolver::setup()`へproject root、絶対パス方針、project JSON、user directoryを渡す。
3. [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L11) でasset store manifestを検査。
4. `ProjectSource`へproject JSONとengine version無視方針を保存。
5. `PelicanCore::run()`を呼ぶ。

この「`run()`前のmodule設定」があるため、`PelicanCore`だけを見てもproject rootの受け渡しは見つかりません。入口とcoreの両方を読む必要があります。

### 段階C: `PelicanCore::run()` でruntimeを組み立てる

[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L32) は短いですが、初期化の正規順序を定義しています。

1. ローカルな `FastModuleContainer` と `RuntimeTeardownGuard` を生成。
2. constructorへ渡されたsettings JSONを`ProjectSource`の上書きsourceへ設定。
3. `Persistence`からsettingsを読み、audio設定を適用。
4. [`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L18) で組み込みComponent/Systemを登録。
5. `ProjectBasicConfig.defaultSceneId()` のsceneを即時ロード。
6. `Loop::run()`へ入る。
7. loop終了後、Vulkan deviceをidleまで待つ。
8. teardown guardでruntime資源を順序付き解放。
9. 関数を抜けるとmodule containerがmoduleを生成逆順に破棄。

標準例外も非標準例外もここで捕捉され、ログを出して`false`を返します。したがって、playerの終了コードは `pl.run() ? 0 : 1` です。

## 2.2 `DECLARE_MODULE` / `GET_MODULE` の正体

定義は [`src/core/container.hpp`](../../src/core/container.hpp#L8) です。

```cpp
#define DECLARE_MODULE(name) class name : public ModuleBase<name>
#define GET_MODULE(name) FastModuleContainer::get<name>()
```

### 保存場所

各module型`T`は、CRTP基底 [`ModuleBase<T>::__get()`](../../src/core/container.hpp#L13) が返す関数ローカルstaticな `std::optional<T>` に実体を持ちます。これは「containerインスタンスのメンバ」ではなく、型ごとのプロセス内staticです。

### 遅延生成

[`FastModuleContainer::get<T>()`](../../src/core/container.hpp#L25) はoptionalが空なら`emplace()`し、破棄関数をstaticな`cleaners`へ積みます。従って、**最初に`GET_MODULE(T)`を呼んだ瞬間がTのconstructor実行時点**です。

例として `Renderer` のconstructorは [`loadDefaultRenderingPassFromConfig()`](../../src/core/vkcore/renderer.cpp#L443) しか呼んでいないように見えますが、その内部で次のmoduleが連鎖的に生成されます。

```text
Renderer
└─ RenderingPassContainer
└─ ProjectBasicConfig
   └─ ProjectSource
   └─ PathResolver
└─ RenderTarget
   └─ OffscreenFrameTarget または SwapchainFrameTarget
      └─ VulkanManageCore
└─ RenderTargetContainer
└─ ShaderLibrary
└─ FullscreenPassContainer
└─ ComputeTaskContainer / FrameGraphResourceContainer / ...
```

### 破棄順

[`~FastModuleContainer()`](../../src/core/container.hpp#L36) は`cleaners`を後ろからpopします。つまり基本は**初期化の逆順**です。ただしGPU資源の安全な解放には「単に逆順」だけでは足りないため、後述の明示teardownを先に実行します。

### 注意点

- `get<T>()`と`cleaners`更新はmutexで保護されていません。module生成は基本的にmain threadで済ませてからjobを走らせる前提です。
- constructor内の`GET_MODULE()`が隠れた依存になります。調査時はconstructorと全`GET_MODULE`呼び出しをセットで検索します。
- `FastModuleContainer`を複数作ってもmodule実体は型ごとのstaticです。テストのcontainerはスコープ終了時に登録済みmoduleを全消去するための寿命ガードとして使われます。
- 先に`main()`で作られたmoduleも同じstatic `cleaners`へ載るため、`PelicanCore::run()`内のcontainer破棄時にまとめて片付けられます。

## 2.3 明示teardownが必要な理由

moduleのC++ destructorだけに任せると、ECS Componentの`deinit()`が参照する描画moduleが先に壊れる可能性があります。そこで [`teardownRuntimeNoThrow()`](../../src/core/appflow/teardown.cpp#L25) が、module optionalの存在を確認して次の順で明示解放します。

1. Vulkan `waitIdle()`
2. physics bindingをclear
3. ECS entityをclearし、全Componentの`deinit()`とdestructorを実行
4. model instanceをclear

各段階は例外を飲み込みつつログを残します。[`RuntimeTeardownGuard::~RuntimeTeardownGuard()`](../../src/core/appflow/teardown.cpp#L48) も`run()`を呼ぶため、loopや初期化の途中で例外が出ても同じ順序を通ります。

この契約は [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L453) で「Componentのdeinit/destroyがmodule destructorより先」として検証されています。

## 2.4 `Loop::run()` の三モード

中心は [`Loop::run()`](../../src/core/appflow/loop.cpp#L118) です。

### windowed

```text
Window::process
→ window eventをInputStateへqueue
→ EngineTime::advance(realtime)
→ updateFrameState
→ Renderer::render
→ FramerateAdjust::wait
```

GLFW callbackは [`Window`](../../src/core/os/window.hpp#L15) の`input_events`へイベントを積み、loopが`drainInputEvents()`して`InputState`へ渡します。ゲームロジックは直接GLFW状態を問い合わせません。

### headless固定フレーム

```text
InputState::clear
→ EngineTime::advance(fixed_step)
→ updateFrameState
→ Renderer::render
→ 必要ならPNG capture
```

`headless_frames == 0`なら無制限です。出力パスに`%d`または`%0Nd`があればフレームごとに保存し、なければ最後のフレームだけ保存します（[`parseRenderOutPattern()`](../../src/core/appflow/loop.cpp#L42)）。

### headless RPC

RPCモードでは通常のfor-loopへ入らず、[`runEngineRpcServer(std::cin, std::cout)`](../../src/core/appflow/loop.cpp#L141) を呼びます。フレーム進行はクライアントの`step_frame`要求が所有します。stdoutはNDJSON protocol専用なので、[`PelicanCore` constructor](../../src/core/userpublic/pelican_core.cpp#L25) がloggerをprotocol対応で初期化します。

## 2.5 1フレームの五つの状態フェーズ

順序は [`frame_phase_order`](../../src/core/appflow/framephase.hpp#L17)、実装は [`updateFrameState()`](../../src/core/appflow/framephase.cpp#L17) です。

| 順 | `FramePhase` | 実際の処理 | 保証 |
|---:|---|---|---|
| 1 | `freeze_events` | pending eventを今回配送分へswap | この後emitされたeventは次フレーム |
| 2 | `freeze_input` | pending input eventsから`InputSnapshot`を作る | フレーム中の生入力が不変 |
| 3 | `freeze_actions` | Action mapを一回だけ評価 | 全Systemが同じAction値を見る |
| 4 | `deliver_events` | frozen eventをゲームSystemへ登録順にdispatch | event handlerがupdateより先 |
| 5 | `update_game` | 内部ECS → ゲームSystem → sequence → pending scene load | 構造変更の境界を一本化 |

`update_game`内部の正確な順序は次です。

```text
ECSCore::update()
→ updateRegisteredGameSystems(GameContext&)
→ SeqPlayer::update(current_time)
→ SceneLoader::applyPendingLoad()
```

### eventの1フレーム遅延

`GameContext::emit()`はeventを`pending_events`へ積みます（[`emit()`](../../src/core/userpublic/details/event/registerer.hpp#L79)）。次のフレーム冒頭で[`freezePendingEventsForFrame()`](../../src/core/userpublic/details/event/registerer.cpp#L89)が`deliver_now_events`へswapし、配送します。

- update中にemit → 次フレームで配送
- event handler内で別eventをemit → さらに次フレームで配送
- scene load完了時の`SceneLoaded` → loadがフレーム末尾なら次フレームで配送

この性質により、配送中vectorの再確保や再入dispatchを避けています。テストは [`eventlayer_test.cpp`](../../test/eventlayer_test.cpp#L48) です。

### inputの1フレーム固定

[`InputStateCore::beginFrame()`](../../src/core/os/inputstate.cpp#L229) がpending eventをframe eventへswapし、down/pushed/released/mouse deltaを作ります。Action層は [`freezeInputActionsFrame()`](../../src/core/userpublic/userinput.cpp#L227) で消費maskを適用したsnapshotを評価します。以後の`Actions::*`は再計算せず同じ`InputActionFrame`を返します。

## 2.6 EngineTime

[`EngineTime`](../../src/core/appflow/enginetime.hpp#L10) には二モードあります。

- `realtime`: `steady_clock`差分。異常に長い停止は0.1秒へclamp（[`advance()`](../../src/core/appflow/enginetime.cpp#L32)）。
- `fixed_step`: `1 / launch_config.fps`を毎回加算。headless/RPCの再現性に使う。

`advance()`後に`current_time += delta_time`、`frame_index++`です。最初の更新フレームはindex 1になります。RPCの`set_time`は時刻だけを直接変更し、deltaを0へ戻します。

## 2.7 ゲームSystemと内部ECS Systemは別の更新列

名前は似ていますが、二種類あります。

| 種類 | 登録 | 呼び出し | 主用途 |
|---|---|---|---|
| 内部ECS System | [`ECSCore::registerSystem`](../../src/core/ecs/core.hpp#L30) | `ECSCore::update()` | Component配列をまとめて処理 |
| ゲームSystem | [`PELICAN_REGISTER_SYSTEM`](../../src/core/userpublic/details/system/registerer.hpp#L128) | `updateRegisteredGameSystems()` | `GameContext`経由のゲームロジック |

内部ECSが先に走ります。そのため、ゲームSystemが同フレーム中に`LocalTransform`を書き換えた結果を内部transform Systemが見るのは原則次フレームです。ただし [`GameObjects::setLocalTransform()`](../../src/core/userpublic/gameobjects.cpp#L38) は公開componentと内部`TransformComponent`を即時同期し、model instanceへも直接TRSを反映するため、公開API経由の通常操作では表示遅延を避けています。

## 2.8 描画フレーム

状態更新後に [`Renderer::render()`](../../src/core/vkcore/renderer.cpp#L472) が呼ばれます。

1. `DeletionQueue.beginFrame()`で安全になった旧GPU資源を解放。
2. shader hot reloadを検査し、必要ならpipelineを再構築。
3. light animation/UBOを更新。
4. `RenderTarget.render_begin()`でcommand bufferとframe attachmentを取得。
5. resizeがあればoffscreen RT再生成、descriptor再bind、layout tracker reset。
6. frame graph計画順でrender/compute nodeを実行。
7. `RenderTarget.render_end()`でsubmit/presentまたはoffscreen完了。

この中身は[第6章](06_rendering_vulkan_shader.md)で分解します。

## 2.9 例外の境界

- 形式の不正、GPU初期化失敗、Component不正は基本的に`std::runtime_error`でfail-fastです。
- `PelicanCore::run()`がruntime全体の最終catchです。
- job workerの例外は [`JobSystem`](../../src/core/job_system.cpp#L31) が`exception_ptr`に保持し、main threadの`wait()`で再throwします。
- shader hot reload失敗だけは旧shader/pipelineを維持してwarningにします（[`ShaderLibrary::reload()`](../../src/core/shader/shaderlibrary.cpp#L293)、[`PipelineFactory::rebuildDirty()`](../../src/core/shader/pipelinefactory.cpp#L424)）。
- teardownは例外を外へ出しません。

この違いは「初回構築に失敗した不完全なruntimeは続けないが、稼働中の編集失敗では最後の正常版を守る」という方針です。
