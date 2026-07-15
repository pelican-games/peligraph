# 2D ゲーム層と 2D⇔3D 相互変換(v2.2)

対象読者: エンジン担当・2D ゲームを作る人・2D/3D 混在演出を作る人。
ステータス: **v2.2 — C4/S2D-1 確定(2026-07-15)**。v1 は敵対レビュー
`docs/design_reviews/2026-07-12_hr_v2_2d_v1_review_codex.md` §4-8(以下
「レビュー」)で **Reject** — 中核方向(単一ワールド)は妥当だが、
①「機構を増やさない」への過剰一般化 ②SpriteView 例が scene v1 の
`name` dispatch と不一致 ③U1 の framebuffer-px quad ABI は world quad に
流用不能 ④camera snap だけでは pixel perfect にならない ⑤現 physquery
だけでは platformer が書けない ⑥S2D-0 の粒度過大、が理由。
v2 = 再受理条件 2D-R1〜R6 の全反映 → 再レビュー
`docs/design_reviews/2026-07-12_2d_v2_rereview_codex.md`(以下
「再レビュー」)で**条件付き受理** — C1〜C3 = S2D-0a、C4 = S2D-1、
C5 = S2D-P の exit gate に添付。v2.1 = その 5 条件の正本反映。
前提: `design_ui_2d_foundation.md` v8、`design_camera_system.md`
(orthographic = WP48 済)、`design_physics_queries.md`(P1/P2 済)、
scene v1、「feature 層 = ユーザー空間」方針、サブセット原則。

## 0. スコープ定義 — 「2D⇔3D 相互変換」とは何か

- **2D → 3D(2D シーンの 3D 空間配置)**: スプライトのコンテンツを 3D
  空間の任意の位置・向きに置ける(ペーパーマリオ型・3D 内の看板/画面)
- **3D → 2D(3D シーンの 2D 投影編集)**: 3D コンテンツを直交投影で
  2D ゲームとして見せる/編集する

### 0-1. 中核決定(2D-R1 で限定を修正)

**単一 ECS ワールド・単一 EntityId・Transform 階層の共用。第二の scene
ownership は作らない。** 2D→3D = transform、3D→2D = projection に還元する。

v1 の「エンジン機構も増やさない」は**過剰一般化として撤回**する。
禁止するのは**所有権と identity の二重化**(別ワールド・別 ID・同期
ブリッジ)であって、同じワールドの**派生 index / consumer** は許可する:
2D render extraction・visibility culling・tile chunk・pixel-perfect
compositor・query helper はここに属す。Godot 分離型の実利は別 ID では
なく 2D 専用最適化にあり、その実利はこの形で取り込む(レビュー §4.2)。

### 0-2. UI 層との共有境界(2D-R3 で縮小)

v1 の「QuadCommand 相当の POD と quad 展開コードを共有」は撤回。現 U1 の
`QuadVertex` は framebuffer px 直書き(`src/core/ui/drawcommands.hpp`)、
ui.vert は px→clip 変換で depth 固定 0 — world transform・camera VP・
回転・pivot・per-vertex depth を表せない。

**共有するもの(資産・規約・一部 helper に限定)**:

- `AtlasDocument / AtlasAsset`: atlas parser・GPU page upload・page
  lifetime を **UiModule/UIContainer から consumer-neutral な resource へ
  抽出**する。UI とスプライトの一方だけ有効でも他方を初期化しない
  (U1 の purge gate 維持)。両方有効時は同じ image/page allocation を参照
- texture page identity・sampler key(nearest/linear)・
  straight alpha/blend 値・色 ABI(linear 出力)
- quad index topology / chunk 分割 helper(純 CPU)

**共有しないもの**: 頂点/コマンド ABI・pipeline・パス・depth・ソート・
座標変換。UI = `UiQuadCommand/QuadVertexPx`(現行)、スプライト =
**`SpriteCommand`**(world transform・pivot・UV・color・page・layer・
sort key・billboard)+ world-space 頂点/インスタンス ABI を別に持つ。

U1 の 16384 quad 上限は**一 document の上限**であり、スプライトへ転用
しない。スプライトは visibility cull 後に複数 uint16 chunk(または
uint32/instancing)へ分割する(§5)。

## 1. スプライト(機構)

### 1-1. sprite_view コンポーネント(2D-R2 — scene v1 準拠に修正)

