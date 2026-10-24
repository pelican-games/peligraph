# 描画グラフの中立な中間表現(RenderGraphIR): 完成した graph の JSON を 1 改訂につき 1 回だけ読み、1 つの語彙と 1 つの順序で compile する

対象読者: エンジン担当(描画コンパイラ / renderer / Studio)。
ステータス: **方針決定**(2026-10-24、利用者了承。D1〜D3 も決定)。codex の設計レビュー 2 回(10 件 + 9 件)を受けた版で、§9 に対応表。WP は RC-3a〜RC-3e(§5)。各 WP は仕様を書いて codex の仕様レビューを通してから実装する。
`docs/design_stage2_typed_pipeline.md` を置き換える(同メモは前提を 2 回誤った。履歴として残す)。
事実の根拠は、統合側が 3 観点(parse / 型 / 流れ)で作った事実の地図(repo の外の作業メモ)。本文の M1 / M2 / M3 はその地図の節で、主な主張には file:line を併記した。外からの要求は v1feature(PF・PSN)から受けたもの(§2.5、§5)。

## 0. 目的(利用者の言葉)

「不要なものは削り、シンプルで必要不可欠な構造を目指す」。「PF-3 を待たせてでも設計をしっかり練る。ここはかなり重要な修正」。

1. **完成した graph は 1 改訂につき 1 回だけ読む**: 合成の終わった graph の JSON(と、graph transform / subgraph の provider の応答)を typed にするのは project の 1 つの JSON parser で、改訂ごとに 1 回。結果を次の段に引き継ぐ。
2. **1 つの語彙**: 同じ事実は 1 つの中立な型で。Vulkan への写像は core の 1 箇所。
3. **1 つの順序**: node の順は planner が 1 回決める。物理計画と実行はそれに従う(view の展開はフレームごと)。
4. **検査は 1 箇所ずつ**: authored な形は JSON parser、資源の性質は generation の資源の記述、material の状態は「pass × material × device」。
5. **使われない物を残さない**: ただし UI・公開 ABI に出ている物は出口まで含めて扱う。

## 1. 今の構造の問題(事実)

| # | 問題 | 根拠 |
|---|---|---|
| P1 | 段の間は JSON。1 回の runtime variant の compile で normalized JSON を読む所が 10 箇所。typed parse は計画用・実行用の 2 系統で、attachment・material_variant・material_resources・footprint は二重実装 | M3 §4.2、M1 §0-1、§6.1 |
| P2 | `validatePassFieldOwnership` が pass 1 個あたり 8〜24 回。拒否は最初に読んだ所が勝つ | M1 §2.2、§7 |
| P3 | 実行用の parse は GPU 登録後の metadata を使うので、normalized JSON を registration まで保持してもう一度読む | M1 §0-10、M3 §4.1 |
| P4 | 写しが多い: 入出力は 7 段以上、attachment の load / store は 4 表現、format は vk::Format と 2 種の綴りと別表 | M2 §3、§2 |
| P5 | 順序が 4 系統。実行は物理計画の scope 順、barrier は FramePlan 由来で、一致の検査が無い | M3 §0-6 |
| P6 | 受理する形が reader ごとに違う(`after` / `input` の文字列形、`output.color` / depth の object 形、`buffers` の文字列形) | M1 §6.2 |
| P7 | parse 後に `graph_definitions` だけを書き換える(`#xr` 改名、AS node、`frame_node_order`) | M3 §1 行 15 |
| P8 | 使われない物(§3) | M1 §8、M2 §5、M3 §3 |
| P9 | Studio の多くの RPC は別の合成で候補を作り、宣言を自前の緩い規則で読む | M3 §3、M1 §5 |
| P10 | 段階 1 と段階 2 で policy を 2 回作って比べる | M3 §2.1 |
| P11 | graph transform は要求 1 件ごとに前後 2 回 typed parse し、compiler が最終 JSON をもう一度読む | `graphtransformregistry.cpp:240`、`:1217`、`:1269`、`vulkanrendercompilerprogram.cpp:811` |

## 2. 目標の構造

```text
authored 文書 ─ lowering ─▶ compose(JSON→JSON。preset → render strategy(feature の選択を変える。compose の中に残す)→ feature の合成 → graph の lower / loop / canonical)
   ─▶ variant の変換 ─▶ ImGui の挿入(flat)
   各 JSON の入口で normalizeRenderingJson(§2.1)
   ─▶ ★ parseRenderGraph(project、唯一の JSON parser)─▶ RenderGraphIR の改訂 0
        ─▶ graph transform / subgraph の provider(ABI は JSON のまま。応答 = 新しい改訂 → parseRenderGraph で 1 回)…
   ─▶ finalizeRenderGraph(typed。provider の列の後に 1 回: `#xr` 改名、AS node、frame_node_order。JSON を読まない)─▶ RenderGraphIR(確定、不変)
   ─▶ planRenderGraph ─▶ FramePlan(唯一の node 順と barrier)
   ─▶ toLogicalGraph ─▶ 論理 graph ─▶ 物理計画(FramePlan の順の上で scope を切る)
   全 variant の後 ─▶ resolveGenerationResources(§2.4)─▶ 資源の記述(variant 間で統合済み)─▶ 資源の検査
   ─▶ lowerToVulkan(core、写像だけ)─▶ generation(scope ↔ node の対応を前計算。view の展開はフレームごと)
