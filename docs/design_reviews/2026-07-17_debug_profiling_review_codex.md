# デバッグ・プロファイリング・最適化トラック v1 敵対レビュー

対象: `docs/design_debug_profiling.md` v1 (2026-07-17)

根拠は 2026-07-17 時点の現行ツリー、WP29/WP40/WP133/WP137 の設計・実装・完了報告、
および末尾の一次資料との照合による。重大度は S0 = 現状の記述どおり実装すると値または
運用契約を誤る、S1 = 着手前に設計・受け入れ条件へ追加必須、S2 = 診断品質を固定する補強、
とする。

## 判定

**条件付き Accept。現状の WP 文面のままの着手は Reject。**

「常設の軽量計測」「表示の分離」「外部 profiler の opt-in」「計測してから最適化」という
方向は妥当である。一方、次の条件は任意の改善ではない。設計本文と WP へ反映されるまで、
D-P0〜D-P6 の実装・マージは承認しない。

1. **C1 — D-P0 を debug-utils 基盤/command label と resource coverage の二つに分割する。**
   「名前の正本は既に全部ある」は削除し、対象 object 種別、命名元、fallback、生成・再生成・
   破棄経路を inventory にする。D-P1 が依存するのは前者だけとする。
2. **C2 — RenderDoc は注入済み module の受動取得だけに限定し、capture の状態機械と境界を
   規範化する。** `LoadLibrary` しない。RPC は既存 `render_frame` と同じ current-time render を
   一回だけ capture し、実際に増えた capture index から `.rdc` path を得る。F11、RPC、
   overlapping request、失敗 code、flat/headless/XR の境界を固定する。
3. **C3 — D-P2 のキーを pass 名だけにしない。** `graph variant + logical frame + view index +
   node ordinal/kind/name + subrange` を identity とし、左右眼を混ぜない。barrier、render、compute、
   anchor、output transform、mirror のどこを含むかを宣言し、query pool は in-flight ring で再利用する。
4. **C4 — VRAM と XR timing の用語を訂正する。** memory budget は heap ごとの driver budget と
   engine logical allocation/free-range を分離する。`shouldRender=false` を dropped frame と数えず、
   portable な `XR_FRAME_DISCARDED`、mirror drop、vendor metric を別 counter にする。XrTime と QPC の
   差は `XR_KHR_win32_convert_performance_counter_time` が使える場合だけ計算する。
5. **C5 — 計測値と決定性成果物を分離する。** `get_status` の timing/memory は diagnostics として
   RPC 二回一致比較から正規化除外する一方、simulation state と final RGBA8 は同一入力で厳密一致を
   維持する。`.rdc` 自体の byte 一致を決定性 gate にしてはならない。
6. **C6 — Tracy の build 契約を修正する。** `PELICAN_ENABLE_TRACY` に改名するか、
   `PELICAN_WITH_TRACY=OFF` を build-tier 規約の明示例外にする。「唯一の既定 OFF」は削除する。
   OFF 時の source/dependency/import/symbol 不在、dist preset の強制 OFF、ON-disconnected の hot-path
   cost、Tracy Vulkan が追加する timestamp/query pool を gate に含める。
7. **C7 — D-P5 を CI0/CI2/toolchain/dist-bake の境界で分割する。** MSVC ASan の定期実行、別
   Clang UBSan job、GPU 保証 runner 上の validation/golden、SPIR-V compile validation を一つの WP に
   混ぜない。GPU validation は WP137 の将来 CI2、dist-bake validation は B4 に依存させる。
8. **C8 — D-P6 を user-mode crash と Vulkan device loss に分割する。** crash handler 内で quill や
   JSON graph を走らせず、別 process の dump helper と平常時に pre-serialized した status/breadcrumb を
   使う。PDB/build-id、privacy/retention、RenderDoc crash handler との所有関係を成果物契約に含める。
   `VK_EXT_device_fault` は device feature enable と `VK_ERROR_DEVICE_LOST` 後の独立 artifact を要件化する。
9. **C9 — 1 WP = 1 branch/PR に収まる粒度へ分割する。** 最初の三つは
   `D-P0a(debug-utils/labels) → D-P1a(flat/headless capture) → D-P2a(stereo-safe GPU timing)`
   とし、resource 全面命名、memory、XR timing、XR capture、CI、crash/device-fault は後続 WP にする。

## 1. S1 — D-P0 は「名前を付ける」前に名前を運ぶ API が不足している

設計は pass、RT、buffer、pipeline、texture、descriptor set の論理名を列挙し、
「名前の正本は既に全部ある」とする (`docs/design_debug_profiling.md:22-37`)。pass と RT には正本が
あるが、全 object にはない。

- `BufferWrapper` は VMA buffer/allocation だけ、`ImageWrapper` は extent/format/mip/image/allocation
  だけを持つ (`src/core/vkcore/buf.hpp:7-10`, `src/core/vkcore/image.hpp:7-13`)。
