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

入口は [`src/player/main.cpp` の `main()`](../../src/player/main.cpp#L714) です。先に [`parseLaunchConfig()`](../../src/player/main.cpp#L345) が次を決めます。

- windowedかheadlessか
- RPCを使うか（`--rpc`）
- headlessの解像度・フレーム数・固定FPS
- headlessの開始時刻（`--start-time`、[引数宣言](../../src/player/main.cpp#L378)。後述）
- project rootと`project.json`
- asset検証をstrictにするか
- render出力、frame plan dump（`--dump-frame-plan`）
- Vulkanのdebugオブジェクト名とコマンドラベル（`--gpu-labels` → [`EngineLaunchConfig::gpu_labels`](../../src/core/launchconfig.hpp#L100)）
- Vulkan validation + synchronization validation（`--vulkan-validation=off|on`。未指定は `_DEBUG` だけ ON）
- sequence/VAT再生とcamera override
- XRモード（`--xr off|auto|on`）
- game logic DLL（`--game-logic`）
- 入力の記録/再生（`--record-input` / `--replay` / `--input-profile`）
- camera bake（`--bake-camera-output`、headless + replay 必須）

> **設計決定:** `--rpc` は `--headless` を要求しません（WP156。以後この文書に出る `WP<番号>` は work package = 台帳 [`docs/implementation_plan.md`](../implementation_plan.md) が「独立に依頼できる単位」として採番した作業の番号で、`-fix` / `a` / `b` は同じ作業の分割・追補です。[用語集](../manual/14_glossary.md)）。ヘルプ文言そのものが現在の契約です — `enable stdio JSON-RPC (blocking in headless, frame-boundary in windowed mode)`（[`main.cpp`](../../src/player/main.cpp#L352)）。headlessではRPCがフレーム進行を所有し、windowedではフレーム境界でだけdispatchされます。両者の違いは §2.4 で分解します。

`--project` がなければ、実行ファイルの祖先から `projects/example/project.json` を探索します（[`configureImplicitProject()`](../../src/player/main.cpp#L271)）。明示projectなら、directoryまたは`project.json`そのものを受け付けます（[`configureExplicitProject()`](../../src/player/main.cpp#L286)）。

#### `--start-time`: 秒はここでフレーム番号になる ✅実装済み（O-1）

`--start-time <秒>` は「その時刻から `--frames` 枚を出す」オフライン出力用の引数です。押さえる点は3つあります。

**engineは frame 番号しか受け取りません。** 変換は [`parseStartFrame()`](../../src/player/main.cpp#L326) 1箇所だけで、`llround(seconds * fps)` によって固定ステップのタイムライン上のフレーム番号に落ちます（フレームとフレームの間の時刻は近い方のフレームへ丸められ、engineへ渡ることはありません）。結果は [`EngineLaunchConfig::start_frames`](../../src/core/launchconfig.hpp#L94) に入ります。非常に大きな秒数では `llround` が溢れますが、これは台帳 O-1 が既知の穴として記録している範囲で、実用的な入力では起きません。

**失敗は全部ひとつの名前になります。** argparse に `double` としてscanさせず、テキストのまま `std::stod` で読んで「文字を全部消費したか・有限か・非負か」を自分で確かめます（[検査](../../src/player/main.cpp#L334)）。綴り間違いも負の時刻も同じ `offline.time_invalid@1` で、理由は「そんなフレームは存在しない」の一言に畳めるからです。

**組み合わせの衝突は黙って無視しません。** 固定ステップのタイムラインが存在するのは headless だけなので、`--headless` が無ければ `offline.headless_required@1`。そして「このrunのフレームがどこから来るか」を決める引数なので、同じことを決める `--rpc` / `--replay` / `--record-input` / `--bake-camera-output` との併用は [`offline.mode_conflict@1`](../../src/player/main.cpp#L657) で、どのフラグと衝突したかを名前で返します。

#### `--start-state`: edit で起こす ✅実装済み（PM-1b）

`--start-state edit|playing`（既定 `playing`）は、engine が最初にどの再生の状態で動くかを決めます。値は [`parseStartState()`](../../src/player/main.cpp#L155) が読み、それ以外の値は `--start-state must be one of: edit, playing` です。結果は [`EngineLaunchConfig::start_state`](../../src/core/launchconfig.hpp#L54) に入ります。`edit` の engine はクライアントが `set_play_state` で game を始めるのを待つので、run の最初のフレームから進み方を決める引数（`--bake-camera-output` / `--replay` / `--record-input` / `--start-time` / `--rpc` の無い `--headless`）は、`--start-time` と同じ形の [`offline.mode_conflict@1: --start-state edit cannot be combined with <引数>`](../../src/player/main.cpp#L632) で断ります（挙げた順に調べ、最初に当たった 1 つを名前で返す）。この検査は `--start-time` の衝突の検査より前にあります。通れば [`completeEditStartLaunch()`](../../src/core/appflow/playmode.cpp#L46) が、`--free-camera` の無い起動に既定の free camera とその入力 overlay を足します（editor camera になる。`--editor-transform` は足さない）。

### 段階B: `run()` 前に共有moduleへ起動情報を注入

`main()` は [`PelicanCore`](../../src/core/userpublic/pelican_core.hpp#L8) を作った後、次の順でmoduleを設定します。

1. `EngineLaunchConfig`へCLI結果を代入。
2. `PathResolver::setup()`へproject root、絶対パス方針、project JSON、user directoryを渡す。
3. `AssetsVerification::verifyAtStartup()`(中で [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L148))でasset store manifestを検査し、結果を RPC 用に持つ(✅AV-1。[第3章](03_project_and_loading.md) §3.4)（asset store = アセットの置き場所の間接化。`project.json` の `asset_stores` が論理マウントを宣言し、実パスは git に入れない `.pelican/local.json` で上書きします。[第3章](03_project_and_loading.md) の「asset store」／[用語集](../manual/14_glossary.md)）。
4. `ProjectSource`へproject JSONとengine version無視方針を保存。
5. `PelicanCore::run()`を呼ぶ。

この「`run()`前のmodule設定」があるため、`PelicanCore`だけを見てもproject rootの受け渡しは見つかりません。入口とcoreの両方を読む必要があります。

### 段階C: `PelicanCore::run()` でruntimeを組み立てる

[`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L100) は短いですが、初期化の正規順序を定義しています。

1. ローカルな `FastModuleContainer` と `RuntimeTeardownGuard` を生成。
2. `StartupMetrics` を開始。
3. **XR activationを決定**: [`resolveXrActivation()`](../../src/core/xractivation.hpp) がheadless/RPC/replay等のforced-offとOpenXR discovery hookから `launch_config.xr_active` を確定。
4. constructorへ渡されたsettings JSONを`ProjectSource`の上書きsourceへ設定し、[`watch::ReloadGate.configureFromLaunch()`](../../src/core/watch/reloadgate.hpp) を呼ぶ。
5. [`recoverRenderConfigDocumentTransaction()`](../../src/core/userpublic/pelican_core.cpp#L125) — プロセスのクラッシュをまたいだ project 文書の transaction を、`ProjectBasicConfig` や `Renderer` がdocument setのどれかを観測する**前**に決着させます。この transaction は project 文書全般のもので、rendering config の編集と `project migrate` の apply が共用します(MG-1 で実体が [`recoverProjectDocumentTransaction()`](../../src/project/projectdocumenttransaction.cpp#L1093) として `pelican_project` へ移り、ここで呼ぶ名前は [editor 側の別名](../../src/core/communication/renderconfigtransaction.hpp#L36) として残りました。ディレクトリと形式は変わっていません)。なお `pelican_player` は project.json を持つ project を開くとき、`PelicanCore::run()` より前、`parseProjectEnvelopeText` の手前の MG-1 の入口([`requireProjectMigrationNotRequired()`](../../src/project/projectmigration.cpp#L563)、[呼ぶ場所](../../src/player/main.cpp#L739))で同じ復旧を先に一度走らせます。この段の復旧はそのあとで、ふつうは片付けるものが残っていません。ここでいう document set は「rendering config の root 文書と、それが参照する feature JSON 断片を、1つのtransactionでまとめて書き換える単位」です（[`RenderConfigCandidateDocumentSet`](../../src/project/renderconfigdocument.hpp#L165)。ヘッダのコメントが `A render-authoring candidate is a document set, not a replacement string for the root document.` と書いています）。root だけ新版で断片が旧版、という中途半端な組み合わせを誰にも観測させないために、この復旧が最初に来ます。
   復旧が読む「このtransactionはcommit済みか」の答えは、`manifest.json` の中のフィールドではなく **`committed` という別ファイルが在るかどうか**です（[`committedMarkerName`](../../src/project/projectdocumenttransaction.cpp#L324)、判定は [`transactionDecision()`](../../src/project/projectdocumenttransaction.cpp#L409)）。commit側は `committed.next` を書いてから [`linkExclusive()`](../../src/project/projectdocumenttransaction.cpp#L217) で `committed` という**それまで存在しなかった名前**を作ります。生きているファイルを上書きしないので、「途中まで書けた commit 済み印」という状態が原理的に作れません（R1-2）。marker は自分が属する manifest の digest を持っていて、`manifest.json` が別物にすり替わっていれば [`render_authoring_manifest_mismatch@1`](../../src/project/projectdocumenttransaction.cpp#L421) で名前付きに落ちます —— 黙って rollback すると commit 済みの編集を消す破壊操作になるためです。旧形式(marker を持たず、`manifest.json` の `status: "committed"` で決定を記録していた形)のディレクトリは、MG-2a から**読みません**。[`rejectLegacyTransactionDirectory()`](../../src/project/projectdocumenttransaction.cpp#L399) が判定の先頭で `render_config.legacy_transaction_dir@1`(error_fields は `path`)を投げ、rollback も committed としての片付けもせずにディレクトリを残します。この段で投げられると `PelicanCore::run()` の catch が `Pelican fatal error` として記録し、起動はそこで終わります(player では先に MG-1 の入口の復旧が同じ例外に当たるので、stderr に 1 行出して exit 1 で終わります。コードを読んで確認、実行では未確認)。以前はこの形を committed として読む互換を残していましたが(旧形式の dir が残った project が二度と commit できなくなるのを避けるため)、MG-2a で「黙って読む」から「名前付きで止める」へ替わりました。片付けの手順は [manual 第3章 §3.1](../manual/03_project_format.md#31-プロジェクトとは) にあります。
6. `Persistence`からsettingsを読み、audio設定を適用。
7. [`configureRayLayoutModeBeforeComponentRegistration()`](../../src/core/userpublic/pelican_core.cpp#L41) — rendering configから ray layout の意味モードを求めて [`RayLayoutRuntimeState`](../../src/core/shader/raygrouplayout.hpp#L197) へ固定します。意味モードは `LegacyNoLayout` / `LayoutV3` の2値で（[`RayLayoutSemanticMode`](../../src/core/shader/raygrouplayout.hpp#L75)）、ray layout 文書が無い（`LegacyNoLayout`）かある（`LayoutV3`）かだけを表します。MG-2a より前は版ごとの `LayoutV1Q` / `LayoutV2` もありましたが、layout は version 3 だけを読むようになり、版差を運ぶ値とフィールド（`RayGroupLayout::version` / `semantic_mode` など）は消えました。どの値になるかは次の3つの判定だけで決まります。(1) rendering config の `rendering` 節に `ray_group_layout` key が無ければ(root は MG-2b から `pelican.graph` だけで、それ以外は [`RenderConfigLegacyRoot`](../../src/core/shader/raygrouplayout.cpp#L1485) で拒否) [`loadRayTracingProjectConfig()` が空を返し](../../src/core/shader/raygrouplayout.cpp#L1492)、[`rayLayoutSemanticMode()` の `config == nullptr` 枝](../../src/core/shader/raygrouplayout.cpp#L777)が `LegacyNoLayout` を返します（`ray_bindings` だけ書くと `pelican.ray_tracing.bindings_without_layout@1` で拒否）。(2) key があれば参照先の layout 文書を [`parseRayGroupLayout()`](../../src/core/shader/raygrouplayout.cpp#L843) が読み、読めれば [`rayLayoutSemanticMode()` の layout 有りの枝](../../src/core/shader/raygrouplayout.cpp#L778)が `LayoutV3` を返します。(3) 読めないのは2通りです。schema 名が違うか `version` が整数でなければ [schema エラー](../../src/core/shader/raygrouplayout.cpp#L859)、整数でも 3 以外(1 と 2 を含む)なら [`pelican.ray_group_layout.version_unsupported@1`](../../src/core/shader/raygrouplayout.cpp#L866) で拒否されます。旧版を旧い意味で読む分岐は残っていないので、version 1 / 2 の layout は version 3 の形(各 group に `type`)へ書き直すしかありません。組み込みComponent登録より前に置くのは、`pelican_ray_instance` の登録可否がこのモードで決まるためです（[`builtinRayInstanceEnabled()`](../../src/core/shader/raygrouplayout.hpp#L206) が真になるのは `LayoutV3` のとき、つまり layout があるとき。§3.11）。プロセス寿命の間このモードは変えられません。✅RTL-1b(`8034d096`)から、モードの判定は `loadRayTracingProjectConfig()` の中ではなく(この関数は純粋な parse に戻り、起動時の先読み・登録・scene の ray の計画が同じ関数を呼ぶ)、render pipeline の登録が候補の task の parse・shader の compile・世代の検証を全部通した後、staging の前に [`RayLayoutRuntimeState::validate()`](../../src/core/shader/raygrouplayout.hpp#L203) で行います([登録の mode の判定](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1525))。食い違いは型付きの [`RayLayoutModeChanged`](../../src/core/shader/raygrouplayout.hpp#L180)(`what()` は前と同じ `pelican.ray_layout_mode_change_requires_restart@1: …`)で、render config の editor はそれだけが理由の候補を commit して `restart_required`(`ray_layout_mode_changed`)を返します。engine の fallback の世代の登録([`isRenderPipelineFallbackConfig()`](../../src/core/vkcore/renderer_config.cpp#L448) が真の root)は判定しません。今のモードは [`currentRayLayoutMode()`](../../src/core/shader/raygrouplayout.cpp#L781)(未設定なら `std::nullopt`)で読み、`get_status.ray_tracing.ray_layout_mode` がこれを返します。 ✅BS-1(`d9811bfc`)から、この呼び出しは try の中にあります。`--render-fallback` の起動に限り、投げた例外を投げ直さず、[`renderPipelineRefusalJson()`](../../src/core/vkcore/renderer_config.cpp#L468) で作った拒否を [`RenderPipelineStartupRefusal`](../../src/core/vkcore/renderer.hpp#L165) に書き、mode を `LegacyNoLayout` に固定して進みます([先読みが断られたときに layout 無しにする所(BS-1)](../../src/core/userpublic/pelican_core.cpp#L138))。引数が無ければ今までどおり投げ直し、下の catch が致命の行を書きます。
8. [`ECSPredefinedRegistration::reg()`](../../src/core/ecs/predefined.cpp#L22) で組み込みComponent/Systemを登録。
9. **[`initializeConfiguredGameLogic()`](../../src/core/gamelogic/gamelogicreload.cpp#L917) でgame DLLをロード**（WP110系列。§2.11）。失敗しても`run()`は続行し、`error_code` と `error_fields` を添えて [`LOG_ERROR`](../../src/core/userpublic/pelican_core.cpp#L157) するだけです。保存された失敗はRPC `status` から読めます（§2.11）。
10. **`(void)GET_MODULE(Renderer)`**（[この1行](../../src/core/userpublic/pelican_core.cpp#L165)） — render graphの検証と公開を、scene loadのprepareが `ModelAssetContainer` を実体化してmodelをuploadするより**前**に済ませます（WP380a-fix。§3.8）。 ✅BS-1: [`Renderer::Renderer()`](../../src/core/vkcore/renderer.cpp#L3236) は、`--render-fallback` の起動なら、7 番が残した拒否か、自分の `snapshotMaterialRouteDefault()` と `loadRenderGraphVariantsFromConfigWithStartupFeatureOverlays()` が投げた例外を拒否の JSON にし、error の行を書いて、[`renderPipelineFallbackConfigJson()`](../../src/core/vkcore/renderer_config.cpp#L420) の文書を `ProjectBasicConfig` に publish してから、同じ overlay 付きの読み込みでそれを組みます。拒否は `render_pipeline_fallback_refusal` に持ち、project の構成を最初に publish したときに消えます。引数が無ければ例外はそのまま `run()` の catch に届き、致命で終わります(manual 第6章 §6.16)。 ✅PB-1(`a55d1eaf`): constructor は拒否の有無に関わらず [`ProblemList`](../../src/core/watch/problemlist.hpp#L127) の module を作り(module の graph が固まる前に要るため)、fallback に入るときは拒否を `source: "startup"` の 1 件として記録します([`recordRenderPipelineProblem()`](../../src/core/watch/problemlist.cpp#L361))。project の世代で起動した `--render-fallback` の起動では、最後に [`MaterialStartupUnserved`](../../src/core/material/materialstartupunserved.hpp#L20) の `collecting` を立てます。この後、loop に入る前までに登録される material(engine の組み込み material と起動の scene の material)の route を受ける pass が無くても、`material_route_unserved@1` で止まらずにここへ書き留められます(§2.4 の `enterStartupFallbackForUnservedMaterials()`)。
11. **`--start-state edit` なら先に edit にする**（✅PM-1b。[`internal::applyLaunchStartState()`](../../src/core/userpublic/pelican_core.cpp#L167)。`EngineTime` を `edit` にし、behavior の arena を眠らせるので、次の起動 scene は instance の無い behavior で読み込まれる。§2.6）。そのあと `ProjectBasicConfig.defaultSceneId()` のsceneを [`SceneLoadRoot::startup`](../../src/core/userpublic/pelican_core.cpp#L172) の `Borrowed` requestで即時ロード（§3.5 の 3）。`Borrowed` は scene load 要求の2種別のうち「いま commit 済みの projection をそのまま借りて読む」側で、新しい document を持ち込む `Owned`（import 経路）と対です（[`SceneLoadRequest`](../../src/core/loader/sceneload.hpp#L222)）。
12. `ProjectMaterialAssetContainer` と `ModelAssetContainer` を明示的に生成（並列prepareのcommitを起動スレッドで実施）。
13. [`watch::ReloadService.setup()`](../../src/core/watch/reloadservice.hpp#L207) でwatcherのlive inventoryを種付け。
14. `Loop::run()`へ入る。
15. loop終了後、Vulkan deviceをidleまで待つ。
16. [`internal::runTerminalShutdown(teardown)`](../../src/core/userpublic/pelican_core.cpp#L211) — `teardown.run()` でruntime資源を順序付き解放し、続けて `shutdownConfiguredGameLogic()`。
17. 関数を抜けるとmodule containerがmoduleを生成逆順に破棄。

16番はtry-catchの**外**にあります（[呼び出し箇所](../../src/core/userpublic/pelican_core.cpp#L211)）。この2手を [`terminalshutdown.hpp`](../../src/core/terminalshutdown.hpp#L10) の `noexcept` 関数1本にまとめてあるのはWP379aで、ヘッダのコメントが規範です — playerとlifecycleの受け入れテストが**同じ終了順序**を共有します（テスト側の呼び出しは [`raytracing_rt3_1_production_test.cpp` 内](../../test/raytracing_rt3_1_production_test.cpp#L4527)）。順序が2箇所に書かれていれば、テストだけが正しい順で終わる状態を作れてしまいます。

`RuntimeTeardownGuard` は関数冒頭（[`pelican_core.cpp` 内](../../src/core/userpublic/pelican_core.cpp#L102)）で `RuntimeTeardownMode::terminal_shutdown` として作られ、`FastModuleContainer::beginShutdown()` はguardの外ではなく [`RuntimeTeardownGuard::run()`](../../src/core/appflow/teardown.cpp#L149) の内部で、8段階の解放へ入る直前に呼ばれます（[`teardown.cpp` 内](../../src/core/appflow/teardown.cpp#L158)）。したがって初期化途中で例外が出ても、同じ「新規module生成を閉じてから順序解放」という経路を通ります。

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

例として [`Renderer` のconstructor](../../src/core/vkcore/renderer.cpp#L3236) は [`loadRenderGraphVariantsFromConfig()`](../../src/core/vkcore/renderer_config.cpp#L589) を呼ぶだけに見えますが、その内部で次のmoduleが連鎖的に生成されます。ただし現在は、[`Renderer::prepareRuntimeModules()`](../../src/core/vkcore/renderer.hpp#L371) と [`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L259) により「render前に依存を全解決してから、以後の新規module生成を禁止する（module graphを凍結する）」方式へ変わっています。

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
- [`FastModuleContainer::freezeCreation()`](../../src/core/container.hpp#L199) がLoop開始直前に呼ばれ（[loop.cpp](../../src/core/appflow/loop.cpp#L538)）、以後の新規module生成はエラーになります。[`tryGet<T>()`](../../src/core/container.hpp#L144) は生成せずoptional参照を返します。`graphSnapshot()` がmodule依存グラフを記録します。
- constructor内の`GET_MODULE()`が隠れた依存になります。調査時はconstructorと全`GET_MODULE`呼び出しをセットで検索します。
- module実体は型ごとのstaticなので、containerが管理するのは「何個目のcontainerか」ではなく「生存スコープが今1本開いているか」だけです。スコープは重ねられず、生きているcontainerがあるうちに2個目を作るとconstructorが`logic_error`を投げます（[throw 箇所](../../src/core/container.hpp#L124)、[`module lifetime scopes cannot overlap`](../../test/module_container_test.cpp#L214)）。したがって「containerごとにmoduleの寿命が分かれる」ことはありません。テストがcontainerを作るのは、スコープを抜けるときに登録済みmoduleを（どこで生成されたものでも）全て破棄させるためで、destructorが最後にphaseを`booting`へ戻すので同じプロセスで次のcontainerを作り直せます。
- 先に`main()`で作られたmoduleも同じstatic `cleaners`へ載るため、`PelicanCore::run()`内のcontainer破棄時にまとめて片付けられます。

> 🧩 **難所 — 「生成の逆順」が成立する条件**([`FastModuleContainer::get<T>()`](../../src/core/container.hpp#L149) / [`~FastModuleContainer()`](../../src/core/container.hpp#L258))
>
> **何をする所か**: module 実体の遅延生成と、破棄順序の記録です。上の「基本は初期化の逆順」がなぜ正しいのか、そしてどこで破れるのかがここに書かれています。
>
> **素朴に読むと**: 「積んだ順の逆に pop するから安全」で終わりに見えます。しかし成立の根拠はもう一段細かい所にあります — [`cleaners.push_back()`](../../src/core/container.hpp#L181) は `obj_ref.emplace()` の**後**、つまり T の constructor が**終わってから**実行されます。constructor 内の `GET_MODULE(Dep)` は再帰的に先へ進むので、`cleaners` には必ず `Dep` が先、`T` が後で載ります。逆順 pop はしたがって「依存される側より、依存する側を先に壊す」になります。逆に言えば、**constructor の外で初めて `GET_MODULE` した依存は逆順保証の外**です。T の生成後に T のメソッドが初めて `Dep` を掴むと `cleaners` は `[T, Dep]` の順になり、破棄では `Dep` が先に消えて T の destructor が壊れた module を触ります。この穴は §2.4 の二つで塞ぎます — [`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L259) が loop へ入る前に全依存を実体化し、その後で [`freezeCreation()`](../../src/core/container.hpp#L199) が以後の初回生成を禁止します（実体化するのは前者、禁止するのは後者です）。
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

moduleのC++ destructorだけに任せると、ECS Componentの`deinit()`が参照する描画moduleが先に壊れるほか、pending/frozen event、behavior deferred mutation、GPU deletion callbackがowner/DLLの破棄後まで残る可能性があります。ここでの **pending event** は `emit()` されてまだ配送されていない列（[`pending_events`](../../src/core/userpublic/details/event/registerer.hpp#L110)）、**frozen event** はフレーム冒頭の `freeze_events` でそこから移された「今回配送する分」の列（[`deliver_now_events`](../../src/core/userpublic/details/event/registerer.hpp#L111)）です。どちらも payload を `shared_ptr<const void>` と発行元の `RegistrationOwner` で持つ（[`QueuedEvent`](../../src/core/userpublic/details/event/registerer.hpp#L37)）ので、game DLL 由来のpayloadが列に残ったままDLLを外せない、というのが下の7番の理由です（列の意味は §2.5 の「eventの1フレーム遅延」）。そこで [`teardownRuntimeNoThrow()`](../../src/core/appflow/teardown.cpp) が、module optionalの存在を確認して次の順で明示解放します。

1. Vulkan `waitIdle()`
2. behavior owner callbackを完了
3. physics bindingをclear（現在は `#if PELICAN_WITH_PHYSICS` 内）
4. ECS entityをclearし、全Componentの`deinit()`とdestructorを実行
5. model instanceをclear
6. behavior deferred mutationをdrain
7. pending/frozen eventをdrain
8. GPU deletion queueをdrain（GPU deletion queue = まだGPUが読んでいるかもしれないVulkan資源を、その場で壊さず「いまのretirement batch」へ預けておく仕組みです。batchは [`leaseForNextSubmission()`](../../src/core/vkcore/deletionqueue.hpp#L98) で1回のGPU submissionへ紐付けられ、submission側がleaseを手放したときに解放されます — リングバッファでもスタックでもなく、submission単位のbatchを `weak_ptr` で並べた形です（[`DeletionQueueCore`](../../src/core/vkcore/deletionqueue.hpp#L17)））

terminal shutdownはこの前に`FastModuleContainer::beginShutdown()`で新規module生成を閉じ、最後のdeletion queueもcloseします。game-logic reloadの`runtime_reset`は同じ順で既存workをdrainしますが、module phaseとdeletion queueを再利用可能に保ちます。

各段階は例外を飲み込みつつログを残します。[`RuntimeTeardownGuard::~RuntimeTeardownGuard()`](../../src/core/appflow/teardown.cpp) も`run()`を呼ぶため、loopや初期化の途中で例外が出ても同じ順序を通ります。

この契約は [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp) の「Componentのdeinit/destroyがmodule destructorより先」と、[`lifetime_teardown_test.cpp`](../../test/lifetime_teardown_test.cpp) の全工程fault injection（意図的に各段階を失敗させて、残りの段階が飛ばされないか・順序が崩れないかを確かめるテスト手法）、起動順列、起動途中失敗で検証されています。

> 🧩 **難所 — 一本の順序が二つのモードを兼ねる**([`RuntimeTeardownGuard::run()`](../../src/core/appflow/teardown.cpp#L149) / [`runProductionStep()`](../../src/core/appflow/teardown.cpp#L63))
>
> **何をする所か**: 上の8段階を、terminal shutdown と game-logic reload の `runtime_reset` の**両方**で回します。順序の定義は [`runtime_teardown_order`](../../src/core/appflow/teardown.hpp#L23) の1本だけで、モードごとの別経路はありません。
>
> **素朴に読むと**: 「shutdown と reload は別処理」と読みたくなりますが、差分は実質2箇所です。(1) [`beginShutdown()`](../../src/core/container.hpp#L213) を呼ぶかどうか — `run()` の中で `mode == terminal_shutdown` のときだけ、8段階へ入る**直前**に呼びます(container の destructor ではありません)。(2) 最後の deletion queue の扱い — [`RuntimeTeardownStep::deletion_queue`](../../src/core/appflow/teardown.cpp#L94) が `FastModuleContainer::phase()` の値で分岐し、`shutting_down` なら `drainForTeardown()`（空にしたうえで以後の登録を拒否）、そうでなければ `flushAll()`（空にするだけで受付は継続）です。つまり mode フラグを8段階へ引き回すのではなく、入口で phase を動かしておき、後段は**そのときの phase を読んで**振る舞いを決めます。もう一つの要点は `runProductionStep()` が全段階で `tryGet<T>()` しか使わないことです。teardown 中は [`requireCreationAllowedLocked()`](../../src/core/container.hpp#L98) が生成を拒否するので、`GET_MODULE` を1つでも混ぜると「片付けようとして例外」になります。
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
> **手がかり**: [`cleanupStep()`](../../src/core/appflow/teardown.cpp#L22) が段階ごとに例外を飲むので、**途中の失敗が後続段階を飛ばしません** — 「wait_idle が落ちたので event が残った」という連鎖を作らないための構造です。`RuntimeTeardownActions` を取る overload は各段を `std::function` で差し替えるテスト用の面で、空の `std::function` は「起動が失敗してその module がまだ無い」を表します(ヘッダのコメント [該当箇所](../../src/core/appflow/teardown.hpp#L49) が規範)。`completed` フラグがあるため、§2.1 の12番(明示 `teardown.run()`)と destructor 経由が重なっても8段階は1回しか走りません。テストは [`lifetime_teardown_test.cpp` 内](../../test/lifetime_teardown_test.cpp#L196)(規範順序)/ [同ファイルの reset 側](../../test/lifetime_teardown_test.cpp#L235)(runtime reset が terminal shutdown へ入らない)/ [同ファイルの起動失敗側](../../test/lifetime_teardown_test.cpp#L312)(起動途中失敗)。
>
> **不変条件**: teardown 経路では module を新規生成しない(`tryGet` のみ)。段階の順序を変えない。`run()` は何度呼んでも副作用が1回。`runtime_reset` は module phase を `shutting_down` にしない(reload 後もフレームを回し続けるため。`shutting_down` にすると deletion queue が `drainForTeardown()` で閉じ、以後のGPU資源登録が拒否されます)。

## 2.4 `Loop::run()` の五経路

中心は [`Loop::run()`](../../src/core/appflow/loop.cpp#L772) です。loop本体へ入る前に、入力のrecord/replay（[`InputSequenceRuntime`](../../src/core/os/inputsequence.hpp#L61)、[loop.cpp](../../src/core/appflow/loop.cpp#L784)）、replay時のfixed_step切替（[その分岐](../../src/core/appflow/loop.cpp#L783)）、camera bakeの開始（[その呼び出し](../../src/core/appflow/loop.cpp#L795)）、[`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L259) と [`FastModuleContainer::freezeCreation()`](../../src/core/appflow/loop.cpp#L806) が実行されます。
✅PB-1(`a55d1eaf`)から、この 2 つの間で [`Renderer::enterStartupFallbackForUnservedMaterials()`](../../src/core/vkcore/renderer.cpp#L3318) を呼びます([呼ぶ所(PB-1)](../../src/core/appflow/loop.cpp#L800))。
`collecting` を倒し、`--render-fallback` の起動で route を受ける pass の無い material が書き留められていれば、`material_route_unserved@1` の拒否を作って一覧に記録し、
fallback の JSON を publish して組み直し(登録済みの material は fallback と照らさない `validate_live_materials = false`)、fallback の世代で loop に入ります。module の生成は固める前なので、ここで組み直せます。

フレームそのものは三つの経路で三度書かれてはいません。[`FrameStep`](../../src/core/appflow/loop.cpp#L413) が `advance` → `update` → `render` → `record` の4段を1箇所に書き、三つのドライバ([`runHeadlessFrames()`](../../src/core/appflow/loop.cpp#L634) / [`runInteractive()`](../../src/core/appflow/loop.cpp#L695) / [`runXrFrame()`](../../src/core/appflow/loop.cpp#L548))はこの4呼び出しへ渡すものだけが違います。経路差は三つの enum として明示され、差が無い呼び出し点でも値を書きます — [`AdvanceTiming`](../../src/core/appflow/loop.cpp#L375)(update 計測を timeline advance の前に開くか後に開くか)、[`GraphVariantSelection`](../../src/core/appflow/loop.cpp#L387)(graph variant を driver が選ぶか `Renderer::render()` に任せるか)、[`FramerateWait`](../../src/core/appflow/loop.cpp#L401)(`FramerateAdjust` で待つか)。

経路は次の五つです。

| 経路 | 条件 | フレーム進行の所有者 |
|---|---|---|
| headless RPC | `headless` かつ `rpc` | RPCクライアントの `step_frame` |
| headless固定フレーム | `headless` | `--frames` またはreplayのフレーム数 |
| windowed + XR | `xr_active` かつXRセッションがrunning | OpenXRの `waitFrame` |
| windowed + RPC | windowed かつ `rpc` | windowのフレーム。RPCはフレーム境界で処理 |
| windowed | 上記以外 | windowのフレーム |

windowed + RPC は他のwindowed経路と排他ではなく、同じwhileループへ**追加**される層です。ホスト自体はwhileループへ入る前に1個だけ作られ（[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L485)）、ループ側に増えるのは `update_interactive_state` の中のdispatch1行だけです（[その1行](../../src/core/appflow/loop.cpp#L712)）。

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

GLFW callbackは [`Window`](../../src/core/os/window.hpp#L17) の`input_events`へイベントを積み、loopが`drainInputEvents()`して`InputState`へ渡します。ゲームロジックは直接GLFW状態を問い合わせません。windowedではF5キーでgame logic reloadを要求できます（[loop.cpp](../../src/core/appflow/loop.cpp#L713)）。`FramerateAdjust`はwindowedのみです。

フレーム内で状態更新にあたる部分は `update_interactive_state` ラムダ（[loop.cpp](../../src/core/appflow/loop.cpp#L710)）に集約され、windowedとwindowed+XRの両方から呼ばれます。ここが「そのフレームの状態更新が終わった直後」を表す唯一の点であり、windowed RPCのdispatchもF11キャプチャのarmもこの中にあります。

### windowed + RPC（フレーム境界dispatch） ✅実装済み

`--rpc` をwindowedで指定すると、通常のwindowed経路に [`WindowedRpcHost`](../../src/core/communication/rpcserver.hpp#L166) が重なります（[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L481)、`#if PELICAN_WITH_RPC`）。

```cpp
void start(std::istream &input, std::ostream &output) {
    endpoint_ = std::make_unique<EngineRpcEndpoint>(input, output);
    host_ = std::make_unique<WindowedRpcHost>(
        input, output,
        [this](std::string_view line) { return endpoint_->processLine(line); },
        defaultWindowedRpcQueueCapacity);
}
```

この経路の要点は、readerスレッドとエンジンスレッドの分業です。

- 読み取り専用スレッドは**行をqueueへ積むだけ**で、エンジン状態に一切触りません。
- dispatchは [`WindowedRpcHost::processFrameBoundary()`](../../src/core/communication/windowedrpchost.cpp#L119) がエンジンスレッドで行い、呼び出し点は `updateFrameState()` の直後（[loop.cpp](../../src/core/appflow/loop.cpp#L712)）です。
- queue容量は [`defaultWindowedRpcQueueCapacity = 64`](../../src/core/communication/rpcserver.hpp#L187) です。溢れたリクエストには**readerスレッドが即座に**エラーを返します（[`busyResponse()`](../../src/core/communication/windowedrpchost.cpp#L39)）。コードは `-32000`（`applicationError`）、メッセージは `windowed rpc request queue is busy`、`data` に `reason: "busy"` と `queue_capacity` が入ります。
- デストラクタは、`std::istream` に移植可能なキャンセル手段がないため、readerがまだブロック中なら `detach()` します（[`windowedrpchost.cpp` 内](../../src/core/communication/windowedrpchost.cpp#L95)）。本番はプロセス寿命の `std::cin` を使う前提です。

> **設計決定:** windowed RPCは「フレームの状態更新が終わった直後に一括処理」であり、任意タイミングの割り込みではありません。エディタからの編集要求がフレームの途中でECSやGPU資源へ触れないため、決定性とteardown順序の契約をそのまま保てます。

### windowed + XRセッション実行中

`launch_config.xr_active` かつXRセッションがrunningの間は、同じwhileループ内で専用経路を通ります（[`runXrFrame()`](../../src/core/appflow/loop.cpp#L548)、`#if PELICAN_WITH_OPENXR`）。

```text
pollEvents → waitFrame → EngineTime::advance
→ VulkanManageCore::setCurrentFrameIndex(frameIndex)
→ beginFrame → locateViews
→ syncActions（action backendとpose samplesをInputStateへ注入）
→ update_interactive_state(xr_frame = true)
→ selectGraphVariant(xr) → XrCompositionTarget.prepareFrame
→ renderLogicalFrame(2 views) → XrMirrorSink.tryPresent → recordMirrorStatistics
```

このフレームで実際に描画するかは、`shouldRender()` だけでは決まりません（[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L577)）。

```cpp
const bool xr_frame_renderable =
    begin_result == OpenXr::XrBeginFrameResult::ready &&
    display_timing.shouldRender();
```

つまり `beginFrame()` が `ready` を返し、**かつ** display timingが描画を要求したときだけ `locateViews()` と `renderLogicalFrame()` を実行します。描画しないフレームでも状態更新（`update_interactive_state`）は必ず走るため、ゲーム状態とXRの表示リズムは分離されています。

mirror表示の結果はsessionへ戻され、`xr_session->recordMirrorStatistics(presented, dropped, failures)`（[loop.cpp](../../src/core/appflow/loop.cpp#L608)）で presented / dropped / failures が記録されます。

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

#### `--start-time` の replay prologue（O-1）

`--start-time` が指定されると、上のループの**前**にもう1本のループが走ります（[replay prologue の本体](../../src/core/appflow/loop.cpp#L672)）。`start_frames - 1` 枚を、captureだけ外して同じ3呼び出し（`advance` → `updateFrameState` → `render`）で回してから、本番のフレームへ入ります。

```text
offline: start_time=<秒> start_frame=<番号> replay=<枚数> frames=<枚数>   ← stderr へ1行
（replay 枚数だけ headlessFrame(capture=false)）
→ 通常の headless ループ（--frames 枚、capture あり）
```

**時刻を飛ばさないのは手抜きではなく、飛ばせないからです。** 1フレームの見た目を決める状態は時計から再構成できません —— 共通グラフのクロックは `render()` の中でしか進まず、プロジェクトのゲームSystemは自分で `dt` を積分し、animation componentの評価器に至っては「時刻tのポーズをくれ」と聞ける相手ではありません（[その理由を書いたコメント](../../src/core/appflow/loop.cpp#L663)）。したがって時刻tに居る唯一の正直な方法は、その手前の全ステップを通ってくることです。stderr の1行に `replay=` を出すのは、「何枚ぶん先に回したか」が出力の再現条件そのものだからです。

`headless_frames == 0`なら無制限です。入力replay時はフレーム数をreplayから導出します（[入力replayからのフレーム数導出](../../src/core/appflow/loop.cpp#L681)）。出力パスに`%d`または`%0Nd`があればフレームごとに保存し、なければ最後のフレームだけ保存します（[`parseRenderOutPattern()`](../../src/core/appflow/loop.cpp#L75)）。

### headless RPC

RPCモードでは通常のfor-loopへ入らず、[`runEngineRpcServer(std::cin, std::cout)`](../../src/core/appflow/loop.cpp#L817) を呼びます。フレーム進行はクライアントの`step_frame`要求が所有します。stdoutはNDJSON protocol（newline-delimited JSON — 1行に1個のJSON値を置く形式。ここでは1行が1リクエストまたは1レスポンスにあたるため、ログを1行でも混ぜると相手のparseが壊れます）専用なので、[`PelicanCore` constructor](../../src/core/userpublic/pelican_core.cpp#L95) がloggerをprotocol対応で初期化します。

### 全経路に共通する `setCurrentFrameIndex()`

`engine_time.advance()` の直後に `vulkan_.setCurrentFrameIndex(engine_time_.evaluationIndex())` を呼ぶのは、いまや三経路に一つずつではなく [`FrameStep::advance()`](../../src/core/appflow/loop.cpp#L422) の中の[評価の番号を渡すこの1行](../../src/core/appflow/loop.cpp#L426)だけです(渡すのは game のフレームではなく評価の番号。§2.6。✅PM-1a)。headless RPC の `step_frame` / `advance_frames` は `FrameStep` を通らず、[`rpcserver.cpp`](../../src/core/communication/rpcserver.cpp#L2093) の中で同じ 2 行を自分で呼びます。三つのドライバはどれもフレームの先頭でこの `advance` を呼ぶので、「ある経路だけ呼び忘れる」形が構造として作れません。これが [`VulkanManageCore`](../../src/core/vkcore/core.hpp#L36) 側の `logical_frame` 軸になり、debug-utilsラベルとGPU timingの計測が同じフレーム番号で並びます。

### F11 RenderDocキャプチャ（windowedのflat描画のみ） ✅実装済み

windowedでは、F11でRenderDocのin-applicationキャプチャを1フレーム分だけ要求できます。二段構えです。

1. **arm**（アーム — 「次に描く1フレームを撮る」という予約だけを立てて、実際の発行は後段へ任せる状態にすること）: [`requestF11CaptureIfNeeded()`](../../src/core/appflow/loop.cpp#L319) が `UserInput::isKeyPushed(KeyCode::F11) && capture.available()` のときだけ `RenderDocCapture::request(RenderDocCaptureSource::f11, xr_active)` を呼びます。呼び出しは `update_interactive_state` の末尾（[F11 arm 呼び出し箇所](../../src/core/appflow/loop.cpp#L718)）で、拒否されてもログを出すだけでフレームは継続します。
2. **capture**: 実際のキャプチャは [`renderFlatFrameWithOptionalCapture()`](../../src/core/appflow/loop.cpp#L329) が行います（呼び出しは [windowed capture 呼び出し](../../src/core/appflow/loop.cpp#L759)）。`state() == armed` のときだけ `captureArmedFrame()` で `StartFrameCapture` / `EndFrameCapture` を明示発行し、それ以外は素の `renderer.render()` です。

> **設計決定:** キャプチャが失敗しても**論理フレームは必ず1回描画されます**。`captureArmedFrame()` へ渡すコールバックが `rendered` フラグを立てるので、`EndFrameCapture` が描画後に失敗しても二重描画にはなりません（[`loop.cpp` 内](../../src/core/appflow/loop.cpp#L332)）。デバッグ機能がフレーム進行の正しさを壊さない、という線引きです。

[`RenderDocCapture`](../../src/core/renderdoc/renderdoccapture.hpp#L66) は `Renderer` がVulkan instanceを作る前に [`resolveLoopModules()`](../../src/core/appflow/loop.cpp#L156) で解決されます。RenderDoc自体をロードするのではなく、**既に注入されているAPIを観測するだけ**です。終了時は [`finishLoopResources()`](../../src/core/appflow/loop.cpp#L302) の先頭で `renderdoc_capture.beginShutdown()`（[RenderDoc shutdown 呼び出し](../../src/core/appflow/loop.cpp#L307)）を呼びます。XR経路にはキャプチャ点がありません（flat描画のみ対象）。

## 2.5 1フレームの五つの状態フェーズ

順序は [`frame_phase_order`](../../src/core/appflow/framephase.hpp#L17)、実装は [`updateFrameState()`](../../src/core/appflow/framephase.cpp#L152) です。フェーズが始まる前に、フレーム境界の処理が二つ走ります。

```cpp
void updateFrameState() {
    auto modules = resolveFrameStateModules();
    // Reload publication is a frame-boundary operation and happens before any
    // phase can observe game/runtime state.
    if (modules.reload_service != nullptr) modules.reload_service->applyFrame();
    invokeEditorCommitQueueHook();
```

1. [`ReloadService.applyFrame()`](../../src/core/appflow/framephase.cpp#L147) — reloadの公開をフレーム境界へ揃えます。
2. [`invokeEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L146) — 編集トランザクションのコミットキューを流します（後述）。

| 順 | `FramePhase` | 実際の処理 | 保証 |
|---:|---|---|---|
| 1 | `freeze_events` | pending eventを今回配送分（= frozen event）へswap。本節末の「eventの1フレーム遅延」に実体 | この後emitされたeventは次フレーム |
| 2 | `freeze_input` | `input_sequence.prepareFrame()` → `beginFrame()` → `recordFrame()`（record/replay挿入点） | フレーム中の生入力が不変 |
| 3 | `freeze_actions` | ImGuiへの入力ルーティング → `ui::UiModule.routeFrameInput()`（toolがポインタを消費していない場合）→ Action mapを一回だけ評価 | 全Systemが同じAction値を見る |
| 4 | `deliver_events` | frozen eventをゲームSystemへ登録順にdispatch | event handlerがupdateより先 |
| 5 | `update_game` | 内部ECS → ゲームSystem → animation → sequence → pending scene load | 構造変更の境界を一本化 |

### 編集コミットキューのフック ✅実装済み

`invokeEditorCommitQueueHook()` は、エディタの編集トランザクションをフレーム境界で公開するための唯一の穴です。契約は [`framephase.hpp` 内](../../src/core/appflow/framephase.hpp#L35) が正で、次の三点です。

- **single-owner**: [`installEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L129) は二重登録を拒否します。
- **位置が固定**: reload公開の後、`freeze_events` の直前。windowed / headless固定フレーム / RPC `step_frame` の全loop面で同じ位置です。
- **未設置ならzero-state no-op**: hookが無い場合、moduleを生成せずmodule graphも変えません。

実装者は現在 [`EditorJournal` の登録箇所](../../src/core/communication/editorjournal.cpp#L4348) の1箇所だけです。

> **設計決定:** 編集の公開点をフレーム境界の1箇所へ寄せることで、「エディタが動いていないビルド／セッションでは編集面が存在しない」状態を保っています。§2.4 のwindowed RPC dispatch（`updateFrameState()` の直後）と合わせて読むと、リクエスト受理→次フレーム冒頭で公開、という往復になります。

`update_game`内部の正確な順序は次です（[`framephase.cpp` 内](../../src/core/appflow/framephase.cpp#L226)）。

```text
ECSCore::update()
→ updateRegisteredGameSystems(GameContext&, all | editor_only | game_only)
→ Vrm::applicationServiceRuntime().ensureStandardPhasesRegistered()
→ Animation::animationServiceRuntime().runAllPhases(evaluation_index, all | engine_only)
→ SeqPlayer::update(current_time)
→ SceneLoader::applyPendingLoad()      # game の 1 歩のときだけ
```

**一時停止の分岐(✅PM-1a)。**`updateFrameState()` は reload 公開と commit hook の後で [`EngineTime::gameStepping()`](../../src/core/appflow/enginetime.cpp#L84) を 1 回読み、そのフレームが game の 1 歩か(`playing` か、`paused` で `advance_frames` の歩を取るフレームか)を [`game_step`](../../src/core/appflow/framephase.cpp#L174) に取ります。1 歩でないフレームでは、`runtime_spawner.applyBoundary()`・`freeze_events` と `deliver_events`(片方だけ止めると event の registerer のフレームの検査に当たるので一緒に止める)・`applyPendingLoad()` を飛ばし、game の UI には [`setEventsSuppressed(true)`](../../src/core/ui/module.hpp#L116) で event を積ませず、game system は [`GameSystemScope::editor_only`](../../src/core/userpublic/details/system/registerer.hpp#L145)(engine が [`markEditorGameSystem()`](../../src/core/userpublic/details/system/registerer.hpp#L155) で印を付けた camera controller と modal transform だけ)、animation の phase は `PhaseOwners::engine_only`(game DLL が登録した phase を外す)で走らせます。入力の凍結・内部 ECS・engine 側の animation・seq・描画は毎フレーム走り、止まった game の時刻で評価し直します。editor の system を呼ぶ間は [`ScopedEditorClock`](../../src/core/userpublic/details/system/registerer.hpp#L160) が立ち、`GameContext::deltaTime()` が game の `dt` ではなく `EngineTime::editorDt()` を返します。

**edit の分岐(✅PM-1b)。**`edit` は `gameStepping()` が常に偽の状態なので(歩は `paused` でしか頼めない)、上の「1 歩でないフレーム」の分岐がそのまま効きます。PM-1b が `updateFrameState()` に足したのは 3 つです。音の全体の一時停止を毎フレーム状態に揃え直し(`playing` 以外なら止める。[`setAllPaused()` の呼び出し](../../src/core/appflow/framephase.cpp#L161)。RPC を 1 度も受けていない `edit` 起動の engine も黙る)、`edit` では game の UI へ入力を渡さず([分岐](../../src/core/appflow/framephase.cpp#L208))、`--start-state edit` で起動した engine の `playing` のフレームでは game system を [`GameSystemScope::game_only`](../../src/core/userpublic/details/system/registerer.hpp#L148) で呼んで editor の system を外します([`system_scope`](../../src/core/appflow/framephase.cpp#L177))。状態の移り方そのものはフレームの外、RPC の `set_play_state` が呼ぶ [`internal::changePlayState()`](../../src/core/appflow/playmode.cpp#L63) にあります。`edit` と他の 2 つの間を移るときは、pending の event を捨て、`EngineTime` のセッションを始めるか終え(§2.6)、behavior の arena の眠りを切り替えてから、渡された関数で今の scene を作り直します(RPC 側は `load_scene` と同じ `SceneLoadRoot::rpc` の読み込み)。作り直しが投げたら、眠り・状態・時計を元に戻して例外を通します。最後に [`applyPlayStateSurroundings()`](../../src/core/appflow/playmode.cpp#L28) が音を揃え、`--start-state edit` の起動に限って editor の入力 overlay を stack から外すか積み直し([`setEditorInputOverlaysStacked()`](../../src/core/userpublic/userinput.hpp#L210))、見る camera を `edit` なら editor、それ以外なら game に切り替えます。`playing ↔ paused` の移り方は PM-1a のまま(時計の状態・音・overlay・camera だけ)で、作り直しません。

**再生のセッション(✅PM-2)。**`changePlayState()` の外側、[`set_play_state` の handler](../../src/core/communication/rpcserver.cpp#L2129) が、`edit` との間の移りを「セッション」として包みます。セッションの持ち物は endpoint の `Impl` が 1 つ持つ [`internal::PlaySession`](../../src/core/communication/playsession.hpp#L51) です。

- **始める(`edit` → `playing` / `paused`)**: [`requireNoPendingSceneEdit()`](../../src/core/communication/rpcserver.cpp#L976) が受け付けたまま commit されていない ticket と開いた preview を `play_busy` で断る → game logic の DLL を読んでいれば、[replay / golden の driver でないとき](../../src/core/communication/rpcserver.cpp#L2208)に reload する(reload service があれば `applyRuntimeNow`。失敗はその code で断り、セッションは始まらない)→ 今の scene があれば [`PlaySession::begin()`](../../src/core/communication/playsession.cpp#L18) が authoring の文書・`currentSceneId()`・editor の [`snapshotHistory()`](../../src/core/communication/editorjournal.cpp#L5019)(journal・last_writers・undo / redo の stack)と、そのときの journal の長さを取る → `changePlayState()` の作り直し。作り直しが投げたら `discard()` で捨てる
- **再生中**: 記録は別に持たず、[`PlaySession::recordedEdits()`](../../src/core/communication/playsession.cpp#L32) が journal の起点より後を毎回読みます。`undo` / `redo` の項目は `source_transaction_id` が記録の行を指せばその行を消し、指さなければ自分を積みます。`get_status.play` の `session` / `recorded_edits` はこれです。[`refuseDuringPlaySession()`](../../src/core/communication/rpcserver.cpp#L1023) を `load_scene` が自分で、`save_scene` / `import_scene_snapshot` が editor の RPC の hook `refuse_during_play_session` から呼び、`play_session_active` で断ります
- **終える(→ `edit`)**: `keep` の形を読み([`parsePlaySessionKeep()`](../../src/core/communication/rpcserver.cpp#L995))、`play_busy`、[`PlaySession::keptRecords()`](../../src/core/communication/playsession.cpp#L55) が記録に無い transaction_id を `play_keep_unknown_transaction` で断る → [セッションを終える所](../../src/core/communication/rpcserver.cpp#L2262)で `changePlayState()` を呼び、渡す関数の中で preview を中止し、[`Audio::stopAll()`](../../src/core/audio/audio.cpp#L867) で音を全部止め、セッションがあれば保存した scene を [`ProjectBasicConfig::prepareRestoredSceneMetadata()`](../../src/core/loader/basicconfig.cpp#L896) の token で `SceneLoadRoot::rpc` の読み込みにかける(無ければ PM-1b のとおり今の authoring から作り直す)→ [`restoreHistory()`](../../src/core/communication/editorjournal.cpp#L5028) で履歴を戻す → `keep` があれば [`replayJournalRecords()`](../../src/core/communication/editorjournal.cpp#L5036) で当て直し、`not_kept` と `kept_transaction_id` を返す
- 順が設計([design_play_mode.md](../design_play_mode.md) §6.3)と違い、runtime を作ってから履歴を戻して当て直します。当て直しは editor の edit と同じく runtime の射影まで通るので、戻した authoring から作った `edit` の runtime が先に要るためです
- `--start-state` を省いた起動の最初の `playing` にはセッションが無いので、最初の Stop は作り直すだけです。scene の無い engine もセッションを作りません

フェーズ後には `CameraBakeRecorder` が（有効時のみ）フレームを記録します（[framephase.cpp](../../src/core/appflow/framephase.cpp#L241)）。

### 共通グラフは1フレームに二度評価される

`pelican.graph` を持つプロジェクトで**かつ条件が揃ったとき**、同じ文書のスカラ側（values 分割）が**1フレームに2回**評価されます。片方は上の `update_game` の中、もう片方は §2.8 の `render()` の中です。条件は3つで、`PELICAN_WITH_ANIMATION` が ON の build であること（[`#if`](../../src/core/ecs/predefined/animationsystem.cpp#L262)）、その program が空でない pose graph と value graph を持つこと（[両方の検査](../../src/core/ecs/predefined/animationsystem.cpp#L286)）、pose graph が[スカラ出力を宣言していること](../../src/core/ecs/predefined/animationsystem.cpp#L373)です。`pose.*` を一切持たない `projects/example` は `pelican.graph` root ですが、評価は `render()` の1回だけです。

| 走る場所 | 誰が | 何のために | 使う state |
|---|---|---|---|
| `update_game`（animation） | [`AnimationSystem::updatePoseGraphs()`](../../src/core/ecs/predefined/animationsystem.cpp#L261) | animation 分割の node が今フレーム消費するスカラを集める | [`poseGraphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L153) |
| `render()` | [`Renderer` の publish](../../src/core/vkcore/renderer.cpp#L6328) | rendering 分割の sink（バッファ、音声、RT値）へ実際に書く | [`graphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L149) |

**2回評価するのに state を分けているのが要点**です。`value.trigger_clock` のような「前回の評価が何をしたか」に依存する op は state を進めるので、1つの state を共有させると1フレームで2回進んでしまいます。だから `SceneLoader` は同じ文書に対して2つの `EvaluationState` を持ち、グラフ世代の切り替えでも両方を一緒に移送します（片方だけ置いていくと、描画側は走り続けたまま姿勢側のクロックだけが巻き戻ります）。順序は固定で、animation 分割は values 分割の1段後ろに置かれます（[分割の順序](../../src/project/graphpartition.hpp#L10) / [段の定義](../../src/project/graphcomposition.hpp#L260)）。ただし animation 側が描画側の評価結果を受け取っているわけではありません —— animation フェーズは**自分の state で values 分割を自分で評価し直します**（[`evaluateLowered()` の呼び出し](../../src/core/ecs/predefined/animationsystem.cpp#L376)）。2つの state が「同じ入力で1フレームに1回ずつ進む」ので、姿勢側と描画側は互いのクロックを進めないまま同じ数値を読みます（[コメントの規範](../../src/core/ecs/predefined/animationsystem.cpp#L363)）。

描画側の評価結果は、描いたその場で [`publishGraphValues()`](../../src/core/loader/scenegraphruntime.cpp#L144) がスナップショットとして置いていきます。読み手が自分でもう一度評価すると**クロックが進みイベントが消費される**ので、値を持ち出せるのはこの1点だけです。詳細は[第3章 §3.5](03_project_and_loading.md)。

### `freeze_actions` とImGuiゲートの後始末

`freeze_actions` でImGuiへ入力をルーティングする前に、[`resolveFrameStateModules()`](../../src/core/appflow/framephase.cpp#L80) がゲートの開閉を判定します。ゲートの実体は [`isImGuiRuntimeEnabled(config)`](../../src/core/imgui/imguiruntime.cpp#L9) で、`EngineLaunchConfig` の `headless` / `rpc` / `input_replay` / `golden_mode` / `xr_active` を読むだけの述語です（この関数自身は何も書き換えません）。ここでの「開いている」はこの述語が真で、そのフレームの `FrameStateModules.imgui_system` に [`GET_MODULE(ImGuiSystem)` が入る](../../src/core/appflow/framephase.cpp#L98)状態、「閉じている」は偽で `imgui_system` が nullptr のまま（= `freeze_actions` の [`routeInputAndBeginFrame()`](../../src/core/appflow/framephase.cpp#L203) が呼ばれず、そのフレームで ImGui フレームが始まらない）状態を指します。開閉が変わるのは、読んでいる側のlaunch configが変わったときです（例: XRのVulkan bootstrapが失敗してflatへ落ちる [`core.cpp` 内](../../src/core/vkcore/core.cpp#L626)）。ImGuiゲートが閉じたフレームでは、開始済みのImGuiフレームを [`ImGuiSystem::endFrameIfStarted()`](../../src/core/imgui/imguisystem.hpp#L31) で畳みます（[`framephase.cpp` 内](../../src/core/appflow/framephase.cpp#L83)）。守っている対応関係は「`freeze_actions` の `routeInputAndBeginFrame()` が立てた `frame_started` は、そのフレームの `ImGuiSystem::render()` が必ず倒す」で、ImGuiパスを持たないグラフへ持ち越すと `render()` が呼ばれず、立ったままのフレームへ次の `NewFrame()` が重なります。

> **設計決定:** ゲートは論理フレーム境界で閉じ得るため、「開始済みのImGuiフレームを、ImGui passを持たないグラフへ持ち越さない」ことを明示的に保証しています。ソース中のコメントがそのまま契約です。

> 🧩 **難所 — 名前は「解決」、中身は副作用**([`resolveFrameStateModules()`](../../src/core/appflow/framephase.cpp#L80) / [`prepareFrameStateModules()`](../../src/core/appflow/framephase.cpp#L125))
>
> **何をする所か**: `updateFrameState()` の冒頭で、そのフレームが触る module 参照を1つの構造体へ集めます。名前どおりの解決に加えて、直前の節で見たImGuiゲートの後始末という**状態変更**もここで起きます。
>
> **素朴に読むと**: 「参照を集めるだけの純粋な関数」に見えるので、毎フレーム呼ぶのが無駄に見えます。実際には毎フレーム通ることに意味があります。(1) ImGuiゲートが閉じたフレームでは [`endFrameIfStarted()`](../../src/core/imgui/imguisystem.hpp#L31) をここで呼びます — ゲートは論理フレーム境界で閉じ得るので、判定と後始末を同じ場所へ置かないと開始済みImGuiフレームが持ち越されます。(2) `ui_module` と `render_target` は**どちらか一方が無ければ両方 nullptr にします**([その判定](../../src/core/appflow/framephase.cpp#L60))。`freeze_actions` の [`routeFrameInput(input_state, render_target->getExtent())`](../../src/core/appflow/framephase.cpp#L199) が `ui_module != nullptr` の分岐の中で `render_target` を**無条件に**参照するからで、片方だけの nullptr を許すとここで落ちます。(3) `tryGet` と `GET_MODULE` の使い分けも意図的です。`GET_MODULE` 側は「必ず存在する前提」で、凍結後に未生成のmoduleを `GET_MODULE` すると例外になります。だから `prepareFrameStateModules()` が凍結前に同じ解決を一度だけ走らせて実体化しておきます([`prepareRuntimeModuleGraph()`](../../src/core/appflow/loop.cpp#L259) から呼ばれる)。
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
> **手がかり**: [`prepareFrameStateModules()`](../../src/core/appflow/framephase.cpp#L125) の中身は `(void)resolveFrameStateModules();` の一行で、**戻り値ではなく副作用が目的**であることがそのまま現れています。hookの位置(reload公開の後、`freeze_events` の直前)を規範として書いているのは [`framephase.hpp` 内](../../src/core/appflow/framephase.hpp#L35) のコメントです。[`installEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L129) は null と二重登録を弾き、[`removeEditorCommitQueueHook()`](../../src/core/appflow/framephase.cpp#L140) は**登録時と同じ context** でなければ何もしません(他人のhookを外せない)。テストは [`Windowed and RPC frames use the same five-phase executor`](../../test/framephase_test.cpp#L9)(windowedとRPCが同じ5フェーズ)と [`Editor commit queue hook is optional and has one explicit boundary`](../../test/framephase_test.cpp#L36)(hookは任意で境界は1つ)。
>
> **不変条件**: `updateFrameState()` は毎フレーム `resolveFrameStateModules()` を通る(ゲート後始末を飛ばさない)。`ui_module` / `render_target` は対で有効・対で無効。フレーム実行中にmoduleを新規生成しない。hookはsingle-ownerで、外せるのは登録した本人だけ。

> 🧩 **難所 — capture は黙って消える**([`InputRouter::route()`](../../src/core/ui/inputrouter.cpp#L45))
>
> **何をする所か**: `freeze_actions` が呼ぶ [`UiModule::routeFrameInput()`](../../src/core/ui/module.cpp#L281) の中身([`framephase.cpp` 内](../../src/core/appflow/framephase.cpp#L190))で、そのフレームの生ポインタ列を widget へ割り当て、`Capture` / `Click` / `DragStart` などのeffect列へ変換します。
>
> **素朴に読むと**: Down/Move/Up の switch だけ見ると「Move は今カーソルの下にある widget へ配られる」と読めます。実際は逆で、**Down で捕まえた widget が Up まで全ての Move を受け取ります** — Move の `routed.target` は hit 判定の結果ではなく `state.capture->owner` です([Move 側の代入](../../src/core/ui/inputrouter.cpp#L67))。物理判定は `hover_target` 側だけを更新するので、**hover と target がずれているのが正常状態**です。この前提が崩れる唯一の経路が switch より**前**の2行([消えた capture の掃除](../../src/core/ui/inputrouter.cpp#L58))で、capture 先の widget が arena から消えていると `state.capture` を無言で `reset()` します。ここだけ `ReleaseCapture` も `Cancel` も出ません。明示通知する [`cancelCapture()`](../../src/core/ui/inputrouter.cpp#L144) は `{Cancel, ReleaseCapture}` を返しますが、現在エンジン内に呼び出し元がありません(定義だけがある状態)。したがって **widget を消す側のコードが「ドラッグ中に消すと DragEnd 相当の通知が来ない」ことを知っている必要があります**。消えたと判定されるのは [`WidgetArena::erase()`](../../src/core/ui/widgetarena.cpp#L20) / `clear()` が slot の generation(世代番号 — 同じ index を再利用しても古い `WidgetId` を別物と判別するための連番)を進め、[`resolve()`](../../src/core/ui/widgetarena.cpp#L33) が `nullptr` を返すようになるからです。
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
> **手がかり**: `drag_started` はラッチ(一度立つと Up まで倒れないフラグ)なので、閾値を越えた後に押した位置へ指を戻しても Click にはなりません — Up 側の `!state.capture->drag_started && physical_target == state.capture->owner`([Click 発行の条件](../../src/core/ui/inputrouter.cpp#L94))がその裏返しです。閾値 `dx + dy >= 4`([DragStart の閾値](../../src/core/ui/inputrouter.cpp#L74))は**UI単位**のマンハッタン距離(各軸の差の絶対値の和。斜め移動は直線距離より早く閾値へ届きます)で、[`windowToUi()`](../../src/core/ui/types.cpp#L96) が framebuffer 座標を `ui_scale` で割った後の値です — 物理ピクセルではないので、`ui_scale` が大きいほど実際の移動量は長く必要になります。capture が黙って消えた後の Up は `else routed.target = physical_target`([capture 無しの Up](../../src/core/ui/inputrouter.cpp#L97))へ落ちるため、ボタン不一致の検査([その throw](../../src/core/ui/inputrouter.cpp#L91))も素通りします。`routed.target` か capture があれば `consumed_pointer` が立ち、`UiModule` が `consumePointerForActions()` を呼ぶ([`module.cpp` 内](../../src/core/ui/module.cpp#L288))ので、capture 中はAction層からポインタが隠れます。テストは [`Ordered router keeps per-pointer captures independent`](../../test/ui_foundation_test.cpp#L246)(pointer_idごとにcaptureが独立)と [同ファイルの U2 ボタン検証](../../test/ui_foundation_test.cpp#L305)(Upが `ReleaseCapture` → `Click` の順)。
>
> **不変条件**: Down から Up までの Move は capture owner へ届く(hover は別軸で更新される)。`event_seq` はフレーム内で狭義単調増加。`drag_started` は Up まで倒れず、Click と排他。capture 中の widget 削除だけは effect を出さないので、終了処理は削除する側が持つ。

### eventの1フレーム遅延

`GameContext::emit()`はeventを`pending_events`へ積みます（[`emit()`](../../src/core/userpublic/details/event/registerer.hpp#L257)）。次のフレーム冒頭で[`freezePendingEventsForFrame()`](../../src/core/userpublic/details/event/registerer.cpp#L495)が`deliver_now_events`へswapし、配送します（メンバ実装は[`UserEventRegistererTemplatePublic::freezePendingEventsForFrame()`](../../src/core/userpublic/details/event/registerer.cpp#L495)）。

- update中にemit → 次フレームで配送
- event handler内で別eventをemit → さらに次フレームで配送
- scene load完了時の`SceneLoaded` → loadがフレーム末尾なら次フレームで配送

この性質により、配送中vectorの再確保や再入dispatchを避けています。テストは [`eventlayer_test.cpp`](../../test/eventlayer_test.cpp#L48) です。

### inputの1フレーム固定

[`InputStateCore::beginFrame()`](../../src/core/os/inputstate.cpp#L417) がpending eventをframe eventへswapし、down/pushed/released/mouse deltaを作ります。Action層は [`freezeInputActionsFrame()`](../../src/core/userpublic/userinput.cpp#L454) で消費maskを適用したsnapshotを評価します。以後の`Actions::*`は再計算せず同じ`InputActionFrame`を返します。

## 2.6 EngineTime

[`EngineTime`](../../src/core/appflow/enginetime.hpp#L31) には二モードあります。

- `realtime`: `steady_clock`差分。異常に長い停止は0.1秒へclamp（[`advance()`](../../src/core/appflow/enginetime.cpp#L36)）。
- `fixed_step`: `1 / launch_config.fps`を毎回加算。headless/RPC/replayの再現性に使う。

`advance()`後に`current_time += delta_time`、`frame_index++`です。最初の更新フレームはindex 1になります。

**評価の番号と play state(✅PM-1a)。**[`advance()`](../../src/core/appflow/enginetime.cpp#L36) は描画のフレームごとに必ず `evaluation_index` を 1 進めます(戻すのは `setup()` だけ)。animation の書き込み・graph・phase の revision、VMA の frame index、GPU timing、TAA、shader の `frame_index`、graph の publish、picking、RenderDoc、XR の mirror、RPC の `frame`、そして `GameContext::frameIndex()` が読むのはこちらで、[`evaluationIndex()`](../../src/core/appflow/enginetime.hpp#L93) で取ります。上の `current_time` / `delta_time` / `frame_index` は game の時計と game のフレームで、`PlayState::playing` の間は毎フレーム、`paused` の間は [`requestSteps()`](../../src/core/appflow/enginetime.cpp#L134) で頼まれた歩を 1 フレームに 1 つ取るときだけ進みます(そのときの `dt` は固定の `1/fps`)。それ以外の `paused` のフレームは `delta_time = 0` で、止まった時刻のまま評価し直されます。そのフレーム自身の差分(realtime の測定値か fixed_step の固定値)は `editor_delta_time` に別に取り、[`editorDt()`](../../src/core/appflow/enginetime.hpp#L94) で editor の system が読みます。[`setPlayState()`](../../src/core/appflow/enginetime.cpp#L90) は `paused` から出るときに取っていない歩を捨て、`last_tick` を今に合わせ直すので、再開の最初のフレームの `dt` に一時停止の時間は入りません。一時停止しない実行では、評価の番号と game のフレームは同じ値で進みます。RPCの`set_time`は時刻だけを直接変更し、deltaを0へ戻します。この不連続は [`timeSetRevision()`](../../src/core/appflow/enginetime.cpp#L78) で観測でき、rendererはこれとcamera不連続をまとめて検知してtemporal history（前フレームのview/projection行列やcamera位置をview単位で覚えておく履歴。前フレームの描画結果を今フレームへ再投影して混ぜる処理が使うので、時刻やcameraが飛ぶと対応関係が崩れて捨てる必要があります）をリセットします（[`renderer.cpp` 内](../../src/core/vkcore/renderer.cpp#L1599)）。

**edit とセッション(✅PM-1b)。**`PlayState` に 3 つ目の `edit` が入りました。時計の進み方は `paused` と同じ(`dt = 0`、評価の番号だけ進む)で、`requestSteps()` は `paused` 以外で投げるので `edit` では歩を取りません。`setPlayState()` は `paused` か `edit` から出るときに歩を捨て `last_tick` を合わせ直します。`edit` と他の 2 つの間には「セッション」があり、[`beginGameSession()`](../../src/core/appflow/enginetime.cpp#L104) は今の時刻を覚えて game の時計と game のフレームを 0 に、[`endGameSession()`](../../src/core/appflow/enginetime.cpp#L112) は覚えた時刻(セッションが無ければ 0)に時計を戻します。**game のフレームは終わるときに戻しません**(`edit` では進まないので、前のセッションの最後の値が次のセッションの始めまで残ります)。状態の移りが途中で失敗したときのために、`gameClock()` / `restoreGameClock()` で時刻・フレーム・セッションの有無をまとめて取って戻せます。

## 2.7 ゲームSystemと内部ECS Systemは別の更新列

名前は似ていますが、二種類あります。

| 種類 | 登録 | 呼び出し | 主用途 |
|---|---|---|---|
| 内部ECS System | [`ECSCore::registerSystem`](../../src/core/ecs/core.hpp#L40) | `ECSCore::update()` | Component配列をまとめて処理 |
| ゲームSystem | [`PELICAN_REGISTER_SYSTEM`](../../src/core/userpublic/details/system/registerer.hpp#L203) | `updateRegisteredGameSystems()` | `GameContext`経由のゲームロジック |

内部ECSが先に走ります。そのため、ゲームSystemが同フレーム中に`LocalTransform`を書き換えた結果を内部transform Systemが見るのは原則次フレームです。ただし [`GameObjects::setLocalTransform()`](../../src/core/userpublic/gameobjects.cpp#L67) は公開componentと内部`TransformComponent`を即時同期し、model instanceへも直接TRSを反映するため、公開API経由の通常操作では表示遅延を避けています。

## 2.8 描画フレーム

状態更新後に [`Renderer::render()`](../../src/core/vkcore/renderer.cpp#L7093) が呼ばれます（windowedでは [`renderFlatFrameWithOptionalCapture()`](../../src/core/appflow/loop.cpp#L329) 経由）。WP128以降、`render()`は1-viewのアダプタで、実体は [`Renderer::renderLogicalFrame()`](../../src/core/vkcore/renderer.cpp#L5444) です。

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
- shader hot reload失敗だけは旧shader/pipelineを維持してwarningにします（[`ShaderLibrary::prepareReload()`](../../src/core/shader/shaderlibrary.cpp#L1157)、[`PipelineFactory::rebuildPrepared()`](../../src/core/shader/pipelinefactory.cpp#L1911)）。
- teardownは例外を外へ出しません。

この違いは「初回構築に失敗した不完全なruntimeは続けないが、稼働中の編集失敗では最後の正常版を守る」という方針です。

## 2.10 編集RPCホストとpreviewの実行面 🚧部分実装

エディタ関連の実行面は、ここまでに出た三つの点で構成されます。RPCのメソッド一覧そのものは[第7章](07_tools_rpc_tests.md)、データ側の受理仕様は[第3章](03_project_and_loading.md)を参照してください。

### RPCディスパッチャは1個

[`EngineRpcEndpoint`](../../src/core/communication/rpcserver.hpp#L148) が「状態を持つエンジンRPCディスパッチャ」を1個所有します。ヘッダのコメントが契約です — headlessは [`run()`](../../src/core/communication/rpcserver.cpp#L2853) がEOFまでブロックし、windowedのホストは [`processLine()`](../../src/core/communication/rpcserver.hpp#L101) を**フレーム境界でだけ**呼びます。`runEngineRpcServer()` はheadless用の薄いラッパです。

### 編集セッションの生成点も1個

編集面の生成は [`makeEditorRuntimeService()`](../../src/core/communication/editorruntimefactory.hpp#L144) だけです。ヘッダのコメントが規範で、要点は次の通りです。

- RPC endpointか、interactiveなImGui runtimeの**どちらか一方**が使う、唯一のproduction composition。
- 決定的ドライバ（headless固定フレームやgolden test）はinteractive runtimeを作らないため、**編集面はそもそも存在しません**。

> **設計決定:** 「編集できるかどうか」をフラグで切り替えるのではなく、生成点を1つに絞ったうえで決定的経路がそこを通らない、という形にしています。決定性を守る側にif文が増えません。

### preview graphは起動時にコンパイルされる第3のグラフ

flat / xr の `RenderingPassId` に対して、preview は**データだけのグラフプログラム**です（[`PreviewGraphProgram`](../../src/core/renderingpass/previewgraph.hpp#L21)）。

- コンパイルは起動時、runtime moduleが凍結される前です。[`loadRenderGraphVariantsFromConfig()`](../../src/core/vkcore/renderer_config.cpp#L589) が [`precompilePreviewGraph()`](../../src/core/renderingpass/previewgraph.hpp#L66) を呼び、結果を `Renderer` の [`preview_graph_program`](../../src/core/vkcore/renderer.hpp#L204) が保持します。
- 共有のrender target / pass登録は**意図的に行いません**。ヘッダのコメント通り、`render_preview` がリクエストローカルな資源に対して実行するため、`Renderer::renderLogicalFrame()` には入りません。
- 実行と隔離キャプチャは [`PreviewExecutor`](../../src/core/vkcore/previewexecutor.hpp#L61) が担当します。
- ✅PV-1(`a173e70c`)から、preview を組めなくても起動は失敗しません。`preview_graph_program` は `available: false` と理由を持つ空の program になり、flat / xr は通常どおり公開されます。起動の log に warning が 1 行出て、`render_preview` は `preview.unavailable@1` で断ります(第6章「graph variant: flat、`#xr`、preview」の PV-1 の段落)。

つまりpreviewは「フレームループの外側で、同じ宣言から作った別プログラムを動かす」構造で、通常フレームの決定性やtemporal historyへは触れません。

## 2.11 game logic DLLの入口・実行前診断・ホットリロード ✅実装済み

§2.1 の 9番（起動）と §2.4 のF5、RPCの `reload_game_logic`（[第7章](07_tools_rpc_tests.md)）が入る先です。DLLの中身の話は §5.2 / §5.13、ここでは**プロセス寿命の側**だけを見ます。

### 入口は「epoch anchor」と「v2 / v3 の登録関数」

SDKのepochは単一定義から導かれます。[`gameLogicAbiVersion`](../../src/core/userpublic/gamelogic.hpp#L21) が `2` で、そこから anchor のシンボル名 `pelican_game_logic_epoch_v2` を作るのが [`PELICAN_GAME_LOGIC_EPOCH_ANCHOR`](../../src/core/userpublic/gamelogic.hpp#L11) と [`gameLogicEpochAnchorSymbol`](../../src/core/userpublic/gamelogic.hpp#L23) です。宣言は `extern "C"` の [1関数](../../src/core/userpublic/gamelogic.hpp#L28) で、実体はhost（player実行ファイル）側にあります。

game DLL側でこれを**必ず**importさせるのが [`gamelogic_entry.cpp`](../../src/player/gamelogic_entry.cpp#L17) です。exportされた非inline関数 `pelican_game_logic_epoch_nonce()` の中でanchorを呼ぶので、登録を1つも持たないDLLでも通常・直接・名前付きのimportが残ります。コメントが理由をそのまま書いています — OS loaderはDLLのinitializerを走らせる**前**にこのimportを解決します。

登録の入口はepochとは別軸で、schemaが再帰的かどうかでコンパイル時に選ばれます。

| 種類 | v2入口 | v3入口 | 選択 |
|---|---|---|---|
| behavior | [`pelican_register_behavior_v2`](../../src/core/userpublic/details/behavior/registerer.hpp#L243) | [`pelican_register_behavior_v3`](../../src/core/userpublic/details/behavior/registerer.hpp#L245) | [`schemaRequiresV3(Params::schema)`](../../src/core/userpublic/details/behavior/registerer.hpp#L322) |
| event | [`pelican_register_event_v2`](../../src/core/userpublic/details/event/registerer.hpp#L116) | [`pelican_register_event_v3`](../../src/core/userpublic/details/event/registerer.hpp#L118) | [`recursive_descriptor`](../../src/core/userpublic/details/event/registerer.hpp#L151) |
| component | [`__registerComponent`](../../src/core/userpublic/details/component/registerer.hpp#L110)（WP381 の改名対象外。旧名のまま凍結） | [`pelican_register_component_v3`](../../src/core/userpublic/details/component/registerer.hpp#L112) | [`has_recursive_schema`](../../src/core/userpublic/details/component/registerer.hpp#L186) |
| game system | [`pelican_register_system_v2`](../../src/core/userpublic/details/system/registerer.hpp#L96) | — | 常にv2 |
| event emit | [`pelican_emit_v2`](../../src/core/userpublic/details/event/registerer.hpp#L119) | — | 常にv2 |

判定の実体は [`schemaRequiresV3()`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L823) で、`Flags` か `List` のフィールドが1つでもあれば真です（§3.11 の `SchemaNode`）。v3経路では平坦な `StructFieldSchema` 列ではなく [`materializeSchemaNode()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L972) の再帰記述子と、その [fingerprint](../../src/core/userpublic/details/schema/structfieldjson.hpp#L1040) を一緒に渡します。v2側のfingerprint関数には [`static_assert(!schemaRequiresV3(...))`](../../src/core/userpublic/details/behavior/registerer.hpp#L185) が置いてあり、再帰schemaがv2入口へ落ちるとコンパイルエラーです。

> **設計決定:** epoch anchorは `v2` のままで、schemaの入口だけが `v3` です。二つを別の軸にしてあるので、再帰schemaを使わないDLLは再ビルド無しで動き続けます。診断側もこの二軸をそのまま持ちます — [`diagnoseGameLogicImports()`](../../src/core/gamelogic/gamelogicimports.cpp#L559) の `expected_epoch` と `host_schema_entrance`（既定 `3`、[宣言](../../src/core/gamelogic/gamelogicimports.hpp#L69)）は別の引数です。

### ロード前のraw import診断（Windowsのみ）

[`loadCopy()`](../../src/core/gamelogic/gamelogicreload.cpp#L405) は `LoadLibraryW` を呼ぶ**前**に [`diagnoseGameLogicImports(path, currentExecutableName(), gameLogicAbiVersion)`](../../src/core/gamelogic/gamelogicreload.cpp#L408) を通します。読むのは [`readGameLogicImports()`](../../src/core/gamelogic/gamelogicimports.cpp#L516) で、imageをmapせず、importしたモジュールも辿らず、PEのimport directory（index 1）とdelay-import directory（index 13）だけを `std::ifstream` で読みます。

作業量の上限はPE形式の制限ではなく**パーサの自衛**です（[先頭の3定数](../../src/core/gamelogic/gamelogicimports.cpp#L24)。4 つ目の `maxImportStringBytes` だけは [`readCString()`](../../src/core/gamelogic/gamelogicimports.cpp#L77) の中のローカル定数です）。

```text
maxPeFileBytes       = 256 MiB  # 超えたら unreadable_reason = "too_large"
maxImportDescriptors = 4096     # descriptor 数と directory.size / 20(delay は 32) の両方に効く
maxImportThunks      = 65536    # 1 モジュールあたりの thunk 走査上限
maxImportStringBytes = 4096     # readCString() の 1 文字列上限
```

読めない場合は `game_logic_abi_unreadable@1` に `reason`（`not_pe` / `truncated_pe_header` / `invalid_pe_signature` / `too_large` / `reader_exception` など）が入ります。読めた場合の判定は、**hostモジュール名（ASCII小文字化して比較）宛てのimportだけ**を対象に次の順です。

1. delay import（`delay_named` / `delay_ordinal`）→ `game_logic_abi_unsupported_import@1`、`kind: "delay_load"`（[その分岐](../../src/core/gamelogic/gamelogicimports.cpp#L577)）。delay importはOS loaderが起動時に解決しないので、「anchorをimportしていればloaderが弾く」という前提が崩れます。
2. 序数import → 同じerror codeで `kind: "ordinal"`（[その分岐](../../src/core/gamelogic/gamelogicimports.cpp#L585)）。名前が無ければanchorかどうかを判定できません。
3. `pelican_register_behavior_v3` / `pelican_register_event_v3` / `pelican_register_component_v3` のいずれかを含むなら、そのDLLのschema entranceを `3` と見なします（[その検出](../../src/core/gamelogic/gamelogicimports.cpp#L598)）。`imported_schema_entrance > host_schema_entrance` なら `game_logic_abi_stale@1` に `{found: "v3", host: "v2"}` を載せます — 新しいDLLを古いhostへ差した側の検出です。
4. anchorのepochは [`importedEpoch()`](../../src/core/gamelogic/gamelogicimports.cpp#L486) が `pelican_game_logic_epoch_v<N>` の `<N>` を `from_chars` で読みます。期待epochが1つも見つからない、または旧v1名（[`legacyEntrance()`](../../src/core/gamelogic/gamelogicimports.cpp#L473) の `__registerBehavior` / `__registerEvent` / `__emit` / `__registerSystem`）が残っていれば `game_logic_abi_stale@1` です。`found` はepochが空なら文字列 `"absent"`、あれば `"v<最小>"` で、旧名は `legacy_symbols` に並びます。

診断が落ちた場合は `LoadLibraryW` へ進まないので、**旧DLLのconstructorは走りません**。Linux側にこのreaderは無く、`RTLD_NOW` が解決に失敗しないDLLは止まりません（[第1章](01_architecture_and_build.md) の同じ話）。

### shadow copyはpidごとのディレクトリ

[`initialize()`](../../src/core/gamelogic/gamelogicreload.cpp#L497) が決めるshadow directoryは `<DLLの親ディレクトリ>/.pelican-hot-reload/<pid>` です（[その組み立て](../../src/core/gamelogic/gamelogicreload.cpp#L500)）。pidを挟むので、同じDLLを複数プロセスが同時に開いてもコピー名が衝突しません。

directoryを決めた直後に [`sweepStaleShadowDirectories()`](../../src/core/gamelogic/gamelogicreload.cpp#L335) が親を1階層だけ走査し、**名前が10進数として完全にparseでき、かつそのpidが生きていない**ディレクトリを `remove_all` します（[判定](../../src/core/gamelogic/gamelogicreload.cpp#L348)）。数字以外の名前や生存プロセスのdirectoryには触りません。親directoryがまだ無い場合は「`makeShadowCopy()` が遅延生成する」というコメント付きで黙って戻ります。

コピー名は [`makeShadowCopy(purpose)`](../../src/core/gamelogic/gamelogicreload.cpp#L373) が `<stem>_<purpose>_<copy_sequence><拡張子>` で作ります。`purpose` は `"initial"` か `"candidate"`、`copy_sequence` は単調増加なので、現行DLLと候補DLLが必ず別パスになります（同じパスならOSは同じ `HMODULE` を返し、2世代を同時に持てません。[第10章](10_background_knowledge.md)）。`.pdb` も追随しますが、そちらの失敗はwarningどまりです。

正常終了では [`shutdown()`](../../src/core/gamelogic/gamelogicreload.cpp#L843) がロード済みコピーを消したうえで、**自分のpid directoryを丸ごと**削除します。

### 自動pollは「2回一致 → read lease → busyなら保留」

[`pollAttempt()`](../../src/core/gamelogic/gamelogicreload.cpp#L742) は `force` でなければ次を順に見ます。

```text
指紋 = (last_write_time, file_size)
指紋 == observed_source            -> 何もしない(settling を捨てる)
settling が無い / 指紋が違う        -> settling = 指紋 で return   # 1 回目の観測
(Windows) read lease を取れない:
    writer が居る -> pending にして source_busy_retries++
                     3 回目で game_logic_source_busy@1 を last_error へ
    それ以外      -> game_logic_unavailable@1(readiness_open_failed)
lease 保持中に取り直した指紋が違う  -> settling を更新して return
ここまで通れば observed_source を更新して reloadTransaction()
```

2回続けて同じ指紋を見るのは、リンカがtruncate→refillしている最中の部分的なDLLを診断へ渡さないためです。それでも足りないので、コピーを取る間だけwriterを排除するread leaseを重ねます（[そのコメント](../../src/core/gamelogic/gamelogicreload.cpp#L774) が規範）。busyは**失敗ではなく保留**で、[`sourceBusyRetryLimit = 3`](../../src/core/gamelogic/gamelogicreload.cpp#L236) 回連続して初めて `game_logic_source_busy@1`（`path` と `retries` 付き）を公開し、writerが閉じれば次のpollで [`clearSourceBusy()`](../../src/core/gamelogic/gamelogicreload.cpp#L327) がその error code だけを消します。状態は [`GameLogicReloadStatus`](../../src/core/gamelogic/gamelogicreload.hpp#L17) の `pending` / `source_busy_retries` から読めます。

pollを駆動するのはreload serviceのruntime participant `game_logic` で（[登録箇所](../../src/core/watch/reloadservice.cpp#L777)）、境界は `frame_start` です。**headless / RPC では `poll` triggerが無視され**（[その早期return](../../src/core/watch/reloadservice.cpp#L788)）、明示コマンドだけが効きます。windowedのF5は「要求」を出すだけで、実際の適用はフレーム境界です（[F5の呼び出し](../../src/core/appflow/loop.cpp#L713)）。

### 起動時の失敗はRPC `status` まで運ばれる

`initializeConfiguredGameLogic()` が `false` を返しても `run()` は続きます（§2.1 の 9番）。失敗の内容は `GameLogicReloader` に保存され、[`configuredGameLogicStatus()`](../../src/core/gamelogic/gamelogicreload.cpp#L951) から読めます。RPC `status` の `participants.game_logic`（[組み立て箇所](../../src/core/communication/rpcserver.cpp#L1782)）を作るのは [`gameLogicParticipantRpcStatus()`](../../src/core/communication/rpcserver.cpp#L1288) で、規則はコメントが規範です。

- reload serviceのruntime participant行があり、その `attempted` が0でない（= 一度は走った）なら、その行をそのまま返す。
- participantが**まだ一度も試されていない**（`attempted == 0`）場合に限り、保存済みの起動時失敗（`status.error_code`）を優先する。
- どちらも無ければ `{error_code: null, error_fields: {}}`。

つまり「起動で失敗し、その後まだ誰もreloadしていない」状態でも、エディタ側は最初の失敗理由を読めます。逆に一度でもreloadを試したら、そちらの結果が正になります。
