# OpenXR 対応(v2.2 — 条件付き受理)— Quest 3 で VRM キャラを見る

対象読者: エンジン担当・VR コンテンツを作る人。
ステータス: v2.2 ドラフト(2026-07-27)。v1 は敵対レビュー
`docs/design_reviews/2026-07-17_openxr_review_codex.md`(以下「レビュー」)
で **Reject** — ①「既存フレームを view ごとに 2 回実行」は
`Renderer::render()` = 1 論理フレーム(history flip・temporal advance・
deletion 前進込み)である現実と両立しない ②Vulkan bootstrap を
OpenXR-aware にしないと session を作れない(runtime 指定 device)
③IFrameTarget の 1 acquire/1 present 契約は XR frame を表せない
④clock 二本立ての自己矛盾 ⑤pose 配送は L0/L1 に存在しない
⑥TAA off は graph variant 切替 — 等 9 blocker。
**v2 = レビュー §1〜11 の全訂正 + §12 の WP 分割を採用**。
各 WP の受け入れ条件はレビュー §12 の逐語ブロックが正
(本書は構造の説明・条件の原文はレビュー参照)。

## 0. ターゲット(維持)

MVP = **PCVR(Quest Link / Air Link)**。standalone は SA トラック
(backlog)。XR 層は standalone 再利用可。

## 1. 有効化契約(XR0 — レビュー §11)

- `PELICAN_WITH_OPENXR` = 既定 ON の private unit(OFF で
  source/symbol が成果物から消える。single-OFF smoke)
- **activation エッジ行列(再レビュー条件 2 / XR0-ACTIVATION-EDGE
  逐語)**: 通常 window 起動では `--xr off` = probe なし flat /
  `--xr auto` = runtime/system/graphics-binding 不在時に名前入り INFO を
  一回出して flat / `--xr on` = 同じ不在・不適合を**名前入り hard
  error**。headless・RPC・golden・replay では `off` = flat・`auto` =
  **discovery を行わず off に正規化**・explicit `on` =
  `--xr on is incompatible with <mode>` の名前入り hard error。
  `PELICAN_WITH_OPENXR=OFF` binary では `on` の error に build flag 名を
  含め、`auto` は INFO + flat。**配布 v1 は `pelican_cli dist-config
  <project> --with openxr` だけが preset へ ON を入れる明示入力**
  (省略時は配布 preset で OFF — runtime 既定 `xr=off` は不変)。
  compile ON/OFF × mode off/auto/on × window/強制 off driver の
  **表駆動 fixture** を XR0 gate に
- OpenXR-SDK は FetchContent **exact commit 固定**・不要 target OFF・
  private link

## 2. bootstrap の順序(XR1a — レビュー §1。v1 の最大の欠落)

**Vulkan 初期化の前に OpenXR discovery を行う**:

```
1. activation 評価(off/auto/on)
2. XR を試す場合: XrInstance → XrSystem →
   XR_KHR_vulkan_enable2 の graphics requirements
3. window/engine 必須 extension を merge して
   xrCreateVulkanInstanceKHR / xrCreateVulkanDeviceKHR。
   physical device は xrGetVulkanGraphicsDevice2KHR の返す device
4. runtime 不在・不適合 → GPU resource がまだ無い時点で一度だけ
   フラット bootstrap へ降格(session 後の GPU hot switch はしない)
```

## 3. session とループ(XR1b — レビュー §4)

- **gate は `session_running`**(xrBeginSession 成功〜xrEndSession)。
  state 数値比較で代用しない。FOCUSED は入力適格性のみ
- running 中は毎フレーム wait/begin/end を一組。
  `shouldRender == false` でも begin/end(zero layer)+ update 一回
- **clock は二本に分離**: `EngineTime`(シミュレーション —
  now/dt/frame_index、fixed-step/replay 契約不変。predicted time を
  `setTime()` に書かない = 毎フレーム temporal reset の罠を踏まない)と
  `XrDisplayTiming`(frame-local immutable —
  predictedDisplayTime/Period/shouldRender。xrLocateViews と
  render snapshot が消費)
- running 中は FramerateAdjust bypass。READY で begin・STOPPING で
  loop 停止 + end・LOSS_PENDING/EXITING は別 terminal path

## 4. renderer の論理フレーム/view 分離(XR2a.0 — レビュー §2。OpenXR 非依存)

v1 の「2 回実行」を撤回。**`Renderer::render()` を logical frame と
per-view execution に分離**する:

```
renderLogicalFrame(frame_target, RenderViewFamily[N]):
  論理フレーム開始を一回(reload/deletion/animation/reset/target acquire)
  共有シーン状態を一回 freeze
  family projection modifierを一回解決(jitterはXR policyで禁止)
  for each view:
    in-flight × view の上書き不能 FrameUBO slot を選択
    view 別 current/previous snapshot(view/proj/**camera_position** —
    レビュー §5: position も view 別。specular が読む)
    グラフを 1 回実行して view 出力へ
  submit/release/end を一回
  RT/object/morph/camera history を一回だけ advance
```

- **FrameUBO は `in_flight × view_count` slot**(dynamic offset か
  set 複数)— 単一 buffer 上書きの眼間汚染(レビュー §2)を封じる
- family IDは`$main`、eye IDは`$xr/0` / `$xr/1`としてframe間で安定させる。
  temporal matrixは添字でなくIDから引き、実行順変更時はhistory imageも安全にresetする
- グラフの node 定義は無改造(view-aware 化は XR2b)。
  **view_count=1 の既存経路は golden byte 一致**が gate
- この WP は OpenXR 非依存 — **実 Vulkan の synthetic stereo target**
  (offscreen 二眼)で検証できる(§8 のテスト三層の 2 層目)

## 5. XR composition target(XR2a.1 — レビュー §3)

- **分離案に確定(再レビュー条件 3 / XR2A1-TARGET-BOUNDARY 逐語)**:
  既存 `IFrameTarget` の 1 acquire/1 submit/present 契約は **flat 専用
  として不変**。XR は別の **`IXrCompositionTarget`** が
  `beginLogicalFrame / beginView(i) / endView(i) / endLogicalFrame`・
  stereo array swapchain・projection layer・単一 xrEndFrame を
  所有。renderer は target 非依存の logical-frame core から per-view
  context を受け取る(XR target を IFrameTarget の一実装として偽装
  しない)
- **swapchain 状態機械**: color と optional depth の各 swapchain を独立に
  `idle → acquired → waited → submitted → released` で追跡。
  `XR_TIMEOUT_EXPIRED` は同じ acquired image への wait 再試行
  (wait 成功前に release しない)。途中失敗時は成功済み swapchain を
  合法順で unwind し、未 submit image を参照する layer を渡さない。
  session が継続可能なら zero-layer xrEndFrame で閉じ、loss は
  generation teardown へ。**各段の失敗位置を protocol trace の
  表駆動 gate に**
- XR2a.1 の初期実装は view ごとの `arraySize=1` を二個使った。WP203c 以後の
  production 規範は **`arraySize=2` の color swapchain 一個**である。
  PRIMARY_STEREO の共通 extent を選び、color image を一回だけ
  acquire→wait→submit→releaseし、左右の projection view は同じ image の
  `imageArrayIndex=0/1` を参照する。これにより sequential layer rendering と
  一回の multiview rendering が同じ composition target を共有する
- release 時 layout は runtime 要求(desktop の PRESENT_SRC 遷移を
  流用しない)

## 6. 座標系と reference space(XR2a.2 — レビュー §5)

行列規範(RH・+Y up・-Z forward・m・quaternion xyzw — 既存外部規約と
一致):

```
world_from_stage = inverse(active_camera_view_at_frame_start)
stage_from_eye[i] = XrView.pose の rigid 行列
world_from_eye[i] = world_from_stage * stage_from_eye[i]
view[i]           = inverse(world_from_eye[i])
camera_position[i]= translation(world_from_eye[i])
projection[i]     = XrFovf からの非対称 RH_ZO
```

- active camera transform = **`world_from_stage`**(フラットの
  camera API は不変・XR adapter 内でのみ上式)
- reference space は **STAGE → LOCAL_FLOOR → LOCAL** の優先順。
  LOCAL fallback は floor 非保証を明示(get_status に選択 space と
  floor semantics)。LOCAL を無補正で STAGE と呼ばない

## 7. feature 方針と mirror(XR2a.3 — レビュー §7・§10)

- **TAA off = graph variant 切替**(jitter=0 では消えない —
  compose は起動時焼き込みのため)。**flat graph と XR graph
  (TAA/jitter/velocity/history/UI 除外)を起動時に両方 compile** し、
  logical frame 境界で選択。除外できない未知 history feature が
  あれば XR activation を名前入り拒否。XR 出入りの最初のフレームで
  temporal history reset(epoch 1 回)
- **mirror** = engine 所有の left-eye intermediate を window extent へ
  scale/letterbox → screen-space UI を window 座標で overlay →
  present する**別 sink**(XR image から copy しない・window stall で
  HMD ループを止めない optional sink)。window surface / swapchainの
  epoch交換、nonblocking drop、present retirementは
  [`design_wsi_epoch_recovery.md`](design_wsi_epoch_recovery.md)を正とし、
  mirrorから同期的なglobal idle / recoveryを起動しない
