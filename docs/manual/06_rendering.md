# 第6章 レンダリング

対象: pelican2(2026-07-31 に全域再確認、2026-09-04 に §6.15(レイトレの宣言層)を追記、2026-09-17 に §6.5 / §6.11 / §6.16 を更新し §6.17(共通グラフのノード語彙)を追記、2026-09-25 に §6.16 を MG-2b(root は `pelican.graph` だけ)に合わせて書き直し。HEAD=`82054c05`)/ このマニュアルはコードを正とする

## この章で学ぶこと

- レンダリングパイプラインを **JSON だけ**で定義する方法(rendering config)
- パス種別(`material` / `fullscreen` / `raster` / `output_transform` / `ui` / `shadow_depth` / `velocity` / `picking` / `gizmo` / `debug_draw` / `debug_text`)と compute タスク
- カラーパイプライン(表示用の SRGB スワップチェーン + シェーダ内はリニア値、という色の運び方)で気をつけること
- feature(1 行で有効化できるパージ可能な GPU 機能)— canonical anchor(挿入位置に使える標準の目印)・パラメータ・named binding(feature 側の別名を実 RT に結ぶ仕組み)
- フレームグラフ(パス同士の依存だけを宣言し、実行順とバリアはエンジンが決める仕組み)とプランダンプ
- シェーダ基盤 — stem 参照(拡張子を書かないシェーダ参照)・FrameUBO(全シェーダ共通のフレーム定数)・ホットリロード・descriptor set 規約(シェーダへ資源を渡す番号の割り当て)
- マテリアル(`.surface` + `pelican.material` + **OpenPBR**(業界標準の物理ベース材質モデル))、テンポラル(前フレームの絵を使う技法一式 — history / velocity / **projection jitter / 標準 TAA**)、2D スプライト
- OpenXR ステレオレンダリング(論理フレーム = 左右眼をまとめた 1 単位 / feature policy / ミラー)
- グラフ variant(flat / XR / preview)と、GPU デバッグラベル・GPU タイミング・VRAM の見方
- レイトレーシングの宣言層 — `ray_group_layout`(レイトレのシェーダと資源の並びを宣言する形式。読める版は 3 だけ)、`acceleration_structure` 規則、`pelican_ray_instance`
- rendering config の root(`pelican.graph` root だけ。旧い legacy root は MG-2b から読めません)と、**共通グラフのノード語彙**(module / op / 型 / sink、`definitions` と `graph.instance`、入力の 4 mode)
- **共通グラフのループ `graph.loop`** と、グラフで組む pass —— 既存 target の参照 `render.target_ref`、pass の置き場所、view の軸と `view_select`、material ごとの描き方(draw_loop)、feature の pass の置き換え `pass_replacements`、出荷する分解版(§6.18)

## 6.1 全体像

pelican2 の描画パイプラインは、`project.json` の `basic_config.rendering_config_json` が指す **rendering config JSON** で完全に宣言されます。**C++ を書く必要はありません**。

```
project.json ──rendering_config_json──▶ rendering config JSON
                                            │  features 合成 + canonical anchor 実体化
                                            │  + 終端 output_transform 付加(§6.5)
                                            ▼
                              レンダーターゲット登録 + パス列パース
                                            │
                                            ▼
                              フレームグラフのプラン作成・検証(§6.6)
                                            │
                                            ▼
              毎フレーム: プラン順に layout 遷移 → dynamic rendering でパス実行
```

ここで **swapchain(スワップチェーン)** は「画面に出す番が来たら OS/ドライバへ渡す画像」の並びのことで、**display** はエンジンが用意する canonical(標準)なレンダーターゲット名です — 終端の `output_transform` が書き込む先で、最終的に swapchain の画像になります([用語集](14_glossary.md))。

`basic_config.default_rendering_pass` が rendering config 内のどのパス列(`rendering_passes[].name`)を使うかを決めます。エンジンは常にパージ不能な終端 `output_transform` ノード(リニア → 表示エンコードの 1 箇所)を生成し、**このノードが終端に来ないパス列を既定に指定すると起動時エラー**です(§6.3)。なお authored な `"swapchain"` 出力は合成時に `display` RT へ置換されるため、`display` を誰も書かない構成はフレームグラフ検証で落ちます(§6.11)。

### 座標の規約(✅CC-1 `c8367b87`、CC-2 `1501685a`)

> **設計決定(2026-10-02 利用者裁定。[design_coordinate_convention.md](../design_coordinate_convention.md) §0・§2):** 画面の側(clip・NDC・画素・uv・画像)は全部 y 下、world の側は全部 y 上。**両者をつなぐのは camera の射影だけ**で、射影の中で clip の y を反転する。engine 全体を設定で切り替える形にはしない。

| 空間 | 規約 |
|---|---|
| world | 右手系、+Y が上、メートル。glTF と同じ(設計 §1.2 の調べでは、glTF の loader は軸も UV も変換しません) |
| view(camera) | +X が画面の右、+Y が上、**−Z が前方**。回転なしの camera は −Z を向き、up は transform の回転の +Y(glTF の camera と同じ。[第4章 §4.2](04_scene_ecs.md) の camera) |
| clip・NDC | Vulkan のまま。x / y は −1..1 で **+y が画面の下**、z(深度)は 0..1 |
| 画素・画像 | 左上が原点、y 下。viewport の height は正のまま(engine は負の height の反転を使いません。例外は下の `raster` pass の `clip_y: "up"` だけ) |
| uv | NDC と同じ向き(`uv = ndc * 0.5 + 0.5`、左上が (0, 0))。engine の [fullscreen.vert](../../src/core/resources/fullscreen.vert#L12) も `gl_Position = uv * 2 - 1` |
| 面の表 | **反時計回り**(glTF と同じ)。cube の面への描画だけ時計回り(下) |

- 射影を作る関数は [`clipspace.hpp`](../../src/core/renderer/clipspace.hpp#L35) の `vulkanPerspective` / `vulkanOrtho` / `vulkanAsymmetric`(右手系・深度 0..1・`[1][1]` が負)に集めてあり、flat の camera、editor の preview、XR の両眼、spot と directional の影がこれを使います。view は transform の逆行列([`viewFromWorld()`](../../src/core/renderer/clipspace.hpp#L100))です。FrameUBO の `projection` はこの反転を含み、その上に projection jitter が NDC で足されます(§6.8。反転は jitter の前なので、TAA・velocity・再投影の符号は変わっていません)。shader が `projection * view * world` で clip を作れば、この規約に乗ります。
- **mesh を描く pipeline(material / velocity / shadow depth)の面の表は view ごとに決まります**。pipeline は front face を dynamic state(`VK_DYNAMIC_STATE_FRONT_FACE`)で持ち、描くときに view が鏡像かどうか([`viewIsMirrored()`](../../src/core/renderer/clipspace.hpp#L110))で反時計回りか時計回りを選びます([選ぶ所](../../src/core/renderer/materialrender.cpp#L34))。鏡像になるのは cube の面への描画(cube capture と点光源の影)だけで、Vulkan の cube の面の規約どおり、y を反転しない行列で描くので時計回りが表です。planar reflection の view は反射と clip の x の反転で巻き方を保つので反時計回りです。
- **NDC と uv は同じ向きで、範囲だけが違います。** shader で行き来するときは `pelican_view.glsl` の `pelican_ndc_to_uv(vec2)`(`ndc * 0.5 + 0.5`)と `pelican_uv_to_ndc(vec2)`(`uv * 2 - 1`)を使います(✅CC-2、`1501685a`。[関数](../../src/core/resources/shaders/include/pelican_view.glsl#L20))。y を `1 - y` で反転する必要はありません。
- **`raster` pass の `clip_y`(✅CC-2、`1501685a`)。** 利用者の vertex shader が clip を直接書く `raster` pass(と graph の `render.raster` node)は、pass の直下(`raster_state` の中ではない)に `clip_y: "down" | "up"` を書けます。既定の `"down"` は engine と同じ規約(clip の +y が画像の下)です。`"up"` はその pass を負の height の viewport で描き(clip の +y が画像の上。y 上の clip を前提に書いた shader がそのまま正立する)、描き終えたら viewport を戻します([`setClipYViewport()`](../../src/core/vkcore/render_pass_dispatch.cpp#L44))。scissor と出力の画像の向きは変わりません。`"down"` / `"up"` 以外は `raster pass '<名前>' field 'clip_y' has unknown value '<値>'`、文字列でなければ `... field 'clip_y' must be a string`、graph の node では `graph.render_node_parameter_invalid@1 {node, op, parameter: "clip_y", expected: "enum", reason: "enum", allowed: ["down", "up"]}` です。catalog の `render.raster` の parameters に `clip_y`(enum、既定 `down`)が載ります。正規形の JSON には常に `clip_y` が出ます([正規形](../../src/project/rasterpass.cpp#L603))。
- **`front_face` と `cull` は、どちらの `clip_y` でも出来上がった画像の上で見た巻き方を指します。** Vulkan は viewport を通した後の framebuffer の座標で面を判定し、engine は `clip_y: "up"` で viewport しか変えないからです(コードの注記)。`"up"` では絵が上下に反転するので、同じ頂点データの表と裏も入れ替わります(`"down"` で表だった三角形は `"up"` では裏)。`raster_state.front_face` の値は pipeline に固定で入り、material pass のように view で切り替わりません。engine の `projection` を通して clip を作るなら `"down"` のまま上の規約に乗ります。
- **`clip_y` は `raster` pass だけが持ちます。** `fullscreen` pass(と material pass ほか raster 以外の種別)に書くと pass の欄の所有の検査で `Pass '<名前>' type 'fullscreen' does not own field 'clip_y'` の形で断られます(test が確かめているのは fullscreen。ほかの種別は同じ検査を通るコードの読み)。`raster_state` の中に書くと未知の field です。test: `rasterpass_test` の「CC-2 a raster pass's clip_y is down unless written, and only render.raster takes it」、`headless_render_test` の「CC-2 a raster pass with clip_y up draws its clip +y at the top and judges faces on the image」(GPU。同じ vertex shader を `down` と `up` で描き、`cull: back` と `gl_FrontFacing` で表裏の入れ替わりを見る)。
- CC-1 より前は、3D の画面が world の**鏡像**でした(右手系の view・y を反転しない射影・正の viewport height の組み合わせ。面の表を時計回りに固定して辻褄を合わせていた)。正面から見た文字が左右反転し、+X が画面の左に出ていました。検査は `coordinate_mirror_headless`(「F」の板と +X・+Y の印を写し、+X が右・+Y が上・F が正しい向きで読めることを確かめる。GPU)です。経緯は [source-code-guide 第9章 §9.8](../source-code-guide/09_black_magic_and_gotchas.md) の「画面は長く world の鏡像だった」。

## 6.2 rendering config のスキーマ

実物の最小構成([../../projects/example/passes/example_renderingpass_data.json](../../projects/example/passes/example_renderingpass_data.json) — G-buffer → ライティング → UI。**G-buffer** は「1 パス目で色・法線・粗さなどを画面サイズの複数枚に書き出し、2 パス目でそれを読んでライティングする」deferred 方式の中間バッファ群のことです。[用語集](14_glossary.md))は前版と同じ骨格で動きます。example が実際に使う [main_rendering_config.json](../../projects/example/passes/main_rendering_config.json) は SSAO と 4 段ブルームを足した 17 パス構成です(ファイルに書かれた authored 件数。実行時はこれに ui feature のパスと終端 `output_transform` が合成されます)。

### トップレベルキー

| キー | 型 | 内容 |
|---|---|---|
| `features` | string 配列 | feature fragment の参照(§6.5) |
| `shader_defines` | string 配列 | 全シェーダのコンパイルに注入する define |
| `render_targets` | 配列 | レンダーターゲット(RT)宣言 |
| `buffers` | 配列 | フレームグラフ用 GPU バッファ(§6.6) |
| `rendering_passes` | 配列 | `{name, passes: [...]}` のパス列 |
| `compute_tasks` | 配列 | compute タスク(§6.6) |
| `snapshots` | 配列 | 名前付きスクリーンスナップショット(§6.8)✅WP83 |
| `resolver_version` | int | 省略可(省略時 2 を補完)。**2 のみ受理** ✅WP73 |
| `draw_sort` | object | 描画順ポリシー(後述「描画順ポリシー」)✅WP183/184 |
| `graph_transforms` | 配列 | 論理グラフ全体を変換する provider の順序付き chain(後述「差し替えプロバイダの入口」)✅WP202a |
| `render_strategy` | object | renderer の seed config そのものを生成する provider の選択(同上)✅WP202b |
| `material_route_default` | string | automatic な material に当てる route の既定。`"auto"`(省略時)/ `"forward"`(§6.7「material の route の既定」)✅G-4 |

このほか `target_planning`(§6.6)、`vulkan_plan_pins` / `vulkan_physical_fragments`(§6.6)、`xr`(§6.12)もトップレベルキーです。

editor の RPC から書くときは、`shader_defines`・`snapshots`・`resolver_version`・`draw_sort`・`graph_transforms`・`render_strategy`・`xr`・`multisampling` を `edit_rendering_keys`(✅EL-3)が丸ごと書き、ほかのキーはそれぞれ別の RPC が書きます(`features` は `edit_render_features`、`material_route_default` は `set_material_route_default` など。対応の表は [第13章](13_editor.md) §13.5 の「rendering 節のほかのキーを書く」)。preset を選ぶ root では `multisampling` と `resolver_version` を書けません。

> **上の表のキーはファイルの最上位ではなく `rendering` の下に書きます。** rendering config のファイルは `"schema": "pelican.graph"` を名乗る **`pelican.graph` root** で、最上位には共通グラフ自身の 5 キーだけが並び、**上の表のキーは全部 `rendering` の下に入ります**。ただし `buffers` と `compute_tasks` は `rendering` の下にも書けません(共通グラフのノードか task feature で書きます)。上の表のキーを最上位に直接並べた旧い形(legacy root)は、MG-2b から `render_config.legacy_root@1` で断られます。詳しくは **§6.16** を読んでください。

名前の重複(RT・パス・バッファ)はすべて hard error です。

> **direct root(preset を選ばない root)が `rendering_passes` に自分で宣言した pass は、Studio の Pass Editor と Frame Plan から編集でき、root の文書に書き戻されます(✅F-6。RPC は `edit_preset_pass`、[第13章 §13.5](13_editor.md))。**

### render_targets

| フィールド | 必須 | 内容 |
|---|---|---|
| `name` | ✔ | `"swapchain"` は予約名(書くとエラー) |
| `extent_scale` | ✔ | ウィンドウ(swapchain)サイズ × scale。> 0 |
| `width` / `height` | 任意(両方セット) | 固定サイズ RT(シャドウマップ等)。片方だけはエラー |
| `format` | ✔ | 45 種。8/16/32-bit の `R` / `RG` / `RGBA` に対する UNORM・SNORM・UINT・SINT・SFLOAT、sRGB、10/11-bit packed、depth を受理する。完全な正は [`renderingpassjsonhelpers.cpp`](../../src/core/renderingpass/renderingpassjsonhelpers.cpp) の `format_names` |
| `format_candidates` | 任意 string 配列 | verified Vulkan physical fragment が選択してよい追加 format。`format` は常に自動/default候補として先頭へ補われ、重複は除去される。宣言しただけでは自動formatは変わらない |
| `format_class` | 任意 | `scene` / `display` / `data` / `explicit(<FORMAT>)`。カラーリゾルバ(= `format_class` から実 Vulkan format を決める解決器。§6.3 で説明します)が実 format を決める✅WP73 |
| `role` | 任意 | `color` / `data`。SRGB view / UNORM view の選択 |
| `history` | 任意 bool | `true` で 2 面持ち。前フレーム面は `<名前>@history` で読める(§6.8)✅WP88 |
| `clear_color` | 任意 | history RT の初期化色 |
| `lifetime` | 任意 | `transient`(既定)/ `persistent`。✅PS-1。`persistent` は中身を frame をまたいで保つ 1 枚の image で、alias・transient attachment・tile-local の候補になりません(§6.18「一度だけ走る pass と、frame をまたぐ image」)。ほかの値は `Render target lifetime must be persistent or transient: <名前>`([読む所](../../src/core/renderingpass/rendertargetjsonparser.cpp#L270))。graph の `render.image` の `parameters.lifetime` はここへそのまま運ばれます |
| `dimension` | 任意 | `2d`(既定) / `cube`。cubeは固定かつ正方形の`width`/`height`と6 layersが必要 |
| `mip_levels` | 任意 | 正整数または `"full"`。省略時1。`"full"`は実extentから最大mip chainを作り、resize時に再計算する |
| `layers` | 任意 | 正整数。2Dは省略時1でarray容量を表し、XRの内部view数とはphysical loweringで最大値を取る。cubeは省略時6、明示時も6固定 |
| `usage` | ✔ | `COLOR_ATTACHMENT` / `DEPTH_STENCIL_ATTACHMENT` / `SAMPLED` / `STORAGE` / `TRANSFER_DST` / `TRANSFER_SRC` |

> **Studio から宣言できます(✅F-1)。**Frame Plan の **New render target...** は、この表の宣言を
> `project://passes/authoring/render-targets.json`(`render_targets[]` だけを持つ editor 管理の feature。root の `features[]`
> に自動で足されます)に書きます。Studio が編集・削除できるのはこの file の target だけで、root や preset、他の feature で
> 宣言した target は読み取り専用です。RPC は `edit_render_targets`、拒否の一覧は [第13章 §13.5](13_editor.md) にあります。

runtime cubemapは資源形状だけを宣言し、六面をViewFamilyの6 viewとはみなしません(**view** = 1 つの視点、**ViewFamily** = 同じ内容を描く視点の束のこと。§6.12・[用語集](14_glossary.md))。

```json
{
  "name": "environment_capture",
  "dimension": "cube",
  "width": 256,
  "height": 256,
  "extent_scale": 1.0,
  "format": "R16G16B16A16_SFLOAT",
  "mip_levels": "full",
  "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
}
```

このimageはraster時に各faceの2D attachment view、sampling時に6 faceのcube viewを公開します。
cube array、runtime 3D、cube storage imageは現在未対応で、暗黙に2Dへ落とさず起動時に拒否します。

### pass 共通フィールド

以下で **attachment(アタッチメント)** は「そのパスが描き込む先として GPU のパイプラインに結び付ける画像」を指す Vulkan の用語です(`output.color` / `output.depth` に書いた RT がそれになります)。パスが読むだけの画像は attachment ではなく `input` 側です。

| フィールド | 必須 | 既定 | 内容 |
|---|---|---|---|
| `name` | ✔ | — | パス列内で一意 |
| `type` | ✔ | — | `material` / `fullscreen` / `raster` / `output_transform` / `ui` / `shadow_depth` / `velocity` / `picking` / `gizmo` / `debug_draw` / `debug_text`(+ ImGui ビルド時 `imgui`) |
| `output` | ✔ | — | `color`(null / attachment / attachment配列)と `depth`(null / attachment)の**両キー必須**。attachmentは従来の名前または`{target, subresource}`。`"swapchain"` は color のみ |
| `input` | 任意 | — | **`fullscreen` / `output_transform` / `raster` / `material` 限定**(ほかの type に書くと違反名 `input_unsupported` で、文言は `Pass shape violation 'input_unsupported' in pass '<名前>': ...`)。読み込む RT / バッファ名で、RT には **`@history` サフィックス**可(history RT のみ) |
| `resource_ports` | 任意 | — | **`fullscreen` / `output_transform` / `raster` 限定**(ほかは `Pass '<名前>' type '<type>' does not own field 'resource_ports'`)。`input` の画像を logical name、sampled access、shared/per-view/cube view、filter/address、mip/layer subresourceで注釈し、generated shader accessorを作る。依存edgeは増やさない |
| `input_settings` | 任意 | — | ✅RS-14(`8c1787ca`)。**`fullscreen` / `output_transform` / `raster` 限定**([所有の表](../../src/project/passfieldownership.cpp#L31)。ほかは `does not own field 'input_settings'`)。`input` の画像の入力の番号順の配列で、要素は `null`(既定のまま)か `{sampling?, subresource?}`。中身は `resource_ports` の port の `sampling` / `subresource` と同じ parser で読みます([`parseShaderResourceInputSetting()`](../../src/project/shaderresourceport.cpp#L582))。画像の入力の数より短くてよく、長いと `Pass input_settings has more entries than image inputs`。resource port が束ねる入力に設定を書くと `Pass input <i> is bound by a resource port and also has input_settings`。`fullscreen` の `input_sampling`(filter / address だけの配列。数は画像の入力と同じ、`resource_ports` とは同居できない)と並べると、設定のある入力は `input_settings` の `sampling` が上書きします([`applyPassInputSettings()`](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L1673))。通常は手で書かず、graph の番号付きの入力から lowering が書きます(§6.18「番号付きの入力」) |
| `view_family` | 任意 | `$main` | このパスを実行する ViewFamily の ID(§6.12)。compute タスクにも同じキーがある |
| `resolution_domain` | 任意 | type 依存 | `scene` / `output` / `independent` / `unclassified`(= `none`)。MSAA の sample count 計画がパスを同じ解像度圏へまとめる区分。既定は `material` / `velocity` / `picking` = `scene`、`output_transform` / `ui` / `gizmo` / `imgui` = `output`、`shadow_depth` = `independent`、それ以外 = `unclassified` |
| `regions` | 任意 | — | subgraph replacement 用の region タグ(string 配列)。重複は `has duplicate region tag`(後述「差し替えプロバイダの入口」)|
| `implementation` | 任意 | — | **`fullscreen` / `output_transform` 限定**。`{"provider": "<名前>"}` **ちょうど 1 キー**。ほかの type に書くと所有していない旨の名指しエラー|
| `draw` | `raster` では必須 | — | **`raster` 限定**(ほかの type に書くと所有していない旨の名指しエラー)。後述「type: raster」|
| `raster_state` | 任意 | — | **`raster` / `fullscreen` / `output_transform` 限定**。`fullscreen` 系では color blend/write-mask と topology/cull/front-face だけを受理し、`depth_*` は名前付きエラーで拒否する。後述「固定機能状態」|
| `material_resources` | 任意 | — | **`material` 限定**。`.surface` のtyped buffer/image portをframe-graph resourceへ割り当てる。resource、history、view、sampling、mip/layer subresource、read footprintから依存とbarrierを導出する |
| `color_load_op` / `color_store_op` | 任意 | `Clear` / `Store`(ui のみ load 既定) | `Clear` / `Load` / `DontCare` |
| `depth_load_op` / `depth_store_op` | 任意 | `Clear` / `DontCare` | シャドウマップでは `depth_store_op: "store"` を明示 |
| `clear_color` | 任意 | `[0,0,0,1]` | 4 要素固定 |
| `clear_colors` | 任意 | — | `output.color` の RT 名をキーにした attachment 別 clear。未指定の attachment は `clear_color` を使う。UINT/SINT RT では整数範囲・整数値を起動時検証 |
| `after` / `before` | 任意 | — | フレームグラフの明示エッジ(§6.6) |
| `view_select` | 任意 | — | ✅LP-Va。二次の view family のうち、軸の値が一致する view だけで走る(`{"cascade": 1}` など。§6.18「view の軸を選ぶ」)。compute タスクにも同じキーがある |

attachmentのmip/layerを明示する例:

```json
"output": {
  "color": [{
    "target": "reflection_color",
    "subresource": {
      "mip": 2,
      "layer": 4,
      "layer_count": 2
    }
  }],
  "depth": {
    "target": "reflection_depth",
    "subresource": {
      "mip": 2,
      "layer": 4,
      "layer_count": 2
    }
  }
}
```

`subresource`の語彙はresource portと共通ですが、raster attachmentは厳密に1 mipだけです。
`mip_count`の省略値は1で、複数値や`"remaining"`は使えません。`layer_count`はpassの
logical view数と一致させます。sequential実行では`layer + view_index`の1-layer viewへ、
multiviewでは指定範囲の2D-array viewへloweringされます。選択mipの実サイズがrender area、
viewport、scissorになり、全color/depth attachmentで一致する必要があります。
swapchainのsubresource、同一targetの複数attachment slot、範囲外、non-zero mipのMSAAは
起動時エラーです。省略した従来の名前形式はmip 0と既存のview-index規則を保ちます。

依存、barrier、layout trackingは現時点ではimage全体を保守的に扱います。明示subresourceの
raster attachmentは、同じrangeを指名するsame-pixel input contractがまだ無いため
tile-local化せずmaterialized imageとして実行されます。

cube targetのface出力も同じ形式です。`layer`は0〜5のface indexで、単一view passでは
`layer_count`を省略できます。cube全体をraster attachmentとして一度にbindするのではなく、
各faceを2D viewとして明示的に描画します。

主な検証(すべて起動時の名指しエラー): input の RT に `SAMPLED` usage が必要 / 1 パスの全出力 attachment は同一実サイズ / 同一 RT の入出力同時使用は不可(違反名 `current_frame_color_feedback` / `current_frame_depth_feedback`)—— ただし **`@history` を付けた input は免除**され、同梱 TAA はこれに依っている(`taa_accum@history` を読みつつ `taa_accum` へ書く) / **同じ RT を `input` に 2 度書くのは不可**(`duplicate_input`)—— **判定対象は画像(RT)だけで、buffer input は重複してよい。**重複判定は `@history` を剥がした後の名前で行うので、**`X` と `X@history` を同じパスに両方書くと起動時に落ちる** / input に書いた RT は先行パスが出力していること。

### type 別の要点

- **`material`** — シーン内のモデルを描く material パス。`material_outputs` を省略した従来設定は、既定の 5 枚 G-buffer（または forward の scene color 1 枚）をそのまま使います。この 5 枚の内訳を決めるのはエンジンではなくプロジェクトの config とシェーダで、example では `gbuffer_albedo` / `gbuffer_normal` / `gbuffer_material` / `gbuffer_worldpos` / `g_emissive` です([main_rendering_config.json:155](../../projects/example/passes/main_rendering_config.json#L98))。明示した場合は**順序・枚数・数値型を任意に定義**でき、`material_output_states`でfield別のblend/write-maskも指定できます。固定のエンジン上限はなく、実行デバイスの `maxColorAttachments` と各 format/sample capability が物理上限です。詳しくは §6.7。`material_outputs` を書かない material pass に contract が要求するのは、forward 系(`forward_opaque_v1` / `forward_transparent_v1`)なら color 1 枚(`R16G16B16A16_SFLOAT`)、G-buffer 系(`deferred_geometry_v1` / `legacy_gbuffer_v1`。`material_contract` を省略すると後者)なら color 5 枚(albedo / normal / material / world position / emissive の順。albedo と emissive は HDR の表で `R16G16B16A16_SFLOAT`、SDR の表で `B8G8R8A8_SRGB` —— どちらの表かは 1 枚目の format で決まります —、normal と world position は `R16G16B16A16_SFLOAT`、material は `R8G8B8A8_UNORM`。[G-buffer の SDR / HDR の format 表](../../src/core/renderingpass/materialpassattachments.hpp#L15))で、どちらも `D32_SFLOAT` の depth 1 枚が必須、swapchain には書けません([material pass の contract ごとの枚数と format を検査する所](../../src/core/renderingpass/renderingpassvalidation.cpp#L299))。engine が受ける format は、この検査と同じ表から `get_graph_catalog` の `material_pass_contracts`(contract ごとの socket と、通る組)と `material_output_formats`(`material_outputs` の型ごと)で読めます(✅EL-4。[第13章](13_editor.md) §13.5)。Studio からは Frame Plan の **Add material pass...** で contract を選んで足せ、足りない target をこの format で作れます(✅G-1、[第13章](13_editor.md) §13.5)。
- **`fullscreen`** — 全画面 1 枚描き。`shader: { "vertex": <stem>, "fragment": <stem> }` **必須**(任意で `defines`。✅VI-1、§6.4 の「pass ごとの `shader.defines`」)。`input` の画像は通常 `resource_ports` で名前を付け、fragment shaderからgenerated `pelican_sample_<port>()`で読む。**入力枚数にエンジン固定の上限はありません**(✅WP238b で `Fullscreen pass has too many inputs` の per-pass 拒否を撤去。残った数は descriptor pool のサイズ見積もりで —— WP238b では 8、✅VI-1 から可変の組の上限と同じ 16([見積もり](../../src/core/fullscreenpass/fullscreenpasscontainer.cpp#L32))—、実上限は選択されたデバイスと pipeline layout が持ちます)。1つのinput resourceへ複数portは割り当てず、mip/layerを変える場合も1 portの`subresource`で選ぶ。raw shaderだけは従来どおりset 1へ配列順でbindする。`uses_light_data: true` と `push_constants`(`"none"` / `"camera_position"` / `"projection_view"`)は**互換キー**として受理されますが、GPU への実際の供給元は常に set 0 の FrameUBO / LightUBO です(§6.4)。任意の`raster_state`で1枚のcolor attachmentのblend/write-maskとtopology/cull/front-faceを書けます。深度状態は受理しません。`input` の数・順・種類は fragment の set 1 の宣言と合わせます(§6.4 の「fullscreen pass の入力と shader の宣言の契約」。✅Q-14e)。
- **`raster`** — 汎用ラスタパス(✅WP238b)。シーンのモデルにも全画面 quad にも依らず、`draw` で描画コマンドを、`raster_state` で固定機能状態を直接宣言します。後述の専用項を参照。
- **`output_transform`** — リニア → 表示エンコードの終端ノード。**自動付加されるため通常は書きません**(§6.3)。内部契約は`fullscreen`と同じ`raster_state`所有表を共有し、blend/write-mask等を同じ経路で解決します。自動生成される既定ノードは`raster_state`を省略します。
- **`velocity`** — モーションベクタ出力(§6.8)。**モーションベクタ(velocity)** は「この画素の中身が前フレームからどれだけ画面上を動いたか」を 2 成分で書いた画像で、TAA やモーションブラーが前フレームの対応画素を引くのに使います。フィールドは `shader.{vertex, skinned_vertex, fragment}`(既定 `engine://velocity` / `engine://velocity_skinned`)。通常は feature 経由。
- **`picking`** — モデル面へ整数 ID を書く視覚ピッキングパス。フィールドは `shader.{vertex, skinned_vertex, fragment}`(既定 `engine://picking` / `engine://picking_skinned`)。通常は `engine://features/picking.json` 経由(本節後半)。
- **`gizmo`** — 選択 transform の移動・回転・拡縮ハンドルを canonical display へ重ねる line-list パス。通常は `engine://features/gizmo.json` 経由(本節後半)。
- **`ui`** — UI オーバーレイ([第7章](07_input_ui.md))。WP87 以降は `engine://features/ui.json` 経由の挿入が標準です。
- **`shadow_depth`** — depth 専用パス。`shader` 省略時は `engine://shadow_depth`。
- **`debug_draw` / `debug_text` / `imgui`** — 通常は feature 経由で挿入されます(§6.5)。

なお `sprite` という**パス型はありません** — スプライトは feature + anchor 方式で描かれます(§6.9)。

### type: raster — 汎用ラスタパス(✅WP238b)

`material` でも `fullscreen` でもないラスタ技法(クリア三角形、プロシージャルジオメトリ、
デバッグ描画の自作版など)を、エンジンにパス種別を足さずに書くための型です。
新しい技法ごとにエンジン側の `PassInfo` とパーサと実行分岐を増やす構造をやめる、
というのが設計意図です(設計の正: [../design_heterogeneous_execution_graph.md](../design_heterogeneous_execution_graph.md))。

`sprite_demo` が実際に使っている実物([../../projects/sprite_demo/passes/main.json](../../projects/sprite_demo/passes/main.json) の 1 行を整形):

```json
{
  "name": "ssao_clear",
  "type": "raster",
  "output": { "color": "ssao_blur", "depth": null },
  "draw": {
    "implementation": "sprite_demo.clear_triangle@1",
    "operation": "direct",
    "vertex_count": 3
  },
  "shader": {
    "implementation": "sprite_demo.clear_shader@1",
    "vertex": "shaders/fullscreen",
    "fragment": "shaders/white"
  },
  "clear_color": [1, 1, 1, 1]
}
```

**`shader`(必須・object)** — 受理するフィールドは `implementation` / `vertex` / `fragment` / `defines`(✅VI-1。§6.4 の「pass ごとの `shader.defines`」)だけです
(ほかは `Unknown raster shader field '<key>'`)。`vertex` は必須。`fragment` は任意ですが、
`output.color` が 1 枚以上あるときは必須です(`Raster pass with color outputs requires a fragment shader`)。
`implementation` は省略時 `pelican.raster.authored_shader@1` で、書く場合は
**`namespace.name@major` の canonical 構文**でなければ
`Raster pass shader implementation must use canonical namespace.name@major syntax` になります。

**`draw`(必須・object)**

| フィールド | 既定 | 内容 |
|---|---|---|
| `implementation` | `pelican.raster.authored_direct@1` | 描画コマンドを生成した algorithm の provenance。backend が消費するのは `operation` の側で、この ID ではありません |
| `operation` | `"direct"` | **現状 `"direct"` のみ**。ほかを書くと `has unsupported typed operation '<x>'` |
| `vertex_count` | 3 | direct draw の頂点数 |
| `instance_count` | 1 | インスタンス数 |
| `first_vertex` | 0 | 先頭頂点 |
| `first_instance` | 0 | 先頭インスタンス |

`draw` 自体を省略すると `<パス名> requires a draw object` です。

**`clip_y`(任意・`"down"` / `"up"`、既定 `"down"`。✅CC-2 `1501685a`)** — vertex shader が書く clip の +y が画像のどちらを向くか。`"up"` はこの pass だけ負の height の viewport で描きます。`raster_state` ではなく pass の直下に書き、`raster` 以外の pass は持てません。`front_face` / `cull` の意味は変わらず、出来上がった画像の上で見た巻き方です(§6.1「座標の規約」)。

**固定機能状態 `raster_state`(任意・object、✅WP238b/353)**

| フィールド | 既定 | 値 |
|---|---|---|
| `topology` | `triangle_list` | `point_list` / `line_list` / `line_strip` / `triangle_list` / `triangle_strip` |
| `cull` | `none` | `none` / `front` / `back` |
| `front_face` | `counter_clockwise` | `counter_clockwise` / `clockwise`。出来上がった画像の上で見た巻き方で、`clip_y` がどちらでも同じ意味です。値はこの pass の pipeline に固定で入り、material pass のように view で切り替わりません(§6.1「座標の規約」) |
| `depth_test` / `depth_write` | `false` | bool。**`raster`限定**。`fullscreen` / `output_transform`では拒否 |
| `depth_compare` | `less` | `never` / `less` / `equal` / `less_equal` / `greater` / `not_equal` / `greater_equal` / `always`。**`raster`限定** |
| `color_attachments` | — | 配列。`raster`では**要素数を `output.color` の枚数と厳密一致**させる(`color_attachments count must match pass color outputs`)。`fullscreen` / `output_transform`はcolor出力が1枚なので1要素。各要素は `blend` / `write_mask` の 2 キーのみ |

`color_attachments[].blend` / `.write_mask` の値の語彙は §6.7 の `material_output_states` と**同一のパーサ**です。
ただしキーは別で、`material_outputs` / `material_output_states` は material パス限定です
(所有していない旨の名指しエラー)。`raster` / `fullscreen` / `output_transform`のblend / write maskは
必ず `raster_state.color_attachments[]` に書きます。`"additive"` presetのcolor係数は
`SrcAlpha/One`、alpha係数は`One/One`です。colorも`One/One`にしたい場合はpresetではなく
color/alphaを明示してください。UINT/SINT color targetでblendを有効にすると、shader登録前に
パス名と出力添字を含むエラーで拒否します。

`fullscreen` / `output_transform`で`raster_state`自体を省略した場合、従来どおりpipelineの
color attachment stateは「未指定」の空表現を保ちます。省略をopaque/RGBAの展開済み既定へ
書き換えません。明示した場合だけ固定機能状態を生成します。また`depth_test`、`depth_write`、
`depth_compare`など`depth_*`をこれらの型へ書くと、無視せずフィールド名付きで拒否します。

`input` / `resource_ports` も受理します。resource port は image が `sampled`、buffer が `storage` に限られ、
1 つの resource へ複数 port は割り当てられません。color attachment 数にもエンジン固定の上限はありません。

### 描画順ポリシー `draw_sort`(✅WP183/184)

不透明・半透明それぞれの DrawQueue の並べ替え手法を、rendering config のトップレベルで選びます。

```json
{
  "draw_sort": {
    "opaque": { "provider": "state_batched_v1" },
    "transparent": { "provider": "back_to_front_v1" },
    "xr_view_policy": "logical_view_center"
  }
}
```

- 受理するキーは `opaque` / `transparent` / `xr_view_policy` の 3 つだけ、phase オブジェクトは `provider` 1 キーだけです。
- 既定値は **`opaque` = `state_batched_v1` / `transparent` = `back_to_front_v1` / `xr_view_policy` = `logical_view_center`**。同梱プリセット `engine://render_pipelines/hybrid_v1.json` もこの 3 つをそのまま書いています。
- `xr_view_policy` は `logical_view_center`(既定・左右眼の中心を 1 回だけ評価)または `per_view`(view ごとに sort をやり直す)。未知の値は `resolved draw_sort has unknown xr_view_policy: <x>` です。
- **`per_view` は view 数がちょうど 2 のときだけ有効**で、それ以外の view 数のステレオ描画では実行時に `XR per_view draw sorting requires exactly two views` で落ちます。
- provider は公開 ABI(`src/core/userpublic/render/draw_sort_abi_v1.hpp`)で、ゲーム DLL 側から自作の並べ替えを登録して名前で選べます。
- secondary ViewFamily(planar reflection 等)の透明パスは、**同じ provider を view ごとに再評価**します(§6.5 / §6.12)。

### 差し替えプロバイダの入口(✅WP200/201/202a/202b)

エンジンは「どの粒度をユーザー空間へ開くか」を 4 段階に分けて公開しています。
**マニュアルは JSON の入口だけを示します** — 各 ABI の設計論と C 側の契約は
[../design_render_pipeline_extensibility.md](../design_render_pipeline_extensibility.md) が正です。
`projects/` の同梱プロジェクトはまだこの 4 つを使っていない(使っているのは golden fixture 側)ため、下の JSON は**パーサ実装から起こした(例)**です。

| 粒度 | config の書き場所 | 既定 provider | 対象 |
|---|---|---|---|
| 1 パスの実装 | pass の `implementation.provider` | `builtin.fullscreen_v1`(`builtin.` はエンジン同梱 provider の namespace、`_v1` は上の `namespace.name@major` と同じ意味の major 版) | **`fullscreen` / `output_transform` 限定**(§6.2)。logical な pass contract を保ったままシェーダ対を差し替える |
| 連続区間の置換 | pass の `regions[]` + パス列の `region_replacements[]` | `builtin.identity_v1` | **連続した fullscreen パス**を最大 256 パスへ展開・置換する |
| 論理グラフ全体の変換 | トップレベル `graph_transforms[]` | (省略可) | feature 合成後の config を順序付き chain(最大 32 段)で変換する |
| renderer の seed 生成 | トップレベル `render_strategy` | `builtin.authored_config_v1` | preset 展開後・feature 合成前の seed config **全体**を provider が生成する |

```json
{
  "render_strategy": { "name": "project.custom_renderer", "provider": "project.render_strategy", "parameters": {} },
  "graph_transforms": [ { "name": "project.msaa_like", "parameters": {} } ],
  "rendering_passes": [
    {
      "name": "main",
      "region_replacements": [ { "region": "region.post", "provider": "game.custom_post" } ],
      "passes": [
        { "name": "tone", "type": "fullscreen", "regions": ["region.post"],
          "input": ["scene_linear"],
          "shader": { "vertex": "engine://fullscreen", "fragment": "engine://scene_present" },
          "output": { "color": "display_linear", "depth": null } }
      ]
    }
  ]
}
```

- `render_strategy` / `graph_transforms[]` の要素はどちらも `name`(必須)/ `provider` / `parameters` の 3 キーだけです。
- `region_replacements[]` の要素は `{region}` か `{region, provider}` のちょうど 1〜2 キー、1 パス列あたり最大 64 件です。region タグの重複、保護ノード(`output_transform` / `__anchor_*`)の置換、非連続なタグ付けはすべて起動時の名指しエラーになります。
- **黙った fallback はしません** — 未登録 provider、ABI 版の不一致、provider の出力が契約を満たさない場合は、いずれもパス名と provider 名入りの compile error です。

## 6.3 カラーパイプライン(✅WP72〜74)

前提の 2 語だけ先に置きます。**sRGB** は PNG やモニタが使う色の符号化で、格納値と実際の光の量が非線形に対応します(暗部に段階を厚く割り当てるため。符号化に使う区分関数と色域の規格は **IEC 61966-2-1** で、エンジンの `Pelican::srgb()` も同じ区分点 0.04045 / 12.92 / 1.055 / 2.4 を使います — [color.hpp:33](../../src/core/userpublic/color.hpp#L33))。**リニア(線形)** はその逆で、値がそのまま光の量に比例します。加算や乗算が物理的に正しくなるのはリニア側なので、計算はリニアで行い、表示の直前だけ sRGB へ符号化する、というのがここでの方針です(用語としての整理は [用語集](14_glossary.md)、Vulkan 側の `*_SRGB` フォーマットの扱いは [ソースコード読解ガイド §10.1](../source-code-guide/10_background_knowledge.md#srgb-フォーマットをスコアで選ぶ理由))。

2026-07-11 に **SRGB スワップチェーン + リニアワークフロー**へ全面移行しました(設計の正: [../design_color_pipeline.md](../design_color_pipeline.md) v4)。

> **設計決定(リニアワークフロー):** 作業空間は linear-sRGB(Rec.709/D65)。**シェーダの入出力はすべてリニア**で、sRGB の encode/decode はハードウェア(`*_SRGB` view)か、終端の `output_transform` ノードの 1 箇所だけが行う。transfer が掛かるのは RGB のみ(alpha は常にリニア straight)。手書きの `pow(x, 2.2)` 等はエンジンシェーダから全廃済み。

ユーザーが気をつけること:

1. **色テクスチャは sRGB で作る**(baseColor / emissive → SRGB view で自動デコード)。normal / metallicRoughness / occlusion / データテクスチャはリニア(UNORM view)。`.surface` の custom texture は `color_space: srgb` 宣言で SRGB になります(§6.7)。
2. **シェーダに gamma 補正を書かない**。書くと二重変換になります。
3. glTF の `baseColorFactor` / `emissiveFactor` / `COLOR_0` は**リニア値のままエンジンに入ります**(仕様どおり)。C++ 側でデバッグ色などを直書きするときは `Pelican::srgb(r,g,b)` ヘルパ([color.hpp](../../src/core/userpublic/color.hpp))。
4. RT の `format_class` を使うと実 format はリゾルバが決めます: `scene` → HDR 時 `R16G16B16A16_SFLOAT` / SDR 時 `B8G8R8A8_SRGB`、`display` → `B8G8R8A8_SRGB`、`data` / `explicit(...)` → 宣言した `format` をそのまま使う(リゾルバは触りません — depth や float16 の data RT も普通にあります)。
5. スワップチェーン・ヘッドレス出力とも既定 SRGB。SRGB 非対応デバイスのみ UNORM フォールバック(`output_transform` シェーダが OETF を適用)。capture / golden の PNG は「encoded-sRGB + リニア straight alpha」契約です(`test/golden/README.md`)。

移行の台帳(何をどの根拠で変えたか)は [../color_migration_manifest.json](../../docs/color_migration_manifest.json)(WP72 監査の機械可読成果物)にあります。`get_status` の `color` オブジェクトで実行時の契約(swapchain format / readback encoding 等)を確認できます。

## 6.4 シェーダ基盤(Shader Freedom Kit ✅WP10〜15/19 + M2b/WP70)

### stem 参照

**stem(ステム)** はファイル名から拡張子を落とした部分のことです([用語集](14_glossary.md))。シェーダは**拡張子なしの stem** で参照します(例: `"shaders/lava"`、`"engine://ssao"`)。`.vert` / `.frag` / `.spv` などの拡張子を書くと「use an extensionless <stage> shader stem」エラーになります(WP63)。

拡張子は stem ではなく、**その参照を書いた場所(どのシェーダステージか)**が決めます。`shader.vertex` に書いた `"shaders/lava"` なら `.vert`、`shader.fragment` なら `.frag`、compute の `shader` なら `.comp` です。そのうえで解決順は ① `<stem>.<ステージ拡張子>`(GLSL ソース → 実行時コンパイル)→ ② `<stem>.<ステージ拡張子>.spv`(ビルド済み SPIR-V)。つまり fragment に `"shaders/lava"` と書くと `shaders/lava.frag` → `shaders/lava.frag.spv` の順に探します。①の候補は**実行時コンパイラを含むビルドでのみ**列に入る(`PELICAN_RUNTIME_SHADER_COMPILER` — [第2章](02_getting_started.md))ので、配布ビルドでは実質 `.spv` だけです([shaderlibrary.cpp](../../src/core/shader/shaderlibrary.cpp#L587))。どちらも無ければ試行パス一覧付きエラー。

(**GLSL** は Vulkan/OpenGL 系のシェーダ言語、**SPIR-V** はそれをコンパイルした GPU 向け中間バイナリ形式です。)

> **設計決定(二層供給):** 開発時 = GLSL 実行時コンパイル(defines によるバリアント可)、配布 = `.spv` 固定 1 バリアント。`.spv` に `shader_defines` を組み合わせると「Shader defines and generated includes require GLSL source」エラー。feature を使う config は実行時コンパイラ必須。この stem 規約は web プロファイルと共有される可搬形式です([第9章](09_web.md))。

### 自動リフレクションとキャッシュ

**SPIR-V reflection(リフレクション)** は、コンパイル済みシェーダのバイナリを読み返して「このシェーダはどの番号にどんな資源を要求しているか」を機械的に取り出すことです(C++ 側に同じ宣言を手で書き写さずに済み、書き写しのズレも起きません)。`spirv-reflect` はそれを行う Khronos 公式のライブラリです([用語集](14_glossary.md))。

SPIR-V を spirv-reflect で解析し、**descriptor set layout・pipeline layout・push constant range を全自動生成**します。頂点+フラグメントで binding の型が食い違うと起動時エラー。パイプラインキャッシュ(`pipeline_cache.bin`)に加え、✅WP82 で **SPIR-V ディスクキャッシュ**(`<project>/.pelican/shader_cache/`、SHA-256 キー)が入り、2 回目以降の起動が大幅に短縮されます(破損時は再コンパイルへフォールバック)。

**fullscreen pass の入力と shader の宣言の契約(✅Q-14e・Q-14f)。**fullscreen pass(と入力を持つ raster pass)の入力は、set 1(`PELICAN_SET_PASS_INPUT`)の binding を `input` の位置で数えます —— texture の入力が binding 0 から、その後に buffer の入力が続きます(resource port の宣言も同じ番号に並びます —— 仮想 include `pelican_resource_ports.glsl` が `layout(set = PELICAN_SET_PASS_INPUT, binding = N)` で、画像は入力の index、buffer は画像の数 + i(shader が可変の組を宣言していれば `first + max + i`。下の「可変の入力」)。pipeline の作成時に port ごとに型・名前・次元が照合されます。graph から作った pass の `reads` は名前の順に並びます)。engine は pass を組むとき、入力ごとに、shader の reflection がその binding に要る種類の宣言を持つかを見ます。普通の texture の入力は combined image sampler、local read に選ばれた入力(§6.6 の「自動 tile-local attachment / same-pixel local read」)は input attachment、buffer の入力は storage buffer です([入力ごとの照合](../../src/core/fullscreenpass/fullscreenpasscontainer.cpp#L134))。見るのは reflection なので、`PELICAN_DECLARE_INPUT_<i>(<名前>)`(読むのは `PELICAN_TEXTURE_2D_<i>(<名前>, uv)`)で宣言しても、`layout(set = PELICAN_SET_PASS_INPUT, binding = <i>)` を手で書いても同じです。宣言が無い(か種類の違う)最初の binding で pass は組めず、編集 RPC では `render_pipeline_preflight_failed` に **`shader_input_mismatch`** `{pass, fragment, direction, kind, binding, resource, expected, pass_texture_inputs, pass_buffer_inputs, shader_pass_inputs}`(この向きの `direction` は `"pass_input_undeclared"`)が付いて断られ、文書は変わりません(鍵と例は [第13章](13_editor.md) §13.5「レンダー設定を編集する」)。message は `Fullscreen pass input texture/local-read contract does not match shader reflection: pass '<pass>' input <binding> ('<resource>') expects a sampled image at set 1 binding <binding>, but fragment '<fragment>' declares no pass input at that binding (shader declares <N> pass inputs)` の形です(buffer は `Fullscreen pass input buffer does not match shader reflection` で始まります。[型付きの拒否](../../src/core/fullscreenpass/fullscreeninputbindingmismatch.hpp#L122))。逆向き —— shader が、pass の渡す入力の数以上の binding を set 1 に宣言している —— は、engine がその binding を既定の resource で埋めます(✅Q-14f)。combined image sampler と separate image(2D / 2D array / cube の view)は 1x1 の黒 (0,0,0,0)、sampler は既定の sampler(linear / repeat)、storage buffer は 0 で埋めた 64 KiB の buffer です。配列の binding は全要素に同じ resource が入ります。入力が 0 個の fullscreen pass でも、shader が set 1 を宣言していれば set 1 を bind します(入力を持つ raster pass も同じ関数を通ります。入力の無い raster pass は通りません。[埋められる宣言](../../src/core/fullscreenpass/fullscreenpasscontainer.cpp#L100)、[既定の resource](../../src/core/fullscreenpass/fullscreenpasscontainer.cpp#L590))。見るのは reflection なので、宣言しただけで読まない binding も埋まります。storage buffer の block が 64 KiB より大きくても検出しません(reflection が block の大きさを持たないため)。埋めたときは編集は成功し、compile ごとに LOG_WARNING が 1 行出ます —— 下の拒否と同じ文(埋めた最小の binding について)の後に `; the engine binds a default resource (zeros) at set 1 binding(s) 1, 2` のように埋めた binding が続きます([警告を出す所](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L2496))。埋められない種類の宣言 —— input attachment、storage image、上に無い次元の画像(3D など)、uniform buffer、dynamic buffer、texel buffer、acceleration structure など —— が 1 つでもあれば pass は組めず、同じ `render_pipeline_preflight_failed` / `shader_input_mismatch` で断られます(ほかの binding が埋められる種類でも、全体が断られます)。このとき `direction` は `"shader_input_unsupplied"`、`binding` は埋められない宣言のうち最小の binding、`kind` は宣言の種類(`"texture"` / `"buffer"` / `"other"`)、`expected` は `"none"` で、`resource` は付きません。message は `Fullscreen shader declares a pass input the pass does not supply: pass '<pass>' supplies <M> pass inputs, but fragment '<fragment>' declares a uniform buffer at set 1 binding <binding> (shader declares <N> pass inputs)` の形です(M が 1 なら `pass input`。[message を組む所](../../src/core/fullscreenpass/fullscreeninputbindingmismatch.cpp#L32))。実行時サイズの配列(`[]`)は、engine の reflection が要素 1 つの配列として扱います(set layout も同じ数から作られます)。pass が渡さなければ要素 0 が既定の資源で埋まり、渡せば要素 0 に bind されるので、どちらでも layout の全要素が書かれます(断られるのは reflection がこの読み替えをやめた場合だけ。runtime 配列には descriptor indexing の有効化が要り、今の device の設定では pipeline を作れない可能性が高いので、専用の test は無く未確認です)。入力を足すなら shader の宣言も足します。Studio はこの拒否の横で、pass の fragment を入力がすべて可変の `engine://passthrough` に替えるか(✅VI-3b。以前は入力を読む pass-through の shader を書いていました)、足す行を clipboard に入れます(✅Q-14、[第13章](13_editor.md) §13.5)。逆向き(`shader_input_unsupplied`)の拒否には、Studio は宣言を消すよう言う 1 行(`<pass>'s shader (<stem>) declares an input the pass cannot supply (<型> at binding <b>). Remove that declaration from the shader.`)を出し、操作は Open the shader だけです(✅Q-15。`engine://passthrough` への切り替えは出さず、clipboard にも何も入れません。[第13章](13_editor.md) §13.5)。

**可変の入力(✅VI-1)。**fullscreen / raster pass の fragment は、固定の入力の後ろに**数の決まっていない入力の組**を 1 つ宣言できます。`#include "pelican_view.glsl"` の後に global scope で 1 回、`PELICAN_DECLARE_VARIADIC_INPUTS(first, max);` と書くと、set 1 の binding `first` 〜 `first + max - 1` に 2D の combined image sampler(`sampler2D`)が `pelican_variadic_input_<k>`(k = 0 〜 max-1)の名前で宣言されます。`first` と `max` は整数の literal(かそれに展開される macro)で、`max` は 1 〜 16(`PELICAN_VARIADIC_INPUTS_LIMIT`)です。読むのは `PELICAN_VARIADIC_TEXTURE(k, uv)` で、k はループ変数でもよく、`[0, max)` の外は `vec4(0)` を返します。pass が渡している組の数は `PELICAN_VARIADIC_INPUT_COUNT` で、これは preprocessor の数ではなく const int の式なので `#if` には使えません。engine は fullscreen / raster pass の shader を compile するときに `PELICAN_PASS_TEXTURE_INPUT_COUNT=<pass の texture の入力の数>` を define し、マクロはそれから `first` を引いて `[0, max]` に clamp した値を数にします([マクロ](../../src/core/resources/shaders/include/pelican_view.glsl#L176)、[define を足す所](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L2146))。組のうち pass が渡さない binding は Q-14f の既定の resource(0 の 1x1)で埋まり、これは意図した使い方なので LOG_WARNING は出ません(固定の宣言を埋めたときは今までどおり出ます)。engine は組を reflection の変数名の接頭辞 `pelican_variadic_input_` で見分けるので、専用の metadata は要りません([宣言を読む所](../../src/core/fullscreenpass/passinputlayout.cpp#L97))。

- **並べ方の規則**: 固定の texture の入力が先、可変の組は最後に 1 組だけ。組が 2 つある、組の後ろ(か組の binding の中)に固定の宣言がある、組の名前なのにマクロが作った形でない shader は、load と編集で `render_pipeline_preflight_failed` の `shader_input_mismatch`(`direction: "variadic_layout_invalid"` と `variadic_rule`)で断られます。pass の texture の入力が `first + max` 個より多いと `direction: "variadic_capacity_exceeded"` です(鍵と message は [第13章](13_editor.md) §13.5「可変の入力の拒否」)。
- **buffer の入力の番号**: 可変の組がある pass では、buffer の入力 i は texture の入力の数によらず binding `first + max + i` に入ります。組が無い pass は今までどおり texture の入力の数 + i です。この規則は 1 つの関数で、入力の照合・descriptor の書き込み・resource port の生成 include が同じものを使います([`passBufferInputBinding()`](../../src/core/fullscreenpass/passinputlayout.cpp#L200))。resource port の include は、いったん texture の数 + i で compile し、reflection に組が見えて終わりが違えば buffer を組の後ろへ移して compile し直します([登録し直す所](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L2171))。
- **raster の vertex を自前に替えても fragment は `engine://passthrough` のままでよい(✅RS-15)。**raster の vertex が pass の入力を固定の宣言(`PELICAN_DECLARE_INPUT_0` など)で読み、fragment の `engine://passthrough` が同じ binding を可変の組で宣言すると、RS-15 の前は 2 つの stage の reflection を合わせるときに固定の側の名前が残り、組に穴のある形として `Fullscreen shader's variadic pass inputs are malformed` で断られていました(fragment を先に替えると通る、設定の順に依る拒否)。今は合わせるときに組の名前を残すので([`merge()` の該当箇所](../../src/core/shader/shaderreflection.cpp#L367))、組は欠けずに読まれます。fragment が組を持たない固定の宣言なら今までどおり固定の名前です。test: `vi1_variadic_inputs_test` の [「RS-15 a raster vertex shader's fixed input and engine://passthrough's variadic group merge into one layout」](../../test/vi1_variadic_inputs_test.cpp#L1755) と [「RS-15 a graph raster pass with its own vertex shader and engine://passthrough draws」](../../test/vi1_variadic_inputs_test.cpp#L1786)(GPU)。
- 可変の組の宣言は `sampler2D` だけです。2D array や input attachment を要る入力(layered・multiview・local read)を組で受ける形はありません(そういう入力を組の binding に渡したときの挙動は test が無く未確認です)。

**pass ごとの `shader.defines`(✅VI-1)。**fullscreen pass と raster pass の `shader` は任意の `defines` を持てます。root の `shader_defines` と同じ `"NAME"` / `"NAME=VALUE"` の文字列の配列で、その pass の shader の compile に足されます([読む所](../../src/core/renderingpass/passshaderdefines.cpp#L61))。engine が pass の compile で自分で定義する名前(`PELICAN_PASS_TEXTURE_INPUT_COUNT` など)や、識別子でない名前、同じ名前の 2 回目は断られます。編集 RPC では `render_pipeline_preflight_failed` に `pass_shader_define {pass, define, name, index, reason}` が付きます(予約の名前の一覧は [第13章](13_editor.md) §13.5「pass ごとの `shader.defines` の拒否」)。`.spv` の shader には今までどおり defines を渡せません(`Shader defines and generated includes require GLSL source`)。ただし defines が engine の入れる `PELICAN_PASS_TEXTURE_INPUT_COUNT` だけなら、`.spv` は断られず define を読まずにそのまま使われます(compile 済みの module はこの define を読めないので、これだけを理由には断らない。[判定](../../src/core/shader/shaderlibrary.cpp#L331))。

**`engine://passthrough`(✅VI-1)。**engine 同梱の fragment で、入力はすべて可変(`PELICAN_DECLARE_VARIADIC_INPUTS(0, 8)`)です。`PELICAN_PASSTHROUGH_INPUT`(既定 0。pass の `shader.defines` に `"PELICAN_PASSTHROUGH_INPUT=2"` のように書く)番の入力をそのまま書き出します。入力が 1 つも無いときは UV の gradient `vec4(u, v, 0.5, 1)` を書くので、pass が動いていることが見えます。渡していない番号(組の中)を指すと既定の resource を読んで (0,0,0,0)、capture では黒になります([shader](../../src/core/resources/passthrough.frag#L22))。整数の target・配列・cube を渡したときの挙動は test が無く未確認です。blank 雛形の `present` はこの shader で描きます(✅VI-2。[第10章](10_tools.md) の `blank` 雛形)。

**各 pass の宣言は `get_frame_plan` で読めます(✅VI-1)。**fullscreen / raster pass の node の `pass_inputs` に、その pass が描いている shader の set 1 の宣言(binding・型・名前・可変か)、可変の組の範囲、pass が渡している入力の数、buffer の入力の始まりの binding が載ります(形は [第13章](13_editor.md) §13.5「各 pass の set 1 の宣言を `get_frame_plan` で読む」)。

**shader の読み直しも同じ照合を通ります(✅VI-1b)。**fullscreen pass と入力を持つ raster pass では、読み直した shader の宣言が pass の入力と合わない(上の Q-14e / Q-14f / 可変の規則のどれかで断られる)とき、その pass だけが新しい shader を断り、前の pipeline と descriptor set で描き続けます。同じ読み直しのほかの pass は新しい shader に替わります。受け入れた読み直しは、その pass の set 1 の descriptor set を新しい layout で作り直します。`reload_shaders` の `refused_passes`、reload status の `shader_reload_refused@1`、`pass_inputs.reload_refused` の形は [第13章](13_editor.md) §13.5「shader を読み直させる」。

### ホットリロード(✅WP14 → WP108 で全面刷新)

ウィンドウモードで実行中にシェーダや `.surface` を保存すると、**単一の FileWatcher 経路**([src/core/watch/](../../src/core/watch))が検出し、**アトミックなトランザクション**で反映します: 1 変更に影響する全 define/pass バリアント・リフレクション・依存パイプライン・`.surface` の vert/frag ペア・マテリアル layout/values をすべて準備してからフレーム境界で一括公開。どれか 1 つでも失敗すれば**全体を破棄して旧世代を維持**し、WARNING を出すだけです(旧来の「1 秒ポーリング」は廃止)。例外は ✅VI-1b の pass ごとの断りで、compile は通ったが fullscreen pass(と入力を持つ raster pass)の入力と宣言が合わない pipeline だけを外し、その pass は前の shader で描き続け、残りは公開します(§6.4 の「shader の読み直しも同じ照合を通ります」)。

テクスチャ(WP100)・マテリアル値(WP105)・モデルコンテナ .glb/.gltf/.vrm(WP110 — 配置済みインスタンスごと一括再構築)もリロード対象です(全体像は [第10章](10_tools.md) §10.7)。リプレイ / `--strict-assets` / rpc 駆動中はゲートにより無効、headless では自動ポーリングしません(✅HW-1 から、project.json の `hot_reload` に書いた素材とフォルダは rpc 駆動中も監視します。[第3章](03_project_format.md) の「hot reload で監視するもの」)。状態は `get_status` の `reload.*` で見えます。

### descriptor set 規約(shader_contract.md が正)

**descriptor set(デスクリプタセット)** は Vulkan で「シェーダに渡す資源(UBO・SSBO・テクスチャ)の束」に付ける番号で、GLSL では `layout(set = N, binding = M)` と書きます。pelican は set 0〜3 の意味を固定し、どの番号に何が来るかをエンジンとシェーダの契約にしています([用語集](14_glossary.md))。この契約の正本はリポジトリ内の [../shader_contract.md](../shader_contract.md)(`docs/shader_contract.md`)で、set/binding の全表・`engineMvp` の規範意味論・生成アクセサの ABI がそこにあります。以下はその要約です。

GLSL からは `#include "pelican_sets.glsl"` / `#include "pelican_frame.glsl"` で定数と構造体を共有します。

| set | 定数 | 用途(✅WP70 で統一済み) |
|---|---|---|
| 0 | `PELICAN_SET_FRAME` | **全パイプライン共通の固定 layout**: binding 0 `FrameUBO` / 1 `ObjectBuffer` SSBO / 2 `LightUBO` / 3 `PreviousObjectBuffer`(velocity 用) / 4 `FrameResolutionUBO`。graphics/computeの両方でbindされ、シェーダが宣言する場合は一致必須 |
| 1 | `PELICAN_SET_PASS_INPUT` | パス入力。fullscreen/compute/materialの通常経路はlogical nameからgenerated bindingへ解決。raw set 1は低レベルescape hatch |
| 2 | `PELICAN_SET_MATERIAL` | binding 0-3 標準 PBR テクスチャ、4-5 VAT(Vertex Animation Texture = 頂点の動きをテクスチャに焼いた形式。[第5章](05_assets.md) §5.4)、**6 = 全マテリアル配列の `MaterialBuffer` SSBO**(標準 96B + custom values 256B / 要素)、7 = occlusion、8 以降 = `.surface` の custom texture(宣言順) |
| 3 | `PELICAN_SET_FREE` | 名前に反して**大半はエンジンが所有**します: binding 0/1 = debug_draw / debug_text、2/3 = スキンパレット(現 / 前フレーム)、4-8 = morph(instance / weight / previous weight / metadata / delta)、9-11 = per-instance マテリアルオーバーライド(§6.8) |

`FrameUBO`(全シェーダから読める・368B): `time` / `dt` / 64bit `frame_index` / 64bit `view_family_token` / `resolution` + 逆数 / `camera_position` / `view` / `projection` / `previous_view` / `previous_projection` / `jitter_ndc` / `previous_jitter_ndc` / `temporal_reset_epoch` / `previous_temporal_reset_epoch` / `view_index` / `view_count` / `clip_plane`。GLSL上は`frame_index.xy`が論理frame番号、`.zw`がstable family tokenで、`pelican_view_family_token()`から読めます。projection jitter が無効な既定構成では jitter は `(0, 0)` です。

push constant は 128B(エンジン 64B + シェーダ 64B)で、✅**リフレクション段階で enforcement 済み**です(4B align・128B 上限・エンジン領域の部分使用をパイプライン作成前に拒否)。エンジン領域 offset 0..63 の `engineMvp` は**名前に反して MVP ではなく `view_projection_jittered`(world → jittered clip)**です — 独自 vertex シェーダは model 行列で作った world position にこれを 1 回掛けるだけにし、**jitter を後から足してはいけません**(§6.8 の projection jitter が二重適用になります)。この意味論・64B の範囲・symbol 名は v1 で凍結です([../shader_contract.md](../shader_contract.md) の「`engineMvp` v1 の規範意味論」)。エンジン頂点レイアウトは location 0=`inPos`, 1=`inNormal`, 2=`inTexUV`, 3=`inColor`, 4=`inTangent`。

エンジン同梱シェーダ(`engine://`、抜粋): `fullscreen` / **`passthrough`**(✅VI-1。上の「`engine://passthrough`」) / `ssao` / `ssao_blur` / `bloom_*` / `tonemap` / **`sky_ambient`** / **`output_transform`** / `shadow_depth` / `skinned` / `skinned_shadow_depth` / **`velocity` / `velocity_skinned`** / **`taa_resolve` / `taa_composite`** / **`sprite`** / `debug_draw` / `debug_text` / `default` / `vat` / `ui`、マテリアルテンプレート `shaders/material/surface_v1.{vert,frag}` + `standard_lighting.glsl` / `toon_lighting.glsl` / **`openpbr_lighting.glsl`**、**OpenPBR wrapper 6 種 `engine://surfaces/openpbr/*.surface`**、公開 include 群 `shaders/include/pelican_*.glsl`。

🚧 HLSL / Slang: `.surface` の `language` フィールドとして形式上は受理されますが、既定バックエンドは GLSL 以外を reject します(他言語は spv-link experimental の視野 — §6.7)。

## 6.5 feature — パージ可能な GPU 機能(✅WP28〜31/54/87/88/104/226)

**feature(フィーチャ)** は「RT・パス・compute タスク・シェーダ define をひとまとめにした断片 JSON(+ シェーダ)」で、rendering config の `features` 配列に参照を 1 行書くと、合成時にそのパスが既存のパス列へ挿し込まれます。SSAO や TAA のような描画機能 1 個ぶんの単位だと思ってください。**パージ可能**とは「参照を消せば機構ごと丸ごと消え、出力が feature 導入前と byte 単位で一致する」という意味です([用語集](14_glossary.md))。

> **設計決定(参照 = 存在):** feature は rendering config に**参照を書いたときだけ**存在する。1 つも書かなければ合成機構ごと素通りし、挙動は完全に不変(パージ可能)。golden テストの「feature off = 既存出力の完全維持」で保証されている。

> **設計決定(feature 層 = ユーザー空間、2026-07-12):** エンジンが持つのは**機構語彙**(anchor / history / snapshot / format_class など — 版付きで additive にのみ増える)だけ。feature(JSON + シェーダ)は**ユーザー空間**で、同梱 feature は**特権なしの標準ライブラリ**にすぎない。`engine://features/*.json` をプロジェクトへコピーして改造したら自分のもの、が公式ワークフロー。canonical anchor の全順序・`output_transform` 等の常設ノード・色 invariant・決定性ゲートだけは名前入りエラーで防衛される。

使い方は 1 行です:

```json
{ "features": [ "engine://features/shadow_directional.json", "engine://features/velocity.json" ], ... }
```

エンジン同梱 feature([src/core/resources/features/](../../src/core/resources/features/) の **21 ファイル**・✅すべて実装済み)。うち 20 本が `pelican.render_feature`、`editor.json` だけは `pelican.render_feature_overlay`(他の feature を並べて参照するオーバーレイ)です。表に出る業界用語を先に 1 行ずつ: **SSAO**(screen-space ambient occlusion = 深度と法線から「物が近接して光が届きにくい隅」を推定して暗くする画面空間の手法)、**bloom**(明るい部分をぼかして周囲へにじませる後処理)、**TAA**(temporal anti-aliasing = 毎フレーム微妙に投影をずらして描き、前フレームの結果と混ぜてジャギーを消す手法。§6.8)、**IBL**(image-based lighting = 環境画像を光源として使う照明)、**CSM**(cascaded shadow map = 距離帯ごとに解像度の違う影マップを使い分ける方式)、**cluster**(画面を格子に切って「その格子に効くライトの一覧」を先に作る手法)。いずれも [用語集](14_glossary.md) にも項目があります。

| feature | 内容 |
|---|---|
| `hdr.json` | `format_class: scene` の RT を float16 化(切替はエンジンの色リゾルバが feature の有無で行う)し、`scene_ldr_in` を挟んでトーンマップパスを `after:tonemap` アンカーに挿入 |
| `bloom.json` | `example` から移した 13 pass + 9 RT(`Bloom_*`、`format_class: scene`、`extent_scale` 付き)を `after:post_ldr` に挿入する後処理 bloom(P-2a-1b)。parameter `threshold` [0,4] 0.4 / `knee` [0,1] 0.7 は define(✅RS-8 から shader も同じ既定値を持ち、feature の外でも compile できる)、**`intensity` [0,8] 2.0 は `binding: "buffer"`**(define ではなく `bloom.intensity` という 4 byte の `runtime_f32` バッファの初期値。本節後半と §6.17)(P-2a-1c)。materialize した派生も parameter を編集できる(1c)。hdr on との併用は未対応(anchor が `post_ldr` 固定。hdr on で組むなら派生して `post_main` に) |
| `clustered_lighting.json` | computeでViewFamily/view別のcluster index/list bufferを構築し、standard lighting passへtyped buffer resourceとして注入。planar reflection併用時はreflection-local selectorも自動合成 |
| `shadow_directional.json` | 既定2048×2048・1 cascadeのdirectional shadow。1〜8 cascade、解像度、距離、split、安定化をパラメータ化し、`shadow_depth` と受光入力を追加 |
| `shadow_point.json` | 点光源の cube shadow。`$shadow/point` ViewFamily の 6 layer depth RT `point_shadow_map`(既定 1024×1024・`D32_SFLOAT`)と行列バッファ `point_shadow_matrices`(`host_source: view_family_matrices_v1`)を足し、`shadow_depth` パスを `begin` に挿入。`surface_resources` の `point_shadow` 契約で受光側に渡る。パラメータは `resolution` / `near_distance` / `far_distance` / `matrix_buffer_bytes` / `pcf_radius` / `bias` / `strength`。**`shader_define: false` を書いてあるのは前の 4 つだけ**で、`pcf_radius` / `bias` / `strength` は鍵を持ちません —— 既定の `binding: "define"` では `shader_define` の既定が **true** なので([featurecompose.cpp:1070](../../src/project/featurecompose.cpp#L1155))、この 3 つは `PELICAN_FEATURE_SHADOW_POINT_*` を吐き、**値を変えるとシェーダが再コンパイルされます** |
| `shadow_spot.json` | スポットライト版。同じ形で `$shadow/spot` ViewFamily・1 layer の `spot_shadow_map` と `spot_shadow_matrices`、契約名は `spot_shadow`。パラメータは shadow_point と同名・同既定(`shader_define` の有無も同じで、`PELICAN_FEATURE_SHADOW_SPOT_*` を吐くのは同じ 3 つ) |
| `rt_shadow_mask.json` | `pelican.vulkan.ray_query@1` を要求し、被覆付き `gbuffer_worldpos` と `gbuffer_normal` から TLAS へ法線方向 bias 付き hard shadow ray を飛ばして、`R8_UNORM` マスクへ出力。静的・skin・morph・VAT に対応。対象 0 件は全面可視。既定無効。TLAS は set 0、fullscreen 入力は set 1 |
| `rt_shadow_mask_pipeline.json` | `pelican.vulkan.ray_tracing_pipeline@1` を要求し、上の ray query 版と同じマスクを raygen / miss / closest-hit と SBT で専有 `R8_UNORM` storage image へ出力。既定無効で、両 feature は同時利用可能。画素テストは許容差 0 で完全一致を要求 |
| `sky_ambient.json` | `scene_depth` の遠クリップだけを塗る単色背景と、deferred/forward共通の単色環境光。色・ambient強度・sky強度はruntime parameter。IBLは含まない |
| `planar_reflection.json` | 指定world planeでmain viewを反転し、独立解像度のdeferred G-buffer/SSAO/lightingを`$reflection/planar` familyへ追加。結果をforward transparentの`planar_reflection` resource portへ割り当て |
| `cube_capture.json` | stable `$capture/cube` family の 6 面 sequential capture。`cube_capture_color`(現在 1 mip)を作る。cube 専用のパス種別は増やさない(本節後半)✅WP237 |
| `ui.json` | UI の GPU quad 描画(✅WP87。[第7章](07_input_ui.md)) |
| `velocity.json` | モーションベクタ RT(`R16G16_SFLOAT`)+ `velocity` パスを `before:post_main` に挿入(✅WP88) |
| `picking.json` | 視覚ピッキング用 `R32_UINT` ID RT + depth + `picking` パスを `before:post_main` に挿入。既定無効で、座標読み出しは §6.5「ID バッファ picking」(✅WP262) |
| `gizmo.json` | 移動・回転・拡縮の world-axis ハンドルを `before:debug_text` に重ねる。既定無効で、表示と状態なし当たり判定は §6.5「ギズモ」(✅WP274) |
| `taa.json` | **標準 TAA**(resolve + composite の二パス + halton23/8 の jitter provider + スカラーパラメータ。§6.8)✅WP113 |
| `sprite.json` | **純ゲート**(RT もパスも足さない)。参照すると `__anchor_sprite` でスプライトが描かれる(§6.9) |
| `debug_draw.json` | ワイヤフレームオーバーレイ(collider 可視化など) |
| `debug_text.json` | ビットマップ文字 HUD([第7章](07_input_ui.md)) |
| `editor.json` | **唯一の `pelican.render_feature_overlay`**(RT もパスも持たない)。`gizmo.json` / `picking.json` / `gpu_timing.json` の 3 本を並べて参照するだけの束ねで、`--feature-overlay` で editor 用の 3 機能を一括で足すために使う |
| `gpu_timing.json` | パスなしの計測フラグ。フレームグラフの**ノードごとに `barriers` / `body` の GPU タイムスタンプ**を取り、ログ・`get_status.gpu_timing`・ImGui に出す(✅WP29/143。XR では左右眼とミラーを別 view として分離。§6.14) |

fragment(`pelican.render_feature` v1)に書けるもの: `render_targets` / `buffers` / `compute_tasks`(追加。名前衝突はエラー。RT には `format_class` / `role` / `history` / `format_candidates` も書ける)、`render_target_overrides`(既存 RT の format/usage 上書きと `format_candidates` の重複なし追記)、`passes`(`insert: "begin" | "before:<アンカー|パス名>" | "after:<...>" | "end"`。`begin` は `rendering_passes` がちょうど 1 本のときだけ有効)、`pass_overrides`(既存パスへの `input` / `resource_ports` / `material_resources` 追加。この 3 キー以外は `render feature pass override has unsupported field '<key>'`)、`shader_defines`、`required_capabilities`(全 rendering graph の target planning 要求へ合流)、`runtime_shader_compiler`(`"required"` または `"optional"`。省略時 required。optional は define / 生成 include 不要の埋め込み SPIR-V feature 用)、**`parameters`(下記)**、**`surface_resources`(下記)**、**`projection_jitter`(§6.8)**、**`integrations`(下記)**、`required_targets`(✅FR-1。pipeline に要る target の型の宣言。下の「feature が pipeline に求める target と挿入先」)。`shadow_directional.json` の全文例は前版と同じです。

別featureとの組合せでだけ必要なfragmentは`integrations`へ置けます。全base featureを合成した
後に`requires`のfeature名がすべて存在するときだけ適用されるため、`features`配列の記述順へ
依存しません。縮小版featureなど任意passの有無も`requires_passes`で条件化できます。

```json
"integrations": [{
  "name": "planar_reflection_local_data",
  "requires": ["planar_reflection"],
  "requires_passes": ["planar_reflection_forward_opaque"],
  "fragment": {
    "pass_overrides": {
      "planar_reflection_forward_opaque": {
        "material_resources": {"selection": "reflection_selection"}
      }
    }
  }
}]
```

integration fragmentは通常fragmentと同じ名前衝突・field検証を受けます。条件に無いfeatureや
passへ暗黙fallbackしたり、既存bindingを上書きしたりはしません。

### feature パラメータと named binding(✅WP112/114/233)

feature は**パラメータ化**できます。`features` 配列は文字列のほかに `{ref, parameters}` のインスタンス形式を受理します:

```json
"features": [
  "engine://features/velocity.json",
  { "ref": "engine://features/taa.json",
    "parameters": { "scene_color": "lit_color", "alpha": 0.1 } }
]
```

- feature 側は `parameters`(`pelican.render_feature_parameters` v1)で宣言します: `render_targets`(name / required / default / role / format_class / usage / extent_scale / width / height / sample_count — **RT の named binding**)、`scalars`(`float` / `int` / `bool`。`default` 必須、float/int は `range: [min,max]` 必須、任意で `shader_define`)、`shader_assets`(name / `stage: vertex|fragment|compute` / default)。
- feature 本文の中では **`$<パラメータ名>`** プレースホルダで参照します(`$scene_color@history` も可)。未解決・型不適合は feature 名・パラメータ名入りの compose エラー。
- **プレースホルダと named binding は同じ機構の裏表**です。`$<名前>` は「feature 本文の書き方」、named binding は「`parameters.render_targets` 種のパラメータがやること = feature 内の呼び名を、プロジェクト側の実 RT 名に結び直すこと」を指します。同梱 `taa.json` の実物がその両方を見せます:

  ```json
  "parameters": { "schema": "pelican.render_feature_parameters", "version": 1,
    "render_targets": [ { "name": "scene_color", "required": false, "default": "lit_color", "usage": ["SAMPLED"] } ] },
  "passes": [ { "insert": "after:post_main", "pass": {
      "name": "taa_resolve", "type": "fullscreen",
      "input": ["$scene_color", "$velocity", "$depth", "taa_accum@history"], ... } } ]
  ```

  `scene_color` が named binding の宣言、`"$scene_color"` がその使用です。プロジェクトが何も渡さなければ `default` の `lit_color` に、`{"scene_color": "my_hdr"}` を渡せば `my_hdr` に置換されて合成結果に入ります。
- スカラーの `$名前` は fragment JSON の**どのスカラー位置にも書けます**。文字列が `"$名前"` **ちょうど一致**のときはパラメータの JSON 型がそのまま残るので、シェーダ define だけでなく**物理宣言も 1 つの値で駆動**できます。同梱 feature の実例:

  ```json
  "render_targets": [{
    "name": "shadow_map",
    "extent_scale": 1.0,
    "width": "$resolution",
    "height": "$resolution",
    "format": "D32_SFLOAT",
    "format_class": "data",
    "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"],
    "layers": "$cascade_count"
  }]
  ```

  `"$resolution"` は int の `resolution` に、`"$cascade_count"` は int の `cascade_count` に置換されるため、`{"cascade_count": 4, "resolution": 2048}` を渡すだけで 2048×2048 の 4 layer array target になります。文字列の一部としての `$名前` は置換対象ではありません(RT 名の named binding だけが `$scene_color@history` のような接尾辞付き参照を扱います)。
- `scalars` は `binding` も宣言できます(`"define"` | `"buffer"`、既定 `"define"`)。**`binding: "buffer"`** にしたスカラーは define ではなく、feature 自身が `buffers[]` に宣言した `host_source: "runtime_f32"` の 4 byte バッファ 1 本の**初期値**になります。バッファ名は **`<feature 名>.<パラメータ名>`** 固定で、シェーダは define ではなく `resource_ports` の port で読みます。同梱 `bloom.json` の `intensity` が実物です:

  ```json
  "parameters": { "scalars": [
    { "name": "intensity", "type": "float", "range": [0.0, 8.0], "default": 2.0, "binding": "buffer" } ] },
  "buffers": [
    { "name": "bloom.intensity", "size": 4, "lifetime": "persistent", "host_source": "runtime_f32" } ],
  "passes": [ { "pass": {
      "name": "FinalBloomComposite",
      "input": ["lit_color", "Bloom_Upsample_V_0_RT", "bloom.intensity"],
      "resource_ports": { "bloomIntensity": {
          "resource": "bloom.intensity", "kind": "buffer", "element": "float", "access": "storage" } } } } ]
  ```

  シェーダ側は `#include "pelican_resource_ports.glsl"`(合成時に生成される per-pass の port ヘッダ)を読み、`pelican_load_<port 名>(0u)` というアクセサで値を取ります —— 条件無しにアクセサを呼ぶシェーダでは、**その port を割り当てていないパスはアクセサの名前でコンパイルに失敗します**。✅RS-11(`5d065436`)から `bloom_composite` は `#ifdef PELICAN_RESOURCE_PORT_bloomIntensity` で読み分け、port の無いパス(port を書かない graph の `render.fullscreen` など)では `intensity` の既定 2.0 を使います([bloom_composite.frag](../../src/core/resources/bloom_composite.frag#L24)。define を出す仕組みは §6.6「compute タスク」の生成 include の段落)。なお同梱 bloom のシェーダは in-tree では **GLSL ソースだけ**で、`.spv` は生成物です(`src/core/resources/.gitignore`)。`.spv` しか無い環境のエンジンシェーダは define 付きの config では使えません(§6.11 の `Shader defines and generated includes require GLSL source, not SPIR-V`)。

  ✅RS-8(`5d065436`)から、`bloom_highpass.frag` は `PELICAN_FEATURE_BLOOM_THRESHOLD` / `PELICAN_FEATURE_BLOOM_KNEE` が定義されていなければ、`bloom.json` の既定値と同じ 0.4 / 0.7 を自分で定義します([既定値](../../src/core/resources/bloom_highpass.frag#L12))。feature の中では今までどおり feature の parameter の define が上書きし、feature の外(graph の `render.fullscreen`)では define 無しで compile できます。RS-8 の前は define が無いと `#error` で止まりました。名前は engine の予約のままで、pass の `shader.defines` に `PELICAN_FEATURE_*` を書くと今までどおり断られます(§6.4「pass ごとの `shader.defines`」)。shader の既定値が `bloom.json` と一致することは test が source から読んで比べます([「RS-8 / RS-11 the bloom shaders compile on their own with the bloom feature's parameter defaults」](../../test/vi1_variadic_inputs_test.cpp#L1468))。graph の fullscreen pass から `bloom_highpass` と、port の有無それぞれの `bloom_composite` を描く GPU の test は [同じファイルの次の case](../../test/vi1_variadic_inputs_test.cpp#L1517) です。

  `binding: "buffer"` を宣言したのに同名の `runtime_f32` バッファが無い/型が違う、`binding: "buffer"` と `shader_define: true` の併記、float 以外、`initial_f32` の手書き、未知の binding 名 —— どれも名前付きの compose エラーです。
- **`binding: "buffer"` の値は毎フレーム書き換えられます。** `pelican.graph` root(§6.16)の `render.scalar` ノードに `parameters.target` としてバッファ名を書き、その `value` 入力に曲線などを繋ぐと、publish のたびにその値が GPU バッファへ載ります(パスが読む前に書かれます)。`target` は宣言済みのフレームグラフバッファ名に解決され、無ければ `unbound frame graph output: <名前>` の hard error、サイズ/`host_source` が合わなければ `graph.render_scalar_target_incompatible@1` です(✅CT-5 から。前は名前の無い `incompatible frame graph output buffer: <名前>`。下の CT-5 の項)—— 綴り間違いが「動かない曲線」になることはありません。`set_parameters` で初期値を変えるのは再 compose を伴う編集操作、`render.scalar` は再 compose なしの毎フレーム駆動、と役割が分かれます。MG-2b から rendering config は `pelican.graph` root だけなので、どのプロジェクトでもこの駆動が使えます。
- **`render.scalar` は vec2〜vec4 と offset も書けます(✅RS-15、`7b16e9c8`)。**`parameters.value` に 2〜4 個の数の配列を書くと、その数を定数として順に載せます([読む所](../../src/core/userpublic/valuegraph.cpp#L266))。配列を書いた node は `value` の入力 edge を持ちません(scalar は今までどおり `value` の edge で受けます。catalog には edge の形が載ったままです)。`parameters.offset` は buffer の中の位置(byte。0 以上の 4 の倍数の整数、省略は 0)です。同じ buffer に書く `render.scalar` は何本でも置けますが、offset の順に並べたとき **byte 0 から隙間も重なりも無く**並ばなければならず、違えば `graph.render_scalar_layout_invalid@1` `{target, node, offset, expected_offset, reason: "overlap" | "gap"}` で断ります([検査](../../src/core/userpublic/valuegraph.cpp#L668))。値は float32 なので、graph の `render.buffer` / `render.attribute` で `element` が `float` / `vec2` / `vec3` / `vec4` / `mat4` 以外(`uint` など)の buffer に書くと `graph.render_scalar_type_mismatch@1` `{node, target, element, components}` です([検査](../../src/core/userpublic/valuegraph.cpp#L299))。配列が 2〜4 個でない、offset が 4 の倍数でない、といった形の誤りは code の無い `std::invalid_argument` です。同じ buffer の sink は毎フレーム offset の順に 1 回の書き込みにまとめて載せ、どれか 1 つでもその frame に値が無い(owner が非活性など)と、その buffer はその frame 書かずに unavailable(`render_buffer:<target>`)に数えます([載せる所](../../src/core/userpublic/valuegraph.cpp#L1057))。scalar 1 つで offset の無い今までの書き方の出力は変わりません。例: 5 要素の `float` の buffer `packed` に、`{"op": "render.scalar", "parameters": {"target": "packed", "value": [0.25, 0.5, 0.75, 1.0], "offset": 0}}` と、edge で値を受ける `{"op": "render.scalar", "parameters": {"target": "packed", "offset": 16}}` を置くと、`0.25, 0.5, 0.75, 1.0, <edge の値>` が 1 回で載ります。offset を 12 にすると `overlap`(`expected_offset: 16`)、20 にすると `gap` です。test: `curve_test` の [「RS-15 render.scalar writes a vec4 and a float into one buffer at their offsets」](../../test/curve_test.cpp#L199)。
- **`render.scalar` の書き込み先の条件と、合わない buffer の拒否(✅CT-5、`65f3a8a3`)。**
  - **前提**: `render.scalar` は毎フレーム host から float32 の値を buffer に書きます。なので書き込み先は `host_source: "runtime_f32"` で、大きさ(`size`)がその buffer に書く sink の合計(scalar 1 つが 4 byte、`value` の配列が 2〜4 個の数 × 4 byte)と同じでなければなりません([CT-5 の書き込み先の検査 `checkRenderScalarTargets()`](../../src/project/graphpartition.cpp#L276))。
  - `value` の線で繋いだ `render.buffer` / `render.attribute`(入力 edge `value` を持つ resource の node)は、partition が `size: 4` と `host_source: "runtime_f32"` を補います(`element` が `float` 以外、または別の値を書いていれば `scalar-connected buffer requires one runtime float: <id>`。[値の線で繋いだ buffer を補う所](../../src/project/graphpartition.cpp#L414))。なので補われる buffer はこの拒否に当たりません。
  - **graph の中の buffer**(graph の `render.buffer` / `render.attribute`)は、graph の partition の段で断ります。partition は編集でも読み込みでも通ります([partition から呼ぶ所 CT-5](../../src/project/graphpartition.cpp#L448))。`graph.render_scalar_target_incompatible@1` `{node, target, expected: {host_source: "runtime_f32", size?}, actual: {host_source, size?}}` で、`node` はその buffer に書く最初の sink(利用者が書いた sink を `$upload/` の sink より先に選ぶ)、`expected.size` は sink の合計(`value` の形が 2〜4 個の数でない sink があって合計が分からないときは付きません)、`actual.host_source` は書かれていなければ `null` です。partition は `size` を書いていない buffer の大きさを比べません。`render.attribute` の大きさは `count` × 要素の stride です(test)。`element` が float 系でない buffer はこの拒否ではなく今までどおり `graph.render_scalar_type_mismatch@1` です。
  - **graph の外で宣言された buffer**(feature の `bloom.intensity` など)は、partition では見ず、登録の段(世代を組むとき)で同じ code で断ります。どの sink が書いたかがそこでは分からないので `node` は付きません。`expected` は `{host_source: "runtime_f32", size}`、`actual` は `{host_source, size}` です([CT-5 の登録の段の拒否](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1357))。
  - `graph.` の code なので CT-2 の `document` も付きます(編集の ticket の rejected で確かめています。登録の段の拒否に `document` が付く道は CT-2 の規則のコードの読みで、test はありません)。
  - 例(test。`{"element": "float", "size": 8}` の buffer `data` に `value: [1.0, 2.0]` を書く): `{code: "graph.render_scalar_target_incompatible@1", node: "write", target: "data", expected: {host_source: "runtime_f32", size: 8}, actual: {host_source: null, size: 8}, document: "project://passes/main.json"}`。`size: 16` にすると `expected.size: 8`・`actual.size: 16` で断られ、`host_source: "runtime_f32"`・`size: 8` なら通ります。
  - catalog の `render.scalar` の `target` の `choices_from` にも同じ条件が `where` で載ります(§6.17 の CT-5 の段落)。
  - test: `curve_test` の [「CT-5 the catalog's remaining choices and a render.scalar writing an incompatible buffer」](../../test/curve_test.cpp#L1586)(partition の拒否の形、`render.attribute` の大きさ、整数の buffer と graph の外の buffer は partition が断らないこと)と、`renderconfigeditor_test` の [「CT-5 an edit writing a render.scalar into an incompatible buffer is refused by name」](../../test/renderconfigeditor_test.cpp#L1535)(編集の ticket の rejected、文書は書かれない)。`skeletal_gltf_test` は graph の 8 byte の buffer に 1 つの float を書く文書の読み込みが、この code で断られることを見ます。
- `scalars` の `shader_define`(bool・既定 `true`)を `false` にすると、そのパラメータは値付き define を**出しません**。物理 planning や runtime adapter から UBO へ送る値に付けます — `true` のままだと値を変えるたびにシェーダキャッシュキーが変わり、**全シェーダが再コンパイル**されます。同梱では `cube_capture.json` の 6 スカラー全部、`planar_reflection.json` の `prefilter_radius` 以外、`shadow_directional.json` の `cascade_count` 以外、および `sky_ambient.json` の 5 スカラー全部が `false` です。
- `shader_assets`は`shader`の型付きslotだけへ完全一致の`$name`として置けます。compute taskの文字列`shader`、またはgraphics passの`shader.vertex|skinned_vertex|fragment|compute`と宣言stageが一致しなければcompose時に拒否します。`skinned_vertex`は`vertex` stageです。instance値は`engine://`にも`project://`にもでき、feature JSONを複製せずalgorithm assetだけを交換できます。
- `shader_define: true` のスカラーは **`PELICAN_FEATURE_<FEATURE名>_<PARAM名>=<値>`** の値付き define に lower され、graphics/fullscreen/computeの全shader recipeへ渡ります(float は 9 桁 round-trip 表記。値を変えるとシェーダキャッシュキーも変わる = 正しく再コンパイル)。
- 解決結果は frame plan の `feature_instances` に出ます(`--dump-frame-plan` / Plan Viewer で確認可能)。

`materializeRenderFeatureDocument`はfeatureのパラメーターを具体値に展開した文書を返します。
入力configには、そのfeatureの合成位置で参照できるresource宣言を渡します。
戻り値は通常の`pelican.render_feature`として保存・再読込できます。**`parameters` 宣言は残ります**(✅P-2a-1c で変わりました。以前の版の「宣言を除去する」という記述は誤りでした):

- **`parameters` の宣言はそのまま残り、選ばれた値は `bound_parameters` オブジェクトに記録されます**([featurecompose.cpp:2859](../../src/project/featurecompose.cpp#L3343))。materialize 済み feature の parameter を後から編集できるのはこのためです
- **ただし残るのは `scalars` の宣言だけです。**`render_targets` と `shader_assets` の宣言は materialize が**食べてしまい**、展開後の文書には転記されません([featurecompose.cpp:1633](../../src/project/featurecompose.cpp#L1718))。束縛済みの値は `bound_parameters` 経由で runtime から見えます([featurecompose.cpp:1465](../../src/project/featurecompose.cpp#L1550))が、**「どの RT / シェーダを選べるか」という選択肢は派生の時点で固定**されます。
- **スカラーの値付き define は焼き込みません。** 宣言が残っているので、合成のたびに **その時点で効いている値から define が作り直されます**([featurecompose.cpp:2850](../../src/project/featurecompose.cpp#L3334))。焼き込むと古い `PELICAN_FEATURE_<F>_<N>=<旧値>` が新しい方の隣に残ってしまうためです。`shader_defines` に保存されるのは feature が自分で書いていた define だけです。
- **`$名前` プレースホルダも展開しません。** `"width": "$resolution"` のような**物理宣言側の `$名前` は文字列のまま残り**、後の編集が define だけでなく物理フィールドにも届きます(✅P-2a-1c の逸脱裁定)。
- **値の優先順位は `instance parameters` > `bound_parameters` > 宣言の `default`** です([featurecompose.cpp:1393](../../src/project/featurecompose.cpp#L1478))。つまり **`bound_parameters` は「派生 feature 自身の既定値」**であって、インスタンス側の上書きを禁じるものではありません(以前の版の「併記と instance 上書きは拒否」という記述は誤りでした)。
- RPC の `get_render_features` の `feature_instances[].parameter_schema` では、materialize 済みの feature の宣言の `default` は `bound_parameters` の値に置き換わります(この派生自身の既定値として見せるため)。✅SC-1(`14e10b19`)から、元の宣言の `default`(engine の既定)は `engine_default` で並びます。付くのは materialize 済みの feature の、元の `default` を持つ宣言だけで、`default` の意味は変わりません([`engine_default` を足す所](../../src/core/communication/renderconfigeditor.cpp#L1196))。例(test の期待値): `sky_ambient` の `color_r` を 0.7 で materialize すると `default: 0.7`、`engine_default: 0.12`。詳しくは [第13章](13_editor.md) の Render Features の Parameters... の段落。
- 拒否されるのは **2 度目の materialize** です。`bound_parameters` を既に持つ文書をもう一度展開しようとすると `Render feature '<名前>' is already materialized and cannot be materialized again` になります([featurecompose.cpp:2874](../../src/project/featurecompose.cpp#L3358))。
- 一方、**展開前の**パラメータ付き feature の pass を直接編集することはできません(`Parameterized feature passes must be edited in their source document.`)。materialize 済みなら pass は束縛済みなので、その場で編集できます([renderconfigeditor.cpp:76](../../src/core/communication/renderconfigeditor.cpp#L303))。

展開後はノード宣言やshader define、必要ならprovider用の`bound_parameters`を直接編集します。
`edit_render_features`の`materialize`操作は、この展開文書の新規保存と選択instanceの参照切替を
同じtransactionで行います。通常の合成順で確定したresource bindingを使います。
StudioではRender featuresで対象を選び、**Make editable...**で新しいprojectファイルを指定し、
**Apply**で保存・参照切替を行います。未適用の変更がある場合は先にApplyしてください。
展開後の通常passはFrame Planから既存のPass Editorで編集できます。
有効なintegrationが追加したpassやcompute taskも、出所のintegration fragmentへ保存できます。
条件判定を編集側でやり直さず、合成結果のprovider情報で元宣言を選びます。
対象featureが追加・直接上書きするpass、有効なintegrationが上書きするpass、surface resourceの接続先を含む
タグ付き領域では、差し替えproviderの指定も同じ保存で解除します。無効なintegrationは対象を増やしません。
対象領域は既存の差し替えregistryが対応するfullscreen pass構成です。
他の領域の指定は保持します。差し替え指定の保存先がengine presetの場合は先にFork presetしてください。
一般的なコンパイラ最適化は維持します。XR/preview向けの実行variantを独立した文書へ変換する操作ではありません。

**materialize した派生 feature は、プロジェクトの外へ「共有部品」として持ち出せます(✅P-2a-3)。** `export_render_feature{scope, index}` は `features` 配列の 1 インスタンスを 1 ディレクトリ `user://components/<feature の name>/`(`component.json` + `feature.json`)として書き出し、`import_render_feature` が別プロジェクトの `project://` へ取り込みます([renderconfigeditor.cpp:893](../../src/core/communication/renderconfigeditor.cpp#L1430))。持ち出せるのは **`project://` を一文字も含まない** feature だけで(含んでいれば `render_component_project_reference`)、`engine://` の同梱 feature は「既に共有されている」として `render_component_engine_reference` で断られます —— まず派生を作ってから、という順序です([rendercomponent.cpp:212](../../src/project/rendercomponent.cpp#L212)、`project://` 走査は [:240](../../src/project/rendercomponent.cpp#L240))。部品が要求する RT は `component.json` の `requires.render_targets` に並びます —— **「パスが使っている名前」から「feature 自身が `render_targets` / `buffers` で宣言した名前」を引いた差**なので、`bloom` を派生させて書き出すと `lit_color` と `swapchain` の 2 つが入り、feature 自身が宣言する `Bloom_*` と `bloom.intensity` は入りません([rendercomponent.cpp:163](../../src/project/rendercomponent.cpp#L163))。`swapchain` はエンジン所有なので、取り込み先で「無い」と言われることはありません。**形式・拒否 8 種・取り込み先の規則の本文は [第3章 §3.12](03_project_format.md)** にあります。ここで覚えておくことは 1 つだけ —— **共有できるのは「プロジェクトを指していない派生 feature」**です。

> **同じ綴りの別語に注意。** ここの **component** は `pelican.render_component`(feature 1 本を包んだ共有部品)で、[第4章](04_scene_ecs.md) の ECS の component とも、package の `provides.components` とも **別の語**です。同じく **requires** も 3 か所で別物で、`component.json` の `requires.render_targets`(この部品が要求する RT)/ feature の `integrations[].requires`(そのフラグメントを適用する条件の feature 名)/ `project.json` の `requires`(ビルド機能ユニットの要求。[第3章](03_project_format.md))は互いに無関係です([用語集](14_glossary.md))。

StudioのRender featuresでrootに指定したfeatureを選び、**Parameters...**を開くと、公開された
数値・bool・render target・shader assetパラメーターを編集できます。項目のチェックをONにすると
project側の上書きを指定し、OFFにするとfeatureの既定値へ戻ります。数値は宣言された型・範囲内で入力します。
ダイアログのOKは変更の準備で、Render featuresの**Apply**で保存・反映します。
engine内蔵featureでも公開パラメーターは変更でき、featureの元ファイルは書き換えません。
preset内のfeatureも一覧に`(preset)`付きで表示します。projectへforkしたpresetなら同じ画面で編集でき、
保存先はpresetのfeature instanceです。engine内蔵presetは先にFork presetでprojectへコピーしてください。

単色背景と環境光は preset を変更せず、次の feature 宣言だけで有効になります。
値を省略したときの非ゼロ既定値は `sky_ambient.json` だけが所有します。feature を外した場合は
背景 pass と define が消え、LightUBO の環境 radiance は zero になります。

```json
"features": [
  {
    "ref": "engine://features/sky_ambient.json",
    "parameters": {
      "color_r": 0.12,
      "color_g": 0.16,
      "color_b": 0.24,
      "ambient_intensity": 0.5,
      "sky_intensity": 1.0
    }
  }
]
```

値は linear-sRGB radiance で、5 scalar はすべて `shader_define: false` です。変更しても
shader variant は増えず、次フレームの LightUBO へ反映されます。現段階の ambient は
ライトが 0 灯でも非 emissive の形状を視認するための単色 fallback であり、cubemap、
irradiance、prefiltered environment、BRDF LUT は IBL feature の責務です。

directional shadowは同じ仕組みでCSMを有効化できます。指定を省けば従来互換の1 cascadeです。

```json
"features": [
  {
    "ref": "engine://features/shadow_directional.json",
    "parameters": {
      "cascade_count": 4,
      "resolution": 2048,
      "max_distance": 120.0,
      "split_lambda": 0.7,
      "stabilize": true
    }
  }
]
```

`cascade_count`は1〜8、`resolution`は64〜8192、`split_lambda`は0〜1です。
各cascadeはstable view ID `$cascade/N`を持ち、`shadow_map`の同じ番号のarray layerへ
sequentialに描画されます。splitはmain cameraのlinear depthで選択し、`max_distance`より
遠いsurfaceはshadow外として扱います。XRでもshadow familyは一つで、左右眼frustumの
unionを覆うため左右別にshadow mapを重複生成しません。world boundsが分かるdrawは
cascadeごとに保守的にsubmission cullingされ、境界交差またはbounds不明のdrawは残ります。

planar reflectionも同じparameter/provider境界で有効化できます。標準algorithm packageが
有効なら、stable `$reflection/planar` familyのproviderも自動登録されます。

```json
"features": [
  {
    "ref": "engine://features/planar_reflection.json",
    "parameters": {
      "resolution": 1024,
      "plane_x": 0.0,
      "plane_y": 1.0,
      "plane_z": 0.0,
      "plane_offset": 0.0,
      "preserve_raster_winding": true,
      "oblique_near_plane": true,
      "prefilter_radius": 1.0
    }
  }
]
```

plane式は`dot(normal, world_position) + plane_offset = 0`です。normalはruntimeで正規化され、
main familyの各viewからstable `$mirror/<source-view>`を作ります。標準surfaceはper-view
clip planeで反対側のgeometryを捨て、既知world boundsは同じplaneと反射frustumで
submission cullingされます。targetは`resolution × resolution`、2 layers固定で、flatでは
layer 0、XR sequentialでは対応eye layerを使います。同名`$reflection/planar` familyを
runtime callerが渡すと標準camera providerによる生成を置換できます。flatの通常出力でも
`Renderer::render(const RenderViewFamilies&)`へmainとsecondary familyをまとめて渡せます。

`oblique_near_plane`は既定で有効です。標準package providerは反射後の非jitter投影を
Vulkan forward-Zの`0 <= z <= w`規約で補正し、retained half-spaceのplaneをrasterizerの
near境界へ移します。透視、非対称XR、正射影、`preserve_raster_winding`のX反転を同じ
一般式で扱います。planeが反射cameraの前に無い、view方向へ後退する、またはfar面と
交差しない場合は元の投影を維持し、既存のCPU cullingとfragment clip planeだけを使います。
独自projection conventionを使うprojectはこのparameterを`false`にするか、
`$reflection/planar` family自体を提供できます。

標準featureは`deferred_geometry_v1`、`forward_opaque_v1`、`forward_transparent_v1`を
captureします。Deferred geometryはreflection用G-bufferとlightingを通り、Forward opaqueは
そのcolor/depthへloadして同じreflection familyから再描画されます。その直後のopaque
color/depth snapshotをreflection-local scene inputとしてForward transparentを再描画します。
transparent surfaceはcanonical passと同じsort providerを反射cameraごとに再評価するため、
main cameraと鏡映cameraで奥行き順が異なってもmain-view順序を流用しません
(ここでいう sort provider は §6.2 の `draw_sort` で選ぶものです)。

公開結果targetは`planar_reflection_color`で、canonical `forward_transparent` passでは同名の
`planar_reflection` material resource portへ自動bindingされます。reflection内のtransparent
passではこのportをopaque color snapshotへ切り替えるため、書き込み中のreflection attachmentを
同時にsampleしません。公開targetは7 mipを持ち、capture後に6個のimage-extent compute taskが
前段mipをroughness-aware 13-tap tent filterして後段mipを生成します。taskは`$reflection/planar` family単位で
実行され、flatのscalar imageもXRのfamily arrayも同じtyped portからloweringされます。

標準filterはtyped shader asset parameterなので、feature全体をコピーせず差し替えられます。

```json
{
  "ref": "engine://features/planar_reflection.json",
  "parameters": {
    "prefilter_shader": "project://shaders/my_planar_prefilter",
    "prefilter_radius": 1.25
  }
}
```

project compute shaderは同じ`source_color` / `filtered_color` port contractを使います。
`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF`で配布すると、標準assetに加えて
planar camera reflection、winding補正、oblique projection、runtime providerのC++実装も
source/objectごと除外されます。planar reflectionを使う全instanceで
`prefilter_shader`をproject実装へ指定し、callerから`$reflection/planar` familyを渡すか、
source buildしたhost側で同じstable IDのproviderを登録してください。graph compiler、compute task、
typed port、汎用ViewFamily resolver、scheduler、Vulkan backendはこの設定でも残ります。

surfaceはgraph/view形状を宣言せず、semantic portとsampling algorithmだけを書きます。
material pass側の`view: "family_array"`が`sampler2DArray` ABIを選び、
`pelican_sample_*()`は現在のlogical viewを自動選択します。必要ならview index付きoverloadも
利用できます。たとえばroughnessから標準mip列を読む最小形は次です。

```glsl
float lod = roughness * roughness *
    float(max(pelican_mip_count_planar_reflection(), 1u) - 1u);
vec3 reflected =
    pelican_sample_lod_planar_reflection(screen_uv, lod).rgb;
```

Fresnel、法線由来の歪み、GGX importance-sampled convolutionはmaterial/project側の
置換可能なalgorithmです。標準filterはmipに応じて半径を広げるboundedな7-level tent
low-passであり、方向空間の物理BRDF積分そのものではありません。

dynamic cubemap captureは標準`cube_capture` featureで有効化できます。

```json
"features": [
  {
    "ref": "engine://features/cube_capture.json",
    "parameters": {
      "resolution": 256,
      "position_x": 0.0,
      "position_y": 1.5,
      "position_z": 0.0,
      "near_distance": 0.1,
      "far_distance": 1000.0
    }
  }
]
```

標準providerはstable `$capture/cube` familyを作り、Vulkan cube layer順の
`$face/+x`、`$face/-x`、`$face/+y`、`$face/-y`、`$face/+z`、`$face/-z`を
`cube_capture_color`の6 faceへsequential描画します。captureは通常のDeferred geometry、
SSAO、lighting、Forward opaque/transparent passだけで構成され、cube専用pass typeは
ありません。clustered lighting featureと併用すると、6 view専用のlight selectionも
feature integrationから自動追加されます。

`cube_capture_color`は現在1 mipです。main materialへ暗黙bindingせず、利用するsurface/passが
typed image portを`resource: "cube_capture_color"`、`view: "cube"`として明示します。
BRDF-aware mip prefilter、複数probeの更新頻度/選択、capture結果の自動割当は技法ごとに
異なるため、後続の交換可能algorithmです。

callerが同じ`$capture/cube` familyを渡すと、標準の位置/六方向camera policyを全面置換できます。
`PELICAN_WITH_STANDARD_RENDER_ALGORITHMS=OFF`では標準providerのsource/objectが除外されますが、
feature schemaと汎用compiler/scheduler/backendは残ります。その構成でfeatureを使う場合は
caller-authored family、またはhostが登録する同じstable family IDのproviderを用意してください。

project featureでcanonical passと同じmaterial bindingを別target/viewへ再利用するときは、
composer helperの`"inherit_bindings_from": "<pass-name>"`を指定できます。全featureの
surface/material bindingが解決された後に継承されるため、shadowやclustered lighting featureの
記述順には依存しません。再描画pass側で明示したtarget、view family、load/storeは維持されます。
material passは参照元と同じ`material_contract`である必要があり、未知pass、cycle、種類不一致は
compile errorです。helperはcompose後に消えるのでruntime/backendのpass schemaには残りません。

clustered light selection v2はViewFamily/viewごとに独立します。標準bufferは最大2 view分を
持ち、各領域のheaderへtile情報、`view_index` / `view_count`、stable family tokenを書きます。
flatは先頭領域、XR sequentialは左右眼の各領域を使います。planar reflection併用時は
`$reflection/planar`専用buffer/taskがintegrationから追加され、scalable inventoryだけを
main viewと共有します。headerが現在のFrameUBOと一致しない場合はLightUBOへ安全側fallback
するため、別familyのselectionや未生成領域を誤用しません。cube capture併用時も同じ契約で
6-view専用selectionを作ります。secondary multiviewとcube capture用BRDF-aware prefilterは
後続です。

### feature が所有する material 入力契約 `surface_resources`(✅WP205)

feature が作った RT を、**material 側の公開アクセサとして配る**ための宣言です。
同梱 `src/core/resources/features/shadow_directional.json` の実物:

```json
"surface_resources": [
  {
    "contract": "directional_shadow",
    "resource": "shadow_map",
    "producer": "shadow_depth",
    "material_contracts": ["forward_opaque_v1", "forward_transparent_v1"],
    "fullscreen_consumers": [
      { "fragment": "engine://fullscreen", "uses_light_data": true }
    ]
  }
]
```

- 受理するフィールドは `contract` / `resource` / `producer` / `material_contracts` / `fullscreen_consumers` の 5 個だけです。
- **`contract` は v1 では `directional_shadow` の 1 つだけ**が公開契約です。ほかを書くと `does not name a public feature-owned material input contract`。任意の名前で自作契約を生やす入口ではありません(語彙を増やすときは語彙 WP としてエスカレーションする、という §6.5 冒頭の方針どおりです)。
- `material_contracts` に書けるのは forward 系(`forward_opaque_v1` / `forward_transparent_v1`)だけです(`material_contracts must name forward material contracts`)。
- `resource` が RT として存在しない、`producer` がその RT を書いていない場合は、feature 名とインデックス入りの compose エラーになります。
- 選ばれた consumer パスには合成後に `surface_resources` が**自動注入**されます。解決は全 feature がパスを挿し終わってから行われるため、**`features` 配列の記述順に依存しません**(producer が consumer の前に書かれていても後でも結果は同じ)。
- ✅RS-10(`5d065436`)。`fullscreen_consumers` を書いた宣言では、selector に合う fullscreen pass に加えて、**`input` にその資源の名前を挙げている fullscreen pass** を、fragment の shader が何であれ consumer に数えます(input は足し直しません。[判定](../../src/project/featurecompose.cpp#L2542))。どちらも無くても、`uses_light_data: true` の selector があり、`uses_light_data: true` の fullscreen pass(selector と違う fragment の、作者の lighting の pass。コードの注記では Studio の Deferred lighting の雛形)があれば、consumer が空のまま断りません([判定](../../src/project/featurecompose.cpp#L2555)、[断る所](../../src/project/featurecompose.cpp#L2790))。その pass に engine は input を足しません(shader がその入力を宣言しているとは限らないため)。その pass が影を受けるには、`input` に資源(`shadow_directional` なら `shadow_map`)を挙げ、shader がその入力を宣言している必要があります(Studio の雛形がそうなっているかは、この文書では確かめていません)。lighting の pass も無ければ、今どおり `did not match any material or fullscreen consumer` で断られます。ループの回の中で consumer を選び直すとき([`selectSurfaceResourceConsumersAmong()`](../../src/project/featurecompose.cpp#L2826))も、`input` に挙げた pass を consumer に数えます。test: `featurecompose_test` の [「RS-10 shadow_directional composes over a deferred pipeline with the author's lighting pass」](../../test/featurecompose_test.cpp#L9241)(作者の lighting の pass だけなら consumer 空で組める / `shadow_map` を挙げれば consumer になり input は足されない / lighting の pass が無ければ断られる)。
- material 側から見た受け取り口は `.surface` の screen input 契約 `directional_shadow` で、これは feature 専有のため `.surface` から直接 authoring できません(§6.7)。生成アクセサは `pelican_shadow()` です。

同梱 `shadow_directional.json` をプロジェクトへコピーして参照しても出力が **byte 一致**することは、golden の `shadow_b_layer_engine` / `shadow_b_layer_project` が同一 RGBA8 ハッシュで固定されていることで機械的に保証されています(`test/fixtures/wp73_rgba8_hashes.json`)。§6.5 冒頭の「同梱 feature は特権なしの標準ライブラリ」の実証です。

### canonical anchor(✅WP73)

**canonical anchor(標準アンカー)** は「パイプラインのこの位置」を指すためのエンジン定義の目印です。feature は挿入先に実パス名(プロジェクトごとに違う)ではなくアンカー名を書けるので、どの config へコピーしても同じ場所に入ります([用語集](14_glossary.md))。

ここでいう**実体化**は「描画するパスになる」ことではなく、**何も挿されていなくても `__anchor_<name>` という空ノードがフレームグラフに必ず 1 個ずつ存在する**、という意味です(プランのノード種別 `anchor`。後述 §6.6)。順序の基準点があるので、挿す側は基準点の存在を確かめずに `before:` / `after:` を書けます。

具体例 — 同梱 `engine://features/velocity.json` の挿入指定はこの 1 行です:

```json
{ "insert": "before:post_main", "pass": { "name": "velocity_pass", "type": "velocity", ... } }
```

`post_main` という名前のパスがプロジェクトに無くても、`__anchor_post_main` は常にあるので解決できます。

feature 合成は次の 8 アンカーを `__anchor_<name>` ノードとして常時実体化します:

```
sprite → post_main → tonemap → post_ldr → pelican_ui → debug_draw → debug_text → imgui
```

アンカーの全順序はエンジンの契約で、並べ替えはできません(挿すのは自由)。かつての「同梱 hdr が `before:present` を参照していて example で起動エラーになる」問題は、この標準化で解消済みです。

アンカーは feature の挿入先だけでなく、**プロジェクトが直接書いたパス自身の置き場所**にも使えます。Bloom のような authored pass は `canonical_anchor` で bucket を選びます —— hdr on では `post_main`(tonemap の前)、hdr off では `post_ldr`(tonemap の後)。`projects/example` は hdr off なので 13 本の bloom パスが `"canonical_anchor": "post_ldr"` を持ちます。`canonical_anchor` を書かないパスは、swapchain を書くもの(hdr on で `post_main`、off で `post_ldr`)を除いてシーンパスとして `post_main` より前に並びます。パス名は置き場所に影響しません(✅P-2a-1a。それ以前は `HighLuminanceExtraction` などの名前と `Bloom_` 接頭辞がエンジン側にハードコードされていました)。

シェーダ側の合流は従来どおり `#ifdef` バリアント方式(`PELICAN_FEATURE_SHADOW` 等)。feature を差し替えたいときは fragment をプロジェクト内にコピーして参照を書き換えます。

### feature が pipeline に求める target と挿入先(✅FR-1 / FR-2 / FR-3)

Graph で組んだ pipeline(Blank から始めたものなど)は、同梱の preset が宣言する target や pass を持たないことがあります。そこに feature を足して合成が通らないとき、engine は target も pass も黙って足しません(Graph の文書に見えない資源を増やさないため。仕様書の記述)。代わりに**何が要るかを名前付きで断り**、Studio が 1 回の編集で直せる材料を返します(FR-1 `45ca769b`、FR-2 `0b15fb62`、FR-3 `55390f4b`)。どちらの拒否も、編集の ticket では rejected の `error` に code と fields が並びます。

**要る target —— `required_targets`(✅FR-1)**

- `list_render_features` の各 feature に `required_targets: [{name, type, usage?}]` が付きます。feature が読み書きするのに自分では宣言しない target の一覧です([`renderFeatureRequiredTargets()`](../../src/project/featurecompose.cpp#L4284))。
  - 拾う所: pass の `output.color` / `output.depth` / `input`、`render_target_overrides` の鍵、feature 文書の `required_targets`。`$<parameter>` の名前はその parameter の `default` で読み、`<名前>@history` は `<名前>` として数えます。
  - 外すもの: feature 自身の `render_targets` / `buffers`、`swapchain` / `display`、`default` の無い parameter、integration の fragment。
  - `type` は `render.target_ref` に書く論理型です。文書の `required_targets` に同じ名前があればその型、無ければ depth attachment に使う target は `pelican.render.depth@1`(`representation: device`、`space: projection`)、それ以外は `pelican.render.legacy_opaque_image@1` です。
  - `usage` は feature の使い方(`COLOR_ATTACHMENT` / `DEPTH_STENCIL_ATTACHMENT` / `SAMPLED`)に、override と parameter の `usage` を足したものです。並びは名前が最初に出た順です。
  - 文書が読めない feature は `[]` です。
- 例(test): `sky_ambient` は `lit_color`(`["COLOR_ATTACHMENT"]`)と `scene_depth`(device の depth、`["SAMPLED"]`)の 2 件、`shadow_directional` と `ui` は `[]` です。`lit_color` の型は FR-1 の時点では推論の `legacy_opaque_image` で、✅FR-4 からは文書の宣言の `color_signal` です(下の「色の target の型の宣言」)。
- **feature 文書の `required_targets`**(任意の top-level の鍵)は、使い方から型を推せない target の型を書く所です: `[{"name": "<target>", "type": {"semantic": "...", "arguments": {...}}}]`。要素の鍵は `name` と `type` だけで、ほかの鍵・空の名前・`semantic` の無い型は `render feature '<名前>' required_targets ...` で断られます(code の無い例外。[FR-1 の宣言の検査](../../src/project/featurecompose.cpp#L1959))。同梱では `sky_ambient.json` が `scene_depth` を device の depth と宣言しています([sky_ambient の宣言](../../src/core/resources/features/sky_ambient.json#L77))。
- **色の target の型の宣言(✅FR-4、`2eb08d21`)**: 同梱の 3 つの feature が、Graph の color port と繋ぐ色の target を、文書の `required_targets` で `{"semantic": "pelican.render.color_signal@1", "arguments": {"reference": "scene", "transfer": "linear", "range": "extended"}}` と宣言しました。`sky_ambient` と `bloom` の `lit_color`([FR-4 の sky_ambient の lit_color](../../src/core/resources/features/sky_ambient.json#L79)、[FR-4 の bloom の required_targets](../../src/core/resources/features/bloom.json#L413))と、`rt_shadow_mask` の `gbuffer_worldpos` / `gbuffer_normal`([FR-4 の rt_shadow_mask の required_targets](../../src/core/resources/features/rt_shadow_mask.json#L46))です。
  - FR-4 の前は、この 4 つは推論で `legacy_opaque_image` になり、Graph の color port(`color_signal`)と型が合いませんでした。Studio は繋ぐときに `render.target_ref` の型を書き換えていました(仕様書の記述)。今は `list_render_features` の型をそのまま target_ref に書けば、lighting の pass の color port と同じ型です(test は fixture の `deferred_lighting` の color port の型と比べています)。
  - 推論の既定は変えていません。宣言の無い色の target は今も `legacy_opaque_image` です(test: `taa` の `velocity`)。`taa`(`lit_color` を parameter の `default` で使う)と `hdr`(`scene_ldr_in` を読む)は宣言していません。宣言しなかった理由は commit と仕様書には書かれていません(未確認)。
- **断る**: 合成は feature を当てる前に、要る target を pipeline・feature(integration の fragment を含む)・snapshot のどれかが宣言しているかを見て、最初に無いものを `feature.required_target_missing@1` `{feature, target, type, document}` で断ります([FR-1 で断る所](../../src/project/featurecompose.cpp#L1985))。`type` は上の `required_targets` と同じ型、`document` は feature 文書の参照(`engine://features/sky_ambient.json` など)で、CT-2 の root 文書の `document` とは別のものです。FR-1 の前は `render target override references unknown render target: scene_depth` の文でした。
- 直し方: target の宣言(rendering 節の `render_targets` の `scene_depth`、`D32_SFLOAT`)と、拒否の `type` を書いた `render.target_ref` を足すと、この拒否は消えます。ただし宣言しただけで誰も書いていない target は読めません。✅FR-3 からは、宣言と target_ref だけでは下の `feature.required_target_unwritten@1` で断られます(Blank から Graph で組んだ pipeline でもそうなることを test で確かめています)。通すには、前の pass の出力をその target へ繋ぎ替えます(下の「書き手」)。FR-1 の test も FR-3 で、前の pass の `output.depth` を `scene_depth` にして通す形に書き換えました。

**書き手 —— `needs_writer` と `feature.required_target_unwritten@1`(✅FR-3)**

宣言はあるが誰も描いていない target を feature が読むとき、FR-3(`55390f4b`)の前は、合成が通った後で frame planner が `Pass input target is not produced as an earlier output: scene_depth in pass: sky_background` の文で断っていました。今は合成が名前付きで断り、書ける pass を候補として返します。

- `list_render_features` の `required_targets` の各要素に `needs_writer: true | false` が付きます([FR-3 で needs_writer を決める所](../../src/project/featurecompose.cpp#L4404))。`true` は、feature の pass がその target をこの frame に読み(`input` の `SAMPLED`。`<名前>@history` の読みは数えません)、feature のどの pass も color / depth の出力として書かないものです。pipeline の誰かが先に描いておく必要がある target です。例(test): `sky_ambient` は `lit_color` が `false`(`sky_background` が描く)、`scene_depth` が `true`(読むだけ)です。
- **断る**: 合成の終わり(全 feature と fold group を当てた後)に、各 feature の `needs_writer` の target を見て、誰も書いていなければ `feature.required_target_unwritten@1` `{feature, target, type, writer_candidates: [{node?, pass, port, resource}], document}` で断ります([`requireFeatureTargetsWritten()`](../../src/project/featurecompose.cpp#L2072)、[FR-3 の検査を呼ぶ所](../../src/project/featurecompose.cpp#L4244))。`feature` / `type` / `document` は FR-1 の拒否と同じ意味です。
  - 見ない target: buffer、`initial` を持つ target(✅IA-1 の image は pass より前に中身を持つ)、宣言の無い target(FR-1 の `feature.required_target_missing@1` が先に断る)、その合成に feature の読む pass が無いもの。
  - 「書いている」の判定: pipeline の pass と compute task の宣言のどこかが、読む鍵(`input` / `reads` / `input_sampling`)と `name` / `after` / `before` 以外の所でその名前を持っていれば、書いているとみなします。コードの注記では、迷えば「書くかもしれない」に倒し、残りは frame planner の拒否に任せます。
  - feature の parameter に結んだ target(`taa` の `depth` など): ✅RG-3(`7a12fc2a`)から、この検査は parameter を束ねる前の feature の文書を、instance の値(resolved parameters)と合わせて読みます。`$<parameter>` は parameter の `default` ではなく結んだ先の名前になり、parameter の宣言の `usage` がその target の型に入ります([RG-3 で結んだ先の名前を読む所](../../src/project/featurecompose.cpp#L4333))。そのため depth の parameter(`usage` が `DEPTH_STENCIL_ATTACHMENT`)に結んだ target は `pelican.render.depth@1` として断られ、`writer_candidates` は depth を書く pass の出力です。RG-3 の前は束ねた後の文書を読んでいたので、parameter の宣言は効いていませんでした(差分の読み)。例(test): example の pipeline に `velocity` と、`depth` を誰も描かない `taa_bound_depth` に結んだ `taa` を足すと、`target: "taa_bound_depth"`、`type` は depth で、候補の `port` はすべて `depth`(`offscreen_depth` を書く出力を含む)です。
- **`writer_candidates`** は、feature の読む pass と同じ family で、その pass より前にある pass の出力のうち、同じ種類の target を書くものです。種類は、`type` の semantic が `pelican.render.depth@1` なら depth(`output.depth` を見る)、それ以外は color(`output.color`。2 つ目からは `color.1` …)です。書いている target の種類は、その宣言の format の先頭が `D` なら depth です。
  - 外すもの: `view_family` を書いた pass(shadow map など別の view を描く pass)、feature の pass、`swapchain` / `display` と要る target 自身を書く出力です([FR-3 で候補から外す所](../../src/project/featurecompose.cpp#L2139))。
  - `pass` は frame の pass の名前、`resource` は今書いている target です。root の graph の node が作った pass(instance やループの中ではないもの)で、その node の入力 port が `render.image` か `render.target_ref` を通してその target に繋がっていれば、`node` にその node の id、`port` にその graph の port の名前が入ります。それ以外は `node` が無く、`port` は `depth` / `color` / `color.<番号>` です。
  - 候補が無ければ空の配列です(test: depth を書く pass の無い pipeline)。
- **直し方(Studio の 1 回の編集)**: 候補の 1 つの `port` を、要る target の `render.target_ref` に繋ぎ替えます。例(test。UX-12b の GUI session が Blank から Graph で組んだ deferred の pipeline を fixture にしたもの): `sky_ambient` の 2 つの target と target_ref を足した後で `sky_ambient` を足すと、`writer_candidates` は `[{"node": "render_material_pass", "pass": "render_material_pass", "port": "depth", "resource": "render_material_pass__depth"}]` です(material pass が自分の `render.image` に描く depth)。`shadow_depth`(別の view で、feature の pass)は候補に出ません。ここで `edit_graph_document {ops: [{"op": "set_input", "node": "render_material_pass", "port": "depth", "source": {"node": "scene_depth_ref", "port": "resource"}}]}` を送ると通り、次の `sky_ambient` の追加が committed になります。frame plan では `render_material_pass` が `scene_depth` を書き、`sky_background` が読みます。実 Renderer を RPC で動かす GPU の test でも同じ結果です。
- 宣言・target_ref・繋ぎ替えのような複数の編集を 1 つの ticket で送るには、✅FR-3 の `edit_batch` を使います([第13章](13_editor.md) §13.5「複数の編集を 1 つの ticket で —— `edit_batch`」)。✅FR-4 から `edit_preset_pass` も batch に入るので、`sky_ambient` を足した後で空を present に映す 2 つの編集(lighting の pass の color を `lit_color_ref` へ繋ぐ `edit_graph_document` と、present の入力を `lit_color` にする `edit_preset_pass`)は 1 つの ticket で、`restore_render_documents` 1 回で戻ります(例は同じ節)。

**挿入先 —— `insertion`(✅FR-2)**

- `list_render_features` の各 feature に `insertion: [{pass, anchor, position}]` が付きます。`insert` が `after:<名前>` / `before:<名前>` の pass ごとに 1 件、文書の順です([`renderFeatureInsertions()`](../../src/project/featurecompose.cpp#L4411))。`begin` / `end` と、名前の無い `after:` は載りません。例(test): `sky_ambient` は `[{"pass": "sky_background", "anchor": "deferred_lighting", "position": "after"}]`。
- 挿入先の名前が pipeline に無いと `feature.insertion_anchor_missing@1` `{feature, anchor, candidates, document}` です([FR-2 で断る所](../../src/project/featurecompose.cpp#L2336))。`feature` は feature の名前(integration の pass ではヘッダの注記どおり `<provider>/<integration>`)、`anchor` は指した名前、`document` は feature 文書の参照です。
  - `candidates` は、その時点まで合成した pipeline で `after:` / `before:` がちょうど 1 つの frame node に解決する名前です。anchor は anchor 名、それ以外の pass は pass 名で、pipeline の並び(family の順、その中の pass の順)のままです([FR-2 の候補を作る所](../../src/project/featurecompose.cpp#L2293))。
  - FR-2 の前は `render feature pass insert anchor was not found: <名前>` の文でした。2 つ以上に当たる名前は今も code の無い `render feature pass insert anchor is ambiguous: <名前>` です。
- 例(test): `deferred_lighting` の代わりに `lighting_pass` を持つ graph の pipeline に `sky_ambient` を足すと、`candidates` は `["lighting_pass", "present", "sprite", "post_main", "tonemap", "post_ldr", "pelican_ui", "debug_draw", "debug_text", "imgui", "output_transform"]` で、どれを `after:` に書いた feature も合成が通ります。pass の名前を `deferred_lighting` に変えれば、元の feature のまま通ります。
- **断る順**: 1 つの feature では、要る target の検査が pass の挿入より先です([FR-1 の検査を呼ぶ所](../../src/project/featurecompose.cpp#L3710))。両方が欠けていれば `feature.required_target_missing@1` が出ます。FR-3 の `feature.required_target_unwritten@1` は合成の終わりの検査なので、この 2 つのどちらかが出る間は出ません。
  - feature の parameter に結んだ target(instance の値でも `default` でも)は、要る target の検査より前の、parameter を束ねる段で宣言の有無を見られ、無ければ `render feature '<feature>' parameter '<parameter>' target '<target>': unknown render target` で断られます([束ねる段で断る所](../../src/project/featurecompose.cpp#L1269))。だから FR-1 の `feature.required_target_missing@1` はこの target には届きません。この束ねる段の拒否は code の無い文で、まだ名前付きではありません(FR-5 の実測)。

test(FR-3): `renderconfigeditor_test` の [「FR-3 a feature's target that no pass writes is refused with the passes that could write it, and connecting one composes」](../../test/renderconfigeditor_test.cpp#L11819)(編集の ticket と、実際の Vulkan の program を device 無しで組む preflight を通す)と、`engine_primitives_test` の [「FR-3 sky_ambient on the Blank-built deferred pipeline names the material pass's depth as its writer, and connecting it publishes, through the running renderer」](../../test/engine_primitives_test.cpp#L3160)(RPC と実 Renderer。GPU)。

test: `featurecompose_test` の [「FR-1 a feature names the targets it leaves to the pipeline and is refused by name without one」](../../test/featurecompose_test.cpp#L9921) と [「FR-2 a feature inserted after a pass the pipeline lacks is refused with the names it takes」](../../test/featurecompose_test.cpp#L10119)、`renderconfigeditor_test` の [「FR-1 the catalog gives each feature's required targets and adding one without them is refused by name」](../../test/renderconfigeditor_test.cpp#L11521) と [「FR-2 the catalog gives each feature's insertion points and adding one whose anchor is missing is refused by name」](../../test/renderconfigeditor_test.cpp#L11591)(どちらも `edit_render_features` の ticket が rejected で、文書は書かれないこと)。

### ID バッファ picking(✅WP262)

視覚ピッキングはエディタ専用機能ではなく、通常の project 空間から選べる render feature です。

```json
{
  "pipeline": {"preset": "engine://render_pipelines/hybrid_v1.json"},
  "features": ["engine://features/picking.json"]
}
```

参照すると `picking_id`(`R32_UINT` / `COLOR_ATTACHMENT` + `TRANSFER_SRC`)、
`picking_depth`(`D32_SFLOAT`)と `picking_pass` が追加されます。参照を外すと三つとも
合成結果に存在せず、画像、パス、描画の GPU コストも発生しません。既定は無効です。

ID バッファの値は `0 = 背景`、`N + 1 = model instance slot N` です。slot 単独は再利用されるため、
読み出し側は描画を完了した同じフレームに保存した `{index, generation, scene_epoch}` と照合します。
その token から現在の authoring binding を引き、WP258 と同じ
`(scene_id, declaration_index)` と `authoring_object_id` へ解決します。`load_gltf` 由来など
authoring 宣言を持たない一時オブジェクトでは、runtime の `model_instance` token は返りますが、
三つの authoring field は `null` です。古い slot を別オブジェクトへ誤対応させることはありません。

座標は `picking_id` の左上を `(0, 0)` とする整数ピクセル座標です。読み出しは最後に完了した
フレームから 1 ピクセルだけ staging buffer へコピーし、同期して返します。クリックやデバッグ問い合わせの
ような低頻度操作で、要求した応答と同じフレームの ID を決定的に対応付けることを優先したためです。
高頻度 hover 用の非同期キューは v1 にはありません。汎用入口は
[`Renderer::readPickingPixel()`](../../src/core/vkcore/renderer.hpp)、外部ツール入口は
[`pick_object`](10_tools.md#基盤メソッド一覧) で、いずれも feature が無効なら明示エラーになります。

### ギズモ render feature と状態なし当たり判定(✅WP274)

ギズモも picking と同じくエディタ専用の裏口ではなく、通常の project 空間と汎用 RPC から
利用する render feature です。既定は無効で、次の参照を明示したときだけ存在します。

```json
{
  "pipeline": {"preset": "engine://render_pipelines/hybrid_v1.json"},
  "features": ["engine://features/gizmo.json"]
}
```

参照すると `gizmo_pass` が canonical `display` へ直接 overlay されます。透明な中間画像を
必要としないため feature-private RT は作りません。参照を外すと `gizmo_pass`、その pass pipeline
登録、`gizmo` 名の target は active graph に存在しません。最初から feature-off の構成では
module も upload buffer も生成せず、既存出力に描画・メモリコストを加えません。

表示は [`set_gizmo`](10_tools.md#基盤メソッド一覧) へ、`kind:declaration` の
`(scene_id, declaration_index)` または `kind:runtime` の世代付き `GameObjectId` と、
`translate` / `rotate` / `scale` の mode を送ります。
軸は world X/Y/Z、画面上の半径は 72 論理 px です。`selection: null` は表示を消します。
視線方向へ潰れて線分が 8 論理 px 未満になる移動・拡縮軸は、同じ軸色の菱形・矩形マーカーへ
切り替わります。ただし射影方向が定まらないため、そのマーカー上の drag query は `handle: null` を
返します。表示を残しつつ NaN や巨大な移動を起こさない退化規約です。
engine が保持するのはこの**表示要求**だけで、hover、押下、active handle、ドラッグ開始点、
ドラッグ中の値は一切保持しません。表示要求は保存されず、実行をまたいで不安定な runtime ID を
scene/project へ永続化しません。

当たり判定は [`query_gizmo_handle`](10_tools.md#基盤メソッド一覧) へ選択、mode、左上原点の
物理 pixel `(x, y)` を毎回すべて渡す純粋な問い合わせです。contract 2 の結果は、物理 pixel 差分を
論理 pixel 差分へ直す `content_scale` を含みます。handle は `translate_x` / `rotate_z` /
`scale_y` のような ID と axis、値が増える `drag_direction` と `value_per_logical_pixel`、
または `null` です。移動・拡縮の向きは投影軸、
回転の向きは掴んだ線分における正回転の接線です。移動量だけはカメラ距離に応じて変わります。
描画済み frame や `set_gizmo` の現在値には依存せず、問い合わせによって engine 状態も
変わりません。このため WP275 側は押下時に一度だけ問い合わせ、掴んだ handle とドラッグ状態を
Studio 側へ保持できます。

線のラスタ幅は Vulkan pipeline の既定どおり **1 物理 px**ですが、掴み代は線幅と切り離した
**半径 10 論理 px**です。window がある場合は
`framebuffer extent / logical window extent` の X/Y 比の平均を content scale とし、異常な値を
避けるため `0.5..4.0` に clamp して物理 pixel 半径へ変換します。headless または logical extent
不明時は scale 1 です。したがって 2x DPI なら半径 20 物理 px になり、1 px 線でも操作しやすさを
失いません。描画幾何と問い合わせは同じ投影関数を使います。

## 6.6 フレームグラフ(✅F0〜F2 = WP33〜35 + WP64)

**フレームグラフ(frame graph)** は「1 フレームぶんのパスと GPU 資源を、ノードと依存辺のグラフとして表したもの」です。DirectX 12 / Vulkan 世代のエンジンで広く使われている考え方で、pelican 固有の発明ではありませんが、**入力は rendering config JSON だけ**(C++ でグラフを組み立てる API は公開されていません)という点が pelican の形です([用語集](14_glossary.md))。

以下 **data edge(データ辺)** は「あるパスが書いた資源を別のパスが読む」という依存の辺を指します。対して `after` / `before` で人が書く辺を**明示エッジ**と呼びます。この 2 種類だけが順序の契約です。

> **設計決定(三層モデル):**
> 1. **正しさ** — データ依存(reads/writes)と明示エッジ(after/before)**だけ**が順序の契約。手書きバリアは禁止。
> 2. **機械最適化** — トポロジカルソート・層別・バリア導出はエンジンの仕事。
> 3. **手詰め** — JSON の宣言順は**安定タイブレーク**。依存が許す範囲で宣言順が尊重される。

三層を example の実プラン([test/fixtures/devstudio/example_frame_plan.json](../../test/fixtures/devstudio/example_frame_plan.json))で見ると:

1. **正しさ** — `gbuffer_pass` の `writes` に `gbuffer_normal` があり、`ssao_pass` の `reads` に同じ名前がある。この一致だけが「ssao は gbuffer の後」という契約で、JSON のどこにも順序は書きません。
2. **機械最適化** — その一致からエンジンが `{"from":"gbuffer_pass", "kind":"read_after_write", "resource":"gbuffer_normal", "to":"ssao_pass"}` というバリアを導き、`levels` に `[["gbuffer_pass"], ["ssao_pass"], ...]` の層を作ります。
3. **手詰め** — 依存が同点のノード同士は JSON の宣言順で決まります。プランの各ノードは `declaration_index`(JSON 上の位置)と `order`(実行順)の両方を持つので、宣言順がどこまで通ったかをダンプで確認できます。

依存はパスの `input` / `output`(+ `color_load_op: "load"` は reads 扱い、`@history` は**前フレームの面**を読むだけなのでフレーム内依存を作らない。§6.8)から自動導出されます。writes-writes 競合は起動時 hard error です。

✅WP64 で**実行系はプラン駆動に一本化**されました。旧レガシー経路(compute を含まない構成だけ JSON 宣言順で実行)は撤去済みで、全構成が FramePlan の順序で実行され、純 render 構成でも `before` / `after` 明示エッジが効きます。プランのノード種別は `render` / `compute` / `anchor` / `snapshot_copy` / `output_transform` です。

### compute タスク

compute シェーダは `<stem>.comp`、`reads` / `writes` の宣言だけでframe graphへ並びます。
画像の通常経路はoptional `resource_ports`で、read-only imageは`sampled`、write imageは
`storage`として名前を付けます。shaderは
`#include "pelican_resource_ports.glsl"`から`pelican_sample_<port>()` /
`pelican_store_<port>()`を使い、set/binding番号を書きません。computeでも
`#include "pelican_frame.glsl"`からtime/camera/resolution/lightを読めます。
portは既存の`reads` / `writes`を注釈するだけで、新しいedgeや順序を作りません。

**port の binding は shader の反映の名前で決まります(✅RS-5)。**compute task(下の RT pipeline task を含む)の resource port の set 1 の binding は、engine が shader を読み込んだ後に、その反映の set 1 で `pelican_resource_<port>` の名前を持つ descriptor の binding になります(RT task は raygen / miss / closest-hit / any-hit / intersection / callable の全 stage の反映を合わせて引きます。[名前で引く所](../../src/core/renderingpass/computetaskcontainer.cpp#L1388)、[`bindShaderResourceInterfaceByReflectedName()`](../../src/core/shader/shaderresourceinterface.cpp#L941))。descriptor set layout はその反映から作られ、descriptor の書き込みも同じ binding で port を引くので、両者は名前で一致します。したがって port を書く順(graph の `ports` の object の鍵の順、`reads` / `writes` の並び)は binding に影響しません。`pelican_resource_ports.glsl` の生成 include で compile される shader は include が振った binding がそのまま反映に出るので結果は以前と同じで、変わるのは binding を自分で書いた shader や事前 compile の shader を使う場合です(engine の `rt_shadow_mask_pipeline` の raygen は world position・normal・shadow mask を set 1 の binding 0・1・2 に書いています)。以前は port の並びの位置で番号を振っていたため、graph が鍵の名前順に並べると `Shader resource port '<port>' (resource '<resource>') reflected unexpected descriptor 'pelican_resource_<別の port>' at binding <n>` で断られていました。反映が名前を持たない port は番号を変えず、pipeline の作成時の検査が今までどおり `... is absent from shader reflection; include <pelican_resource_ports.glsl> and use its generated accessor` で断ります(RT では ray tracing の ABI の検査の後)。1 つの名前が 2 つの binding にあれば `Shader resource port '<port>' (resource '<resource>') reflected descriptor 'pelican_resource_<port>' at bindings <a> and <b>` で断ります。shader の hot reload では binding を決め直しません(登録のときの pipeline の記述のまま組み直します)。fullscreen / raster pass は変わっていません(binding は pass の入力の位置のまま。§6.4 の「fullscreen pass の入力と shader の宣言の契約」)。

**生成 include は port ごとに define を出します(✅RS-11、`5d065436`)。**resource port を持つ pass・task の shader を compile するときに生成する `pelican_resource_ports.glsl` は、port ごとに `#define PELICAN_RESOURCE_PORT_<port> 1` を書きます([書く所](../../src/core/shader/shaderresourceinterface.cpp#L667))。port を持たない pass では、engine に組み込まれた空の `pelican_resource_ports.glsl`([中身](../../src/core/resources/shaders/include/pelican_resource_ports.glsl#L4))で compile されます。そのため port を任意にした shader は `#ifdef PELICAN_RESOURCE_PORT_<port>` で既定値に切り替えられます(`engine://bloom_composite` の `bloomIntensity`。§6.5 の bloom)。条件無しにアクセサを呼ぶ shader は、port の無い pass では今までどおりアクセサの名前で compile に失敗します。RS-11 の前は、port の無い pass から `engine://bloom_composite` を compile できませんでした(EL-4 の一覧でも `reflection_error` でした)。

`view`は既定`shared_2d`、eyeごとのtargetは`per_view`です。sampled portの
`sampling.filter`は`linear|nearest`、`sampling.address`は
`repeat|mirrored_repeat|clamp_to_edge`です。raw storage buffer/image layoutは
typed buffer未実装時や特殊descriptor用のescape hatchとして維持されます。
`layers`がXRの論理view数より大きくても、layered `per_view` descriptorは選択した先頭の
論理view数ぶんだけを公開します。明示`subresource`は1 layerを起点としてlogical view数へ
展開するか、logical view数と同じ連続layer群を指定します。1枚再利用の
`sequential_2d`では`layer_count: 1`のままです。

2D RTの部分viewは`subresource`で指定します。

```json
"resource_ports": {
  "source": {
    "resource": "depth_pyramid",
    "access": "sampled",
    "subresource": {
      "mip": 0,
      "mip_count": 1,
      "layer": 1,
      "layer_count": 1
    }
  },
  "destination": {
    "resource": "depth_pyramid",
    "access": "storage",
    "subresource": {"mip": 1, "layer": 1}
  }
}
```

4 fieldの省略値は0/1/0/1です。sampled viewの`mip_count`は正整数に加えて
`"remaining"`を指定でき、base mipから現在のtarget最終mipまでを公開します。resizeで
`mip_levels: "full"`の実段数が変わっても再解決されます。同じimageを複数portへ割り当てる場合は、各portに
明示rangeとaccessが必要です。storageを含むrange同士のoverlap、範囲外、bufferへの
subresource、2D viewの複数layer、storage viewの複数mipは起動時エラーです。
material sampled portでも同じ指定を使えますが、subresource viewは`same_pixel` local readへ
変換されずsamplerとして実行されます。
generated image interfaceは`pelican_base_mip_<port>()`と
`pelican_base_layer_<port>()`も公開します。これはgraphが選んだ絶対base subresourceであり、
shaderがtask名やVulkan image viewを推測せず、mipごとのkernel parameterを決めるために使えます。
現在実行中のlogical viewは引き続き`pelican_view_index()`で取得します。
依存とlayout trackerは現在resource単位なので、rangeが離れていても実行順やbarrierを
勝手に緩和せず、image全体を保守的に遷移します。

runtime cubeを読むportは`view: "cube"`、`access: "sampled"`を指定します。省略した
subresourceはmip 0と全6 faceへ正規化され、明示時も`layer: 0, layer_count: 6`が必要です。
full mip chainを読む場合は`mip_count: "remaining"`を指定します。

```json
"resource_ports": {
  "environment": {
    "resource": "environment_capture",
    "access": "sampled",
    "view": "cube",
    "subresource": {
      "mip": 0,
      "mip_count": "remaining",
      "layer": 0,
      "layer_count": 6
    }
  }
}
```

generated interfaceは`samplerCube`と
`pelican_sample_environment(vec3 direction)` /
`pelican_sample_lod_environment(vec3 direction, float lod)`を作ります。同じ契約をfullscreenと
material sampled resource portでも使えます。cubeをstorageまたはsame-pixel local readとして
使う設定は現在hard errorです。`pelican_view_count_environment()`の6はdescriptorのface数で、
実行中ViewFamilyのcardinalityは引き続き引数なしの`pelican_view_count()`で取得します。

`schedule` は既定`per_frame`（logical frameで1回）または`per_view`
（logical viewごとに1回）です。`per_view`はXRのdepth pyramid、culling、eye別post-process
などに使い、他のscopeがmultiviewでも当該compute taskはsequentialに実行されます。
✅PS-1 から `once` も書けます。走る frame では `per_frame` と同じく view によらず 1 回 dispatch し、
入力か出力が変わった frame 以外は何も記録しません(§6.18「一度だけ走る pass と、frame をまたぐ image」)。
ほかの値は `compute task schedule supports per_frame, per_view or once: <task>` です。
shaderは`#include "pelican_frame.glsl"`の`pelican_view_index()` /
`pelican_view_count()`で現在のviewを取得します。

画像の現在extentからgroup数を決める場合は、typed image portを指定します。

```json
"dispatch": {
  "groups_from": {"port": "reduced_depth"}
}
```

portの`subresource.mip`における幅・高さをshader reflectionの
`layout(local_size_x=..., local_size_y=..., local_size_z=1)`で切り上げ除算します。
render target resizeとrebind時にも自動再計算されます。`local_size`をJSONへ重複記述は
できません。
設定とshaderの最小例は
[`adding_features.md`のレシピ4](../adding_features.md)と
`test/run_compute_headless.cmake`です。

RT pipeline task も最上位 `compute_tasks` を使います。これは `vkCmdTraceRaysKHR` を dynamic rendering scope の外へ置くための実行上の分類であり、fullscreen pass ではありません。

```json
{
  "name": "trace_mask",
  "ray_tracing": {
    "raygen": "shaders/trace_mask",
    "miss": "shaders/trace_mask",
    "closesthit": "shaders/trace_mask"
  },
  "reads": ["gbuffer_worldpos", "gbuffer_normal"],
  "writes": ["trace_mask"],
  "resource_ports": {
    "world_position": {"resource": "gbuffer_worldpos", "access": "sampled"},
    "normal": {"resource": "gbuffer_normal", "access": "sampled"},
    "mask": {"resource": "trace_mask", "access": "storage"}
  },
  "dispatch": {"rays_from": {"port": "mask"}}
}
```

stage stem は順に `.rgen` / `.rmiss` / `.rchit`（または埋め込み `.spv`）へ解決されます。`shader` との併記、compute 用 `groups` / `groups_from` の指定はエラーです。`rays_from` は指定image mipの幅・高さから実行サイズを決め、resize/rebind後に再計算します。callableは`ray_tracing.callable`、any-hit/intersectionはray group layoutから指定します。resource port の binding は、全 stage の反映を合わせた中の `pelican_resource_<port>` の名前で決まり、port を書く順には依りません(✅RS-5。上の「compute タスク」の「port の binding は shader の反映の名前で決まります」)。[加速構造とRT pipelineの説明](../source-code-guide/11_ray_query_acceleration_structures.md)も参照してください。

Callable recordの初期値は、StudioのRender Features → Callable Records...で編集できます。
taskとcallableを選び、表示された型に沿って値を入力し、Applyで保存・反映します。
整数はi32/u32、実数はf32、vectorは成分ごとに入力します。Reset taskは元宣言の値に戻し、
Applyでそのtaskの上書きを除きます。Refreshはローカルの編集を保存済み初期値で置き換えます。
保存先はprojectの`ray_task_callable_records`で、共有featureを変更しません。
ゲームコードが実行中に更新した値は、この初期値編集画面には表示されません。

画像と独立した3D領域を走査する場合は、代わりに次を指定します。値は正のuint32で、デバイスの各次元上限・総起動数上限も検査します。

```json
"dispatch": {"rays": [32, 16, 8]}
```

`rays`と`rays_from`は排他的です。`gl_LaunchIDEXT`と`gl_LaunchSizeEXT`は指定した3次元をそのまま反映します。画像への書込み座標はshader側で決めます。

ゲーム/updateスレッドからは、明示サイズのタスクを次のように変更できます。不正な値は現在の実行サイズを保持したまま拒否します。画像由来のサイズ変更とscene activation/destruction transaction中の呼出しは対象外です。

```cpp
context.setRayTracingDispatchExtent("trace_grid", 64, 16, 8);
```

GPUに起動数を決めさせるRT taskでは、`command_layout: "ray_dispatch"` のbufferと `dispatch.indirect` を使います。

```json
"buffers": [{"name": "ray_args", "size": 12, "command_layout": "ray_dispatch"}]
```

```json
"dispatch": {"indirect": {"buffer": "ray_args", "offset": 0}}
```

指定offsetから幅・高さ・奥行きのuint32を3個並べます。offsetは4-byte整列、コマンドは12 bytesです。bufferを書き込む独立したcompute taskをRT taskより前に宣言すると、frame graphがproducer→RT consumerの依存と間接コマンド読取りへのbarrierを作ります。`rays` / `rays_from` / `indirect`は排他的です。GPUが生成する値はCPUへ読み戻さないため、デバイスの次元・総起動数上限はproducer shaderで守ります。

この経路にはVulkanの`rayTracingPipelineTraceRaysIndirect`機能が必要です。非対応GPUでは`pelican.ray_tracing.indirect_unavailable@1`で拒否し、通常のdirect RTは引き続き使用できます。

RT taskごとに対象の集合を変える場合は、rendering configの`scene_acceleration_structures`へ
`{"name":"actors","instance_mask":1}`のような定義を並べ、taskの`ray_tracing`内で
`"acceleration_structure":"actors"`を指定します。instanceのmaskと指定maskのbit ANDが
非ゼロの個体だけが対象です。省略時は`"scene"`（シーン全体）。定義のmaskは0〜255で、
0は空の集合、省略時は255です。BLASとSBTは共有し、TLASと選択用descriptorを分けます。
compute shaderのray queryでも、taskへ`"ray_query":{"acceleration_structure":"actors"}`を
指定して同じ集合を選べます。この宣言でray query用のAS構築も有効になります。
シーン全体を使うcomputeでも`"ray_query":{"acceleration_structure":"scene"}`を明記します。
shaderだけがASを使用しtaskの宣言がない場合は、ロード・shader再読み込み時に拒否します。
詳細は[名前付きTLAS](../design_raytracing.md)を参照してください。

ray layoutを使う同じモデル個体のgeometryは、姿勢とinstance設定が共通でrecord行が連続なら、
エンジンが同じBLASへまとめます。
materialやgeometry flagsは個別に保持します。hit shaderの`gl_GeometryIndexEXT`はBLAS内の
geometry番号で、materialやgeometry tableの参照には対応するshader recordを使います。
変形する複数geometryもrefit対象です。
異なる位置・姿勢のtriangle個体もまとめる場合は、ray layoutの
`acceleration_structure`に`"triangle_space":"world"`を指定します。
mask・instance flags・custom indexとrecordの並びが結合条件を満たす範囲で、
world変換を各geometryのBLAS入力へ反映します。省略または`"geometry"`では従来の個体座標系です。
worldモードのhit shaderではobject座標がworld座標になり、ObjectToWorld/WorldToObjectはidentityです。
geometry tableの元頂点座標は変わりません。`gl_InstanceID`は結合後の番号です。
静的個体の移動はBLAS再構築、同じtopologyの変形はrefitで追随します。
この指定はAABBの座標系や結合条件を変更しません。
異なるtransformのAABBも結合する場合は、layout v3で`engine_record`に`"procedural_transform"`を追加し、
`acceleration_structure`へ`"procedural_space":"world"`を指定します。
layout v3で`engine_record`に`procedural_transform`を宣言済みなら、Scene AS画面の
**AABB coordinate space**からgeometry/worldを切り替えられます。三角形の設定と同時に保存でき、
既存のshader record配置は保持します。未対応のlayoutは編集不可の理由を表示します。
シェーダー側も以下の変換helperに対応させてから使用してください。
元の座標系で交差を計算するshaderは、`gl_ObjectRayOriginEXT`/`gl_ObjectRayDirectionEXT`の代わりに
`pelicanProceduralRayOrigin(record)`/`pelicanProceduralRayDirection(record)`を使います。返された方向を正規化しないでください。
組込み`procedural_sphere`はこの宣言に対応しています。同じassetを使う複数個体でも各transformを保持し、移動に追随します。
新しいengine recordを宣言するとshader recordのABIが変わるので、`PELICAN_RAY_ENGINE_RECORD_MEMBERS`を使って再コンパイルします。
未指定のgeometryモードと既存shaderの契約は変わりません。現行material_alpha_v1 profileとの併用は対象外です。
`{"name":"pelican_procedural_geometry","assets":["left_sphere","right_sphere"]}`のように、
一つのcomponentで複数assetを指定できます。単一指定の`asset`との併記はできません。
姿勢・instance設定・連続record行の条件を満たせば、
AABB geometryを同じBLASへまとめます。別objectの行を一つのTLAS instanceにはまとめず、triangleとの混合groupも作りません。
AABB boundsの変更はBLASを再構築し、recordだけの変更ではBLASを再利用します。

既存procedural assetのbounds・recordは、編集RPCから実行中に変更できます。
`get_procedural_assets({})`の`source_digest.hex`とasset名を使い、
`edit_procedural_asset`へ`base_source_digest`、`asset_name`、更新する`aabb`または`record`を渡します。
`aabb`は`min`/`max`、`record`はそのassetのrecord object全体を指定します。
返されたticketを`get_edit_result`で確認します。成功時はassets元ファイルへの保存とruntime更新を行い、
次の描画で変更が使われます。componentのSave sceneとは別の操作です。
名前・kindや他assetは保持し、ファイル競合・不正record・保存失敗時は現在の描画状態を保持します。
recordのshader型照合は現在sceneに装着したassetが対象で、未装着assetは装着時に検査します。
プレビュー中やrender pipelineの公開待ち中は編集できません。
新規作成は同じRPCに`operation: "create"`と未使用の`asset_name`、`aabb`、`record`を渡します。
削除は`operation: "delete"`、`asset_name`、`base_source_digest`を渡し、`aabb`/`record`は指定しません。
active sceneまたは他の解決済みsceneのcomponentが参照するassetは、保存前に拒否します。
先にcomponentの参照を外してください。削除も既存の編集結果ticketから確定を確認します。
`kind`は`aabb_v1`です。最初のprocedural assetを作る場合も使え、asset documentのversion 1は保存時に2へ更新します。
同名の定義は上書きしません。作成したassetはcomponentの選択候補に入り、装着時にshader recordとの適合性を検査します。
StudioではRender Featuresの **Procedural assets...** からassetを選び、
Min/MaxのX・Y・ZとRecord JSONを編集して **Apply** で保存・反映します。
**New...** で新しい名前を入力すると、選択中のAABB・recordをコピーした草稿を作ります。
空の一覧ではAABBを±1、recordを空objectとして開始します。編集後に **Create asset** で保存します。
保存済みassetは **Delete asset** で名前を確認して削除できます。未保存の新規草稿は **Reset selected** で破棄します。
assetを切り替えても入力内容は保持します。**Refresh / retry** は一覧と編集結果を取得し、
未保存の草稿とその元の保存世代は保持します。
**Reset selected** は選択中の草稿だけを、最後に取得した保存済みの値へ戻します。
保存済みの定義がない新規草稿は破棄します。
他のassetの保存によって競合した草稿も自動的に上書き・再送しません。

StudioのRender Features画面から「Scene AS...」を開くと、名前付きTLASの名前とmaskを
表で編集できます。Add／Removeで行を増減し、Applyで再コンパイルと保存を行います。
不正な定義や使用中のASを削除する変更は適用されず、失敗時は表の編集内容を保持します。
別の編集で元ファイルが変わった場合は競合として拒否します。Refreshはengineの現在値で
表を読み直す操作です。下段のRay task表ではtaskごとのASを選択できます。
接続先はprojectの`ray_task_acceleration_structures`に保存され、共有featureの設定は変更しません。
元のASへ戻すと、そのtaskの上書き指定を除きます。
Frame Planの **Order nodes...** では、名前付きASの構築とconsumerを交互に配置できます。
例えばleft AS→leftを使うtask→right ASという順序が可能です。
consumerを自身の接続先ASより前に置く変更は拒否します。ASを使わないcompute・通常anchor・copyは、
データ依存に反しなければ共有ASより前へ配置できます。
Frame Planの**Edit pass...**は、project presetに加えてproject内のfeatureファイルが明示的に宣言した
fullscreen・material・raster passを編集します。featureの保存は元のファイルに反映され、同じfeatureを使うgraphにも適用されます。
自動生成やパラメーター展開を伴うノード、engine内蔵featureなどは、対応する編集宣言がない理由を表示します。

Studioでprojectへforkしたpresetのmaterial passを編集すると、**Material contract**で描画契約を選択できます。
**Material settings (JSON)**では対象範囲・tag filter・variant・出力schema/state・resource宣言を編集します。
欄からキーを除くと、その設定を削除して既定動作へ戻します。入出力接続やray query設定も同時に保存できます。
**Preset material routing (JSON)**は全graph共通の`hybrid_auto_v1` routing表です。空欄にすると表を削除します。
contractとrouteの不整合、存在しないpass参照などは既存のpipeline検証で拒否され、適用に失敗した変更は保存されません。

material・fullscreen・raster・output_transformのshaderでscene全体のray queryを使う場合は、passへ`"ray_query": true`を指定します。
materialの宣言も省略時はfalseです。ASを読むsurfaceを使う場合は、そのmaterialが描画される各passに宣言してください。
ASを使わないmaterial passは、他のデータ依存に反しなければAS構築前にも配置できます。
trueのpassは共有AS構築後に実行し、falseのpassは共有AS前にも配置できます。
falseでASを使うshaderはロード・shader再読込時に拒否します。
固定shaderを使うspriteもAS構築を待ちません。色・depthを用意する描画など、通常の依存を満たせば
Frame Planの`__anchor_sprite`を共有AS構築より前に配置できます。
Studioのfullscreen pass編集では **Use scene ray query** で切り替え、保存で確定します。
既存project presetの編集時も元の値を読み込み、チェックを外すとfalseを保存します。
名前付きTLASを選ぶ場合は`"ray_query": {"acceleration_structure": "actors"}`のように指定します。
Studioの **Acceleration structure** 欄では`scene`（scene全体）または登録済みTLAS名を入力します。
passは共有ASと選択したTLASの構築を待ち、そのTLASをquery用descriptorへbindします。未登録名は保存・ロード時に拒否します。

frame graphには、共有BLASとdefault TLASを作る`__scene_acceleration_structure`と、
名前付きTLASごとの`__scene_acceleration_structure:<name>`が表示されます。
各名前付きnodeでそのTLASを構築し、以降のconsumerから利用できます。実行traceの`recorded`が
falseなら、その操作で新しいbuild commandは記録していません（既存ASや後続XR viewでの再利用）。

GPU が同じフレーム内で次の compute task の dispatch 数を決める場合は、12 byte 以上の
typed command buffer と `dispatch.indirect` を使います。

```json
{
  "buffers": [{
    "name": "dispatch_arguments",
    "size": 12,
    "command_layout": "compute_dispatch"
  }],
  "compute_tasks": [
    {
      "name": "build_dispatch",
      "shader": "shaders/build_dispatch",
      "writes": ["dispatch_arguments"],
      "dispatch": {"groups": [1, 1, 1]}
    },
    {
      "name": "consume_dispatch",
      "shader": "shaders/consume_dispatch",
      "dispatch": {
        "indirect": {
          "buffer": "dispatch_arguments",
          "offset": 0
        }
      }
    }
  ]
}
```

`command_layout: "compute_dispatch"` は3個の連続した `uint` (`x`, `y`, `z`)を表す
論理レイアウトで、エンジンが必要な indirect buffer usage へ lower します。`offset` は省略時
0、4 byte alignment 必須で、12 byte の command 全体が buffer 内に収まる必要があります。
`dispatch.indirect` 自体が frame graph の read edge を作るため、consumer shader がその
buffer を読まない限り `reads` へ重複記述しません。producer の shader write から
indirect command read への stage/access barrier もプランから自動発行されます。
同じtaskが自身のindirect bufferを書く構成はframe間の隠れたfeedbackになるため拒否され、
例のようにproducerを別taskへ分けます。
`groups` / image `groups_from` / `indirect`の併記は起動時エラーです。
`local_size`は常にshader側の宣言がauthorityです。

### バッファサイズを RT の extent から決める(`size_from_extent`)

タイル単位のバッファ(clustered lighting の selection buffer など)は、バイト数を手書きせず
**参照する RT の実 extent から導出**できます。`clustered_lighting.json` の実物:

```json
"buffers": [
  {
    "name": "clustered_light_selection",
    "lifetime": "persistent",
    "size_from_extent": {
      "resource": "lit_color",
      "tile_width": 32,
      "tile_height": 32,
      "header_bytes": 48,
      "bytes_per_tile": 260,
      "copies": 2
    }
  }
]
```

- 受理するフィールドは `resource` / `tile_width` / `tile_height` / `header_bytes` / `bytes_per_tile` / `copies` の 6 個だけです(ほかは `.size_from_extent has unknown field '<x>'`)。
- サイズは `resource` の extent をタイルサイズで**切り上げ除算**し、`header_bytes + tiles × bytes_per_tile` を `copies` 個ぶん確保します。`copies` は省略時 1 で、ViewFamily の view 数ぶん領域を分けたいときに使います(planar reflection 版は 2、cube capture 版は 6)。
- `tile_width` / `tile_height` / `bytes_per_tile` は正の整数が必須です。`copies` は省略可(既定 1)ですが、書いた場合は 0 を許しません。
- **`size` と `size_from_extent` の併記はエラー**(`cannot declare both size and size_from_extent`)。
- RT の extent が変わると再計算されるので、ウィンドウリサイズや `resolution` パラメータ変更にそのまま追随します。

### GPU が indexed draw 数を決める

同じフレームの compute task が material pass の indexed draw command と draw count を
生成する場合は、`indexed_draw` / `draw_count` の typed command buffer と
`gpu_draw_source` を使います。

```json
{
  "buffers": [
    {
      "name": "draw_candidates",
      "size": 40,
      "host_source": "scene_draw_commands_v1",
      "command_layout": "indexed_draw"
    },
    {
      "name": "draw_bounds",
      "size": 64,
      "host_source": "scene_draw_bounds_v1"
    },
    {
      "name": "visible_draws",
      "size": 40,
      "command_layout": "indexed_draw"
    },
    {
      "name": "visible_draw_count",
      "size": 4,
      "command_layout": "draw_count"
    }
  ],
  "compute_tasks": [{
    "name": "build_visible_draws",
    "shader": "shaders/build_visible_draws",
    "reads": ["draw_candidates", "draw_bounds"],
    "writes": ["visible_draws", "visible_draw_count"],
    "before": ["gbuffer_pass"],
    "dispatch": {"groups": [1, 1, 1]}
  }],
  "rendering_passes": [{
    "name": "main",
    "passes": [{
      "name": "gbuffer_pass",
      "type": "material",
      "material_range": {"start": 0, "count": 1},
      "gpu_draw_source": {
        "commands": "visible_draws",
        "count": "visible_draw_count",
        "max_draw_count": 2,
        "command_offset": 0,
        "count_offset": 0,
        "fallback": "cpu_draw_queue",
        "execution": "automatic"
      }
    }]
  }]
}
```

`indexed_draw` の1要素は Vulkan と同じ packed 20 byte
(`uint indexCount`, `uint instanceCount`, `uint firstIndex`, `int vertexOffset`,
`uint firstInstance`)です。`draw_count` は1個の `uint` です。両 offset は4 byte
alignment 必須で、`command_offset + max_draw_count * 20` と
`count_offset + 4` が各 buffer 内に収まる必要があります。GPU が書いた count が0なら
描画せず、`max_draw_count`を超えれば Vulkan command が上限で clamp します。
デバイスの`maxDrawIndirectCount`がより小さい場合もその値を上限にします。

`scene_draw_commands_v1` は、そのフレームのCPU DrawQueueを上記20 byte形式へ詰めた
候補列を供給する任意の host source です。compute shader はこの列をコピー、compact、
または書き換えて出力できます。ただし候補列にはmaterial境界などのsegment metadataは
まだありません。

`scene_draw_bounds_v1`はcommand候補と同じflattened DrawQueue順序のworld AABBです。
1要素は`vec4 minimum`、`vec4 maximum`の32 byteで、`minimum.w == 1`ならxyzが有効です。
deformation済みのboundsを毎フレーム公開します。安全なboundsを持たないcustom geometryは
`minimum.w == 0`になるので、culling shaderは推測で落とさず残してください。
commands/boundsのbuffer容量はそれぞれ独立しており、overflow時は各bufferの末尾を
ゼロ埋めしてpopulation診断を残します。consumerは両方のruntime array lengthの小さい方を
候補数にすると、異なる容量でも範囲外参照を起こしません。

full mip chainをsampled resource portで読むshaderでは、生成accessor
`pelican_sample_lod_<port>(uv, lod)`、`pelican_size_lod_<port>(lod)`、
`pelican_mip_count_<port>()`を利用できます。2D-array portのsample LODだけは
`(uv, view_index, lod)`です。scalar 2D portにも同じindexed overloadが生成され、
`view_index`はdescriptorが既に選んだ眼を表すため無視されます。このため同じshaderを
1枚再利用のsequential XRと2D-array loweringの両方で使えます。
depth pyramidのmax/min reduction、mip選択、biasなどの
アルゴリズムはengine固定ではなくproject側のcompute shaderで変更できます。
選択viewの絶対base位置が必要なら`pelican_base_mip_<port>()` /
`pelican_base_layer_<port>()`を使います。

既定の`gpu_draw_source.layout`は`"fixed_state_v1"`で、**1つの
`material_range` entryにpipeline、material descriptor、static/skinned vertex layoutを
固定**します。この場合`material_range.count`は1が必須です。

複数stateを処理する場合は次のように指定します。

```json
{
  "material_range": {"start": 0, "count": 4},
  "gpu_draw_source": {
    "layout": "draw_queue_segments_v1",
    "segments": "draw_segments",
    "commands": "visible_draws",
    "count": "visible_draw_counts",
    "max_draw_count": 256
  }
}
```

`scene_draw_segments_v1`は1要素32 byteの`uint32_t[8]`で、順に
`source_first_command`、`command_capacity`、`output_first_command`、
`output_count_index`、`sort_view_index`、`phase`（0=opaque、1=transparent）、
`visibility_view`（0=third person、1=first person）、`material_filter_index`
（`0xffffffff`=filterなし）です。各DrawQueue rangeは1つのsegment indexを持ちます。
view/filterが同じsourceを参照しても競合しないよう、output command範囲とcount slotは
segmentごとに分離されています。

segmented layoutの`max_draw_count`は、1 segment当たりではなく
`command_offset`以降に確保した**output command slot総数**です。count bufferには
`count_offset + (output_count_index + 1) * 4`までの容量が必要です。現在フレームの選択segmentが
segment host buffer、commands、countのいずれかに収まらなければ、rendererは一部のstateだけを
GPU実行せずpass全体をCPU DrawQueueへ戻します。

GPUが変更できるのは各CPU-bound segment内のgeometry/instance commandとcountです。
pipeline/material handleをGPUへ渡しておらず、CPU DrawQueueに存在しない新しいstateは
生成できません。project compute shaderはsegmentごとのcountを先に0へ戻し、
`source_*`から読み、`output_*`へcompactしてください。zero-filled tailと容量超過に備え、
全runtime bufferの`.length()`を境界として使います。segmentのsource範囲がcompute taskで
実際に読むcandidate commands/bounds host bufferへ収まるかはproject shader側の責務です。
dispatch数もsegment bufferの処理対象slotを覆う必要があります。任意のcompute graphから
入力bufferの意味やdispatch policyをengineが推測することはありません。

rendering configをホットリロードすると、segmented drawのcommands/count/segmentsを含む
GPU資源は新しいruntime generationへ一括差し替えされます。segment strideや容量が不正な
候補はpublishされず、直前のgraphとbuffer IDで描画を継続します。compute shaderファイル
だけを保存した場合はshader/pipelineだけをtransaction更新するため、graph generationと
buffer IDは変わりません。shader compileに失敗しても直前の有効pipelineを継続します。

`gpu_draw_source` はcommands/countをrender nodeのreadへ自動追加し、順序付け済みの
compute writeから`DrawIndirect` / `IndirectCommandRead` barrierを導出します。現プランナは
宣言順より後ろのwriterをRAW producerとして推測しないため、top-levelでrender passより後に
組み立てられるcompute producerには、例の`before: ["gbuffer_pass"]`または同等の明示edgeが
必要です。

`execution: "automatic"`はVulkan 1.2 coreの`drawIndirectCount` featureが有効なら
`drawIndexedIndirectCount`を使います。未対応デバイスでは`fallback: "cpu_draw_queue"`により
既存CPU DrawQueueを描画します。比較・診断用の`execution: "cpu"`も同じfallbackを強制します。
現時点で他のfallbackとVulkan 1.1 + `VK_KHR_draw_indirect_count`だけの経路は受理しません。

Studio では ✅UX-15a から、`gpu_draw_source` を material pass の form(Pass Editor と Graph の material pass の欄)の **GPU draws** の欄で書きます。layout は選択、commands / count / segments は宣言済みの buffer(Graph では `render.buffer` の node)から選び、JSON は打ちません([第13章](13_editor.md)の「gpu_draw_source の欄(✅UX-15a)」)。

### プランダンプ(実行計画の可視化)

```sh
pelican_player --project mygame --headless --dump-frame-plan   # stderr に出力
# または rpc の get_frame_plan。GUI では ImGui の Plan Viewer(第10章)
```

出力にはノードごとの `order` / `level` / `reads` / `writes`(`@history` 読みは `reads_history`)と導出された `barriers`、`snapshot_copy` ノードが含まれます。

renderer から取得する文書は `"profile": "runtime"` を持ち、全ノードに
`shader_resolution` を付加します。これは raw planner の `framePlanToJson` には含まれません。
パス実装 provider がシェーダを差し替えても、typed parse 時の宣言と実効値を別々に確認できます。

```json
"shader_resolution": {
  "state": "resolved",
  "stages": [
    {"stage":"vertex", "declared_ref":"shaders/authored",
     "effective_ref":"shaders/provider_vertex", "origin":"provider",
     "source_open_ref":"project://shaders/provider_vertex.vert"}
  ]
}
```

`origin` は `authored` / `engine_default` / `provider` / `generated` です。
`declared_ref` は stage が元 JSON に存在した場合だけ出ます。provider は identity passthrough
以外では fullscreen の vertex/fragment pair の両方を `provider` とします。
parser が省略を補った stage だけが `engine_default` です。ray の `miss` / `closesthit` は
authored 順の `{stage,index}` エントリです。

| pass family | state | stage |
|---|---|---|
| material | `material_owned` | material shader の掘り下げは別契約 |
| fullscreen | `resolved` | vertex, fragment |
| raster | `resolved` | vertex, optional fragment |
| output_transform | `resolved` | vertex, fragment |
| debug_draw | `resolved` | vertex, fragment |
| gizmo | `resolved` | vertex, fragment |
| debug_text | `resolved` | vertex, fragment |
| shadow_depth | `resolved` | vertex |
| velocity | `resolved` | vertex, skinned_vertex, fragment |
| picking | `resolved` | vertex, skinned_vertex, fragment |
| ui | `resolved` (`engine_fixed`) | vertex, fragment |
| imgui | `resolved` (`engine_fixed`) | vertex, fragment |
| canonical_anchor | `not_applicable` | — |
| snapshot_copy | `not_applicable` | — |
| compute_task | `resolved` | compute、または raygen + miss[] + closesthit[] |

`source_open_ref` は拡張子付きの正規論理参照(`project://` / `user://`、asset store の
mount 名も論理パス内に保持)だけです。絶対パスは wire に出ません。Studio の「開く」は
同じ project resolver で押下時に物理パスへ変換します。embedded/generated/missing source は
`null` と `embedded_engine_resource` / `generated` / `source_not_found` を表示し、ボタンを無効にします。
Studio と ImGui Plan Viewer は runtime profile 以外を受理しません。
request-local な preview graph はこの renderer runtime 投影の対象外です。

**同じ世代の `get_frame_plan` は、世代で決まる部分を使い回します(✅FP-1、`eab549b6`)。**返り値の形と bytes は変わりません。renderer は返り値を、公開した世代とその program で決まる部分と、呼ぶたびに読む部分に分けます。前者はその世代で最初に呼ばれたときに 1 回だけ組み、以後は写しに後者を足して返します([`framePlanGenerationPartJson()`](../../src/core/vkcore/renderer.cpp#L4156))。

- 世代で決まる部分: `framePlanToJson` の plan(ループ・`diagnostics`・fold の決定を含む)、`execution_plan`、`gpu_owner_scope`、`window_output`、`gpu_resource_arena`、`physical_target_plan`(と `from_nodes`)、`native_scope_executors`、`sample_count_plan` など。
- 呼ぶたびに読む部分: `shader_resolution`(source の file が在るかは今の file system)、`preview`、PS-1 の `last_run_*`・MP-1 の `drawn_materials`・LP-3a の draw_loop の要素など frame で変わる node の field、material filter、`runtime_resolution`、照明(`lighting_data_runtime`)、`sprite`、`initial_asset` など。
- 組み直すのは、世代(の object)・世代の番号・rendering pass の id・rendering の root の参照のどれかが変わったときです([FP-1 で組み直しを決める所](../../src/core/vkcore/renderer.cpp#L4355))。
- commit の記録の計測: Debug の player、約 197 KB の返り値で、往復 190〜222 ms → 68〜84 ms(15 回の中央値、前後を交互に)。前後 6 組の返り値は byte 一致でした。test: `engine_primitives_test` の [「FP-1 get_frame_plan builds its generation's part once and reads a frame's part on every call」](../../test/engine_primitives_test.cpp#L2507)(GPU)。

**`execution_plan`(🚧WP238a)** — 同じダンプに backend 非依存の共通 IR が併記されます
(`schema: "pelican.frame_execution_plan"`)。ノードごとの semantic dialect、選択された implementation と
endpoint、required / provided capability、resource use(current / previous epoch × read / write × footprint)、
依存、bridge、fingerprint が見えます。endpoint の `class` は `host` / `device` / `external` の
3 値だけが閉じた語彙で、queue も `backend` 名も open な ID です。

この IR は**現時点では観測面**です。パブリッシュ時に FramePlan と graph 名・ノード数・
`order` / `level` / `view_family` が一致することを検証されますが、
**実行順・level・barrier の権威は今も FramePlan(`planFrameGraph()`)側**にあります。
また rendering config に書くキーはありません。

**`native_scope_executors`(🚧WP238d)** — 検証済みの完全 physical package が
1 個以上の physical scope を「provider が自前で command を記録する NativeScope 実行」へ
切り替えたときだけ出ます。complete-plan fingerprint、scope と implementation の identity、
provider の owner / generation、capability、synchronization mode(`automatic` / `manual` / `unchecked`)、
queue capability、lowered resource kind が見えます。scope が覆った区間では、通常の
render / compute / transfer / output / marker のノード本体は provider の callback へ置き換わります。

> 🚧 NativeScope はこの時点では **source-level の拡張**です。rendering config に書くキーはなく、
> **ゲーム DLL 向けの raw Vulkan callback ABI も公開されていません**。同梱の builtin provider は
> `builtin.vulkan` 1 本で、その唯一の implementation が空マーカー `builtin.vulkan.noop_marker@1` です。
> この境界に乗る production の描画機能はまだありません
> (正: [../design_render_graph_compiler.md](../design_render_graph_compiler.md) /
> [../design_heterogeneous_execution_graph.md](../design_heterogeneous_execution_graph.md))。

### target planning と Vulkan plan pin

ここから **physical(物理)計画**の話に入るので、先に logical との線引きを置きます。

> **分離基準:** 実デバイスの能力を知らなくても決まることが **logical(論理)plan**、実デバイスの能力を見ないと決められないことが **physical plan**([用語集](14_glossary.md))。

具体例 — rendering config に `gbuffer_albedo` という RT を宣言し、`gbuffer_pass` が書いて `lighting_pass` が読むと書いたとします。

- **logical plan** に載るのは「`gbuffer_albedo` というノードと資源があり、その 2 パスの間に read-after-write の辺がある」ところまでです。GPU が何であっても同じ答えになります。
- **physical plan** に載るのは「その `gbuffer_albedo` に実際どの VkFormat を割り当てるか」「`materialized_image` / `transient_attachment` / `tile_local_attachment` のどれで実現するか」「どのノードを 1 つの Vulkan dynamic rendering scope にまとめるか」「どの資源同士がメモリ確保を共有(alias)してよいか」です。同じ config でも GPU が違えば違う答えになり得ます。

この線引きは fingerprint にも現れます — `logical_graph_fingerprint` は前者だけから、`automatic_plan_fingerprint` は後者(選ばれた候補・target facts・provider generation)も含めて計算されます。

通常は何も指定せず、`optimized` profile で自動計画します。再現試験や特定部分だけを保守的にしたい場合は、同じ rendering config に portable な `target_planning` を追加できます。

```json
{
  "target_planning": {
    "profile": {
      "kind": "hazard_stress",
      "seed": 23
    },
    "graphs": {
      "main": {
        "nodes": {
          "transparent": {
            "serial": true,
            "isolate": false
          }
        },
        "resources": {
          "scene_depth": {
            "no_alias": true
          }
        }
      }
    },
    "diagnostics": {
      "strict_warnings": [
        "pelican.warning.example@1"
      ]
    }
  }
}
```

- profile は `optimized`、`conservative_debug`、`hazard_stress`。3 つの `kind` の意味は次のとおりです([targetplanning.cpp](../../src/project/targetplanning.cpp#L972))。
  - `optimized`(既定) — 並列化・scope 融合(tile-local)・alias の候補をすべて拾い、安定な順序で採用する。
  - `conservative_debug` — 並列候補を出さず、rendering scope の融合もしない。つまり「順番に 1 つずつ、資源は共有しない」側へ倒す。バリアや融合を疑うときの対照用です。
  - `hazard_stress` — **hazard(ハザード)** は「同じ資源への読み書きが正しい順序で並ばないと壊れる関係」を指す GPU 同期の用語で、この profile はその取りこぼしを探すためのものです([用語集](14_glossary.md))。**依存が許す範囲の任意な選択**(実行可能ノードの取り出し順、並列/融合/alias 候補の並び)を `seed` で撹拌し、alias 候補を半分に間引く。順序に暗黙に依存したバグを揺さぶり出すための profile で、`seed` はここでだけ指定できます(同じ seed なら再現します)。
- node/resource 制約は graph ごとです。`main` と書けば XR variant のコンパイル時には自動的に `main#xr` へ対応し、個々の node/resource 名は変わりません。
- `serial` / `isolate` / `no_alias` は禁止側だけを明示する設計です。通常 node に `parallel_safe` のような boilerplate は要りません。
- 未知 graph/node/resource、重複した warning ID、型の違うフラグは runtime object を作る前に compile error になります。
- 書く RPC は `edit_target_planning`(✅EL-2)です。graph ごとの `nodes` / `resources` を名前単位で混ぜ、`null` で消します。今の合成に無い名前は、ticket を取らずに `target_planning_name_unknown` で断ります。`profile` と `diagnostics` を書く RPC は無く、文書を直すか `restore_render_documents` です。形と拒否は[第13章](13_editor.md)の「graph 全体の計画の決定を書く(✅EL-2)」にあります。
- ✅FG-1: graph の root 文書の `fold_groups` に、root の node の組(断片)の `serial` / `isolate` / `no_alias` をローカル名(`{node, element?, port?}`)で書けます。組み立てのたびに engine が展開名へ写し、この `target_planning` に `true` で足します([`applyGraphFoldGroups()`](../../src/project/graphfold.cpp#L269))。ループの回の数が変わっても書き直しは要りません。効くのは、`verify_fold`(✅FG-2)が付けた印の digest が今の断片と同じ group だけで、ほかは決定を捨てて開いた形で走り、frame plan の `diagnostics` に `fold_stale` / `fold_unverified` が出ます。ここに `false` と書いた名前と食い違えば `graph.fold_decision_conflict@1` で断られます。足された決定は frame plan の `fold_decisions` と、`physical_target_plan.decisions[]` の `pelican.plan.fold_decision_applied@1`(`from_fold` 付き)に出ます。形と RPC は[第13章](13_editor.md)の「断片の決定を宣言する(✅FG-1)」と「断片を検査して印を付ける —— `verify_fold`(✅FG-2)」にあります。

#### 3 つの representation と、その選ばれ方

自動計画は attachment 1 枚ごとに次の 3 つから 1 つを選びます。利用者が直接指定する field はありません。

| representation | 何をしないか | GPU 上の効果 | 選ばれる条件(概略) |
|---|---|---|---|
| `materialized_image` | 何も省かない(既定・安全側) | 通常の device-local メモリに 1 枚確保し、内容を保存する | 下の 2 つの条件を満たさないときは常にこれ |
| `transient_attachment` | 描画区間の外への**内容の永続化**を要求しない | lazily allocated memory を優先し、最後の Store を Discard にする | attachment としてしか使わず、後段の read も history も無い(= 書きっぱなし) |
| `tile_local_attachment` | メモリへの**書き戻し**自体をしない | producer と consumer が同じ dynamic rendering instance で走り、値がタイル内に留まる | 上に加えて、直後の consumer が全員 `same_pixel` で読み、extent が一致し、device が `VK_KHR_dynamic_rendering_local_read` を持つ |

下へ行くほど得ですが条件が厳しく、1 つでも外れると 1 段上へ自動で戻ります(黙って壊れる方向へは倒れません)。以下はそれぞれの条件の詳細です。

#### 自動 transient attachment

`optimized` profileでは、後段から読まれない一時的なattachmentを自動的に
`pelican.vulkan.transient_plan@1`へloweringできます。利用者がstorage modeを直接指定する
fieldはありません。たとえば次のtargetをpassがwriteするだけで、その値をsample、transfer、
history、external depth exportに使わなければ候補になります。

```json
{
  "name": "scratch",
  "format": "R8G8B8A8_UNORM",
  "usage": ["COLOR_ATTACHMENT"]
}
```

自動選択の条件は、attachment-only、non-history、single-sample、write-onlyであり、対象deviceが
そのformatと`TRANSIENT_ATTACHMENT` usageを実際に受理することです。選択時はphysical
representationが`transient_attachment`になり、最後のStoreは自動的にDiscardへloweringされます。
runtimeはimageへ`VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT`を付け、lazily allocated memoryを
優先します。対応memory typeがないdeviceではdevice-local memoryへ戻せるため、transientは
「毎frame VkImageを作り直す」という意味ではなく、attachment内容の永続化を要求しない契約です。

format/usageが非対応、後段readがある、MSAAである、または`conservative_debug` profileなら、
`pelican.vulkan.materialized_plan@1`へ安全側に戻りStoreを維持します。後段readをscope内に
閉じ込めるtile-local loweringは別候補であり、次の条件を満たす場合だけ選ばれます。

#### 自動 tile-local attachment / same-pixel local read

`optimized` profileでは、producerがattachmentへ書いた値を直後のfullscreenまたは
material raster passが`same_pixel`で読む区間を、自動的に一つのphysical rendering
scopeへ融合できます。
通常のrendering configへstorage modeやVulkan layoutを追加する必要はありません。

自動選択には次の条件がすべて必要です。

- resourceはnon-history、single-sampleのcolor/depth attachmentである
- producerはそのresourceをattachmentとしてwriteする
- すべてのconsumerは対応済みfullscreen/material passで、read footprintが
  `same_pixel`である
- material image resourceはfragment-onlyである
- consumerの全attachmentとlocal-read resourceのphysical extentが一致し、scope内に
  swapchain attachmentを含まない
- deviceが`VK_KHR_dynamic_rendering_local_read`のextension/featureを有効化できる
- 対象formatがattachment、input attachment、transient attachmentの組み合わせを受理する

選択結果は`pelican.vulkan.tile_local_plan@1`です。対象resourceは
`tile_local_attachment`となり、producerとconsumerは同じdynamic rendering instanceで
実行されます。compilerはattachment locationとinput attachment indexを固定し、pass間に
BY_REGIONのlocal-read dependencyを入れます。fullscreen shaderの
`PELICAN_DECLARE_INPUT_N` / `PELICAN_SAMPLE_INPUT`は同じsourceのまま、選択されたpipelineだけ
`subpassInput` variantへコンパイルされます。materialの
`pelican_screen_<name>()` / `pelican_sample_<name>()`も公開sourceを変えず、
samplerまたはinput attachmentへ物理解決されます。descriptor bindingと
input attachment indexは別の番号空間です。scope外へ値を残さないためproducerのStoreは
Discardです。allocatorは`INPUT_ATTACHMENT | TRANSIENT_ATTACHMENT`を付け、lazy memoryを
優先します。local-read対象だけを`RENDERING_LOCAL_READ_KHR` layoutへ置き、同じscopeの
output-only attachmentは通常のcolor/depth attachment layoutを保ちます。

extension/feature/format非対応、未対応のraster pass kind、`neighborhood` /
`arbitrary` / `temporal` read、history、extent不一致、MSAAではmaterialized planへ
自動fallbackします。
同名targetを複数graphが共有し、一方がscope外materializationを必要とする場合も
`materialized_image`を優先します。ただしtile-local graphが同じtargetをinput attachmentとして
読む契約は失わないため、runtime image usageには`INPUT_ATTACHMENT`が残ります。

material shaderはrouteを共有する全active graph variantで、各inputのsampler/local種別と
input attachment indexが一致する必要があります。local variantではUV/LOD引数は
公開ABIを保つため残りますが、`subpassLoad()`は現在画素だけを読み引数を使いません。
生成image accessorは現在`vec4`契約なので、UINT/SINT color input attachmentは
typed accessorを追加するまで名前付きエラーになります。

XRの2-view planでは、融合scopeも他のscopeと同じtyped view contractを継承します。
multiview選択時は2-layer image、`viewMask=0b11`、execution count 1となり、sequential選択時は
同じlogical planをlayerごとに実行します。synthetic Vulkan fixtureはlayered local readの
左右出力を検証しますが、Quest/Meta XR Simulatorでの帯域効果と実表示は外部gateです。

さらに、現在の自動計画が選んだ Vulkan backend candidate を固定したい場合は、まず pin なしで起動し、`get_frame_plan.physical_target_plan.ejectable_pin_package` をそのままコピーします。`get_frame_plan` は実行中の player に stdio で問い合わせる **JSON-RPC メソッド**です([第10章](10_tools.md#基盤メソッド一覧)。Studio の Frame Plan タブと Plan Viewer も同じ JSON を読みます)。`ejectable_pin_package` はその応答に入っている**そのまま貼れる形の JSON 断片**で、「今回の自動計画がどの backend candidate を選んだか」を記録したものです。コピー先は variant 別の `vulkan_plan_pins` 配列です。

```json
{
  "vulkan_plan_pins": {
    "flat": [
      {
        "schema": "pelican.vulkan_target_plan_pins",
        "version": 1,
        "graph": "main",
        "logical_graph_fingerprint": "fnv1a64:0123456789abcdef",
        "pins": {
          "backend_candidate": "pelican.vulkan.materialized_plan@1"
        }
      }
    ],
    "xr": []
  }
}
```

貼り付け後は `physical_target_plan.applied_pin_package` に同じ package が出ます。論理 graph が変わった package は `pin package is stale`、現在の device/provider で成立しない candidate は `pinned backend candidate is infeasible` として失敗し、別 candidate へ黙って fallback しません。ここで **fingerprint** は「対象の内容から計算した短いハッシュ値」で、貼った package が今のグラフ向けかどうかを照合するための鍵です。`logical_graph_fingerprint` は論理グラフ(パスと資源と依存)から計算され、**物理側の指定(`target_planning` / `vulkan_plan_pins` / `vulkan_physical_fragments`)はその計算に入りません**。したがってこれらを貼ったこと自体では logical fingerprint は変わらず、貼った package が直後に stale になることもありません。

RPC から書くときは `edit_vulkan_plan_pins {base_source_digest, family, pin}`(✅EL-2)です。同じ family の同じ `graph` の package を置き換え、`pin: null` で消します([第13章](13_editor.md)の「graph 全体の計画の決定を書く(✅EL-2)」)。✅UX-15b から、Studio の Frame Plan の Backends タブで候補の行を右クリックして **Pin this candidate** を押すと、`ejectable_pin_package` の候補だけを替えたものをこの RPC で送ります(帯の **Unpin** で消します。第13章の「下の層の決定を Frame Plan から書く(✅UX-15b)」)。

v1 pin が固定するのは有限集合の **backend candidate だけ**です。resource/scopeを編集する場合は、次の別schemaを使います。

### Vulkan physical fragment v1 / v2 / v3

`physical_target_plan.ejectable_physical_fragment`を丸ごとコピーし、対応するvariantの`vulkan_physical_fragments`へ貼ります。
現在はversion 3をejectします。version 1はresource/scope/alias、version 2はさらに
attachment load/storeを扱う既存packageとして引き続き読めます。version 3は
`scope_edit_mode`を追加し、古いpackageの意味を広げずに依存関係を保つscope編集を選べます。

```json
{
  "vulkan_physical_fragments": {
    "flat": [
      {
        "schema": "pelican.vulkan_physical_fragment",
        "version": 3,
        "scope_edit_mode": "dependency_safe",
        "graph": "main",
        "logical_graph_fingerprint": "fnv1a64:0123456789abcdef",
        "automatic_plan_fingerprint": "fnv1a64:fedcba9876543210",
        "backend_candidate": "pelican.vulkan.tile_local_plan@1",
        "resources": [
          {
            "logical_resource": "gbuffer_albedo",
            "format": "R8G8B8A8Unorm",
            "representation": "materialized_image"
          }
        ],
        "scopes": [
          {
            "id": "manual:geometry",
            "nodes": ["geometry"]
          },
          {
            "id": "manual:lighting",
            "nodes": ["lighting"]
          }
        ],
        "alias_groups": [],
        "attachments": [
          {
            "node": "geometry",
            "logical_resource": "gbuffer_albedo",
            "load_op": "discard",
            "store_op": "store"
          }
        ]
      }
    ]
  }
}
```

`scopes[].id` の `manual:` は「自動計画が付けた ID ではなく、人が手で書いた scope である」ことを示す接頭辞です(上の例では `manual:geometry` / `manual:lighting`)。eject結果は全resource/scope/alias groupを含みますが、手書きpackageの`resources`は変更するresourceだけに減らせます。`scopes`または`alias_groups`自体を省略すると、その部分は自動planを維持します。**alias group** は「同じ GPU メモリ確保を使い回してよい resource の組」です。lifetime(生存区間)が重ならない画像同士なら、別々に確保せず 1 つの割り当てを共有できるので VRAM が減ります。`alias_groups: []`はaliasなしの明示指定です。`scopes`は全nodeのexact partitionなので、空配列が有効なのはnodeを持たないgraphだけです。

別formatを選ぶ場合は、先にrender target側で許可する集合を宣言します。

```json
{
  "name": "low_color",
  "format": "R8G8B8A8_UNORM",
  "format_candidates": ["R16G16B16A16_SFLOAT"],
  "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
}
```

その後、自動planからejectしたfragmentの該当resourceを、eject結果と同じ**canonical 表記**（fragment 側の format 名の綴り方。render target JSON の `R16G16B16A16_SFLOAT` に対し、fragment では区切り記号の無い綴り）
（例: `"R16G16B16A16Sfloat"`）へ変更します。render target JSONのformat名をfragmentへ
推測で転記せず、必ず現在の`ejectable_physical_fragment`を編集してください。
link時には、対象deviceのrequired image usage（historyなら`TRANSFER_DST`も含む）、
sample count、array layer上限、external depthなら`TRANSFER_SRC`契約を再照合します。
候補未宣言、非`materialized_image`、device非対応の変更は**黙って別の値へ落とさず(fallback せず)**、名前付きエラーで起動を止めます。「指定は通ったが実際は別の format で動いていた」という状態を作らないためです。

全versionで共通して許可する編集は次です。

- resource representationの維持、または自動`tile_local_attachment` / `transient_attachment`から`materialized_image`への保守的な変更
- 自動planが選んだformatの保持、または`format_candidates`で宣言され、実device capabilityを満たす`materialized_image` formatへの変更
- format、representation、sample count、view layout、extentが一致し、**論理lifetime**(そのresourceが最初にwriteされるnodeから最後にreadされるnodeまでの、論理グラフ上の区間)が重ならないresourceだけのalias group
- v2以降では`(node, logical_resource)`で特定したraster attachmentの`load_op` / `store_op`。
  `load_op`は`load|clear|discard`、`store_op`は`store|discard`で、未記述fieldは自動planを維持する

**scope** はここでは「1 つの Vulkan dynamic rendering instance にまとめて実行するノードの並び」を指します(この節の物理計画の文脈に限った意味です — [用語集](14_glossary.md))。`scope_edit_mode` はその scope をどこまで編集してよいかの指定で、**編集を許す範囲が version ごとに異なります**。

- v1/v2、またはv3の`scope_edit_mode: "split_only"`は、自動scopeをさらに**分ける**
  exact ordered partitionだけを許可します。**tile-local 値**(scope の中だけで受け渡され、GPU メモリへ書き戻さない attachment の値。上の「自動 tile-local attachment / same-pixel local read」)が新しい境界をまたぐ場合は、その
  resourceを先にmaterializeします。
- v3の`scope_edit_mode: "dependency_safe"`は、data edge(§6.6 冒頭 — 書いた資源を読む依存)と明示`after` / `before`をすべて
  保つ範囲でnode/scopeを並べ替えられます。宣言順はcorrectness根拠にしませんが、
  feature compositionが通常pass列へ追加した明示`after`も消せません。

  具体例(回帰テスト `test/targetrenderplanning_test.cpp` の 2 ケース): 自動 scope が `[GBuffer, Lighting]` と `[Forward]` に分かれているとき、`split_only` でできるのは `[GBuffer]` と `[Lighting]` へ**割る**ことだけです。`dependency_safe` なら、`Lighting` が書いた `scene_color` を `Forward` が同じ attachment として `load` している(= data edge が保たれる)ので、`{"id": "manual:lighting_forward", "nodes": ["Lighting", "Forward"]}` と書いて 2 ノードを**1 つの scope へ融合**できます。逆に `Forward` を `Lighting` より前へ動かす並べ替えは、同じ data edge に反するので reject されます。
- 並べ替え後はresource lifetimeを新しい物理順序から再計算し、手書きalias groupをその
  lifetimeへ再照合します。
- 異なる自動scopeの融合は、内部`materialized_image`だけを使うsingle-sample rendering
  scopeに限定します。kind、view実行、ordered color/depth attachmentが一致し、2番目以降が
  Load、最後以外がStoreでなければrejectされます。成立したscopeは一つのVulkan dynamic
  rendering instanceとして実行されます。swapchain、UI/ImGui、MSAA、異なるattachment集合、
  sampled/storage/transfer依存を含む融合はまだ受理しません。

貼り付け後は`applied_physical_fragment`で適用内容を再観測できます。logical graphだけでなく、変更前の自動plan、target facts、選択候補、provider generationも`automatic_plan_fingerprint`へ束縛されます。いずれかが変わった古いfragmentは **stale error**(貼った package が前提にしていた plan はもう存在しない、という失敗)となり、部分的に推測して修復しません。直し方は「もう一度 eject してから編集し直す」だけです。

RPC から書くときは `put_vulkan_physical_fragment {base_source_digest, family, fragment}`(✅EL-2)です。stale な fragment は、ticket の結果の `physical_fragment_stale {family, graph, fingerprint, expected, actual}` で返り、文書は変わりません(`fingerprint` は `logical_graph` か `automatic_plan`。[第13章](13_editor.md)の「graph 全体の計画の決定を書く(✅EL-2)」)。✅UX-15b から、Studio の Frame Plan の Physical plan タブの **Freeze this physical plan** が `ejectable_physical_fragment` をそのままこの RPC で送り、**Remove** が消し、stale で断られたときは **Eject again** が画面の plan の fragment で置き換えます(第13章の「下の層の決定を Frame Plan から書く(✅UX-15b)」)。

同じ名前のtargetを複数graphやflat/XR variantが共有する場合、全planは同じphysical formatを選ぶ必要があります。異なるformatを同時に使う設計ならtarget名を分けてください。hot reloadでformatを変更した場合は、新しいruntime generationのimage・view・pipelineを作ってから一括publishし、旧generationは既存のGPU lease規則でretireします。

v2/v3のattachment編集はlogical dependencyを壊せません。論理readを持つattachmentは必ず
`load`、readを持たないattachmentは`load`にできません。編集対象は現在
`materialized_image`またはexternal targetに限定されます。`store`から`discard`への変更は、
別MSAA resolveが論理値を保存し、そのmultisample surfaceを後続attachmentがloadしない場合だけ
許可されます。通常raster、output transform、UI、ImGuiはいずれもlink済みのattachmentごとの
操作をdynamic renderingへ適用します。hot reloadもformat変更と同じgeneration transactionで
prepare/publishされます。

自動planがwrite-only targetを`transient_attachment`にした場合、そのattachmentは初めから
`discard`です。内容の保存が必要な特殊実験では、ejectした同じfragmentでresourceを
`materialized_image`へ保守的に変更し、attachmentを`store`へ戻せます。

自動planのalias groupは、non-history・single-sample・materializedで、format、usage、
extent、view契約が同一、かつlifetimeが重ならないimageに限ってruntime allocationへ適用されます。
同じtarget集合を共有する全graph variantが完全に同じgroupへ合意しない場合は、正しさを保ったまま
allocation共有だけを無効化します。実行時はgroup内でも別々のVkImageを使い、同じVMA allocationを
共有します。hot reload candidateはgeneration固有groupなので、旧in-flight imageとは共有しません。

まだ受理しないのはphysical fragmentによる`materialized_image`からtile-localへの攻めた変更、
MSAA・external・異なるattachment集合をまたぐscope融合、依存関係を破るreorder、一般の
materialized single-sample surfaceに対するstore elision、barrier、queue、任意Vulkan flagです。
production runtimeが実行する
非materialized imageは、上記のwrite-only `transient_attachment`と、verified
same-pixel fullscreen/material subsetの`tile_local_attachment`です。alias runtimeは現在、
color attachment + sampled用途のmaterialized imageだけを対象とし、MSAA、history、depth、
storage/transfer image、bufferとの混在はまだ受理しません。

**alias から外れた理由(✅PO-1)。**runtime の alias の候補は、history でなく、format class が `display` でなく、storage mode が materialized で、usage がちょうど
`{COLOR_ATTACHMENT, SAMPLED}` の target です。外れた target の理由は `physical_target_plan.resources[].reason`(と `pelican.plan.resource_representation@1` の `detail`)が
`logical materialization requirement is required` か `ResourcePattern requires contents to be stored` のとき、その後に `: ` と原因を続けて言います(原因が複数なら `; ` で区切る。
[原因の文を作る所](../../src/core/renderingpass/renderingsamplecount.cpp#L116)、[理由に続ける所](../../src/project/targetrenderplanning.cpp#L997))。

| 原因の文 | いつ |
|---|---|
| `history target keeps its contents for the next frame` | history の target |
| `display-format target is not aliased` | `format_class` が `display` |
| `storage mode is not materialized` | storage mode が materialized でない |
| `usage <X> is outside the alias-safe set {COLOR_ATTACHMENT, SAMPLED}`(複数は `usage <X>, <Y> are …`) | usage に集合の外の flag がある(usage の表の順に並べる) |
| `usage lacks <X> of the alias-safe set {COLOR_ATTACHMENT, SAMPLED}` | usage に集合の flag が欠けている |

たとえば usage に `TRANSFER_SRC` を足した target は `logical materialization requirement is required: usage TRANSFER_SRC is outside the alias-safe set {COLOR_ATTACHMENT, SAMPLED}`
になります(PO-1 の test)。判定の規則は変えておらず、言葉を足しただけです。ただし `reason` は `automatic_plan_fingerprint` に入るので
([入れる所](../../src/project/vulkanphysicalfragment.cpp#L831))、上の 2 つの理由で外れた target を持つ plan では PO-1 の前後で fingerprint の値が変わり、PO-1 より前に eject して
貼った physical fragment は stale になります(eject し直して貼り直します)。

## 6.7 マテリアル(✅M1〜M3.5 = WP58/68/70/76/78/83)

マテリアルは「**`.surface`(シェーダ+パラメータ宣言)+ `.material.json`(値)**」の 2 ファイル方式です。

### .surface — 自己記述コンテナ

`.surface` は**テキストファイル**で、GLSL スニペット + `//!` 構造化ヘッダから成ります。実物([../../projects/example/shaders/toon.surface](../../projects/example/shaders/toon.surface)):

```glsl
//! pelican.surface v1
//! language: glsl
//! params:
//!   - { name: tint, type: color, default: [1.0, 0.32, 0.08, 1.0] }

void pelican_surface_v1(in PelicanSurfaceInputV1 surface_input, inout PelicanSurfaceV1 surface) {
    surface.base_color = pelican_param_tint() * surface_input.vertex_color;
    ...
}
vec3 pelican_lighting_v1(in PelicanSurfaceV1 surface, in PelicanSurfaceInputV1 surface_input) { ... }
```

- ヘッダ語彙: `language` / 順序付き `params[]`(型 `float|vec2|vec3|vec4|int|color`、**`default` 必須**、min/max/hint 任意)/ `textures[]`(`name` / `default` / `color_space: srgb|linear` が**必須** — テクスチャの中身が sRGB 符号化された色か、そのまま使うリニア値かの申告です(§6.3)。SRGB/UNORM view を決めるのは `color_space` で、`role: color|data` は任意の整合チェック)/ `screen_inputs[]`(§6.8)/ `resource_ports[]`(下記)/ `render_state`。網羅例は `test/fixtures/surface_format/valid/full.surface`。
- **フック梯子 v1(凍結)**: エンジン側テンプレートが用意した差し込み口(フック)を、頂点変形 → 表面値 → BRDF/環境光 → ライティングの順に**梯子の段のように**並べたもの、という意味の呼び名です。段の名前は `pelican_vertex_displace_v1` / `pelican_surface_v1` / `pelican_brdf_v1` / `pelican_ambient_v1` / `pelican_lighting_v1` の 5 つで、書いた関数がそのまま宣言になります(定義しなかった段はテンプレートの既定のまま)。`brdf` と `lighting` は排他、未知の `pelican_` 関数定義やフックゼロはロードエラー。
- スニペットはエンジン所有テンプレート(`engine://shaders/material/surface_v1.{vert,frag}`)へ**逆 include** され、エラーは `#line` で元ファイル名・行番号に翻訳されます。**逆**というのは向きのことです — 普通は自分のシェーダが共通ヘッダを `#include` しますが、ここでは**テンプレートの側が利用者の `.surface` を include** します。具体的にはコンパイル時に `.surface` の本文が `__pelican_user_surface.glsl` という仮想ファイル名で登録され([surfacecompiler.cpp](../../src/core/shader/surfacecompiler.cpp#L69))、テンプレート [surface_v1.frag](../../src/core/resources/shaders/material/surface_v1.frag#L27) の `#include "__pelican_user_surface.glsl"` がそれを取り込みます。取り込んだあと、同ファイル下方の `main()` が `pelican_surface_v1(input_data, surface)` を呼びます。だから `.surface` に `main()` は書きません。パラメータへは自動生成アクセサ `pelican_param_<name>()` / `pelican_sample_<name>(uv)` でアクセスします。
- 同梱の standard / toon ライティングも**同じ公開経路**で書かれています(特権なし standard library。dogfooding)。

### 任意 G-buffer / material output ABI・attachment state（✅WP218/219）

ここでの**任意**は「書き出す枚数と各枚の中身をプロジェクトが決めてよい」という意味です(形式が曖昧、という意味ではありません)。G-buffer はエンジン固定の5スロットではありません。**material pass**(= `"type": "material"` のパス。シーン内のモデルを描くパス種別。§6.2)が
`pelican.material_outputs` v1 を宣言すると、`outputs[]` の配列順が fragment shader の
`location = 0..N-1` と `output.color` の RT 順になります。`N` にエンジン独自の上限はなく、
Vulkan デバイスの `maxColorAttachments` と、選択した format / MSAA の capability だけを
検証します。

```json
{
  "name": "deferred_geometry",
  "type": "material",
  "material_contract": "deferred_geometry_v1",
  "material_outputs": {
    "schema": "pelican.material_outputs",
    "version": 1,
    "name": "my_renderer.extended_gbuffer",
    "outputs": [
      { "name": "albedo",    "type": "vec4", "source": "surface.base_color" },
      { "name": "normal",    "type": "vec4", "source": "surface.normal_encoded" },
      { "name": "material",  "type": "vec4", "source": "surface.material" },
      { "name": "world_pos", "type": "vec4", "source": "input.world_position" },
      { "name": "emissive",  "type": "vec4", "source": "surface.emissive" },
      { "name": "object_id", "type": "uint", "source": "custom" }
    ]
  },
  "output": {
    "color": [
      "gbuffer_albedo", "gbuffer_normal", "gbuffer_material",
      "gbuffer_world_pos", "gbuffer_emissive", "gbuffer_object_id"
    ],
    "depth": "offscreen_depth"
  },
  "clear_colors": {
    "gbuffer_object_id": [4294967295, 0, 0, 0]
  }
}
```

`type` は `float/vec2/vec3/vec4`、`int/ivec2/ivec3/ivec4`、
`uint/uvec2/uvec3/uvec4`。ここでいう**数値クラス**は「その値を浮動小数(floating)・符号付き整数(SINT)・符号なし整数(UINT)のどれとして扱うか」の 3 区分で、成分数(vec2 か vec4 か)とは別の軸です。RT format とこの数値クラスが一致しない
設定は GPU pipeline 作成前に拒否します(たとえば `"type": "uint"` の出力を `R8G8B8A8_UNORM` の RT へ向けるのは不可)。組み込み `source` は
`surface.base_color`、`surface.normal`、`surface.normal_encoded`、
`surface.material`、`input.world_position`、`surface.emissive`、
`lighting.scene_color`。`source` は「その出力スロットに何を書くか」の指定です。組み込み名を書けばエンジンが対応する値を自動で入れ、`custom` を書いた場合だけ**値の供給を `.surface` 側に委ねます** — 下の `pelican_material_outputs_v1()` を書き、`outputs.<outputs[].name>` へ代入するのが供給の唯一の口です。上の例の `object_id` は `"source": "custom"` なので、次のスニペットの `outputs.object_id = 73u;` がその値になります。代入しなければ未初期化のままなので、`custom` を書いたら必ずこの関数で埋めてください。

```glsl
void pelican_material_outputs_v1(
    in PelicanSurfaceInputV1 input_data,
    in PelicanSurfaceV1 surface,
    inout PelicanMaterialOutputsV1 outputs) {
    outputs.object_id = 73u;
}
```

生成後の SPIR-V reflection(コンパイル済みシェーダを読み返して要求資源を取り出すこと。§6.4)は全 location と数値型を宣言スキーマへ照合します。
flat / XR variant は同じ route に同一スキーマを要求します。組み直した graph の候補世代が、生きている material を別の
output schema・format・MSAA(sample 数)・local-read・attachment state で描くことになる場合の扱いは、候補がどこから来たかで違います。

- **ディスクからの読み直し**(file watcher が rendering 文書の変更を拾ったとき。[`Renderer::reloadRenderPipelineFromDisk()`](../../src/core/vkcore/renderer.cpp#L3563)):
  ReloadService があると、live の material との照合を外し([`validate_live_materials = false` の所](../../src/core/vkcore/renderer.cpp#L3595))、
  公開の直前の hook で、同じ watcher の batch に入った shader・material・model の変更と一緒に、material の shader と pipeline を
  候補世代に合わせて作り直してから公開します([`prepareRenderPipelineModelReload` と `applyRenderPipelineCompanionReload` を呼ぶ所](../../src/core/vkcore/renderer.cpp#L3604)、
  [作り直しの計画](../../src/core/material/materialgenerationplanner.cpp#L1294))。output schema が変わる material は fragment shader を
  新しい schema で compile し直し、format・sample 数・local-read・attachment state が変わる material は pipeline を作り直します。
  作り直しのどれかが失敗すると例外で候補世代を捨て、旧世代を保ちます([`ReloadService::applyRenderPipelineCompanionReload()`](../../src/core/watch/reloadservice.cpp#L432))。
  graph と material surface を同じ transaction で組み直す仕組みは、この経路では既に動いています。
  - この経路でも断られるもの: 受け皿の無い route(`material_route_unserved@1`)、1 つの material の route が flat と XR で別の schema や
    別の物理の契約になる候補、同じ fragment shader や pipeline を共有する material が両立しない契約を要求する候補。
  - ⚠ 作り直しが commit した後で、その後ろの準備(XR の variant が残っているかの確認、model の reload の `preparePublication`)が失敗した
    場合に、差し替え済みの material の pipeline が戻るかは確かめていません(未確認)。
  - ReloadService は Renderer を作るときに必ず作られます([reload の参加者を登録する所](../../src/core/vkcore/renderer.cpp#L3259))。
    ReloadService が無い場合の分岐では照合が効き、今までどおり断ります([照合を切り替える所](../../src/core/vkcore/renderer_config.cpp#L504))。
    ただし通常の起動でこの分岐を通る道は見当たりません(コードからの推論)。
  - file watcher が動くのは、窓ありで、`--rpc`・`--replay`・`--strict-assets` のどれも付けない起動だけです
    ([gate の条件](../../src/core/watch/reloadgate.cpp#L46)、[headless では読み直しを要求しない所](../../src/player/main.cpp#L502))。
    ✅AV-1 から、`PELICAN_WITH_HOT_RELOAD=OFF` の build(配布 build)では起動のしかたに関係なく動きません([第10章](10_tools.md) §10.5)。
    ✅HW-1 から、project.json に `hot_reload` を宣言すると `--rpc` の下でも watcher は動きますが、`--rpc` の下では watcher の変更は
    この経路(`pelican.render_pipeline` の参加者)に届きません([除外する所](../../src/core/watch/reloadservice.cpp#L920))。
    Studio が起こす engine には常に `--rpc` が付くので([Studio の起動引数](../../src/devstudio/viewport/studioplayerarguments.cpp#L81))、
    Studio の中の engine はこの経路を通りません。Studio の外でディスクの文書を書き換えたときは、`reload_render_documents`(✅RL-1)で取り込みます。
    これは下の「編集 RPC の候補」として組み直します([第13章](13_editor.md)の「ディスクで書き換えた render 文書を取り込む」)。
- **編集 RPC の候補**(`edit_graph_document` など): 照合を外しません([`validate_live_materials = true` の所](../../src/core/vkcore/renderer.cpp#L3793))。
  output schema が変わる候補は断ります。format・MSAA・local-read・attachment state だけが違う候補は、次の段落の RS-1 の扱いです。

編集 RPC の候補では、変わるのが format・MSAA・local-read・attachment state だけで、ほかの検査がすべて通るとき、
editor は断らずに文書を commit し、`restart_required: true` と `restart_reason`(`code: "material_physical_contract_changed"`)を
返します(✅RS-1)。live の世代はそのままで、engine を起動し直すと commit した文書で組み直します
([第13章](13_editor.md) §13.5 の「live の material が受けられない編集は、書いてから再起動を求めます」)。
schema の変更は今までどおり断られます。編集 RPC の経路でもディスクの経路と同じ作り直しを使って、起動し直さずにその場で載せる案は
採らず、起動し直しのままにしています(2026-10-01 利用者の決定)。

outputごとにblendまたは書き込みchannelを変える場合は、同じpassへ
`material_output_states`を追加します。キーはRT名やlocation番号ではなく
`material_outputs.outputs[].name`です。書かなかったoutputは`.surface`ヘッダの
`render_state`(本節冒頭の「.surface — 自己記述コンテナ」のヘッダ語彙の 1 つ)が持つ`blend`と`rgba` writeを継承します。

```json
"material_output_states": {
  "accum": {
    "blend": {
      "color": { "src": "one", "dst": "one", "op": "add" },
      "alpha": { "src": "one", "dst": "one", "op": "add" }
    },
    "write_mask": "rgba"
  },
  "revealage": {
    "blend": {
      "color": {
        "src": "zero",
        "dst": "one_minus_src_color",
        "op": "add"
      },
      "alpha": {
        "src": "zero",
        "dst": "one_minus_src_alpha",
        "op": "add"
      }
    },
    "write_mask": "r"
  },
  "object_id": {
    "blend": "opaque",
    "write_mask": "r"
  }
}
```

`blend`には`opaque` / `blend` / `additive` preset、またはcolor/alpha別の
`src` / `dst` / `op`を指定できます。write maskは`rgba`の任意部分集合または
`none`です。integer outputでblendを有効にする設定は拒否します。attachment間で
stateが異なる場合はdeviceの`independentBlend`(color attachment ごとに別々の blend 設定を持てる、という Vulkan のデバイス機能。無いデバイスでは全 attachment が同じ blend 設定でなければならない)、blend対象formatには
`COLOR_ATTACHMENT_BLEND` capabilityが必要です。全factor/opと現在含めていない
blend constant/dual-source blendの境界は
[WP219実装報告](../design_reviews/2026-07-28_wp219_material_output_states_report.md)
を参照してください。

`material_outputs` / `material_output_states` は **material パス限定**です
(ほかの type に書くと所有していない旨の名指しエラー)。
`raster` / `fullscreen` / `output_transform` パスの attachment blend / write mask は、値の語彙は同じですが
`raster_state.color_attachments[]` の側に書きます(§6.2)。

`material_outputs` を省略した pass は既存プロジェクト向けの内蔵5-MRT/1-color ABIを
維持します。独自スキーマは生成 fragment shader が必要なため、現時点では runtime
shader compiler を有効にした開発ビルドで使います。shaderc OFF の配布物は
`dist-bake`（WP211）の生成物へ移す予定です。

### compute/別passのresourceをmaterialから読む（✅WP207b/220）

`.surface` はgraph固有名ではなく、再利用可能な **semantic port** を宣言します。ここでの *semantic* は「この `.surface` にとっての役割名」という意味で、`displacement` や `simulation_color` のように**使う側の呼び名**を書きます。実際にどの frame-graph resource を挿すかは pass 側の `material_resources` が決めるので、同じ `.surface` を別のグラフへ持っていっても中身を書き換えずに済みます(この対応付けは §6.5 の named binding と同じ考え方です)。

```glsl
//! resource_ports:
//!   - { name: displacement, kind: buffer, element: vec4, stage: vertex }
//!   - { name: simulation_color, kind: image, stage: fragment }

void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {
    vertex.position += pelican_load_displacement(0u).xyz;
}
```

material pass側で同名portをframe-graph resourceへ割り当てます。

```json
"material_resources": {
  "displacement": {
    "resource": "simulation_positions",
    "access": "storage",
    "footprint": "arbitrary"
  },
  "simulation_color": {
    "resource": "simulation_color",
    "access": "sampled",
    "view": "shared_2d",
    "sampling": {"filter": "linear", "address": "clamp_to_edge"},
    "footprint": "same_pixel"
  }
}
```

`footprint`(読み取りの広がり)は「そのシェーダが 1 画素を出すために、入力画像のどこまで読むか」の申告で、値は `same_pixel`(同じ座標の 1 画素だけ)/ `neighborhood`(周囲の限られた範囲。任意で `radius`)/ `arbitrary`(どこでも)/ `temporal`(別フレームの面)です([logicalrendergraph.hpp](../../src/project/logicalrendergraph.hpp#L115))。`same_pixel` だけが tile-local 化の候補になります(§6.6)。`sampling.filter` はテクセル間の補間方法(`linear` = 線形補間 / `nearest` = 最近傍そのまま)、`sampling.address` は範囲外の座標をどう扱うか(`repeat` = 繰り返し / `mirrored_repeat` = 折り返して繰り返し / `clamp_to_edge` = 端の値で止める)で、どちらも Vulkan の sampler 設定そのものです。

bufferはreadonly std430 arrayで`pelican_load_<name>()` /
`pelican_count_<name>()`、imageは`pelican_sample_<name>()` /
`pelican_size_<name>()`を生成します。binding番号は書きません。
`stage`は`vertex|fragment|vertex_fragment`、bufferの`element`は
`float/vec*/int/ivec*/uint/uvec*/mat4`。image historyはresource名の
`@history`で指定します。

imageは通常`sampler2D`ですが、fragment-only portへ`footprint: "same_pixel"`を指定し、
target plannerがtile-local scopeを選んだ場合は、同じ
`pelican_sample_<name>()`が`subpassInput` + `subpassLoad()`へ自動loweringされます。
このlocal variantではUV/LODは使わず、history/subresource/MSAAは利用できません。
条件を満たさないdevice/graphでは通常のsampled imageへfallbackし、filter/address指定を
そのまま使います。
compute/storage producerの出力自体はmaterializedのままです。tile-local化されるのは、
producerが同じresourceをraster attachmentとしてwriteするgraphだけです。

shared 2Dとsequential per-view 2Dはsampler/local両経路で利用できます。
material passが`view: "family_array"`を宣言したsampled imageは`sampler2DArray`へloweringされ、
`pelican_sample_<name>(uv)` / `pelican_sample_lod_<name>(uv, lod)`は現在のlogical viewを
自動選択します。明示的に別viewを読む場合はview index付きoverloadを使います。1-view familyが
scalar 2D imageへ物理化された場合も、runtimeが1-layer array viewを作るため同じsurface ABIを
維持できます。consumer-owned layered multiviewの一般sampled inputはまだ未対応で、
local input attachment経路だけを利用できます。現在のgenerated image accessorは
floating-point `vec4`契約で、UINT/SINT input attachmentはtyped image port追加まで拒否します。
全active graph variantはsampler/local種別、input attachment index、正規化後のshader view ABIが
一致する必要があります。
完全な契約は[シェーダ契約](../shader_contract.md)を参照してください。

### pelican.material — 値だけの JSON

```json
{ "schema": "pelican.material", "version": 1,
  "materials": [ { "name": "example_toon", "surface": "project://shaders/toon.surface",
                   "values": { "tint": [1.0, 0.32, 0.08, 1.0] } } ] }
```

(実物: [../../projects/example/materials/toon.material.json](../../projects/example/materials/toon.material.json))

### 安定した描画対象の選択（✅WP206a）

特定のマテリアルだけを追加の material pass へ参加させる場合は、マテリアルに
`tags`、パスに`material_filter`を書きます。material ID や描画順を指定する必要はありません。

```json
{
  "schema": "pelican.material",
  "version": 1,
  "materials": [{
    "name": "hero_coat",
    "surface": "project://shaders/coat.surface",
    "tags": ["character", "outline"],
    "values": {}
  }]
}
```

```json
{
  "name": "outline_geometry",
  "type": "material",
  "material_filter": {
    "include": ["outline", "selected"],
    "exclude": ["hidden"]
  },
  "output": { "color": "display", "depth": "offscreen_depth" }
}
```

- `include`は any-match（OR）です。省略または空配列なら全マテリアルを候補にします。
- `exclude`も any-match で、該当時は`include`より優先して除外します。
- tag は空文字不可、最大255 byte、同一リスト内の重複不可です。同じtagを
  include/excludeの両方へ書くこともエラーです。宣言順は意味を持ちません。
- 未知tagは設定エラーにはしません。includeにしかない未知tagは0件を選び、
  excludeの未知tagは何も除外しません。現在のframe-plan dumpには
  `resolved_draw_count`、`unmatched_include` / `unmatched_exclude`、
  `pelican.draw_queue_builder.material_tag_filter@1` provenanceが残ります。
- 文字列照合はフレームのimmutable draw queueをcompileするときに終わります。
  Vulkan描画ループは解決済みの連続rangeだけを受け取るため、drawごとの文字列比較は行いません。
- 選択はmaterial登録順、draw sort、visibilityによるordinal変化に依存しません。
  previewとXRも同じlogical filterを使います。values-only hot reloadではtag変更を拒否し、
  構造reload側のtransactionに委ねます。

旧`material_range`は低レベルfixture・再現試験用のdraw-call ordinalとして互換維持しています。
通常authoringでは使用しません。`material_filter`と併記した場合だけは、tagでcompact化した
rangeへ`material_range`を後段適用します。

### 同じ物体を別 surface でも描く（✅WP206b）

同じentity・mesh・materialを複製せず、選択した物体だけを別の`.surface`とrender stateで
もう一度描く場合は、materialに任意名の`variants`を宣言し、追加のmaterial passから
`material_variant`で選びます。名前に`outline`等の固定された意味はありません。

```json
{
  "schema": "pelican.material",
  "version": 1,
  "materials": [{
    "name": "hero_coat",
    "tags": ["character", "outlined"],
    "surface": "project://shaders/coat.surface",
    "variants": {
      "silhouette": {
        "surface": "project://shaders/silhouette.surface",
        "render_path": "forward",
        "values": {
          "width": 0.015,
          "color": [0.02, 0.02, 0.03, 1.0]
        }
      }
    }
  }]
}
```

```json
{
  "name": "silhouette_overlay",
  "type": "material",
  "material_contract": "forward_opaque_v1",
  "material_filter": { "include": ["outlined"] },
  "material_variant": "silhouette",
  "after": ["forward_opaque"],
  "output": { "color": "display", "depth": "offscreen_depth" }
}
```

- variantは別surfaceの`defines`、`values`、`textures`、`render_path`を持てます。
  `.surface`側の`render_state`も通常と同じloweringを通るため、front cullやblend等を
  variant専用の仕組みで再記述する必要はありません。
- 固定PBR factor・固定texture slot・skinning/VAT identityはbaseから継承します。
  custom値、custom texture、shader、pipeline、descriptorは独立しているため、異なる
  surface layoutでもbase materialのGPU recordを再解釈しません。
- `material_variant`には明示的な`material_contract`と、空でない
  `material_filter.include`が必要です。選択されたmaterialに同名variantが無い場合は、
  pass名とvariant名を含む設定エラーになります。
- 現在はbase draw queueを再利用するため、deferredからforward opaqueのような
  opaque phase内のroute変更はできますが、opaqueとtransparentを跨ぐ変更は登録時に
  拒否します。transparent独自のsortを必要とする跨ぎ方は、variant-aware draw queueの
  後続拡張で扱います。
- pass順は通常どおりresource hazardと`after` / `before`で決めます。
  flat、preview、sequential XR、multiviewでも同じlogical variant指定が保持されます。

GPU への経路は params 宣言順の std140 レイアウト → set 2 binding 6 の `MaterialBuffer` SSBO です。値の同レイアウト・ホットリロードも効きます(✅WP105)。`pelican_cli dump-lowered-material <surface>` で生成物(lowered GLSL・レイアウト)を確認できます。

### material の route の既定 `material_route_default`(✅G-4)

material は lowering のときに **route**(どの種類の material pass が描くか)が決まります
([route を決める `routeMaterial()`](../../src/project/materiallowering.cpp#L294))。`.material.json` の `render_path` が `"auto"`
(省略時)の material、glTF の material、基本形の既定の material `pelican.primitive.default`、engine の組込 material は
**automatic** で、blend か screen input を持てば `forward_transparent`、それ以外はほぼ `deferred_geometry` です(custom の
lighting / ambient / BRDF hook を持つ opaque で deferred に載らないものは `forward_opaque`)。

rendering 文書の root に `material_route_default` を置くと、automatic な material に当てる route を project 全体で変えられます。

| 値 | automatic な material の route |
|---|---|
| `"auto"`(省略時と同じ) | 上の規則のまま(key が無かったときと byte 一致) |
| `"forward"` | opaque → `forward_opaque`、blend または screen input → `forward_transparent`(`render_path: "forward"` と同じ規則) |

- `render_path` を明示した material には効きません(明示が常に勝ちます)。`pelican_cli dump-lowered-material` の `route:` 行は、
  既定で forward になった material を `reason=default_forward` と書きます(明示の forward は `explicit_forward`)。
  CLI に `--project <dir>` を付けると、その project の rendering 文書の既定で lowering します([第10章](10_tools.md))。
- **書ける場所は root だけ**です: preset を選ばない direct root と、preset を選ぶ root(`pelican.graph` root なら `rendering` の下)。
  preset 文書の `config` と feature に書くと名前付きで断られます(`render pipeline preset config cannot own material_route_default` /
  `render feature cannot declare material_route_default`)。値が `"auto"` / `"forward"` 以外なら
  `rendering config material_route_default must be "auto" or "forward"` です
  ([`readMaterialRouteDefault()`](../../src/project/renderpipeline.cpp#L652))。合成した config には残りません。
- **engine は起動時に 1 回だけ読みます**(renderer を作るとき。[起動時の snapshot](../../src/core/vkcore/renderer.cpp#L3201))。
  起動後に文書を書き換えても、編集 RPC で変えても、engine を起動し直すまで効きません。編集 RPC は `restart_required: true` を返し、
  Studio はそれを受けて engine を再起動します([第13章](13_editor.md) §13.5)。renderer を持たない構成(loader や tool の test)では
  `"auto"` と読みます。
- engine の組込 material には名前があり、拒否の文に出ます: `pelican.builtin.transparent`(透過の fallback)・
  `pelican.builtin.sphere`(sequence player の球)・`pelican.builtin.skinned_fallback`(material の無い skinned primitive)・
  `pelican.builtin.default`(material の無い glTF primitive)
  ([`StandardMaterialResource::builtinMaterialInfo()`](../../src/core/material/standardmaterialresource.cpp#L171))。
  `"forward"` のとき組込は `forward_opaque` で描かれ、その forward 版の shader は runtime shader compiler で作ります。
  **runtime shader compiler の無い build(`PELICAN_RUNTIME_SHADER_COMPILER=OFF`)では `"forward"` は使えず**、load は
  `material_route_default_unavailable@1` で止まります。

**受け皿の規則**: pipeline に material pass が 1 つでもあるなら、登録済みのすべての material(組込を含む)に、その route と
shader 契約を受ける pass が要ります。material pass が 1 つも無い pipeline(RT だけの graph など)は material を描かないので
この検査はしません。ただし**編集や reload で入れ替える候補の pipeline が material pass を 1 つも持たないときは、公開中の世代が
描いていた route を落としてはいけません**(同じ `material_route_unserved@1` で断ります。まだ何も公開していなければ通ります)
([2 つの規則を当てる requireEveryMaterialServed](../../src/core/material/materialgenerationplanner.cpp#L781))。G-4 の直後はこの
2 本目が抜けていて、material pass を全部外す候補が通っていました(G-4 の後の修正 `5ca18ee1` で戻しました)。
受け皿が無いと load は `material_route_unserved@1` の fatal で止まり、文は route と shader 契約・material の
名前と出所(組込は `(engine built-in)`)・直し方の hint を言います([load 時の fatal の文](../../src/core/material/materialgenerationplanner.cpp#L251))。
たとえば forward の pass だけの pipeline を `"auto"` のまま起動すると、
`material_route_unserved@1: rendering config '<参照>' has no material pass for route 'deferred_geometry' (gbuffer_v1), needed by '<名前>' ...
Hint: set material_route_default to "forward" on the rendering config root, or add a deferred_geometry_v1 material pass`
の形で止まります。編集 RPC は同じ規則を編集の候補にも当て、同じ code で断ります([第13章](13_editor.md) §13.5)。forward の pass
だけで組むときは `"forward"` を置けば automatic な material が forward の pass に集まります。ただし blend の material が
あれば、`forward_transparent_v1` の pass も要ります(deferred だけの構成で透過 material を救う仕組みはありません)。

### OpenPBR — 第 3 の標準サーフェス(✅WP116/117)

standard / toon に続く**特権なしの standard library surface** として、OpenPBR Surface **1.1.1(exact pin)** のサブセットが同梱されています。使い方は wrapper を surface 参照するだけです(実物: [../../projects/example/materials/openpbr_coat.material.json](../../projects/example/materials/openpbr_coat.material.json)):

```json
{ "schema": "pelican.material", "version": 1, "materials": [ {
    "name": "openpbr_1_1_1_coat_sphere",
    "surface": "engine://surfaces/openpbr/opaque_double.surface",
    "values": { "base_color": [0.12, 0.32, 0.72, 1.0], "coat_weight": 0.9, "coat_ior": 1.6 },
    "routing": { "alpha_mode": "opaque", "double_sided": true } } ] }
```

- wrapper は `{opaque,mask,blend}_{single,double}` の 6 種(BRDF 本体は単一 include `openpbr_lighting.glsl`)。v1 サブセット = base / specular / IOR / coat 1 層 / emission / normal + coat normal / opacity / alpha_cutoff。非対応入力(transmission 等)は authored かつ寄与時のみ名前入り WARN。写像表の正は [../openpbr_1_1_1_mapping.md](../openpbr_1_1_1_mapping.md)。
- `pelican.material` の additive キー: `textures`(宣言済みスロットの per-material 差し替え)/ `routing`(`alpha_mode` + `double_sided` — opaque/mask は depth write、blend は read-only)/ `defines`。
- **project material**: `asset_data.json` の `materials[]` へ文書を索引すると、起動時に surface lowering・shader compile・GPU 登録が行われます(✅WP240c)。texture 参照と material values/texture hot reload も runtime 経路に接続済みです。
- **primitive binding**: `pelican.material_bindings` v1(プリミティブ → マテリアル名の whole-model 契約)を asset_data の `models[].material_bindings` で参照できます(✅WP116/240c。fragment モデルには適用不可)。`binding.material` は glTF 内 named material と project material の両域を探索し、同名衝突は hard error、`binding.routing` の不一致も hard error です。
- **glTF 再現**: glTF material も同じ lowering 経路を通り、`alphaMode` / `alphaCutoff` / `doubleSided` は routing 6 状態から得ます。BLEND は `forward_transparent`、MASK は cutout wrapper へ流れます。

### 現状と重要な限界

- レンダラ接続は ✅ — `.surface` → コンパイル → パイプライン → SSBO → 描画まで golden(`surface_toon` / `skeletal_toon` / `openpbr_coat_sphere`)で実証済みです。
- project 宣言レーンは ✅WP240c — `.material.json` を起動時に読み込み、binding でモデルへ割り当てた OpenPBR を実 GPU golden まで検証しています。宣言例と strict `pelican.asset_data`(version 2 だけ。MG-2b から version 1 は `pelican.asset_data.version_unsupported@1` で断られます)は [第5章](05_assets.md) §5.2 を参照してください。
- **per-instance マテリアルオーバーライド**(factor / UV の乗算 + material 別の絶対上書き)は ✅WP122/122b で renderer 機構として実装済みです。公開の書き込み面は VRM application service([第8章](08_gameplay.md) §8.13)経由で、GameContext API はありません。
- **spv-link(M3b/WP80)は experimental**: build時に`PELICAN_WITH_SPIRV_LINK=ON`、実行時に環境変数 `PELICAN_SPV_LINK=experimental` の両方を明示した場合だけ SPIR-V リンクバックエンドに切り替わります。build unitの既定はOFF、runtimeの既定は常にソース経路です。
- 設計文書 [../design_material_shading.md](../design_material_shading.md) は v1.2 のまま実装が追い越しています。**現行契約の正は [../shader_contract.md](../shader_contract.md)** です。

## 6.8 テンポラルとスナップショット(✅T1/T2 = WP88/95、M3.5 = WP83)

### history RT(`@history` 読み)

**`@history` は RT 名に付ける接尾辞**で、「同じ RT の**前フレームの面**を読む」という意味です。RT 宣言に `"history": true` を付けると物理 2 面持ちになり(今フレーム用と前フレーム用が毎フレーム入れ替わる)、パスの `input` で `<名前>@history` と書くとその前フレームの面がバインドされます(`@history` はフレーム内依存を作りません)。実例(golden `temporal_accumulation` の feature):

```json
{ "schema": "pelican.render_feature", "version": 1, "name": "accumulation_fixture",
  "render_targets": [ { "name": "temporal_accum", "extent_scale": 1.0,
    "format": "R16G16B16A16_SFLOAT", "format_class": "explicit(R16G16B16A16_SFLOAT)", "role": "color",
    "usage": ["COLOR_ATTACHMENT", "SAMPLED"], "history": true, "clear_color": [0.0, 0.0, 0.0, 1.0] } ],
  "passes": [ { "insert": "after:post_main", "pass": {
    "name": "temporal_accumulate", "type": "fullscreen",
    "input": ["current_color", "temporal_accum@history"],
    "output": { "color": "temporal_accum", "depth": null },
    "shader": { "vertex": "shaders/fullscreen", "fragment": "shaders/accumulate" } } } ] }
```

### velocity

`"features": ["engine://features/velocity.json"]` の 1 行で、UV 空間モーションベクタ RT(`R16G16_SFLOAT`)と `velocity` パスが `before:post_main` に入ります。スキンドメッシュも正しい変形速度が出ます(前フレームの skin palette 保持 — ✅WP95)。set 0 の `PreviousObjectBuffer` と FrameUBO の `previous_view/projection` が対応する機構です。

### projection jitter(✅WP112/115)

feature JSON のトップレベルで宣言します(同時に有効化できる provider は 1 個。既定 off — 参照しなければ golden byte 不変):

```json
"projection_jitter": { "pattern": "halton23", "phases": 8 }
```

- 名前付きパターンは `halton23` — 基数 2 と 3 の **Halton 列**(乱数のように見えて偏りが小さい準乱数列)を x / y に使い、`phases` 個の位相を巡回する、という意味です([projectionjitter.cpp](../../src/core/renderer/projectionjitter.cpp#L11))。毎フレーム投影を 1 画素未満だけずらす目的なので、点が塊にならず均等に散ることが要件になります。**ユーザー定義数表**も同格です(✅WP115 — 名前付き系列に特権はなく、同じ数表を直書きすると全 byte 一致することが fixture で証明されています):

```json
"projection_jitter": { "pattern": "table", "offsets_px": [[0.0, -0.1667], [-0.25, 0.1667], [0.25, -0.3889]] }
```

(`offsets_px` は 1〜64 個・各成分 [-0.5, 0.5))

- **consumer 別配送**: メイン raster / SSAO / sprite は jittered、velocity はジッタ減算済み、**shadow・カリング・rpc(外部制御用の JSON-RPC プロトコル。[第10章](10_tools.md) §10.3)・ゲームプレイは非ジッタのまま**(カメラ公開 API 不変)。
- **reset epoch**: **epoch(エポック)** は単調増加するだけの整数で、「ここで連続性が切れた」を表す世代番号です([ソースコード読解ガイド §10.3](../source-code-guide/10_background_knowledge.md#単調増加する-generation--epoch-を変わったかの検出に使う))。初回・リサイズ・`set_time`・カメラカット等で FrameUBO の epoch ペア(今フレームと前フレームの値)が 1 フレームだけ不一致になるので、シェーダは自前で状態を持たずに「前フレームの絵は使えない」と判定できます。

### 標準 TAA(✅WP113)

TAA は**特権なしの標準 feature** です(エンジン本体は §6.8 冒頭の機構語彙だけを提供)。velocity と併せて 2 行で有効化できます:

```json
"features": [
  "engine://features/velocity.json",
  { "ref": "engine://features/taa.json",
    "parameters": { "scene_color": "lit_color", "velocity": "velocity",
                    "depth": "offscreen_depth", "downstream_color": "lit_color",
                    "alpha": 0.1, "disocclusion_tau": 0.1, "depth_epsilon": 0.00001 } }
]
```

全パラメータに default があるため `"engine://features/taa.json"` の 1 行でも動きます。`taa_resolve`(history 蓄積 + neighborhood clamp)→ `taa_composite` の二パスで、taa.json 自身が halton23/8 の jitter provider です。history 無効時(epoch 不一致・disocclusion)は current をそのまま出します。golden 7 件(静止・カメラ/オブジェクト移動・disocclusion・resize・set_time・ortho)で回帰保護。

> **設計決定(temporal のユーザー管理境界・2026-07-16):** taa.json・resolve/composite シェーダ・パラメータ・ジッタ系列は**全部ユーザー管理**(プロジェクトへコピーして改造したら自分のもの)。エンジン側に残るのは 6 つの固定語彙(jitter 適用点・単一 provider 排他・FrameUBO 供給 field・history flip・velocity の前フレームデータ・reset epoch)だけで、判定基準は「5 年後に発展しているのはどちら側か」([../design_taa_jitter.md](../design_taa_jitter.md) §0-1、レシピは [../adding_features.md](../adding_features.md))。

※ example の main config は TAA を有効化していません(必要なプロジェクトが 1 行足す方式)。OpenXR 起動時は TAA / jitter 系 feature は XR graph から自動除外されます(§6.12)。

### per-instance マテリアルオーバーライド(✅WP122/122b — 機構)

インスタンス単位の factor / UV 乗算オーバーライドと、`(インスタンス, glTF material index)` 単位の絶対上書きが renderer 機構として入っています(N/N-1 履歴付き — VRM 表情の適用先)。公開の書き込み面は VRM application service([第8章](08_gameplay.md) §8.13)で、rendering config に書くものはありません。

### 名前付きスクリーンスナップショット(屈折・歪みの入口)

```json
"snapshots": [ { "name": "opaque_color", "after": "opaque" } ]
```

合成時に `snapshot_copy` ノードへ展開され、`.surface` 側の宣言と自動生成サンプラで読めます:

```glsl
//! screen_inputs: [opaque_color]
vec4 behind = pelican_screen_opaque_color(surface_input.uv + offset);
```

(実物: `projects/example/shaders/refract.surface`)。公開アクセサは物理descriptorを固定しません。
組み込みcontractが`same_pixel`でtarget plannerもtile-localを選んだ入力は、
同じ関数がinput attachment readになります。`opaque_color`は屈折offsetを許す
`neighborhood` contract(周囲まで読む read footprint。値の一覧は §6.7「compute/別passのresourceをmaterialから読む」)なので、この例は通常のsamplerを維持します。

v1 の制限: snapshot は **1 個だけ・不透明描画後の 1 点のみ**(透明描画後・
`pelican_ui` 以後はエラー)、逐次屈折は非対応です。

**`after` に書ける名前(✅FR-1、`45ca769b`)。**`after` は pass 名か canonical anchor 名です。

- 書ける名前は、各 family の `post_ldr` の anchor から `pelican_ui` の anchor の手前までの frame node のうち、全部の family にあるものです(anchor は anchor 名、pass は pass 名。snapshot の copy は除く)。`get_render_authoring_context` の `snapshot_after_candidates` が、最初の family の順で返します([`renderSnapshotAfterCandidates()`](../../src/project/featurecompose.cpp#L4435))。
- 無い名前は `snapshot.after_unknown@1` `{snapshot, after, resolved?, candidates}` で断られます([FR-1 の snapshot の拒否](../../src/project/featurecompose.cpp#L611))。`resolved` は `after` が anchor 名のときだけ付き、その anchor の frame node `__anchor_<anchor>` です。`candidates` は上の名前です。FR-1 の前は `snapshot '<名前>' copy point was not found: <after>` の文でした。名前はあるが範囲の外(`post_ldr` より前か `pelican_ui` 以後)のときは、今も code の無い `... is not the opaque post_ldr region ...` です。
- 例(test): Blank 相当の graph の pipeline(`deferred_lighting` と `present` の 2 pass)に `after: "opaque"` を書くと `candidates` は `["post_ldr"]`。swapchain に書く pass を持つ構成では `["post_ldr", "present"]` です。
- **直った不具合**: FR-1 の前は、anchor 名を `after` に書くと、copy の node の `snapshot_after` に anchor の素の名前(`post_ldr`)が入りました。その名前の frame node は無いので、実行計画が依存を断っていました。今は `__anchor_post_ldr` が入ります([FR-1 で直した所](../../src/project/featurecompose.cpp#L634))。snapshot の展開は合成の 1 か所(`materializeSnapshots()`)で、pipeline の作り方を見ません。なので、anchor 名を書いた snapshot はどの pipeline でも同じように落ちていました。
- test: `featurecompose_test` の [「FR-1 a snapshot after a name the pipeline lacks is refused with the names it takes」](../../test/featurecompose_test.cpp#L10033)(候補のどれを書いても合成が通り、実行計画に `__anchor_post_ldr` → copy の `pelican.dependency.snapshot_after@1` の依存があること)。

## 6.9 2D スプライト(✅S2D = WP103/104/106/109)

スプライトの描画は **feature 1 行 + シーンコンポーネント**です。新しいパス型はありません。

```json
"features": [ "engine://features/sprite.json" ]
```

(実物: [../../projects/sprite_demo/passes/main.json](../../projects/sprite_demo/passes/main.json))

`sprite.json` は RT もパスも足さない**純ゲート**で、参照すると canonical anchor(§6.5)の 8 個のうち先頭の `sprite` の位置(3D 不透明の後・ポスト処理の前)で、直近の scene color + depth に対して depth test ON / write OFF で描かれます(color/depth が無い構成では名指しエラー)。

- **コンポーネント** `sprite_view`(scene v2・closed schema): `texture`(必須。`"demo_atlas#sprite/full"` 形式の atlas 参照)/ `size`(vec2)/ `pivot`(既定 [0.5,0.5])/ `color` / `flip`([flip_x, flip_y])/ `layer`(int16)/ `billboard`(`none|y_axis|full`)。スプライトは常に **XY 平面 + Z 法線**(2026-07-12 決定。床置きは Transform の回転で表現)。画像の上の行は sprite の local +Y 側(上)に出ます。`pivot` と画像の角は画像の空間(左上が原点、y 下、0..1)で測るので、`pivot` の y は 0 が画像の上端、1 が下端です(✅CC-1 `c8367b87` で [quad を作る所](../../src/core/renderer/spriterenderer.cpp#L79) の y を反転。以前は画像の上の行が local −Y 側に置かれていました)。ランタイム生成は `GameContext::createSpriteObject(...)`([第8章](08_gameplay.md))。
- **pixel policy**(✅WP106): `project.json` の `basic_config.sprite.pixels_per_unit`(既定 100.0)と、カメラの `sprite: {"pixel_perfect": "off"|"strict", "sort": "z"|"y_down"|"declaration"}`。`strict` の成立条件(正射影・整数ズーム・nearest・非 billboard 等)は、どれも「テクスチャの 1 テクセルが画面上のちょうど整数個のピクセルに落ちる」ために要るものです — 実際の判定は `zoom = (1 / pixels_per_unit) / (1 ピクセルあたりのワールド長)` を計算し、これが**整数・1 以上・縦横で同値**であることを確かめます([pixelpolicy.cpp](../../src/core/userpublic/sprite/pixelpolicy.cpp#L24))。満たされない場合は **silent fallback せず**理由付き WARN + `get_status.sprite` に状態が出ます。
- **flipbook**(✅WP106): コンポーネントではなく公開 stdlib API `Pelican::sprite::FlipbookClip`(ゲームコードが決定的な local time を渡す)。実例: `projects/sprite_demo/code/flipbook_demo.cpp`。
- 実例プロジェクト: [../../projects/sprite_demo](../../projects/sprite_demo)(WP109 の横スクロール vertical slice。操作系は [第8章](08_gameplay.md))。golden はスプライト系 9 ケースで回帰保護。

## 6.10 ヘッドレス描画とテスト(✅WP1〜8/16)

```sh
pelican_player --headless --project mygame --frames 3 --size 1280x720 --render-out out.png
```

- パス JSON の `"swapchain"` 出力は自動的にオフスクリーンイメージへ解決されるので、**config は無変更で動きます**。出力 PNG は encoded-sRGB(§6.3)。
- ゴールデンイメージテストは **52 ケース**(2026-08-20 時点。**この数字はここが正本**)。プラン比較テストとあわせて回帰保護の柱です。検証の流儀は [../rendering_phase1_review.md](../rendering_phase1_review.md)。

> ⚠ **変更(✅WP141):** ケース集合の正本はコミット済みの **`test/golden/inventory.json`**(`pelican.golden_inventory` v1)になりました。**`test/golden/` にディレクトリを置くだけでは発見されません** — 未登録のディレクトリは描画テストの対象にならず、CPU 側の inventory ゲートが名指しで FAIL します。ケースを増減したら次の手順を踏みます。
>
> ```powershell
> python -B test/golden_inventory.py --repo-root .            # 照合(PASS / FAIL + 理由列挙)
> python -B test/golden_inventory.py --repo-root . --update   # 意図した変更のあと manifest を再生成
> git diff -- test/golden/inventory.json                   # diff をレビューしてコミット
> ```
>
> manifest はケースごとに `name` / `mode` / `files` / `expected_png_sha256` / `tolerance` / `vat`(`on_and_off` か `on_only`)/ `traces` を宣言します。照合は **GPU 不要**で、`ctest -LE gpu` と CPU CI ゲートの両方で毎回走ります。`PELICAN_UPDATE_GOLDEN` などの更新モードも manifest に載ったケースだけを回り、**manifest の再生成が `expected.png` を書き換えることはありません**(画像とトレースの再ベースラインは従来どおり [../design_color_pipeline.md](../design_color_pipeline.md) §3 の 6 段階手順です — `test/golden/README.md`)。

> **設計決定(レイヤ規則):** `src/core/vkcore`(Vulkan 低層)から `src/core/renderingpass` 以上のレイヤへ include を追加しない。下位層はフラグを上げるだけで、編成は上位層 Renderer が行う。

## 6.11 トラブルシューティング

| エラー(抜粋) | 原因 |
|---|---|
| `Rendering pass not found: <name>` | `default_rendering_pass` の名前が rendering config のパス列に無い |
| `Default rendering pass has no terminal output_transform: <name>` | 既定のパス列の終端が `output_transform` ノードになっていない |
| `Pass input target is not produced as an earlier output: display` | authored な `"swapchain"` 出力は `display` RT へ置換されるため、`display` を誰も書かない構成はここで落ちる(§6.1) |
| `uses an explicit file extension; use an extensionless ... shader stem` | shader 参照に拡張子を書いた |
| `Shader stem could not be resolved: ... Tried: ...` | stem のパスミス(試行一覧がエラーに含まれる) |
| `Pass '<名前>' has unknown type '<type>'` | `type` の typo(§6.2 の一覧参照) |
| `Pass '<名前>' type '<type>' does not own field '<鍵>'` | その type が所有していない鍵を書いた(§6.2)。**形の規則の正本は `src/project/passshapepolicy.*` で、違反は `Pass shape violation '<違反名>' in pass '<名前>'` の形で出る** |
| `swapchain_input` / `swapchain_depth_output` | `swapchain` を input か depth 出力に使った。**color 出力には使える** |
| `duplicate_input` / `duplicate_color_output` | 同じ名前を 2 度書いた。**`@history` を剥がした後の名前で判定する** |
| `current_frame_color_feedback` / `current_frame_depth_feedback` | 同じ RT を読み書きした。**`@history` 付きの読みは免除される** |
| `<pass> requires a draw object` / `Raster pass requires shader: <pass>` | `raster` パスに `draw` / `shader` が無い |
| `Raster pass shader implementation must use canonical namespace.name@major syntax` | `shader.implementation` が `namespace.name@major` 形式でない |
| `raster_state color_attachments count must match pass color outputs` | `color_attachments` の要素数が `output.color` の枚数と違う |
| `Fullscreen pass raster_state does not support depth field '<鍵>': <名前>` | `fullscreen` / `output_transform` の `raster_state` に範囲外の `depth_*` を書いた |
| `Fullscreen pass '<名前>' cannot enable blending for integer color output <添字>` | UINT/SINT color target に対してblendを有効にした。`opaque`にするかfloating-point targetを使う |
| `Pass '<名前>' type '<type>' does not own field 'implementation'` | `implementation` を `fullscreen` / `output_transform` 以外の type に書いた(§6.2) |
| `tagged region v1 supports contiguous fullscreen passes only` / `... requires contiguous authored passes` | `regions` タグが非連続、または fullscreen 以外のパスに付いている(§6.2) |
| `resolved draw_sort has unknown xr_view_policy: <x>` / `XR per_view draw sorting requires exactly two views` | `draw_sort.xr_view_policy` の値が不正 / `per_view` を view 数 2 以外で使った(§6.2) |
| `Only rendering resolver_version 2 is supported` | `resolver_version` に 2 以外を書いた |
| `feature.insertion_anchor_missing@1` `{feature, anchor, candidates, document}`(FR-2 の前は `insert anchor was not found: <name>`) | feature の挿入先が pipeline に無い(標準アンカー 8 個 + パス名が有効)。`candidates` が書ける名前(✅FR-2。§6.5「feature が pipeline に求める target と挿入先」) |
| `feature.required_target_missing@1` `{feature, target, type, document}`(FR-1 の前は `render target override references unknown render target: <name>`) | feature が読み書きする target を pipeline が宣言していない。`type` は `render.target_ref` に書く型(✅FR-1。同上) |
| `feature.required_target_unwritten@1` `{feature, target, type, writer_candidates, document}`(FR-3 の前は frame planner の `Pass input target is not produced as an earlier output: <name> in pass: <pass>`) | feature が読む target は宣言されているが、pipeline の誰も書いていない。`writer_candidates` の出力の 1 つをその target へ繋ぎ替える(✅FR-3。同上の「書き手」) |
| `Ambiguous writes-writes dependency for resource X between A and B` | 書き込み順が導出不能。after/before か中間リソースで解消 |
| `@history is supported only for render targets: ...` | バッファに `@history` を付けた |
| `Unknown or non-history resource reference ...@history` | `history: true` でない RT を `@history` 参照した |
| `sprite feature requires a scene color and depth attachment before sprite anchor` | sprite anchor の前に color+depth を出すパスが無い |
| snapshot 系エラー(2 個目・透明後・未定義名) | §6.8 の v1 制限違反。未定義名は ✅FR-1 から `snapshot.after_unknown@1`(§6.8「`after` に書ける名前」) |
| pixel policy の WARN | strict 条件不成立(silent fallback しない方針) |
| `Shader defines and generated includes require GLSL source, not SPIR-V` | `.spv` しか無い環境で feature(defines)を使った。**生成 include**(`pelican_resource_ports.glsl`)を要るパスも同じ一文で断られます([shaderlibrary.cpp:344](../../src/core/shader/shaderlibrary.cpp#L352)、engine:// 側は [:528](../../src/core/shader/shaderlibrary.cpp#L538)) |
| `Shader descriptor binding mismatch` | vert/frag で同一 binding の型が不一致 |
| `Shader/pipeline transaction failed; keeping the previous generation: ...` | ホットリロード失敗(旧版継続中。ソースを直せば次の保存で回復) |
| `placeholder has no resolved binding` / parameter 名入り compose エラー | feature の `$名前` に対応する binding / default が無い(§6.5) |
| projection jitter provider の重複エラー | `projection_jitter` を宣言する feature を 2 個以上参照した(単一 provider 排他) |
| `OpenXR activation rejected history feature '<name>'` ほか XR activation 拒否 | XR graph から除外できない temporal / UI 構成(§6.12) |
| `runtime-selected physical device lacks Vulkan feature timelineSemaphore required by OpenXR` | OpenXR ランタイムが選んだ GPU が `timelineSemaphore` 非対応(✅WP138 で silent 失敗から名指しエラーへ) |
| `preview graph '<config>' rejects authored projection_jitter` / `... rejects authored history target '<name>'` / `... rejects authored unsafe target '<name>'` | preview graph に載せられない構成を rendering config が直書きしている(§6.13)。✅PV-1 からこれは起動の失敗ではありません。通常の再生は起動し、この文は warning の行と `get_status` / `get_frame_plan` の `preview.reason.message` に出ます(`render_preview` だけが使えない) |
| `preview graph '<列名>' rejects authored pass '<パス名>': <理由>` | 同上(理由は `unsafe pass type/name <type>` / `history read` / `history write`。✅PV-1 から起動は止まらない)|
| `render preview unavailable (preview.unsupported_authoring@1 feature=<feature> target=<name>); normal play continues, render_preview refuses: <理由の文>`(WARN) | ✅PV-1。上の 2 行の構成で、通常の再生は続き、単発 preview(`render_preview`)だけが使えない。`target=` は reason に `target` があるときだけ(§6.13 の PV-1 の段落) |
| `preview graph config requires rendering_passes` | 合成後の config に `rendering_passes` が無い(§6.13) |
| `golden inventory: FAIL` + `- <issue>` | `test/golden/inventory.json` と `test/golden/` の実体が食い違う(§6.10) |
| `render_config_section_unlocated`(RPC の拒否コード) | `pelican.graph` root なのに `rendering` 節が無い(あるいは object でない)文書を authoring RPC が編集しようとした。**黙って root にキーを作らない**([renderconfigeditor.cpp:887](../../src/core/communication/renderconfigeditor.cpp#L1424)。§6.16) |
| `render_config.section_unlocated@1` | 同じ理由で `--feature-overlay` の適用先が見つからない([renderfeatureoverlay.cpp:142](../../src/project/renderfeatureoverlay.cpp#L146)) |
| `graph.operation_unregistered@1` `{node, op}` | ノードの `op` をどの module も登録していない。綴り間違いのほか、**`PELICAN_WITH_ANIMATION=OFF` の build で `pose.*` を書いた**ときもこれ([graphmodule.cpp:257](../../src/project/graphmodule.cpp#L494)。§6.17) |
| `graph.operation_unexecutable@1` `{backend, op, node}` | op は登録されているのに、それを実行する backend がこの build に無い。rendering 側は `backend: "pelican.render"` で、lowering できる 6 op 以外を rendering 区画に置いたときに出ます([rendergraphcomposition.cpp:58](../../src/project/rendergraphcomposition.cpp#L719))。姿勢側・値側にも同名の拒否があります([posegraph.cpp:91](../../src/core/animation/posegraph.cpp#L91) / [valuegraph.cpp:289](../../src/core/userpublic/valuegraph.cpp#L555)) |
| `graph.node_duplicate@1` / `graph.node_missing@1` / `graph.input_unknown@1` / `graph.input_referenced@1` / `graph.output_referenced@1` / `graph.nested_instance@1` / `graph.input_scope@1` / `graph.definition_immutable@1` / `graph.layout_invalid@1` | `edit_graph_document` の **9 つ**の名前付き拒否(重複 id / 不在 node / 不明な入力 port / まだ供給されている `graph.input` の削除 / まだ参照されている node の削除 / definition の中の `graph.instance` / root の `graph.input` / 編集できない scope)。正は [`editGraphDocument()` の宣言コメント](../../src/project/graphcomposition.hpp#L245)(9 つ全部を列挙しています。9 つ目 `graph.layout_invalid@1` は N-6 の `set_layout`)と throw 箇所([graphcomposition.cpp:363](../../src/project/graphcomposition.cpp#L1502) 〜 [:534](../../src/project/graphcomposition.cpp#L2350))。一覧は [第13章](13_editor.md) |
| `graph.override_shadowed@1` `{parameter}` | edge が供給している入力に `setGraphParameter` で実行時の上書きをかけた。**黙って消えない**([scenegraphruntime.cpp:123](../../src/core/loader/scenegraphruntime.cpp#L129)。§6.17) |
| `unconnected graph input: <node>.<port>` / `unconnected subgraph input: <名前>` | 必須入力に edge も `parameters` も `default` も無い([graphcomposition.cpp:323](../../src/project/graphcomposition.cpp#L1445)、[:59](../../src/project/graphcomposition.cpp#L698)) |
| `unknown graph definition: <名前>` / `recursive graph definition: <名前>` / `cyclic subgraph connection: <id>` | `graph.instance.definition` の綴り違い / 定義の再帰 / インスタンス同士の循環([graphcomposition.cpp:93](../../src/project/graphcomposition.cpp#L765) / [:108](../../src/project/graphcomposition.cpp#L788) / [:90](../../src/project/graphcomposition.cpp#L762)) |

## 6.12 OpenXR ステレオレンダリング(✅WP125〜138 / WP203a〜c実装済み)

`--xr on|auto` で起動すると([第2章](02_getting_started.md))、レンダラは**論理フレーム**単位の二眼描画に切り替わります。**論理フレーム**は「左右眼など複数の view をまとめた、時間軸上の 1 単位」です。flat では view が 1 つなので論理フレーム = 描画 1 回ですが、XR では 1 論理フレームが 2 回の view 描画を含みます。アニメーションの更新やテンポラル history の入れ替えのような**時刻に紐づく処理を左右で 2 回走らせない**ための区切りだと考えてください。**ViewFamily** はその中の「同じカメラ設定を共有する view の束」(`$main` の左右眼、影の cascade 群、反射の鏡像群など)です([用語集](14_glossary.md))。

- **論理フレーム / ViewFamily(WP128/WP223〜237)**: `renderLogicalFrame(target, view_families)`が共有更新(アニメ・リロード・共有アップロード・**temporal historyのadvance**)を論理フレームにつき**一回**だけ行います。flatは`$main/$mono`、XRはstable eye ID付き`$main` stereo familyです。pass/taskの`view_family`は汎用runtime registryで解決され、caller-authored familyが常に優先されます。標準directional shadowは`$shadow/directional`のstable `$cascade/N`群、標準planar reflection packageは`$reflection/planar`のstable `$mirror/<source-view>`群、標準cube captureは`$capture/cube`のstable `$face/{+x,-x,+y,-y,+z,-z}`群としてmain cameraから独立して実行されます。projection jitterはmain family modifierとして一度sampleされ、FrameUBOはin-flight × 全family viewのスロットとstable family tokenで相互汚染を防ぎます。clustered selectionもfamily/view別領域を検証して使います。標準planar providerはclip planeをVulkan ZOのoblique near planeへ変換し、cube providerはsquare 90度投影の六方向cameraを作ります。どちらもpackage OFF時はcaller/project providerで全面置換できます。secondary familyの複数viewはsequential実行に対応し、transparent passを持つfamilyは同じ公開sort providerをviewごとに再評価します。secondary multiviewは未対応です。
- **コンポジション(WP129/203c)**: XR 用は `IFrameTarget` とは別系統の `IXrCompositionTarget`。現在は **2-layer の color array swapchain を一個**使い、一回だけ acquire/wait/release します。左右の projection view は同じ image の `imageArrayIndex=0/1` を参照し、1 つの projection layer・**単一の `xrEndFrame`** で提出します。
- **optional composition depth(WP203c)**: `XR_KHR_composition_layer_depth`、compiled graph の external depth export、OpenXR/Vulkan の format/usage 条件が成立すると 2-layer depth swapchain を作ります。各フレームで source format/extent も一致したときだけ有効化し、sequential 描画なら layer ごと、multiview 描画なら array 全体を copy して左右の `XrCompositionLayerDepthInfoKHR` を提出します。不一致なら利用可能な depth swapchain も idle のままにし、color-only へ戻ります。
- **view execution(WP203a〜c)**: `xr.view_execution` は `"auto"` / `"sequential"` / `"multiview"`。`auto` は対応済み scope だけを multiview にし、material/custom pass は capability を明示するまで sequential のまま混在実行します。required `"multiview"` は対応不能な device/pass を fallback せず compile error にします。選択根拠は `get_frame_plan` の `physical_target_plan.view_execution_plan.auto_gate` で確認できます。
- **座標系(WP131)**: `world_from_stage = inverse(フレーム開始時の active camera view)` — flat のカメラ API は不変のまま、XR アダプタ内でのみ変換します。reference space は STAGE → LOCAL_FLOOR → LOCAL の優先選択で、LOCAL への fallback 時は「床は非保証」を `get_status.xr` が明示します。✅CC-1(`c8367b87`)から flat の camera の view は transform の逆行列(−Z 前方、up は回転から)なので、stage は active camera の transform(位置と回転)に重なります。以前は camera の回転の +Z が前方で、up は project の設定から取っていました。両眼の射影は以前から clip の y を反転していて、CC-1 で flat と同じ [`vulkanAsymmetric()`](../../src/core/renderer/clipspace.hpp#L50) に寄せました。面の表を反時計回りにした修正で、以前の XR は表の面を cull していた疑いが解けました(commit の記録では、XR の SSAO の旧 byte golden は面の表が cull されて物体が写っていなかった。実機では未確認)。
- **feature policy(WP133)**: flat / XR の両グラフを起動時にコンパイルし、**XR グラフからは TAA・projection jitter・velocity・history・UI の feature を自動除外**します。除外できない構成(config 直書きの history 読み・UI パス・未知の history feature)は XR 起動を名指しで拒否します — **history を持つ自作 feature は `--xr on` を止める**ことに注意してください(vrm_xr_demo の config が features 空配列なのはこのため)。XR の出入り境界では temporal reset が 1 回入ります。
- **ミラー(WP133)**: デスクトップウィンドウには左眼の best-effort ミラー(scale + letterbox、UI はミラー側にのみ重畳)。ウィンドウが詰まっても **HMD のフレームループは待たされません**。XR 中の従来 capture は名指しで拒否されます(headless / golden は常に flat 経路)。
- **ImGui は XR 中に出ません(✅WP138・v1 仕様)**: XR グラフには `imgui` パスが無いため、**XR session が active な間はエンジンが ImGui frame を begin しません**([src/core/imgui/imguiruntime.cpp](../../src/core/imgui/imguiruntime.cpp) の `isImGuiRuntimeEnabled()` が headless / rpc / リプレイ / golden と同列で `xr_active` を弾きます)。論理フレーム境界でゲートが閉じた場合、開始済みのフレームは `endFrameIfStarted()` で閉じられます。ミラーへの ImGui 表示は将来の別 WP です([第7章](07_input_ui.md) §7.1)。
- **実ランタイム検証(✅WP136/138)**: Meta XR Simulator **v201.0** 上の旧 sequential composition 経路は検証済みです — `FOCUSED` 到達 / `shouldRender=true` / 連続 1000 XR フレーム / デスクトップミラー 1000 present・0 drop・0 failure / 190.5 秒連続運転 / Vulkan validation エラー 0。Simulator 検証で見つかった 3 つの blocker(XR 中の ImGui frame 不整合・`timelineSemaphore` 未有効化による validation エラー・診断ログが無く観測できないこと)は WP138 で修正済みです。
- ⚠ 制限: WP203c の array color/depth composition と multiview は protocol fake・synthetic Vulkan fixture まで通過していますが、**この現実装を Meta XR Simulator と物理 HMD(Quest 3 Link 等)で表示確認する gate は未実施**です。focus loss / regain も未確認です(Simulator v201.0 が focus-loss イベントを発行しないランタイム制約のため、エンジン側の契約は headless の fake fixture で固定しています)。world-space UI は 📐。session loss 時の再生成は loop に未配線で、現状は安全に終了します。

`auto` を実測で調整する場合は rendering config の `xr` に profile を置きます。

```json
{
  "xr": {
    "view_execution": "auto",
    "multiview_auto": {
      "minimum_gain_percent": 2.0,
      "profiles": [
        {
          "id": "quest3_link_main",
          "vendor_id": 4318,
          "device_id": 9860,
          "driver_version": 123456,
          "device_name_contains": "GeForce",
          "graph": "main#xr",
          "measurement": {
            "sequential_gpu_ms": 8.0,
            "multiview_gpu_ms": 6.5,
            "sample_count": 120,
            "source": "get_status.gpu_timing"
          }
        }
      ]
    }
  }
}
```

`vendor_id` は必須、`device_id` / `driver_version` / `device_name_contains` / `graph` は任意です。より多くの任意条件が一致する profile を優先し、同じ具体度なら先に書いた profile が勝ちます。一致 profile が無ければ optimize-by-default で multiview、一致すれば `minimum_gain_percent` 以上かつ strictly faster のときだけ multiview を選びます。

測定は次のように**別プロセス**で行います。同じ 120-frame ring に二つの mode を混ぜると平均から分離できません。

1. `engine://features/gpu_timing.json` を有効にし、`view_execution: "sequential"` で起動します。warm-up 後 120 frame 以上動かし、`get_status.gpu_timing.logical_frame_averages[]` の XR graph の `average_total_ms` と `frame_count` を控えます。
2. process を終了し、測定 profile を一旦外した `view_execution: "auto"` で同じ scene を起動します。profile 未一致時の optimize-by-default が、対応 scope の multiview 候補です。同じ方法で平均を控えます。全 pass が対応する構成では required `"multiview"` も使えます。
3. device 値は `get_frame_plan.physical_target_plan.view_execution_plan.auto_gate.device` から写し、二つの平均と十分な小さい方の `frame_count` を profile の measurement に記録します。
4. `view_execution: "auto"` で再起動し、`auto_gate.selection` / `profile_id` / `measured_gain_percent` / `reason` が意図どおりか確認します。

## 6.13 グラフ variant(flat / XR / preview)(✅WP133/172)

同じ rendering config から、起動時に**最大 3 つのグラフ**が合成・検証されます([src/core/vkcore/renderer_config.cpp](../../src/core/vkcore/renderer_config.cpp) の `loadRenderGraphVariantsFromConfig()`)。

| variant | いつ作られるか | 実行するもの |
|---|---|---|
| `flat` | 常に | 通常の描画([第2章](02_getting_started.md)の起動すべて) |
| `xr` | `--xr on/auto` で XR session が立つときだけ | 二眼描画 + ミラー(§6.12)。パス列名(`rendering_passes[].name`)と compute task 名に `#xr` サフィックスが付きます(個々のパス名は不変) |
| `preview` | **常に**(XR / headless / rpc を問わず起動時に必ずコンパイル) | エディタのプレビュー描画(`render_preview`)。[第13章](13_editor.md) |

> **設計決定(preview は共有状態を持たないデータプログラム):** preview graph は `flat` / `xr` と違って `RenderingPassId` を持たず、**共有レンダーターゲットもパスも一切登録しません**。合成結果(`PreviewGraphProgram` = `{name, generation, pass_names, excluded_feature_names, composed_config}`)は**データとして保持**され、リクエストごとのリソースに対して実行されます。したがって `Renderer::renderLogicalFrame` の経路には入らず、通常描画のスループットにも決定性にも影響しません([src/core/renderingpass/previewgraph.hpp](../../src/core/renderingpass/previewgraph.hpp))。

preview graph の合成規則(すべて起動時に検証されます):

- 終端の `"swapchain"` 出力は**リクエストローカルな `preview_capture`** へ書き換えられます。swapchain イメージ・ミラーシンク・present キュー・共有 descriptor view は preview のプログラムに表現できません。
- feature は次のどれかに当たると**まるごと除外**されます(除外名は `excluded_feature_names` に記録され、composer の依存/アンカー検証はそのまま働きます。[preview と xr の判定](../../src/project/graphvariantpolicy.cpp#L623))。括弧の中は理由の名前です。
  - feature 名が下の marker を語として含む(`unsafe_name`)
  - `history: true` の RT を宣言する(`history`)/ `projection_jitter` を宣言する(`projection_jitter`)
  - 名前に `velocity` / `motion_vector` を含む RT(ここだけは部分一致)か、`type: "velocity"` の pass を宣言する(`velocity`)
  - `type: "ui"` の pass か、`insert` が `after:pelican_ui` / `before:pelican_ui` と**完全に一致する** pass を宣言する(`user_interface`)
  - 宣言した RT の名前、または pass の `name` / `type` が marker を語として含む(`unsafe_surface`)
- marker は `taa` / `velocity` / `motion_vector` / `projection_jitter` / `ui` / `imgui` / `mirror` / `present` で、✅G-6 から**語単位**で照合します。名前を英数字以外の文字・小文字から大文字への境・文字と数字の境で語に切り、小文字にして、marker の語の並びが続けて現れるかを見ます。marker の最後の語は末尾に `s` が付いても一致します(`motion_vectors` など)。`fluid_*`・`build_*`・`presentation` は一致せず、`final_present`・`debugUI` は一致します([語単位の照合](../../src/project/graphvariantpolicy.cpp#L79))。
- pass の `insert` の文字列は、上の `pelican_ui` の 2 つを除いて判定に使いません。`insert: "before:present"` の feature は除外されません。swapchain に書くことも除外の理由ではありません。`hdr`・`bloom`・`debug_draw`・`gizmo` は swapchain に書きますが、上の書き換えで `preview_capture` に書いて残ります。
- **除外は依存する feature に伝わります(✅G-6)。**除外された feature の pass・compute task・RT・buffer を後の feature が使っていれば、その feature も同じ変種で除外されます。「使う」は、`insert` の anchor にする / pass・compute task の `after` / `before` に書く / `pass_overrides`・`render_target_overrides` の key にする / pass・compute task の `input` / `output` で読み書きする(`<名前>@history` も)のどれかです。理由は `dependency` で、解決結果の診断 `feature_excluded` の detail は `preview:dependency:<依存先の feature 名>` です(この診断が RPC に出るかは未確認)。canonical anchor(`after:post_main` など)は、同じ名前の pass が feature にあっても依存になりません。`features[]` の順に 1 回走査して決めるので、依存で除外された feature に依存する feature も同じ走査で除外されます。xr 変種も同じ規則です(`xr:dependency:<名前>`。[依存の検出](../../src/project/graphvariantpolicy.cpp#L832))。
- 編集 RPC は、候補を flat(と、起動時に XR を始めた engine では xr)で合成してから受けます([第13章](13_editor.md))。✅PV-1 の前は preview でも合成し、preview で合成できない編集を `render_pipeline_preflight_failed` に `variant: "preview"` を付けて断っていました。今はそういう編集も committed になり、結果の `preview` が `available: false` になります(下の PV-1 の段落)。
- **config へ直書き**した同種の構成は除外できません。✅PV-1 からは起動エラーではなく、preview を利用できない状態として記録し、通常の再生は起動します(下の PV-1 の段落。PV-1 の前は §6.11 の `preview graph ...` 行の名指しの起動エラーでした)。`@history` の読み書きも同様です。ただし名前が `present` で終わる終端パスだけは、出力が request-local な終端(`preview_capture` / `display`)に解決されていて `mirror` / `ui` を語として含まない場合に限り保持されます。直書きの pass の名前も、上と同じ語単位で照合します。

合成結果には `generation`(合成後 config の FNV ハッシュ)が付き、`render_preview` の応答に `graph_generation` として返ります。config を編集して再合成すると値が変わるので、エディタ側の再取得判定に使えます。

**preview を組めない構成でも通常の再生は起動する(✅PV-1、`a173e70c`)。**

- **背景**: 利用者の intro link の比較で見つかりました。`history: true` の render target を root に直書きした project を player で起動すると、通常の再生でも `preview graph '<config>' rejects authored history target 'accum'` で起動できませんでした。通常の起動でも preview の変種を含む family を組み、preview の検査の例外で登録全体が落ちていたためです(仕様書の記述)。
- **今の扱い**: preview の検査([`validatePreviewConfig()`](../../src/project/graphvariantpolicy.cpp#L317))そのものは変わらず、断る構成を preview で走らせることはしません。断りは [`PreviewUnsupportedAuthoringError`](../../src/project/graphvariantpolicy.hpp#L235) になり、compiler program は preview の変種を組めないとき、それを「利用できない」と記録して flat / xr を今までどおり登録・公開します([PV-1 で preview の失敗を記録する所](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L986)、[登録の段で unavailable の preview を作る所](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1200))。flat / xr が組めない構成は今までどおり失敗です。
- **受け止める失敗(✅RG-3、`7a12fc2a`)**: PV-1 の compiler program は preview の変種の失敗を全部受け止めていたため、preview と関係の無い authoring の拒否(commit の記録では `does not own field 'gpu_draw_source'`)まで外に出なくなっていました。今は `PreviewUnsupportedAuthoringError` は常に「利用できない」と記録し、それ以外の失敗は、同じ呼び出しで runtime の変種(flat / xr)が先に組めた後だけ記録します(下の `composition`)。runtime の変種が組めていなければ、flat と同じように投げます([RG-3 で投げ直す所](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L1011))。登録は runtime の変種を先に並べるので(コードの注記)、起動の扱いは変わりません。runtime の変種の無い呼び出しがどこから来るかは未確認です。
- **状態の形**([`previewGraphStatusJson()`](../../src/core/renderingpass/previewgraph.cpp#L123)、reason は [`previewUnsupportedAuthoringReason()`](../../src/core/renderingpass/previewgraph.cpp#L82)): 組めたときは `preview: {available: true}`、組めないときは `preview: {available: false, reason: {code: "preview.unsupported_authoring@1", feature, target?, graph?, pass?, cause?, message}}` です。`message` は断った文そのものです。`feature` は次のどれかです。
  - `history`: `history: true` の render target(`target` に名前)
  - `projection_jitter`: `projection_jitter` の直書き
  - `unsafe_target_name`: 除外の marker を名前に含む render target(`target`)
  - `unsafe_pass`: 名前か type が除外の面に当たる pass(`graph`・`pass`)
  - `history_read`: `<target>@history` を読む pass(`graph`・`pass`)
  - `history_write`: `writes_history` の pass(`graph`・`pass`)
  - `composition`: それ以外の理由で preview を組めなかった。拒否が名前付き(`GraphRegistryError` か `<code>: <fields>` の形)なら `cause: {code, ...その fields}` が付きます(例: preview だけが断る graph のループの `graph.loop_expression_invalid@1` は `cause: {code: "graph.loop_expression_invalid@1", node: "steps", reason: "not_static"}`。test)
- **出る所**: `get_status` の `preview`([`get_status` の PV-1 の鍵](../../src/core/communication/rpcserver.cpp#L1657))、`get_frame_plan` の最上位の `preview`([frame plan の PV-1 の鍵](../../src/core/vkcore/renderer.cpp#L4377))、render の編集の committed の結果の `preview`(公開した世代の状態。`reload_render_documents` と `edit_batch` の committed も同じ。[committed に preview を足す所](../../src/core/communication/renderconfigeditor.cpp#L6749))。
- **`render_preview`**: preview が利用できない世代では、capture の中身を読む前に `preview.unavailable@1` で断ります。JSON-RPC の error(`-32000`)の `data` が `{code: "preview.unavailable@1", method: "render_preview", reason: <上の reason>}` です([`checkPreviewAvailable()`](../../src/core/communication/editorpreviewservice.cpp#L134))。
- **編集**: 編集の preflight は preview を組みません([PV-1 で preview を組まなくなった preflight](../../src/core/renderingpass/previewgraph.cpp#L223))。preview だけが組めない編集も committed になり、`preview` が `available: false` に変わります。逆に history を外す編集で `available: true` に戻ります。
- **log**: 起動時に、preview が利用できなければ warning を 1 行出します。形は `render preview unavailable (preview.unsupported_authoring@1 feature=history target=accum); normal play continues, render_preview refuses: <message>` で、`target=` は reason に `target` があるときだけです([PV-1 の warning の行](../../src/core/vkcore/renderer_config.cpp#L584))。同じ関数は reload と編集の組み直しでも通るので、そこでも preview が利用できなければ同じ行が出ます(コードの読み。test は起動時の 1 行だけを見ます)。
- 例(test。直書きの target `accum` が history を持つ fixture `test/fixtures/pv1_history_accum`): 起動して描け、2 frame の画が違う(history が効く)。`get_status` と `get_frame_plan` の `preview` が `{available: false, reason: {code: "preview.unsupported_authoring@1", feature: "history", target: "accum", message: "preview graph '<config>' rejects authored history target 'accum'"}}`。root から history を外した reload の committed が `preview: {available: true}` で、`render_preview` が `rendered`。`edit_render_targets` で history の target `trail` を足しても preview はその fragment ごと外すので `available: true` のまま。`edit_preset_pass` で `present` に `trail@history` を読ませると committed で、`preview.reason` が `{feature: "history_read", graph: "main_render", pass: "present"}`(PV-1 の前はこの編集が断られていました)。
- test: `engine_primitives_test` の [「PV-1 an authored history target plays with the one-shot preview unavailable, and edits move preview both ways without refusing」](../../test/engine_primitives_test.cpp#L3469)(GPU。上の例)、ctest `pv1_preview_unavailable_player`(`test/run_pv1_preview_unavailable_player.cmake`。headless の `pelican_player` で起動・2 frame の画が違う・`get_status` / `get_frame_plan` の `preview`・`render_preview` の `preview.unavailable@1`・WARN の行が 1 つ)、`editorpreview_test` の [「PV-1 render_preview refuses an unavailable preview as preview.unavailable@1 with its reason」](../../test/editorpreview_test.cpp#L415)。`renderconfigeditor_test` の G-6 の「preview が組めない編集は ticket の前に断る」test は、PV-1 で「committed になる」test に書き換わりました。

## 6.14 GPU デバッグラベルと計測(✅WP139/140/143/145)

「絵が違う」「重い」を追うための面は 4 つあり、有効化の重さが違います。設計の正は [../design_debug_profiling.md](../design_debug_profiling.md)(v1.1)です。

| 面 | 有効化 | 出口 |
|---|---|---|
| フレームメトリクスログ / `get_status.memory` / `get_status.xr.timing` | 常設(何もしない) | ログ・rpc・ImGui |
| GPU タイミング | rendering config の features に `engine://features/gpu_timing.json` を 1 行 | ログ・`get_status.gpu_timing`・ImGui |
| Vulkan validation + synchronization validation | `--vulkan-validation=off\|on`。未指定は `_DEBUG` だけ ON | validation メッセージ・`get_status.vulkan_validation` |
| Vulkan のデバッグ名とコマンドラベル | 起動フラグ `--gpu-labels`(既定 OFF) | RenderDoc / NSight / validation メッセージ |
| RenderDoc キャプチャ | RenderDoc の Launch Application から player を起動(注入) | F11 / rpc `capture_gpu` → `.rdc` |

### `--vulkan-validation` — 最適化ビルドで検証する(✅WP297)

検証レイヤはビルド種別へ固定されていません。引数を省略した場合だけ従来の既定、すなわち
`_DEBUG` で ON、それ以外で OFF です。`--vulkan-validation=on` はどの構成でも
`VK_LAYER_KHRONOS_validation` と
synchronization validation を有効にし、`--vulkan-validation=off` はどの構成でも両方を
無効にします。同期検証を別スイッチにしなかったのは、従来の `_DEBUG` 挙動を保ち、最も価値が高く
最も高価な検査を落とした「一部だけの validation on」を作らないためです。

状態は起動引数ではなく、生成に成功した Vulkan instance の状態を
`get_status.vulkan_validation` で確認します:

```json
"vulkan_validation": {
  "layer": "VK_LAYER_KHRONOS_validation",
  "available": true,
  "enabled": true,
  "synchronization": true,
  "reason": "enabled_by_launch_option"
}
```

`reason` は `enabled_by_debug_build_default` / `disabled_by_non_debug_build_default` /
`enabled_by_launch_option` / `disabled_by_launch_option` のいずれかです。明示 ON または
`_DEBUG` 既定で層が導入されていなければ、黙って無効化せず
`pelican.vulkan.validation_layer_unavailable@1` と `VK_LAYER_KHRONOS_validation` を含む
hard error で終了します。

### `--gpu-labels` — ラベルの読み方(✅WP139)

`--gpu-labels` は**ビルドフラグではなく起動フラグ**です(配布ビルドでも使えます。`PELICAN_WITH_RENDERDOC` とは独立で、ラベルだけ欲しいなら RenderDoc は不要)。有効になる条件は 3 つの AND — フラグ / instance extension `VK_EXT_debug_utils` の存在 / 関数ポインタの解決。結果は `get_status.debug_utils` で確認します:

```json
"debug_utils": { "available": true, "enabled": true, "reason": "enabled",
                 "capabilities": { "object_name": true, "command_label": true, "queue_label": true } }
```

`reason` は `disabled_by_launch_option` / `VK_EXT_debug_utils_unavailable` / `required_function_unavailable` / `enabled` のいずれかです。関数が足りないときは例外にせず `enabled: false` に落ちます。

コマンドラベルは**フレームプランの順序どおり**に、次の正規形で入れ子になります:

```
frame/<logical-frame>/graph/<flat|xr>/view/<index>/node/<ordinal>:<kind>:<name>
  ├─ barriers
  └─ body
```

`<kind>` は §6.6 のノード種別(`render` / `compute` / `anchor` / `snapshot_copy` / `output_transform`)に、XR ミラー経路の `mirror` を加えたものです。オブジェクト名は RT が `rt/<名前>/surface/<n>/image`(と `/view`)、ほかに `swapchain/...` / `offscreen/...` / `xr/view/<view>/swapchain/image/<n>/...` / `frame/in_flight/<frame>/view/<view>/ubo`(と `/descriptor_set`)が付きます。

- ⚠ `queue_label: true` は「関数が解決できた」という意味だけで、**submit 境界に queue label は出ません**(呼び出し側が未実装)。バッファ / パイプライン / テクスチャ / サンプラの網羅命名も 📐 です。
- ラベルの ON / OFF で**最終 RGBA8 は byte 一致**します(golden 全ケースで検証済み)。無効時はラベル文字列の組み立て自体を行いません。
- validation layer は `--gpu-labels` では有効になりません。`--vulkan-validation=on` または `_DEBUG` 既定で別に選択します。ただし両方を有効にすると validation メッセージにも同じ論理名が出ます。

### RenderDoc キャプチャ(✅WP140)

> **設計決定(受動接続):** Pelican は RenderDoc を**ロードしません**。既に注入済みの `renderdoc.dll` を観測するだけです。したがって **RenderDoc の Launch Application から player を起動する**のが唯一の有効化手段で、起動後の attach は使いません。接続に成功すると RenderDoc 側のキャプチャホットキーは無効化され、F11 の所有権は Pelican に一本化されます。

トリガは 2 系統で、**キャプチャ専用の CLI 引数はありません**。

- **F11**(ウィンドウ + flat モード): armed 状態の次の 1 フレームだけが `StartFrameCapture` / `EndFrameCapture` で囲まれます。キャプチャに失敗しても論理フレームは必ず 1 回描かれます(二重描画はしません)。⚠ リプレイ中はライブのウィンドウイベントを積まないので、**キーボードの F11 は効きません**(収録シーケンスに F11 が入っている場合だけ発火します)。
- **rpc `capture_gpu`**(headless 想定): `render_frame` と同型で、**時刻もフレーム index も進めません**。決定的リプレイで同じ論理フレームに固定してから撮るのが定石です([第10章](10_tools.md) / [../adding_features.md](../adding_features.md) Recipe 8)。

XR session が active な間は `capture_xr_unsupported` で拒否されます(v1 の境界)。状態と失敗理由は `get_status` の `renderdoc`(文字列)と `diagnostics.renderdoc`(`{status, state, reason, api_version, source}`)で読めます。RenderDoc 未注入なら `absent` / `renderdoc_not_injected`、`PELICAN_WITH_RENDERDOC=OFF` ビルドなら `disabled` / `renderdoc_build_disabled` です。

### GPU タイミング(✅WP143)

有効化は rendering config の 1 行だけです。**同梱プロジェクトはどれも参照していない**ので、自分の config に足してください(feature が無ければ query pool 自体を作りません)。

```json
{
  "features": ["engine://features/gpu_timing.json"],
  "render_targets": [],
  "rendering_passes": [ { "name": "main", "passes": [ /* ... */ ] } ]
}
```

(結合テストのフィクスチャ `test/run_gpu_timing_headless.cmake` からの抜粋)

`get_status.gpu_timing`(`schema_version: 2`)の読み方:

| フィールド | 内容 |
|---|---|
| `enabled` / `supported` / `reason` | feature の有無 / キューが timestamp を持つか / `enabled`・`graphics_queue_timestamps_unsupported`・`feature_not_enabled` |
| `history_capacity` / `history_count` | 論理フレーム単位の履歴リング(容量 120)|
| `dropped_samples` | best-effort な提出が失敗して捨てた sample 数 |
| `logical_frame_history[]` | 履歴内の論理フレームごとの `{logical_frame, graph_variant, total_ms, supported_sample_count}` |
| `logical_frame_averages[]` | graph variant ごとの `{graph_variant, frame_count, average_total_ms, min_total_ms, max_total_ms}`。XR multiview profile の測定元 |
| `generation` / `samples` | ✅PO-1。平均の窓が始まった runtime の世代(最初の frame が世代を言うまでは `null`)と、窓が持つ論理フレームの数(`history_count` と同じ。`logical_frame_averages` はこの frame の平均)。feature 無効時は `null` / `0` |
| `logical_frame_total_sum_views_ms` | 最新フレームの view 行の**単純合計**(左右眼を平均しません)|
| `views[]` | `{logical_frame, graph_variant, view_index, label, barriers_ms, body_ms, total_ms}`。`label` は flat/0 → `flat`、xr/0 → `left`、xr/1 → `right`、xr/2 → `mirror` |
| `nodes[]` | `{..., node_ordinal, node_kind, node_name, subrange, identity, supported, reason, ms}`。✅ST-1 から**最後の 1 論理フレームの sample だけ**(下の「`nodes` は最後の 1 frame」) |
| `node_averages[]` | ✅ST-1。`get_gpu_timing` の `nodes` と同じ行・同じ値 —— node ごとの、窓の最後の 30 論理フレーム(窓がそれより短ければその数)の平均 `{logical_frame, graph_variant, view_index, node_ordinal, node_kind, node_name, barriers_ms, body_ms, body_supported, sample_count}`。feature 無効時は `[]` |
| `query_pool` | `{frame_slots, range_slots, max_nodes, query_capacity, create_count, pending_ranges}` |

`nodes[].identity` は**コマンドラベルと同じ文字列に `/barriers` または `/body` を足したもの**なので、RenderDoc の Event Browser の木と 1:1 で突き合わせられます。`barriers` は「合成されて入ってきたバリア」、`body` は「そのノード自身の遷移と描画/ディスパッチ」です(帰属契約の正本は `test/fixtures/gpu_timing_attribution.json`)。仕事を持たないアンカーは `supported: false` / `reason: "no_gpu_work"` / `ms: 0.0` になります。

feature 無効時も**同じ封筒**が返ります(`{"schema_version": 2, "enabled": false, "supported": false, "reason": "feature_not_enabled", ...}`)。

**窓は runtime の世代ごと(✅PO-1)。**編集などで新しい runtime の世代が公開されると、その世代の最初の論理フレームを記録する前に窓(`logical_frame_history` とそこから作る平均)を
空にし、新しい世代から数え直します。前の世代で記録して後から回収した range は捨てます([`RenderTiming::beginRuntimeGeneration()`](../../src/core/vkcore/rendertiming.cpp#L516))。
PO-1 の前は、編集の前後の frame が 1 つの平均に混ざっていました。`get_gpu_timing` の node の平均も同じ履歴から作るので、同じく世代ごとになります
(コードの読み。test は `get_status` だけを見ます)。

**`nodes` は最後の 1 frame(✅ST-1、`1efecd29`)。**ST-1 の前の `nodes` は、履歴(最大 120 論理フレーム)の全部の sample を `get_status` のたびに並べていました。ループを展開した graph(GB-3 の FFT の海)では約 2 MB になり、engine の main thread が `get_status` 1 回ごとに約 0.8 s 止まって、Studio の 5 秒の RPC の制限を越えていました(仕様書の調査の記述)。今は最後の 1 論理フレームの sample だけを並べ([並べる所](../../src/core/vkcore/rendertiming.cpp#L308))、窓の node ごとの平均は `node_averages` で読みます。`node_averages` と `get_gpu_timing` の `nodes` は同じ関数 [`averageGpuTimingHistoryNodes()`](../../src/core/vkcore/rendertiming.cpp#L138) と同じ行の組み立てから作るので、値も同じです。`get_gpu_timing` は変わっていません。`logical_frame_history` など、ほかの鍵もそのままです。commit の記述では、q14e の test の project で `get_status` の JSON が 867 KB から 28 KB になりました。全履歴の sample を読んでいた engine 側の読み手(golden の GPU draw timing の測り方)は、`RenderTiming` の履歴を直接読むように替わりました(`test/golden_harness.cpp`)。test: `rendertiming_test` の [「ST-1 GPU timing status carries the last frame's samples and the window's node averages」](../../test/rendertiming_test.cpp#L325) と、`engine_primitives_test` の [「ST-1 get_status carries the last frame's GPU samples and get_gpu_timing's node averages」](../../test/engine_primitives_test.cpp#L1495)(GPU)。

ログは 1 秒ごとに 1 行です(GPU 側のキーは `graph/<variant>/view/<i>/node/<ordinal>:<kind>:<name>/<subrange>`):

```
pelican frame metrics frames=<n> cpu_update_ms_avg=<x> cpu_render_ms_avg=<x> cpu_present_wait_ms_avg=<x> gpu_ms_avg=<key=ms,...>
```

GUI では ImGui の `Pelican Engine Stats` → `GPU timing`(View / Node / Kind / Barriers ms / Body ms)。表示は**最新フレームのみ**で、120 フレームの積み上げグラフは 🚧 です。

#### GPU-written draw の損益分岐を測る

`GpuDrawTimingObservation`(`src/project/gpudrawtiming.hpp`)は、GPU culling と
CPU DrawQueue を比較するための offline 計測形式です。`pelican.gpu_draw_timing` version 1
には device identity、graph variant、candidate/visible/segment/view/output-capacity record 数と、
両経路の次の値を保存します。

| 値 | 用途 |
|---|---|
| `frame_gpu_ms` | v1 の損益分岐 metric。graph 全体の GPU timestamp |
| `culling_gpu_ms` | GPU path の count reset + cull body。query identity の証拠 |
| `material_draw_gpu_ms` | `gbuffer_pass/body` の帰属確認 |
| `host_frame_ms` | `Renderer::render()` の wall-clock 診断値。fence/pacing を含み得るので選択には未使用 |
| `sample_count` | minimum sample gate |

`evaluateGpuDrawBreakEven()`は、sample不足なら`inconclusive`、それ以外は
`frame_gpu_ms`の改善が指定したminimum gain以上のときだけ`gpu_culling`を返します。
これは実行中に呼んで次frameを切り替える仕組みではありません。測定結果をレビューし、
必要なら将来のdevice/workload profileへ固定値として移すための判断入力です。

実Vulkan受け入れは10 / 130 / 1024 candidate recordsを各24 frame測定し、絶対msや
CPU/GPUどちらが勝つかではなく、query identity、sample数、query全回収、最終RGBA8一致を
検証します。最新レポートは
`build-off-openxr/test_artifacts/wp210_gpu_draw_break_even.json`へ1ファイルだけ上書きされます。
ローカル参考値と測定条件は
[`2026-07-28_wp210g_gpu_draw_timing.md`](../design_reviews/2026-07-28_wp210g_gpu_draw_timing.md)
を参照してください。

### VRAM(✅WP145)

`get_status.memory`(`schema_version: 1`)は常設で、feature も CLI も要りません。

```
{ "schema_version": 1, "driver_available": <bool>, "driver_reason": <string|null>,
  "heaps": [ {heap_index, device_local, size, usage, budget, source} ],
  "engine_categories": [ {category, allocated_bytes, logical_used_bytes, free_bytes,
                          high_water_bytes, object_count, source, reason} ] }
```

- **ドライバ heap とエンジン論理値は別物で、足しません。** `heaps[]` が埋まるのは `VK_EXT_memory_budget` が有効なときだけで、非対応時は **0 byte の偽 heap を作らず**空配列 + `driver_reason` を返します。
- `engine_categories[]` は 6 カテゴリ(`model_indices` / `model_vertices` / `model_skinned_vertices` / `model_morph_deltas` / `textures` / `materials`)。
- ⚠ **`null` は「0 バイト」ではありません。** 読み取り専用サーフェスから公開できない値は `null` + `reason` の named absence です(現状 `allocated_bytes` / `free_bytes` / `high_water_bytes` は `null`)。

GUI では ImGui の `Pelican Engine Stats` → `Memory`(heap 別の Size / Usage / Budget MiB と engine category)。

### 決定性との関係・未実装

- live の計測値は**診断専用**で、その場の simulation や render policy へ自動 feedback しません。XR の `auto` が読むのは人が rendering config に固定した device profile だけです。GPU draw の break-even evaluator もoffline専用で、観測値をruntimeへ自動適用しません。rpc の 2 回一致比較では `gpu_timing` と `memory` を丸ごと `<measured>` に正規化して除外します。一方 `frame` / `time` / `seed` などの simulation state と最終 RGBA8 は正規化しません。`.rdc` の byte 一致は決定性ゲートにしません。
- 📐 未実装: buffer / pipeline / texture / sampler の網羅命名と submit 境界の queue label(D-P0b)、ImGui からのキャプチャボタン、XR 中のキャプチャ、Tracy、validation 常設 CI、crash 診断(minidump / `VK_EXT_device_fault`)。
- 🚧 CPU フェーズ計測は `update` / `render` / `present_wait` の 3 区間が上のログに出るだけです(`get_status.cpu_timing` はありません)。

## 6.15 レイトレーシングの宣言層 — ray layout v3 と `pelican_ray_instance`(✅RT3-1 = WP374/374a/377/379/383)

§6.6 の `ray_tracing` compute task は「どのシェーダで trace するか」でした。この節はその手前、**何を TLAS に入れ、どんな Vulkan の operand で入れるか**をプロジェクト側から宣言する層です。`rt_shadow_mask` 系 feature(§6.5)は宣言層を使わずに動くので、この節は「レイトレの見た目をプロジェクトから制御したい」場合だけ必要です。

### 入口は rendering config のルートに 2 キー

```json
{
  "ray_group_layout": "ray_layout.json",
  "ray_bindings": "ray_bindings.json"
}
```

- `ray_bindings` だけを書くと `pelican.ray_tracing.bindings_without_layout@1`。
- `ray_group_layout` があるプロジェクトは**ランタイムシェーダコンパイラ必須**で、`PELICAN_RUNTIME_SHADER_COMPILER=OFF` のビルドでは `pelican.ray_tracing.layout_requires_runtime_compiler@1` になります。
- layout 文書は `pelican.ray_group_layout` の **version 3 だけ**を、bindings 文書は `pelican.ray_bindings` の **version 2 だけ**を受理します(✅MG-2a)。ほかの整数版(古い 1 / 2 を含む)は `pelican.ray_group_layout.version_unsupported@1` / `pelican.ray_bindings.version_unsupported@1` で拒否され、メッセージに文書の参照が付きます([raygrouplayout.cpp](../../src/core/shader/raygrouplayout.cpp#L843))。旧版を旧い意味で読む経路は無く、自動移行もしないので、手で書き直します。layout version 1 / 2 は各 `groups[]` に `"type": "triangles"` を足して `version` を 3 に、bindings version 1 は `version` を 2 にします。⚠ version 1 の layout では行の `instance_custom_index` が q(行の正規順の添字)でしたが、version 3 では `pelican_ray_instance` の `custom_index` で宣言しない限り 0 です(shader に出る値が変わります)。
- group の型(`triangles` / `procedural`)は layout の `groups[].type` だけが持ちます。compute task 側の `hit_shaders` の group に `type` を書くと、値を問わず `pelican.ray_group_type_declared_in_task@1` です(MG-2a より前は `"triangles"` だけ受理していました)。

layout の骨格:

```json
{
  "schema": "pelican.ray_group_layout", "version": 3,
  "ray_types": ["primary", "shadow"],
  "default_group": "opaque",
  "groups": [{"name": "opaque", "type": "triangles"}],
  "misses": [{"name": "sky"}, {"name": "shadow_open"}],
  "record": [],
  "acceleration_structure": {
    "defaults": {"mask": 127, "instance_flags": ["triangle_facing_cull_disable"], "geometry_flags": []},
    "rules": [
      {"material": {"document": "materials/multi.json", "material_name": "Oak"},
       "geometry_flags": ["opaque"]}
    ]
  }
}
```

`acceleration_structure` は SBT の入力ではないので、layout の digest はこのキーを**取り除いてから**計算されます(AS 宣言だけを変えてもシェーダ record ABI の digest は動きません)。AS 宣言側には別の digest が付きます。

### semantic mode はプロセス一生ものの 2 値

起動時、コンポーネント登録より前に rendering config を先読みして `RayLayoutRuntimeState` を確定します([pelican_core.cpp](../../src/core/userpublic/pelican_core.cpp#L40))。

| layout | semantic mode | 効果 |
|---|---|---|
| 無し | `LegacyNoLayout` | 宣言層は無効。`acceleration_structure` も `pelican_ray_instance` も書けない |
| 有り(version 3) | `LayoutV3` | `acceleration_structure` と `pelican_ray_instance` が使える |

MG-2a より前は version 1(`LayoutV1Q`: 行の `instance_custom_index` を常に q で上書き)と version 2(`LayoutV2`)を別の mode として持っていましたが、どちらも消えました。同一プロセス内で mode が変わる差し替え(グラフのリロード等)は `pelican.ray_layout_mode_change_requires_restart@1` で拒否されます。組み込みの `pelican_ray_instance` codec は **layout があるときだけ codec 表に載ります**([componentcodec.cpp](../../src/core/loader/componentcodec.cpp#L2224))。codec 表は既定で 8 件、layout があるプロジェクトだけ 9 件目としてこれが載る、という形です([第4章](04_scene_ecs.md) §4.2 に「条件付き」として別項があります)。

### 状態表

layout(無し / 有り)× `acceleration_structure` 宣言 × `pelican_ray_instance` の組み合わせは 6 通りです(layout が無ければ AS 宣言の置き場所自体が無いので、その列は 2 通り)。layout の version が 3 でなければ、この表に入る前に layout の読み込みが `pelican.ray_group_layout.version_unsupported@1` で止まります。

| # | layout | AS 宣言 | `pelican_ray_instance` | 結果 |
|---|---|---|---|---|
| 1 | 無し | — | 無 | 成功(宣言層は無効) |
| 2 | 無し | — | 有 | `pelican.ray_instance_requires_layout_v2@1` |
| 3 | 有り | 無 | 無 | 成功(canonical 既定。`instance_custom_index` は q ではなく 0) |
| 4 | 有り | 無 | 有 | 成功 |
| 5 | 有り | 有 | 無 | 成功 |
| 6 | 有り | 有 | 有 | 成功 |

`pelican.ray_instance_requires_layout_v2@1` と、procedural geometry を layout 無しで書いたときの `pelican.ray_group_type_requires_layout_v3@1` は、公開したときの名前を据え置いています。名前に版が入っていますが、今の意味はどちらも「ray group layout が無い」です。

2 は scene load 時の検査です([scene.cpp](../../src/core/loader/scene.cpp#L64))。**ゲーム DLL が `pelican_ray_instance` という名前のコンポーネントを自分で登録している場合は素通しします** — 名前の所有者が外部なら、組み込みの規則を当てないためです。

### `pelican_ray_instance` コンポーネント

scene JSON の `components` に書きます。

```json
{"name": "pelican_ray_instance", "mask": 9,
 "instance_flags": ["triangle_facing_cull_disable"], "custom_index": 7}
```

| フィールド | 型・範囲 | 省略時 |
|---|---|---|
| `mask` | 0〜255 | AS 宣言の `defaults.mask`(AS 宣言が無ければ 255) |
| `instance_flags` | 文字列配列。`triangle_facing_cull_disable` / `triangle_flip_facing` / `force_opaque` / `force_no_opaque` | `defaults.instance_flags`(無ければ空) |
| `custom_index` | 0〜`0x00ffffff`(**24 bit** — この値がそのまま Vulkan の TLAS インスタンス記録 `VkAccelerationStructureInstanceKHR::instanceCustomIndex` へ入り、その field 幅が 24 bit だからです。エンジンが決めた上限ではありません) | 常に 0(AS 宣言の `defaults` に `custom_index` は書けません — allowlist は `mask` / `instance_flags` / `geometry_flags` の 3 キーだけ) |

- **3 つとも省略した空のコンポーネントは `pelican.ray_instance_empty@1` で拒否**されます。一方 `"instance_flags": []` は「空という明示的な上書き」として有効です(欠落と空配列は別)。
- `force_opaque` と `force_no_opaque` の同時指定は `pelican.ray_as_conflicting_instance_flags@1`、未知のフラグ名は `pelican.ray_as_unknown_flag@1`、未知のフィールドは `pelican.ray_instance_unknown_field@1`、範囲外は `pelican.ray_instance_field_range@1` です([componentcodec.cpp](../../src/core/loader/componentcodec.cpp#L1743))。
- **`overrides`はWP382dで受理します**。`List`の各行で対象occurrenceのmask/flags/custom indexを上書きし、省略値はobject/layoutから継承します。行削除は継承へ戻ります([第3章](03_project_format.md) §3.10)。
- `simplemodelview` の無いオブジェクトに付けると `pelican.ray_instance_not_ray_traced@1`。BLAS 適格な静的ジオメトリが 1 つも無い場合も同じコードです(スキン / morph / VAT のプリミティブは静的 BLAS に入りません — [ソースコード読解ガイド第11章](../source-code-guide/11_ray_query_acceleration_structures.md))。
- 上書きは**フィールド単位**です。書かなかったフィールドは AS 宣言の `defaults`(それも無ければ canonical 既定 mask=255 / flags なし / custom_index=0)になります([rayinstance.hpp](../../src/core/userpublic/components/rayinstance.hpp#L23))。

### AS 規則の material 鍵(✅WP383)

`acceleration_structure.rules[]` は **material identity** で行を選び、その行の `geometry_flags` を差し替えます。identity は 3 種の直和で、**gate(規則を要求するか)と lookup(どの規則に当たるか)が同じ値を見ます**([raygrouplayout.cpp](../../src/core/shader/raygrouplayout.cpp#L61)):

| identity | 成立条件 | selector の書き方 |
|---|---|---|
| `Document{document, material_name?}` | 行に material 文書の参照がある(**document の存在が勝つ**。名前は任意) | `{"document": "materials/x.json"}`(+ 任意で `"material_name": "Oak"`) |
| `Asset{asset, source_material_index}` | document が無く、asset 参照と `source_material_index` の両方がある | `{"asset": "assets/models/x.glb", "source_material_index": 0}` |
| `Unavailable` | どちらも成立しない | 選べない |

- selector の形はこの 2 通りだけで、混在(`document` + `source_material_index` など)は `pelican.ray_as_rule_malformed@1`。canonical 鍵が同じ規則が 2 つあれば `pelican.ray_as_duplicate_rule@1`、1 行が 2 規則に当たれば `pelican.ray_as_rule_ambiguous@1` です。
- `document` selector は `material_name` を書かなければ名前を問わず一致し、書けば一致を要求します。
- `source_material_index` は optional のまま運ばれ、**欠損を 0 に潰しません**。内部の「無し」番兵が identity へ届くと `pelican.ray_material_index_out_of_range@1`。
- 規則が 1 つでもある layout で identity が `Unavailable` の行があると `pelican.ray_as_row_material_identity_unavailable@1` です。規則が空(または AS 宣言が無い)なら identity は要求されません。
- モデル側で identity がどこから決まるかは [第5章](05_assets.md) を参照してください。

`geometry_flags` に書けるのは `opaque` と `no_duplicate_any_hit_invocation` の 2 つで、`instance_flags` とは語彙が別です。

### BLAS variant と `resident_blas_keys` 診断

geometry flags は行ごとに変わるので、BLAS は **`(geometry_allocation_id, geometry_flags)`** の組を鍵にキャッシュされます([accelerationstructure.cpp](../../src/core/renderer/accelerationstructure.cpp#L916))。同じジオメトリでも flags が違えば別 BLAS です。

プランダンプ(§6.6)の `ray_query_acceleration_structures` に出ます([accelerationstructure.cpp](../../src/core/renderer/accelerationstructure.cpp#L830)):

- `blas_variant_count` — 常駐している variant の数
- `resident_blas_keys` — variant ごとの `{geometry_allocation_id, geometry_flags, index_device_address, vertex_device_address}`。同じ配列が `active_blas_inputs` というキーにも出ます(同一内容の 2 キー)
- `blas_build_count` / `tlas_build_count` / `active_blas_count` / `tlas_instance_count` / `excluded.*`(スキン・morph・VAT・BLAS 不適格の内訳と名前)

「1 フレームに何回 build したか」ではなく、**どの鍵が常駐しているか**を見てください。build 回数は無効化のタイミングに左右されますが、常駐鍵は宣言の結果そのものです。

### 移植プロファイルとの相互作用

layout に `"portable_profile": "material_alpha_v1"` がある場合、`force_opaque` を持つことになった行は `pelican.ray_profile_opaque_as@1` で拒否されます(alpha を any-hit 側で評価するプロファイルと矛盾するため)。AS 宣言の `defaults` 由来でも `pelican_ray_instance` 由来でも同じです([raygrouplayout.cpp](../../src/core/shader/raygrouplayout.cpp#L1519))。

## 6.16 rendering config の root —— `pelican.graph` root(✅P-2a-4a/4b、MG-2b)

rendering config のファイルは **`pelican.graph` root** です。最上位には共通グラフ自身のキーだけが並び、§6.2 の表のキーは全部 `rendering` の下に入ります。出荷プロジェクト 7 本(`animgraph_demo` / `example` / `pathtrace_demo` / `posegraph_demo` / `raytracing_demo` / `sprite_demo` / `vrm_xr_demo`)も、`pelican_cli project init` が作る新規プロジェクトもこの形です。

| | `pelican.graph` root |
|---|---|
| 最上位のキー | `schema` / `version` / `definitions` / `nodes` / `outputs`(と N-6 からは editor 用の `layout`) |
| §6.2 の表のキー | **`rendering` の下**(`rendering.features`、`rendering.render_targets`、…)。ただし `buffers` / `compute_tasks` は書けない(下) |
| 共通グラフ(ノード) | 最上位の `definitions` / `nodes` / `outputs` に書く(§6.17) |

```json
{
  "schema": "pelican.graph",
  "version": 1,
  "definitions": {},
  "nodes": [],
  "outputs": {},
  "rendering": {
    "features": ["engine://features/bloom.json", "engine://features/ui.json"],
    "render_targets": [ ... ],
    "rendering_passes": [ ... ]
  }
}
```

**規則は 1 つだけです**: `schema` / `version` / `definitions` / `nodes` / `outputs`(と `layout`)は共通グラフが所有し、**それ以外の rendering config のキーは全部 `rendering` 節に属します**。エンジンは composer の入口でこの文書を平らな rendering config へ lower してからパスを組みます([lowerRenderGraphDocument()](../../src/project/rendergraphcomposition.cpp#L1354))。`rendering` 節は、旧い legacy root(下)を最上位に置いたときと同じ意味で読まれます。

- **旧い legacy root は読めません(MG-2b)。**§6.2 の表のキーを最上位に直接並べた形は、起動時の読み込み([読み込みで legacy root を断る所](../../src/core/renderingpass/renderingpassconfigloader.cpp#L24))、`--feature-overlay` の重ね合わせ、レイトレの設定の読み出し、Studio / RPC の編集文書のどれもが `render_config.legacy_root@1` で断ります([`RenderConfigLegacyRoot`](../../src/project/renderconfigdocument.hpp#L37))。v0 は形式ごとに版を 1 つだけ持ち、旧い形は変換せず書き直す方針です。直すには、旧いファイル全体を `rendering` の値にして上の 5 キーを足します(`{"schema": "pelican.graph", "version": 1, "definitions": {}, "nodes": [], "outputs": {}, "rendering": <旧い root>}`)。MG-2b は出荷の legacy root 5 本(`animgraph_demo` / `sprite_demo` / `vrm_xr_demo` / `raytracing_demo` / `pathtrace_demo`)をこの手で包みました。
- `buffers` と `compute_tasks` は **グラフが所有します**。lowering は `render.buffer` / `render.compute` などのノードの接続からこの 2 つを作るので、`rendering` 節に書くと `render graph derives field from connections: buffers` のように名前付きで拒否します。buffer と compute task は graph node か task feature で書きます。
- Studio と RPC の authoring(`get_render_features` / `edit_render_features` / `materialize` / `--feature-overlay`)は、同じ 1 つの locator([`renderingSectionOf()`](../../src/project/renderconfigsection.hpp#L36))を通って `rendering` 節を読み書きします。locator は `pelican.graph` root でない文書を `render_config.legacy_root@1` で断り(MG-2b より前はその文書自身を節として扱っていました)、graph root なのに `rendering` が無い(あるいは object でない)文書への編集は、黙って root にキーを作らず `render_config_section_unlocated` で拒否します。
- **ただし「1 箇所」なのは読み書きの locator までです。**byte-lossless の書き込み経路は同じ規則を鍵の側から判定していて、上の 6 キーかどうかを [`isGraphRootKey()`](../../src/project/renderconfigdocument.cpp#L443) で見てから root と節を振り分けます([withSectionValue()](../../src/project/renderconfigdocument.cpp#L691))。さらにレイトレの読み出しは**同じ規則をその場で書き直しており**、`rendering` が無いときは例外ではなく**空 object** として続行します([raygrouplayout.cpp:1465](../../src/core/shader/raygrouplayout.cpp#L1465))—— つまり「節が無い」ときの振る舞いは経路によって違います。
- `test/fixtures/graph_root_example/` は、出荷 `example` の節と、preset を選ぶ `animgraph_demo` の節を `rendering` の下に移した実物です(P-2a-4a)。
- web ビューアは `pelican.graph` root をまだ読めません([第9章](09_web.md) §9.3)。MG-2b の記録は、web ビューア(WEB-1)と外部の USD import の道具が追従する必要があるとしています。

`nodes` に何を書けるか(`value.*` / `render.*` / `pose.*` / `graph.instance` のノード語彙、`definitions` の使い方、owner の宣言)は **次の §6.17** です。GUI から装着を足す・外す手順は [第13章 エディタ](13_editor.md) の「グラフの装着を編集する」、語の索引は [用語集](14_glossary.md)、実物の手順は `projects/posegraph_demo/README.md` にあります。

## 6.17 共通グラフのノード語彙(✅AG-0/AG-1a/AG-1b/P-1/AG-2/AG-3a)

§6.16 の `pelican.graph` root が持つ `definitions` / `nodes` / `outputs` の中身が **共通グラフ**です。「共通」というのは **値(スカラー)・描画・アニメーションが 1 つのノード語彙を共有する**という意味で、曲線を 1 本引いて、その値を GPU バッファにも姿勢のブレンド率にも送れます。`projects/posegraph_demo/passes/main.json` が最短の実物です(曲線 → 姿勢ブレンド)。

> MG-2b から rendering config は `pelican.graph` root だけなので(§6.16)、この節の話はどのプロジェクトにも当てはまります。

### module — 語彙の単位

op は **module** が登録します。登録済み module はエンジンに最大 5 本で、どれも module version 1 です([graphmodule.cpp:286](../../src/project/graphmodule.cpp#L773))。

| module | 内容 | op |
|---|---|---|
| `pelican.value` | スカラー、曲線、時計、再生イベント | 11 |
| `pelican.audio` | 音声ボイスと、それを駆動する値 sink | 3 |
| `pelican.render` | GPU バッファ・属性・画像・パス(LP 系で既存 target の参照・material pass・影と snapshot の pass・draw step が加わった)と、スカラー公開 sink | 14 |
| `pelican.pose` | 骨格姿勢(クリップ標本化・ブレンド・ステートマシン)と単一 writer の sink | 6 |
| `pelican.ui` | UI の node の値と表示を書く sink(`ui.value` / `ui.visible`。parameter `node` = UI node の path) | 2 |

合計 **36 op**(5 module。LP-0a / LP-0e / LP-3a で `pelican.render` に 7 本が加わった)。`PELICAN_WITH_ANIMATION=OFF` の build では **`pelican.pose` module がそもそも登録されない**ので 4 module / 30 op、`PELICAN_WITH_AUDIO=OFF` では `pelican.audio` が無く 4 module / 33 op、両方 OFF で 3 module / 27 op になります。構造の node の `graph.loop`(§6.18)は `graph.instance` と同じく catalog に載らないので、この数に入りません(正本は [docs/v0_freeze.md](../v0_freeze.md) §1.3)([graphmodule.cpp:340](../../src/project/graphmodule.cpp#L913))。module の登録の仕方はエンジンが持ってくる場合も同じで、**「エンジンの特権」ではなく「登録契約」**です(feature が機構語彙に対してそうであるように。§6.5)。

- 登録が無い `op` を書いた文書は **`graph.operation_unregistered@1` `{node, op}`**。OFF build で `pose.*` を書いたときもこれです。
- op は登録されているのに、それを実行する backend がいない場合は **`graph.operation_unexecutable@1` `{backend, op, node}`**。

### op の契約 — `domain` / `evaluation` / `change`

どの op も、ポートの型のほかに 3 つの分類を宣言します(それぞれ別の enum です: [domain](../../src/project/graphcomposition.hpp#L263) / [evaluation](../../src/project/graphcomposition.hpp#L267) / [change](../../src/project/graphcomposition.hpp#L271))。

| 分類 | 値 | 意味 |
|---|---|---|
| `domain` | `values` / `rendering` / `animation` | **どの実行段が評価するか**。`animation` は同じフレームの中で `values` より **1 段早く**走ります。だから **姿勢の op は「今フレームのスカラー」を読めて、逆向きは無い**(一方向)([graphcomposition.hpp:102](../../src/project/graphcomposition.hpp#L258)) |
| `evaluation` | `direct` / `replay` / `baked` / `recorded` | **評価をまたいで結果がどう作られるか**。`direct` は毎回同じ入力から同じ出力。`replay` は状態を持ち、「ここまでの経過」が要る(時計、ステートマシン、root motion の抽出) |
| `change` | `value` / `recompile` / `rebuild` | **ノードの `parameters` を変えたときの代償**。`value` は値が変わるだけ、`rebuild` はノードを作り直す |

`evaluation` はノード単位で **強い方へ上げられます**(引き下げは採られません)。実物は `pose.clip_sample` で、`root_motion: "extract"` を書いたノードだけが `direct` → `replay` に上がります([posegraphmodule.cpp:179](../../src/project/posegraphmodule.cpp#L179))。同梱 op が宣言するのは `direct` と `replay` と、✅AB-1 の `value.sample_table` の `baked`(表は graph を組む時に 1 回読む)の 3 つで、`recorded` を宣言する op はありません。

`ports` は `declared`(op が固定のポート一覧を持つ)か `parameter_dependent`(ノード自身の書きぶりから決まる)です。catalog が `parameter_dependent` と書く条件は「factory を持ち、**かつ宣言コントラクトの `inputs` / `outputs` / `forwarded_outputs`(✅RS-4)がすべて空**」で([parameter_dependent の判定](../../src/project/graphmodule.cpp#L600))、これに当たるのは **`render.*` の 8 本だけ**です(`render.buffer` / `attribute` / `image` / `compute` / `fullscreen` / `raster` と、LP 系の `render.target_ref` / `render.material_pass`)。LP 系のうち `render.shadow_depth` / `render.snapshot_copy` と `draw.*` の 3 本は socket が固定なので `declared` です。✅RS-4 から、`render.shadow_depth` / `render.snapshot_copy` の `outputs` には書いた socket と同名の出力 port が `{"port": "depth", "type_from_input": "depth"}` の形で載ります(`type` の鍵は無く、型はその名前の入力 socket に入る edge の型。[載せる所](../../src/project/graphmodule.cpp#L613))。`render.material_pass` も出力 port を持ちますが、port が node の parameters から決まるので catalog は `parameter_dependent` のままです(§6.18)。`pose.state_machine` は包む文書に従ってポートが増えますが、`pose` 出力を宣言しているので catalog の `ports` は **`declared`** です —— 増えた入力ポートは解決後にしか見えません。Studio のキャンバスもこのフィールドで分岐します([graphcanvasmodel.cpp:434](../../src/devstudio/model/graphcanvasmodel.cpp#L2001))。

### op 一覧

ポートと型まで含んだ**正本は 2 つ**あります —— 走っているエンジンから引く `pelican_cli graph-catalog`(下記)と、それと機械照合されている [docs/v0_freeze.md](../v0_freeze.md) §1.3 の表です。ここでは「何をする op か」だけを並べます。

| module | op | 何をするか |
|---|---|---|
| `pelican.value` | `value.input` | エンジンが供給する名前付きスカラー(`time` / `delta_time` / `scene_time`)を引き込む。書ける名前は catalog の `input_names`(✅CT-1。下の「catalog の読み方」)で、それ以外の名前は今までどおり `unbound frame graph input: <名前>` で断られる |
| | `value.constant` / `value.parameter` | 固定値 / 外から差し替えられる値(instance の入力は展開時にこれになります) |
| | `value.add` / `value.multiply` | 2 入力の算術 |
| | `value.curve` | `time` を受けて `keys` のキーフレーム曲線を評価する |
| | `value.sample_table` | 焼いた表(✅AB-1)の 1 行を `time` で補間して出す。`time` の edge を省くと `scene_time`。`evaluation` は `baked`、`asset` / `row` の変更は rebuild(§6.18「焼いた音の解析の表を読む」) |
| | `value.playback` | `elapsed`(必須)を受けて再生位置を出す。`source_begin` / `source_end` / `duration` / `speed` / `delay` / `sample_interval` は**固定値なら `parameters`、駆動したいなら入力ポート**のどちらでも書けます(catalog では任意ポートとして並びます)。`wrap` は `parameters` 専用で `once`(既定)/ `repeat` / `ping_pong`([valuegraph.cpp:259](../../src/core/userpublic/valuegraph.cpp#L456)) |
| | `value.trigger_clock` / `value.event_clock` | 状態を持つ時計(`replay`)。トリガ / イベント列で進む |
| | `event.playback` | 再生制御イベント(`pelican.event.playback_control@1`)の発行口 |
| | `value.morph_weight` | owner 付き instance の中でだけ使える読み口。`parameters.node` / `parameters.target`(どちらも glTF の index)が指すモーフターゲットの現在 weight を値として引き出す([valuegraph.cpp:180](../../src/core/userpublic/valuegraph.cpp#L375)) |
| `pelican.audio` | `audio.playback` | ボイスを 1 本鳴らす(`replay`)。入力は `time`(必須)と、任意の `events` / `position` / `source_begin` / `source_end` / `speed` / `delay`。parameter は `asset` / `bus`(`se` / `bgm`)/ `wrap`(`once` / `repeat` / `ping_pong`)と、写像の 4 つの固定値。写像を parameter で持つ一体版と、`position` に従う分解版があり、`asset` / `bus` / `wrap` の変更は rebuild([第8章](08_gameplay.md) §8.11) |
| | `audio.volume` / `audio.pitch` | **sink**。値をボイスへ渡す |
| `pelican.render` | `render.buffer` / `render.attribute` / `render.image` | フレームグラフの資源をグラフ側から宣言する |
| | `render.compute` / `render.fullscreen` / `render.raster` | パス種別をグラフ側から宣言する |
| | `render.target_ref` | rendering 節・preset・feature が宣言した target をグラフから指す(作らない。✅LP-0a、§6.18) |
| | `render.material_pass` / `render.shadow_depth` / `render.snapshot_copy` | 同名の type の pass をグラフで書く(✅LP-3a / LP-0e、§6.18) |
| | `draw.material` / `draw.variant` / `draw.override` | draw_loop の body で、1 つの material の描き方を 1 手ずつ並べる(✅LP-3a、§6.18) |
| | `render.scalar` | **sink**。`parameters.target` の名前のフレームグラフバッファへ、毎フレーム値を載せる。✅RS-15 から `value` に vec2〜vec4 の数の配列、`offset`(byte)も書ける(§6.5) |
| `pelican.pose` | `pose.clip_sample` | `time` を受けてクリップを標本化し姿勢を出す。`rig` / `clip` は必須、`speed` / `loop` / `start_time` / `root_motion` / `root_node` は任意([第8章](08_gameplay.md) §8.13) |
| | `pose.blend` | 2 つの姿勢を `weight` で混ぜる |
| | `pose.state_machine` | `pelican.anim_graph` v1 文書を包む(`replay`)。パラメータ名がそのまま入力ポートになる |
| | `pose.retarget` | 姿勢を別のリグ向けに写す |
| | `pose.skeletal` | **sink**。owner 個体の姿勢としてコミットする |
| | `pose.transform` | **sink**。catalog には載りますが、**v0 では装着時に必ず拒否されます**(`pose.transform has no root writer yet`。[posegraphruntime.cpp:374](../../src/core/animation/posegraphruntime.cpp#L433))。root motion は `pose.clip_sample` の parameter として運ばれます |

登録されている **semantic 型は 12 個**です: `pelican.value.scalar@1`([logicalrendertype.cpp:557](../../src/project/logicalrendertype.cpp#L560))、`pelican.audio.voice@1` / `pelican.event.playback_control@1`([:562](../../src/project/logicalrendertype.cpp#L567))、6 つの `pelican.render.*`(`storage_buffer` / `attribute` / `color_signal` / `depth` / `legacy_opaque_image` / `legacy_opaque_resource`。[:580](../../src/project/logicalrendertype.cpp#L583) 〜 [:627](../../src/project/logicalrendertype.cpp#L630))、LP-3a の `pelican.render.draw_stream@1`(stream。draw_loop の 1 material の描き方の列。[:637](../../src/project/logicalrendertype.cpp#L637))、`pelican.pose@1` / `pelican.rig@1`([posegraphmodule.cpp:74](../../src/project/posegraphmodule.cpp#L74))。スカラーの経路を流れるのは `pelican.value.scalar@1` ですが、`render.*` のノードも型付きのポートを持ちます —— `render.buffer` / `render.attribute` は `outputs["resource"]` を、`render.image` は宣言から作った resource 型を、`render.compute` / `render.fullscreen` / `render.raster` は `parameters.ports` の各ポートを `pelican.render.*` で型付けします([`renderPorts()`](../../src/project/graphmodule.cpp#L328))。`pelican.pose@1` は **opaque** で、グラフを流れるのは姿勢の bytes ではなく **staged pose のハンドル + レイアウト世代 + フレーム改訂**です(この約束の正はヘッダのコメント [posegraphmodule.hpp:11](../../src/project/posegraphmodule.hpp#L11))。だからグラフはジョイントを読みません。`pelican.rig@1` は登録されているだけで、**どのポートにも現れません**([第8章](08_gameplay.md) §8.13)。

### `definitions` と `graph.instance`

`definitions` は **名前 → 部分グラフ文書**(`{nodes, outputs}`)の辞書です。部分グラフの入力口は `graph.input` という特別なノードで、`default` を書けます。root の `nodes` に置いた **`graph.instance`** がそれを 1 つ実体化します([graphcomposition.cpp:88](../../src/project/graphcomposition.cpp#L760))。

```json
{"id": "hero_walk_run", "op": "graph.instance", "definition": "walk_run",
 "owner": {"scene": "default_scene", "object": 1},
 "parameters": {"weight": 0.0},
 "inputs": {"weight": {"node": "blend_curve", "port": "value"}}}
```

- 展開後のノード id は **`<instance id>/<定義内の id>`**(上なら `hero_walk_run/mix`)。エラーメッセージにはこの展開後の名前が出ます。
- `owner` は **`scene` と `object` のちょうど 2 キー**で、`object` は **正の整数の authoring object id** です。この id は `scene.json` には書かれておらず、**宣言順に振られる通し番号**です(既定では 1 始まりですが、**文書内のシーンをまたいで連番**なので 2 つ目のシーンの先頭は 1 ではありません — [第4章](04_scene_ecs.md) §4.1。[graphcomposition.cpp:100](../../src/project/graphcomposition.cpp#L779))。**並べ替えると指す個体が変わります** —— 永続 id は v0 の穴として記録されています。存在しない id は **inactive**(装着が効かないだけで、ロードは止まりません)。ただし**無音ではなく**、そのシーンで初めて解決できなかった scope をまとめた `LOG_WARNING` が 1 回出ます([scenegraphruntime.cpp:61](../../src/core/loader/scenegraphruntime.cpp#L66)。[第4章](04_scene_ecs.md) §4.1)。存在する別の個体を指すと装着の解決が名前付きで失敗します。
- `definition` の綴り違いは `unknown graph definition`、定義の再帰は `recursive graph definition`、インスタンス同士の循環は `cyclic subgraph connection` です。
- `definitions` の中のノードと、インスタンス経路越しのノードは **`edit_graph_document` では編集できません**(`graph.definition_immutable@1`)。[第13章](13_editor.md) を参照してください。

### 入力の 4 つの mode と優先順位(✅P-1)

instance の入力ポート 1 本ごとに、値がどこから来ているかが決まります。

| mode | 供給元 |
|---|---|
| `connected` | 展開後にそのポートへ入る **edge**(`inputs` に書いた接続) |
| `parameter` | instance の **`parameters`** に書いた固定スカラー |
| `default` | 定義側の `graph.input` ノードの **`default`** |
| `override` | 実行時の上書き(`RenderContext::setValueGraphParameter` = `ctx.render().setValueGraphParameter` / RPC) |

**優先順位は `edge > parameters > default`** です([graphcomposition.cpp:48](../../src/project/graphcomposition.cpp#L687))。以前の「edge と `parameters` の二重指定は拒否」は P-1 で**優先順に緩められました** —— 上の posegraph_demo の実物が、edge を張ったうえで `parameters.weight: 0.0` を残しているのはそのためです。**`parameters` は「接続を外したときに戻る値」**なので、消さずに残すのが正しい形です(そして `default` を持たない入力も外せるようになりました)。

実行時の `override` は 4 つ目の mode として由来に出ます。ただし **edge が供給しているポートを上書きしようとすると `graph.override_shadowed@1`** になります —— 展開が `value.parameter` ノードを作っていないので上書きの着地点が無く、**黙って消えるのではなく名前で断ります**([scenegraphruntime.cpp:123](../../src/core/loader/scenegraphruntime.cpp#L129))。

現在の実効値と由来は、再評価せずに **publish 時のスナップショット**から読みます(`get_graph_values{instance}` / `get_graph_node_values{instance?}`。[第10章](10_tools.md))。

### sink — グラフの末端

**sink** は出力ポートを持たず、受け取った値をエンジン側の何かへ渡して終わるノードです。v0 の sink は 7 本です(OFF にした module の sink はその分減ります)。

| sink | 渡す先 |
|---|---|
| `render.scalar` | `parameters.target` が名指すフレームグラフバッファ(`host_source: "runtime_f32"`)。同じ buffer に offset を変えて複数書ける(✅RS-15)。§6.5 |
| `audio.volume` / `audio.pitch` | 音声ボイス |
| `pose.skeletal` | owner 個体の姿勢。**owner 1 個につき 1 本**で、2 本目は `pose.sink_conflict@1` |
| `pose.transform` | (上記のとおり v0 では装着時に拒否) |
| `ui.value` / `ui.visible` | UI の node の値 / 表示。owner に属さないので、**単一 writer の単位は `(文書, path, 性質)`**(二重 writer は準備時に `ui.sink_conflict@1`) |

姿勢の sink が **owner 単位の単一 writer** であることが、`animation` コンポーネントとの排他の土台です([第8章](08_gameplay.md) §8.13、[第4章](04_scene_ecs.md) §4.2)。

### catalog の読み方

語彙は文書ではなく**走っているエンジンが正本**です。2 つの引き方があります。

```console
$ pelican_cli graph-catalog        # 引数なし。JSON を stdout へ
```

RPC なら `get_graph_catalog`([第10章](10_tools.md))。どちらも同じ `pelican.graph_catalog` v1 を返します(✅EL-4 から RPC の方にだけ `material_pass_contracts` / `material_output_formats` の 2 つの鍵が足されます。engine の shader の一覧は ✅EL-4b から別の RPC `get_engine_shaders` が返します。[第13章](13_editor.md) §13.5):

```json
{ "schema": "pelican.graph_catalog", "version": 1,
  "modules": [ { "namespace": "pelican.pose", "version": 1, "description": "...",
    "types": [ {"id": "pelican.pose@1", "constructor": "value", "traits": ["opaque","pose"], ...} ],
    "operations": [ { "op": "pose.blend", "domain": "animation", "evaluation": "direct",
        "change": {"default": "value", "parameters": {}}, "sink": false, "ports": "declared",
        "inputs": [ {"port": "a", "required": true, "accepted_type": {...}} ],
        "outputs": [ {"port": "pose", "type": {...}} ] } ],
    "sinks": ["pose.skeletal", "pose.transform"] } ] }
```

読む順序は **`modules[].operations[]` → `op` → `domain` / `evaluation` / `change` → `inputs` / `outputs`** です。`sink: true` の op は `outputs` が空で、module の `sinks` 配列にも名前が出ます。`outputs` の要素は `{port, type}` のほか、✅RS-4 から `{port, type_from_input}` があります(型を持たず、`type_from_input` が名指す入力 socket に入る edge の型になる。今は `render.shadow_depth` の `depth` と `render.snapshot_copy` の `destination`)。`ports: "parameter_dependent"` の op は `inputs` / `outputs` のキー自体が無く、ノードの書きぶりで決まります。

**必須の parameter と value.input の名前(✅CT-1、`ac8d836b`)。**どの op にも `required_parameters` が付きます。中身は、node をその op で組むときに `graph.render_node_parameters_missing@1` を出す検査と**同じ表**([`renderNodeRequiredParameters()`](../../src/project/graphmodule.cpp#L262))から作った配列で、単独の必須は `{name}`、どれか 1 つでよい組は `{one_of: [...]}`、必須の無い op は `[]` です([組む所](../../src/project/graphmodule.cpp#L576))。例: `render.scalar` は `[{"name": "target"}]`、`render.compute` は `[{"name": "ports"}, {"one_of": ["shader", "ray_tracing"]}]`、`audio.volume` は `[{"one_of": ["target", "voice"]}]`、`value.add` は `[]`。名前は同名の入力 edge でも満たせます(`audio.volume` の `voice` の edge など。下の §6.18「名前付きの拒否の一覧」の RS-9 / RS-15 の項)。`value.input` の op にだけ `input_names: [{name, type, description}]` が付き、`value.input` の `name` に書ける名前を並べます —— `time`(game の時計の秒)・`delta_time`(この frame の長さの秒)・`scene_time`(今の scene が始まってからの秒)で、`type` は `pelican.value.scalar@1` を catalog の型の書き方(`{semantic, ...}`)にしたものです([`graphFrameInputNames()`](../../src/project/graphmodule.cpp#L927))。frame が値を渡す名前の検査(`validateGraphInputs()`)も同じ表を読むので、並んだ名前は通り、それ以外は `unbound frame graph input: <名前>` で断られます。test: `curve_test` の [「CT-1 the catalog lists each op's required parameters and the names value.input takes」](../../test/curve_test.cpp#L1147)。

**各 op の parameter の形(✅CT-3、`0d215a68`)。**どの op にも `parameters` と `parameters_complete` が付きます。catalog 本体が出す鍵なので、`pelican_cli graph-catalog` にも出ます([CT-3 で op に足す所](../../src/project/graphmodule.cpp#L577))。表は [`graphparameterschema.cpp` の CT-3 の表 `makeTable()`](../../src/project/graphparameterschema.cpp#L386) の 1 つで、各 op の parser が読む鍵から拾ったものです(コードの注記)。`required_parameters`(上の CT-1)と RS-9 の必須の検査も、この表の `required` から作ります(RS-9 の表は CT-3 で置き換えました)。

`parameters` の要素は `{name, kind, required?, alternatives?, default?, enum?, min?, max?, items?, fields?, expression?, description?}` です([1 件を JSON にする所 CT-3](../../src/project/graphparameterschema.cpp#L794))。

- **`kind`** は `number` / `integer` / `bool` / `string` / `enum` / `vec2` / `vec3` / `vec4` / `color` / `array` / `object` / `asset` / `shader` / `node_ref` / `expression` / `json` / `union`(✅CT-4)のどれかです。`json` は形を言えない値です(`render.material_pass` の `material_filter`、`render.compute` の `dispatch` など)。`union` は 2 通りの形を取る値で、形を `variants` に並べます(下の CT-4 の段落)。今の表で `vec2` / `vec3` / `vec4` / `expression` の kind を持つ parameter はありません。
- **入れ子**: `object` は名前の決まった member を `fields` に並べます。`items` があれば、任意の鍵を持つ map です(`view_select` の軸の名前 → 番号、`clear_colors` の出力の名前 → 色)。`array` の `items` は要素の形です。`items` の中は `name` を持ちません。`shader` は、`fields` があれば stage の object(`render.fullscreen` の `vertex` / `fragment` / `defines` など)、無ければ shader 1 本の path の文字列です(`render.compute` の `shader`)。
- **`required`**: 最上位の `required` は、RS-9 の `graph.render_node_parameters_missing@1` が断る条件です。`alternatives` は、代わりにそれを書けば足りる名前です(`required_parameters` の `one_of` の 2 つ目から)。`fields` の中の `required` は、その op の parser が断る条件を表示のために書いたもので、CT-3 の検査はこれを見ません。
- **`default`** は parser が省略時に使う値です。既定の無い parameter には鍵がありません。`enum` は取れる値、`min` / `max` は範囲です。符号なし 32 bit の値は `min: 0, max: 4294967295` です。
- **`expression: true`** は、ループの body で `$expr` / `$format` を書ける所です(LP-1b の許可の表。下の §6.18「ループの式」)。
- **`parameters_complete`** が `false` なら、並んだものは正しいが、parser はほかにも読みます。今 `false` の op は `render.image` / `render.fullscreen` / `render.raster` / `render.compute` / `render.material_pass` の 5 つです。`ports` の中の port の形、`raster_state`・`dispatch`・`ray_tracing`・draw の中身は出していません(`json` か、`fields` の無い `object` として並びます)。表に載っていない op は `parameters: []`・`parameters_complete: false` です。parameter を持たない op(`value.add`・`pose.blend`・`draw.material` など)は `parameters: []`・`parameters_complete: true` です。

例(test の期待): `pose.clip_sample` は `rig`(`asset`、必須)・`clip`(`string`、必須)・`speed` `{"name": "speed", "kind": "number", "default": 1.0}`・`start_time`(既定 0)・`loop` `{"name": "loop", "kind": "bool", "default": true}`・`root_motion`(`enum` `["none", "extract"]`、既定 `none`)・`root_node` の順です。`render.buffer` の `element` は必須の `enum`(13 個)、`size` は `integer` で `min: 0`、`lifetime` は `["persistent", "transient"]` で既定 `persistent` です。`render.shadow_depth` の `shader` は `kind: "shader"` で、`fields` が `[{"name": "vertex", "kind": "string", "required": true}]`、`default` が `{"vertex": "engine://shadow_depth"}` です。`render.image` の `type` は `object` で、`fields` が `semantic`(`string`)と `arguments`(`items` が `string` の map)です。`initial` の `fields` は `[{"name": "asset", "kind": "asset", "required": true}]` で、✅CT-4 からこの要素に `"asset_kind": "image"`・`"extensions": [".ktx2", ".png", ".jpg"]` も付きます。

CT-3 で新しく必須になった名前は、`pose.clip_sample` の `rig` / `clip`、`pose.state_machine` の `rig` / `graph`、`pose.retarget` の `profile`、`draw.variant` の `variant` です。CT-3 の前は、これらの op の parser が自分で断っていました(test の注記)。今は RS-9 の `graph.render_node_parameters_missing@1` で断られます。表の値の型・enum・範囲の検査は、§6.18「名前付きの拒否の一覧」の CT-3 の項です。test: `curve_test` の [「CT-3 the catalog lists each op's parameters with their kinds and a wrong value is refused by name」](../../test/curve_test.cpp#L1207)。この test は、全 op の `required_parameters` が表の `required` から作ったものと同じで、CT-3 の前の RS-9 の表に上の 6 つを足したものとも同じであることも見ます。

**値の出どころ・asset の種類・2 通りの形・実用の範囲・置ける場所(✅CT-4、`4d682338`)。**Studio が CT-3 の表から欄を作るのに足りなかったものを、同じ表に足しました。CT-3 の検査(何を断るか)は変わりません(test の注記)。`parameters` の要素に `soft_min?` / `soft_max?` / `asset_kind?` / `extensions?` / `choices_from?` / `variants?` が加わり、op に `placement` が付きます([CT-4 の鍵を JSON にする所](../../src/project/graphparameterschema.cpp#L803))。

- **`choices_from`**: string の値を engine の別の一覧から選ぶ parameter に付きます。形は 2 つです([`GraphParameterChoices`](../../src/project/graphparameterschema.hpp#L55) の注記。✅CT-5 で 3 つ目の `asset_manifest` と、条件の `where` が加わりました。下の CT-5 の段落)。
  - `{rpc, key, field?}`: RPC の返り値の中の一覧です。`key` は返り値の中の道を `.` でつないだもので、途中で配列に会えば要素ごとにたどります。`field` は各要素の中で名前を持つ member で、無ければ要素そのものが名前です。`field` の値が配列なら、その中の文字列の 1 つずつが名前です。今の `rpc` はどれも `get_render_authoring_context` です。
  - `{graph_nodes: [<op>...]}`: 同じ graph の、その op の node の id です。
- 付いている parameter(下の表の `rpc` は省いています。どれも `get_render_authoring_context`):

| parameter | `choices_from` |
|---|---|
| `view_family`(pass を作る 6 op と、`render.buffer` / `render.attribute`) | `{key: "view_families"}` |
| `pass_set` | `{key: "graphs", field: "name"}` |
| `canonical_anchor` | `{key: "canonical_anchors"}` |
| `render.image` の `format` と、`format_candidates` の要素 | `{key: "render_target_vocabulary.formats"}` |
| `render.image` の `format_class` | `{key: "render_target_vocabulary.format_classes"}` |
| `render.image` の `usage` の要素 | `{key: "render_target_vocabulary.usage"}` |
| `render.target_ref` の `name`、`render.buffer` の `size_from_extent.resource` | `{key: "render_targets", field: "name"}` |
| `render.material_pass` / `render.fullscreen` の `inherit_bindings_from` | `{key: "graphs.passes", field: "name"}` |
| `draw.variant` の `variant` | `{key: "materials", field: "variants"}` |
| `audio.volume` / `audio.pitch` の `target` | `{graph_nodes: ["audio.playback"]}` |
| `render.scalar` の `target` | `{graph_nodes: ["render.buffer", "render.attribute"]}` |

- `get_render_authoring_context` には CT-4 で `view_families`(`$main` と、登録された view family の provider の family の名前)と `canonical_anchors`(合成が pass を置く 8 つの anchor を pipeline の順に。`sprite` から `imgui` まで)を足しました([CT-4 で context に view_families を足す所](../../src/core/communication/renderconfigeditor.cpp#L2651))。
- **`asset_kind` と `extensions`**: kind `asset` の parameter の asset の種類と、それを読む所が受ける拡張子です。`pose.clip_sample` / `pose.state_machine` の `rig` は `model`(`.glb` / `.gltf` / `.vrm`)、`render.attribute` の `source.asset` は `model`(`.glb` / `.gltf`)、`render.image` の `initial.asset` は `image`(`.ktx2` / `.png` / `.jpg`)、`audio.playback` の `asset` は `audio`(`.wav` / `.ogg`)、`value.sample_table` の `asset` は `analysis_table`(`.ktx2`)です。
- **kind `union` と `variants`**: 2 通りの形を取る値は kind `union` で、`variants` に形を並べます(各形は `name` を持ちません)。今の 3 つ:
  - pass の `ray_query`(`render.shadow_depth` / `render.material_pass` / `render.fullscreen` / `render.raster`): `bool` か、`acceleration_structure`(string、必須)を持つ `object`。既定は `false`。
  - `render.image` の `mip_levels`: `integer`(`min: 1`、`soft_max: 14`)か、`enum` `["full"]`。
  - `value.sample_table` の `row`: `integer`(0 以上の 32 bit)か `string`。
  - CT-3 の前の `json` と同じく、`union` の値は検査しません(op の parser が判定します。`ray_query: "yes"` も CT-3 の検査を通ることを test で確かめています)。
- **`soft_min` / `soft_max`**: slider の範囲で、`min` / `max` の内側です。検査はしません。今付いているのは `render.image` の `width` / `height` の `soft_max: 8192` と、`mip_levels` の整数の形の `soft_max: 14` だけです(`layers` には付けていません)。8192 は engine の feature の `resolution` の範囲 `[64, 8192]`(`shadow_directional` / `shadow_point` / `shadow_spot` / `planar_reflection` / `cube_capture`)、14 はその mip の段の数 `floor(log2(8192)) + 1` です([CT-4 の 8192 の出どころの注記](../../src/project/graphparameterschema.cpp#L146))。
- **op の `placement`**: どの op にも `"anywhere"` か `"owned_instance"` が付きます([CT-4 で op に placement を足す所](../../src/project/graphmodule.cpp#L588))。`owned_instance` は `owner` 付きの `graph.instance` の中にしか置けない op で、`pose.clip_sample` / `pose.blend` / `pose.state_machine` / `pose.retarget` / `pose.skeletal` / `pose.transform` と `value.morph_weight` です(owner の model を読む・owner の skeletal の sink に書くため。コードの注記)。ANIMATION=OFF の build では `value.morph_weight` だけです。
  - 外に置くと `graph.op_requires_owned_instance@1` `{node, op}` で断ります。検査は graph の partition の中で、編集でも読み込みでも同じ所を通ります([CT-4 の placement の検査を呼ぶ所](../../src/project/graphpartition.cpp#L374)、[`checkGraphOperationPlacement()`](../../src/project/graphparameterschema.cpp#L789))。`node` は展開名で、owner の無い instance の中なら `hero/walk` のようになります。`graph.` の code なので CT-2 の `document`(と、body の node なら `definition`)も付きます。owner を付ければ同じ文書が通ります(test)。

例(test の期待): `render.scalar` の `target` は `{"graph_nodes": ["render.buffer", "render.attribute"]}`(✅CT-5 からはこれに `where` が付きます。下の CT-5 の段落)、`render.shadow_depth` の `pass_set` は `{"rpc": "get_render_authoring_context", "key": "graphs", "field": "name"}`、`render.shadow_depth` の `ray_query` は `{"kind": "union", "default": false, "variants": [{"kind": "bool"}, {"kind": "object", "fields": [{"name": "acceleration_structure", "kind": "string", "required": true}]}]}`、`render.image` の `width` は `max: 4294967295`・`soft_max: 8192` です。test: `curve_test` の [「CT-4 the catalog says where a value is chosen from, an asset's kind, a union's shapes, a slider's range and where an op may stand」](../../test/curve_test.cpp#L1427) と、`renderconfigeditor_test` の [「CT-4 an edit placing an owned-instance op at the root is refused by name」](../../test/renderconfigeditor_test.cpp#L1497)(root に置いた `value.morph_weight` と `pose.clip_sample` の編集が rejected で、`document: "project://passes/main.json"` が付き、文書は書かれないこと)。

**値の出どころの条件、asset の manifest、残りの一覧(✅CT-5、`65f3a8a3`)。**GUI の UX-12e の報告で、CT-4 の表に残っていた出どころの無い string を、engine が実際に読む一覧があるものだけ埋めました。CT-3 の検査の規則は変わりません。

- **`where`**: `choices_from` に付く条件で、候補をその member の値が合うものだけに絞ります。RPC の一覧なら各要素の member、`graph_nodes` なら各 node の parameters の値を見ます。値が 1 つなら文字列、複数なら配列(どれか 1 つ)です([CT-5 の `where` を JSON にする所](../../src/project/graphparameterschema.cpp#L827))。
- **`asset_manifest`**: 3 つ目の形 `{asset_manifest: {parameter, manifest: "<asset>.import.json", key}}` です。同じ node の parameter `parameter` が名指す asset の隣の import manifest の、`key`(`.` でつないだ道)の名前が候補です([CT-5 の `asset_manifest` を JSON にする所](../../src/project/graphparameterschema.cpp#L813))。
- 足した `choices_from`(`rpc` を省いたものは `get_render_authoring_context`):

| parameter | `choices_from` |
|---|---|
| `render.scalar` の `target` | `{graph_nodes: ["render.buffer", "render.attribute"], where: {host_source: "runtime_f32", element: ["float", "vec2", "vec3", "vec4", "mat4"]}}`(書ける buffer の条件。§6.5 の CT-5 の項。[書ける要素の一覧](../../src/project/graphparameterschema.cpp#L152)) |
| `value.sample_table` の `row` の文字列の形 | `{asset_manifest: {parameter: "asset", manifest: "<asset>.import.json", key: "source.metadata.layout.features"}}` |
| `render.image` / `render.target_ref` の `type.semantic` | `{rpc: "get_graph_catalog", key: "modules.types", field: "id", where: {constructor: "image"}}` |
| pass の `ray_query` の object の形の `acceleration_structure`(`render.fullscreen` などの pass の op と `render.compute`) | `{key: "acceleration_structures"}` |
| `region_tags` の要素(`render.fullscreen` / `render.raster` / `render.material_pass` / `render.compute` / `render.shadow_depth` / `render.snapshot_copy`)と `regions` の要素(前の 4 つ) | `{key: "graphs.passes.declaration", field: "regions"}`(合成された pass が宣言している tag。新しい tag も書けます。コードの注記) |

- **`push_constants`**: `render.fullscreen` の `push_constants` は kind `enum` `["none", "camera_position", "projection_view"]`、既定 `none` になりました(fullscreen の parser が読む名前。[CT-5 の enum](../../src/project/graphparameterschema.cpp#L560))。CT-3 の検査に乗るので、外の値(例 `"camera"`)は `graph.render_node_parameter_invalid@1` で断られます(test)。
- **`get_render_authoring_context`** に 2 つ足しました。
  - `acceleration_structures`: ray_query の `acceleration_structure` に書ける名前で、先頭が `"scene"`、その後に合成された `scene_acceleration_structures` の各要素の `name` です(ほかの名前は compiler が断ります。コードの注記。[CT-5 で一覧を足す所](../../src/core/communication/renderconfigeditor.cpp#L2662))。
  - `render_target_vocabulary.format_class_patterns`: 引数を取る format class の書き方です。今は `[{name: "explicit", form: "explicit(<format>)", argument_choices_from: {rpc: "get_render_authoring_context", key: "render_target_vocabulary.formats"}}]` の 1 つです([CT-5 で書き方を足す所](../../src/core/communication/renderconfigeditor.cpp#L2632))。
- **付けなかったもの**(engine が読む一覧が無い。test): `pose.retarget` の `profile`、`draw.override` の `defines` の要素、`render.attribute` の `source.semantic`。`soft_min` はどこにも足していません。
- definition を owner 付きの instance だけが使うかは、`get_graph_instances` の `definition_instances` で読みます([第13章](13_editor.md) §13.5 の表の `get_graph_instances`)。
- test: `curve_test` の上の CT-5 の test(`where` と `asset_manifest` の形、`type.semantic`・`acceleration_structure`・`region_tags` の値、`push_constants` の enum、付けなかったもの)と、`renderconfigeditor_test` の [「CT-5 the authoring context lists acceleration structures and the explicit format class's form」](../../test/renderconfigeditor_test.cpp#L1668)。

ANIMATION と AUDIO が ON の build では 5 module / 37 op(✅AB-1 の `value.sample_table` を含む。v0_freeze の表の行の数)。ANIMATION=OFF で 4 / 31(`pose.*` の 6 本が消える)、AUDIO=OFF で 4 / 34(`audio.*` の 3 本)、両方 OFF で 3 / 28。この対応は `test/run_devcli_graph_catalog.cmake` が [docs/v0_freeze.md](../v0_freeze.md) §1.3 の表と機械照合しています —— **表と実装がずれたらテストが赤になります**。

> **`pelican.value_graph` はプロジェクトに書く形ではありません。** コードとテストにこの schema 名が出てきますが、これは合成後の内部表現(とテストの入口)です。プロジェクトが書くのは `pelican.graph` root の `nodes` / `definitions` だけです。

## 6.18 共通グラフのループと、グラフで組む pass(✅LP-0a〜0e / LP-1a〜1e / LP-Va / LP-Va2 / LP-Vb / LP-2 / LP-3a / LP-3b / LP-6)

§6.17 の語彙に、**既存の target を指す node・pass の置き場所・1 種類のループ node `graph.loop`** が加わりました。これで bloom の段や planar reflection の mip 連鎖のように JSON で手展開していた反復と、cube capture の 6 面・影の cascade のように engine の中にあったループを、グラフの中で「形」として書けます。設計の理由・裁定・受け入れは [design_graph_loops.md](../design_graph_loops.md)(v3)が正で、この節は実装で確かめた書き方と拒否をまとめます。Studio での操作(ループゾーン・分岐表・Decompose into loop・View の欄)は [第13章](13_editor.md) です。

### 全体像 —— 展開は 2 段

グラフの lowering は preset と feature の合成より**先に**走りますが、ループの回数・target の有無・挿入先の pass は合成の**後**でないと決まりません。そこで処理が 2 段に分かれています。

| 段 | いつ | 何をする | 拒否が出る時 |
|---|---|---|---|
| 形の検査 | 編集のたび・読み込みのたび([`expandGraphDocument()`](../../src/project/graphcomposition.cpp#L1137)) | ループの parameters の形、分岐の入出力、入れ子、段の分類、式の文法と置き場所、展開名の参照 | 編集 RPC がその場で断る |
| 解決の段 | 組み直し(preset・feature・integration・surface resources・snapshot を合成した後。[`resolveGraphDocumentAfterComposition()`](../../src/project/rendergraphcomposition.cpp#L1511)) | target の参照と history の検査、ループの回数と body の複写、`pass_replacements` の適用 | 組み直しが断る(編集 RPC では ticket の結果、起動では起動エラー) |

保存されるのは書いた文書だけです。ループの展開結果(複写した pass・target、どの要素がどの分岐に入ったか)は派生物で、文書に書き戻されません。

### 既存の target を指す —— `render.target_ref`(✅LP-0a)

rendering 節・preset・feature が宣言した render target を、グラフの node から名前で指します。**所有しません** —— lowering は target を 1 枚も足さず、つないだ port はその target の名前をそのまま使います。aliasing と history は元の宣言に従います。

```json
{"id": "lit", "op": "render.target_ref",
 "parameters": {"name": "lit_color",
                "type": {"semantic": "pelican.render.color_signal@1",
                         "arguments": {"reference": "scene", "transfer": "linear", "range": "extended"}}},
 "outputs": ["resource"]}
```

- parameters は **`name` と `type` だけ**で、`type`(image の論理型)は必須です。`name` に `@` を含めると断られます(前のフレームは下の `history` で読む)。
- 名前が合成後に無ければ `graph.target_ref_unknown@1` `{node, name}`。検査は解決の段なので、**編集の時点では後から feature が足す target を名指してよい**のです。
- `type` の `semantic` が `color_signal` / `depth` のときは、合成後の宣言と突き合わせます。format の先頭が `D` なら depth、それ以外は color で、違えば `graph.target_ref_type_mismatch@1` `{node, name, field: "kind", declared, actual}`。`arguments.topology`(省略は `two_d`)も `cube` / `two_d_array`(layers > 1)/ `two_d` と比べ、違えば `field: "topology"`。`legacy_opaque_image@1` は何も言わない型なので比べません([`checkTargetReference()`](../../src/project/rendergraphcomposition.cpp#L1417))。✅FR-1(`45ca769b`)から、この拒否に **`expected: {type, layers?}`** が付きます。`type` は書き直せば通る型で、kind が合っていれば書いた型の `arguments.topology` を target のものに替えたもの(`two_d` なら鍵を消す)、合っていなければ depth の target には device の depth、color の target には `legacy_opaque_image@1` です。`layers` は target が `two_d_array` のときのその層の数です([`expectedTargetRefType()`](../../src/project/rendergraphcomposition.cpp#L1384))。例(test): `shadow_directional` の `cascade_count` を 3 にすると `shadow_map` は 3 層の配列になり、`two_d` の depth と書いた `render.target_ref` は `{field: "topology", declared: "two_d", actual: "two_d_array", expected: {type: <device の depth、topology two_d_array>, layers: 3}}` で断られ、`expected.type` に書き換えると通ります([「FR-1 a target_ref whose type contradicts its target is told the type to write」](../../test/featurecompose_test.cpp#L10077))。

**前のフレームを読む `history`。**`render.compute` / `render.fullscreen` / `render.raster` の port に `"history": true` を書くと、その image を `<target>@history` として読みます。読みだけで、同じフレームのどの pass も待ちません。

- 書く port(`access` が `read` 以外、または attachment)に書くと `graph.history_write@1` `{node, port}`(編集の時点)。history を持つ target そのものへ書くことは、一体版と同じく許されます。
- history を持たない target の `@history` を読むと `graph.history_unavailable@1` `{node, port, target}`(解決の段)。

### graph の pass の置き場所(✅LP-0b / LP-3b / LP-1e)

graph の render node(`render.fullscreen` / `render.raster` / `render.material_pass` / `render.shadow_depth` / `render.snapshot_copy`)と `graph.loop` は、feature の pass と同じ意味の置き場所を持てます。

| 書き方 | 意味 |
|---|---|
| `"insert": {"after": "<pass>"}` / `{"before": "<pass>"}` | その pass の直後 / 直前。pass は preset・feature・rendering 節・別の graph pass のどれでもよい(解決の段で探す) |
| `"canonical_anchor": "<anchor>"` | `insert: {after: <anchor>}` と同じ。anchor の bucket の中では feature の pass より後ろに入る。anchor は §6.5「canonical anchor」の 8 つ(`sprite` / `post_main` / `tonemap` / `post_ldr` / `pelican_ui` / `debug_draw` / `debug_text` / `imgui`) |
| `"insert": {"before"｜"after": "<pass>", "declared": true}` | ✅LP-3b。feature を合成する**前**に、rendering 節と preset が宣言した pass の列(と graph が末尾に足した pass)の中へ置く。feature の `insert`・`pass_overrides`・integration の `requires_passes` がこの pass を名指せる。`decompose_material_pass` がこの形を書く。`render.fullscreen` / `render.raster` / `render.material_pass` だけが受ける |

- `insert` と `canonical_anchor` は排他です。形の誤りは編集の時点で断られ、知らない anchor 名の `canonical_anchor` は `graph.insert_anchor_unknown@1` `{node, anchor}`(編集の時点)。`insert` の pass 名は解決の段で探し、どの family にも無ければ `graph.insert_anchor_unknown@1`、node の `pass_set` とは別の family にだけあれば `graph.insert_family_mismatch@1` `{node, anchor}` です([`placeRenderGraphPassRun()`](../../src/project/rendergraphcomposition.cpp#L1328))。`declared: true` の置き場で anchor が見つからないときも同じ code で、feature の pass は anchor にできません。
- `render.compute` には置き場所を書けません(compute task は常に全 pass の後に宣言されるため): `graph.insert_on_compute@1` `{node}`。body が compute だけのループに `insert` を書いたときも同じです。
- 同じ family・同じ向き・同じ anchor を名指す graph の pass は **1 つの run** として入り、並びは graph の依存の順(edge と `after` / `before`。同順位は id 順)です。feature の pass のように逆順にはなりません。
- **置き場所を書かない graph の pass と、置き場所の無いループの展開列**は、rendering 節の scene の pass と同じ扱いで、scene の pass の後・その family の最初の canonical anchor の前に入ります(待つ相手が後ろの bucket にあれば、その直後。✅LP-1e。[`insertAmongScenePasses()`](../../src/project/rendergraphcomposition.cpp#L2152))。ループの展開列は、graph の依存の順で隣にいる graph の pass があればその隣に入ります。
- ✅RS-3(`1b4f7527`)。**置き場所を書かない graph の pass の塊が書く image を、同じ family の rendering 節の pass(rendering 節と preset の scene の pass)が読む**なら、塊はその読み手のうち最初のものの**直前**に入ります。塊の中の順(graph の依存の順)は変わらず、隣にいるループの展開列も一緒に動きます。graph の pass の無い family に入る、置き場所の無いループの展開列も同じ規則です([`graphPassRunReaderPosition()`](../../src/project/rendergraphcomposition.cpp#L1687)。呼ぶ所は [graph の pass の塊](../../src/project/featurecompose.cpp#L3530) と [`insertAmongScenePasses()`](../../src/project/rendergraphcomposition.cpp#L2152))。これで、graph の pass が書く image の名前を rendering 節の `present` の `input` に書く構成が通ります。RS-3 の前は LP-1e のとおり scene の pass の後に入り、`Pass input target is not produced as an earlier output: <image> in pass: present` で断られていました。
  - 「読む」は LP-6 と同じ数え方で、`input`、`reads`、書かない resource port、`snapshot_copy` の `source` です。`@history` の読みは数えません([`declarationFrameReads()`](../../src/project/rendergraphcomposition.cpp#L1662))。塊が「書く」のは、pass の `output.color` / `output.depth`、`writes`、snapshot の `destination` です。
  - 動くのは、読み手より前にその image を書く pass も compute task も無い場合だけです。つまり今まで断られていた構成だけが変わり、今通っている構成の順序は変わりません。
  - canonical の並べ替えで最初の anchor より後ろに行く pass(swapchain に書く pass、`canonical_anchor` を持つ pass、type が `ui` / `debug_draw` / `gizmo` / `debug_text` の pass)は読み手に数えません([読み手に数える pass の判定](../../src/project/featurecompose.cpp#L3524))。
  - 明示の置き場所(`insert` / `canonical_anchor` / `declared: true`、置き場所を書いたループ)は対象外です。塊がその読み手かそれより後ろの pass を待つとき(塊の `after` がその pass を名指す、その pass の `before` が塊を名指す、塊が読む image をその pass が書く)は動かさず、今どおり断られます(明示が勝ちます)。
  - 分かっている穴(実装者の報告。test は無い): feature の pass が読み手より前で同じ image を書く構成は考えていません。graph の compute task が書いた image を読む場合は、次の RS-6 です(RS-3 だけでは塊を動かしませんでした)。
- ✅RS-6(`257cdef0`)。**graph の compute task が書く image を rendering 節の pass が読むと、その pass はその task を待ちます。**compute task は常に全 pass の後に宣言され、frame planner は前に宣言された書き手からしか読みの順を引かないので、RS-6 の前は graph の `render.compute`(ray tracing を含む)が書いた image を `present` の `input` に書くと `Pass input target is not produced as an earlier output: <image> in pass: present` で断られていました。
  - 対象の task は graph が作った compute task です: root の `render.compute`、ループが展開した task、`pass_replacements` で feature の task の代わりに入った task。✅RS-6b(`b8015064`)から、feature と integration の compute task(ray tracing を含む。例: `rt_shadow_mask_pipeline` の task)も対象です([task を選ぶ所](../../src/project/featurecompose.cpp#L4157)、[合成から RS-6 を呼ぶ所](../../src/project/featurecompose.cpp#L4163))。feature の task も全 pass の後に宣言されるので、RS-6b の前は `rt_shadow_mask_pipeline` の mask を `present` の `input` に書くと同じ言葉で断られていました。適用は root が `pelican.graph` の文書のときだけです(RS-6 と同じ)。
  - task が書く render target ごと、family ごとに、最初の canonical anchor(か `output_transform`)より前でその image を読む最初の pass に `after: <task>` を足します([`waitOnGraphTasksInScenePasses()`](../../src/project/rendergraphcomposition.cpp#L3075))。読み手の候補は RS-3 と同じく rendering 節と preset の pass で(graph の pass、feature の pass、swapchain に書く pass、type が `ui` / `debug_draw` / `gizmo` / `debug_text` の pass は数えません)、「読む」も RS-3 と同じ数え方です(`@history` の読みは数えません)。feature の pass は読み手に数えないので、feature が自分の `before` / `after` で決めた順は RS-6b でも変わりません。task の側には何も足しません。task はどの family の frame graph にも入るので、task が pass を名指すと、その pass の無い family で断られるためです。
  - 足さないとき: その pass より前の pass がその image を書く / その image の書き手(task 自身を含む)がすでにその pass より前に走る / task がその pass かそれより後ろを待つ(足すと循環になる)。「前に走る」は frame planner と同じ順(`after` / `before` と、前に宣言された最後の書き手から読みへの順)で辿ります([`FamilyOrder`](../../src/project/rendergraphcomposition.cpp#L3022))。buffer は対象外です(見るのは render target だけ)。
  - task が待っている、置き場所の無い graph の pass の塊も、その読み手の前へ動きます。「待っている」は、task の `after` が塊の pass を名指すか、塊の pass の `before` が task を名指すことで、そういう task を同じように待つ task も含みます。これで G-buffer を書く graph の pass → trace の task → `present` が組めます([`graphPassRunReaderPosition()` の RS-6 の分](../../src/project/rendergraphcomposition.cpp#L1741))。塊かその task が読み手かそれより後ろの pass を待つときは、塊を動かさず `after` も足さず、今どおり断られます。
  - 読み手がその image を `input` に挙げ、image が graph の `render.image` で `usage` に `SAMPLED` が無ければ、`SAMPLED` を足します。✅RS-6b から feature の render target も同じです([feature の target を加える所](../../src/project/featurecompose.cpp#L4162))。足すのは合成した config の宣言だけで、feature の文書は書き換えません。足した target は `get_frame_plan` の `resources[]` に `added_usage: ["SAMPLED"]` が付きます(graph の `render.image` に足したときも同じ。足していない target には鍵が無い。[第13章](13_editor.md) §13.5「`get_frame_plan` に足されたもの」)。
  - 足した順序は `get_frame_plan` の node の `added_after: [<task>, ...]` に出ます(足していない node には鍵が無い。[第13章](13_editor.md) §13.5「`get_frame_plan` に足されたもの」)。
  - 分かっている穴(実装者の報告。コードでは確かめていません): ループの展開で生まれる task を介した塊の移動と、`render.target_ref` を介した読みは扱っていません。
  - 分かっている穴(RS-6b、実装者の報告。コードでは確かめていません): feature の task が読む G-buffer を書く pass が `present` より後ろにある構成は、今も断られます。
  - test: `featurecompose_test` の [「RS-6 a scene pass that reads what a graph compute task wrote waits on that task」](../../test/featurecompose_test.cpp#L8956)(3 SECTION: `present` が task を待ち、task は何も名指さず、`SAMPLED` が足され、`added_after: ["trace"]` が出る / task が読む graph の pass が task と一緒に `present` の前へ動く / `present` を待つ graph の pass があれば何も動かず今どおり断られる)。
  - test(RS-6b): `featurecompose_test` の [「RS-6b a scene pass that reads what a feature's compute task wrote waits on that task」](../../test/featurecompose_test.cpp#L9039)(2 SECTION: blank の project に G-buffer を書く pass 2 つと `rt_shadow_mask_pipeline` を足し、`present` の `input` に mask を書くと、`present` が task を待ち、task は何も名指さず、mask の `usage` が `["STORAGE", "TRANSFER_SRC", "SAMPLED"]` になり、frame plan に `added_after: ["rt_shadow_mask_pipeline"]` と `added_usage: ["SAMPLED"]` が出る / `clustered_lighting`・`planar_reflection`・`rt_shadow_mask_pipeline` を足した構成は、graph の root で合成しても root の無い合成と同じ順で、`added_after` も `added_usage` も付かない)。
- **`pass_set` を省いた** graph の pass とループの body の pass は、合成後の config の**最初の** `rendering_passes` family に入ります(preset があれば preset の最初の family、無ければ rendering 節の最初の family、どちらも無いときだけ `main`。✅LP-1e。[`defaultGraphPassFamily()`](../../src/project/featurecompose.cpp#L3396))。LP-1e の前は常に `main` で、blank 雛形(family `main_render`)では family が 2 つになって断られていました。

### image node を読む pass の順(✅LP-6)

`render.image` node は target を宣言するだけで pass を作りません。LP-6 から、graph の lowering は **image node から読む pass の `after` に、その image を書く graph の pass を足します**([`waitOnImageWriters` の定義](../../src/project/rendergraphcomposition.cpp#L587))。LP-6 の前は id の順任せで、root の image を読むループの body がその書き手より前に置かれると `Pass input target is not produced as an earlier output` で断られていました。

- 書き手: その image を attachment か `write` / `read_write` の port で書く fullscreen / raster / compute、`render.material_pass` の color と `depth`、`render.shadow_depth` の `depth`、`render.snapshot_copy` の `destination`
- 待つ読み: fullscreen / raster / compute の `access: "read"` の port(`binding: "input"` の番号付きの入力を含む)、`render.snapshot_copy` の `source`、carry の最初の値を image node から取るループ
- 待たないもの: `history: true` の読み、別の body(ループ)の書き手、待てない書き手(graphics の pass は同じ family の pass と compute task だけを、compute task は compute task だけを待つ)、読み手の後に走ることが edge や `after` / `before` で決まっている書き手(循環を作らないため)、`read_write` の port の読み
- 対象外: `render.target_ref`(書く pass が graph の外にありうる)と buffer / attribute。これらの順は今までどおり明示の `after` か置き場所で書きます
- root の graph の pass は、ループの有無にかかわらず graph の依存の順(edge と `after` / `before`。同順位は id 順)に宣言されます。LP-6 の前はループの無い graph では id 順で、id 順が依存と逆だと循環で断られていました

規則の全体と理由は [design_graph_loops.md](../design_graph_loops.md) §5.2 です。

### グラフで書く pass —— `render.material_pass` / `render.shadow_depth` / `render.snapshot_copy`(✅LP-3a / LP-0e)

どれも rendering 節の同名の type の pass に lower され、**node の id が pass 名**になります。attachment は socket に `render.target_ref` か `render.image` をつないで決めます。✅RS-4(`010c038f`)から、**書いた socket は同名の出力 port として出てきます**(下の表の「出力 port」。`render.fullscreen` の attachment の port と同じ)。port の型はその socket に入る edge の型で、edge の元が別の pass の出力 port でも、そこから辿ります([`validateGraphPorts()`](../../src/project/graphcomposition.cpp#L1365))。その port を読む node は、書いた pass を `after` に持ち、読む資源は socket につないだ image の名前になります。fullscreen の attachment の port を読むときと同じで、ループの body の中や、ループの出力 `p` を経る読みでも同じです。`region_tags` は pass の `regions` になります(両方書けば順に合わせて 1 つ。✅LP-Vb)。

| op | socket | 出力 port(✅RS-4) | parameters |
|---|---|---|---|
| `render.material_pass` | color(`material_outputs` があればその名前、無ければ contract から: forward は `color`、deferred は `albedo` / `normal` / `material` / `worldpos` / `emissive`)・`depth`(必須)・`draws`(任意。下の draw_loop) | color の socket と同名の port と `depth`。`draws` は出さない | material pass の field([`isRenderMaterialPassParameter()`](../../src/project/graphmodule.cpp#L954)): `material_contract`(必須)/ `material_filter` / `material_variant` / `material_range` / `gpu_draw_source` / `material_outputs` / `material_output_states` / `screen_inputs` / `surface_resources` / `material_resources` / `ray_query` / load・store・clear の各 field / `view_family` / `view_select` / `region_tags` / `regions` / `resolution_domain` と `inherit_bindings_from`(✅LP-Vb)/ `pass_set` / `after` / `before` / `insert` / `canonical_anchor` |
| `render.shadow_depth` | `depth`(どんな image でもよい。影の map は depth の配列) | `depth` | `shader` / `ray_query` / `depth_load_op` / `depth_store_op` / `resolution_domain` と共通の `view_family` / `view_select` / `region_tags` / `pass_set` / `after` / `before` / `insert` / `canonical_anchor`([`renderPassKindPorts()`](../../src/project/graphmodule.cpp#L50)) |
| `render.snapshot_copy` | `source` / `destination`(topology が違ってよい。cube capture は cube を 2D 配列へ写す) | `destination`(`source` は出さない) | **`snapshot_after`(必須。写す直前の pass)**と上の共通の field。snapshot の名前は `destination` の target 名になる。`snapshot_after` が同じ body の graph pass なら `after` にも足される([`lowerRenderPassKindNode()`](../../src/project/rendergraphcomposition.cpp#L120)) |

- 出力 port を読むには、書いた node の `outputs` にその port 名を書きます。書いていない port を読むと `unknown graph output port: <node>.<port>` で断られます。読まないなら書かなくてよく、RS-4 の前に書いた `"outputs": []` の文書もそのまま通ります(fullscreen と違い `missing graph output` にはなりません)。
- 出力 port の型を辿って同じ port に戻ると `cyclic graph output forwarding: <node>.<port>`、その socket に何もつながっていなければ `unconnected graph input: <node>.<socket>` です(どちらも `std::invalid_argument` の文)。
- rendering 節の pass は graph の port ではなく名前で読みます。rendering 節の `present` に出力 port を「つなぐ」とは、その port が解決される image の名前(socket につないだ `render.image` / `render.target_ref` の target 名)を `present` の `input` に書くことです。書き手の graph の pass に置き場所が無ければ、RS-3 で `present` の前に入ります(上の「graph の pass の置き場所」)。書き手が graph の compute task なら、RS-6 で `present` がその task を待ちます(同じ所)。
- 同じ target を `render.target_ref` / `render.image` から名前で読む書き方も、今までどおり使えます。
- `render.material_pass` の `draws` をつながなければ、今の一体版の pass と同じ描き方です(material ごとに 1 手。pass に `material_variant` があればその variant)。つなぐ相手は draw_loop の `draws` 出力だけで、それ以外は断られます。
- format・枚数・大きさの検査は、JSON の pass と同じく pass を compile するときに行います。
- **`screen_inputs` / `surface_resources` に書ける名前(✅MP-1、`afd923e1`)**: engine の 2 つの一覧が正です([MP-1 の 2 つの一覧](../../src/project/materialscreeninput.hpp#L95))。`screen_inputs` は `opaque_color` / `opaque_depth` / `scene_depth` / `linear_view_depth`、`surface_resources` は feature が持つ `directional_shadow` / `point_shadow` / `spot_shadow` です。入力の契約を作る `makeBuiltinMaterialPassInputContract()` は、どちらの一覧にも無い名前を `unknown material pass input contract: <名前>` で断ります([MP-1 で一覧の外を断る所](../../src/project/materialscreeninput.cpp#L68))。同じ 2 つの一覧が、`get_graph_catalog` の `material_pass_contracts` の各 contract の `screen_inputs` / `surface_resources` に `[{name, source_type}]` で出ます。`source_type` は、つなぐ render target が持つべき semantic type です(`opaque_color` は `pelican.render.color_signal@1`、ほかは `pelican.render.depth@1`)。pass の parser は contract によって名前を変えないので、4 つの contract とも同じ一覧です([第13章](13_editor.md) §13.5)。
- **pass ごとに描いた material(✅MP-1)**: `get_frame_plan` の material pass の node に `drawn_materials: [{material?, id, draws}]` と `drawn_materials_frame` が付きます。中身は、最後に公開した logical frame で、その pass が実際に描いた material です。graph の `render.material_pass` に限らず、feature や rendering 節の material pass にも付きます。
  - `id` は、variant と draw_loop の step を解決した後の実効の material です(route と tag の照合も済んだ後)。`material` はその名前で、名前の無い material では鍵がありません。並びは最初に描いた順です。
  - `draws` は、その material について出した indirect draw の command の数を、その frame の pass の呼び出し全部にわたって足したものです。draw_loop が同じ range を 2 回描けば 2 倍になります。GPU が数を書く draw(`gpu_draw_source`)でも、数えるのは CPU の draw queue の range の command の数で、GPU が書いた数は読み戻しません([MP-1 で数える所](../../src/core/renderer/materialrender.cpp#L310))。
  - `drawn_materials_frame` はその frame の番号(`EngineTime::evaluationIndex()`)です。今の program と世代の frame がまだ公開されていなければ、2 つの鍵はありません。その frame に何も描かなかった pass は `drawn_materials: []` です。
  - 257 件目からは出さず、`drawn_materials_truncated: true` が付きます。
  - 形の表は [第13章](13_editor.md) §13.5 の「`get_frame_plan` に足されたもの」です。test: `headless_material_draw_loop_test` の [「MP-1 get_frame_plan lists the materials each material pass drew in the last frame」](../../test/headless_material_draw_loop_test.cpp#L1422)(GPU。deferred の pass に transparent の material が出ず、transparent の pass にだけ出ること、`forward_opaque` が `[]` であること、数が renderer の記録した draw と同じで、draw_loop の 2 step で 2 倍になること)。
- `render.snapshot_copy` の `source` と `destination` が graph の `render.image` を指すとき、lowering はその image の `usage` に、`source` 側は `TRANSFER_SRC` を、`destination` 側は `TRANSFER_DST` と `SAMPLED` を足します(書いた `usage` に無いものだけ。[足す所](../../src/project/rendergraphcomposition.cpp#L690)、[宣言に書く所](../../src/project/rendergraphcomposition.cpp#L996))。PS-1(`9b3c9566`)で足しました。UX-6a から Studio の palette が作る image は `TRANSFER_SRC` を宣言しない(alias から外れないように)ので、pass の image を写すと登録の時に `Snapshot copy usage mismatch` で断られていました(test のコメントの記述)。`render.target_ref` の先の target には足しません。test: `featurecompose_test` の [「a snapshot copy of a pass's own image gives the images the usages the copy needs」](../../test/featurecompose_test.cpp#L9708)。
- LP-0e で、`render.target_ref` の topology の判定が、feature の parameter から入った符号付き整数の `layers` を数え損ねる穴も塞ぎました。

### 番号付きの入力 —— `binding: "input"` と `input_index`(✅LP-1d)

graph の fullscreen / raster pass の読む image port は、既定では resource port(generated の `pelican_sample_<port>()` で読む)になります。port に `"binding": "input"` と `"input_index": <n>` を書くと、resource port ではなく pass の `input` 配列の n 番目になり、`inputTexture`、`PELICAN_DECLARE_INPUT_<i>`、可変の入力(§6.4)、`engine://passthrough` の shader がそのまま使えます。

- 読みの image port だけが受けます(attachment・書く port・`view` 付きは断る。`sampling` / `subresource` は ✅RS-14 から受ける。下)。`render.compute` は受けません。`history: true` とは併用できます。
- pass の `input` は、番号付きの入力が番号順に先、その後に resource port が読む資源が並びます。
- 番号は 0 から抜けも重複も無く並べます。違反は `graph.input_index_invalid@1` `{node, input_index, reason: "missing"｜"duplicate", ports}`(編集の時点)。
- ✅RS-14(`8c1787ca`)。**番号付きの入力にも、名前付きの resource port と同じ階層で `sampling` と `subresource` を書けます**(可変の入力の port も同じ `binding: "input"` の port です)。RS-14 の前は `an input port takes no sampling` / `an input port takes no subresource` で断られ、Graph で組んだ bloom の pass が画面の端で clamp できず、反対側の端を読んでいました(commit の記述)。
  - `sampling`: `{filter: "linear"(既定)｜"nearest", address: "repeat"(既定)｜"mirrored_repeat"｜"clamp_to_edge"}`。`subresource`: `{mip(既定 0), mip_count(既定 1。または "remaining"), layer(既定 0), layer_count(既定 1)}`。
  - `view` は今も断ります(`an input port takes no view: <port>`。view はその番号を宣言した shader のもの。[`view` を断る所](../../src/project/graphmodule.cpp#L442))。graph module(編集の時点)が見るのはこれだけで、`sampling` / `subresource` の値は pass の JSON を読む段(組み直し)で、名前付きの port と同じ parser が検査します。言葉も同じで、先頭の場所だけが違います(`pass '<pass>'.input_settings[<i>].sampling.address has unknown value 'bogus'` / 名前付きの port なら `<pass>.resource_ports.<port>.sampling.address has unknown value 'bogus'`)。
  - lowering は、番号付きの入力に書いた 2 つを pass の新しい field `input_settings`(§6.2 の表)に、`input_index` の順の配列で書きます。設定の無い番号は `null`、配列は設定を持つ最後の番号で終わります。どの番号付きの入力にも設定が無ければ field を出さないので、書かない graph の pass JSON は RS-14 の前とバイト単位で同じです([書く所](../../src/project/rendergraphcomposition.cpp#L983))。node の `parameters` に `input_settings` を直接書くと `render graph derives field from connections: input_settings` で断ります([断る所](../../src/project/rendergraphcomposition.cpp#L821))。
  - fullscreen と raster の pass は、それをその入力のサンプラーと view に載せます([`applyPassInputSettings()`](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L1673))。同じ pass の名前付きの resource port とは同居できます。既定値は変わりません(sampling は linear・repeat。subresource を書かない入力の view は RS-14 の前のまま)。
  - 画素で確かめたこと: 上半分が赤・下半分が緑の画像を下端の外(v = 1.25)で読む fullscreen pass は、`address: "clamp_to_edge"` を書くと全面緑、書かないと(repeat)全面赤です。サンプラーの値は、`bloom_composite` の番号 1 に `nearest` / `clamp_to_edge` / `mip 0` を書き、番号 0 は既定のまま、という組でも確かめています。raster の番号付きの入力と、`subresource` の画素は test で見ていません。
  - test: `featurecompose_test` の [「RS-14 a numbered input's sampling and subresource reach the pass as input_settings」](../../test/featurecompose_test.cpp#L8460)(書かなければ field が無い / 番号 1 だけなら `[null, {…}]` / 番号 0 だけなら 1 要素 / 名前付きの port と同じ言葉で断る / `view` は断る)、`vi1_variadic_inputs_test` の [「RS-14 a graph numbered input takes a named port's sampling and subresource onto its sampler」](../../test/vi1_variadic_inputs_test.cpp#L1634)(GPU)。
  - Studio の Graph の pass の欄は ✅UX-7(`1fcd155e`)から番号付きの入力の Filter / Address / mip を押せて、この 2 つを port に書きます。fullscreen の pass に新しく作る番号付きの入力には `clamp_to_edge` を書きます(engine の既定は `repeat` のまま。[第13章](13_editor.md) §13.5「pass の欄の Inputs」)。

### view の軸を選ぶ —— `view_select`(✅LP-Va / LP-Va2)

provider が作る view family の view は、名前の付いた整数の**軸の座標**を持ちます。

| family | 軸 | 長さが決まる時期 |
|---|---|---|
| `$shadow/directional` | `light`、`cascade` | `cascade` は組み直し時(feature の `cascade_count`)、`light` は実行時 |
| `$capture/cube` | `face`(0〜5。表示用のラベル `+X` / `-X` / `+Y` / `-Y` / `+Z` / `-Z`) | 組み直し時(6) |
| `$shadow/point` | `light`、`face` | `face` は 6、`light` は実行時 |
| `$shadow/spot` | `light` | 実行時 |

`$main`、`$reflection/planar`、作者が宣言した family(provider が無い)は軸を持ちません([`renderViewAxisLength()`](../../src/core/renderer/viewfamilyproviderregistry.hpp#L125))。

pass と compute task(rendering 節の JSON でも graph の render node でも)に `"view_select": {"<軸>": <整数>}` を書くと、その pass は**軸の値が一致する view だけ**で走ります。書かなかった軸は今までどおり全部を回ります。形は空でない object で、値は 0 以上の 32 bit 整数です([`parseRenderViewSelect()`](../../src/core/renderingpass/renderingpassjsonhelpers.cpp#L464))。

- 一致した view だけが invocation になり、順は family の view の順のまま、`logical_view_count` は family の view 数のままです(attachment の layer は今までどおり view の index)。一致する view が 0 個のフレームは、非活性の node と同じに完了扱いになります。
- `view_select` の違う pass は、1 つの physical scope に合流しません。`view_select` は frame planner の node・execution plan・physical scope の JSON と fingerprint まで運ばれます。
- 組み直しの拒否([`validateRenderViewSelects()`](../../src/core/renderer/viewfamilyproviderregistry.hpp#L165)): 二次でない family(`$main` など)は `graph.view_select_primary_family@1` `{pass, family}`、family に無い軸は `graph.view_select_axis_unknown@1` `{pass, family, axis}`(軸の長さの外の値は同じ code に `value` と `length` を足す)、実行時に長さが決まる軸は `graph.view_select_runtime_axis@1` `{pass, family, axis}`。
- physical target plan を作らない経路(二次 family の node が view 0 で 1 回だけ走る経路)では、`view_select` のある pass を `graph.view_select_needs_target_plan@1` `{pass}` で断ります。黙って layer 0 に上書きしないためです。
- `get_frame_plan` の node には `view_select` のほか、family の軸 `view_axes` と、直近のフレームで一致する view が無かった印 `no_selected_view` が載ります(形は [第13章](13_editor.md) の「ループと分解の RPC」)。

### `graph.loop` —— 1 種類のループ node(✅LP-1a / LP-1b / LP-Vb)

`graph.loop` は `graph.instance` と同じ**構造の node** です。実行時の op ではなく、catalog に載りません。置けるのは root の `nodes` だけで、body(definition)の中に置くと `graph.loop_nested@1` `{node, definition}` です(使われていない definition も含めて全部を見ます)。

```json
{"id": "bloom_up", "op": "graph.loop",
 "parameters": {
   "over":     {"range": {"count": 3}},
   "order":    {"by": "element", "direction": "descending"},
   "branches": [{"body": "bloom_up_step"}],
   "carry":    [{"from": "color", "to": "source"}],
   "insert":   {"before": "bloom_composite"}},
 "inputs": {
   "source": {"node": "bloom_down", "port": "color"},
   "target": {"node": "bloom_down", "port": "color[]", "element": {"$expr": "element"}}},
 "outputs": ["color"]}
```

(`engine://graphs/bloom_loop.json` の上り段。3 回を 2 → 1 → 0 の順に回り、各回が下り段の同じ番号の段へ書き戻す)

| parameter | 意味 |
|---|---|
| `over` | 集合。ちょうど 1 つ: `{"range": {"count": n}}`(n は 0〜65536 の整数か式)、`{"view_axis": {"family", "axis"}}`(✅LP-Vb)、`{"draw_items": {"contract"}}`(draw_loop。下) |
| `where` | 要素を絞る条件。`{"element": v}` / `{"element_in": [v, ...]}` / `{"element_not_in": [v, ...]}`(v は整数か式) |
| `order` | `{"by": "element", "direction": "ascending"｜"descending"}`(省略は昇順)。draw_loop は `{"by": "phase_default"}` だけ。ほかは `graph.loop_order_unsupported@1` `{node, order}` |
| `limit` | 並べた後の先頭 N 個(0 以上の整数) |
| `branches` | 上から順に `when` を見て、最初に合った分岐の `body`(definition 名)を使う。**最後の分岐は `when` 無し(else)**で、無ければ `graph.loop_branch_else_missing@1` `{node}`。`"body": null` はその回に何もしない |
| `carry` | `[{"from": <body の出力>, "to": <body の graph.input>}]`。前の回の出力を次の回の入力へ渡す |
| `tier` | 段の固定(任意): `"expand"` / `"draw_loop"`。実際の段と違えば `graph.loop_tier_mismatch@1` `{node, tier, actual}` |
| `after` / `before` / `insert` / `canonical_anchor` | 展開列全体の順序と置き場所(上の「置き場所」) |

node の `inputs` は body の `graph.input` へ、`outputs` は body の出力を公開する宣言です。形の検査は [`parseGraphLoopShape()`](../../src/project/graphcomposition.cpp#L227) です。

**body の規則**

- 全分岐の body(null を除く)は、同じ `graph.input` の集合と同じ出力の集合を持ちます。違えば `graph.loop_branch_interface@1` `{node, branch, definition, port}`。
- body の node は自分の置き場所(`insert` / `canonical_anchor`)を持てません。置き場所はループの parameters に書きます。
- body の中の `after` / `before` / `snapshot_after` が同じ body の node を指すときは、回ごとの名前に付け替わります(同じ穴は `graph.instance` の body でも塞がれました)。
- body に value node があると、その node は回ごとに 1 回ずつ評価されます。

**入力は 3 種類、出力は 2 種類**

| 入力 | 書き方 | 回ごとの値 |
|---|---|---|
| 全回に同じ | ふつうの edge | 全回に同じ値 |
| carry | `carry` の `to` にある入力。ループの入力 edge が必須 | 最初の回はループの入力 edge、以後は前の回の `from`。`"body": null` の回は値をそのまま次へ渡す |
| 回ごとの入力 | edge に `"element": <整数か式>` を付け、他のループの `<port>[]` を指す | 回ごとに式を評価し、その要素の値を読む。式の `element` は**受け手のループ**の要素。受け手がループでなければ定数だけ書ける |

- `outputs` に `"p"` と書くと**最後に body を持った回**の値、`"p[]"` と書くと回ごとの値の束です。`p[]` は edge に `element` を付けて 1 回分ずつしか読めません。
- 回が 1 つも無いループの `p` を読むと `graph.loop_output_empty@1` `{node, port}`。ただし `p` が carry の `from` なら、carry の入力の値がそのまま出ます。
- `element` が指す回が無ければ `graph.loop_element_unknown@1` `{node, port, element}`(node と port は受け手と、その入力)。
- carry の両端の型が違えば `graph.loop_carry_type_mismatch@1` `{node, from, to}`(編集の時点)。回ごとの入力を carry に使うことはできません。

### ループの式 —— `$expr` / `$format`(✅LP-1b)

回ごとに変わる値は **object の形の式**で書きます。平文の文字列は走査されません。

- 数値: `{"$expr": "element + 1"}`
- 文字列: `{"$format": "BLUR_STEP={element}"}`(`{…}` の中は整数の式。`{{` と `}}` は波括弧そのもの)

文法の正本は [graphloopexpression.hpp](../../src/project/graphloopexpression.hpp#L20) 冒頭の BNF です。使えるのは整数と小数のリテラル、`+ - * / %`、比較(`== != < <= > >=`)、`&& || !`、括弧、`min(…)` / `max(…)`、`pow2(n)`、`log2(n)`(✅RS-15)、組み直し時の定数 `mip_levels_of("<target>")` / `layers_of("<target>")` です。長さは 256 字、括弧の入れ子は 16 段までです。整数を求める場所では整数だけで計算し(`/` は 0 方向へ切り捨て)、実数を求める場所(`extent_scale`)では 2 つの 64 bit 整数の分数で計算して最後に 1 回だけ実数にします。

| 置ける場所 | 種類 | 読める変数 |
|---|---|---|
| ループの body の `render.*` node の `parameters.extent_scale` | 実数 | `element` / `index` / `count` |
| 同 `parameters.view_select.<軸>` | 整数 | 同上 |
| 同 `parameters.shader.defines[i]` | `$format` | 同上 |
| 同 `parameters.ports.<port>.subresource.mip` / `.layer` | 整数 | 同上 |
| ループの body の `render.image` の `parameters.width` / `.height` / `.layers` / `.mip_levels` | 整数(✅RS-15) | 同上 |
| ループの `over.range.count` | 整数 | なし(組み直し時の定数だけ) |
| ループの `where` の値 | 整数 | `element` |
| ループの分岐の `when` の値 | 整数 | `element` / `index` / `count` |
| ループの入力 edge の `element` | 整数 | 受け手がループなら `element` / `index` / `count`、そうでなければ定数だけ |

変数は `element`(集合の値。`view_axis` では軸の値)、`index`(並べた後の位置)、`count`(並べた後の回数)です。表の外(root の node、instance の definition、body の node の許可されていない field)に式を書くと `field_not_allowed` です([`bodyExpressionPlace()`](../../src/project/graphcomposition.cpp#L586))。`mip_levels_of` / `layers_of` は合成後の target の `mip_levels` / `layers`(省略なら 1、cube の `layers` は 6)で、合成後に無い target、`shadow_depth` の pass が書く target(実行時に layer が伸びる)、extent に従う値は `not_static` です([`composedConstants()`](../../src/project/rendergraphcomposition.cpp#L1920))。ただし ✅RS-15 から、`mip_levels: "full"` の target でも、`width` と `height` が正の整数で書かれ、`format_class` が `display` でなければ、`mip_levels_of` は `floor(log2(max(width, height))) + 1` を定数として返します(256×256 なら 9。[その規則](../../src/project/rendergraphcomposition.cpp#L1931))。大きさが画面に従う target の `"full"` は今までどおり `not_static` です。

**`log2(n)`(✅RS-15)。**正の整数 n の 2 を底とする対数を切り捨てた整数です(`log2(256)` は 8、`log2(255)` は 7)。n が整数でないとき、1 未満のときは `not_integer` で断ります([評価する所](../../src/project/graphloopexpression.cpp#L252))。GB-3(Blank から GUI で組んだ試験)で、ループの回数を `log2(N)` で書けなかった件から足しました(仕様書の記述)。

**body の image の大きさ(✅RS-15)。**ループの body の `render.image` の `width` / `height` / `layers` / `mip_levels` に整数の式を書けます(RS-15 の前は `extent_scale` だけで、`width` は `field_not_allowed` でした。[許す所](../../src/project/graphcomposition.cpp#L593))。式の結果の整数は、書いた数と同じく render target を読む所で検査されます。結果が整数でなければ `not_integer` です。例: `"width": {"$expr": "64 + element"}` の body を 3 回まわすと、回の image の幅は 64・65・66 になります。test: `featurecompose_test` の [「RS-15 a loop body image takes its width and height from an expression」](../../test/featurecompose_test.cpp#L6074) と [「RS-15 log2 rounds down, and mip_levels_of a fixed-size full chain is static」](../../test/featurecompose_test.cpp#L6104)。body に value node があるループは、回数・`where`・`when` が合成の前に決まらなければなりません(決まらなければ `not_static`)。

式の拒否はすべて `graph.loop_expression_invalid@1` `{node, pointer, reason}` で、`reason` は `syntax` / `unknown_variable` / `not_integer` / `not_static` / `division_by_zero` / `overflow` / `too_long` / `field_not_allowed`、`pointer` は**書いた文書の中の JSON pointer** です(回ごとの評価で断られても、書いた場所を指します)。

### 回の番号を shader に渡す —— `pelican_loop.glsl`(✅LC-1)

expand のループが展開した pass と compute task の shader は、自分が何回目の回か(element)と回数(count)を push constant で読めます(`158abae9`)。回ごとの define(`$format`)は回の数だけ compile が増えるので、回の番号を shader に渡す正式な道はこちらです。define は compile の時に分岐を選ぶ道として残ります。

```glsl
#include "pelican_loop.glsl"
// ...
uint element = pelican_loop_element(); // 0 始まり
uint count = pelican_loop_count();
```

- **include**: engine 同梱の [pelican_loop.glsl](../../src/core/resources/shaders/include/pelican_loop.glsl)。既定では include 自身が push constant の block `PelicanLoopPush { layout(offset = PELICAN_PUSH_LOOP_OFFSET) uint element; uint count; } pelican_loop_push;` を宣言します。`PELICAN_PUSH_LOOP_OFFSET` は 112 で、`PELICAN_PUSH_LOOP_BYTES` は 8(`[112, 120)`。[pelican_sets.glsl](../../src/core/resources/shaders/include/pelican_sets.glsl#L34))。engine の先頭 64 byte の後、保証される 128 byte の中です。
- **自分の push constant を持つ shader**(1 つの stage が持てる push constant の block は 1 つ)は、include の前に `#define PELICAN_LOOP_CUSTOM_PUSH` を書き、自分の block の最後に `PELICAN_LOOP_PUSH_FIELDS` を置き、block の後で instance の名前を `PELICAN_LOOP_FUNCTIONS(<instance>)` に渡します([macro](../../src/core/resources/shaders/include/pelican_loop.glsl#L38)):

  ```glsl
  #define PELICAN_LOOP_CUSTOM_PUSH
  #include "pelican_loop.glsl"
  layout(push_constant) uniform Push {
      mat4 projection_view;
      PELICAN_LOOP_PUSH_FIELDS
  } push;
  PELICAN_LOOP_FUNCTIONS(push)
  ```

- **engine が渡す所**: fullscreen / raster の pass([渡す所](../../src/core/renderer/fullscreenpassrenderer.cpp#L55))と compute task([渡す所](../../src/core/renderingpass/computetaskcontainer.cpp#L1900))。pipeline の stage のうち push constant の range が `[112, 120)` を丸ごと含むものを集め([`loopIndexPushStages()`](../../src/core/shader/shaderreflection.cpp#L549))、その stage flags の OR で 1 回 `pushConstants` します([`PipelineFactory::pushLoopIndex()`](../../src/core/shader/pipelinefactory.cpp#L1881))。raster で vertex と fragment の両方が include すれば両方に同じ値が入り、どの stage も宣言していなければ何もしません。
- **値**: element は展開の `element`(集合の値。並べた後の位置 `index` ではない)、count は展開後の回数(`get_frame_plan` の `loops[].count`)です([回ごとに決める所](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L471))。ループの外の pass と task は element 0・count 1 です。
- **compile は増えない**: 回の番号は値で define ではないので、同じ shader と同じ設定の回は同じ compile(同じ cache key)を共有します。pipeline は今までどおり pass ごとです。
- **断るもの**: push constant の range が `[112, 120)` に**一部だけ**重なる宣言(例: offset 100 から 16 byte)は、graphics と compute の pipeline を作るときに `shader.loop_push_overlap@1` `{shader, stage, range: {offset, size}}` で断ります([断る所](../../src/core/shader/shaderreflection.cpp#L560))。編集 RPC の結果では、draw_loop の code と同じく**その code のまま** fields が並びます(下の一覧の注記)。
- **見える所**: `get_frame_plan` の fullscreen / raster / compute の node と、`get_engine_shaders` の engine の shader の項目に、どれかの stage が `[112, 120)` を宣言していれば `reads_loop_index: true` が付きます(宣言していなければ鍵が無い。[第13章](13_editor.md) §13.5)。
- **対象外と既知の穴**
  - material pass の shader では使えません(engine は material pass に回の番号を push しません)。draw_loop も対象外です。
  - ray tracing の pipeline は重なりを検査しません(検査は graphics と compute の pipeline を作る所だけ)。compute task の dispatch は ray tracing の task でも同じ push を通るので、ray tracing の shader が `[112, 120)` を宣言すれば値は渡るはずです(コードの読み。test は無く未確認)。
  - fullscreen pass の `push_constants: "camera_position"` / `"projection_view"` は宣言だけで、engine は今その値を push していません(fullscreen の描画で `pushConstants` を呼ぶのは LC-1 の回の番号だけ)。上の `PELICAN_LOOP_CUSTOM_PUSH` で `projection_view` と同じ block に置く形は、compile と反映の test だけで確かめています。
  - GPU の test が描いて確かめているのは fullscreen と raster の pass です。compute task への push はコードの読みです。
- test: `headless_render_test` の [「LC-1 a loop body's passes read their element and count from push constants」](../../test/headless_render_test.cpp#L15255)(GPU。3 回のループの body の fullscreen pass が回の番号の色を書き、raster の vertex と fragment が同じ値を読み、ループの外の同じ shader の pass が 0 / 1 を読み、回の shader が 1 つの compile を共有する)と、`shader_compiler_reflection_test` の [「LC-1 pelican_loop.glsl declares the loop index at 112 in its own block or the shader's」](../../test/shader_compiler_reflection_test.cpp#L831)(`PELICAN_LOOP_CUSTOM_PUSH` の形と、重なりの拒否)。

### 一度だけ走る pass と、frame をまたぐ image —— `schedule: "once"` / `lifetime: "persistent"`(✅PS-1)

GB-3(Blank から GUI で FFT の海を組んだ試験)で、初期スペクトル(h0)と twiddle を毎 frame 計算し直していた件から足しました(`9b3c9566`。仕様書の記述)。**依っているものが変わった frame にだけ走る pass** と、**その出力を frame をまたいで保つ image** の組です。

```json
{"id": "h0_image", "op": "render.image",
 "parameters": {"type": {...}, "format": "R8G8B8A8_UNORM", "extent_scale": 1.0,
                "usage": ["COLOR_ATTACHMENT", "SAMPLED"], "lifetime": "persistent"},
 "outputs": ["resource"]},
{"id": "h0", "op": "render.fullscreen",
 "parameters": {"pass_set": "main_render", "schedule": "once",
                "shader": {"vertex": "engine://fullscreen", "fragment": "engine://passthrough"},
                "ports": {"level": {"kind": "buffer", "element": "float", "access": "read"},
                          "color": {"kind": "image", "type": {...}, "access": "write",
                                    "attachment": "color", "location": 0}}},
 "inputs": {"level": {"node": "level", "port": "resource"},
            "color": {"node": "h0_image", "port": "resource"}},
 "outputs": ["color"]}
```

(`featurecompose_test` の PS-1 の graph の抜粋。`level` は `value.constant` を受ける `render.buffer`。`twiddle` も once で `h0` の `color` を読んで persistent の `twiddle_image` に書き、毎 frame 走る `show` が `twiddle` の出力を読む)

**`render.image` の `lifetime`**

- `"transient"`(既定。今までの扱い)か `"persistent"`。ほかの値は編集の時点で `render.image lifetime must be persistent or transient: <node>` です([lifetime の検査](../../src/project/graphmodule.cpp#L391))。lowering は値をそのまま render target の宣言へ運び、rendering 節の `render_targets` にも同じ鍵を書けます(§6.2 の表)。
- persistent の image は 1 枚を保ちます(ping-pong の 2 枚にはしません)。runtime の alias の候補から外れ、`get_frame_plan` の `physical_target_plan.resources[]` で `aliasable: false`、`reason` に `persistent lifetime` が入ります([理由を足す所](../../src/core/renderingpass/renderingsamplecount.cpp#L126))。transient attachment と tile-local の候補にもなりません。
- layout tracker の reset(コードの注記では世代の commit・history の reset・view family の変化)をまたいで、その image の layout を覚えています([`RenderTargetLayoutTracker::reset()`](../../src/core/vkcore/render_target_layout_tracker.cpp#L337))。VkImage が作り直されたら(大きさ・format の変更、組み直し)、次に使う時に UNDEFINED から始めます([`RenderTargetLayoutTracker::forgetRecreatedPersistentImage()`](../../src/core/vkcore/render_target_layout_tracker.cpp#L363))。そのとき中身は未定義です。
- PS-1 は pass の load op を変えていません。
- `render.buffer` の `lifetime`(既定は persistent)は今までどおりです。

**`schedule`**

- `render.fullscreen` / `render.raster` は `"per_frame"`(既定)か `"once"`、`render.compute` は `"per_frame"` / `"per_view"` / `"once"`。ほかの値は編集の時点で `<op> schedule must be per_frame or once: <node>` / `render.compute schedule must be per_frame, per_view or once: <node>` です([schedule の検査](../../src/project/graphmodule.cpp#L481))。lowering は値を pass と task の `schedule` へ運びます。
- rendering 節の JSON の fullscreen / raster の pass にも `schedule` を書けます(field の所有表で受けるのはこの 2 つの type だけ。ほかの値は `Pass schedule must be per_frame or once: <pass>`。[`parsePassSchedule()`](../../src/core/renderingpass/renderingpassjsonhelpers.cpp#L510))。compute task の `once` は §6.6 の `schedule` の段落です。
- compute の once は、走る frame で `per_frame` と同じく view によらず 1 回 dispatch します。fullscreen / raster の once は、走る frame でその family の全 view に描きます。
- ループの body の pass / task に書くと、展開した全回が once です。
- once の node は、target planning で自分だけの物理 scope に isolate されます。走らない frame の node は scope を始めないので、走る node と scope(begin・clear・合流)を共有させないためです。書いた `target_planning` に足して効きます([足す所](../../src/core/renderingpass/renderingsamplecount.cpp#L160))。

**once が走る frame。**毎 frame、host の書き込み(value graph の sink、light)を stage した後で [`planOnceRuns()`](../../src/core/renderingpass/oncescheduler.cpp#L5) が決めます。次のどれかがあれば走り、無ければその node は何も記録しません。上から順に見て、最初に当たったものが理由です。

| 理由 | 起きる時 |
|---|---|
| `generation` | その node が今の runtime の世代でまだ走っていない(最初の frame、組み直しの後) |
| `requested` | `rerun_once` がその node を名指した |
| `upstream` | 同じ frame で plan の先に走る once の node が、この node の読むものを書く(h0 → twiddle の連鎖) |
| `output_recreated` | この node が書く resource の VkImage / VkBuffer が、前に走った時と違う(作り直された) |
| `input_changed` | この node が読む host 書きの buffer(`render.scalar` の sink、`runtime_f32` など)の bytes の digest が、前に走った時と違う。digest は bytes と長さの FNV-1a 64 です([digest を取る所](../../src/core/renderingpass/framegraphbuffercontainer.cpp#L303)) |

- 走らない frame の node は command を記録せず、scope も始めず、完了扱いになります(`view_select` に合う view が無い node と同じ。[飛ばす所](../../src/core/vkcore/renderer.cpp#L1759))。test 用の execution trace には `{"recorded": false, "once_skipped": true}` が出ます。
- 再生(Play / Stop)は世代を変えないので、once は走り直しません(GPU の test で確かめています)。
- 走った記録は、その frame を publish した後に node ごとに残ります。owner が非活性の node はその frame の判定に入りません。
- GPU の test が確かめている走り直しは、`rerun_once` と、文書を書き換えて `reload_render_documents` で組み直す道(`generation`)です。`input_changed` と `output_recreated` は `planOnceRuns()` だけを回す test で見ています。

**`rerun_once {node?}`** → `{scheduled: [<pass / task の名前>...]}`(plan の順)。次の frame で走ります。`node` を省くと今の frame plan の once の node の全部、graph の node の id を書くとその node が作った once の pass / task、`graph.loop` の id なら全回の once です。per_frame の node を名指すと `{scheduled: []}`、frame plan に無い名前は `-32602` で error の `data` が `{code: "node_unknown", node}` です(第10章 §10.3)。

**`get_frame_plan`** の once の node には `schedule: "once"`・`last_run_generation`・`last_run_frame` が付きます(まだ走っていなければ後の 2 つは `null`。第13章「`get_frame_plan` に足されたもの」)。

**拒否。**once の node が毎 frame 変わるものに触れないことを、graph の lowering(合成の前)で検査します([検査する所](../../src/project/rendergraphcomposition.cpp#L405))。

- 書く port(`write` / `read_write` / attachment)が中身を保たない resource に届くと `graph.once_writes_transient@1` `{node, resource}`(`resource` は image / buffer の node の id)。`lifetime` を書かない `render.image` と、`lifetime: "transient"` の buffer がこれに当たります。
- 読む port が次のものに届くと `graph.once_reads_per_frame@1` `{node, input}`(`input` は port の名前):
  - per_frame の pass の出力 port
  - `history: true` の読み
  - transient の image / buffer
  - once でない node が書く resource(persistent でも)
  - value graph が `time` / `delta_time` / `scene_time` の `value.input` から値を作る buffer(その buffer を `target` にする `render.scalar` から上流を辿る)
- `render.target_ref` の先は合成の後(解決の段)で合成した宣言の `lifetime` を見て、`persistent` でなければ、書きは `graph.once_writes_transient@1` `{node, resource: <target 名>}`、読みは `graph.once_reads_per_frame@1` です([解決の段の検査](../../src/project/rendergraphcomposition.cpp#L1515))。
- once の node が once の node の出力を読むのは許されます(h0 → twiddle)。per_frame の node が once の出力を読むのは今までどおりです。

**分かっている穴**(仕様書の記述。コードの読みで確かめた範囲)

- rendering 節の JSON に直接書いた once は、上の拒否の検査を通りません(検査は graph の lowering にだけあります)。
- ループの出力と carry の値を読む once は判定しません(resource を辿るのがループの出力と carry の印で止まります)。
- `view_select` でどの view にも合わなかった once の node も、走ったことになります(記録が残り、次の frame からは走りません)。

test: `featurecompose_test` の [「PS-1 schedule and lifetime reach the passes, the targets and the frame plan」](../../test/featurecompose_test.cpp#L9568)(compute task の once、ループの body の全回が once)と [「PS-1 a once node that writes a transient resource or reads one that changes every frame is refused by name」](../../test/featurecompose_test.cpp#L9631)、`frameplanner_test` の [「PS-1 a once node runs on a new generation, a request, a recreated output, other host bytes or an upstream run」](../../test/frameplanner_test.cpp#L2586)、`renderingsamplecount_test` の [「PS-1 a persistent target is kept out of runtime aliasing with persistent lifetime as the reason」](../../test/renderingsamplecount_test.cpp#L2089) と [「PS-1 a once pass is isolated in its own physical scope」](../../test/renderingsamplecount_test.cpp#L2159)、`engine_primitives_test` の [「PS-1 once passes run on the first frame and again only when asked, edited or upstream ran」](../../test/engine_primitives_test.cpp#L2335)(GPU。frame 1 で h0 と twiddle が走り、frame 2〜5 は記録されず画素は同じ、`rerun_once {node: "h0"}` で h0 と twiddle が走る、Play / Stop で走らない、文書の値を変えて組み直すと新しい世代で走り画素が変わる)。

### persistent の image を画像資産から始める —— `render.image` の `initial: {asset}`(✅IA-1)

`lifetime: "persistent"` の `render.image` は、画像資産(KTX2 / PNG など)の中身から始められます(`c6690eba`)。動機は GB-3 の LTC の面光源の LUT(64×64 の RGBA float)と、後の音の解析の焼き込み(1 次元 / 2 次元の float の画像)です(仕様書の記述)。

```json
{"id": "lut_float", "op": "render.image",
 "parameters": {"type": {...}, "format": "R32G32B32A32_SFLOAT", "lifetime": "persistent",
                "usage": ["SAMPLED", "TRANSFER_SRC"],
                "initial": {"asset": "assets/luts/ltc.ktx2"}},
 "outputs": ["resource"]}
```

(`engine_primitives_test` の IA-1 の graph の抜粋。`width` / `height` を書いていないので、64×64 は asset から採ります)

**書き方**

- `initial` は `{"asset": "<参照>"}` の 1 鍵だけで、値は空でない文字列です。ほかの形は `render.image initial must be {"asset": "<reference>"}: <node>`(code の無い `std::invalid_argument`。[`checkRenderImageInitial()`](../../src/project/graphpartition.cpp#L502))。参照は engine の path resolver が既存の file として解きます。test の書き方は project からの相対(`assets/luts/ltc.ktx2`)です。
- `lifetime: "persistent"` の image にだけ書けます。`lifetime` を書かない(既定の transient)・`"transient"` の image に書くと `graph.image_initial_requires_persistent@1` `{node}` です。
- `width` / `height` を省くと asset の大きさを採ります。engine が asset を読んだ後に、組み立てに使う文書の写しへ書き込みます(文書の file は書き換えません)。このとき `extent_scale` も書くと `graph.image_initial_extent_conflict@1` `{node}` です。`width` / `height` を書いたときは、その値を asset と比べます。
- `dimension` が `"2d"` でない・`array_layers` が 1 でない image は `render.image initial requires one 2d layer: <node>`、`graph.loop` の body の中の image は `render.image initial is not supported in a graph.loop body: <node>` です(どちらも code の無い `std::invalid_argument`)。
- asset も 2D・1 層・非圧縮でなければ `render.image initial asset must be one uncompressed 2d image: <asset>` です(BC の KTX2 はここで断られます)。

**lowering。**`initial` のある image の宣言に、lowering は `usage` の `TRANSFER_DST` を足し、`extent_scale` が無ければ `1.0` を入れます(render target の parser が求めるため。固定の `width` / `height` があればそちらが効きます)。`initial` は render target の宣言にそのまま残ります([IA-1 の lowering](../../src/project/rendergraphcomposition.cpp#L801))。compile は、`initial` のある target を frame の pass より前に中身があるものとして扱うので、先に書く pass が無くても pass が読めます([IA-1 で書き手の無い読みを通す所](../../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L903))。

**照らし合わせ。**世代を組む段で、公開の前に asset を読み、image と照らし合わせます([IA-1 の asset を読む段](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1100))。拡大縮小と format の変換はしません。合わなければ `graph.image_initial_mismatch@1` `{node, asset, what, expected: {width, height, format}, actual: {width, height, format}}` です。

| `what` | 起きる時 |
|---|---|
| `extent` | 書いた `width` / `height` が asset の大きさと違う(式で書いた値は比べず、その場合 `expected` の値は `null`) |
| `format` | sRGB を UNORM と同じに見た形(`R8G8B8A8_SRGB` → `R8G8B8A8_UNORM`)が違う。成分の数・bit 数・float かどうかの違いで、PNG を float の image に入れるのもこれ |
| `color_space` | 形は同じで、KTX2 の format の sRGB / UNORM が image の `format` と食い違う |

- PNG / JPG は 8 bit の RGBA(`R8G8B8A8_UNORM` の形)に decode され、色空間を宣言しない扱いです。`R8G8B8A8_UNORM` と `R8G8B8A8_SRGB` のどちらの image にも同じ bytes が入り、SRGB の image では GPU が linear に戻して読みます(GPU の test で確かめています)。
- image の `format` が文字列でない(`format_class` など)ときは、大きさだけを比べます(宣言の注記)。
- 例(test): 64×64 の `R32G32B32A32_SFLOAT` の KTX2 を `R16G16B16A16_SFLOAT` の image に書くと `{"node": "lut", "asset": "assets/lut.ktx2", "what": "format", "expected": {"width": 64, "height": 64, "format": "R16G16B16A16_SFLOAT"}, "actual": {"width": 64, "height": 64, "format": "R32G32B32A32_SFLOAT"}}`。

**copy する時。**image が作られた時と、runtime の世代の最初の frame に、その frame のどの pass よりも前(once の pass より前)に 1 回、asset の level 0 を copy します([`uploadImageInitialAssets()`](../../src/core/vkcore/renderer.cpp#L6618))。

- program と target ごとに、copy した `VkImage`・世代・読んだ asset を覚え、どれかが変わったときにまた copy します。
- copy は別の submission で、終わるまで待ちます。once の pass が同じ image に書けば、copy の上に書きます。
- copy の時点で target の大きさや format が asset に合わない(preflight の後に合成が変えた)ときは、warning `render.image initial asset <asset> does not fit target <name> (...); not copied` を出して copy しません。

**asset を変えた時**

- `initial` の asset は render pipeline の監視対象に入ります([IA-1 で監視対象に入れる所](../../src/core/vkcore/renderer.cpp#L317))。コードの注記では、書き換えると新しい世代で読み直し、その最初の frame で copy し直します。hot reload の道でこれを確かめる test はありません(未確認)。`--rpc` の下では watcher の変更は render pipeline に届きません(✅HW-1。[第3章](03_project_format.md) の「hot reload で監視するもの」)。
- ✅IA-2(`cdcdebc1`)から、`reload_render_documents` は文書の digest に加えて、live の世代が `initial` に使う asset の中身の sha256 も比べます。文書が同じでも、asset のどれかの中身が変わっていれば組み直して公開し、新しい世代の最初の frame で copy し直します(下の「asset だけを書き換えた reload(✅IA-2)」)。IA-2 の前は、asset だけを書き換えても `no_change` で読み直しませんでした。✅IA-3 から、`value.sample_table` が読む表と manifest も同じ比較に入ります(下の「表を書き換えた reload(✅IA-3)」)。
- 文書を書き換えて `reload_render_documents` で組み直すと、新しい世代で asset を読み直して copy します(GPU の test で確かめています)。

**asset だけを書き換えた reload(✅IA-2)。**

- **比べる相手**: 世代を組む段で asset を読むとき、decode の前に file の中身の sha256 を `assetFileSha256` で記録します([IA-2 で decode の前に hash を取る所](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1110))。decode の前に取るので、間に書き込まれても次の reload では「変わった」と読まれ、「変わっていない」とは読まれません(コードの注記)。`get_frame_plan` の `initial_asset.sha256`(manifest の値)とは別の値です。
- **比べ方**: `reload_render_documents` は live の世代の program が記録した asset を集め([live の世代の initial の asset を集める所](../../src/core/communication/editorruntimefactory.cpp#L2306))、各 file を読み直して hash し、記録と比べます([IA-2 で asset を比べる所](../../src/core/communication/renderconfigeditor.cpp#L6424))。今読めない file は「変わった」に数えます。
- **結果**: committed の結果には毎回 `changed_assets: [<asset>...]` が付きます。値は書いた参照のまま(`assets/luts/ltc.ktx2`)、辞書順、重複なしで、空のこともあります。文書も asset も変わらなければ今どおり `no_change: true` で、`changed_assets: []` です。asset だけが変わったときは `changed: []` で `changed_assets` にその asset が出て、`source_digest` は変わりません(editor の文書は変わらないため)。
- **文書の変更で新しく足した asset**: 比べるのは live の世代がすでに読んだ asset だけなので、文書を書き換えて新しく `initial` に足した asset は `changed_assets` には出ず、その文書が `changed` に出ます(コードの読み)。
- **合わない asset**: 書き換えた asset が image に合わないときは、ticket が受けられた(`accepted`)後、次の frame の組み直しの段で `graph.image_initial_mismatch@1` の rejected になり(asset を読むのは世代を組む段だけで、ticket を取る前の preflight は読みません。コードの読み)、live の世代は前のまま、画面も前の値のままです(GPU の test。64×64 の 1 成分の KTX2 を RGBA32F の image の asset に上書きして `node` / `asset` / `what: "format"` を見る)。CT-2 の規則で error に `document` も付くはずですが、この test は `document` を確かめていません(未確認)。
- test: `engine_primitives_test` の [「IA-2 reload_render_documents takes in an initial asset rewritten on disk with the documents unchanged」](../../test/engine_primitives_test.cpp#L2811)(GPU。何も変えない reload が `no_change`・`changed_assets: []`、LUT の中身だけを書き換えた reload が新しい世代で `changed: []`・`changed_assets: ["assets/luts/ltc.ktx2"]`・同じ `source_digest`・画面が新しい値、続く reload が `no_change`、合わない asset の拒否で画面が前の値のまま)。

**`get_frame_plan`。**`physical_target_plan.resources[]` のうち `initial` を持つ image の要素に `initial_asset: {asset, sha256}` が付きます([IA-1 で `initial_asset` を足す所](../../src/core/vkcore/renderer.cpp#L4796))。`asset` は書いた参照のまま、`sha256` はその file を持つ asset store の `assets.manifest` の値で、manifest に載っていなければ `null` です。object にしてあるのは、後の WP が焼いた元の情報を足せるようにするためです(コードの注記)。✅AB-1 から、asset の隣に `<asset>.import.json` があれば `derived_from: {source, tool: {name, version}}`・`stale`・`stale_reasons` も付きます(`list_derived_assets` と同じ値。[AB-1 で `initial_asset` に足す所](../../src/core/vkcore/renderer.cpp#L4813)。理由の一覧は [第5章](05_assets.md) §5.8「音の解析を焼いた表」)。

**assets。**manifest は store の中の file を全部数えるので、`initial` の asset もほかの file と同じく manifest と検証の対象です。engine に asset を消すパージはありません。

test: `featurecompose_test` の [「IA-1 a persistent image takes its initial asset's extent and is copied into」](../../test/featurecompose_test.cpp#L9786)(大きさの書き込み、lowering の `TRANSFER_DST`・`extent_scale`、PNG が UNORM と SRGB の両方に入る)と [「IA-1 an initial asset on a transient image, beside an extent scale, or that does not fit is refused by name」](../../test/featurecompose_test.cpp#L9820)(拒否の 3 つの code と `what` の 3 つ)、`engine_primitives_test` の [「IA-1 persistent images start from their KTX2 and PNG assets and per-frame passes read them」](../../test/engine_primitives_test.cpp#L2704)(GPU。float の LUT が bit どおり画面に届く、PNG の UNORM と SRGB の読みの違い、`get_frame_plan` の `initial_asset`、文書も書き換えた reload で新しい値が copy される)。asset だけを書き換えた reload は上の IA-2 の test です。

### 焼いた音の解析の表を読む —— `value.sample_table` と `pelican_sample_table.glsl`(✅AB-1)

`bake_audio_analysis` / `pelican_cli audio analyze` が焼いた表(R32_SFLOAT の KTX2 と、隣の `<表>.import.json`。形は [第5章](05_assets.md) §5.8「音の解析を焼いた表」)を、値の graph と shader から読みます(`892a1455`)。

**CPU —— `value.sample_table {asset, row, time?}`**

```json
{"id": "level", "op": "value.sample_table",
 "parameters": {"asset": "tables/tone.ktx2", "row": "rms"},
 "outputs": ["value"]}
```

- `asset`(kind `asset`。✅CT-4 から `asset_kind: "analysis_table"`・`extensions: [".ktx2"]`)と `row`(CT-3 では kind `json`。✅CT-4 から kind `union` で、`variants` は整数と文字列)は必須です(CT-3 の表。[AB-1 の parameter の行](../../src/project/graphparameterschema.cpp#L413))。`row` は行の番号(整数)か名前(`"rms"` / `"peak"` / `"band:3"`。manifest の `layout.features` の名前)です。
- 入力 `time` は任意です。edge を省くと、lowering が `scene_time` を読む入力の node(`<id>/$scene_time`)を足して、それを読みます。`audio.playback` の event と同じ時計です([AB-1 の lowering](../../src/core/userpublic/valuegraph.cpp#L216))。
- 値は、標本 `time / hop_seconds` の前後 2 つの線形補間で、最初より前と最後より後は端の値です(`time` が有限でなければ 0 秒として読みます)。
- 表は **graph を組む時に 1 回読みます**([AB-1 で表を読む所](../../src/core/userpublic/valuegraph.cpp#L474))。読むのは KTX2 と、manifest の `source.metadata.layout` だけです。asset は project が立っていれば path resolver が既存の file として解き、立っていなければ(tool や test)file の path として読みます。同じ file の表は、file と manifest の中身の sha256 が同じあいだ graph の間で共有されます([`loadSampleTable()`](../../src/core/loader/sampletable.cpp#L55))。✅IA-3(`26473e75`)の前は更新時刻と大きさで判定していたので、同じ大きさで時刻を戻した書き換えは古い表のまま使われました。IA-3 からは読んだ bytes を hash し、parse するのもその同じ bytes です([IA-3 の中身で判定する cache](../../src/core/loader/sampletable.cpp#L61))。
- 表は世代(graph)を組むときに読むので、**焼き直した表は、組み直した新しい世代で値の node に届きます**。✅IA-3 から `reload_render_documents` は表と隣の manifest の中身も比べるので、表だけを焼き直した後の reload で新しい世代になり、そこで届きます(下の「表を書き換えた reload」)。`asset` / `row` の変更は rebuild です([AB-1 の op の登録](../../src/project/graphmodule.cpp#L787))。`evaluation` は `baked` です。
- 拒否(どれも graph を組む時): 表・manifest・layout が読めない、または表が layout の形(`R32_SFLOAT` の 2D、`width` × `rows_per_feature × features の数`)でないと `value.sample_table_unreadable@1` `{node, asset, reason}`。表に無い行は `value.sample_table_row_unknown@1` `{node, asset, row, rows}`(`rows` は表の行の名前)です。`asset` / `row` の欠けは `value.sample_table requires an asset and a row: <id>` です。
- 例(test。手で書いた 2 行・3 標本・`hop_seconds` 0.5 の表、行 `a` = {0, 1, 4}、`b` = {10, 20, 30}): `a` を `time` 0.25 で読むと 0.5、0.75 で 2.5、-1 で 0、10 で 4。行 1 を 0.5 で読むと 20。`time` の edge の無い `b` は `scene_time` 0.25 で 15 です。

**表を書き換えた reload(✅IA-3、`26473e75`)。**上の「asset だけを書き換えた reload(✅IA-2)」と同じ口に、`value.sample_table` の表を足しました。

- **記録するもの**: 表を読むときに、表の KTX2 と隣の `<表>.import.json` の両方の中身の sha256 を `SampleTable` に入れます(`file_sha256` / `manifest_sha256`)。値の graph は、どの node がどの表を読んだかを [`Graph::sampleTables()`](../../src/core/userpublic/valuegraph.cpp#L813) で返します。
- **比べるもの**: `reload_render_documents` が比べる asset の一覧(依存の名前は IA-2 の `published_initial_assets` から `published_assets` に、型は `RenderConfigPublishedInitialAsset` から `RenderConfigPublishedAsset` に変わりました)に、live の世代の値の graph が読んだ表を 2 行(表と manifest)ずつ足します。どちらの行も表の参照の名前で入ります([IA-3 で表と manifest を足す所](../../src/core/communication/editorruntimefactory.cpp#L2310))。比べ方は IA-2 と同じで、参照ごとにまとめ、どちらかが違うか読めなければ変わったと数えます([IA-3 も通る asset の比較](../../src/core/communication/renderconfigeditor.cpp#L6431))。
- **結果**: `changed_assets` には表の参照(node の `asset` に書いた綴りのまま)が出ます。manifest だけが変わったときも、出るのは表の名前です。新しい世代は表を読み直すので、node の値は新しい表のものになります。何も変えなければ今どおり `no_change: true`・`changed_assets: []` です。
- **見張り**: hot reload の render pipeline の監視対象(`renderPipelineWatchSources()` の [IA-3 で表と manifest を監視に入れる所](../../src/core/vkcore/renderer.cpp#L323))にも表と manifest を足しました。対象は flat の program の値の graph が読んだ表です。`--rpc` の下では watcher の変更は render pipeline に届きません(✅HW-1。上の「asset を変えた時」と同じ)。hot reload の道でこれを確かめる test はありません(未確認)。
- test: `engine_primitives_test` の [「IA-3 reload_render_documents takes in a value.sample_table table rewritten on disk with the documents unchanged」](../../test/engine_primitives_test.cpp#L2935)(GPU。本番の RPC の経路。何も変えない reload が `no_change`、値を 3 から 9 に焼き直して更新時刻を戻した表の reload が `changed: []`・`changed_assets: ["assets/tables/level.ktx2"]`・新しい世代で、その世代の `get_graph_node_values` が 9、続く reload が `no_change`、表の bytes を変えずに manifest の `hop_seconds` だけを変えた reload も表の名前で新しい世代)。

**GPU —— `pelican_sample_table.glsl`**([include](../../src/core/resources/shaders/include/pelican_sample_table.glsl#L22))

```glsl
#include "pelican_sample_table.glsl"
float pelican_sample_table(sampler2D table, uint feature, uint rows_per_feature, uint sample_count,
                           float hop_seconds, float t);
```

- CPU と同じ読み方です。`feature` 行の `t` 秒の値を、`texelFetch` の 2 標本の線形補間で返します(filter はしない)。端の外は端の値、`sample_count` が 0 か `hop_seconds` が正でなければ 0 です。幅は texture 自身の幅を読みます。
- include のコメントが書く想定の使い方は、表を `render.image` の `initial: {asset}`(`lifetime: "persistent"`。上の「persistent の image を画像資産から始める」)で読み込み、`rows_per_feature` / `sample_count` / `hop_seconds` と時刻を `render.scalar` の vec4 で渡す形です。
- engine の include として登録されています([登録](../../src/core/loader/engineresources.cpp#L120))。fragment と compute の stage で compile できることは test で確かめていますが、**GPU で実際に描いて値を見る test はありません**(未確認)。

test: `ab1_audio_analysis_test` の [「AB-1 value.sample_table reads a row by name or index at its time edge, or at scene_time without one」](../../test/ab1_audio_analysis_test.cpp#L228)(上の例の値、`time` の edge が無いときの入力が `scene_time` だけであること、無い行・manifest の無い表の拒否。`ValueGraph::Graph` を直接組む test で、scene の graph の中で読む道は通していません)と、`shader_compiler_reflection_test` の [「AB-1 pelican_sample_table.glsl compiles where a fragment or compute stage reads a baked table」](../../test/shader_compiler_reflection_test.cpp#L957)。

### 段 —— expand と draw_loop(✅LP-1a / LP-3a)

ループがどこで実行されるかは `over` と body の中身で決まります([`classifyGraphLoopTier()`](../../src/project/graphcomposition.cpp#L409))。v0 では段が黙って変わることはなく、合わなければ断ります。

| `over` | body | 段 | 実行 |
|---|---|---|---|
| `range`、`view_axis` | `render.*` と `value.*` | **expand** | 組み直しのとき body を回の数だけ複写し、ふつうの pass・compute task・target として frame graph に渡す |
| `draw_items` | `draw.*` だけ | **draw_loop** | 1 つの material pass の中で、今の queue のまま、material ごとに分岐が決めた描き方で描く |

断るときは `graph.loop_tier_unsupported@1` `{node, reason}` です。

| `reason` | 場面 |
|---|---|
| `nested` | body に `graph.instance` がある(`graph.loop` がある body は、その前に `graph.loop_nested@1`) |
| `mixed_body` | body に `draw.*` と `render.*` / `value.*` が混ざる |
| `resource_in_draw_body` | draw_loop の body で `render.image` / `render.buffer` / `render.attribute` を作る |
| `carry_in_draw_loop` / `limit_in_draw_loop` | draw_loop に `carry` / `limit` を書いた |
| `set_unsupported` | v0 に無い集合、または実行時にしか長さが決まらない軸(`light`)の `view_axis` |
| `per_frame_value` | `range.count` を `{node, port}` の edge で書いた(毎フレーム変わる値) |

### expand の展開の規則(✅LP-1a / LP-1b / LP-Vb)

解決の段の [`expandRenderGraphLoops()`](../../src/project/rendergraphcomposition.cpp#L2192) が、ループを依存の順に 1 本ずつ展開します。

1. **要素**: `range` は 0〜count-1、`view_axis` は 0〜軸の長さ-1(長さは組み直し時の provider の値。cascade は `cascade_count`、face は 6)。`where` → `order` → `limit` の順で絞って並べ、回ごとに最初に合った分岐を選びます。
2. **上限**: 1 つのループの回は 64 まで([`graphLoopIterationLimit`](../../src/project/graphloop.hpp#L164))、graph 全体の pass と compute task は(ループの外の graph node を含めて)128 までで、超えると `graph.loop_expansion_limit@1` `{node, count, limit}`。
3. **名前と出所**: 複写した node は `<loop>/<element>/<node>`(要素の**値**で名付けるので、回数や順を変えても残る回の名前は変わらない)。body の中の `render.image` は回ごとに別の target になります。複写した pass・task・target・buffer には出所 `loop_origin: {loop, element, node}` が付き、`get_frame_plan` の node と resource にも載ります。✅GL-2a から、ループの回に限らず graph の node が作った pass と task には、`get_frame_plan` の node に作った node の `origin`(`{graph, node, definition?, instance_path?, loop?, loop_branch?, element?}`)も付きます。preset・feature の pass には付かないので、preset が `engine://` で `source` が同じ `"engine"` でも graph の pass を見分けられます(形は第13章「`get_frame_plan` に足されたもの」)。
4. **順序**: body の外の資源(`render.target_ref`、ループの入力、外の image や buffer、carry の値、他のループの回)に触る回は、読みだけの回も含めて、**要素の順に明示の `after` でつながれます**。展開器は名前を辞書順に並べる(`<loop>/10/x` は `<loop>/9/x` より前)ので、宣言順には頼りません。回ごとの `render.image` だけに触る回どうしはつながれません。
5. **ループ id での順序**: ループの `after` / `before` は全部の回に写ります。ほかの node の `after: ["<loop>"]` は**そのループの全部の回**(つまり最後の回の後)を意味し、回が 0 個なら依存から落ちます。
6. **置き場所**: ループの `insert` / `canonical_anchor` があれば、family ごとの展開列を 1 つの run としてそこへ置きます。無ければ上の「置き場所」の既定です。compute task の展開列は task の列の、そのループを待つ最初の task の前に入ります。
7. **出力を読む側**: ループの外で `p` を読む pass は最後の回の値を読み、それを書いた pass の後に動きます。`p[]` は edge の `element` の回です。

**展開名は書けません。**書いた文書が `<loop>/…` の形の名前を `after` / `before` / `insert`(graph の node と rendering 節の pass)、`frame_node_order`、`target_planning` の node で名指すと `graph.loop_expansion_referenced@1` `{name, pointer}` です([`requireLoopExpansionUnreferenced()`](../../src/project/graphcomposition.cpp#L1011))。ループ全体の順序はループの id で書きます。

**`view_axis` のループ(✅LP-Vb)。**`"over": {"view_axis": {"family": "$shadow/directional", "axis": "cascade"}}` の回数は組み直し時の軸の長さで、body の pass に `"view_select": {"cascade": {"$expr": "element"}}` と書いて回ごとの view を選びます。family に無い軸は `graph.view_select_axis_unknown@1` `{pass: <loop id>, family, axis}`、実行時の軸は `graph.loop_tier_unsupported@1` `{reason: "set_unsupported"}` です。

### draw_loop —— material ごとの描き方(✅LP-2 / LP-3a)

draw_loop は 1 つの `render.material_pass` の中で動くループです。要素は**その pass が描く material**で、分岐ごとに「その material をどう描くか」を `draw.*` の列で決めます。queue と並び順は今のままです。

```json
{"id": "opaque_draws", "op": "graph.loop",
 "parameters": {"over": {"draw_items": {"contract": "forward_opaque_v1"}},
                "where": {"tags": {"include": ["hero"]}},
                "order": {"by": "phase_default"},
                "branches": [{"when": {"tags": {"include": ["outline"]}}, "body": "outline_then_base"},
                             {"body": "base_only"}]},
 "outputs": ["draws"]}
```

- `over.draw_items.contract` は material pass の contract(`deferred_geometry_v1` / `forward_opaque_v1` / `forward_transparent_v1`)。`legacy_gbuffer_v1` は `graph.loop_contract_unsupported@1` `{node, contract}` です。
- `where` は `{"tags": {"include", "exclude", "require_all"}}` だけです。`include` / `exclude` は pass の `material_filter`(queue の tag filter)になり、`require_all` はループ自身の条件です(queue に残っても `require_all` を満たさない material はこの pass で描かれません)。pass に `material_filter` が既にあるなら、`include` / `exclude` が一致していなければ断られます。
- 分岐の `when` が見られるのは material 単位で組み直し時に決まる属性だけで、書いた key はすべて満たす必要があります(AND): `material`(照合の鍵。下)/ `tags` / `contract`(material の shader contract: `gbuffer_v1` / `forward_scene_color_v1` / `legacy_gbuffer_v1`)/ `skinned`(真偽)/ `has_variant`(真偽、または variant 名)。値は literal だけで、式は書けません。
- ループの出力 `draws`(型 `pelican.render.draw_stream@1`)は、**ちょうど 1 つ**の `render.material_pass` の `draws` 入力につなぎます。0 本・2 本以上・別の port は `graph.loop_draws_unconnected@1` `{node, count}`。ループの contract と pass の `material_contract` が違えば断られます([`requireDrawLoopsConnected()`](../../src/project/graphcomposition.cpp#L1090))。
- draw_loop 自身は置き場所も `after` / `before` も持てません(pass の方に書く)。ほかの node が `after` にループの id を書くと、その material pass を指したことになります。

**描き方の列(`draw.*`)。**body の出力 `draws` から `after` の edge を逆にたどった列が、1 つの material の描き方です(先頭から順に描く)。

| node | parameters | 描き方 |
|---|---|---|
| `draw.material` | なし | 今と同じ(その material 自身の pipeline) |
| `draw.variant` | `variant`(必須) | 名前付き variant で描く。variant を持たない material に当たると `graph.loop_variant_missing@1` `{node, material, material_variant}` |
| `draw.override` | `render_state`(`blend`: `opaque`/`blend`/`additive`、`cull`: `none`/`front`/`back`、`depth_test`、`depth_write`、`depth_compare`)と `defines`(文字列配列)。少なくとも 1 つを書く | 同じ material を、render state と define を替えた pipeline で描く |

- `draw.*` を置いた definition はループの body 専用で、`graph.instance` から使うと `graph.definition_role@1` `{definition, node}`。ループの外に `draw.*` を置くことも断られます。`draw.override` で `depth_test: false` と `depth_write: true` を同時に書くと断られます([`checkDrawStepParameters()`](../../src/project/graphmodule.cpp#L190))。
- 分岐の body が `null` の material は、この pass では描かれません(意図して描かないので「受けた」と数えます)。`over` と `where` の中なのに、選ばれた分岐の body が何も描かない material は `material_route_unserved@1` に加わって断られます。
- **override は新しい material ではありません。**base の pipeline の記述を複写し、render state と define だけを替えた pipeline を**世代が所有**する表で作り、base の descriptor set と GPU の material index で描きます。値の編集と値だけの reload は 2 回目の描画にも同じように効き、surface の reload や組み直しで base の pipeline が作り直されると override も追従します([`MaterialDrawLoopTable`](../../src/core/material/materialdrawlooptable.hpp#L42))。
- override の拒否(世代を準備するとき): blend の変更で opaque と transparent の phase をまたぐと `graph.loop_phase_crossing@1` `{node, material}`、pipeline layout(set layout の binding と stage、push constant)が base と違うと `graph.loop_override_interface@1` `{node, material, binding}`、`defines` があるのに runtime shader compiler の無い build では `graph.loop_override_needs_compiler@1` `{node}`、define の差し替えの compile が 1 世代で 16 を超えると `graph.loop_override_compile_limit@1` `{node, count, limit}`([`materialDrawOverrideCompileLimit`](../../src/project/materialdrawloop.hpp#L57)。LP-2 の実測で 1 組約 210 ms)。define で使う資源が変わる override は、compiler が使われない binding を消すので、多くが `override_interface` で断られます。
- **material の照合の鍵**(`when.material` と、`get_frame_plan` の要素): project の material は `<material 文書>#<名前>`、glTF の material は名前に依らず `<asset>#material[<index>]`、それ以外は登録名です([`materialDrawSubjectKey()`](../../src/project/materialdrawloop.cpp#L17))。
- 後から登録された material(glTF の読み込み、spawn、variant)も、生きている世代の規則で評価されます。
- **既知の制限**: draw_loop で描かなかった material も、影と velocity の pass には描かれます(この 2 つは queue 全体を描く)。

**一体版との関係。**`draws` をつながない `render.material_pass` と、rendering 節の今の material pass は、「else の 1 分岐で body が `draw.material`(`material_variant` があれば `draw.variant`)の draw_loop」と同じ描き方です。hybrid の golden の case で、queue の byte・draw の列・画素の一致を確かめています。rendering 節の material pass をこの形へ書き換える RPC が `decompose_material_pass` です([第13章](13_editor.md))。

### feature の pass を置き換える —— `pass_replacements`(✅LP-0d / LP-Vb)

feature の instance は object 形で `pass_replacements` を持てます。値は graph の pass・compute task・ループの id です。

```json
"features": [
  {"ref": "engine://features/shadow_directional.json",
   "pass_replacements": {"shadow_depth": "shadow_cascades"}}
]
```

- 置き換えた pass・task は feature から外れ、置き換え先(ループなら展開した回の列)がその位置に入ります。`after` / `before`・`snapshot_after`・`frame_node_order`・`surface_resources.producer` の名前も置き換え先へ写ります(`after` は最後の回、`before` は最初の回)。置き換えた名前は `target_planning` から消えます([`applyRenderFeaturePassReplacements()`](../../src/project/rendergraphcomposition.cpp#L2804))。
- feature の consumer の pass(影を受ける material pass など)を置き換えたときは、その名前が `material_consumers` / `fullscreen_consumers` から外れ、ループの展開の後で consumer がもう一度選ばれます(✅LP-Vb)。置き換え先の material pass も影の map などを受け取ります。
- 置き換え先は、置き換えた pass が書いていたものを書かなければなりません(ループならどれかの回)。書かなければ `surface_resources` の producer の検査と同じ「no longer writes」の拒否です。
- feature の pass・task でない名前は `render_feature.pass_replacement_unknown@1` `{feature, pass}`、graph の pass・task・ループでない置き換え先は `graph.node_missing@1` `{node, feature, pass}` です。置き換え先の family にその pass の family の pass が無ければ `graph.insert_family_mismatch@1`。draw_loop の id は書けません(その `render.material_pass` の id を書く)。
- 置き換え先の node には、`get_frame_plan` で `replaces: [{feature, pass}]` が付きます(✅GL-2a。ループなら置き換えた pass と同じ family の回に)。

**変種で外れる feature への依存。**preview / XR の変種で feature が外れると、その feature に依存する graph の node は同じ変種で**非活性**になります(変種の組み直し全体は落ちません。[`withoutExcludedGraphDependents()`](../../src/project/rendergraphcomposition.cpp#L2622))。理由([`RenderGraphDeactivatedNode`](../../src/project/renderloopreport.hpp#L59))は `target_ref`(その feature の target を指す)/ `insert`(その feature の pass か非活性の graph pass を anchor にする)/ `order`(`after` / `before` がその pass を指す)/ `pass_replacement`(その pass を置き換える)/ `loop_output`(非活性のループの出力を読む、または carry する)です。合成結果の `deactivated_graph_nodes` と、diagnostic の kind `graph_node_deactivated`(detail は `<variant>:<reason>:<feature>`)に出ます(✅GL-2a から `get_frame_plan` の最上位に `deactivated_graph_nodes` と `diagnostics` として出ます)。draw_loop は、その material pass と一緒に非活性になります。

### 出荷する分解版 —— `engine://graphs/*.json`(✅LP-1c / LP-1d / LP-Vb)

標準 feature の分解版を、engine 付属の graph 文書として出荷します。engine はこれを**読みません** —— project が写して使う雛形です。各文書は `decomposes` を持ちます。

| 文書 | 中身 | `decomposes` |
|---|---|---|
| [bloom_loop.json](../../src/core/resources/graphs/bloom_loop.json) | threshold(`canonical_anchor: post_ldr`)、4 回の下り段ループ(carry、回ごとの `1 / pow2(element + 1)` の大きさの image)、3 回の降順の上り段ループ(回ごとの入力で下り段の同じ番号へ加算)、composite。2 本のループは `insert: {before: bloom_composite}` | `feature: engine://features/bloom.json`、`remove_feature: true`、`shader_defines`(threshold と knee) |
| [planar_mip_chain_loop.json](../../src/core/resources/graphs/planar_mip_chain_loop.json) | `mip_levels_of("planar_reflection_color") - 1` 回の compute ループ。回ごとに `subresource.mip` を `element` → `element + 1` | `feature: engine://features/planar_reflection.json`、`pass_replacements`(`planar_reflection_filter_mip_1`〜`_6`) |
| [cube_capture_loop.json](../../src/core/resources/graphs/cube_capture_loop.json) | `$capture/cube` の `face` のループ。body は feature の 8 pass(material pass 3、snapshot_copy 2 ほか)で、各 pass が `view_select: {face: element}` | `feature: engine://features/cube_capture.json`、`pass_replacements`(feature の 8 pass → ループ) |
| [shadow_cascade_loop.json](../../src/core/resources/graphs/shadow_cascade_loop.json) | `$shadow/directional` の `cascade` のループ。body は `render.shadow_depth` 1 本 | `feature: engine://features/shadow_directional.json`、`pass_replacements`(`shadow_depth` → ループ) |

**project への入れ方**([test/lp1c_decompose_support.hpp](../../test/lp1c_decompose_support.hpp) の手順): (1) 雛形の `definitions` と `nodes` を自分の graph へ写す(名前が衝突したら入れない)、(2) `remove_feature: true` なら `rendering.features` からその feature を外す、(3) `pass_replacements` があれば、その feature の instance に書く、(4) `shader_defines` があれば `rendering.shader_defines` に足す。(1) は `edit_graph_document` の `add_definition` / `add_node` で送れますが、(2)〜(4) を書く専用の RPC はありません。

一致の確認: bloom は 9 target・display・frame plan が一体版と byte 単位で一致し(LP-1d から engine の shader のまま)、planar は全 mip・両 layer の画素と frame plan、cube は face ごとの AO の byte・face の中の invocation の順・影の付いた色、cascade は光源 1 と 2 で layer ごとの depth・culling の数・画素が一致します。cube の分解版は**実行の順が変わります**(一体版は pass が外側、分解版は face が外側)。

### 名前付きの拒否の一覧(LP 系)

「編集」は `edit_graph_document` などの編集 RPC がその場で断るもの(読み込みでも同じ)、「組み直し」は解決の段か pipeline の準備で断るものです。組み直しで回ごとの node(`<loop>/<element>/<node>`)が名指されたときは、error_fields に `loop_origin: {loop, element, node}` が付きます。

| code | 時 | error_fields |
|---|---|---|
| `graph.target_ref_unknown@1` | 組み直し | `{node, name}` |
| `graph.target_ref_type_mismatch@1` | 組み直し | `{node, name, field: kind｜topology, declared, actual, expected: {type, layers?}}`(`expected` は ✅FR-1。上の「既存の target を指す」) |
| `feature.required_target_missing@1` | 組み直し | `{feature, target, type, document}`(✅FR-1。§6.5「feature が pipeline に求める target と挿入先」) |
| `feature.insertion_anchor_missing@1` | 組み直し | `{feature, anchor, candidates, document}`(✅FR-2。同上) |
| `feature.required_target_unwritten@1` | 組み直し(合成の終わり) | `{feature, target, type, writer_candidates: [{node?, pass, port, resource}], document}`(✅FR-3。§6.5 の「書き手」。FR-3 の前は frame planner の `Pass input target is not produced as an earlier output: <target> in pass: <pass>`) |
| `snapshot.after_unknown@1` | 組み直し | `{snapshot, after, resolved?, candidates}`(✅FR-1。§6.8「`after` に書ける名前」) |
| `value.sample_table_unreadable@1` / `value.sample_table_row_unknown@1` | 値の graph を組む時 | `{node, asset, reason}` / `{node, asset, row, rows}`(✅AB-1。上の「焼いた音の解析の表を読む」) |
| `graph.history_write@1` | 編集 | `{node, port}` |
| `graph.history_unavailable@1` | 組み直し | `{node, port, target}` |
| `graph.insert_anchor_unknown@1` | 編集(`canonical_anchor`)/ 組み直し(`insert`) | `{node, anchor}` |
| `graph.insert_family_mismatch@1` | 組み直し | `{node, anchor}` |
| `graph.insert_on_compute@1` | 編集 | `{node}` |
| `graph.input_index_invalid@1` | 編集 | `{node, input_index, reason, ports}` |
| `graph.render_node_parameters_missing@1` | 編集 | `{node, op, missing}`(✅RS-9。✅RS-15 で value / audio / ui の op にも。表の下) |
| `graph.port_field_missing@1` | 編集 | `{node, port, field}`(✅CT-1。表の下) |
| `graph.render_node_parameter_invalid@1` | 編集 | `{node, op, parameter, expected, reason: type｜enum｜range, allowed? ｜ min? / max?}`(✅CT-3。表の下) |
| `graph.op_requires_owned_instance@1` | 編集(読み込みでも同じ。graph の partition) | `{node, op}`(✅CT-4。catalog の `placement` が `owned_instance` の op(`pose.*`・`value.morph_weight`)を、`owner` 付きの `graph.instance` の外に置いた。§6.17 の CT-4 の段落) |
| `graph.render_scalar_target_incompatible@1` | 編集・読み込み(graph の中の buffer は graph の partition)/ 組み直し(graph の外の buffer は登録の段) | `{node?, target, expected: {host_source, size?}, actual}`(✅CT-5。`render.scalar` の書き込み先が `runtime_f32` でない、または大きさが sink の合計と違う。登録の段では `node` が付かない。§6.5 の CT-5 の項) |
| `shader.loop_push_overlap@1` | 組み直し(pipeline を作る時) | `{shader, stage, range: {offset, size}}`(✅LC-1。上の「回の番号を shader に渡す」) |
| `graph.once_writes_transient@1` | lowering(合成の前)/ 組み直し(`render.target_ref` の先) | `{node, resource}`(✅PS-1。上の「一度だけ走る pass と、frame をまたぐ image」) |
| `graph.once_reads_per_frame@1` | lowering(合成の前)/ 組み直し(`render.target_ref` の先) | `{node, input}`(✅PS-1。同上) |
| `graph.image_initial_requires_persistent@1` | lowering(合成の前)/ 組み直し(asset を読む段) | `{node}`(✅IA-1。上の「persistent の image を画像資産から始める」) |
| `graph.image_initial_extent_conflict@1` | lowering(合成の前)/ 組み直し(asset を読む段) | `{node}`(✅IA-1。同上) |
| `graph.image_initial_mismatch@1` | 組み直し(asset を読む段) | `{node, asset, what: extent｜format｜color_space, expected, actual}`(✅IA-1。同上) |
| `graph.view_select_axis_unknown@1` | 組み直し | `{pass, family, axis}`(長さの外は `value`・`length` も) |
| `graph.view_select_runtime_axis@1` | 組み直し | `{pass, family, axis}` |
| `graph.view_select_primary_family@1` | 組み直し | `{pass, family}` |
| `graph.view_select_needs_target_plan@1` | 実行時(target plan の無い経路) | `{pass}` |
| `graph.loop_branch_else_missing@1` | 編集 | `{node}` |
| `graph.loop_branch_interface@1` | 編集 | `{node, branch, definition, port}` |
| `graph.loop_nested@1` | 編集 | `{node, definition}` |
| `graph.loop_order_unsupported@1` | 編集 | `{node, order}` |
| `graph.loop_tier_unsupported@1` | 編集 / 組み直し(`view_axis` の実行時の軸) | `{node, reason}` |
| `graph.loop_tier_mismatch@1` | 編集 | `{node, tier, actual}` |
| `graph.loop_expression_invalid@1` | 編集 / 組み直し | `{node, pointer, reason}` |
| `graph.loop_carry_type_mismatch@1` | 編集 | `{node, from, to}` |
| `graph.loop_element_unknown@1` | 組み直し | `{node, port, element}` |
| `graph.loop_output_empty@1` | 組み直し | `{node, port}` |
| `graph.loop_expansion_limit@1` | 組み直し | `{node, count, limit}` |
| `graph.loop_expansion_referenced@1` | 編集 | `{name, pointer}` |
| `graph.loop_contract_unsupported@1` | 編集 | `{node, contract}` |
| `graph.loop_draws_unconnected@1` | 編集 | `{node, count}` |
| `graph.definition_role@1` | 編集 | `{definition, node}` |
| `graph.loop_variant_missing@1` | 組み直し(世代の準備) | `{node, material, material_variant}`(error の最上位の `variant` は graph の変種だけを指すので、material の変種は `material_variant`) |
| `graph.loop_phase_crossing@1` | 組み直し(世代の準備) | `{node, material}` |
| `graph.loop_override_interface@1` | 組み直し(世代の準備) | `{node, material, binding}` |
| `graph.loop_override_needs_compiler@1` | 組み直し(世代の準備) | `{node}` |
| `graph.loop_override_compile_limit@1` | 組み直し(世代の準備) | `{node, count, limit}` |
| `render_feature.pass_replacement_unknown@1` | 組み直し | `{feature, pass}` |
| `graph.node_missing@1`(置き換え先) | 組み直し | `{node, feature, pass}` |
| `graph.definition_duplicate@1` / `graph.definition_missing@1` / `graph.definition_referenced@1` | 編集(definition の op) | [第13章](13_editor.md) |

- `graph.loop_feature_pending@1` は LP-1a の間だけ使った予約の code で、今は出ません。
- ✅FR-1 / FR-2 / FR-3 の `feature.*` と `snapshot.*` は `graph.` で始まらないので、CT-2 の `document` / `definition` は付きません。`feature.*` の `document` は feature 文書の参照です。
- ✅RS-9(`5d065436`)。render の node の `parameters` に op が要るものが無いと、`graph.render_node_parameters_missing@1` `{node, op, missing}` で断ります([op ごとの表 `renderNodeRequiredParameters()`(CT-3 からは parameter の表を読む)](../../src/project/graphmodule.cpp#L262)、[検査](../../src/project/graphmodule.cpp#L269)。✅RS-15 からは [`GraphModuleRegistry::resolveOperation()`](../../src/project/graphmodule.cpp#L676) が契約を作る前に 1 か所で呼びます([呼ぶ所](../../src/project/graphmodule.cpp#L685)))。`missing` は欠けた名前を表の順に並べます。必須は `render.fullscreen`: `ports`・`shader` / `render.raster`: `ports`・`shader`・`draw` / `render.compute`: `ports`・(`shader` か `ray_tracing`。どちらも無ければ `missing` には `shader` と出る)/ `render.material_pass`: `material_contract` / `render.snapshot_copy`: `snapshot_after` / `render.target_ref`: `name`・`type` / `render.image`: `type` / `render.buffer`: `element` / `render.attribute`: `element`・`domain` / `render.shadow_depth`: 無し(`engine://shadow_depth` で depth の socket に描く)です。値が `null` の鍵は無いものとして数え、`parameters` が無い node と `null` の node は空の object として扱います(コードの注記では、Studio の `set_node_parameters` は空の parameters を書かないため。parameters の無い `render.shadow_depth` はそのまま組めます)。値そのものの検査は今までどおり読む所で行います。RS-9 の前は code の無い例外の文で断られ、parameters の無い `render.shadow_depth` も `render node parameters must be an object` で断られていました。test: `featurecompose_test` の [「RS-9 a render node without the parameters its op requires is refused by name」](../../test/featurecompose_test.cpp#L9141)。
- ✅RS-15(`7b16e9c8`)。同じ表と同じ code を、render の op 以外で値を `at()` で読んでいた op にも広げました。palette から置いたばかりの node(parameters が空)が、JSON の library の `key 'target' not found` ではなく名前で断られます。足した必須は `render.scalar`: `target` / `value.input`: `name` / `value.constant`: `value` / `value.parameter`: `name`・`default` / `value.morph_weight`: `node`・`target` / `value.curve`: `keys` / `audio.playback`: `asset` / `audio.volume`・`audio.pitch`: (`target` か `voice`) / `ui.value`・`ui.visible`: `node` で、`render.attribute` に(`count` か `source`)が加わりました。名前は parameters の鍵のほか、**同名の入力 edge** でも満たせます(コードの注記の例は、`voice` の edge で書く `audio.volume` と、`source` から数を取る `render.attribute`)。検査は op の契約を作る前の [`resolveOperation()`](../../src/project/graphmodule.cpp#L676) の 1 か所で、宣言だけの契約の op にも factory の op にも同じに効きます。同じ表は ✅CT-1 から catalog の `required_parameters` にも出ます(§6.17「catalog の読み方」)。test: `featurecompose_test` の [「RS-15 render.scalar, value.constant and value.input without their parameters are refused by name」](../../test/featurecompose_test.cpp#L9199)。
- ✅CT-1(`ac8d836b`)。graph の文書で、利用者が書く所の鍵が欠けているときも名前で断ります(CT-1 の前は JSON の library の `key 'element' not found` の例外で、起動時の読み込みでは engine の process が落ちていました。仕様書の記述)。
  - **port の欠け**: `render.fullscreen` / `render.raster` / `render.compute` の `parameters.ports.<port>` に要るものが無いと `graph.port_field_missing@1` `{node, port, field}` です。`field` は `access`(どの port も)、`element`(buffer の port)、`type` / `type.semantic` / `type.arguments`(image の port)、`location`(`attachment: "color"` の port)のどれかです([port の検査](../../src/project/graphmodule.cpp#L313)、[code を作る所](../../src/project/graphmodule.cpp#L304))。port の宣言が object でない、`access` / `kind` / `binding` / `attachment` が文字列でない、型の値の形が違う、は code の無い `std::invalid_argument` です。
  - **resource の node の欠け**: `render.buffer` / `render.attribute` の `element`、`render.image` / `render.target_ref` の `type` の中の `semantic` / `arguments` の欠けは、RS-9 の `graph.render_node_parameters_missing@1` で、`missing` にその名前(`type.semantic` など)が 1 つ入ります([作る所](../../src/project/graphmodule.cpp#L295))。
  - **graph の展開の欠け**: 文書や definition の `nodes` / `outputs`、node の `id` / `op`、`graph.instance` の `definition`、edge の `node` / `port` が無いと、場所を名指す `std::invalid_argument`(`graph definition <名前> requires nodes`、`graph node <id> requires op` など。[作る所](../../src/project/graphcomposition.cpp#L32))。node の `outputs` が無いときは、出力の無い node として `unknown graph output port: <id>.<port>` になります。`render.raster` の `draw` の `vertex_source` / `index_source` が pass の port でない、または edge の来ない port を名指すと `render.raster draw <鍵> names no port of this pass` / `names a port no edge feeds`。
  - **どこで断られるか**: 編集の RPC では rejected で、engine は動き続けます(test で確かめています)。`reload_render_documents` では `render_pipeline_preflight_failed` の message の中に code が入ります。起動時の読み込みでは今も致命の扱いで engine は終了し、その致命の行に code が出るようになっただけです(この 2 つは実装者の報告で、test はありません)。
  - test: `renderconfigeditor_test` の [「CT-1 a render port or parameter field the document lacks is refused by name」](../../test/renderconfigeditor_test.cpp#L1319)。
- ✅CT-2(`51d1c2d8`)。`graph.` で始まる code の名前付きの拒否の全部に、node を書いた文書 `document` と、definition の body の node なら `definition` が付きます。既存の field はそのままで、足すだけです。
  - **`document`** は rendering の root 文書の参照で、`project://<path>` の形です(project.json の `basic_config.rendering_config_json` が `passes/main.json` なら `project://passes/main.json`)。graph の node を持つのは root 文書だけで、preset と authored pass は pass だけを持つ feature です(graph の template は root に写されます。コードの注記)。なので、どの node でも `document` は root 文書です。
  - **`definition`** は、拒否された node を body に持つ definition の名前です。展開名(`<instance>/<node>`、ループの `<loop>/$<branch>/<node>` / `<loop>/<element>/<node>`)を root の文書で辿るか、編集の `scope` が definition を名指していればその名前です。root の node には付きません。
  - 拒否がもう同じ名前の field を持っていれば、その値を残します(`graph.loop_nested@1` の `definition` など、code 自身の意味の field)。分からない field は出しません(`null` は入れません)。`graph.` 以外の code には付けません。
  - 載る所: 起動時の致命の行、編集の ticket の rejected の error(field と message の両方)、`reload_render_documents` の message。例: `Pelican fatal error : graph.port_field_missing@1 {"document":"project://passes/main.json","field":"element","node":"sim","port":"data"}`(仕様書の例)。下の項の `<code>: <JSON>` の形で運ばれる code(`view_select` の 4 つと draw_loop の 5 つ)にも同じ規則で付けます([CT-2 で renderer が構成を読む所で足す所](../../src/core/vkcore/renderer_config.cpp#L625))。
  - 付かないもの(仕様書の記述。CT-2 の差分はこれらの経路を変えていません): `edit_graph_instances` の `invalid_graph_attachment`、scene の読み直しでの graph の拒否、`pelican_cli` の出力。
    - ✅CT-5(`65f3a8a3`)から、`edit_graph_instances` は graph の名前付きの拒否(`GraphRegistryError`)を `invalid_graph_attachment` に畳まず、その code と field のまま CT-2 の `document`(と `definition`)を付けて返します。判定に使う root は edit を当てた後のものです([CT-5 で edit_graph_instances が code を保つ所](../../src/core/communication/renderconfigeditor.cpp#L2910))。`invalid_graph_attachment` に残るのは名前の無い拒否だけです。例(test。root にもうある id の instance を attach): `{code: "graph.node_duplicate@1", node: "left", document: "project://passes/main.json"}`。`graph.op_requires_owned_instance@1` に `edit_graph_document` と `edit_batch` の道でも `document` と `definition` が付くことも、同じ test が見ます([「CT-5 an owned-instance op refused inside a definition names its document on every edit path」](../../test/renderconfigeditor_test.cpp#L1587))。
  - `get_frame_plan` の node の `origin.graph`(✅GL-2a)は、今も `project://` の付かない参照のままで、`document` と書き方が揃っていません。
  - test が確かめているのは編集の ticket の道(root の node、definition の body の node、RS-9 の code)と、展開名から definition を辿る規則です。起動時の致命の行と reload の message は、コードの読みと実装者の報告で、test はありません。
  - test: `renderconfigeditor_test` の [「CT-2 a graph refusal names the document and the definition its node was written in」](../../test/renderconfigeditor_test.cpp#L1387) と [「CT-2 a refused node id is traced to the definition that holds it」](../../test/renderconfigeditor_test.cpp#L1692)。
- ✅CT-3(`0d215a68`)。§6.17「各 op の parameter の形」の表が kind を付けた値を、op の parser が読む前に名前で断ります。
  - **検査する所**: [CT-3 の `checkGraphNodeParameterTypes()`](../../src/project/graphparameterschema.cpp#L860) です。[`resolveOperation()` が RS-9 の検査の直後に呼びます](../../src/project/graphmodule.cpp#L687)。なので RS-9 と同じく、op の契約を作る所の全部で効きます。必須のものが欠けていれば、RS-9 の `graph.render_node_parameters_missing@1` が先です。
  - **code と field**: `graph.render_node_parameter_invalid@1` `{node, op, parameter, expected, reason}` です。`expected` は表の kind です。`reason` は `type`(kind が違う)/ `enum`(enum の外。`allowed` に取れる値)/ `range`(範囲の外。表にある方の端を `min` / `max` に)のどれかです。`graph.` の code なので、CT-2 の `document`(と `definition`)も付きます。
  - **どれを名指すか**: 表の順に見て、最初に合わない 1 つです。入れ子は道筋で名指します。配列の要素は `[<番号>]`、object の member は `.<鍵>` です(例: `value.curve` の `keys[0].interpolation`)。
  - **kind ごとに通る値**([CT-3 の kind ごとの判定](../../src/project/graphparameterschema.cpp#L675)):
    - `number` は数、`integer` は整数、`bool` は真偽値、`string` / `asset` / `node_ref` は文字列、`enum` は `enum` の中の文字列です。
    - `vec2` / `vec3` / `vec4` / `color` は、2 / 3 / 4 / 4 個の数の配列です(要素は式の object でもよい)。
    - `array` は配列で、`items` があれば要素ごとに同じ規則で見ます。
    - `object` は object です。`fields` は書いてある member だけを見て、`items` があれば全部の member を見ます。
    - `shader` は、`fields` があれば object、無ければ文字列です。
    - `expression` / `json` と、✅CT-4 の `union` は見ません。
  - **断らないもの**: JSON の `null`、`$expr` / `$format` の式の object(書いた所が `expression: true` かどうかに関わらず。書ける所かどうかは LP-1b の検査です)、表に無い鍵(今までどおり各 op の parser に任せます)。`fields` の中の `required` も見ません。
  - **kind を書いても検査しないもの**: `pose.clip_sample` の `root_node` です。rig に無い node は、今までどおり op が `pose.root_node_unknown@1` で断ります。`canonical_anchor` と `value.input` の `name` は文字列かどうかだけを見ます。名前が正しいかは、今までどおり `graph.insert_anchor_unknown@1` と `unbound frame graph input: <名前>` が断ります。
  - **名前付きに変わった拒否**(CT-3 で書き換えた test の期待): `audio.playback` の `bus` の外の値(前は `unknown audio bus`)、`value.playback` の `duration` に文字列、`schedule` の enum の外(前は `schedule must be per_frame or once`)、`render.image` の `lifetime` の enum の外(前は `lifetime must be persistent or transient`)と、object でない `initial`(前は `render.image initial must be ...`)、`pose.clip_sample` の `loop` / `speed` の型違いと `root_motion` の enum の外。graph の pass の置き場所で `insert` / `canonical_anchor` の型が違うときは、この code か今までの `std::invalid_argument` のどちらかです(test はどちらも受けます)。
  - test: `curve_test` の §6.17 の CT-3 の test(型・enum・範囲・入れ子の道筋、式と `null` と表に無い鍵を断らないこと)と、`renderconfigeditor_test` の [「CT-3 an edit with a parameter of the wrong kind, enum or range is refused by name」](../../test/renderconfigeditor_test.cpp#L1446)(編集の ticket の rejected に code・field・`document: "project://passes/main.json"` が載り、文書は書かれないこと)。
- `graph.loop_*` の draw_loop の 5 つ(`variant_missing` 以下)と `view_select` の 4 つは、例外の文として `<code>: <JSON の fields>` の形で運ばれ、編集 RPC の結果では**その code のまま**、fields を `ticket` と `participant` の横に並べて返ります(message の頭は `<code>: <JSON>` のまま)。draw_loop の 5 つがこの形になったのは engine の `992225fb` からで、それより前は `render_pipeline_preflight_failed` の message に code が埋もれ、fields がありませんでした([`isCarriedRefusalCode()`](../../src/core/communication/renderconfigeditor.cpp#L77))。変種(preview / xr)の preflight で断られたときも同じ形で、横に `variant` が付きます(第13章「変種と組み直しの拒否の形」)。✅PV-1(`a173e70c`)から編集の preflight は preview を組まないので、今 `variant` が付くのは xr だけです(§6.13 の PV-1 の段落)。
- web は `graph.loop` と LP 系の op を読みません(`graph.operation_unregistered@1`)。`view_select` と `pass_replacements` を web の parser が名前付きで断る件は WEB-1(別 repo)の範囲で、この repo では確かめていません(未確認)。

## 関連文書

- [../v0_freeze.md](../v0_freeze.md) — v0 の凍結一覧。§1.3 が共通グラフの module / op / 型 / sink の表の正本(§6.17)、§1.6 が [rendering config の root と 2 つの装着様式](../v0_freeze.md#16-rendering-config-の-root-と-2-つの装着様式)
- [../design_graph_composition.md](../design_graph_composition.md) — 共通グラフの合成(instance の入力 parameter、装着編集)の設計の正
- [../design_graph_loops.md](../design_graph_loops.md) — 共通グラフのループ(`graph.loop`、`render.target_ref`、置き場所、`pass_replacements`、`view_select`、draw_loop)の設計の正。v3、LP 系で実装済み(§6.18)
- [../shader_contract.md](../shader_contract.md) — シェーダ契約(set 規約・FrameUBO・.surface フック・snapshots)の正
- [../design_color_pipeline.md](../design_color_pipeline.md) — カラーパイプライン(v4・実装済み)
- [../design_shader_freedom_kit.md](../design_shader_freedom_kit.md) — シェーダ基盤 [SF]。実装済み
- [../design_render_feature_modules.md](../design_render_feature_modules.md) — feature 合成(実装済み)
- [../design_compute_task_graph.md](../design_compute_task_graph.md) — 統一フレームグラフ(WP64 で実行一本化済み)
- [../design_headless_rendering.md](../design_headless_rendering.md) — ヘッドレス描画 [HL]。実装済み
- [../design_material_shading.md](../design_material_shading.md) — マテリアル設計(v1.2。M2a〜M3.5 実装済み、現行契約の正は shader_contract.md)
- [../design_postprocess_temporal.md](../design_postprocess_temporal.md) — history/velocity(実装済み)+ ポストスタック(一部 📐)
- [../design_taa_jitter.md](../design_taa_jitter.md) — TAA + projection jitter(v2.1・J1/J1b/J1c + 標準 TAA すべて実装済み)
- [../design_usd_openpbr.md](../design_usd_openpbr.md) — USD レーン + OpenPBR(v2.1・M-PBR0/U-USD0 実装済み)
- [../openpbr_1_1_1_mapping.md](../openpbr_1_1_1_mapping.md) — OpenPBR 写像表の正
- [../design_openxr.md](../design_openxr.md) — OpenXR(v2.2・XR0〜XR4 + WP203a〜c local implementation済み。desktop mirror WSI lifecycleは**WP215〜217で実装済み**(RDP / ディスプレイ切替などの manual platform gate と XR 実機 gate は未消化)。現実装のSimulator/物理HMD・対象GPU実測gateは未)
- [../design_render_pipeline_extensibility.md](../design_render_pipeline_extensibility.md) — レンダーパイプライン拡張境界(draw sort / pass implementation / subgraph replacement / graph transform / render strategy の設計の正)
- [../design_render_graph_compiler.md](../design_render_graph_compiler.md) — 論理型カーネル・target planning・Vulkan physical plan・generic raster pass の設計の正
- [../design_heterogeneous_execution_graph.md](../design_heterogeneous_execution_graph.md) — 共通実行計画(FrameExecutionPlan)と NativeScope 境界の設計の正
- [../design_wsi_epoch_recovery.md](../design_wsi_epoch_recovery.md) — window 出力の lifecycle(surface / swapchain epoch。WP215〜217 実装済み)
- [../design_2d_game_layer.md](../design_2d_game_layer.md) — 2D ゲーム層(v2.4・S2D 実装済み)
- [../design_asset_hot_reload.md](../design_asset_hot_reload.md) — アセットホットリロード(v2.1・HR0〜HR2-G 実装済み)
- [../design_debug_profiling.md](../design_debug_profiling.md) — デバッグ・プロファイリング(v1.1・条件付き受理。D-P0a/D-P1a/D-P2a/D-P2b 実装済み、§6.14)
- [第5章 アセット](05_assets.md) / [第9章 Web プロファイル](09_web.md) / [第10章 ツールリファレンス](10_tools.md)