scene v1 の component dispatch は `name` キー
(`src/core/loader/scene.cpp` の `component.at("name")`)。また component
params の直接ファイル参照は scene 正本で禁止 — texture は **asset
declaration ID + フラグメント**で指す:

```jsonc
{ "name": "sprite_view",
  "texture": "ui_atlas#sprite/hero_idle_0",  // asset宣言ID + #sprite/
  "size": [1.0, 1.5],          // world 単位。省略時 = source px / ppu
  "pivot": [0.5, 0.0],         // 0..1、既定 [0.5, 0.5]
  "color": [1,1,1,1],          // 乗算 tint(linear)
  "flip": [false, false],
  "layer": 0,                  // int16
  "billboard": "none"          // "none" | "y_axis" | "full"
}
```

- 単独画像(png/KTX2)も asset 宣言 ID で指す(内部で 1 sprite の暗黙
  atlas resource 扱い)
- public struct・`ref`/validation・未知 field・既定値・範囲
  (size/pivot/layer)・finite transform/color・atlas fragment の
  rename/missing を S2D-0a で閉じる(validation fixture 付き)
- **sampler の正本 = asset 宣言側**(再レビュー C1 の確定):
  atlas / 単独画像の宣言メタが `nearest | linear` を持つ(既定 =
  linear)。**sprite_view に per-sprite override は置かない(v1)**。
  `SpriteCommand`(が参照する immutable page/material key)に sampler を
  保持し **run/batch key の一部**とする。schema fixture: 不正値・
  未知 field・既定値。strict pixel-perfect(§2)は nearest を要求し、
  非 nearest アセットを strict カメラで描く場合は**名前入り WARN +
  当該スプライトのみ非 strict 降格**(status で観測可能 — silent
  degradation 禁止)
- スプライト = object transform で置かれる world-space quad(XY 平面・
  +Z 法線)。回転・スケール・親子は Transform がそのまま効く
- flipbook はエンジン機構にしない(frame 差し替えはユーザー空間
  システム。同梱 flipbook = 特権なし標準ライブラリ)

### 1-2. 描画パス

専用 sprite パス 1 本(3D 不透明の後・post_main の前の固定位置)。
depth test ON / depth write OFF(§4)。マテリアル梯子と独立の固定
シェーダから始める(B 層フックは需要が出たら別途設計)。

## 2. ピクセル契約(2D-R4 — camera snap 単独案を撤回)

v1 の「camera を 1/ppu 格子へ丸める」は不成立: 現 orthographic は
`[-xmag,+xmag]×[-ymag,+ymag]` を framebuffer 全体へ写す
(`src/core/renderer/camera.cpp`)ため、1 framebuffer pixel の world 幅は
`2*xmag/width` であって一般に `1/ppu` ではない。契約を三概念に分離する:

1. **`ppu`**(プロジェクト設定): アセット寸法の変換係数のみ
   (size 省略時 = source px / ppu)
2. **`world_units_per_framebuffer_pixel`**: projection(xmag/ymag)と
   viewport から導出される値。ppu とは独立
3. **`zoom` = framebuffer_pixels_per_source_pixel**: strict モードの
   許可値を定義(整数、または明示 opt-in の非整数)

**strict pixel-perfect mode**(カメラ単位の opt-in)は次のどちらかで実装
(S2D-1 で選定・どちらでも物理 Transform は変更しない):

- (a) **logical-resolution render target 方式**: 1 source texel =
  1 target pixel で描き、整数倍 nearest upscale で合成
- (b) **render-only quantization 方式**: world→view 変換後に頂点/pivot を
  target pixel 格子へ量子化(描画のみ・シミュレーションに不可視)

camera snap は `1/ppu` でなく **選んだ projection の
world-units-per-target-pixel と pixel-center/half-texel 規則**で行う。
x/y の pixel density 不一致・非整数 upscale・letterbox は
error / letterbox / 非 strict 降格 のいずれかに固定する。fractional に
動くスプライトの量子化と smooth camera 補間の両立は renderer policy として
明文化する(個別 sprite snap は持たない)。

**strict の成立式と対象集合(再レビュー C4 — S2D-1 の exit gate)**:

1. 各軸で `zoom_axis = (1 / ppu) / world_units_per_framebuffer_pixel_axis`
   (logical target 方式では target→framebuffer の整数倍率を含む同値式)を
   規範化する。x 軸の world 幅 = `2*xmag`、y = `2*ymag` なので、
   **どの extent を content viewport として式へ入れるか**(letterbox の
   content rect・odd viewport の丸め・pixel center 原点・整数 zoom の
   判定許容差)を S2D-1 で確定する
2. **strict 対象集合を表で固定**: size 省略・単位 scale の sprite /
   明示 size / 整数 scale / fractional・non-uniform scale / rotation /
   billboard のどこまでを対象とするか。対象外は silent degradation で
   なく status/error で観測可能に
3. camera snap と render-only quantization の**適用順を一つに固定**し
   二重丸めを禁止。物理 Transform 不変の契約は維持

golden: odd/even テクスチャ・odd/even viewport・zoom 1/2/3・fractional
camera・fractional sprite・回転/非一様スケール(strict 対象外なら明示)・
atlas edge。**+ 成立式を一項だけ破る xmag/ymag・viewport・ppu の
negative fixture(C4-4)**。

### 2-1. S2D-1 で確定した strict 契約(WP106)

方式は **render-only quantization** に固定する。logical-resolution target、
letterbox、camera snap は v1 に持たない。content viewport は framebuffer 全域、
pixel center は `(n+0.5, m+0.5)`、境界は整数である。各軸の規範式は:

```text
wupp_x = 2*xmag / framebuffer_width
wupp_y = 2*ymag / framebuffer_height
zoom_x = (1/ppu) / wupp_x
zoom_y = (1/ppu) / wupp_y
```

`zoom_x` と `zoom_y` が相対許容差 `1e-4` 以内で同じ整数かつ 1 以上のとき
だけ camera contract を active にする。不成立時は letterbox 等へ暗黙 fallback
せず、理由を `get_status.sprite.pixel_perfect.failure` と名前入り WARN に出す。

| sprite 条件 | strict | status reason |
|---|---|---|
| size 省略 + nearest + 単位 scale + view 軸平行 | 対象 | `eligible` |
| 明示 size | 最終 framebuffer px/source texel が各軸整数なら対象 | `eligible` または `non_integer_texel_scale` |
| 整数 scale | 対象 | `eligible` |
| 非一様 scale | x/y を独立判定し、両方整数なら対象 | `eligible` または `non_integer_texel_scale` |
| fractional size/scale | 結果が整数 texel scale の場合だけ対象 | `non_integer_texel_scale` |
| view 軸に対する回転/傾き(90°を含む) | 対象外 | `rotated_or_tilted` |
| `y_axis` / `full` billboard | 対象外 | `billboard` |
| linear sampler | 対象外・asset 名入り WARN | `linear_sampler` |
| perspective / zoom 不成立 camera | 全 sprite 対象外・camera 名入り WARN | `camera_contract_invalid` |

適用順は **物理 Transform 不変 → 通常の world/view/projection → projected local
atlas corner を framebuffer 整数境界へ1回だけ丸める → その NDC delta を quad
全頂点へ同一に加える**。camera と sprite の二重丸めは禁止する。

## 3. ソート(2D-R5 — 全順序を閉じる)

1. **layer(int16)**: 異なれば必ず layer 順
2. **layer 内ポリシー**(所有はカメラ側・宣言はカメラ定義):
   - `"z"`(既定): **view-space の pivot depth** 遠→近
   - `"y_down"`: **world-space の pivot Y** が大きい方が奥
   - `"declaration"`: **monotonic な `declaration_seq:uint64`**
     (sprite 登録時に発行。ECS index は free-list 再利用されるため
     生成順の代用にならない — scene 宣言順・replay・recreate 時の
     再発行規則を S2D-0a で固定)
3. **タイブレーク**: 完全 `(EntityId.index, EntityId.generation)` +
   **source-local stable ordinal**(再レビュー C2): §5 の render source は
   1 entity から複数 command を出すため、EntityId の後ろに source 内で
   安定な primitive/command ordinal を置いて全順序を閉じる。発行規則・
   cache 再利用・削除再追加・replay 時の不変性を S2D-0a で定義し、
   「source の内部格納順を変えても規範 ordinal が同じなら同一 bytes」を
   fixture 化する

