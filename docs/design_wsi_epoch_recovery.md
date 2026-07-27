# Window presentation / WSI epoch と回復設計(v1)

対象読者: Vulkan・renderer・OpenXR mirror の実装者。

ステータス: v1 実装方針(2026-07-27、ユーザー決定)。本書は window
surface、swapchain、window 出力へ依存する render target / pipeline の寿命と
回復について正本とする。renderer 全体の compile / publication 規則は
[`design_render_pipeline_extensibility.md`](design_render_pipeline_extensibility.md)、
logical / target compiler は
[`design_render_graph_compiler.md`](design_render_graph_compiler.md)、
色意味論は [`design_color_pipeline.md`](design_color_pipeline.md)、
OpenXR composition lifecycle は
[`design_openxr.md`](design_openxr.md) を正とする。

## 0. 決定

1. `SurfaceEpoch` / `SwapchainEpoch` を、**プロセス内だけの実行時寿命単位**
   として導入する。これは project schema、public ABI、保存形式の version ではない。
2. `VK_ERROR_OUT_OF_DATE_KHR` は同じ `SurfaceEpoch` の
   `SwapchainEpoch` だけを交換し、`VK_ERROR_SURFACE_LOST_KHR` は
   `SurfaceEpoch` ごと交換する。
3. window 出力、window facts に依存する target plan / GPU resource /
   pipeline は既存 renderer generation と一つの immutable root で publish
   する。WSI 専用の第二 active root は作らない。
4. 旧 epoch は旧版互換のために残さない。旧 frame の GPU submit と
   presentation lifetime が終了するまでだけ保持し、終了後に破棄する。
5. zero extent、`OUT_OF_DATE`、`SUBOPTIMAL`、`SURFACE_LOST` は通常の
   runtime 状態として分類し、renderer から `glfwWaitEvents()` や
   `device.waitIdle()` を呼ばない。flat main loop、RPC、hot reload、ECS、
   audio、XR HMD loop は継続する。
6. `IFrameTarget` は begin/end 間の暗黙 mutable state を廃止し、
   move-only frame token を受け渡す breaking API へ変更する。旧 API の
   compatibility facade は残さない。
7. surface 再作成後に現在の physical device / 作成済み queue が present
   できない場合、別 physical device へ暗黙移行しない。flat は
   `device_rebuild_required`、optional XR mirror は理由付き disable とする。
8. WSI の format / present-mode 選択 policy は将来差し替え可能な data-only
   境界に置くが、native surface、present 同期、epoch publication は engine
   runtime の責務に保つ。

## 1. 対象と非対象

対象:

- 通常 window の surface / swapchain / depth / acquire・present 同期
- flat 出力と、同じ window を使う optional OpenXR desktop mirror
- resize、minimize、DPI / display 移動、format / color-space 変更
- `SUBOPTIMAL`、`OUT_OF_DATE`、`SURFACE_LOST`
- window facts 変更に伴う target lowering、GPU target、descriptor、
  output pipeline、temporal history の更新
- prepare / publish / rollback-or-suspend / retire と fault injection

非対象:

- `VK_ERROR_DEVICE_LOST` 後の logical device / allocator / 全 GPU resource
  再作成。これは別の device recovery 設計とする
- OpenXR runtime が所有する composition swapchain の置換。そちらは
  `design_openxr.md` §9 の session generation に従う
- HDR / wide-gamut の authoring policy。本書は surface format / color space
  が変わったとき、選択済み色契約を再 compile へ正しく渡すところまでを扱う
- OS native window 自体の自動作り直し。factory は将来の
  `native_window_rebuild_required` を表現できるが、v1 は同じ GLFW window
  から surface を作り直す

## 2. 現行監査との対応

本設計は Claude / Codex の 2026-07-26 bug hunt で残った window 出力群を、
局所 catch の追加ではなく一つの lifecycle として閉じる。

