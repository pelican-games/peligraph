# projection jitter 枠と標準 TAA feature(v2)

対象読者: エンジン担当・テンポラル系エフェクトを書く人。
ステータス: v2 ドラフト(2026-07-16)。v1 は敵対レビュー
`docs/design_reviews/2026-07-16_taa_usd_review_codex.md` §1(以下
「レビュー」)で **Reject** — ①投影式が透視で符号逆・直交で要素誤り
(TAA-1)②主マテリアルは FrameUBO でなく push constant `engineMvp` を
使うため「シェーダ無変更」が不成立(TAA-2)③velocity の NDC 除算は
共通 frag にあり変更箇所の指示が誤り(TAA-3)④一パス RMW+二出力は
validator が明示禁止(TAA-4)⑤`frame_index` では reset を検知できない
(TAA-5)⑥J1 gate の自己矛盾(TAA-6)。v2 = 全 blocker の反映。
前提: `design_postprocess_temporal.md` §5-2(方向確定は維持)、
WP88/95、「feature 層 = ユーザー空間」方針。

## 0. 二層の切り分け(維持)

機構(§1)= エンジン、標準 TAA feature(§2)= 特権なし stdlib。
カメラ API・カリング・ゲームプレイには一切見せない。

## 1. 機構: `projection_jitter` 枠

### 1-1. 宣言(v1 から変更なし + schema 検証を明記)

```jsonc
"projection_jitter": { "pattern": "halton23", "phases": 8 }
```

- 同時 1 提供者(2 つ目は **feature 名入り**で reject)。未知 pattern・
  型・range(phases 1..64)・未知 key は compose 時に検証(J1-SCHEMA)
- compose result(現行は config/defines/feature_names のみ —
  `src/project/featurecompose.hpp`)に `{provider, pattern, phases}` を
  追加し、`pelican.frame_plan` v1 へも additive key として出す。
  `get_frame_plan` fixture を追加
- アップスケーラ予約キー(存在 = エラー): `phases_from_scale` /
  `mip_bias` / `render_scale`

### 1-2. 系列の契約(J1-SERIES)

- `offset_px = halton23(frame_index % phases) - 0.5`(px、[-0.5,0.5))。
  純関数・状態なし
- **schema contract として固定**: Halton の index 原点(**1 起点** —
  index 0 の (0,0) を避ける)、最初の 8 規範値の数表 fixture、
  phase wrap、width/height=0 は名前入りエラー、resize 後の最初の
  sample は `frame_index` から通常どおり導出(特別扱いなし)
- `jitter_ndc = (2*jx/width, sign_y * 2*jy/height)`。**`jy` の正方向 =
  framebuffer 下向き(Vulkan viewport 正 height・画像座標と同義)**と
  規範化し、CPU offset・`jitter_ndc`・shader sampling の三者同符号を
  fixture で固定(J1-MATH 末尾)

### 1-3. 投影式の規範(J1-MATH — v1 の式を全面訂正)

**規範は「透視除算後に定数 NDC delta」**:

```text
clip'.x = clip.x + dx * clip.w
clip'.y = clip.y + dy * clip.w
```

GLM `P[column][row]`(column-major)では全列 c について:

```text
P'[c][0] = P[c][0] + dx * P[c][3]
P'[c][1] = P[c][1] + dy * P[c][3]
```

- 現行 `perspectiveRH_ZO`(`P[2][3] = -1`)への簡約は
  **`P'[2][0] -= dx`、`P'[2][1] -= dy`**(v1 の `+=` は逆方向 — 訂正)
- 現行 `orthoRH_ZO`(`clip.w = 1`)は translation 列
  **`P'[3][0] += dx`、`P'[3][1] += dy`**(v1 の `P[2][*]` 変更は
  z 依存の shearing になる — 訂正)
