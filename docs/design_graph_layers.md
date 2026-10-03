# Graph の層 —— 畳んだ高機能 node と、上から下まで辿れる 1 枚の canvas

状態: v2(2026-09-27)。利用者の指摘から起こした。§1 は実物で確かめた事実、§3〜§4 は Studio の形、§5 は engine と詰める契約の案。v1 を engine session が実物(01b244cd)に当て、実装できない所 3 つと事実の誤りを指摘したので反映した(§10)。

## 0. 経緯と裁定

2026-09-27、利用者が Release 版で試して、次を指摘した。

- Frame Plan で足した pass は Graph に出ず、Open in Graph も出ない。「Graph と Frame Plan は展開の上下関係のはずで、ここまで行き来が不自由な印象は無かった」
- 「`deferred_geometry` はかなり抽象化された node のはず。中を見ればループゾーンを含む node に見えるべきで、それは単純な論理 node ではなく最適化済みの論理 node で、実体の pass はさらに下の層で展開される」
- 「上の層から下の層まで一貫して辿れるべきで、最終的には Vulkan など hard 依存 API の切り替え層まで見せられるとよい」
- 「逆に、論理 node から組んだ非効率な node を複数選び、下の層の展開結果を改造する形で最適化したら、上の論理層では `deferred` と同じ折り畳める高機能 node として見えるべき」

| 日付 | 裁定(利用者) |
|---|---|
| 09-27 | 上の 4 点は元の設計思想であり、今の Studio はそこからずれている。戻す |
| 09-27 | **全領域(render / pose / value / audio / ui)を 1 枚の canvas に出さない。領域の filter が前提** |
| 09-27 | 体現できるか・使いやすくできるかを先に考える。答えは §8 |

これは新しい思想ではない。次の既存の裁定を Studio に通すだけである。

- [pelican_vision.md](pelican_vision.md) 原則 1(一つのグラフモデル。編集の正本は文書、GUI は view。同じ接続に二つの正本を残さない)、原則 3(標準機能は最適化した一体版と編集できる分解版の両方を持ち、同じ設定を表す)
- [v1_development_plan.md](v1_development_plan.md) の「一つのグラフモデル」の行:「意味の共通化であり、一枚のキャンバスではない。Studio は view に分け、同じ正本を見る。Frame Plan から元の feature・node・コードへ戻れるようにする(生成結果を第二の正本にしない)」
- [design_graph_composition.md](design_graph_composition.md) の「実行領域への変換」:「標準サブグラフは境界契約を保つ最適化実装へ置換できる。通常の折り畳み表示と『展開して編集可能にする操作』は別。表示を開閉しただけで最適化を解除しない」
- [design_graph_loops.md](design_graph_loops.md) §4.3:一体版の material pass の意味は、else 1 分岐の draw_loop を `render.material_pass` が受けたものと**同じ**

## 1. 今の作り(2026-09-27、develop `a0f8a670`。実物で確認)

**文書は 1 つ、書き方は 2 つ。** rendering config の root は `pelican.graph` だけ(v0_freeze §1.6)。その中に、

- `nodes` / `definitions` / `outputs`:共通グラフの node。Graph タブが描く
- `rendering` 節:pass の列、target、feature の instance、preset の参照。Frame Plan・Features・Pass Editor が編集する

**Studio の入口は 4 つに分かれている。**

| 入口 | 見せるもの | 編集するもの |
|---|---|---|
| Graph タブ | root の `nodes`(ループはゾーン)。definition には潜れる | `nodes` / `definitions` |
| Frame Plan | engine が組み立てた frame plan(展開結果)。タブ: Logical graph / Passes / Resources / Physical plan / Decisions / Backends / Barriers / Materials / Raw JSON | `rendering` 節の pass(Edit pass / Add pass / Add material pass / Decompose into loop)と render target |
| Features | feature の一覧と parameter | `rendering.features[]`(Replace with loop version はここ) |
| Inspector の Attached graphs | object に装着された `graph.instance` | component の parameter |