| Finding | 根本原因 | 本設計での閉じ方 |
|---|---|---|
| A-F2 | positive extent 待機後の再 query で 0 に戻る TOCTOU | extent snapshot を revision 付きで取り、0 は nonblocking `suspended_zero_extent` |
| A-F3 |複数 internal RT を一個ずつ commit | target resource candidate を全作成後、renderer root と一回 publish |
| A-F4 | acquire / fence reset 後に resize 処理が throw | output revision を begin 前に解決し、move-only frame token で begun frame を必ず閉じる |
| A-F5 | surface の fresh factory / 交換所有者が無い | `WindowSurfaceFactory` と `SurfaceEpoch` を導入し、`VulkanManageCore::getSurface()` を削除 |
| A-F6 | format 変更を extent-only resize として処理 | typed `OutputCompileFacts` fingerprint から target lowering / output pipeline を再 prepare |
| A-F7 |同じ `SUBOPTIMAL` で毎 frame 全再構築 | dirty revision + facts hash で coalesce し、同じ結果を再試行しない |
| A-F9 |旧 swapchain / depth の破棄が候補完成より先 | engine-owned resource は candidate 化。WSI の不可逆 cutover は明示 state で封じ込める |
| A-F11 | renderer 内の `glfwWaitEvents()` が全 loop を停止 | target は `unavailable` を返すだけ。event wait / tick rate は app loop の policy |
| A-F12 | mirror recovery の `device.waitIdle()` が HMD を止める | mirror は frame drop、recovery は HMD critical path 外、retire は fence poll |
| A-F13 | bool latch と例外経路で frame / generation lease が残る | frame token と epoch-owned submission slot。状態遷移を一か所で検証 |

A-F10 の present semaphore は既に swapchain image 単位へ修正済みである。本書は
その規則を epoch の steady-state / retirement 契約として固定する。

## 3. 「version」と「epoch」を混ぜない

### 3.1 Version

project schema、serialized artifact、game-DLL ABI の version は外部データの
解釈を選ぶ識別子である。現時点では `implementation_plan.md` §0 に従い、
**現行版一つだけを受理**し、旧版 parser / adapter / migration を runtime に
残さない。

### 3.2 Epoch

epoch は同じ型の runtime object が置き換わったことを表す。

```cpp
using SurfaceEpochId = std::uint64_t;
using SwapchainEpochId = std::uint64_t;
using RendererRuntimeGenerationId = std::uint64_t;
```

- process-local、単調増加、保存しない
- decoder / compatibility 分岐に使わない
- stale token の検出、log、status、fault test にだけ使う
- 古い epoch を新 epoch と相互運用しない
- 古い epoch は、その epoch を参照した submit / present の完了までだけ生存する

TAA 等の `temporal_reset_epoch` も runtime discontinuity marker だが、
WSI object の所有権とは別である。window output の意味が変わったときに
WSI publication が temporal reset を一回要求するだけで、同じ counter を共有しない。

## 4. 不変条件

1. **一 logical frame = 一 renderer root**
   frame 開始時に取得した root を全 view、全 pass、submit まで保持する。
2. **一 frame token = 一 swapchain epoch**
   acquire した epoch と異なる epoch へ submit / present しない。
3. **Ready は完全な組だけ**
   active state が `ready` なら、surface、swapchain、views、同期物、
   output facts、target plan、GPU target、pipeline fingerprint が一致する。
4. **partial candidate は不可視**
   surface / swapchain / image view / target / pipeline の途中生成物を
   `getExtent()` 等の global facade から観測できない。
5. **通常の WSI 状態で loop を止めない**
   zero extent、out-of-date、surface lost は frame skip / recovery request
   であり、RPC / reload / ECS / audio / XR frame を止める待機ではない。
6. **GPU completion と present completion を同一視しない**
   submit fence は command buffer / image view 利用を守るが、
   `vkQueuePresentKHR` が待つ binary semaphore の消費完了を単独では証明しない。
7. **旧 root の寿命は参照で決まる**
   frame count や「二世代あれば十分」という推測で破棄しない。
8. **surface change は logical authoring を書き換えない**
  同じ compiled logical graph を、新しい target facts で再 lower する。
9. **不可逆操作を rollback 可能と偽らない**
   Vulkan WSI が旧 swapchain を retire した後の失敗は、旧 Ready へ戻さず
  明示 `unavailable` に留めて再試行する。
10. **optional mirror は optional のまま**
    mirror の失敗、resize、surface recreation は HMD composition の
    success / frame pacing を変更しない。

## 5. 所有構造

