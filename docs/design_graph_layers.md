# Graph の層 —— 畳んだ高機能 node と、上から下まで辿れる 1 枚の canvas

状態: v1 草稿(2026-09-27)。利用者の指摘から起こした。§1 は実物で確かめた事実、§3〜§4 は Studio の形、§5 は engine と詰める契約の案。engine session の検証はまだ。

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

**出自は途中まで通っている。** frame plan の node には `source`(`feature` / `graph_node` / root …)、`provider_feature` / `provider_ref`、`loop_origin {loop, element, node}`、`authored_name` が付く([出す所](../src/core/renderingpass/frameplanner.cpp#L1905))。Studio はこれで Frame Plan の箱を feature ごとに束ね、`graph_node` の pass にだけ Open in Graph を出す。**Physical plan・Decisions・Backends・Barriers のタブは、この出自を持ち越していない**(pass 名で引くだけ)。

**一体版と分解版はつながっていない。** engine 同梱の雛形(`engine://graphs/*.json`)は `decomposes: {feature, …}` で「どの feature の分解版か」を言うが、使い道は `apply_graph_template`(feature を外して雛形を写す一方通行)だけ。「畳んだまま feature を node として見せ、開くと分解版が読める」段階は無い。material pass の Decompose into loop も同じく一方通行。**畳み直す道は無い。**

**等価性の検査は既にある。** 一体版と分解版の一致(queue の byte・記録された draw の列・golden の画素)は design_graph_loops §4.3 / §5.3 が課し、test で見ている。ただし「この project のこの definition が、この実装と一致している」という**印**は文書にも RPC にも無い。

**境界契約を保つ置換の仕組みはある。** RPE11b(WP201)の tagged region / subgraph replacement:元の config を typed logical graph に compile して境界の contract を作り、provider の結果を splice し、resource 集合・型・materialization・boundary port・fingerprint が一致したときだけ target lowering に進む。provider / source / replacement の provenance は `CompiledLogicalRenderGraph` と `VulkanTargetPlan` に残る([選択の型](../src/project/logicalrendergraph.hpp#L229))。**利用者が Studio から使う入口は無い。**

**下の層の分け方は設計にある。** [design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md) の LogicalCompiler → TargetExecutionCompiler → BackendLowerer → ExecutionLinker。Frame Plan のタブは、この段の出力を表で見せている。

**共通グラフは描画だけではない。** catalog の op は `render.*` / `draw.*` / `pose.*` / `value.*` / `audio.*` / `ui.*` / `event.*`。`playground_pose` の graph 文書には `pose.clip_sample` と `value.input` で walk と run を blend する definition があり、object の component から `graph.instance` として装着されている(root の `nodes` は空)。

## 2. ずれの本体

engine の設計は「同じ設定を、畳んだ形と開いた形の両方で持ち、下の層へ出自を通す」。Studio はそれを、**`rendering` 節を編集する画面群**と**`nodes` を編集する Graph**に分けて作った。F 系(Frame Plan の編集)が先にあり、ループ(LP-4)で Graph を `nodes` だけの canvas として後から足し、二つを 1 つの view にする WP を立てなかった。結果、

1. feature の pass は Graph に無く、Graph で足した pass は Frame Plan からしか展開結果を見られない。同じ fullscreen pass でも、どちらで足したかで別の文書の場所に書かれ、見た目で区別できない
2. `deferred_geometry` は Graph に出ないので、「中に draw_loop を持つ最適化済みの node」という設計上の意味が画面に無い。draw_loop 段のループは Frame Plan では material pass の 1 行なので、Graph のゾーンと同じものだと読み取れない
3. 下の層(物理 plan・barrier・backend)は表で見えるが、上の node の identity で引けない
4. 利用者が組んだ definition を、engine 同梱の feature と同じ「畳んだ実装を持つ node」にする道が無い(非対称)

## 3. 形

### 3.1 畳んだ node = definition + 実装

definition は今の「開いた形」(`nodes` / `outputs`)に加えて、**畳んだ実装**を 0 個か 1 個持てる。

```
definition
  ├ 開いた形: nodes / outputs / (graph.input)          ← 今のまま。編集の正本
  └ 実装(任意): { kind, ref, verified: {digest, at} }  ← §5.1
```

`kind` は 3 つ。どれも境界(公開 port と型)は開いた形と同じでなければならない。

| kind | ref | 実体 | 例 |
|---|---|---|---|
| `feature` | feature 名(+ preset) | engine 同梱の一体版。native の固定処理 | `deferred_geometry`(material pass)、bloom |
| `replacement` | region の provider | RPE11b の subgraph replacement provider(builtin か game DLL) | 利用者が書いた DLL の最適化 |
| `decisions` | — | §3.6 の宣言できる決定の束。実装は engine の compiler が決定に従って作る | pass の統合、target の aliasing、backend の指定 |

engine 同梱の feature は、engine が `engine://graphs/<name>.json` の雛形と対にして**最初から「実装付きの definition」として登録する**。project 側は何も書かなくても、`rendering.features[]` の instance が「その definition の畳んだ instance」として Graph に出る(§3.3)。今の `decomposes` の印はこの登録に吸収される。

### 3.2 層と identity

| 層 | 実体 | 見る所 | 編集 |
|---|---|---|---|
| L0 authoring | `pelican.graph`(node / definition / instance / loop / `rendering` 節) | Graph の canvas | する(正本) |
| L1 展開 | frame plan の node(instance を展開、ループを回に、feature を pass に) | canvas の node の帯、Frame Plan の Passes / Logical graph / Materials | しない。決定(§3.6)は L0 に書く |
| L2 物理 | target planning(scope、資源の寿命、aliasing) | node の詳細欄、Frame Plan の Physical plan / Resources / Barriers | しない |
| L3 backend | BackendLowerer / ExecutionLinker の結果(Vulkan の command の形、backend の選択) | node の詳細欄、Frame Plan の Backends / Decisions | しない。backend の選択は決定として L0 に書く |

**identity の鎖**:L0 の `{scope, node}`(instance の path と `loop_origin` を含む)を L1 の node が持ち(今の `source` / `graph_node` / `loop_origin` / `provider_feature` を 1 つの `origin` にまとめる)、L2 の scope と資源、L3 の command 群は、それを作った L1 の node の名前を持つ。engine が返す JSON のどの層の要素からも `origin` を辿って L0 の node に戻れることを、engine の契約にする(§5.2)。

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
| **潜る**(ダブルクリック / Open) | 畳んだ node の中(開いた形)を**読む**。実装が feature なら雛形の分解版を、ループならゾーンを、definition なら body を、同じ canvas に描く。パンくずで戻る | 変えない |
| **展開して編集** | その instance だけを開いた形の複製にする(今の Decompose into loop / Replace with loop version はこれの 2 例)。他の instance は畳んだまま(`shared (N users)` の印) | `rendering` 節の instance を外して `nodes` に写す。RD-1 の undo で戻る |
| **畳み直す** | 選んだ node 群(か definition)を definition にまとめ、実装を付ける。実装が `decisions` なら決定を書く。付けた直後に等価性の検査(§3.5)が走り、通れば印が付く | definition と実装を書く |

「表示を開閉しただけで最適化を解除しない」(design_graph_composition)は、潜るが読むだけであることで守られる。

### 3.5 等価性の印と失効

- 印は `verified: {digest, at}`。`digest` は開いた形の内容の digest(node / 線 / parameter。layout は含めない)、`at` は検査した日時
- 検査の中身は design_graph_loops §4.3 / §5.3 と同じ:frame plan(名前の対応表を通す)、queue の byte、記録された draw の列、指定した golden の画素。engine が RPC で走らせる(§5.3)
- **失効**:開いた形の digest が `verified.digest` と違えば失効。実装の側(feature の版、provider の fingerprint)が変わっても失効
- **失効した node の実行**:engine は**開いた形で走らせ**、`graph.implementation_stale@1 {definition, verified_digest, current_digest}` を名前付きで返す(黙って畳んだ実装を使わない。原則 7)。開いた形が走れない(feature を外していて、雛形が project に無い)ときは `graph.implementation_unverified@1` で止める
- 印は文書に保存する(印が無ければ、その definition の実装は「未検査」であり、engine は開いた形を使う)。engine 同梱の feature は engine の test が検査しているので、engine が登録時に印を持つ

### 3.6 宣言できる決定

「下の層の展開結果を改造する」を L0 に書く形。今 Frame Plan の Decisions タブに名前付きで出ているものを、definition(か root)に付けて保存できるようにする。

| 決定 | 書く所 | engine の扱い |
|---|---|---|
| pass の統合(連続する fullscreen を 1 つの region として provider に置換) | definition の `implementation.decisions.regions[]` | RPE11b の region_replacements。境界の contract が一致しなければ名前付きで断る |
| target の aliasing / 寿命の指示 | 同上 `resources[]` | target planning の override(既にある footprint override の口) |
| backend の選択(Vulkan の queue、compute か graphics か、…) | 同上 `backends[]` | TargetExecutionCompiler の requirement / selection(design_heterogeneous_execution_graph §5.3) |
| view family / extent(既にある) | node の parameters | 今のまま |

決定は**要求**であって結果ではない。engine が採れなければ名前付きで断り、採った結果は L1〜L3 の `origin` から辿れる(「この barrier はこの決定のせい」)。

## 4. Studio の見え方(使いやすさの規則)

1. **canvas は 1 枚、見える段は 1 つ。** 下の層は別の canvas ではなく、同じ node の帯(`×3 copies · 2 barriers · vulkan`)と、選んだときの右の詳細欄。Frame Plan は「L1 を時間順に見る mode」として残し、独立の編集入口ではなくなる。Frame Plan の編集ボタン(Edit pass / Add pass / …)は Graph の同じ操作を呼ぶ
2. **黒箱は黒箱のまま使える。** 初めて触る人は `deferred_geometry` を中を見ずに使える。最初の画面で中身を展開して見せない
3. **開けるが直せない状態を作らない。** 潜れば読める。直したければ「展開して編集」。畳んだ実装を持つ node の中で線や値を触ろうとしたら、その場で「展開して編集しますか」を 1 回だけ聞く(Cancel が既定)
4. **feature の線は派生の破線。** `rendering` 節の feature は target の名前でつながり edge を持たないので、frame plan の読み書きから線を派生させて破線で描く(body の出力の破線と同じ流儀)。触ろうとしたら 3 と同じ案内。根治は保存形の一本化(§6)
5. **印は既定と違うときだけ。** 失効は橙の印 1 つ、開いた形で走っている間は帯に `running open form`。全 node に常時 8 個の印が並ぶ形にしない
6. **潜る操作は 1 種類。** definition の body・畳んだ node の中・ループのゾーンの中、すべてダブルクリックと Open。パンくずで戻る
7. **領域は filter。** 既定 1 領域。またぐ線は口だけ(§3.3)
8. **打つものを増やさない。** definition の名前・region の名前は Studio が提案する(`suggestDefinitionName` の流儀)

## 5. engine の契約(案。engine session と詰める)

### 5.1 definition の `implementation`

```json
"definitions": {
  "my_post": {
    "nodes": [...], "outputs": {...},
    "implementation": {
      "kind": "decisions",
      "decisions": { "regions": [...], "resources": [...], "backends": [...] },
      "verified": { "digest": "<開いた形の digest>", "at": "2026-09-27T12:00:00Z" }
    }
  }
}
```

- `kind`: `feature` / `replacement` / `decisions`。`feature` は engine 同梱の definition にだけ engine が付ける(project の文書には書かない。書けば `graph.implementation_kind_reserved@1`)
- 境界の一致(公開 port と型)を組み直しで検査。違えば `graph.implementation_boundary@1 {definition, port}`
- 版は `pelican.graph` の field の追加なので上げない(v0_freeze §1.1 の「足すだけ」の流儀)

### 5.2 `origin` の鎖

- `get_frame_plan` の node の `source` / `graph_node` / `loop_origin` / `provider_feature` を、1 つの `origin {scope, node, instance_path?, loop?: {loop, element}, feature?: {name, preset?}, implementation?: {definition, kind}}` にまとめる(今の field は 1 段の間は残す)
- Physical plan の scope と資源、Barriers の各行、Backends の各項目に、それを作った L1 の node 名(複数可)を `from_nodes[]` で付ける
- 受け入れ: どの層のどの要素からも `from_nodes` → `origin` で L0 に戻れる。戻れない要素があれば engine の test が落ちる

### 5.3 検査と印

- `verify_implementation {definition}`:開いた形と実装で §3.5 の一致を取り、通れば `verified` を文書に書く(ticket。RD-1 の undo の対象)。通らなければ `graph.implementation_mismatch@1 {definition, what: "frame_plan" | "queue" | "draws" | "pixels", detail}`
- `get_status`(か `get_frame_plan`)に `implementations[] {definition, kind, verified: bool, running: "folded" | "open"}`

### 5.4 展開して編集・畳み直す

- 展開して編集: 今の `decompose_material_pass` / `apply_graph_template` を一般化した `open_implementation {definition | instance}`。1 ticket
- 畳み直す: `fold_nodes {scope, nodes[], definition, implementation?}`。definition を作り、選んだ node を `graph.instance` に置き換え、実装があれば付けて `verify_implementation` を続けて走らせる。1 ticket

## 6. 保存形

- **変えないもの**: `pelican.graph` の root、`rendering` 節、雛形、ループ、RD-1 の CAS と undo
- **足すもの**: definition の `implementation`(§5.1)。field の追加のみ
- **後に回すもの**: `rendering` 節の pass と feature を `nodes` に移す一本化。移行(MG 系)が絡むので、§7 の 1〜4 で見せ方と結び付けを揃え、feature の線が派生のままで困る場面が実測で出てから決める

## 7. 段(WP の順。それぞれ「何を取り消したら落ちるか」を付ける)

| 段 | 中身 | engine | 受け入れ(要点) |
|---|---|---|---|
| **GL-1 見せ方だけ** | Graph に `rendering` 節の pass と feature を畳んだ node として出す。feature の線は frame plan から派生した破線。ダブルクリックで雛形の分解版(あれば)か pass の詳細を読む。領域 filter(既定 `render`)。Frame Plan の編集ボタンは今のまま | 無し | Release で `deferred_geometry` が Graph に出て潜れる。GPU シナリオ: playground で feature の node を選び潜り、戻る。対照: 派生の線を消す変異で線の test が赤 |
| **GL-2 層の印と Open in Graph 全 pass** | `origin` の鎖(§5.2)を Studio が読み、node の帯と詳細欄に L1〜L3 を出す。Frame Plan のどの層のどの行からも Open in Graph | §5.2 | frame plan の全 node に Open in Graph。Barriers の行から node に戻る。対照: `from_nodes` を落とす変異で赤 |
| **GL-3 実装と印** | definition の `implementation`、`verify_implementation`、失効と `running open form` の表示、`open_implementation` で Decompose と Replace with loop version を置き換える | §5.1 / §5.3 / §5.4 の前半 | 失効させた definition が開いた形で走り、名前付きの警告が Studio に出る。対照: 失効の判定を外す変異で赤 |
| **GL-4 畳み直しと決定** | 複数選択 → Fold、`decisions` の欄(regions / resources / backends)、検査の実行と印 | §3.6 / §5.4 の後半 | 連続する fullscreen 2 本を畳んで region 置換し、画素が一致して印が付く。対照: 境界を変えた実装で `graph.implementation_mismatch@1` |
| **GL-5 Frame Plan を mode に** | Frame Plan の編集入口を Graph の操作へ束ね、独立の入口を消す | 無し | Frame Plan から送る op が Graph と同じ 1 経路になる(grep で第二の経路が無い) |
| GL-6(後) | 保存形の一本化(§6) | 大 | 別設計 |

GL-1 は engine を触らず、Release で使い心地を確かめられる。**GL-1 が使いにくければ、ここで §3〜§4 を直す**(損は小さい)。GL-3 以降は engine session と契約(§5)を先に固める。

## 8. 体現できるか・使いやすくできるか(2026-09-27 の答え)

**できる。ただし「層を全部見せる」をそのまま作ると使いにくい。** 成り立つ形は §4 の規則に限られる。

難しいのは 3 点で、答えは:

1. **等価性を誰が保証するか** — engine は証明できず検査しかできない。印と失効(§3.5)で「検査済みの範囲」を明示し、失効時は開いた形で走らせる。「速くなったはずが古い実装が走っていた」を fail-fast で防ぐ
2. **「下の層を改造する」の実体** — Vulkan の直接編集は第二の正本。実体は宣言できる決定(§3.6)と native の provider(RPE11b)の 2 段で、どちらも上の層では同じ畳んだ node に見える。決定だけでどれだけ速くなるかは**作ってから測る**
3. **feature の線** — 派生の破線で見せ、触れば「展開して編集」を促す。根治は一本化(§6)

正直な限界:

- native の実装を書く道は v0 では SDK の module(DLL の provider)で、Studio の中で完結しない
- L3(Vulkan)は**見えるだけ**。RenderDoc の見え方に寄せ、編集はしない
- 一本化までの間、feature の線は派生のまま

## 9. 残る論点

- `decisions` の語彙をどこまで v0 に入れるか(regions は RPE11b がある。resources / backends は override の口の実物を engine session と確かめる)
- 検査の golden をどう選ぶか(project の viewport の 1 枚か、指定した case か)
- `pose` / `audio` の下の層(Evaluator の状態、voice)を帯に出すのは、`render` で形が固まってから
- Frame Plan の名前を変えるか(mode になった後)
