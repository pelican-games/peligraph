# 第6章 レンダリング

対象: pelican2(2026-07-10 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- レンダリングパイプラインを **JSON だけ**で定義する方法(rendering config)
- パス種別(`material` / `fullscreen` / `ui` / `shadow_depth` / `debug_draw` / `debug_text`)と compute タスク
- feature(1 行で有効化できるパージ可能な GPU 機能)の使い方と作り方
- フレームグラフ(依存宣言 → 機械最適化 → 手詰め)の考え方とプランダンプ
- シェーダ基盤 — stem 参照・実行時コンパイル・ホットリロード・descriptor set 規約
- マテリアル・ポストプロセスの現状(何が設計のみか)

## 6.1 全体像

pelican2 の描画パイプラインは、`project.json` の `basic_config.rendering_config_json` が指す **rendering config JSON** で完全に宣言されます。**C++ を書く必要はありません**。

```
project.json ──rendering_config_json──▶ rendering config JSON
                                            │  features 合成(§6.4)
                                            ▼
                              レンダーターゲット登録 + パス列パース
                                            │
                                            ▼
                              フレームグラフのプラン作成・検証(§6.5)
                                            │
                                            ▼
                     毎フレーム: layout 遷移 → dynamic rendering でパス実行
```

`basic_config.default_rendering_pass` が rendering config 内のどのパス列(`rendering_passes[].name`)を使うかを決めます。**デフォルトのパス列が `swapchain` に出力しない場合は起動時エラー**です。

## 6.2 rendering config のスキーマ

実物の最小構成([../../projects/example/passes/example_renderingpass_data.json](../../projects/example/passes/example_renderingpass_data.json) — G-buffer → ライティング → UI):

```json
{
  "render_targets": [
    { "name": "gbuffer_albedo",   "extent_scale": 1.0, "format": "B8G8R8A8_UNORM",      "usage": ["COLOR_ATTACHMENT", "SAMPLED"] },
    { "name": "gbuffer_normal",   "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "usage": ["COLOR_ATTACHMENT", "SAMPLED"] },
    { "name": "gbuffer_material", "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",      "usage": ["COLOR_ATTACHMENT", "SAMPLED"] },
    { "name": "g_emissive",       "extent_scale": 1.0, "format": "R8G8B8A8_UNORM",      "usage": ["COLOR_ATTACHMENT", "SAMPLED"] },
    { "name": "gbuffer_worldpos", "extent_scale": 1.0, "format": "R16G16B16A16_SFLOAT", "usage": ["COLOR_ATTACHMENT", "SAMPLED"] },
    { "name": "offscreen_depth",  "extent_scale": 1.0, "format": "D32_SFLOAT",          "usage": ["DEPTH_STENCIL_ATTACHMENT"] }
  ],
  "rendering_passes": [
    {
      "name": "main_render",
      "passes": [
        {
          "name": "gbuffer_pass",
          "type": "material",
          "output": {
            "color": ["gbuffer_albedo", "gbuffer_normal", "gbuffer_material", "gbuffer_worldpos", "g_emissive"],
            "depth": "offscreen_depth"
          }
        },
        {
          "name": "lighting_pass",
          "type": "fullscreen",
          "output": { "color": "swapchain", "depth": null },
          "input": ["gbuffer_albedo", "gbuffer_normal", "gbuffer_material", "gbuffer_worldpos", "g_emissive"],
          "shader": { "vertex": "engine://fullscreen", "fragment": "engine://fullscreen" },
          "push_constants": "camera_position",
          "uses_light_data": true
        },
        {
          "name": "ui_pass",
          "type": "ui",
          "output": { "color": "swapchain", "depth": null }
        }
      ]
    }
  ]
}
```

example が実際に使う [main_rendering_config.json](../../projects/example/passes/main_rendering_config.json) は、この骨格に SSAO と 4 段ブルームを足した 18 パス構成です(ダウンサンプルは `extent_scale: 0.5 / 0.25 / ...` の RT で表現)。

### トップレベルキー

| キー | 型 | 内容 |
|---|---|---|
| `features` | string 配列 | feature fragment の参照(§6.4) |
| `shader_defines` | string 配列 | 全シェーダのコンパイルに注入する define |
| `render_targets` | 配列 | レンダーターゲット(RT)宣言 |
| `buffers` | 配列 | フレームグラフ用 GPU バッファ(§6.5) |
| `rendering_passes` | 配列 | `{name, passes: [...]}` のパス列 |
| `compute_tasks` | 配列 | compute タスク(§6.5) |

名前の重複(RT・パス・バッファ)はすべて hard error です。

### render_targets

| フィールド | 必須 | 内容 |
|---|---|---|
| `name` | ✔ | `"swapchain"` は予約名(書くとエラー) |
| `extent_scale` | ✔ | ウィンドウ(swapchain)サイズ × scale。> 0 |
| `width` / `height` | 任意(両方セット) | 固定サイズ RT(シャドウマップ等)。片方だけはエラー |
| `format` | ✔ | `B8G8R8A8_UNORM` / `R8G8B8A8_UNORM` / `R8_UNORM` / `R16G16B16A16_SFLOAT` / `D32_SFLOAT` / `D24_UNORM_S8_UINT` / `D16_UNORM` |
| `usage` | ✔ | `COLOR_ATTACHMENT` / `DEPTH_STENCIL_ATTACHMENT` / `SAMPLED` / `STORAGE` / `TRANSFER_DST` / `TRANSFER_SRC` |

### pass 共通フィールド

| フィールド | 必須 | 既定 | 内容 |
|---|---|---|---|
| `name` | ✔ | — | パス列内で一意 |
| `type` | ✔ | — | `material` / `fullscreen` / `ui` / `shadow_depth` / `debug_draw` / `debug_text` |
| `output` | ✔ | — | `color`(null / 名前 / 名前配列)と `depth`(null / 名前)の**両キー必須**。`"swapchain"` は color のみ |
| `input` | 任意 | — | 読み込む RT / バッファ名。**fullscreen 専用** |
| `color_load_op` / `color_store_op` | 任意 | `Clear` / `Store`(ui のみ load 既定) | `Clear` / `Load` / `DontCare` |
| `depth_load_op` / `depth_store_op` | 任意 | `Clear` / `DontCare` | シャドウマップでは `depth_store_op: "store"` を明示 |
| `clear_color` | 任意 | `[0,0,0,1]` | 4 要素固定 |
| `after` / `before` | 任意 | — | フレームグラフの明示エッジ(§6.5) |

主な検証(すべて起動時の名指しエラー): input の RT に `SAMPLED` usage が必要 / 1 パスの全出力 RT は同一サイズ / 同一 RT の入出力同時使用は不可 / input に書いた RT は先行パスが出力していること。

### type 別の要点

- **`material`** — シーン内の全モデルを描く G-buffer パス。**color 出力はちょうど 5 枚**で、フォーマット順は `B8G8R8A8(albedo), R16G16B16A16F(normal), R8G8B8A8(material), R16G16B16A16F(worldpos), R8G8B8A8(emissive)`、depth は `D32_SFLOAT` 固定契約です。`shader` は書けません(マテリアル側で決まる)。
- **`fullscreen`** — 全画面 1 枚描き。`shader: { "vertex": <stem>, "fragment": <stem> }` **必須**。`input` の RT はシェーダの set 1 に**配列順**でバインドされます(最大 8 入力)。`push_constants` は `"none"` / `"camera_position"` / `"projection_view"`。`uses_light_data: true` でライト情報 UBO が使えます。
- **`ui`** — UI オーバーレイ([第7章](07_input_ui.md))。追加フィールドなし。
- **`shadow_depth`** — depth 専用パス。`shader` 省略時は `engine://shadow_depth`。ライトの view-proj で全モデルを描きます。
- **`debug_draw` / `debug_text`** — 通常は feature 経由で挿入されます(§6.4)。

## 6.3 シェーダ基盤(Shader Freedom Kit ✅WP10〜15/19)

### stem 参照

シェーダは**拡張子なしの stem** で参照します(例: `"shaders/lava"`、`"engine://ssao"`)。`.vert` / `.frag` / `.spv` などの拡張子を書くと「use an extensionless <stage> shader stem」エラーになります(WP63)。

解決順: ① `<stem>.vert`(GLSL ソース → 実行時コンパイル。`PELICAN_RUNTIME_SHADER_COMPILER=ON` 時)→ ② `<stem>.vert.spv`(ビルド済み SPIR-V)。どちらも無ければ試行パス一覧付きエラー。

> **設計決定(二層供給):** 開発時 = GLSL 実行時コンパイル(defines によるバリアント可)、配布 = `.spv` 固定 1 バリアント。`.spv` に `shader_defines` を組み合わせると「Shader defines require GLSL source」エラー。feature を使う config は実行時コンパイラ必須(v1 の割り切り)。この stem 規約は web プロファイルと共有される可搬形式です([第9章](09_web.md))。

### 自動リフレクション

SPIR-V を spirv-reflect で解析し、**descriptor set layout・pipeline layout・push constant range を全自動生成**します。頂点+フラグメントで binding の型が食い違うと起動時エラー。パイプラインキャッシュは exe と同じディレクトリの `pipeline_cache.bin` に永続化されます。

### ホットリロード(✅WP14)

ウィンドウモードで実行中、シェーダソースを保存すると約 1 秒のポーリングで検出され、再コンパイル → パイプライン再構築されます。**コンパイル失敗時は旧版を維持**して WARNING を出すだけなので、編集途中の保存でクラッシュしません(シェーダコンパイルだけが fail-fast の例外とされている理由)。ヘッドレスでは無効です。

### descriptor set 規約(shader_contract.md が正)

GLSL からは `#include "pelican_sets.glsl"` で定数を共有します(C++ 側 `pelican_sets.hpp` と同値)。

| set | 定数 | 用途(現行実装) |
|---|---|---|
| 0 | `PELICAN_SET_FRAME` | エンジン管理。※現状はパイプラインごとに中身が異なる(material/shadow = ObjectBuffer SSBO、lighting = LightUBO)。「フレーム共通(カメラ・時間)」への統一は 📐M2b 予定 |
| 1 | `PELICAN_SET_PASS_INPUT` | パス入力。fullscreen の `input` 配列順に binding 0..、compute の reads/writes も set 1 |
| 2 | `PELICAN_SET_MATERIAL` | マテリアルテクスチャ(0 baseColor / 1 metallicRoughness / 2 normal / 3 emissive、VAT 時 4-5)。params UBO は 📐未実装 |
| 3 | `PELICAN_SET_FREE` | 自由枠(debug_draw / debug_text が使用) |

push constant は合計 128B(エンジン 64B + シェーダ 64B の取り決め。ただし現状 enforcement はなく、カスタムシェーダ作者が衝突を避ける責務)。エンジン頂点レイアウトは location 0=`inPos`, 1=`inNormal`, 2=`inTexUV`, 3=`inColor`, 4=`inTangent` です。

エンジン同梱シェーダ(`engine://` で参照可): `fullscreen`(ライティング)/ `ssao` / `ssao_blur` / `bloom_highpass` / `bloom_blur_h` / `bloom_blur_v` / `bloom_composite` / `tonemap` / `shadow_depth` / `debug_draw` / `debug_text` / `default`(material 標準)/ `vat` / `ui` など。

📐設計のみ: HLSL / Slang 対応、リフレクション由来の自動頂点レイアウト。

## 6.4 feature — パージ可能な GPU 機能(✅WP28〜31/54)

> **設計決定(参照 = 存在):** feature は rendering config に**参照を書いたときだけ**存在する。1 つも書かなければ合成機構ごと素通りし、挙動は完全に不変(パージ可能)。エンジンに機能を「埋め込まない」ための中核原則で、golden テストの「feature off = 既存出力の完全維持」で保証されている。

使い方は 1 行です:

```json
{
  "features": [
    "engine://features/shadow_directional.json",
    "engine://features/debug_draw.json"
  ],
  ...
}
```

エンジン同梱 feature(5 個・✅すべて実装済み):

| feature | 内容 |
|---|---|
| `hdr.json` | `lit_color` を R16G16B16A16F 化し、トーンマップパスを挿入 |
| `shadow_directional.json` | 2048×2048 のシャドウマップ+`shadow_depth` パスを挿入し、`lighting_pass` に入力を追加 |
| `debug_draw.json` | ワイヤフレームオーバーレイ(collider 可視化など)を末尾に挿入 |
| `debug_text.json` | ビットマップ文字 HUD([第7章](07_input_ui.md)) |
| `gpu_timing.json` | パスなしの計測フラグ。GPU パス単位タイムスタンプをログに出力 |

feature の実体は `pelican.render_feature` v1 の JSON(fragment)です。実物(`shadow_directional.json` 全文):

```json
{
  "schema": "pelican.render_feature",
  "version": 1,
  "name": "shadow_directional",
  "render_targets": [
    { "name": "shadow_map", "extent_scale": 1.0, "width": 2048, "height": 2048,
      "format": "D32_SFLOAT", "usage": ["DEPTH_STENCIL_ATTACHMENT", "SAMPLED"] }
  ],
  "passes": [
    { "insert": "before:lighting_pass",
      "pass": {
        "name": "shadow_depth", "type": "shadow_depth",
        "output": { "color": null, "depth": "shadow_map" },
        "depth_store_op": "store",
        "shader": { "vertex": "engine://shadow_depth" }
      } }
  ],
  "pass_overrides": {
    "lighting_pass": { "input": ["shadow_map"] }
  },
  "shader_defines": ["PELICAN_FEATURE_SHADOW"]
}
```

fragment に書けるもの: `render_targets` / `buffers` / `compute_tasks`(追加。名前衝突はエラー)、`render_target_overrides`(既存 RT の format/usage/width/height を上書き)、`passes`(`insert: "before:<パス名>" | "after:<パス名>" | "end"` で挿入)、`pass_overrides`(既存パスへの input 追加のみ)、`shader_defines`。

> **設計決定(アンカー = 明示エッジの糖衣):** `insert: "before:present"` で挿入されたパスには自動的に `before: ["present"]` エッジが付与される。フレームグラフのプランナが賢くなっても「挿入の意図」が契約として残る。

シェーダ側の合流は `#ifdef` バリアント方式です。feature を参照しなければ define が付かない = シェーダからも宣言・命令ごと消えます。

```glsl
#ifdef PELICAN_FEATURE_SHADOW
layout(set = PELICAN_SET_PASS_INPUT, binding = 6) uniform sampler2D shadowMapSampler;
#endif
```

feature を差し替えたいときは fragment をプロジェクト内にコピーし、参照パスを書き換えます(engine:// のシャドーイングはできないため)。

⚠️ 既知の罠: 同梱 `hdr.json` のアンカーは `before:present` ですが、**example の config に `present` という名前のパスは存在しません**(最終パスは `FinalBloomComposite`)。example にそのまま hdr を足すと「insert anchor was not found: present」で起動エラーになります。標準アンカー名の慣習はまだ文書化されていません(ポストプロセス設計 📐 で標準化予定)。

## 6.5 フレームグラフ(✅F0〜F2 = WP33〜35)

> **設計決定(三層モデル):**
> 1. **正しさ** — データ依存(reads/writes)と明示エッジ(after/before)**だけ**が順序の契約。手書きバリアは禁止(バリア・layout 遷移は全部エンジンが導出する)。
> 2. **機械最適化** — トポロジカルソート・層別・バリア導出はエンジンの仕事。
> 3. **手詰め** — JSON の宣言順は「正しさ」ではなく**安定タイブレーク**。依存が許す範囲で宣言順が尊重されるので、配列の並べ替えは安全な調整手段になり、依存に反する並べ替えは「単に効果がない」。強い意図は after/before で形式に残す。

依存はパスの `input` / `output`(+ `color_load_op: "load"` は「前内容を読む」ので reads 扱い)から自動導出されます。順序が決められない writes-writes 競合は**起動時 hard error**(「Ambiguous writes-writes dependency ...」)で、明示エッジか中間リソースで解消します。

### compute タスク

```json
{
  "buffers": [ { "name": "compute_color", "size": 16, "lifetime": "persistent" } ],
  "compute_tasks": [
    { "name": "write_color", "shader": "shaders/write_color",
      "writes": ["compute_color"], "before": ["present"],
      "dispatch": { "groups": [1, 1, 1] }, "schedule": "per_frame" }
  ]
}
```

- compute シェーダは `<stem>.comp`。リソースは **set 1** に置くのが契約で、binding は「変数名がリソース名と一致すれば名前優先、それ以外は binding 順」でマップされます。
- `reads` / `writes` にはバッファ名も RT 名も書けます(RT は `STORAGE` usage が必要)。順序は書かず、依存宣言だけで並びます。
- v1 の制限: `schedule` は `per_frame` のみ。`dispatch.groups_from` はパースされますが**実行時未使用**(予約)。非同期 compute キューは 📐将来。
- 完全な最小プロジェクト例(compute が SSBO に色を書き、fullscreen が表示)がテスト `test/run_compute_headless.cmake` にあります。

### プランダンプ(実行計画の可視化)

> **設計決定:** 「機械が何をしたか見えないものは手で詰められない」。実行計画は `pelican.frame_plan` v1 の JSON として取り出せ、プラン比較テスト(実行計画版のゴールデン)で回帰保護される。

```sh
pelican_player --project mygame --headless --dump-frame-plan   # stderr に出力
# または rpc の get_frame_plan
```

出力にはノードごとの `order` / `level`(並列可能な層)/ `reads` / `writes` と、導出された `barriers` が含まれます。

### ⚠️ 実行系の現状(WP64 = 🚧登録済み・未実施)

プランは全構成で起動時に必ず作成・検証されますが、**実行**は二経路あります: compute タスクを 1 つでも含む構成はプラン順で実行、**純 render 構成は JSON 宣言順の legacy 経路**で実行されます(F0 の検証により既存構成では導出順 = 宣言順なので観測上は同じ)。legacy 経路の削除(A/B 差分ゲート付き)が WP64 として登録されています。現在のブランチ名 `codex/rendering-phase1-refactor` はこのリファクタリング系列の統合ブランチです。

## 6.6 マテリアルの現状

- 現行のマテリアルは **glb ロード経路で決まります**: glTF の pbrMetallicRoughness からテクスチャ(set 2)が登録され、標準シェーダ(`engine://default`)で描かれます。rendering config の material パスにシェーダは書けません。
- `pelican.material` v1 のパーサは ✅WP58(M1)で実装済みですが、**レンダラーには未接続**です(パース結果を消費するコードがまだない)。「マテリアル JSON が書ける」段階ではない点に注意してください。
- `.surface` コンテナ・シェーダフック梯子・spv-link は 📐設計のみ(M2 以降。[../design_material_shading.md](../design_material_shading.md) v1.2)。

## 6.7 ポストプロセス/テンポラル(📐設計のみ)

history リソース(`"history": true`)・velocity feature・TAA・モーションブラー・標準ポストスタックアンカーは [../design_postprocess_temporal.md](../design_postprocess_temporal.md) にある設計のみで、**実装はゼロ**です(パーサに `history` キーは存在しません)。現状でブルームや SSAO を組みたい場合は example のように通常の fullscreen パスとして書きます。

## 6.8 ヘッドレス描画とテスト(✅WP1〜8/16)

```sh
pelican_player --headless --project mygame --frames 3 --size 1280x720 --render-out out.png
```

- ウィンドウなしで描画し PNG を出力します。パス JSON の `"swapchain"` 出力は自動的にオフスクリーンイメージへ解決されるので、**config は無変更で動きます**。
- ゴールデンイメージテスト(基準画像との比較)とプラン比較テストが回帰保護の柱です。検証の流儀は [../rendering_phase1_review.md](../rendering_phase1_review.md)(Validation Run)を参照してください。

> **設計決定(レイヤ規則):** `src/core/vkcore`(Vulkan 低層)から `src/core/renderingpass` 以上のレイヤへ include を追加しない。下位層はフラグ(例: リサイズ発生)を上げるだけで、編成(RT 再生成・再バインド)は上位層 Renderer が行う。

## 6.9 トラブルシューティング

| エラー(抜粋) | 原因 |
|---|---|
| `Default rendering pass does not output to swapchain` | `default_rendering_pass` のパス列が画面に出力していない |
| `uses an explicit file extension; use an extensionless ... shader stem` | shader 参照に拡張子を書いた |
| `Shader stem could not be resolved: ... Tried: ...` | stem のパスミス(試行一覧がエラーに含まれる) |
| `insert anchor was not found: <name>` | feature の挿入先パス名が config に無い(§6.4 の hdr の罠) |
| `Ambiguous writes-writes dependency for resource X between A and B` | 同一リソースへの書き込み順が導出不能。after/before か中間リソースで解消 |
| `Shader defines require GLSL source, not SPIR-V` | .spv しか無い環境で feature(defines)を使った |
| `Shader descriptor binding mismatch` | vert/frag で同一 binding の型が不一致 |

## 関連文書

- [../shader_contract.md](../shader_contract.md) — シェーダ契約(set 規約・push constant・頂点レイアウト)の正
- [../design_shader_freedom_kit.md](../design_shader_freedom_kit.md) — シェーダ基盤 [SF]。実装済み
- [../design_render_feature_modules.md](../design_render_feature_modules.md) — feature 合成(実装済み)
- [../design_compute_task_graph.md](../design_compute_task_graph.md) — 統一フレームグラフ(F0〜F2 実装済み)
- [../design_headless_rendering.md](../design_headless_rendering.md) — ヘッドレス描画 [HL]。実装済み
- [../design_material_shading.md](../design_material_shading.md) — pelican.material(M1 のみ実装)
- [../design_postprocess_temporal.md](../design_postprocess_temporal.md) — history/velocity(設計のみ)
- [第5章 アセット](05_assets.md) / [第9章 Web プロファイル](09_web.md) / [第10章 ツールリファレンス](10_tools.md)
