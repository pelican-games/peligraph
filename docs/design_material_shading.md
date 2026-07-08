# マテリアル/シェーディング接続点(v1)

対象読者: エンジン担当・プロジェクトでカスタムシェーダを書く人。
ステータス: v1.1(2026-07-08 レビュー反映 — **A/B/C 梯子と textures を追加、
ユーザー合意済み**。M1 = WP58 実装済み)。
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
- `textures`(v1.1 追加): 名前 → パスの辞書(カスタムテクスチャスロット)。
  set 2 の固定 PBR スロットの後ろに**宣言順で binding を割当**(対応規則は
  shader_contract.md の契約に含める)。値は通常のパス参照(#フラグメント可)。
  **std140/binding の手書き一致は要求しない** — params/textures から GLSL の
  uniform block + sampler 宣言をツールが生成し(`<stem>.params.glsl`)、
  シェーダは include するだけ(踏み抜き防止。M2 で実装)

### 3-1. 供給の 3 段(A/B/C 梯子 — 2026-07-08 合意)

**書きやすさの主役は A と B**。旗ではなくキーの選択がモードを決める
(参照 = 存在の原理):

| 段 | 書き方 | 書く量 | 位置づけ |
|----|--------|--------|---------|
| A: uber | `base` + `defines` + `params` + `textures` のみ(shader キーなし) | ゼロ | 既定。カスタムマテリアルの大半はここで足りる |
| B: surface | `"surface": "shaders/lava.surface"` — **エンジンのテンプレートがユーザーファイルを include する**(逆 include)。ユーザーは表面関数(+任意で頂点変位)だけ書く | 数行〜数十行 | ライティング・影・スキニング・パス対応はテンプレートが所有 — **機能追加でユーザーファイルは壊れない** |
| C: raw | `"shader": "shaders/weird"` — main() まで全部書く。エンジンライブラリ(`engine://shaders/lib/`)の利用は任意 | 全部 | 完全な自由・自己責任(契約追従義務はユーザー側)。**.spv 直接供給もここ** |

B の規律(レビューで確定):

1. **フックは 2 点から開始**(`pelican_surface(inout PelicanSurface)` /
   `pelican_vertex_displace(...)`)。フック追加はエンジン側の凍結改訂扱い
   (Unity surface shader の迷宮化を規律で防ぐ)
2. **ライティングモデルはフックでなく選択肢**: `"lighting": "standard" | "toon"`
   (defines でテンプレート内ライブラリが切替)。**トゥーンを B の範囲内に
   収める鍵**(VRM 需要)
3. `PelicanSurface` 構造体は公開 API — **`PELICAN_SURFACE_V1` で版数を刻み**、
   意味変更は凍結改訂の流儀
4. テンプレートのピン留め(project:// へコピー)は**許すが C と同義**
   (自己責任へ移行)。同時に B→C の公式移行手順を兼ねる(崖をスロープに)
5. **web は A まで対応**。B/C は native 限定(web は WARN + 既定 PBR
   フォールバック — サブセット原則の明示的適用)
6. エラーは**ユーザーファイル起点に翻訳**して表示(include 展開後の
   テンプレート行番号を出さない)+ マゼンタフォールバック + ホットリロード

### 3-2. SPIR-V ABI が真の契約(自由化との整合)

梯子はすべて**ソースレベルの糖衣**であり、最終形は同一の SPIR-V ABI
(shader_contract.md)。precompiled .spv の自由は C 層として無傷
(既存の割り切りどおり .spv は defines variant 対象外の固定 1 バリアント)。
**B はソース供給専用**(テンプレートと混ぜてからコンパイルするため —
明文化された唯一の制約)。dist-bake(B4)で B/C も全 variant を .spv 化
するので、**ランタイム契約に include は漏れない**。

### 3-3. パス variant 規約(影の剥離防止)

マテリアルの頂点ロジック(変位含む)は main パス専用ではなく、
**同一シェーダを `PELICAN_PASS_DEPTH` / `PELICAN_PASS_VELOCITY` 付きで
再コンパイルして depth / velocity パスでも使う**(揺れる草の影が揺れない
事故の構造的防止)。B ではテンプレートが自動で満たす。C では作者責務
(contract に記載)。M2 実装が main 専用の作りにならないよう最初から要件化。

### 3-4. 属性と欠損の既定

- 頂点属性は現行 5 つ(pos/normal/uv/color/tangent — contract 記載)+
  **予約 location**(uv1 等)を契約に確保し、存在は `PELICAN_HAS_*` defines
- 未バインドのテクスチャスロットは**ダミー自動バインド**(白 / 平坦法線)—
  variant 爆発より「とりあえず動く」を優先。defines 分岐は opt-in
- `render_state`(v1.1 追加、マテリアルの拡張キー): blend
  (`opaque|blend|additive`)・depth_write・cull の最小セット。
  glTF 由来の alphaMode/doubleSided はこれへの別名として受理
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

## 6. 移行(WP 候補 — v1.1 改訂)

| 段階 | 内容 | 依存 |
|------|------|------|
| M1 | pelican.material パーサ + 検証 + 契約文書 | **完了(WP58)** |
| M2 | **A 完成**: バインダ(params UBO・textures 辞書・material コンポーネント・glb extras・render_state・ダミーテクスチャ)+ `<stem>.params.glsl` 生成 + **contract 記載の実装差分 3 件の解消**(set 0 の意味統一・push constant 分割 enforcement・params UBO)。既定 PBR は現行挙動維持(golden 全維持)。パス variant 要件(§3-3)込み | M1 |
| M3 | **B 実装**: include 機構(shaderc includer)+ テンプレート(surface/vertex_displace 2 フック + lighting standard/toon)+ PelicanSurface V1 + エラー翻訳 + example に surface マテリアル 1 個 + golden | M2 |
| M4 | **C 整備 + web**: エンジン GLSL ライブラリ公開 + テンプレートコピー手順文書化 + web 側 A 対応(B/C はフォールバック) | M3 |

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
