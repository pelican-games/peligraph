# 第5章 アセットパイプライン

対象: pelican2(2026-07-10 時点)/ このマニュアルはコードを正とする

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

- `models` 配列は必須、各要素は `name` と `path`(プロジェクトルート基準)の両方が必須です。※この JSON には `schema`/`version` エンベロープがありません(R10 規約の現行例外)。
- **起動時に全件即時ロード**されます(遅延ロードなし)。ファイルが無ければ解決後の絶対パス入りで fail-fast。
- glTF のロードでは、ノード階層の変換が**頂点に焼き込まれて平坦化**されます(モデル内シーングラフは保持されません)。マテリアルは pbrMetallicRoughness の 4 テクスチャ(baseColor / metallicRoughness / normal / emissive)として登録されます。
- ⚠️ **スケルタルアニメーション・モーフの再生は 📐未実装**です(JOINTS/WEIGHTS 属性は読み込むだけ)。これはエンジン最大の未設計領域として認識されています(E2)。動きが必要な場合は次節の transform_seq / VAT を使います。

### テクスチャ・画像

- glTF 内のテクスチャは自動ロードされます。
- 単体画像の現在の用途は UI オーバーレイ(`ui/ui_overlay.json` — [第7章](07_input_ui.md))です。PNG/JPG は stb_image、`.exr` は tinyexr(✅WP26・`PELICAN_WITH_EXR`)。
- EXR は**限定スコープ**: single-part・scanline・half/float のみ。multi-part / deep / tiled は名指しで拒否されます。
- KTX2 / Basis は 📐WP23 候補予約(未実装)。

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

⚠️ 現状の注意: Blender ブリッジ(`dcc/blender_pelican2_bridge/`)や Houdini アダプタ、変換ツール集 `pelican-import-tools` は**設計のみで、リポジトリに存在しません** 📐。エンジン側の受け口(headless 描画・SeqPlayer・VAT・import)はすべて完成しています。FBX→glb の標準変換レーンは「Blender headless(`blender --background --python`)」と決定済みです。

## 5.6 フラグメント参照とアセットコンテナ(🚧構文のみ実装・WP55)

1 ファイル(コンテナ)内のサブアセットを指す構文が [PF] v6.3 で凍結されています:

```
assets/models/city.glb#mesh/LampPost     … 短い一意名(糖衣)
assets/models/city.glb#node/Root/Arm/Cube … フルパス(正準形)
```

- 構文とバリデーション(`#` は 1 個・種別は英数字・**全数字の index 参照は構文レベルで拒否** — DCC の並び替えで壊れるため)は実装済みです。
- ⚠️ **実際の部分ロード(K1)は未実装**: フラグメント付き参照がローダーに到達すると「Unsupported asset fragment kind」で明確に失敗します。現時点でシーン等に書いても動きません。
- glTF シーン抽出(`--extract-scene`)、PSD レーン(psd-tools)、アトラスパック、import ルール表(`imports.rules.json`)はすべて 📐設計のみです([../design_asset_containers.md](../design_asset_containers.md))。

> **設計決定(参照 = 存在):** コンテナを丸ごと参照すればパック、フラグメントで参照すれば分解。有効/無効フラグは持たない。per-file サイドカー(.meta)は不採用で、import の意味論は中央のルール表に置く(works-on-my-machine の構造的封じ込め)。

## 5.7 バイナリアセットの管理

- エンジンリポジトリでは example のバイナリは git 管理外で、[projects/example/README.md](../../projects/example/README.md) の sha256 表が第一防衛線です(assets manifest = WP66 で置換予定 📐)。
- 置き場所を外部化したい場合は asset store([第3章](03_project_format.md) §3.4)を使います。
- `pelican_cli project init` が生成する `.gitattributes` は `*.glb -text` 等の改行変換防止(CRLF 事故防止)+ LFS 行のコメントアウトを含みます。

## 5.8 対応形式の一覧(実装状況付き)

| 形式 | 用途 | 状態 |
|---|---|---|
| glb / glTF / VRM | モデル(静的メッシュ+PBR) | ✅(スケルタル再生は 📐) |
| PNG / JPG | テクスチャ・UI 画像 | ✅ |
| EXR(限定スコープ) | HDR テクスチャ | ✅ WP26(`PELICAN_WITH_EXR`) |
| WAV | SE([第8章](08_gameplay.md)) | ✅ WP51(`PELICAN_WITH_AUDIO`) |
| pelican.transform_seq(JSONL) | TRS 焼き込みアニメ | ✅ WP17(`PELICAN_WITH_SEQPLAYER`) |
| pelican.vat(glb extras) | 頂点アニメ | ✅ WP20(`PELICAN_WITH_VAT`) |
| pelican.import(manifest) | 納品の検証・登録 | ✅ WP21 |
| GLSL / SPIR-V | シェーダ([第6章](06_rendering.md)) | ✅ |
| KTX2 / Basis | 圧縮テクスチャ | 📐 WP23 予約 |
| pelican.pointcache | パーティクル点群 | 📐 方向決定のみ |
| PSD(外部展開) | UI ソース | 📐 K3 設計のみ |
| FBX / USD / .hip | ソース層(エンジンは読まない) | — 外部変換のみ |

## 関連文書

- [../design_asset_format_policy.md](../design_asset_format_policy.md) — 二層モデルと形式ポリシー(状態列は一部古い。この章が現状)
- [../design_asset_containers.md](../design_asset_containers.md) — #フラグメント・コンテナ・PSD レーン
- [../external_tools_requirements.md](../external_tools_requirements.md) — 外部ツール契約 R1〜R10(座標系 R6、命名 R7、バージョニング R10)
- [../design_project_dcc_houdini.md](../design_project_dcc_houdini.md) — Houdini レーンと pelican.import
- [../dcc_integration_qa_2026-06-12.md](../dcc_integration_qa_2026-06-12.md) — **pelican.vat v1 仕様の正**(§6)
- [../design_project_vcs.md](../design_project_vcs.md) — asset store と assets manifest
- [第3章 プロジェクト形式](03_project_format.md) / [第4章 シーンと ECS](04_scene_ecs.md) / [第10章 ツールリファレンス](10_tools.md)