```text
Window
  ├─ framebuffer snapshot {extent, revision}   (callback から更新)
  └─ WindowSurfaceFactory(instance, native window)
         └─ PreparedSurface

Vulkan runtime
  ├─ InstanceRuntime
  ├─ DeviceRuntime {physical device, device, created queues, capabilities}
  └─ PresentationRetirementQueue

RendererRuntimeGeneration                 immutable / atomic publication
  ├─ compiled logical pipeline             target-independent child
  ├─ flat / XR target runtime children     unchanged childは共有可能
  └─ WindowOutputGeneration?               headlessでは無し
       ├─ OutputCompileFacts + fingerprint
       ├─ SurfaceEpoch
       │    └─ vk::UniqueSurfaceKHR + present support facts
       ├─ SwapchainEpoch
       │    ├─ swapchain / images / views / depth
       │    ├─ per-slot acquire sync / command state
       │    ├─ per-image present semaphore / optional present fence
       │    └─ WsiPresentConfiguration
       └─ target-specific physical plan / GPU resource leases

FrameTargetFrame                           move-only
  ├─ shared_ptr<const RendererRuntimeGeneration>
  ├─ shared_ptr<const SwapchainEpoch>
  ├─ image / slot / command context
  └─ acquired → recording → submitted → presented/abandoned
```

ここで immutable なのはepochのmember集合、handle identity、facts、
fingerprintと所有関係である。fence / semaphoreのdevice stateとepoch-scopedな
frame slot ledgerは、公開済みmemberを差し替えずexecutorの状態機械に従って進む。
「rootがimmutable」を「Vulkan同期objectがsignal/resetされない」という意味には
しない。

`RendererRuntimeGeneration` は既存
`RenderPipelineRuntimeGeneration` publication を広げた名前である。別の
`active_wsi` pointer を追加して二回 publish してはならない。既存名が実体を
誤解させる場合は breaking rename し、compatibility alias は残さない。

### 5.1 Vulkan bootstrap

現在の `VulkanManageCore` は instance、surface、physical/logical device を
一つの永久所有 object にしている。最終形は bootstrap を次の順で構成する。

```text
create instance
→ create initial PreparedSurface
→ surfaceを使ってphysical device / queue familyを選ぶ
→ create logical device / queues
→ initial SurfaceEpoch + SwapchainEpoch + renderer candidateを作る
→最初のRendererRuntimeGenerationをpublish
```

bootstrap の返り値は内部 aggregate とし、composition root が instance/device を
Vulkan runtime へ、initial surface を最初の window output candidate へ move する。
`VulkanManageCore` は active surface を持たず、次だけを提供する。

- instance / physical device / device / 作成済み queue と capability snapshot
- `WindowSurfaceFactory` を作るための instance lifetime
- image / buffer / command 等の device service

次を削除する。

- `vk::UniqueSurfaceKHR VulkanManageCore::surface`
- `VulkanManageCore::getSurface()`
- 「core が持つ唯一の surface は永遠に有効」という暗黙契約

### 5.2 SurfaceEpoch

```cpp
struct SurfaceEpoch {
    SurfaceEpochId epoch_id;
    std::uint64_t native_window_revision;
    vk::UniqueSurfaceKHR surface;
    vk::PhysicalDevice physical_device;
    std::uint32_t graphics_queue_family;
    std::uint32_t presentation_queue_family;
    SurfaceSupportSnapshot support;
};
```

`SurfaceSupportSnapshot` は capabilities、format 列、present mode 列と、
選択理由を作るための immutable data である。raw Vulkan query と policy
resolve を分け、同じ snapshot / policy から同じ選択を得る CPU test を持つ。

fresh surface 作成後は必ず次を再検証する。

- 現在の physical device が surface を support する
- 現在 device で実際に作成済みの presentation queue family が support する
- graphics / presentation family の sharing / ownership plan が成立する
- 要求する swapchain extension / selected usage / format が成立する

別の queue family が surface を support していても、その family の queue を
logical device 作成時に要求していなければ後から取得できない。その場合は
`device_rebuild_required` である。

### 5.3 SwapchainEpoch

`SwapchainEpoch` は swapchain に従属する object を一つに束ねる。

- `vk::UniqueSwapchainKHR`
- swapchain images と image views
- target-owned depth image / view
- acquire semaphore は in-flight slot 単位
- present wait semaphore は **swapchain image 単位**
- optional `VK_KHR_swapchain_maintenance1` /
  `VK_EXT_swapchain_maintenance1` present fence
- frame slot fence / command buffer と、その slot が保持する runtime lease
- selected format / extent / usage / image count / present mode
- image ごとの「前回 present wait の完了が reacquire で証明済みか」

外側の `SwapchainFrameTarget` に `current_image_index`、`frame_acquired`、
`frame_recording`、`frame_submitted` 等をばらばらに残さない。per-frame
状態は `FrameTargetFrame`、epoch 全体の状態は `SwapchainEpoch` が持つ。

## 6. Compile facts と WSI-only configuration

