# OpenXR 設計 v1 敵対レビュー

レビュー日: 2026-07-17  
対象: `docs/design_openxr.md` v1  
照合 commit: `8884788418c8`  
方法: 設計・実装・OpenXR 1.1.61 仕様の静的照合。依頼どおりビルドと実機実行は行っていない。

## 冒頭判定

**Reject**。

PCVR を先に成立させ、multiview を後続へ送る切り方自体は妥当である。しかし v1 の
XR2a は「現行フレームグラフを view ごとに 2 回実行すれば最小実装になる」
(`docs/design_openxr.md:65-85`)という中心仮説が現行 renderer と両立しない。
`Renderer::render()` は単なる graph executor ではなく、一回の呼出しを一 logical frame とみなし、
Frame UBO 更新、history flip、object/animation temporal advance、snapshot commit、deferred deletion の
frame advance、frame target の acquire/submit/present を一括して行う
(`src/core/vkcore/renderer.cpp:927-991`)。二回呼べば左右眼ではなく二フレーム進む。

さらに Frame UBO は一個の host-written buffer と一個の descriptor set しか持たない
(`src/core/renderer/frameresources.hpp:42-59`,
`src/core/renderer/frameresources.cpp:26-44`,
`src/core/renderer/frameresources.cpp:82-88`)。左右の command を GPU 実行前に記録すると、
後から書いた眼の行列で同じ buffer が上書きされ、両眼が同じ値を読む可能性がある。
CPU 側に view 別 `RenderFrameSnapshot` を二個作るだけでは解決しない。

その前段にも blocker がある。現行 Vulkan 初期化は OpenXR より先に独自の heuristic で physical
device を選び、独自に instance/device を作る
(`src/core/vkcore/core.cpp:16-52`, `src/core/vkcore/core.cpp:128-175`,
`src/core/vkcore/core.cpp:177-220`, `src/core/vkcore/core.cpp:242-261`)。OpenXR の Vulkan session は
runtime が指定した physical device と必要 extension を使わなければならない。XR1 の記述にはこの
bootstrap 改造がなく、複数 GPU 機では XR2 より前に session 作成が失敗し得る。

以下の Blocker を設計へ戻し、§12 の WP 分割と逐語条件で再提出するまでは実装着手不可とする。

## 仕様照合元