**出自は途中まで通っている。** frame plan の node には `source`、`provider_feature` / `provider_ref`、`loop_origin {loop, element, node}`、`authored_name` が付く([出す所](../src/core/renderingpass/frameplanner.cpp#L1905))。
- `source` は `project` / `feature:<name>` / `engine` の 3 つだけ。`graph_node` は compute task の authoring の `origin` にしかない。preset が `engine://` のとき、graph の node やループの回が作った pass も `engine` になり、preset 自身の pass と区別できない
- engine の中には `GraphNodeOrigin {instance_path, definition, node, loop, loop_branch}`([型](../src/project/graphcomposition.hpp#L38))があるが、pass の provenance(`RenderPassProvenance`)まで通っていない
- 物理 plan の側は、scope の nodes、attachment の node、lowering の sources、subgraph_replacements、barriers が既に node 名を持つ。無いのは resource・alias・decision・backend から node への対応と、diagnostics / `deactivated_graph_nodes` を `get_frame_plan` に出すこと
- Studio の Frame Plan のタブは、この node 名を上の層の node へ結び付けていない

**一体版と分解版はつながっていない。** engine 同梱の雛形(`engine://graphs/*.json`)は `decomposes: {feature, …}` で「どの feature の分解版か」を言うが、使い道は `apply_graph_template`(feature を外して雛形を写す一方通行)だけ。「畳んだまま feature を node として見せ、開くと分解版が読める」段階は無い。material pass の Decompose into loop も同じく一方通行。**畳み直す道は無い。**

**等価性の検査は既にある。** 一体版と分解版の一致(queue の byte・記録された draw の列・golden の画素)は design_graph_loops §4.3 / §5.3 が課し、test で見ている。ただし「この project のこの definition が、この実装と一致している」という**印**は文書にも RPC にも無い。

**境界契約を保つ置換の仕組みはある。** RPE11b(WP201)の tagged region / subgraph replacement:元の config を typed logical graph に compile して境界の contract を作り、provider の結果を splice し、resource 集合・型・materialization・boundary port・fingerprint が一致したときだけ target lowering に進む。provider / source / replacement の provenance は `CompiledLogicalRenderGraph` と `VulkanTargetPlan` に残る([選択の型](../src/project/logicalrendergraph.hpp#L229))。Studio の入口は、RPC `edit_region_replacement` と Features の **Regions** ボタン(fork 済み preset の region の provider 選択)だけで、graph の node と結び付いていない。

**下の層を書く口は graph 全体の単位。** feature の instance に書けるのは `ref` / `parameters` / `pass_replacements` だけ(format・usage・alias・pass の種類・backend は書けない)。下の層の口はどれも graph 全体の単位で、展開後の名前で書く。
- `target_planning.graphs.<g>.{nodes: {serial, isolate}, resources: {no_alias}}`
- `vulkan_physical_fragments`(format・表現・scope・alias・load/store、fingerprint 付き)
- `vulkan_plan_pins`(graph 全体の backend candidate を 1 つ固定)
- node 単位の raster / compute / RT の選択を書く口は無い(`selected_implementation` は pass の種類から自動)。raster と RT は別の feature を選ぶ形(`rt_shadow_mask*`)

**開いた形は 1 つの definition ではない。** `graph.loop` は root にしか置けず(`graph.loop_nested@1`)、definition の中に instance も置けない(`graph.nested_instance@1`)。bloom・cube・cascade・draw_loop の開いた形は「root の断片(nodes + 複数の body definition)」で、GT-1 の雛形もこの形である。feature の instance は `rendering.features[]` にあり `graph.instance` ではない。`deferred_geometry` は feature ではなく preset `hybrid_v1` の material pass(分解は `decompose_material_pass`)。

**下の層の分け方は設計にある。** [design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md) の LogicalCompiler → TargetExecutionCompiler → BackendLowerer → ExecutionLinker。Frame Plan のタブは、この段の出力を表で見せている。

**共通グラフは描画だけではない。** catalog の op は `render.*` / `draw.*` / `pose.*` / `value.*` / `audio.*` / `ui.*` / `event.*`。`playground_pose` の graph 文書には `pose.clip_sample` と `value.input` で walk と run を blend する definition があり、object の component から `graph.instance` として装着されている(root の `nodes` は空)。

## 2. ずれの本体

engine の設計は「同じ設定を、畳んだ形と開いた形の両方で持ち、下の層へ出自を通す」。Studio はそれを、**`rendering` 節を編集する画面群**と**`nodes` を編集する Graph**に分けて作った。F 系(Frame Plan の編集)が先にあり、ループ(LP-4)で Graph を `nodes` だけの canvas として後から足し、二つを 1 つの view にする WP を立てなかった。結果、

1. feature の pass は Graph に無く、Graph で足した pass は Frame Plan からしか展開結果を見られない。同じ fullscreen pass でも、どちらで足したかで別の文書の場所に書かれ、見た目で区別できない
2. `deferred_geometry` は Graph に出ないので、「中に draw_loop を持つ最適化済みの node」という設計上の意味が画面に無い。draw_loop 段のループは Frame Plan では material pass の 1 行なので、Graph のゾーンと同じものだと読み取れない
3. 下の層(物理 plan・barrier・backend)は表で見えるが、上の node の identity で引けない
4. 利用者が組んだ definition を、engine 同梱の feature と同じ「畳んだ実装を持つ node」にする道が無い(非対称)

## 3. 形

### 3.1 畳んだ node = 断片 + 畳んだ形

畳む単位は **root の断片**(root の node の集まりと、それが使う body の definition)である。1 つの definition ではない(§1 の「開いた形は 1 つの definition ではない」)。GT-1 の雛形と同じ形で、engine の既存の規則にぶつからない。

断片には 2 つの向きがある。

| 向き | 文書にあるもの | もう一方 | 例 |
|---|---|---|---|
| **畳んで書いてある** | 一体版(preset の pass、`rendering` 節の pass、`rendering.features[]` の instance) | 開いた形 = 雛形(`engine://graphs/*.json`)か、engine が意味を定めた形(material pass の draw_loop。loops §4.3) | `deferred_geometry`(preset の material pass)、bloom(feature) |
| **開いて書いてある** | root の node 群(ループ・instance を含む) | 畳んだ形 = `replacement`(RPE11b の provider)か `decisions`(§3.6) | 利用者が組んで最適化したもの |

- 「畳んで書いてある」側は、文書を変えずに Graph に畳んだ node として出し、潜ると開いた形を**読める**(GL-1)。開いて編集するのは今の `decompose_material_pass` / `apply_graph_template`(一方通行。戻すのは RD-1 の undo)
- 「開いて書いてある」側は、root に**断片の宣言**を足して畳む(§5.2)。境界(断片の外から読む・外へ書く資源と port)は、畳んだ形でも同じでなければならない
- engine 同梱の一体版と雛形の対応は、今の雛形の `decomposes` と material pass の意味(loops §4.3)をそのまま使う。「engine の合成は `decomposes` を読まない」の契約(v0_freeze)は変えない(読むのは Studio と RPC)

### 3.2 層と identity

| 層 | 実体 | 見る所 | 編集 |
|---|---|---|---|
| L0 authoring | `pelican.graph`(node / definition / instance / loop / `rendering` 節) | Graph の canvas | する(正本) |
| L1 展開 | frame plan の node(instance を展開、ループを回に、feature を pass に) | canvas の node の帯、Frame Plan の Passes / Logical graph / Materials | しない。決定(§3.6)は L0 に書く |
| L2 物理 | target planning(scope、資源の寿命、aliasing) | node の詳細欄、Frame Plan の Physical plan / Resources / Barriers | しない |
| L3 backend | BackendLowerer / ExecutionLinker の結果(Vulkan の command の形、backend の選択) | node の詳細欄、Frame Plan の Backends / Decisions | しない。backend の固定は決定として L0 に書く |

**identity の鎖**:L1 の node は L0 の出自 `origin`(`GraphNodeOrigin` か、feature / preset / `rendering` 節の pass)を持ち、L2 の scope と資源、L3 の項目は、それを作った L1 の node の名前を持つ。engine が返す JSON のどの層の要素からも、node 名 → `origin` で L0 に戻れることを engine の契約にする(§5.1)。

**編集できるのは L0 だけ**。「下の層を改造して最適化する」は、L2 / L3 で見えた事実をもとに、L0 に決定(§3.6)を書く操作である。Vulkan の command 列を直接書き換える道は作らない(第二の正本になる。原則 1)。

### 3.3 領域の filter

canvas は 1 枚だが、**見せる領域を選ぶ**。全領域を一度に描かない(裁定)。

- 領域は op の接頭辞: `render`(`draw` を含む)/ `pose` / `value` / `audio` / `ui`(`event` は属する領域の中で見せる)
- 既定は 1 領域。描画から開けば `render` だけ。複数を同時に選べる
- 領域をまたぐ線は端で切る。`render` の filter で `render.scalar` が `value` の node から値を受けていれば、その入力に `← value (3 nodes)` の口だけを出す。押すとその領域が filter に加わる
- object に装着された instance(`pose` の大半)は、既定では出さない。filter に `attached to <object>` を足したときだけ出す。Inspector の Attached graphs から入る道は残す
- 下の層の帯と詳細(§3.2)は領域ごとに違うので、**filter が 1 領域のときだけ出す**。混ぜたときは畳んだ node の形だけ

### 3.4 潜る・展開して編集・畳み直す

操作は 3 つだけ。どれも今ある動作の延長で、新しい概念を足さない。

| 操作 | 動作 | 文書 |
|---|---|---|
| **潜る**(ダブルクリック / Open) | 畳んだ node の中(開いた形)を**読む**。material pass なら `render.material_pass` と draw_loop のゾーン、雛形のある feature なら雛形、開いて書いた断片なら中の node を、同じ canvas に描く。パンくずで戻る | 変えない |
| **展開して編集** | 畳んで書いてあるものを開いた形にする(今の Decompose into loop / Replace with loop version はこれの 2 例) | 今の RPC のまま。RD-1 の undo で戻る |
| **畳み直す** | 選んだ root の node 群を断片として宣言し、畳んだ形(`replacement` か `decisions`)を付ける。付けた直後に等価性の検査(§3.5)が走り、通れば印が付く | 断片の宣言を書く |

- 「表示を開閉しただけで最適化を解除しない」(design_graph_composition)は、潜るが読むだけであることで守られる
- **畳み直しと物理の plan。** 断片の宣言は instance を作らず、root の node をそのまま残して印を付けるだけなので、`decisions` の畳み直しでは `isolate` は増えない。一方、node を `graph.instance` にまとめると、instance の中の render node と下流に `isolate` が付き、scope の融合や tile-local が失われる(LP-1c の一致の test も isolate の差を除いている)。instance にまとめる操作は利用者が明示したときだけ行い、Studio は「物理の plan が変わりうる」を確認の文で言う。`replacement` は RPE11b の境界の検査に従う

### 3.5 等価性の印と失効

- 印は `verified: {digest, at}`。`digest` は開いた形(断片の node / 線 / parameter と body の definition。layout は含めない)の digest、`at` は検査した日時
- 検査の中身は design_graph_loops §4.3 / §5.3 と同じ:frame plan(名前の対応表を通す)、queue の byte、記録された draw の列、指定した golden の画素。**今は材料が test にしかない**(名前の対応表は手書き、描き比べの probe も test 専用)。対応表をデータにすることと、実行中の engine で 2 つの形を比べる道を作るのが engine の仕事(§5.3)
- **失効**:開いた形の digest が `verified.digest` と違えば失効。畳んだ形の側(provider の fingerprint、決定の fingerprint)が変わっても失効
- **失効した断片の実行**:engine は**開いた形で走らせ**、`graph.fold_stale@1 {group, verified_digest, current_digest}` を名前付きで返す(黙って畳んだ形を使わない。原則 7)
- engine 同梱の一体版と雛形は engine の test が一致を見ているので、印を文書に持たない(常に検査済みとして扱う)

### 3.6 宣言できる決定

「下の層の展開結果を改造する」を L0 に書く形。**新しい下の層の口は作らず、今ある graph 全体の口(§1)を断片から書けるようにする。**

| 決定 | 今ある口 | 足りないもの |
|---|---|---|
| node の直列化・分離 | `target_planning.graphs.<g>.nodes.{serial, isolate}` | 断片のローカル名(root の node id、ループの回)から展開名へ写す段(前例: isolate の派生) |
| 資源の aliasing の禁止 | `target_planning.graphs.<g>.resources.no_alias` | 同上 |
| format・表現・scope・alias・load/store | `vulkan_physical_fragments`(fingerprint 付き) | 同上と、fingerprint が切れたときの名前付きの拒否 |
| backend の候補の固定 | `vulkan_plan_pins`(graph 全体) | node 単位の backend の語彙(HEG3 以降。v0 では graph 全体のまま) |
| 連続する fullscreen の置換 | RPE11b の region と provider(`edit_region_replacement`) | 断片と region の対応 |

決定は**要求**であって結果ではない。engine が採れなければ名前付きで断り、採った結果は L1〜L3 から辿れる(「この barrier はこの決定のせい」)。raster と RT の切り替えは、今は別の feature を選ぶ形なので、決定ではなく「展開して編集」で別の雛形に替える操作になる。

## 4. Studio の見え方(使いやすさの規則)

1. **canvas は 1 枚、見える段は 1 つ。** 下の層は別の canvas ではなく、同じ node の帯(`×3 copies · 2 barriers · vulkan`)と、選んだときの右の詳細欄。Frame Plan は「L1 を時間順に見る mode」として残し、独立の編集入口ではなくなる。Frame Plan の編集ボタン(Edit pass / Add pass / …)は Graph の同じ操作を呼ぶ
2. **黒箱は黒箱のまま使える。** 初めて触る人は `deferred_geometry` を中を見ずに使える。最初の画面で中身を展開して見せない
3. **開けるが直せない状態を作らない。** 潜れば読める。直したければ「展開して編集」。畳んだ node の中で線や値を触ろうとしたら、その場で notice に 1 行と **Expand to edit…** を出す(押すと確認のダイアログ。Cancel が既定)
4. **feature の線は派生の破線。** `rendering` 節の pass と feature は target の名前でつながり edge を持たないので、frame plan の読み書きから線を派生させて破線で描く(body の出力の破線と同じ流儀)。触ろうとしたら 3 と同じ案内。根治は保存形の一本化(§6)
5. **印は既定と違うときだけ。** 失効は橙の印 1 つ、開いた形で走っている間は帯に `running open form`。全 node に常時 8 個の印が並ぶ形にしない
6. **潜る操作は 1 種類。** definition の body・畳んだ node の中・ループのゾーンの中、すべてダブルクリックと Open。パンくずで戻る
7. **領域は filter。** 既定 1 領域。またぐ線は口だけ(§3.3)
8. **打つものを増やさない。** 断片の名前・region の名前は Studio が提案する(`suggestDefinitionName` の流儀)

## 5. engine の契約(案。engine session と詰める)

順番は engine session の提案に従う:まず `origin`(§5.1)。挙動を変え、単独で Frame Plan → Graph の行き来に効く。断片・決定・検査(§5.2〜§5.4)は、その後で設計を詰める。

### 5.1 pass の `origin`(GL-2a。engine の最初の WP)

- `GraphNodeOrigin {instance_path, definition, node, loop, loop_branch}` を pass と compute task の provenance(`RenderPassProvenance`)まで通し、`get_frame_plan` の node に `origin` として出す。graph の node が作っていない pass には出さない
- これで preset が `engine://` でも、graph の pass と preset の pass を区別できる(今は両方 `source: "engine"`)
- 物理 plan の側は既に node 名を持つ要素が多い。足りない resource・alias・decision・backend → node の対応を `from_nodes[]` で足し、diagnostics と `deactivated_graph_nodes` を `get_frame_plan` に出す
- 受け入れ: どの層のどの要素からも node 名 → `origin` で L0 に戻れる

**着地(GL-2a、`d3a34869`、2026-09-28)。** 上の 4 行は着地前の案として残す。実物は次のとおり(コードで確認):

- 通り道: 展開の [`GraphNodeOrigin`](../src/project/graphcomposition.hpp#L38) を、lowering が node の作った pass・task の名前ごとに `LoweredRenderGraphDocument::origins` に残し([残す所](../src/project/rendergraphcomposition.cpp#L760))、合成が provenance を同期した直後に [`RenderPassProvenance::graph_origin`](../src/project/renderpipeline.hpp#L267) と `replaces` に載せる([載せる所](../src/project/featurecompose.cpp#L3666))。付くのは root の node・instance の中の node・ループの回(`loop_origin` の要素が選んだ分岐から body の node を引く)が作った pass と compute task だけ。preset・feature・rendering 節の pass には付かない(feature の pass は、同じ名前の node が非活性でも付かない)
- `get_frame_plan` の node の形は `origin: {graph, node, definition?, instance_path?, loop?, loop_branch?, element?}`([組む所](../src/core/renderingpass/frameplanner.cpp#L1639))。案に無かった `graph` と `element` を足した。`graph` は rendering root 文書の参照(`ProjectBasicConfig::renderingConfigReference()`。`get_compute_task_authoring` が node の task に返す `source_reference` と同じ値)で、renderer が `framePlanToJson()` に渡す([渡す所](../src/core/vkcore/renderer.cpp#L4012))。渡さない経路(CPU の test など)では空文字。`definition` は root の node では出ない。`instance_path` は末尾の `/` を除き、root とループの body では出ない(body は instance ではない)。`element` は `loop_origin.element` と同じ
- `replaces: [{feature, pass}]` は配列(案には無かった)。1 つの node が複数の feature pass を置き換えうるため(planar の mip chain の雛形は `planar_reflection_filter_mip_1`〜`_6` を 1 つのループで置き換える)。ループでは、置き換えた pass と同じ pass family に移った回にだけ付く([対応を取る所](../src/project/rendergraphcomposition.cpp#L2339))
- `from_nodes[]`(frame plan の node 名、plan の node の順)は `physical_target_plan` の resources[](reads / writes / reads_history と attachment の node から)・alias_groups[](メンバーの和)・4 か所の decisions[](最上位・`lowering_graph`・`planning_opportunities`・`backend_selection`。`subject` を node → resource → alias group → scope の順で引き、lowering の node は `sources` へ開く。graph 全体の決定は `[]`)・`backend_selection.candidates[]` に付く([`appendFramePlanPhysicalNodeReferences()`](../src/core/renderingpass/frameplanner.cpp#L2183))。backend の候補は failures / diagnostics の `subject` から引くが、`probeVulkanBackend()` の拒否は `subject` に候補自身の名前を入れるので([拒否を積む所](../src/project/targetplanning.cpp#L488))node に当たらない。実装者の報告では GL-2a の test の plan で候補は全部 `[]` だった(どの構成でも必ず `[]` かは未確認)
- `diagnostics: [{kind, subject, detail}]`(kind は `graph_variant_selected` / `pipeline_preset_resolved` / `feature_excluded` / `graph_node_deactivated`)と `deactivated_graph_nodes: [{node, feature, reason}]` は、compiled pipeline のある plan では空でも最上位に出る
- 既存の `source` / `loop_origin` / `provider_*` / `authored_name` は変わらない。`get_compute_task_authoring` は root を展開し直すのをやめ、provenance の `graph_origin` を読む(出力は同じ。ループの回の task は今までどおり `graph_node` にならない)
- 案の受け入れとの差: node を指す要素からは node 名 → `origin` で戻れる。graph 全体の決定と backend の候補は `from_nodes: []` で、戻る先を持たない
- test: [`GL-2a frame plan nodes carry the graph node that made them`](../test/rendercompilerprogram_test.cpp#L1717)(compiler_only の本番の compiler。`engine://` preset・instance・ループ・`pass_replacements` を持つ graph で、root / instance / ループの回 / 置き換え / preset の pass に `origin` が無いこと / `from_nodes` の名前が plan に在ること / diagnostics と `deactivated_graph_nodes`)。LP-3b の provenance の一致の check は「`graph_origin` だけが違う」に直した([直した所](../test/featurecompose_test.cpp#L7872))

### 5.2 断片の宣言

```json
"fold_groups": {
  "my_post": {
    "nodes": ["blur_loop", "blur_mix"],
    "folded": {
      "kind": "decisions",
      "decisions": { "serial": [], "no_alias": [], "physical": [] },
      "verified": { "digest": "<開いた形の digest>", "at": "2026-09-27T12:00:00Z" }
    }
  }
}
```

- root の key `fold_groups`(名前は仮。`implementation` は pass・raster・shader で使われているので避ける)。`nodes` は root の node id。ループと instance はその body の definition ごと含む
- `kind`: `replacement`(RPE11b の provider)/ `decisions`(§3.6)
- 境界(断片の外との資源と port)は組み直しで検査する。`fold_groups` は field の追加なので版は上げない

**合意した形(2026-10-02、GUI session と engine session)。** 上の JSON と 3 行は 2026-09-27 の案として残す。§5.2〜§5.4 の合意は次のとおりで、着地しているのは EL-2 だけ(FG-1 / FG-2 は未着地)。

層の分け方(WP の順):

| WP | 中身 | 状態 |
|---|---|---|
| EL-2 | graph 全体の口(`target_planning` / `vulkan_plan_pins` / `vulkan_physical_fragments`)を**展開名で**書く ticket 3 本(`edit_target_planning` / `edit_vulkan_plan_pins` / `put_vulkan_physical_fragment`) | 着地(`b5a58b11`)。形は manual 13 章の「graph 全体の計画の決定を書く(✅EL-2)」、入口は [`editTargetPlanning()`](../src/core/communication/renderconfigeditor.cpp#L3527) |
| FG-1 | `fold_groups` の宣言と、ローカル名で書いた決定を組み立てのたびに展開名へ写す段(この節) | 未着地 |
| FG-2 | `verify_fold`(等価性の検査と印。§5.3) | 未着地 |
| FG-3(後回し) | physical fragment のローカル名化、`kind: "replacement"`、feature と分解版の名前の対応表のデータ化 | 後回し |

FG-1 の宣言(root の `fold_groups`):

```json
"fold_groups": {
  "my_post": {
    "nodes": ["blur_loop", "blur_mix", "post_color"],
    "folded": {
      "kind": "decisions",
      "decisions": {
        "serial":   [{ "node": "blur_loop", "element": 2 }],
        "isolate":  [{ "node": "blur_mix" }],
        "no_alias": [{ "node": "post_color" }]
      },
      "verified": { "digest": "<開いた形の digest>", "at": "2026-10-02T12:00:00Z", "frames": 3 }
    }
  }
}
```

- `fold_groups.<group> = {nodes: [root の node id], folded: {kind: "decisions", decisions: {serial: [...], isolate: [...], no_alias: [...]}, verified?: {digest, at, frames?}}}`。案の `physical` は無い(physical fragment のローカル名化は FG-3)
- **主語**は `{node: <root の node id>, element?: N}`。loop / instance の node ならその展開の全部(ループの全部の回、instance の中の全部の node)、`element` を付けるとループのその回だけ
- `no_alias` の主語は `{node: <render.image の node id>}` で、画像の node 自体を指せる。port を指す `{node, port}` は補助
- 主語は group の `nodes`(とその body)の中に限る。外を指すと `graph.fold_subject_outside@1`
- `kind` は今回 `decisions` だけ。`replacement` は `graph.fold_kind_unsupported@1`(FG-3 まで)
- **写す段**: 組み立てのたびに、GL-2a の `graph_origin`(§5.1。frame plan の node の `origin`)を逆に引いてローカル名 → 展開名へ写し、`target_planning` の `serial` / `isolate` / `no_alias` に足す。文書には展開名を書かないので、ループの count を変えても宣言は古くならない
  - 今の engine の事実(コードで確認): LP-1a は、書いた文書が `target_planning` の node に `<loop>/…` の展開名を書くことを `graph.loop_expansion_referenced@1` で断る([`requireLoopExpansionUnreferenced()`](../src/project/graphcomposition.cpp#L989))。したがって EL-2 の `edit_target_planning` ではループの回の node を名指せない(コードの読み。test は無い)。回ごとの決定を書く道は FG-1 の写しになる
- EL-2 の明示(root の `target_planning` に書いた値)と逆の指定は `graph.fold_decision_conflict@1`
- frame plan の `decisions` の要素に `from_fold: <group>` を足す。今の要素は `{id, subject, selected, detail}` と GL-2a の `from_nodes` で、決定の出所の印は無い
- **digest**: 開いた形(`nodes` に挙げた root の node、それらの間の線と `parameters`、使う body の definition。layout は除く)の canonical な digest を engine が計算する
- **失効**: `verified` が無い、または digest が違うときは、決定を捨てて**開いた形で走らせ**、`diagnostics` に `fold_unverified` / `fold_stale {verified_digest, current_digest}` を出す。断らない、黙らない(§3.5 の案の `graph.fold_stale@1` を名前付きで返す形は、組み直しを断らない diagnostics になった)
- **RPC**:
  - `get_fold_groups {}` → `[{group, nodes, kind, digest, verified?, state: "verified" | "stale" | "unverified"}]`
  - `fold_nodes {base_source_digest, group, nodes, folded}`: 1 ticket、RD-1 の対象(`restore_render_documents` で戻せる)。`folded.verified` は書かせない(書くのは `verify_fold` だけ)。`folded: null` で group を消す

**着地(FG-1、`61a88f9f`、2026-10-02)。** 上の合意した形は着地前の形として残す。実物は次のとおり(コードで確認):

- 宣言の形は合意どおり(`fold_groups.<group> = {nodes, folded: {kind: "decisions", decisions: {serial, isolate, no_alias}, verified?: {digest, at, frames?}}}`。[`parseGraphFoldGroups()`](../src/project/graphfold.cpp#L128))。決定の `physical`(2026-09-27 の案の例にはあった)は合意どおり無く、書くと形の誤り(`fold_nodes` では invalid params)
- 主語は `{node, element?, port?}`。`port` は `no_alias` だけで、`{node: <書き手>, port}` はその node の入力 `port` につながった `render.image` / `render.buffer`(`render.target_ref` ならその target)を指す。`no_alias` は `render.buffer` の node 自体と、ループ / instance の node(展開した target と buffer の全部、`element` でその回)も指せる。`render.image` / `render.buffer` の node を `serial` / `isolate` に書いても何も足さない
- 写す段: [`applyGraphFoldGroups()`](../src/project/graphfold.cpp#L269) を、合成が `graph_origin` を provenance に載せた後・`target_planning` の compile の前に呼ぶ([呼ぶ所](../src/project/featurecompose.cpp#L3904))。pass はその family の graph、compute task と資源は全 family の graph の `target_planning` に `true` で足す。当たらない主語は何も足さない
- **失効は diagnostics**: 合意どおり断らず、frame plan の `diagnostics` に `{kind: "fold_stale" | "fold_unverified", subject: <group>, detail: {verified_digest?, current_digest}}` を出す([出す所](../src/project/renderpipeline.cpp#L1400))
- **`fold_decisions` と `fold_decision_applied@1`**: 合意の「`decisions` の要素に `from_fold` を足す」は、既存の要素に印を付ける形ではなくなった。fold group が足した決定を frame plan の最上位の `fold_decisions [{group, kind: "node" | "resource", name, field}]` に並べ、1 件ごとに `physical_target_plan.decisions[]` へ新しい要素 `{id: "pelican.plan.fold_decision_applied@1", subject, selected, detail, from_nodes, from_fold}` を足す([足す所](../src/core/renderingpass/frameplanner.cpp#L2413))
- **digest は node 間の線だけ**: group の node は、入力のうち group の他の node から来る線だけを残して入れる(外から来る線は入らない)。`parameters` は入るので、ループの count を変えると印が古くなる(宣言は古くならない)。definition は推移的に辿り、`layout` を除く。鍵を整列した JSON の sha256 hex([`graphFoldGroupDigest()`](../src/project/graphfold.cpp#L185))
- 拒否: `graph.fold_kind_unsupported@1 {group, kind}`、`graph.fold_subject_outside@1 {group, subject}`、`graph.fold_decision_conflict@1 {group, name, field}`(組み立てと、EL-2 の `edit_target_planning` の両方。[EL-2 から断る所](../src/core/communication/renderconfigeditor.cpp#L3852))
- RPC: `get_fold_groups {}` は合意の配列ではなく `{source_digest, fold_groups: [...]}` を返す。`fold_nodes` は合意どおり。既存の group は丸ごと置き換わるので、書き直すと `verified` も消える
- 形と test は manual 13 章の「断片の決定を宣言する(✅FG-1)」。着地の W1 で 106 件が緑(統合側の報告)

### 5.3 検査と印

- `verify_fold {group}`:開いた形と畳んだ形で §3.5 の一致を取り、通れば `verified` を書く(ticket。RD-1 の undo の対象)。通らなければ `graph.fold_mismatch@1 {group, what: "frame_plan" | "queue" | "draws" | "pixels", detail}`
- 前提として、一体版と分解版の名前の対応表をデータにし、実行中の engine で 2 つの形を描き比べる道を作る(今は test 専用)

**合意した形(2026-10-02)。** 上の 2 行は案として残す。FG-2 として次の形にする(未着地)。

- `verify_fold {group}` は非同期の ticket(`accepted` → `running` → `committed` / `rejected`)
- **「同じ」の定義**(`kind: "decisions"`): 絵が同じで、validation が出ないこと。frame plan と queue の byte は一致しなくてよい(決定は物理の plan を変えるためのもの)。案の `what: "frame_plan" | "queue" | "draws"` は使わない
- **走らせ方**: engine が自分の exe を headless の子 process で 3 回起こす —— 開いた形 ×2(対照)と決定ありの形 ×1。同じ文書を一時 dir に写して走らせる
- 各回、warm-up(既定 30 s 相当)を描いて捨て、その後の N frame(既定 3)を byte で比べる。warm-up は NVIDIA の二段の shader compile の対策(起動直後の code が数秒後に最適化した code に差し替わり、値が変わりうる)
- **結果**:
  - 対照どうしが一致しない: `graph.fold_verify_unstable@1 {group, detail}`(決定のせいではない。比べられない)
  - 対照が一致し、決定ありだけ違う: `graph.fold_mismatch@1 {group, what: "pixels" | "validation" | "compile", detail}`(`detail` は最初に違った frame と差の数)
  - 通れば `verified {digest, at, frames}` を書く(RD-1 の undo の対象)
- Studio は既定 3 frame・byte 一致で、詳細の折り畳みに frame 数と許容幅を出す
- 失効しても自動では走らせ直さない(Studio の Verify ボタンで走らせる)
- engine 同梱の一体版と雛形は今どおり engine の test が一致を見る(印を持たない。§3.5)
- 案の「名前の対応表をデータにする」は FG-3 に回した。`kind: "decisions"` の検査は同じ文書の開いた形どうし(決定の有無)を比べるので、一体版と分解版の名前の対応表を使わない

**着地(FG-2、`9c71f97b`、2026-10-02)。** 上の合意した形は着地前の形として残す。実物は次のとおり(コードで確認):

- 要求は `verify_fold {base_source_digest, group, frames?, warmup_seconds?, tolerance?}`(合意の `{group}` に CAS と 3 つの数を足した。`frames` は 1〜600 で既定 3、`warmup_seconds` は 0〜3600 で既定 30、`tolerance` は 1 frame で違ってよい画素の数で既定 0。[`verifyFold()`](../src/core/communication/renderconfigeditor.cpp#L2613))
- **running の形**: 受けた時は `accepted`。描いている間の `get_edit_result` は `{ticket, status: "running", operation: "verify_fold", group}`。通った後は印を書く root 編集の ticket を同じ番号で並べるので、`pending` を経て `committed`(結果に `group`・`verified`)か `rejected` になる。印の ticket は既に並んでいる ticket より先に commit する([`harvestFoldVerifies()`](../src/core/communication/renderconfigeditor.cpp#L2702))
- **子の起こし方**: project を丸ごと(最上位の `.git` を除く)system の temp に写し、同じ exe を `--headless --project <写し> --frames N --fps <親> --size <project の窓の大きさ> --start-time <warmup_seconds> --render-out … --vulkan-validation on --log-file …` で 3 回、順に起こす([組み立て](../src/core/communication/graphfoldverify.cpp#L291))。合意に無かった **`--size`** は、子が親の project の窓と同じ大きさで描くため([親から読む所](../src/core/communication/editorruntimefactory.cpp#L1967))。warm-up は `--start-time` の fixed-step の frame の数で数える
- **validation の判定**: 子の stdout / stderr の `Validation Error` の数を数え、決定ありの形の数が対照の多い方を超えたときだけ `what: "validation"`(合意の「validation が出ないこと」は、対照にも出る error を許す形になった)
- **compile**: 決定ありの子が 0 以外で終わる(か frame が出ない・時間切れ)と `what: "compile"`
- **`fold_verify_failed` の新設**: 対照(開いた形)が描けないとき、project の写しが作れないときは、決定のせいとも不安定とも言えないので `fold_verify_failed {group, run: "open" | "setup", detail}` にした
- ほかに合意に無かった拒否: `fold_verify_busy {group, ticket}`、`fold_verify_unavailable {reason: headless | golden | replay | executable_unknown | no_runner | shutdown}`、走っている間に root が変われば印の ticket が `external_modification`
- 子は kill-on-close の job object に入れ、時間切れ 900 s と engine の終了時は job の handle を閉じて終わらせる([`runChild()`](../src/core/communication/graphfoldverify.cpp#L157))
- 残した穴: 子は project の既定の scene を描く。warm-up は frame の数なので、速い Release では実時間で短い(対照どうしの比較が捕まえる)。子を起こせるのは Windows だけ。既定値(30 s・3 frame)での所要時間は測っていない
- 形と test は manual 13 章の「断片を検査して印を付ける —— `verify_fold`(✅FG-2)」。着地の W1 で 33 件が緑(統合側の報告)

### 5.4 展開して編集・畳み直す

- 展開して編集: 今の `decompose_material_pass` / `apply_graph_template` のまま。一般化は要るときに
- 畳み直す: `fold_nodes {nodes[], group, folded}`。断片の宣言を書き、`verify_fold` を続けて走らせる。1 ticket

**合意した形(2026-10-02)。** 上の 2 行は案として残す。

- 展開して編集は今の `decompose_material_pass` / `apply_graph_template` のまま(一方通行。戻すのは RD-1 の undo)。畳んだ形へ戻す逆の RPC は作らない
- 畳み直すは §5.2 の合意の `fold_nodes {base_source_digest, group, nodes, folded}`(1 ticket)。断片の宣言と決定を書くだけで、`verified` は書かない。等価性の検査は別の ticket の `verify_fold`(§5.3)で、書いた直後の group は `unverified` として開いた形で走る(決定は効かない)
- 2 つの関係: 展開して編集した結果は root の node になる(material pass なら `render.material_pass` と draw_loop のループと body の definition、雛形なら雛形の nodes / definitions)。それを `fold_nodes` で group として宣言して決定を付けるのが、「畳んで書いてある」ものを開いた後で「開いて書いてある」側の断片(§3.1)にする道になる。開いた形を編集すれば digest が変わって group は `stale` になり、検査し直すまで決定は効かない
- `kind: "replacement"`(provider での置き換え)での畳み直しは FG-3 まで無い

## 6. 保存形

- **変えないもの**: `pelican.graph` の root、`rendering` 節、雛形、ループ、RD-1 の CAS と undo、「engine の合成は `decomposes` を読まない」
- **足すもの**: frame plan の `origin` / `from_nodes`(RPC の出力)、root の `fold_groups`(§5.2)
- **後に回すもの**: `rendering` 節の pass と feature を `nodes` に移す一本化。移行(MG 系)が絡むので、§7 の段で見せ方と結び付けを揃え、feature の線が派生のままで困る場面が実測で出てから決める

## 7. 段(WP の順)

| 段 | 中身 | engine | 受け入れ(要点) |
|---|---|---|---|
| **✅GL-1 見せ方だけ** | Graph に `rendering` 節の pass・preset の pass・feature を畳んだ node として出す。線は frame plan から派生した破線。ダブルクリックで開いた形を読む(material pass はゾーン、雛形のある feature は雛形)。領域 filter(既定 `render`)。Frame Plan の Open in Graph を全 pass に。graph の pass かどうかは名前の一致で判定(GL-2b で `origin` に置き換える)(`4f13a95c`) | 無し | Release で `deferred_geometry` が Graph に出て潜れる。GPU シナリオ 1 本 |
| **GL-2a engine の origin** | §5.1 | 有り | graph の pass と preset の pass が `origin` で区別でき、物理 plan の要素から node に戻れる |
| **✅GL-2b 層の印** | Studio が `origin` / `from_nodes` を読み、node の帯と詳細欄に L1〜L3 を出す。Frame Plan のどの層のどの行からも Open in Graph。GL-1 の判定を `origin` に置き換える(`91f3dc16`) | 無し | Barriers の行から node に戻る |
| **✅GL-1b 配置と再起動**(GL-2b の後に足した行) | 畳んだ node と派生の線を root の 1 回の配置(Frame Plan の Logical graph と同じ `layoutNodeGraph`)に入れる。graph の編集の commit が `restart_required` を返したら(engine の RS-1)Frame Plan と同じ再起動へ。ループの帯は回の数をゾーンの下端に任せる(`a103e100`。`restart_reason.detail` の読みの直しは `43317a76`) | 無し(RS-1 の応答を読む) | 台帳の GL-1b の完了記録 |
| **✅GL-1c 畳んだ node とゾーン**(同上) | 畳んだ node を、root に描いたもの(ゾーンの枠・下端の行・carry を含む)に重ねない。覚えた場所は重ならない間だけ使う(`ca6d774e`) | 無し | 台帳の GL-1c の完了記録 |
| **✅GL-1d Blank から Graph で描画を組む**(同上) | palette の `render.material_pass` が draw ループ(`<pass>_draws`、body `<pass>_draw` の `draw.material`)と `draws` の線を同じ列で連れて来る。Loop の欄の Over に Draw items。forward の pass を deferred の pass の無い pipeline に足すときは `material_route_default: "forward"` を同じ編集で送る。palette はキーボードで使える(`565de69b`) | 無し(RS-2 の param を使う) | 台帳の GL-1d の完了記録 |
| **✅GL-1e Graph から画面に出す**(同上) | root が宣言した rendering 節の fullscreen pass(Blank の `present`)が `input` の各項目をソケットで出し、可変の組があれば `+ input`。graph の node の出力を落とすと、Frame Plan のソケットへの drop と同じ関数が作る `edit_preset_pass` を Graph タブが送る(置き換え / 末尾に追加)。線は実線、Delete で外す。Graph の Undo は、自分の最後の編集が pass の編集である間は RU-1 の履歴へ。RS-3 の前の engine の拒否に 1 句足す(`38da763e`) | 無し(RS-3 の順序に乗る) | 台帳の GL-1e の完了記録 |
| **✅GL-1f pass の出力のソケット**(同上) | `render.material_pass`(color のソケットと `depth`)/ `render.shadow_depth`(`depth`)/ `render.snapshot_copy`(`destination`)が出力のソケットを出す。型は入力につないだ image の型。線を引くと書き手の `outputs` に port を足し(同じ列、Undo 1 回)、最後の読みが消えると外す。`present` への実線はその image を書いた pass の出力から(`7a3ea86c`) | 無し(RS-4 の port を使う) | 台帳の GL-1f の完了記録 |
| **✅GL-3 断片と決定** | `fold_groups`、`decisions` の欄(serial / no_alias / physical)、ローカル名を展開名へ写す段。engine は ✅FG-1(`61a88f9f`)、Studio 側は ✅GL-3a(`f5579426`) | §5.2・§3.6 | 断片に付けた決定が展開名で効き、L2 の表に出る |
| **✅GL-4 検査と印** | `verify_fold`、失効と `running open form` の表示、複数選択 → Fold。engine は ✅FG-2(`9c71f97b`)、Studio 側は ✅GL-4a(`f5579426`) | §5.3・§5.4 | 失効させた断片が開いた形で走り、名前付きの警告が出る |
| **GL-5 Frame Plan を mode に** | Frame Plan の編集入口を Graph の操作へ束ね、独立の入口を消す。書き先は今のまま(rendering 節の pass は `edit_preset_pass`)で入口だけを束ね、3 本に分ける: 5a 畳んだ node の詳細欄で編集(✅GL-5a `6565221b`。root の rendering 節の pass を Pass Editor と同じ部品・同じ `presetPassEditParams()` で直し、Serial / Isolate / No alias も置く。task の欄は root が `compute_tasks` を宣言できない(MG-2b)ので作らなかった)、5b 追加の操作(Add fullscreen pass / Add compute task / Add material pass / New render target)を palette と同じ関数に(✅GL-5b `60ff3a2c`。New render target を Frame Plan と Graph で 1 つの関数にし、Graph の palette の `render.target_ref` と詳細欄の出力の行に入口を足し、詳細欄で出力の target を選び直す。Add の 3 つは書き先が違う(Frame Plan は feature、palette は graph の `nodes`)ので束ねず、寄せ方は GL-6 の判断)、5c Frame Plan の Edit pass / Edit task を Graph への委譲に | 無し | Frame Plan から送る op が Graph と同じ 1 経路になる |
| GL-6(後) | 保存形の一本化(§6) | 大 | 別設計 |

注(2026-10-02): GL の段とは別に、Graph タブの使い勝手の WP が着地した。層の段ではないので表に行は足さない。UX-1(`7620fe1f`。線の吸着、ゾーンと body の node の drag、分岐ごとの body、画面に書くものを最後の列に)、UX-2(`5f048297`。`render.image` と render pass の値を選ぶ欄)、UX-3(`f34adcc7`。palette で置ける render node の種、Place の欄、ソケットの設定、EL-1 の一覧)、UX-4a(`ad1e0771`。Pass Editor の部品のフォーム)、RSX(`6a1069c8`。どのパネルの編集も `restart_required` で起動し直す、Rendering settings タブ)、GB-1(`3c6442a9`。Blank から GUI だけで組むシナリオ 5 本)。記録は台帳の各 WP の完了記録、見え方は manual 13 章。UX-2・UX-3・UX-4a は、名前や JSON を打つ欄を一覧から選ぶ欄に替えた(§4 の 8「打つものを増やさない」と同じ向き)。GB-1 では、forward の pass を画面に出すまでが Graph タブだけで通り(A)、deferred と lighting は Frame Plan の道で通った(B。Graph の palette の道は止まる。GB-1 の description の記録)。

GL-1 は engine を触らず、Release で使い心地を確かめられる。**GL-1 が使いにくければ、ここで §3〜§4 を直す**(損は小さい)。test は段ごとに要の 1 本ずつにし、全数は段の終わりに統合側で 1 回回す(利用者の指示「テストで実装速度を遅くしない」)。

## 8. 体現できるか・使いやすくできるか(2026-09-27 の答え)

**できる。ただし「層を全部見せる」をそのまま作ると使いにくい。** 成り立つ形は §4 の規則に限られる。

難しいのは 3 点で、答えは:

1. **等価性を誰が保証するか** — engine は証明できず検査しかできない。印と失効(§3.5)で「検査済みの範囲」を明示し、失効時は開いた形で走らせる。「速くなったはずが古い実装が走っていた」を fail-fast で防ぐ。検査の材料は今は test にしかないので、engine で作る(§5.3)
2. **「下の層を改造する」の実体** — Vulkan の直接編集は第二の正本。実体は宣言できる決定(§3.6。今ある graph 全体の口を断片から書く)と native の provider(RPE11b)の 2 段で、どちらも上の層では同じ畳んだ node に見える。決定だけでどれだけ速くなるかは**作ってから測る**
3. **feature の線** — 派生の破線で見せ、触れば「展開して編集」を促す。根治は一本化(§6)

正直な限界:

- native の実装を書く道は v0 では SDK の module(DLL の provider)で、Studio の中で完結しない
- L3(Vulkan)は**見えるだけ**。RenderDoc の見え方に寄せ、編集はしない。backend の固定は graph 全体の単位まで(node 単位は HEG3 以降)
- 一本化までの間、feature の線は派生のまま

## 9. 残る論点

- `decisions` の語彙をどこまで v0 に入れるか(serial / isolate / no_alias は口がある。physical fragments は fingerprint の拒否が要る)
- 検査の golden をどう選ぶか(project の viewport の 1 枚か、指定した case か)
- `pose` / `audio` の下の層(Evaluator の状態、voice)を帯に出すのは、`render` で形が固まってから
- Frame Plan の名前を変えるか(mode になった後)

## 10. 検証の指摘と答え

### 10.1 第 1 回(v1、2026-09-27。engine session が実物 01b244cd に当てた)

| 指摘 | 答え |
|---|---|
| decisions の resources / backends に当たる、feature の instance ごとの override の口は無い。口は graph 全体の単位で展開名で書く(`target_planning`・`vulkan_physical_fragments`・`vulkan_plan_pins`)。node 単位の backend の語彙も無い。「footprint override の口」という名前のものも無い | §1 に事実を足し、§3.6 を「今ある graph 全体の口を断片から書く」に直した。足りないのは名前を写す段と fingerprint の拒否。node 単位の backend は v0 では graph 全体のまま |
| 「definition + implementation」は `graph.loop_nested@1` / `graph.nested_instance@1` とぶつかる。開いた形は root の断片 | §3.1 を「root の断片」に直した(engine の提案) |
| `deferred_geometry` は feature ではなく preset の material pass | §1・§3.1 を直した |
| `decomposes` を engine の合成が読まない契約を変えることになる | 変えない(§3.1・§6) |
| `implementation` の key は既に使われている | `fold_groups` / `folded`(仮)にした |
| `fold_nodes` は isolate で物理の plan が変わる | §3.4 に書いた。断片の宣言は instance を作らないので `decisions` では変わらない。instance にまとめる操作は明示のときだけ、確認の文で言う |
| frame plan の `source` に `graph_node` は無い。preset が engine:// だと graph の pass も `engine` | §1 を直した。GL-1 は名前の一致で判定し、GL-2 で `origin` に置き換える |
| `origin` は GraphNodeOrigin を pass の provenance に通す作業が先。小さく単独で役に立つ | §5.1 を engine の最初の WP にした(GL-2a) |
| `verify_implementation` の材料は test にしかない | §3.5・§5.3 に前提として書いた |
| RPE11b に Studio の入口は無い、は誤り(`edit_region_replacement` と Regions ボタン) | §1 を直した |