- 実装は簡約形でなく**一般式(全列)**を適用してよい(両投影で同一
  コード)。**CPU 単体テスト**: perspective/ortho それぞれで複数の
  view-space z の点が透視除算後に同一の NDC delta を持つこと

### 1-4. 行列の配送(J1-DELIVERY — v1 の「FrameUBO だけ」を撤回)

現実: 主マテリアルは `camera.getVPMatrix()` の push constant
`engineMvp` を使い(`materialrender.cpp`)、FrameUBO は
material/shadow/velocity で共有・フレーム冒頭に一度だけ更新される。
よって:

- **render-frame snapshot**(フレーム冒頭に一度確定)に 4 組を保持:
  `{proj, view_proj} × {jittered, non_jittered} × {current, previous}`
- **consumer 表(規範 — anchor 位置を意味論の判定に使わない)**:

| consumer | 行列 | 供給経路 |
|----------|------|---------|
| 主 material(`engineMvp`)| **jittered VP** | push constant(materialrender の 1 行変更)|
| velocity パス | **jittered**(current/previous とも — §1-5 で差引)| FrameUBO |
| sprite パス(S2D)| **jittered VP** | 専用 UBO/push(実装調査の上 — scene ラスタなので jittered)|
| world debug_draw | **jittered VP** | 同上(絵と輪郭を一致させる)|
| shadow | light VP(**無関係** — jitter 不適用)| 現行どおり push constant |
| fullscreen 系(tonemap 等)| 投影不使用 | 変更なし |
| UI | 独自 px→clip | 変更なし |
| **カリング** | **non-jittered** | CPU(現行どおり)|
| rpc / get_status / ray 系 | **non-jittered** | 変更なし |
- FrameUBO の `projection` / `previous_projection` は **jittered** に
  統一(velocity が使う面)。non-jittered が要る GPU consumer は
  現状存在しない — 必要になったら FrameUBO へ additive 追加
- integration fixture: jitter on で「カリング結果(ドローコール数)
  不変」+「shadow 出力 byte 不変」

### 1-5. FrameUBO 追加と velocity 差引(J1-ABI/VELOCITY — 変更箇所を訂正)

FrameUBO に additive 追加(**CPU/GLSL の std140 offset/size を同時
更新し、`offsetof`/`sizeof` の ABI static_assert gate を更新**):

```glsl
vec2 jitter_ndc;            // 今フレーム(無効時 0)
vec2 previous_jitter_ndc;   // 前フレーム(無効時 0)
uint temporal_reset_epoch;  // §1-6
uint _pad;
```

- **velocity の変更箇所 = 共通 `velocity.frag`**(v1 の「.vert 二行」は
  誤り — NDC 除算は frag にある): current/previous の NDC から
  `jitter_ndc` / `previous_jitter_ndc` をそれぞれ引いた後、既存の
  `*0.5` で UV delta 化する。これを**単位の規範**とする
  (velocity 出力 = UV delta・jitter 減算は NDC 空間)
- 数値 fixture: static / skinned / camera motion / object motion /
  jitter-only(運動なし・ジッタのみ → velocity 0)/ jitter-off
- 「byte 不変」の定義 = **capture(絵)の byte 一致**。shader binary の
  同一性は要求しない(TAA-3 の指摘どおり)

### 1-6. 履歴 reset の明示化(J1-RESET — frame_index 案を撤回)

`EngineTime::setTime` は frame_index を変えず、resize も同様 —
frame_index では reset を検知できない。よって:

- FrameUBO に **`temporal_reset_epoch`**(uint・単調増加)を追加。
  増加イベント = 初回・resize・`set_time`・カメラ不連続(将来の
  camera cut 通知)・**temporal 系 feature の有効化フレーム**
- 増加したフレームでは: previous projection/jitter を current と同値に
  セット(zero velocity / zero reprojection)。TAA 等の consumer は
  「前フレームの epoch と違う → 履歴無効、現フレームをそのまま出す」
