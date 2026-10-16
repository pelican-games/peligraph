# レンダリング preset の feature レシピ化: 追随と分岐の単位を feature に揃える

対象読者: エンジン担当。
ステータス: 改善案メモ(2026-10-12。議論の記録。WP 化・レビュー前)。
前提: [design_render_feature_modules.md](design_render_feature_modules.md)(feature fragment の 3 要求)、
[design_render_pipeline_extensibility.md](design_render_pipeline_extensibility.md) §3(preset / eject の段階)、
WP240a(既定 rendering config の preset 化)、マニュアル [第6章 preset の節](manual/06_rendering.md#render-preset)。

## 0. 要約

- 「エンジンの改善に project が自動追随する」単位は、preset(pipeline 全体)ではなく **feature** であるべき。
- feature には追随(`engine://` 参照)・分岐(`materialize`)・依存宣言(`required_targets`)・差し替え(`pass_replacements`)が
  既にある。preset だけが一枚岩で、分岐が `fork_render_pipeline`(全体コピー・追随放棄)の一択になっている。
- 提案: **preset を「構造」ではなく「feature の一覧 + pipeline 全体の方針」にする。**
  `hybrid_v1` のパスと target を engine feature(`deferred_base` / `forward_base` / `present`)へ切り出し、
  project は feature 単位で追随・分岐・除外・差し替えを選ぶ。direct root(手書き)は残す。

## 1. 現状と証拠

### 1.1 preset の合成規則(v1)

[`resolveRenderPipelinePreset()`](../src/project/renderpipeline.cpp#L888) の規則。コメントは
*"Keep v1 deterministic. Structural overrides use an explicit eject/copy workflow so a preset update cannot
silently reinterpret a deep merge."*

| 操作 | 可否 |
|---|---|
| root の `features` / `shader_defines` / `graph_transforms` を preset へ追記 | 可 |
| root の `draw_sort` で preset を置換 | 可 |
| `render_strategy` / `snapshots` / `target_planning` / `vulkan_plan_pins` / `vulkan_physical_fragments` | preset が持っていれば root から不可 |
| preset を選ぶ root に `render_targets` / `rendering_passes` を併記 | 不可(hard error) |
| preset から preset の参照 | 不可 |

意図は正しい(deep merge は preset の次の版で意味が黙って変わる)。問題は粒度。

### 1.2 使われ方

- 同梱 project 6 本のうち preset を使うのは `animgraph_demo` / `posegraph_demo` の 2 本(自前パス 0)。
  `example` / `sprite_demo` / `vrm_xr_demo` は direct root。WP240a も「example は変換しない(機能を失う)」と明記。
- engine preset は `hybrid_v1` の 1 本だけ。
- preset は `features` を持てないので、「標準の見た目 = hybrid_v1 + shadow_directional + sky_ambient」を
  project ごとに列挙している(`projecttemplate.cpp` の `rendering_config_json`)。
- 中間の自由度を担う機構は preset のパスに届かない:
  `pass_replacements` は feature のパスだけ、`graph_transforms` / `render_strategy` / subgraph replacement は
  provider(C++)が要る。
- `fork_render_pipeline` は `base_preset_digest` を provenance に残すが、元 preset の更新を取り込む手段は無い。

### 1.3 feature には既にある

| 要求 | feature | preset |
|---|---|---|
| エンジン更新への追随 | `engine://features/*.json` の参照 | `engine://render_pipelines/*.json` の参照 |
| 分岐 | `materialize`(1 本を project 派生に。`engine_default` / `bound_parameters` を保持) | `fork_render_pipeline`(全体コピー) |
| 依存の宣言 | `required_targets`(型付き。無ければ FR-1 で名指しで断る) | なし |
| 差し替え | `pass_replacements` | なし |
| 位置 | `insert: after:<anchor or pass>`(相対) | 絶対順序 |

`design_render_feature_modules.md` §0 の 3 要求(パージ可能 / 1 行有効化 / 差し替え可能)は feature に対して
定義されている。preset は WP240a で後から足された「既定構成の束」で、粒度の不整合は歴史的経緯。

## 2. 提案

```text
今:   preset = { render_targets, rendering_passes, draw_sort, material_routing, shader_defines }
      追随は all-or-nothing。分岐は fork(全体)。

案:   hybrid_v1 = {
        "features": [
          "engine://features/deferred_base.json",   // gbuffer 5 枚 + deferred_geometry + ssao ×2 + deferred_lighting
          "engine://features/forward_base.json",    // forward_opaque + opaque snapshot ×2 + forward_transparent
          "engine://features/present.json",         // scene_present
          "engine://features/shadow_directional.json",
          "engine://features/sky_ambient.json"
        ],
        "material_routing": ..., "draw_sort": ..., "shader_defines": ...   // pipeline 全体の方針だけ
      }
      project は feature 単位で: 追随(参照のまま)/ 分岐(materialize)/ 除外(exclude)/ 差し替え(自作 feature)
```

- preset の `rendering_passes` / `render_targets` は空(または禁止)になり、preset = **レシピ**。
- root の `features` は preset の `features` の**後ろに追記**(配列の追記なので一意)。
- root に `exclude_features: [...]` を足し、preset の feature を名前で外せる(無い名前は名指しで断る)。
- 分岐は feature 単位の `materialize`。`fork_render_pipeline` は「全 feature を materialize し root を direct にする糖衣」として残す。
- direct root は変更しない(手書き派の経路。example はそのまま)。

決定性・fail-fast との整合: 合成は「配列の追記 + 名前による除外」だけで、位置の意味が preset の版に依存しない。
feature 間の名前結合(`after:deferred_lighting`)は、名前が新版で消えれば**名指しで落ちる**。黙って意味が変わる経路は増えない。

## 3. 埋めるべき穴(論点)

1. **scene 段の anchor が無い。** canonical anchor 8 個(`sprite` / `post_main` / `tonemap` / `post_ldr` / `pelican_ui` /
   `debug_draw` / `debug_text` / `imgui`、[featurecompose.cpp](../src/project/featurecompose.cpp#L33))は後段だけ。
   gbuffer → lighting → forward は「scene passes(anchor より前、宣言順)」扱い。base を feature に分けると
   feature 間の順序をパス名への結合で書くことになる。`geometry` / `lighting` / `forward` のような scene 段 anchor を
   足すと疎結合になる(anchor の全順序は凍結面なので、追加は版の扱いを決めてから)。
   → 2026-10-16 の議論で、anchor 自体を「段タグ + Studio の `Show:` フィルターの階層」へ移す案に発展した。
   [design_render_stage_as_canvas_filter.md](design_render_stage_as_canvas_filter.md) を参照(scene 段は anchor ではなく `stage` の値として足す)。
2. **pipeline 全体の方針の置き場。** `material_routing` / `draw_sort` / `shader_defines` / `target_planning` /
   `xr` は feature が持てない。薄い preset に残す(小さい)か、「policy feature」種別を作るか。
3. **render target の所有。** gbuffer 5 枚は `deferred_base` が宣言し、lighting / bloom / forward は
   `required_targets` で要求する。bloom が `lit_color` に対して既にやっている形(`bloom.json` の `required_targets`)。
   `material_contract`(`deferred_geometry_v1` 等)が要求する枚数・format の検査はそのまま効く。
4. **materialize の既知の弱点。** `render_targets` と `shader_assets` の宣言は派生時に食べられ、選択肢が固定される
   (マニュアル §6 の materialize の段)。base を materialize する運用が増えるので、ここを先に直すか、
   base feature は `render_targets` の選択肢を持たない形にするか。
5. **Studio / RPC の前提。** `edit_preset_pass`、`fork_render_pipeline`、FR-1〜3 の「preset が target を持つ」前提の
   修復提案、Render Features の `(preset)` 表示。レシピ化後は「preset のパス編集」=「その feature を materialize して編集」。
6. **web 鏡像(サブセット原則)。** feature 合成は純ロジックで web 側も同じ合成を持つ前提。分解した base feature と
   scene 段 anchor を web が読めることの確認。
7. **凍結面。** `pelican.render_pipeline` v1 の意味が変わる(`rendering_passes` を持たない preset)。v0 の
   `(schema, version)` 一覧([v0_freeze.md](v0_freeze.md) §1.1)に対する扱い — version 2 にするか、v1 のまま
   「空でもよい」とするか。`hybrid_v1` の content hash を provenance に持つ fork 済み project への影響。

## 4. 段階

| 段 | 内容 | 変更範囲 | 検証 |
|---|---|---|---|
| 1 | preset が `features` を持てる。root の `features` は後ろへ追記、`exclude_features` で除外。雛形(`projecttemplate.cpp`)を 1 行に | `src/project/renderpipeline.cpp`(`resolveRenderPipelinePreset`)、`featurecompose.cpp`、template | 純ロジックの fixture テスト、`devcli_project_init_command`、golden 非回帰 |
| 2 | `hybrid_v1` のパス・target を `deferred_base` / `forward_base` / `present` の engine feature に切り出し。scene 段 anchor を追加。`hybrid_v1` は feature list + 方針だけに | `src/core/resources/features/`、`featurecompose.cpp`(anchor)、`engineresources.cpp`、web 鏡像 | preset 経路の headless(`animgraph_demo_preset_headless_player`)と golden が不変 |
| 3 | `edit_preset_pass` 系を「materialize して編集」へ寄せる。`fork_render_pipeline` を糖衣に。materialize の `render_targets` / `shader_assets` 固定を解消 | `renderconfigeditor.cpp`、Studio の Frame Plan / Render Features | Studio scenario、RPC テスト |

1〜2 は純ロジック中心で、GPU 無しの環境でもテストまで回せる。3 は Studio(Windows / Qt)が要る。

## 5. 代替案(比較用)

- **A. preset が既定 `features` を持てる**だけ(= 段 1 のみ)。小さいが、base の分岐は依然 fork。
- **B. root から preset パスの `pass_replacements`**(名前指定で置換・無効化)。fail-fast には合うが、preset が構造を
  持ち続けるので「追随単位 = pipeline 全体」は変わらない。
- **C. fork の再同期**(`base_preset_digest` で 3-way merge)。provenance はあるが、衝突解決が利用者の仕事になり、
  feature 単位の分岐より重い。
- 本提案は A を含み、B / C を不要にする。

## 6. 未決

- scene 段 anchor の名前と全順序(凍結面への影響と版の扱い)。
  anchor を `stage` タグに変える案([design_render_stage_as_canvas_filter.md](design_render_stage_as_canvas_filter.md))を採るなら、全順序の凍結面そのものを外す話になる。
- `material_routing` の置き場(薄い preset か policy feature か)。
- `pelican.render_pipeline` の版(v1 のまま「構造を持たない preset」を許すか、v2 にするか)。
- 分解した base feature の粒度(deferred と ssao を分けるか、lighting と sky を分けるか)。
