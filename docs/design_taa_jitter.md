# projection jitter 枠と標準 TAA feature(v1)

対象読者: エンジン担当・テンポラル系エフェクトを書く人。
ステータス: v1 ドラフト(2026-07-16。敵対レビュー前)。
前提: `design_postprocess_temporal.md` §5-2(2026-07-12 の方向確定 —
「ジッタはカメラ API から完全分離したレンダー側の projection modifier 枠」)、
WP88(history/velocity)、WP95(skinned velocity prev palette)、
「feature 層 = ユーザー空間」方針、色パイプライン v4(canonical anchor)。

## 0. 二層の切り分け

| 層 | 内容 | 本書 |
|----|------|------|
| **機構(エンジン)** | `projection_jitter` 枠 — 宣言・系列・適用点・FrameUBO 公開・排他 | §1 |
| **標準 TAA feature(ユーザー空間)** | `engine://features/taa.json` + シェーダ = 特権なし stdlib | §2 |

消費者はテンポラル一族のみ(TAA / DLSS・FSR2・XeSS 系 / チェッカー
ボード / 静止時アキュムレーション)。**カリング・レイキャスト・UI 投影・
ゲームプレイ・カメラ API には一切見せない**(§5-2 の確定どおり)。

## 1. 機構: `projection_jitter` 枠

### 1-1. 宣言(render_feature JSON の追加キー)

```jsonc
{
  "schema": "pelican.render_feature",
  // ...
  "projection_jitter": {
    "pattern": "halton23",     // 名前付き系列(v1: halton23 のみ)
    "phases": 8                // 1..64。系列の周期
  }
}
```

- **同時に 1 提供者のみ**。2 つの feature が宣言したら起動時に名前入り
  エラー(ターミナルフック排他・active camera 排他と同じ流儀)
- `pattern` は将来 additive に増える(エンジン側の名前付き系列)。
  未知 pattern は名前入りエラー
- **アップスケーラ予約(v1 では存在 = エラーの予約キー)**:
  `"phases_from_scale"`(解像度比から周期導出)・`"mip_bias"`・
  `"render_scale"`。DLSS/FSR2 系の要求はこの枠の属性拡張で受ける

### 1-2. 系列の意味論(決定性)

- ジッタ値は **`frame_index` の純関数**: `offset_px =
  pattern(frame_index % phases) - 0.5`(px 単位、[-0.5, 0.5) の
  サブピクセル)。**状態を持たない** — replay/golden は既存の
  `step_frame ×N` 流儀でそのまま一致する
- halton23 の規範値(最初の 8 フレーム分)を数表として fixture に固定
  (実装の入れ替えで golden が黙って動かないように)
- `set_time` / resize は history リセット(既存 T1 規約)のみ —
  ジッタ自体は frame_index から再計算されるだけで特別処理なし

### 1-3. 適用点(scene ラスタ投影のみ)

- 適用 = clip 空間の平行移動: `proj'[2][0] += 2*jx/width`、
  `proj'[2][1] += 2*jy/height`(列優先・Vulkan 規約に合わせて実装で確定)。
  透視除算後の NDC オフセットは定数 `(2*jx/width, 2*jy/height)` になる
  (この線形性が §1-4 の減算の根拠)
- **ジッタ済み行列を使うのは scene ラスタパス(anchor が
  output_transform より前の描画パス)の FrameUBO `projection` のみ**。
  以下は非ジッタのまま:
  - カリング(frustum)
  - `set_camera` / get_status / ray 変換系の rpc
  - UI(そもそも独自 px→clip)・debug_draw の world 系は
    scene パスに乗るためジッタ済みで正(輪郭が絵と一致する)
- カメラ定義・コントローラ・カメラ component からは不可視
  (読み書きの API を追加しない)

### 1-4. FrameUBO 公開(velocity と resolve のため)

FrameUBO に additive 追加:

```glsl
vec2 jitter_ndc;           // 今フレームの NDC オフセット(無効時 0)
vec2 previous_jitter_ndc;  // 前フレーム分(無効時 0)
```

- `projection` / `previous_projection` は**ジッタ済み**(scene パスの
  実効行列 — 現行シェーダは変更なしで動く)