- 既存の history RT クリア・camera/object history invalidation と
  **同一の invalidation 経路**に統合(handler 独自判定の禁止 —
  HR gate と同じ流儀)

### 1-7. J1 の受け入れゲート(J1-GATE — 三分割に訂正)

1. **provider なし / 既定 off**: 既存 golden 全部が byte 一致
2. **provider on / TAA なし**: 画像不変は要求しない(ジッタは絵を
   変える)。新設の **jitter-only 決定的 fixture**(規範 Halton sample
   ごとの capture が数表と一致・2 回実行 byte 一致)
3. **provider on / TAA on**: T-TAA 側の N フレーム golden

## 2. 標準 TAA feature(TAA-GRAPH — 二パス構成に訂正)

現行 validator は同一 target の非 history read/write を禁止し、
fullscreen の color output はちょうど 1 個。**v1 の一パス RMW 案は
撤回**し、validator 無変更の二パス構成とする:

```
pass taa_resolve:
  reads:  <scene_color>, <velocity>, <depth>, taa_accum@history
  writes: taa_accum                      // 1 出力
pass taa_composite:
  reads:  taa_accum                      // 現フレーム面
  writes: <downstream_color>             // 1 出力
```

- **リソース名の解決(TAA-RESOURCE)**: stdlib feature に
  プロジェクト固有 RT 名を直書きしない。feature の **parameters**
  (`<scene_color>` 等の bind 先を宣言側が指定 — render_feature の
  既存 override/挿入機構の上に **named binding パラメータ**を追加。
  この 1 点だけは J1 の機構追加に含める)+ 既定値は canonical anchor
  の慣習名
- depth は feature override で `SAMPLED` usage を追加(既存機構 —
  `featurecompose.cpp` の override で可能なことを確認済み)。
  depth の線形化式(ZO・perspective/ortho 両対応)を feature シェーダ
  内に規範として持つ
- **resolve の意味論(TAA-RESOLVE — 全て数式で固定)**: velocity 単位 =
  UV delta・UV 原点 = 左上・jitter 単位 = NDC・再投影式・画面外 reject・
  **depth 不連続(disocclusion)閾値** = 線形深度の相対差 τ(既定
  0.1・params 公開)で履歴棄却・履歴無効(§1-6 epoch)は現フレーム
  素通し・alpha は scene alpha を素通し・3×3 clamp の端 sampling =
  clamp-to-edge・blend `out = mix(history, current, α)`(α 既定 0.1 —
  「新フレーム寄与率」と明記し向きの混乱を封じる)
- example config への compose/validate が通る完全な `taa.json` fixture
  を gate に含める

### 2-1. golden(TAA-GOLDEN)

既定 off の既存全 golden byte 一致 / jitter-only / 静止 N フレーム収束 /
camera motion / object motion / **disocclusion**(手前の物体が動いて
背景が現れる)/ resize 後 1 フレーム / set_time 後 1 フレーム /
orthographic / 2 回実行 byte 一致 — をそれぞれ**別 gate** とする。

## 3. WP 分割

| WP | 内容 | gate | 依存 |
|----|------|------|------|
| **J1(機構)** | §1 全部(J1-MATH/SERIES/SCHEMA/DELIVERY/ABI・VELOCITY/RESET)+ named binding パラメータ | §1-7 の三分割 gate + CPU 単体テスト + consumer 表 fixture | WP88/95(済) |
| **T-TAA(stdlib)** | §2 の二パス feature + adding_features.md レシピ | §2-1 の全 golden + compose/validate fixture | J1 |

## 4. 未決事項

1. blend α と disocclusion τ の params 公開粒度 — v1 は α と τ のみ
2. `render_scale` 系(アップスケーラ)— 予約のまま
3. 半透明の responsive マスク — 需要時に stdlib 改造例として
4. sprite/debug_draw への jittered VP 配送の実装形(専用 UBO か push か)
   — J1 実装時に現行コードを見て確定(consumer 表の意味論は不変)