- legacy capture は XR 中 **名前入り reject**(v1)。headless/golden は
  常にフラット経路のみ

## 8. テスト三層(XR1c — レビュー §8)

1. **protocol fake**: `xrGetInstanceProcAddr` 解決込みの注入表。
   session 遷移・wait/begin/end・acquire/wait/release・zero-layer・
   loss/error の**呼び出し順序のみ**検査。fake VkImage を Vulkan へ
   渡さない
2. **Vulkan-backed synthetic stereo target**: OpenXR なしで実 offscreen
   二眼を注入し、logical-frame 分離・view 別 UBO・history 一回・
   layout/barrier を validation 付きで検査(XR2a.0 の主 gate)
3. **実機(Quest 3 Link)**: 手動 gate — レビュー §13 の追加項目込み
   (runtime 選択 GPU・focus loss・shouldRender=false・window
   minimize・Link 切断/再接続・TAA-on project・LOCAL fallback)

## 9. session 喪失と再生成(レビュー §9)

view configuration/format の再照会・swapchain generation 管理・
GPU 完了待ち→子リソース破棄→XrSwapchain 破棄→1 transaction 再作成・
LOSS_PENDING は同一 device で再作成可能な場合のみ retry・EXITING は
自動再開しない・INSTANCE_LOST/別 device 要求は v1 ではフラット降格。

## 10. 入力(XR3a/3b — レビュー §6)

- **XR3a(action/binding)**: project action set を attach 前に
  XrAction 化・active set stack を毎フレーム xrSyncActions・
  Touch suggested bindings 同梱・非 FOCUSED は inactive snapshot
  (error にしない)・aim/grip は **left/right subaction を公開名で
  区別**
- **XR3b(pose provider)**: 現行 L0/L1 に pose は流れない(evaluator
  が明示 skip・`Actions::pose()` は未実装 error)— **L1 を ordered
  event + typed pose sample の frame provider に改訂**(既存
  kbd/mouse/pad の直列化は不変)。**head は synthetic source**
  (xrLocateViews 由来 — XrAction ではないと明記)。`ActionPose` に
  orientation/position の valid/tracked 4 flag + source/space identity。
  pose の record/replay は v1 非対応(query/record 開始時に名前入り
  error で固定)
- `xrSyncActions` + pose locate は freeze_input/freeze_actions の前に
  一回

## 11. WP 分割(レビュー §12 逐語 — 各条件の原文はレビュー参照)

| WP | 内容 | 逐語条件 |
|----|------|---------|
| **XR0** | build unit / activation 三値 | XR0-BUILD-ACTIVATION |
| **XR1a** | OpenXR discovery + Vulkan bootstrap | XR1A-VULKAN-BOOTSTRAP |
| **XR1b** | session_running + loop/display timing | XR1B-SESSION-LOOP |
| **XR1c** | protocol fake seam | XR1C-TEST-SEAM |
| **XR2a.0** | renderer logical-frame/view 分離(OpenXR 非依存) | XR2A0-LOGICAL-FRAME |
| **XR2a.1** | Xr composition target / swapchain | XR2A1-SWAPCHAIN |
| **XR2a.2** | view snapshot / reference space | XR2A2-VIEW-SPACE |
| **XR2a.3** | feature policy + mirror | XR2A3-FEATURE-MIRROR |
| **XR3a / XR3b** | action set / pose provider | XR3A-ACTIONS / XR3B-POSE |
| **XR4** | VRM キャラデモ(XR2a.*・XR3 後) | 実機チェックリスト(§8-3) |
| XR2b | multiview(XR2a.0 の契約を保ったまま view 次元) | flat byte 一致 + GPU 計測改善。**「2 回 render」を互換契約として残さない** |

**逐語添付の規律(再レビュー条件 1)**: 上表のタグは索引であり、
**WP 登録時には両レビュー(初回 §12 + 再レビューの
XR0-ACTIVATION-EDGE / XR2A1-TARGET-BOUNDARY)の該当ブロック全文を
implementation_plan へコピーする**。タグ参照だけで受け入れ条件を
満たしたとみなさない。WP を再分割した場合も各条件の所有 WP を本文で
一意にし、証跡リンクなしに完了扱いしない。

実装順: XR0 → XR1a → XR1b/1c → **XR2a.0(最重量・OpenXR 非依存なので
早期着手可)** → XR2a.1/2a.2 → XR2a.3 → XR3a/3b → XR4 → XR2b。

### XR2b進捗(2026-07-26)