- **velocity のジッタ差引**: velocity 系シェーダは NDC 変換後に
  `cur_ndc.xy -= jitter_ndc; prev_ndc.xy -= previous_jitter_ndc;` を
  行い、**velocity は純運動のみ**を持つ(ジッタが velocity に漏れると
  TAA/モーションブラーが 1px 未満で常時ブレる)。既存
  `velocity.vert` / `velocity_skinned.vert` の 2 行変更(ジッタ無効時は
  0 減算で挙動不変 = golden 維持)
- TAA resolve は `jitter_ndc` で現フレームのサンプル位置を復元する

### 1-5. 排他と検証

- 提供者 2 重宣言 = 起動時名前入りエラー(fixture)
- ジッタ有効時にカリング結果が不変であることの fixture(境界ぎりぎりの
  オブジェクトで on/off のドローコール数一致)
- `get_frame_plan` に `"projection_jitter": {pattern, phases}` を出す
  (観測点 — 状態は持たないので値そのものは出さない)

## 2. 標準 TAA feature(ユーザー空間 stdlib)

`engine://features/taa.json` + シェーダ。**特権なし** — 使う機構は
既存の公開面のみ: `projection_jitter` 宣言・`history:true` RT・
velocity reads・RMW 依存(bloom 連鎖の前例)。プロジェクトへコピーして
改造したら自分のもの、が公式ワークフロー。

### 2-1. パス構成

```
reads:  scene_color(RMW), velocity, taa_accum@history, depth
writes: scene_color, taa_accum
anchor: post_main(先頭 — 宣言順タイブレークで最初に置く)
RT:     taa_accum(format_class = scene 域と同じ、history: true)
```

1. `jitter_ndc` で現サンプル位置を非ジッタ化
2. velocity で前フレーム位置へ再投影 → `taa_accum@history` をサンプル
   (bilinear。catmull-rom は v2 改造例としてコメントに)
3. **近傍クランプ**(3×3 の RGB min/max AABB — YCoCg 化・variance clip
   は改造例)で ghosting 抑制
4. blend(既定 α=0.9)して `taa_accum` と `scene_color` へ出力
5. 履歴無効(初回・リセット直後)は現フレームをそのまま出力
   (`frame_index` は FrameUBO に既存)

### 2-2. 品質規約

- 入力は linear HDR(色 v4)。tonemap 前に置く(anchor 上そうなる)
- 静止時(velocity ≈ 0 かつカメラ不変)はジッタにより**超解像的に
  収束**する — これが halton 系列の意味であり、静止 golden の基準
- 既知の限界を明記: 半透明は velocity を書かないため再投影が二重像に
  なり得る(v1 は既知制限 — responsive AA マスクは改造例)

## 3. 決定性と golden

- ジッタ = frame_index の純関数 + TAA = 決定的シェーダ → 既存の
  「同一 build/arch で byte 一致」規範にそのまま乗る
- golden: ①TAA off(既存全維持)②TAA on 静止 N フレーム収束
  ③TAA on カメラ移動(orbit 決定軌道の流儀)④set_time 後 1 フレーム
  (履歴リセット = 現フレームそのまま)
- 2 回実行 byte 一致(WP89 replay 経路)

## 4. WP 分割

| WP | 内容 | gate | 依存 |
|----|------|------|------|
| **J1(機構)** | `projection_jitter` スキーマ + 系列(halton23 数表 fixture)+ scene パス限定適用 + FrameUBO 2 field + velocity 差引 + 排他/カリング不変 fixture + get_frame_plan | ジッタ有効・TAA なしで golden **不変**(サブピクセルは accumulation なしでは平均化されないため、ここは「ジッタ off が既定・on は feature 宣言時のみ」で担保 — off 時 byte 不変) | WP88/95(済) |
| **T-TAA(feature)** | taa.json + resolve シェーダ + §3 golden 4 種 + adding_features.md にユーザーレシピ(コピーして改造する手順) | §3 全部 + 既存 golden 全維持 | J1 |

## 5. 未決事項

1. blend α と clamp 方式を feature の params(.surface 側)に出すか
   固定か — v1 は JSON params で α のみ公開を仮置き
2. `render_scale`(アップスケーラ)解禁の時期 — 予約キーのまま。
   解禁時は phases_from_scale・mip_bias・出力解像度分離を一括設計
3. 半透明の responsive マスク — 需要が出たら改造例として stdlib に追記
