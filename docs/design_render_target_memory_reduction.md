# 不要な render target の削減: 寿命の正確化と区間詰めによるメモリ共有

対象読者: エンジン担当(描画コンパイラ / renderer)。
ステータス: 改善案メモ(2026-10-16。議論の記録。WP 化・レビュー前)。段 0(計測)は RC-1 で済み(2026-10-23、`631caac21`。§5)。合計の数え方を実際の確保に合わせる RC-1b も済み(`504a6492a`。§5)。段 1 の寿命の部分(A)は RC-6c で済み(2026-10-30、`be795c9a5`。[§2 の末尾](#rc6c-lifetime)と §5)。中間の RT を名前で読む約束(`retain`)は RC-7a で済み(2026-10-31、`2d1715cf3`。[§2 の末尾の RC-7a](#rc7a-readback)と §5)。
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

## 3. B: alias を区間詰めに一般化する(本丸)

### 3.1 現状

- 計画: [`deriveLegalAliasCandidates()`](../src/project/targetrenderplanning.cpp#L3124) は **同 representation・format・samples・view layout・mip・layers・dimension・extent** かつ寿命非重複の組だけを候補にする。
- runtime: [`allocAliasingImage()`](../src/core/vkcore/core.cpp) は owner の `VkImageCreateInfo` をそのまま複製して owner の allocation に bind
  (offset 0、同サイズ)。[`rendertargetcontainer.cpp:710`](../src/core/renderingpass/rendertargetcontainer.cpp#L710) も format / usage / samples / mip の一致を要求。
- 結果、「大きい gbuffer の跡地に小さい bloom を置く」ができない。

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
| 2 | B: 区間詰め alias(planner → runtime → barrier)。**RC-7b〜7e に分けた(下の「RC-7 の段」)** | 大 | planner 単体(詰め結果の決定性、no_alias、memoryTypeBits 拒否)、GPU gate 全数、`hazard_stress` での揺さぶり、validation layer |
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
2. **RC-7b** 同じ形の共有の条件を広げる(offset 0・既存の barrier のまま。広げた後の画像の契約の互換と frame の境界での使い回しの同期を完了の条件に入れる)。
3. **RC-7c** 形の違う区間詰め(retain の資源への TRANSFER_SRC の自動の付け足し、device facts の memory requirements、memory の種類ごとの heap と offset、`alias_reuse` の barrier)。
4. **RC-7d** memory の種類ごと(transient / tile-local を同じ allocator の道へ。lazily allocated は作り物の device facts の CPU test まで)。
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

残るもの:

- `required` を付けている論理型の既定が末尾までの生存を要求しているか(調べる。変わらず)。
  → **RC-6c で解けた**: `required` は寿命を延ばす理由にしない。末尾まで生きるのは graph の外の読み手がある資源だけ([§2 の末尾](#rc6c-lifetime))。
- **ヒープの単位** → MoltenVK は 1.3.0 から MTLHeap が既定なので heap 1 つで素直に動く見込み。Windows は memoryTypeBits ごとに分ける。
  device facts で切り替える形にする。
- **`alias_reuse` barrier の表現** → 別の表を推す(`FramePlanBarrier` は resource 1 つ。2 つ目の resource のために既存の形を歪めない)。
- **同じ WP に入るもの** → 物理計画の固定ルートを `target_planning` の policy に一本化する件([棚卸しメモ](design_plan_layer_inventory.md) 2-3、方針決定 2026-10-22)。`no_alias` は既にあり、`alias_group` / `materialize` などを足す語彙は区間詰めの入力と同じ場所。