Vulkan の全 surface field を renderer graph fingerprint に入れると、present mode
だけの変更でも pipeline を作り直す。逆に extent / format だけを見ると、
queue ownership、capture usage、color path の変更を落とす。そこで二つに分ける。

### 6.1 OutputCompileFacts

```cpp
struct OutputCompileFacts {
    OutputTargetKind target_kind;        // window
    vk::Extent2D extent;
    vk::Format color_format;
    vk::ColorSpaceKHR color_space;
    OutputEncodingPath encoding_path;    // srgb_hardware / srgb_shader_unorm / future HDR
    vk::ImageUsageFlags selected_usage;
    bool capture_available;
    vk::SurfaceTransformFlagBitsKHR surface_transform;
    std::uint32_t graphics_queue_family;
    std::uint32_t presentation_queue_family;
};
```

`FrameTargetCaps::color_path` の free-form string は
`OutputEncodingPath` へ置換する。RPC / log で文字列化するのは表示 adapter の
責務とし、compiler が string 比較しない。

`OutputCompileFacts` の canonical byte / hash は次を束縛する。

- target lowering と concrete resource extent / format
- output transform implementation / specialization
- attachment format を含む graphics pipeline compatibility
- capture path の availability
- queue family ownership / sharing plan
- temporal reset を要する output discontinuity

### 6.2 WsiPresentConfiguration

```cpp
struct WsiPresentConfiguration {
    vk::PresentModeKHR present_mode;
    std::uint32_t image_count;
    vk::CompositeAlphaFlagBitsKHR composite_alpha;
    bool clipped;
};
```

これらは通常、frame graph / pipeline の意味を変えない。変更時は
`SwapchainEpoch` だけを作り直し、同じ target runtime child を共有できる。
将来 present-mode compatibility extension を使う場合もこの層だけで扱う。

selected usage、surface transform 等が target compile へ影響しないと証明できた
場合だけ `OutputCompileFacts` から WSI-only 側へ移す。「現在たまたま読んでいない」
ことを理由に fingerprint から外さない。

## 7. 状態機械

### 7.1 Window output state

| State | begin の結果 |許可する遷移 |
|---|---|---|
| `ready` | frame token | `refresh_pending` / `surface_lost` / `suspended_zero_extent` |
| `refresh_pending` | suboptimalなら継続可、out-of-dateならskip | `preparing` / `suspended_zero_extent` |
| `suspended_zero_extent` | `unavailable(zero_extent)` | positive extent revisionで`preparing` |
| `surface_lost` | `unavailable(surface_lost)` | old epoch detach後`preparing_surface` |
| `preparing` | policyにより旧Ready継続またはskip | `ready(new)` / `unavailable_retry` |
| `unavailable_retry` |理由付きskip |新しいrevision / backoff満了で`preparing` |
| `device_rebuild_required` | flatは終了要求、mirrorはdisable | device recovery設計以外では復帰しない |
| `fatal` |終了要求 |無し |

state は bool の組ではなく discriminated union とする。`surface_stale` と
`extent_changed` を別々に latch しない。

### 7.2 VkResult の分類

acquire、present、surface query、swapchain creation の Vulkan-Hpp 例外を
各 call site で個別 catch しない。raw result / exception を一つの adapter で
次へ写像する。

| Vulkan result |分類 |方針 |
|---|---|---|
| `eSuccess` | ready |通常継続 |
| `eSuboptimalKHR` | refresh advisory |現在 frame は継続可。facts revision単位で一回だけrefresh |
| `eErrorOutOfDateKHR` | swapchain unavailable |現在 frameをdropし、同surfaceで再prepare |
| `eErrorSurfaceLostKHR` | surface unavailable |surface + swapchain epochをdetach / recreate |
| `eErrorDeviceLost` | device lost |本書外。WSI retryへ誤分類しない |
| host/device OOM | prepare failure |active旧出力が使えるなら維持。retire後ならunavailable + bounded retry |
|その他 | fatal / platform decision |名前入り診断。無限retryしない |

### 7.3 SUBOPTIMAL の coalesce

`SUBOPTIMAL` を受けるたびに再作成してはならない。recovery key を
次で作る。

```text
{window framebuffer revision,
 surface support fingerprint,
 selected OutputCompileFacts,
 selected WsiPresentConfiguration}
```