比較の規範: transform/sort 入力は **finite 必須**(NaN/Inf は
コンポーネント validation で拒否)。float の z/y は canonical な
total key(必要なら量子化)へ変換してから比較し、`-0/+0`・epsilon 同値で
comparator の strict weak order が壊れないことを fixture 化する。
複数カメラはカメラごとに command list を生成する。

fixture: EntityId 再利用・同 z/y・`-0/+0`・非 finite 拒否・複数 layer・
カメラ移動・billboard・opaque 3D の前後 — 2 回実行で command bytes と
最終絵の一致。

## 4. 3D との合成

- sprite パス = depth test ON / depth write OFF。3D 不透明には正しく
  遮蔽され、スプライト同士は §3 の painter 順
- **明記する限界(仕様)**: スプライトは後続 draw の occluder に
  ならない。layer は幾何 depth を上書きする。opaque cutout・3D 半透明
  との相互ソート・交差する大きな回転 quad は v1 の対象外。
  sprite depth prepass / alpha-cutout は将来拡張点として予約
- 完全 2D プロジェクトでも同じパスが走る(2D 専用モードなし)

## 5. スケールの逃げ道(レビュー §6 — 単一ワールドの破綻条件を塞ぐ)

単一ワールドが破綻するのは「1 sprite = 必ず 1 entity + 毎フレーム全件
CPU sort + 単一 16384 quad buffer」に固定した場合。escape hatch を機構に
含める:

1. 通常の sprite_view は ECS entity でよいが、**TileMap/particle/
   foliage は 1 entity が chunk/instance 列を供給できる render source**
   とする(tile 1 枚ごとの EntityId を要求しない)
2. camera frustum/canvas bounds で **visibility cull → sort → 16384
   以下の draw chunk 分割**。static chunk は transform/atlas/sort
   revision が変わるまで command cache を再利用
3. fixture: **50,000 論理 sprite(可視 ~2,000)の純 CPU fixture** を
   S2D-0a に置く — 出力順/hash・chunk 上限・非可視除外・同一入力
   再実行一致。性能は WP29 の計測形式で記録。
   **加えて chunk 境界を実際に跨ぐケース(再レビュー C3)**: 可視数
   16384・16385・2 chunk 超の各ケースで、chunk ごとの vertex/index 型
   上限・連結 command 順/hash・境界前後の欠落/重複なしを検査
   (U1 の maxQuads=16384 を frame 上限として誤流用する回帰の検出)
4. tilemap 形式は将来だが、S2D の ABI が「1 command = 1 entity」
   「1 フレーム最大 16384」に凍結されないことを S2D-0 で保証する

## 6. 2D⇔3D 相互変換の各論

- **2D→3D**: スプライトは最初から world オブジェクト。配置 = 親
  Transform のみ。機構で足すのは billboard("y_axis"/"full" — view
  依存のため描画側)だけ
- **3D→2D**: 直交カメラ(WP48 — ただし実装済みは projection のみで
  pixel snap は含まない)+ ゲームロジックの移動拘束(ユーザー空間)。
  devstudio の 2D 投影編集は D2 の仕事で本書からの追加要求なし
- **3D の 2D 素材化**: M3.5 named snapshot を texture 源に許す
  (`snapshot:` スキーム予約)— v1 対象外。**注意(再レビュー §3.2)**:
  現 PathResolver は `://` 構文のみ scheme と認識し未知 scheme を reject
  する — この予約は「現 resolver が解決できる」という意味ではない。
  将来 WP で asset/path 参照とは別の **typed runtime resource ref** と
  して正式設計する。S2D-0a〜2 では parser にも `sprite_view.texture` にも
  受理させない(予約語のまま)

## 7. 2D 物理(2D-R6 — 「既存 query で足りる」を撤回)

現 physquery は ray×3 形状 + boolean overlap + closest ray のみ。これで
書けるのは足元 ray の hand-authored ゲームまでで、platformer 一般の成立
根拠にはならない(tunneling・斜面・one-way が書けない)。ただし解は
物理シミュレーション導入ではなく **query の拡張**(S2D-P):

1. **`shapeCast/sweep(start, delta, filter)`**(capsule/box): TOI・
   position・normal・安定 collider/entity ID を返す決定的 sweep
   (薄い床/壁の tunneling 防止)
2. **layer/mask・self/ignore set・trigger・one-way metadata を query
   filter で**。one-way は前位置/接近方向のユーザー空間 policy でも
   よいが、「無視した後の次 hit」を取れる **all-hit / filter 継続**が必要