- graphics/compute pipeline description と `PipelineRecord` に logical name はなく、shader id、defines、
  format、reflection と Vulkan handle だけである (`src/core/shader/pipelinefactory.hpp:18-46`,
  `src/core/shader/pipelinefactory.hpp:70-85`)。
- RT は name/role/format/extent/history を保持するため命名元になれる
  (`src/core/renderingpass/rendertargetcontainer.hpp:19-35`)。一方 descriptor set は material、fullscreen、
  frame resource 等に分散して生成される。例えば material の set は allocation 時点で owner 文字列を
  受け取らない (`src/core/material/materialcontainer.cpp:403-407`,
  `src/core/material/materialcontainer.cpp:769-805`)。
- 現在の通常 bootstrap が enable する instance extension は window 要求と portability だけで、XR 側も
  同様である (`src/core/vkcore/core.cpp:23-59`, `src/core/vkcore/core.cpp:71-126`)。

従って「全面付与」は一つの中 WP ではなく、少なくとも次の二段階にする。

### D-P0a に貼る受け入れ条件

- `VK_EXT_debug_utils` は instance extension 列挙後、存在するときだけ通常 bootstrap と
  OpenXR-selected bootstrap の両方で enable する。不在時は全 helper が no-op となり、起動結果を変えない。
- function pointer は `vkGetInstanceProcAddr` から一度解決し、object name/command label/queue label の
  capability を `get_status.debug_utils` に `available/enabled/reason` として返す。
- command label identity は
  `frame/<logical-frame>/graph/<flat|xr>/view/<index>/node/<ordinal>:<kind>:<name>` とする。
  node label は incoming barrier の前から node body の後まで、nested `barriers` と `body` を持つ。
  render/compute/anchor/output-transform の全 node が plan order と同じ順で現れることを fake-dispatch test で
  検証する。
- extension 不在、feature off、dist preset の三経路で、描画 byte、frame plan、replay state を変えない。

### D-P0b に貼る受け入れ条件

- `VkBuffer/VkImage/VkImageView/VkPipeline/VkPipelineLayout/VkDescriptorSet/VkDescriptorSetLayout/
  VkSampler/VkQueryPool/VkSwapchainKHR` ごとに、create/recreate/destroy site、owner、命名式、fallback を
  inventory 表にする。coverage は「全 resource」という文章ではなく、inventory の分子/分母で gate する。
- name は user path、秘密情報、pointer/address を含めず、同一入力で安定する logical id を優先する。
  正本がない object は `<type>/<owner-kind>/<stable-handle-or-ordinal>` とし、fallback 使用数を report する。
- resize、hot reload、history ping-pong、XR 二眼 swapchain の再生成後にも新 handle へ name を再付与する。
- validation message の object handle に name が含まれることは副次効果であり、D-P5 の error-count gate の
  代替にはしない。