- 同じ key で成功済みなら再作成しない
- 同じ key で失敗済みなら毎 frame 再試行せず backoff / 新 revision を待つ
- 複数 callback / acquire / present 通知は一 candidate へ coalesce
- 新 candidate の facts が active と同じなら advisory を acknowledge し、
  pipeline / target を作り直さない

### 7.4 Zero extent

`Window` は framebuffer callback から `{extent, revision}` の一貫した
snapshot を更新する。worker が `glfwGetFramebufferSize()` を直接呼ばない。

- 幅または高さ 0 はエラーでなく `suspended_zero_extent`
- renderer / frame target は `glfwWaitEvents()` を呼ばない
- flat app の省電力待機は app loop が `waitEventsTimeout` 等で行い、RPC /
  audio / reload の tick 上限を守る
- XR 中は mirror だけをdropし、HMD logical frameを継続
- positive extentへ戻った revision で一回だけ candidate を作る

## 8. Prepare / cutover / publish / retire

### 8.1 通常の resize / OUT_OF_DATE

1. **observe**
   window snapshot と surface support を読み、data-only policy で
   `OutputCompileFacts` / `WsiPresentConfiguration` を resolve する。
2. **prepare engine-owned resources**
   compiled logical graph は共有し、新 facts に対する target plan、
   extent-relative render targets、descriptor、必要な pipeline を candidate
   arenaへ作る。global registryを一個ずつ書き換えない。
3. **validate**
   candidate の target-plan fingerprint、pipeline attachment signature、
   output facts fingerprint が一致することを検証する。
4. **frame-boundary cutover**
   base renderer generation が変わっていないことを確認し、新 acquire を止める。
   `vkCreateSwapchainKHR(oldSwapchain=...)` を実行し、swapchain images / views /
   sync object を完成させる。
5. **publish**
  完全な `WindowOutputGeneration` を既存 renderer generation と
   **一回の CAS** で publish する。
6. **retire**
  旧 renderer root、target arena、swapchain epochをそれぞれのGPU /
   present completion後に解放する。

Vulkan では `oldSwapchain` を指定した `vkCreateSwapchainKHR` は、作成が
失敗しても old swapchain を retire し得る。従って step 4 以降は強い
rollback を約束できない。

- step 4 より前の失敗: candidate だけ rollback、旧 `ready` を維持
- step 4 以降の失敗: partial candidate を破棄し、active state を
  `unavailable_retry` へ一回 publish。retired old swapchain を再利用しない
- observer から見えるのは complete `ready` または complete `unavailable`だけ

これを「WSI も完全 rollback 可能」と偽らず、**failure-contained
transaction** と呼ぶ。

### 8.2 SURFACE_LOST

surface lost 後の旧 epoch は表示に再利用できない。処理順は次で固定する。

1. active root を `surface_lost` とし、新 acquire を止める
2. 旧 epoch の submit fence をnonblocking pollし、完了したframe slotからleaseを解放
3. 旧 swapchainをretireし、未証明present semaphoreを§9のquarantineへ移す
4. 旧 swapchainを破棄してから旧 surfaceを破棄
5. `WindowSurfaceFactory` でfresh surfaceを作る
6. 現在の physical device / created queuesに対するsupportを再照会
7. 同じdeviceで成立すれば新`SurfaceEpoch`、target candidate、
  `SwapchainEpoch`をprepareして一回publish
8. 成立しなければ`device_rebuild_required`

surface 作成自体に失敗した場合も、旧 surfaceへ戻らない。
`unavailable_retry` のまま新しい window revision / bounded backoff で再試行する。

### 8.3 Hot reload との競合

hot reload と WSI recovery は同じ base
`RendererRuntimeGenerationId` を持つ transaction とする。

- prepare は並行可能
- GPU registry commit / WSI cutover / publication は engine owner の
  serialize 区間
- 先に別 candidateがpublishされbase idが変わったら、cutover前のcandidateは
  staleとして捨て、新rootを基準にprepareし直す
- WSI cutover後はstale CASにしてはならないため、base再確認とWSI cutoverを
  同じowner区間に置く
- window出力だけが変わる場合、flat/XR logical / GPU childのうち
  fingerprintが同じものは共有する

## 9. Present lifetime と global idle の排除

### 9.1 steady state

`vkQueuePresentKHR` の wait semaphore は swapchain image index で選ぶ。
その image を次に acquire し、acquire signalをqueue submitでwaitした時点で、
前回そのimageをpresentした操作がwait semaphoreを消費済みであることを証明できる。

frame slot fenceでpresent semaphoreを再利用しない。submit fenceは
presentation engineの完了を保証しない。

