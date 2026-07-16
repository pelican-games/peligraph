# USD 変換レーンと OpenPBR マテリアル対応(v2.1 — 条件付き受理)

対象読者: エンジン担当・DCC 連携を使う人・マテリアルを書く人。
ステータス: v2 ドラフト(2026-07-16)。v1 は敵対レビュー
`docs/design_reviews/2026-07-16_taa_usd_review_codex.md` §2(以下
「レビュー」)で **Reject** — ①OpenPBR の写像先が現行 material/surface
ABI に存在しない(PelicanSurfaceV1 は凍結済みで coat/IOR を持てず、
pelican.material は custom texture override を parser が reject、
alpha は surface 固定 render state)②外部 material と GLB primitive の
binding 経路が未実装 ③**guc は方向が逆(glTF→USD)かつ archived** —
候補集合の事実誤認 ④manifest schema 不一致(既存実バグ含む)と
決定性記述の自己矛盾 ⑤axis/unit/flatten/USDZ の再現契約不足。
v2 = 全 blocker 反映 + レビュー §2.2 の WP 再分割を採用。
前提: v1 と同じ(二層モデル・A/B/C 梯子・import manifest・
サブセット原則)。

## 0. 位置づけ(維持)+ やらないこと

USD = ソース層の変換レーン / OpenPBR = ①同梱 surface(特権なし
stdlib)②import 写像表。USD/MaterialX のランタイムリンク・実行時
コンポジション解決・OpenPBR 全項目実装はやらない。

## 1. OpenPBR — 表現 ABI を先に決める(M-PBR0a/0b の二段)

### 1-1. M-PBR0a: representation / routing ABI(最初の WP)

v1 の「別スニペットだから既存無変更」は不成立(レビュー USD-1)。
以下を**先に決めて実装する**:

1. **OpenPBR 対応 input の置き場表**: 各 input を
   `surface param / custom texture / render state / lighting` の
   どこへ保持するかを 1 表で確定
2. **shading 経路の決定**: `PelicanSurfaceV2 + G-buffer 拡張` か
   `pelican_lighting_v1 hook + forward 直行` か。
   **v2 の推奨 = lighting hook + forward**(理由: PelicanSurfaceV1 は
   凍結 ABI・coat を G-buffer に足すのは deferred 展望 §3-9 の
   再設計と衝突・material 設計自身が任意 BRDF は forward 行きと規定
   済み)。deferred 対応は OpenPBR 需要が deferred 化と重なった時に
   PelicanSurfaceV2 として別途
3. **per-material custom texture override ABI**: 現行 parser は
   top-level `textures` を明示 reject・`.surface` texture 宣言は
   default_reference 1 個のみ。material 側から surface 宣言 texture を
   差し替える形式(`pelican.material` の additive key)+ lowering +
   binder を新設
4. **alpha/render state + doubleSided(再レビュー USD-C1 /
   M-PBR0a-STATE)**: OPAQUE/MASK/BLEND × single/double-sided は
   surface 固定 state の現行制約に対し、
   **`{opaque, mask, blend} × {single_sided(cull=back),
   double_sided(cull=none)}` の最大六 surface variant へ決定的に
   route** する(material-level cull override は入れない — 予約)。
   MASK = opaque blend/depth-write + alpha cutoff discard、BLEND =
   blend/depth-read-only。gate: 各組合せの pipeline state dump・
   front/back view・cutoff 境界・primitive binding golden。
   `doubleSided` は input 置き場表と binding ABI に含める
5. **material → GLB primitive binding ABI**(レビュー USD-2):
   `USD prim/subset path → GLB mesh/primitive index → pelican material
   名`の安定 mapping を import が出力し、**scene loader / model
   template がそれを消費して primitive 単位で material を差し替える**
   経路を新設(material 設計 §「scene material component」案の実装化)。
   collision・missing・duplicate・fragment ロードは名前入り hard error
6. gate: dump-lowered-material 追加・schema error fixture・
   **既存 material の golden 全維持**(新 ABI は additive)

### 1-2. M-PBR0b: openpbr surface + 写像表

M-PBR0a の ABI 上に実装:

- `openpbr` surface スニペット(forward・公開 lighting ライブラリのみ =
  特権なし)。**variant の artifact 形(再レビュー USD-C2 /
  M-PBR0b-VARIANTS)**: 現行 `.surface` は variant/inheritance を
  持たないため、**(B) 薄い `.surface` wrapper ×6(三 alpha × 二 cull)
  + lighting 実装は単一の登録済み engine GLSL include に集約**を採る
  ((A) build 時生成は生成器の新設が要るため不採用)。全 wrapper の
  params/textures の名前・順序・型・default・colorspace が同一で、
  差分が render_state/cutoff/cull だけであることを **parser fixture で
  比較**。variant 名と import routing は決定的・六 variant の
  shader/pipeline cache 列挙を gate に
- **OpenPBR は 1.1.1 に exact pin**(仕様 tag/commit hash を manifest と
  マテリアルに記録。patch 更新も自動 additive とみなさない —
  レビュー USD-6)
- v1 サブセット(base/specular/IOR/coat 1 層/emission/normal/alpha)の
  **unit・range・default・colorspace・channel を数表で規範化**
- 写像表: UsdPreviewSurface / MaterialX OpenPBR(allowlist — §2-3)/
  glTF KHR_materials_* → 上記パラメータ
- **未対応 WARN の規範**(USD-6): 「node が存在したら」ではなく
  「**非 default 値が authored、または connection があり結果に寄与する
  場合**」のみ WARN。WARN code・USD prim path・input 名・fallback 値を
  machine-testable に固定
- gate: base/specular/IOR/coat/emission/normal/alpha の数値・画像
  golden + 既存 material 全 golden 維持

### 1-3. glTF loader の KHR 拡張(USD-7 — 別レール)

現行 loader が読む material 拡張は `KHR_materials_emissive_strength`
のみ。specular/ior/clearcoat 等の追加は **M-PBR0a の binding 問題を
解いた後**に、①拡張なし既存 material の byte-identical gate ②拡張あり
の新 routing gate を分けて行う(同じ material の二経路解釈を作らない)。

## 2. USD 変換レーン(U-USD0a〜0c の三段 + U1/U2 各二分割)

### 2-1. U-USD0a: ツール選定 spike(production レシピと分離)

- **guc は候補から削除**(glTF→USD の逆方向ツール・2024-06 archived —
  v1 の事実誤認を訂正)
- 候補: **`usd-core`(PyPI wheel)自前抽出を一次**、Blender headless は
  **lossy UsdPreviewSurface fallback** としてのみ評価(公式 manual が
  import の layers/references 非対応・PreviewSurface lossy を明記)
- **UsdMtlx の probe**: MaterialX 読取は core の値読取ではなく
  file-format/discovery plugin + 検索 path を要する — wheel に plugin/
  data が同梱されるかを **Windows CI で probe**(なければ MaterialX は
  「reference implementation で bake 済みの値のみ」等の縮退を明記)
- **比較軸(レビュー指定の完全版)**: 変換方向 / maintenance・
  security / license と再配布 / Windows CI インストール / exact
  version・hash / resolver・plugin 可用性 / variant・payload・layer /
  UsdGeomSubset・material binding / MaterialX 忠実度 / UDIM /
  colorspace / texture transform・channel packing / primvar
  interpolation / triangulation・normals・tangents / negative
  determinant / 決定的 naming・順序・float 直列化・GLB metadata /
  診断 / 性能
- 成果物 = 固定 USD corpus での比較レポート + 採用 tool/version/hash/
  license/導入手順

### 2-2. U-USD0b: コンポジションと静的ジオメトリ

採用レーンで実装(**material は default standard へ落とすため
M-PBR0 に依存しない** — 並行可能):

- variant selection・payload load policy・population mask・defaultPrim・
  resolver/search path を**レシピ引数として受け、全て manifest に記録**
- **正規化契約(NORMALIZATION — 「記録する」の中身を規範化)**:
  authored `upAxis`/`metersPerUnit`(欠損時の既定値も)・適用した 4×4
  basis/scale・precision/rounding・xform を node に残すか vertex bake
  するか・negative determinant 時の winding/normal/tangent 反転・
  normal の変換行列 — 全 metadata key と型を manifest schema と
  fixture に固定
- **localization**: flatten は composition を単一 layer に焼くだけで
  外部参照の移送は別問題 — dependency closure の resolved URI + hash を
  記録し、texture 等は import 出力側へ localize
- **静的 geometry 抽出規範(再レビュー USD-C3 / U-USD0b-GEOMETRY —
  比較 spike だけでなく本 WP の gate)**: UsdGeomMesh の points/
  faceVertexCounts/faceVertexIndices・orientation・holes・subdivision
  policy、UsdGeomSubset の family/type、primvar の constant/uniform/
  varying/vertex/faceVarying + indexed values を規範化。triangulation・
  normal/tangent の生成/保持・UV set・negative determinant 反転後の
  順序を固定し、**USD prim/subset path → GLB mesh/primitive index の
  mapping が決定的 sort 後にも一致する fixture** を gate に置く
