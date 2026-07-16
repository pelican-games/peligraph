# OpenXR 設計 v2 再レビュー

レビュー日: 2026-07-17

対象: `docs/design_openxr.md` v2

照合 commit: `e66077ae8cf7`
方法: v2、前回レビュー、関連する build-tier 規約、OpenXR 1.1.61 の静的照合。依頼どおりビルド・実機実行・既存ファイル変更は行っていない。前回レビューが根拠にした renderer/input/loop 等の実装ファイルには、照合 commit `8884788418c8` から本 commit まで変更がないことも確認した。

## 判定

**条件付き**。

前回の Blocker 9 件は、実装方針としてはすべて正しい方向へ改稿された。特に、
runtime-selected Vulkan device を resource 作成前に確定する bootstrap、
一 logical frame 内の per-view execution、`in_flight × view` UBO、
二個の `arraySize=1` swapchain を一 projection layer／一 `xrEndFrame` へ載せる規範、
simulation/display clock 分離、view 別 `camera_position`、typed pose provider、
XR graph variant、三層テスト、session generation、mirror 別 sink は、v1 の Reject 理由を骨抜きにせず反映している
(`docs/design_openxr.md:33-172`)。

ただし、前回 Reject 解除条件だった「各 WP への逐語添付」は実行されず、識別子と旧レビュー参照だけになっている
(`docs/design_openxr.md:13-15`, `docs/design_openxr.md:174-188`)。加えて、activation の強制 off ケースと
`dist-config` 入力、および XR composition target の interface 二択が未確定である。以下の三条件を設計本文と
各 WP 本文へ逐語採用すれば Accept とし、中心アーキテクチャの再差し戻しは不要である。

## 残存条件（重要度順）

### 1. [Major] §12 の「逐語添付」がタグ参照へ弱められている

v2 は「各 WP の受け入れ条件はレビュー §12 の逐語ブロックが正」と宣言する一方、実際の WP 表には
`XR0-BUILD-ACTIVATION` 等の識別子しかない
(`docs/design_openxr.md:13-15`, `docs/design_openxr.md:174-188`)。前回の解除条件は、分割だけでなく、
fixture・failure unwind・byte 一致を含む引用ブロックを「逐語添付」することだった
(`docs/design_reviews/2026-07-17_openxr_review_codex.md:363-444`)。外部参照だけでは、WP 起票時に
`required extension merge/missing`、`partial failure unwind`、`epoch/object previous once` 等だけが落ちても
条件を満たしたように見えてしまう。

次を全 WP 共通規約として逐語添付すること。

> **WP-VERBATIM-GATE**: 「XR0、XR1a、XR1b、XR1c、XR2a.0、XR2a.1、XR2a.2、
> XR2a.3、XR3a、XR3b の各 WP 本文には、
> `docs/design_reviews/2026-07-17_openxr_review_codex.md:368-438` の同名引用ブロックを
> 省略・要約・タグ参照に置換せず逐語コピーする。XR4 と XR2b には同文書
> `:440-444` の順序・semantic image・flat byte 一致・GPU 計測条件を逐語コピーする。
> WP を分割した場合も、各受け入れ条件の所有 WP を本文で一意にし、証跡へのリンクなしに完了扱いしない。」

### 2. [Major] activation 三値の強制 off 行列と配布 preset 入力が閉じていない

通常起動の `off/auto/on`、既定 `off`、compile OFF、SDK pin は反映済みである
(`docs/design_openxr.md:22-31`)。しかし headless/RPC/golden/replay を「off 強制」とだけ書いたため、
explicit `--xr on` も黙って off にするのか、前回条件どおり hard error にするのかが一意でない
(`docs/design_openxr.md:26-29`, `docs/design_reviews/2026-07-17_openxr_review_codex.md:348-359`)。

また build-tier 設計では配布 preset が project と `--with ...` から unit を導出する
(`docs/design_build_tiers.md:51-67`)が、OpenXR を preset に含める入力が v2 にない。これは前回 Major で
明示した未解決点である
(`docs/design_reviews/2026-07-17_openxr_review_codex.md:342-359`)。

XR0 へ次を逐語添付すること。

> **XR0-ACTIVATION-EDGE**: 「通常 window 起動では `--xr off` は probe なしの flat、
> `--xr auto` は runtime/system/graphics-binding 不在時に名前入り INFO を一回出して flat、
> `--xr on` は同じ不在・不適合を名前入り hard error とする。headless、RPC、golden、replay では
> `off` は flat、`auto` は OpenXR discovery を行わず off に正規化、explicit `on` は
> `--xr on is incompatible with <mode>` の名前入り hard error とする。
> `PELICAN_WITH_OPENXR=OFF` binary では `on` の error に同 build flag 名を含め、`auto` は INFO + flat とする。
> 配布 v1 は `pelican_cli dist-config <project> --with openxr` だけが
> `PELICAN_WITH_OPENXR=ON` を preset へ入れる明示入力で、省略時は配布 preset で OFF とする。
> これは runtime 既定 `xr=off` を変更しない。compile ON/OFF × mode off/auto/on ×
> window/強制-off driver の表駆動 fixture を XR0 gate にする。」