### 9.2 exact retirement capability

`VK_KHR_swapchain_maintenance1` または
`VK_EXT_swapchain_maintenance1` が利用可能なら、各 present に
`VkSwapchainPresentFenceInfoKHR` / `EXT` を付ける。fence signal は
present wait semaphoreをdestroy / recycleしてよい時点を表す。

- `VulkanRuntimeCapabilities` に `swapchain_maintenance1` を追加
- fence は `SwapchainEpoch` が所有
- retire queue はwaitせずpollし、GPU submitとpresent fenceの双方が完了した
  epochだけを破棄
- extension非対応を起動拒否にはしない

`VK_KHR_present_wait` はpresentation pacing用の別契約であり、v1では
present resource retirementの代用品としない。利用を追加する場合は、
retired / out-of-date / surface-lost時にもresource破棄を証明できることを
公式仕様とtarget device testで別途固定する。

### 9.3 base Vulkan fallback

maintenance1非対応deviceでは、reacquireにより消費完了を証明できた
per-image semaphoreだけを通常破棄する。retire時点で証明できない
present semaphoreは小さな `PresentationSemaphoreQuarantine` へmoveし、
device teardownまで保持する。

- whole swapchain epoch / render target / image viewをprocess lifetimeへ漏らさない
- quarantine件数、byte概算、原因epochをstatusへ出す
- `device.waitIdle()` / `queue.waitIdle()` を「present完了証明」として使わない
- 同じSUBOPTIMALのcoalesceによりquarantineの無制限増加を抑える
- budget超過時はwarningを出し、flatはcontrolled restart要求、
  optional mirrorはdisableを選べる。未証明semaphoreを推測でdestroyしない

### 9.4 XR mirror

mirror beginが`unavailable`なら、そのlogical frameのmirrorだけをdropする。
`XrMirrorSink` から同期的な `recoverSurfaceIfStale()` を呼ばない。

- HMD critical path上では fence wait、`device.waitIdle()`、
  `glfwWaitEvents()`、retry loopを実行しない
- surface/swapchain createのようにdriver内でblockし得るprepareは
  presentation maintenance workerへ送る
- GLFW surface作成はworkerから可能だが、framebuffer extentはmain-thread
  callback snapshotを使う
- old/new surface・swapchainごとにhost access mutexを分け、
  active objectとworker candidateを同時に触らない
- renderer registryのcommitとroot publicationだけをframe boundaryの
  engine owner区間へ戻す
- workerを利用できないplatformではXR session中のmirrorをdisable / postponeし、
  HMD threadでblocking recoveryを代行しない

## 10. Frame token API

概念 API:

```cpp
enum class FrameBeginDisposition {
    ready,
    unavailable,
    device_rebuild_required,
    fatal,
};

struct FrameBeginResult {
    FrameBeginDisposition disposition;
    FrameUnavailableReason reason;
    std::optional<FrameTargetFrame> frame;
};

class IFrameTarget {
  public:
    virtual FrameBeginResult beginFrame(
        std::shared_ptr<const RendererRuntimeGeneration> runtime,
        FrameBeginMode mode) = 0;
    virtual FrameSubmitResult submit(FrameTargetFrame frame) = 0;
    virtual void abandon(FrameTargetFrame frame) noexcept = 0;
};
```

`FrameTargetFrame` は copy不可・move-only とし、次を保持する。

- renderer runtime generation lease
- surface / swapchain epoch lease
- in-flight slot、image index
- command buffer、attachments、extent、required layout
- acquired / recording / submitted の単一 state

`submit` / `abandon` は token を消費する。同じ token の二重 submit、
別 targetへの submit、epoch mismatch はassertだけでなくrelease buildでも
名前入りerrorにする。

routine WSI状態はexceptionでbegin/endを飛び越えず、`FrameBeginResult` /
`FrameSubmitResult`へ分類する。OOM、programmer error、device lost等の
例外が出ても、explicit `abandon`またはtokenが持つepoch cleanup control blockの
noexcept destructorが次を保証する。destructorはblocking Vulkan callを行わず、
必要なcleanup / retireをtargetのmaintenance queueへenqueueする。正常経路では
explicit consumeを必須とし、暗黙destructor cleanupはdebug診断へ残す。

- reset済みfenceを未signalのままslot再利用しない
- 取得済みimageを黙って忘れない
- maintenance1対応なら`vkReleaseSwapchainImagesKHR`を利用可能
- 非対応ならminimal submit/presentまたはepoch retireへ進み、
  同じimageを通常frameとして再利用しない
