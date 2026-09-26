# 共通グラフのループ —— 1 種類のループ node と、2 つの実行段(v3、2026-09-26)

状態: **v3**。

検証の経緯:

- v1 → v2: Claude の subagent 2 本が敵対検証した(engine の実現性と、形式・作法・使い勝手)。codex は利用者指示で使用不可のため、その代替。指摘は 42 件。
- v2 → v3: 新しく足した部分を 1 本で再検証した。指摘は 27 件(致命 4 / 重い 11 / 軽い 12)。

どちらも全件を本文に反映し、答えの対応を §13 に置く。

v2 から v3 の主な変更:

- 展開を 2 段にした。形の展開は合成の前、回数・target・挿入位置の解決は合成の後(§5.1)。
- feature の pass を分解版で置き換える仕組みと、影・snapshot の pass を graph で書く node を、前提の WP に足した(§6)。
- 他のループの回ごとの出力を、回ごとに読む入力を足した(決定 2)。
- 外部の資源に触る回を、並べた順に明示の依存でつなぐことにした(§5.2)。
- cube の分解版の実行順が変わることを明記し、受け入れを面ごとの一致に変えた(§7.3)。
- WP を 15 本に細かく分けた(§11)。

## 0. 経緯と裁定

2026-09-25、利用者の問い「ディファードもフォワードもマテリアルに対するループ処理だと思うけど、それをノードから変更するのは今は無理?」から始まった。

今の material pass は、見えている instance を material ごとにまとめて bind と draw を行う、1 つの固定処理である(`src/core/renderer/materialrender.cpp`)。node から形を変える手段が無い。

| 日付 | 裁定(利用者) |
|---|---|
| 09-25 | ループの分解を **v0 に入れる**。仕様はよく考える |
| 09-25 | material ループは 3 段(選ぶ / 並べる / material ごとに描く)。本体は **material ごとの if** で、分岐ごとに subgraph を持つ。保存するのは条件と subgraph だけ(展開結果は保存しない)。v0 の material の対象は material pass(影・velocity・picking の描画ループは後) |
| 09-26 | ループの前の**並べる段**は、全ループに要る |
| 09-26 | ループの利用例すべてに使える**汎用のループ node を 1 種類**にする。実行の段は本体の中身で決まる。最適化で速度が落ちることを隠さない |
| 09-26 | **v0 は draw_loop 段(pass の中の draw ループ)と expand 段(組み直し時に pass へ展開)まで**。毎フレーム数が変わる段は v1 |
| 09-26 | **影の cascade と cube の 6 面も v0 に入れる** |

### 0.1 覆す暫定方針