### 3. [Major] XR composition target の interface 二択を XR2a.1 の実装前に一意化する

v2 は flat `IFrameTarget` の 1 acquire/1 present 契約を温存し、XR に logical-frame lifecycle を設ける点では
前回 blocker を解消している。しかし直後に「既存 interface の拡張」又は「`IXrCompositionTarget` 分離」を
XR2a.1 で決めるとしており、契約所有者がまだ二択である
(`docs/design_openxr.md:88-93`)。前回はこの二方式のどちらかを設計で明示するよう要求していた
(`docs/design_reviews/2026-07-17_openxr_review_codex.md:125-148`)。

v2 の「既存 `IFrameTarget` 契約を温存」と整合し、flat の byte 一致面積を最小にするため、分離案に固定する。
XR2a.1 へ次を逐語添付すること。

> **XR2A1-TARGET-BOUNDARY**: 「既存 `IFrameTarget` とその 1 acquire/1 submit/present 契約は
> flat 専用として変更しない。XR は別の `IXrCompositionTarget` が
> `beginLogicalFrame / beginView(i) / endView(i) / endLogicalFrame`、二個の
> `arraySize=1` color swapchain、projection layer、単一 `xrEndFrame` を所有する。
> renderer は target 非依存の logical-frame core から per-view context を受け取り、
> XR target を `IFrameTarget` の一実装として偽装しない。各 swapchain の状態を独立に
> `idle → acquired → waited → submitted → released` と追跡する。
> `XR_TIMEOUT_EXPIRED` は同じ acquired image への wait を再試行し、wait 成功前に release しない。
> 一方の acquire/wait/submit/release が失敗した場合は、成功済みの他方を合法な順序で unwind し、
> 未 submit の image を参照する projection layer は渡さない。session が frame call を継続可能な場合だけ
> zero-layer `xrEndFrame` で閉じ、session/instance loss は generation teardown へ送る。
> 左右それぞれの acquire、wait timeout、GPU submit、release 失敗位置を protocol trace の表駆動 gate にする。」

