# 第6章 レンダリング

対象: pelican2(2026-07-17 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- レンダリングパイプラインを **JSON だけ**で定義する方法(rendering config)
- パス種別(`material` / `fullscreen` / `output_transform` / `ui` / `shadow_depth` / `velocity` / `debug_draw` / `debug_text`)と compute タスク
- カラーパイプライン(SRGB スワップチェーン + リニアワークフロー)で気をつけること
- feature(1 行で有効化できるパージ可能な GPU 機能)— canonical anchor・パラメータ・named binding
- フレームグラフ(依存宣言 → 機械最適化 → 手詰め)とプランダンプ
- シェーダ基盤 — stem 参照・FrameUBO・ホットリロード・descriptor set 規約
- マテリアル(`.surface` + `pelican.material` + **OpenPBR**)、テンポラル(history / velocity / **projection jitter / 標準 TAA**)、2D スプライト
- OpenXR ステレオレンダリング(論理フレーム / feature policy / ミラー)

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

`basic_config.default_rendering_pass` が rendering config 内のどのパス列(`rendering_passes[].name`)を使うかを決めます。**デフォルトのパス列が `swapchain` に出力しない場合は起動時エラー**です。エンジンは常にパージ不能な終端 `output_transform` ノード(リニア → 表示エンコードの 1 箇所)を生成します(§6.3)。

## 6.2 rendering config のスキーマ

実物の最小構成([../../projects/example/passes/example_renderingpass_data.json](../../projects/example/passes/example_renderingpass_data.json) — G-buffer → ライティング → UI)は前版と同じ骨格で動きます。example が実際に使う [main_rendering_config.json](../../projects/example/passes/main_rendering_config.json) は SSAO と 4 段ブルームを足した 18 パス構成です。

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
| `format` | ✔ | `B8G8R8A8_UNORM` / `B8G8R8A8_SRGB` / `R8G8B8A8_UNORM` / `R8G8B8A8_SRGB` / `R8_UNORM` / `R16G16_SFLOAT` / `R16G16B16A16_SFLOAT` / `D32_SFLOAT` / `D24_UNORM_S8_UINT` / `D16_UNORM` |
| `format_class` | 任意 | `scene` / `display` / `data` / `explicit(<FORMAT>)`。カラーリゾルバが実 format を決める(§6.3)✅WP73 |
| `role` | 任意 | `color` / `data`。SRGB view / UNORM view の選択 |
| `history` | 任意 bool | `true` で 2 面持ち。前フレーム面は `<名前>@history` で読める(§6.8)✅WP88 |
| `clear_color` | 任意 | history RT の初期化色 |
| `usage` | ✔ | `COLOR_ATTACHMENT` / `DEPTH_STENCIL_ATTACHMENT` / `SAMPLED` / `STORAGE` / `TRANSFER_DST` / `TRANSFER_SRC` |

### pass 共通フィールド

| フィールド | 必須 | 既定 | 内容 |
|---|---|---|---|
| `name` | ✔ | — | パス列内で一意 |
| `type` | ✔ | — | `material` / `fullscreen` / `output_transform` / `ui` / `shadow_depth` / `velocity` / `debug_draw` / `debug_text`(+ ImGui ビルド時 `imgui`) |
| `output` | ✔ | — | `color`(null / 名前 / 名前配列)と `depth`(null / 名前)の**両キー必須**。`"swapchain"` は color のみ |
| `input` | 任意 | — | 読み込む RT / バッファ名。RT には **`@history` サフィックス**可(history RT のみ) |
| `color_load_op` / `color_store_op` | 任意 | `Clear` / `Store`(ui のみ load 既定) | `Clear` / `Load` / `DontCare` |
| `depth_load_op` / `depth_store_op` | 任意 | `Clear` / `DontCare` | シャドウマップでは `depth_store_op: "store"` を明示 |
| `clear_color` | 任意 | `[0,0,0,1]` | 4 要素固定 |
| `after` / `before` | 任意 | — | フレームグラフの明示エッジ(§6.6) |

主な検証(すべて起動時の名指しエラー): input の RT に `SAMPLED` usage が必要 / 1 パスの全出力 RT は同一サイズ / 同一 RT の入出力同時使用は不可 / input に書いた RT は先行パスが出力していること。

### type 別の要点

- **`material`** — シーン内の全モデルを描く G-buffer パス。**color 出力はちょうど 5 枚**。カラーパイプライン移行後の契約は SDR 時 `B8G8R8A8_SRGB(albedo), R16G16B16A16F(normal), R8G8B8A8_UNORM(material), R16G16B16A16F(worldpos), B8G8R8A8_SRGB(emissive)`、depth は `D32_SFLOAT` 固定(HDR 時は albedo/emissive が float16 変種)。`shader` は書けません。
- **`fullscreen`** — 全画面 1 枚描き。`shader: { "vertex": <stem>, "fragment": <stem> }` **必須**。`input` の RT はシェーダの set 1 に**配列順**でバインド(最大 8 入力)。`uses_light_data: true` でライト UBO。`push_constants`(`"none"` / `"camera_position"` / `"projection_view"`)は**互換キー**として受理されますが、GPU への実際の供給元は常に set 0 の FrameUBO / LightUBO です(§6.4)。
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

1. **色テクスチャは sRGB で作る**(baseColor / emissive → SRGB view で自動デコード)。normal / metallicRoughness / データテクスチャはリニア(UNORM view)。`.surface` の custom texture は `role: color` 宣言で SRGB になります。
2. **シェーダに gamma 補正を書かない**。書くと二重変換になります。
3. glTF の `baseColorFactor` / `emissiveFactor` / `COLOR_0` は**リニア値のままエンジンに入ります**(仕様どおり)。C++ 側でデバッグ色などを直書きするときは `Pelican::srgb(r,g,b)` ヘルパ([color.hpp](../../src/core/userpublic/color.hpp))。
4. RT の `format_class` を使うと実 format はリゾルバが決めます: `scene` → HDR 時 `R16G16B16A16_SFLOAT` / SDR 時 `B8G8R8A8_SRGB`、`display` → `B8G8R8A8_SRGB`、`data` → UNORM のまま。
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
| 0 | `PELICAN_SET_FRAME` | **全パイプライン共通の固定 layout**: binding 0 `FrameUBO` / 1 `ObjectBuffer` SSBO / 2 `LightUBO` / 3 `PreviousObjectBuffer`(velocity 用)。シェーダが set 0 を宣言する場合は一致必須、宣言しなくても PipelineFactory が挿入 |
| 1 | `PELICAN_SET_PASS_INPUT` | パス入力。fullscreen の `input` 配列順に binding 0..、compute の reads/writes も set 1 |
| 2 | `PELICAN_SET_MATERIAL` | binding 0-3 標準 PBR テクスチャ、4-5 VAT、**6 = 全マテリアル配列の `MaterialBuffer` SSBO**(標準 96B + custom values 256B / 要素)、7 以降 = `.surface` の custom texture(宣言順) |
| 3 | `PELICAN_SET_FREE` | 自由枠(debug_draw / debug_text が使用) |

`FrameUBO`(全シェーダから読める・352B): `time` / `dt` / 64bit `frame_index` / `resolution` + 逆数 / `camera_position` / `view` / `projection` / `previous_view` / `previous_projection` / `jitter_ndc` / `previous_jitter_ndc` / `temporal_reset_epoch` / `previous_temporal_reset_epoch`。projection jitter が無効な既定構成では jitter は `(0, 0)` です。

push constant は 128B(エンジン 64B + シェーダ 64B)で、✅**リフレクション段階で enforcement 済み**です(4B align・128B 上限・エンジン領域の部分使用をパイプライン作成前に拒否)。エンジン頂点レイアウトは location 0=`inPos`, 1=`inNormal`, 2=`inTexUV`, 3=`inColor`, 4=`inTangent`。

エンジン同梱シェーダ(`engine://`、抜粋): `fullscreen` / `ssao` / `ssao_blur` / `bloom_*` / `tonemap` / **`output_transform`** / `shadow_depth` / `skinned` / `skinned_shadow_depth` / **`velocity` / `velocity_skinned`** / **`taa_resolve` / `taa_composite`** / **`sprite`** / `debug_draw` / `debug_text` / `default` / `vat` / `ui`、マテリアルテンプレート `shaders/material/surface_v1.{vert,frag}` + `standard_lighting.glsl` / `toon_lighting.glsl` / **`openpbr_lighting.glsl`**、**OpenPBR wrapper 6 種 `engine://surfaces/openpbr/*.surface`**、公開 include 群 `shaders/include/pelican_*.glsl`。

🚧 HLSL / Slang: `.surface` の `language` フィールドとして形式上は受理されますが、既定バックエンドは GLSL 以外を reject します(他言語は spv-link experimental の視野 — §6.7)。

## 6.5 feature — パージ可能な GPU 機能(✅WP28〜31/54/87/88/104)

> **設計決定(参照 = 存在):** feature は rendering config に**参照を書いたときだけ**存在する。1 つも書かなければ合成機構ごと素通りし、挙動は完全に不変(パージ可能)。golden テストの「feature off = 既存出力の完全維持」で保証されている。

> **設計決定(feature 層 = ユーザー空間、2026-07-12):** エンジンが持つのは**機構語彙**(anchor / history / snapshot / format_class など — 版付きで additive にのみ増える)だけ。feature(JSON + シェーダ)は**ユーザー空間**で、同梱 feature は**特権なしの標準ライブラリ**にすぎない。`engine://features/*.json` をプロジェクトへコピーして改造したら自分のもの、が公式ワークフロー。canonical anchor の全順序・`output_transform` 等の常設ノード・色 invariant・決定性ゲートだけは名前入りエラーで防衛される。

使い方は 1 行です:

```json
{ "features": [ "engine://features/shadow_directional.json", "engine://features/velocity.json" ], ... }
```

エンジン同梱 feature(9 個・✅すべて実装済み):

| feature | 内容 |
|---|---|
| `hdr.json` | `lit_color` を float16 化し、トーンマップパスを `after:tonemap` アンカーに挿入 |
| `shadow_directional.json` | 2048×2048 シャドウマップ + `shadow_depth` パス挿入 + `lighting_pass` へ入力追加 |
| `ui.json` | UI の GPU quad 描画(✅WP87。[第7章](07_input_ui.md)) |
| `velocity.json` | モーションベクタ RT(`R16G16_SFLOAT`)+ `velocity` パスを `before:post_main` に挿入(✅WP88) |
| `taa.json` | **標準 TAA**(resolve + composite の二パス + halton23/8 の jitter provider + スカラーパラメータ。§6.8)✅WP113 |
| `sprite.json` | **純ゲート**(RT もパスも足さない)。参照すると `__anchor_sprite` でスプライトが描かれる(§6.9) |
| `debug_draw.json` | ワイヤフレームオーバーレイ(collider 可視化など) |
| `debug_text.json` | ビットマップ文字 HUD([第7章](07_input_ui.md)) |
| `gpu_timing.json` | パスなしの計測フラグ。GPU パス単位タイムスタンプをログに出力 |

fragment(`pelican.render_feature` v1)に書けるもの: `render_targets` / `buffers` / `compute_tasks`(追加。名前衝突はエラー。RT には `format_class` / `role` / `history` も書ける)、`render_target_overrides`(既存 RT の format/usage 等を上書き)、`passes`(`insert: "before:<アンカー|パス名>" | "after:<...>" | "end"`)、`pass_overrides`(既存パスへの input 追加)、`shader_defines`、**`parameters`(下記)**、**`projection_jitter`(§6.8)**。`shadow_directional.json` の全文例は前版と同じです。

### feature パラメータと named binding(✅WP112/114)

feature は**パラメータ化**できます。`features` 配列は文字列のほかに `{ref, parameters}` のインスタンス形式を受理します:

```json
"features": [
  "engine://features/velocity.json",
  { "ref": "engine://features/taa.json",
    "parameters": { "scene_color": "lit_color", "alpha": 0.1 } }
]
```

- feature 側は `parameters`(`pelican.render_feature_parameters` v1)で宣言します: `render_targets`(name / required / default / role / format_class / usage — **RT の named binding**)と `scalars`(`float` / `int` / `bool`。`default` 必須、float/int は `range: [min,max]` 必須)。
- feature 本文の中では **`$<パラメータ名>`** プレースホルダで参照します(`$scene_color@history` も可)。未解決・型不適合は feature 名・パラメータ名入りの compose エラー。
- スカラーは **`PELICAN_FEATURE_<FEATURE名>_<PARAM名>=<値>`** の値付き define に lower されます(float は 9 桁 round-trip 表記。値を変えるとシェーダキャッシュキーも変わる = 正しく再コンパイル)。
- 解決結果は frame plan の `feature_instances` に出ます(`--dump-frame-plan` / Plan Viewer で確認可能)。

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

(スキーマは前版から不変)compute シェーダは `<stem>.comp`、リソースは set 1、`reads` / `writes` の宣言だけで並びます。`schedule` は `per_frame` のみ、`dispatch.groups_from` は予約。最小の実例は `test/run_compute_headless.cmake`。

### プランダンプ(実行計画の可視化)

```sh
pelican_player --project mygame --headless --dump-frame-plan   # stderr に出力
# または rpc の get_frame_plan。GUI では ImGui の Plan Viewer(第10章)
```

出力にはノードごとの `order` / `level` / `reads` / `writes`(`@history` 読みは `reads_history`)と導出された `barriers`、`snapshot_copy` ノードが含まれます。

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

- ヘッダ語彙: `language` / 順序付き `params[]`(型 `floating|vec2|vec3|vec4|integer|color`、**`default` 必須**、min/max/hint 任意)/ `textures[]`(`role: color|data` — SRGB/UNORM view の選択)/ `screen_inputs[]`(§6.8)/ `render_state`。網羅例は `test/fixtures/surface_format/valid/full.surface`。
- **フック梯子 v1(凍結)**: `pelican_vertex_displace_v1` / `pelican_surface_v1` / `pelican_brdf_v1` / `pelican_ambient_v1` / `pelican_lighting_v1`。書いた関数がそのまま宣言になります。`brdf` と `lighting` は排他、未知の `pelican_` 関数定義やフックゼロはロードエラー。
- スニペットはエンジン所有テンプレート(`engine://shaders/material/surface_v1.{vert,frag}`)へ逆 include され、エラーは `#line` で元ファイル名・行番号に翻訳されます。パラメータへは自動生成アクセサ `pelican_param_<name>()` / `pelican_sample_<name>(uv)` でアクセスします。
- 同梱の standard / toon ライティングも**同じ公開経路**で書かれています(特権なし standard library。dogfooding)。

### pelican.material — 値だけの JSON

```json
{ "schema": "pelican.material", "version": 1,
  "materials": [ { "name": "example_toon", "surface": "project://shaders/toon.surface",
                   "values": { "tint": [1.0, 0.32, 0.08, 1.0] } } ] }
```

(実物: [../../projects/example/materials/toon.material.json](../../projects/example/materials/toon.material.json))

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
- **per-instance マテリアルオーバーライド**(factor / UV の乗算 + material 別の絶対上書き)は ✅WP122/122b で renderer 機構として実装済みです。公開の書き込み面は VRM application service([第8章](08_gameplay.md) §8.12)経由で、GameContext API はありません。
- **spv-link(M3b/WP80)は experimental**: 環境変数 `PELICAN_SPV_LINK=experimental` を明示したプロセスのみ SPIR-V リンクバックエンドに切り替わります。既定は常にソース経路です。
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

インスタンス単位の factor / UV 乗算オーバーライドと、`(インスタンス, glTF material index)` 単位の絶対上書きが renderer 機構として入っています(N/N-1 履歴付き — VRM 表情の適用先)。公開の書き込み面は VRM application service([第8章](08_gameplay.md) §8.12)で、rendering config に書くものはありません。

### 名前付きスクリーンスナップショット(屈折・歪みの入口)

```json
"snapshots": [ { "name": "opaque_color", "after": "opaque" } ]
```

合成時に `snapshot_copy` ノードへ展開され、`.surface` 側の宣言と自動生成サンプラで読めます:

```glsl
//! screen_inputs: [opaque_color]
vec4 behind = pelican_screen_opaque_color(surface_input.uv + offset);
```

(実物: `projects/example/shaders/refract.surface`)。v1 の制限: snapshot は **1 個だけ・不透明描画後の 1 点のみ**(透明描画後・`pelican_ui` 以後はエラー)、逐次屈折は非対応です。

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
- ゴールデンイメージテストは **49 ケース**(2026-07-17 時点。`test/golden/` — ディレクトリを置くだけで自動発見されます)。プラン比較テストとあわせて回帰保護の柱です。検証の流儀は [../rendering_phase1_review.md](../rendering_phase1_review.md)。

> **設計決定(レイヤ規則):** `src/core/vkcore`(Vulkan 低層)から `src/core/renderingpass` 以上のレイヤへ include を追加しない。下位層はフラグを上げるだけで、編成は上位層 Renderer が行う。

## 6.11 トラブルシューティング

| エラー(抜粋) | 原因 |
|---|---|
| `Default rendering pass does not output to swapchain` | `default_rendering_pass` のパス列が画面に出力していない |
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

## 6.12 OpenXR ステレオレンダリング(✅WP125〜135)

`--xr on|auto` で起動すると([第2章](02_getting_started.md))、レンダラは**論理フレーム**単位の二眼描画に切り替わります。

- **論理フレーム / view 分離(WP128)**: `renderLogicalFrame(target, view_count)` が共有更新(アニメ・リロード・共有アップロード・**temporal history の advance**)を論理フレームにつき**一回**だけ行い、view ごとに camera / projection / FrameUBO を切り替えてグラフを実行します。FrameUBO は in-flight × view のスロット制で、眼間の汚染を構造的に防ぎます。flat の `render()` は view_count=1 の互換アダプタです(golden byte 一致で保証)。
- **コンポジション(WP129)**: XR 用は `IFrameTarget` とは別系統の `IXrCompositionTarget`。view ごとの swapchain 2 個 + per-swapchain 状態機械で、両 view を 1 つの projection layer・**単一の `xrEndFrame`** で提出します。
- **座標系(WP131)**: `world_from_stage = inverse(フレーム開始時の active camera view)` — flat のカメラ API は不変のまま、XR アダプタ内でのみ変換します。reference space は STAGE → LOCAL_FLOOR → LOCAL の優先選択で、LOCAL への fallback 時は「床は非保証」を `get_status.xr` が明示します。
- **feature policy(WP133)**: flat / XR の両グラフを起動時にコンパイルし、**XR グラフからは TAA・projection jitter・velocity・history・UI の feature を自動除外**します。除外できない構成(config 直書きの history 読み・UI パス・未知の history feature)は XR 起動を名指しで拒否します — **history を持つ自作 feature は `--xr on` を止める**ことに注意してください(vrm_xr_demo の config が features 空配列なのはこのため)。XR の出入り境界では temporal reset が 1 回入ります。
- **ミラー(WP133)**: デスクトップウィンドウには左眼の best-effort ミラー(scale + letterbox、UI はミラー側にのみ重畳)。ウィンドウが詰まっても **HMD のフレームループは待たされません**。XR 中の従来 capture は名指しで拒否されます(headless / golden は常に flat 経路)。
- ⚠ 制限: 実機(Quest 3 Link)での表示確認は未実施(WP136 = XR Simulator smoke が未着手)。multiview(XR2b)・深度 submit・world-space UI は 📐。セッション喪失時の再生成はループ側未配線(現状は終了)。

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
- [../design_openxr.md](../design_openxr.md) — OpenXR(v2.1・XR0〜XR4 実装済み。XR2b・実機 gate は未)
- [../design_2d_game_layer.md](../design_2d_game_layer.md) — 2D ゲーム層(v2.4・S2D 実装済み)
- [../design_asset_hot_reload.md](../design_asset_hot_reload.md) — アセットホットリロード(v2.1・HR0〜HR2-G 実装済み)
- [第5章 アセット](05_assets.md) / [第9章 Web プロファイル](09_web.md) / [第10章 ツールリファレンス](10_tools.md)