- OpenXR の session は `xrBeginSession` 成功後から `xrEndSession` まで running であり、running 中は
  state が FOCUSED でなくても `xrWaitFrame` / `xrBeginFrame` / `xrEndFrame` を継続する。
  `shouldRender == XR_FALSE` でも begin/end を行い、layer を省略する
  ([OpenXR 1.1 Session Lifecycle](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#session-lifecycle),
  [Frame Synchronization](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#frame-synchronization))。
- Vulkan graphics binding の physical device は runtime が返す device と一致し、必要な instance/device
  extension を有効化しなければならない
  ([XrGraphicsBindingVulkanKHR](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrGraphicsBindingVulkanKHR.html),
  [XR_KHR_vulkan_enable2](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XR_KHR_vulkan_enable2.html))。
- swapchain image は acquire だけで書込み可能にならず、各 acquire に wait が必要である。2D array
  swapchain では一個の acquired image index が array 全体を指し、一 logical frame で一 swapchain
  につき最後に release した一 image index が使われる
  ([xrAcquireSwapchainImage](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrAcquireSwapchainImage.html),
  [OpenXR swapchain release semantics](https://registry.khronos.org/OpenXR/specs/1.1-khr/html/xrspec.html#swapchain-image-release))。
- projection layer は対応 view をすべて一回の `xrEndFrame` に渡す必要がある
  ([OpenXR Frame Submission](https://registry.khronos.org/OpenXR/specs/1.1/html/xrspec.html#frame-submission))。

## 1. [Blocker] XR1 より前に Vulkan bootstrap を OpenXR-aware にしないと session を作れない

設計は XR1 を `instance → system → session → reference space` とする
(`docs/design_openxr.md:36-54`)が、Vulkan graphics binding の確立順を一行も定めていない。
現行実装は window が要求する Vulkan instance extension だけを集め
(`src/core/vkcore/core.cpp:16-50`)、surface 対応などの独自 score で GPU を選ぶ
(`src/core/vkcore/core.cpp:128-175`)。device extension も windowed では
`VK_KHR_swapchain` だけである (`src/core/vkcore/core.cpp:177-205`)。

Quest Link runtime が dGPU を指定し、現行 heuristic が iGPU を選んだ場合、既に作った device と全 GPU
resource を session 作成後に差し替えることはできない。従って「runtime 不在ならフラット降格」と
「OpenXR-compatible Vulkan device の選択」は、`VulkanManageCore` 構築前に決着させなければならない。

最小の正しい形は次である。

1. explicit/auto XR activation を Vulkan 初期化前に評価する。
2. XR を試す場合は XrInstance/XrSystem を先に作り `XR_KHR_vulkan_enable2` の graphics requirements を得る。
3. window surface extension と engine 必須 extension を merge した create info を
   `xrCreateVulkanInstanceKHR` / `xrCreateVulkanDeviceKHR` に渡し、
   `xrGetVulkanGraphicsDevice2KHR` が返す physical device を採用する。
4. runtime 不在・system 不在・graphics requirements 不適合なら、まだ GPU resource がない時点で
   flat bootstrap へ一度だけ降格する。session 開始後の別 GPU への hot switch は v1 では行わない。

この改造は XR1 の依存ではなく XR1 の一部である。XR1 に Vulkan bootstrap が無い現在の WP 表
(`docs/design_openxr.md:130-138`)は成立しない。

## 2. [Blocker] XR2a の「既存 frame を二回」は temporal state と Frame UBO を破壊する

設計が懸念している history flip は実際に壊れる。`Renderer::render()` 一回ごとに次が起きる。

- deferred deletion の logical frame が進む (`src/core/vkcore/renderer.cpp:927-929`,
  `src/core/vkcore/deletionqueue.cpp:56-63`)
- camera と projection を module から一回取得し、一個の Frame UBO を上書きする
  (`src/core/vkcore/renderer.cpp:956-967`)
- history RT の面を flip する (`src/core/vkcore/renderer.cpp:988`,
  `src/core/renderingpass/rendertargetcontainer.cpp:212-219`)
- previous model/skin/morph を current へ advance する
  (`src/core/vkcore/renderer.cpp:989`,
  `src/core/renderer/polygoninstancecontainer.cpp:381-398`)
- camera/projection snapshot history を現在眼へ commit する
  (`src/core/vkcore/renderer.cpp:990-991`,
  `src/core/renderer/projectionjitter.cpp:120-129`)

従って左眼の後に右眼を呼ぶと、右眼の `previous_view` は前フレーム右眼ではなく同フレーム左眼になる。
object velocity は左眼終了時に current=previous へ進み、右眼では消える。history RT は左右が同じ ping-pong
pair を交互に使う。二回目終了後は parity が元に戻るため、次 logical frame の「前面」も規約から外れる。

さらに singleton Frame UBO は左右眼を保持できない。`FrameResources::update()` は同じ buffer offset 0 を
毎回上書きし、bind は同じ descriptor set を使う
(`src/core/renderer/frameresources.cpp:38-44`, `src/core/renderer/frameresources.cpp:82-88`)。
二眼分の GPU work を同じ queue へ submit しても、CPU write の lifetime を view ごとに分けなければならない。

XR2a の最小形は「`Renderer::render()` を二回」ではなく、概念的に次である。

```text
renderLogicalFrame(frame_input, views[2]):
  begin logical frame once (reload/deletion/animation/reset/target acquire)
  freeze shared scene state once
  for each view:
    select per-in-flight × per-view FrameUBO slot
    build per-view current/previous snapshot
    execute the unchanged graph once into that view output
  submit/release/end logical frame once
  advance RT/object/morph/camera history once
```

frame graph の node 定義を view-aware にしないまま二回走らせることは可能だが、graph executor の外側を
logical-frame/view の二層に分割し、内部 transient RT を view 間で安全に再利用する必要がある。
Frame UBO は最低でも `in_flight_frames × view_count` の不変 slot と descriptor/dynamic offset を持たせる。

## 3. [Blocker] `IFrameTarget` の一回 acquire/present 契約は OpenXR frame を表せない

現行 interface は一個の context を `render_begin()` で返し、`render_end()` で一回 submit/present する
(`src/core/vkcore/frametarget.hpp:20-29`)。desktop 実装では begin が swapchain image acquire と command
recording startを行い (`src/core/vkcore/swapchainframetarget.cpp:218-285`)、end が submit/present と
in-flight index advance を行う (`src/core/vkcore/swapchainframetarget.cpp:336-374`)。これは明確に
「一 render call = 一 present」である。

OpenXR は一 logical frame につき wait/begin/end が一組であり、その end に左右 view を同時に載せる。
`shouldRender == false` の zero-layer frame も表現しなければならない。従って `XrFrameTarget` を同じ
interface の「第 3 実装」と呼ぶだけ (`docs/design_openxr.md:71-77`)では意味論が合わない。

XR2a で以下のどちらかを明示すること。

- `IFrameTarget` を `beginLogicalFrame` / `beginView(i)` / `endView(i)` /
  `endLogicalFrame` に拡張し、flat 実装を view_count=1 adapter にする。
- 既存 `IFrameTarget` を flat target のまま保持し、別の `IXrCompositionTarget` が二 view と
  OpenXR frame lifecycle を所有する。renderer は両者に共通な view context を受け取る。

MVP swapchain 形も一意化が必要である。最小で堅いのは view ごとに `arraySize=1` の color swapchain を
二個作る形であり、異なる recommended extent をそのまま扱える。一個の `arraySize=2` swapchain を使うなら、
一回だけ acquire/wait して取得 image の layer 0/1 view へ描き、一回だけ release すること、両 view の
extent が異なる場合の共通 image size と `imageRect` を規範化すること。現在の「per-view image array」
(`docs/design_openxr.md:73-74`)は両方式を区別できない。

なお desktop target は最終 layout を `PRESENT_SRC_KHR` にするが
(`src/core/vkcore/swapchainframetarget.cpp:322-332`)、OpenXR Vulkan swapchain image は release 時に
runtime 要求の color attachment compatible layout/queue ownershipへ戻す必要がある。desktop present の
layout 遷移を流用してはならない。

## 4. [Blocker] xrWaitFrame と EngineTime の二本立てを宣言しながら直後に同一視している

設計は fixed-step の決定性を維持して表示予測時刻と二本立てにするとした直後、v1 は
「シミュ時刻 = 表示時刻」で開始するとしている (`docs/design_openxr.md:44-54`)。両方は同時に真にならない。
現行 `EngineTime` は `now/dt/frame_index` 一組だけで
(`src/core/appflow/enginetime.hpp:20-37`)、`setTime()` は revision を進める
(`src/core/appflow/enginetime.cpp:44-49`)。predicted time を毎フレーム `setTime()` で入れると renderer は
毎フレーム temporal reset する (`src/core/vkcore/renderer.cpp:936-948`)。

正しい最小形は XR1 から clock を二つに分けることである。

- `EngineTime`: simulation の `now/dt/frame_index`。headless/replay/RPC の fixed-step 契約を一切変えない。
- `XrDisplayTiming`（frame-local immutable value）: `predictedDisplayTime`,
  `predictedDisplayPeriod`, `shouldRender`。`xrLocateViews` / pose locate と render snapshot が使う。
- interactive XR では `xrWaitFrame` の後に simulation を一回だけ advance/update する。
  predicted time を `EngineTime::setTime()` へ書かない。

現行 interactive loop は `engine_time.advance → update → render → FramerateAdjust::wait` の固定順である
(`src/core/appflow/loop.cpp:327-350`)。XR running 時は次へ分岐する必要がある。

```text
window events + xrPollEvent
if session_running:
  xrWaitFrame
  EngineTime.advance + updateFrameState exactly once
  xrBeginFrame
  if shouldRender: locate views at predictedDisplayTime + render two views
  xrEndFrame (zero layers when shouldRender=false)
  do not call FramerateAdjust.wait
else:
  current flat update/render/FramerateAdjust path unchanged
```

gate は `state >= SYNCHRONIZED` ではなく `xrBeginSession` 成功から `xrEndSession` までの
`session_running` bool である。FOCUSED は input eligibility であって描画 gate ではない。
READY で begin、STOPPING で frame loop を止めて end、LOSS_PENDING/EXITING は別 terminal path とする。
headless/RPC/golden/replay は XR activation を強制 off とし、現行 `step_frame`
(`src/core/communication/rpcserver.cpp:843-858`)を xrWaitFrame に接続しない。

## 5. [Blocker] view 別 snapshot に camera position と座標変換規約がない

`RenderFrameSnapshot` は view/projection は持つが camera position を持たない
(`src/core/renderer/projectionjitter.hpp:29-48`)。Frame UBO の camera position は snapshot ではなく
active `Camera` module から直接読む (`src/core/vkcore/renderer.cpp:184-211`)。設計どおり view/proj だけを
view 別にしても、specular 等が読む camera position は左右で同じ stage origin のままである。

座標規約も「原点からのオフセットとして合成」だけでは実装規範にならない
(`docs/design_openxr.md:78-82`)。Pelican の外部規約は右手系、+Y up、-Z forward、meter、quaternion xyzw
(`docs/external_tools_requirements.md:64-70`)で OpenXR と軸・単位は一致する。一方、現行 camera は
RH_ZO projection を使う (`src/core/renderer/camera.cpp:471-482`)が、scene camera の direction は
node rotation の +Z を使う (`src/core/renderer/camera.cpp:456-458`)。暗黙の符号コピーは禁止し、行列で
規範化すべきである。

XR2a へ次を明記すること。

```text
world_from_stage = inverse(active_camera_view_at_frame_start)
stage_from_eye[i] = rigid matrix from XrView.pose (xyzw, metres)
world_from_eye[i] = world_from_stage * stage_from_eye[i]
view[i]           = inverse(world_from_eye[i])
camera_position[i]= translation(world_from_eye[i])
projection[i]     = asymmetric RH_ZO matrix from XrFovf
```

Vulkan viewport の Y と asymmetric FOV の top/bottom 符号、near/far の出所も fixture で固定する。
active camera transform を「既存 eye camera」ではなく `world_from_stage` と再定義するため、flat camera API を
変えず XR adapter の中だけで上式を使う。

STAGE と LOCAL は同じ意味ではない。STAGE は floor-level origin を期待できるが、LOCAL fallback は initial
head 近傍で floor を保証しない。supported reference spaces を列挙し、STAGE、利用可能なら LOCAL_FLOOR、
最後に LOCAL の順とするか、LOCAL 時の明示 floor offset/calibration を定義すること。`get_status` には
選択 space と floor semantics を出す。LOCAL を無補正で STAGE と呼んではならない。

## 6. [Blocker] XR3 は既存 L0/L1 へ pose を配送できず、head も XrAction ではない

設計は head/aim/grip を pose action として「入力スナップショットへ」入れ、L2 以上を無改造とする
(`docs/design_openxr.md:103-111`)。しかし現行 `InputSnapshot` は key/mouse/gamepad だけ
(`src/core/os/inputstate.hpp:60-83`)、`InputEvent` に pose event はない
(`src/core/os/inputstate.hpp:85-119`)。pose action は evaluator が明示的に skip し
(`src/core/os/actionmap.cpp:826-857`)、公開 `Actions::pose()` は必ず未実装 error になる
(`src/core/os/actionmap.cpp:673-683`)。

`ActionPose` も position/orientation と一個の `valid` しかない
(`src/core/userpublic/userinput.hpp:98-107`)。OpenXR は orientation/position の valid/tracked を別 flag で返す。
tracking loss 時に last-known pose、invalid pose、tracked poseを区別できない。

さらに head pose は controller の XrAction ではない。head/view は `xrLocateViews` 又は VIEW space 由来の
engine synthetic pose である。aim/grip は XrAction + action space だが、左右 hand の subaction path を
一個の action 名へどう公開するかを決めなければならない。

XR3 の最小契約は次である。

- L0 の「全 backend が同じ event queue」という説明
  (`docs/design_input_actions.md:14-27`)を、ordered events と typed pose samples を持つ
  frame provider へ改訂する。kbd/mouse/gamepad の既存 serialization は維持する。
- `xrSyncActions` と pose locate を `freeze_input/freeze_actions` より前に一回行う。現行 action freeze は
  game update 前の一回だけである (`src/core/appflow/framephase.cpp:80-125`)。
- 全 project action set/action を session attach 前に作り、毎フレーム active set stack を
  `xrSyncActions` へ写す。非 FOCUSED は inactive snapshot とし error にしない。
- aim/grip は `left/right` を別 action 名にするか、公開 pose key に subaction dimension を追加する。
  head は synthetic source と明記し「全 action が XrAction と 1:1」を撤回する。
- `ActionPose` は少なくとも orientation_valid, position_valid,
  orientation_tracked, position_tracked と source/reference-space identity を持つ。
  v1 replay 非対応でも、その非対応を pose query/record 開始時の名前入り error で固定する。

## 7. [Blocker] TAA の「自動 off」は jitter=0 だけではなく graph variant の切替でなければならない

TAA は startup の feature compose で pass/history RT/shader define として graph に焼き込まれる。
登録時に feature names と projection jitter metadata が確定する
(`src/core/renderingpass/renderingpassconfigregistration.cpp:100-162`)。renderer の
`projection_jitter` も constructor で一回取得するだけである
(`src/core/vkcore/renderer.cpp:833-862`)。runtime session state を見て feature を除去する機構はない。

従って jitter sample を zero にしても `taa_resolve` / `taa_composite` と history read/write は残り、
「TAA 自動 off」にはならない。XR2a の sequential graph が history を左右で共有すれば §2 の眼間汚染も起きる。

v1 の正しい最小形は flat graph と XR graph を startup 時に両方 compile し、logical frame boundary で
選ぶことである。XR graph は TAA、projection jitter、velocity と未知の temporal/history feature を除外する。
未知 history feature が残る場合は XR activation を名前入りで拒否する。条件は FOCUSED ではなく
「XR projection view を描く logical frame」である。flat へ戻る最初の frame と XR へ入る最初の frame で
それぞれの temporal history を reset する。

gate は、TAA 有効 project で session transition trace を流し、XR frame plan に `taa_resolve`,
`taa_composite`, velocity/history read、projection jitter が無く、flat 復帰後には TAA が戻り reset epoch が
一回だけ進むこと。単なる `jitter_ndc == 0` 検査では不足である。

## 8. [Major] 関数表 fake は state machine には有効だが Vulkan swapchain 統合 fake にはならない

`XrApi` 関数表注入 (`docs/design_openxr.md:56-63`)は core call order のテスト seam としては現実的である。
ただし extension function は XrInstance 作成後に `xrGetInstanceProcAddr` で解決するため、表を単純な
link-time symbol 集合にしてはならない。core/extension dispatch の解決失敗もテスト対象にする必要がある。

fake が返す偽 `VkImage` / `VkImageView` handle を現行 renderer に渡すことはできない。renderer はそれらを
実 Vulkan command に記録するため、validation以前に未定義動作になる。テスト二重化を次へ分けるべきである。

1. **protocol fake**: Xr handle、event、frame state、view、swapchain index を偽装し、session 遷移、
   wait/begin/end、acquire/wait/release、zero-layer、loss/error、extension dispatch の順序だけを検査する。
   Vulkan API へ fake image を一切渡さない。
2. **Vulkan-backed synthetic stereo target**: OpenXR を使わず、engine が作った実 offscreen image/view を二眼分
   注入し、renderer logical-frame 分割、左右 UBO、history once、layout/barrier、capture を Vulkan validation
   付きで検査する。
3. **実 runtime integration**: Meta runtime/Quest Link は手動 gate。loader + runtime + compositor + runtime-owned
   VkImage の境界は protocol fake だけでは証明しない。

この分離なら関数表 fake を巨大な擬似 runtime に育てず、最も壊れやすい renderer 側も実 VkImage で試せる。

## 9. [Blocker] session loss/extent/format 変更は現行 resize bool では復帰できない

現行 `FrameTargetCaps` は format/extent を一組しか持たず、変更通知も bool 一個である
(`src/core/vkcore/frametarget.hpp:12-29`)。renderer の resize path は一 extent で internal RT を再作成し、
fullscreen input を rebind するだけである (`src/core/vkcore/renderer.cpp:822-830`)。
`RenderTarget` 自体も headless か否かで constructor 時に実装を一個選ぶ
(`src/core/vkcore/rendertarget.cpp:13-24`)。

OpenXR では view ごとの recommended extent/sample count、runtime-supported format、swapchain generation、
session generation を管理する必要がある。format が変われば config 登録時に解決した target format
(`src/core/renderingpass/renderingpassconfigregistration.cpp:100-116`)も再評価対象であり、単なる resize ではない。

XR2a には次の lifecycle を入れること。

- session 作成/再作成時に view configuration と swapchain formats を再照会する。
- `XR_EXT_view_configuration_views_change` を有効化できる runtime では該当 event で、そうでなければ
  新 session 作成時に generation を更新する。
- GPU 使用完了を待って view/image view/framebuffer を破棄し、その後 XrSwapchain を破棄する。
  再作成後に internal RT、descriptor、per-view history を一 transaction で差し替える。
- `LOSS_PENDING` は current session children を破棄し、同じ Vulkan device で再作成可能な場合だけ retry。
  `EXITING` は自動再開しない。`INSTANCE_LOST` 又は別 physical device 要求は v1 では flat 降格/再起動要求。
- flat desktop swapchain は mirror/降格用に XR target と別 lifetime で保持する。

## 10. [Blocker] mirror と screen-space UI は現行 output/capture path の流用では実装できない

設計は左眼を既存 window へ mirror し、screen-space UI は mirror のみに出す
(`docs/design_openxr.md:86-101`)。しかし desktop `recordOutputTransformCopy` は source/destination の format と
extent が完全一致しないと拒否する (`src/core/vkcore/swapchainframetarget.cpp:289-320`)。Quest の eye extent と
window extent は通常一致しない。runtime-owned XR image が transfer-src usage を持つ保証にも依存できない。

また UI は現在の frame graph 内の feature であり、XR graph から UI を外すだけでは mirror に UI は出ない。
flat mirror 用に「左眼 scene output を window extent へ scale/letterbox し、その後 UI を window 座標で overlay
して present」する別 sink/pass が必要である。XR image から copy せず、engine-owned scene/display intermediate
を transfer source にする。window minimize/out-of-date/acquire stall で HMD frame loopを止めないよう、mirror は
frame drop 可能な optional sink とする。

legacy capture の意味も固定が必要である。現在は active `RenderTarget` の last frame を読む
(`src/core/vkcore/rendertarget.cpp:45-69`)。XR 中に既存 `capture` を暗黙に左眼/mirrorへ変えると API 互換を壊す。
v1 は XR 中の legacy capture を名前入りで reject するか、`source=flat|mirror|left_eye` を新規 API として追加する。
headless/golden は常に flat/offscreen target だけを使い、XR code pathへ入れない。

## 11. [Major] build unit と activation policy が閉じておらず「既存挙動不変」と矛盾する

build tiers は全 unit default ON、OFF binary が参照されたら明確な error、single-OFF smoke を要求する
(`docs/design_build_tiers.md:41-49`)。OpenXR 設計は一方で「XR 無効なら INFO + flat 継続」とする
(`docs/design_openxr.md:26-34`)。明示要求と自動 probe を分けなければ両立しない。

次の activation contract を設けるべきである。

- compile default は `PELICAN_WITH_OPENXR=ON` でも、runtime default は `xr=off` とし既存起動を変えない。
- `--xr auto`: runtime/system/HMD 不在は名前入り INFO + flat。
- `--xr on`: compile OFF、runtime 不在、graphics binding 不適合は名前入り hard error。
- headless/RPC/golden/replay は `--xr on` を拒否し、auto は off に正規化する。
- dist-config は project 宣言又は `--with openxr` を入力にする。現行設計の「要求時のみ」が何を読むかを定義する。

OpenXR-SDK FetchContent は `if(PELICAN_WITH_OPENXR)` の内側だけで exact commit に固定し、loader/sample/test/API
layer の不要 target を無効化し、`OpenXR::openxr_loader` を private link する。OFF smoke は configure/build
だけでなく OpenXR source/object/library/symbol が成果物に無いこと、`--xr on` が
`PELICAN_WITH_OPENXR=OFF` を含む error になることを検査する。現行 unit の compile definitions/source exclusion
pattern は `src/core/CMakeLists.txt:6-18`, `src/core/CMakeLists.txt:82-90`、smoke の前例は
`test/run_build_units_smoke.cmake:393-503` にある。

## 12. 再提出時の WP 分割と逐語添付条件

現在の XR1/2a/3/4/2b 表 (`docs/design_openxr.md:130-138`)は XR1 と XR2a が大きすぎ、blocker を隠す。
以下へ分割することを Reject 解除の条件とする。

### XR0 — build unit / activation

> **XR0-BUILD-ACTIVATION**: 「`PELICAN_WITH_OPENXR` は default ON の private unit とし、OFF では
> OpenXR source/object/library/symbol を binary から除去する。runtime activation は off/auto/on の三値で、
> explicit on の unavailable は hard error、auto の unavailable は名前入り INFO + flat とする。
> headless/RPC/golden/replay は XR off を強制する。OpenXR-SDK は exact commit 固定・不要 target OFF とし、
> single-OFF smoke と full-build flat golden byte 一致を gate にする。」

### XR1a — OpenXR discovery + Vulkan bootstrap

> **XR1A-VULKAN-BOOTSTRAP**: 「XrInstance/XrSystem discovery を GPU resource 作成前に行い、
> `XR_KHR_vulkan_enable2` の requirements、runtime-selected physical device、window/engine 必須 extension を
> 満たす VkInstance/VkDevice を作る。runtime 不在時だけ resource 作成前に flat bootstrap へ降格する。
> fake が engine heuristic と異なる GPU を返す fixture、required extension merge/missing fixture、
> multi-GPU 実機記録を gate にする。」

### XR1b — session state + loop/display timing

> **XR1B-SESSION-LOOP**: 「session_running は beginSession 成功から endSession 呼出しまでとし、state の
> 数値比較で代用しない。running 中は毎回 wait/begin/end を一組実行し、shouldRender=false は update 一回・
> zero layer、FOCUSED は input eligibility のみに使う。predictedDisplayTime は immutable display timing として
> simulation EngineTime から分離し、FramerateAdjust は running 中だけ bypass する。READY/STOPPING/
> LOSS_PENDING/EXITING と wait/begin/end error の call trace fixture を gate にする。」

### XR1c — protocol fake seam

> **XR1C-TEST-SEAM**: 「core/extension dispatch は `xrGetInstanceProcAddr` 解決を含む注入表とする。
> protocol fake は fake VkImage を Vulkan へ渡さず、session/event/frame/swapchain call order と error/loss を
> 検査する。renderer は別の real-Vulkan synthetic stereo target で検査する。」

### XR2a.0 — renderer logical-frame/view 分離（OpenXR 非依存）

> **XR2A0-LOGICAL-FRAME**: 「`Renderer::render()` を logical frame と per-view execution に分離する。
> update/reload/deletion/animation、target acquire/submit、history flip、object/morph advance、snapshot commit は
> logical frame ごとに一回だけ行う。Frame UBO は in-flight×view の上書き不能 slot とする。
> real offscreen 二眼 fixture で異なる view/projection/camera_position が各出力へ届き、frame_index/history/
> epoch/object previous が一回だけ進むことを validation layer 付きで検査する。view_count=1 の既存 golden は
> byte 一致させる。」

### XR2a.1 — Xr composition target / swapchain

> **XR2A1-SWAPCHAIN**: 「二個の 2D swapchain 又は一個の 2-layer swapchain のどちらかを規範として選ぶ。
> 一 logical frame で各 swapchain を acquire→wait→GPU submit→release し、左右両 viewを一枚の projection
> layerとして一回の xrEndFrame に渡す。release layout/queue ownership、format、sample count、per-view rect、
> shouldRender=false、partial failure unwind を call trace と Vulkan fixture で固定する。
> `IFrameTarget` の flat acquire/present 一回契約を無理に偽装しない。」

### XR2a.2 — view snapshot / reference space

> **XR2A2-VIEW-SPACE**: 「`world_from_stage * stage_from_eye` と inverse view の行列式、RH/+Y/-Z/m/xyzw、
> asymmetric RH_ZO projection と viewport Y、per-view camera_position を規範化する。STAGE/LOCAL_FLOOR/LOCAL
> の選択と LOCAL floor offset を status に出す。identity、既知 IPD、90 度 yaw、非対称 FOV、LOCAL fallback の
> CPU fixture と Quest 実機の左右/IPD/head tracking gate を持つ。」

### XR2a.3 — feature policy + mirror

> **XR2A3-FEATURE-MIRROR**: 「flat graph と history/TAA/velocity/UI を除いた XR graph を precompile し、
> logical frame boundary で切り替える。XR entry/flat return で各 history を一回 reset する。mirror は
> engine-owned left-eye intermediate を window extent へ scale/letterbox 後、screen-space UI を overlay する
> optional sink とし、window stall で HMD を止めない。legacy capture/golden は flat path の意味を変えない。
> TAA-on transition trace、window resize/minimize、XR-off golden byte 一致を gate にする。」

### XR3a — button/axis/action set、XR3b — pose provider

> **XR3A-ACTIONS**: 「project action/action set を attach 前に XrAction へ作成し、active set stack を
> xrSyncActions へ写す。Touch suggested binding、FOCUSED loss、boolean/float/vector2、左右 subaction を
> protocol fake と実機で検査する。」
>
> **XR3B-POSE**: 「L1 を ordered event + typed pose sample に拡張し、aim/grip action space と synthetic
> head/view source を区別する。position/orientation の valid/tracked flag、reference space、左右 identity、
> freeze 時点を固定する。pose record/replay v1 非対応は名前入り error とする。」

### XR4 — demo、XR2b — multiview

XR4 は XR2a.0〜3 と XR3a/b 完了後に限る。XR2b は XR2a.0 の logical-frame/view contract を保ったまま
graph/pass の view dimension を追加し、XR2a と同じ semantic image、flat view_count=1 byte 一致、GPU 計測改善を
gate にする。XR2a の壊れた二回 `Renderer::render()` を互換契約として残してはならない。

## 13. CI と Quest 3 手動 gate の境界

### CI で代替できるもの

- build unit ON/OFF、runtime/system unavailable、extension/device mismatch
- session state と call order、shouldRender=false、loss/error unwind
- acquire/wait/release と全 view 一括 submit の protocol trace
- synthetic stereo の左右行列/UBO/output、logical history 一回 advance
- STAGE/LOCAL 行列、asymmetric FOV、IPD の CPU fixture
- TAA/history/UI を除いた XR graph trace、flat 復帰 reset
- XR off/headless/RPC/golden の既存 image byte 一致

### Quest 3 (Link/Air Link) でしか閉じないもの

- Meta runtime が選ぶ GPU/extension と実 XrSwapchainImageVulkan の互換性
- READY→SYNCHRONIZED→VISIBLE→FOCUSED と focus loss/reacquire
- 両眼の正しい IPD、歪み、head tracking、controller aim/grip と tracking loss
- shouldRender=false/system UI、HMD 着脱、Link 切断再接続、session loss 復帰
- window resize/minimize 中も HMD frame loop が継続すること
- TAA 有効 project でも XR 中に ghost/jitter がなく、flat 復帰後に TAA が正常再開すること
- 15 分連続、Vulkan/OpenXR validation error 0、crash 0、frame pacing 記録

元の手動 checklist (`docs/design_openxr.md:122-128`)には、runtime-selected GPU、focus loss、
shouldRender=false、window minimize、Link 切断/session recreation、TAA-on project、LOCAL fallback を追加すること。
スクリーンショット/左眼 mirror 録画だけでは左右眼/IPD、compositor timing、tracking loss の合格根拠にならない。

## 結論

XR2a は frame graph の node 定義を無改造に保つことはできる。しかし renderer/FrameUBO/frame target を
無改造のまま二回呼ぶことはできない。正しい最小境界は「一 logical frame の中に二 view execution」であり、
OpenXR frame lifecycle、per-view immutable GPU data、history once を最初に作ることである。

また XR1 は単なる loader/session WP ではない。runtime-selected Vulkan device を resource 作成前に採用する
bootstrap WP でなければならない。この二点を直さないまま XR1/XR2a を開始すると、fake runtime の call-order
testだけは通っても Quest Link では session 作成又は左右描画で失敗する。従って v1 は **Reject** とする。