3. **initial overlap の signed distance / MTD**(または決定的
   depenetration query)
4. `moveAndSlide`・slope limit・step-up・coyote time・moving platform は
   **標準ユーザー空間ライブラリ**。エンジンは sweep/contact data と
   安定 identity だけ保証
5. fixture(純 CPU・fixed-step): 落下接地・斜面上り/下り・壁 slide・
   corner・薄床高速移動・one-way の下から通過/上から接地・
   initial overlap・同時 hit tie
6. **public contract の固定(再レビュー C5 — S2D-P の exit gate。
   header と fixture を同じ WP に)**:
   - 入力 = moving `Shape`(寸法・初期 pose)+ `delta` + filter。
     zero delta / non-finite / initial overlap の扱いを定義
   - hit = TOI・position・normal・**安定 collider identity + ECS entity
     がある場合は full identity**(名前だけ・再利用 index だけに退化
     させない)
   - all-hit の規範順 = `(canonical TOI, stable collider identity,
     shape/subshape ordinal)` の total key。同 TOI・MTD 非一意軸の
     tie-break を定義。closest と filter 継続は**この同一 ordered
     result から導出**
   - layer/mask・self/ignore・trigger・one-way metadata の格納先と
     既定値を collider/public query schema に追加。未知 bit・stale
     identity・ignore 後の次 hit を positive/negative fixture 化

## 8. 決定性と検証

- §3 の全順序 fixture・§5 の 50k sprite fixture・§2 の pixel golden・
  §7 の platformer シナリオ(replay 2 回 byte 一致)
- ppu・zoom・ソートポリシー・chunk 数は get_frame_plan / get_status で
  観測可能に

## 9. 実装順(レビュー §7 の再分割を採用)

| WP | 内容 | exit gate | 依存 |
|----|------|-----------|------|
| **S2D-0a Contract/CPU** | sprite_view schema(scene v1 準拠)+ public component、consumer-neutral AtlasAsset 抽出、SpriteCommand/world ABI、sort total key + declaration_seq、visibility/caching/chunking | schema fixture・ID 再利用/float sort fixture・50k/2k chunk fixture(GPU 不要)**+ 再レビュー C1(sampler key)・C2(source ordinal)・C3(chunk 境界 16384/16385/2+)** | K3・U1(コード流用でなく抽出元) |
| **S2D-0b GPU world quad** | world-space 頂点/インスタンス buffer・camera VP・pivot/flip・シーンパス・straight alpha・depth test ON/write OFF・atlas page bind | 2D/3D 遮蔽・layer/z・複数 atlas・回転/親子・UI 無し/有りの resource ownership・golden | S2D-0a・WP48 |
| **S2D-1 Pixel/Policy (WP106 済)** | strict pixel-perfect(render-only quantization)・ppu/zoom・y_down/declaration・billboard・flipbook dogfood(ユーザー空間) | §2/§3 fixture + pixel golden 全通過 **+ 再レビュー C4(成立式・対象集合表・適用順・negative fixture)** | S2D-0b |
| **S2D-P Query minimum** | sweep/shapeCast・filter/all-hit・MTD・安定 collider identity | §7 の純 CPU fixture **+ 再レビュー C5(public contract と同順位規則)** | P1/P2 |
| **S2D-2 Vertical slice** | **side-scroller 1 面を固定選択**(top-down に逃げて platformer 条件を未検証にしない): 入力/event/fixed-step/接地/斜面/one-way の実証 | replay 2 回一致・接地/斜面/one-way/tunneling シナリオ | S2D-1・S2D-P |
| 将来 | TileMap 形式/chunk・9-slice・snapshot texture・3D 半透明統合 | 各別設計 | — |

## 10. 未決事項

1. ~~スプライト平面の既定~~ → **確定(2026-07-12 ユーザー決定): XY 平面 +
   Z 法線に固定**。plane 切替コンポーネント指定は持たない。床置き等は
   Transform の回転で表現(見下ろしはカメラと y_down ソートで解く)
2. ~~strict pixel-perfect の方式選定~~ → **確定(WP106): render-only
   quantization**。full framebuffer・整数 zoom・対象集合は §2-1 が正
3. タイルマップ形式(チャンク・衝突・オートタイル)— 別文書
4. スプライトへのライティング — 需要が出たら surface 化(B 層)を検討
