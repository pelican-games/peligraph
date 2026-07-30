# 第6章 レンダリング

対象: pelican2(2026-07-21 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- レンダリングパイプラインを **JSON だけ**で定義する方法(rendering config)
- パス種別(`material` / `fullscreen` / `output_transform` / `ui` / `shadow_depth` / `velocity` / `debug_draw` / `debug_text`)と compute タスク
- カラーパイプライン(SRGB スワップチェーン + リニアワークフロー)で気をつけること
- feature(1 行で有効化できるパージ可能な GPU 機能)— canonical anchor・パラメータ・named binding
- フレームグラフ(依存宣言 → 機械最適化 → 手詰め)とプランダンプ
- シェーダ基盤 — stem 参照・FrameUBO・ホットリロード・descriptor set 規約
- マテリアル(`.surface` + `pelican.material` + **OpenPBR**)、テンポラル(history / velocity / **projection jitter / 標準 TAA**)、2D スプライト
- OpenXR ステレオレンダリング(論理フレーム / feature policy / ミラー)
- グラフ variant(flat / XR / preview)と、GPU デバッグラベル・GPU タイミング・VRAM の見方

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

`basic_config.default_rendering_pass` が rendering config 内のどのパス列(`rendering_passes[].name`)を使うかを決めます。エンジンは常にパージ不能な終端 `output_transform` ノード(リニア → 表示エンコードの 1 箇所)を生成し、**このノードが終端に来ないパス列を既定に指定すると起動時エラー**です(§6.3)。なお authored な `"swapchain"` 出力は合成時に `display` RT へ置換されるため、`display` を誰も書かない構成はフレームグラフ検証で落ちます(§6.11)。

## 6.2 rendering config のスキーマ

実物の最小構成([../../projects/example/passes/example_renderingpass_data.json](../../projects/example/passes/example_renderingpass_data.json) — G-buffer → ライティング → UI)は前版と同じ骨格で動きます。example が実際に使う [main_rendering_config.json](../../projects/example/passes/main_rendering_config.json) は SSAO と 4 段ブルームを足した 17 パス構成です(ファイルに書かれた authored 件数。実行時はこれに ui feature のパスと終端 `output_transform` が合成されます)。

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

名前の重複(RT・パス・バッファ)はすべて hard error です。

### render_targets

| フィールド | 必須 | 内容 |
|---|---|---|
| `name` | ✔ | `"swapchain"` は予約名(書くとエラー) |
| `extent_scale` | ✔ | ウィンドウ(swapchain)サイズ × scale。> 0 |
| `width` / `height` | 任意(両方セット) | 固定サイズ RT(シャドウマップ等)。片方だけはエラー |
| `format` | ✔ | 45 種。8/16/32-bit の `R` / `RG` / `RGBA` に対する UNORM・SNORM・UINT・SINT・SFLOAT、sRGB、10/11-bit packed、depth を受理する。完全な正は [`renderingpassjsonhelpers.cpp`](../../src/core/renderingpass/renderingpassjsonhelpers.cpp) の `format_names` |
| `format_candidates` | 任意 string 配列 | verified Vulkan physical fragment が選択してよい追加 format。`format` は常に自動/default候補として先頭へ補われ、重複は除去される。宣言しただけでは自動formatは変わらない |
| `format_class` | 任意 | `scene` / `display` / `data` / `explicit(<FORMAT>)`。カラーリゾルバが実 format を決める(§6.3)✅WP73 |
| `role` | 任意 | `color` / `data`。SRGB view / UNORM view の選択 |
| `history` | 任意 bool | `true` で 2 面持ち。前フレーム面は `<名前>@history` で読める(§6.8)✅WP88 |
| `clear_color` | 任意 | history RT の初期化色 |
| `dimension` | 任意 | `2d`(既定) / `cube`。cubeは固定かつ正方形の`width`/`height`と6 layersが必要 |
| `mip_levels` | 任意 | 正整数または `"full"`。省略時1。`"full"`は実extentから最大mip chainを作り、resize時に再計算する |
| `layers` | 任意 | 正整数。2Dは省略時1でarray容量を表し、XRの内部view数とはphysical loweringで最大値を取る。cubeは省略時6、明示時も6固定 |
| `usage` | ✔ | `COLOR_ATTACHMENT` / `DEPTH_STENCIL_ATTACHMENT` / `SAMPLED` / `STORAGE` / `TRANSFER_DST` / `TRANSFER_SRC` |

runtime cubemapは資源形状だけを宣言し、六面をViewFamilyの6 viewとはみなしません。

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

| フィールド | 必須 | 既定 | 内容 |
|---|---|---|---|
| `name` | ✔ | — | パス列内で一意 |
| `type` | ✔ | — | `material` / `fullscreen` / `output_transform` / `ui` / `shadow_depth` / `velocity` / `debug_draw` / `debug_text`(+ ImGui ビルド時 `imgui`) |
| `output` | ✔ | — | `color`(null / attachment / attachment配列)と `depth`(null / attachment)の**両キー必須**。attachmentは従来の名前または`{target, subresource}`。`"swapchain"` は color のみ |
| `input` | 任意 | — | **`fullscreen` / `output_transform` 限定**(ほかの type に書くと `Only fullscreen passes support input targets`)。読み込む RT / バッファ名で、RT には **`@history` サフィックス**可(history RT のみ) |
| `resource_ports` | 任意 | — | **`fullscreen` 限定**。`input` の画像を logical name、sampled access、shared/per-view/cube view、filter/address、mip/layer subresourceで注釈し、generated shader accessorを作る。依存edgeは増やさない |
| `material_resources` | 任意 | — | **`material` 限定**。`.surface` のtyped buffer/image portをframe-graph resourceへ割り当てる。resource、history、view、sampling、mip/layer subresource、read footprintから依存とbarrierを導出する |
| `color_load_op` / `color_store_op` | 任意 | `Clear` / `Store`(ui のみ load 既定) | `Clear` / `Load` / `DontCare` |
| `depth_load_op` / `depth_store_op` | 任意 | `Clear` / `DontCare` | シャドウマップでは `depth_store_op: "store"` を明示 |
| `clear_color` | 任意 | `[0,0,0,1]` | 4 要素固定 |
| `clear_colors` | 任意 | — | `output.color` の RT 名をキーにした attachment 別 clear。未指定の attachment は `clear_color` を使う。UINT/SINT RT では整数範囲・整数値を起動時検証 |
| `after` / `before` | 任意 | — | フレームグラフの明示エッジ(§6.6) |

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

主な検証(すべて起動時の名指しエラー): input の RT に `SAMPLED` usage が必要 / 1 パスの全出力 attachment は同一実サイズ / 同一 RT の入出力同時使用は不可 / input に書いた RT は先行パスが出力していること。

### type 別の要点

- **`material`** — シーン内のモデルを描く material パス。`material_outputs` を省略した従来設定は、既定の 5 枚 G-buffer（または forward の scene color 1 枚）をそのまま使います。明示した場合は**順序・枚数・数値型を任意に定義**でき、`material_output_states`でfield別のblend/write-maskも指定できます。固定のエンジン上限はなく、実行デバイスの `maxColorAttachments` と各 format/sample capability が物理上限です。詳しくは §6.7。
- **`fullscreen`** — 全画面 1 枚描き。`shader: { "vertex": <stem>, "fragment": <stem> }` **必須**。`input` の画像は通常 `resource_ports` で名前を付け、fragment shaderからgenerated `pelican_sample_<port>()`で読む(最大 8 入力)。1つのinput resourceへ複数portは割り当てず、mip/layerを変える場合も1 portの`subresource`で選ぶ。raw shaderだけは従来どおりset 1へ配列順でbindする。`uses_light_data: true` と `push_constants`(`"none"` / `"camera_position"` / `"projection_view"`)は**互換キー**として受理されますが、GPU への実際の供給元は常に set 0 の FrameUBO / LightUBO です(§6.4)。
- **`output_transform`** — リニア → 表示エンコードの終端ノード。**自動付加されるため通常は書きません**(§6.3)。
- **`velocity`** — モーションベクタ出力(§6.8)。フィールドは `shader.{vertex, skinned_vertex, fragment}`(既定 `engine://velocity` / `engine://velocity_skinned`)。通常は feature 経由。
- **`ui`** — UI オーバーレイ([第7章](07_input_ui.md))。WP87 以降は `engine://features/ui.json` 経由の挿入が標準です。
- **`shadow_depth`** — depth 専用パス。`shader` 省略時は `engine://shadow_depth`。
- **`debug_draw` / `debug_text` / `imgui`** — 通常は feature 経由で挿入されます(§6.5)。

なお `sprite` という**パス型はありません** — スプライトは feature + anchor 方式で描かれます(§6.9)。

## 6.3 カラーパイプライン(✅WP72〜74)

2026-07-11 に **SRGB スワップチェーン + リニアワークフロー**へ全面移行しました(設計の正: [../design_color_pipeline.md](../design_color_pipeline.md) v4)。

> **設計決定(リニアワークフロー):** 作業空間は linear-sRGB(Rec.709/D65)。**シェーダの入出力はすべてリニア**で、sRGB の encode/decode はハードウェア(`*_SRGB` view)か、終端の `output_transform` ノードの 1 箇所だけが行う。transfer が掛かるのは RGB のみ(alpha は常にリニア straight)。手書きの `pow(x, 2.2)` 等はエンジンシェーダから全廃済み。

ユーザーが気をつけること:

1. **色テクスチャは sRGB で作る**(baseColor / emissive → SRGB view で自動デコード)。normal / metallicRoughness / データテクスチャはリニア(UNORM view)。`.surface` の custom texture は `color_space: srgb` 宣言で SRGB になります(§6.7)。
2. **シェーダに gamma 補正を書かない**。書くと二重変換になります。
3. glTF の `baseColorFactor` / `emissiveFactor` / `COLOR_0` は**リニア値のままエンジンに入ります**(仕様どおり)。C++ 側でデバッグ色などを直書きするときは `Pelican::srgb(r,g,b)` ヘルパ([color.hpp](../../src/core/userpublic/color.hpp))。
4. RT の `format_class` を使うと実 format はリゾルバが決めます: `scene` → HDR 時 `R16G16B16A16_SFLOAT` / SDR 時 `B8G8R8A8_SRGB`、`display` → `B8G8R8A8_SRGB`、`data` / `explicit(...)` → 宣言した `format` をそのまま使う(リゾルバは触りません — depth や float16 の data RT も普通にあります)。
5. スワップチェーン・ヘッドレス出力とも既定 SRGB。SRGB 非対応デバイスのみ UNORM フォールバック(`output_transform` シェーダが OETF を適用)。capture / golden の PNG は「encoded-sRGB + リニア straight alpha」契約です(`test/golden/README.md`)。

移行の台帳(何をどの根拠で変えたか)は [../color_migration_manifest.json](../../docs/color_migration_manifest.json)(WP72 監査の機械可読成果物)にあります。`get_status` の `color` オブジェクトで実行時の契約(swapchain format / readback encoding 等)を確認できます。

## 6.4 シェーダ基盤(Shader Freedom Kit ✅WP10〜15/19 + M2b/WP70)

### stem 参照

シェーダは**拡張子なしの stem** で参照します(例: `"shaders/lava"`、`"engine://ssao"`)。`.vert` / `.frag` / `.spv` などの拡張子を書くと「use an extensionless <stage> shader stem」エラーになります(WP63)。

解決順: ① `<stem>.vert`(GLSL ソース → 実行時コンパイル)→ ② `<stem>.vert.spv`(ビルド済み SPIR-V)。どちらも無ければ試行パス一覧付きエラー。

> **設計決定(二層供給):** 開発時 = GLSL 実行時コンパイル(defines によるバリアント可)、配布 = `.spv` 固定 1 バリアント。`.spv` に `shader_defines` を組み合わせると「Shader defines require GLSL source」エラー。feature を使う config は実行時コンパイラ必須。この stem 規約は web プロファイルと共有される可搬形式です([第9章](09_web.md))。

### 自動リフレクションとキャッシュ

SPIR-V を spirv-reflect で解析し、**descriptor set layout・pipeline layout・push constant range を全自動生成**します。頂点+フラグメントで binding の型が食い違うと起動時エラー。パイプラインキャッシュ(`pipeline_cache.bin`)に加え、✅WP82 で **SPIR-V ディスクキャッシュ**(`<project>/.pelican/shader_cache/`、SHA-256 キー)が入り、2 回目以降の起動が大幅に短縮されます(破損時は再コンパイルへフォールバック)。

### ホットリロード(✅WP14 → WP108 で全面刷新)

ウィンドウモードで実行中にシェーダや `.surface` を保存すると、**単一の FileWatcher 経路**([src/core/watch/](../../src/core/watch))が検出し、**アトミックなトランザクション**で反映します: 1 変更に影響する全 define/pass バリアント・リフレクション・依存パイプライン・`.surface` の vert/frag ペア・マテリアル layout/values をすべて準備してからフレーム境界で一括公開。どれか 1 つでも失敗すれば**全体を破棄して旧世代を維持**し、WARNING を出すだけです(旧来の「1 秒ポーリング」は廃止)。

テクスチャ(WP100)・マテリアル値(WP105)・モデルコンテナ .glb/.gltf/.vrm(WP110 — 配置済みインスタンスごと一括再構築)もリロード対象です(全体像は [第10章](10_tools.md) §10.7)。リプレイ / `--strict-assets` / rpc 駆動中はゲートにより無効、headless では自動ポーリングしません。状態は `get_status` の `reload.*` で見えます。

### descriptor set 規約(shader_contract.md が正)

GLSL からは `#include "pelican_sets.glsl"` / `#include "pelican_frame.glsl"` で定数と構造体を共有します。

| set | 定数 | 用途(✅WP70 で統一済み) |
|---|---|---|
| 0 | `PELICAN_SET_FRAME` | **全パイプライン共通の固定 layout**: binding 0 `FrameUBO` / 1 `ObjectBuffer` SSBO / 2 `LightUBO` / 3 `PreviousObjectBuffer`(velocity 用) / 4 `FrameResolutionUBO`。graphics/computeの両方でbindされ、シェーダが宣言する場合は一致必須 |
| 1 | `PELICAN_SET_PASS_INPUT` | パス入力。fullscreen/compute/materialの通常経路はlogical nameからgenerated bindingへ解決。raw set 1は低レベルescape hatch |
| 2 | `PELICAN_SET_MATERIAL` | binding 0-3 標準 PBR テクスチャ、4-5 VAT、**6 = 全マテリアル配列の `MaterialBuffer` SSBO**(標準 96B + custom values 256B / 要素)、7 以降 = `.surface` の custom texture(宣言順) |
| 3 | `PELICAN_SET_FREE` | 名前に反して**大半はエンジンが所有**します: binding 0/1 = debug_draw / debug_text、2/3 = スキンパレット(現 / 前フレーム)、4-8 = morph(instance / weight / previous weight / metadata / delta)、9-11 = per-instance マテリアルオーバーライド(§6.8) |

`FrameUBO`(全シェーダから読める・368B): `time` / `dt` / 64bit `frame_index` / 64bit `view_family_token` / `resolution` + 逆数 / `camera_position` / `view` / `projection` / `previous_view` / `previous_projection` / `jitter_ndc` / `previous_jitter_ndc` / `temporal_reset_epoch` / `previous_temporal_reset_epoch` / `view_index` / `view_count` / `clip_plane`。GLSL上は`frame_index.xy`が論理frame番号、`.zw`がstable family tokenで、`pelican_view_family_token()`から読めます。projection jitter が無効な既定構成では jitter は `(0, 0)` です。

push constant は 128B(エンジン 64B + シェーダ 64B)で、✅**リフレクション段階で enforcement 済み**です(4B align・128B 上限・エンジン領域の部分使用をパイプライン作成前に拒否)。エンジン領域 offset 0..63 の `engineMvp` は**名前に反して MVP ではなく `view_projection_jittered`(world → jittered clip)**です — 独自 vertex シェーダは model 行列で作った world position にこれを 1 回掛けるだけにし、**jitter を後から足してはいけません**(§6.8 の projection jitter が二重適用になります)。この意味論・64B の範囲・symbol 名は v1 で凍結です([../shader_contract.md](../shader_contract.md) の「`engineMvp` v1 の規範意味論」)。エンジン頂点レイアウトは location 0=`inPos`, 1=`inNormal`, 2=`inTexUV`, 3=`inColor`, 4=`inTangent`。

エンジン同梱シェーダ(`engine://`、抜粋): `fullscreen` / `ssao` / `ssao_blur` / `bloom_*` / `tonemap` / **`output_transform`** / `shadow_depth` / `skinned` / `skinned_shadow_depth` / **`velocity` / `velocity_skinned`** / **`taa_resolve` / `taa_composite`** / **`sprite`** / `debug_draw` / `debug_text` / `default` / `vat` / `ui`、マテリアルテンプレート `shaders/material/surface_v1.{vert,frag}` + `standard_lighting.glsl` / `toon_lighting.glsl` / **`openpbr_lighting.glsl`**、**OpenPBR wrapper 6 種 `engine://surfaces/openpbr/*.surface`**、公開 include 群 `shaders/include/pelican_*.glsl`。

🚧 HLSL / Slang: `.surface` の `language` フィールドとして形式上は受理されますが、既定バックエンドは GLSL 以外を reject します(他言語は spv-link experimental の視野 — §6.7)。

## 6.5 feature — パージ可能な GPU 機能(✅WP28〜31/54/87/88/104/226)

> **設計決定(参照 = 存在):** feature は rendering config に**参照を書いたときだけ**存在する。1 つも書かなければ合成機構ごと素通りし、挙動は完全に不変(パージ可能)。golden テストの「feature off = 既存出力の完全維持」で保証されている。

> **設計決定(feature 層 = ユーザー空間、2026-07-12):** エンジンが持つのは**機構語彙**(anchor / history / snapshot / format_class など — 版付きで additive にのみ増える)だけ。feature(JSON + シェーダ)は**ユーザー空間**で、同梱 feature は**特権なしの標準ライブラリ**にすぎない。`engine://features/*.json` をプロジェクトへコピーして改造したら自分のもの、が公式ワークフロー。canonical anchor の全順序・`output_transform` 等の常設ノード・色 invariant・決定性ゲートだけは名前入りエラーで防衛される。

使い方は 1 行です:

```json
{ "features": [ "engine://features/shadow_directional.json", "engine://features/velocity.json" ], ... }
```

エンジン同梱 feature(11 個・✅すべて実装済み):

| feature | 内容 |
|---|---|
| `hdr.json` | `format_class: scene` の RT を float16 化(切替はエンジンの色リゾルバが feature の有無で行う)し、`scene_ldr_in` を挟んでトーンマップパスを `after:tonemap` アンカーに挿入 |
| `clustered_lighting.json` | computeでViewFamily/view別のcluster index/list bufferを構築し、standard lighting passへtyped buffer resourceとして注入。planar reflection併用時はreflection-local selectorも自動合成 |
| `shadow_directional.json` | 既定2048×2048・1 cascadeのdirectional shadow。1〜8 cascade、解像度、距離、split、安定化をパラメータ化し、`shadow_depth` と受光入力を追加 |
| `planar_reflection.json` | 指定world planeでmain viewを反転し、独立解像度のdeferred G-buffer/SSAO/lightingを`$reflection/planar` familyへ追加。結果をforward transparentの`planar_reflection` resource portへ割り当て |
| `ui.json` | UI の GPU quad 描画(✅WP87。[第7章](07_input_ui.md)) |
| `velocity.json` | モーションベクタ RT(`R16G16_SFLOAT`)+ `velocity` パスを `before:post_main` に挿入(✅WP88) |
| `taa.json` | **標準 TAA**(resolve + composite の二パス + halton23/8 の jitter provider + スカラーパラメータ。§6.8)✅WP113 |
| `sprite.json` | **純ゲート**(RT もパスも足さない)。参照すると `__anchor_sprite` でスプライトが描かれる(§6.9) |
| `debug_draw.json` | ワイヤフレームオーバーレイ(collider 可視化など) |
| `debug_text.json` | ビットマップ文字 HUD([第7章](07_input_ui.md)) |
| `gpu_timing.json` | パスなしの計測フラグ。フレームグラフの**ノードごとに `barriers` / `body` の GPU タイムスタンプ**を取り、ログ・`get_status.gpu_timing`・ImGui に出す(✅WP29/143。XR では左右眼とミラーを別 view として分離。§6.14) |

fragment(`pelican.render_feature` v1)に書けるもの: `render_targets` / `buffers` / `compute_tasks`(追加。名前衝突はエラー。RT には `format_class` / `role` / `history` / `format_candidates` も書ける)、`render_target_overrides`(既存 RT の format/usage 上書きと `format_candidates` の重複なし追記)、`passes`(`insert: "before:<アンカー|パス名>" | "after:<...>" | "end"`)、`pass_overrides`(既存パスへの input / material resource追加)、`shader_defines`、**`parameters`(下記)**、**`projection_jitter`(§6.8)**、**`integrations`(下記)**。`shadow_directional.json` の全文例は前版と同じです。

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

- feature 側は `parameters`(`pelican.render_feature_parameters` v1)で宣言します: `render_targets`(name / required / default / role / format_class / usage — **RT の named binding**)、`scalars`(`float` / `int` / `bool`。`default` 必須、float/int は `range: [min,max]` 必須)、`shader_assets`(name / `stage: vertex|fragment|compute` / default)。
- feature 本文の中では **`$<パラメータ名>`** プレースホルダで参照します(`$scene_color@history` も可)。未解決・型不適合は feature 名・パラメータ名入りの compose エラー。
- `shader_assets`は`shader`の型付きslotだけへ完全一致の`$name`として置けます。compute taskの文字列`shader`、またはgraphics passの`shader.vertex|skinned_vertex|fragment|compute`と宣言stageが一致しなければcompose時に拒否します。`skinned_vertex`は`vertex` stageです。instance値は`engine://`にも`project://`にもでき、feature JSONを複製せずalgorithm assetだけを交換できます。
- スカラーは **`PELICAN_FEATURE_<FEATURE名>_<PARAM名>=<値>`** の値付き define に lower され、graphics/fullscreen/computeの全shader recipeへ渡ります(float は 9 桁 round-trip 表記。値を変えるとシェーダキャッシュキーも変わる = 正しく再コンパイル)。
- 解決結果は frame plan の `feature_instances` に出ます(`--dump-frame-plan` / Plan Viewer で確認可能)。

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
main cameraと鏡映cameraで奥行き順が異なってもmain-view順序を流用しません。

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

### canonical anchor(✅WP73)

挿入先には、パス名のほかに**標準アンカー**が使えます。feature 合成は次の 8 アンカーを `__anchor_<name>` ノードとして常時実体化します:

```
sprite → post_main → tonemap → post_ldr → pelican_ui → debug_draw → debug_text → imgui
```

アンカーの全順序はエンジンの契約で、並べ替えはできません(挿すのは自由)。かつての「同梱 hdr が `before:present` を参照していて example で起動エラーになる」問題は、この標準化で解消済みです。

シェーダ側の合流は従来どおり `#ifdef` バリアント方式(`PELICAN_FEATURE_SHADOW` 等)。feature を差し替えたいときは fragment をプロジェクト内にコピーして参照を書き換えます。

## 6.6 フレームグラフ(✅F0〜F2 = WP33〜35 + WP64)

> **設計決定(三層モデル):**
> 1. **正しさ** — データ依存(reads/writes)と明示エッジ(after/before)**だけ**が順序の契約。手書きバリアは禁止。
> 2. **機械最適化** — トポロジカルソート・層別・バリア導出はエンジンの仕事。
> 3. **手詰め** — JSON の宣言順は**安定タイブレーク**。依存が許す範囲で宣言順が尊重される。

依存はパスの `input` / `output`(+ `color_load_op: "load"` は reads 扱い、`@history` はフレーム内依存を作らない)から自動導出されます。writes-writes 競合は起動時 hard error です。

✅WP64 で**実行系はプラン駆動に一本化**されました。旧レガシー経路(compute を含まない構成だけ JSON 宣言順で実行)は撤去済みで、全構成が FramePlan の順序で実行され、純 render 構成でも `before` / `after` 明示エッジが効きます。プランのノード種別は `render` / `compute` / `anchor` / `snapshot_copy` / `output_transform` です。

### compute タスク

compute シェーダは `<stem>.comp`、`reads` / `writes` の宣言だけでframe graphへ並びます。
画像の通常経路はoptional `resource_ports`で、read-only imageは`sampled`、write imageは
`storage`として名前を付けます。shaderは
`#include "pelican_resource_ports.glsl"`から`pelican_sample_<port>()` /
`pelican_store_<port>()`を使い、set/binding番号を書きません。computeでも
`#include "pelican_frame.glsl"`からtime/camera/resolution/lightを読めます。
portは既存の`reads` / `writes`を注釈するだけで、新しいedgeや順序を作りません。

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

### プランダンプ(実行計画の可視化)

```sh
pelican_player --project mygame --headless --dump-frame-plan   # stderr に出力
# または rpc の get_frame_plan。GUI では ImGui の Plan Viewer(第10章)
```

出力にはノードごとの `order` / `level` / `reads` / `writes`(`@history` 読みは `reads_history`)と導出された `barriers`、`snapshot_copy` ノードが含まれます。

### target planning と Vulkan plan pin

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

- profile は `optimized`、`conservative_debug`、`hazard_stress`。`seed` は `hazard_stress` だけに指定できます。
- node/resource 制約は graph ごとです。`main` と書けば XR variant のコンパイル時には自動的に `main#xr` へ対応し、個々の node/resource 名は変わりません。
- `serial` / `isolate` / `no_alias` は禁止側だけを明示する設計です。通常 node に `parallel_safe` のような boilerplate は要りません。
- 未知 graph/node/resource、重複した warning ID、型の違うフラグは runtime object を作る前に compile error になります。

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

さらに、現在の自動計画が選んだ Vulkan backend candidate を固定したい場合は、まず pin なしで起動し、`get_frame_plan.physical_target_plan.ejectable_pin_package` をそのままコピーします。コピー先は variant 別の `vulkan_plan_pins` 配列です。

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

貼り付け後は `physical_target_plan.applied_pin_package` に同じ package が出ます。論理 graph が変わった package は `pin package is stale`、現在の device/provider で成立しない candidate は `pinned backend candidate is infeasible` として失敗し、別 candidate へ黙って fallback しません。render strategy の ABI fingerprint から `target_planning` / `vulkan_plan_pins` / `vulkan_physical_fragments` は分離されているため、これらを貼ったこと自体では logical fingerprint は変わりません。

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

eject結果は全resource/scope/alias groupを含みますが、手書きpackageの`resources`は変更するresourceだけに減らせます。`scopes`または`alias_groups`自体を省略すると、その部分は自動planを維持します。`alias_groups: []`はaliasなしの明示指定です。`scopes`は全nodeのexact partitionなので、空配列が有効なのはnodeを持たないgraphだけです。

別formatを選ぶ場合は、先にrender target側で許可する集合を宣言します。

```json
{
  "name": "low_color",
  "format": "R8G8B8A8_UNORM",
  "format_candidates": ["R16G16B16A16_SFLOAT"],
  "usage": ["COLOR_ATTACHMENT", "SAMPLED"]
}
```

その後、自動planからejectしたfragmentの該当resourceを、eject結果と同じcanonical表記
（例: `"R16G16B16A16Sfloat"`）へ変更します。render target JSONのformat名をfragmentへ
推測で転記せず、必ず現在の`ejectable_physical_fragment`を編集してください。
link時には、対象deviceのrequired image usage（historyなら`TRANSFER_DST`も含む）、
sample count、array layer上限、external depthなら`TRANSFER_SRC`契約を再照合します。
候補未宣言、非`materialized_image`、device非対応の変更はfallbackせずrejectされます。

全versionで共通して許可する編集は次です。

- resource representationの維持、または自動`tile_local_attachment` / `transient_attachment`から`materialized_image`への保守的な変更
- 自動planが選んだformatの保持、または`format_candidates`で宣言され、実device capabilityを満たす`materialized_image` formatへの変更
- format、representation、sample count、view layout、extentが一致し、論理lifetimeが重ならないresourceだけのalias group
- v2以降では`(node, logical_resource)`で特定したraster attachmentの`load_op` / `store_op`。
  `load_op`は`load|clear|discard`、`store_op`は`store|discard`で、未記述fieldは自動planを維持する

scope編集の意味はversionごとに異なります。

- v1/v2、またはv3の`scope_edit_mode: "split_only"`は、自動scopeをさらに分ける
  exact ordered partitionだけを許可します。tile-local値が新しい境界をまたぐ場合は、その
  resourceを先にmaterializeします。
- v3の`scope_edit_mode: "dependency_safe"`は、data edgeと明示`after` / `before`をすべて
  保つ範囲でnode/scopeを並べ替えられます。宣言順はcorrectness根拠にしませんが、
  feature compositionが通常pass列へ追加した明示`after`も消せません。
- 並べ替え後はresource lifetimeを新しい物理順序から再計算し、手書きalias groupをその
  lifetimeへ再照合します。
- 異なる自動scopeの融合は、内部`materialized_image`だけを使うsingle-sample rendering
  scopeに限定します。kind、view実行、ordered color/depth attachmentが一致し、2番目以降が
  Load、最後以外がStoreでなければrejectされます。成立したscopeは一つのVulkan dynamic
  rendering instanceとして実行されます。swapchain、UI/ImGui、MSAA、異なるattachment集合、
  sampled/storage/transfer依存を含む融合はまだ受理しません。

貼り付け後は`applied_physical_fragment`で適用内容を再観測できます。logical graphだけでなく、変更前の自動plan、target facts、選択候補、provider generationも`automatic_plan_fingerprint`へ束縛されます。いずれかが変わった古いfragmentはstale errorとなり、部分的に推測して修復しません。

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

- ヘッダ語彙: `language` / 順序付き `params[]`(型 `float|vec2|vec3|vec4|int|color`、**`default` 必須**、min/max/hint 任意)/ `textures[]`(`name` / `default` / `color_space: srgb|linear` が**必須** — SRGB/UNORM view を決めるのは `color_space` で、`role: color|data` は任意の整合チェック)/ `screen_inputs[]`(§6.8)/ `resource_ports[]`(下記)/ `render_state`。網羅例は `test/fixtures/surface_format/valid/full.surface`。
- **フック梯子 v1(凍結)**: `pelican_vertex_displace_v1` / `pelican_surface_v1` / `pelican_brdf_v1` / `pelican_ambient_v1` / `pelican_lighting_v1`。書いた関数がそのまま宣言になります。`brdf` と `lighting` は排他、未知の `pelican_` 関数定義やフックゼロはロードエラー。
- スニペットはエンジン所有テンプレート(`engine://shaders/material/surface_v1.{vert,frag}`)へ逆 include され、エラーは `#line` で元ファイル名・行番号に翻訳されます。パラメータへは自動生成アクセサ `pelican_param_<name>()` / `pelican_sample_<name>(uv)` でアクセスします。
- 同梱の standard / toon ライティングも**同じ公開経路**で書かれています(特権なし standard library。dogfooding)。

### 任意 G-buffer / material output ABI・attachment state（✅WP218/219）

G-buffer はエンジン固定の5スロットではありません。material pass が
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
`uint/uvec2/uvec3/uvec4`。RT format と floating / SINT / UINT の数値クラスが一致しない
設定は GPU pipeline 作成前に拒否します。組み込み `source` は
`surface.base_color`、`surface.normal`、`surface.normal_encoded`、
`surface.material`、`input.world_position`、`surface.emissive`、
`lighting.scene_color`。独自の packing や object ID は `custom` にして `.surface` から
名前付きフィールドへ代入します。

```glsl
void pelican_material_outputs_v1(
    in PelicanSurfaceInputV1 input_data,
    in PelicanSurfaceV1 surface,
    inout PelicanMaterialOutputsV1 outputs) {
    outputs.object_id = 73u;
}
```

生成後の SPIR-V reflection は全 location と数値型を宣言スキーマへ照合します。
flat / XR variant は同じ route に同一スキーマを要求します。graph の hot reload で
生存中 material の schema・format・MSAA・local-read・attachment state contract を
変える場合、現状は
候補世代を拒否して旧世代を維持します。これは不整合 pipeline を公開しないための境界で、
graph と material surface を同時に再構築する将来の coordinated transaction とは別です。

outputごとにblendまたは書き込みchannelを変える場合は、同じpassへ
`material_output_states`を追加します。キーはRT名やlocation番号ではなく
`material_outputs.outputs[].name`です。書かなかったoutputは`.surface`の
`render_state.blend`と`rgba` writeを継承します。

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
stateが異なる場合はdeviceの`independentBlend`、blend対象formatには
`COLOR_ATTACHMENT_BLEND` capabilityが必要です。全factor/opと現在含めていない
blend constant/dual-source blendの境界は
[WP219実装報告](../design_reviews/2026-07-28_wp219_material_output_states_report.md)
を参照してください。

`material_outputs` を省略した pass は既存プロジェクト向けの内蔵5-MRT/1-color ABIを
維持します。独自スキーマは生成 fragment shader が必要なため、現時点では runtime
shader compiler を有効にした開発ビルドで使います。shaderc OFF の配布物は
`dist-bake`（WP211）の生成物へ移す予定です。

### compute/別passのresourceをmaterialから読む（✅WP207b/220）

`.surface` はgraph固有名ではなく、再利用可能なsemantic portを宣言します。

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
- **primitive binding**: `pelican.material_bindings` v1(プリミティブ → マテリアル名の whole-model 契約)を asset_data の `models[].material_bindings` で参照できます(✅WP116。fragment モデルには適用不可)。

### 現状の重要な限界(バッジの肝)

- レンダラ接続は ✅ — `.surface` → コンパイル → パイプライン → SSBO → 描画まで golden(`surface_toon` / `skeletal_toon` / `openpbr_coat_sphere`)で実証済みです。
- **手書きの `.material.json` をプロジェクト起動時に読み込んでモデルへ割り当てる宣言的レーンは依然 🚧**です。既定のバインディング解決は **GLB 内の named material** にのみ働き(binding ABI ✅WP116)、OpenPBR golden もテストハーネスが登録を行っています。
- **per-instance マテリアルオーバーライド**(factor / UV の乗算 + material 別の絶対上書き)は ✅WP122/122b で renderer 機構として実装済みです。公開の書き込み面は VRM application service([第8章](08_gameplay.md) §8.13)経由で、GameContext API はありません。
- **spv-link(M3b/WP80)は experimental**: build時に`PELICAN_WITH_SPIRV_LINK=ON`、実行時に環境変数 `PELICAN_SPV_LINK=experimental` の両方を明示した場合だけ SPIR-V リンクバックエンドに切り替わります。build unitの既定はOFF、runtimeの既定は常にソース経路です。
- 設計文書 [../design_material_shading.md](../design_material_shading.md) は v1.2 のまま実装が追い越しています。**現行契約の正は [../shader_contract.md](../shader_contract.md)** です。

## 6.8 テンポラルとスナップショット(✅T1/T2 = WP88/95、M3.5 = WP83)

### history RT(`@history` 読み)

RT 宣言に `"history": true` を付けると物理 2 面持ちになり、パスの `input` で `<名前>@history` と書くと**前フレームの面**がバインドされます(`@history` はフレーム内依存を作りません)。実例(golden `temporal_accumulation` の feature):

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

- 名前付きパターンは `halton23`。**ユーザー定義数表**も同格です(✅WP115 — 名前付き系列に特権はなく、同じ数表を直書きすると全 byte 一致することが fixture で証明されています):

```json
"projection_jitter": { "pattern": "table", "offsets_px": [[0.0, -0.1667], [-0.25, 0.1667], [0.25, -0.3889]] }
```

(`offsets_px` は 1〜64 個・各成分 [-0.5, 0.5))

- **consumer 別配送**: メイン raster / SSAO / sprite は jittered、velocity はジッタ減算済み、**shadow・カリング・rpc・ゲームプレイは非ジッタのまま**(カメラ公開 API 不変)。
- **reset epoch**: 初回・リサイズ・`set_time`・カメラカット等で FrameUBO の epoch ペアが 1 フレームだけ不一致になり、シェーダは stateless に history 無効を判定できます。

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
`neighborhood` contractなので、この例は通常のsamplerを維持します。

v1 の制限: snapshot は **1 個だけ・不透明描画後の 1 点のみ**(透明描画後・
`pelican_ui` 以後はエラー)、逐次屈折は非対応です。

## 6.9 2D スプライト(✅S2D = WP103/104/106/109)

スプライトの描画は **feature 1 行 + シーンコンポーネント**です。新しいパス型はありません。

```json
"features": [ "engine://features/sprite.json" ]
```

(実物: [../../projects/sprite_demo/passes/main.json](../../projects/sprite_demo/passes/main.json))

`sprite.json` は RT もパスも足さない**純ゲート**で、参照すると canonical anchor `sprite` の位置(3D 不透明の後・ポスト処理の前)で、直近の scene color + depth に対して depth test ON / write OFF で描かれます(color/depth が無い構成では名指しエラー)。

- **コンポーネント** `sprite_view`(scene v1・closed schema): `texture`(必須。`"demo_atlas#sprite/full"` 形式の atlas 参照)/ `size`(vec2)/ `pivot`(既定 [0.5,0.5])/ `color` / `flip`([flip_x, flip_y])/ `layer`(int16)/ `billboard`(`none|y_axis|full`)。スプライトは常に **XY 平面 + Z 法線**(2026-07-12 決定。床置きは Transform の回転で表現)。ランタイム生成は `GameContext::createSpriteObject(...)`([第8章](08_gameplay.md))。
- **pixel policy**(✅WP106): `project.json` の `basic_config.sprite.pixels_per_unit`(既定 100.0)と、カメラの `sprite: {"pixel_perfect": "off"|"strict", "sort": "z"|"y_down"|"declaration"}`。`strict` の成立条件(正射影・整数ズーム・nearest・非 billboard 等)が満たされない場合は **silent fallback せず**理由付き WARN + `get_status.sprite` に状態が出ます。
- **flipbook**(✅WP106): コンポーネントではなく公開 stdlib API `Pelican::sprite::FlipbookClip`(ゲームコードが決定的な local time を渡す)。実例: `projects/sprite_demo/code/flipbook_demo.cpp`。
- 実例プロジェクト: [../../projects/sprite_demo](../../projects/sprite_demo)(WP109 の横スクロール vertical slice。操作系は [第8章](08_gameplay.md))。golden はスプライト系 9 ケースで回帰保護。

## 6.10 ヘッドレス描画とテスト(✅WP1〜8/16)

```sh
pelican_player --headless --project mygame --frames 3 --size 1280x720 --render-out out.png
```

- パス JSON の `"swapchain"` 出力は自動的にオフスクリーンイメージへ解決されるので、**config は無変更で動きます**。出力 PNG は encoded-sRGB(§6.3)。
- ゴールデンイメージテストは **49 ケース**(2026-07-21 時点)。プラン比較テストとあわせて回帰保護の柱です。検証の流儀は [../rendering_phase1_review.md](../rendering_phase1_review.md)。

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
| `Unknown pass type: ...` | `type` の typo(§6.2 の一覧参照) |
| `Only rendering resolver_version 2 is supported` | `resolver_version` に 2 以外を書いた |
| `insert anchor was not found: <name>` | feature の挿入先が存在しない(標準アンカー 8 個 + パス名が有効) |
| `Ambiguous writes-writes dependency for resource X between A and B` | 書き込み順が導出不能。after/before か中間リソースで解消 |
| `@history is supported only for render targets: ...` | バッファに `@history` を付けた |
| `Unknown or non-history resource reference ...@history` | `history: true` でない RT を `@history` 参照した |
| `sprite feature requires a scene color and depth attachment before sprite anchor` | sprite anchor の前に color+depth を出すパスが無い |
| snapshot 系エラー(2 個目・透明後・未定義名) | §6.8 の v1 制限違反 |
| pixel policy の WARN | strict 条件不成立(silent fallback しない方針) |
| `Shader defines require GLSL source, not SPIR-V` | .spv しか無い環境で feature(defines)を使った |
| `Shader descriptor binding mismatch` | vert/frag で同一 binding の型が不一致 |
| `Shader/pipeline transaction failed; keeping the previous generation: ...` | ホットリロード失敗(旧版継続中。ソースを直せば次の保存で回復) |
| `placeholder has no resolved binding` / parameter 名入り compose エラー | feature の `$名前` に対応する binding / default が無い(§6.5) |
| projection jitter provider の重複エラー | `projection_jitter` を宣言する feature を 2 個以上参照した(単一 provider 排他) |
| `OpenXR activation rejected history feature '<name>'` ほか XR activation 拒否 | XR graph から除外できない temporal / UI 構成(§6.12) |
| `runtime-selected physical device lacks Vulkan feature timelineSemaphore required by OpenXR` | OpenXR ランタイムが選んだ GPU が `timelineSemaphore` 非対応(✅WP138 で silent 失敗から名指しエラーへ) |
| `preview graph '<config>' rejects authored projection_jitter` / `... rejects authored history target '<name>'` / `... rejects authored unsafe target '<name>'` | preview graph に載せられない構成を rendering config が直書きしている(§6.13) |
| `preview graph '<列名>' rejects authored pass '<パス名>': <理由>` | 同上(理由は `unsafe pass type/name <type>` / `history read` / `history write`)|
| `preview graph config requires rendering_passes` | 合成後の config に `rendering_passes` が無い(§6.13) |
| `golden inventory: FAIL` + `- <issue>` | `test/golden/inventory.json` と `test/golden/` の実体が食い違う(§6.10) |

## 6.12 OpenXR ステレオレンダリング(✅WP125〜138 / WP203a〜c実装済み)

`--xr on|auto` で起動すると([第2章](02_getting_started.md))、レンダラは**論理フレーム**単位の二眼描画に切り替わります。

- **論理フレーム / ViewFamily(WP128/WP223〜237)**: `renderLogicalFrame(target, view_families)`が共有更新(アニメ・リロード・共有アップロード・**temporal historyのadvance**)を論理フレームにつき**一回**だけ行います。flatは`$main/$mono`、XRはstable eye ID付き`$main` stereo familyです。pass/taskの`view_family`は汎用runtime registryで解決され、caller-authored familyが常に優先されます。標準directional shadowは`$shadow/directional`のstable `$cascade/N`群、標準planar reflection packageは`$reflection/planar`のstable `$mirror/<source-view>`群、標準cube captureは`$capture/cube`のstable `$face/{+x,-x,+y,-y,+z,-z}`群としてmain cameraから独立して実行されます。projection jitterはmain family modifierとして一度sampleされ、FrameUBOはin-flight × 全family viewのスロットとstable family tokenで相互汚染を防ぎます。clustered selectionもfamily/view別領域を検証して使います。標準planar providerはclip planeをVulkan ZOのoblique near planeへ変換し、cube providerはsquare 90度投影の六方向cameraを作ります。どちらもpackage OFF時はcaller/project providerで全面置換できます。secondary familyの複数viewはsequential実行に対応し、transparent passを持つfamilyは同じ公開sort providerをviewごとに再評価します。secondary multiviewは未対応です。
- **コンポジション(WP129/203c)**: XR 用は `IFrameTarget` とは別系統の `IXrCompositionTarget`。現在は **2-layer の color array swapchain を一個**使い、一回だけ acquire/wait/release します。左右の projection view は同じ image の `imageArrayIndex=0/1` を参照し、1 つの projection layer・**単一の `xrEndFrame`** で提出します。
- **optional composition depth(WP203c)**: `XR_KHR_composition_layer_depth`、compiled graph の external depth export、OpenXR/Vulkan の format/usage 条件が成立すると 2-layer depth swapchain を作ります。各フレームで source format/extent も一致したときだけ有効化し、sequential 描画なら layer ごと、multiview 描画なら array 全体を copy して左右の `XrCompositionLayerDepthInfoKHR` を提出します。不一致なら利用可能な depth swapchain も idle のままにし、color-only へ戻ります。
- **view execution(WP203a〜c)**: `xr.view_execution` は `"auto"` / `"sequential"` / `"multiview"`。`auto` は対応済み scope だけを multiview にし、material/custom pass は capability を明示するまで sequential のまま混在実行します。required `"multiview"` は対応不能な device/pass を fallback せず compile error にします。選択根拠は `get_frame_plan` の `physical_target_plan.view_execution_plan.auto_gate` で確認できます。
- **座標系(WP131)**: `world_from_stage = inverse(フレーム開始時の active camera view)` — flat のカメラ API は不変のまま、XR アダプタ内でのみ変換します。reference space は STAGE → LOCAL_FLOOR → LOCAL の優先選択で、LOCAL への fallback 時は「床は非保証」を `get_status.xr` が明示します。
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
- 名前または宣言面に `taa` / `velocity` / `motion_vector` / `projection_jitter` / `ui` / `imgui` / `mirror` / `present` を含む feature、`history: true` の RT を宣言する feature、`projection_jitter` を宣言する feature は**まるごと除外**されます(除外名は `excluded_feature_names` に記録され、composer の依存/アンカー検証はそのまま働きます)。
- **config へ直書き**した同種の構成は除外できないため、名指しの起動エラーになります(§6.11 の `preview graph ...` 行)。`@history` の読み書きも同様です。ただし名前が `present` で終わる終端パスだけは、出力が request-local な終端(`preview_capture` / `display`)に解決されていて `mirror` / `ui` を含まない場合に限り保持されます。

合成結果には `generation`(合成後 config の FNV ハッシュ)が付き、`render_preview` の応答に `graph_generation` として返ります。config を編集して再合成すると値が変わるので、エディタ側の再取得判定に使えます。

## 6.14 GPU デバッグラベルと計測(✅WP139/140/143/145)

「絵が違う」「重い」を追うための面は 4 つあり、有効化の重さが違います。設計の正は [../design_debug_profiling.md](../design_debug_profiling.md)(v1.1)です。

| 面 | 有効化 | 出口 |
|---|---|---|
| フレームメトリクスログ / `get_status.memory` / `get_status.xr.timing` | 常設(何もしない) | ログ・rpc・ImGui |
| GPU タイミング | rendering config の features に `engine://features/gpu_timing.json` を 1 行 | ログ・`get_status.gpu_timing`・ImGui |
| Vulkan のデバッグ名とコマンドラベル | 起動フラグ `--gpu-labels`(既定 OFF) | RenderDoc / NSight / validation メッセージ |
| RenderDoc キャプチャ | RenderDoc の Launch Application から player を起動(注入) | F11 / rpc `capture_gpu` → `.rdc` |

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
- validation layer は `--gpu-labels` では有効になりません(`_DEBUG` 構成のみ)。ただし `--gpu-labels` を付けると validation メッセージにも同じ論理名が出ます。

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
| `logical_frame_total_sum_views_ms` | 最新フレームの view 行の**単純合計**(左右眼を平均しません)|
| `views[]` | `{logical_frame, graph_variant, view_index, label, barriers_ms, body_ms, total_ms}`。`label` は flat/0 → `flat`、xr/0 → `left`、xr/1 → `right`、xr/2 → `mirror` |
| `nodes[]` | `{..., node_ordinal, node_kind, node_name, subrange, identity, supported, reason, ms}` |
| `query_pool` | `{frame_slots, range_slots, max_nodes, query_capacity, create_count, pending_ranges}` |

`nodes[].identity` は**コマンドラベルと同じ文字列に `/barriers` または `/body` を足したもの**なので、RenderDoc の Event Browser の木と 1:1 で突き合わせられます。`barriers` は「合成されて入ってきたバリア」、`body` は「そのノード自身の遷移と描画/ディスパッチ」です(帰属契約の正本は `test/fixtures/gpu_timing_attribution.json`)。仕事を持たないアンカーは `supported: false` / `reason: "no_gpu_work"` / `ms: 0.0` になります。

feature 無効時も**同じ封筒**が返ります(`{"schema_version": 2, "enabled": false, "supported": false, "reason": "feature_not_enabled", ...}`)。

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

## 関連文書

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
- [../design_openxr.md](../design_openxr.md) — OpenXR(v2.2・XR0〜XR4 + WP203a〜c local implementation済み。desktop mirror WSI lifecycleはWP215〜217計画済み。現実装のSimulator/物理HMD・対象GPU実測gateは未)
- [../design_2d_game_layer.md](../design_2d_game_layer.md) — 2D ゲーム層(v2.4・S2D 実装済み)
- [../design_asset_hot_reload.md](../design_asset_hot_reload.md) — アセットホットリロード(v2.1・HR0〜HR2-G 実装済み)
- [../design_debug_profiling.md](../design_debug_profiling.md) — デバッグ・プロファイリング(v1.1・条件付き受理。D-P0a/D-P1a/D-P2a/D-P2b 実装済み、§6.14)
- [第5章 アセット](05_assets.md) / [第9章 Web プロファイル](09_web.md) / [第10章 ツールリファレンス](10_tools.md)