- runtime / DLL / GPU registry leaseを対応fence前に解放しない

次の旧 API は削除する。

- 引数なし `render_begin()` / `render_end(GpuSubmissionLease)`
- 外側mutable stateへ作用する `abort_render()`
- sticky boolを読む `consumeExtentChanged()`
- callerがblocking回復を起動する `recoverSurfaceIfStale()`

## 11. Render graph / pipeline との接続

surface/swapchain変更はauthoring requestを変更しない。invalidatedな段階だけを
再実行する。

|変更 | Logical compile | Target compile / physical lowering | GPU target / descriptor | Graphics pipeline | Temporal reset |
|---|---|---|---|---|---|
| extent |再利用 |再実行 | extent依存だけ再作成 | format/sample key同一なら再利用 | 1回 |
| format / color space / encoding path |再利用 |再実行 |影響targetを再作成 | attachment/output specializationを再作成 | 1回 |
| selected usage / capture availability |再利用 |影響consumerを再検証 |必要分だけ |通常再利用 | capture graphが変われば1回 |
| queue family binding |再利用 |queue ownershipを再lower | command/sync plan再作成 |通常再利用 |不要 |
| present mode / image count |再利用 |再利用 | WSI syncだけ |再利用 |不要 |
| surface epochだけ変更、facts同一 |再利用 |再利用 |再利用 |再利用 |不要 |
| project / feature hot reload |既存RPE規則 |既存RPE規則 |既存RPE規則 |既存RPE規則 |既存規則 |

初回実装が安全側にwindow target variant全体を再prepareすることは許すが、
logical compileまでやり直すことを恒久仕様にしない。dumpには各childの
`reused / rebuilt` と理由を残し、後で測定に基づきcache粒度を細かくできるようにする。

`RenderTargetContainer::recreateForExtent()` のin-place更新は最終形では削除する。
candidate arenaは全targetを作り終えてからrootへ結び、途中失敗時はarena全体を
逆依存順にrollbackする。fullscreen input rebind、layout tracker、temporal
historyも「publish後の副作用列」ではなくcandidate / publication contractへ含める。

## 12. ユーザーが変更できる境界

通常のproject / feature作者が変更できるもの:

- window output preset(SDR / 将来HDR等)
- format / present mode / image countの希望を表すtyped policy input
- capture有効化、latency / vsync preference
- output transformより上のlogical pass / physical fragment

engine sourceまたはnative backend extensionが必要なもの:

- native window / `VkSurfaceKHR` の作成
- acquire / present / present fence / semaphore retirement
- epoch state machineとatomic publication
- device / queueの作り直し

selection algorithm自体を差し替える場合も、providerへraw surface、
`vk::Device`、mutable moduleを渡さない。

```text
SurfaceSupportSnapshot + WindowPresentationRequest
        → ResolvedWindowPresentation
```

という純粋関数境界を使い、結果をruntime側が再検証する。builtinと
project/game providerを将来同じregistryに置けるが、具体需要とfixtureができるまで
public ABIを凍結しない。

## 13. 診断

`get_status` / frame plan dumpへ最低限次を追加する。

```text
window_output.state
window_output.reason
window_output.surface_epoch
window_output.swapchain_epoch
window_output.renderer_generation
window_output.extent_revision
window_output.output_facts_hash
window_output.present_config
window_output.last_vk_result
window_output.retry_count
window_output.last_successful_recovery_ms
window_output.retirement_mode = present_fence | reacquire | quarantine
window_output.quarantined_present_semaphores
window_output.device_rebuild_reason
```

epoch idは診断用であり、RPC consumerに旧epochとの互換動作を要求しない。
状態名とreason codeはtyped内部値から表示adapterで文字列化する。

## 14. テスト

### 14.1 Pure CPU

- surface format / present mode / image count resolverの決定性
- `OutputCompileFacts` canonical hash、含めるfield /除外するfield
- VkResult → state transitionの全表
- SUBOPTIMAL coalesce key、success / failure backoff
- 0→positive→0 extent revision
- present support変化時のsame queue / already-created alternate queue /
  `device_rebuild_required`
- format / extent / present modeごとのrebuild matrix(§11)

### 14.2 Protocol fake / fault injection

各 fault point でactive rootがcomplete oldまたはcomplete unavailableの
どちらかであり、partial newを観測しないことを検証する。

