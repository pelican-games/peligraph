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

入口は [`src/player/main.cpp` の `main()`](../../src/player/main.cpp#L434) です。先に [`parseLaunchConfig()`](../../src/player/main.cpp#L226) が次を決めます。

- windowedかheadlessか
- RPCを使うか（`--rpc`）
- headlessの解像度・フレーム数・固定FPS
- project rootと`project.json`
- asset検証をstrictにするか
- render出力、frame plan dump（`--dump-frame-plan`）
- Vulkanのdebugオブジェクト名とコマンドラベル（`--gpu-labels` → [`EngineLaunchConfig::gpu_labels`](../../src/core/launchconfig.hpp#L45)）
- sequence/VAT再生とcamera override
- XRモード（`--xr off|auto|on`）
- game logic DLL（`--game-logic`）
- 入力の記録/再生（`--record-input` / `--replay` / `--input-profile`）
- camera bake（`--bake-camera-output`、headless + replay 必須）

> **設計決定:** `--rpc` は `--headless` を要求しません（WP156）。ヘルプ文言そのものが現在の契約です — `enable stdio JSON-RPC (blocking in headless, frame-boundary in windowed mode)`（[`main.cpp`](../../src/player/main.cpp#L229)）。headlessではRPCがフレーム進行を所有し、windowedではフレーム境界でだけdispatchされます。両者の違いは §2.4 で分解します。

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
6. [`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L20) で組み込みComponent/Systemを登録。
7. **[`initializeConfiguredGameLogic()`](../../src/core/gamelogic/gamelogicreload.cpp#L428) でgame DLLをロード**（WP110系列）。
8. `ProjectBasicConfig.defaultSceneId()` のsceneを即時ロードし、`ModelAssetContainer` を明示的に先行生成（並列prepareのcommitを起動スレッドで実施）。
9. [`watch::ReloadService.setup()`](../../src/core/watch/reloadservice.hpp#L75) でwatcherのlive inventoryを種付け。
10. `Loop::run()`へ入る。
11. loop終了後、Vulkan deviceをidleまで待つ。
12. [`teardown.run()`](../../src/core/userpublic/pelican_core.cpp#L98) でruntime資源を順序付き解放し、続けて [`shutdownConfiguredGameLogic()`](../../src/core/userpublic/pelican_core.cpp#L99)。
13. 関数を抜けるとmodule containerがmoduleを生成逆順に破棄。

12番はtry-catchの**外**にあります。`RuntimeTeardownGuard` は関数冒頭（[`pelican_core.cpp#L46`](../../src/core/userpublic/pelican_core.cpp#L46)）で `RuntimeTeardownMode::terminal_shutdown` として作られ、`FastModuleContainer::beginShutdown()` はguardの外ではなく [`RuntimeTeardownGuard::run()`](../../src/core/appflow/teardown.cpp#L109) の内部で、8段階の解放へ入る直前に呼ばれます（[`teardown.cpp#L114`](../../src/core/appflow/teardown.cpp#L114)）。したがって初期化途中で例外が出ても、同じ「新規module生成を閉じてから順序解放」という経路を通ります。

標準例外も非標準例外もここで捕捉され、ログを出して`false`を返します。したがって、playerの終了コードは `pl.run() ? 0 : 1` です。

## 2.2 `DECLARE_MODULE` / `GET_MODULE` の正体

定義は [`src/core/container.hpp`](../../src/core/container.hpp#L15) です。

```cpp
#define DECLARE_MODULE(name) class name : public ModuleBase<name>
#define GET_MODULE(name) FastModuleContainer::get<name>()
```

### 保存場所

各module型`T`は、CRTP基底 [`ModuleBase<T>::__get()`](../../src/core/container.hpp#L42) が返す関数ローカルstaticな `std::optional<T>` に実体を持ちます。これは「containerインスタンスのメンバ」ではなく、型ごとのプロセス内staticです。

### 遅延生成

[`FastModuleContainer::get<T>()`](../../src/core/container.hpp#L149) はoptionalが空なら`emplace()`し、破棄関数をstaticな`cleaners`へ積みます。従って、**最初に`GET_MODULE(T)`を呼んだ瞬間がTのconstructor実行時点**です。

例として [`Renderer` のconstructor](../../src/core/vkcore/renderer.cpp#L1044) は [`loadRenderGraphVariantsFromConfig()`](../../src/core/vkcore/renderer_config.cpp#L128) を呼ぶだけに見えますが、その内部で次のmoduleが連鎖的に生成されます。ただし現在は、[`Renderer::prepareRuntimeModules()`](../../src/core/vkcore/renderer.hpp#L99) と [`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L244) により「render前に依存を全解決してから凍結する」方式へ変わっています。

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

[`~FastModuleContainer()`](../../src/core/container.hpp#L258) は`cleaners`を後ろからpopします。つまり基本は**初期化の逆順**です。ただしGPU資源の安全な解放には「単に逆順」だけでは足りないため、後述の明示teardownを先に実行します。

### 注意点

- `get<T>()`と`cleaners`更新は [`std::recursive_mutex state_mutex`](../../src/core/container.hpp#L62) で保護されています（各APIがscoped_lockを取る）。それでもmodule生成は基本的にmain threadで済ませてからjobを走らせる前提です。
- [`FastModuleContainer::freezeCreation()`](../../src/core/container.hpp#L199) がLoop開始直前に呼ばれ（[loop.cpp](../../src/core/appflow/loop.cpp#L374)）、以後の新規module生成はエラーになります。[`tryGet<T>()`](../../src/core/container.hpp#L144) は生成せずoptional参照を返します。`graphSnapshot()` がmodule依存グラフを記録します。
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

## 2.4 `Loop::run()` の五経路

中心は [`Loop::run()`](../../src/core/appflow/loop.cpp#L332) です。loop本体へ入る前に、入力のrecord/replay（[`InputSequenceRuntime`](../../src/core/os/inputsequence.hpp#L45)、[loop.cpp](../../src/core/appflow/loop.cpp#L347)）、replay時のfixed_step切替（[同 L353](../../src/core/appflow/loop.cpp#L353)）、camera bakeの開始（[同 L358](../../src/core/appflow/loop.cpp#L358)）、[`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L361) と [`FastModuleContainer::freezeCreation()`](../../src/core/appflow/loop.cpp#L374) が実行されます。

経路は次の五つです。

| 経路 | 条件 | フレーム進行の所有者 |
|---|---|---|
| headless RPC | `headless` かつ `rpc` | RPCクライアントの `step_frame` |
| headless固定フレーム | `headless` | `--frames` またはreplayのフレーム数 |
| windowed + XR | `xr_active` かつXRセッションがrunning | OpenXRの `waitFrame` |
| windowed + RPC | windowed かつ `rpc` | windowのフレーム。RPCはフレーム境界で処理 |
| windowed | 上記以外 | windowのフレーム |

windowed + RPC は他のwindowed経路と排他ではなく、同じwhileループに**追加**される層です。

### windowed

```text
Window::process
→ window eventをInputStateへqueue
→ EngineTime::advance(realtime)
→ VulkanManageCore::setCurrentFrameIndex(frameIndex)
→ update_interactive_state:
     updateFrameState
     → WindowedRpcHost::processFrameBoundary（--rpc 時のみ）
     → F5 game logic reload要求
     → F11 RenderDoc capture arm
→ renderFlatFrameWithOptionalCapture（実体は Renderer::render）
→ FramerateAdjust::wait
```

GLFW callbackは [`Window`](../../src/core/os/window.hpp#L15) の`input_events`へイベントを積み、loopが`drainInputEvents()`して`InputState`へ渡します。ゲームロジックは直接GLFW状態を問い合わせません。windowedではF5キーでgame logic reloadを要求できます（[loop.cpp](../../src/core/appflow/loop.cpp#L460)）。`FramerateAdjust`はwindowedのみです。

フレーム内で状態更新にあたる部分は `update_interactive_state` ラムダ（[loop.cpp](../../src/core/appflow/loop.cpp#L455)）に集約され、windowedとwindowed+XRの両方から呼ばれます。ここが「そのフレームの状態更新が終わった直後」を表す唯一の点であり、windowed RPCのdispatchもF11キャプチャのarmもこの中にあります。

### windowed + RPC（フレーム境界dispatch） ✅実装済み

`--rpc` をwindowedで指定すると、通常のwindowed経路に [`WindowedRpcHost`](../../src/core/communication/rpcserver.hpp#L78) が重なります（[loop.cpp#L430-L442](../../src/core/appflow/loop.cpp#L430)、`#if PELICAN_WITH_RPC`）。

```cpp
windowed_rpc_endpoint = std::make_unique<EngineRpcEndpoint>(std::cin, std::cout);
windowed_rpc_host = std::make_unique<WindowedRpcHost>(
    std::cin, std::cout,
    [&windowed_rpc_endpoint](std::string_view line) {
        return windowed_rpc_endpoint->processLine(line);
    },
    defaultWindowedRpcQueueCapacity);
```

分業がこの経路の要点です。

- 読み取り専用スレッドは**行をqueueへ積むだけ**で、エンジン状態に一切触りません。
- dispatchは [`WindowedRpcHost::processFrameBoundary()`](../../src/core/communication/windowedrpchost.cpp#L119) がエンジンスレッドで行い、呼び出し点は `updateFrameState()` の直後（[loop.cpp](../../src/core/appflow/loop.cpp#L458)）です。
- queue容量は [`defaultWindowedRpcQueueCapacity = 64`](../../src/core/communication/rpcserver.hpp#L99) です。溢れたリクエストには**readerスレッドが即座に**エラーを返します（[`busyResponse()`](../../src/core/communication/windowedrpchost.cpp#L39)）。コードは `-32000`（`applicationError`）、メッセージは `windowed rpc request queue is busy`、`data` に `reason: "busy"` と `queue_capacity` が入ります。
- デストラクタは、`std::istream` に移植可能なキャンセル手段がないため、readerがまだブロック中なら `detach()` します（[windowedrpchost.cpp#L95-L103](../../src/core/communication/windowedrpchost.cpp#L95)）。本番はプロセス寿命の `std::cin` を使う前提です。

> **設計決定:** windowed RPCは「フレームの状態更新が終わった直後に一括処理」であり、任意タイミングの割り込みではありません。エディタからの編集要求がフレームの途中でECSやGPU資源へ触れないため、決定性とteardown順序の契約をそのまま保てます。

### windowed + XRセッション実行中

`launch_config.xr_active` かつXRセッションがrunningの間は、同じwhileループ内で専用経路を通ります（[loop.cpp](../../src/core/appflow/loop.cpp#L475)、`#if PELICAN_WITH_OPENXR`）。

```text
pollEvents → waitFrame → EngineTime::advance
→ VulkanManageCore::setCurrentFrameIndex(frameIndex)
→ beginFrame → locateViews
→ syncActions（action backendとpose samplesをInputStateへ注入）
→ update_interactive_state(xr_frame = true)
→ selectGraphVariant(xr) → XrCompositionTarget.prepareFrame
→ renderLogicalFrame(2 views) → XrMirrorSink.tryPresent → recordMirrorStatistics
```

このフレームで実際に描画するかは、`shouldRender()` だけでは決まりません（[loop.cpp#L489-L491](../../src/core/appflow/loop.cpp#L489)）。

```cpp
const bool xr_frame_renderable =
    begin_result == OpenXr::XrBeginFrameResult::ready &&
    display_timing.shouldRender();
```

つまり `beginFrame()` が `ready` を返し、**かつ** display timingが描画を要求したときだけ `locateViews()` と `renderLogicalFrame()` を実行します。描画しないフレームでも状態更新（`update_interactive_state`）は必ず走るため、ゲーム状態とXRの表示リズムは分離されています。

mirror表示の結果はsessionへ戻され、`xr_session->recordMirrorStatistics(presented, dropped, failures)`（[loop.cpp](../../src/core/appflow/loop.cpp#L522)）で presented / dropped / failures が記録されます。

両eyeのposeはこのフレーム境界で取得したactive cameraへanchorされます（WP131、[`buildRenderViewParameters`](../../src/core/openxr/openxrviewspace.hpp)）。使う投影パラメータは [`Camera::getProjectionSpec()`](../../src/core/appflow/loop.cpp#L499) のznear/zfarです。

### headless固定フレーム

```text
InputState::clear（replay中はスキップ）
→ EngineTime::advance(fixed_step)
→ VulkanManageCore::setCurrentFrameIndex(frameIndex)
→ updateFrameState
→ Renderer::render
→ 必要ならPNG capture
```

`headless_frames == 0`なら無制限です。replay時はフレーム数をreplayから導出します（[loop.cpp](../../src/core/appflow/loop.cpp#L391)）。出力パスに`%d`または`%0Nd`があればフレームごとに保存し、なければ最後のフレームだけ保存します（[`parseRenderOutPattern()`](../../src/core/appflow/loop.cpp#L72)）。

### headless RPC

RPCモードでは通常のfor-loopへ入らず、[`runEngineRpcServer(std::cin, std::cout)`](../../src/core/appflow/loop.cpp#L383) を呼びます。フレーム進行はクライアントの`step_frame`要求が所有します。stdoutはNDJSON protocol専用なので、[`PelicanCore` constructor](../../src/core/userpublic/pelican_core.cpp#L39) がloggerをprotocol対応で初期化します。

### 全経路に共通する `setCurrentFrameIndex()`

`engine_time.advance()` の直後に、どの経路でも `modules.vulkan.setCurrentFrameIndex(engine_time.frameIndex())` を呼びます（headless: [loop.cpp#L402](../../src/core/appflow/loop.cpp#L402)、XR: [#L486](../../src/core/appflow/loop.cpp#L486)、windowed: [#L545](../../src/core/appflow/loop.cpp#L545)）。これが [`VulkanManageCore`](../../src/core/vkcore/core.hpp#L24) 側の `logical_frame` 軸になり、debug-utilsラベルとGPU timingの計測が同じフレーム番号で並びます。

### F11 RenderDocキャプチャ（windowedのflat描画のみ） ✅実装済み

windowedでは、F11でRenderDocのin-applicationキャプチャを1フレーム分だけ要求できます。二段構えです。

1. **arm**: [`requestF11CaptureIfNeeded()`](../../src/core/appflow/loop.cpp#L290) が `UserInput::isKeyPushed(KeyCode::F11) && capture.available()` のときだけ `RenderDocCapture::request(RenderDocCaptureSource::f11, xr_active)` を呼びます。呼び出しは `update_interactive_state` の末尾（[loop.cpp](../../src/core/appflow/loop.cpp#L465)）で、拒否されてもログを出すだけでフレームは継続します。
2. **capture**: 実際のキャプチャは [`renderFlatFrameWithOptionalCapture()`](../../src/core/appflow/loop.cpp#L300) が行います（呼び出しは [#L550](../../src/core/appflow/loop.cpp#L550)）。`state() == armed` のときだけ `captureArmedFrame()` で `StartFrameCapture` / `EndFrameCapture` を明示発行し、それ以外は素の `renderer.render()` です。

> **設計決定:** キャプチャが失敗しても**論理フレームは必ず1回描画されます**。`captureArmedFrame()` へ渡すコールバックが `rendered` フラグを立てるので、`EndFrameCapture` が描画後に失敗しても二重描画にはなりません（[loop.cpp#L322-L324](../../src/core/appflow/loop.cpp#L322)）。デバッグ機能がフレーム進行の正しさを壊さない、という線引きです。

[`RenderDocCapture`](../../src/core/renderdoc/renderdoccapture.hpp#L66) は `Renderer` がVulkan instanceを作る前に [`resolveLoopModules()`](../../src/core/appflow/loop.cpp#L153) で解決されます。RenderDoc自体をロードするのではなく、**既に注入されているAPIを観測するだけ**です。終了時は [`finishLoopResources()`](../../src/core/appflow/loop.cpp#L274) の先頭で `renderdoc_capture.beginShutdown()`（[loop.cpp](../../src/core/appflow/loop.cpp#L279)）を呼びます。XR経路にはキャプチャ点がありません（flat描画のみ対象）。

## 2.5 1フレームの五つの状態フェーズ

順序は [`frame_phase_order`](../../src/core/appflow/framephase.hpp#L17)、実装は [`updateFrameState()`](../../src/core/appflow/framephase.cpp#L119) です。フェーズが始まる前に、フレーム境界の処理が二つ走ります。

```cpp
void updateFrameState() {
    auto modules = resolveFrameStateModules();
    // Reload publication is a frame-boundary operation and happens before any
    // phase can observe game/runtime state.
    if (modules.reload_service != nullptr) modules.reload_service->applyFrame();
    invokeEditorCommitQueueHook();
```

1. [`ReloadService.applyFrame()`](../../src/core/appflow/framephase.cpp#L123) — reloadの公開をフレーム境界へ揃えます。
2. [`invokeEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L124) — 編集トランザクションのコミットキューを流します（後述）。

| 順 | `FramePhase` | 実際の処理 | 保証 |
|---:|---|---|---|
| 1 | `freeze_events` | pending eventを今回配送分へswap | この後emitされたeventは次フレーム |
| 2 | `freeze_input` | `input_sequence.prepareFrame()` → `beginFrame()` → `recordFrame()`（record/replay挿入点） | フレーム中の生入力が不変 |
| 3 | `freeze_actions` | ImGuiへの入力ルーティング → `ui::UiModule.routeFrameInput()`（toolがポインタを消費していない場合）→ Action mapを一回だけ評価 | 全Systemが同じAction値を見る |
| 4 | `deliver_events` | frozen eventをゲームSystemへ登録順にdispatch | event handlerがupdateより先 |
| 5 | `update_game` | 内部ECS → ゲームSystem → animation → sequence → pending scene load | 構造変更の境界を一本化 |

### 編集コミットキューのフック ✅実装済み

`invokeEditorCommitQueueHook()` は、エディタの編集トランザクションをフレーム境界で公開するための唯一の穴です。契約は [`framephase.hpp#L35-L43`](../../src/core/appflow/framephase.hpp#L35) が正で、次の三点です。

- **single-owner**: [`installEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L96) は二重登録を拒否します。
- **位置が固定**: reload公開の後、`freeze_events` の直前。windowed / headless固定フレーム / RPC `step_frame` の全loop面で同じ位置です。
- **未設置ならzero-state no-op**: hookが無い場合、moduleを生成せずmodule graphも変えません。

実装者は現在 [`EditorJournal`](../../src/core/communication/editorjournal.cpp#L2673) の1箇所だけです。

> **設計決定:** 編集の公開点をフレーム境界の1箇所へ寄せることで、「エディタが動いていないビルド／セッションでは編集面が存在しない」状態を保っています。§2.4 のwindowed RPC dispatch（`updateFrameState()` の直後）と合わせて読むと、リクエスト受理→次フレーム冒頭で公開、という往復になります。

`update_game`内部の正確な順序は次です（[framephase.cpp#L155-L173](../../src/core/appflow/framephase.cpp#L155)）。

```text
ECSCore::update()
→ updateRegisteredGameSystems(GameContext&)
→ Vrm::applicationServiceRuntime().ensureStandardPhasesRegistered()
→ Animation::animationServiceRuntime().runAllPhases(frame_index)
→ SeqPlayer::update(current_time)
→ SceneLoader::applyPendingLoad()
```

フェーズ後には `CameraBakeRecorder` が（有効時のみ）フレームを記録します（[framephase.cpp](../../src/core/appflow/framephase.cpp#L180)）。

### `freeze_actions` とImGuiゲートの後始末

`freeze_actions` でImGuiへ入力をルーティングする前に、[`resolveFrameStateModules()`](../../src/core/appflow/framephase.cpp#L51) がゲートの開閉を見ます。ImGuiゲートが閉じたフレーム（XR activationが勝った場合など）では、開始済みのImGuiフレームを [`ImGuiSystem::endFrameIfStarted()`](../../src/core/imgui/imguisystem.hpp#L22) で畳みます（[framephase.cpp#L60-L68](../../src/core/appflow/framephase.cpp#L60)）。

> **設計決定:** ゲートは論理フレーム境界で閉じ得るため、「開始済みのImGuiフレームを、ImGui passを持たないグラフへ持ち越さない」ことを明示的に保証しています。ソース中のコメントがそのまま契約です。

### eventの1フレーム遅延

`GameContext::emit()`はeventを`pending_events`へ積みます（[`emit()`](../../src/core/userpublic/details/event/registerer.hpp#L163)）。次のフレーム冒頭で[`freezePendingEventsForFrame()`](../../src/core/userpublic/details/event/registerer.cpp#L444)が`deliver_now_events`へswapし、配送します（メンバ実装は[同 #L405](../../src/core/userpublic/details/event/registerer.cpp#L405)）。

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

`advance()`後に`current_time += delta_time`、`frame_index++`です。最初の更新フレームはindex 1になります。RPCの`set_time`は時刻だけを直接変更し、deltaを0へ戻します。この不連続は [`timeSetRevision()`](../../src/core/appflow/enginetime.cpp#L57) で観測でき、rendererはこれとcamera不連続をまとめて検知してtemporal historyをリセットします（[renderer.cpp#L1285-L1290](../../src/core/vkcore/renderer.cpp#L1285)）。

## 2.7 ゲームSystemと内部ECS Systemは別の更新列

名前は似ていますが、二種類あります。

| 種類 | 登録 | 呼び出し | 主用途 |
|---|---|---|---|
| 内部ECS System | [`ECSCore::registerSystem`](../../src/core/ecs/core.hpp#L40) | `ECSCore::update()` | Component配列をまとめて処理 |
| ゲームSystem | [`PELICAN_REGISTER_SYSTEM`](../../src/core/userpublic/details/system/registerer.hpp#L173) | `updateRegisteredGameSystems()` | `GameContext`経由のゲームロジック |

内部ECSが先に走ります。そのため、ゲームSystemが同フレーム中に`LocalTransform`を書き換えた結果を内部transform Systemが見るのは原則次フレームです。ただし [`GameObjects::setLocalTransform()`](../../src/core/userpublic/gameobjects.cpp#L55) は公開componentと内部`TransformComponent`を即時同期し、model instanceへも直接TRSを反映するため、公開API経由の通常操作では表示遅延を避けています。

## 2.8 描画フレーム

状態更新後に [`Renderer::render()`](../../src/core/vkcore/renderer.cpp#L1417) が呼ばれます（windowedでは [`renderFlatFrameWithOptionalCapture()`](../../src/core/appflow/loop.cpp#L300) 経由）。WP128以降、`render()`は1-viewのアダプタで、実体は [`Renderer::renderLogicalFrame()`](../../src/core/vkcore/renderer.cpp#L1238) です。

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

## 2.10 編集RPCホストとpreviewの実行面 🚧部分実装

エディタ関連の実行面は、ここまでに出た三つの点で構成されます。RPCのメソッド一覧そのものは[第7章](07_tools_rpc_tests.md)、データ側の受理仕様は[第3章](03_project_and_loading.md)を参照してください。

### RPCディスパッチャは1個

[`EngineRpcEndpoint`](../../src/core/communication/rpcserver.hpp#L60) が「状態を持つエンジンRPCディスパッチャ」を1個所有します。ヘッダのコメントが契約です — headlessは [`run()`](../../src/core/communication/rpcserver.cpp#L1242) がEOFまでブロックし、windowedのホストは [`processLine()`](../../src/core/communication/rpcserver.hpp#L71) を**フレーム境界でだけ**呼びます。`runEngineRpcServer()` はheadless用の薄いラッパです。

### 編集セッションの生成点も1個

編集面の生成は [`makeEditorRuntimeService()`](../../src/core/communication/editorruntimefactory.hpp#L25) だけです。ヘッダのコメントが規範で、要点は次の通りです。

- RPC endpointか、interactiveなImGui runtimeの**どちらか一方**が使う、唯一のproduction composition。
- 決定的ドライバ（headless固定フレームやgolden test）はinteractive runtimeを作らないため、**編集面はそもそも存在しません**。

> **設計決定:** 「編集できるかどうか」をフラグで切り替えるのではなく、生成点を1つに絞ったうえで決定的経路がそこを通らない、という形にしています。決定性を守る側にif文が増えません。

### preview graphは起動時にコンパイルされる第3のグラフ

flat / xr の `RenderingPassId` に対して、preview は**データだけのグラフプログラム**です（[`PreviewGraphProgram`](../../src/core/renderingpass/previewgraph.hpp#L15)）。

- コンパイルは起動時、runtime moduleが凍結される前です。[`loadRenderGraphVariantsFromConfig()`](../../src/core/vkcore/renderer_config.cpp#L128) が [`precompilePreviewGraph()`](../../src/core/renderingpass/previewgraph.hpp#L27) を呼び、結果を `Renderer` の [`preview_graph_program`](../../src/core/vkcore/renderer.hpp#L67) が保持します。
- 共有のrender target / pass登録は**意図的に行いません**。ヘッダのコメント通り、`render_preview` がリクエストローカルな資源に対して実行するため、`Renderer::renderLogicalFrame()` には入りません。
- 実行と隔離キャプチャは [`PreviewExecutor`](../../src/core/vkcore/previewexecutor.hpp#L61) が担当します。

つまりpreviewは「フレームループの外側で、同じ宣言から作った別プログラムを動かす」構造で、通常フレームの決定性やtemporal historyへは触れません。
