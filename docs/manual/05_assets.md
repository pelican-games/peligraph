# 第5章 アセットパイプライン

対象: pelican2(2026-07-17 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- 二層モデル(ランタイム層/ソース層)— エンジンが読む形式と読まない形式の厳密な線引き
- モデル・テクスチャの追加方法と `asset_data.json`
- DCC(Blender / Houdini)連携 — glTF ハブ、`pelican.import` manifest、`pelican_cli import`
- 焼き込みアニメーション 2 形式: `pelican.transform_seq`(JSONL)と `pelican.vat`(GLB 内 VAT)
- `#` フラグメント参照・アセットコンテナの現状

## 5.1 二層モデル — この章で最も重要な原則

> **設計決定(二層モデル):** アセット形式は 2 層に分かれる。
> - **ランタイム層** = エンジンが直接読む**閉じた小集合**: glTF/glb/VRM、PNG/JPG、EXR(限定スコープ)、WAV、JSON/JSONL、SPIR-V/GLSL。この集合への追加は設計文書([../design_asset_format_policy.md](../design_asset_format_policy.md))の改訂が門番。
> - **ソース層** = FBX・USD・PSD・.hip(Houdini)など。**エンジンは 1 バイトも読まない**(FBX SDK・USD・Alembic をリンクしない — ライセンス・ビルド時間・保守のため)。外部ツールでランタイム層形式に変換し、`imports/` に納品物として着地させる。
>
> 変換は「明示的・決定的・ファイルとして残る」。Unity/Unreal 式の隠れインポートキャッシュは不採用。変換結果の出所は `pelican.import` manifest が記録する。

チュートリアル的な帰結: **FBX や PSD をプロジェクトの `assets/` に置かない**でください。置くのは変換済みの glb / PNG などだけです。

> **設計決定(交換ハブ = glTF 2.0):** ジオメトリ・スケルトン・アニメーションは glb に正規化する。独自フォーマットを外部ツールとの交換に使わない。座標系は**書き出し時点で glTF 規約**(右手系 +Y up・-Z forward、メートル、秒、クォータニオン xyzw)に統一され、変換は DCC 側エクスポータの責務(規約 R3/R6)。

## 5.2 モデルを追加する(基本フロー)

1. `.glb` / `.gltf` / `.vrm` を `assets/models/` に置く(VRM は glTF 拡張としてそのまま受理)。
2. `assets/asset_data.json` に登録する。実物([../../projects/example/assets/asset_data.json](../../projects/example/assets/asset_data.json) 全文):

```json
{
  "models": [
    { "name": "alicia",        "path": "assets/models/AliciaSolid.vrm" },
    { "name": "DamagedHelmet", "path": "assets/models/DamagedHelmet.glb" },
    { "name": "sponza",        "path": "assets/models/sponza.glb" },
    { "name": "sotai",         "path": "assets/models/Sotai_D.glb" },
    { "name": "character",     "path": "assets/models/character.glb" }
  ]
}
```

3. シーンから `{"name": "simplemodelview", "model": "sotai"}` のように **name で**参照する([第4章](04_scene_ecs.md))。

仕様上の注意:

- `models` 配列は必須、各要素は `name` と `path`(プロジェクトルート基準)の両方が必須です。※この JSON には `schema`/`version` エンベロープがありません(R10 規約の現行例外)。`path` には `#` フラグメント(§5.6)も書けます: `{"name":"selected","path":"assets/models/city.glb#mesh/LampPost"}`。任意キー `material_bindings`(`pelican.material_bindings` v1 への参照 — プリミティブ → マテリアルの whole-model 契約。✅WP116、fragment モデルには不可)も書けます。
- **起動時に全件ロード**されます(✅WP82 で CPU 側は並列化・GPU 登録は宣言順直列 = 決定的)。ファイルが無ければ解決後の絶対パス入りで fail-fast。
- 静的メッシュはノード階層の変換が**頂点に焼き込まれて平坦化**されます。マテリアルは pbrMetallicRoughness の 4 テクスチャ(baseColor / metallicRoughness / normal / emissive)として登録されます(色テクスチャは SRGB view — [第6章](06_rendering.md) §6.3)。
- **スケルタルアニメーションは ✅WP38 で実装済み**: glTF の `skins` + `animations` を読み、シーンの `animation` コンポーネント([第4章](04_scene_ecs.md))で再生します。skinned プリミティブはスケルトンを保持します(焼き込まない)。制限: 補間は LINEAR / STEP のみ(CUBICSPLINE はロードエラー)、joint 上限 128、クリップはモデルと同一 glb のみ。
- **モーフターゲットは ✅WP121 で描画対応**: `primitive.targets` の POSITION/NORMAL/TANGENT delta をデコードし、初期 weight(node > mesh > 0)で material / shadow / velocity の全経路に適用します(変形順は morph → skinning → model)。制限: sparse morph accessor 非対応(名指しエラー)、target 64/primitive・weight 256/instance、同一 primitive の **morph + VAT 併用は拒否**。**アニメクリップの `weights` チャネルは今もロードエラー**で、実行時に weight を動かす公開経路は VRM expression service([第8章](08_gameplay.md) §8.12)だけです。
- モデルファイル(.glb/.gltf/.vrm)は実行中のホットリロード対象です(✅WP110 — [第10章](10_tools.md) §10.5)。

### テクスチャ・画像と atlas(`textures` セクション ✅WP103)

`asset_data.json` には任意の `textures` 配列も書けます(スプライト・UI 用のテクスチャ/アトラス宣言):

```json
{
  "models": [],
  "textures": [
    { "name": "demo_atlas", "path": "assets/demo.atlas.json", "sampler": "nearest" }
  ]
}
```

- `name` / `path` 必須、`sampler` は `"nearest"` / `"linear"`(既定 linear)。**sampler はアセット宣言側が正本**(sprite/UI 側での上書きなし)。
- `path` は単体画像(PNG 等)か **`pelican.atlas` v1** の JSON。atlas の実物([../../projects/sprite_demo/assets/demo.atlas.json](../../projects/sprite_demo/assets/demo.atlas.json)):

```json
{ "schema": "pelican.atlas", "version": 1,
  "pages": [ { "image": "demo.png", "size": [16, 16] } ],
  "sprites": { "full":  { "page": 0, "rect": [0, 0, 16, 16] },
               "left":  { "page": 0, "rect": [0, 0, 8, 16] },
               "right": { "page": 0, "rect": [8, 0, 16, 16] } } }
```

`rect` は [left, top, right, bottom] px。個々のスプライトは `"demo_atlas#sprite/left"` の形で `sprite_view` / UI から参照します(この `#sprite/` は atlas 側の解釈で、glb フラグメントとは別系統)。

- 画像デコーダ: PNG/JPG = stb_image、`.exr` = tinyexr(✅WP26。single-part・scanline・half/float 限定)、**`.ktx2` = 自前パーサ(✅WP92)**。
- KTX2 の対応範囲(制限サブセット): **RGBA8 UNORM/SRGB・BC7・BC5** の 2D テクスチャ、全ミップ必須。supercompression(Basis/zstd)・cubemap/array は名指しで拒否。glb 内蔵の `KHR_texture_basisu` は対象外です。専用ビルドフラグはなく常時有効。

## 5.3 焼き込みアニメーション(1) — pelican.transform_seq(✅WP17)

剛体的なオブジェクト群の TRS アニメーションを JSONL で焼いたものです(Houdini RBD などの受け口)。実フィクスチャ(`test/fixtures/seqplayer_two_objects.jsonl` より):

```jsonl
{"schema": "pelican.transform_seq", "version": 1, "fps": 30.0, "objects": ["left", "right"]}
{"t": 0.0, "transforms": [{"pos": [-0.6,0.2,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}, {"pos": [0.6,0.2,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}]}
{"t": 0.033333, "hidden": [1], "transforms": [{"pos": [-0.4,0.4,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}, {"pos": [0.4,0.4,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}]}
```

- 1 行目はヘッダ(`schema` / `version: 1` / `fps` / `objects` すべて必須)。以降はフレーム行で、`transforms` の要素数はヘッダの `objects` と**厳密一致**。
- transform のキーは `pos` / **`rot`**(xyzw)/ `scale`。※rpc の `update_transforms` は `rotation` で、ファイル形式とライブプロトコルでキー名が異なります(意図的な別スキーマ扱い — 混同注意)。
- `hidden` は「そのフレームで消すオブジェクトの添字リスト」。scale=0 で消す方式は法線・カリング・物理との意味の混線を理由に**不採用**になった設計決定です。
- ⚠️ フレーム行の `t` は実装では読み捨てられ、サンプリングは `floor(time × fps)` のフレーム添字で行われます(等間隔前提・補間なし)。不等間隔の `t` を書いても無視されます。

再生(プレビュー):

```sh
pelican_player --headless --project <dir> --frames 3 --size 160x90 --fps 30 \
  --play-seq fixtures/two_objects.jsonl --seq-mesh builtin:sphere \
  --camera "0,1,3,0,0.5,0,40" --render-out out/%04d.png
```

全オブジェクトが `--seq-mesh` の共有メッシュ(既定は実行時生成の UV 球)で描かれます。`--seq-loop` でループ。

## 5.4 焼き込みアニメーション(2) — pelican.vat v1(✅WP20)

頂点単位のアニメーション(布・破壊・流体の表面など)を **glb 内の VAT(Vertex Animation Texture)** として焼く形式です。仕様の正は [../dcc_integration_qa_2026-06-12.md](../dcc_integration_qa_2026-06-12.md) §6。

メタデータは glb の `meshes[i].primitives[j].extras["pelican.vat"]` に置きます:

```json
{
  "schema": "pelican.vat", "version": 1, "generator": "...",
  "fps": 30.0, "frame_count": 60, "vertex_count": 1234,
  "bounds_min": [0,0,0], "bounds_max": [1,1,1],
  "loop": true, "position_view": 3, "normal_view": 4
}
```

- テクスチャの実体は **glb バッファ内の raw half-float bufferView**(`position_view` / 任意の `normal_view`)。EXR や KTX2 への依存を増やさないための決定です。
- 1 primitive = 1 クリップ。`vertex_count` は primitive の実頂点数と照合されます(上限 8192)。トポロジは TRIANGLES のみ。
- 再生はシェーダ(`engine://vat` の vertex shader)が `texelFetch` で前後 2 フレームを読んで**手動 lerp** します(ハードウェア線形フィルタは頂点方向に滲むため禁止)。
- VAT 付き glb は `--play-vat <path.glb>` でプレビューできるほか、**通常のモデルとして asset_data.json に登録しても動きます**(ローダーが primitive 単位で検出)。
- `PELICAN_WITH_VAT=OFF` ビルドでは VAT 付き glb 自体が名指しのエラーで拒否されます。

## 5.5 DCC 納品物の取り込み — pelican.import と `pelican_cli import`(✅WP21)

外部ツール(Houdini アダプタ・Blender スクリプト等)は、変換結果を `imports/<tool>/<納品名>/` に **manifest.json(`pelican.import` v1)付き**で書き出します。manifest の実例とスキーマは [第10章](10_tools.md) §10.4 を参照してください。要点:

- `outputs[]` の各ファイルに **sha256 必須**。`pelican_cli import` が全数照合し、不一致は即エラー。
- 出力の `schema` は `"gltf"` か `"pelican.transform_seq"` のみ。glb だけが `asset_data.json` に自動登録されます(transform_seq は検証のみ)。
- 実行は冪等(登録済みの name / path はスキップ)。

> **設計決定(manifest はエンジンが読まない):** エンジン本体(player)は manifest なしで動く。参照はあくまで scene / asset JSON が張る。manifest を読むのは周辺ツール(`pelican_cli` 等)だけ。同様に、`source.file`(元 .hip のパス等)は検証されない — ツール側マシンのパスでよく、「再現の手がかり」として記録される。

> **設計決定(プロデューサ検証):** エンジンは JSON Schema validator を持たない。書き出したツール自身が書き出し直後に検証する。エンジン側は fail-fast(必須欠落・型不一致は throw)+ tolerant reader(未知フィールドは無視)。

現状の注意: 変換ツール集 **pelican-import-tools は独立リポジトリとして実装済み**(✅WP81 = K3。Python 製 `psd_extract` + `atlas_pack`、出力は `pelican.atlas` + import manifest)ですが、本リポジトリには含まれません。Houdini アダプタも別リポジトリです。Blender ブリッジは依然 📐(FBX→glb の標準変換レーンは「Blender headless」と決定済み)。エンジン側の受け口(headless 描画・SeqPlayer・VAT・import・rules)はすべて完成しています。

**USD レーン(✅WP118/119/124)**: import-tools に production の `usd` レシピが入りました。USD/USDZ を「`model.glb` + `scene.json`(pelican.scene v1)+ `materials.json`(pelican.material v1 — OpenPBR values + routing)+ `material_bindings.json` + PNG 群 + `manifest.json`」の決定的 delivery に変換します(二回ビルドの hash 一致 gate 付き。toolchain は `usd-core==26.5` に pin)。**エンジンは USD を 1 byte も読みません**(二層モデル不変 — エンジン側にあるのは fixture / golden / manifest 受理のみで、`pelican_cli import --rules` のレシピ表に usd はありません)。UsdMtlx は Windows wheel 非同梱のため外部 .mtlx は評価されず、authored 値のみ解釈されます。

import manifest の受理拡張: outputs の schema に `pelican.material`(v1)が追加され、**`khronos.ktx2` 出力には `version: 2` が必須**になりました(欠落・不一致は expected/actual 付きの名指しエラー)。

## 5.6 フラグメント参照とアセットコンテナ(✅実ロード対応・WP55/77/79/84)

1 ファイル(コンテナ)内のサブアセットを指す構文が [PF] v6.3 で凍結され、✅WP77(K1)で**実際の部分ロード**が入りました:

```
assets/models/city.glb#mesh/LampPost      … 短い一意名(糖衣)
assets/models/city.glb#node/Root/Arm/Cube … フルパス(正準形)
```

- 構文規則: `#` は 1 個・種別は英数字・**全数字の index 参照は構文レベルで拒否**(DCC の並び替えで壊れるため)。短い一意名が曖昧な場合は候補全列挙のエラー。
- 対応種別は **`mesh` / `node` / `material` / `animation` の 4 種**(.glb/.gltf のみ)。未対応 kind は対応一覧付きの名指しエラー。依存するリソースだけを GPU 化する真の部分ロードです。
  - `#mesh/...` / `#node/...` → asset_data.json の `models[].path` に書いてモデル参照
  - `#animation/<クリップ名>` → シーンの `animation` コンポーネントの `clip`([第4章](04_scene_ecs.md))
  - ※ atlas の `#sprite/<名>` は別系統(§5.2)です
- フラグメント参照のホットリロードも ✅WP110 で対応済み。

### glTF シーン抽出(✅WP79 = K2)

```sh
pelican_cli import gltf --extract-scene assets/models/city.glb
```

glTF のノード階層を pelican.scene v1 として抽出します: メッシュノード → `simplemodelview` + `#node/<フルパス>` 参照、`KHR_lights_punctual` → `light`、カメラノード → `camera`、**ノード親子 → オブジェクトの `parent`**。ノード名は R7 識別子必須で、重複・循環は抽出時に検出されます。

### import ルール表(✅WP84 = K4)

```sh
pelican_cli import --rules imports.rules.json
```

glob → レシピの中央表(`pelican.import_rules` v1)で一括取り込みを宣言します:

```json
{ "schema": "pelican.import_rules", "version": 1,
  "rules": [ { "match": "ui/**/*.psd", "recipe": "atlas_pack", "options": { "padding": 0 } } ],
  "defaults": { ".psd": "psd_layers", ".png": null } }
```

- レシピは 3 種: `extract_scene`(devcli 内蔵)/ `psd_layers` / `atlas_pack`(外部 pelican-import-tools を起動)。優先は rules > defaults > builtin 既定(`.glb → extract_scene` 等)。`null` で既定を無効化。
- 未知キー・未知レシピ・`\` を含む glob はすべて名指しエラー。出力は `imports/` + `pelican.import` manifest に着地します。

> **設計決定(参照 = 存在):** コンテナを丸ごと参照すればパック、フラグメントで参照すれば分解。有効/無効フラグは持たない。per-file サイドカー(.meta)は不採用で、import の意味論は中央のルール表に置く(works-on-my-machine の構造的封じ込め)。

## 5.7 バイナリアセットの管理

- バイナリの sha256 台帳は **assets manifest(✅WP66)** です: `pelican_cli assets manifest / verify / status` で生成・照合し、起動時にも検証されます([第3章](03_project_format.md) §3.4)。example の README にあった手書き表はこれに置き換わりました。
- 置き場所を外部化したい場合は asset store([第3章](03_project_format.md) §3.4)を使います。
- `pelican_cli project init` が生成する `.gitattributes` は `*.glb -text` 等の改行変換防止(CRLF 事故防止)+ LFS 行のコメントアウトを含みます。

## 5.8 対応形式の一覧(実装状況付き)

| 形式 | 用途 | 状態 |
|---|---|---|
| glb / glTF / VRM | モデル(メッシュ+PBR+スキン+モーフ) | ✅(スケルタル ✅WP38・モーフ ✅WP121。VRM は decode/dump ✅WP111 + **表情/マテリアル色/textureTransform ✅WP123b + bone lookAt/firstPerson ✅WP134** — MToon シェーディング本体・springbone は 📐) |
| PNG / JPG | テクスチャ・UI 画像 | ✅ |
| EXR(限定スコープ) | HDR テクスチャ | ✅ WP26(`PELICAN_WITH_EXR`) |
| KTX2(制限サブセット) | 圧縮テクスチャ(RGBA8/BC7/BC5) | ✅ WP92 |
| WAV | SE([第8章](08_gameplay.md)) | ✅ WP51(`PELICAN_WITH_AUDIO`) |
| pelican.atlas | スプライト/UI のアトラス | ✅ WP103 |
| pelican.transform_seq(JSONL) | TRS 焼き込みアニメ | ✅ WP17(`PELICAN_WITH_SEQPLAYER`) |
| pelican.vat(glb extras) | 頂点アニメ | ✅ WP20(`PELICAN_WITH_VAT`) |
| pelican.anim_graph | アニメーショングラフ([第8章](08_gameplay.md)) | ✅ WP101 |
| pelican.import(manifest) | 納品の検証・登録 | ✅ WP21 |
| pelican.import_rules | 一括取り込みのルール表 | ✅ WP84 |
| pelican.assets(manifest) | バイナリの sha256 台帳 | ✅ WP66 |
| pelican.input_seq(JSONL) | 入力収録([第7章](07_input_ui.md)) | ✅ WP89 |
| pelican.settings / セーブ JSON | user:// 永続化([第8章](08_gameplay.md)) | ✅ WP65 |
| `.surface` / pelican.material | マテリアル([第6章](06_rendering.md)) | ✅ WP68〜78 |
| GLSL / SPIR-V | シェーダ([第6章](06_rendering.md)) | ✅ |
| pelican.pointcache | パーティクル点群 | 📐 方向決定のみ |
| PSD(外部展開) | UI ソース | ✅ K3(外部 pelican-import-tools が展開。エンジンは読まない) |
| USD / USDZ | ソース層(エンジンは読まない) | ✅ 外部 pelican-import-tools の `usd` レシピ(WP118/119/124 — §5.5)。UsdSkel / camera / anim は 📐U-USD1 以降 |
| FBX / .hip | ソース層(エンジンは読まない) | — 外部変換のみ |

## 関連文書

- [../design_asset_format_policy.md](../design_asset_format_policy.md) — 二層モデルと形式ポリシー(状態列は一部古い。この章が現状)
- [../design_asset_containers.md](../design_asset_containers.md) — #フラグメント・コンテナ・PSD レーン
- [../external_tools_requirements.md](../external_tools_requirements.md) — 外部ツール契約 R1〜R10(座標系 R6、命名 R7、バージョニング R10)
- [../design_project_dcc_houdini.md](../design_project_dcc_houdini.md) — Houdini レーンと pelican.import
- [../dcc_integration_qa_2026-06-12.md](../dcc_integration_qa_2026-06-12.md) — **pelican.vat v1 仕様の正**(§6)
- [../design_project_vcs.md](../design_project_vcs.md) — asset store と assets manifest
- [第3章 プロジェクト形式](03_project_format.md) / [第4章 シーンと ECS](04_scene_ecs.md) / [第10章 ツールリファレンス](10_tools.md)
