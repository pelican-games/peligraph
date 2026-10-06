# RPC の追加の記録(2026)

**これは記録であり、現在の作業指示ではありません。** 2026-10-06(DOC-2 段 b)に [第10章](../manual/10_tools.md) §10.3 の「エディタ拡張メソッド」の下から移した、2026-09〜10 に RPC へ足したものの箇条です。
本文は第10章にあったときのまま移したので、「上の表」「下の「…」」は第10章の当時の位置を、「形の正は …」は当時の正本を指します。当時の「形の正」が第10章の節(「material を消す・戻す・改名する」など)を指すものは、今は [第13章](../manual/13_editor.md) §13.5 の同じ名前の節です。
今の params・戻り値・拒否コードの正本は、基盤・読取の method が [第10章](../manual/10_tools.md) §10.3、編集系の method が [第13章](../manual/13_editor.md) §13.5 です([第11章](../manual/11_status.md) §11.4 のルール 6)。

## 2026-09〜10 の追加(第10章 §10.3 から)

既存メソッドの変化: `get_status` に `scene_source`(シーンは**ホットリロード対象外**・保存は `save_scene`)が追加、`step_frame` の応答に `edit_results[]` が追加、`load_scene` / `reload_game_logic` / `start_input_replay` は進行中のプレビューを強制中止します。
2026-09 の追加(詳細は [第13章](../manual/13_editor.md) §13.13): `get_status` に `participants.game_logic`
(`error_code` / `error_fields`。**起動時の初期化失敗も未試行の状態で出ます**)、`load_scene` と
`import_scene_snapshot` の result に `committed` / `effect_errors`、`get_components` の各
component に `resolution_error`。

