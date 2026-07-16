# USD 変換レーンと OpenPBR マテリアル対応(v1)

対象読者: エンジン担当・DCC 連携を使う人・マテリアルを書く人。
ステータス: v1 ドラフト(2026-07-16。敵対レビュー前)。
前提: `design_asset_format_policy.md`(二層モデル — ソース層はエンジン
非リンク)、`design_material_shading.md`(A/B/C 梯子・同梱 lighting =
特権なし stdlib)、`design_project_dcc_houdini.md`(pelican.import
manifest)、pelican-import-tools(外部ツール契約: psd-tools/toktx の流儀)、
サブセット原則(形式拡張はエンジン先行)。

## 0. 位置づけ — 既存原則への写像

| 要求 | 置き場 | 原則 |
|------|--------|------|
| **USD** | **ソース層の変換レーン**(import-tools レシピ)。ランタイムは今後も USD を読まない | 二層モデル §2(2026-07-02 決定の実行) |
| **OpenPBR** | ①**同梱 surface(B 層スニペット)= 特権なし stdlib** ②import 時の**写像表**(USD/glTF → pelican.material) | 梯子 B・「同梱物に特権なし」 |

**やらないこと(明記)**: USD/MaterialX ランタイムのリンク、USD
コンポジション(variant/payload)の実行時解決、OpenPBR 全項目の完全実装
(§2-2 のサブセット宣言)。

## 1. USD 変換レーン(import-tools `usd` レシピ)

### 1-1. ツール契約

- レシピ = `usd`(imports.rules.json から glob→レシピで起動)。
  外部ツールは **`usd-core`(pip 配布の OpenUSD)を一次候補**、
  メッシュ/マテリアル抽出の実装比較として **guc(USD→glTF)** と
  **Blender headless** を試作で評価(§5 未決 1 — 評価軸: 決定性・
  UsdSkel 対応・MaterialX/UsdPreviewSurface の読み・導入の軽さ)
- 不在時は導入手順つきエラー(toktx と同じ流儀)。ツール名と version を
  pelican.import manifest に記録(バイト決定性は強制しない — 既存規約)

### 1-2. 変換スコープ(段階)

| 段階 | 入力 | 出力 |
|------|------|------|
| **U0** | 静的メッシュ・xform 階層・UsdPreviewSurface / OpenPBR マテリアル・テクスチャ | glb(1 ファイル or コンテナ + `#fragment`)+ テクスチャ(png/KTX2 レシピへ連鎖)+ scene v1 断片(objects[] の配置)|
| **U1** | UsdSkel(スキン・スケルトン・アニメ)・カメラ | glb skin/clips・camera コンポーネント |
| **U2** | xform アニメ(rigid)・PointInstancer | transform_seq / インスタンス配置(scene v1)|
| 将来 | トポロジ可変(Alembic 的用途) | VAT レシピへ連鎖 |

- usdz は unzip して同レーン
- **単位・軸**: metersPerUnit / upAxis を読んで glb(m・+Y)へ正規化。
  正規化パラメータを manifest に記録
- コンポジション(variant 等)は **import 時に flatten**(選択は
  レシピ引数)。ランタイムに variant 概念を持ち込まない

### 1-3. 受け入れゲート(U0)

- 決定的出力(同一入力 → 同一 sha256。ツール version 固定の下で)
- fixture: 小 USD(usd-core で生成)→ glb → エンジンロード → golden 1 枚
- 既存 DCC レーン(houdini-adapter)と manifest 形式を共有

## 2. OpenPBR 対応

### 2-1. 二つの成果物

1. **`openpbr` surface(同梱 B 層スニペット)**: standard / toon と並ぶ
   第 3 の標準ライブラリ。特権なし・コピーして改造可能。
   `.surface` の params 宣言に OpenPBR の命名を採用(スキーマの発明を
   避ける — 名前の正本は OpenPBR 仕様)
2. **写像表(import 側)**: USD(UsdPreviewSurface / MaterialX OpenPBR)
   → `pelican.material`(values + textures 辞書)。glTF KHR_materials_*
   → OpenPBR パラメータの対応もここで規定(コード化は import-tools)

### 2-2. v1 サブセット(宣言的に線を引く)

**対応(v1)**: base(weight/color/metalness/diffuse_roughness)・
specular(weight/color/roughness/roughness_anisotropy は保留・IOR)・
emission(luminance/color)・coat(weight/color/roughness — 1 層)・
geometry(opacity/normal)・alpha モード。

**明示的未対応(参照 = 名前入り WARN + 既定値 fallback)**: fuzz・
iridescence(thin-film)・transmission/subsurface・dispersion・
thin_walled。**黙って無視しない**(WARN に「OpenPBR の何がどの既定に
落ちたか」を出す — 検証方針の流儀)。

- OpenPBR の version を manifest/マテリアルに記録(**1.1 に pin**。
  版上げは additive 改訂)
- glTF core PBR との関係: 現行 standard surface は metallic-roughness —
  `openpbr` surface は**別スニペット**として追加し、既存マテリアルは
  無変更(golden 全維持)。KHR_materials_emissive_strength /
  _specular / _ior / _clearcoat の読み取りを glTF loader に追加する場合は
  additive(未対応 KHR は現状どおり無視 → 将来 WARN 化を検討)

### 2-3. dogfooding

`openpbr` surface 自体を公開ライブラリ(pelican_light/shadow 等)のみで
実装(standard/toon と同じ制約)。example に OpenPBR サンプルマテリアル
1 個(coat の効きが golden で見える球)。

## 3. 決定性と検証

- import: 同一入力・同一ツール version → 同一出力 sha256(manifest 照合)
- runtime: `openpbr` surface の golden(coat on/off・emission・alpha)+
  dump-lowered-material(「B は C の糖衣」の既存 CI)に openpbr を追加
- 写像 fixture: UsdPreviewSurface の代表値 → pelican.material values の
  数表一致(import-tools 側 pytest)

## 4. WP 分割

| WP | 内容 | gate | 依存 |
|----|------|------|------|
| **M-PBR0** | `openpbr` surface(§2-2 サブセット)+ 未対応 WARN + golden + dump-lowered 追加 | §3 runtime 項目。既存 golden 全維持 | M3a(済) |
| **U-USD0** | import-tools `usd` レシピ(§1-2 U0)+ ツール選定レポート + 決定性 fixture | §1-3。**別リポジトリ書込は import-tools レシピのみ** | K3(済)・M-PBR0(写像先) |
| **U-USD1** | UsdSkel・カメラ(§1-2 U1) | skel round-trip → エンジン再生 | U-USD0・WP38(済) |
| **U-USD2** | transform_seq・PointInstancer(§1-2 U2) | houdini-adapter と同形式の manifest | U-USD0 |
| 後続 | KHR_materials_* 読取拡張 / VAT 連鎖 / MaterialX 直読(やらない方針の再確認だけ) | — | — |

## 5. 未決事項

1. **U0 のツール選定**(usd-core 自前 vs guc vs Blender headless)—
   U-USD0 の中で 3 案の試作比較レポートを成果物に含めて決定
2. OpenPBR の anisotropy(タンジェント要求)— 頂点属性の現状と相談。
   v1 は保留に倒す
3. USD の Y-up 以外(Z-up)・単位の丸め誤差の扱い — manifest 記録で
   足りるか、警告閾値を置くか
4. OpenPBR → toon の縮退写像(NPR プロジェクトが OpenPBR アセットを
   受け取った場合)— v2 の課題として記録のみ