1. surface作成後
2. support query後
3. target compile後
4. GPU target / pipeline途中
5. swapchain create直前
6. swapchain create成功直後
7. image view / depth / semaphore途中
8. publication直前
9. submit成功・present例外
10. surface lost中の再surface作成失敗

fakeは `SUBOPTIMAL`、`OUT_OF_DATE`、`SURFACE_LOST` をacquire / present /
query / createの各位置へ注入する。call order、old epoch retirement、
frame tokenのexact consume、lease解放を検査する。

### 14.3 Vulkan integration

- window resize / maximize / minimize / restoreを繰り返し、validation error 0
- resize中もRPC / hot reload / audio / ECS tickが進む
- format path fixture(SRGB / UNORM test override)でtarget plan /
  output pipeline fingerprintが追従
- maintenance1対応deviceでpresent fence前に旧semaphoreを破棄しない
- 非対応fixtureで未証明semaphoreだけquarantineされる
- `device.waitIdle()` / `glfwWaitEvents()` がrecovery traceに出ない
- hot reloadとresizeを同frameに起こし、単一root CASまたはstale retry
- ASan / validation / repeated faultでtoken、epoch、registry leaseのleakなし

### 14.4 XR

- mirror window resize / minimize / surface-lost中もHMD frame countが継続
- mirrorはdrop reasonを出し、XR composition resultを変更しない
- recovery workerの失敗でmirrorだけdisable
- XR offのflat image / frame-plan byteを意図なく変えない
- Meta XR Simulatorと対象HMDでp95/p99 frame timeを記録し、
  mirror recovery前後にglobal idle由来のspikeがない

### 14.5 手動 platform gate

- Windows: RDP接続/切断、display mode変更、monitor移動、DPI変更
- Linux: X11 / Waylandでminimize・monitor移動
- 将来Quest standalone: Android surface lifecycle

未実行platformは「対応済み」とせず、status / reportにgate待ちと明記する。

## 15. 実装順

### WP215 — transactional output root / frame token

- `OutputCompileFacts`、typed encoding path、renderer top-level root
- immutable target candidate / all-or-nothing publication
- move-only `FrameTargetFrame`
- extent / format変更のtarget re-lowerとoutput pipeline追従
- 旧 extent bool / in-place RT recreate / implicit begin-end stateの削除
- A-F3 / A-F4 / A-F6 / A-F9を閉じる

### WP216 — nonblocking SwapchainEpoch

- swapchain-dependent objectのepoch集約
- zero extent / SUBOPTIMAL / OUT_OF_DATE state machineとcoalesce
- per-image present lifetime、maintenance1 present fence、
  base Vulkan quarantine
- `device.waitIdle()` / renderer内`glfwWaitEvents()` /
  `recoverSurfaceIfStale()`の削除
- XR mirror drop + maintenance worker
- A-F2 / A-F7 / A-F11 / A-F12 / A-F13を閉じる

### WP217 — SurfaceEpoch recreation

- bootstrap ownership分割、`WindowSurfaceFactory`
- permanent core surface / `getSurface()`の削除
- `SurfaceLostKHRError`を含むfresh surface recreation
- physical device / created queue support再検証
- `device_rebuild_required` / optional mirror disable
- RDP / display切替のplatform gate
- A-F5を閉じる

WP215〜217は順に実装する。各WPで旧APIを直接削除し、互換shimを追加しない。
WP217完了前に「surface lost対応済み」と表示しない。

## 16. Vulkan / GLFW 根拠

- [Vulkan swapchain semaphore reuse guide](https://docs.vulkan.org/guide/latest/swapchain_semaphore_reuse.html)
  — submit fenceとpresent semaphore lifetimeが別であること、per-image reuse、
  maintenance1 present fence
- [`VK_EXT_swapchain_maintenance1` proposal](https://docs.vulkan.org/features/latest/features/proposals/VK_EXT_swapchain_maintenance1.html)
  — present fenceとacquired image release
- [`vkCreateSwapchainKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkCreateSwapchainKHR.html)
  / [`VkSwapchainCreateInfoKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/VkSwapchainCreateInfoKHR.html)
  — native windowのactive swapchain制約とold swapchain retirement
- [`vkDestroySwapchainKHR`](https://docs.vulkan.org/refpages/latest/refpages/source/vkDestroySwapchainKHR.html)
  — acquired image operation完了とpresentation engine lifetime
- [GLFW Vulkan reference](https://www.glfw.org/docs/latest/vulkan_guide.html)
  — window surface factoryとcaller ownership