- **USDZ の安全展開(SAFETY)**: 一般 unzip 禁止。package/resolver API
  経由か、root layer 選択・path traversal・絶対パス・symlink・重複/
  大文字小文字衝突・archive bomb 上限を検査する安全展開器 + fixture
- gate: GLB + scene の runtime ロード fixture・dependency closure hash・
  **2 回実行 SHA-256 一致**

### 2-3. U-USD0c: マテリアルとテクスチャ(依存: 0b + M-PBR0b)

- UsdPreviewSurface + **allowlist 済み MaterialX OpenPBR**(受ける
  node/category・constant・image・channel・colorspace・texcoord・
  transform・connection を列挙 — bake するもの / reject するものを
  分ける)→ M-PBR0b の写像表へ
- per-primitive/subset binding(M-PBR0a の binding ABI へ出力)・
  PNG/KTX2 連鎖・UDIM policy・colorspace・texture transform・
  WARN/reject
- gate: **生成した GLB + scene + pelican.material をエンジンが実際に
  OpenPBR で描く golden**

### 2-4. U-USD1a/1b・U-USD2a/2b(各二分割)

- **U-USD1a**: UsdSkel(joint 順・bind/rest・weights・clip/time
  sampling の fixture)/ **U-USD1b**: カメラ(persp/ortho・aperture/
  focal・clip・axis/unit・animated)
- **U-USD2a**: rigid xform アニメ → transform_seq(interpolation・
  timeCodesPerSecond)/ **U-USD2b**: PointInstancer(prototype
  mapping・protoIndices・ids/inactiveIds・per-instance primvars・
  scale/orientation)

## 3. 全 USD WP 共通 contract(レビュー §2.2 逐語)

1. **MANIFEST**: engine と import-tools 双方が受理する output
   schema/version の表を一つの fixture に同期(`gltf`・`png`・
   `pelican.scene`・**`pelican.material`(新規追加)**・
   **`khronos.ktx2`**)。`source.toolchain[]` に全 tool/package の
   name/version/hash・レシピ引数・resolver/plugin 環境・OpenPBR exact
   version を記録。
   **既存実バグ(レビューで発見)**: import-tools の ktx2 レシピが出す
   `khronos.ktx2` v2 を現行エンジン parser が reject する
   (`importmanifest.cpp` の validateOutputSchema)— **本設計と独立の
   即時修正対象**(小 WP or 直接修正)
2. **DETERMINISM**(v1 の矛盾を解消): **U-USD の受け入れ gate は
   SHA-256 二回一致で統一**(同一 source dependency closure + 同一
   toolchain の下で)。そのために converter・依存ツール・出力順序・
   float 直列化・GLB JSON key 順・image encoder・環境を pin する。
   「バイト決定性を強制しない」は**廃止**(この gate を採用しない
   output があれば、field 単位の semantic compare を個別定義)
3. **NORMALIZATION**: §2-2 の全 metadata key/型の固定
4. **BINDING**: §1-1-5 の mapping を import が安定出力し、runtime が
   消費。collision/missing/duplicate/fragment は名前入り hard error
5. **SAFETY**: §2-2 の USDZ 規約

## 4. WP 一覧(依存順)

| WP | 内容 | 依存 |
|----|------|------|
| **M-PBR0a** | representation/routing ABI(§1-1) | M3a(済) |
| **M-PBR0b** | openpbr surface + 写像表(§1-2) | M-PBR0a |
| **U-USD0a** | ツール選定 spike(§2-1) | なし(並行可) |
| **U-USD0b** | コンポジション/静的ジオメトリ(§2-2) | U-USD0a |
| **U-USD0c** | マテリアル/binding(§2-3) | U-USD0b・M-PBR0b |
| U-USD1a/1b・U-USD2a/2b | skel/camera・xform anim/instancer(§2-4) | U-USD0b |
| (即時小修正) | manifest の `khronos.ktx2`/`pelican.material` 受理 | なし |

## 5. 未決事項

1. anisotropy(タンジェント要求)— v1 サブセット外のまま
2. material-level pipeline state 化(variant 三種の次)— 予約
3. OpenPBR → toon の縮退写像 — v2 課題として記録のみ
4. transmission/subsurface — forward 経路が入ってから再評価