`xrWaitSwapchainImage` は timeout 後も同じ acquired image を次回 wait し、wait 成功後は release が必要である。
また `xrReleaseSwapchainImage` は wait 済み image がなければ call-order error になるため、この状態機械は仕様とも整合する
([xrWaitSwapchainImage](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrWaitSwapchainImage.html),
[xrReleaseSwapchainImage](https://registry.khronos.org/OpenXR/specs/1.1/man/html/xrReleaseSwapchainImage.html))。

## 前回指摘の反映表

| 前回指摘 | v2 判定 | 根拠 |
|---|---|---|
| §1 Vulkan bootstrap | **反映** | discovery を Vulkan 前へ移し、enable2 requirements、`xrCreateVulkan*`、runtime device、resource 前 downgrade を規範化した (`docs/design_openxr.md:33-46`)。前回要求は `docs/design_reviews/2026-07-17_openxr_review_codex.md:68-79`。個別 fixture は条件 1 で逐語復元する。 |
| §2 logical-frame/view 分離 | **反映** | 二回の `Renderer::render()` を撤回し、logical once、per-view snapshot、`in_flight × view` UBO、history once、flat byte 一致を規範化した (`docs/design_openxr.md:63-86`)。前回要求は `docs/design_reviews/2026-07-17_openxr_review_codex.md:107-123`。 |
| §3 composition target | **一部条件** | 二 swapchain、各 acquire→wait→submit→release、全 view を一 projection layer／一 `xrEndFrame`、runtime layout は反映 (`docs/design_openxr.md:88-99`)。interface 二択だけ条件 3 で閉じる。 |
| §4 clock/session gate | **反映** | `session_running`、FOCUSED の入力限定、zero layer、`EngineTime`/`XrDisplayTiming`、FramerateAdjust bypass、terminal path を分離した (`docs/design_openxr.md:48-61`)。前回要求は `docs/design_reviews/2026-07-17_openxr_review_codex.md:164-192`。 |
| §5 座標・camera position | **反映** | 行列式、RH/+Y/-Z/m/xyzw、asymmetric RH_ZO、view 別 `camera_position`、STAGE→LOCAL_FLOOR→LOCAL と status を明記した (`docs/design_openxr.md:101-119`)。前回要求は `docs/design_reviews/2026-07-17_openxr_review_codex.md:208-226`。fixture の逐語条件は条件 1 で復元する。 |
| §6 入力 | **反映** | action/binding と pose provider を分け、L1 改訂、synthetic head、左右 subaction、valid/tracked 四 flag、source/space identity、freeze 前一回を明記した (`docs/design_openxr.md:156-172`)。前回要求は `docs/design_reviews/2026-07-17_openxr_review_codex.md:245-258`。 |
| §7 TAA | **反映** | flat/XR graph を precompile し、TAA/jitter/velocity/history/UI を除外、未知 history を reject、境界切替と epoch 一回 reset を規範化した (`docs/design_openxr.md:121-128`)。前回要求は `docs/design_reviews/2026-07-17_openxr_review_codex.md:260-279`。 |
| §8 テスト三層 | **反映** | dispatch を含む protocol fake、real-Vulkan synthetic stereo、Quest 3 manual gate に分離した (`docs/design_openxr.md:136-147`)。前回要求は `docs/design_reviews/2026-07-17_openxr_review_codex.md:281-299`。 |
| §9 session loss | **反映** | view configuration/format 再照会、generation、GPU 完了後の順序破棄、一 transaction 再作成、LOSS_PENDING/EXITING/INSTANCE_LOST/別 device を分岐した (`docs/design_openxr.md:149-154`)。前回要求は `docs/design_reviews/2026-07-17_openxr_review_codex.md:313-322`。extension event 等の詳細は条件 1 で逐語復元する。 |
| §10 mirror/capture | **反映** | engine-owned intermediate から scale/letterbox + UI overlay する optional sink とし、HMD loop 非阻害、legacy capture reject、headless/golden flat を固定した (`docs/design_openxr.md:129-134`)。前回要求は `docs/design_reviews/2026-07-17_openxr_review_codex.md:324-340`。 |
| §11 activation/build | **一部条件** | compile unit、runtime 三値、既定 off、SDK pin/private link は反映 (`docs/design_openxr.md:22-31`)。強制-off explicit on と `dist-config` は条件 2 が必要。 |
| §12 WP 分割・順序 | **一部条件** | XR0/1a-c/2a.0-3/3a-b/4/2b への分割と統合順は採用 (`docs/design_openxr.md:174-191`)。逐語添付は条件 1 が必要。 |

## v2 の新規判断

### XR2a.0 を OpenXR 非依存として早期着手する点

**異議なし**。synthetic stereo target によって OpenXR runtime なしで renderer の最も高リスクな境界を先に検証できる
(`docs/design_openxr.md:63-86`)。これは protocol fake と real Vulkan renderer test を分ける前回方針とも一致する
(`docs/design_reviews/2026-07-17_openxr_review_codex.md:281-299`)。

ただし `XR0 → XR1a → XR1b/1c → XR2a.0` という一本の矢印は「開始順」に読むと早期着手と矛盾する
(`docs/design_openxr.md:190-191`)。WP 管理上は次の一文に置き換えるのが明瞭である（Accept 条件ではなく編集推奨）。

> 「XR2a.0 は XR0/XR1 に依存せず並行着手可。XR2a.1 の統合開始条件は
> XR1a、XR1b、XR2a.0 の契約確定、XR4 の開始条件は XR2a.0〜3 と XR3a/b の完了、XR2b は最後とする。」

### view ごとに `arraySize=1` の swapchain を二個選ぶ点

**異議なし**。左右で異なる recommended extent を自然に保持でき、各 projection view の `subImage` は独自の
swapchain を参照できる。`arraySize=1` は通常の 2D image として仕様上有効である
([XrCompositionLayerProjectionView](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrCompositionLayerProjectionView.html),
[XrSwapchainCreateInfo](https://registry.khronos.org/OpenXR/specs/1.1/man/html/XrSwapchainCreateInfo.html))。
v2 の「二 view を一 projection layer、一 `xrEndFrame`」も正しい
(`docs/design_openxr.md:94-99`)。増える acquire/wait/release と partial failure の状態数は条件 3 の state machine と
表駆動 trace で閉じればよい。

## 結論

v2 は v1 の中心仮説を実質的に置き換えており、前回と同じ理由での Reject は不要である。
残件は、既に選ばれたアーキテクチャを実装 WP で弱めないための受け入れ条件と境界の確定である。

1. 旧レビュー §12 の全文条件を各 WP へ逐語コピーする。
2. `XR0-ACTIVATION-EDGE` を採用する。
3. `XR2A1-TARGET-BOUNDARY` を採用し、分離案と failure unwind を固定する。

この三点が設計本文および該当 WP に入った時点で **Accept**。実装結果については、各 WP の CI／Quest 3 gate の
証跡で別途判定する。
