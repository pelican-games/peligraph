# レンダリング preset の feature レシピ化: 追随と分岐の単位を feature に揃える

対象読者: エンジン担当。
ステータス: 改善案メモ(2026-10-12。議論の記録。WP 化・レビュー前)。
**2026-10-27 追記: RC-5(RC-5a = 4001e48af、RC-5b = 0f0cea36b)で一部を実装した。実際に入った形は本文の案と違う(base は feature ではなく
構造の断片)。入った形・本文から変わった所・§1.2 と §6 の訂正は末尾の [§7 実装(RC-5、2026-10-27)](#rc5-implementation) を正とする。**
§0〜§5 は議論の記録として残す。
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
| `render_strategy` / `snapshots` / `target_planning` / `vulkan_plan_pins` / `vulkan_physical_fragments` | preset が持っていれば root から不可(後の 2 つは RC-6b2 で撤去。§7.10) |
| preset を選ぶ root に `render_targets` / `rendering_passes` を併記 | 不可(hard error) |
| preset から preset の参照 | 不可 |

意図は正しい(deep merge は preset の次の版で意味が黙って変わる)。問題は粒度。

### 1.2 使われ方

- 同梱 project 6 本のうち preset を使うのは `animgraph_demo` / `posegraph_demo` の 2 本(自前パス 0)。
  `example` / `sprite_demo` / `vrm_xr_demo` は direct root。WP240a も「example は変換しない(機能を失う)」と明記。
- engine preset は `hybrid_v1` の 1 本だけ。
- preset は `features` を持てないので、「標準の見た目 = hybrid_v1 + shadow_directional + sky_ambient」を
  project ごとに列挙している(`projecttemplate.cpp` の `rendering_config_json`)。
  - **訂正(2026-10-27、RC-5 の調査)**: 「preset は `features` を持てない」は誤り。RC-5 の前から、preset の `config.features` は
    合成できる(preset の配列を起点に root の `features` を後ろへ足し、JSON 値の完全一致で重複を省く。§1.1 の表の 1 行目と同じ規則)。
    事実なのは「出荷の `hybrid_v1` が `features` を持っていない」ことと「root の `exclude_features` は無い」こと。
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

## 6. 未決(2026-10-17 更新)

議論で決まったもの:

- **scene 段 anchor の名前と全順序** → anchor は `stage` タグへ([段メモ](design_render_stage_as_canvas_filter.md))。全順序の凍結面は v1 で外す。
  scene 段の数は段メモ §7 の通り 3 段(`geometry` / `lighting` / `forward`)を推す。
- **`material_routing` の置き場** → preset の「pipeline 全体の方針」側。[棚卸しメモ](design_plan_layer_inventory.md) 2-6 で段階 2 を
  「typed 論理 graph + policy」に育てると決めたので、policy は feature ではなく段階 2 の入力として preset が運ぶ。
- **分解した base feature の粒度** → scene 段に揃える(`deferred_geometry` / `deferred_lighting` / `forward_base`)。ssao と sky は既に feature なので分けたまま。
  - **訂正(2026-10-27、RC-5 の調査)**: SSAO は feature ではなかった。`ssao_pass` / `ssao_blur_pass` の 2 pass と `ssao_output` / `ssao_blur` の
    2 target は `hybrid_v1.json` の中にあり、同梱の feature に SSAO は無い(sky は `sky_ambient` の feature)。RC-5 は SSAO を
    構造の断片 `ssao` として切り出した(feature にはしていない。§7.2)。

残るもの(**2026-10-27 に決着。§7.5**):

- **`pelican.render_pipeline` の版**。構造を持たない preset と `stage` は `@1` の読み手を壊すので `@2` にし、`@1` は読み替えで受ける案を推す。
  v1 ブランチの凍結面の方針(アンカーの全順序を外すこと)と一緒に決める。

<a id="rc5-implementation"></a>

## 7. 実装(RC-5、2026-10-27)

RC-5a(4001e48af)と RC-5b(0f0cea36b)。branch `agent/rc5`。仕様は `<作業ディレクトリ>/rc5_spec.md`(本文 → 追記 1〜4。後の追記が正)、
実装者の報告は `<作業ディレクトリ>/rc5a_report.md` / `<作業ディレクトリ>/rc5b_report.md`。数字は報告から写した。

### 7.1 利用者の決定(2026-10-27)

1. `pelican.render_pipeline@1` の読み手と `engine://render_pipelines/hybrid_v1.json` は今のまま。レシピは `@2` の新しい preset。
2. 何を既定の preset にするかは後で決める。レシピの preset は**実験として作る**。新しい project の雛形(`projecttemplate.cpp`)は今のまま。
3. `fork_render_pipeline` は今のまま(選んだ preset の原文を project のファイルへ写し、root の参照を切り替える)。
4. stage は engine の語彙にしない。カスタム可能な階層フィルター(Studio の `Show:` の拡張。GF-1 と一体)の 1 例として、preset などに
   付いてくるデータにする。Studio 側の別の WP。レシピの preset がフィルターの定義を持つ欄も今は作っていない。
5. canonical anchor の撤去(順の骨格・`__anchor_*` のノード・sprite の本物の pass 化)は RC-5s(別の段)。

### 7.2 入った形: base は feature ではなく「構造の断片」

- **`pelican.render_pipeline@2`(レシピ)**: `config` に書けるキーは 11 個 —— `base`・`features`・`graph_transforms`・`shader_defines`・
  `draw_sort`・`material_routing`・`xr`・`render_strategy`・`target_planning`・`vulkan_plan_pins`・`vulkan_physical_fragments`(RC-6b2 で後の 2 つが外れて 9 個。§7.10)。
  `render_targets` / `rendering_passes` / `snapshots` / `buffers` / `compute_tasks` は `render_pipeline.recipe_has_structure@1`(`key`)、
  ほかのキーは `render_pipeline.recipe_unknown_key@1`(`key`)で断る([`expandRenderPipelineRecipe()`](../src/project/renderpipelinerecipe.cpp#L65))。
  `features` 以下の 10 個の合成と root からの上書きの可否は `@1` と同じ(報告 §2 が `@1` の resolver の表と照らした)。RC-6a1 から `target_planning` だけは違う(root が葉ごとに上書きする。§7.8)。
- **`base: [{family, fragments: [ref …]}]`**: family ごとに、断片の `render_targets` を書いた順に連結し、`passes` を書いた順に連結して、
  `@1` の `config` と同じ形の `render_targets` と `rendering_passes: [{name: family, passes}]` を作る。展開した後は `@1` の道
  (正規化・root の合成・canonical・HDR・feature の挿入・Graph・IR)を**そのまま**通る([`resolveRenderPipelinePreset()` の版の分岐](../src/project/renderpipeline.cpp#L953))。
  展開の結果に `base` と root の `fragment_overrides` は残らず、レシピに無いキーは補わない(`features` の無いレシピは `features` を出さない)。
  family は 1 つ以上で名前の重複は断る。展開した base 全体で pass 名と target 名が重なれば `render_pipeline.recipe_duplicate_name@1`
  (`kind`: `pass` / `target`・`name`・`fragments`)。Graph の pass の既定の family は、展開した `rendering_passes` の先頭(今の規則のまま)。
- **構造の断片 `pelican.render_fragment@1`**: `{schema, version, name, render_targets, passes, required_targets?}`。RC-6a1 から `target_planning?` も書ける(§7.8)。`insert` も `parameters` も
  持たない(位置は `base` の列の順)。`required_targets` は target 名の文字列の配列で、展開した base 全体の target の宣言に無ければ
  `render_pipeline.fragment_missing_target@1`(`fragment`・`target`)。形の不正は `render_pipeline.fragment_invalid@1`、レシピの形の不正は
  `render_pipeline.recipe_invalid@1`(どちらも `field`・`reason`)。
- **なぜ feature にしなかったか**(仕様の追記 1。仕様レビューの指摘 1〜3): base を普通の feature(pass ごとに `insert` で差し込む)にすると、
  (1) canonical の振り分けが feature の挿入より前にある、(2) HDR のときは feature の pass を足す前に swapchain を書く scene の終端を探す
  (base を feature に移すとその時点に終端が無い)、(3) `insert` の並びの規則(同じ pass への `after` は後から入れた方が前)、の 3 つとぶつかり、
  `hybrid_v1` と同じ順と依存を再現できない。そこで base は preset の構造に展開してから `@1` と同じ道を通す形にした。
- **engine の断片 5 つ**(`engine://render_fragments/*.json`。中身は `hybrid_v1.json` の該当の target と pass を JSON 値のまま写した):

  | 断片 | target / pass の数 | 中身 | `required_targets` |
  |---|---|---|---|
  | `deferred_geometry` | 6 / 1 | G-buffer 5 枚 + `scene_depth`、pass `deferred_geometry` | 無し |
  | `ssao` | 2 / 2 | `ssao_output` / `ssao_blur`、pass `ssao_pass` / `ssao_blur_pass` | `gbuffer_worldpos`・`gbuffer_normal` |
  | `deferred_lighting` | 1 / 1 | `lit_color`、pass `deferred_lighting` | G-buffer 5 枚・`ssao_blur` |
  | `forward_base` | 2 / 4 | `opaque_color` / `opaque_depth`、pass `forward_opaque` / `__snapshot_opaque_color` / `__snapshot_opaque_depth` / `forward_transparent` | `lit_color`・`scene_depth` |
  | `present` | 0 / 1 | pass `scene_present` | `lit_color` |

- **実験の preset 2 本**(雛形と出荷 project は使わない): `engine://render_pipelines/experimental/hybrid_recipe.json`(`base` = `main_render` に
  上の 5 つを `hybrid_v1` の並びで、方針は `hybrid_v1` と同じ、`features` キー無し)と `…/experimental/hybrid_recipe_lit.json`
  (同じ + `features` = `shadow_directional` → `sky_ambient`)。
- **provenance**: pass / 資源の `source`(project / feature / engine)とそれを決める規則は変えていない。断片から来た pass と render target に
  だけ、任意の `fragment: {name, reference}` を付ける([付ける所](../src/project/featurecompose.cpp#L3507))。`get_frame_plan` の node と
  resource に出る。anchor・Graph・普通の feature・生成資源には付かず、資源計画の `ResourcePattern.provenance` と fingerprint には入らない。
- **等価の結果**(報告): 展開の直後の config は `hybrid_v1` の config と JSON で一致(lit は「`hybrid_v1` + root の shadow / sky」と一致)。
  bare / lit × HDR on / off × project の feature と Graph の node の有無 × flat / XR の 16 構成で、typed IR の全体・3 種の fingerprint・
  FramePlan の全 JSON・物理計画の全 JSON が一致。差は各構成 25 の JSON pointer(node / resource の `fragment` 欄と、preset の文書の識別
  `pipeline_preset.{name, ref, version}` と診断 `pipeline_preset_resolved`)だけ。旧構成で作った pin / physical fragment(RC-6b2 で撤去)は新構成にそのまま
  適用できた。golden 5 ケースと `animgraph_demo` の preset の headless の PNG が byte 一致。

### 7.3 root の `fragment_overrides`(base の断片の置き換えと除外)

- `@2` の preset を選ぶ root だけが書ける: `fragment_overrides: {"<断片の name>": "<参照>" | null}`。参照なら **base の中の同じ位置で**
  置き換え、`null` なら除外する。置き換え先の文書の `name` はキーと同じでなければならない。
- 拒否: base に無い名前は `render_pipeline.fragment_override_unknown@1`(`name`・`inherited` = base の名前の一覧)、置き換え先の名前違いは
  `render_pipeline.fragment_override_name_mismatch@1`(`name`・`replacement_name`)、base に同じ名前の断片が 2 つあるレシピは
  `render_pipeline.recipe_duplicate_fragment@1`(`name`)、`@1` の preset を選ぶ root と preset を選ばない root の `fragment_overrides` は
  `render_pipeline.fragment_overrides_need_recipe@1`。
- 等価を保証するのは「同じ中身の断片への置き換え」(materialize)だけ。除外や中身の違う断片への置き換えで足りなくなる物は、自動では補わず
  今の名前付きの拒否のまま(例: `ssao` を `null` にすると `render_pipeline.fragment_missing_target@1`、`fragment: deferred_lighting`・
  `target: ssao_blur`)。material routing が要る pass 名(`deferred_geometry` / `forward_opaque` / `forward_transparent`)を消す置き換えは
  今の routing の拒否になる(仕様の決め事。これを通す test は報告に無い)。
- 本文 §2 の `exclude_features`(feature の除外)と、仕様の本文にあった feature の instance の置き換え(`feature_overrides`)は作っていない。
  レシピの `features` は今の合成のまま(root の `features` を後ろへ足すだけ)。

### 7.4 断片を 1 本だけ project に分岐する(RC-5b)

- `materialize_render_fragment {name, base_source_digest, base_preset_digest, base_fragment_digest, destination_reference}`
  ([`RenderConfigEditorService::materializeRenderFragment()`](../src/core/communication/renderconfigeditor.cpp#L3724)): 実効の断片の bytes を
  そのまま project の新しい文書に写し、root の `fragment_overrides` に `{name: destination}` を書く。文書の作成と root の書き換えは
  1 つの transaction。preset は fork しない。ほかの断片は engine の参照のまま(engine の更新に追随する)。
- 読み: `get_render_features` の `base_fragments`([組み立て](../src/core/communication/renderconfigeditor.cpp#L1459))、
  `get_render_authoring_context` の pass ごとの `editable_fragment_pass` / `fragment` / `fragment_declaration`。
- 編集: `edit_preset_pass` の所有の digest に `base_fragment_digest` を足した(所有の digest は高々 1 つ。project の断片の pass だけを書く)。
  追随に戻すのは `edit_rendering_keys` で `fragment_overrides` の行を消す(新しい RPC は足していない。project の断片の文書は残る)。
- 文書の列挙・復元(`get_render_documents` / `restore_render_documents`)に `kind: "fragment"` を足し、Studio の render の編集の履歴に
  `materialize_render_fragment` を登録した。Studio は Render Features の **Base fragments**(Following / Branched / Excluded、
  **Materialize** / **Return to inheritance**)と、Frame Plan・Graph・Pass Editor の断片の pass の編集。
- 断片は parameter を持たないので、本文 §3-4 の materialize の弱点(render target / shader asset の宣言が失われる)は断片には当たらない。
- 利用者向けの契約は manual [第6章「preset のレシピ」](manual/06_rendering.md#render-recipe)と [第13章「レシピの base の断片を分岐する」](manual/13_editor.md#recipe-fragment-materialize)。

### 7.5 本文との対応(決着と変更)

| 本文 | RC-5 の後 |
|---|---|
| §0 / §2 「`hybrid_v1` を engine feature に切り出し、preset = feature の一覧」 | 切り出し先は feature ではなく構造の断片(§7.2)。`hybrid_v1.json` は書き換えず、レシピは別の実験の preset |
| §1.2 「preset は `features` を持てない」 | 誤り(§1.2 の訂正)。RC-5 の前から合成できた |
| §2 `exclude_features` / 分岐は feature 単位の `materialize` | base の断片に対する `fragment_overrides`(`null` で除外)と `materialize_render_fragment`。feature の除外・置き換えは作っていない |
| §2 / §4 段 3 「`fork_render_pipeline` を全 feature の materialize の糖衣に」 | しない(利用者の決定 3)。fork は今のまま |
| §3-1 scene 段 anchor → `stage` タグ | stage は engine の語彙にしない(利用者の決定 4)。[段メモ](design_render_stage_as_canvas_filter.md)の冒頭の注記を参照。anchor の撤去は RC-5s |
| §3-3 target の所有 | 断片が target を宣言し、`required_targets` で要求する(§7.2 の表)。除外で足りなくなる物は自動で補わない(§7.3) |
| §3-4 materialize の弱点 | 断片は parameter を持たないので当たらない。feature の materialize の弱点は今のまま |
| §3-5 Studio / RPC | §7.4 |
| §3-6 web 鏡像 | 未確認(鏡像は別 repo)。この repo の engine の ID の台帳 `test/fixtures/project_format/engine_resources.json` には 7 文書を足した |
| §3-7 / §6 「`pelican.render_pipeline` の版」 | `@2` に決着。`@1` は読み替えずに今のまま読む(`@1` を書き換えないので移行の step は要らない)。`docs/v0_freeze.md` には新しい schema を足さない |
| §4 段 1 「雛形を 1 行に」 | していない(利用者の決定 2)。何を既定の preset にするかは後で決める |
| §6 「ssao は既に feature」 | 誤り(§6 の訂正)。`ssao` は構造の断片として切り出した |

### 7.6 残り

- RC-5s: canonical anchor の撤去(順の骨格・`__anchor_*`・sprite の本物の pass)。✅2026-10-28 に着地(§7.7)。報告の申し送り: canonical anchor・HDR の初期化・Graph の
  既定 family と配置・snapshot・IR → FramePlan → 物理計画は RC-5 で触っていない。変えるときは RC-5a の 16 構成の全 node・fingerprint・
  旧 package の適用の比較を保つこと。
- stage(カスタム可能な階層フィルター)= Studio 側の WP。既定の preset の決定と雛形の切り替え。web の鏡像。
- 細部(コードの読み): preset の版の検査の文は `@2` も受けるようになった今も `render pipeline preset version must be exactly 1` のまま
  ([検査](../src/project/renderpipeline.cpp#L941))。engine のレシピが `features` に持つ instance(`hybrid_recipe_lit` の shadow / sky)は、
  `get_render_features` で `scope: "preset"` の行になり、今までの engine preset の feature と同じく「先に fork」と案内される
  (`materializable: false`)。

<a id="rc5s-implementation"></a>

### 7.7 RC-5s(canonical anchor の撤去、2026-10-28)

develop_v1 の `1e05031d7` / `e17754510`。仕様は `<作業ディレクトリ>/rc5s_spec.md`(冒頭の利用者の決定と追記 1 が正)、報告は `<作業ディレクトリ>/rc5s_report.md`。
全体は [段メモ §8](design_render_stage_as_canvas_filter.md#rc5s-implementation) と [manual 第6章「canonical anchor」](manual/06_rendering.md#canonical-anchorwp73)。
レシピに関わる所だけ書く。

- **展開の後の道**: §7.2 の「`@1` と同じ道」から canonical anchor の振り分けと鎖が抜けた。レシピを展開した config も、`@1` の preset と同じく
  宣言順 → feature の `insert` の位置 → Graph の node の列と、資源の依存・`after` / `before`・`frame_node_order` で順が決まる。
  manual の「断片を feature にしなかった理由」(canonical の振り分けが feature の挿入より前にあること、など)は RC-5 の判断の時点の理由として残した。
- **断片の境界**: 断片を読むとき `canonical_anchor` の欄と `type: "canonical_anchor"` を `render_graph.canonical_anchor_removed@1` で断る
  ([`loadFragment()` で見る所](../src/project/renderpipelinerecipe.cpp#L38))。展開の後、実効の断片それぞれの `insert` が旧い anchor 名を指し、
  展開した config に同じ名前の pass が無ければ同じ code で断る([展開の後に見る所](../src/project/renderpipelinerecipe.cpp#L151))。preset の文書も
  同じ検査を通る([`resolveRenderPipelinePreset()` で見る所](../src/project/renderpipeline.cpp#L951))。同梱の断片(`present.json` など)に旧い書き方は無い。
- **§7.6 の申し送りへの答え**: RC-5 の 16 構成の比較は保った。typed IR / FramePlan / 物理計画 / 新しく eject した package の比較は
  基準が無くても常に走り、固定 HEAD(`c85717be8`)で作った complete fragment と pin の package は stale として断られる、に期待を変えて
  別の名前付きの test に分けた(「RC-5s fixed HEAD complete fragment and pin packages are stale」。基準 `RC5_BASELINE_DIR` が無ければ
  `rc5s.fixed_head_packages_unavailable@1` の名前付きの skip。`test/ci/cpu_skip_allowlist.txt` に登録)。物理計画の fingerprint が変わるのは
  意図した変化(RC-3d2 と同じ扱い)。報告では compiler の 16 構成が 16/16(OpenXR ON)、runtime も 8 flat + 8 の合成の stereo で 16/16。
- **sprite の資源計画**: レシピの 16 構成の test に、sprite の pass の IR の use・attachment・物理計画の rendering scope・barrier・lifetime の
  検査を足した(`rc5_recipe_compiler_test.inc` の「RC-5s sprite attachments participate in IR and physical planning」)。
- **stage**: §7.1 の決定 4 のまま。RC-5s でもレシピの preset にフィルターの定義を持つ欄は作っていない。

<a id="rc6a1-implementation"></a>

### 7.8 RC-6a1(部品とレシピの `target_planning`、2026-10-28)

`3fa229eba`(branch `agent/rc6`)。仕様は `<作業ディレクトリ>/rc6_spec.md` §1、報告は `<作業ディレクトリ>/rc6a1_report.md` と修正の報告 3 本。全体(強さ・出所・優先・結び付け・旧い package との同居の拒否・dump)は
[棚卸しメモ §2-3 の RC-6a1](design_plan_layer_inventory.md#rc6a1-implementation)、利用者向けは manual 第6章「target planning と Vulkan plan pin」。レシピと部品に関わる所だけ書く。
行番号は `3fa229eba` の物。

- **書ける所**: feature の文書、構造の断片 `pelican.render_fragment@1`(許すキーに `target_planning` を足した。[`loadFragment()` の鍵の検査](../src/project/renderpipelinerecipe.cpp#L42))、
  `@2` レシピの `config.target_planning` の 3 つ。形は root と同じ `{graphs: {<graph>: {nodes, resources}}, diagnostics?}`。`profile` は root だけ。
- **書ける名前(所有)**: 自分の文書が宣言した pass・compute task・render target・buffer の名前だけ。レシピの所有は、`fragment_overrides` を当てた後の展開した base が宣言する名前
  (= 選んだ断片の名前の和)。feature は自分の文書に、採った integration の fragment の `render_targets` / `buffers` / `passes` / `compute_tasks` を足した物が所有になる
  (feature 条件・pass 条件で採らなかった integration の名前は他人)。`required_targets` で借りた名前は所有に入らない。所有に無い名前は
  `target_planning.policy_foreign_name@1`(`document`・`pointer`・`target`。[検査](../src/project/renderpipeline.cpp#L925))。例(報告の B4): feature の文書が base の `lit_color` を指すと断る。
- **集める物**: 選んだ断片と、採って parameter を束縛した feature の文書だけ。`fragment_overrides` で置き換えた・外した断片の宣言は集めない(報告の B10)。
  レシピの宣言は展開の直後に別の出所として取り出し、展開した config からは `target_planning` を消す([取り出す所](../src/project/renderpipeline.cpp#L1231))。
  断片の宣言は断片ごとに集める([集める所](../src/project/renderpipelinerecipe.cpp#L131))。
- **強さと優先**: 裸の値はレシピも部品も `prefer`。同じ (graph, 対象, 鍵) は root > レシピ > 部品で、下は dump に `overridden` で残る。報告の B3 では、断片の `serial: true` に
  レシピの `serial: false` が勝ち、物理計画の `planning_opportunities.parallel_candidates` が policy の無い時と同じ集合に戻った。
- **`@2` の root の書き方(§1.1 の表からの変更)**: `@1` の「preset が `target_planning` を持てば root から上書き不可」は `@2` には当てない。`@2` の root は自分の `target_planning` を
  持て、レシピの宣言と葉((graph, 対象, 鍵))ごとに競って root が勝つ。editor の `edit_target_planning` も `@2` の preset を選ぶ root では `rendering_key_not_allowed_here`
  で断らない([preset の判定](../src/core/communication/renderconfigeditor.cpp#L5323))。`@1` の preset は今のまま(root から不可、preset の宣言は root の出所として扱う)。
- **消えた対象**: `pass_replacements` / inactive で部品の宣言の対象が消えても warning は出さず、dump に `absent` で残る。置き換え先の pass には効かない(報告の B6・B10)。
- **出荷の文書**: engine の feature・断片・レシピに `target_planning` を書いた物は無い(`src/core/resources` の検索で 0 件)。

<a id="rc6a2-implementation"></a>

### 7.9 RC-6a2(レシピの `backend` と、部品の新しい鍵、2026-10-29)

`a99725b5f`(branch `agent/rc6`)。仕様は `<作業ディレクトリ>/rc6_spec.md` §2、報告は `<作業ディレクトリ>/rc6a2_report.md` と修正の報告 2 本。全体(5 つの鍵・候補ごとの反映・`infeasible_prefer`・
最終の検証)は [棚卸しメモ §2-3 の RC-6a2](design_plan_layer_inventory.md#rc6a2-implementation)。レシピと部品に関わる所だけ書く。行番号は `a99725b5f` の物。

- **書ける鍵**: §7.8 の 3 つの置き場の `target_planning` に、node の `scope_break` / `scope_join` と資源の `representation` / `alias_group` も書ける(名前の所有の規則は §7.8 のまま。
  裸の値は `prefer`)。graph の `backend`(`graphs.<g>.backend`)は **`@2` レシピの `config.target_planning` だけ**が書け、feature と構造の断片の `backend` は読む時点で
  `target_planning.policy_invalid@1`(`graph`・`target`・`key`・`document`・`pointer`。[断る所](../src/project/renderpipeline.cpp#L269))。graph を指す宣言は名前の所有の検査から
  外した([その所](../src/project/renderpipeline.cpp#L1000))ので、レシピの `backend` は `policy_foreign_name@1` にならない。base の断片は出所が部品(`part`)なので書けない。
- **優先**: レシピの `backend` は root の `backend` に負けて dump に `overridden` で残る(test「RC-6a2 compiler binds new root recipe and part keys through the production bridge」:
  レシピの `transient` の `prefer` に root の `materialized` が勝ち、materialized の候補が選ばれる)。
- **`alias_group` の組名は文書の中だけ**: 組は (宣言の出所の document, 組名) で決まる。feature F と feature G が同じ組名 `"scratch"` を書いても別の組で、共有させたければ root に
  書く(報告の C5)。レシピの組名はレシピの文書の中、断片の組名はその断片の文書の中だけで通じる(断片ごとに document が違うため。コードの読み)。
- **出荷の文書**: engine の feature・断片・レシピに `target_planning` を書いた物は今も無い(`src/core/resources` の検索で 0 件)。

### 7.10 RC-6b2(旧い pin と physical fragment の撤去、2026-10-29)

`4e21d7d03`(branch `agent/rc6`)。仕様は `<作業ディレクトリ>/rc6_spec.md` §5 の RC-6b2、報告は `<作業ディレクトリ>/rc6b2_report.md` と修正の報告。全体は
[棚卸しメモ §2-3 の RC-6b2](design_plan_layer_inventory.md#rc6b2-implementation)。レシピと部品に関わる所だけ書く。

- **鍵**: `@1` の preset の `config`、`@2` のレシピの `config`、構造の断片の文書に `vulkan_plan_pins` / `vulkan_physical_fragments` があれば、値によらず
  `target_planning.legacy_package_removed@1 {key, document, message}`。レシピの `config` の鍵の検査より先に断る([`expandRenderPipelineRecipe()`](../src/project/renderpipelinerecipe.cpp#L71) の
  先頭)ので、レシピの `config` に書ける鍵は 9 個(`base`・`features`・`graph_transforms`・`shader_defines`・`draw_sort`・`material_routing`・`xr`・`render_strategy`・`target_planning`)。
  test「RC-6b2 removed planning keys are refused in preset recipe and structural fragment documents」。
- **`restore_render_documents`**: preset / レシピの `config` と構造の断片も、復元の後に選ばれなくなる場合を含めて同じ code で断る(test「RC-6b2 restore rejects removed planning keys
  in every document even after switching presets」)。
- **A5**: §7.2 の 16 構成の比較は記録し直さず、期待と実際の両方から snapshot の `pins` / `physical_fragments` と dump の廃止した 5 欄だけを除いて比べる。RC-5s の固定 HEAD の stale の test は
  complete package の検査だけを残した(「RC-5s fixed HEAD complete packages are stale」)。

### 7.11 RC-6c(資源の寿命、2026-10-30)

`be795c9a5`(branch `agent/rc6`)。仕様は `<作業ディレクトリ>/rc6_spec.md` §6、報告は `<作業ディレクトリ>/rc6c_report.md`。寿命の意味は [RT メモ §2 の末尾](design_render_target_memory_reduction.md#rc6c-lifetime)。
レシピの 16 構成に関わる所だけ書く。

- **A5**: §7.2 の 16 構成の比較は記録し直さない。比べる前に、資源名ごとの最後の use の node(例: `lit_color` は `scene_present`、`opaque_color` / `opaque_depth` は
  `forward_transparent`、G-buffer は `deferred_lighting`)と理由(`swapchain` は `["present"]`、XR の構成の `display` は `["xr_mirror"]`、ほかは無し)を期待値の表で検査し
  ([表](../test/rc5_recipe_compiler_test.inc#L340))、`automatic_plan_fingerprint` も今の計画から計算し直して照らす。その後で RC-6b2 の除外に加えて
  `physical_plans.*.automatic_plan_fingerprint` と `physical_plans.*.{resources,lowering_graph.resources}[*].lifetime.{last_use,retain}` だけを除いて比べる。
  `used`・`first_use`・資源の集合・alias の組・representation・attachment の ops・cost は除かない。16 構成・3203 assertions が緑(報告)。
- 固定 package の物理値から、登録の placement・generation の image・allocation token・bytes も照らす(報告の E3。alias の組と登録は RC-6c の前と同じ)。

### 7.12 RC-7a(名前の readback の約束 `retain`、2026-10-31)

`2d1715cf3`(branch `agent/rc7`)。仕様は `<作業ディレクトリ>/rc7_spec.md` §1、報告は `<作業ディレクトリ>/rc7a_report.md` と修正の報告 2 本。約束の意味は
[RT メモ §2 の末尾の RC-7a](design_render_target_memory_reduction.md#rc7a-readback)。レシピと部品、16 構成に関わる所だけ書く。

- **書ける所**: 資源の `retain` は §7.8 の 3 つの置き場(root・`@2` のレシピの `config.target_planning`・feature と構造の断片)に書ける。名前の所有の規則・優先(root > レシピ > 部品)・
  裸の値の強さ(root は `require`、ほかは `prefer`)は §7.8 / §7.9 のまま。test「RC-7a F3 retain follows root recipe and part precedence and round trips」: feature の `retain: true` が
  `adopted` で読め、root の `false` で `overridden` になって readback が断られる。構造の断片の `true` はレシピの `false` に負ける。
- **所有と置き換え**: feature が自分の宣言しない名前(例 `lit_color`)に書くと `target_planning.policy_foreign_name@1`。`fragment_overrides` で断片を置き換えると、元の断片の `retain` は
  置き換え先に移らない(test「RC-7a F3 replacing a same-name owner does not carry its retain」)。
- **TRANSFER_SRC**: RC-7a では宣言に要る。test の feature は bloom の `Bloom_Threshold_RT` の `usage` に `TRANSFER_SRC` を足してから `retain` を書いている。
- **16 構成**: `retain` を書かない 16 構成は、`283aacc05` で記録した `<作業ディレクトリ>/rc7_baseline/rc5` と fingerprint を含めて完全一致(test「RC-7a F4 unchanged HEAD baseline including fingerprints」、
  [その所](../test/rc5_recipe_compiler_test.inc#L522)。`[.]` で手で走らせる)。§7.2 の A5 は RC-6c の除外のまま(除外を増やしていない。報告)。
- **出荷の文書**: engine の feature・断片・レシピと `projects/` に `retain` を書いた物は無い(検索で 0 件)。

### 7.13 RC-7b(同じ形の共有の条件を広げる、2026-10-31)

`0091c071c`(branch `agent/rc7b`)。仕様は `<作業ディレクトリ>/rc7_spec.md` §3、報告は `<作業ディレクトリ>/rc7b_report.md` と修正の報告 2 本。条件の意味は
[RT メモ §2 の末尾の RC-7b](design_render_target_memory_reduction.md#rc7b-sharing)。レシピと部品、16 構成に関わる所だけ書く。行番号は `0091c071c` の物。

- **部品の `alias_group` の照合**: 組が (宣言の出所の document, 組名) で決まる規則(§7.9)は変わらない。登録の前の最終の照合も、最終の物理計画の台帳で採った `require` の行ごとに、
  `origin.document` と組名が同じ採った行を peer として探す([その所](../src/core/renderingpass/rendercompilerprogram.cpp#L258))。だから feature が書いた組の `require` は、その feature の文書の
  peer とだけ照らされる(コードの読み)。新しく適格になった depth・`TRANSFER_SRC` 付き・display でない BGRA の資源にも、`no_alias`・`isolate`・`alias_group` の `require` / `prefer` が
  同じく効く(test「RC-7b B1 B6 identical effective image contracts admit depth transfer and BGRA」)。
- **16 構成**: 実装前の HEAD `2d1715cf3` で記録した `<作業ディレクトリ>/rc7b_baseline/rc5` と、意味の欄が一致する(test「RC-7b B4 semantic fields match sixteen immutable preimplementation snapshots」、
  [その所](../test/rc5_recipe_compiler_test.inc#L586)。`[.]` で手で走らせる)。差は共有の派生の欄だけで、組と `alias_candidates` は 16 構成とも変わらない。資源ごとの差は
  RT メモの「計画の差(B4)」。§7.2 の A5(`[rc6-baseline-compare]`)と §7.12 の F4(`[rc7-baseline-compare]`)も、同じ除外の関数 [`rc7bComparable()`](../test/rc5_recipe_compiler_test.inc#L447)
  を通して比べる形になった(F4 は fingerprint を含む完全一致ではなくなった。fingerprint は派生の欄の分だけ変わる)。
- **出荷の文書**: engine の feature・断片・レシピと `projects/` に `alias_group` / `no_alias` を書いた物は無い(`src/core/resources` と `projects/` の検索で 0 件)。

### 7.14 RC-7c(形の違う区間詰め、2026-10-31)

`27a919f4a`(branch `agent/rc7c`)。仕様は `<作業ディレクトリ>/rc7_spec.md` §4・§4.9、報告は区切りごとに 2 本。意味は
[RT メモ §2 の末尾の RC-7c](design_render_target_memory_reduction.md#rc7c-packing)。レシピと部品、16 構成に関わる所だけ書く。行番号は `27a919f4a` の物。

- **部品の `alias_group` の照合**: 組が (宣言の出所の document, 組名) で決まる規則(§7.9)は、planner の slot の決定([`updateGenerationAliasPolicy()`](../src/project/targetrenderplanning.cpp#L4716))と
  runtime の詰め直しの `require` の集合([その所](../src/core/renderingpass/renderingpassconfigregistration.cpp#L1257))の両方で同じ鍵になった。別の文書が同じ組名を書いた `require` は、runtime でも別の組
  (③ の修正の報告の 1。GPU の test「RC-7c fix1 runtime required groups belong to their declaring documents」)。組は同じ region の同じ offset を求める。
- **16 構成**: 実装前の HEAD で記録した `<作業ディレクトリ>/rc7c_baseline/rc5`(compiler_only・facts なし)と、意味の欄が一致する(test「RC-7c C5 sixteen fixed snapshots retain every semantic field」、
  [その所](../test/rc5_recipe_compiler_test.inc#L622)。`[.]` で手で走らせる)。差は配置の派生の欄(`alias_groups`・`memory_regions`・`memory_packing`・`alias_candidates` の未確定・memory の集計・fingerprint)だけで、
  facts が無いので旧い組は全部別の確保に戻る(LDR・extras なしは元から組なし、HDR・extras なしは `g_emissive` / `scene_ldr_in`、LDR・extras ありは `gbuffer_normal` / `probe_image`、
  HDR・extras ありは `g_emissive` / `probe_image` と `gbuffer_albedo` / `scene_ldr_in`。bare / lit、flat / XR で共通。実装者の報告)。§7.2 の A5・§7.12 の F4・§7.13 の B4 も、配置の派生の欄だけを
  新しい形で比べるよう移した(意味の欄の除外は増やしていない)。policy の効き方を見る helper は作り物の facts を渡す `rc5CompileWithFacts` に移し、基準の `rc5Compile` は compiler_only のまま。
- **出荷の文書**: engine の feature・断片・レシピと `projects/` に `alias_group` / `no_alias` / `retain` を書いた物は無い(`src/core/resources` と `projects/` の検索で 0 件)。

### 7.15 RC-7d(memory の種類ごと、2026-11-01)

`c941a8346`(branch `agent/rc7d`)。仕様は `<作業ディレクトリ>/rc7_spec.md` §5、報告は `<作業ディレクトリ>/rc7d_report.md` と修正の報告 `<作業ディレクトリ>/rc7d_fix_report.md`。意味は
[RT メモ §2 の末尾の RC-7d](design_render_target_memory_reduction.md#rc7d-memory-kinds)。16 構成に関わる所だけ書く。行番号は `c941a8346` の物。

- **16 構成と A5 / F4 / B4**: 物理計画の JSON に足した `memory_types`・`image_memory_facts` を配置の派生の欄として比較から除いた(A5 / F4 / B4 は [`rc7bComparable()`](../test/rc5_recipe_compiler_test.inc#L457)、
  C5 は [その所](../test/rc5_recipe_compiler_test.inc#L631))。C5 は lowering graph の pattern の `allow_alias`(transient / tile-local の候補でも宣言の適格の答えになった)も除く。意味の欄の除外は増やしていない。
  C5 は生の差も書き出し、16 構成の資源ごとの差は `<作業ディレクトリ>/rc7d_c5_details.json`(配置の差は着地済みの RC-7c の分を含む。実装者の報告)。4 種とも緑。
- **Freeze**: Studio の Freeze が transient / tile-local の住人に `alias_group` を `prefer` で書くようになった(§2 の末尾の RC-7d)。レシピと部品が書く `alias_group` の規則(§7.9)は変えていない。
