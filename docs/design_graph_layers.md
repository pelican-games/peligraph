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

### 5.3 検査と印

- `verify_fold {group}`:開いた形と畳んだ形で §3.5 の一致を取り、通れば `verified` を書く(ticket。RD-1 の undo の対象)。通らなければ `graph.fold_mismatch@1 {group, what: "frame_plan" | "queue" | "draws" | "pixels", detail}`
- 前提として、一体版と分解版の名前の対応表をデータにし、実行中の engine で 2 つの形を描き比べる道を作る(今は test 専用)

### 5.4 展開して編集・畳み直す

- 展開して編集: 今の `decompose_material_pass` / `apply_graph_template` のまま。一般化は要るときに
- 畳み直す: `fold_nodes {nodes[], group, folded}`。断片の宣言を書き、`verify_fold` を続けて走らせる。1 ticket

## 6. 保存形

- **変えないもの**: `pelican.graph` の root、`rendering` 節、雛形、ループ、RD-1 の CAS と undo、「engine の合成は `decomposes` を読まない」
- **足すもの**: frame plan の `origin` / `from_nodes`(RPC の出力)、root の `fold_groups`(§5.2)
- **後に回すもの**: `rendering` 節の pass と feature を `nodes` に移す一本化。移行(MG 系)が絡むので、§7 の段で見せ方と結び付けを揃え、feature の線が派生のままで困る場面が実測で出てから決める

## 7. 段(WP の順)

| 段 | 中身 | engine | 受け入れ(要点) |
|---|---|---|---|
| **GL-1 見せ方だけ** | Graph に `rendering` 節の pass・preset の pass・feature を畳んだ node として出す。線は frame plan から派生した破線。ダブルクリックで開いた形を読む(material pass はゾーン、雛形のある feature は雛形)。領域 filter(既定 `render`)。Frame Plan の Open in Graph を全 pass に。graph の pass かどうかは名前の一致で判定(GL-2b で `origin` に置き換える) | 無し | Release で `deferred_geometry` が Graph に出て潜れる。GPU シナリオ 1 本 |
| **GL-2a engine の origin** | §5.1 | 有り | graph の pass と preset の pass が `origin` で区別でき、物理 plan の要素から node に戻れる |
| **GL-2b 層の印** | Studio が `origin` / `from_nodes` を読み、node の帯と詳細欄に L1〜L3 を出す。Frame Plan のどの層のどの行からも Open in Graph。GL-1 の判定を `origin` に置き換える | 無し | Barriers の行から node に戻る |
| **GL-3 断片と決定** | `fold_groups`、`decisions` の欄(serial / no_alias / physical)、ローカル名を展開名へ写す段 | §5.2・§3.6 | 断片に付けた決定が展開名で効き、L2 の表に出る |
| **GL-4 検査と印** | `verify_fold`、失効と `running open form` の表示、複数選択 → Fold | §5.3・§5.4 | 失効させた断片が開いた形で走り、名前付きの警告が出る |
| **GL-5 Frame Plan を mode に** | Frame Plan の編集入口を Graph の操作へ束ね、独立の入口を消す | 無し | Frame Plan から送る op が Graph と同じ 1 経路になる |
| GL-6(後) | 保存形の一本化(§6) | 大 | 別設計 |

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