[design_graph_composition.md の material 反復の暫定案](design_graph_composition.md#L449) のうち、次の 2 項目を覆す。

1. 「この段階で汎用foreach文法 … を追加しない」
2. 「明示的に展開して編集した場合は、展開後の構成を正本へ保存する」

根拠:

- **利用者の裁定がある**:上表の 09-25(保存)と 09-26(汎用ループ)。暫定案は当時、利用者が検討中の段階で書かれたものだった。
- **原則 3 を render で満たすには反復が要る**:原則 3 は、標準機能に一体版と分解版の両方を持たせる。今の分解版は手展開しか書く手段が無い。
  - bloom は 13 pass・9 target を JSON で手展開している。
  - planar reflection は、mip の連鎖を compute task 6 本で手展開している。
- **足すのは任意の実行時ループではない**:足すのは次の 2 種類で、どちらも組み直しのときに有限の形へ落ちる(§3)。
  - 回数と集合が組み直し時に決まるループ
  - 毎フレームの draw の列を、material 単位の分岐で描くループ
- **展開形を正本にしない**:展開したものを正本にする代わりに、要素を指す分岐を足す(決定 3)。こうすれば、展開形という第 2 の正本が生まれない。

暫定案の残りは、決定 3・5・6 がそのまま引き継ぐ。

- 対象集合・反復本体・公開入出力を保存し、実行時の一覧は派生とする
- 回ごとの資源・共有資源・前回から受け取る値を区別する
- 独立な集合処理と順序依存の反復を区別する

## 1. 前提(2026-09-26、develop `77ed40d6`。実物で確認し、検証の訂正を反映)

### 1.1 graph の展開と render への変換

- **展開のしくみ**
  - `definitions` と `graph.instance` は、名前の付け替えだけで展開される。body の node id は `<instance>/<node>` になる(`src/project/graphcomposition.cpp:110`)。
  - 展開器は node を id の辞書順に並べる(:35, :137)。**配列の順は保たれない**。
  - 展開器は文書だけを見る(:172)。
- **parameter の受け渡し**
  - instance に渡せる parameter は、有限の scalar だけ(:52-53, :62-63)。
  - node の `parameters` はそのまま複写される(:148)。
  - 値の差し込みは feature 層にしか無い。`bindFeatureParameters`(`src/project/featurecompose.cpp:1417`)は、文字列全体が `$name` のときだけ置き換える。`$` で始まる文字列は、ほかに次の意味でも使われている。
    - view family の名前(`$reflection/planar`)
    - 予約された node id(`$upload/…`)
    - 展開器の内部の `$scopes`
- **編集 API の制限**
  - 入れ子の instance は、編集 API が拒む(`graph.nested_instance@1`)。
  - **definition を作る・消す・改名する op は無い**(:777-899)。
  - 参照の検査は `graph.instance` だけを見る。
- **graph の render node と合成**
  - graph から作れる render node は、`render.buffer/attribute/image/compute/fullscreen/raster` だけ(`rendergraphcomposition.cpp:48-52`)。material pass・shadow_depth・snapshot_copy は作れない。
  - 指せる資源は、graph の中の `render.image` / `render.buffer` / `render.attribute` だけ(:39-49)。
  - 作った pass は、pass family の**末尾に足される**(:205-214)。
  - `composeRenderFeatureConfig` は、**graph の変換を preset と feature の合成より先に行う**(`featurecompose.cpp:2929-2932`。preset は 2951、feature の target と pass は 3058 以降)。
- **feature の pass を外す手段は無い**
  - `pass_overrides` が受けるのは `input` / `resource_ports` / `material_resources` の 3 つだけ(`featurecompose.cpp:2458-2466`)。
  - `insert` は feature の pass だけが持つ(:2022-2037)。
- **frame planner**
  - データ依存は、宣言順で直前に書いた pass で決まる(`frameplanner.cpp:1440-1453`)。
  - compute task は常に全 pass の後に宣言される(:1683-1685)。
  - `after` の参照先が無ければ落ちる(:1459-1470)。
- **preview / XR の変種**
  - feature の除外は、依存する feature に伝わる(`graphvariantpolicy.cpp:860-880`)。
  - graph の node は、この伝わり方の外にある。

### 1.2 資源の一部を指す

- **render target の形**
  - render target は `dimension`(2d / cube)、`mip_levels`、`layers`、`extent_scale` を持つ。
  - 影の target の layer は、実行時に光源数 × cascade 数へ伸ばされる(`directionalshadowviewprovider.cpp:345-367`)。
- **attachment の subresource**
  - pass の JSON の attachment は、`{target, subresource}` 形で mip を 1 つ指せる。
  - 実行時は、`layer_count` が論理 view 数と一致しなければならない(`renderingpassruntimecompiler.cpp:1257-1290`)。
  - 共通グラフの変換は、attachment の `subresource` を拒む(`rendergraphcomposition.cpp:156-158`)。
- **1 枚の image の中の mip 連鎖**
  - graphics の pass では書けない(`passshapepolicy.cpp:23-25, 419-444`、`rendergraphcomposition.cpp:174-175`)。compute なら書ける(`shaderresourceport.cpp:540-577`)。
  - 依存と barrier は image 全体の単位で追われる(`frameplanner.cpp:245-250`)。

### 1.3 既にある反復

| 反復 | 形 | 回数が決まる時期 |
|---|---|---|
| bloom の段 | JSON の手展開(13 pass、target 9 枚)。上り段 e は、下り段の e-1 段目の target へ `load_op: Load` と加算 blend で書き戻す | 書いた時 |
| planar reflection の mip 連鎖 | JSON の手展開(compute 6 本、1 枚の image の mip 1〜6)。`$reflection/planar` の view ごとに回る | 書いた時 |
| 影の cascade | engine の中のループ。1 つの `shadow_depth` pass を、`$shadow/directional` の view ごとに繰り返す。view = 光源 × cascade(index = 光源 × cascade 数 + cascade)。provider は `cascade_count` と target を feature の instance と `surface_resources`(`producer: "shadow_depth"`)から読む(`directionalshadowviewprovider.cpp:29-99`) | cascade 数は組み直し時、光源数は実行時(光源の枠は最低 1) |
| cube capture の 6 面 | engine の中のループ。8 つの pass(material pass 3、snapshot_copy 2、ほか)を、`$capture/cube` の view ごとに繰り返す | 組み直し時(常に 6) |
| 点光源 / spot の影 | engine の中のループ。view = 光源 × 6 面 / 光源 | 面は 6、光源数は実行時 |
| material ごとの draw | engine の中のループ(`renderMaterialDraws`) | queue は毎フレーム |
| 前のフレームを読む | `X@history` | — |

view family のしくみ:

- **scope の作られ方**
  - scope は、tile の候補・profile・attachment の辺の条件が揃ったときだけ、複数の pass をまとめる(`targetrenderplanning.cpp:2123-2185`)。
  - desktop では、cube の 8 pass はそれぞれ別の scope になる。
- **scheduler の回し方**
  - 二次 family の scope は、1 view の雛形として扱われる。scheduler はそれを view の数だけ順に繰り返す(`viewexecutionscheduler.hpp:255-360`)。
  - 回す単位は scope なので、今の cube は **pass が外側**になる(geometry×6 → ssao×6 → …)。
- **1 つの view だけで動かす仕組みは無い**
- **view ごとのデータの引き方**:次のものは、すべて (family, view index) で引く。どの pass が動いているかには依らない。
  - attachment の layer(`render_pass_frame_setup.cpp:11-43`)
  - frame UBO の slot(`renderer.cpp:5331-5440`)
  - culling 済みの draw(`polygoninstancecontainer.cpp:2254-2680`)
  - view-projection(`renderer.cpp:2589-2675`)
- **target plan が無い経路**:二次 family の node は、view 0 で 1 回だけ走る(`renderer.cpp:1576-1611`)。

### 1.4 material pass の中

- **実効 material と variant**
  - 1 つの pass の中では、各 material は**ちょうど 1 つの実効 material**(1 本の pipeline)に解決される。
  - 別の pipeline で描くには、別の登録 material(`material_variant`)を別の pass で描く(`materialcontainer.cpp:1181-1281, 1834-1854`)。
  - glTF の material は variant を持てない。
- **同じ range を 2 回描く経路は無い**
  - `bindResource` は、実効 material で pipeline と descriptor set を比べる(`materialcontainer.cpp:2268-2274`)。
  - `renderMaterialDraws` は、直前の material を base の id で持つ(`materialrender.cpp:282-302`)。
- **pipeline の cache**
  - material の pipeline の cache は増えるだけで、解放されない(`materialcontainer.cpp:851-866`)。
  - `makePipelineKey` は shader の id を鍵にし、define を含まない(:105-170)。
- **並べ方**
  - 並べ方は、phase ごとに全体で 1 つ(`renderpipeline.cpp:1963-2004`)。
  - queue の構造と共有:
    - queue は (phase, sort view) ごとに作る。
    - tag の filter ごとに range を作り、filter の安定 id で pass 間で共有する。
- **値の置き場所と reload**
  - 値は material ごとの GPU の slot にあり、GPU の material index で引く。
  - 値だけの hot reload は、slot だけを書き換える。
  - surface の reload(`prepareSurfaceMaterialReload`、`materialcontainer.hpp:373`)は、世代を変えずに pipeline と layout を差し替える。
- **受け皿の無い material**
  - 受け皿の無い material は、`material_route_unserved@1` で断る(`src/core/material/materialgenerationplanner.cpp:779-836`)。
- **影と velocity**:影と velocity の pass は、queue 全体を描く(`materialrender.cpp:375, 406`)。

## 2. ループの形(利用者が触るもの)

```
[集合] → [絞る] → [並べる] → [上から N 個] → for each {  本体  }
                                              if   条件 → body
                                              elif 条件 → body
                                              else      → body
                                            └ carry: 前の回の出力を次の回の入力へ(連鎖のときだけ)
```

### 決定 1: node は 1 種類 `graph.loop`

`graph.loop` は、`graph.instance` と同じ**構造の node** で、展開器が扱う。実行時の op ではなく、catalog にも載せない(§8)。置けるのは root の scope だけ。

```json
{
  "id": "bloom_up",
  "op": "graph.loop",
  "parameters": {
    "over":     {"range": {"count": 3}},
    "order":    {"by": "element", "direction": "descending"},
    "branches": [{"body": "bloom_up_step"}],
    "carry":    [{"from": "color", "to": "source"}],
    "tier":     "expand"
  },
  "inputs": {
    "source": {"node": "bloom_down", "port": "color"},
    "blend_target": {"node": "bloom_down", "port": "level[]",
                     "element": {"$expr": "element"}}
  },
  "outputs": ["color", "color[]"]
}
```

| 項目 | 意味 | v0 で受けるもの |
|---|---|---|
| `over` | 集合 | `range`、`view_axis`(expand)、`draw_items`(draw_loop) |
| `where` | 絞る(要素ごとの条件) | draw_loop:material の tag。expand:要素の条件 |
| `order` | 並べる | `{"by": "element", "direction": "ascending"｜"descending"}`(expand)、`{"by": "phase_default"}`(draw_loop。v0 はこれだけ) |
| `limit` | 並べた後の上から N 個 | expand だけ。回数を超えたら回数に丸める |
| `branches` | 本体。上から順に `when` を見て、最初に合ったものの `body`(definition 名)を使う | 最後は `when` 無し(else)でなければならない |
| `carry` | body の出力 port → 次の回の body の入力 port | expand だけ |
| `tier` | 段の固定(任意)。`"draw_loop"`・`"expand"` | 書くと、その段で実行できないときに `graph.loop_tier_mismatch@1` |

**body の書き方**

- body は、常に `definitions` の definition 名で書く。
- 全分岐の body は、同じ公開入出力を持たなければならない(`graph.loop_branch_interface@1`)。
- 何もしない回は `"body": null` と書く。
- body の中に `graph.loop` と `graph.instance` は置けない(`graph.loop_nested@1`)。

**else は必須**

else の無い `branches` は、`graph.loop_branch_else_missing@1` で断る。

### 決定 2: ループの入出力 port

**入力は 3 種類**

| 種類 | 書き方 | 回ごとの値 |
|---|---|---|
| 全回に同じ値 | ふつうの edge | 全回に同じ値を配る |
| carry | `carry` の `to` にある入力 | 0 回目はループの入力 edge の値、1 回目以降は前の回の `from` の値 |
| **回ごとの入力** | edge に `"element": {"$expr": …}` を付ける。指す先は他のループの `<port>[]` | 回ごとに式を評価する。式の `element` は**受け手のループ**の要素。指した要素が無ければ `graph.loop_element_unknown@1` |

bloom の上り段 e は、「回ごとの入力」で下り段の `level[e-1]` へ書き戻す。

**出力は、body の出力 `p` ごとに 2 つ**

loop の `outputs` には、使うものを宣言する。

| 出力 | 中身 | 回数が 0 のとき |
|---|---|---|
| `p` | 最後の回の値 | carry の `from` なら、carry の入力 edge の値。carry でなければ無い(参照すると `graph.loop_output_empty@1`) |
| `p[]` | 回ごとの値の束 | 空 |

**`"body": null` の回**

carry の値をそのまま次へ通す。その回の `p[]` は無い。

**型の検査**

carry の型が合わなければ、`graph.loop_carry_type_mismatch@1` で断る。

### 決定 3: 保存するのは形だけ

**保存するもの**:`graph.loop` の parameter と、body の definition だけ。

**保存しないもの**:次のものは派生で、Studio は読むだけにする。

- 展開した pass と target
- どの要素がどの分岐に入ったか
- pipeline の差し替え

**1 つの要素だけを変えたいとき**:その要素を指す `when` の分岐を足す。

**展開後の名前は参照させない**

- 展開後の名前を authored 文書から参照することは断る(`graph.loop_expansion_referenced@1`)。
- ループ全体の順は、ループ node の id を `after` / `before` に書いて決める。
  - ループの id は、展開後の**全** pass に写す。
    - `after: [<loop>]` を持つ pass は、ループの最後の pass の後に動く。
    - ループを `after: [X]` にすると、先頭の回が X の後に動く。
  - 回数が 0 なら、非活性な node と同じく、その名前を依存から除く。

**当たらなくなった分岐**:どの要素にも当たらなくなった分岐は、報告の `unmatched_branches` と Studio の警告に出す。

### 決定 4: 並べる段

- **expand**:`{"by": "element", "direction": …}`。省略時は昇順。
- **draw_loop**:v0 は `{"by": "phase_default"}` だけを受け、それ以外は `graph.loop_order_unsupported@1` で断る。ループごとの並べ方は v1 にする。queue の (phase, sort view) 一意・容量・GPU segment・二次 family・XR の不変条件を、すべてほどく必要があるため。
- **同じ値の要素**:engine の既存の安定順で並べる。

### 決定 5: 回ごとの値(式)

**書き方**

- 式は object 形だけにする。平文の文字列は走査しない。
  - 数値は `{"$expr": "…"}`。
  - 文字列に埋め込むときは `{"$format": "BLUR_STEP={element}"}`。
- 式を置けるのは、許可リストの field だけ。
  - `render.*` の `extent_scale`
  - port の `subresource.mip` / `layer`
  - `shader.defines` の要素
  - `view_select` の値
  - `range.count`
  - `when` / `where` の比較値
  - 回ごとの入力の `element`
- 許可リストの外に書くと、`graph.loop_expression_invalid@1` で断る。

**変数**

| 変数 | 意味 |
|---|---|
| `element` | 集合の値(0 始まりの整数。`view_axis` の軸の値も整数で、面の名前は表示用のラベル) |
| `index` | 並べた後の位置 |
| `count` | 展開後の回数 |

- `where` で使えるのは、`element` と組み直し時の定数だけ。
- `range.count` の式で使えるのは、組み直し時の定数だけ。
- 上の 2 か所で `index` / `count` を使うと、`graph.loop_expression_invalid@1` で断る(順序の循環を防ぐため)。

**文法**(実装で BNF に固定する)

- 使えるもの:整数と実数のリテラル、`+ - * / %`、比較、`&& || !`、括弧、`min` / `max`、`pow2(n)`。
- 整数を要求する場所では、整数だけで計算する(`/` は切り捨て)。
- 実数の `pow` は持たない。有理数で計算し、最後に 1 回だけ実数にする(浮動小数の丸めを platform に依存させない)。
- 0 除算・overflow・長さの超過(256 字)・入れ子の超過(16 段)は、`graph.loop_expression_invalid@1` で断る。

**確定した文法と置き場所(LP-1b)**

- BNF は `src/project/graphloopexpression.hpp` の冒頭が正本。`$format` の `{…}` の中は整数式(`{{` `}}` は波括弧)。
- 式はループの body の中でだけ書ける(root の node や instance の definition では、許可リストの場所でも `field_not_allowed`)。
- `when` は `element` / `index` / `count`、`where` は `element` だけ、`range.count` は組み直し時の定数だけ。edge の `element` は、受け手がループならその要素、そうでなければ定数だけ。
- body に value node があるループは、値の partition が合成と別に評価されるため、count / where / when が合成の前に決まらなければならない(決まらなければ `not_static`)。
- `set_input` の `source` は `{node, port, element?}` を受ける(Studio が回ごとの入力を張るため)。

**組み直し時の定数**

- `mip_levels_of("<target>")`、`layers_of("<target>")` など。
- 実行時に伸ばされる target(影の target)は定数として使えない。使うと `graph.loop_expression_invalid@1`(`not_static`)。
- 例:planar の連鎖は `"count": {"$expr": "mip_levels_of(\"planar_reflection_color\") - 1"}`。

## 3. 実行の段(engine が決めるもの)

| `over` の種類 | body の中身 | 段 | 実行 |
|---|---|---|---|
| `draw_items` | `draw.*` だけ | **draw_loop** | 1 つの material pass の中で、今の queue をそのまま使う。range ごとに、分岐が決めた描き方を記録する |
| `range`、`view_axis` | `render.*`・`value.*` | **expand** | 組み直しのときに body を N 個に展開する。展開したものは、ふつうの pass・compute task・target として frame graph に渡す |
| 上記以外 | — | 断る | `graph.loop_tier_unsupported@1` |

断るときの `error_fields` は `{node, reason}`。`reason` は次のとおり。

| reason | 起きる場面 | 最適化との関係 |
|---|---|---|
| `carry_in_draw_loop` | draw_loop に `carry` を書いた | 前の回へ依存すると、並べ替えられない |
| `limit_in_draw_loop` | draw_loop に `limit` を書いた | 毎フレームの draw 数を切るのは、毎フレームの段の話 |
| `per_frame_value` | 回数・`when`・`where` が、毎フレーム変わる値を見る | pipeline と展開を、組み直し時に作れない |
| `resource_in_draw_body` | draw_loop の body で資源を作る | 1 つの pass に収まらない |
| `mixed_body` | body に `draw.*` と `render.*` が混ざる | 実行の場所が 1 つに決まらない |
| `nested` | body に `graph.loop` か `graph.instance` がある | v0 の範囲外 |
| `set_unsupported` | v0 に無い集合(光源、動的な view、実行時の軸) | v1 の毎フレームの段 |

**v0 では段が黙って落ちない**:v0 では `over` の種類で段が決まり、違反はすべて断りになる。v1 で毎フレームの段を入れるときの約束は、次のとおり。

- 段の固定が無ければ、遅い段へ落ちることを許す。そのときは報告と log に `demoted_from` を出す。
- 段の固定があれば、落とさずに断る。

**どの段でも導けないもの**:アルゴリズムの置き換え(光源ごとの pass ↔ clustered lighting)は、どの段でも導けない。原則 3 の一体版と分解版の関係として、意味の契約で結ぶ。

## 4. draw_loop 段: material ごとの if

### 4.1 表面

**`over` と `where`**

- `over` は `{"draw_items": {"contract": "<material_contract>"}}`。`legacy_gbuffer_v1` は `graph.loop_contract_unsupported@1` で断る。
- `where` は、今の `material_filter` と同じ語彙で書く:`{"tags": {"include": [...], "exclude": [...], "require_all": [...]}}`

**`when` で見られる属性**

すべて material 単位で、組み直し時に決まるもの。

- `material`(照合の鍵は §14)
- `tags`
- `contract`
- `skinned`
- `has_variant`

複数の key を書いたら AND になる。material の値(`values`)は見ない。

**body の node**(`pelican.render` の op)

| node | 入力 → 出力 | 意味 |
|---|---|---|
| `draw.material` | `after: draw_stream`(任意)→ `draws: draw_stream` | 今と同じ描き方 |
| `draw.variant {variant}` | 同上 | 名前付き variant で描く。variant を持たない material に当たると `graph.loop_variant_missing@1` |
| `draw.override {render_state?, defines?}` | 同上 | 同じ material を、render state と define を差し替えて描く |

- **描く順**:`after` の edge で表す。definition の `outputs.draws` が列の末尾になる。
- **surface の差し替え**:v0 では `draw.variant` でだけできる。
- **phase をまたぐ差し替え**:`graph.loop_phase_crossing@1` で断る。
- **`draw.*` を置いた definition**:loop の body 専用になる。`graph.instance` から使うと `graph.definition_role@1` で断る。

**`render.material_pass` との接続**

- **draws をつないだとき**:ループの出力 `draws`(`pelican.render.draw_stream@1`)は、`render.material_pass` の入力 `draws` に 1 本つなぐ。
- **draws をつながないとき**:`draws` を空けた `render.material_pass` は、今の一体版と同じ既定の描き方になる(`draw.material` の else)。cube の分解版の body に置く material pass は、この形になる。

### 4.2 engine の変更

1. **pipeline の差し替え**
   - `draw.override` は material を生成しない。(世代, pass, base material, override の鍵) → pipeline の**別の表**で描く。
   - descriptor set と GPU の material index は、base のものをそのまま使う。値の編集も値の reload も、2 回目の描画に同じように効く。`maxMaterials` の枠も使わない。
   - 差し替えの pipeline は、base の pipeline の記述(`graphicsDesc`)を複写し、render state と define を替えて作る。**世代が所有**し、登録 material の cache とは共有しない。これで、世代と一緒に解放しても登録 material を壊さない。
   - define の差し替えには shader の compile が要る。runtime compiler の無い build では、`graph.loop_override_needs_compiler@1` で断る。
     - define が効く stage:vertex と fragment の両方。
     - 世代あたりの compile 数には上限を置く。数は実装で決める。
   - 差し替えの pipeline は、base と **pipeline layout が互換**でなければならない(set layout の binding・stage flags・push constant)。互換でなければ `graph.loop_override_interface@1` で断る。
     - compiler は、使われない binding を消す。そのため define で使う資源が変わる差し替えは、多くがここで断られる(§14)。
2. **range ごとの描き方の列**
   - 「(世代, pass, base) → 描き方の列」を持つ。
   - `renderMaterialDraws` は、range ごとに列を順に回す。前回の記録は「最後に bind した実効の pipeline と descriptor set」にする。
   - GPU の segment 経路も、同じ列を回す。
3. **世代の持ち物**
   - 描き方の列と差し替え表は、`RendererRuntimeGeneration` が持つ。
   - 組み直しの候補の世代で作り、拒否されたら捨てる。世代と一緒に解放する。
   - material が後から増えたとき(glTF の読み込み・spawn)は、live の世代の分岐を評価して足す。候補の世代があれば、候補でも評価する。
   - surface の reload と、組み直しで base の pipeline がその場で作り直されるときは、差し替えの pipeline も base の新しい記述に**追従**させる(表そのものは組み直さない。surface の reload は binding と render state の変更をもともと断るため。LP-2)。
   - material の照合の鍵(§14 の 2): project の material は `<material 文書>#<名前>`、glTF は `<asset>#material[<index>]`(名前を見ないので重複名・空名でも一意)、それ以外は登録名(LP-2)。
4. **queue は変えない**
   - range は、今どおり material 単位で切れる。
   - `where` は、今の tag filter と同じ安定 id で共有する。
5. **受け皿の検査**(LP-3a で `graph.loop` の parse と一緒に入れる)
   - `graph.loop` の `over` と `where` に入るのに、どの分岐でも描かれない material があれば、`material_route_unserved@1` に足して断る。
   - `"body": null` の分岐は「受けた」と数える。
   - この検査は `graph.loop` にだけ掛ける。

### 4.3 一体版との関係と、既知の制限

**一体版の意味**

今の material pass の JSON は、形式を変えずに残す。その意味は、次の draw_loop を `render.material_pass` が受けたもの(pass 名は同じ)と**同じ**と定める。

- `over`: `draw_items(material_contract)`
- `where`: `material_filter`
- `order`: `phase_default`
- 分岐:else の 1 つだけ。body は `draw.material` 1 つ(`material_variant` があれば `draw.variant`)

**一致の検査**

一体版と分解版で、次の 3 つが一致すること。

- queue の byte
- 記録された draw の列
- hybrid preset を使う golden の case の画素(case は実装時に列挙する)

**既知の制限**

- draw_loop で描かなかった material も、影と velocity の pass には描かれる(TAA の残像になりうる)。v1 で影と velocity の描画ループを分解するときに扱う。
- 半透明で 1 material に複数回描くとき、奥から手前の並びは draw の単位で決まる。「全部の裏面の後に、全部の表面」にしたいときは、pass を 2 つに分ける。

## 5. expand 段: 組み直し時の展開

### 5.1 展開は 2 段

graph の変換は、preset と feature の合成より先に走る(§1.1)。一方で、ループの回数・target の参照・挿入位置は、合成の後でないと決まらない。そこで展開を 2 段に分ける。

| 段 | いつ | 何をする | 失敗 |
|---|---|---|---|
| **形の展開** | 編集・読み込み(今の graph の変換の位置) | 構造の検査(else、分岐の入出力、入れ子、body の役割、式の文法、展開名の参照)。socket の型は総称で済ませる(target の参照は「何かの image」) | 編集時に名前付きで断る |
| **解決** | 組み直し(preset と feature の合成の**後**) | 回数・組み直し時の定数・target の参照の型・`view_axis` の長さ・挿入位置を解決し、body を回ごとに展開して pass・task・target を足す | 組み直し時に名前付きで断る |

**解決の段の中身**

- **node id と出所**
  - 展開した node の id は `<loop>/<element>/<node>`。要素の値で名付けるので、回数や順を変えても、残る回の名前は変わらない。
  - 出所は、`origin` に `{loop, element}` を足す(`instance_path` とは別の欄)。
- **名前の付け替え**:body の中の `after` / `before` にある同じ body の node 名は、回ごとの名前に付け替える(instance の同じ穴もここで塞ぐ)。
- **展開の上限**
  - 1 つのループで 64 回、graph 全体で pass と task が 128 本まで。超えると `graph.loop_expansion_limit@1`。
  - frame planner の推移閉包は O(n³) なので、LP-1a で、編集ごとの組み直しの時間を測って見直す。

### 5.2 回ごとの資源と、順序

**回ごとの資源**

- **body の中の `render.image`**:回ごとに別の target になる。
- **body の外の image の一部を回ごとに使う**:port の `subresource` に式を書く。v0 は **compute だけ**。
- **graphics の attachment**:v0 では `subresource` を解かない。
  - 解かない理由:
    - XR の layer 数の制約がある。
    - 同じ pass で読んで書くと、feedback として断られる。
    - 独立な回が同じ image の別の layer に書くと、曖昧さが出る。
  - view ごとの layer は、§7 の view の選択で書く。

**順序**

**外部の資源**(body の外の image・buffer、`render.target_ref`)に触る回は、読みだけの回も含めて、並べた順に**明示の `after`** でつなぐ。

- 理由:展開器は id を辞書順に並べる(`<loop>/10/x` は `<loop>/9/x` より前)。資源 node が producer のとき、lowering は `after` を足さない。宣言順だけに頼ると、10 回以上や `descending` で順が壊れる。
- 回ごとの `render.image` だけに触る回どうしは、つながない(並列にできる)。

### 5.3 一体版との関係

bloom と planar reflection の分解版を、engine 付属の graph として出荷する。

- `engine://graphs/bloom_loop.json`
- `engine://graphs/planar_mip_chain_loop.json`

v0 では feature の JSON はループに書き換えない。一体版は、そのまま残す。

**使い方**:分解版は、一体版の feature の pass を**置き換える**形で使う(§6 の「feature の pass の置き換え」)。

- bloom は、分解版が feature 全体の代わりになる。そのため feature を外して、分解版の graph を入れる。
- planar は、feature の資源・view family・`surface_resources` をそのまま使い、mip 連鎖の task 6 本だけを置き換える。

**一致の検査**:分解版と今の feature で、次の 2 つが一致すること。

- frame plan(pass と target の名前の対応表を通す。所有者付きの scope の `isolate` の差は表に明記して除く)
- 画素

## 6. 前提の仕組み(ループより先に要るもの)

| 仕組み | 内容 | 理由 |
|---|---|---|
| **既存の target の参照** `render.target_ref {name}` | rendering 節・preset・feature が宣言した target を、graph から指す。解決は §5.1 の解決の段で行う。所有はしない(作らず、aliasing と history は元の宣言に従う)。port に `history: true` を書くと `X@history`(前のフレームの資源)を読める。`X@history` へ書く port は断る(history を持つ target そのものへ書くことは、一体版と同じく許す。LP-0a)。無い名前は `graph.target_ref_unknown@1` | 分解版が `lit_color`・G-buffer・`scene_depth`・feature の target を読み書きするため |
| **graph の pass の挿入位置** | graph の render node と `graph.loop` に、feature の pass と同じ `insert`(`{after｜before: "<pass 名>"}`、`canonical_anchor`)を足す。解決の段で、feature の合成の後に入れる。展開した列は、先頭の 1 本を anchor に入れ、残りを宣言順で連続して並べる(`enforceCanonicalOrder` が宣言順の依存を足すので、明示の edge は要らない。LP-0b で実測)。`canonical_anchor: X` は `insert: {after: X}` と同じで、anchor の bucket の中では feature の pass より後ろに入る。anchor は node の `pass_set` の family の中だけで探す。compute の body には `insert` を書けない(compute task は常に pass の後に宣言されるため。`graph.insert_on_compute@1`)。anchor と `pass_set` の family が違えば `graph.insert_family_mismatch@1` | 今は末尾に足されるだけで、分解版が一体版と同じ場所に入れない |
| **definition の編集 op** | `add_definition` / `remove_definition` / `rename_definition`(逆 op を作れる形)。参照されている definition を消すと `graph.definition_referenced@1`。`graph.input_referenced@1` / `graph.input_unknown@1` の対象に、`graph.loop` の `inputs`・`carry`・`branches[].body` を足す | branch の body になる definition を、Studio とエージェントが作れるようにするため。`edit_graph_document` の op の列は今も 1 つの候補に順に当てて最後にまとめて確定するので、「ループの `inputs` に 1 つ足す」と「全分岐の definition に `graph.input` を 1 つ足す」は、既存の列 1 回で送れる(新しい op は作らない)。ただし `graph.loop` の構造の検査(分岐の入出力の一致など)は、op ごとではなく**列の最後に 1 回**だけ行う。途中の op で分岐どうしの入出力が一時的に食い違っても断らない。`graph.definition_referenced@1`・`graph.input_unknown@1`・`graph.input_referenced@1` も列の最後に検査する(LP-0c)。`remove_definition` の逆 op の本体は、Studio が `get_graph_instances` の `definitions` から事前に読む(`base_source_digest` の CAS で一致が保証される)。definition の名前 `"root"` は op の `scope` と紛れるので断る |
| **feature の pass の置き換え** | feature の instance の設定に `pass_replacements: {"<feature の pass / task 名>": "<graph の node か loop の id>"}` を足す。置き換えた pass は feature から外れる。`surface_resources.producer` などの名前の参照は、置き換え先へ写す。置き換え先が同じ資源を書かなければ、今の `producer … no longer writes` と同じく断る。置き換えた名前を他から参照すると、置き換え先に写す | 分解版が一体版の pass を置き換えられない。feature を残すと pass が二重になり、外すと provider が cascade 数と target を失う |
| **graph で書く pass の種類** | `render.shadow_depth`・`render.snapshot_copy` を足す(pass の JSON の同名の type と同じ意味)。`render.material_pass` は LP-3a | cube の 8 pass と影の pass を graph で書くため |
| **変種で外れる feature への依存** | preview / XR の変種で feature が外れたとき、その feature の target を `render.target_ref` で指す node と、その pass を `insert` で指す node は、同じ変種で非活性にする。これも今の依存の伝わり方と同じく、報告に出す | 依存が伝わらないと、変種の組み直し全体が落ちる。LP-0d で、`after` / `before` で外れた feature の pass を指す node、非活性の loop の出力を読む node と carry する loop、非活性の pass を anchor にする run も非活性にした。報告は `deactivated_graph_nodes` と diagnostics の kind `graph_node_deactivated`(detail `<variant>:<reason>:<feature>`)。変種の preflight の graph の拒否は、graph の code のまま `variant` を添えて返す |

## 7. view の軸を選ぶ(cascade と cube の 6 面)

### 7.1 考え方

view family の view に、名前の付いた**整数の軸の座標**を持たせる。

| family | 軸 | 長さが決まる時期 |
|---|---|---|
| `$shadow/directional` | `light`、`cascade` | `cascade` は組み直し時(`cascade_count`)、`light` は実行時 |
| `$capture/cube` | `face`(0〜5。表示のラベルは +X, -X, +Y, -Y, +Z, -Z(ASCII)) | 組み直し時(6) |
| `$shadow/point` | `light`、`face` | `face` は 6、`light` は実行時 |
| `$shadow/spot` | `light` | 実行時 |

**`view_select`**

- pass と compute task に `view_select: {<軸>: <整数>}` を足す。rendering 節の JSON でも、graph の node でも書ける。
- scheduler は、その pass を**軸の値が一致する view だけ**で動かす。選ばなかった軸は、今と同じく実行時に全部を繰り返す。
- view の行列・frame UBO・culling 済みの draw・attachment の layer は、今どおり (family, view index) で引く。

**`view_axis`**

- expand 段の `over` に、`{"view_axis": {"family": "<family>", "axis": "<軸>"}}` を足す。
- 回数は、組み直し時に決まる軸の長さ。実行時の軸を指すと、`set_unsupported` で断る。
- body の pass には、`"view_select": {"<軸>": {"$expr": "element"}}` を書く。

### 7.2 engine の変更

1. **軸の座標**
   - family の provider が、view ごとに軸の座標を返す。
   - 組み直し時に決まる長さは、組み直しの段で引ける。
   - 作者が宣言した family は軸を持たないので、`graph.view_select_axis_unknown@1` で断る。
2. **`view_select` を運ぶ場所**
   - 次のすべてに `view_select` を足す。
     - pass と task の定義
     - frame planner の node
     - execution node
     - physical scope の JSON と fingerprint(`targetrenderplanning.cpp:377-382`、`vulkancompletephysicalplan.cpp:649`、`vulkanphysicalfragment.cpp:844-862`)
     - 作者の physical fragment と pin の検査
     - Studio が読む frame plan の model
   - `view_select` の違う pass は、同じ scope に合流させない(合流の条件 `targetrenderplanning.cpp:2123-2185` に足す)。
3. **scheduler**
   - `view_select` のある二次 family の scope は、一致する view index の分だけ invocation を出す。
   - invocation は `execution = sequential` のまま、`logical_view_count` は family の view 数のままにする。1 件だけにするときも、single_view にしない。single_view にすると、snapshot_copy が layer 0 を写してしまう(`renderer.cpp:2411-2416`)。
   - 一致する view が 0 個のフレームは、非活性の node と同じ扱いにする(`completed_nodes` に入れ、barrier の検査を通す)。
   - 読み飛ばした node が barrier の元で、それ以前に誰もその image を触っていない(layout が undefined)ときは、その memory dependency を省く(LP-Va。無いと layout の追跡が例外を出す)。
4. **target plan の無い経路**
   - 二次 family の node が view 0 で 1 回だけ走る経路では、`view_select` のある pass を `graph.view_select_needs_target_plan@1` で断る。黙って layer 0 に上書きしないため。
5. **検査**
   - 無い軸・長さの外の値・実行時の軸は、組み直し時に断る(`graph.view_select_axis_unknown@1` / `graph.view_select_runtime_axis@1`)。
6. **XR**
   - XR の二次 family の multiview は今も未実装なので、今のまま順に回す。

### 7.3 一体版との関係

**cube capture**

- 分解版は、`face` の軸のループになる。body には、次の 8 本の pass を置く。
  - `render.material_pass` 3 本(draws を空ける)
  - `render.snapshot_copy` 2 本
  - ほか
- `pass_replacements` で、feature の 8 pass を置き換える。
- 実行の順は**変わる**。
  - 今は pass が外側(geometry×6 → ssao×6 → …。desktop では 8 pass が別々の scope)。
  - 分解版は face が外側。
  - image 単位の layout の遷移と barrier は増える。
- 一致の検査:
  - face ごとの SSAO の cube の byte(`test/golden_ssao_test.cpp`、`test/fixtures/wp338_ssao/cube.r8`)
  - face ごとに取り出した invocation の部分列

**影の cascade**

- 分解版は、`cascade` の軸のループになる。body は `render.shadow_depth` 1 本。
- `pass_replacements` で、feature の `shadow_depth` を置き換える(`surface_resources.producer` はループへ写す)。
- 光源が 1 つなら、invocation の順は同じ。
- 光源が複数なら、順が変わる。今は光源の中で cascade が回る。分解版は cascade の中で光源が回る。
  - 各 layer は独立に書かれるので、結果は同じ。
  - pass が cascade 数だけに増え、shadow の pipeline も pass ごとに持つ(性能の差。記録する)。
- 一致の検査:
  - layer ごとの depth と culling の数、画素。光源 1 と 2 の両方で比べる。
  - 今の `runBLayerShadowEquivalence` は最終の画素しか比べていない。そのため、layer ごとの test を新しく起こす。

どちらの分解版も、engine 付属の graph として出荷する。

## 8. 型・node の登録・版

**型**

- `pelican.render.draw_stream@1`(stream)を足す。
- `object_set` は v0 では登録しない。

**構造の node**

- `graph.loop` は構造の node。catalog には載せない。
- `v0_freeze.md` の `pelican.graph` の行と、Studio の構造 node の一覧に足す。

**`pelican.render` の op**

- 次の op を足す。
  - `draw.material` / `draw.variant` / `draw.override`
  - `render.material_pass` / `render.target_ref` / `render.shadow_depth` / `render.snapshot_copy`
- catalog の test と、`v0_freeze.md` の op 表を更新する。
- `render.material_pass` の port は `parameter_dependent`(契約から socket を作る)。

**pass と feature の JSON**

- pass と task の JSON に `view_select` を足す。
- feature の instance の設定に `pass_replacements` を足す。

**版**

- すべて v0 の tag の前に**足すだけ**なので、既存文書の版は上げない。移行手順も要らない。
- tag の後に形を変えるなら、`pelican.graph` 2 と移行手順を登録する。
- `v0_freeze.md` の §1.1・§1.3・§5 を更新する。

## 9. 報告と Studio

**view の軸と選択(LP-Va / LP-Va2)**

- `get_frame_plan` の pass・compute task・snapshot_copy の node に、`view_axes: [{axis, length, runtime, labels?}]`(軸の順は provider の index の順、`length` は今の世代の値、実行時の軸は `null`)を載せる。
- 直近のフレームで一致する view が 0 個だった node には `no_selected_view: true` を載せる。
- `edit_preset_pass`(root pass の編集を含む)の置換 payload は `view_select` を受ける(省略で保持、`null` で消す)。

**報告の RPC**

- `get_frame_plan` の node に、`loop: {tier, reason?, demoted_from?, elements, unmatched_branches}` を足す。
- draw_loop の `elements` は paging する(`{offset, limit}`)。

**`decompose_material_pass`**

- 引数は `{base_source_digest, base_preset_digest?, pass}`。
- 一体版の material pass を、次の 3 つに書き換える。rendering 節と graph 節を、1 つの transaction で書き換える。
  - `render.material_pass`(pass 名と位置を保つ)
  - `graph.loop`
  - definition
- engine の preset と feature は、fork していなければ `project_preset_required` で断る。
- 逆の操作は v0 では持たない(Studio の undo と git が担う)。

**Studio(GUI session)**

- Studio は `graph.loop` を、Blender のループゾーンのように枠として見せる(2026-09-26、利用者と GUI session で決定)。枠の中に body の definition の node を並べて直接編集し、外から枠へ線を引くとループの入力と definition の入力を足してつなぐ。carry は戻り矢印、分岐は枠の上端のタブ(else だけならタブ無し)、展開後の列と material ごとの分岐表は読むだけの表示。保存形式は上のまま(body は definition 名)で、見せ方は Studio の持ち物。

- loop node に、段の印・理由・`tier` の固定の欄を付ける。
- 分岐表を編集できるようにする(definition の作成を含む)。
- 要素ごとの分岐と、`unmatched_branches` の警告を出す。
- 「Decompose into loop」を足す。
- `view_select` の欄を足す。
- `pass_replacements` の設定を足す。

## 10. 名前付きエラー

| code | 出る時 | error_fields |
|---|---|---|
| `graph.loop_branch_else_missing@1` | 編集・読み込み | `{node}` |
| `graph.loop_branch_interface@1` | 編集・読み込み | `{node, branch, definition, port}` |
| `graph.loop_nested@1` | 編集・読み込み | `{node, definition}`(body の definition に `graph.loop` がある。`graph.instance` がある body は `graph.loop_tier_unsupported@1` の `nested`。LP-1a) |
| `graph.loop_feature_pending@1` | —(予約。LP-1b の後は出ない) | `{node, feature}` |
| `graph.loop_tier_unsupported@1` | 編集・組み直し | `{node, reason}` |
| `graph.loop_tier_mismatch@1` | 組み直し | `{node, tier, actual}` |
| `graph.loop_order_unsupported@1` | 編集・読み込み | `{node, order}` |
| `graph.loop_expression_invalid@1` | 編集・読み込み・組み直し | `{node, pointer, reason}`(`syntax` / `unknown_variable` / `not_integer` / `not_static` / `division_by_zero` / `overflow` / `too_long` / `field_not_allowed`) |
| `graph.loop_carry_type_mismatch@1` | 編集・読み込み(型は形の段で分かる。LP-1b) | `{node, from, to}` |
| `graph.loop_element_unknown@1` | 組み直し | `{node, port, element}` |
| `graph.loop_output_empty@1` | 組み直し | `{node, port}` |
| `graph.loop_expansion_limit@1` | 組み直し | `{node, count, limit}` |
| `graph.loop_expansion_referenced@1` | 編集・読み込み | `{name, pointer}` |
| `graph.loop_contract_unsupported@1` | 編集・読み込み | `{node, contract}` |
| `graph.loop_variant_missing@1` | 組み直し | `{node, material, variant}` |
| `graph.loop_phase_crossing@1` | 組み直し | `{node, material}` |
| `graph.loop_override_interface@1` | 組み直し | `{node, material, binding}` |
| `graph.loop_override_needs_compiler@1` | 組み直し | `{node}` |
| `graph.loop_override_compile_limit@1` | 組み直し | `{node, count, limit}`(define の差し替えの compile は世代あたり 16。1 件約 200 ms。LP-2) |
| `graph.definition_role@1` | 編集・読み込み | `{definition, node}`(node は graph.instance の id) |
| `graph.loop_draws_unconnected@1` | 編集・読み込み | `{node, count}`(draw_loop の出力 draws が 0 本 / 2 本以上。LP-3a) |
| `graph.definition_referenced@1` | 編集 | `{definition, referenced_by}` |
| `graph.definition_duplicate@1` | 編集 | `{definition}`(LP-0c) |
| `graph.definition_missing@1` | 編集 | `{definition}`(rename では `from`。LP-0c) |
| `graph.target_ref_unknown@1` | 組み直し | `{node, name}` |
| `graph.target_ref_type_mismatch@1` | 組み直し | `{node, name, field, declared, actual}`(field: `kind` / `topology`。LP-0a) |
| `graph.history_write@1` | 編集・読み込み・組み直し | `{node, port}`(`X@history` へ書く port。LP-0a) |
| `graph.history_unavailable@1` | 組み直し | `{node, port, target}`(history を持たない target の `@history` を読む。LP-0a) |
| `graph.insert_on_compute@1` | 編集・読み込み | `{node}` |
| `graph.insert_anchor_unknown@1` | 組み直し(`insert`)/ 編集・読み込み(`canonical_anchor` の名前) | `{node, anchor}`(LP-0b) |
| `graph.insert_family_mismatch@1` | 組み直し | `{node, anchor}` |
| `graph.view_select_axis_unknown@1` | 組み直し(作者が差し替えた family はフレームの時点) | `{pass, family, axis}`、長さの外の値は `value`・`length` を足す(LP-Va) |
| `graph.view_select_primary_family@1` | 組み直し | `{pass, family}`(`$main` など二次でない family。LP-Va) |
| `graph.view_select_runtime_axis@1` | 組み直し | `{pass, family, axis}` |
| `graph.view_select_needs_target_plan@1` | 組み直し | `{pass}` |
| `render_feature.pass_replacement_unknown@1` | 組み直し | `{feature, pass}` |
| `graph.node_missing@1`(置き換え先) | 組み直し | `{node, feature, pass}`(LP-0d) |

**既存の code に寄せるもの**

- 受け皿の無い material は、今の `material_route_unserved@1` に足す。
- 置き換え先が資源を書かないときは、今の `producer … no longer writes` と同じ code にする。

**web**

- web は `graph.operation_unregistered@1` で断る。
- `view_select` と `pass_replacements` は、web の parser が名前付きで断る(WEB-1)。

**message と pointer の表**

- 組み直しで、ループの回ごとの node(`<loop>/<element>/<node>`)が名前付きで断られたときは、error_fields に `loop_origin: {loop, element, node}` が付く(LP-1b)。`graph.loop_expression_invalid@1` の `pointer` は常に authoring 文書の JSON pointer。
- 各 code の JSON pointer と message の形は、実装の WP の報告で表にし、manual に載せる。

## 11. WP の分け方

| WP | 内容 | 前提 |
|---|---|---|
| LP-0a | `render.target_ref` と、展開の解決の段(合成の後の段を作る) | — |
| LP-0b | graph の pass の挿入位置 | LP-0a |
| LP-0c | definition の編集 op と、参照の検査の拡張 | — |
| LP-0d | feature の pass の置き換え(`pass_replacements`)と、変種で外れる feature への依存 | LP-0a |
| LP-0e | `render.shadow_depth`・`render.snapshot_copy` | LP-0a |
| LP-1a | `graph.loop` の芯:形の展開(構造の検査)、解決の段での展開、`range`、名前と出所、`after` の付け替え、外部資源の直列化、ループ id の依存、展開名の参照の拒否、上限、報告 RPC(expand 分) | LP-0a、LP-0b、LP-0c |
| LP-1b | 式(`$expr` / `$format`、許可リスト、組み直し時の定数)、`carry`・回ごとの入力・`p[]` | LP-1a |
| LP-1c | bloom / planar の分解版の出荷と、一致の検査 | LP-1b、LP-0d |
| LP-Va | view の軸と `view_select`(provider の座標、運ぶ場所、scope の合流、scheduler、target plan の無い経路)。rendering 節の JSON で test する(ループ無し) | — |
| LP-Vb | `over: view_axis`、cube capture と影の cascade の分解版の出荷と一致の検査 | LP-Va、LP-1b、LP-0d、LP-0e、LP-3a |
| LP-2 | draw_loop の engine:pipeline の差し替え、描き方の列、前回の記録、世代の持ち物、surface の reload | — |
| LP-3a | `render.material_pass` の変換(全 field、pass 名を保つ、draws を空けたときの既定)、`draw.*` の op、draw_loop の分類、受け皿の検査、一体版との一致の検査 | LP-0a、LP-0b、LP-1a、LP-2 |
| LP-3b | `decompose_material_pass` と、報告 RPC(draw_loop 分、paging) | LP-3a |
| LP-4 | Studio(GUI session) | LP-1b、LP-3b、LP-Va |
| WEB-1 追記 | web が新しい op と field を名前付きで断る | LP-1a の着地時点から |

**docs**:各 WP で次を更新する。

- manual 06:ループ・target の参照・挿入位置・view の選択・置き換え・拒否の表
- manual 13:RPC・definition の op・decompose
- manual 14:用語
- source-code-guide の該当章
- `v0_freeze.md`

## 12. 受け入れ(各 WP の受け入れの元。すべて「何を取り消したら落ちるか」を付ける)

1. **expand の等価**
   - bloom の分解版(置き換え済み)の frame plan(対応表を通す)と画素が、今の bloom feature と一致する。planar の mip 連鎖も同じ。
   - 対照:式の置き換えを外す、`carry` を外す、回ごとの入力を最後の回に固定する、外部資源の直列化を外す(10 回以上のループで順が壊れる)。それぞれの変異で赤になる。
2. **view の等価**
   - cube capture の分解版で、face ごとの SSAO の cube の byte と、face ごとの invocation の部分列が一致する。
   - 影の cascade の分解版で、光源 1 と 2 の両方について、layer ごとの depth と culling の数と画素が一致する。
   - 対照:scheduler の `view_select` の絞り込みを外す(invocation の観測で赤。各回が全 layer に同じ内容を書くので、画素だけでは赤にならない。LP-Va で実測)。scope の合流の条件から `view_select` を外す(desktop ではこれらの pass はもともと合流しないので、tile 構成の CPU の test で赤)。
   - 対照:target plan の無い経路で、`view_select` の pass が名前付きで断られる。
3. **draw_loop の等価**
   - hybrid の golden の case で、一体版と分解版の queue の byte・draw の列・画素が一致する。
   - 対照:前回の記録を base に戻す変異で、2 度描きの test が赤になる。
4. **draw_loop の分岐**
   - tag で分けた 2 分岐で、分岐ごとに pipeline が違い、画素が期待どおりになる。
   - `draw.override{cull: front}` → `draw.material` の 2 度描きで、裏面の色が見える。
   - 値を編集すると、2 回目の描画にも効く。
   - 対照:差し替えの pipeline を base に戻す変異で赤になる。
5. **段の分類**
   - §3 の各 reason が名前付きで断られる。`tier` を固定して破ると `graph.loop_tier_mismatch@1`。
   - 対照:分類を外す変異で赤になる。
6. **保存**
   - ループを含む graph を保存して開き直すと、同じものになる。保存物に展開結果が無い。
   - 対照:展開結果を書き戻す変異で赤になる。
7. **増える material**
   - glTF を後から読み込むと、新しい material が分岐に従って描かれる。
   - 対照:登録時の分岐の評価を外す変異で赤になる。
8. **編集**
   - definition の追加・改名・削除が、逆 op で戻る。
   - 参照されている definition の削除と、body の入力の削除は、名前付きで断られる。
   - 対照:参照の検査を外す変異で赤になる。
9. **置き換え**
   - `pass_replacements` で置き換えた pass が feature から外れる。`surface_resources.producer` の参照が置き換え先に写る。
   - 置き換え先が資源を書かないと断られる。
   - 対照:置き換えの適用を外す変異で、pass が二重になって赤になる。

## 13. 検証の指摘と答え

### 13.1 第 1 回(v1、2026-09-26。engine 側 E と形式・作法側 F)

| 指摘 | 重さ | 答え |
|---|---|---|
| E-C1 / F-H3 graph が既存 target を指せない | 致命 | §6 `render.target_ref`、LP-0a |
| E-C2 / F-H3 graph の pass は末尾追記 | 致命 | §6 挿入位置、LP-0b |
| E-C3 / F-F1 bloom の上り段、loop の port | 致命 | 決定 2 |
| F-F2 draw の順、body の形 | 致命 | §4.1、決定 1 |
| F-F3 definition の編集 op | 致命 | §6、LP-0c |
| F-F4 展開名が保存物に入る | 致命 | 決定 3、§5.1 |
| E-H1 / F-H2 `$` 式 | 重い | 決定 5 |
| E-H2 override の生成 material | 重い | §4.2 の 1 |
| E-H3 世代 | 重い | §4.2 の 3 |
| E-H4 ループごとの並べ方 | 重い | 決定 4 |
| E-H5 graphics の attachment subresource | 重い | §5.2 |
| E-H6 `render.material_pass` と fork | 重い | LP-3a / LP-3b、§9 |
| F-H1 変数の矛盾 | 重い | 決定 5、決定 2 |
| F-H4 分類と token | 重い | §3 |
| F-H5 構造 node と catalog | 重い | §8 |
| F-H6 エラーの名前 | 重い | §10 |
| F-H7 死んだ分岐、値の reload | 重い | 決定 3、§4.1 |
| F-H8 view の回の除外 | 重い | 利用者裁定で v0 に入れた。§7 |
| F-H9 分解版が fixture だけ | 重い | §5.3 / §7.3 出荷 |
| F-H10 variant の枠 | 重い | §4.2 の 1 |
| 軽い 19 件(E-L1〜12、F-L1〜6、§1 の訂正) | 軽い | v2 で本文に反映(§1、§4.1〜4.3、§5、§8〜§12) |

### 13.2 第 2 回(v2、2026-09-26。新しい部分の再検証)

| 指摘 | 重さ | 答え |
|---|---|---|
| F1 分解版が feature の pass を置き換えられない | 致命 | §6 `pass_replacements`、LP-0d、§5.3 / §7.3 |
| F2 cube と影の body を graph で書けない | 致命 | §6 `render.shadow_depth` / `render.snapshot_copy`、LP-0e。LP-Vb の前提に LP-3a |
| F3 graph の変換が合成より先 | 致命 | §5.1 展開を 2 段、LP-0a で解決の段を作る |
| F4 cube の invocation の順が同じという誤り | 致命 | §1.3・§7.3 を訂正、受け入れを face ごとに |
| H1 `view_select` の持ち場 | 重い | §7.2 の 2・3 |
| H2 target plan の無い経路 | 重い | §7.2 の 4 |
| H3 回ごとに違う入力 | 重い | 決定 2(回ごとの入力)、決定 5 の許可リスト |
| H4 宣言順頼みの順 | 重い | §5.2(外部資源に触る回を明示の `after` で直列化) |
| H5 ループ id と 0 回 | 重い | 決定 3 |
| H6 insert の意味 | 重い | §6(feature の insert に合わせる、合成の後、鎖、compute は断る) |
| H7 変種で外れる feature | 重い | §6 |
| H8 draws と expand の組み合わせ | 重い | §4.1(draws を空けたら既定の描き方) |
| H9 pipeline の cache と所有 | 重い | §4.2 の 1(別の表、世代が所有、`graphicsDesc` の複写) |
| H10 surface の reload | 重い | §4.2 の 3 |
| H11 WP の粒度 | 重い | §11(15 本に分割、受け皿の検査を LP-3a へ、WEB-1 の時期) |
| L1 実行時に伸びる target | 軽い | 決定 5(`not_static`) |
| L2 影の test の参照先 | 軽い | §7.3(layer ごとの test を新設) |
| L3 view 0 個 | 軽い | §7.2 の 3 |
| L4 軸の値は整数 | 軽い | §7.1、決定 5 |
| L5 `count` の式の変数 | 軽い | 決定 5 |
| L6 出力の名前と 0 回 | 軽い | 決定 2 |
| L7 define の compile | 軽い | §4.2 の 1 |
| L8 resource interface の定義 | 軽い | §4.2 の 1、§14 |
| L9 target_ref と history | 軽い | §6 |
| L10 引用の訂正 | 軽い | §7.2 |
| L11 anchor の family | 軽い | §6(`graph.insert_family_mismatch@1`) |
| L12 複数光源の性能 | 軽い | §7.3 |

## 14. 残る論点

1. **define の差し替えで使う資源が変わる場合**
   - `draw.override` の define で使う資源が変わると、pipeline layout が base と互換でなくなり、`graph.loop_override_interface@1` で断られる。
   - これを受けるには、VI-1 の可変入力を material の set に広げる必要がある。v1 で扱う。
2. **`material` の照合の鍵**
   - glTF の重複名・空名の扱いが未定。LP-2 で決めて報告する。
3. **展開の上限と、define の compile 数の上限**
   - LP-1a と LP-2 で測って決める。
4. **光源の数が実行時に決まる軸**
   - 点光源と spot の光源ごと、複数光源の directional の光源ごとの軸が該当する。v1 の毎フレームの段で扱う。
5. **v0 の WP の本数と所要**
   - 15 本は、v0 の残りの作業として最大の塊になる。
   - 着地の順は §11 の前提の順。LP-Va と LP-2 は、ほかと並べて走らせられる。
6. **置き場所を指定した graph の pass と surface_resources**(LP-0b で発見。**LP-3a で解決**: 配置を surface resources の選択の前へ移した。expand ループの展開で入る pass は今も選択の後)
   - 置き場所を指定した graph の pass は、解決の段で `applySurfaceResourceContracts`(fullscreen_consumers と material の選択)の後に入るので、消費側として選ばれない。
   - LP-3a の `render.material_pass` を `insert` で置くと、影の map などが結ばれない。LP-3a で、配置を surface resources の前に移すか、配置の後に選択をやり直す。
7. **置き換えた consumer の pass と surface_resources**(LP-3a と LP-0d の merge で判明)
   - `pass_replacements` の置き換えは loop の展開の後(surface resources の消費側の選択より後)に行う。そのため、feature の consumer の pass(影を受ける material pass など)を graph の pass で置き換えると、置き換え先は影などを受けない(producer の置き換えは問題ない)。`material_consumers` / `fullscreen_consumers` の名前も写していない(LP-0d)。LP-Vb(cube の material pass を置き換える)で、置き換えの後に消費側の選択をやり直すか、名前を写す。
   - `pass_replacements` の置き換え先に draw_loop の id は書けない(`render.material_pass` の id を書く)。
