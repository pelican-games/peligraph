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
| 写しの enum(load / store 3 種、sampling 2 種、view layout 2 種、cull / depth compare 2 種) | 1 つに |
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
| RC-3a 語彙 | §4 の format の表、usage、load / store(既定と優先順)、`Extent2D`、`RenderResolutionDomain`、gpu draw の宣言、buffer / compute の大きさ(uint64)を project に。写しの enum を統合。core は写像 | 全 GPU golden・headless・Studio シナリオが緑(挙動不変)。dump の format の綴りの変更だけ fixture を更新 |
| RC-3b 正規化 | §2.1 を全ての入口に。内部 generator を正規形に。target 参照の読みと書き換えを 1 組の helper に | 短縮形を root・feature・provider 応答のそれぞれに置いた project を本番の loader で読み、**解決した writer → reader の対応・FramePlan の順・出力先(swapchain → display、HDR の中間資源、`$dst` の parameter 先)が期待値どおり**。対照: 正規化を外すと、同じ入力が所定の名前付きコードで断られる |
| RC-3c1 IR と計画 | `parseRenderGraph`(§2.2、§2.3)、provider の改訂ごとの IR の引き継ぎ、`finalizeRenderGraph`、FramePlan・論理 graph を IR から(project)。core の計画用 parse と部分読みを削る | 本番の registration を通して、`parseRenderGraph` の呼び出し回数 == 完成した graph の改訂の数(transform 1 個なら 2)。provider が足した node を `frame_node_order` が指せる。拒否は入力ごとにコードと対象名を検査。golden・headless 緑 |
| RC-3c2 資源と registration | §2.4、資源の検査の純ロジック化、registration を確定 IR + 資源の記述から(実行用 parse を削り、normalized JSON を保持しない)、§2.5 の material の検査 | XR の外部 depth を足した 2 variant と、片方の variant に無い alias member を持つ構成で、**検査と GPU 登録が消費した format・usage・extent・samples・layers・alias group が期待値(usage の和集合、layers の最大、group の解除)どおり**。対照: 統合の各規則を外すと、その具体的な値の欠落が検出される。shadow の light 数を変えた時の layers の検査が効く。ここで「1 改訂 1 parse」が完成 |
| RC-3d 順序 | §2.6。物理計画は FramePlan の順を入力に、scope ↔ node の対応を前計算、`frameNodeDependencies` を削る | 融合 scope + 2 view の構成で、実際の呼び出し列が A₀→B₀→A₁→B₁、scope を展開した node 順が FramePlan と一致。対照: 物理計画に別の順を渡すと静的検査が名前付きで断る |
| RC-3e Studio | §2.9 | 正常な編集が公開される / 不正な候補は名前付きで断られ公開中の generation が変わらない / **compose で失敗する文書(存在しない node を指す `snapshots[].after` など)と IR で失敗する文書の両方**で原文・digest が取れ、直して保存できる、を同じ test で |
| (後) | 変わっていない部分の再 compile を省く、窓の大きさの変更で全段に入らない | — |

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
