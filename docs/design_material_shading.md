# マテリアル/シェーディング接続点(v1)

対象読者: エンジン担当・プロジェクトでカスタムシェーダを書く人。
ステータス: v1 ドラフト(2026-07-08。レビュー前)。
前提: `design_render_feature_modules.md`(shader_defines・variant 機構)、
`design_scene_format.md`、[PF] シェーダ stem 規約、[PFW] サブセット原則。

## 0. 目的とスコープ

「ユーザーが自分のシェーディングを書く」ときの入口を形式として定義する。

1. **pelican.material v1** — マテリアル定義のプロジェクト形式
   (glTF PBR 1:1 + カスタムシェーダ接続 + パラメータ)
2. **マテリアルシェーダの契約** — set / push constant / 頂点入力の規約を
   文書化された安定 API に昇格
3. **variant 管理の規律** — feature defines × マテリアル defines の爆発抑制

スコープ外: シェーディングモデル自体の刷新(現行 forward を維持)、
ノードグラフ的マテリアルエディタ(devstudio の遠い将来)、bindless。

## 1. 現状(2026-07-08 実装ベース)

- `MaterialInfo` は **マテリアル単位の vert/frag シェーダを既に持つ**
  (ShaderBundleId)+ 固定 PBR スロット(base_color / metallic_roughness /
  normal / emissive)+ VAT 拡張。ランタイムの器はほぼある
- set 規約は `pelican_sets.hpp` に定数として存在:
  set 0 = FRAME、1 = PASS_INPUT、2 = MATERIAL、3 = FREE。
  push constant は engine 64B(mvp)+ shader 64B
- 欠けているのは**プロジェクト側**: glb のマテリアルは固定経路でロードされ、
  「このマテリアルはこのシェーダ・このパラメータ」を**データで**言う手段がない

## 2. カメラと同じ「glTF 同等以上」方針

1. **同等** = glTF `pbrMetallicRoughness` を損失なく読む(factor/texture、
   normal/occlusion/emissive、alphaMode/alphaCutoff/doubleSided)。
   glb 内マテリアルはそのまま既定 PBR シェーダで出る
2. **以上** = カスタムシェーダ・追加パラメータは **glTF を汚さない層**
   (glb では extras、プロジェクトでは pelican.material ファイル)に置く

## 3. pelican.material v1(形式)

置き場所: `materials/*.json`(project://)。エンベロープは他形式と同じ
schema/version。**web でも読める形式にする**(サブセット原則 — shader 参照は
stem 規約なので native/web が同じ記述で成立する)。

```json
{
  "schema": "pelican.material",
  "version": 1,
  "materials": [
    {
      "name": "lava",
      "base": {
        "baseColorFactor": [1, 1, 1, 1],
        "baseColorTexture": "project://textures/lava_albedo.png",
        "metallicFactor": 0.0,
        "roughnessFactor": 0.8,
        "emissiveFactor": [2.0, 0.5, 0.1]
      },
      "shader": "project://shaders/lava",
      "defines": ["LAVA_FLOW"],
      "params": { "flow_speed": 0.35, "distortion": [0.1, 0.2] }
    }
  ]
}
```

- `base`: glTF pbrMetallicRoughness のキー名を**そのまま**使う(1:1)。
  省略時は glTF 既定値。texture 参照は project:// / engine://
- `shader`: 拡張子なし stem(省略 = エンジン既定 PBR)。vert/frag は
  stem から解決(既存規約)。**vert のみ・frag のみの差し替えも可**
  (`shader_vert` / `shader_frag` で個別指定 — 未決 1)
- `defines`: bool フラグのみ(§5 の規律)
- `params`: 自由スキーマ → **material params UBO**(set 2 に追加 binding)。
  数値(float / vecN / int)のみ、レイアウト規約は宣言順 std140。
  64B の shader push constant は「毎フレーム変わる少量」用として残す
- 適用: scene v1 のオブジェクトに `material` コンポーネント
  (`{"name": "material", "ref": "lava"}`)で割当。glb 側マテリアルの
  上書き。**glb extras**(`pelican_material: "lava"`)でも同じことが言える
  (DCC 側で割当を焼く経路 — R6 の extras 規約と同じ)

## 4. マテリアルシェーダの契約(安定 API 化)

`docs/shader_contract.md`(新設、adding_features.md から参照)に以下を明文化:

| 項目 | 規約 |
|------|------|
| set 0 (FRAME) | カメラ行列・時間・ライト UBO(エンジン管理、読み取りのみ) |
| set 1 (PASS_INPUT) | パス入力(前段 RT — feature が宣言) |
| set 2 (MATERIAL) | PBR テクスチャ固定スロット + **params UBO(新設)** |
| set 3 (FREE) | 予約(ユーザー/将来機能) |
| push constant | engine 64B(触るな)+ shader 64B(自由) |
| 頂点入力 | position/normal/uv/(tangent)の location 固定表 |
| defines 合成 | feature 由来(`PELICAN_FEATURE_*`)→ マテリアル由来の順に結合 |

契約の変更 = 破壊的変更としてバージョン管理(形式凍結の流儀)。
これが「エンジンを読まずにシェーダが書ける」状態の定義。

## 5. variant 管理の規律

- キャッシュキー = (shader stem, ソート済み defines 集合, パス種)。
  WP28 の実行時コンパイル機構をそのまま使う(新設なし)
- **爆発抑制の規律**: defines は bool のみ・数値は params UBO へ。
  feature defines は config 全体で一様(マテリアル毎に変えない)。
  よって variant 数 = マテリアル defines の実使用組合せ × feature 組合せ
  (プロジェクト実測で管理 — `--dump-frame-plan` の流儀で
  `dump-shader-variants` を用意、未決 3)
- 配布: dist-bake(B4)がこの variant 列挙を焼く対象になる(設計整合のみ、
  実装は B4)

## 6. 移行(WP 候補)

| 段階 | 内容 | 依存 |
|------|------|------|
| M1 | pelican.material パーサ + 検証(pelican_project、純ロジック)+ 契約文書 | なし |
| M2 | バインダ: params UBO・material コンポーネント割当・glb extras。既定 PBR は現行挙動維持(golden 全維持) | M1 |
| M3 | カスタムシェーダ実証: example に 1 マテリアル + golden。web 側 WW: 同形式を読み既定 PBR にフォールバック(shader が web に無い場合は WARN + 既定)| M2 |

## 7. 未決事項

1. vert/frag 個別差し替え(`shader_vert`/`shader_frag`)を v1 に入れるか
   (推奨: 入れる — VAT が vert 差し替えの現実例)
2. params UBO のレイアウト: 宣言順 std140 か、明示 offset か
   (推奨: 宣言順。シンプル優先、ツールが offset を計算)
3. variant 列挙ダンプ(`dump-shader-variants`)を M1 に含めるか
4. alphaMode blend の描画順(半透明ソート)は本設計のスコープ外 —
   必要になった時点で別文書(ソートはフレームグラフでなくパス内の問題)
5. occlusion texture スロットは現行 MaterialInfo に無い — M2 で追加するか
   IBL(ライティング設計)と同時か
