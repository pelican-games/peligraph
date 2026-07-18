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

入口は [`src/player/main.cpp` の `main()`](../../src/player/main.cpp#L433) です。先に [`parseLaunchConfig()`](../../src/player/main.cpp#L226) が次を決めます。

- windowedかheadlessか
- RPCを使うか
- headlessの解像度・フレーム数・固定FPS
- project rootと`project.json`
- asset検証をstrictにするか
- render出力、frame plan dump（`--dump-frame-plan`）
- sequence/VAT再生とcamera override
- XRモード（`--xr off|auto|on`）
- game logic DLL（`--game-logic`）
- 入力の記録/再生（`--record-input` / `--replay` / `--input-profile`）
- camera bake（`--bake-camera-output`、headless + replay 必須）

`--project` がなければ、実行ファイルの祖先から `projects/example/project.json` を探索します（[`configureImplicitProject()`](../../src/player/main.cpp#L182)）。明示projectなら、directoryまたは`project.json`そのものを受け付けます（[`configureExplicitProject()`](../../src/player/main.cpp#L196)）。

### 段階B: `run()` 前に共有moduleへ起動情報を注入

`main()` は [`PelicanCore`](../../src/core/userpublic/pelican_core.hpp#L7) を作った後、次の順でmoduleを設定します。

1. `EngineLaunchConfig`へCLI結果を代入。
2. `PathResolver::setup()`へproject root、絶対パス方針、project JSON、user directoryを渡す。
3. [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L11) でasset store manifestを検査。
4. `ProjectSource`へproject JSONとengine version無視方針を保存。
5. `PelicanCore::run()`を呼ぶ。

この「`run()`前のmodule設定」があるため、`PelicanCore`だけを見てもproject rootの受け渡しは見つかりません。入口とcoreの両方を読む必要があります。

### 段階C: `PelicanCore::run()` でruntimeを組み立てる

[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L44) は短いですが、初期化の正規順序を定義しています。

1. ローカルな `FastModuleContainer` と `RuntimeTeardownGuard` を生成。
2. `StartupMetrics` を開始。
3. **XR activationを決定**: [`resolveXrActivation()`](../../src/core/xractivation.hpp) がheadless/RPC/replay等のforced-offとOpenXR discovery hookから `launch_config.xr_active` を確定。
4. constructorへ渡されたsettings JSONを`ProjectSource`の上書きsourceへ設定し、[`watch::ReloadGate.configureFromLaunch()`](../../src/core/watch/reloadgate.hpp) を呼ぶ。
5. `Persistence`からsettingsを読み、audio設定を適用。
6. [`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L18) で組み込みComponent/Systemを登録。
7. **[`initializeConfiguredGameLogic()`](../../src/core/gamelogic/gamelogicreload.cpp#L371) でgame DLLをロード**（WP110系列）。
8. `ProjectBasicConfig.defaultSceneId()` のsceneを即時ロードし、`ModelAssetContainer` を明示的に先行生成（並列prepareのcommitを起動スレッドで実施）。
9. [`watch::ReloadService.setup()`](../../src/core/watch/reloadservice.hpp#L75) でwatcherのlive inventoryを種付け。
10. `Loop::run()`へ入る。
11. loop終了後、Vulkan deviceをidleまで待つ。
12. `FastModuleContainer::beginShutdown()` → teardown guardでruntime資源を順序付き解放 → `shutdownConfiguredGameLogic()`。
13. 関数を抜けるとmodule containerがmoduleを生成逆順に破棄。

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

例として `Renderer` のconstructorは [`loadDefaultRenderingPassFromConfig()`](../../src/core/vkcore/renderer.cpp#L965) しか呼んでいないように見えますが、その内部で次のmoduleが連鎖的に生成されます。ただし現在は、[`Renderer::prepareRuntimeModules()`](../../src/core/vkcore/renderer.hpp#L93) と [`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L312) により「render前に依存を全解決してから凍結する」方式へ変わっています。

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

- `get<T>()`と`cleaners`更新は [`std::recursive_mutex state_mutex`](../../src/core/container.hpp#L62) で保護されています（各APIがscoped_lockを取る）。それでもmodule生成は基本的にmain threadで済ませてからjobを走らせる前提です。
- [`FastModuleContainer::freezeCreation()`](../../src/core/container.hpp#L199) がLoop開始直前に呼ばれ（[loop.cpp](../../src/core/appflow/loop.cpp#L325)）、以後の新規module生成はエラーになります。[`tryGet<T>()`](../../src/core/container.hpp#L144) は生成せずoptional参照を返します。`graphSnapshot()` がmodule依存グラフを記録します。
- constructor内の`GET_MODULE()`が隠れた依存になります。調査時はconstructorと全`GET_MODULE`呼び出しをセットで検索します。
- `FastModuleContainer`を複数作ってもmodule実体は型ごとのstaticです。テストのcontainerはスコープ終了時に登録済みmoduleを全消去するための寿命ガードとして使われます。
- 先に`main()`で作られたmoduleも同じstatic `cleaners`へ載るため、`PelicanCore::run()`内のcontainer破棄時にまとめて片付けられます。

## 2.3 明示teardownが必要な理由

moduleのC++ destructorだけに任せると、ECS Componentの`deinit()`が参照する描画moduleが先に壊れるほか、pending/frozen event、behavior deferred mutation、GPU deletion callbackがowner/DLLの破棄後まで残る可能性があります。そこで [`teardownRuntimeNoThrow()`](../../src/core/appflow/teardown.cpp) が、module optionalの存在を確認して次の順で明示解放します。

1. Vulkan `waitIdle()`
2. behavior owner callbackを完了
3. physics bindingをclear（現在は `#if PELICAN_WITH_PHYSICS` 内）
4. ECS entityをclearし、全Componentの`deinit()`とdestructorを実行
5. model instanceをclear
6. behavior deferred mutationをdrain
7. pending/frozen eventをdrain
8. GPU deletion queueをdrain

terminal shutdownはこの前に`FastModuleContainer::beginShutdown()`で新規module生成を閉じ、最後のdeletion queueもcloseします。game-logic reloadの`runtime_reset`は同じ順で既存workをdrainしますが、module phaseとdeletion queueを再利用可能に保ちます。

各段階は例外を飲み込みつつログを残します。[`RuntimeTeardownGuard::~RuntimeTeardownGuard()`](../../src/core/appflow/teardown.cpp) も`run()`を呼ぶため、loopや初期化の途中で例外が出ても同じ順序を通ります。

この契約は [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp) の「Componentのdeinit/destroyがmodule destructorより先」と、[`lifetime_teardown_test.cpp`](../../test/lifetime_teardown_test.cpp) の全工程fault injection、起動順列、起動途中失敗で検証されています。

## 2.4 `Loop::run()` の四経路

中心は [`Loop::run()`](../../src/core/appflow/loop.cpp#L283) です。loop本体へ入る前に、入力のrecord/replay（[`InputSequenceRuntime`](../../src/core/os/inputsequence.hpp#L45)、[loop.cpp](../../src/core/appflow/loop.cpp#L298)）、replay時のfixed_step切替、camera bakeの開始（[同 L309](../../src/core/appflow/loop.cpp#L309)）、`prepareRuntimeModuleGraph()` と [`FastModuleContainer::freezeCreation()`](../../src/core/appflow/loop.cpp#L325) が実行されます。

### windowed

```text
Window::process
→ window eventをInputStateへqueue
→ EngineTime::advance(realtime)
→ updateFrameState
→ Renderer::render
→ FramerateAdjust::wait
```

GLFW callbackは [`Window`](../../src/core/os/window.hpp#L15) の`input_events`へイベントを積み、loopが`drainInputEvents()`して`InputState`へ渡します。ゲームロジックは直接GLFW状態を問い合わせません。windowedではF5キーでgame logic reloadを要求できます（[loop.cpp](../../src/core/appflow/loop.cpp#L391)）。`FramerateAdjust`はwindowedのみです。

### windowed + XRセッション実行中

`launch_config.xr_active` かつXRセッションがrunningの間は、同じwhileループ内で専用経路を通ります（[loop.cpp](../../src/core/appflow/loop.cpp#L405)、`#if PELICAN_WITH_OPENXR`）。

```text
pollEvents → waitFrame → EngineTime::advance → beginFrame → locateViews
→ syncActions（action backendとpose samplesをInputStateへ注入）
→ updateFrameState
→ selectGraphVariant(xr) → XrCompositionTarget.prepareFrame
→ renderLogicalFrame(2 views) → XrMirrorSink.tryPresent
```

両eyeのposeはこのフレーム境界で取得したactive cameraへanchorされます（WP131、[`buildRenderViewParameters`](../../src/core/openxr/openxrviewspace.hpp)）。

### headless固定フレーム

```text
InputState::clear（replay中はスキップ）
→ EngineTime::advance(fixed_step)
→ updateFrameState
→ Renderer::render
→ 必要ならPNG capture
```

`headless_frames == 0`なら無制限です。replay時はフレーム数をreplayから導出します（[loop.cpp](../../src/core/appflow/loop.cpp#L341)）。出力パスに`%d`または`%0Nd`があればフレームごとに保存し、なければ最後のフレームだけ保存します（[`parseRenderOutPattern()`](../../src/core/appflow/loop.cpp#L71)）。

### headless RPC

RPCモードでは通常のfor-loopへ入らず、[`runEngineRpcServer(std::cin, std::cout)`](../../src/core/appflow/loop.cpp#L334) を呼びます。フレーム進行はクライアントの`step_frame`要求が所有します。stdoutはNDJSON protocol専用なので、[`PelicanCore` constructor](../../src/core/userpublic/pelican_core.cpp#L39) がloggerをprotocol対応で初期化します。

## 2.5 1フレームの五つの状態フェーズ

順序は [`frame_phase_order`](../../src/core/appflow/framephase.hpp#L17)、実装は [`updateFrameState()`](../../src/core/appflow/framephase.cpp#L81) です。フレーム冒頭（フェーズ前）には [`ReloadService.applyFrame()`](../../src/core/appflow/framephase.cpp#L85) が走り、reloadの公開はフレーム境界に揃えられます。

| 順 | `FramePhase` | 実際の処理 | 保証 |
|---:|---|---|---|
| 1 | `freeze_events` | pending eventを今回配送分へswap | この後emitされたeventは次フレーム |
| 2 | `freeze_input` | `input_sequence.prepareFrame()` → `beginFrame()` → `recordFrame()`（record/replay挿入点） | フレーム中の生入力が不変 |
| 3 | `freeze_actions` | ImGuiへの入力ルーティング → `ui::UiModule.routeFrameInput()`（toolがポインタを消費していない場合）→ Action mapを一回だけ評価 | 全Systemが同じAction値を見る |
| 4 | `deliver_events` | frozen eventをゲームSystemへ登録順にdispatch | event handlerがupdateより先 |
| 5 | `update_game` | 内部ECS → ゲームSystem → animation → sequence → pending scene load | 構造変更の境界を一本化 |

`update_game`内部の正確な順序は次です（[framephase.cpp](../../src/core/appflow/framephase.cpp#L117)）。

```text
ECSCore::update()
→ updateRegisteredGameSystems(GameContext&)
→ Vrm::applicationServiceRuntime().ensureStandardPhasesRegistered()
→ Animation::animationServiceRuntime().runAllPhases(frame_index)
→ SeqPlayer::update(current_time)
→ SceneLoader::applyPendingLoad()
```

フェーズ後には `CameraBakeRecorder` が（有効時のみ）フレームを記録します（[framephase.cpp](../../src/core/appflow/framephase.cpp#L140)）。

### eventの1フレーム遅延

`GameContext::emit()`はeventを`pending_events`へ積みます（[`emit()`](../../src/core/userpublic/details/event/registerer.hpp#L138)）。次のフレーム冒頭で[`freezePendingEventsForFrame()`](../../src/core/userpublic/details/event/registerer.cpp#L383)が`deliver_now_events`へswapし、配送します。

- update中にemit → 次フレームで配送
- event handler内で別eventをemit → さらに次フレームで配送
- scene load完了時の`SceneLoaded` → loadがフレーム末尾なら次フレームで配送

この性質により、配送中vectorの再確保や再入dispatchを避けています。テストは [`eventlayer_test.cpp`](../../test/eventlayer_test.cpp#L48) です。

### inputの1フレーム固定

[`InputStateCore::beginFrame()`](../../src/core/os/inputstate.cpp#L373) がpending eventをframe eventへswapし、down/pushed/released/mouse deltaを作ります。Action層は [`freezeInputActionsFrame()`](../../src/core/userpublic/userinput.cpp#L294) で消費maskを適用したsnapshotを評価します。以後の`Actions::*`は再計算せず同じ`InputActionFrame`を返します。

## 2.6 EngineTime

[`EngineTime`](../../src/core/appflow/enginetime.hpp#L10) には二モードあります。

- `realtime`: `steady_clock`差分。異常に長い停止は0.1秒へclamp（[`advance()`](../../src/core/appflow/enginetime.cpp#L30)）。
- `fixed_step`: `1 / launch_config.fps`を毎回加算。headless/RPC/replayの再現性に使う。

`advance()`後に`current_time += delta_time`、`frame_index++`です。最初の更新フレームはindex 1になります。RPCの`set_time`は時刻だけを直接変更し、deltaを0へ戻します。この不連続は [`timeSetRevision()`](../../src/core/appflow/enginetime.cpp#L57) で観測でき、rendererはこれを検知してtemporal historyをリセットします（[renderer.cpp](../../src/core/vkcore/renderer.cpp#L1110)）。

## 2.7 ゲームSystemと内部ECS Systemは別の更新列

名前は似ていますが、二種類あります。

| 種類 | 登録 | 呼び出し | 主用途 |
|---|---|---|---|
| 内部ECS System | [`ECSCore::registerSystem`](../../src/core/ecs/core.hpp#L30) | `ECSCore::update()` | Component配列をまとめて処理 |
| ゲームSystem | [`PELICAN_REGISTER_SYSTEM`](../../src/core/userpublic/details/system/registerer.hpp#L151) | `updateRegisteredGameSystems()` | `GameContext`経由のゲームロジック |

内部ECSが先に走ります。そのため、ゲームSystemが同フレーム中に`LocalTransform`を書き換えた結果を内部transform Systemが見るのは原則次フレームです。ただし [`GameObjects::setLocalTransform()`](../../src/core/userpublic/gameobjects.cpp#L38) は公開componentと内部`TransformComponent`を即時同期し、model instanceへも直接TRSを反映するため、公開API経由の通常操作では表示遅延を避けています。

## 2.8 描画フレーム

状態更新後に [`Renderer::render()`](../../src/core/vkcore/renderer.cpp#L1244) が呼ばれます。WP128以降、`render()`は1-viewのアダプタで、実体は [`Renderer::renderLogicalFrame()`](../../src/core/vkcore/renderer.cpp#L1082) です。

1. `DeletionQueue.beginFrame()`で安全になった旧GPU資源を解放。
2. view数変化・`set_time`・camera不連続を検知してtemporal historyをリセット。
3. shader reload publicationをconsumeし、必要ならfullscreen入力を再bind。
4. light animationを更新。
5. `target.beginLogicalFrame(view_count)`後、view毎に`beginView()`でcommand bufferとframe attachmentを取得。
6. view 0でresize処理とinstance凍結を行い、view毎にframe graph計画順でrender/compute nodeを実行。
7. `endView()`/`endLogicalFrame()`でsubmit/presentまたはXR composition完了。RT historyのflipとsnapshot commitはフレーム末尾に一回。

flat画面では`render()`がactive Cameraを1-view providerとして渡します。この中身は[第6章](06_rendering_vulkan_shader.md)で分解します。

## 2.9 例外の境界

- 形式の不正、GPU初期化失敗、Component不正は基本的に`std::runtime_error`でfail-fastです。
- `PelicanCore::run()`がruntime全体の最終catchです。
- job workerの例外は [`JobSystem`](../../src/core/job_system.cpp#L31) が`exception_ptr`に保持し、main threadの`wait()`で再throwします。
- shader hot reload失敗だけは旧shader/pipelineを維持してwarningにします（[`ShaderLibrary::prepareReload()`](../../src/core/shader/shaderlibrary.cpp#L593)、[`PipelineFactory::rebuildPrepared()`](../../src/core/shader/pipelinefactory.cpp#L492)）。
- teardownは例外を外へ出しません。

この違いは「初回構築に失敗した不完全なruntimeは続けないが、稼働中の編集失敗では最後の正常版を守る」という方針です。
