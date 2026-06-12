# クロスシミュレーション 設計(揺れものランタイム+ベイク受け入れ)

対象読者: エンジン担当(物理/アニメ領域の将来設計)+ 外部ツール群との合意用

ステータス: ドラフト(レビュー待ち)

前提: `design_roadmap_renderworld.md` §2.1(外部ツール連携要件)、`docs/external_tools_requirements.md`(R1〜R10)、
ツール側の対文書 `mmd_blender_mocap_lab/docs/cloth_design_2026-06-12.md`。
ロードマップ上、アニメーションは意図的に後回しであり本書はそれを変えない。本書の目的は
(a) 将来のランタイム揺れもの(springbone 系)の設計方針を先に合意すること、
(b) ツール側ベイク資産を**現行エンジンのまま**受けられる受け入れ階段を確定すること。

## 1. 役割分担(ツール側と合意済みの前提)

- エンジン: 衣装の揺れ動き(スカート・髪・リボン)のランタイムシミュレーション。
- ツール側: 着脱・重ね着・ドレープなど複雑シーンのオフラインベイク(Blender ランナー)。
- 両者の見た目一致は「同じ `cloth_setup` 資産+metrics によるパラメータ同定」で取る
  (ソルバのコード共有はしない)。

## 2. 受け入れ階段(E フェーズ)

| # | 段階 | エンジン側の作業 | 前提 |
|---|------|------------------|------|
| E0 | 剛体セグメントチェーン再生 | **なし**(`RenderWorld::updateTransforms` で受かる、ロードマップ §2.1 のとおり) | transform_seq の搬入経路(ファイル→ローダ or コマンド層) |
| E1 | VAT(Vertex Animation Texture)再生 | ほぼなし(マテリアルパス JSON+頂点シェーダ=アセット。メタ JSON 読みの薄い glue のみ) | 本線 3(シェーダ自由化キット) |
| E2 | スケルタルアニメ/スキニング | 別途設計(ロードマップ未組込のまま。本書はスコープ外と明記) | — |
| E3 | springbone ライブソルバ | feature モジュール実装(§4) | E2(skinned モード)。rigid モードは E2 前でも可 |
| E4 | ツールからのライブ注入 | コマンド層に `update_transforms`(R8 で既定義) | 並行トラック B |

ポイント: **E0/E1 で「ツールでベイクしたクロス(および液滴)を Pelican2 で再生して動画にする」
までは到達できる。** E2/E3 はゲームとしての対話性が必要になった時の投資。

## 3. アセット契約

- `cloth_setup` v1: glb extras(`pelican.cloth_setup`)。チェーン定義・コライダー
  (球/カプセル)・per-joint パラメータ(VRM 1.0 `VRMC_springBone` 互換語彙:
  stiffness / dragForce / gravityPower / gravityDir / hitRadius)。
  **JSON Schema の正本はツール側リポジトリの `schemas/`**(こちらはコピー+正本明記、
  要求書の流儀)。未知の extras は無視してよい(前方互換)。
- `pelican.transform_seq` v1: 既存 R4。**追加規約案: scale `[0,0,0]` = 非表示**
  (液滴の出現/消滅、チェーンの LOD にも使う。要合意 → §7)。
- `pelican.vat` v1(提案): メタ JSON(schema/version/fps/vertex_count/frame_count/
  bounds_min/bounds_max/texture path)+ RGBA16F 位置テクスチャ(行=フレーム、列=頂点、
  bounds 正規化)。頂点シェーダが `gl_VertexIndex` × 時刻でサンプルして再生。
  法線テクスチャは optional。R5(Alembic 第一候補)への engine 向け追補としてツール側と
  要求書を更新してから実装する。

## 4. E3: springbone ソルバ設計スケッチ

### 4.1 配置(層の判断)