WP203aでtarget-planning側を先行実装した。logical XR policyはexact 2-viewのまま、
後段がendpoint capability / 最大view数 / scope implementation対応から
single-view、sequential、multiviewまたは混在executionを選ぶ。scopeのview maskと実行回数、
image resourceのshared/sequential/2D-array layoutは`VulkanTargetPlan`へ残る。
`xr.view_execution`は`auto` / `sequential` / `multiview`を受理し、必須条件を満たさない
`multiview`はfallbackせずcompile errorになる。

WP203bでは、internal array imageとlayer別/2D-array view、per-view FrameUBO、
`gl_ViewIndex` reflection、typed pipeline/pass view contract、dynamic renderingの
`viewMask`を実装した。phase 2でengine fullscreen shaderのmultiview variant、
shared 2D / sequential 2D / layered 2D-array input descriptor、node-major mixed-scope
scheduler、`ILogicalFrameTarget`のview-family command境界まで接続した。

production capabilityはbuiltin fullscreen実装と最終SPIR-V reflectionの両方でgateする。
未対応material/custom passは逐次scopeに残り、per-frame computeとview-independent
anchorは一回だけ実行する。synthetic Vulkan fixtureはruntime compilerがphysical planから
layered sampler/pipelineを生成する経路と、一回のmultiview描画がsequential referenceの
左右layerとbyte一致することを検証する。

WP204のtile-local runtime接続後は、`same_pixel` fullscreen consumerを持つ
`tile_local_attachment`も同じview contractを使う。pure planner fixtureは融合scopeが
2-layer、`viewMask=0b11`、一回実行のmultiview planを維持することを検証し、synthetic
Vulkan fixtureは実際のlayered input attachment local readを検証する。OpenXR protocol fakeへ
fake VkImageを渡さない三層規律は変えず、現composition targetとのMeta XR Simulator /
物理HMD統合と対象tile GPUの帯域計測は外部gateに残す。

WP203cでは OpenXR target を 2-layer color array swapchain へ移し、
`imageArrayIndex=0/1` と一回の acquire/wait/release を view-family 経路へ接続した。
`XR_KHR_composition_layer_depth` が利用でき、compiled graph の typed external depth
export と runtime/Vulkan の format・extent・usage 条件が一致する場合は、2-layer depth
swapchain を作成する。renderer は sequential resource なら layer ごと、multiview resource
なら array 全体を copy し、各 projection view に
`XrCompositionLayerDepthInfoKHR` を chain する。extension/format/extent が不適合なら
depth image を参照せず color-only で提出する。

`xr.view_execution: "auto"` は capability gate に加えて
`xr.multiview_auto` の device profile を使う。profile は Vulkan
`vendor_id` を必須、`device_id` / `driver_version` / `device_name_contains` /
compiled `graph` を任意の絞り込みとして、別 run で測った
`sequential_gpu_ms` / `multiview_gpu_ms` / `sample_count` / `source` を保持する。
一致 profile が無い device は optimize-by-default で multiview、一致 profile がある場合は
実測 gain が `minimum_gain_percent` 以上のときだけ multiview となる。同じ specificity
なら宣言順、より具体的な profile は宣言順によらず優先する。required `multiview` と
明示 `sequential` はこの性能選択を上書きするが、required mode の capability 検査は残る。

選択証跡は physical target plan の
`view_execution_plan.auto_gate` に device identity、graph、profile、measurement、gain、
reason として残る。GPU 実測値は
`get_status.gpu_timing.logical_frame_averages` から取得できる。実装証跡と未完了の外部 gate は
[`design_reviews/2026-07-26_wp203c_implementation_report.md`](design_reviews/2026-07-26_wp203c_implementation_report.md)。
従来段階の詳細は
[`design_reviews/2026-07-25_wp203a_report.md`](design_reviews/2026-07-25_wp203a_report.md)。
phase 1の中間証跡は
[`design_reviews/2026-07-25_wp203b_phase1_report.md`](design_reviews/2026-07-25_wp203b_phase1_report.md)。
phase 2の証跡は
[`design_reviews/2026-07-26_wp203b_phase2_report.md`](design_reviews/2026-07-26_wp203b_phase2_report.md)。

## 12. 未決事項

1. シミュ時刻と predictedDisplayTime の補間(§3 の分離を前提に、
   interactive XR での update 位相)— XR1b 実装所見で
2. 深度 submit の Simulator/物理 HMD 実証 — 実装と protocol fake は WP203c で済み
3. world-space UI パネル — UI トラック合流点
4. standalone = SA トラック(backlog)
5. haptics — XR3 の余力次第
