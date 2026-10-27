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
  `draw_sort`・`material_routing`・`xr`・`render_strategy`・`target_planning`・`vulkan_plan_pins`・`vulkan_physical_fragments`。
  `render_targets` / `rendering_passes` / `snapshots` / `buffers` / `compute_tasks` は `render_pipeline.recipe_has_structure@1`(`key`)、
  ほかのキーは `render_pipeline.recipe_unknown_key@1`(`key`)で断る([`expandRenderPipelineRecipe()`](../src/project/renderpipelinerecipe.cpp#L65))。
  `features` 以下の 10 個の合成と root からの上書きの可否は `@1` と同じ(報告 §2 が `@1` の resolver の表と照らした)。
- **`base: [{family, fragments: [ref …]}]`**: family ごとに、断片の `render_targets` を書いた順に連結し、`passes` を書いた順に連結して、
  `@1` の `config` と同じ形の `render_targets` と `rendering_passes: [{name: family, passes}]` を作る。展開した後は `@1` の道
  (正規化・root の合成・canonical・HDR・feature の挿入・Graph・IR)を**そのまま**通る([`resolveRenderPipelinePreset()` の版の分岐](../src/project/renderpipeline.cpp#L953))。
  展開の結果に `base` と root の `fragment_overrides` は残らず、レシピに無いキーは補わない(`features` の無いレシピは `features` を出さない)。
  family は 1 つ以上で名前の重複は断る。展開した base 全体で pass 名と target 名が重なれば `render_pipeline.recipe_duplicate_name@1`
  (`kind`: `pass` / `target`・`name`・`fragments`)。Graph の pass の既定の family は、展開した `rendering_passes` の先頭(今の規則のまま)。
- **構造の断片 `pelican.render_fragment@1`**: `{schema, version, name, render_targets, passes, required_targets?}`。`insert` も `parameters` も
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
  `pipeline_preset.{name, ref, version}` と診断 `pipeline_preset_resolved`)だけ。旧構成で作った pin / physical fragment は新構成にそのまま
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

- RC-5s: canonical anchor の撤去(順の骨格・`__anchor_*`・sprite の本物の pass)。報告の申し送り: canonical anchor・HDR の初期化・Graph の
  既定 family と配置・snapshot・IR → FramePlan → 物理計画は RC-5 で触っていない。変えるときは RC-5a の 16 構成の全 node・fingerprint・
  旧 package の適用の比較を保つこと。
- stage(カスタム可能な階層フィルター)= Studio 側の WP。既定の preset の決定と雛形の切り替え。web の鏡像。
- 細部(コードの読み): preset の版の検査の文は `@2` も受けるようになった今も `render pipeline preset version must be exactly 1` のまま
  ([検査](../src/project/renderpipeline.cpp#L941))。engine のレシピが `features` に持つ instance(`hybrid_recipe_lit` の shadow / sky)は、
  `get_render_features` で `scope: "preset"` の行になり、今までの engine preset の feature と同じく「先に fork」と案内される
  (`materializable: false`)。
