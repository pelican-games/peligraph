# 不要な render target の削減: 寿命の正確化と区間詰めによるメモリ共有

対象読者: エンジン担当(描画コンパイラ / renderer)。
ステータス: 改善案メモ(2026-10-16。議論の記録。WP 化・レビュー前)。段 0(計測)は RC-1 で済み(2026-10-23、`631caac21`。§5)。合計の数え方を実際の確保に合わせる RC-1b も済み(`504a6492a`。§5)。段 1 の寿命の部分(A)は RC-6c で済み(2026-10-30、`be795c9a5`。[§2 の末尾](#rc6c-lifetime)と §5)。中間の RT を名前で読む約束(`retain`)は RC-7a で済み(2026-10-31、`2d1715cf3`。[§2 の末尾の RC-7a](#rc7a-readback)と §5)。同じ形の共有の条件を広げる RC-7b も済み(2026-10-31、`0091c071c`。[§2 の末尾の RC-7b](#rc7b-sharing)と §5)。形の違う区間詰め RC-7c も済み(2026-10-31、`27a919f4a`。[§2 の末尾の RC-7c](#rc7c-packing)と §5。§3.2 の `alias_reuse` の表は作らない形に変えた)。memory の種類ごとの RC-7d も済み(2026-11-01、`c941a8346`。[§2 の末尾の RC-7d](#rc7d-memory-kinds)と §5。本物の lazily allocated memory の確保は未検証)。
前提: マニュアル [第6章 §6.6 target planning](manual/06_rendering.md#target-planning-tree)、
[design_render_pipeline_extensibility.md](design_render_pipeline_extensibility.md)、
[v1 開発計画](v1_development_plan.md)「保守上の整理候補」。
数字は `test/fixtures/devstudio/example_frame_plan.json`(example の実機の物理計画)から 1920×1080 で手計算したもの。
§1 の display の行と合計は RC-1 の計測(planner の test)に合わせて直した。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- example(1080p)で **全 RT の合計 120 MiB に対し、同時に生きている最大は 81 MiB**。今の alias が節約しているのは 1 組(7.9 MiB)だけ。
- 「不要」は 3 種類あり、効く手段が違う: **(A) 人為的に伸びた寿命**、**(B) alias 条件の狭さ**(同 format・同 extent・同 usage の画像同士しか共有できない)、**(C) 宣言・パス単位の無駄**(unread な出力・パス、不要な store)。
- 提案の本丸は B: alias を**寿命区間による汎用メモリ共有(区間詰め)**に一般化し、gbuffer の跡地に bloom 鎖が入るようにする。
  まず計測の口(合計 / 同時最大 / 資源ごとの bytes と寿命)を `get_frame_plan` に出す。

## 1. 現状の数字(example、1080p)

| 資源 | format | extent | MiB | 寿命(order) | aliasable |
|---|---|---|---:|---|---|
| gbuffer_albedo / g_emissive / gbuffer_material | B8G8R8A8 / R8G8B8A8 | 1920×1080 | 7.9 ×3 | 0–3 | ○ |
| gbuffer_normal / gbuffer_worldpos | R16G16B16A16 | 1920×1080 | 15.8 ×2 | 0–3 | ○ |
| offscreen_depth | D32 | 1920×1080 | 7.9 | 0–**29** | ×(required) |
| ssao_output / ssao_blur | R8 | 1920×1080 | 2.0 ×2 | 1–2 / 2–3 | ○ |
| lit_color | R16G16B16A16 | 1920×1080 | 15.8 | 3–**29** | ×(required) |
| opaque_color / opaque_depth(snapshot) | R16G16B16A16 / D32 | 1920×1080 | 15.8 / 7.9 | 4–**29** / 5–**29** | ×(require_store) |
| Bloom_Threshold_RT | B8G8R8A8 | 1920×1080 | 7.9 | 11–12 | ○(g_emissive と alias) |
| Bloom_Downsample_H_k / Upsample_V_k(k=0..3) | B8G8R8A8 | 1/2 … 1/16 | 2.0 … 0.03 | 12–23 | ○ |
| display | B8G8R8A8 | 1920×1080(出力の大きさ) | 7.9 | 23–29 | × |

- display は format_class `display` で、runtime の正規化([`resolveRenderTargetFormatClassesV2()`](../src/core/renderingpass/rendertargetjsonparser.cpp))が出力の extent を書き込む。
  fixture の 160×90 は、その fixture を取ったときの出力の大きさ。
- 合計 119.95 MiB(125,776,320 B)、同時最大 81.08 MiB(85,017,600 B、order 3 = lighting_pass)。RC-1(`631caac21`)の planner の test の値。
  合計は alias の組を 1 回と数える(組は最大の住人の bytes で 1 回。21 枚が 20 の確保)。
- 以前この行にあった手計算の 120.0 MiB は、2 つのずれがほぼ打ち消し合った値だった: display を 160×90 で数えた(1080p の実物より 8,236,800 B 少ない)のと、alias の組の 1 枚(8,294,400 B)を引かなかった(多い)。差は 57,600 B。
- gbuffer 5 枚 + depth = 63 MiB は lighting の後に空く。bloom 鎖 9 枚 = 13 MiB は order 11 から始まるので、全部その跡地に入る大きさ。
- `opaque_*`(24 MiB)は `forward_transparent`(order 6)しか読まないのに末尾まで生きている。
  → **RC-6c で変わった**: 表の太字の `29`(`offscreen_depth` / `lit_color` / `opaque_*` の末尾までの寿命)は RC-6c の前の値。今はどれも graph の中の最後の use で終わる([§2 の末尾](#rc6c-lifetime))。表は記録として残す。
- 物理計画の `reason`: 21 枚中 15 枚が `arbitrary read requires a materialized resource`(sampler 読みなので妥当)。
- `unread_output` 診断: 0 件(example には無駄な出力は無い)。

## 2. A: 人為的に伸びている寿命

[`targetrenderplanning.cpp:1700-1710`](../src/project/targetrenderplanning.cpp#L1700): `materialization == required / external` か
`pattern.require_store` の資源は **`last_use` を末尾ノードに強制**する。

- `required` の出所([`logicalframegraphadapter.cpp:120-150`](../src/core/renderingpass/logicalframegraphadapter.cpp#L120)):
  (a) history 付き資源(`legacy history resource must survive across frames`)、
  (b) 論理型の宣言に `materialization` があるもの(`required_targets` / `render.target_ref` が運ぶ型。`lit_color` は bloom の
  `required_targets` の `pelican.render.color_signal@1`、`offscreen_depth` は depth 型と思われる — **型側の既定を確認する**)。
- `require_store` の出所: snapshot の出力(`snapshot output requires a stable sampled image`)。

調べること: (b) と snapshot が「フレーム末尾まで」要るのは何のためか(headless の `--render-out` の読み出し、picking の depth、
Studio / RenderDoc のキャプチャ、外部 depth export)。用途が特定の経路だけなら、その経路が有効なときだけ末尾まで伸ばす
(opt-in)。example では最大 −24(opaque_*)〜−47 MiB(lit_color / offscreen_depth 含む)の寿命短縮になり、B の効果も上がる。

上の行番号と「強制する」の説明は RC-6c の前の物(この一律の延長のコードは RC-6c で消えた)。

<a id="rc6c-lifetime"></a>

**RC-6c で変わった(2026-10-30、`be795c9a5`)。**仕様は `<作業ディレクトリ>/rc6_spec.md` §6、実装者の報告は `<作業ディレクトリ>/rc6c_report.md`。この段の行番号は `be795c9a5` の物。

- **寿命は graph の中の最後の use まで**。`required` / `external` / `require_store` は寿命を延ばす理由ではなくなった(planner と complete verifier の両方から一律の延長を消した)。
  **graph の外で読まれる物だけ**、理由(`retain`)を付けて graph の末尾まで延ばす。`first_use` は graph の中の最初の use のまま。frame をまたぐ別の印は作らない。
- **理由は 7 つ**(ほかの文字列は [`isTargetResourceRetainReason()`](../src/project/targetrenderplanning.cpp#L1587) で断る)。理由は compiler が持つ事実(IR・実際の FrameTarget・
  解決済みの variant・選んだ depth の元)から作り、package の申告からは作らない。IR から作る所は [`renderingTargetRetainReasons()`](../src/core/renderingpass/renderingsamplecount.cpp#L1478)。

| 理由 | 付く資源(理由を作る所) | graph の外の読み手 |
|---|---|---|
| `present` | 予約の出力 `swapchain` を書く use(output_transform を通らずに直接書く pass も、offscreen の出力も) | 実際の FrameTarget(present、[`readbackLastFrameRGBA8()`](../src/core/vkcore/rendertarget.cpp#L86)) |
| `history` | IR の image の `history` | 二面の初期化と反転([`resetHistory()`](../src/core/renderingpass/rendertargetcontainer.cpp#L1252)) |
| `persistent` | IR の image の `persistent`(once の結果、`initial` の asset を持つ image を含む)か `source`(画像ファイル由来) | 後の frame([once の要求](../src/project/rendergraphcomposition.cpp#L453)、[source の upload](../src/core/renderingpass/rendertargetcontainer.cpp#L1052)、[initial の読み込み](../src/core/renderingpass/renderingpassconfigregistration.cpp#L943)) |
| `external_depth_export` | 既存の選択で決まった export の元([足す所](../src/project/targetrenderplanning.cpp#L3819)。選んだ後に寿命を計算し直す) | [`recordExternalDepthExport()` を呼ぶ所](../src/core/vkcore/renderer.cpp#L6763) |
| `xr_mirror` | 解決済みの variant の `mirror_output` が `left_eye` のときの `display`([渡す所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L522)) | [mirror のコピー](../src/core/vkcore/renderer.cpp#L6774)(コピー元が [`display`](../src/core/vkcore/renderer.cpp#L2833)) |
| `picking` | 実在する picking の pass が color attachment として書く `picking_id` | [picking の有効化](../src/core/vkcore/renderer.cpp#L5724)と [`readPickingPixel()`](../src/core/vkcore/renderer.cpp#L5039) |
| `capture` | 実際の FrameTarget が capture できる(`capture_available`)flat の variant の `swapchain`([決める所](../src/core/renderingpass/renderingpassconfigregistration.cpp#L984)) | `--render-out`([`captureLastFrameToPng()` を呼ぶ所](../src/core/appflow/loop.cpp#L661))と通常の capture |

- `present` と `capture` は同じ資源に両方付く。capture は要求のたびに計画を変えず、capture できる FrameTarget の最終出力にいつも付ける(報告の判断)。
  XR mirror のコピー先の中間 image は engine が graph の外で持つ確保のままで、RC-6c の対象外。
- **理由にしない物**: snapshot(graph の中の use)、cube capture という種類だけ(実際の persistent・実行条件で決まる)、XR が有効なだけ、RenderDoc、preview、ImGui、GPU timing、NativeScope、
  buffer の CPU readback(test の `readFrameGraphBuffer` など。image の寿命の契約で守られているとは扱わない)。
- **純粋な関数 1 つを 2 か所で計算し直す**: [`calculateTargetResourceLifetimes()`](../src/project/targetrenderplanning.cpp#L1594) は canonical graph・検査済みの node の順・資源名ごとの理由から
  資源ごとの `used / first_use / last_use / retain` を出す。使われた資源で理由がある物だけ末尾へ延ばし、理由は辞書順・重複なしにする。使われない資源には理由を付けない。
  planner([lowering graph を作る所](../src/project/targetrenderplanning.cpp#L1636))と complete verifier([`verifyVulkanCompletePhysicalPlanPackage()` の中](../src/project/vulkancompletephysicalplan.cpp#L1822))が
  それぞれ自分の入力から計算する。verifier の理由の入力は compiler が持つ `VulkanTargetPlan::retain_reasons`(寿命の出力とは別の欄で、直列化しない)で、package の寿命も自動計画の寿命も写さない。
  順と coverage の検査は前のまま。
- **complete package と dump**: complete の resource の `lifetime` に `retain` が入り、parse・serialize・比較の対象になった。`last_use` か `retain` を変えた package は
  `Vulkan complete physical resource has stale lifetime or read-footprint metadata` で断られる。parser は知らない理由・重複・空の配列・使われない資源の理由を断る
  ([読む所](../src/project/vulkancompletephysicalplan.cpp#L516))。`get_frame_plan` の `physical_target_plan` の資源と lowering graph の資源の `lifetime.retain` は理由の配列で、理由が無ければ欄が無い。
  寿命から作る fingerprint に理由も入る([`appendLifetimeFingerprint()`](../src/project/vulkanphysicalfragment.cpp#L301))。寿命が変わった計画の `automatic_plan_fingerprint` は RC-6c の前と違う。
- **変えなかった物**: alias の適格性・runtime の共有・登録・representation・attachment の load / store・`total_bytes`・peak の定義(閉区間 `[first_use, last_use]` の和)。
  出荷 7 project と RC-5 の 16 構成で alias の組・登録・ops・候補・representation が変わらないことを test が見る(報告の E3。example の alias は前と同じ `Bloom_Threshold_RT` と `g_emissive` の 1 組)。
  汎用の planner で `required` かつ alias 可・`require_store` なしの入力だけは寿命が縮んで alias の組が増え得る。増えた組の寿命が重ならないことを test が見る。出荷の構成にこの入力は無い(test)。
- **example の数字**(1920×1080、test。lowering graph は 23 node): total 125,776,320 B と peak 85,017,600 B(order 3 = `lighting_pass`)は変わらない。縮んだのは後半の生存量で、
  order 11〜20(bloom 鎖と合成)は 60,134,400〜61,171,200 B → 26,956,800〜27,993,600 B、order 21〜22(`pelican_ui` / `output_transform`)は 58,060,800 → 8,294,400 B。
  order 21 の差 49,766,400 B は `offscreen_depth`・`lit_color`・`opaque_color`・`opaque_depth` の 4 枚の和(§1 の format と大きさ)と一致する(数字からの推定。資源ごとの内訳は test が見ていない)。**実際の確保は減っていない**(alias を変えていないため)。
- **RC-7 に残した物**: alias の適格性の拡張、縮んだ寿命による領域の使い回し(§3 の区間詰め)、discard / store の最適化(§4)、実メモリの削減、
  名前の readback の opt-in(policy の `retain` と、`retain` の無い RT の名前の readback の拒否、test の 144 か所の移行)、使い回した領域の readback の中身の test。
  RC-6c では alias を変えないので中間の RT の中身は残り、名前の readback は今のまま動く。XR mirror の中間と buffer の readback は、image の保持理由で守られているとは扱わない(報告)。
  → 名前の readback の opt-in・拒否・test の移行は RC-7a で済んだ(下)。

<a id="rc7a-readback"></a>

**RC-7a で変わった(2026-10-31、`2d1715cf3`。名前の readback の約束)。**仕様は `<作業ディレクトリ>/rc7_spec.md` の冒頭(利用者の決定 7 点。下の §5)と §1、実装者の報告は
`<作業ディレクトリ>/rc7a_report.md` と修正の報告 2 本(`rc7a_fix_report.md` / `rc7a_fix2_report.md`)。この段の行番号は `2d1715cf3` の物。alias の作り方(アルゴリズム)は変えていない。

- **約束の鍵 `retain`**: `target_planning.graphs.<g>.resources.<r>.retain`。値は bool か `{value, strength}`(RC-6a1 の形)。`retain: true` は「この資源は graph の後に名前で読める」の約束。
  台帳・出所・優先(root > レシピ > 部品)・所有の検査・結び付け・absent・complete package との同居の拒否は RC-6a1 の道のまま([読む所](../src/project/renderpipeline.cpp#L503)、
  再シリアライズの鍵の列挙は [その所](../src/project/renderpipeline.cpp#L569))。全体の流れは [棚卸しメモ §2-3 の RC-7a](design_plan_layer_inventory.md#rc7a-implementation)。
- **寿命の理由 `readback`**: 採った `retain: true` の資源に理由 `readback` を足し、RC-6c の寿命の関数で末尾まで延ばす。上の表の engine の 7 つの理由(compiler が持つ事実から作る)とは
  別の入力で、policy から来る。理由の一覧([`isTargetResourceRetainReason()`](../src/project/targetrenderplanning.cpp#L1595))は 8 つになった。dump の `lifetime.retain` にも `readback` が出る。
- **候補ごと・再試行ごとに作り直す**([その所](../src/project/targetrenderplanning.cpp#L3905)): 各候補の各再試行で、その時に採っている `retain: true` から、理由 `readback`・
  その資源を書く attachment の全部の `store_op: store`・寿命を作り直す。選んだ候補の分が `plan.attachments` と `VulkanTargetPlan::retain_reasons` に残る。表現は派生の
  `representation: materialized` で、台帳に行を足さず `retain` の行の強さ・出所のまま効く([`materializingPolicy()`](../src/project/targetrenderplanning.cpp#L1040))。原因つきの再試行で
  `prefer` の `retain` が外れた候補には、理由も store も残らない(test「RC-7a F2 causal alias retries erase dropped retain lifetime and operations」)。
- **TRANSFER_SRC は RC-7a では宣言に要る**: 利用者の決定 1 は「planner が materialized・store・TRANSFER_SRC を足す」だが、RC-7a は TRANSFER_SRC を足さない(仕様 §1.2 の一時的なずれ)。
  usage は宣言・variant の和・device への問い・登録で決まり、その道は RC-7c で device facts と一緒に作るので、自動で足すのはそこ。RC-7a では generation の実効の usage
  (登録と同じ resolver の答え。下の「実効の usage」)に TRANSFER_SRC が無ければ約束は成り立たない。
- **成り立たない条件**([`retainContractFailure()`](../src/project/targetrenderplanning.cpp#L3343)): 使われる物理 image が無い(宣言だけで graph が使わない image を含む)、
  materialized の image でない(external と buffer は不可)、format が color か D32 でない、resolve の無い multisample、実効の usage に TRANSFER_SRC が無い、寿命が末尾に届かないか
  理由 `readback` が無い、attachment の書き手に store でない物か対応する attachment の行の無い物がある。`require` が 3 候補とも成り立たなければ
  `target_planning.retain_unsatisfiable@1`(`graph`・`resource`・`declaration`・`candidates`。[投げる所](../src/project/targetrenderplanning.cpp#L3978))。`prefer` は `infeasible_prefer` で外れ、
  その RT の名前の readback は断られる(test「RC-7a F2 unsupported usage rejects require and discards prefer without residue」)。
- **最終の検証**: [`validateVulkanTargetPlanRequirements()`](../src/project/targetrenderplanning.cpp#L3786) が、採った `require` の `retain` を結果の資源・`plan.attachments`・
  `plan.effective_image_usage` から同じ条件で確かめ直す。破れは RC-6a2 と同じ `target_planning.requirement_unsatisfied@1`。見る書き手は、明示の attachment intent の全 node と、
  automatic intent の render / output_transform の node(1 回目の修正の報告の 2。ToneMap → `display_output` の行を消した計画・`dont_care` にした計画を断る test)。
- **`retain: false`**: 採った `false` は下位の `true` を打ち消すだけ(RC-6a1 の優先)。engine の 7 つの理由は消えない(test「RC-7a F3 false preserves independent engine retention」)。
- **名前の readback の拒否**: `Renderer::readRenderTargetForTesting()`([検査を呼ぶ所](../src/core/vkcore/renderer.cpp#L5167)。R8 版は委譲)は、読むのに使う snapshot の同じ program の
  `frame_graph.target_plan` だけを見る([`validateTargetReadbackRetention()`](../src/project/targetrenderplanning.cpp#L3771))。その資源の理由に `readback`・`present`・`capture` の
  どれかがあれば読める。plan が無い・資源が無い・理由が無ければ `render_target.readback_not_retained@1`(`resource`・`graph`・`reason`)。`persistent` / `picking` / `history` /
  `xr_mirror` / `external_depth_export` だけでは読めない。別の program・variant や、generation を更新する前の許可は持ち越さない(test「RC-7a F1 snapshot program and generation never
  inherit another readback permission」)。program が無いときは前と同じ `render-target readback requires a compiled render pipeline` が先(1 回目の修正の報告の 4)。最終出力は今までどおり
  FrameTarget の readback / capture の道で読み、`retain` は要らない。名前の readback の口は test 用の C++ だけで、RPC には無い(コードの読み)。
- **実効の usage**: `retain: true` がどこかの variant で採られているときだけ、compiler program が全 variant を 1 度、`retain` を `representation: materialized` に置き換えて
  (TRANSFER_SRC は足さずに)計画し、登録と同じ resolver [`resolveCompilerGenerationResources()`](../src/core/renderingpass/rendercompilerprogram.cpp#L223) の答えから資源名 → usage を作る
  ([その所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L672))。その表を本計画の各候補と最終の検証に渡す。alias の適格性と device への問いは宣言の usage のまま
  (2 回目の修正の報告の 1。無関係な transient の image の representation と usage が変わらない GPU の test)。`retain` が無ければ、表は宣言と engine の規則から作り
  ([その所](../src/core/renderingpass/renderingsamplecount.cpp#L1626))、先の 1 度の計画はしない。
- **test の移行**: 名前の readback を使う test は、読む資源だけに `retain: true` を compile の前に足す(`TestSupport::retainObserved()`)。Catch の listener が fixture・graph・variant ごとに
  「採った `retain` の集合 = 成功した readback の集合」を確かめる(成功が 0 回でも、SKIP した実行は除く)。移行した呼び出しは 144 か所(汎用 135・R8 9)。修正の報告の実測は、
  fixture・graph・variant の組 189、成功した readback 977 回、一意の資源名 105、不一致 0。移行の途中で余分な `retain` が 1 つ見つかり(読まない FP-1 の test が共通の builder の
  `show_image` を retain していた)、読む PS-1 の fixture だけに移した。
- **変えなかった物**: `retain` を書かない project の計画。RC-6 の A5(除外は RC-6c のまま)と、`283aacc05` で記録した 16 構成(`<作業ディレクトリ>/rc7_baseline/rc5`)が fingerprint を含めて
  一致する(test「RC-7a F4 unchanged HEAD baseline including fingerprints」。`[.]` で手で走らせる)。
- **確かめた範囲**: 統合側で GPU 全数・非 GPU 全数・シナリオ 23 本が緑(既知の環境由来の赤のみ)。既存の SKIP 3 件(画像の基準が無い 2 件と CB-1 の 08)は未検証。
- **RC-7 に残した物**: TRANSFER_SRC の自動の付け足し(RC-7c)、alias の適格性の拡張(RC-7b)、形の違う区間詰め(RC-7c)、memory の種類ごとの確保(RC-7d)、
  使われない RT の確保の除去(RC-7e)。段の並びは §5。
  → alias の適格性の拡張は RC-7b で済んだ(下)。

<a id="rc7b-sharing"></a>

**RC-7b で変わった(2026-10-31、`0091c071c`、branch `agent/rc7b`。同じ形の共有の条件を広げる)。**仕様は `<作業ディレクトリ>/rc7_spec.md` の冒頭(利用者の決定 7 点)と §3、実装者の報告は
`<作業ディレクトリ>/rc7b_report.md` と修正の報告 2 本(`rc7b_fix_report.md` / `rc7b_fix2_report.md`)。この段の行番号は `0091c071c` の物。配置は今のまま offset 0 で、owner の allocation 1 つを
住人が共有する(`allocAliasingImage()`)。barrier も layout tracker の住人の切替のまま(決定 5)。形・offset・heap・usage の和が違う物の共有は RC-7c。

- **判定は planner の 1 か所**: [`applyCandidateAliasPolicy()`](../src/project/targetrenderplanning.cpp#L3525) の先頭で、sample 数・view layout・scope を決めた後に資源ごとの `aliasable` を決める
  (前は `buildCandidateDraft()` の式と、`applyResolvedSampleCounts()` の multisample の分の代入の 2 か所。どちらも消した)。宣言の側の入口 [`isRuntimeAliasEligible()`](../src/project/rendergraphir.cpp#L241)
  からは「usage がちょうど `COLOR_ATTACHMENT | SAMPLED`」の条件を外した(history・file source・persistent・`display` の除外は残る)。物理の `aliasable` と `require_store` の結び付きも外した。
  外れた資源には dump の `decisions[]` に `pelican.plan.alias_ineligible@1`(`subject` = 資源、`selected` = 下の条件の名前、`detail` = `RC-7b image-wide alias eligibility`)、
  組めない対には `pelican.plan.alias_pair_ineligible@1`(`subject` = `<a>/<b>`、`selected` = `effective_usage` か `same_scope`)が出る。
- **適格の条件**(仕様 §3.1 の 1〜7。括弧は `selected` の名前):
  1. **保持の理由が無い**(`retained`): 寿命の `retain` が空。engine の 7 つの理由と RC-7a の `readback` のどれも採っていない。display / history / persistent / source の除外は今のまま
     (宣言の入口で pattern の `allow_alias` が偽になり、`ownership_or_samples` で外れる)。
  2. **旧住人の中身を要らない**(`first_use`): 最初の use の node が render の node で、その node がその資源を入力として読まず、load op が `load` でない automatic attachment としてその資源を書き、
     その attachment が全 subresource を覆う。覆うとは、subresource の range が無ければ mip が既定(`ImageMipLevelCount{}`)で layer が 1 か scope の `view_count` 以上、range があれば
     base の mip と layer が 0・layer 数が全部・mip が `remaining` か固定の数が一致。aspect は color と depth だけを数え、depth-stencil と stencil の format は外す(下)。最初が読み・load・
     compute / transfer の書き込み・一部の range なら外れる。
  3. **単一 sample の materialized image**(`ownership_or_samples`): pattern の `allow_alias`、logical materialization が external でない、temporal の読みが無い、
     representation が `materialized_image`、`rasterization_samples = 1`、resolve が無い。使われない資源は `unused`。
  4. **画像の契約が同じ**(対の `effective_usage`): 対の両方の実効の usage(`VulkanTargetPlanRequest::effective_image_usage`。RC-7a から compiler が渡す generation の usage)が一致すること。
     表に名前が無い(facts が不明な)対も外す。representation・format・samples・view layout・mip・layer・dimension・extent の一致と寿命の非重複は前からの
     [`deriveLegalAliasCandidates()`](../src/project/targetrenderplanning.cpp#L3164) の検査のまま。
  5. **同じ rendering scope の中で住人を切り替えない**(対の `same_scope`): 先の住人の最後の use と後の住人の最初の use が、採った計画の同じ物理 scope に入る対は外す。候補・再試行ごとに
     scope を決めた後で見る。`alias_group` の組がこれで成り立たないときの原因には、両方の資源に触る scope の `scope_join: true` も入る
     ([`aliasConflictCauses()`](../src/project/targetrenderplanning.cpp#L3459))ので、`prefer` の join は RC-6a2 の再試行で外れる。
  6. **manual / unchecked の NativeScope の境界に触れない**(`native_boundary`): planner は `VulkanTargetPlanRequest::manual_native_resources` の名前を外す。本番の呼び出しでこの欄を埋める所は
     src に無い(grep で確認。test「RC-7b planner excludes provider manual NativeScope boundary facts」が直接埋める)。manual / unchecked の synchronization は complete package の値で(コードの読み)、
     そちらは verifier が package の NativeScope から断る(下)。
  7. 寿命の非重複・`no_alias`・`isolate`・profile は今のまま。

  ほかに、generation の合意で外れた資源は `generation_contract: <理由>` で外れる(下)。
- **depth-stencil を共有しない理由**: attachment の aspect の型 `AttachmentAspect` は color と depth だけで、stencil を表さない(コードの注記「AttachmentAspect::depth supplies only depth, never stencil」)。
  条件 2 の「format の全 aspect を書く」を depth-stencil では計画から示せないので、depth-stencil と stencil の format は共有しない(実装者の報告の「最初の事実の確認」)。depth だけの
  `D32_SFLOAT` は共有できる。
- **variant を統合してから判定する**: compiler program は `retain` の有無によらず、全 variant を先に 1 度計画して(`discover_placements`)generation の実効の usage・storage・配置を集める
  ([その所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L678))。RC-7a ではこの先の計画は `retain` が採られているときだけだった。先の計画では `retain` に加えて `alias_group` の宣言も
  台帳から外す([その所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L532))。variant ごとの宣言の usage で判定すると、統合すれば合法になる組の `require` を先に断ってしまうため
  (1 回目の修正の報告の 1)。本計画は統合した usage で行い、その配置でもう 1 度 [`resolveCompilerGenerationResources()`](../src/core/renderingpass/rendercompilerprogram.cpp#L224) を呼び、
  共有を外した資源があれば理由つきの除外(`generation_alias_exclusions`)を渡して全 variant を計画し直す([その所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L701))。
  - generation の resolver([`resolveGenerationResources()`](../src/project/generationresources.cpp#L97))は、組の中で usage か storage が違う組を、前の `render_graph.alias_group_invalid@1` の例外ではなく
    共有の解除にし、資源ごとの理由を `GenerationResources::alias_rejections` に入れる(`generation image contracts differ: <組>`。variant が組に合意しない組は
    `generation variants do not agree on alias placement: <組>`)。format・extent・samples・mip・layer・dimension・history の食い違いは今のまま `alias_group_invalid@1`。
  - 自動の共有と `prefer` は外れ、理由が dump の `pelican.plan.alias_ineligible@1` に出る。`require` は planner の組の検査で成り立たず、3 候補とも不成立で `target_planning.requirement_unsatisfied@1`
    (候補の失敗の文に `alias.<条件>@1: <資源>` が付く)。test「RC-7b merged usage restores legal variant sharing」(flat では usage が違い XR では同じ 2 variant。統合後は automatic / prefer /
    require とも共有する)、「RC-7b retain-free variants resolve merged contracts before planner policy」(`retain` を書かない 2 variant の合意と解除)。
- **graph 間で外れた共有**: 1 つの variant の複数の graph が組に合意しないとき、runtime の組の統合 `mergeRuntimeAliasGroups()` が外した資源に理由
  (`frame graphs do not agree on alias placement: <graph>` か `…: overlapping groups`)を返し、[`compileRenderingTargetPlans()`](../src/core/renderingpass/renderingsamplecount.cpp#L1961) がそれを除外に足して
  全 graph を計画し直す。各 graph の `alias_groups`・decision・dump・登録の assignment が同じ非共有の配置を言う(前は登録が非共有なのに最初の graph の計画と dump が共有を表示していた)。
  `require` は名前付きで断られる(1 回目の修正の報告の 2。test「RC-7b graph overlap releases generation sharing and its policy display」)。
- **`require` の最終の照合**: 登録の前の仕上げ [`completeGenerationResources()`](../src/core/renderingpass/rendercompilerprogram.cpp#L249) が、各 variant の最終の物理計画の台帳(`plan->policy`)で
  採った `require` の `alias_group` ごとに、同じ文書・同じ組名で採った peer が最終の `GenerationResources` の同じ alias の組にいるかを見る([その所](../src/core/renderingpass/rendercompilerprogram.cpp#L258))。
  いなければ `target_planning.alias_generation_contract@1 {graph, resource, peer, reason}`(`reason` は `alias_rejections` の文か `final generation placement does not share required alias group`)。
  前の照合は「`alias_rejections` に名前があるか」で、提案そのものが消えた場合を見逃した(1 回目の修正の報告の 2)。宣言(`compiled_pipeline` の台帳)でなく最終の計画の台帳を見るのは、
  再試行で `infeasible_prefer` になった peer を照合から外すため(2 回目の修正の報告。A / C が `require`・B が `prefer` の組で B に `no_alias` か `retain` を書くと、B が外れて A / C だけが共有する。
  test「RC-7b final generation accepts required sharing after no_alias drops a prefer peer」)。
- **登録の前の配置の検査**: [`imagePlacementsOf()`](../src/core/renderingpass/renderingsamplecount.cpp#L2314) の組の検査は、宣言の usage ではなく物理計画の実効の usage を比べ、local read の禁止を外した
  (local read は条件 2 と 5 で planner が見る)。container の検査は変えていない(owner の create info の複製の検査なので depth・transfer・BGRA も同じく通る。報告)。
- **complete package の verifier**: [`verifyVulkanCompletePhysicalPlanPackage()`](../src/project/vulkancompletephysicalplan.cpp#L1542) は package の `aliasable` を根拠にせず、組の住人ごとに compiler の事実
  (`automatic_plan` の寿命・`effective_image_usage`・`generation_alias_exclusions`・`manual_native_resources`・lowering の pattern)と package の attachment・scope・NativeScope から条件を計算し直す
  ([その所](../src/project/vulkancompletephysicalplan.cpp#L2500))。断る文の頭は `alias.native_boundary@1`・`alias.generation_contract@1`・`alias.retained@1`・`alias.ownership_or_samples@1`・
  `alias.first_use@1`・`alias.missing_resource@1`、対の `alias.image_contract@1`・`alias.same_scope@1`・`alias.shape_or_lifetime@1`。そのために `VulkanTargetPlan` に `generation_alias_exclusions` と
  `manual_native_resources` を足した(JSON には書かない。grep で確認)。
- **tracker の frame 先頭の pending**: [`RenderTargetLayoutTracker::beginFrame()`](../src/core/vkcore/render_target_layout_tracker.cpp#L352) が、住人のいる共有の token を全部 pending にする。renderer は
  `renderLogicalFrame()` の `beginFrameResources()` で frame ごとに 1 回、記録の前に呼ぶ([呼ぶ所](../src/core/vkcore/renderer.cpp#L6319))。[`transition()`](../src/core/vkcore/render_target_layout_tracker.cpp#L131)
  は pending か住人の切替のとき、ALL_COMMANDS → ALL_COMMANDS・`MEMORY_READ | MEMORY_WRITE` の memory barrier を出し、住人の layout を UNDEFINED にする(前からの形)。前は同じ住人・同じ layout が
  frame をまたいで続くと barrier が出ず、frame 間の依存が保証されていなかった(報告の「最初の事実の確認」)。新しい token(新しい generation の allocation)は pending に入らないので、最初の access は
  barrier なしで UNDEFINED から始まる。`reset()` の後の同じ token は前からの pending の道。test の観測口 `alias_transition_observer`(`AliasTransition`)を足した。
- **生きている generation の更新**: 置き換えの transaction(arena と checkpoint)は変えていない。所有する scope の中の組の結合・分割・解除がこの transaction で通り、遅い失敗で名前・allocation・
  readback の許可が戻ることを GPU の test で確かめた(§5 の RC-7b の B5)。それ以外の live の契約変更の作り直しは 7b の外(今の拒否のまま)。
- **example は 1 組のまま**: 1920×1080 の example は `{Bloom_Threshold_RT, g_emissive}` の 1 組・21 枚 20 確保・total 125,776,320 / peak 85,017,600(order 3 `lighting_pass`)のまま。
  7b で新しく候補に入る `lit_color`・`scene_depth`・`shadow_map` には、同じ契約で寿命の重ならない相手が無い(usage が違うか寿命が重なる)。`opaque_color` / `opaque_depth` は最初が snapshot の
  transfer の書き込み、`display` は display の除外、`ssao_output` / `ssao_blur` と bloom の各段の対は閉区間の端で重なる(報告の表)。7b は 7c の前提を作る段で、効果は専用の fixture で見る
  (§5 の B1)。example の確保が減るのは RC-7c(形の違う区間詰め)から。
- **計画の差(B4)**: 実装前の HEAD `2d1715cf3` で記録した 16 構成(`<作業ディレクトリ>/rc7b_baseline/rc5`)と比べ、意味の欄(実行順・scope・representation・format・samples・attachment の ops・usage・寿命・
  保持の理由)は一致する。差は共有の派生の欄だけで、固定の除外を外した `lit_color`・`scene_depth`(各 16 構成)・`shadow_map`(8)は logical materialization が required → virtual、
  runtime pattern の `allow_alias` が false → true、`require_store` が true → false、`aliasable` が false → true。`opaque_color` / `opaque_depth` は pattern が候補の側に変わるが、
  `aliasable` は false のまま。`reason` の文は 56 か所で変わる(`display` は `logical materialization requirement is required: display-format target is not aliased`、ほかは
  `arbitrary read requires a materialized resource`)。新しい decision は資源の不適格 64 行と対の `effective_usage` 352 行(30 種の対)。組と `alias_candidates` は 16 構成とも変わらず、
  fingerprint は派生の欄の分だけ変わる(報告)。比べる test は「RC-7b B4 semantic fields match sixteen immutable preimplementation snapshots」(`[.]`)。RC-6 の A5 と RC-7a の F4 も、同じ除外の関数
  `rc7bComparable()` を通して比べる形になった(派生の欄だけを除き、`reason` は上の既知の文への対応を確かめてから記録の文に戻す)。
- **RC-7 に残した物**: 形・offset・heap・usage の和が違う共有と `alias_reuse` の barrier・`retain` の資源への TRANSFER_SRC の自動の付け足し(RC-7c)、memory の種類ごとの確保(RC-7d)、
  使われない RT の確保の除去(RC-7e)。depth-stencil の共有は 7b の外(どの段で扱うかは未定)。
  → 形・offset・heap の違う共有と TRANSFER_SRC の自動の付け足しは RC-7c で済んだ(下)。`alias_reuse` の barrier の表は作らない形に変えた(下と §3.2)。

<a id="rc7c-packing"></a>

**RC-7c で変わった(2026-10-31、`27a919f4a`、branch `agent/rc7c`。形の違う区間詰め)。**仕様は `<作業ディレクトリ>/rc7_spec.md` §4(仕様レビュー 12 件の反映)と §4.9(確認レビュー 5 件で確定した事)、
実装者の報告は区切りごとに 2 本(`<作業ディレクトリ>/rc7c1_report.md` / `rc7c1_fix_report.md`、`rc7c2_report.md` / `rc7c2_fix_report.md`、`rc7c3_report.md` / `rc7c3_fix_report.md`)。
この段の行番号は `27a919f4a` の物。全体の流れは [棚卸しメモ §2-3 の RC-7c](design_plan_layer_inventory.md#rc7c-implementation)。

- **区切り**: ① 契約(create info の関数の 1 本化・`maintenance4`・facts の照会・`packMemoryRegions` と不変の入力)→ ② 計画(planner の出力・合意・TRANSFER_SRC の自動の付け足し・complete / wire /
  fingerprint・Studio の移行・計測の分け方)→ ③ runtime(実画像での配置・VMA の region と bind・tracker の range 化・resize の 2 つの道・置き換え・復元・heap の寿命)。② の間は「全員の create info が
  同じで offset 0」の集合だけを旧い token の共有に渡す橋(`legacyAliasTokenDefinitions`)を置き、③ で消した。着地は ③ の後の 1 回(① ② だけでは配置が実確保と食い違うため。仕様 §4.8)。
- **memory の鍵**: `memoryTypeBits` の値の完全一致(AND は取らない。合わなければ別の region = 決定 3)。RC-7b の条件 4(画像の契約が同じ)はこの鍵に置き換えた。対の候補
  [`deriveLegalAliasCandidates()`](../src/project/targetrenderplanning.cpp#L3168) は寿命と同じ scope だけを見る。資源ごとの条件 1〜3・5〜7(RC-7b)はそのまま region に入る条件。
  dedicated が必須(`requiresDedicatedAllocation`)と bits 0 は region に入れず通常の確保。`prefersDedicatedAllocation` だけなら入れる。
- **requirements は実画像が正**: create info は [`renderTargetImageContract()`](../src/core/renderingpass/rendertargetimagecontract.cpp#L11) の 1 か所で作り、計画の照会と登録が同じ値を使う。
  計画の照会は `vkGetDeviceImageMemoryRequirements`(+ `VkMemoryDedicatedRequirements`。[その所](../src/core/renderingpass/renderingtargetmemoryfacts.cpp#L4))で、device の作成で `maintenance4` を
  有効にした([その所](../src/core/vkcore/core.cpp#L796)。支持しなければ `vulkan.required_feature_missing@1: maintenance4`)ので同じ create info の実画像と一致する(GPU の test で 5 通りの create info を照合)。
  計画の offset は dump と見積もり用で、runtime は実画像の requirements で決め直す。
- **packing の不変の入力**(planner が generation ごとに作り、登録が持つ): (a) region に入る資源と鍵の snapshot、(b) 重ねてはいけない対、(c) 同じ offset の集合(採った `alias_group` の住人。
  1 つの slot として置く: size・alignment は住人の最大、禁止の対は住人の和)、(d) 詰める順(first_use 昇順・size 降順・name 昇順、`hazard_stress` の seed で並べ替え)。
  [`packMemoryRegions()`](../src/project/memoryregionpacking.cpp#L24) は project の層の純関数で、first-fit・offset は slot の alignment の倍数・heap の size は max(offset + size)・heap の alignment は住人の最大・
  決定的。全 RT は optimal tiling なので bufferImageGranularity は考えない。
- **禁止の対の定義**(決定 4、generation で 1 つの配置): 2 つの資源を重ねてよいのは、両方が現れる graph が 1 つ以上あり、両方が現れるどの graph・variant でも寿命が重ならず同じ scope に入らない時だけ
  ([`targetMemoryOverlapForbidden()`](../src/project/targetrenderplanning.cpp#L4703)、[`planGenerationMemory()`](../src/project/targetrenderplanning.cpp#L4747))。一緒に現れる graph が無い対は重ねない(graph の外の時刻の関係は
  仮定しない)。資源ごとの適格と policy は、その資源が現れる全 graph・全 variant で成り立つことを先に求める(1 つでも外れれば region に入らない)。合意で外れた対の自動 / `prefer` は外して理由を dump に、
  `require` の集合の中に禁止の対が出たら `target_planning.alias_generation_contract@1`。`prefer` の採否は packing の置いた slot から (文書, 組名) ごとに一括で決める
  ([`updateGenerationAliasPolicy()`](../src/project/targetrenderplanning.cpp#L4716)。② の修正の報告の 4)。runtime が layer 数を変える shadow の image は packing の前に除外(`runtime_array_layers`。② の修正の報告の 1)。
- **`alias_groups` = 同じ offset の最大の集合**: plan の `alias_groups` は、同じ region・同じ offset の住人の最大の集合(size が違っても同じ集合。互いに素で一意)。採った `alias_group` の集合はその部分集合。
  部分的な重なりは出さないので、Studio の Freeze(`alias_groups` の全員に `alias_group: require` を書く)は満たせる `require` だけを書く(確認レビュー 3)。plan の `memory_regions` / `alias_groups` は計画の facts の値で、
  登録の結果は別に `memory_summary.actual_placement` に出す(確認レビュー 4)。
- **`alias_reuse` の表は作らない**: 重なりの関係は実画像の size で変わる(resize)ので、計画の時の表は実配置の写しにならない。barrier は下の同期の規則で runtime が出し、実際の重なりは登録の結果に出す。
  §3.2 の「planner が `alias_reuse` を出す」からの変更(§3.2 に注記した)。
- **facts が無いとき**(compiler_only・照会が空): 決定 3 どおり自動の共有はしない(RC-7b の同じ形の組も作らない)。候補は dump の `alias_candidates` に `adoption: "unknown"` で出し、資源ごとに
  `pelican.plan.alias_requirements_unknown@1`。`alias_group: require` は `alias.requirements_unknown` で断る。本番の登録は常に実画像の requirements を持つので、本番の配置は影響されない。
- **retain の TRANSFER_SRC**: 採った `retain` の資源に planner が TRANSFER_SRC を足す([その所](../src/project/targetrenderplanning.cpp#L4054))。再試行で `prefer` の `retain` を落とせば TRANSFER_SRC・寿命・store も消える。
  RC-7a の `retain_unsatisfiable@1` は「TRANSFER_SRC が宣言に無い」で断ることが無くなり、device がその format・usage の組で TRANSFER_SRC を支持しない時だけ `require` を断る(`prefer` は不採用)。
  画像でない資源(buffer)の `retain` は capability の照会の前に断る(② の修正の報告の 2)。最終の verifier の「TRANSFER_SRC が実効の usage にある」検査は残した。
- **complete package・wire**: package・wire・fingerprint に `memory_regions` と `memory_packing` を足した。verifier は package の配置も禁止の対も自己申告を信じず、compiler の (variant, graph) ごとの provenance
  から (a)〜(d) を作り直し、`memory.overlap@1`・`memory.alignment@1`・`memory.heap_size@1`・`memory.key@1`・`memory.dedicated@1`・`memory.same_offset@1`・`memory.inputs@1` で断る
  ([その所](../src/project/vulkancompletephysicalplan.cpp#L2617))。`alias_groups` だけの package は `complete_physical_plan.memory_regions_required@1`。RC-7b の保持・最初の use・NativeScope の検査は維持。
- **runtime の配置**: 登録は region に入る資源の image を memory なしで作り(ALIAS_BIT)、`vkGetImageMemoryRequirements2` + dedicated で実際の requirements を取り、保持した入力で詰め直し
  ([`prepareRegionImages()`](../src/core/renderingpass/rendertargetcontainer.cpp#L312))、region ごとに VMA の allocation を 1 つ(heap の size・最大の alignment・鍵の bits、DEVICE_LOCAL を preferred)、
  `vmaBindImageMemory2(allocation, offset, image)` で bind する。bind の前に offset + size ≤ heap の size・offset が alignment の倍数・選んだ memory type が bits に入る、を確かめ、破れは
  `render_target.memory_bind_invalid@1`([その所](../src/core/vkcore/core.cpp#L1619))。`allocAliasingImage()`(owner の create info の複製)と offset 0 の token の道は消した。各 image は region の allocation を
  共同で持ち(image → allocation の順に壊す)、resize の退役の batch と submit の lease が in-flight の image と heap を保つ。
- **同期**(決定 5、広め): layout は image ごと、再利用の状態は range の重なりで追う(region の単一の住人で追わない)。資源 R の access の前に、R がこの frame で初めての access か、R の前回の access の後に
  R と range の重なる別の資源が access されたなら、ALL_COMMANDS → ALL_COMMANDS・`MEMORY_READ | MEMORY_WRITE` の memory barrier と R の oldLayout = UNDEFINED。R と重なる資源の記録の layout も
  UNDEFINED にし、重ならない資源には触れない([`recordAccess()`](../src/core/vkcore/render_target_layout_tracker.cpp#L141))。frame の中の交替・同じ frame の中の再訪・frame の境界・前の住人が走らない frame を
  同じ規則で覆う。新しい generation の image の最初の access にも frame の最初の barrier が出る(RC-7b の「新しい token は barrier なし」は無くなった)。transition も同じ layout の依存も同じ関数を通す
  (③ の修正の報告の 3: 前の frame の `GENERAL` を保つ image への compute の最初の access で barrier が出ていなかった)。
- **resize・置き換え・復元**: resize の 2 つの道(窓は再 compile、XR / test は container だけで作り直す)、generation の置き換え、失敗の復元のどれも、保持した不変の入力と作り直した実画像の requirements から
  詰め直す。不変なのは資源の識別・禁止の対・採った同じ offset の集合・詰める順で、鍵・region の所属・dedicated は配置のたびに作り直す(確認レビュー 5)。作り直しで自動 / `prefer` の重なりが成り立たなければ
  理由つきで分け、採った `require` が成り立たなければ `render_target.alias_repack_unsatisfiable@1` で transaction を戻す。`require` の組は (文書, 組名) で決める(③ の修正の報告の 1)。
- **計測の欄の意味**: `get_frame_plan` の `memory_summary` は、payload(`payload_bytes`。texel × extent の今の式)・計画の見積もり(`planned_region_bytes` / `planned_region_count`。計画の facts の region の和で、
  region の外の確保は入らない)・実際の確保(`total_bytes` と `actual_allocation_count` / `actual_allocation_bytes`。一意の VMA allocation ごとに size を 1 回。重なる住人の requirements の和ではない)を分けた。
  `actual_placement {extent, revision, regions, overlaps, …}` は登録の結果で、compiler_only と CPU の代役では `null`(未計測)。VMA の allocation と VkDeviceMemory の block は別の単位で、block の予約量は測っていない。
  frame ごとの alias の barrier の数は frame plan に載せず tracker の `frameAliasDependencyCount()` で読む(載せると WP366 の frame plan の完全一致が崩れた。③ の修正の報告の 5)。
- **example の実確保(C1)**: example・flat・1920×1080・NVIDIA GeForce RTX 5080(`driverVersion` 2559967232)で、21 枚の登録が一意の VMA allocation **20 → 4**、bytes **135,299,072 → 128,286,720**
  (129.03 → 122.34 MiB、7,012,352 減)。参照は RC-7b の HEAD `9cd7bb971` の example の 21 画像の登録の集合(名前・format・extent・mip・layer・usage・samples・dimension)を test に固定し、同じ device で
  各画像を単独で照会して `g_emissive` / `Bloom_Threshold_RT` の 1 組を 1 回とした値(古い binary の再実行ではない。③ の修正の報告の 4)。数と bytes は別々に assert。4 個の内訳は 18 住人の region 1 つ
  (計画の見積もりも実際も 92,897,280 bytes、`memoryTypeBits` 3・最大の alignment 1024・VMA の選んだ memoryTypeIndex 1)と region の外の 3 つ(`opaque_color` / `opaque_depth` は最初の use が snapshot の
  transfer、`display` は display の除外。35,389,440 bytes)。地図 §7.3 の「全 21 枚を 1 heap」の机上値 87,091,200 との差 41,195,520 は、region の外の 3 枚の payload 33,177,600 と、実際の requirements と
  alignment の差 8,017,920(実装者の報告。driver の内部の tiling / 圧縮の内訳は取っていない)。81 MiB は求めていない(決定 7)。
- **計画の差(C5)**: 実装前の HEAD の 16 構成(`<作業ディレクトリ>/rc7c_baseline/rc5`、compiler_only・facts なし)と比べ、意味の欄(実行順・scope・representation・format・samples・attachment の ops・usage・寿命・
  保持の理由)は一致する。変わるのは配置の派生の欄(`alias_groups`・`memory_regions`・`memory_packing`・`alias_candidates` の未確定・memory の集計・fingerprint)だけ。facts が無いので旧い組は全部別の確保に
  戻る(HDR の `g_emissive` / `scene_ldr_in`、extras ありの `gbuffer_normal` / `probe_image`、両方ありの `g_emissive` / `probe_image` と `gbuffer_albedo` / `scene_ldr_in`。資源ごとの説明は
  `<作業ディレクトリ>/rc7c2_c5_details.json`)。A5・F4・B4 も配置の派生の欄だけを新しい形で比べるよう移した(意味の欄の除外は増やしていない)。
- **移行の注意**: facts を渡さない compile(compiler_only)では共有しなくなった。example の `g_emissive` / `Bloom_Threshold_RT` の組も compiler_only では別の確保(`frameplanner_test` の example は
  21 枚 21 確保・134,070,720 bytes。前の 125,776,320 との差 8,294,400 は旧い 1 組の重複の控除)。共有の効果を見る CPU の test は作り物の facts を注入する形に移し、facts なしの対照は専用の test に分けた。
  golden の renderer の trace は `skeletal_toon` / `usd0b_static_geometry` の `final_layouts` の 6 資源(`Bloom_Downsample_H_0〜2_RT`・`Bloom_Threshold_RT`・`gbuffer_normal`・`gbuffer_worldpos`。後の住人が実際に range を重ねる物。RC-7b の HEAD との差を fixture で確認)が `shader_read_only_optimal` → `undefined` に変わった(画像と RGBA8 の hash は
  変えていない)。
- **RC-7 に残した物**: memory の種類ごとの確保(RC-7d。transient / tile-local を同じ allocator の道へ、lazily allocated は作り物の facts の CPU test まで)、使われない RT の確保の除去(RC-7e)。
  depth-stencil の共有は RC-7b のまま外(どの段で扱うかは未定)。未確認: `maintenance4` を支持しない実機での起動の失敗、linear tiling の assert の subprocess の test、overflow の限界値、
  大きさだけで `memoryTypeBits` / dedicated が変わる実機(作り物の facts で道だけ確かめた)、native WSI の実窓の resize(window の道は headless の facts の seam で通した)、lazy memory の実 GPU。
  → memory の種類ごとの確保は RC-7d で済んだ(下)。lazy memory の実 GPU は RC-7d でも未検証。

<a id="rc7d-memory-kinds"></a>

**RC-7d で変わった(2026-11-01、`c941a8346`、branch `agent/rc7d`。memory の種類ごと)。**仕様は `<作業ディレクトリ>/rc7_spec.md` §5(仕様レビュー 10 件の反映)、実装者の報告は `<作業ディレクトリ>/rc7d_report.md` とレビューの指摘 3 件の修正の報告 `<作業ディレクトリ>/rc7d_fix_report.md`。
この段の行番号は `c941a8346` の物。ファイルごとの所在は [棚卸しメモ §2-3 の RC-7d](design_plan_layer_inventory.md#rc7d-implementation)、作者向けの形は manual [第6章の「memory の種類」](manual/06_rendering.md#memory-kinds)。

- **変えた挙動**: desktop(LAZILY_ALLOCATED の memory type の無い device)では transient / tile-local の RT も region に入り、実確保が減る。LAZILY_ALLOCATED の type がある device では transient / tile-local は
  region の外で lazily allocated memory に dedicated で個別に置く。representation の選び方(候補・cost)は変えない。
- **種類(kind)の規則は 1 つ**([`imageMemoryKind()`](../src/project/memoryregionpacking.cpp#L9)。planner・generation・verifier・wire・runtime が呼ぶ): 最終の usage に `TRANSIENT_ATTACHMENT` の無い image は
  memory types によらず `ordinary`(Vulkan では TRANSIENT の無い image の bits に LAZILY の type は入らない)。TRANSIENT の image は、requirements の `memoryTypeBits` の中に LAZILY_ALLOCATED を持つ type が
  1 つでもあれば `lazy`、無ければ `ordinary`。device 名や「tile GPU か」の旗は使わない。memory types が空なら TRANSIENT の image だけ `unknown`(理由 `lazy_kind_unknown`。region に入れない)、
  materialized は RC-7c のまま。planner は transient / tile-local の候補の usage に TRANSIENT を足してから照会と判定をする(修正の報告の 1。`engineImageUsage()` は tile-local の usage を TRANSIENT 以上に変えるので使わない)。
- **facts の写し**: `VulkanTargetPlan` に facts の memory type の写し(`memory_types`: property flags と heap index の列)を持たせ、JSON にも `memory_types` と `image_memory_facts {transient, memory_type_bits}` を
  書く([その所](../src/project/targetrenderplanning.cpp#L4688))。verifier と wire はそこから種類を作り直し、package の自己申告を信じない。wire は `transient` の値に住人の representation(transient / tile-local)を
  OR してから判定する(修正の報告の 2)。
- **門を開けた所**: runtime pattern の `allow_alias`(storage の条件を外した。[`isRuntimeAliasCandidate()`](../src/core/renderingpass/renderingsamplecount.cpp#L79))、planner の `applyCandidateAliasPolicy()`、
  verifier の住人の representation と `aliasable`、generation の resolver の組の検査、登録の container の検査。どれも「materialized」を「materialized、または ordinary の transient / tile-local」に広げた。
  `stored` を求めるのは materialized の住人だけ。`require_store` の式は変えない。他の適格の条件(samples = 1・resolve なし・history でない・保持の理由なし・最初の use が全 subresource を load なしで書く・
  manual NativeScope・融合後の禁止の対)はそのまま。stencil を持つ transient は既存の条件で外のまま。安全性は RC-7c の禁止の対から来て、「transient は 1 つの scope に収まる」は仮定しない。
  transient の候補が materialized に倒れた資源が region に入るのは意図した差。
- **lazy は region に入れない**: planner の packing の入力([その所](../src/project/targetrenderplanning.cpp#L3608))と `planGenerationMemory()`([その所](../src/project/targetrenderplanning.cpp#L4803))で外し、
  dump に decision `pelican.plan.alias_ineligible@1` の `lazy_memory` / `lazy_kind_unknown`。`alias_group` の `require` に lazy の資源があれば `alias.lazy_memory@1`([その所](../src/project/targetrenderplanning.cpp#L4163))、
  種類が分からない住人の `require` は失敗の文 `alias.lazy_kind_unknown@1` の `target_planning.requirement_unsatisfied@1`。`prefer` と自動は理由つきで外す。
- **verifier と wire**: complete の verifier は住人ごとの検査の最初に種類を見る(`memory.lazy_in_region@1` / `memory.lazy_kind_unknown@1`。`verifyAliasMember` と網羅の検査より前。
  [その所](../src/project/vulkancompletephysicalplan.cpp#L1858))。`stored` の不一致は `memory.stored@1`。wire も住人の representation・materialized の `stored`・種類を検査する([その所](../src/project/physicaltargetplanwire.cpp#L717))。
- **runtime の確保**: region の確保(`allocateImageRegion`)は今のままで、ordinary の transient / tile-local もこの道。個別の確保は全部 [`allocateRenderTargetImage()`](../src/core/renderingpass/rendertargetcontainer.cpp#L247)
  の 1 つ(最初の登録、region から外れた資源、container だけの resize の道、runtime の layer の変更。resize で storage mode を写すようにした)。種類は実画像の requirements の bits・物理 device の memory properties・
  create info の TRANSIENT から決める(storage mode に頼らない)。lazy は `VMA_MEMORY_USAGE_GPU_LAZILY_ALLOCATED`(仕様では LAZILY の required と dedicated を含む。VMA の振る舞いはこの repo では読んでいない)。
  vk-hpp の `FeatureNotPresent` → `lazy_type_unavailable`、`OutOfDeviceMemory` → `lazy_allocation_failed` で AutoPreferDevice の個別確保に戻り、他の例外は戻さない。ordinary の個別確保は AutoPreferDevice で
  preferred の flags なし(RC-7c までの LAZILY の preferred は無くなった)。repack は種類を作り直さない(bits が計画の鍵と違えば今の `memory_key_changed`)。
- **観測**: `actual_placement` の allocation ごとに、選んだ memory type の property flags・kind・VMA に渡した usage / requiredFlags / dedicated・戻りの理由と、戻った後の実効の usage / requiredFlags / dedicated
  ([その所](../src/core/renderingpass/rendertargetcontainer.cpp#L993))。lazy の allocation の size は `allocation_bytes` にそのまま足し、commit 量は測らない(`vkGetDeviceMemoryCommitment` は入れない)。
  `planned_region_bytes` は ordinary の transient / tile-local を含むようになった。rendering の中の取り直しを数えるため、tracker の観測に `inside_rendering` を足した(barrier の出し方は変えていない)。
  Studio の列は足さない(観測の JSON に出るだけ。表示は後追い)。
- **test の差し込み口**: 登録の option `runtime_memory_types_for_testing`(`runtime_image_requirements_for_testing` と同じ形)。desktop の GPU で種類を lazy に偽り、本物の VMA が `FeatureNotPresent` を返して戻る道を通す。
- **Freeze**: transient / tile-local の住人には `alias_group` を `prefer` で書く(materialized は `require` のまま。[その所](../src/devstudio/model/frameplanpolicy.hpp#L67))。desktop で Freeze した project が tiler で
  `alias.lazy_memory@1` にならないため(モバイルも最終目標)。
- **example**: transient / tile-local の RT が無いので変わらない(RTX 5080 で 4 allocation・128,286,720 bytes。21 枚全部 materialized で、全資源の契約と offset が RC-7c と一致。実装者の報告)。
- **計画と登録の差**: RC-3c2a C1 の fixture で、flat の保持されない `depth`(ordinary の transient、1 layer)が region に入り allocation が 10 → 9(XR と合わせた登録の `depth` は 2 layer・export の
  materialized で region の外、10 のまま)。golden の renderer の trace の `final_layouts` は 15 ケース(`offscreen_depth` 8・`velocity_depth` 7)で `depth_attachment_optimal` → `undefined`(保持されない depth の
  範囲を後の住人が使い直し、tracker が旧い layout を無効にするため)。node の順・attachment の ops・canonical の frame plan の trace と RGBA8 の 52 hash は不変。hidden compare 4 種(A5 / F4 / B4 / C5)は
  新しい `memory_types`・`image_memory_facts` と lowering graph の pattern の `allow_alias` を配置の派生の欄として除いた(意味の欄の除外は増やしていない。C5 の資源ごとの差は `<作業ディレクトリ>/rc7d_c5_details.json`)。
- **RC-7 に残した物**: 使われない RT の確保の除去(RC-7e)。depth-stencil の共有は外のまま。未確認: 本物の lazily allocated memory の確保と commit(手元に tiler が無い)、`OutOfDeviceMemory` での戻りの
  強制、MoltenVK での種類の判定、他の GPU / driver。

## 3. B: alias を区間詰めに一般化する(本丸)

### 3.1 現状

- 計画: [`deriveLegalAliasCandidates()`](../src/project/targetrenderplanning.cpp#L3124) は **同 representation・format・samples・view layout・mip・layers・dimension・extent** かつ寿命非重複の組だけを候補にする。
- runtime: [`allocAliasingImage()`](../src/core/vkcore/core.cpp) は owner の `VkImageCreateInfo` をそのまま複製して owner の allocation に bind
  (offset 0、同サイズ)。[`rendertargetcontainer.cpp:710`](../src/core/renderingpass/rendertargetcontainer.cpp#L710) も format / usage / samples / mip の一致を要求。
- 結果、「大きい gbuffer の跡地に小さい bloom を置く」ができない。
  → RC-7c で解けた(`allocAliasingImage()` は無くなり、region と offset の配置になった。[§2 の末尾の RC-7c](#rc7c-packing))。

### 3.2 提案

```text
planner(src/project/targetrenderplanning、純ロジック)
  入力: 資源ごとの bytes(format × extent × layers × mips、alignment)と寿命 [first_use, last_use]、aliasable、no_alias
  処理: 寿命が重ならない資源を 1 つ以上のヒープに区間詰め(interval packing。first-fit で十分、決定的順序)
  出力: alias_groups → memory_regions [{heap, offset, size, resources[]}]
        + alias_reuse barrier の要求 [{from: 前の住人の最終アクセス node, to: 次の住人の最初の書き込み node, heap, offset}]
runtime(rendertargetcontainer / vkcore)
  ヒープごとに allocation 1 つ(VMA)。各 image は vmaCreateAliasingImage2(allocation, offset, createInfo) で bind。
  VK_IMAGE_CREATE_ALIAS_BIT は既に付けている。
  memoryTypeBits はヒープ内の全 image で AND を取り、空なら計画段階で拒否(device facts として planner に渡す)。
renderer(executePlannedFrameGraph)
  alias_reuse barrier: 前の住人の最終アクセス → 次の住人の最初の書き込みに execution + memory dependency、
  次の住人の oldLayout = UNDEFINED(内容は引き継がない)。最初の使用が `load` の資源は共有不可(既存の aliasable 判定と同じ)。
```

- `no_alias` / `isolate` / `hazard_stress` profile はそのまま効く(詰め方の撹拌にも使える)。
- 期待値: example で 120 → 85 MiB 前後、A と合わせて 81 に近づく。RT を多く宣言する feature(bloom、planar reflection)ほど効く。
  **「feature は RT の枚数を気にせず宣言してよい」を成立させる手段**でもあり、preset レシピ化([design_render_preset_feature_recipe.md](design_render_preset_feature_recipe.md))と相性がよい。
- 正しさの難所は barrier。planner が `alias_reuse` を**自動で出す**形にして、手書き barrier ゼロの原則を崩さない。
  frame をまたぐ再利用(frame N の最後の住人 → frame N+1 の最初の住人)も同じ規則で、frame 先頭の layout 遷移に含める。
- → **RC-7c で変えた(2026-10-31、`27a919f4a`。仕様 §4.3)**: planner は `alias_reuse` の表を出さない。重なりの関係は実画像の size で変わり(resize)、計画の時の表は実際の配置の写しにならないため。planner が出すのは `memory_regions`(計画の facts での region と住人の offset)と packing の不変の入力(region の住人と鍵・重ねてはいけない対・同じ offset の集合・詰める順)で、runtime が実画像の requirements で詰め直し、barrier は「frame の最初の access か、前回の後に range の重なる別の資源が access された」ときの広い barrier + UNDEFINED を runtime の tracker が出す。実際の重なりの関係は登録の結果として `get_frame_plan` の `memory_summary.actual_placement` に出る。手書きの barrier がゼロの原則はそのまま。memoryTypeBits は AND を取らず完全一致を鍵にし、合わなければ拒否ではなく別の region(利用者の決定 3)。bind は `vmaCreateAliasingImage2` ではなく、未 bind の image に `vmaBindImageMemory2(allocation, offset, image)`。詳しくは [§2 の末尾の RC-7c](#rc7c-packing)。

### 3.3 やらないこと

- format / extent が違う image の**内容**の共有(alias は内容を引き継がない)。
- 複数 queue / async compute(graphics queue 1 本のまま)。
- tile-local / transient 候補の変更(別件。desktop では選ばれない)。

## 4. C: 宣言・パス単位の無駄

- **dead pass elimination**: `unread_output` 診断([`findFramePlanUnreadOutputs()`](../src/core/renderingpass/frameplanner.cpp#L1689))は出すだけ。
  出力が全部 unread で副作用(snapshot / readback / 外部 export)を持たないパスを実行対象から外す。feature 除外や編集中に出やすい。
- **store の自動 discard**: 最終読み手の後の `store_op` を `discard` に(帯域)。寿命解析は既にあるので planner で決められる。
  既定は color 全 `store`、depth `dont_care`([`passattachmentoperations.cpp:7`](../src/core/renderingpass/passattachmentoperations.cpp#L7))。
- 宣言だけされて一度も使われない RT は、lifetime.used = false の資源として既に計画から落ちている(確保されない)ことを確認する。
  → RC-1b で確認(§5): 計画には出ないが、runtime の登録は定義を全部登録するので確保されている。

## 5. 段階

| 段 | 内容 | 規模 | 検証 |
|---|---|---|---|
| 0 | 計測: `get_frame_plan` に `memory_summary {total_bytes, peak_live_bytes, peak_node}` と資源ごとの `bytes` を出す。Studio の Frame Plan に表示。**RC-1 で済み(`631caac21`)** | 小 | planner の単体テスト(GPU 不要)。手計算の 120 / 81 MiB と一致 → 実測は 119.95 / 81.08 MiB(合計のずれは §1 の注) |
| 1 | A: required / require_store の末尾延長の理由を特定し、必要な経路だけ opt-in に。**寿命の意味は RC-6c で済み(`be795c9a5`。graph の外の読み手 7 つを理由に)。名前の readback の opt-in は RC-7a で済み(`2d1715cf3`)。実確保の削減は RC-7b〜7e** | 小〜中 | 物理計画の fixture 差分、golden 不変、headless 読み出し・picking・capture の回帰 |
| 2 | B: 区間詰め alias(planner → runtime → barrier)。**RC-7b〜7e に分けた(下の「RC-7 の段」)。同じ形の共有の条件の拡張は RC-7b で済み(`0091c071c`)。形の違う区間詰めは RC-7c で済み(`27a919f4a`。`alias_reuse` の表は作らない)。memory の種類ごとは RC-7d で済み(`c941a8346`。lazily allocated の実確保は未検証)** | 大 | planner 単体(詰め結果の決定性、no_alias、memoryTypeBits 拒否)、GPU gate 全数、`hazard_stress` での揺さぶり、validation layer |
| 3 | C: dead pass elimination、store 自動 discard | 中 | frame plan の fixture、golden 不変 |

0 と 1 は純ロジック中心。2 は GPU gate 必須(Windows)。

段 0 の完了記録(RC-1、2026-10-23、`631caac21`。検証は実装者の報告): 非 GPU の家族 305/305 緑(RC-1 の 7 本を含む)。`imageResourceBytes()` を 0 にした対照で 7 本中 5 本が赤。
GPU の `hybrid_v1 preset registers and renders a headless frame` に足した assert(arena と plan の bytes の照合)は報告の時点で未実行。

RC-1b の完了記録(2026-10-23、`504a6492a`。検証は実装者の報告と統合側): codex の RC-1 レビューの数え漏れ(runtime が取り消した複数 graph の alias、計画に出ないが登録が確保する RT、`setRuntimeArrayLayers` の後の arena の古い値)を直した。`memory_summary.total_bytes` は「この計画の image の和」から「公開中の世代の GPU arena に登録された RT 全部を今の metadata で 1 回ずつ(runtime の alias の組 = `aliasGroup(id)` ごとに 1 回)」に変わり、`unused_bytes` / `unused_resources` / `peak_graph` を足した。example の 1920×1080 は 125,776,320 / 85,017,600 のまま。dump の arena の RT の `declared_bytes` は書き出す時点の metadata の値。数えられない計画の image は `frame_plan.memory_unmeasured@1`。非 GPU は触った 6 つの exe の 903/903 緑(`-R "RC-1"` 10/10)。GPU は `get_frame_plan` を呼ぶ 10 本(上の hybrid_v1 を含む)を統合側で実行して緑。
どの graph も使わない RT が確保されていることを計測で確かめた(`unused_resources` に出る)。その確保の除去は RC-7 の範囲(この記録の時点で台帳に節は無い)。

段 1 の寿命の部分の完了記録(RC-6c、2026-10-30、`be795c9a5`。検証は実装者の報告): 内容は [§2 の末尾](#rc6c-lifetime)。非 GPU 745 件(render_pipeline 501・target planning 46・frameplanner 45・
Studio の Frame Plan 153)と GPU 5 件(NativeScope 2・complete の順序の拒否と融合の実行 2・XR multiview と depth export 1)が緑。A5 は baseline を記録し直さずに 16 構成が一致
(寿命の欄と寿命から作る fingerprint だけを除いて比べ、除く前に資源ごとの新しい `last_use` と `retain` を期待値の表で検査)。対照: 共通の関数から `picking` を外すと E1 が赤、
verifier だけ一律の延長に戻すと正常な package の検証が赤。golden の全画像と GPU・シナリオの全数は統合側(報告の時点で未実施)。

<a id="rc7-stages"></a>

**RC-7 の段と利用者の決定(2026-10-30)。**仕様は `<作業ディレクトリ>/rc7_spec.md`(冒頭と §2。RC-7b〜7e は骨組みで、それぞれの前に詰める)。段 1 の残り(実確保の削減)と段 2 を RC-7 として次の順に分けた。

1. **RC-7a** readback の約束(✅ 済み。下の完了記録)。
2. **RC-7b** 同じ形の共有の条件を広げる(offset 0・既存の barrier のまま。広げた後の画像の契約の互換と frame の境界での使い回しの同期を完了の条件に入れる)(✅ 済み。下の完了記録)。
3. **RC-7c** 形の違う区間詰め(retain の資源への TRANSFER_SRC の自動の付け足し、device facts の memory requirements、memory の種類ごとの heap と offset、`alias_reuse` の barrier)(✅ 済み。下の完了記録。`alias_reuse` の表は作らず、runtime の range の規則にした)。
4. **RC-7d** memory の種類ごと(transient / tile-local を同じ allocator の道へ。lazily allocated は作り物の device facts の CPU test まで)(✅ 済み。下の完了記録。lazily allocated の道は作り物の facts の CPU test と、desktop で種類を偽って戻りを見る GPU test まで)。
5. **RC-7e** 使われない RT の確保の除去。

利用者の決定(仕様の冒頭の 7 点の要約): (1) `retain: true` は「graph の後に名前で読める」の約束で、planner が materialized・store・TRANSFER_SRC を足す(TRANSFER_SRC は RC-7c から。
上の RC-7a の節)。成り立たなければ `require` は名前付きの拒否、`prefer` は不採用で readback も拒否。`retain` の無い中間の RT の名前の readback は拒否。test は観測する資源だけに
`retain` を書く。(2) `retain: false` は下位の readback の指定だけを打ち消し、engine の保持(7 つの理由)は消せない。(3) memory の種類が合わない・facts が不明・dedicated が必須の物は、
自動の共有をやめて分けた確保に戻す。明示の `require` だけ拒否。(4) graph / variant をまたぐ配置は generation 全体で安全な 1 つにし、共有を外した理由は dump に出す。(5) 使い回しの
barrier は最初は既存並みに広く、精密化は別の段。(6) dead pass の除去と store の自動 discard は RC-7 の外(§4)、使われない RT の確保の除去は RC-7。(7) 合格は heap / offset の適法性・
実確保の減少・golden 不変・validation layer の警告なし。

RC-7a の完了記録(2026-10-31、`2d1715cf3`、branch `agent/rc7`。検証は実装者の報告と統合側): 内容は [§2 の末尾の RC-7a](#rc7a-readback)。実装者の報告(2 回目の修正の後)では、
RC-7a の test 24 件(期待どおりに失敗する対照 1 件と SKIP の対照 1 件を含む)・A5 と F4 の各 16 構成・移行した GPU の test file が緑。対照: 拒否を外すと F1、派生の materialize・store の付け足し・
`readback` の寿命の理由のどれかを外すと F2 の対応する検査が赤。統合側で GPU 全数・非 GPU 全数・シナリオ 23 本が緑(既知の環境由来の赤のみ)。既存の SKIP 3 件(画像の基準が無い 2 件と
CB-1 の 08)は未検証。

RC-7b の完了記録(2026-10-31、`0091c071c`、branch `agent/rc7b`。検証は実装者の報告と修正の報告 2 本): 内容は [§2 の末尾の RC-7b](#rc7b-sharing)。仕様 §3.4 の受け入れごとに:

- **B1 新しく共有される**: GPU の「RC-7b B1 B2 B3 B5 expanded image sharing survives frames and edits」(`headless_render_test`)が 32×32 の 3 形式(`R8G8B8A8_UNORM` + `TRANSFER_SRC`、
  display でない `B8G8R8A8_UNORM`、`D32_SFLOAT`)で、各形式の共有される 3 枚が generation の placement の同じ組・同じ allocation・別の VkImage であることを見る。実 allocation は VMA の
  `getAllocationInfo().size`(test の口 `imageAllocationBytes()`)と allocation の一意数で測り、各形式とも共有なし 3 個 / 12,288 B → 共有 1 個 / 4,096 B(2 個 / 8,192 B 減)。`retain` は
  寿命の中で写した観測用の出力 2 つだけに付け、共有される資源自身は名前で読まない。CPU: 「RC-7b B1 B6 identical effective image contracts admit depth transfer and BGRA」。
- **B2 画素**: 観測用の出力が赤 / 緑の RGBA で全画素一致(3 frame の連続 submit の後、reset の後、さらに 2 frame の各 frame、共有の解除の後、失敗の復元の後)。golden の全画像と GPU の全数は
  統合側(報告の時点で未実施)。
- **B3 同期**: 同じ GPU の test で a 同じ frame の中の切替、b 3 frame の連続 submit の frame 先頭、c reset の後の pending と新しい generation / token を分けて、d production の view / once の
  scheduler で住人を記録しない frame を挟む、e 他の住人を記録せず同じ画像・同じ layout の attachment clear を続ける。依存の token・位置・ALL_COMMANDS・`MEMORY_READ | MEMORY_WRITE`・
  次の画像の oldLayout = UNDEFINED を検査。Khronos の validation と synchronization validation を有効にしたことを assert し、debug messenger が拾った warning / error は 0 件(共有した資源、
  共有を全部外した 3 frame の対照とも。前からある frame 間の警告も出なかったので、別の WP に切り出す物は無い)。記録は `<作業ディレクトリ>/rc7b_fix2_validation.jsonl`。
- **B4 計画の差**: §2 の末尾の RC-7b の「計画の差」。A5(`[rc6-baseline-compare]`、3,531 assertions)・F4(`[rc7-baseline-compare]`、466)・B4(`[rc7b-baseline-compare]`、450)の各 16 構成が緑。
  example の 1 組と total / peak は固定の assert で維持。`frameplanner_test` の example の test は、RC-6c の前の一律の末尾延長を、RC-7b で変わる pattern の欄からでなく当時の資源の集合
  (`display`・`lit_color`・`offscreen_depth`・`opaque_color`・`opaque_depth`・`swapchain`)を固定して作るようにした(数値の期待は不変)。
- **B5 編集**: `{a,b,c}` → `{a,c}+{b}` の分割、3 つの独立した allocation への解除、再結合。前の snapshot・ID・画像を保ち、submit と次の置き換えの間に waitIdle を入れない場合も見る。
  `after_runtime_prepare` の故障の注入で名前の binding・allocation・readback の許可が戻り、shader の欠けで失敗しても公開の snapshot は変わらない。
- **B6 policy と verifier**: 新しく適格な 3 形式で `no_alias`・`isolate`・`alias_group` の `require` / `prefer` が効く。`retain` を書かない 2 variant の usage / storage の合意(自動と `prefer` は解除、
  `require` は名前付きの拒否)。complete package の verifier が、保持・最初の use・usage・scope・manual / unchecked の NativeScope・MSAA・resolve・寿命の重なりを壊した package を、
  fingerprint の不一致の手前の拒否ではなくそれぞれの `alias.*@1` で断る(「RC-7b first-use controls reach the planner and independent verifier」「RC-7b verifier rejects native boundaries samples and
  overlapping residents」ほか)。
- **対照**(実装者の報告): 条件 1(8 つの理由ごと)・2(9 入力。planner と verifier の両方で `alias.first_use@1`)・4(不一致と facts 不明)・5・6 と alias の barrier をそれぞれ外す変異で
  対応する検査が赤、戻して緑。修正の報告 2 本も、修正を外すと新しい test(`[rc7b-fix-variants]`・`[rc7b-fix-graphs]`・`[rc7b-fix-retry]`)が赤。
- **既存の test の変更**: RC-3c2a C1 の GPU の fixture(`generationresources_device_test`)は、前は `TRANSFER_SRC` で共有から外れていた中と末の 6 枚に明示の `no_alias` を書き、a / b / c の組だけを測る
  (flat 単独 3 組・12 確保、XR と統合 2 組・13 確保は不変)。`renderingsamplecount_test` の PO-1 の usage の理由の test は「RC-7b differing effective usages prevent runtime aliasing without
  forcing storage」に変わり、graph 間の統合の 2 件は各 graph の計画と dump が非共有であることを見る形にした。
- **触った家族**(2 回目の修正の後): project_planning 116 件、render_planning_core 118 件、render_pipeline 514 件(513 緑・既存の SKIP 1 件 `rc5s.fixed_head_packages_unavailable@1`)、
  RC-3c2a C1 の GPU と RC-7b の GPU が緑。
- **統合側の gate**(1 回目の修正の後の tree。2 回目の修正は最終の照合の入力 1 か所だけ): GPU 全数 476 中 475 緑(既知の環境由来 `player_arguments_ds1_path` のみ)、非 GPU 全数は既知の 29 件と名前で一致、
  シナリオ 23 本が緑、A5・F4・B4 一致。既存の SKIP 3 件(画像の基準が無い 2 件と CB-1 の 08)と他の GPU / driver は未検証。

RC-7c の完了記録(2026-10-31、`27a919f4a`、branch `agent/rc7c`。検証は区切りごとの実装者の報告と修正の報告、統合側の gate): 内容は [§2 の末尾の RC-7c](#rc7c-packing)。仕様 §4.7 の受け入れごとに:

- **区切り**: ① 契約(create info の 1 本化・`maintenance4`・facts の照会・packer。修正 2 件: alias の住人の作成を owner の実画像の契約から、live の再利用の前に usage だけを取る)→ ② 計画(planner の出力・
  合意・TRANSFER_SRC・complete / wire / fingerprint・Studio・計測の分け方。修正 7 件: runtime の layer の除外・buffer の `retain`・(variant, graph) の provenance・`prefer` の一括の決定・禁止の判定の共通化と
  wire の再構成・golden の trace・facts の注入)→ ③ runtime(実画像の配置・region と bind・range の tracker・resize・置き換え・復元・heap の寿命。修正 5 件: `require` の組の鍵に文書・batch で live の image を
  先に解決・全 access の道を 1 つの関数に・C1 の参照を RC-7b の 21 画像の集合に固定・frame ごとの数を frame plan から外す)。各区切りで wip を commit してレビュー、着地は ③ の後の 1 回。
- **C1 実確保の減少**: example・flat・1920×1080・NVIDIA GeForce RTX 5080 で、一意の VMA allocation **20 → 4**、bytes **135,299,072 → 128,286,720**(数と bytes を別々に assert)。参照は RC-7b の
  21 画像の登録の集合を test に固定し、同じ device で照会した値(「RC-7c C1 example actual VMA allocations and bytes beat RC-7b reference」。`opaque_depth` を除いた集合が照合に落ちる対照つき)。
  payload の机上の値との差は資源ごとに報告(§2 の末尾の RC-7c)。
- **C2 packing の適法性**(CPU): 禁止の対の range・alignment の倍数・heap の size・同じ offset・seed 0〜7 の再現と適法・形と format の違う 4 資源の部分の重なり・鍵の違いで region が分かれる・dedicated と
  不明の除外・`require` の slot が first-fit の順に負けない(A[0,0]・X[1,3]・B[2,2])・同じ形では RC-7b の住み分けと同じ・計画の facts と実画像の requirements の一致(GPU、5 通りの create info)。
- **C3 画素と同期**(GPU): 同じ allocation・別の VkImage・offset の値。同じ frame の交替、同じ frame の中の再訪、frame の境界、前の住人が走らない frame、別 offset で同時に生きる A / B と両方に部分的に
  重なる C(A / B の layout と画素が壊れない)、未完了の 2 つの submit を host の timeline の gate で保ったままの container の resize・窓の resize(headless の facts の seam)・generation の置き換え・失敗の復元。
  synchronization validation の warning / error は 0 件(region の memory に関わる物も 0)。
- **C4 golden**: 画像と RGBA8 の hash は不変。renderer の trace の `final_layouts` だけが 2 ケース × 6 資源で `undefined` に変わった(§2 の末尾の RC-7c の「移行の注意」)。
- **C5 計画の差**: 16 構成で意味の欄が一致し、配置の派生の欄だけが変わる(「RC-7c C5 sixteen fixed snapshots retain every semantic field」、`[.]`)。作り物の facts の example の住人の表と、
  今まで不採用だった `prefer` の `retain` の意図した差(`write_only_scratch`)は実装者の報告。
- **C6 policy・verifier・合意**: `alias_group` の `require` / `prefer` が同じ offset を求める、`no_alias` は region に入らない、一部の graph にだけ現れる資源の適格、graph / variant の合意で外れた対の
  自動 / `prefer` の解除と `require` の名前付きの拒否、verifier の `memory.*@1` をそれぞれの code で、TRANSFER_SRC の自動の付け足し(宣言が `COLOR_ATTACHMENT` だけの資源の名前の readback が通る GPU、
  再試行で落ちた `prefer` に残らない CPU、device が支持しない組の拒否)。
- **対照**(実装者の報告): ① 禁止の range・alignUp・slot 化、② 禁止の range・verifier の鍵の一致・slot 化・generation の照合を最初の graph だけにする、② の修正 7 件、③ frame の最初の規則・取り直しの規則・
  禁止の対の重なり・bind の前の検査・extent の退役の defer、③ の修正 5 件を、それぞれ 1 つずつ外して対応する test が赤、戻して緑。取り直しの規則の赤は barrier の観測の欠落で、画素と validation での赤は
  主張していない(報告の明記)。
- **触った家族**(③ の修正の後): render_planning_core 130・render_pipeline 518(既知の SKIP 1)・project_planning 127、studio_cpu 1,014、hidden の A5 / F4 / B4 / C5(4,565 assertions)、選んだ GPU 17 件が緑。
- **統合側の gate**: GPU 全数 484 件中、赤は既知の `player_arguments_ds1_path` の 1 件だけ。非 GPU 全数は既知の 29 件だけが赤。シナリオ 23 本中 23 本が緑。基準との比較 A5 / F4 / B4 / C5 が緑。
  未確認: `maintenance4` を支持しない実機、大きさだけで鍵や dedicated が変わる実機、native WSI の実窓の resize、lazy memory の実 GPU、VkDeviceMemory の block の予約量、他の GPU / driver。

RC-7d の完了記録(2026-11-01、`c941a8346`、branch `agent/rc7d`。検証は実装者の報告とレビューの指摘 3 件の修正の報告、統合側の gate): 内容は [§2 の末尾の RC-7d](#rc7d-memory-kinds)。仕様 §5.5 の受け入れごとに:

- **D1 tiler の作り物の facts**(CPU): transient attachment・local read を有効、memory types = {DEVICE_LOCAL, DEVICE_LOCAL | LAZILY}、TRANSIENT の image の bits = 3・他 = 1 の facts で
  `runRenderCompilerProgram`(2 回の計画を通る道)を回す。`scratch` が transient、`local_source` が tile-local であることを先に assert し、どちらも lazy で region の外(`lazy_memory`)、materialized の
  region は保たれ、`alias_group` の `require` に lazy の資源で `alias.lazy_memory@1`。memory types が空なら TRANSIENT の image だけ `lazy_kind_unknown`、materialized は共有のまま
  (「RC-7d D1 D2 compiler derives lazy kind and exact final usage」)。
- **D2 desktop の作り物の facts**(CPU): LAZILY の type が無く bits が usage によらず同じ facts で、ordinary の `scratch` / `local_source` が materialized と同じ region に入り、同じ scope の
  `local_source` / `local_output` の range は重ならない。照会の callback で照会を記録し、最後の合意の照会の usage と登録の create info の usage を image ごとに 1 対 1 で照らす(修正の報告の 3。
  前の「どれかの照会と一致」の検査は、前の段の照会と一致して通ってしまうことを対照で再現した)。
- **D3 lazy の戻り**(GPU、desktop): `runtime_memory_types_for_testing` で `local_source` の種類を lazy に偽ると、VMA に GPU_LAZILY_ALLOCATED(usage 6・requiredFlags 16・dedicated)を渡し、本物の VMA の
  `FeatureNotPresent` で AutoPreferDevice(実効の usage 8)に戻り、理由 `lazy_type_unavailable`。選ばれた type の property flags は 1(LAZILY なし)。全画素が正しい(「RC-7d D3 lazy allocation
  fallback preserves pixels」)。
- **D4 desktop の実確保**(GPU、NVIDIA GeForce RTX 5080): ordinary の tile-local `local_source`・ordinary の transient `scratch`・materialized の `local_output` の 3 枚が **1 つの allocation の offset
  0 / 1024 / 2048**(各 1,024 bytes、計 3,072)。3 組とも range が重ならない。複数 frame と resize の後に全画素一致、alias の取り直しの依存 12 件のうち **rendering の中(beginRendering 〜 endRendering)は 0 件**、
  synchronization validation を含む警告 / エラーは **0 件**(「RC-7d D4 ordinary transient and tile local share disjoint scope ranges」。offset の値は報告の JSON `<作業ディレクトリ>/rc7d_d4.json`)。
  example(flat・1920×1080)は transient / tile-local が無く、**4 allocation・128,286,720 bytes のまま**(RC-7c と同じ。21 行全部の契約と offset を RC-7c の参照と機械照合。C1 の test に上限の assert を足した)。
- **D5 verifier と Freeze**(CPU): lazy の住人は他の破損(`aliasable`・`stored`)より先に `memory.lazy_in_region@1`、`stored` の無い materialized の住人は `memory.stored@1`、ordinary の transient の住人は通る。
  wire は住人の representation から transient を決める(自己申告 `false` でも transient / tile-local は lazy で断り、LAZILY の無い types なら通り、materialized は LAZILY の types でも通る)。Freeze は transient /
  tile-local に `prefer`、materialized に `require`。
- **既存の test の変更**: 「RC-6a2 fix2 drops only the causal prefer when require-only baseline is infeasible」は、tile-local の `require` の組が desktop の memory types で通り、memory types が空なら
  `requirement_unsatisfied@1 / alias_group` で断られる形に移した。RC-3c2a C1(flat の `depth` が region に入り 10 → 9)と golden の renderer の trace(15 ケースの depth の `final_layouts`)は §2 の末尾の RC-7d。
- **対照**(実装者の報告、各 1 回で赤を確かめて戻す): 種類の判定を外す(lazy を ordinary に)→ D1 が赤、照会の lambda で TRANSIENT を落とす → D2 の usage の一致が赤、lazy の VMA usage を AutoPreferDevice に
  戻す → D3 の観測が赤、`allow_alias` の門を閉じる → D2 / D4 が赤、取り直しを scope の中に出す → D4 の rendering の中の依存が 9 件(resize の後 12 件)で赤、validation も赤。修正の報告の 3 件(planner の
  TRANSIENT・wire の representation・D2 の 1 対 1 の照合)も、修正を外すと新しい test が赤。
- **触った家族**(修正の後): render_planning_core 130・render_pipeline 518(既知の SKIP 1)・project_planning 130、hidden の A5 / F4 / B4 / C5(4,565 assertions)、選んだ GPU 19 件(-j1)が緑。
  studio_cpu 1,014 は修正の前の実装者の報告で緑。
- **統合側の gate**: GPU 全数 486 件中、赤は既知の `player_arguments_ds1_path` の 1 件だけ。非 GPU 全数は既知の 29 件だけが赤(修正の後の非 GPU 全数も既知の 29 件だけ)。シナリオ 23 本中 22 本が緑
  (`view_select_field` は単独で走らせると緑の間欠)。基準との比較 A5 / F4 / B4 / C5 が緑。
  未確認: **本物の lazily allocated memory の確保と commit**(手元にタイルベースの GPU が無い。D3 は desktop で種類を偽った戻りの道だけ)、`OutOfDeviceMemory` での戻り、VMA が GPU_LAZILY_ALLOCATED で
  dedicated にすることの読み返し、他の GPU / driver。

## 6. 未決(2026-10-17 更新)

議論で決まった・動いたもの:

- **計測の表示場所** → arena の `declared_bytes`([棚卸しメモ](design_plan_layer_inventory.md) 2-4)。今 0 固定の field を埋めれば
  Frame Plan の bytes 列がそのまま表示になる。`get_status` の VRAM 区分は後。
  → **RC-1 で済み(`631caac21`)**: render target の `declared_bytes` が埋まり、Frame Plan の status 行に合計と同時最大の 1 行、Resources の行に `Bytes` も出る。
  arena の値は登録時のもので、同じ id のまま image を作り直す経路(window 以外の resize、影の layer 数の変更)では更新されない(`get_frame_plan` の `bytes` と `memory_summary` は呼ぶたびに数える)。
  → **RC-1b(`504a6492a`)**: `get_frame_plan` の dump の RT の `declared_bytes` は書き出す時点の metadata から数え直す(arena の構造体の値は登録時のまま)。status 行の合計は世代が登録した RT 全部の和になった。
- **優先度** → 上がった。mac(統合メモリ)と Blender の教訓(OpenGL ドライバは GPU メモリを RAM に退避できるが Vulkan はできない。
  Blender 5.0 が Vulkan を既定にしなかった理由)で、同時最大の VRAM がそのまま体験に効く。
- **Apple GPU での効き方** → MoltenVK では transient 候補が memoryless texture に落ちるので、A の寿命短縮は alias だけでなく
  transient 判定にも効く。段 1 の価値が増す。
  → **RC-7d**: transient / tile-local の置き場所は device の memory type(LAZILY_ALLOCATED の有無)で決まり、機種別の分岐は書かない([§2 の末尾の RC-7d](#rc7d-memory-kinds))。MoltenVK での実測はしていない(未確認)。

残るもの:

- `required` を付けている論理型の既定が末尾までの生存を要求しているか(調べる。変わらず)。
  → **RC-6c で解けた**: `required` は寿命を延ばす理由にしない。末尾まで生きるのは graph の外の読み手がある資源だけ([§2 の末尾](#rc6c-lifetime))。
- **ヒープの単位** → MoltenVK は 1.3.0 から MTLHeap が既定なので heap 1 つで素直に動く見込み。Windows は memoryTypeBits ごとに分ける。
  device facts で切り替える形にする。
  → **RC-7c**: region は memoryTypeBits の完全一致ごと(device facts の答えで分かれる。機種別の分岐は書かない)。MoltenVK での実測はしていない(未確認)。
- **`alias_reuse` barrier の表現** → 別の表を推す(`FramePlanBarrier` は resource 1 つ。2 つ目の resource のために既存の形を歪めない)。
  → **RC-7c で決着**: 表は作らない。barrier は runtime の tracker が range の重なりで出す(§3.2 の注記と [§2 の末尾の RC-7c](#rc7c-packing))。
- **同じ WP に入るもの** → 物理計画の固定ルートを `target_planning` の policy に一本化する件([棚卸しメモ](design_plan_layer_inventory.md) 2-3、方針決定 2026-10-22)。`no_alias` は既にあり、`alias_group` / `materialize` などを足す語彙は区間詰めの入力と同じ場所。
