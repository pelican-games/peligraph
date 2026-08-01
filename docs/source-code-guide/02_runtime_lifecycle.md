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

入口は [`src/player/main.cpp` の `main()`](../../src/player/main.cpp#L437) です。先に [`parseLaunchConfig()`](../../src/player/main.cpp#L226) が次を決めます。

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

`main()` は [`PelicanCore`](../../src/core/userpublic/pelican_core.hpp#L8) を作った後、次の順でmoduleを設定します。

1. `EngineLaunchConfig`へCLI結果を代入。
2. `PathResolver::setup()`へproject root、絶対パス方針、project JSON、user directoryを渡す。
3. [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L11) でasset store manifestを検査。
4. `ProjectSource`へproject JSONとengine version無視方針を保存。
5. `PelicanCore::run()`を呼ぶ。

この「`run()`前のmodule設定」があるため、`PelicanCore`だけを見てもproject rootの受け渡しは見つかりません。入口とcoreの両方を読む必要があります。

### 段階C: `PelicanCore::run()` でruntimeを組み立てる

[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L46) は短いですが、初期化の正規順序を定義しています。

1. ローカルな `FastModuleContainer` と `RuntimeTeardownGuard` を生成。
2. `StartupMetrics` を開始。
3. **XR activationを決定**: [`resolveXrActivation()`](../../src/core/xractivation.hpp) がheadless/RPC/replay等のforced-offとOpenXR discovery hookから `launch_config.xr_active` を確定。
4. constructorへ渡されたsettings JSONを`ProjectSource`の上書きsourceへ設定し、[`watch::ReloadGate.configureFromLaunch()`](../../src/core/watch/reloadgate.hpp) を呼ぶ。
5. `Persistence`からsettingsを読み、audio設定を適用。
6. [`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L20) で組み込みComponent/Systemを登録。
7. **[`initializeConfiguredGameLogic()`](../../src/core/gamelogic/gamelogicreload.cpp#L463) でgame DLLをロード**（WP110系列）。
8. `ProjectBasicConfig.defaultSceneId()` のsceneを即時ロードし、`ModelAssetContainer` を明示的に先行生成（並列prepareのcommitを起動スレッドで実施）。
9. [`watch::ReloadService.setup()`](../../src/core/watch/reloadservice.hpp#L75) でwatcherのlive inventoryを種付け。
10. `Loop::run()`へ入る。
11. loop終了後、Vulkan deviceをidleまで待つ。
12. [`teardown.run()`](../../src/core/userpublic/pelican_core.cpp#L105) でruntime資源を順序付き解放し、続けて [`shutdownConfiguredGameLogic()`](../../src/core/userpublic/pelican_core.cpp#L106)。
13. 関数を抜けるとmodule containerがmoduleを生成逆順に破棄。

12番はtry-catchの**外**にあります。`RuntimeTeardownGuard` は関数冒頭（[`pelican_core.cpp` 内](../../src/core/userpublic/pelican_core.cpp#L48)）で `RuntimeTeardownMode::terminal_shutdown` として作られ、`FastModuleContainer::beginShutdown()` はguardの外ではなく [`RuntimeTeardownGuard::run()`](../../src/core/appflow/teardown.cpp#L149) の内部で、8段階の解放へ入る直前に呼ばれます（[`teardown.cpp` 内](../../src/core/appflow/teardown.cpp#L155)）。したがって初期化途中で例外が出ても、同じ「新規module生成を閉じてから順序解放」という経路を通ります。

標準例外も非標準例外もここで捕捉され、ログを出して`false`を返します。したがって、playerの終了コードは `pl.run() ? 0 : 1` です。

## 2.2 `DECLARE_MODULE` / `GET_MODULE` の正体

定義は [`src/core/container.hpp`](../../src/core/container.hpp#L15) です。

```cpp
#define DECLARE_MODULE(name) class name : public ModuleBase<name>
#define GET_MODULE(name) FastModuleContainer::get<name>()
```

### 保存場所

各module型`T`は、CRTP基底（CRTP = Curiously Recurring Template Pattern。`class T : public ModuleBase<T>` のように派生クラスが自分自身を基底のテンプレート引数へ渡す書き方で、基底が「誰に継承されたか」を型として知れるため、virtual関数を使わずに型ごとの静的な置き場を持てます） [`ModuleBase<T>::__get()`](../../src/core/container.hpp#L42) が返す関数ローカルstaticな `std::optional<T>` に実体を持ちます。これは「containerインスタンスのメンバ」ではなく、型ごとのプロセス内staticです。

### 遅延生成

[`FastModuleContainer::get<T>()`](../../src/core/container.hpp#L149) はoptionalが空なら`emplace()`し、破棄関数をstaticな`cleaners`へ積みます。従って、**最初に`GET_MODULE(T)`を呼んだ瞬間がTのconstructor実行時点**です。

例として [`Renderer` のconstructor](../../src/core/vkcore/renderer.cpp#L2750) は [`loadRenderGraphVariantsFromConfig()`](../../src/core/vkcore/renderer_config.cpp#L217) を呼ぶだけに見えますが、その内部で次のmoduleが連鎖的に生成されます。ただし現在は、[`Renderer::prepareRuntimeModules()`](../../src/core/vkcore/renderer.hpp#L152) と [`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L250) により「render前に依存を全解決してから、以後の新規module生成を禁止する（module graphを凍結する）」方式へ変わっています。

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

- `get<T>()`と`cleaners`更新は [`std::recursive_mutex state_mutex`](../../src/core/container.hpp#L62) で保護されています。`__ready()`がfalseだった経路はlockを取ってからもう一度`__ready()`を確認するので、同じ型のconstructorが二重に走ることはありません。加えて初回生成は [`requireCreationAllowedLocked()`](../../src/core/container.hpp#L98) がowner thread（最初にmoduleを生成したthread）へ固定するため、別threadからの初回`GET_MODULE`は`logic_error`になります。生成済みmoduleの読み取りだけは別threadからでもlock無しのfast pathで通ります（[`module_container_test.cpp` 内](../../test/module_container_test.cpp#L147)）。
- [`FastModuleContainer::freezeCreation()`](../../src/core/container.hpp#L199) がLoop開始直前に呼ばれ（[loop.cpp](../../src/core/appflow/loop.cpp#L374)）、以後の新規module生成はエラーになります。[`tryGet<T>()`](../../src/core/container.hpp#L144) は生成せずoptional参照を返します。`graphSnapshot()` がmodule依存グラフを記録します。
- constructor内の`GET_MODULE()`が隠れた依存になります。調査時はconstructorと全`GET_MODULE`呼び出しをセットで検索します。
- module実体は型ごとのstaticなので、containerが管理するのは「何個目のcontainerか」ではなく「生存スコープが今1本開いているか」だけです。スコープは重ねられず、生きているcontainerがあるうちに2個目を作るとconstructorが`logic_error`を投げます（[throw 箇所](../../src/core/container.hpp#L124)、[`module lifetime scopes cannot overlap`](../../test/module_container_test.cpp#L214)）。したがって「containerごとにmoduleの寿命が分かれる」ことはありません。テストがcontainerを作るのは、スコープを抜けるときに登録済みmoduleを（どこで生成されたものでも）全て破棄させるためで、destructorが最後にphaseを`booting`へ戻すので同じプロセスで次のcontainerを作り直せます。
- 先に`main()`で作られたmoduleも同じstatic `cleaners`へ載るため、`PelicanCore::run()`内のcontainer破棄時にまとめて片付けられます。

> 🧩 **難所 — 「生成の逆順」が成立する条件**([`FastModuleContainer::get<T>()`](../../src/core/container.hpp#L149) / [`~FastModuleContainer()`](../../src/core/container.hpp#L258))
>
> **何をする所か**: module 実体の遅延生成と、破棄順序の記録です。上の「基本は初期化の逆順」がなぜ正しいのか、そしてどこで破れるのかがここに書かれています。
>
> **素朴に読むと**: 「積んだ順の逆に pop するから安全」で終わりに見えます。しかし成立の根拠はもう一段細かい所にあります — [`cleaners.push_back()`](../../src/core/container.hpp#L181) は `obj_ref.emplace()` の**後**、つまり T の constructor が**終わってから**実行されます。constructor 内の `GET_MODULE(Dep)` は再帰的に先へ進むので、`cleaners` には必ず `Dep` が先、`T` が後で載ります。逆順 pop はしたがって「依存される側より、依存する側を先に壊す」になります。逆に言えば、**constructor の外で初めて `GET_MODULE` した依存は逆順保証の外**です。T の生成後に T のメソッドが初めて `Dep` を掴むと `cleaners` は `[T, Dep]` の順になり、破棄では `Dep` が先に消えて T の destructor が壊れた module を触ります。この穴は §2.4 の二つで塞ぎます — [`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L250) が loop へ入る前に全依存を実体化し、その後で [`freezeCreation()`](../../src/core/container.hpp#L199) が以後の初回生成を禁止します（実体化するのは前者、禁止するのは後者です）。
>
> **骨子**:
> ```text
> get<T>():
>   __ready() が true       -> ロック無しで参照を返す(fast path)
>   構築中スタックに T あり  -> Module construction cycle
>   creation_frozen / shutting_down / 別スレッド -> logic_error
>   obj_ref.emplace()          # ここで走る ctor が Dep を先に cleaners へ積む
>   cleaners.push_back(T)      # ← ctor の「後」。ここが逆順保証の根拠
>   __ready() = true           # ← 最後。ここまで T は公開されない
>
> cleaners: [ Dep, T ]  --pop_back-->  T を破棄 -> Dep を破棄
> ```
>
> **手がかり**: `__ready()` を最後に立てるので、constructor が throw すると `cleaners` にも `__ready()` にも T は現れません(部分構築moduleが公開されない)。`obj_ref.emplace()` が throw した場合 `std::optional` は値を持たないままなので、**T には破棄すべき実体がそもそも存在しません** — `cleaners` に T が載らないことはリークではなく、後始末が不要な状態です。ただし**失敗した constructor が途中まで作った Dep は生き残ります** — 既に `cleaners` に載っているからで、これは意図的です。`cleaners.push_back` 自身の失敗を拾う catch（[巻き戻し](../../src/core/container.hpp#L187)）が `obj_ref.reset()` するのも同じ対称性です。`construction_stack` だけが `thread_local`（[`construction_stack` の宣言](../../src/core/container.hpp#L69)）で `cleaners` / `dependency_edges` は static なので、循環検出と依存辺の記録はスレッドごとです — 実際には [`requireCreationAllowedLocked()`](../../src/core/container.hpp#L98) が生成を owner thread へ固定するため、差が出るのはテストだけです。テストは [`module_container_test.cpp` 内](../../test/module_container_test.cpp#L114)(依存が後に壊れる)と [`module construction failures and cycles never publish partial modules`](../../test/module_container_test.cpp#L132)(失敗と循環で部分公開しない)。
>
> **不変条件**: 依存は constructor で掴む(実行時に初めて掴むと破棄順が逆転する)。`__ready()` は `cleaners` 登録の後にだけ立てる。module 実体は container のメンバではなく型ごとの関数ローカル static なので、container の生存スコープは重ねられない（[constructor の重複検査](../../src/core/container.hpp#L124) が `lifetime_scope_active` で拒否）。

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

この契約は [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp) の「Componentのdeinit/destroyがmodule destructorより先」と、[`lifetime_teardown_test.cpp`](../../test/lifetime_teardown_test.cpp) の全工程fault injection（意図的に各段階を失敗させて、残りの段階が飛ばされないか・順序が崩れないかを確かめるテスト手法）、起動順列、起動途中失敗で検証されています。

> 🧩 **難所 — 一本の順序が二つのモードを兼ねる**([`RuntimeTeardownGuard::run()`](../../src/core/appflow/teardown.cpp#L149) / [`runProductionStep()`](../../src/core/appflow/teardown.cpp#L62))
>
> **何をする所か**: 上の8段階を、terminal shutdown と game-logic reload の `runtime_reset` の**両方**で回します。順序の定義は [`runtime_teardown_order`](../../src/core/appflow/teardown.hpp#L23) の1本だけで、モードごとの別経路はありません。
>
> **素朴に読むと**: 「shutdown と reload は別処理」と読みたくなりますが、差分は実質2箇所です。(1) [`beginShutdown()`](../../src/core/container.hpp#L213) を呼ぶかどうか — `run()` の中で `mode == terminal_shutdown` のときだけ、8段階へ入る**直前**に呼びます(container の destructor ではありません)。(2) 最後の deletion queue の扱い — [deletion queue 段](../../src/core/appflow/teardown.cpp#L76) が `FastModuleContainer::phase()` の値で分岐し、`shutting_down` なら `drainForTeardown()`（空にしたうえで以後の登録を拒否）、そうでなければ `flushAll()`（空にするだけで受付は継続）です。つまり mode フラグを8段階へ引き回すのではなく、入口で phase を動かしておき、後段は**そのときの phase を読んで**振る舞いを決めます。もう一つの要点は `runProductionStep()` が全段階で `tryGet<T>()` しか使わないことです。teardown 中は [`requireCreationAllowedLocked()`](../../src/core/container.hpp#L98) が生成を拒否するので、`GET_MODULE` を1つでも混ぜると「片付けようとして例外」になります。
>
> **骨子**:
> ```text
> RuntimeTeardownGuard::run():
>   completed なら即 return                   # 明示呼びと destructor の二重実行を吸収
>   completed = true
>   mode == terminal_shutdown -> FastModuleContainer::beginShutdown()
>   for step in runtime_teardown_order:       # 8段固定
>       cleanupStep(step) { try { ... } catch { ログのみ } }
>
> deletion_queue: phase()==shutting_down ? drainForTeardown() : flushAll()
>
> phase を動かすのはこの3箇所だけ:
>   booting --freezeCreation() (loop.cpp#L374、loop直前に1回)--> running
>   running --beginShutdown() (teardown.cpp#L115、terminal_shutdown のみ)--> shutting_down
>   ~FastModuleContainer(): 入口で shutting_down にして全破棄、最後に booting へ戻す
>   runtime_reset は phase に触れないので running のまま(戻す処理も要らない)
>   ※ enterRunningPhase() も phase を動かせるが production では未使用(test が loop.cpp に無いことを検査)
> ```
>
> **手がかり**: [`cleanupStep()`](../../src/core/appflow/teardown.cpp#L21) が段階ごとに例外を飲むので、**途中の失敗が後続段階を飛ばしません** — 「wait_idle が落ちたので event が残った」という連鎖を作らないための構造です。`RuntimeTeardownActions` を取る overload は各段を `std::function` で差し替えるテスト用の面で、空の `std::function` は「起動が失敗してその module がまだ無い」を表します(ヘッダのコメント [該当箇所](../../src/core/appflow/teardown.hpp#L49) が規範)。`completed` フラグがあるため、§2.1 の12番(明示 `teardown.run()`)と destructor 経由が重なっても8段階は1回しか走りません。テストは [`lifetime_teardown_test.cpp` 内](../../test/lifetime_teardown_test.cpp#L196)(規範順序)/ [同ファイルの reset 側](../../test/lifetime_teardown_test.cpp#L235)(runtime reset が terminal shutdown へ入らない)/ [同ファイルの起動失敗側](../../test/lifetime_teardown_test.cpp#L312)(起動途中失敗)。
>
> **不変条件**: teardown 経路では module を新規生成しない(`tryGet` のみ)。段階の順序を変えない。`run()` は何度呼んでも副作用が1回。`runtime_reset` は module phase を `shutting_down` にしない(reload 後もフレームを回し続けるため。`shutting_down` にすると deletion queue が `drainForTeardown()` で閉じ、以後のGPU資源登録が拒否されます)。

## 2.4 `Loop::run()` の五経路

中心は [`Loop::run()`](../../src/core/appflow/loop.cpp#L338) です。loop本体へ入る前に、入力のrecord/replay（[`InputSequenceRuntime`](../../src/core/os/inputsequence.hpp#L45)、[loop.cpp](../../src/core/appflow/loop.cpp#L347)）、replay時のfixed_step切替（[その分岐](../../src/core/appflow/loop.cpp#L353)）、camera bakeの開始（[その呼び出し](../../src/core/appflow/loop.cpp#L365)）、[`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L250) と [`FastModuleContainer::freezeCreation()`](../../src/core/appflow/loop.cpp#L380) が実行されます。

経路は次の五つです。

| 経路 | 条件 | フレーム進行の所有者 |
|---|---|---|
| headless RPC | `headless` かつ `rpc` | RPCクライアントの `step_frame` |
| headless固定フレーム | `headless` | `--frames` またはreplayのフレーム数 |
| windowed + XR | `xr_active` かつXRセッションがrunning | OpenXRの `waitFrame` |
| windowed + RPC | windowed かつ `rpc` | windowのフレーム。RPCはフレーム境界で処理 |
| windowed | 上記以外 | windowのフレーム |

windowed + RPC は他のwindowed経路と排他ではなく、同じwhileループへ**追加**される層です。ホスト自体はwhileループへ入る前に1個だけ作られ（[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L441)）、ループ側に増えるのは `update_interactive_state` の中のdispatch1行だけです（[その1行](../../src/core/appflow/loop.cpp#L458)）。

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

GLFW callbackは [`Window`](../../src/core/os/window.hpp#L17) の`input_events`へイベントを積み、loopが`drainInputEvents()`して`InputState`へ渡します。ゲームロジックは直接GLFW状態を問い合わせません。windowedではF5キーでgame logic reloadを要求できます（[loop.cpp](../../src/core/appflow/loop.cpp#L466)）。`FramerateAdjust`はwindowedのみです。

フレーム内で状態更新にあたる部分は `update_interactive_state` ラムダ（[loop.cpp](../../src/core/appflow/loop.cpp#L455)）に集約され、windowedとwindowed+XRの両方から呼ばれます。ここが「そのフレームの状態更新が終わった直後」を表す唯一の点であり、windowed RPCのdispatchもF11キャプチャのarmもこの中にあります。

### windowed + RPC（フレーム境界dispatch） ✅実装済み

`--rpc` をwindowedで指定すると、通常のwindowed経路に [`WindowedRpcHost`](../../src/core/communication/rpcserver.hpp#L78) が重なります（[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L440)、`#if PELICAN_WITH_RPC`）。

```cpp
windowed_rpc_endpoint = std::make_unique<EngineRpcEndpoint>(std::cin, std::cout);
windowed_rpc_host = std::make_unique<WindowedRpcHost>(
    std::cin, std::cout,
    [&windowed_rpc_endpoint](std::string_view line) {
        return windowed_rpc_endpoint->processLine(line);
    },
    defaultWindowedRpcQueueCapacity);
```

この経路の要点は、readerスレッドとエンジンスレッドの分業です。

- 読み取り専用スレッドは**行をqueueへ積むだけ**で、エンジン状態に一切触りません。
- dispatchは [`WindowedRpcHost::processFrameBoundary()`](../../src/core/communication/windowedrpchost.cpp#L119) がエンジンスレッドで行い、呼び出し点は `updateFrameState()` の直後（[loop.cpp](../../src/core/appflow/loop.cpp#L458)）です。
- queue容量は [`defaultWindowedRpcQueueCapacity = 64`](../../src/core/communication/rpcserver.hpp#L99) です。溢れたリクエストには**readerスレッドが即座に**エラーを返します（[`busyResponse()`](../../src/core/communication/windowedrpchost.cpp#L39)）。コードは `-32000`（`applicationError`）、メッセージは `windowed rpc request queue is busy`、`data` に `reason: "busy"` と `queue_capacity` が入ります。
- デストラクタは、`std::istream` に移植可能なキャンセル手段がないため、readerがまだブロック中なら `detach()` します（[`windowedrpchost.cpp` 内](../../src/core/communication/windowedrpchost.cpp#L95)）。本番はプロセス寿命の `std::cin` を使う前提です。

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

このフレームで実際に描画するかは、`shouldRender()` だけでは決まりません（[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L489)）。

```cpp
const bool xr_frame_renderable =
    begin_result == OpenXr::XrBeginFrameResult::ready &&
    display_timing.shouldRender();
```

つまり `beginFrame()` が `ready` を返し、**かつ** display timingが描画を要求したときだけ `locateViews()` と `renderLogicalFrame()` を実行します。描画しないフレームでも状態更新（`update_interactive_state`）は必ず走るため、ゲーム状態とXRの表示リズムは分離されています。

mirror表示の結果はsessionへ戻され、`xr_session->recordMirrorStatistics(presented, dropped, failures)`（[loop.cpp](../../src/core/appflow/loop.cpp#L522)）で presented / dropped / failures が記録されます。

両eyeのposeはこのフレーム境界で取得したactive cameraへanchorされ、stable eye ID付き
`$main` familyになります（WP131/WP223、
[`buildMainRenderViewFamily`](../../src/core/openxr/openxrviewspace.hpp)）。使う投影パラメータは
[`Camera::getProjectionSpec()`](../../src/core/appflow/loop.cpp)のznear/zfarです。

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

RPCモードでは通常のfor-loopへ入らず、[`runEngineRpcServer(std::cin, std::cout)`](../../src/core/appflow/loop.cpp#L389) を呼びます。フレーム進行はクライアントの`step_frame`要求が所有します。stdoutはNDJSON protocol（newline-delimited JSON — 1行に1個のJSON値を置く形式。ここでは1行が1リクエストまたは1レスポンスにあたるため、ログを1行でも混ぜると相手のparseが壊れます）専用なので、[`PelicanCore` constructor](../../src/core/userpublic/pelican_core.cpp#L41) がloggerをprotocol対応で初期化します。

### 全経路に共通する `setCurrentFrameIndex()`

`engine_time.advance()` の直後に、どの経路でも `modules.vulkan.setCurrentFrameIndex(engine_time.frameIndex())` を呼びます（headless: [`loop.cpp` 内](../../src/core/appflow/loop.cpp#L402)、XR: [XR セッション側](../../src/core/appflow/loop.cpp#L486)、windowed: [windowed ループ側](../../src/core/appflow/loop.cpp#L545)）。これが [`VulkanManageCore`](../../src/core/vkcore/core.hpp#L45) 側の `logical_frame` 軸になり、debug-utilsラベルとGPU timingの計測が同じフレーム番号で並びます。

### F11 RenderDocキャプチャ（windowedのflat描画のみ） ✅実装済み

windowedでは、F11でRenderDocのin-applicationキャプチャを1フレーム分だけ要求できます。二段構えです。

1. **arm**（アーム — 「次に描く1フレームを撮る」という予約だけを立てて、実際の発行は後段へ任せる状態にすること）: [`requestF11CaptureIfNeeded()`](../../src/core/appflow/loop.cpp#L296) が `UserInput::isKeyPushed(KeyCode::F11) && capture.available()` のときだけ `RenderDocCapture::request(RenderDocCaptureSource::f11, xr_active)` を呼びます。呼び出しは `update_interactive_state` の末尾（[loop.cpp](../../src/core/appflow/loop.cpp#L465)）で、拒否されてもログを出すだけでフレームは継続します。
2. **capture**: 実際のキャプチャは [`renderFlatFrameWithOptionalCapture()`](../../src/core/appflow/loop.cpp#L306) が行います（呼び出しは [windowed 経路](../../src/core/appflow/loop.cpp#L550)）。`state() == armed` のときだけ `captureArmedFrame()` で `StartFrameCapture` / `EndFrameCapture` を明示発行し、それ以外は素の `renderer.render()` です。

> **設計決定:** キャプチャが失敗しても**論理フレームは必ず1回描画されます**。`captureArmedFrame()` へ渡すコールバックが `rendered` フラグを立てるので、`EndFrameCapture` が描画後に失敗しても二重描画にはなりません（[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L322)）。デバッグ機能がフレーム進行の正しさを壊さない、という線引きです。

[`RenderDocCapture`](../../src/core/renderdoc/renderdoccapture.hpp#L66) は `Renderer` がVulkan instanceを作る前に [`resolveLoopModules()`](../../src/core/appflow/loop.cpp#L153) で解決されます。RenderDoc自体をロードするのではなく、**既に注入されているAPIを観測するだけ**です。終了時は [`finishLoopResources()`](../../src/core/appflow/loop.cpp#L280) の先頭で `renderdoc_capture.beginShutdown()`（[loop.cpp](../../src/core/appflow/loop.cpp#L285)）を呼びます。XR経路にはキャプチャ点がありません（flat描画のみ対象）。

## 2.5 1フレームの五つの状態フェーズ

順序は [`frame_phase_order`](../../src/core/appflow/framephase.hpp#L17)、実装は [`updateFrameState()`](../../src/core/appflow/framephase.cpp#L128) です。フェーズが始まる前に、フレーム境界の処理が二つ走ります。

```cpp
void updateFrameState() {
    auto modules = resolveFrameStateModules();
    // Reload publication is a frame-boundary operation and happens before any
    // phase can observe game/runtime state.
    if (modules.reload_service != nullptr) modules.reload_service->applyFrame();
    invokeEditorCommitQueueHook();
```

1. [`ReloadService.applyFrame()`](../../src/core/appflow/framephase.cpp#L123) — reloadの公開をフレーム境界へ揃えます。
2. [`invokeEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L122) — 編集トランザクションのコミットキューを流します（後述）。

| 順 | `FramePhase` | 実際の処理 | 保証 |
|---:|---|---|---|
| 1 | `freeze_events` | pending eventを今回配送分へswap | この後emitされたeventは次フレーム |
| 2 | `freeze_input` | `input_sequence.prepareFrame()` → `beginFrame()` → `recordFrame()`（record/replay挿入点） | フレーム中の生入力が不変 |
| 3 | `freeze_actions` | ImGuiへの入力ルーティング → `ui::UiModule.routeFrameInput()`（toolがポインタを消費していない場合）→ Action mapを一回だけ評価 | 全Systemが同じAction値を見る |
| 4 | `deliver_events` | frozen eventをゲームSystemへ登録順にdispatch | event handlerがupdateより先 |
| 5 | `update_game` | 内部ECS → ゲームSystem → animation → sequence → pending scene load | 構造変更の境界を一本化 |

### 編集コミットキューのフック ✅実装済み

`invokeEditorCommitQueueHook()` は、エディタの編集トランザクションをフレーム境界で公開するための唯一の穴です。契約は [`framephase.hpp` 内](../../src/core/appflow/framephase.hpp#L35) が正で、次の三点です。

- **single-owner**: [`installEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L105) は二重登録を拒否します。
- **位置が固定**: reload公開の後、`freeze_events` の直前。windowed / headless固定フレーム / RPC `step_frame` の全loop面で同じ位置です。
- **未設置ならzero-state no-op**: hookが無い場合、moduleを生成せずmodule graphも変えません。

実装者は現在 [`EditorJournal` の登録箇所](../../src/core/communication/editorjournal.cpp#L2711) の1箇所だけです。

> **設計決定:** 編集の公開点をフレーム境界の1箇所へ寄せることで、「エディタが動いていないビルド／セッションでは編集面が存在しない」状態を保っています。§2.4 のwindowed RPC dispatch（`updateFrameState()` の直後）と合わせて読むと、リクエスト受理→次フレーム冒頭で公開、という往復になります。

`update_game`内部の正確な順序は次です（[`framephase.cpp` 内](../../src/core/appflow/framephase.cpp#L155)）。

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

`freeze_actions` でImGuiへ入力をルーティングする前に、[`resolveFrameStateModules()`](../../src/core/appflow/framephase.cpp#L57) がゲートの開閉を判定します。ゲートの実体は [`isImGuiRuntimeEnabled(config)`](../../src/core/imgui/imguiruntime.cpp#L9) で、`EngineLaunchConfig` の `headless` / `rpc` / `input_replay` / `golden_mode` / `xr_active` を読むだけの述語です（この関数自身は何も書き換えません）。開閉が変わるのは、読んでいる側のlaunch configが変わったときです（例: XRのVulkan bootstrapが失敗してflatへ落ちる [`core.cpp` 内](../../src/core/vkcore/core.cpp#L451)）。ImGuiゲートが閉じたフレームでは、開始済みのImGuiフレームを [`ImGuiSystem::endFrameIfStarted()`](../../src/core/imgui/imguisystem.hpp#L31) で畳みます（[`framephase.cpp` 内](../../src/core/appflow/framephase.cpp#L60)）。守っている対応関係は「`freeze_actions` の `routeInputAndBeginFrame()` が立てた `frame_started` は、そのフレームの `ImGuiSystem::render()` が必ず倒す」で、ImGuiパスを持たないグラフへ持ち越すと `render()` が呼ばれず、立ったままのフレームへ次の `NewFrame()` が重なります。

> **設計決定:** ゲートは論理フレーム境界で閉じ得るため、「開始済みのImGuiフレームを、ImGui passを持たないグラフへ持ち越さない」ことを明示的に保証しています。ソース中のコメントがそのまま契約です。

> 🧩 **難所 — 名前は「解決」、中身は副作用**([`resolveFrameStateModules()`](../../src/core/appflow/framephase.cpp#L57) / [`prepareFrameStateModules()`](../../src/core/appflow/framephase.cpp#L101))
>
> **何をする所か**: `updateFrameState()` の冒頭で、そのフレームが触る module 参照を1つの構造体へ集めます。名前どおりの解決に加えて、直前の節で見たImGuiゲートの後始末という**状態変更**もここで起きます。
>
> **素朴に読むと**: 「参照を集めるだけの純粋な関数」に見えるので、毎フレーム呼ぶのが無駄に見えます。実際には毎フレーム通ることに意味があります。(1) ImGuiゲートが閉じたフレームでは [`endFrameIfStarted()`](../../src/core/imgui/imguisystem.hpp#L31) をここで呼びます — ゲートは論理フレーム境界で閉じ得るので、判定と後始末を同じ場所へ置かないと開始済みImGuiフレームが持ち越されます。(2) `ui_module` と `render_target` は**どちらか一方が無ければ両方 nullptr にします**([その判定](../../src/core/appflow/framephase.cpp#L54))。`freeze_actions` の [`routeFrameInput(input_state, render_target->getExtent())`](../../src/core/appflow/framephase.cpp#L148) が `ui_module != nullptr` の分岐の中で `render_target` を**無条件に**参照するからで、片方だけの nullptr を許すとここで落ちます。(3) `tryGet` と `GET_MODULE` の使い分けも意図的です。`GET_MODULE` 側は「必ず存在する前提」で、凍結後に未生成のmoduleを `GET_MODULE` すると例外になります。だから `prepareFrameStateModules()` が凍結前に同じ解決を一度だけ走らせて実体化しておきます([`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L250) から呼ばれる)。
>
> **骨子**:
> ```text
> updateFrameState():
>   resolveFrameStateModules()        # ImGui ゲート後始末 + module 参照の確定
>   reload_service->applyFrame()      # reload 公開(フレーム境界)
>   invokeEditorCommitQueueHook()     # 編集公開(フレーム境界)
>   forEachFramePhase(...)            # 5フェーズ
>   camera_bake->recordFrame()        # tryGet。無効なら何もしない
>
> ui_module==null または render_target==null -> 両方 null   # 対で扱う
> ```
>
> **手がかり**: [`prepareFrameStateModules()`](../../src/core/appflow/framephase.cpp#L101) の中身は `(void)resolveFrameStateModules();` の一行で、**戻り値ではなく副作用が目的**であることがそのまま現れています。hookの位置(reload公開の後、`freeze_events` の直前)を規範として書いているのは [`framephase.hpp` 内](../../src/core/appflow/framephase.hpp#L35) のコメントです。[`installEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L105) は null と二重登録を弾き、[`removeEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L116) は**登録時と同じ context** でなければ何もしません(他人のhookを外せない)。テストは [`Windowed and RPC frames use the same five-phase executor`](../../test/framephase_test.cpp#L9)(windowedとRPCが同じ5フェーズ)と [`Editor commit queue hook is optional and has one explicit boundary`](../../test/framephase_test.cpp#L36)(hookは任意で境界は1つ)。
>
> **不変条件**: `updateFrameState()` は毎フレーム `resolveFrameStateModules()` を通る(ゲート後始末を飛ばさない)。`ui_module` / `render_target` は対で有効・対で無効。フレーム実行中にmoduleを新規生成しない。hookはsingle-ownerで、外せるのは登録した本人だけ。

> 🧩 **難所 — capture は黙って消える**([`InputRouter::route()`](../../src/core/ui/inputrouter.cpp#L45))
>
> **何をする所か**: `freeze_actions` が呼ぶ [`UiModule::routeFrameInput()`](../../src/core/ui/module.cpp#L266) の中身([`framephase.cpp` 内](../../src/core/appflow/framephase.cpp#L141))で、そのフレームの生ポインタ列を widget へ割り当て、`Capture` / `Click` / `DragStart` などのeffect列へ変換します。
>
> **素朴に読むと**: Down/Move/Up の switch だけ見ると「Move は今カーソルの下にある widget へ配られる」と読めます。実際は逆で、**Down で捕まえた widget が Up まで全ての Move を受け取ります** — Move の `routed.target` は hit 判定の結果ではなく `state.capture->owner` です([Move 側の代入](../../src/core/ui/inputrouter.cpp#L67))。物理判定は `hover_target` 側だけを更新するので、**hover と target がずれているのが正常状態**です。この前提が崩れる唯一の経路が switch より**前**の2行([消えた capture の掃除](../../src/core/ui/inputrouter.cpp#L58))で、capture 先の widget が arena から消えていると `state.capture` を無言で `reset()` します。ここだけ `ReleaseCapture` も `Cancel` も出ません。明示通知する [`cancelCapture()`](../../src/core/ui/inputrouter.cpp#L136) は `{Cancel, ReleaseCapture}` を返しますが、現在エンジン内に呼び出し元がありません(定義だけがある状態)。したがって **widget を消す側のコードが「ドラッグ中に消すと DragEnd 相当の通知が来ない」ことを知っている必要があります**。消えたと判定されるのは [`WidgetArena::erase()`](../../src/core/ui/widgetarena.cpp#L20) / `clear()` が slot の generation(世代番号 — 同じ index を再利用しても古い `WidgetId` を別物と判別するための連番)を進め、[`resolve()`](../../src/core/ui/widgetarena.cpp#L33) が `nullptr` を返すようになるからです。
>
> **骨子**:
> ```text
> route(events):                      # そのフレームの列を先頭から順に
>   event_seq が狭義単調増加でなければ throw     # 状態機械がフレーム内順序に依存する
>   physical = (Cancel なら null) else hitTest(px)
>   capture の owner が arena に無い -> capture.reset()   # ← effect を出さない唯一の出口
>   Move: hover != physical -> HoverExit / HoverEnter     # hover だけは物理判定
>         capture あり -> target = capture.owner; |dx|+|dy| >= 4 で DragStart(ラッチ) -> 以後 Drag
>         capture なし -> target = physical
>   Down: capture 済みなら throw / physical があれば capture を作り Capture
>   Up  : capture あり -> ReleaseCapture; !drag_started かつ physical == owner なら Click
> ```
>
> **手がかり**: `drag_started` はラッチ(一度立つと Up まで倒れないフラグ)なので、閾値を越えた後に押した位置へ指を戻しても Click にはなりません — Up 側の `!state.capture->drag_started && physical_target == state.capture->owner`([Click 発行の条件](../../src/core/ui/inputrouter.cpp#L94))がその裏返しです。閾値 `dx + dy >= 4`([DragStart の閾値](../../src/core/ui/inputrouter.cpp#L74))は**UI単位**のマンハッタン距離(各軸の差の絶対値の和。斜め移動は直線距離より早く閾値へ届きます)で、[`windowToUi()`](../../src/core/ui/types.cpp#L96) が framebuffer 座標を `ui_scale` で割った後の値です — 物理ピクセルではないので、`ui_scale` が大きいほど実際の移動量は長く必要になります。capture が黙って消えた後の Up は `else routed.target = physical_target`([capture 無しの Up](../../src/core/ui/inputrouter.cpp#L97))へ落ちるため、ボタン不一致の検査([その throw](../../src/core/ui/inputrouter.cpp#L91))も素通りします。`routed.target` か capture があれば `consumed_pointer` が立ち、`UiModule` が `consumePointerForActions()` を呼ぶ([`module.cpp` 内](../../src/core/ui/module.cpp#L273))ので、capture 中はAction層からポインタが隠れます。テストは [`Ordered router keeps per-pointer captures independent`](../../test/ui_foundation_test.cpp#L246)(pointer_idごとにcaptureが独立)と [同ファイルの U2 ボタン検証](../../test/ui_foundation_test.cpp#L305)(Upが `ReleaseCapture` → `Click` の順)。
>
> **不変条件**: Down から Up までの Move は capture owner へ届く(hover は別軸で更新される)。`event_seq` はフレーム内で狭義単調増加。`drag_started` は Up まで倒れず、Click と排他。capture 中の widget 削除だけは effect を出さないので、終了処理は削除する側が持つ。

### eventの1フレーム遅延

`GameContext::emit()`はeventを`pending_events`へ積みます（[`emit()`](../../src/core/userpublic/details/event/registerer.hpp#L163)）。次のフレーム冒頭で[`freezePendingEventsForFrame()`](../../src/core/userpublic/details/event/registerer.cpp#L444)が`deliver_now_events`へswapし、配送します（メンバ実装は[`UserEventRegistererTemplatePublic::freezePendingEventsForFrame()`](../../src/core/userpublic/details/event/registerer.cpp#L405)）。

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

`advance()`後に`current_time += delta_time`、`frame_index++`です。最初の更新フレームはindex 1になります。RPCの`set_time`は時刻だけを直接変更し、deltaを0へ戻します。この不連続は [`timeSetRevision()`](../../src/core/appflow/enginetime.cpp#L57) で観測でき、rendererはこれとcamera不連続をまとめて検知してtemporal history（前フレームのview/projection行列やcamera位置をview単位で覚えておく履歴。前フレームの描画結果を今フレームへ再投影して混ぜる処理が使うので、時刻やcameraが飛ぶと対応関係が崩れて捨てる必要があります）をリセットします（[`renderer.cpp` 内](../../src/core/vkcore/renderer.cpp#L1319)）。

## 2.7 ゲームSystemと内部ECS Systemは別の更新列

名前は似ていますが、二種類あります。

| 種類 | 登録 | 呼び出し | 主用途 |
|---|---|---|---|
| 内部ECS System | [`ECSCore::registerSystem`](../../src/core/ecs/core.hpp#L40) | `ECSCore::update()` | Component配列をまとめて処理 |
| ゲームSystem | [`PELICAN_REGISTER_SYSTEM`](../../src/core/userpublic/details/system/registerer.hpp#L173) | `updateRegisteredGameSystems()` | `GameContext`経由のゲームロジック |

内部ECSが先に走ります。そのため、ゲームSystemが同フレーム中に`LocalTransform`を書き換えた結果を内部transform Systemが見るのは原則次フレームです。ただし [`GameObjects::setLocalTransform()`](../../src/core/userpublic/gameobjects.cpp#L55) は公開componentと内部`TransformComponent`を即時同期し、model instanceへも直接TRSを反映するため、公開API経由の通常操作では表示遅延を避けています。

## 2.8 描画フレーム

状態更新後に [`Renderer::render()`](../../src/core/vkcore/renderer.cpp#L4522) が呼ばれます（windowedでは [`renderFlatFrameWithOptionalCapture()`](../../src/core/appflow/loop.cpp#L306) 経由）。WP128以降、`render()`は1-viewのアダプタで、実体は [`Renderer::renderLogicalFrame()`](../../src/core/vkcore/renderer.cpp#L3710) です。

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
- job workerの例外は [`JobSystem` が例外を退避する箇所](../../src/core/job_system.cpp#L51) で`exception_ptr`に保持し、main threadの`wait()`で再throwします。
- shader hot reload失敗だけは旧shader/pipelineを維持してwarningにします（[`ShaderLibrary::prepareReload()`](../../src/core/shader/shaderlibrary.cpp#L899)、[`PipelineFactory::rebuildPrepared()`](../../src/core/shader/pipelinefactory.cpp#L671)）。
- teardownは例外を外へ出しません。

この違いは「初回構築に失敗した不完全なruntimeは続けないが、稼働中の編集失敗では最後の正常版を守る」という方針です。

## 2.10 編集RPCホストとpreviewの実行面 🚧部分実装

エディタ関連の実行面は、ここまでに出た三つの点で構成されます。RPCのメソッド一覧そのものは[第7章](07_tools_rpc_tests.md)、データ側の受理仕様は[第3章](03_project_and_loading.md)を参照してください。

### RPCディスパッチャは1個

[`EngineRpcEndpoint`](../../src/core/communication/rpcserver.hpp#L60) が「状態を持つエンジンRPCディスパッチャ」を1個所有します。ヘッダのコメントが契約です — headlessは [`run()`](../../src/core/communication/rpcserver.cpp#L1199) がEOFまでブロックし、windowedのホストは [`processLine()`](../../src/core/communication/rpcserver.hpp#L71) を**フレーム境界でだけ**呼びます。`runEngineRpcServer()` はheadless用の薄いラッパです。

### 編集セッションの生成点も1個

編集面の生成は [`makeEditorRuntimeService()`](../../src/core/communication/editorruntimefactory.hpp#L25) だけです。ヘッダのコメントが規範で、要点は次の通りです。

- RPC endpointか、interactiveなImGui runtimeの**どちらか一方**が使う、唯一のproduction composition。
- 決定的ドライバ（headless固定フレームやgolden test）はinteractive runtimeを作らないため、**編集面はそもそも存在しません**。

> **設計決定:** 「編集できるかどうか」をフラグで切り替えるのではなく、生成点を1つに絞ったうえで決定的経路がそこを通らない、という形にしています。決定性を守る側にif文が増えません。

### preview graphは起動時にコンパイルされる第3のグラフ

flat / xr の `RenderingPassId` に対して、preview は**データだけのグラフプログラム**です（[`PreviewGraphProgram`](../../src/core/renderingpass/previewgraph.hpp#L20)）。

- コンパイルは起動時、runtime moduleが凍結される前です。[`loadRenderGraphVariantsFromConfig()`](../../src/core/vkcore/renderer_config.cpp#L217) が [`precompilePreviewGraph()`](../../src/core/renderingpass/previewgraph.hpp#L42) を呼び、結果を `Renderer` の [`preview_graph_program`](../../src/core/vkcore/renderer.hpp#L104) が保持します。
- 共有のrender target / pass登録は**意図的に行いません**。ヘッダのコメント通り、`render_preview` がリクエストローカルな資源に対して実行するため、`Renderer::renderLogicalFrame()` には入りません。
- 実行と隔離キャプチャは [`PreviewExecutor`](../../src/core/vkcore/previewexecutor.hpp#L61) が担当します。

つまりpreviewは「フレームループの外側で、同じ宣言から作った別プログラムを動かす」構造で、通常フレームの決定性やtemporal historyへは触れません。
