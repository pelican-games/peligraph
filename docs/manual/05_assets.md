# 第5章 アセットパイプライン

最終確認: 2026-10-10

## この章で学ぶこと

- 二層モデル(ランタイム層/ソース層)— エンジンが読む形式と読まない形式の厳密な線引き([用語集](14_glossary.md#二層モデル))
- モデル・テクスチャ・マテリアルの追加方法と `asset_data.json`、および**そこに登録できない形式**(`.vrma`)
- 起動時ロードの順序と失敗時の GPU 資源(壊れた rendering config ではモデルを upload しない)
- モデル材質の identity — レイトレの加速構造規則が見る値
- DCC(Blender / Houdini)連携 — glTF ハブ、`pelican.import` manifest、`pelican_cli import`
- 焼き込みアニメーション 2 形式: `pelican.transform_seq`(JSONL)と `pelican.vat`(GLB 内 VAT)
- `#` フラグメント参照・アセットコンテナの現状

## 5.1 二層モデル — この章で最も重要な原則

> **設計決定(二層モデル):** アセット形式は 2 層に分かれる。
> - **ランタイム層** = エンジンが直接読む**閉じた小集合**: glTF/glb/VRM、**.vrma(VRM Animation。glb コンテナだが別 alias ゲート — §5.2)**、PNG/JPG、EXR(限定スコープ)、WAV・Ogg Vorbis(音声。AU-2 で Ogg Vorbis を追加)、JSON/JSONL、SPIR-V/GLSL。この集合への追加は設計文書([../design_asset_format_policy.md](../design_asset_format_policy.md))の改訂が門番。
> - **ソース層** = FBX・USD・PSD・.hip(Houdini)など。**エンジンは 1 バイトも読まない**(FBX SDK・USD・Alembic をリンクしない — ライセンス・ビルド時間・保守のため)。外部ツールでランタイム層形式に変換し、`imports/` に納品物として着地させる。
>
> 変換は「明示的・決定的・ファイルとして残る」。Unity/Unreal 式の隠れインポートキャッシュは不採用。変換結果の出所は `pelican.import` manifest が記録する。

チュートリアル的な帰結: **FBX や PSD をプロジェクトの `assets/` に置かない**でください。置くのは変換済みの glb / PNG などだけです。

> **設計決定(交換ハブ = glTF 2.0):** ジオメトリ・スケルトン・アニメーションは glb に正規化する。独自フォーマットを外部ツールとの交換に使わない。座標系は**書き出し時点で glTF 規約**(右手系 +Y up・-Z forward、メートル、秒、クォータニオン xyzw)に統一され、変換は DCC 側エクスポータの責務(規約 R3/R6)。

## 5.2 モデルを追加する(基本フロー)

1. `.glb` / `.gltf` / `.vrm` を `assets/models/` に置く(VRM は glTF 拡張としてそのまま受理)。※`.vrma`(VRM Animation)はここに含まれません — `asset_data.json` に登録する経路がありません(後述の「`.vrma`」)。
2. `assets/asset_data.json` に登録する。実物([../../projects/example/assets/asset_data.json](../../projects/example/assets/asset_data.json) 全文。字下げは詰めています):

```json
{
  "schema": "pelican.asset_data",
  "version": 2,
  "models": [
    { "name": "DamagedHelmet", "path": "assets/models/DamagedHelmet.glb" },
    { "name": "sponza",        "path": "assets/models/sponza.glb" },
    { "name": "sotai",         "path": "assets/models/Sotai_D.glb" },
    { "name": "character",     "path": "assets/models/character.glb" }
  ]
}
```

   VRM も同じ形で登録します。実物([../../projects/vrm_xr_demo/assets/asset_data.json](../../projects/vrm_xr_demo/assets/asset_data.json) の `models`):
   `{"name": "vrm_xr_character", "path": "assets/vrm_xr_character.vrm"}`(置き場所は `assets/models/` でなくても、project 相対の path なら読めます)。

3. シーンから `{"name": "simplemodelview", "model": "sotai"}` のように **name で**参照する([第4章](04_scene_ecs.md))。

**動いている engine に登録する**: 上の 2 は手で書いて engine を起こし直す道です。engine が `--rpc` で動いているときは、
1 のファイルを project の中に置いた後、RPC `register_model {path, name?}` で `models[]` に `{name, path}` を足させ、その場で登録させられます
(`name` を省くとファイル名の stem。起動時と同じ読み込みの検査を通らないファイルは断られ、asset_data は変わりません)。
外すのは `unregister_model {name}` で、`models[]` から外しますが、ファイルは消しません。ファイルを project にコピーするのは
呼び手の役目です。

rig を持つ model(VRM の大半)を外したときと、登録できなかったときは、書いたうえで `restart_required` を返し、
engine を起こし直すと効きます。引数・拒否・live になる条件は [第13章](13_editor.md#project-の中の-model-ファイルを登録する外すmi-1) §13.5 の「project の中の model ファイルを登録する・外す」です。

登録した model は `list_assets` に出ます。`path` は asset_data に書いた project 相対の綴り、`resolved_path` は解決した絶対 path です。
Studio の画面からこの RPC を呼ぶ道は下の「Studio から持ち込む」です。

**Studio から持ち込む**: Studio で project を開いて engine が動いている間に、Outliner の右クリックか Scene の toolbar の
**Add Object > Model from file…** を押し、ファイルダイアログで `.glb` / `.vrm` を選びます。project の外のファイルは `assets/models/` にコピーされ
(同じ名前が既にあれば `<stem>_2`…)、上の 2 の登録(`register_model`)と 3 の object(原点に、ファイル名の stem を名前にした model を描く object)を
Studio がします。名前が既にある model と重なれば `<stem>_001`… で登録します。

いま選んでいる object の model を替えるときは、Inspector の
model 欄の右の **Import…** で同じようにファイルを選ぶと、その欄が登録した model になります。File > Save Scene で scene に残ります。

外の `.gltf` は隣の `.bin` と画像を読むので選んでも断られます。関連するファイルごと project のフォルダに置いてから選ぶと、その場所のまま登録されます。
engine が登録を書いたもののその場で使えなかったとき(`restart_required`)は、object は置かれず、Outliner の下に再起動の案内と **Restart engine** が出ます。

登録を外すのは Assets のドックのそのファイルの行の右クリックの **Remove from project…** で、ファイルは消えません。
詳しくは [第13章](13_editor.md) §13.1 の「Studio で外の model を持ち込む(✅MI-1s)」です。

仕様上の注意:

- `schema: "pelican.asset_data"` と `version: 2` は必須です。`2` 以外は `pelican.asset_data.version_unsupported@1` で拒否します(✅MG-2b。[assetdataformat.cpp](../../src/project/assetdataformat.cpp#L110))。MG-2b より前にあった version `1` は「`procedural_geometry` を受けない」ことだけが違う版で、今は読めません。手元に `1` のファイルがあれば `version` を `2` に書き換えるだけで読めます。旧い未版形式も受理しません。
- `models` 配列は必須、各要素は `name` と `path`(プロジェクトルート基準)の両方が必須です。`path` には `#` フラグメント(§5.6)も書けます: `{"name":"selected","path":"assets/models/city.glb#mesh/LampPost"}`。任意キー `material_bindings`(`pelican.material_bindings` v1 への参照 — プリミティブ → マテリアルの whole-model 契約。✅WP116、fragment モデルには不可)も書けます。
- **起動時に全件ロード**されます(✅WP82 で CPU 側は並列化・GPU 登録は宣言順直列 = 決定的)。ここでいう**宣言順**は上の `models` 配列に書いた順のことです。パース(CPU)は複数スレッドで走りますが、結果は添字順の配列に置かれ、GPU への登録は全ジョブの合流後にこの順で 1 件ずつ行われます([parallel_prepare.hpp](../../src/core/parallel_prepare.hpp#L20))。スレッドの完了順が実行ごとに揺れても GPU に載る順序は変わらない、というのが「決定的」の中身です([用語集](14_glossary.md#決定的--決定性deterministic))。ファイルが無ければ解決後の絶対パス入りで fail-fast。
- **rendering config が壊れているとモデルは 1 件も upload されません(✅WP380a-fix)**: 起動時、レンダーグラフの検証と publish は scene の prepare より**前**に走ります([pelican_core.cpp](../../src/core/userpublic/pelican_core.cpp#L168))。マテリアル経路の誤りのような config の失敗が常に最初の起動エラーになり、頂点プールを消費しません。
- **失敗した load は GPU 資源を返します**: ロード途中で例外が出ると、その候補モデル(と登録済みモデル)の index / vertex / skinned vertex / morph delta / material / texture が解放されます([model.cpp](../../src/core/asset/model.cpp#L615))。**失敗した候補を warm cache として残さない**ので、同じプロセスで再試行しても頂点プールの使用量は増えません。解放そのものに失敗した分は固定容量 8 の報告([modelresourcerelease.hpp](../../src/core/model/modelresourcerelease.hpp#L36))へ畳まれ、scene load 失敗の結果に `release_errors`(stage 名の列。溢れた分は `overflow:N`)として現れます([sceneload.hpp](../../src/core/loader/sceneload.hpp#L142))。呼び元へ返せないデストラクタ経路の分は `ReleaseErrorSink` の累積カウンタに積まれます。
- 静的メッシュはノード階層の変換が**頂点に焼き込まれて平坦化**されます。glTF は「親ノードの行列 × 子ノードの行列 × …」の木で頂点位置を決めますが、ロード時にその積を頂点座標そのものへ掛けてしまい、ランタイムには親子関係のない 1 枚の頂点列だけが残る、という意味です(親を動かして子が付いてくる、という表現はできません。動かしたい単位はシーン側のオブジェクトに分けます)。マテリアルは 5 テクスチャ(baseColor / metallicRoughness / normal / emissive / occlusion)として登録されます。occlusion は metallicRoughness の R から推測せず、glTF の独立した `occlusionTexture` を使います(色テクスチャは SRGB view、metallicRoughness / normal / occlusion は UNORM view — [第6章](06_rendering.md) §6.3)。
- **スケルタルアニメーションは ✅WP38 で実装済み**: glTF の `skins` + `animations` を読み、シーンの `animation` コンポーネント([第4章](04_scene_ecs.md))で再生します。skinned プリミティブはスケルトンを保持します(焼き込まない)。制限: 補間は LINEAR / STEP のみ(CUBICSPLINE はロードエラー)、joint 上限 128、クリップはモデルと同一 glb のみ。※この 3 つの制限は **glb 内蔵クリップ + `animation` コンポーネントの経路に限った話**です。`.vrma` は別経路で、CUBICSPLINE も受理します(後述)。
- **モーフターゲットは ✅WP121 で描画対応**: `primitive.targets` の POSITION/NORMAL/TANGENT delta をデコードし、初期 weight(node > mesh > 0)で material / shadow / velocity の全経路に適用します(変形順は morph → skinning → model)。制限: sparse morph accessor 非対応(名指しエラー。glTF の **sparse accessor** は「既定値からの差分だけを (添字, 値) の対で持つ」省サイズ表現で、通常の accessor が全要素を並べるのと形式が違います — [ソースコード読解ガイド §10.2](../source-code-guide/10_background_knowledge.md#sparse-accessor))、target 64/primitive・weight 256/instance、同一 primitive の **morph + VAT 併用は拒否**。**アニメクリップの `weights` チャネルは今もロードエラー**で、実行時に weight を動かす公開経路は VRM expression service([第8章](08_gameplay.md) §8.13)だけです。
  - morph delta の上限は全 model 共有で524,288 records(primitive ごとの頂点数 × target 数)、target は64 / primitive、weight は256 / instanceです。上限を超えると起動を止め、問題の一覧には載りません。頂点 metadata の拡張は[読解ガイド](../source-code-guide/03_project_and_loading.md)を参照してください。
- モデルファイル(.glb/.gltf/.vrm)は実行中のホットリロード対象です(✅WP110 — [第10章](10_tools.md) §10.5)。
- **リロードの世代はアセット単位です(✅WP147)**: スケルタルモデルを差し替えると、**そのモデルのアセットだけ**世代が 1 進みます。**animation registry** はスケルタルモデル 1 件ごとに rig・skin binding・クリップのハンドルを保持するプロセス内の表です([`AnimationAssetRegistry`](../../src/core/animation/animationjobs.hpp#L59))。以前は 1 件でもスケルタルモデルが含まれるとこの表を全体ごと破棄していましたが、現在は**無関係なモデルに付いた評価器は止まりません**。差し替えられた側の rig / レイアウト / クリップ / カーソル / ポーズの旧ハンドルは `stale_generation` になります。宣言的な `animation` コンポーネント([第4章](04_scene_ecs.md))は独自レジストリを持つため自動で復帰しますが、`pelican.anim_graph` の評価器は `EvaluatorV1::rebind()` の明示呼び出しが必要です([第8章](08_gameplay.md) §8.13)。

### テクスチャ・画像と atlas(`textures` セクション ✅WP103)

`asset_data.json` には任意の `textures` 配列も書けます(スプライト・UI 用のテクスチャ/アトラス宣言):

```json
{
  "schema": "pelican.asset_data",
  "version": 2,
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
- KTX2 の対応範囲(制限サブセット): **RGBA8 UNORM/SRGB・BC7・BC5** の 2D テクスチャ。supercompression(Basis/zstd)・cubemap/array は名指しで拒否。glb 内蔵の `KHR_texture_basisu` は対象外です。専用ビルドフラグはなく常時有効。
  - 非圧縮の float の 6 つも読みます: `R16_SFLOAT` / `R16G16_SFLOAT` / `R16G16B16A16_SFLOAT` / `R32_SFLOAT` / `R32G32_SFLOAT` / `R32G32B32A32_SFLOAT`(VkFormat の番号は 76 / 83 / 97 / 100 / 103 / 109。[KTX2 の format の表](../../src/core/loader/ktx2.cpp#L71))。KTX2 の `typeSize` は 16 bit の format で 2、32 bit で 4 でなければ断ります。
  - **mip は1段か全段**です。RGBA8 / BC も1段を受けます。途中までの段は `incomplete mip chain: expected 1 or <全段の数> levels ...` で拒否します([検査](../../src/core/loader/ktx2.cpp#L260))。
  - KTX2 は format で色空間(sRGB か UNORM か)を宣言する扱いで、PNG / JPG は宣言しない扱いです(`LoadedImage::color_space_declared`)。これを使うのは graph の `render.image` の `initial` の照合だけです([第6章](06_rendering.md) §6.18「persistent の image を画像資産から始める」)。
  - float の KTX2 を作る道具として `pelican_cli image ktx2`(raw の bytes か `.hdr` から書く。[第10章](10_tools.md) §10.4)と、engine の関数 [`writeKtx2()`](../../src/core/loader/ktx2.cpp#L393) があります。
  - float の KTX2 を IA-1 の test が通しているのは、`render.image` の `initial` と、読み込み・書き出しの道だけです。material の texture として使えるかは確かめていません(未確認)。

### プロジェクトマテリアル(`materials` セクション ✅WP240c)

`pelican.material` v1 文書は `asset_data.json` の任意の `materials` 配列で索引します。索引要素は
`path` だけを持ち、名前は material 文書内の `materials[].name` を正本とします。

```json
{
  "schema": "pelican.asset_data",
  "version": 1,
  "models": [
    {
      "name": "helmet",
      "path": "assets/models/helmet.glb",
      "material_bindings": "assets/material_bindings/helmet.json"
    }
  ],
  "materials": [
    { "path": "materials/helmet.material.json" }
  ]
}
```

- 起動時に `.surface` を解決し、`parseSurfaceFormat` → material lowering → shader compile →
  GPU material 登録まで実行します。`engine://textures/{white,black,flat_normal,flat_gray}` と
  `project://*` の texture 参照は常設 resolver が解決します。
- `pelican.material_bindings` v1 の `binding.material` は下の「material の割り当て」の参照の形で、
  project material か、その model の glTF の material を指します。project 文書間で material 名が重複した場合は名指しで失敗します。
- **engine の基本形**(GP-5): `engine://primitives/{cube,plane,sphere}` は asset_data に登録しなくても model の参照として使えます(`simplemodelview.model` にそのまま書く)。形は engine がコードで生成し(cube は一辺 1、plane は XZ の 1×1 で +Y 向き、sphere は半径 0.5・32×16 分割)、既定の material は灰色の OpenPBR(`pelican.primitive.default`)です。material を差し替えるときは asset_data に `{"name": …, "path": "engine://primitives/cube", "material_bindings": …}` と宣言し、上の `pelican.material_bindings` の `model` に同じ参照を書きます。未知の名前は `model.primitive_unknown@1`。Studio では、この宣言を手で書かずに、Inspector の **Material** の combo が engine の `bind_material`(下の「RPC で書かせる」)で書きます([第13章](13_editor.md) §13.1 の「Studio の Materials のドック」)。
- `binding.routing` は実行時にも照合されます。OpenPBR の
  `{opaque,mask,blend}_{single,double}` と一致しない binding は拒否されます。
- material values と参照 texture は既存のファイル監視経路へ登録され、実行中の
  hot reload 対象です。ファイル監視による変更範囲と描画上の意味は[第6章の material](06_rendering.md#67-マテリアルm1m35--wp586870767883)を参照してください。
- **RPC で書かせる**: material を作る・直す・割り当てるときは、Studio の Materials と Inspector の Material 行を使います。引数・戻り値・拒否・保存ファイルは[第13章の material RPC](13_editor.md#material-文書を読み書きする)を参照してください。

#### material の割り当て —— 道・参照の形・skin 付き部品・VAT・model の差し替え

**3 つの道**: model の部品(mesh / primitive)の material は次のどれかで替えます。

| 道 | 書く場所 | 効く範囲 |
|---|---|---|
| bindings | asset_data の model の `material_bindings` が指す `pelican.material_bindings` v1 | その model を描く全 object。文書は model の全部品を覆います(fragment の model には書けません) |
| scene の上書き | scene の `simplemodelview.materials`([第4章](04_scene_ecs.md)) | その object だけ。部品を指す行と、全部品の行があります |
| RPC | `bind_material`(bindings を書く)/ `set_object_material`(object の行を書く) | 上の 2 つのどちらか。Studio の Materials と Inspector の Material 行がこれを送ります([第15章](15_studio.md) §15.4) |

**参照の形(✅NM-1)**: bindings と scene の上書きの `material` は、文字列の形で出どころが決まります([`resolveModelMaterial()`](../../src/core/model/modeltemplate.cpp#L73))。

| 形 | 指すもの |
|---|---|
| `#material/` を含まない(例 `paint`) | project material の名前。project を先に引き、project に無い名前だけ下の「名前だけの古い行の救済」で glTF の material を探します |
| `<model の path>#material/<名前>@<index>`(例 `assets/models/hero.glb#material/Material.001@2`) | その model の glTF の material。`<index>` は glTF の `materials` 配列の添字で、これが鍵です。名前は照合用です |

- **参照の形になるのは、file から読んだ model の glTF の material だけです**。engine の基本形(`engine://primitives/{cube,plane,sphere}`)の material `pelican.primitive.default` と、engine の組込の material(`pelican.builtin.*`。material を持たない glTF の部品などに使う)は、bindings でも scene の上書きでも RPC の答えでも裸の名前のままです([判定する所](../../src/core/model/modeltemplate.cpp#L54))。誤って参照の形(`engine://primitives/cube#material/pelican.primitive.default@0`)で書かれた行も、裸の名前に直して読みます。
- path は asset_data の `path` と同じ project 相対の綴りです。model の `path` が fragment 付き(`city.glb#mesh/LampPost`)なら、`#` より前の file に `#material/…` を付けます。その model 自身の file 以外を指すと `material reference must name the model's own asset file` で断られます。
- **名前と index が食い違う**(glb を書き出し直して material の名前が変わった)ときは index の material を使い、log に `stale material reference '<参照>': current name is '<今の名前>' (index wins)`、Problems に `material.reference_stale@1`(warning。`fields` は `model` と、行ごとの `row`・`written_name`・`current_name` など)が出ます([`publishBindingProblems()`](../../src/core/asset/model.cpp#L230))。
- **同じ名前の project material と glTF の material があっても読み込みは止まりません**(NM-1 の前は名前の衝突として断っていました)。`#material/` の無い名前は project の方を指します。
- **bindings の `material` は空でなければどんな文字列でも構いません**(✅MB-1。`Material.001`・`Body (skin)`・日本語の名前)。識別子に限られるのは project material の名前(自分の `.material.json` の側)だけです。
- **名前だけの古い行の救済**: project に無い名前で、その model の glTF に同じ名前の material が **1 つだけ**あれば、それを使います(log は INFO の `legacy material '<名前>' resolved as '<参照>'`)。**2 つ以上**あれば `Ambiguous GLB material fragment '<名前>'; use @<index> to select it (candidates: 0, 2)` の文で、bindings ではその部品は model 自身の material で描き、Problems の `material.binding_unresolved@1` にこの文が入ります。scene の上書きの行は warning を出して model の material で描きます。
- **書き手は 3 つ組で書きます**: `bind_material` / `set_object_material`(と Model default への戻し)は file から読んだ model の glTF の material をいつも `…#material/<名前>@<index>` の形で書き(engine の基本形と組込の material は上のとおり裸の名前)、触らない部品の行(名前だけの古い行を含む)もこの形に直します。`get_material_bindings` / `get_object_materials` もこの形で返します。Studio は名前で見せ、同じ名前が複数あるときだけ `名前 (index)` と見せます([第15章](15_studio.md) §15.4)。
- `@<index>` は `#mesh/` `#node/` `#animation/` の fragment 参照でも使えます(§5.6)。

**skin 付きの部品(✅SK-1)**: skin を持つかどうかは material ではなく**部品の性質**です。project material(自作の `.surface` を含む)を skin 付きの部品に当てられ、3 つの道のどれでも同じに描けます。

- project material は登録のとき、同じ surface を `PELICAN_SKINNED` 付きでもう 1 回 compile し、skin 付きの部品を描くときの頂点 program にします([2 回目の compile](../../src/core/material/projectmaterialasset.cpp#L145))。glTF の PBR material と engine の組込 material は engine の `skinned.vert` を持ちます。どの部品を skin の頂点で描くかは、描くときに部品の印で決まります。
- `PELICAN_SKINNED` は engine が付ける define なので、material の `defines` に書くと `material defines cannot override the compiler-owned physical shader ABI` で断られます([compiler が持つ define](../../src/core/shader/shaderlibrary.cpp#L167))。
- 1 つの glTF の material を skin 付きと skin なしの部品が使っていても、material の登録は 1 つです(前は skin 付きの部品のために material の「双子」を作り、material の無い skin 付きの部品には組込の `pelican.builtin.skinned_fallback` を使っていました。どちらも無くなりました)。
- 逆向き、VAT の material を skin 付きの部品に当てると、VAT の material は skin 用の頂点 program を持たないので、draw item を作る段で `material '<名前>' has no skinned vertex pipeline for mesh <m> primitive <p>` で止まります([`makeDrawItemSnapshot()`](../../src/core/renderer/polygoninstancecontainer.cpp#L370))。
- 描き方の差は[第6章](06_rendering.md) §6.7 の「pipeline は頂点の mode ごと」です。

**VAT で焼いた部品に VAT でない material は当てられません(✅SK-1)**: VAT の部品は texture と bounds を自分の material に焼いているので、VAT を持たない material は当てられません。bindings で当てるとその model の読み込みが `material bindings for model '<model>' mesh <m> primitive <p> material '<名前>': VAT primitives keep their baked material` で断られ([bindings の拒否](../../src/core/model/modeltemplate.cpp#L160))、scene の上書きの行は warning を出して元の material のまま描き([scene の行](../../src/core/renderer/polygoninstancecontainer.cpp#L456))、`bind_material` は VAT の部品を含む指定を、当てる material が VAT のものかどうかに関係なく `material.binding_invalid@1`(reason `VAT primitives keep their baked material`)で断ります(部品を指定しない model 全体への指定も、VAT の部品が 1 つでもあれば断られます。[RPC の拒否](../../src/core/communication/materialdocumentrpc.cpp#L1754))。部品ごとに VAT を持たせる形は v1 の予定です。

**model を差し替えた後の行(✅MR-1 / MO-1)**: glb を差し替えて(ファイルの hot reload など)部品が減ったり変わったりしても、起動と reload は止まりません。

- **bindings**: 無くなった部品を指す行は無視し、残っている有効な行はそのまま当てます。bindings に行の無い部品と、名前が解けない material を指す部品は model 自身の material で描きます。log に行ごとの理由(`model '<model>': material bindings row <n>: …`)が出て、部品の集合が合わない model ごとに Problems の `model.bindings_stale@1`(warning。`fields.rows` に 0 始まりの行・mesh・primitive・理由)が 1 件、名前が解けない分は `material.binding_unresolved@1` に行の理由が入ります([`publishBindingProblems()`](../../src/core/asset/model.cpp#L230))。元の glb に戻すか bindings を直すと消えます。
  - この model に `bind_material` を送ると、engine は消えた行を捨て、行の無い部品を足し、解けない行を model の material に戻して文書を書き直します(同じ material を選び直しても書き直します)。
- **scene の上書きの行**: 無くなった部品を指す行は、material の名前を見る前に無視し、log に `object <id>: material override (mesh <m>, primitive <p>) with material '<名前>' refers to a missing primitive in model '<model>'; ignoring the row` が出ます([行を無視する所](../../src/core/renderer/polygoninstancecontainer.cpp#L484))。Problems には `material.object_primitive_missing@1`(warning。`fields` は object・name・scene_id・model・material・primitive)が 1 件出ます([Problems の行](../../src/core/material/projectmaterialasset.cpp#L493))。行は文書から消さないので、model を戻すとまた当たり、Problems も消えます。Studio ではこの行を押すとその object の Inspector に飛びます。

### モデル材質の identity — レイトレの AS 規則が見る値(✅WP383)

レイトレーシングを使うプロジェクトでは、ray layout の `acceleration_structure.rules[]` が **material identity** で描画行を選びます([第6章](06_rendering.md) §6.15)。その identity がモデル側のどこから決まるかだけ、ここに書きます。

identity は下の 3 種のうち**ちょうど 1 つ**の形を取ります(「直和」= どれか 1 つで、2 つを同時に持つことも、どれでもないこともない)。gate(規則を要求するか)と lookup(どの規則に当たるか)は**同じ値**を見ます([raygrouplayout.cpp](../../src/core/shader/raygrouplayout.cpp#L62)):

| identity | 成立条件 | 由来 |
|---|---|---|
| `Document{document, material_name?}` | その行に **project material 文書の参照がある**(名前は任意) | 上記 `materials` セクション経由で解決された material 文書 |
| `Asset{asset, source_material_index}` | document が無く、asset 参照と `source_material_index` の両方がある | `asset_data.json` の `path` から作った**プロジェクト相対の正規キー**(fragment 付きなら `#kind/path` 込み)と、glTF の material 添字 |
| `Unavailable` | どちらも成立しない | material 無しの glTF プリミティブなど |

- **document の存在が勝ちます。** 単一 material の `.mat.json` のように `material_name` が無い document でも `Document` になり、`asset` 側へは落ちません。
- `asset` に入るのは `asset_data.json` の `name`(`"sponza"` 等)ではなく `path` 由来の参照です。マシン依存の絶対パスにならないよう、解決前の authored 参照を保ったまま運びます([model.cpp](../../src/core/asset/model.cpp#L364))。
- **`source_material_index` は optional のまま運ばれ、欠損は 0 に潰されません**([modeltemplate.hpp](../../src/core/model/modeltemplate.hpp#L151) → [accelerationstructure.hpp](../../src/core/vkcore/accelerationstructure.hpp#L50))。内部の「無し」番兵(`0xffffffff`)が identity まで届いた場合は `pelican.ray_material_index_out_of_range@1` で失敗します。
- material の付いていないモデルはレイトレの規則で選べません(`Unavailable`)。AS 規則を 1 つでも書いた v2 プロジェクトにそういう行が混ざると `pelican.ray_as_row_material_identity_unavailable@1` になります。規則を書かなければ identity は要求されません。

### `.vrma`(VRM Animation)— 実行時に読み込めるがプロジェクトから参照できない形式(🚧WP176/177/178)

`.vrma` は「ヒューマノイドのモーションだけ」を持つ glb コンテナ(`VRMC_vrm_animation` 1.0)です。ランタイム層の形式としてエンジンが直接デコードしますが、**プロジェクト形式から指す経路がまだありません**。この節は「置き場所と宣言」の話に限り、再生の詳細は [第8章](08_gameplay.md) §8.13 を参照してください。

> ⚠️ **`.vrma` は `asset_data.json` に登録できません。** `models[]` が受理するのは `.glb` / `.gltf` / `.vrm` だけで、`.vrma` を書くと `model reference requires a .glb/.gltf/.vrm file: <参照>` で拒否されます([model.cpp](../../src/core/asset/model.cpp))。`textures` にも該当しません。**現時点で `.vrma` をプロジェクトのデータとして参照する方法はありません**(CLI・ファイル監視も未接続)。

読み込めるのは**エンジン内部の C++ から import 境界を通した場合だけ**です([vrmadecoder.hpp](../../src/core/loader/vrmadecoder.hpp)):

- 入口は `decodeVrmaAnimation(bytes, container_name, options)` / `loadVrmaAnimation(path, options)`。
- `VrmaDecodeOptions` は `source_uri` / `import_profile` / `tool_version` の 3 つで、**`import_profile` と `tool_version` は空だと拒否**されます(DCC provenance が必須入力)。ここでの **provenance(来歴)** は「この成果物をどのツールのどの版が、どの変換設定で作ったか」の記録のことで、`import_profile` が変換設定の名前、`tool_version` が書き出したツールの版です。再現とデバッグの手がかりとして、値が空のまま素通りすることを許しません。`loadVrmaAnimation` は読んだ bytes をそのままハッシュしてデコードするため、provenance が中身と食い違いません。
- alias ゲート: コンテナ名の拡張子が `.vrma` でなければ `.vrma alias requires a container name ending in '.vrma'; got '<名前>'`。GLB magic 不在・GLB version 2 以外・宣言長の不一致もすべて名指しエラーです。
- 選ばれるのは**常に先頭のアニメーション**で、フラグメント識別子は `#animation/0` 固定です。
- **補間は LINEAR / STEP / CUBICSPLINE の 3 種すべてを受理**します(glb 内蔵クリップの経路と異なる点。この章の冒頭に書いた「補間は LINEAR / STEP のみ」は `.vrma` には当てはまりません)。

#### retarget profile v1(✅WP177)

**retarget(リターゲット)** は、あるリグ(骨の構成)向けに作られたモーションを、骨の長さや初期姿勢が違う別のリグへ移し替えることです。`.vrma` のクリップは「ヒューマノイドの標準骨名」で書かれているので、当てる先の VRM ごとに rest 姿勢の差を吸収する必要があります。**retarget profile** はその吸収のしかた(版・root motion の扱い・来歴)を 1 つの値にまとめたものです。

デコードしたクリップを VRM モデルに適用するには `retargetVrmaClip()`([vrmaretarget.hpp](../../src/core/animation/vrmaretarget.hpp))を通します。ここで生成される **versioned retarget profile v1** が「どう当てたか」の監査可能な成果物です。要点だけ:

- **profile は JSON ファイルではありません**。エンジンが生成する値(`VrmaRetargetProfile`)で、シリアライズ形式は定義されていません(`docs/schemas/` にもありません)。
- `version` は **1 のみ**、root motion policy は `preserve_hips_translation` のみ。**root motion は抽出しません**(hips の translation は通常の body チャネルのまま)。別値を渡すと `unsupported profile version <n>` / `profile version 1 does not support root-motion extraction` で拒否されます。
- 回転は rest 姿勢からの相対で再適用し、hips の translation は source / target の hips rest 高さの比でスケールします。
- 必須の humanoid 15 bone が target に無ければ **エラー**(`target humanoid is missing required bone '<名>'`)。optional bone や target に無い表情は **warning + そのトラックを skip**(`... optional bone is absent from the target rig` / `... matching target expression is absent`)で、ロードは止まりません。
- provenance に元クリップのメタデータと **source / target rig の SHA-256** を保持します。target 側のハッシュには表情インベントリも含まれるため、VRM の表情セットを変えるとハッシュが変わります。

再生は `pelican.anim_graph` の名前解決経由で、graph JSON には `{"clip": "<登録名>"}` と書くだけです(`.vrma` 専用のキーはありません)。ホットリロードの監視対象でもありません([第10章](10_tools.md) §10.5)。

## 5.3 焼き込みアニメーション(1) — pelican.transform_seq(✅WP17)

剛体的なオブジェクト群の TRS アニメーションを JSONL で焼いたものです(Houdini RBD などの受け口)。実フィクスチャ(`test/fixtures/seqplayer_two_objects.jsonl` より):

```jsonl
{"schema": "pelican.transform_seq", "version": 1, "fps": 30.0, "objects": ["left", "right"]}
{"t": 0.0, "transforms": [{"pos": [-0.6,0.2,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}, {"pos": [0.6,0.2,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}]}
{"t": 0.033333, "transforms": [{"pos": [-0.4,0.4,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}, {"pos": [0.4,0.4,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}]}
{"t": 0.066667, "hidden": [1], "transforms": [{"pos": [-0.2,0.6,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}, {"pos": [0.2,0.6,0], "rot": [0,0,0,1], "scale": [0.25,0.25,0.25]}]}
```

- 1 行目はヘッダ(`schema` / `version: 1` / `fps` / `objects` すべて必須)。以降はフレーム行で、`transforms` の要素数はヘッダの `objects` と**厳密一致**。
- transform のキーは `pos` / **`rot`**(xyzw)/ `scale`。※rpc の `update_transforms` は `rotation` で、ファイル形式とライブプロトコルでキー名が異なります(意図的な別スキーマ扱い — 混同注意)。
- `hidden` は「そのフレームで消すオブジェクトの添字リスト」。scale=0 で消す方式は法線・カリング・物理との意味の混線を理由に**不採用**になった設計決定です。
- ⚠️ フレーム行の `t` は実装では読み捨てられ、サンプリングは `floor(time × fps)` のフレーム添字で行われます(等間隔前提・補間なし)。形式がヘッダで `fps` を必須にしている以上、`n` 行目の時刻は `n / fps` に決まっており、行ごとの `t` はその再掲にすぎないためです。`t` は人間が読むときの目印として残っていますが、**書いた値と `fps` が食い違っても検出されません**(不等間隔の `t` を書いても無視されます)。

再生(プレビュー):

```sh
pelican_player --headless --project <dir> --frames 3 --size 160x90 --fps 30 \
  --play-seq fixtures/two_objects.jsonl --seq-mesh builtin:sphere \
  --camera "0,1,3,0,0.5,0,40" --render-out out/%04d.png
```

全オブジェクトが `--seq-mesh` の共有メッシュ(既定は実行時生成の UV 球)で描かれます。`--seq-loop` でループ。

## 5.4 焼き込みアニメーション(2) — pelican.vat v1(✅WP20)

頂点単位のアニメーション(布・破壊・流体の表面など)を **glb 内の VAT(Vertex Animation Texture)** として焼く形式です。形式の規定として最終的に参照すべき文書は [../dcc_integration_qa_2026-06-12.md](../dcc_integration_qa_2026-06-12.md) の §6 で、この節はそこからの要約です(食い違ったら §6 とコードが正)。

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
- 再生はシェーダ(埋め込みシェーダ `vat.vert.spv` の vertex shader)が `texelFetch` で前後 2 フレームを読んで**手動 lerp**(2 値を自分で線形補間)します。VAT のテクスチャは「横 = 頂点番号・縦 = フレーム番号」の並びなので、GPU の線形フィルタを有効にすると時間方向だけでなく**隣の頂点の値まで混ざります**(頂点方向に滲む)。したがってフィルタは nearest 相当の `texelFetch` に固定し、時間方向の混ぜ合わせだけをシェーダ側で書きます。
- VAT 付き glb は `--play-vat <path.glb>` でプレビューできるほか、**通常のモデルとして asset_data.json に登録しても動きます**(ローダーが primitive 単位で検出)。
- `PELICAN_WITH_VAT=OFF` ビルドでは VAT 付き glb 自体が名指しのエラーで拒否されます。
- VAT で焼いた部品の material は、VAT を持たない material には替えられません(bindings は断り、scene の上書きは元のまま描きます。`bind_material` は VAT の部品への指定をすべて断ります。§5.2 の「material の割り当て」)。

## 5.5 DCC 納品物の取り込み — pelican.import と `pelican_cli import`(✅WP21)

外部ツール(Houdini アダプタ・Blender スクリプト等)は、変換結果を `imports/<tool>/<納品名>/` に **manifest.json(`pelican.import` v1)付き**で書き出します。manifest の実例とスキーマは [第10章](10_tools.md) §10.4 を参照してください。要点:

- `outputs[]` の各ファイルに **sha256 必須**。`pelican_cli import` が全数照合し、不一致は即エラー。
- 出力の `schema` は 9 種のみ受理されます([importmanifest.cpp](../../src/project/importmanifest.cpp) の `validateOutputSchema`)。それ以外は `import manifest output schema is not supported` で拒否です。
  - `gltf` / `png` — `version` を**書いてはいけません**(書くとエラー)
  - `pelican.transform_seq` / `pelican.layout` / `pelican.atlas` / `pelican.material` / `pelican.audio_analysis_table`(✅AB-1。音の解析を焼いた表。§5.8 の「音の解析を焼いた表」)— `version: 1` が**必須**(欠落も `requires version 1` で拒否)
  - `pelican.scene` — `version: 2` が**必須**(✅MG-2b。エンジンが scene v2 しか読まないため、`1` を名乗る納品は `expected=2, actual=1` で拒否)
  - `khronos.ktx2` — `version: 2` が**必須**
- glb だけが `asset_data.json` に自動登録されます(それ以外の schema は検証のみ)。
- 実行は冪等(登録済みの name / path はスキップ)。

> **設計決定(manifest はエンジンが読まない):** エンジン本体(player)は manifest なしで動く。参照はあくまで scene / asset JSON が張る。manifest を読むのは周辺ツール(`pelican_cli` 等)だけ。同様に、`source.file`(元 .hip のパス等)は検証されない — ツール側マシンのパスでよく、「再現の手がかり」として記録される。

> **設計決定(プロデューサ検証):** エンジンは JSON Schema validator を持たない。検証の責任は**書き出した側(プロデューサ = 出力ツール自身)**にあり、書き出し直後に自分の出力を検証する。エンジン側は fail-fast(必須欠落・型不一致は throw)+ tolerant reader(未知フィールドは無視)。

現状の注意: 変換ツール集 **pelican-import-tools は独立リポジトリとして実装済み**(✅WP81 = K3。Python 製 `psd_extract` + `atlas_pack`、出力は `pelican.atlas` + import manifest)ですが、本リポジトリには含まれません。Houdini アダプタも別リポジトリです。Blender ブリッジは依然 📐(FBX→glb の標準変換レーンは「Blender headless」と決定済み)。エンジン側の受け口(headless 描画・SeqPlayer・VAT・import・rules)はすべて完成しています。

**USD レーン(✅WP118/119/124/240c)**: ここでいう**レーン**は「ある入力形式をランタイム層へ運ぶ、決まった経路一式(外部ツールのレシピ + 納品物の形 + manifest)」のことです。import-tools に production の `usd` レシピが入りました。USD/USDZ を「`model.glb` + `scene.json`(pelican.scene)+ `materials.json`(pelican.material v1 — OpenPBR values + routing)+ `material_bindings.json` + PNG 群 + `manifest.json`」の決定的 delivery に変換します(二回ビルドの hash 一致 gate 付き。toolchain は `usd-core==26.5` に pin)。

**エンジンは USD を 1 byte も読みません**(二層モデル不変)。delivery の `materials.json` を `asset_data.json` の `materials[]` で索引すれば、engine runtime が OpenPBR material と primitive binding をロードします。`pelican_cli import --rules` のレシピ表に usd はありません。UsdMtlx は Windows wheel 非同梱のため外部 .mtlx は評価されず、authored 値のみ解釈されます。

> ⚠ **MG-2b からエンジンは pelican.scene v2 しか読みません。** この repo の USD fixture(`test/fixtures/usd0b/` など)の manifest と `scene.json` は v2 に書き換えましたが、別 repo の import-tools の `usd` レシピが v2 を出すようになったかは未確認です(台帳の IMPORT-1)。v1 を名乗る delivery は `pelican_cli import` が `expected=2, actual=1` で断ります。

import manifest の受理拡張: outputs の schema に `pelican.material`(v1)が追加され、**`khronos.ktx2` 出力には `version: 2` が必須**になりました(欠落・不一致は expected/actual 付きの名指しエラー)。

## 5.6 フラグメント参照とアセットコンテナ(✅実ロード対応・WP55/77/79/84)

1 ファイル(コンテナ)内のサブアセットを指す構文が [PF] v6.3 で凍結され、✅WP77(K1)で**実際の部分ロード**が入りました:

| 参照 | 意味 |
|---|---|
| `assets/models/city.glb#mesh/LampPost` | 短い一意名(糖衣) |
| `assets/models/city.glb#node/Root/Arm/Cube` | フルパス(正準形) |

- 構文規則: `#` は 1 個・種別は英数字とアンダースコア(`[A-Za-z0-9_]`)・**全数字の index だけの参照は構文レベルで拒否**(DCC の並び替えで壊れるため)。短い一意名が曖昧な場合は候補全列挙のエラー。
- **名前に `@<index>` を足せます(✅NM-1)**: `city.glb#mesh/LampPost@3` のように、最後の `@` の後ろが 0 以上の整数なら、それがその種別の配列の添字で、選ぶ鍵になります(名前は照合用。違えば index の方を使い、警告します)。`@` の後ろが整数でなければ名前の一部です(`a@b` は名前)。同じ名前が複数あるのに `@` 無しで指すと `Ambiguous GLB <種別> fragment '<名前>'; use @<index> to select it (candidates: 0, 2)` で断られます([`resolveAssetFragment()`](../../src/project/projectpathresolver.cpp#L333))。glTF の material を bindings と scene で指す形 `<path>#material/<名前>@<index>` もこの文法です(§5.2)。
- 対応種別は **`mesh` / `node` / `material` / `animation` の 4 種**(.glb/.gltf のみ)。未対応 kind は対応一覧付きの名指しエラー。依存するリソースだけを GPU 化する真の部分ロードです。
  - `#mesh/...` / `#node/...` → asset_data.json の `models[].path` に書いてモデル参照
  - `#animation/<クリップ名>` → シーンの `animation` コンポーネントの `clip`([第4章](04_scene_ecs.md))
  - ※ atlas の `#sprite/<名>` は別系統(§5.2)です
- フラグメント参照のホットリロードも ✅WP110 で対応済み。

### glTF シーン抽出(✅WP79 = K2)

```sh
pelican_cli import gltf --extract-scene assets/models/city.glb
```

glTF のノード階層を pelican.scene v2 として抽出します(✅MG-2b で v1 から変更。[gltfsceneextract.cpp](../../src/devcli/gltfsceneextract.cpp)): メッシュノード → `simplemodelview` + `#node/<フルパス>` 参照、`KHR_lights_punctual` → `light`、カメラノード → `camera`、**ノード親子 → オブジェクトの `parent`**。ノード名は R7 識別子必須で、重複・循環は抽出時に検出されます。

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
- 起動時の照合の結果は engine に残り、RPC で読めて走らせ直せます(`get_status.assets_verification`・`get_assets_verification`・`verify_assets`・`update_assets_manifest`。[第10章](10_tools.md) §10.3 の「起動時の asset 検証の結果」)。照合するのは manifest を宣言した store だけで、Blank / Standard の雛形は store も manifest も宣言しません。動いている engine に `edit_project_settings` で store と manifest の宣言を足せます。manifest のファイルが無ければ engine が今のディスクから書き、照合を走らせ直します。照合が終わった通知は無いので、`get_status.assets_verification.state` を見ます。受ける変更と断る変更は [第3章](03_project_format.md) §3.4 の「起動中の engine に宣言を足す」です。
- 置き場所を外部化したい場合は asset store([第3章](03_project_format.md) §3.4)を使います。
- `pelican_cli project init` が生成する `.gitattributes` は `*.glb -text` 等の改行変換防止(CRLF 事故防止)+ LFS 行のコメントアウトを含みます。⚠️ 並ぶのは `*.glb` / `*.vrm` / `*.png` / `*.wav` / `*.spv` で、**`*.ogg` はまだありません**(AU-2 で Ogg Vorbis を足したときに雛形を直していない)。Ogg を置く project は `*.ogg -text` を自分で足してください([雛形](../../src/project/projecttemplate.cpp#L293))。

## 5.8 対応形式の一覧(実装状況付き)

| 形式 | 用途 | 状態 |
|---|---|---|
| glb / glTF / VRM | モデル(メッシュ+PBR+スキン+モーフ) | ✅(スケルタル ✅WP38・モーフ ✅WP121。VRM は decode/dump ✅WP111 + **表情/マテリアル色/textureTransform ✅WP123b + bone lookAt/firstPerson ✅WP134** — MToon シェーディング本体・springbone は 📐) |
| `.vrma`(VRM Animation / glb alias) | ヒューマノイドのモーション単体コンテナ | 🚧 decode + typed チャネル ✅WP176 / retarget profile v1 ✅WP177 / AnimationSource + anim_graph 統合 ✅WP178。**ただし `asset_data.json`・CLI・ファイル監視からは未接続**(エンジン内部の C++ API のみ — §5.2) |
| PNG / JPG | テクスチャ・UI 画像 | ✅ |
| EXR(限定スコープ) | HDR テクスチャ | ✅ WP26(`PELICAN_WITH_EXR`) |
| KTX2(制限サブセット) | 圧縮テクスチャ(RGBA8/BC7/BC5)、float のデータ(R16 / R32 SFLOAT の 1・2・4 成分。graph の image の初期内容) | ✅ WP92 / IA-1 |
| WAV(PCM / IEEE float) | 音(SE・BGM。[第8章](08_gameplay.md) §8.11) | ✅ WP51(`PELICAN_WITH_AUDIO`) |
| Ogg Vorbis | 音(SE・BGM。[第8章](08_gameplay.md) §8.11) | ✅ AU-2(`PELICAN_WITH_AUDIO`)。Opus / FLAC を入れた Ogg は不可。判定は下の「音声形式」 |
| pelican.atlas | スプライト/UI のアトラス | ✅ WP103 |
| pelican.transform_seq(JSONL) | TRS 焼き込みアニメ | ✅ WP17(`PELICAN_WITH_SEQPLAYER`) |
| pelican.vat(glb extras) | 頂点アニメ | ✅ WP20(`PELICAN_WITH_VAT`) |
| pelican.anim_graph | アニメーショングラフ([第8章](08_gameplay.md)) | ✅ WP101 |
| pelican.import(manifest) | 納品の検証・登録 | ✅ WP21 |
| 音の解析の表(R32_SFLOAT の KTX2 + `<表>.import.json`) | 音から焼いた rms / peak / 帯域の値の表。`value.sample_table` と `pelican_sample_table.glsl` で読む | ✅ AB-1(焼くのは `PELICAN_WITH_AUDIO` の build だけ。下の「音の解析を焼いた表」) |
| pelican.import_rules | 一括取り込みのルール表 | ✅ WP84 |
| pelican.assets(manifest) | バイナリの sha256 台帳 | ✅ WP66 |
| pelican.input_seq(JSONL) | 入力収録([第7章](07_input_ui.md)) | ✅ WP89 |
| pelican.settings / セーブ JSON | user:// 永続化([第8章](08_gameplay.md)) | ✅ WP65 |
| `.surface` / pelican.material | マテリアル([第6章](06_rendering.md)) | ✅ WP58/68/70/76/78(+M3.5 = WP83) |
| GLSL / SPIR-V | シェーダ([第6章](06_rendering.md)) | ✅ |
| pelican.pointcache | パーティクル点群 | 📐 方向決定のみ |
| PSD(外部展開) | UI ソース | ✅ K3(外部 pelican-import-tools が展開。エンジンは読まない) |
| USD / USDZ | ソース層(エンジンは読まない) | ✅ 外部 pelican-import-tools の `usd` レシピ(WP118/119/124 — §5.5)。UsdSkel / camera / anim は 📐U-USD1 以降 |
| FBX / .hip | ソース層(エンジンは読まない) | — 外部変換のみ |

### 音声形式(✅AU-2)

エンジンが鳴らせる音は **WAV と Ogg Vorbis** の 2 つです(2026-09-24 利用者裁定。MP3 / FLAC は需要が出てから)。`playSound` でもグラフの `audio.playback` の `asset` でも同じ規則で、**拡張子と先頭 bytes が同じ形式を指すときだけ**受け付けます([`detectSoundFormat`](../../src/core/audio/sounddecode.cpp#L263)。✅AB-1 で decode は `audio.cpp` から `sounddecode.cpp` に移りました)。

| 形式 | 拡張子(大文字小文字は区別しない) | 先頭 bytes |
|---|---|---|
| WAV | `.wav` | `RIFF` …… `WAVE` |
| Ogg Vorbis | `.ogg` | `OggS` で、最初の packet が `\x01vorbis` |

- それ以外は decode の前に `audio.format_unsupported@1` `{asset, extension, signature, supported}` で拒否されます。拡張子と中身が食い違うもの(中身が WAV の `.ogg` など)や、Opus / FLAC を入れた `.ogg` もここで止まります。
- 10 秒を超える音は全体を decode せず streaming で鳴ります(第8章 §8.11「長い音の streaming」)。
- 方針の正は [資産形式の方針](../design_asset_format_policy.md) §1.1 です。

### 音の解析を焼いた表(✅AB-1)

音の大きさや帯域ごとの強さで絵を動かすときは、音を**先に解析して表に焼き**、その表を読みます。解析は取り込みのたびには走らず、明示の Bake(RPC `bake_audio_analysis` か `pelican_cli audio analyze`)だけが表を書きます。方針と設計は [design_audio.md](../design_audio.md) §6 です。

**焼く口は 2 つで、中身は同じ関数です**([`bakeAudioAnalysis()`](../../src/core/audio/audioanalysis.cpp#L186))。

- RPC `bake_audio_analysis {asset, out, settings?}`: render config の ticket で、worker の thread で焼きます([第13章](13_editor.md) §13.5「音の解析を焼く」)。
- CLI `pelican_cli audio analyze --asset <file> --out <file.ktx2> [...]`([第10章](10_tools.md) §10.4)。
- 音の decode は engine の音の再生と同じ関数の集まり([`decodeWholeSound()`](../../src/core/audio/sounddecode.cpp#L345))で、上の「音声形式」と同じ規則で WAV / Ogg Vorbis だけを受けます。10 秒を超える音も streaming にせず全体を decode します。

**settings**([`parseAudioAnalysisSettings()`](../../src/project/audioanalysisformat.cpp#L91))。どれも省けます。`settings` を省くか `null` なら全部が既定です。

| 鍵 | 既定 | 受ける値 |
|---|---|---|
| `features` | `["rms", "peak", "bands"]`(全部) | この 3 つの名前の、空でない配列(同じ名前を 2 度書くと断る) |
| `bands` | `8` | 整数 1〜128 |
| `hop_ms` | `10` | 数 1〜1000(標本の間隔) |
| `window_ms` | `hop_ms` の 2 倍 | 数 1〜4000(1 標本が見る幅) |
| `smoothing.attack_ms` / `smoothing.release_ms` | `0`(なし) | 数 0〜60000 |
| `channels` | `"mix"` | `"mix"` / `"left"` / `"right"` |

ほかの鍵・型の違い・範囲の外は `audio.analysis_settings_invalid@1` `{field, reason, value?}` で断られ、何も書きません(`field` は `smoothing.attack_ms` のように道筋で)。

**計算**([`analyzeAudio()`](../../src/core/audio/audioanalysis.cpp#L78))

- 標本 i は時刻 `i × hop` を中心にした `window_ms` の窓で、標本の数は `floor(frame の数 / hop) + 1` です。窓が音の外にはみ出した所は 0 として数えます。
- `channels` の `mix` はチャンネルの平均、`left` は 1 本目、`right` は 2 本目(モノラルなら 1 本目)です。
- `rms` と `peak` は窓の中の値そのものから取ります。
- 帯域は Hann 窓をかけた FFT(自前の radix-2。大きさは窓の幅以上の 2 の冪)で、帯 b は `[20 × (nyquist/20)^(b/N), 20 × (nyquist/20)^((b+1)/N))` Hz に入る bin の振幅(`2|X| / 窓の和`)の二乗和の平方根です(20 Hz から Nyquist までの対数間隔。最後の帯は Nyquist の bin まで)。コードの注記では、振幅 1 の正弦は rms 約 0.71、peak 1、自分の帯で約 1.2 です。
- `smoothing` は行ごとの 1 次の追従で、値が上がる時は `attack_ms`、下がる時は `release_ms` の時定数です(0 はその向きの追従なし)。
- 同じ bytes と同じ settings からは同じ file が書かれます(test で 2 回焼いて bytes を比べています)。

**表の形**(`R32_SFLOAT` の 2D の KTX2。mip 1 段)

- 幅は 4096 で固定です。行の並び(feature)は `rms`、`peak`、`band:0` 〜 `band:<N-1>` の順で、`features` で省いたものは入りません。
- 各 feature は `rows_per_feature = ceil(標本の数 / 4096)` 行を縦に使います。feature f の標本 i は texel `(i % 4096, f × rows_per_feature + i / 4096)` で、余りは 0 です。
- 例(test): 48 kHz・1 秒の WAV を既定の settings で焼くと、`width` 4096・`rows_per_feature` 1・`sample_count` 101・`hop_seconds` 0.01、`features` は `rms` / `peak` / `band:0` 〜 `band:7` の 10 行です。

**manifest**(表の隣の `<out>.import.json`。既存の `pelican.import` v1 の形。[`audioAnalysisManifest()`](../../src/project/audioanalysisformat.cpp#L230))。test の 1 回目の bake の形です(音と表が同じ dir):

```json
{"schema": "pelican.import", "version": 1,
 "tool": {"name": "pelican.audio_analysis", "version": "1"},
 "source": {"file": "tone.wav",
            "metadata": {"sha256": "<音の sha256>",
                         "settings": {"features": ["rms", "peak", "bands"], "bands": 8, "hop_ms": 10.0,
                                      "window_ms": 20.0, "smoothing": {"attack_ms": 0.0, "release_ms": 0.0},
                                      "channels": "mix"},
                         "settings_sha256": "<settings の sha256>",
                         "layout": {"width": 4096, "rows_per_feature": 1, "sample_count": 101,
                                    "hop_seconds": 0.01, "features": ["rms", "peak", "band:0", "...", "band:7"]}}},
 "outputs": [{"file": "tone.ktx2", "schema": "pelican.audio_analysis_table", "version": 1, "sha256": "<表の sha256>"}]}
```

- `source.file` は manifest の置き場所からの相対です。`settings` は既定を埋めた全部の鍵で、`settings_sha256` はその JSON の sha256 です。
- `created` は書きません。同じ bytes と settings からは同じ manifest になります。
- 表と manifest は、隣の一時 file に書いてから名前を付け替えます(コードの注記: 読む側が書きかけを見ないため)。
- **読む側が開いている間の置き換え**: 一時 file は `<out>.tmp` / `<out>.import.json.tmp` で、書いて閉じてから置き換えます([`writeFileReplacing()`](../../src/project/filereplace.cpp#L169))。Windows では、置き換えが共有違反・access denied・lock violation で失敗すると、50 ms から倍々に待って、待ちの合計 2 秒まで(最後にもう 1 回)やり直します([AB-2 のやり直しの間隔](../../src/project/filereplace.hpp#L31))。
  - 置き換えは POSIX の意味の rename(`FileRenameInfoEx`。使えない Windows・file system では `MoveFileExW`)です。delete も共有して開いている handle(test は file の見張りの開き方)なら待たずに置き換わり、開いている側は古い中身を読み続けます。`std::ifstream` のように delete を共有しない開き方では、閉じるまでやり直しで待ちます(test: 300 ms 後に閉じると committed)。
  - 2 秒の間ずっと開かれたままなら、下の表の `audio.analysis_failed@1` に `reason: "output_locked"` と `path` が付きます。一時 file は消え、元の表と manifest は書き換わりません(test)。
  - Windows 以外は rename を 1 回だけ試します(やり直しはしません)。
  - 読む側も開いている時間を短くしました。KTX2 の読み込み([`loadKtx2File()`](../../src/core/loader/imageloader.cpp#L102))は全部読んで閉じてから parse し、古さの判定([AB-2 で manifest を先に閉じる `readDerivedAssetStatus()`](../../src/project/audioanalysisformat.cpp#L250))は manifest を閉じてから元の音と出力を hash します。読む側の開き方(共有のモード)は変えていません。
  - test: `ab1_audio_analysis_test` の [「AB-2 bake_audio_analysis replaces a table a reader held open once it closes, and refuses with output_locked when it never does」](../../test/ab1_audio_analysis_test.cpp#L363)(非 GPU。閉じれば committed。ここから先は Windows だけ: 閉じなければ `output_locked` で 1.9 秒以上待ち、一時 file が残らず元が無傷、delete も共有する handle なら 1.5 秒未満で committed)。

**読む**: CPU は graph の op `value.sample_table {asset, row, time?}`、GPU は engine の include `pelican_sample_table.glsl` です([第6章](06_rendering.md) §6.18「焼いた音の解析の表を読む」)。

**古いかどうか**: RPC `list_derived_assets {}` と、`get_frame_plan` の `initial_asset` が、manifest を持つ file について `stale` と `stale_reasons` を返します([第13章](13_editor.md) §13.5)。理由は `source_missing` / `source_changed`(元の音が無い・sha256 が違う)、`output_missing` / `output_changed`(表が無い・sha256 が違う)、`settings_changed`(manifest の `settings` が `settings_sha256` と合わない)、`tool_version`(版が `"1"` でない)です([`readDerivedAssetStatus()`](../../src/project/audioanalysisformat.cpp#L250))。古くなっても自動では焼き直しません。

**拒否**(RPC では rejected、CLI では stderr に `<code> <fields の JSON>` と終了コード -1)

| code | いつ | fields |
|---|---|---|
| `audio.analysis_settings_invalid@1` | settings が読めない(RPC ではすぐ断る) | `{field, reason, value?}` |
| `audio.format_unsupported@1` | 音が WAV / Ogg Vorbis でない(上の「音声形式」と同じ判定) | AU-2 と同じ(RPC では `asset` / `out` が要求の値) |
| `audio.analysis_failed@1` | settings の後の失敗(音が読めない・decode できない・`hop_ms` が音の 1 標本より短い・file が書けない) | `{asset, out, message}`。表か manifest が開かれたままでやり直しの 2 秒を越えたときは `{asset, out, message, reason: "output_locked", path}`(`path` は開かれていた方。RPC では要求の `out` か `<out>.import.json` の綴り) |
| `audio.analysis_busy@1` | 同じ `out` を焼いている途中(RPC だけ) | `{out, ticket}` |
| `audio.analysis_unavailable@1` | `PELICAN_WITH_AUDIO=OFF` の build | `{reason: "built_without_audio"}` |

test: `ab1_audio_analysis_test` の [「AB-1 a bake of 440 Hz then silence: rms and peak follow the tone and the band holding 440 Hz leads」](../../test/ab1_audio_analysis_test.cpp#L156)(CLI で焼く。前半 440 Hz・後半無音の 1 秒の WAV で、前半の rms と peak、440 Hz を含む `band:3` が他の帯より大きいこと、後半がほぼ 0、2 回焼いて同じ bytes、option の組、範囲外の `--bands 0` で何も書かないこと)。

## 関連文書

- [../design_asset_format_policy.md](../design_asset_format_policy.md) — 二層モデルと形式ポリシー(状態列は一部古い。この章が現状)
- [../design_asset_containers.md](../design_asset_containers.md) — #フラグメント・コンテナ・PSD レーン
- [../external_tools_requirements.md](../external_tools_requirements.md) — 外部ツール契約 R1〜R10(座標系 R6、命名 R7、バージョニング R10)
- [../design_project_dcc_houdini.md](../design_project_dcc_houdini.md) — Houdini レーンと pelican.import
- [../dcc_integration_qa_2026-06-12.md](../dcc_integration_qa_2026-06-12.md) — **pelican.vat v1 仕様の正**(§6)
- [../design_project_vcs.md](../design_project_vcs.md) — asset store と assets manifest
- [第3章 プロジェクト形式](03_project_format.md) / [第4章 シーンと ECS](04_scene_ecs.md) / [第10章 ツールリファレンス](10_tools.md)