2026-09 の追加(GP-4、`c095493c`): `get_status` の `input` に `cursor {requested, effective, embedded}`([組み立て](../../src/core/communication/rpcserver.cpp#L1770))。`requested` はゲームコードが `ctx.input().setCursorMode` で頼んだモード、`effective` は窓に適用したモード(`normal` / `hidden` / `locked`)、`embedded` は窓が別の窓に埋め込まれているか(`--rpc` かつ `WS_CHILD`。Windows だけで判定)。埋め込みでは `locked` を頼んでも `effective` は `normal` です。headless は窓が無いので `effective` は常に `normal`、`embedded` は `false`。

2026-09 の追加(AG-3a、`32d9a20b`): `get_status` に `animation`(上の表)。新メソッド `get_graph_node_values{instance?}` → `{instance, generation, frame, published, source:"rendering", nodes:[{node, expanded, outputs:{port: value}}]}`(publish 時の snapshot、再評価しない。`instance` 省略で root 側の展開木全体、rendering domain の node は `outputs: {}`、publish 前は `published: false`)。**animation domain の node は空ではありません** —— 自分の値は出しませんが、そのフレームに**食べたスカラー**が values 側に `$pose/<node>/<port>` として publish されるので、**受けた入力ポートの名前で**並びます([renderconfigeditor.cpp:2010](../../src/core/communication/renderconfigeditor.cpp#L4599)、[graphAnimationScalarOutput()](../../src/project/graphpartition.cpp#L461))。読み取り専用で `edit_*` の CAS には乗らない。

**`get_graph_values` と `get_graph_node_values` は問いが違います。**前者は `instance` 必須で、その
**instance の入力 port だけ**を `{port, value, source, connected|parameter}` として返します(`source` は
`connected` / `parameter` / `default` / `override`)。後者は**展開木のノードを全部**並べ、各ノードの出力値を
`outputs` に入れます([実装](../../src/core/communication/renderconfigeditor.cpp#L4518))。両方とも
**同じ publish snapshot を読むだけ**で再評価しません —— 再評価は graph の時計を進め event を二重に消費するためです。
`source: "rendering"` は「これは描画側の評価が使った値だ」という明示で、animation phase は同じ文書を
自分の state で評価するため、event や trigger で駆動される文書では両者が恒久的に違いうることを表します。

2026-09 の追加(グラフ・共有部品・package): `get_graph_instances` / `get_graph_catalog` / `get_graph_values` /
`edit_graph_document` / `edit_graph_instances`([登録](../../src/core/communication/editorrpchandlers.cpp#L277))、
`export_render_feature` / `import_render_feature`(`user://components/<name>/` 経由の共有部品)、
`install_package` / `uninstall_package`(`project://packages/<ns>/` への vendoring)。params と返り値、
`ops` の 5 種と `scope`、名前付きの拒否は [第13章](../manual/13_editor.md) §13.5 が正です。

2026-10 の追加(IA-2 / SC-1 / EL-6。形の正は [第13章](../manual/13_editor.md) §13.5):

- 新メソッド `get_registered_providers {}`(✅EL-6、`5582bb93`)→ `{draw_sort, pass_implementation, region_replacements, graph_transforms, render_strategy}`。各値は `[{name, source: "engine" | "game", provider_version}]` を名前順で、呼ぶたびに registry から読みます([登録 `get_registered_providers`](../../src/core/communication/editorrpchandlers.cpp#L312))。
- `reload_render_documents` の committed の結果に `changed_assets`(✅IA-2、`cdcdebc1`)。image の `initial` の asset の中身だけが変わったときも組み直して公開し、その asset を辞書順で並べます。何も変わらなければ `no_change: true`・`changed_assets: []`。
- `get_components` / `scene_tree` の `schema.fields[]` と `get_component_add_candidates` の `schema_fields[]` に、camera の `controller.*` の `discriminant` / `applies_to` / `required_for` / `default`(✅SC-1、`14e10b19`)。`controller.type` は `enum` `["orbit", "follow", "fly"]` になりました。
- `get_render_features` の `feature_instances[].parameter_schema` の宣言に `engine_default`(✅SC-1)。materialize 済みの feature で、元の `default` を持つ宣言だけです。

2026-10 の追加(CT-3 / MP-1。形の正は [第13章](../manual/13_editor.md) §13.5 と [第6章](../manual/06_rendering.md) §6.17・§6.18):

- `get_graph_catalog` の各 op に `parameters: [{name, kind, required?, alternatives?, default?, enum?, min?, max?, items?, fields?, expression?, description?}]` と `parameters_complete`(✅CT-3、`0d215a68`)。catalog 本体の鍵なので `pelican_cli graph-catalog` にも出ます。`parameters_complete` が `false` の op は `render.image` / `render.fullscreen` / `render.raster` / `render.material_pass` です(CT-3 のときは `render.compute` を含む 5 つでした。✅CT-6 で `render.compute` は `true` になりました。[第6章](../manual/06_rendering.md) §6.17)。
- 名前付きの拒否 `graph.render_node_parameter_invalid@1 {node, op, parameter, expected, reason: type | enum | range, allowed? | min? / max?}`(✅CT-3)。編集の rejected には CT-2 の `document` も付きます。
- `get_graph_catalog` の `material_pass_contracts` の各 contract に `screen_inputs` / `surface_resources`(`[{name, source_type}]`。✅MP-1、`afd923e1`)。CLI には出ません。
- `get_frame_plan` の material pass の node に `drawn_materials: [{material?, id, draws}]`・`drawn_materials_frame`、256 件を超えたら `drawn_materials_truncated: true`(✅MP-1)。最後に公開した logical frame で、その pass が描いた material です。

2026-10 の追加(FR-1 / FR-2 / AB-1。形の正は [第13章](../manual/13_editor.md) §13.5、[第6章](../manual/06_rendering.md) §6.5・§6.8・§6.18、[第5章](../manual/05_assets.md) §5.8):

- `list_render_features` の各 feature に `required_targets: [{name, type, usage?}]`(✅FR-1、`45ca769b`)と `insertion: [{pass, anchor, position}]`(✅FR-2、`0b15fb62`)。feature を足す前に、pipeline に要る target と挿入先が分かります。
- `get_render_authoring_context` に `snapshot_after_candidates`(✅FR-1)。`rendering.snapshots` の `after` に書ける名前です。
- 名前付きの拒否 `feature.required_target_missing@1 {feature, target, type, document}`・`snapshot.after_unknown@1 {snapshot, after, resolved?, candidates}`(✅FR-1)、`feature.insertion_anchor_missing@1 {feature, anchor, candidates, document}`(✅FR-2)。`graph.target_ref_type_mismatch@1` に `expected: {type, layers?}`(✅FR-1)。
- 新メソッド `bake_audio_analysis {asset, out, settings?}`(✅AB-1、`892a1455`)→ `{ticket, status: "accepted", asset, out}`。worker で焼き、`get_edit_result` の committed に `{operation: "bake_audio_analysis", asset, out, manifest, layout}`([登録 `bake_audio_analysis`](../../src/core/communication/editorrpchandlers.cpp#L271))。
- 新メソッド `list_derived_assets {}`(✅AB-1)→ `[{asset, manifest, tool, source, derived_from, stale, stale_reasons}]`(配列)。asset store の `*.import.json` を全部読みます([登録 `list_derived_assets`](../../src/core/communication/editorrpchandlers.cpp#L274))。

2026-10 の追加(CT-4 / FR-3。形の正は [第13章](../manual/13_editor.md) §13.5、[第6章](../manual/06_rendering.md) §6.5・§6.17・§6.18):

- `get_graph_catalog` の各 op の `parameters` の要素に `choices_from?`(`{rpc, key, field?}` か `{graph_nodes: [<op>...]}`)・`asset_kind?` / `extensions?`・`soft_min?` / `soft_max?`、kind `union` と `variants`、各 op に `placement: "anywhere" | "owned_instance"`(✅CT-4、`4d682338`)。catalog 本体の鍵なので `pelican_cli graph-catalog` にも出ます。
- `get_render_authoring_context` に `view_families` と `canonical_anchors`(✅CT-4)。`choices_from` が指す一覧です。
- 名前付きの拒否 `graph.op_requires_owned_instance@1 {node, op}`(✅CT-4)。`pose.*` と `value.morph_weight` を `owner` 付きの `graph.instance` の外に置いた。編集の rejected には CT-2 の `document` も付きます。
- `list_render_features` の `required_targets` の各要素に `needs_writer`(✅FR-3、`55390f4b`)。名前付きの拒否 `feature.required_target_unwritten@1 {feature, target, type, writer_candidates: [{node?, pass, port, resource}], document}`(✅FR-3)。
- 新メソッド `edit_batch {base_source_digest, edits: [{method, params}]}`(✅FR-3)→ `{ticket, status: "accepted", base_source_digest, methods}`。文書を書き換える 20 の編集 method を順に 1 つの候補へ当て、組み立てと preflight を 1 回だけ行う 1 つの ticket です。当てられない edit は `batch_index` 付きで断ります([登録 `edit_batch`](../../src/core/communication/editorrpchandlers.cpp#L360))。

2026-10 の追加(IA-3 / CT-5 / PV-1 / scene_outline。形の正は [第13章](../manual/13_editor.md) §13.5・§13.7、[第6章](../manual/06_rendering.md) §6.5・§6.13・§6.17・§6.18):

- `reload_render_documents` の比較に、`value.sample_table` が読む表(KTX2)と隣の `.import.json` の中身(✅IA-3、`26473e75`)。`changed_assets` には表の参照で出ます(manifest だけが変わったときも表の名前)。
- `get_graph_catalog` の `choices_from` に条件 `where` と 3 つ目の形 `{asset_manifest: {parameter, manifest, key}}`、`render.fullscreen` の `push_constants` が enum(✅CT-5、`65f3a8a3`)。`get_render_authoring_context` に `acceleration_structures` と `render_target_vocabulary.format_class_patterns`。`get_graph_instances` に `definition_instances {<definition>: {owned_instances_only, instances: [{id, op, owned}]}}`。
- 名前付きの拒否 `graph.render_scalar_target_incompatible@1 {node?, target, expected: {host_source, size?}, actual}`(✅CT-5。graph の中の buffer は partition で、graph の外の buffer は登録の段で。後者は `node` 無し)。`edit_graph_instances` は graph の名前付きの拒否を、その code と CT-2 の `document` のまま返します(前は `invalid_graph_attachment`)。
- `get_status` と `get_frame_plan` に `preview: {available, reason?}`、render の編集の committed の結果にも `preview`(✅PV-1、`a173e70c`)。`render_preview` は preview が使えない世代で `preview.unavailable@1 {method, reason}`。preview だけが組めない編集は断られず committed になります。
- 新メソッド `scene_outline {scene_id?, if_scene_revision?}`(`b29ccfea`。利用者の codex の変更)→ `{scene_id, scene_revision, not_modified, objects?: [{authoring_object_id, declaration_index, name?, parent_authoring_object_id}]}`。component を含まない authoring の一覧で、`if_scene_revision` が今の revision と同じなら `objects` を省きます([登録 `scene_outline`](../../src/core/communication/editorrpchandlers.cpp#L77))。

2026-10 の追加(BS-1 / BS-1b。形の正は [第13章](../manual/13_editor.md) §13.5 と [第6章](../manual/06_rendering.md) §6.16):

- player の引数 `--render-fallback`(✅BS-1、`d9811bfc`。`--rpc` が要る。§10.2)。`get_status` に `render_pipeline: {state: "ready" | "fallback", root, refusal?}`、`get_frame_plan` は fallback の世代の間だけ最上位に同じ `render_pipeline`。fallback の間の `reload_render_documents` は、変わった文書が無くても毎回組み直します(`no_change` にならない)。`get_render_documents` は組めない root でも、root と rendering 節が名指す文書を返します。Studio は ✅BS-1s(`19be8983`)からこの引数で engine を起こし、`render_pipeline` を読みます。
- 名前付きの拒否 `render_document_syntax@1 {document, line?, column?, reason}`(✅BS-1b、`40318ffe`)。root か、rendering 節が名指す feature / preset が JSON として読めない。編集の rejected の `error` には `method` も付きます。root が JSON でない `reload_render_documents` と、JSON でない `content` の `restore_render_documents` は、`render_pipeline_preflight_failed` からこの code に変わりました。`get_render_documents` は読めない文書も、生の `content` とその `digest` で返します。

2026-10 の追加(PB-1。形の正は [第6章](../manual/06_rendering.md) §6.16 の「engine が持つ今の問題の一覧」と [第13章](../manual/13_editor.md) §13.5 の「今の問題の一覧を読む」):

- 新メソッド `get_problems {}`(✅PB-1、`a55d1eaf`)→ `{revision, problems: [...]}`。`get_status` に `problems: {revision, count, errors, warnings}`。1 件は `{id, severity: "error" | "warning", source: "startup" | "hot_reload" | "reload_rpc", code, message, fields, document?, line?, column?, node?, port?, definition?, scope?, document_kind?, at_frame?}` で、
  `document_kind` は `root` / `root_graph` / `definition` / `root_rendering` / `preset` / `feature` / `shader`。`id` は code と場所の hash の 16 桁(`source` を含まない)。今は `warning` を出すものがありません(コードの読み)。
  → ✅FO-1(`a4d7760a`)から `document_kind` に `material` が加わり、`material.binding_unresolved@1` が最初の `warning` です(下の FO-1 の追加)。
  編集 RPC の拒否と、`reload_busy`・`gate_closed`・`external_modification`・`stale_source_digest` は入りません。Studio の Problems の窓(PB-1s)は作業中です。
  → ✅PB-1s(`93934318`)から、Studio は一覧を **Problems のドック**(View > Problems)で読みます。`get_status.problems.revision` が動いたときと読み直しの答えの後に `get_problems` を読み、
  行を押すと直す場所(Graph の node、Show the document の行、Materials、shader の file、Assets)へ飛びます。status bar に件数が出ます([第13章](../manual/13_editor.md) §13.1 の「Studio の Problems のドック」)。
- 名前付きの拒否 `render_feature_requires_restart@1 {feature?, module}`(✅PB-1)。module の graph が固まった後に、その module の要る feature を初めて有効にする構成(起動し直せば通る)。
  message は `render_feature_requires_restart@1: feature 'debug_draw' needs runtime module DebugDraw, which this engine was started without; restart the engine to enable it`。
  前は code の無い `Render pipeline hot reload cannot enable feature '<feature>' after module creation is frozen; missing runtime module <module>`(と、container の `Module '<型>' was first requested after the runtime module graph was frozen`)で、
  `reload_render_documents` では `render_pipeline_preflight_failed` に畳まれていました(コードの読み)。`feature` は module が feature の表にあるときだけ付きます。
  Studio は ✅PB-1s から、BS-1s の再起動の案内をこの code と `fields.feature` で出します(前は文面を探していて、本番の文と一致していませんでした)。
- 名前付きの失敗 `shader_reload_failed@1`(一覧の 1 件の code。`fields: {path, log}`、`document_kind: "shader"`、文書自身の行なら `line`)。`reload_shaders` の失敗の `error_code` と同じ名前です。
- `--render-fallback` の起動で、起動の間に登録する material の route を受ける pass が無い(`material_route_unserved@1`)ときも、致命にせず fallback の世代に入ります(✅PB-1)。

2026-10 の追加(RR-1。形の正は [第13章](../manual/13_editor.md) §13.5 の「reload に断られた版を持つ」と [第6章](../manual/06_rendering.md) §6.16):

- `get_graph_instances`・`get_render_authoring_context`・`get_render_features`・`get_fold_groups`・`get_frame_node_order`・`get_compute_task_authoring`・`get_ray_callable_records`・`get_scene_acceleration_structures` の答えに `live: bool`(✅RR-1、`4549cae8`)。返した版が live の世代を組んだ版かどうかです。文法の壊れた版を持っている間は前に読めた版を返し、`held_unparsed: true` も付きます。`get_render_documents` の答えは `{documents, live}`(`held_unparsed` は付かない)。`get_graph_values` には付きません。
- 文書の都合で断られた `reload_render_documents` の後、editor はディスクの版を base に持ちます。その digest の `restore_render_documents` と構造の編集が通り、前の live の digest の編集は `external_modification` です。持っている間の reload は `no_change` になりません。断られた後にディスクを元に戻したときは、元の digest の編集の前に一度 reload が要ります(前は戻せばそのまま通りました)。Studio はまだ `live` を読みません。

2026-10 の追加(RTL-1a / RTL-1b / RTL-1c。形の正は [第13章](../manual/13_editor.md) §13.5 の「ray の layout と bindings を読み書きする」と [第6章](../manual/06_rendering.md) §6.15):

- 新メソッド `get_ray_layout {}`(✅RTL-1a、`e9f3a333`)→ `{source_reference, source_digest, live, mode, layout, bindings, names, selectable_materials, selectable_procedural_assets, unresolved_bindings}`(`mode` は ✅RTL-1c)。`edit_ray_layout {base_source_digest, layout?, base_layout_digest?, bindings?, base_bindings_digest?, procedural_records?}` → render の編集の ticket([RTL-1a の登録 `get_ray_layout`](../../src/core/communication/editorrpchandlers.cpp#L343))。`edit_batch` が受ける method は 24 になりました。
- `get_render_documents` の `kind` に `ray_group_layout` / `ray_bindings`(✅RTL-1a)。`restore_render_documents` で作る・消す・戻す、`reload_render_documents` が読み直す対象です。
- ray の layout の有無が変わる編集・restore・reload は、拒否ではなく `committed` + `restart_required: true` + `restart_reason: {code: "ray_layout_mode_changed", from, to, message}`(✅RTL-1b、`8034d096`)。`get_status` に `ray_tracing.ray_layout_mode`。
- RT の task の拒否は parser の code のまま、fields(`task` / `ray_type` / `group` / `miss` / `stage` / `keys` など)が `error` に並び、editor と reload の拒否には `document` と graph の node の `node` も付きます(✅RTL-1b)。`get_status.render_pipeline.refusal` にも code と fields が付きます(`document` / `node` は無し)。
- `get_render_authoring_context` に `ray_layout: {mode, reference, ray_types, groups, misses, default_group, error?}`、`get_graph_catalog` の parameter に `key_choices_from`(object の鍵の候補。形は `choices_from` と同じ)(✅RTL-1b)。
- `get_frame_plan` の `ray_query_acceleration_structures` に `named_tlas_blas: {<TLAS 名>: [<resident_blas_keys と同じ形>]}`(✅RTL-1b。[第6章](../manual/06_rendering.md) §6.15 の「BLAS variant と `resident_blas_keys` 診断」)。
- `get_shader_interface` は、root が layout を名指すとき layout の生成 include を渡して compile します(✅RTL-1b)。
- 名前付きの拒否 `pelican.ray_layout_destination_exists@1 {method, path, document_kind}`(✅RTL-1a。`edit_ray_layout` が新しく作る場所に、root が名指していないファイルがある)。edit の時点の layout / bindings の拒否は parser の code のまま(`pelican.ray_group_layout_invalid@1`・`pelican.ray_binding_record_invalid@1`・`pelican.ray_tracing.bindings_without_layout@1` など)、`asset_not_found {method, asset}`・`asset_data_unavailable {method}`(`procedural_records`)。
- 行に当たらない ray binding は失敗にならず、`get_ray_layout.unresolved_bindings` に出ます(✅RTL-1a)。staged の世代の巻き戻りの文 `pelican.ray_tracing.generation_rolled_back@1: …` に原因の文が付きます。

2026-10 の追加(EB-1。形の正は下の「material を消す・戻す・改名する」と §10.4 の `dist-config`):

- 新メソッド `delete_material {name}` / `register_material {path}` / `rename_material {name, new_name}`(✅EB-1、`aaa7b2da`)。名前付きの拒否 `material.in_use@1 {name, bindings, objects, count, render_documents}`・`material.document_shared@1 {name, path, names}`(新)、ほかは MT-1 の `material.*@1`。
- `list_assets` の `path` は asset_data に書かれた綴り(project 相対)になり、解決したファイルの絶対 path は新しい `resolved_path` です。material も名前ごとに 1 行で出ます(`kind: "material"`)。`get_component_add_candidates` の `asset_selection.choices[].path` も同じ相対の値です(実装者の報告)。
- 名前付きの拒否 `graph.buffer_key_unknown@1 {buffer, key, allowed}`(✅EB-1)。graph / frame graph の buffer の entry に、frame graph が読まない鍵がある(`initial_f32` の綴り間違いなど)。
- `update_assets_manifest` は再検証が走っている間は待たずに `assets_verification_running@1 {store}` で断ります(✅EB-1。下の「起動時の asset 検証の結果」)。
- `pelican_cli dist-config` が asset store を manifest と照らし、違えば exit 1(`--skip-assets-verify` で外す。§10.4)。

2026-10 の追加(FO-1。形の正は [第6章](../manual/06_rendering.md) §6.6 の「pass の順を指定する」、§6.2 の `render_targets`、§6.7 の「起動時に読めない material 文書」):

- `edit_frame_node_order` は、root の rendering 節の依存の無い 2 つの pass の入れ替えを通します(✅FO-1、`a4d7760a`。前は依存の循環 `render_pipeline_preflight_failed`)。依存と矛盾する並びは ticket の結果の拒否 `frame_node_order.conflicts_dependency@1 {graph, first, second, reason: "reads" | "after" | "before" | "canonical_order", resource?, via?, anchor?}` です。名前付きの AS の操作の順も同じ code で断ります。
- 名前付きの拒否 `graph.image_key_unknown@1 {image, key, allowed}`(✅FO-1)。合成した構成の `render_targets[]` の entry(graph の `render.image` の node を含む)に、frame graph が読まない鍵がある(`history` の綴り間違いなど)。`allowed` は 17 個です。
- `--strict-assets` の無い起動で、asset_data の `materials[]` が指す文書が無い・読めないとき、起動は止まらず、その文書の material を登録しません(✅FO-1)。`get_problems` に `document_kind: "material"` の 2 種が入ります: 文書ごとの error `material.document_unknown@1` / `material.document_invalid@1`(`document`、JSON でなければ `line` / `column`、`fields: {path, reason, line?, column?}`)と、元の material に落とした model ごとの warning `material.binding_unresolved@1`(`document` 無し、`fields: {model, materials}`)。`--strict-assets` では今までどおり起動が失敗します。
- `list_materials` の行に `line?` / `column?`。起動時に engine が断った文書の `status` / `reason` / `line` / `column` は engine の状態で、起こし直すまでファイルを直しても変わりません(✅FO-1)。`status` の値は `loaded` / `missing` / `invalid` のままです。

2026-10 の追加(EB-2、`e1ba9058`。形の正は下の「material を消す・戻す・改名する」、[第6章](../manual/06_rendering.md) §6.7 の「起こし直さずに取り込む」、[第13章](../manual/13_editor.md) §13.5 の「ray の layout と bindings を読み書きする」):

- `register_material {path}` が、`materials[]` に載っていて起動時に断った文書なら `material.path_conflict@1` で断らずに読み直して登録し、答えに `recovered: true` と `rebuilt_models` が付きます(✅EB-2)。取り込んだ後の `list_materials` は `loaded` です。
- `hot_reload` に宣言された文書なら、ファイルの保存で同じ取り込みが走ります(reload participant `pelican.material_documents`)。watch が見ている登録済みの文書が読めなくなると、`get_problems` に source `hot_reload` の `material.document_invalid@1` / `material.document_unknown@1` が入ります(✅EB-2)。
- `get_ray_layout.selectable_materials` の各行に `used_by: {count, objects}`、asset の selector の行に `model`(✅EB-2)。
- `edit_ray_layout` / `restore_render_documents` の commit は、書き換えた asset_data の record の逆操作を `procedural_records_before: {<参照>: <JSON 文字列>}` で返します。`restore_render_documents` の `documents` に `{reference, base_digest, content: null, procedural_records: <その文字列>}` を渡すと、その record だけを戻します。文字列の中は `{<asset 名>: <record または null>}` です。文字列のまま保持することで数値型・メンバー順を保ち、同じ digest に戻る編集の繰り返しや engine の再起動後も Undo / Redo できます。asset_data 全体の digest と復元先 layout の検査は行います。`procedural_records` を省いた従来の digest だけの形式も受けますが、これは engine が動いている間の履歴に限ります(✅EB-2。拒否 `render_document_not_writable {reason: "procedural_records_only"}`)。

2026-10 の追加(EB-3 / FO-2、`31c390c7`。形の正は [第13章](../manual/13_editor.md) §13.5 の「SBT の全行を 1 回で読む —— `get_ray_table`(✅EB-3)」と「RT の拒否の code と fields」、[第6章](../manual/06_rendering.md) §6.15 の「shader record の宣言(✅EB-3)」と §6.7 の「pass の宣言漏れは別の code(✅FO-2)」):

- 新メソッド `get_ray_table {limit?: 1..65536(既定 1024), offset?}`(✅EB-3)→ `{mode, generation, layout_reference, bindings_reference, groups, acceleration_structures, rows, total, offset, truncated}`。全 object の hit group・binding・record・TLAS の行を 1 回で返します(triangle の行は `get_ray_occurrences` と同じ出所)。layout の無い project は `mode: "none"` と空の `rows`。
- traced の ray type の hit shader(closest hit / any hit)は shader record を宣言しなくてよくなりました(✅EB-3。宣言するなら layout の物理 record と同じ)。
- `pelican.ray_tracing.shader_record_abi_mismatch@1` と `pelican.ray_tracing.payload_mismatch@1` に fields(`expected` / `actual`、`task` / `stage` / `shader` / `ray_type` / `group` / `miss`、payload は `caller` か `closesthit`)が付きます(✅EB-3)。commit の時の拒否では `error` に平らに、問題の一覧では `fields` に。message の文は前のままです。
- 名前付きの問題 `material.pass_contract_unmet@1 {material, material_document, pass, passes?, requirement, reason}`(✅FO-2)。文書は読めるのに、それを描く pass が material の使うもの(今は `ray_query`)を宣言していない。起動時の `get_problems` の error で、`document` / `document_kind` は pass を宣言している文書(`preset` / `feature` / `root_graph` と `node` / `root_rendering`)。FO-1 の時点ではこの場合も `material.document_invalid@1` でした。pass が宣言すると次の frame で engine が取り込み直し、問題は消えます。
- `list_materials` の `status` に `unusable`(✅FO-2。`pass`・`requirement` が付き、`reason` は問題の message、`materials: []`)。値は `loaded` / `missing` / `invalid` / `unusable` の 4 つです。
- `register_material {path}` の recovered の道で、まだ pass が宣言していなければ `material.pass_contract_unmet@1` で断ります(✅FO-2)。
