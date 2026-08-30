# レイトレーシング拡張設計 — DCC 級の自由度へ(v1)

状態: v1 草稿。敵対的設計レビュー前。

## 0. 要求

利用者要求(2026-08-30): **速度性を重視しない DCC ツールと同程度に「好き勝手」できる
RT**。到達点は視覚的正しさと表現の自由度であり、リアルタイム性能は制約にしない。
恒常制約: **機能のパージ性は下げない**(RT は capability gate 付きの purgeable feature
のまま拡張する)。fail-fast / 名前付きエラー / 決定性の家風は全段に適用する。

## 1. 現状の固定点(実測。file:line は 2026-08-30 HEAD)

| 面 | 今 | 根拠 |
|---|---|---|
| 車線 | ray query(fullscreen 内)と RT pipeline(compute_tasks の `ray_tracing`)の 2 本 | features/rt_shadow_mask*.json |
| stage | raygen ちょうど 1 + miss 複数 + closesthit 複数。**any_hit / intersection / callable は名前付き拒否** | computetask.cpp:1190,1194 |
| SBT | 平坦(raygen==1 検査、record = handle のみ、shader record データなし) | shaderbindingtable.cpp:66 |
| hit group | 全 geometry 共通(per-instance / per-material 割り当てなし、sbtOffset 未使用) | 同上 + accelerationstructure.cpp |
| AS | エンジン所有。静的 geometry のみ(`isBlasIneligibleStaticGeometry`)、mask / flags / customIndex の宣言面なし | accelerationstructure.cpp:43 |
| 再帰 | 宣言不可(深度 1 相当) | computetask.cpp に該当なし |
| dispatch | `rays_from: {port}`(port extent がレイ数)のみ | computetask.cpp:1279 |
| material | hit shader を供給しない(opaque 収束の note のみ) | materiallowering.cpp:230 |
| shader | .rgen/.rmiss/.rchit は resolver 済み(engine:// / project:// 両対応) | shadersourceresolver.cpp:118 |

## 2. 到達点(「好き勝手」の定義)

DCC 的に書けるべきもの: alpha-tested / 半透明の hit 制御(anyhit)、
procedural geometry(intersection + AABB)、callable による shader 分岐、
material ごとの hit shader、instance の mask / flags / custom index、
再帰(パストレ)、shader record データ、複数 TLAS(レイヤ分け)。
**非目標**: リアルタイム最適化(denoiser・SER・motion blur AS 等は別設計)。

## 3. 段階(1 段 = 1 出荷単位。各段が単独で挙動を変える)

### RT1: SBT の一般化 — hit_groups・anyhit・再帰・record 定数

```json
"ray_tracing": {
  "raygen": "project://shaders/pt",
  "max_recursion": 4,
  "miss": [ { "shader": "project://shaders/pt", "record": { "sky_tint": [1,1,1] } } ],
  "hit_groups": [
    { "name": "opaque",  "closesthit": "project://shaders/pt" },
    { "name": "foliage", "closesthit": "project://shaders/pt",
      "anyhit": "project://shaders/pt_alpha" } ],
  "default_hit_group": "opaque" }
```

- SBT を group 配列 + 固定長 record(handle ∥ 定数ブロック)に一般化。
  record 定数は StructFieldSchema 語彙で型付け(leaf 検証、WP358 の資産)
- `max_recursion` はデバイス上限超過で名前付き拒否
- 旧形(miss/closesthit の文字列 list)は sugar として受理し、正準化で新形へ
- **オラクル**: CPU 参照レイトレーサ(std のみ、test/ 配下)を新設し、
  fixture scene の交差結果・可視性を device 精度境界つきで比較
  (WP357 の「不等式オラクル」の流儀。golden の目視合わせを禁止する土台)

### RT2: material 束縛 — per-instance hit group と sbtOffset

- material 宣言に `ray_hit_group`(名前参照)を追加。
  instance ごとに TLAS の `instanceShaderBindingTableRecordOffset` を割り当て
- 未定義 group 参照 / RT パス側に該当 group 不在は名前付き
- WP354 の material drill-through(名前付き必須フォローアップ)とここで合流:
  material の shader 出所が RT hit にも投影される
- オラクル: 2 material × 2 group の fixture で、group 差(例: 片方だけ anyhit で
  市松に discard)が CPU 参照と一致

### RT3: AS の宣言面 — mask / flags / custom index / procedural

- component 側(simplemodelview / 新 `procedural_volume`)に
  `ray: { mask, flags: [opaque|no_duplicate_anyhit|...], custom_index }` を追加
- `intersection` stage 解禁 + AABB ソース(component の宣言 bounds か storage port)
- raygen 側は `rayMask` で層別トレースが可能に(ライト層・カメラ層の分離)
- BLAS 適格性の拡張は段内で明示(skinned は RT3 では**やらない**と宣言)

### RT4: callable + 動的 record

- `callable` stage 解禁(SBT に callable region 追加)
- record の動的更新(frame ごとの定数供給を port と同じ流儀で宣言)

### RT5: 複数 TLAS と車線の一級化

- 名前付き TLAS 宣言(所属は component の `ray.scene` 参照)、
  raygen へ複数 AS binding
- ノードエディタ上で RT パスを一級ノード化(compute 車線からの昇格。
  hit_groups・record・AS 参照をソケット/インスペクタで編集)
- RPE12b の凍結・A/B 測定ループに RT パスを乗せる

### 依存と順序

RT1 → RT2 → RT3 → (RT4 ⇄ RT5 は独立)。skinned BLAS・denoise・
motion blur は本設計の外(需要実測後に別設計)。

## 4. 横断原則

- **capability gate**: 各段は `pelican.vulkan.ray_tracing_pipeline@1` 配下の
  追加 capability(例 `…ray_tracing_recursion@N`)で宣言し、不在デバイスでは
  feature 単位の名前付き skip(パージ性維持。web/player subset 原則と整合)
- **fail-fast**: SBT の group 参照・record 型・mask 範囲・再帰上限、全て load 時名前付き
- **決定性**: RT 出力の golden は CPU 参照オラクル合格を機械的ゲートにする
  (WP357 の golden updater 方式を踏襲)。参照器は交差のみ(シェーディングは
  fixture shader を単純化して算術一致域に置く)
- **提示**: 各段で planviewer / studio の表示(hit group・record・mask)を同段に含める
  (G0 可読性の原則: 自動挿入・暗黙値は見えるところに)

## 5. 未決事項(レビューで攻撃してほしい点)

1. CPU 参照オラクルの精度境界の定式化(watertight 交差 vs GPU の RT コア差。
   device の `…rayTracingPipeline` 精度保証は貧弱 —— 不等式で書き切れるか)
2. record 定数の ABI(std430 相当の layout を leaf 語彙からどう決定的に導くか)
3. per-instance offset の再構築コスト(TLAS rebuild 頻度と編集ループの整合)
4. ray query 車線との二重化(RT3 の mask/flags は ray query 側 AS にも適用するか)
5. material drill-through との統合順序(WP354 フォローアップを RT2 に吸収するか独立か)