```

### 2.1 正規化(全ての入口で同じ 1 関数、読みと書き換えを同じ正典で)

- `normalizeRenderingJson` を 1 つ作り、JSON が入る全ての境界で適用する: root の文書、preset、feature の読み込み、graph の lower の結果、ImGui の挿入、provider の応答。内部の generator(graph の lower、snapshot、canvas、template)は最初から正規形を作る。
- 正規形:
  - `input` / `reads` / `writes` / `after` / `before`: 文字列の配列
  - `output.color`: attachment の配列。各要素は `{target, subresource?, load_op?, store_op?}` の object
  - `output.depth`: null または 1 つの attachment object
  - `buffers[]`: object。load / store の綴りは 1 つ
- **target 参照の読み取りと書き換えを 1 組の helper に**: 取り出し(`passWritesSwapchain` / `declarationWrites` / `compiledPassWritesResource` など)と、書き換え(`replaceSwapchainAlias`、HDR の出力先の差し替え、feature の target parameter `$dst` の置換)。書き換えは attachment の `target` だけを変え、load / store / subresource を保つ。
- 正規形でない JSON を受けた reader は名前付きで断る。

### 2.2 RenderGraphIR(project、`src/project/rendergraphir.{hpp,cpp}`)

**唯一にするのは JSON parser(`parseRenderGraph`)**。typed な node の構築(finalize、PF の縫合など)は IR の値を作る関数として別にあってよい。

```text
RenderGraphIR {
  variant
  images:  [ImageResource  { name, format | format_class | candidates, usage, extent(ResourceExtentPlan),
                             mips, layers(静的な制約), dimension, history, persistent, role, clear, initial, source }]
  buffers: [BufferResource { name, size(uint64)| size_from_extent, layout, host_source, view_family, initial }]
  graphs:  [GraphIR { name, nodes: [NodeIR] }]
  policy                                   // 今の CompiledRenderPipeline(1 回だけ作る)
}
NodeIR {
  name, declaration_index, shape           // fullscreen / output_transform / raster / material / compute / ray / snapshot_copy / anchor / shadow_depth / velocity / picking / debug_* / ui / imgui
  uses: [ResourceUse {                     // ★ 資源の利用の正本。依存・barrier・論理 graph・descriptor はここから導く
          resource, role,                  // color_attachment / depth_attachment / sampled / storage / input_attachment / indirect / index / vertex / copy_src / copy_dst
          access, subresource, history, footprint,
          binding: { port, location,       // descriptor の位置 / attachment の位置
                     view, sampling, element, domain }  // descriptor を作るのに要る値は use に付ける(info に重ねない)
        }]
  after, before, regions, view_family, view_select, schedule, resolution_domain, ray_query
  info: variant<Fullscreen, Raster, Material, Compute, RayTracing, Aux, Snapshot, Anchor>
        // shader 参照・defines・Fullscreen / Raster の PipelineState 1 値・Material の override。資源は uses を指す
  provenance
}
```

- authored な形の拒否は `parseRenderGraph` で 1 回ずつ(名前付きコード + 対象名)。`PassFieldOwnershipCapabilities` は引数。
- `finalizeRenderGraph` は provider の列が終わった後に 1 回(`frame_node_order` は provider が足した node を含む最終の graph に効く)。
- IR は不変、variant ごとに 1 つ。preview も IR まで作る。

**追補(RC-3c1a、2026-10-24。上の形の意図は変えず、実装で決まった細部を足す)。**型は `src/project/rendergraphir.hpp`、parser は `src/project/rendergraphparse.{hpp,cpp}`。この段の IR は計画に要る値だけを持つ(実行用の値は RC-3c2 で `info` と資源に足す)。

- **attachment の use は解決済みの op を持つ**: role が `color_attachment` / `depth_attachment` の `ResourceUse` は、RC-3a2 の既定と優先順(attachment > pass > pass の型の既定)で解決した `PassAttachmentOperations operations`(`rendergraphir.hpp:143`。他の role は nullopt)と、`subresource`、`binding.location`(color の何番目か)を持つ。aspect は role で表す(color / depth)。parser が解決する(`rendergraphparse.cpp:877` の `attach`)。LOAD の attachment は access が `read_write` で、footprint `same_pixel` を parser が付ける(§2.3)。計画の node を導く側(`frameGraphDefinitionOf`)は use の footprint を読むだけで、自分では作らない(`rendergraphir.cpp:163` のコメント)。
- **graph ローカルの宣言は graph が持つ**: `GraphIR` は自分の `render_targets` / `buffers` を `images` / `buffers` に持ち、`RenderGraphIR` は config の宣言を持つ(`rendergraphir.hpp:281` / `:303`)。名前は ローカル → top-level の順に引く(`findImage`、`rendergraphir.cpp:325`)。planner の既知資源の列も ローカルの image → ローカルの buffer → config の image → config の buffer の順(`frameGraphDefinitionOf`、`rendergraphir.cpp:348`)。node の並びは graph の passes → graph 自身の compute task → config の compute task の写し(`scope = shared`、`rendergraphparse.cpp:1497` の `append_shared`)。graph ローカルの image は今までどおり計画用の読み(名前・`history`・snapshot の大きさ)だけで、登録はされない(RC-3c2 で扱いを決める。`rendergraphir.hpp:241` のコメント)。history の宣言の別表は持たない(`ImageResource.history`)。
- **合成専用の shape `acceleration_structure`**: `RenderNodeShape` の最後の値で、作者は書けない(`rendergraphir.hpp:65`)。info は `AccelerationStructureNodeInfo{named?}`(scene の分は名前なし)。`NodeDeclarationScope` は `graph` / `shared` / `synthesized` の 3 つ。parse の後に core がしていた 3 つは IR を受けて IR を返す project の関数になった: `namespaceSharedComputeTasks`(variant の接尾辞)、`insertAccelerationStructureNodes`(`__scene_acceleration_structure` と名前付きの分を各 graph の先頭に入れ、問い合わせる pass / task に待たせ、宣言順を振り直す)、`applyFrameNodeOrder`(`rendergraphir.hpp:399` / `:425` / `:431`)。この順で呼ぶのが RC-3c1b の `finalizeRenderGraph` の中身になる。RC-3c1a の時点では本番はまだ core の側(`detail::applyFrameNodeOrder` など)を通る。
- **fullscreen の info**: `FullscreenNodeInfo{implementation_provider?, vertex_shader?, fragment_shader?}`(`rendergraphir.hpp:166`)。`implementation` を書いた pass は `implementation_provider` を持つ(provider が文字列でなければ `""`)。provider の解決は前倒ししない。XR の multiview の候補(`renderGraphMultiviewCapableNodes`、`rendergraphir.hpp:436`)は `implementation` の無い fullscreen だけで、core の `targetViewExecutionRequest` の JSON の読み(`detail::multiviewCapableFullscreenPasses` に切り出し、`vulkanrendercompilerprogramdetail.hpp:47`)と同じ規則。
- **`ResourceUseRole` は出所で分けた**: 上の図の `sampled / storage / input_attachment / …` の列ではなく、`input`(pass の input・task の reads)/ `output`(task の writes)/ `resource_port` / `screen_input` / `surface_resource` / `material_resource` / `color_attachment` / `depth_attachment` / `indirect` / `copy_source` / `copy_destination`(`rendergraphir.hpp:75`)。理由(実装者の報告 §9-4): sampled か input attachment かは物理計画が決める(§2.3 の候補と採用の分離)ので parse の時点では決められない。local read の候補の規則も出所と shape で決まる(`isLocalReadShaderInput`、`rendergraphir.cpp:333`: history でない読みで、fullscreen / raster の `input`、material の `screen_inputs`、buffer でも storage でもない `material_resources`)。descriptor の種類は `binding.kind` / `binding.access` / `intent` に持つ。
- **`frameGraphDefinitionOf` は RC-3c1b までの橋**: IR の 1 graph から core の計画用 graph と同じ `FrameGraphDefinition`(node は 24 field)を導く project の関数(`rendergraphir.hpp:332`)。`planRenderGraph` と `toLogicalGraph` は、project に移した planner(`planFrameGraphWithNamedRefusals`)と shadow(`compileLogicalFrameGraphShadow`)にこれを渡す。RC-3c1a の橋の test は、この導出と core の計画用 parse の結果を 24 field で比べる(実装者の報告 §9-1: test に別の写しを書かず、本番の導出を直接検査する形にした)。RC-3c1b 以後、物理計画(`compileRenderingTargetPlans`)へ IR を渡すときにこれを使うか、消費者を IR に直すかは未確定(報告 §10)。
- **device facts は「この image を問う」形**: `RenderGraphDeviceFacts{tile_local_attachments, supports_local_read_attachment}` で、後者は `RenderGraphImageCapabilityQuery{format, usage, cube_compatible}` を受けて真偽を返す関数(`rendergraphir.hpp:342` / `:355`)。core が Vulkan に問う鍵(format・usage・cube 互換の作成 flag)と同じ。device が持てる (image, format) の集合(`renderGraphDeviceTileLocalFormats`)と、graph ごとの適格の判定(`renderGraphTileLocalFormats`: history の読み、local read の ABI、same_pixel、extent、attachment の subresource)を分け、どちらも IR の `ImageResource` から読む(core の `RenderTargetDefinition` に依存しない)。
- **`framePlanToJson` は core に残る**: FramePlan の型(`FramePlanNodeKind`・`FrameGraph*Definition`・`FramePlan*`)と planner・`validateFramePlanStructure`・`findFramePlanUnreadOutputs`・FO-1 の依存の照合は `src/project/frameplan.{hpp,cpp}` に移り、core の `frameplanner.hpp` はそれを include するだけになった。serializer(view family の軸、`renderViewFamilyExtentJson`)は core の `frameplanner.cpp` のまま(`frameplanner.hpp:51`)。
- **まだ持たないもの**: `RenderGraphIR` の `variant` / `policy`、`NodeIR` の `provenance`、`BufferResource` の名前以外(size・layout・host source・view family・initial)。RC-3c1b / RC-3c2 で足す。

**追補(RC-3c1b、2026-10-24。上の形と RC-3c1a の追補のうち、本番の切り替えで今の事実と変わった所。細部は §5 の「RC-3c1b で決まった細部」)。**

- **本番は IR を通る**: RC-3c1a の追補の「本番はまだ core の側(`detail::applyFrameNodeOrder` など)を通る」「`frameGraphDefinitionOf` は RC-3c1b までの橋」は過去の状態。今は本番の 7 箇所が IR を通り、`frameGraphDefinitionOf` は planner・shadow・物理計画の部品が `FrameGraphDefinition` を読むための導出として残る(物理計画の部品を IR に直すのは後の段)。
- **provenance は graph が持つ**: 上の図の `NodeIR.provenance` ではなく、`GraphIR` の `render_strategy` / `graph_transforms` / `subgraph_replacements`(graph 単位。node 単位の provenance は持たない)。parse は空で作り、provider の列の後に compiler が書く。
- **IR が足した欄**: `RenderGraphIR::ray_task_acceleration_structures`(表の形と値。解決は finalize)、`ImageResource::format_unresolved`(`format_class` だけで format の無い config image)、`ImageResource::snapshot_extent_pending` が format 未解決でも立つこと。`RenderGraphIR` の `variant` / `policy` と `BufferResource` の名前以外は、まだ持たない(RC-3c2)。
- **`finalizeRenderGraph` は 3 つより多い**: RC-3c1a の追補の「3 つをこの順で呼ぶのが finalize の中身」に、前に format の解決の検査・`resolveRenderGraphReferences`・`requireRenderGraphRayQueryCapability` が加わった。preview は finalize を通らず、`resolveRenderGraphReferences` だけを通る(「preview も IR まで作る」はこの形)。
- **multiview の候補**: `targetViewExecutionRequest` は IR の `renderGraphMultiviewCapableNodes` と node の shape・`schedule`・`view_family` から作る。RC-3c1a の追補が書いた core の JSON の読み `detail::multiviewCapableFullscreenPasses` は消えた。

### 2.3 footprint と local read の規則

| 読み | footprint の既定 | 拒否 |
|---|---|---|
| fullscreen / raster の `input`(shader input) | `input_footprints` の宣言。無ければ arbitrary | — |
| compute の `reads` | `read_footprints` の宣言。無ければ arbitrary | — |
| attachment の LOAD | same_pixel(導出) | — |
| material の `material_resources` | 宣言の `footprint`。省略は arbitrary。`@history` は temporal | `@history` に明示の footprint、same_pixel と cube / 明示の subresource の組み合わせ |
| material の `screen_inputs` | screen の組み込み契約(`makeBuiltinMaterialScreenInputContract`) | — |
| material の `surface_resources` | pass input の組み込み契約(`makeBuiltinMaterialPassInputContract`。feature が持つ shadow の契約を含む) | — |
| その他の `@history` | temporal(導出) | — |

- **local read は「候補」と「採用」を分ける**: 候補は「same_pixel かつ role が shader input」。採用は物理計画が決める(attachment の extent の一致などの物理条件。今の `renderingsamplecount.cpp:1027`)。
- **追補(RC-3c1a)**: history の use は入口を問わず temporal を持つ。pass の `input`、compute task の `reads`、`resource_ports`、`material_resources` の `@history` は、parser が IR の use の `footprint` に temporal を入れる(`rendergraphparse.cpp:323` / `:669` / `:804` / `:970`)。宣言の無い読みの既定は project の 1 か所 `undeclaredReadFootprint(history)`(`logicalrendergraph.hpp:137`。history は temporal、current は arbitrary)で、新 parser・core の material resource の実行用 parser(`materialpassinfojsonparser.cpp:860`)・旧 planner(`frameplanner.cpp:414`)・shadow(`logicalframegraphshadow.cpp:390` / `:444`)がここから取る。current の `input` / `reads` で宣言の無いものは、IR の use では `footprint` が nullopt のままで、読む側が arbitrary として扱う(`rendergraphir.hpp:136` のコメント)。
- **追補(RC-3c1a)**: 表の拒否(material の `same_pixel` と cube view / 明示の subresource)は、新 parser では実行用 parser を待たずに parse で断る(`render_graph.footprint_invalid@1`、reason `same_pixel_cube_view` / `same_pixel_subresource`。`rendergraphparse.cpp:679`)。旧の計画用の道はこれを受け、実行用 parser が断っていた。

### 2.4 generation の資源の記述(variant 間で統合)

- `resolveGenerationResources(全 variant の確定 IR と物理計画, output extent, engine の規則)` を project の純関数にし、**資源の検査と GPU 登録の両方がこの結果だけを使う**。
- 統合: usage は和集合、layers は最大、storage が食い違えば materialized。**alias は group 単位で確定**する: 互換を失った group は全メンバーの共有を解除し、最後に「各 group のメンバー数 ≥ 2、契約が一致」を検査する(今は target ごとに比べるので、一方の variant に無い member が 1 人の group として残る)。
- engine が足す usage(sampled、history の転送、XR の外部 depth)はこの関数の中の 1 箇所の規則。
- runtime に変わる値(shadow の layers)は静的な制約を記述に持ち、変わった時の検査を runtime に残す。

### 2.5 pipeline の状態は「所有者ごと」

- fullscreen / raster: pass が `PipelineState` を 1 値持つ。
- material: 状態の正本は material(`SurfaceRenderState`)。pass は override だけを持つ。検査は「pass × material × device facts」。material の資産は IR に取り込まない。
  pass の override は群ごとの欄を持つ構造: 今は attachment の群(`material_output_states`)だけ。後で depth / stencil の群を同じ層に足せる(pass 単位の stencil mask。PSN-3 / PSN-4。v1feature の要望)。

### 2.6 順序と実行

- node の順は planner だけが決める。物理計画は FramePlan の順を入力に取り、その順の連続区間で scope を切る(指定順を受ける入口は今もある: `targetrenderplanning.cpp:851`、`:2131`)。
- generation で前計算するのは scope ↔ node の対応まで。view の数・`view_select` の解決・実行要否はフレームごと。
- 検査: 静的には「scope を展開した node 順 == FramePlan の順」。runtime には view を含む呼び出しの依存と scope の連続性(今の scheduler の検査を残す)。

### 2.7 core に残すもの

- 中立値 → vk の写像を 1 つの翻訳単位に。device への問い合わせ → 中立な device facts。
- GPU 資源の登録(§2.4 の記述から)、pipeline、実行。runtime に残すのは確定 IR から導いた不変の物だけ。normalized JSON は保持しない。

### 2.8 JSON を読む所(移行後)

| 読み手 | 理由 |
|---|---|
| lowering と compose(render strategy を含む) | feature の断片を組む作業。正規形を前提に、target 参照は 1 組の helper |
| graph transform / subgraph の provider | 公開 ABI が JSON(変えない)。応答は `parseRenderGraph` で 1 回 |
| `parseRenderGraph` | 唯一の JSON parser |
| Studio の authoring context | 宣言・provenance・診断を見せる(§2.9) |

**追補(RC-3c1b、2026-10-24)**: 上の表は RC-3c2 まで終えた「移行後」の形。RC-3c1b の時点では、計画(FramePlan・論理 graph・物理計画)の JSON の読みは `parseRenderGraph` だけになったが、core にはまだ次の読みが残る: registration の実行用 parser(render target・buffer・compute task・pass。RC-3c2)、`validateGpuDrawSourceBufferContracts` の gpu draw の buffer の読み(buffer の宣言がまだ IR に無い。RC-3c2)、`detail::namespaceComputeTaskDefinitions` の `compute_tasks` の読み(実行用の改名と pass provenance の同期)、compiler が作者の root の `frame_node_order` の graph の鍵を読む横断検査、ImGui の pass の追加(改訂 0 の前に JSON を書く)、`renderer_config.cpp` の fallback の判定。editor の 3 箇所(render target の使用者、restart の検査、planning の名前)は `parseRenderGraph` の IR を読む。

### 2.9 Studio の復旧の契約

- **原文・文書の識別子・digest は、前処理(lowering / 合成 / 正規化 / parse)の成否と無関係に取れる**。その上に、成功した段までの provenance と、各段の診断を載せる(今ある候補文書の集合を使う。別の緩い compiler は作らない)。
- 候補の検証と公開は厳格に断る。壊れた文書(compose の失敗を含む)を Studio で直して保存できる経路を残す。

## 3. 削るもの / 触らないもの

| 対象 | 扱い |
|---|---|
| 計画用 parse 一式・実行用 parse 一式・compiler の JSON の部分読み 3 つ | 削る(`parseRenderGraph` に置き換え) |
| `makeFrameGraphDefinition` 一式(約 240 行、test だけ)、`parseInputTargetsFromJson`(呼び出し 0)、`makePassInfo(string)`(test だけ) | 削る |
| `PassDefinition.resolution_domain` / `region_tags` / `once`、`ComputeTaskDefinition.after/before`(本番に読み手なし) | 削る(IR に移る) |
| `compiler_only`、`allow_legacy_type_fallback=false`(test だけ) | 削る |
| 写しの enum(load / store 3 種、sampling 2 種、view layout 2 種、cull / depth compare 2 種) | 1 つに(**済み**: RC-3a2 4114e2607。attachment の aspect の写し 3 種も 1 つに) |
| `frameNodeDependencies` | 削る(planner の辺) |
| 段階 2 の policy の作り直しと比較 | 削る |
| `GpuDrawSourceDefinition.fallback` | 触らない(Studio が欄を出し JSON に書く) |
| `LogicalPortRelation` | 触らない(公開 ABI・fingerprint・物理計画の検証が読む) |
| 差し替え可能な compiler program と complete physical plan の install | 利用者の判断(§6 D1) |

## 4. format の性質表

- `ImageFormat`(project の enum、作者の綴りを正、46 種)と性質表:
  - aspect(color / depth / stencil / depth+stencil)
  - **成分ごとの格納の型**(unorm / snorm / uint / sint / sfloat / srgb)。depth+stencil は depth と stencil の成分を別々に持つ(D32S8 = depth: sfloat、stencil: uint)
  - **shader が見る型**(float / int / uint)は格納の型から導く別の性質(混ぜない)
  - 記憶容量の見積もり bytes(RC-1 の表と同じ値。D32S8 は 8)
- Vulkan の texel block size(D32S8 は 5)は別の性質で、使うなら core が vk の traits から取る。性質表と vk の traits の照合 test は aspect と成分ごとの格納の型。
- 物理計画の dump の format の綴りは作者の形に揃う(§6 D3)。

## 5. 段階(WP)

受け入れは本番の経路を通し、**解決した具体値を期待値と比べ**、同じ test の中で対照を走らせる。

| 段 | 内容 | 受け入れ |
|---|---|---|
| RC-3a 語彙(**RC-3a1 済み**: 5446196f0 = format の表・usage・`Extent2D`。**RC-3a2 済み(4114e2607)**: load / store・写しの enum・`RenderResolutionDomain`・gpu draw / buffer / indirect の宣言・uint64 の大きさ) | §4 の format の表、usage、load / store(既定と優先順)、`Extent2D`、`RenderResolutionDomain`、gpu draw の宣言、buffer / compute の大きさ(uint64)を project に。写しの enum を統合。core は写像 | 全 GPU golden・headless・Studio シナリオが緑(挙動不変)。dump の format の綴りの変更だけ fixture を更新 |
| RC-3b 正規化(**済み**: 70aa7c0b1 / afd204203) | §2.1 を全ての入口に。内部 generator を正規形に。target 参照の読みと書き換えを 1 組の helper に | 短縮形を root・feature・provider 応答のそれぞれに置いた project を本番の loader で読み、**解決した writer → reader の対応・FramePlan の順・出力先(swapchain → display、HDR の中間資源、`$dst` の parameter 先)が期待値どおり**。対照: 正規化を外すと、同じ入力が所定の名前付きコードで断られる |
| RC-3c1 IR と計画(2 つに分けた。**RC-3c1a・RC-3c1b 済み**。RC-3c1a(branch `agent/rc3c1a`、15d7face2 / 17febe435 / 8648fbf0f): IR・`parseRenderGraph`・IR → FramePlan / 論理 graph を project に足し、当時の core の道と橋の test で一致を固定(本番はまだ切り替えない)。RC-3c1b(branch `agent/rc3c1b`、955632902 / 04c1dd6e9 / 512419e29): 本番の 7 箇所の切り替え、provider の改訂ごとの IR の引き継ぎ、`finalizeRenderGraph`、core の計画用 parse・部分読み・橋の test の削除。受け入れ(右の列)は RC-3c1b で満たした。細部は下の「RC-3c1b で決まった細部」) | `parseRenderGraph`(§2.2、§2.3)、provider の改訂ごとの IR の引き継ぎ、`finalizeRenderGraph`、FramePlan・論理 graph を IR から(project)。core の計画用 parse と部分読みを削る | 本番の registration を通して、`parseRenderGraph` の呼び出し回数 == 完成した graph の改訂の数(transform 1 個なら 2)。provider が足した node を `frame_node_order` が指せる。拒否は入力ごとにコードと対象名を検査。golden・headless 緑 |
| RC-3c2 資源と registration | §2.4、資源の検査の純ロジック化、registration を確定 IR + 資源の記述から(実行用 parse を削り、normalized JSON を保持しない)、§2.5 の material の検査 | XR の外部 depth を足した 2 variant と、片方の variant に無い alias member を持つ構成で、**検査と GPU 登録が消費した format・usage・extent・samples・layers・alias group が期待値(usage の和集合、layers の最大、group の解除)どおり**。対照: 統合の各規則を外すと、その具体的な値の欠落が検出される。shadow の light 数を変えた時の layers の検査が効く。ここで「1 改訂 1 parse」が完成 |
| RC-3d 順序 | §2.6。物理計画は FramePlan の順を入力に、scope ↔ node の対応を前計算、`frameNodeDependencies` を削る | 融合 scope + 2 view の構成で、実際の呼び出し列が A₀→B₀→A₁→B₁、scope を展開した node 順が FramePlan と一致。対照: 物理計画に別の順を渡すと静的検査が名前付きで断る |
| RC-3e Studio | §2.9 | 正常な編集が公開される / 不正な候補は名前付きで断られ公開中の generation が変わらない / **compose で失敗する文書(存在しない node を指す `snapshots[].after` など)と IR で失敗する文書の両方**で原文・digest が取れ、直して保存できる、を同じ test で |
| (後) | 変わっていない部分の再 compile を省く、窓の大きさの変更で全段に入らない | — |

- RC-3b で決まった細部: 入口は `lowerAuthoringDocument` と `composeRenderFeatureConfig` の先頭(lowering の前。core の `loadRenderingPassConfigJsonFromString` には置かない)・preset・feature の読み込み・graph の lower の結果で、ImGui・template・canvas ship・fallback の root は正規形を直接書き、合成の出口で `requireNormalizedRenderingJson`。core の parser は短縮形を受けたまま(RC-3c で置き換わる)。
- provider の応答(graph transform / subgraph replacement / render strategy)は `normalizeProviderRenderingJson`(正規化してから検査)。入力側も正規化する(graph transform は最初の要求の前、subgraph は要求の前、strategy は seed。identity の strategy で fingerprint が揃う)。
- Studio の feature 一覧(`list_render_features`)は読んだ feature を正規化し、読めない文書に `document_error: {code, detail}` か `{message}` を付ける(Studio はまだ読まない)。folded pass の色の行は attachment object の `target` を読む(afd204203)。
- RC-3a1 で決まった細部(5446196f0): §4 の表は `src/project/imageformat.{hpp,cpp}` の `ImageFormat`(46 値)と性質表 1 つ。格納の型に **ufloat** を足した(`B10G11R11_UFLOAT_PACK32`)。表に「作者が書けるか」を持たせ、作者が書けるのは 45(`D32_SFLOAT_S8_UINT` は内部)、usage も作者の 6 + engine が足す input / transient attachment(`imageusage.{hpp,cpp}`)で、作者の入力の受理と拒否は前と同じ。`Extent2D` は新しい型を作らず既存の `ResolvedResourceExtent`(`resourceextent.hpp` に移した)。vk への写像は core の `vulkanimagevocabulary.{hpp,cpp}` の 1 か所。D3 の綴りは物理計画・`get_frame_plan` の dump・fingerprint・fragment と complete plan の package に適用し、v0 の package の vk の綴りは `imageFormatFromV0PhysicalPlanName` の 1 か所で読み替える(fingerprint が変わるので、v0 で eject した package は stale として名前付きで断られ、eject し直しが要る)。snapshot は bytes を表から取るが、受ける 12 種の集合は別の契約のまま。compose の depth 判定は表の aspect。RC-3a の行のうち gpu draw の宣言を RC-3a2 で扱うかは、RC-3a2 の仕様が出るまで未確認。
- RC-3a2 で決まった細部(4114e2607): load / store は project の `AttachmentLoadOp {load, clear, dont_care}` / `AttachmentStoreOp {store, dont_care}` / `AttachmentAspect`(`src/project/attachmentoperations.{hpp,cpp}`)。既定(`defaultPassAttachmentOperations`、pass の型と aspect から)と優先順(`resolveAttachmentOperations`、attachment > pass > 既定)も同じ所の 1 か所で、計画と実行の両方が呼ぶ。計画・物理計画の attachment は解決済みの op を既定構築の無い `PassAttachmentOperations operations` 1 欄で持ち、省略できない。綴りの表は 1 つで、受ける集合は入口ごと(作者: 大文字小文字の別綴りを含む load 7 / store 5、正規化後: `clear` / `load` / `dont_care`・`store` / `dont_care`、fragment / complete plan の wire: `clear` / `load` / `discard`・`store` / `discard`)。wire の出力の綴りと fingerprint は不変。作者の別綴りの受理は core の parser に残る(RC-3c まで)。正規化の綴りの表(`renderingjsonnormalize.cpp`)は別のまま。vk への写像は `vulkanimagevocabulary` の 1 か所、記録の値は `src/core/vkcore/recordedattachmentops.hpp`。sampling は `ShaderResourcePort*` に、view の次元は `VulkanResourceViewLayout` に、cull / depth compare は `RasterCullMode` / `RasterDepthCompare` に(型だけ。既定は所有者ごとのまま: `SurfaceRenderState` と `PipelineState`)。`RenderResolutionDomain` は `src/project/renderresolutiondomain.{hpp,cpp}`(既定は pass の型の enum で決め、Studio の schema の既定もそこから)。gpu draw の作者の宣言は project の `GpuDrawSourceDeclaration`(`gpudrawsource.hpp`)で、core の `GpuDrawSourceDefinition` はそれを継承して buffer の ID を足すだけ。buffer / compute の indirect の宣言は project の `framegraphbufferdefinition.{hpp,cpp}`、大きさは uint64(indexed draw の 20 bytes は core の `static_assert` で vk と照合)。
- RC-3c1a で決まった細部(branch `agent/rc3c1a`、15d7face2 / 17febe435 / 8648fbf0f。数は実装者の報告 `rc3c1a_report.md` §6・§14、GPU は統合側の全数): **新しい道を足しただけで、本番の経路は今の core の道のまま**(core の計画用 parse `parseFrameGraphDefinitionsFromConfigJson` を本番で呼ぶ 7 箇所: `renderconfigeditor.cpp` の 3 つ、`graphtransformregistry.cpp`、`subgraphreplacementregistry.cpp`、`vulkanrendercompilerprogram.cpp` の 2 つ)。
  - 足した物(すべて project、`pelican_project` だけで link できる): `rendergraphir.{hpp,cpp}`(§2.2 の型と追補、`frameGraphDefinitionOf`・`planRenderGraph`・`toLogicalGraph`・tile-local の判定・IR → IR の 3 関数・multiview の候補)、`rendergraphparse.{hpp,cpp}`(`parseRenderGraph`。正規形だけを受け、top-level の `passes` / `compute_tasks` だけの形は `render.json_not_normalized@1`)、`imagesource.{hpp,cpp}`(`ImageSourceDeclaration` と既定。core の `RenderTargetSource` はその別名)、§2.3 の `undeclaredReadFootprint`(`logicalrendergraph.hpp`)。
  - 置き場の移動(中身は不変): FramePlan の型・planner・`validateFramePlanStructure`・`findFramePlanUnreadOutputs`・`framePlanNodeKindName`・FO-1 の `frameNodeDependencies` / `requireFrameNodeOrderAgreesWithDependencies` を `frameplan.{hpp,cpp}` へ(core の `frameplanner.hpp` は include するだけ)。`compileLogicalFrameGraphShadow` を `logicalframegraphshadow.{hpp,cpp}` へ(core の `logicalframegraphadapter.cpp` は削除、`.hpp` は include だけ)。planner の拒否は 2 通りの綴りで出せる(`planFrameGraph` は旧の文、`planFrameGraphWithNamedRefusals` は名前付き。規則は 1 つ)。`framePlanToJson` は core に残る。
  - RC-3c1b で消す前提で core に足した test の継ぎ目: `compileRenderingLogicalGraphs` の device facts 付きの多重定義(`renderingsamplecount.hpp`)、`detail::multiviewCapableFullscreenPasses` と `detail::requireRayQueryPlanningCapability`(`vulkanrendercompilerprogramdetail.hpp`。本体から切り出しただけで、本番もこれを呼ぶ)。`FrameGraphDefinition` → IR の適合層は作っていない(新しい道は JSON から直接 IR を作る)。
  - 橋の test(`test/rendergraphir_bridge_test.cpp`、GROUP render_planning_core): 今の道(normalize → core の計画用 parse → planner / shadow)と新しい道(normalize → `parseRenderGraph` → `planRenderGraph` / `toLogicalGraph`)を、定義の 24 field・FramePlan(順・barrier・node の全 field)・論理 graph(資源・型・materialization・node・use)で比べ、最初に食い違った path を出す。入力は 71(出荷 project 7・feature を足した構成 28・frameplanner_test の plan fixture 9・error fixture 3・静的な rendering config 20・graph transform / subgraph の config 4)。単一 graph の形の fixture は機械的な包み `wrapSingleGraph` で正規形にし、旧の道の結果が包む前後で同じことを同じ test で確かめる。ほかに専用の構成(graph ローカルの宣言、LOAD の対照、tile-local と device facts、multiview、AS の挿入を独立に書いた期待値、namespace と `frame_node_order`、compute だけの ray query、history の temporal、resource port の向き)。拒否は 3 群を入力ごとにコードと対象名で: (a) 旧の計画用 parse が断る 38、(b) 旧の planner が断る 8(新も `planRenderGraph` で断る)、(c) 実行用 parser が断っていたものを新 parser に前倒し 7。
  - `pelican_project` だけを link する専用の exe `pelican_test_render_graph_ir_boundary`(group に入れない)が JSON → IR → finalize の 3 関数 → FramePlan → 論理 graph を通す。
  - GPU: 本番の経路は変えていないので必須の GPU test は無い想定(報告 §8)。統合側の GPU ラベルの全数は、既知の 2 件(`WP369 production material alpha profile matches mip zero oracle and next frame overrides` と `player_arguments_ds1_path`)以外は緑(1 件 skip)。
  - RC-3c1b への申し送り(報告 §10): 上の 7 箇所の切り替え、`finalizeRenderGraph` = `namespaceSharedComputeTasks` → `insertAccelerationStructureNodes` → `applyFrameNodeOrder`、物理計画は `FrameGraphDefinition` と `RenderTargetDefinition` を受けたまま(`frameGraphDefinitionOf` を使うか消費者を IR に直す)、消すもの(core の計画用 parse 一式と `makeFrameGraphDefinition` 一式、`validateFramePlanPassInputsProduced` / `targetViewExecutionRequest` / `validateGpuDrawSourceBufferContracts` の JSON の読み、core の `shadowOptions` / `graphTileLocalAttachmentFormats`、上の test の継ぎ目、橋の test、frameplanner_test の単一 graph の形の入力は包みを通す書き換え)。
- RC-3c1b で決まった細部(branch `agent/rc3c1b`、955632902 / 04c1dd6e9 / 512419e29。数と実測は実装者の報告 `rc3c1b_report.md`。§19 がコードレビュー後、§20 が確認レビュー後の最新): **本番の 7 箇所(editor の E1〜E3、provider の G / S、compiler の C1 / C2)が `parseRenderGraph` → IR → `finalizeRenderGraph` → `planRenderGraph` / `toLogicalGraph` / 物理計画 を通る**。core に JSON → `FrameGraphDefinition` の道は無く、`FrameGraphDefinition` を作るのは project の `frameGraphDefinitionOf` だけ。
  - **1 改訂 1 parse と provider の列**: compiler は ImGui を足した直後(`compose_runtime_config` の後)に 1 回 parse し(改訂 0。[`vulkanrendercompilerprogram.cpp:391`](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L391))、[`resolveLogicalGraphTransforms`](../src/core/renderingpass/graphtransformregistry.hpp#L145) / [`resolveTaggedSubgraphReplacements`](../src/core/renderingpass/subgraphreplacementregistry.hpp#L144) に「入力の改訂の JSON と IR」を渡す。両者は応答(transform の応答、subgraph の splice 後の config)を正規化して 1 回だけ parse し([transform](../src/core/renderingpass/graphtransformregistry.cpp#L1270)、[subgraph](../src/core/renderingpass/subgraphreplacementregistry.cpp#L1349))、その IR を要求の「後」と次の要求の「前」の両方に使い、`{config, render_graph, ...}` で最終改訂の JSON と IR を返す。JSON は provider の ABI(`config_json`、subgraph の authored JSON)・保護された構造の比較・splice・実行用 parser のために残る。回数は runtime の variant ごとに `1 + T + S`(T = transform の応答の数、S = subgraph の splice の数)、preview(data_only)は compose が成功して parse に届いたとき 1 回。数える口は [`renderGraphParseCount()`](../src/project/rendergraphparse.hpp#L57)(thread_local の計数)。
    - RC-3b で入れた「provider の入力側の正規化」(transform の最初の要求の前、subgraph の要求の前)は消した。渡す IR を作った時点で `parseRenderGraph` が正規形以外を `render.json_not_normalized@1` で断っているため(実装者の報告 §12 の 6)。応答側の `normalizeProviderRenderingJson` は残る。
  - **provenance は `GraphIR` に**: [`GraphIR`](../src/project/rendergraphir.hpp#L288) が `render_strategy` / `graph_transforms` / `subgraph_replacements` を持ち、`apply*Selections` の 3 つは IR に書く([書く所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L411))。`DefaultLogicalVariantCompilation` の provenance の欄は消え、`{compiled_pipeline, composed_config, render_graph}` になった([`vulkanrendercompilerprogram.cpp:203`](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L203))。parse は provenance を空で作るので、provider が受ける論理 graph には provenance が無い(fingerprint は純粋な graph のまま)。`frameGraphDefinitionOf` が 3 欄を写し([写す所](../src/project/rendergraphir.cpp#L372))、論理 graph と物理計画の logical-graph fingerprint に入る(前と同じ流れ)。
    - B3(fingerprint 不変): 切り替え前の本番(develop_v1 = 83e07e19b を build した binary)で、出荷 7 project の flat / xr と provenance 3 種がすべて空でない provider 付きの構成(計 15 graph)を本番の compiler program に通して、物理計画の 2 つの fingerprint・provenance の全欄・eject した pin / physical fragment・frame plan の順・provider が受けた ABI の値・provider の登録状態を記録し、新 binary の値と比べて差 0。旧 binary で eject した pin 15 / fragment 15 は新 binary で全部 install・適用された。対照(provenance の写しを外す)は provider 付きの構成だけ 24 箇所が変わった(実装者の報告 §3。記録用の test は repo に残していない)。
  - **`finalizeRenderGraph` の中身と順**([`rendergraphir.cpp:809`](../src/project/rendergraphir.cpp#L809)、要求は [`FinalizeRenderGraphRequest`](../src/project/rendergraphir.hpp#L460)): (1) config の image の format が全部解決済み(`render_graph.image_format_unresolved@1 {resource}`)→ (2) `resolveRenderGraphReferences` → (3) `requireRenderGraphRayQueryCapability` → (4) `namespaceSharedComputeTasks` → (5) `insertAccelerationStructureNodes` → (6) `applyFrameNodeOrder`(variant ごとは `require_all_graphs = false`)。LP-Va / VF-1 / VF-2 と ray bindless の graph 名は finalize の前(切り替え前と同じ位置。LP-Va / VF は core の関数を `frameGraphDefinitionOf` の結果で呼ぶ)。実行用の `ComputeTaskDefinition` の改名と pass provenance の同期は core の [`detail::namespaceComputeTaskDefinitions`](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L147) に残る(名前の衝突の検査は IR 側が先に済ませる)。
    - §2 の図の「finalize = `#xr` 改名・AS node・frame_node_order」に、(1)〜(3) が加わった。
  - **参照の解決は provider の列の後**: [`resolveRenderGraphReferences`](../src/project/rendergraphir.cpp#L687) は `ray_task_acceleration_structures` の解決(表の task が config の compute task にあり、ray work を持つ。`render_graph.compute_task_invalid@1 {node, field, reason: unknown_task | task_declares_no_ray_work}`)と、config の task と raster 系の pass が選ぶ AS が宣言済みであること(`render_graph.acceleration_structure_unknown@1 {node, acceleration_structure}`。名前は作者の task 名で、接尾辞の前)。AS の存在の検査([`requireSelectedAccelerationStructuresDeclared`](../src/project/rendergraphir.cpp#L663))を呼ぶのはこの関数の 1 か所だけで、(5) の `insertAccelerationStructureNodes` が断るのは予約名(`render_graph.acceleration_structure_reserved@1`)だけ(選ばれた AS は (2) で確かめ済み。ヘッダのコメント [`rendergraphir.hpp:514`](../src/project/rendergraphir.hpp#L514))。報告 §19 の 5 は `insertAccelerationStructureNodes` の中の同じ検査を消したと書いたが呼び出しが 1 つ残っており、§20 の 4 で消えた。表は provider が後で足す task を指してよいので、`parseRenderGraph` は表の形と値を IR([`RenderGraphIR::ray_task_acceleration_structures`](../src/project/rendergraphir.hpp#L330))に持つだけにした(RC-3c1a の (c) 群のうち表の 3 件の拒否の場所が parse から finalize に移った)。preview(data_only)も parse の直後にこの関数を preview の try の中で呼ぶ([`vulkanrendercompilerprogram.cpp:367`](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L367))ので、preview が外した task を表が指せば preview だけが unavailable になり、runtime の variant は成功する。preview の program を作る道は registration の [`previewGraphProgramOf`](../src/core/renderingpass/renderingpassconfigregistration.cpp#L1655)(data_only の成果物 → program、作れなければ unavailable)の 1 つだけなので、preview の program は必ずこの解決を通る。参照の解決を通らなかった単独の入口 `precompilePreviewGraph` は消えた(報告 §20 の 1。本番に呼び手は無く、test は registration と同じ道に置き換えた)。
  - **途中の改訂の構造検査と最終の書き込み順の検査を分けた**: planner の 8 種の拒否(frameplan.hpp の [`planFrameGraph`](../src/project/frameplan.hpp#L194) のコメント)のうち、順の決まらない 2 writer(`render_graph.ambiguous_writes@1`)だけは最終の `frame_node_order` で合法になりうる。そこで [`requireFrameGraphStructure`](../src/project/frameplan.hpp#L200) を足し(2 writer 以外の 7 種)、[`toLogicalGraph`](../src/project/rendergraphir.hpp#L447) は論理 graph を作る前にこれだけを呼ぶ。provider の前後(G / S)と E2 の restart 検査は `toLogicalGraph` を通るので 7 種を名前付きで先に断り、2 writer は finalize の後の C1 の `planRenderGraph` だけが断る。各改訂で要る検査(正規形、pass の形・所有・attachment・footprint・history の参照、image / buffer の宣言、snapshot の大きさ、graph 名、`frame_node_order` と表の形、compute task の欄)は `parseRenderGraph`、最終改訂だけの検査は finalize と C1(planner の 8 種、入力の産出、物理計画、program を横断する `frame_node_order` の graph 名)。
  - **C1 の順**: 最終 IR → render target の実行用 parser → LP-Va / VF-1 / VF-2 → ray bindless → finalize → buffer / compute task の実行用 parser → `validateGpuDrawSourceBufferContracts` → `detail::namespaceComputeTaskDefinitions` → graph ごとに `planRenderGraph` + [`requireRenderGraphInputsProduced`](../src/project/rendergraphir.hpp#L365) → `targetViewExecutionRequest`(IR から)→ 物理計画(IR)([`compileDefaultVulkanVariant`](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L432))。切り替え前は物理計画の後に planner が走っていたが、名前付きの拒否を先に出すため planner を物理計画の前に移した。
  - **物理計画は IR を受ける**: [`compileRenderingTargetPlans(ForVulkanDevice)`](../src/core/renderingpass/renderingsamplecount.hpp#L159) は `(const RenderGraphIR &, span<const RenderTargetDefinition>, ...)`。中の部品(attachmentResources・physicalAttachmentPlans・geometryNodes・resolution plan・once の制約)は graph ごとの `frameGraphDefinitionOf` の結果を読み([作る所](../src/core/renderingpass/renderingsamplecount.cpp#L1494))、論理 graph は `toLogicalGraph(IR, graph, {device})`、tile-local は project の `renderGraphTileLocalFormats` / `renderGraphDeviceTileLocalFormats`。device facts は core の [`renderGraphDeviceFactsOf`](../src/core/renderingpass/renderingsamplecount.cpp#L803) が `queryFormatCapability` から作る(tile-local は transient かつ local read の device、問いは format・usage・cube 互換、真は image の usage と local read の両方に対応するとき)。RC-3c1a の追補の「`frameGraphDefinitionOf` を使うか消費者を IR に直すかは未確定」は、この段では前者に決めた(部品の IR 化は物理計画を直す段)。core の `shadowOptions` / `graphTileLocalAttachmentFormats` / `tileLocalAttachmentFormats` / `knownResources` / `nodeHasSamePixelRead` は消えた。
  - **4 つの判定を 1 つに**: 正本は project の [`ImageDeclarationFacts`](../src/project/rendergraphir.hpp#L371) の上の定義(`isTransientAttachmentCandidate` / `isTileLocalAttachmentCandidate` / `isRuntimeAliasEligible` / `nodeHasSamePixelRead`)。core は `RenderTargetDefinition` → facts の写しを [`declarationFactsOf`](../src/core/renderingpass/renderingsamplecount.cpp#L52) の 1 か所に持ち、transient / tile-local はそれを呼ぶだけ。alias だけは入力が違う: project の `isRuntimeAliasEligible` は宣言の上の適格性(history・source・persistent でない、format class が display でない、usage がちょうど color attachment + sampled)で、物理計画の後の core の使い手(pattern binding、`validateRuntimePhysicalPlan`、`applyRenderingTargetPlan` の alias の検査)は [`isRuntimeAliasCandidate`](../src/core/renderingpass/renderingsamplecount.cpp#L82) = 適格 && `storage_mode == materialized` を明示して使う。
  - **format 未解決の image**: resolver_version 2 の作者の image は `format_class` だけでよく、format は runtime package の解決(`resolveRenderTargetFormatClassesV2`)で決まる。editor の compose(E1 / E3)と preview(C2)はこの解決を通らないので、`format_class` があって `format` が無い config image を [`ImageResource::format_unresolved`](../src/project/rendergraphir.hpp#L270) で受け、固定の大きさでも snapshot の byte 数は出さず `snapshot_extent_pending` にする。format が要る所(finalize の最初、device への問い `renderGraphDeviceTileLocalFormats`)で `render_graph.image_format_unresolved@1 {resource}` と名前付きで断る。本番の runtime は class を解決してから parse するので起きない。切り替え前の E2 は render target の実行用 parser(format を要る)も通していたので class だけの image を断っていたが、今は受ける(実装者の報告 §6)。
  - **拒否は最初に断る場所で名前付き(D2)**: `planFrameGraph` の旧の文(`Cycle detected in frame graph` など)と `planFrameGraphWithNamedRefusals` は 1 つになり、名前付きだけ。`toLogicalGraph` が planner の構造の検査を先に走らせるので、E2 / G / S / C1 のどこでも planner の code が logical shadow の文の拒否より先に出る(実装者の報告 §12 の 1。shadow の拒否の文そのものは書き換えていない)。`validateFramePlanPassInputsProduced` は `requireRenderGraphInputsProduced`(`render_graph.input_not_produced@1 {graph, node, resource}`)になり、対象は graph の pass(`scope == graph`、compute でない)の history でない `input` と snapshot の `source` だけ(config の buffer と、`initial` / file の `source` を持つ config の image は frame の前から中身がある。compute の `reads` は対象外で、persistent image を読み書きする compute は断らない)。graph 名の空・重複は `render_graph.graph_name_invalid@1`、render target 名の重複は `render_graph.image_invalid@1 {resource, field: name, reason: duplicate}` で parse が断る。program を横断する `frame_node_order` の graph 名の検査は `render_graph.frame_node_order_invalid@1` / `render_graph.frame_node_order_unknown@1 {graph}`([`vulkanrendercompilerprogram.cpp:733`](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L733)。各 variant の `RenderCompilerProgramVariantOutput::render_graph` の graph 名を集める)。作者が書いた重複 node 名・重複 graph 名・重複 render target 名・宣言の無い入力は、その前に compose が断る(名前付きでない物を含む。この段の範囲外)。
  - **共通の `preflightRejection`**: editor の [`RenderConfigEditorService::preflightRejection`](../src/core/communication/renderconfigeditor.cpp#L1412) が規則を持つ: `GraphRegistryError` なら `namedRefusal` で code・message(例外の文)・detail の欄(既にある欄は上書きしない。graph.* の code なら `document` も)をそのまま返し、それ以外は今までどおり `render_pipeline_preflight_failed` + `withPreflightRefusalFields`(`shader_input_mismatch` など)。apply / commit の経路(`completeCommitTransaction`)と `preflightGraphVariants` も同じ `namedRefusal` を使う。呼び手は 27 で、`materialize_render_feature` / `export_render_feature` / `import_render_feature` / `fork_render_pipeline`・fold の verify の回収・`decompose_material_pass`・`apply_graph_template`・planning key の編集・compute task の 3 つ・`edit_region_replacement`・`edit_preset_pass`・共通 enqueue を通る編集(`add_authored_pass` など)・`edit_render_targets`・`get_render_documents` / `restore_render_documents` / `reload_render_documents` などで、`GraphRegistryError` の応答が `render_pipeline_preflight_failed` から code + detail に変わった(実装者の報告 §19 の 4 の一覧)。
  - **Studio の hint は code と resource で**: Graph タブの「the graph pass that draws it runs after this pass」の句は、`code == "render_graph.input_not_produced@1"` で、detail の `resource` を公開中の FramePlan の graph の pass が書くときだけ足す([`graphcanvaswidget.cpp:8164`](../src/devstudio/view/graphcanvaswidget.cpp#L8164)、[`GraphCanvasModel::graphWritesResource`](../src/devstudio/model/graphcanvasmodel.cpp#L6094))。文の分割(`graphWritesResourceNamedIn(message)`)は消した。
  - **core に残る JSON の部分読み**(`passes` / `rendering_passes` を読む所。実装者の報告 §8 の表をコードで確かめた): `validateGpuDrawSourceBufferContracts`(buffer の宣言がまだ IR に無い。RC-3c2)、実行用 parser(RC-3c2)、graph transform / subgraph の registry(provider の ABI が JSON)、ImGui の pass の追加(改訂 0 の前の host の追加)、`renderer_config.cpp` の fallback の判定、editor の authoring。ほかに compiler が作者の root の `frame_node_order` の鍵を読む所(上の横断検査)と、`namespaceComputeTaskDefinitions` が composed config の `compute_tasks` を読む所が残る(どちらも RC-3c2 で registration を IR から作るときの対象)。preview の pass 名は IR から取る([`makePreviewGraphProgram`](../src/core/renderingpass/previewgraph.cpp#L127)。graph の node のうち `scope == graph` で compute でも anchor でもない物、宣言順)。
  - **消した物**: core の計画用 parse 一式(`parseFrameGraphDefinitionsFromConfigJson` / `parseFrameGraphDefinitionFromJson` / `parseRenderNodeFromJson` / `parseComputeNodeFromJson` と helper、`frameplanner.cpp` の約 1400 行)、`makeFrameGraphDefinition` 一式、`validateFramePlanPassInputsProduced`、`compileRenderingLogicalGraphs` 2 版、`detail::multiviewCapableFullscreenPasses` / `detail::requireRayQueryPlanningCapability` / `detail::insertSceneAccelerationStructureNodes` / `detail::applyFrameNodeOrder` / `detail::namespaceComputeTasks`、core の `shadowOptions` / `graphTileLocalAttachmentFormats`、`planFrameGraphWithNamedRefusals`、Studio の `graphWritesResourceNamedIn`、橋の test `rendergraphir_bridge_test.cpp`(直接の検査は新しい道だけの `test/rendergraphir_test.cpp` に移した)。`framePlanToJson` は core に残る。
  - **RC-3c2 への申し送り**(実装者の報告 §17): 物理計画の部品は `frameGraphDefinitionOf` と実行用の `RenderTargetDefinition` を読む(IR の images と実行用の宣言の二重を §2.4 で 1 つに)。`validateGpuDrawSourceBufferContracts` の JSON 読み(buffer の宣言を IR に)。実行用の compute task parser の表の適用と改名(`detail::namespaceComputeTaskDefinitions`)は IR と二重。compose の重複名の拒否(「Pass name collides ...」「Duplicate render target name」)は名前付きでない。graph ローカルの render target は計画用の読みだけ(RC-3c1a から変わらず)。
- RC-3a と RC-3b は独立(並行可)。RC-3c1 は両方の後、RC-3c2 は 3c1 の後。RC-3d・3e は 3c2 の後。
- PF-3 は RC-3c1 の後(`shape` と `uses[].footprint`)。PSN-2 以降は IR の `info` に足す。

## 6. 利用者に決めてもらうこと

- **D1**: 差し替え可能な compiler program と complete physical plan の install(本番で使う所なし、test だけ)を削るか。
  **決定(2026-10-24、利用者了承)**: 今回の RC-3 では complete physical plan の install の口を残す(壊さない)。NativeScope の ABI を設計する時に入口を IR の node(`native_scope`)に作り直し、その後でこの口を削る。この口は最適化のための物ではなく、今は NativeScope の宣言を運ぶ唯一の入口(node の最適化 = 自動・policy・provider はどれもこの口を通らない)。
  理由の記録(10-24 の推奨): compiler program の丸ごとの差し替えは削る(大きすぎる口。混在の穴も消える)。complete physical plan の install と NativeScope の仕組みは残す: v1 の方向(2026-10-08 の利用者との整理: DLSS などの外部 SDK、CGH の CUDA 相互運用、語彙に無い Vulkan 機能の試作)の受け皿で、今の NativeScope はこの口を通る時だけ動く。NativeScope の ABI を設計する時に、IR の node の 1 種類(`native_scope`)として入口を作り直す。
- **D2(決定 2026-10-24: 揃える)**: 拒否の文言を「名前付きコード + 対象名」に揃えて変えてよいか(test が文言を固定している所は直す)。
- **D3(決定 2026-10-24: 揃える)**: 物理計画の dump と Studio の format の綴りを作者の形(`R16G16B16A16_SFLOAT`)に揃えてよいか。

## 7. 統合側(Claude)が既に考えた懸念

1. XR の mirror sink は `PassDefinition` をコードで組んで `compileRenderingPassRuntime` に渡す(M2 §6)。RC-3c2 で「IR の node を作る typed な構築」として扱う(§2.2 で typed 構築は許す)。
2. compose は JSON の上で依存を読み続ける(正規形前提にはなる)。合成そのものを typed にするのは範囲外。
3. RC-3c1 / 3c2 は大きい。各段の中でも「新しい道を足す → 本番を切り替える → 古い道を消す」の順で commit を分ける。

## 8. レビューで「削る根拠が無い」とされた物

IR・FramePlan・論理 graph・物理計画の 4 段は、段の数だけを理由に削れない(codex 2 回目)。ただしそれぞれに独立した正本を持たせず、確定 IR の派生物として扱う。

## 9. レビューへの対応

### 1 回目(10 件)

| # | 指摘 | 対応 |
|---|---|---|
| 1 | format の bytes が vk の block size と食い違う | §4: 容量の見積もりと block size を別の性質に |
| 2 | 登録の metadata は variant 間で統合され、runtime にも変わる | §2.4 |
| 3 | material の状態は material が持つ | §2.5 |
| 4 | 呼び出し列は FramePlan の順と同じではない | §2.6 |
| 5 | 入口 1 回の正規化では崩れる | §2.1 |
| 6 | IR が利用の情報を落とすか二重に持つ、footprint の規則が足りない | §2.2 の `ResourceUse`(binding に descriptor の値)、§2.3 |
| 7 | provider の前後で再 parse が残る | §2.2、改訂ごとに 1 回で引き継ぐ |
| 8 | `fallback` と `LogicalPortRelation` は UI / ABI に出る | §3: 触らない |
| 9 | Studio で壊れた文書を直す経路が無い | §2.9 |
| 10 | 受け入れが目的未達でも通る | §5 |

### 2 回目(9 件)

| # | 指摘 | 対応 |
|---|---|---|
| 1 | RC-3b の対照が新しい契約で成立しない / RC-3c2 の一致は共通の誤りを見逃す | §5: 正規化ありは具体値、なしは名前付きの拒否。資源は期待値(和集合・最大・group の解除)と比べる |
| 2 | render strategy は feature の選択を変えるので compose の後に置けない | §2 の図・§2.8: strategy は compose の中に残す。「1 改訂 1 parse」は完成した graph の改訂に限る |
| 3 | 確定処理を中間の graph に掛けてしまう。JSON builder の独占は過剰 | §2.2: `parseRenderGraph`(JSON)と `finalizeRenderGraph`(typed、provider の後)を分ける。唯一にするのは JSON parser |
| 4 | object の正典が compose の書き換え処理を壊す | §2.1: 読みと書き換えを 1 組の helper に、受け入れで出力先を検査 |
| 5 | target ごとの統合では alias の孤立 member が残る | §2.4: group 単位で確定 |
| 6 | ResourceUse に descriptor の値(view、sampling など)が無い | §2.2: `binding` に付ける |
| 7 | footprint の規則が既存の契約を覆っていない、local read の条件が足りない | §2.3: 契約の区別・既定・拒否・候補と採用の分離 |
| 8 | compose で失敗する文書から復旧できない | §2.9: 原文・digest は前処理の成否と無関係。RC-3e の受け入れに compose の失敗 |
| 9 | 数値型に SNORM が無い、複合 format の扱いが無い | §4: 成分ごとの格納の型、shader が見る型は別の性質 |