Khronos の debug-utils 例も、instance extension の enable と function pointer 解決を前提にする
([VK_EXT_debug_utils guide](https://docs.vulkan.org/guide/latest/extensions/VK_EXT_debug_utils.html))。

## 2. S0 — D-P1 は RenderDoc の API 契約と Pelican の frame 境界をまだ接続していない

設計の「注入時のみ DLL を掴む、link 依存なし」は方向として正しい
(`docs/design_debug_profiling.md:39-49`)。ただし RenderDoc の公式 API は、Windows では注入済み
`renderdoc.dll` を `GetModuleHandle` で受動確認することを推奨し、`TriggerCapture` は「次に present される
frame」、`StartFrameCapture/EndFrameCapture` は caller が指定した境界を capture する。Vulkan の
device pointer は `VkInstance` 自体ではなく dispatch-table pointer である。wildcard が複数候補に一致した
場合の選択は未定義である
([RenderDoc in-application API](https://github.com/baldurk/renderdoc/blob/v1.x/docs/in_application_api.rst))。
この header/API は注入手段ではなく、RenderDoc が Vulkan hook/layer を process 起動時から持つ場合の
control plane にすぎない。application が `VK_LAYER_RENDERDOC_Capture` を通常 layer として要求する設計に
すり替えてはならず、RenderDoc UI/command line から launch/attach して Vulkan instance 作成前に注入する
外部手順を gate に含める必要がある。

Pelican の headless RPC には present 境界がない。`step_frame` は time advance + update + render、
`render_frame` は current time の sequence update + render である
(`src/core/communication/rpcserver.cpp:874-889`)。さらに RPC/headless/replay の `--xr auto` は discovery を
行わず flat へ正規化する既存契約である (`docs/design_openxr.md:27-33`)。従って RPC integration test を
XR 実機 capture の証明には使えない。

XR は一論理 frame の中で二 view を順に submit し、両 submission 完了後に一回 `xrEndFrame` する
(`src/core/vkcore/renderer.cpp:1135-1200`, `src/core/openxr/openxrcompositiontarget.cpp:611-668`)。
RenderDoc が公式に掲げる対象 API は Vulkan 等であり OpenXR API そのものではない
([RenderDoc repository](https://github.com/baldurk/renderdoc))。従って、これは一次資料からの**推論**だが、
取得できるのは application process の Vulkan work であり、runtime compositor/reprojection の最終 HMD 像を
capture したとは言えない。

### D-P1a に貼る受け入れ条件

- pinned RenderDoc tag/commit の `renderdoc_app.h` だけを vendor し、binary/import library は同梱・link
  しない。Windows は `GetModuleHandleA("renderdoc.dll")` + `GetProcAddress("RENDERDOC_GetAPI")` のみ。
  `LoadLibrary` は禁止し、Vulkan 初期化後に RenderDoc を遅延 load しない。RenderDoc launch/attach が
  外部の Vulkan capture layer/hook を instance 作成前に注入し、in-app API はその事実を検出・操作するだけとする。
- 状態を `unavailable / idle / armed / capturing / completing / failed` とし、同時 F11/RPC、二重 start、
  shutdown 中 capture を deterministic error で拒否する。`IsFrameCapturing` と engine state の不一致も
  error にする。
- flat window は engine の F11 handler が一回の logical frame を明示 `Start/EndFrameCapture` で囲む。
  RenderDoc 自身の key binding と engine F11 の二重所有を禁止する。ImGui button は同じ request queue を
  呼ぶだけとし、`PELICAN_WITH_IMGUI=OFF` でも F11 path が動く。
- `capture_gpu_frame` は pending transform を flush し、既存 `render_frame` と同じ current-time
  `seq_player.update(now) + renderer.render()` を一回だけ行う。EngineTime/frame index は進めない。
  request 前後の `GetNumCaptures` を比較し、成功した新 index を `GetCapture` の二段呼び出しで取得して、
  canonical absolute `.rdc` path と frame index を返す。template path を成功 path とみなさない。
- injected でない場合は JSON-RPC application error に `renderdoc_not_injected` を含める。API version 不一致、
  capture busy、`EndFrameCapture==0`、capture count 不増加、path/file 不在を別 code/reason にする。
- unit test は fake RenderDoc table で全遷移と失敗を検査する。GPU runner の integration test は実 RenderDoc
  注入で headless/flat の `.rdc` が非空かつ replay-open 可能、D-P0a の node label が plan order で存在、
  final RGBA8 が非注入時と一致することを検査する。`.rdc` の byte 一致は要求しない。

### D-P1b (XR capture) に貼る受け入れ条件

- capture scope を `xr-app-frame` と呼び、既定では `renderer.renderLogicalFrame` の二眼 Vulkan submit と
  一回の `xrEndFrame` までを含め、mirror present は含めない。`include_mirror=true` は別 scope/結果として
  明示し、HMD compositor output を capture したとは表示しない。
- test matrix を (1) fake OpenXR dispatch = API 状態機械のみ、(2) Vulkan synthetic stereo target =
  二眼 command stream、OpenXR runtime なし、(3) Quest 3 Link/Air Link + Meta runtime + 実 RenderDoc = 手動
  device gate、と明記する。既存 synthetic test も「runtime/device discovery は行わない」
  (`test/golden_image_test.cpp:3794-3820`)。この二つを「XR simulator 実機相当」と呼ばない。
- 実機 gate は一つの `.rdc` に left/right の view label と全 node が各一回、resource name、capture 成功、
  HMD session 継続、mirror 有無の宣言を report する。PCVR が対象であり standalone Quest は対象外
  (`docs/design_openxr.md:17-20`)。

## 3. S0 — D-P2 の現 timing は XR 二眼を一つの pass 名へ混ぜる

WP29 実装は feature 有効時だけ query pool を作り、plan node ごとに二 query を置くという最小契約を
満たす (`docs/implementation_plan.md:630-646`)。compute node も実際に計測される
(`src/core/vkcore/renderer.cpp:589-626`)。しかし v2 の「階層 timing」と「120 frame history」をこのまま
足すのは危険である。

- 一 view ごとに `beginGpuFrame` し、その都度新しい `vk::UniqueQueryPool` を作る
  (`src/core/vkcore/rendertiming.cpp:37-59`)。
- 結果 identity は `pass_name` 一個で、左右眼も同名 map へ加算される
  (`src/core/vkcore/rendertiming.cpp:97-103`, `src/core/vkcore/rendertiming.cpp:137-172`)。
- `last_frame_node_names/query_count` も一組だけなので、XR では後から記録した view が前の view を上書きする
  (`src/core/vkcore/rendertiming.hpp:40-50`)。
- incoming barrier は start timestamp より前で、anchor が内部で行う sprite work は anchor 本体へ含まれる
  (`src/core/vkcore/renderer.cpp:597-607`, `src/core/vkcore/renderer.cpp:627-680`)。XR mirror intermediate は
  pass timing 終了後に記録されるため対象外である
  (`src/core/vkcore/renderer.cpp:747-752`, `src/core/vkcore/renderer.cpp:1190-1198`)。
- 現 test は node 名、query 数、回収完了を確認するだけで、同一 scene の timing on/off byte 比較ではない
  (`test/golden_image_test.cpp:985-1000`, `test/golden_image_test.cpp:4102-4106`)。

### D-P2a に貼る受け入れ条件

- sample identity は ordered struct
  `{logical_frame, graph_variant, view_index, node_ordinal, node_kind, node_name, subrange}` とする。
  aggregation key に display name だけを使わない。flat は view 0、XR は left/right を別 series とし、
  logical-frame total は二眼の単純和か critical path かを名前に含める。
- node に `barriers` と `body` を設ける。render、compute、anchor、output-transform を同じ ordered schema で
  出し、no-op anchor は `body=0/unsupported` を区別する。mirror intermediate と desktop mirror は
  graph 外 operation として別 series にする。
- query capacity は graph compile 時に算出し、`frames_in_flight × max_views` の pool/range を再利用する。
  steady state で `vkCreateQueryPool` が frame ごとに呼ばれず、pending queue が無制限に増えないことを
  counter/test で保証する。graph 拡大時だけ安全に再生成する。
- `get_status.gpu_timing` は `schema_version/enabled/supported/reason/history_capacity/history_count/
  dropped_samples/views/nodes` を持つ。120 frame ring は固定 capacity、snapshot serialization は render
  thread の live container を反復しない。
- flat render node、compute node、anchor 内 sprite、XR left/right、mirror の fixture を置く。同一 seed/input
  の timing off/on を各二回実行し、final RGBA8 と replay capture byte を厳密比較する。query count と
  series identity も exact に検査する。

## 4. S0 — memory budget と XR の「dropped」は異なる量を混ぜている

### 4.1 memory/VRAM

設計は `VK_EXT_memory_budget + 自前 allocator 統計` を一つの「VRAM 予算」と呼ぶ
(`docs/design_debug_profiling.md:57-63`)。現 bootstrap の device extension は swapchain だけで、VMA
allocator flag も設定していない (`src/core/vkcore/core.cpp:260-324`, `src/core/vkcore/core.cpp:337-343`)。
allocator は `VulkanManageCore` の private member で status API もない
(`src/core/vkcore/core.hpp:25-67`)。

VMA は、extension を device creation で enable した上で
`VMA_ALLOCATOR_CREATE_EXT_MEMORY_BUDGET_BIT` を指定した場合だけ system budget を使う
([VMA initialization](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/group__group__init.html))。
`VmaBudget::usage` は process の推定使用量であり、VMA の `statistics.blockBytes` とも一致せず、budget は
OS/driver が決める heap ごとの動的値である
([VmaBudget](https://gpuopen-librariesandsdks.github.io/VulkanMemoryAllocator/html/struct_vma_budget.html))。
integrated GPU もあるため単一の「VRAM 使用量」に畳んではならない。

### D-P2b に貼る受け入れ条件

- `VK_EXT_memory_budget` を optional device extension として通常/XR device creation の両方へ渡し、
  enable 成功時だけ VMA flag を立てる。`vmaSetCurrentFrameIndex` を論理 frame ごとに更新する。
- `get_status.memory.heaps[]` は `heap_index/device_local/size/usage/budget/source` を返し、source は
  `vk_ext_memory_budget` または `vma_estimate` とする。unsupported を 0 byte と偽装しない。
- `get_status.memory.engine_categories[]` は heap budget と別に、category、allocated bytes、logical used
  bytes、free bytes、high-water、object count を返す。mega-buffer は free-range から used/free を算出する。
  現在 exposed なのは test 用 count だけなので (`src/core/model/vertbufcontainer.hpp:45-75`,
  `src/core/model/vertbufcontainer.hpp:104-106`)、byte accounting の owner を追加する。
- texture/RT は format、extent、mip、history ping-pong を考慮した allocation size を VMA allocation info から
  記録し、画像の画素数×format の概算を driver heap usage と混ぜない。resize/reload/free の増減を fixture で
  検査する。

### 4.2 XR timing

`XrFrameState::shouldRender` は runtime が app に描画を求めるかであり、false は visibility/session 状態や
system UI によっても起こる。dropped frame ではない
([XrFrameState](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrFrameState.html))。
また `xrBeginFrame` は `XR_FRAME_DISCARDED` と `XR_SESSION_LOSS_PENDING` を成功 code として返し得る
([xrBeginFrame](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrBeginFrame.html))。現実装は
`XR_FAILED(result)` だけを見て両者を通常成功として捨てる
(`src/core/openxr/openxrsession.cpp:279-290`)。

`XrTime` は runtime time domain であり `steady_clock` と直接引けない。Windows では optional
`XR_KHR_win32_convert_performance_counter_time` の conversion を各 sample で使う必要がある。現 discovery が
enable するのは Vulkan enable2 と local-floor だけである
(`src/core/openxr/openxrdiscovery.cpp:98-128`)
([xrConvertTimeToWin32PerformanceCounterKHR](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrConvertTimeToWin32PerformanceCounterKHR.html))。

### D-P2c に貼る受け入れ条件

- `should_render_false_count/rate`、`begin_frame_discarded_count`、`session_loss_pending_count`、
  `mirror_presented/dropped/failures` を別名で保持する。mirror は既に独立 drop counter を持つ
  (`src/core/openxr/openxrmirrorsink.cpp:129-154`, `src/core/openxr/openxrmirrorsink.cpp:197-203`)。
  portable な `hmd_dropped_frames` は提供しない。vendor extension で得る場合は vendor/source/availability を
  field 名に含める。
- `SessionRuntime::beginFrame` は qualified success を caller へ返し、`XR_FRAME_DISCARDED` の frame では
  heavy GPU work/layer submit を行わず、frame order と terminal path を維持する fake-runtime test を置く。
- QPC conversion extension が advertised のときだけ enable/resolve し、`after_wait_margin_ms`、
  `before_submit_margin_ms`、`after_end_frame_margin_ms` を同じ predicted display time に対して計る。
  extension 不在時は `available=false/reason` とし、異なる clock の subtraction を行わない。
- Quest 3 Link 実機 report は runtime 名/version、refresh period、sample count、各 counter、margin の
  median/p95/min、shouldRender=false/session loss の操作手順を残す。これらは観測値であり simulation や
  render policy への feedback に使わない。

## 5. S1 — D-P3 の +1% と replay 不変は測定手順がなければ gate にならない

現 `CpuFrameDurations` は update/render/present-wait の三値だけで、`gpu_timing` feature が有効な場合だけ
記録される (`src/core/vkcore/rendertiming.hpp:14-18`, `src/core/appflow/loop.cpp:150-165`)。D-P3 はこれを
system/animation/record/submit 等へ広げて常設化するため、単なる表示追加ではない
(`docs/design_debug_profiling.md:65-75`)。

決定性規約は wall clock を**判定**に使わないことであり、観測すること自体を禁じてはいない
(`docs/design_determinism_services.md:34-39`)。しかし現在の RPC 二回一致 gate は startup diagnostics だけを
mask する (`test/run_rpc_headless.cmake:211-217`, `test/run_rpc_headless.cmake:271-275`)。timing/memory を
`get_status` に足すだけでは transcript が毎回変わる。

### D-P3 に貼る受け入れ条件

- ring entry は preallocated fixed-capacity POD とし、hot path で allocation、format、map lookup、log、lock を
  行わない。scope name は compile/registration 時に intern し、system 名の dynamic string copy を frame ごとに
  行わない。overflow は deterministic drop counter とし simulation state を変えない。
- timing/memory/renderdoc fields は `get_status.diagnostics` 配下へ置き、RPC schema test では存在・型・有限値を
  検査した後、二回一致比較では subtree を `<measured>` に正規化する。seed/frame/time/input/reload 等の既存
  simulation state は正規化しない。input recording/replay file と golden fixture に観測値を書かない。
- final RGBA8 は同一 seed/input で instrumentation 前後および二 replay が byte exact。既存 replay gate も
  recording/replay の PNG byte を比較している (`test/run_input_record_replay_headless.cmake:88-96`)。
- +1% gate は Release、固定 project/seed/frame count、同一 machine/電源設定、warm-up、前後を交互にした
  11 組以上で行い、CPU update+record の median と p95 を保存する。median の増加が 1% 未満で、p95 の
  95% bootstrap confidence interval が 1% を跨ぐ場合は「合格」とせず追加 sample を取る。GPU present/wait は
  CPU scope overhead の母数に入れない。実装報告へ raw sample と commit/build option を添付する。

## 6. S1 — D-P4 は build-tier 規約と Tracy GPU の実装事実に反する

設計は `PELICAN_WITH_TRACY=OFF` を「唯一の既定 OFF ユニット」とする
(`docs/design_debug_profiling.md:77-85`)。しかし build-tier 規約は全 unit 既定 ON とする
(`docs/design_build_tiers.md:41-49`, `docs/implementation_plan.md:727-750`)。現 CMake にも既定 OFF の
`PELICAN_WITH_JOLT_PHYSICS` と instrumentation option の `PELICAN_ENABLE_ASAN` がある
(`CMakeLists.txt:22-50`)。従って「唯一」は事実でなく、tool instrumentation を feature unit と同じ名前空間に
入れるかも未決である。

また Tracy の header macro は `TRACY_ENABLE` 不在時に空になるが、Vulkan zone は独自 query pool、timestamp、
result collect を持つ。D-P0 の debug label をそのまま「bridge して専用計測を書かない」は成立しない。
共有できるのは placement/name inventory で、計測機構ではない
([Tracy.hpp](https://github.com/wolfpld/tracy/blob/v0.13.1/public/tracy/Tracy.hpp),
[TracyVulkan.hpp](https://github.com/wolfpld/tracy/blob/v0.13.1/public/tracy/TracyVulkan.hpp))。

### D-P4 に貼る受け入れ条件

- 推奨名は `PELICAN_ENABLE_TRACY`、既定 OFF とする。`PELICAN_WITH_TRACY` を維持するなら
  `docs/design_build_tiers.md` に「外部 profiler instrumentation は全 ON 規約の例外」と逐語で追加する。
- Tracy は exact tag/commit を pin し、ON のときだけ fetch/build/link する。`TRACY_ENABLE` と採用する
  `TRACY_ON_DEMAND` 等の option を明記し、client/server version を同じ tag に固定する。
- engine-owned `PELICAN_CPU_ZONE/PELICAN_GPU_ZONE/PELICAN_FRAME_MARK` wrapper を唯一の call site にする。
  OFF build は Tracy header を include せず、macro 引数も評価しない。ON の GPU zone は D-P2 query とは別の
  timestamp/query pool を作ることを設計と overhead report に明記する。
- OFF smoke は `_deps/tracy*` 不在、build metadata に Tracy source 不在、binary import 不在、
  `dumpbin /symbols` の `Tracy|___tracy` 不在を検査する。現 dist preset は既知 flag と ImGui しか出さないため
  (`src/devcli/distconfig.cpp:884-908`)、Tracy OFF の明示行と dist-config test を追加する。
- ON smoke は profiler 未接続/接続の両方で起動・終了し、network thread shutdown が hang しないことを
  検査する。1,000,000 empty CPU zones、代表 frame、D-P2+Tracy GPU zones 併用の median/p95、query 数、
  binary size、idle memory を実装報告に残す。default OFF だからと hot macro cost を未測にしない。

## 7. S0 — D-P5 は「常設化済み部分」と「存在しない CI/B4」を一つにしている

設計は validation golden が現在手動、D-P5 は依存なしとする
(`docs/design_debug_profiling.md:87-95`, `docs/design_debug_profiling.md:122-136`)。現実には Debug の通常/XR
instance は validation layer を常に request し (`src/core/vkcore/core.cpp:33-49`,
`src/core/vkcore/core.cpp:94-115`)、ローカル full CTest では GPU 63 case を含む golden/player が既に自動実行
されている (`docs/design_reviews/2026-07-17_wp137_report.md:63-77`)。不足は GitHub 上の GPU runner である。

WP137 の CI0 は意図的に `ctest -LE gpu` であり、golden/player/validation は将来 CI2 と定義済み
(`docs/design_reviews/2026-07-17_wp137_report.md:5-20`,
`docs/design_reviews/2026-07-17_wp137_report.md:81-89`)。ここへ GPU gate を足すのは WP137 の selection
contract を壊す。

sanitizer も同様で、MSVC ASan option は既に存在する (`CMakeLists.txt:50-63`)。MSVC が現在公開する
`/fsanitize` は address 等で、undefined は将来候補である
([MSVC AddressSanitizer](https://learn.microsoft.com/en-us/cpp/sanitizers/asan?view=msvc-170))。
UBSan は Clang の別 toolchain/job として設計する必要がある
([Clang UBSan](https://clang.llvm.org/docs/UndefinedBehaviorSanitizer.html))。

最後に `dist-bake` は実装済み経路ではなく B4 の将来 WP であり、配布 v1 は shaderc ON のままである
(`docs/design_build_tiers.md:81-105`)。一方 SPIRV-Tools API による linked output の validation は既にある
(`src/core/shader/spvlink.cpp:565-594`)。対象を区別すべきである。

### D-P5a (CPU sanitizer) に貼る受け入れ条件

- 既存 `PELICAN_ENABLE_ASAN=ON` の Windows/MSVC configure/build と `ctest -LE gpu` を WP137 CI0 とは
  別 job/schedule で実行する。未許可 SKIP 0、sanitizer report 0、artifact 保存、retry なしを維持する。
- UBSan は Windows Clang/ClangCL の独立 configure を spike で成立させてから常設する。compiler/version、
  `-fsanitize=undefined` の check set、recover 方針、symbolizer、third-party suppression を固定する。
  MSVC job に存在しない `/fsanitize=undefined` を足さない。

### D-P5b (GPU validation/CI2) に貼る受け入れ条件

- 依存を `WP137-CI2 GPU runner` とする。`ctest -L gpu` の既存 label を唯一の selection boundary とし、
  GPU/validation layer 不在を SKIP allowlist に入れず runner misconfiguration として失敗させる。
- debug messenger の severity=error count と VUID を structured artifact にし、stderr grep だけに依存しない。
  全 GPU case の count=0、golden/player PASS、tracked golden diff 0 を一つの gate にする。
- D-P0/D-P1/D-P2 の capture/trace は failure artifact として保存するが、RenderDoc/Tracy が runner にないことを
  validation job 全体の SKIP 理由にしない。外部 profiler integration は別 job にする。

### D-P5c (SPIR-V) に貼る受け入れ条件

- runtime shaderc output、surface link output、prebuilt `.spv` を同じ pinned SPIRV-Tools validator API と
  target environment で検査する。validation failure は source/log/variant key を返し pipeline を publish しない。
- external `spirv-val.exe` の存在時だけ実行する現 test
  (`test/run_spvlink_golden.cmake:44-52`)を「全 compile 経路の常設」と数えない。
- dist-bake artifact 全件の validation は B4 の受け入れ条件へ置き、D-P5 の依存なし項目にはしない。

## 8. S0 — D-P6 の crash path は通常 runtime API を呼べる前提になっている

設計は「未処理例外 → minidump + log flush + 直近 get_status」を一つの中 WP とする
(`docs/design_debug_profiling.md:97-103`)。現 logger は asynchronous quill backend を start するだけで、
明示 flush/shutdown API や crash-safe breadcrumb store を持たない
(`src/core/log.cpp:15-28`, `src/core/log.hpp:8-10`)。`get_status` は scene、module graph、stores、reload、
startup 等からその場で JSON を組み立てる (`src/core/communication/rpcserver.cpp:670-721`)。heap/lock/破損状態が
不明な exception filter からこの graph を呼ぶのは安全でない。

Microsoft は、crash した target process 内からの `MiniDumpWriteDump` は loader deadlock 等を起こし得るため、
可能なら別 process、無理なら専用 thread を使うよう明記し、DbgHelp は single-threaded とする
([MiniDumpWriteDump](https://learn.microsoft.com/en-us/windows/win32/api/minidumpapiset/nf-minidumpapiset-minidumpwritedump))。
また `.dmp` だけでは build に一致する PDB がなければ診断できない。現 CMake は dist directory を定義するが
PDB/build manifest の保存規約を持たず (`CMakeLists.txt:18-21`, `src/player/CMakeLists.txt:1-28`)、
「三ファイルを送る」だけでは再現性・privacy の契約にならない。

`VK_EXT_device_fault` は Windows exception と別で、device extension/feature を enable し、実装が
`VK_ERROR_DEVICE_LOST` を返した**後**に `vkGetDeviceFaultInfoEXT` を呼ぶ API である
([VK_EXT_device_fault](https://docs.vulkan.org/refpages/latest/refpages/source/VK_EXT_device_fault.html))。
現 device create chain にその extension/feature はない (`src/core/vkcore/core.cpp:260-324`)。

RenderDoc も process 内 crash handler を持ち、in-app API に `UnloadCrashHandler` があるため、所有順を決めないと
相互干渉する
([RenderDoc in-application API](https://github.com/baldurk/renderdoc/blob/v1.x/docs/in_application_api.rst))。

### D-P6a (Windows crash artifact) に貼る受け入れ条件

- player 起動時に別 `pelican_crash_helper` process を開始し、target process handle、PID、artifact directory、
  build id を渡す。exception filter は preallocated packet に exception pointers/thread id/sequence を書いて
  helper を signal するだけとし、quill flush、JSON allocation、module lookup、Vulkan call を行わない。
- `get_status` 相当は平常時の安全点で immutable snapshot を bounded buffer へ pre-serialize する。crash 時は
  最後に complete だった snapshot と lock-free breadcrumb tail を helper が別ファイルへ書く。snapshot の
  schema/version、最大 size、truncate marker、秘密 path/入力の redaction を固定する。
- artifact set は `.dmp`, `.diagnostics.json`, `.log-tail.txt`, `manifest.json` とする。manifest は engine/game
  build id、git revision、UTC、exception code、dump flags、OS/GPU/driver、各 SHA-256、symbol package id を持つ。
  PDB はユーザー送付物ではなく build artifact store に build id で保存し、retention/access policy を定義する。
- RenderDoc 注入時は previous exception filter の chain と `UnloadCrashHandler` を呼ぶ/呼ばない policy を明記し、
  injected/non-injected の両 child-process crash test を置く。
- integration test は CTest 本体で crash せず child player を意図的に落とし、timeout 内に helper が終了、dump
  readable、known crash function が symbolicate、snapshot sequence と breadcrumb sentinel が一致することを
  検査する。単なる file existence と log 末尾文字列だけを合格条件にしない。

### D-P6b (Vulkan device fault) に貼る受け入れ条件

- `VK_EXT_device_fault` support と `VkPhysicalDeviceFaultFeaturesEXT::deviceFault` を query し、通常/XR の device
  create chain で両方成立したときだけ enable する。status は `available/enabled/reason` を持つ。
- queue submit、fence wait、present、query result 等の `VK_ERROR_DEVICE_LOST` を一つの recorder へ集約し、最初の
  device-lost 後に一度だけ counts→payload の二段 `vkGetDeviceFaultInfoEXT` を行う。vendor binary は
  `.gpu-fault.bin`、description/address/vendor info は `.gpu-fault.json` として crash artifact とは別に保存する。
- unsupported device、extension advertised/feature false、payload size 変更、vendor binary 0 byte、API failure を
  fake-dispatch test で検査する。実 device loss を通常 CI で無理に起こさず、vendor/device lab gate と synthetic
  fault test を分ける。

## 9. WP 粒度・依存・順序

現表は D-P2 に GPU hierarchy/history + memory + XR timing、D-P5 に三 toolchain/runner、D-P6 に process crash +
GPU fault をまとめる (`docs/design_debug_profiling.md:122-136`)。これはリポジトリの
「1 WP = 1 branch = 1 PR、bisect/revert を WP 単位にする」規約に反する
(`docs/implementation_plan.md:3643-3650`)。

採用すべき分割は次である。

| WP | 内容 | 依存 | 主 gate |
|---|---|---|---|
| D-P0a | debug-utils capability + frame/view/node labels | なし | fake dispatch、extension 不在 no-op、golden |
| D-P1a | RenderDoc passive API + flat/headless F11/RPC | D-P0a | real injected `.rdc` replay-open、label order、RGBA8 |
| D-P2a | stereo-safe GPU timing schema + 120-frame ring | WP29, D-P0a | render/compute/anchor、left/right、pool reuse、on/off |
| D-P0b | resource naming inventory/coverage | D-P0a | create/recreate coverage、RenderDoc resource names |
| D-P1b | XR app-frame capture | D-P1a, WP133, CI2/device gate | synthetic stereo + Quest Link manual matrix |
| D-P2b | heap budget + engine allocation accounting | D-P2a | extension/estimate、多 heap、resize/free |
| D-P2c | OpenXR margin/counters | WP133, D-P2a | fake qualified-success + Quest Link report |
| D-P3 | always-on CPU phase ring | なし（表示は D-P2a と統合可） | <1%、RPC normalization、golden/replay |
| D-P4 | Tracy opt-in integration | D-P0a, D-P3 | ON/OFF smoke、dist exclusion、hot cost |
| D-P5a | ASan + separate UBSan CPU jobs | WP137 CI0 | `-LE gpu`、report 0、SKIP policy |
| D-P5b | GPU validation CI2 | WP137 CI2 | `-L gpu`、VUID 0、golden/player |
| D-P5c | runtime/prebuilt SPIR-V validation | B4 は dist 部分のみ | validator corpus、publish-before-validate 禁止 |
| D-P6a | crash helper + symbol/artifact contract | なし | child crash + symbolication |
| D-P6b | Vulkan device-fault recorder | D-P6a の artifact schema のみ | fake fault + device lab |

最初の三連を維持するなら **D-P0a → D-P1a → D-P2a** までに限定する。D-P0b の全面 coverage を D-P1 の
前提にすると、小さい capture WP が resource 所有権の全面改修にブロックされる。D-P5 は「独立」ではなく、
CPU 部分は WP137 CI0、GPU 部分は将来 CI2、dist artifact 部分は B4 に明示依存する。

## 10. 受理できる点

- `gpu_timing` が未参照なら module/query pool を作らない WP29 の境界を維持する方針は正しい
  (`docs/implementation_plan.md:635-646`, `src/core/vkcore/renderer.cpp:133-175`)。
- timing を simulation/optimization decision へ feedback しない方針、golden/replay byte 不変を gate にする
  方針は正しい。ただし C5 の diagnostics normalization と paired on/off test が必要である。
- RenderDoc/Tracy を dist から除外し、ImGui/RPC 表示を additive にする三層分離は妥当である。
- OPT-S/XR/MV/CULL を D-P2/D-P3 の数値が出るまで着手しない順序は受理する。特に multiview は、現行の
  二眼逐次 submit と同じ scene/input に対する pass 別 CPU/GPU 値を baseline として保存してから比較すべきである。

以上の C1〜C9 と分割表を設計本文へ取り込めば、トラック全体は実装可能な条件付き Accept になる。