シーンロジックを描画側に置かない原則(`design_roadmap_renderworld.md` §4.3 のライト
アニメーション移管と同じ判断)に従い、クロス更新は **ECS update 側の feature モジュール**
とする。レンダラーへは結果(transform / 将来はジョイント行列)のみが RenderWorld 境界を
通って流れる。

### 4.2 API スケッチ

```cpp
// feature 層: core/phys/clothworld.hpp(phys/ は現在空。最初の住人になる)
namespace Pelican {

struct ClothChainParams {   // cloth_setup v1 と 1:1
    float stiffness, drag_force, gravity_power, hit_radius;
    glm::vec3 gravity_dir;
};

DECLARE_MODULE(ClothWorld) {
  public:
    ClothSetupId loadSetup(const ClothSetupDesc &desc);   // glb extras から構築
    // rigid モード: チェーン各セグメントを RenderInstanceId に対応付け
    void bindRigid(ClothSetupId, std::span<const RenderInstanceId> segments);
    // skinned モード(E2 後): ジョイント行列バッファへの書き込み先を bind(API は E2 設計に従う)
    void step(double dt);    // 固定 dt アキュムレータ+サブステップ。仮想時刻注入と整合
    std::span<const ModelInstanceTransform> rigidResults() const;  // → RenderWorld::updateTransforms
};

} // namespace Pelican
```

- アルゴリズム: verlet 積分+距離拘束+カプセル衝突+親回転追従(標準的 springbone)。
  **外部物理ライブラリ(Bullet/Jolt)は導入しない**(この用途には過剰。導入判断は
  剛体ゲームプレイ物理が必要になった時に別途)。
- 決定性: 仮想時刻注入(ロードマップ §2.1)+固定 dt により、`set_time`/`step_frame`
  経由の再生はフレーム列が決定的になる。ゴールデンイメージテスト(本線 4)で回帰可能。

### 4.3 肥大化対策との整合

- パラメータ(cloth_setup)・VAT・シェーダはすべてアセット(「機能はなるべくアセットにする」)。
  C++ はソルバ本体のみ。
- 外部依存ゼロ・小規模のため `PELICAN_ENABLE_*` フラグは設けない(規律 3 のとおり、
  フラグ化は外部依存があるものだけ)。

## 5. テスト

- ctest: 単振り子/二重振り子の周期 vs 解析解、カプセル貫通なし assert。
- 決定性: 同一 `set_time` 列 → readback PNG のバイト一致(ヘッドレス基盤に相乗り)。
- ツール側との一致: チェーン先端軌道 RMS / 主揺れ周波数を `metrics.json` 形式で出し、
  ツール側のパラメータ同定(対文書 §5)と同じ指標で比較する。

## 6. ツール側との合意事項(チェックリスト)

1. `transform_seq` の scale `[0,0,0]` = 非表示規約(液滴・クロス共通)に合意するか
2. `cloth_setup` v1 の正本をツール側 `schemas/` に置くことに合意するか
3. `pelican.vat` v1 のフォーマット(RGBA16F・bounds 正規化・メタ JSON)で要求書 R5 を
   追補するか(Alembic は Blender レーン用として残置)
4. (隣接)液滴ツールの `pelican.strand_seq` v1(スプライン+半径列、本数可変)を
   R4 追補として受けるか
5. springbone の「互換」はスキーマ互換に留め、挙動一致は metrics 基準とすることに合意するか
6. E0 の transform_seq 搬入経路: 当面ファイル(ローダ)か、コマンド層(トラック B)を
   待つか

## 7. 参考

- ロードマップと RenderWorld 境界: `design_roadmap_renderworld.md`
- ヘッドレス描画(readback・連番出力): `design_headless_rendering.md`
- シェーダ自由化キット(VAT の前提): `design_shader_freedom_kit.md`
- ツール側対文書: `mmd_blender_mocap_lab/docs/cloth_design_2026-06-12.md`
- 要求書: `docs/external_tools_requirements.md`(R4/R5/R7/R8)
