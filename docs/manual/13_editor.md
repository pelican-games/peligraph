# 第13章 エディタとエディタ RPC

対象: pelican2(2026-09-17 時点、HEAD=`82054c05`)/ このマニュアルはコードを正とする

## この章で学ぶこと

- **`pelican_player` が「編集サーバ」になった**という今回いちばん大きな変化(✅WP149〜172)
- 編集の正本 `AuthoringSceneDocument` と、シーンファイルとの関係
- 編集の実行モデル — チケット・フレーム境界コミット・global CAS
- 編集 RPC のリファレンスとエラーの読み方(**総数は挙げない** —— メソッドが増えるたび腐るため)
- プレビューの 4 段梯子(どこまでが「本物の絵」か)
- 保存(`save_scene`)・スナップショット・ImGui のエディタパネル・Python クライアント

**編集 RPC の全体像(接続 → 問い合わせ → 編集 → 保存)**

1. **接続** —— `pelican_player --rpc` を起動し、stdin/stdout の JSON-RPC で `open_editor_session` を呼んで `actor_id` を受け取ります(§13.4)。
2. **問い合わせ** —— `scene_tree` と `get_components` で今のシーンと各コンポーネントの値・schema を読み、`get_scene_revision` で「自分が見ている版」(`SceneRevision`)を控えます(§13.5)。
3. **編集** —— 控えた版を `base_revision` に添えて `edit` を呼びます。返るのは結果ではなく**チケット**で、実際の適用は次のフレーム境界です。結果は `get_edit_result`(ヘッドレスなら `step_frame` の応答の `edit_results[]`)で受け取ります(§13.3)。取り消しは `undo` / `redo`(§13.4)。
4. **保存** —— `save_scene` でメモリ上のオーサリング文書をシーンファイルへ原子的に書き出します(§13.8)。**これを呼ぶまでディスクは変わりません。**

エラーの読み方だけは先に §13.6 を見てください —— `edit` 系の失敗は JSON-RPC の `error` ではなく**成功応答の中の `result.status`** に出ます。

## 13.1 全体像 — 何ができるようになったか

2026-07-18〜19 のウェーブで、実行中の `pelican_player` に対して**外部からシーンを編集する API** が入りました。オブジェクトの生成・削除・親子付け替え・コンポーネントの追加削除・値の変更・undo/redo・プレビュー・保存が、すべて JSON-RPC のメソッドとして呼べます。

> **設計決定(D0 の具体化 — 編集 API は RPC ひとつだけ):** 編集操作は**まず RPC メソッドとして定義**され、エンジン内の ImGui インスペクタも、外部の Python クライアントも、将来の Qt エディタも、**同じ関数を呼ぶ**。エディタ専用の裏口 API は作らない。結果として「人間の GUI 操作」と「エージェントの自動操作」が同じ意味論・同じテストになる。
> (**D0** = 「エディタ特権の禁止」。2026-07-08 のユーザー決定で、標準エディタも公開契約の上に建つ 1 クライアントにすぎない、という規則です — [../design_devstudio_direction.md](../design_devstudio_direction.md) §0.5・[第1章 §1.0](01_overview.md)・[第14章 用語集](14_glossary.md))

⚠ 用語の注意: この章でいう「エディタ」は次の 2 つを指します。Qt 版の Pelican
Studio(`pelican_studio`)は別 process player の native viewport を持ち、schema-driven Inspector と
gizmo からもこの章と同じ編集 RPC / preview lease を呼びます([第10章](10_tools.md) §10.7)。

**Studio が呼ぶ編集面はこの 1 つではありません。**render-tools dock の Features タブは
`edit_render_features`(`add` / `remove` / `set_parameters` / `materialize`)を、Graph タブと
Inspector の「装着したグラフ」欄は `edit_graph_document` / `edit_graph_instances` を呼びます。
後者はシーン編集とは別の実行モデルで、preview lease と scene journal を通りません。
Graph タブの編集には接続中だけ有効な独自の Undo / Redo 履歴があります。
Features と Inspector の装着操作はその履歴の対象外です(§13.5「グラフの装着を編集する」)。

1. **編集 RPC**(この章の §13.3〜§13.8)
2. **エンジン内蔵の ImGui パネル**(§13.9 — Object Tree / Inspector / Asset Browser)

### Studio の未保存変更

シーンに未保存の編集があると、ウィンドウタイトルに `*` が付きます。
ウィンドウを閉じるときや別プロジェクトを開くとき、Frame Plan の保存が engine の再起動を求めたとき(✅G-5。§13.5
「forward の既定を同じ Save で入れる」)は、最新の scene revision を確認し、
**Save / Discard / Cancel** を表示します。Save は保存の成功と最新 revision の再確認を待ってから
画面を切り替えます。保存失敗・接続断・編集中で保存できない場合は、現在のプロジェクトを維持します。
Graph タブの変更は編集ごとにファイルへ保存されるため、この確認はシーン編集が対象です。
game の再生のセッションがある間は、この確認の前に、再生を終えるかを問います(✅PM-3b。§13.1「Studio で game を再生・停止する」)。

### Studio で新しい project を作る

**File > New Project...**(Open Project... の 1 つ上。✅F-2)は、フォルダ・名前・雛形を選ぶ dialog を開きます
([dialog](../../src/devstudio/view/newprojectdialog.cpp#L70))。作られるのは「フォルダ/名前」のディレクトリで、
dialog の Creates 欄にその path が出ます。雛形は **Standard 3D scene**(`pelican_cli project init` の既定と同じ)と
**Blank pipeline**(preset も features も持たず、fullscreen pass `present` 1 本が engine の `engine://passthrough` で gradient を描くだけ。shader は書きません。✅VI-2)の 2 つで、
中身は `pelican_cli project init --template standard|blank` と同じ関数が書きます([第10章 §10.4](10_tools.md))。

- **既にファイルがあるフォルダは拒否します。**dialog は閉じず、`initializeProjectTemplate()` の文言
  (`project init target directory is not empty: <path>`)を dialog の下に出し、何も書きません。
  名前が空・`/` や `\` を含む・`.` / `..` の場合も dialog の中で断ります。
- **書いた project は Open Project と同じ経路で開きます**
  ([接続](../../src/devstudio/view/mainwindow.cpp#L1396))。シーンに未保存の編集があれば上の
  Save / Discard / Cancel が出ます。ただし雛形は**確認より前にディスクへ書かれている**ので、Cancel しても
  作ったフォルダは残ります(開かないだけです)。
- **Browse...** は OS のフォルダ選択(modal の `QFileDialog`)です。自動化面からは操作できないので、
  シナリオはフォルダ欄(`pelican.newProject.location`)に直接 type します。
- **フォルダと名前は埋まった状態で開きます(✅T-1)。**フォルダは前回 Create した場所です(Studio の設定ディレクトリの
  `new_project.ini` に残します。初回や、その場所がもう無いときは Documents)。名前はそのフォルダにまだ同名の項目が無い
  最小の `project_NNN`(3 桁、`project_001` から)です([空いている名前を探す所](../../src/devstudio/view/newprojectdialog.cpp#L58))。
  フォルダを変えると名前も付け直しますが、自分で打った名前は変えません。T-1 より前はフォルダが常に Documents、名前が
  `NewProject` でした。

F-2 のときは、project を切り替えた直後の status bar に
`Modal transform polling failed: pelican_player exited before returning an RPC response` が出ることがありました
(gizmo の `get_modal_transform` polling が、切替で終了した古い engine に宛てた要求の失敗を出していたもの。
[文言](../../src/devstudio/model/gizmomodel.cpp#L1062))。**✅F-7 で出なくなりました**: 別の project へ切り替える前に
gizmo の session を終え、返答待ちの要求を忘れます([切替の前処理](../../src/devstudio/view/mainwindow.cpp#L2299))。
今開いている project の開き直しは engine がそのまま残るので、session も続きます。
なお、Studio で作った project でも `project.json` の `generator` は `"pelican_cli project init"` のままです。

### Studioでcomponentを追加する

Inspectorの **Add component...** で登録済みの型を選び、**Add**で追加します。
初期値はエンジンのcomponent codecから取得し、追加後は通常のInspectorで編集できます。
モデル・sprite・procedural geometryは登録assetの一覧から選び、**Use selected assets**で宣言JSONへ反映できます。
procedural geometryは複数選択でき、1件なら`asset`、複数なら`assets`を設定します。
選択はassetの項目だけを更新し、その他の入力は保持します。**Add**を押すまでは草稿です。
ファイルの無いassetも`missing`として表示し、inlineのprocedural assetは`declared`と表示します。
モデルの一覧には、engine が生成する基本形 `engine://primitives/cube` / `plane` / `sphere` が常に先頭に並びます(store `engine`、状態 `builtin`)。asset_data に登録しなくても `simplemodelview.model` にこの参照をそのまま書けます。asset_data で `path` に基本形を指定したモデルも `builtin` です([第 5 章](05_assets.md)、GP-5 / GP-5b)。
登録asset名などを宣言JSONに直接入力する操作も利用できます。
入力内容は通常のcomponent codecで検査します。
追加は既存のjournalに記録され、Undo/Redoで取り消し・再適用できます。ディスクへの保存にはSaveを使います。

**この build で使えない component は灰色で並びます(✅Q-6)。**`get_component_add_candidates` は、build で外した機能の
component を `available: false` と、`unavailable_reason` に code を入れて返します(PG-1)。Studio はそういう行を**使える行の後ろに
灰色で**並べ、名前の後ろに短い語を付けます: module の無い build なら `— not in this build`、
`pelican.ray_instance_requires_layout_v2@1`(project に ray group layout が無い。MG-2a から layout は version 3 だけで、code の名前は変えていません)なら `— needs a ray layout`(説明欄の文は `This project has no ray group layout`。✅Q-7 で「layout が無い」意味に直しました)、Studio の知らない code なら `— unavailable` です。
行は選べますが、選ぶと **Add**・asset の一覧・**Use selected assets** が押せなくなり、宣言 JSON は読み取り専用になります
([Add と宣言欄を止める所](../../src/devstudio/view/componentaddlist.cpp#L41))。説明欄の 1 行目は読める文と code です
(例: `This build has no physics module (physics.module_unavailable@1)`)。code と文・短い語の対応表は Studio に 1 つで
([使えない component の文の表](../../src/devstudio/model/inspectormodel.cpp#L582))、今ある code は `animation` / `audio` / `physics` /
`xr` / `vat` / `seqplayer` の `.module_unavailable@1`、`pelican.ray_instance_requires_layout_v2@1`、`audio.format_unsupported@1` の 8 つです。表に無い code は
code のまま出ます。

- **音声の format の拒否も表に入りました(✅Q-10)。**`audio.format_unsupported@1`(AU-2。音声は WAV と Ogg Vorbis で、拡張子と先頭の
  bytes が食い違うとこの code)の文は `Sound extension and contents disagree (WAV or Ogg Vorbis)`、短い語は `— unsupported sound` です。
  ただし着地時の記録では、**今この code が Inspector や Add component に届く経路はありません**(音声を取る component が無く、
  engine の asset の一覧に音声の種別が無い)。今この文が出るのは **Graph タブの notice** で、graph の編集の preflight の拒否
  (`render_pipeline_preflight_failed`)が message の頭にこの code を持っているときです。表の code を、拒否の code か message の中に
  持っていれば表の文にします([`knownRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L635)。Graph タブは拒否を notice に
  する [`diagnostic()`](../../src/devstudio/model/graphcanvasmodel.cpp#L20) でこれを通します)。

- **理由が code でない行は止めません。**`available: false` でも `unavailable_reason` が文(`simplemodelview` の
  `A model reference is required.` など。既定値だけでは足りず入力が要る、の意味)なら、その行は普通に並び、宣言に値を入れれば
  Add できます。code かどうかは「小文字・数字・`_` の `.` 区切りの名前 + `@` + 数字」の形で見分けます
  ([`isComponentUnavailableCode()`](../../src/devstudio/model/inspectormodel.cpp#L607))。
- ⚠ 止めているのは Add ボタンの有効・無効だけで、Add を押したときの処理は行を見直しません。

⚠ **Inspector にはもう 1 つ編集面があります。**`Add component...` / `Ray geometry...` の下、
properties の一覧より**上**にある **Attached graphs** / **Instance inputs** がそれで、そちらは component ではなく共通グラフの `graph.instance` を
扱い、**journal にも Undo にも載りません**(§13.5「Inspector のグラフ装着」)。

`get_component_add_candidates`は`get_components`と同じobject指定を受け、
`object_id`・`scene_revision`・`candidates[]`を返します。各候補は選択用の`key`、componentの`name`・`available`と、
既定値だけで追加可能なら初期`component` JSON、追加の入力が必要なら`unavailable_reason`を持ちます。
全候補の`component_template`を宣言の初期値にし、`schema_fields`から項目名と型を取得します。
asset選択に対応する型は`asset_selection`に対象`field`、`multiple`、`choices`(id/path/status)を返します。
procedural geometryの`single_field`は1件選択時に使う項目名です。
通常componentの装着済み型は候補に含めません。behaviorは登録済みの各型を **Behavior: 型名** として表示し、
既に装着した型も追加できます。`params`に初期パラメータを指定し、`name`と`type`は選択した型のままにします。
behavior候補の`key`は`behavior:型名`、`behavior_type`は登録名です。`schema_fields`はparamsの項目で、
再帰schemaを持つ型には`schema_node`も返します。候補取得はbehavior instanceを生成しません。

### Studio で object を置く(✅G-2)

Outliner の右クリックと **Scene** toolbar の **Add Object** は同じ 1 つの menu です
([Add Object の menu を作る所](../../src/devstudio/view/mainwindow.cpp#L1784))。項目は **Empty** / **Cube (built-in)** /
**Plane (built-in)** / **Sphere (built-in)** / **Floor** で、自動化面の action 名は `pelican.outliner.addObject.empty` / `cube` /
`plane` / `sphere` / `floor` です。

- **Floor(✅T-1)** は床として置いた Plane です。Plane の基本形を原点の少し下(`pos` が (0, -0.5, 0))に、広く
  (`scale` が (8, 1, 8))置きます([`addObjectDeclaration()`](../../src/devstudio/model/inspectormodel.cpp#L446))。名前は `Floor`、
  `Floor_001`… です。T-1 より前は Plane を置いてから Inspector で位置と大きさを打っていました。

- **送るもの**: scene の `edit` 1 回に `spawn` op 1 つです(上の `addObjectDeclaration()`)。
  transform は原点(Floor だけは上のとおり)で、形なら `simplemodelview.model` に `engine://primitives/<kind>`(Floor は `plane`)、Empty は transform だけです。
  置き先の scene は選択中の object の scene(無ければ engine が最初に言った scene)で、選択中の object の子にはなりません。
- **名前**: 種類の名前(`Empty` / `Cube` / `Plane` / `Sphere` / `Floor`)が空いていればそれ、使われていれば `Cube_001`、`Cube_002`…
  の最小の空き番号です([`uniqueObjectName()`](../../src/devstudio/model/inspectormodel.cpp#L431))。Blender は `Cube.001` と
  書きますが、scene の object 名は `[a-zA-Z0-9_]` だけなので `_` にしています。
- **選択**: Outliner の読み直しを最短の間隔に縮め、committed の応答の `created_objects` が名乗る object が Outliner に
  出た時点でそれを選び、Inspector に出します([`MainWindow::selectCreatedObject()`](../../src/devstudio/view/mainwindow.cpp#L1894))。
  ✅OL-1 から authoring id で選びます(それまでは名前で選び直していました)。
- **Undo**: coalesce key を付けないので、Undo 1 回で Add 1 回が戻ります。
- **Inspector の編集の途中で押した Add Object は待ちます(✅Q-7)**: Inspector の値の編集が進行中(commit の途中を含む)に
  押すと、spawn は 1 本だけ待ち、Inspector の手が空いた時点で、その編集が残した scene revision の上に送られます
  ([`InspectorModel::spawnObject()`](../../src/devstudio/model/inspectormodel.cpp#L3088))。待っている間は status bar(8 秒)と
  Outliner の下の 1 行(`pelican.outlinerNotice`)に `Adding <名前> after the current edit.` が出ます
  ([Add Object の結果を status bar と Outliner に出す所](../../src/devstudio/view/mainwindow.cpp#L1870))。既に 1 本待っているときに
  押した 2 本目は、置き換えずに断ります。同じ 2 か所に
  `Could not add <名前>: <待っている名前> is already waiting to be added; add another once it is in.` が出るので、
  1 本目が入ってからもう一度押してください。Outliner の下の 1 行は、新しい object が選ばれたときに消えます。
  ⚠ 待っている spawn は engine が止まると捨てられ、そのことは言われません(1 行が消えるだけです)。
- Add Object は常に押せます。project が開いていない・engine の準備ができていないときは、status bar に
  `Add Object needs an open project with its engine running.` と出るだけです。
- ⚠ spawn は Studio が最後に見た scene revision で送ります。他の client が編集した直後は `stale_revision` で断られるので、
  もう一度押してください。また、engine が断った spawn の名前も Studio は使用中と数えたままなので、次の Add はその次の
  番号になります。Empty / Plane / Sphere は単体 test だけで、シナリオが通すのは Cube と Floor です。

**Inspector の model 欄は選ぶ欄です(✅G-2)。**`simplemodelview.model` は打つ欄ではなく combo
(`pelican.inspectorModelChoice`)です([model 欄の選択肢の並び](../../src/devstudio/model/inspectormodel.cpp#L544))。
並びは基本形 3 つ(`Cube (built-in)` などの名前で)→ 区切り → `list_assets` が言う project の model(engine の基本形は
二重に出しません)→ 今の値がそのどれでもなければその値、です。選ぶとすぐ commit します(enum の combo と同じ)。
Add component... の asset 一覧も同じ読み方で、基本形は `Cube (built-in)` の名前で先頭に並び、区切りの下に project の
asset が `id [status]` で並びます。

**Inspector の数値欄の名前(✅G-5)**: 自動化面から届くように、数値の欄は 1 つずつ名前を持ちます。vector の列は
`pelican.inspectorVector.<component>.<field>.<列>`(例: `pelican.inspectorVector.transform.scale.2`)、数 1 つは
`pelican.inspectorNumeric.<component>.<field>` です([数値欄の名前の付け方](../../src/devstudio/view/inspectorwidget.cpp#L1950))。
G-5 より前は、どの vector も最初の列が同じ名前でした。シナリオは欄が有効になるのを待って打ち、Return で commit し、
欄がその値を持つまで待ちます([シナリオの README](../../test/studio_scenarios/README.md#L535))。

**回転は角度で入れます(✅T-1)。**`transform.rotation` は X / Y / Z の 3 欄(度、小数 2 桁、自動化面の名前は
`pelican.inspectorEuler.transform.rotation.0` 〜 `.2`)で見せて編集します([回転の欄を作る所](../../src/devstudio/view/inspectorwidget.cpp#L2001))。
回転は X、Y、Z の順に固定軸の回りにかかります(Rz·Ry·Rx)。保存されるのは今までどおり quaternion `[x, y, z, w]` で、
変換は Studio がします([角度と quaternion の変換](../../src/devstudio/model/inspectormodel.cpp#L478))。読むときは X と Z が
(-180, 180]、Y が [-90, 90] で、Y が ±90 のときは X を 0 にして残りを Z に寄せます。欄の右の **Quaternion** の toggle を開くと、
quaternion の 4 欄(G-5 からの `pelican.inspectorVector.transform.rotation.<列>`)が出て、そちらでも編集できます。開閉は欄を
作り直しても保たれます。viewport の gizmo の回転は今までどおりです。

**表示の桁で同じなら、変えていません(✅T-1)。**数値欄は、保存された値を欄の桁で丸めたものと表示が同じなら「値は変わって
いない」と数えます([表示の桁で比べる所](../../src/devstudio/view/inspectorwidget.cpp#L700))。T-1 より前は、focus を置いて離れただけで
丸めた値(`0.38268343` の代わりに `0.3827`)が commit されていました。角度の欄も、同じ回転を表していれば送りません。

**Inspector の欄の編集と commit(✅Q-7)**

- **focus だけでは編集になりません。**編集は、値を打つ(値が変わる)か欄の上で mouse の button を押したときに始まります
  ([focus で編集を始めなくなった所](../../src/devstudio/view/inspectorwidget.cpp#L2219))。欄に focus を置いたままでも
  **Refresh** は押せ、他の欄も止まりません。vector の中で列を移るあいだは 1 つの編集のままです。
- **busy の間に打った値は、手が空いてから送られます。**別の編集の commit の途中(Inspector が busy)は他の欄が止まりますが、
  focus のある欄は止まりません([busy でも focus のある欄は止めない所](../../src/devstudio/view/inspectorwidget.cpp#L1417))。
  そこへ打った値は欄に残り、欄を離れるか Return を押すと、Inspector の手が空いてから 1 回の編集として送られます
  ([手が空いてから値を送る所](../../src/devstudio/view/inspectorwidget.cpp#L668))。その間に欄が消えた(別の object を選んだなど)か、
  値が既に同じなら何も送りません。
- **commit の往復で欄は作り直さず、値だけ差し替えます**([値だけを差し替える所](../../src/devstudio/view/inspectorwidget.cpp#L809))。
  編集中の欄・送り待ちの値を持つ欄・打ちかけの欄(`1.` のように、表示がまだ値と一致しない欄)は上書きしません。作り直すのは
  描くものが変わったとき(別の object、component の追加・削除、欄の種類や範囲が変わるなど)だけです
  ([作り直すかどうかを決める所](../../src/devstudio/view/inspectorwidget.cpp#L755))。
  ⚠ list を持つ component(`pelican_ray_instance` など)は、list の key に scene revision が入っているので、今も commit のたびに
  作り直されます(打ちかけの値はそこでは消えます)。
- G-6 までは、commit のたびに欄を作り直し、focus だけで編集が始まっていました。commit の間に止まった欄から Qt が focus を
  次の欄へ送り、その focus が始めた編集を誰も終わらせないので、Inspector が busy のまま残り(Refresh も他の欄も止まり、
  窓を閉じると未保存の問いが出る)、窓が active のときだけ起きる間欠の故障になっていました。

### Studio の Outliner で複製・削除・付け替える(✅OL-1)

Outliner の行の右クリックの menu の先頭に **Duplicate** と **Delete** があり、その下が Add Object です
(何も選んでいないときは 2 つとも押せません)。送るのは scene の `edit` 1 回に op 1 つで、Undo 1 回で戻ります(§13.3 の
`duplicate` / `destroy` / `reparent`)。object は**名前でも宣言の位置でもなく `authoring_object_id` で**指します
([Outliner の edit の中身](../../src/devstudio/model/outlineredit.cpp#L21))。

- **Duplicate(Ctrl+D)**: 選択中の object を子孫ごと複製します。copy の名前は engine が付けます(`Cube_copy1` など)。
  複製の後は、committed の応答の `created_objects` のうち、`source_object_id` が複製元のもの(copy の根)を選びます
  ([Duplicate を送る所](../../src/devstudio/view/mainwindow.cpp#L1947))。
- **Delete(Del)**: 選択中の object を消します。子孫も一緒に消えます。**子孫があるときだけ** 1 回問います
  (`Delete <名前>? The N objects under it are deleted too.`、既定の button は Cancel。自動化面の名前は
  `pelican.outliner.deleteConfirm` と `.delete` / `.cancel`)。子孫が無ければ問わずに消します(Undo で戻せるので)。
  消した後は親を、親が無ければ前に宣言された兄弟を選びます([Delete の確認](../../src/devstudio/view/mainwindow.cpp#L1961))。
- Ctrl+D と Del は **Outliner に focus があるときだけ**効きます。Inspector の欄で押した Del は文字を消すだけです
  ([Outliner の shortcut](../../src/devstudio/view/outlinertree.cpp#L68))。
- **drag で親を付け替えます**。行を掴んで別の行の上で離すと([落とし先の判定](../../src/devstudio/view/outlinertree.cpp#L100)):
  - **行の中ほど(上下 1/4 を除いた所)**: その行の object の子になります。画面上の位置は変わりません(`preserve: "world"`)。
    **Alt** を押したまま離すと、書かれている transform をそのまま持って新しい親に付くので、親と一緒に動きます(`preserve: "local"`)。
  - **行の上下 1/4(行と行の間)・scene の行の上・最後の行より下**: scene の root に移ります(`new_parent_id: null`)。
  - **自分の上**・今の親の上・(root にいるときの)root には、何も送りません。
  - **自分の子孫の上**は送る前に断ります。drag の間は赤い枠と禁止の cursor が出て、離すと
    `<名前> cannot go under <子孫>: <子孫> is inside <名前>.` が出ます。別の scene の行の上も断ります。
    Esc で drag をやめられます。
  - Qt の drag and drop ではなく Outliner 自身の press / move / release なので、自動化面の `drag` で動かせます。
- **選択と開いた行は authoring id で保ちます。**複製が前に入って宣言の位置がずれても、付け替え・Undo の後も、
  同じ object が選ばれたまま・開いた行は開いたままです。選択は scene_tree を読み直すたびに id で引き直し、その object が
  消えたときだけ外れます([id で選択を引き直す所](../../src/devstudio/model/selection.cpp#L71))。保存した scene を engine が
  読み直したとき(engine の再起動)も、id が同じなので選択は残ります。**別の project を開くと選択は外れます**。
  Undo / Redo で object が戻ってきたとき(Delete の Undo など)は、その応答の `created_objects` の object を選びます
  ([committed の後に選ぶ所](../../src/devstudio/view/mainwindow.cpp#L2056))。
- **断られたとき**は、status bar(8 秒)と Outliner の下の 1 行(`pelican.outlinerNotice`)に 1 文で出ます
  ([断りの文](../../src/devstudio/model/inspectormodel.cpp#L1035))。例: engine が `cycle_detected` で断った付け替えは `Cannot move <名前> under <親>: <親> is inside <名前>.`、
  preview 中は `Cannot duplicate <名前> while a live preview is open; finish the preview first.`、physics の無い build で
  collider を含む object の複製は Q-6 と同じ文で `Cannot duplicate <名前>: This build has no physics module (physics.module_unavailable@1).`、
  もう無い object は `<名前> is no longer in the scene.` です。
- **Inspector の編集が進行中(commit の途中を含む)のときは断ります。**待たせずに
  `Finish the edit in progress first, then <操作> again.` を同じ 2 か所に出します
  ([進行中なら断る所](../../src/devstudio/model/inspectormodel.cpp#L3120))。進行中の編集が同じ object を変えているかも
  しれないためで、1 本だけ待たせる Add Object(✅Q-7)とは扱いが違います。
- rename・複数選択の一括操作・scene をまたぐ移動はありません。

### Studio で旧版の project を開く(✅MG-1)

前の版の engine が書いた文書を持つ project を開くと、Studio は viewport の代わりに**移行の画面**
(`pelican.projectMigration`)を出し、計画を見せてから適用します。移行の仕組み(文書の `(schema, version)`、plan / apply、
退避先、CLI)は [第3章 §3.13](03_project_format.md#313-前の版の-project-を移すmg-1) を見てください。v0 の時点では production に
移行の手順が 1 本も無いので、この画面が出るのは手順が登録されてからです。

- **Studio が気づく経路は 2 つです。**1 つは Studio 自身が project を読むとき(offline open)で、parser が文書を読む前に
  player と同じ検出を走らせ、移行が要る文書があれば読まずに止めます([offline open の検出](../../src/devstudio/model/project.cpp#L108))。
  検出は読むだけで、落ちたプロセスが残した transaction の復旧はしません(復旧は書くので player の側です)。もう 1 つは
  player が stderr に書く `project.migration_required@1: {...}` の行です。移行が要る project では player は engine を起動せず
  窓も出さないので、Studio はこの行を見た時点で窓の探索をやめ、同じ画面に回ります
  ([gate で止まった player を見分ける所](../../src/devstudio/viewport/embeddedviewport.cpp#L379))。どちらのときも status bar に
  `<project> was saved in an earlier format.` が出ます。
- **画面**は、文書ごとに path・schema・形式(`v<from> → v<to>`)と、その下に手順(`step`)と手で直す項目(`fix by hand`)を
  並べ、保存した engine の版と今の engine の版を 1 行で言います。手で直す項目がある文書は警告色で、ボタンの上に
  `N place(s) must be fixed by hand after migrating; the rest is applied.` が出ます。engine は手順で書き換えられる所を適用し、
  残りを報告するだけで止まらないので、**Migrate** は塞ぎません([手で直す項目の注意を出す所](../../src/devstudio/view/projectmigrationwidget.cpp#L353))。
- **Migrate**(`pelican.projectMigration.migrate`)は `apply_project_migration` に plan が返した `expected` をそのまま載せて
  1 回送ります([apply に plan の expected を載せる所](../../src/devstudio/view/projectmigrationwidget.cpp#L283))。**Don't Open**
  (`pelican.projectMigration.close`)は何も変えずに project を閉じ、status bar に
  `The project was not opened; nothing was changed.` が出ます。
- **計画を読んだ後に外で文書が変わっていた**(`project.migration_plan_stale@1`)ときは、何も書かれていません。Studio は計画を
  読み直して見せ直し、`The project changed on disk, so the plan was read again.` と言います。適用は、もう一度 Migrate を
  押すまでしません([stale で計画を読み直す所](../../src/devstudio/view/projectmigrationwidget.cpp#L234))。適用が失敗したとき
  (transaction が巻き戻ったなど)は、計画とボタンが残り、`The migration was not applied: <理由>` が出ます。
- **成功すると**、画面は `Migrated to engine <版>.` と、前の文書を退避した場所の 1 行
  `The previous documents were copied to <path>`(`pelican.projectMigration.backup`。project の下の `.pelican/migration/<時刻>/`)
  だけになって viewport の上に残り、同じ行が Engine Log にも出ます([退避先の 1 行](../../src/devstudio/view/projectmigrationwidget.cpp#L374))。
  続けて Studio は project を開き直し、engine を**再起動**します。再起動は Frame Plan と同じ `MainWindow::requestEngineRestart()` を
  通るので、自動化面の `wait_engine` は新しい engine を待ちます(✅Q-9。[移行の後に engine を起動し直す所](../../src/devstudio/view/mainwindow.cpp#L1640))。
  残った 2 行は別の project を開くと消えます。
- **plan と apply は、engine を起動していない player にだけ送ります**(MG-1b)。apply は最初に transaction の復旧を走らせ、
  別のプロセスが commit している途中の編集を巻き戻しうるためです。同じ project で engine が走っていれば先に止め、起動し直した
  player が移行の入口で止まってから計画を問います([移行の RPC を gate の player にだけ送る所](../../src/devstudio/viewport/embeddedviewport.cpp#L641))。
  Studio の検出が「移行が要る」と言うのに player が普通に開いたとき(Studio と player の build が食い違っている)は、再起動を
  繰り返さずに `The engine opened this project without migrating it; Studio and its player disagree on the project's format.` と
  言います。移行だけを受ける player は窓が無いので、閉じるときは標準入力を閉じて終わらせます。
- 移行の取り消し(退避先からの復元)の UI はありません。
- **分かっている穴**(着地時の記録): 移行の画面を出している間も、Frame Plan・Pass・Graph の panel は前の project の root を
  持ったままです。移行の後の offline open が parse error になると、移行だけの player は開き直すか Don't Open まで残ります。
  player の窓を 15 秒で諦める timeout は gate で止めていますが、その test はありません。

### Studio で game を再生・停止する(✅PM-3a / PM-3b / PM-3c)

engine の play の RPC(§13.5 の「一時停止と 1 歩ずつ進める(✅PM-1a)」「edit 状態(✅PM-1b)」「再生のセッション(✅PM-2)」
「spawn された object を読む(✅RO-1)」。RPC の形、止まるもの・止まらないもの、Stop で戻るもの・戻らないものはそちら)を
Studio のボタンにしたものです。設計は [design_play_mode.md](../design_play_mode.md) §12。PM-3a が Pause / Step と状態の表示を、
PM-3b が edit での起動・Play / Stop・Keep を、PM-3c が Outliner の runtime の行を入れました。

**edit で起動する**(✅PM-3b)

- Studio は engine を `--embedded` と一緒に `--start-state edit` で起動します。project を開いた直後は `Edit` で、behavior は
  眠り、game の時計は止まっています。Play を押すまで game のコードは走りません([`studioPlayerLaunch()`](../../src/devstudio/viewport/studioplayerarguments.cpp#L22))。
- `PELICAN_STUDIO_PLAYER_ARGUMENTS` に `--replay` / `--record-input` / `--bake-camera-output` / `--start-time` があると、engine は
  それを edit と併用できない(`offline.mode_conflict@1`)ので、Studio は `--start-state edit` を付けずに起動し、status bar に
  15 秒 `The engine starts playing, not in edit: <引数> in PELICAN_STUDIO_PLAYER_ARGUMENTS runs the game from its first frame.` を
  出します(名指すのは最初に見つかった引数です。[1 行を出す所](../../src/devstudio/viewport/embeddedviewport.cpp#L710))。
  同じ変数に `--start-state` があれば、それをそのまま使い、何も言いません。

**ボタンとキー**

- Gizmo の toolbar(`pelican.gizmoToolbar`。Move / Rotate / Scale の並び)の Scale の右に、区切りの後に **▶ Play**・
  **❚❚ Pause**・**▶| Step** が並び、その後ろに `View: Game | Editor`(edit と paused のとき)、**Play changes**、`⋯` の
  メニューが続きます([ボタンとキーを作る所](../../src/devstudio/view/playcontrols.cpp#L61)、
  [toolbar に置く所](../../src/devstudio/view/mainwindow.cpp#L1159))。
  - **▶ Play / ■ Stop**: edit では ▶ Play で、`set_play_state {state: "playing"}` を送り、再生のセッションが始まります。
    playing と paused の間は字が ■ Stop に変わり、押すと下の「Stop」です。paused から playing に戻すのは Pause を外す方です
    (このボタンは Stop になっているため)。
  - **❚❚ Pause**: check できるボタンで、押した状態が `paused` です。playing で押すと `{state: "paused"}`、押した状態で
    もう一度押すと `{state: "playing"}` を送ります。これが Resume で、別のボタンはありません。edit では押せません
    (tooltip `Press Play first; the game is not running`)。
  - **▶| Step**: paused では `advance_frames {count: 1}` です。**edit では 2 手**になり、先に `set_play_state {state: "paused"}` を
    送り(セッションが始まり、`onInit` まで走って game のフレーム 0 で止まる)、その返事が `paused` なら続けて
    `advance_frames {count: 1}` を送ります([`PlayStateModel::step()`](../../src/devstudio/model/playstatemodel.cpp#L328))。
    playing では押せません。
  - tooltip は状態で変わります(paused の Pause は `Resume the game (Ctrl+Shift+P)`)。
- **キー**: Ctrl+P が Play / Stop、Ctrl+Shift+P が Pause の切り替え、Ctrl+Alt+P が Step です。テキストを打つ所(読み取り専用で
  ない line edit・text box・spin box と、編集できる combo)に focus があるときと、そのボタンが見えていないか押せないときは
  効きません([`textEntryHasFocus()`](../../src/devstudio/view/playcontrols.cpp#L50))。キーは action の shortcut ではなく窓の
  別の shortcut にしてあるので、ボタンの click は keyboard の focus がどこにあっても効きます。
- **返事を待つ間は押せません**: `set_play_state`・`advance_frames`・`set_view_camera`・`reload_game_logic` の返事と、Stop の前の
  `get_status` の読み(下の「Stop」)を待つ間は、Play / Stop・Pause・Step・View・Reload game logic のどれも押せず、キーも
  効きません([`PlayStateModel::canPlayOrStop()`](../../src/devstudio/model/playstatemodel.hpp#L232))。
- **断られたとき**は、status bar に 8 秒、1 行で出て、`get_status` を 1 回読み直します。play の拒否と、Play や
  Reload game logic が受ける game logic の DLL の拒否は、Studio の拒否の表の文になります
  ([`playRefusalDescription()`](../../src/devstudio/model/playstatemodel.cpp#L184))。例: `playing` の間に届いた `advance_frames` は
  `The game is playing; this works only while it is paused (play_state_mismatch).`、Play の DLL の読み直しの失敗は
  `The game logic DLL did not reload, so the game did not start: <error_fields> (game_logic_reload_failed@1).`
  (`error_fields` は `鍵 値` を `, ` で並べた文)です。表に無い code は engine の message に `(<code>)` を付けた文です。
- **`play_busy`**(Studio の scene の編集か preview がまだ終わっていない)で Play・Stop・Step が断られると、1 行
  `An edit or a preview is still being sent; press again once it has finished (play_busy).` と、status bar の右に
  **Send when done**(`pelican.play.retry`)が出ます。押すと `Waiting to send...` になり、Inspector の scene の編集が終わったかを
  100 ms おきに見て、終わったら同じ操作をもう一度送ります([`PlayStateModel::retryWhenIdle()`](../../src/devstudio/model/playstatemodel.cpp#L368))。

**状態の表示**

- **status bar の右の行**(`pelican.playStatus`)は `Edit` / `Playing · frame N` / `Paused · frame N` / `Stepping (k left) · frame N`
  です([`PlayStateModel::statusText()`](../../src/devstudio/model/playstatemodel.cpp#L614))。N は `get_status.play.game_frame`
  (game のフレーム)で、`get_status.frame`(評価の番号。一時停止中も進む)ではありません。`Stepping` は paused で
  `pending_steps` が 1 以上のときです。
- **枠と帯の色**: playing は青系、paused は黄系、edit は窓の色のままです。塗るのは viewport の枠(`pelican.viewportFrame`)のうち
  viewport の少し上から枠の端までの縁と、Gizmo の toolbar の背景で、viewport の絵そのもの(engine の native の窓)と、枠の上の
  `Viewport` の字の行は塗りません([塗る所](../../src/devstudio/view/mainwindow.cpp#L1296))。色は theme の token で、
  `play.playing_frame` / `play.playing_band` が Dark `#3d8fe0` / `#1f3f60`、Light `#2f7fd0` / `#b9d6f3`、
  `play.paused_frame` / `play.paused_band` が Dark `#e0ac00` / `#5e4b10`、Light `#c99200` / `#f3d98a` です
  ([theme の表](../../src/devstudio/resources/studio_themes.json#L82))。
- **読む時**: Studio には `get_status` を周期で読む時計がありません(Q-2 で止めてあります)。表示は次の読みで動きます
  ([`PlayStateModel`](../../src/devstudio/model/playstatemodel.hpp#L133)):
  - Studio の誰かが送った `get_status` の返事のすべて。engine の準備を確かめる最初の問い合わせも含みます。viewport が
    `engineStatusRead` で配ります([配る所](../../src/devstudio/viewport/embeddedviewport.cpp#L408))
  - 状態を変える返事の後の 1 回と、断られた後の 1 回
  - Step の後: 頼んだ歩が着く(`game_frame` が `until_game_frame` に届き、`pending_steps` が 0)まで、40 ms おきに最大 40 回
  - セッションの間に Studio が scene の編集を commit した後の 1 回(Play changes の行を追うため)
  - ⚠ そのため、**`playing` の間の `frame N` は最後に読んだ値で止まって見えます**。`paused` と `Stepping` の数は正確です。
    下の Outliner の runtime の読みは `get_runtime_objects` なので、この行を動かしません。
- **play を返さない engine**(PM-1a より前)には、ボタンも status bar の行も出ません(Timeline の scrub も今までどおりです)。
- **engine が止まる・再起動する**と、ボタン・行・色・Keep の印をいったん消します。次の engine が答えると、その `get_status` の
  状態で出し直します。Studio が起動した engine なら `Edit` です([`PlayStateModel::disconnect()`](../../src/devstudio/model/playstatemodel.cpp#L236))。
- **Graph タブの Timeline**: `playing` の間は scrub できません。帯の tooltip が
  `The game is playing, so its clock moves on its own. Pause it (Ctrl+Shift+P) to go to a time.` になり、cursor が禁止の形になり、
  drag しても `set_time` を送りません([止める所](../../src/devstudio/model/graphcanvasmodel.cpp#L818))。edit と paused では
  scrub できます。

**再生中の scene の編集と Keep**(✅PM-3b)

再生中も Inspector・Outliner などの scene の編集はでき、engine はそれを記録します(`get_status.play.recorded_edits`)。
記録された編集は Stop で戻ります。残したい編集には Keep の印を付けます。

- **Inspector の行**: セッションの間に commit された編集は、Inspector の編集の行(`pelican.inspectorNotice`)の後ろに
  ` — reverts on Stop` が付き、右に **Keep** のリンク(`pelican.inspectorKeep`)が出ます。押すと Play changes でその編集に
  check が付き、行の後ろは ` — kept after Stop` になってリンクが消えます([行を組む所](../../src/devstudio/view/inspectorwidget.cpp#L1369))。
- **Play changes**(`pelican.play.changes`。記録が 1 件以上あれば `Play changes (N)`)を押すと、小さな窓
  (`pelican.playChanges`)が開きます。modal ではないので、開いている間も game は進み、新しい編集も行に加わります
  ([窓を作る所](../../src/devstudio/view/playcontrols.cpp#L188))。
  - 行は `recorded_edits` の 1 件ずつで、engine の記録の順です。undo で消えた編集は行から消え、redo で戻ります(engine の
    記録に従います)。
  - 行の文: Studio が送った編集はその項目名です。Inspector の欄の編集は `Edit light.intensity` のような名前、Take from game は
    `Take Transform from game` です。undo は `Undo <元の項目名>`、redo は `Redo <元の項目名>` で、commit の結果の `operation` と
    `source_transaction_id` から元の編集を引きます。Studio が送らなかった編集は `Actor N · <transaction_id>`(tooltip
    `An edit another client made (actor N)`)です([`PlayStateModel::changeRows()`](../../src/devstudio/model/playstatemodel.cpp#L432))。
  - 行ごとに check があり、既定は外れています(= 戻す)。**Keep all** / **Keep none** で全部を付け外しします。
  - 上の 1 行(`pelican.playChanges.summary`)は、セッションの間は `N scene edit(s) in this run. Stop puts them back, except the
    ones marked Keep.`、Stop の後は下の Stop の要約です。
- **Take from game**(`pelican.inspectorTakeFromGame`): 再生中(paused を含む)に Inspector で object を選んでいると、Transform の
  欄(生成された Transform でないもの)にボタンが出ます。押すと `get_components` でその object を読み、Transform の
  `runtime_json.local_trs`(ECS の今の値)の `pos` / `rotation` / `scale` を、authoring の Transform に書く `set_component_value`
  3 つの 1 回の `edit` にして送り(`base_revision` はその読みの `scene_revision`)、その編集に Keep の印を付けます
  ([`handleTakeFromGame()`](../../src/devstudio/model/inspectormodel.cpp#L2414))。commit すると行は
  `Take Transform from game committed…` です。v0 は Transform だけです。下の runtime の行を見ている間は出ません。

**Stop**(✅PM-3b)

- check が 1 つも無ければ、`set_play_state {state: "edit"}` を `keep` 無しで送り、記録された編集は全部戻ります。
- check があれば、先に `get_status` を 1 回読み、そのとき `recorded_edits` にある編集のうち check の付いたものだけを `keep` に
  挙げます。読んだ後に undo された編集を挙げると、engine が `play_keep_unknown_transaction` で Stop ごと断るためです
  ([`PlayStateModel::stop()`](../../src/devstudio/model/playstatemodel.cpp#L290))。読んだ時にもう edit だった(別の client が
  止めた)ときは、何も送りません。
- 返事が来ると、status bar に 8 秒 `Stopped: N reverted, M kept` が出ます。`not_kept` があれば `, K could not be kept` が付きます。
  M は `keep` に挙げた数から `not_kept` を引いた数、N は記録された編集の数から M を引いた数です(当て直せなかった編集は
  reverted に数えます)。Play changes の窓は同じ要約と、`not_kept` の 1 件ごとに `Not kept: <行の文> — <理由>`
  (`pelican.playChanges.notKept`)を次の Play まで残します。理由は code を読める文にしたもので、例えば `closure_unresolvable` は
  `it needs an edit that was not kept (the object it changes was made during the run) (closure_unresolvable)`、表に無い code は
  `the engine could not apply it again (<code>)` です([`notKeptReason()`](../../src/devstudio/model/playstatemodel.cpp#L223))。
- セッションが終わると、Outliner と Inspector をすぐ読み直します(Q-16 の外部変更の watch を待ちません。
  [読み直す所](../../src/devstudio/view/mainwindow.cpp#L1344))。
- **未保存の扱い**: Stop は scene revision を戻さず、新しい revision で scene を戻します(§13.5「再生のセッション」)。そのままでは
  Stop のたびに未保存に見えるので、Studio は、Play の前(edit の間)に scene が保存済みで、Stop が 1 件も keep しなかったとき、
  Stop の後の最初の新しい revision を保存済みとして扱います([`InspectorModel::adoptRestoredRevisionAsSaved()`](../../src/devstudio/model/inspectormodel.cpp#L3184))。

**再生中に変わること**(✅PM-3b)

- **戻らない編集の帯**: セッションがある間、render-tools dock の上(Frame Plan・Features・Graph の 3 タブに共通の 1 本)と
  Pass Editor の上に、帯 `Changes here are written to files and stay after Stop.` が出ます。編集を送らなくても、セッションの間は
  出ています。帯の背景は theme の `play.paused_band` です([帯を作る所](../../src/devstudio/view/mainwindow.cpp#L1230))。
  render の編集の Undo(RU-1)は今までどおり効きます。
- **Save Scene が押せません**: File ▸ Save Scene(`pelican.file.saveScene`。Ctrl+S)はセッションの間は無効で、tooltip は
  `Stop the game first: while it runs, the scene is saved only after Stop puts it back (play_session_active).` です。Studio には
  Load Scene と Import snapshot の入口がありません(`src/devstudio` を grep で確かめた)。engine が `play_session_active` で
  断ったときの文は `The game is running, and Stop puts the scene back as it was when Play was pressed; stop it first (<method>) (play_session_active).`
  です。
- **View: Game | Editor**(`pelican.play.viewGame` / `pelican.play.viewEditor`): `get_status.play.view_camera` を返す engine で、
  edit と paused のときに帯に出て、playing では消えます。押すと `set_view_camera {camera}` を送ります。paused の既定は Game で、
  Editor にすると止まった game の世界を editor camera で見られます。Stop すると engine が editor camera に戻します(PM-1b)。
  editor camera の無い engine では
  `This engine has no editor camera; it was started without --start-state edit or --free-camera (view_camera_unavailable).` です。
- **Reload game logic**: 帯の `⋯`(`pelican.play.menuButton`)のメニューにあり(`pelican.play.reloadGameLogic`)、どの状態でも
  押せます(返事待ちの間を除く)。`reload_game_logic` を送り、成功すると edit では
  `Game logic reloaded; the scene waits in edit, its behaviors asleep.`、再生中は
  `Game logic reloaded; the game goes on from the scene as it is now.` が出ます。
- **再起動・切替・閉じる前の確認**: セッションがある間に Restart Engine・別の project を開く・Studio を閉じる・Stop Engine
  (`pelican.viewportStop`)をすると、先にダイアログ `The Game Is Running`(`pelican.playSessionPrompt`)が
  `<操作> ends the game's run. The scene edits made while it ran are lost (Keep marks are not used).` と問います。既定の button は
  Cancel(`.cancel`)です。**End the Run**(`.end`)を押すと、Restart・切替・閉じるでは先に keep なしで Stop を送り、engine が edit に
  戻ってから、いつもの未保存の確認(§13.1「Studio の未保存変更」)に進みます。Stop を送れなかったときは
  `The game could not be stopped now.` を出して、その操作をやめます。Stop Engine は Stop を送らずに engine を止めます
  ([`MainWindow::askToEndPlaySession()`](../../src/devstudio/view/mainwindow.cpp#L1250))。

**Outliner の runtime の行**(✅PM-3c)

- 再生中(paused を含む)は、Studio が `get_runtime_objects {}` を読み、game が spawn した object を Outliner に出します。読む時は、
  playing の間の 1 秒おき(`pelican.outliner.runtimeReadTimer`)と、Studio の誰かが受けた `get_status` の返事で play の状態が
  変わったとき、paused で `game_frame` が進んだとき(Step)です。edit では読まず、行は空です
  ([`RuntimeObjectsModel::observeStatus()`](../../src/devstudio/model/runtimeobjects.cpp#L111))。play の状態は上のボタンと同じ
  `parsePlayStatus()` で読み、ここで読むのは `get_status.scene` だけです。`get_runtime_objects` を知らない engine(RO-1 より前。
  `-32601`)には一度だけ問い、その engine の間は問いません。
- 行は、再生中の scene(`get_status.scene`。まだ名前が無ければ最初の scene)の下の、scene の object の後ろに並びます。字は
  `<name> (runtime)`(`name` は RO-1 のとおり spawn の名前、名前の無い spawn は `spawn:<boundary>.<index>`)で、灰色の斜体です。
  tooltip は `Spawned from the prefab <prefab> while the game runs.` と `Objects spawned while the game runs cannot be edited.` の
  2 行です([行を足す所](../../src/devstudio/view/runtimeoutliner.cpp#L143))。
- Transform の値だけが変わった読みでは、行を作り直しません(開いたメニューや押している行が消えないように)。作り直すのは、
  object の組・名前・prefab・scene が変わったときです。
- 行を選ぶと、Inspector は欄・Add component・ray の一覧・グラフの装着の欄を隠し、枠 `Transform (runtime, read-only)`
  (`pelican.inspectorRuntime`)に `Position` / `Rotation (x, y, z, w)` / `Scale`(`local_trs`)と `World position`(`world_trs.pos`)を
  数だけで出します。選択の行は `<name> (runtime)` と `spawned from <prefab> while the game runs` の 2 行です。Take from game は
  出ません。次の読みで値が変わっていれば、数も追います([読むだけの表示](../../src/devstudio/view/inspectorwidget.cpp#L507))。
- **編集はできません**: runtime の行を選んでいる間は Outliner の Duplicate / Delete が押せず、tooltip が
  `Objects spawned while the game runs cannot be edited.` になります([`OutlinerTree::setEditRefusal()`](../../src/devstudio/view/outlinertree.cpp#L83))。
  runtime の行は掴んで動かせません。その上に object を落とすと断られ、同じ文が status bar(8 秒)と Outliner の下の 1 行に
  出ます。Outliner に rename はもともとありません。
- Stop(と engine の停止・再起動)で行は消え、runtime の行を選んでいたら Inspector は `None` に戻ります。
- **viewport の選択とはつながっていません**。viewport で pick すると runtime の行の選択は外れます。`pick_object` は model
  instance を返し、`get_runtime_objects` の `entity` を返さないためです([pick の所](../../src/devstudio/view/mainwindow.cpp#L2233))。

**分かっている穴**(着地時の記録。実装者の報告)

- engine の窓が keyboard の focus を持っている間は、play のキーが効きません(PM-3a)。
- `playing` の間の `frame N` は止まって見えます(上の「読む時」)。
- gizmo の drag と Graph attachment の編集は、Studio が送った編集でも Play changes では `Actor N · <transaction_id>` と出ます。
- ✅PM-2b(`fc7a0eb1`)の前は、model を描く scene で Keep を付けて Stop すると engine が assert で落ちました
  (slot の表の写しが free list の予約を落としていた)。そのため PM-3b の GPU シナリオは、model の無い
  `play_mode_roundtrip` と、model があり Keep を使わない `play_view_camera` に分けていました。✅PM-3d で、model のある fixture の `play_mode_roundtrip` 1 本にまとめ、Keep 付きの Stop の後も engine が描き続けることを見ています。
- 確認のダイアログの CPU の test は、Studio を閉じる場合だけです。
- セッションの間に自動化面の `quit` を送ると、確認のダイアログで止まります。シナリオは最後に Stop します。

自動化面の名前(コードで確かめたもの):

| 名前 | 指すもの |
|---|---|
| `pelican.play.play` / `pelican.play.pause` / `pelican.play.step` | ▶ Play(■ Stop)/ ❚❚ Pause / ▶\| Step の `QAction`(`list` は `visible` / `enabled` / `checked` を返します) |
| `pelican.play.viewGame` / `pelican.play.viewEditor` | View: Game / Editor の `QAction`(`pelican.play.viewLabel` は `View:` の字) |
| `pelican.play.changes` | Play changes の窓を開く `QAction` |
| `pelican.playChanges` | Play changes の窓。`.summary`(上の 1 行)、`.row.<i>`(上から i 番目の行の check。0 から)、`.keepAll` / `.keepNone`、`.notKept`(Stop が残せなかった行) |
| `pelican.play.menuButton` / `pelican.play.reloadGameLogic` | 帯の `⋯` と、そのメニューの Reload game logic |
| `pelican.play.retry` | `play_busy` の後の Send when done |
| `pelican.playStatus` | status bar の `Edit` / `Playing · frame N` などの行 |
| `pelican.viewportFrame` | viewport の枠。playing / paused の間は縁が theme の色です(シナリオは左端の画素を見ます) |
| `pelican.inspectorKeep` / `pelican.inspectorTakeFromGame` | Inspector の Keep のリンク / Take from game |
| `pelican.play.renderBanner.renderTools` / `pelican.play.renderBanner.passEditor` | 戻らない編集の帯(render-tools dock / Pass Editor) |
| `pelican.playSessionPrompt` | 再生を終える確認。`.end` / `.cancel` |
| `pelican.outliner::row#<name> (runtime)` | Outliner の runtime の行 |
| `pelican.inspectorRuntime` | Inspector の読むだけの枠。`.position` / `.rotation` / `.scale` / `.worldPosition` |

シナリオ `play_pause_step`・`play_mode_roundtrip`・`play_runtime_objects` は §13.5 の「Studio の自動化面」に
書きます。

### 起動のしかた

```sh
# ウィンドウを出しながら外部ツールから編集する(✅WP156)
pelican_player --project mygame --rpc

# ヘッドレスで編集(CI・エージェント向け)
pelican_player --project mygame --headless --rpc
```

**`--rpc` はもう `--headless` を必要としません。** ウィンドウモードでは、リクエストは有界キュー(既定 64 件)に積まれてフレーム境界で処理されます。溢れた場合は `windowed rpc request queue is busy` のアプリケーションエラー(`data.reason = "busy"`)が即返ります。

> ⚠ **`--rpc` を付けると ImGui の開発者 UI は丸ごと無効になります。** 「ウィンドウで GUI を触りながら、同じプロセスに外部エージェントも繋ぐ」ことは現状できません(設計では想定されていますが未実装)。GUI で編集するか、RPC で編集するかのどちらかです。

## 13.2 編集の正本 — AuthoringSceneDocument(✅WP149)

> **設計決定(オーサリングとランタイムの分離):** 編集が書き換えるのは**メモリ上のオーサリング文書**であって、動いている ECS ではない。ECS 側は文書からの**投影**にすぎない。理由: 名前を持たないオブジェクトや、ライトだけ・collider だけのオブジェクトも一意に指せる ID が必要で、かつ「編集 → 失敗 → 完全に元通り」を保証するには、真実の置き場所を 1 つにする必要があるため。

- 文書 1 つ = **シーンファイル 1 本**です。`pelican.scene` v2 のエンベロープ丸ごと —— そのファイルの `scenes` が宣言する全シーン、その中の全オブジェクト、その全コンポーネントの生 JSON —— を保持します([authoringscenedocument.hpp](../../src/core/loader/authoringscenedocument.hpp#L114))。プロジェクトに複数のシーンファイルがあれば文書も複数で、`export_scene_snapshot` はその全部を返します。
- **`AuthoringObjectId`**(セッション内で安定・1 始まり)と **`SceneRevision`**(単調増加)が編集の座標系です。ランタイムの `EntityId` は付随情報で、**同一性の代用にはしません**(オブジェクトが再生成されると変わるため)。
- コンポーネントの受理・正規化は [第4章](04_scene_ecs.md) §4.2 の component codec が担当します。値を書くと**必ず codec の正規形に書き直されて**保存されます。ここでいう**正規形**は、書いた JSON をいったん codec で読み(`decodeAuthored()`)、その値からもう一度書き出した(`encodeCanonical()`)形のことです —— 鍵の並び・省略した既定値・数値の型が一意に決まるので、同じ意味の JSON は必ず同じバイト列になります([componentcodec.hpp](../../src/core/loader/componentcodec.hpp#L339))。

### 保存の往復について(weak round-trip)

保存時に書き出される JSON は、**キーの順序が辞書順に正規化**されます(配列の順序とスカラの型はそのまま)。手で書いた `project.json` 風の並び順は保たれません:

```json
// 入力
{"schema":"pelican.scene","version":2,"scenes":{"main":{"objects":[]}}}
// 保存・スナップショット出力
{"scenes":{"main":{"objects":[]}},"schema":"pelican.scene","version":2}
```

(実物: `test/fixtures/editor_command_service/export_scene_snapshot_v1.json`。ファイル名に `v1` が残っていますが、中身は MG-2b から SnapshotV2 の要求と scene v2 の文書です)

## 13.3 編集の実行モデル — チケットとフレーム境界

編集メソッドは**その場で編集しません**。「受け付けた」という**チケット**(= 受付番号。後から `get_edit_result` で結果を引き取るための ID)を返し、実際の適用は次のフレーム境界の一点で行われます。

```
edit 呼び出し ──▶ 受付(前提条件チェック)──▶ {ticket, status:"accepted"}
                                              │
                        次のフレーム境界 ──────┘
                        (reload 適用の後・入力凍結の前)
                          └─ 前提条件を再検査 → prepare → 全部成功なら publish
                                              │
                    結果は get_edit_result か step_frame の edit_results[] で受け取る
```

> **設計決定(コミット点は 1 フレームに 1 箇所):** 編集の適用点はフレーム境界の一点だけ。理由は決定性 — ゲームシステムの実行中に ECS の構造が変わらないことを保証し、リプレイとゴールデンテストの前提を壊さないため。同じ理由で、**受付時に通った前提条件はコミット時にもう一度すべて検査**されます(その間に他の編集が入るため)。

- **ウィンドウモードの `step_frame` は断られます**(✅PM-1a。JSON-RPC の `-32000`、`data.code` が `step_frame_windowed`)。フレームは engine が自分で描き続けているので、結果は `get_edit_result` / `get_scene_revision` をポーリングして取ります。game を 1 歩ずつ進めたいときは、`set_play_state {"state": "paused"}` の後に `advance_frames {"count": n}` を使います(§13.5 の「一時停止と 1 歩ずつ進める」)。
- ヘッドレスでは `step_frame` を呼んで進め、応答の `edit_results[]` で結果を受け取るのが基本形です。

### 編集操作(operations)の 13 種

`edit` の `operations[]` には次の 13 種を並べます(正は
[`prepareRpcOperation()`](../../src/core/communication/editorjournal.cpp#L1598) の op 分岐)。1 回の `edit` は**全部成功か全部失敗**で、
表に無い `op` は `method_unavailable{method, adapter}` です。

| op | 主なフィールド | 意味 |
|---|---|---|
| `set_component_value` | `object_id`, `component_slot`, `field_path`(JSON pointer), `value` | 値の変更。`/name` への書き込みは拒否 |
| `add_component` | `object_id`, `component`(コンポーネント JSON) | コンポーネント追加 |
| `remove_component` | `object_id`, `component_slot`(behavior は `attachment_index` も必須) | コンポーネント削除 |
| `insert_component_field` | `address`, `value`, `position?`(behavior は `attachment_handle` / `attachment_seq`) | 可変長宣言(`List` / `$list`)に行を追加([insert の受理キー](../../src/core/communication/editorjournalops.cpp#L407)) |
| `remove_component_field` | `address`(同上) | 行、または行の中の 1 フィールドを削除([remove の受理キー](../../src/core/communication/editorjournalops.cpp#L601)) |
| `move_component_field` | `address`(`row_id`), `position`(`{"before":行ID}` か `{"end":true}`) | 行の並べ替え([move の受理キー](../../src/core/communication/editorjournalops.cpp#L568)。§13.12) |
| `spawn` | `object`(+ `scene_id`) | オブジェクト生成。ID は engine が採番 |
| `destroy` | `object_id` | 子孫ごと削除(逆操作用に全 JSON を保存) |
| `reparent` | `object_id`, `new_parent_id`(null = ルート), `preserve`(`"local"` / `"world"`) | 親子の付け替え |
| `set_prefab_parameter` | `object`(**`object_id` ではない**), `parameter`, `value` | prefab インスタンスのパラメータ上書き |
| `unset_prefab_parameter` | `object`, `parameter` | 上書きを外して prefab 側の既定へ戻す(既定が無ければ拒否) |
| `unpack_prefab` | `object`(このほかのキーは拒否) | prefab インスタンスを素のオブジェクトへ展開 |
| `duplicate` | `object_id`, `declaration_index?`(このほかのキーは拒否) | 子孫ごと複製(✅OL-1。下記) |

`get_procedural_assets` / `edit_procedural_asset` は **`edit` の op ではなく独立した RPC メソッド**です
([登録](../../src/core/communication/editorrpchandlers.cpp#L262))。`edit_procedural_asset` は
`base_source_digest` と `asset_name` を取り、編集は `aabb` / `record`、新規は `operation: "create"` と両方の値、
削除は `operation: "delete"` だけです。実行モデルはレンダー設定側(§13.5)と同じ digest CAS です。

**JSON pointer**(RFC 6901)は `/intensity` や `/pos/1` のように `/` 区切りで JSON 内の位置を指す文字列です。配列は 0 始まりの添字で指し、鍵に含まれる `/` と `~` はそれぞれ `~1` / `~0` と書きます。

> ⚠ **`set_component_value` は「既にある値を置き換える」だけ**です。`field_path` は
> **今の component JSON に実在する pointer** を指す必要があり、**行(配列要素)を新しく作ることはできません**。
> 可変長の宣言(`List` / `$list`。[第11章](11_status.md) §11.1 の WP382a)を編集する
> 行の追加・削除・並べ替えは **WP382b で入る予定で、いまは未対応**です。

実物(`test/editorjournal_test.cpp` より):

```json
{"op":"set_component_value","object_id":1,"component_slot":"light","field_path":"/intensity","value":2.0}
{"op":"add_component","object_id":1,"component":{"name":"collider","shape":"box","half_extents":[1,1,1]}}
{"op":"spawn","scene_id":"main","object":{"name":"Spawned","components":[
   {"name":"transform","pos":[0,0,0],"rotation":[0,0,0,1],"scale":[1,1,1]},
   {"name":"behavior","type":"fixture_behavior"}]}}
{"op":"reparent","object_id":3,"new_parent_id":1,"preserve":"local"}
```

- `behavior` を対象にするときは `component_slot: "behavior"` + `attachment_index` を指定し、`field_path` は `/params/...` に限られます([第8章](08_gameplay.md) §8.5)。
- 数値は **2^53−1 以下の非負整数**が上限です(JSON の安全整数域)。
- `spawn` した ID は undo → redo で**同じ ID に戻ります**(生成物の同一性が保たれる)。

#### `duplicate` — 子孫ごと複製する(✅OL-1)

`{"op":"duplicate","object_id":2}` は、object 2 とその子孫を 1 つのかたまりとして複製します([`prepareDuplicate()`](../../src/core/communication/editorjournalops.cpp#L1296))。

- **置き場所**: 既定では元の subtree の直後に、元の宣言順のまま並びます。`declaration_index` を書くとそこに置きます。シーンの object 数を超える値は `schema_violation` です。
- **名前**: 各 copy は `<名>_copy<N>` になります。`N` はそのシーンでまだ使われていない最小の番号で、object ごとに決まります(`Leaf_copy1` が既にあれば `Middle_copy1` と `Leaf_copy2`)。copy をもう一度複製すると `Middle_copy1_copy1` です([`copyName()`](../../src/core/communication/editorjournalops.cpp#L1279))。
- **親**: subtree の中の `parent` は、付け替えた名前に向け直します。いちばん上の copy は元と同じ親の下に入ります。
- **ID**: copy は新しい `AuthoringObjectId` を取ります。undo は 1 手で、copy をすべて消します。redo は同じ ID で戻します。
- **prefab instance**: instance ごと複製し、copy の `prefab.instance_id` は engine が新しく付けます(`<元の instance_id>_copy<N>`)。
- **feature 検査**: copy はそれぞれ `add_component` / `spawn` と同じ PG-1 の検査を通ります。たとえば physics を外した build で collider を持つ object を複製すると `physics.module_unavailable@1` です。
- **共通グラフの装着**: rendering config の `graph.instance.owner` は元の object の ID を指したままで、copy には付きません(実装のコメントより)。
- **プレビューは非対応**です。プレビューの投影計画は `duplicate` を `editor_preview_unsupported@1` で断ります([分岐](../../src/core/communication/editorprojectionplan.cpp#L850)。コードで確認。プレビュー経由のテストはありません)。
- 知らないキー(たとえば `name`)を書いた要求は、`spawn` / `reparent` と同じく invalid params です。存在しない `object_id` は `closure_unresolvable` です。

応答(`get_edit_result`。`test/editorjournal_test.cpp` の OL-1 の例。Middle(2)の子が Leaf(3)):

```jsonc
{"ticket":"edit-1","actor_id":1,"base_revision":1,"committed_revision":2,
 "transaction_id":1,"operation":"edit","status":"committed",
 "affected_authoring_ids":[5,6],
 "created_objects":[
   {"object_id":5,"scene_id":"main","declaration_index":3,"name":"Middle_copy1","source_object_id":2},
   {"object_id":6,"scene_id":"main","declaration_index":4,"name":"Leaf_copy1","source_object_id":3}]}
```

(`ticket` / `base_revision` / `committed_revision` / `transaction_id` の値は説明用です。鍵と並びは [実装](../../src/core/communication/editorjournal.cpp#L2603) のとおり。)

#### committed の応答に付く 2 つの鍵(✅OL-1)

`edit` / `undo` / `redo` の committed の応答には、どの op でも次の 2 つが付きます([`appendCommittedIdentities()`](../../src/core/communication/editorjournal.cpp#L2603))。

- `affected_authoring_ids` — その編集が触った object の `AuthoringObjectId` の配列。
- `created_objects` — その編集で**新しく現れた** object の配列。1 要素は `{object_id, scene_id, declaration_index, name}` で、`duplicate` の copy には `source_object_id` も付きます。対象は `spawn`、`duplicate`、そして undo が戻した object(`destroy` の undo)です。何も作らない編集では空配列です。`duplicate` の undo も空で、redo は最初の応答と同じ列を返します。

新しくできた object を選ぶのに、`scene_tree` を探し直す必要はありません。

## 13.4 同時編集 — actor と revision

編集する前に**セッションを開いて `actor_id` をもらいます**。undo/redo はこの actor ごとのスタックです。

```jsonc
→ {"method":"open_editor_session","params":{"display_name":"my tool"}}
← {"actor_id":1,"display_name":"my tool","reconnect_token":"..."}
```

- 接続が切れても `resume_editor_session` に `reconnect_token` を渡せば同じ actor を引き継げます(1 接続に紐づく actor は 1 個で、後勝ちです)。
- 編集メソッドには必ず `base_revision`(見ている文書のリビジョン)を添えます。**文書全体で 1 つの `SceneRevision` を使った CAS(Compare-And-Swap = 「自分が見ていた版と今の版が一致するときだけ書き換える」方式)**で、古い前提のまま上書きしてしまうのを防ぎます。判定の単位が文書全体なので、他の誰かが**無関係なオブジェクト**を編集しただけでも `stale_revision` で弾かれます。これは v1 の意図的な制約です — 最新の revision を取り直し、**操作内容も作り直してから**再送してください(revision だけ差し替えて再送するのは誤りです)。
- 現在の revision は `get_scene_revision` で取れます(`preview_epoch` / 直近トランザクション / プレビューリースも一緒に返ります)。変化を検出する目的にはこれをポーリングします。

## 13.5 編集 RPC リファレンス

ここに挙げるのはエディタトラックで増えた分です。基盤メソッドは [第10章](10_tools.md) §10.3。**総数は挙げません** —— メソッドは増え続けるので、数字を書くと必ず腐ります。

### 読み取り

| メソッド | params | 返すもの |
|---|---|---|
| `scene_tree` | `{scene_id?}` | シーンのオブジェクト一覧(`authoring_object_id` / `declaration_index` / 親子 / コンポーネント込み) |
| `get_components` | `{authoring_object_id}` または `{name}` | オブジェクト 1 件の詳細(`declaration_index` 込み。**両方指定・両方省略はエラー**) |
| `get_scene_revision` | `{}` | `{scene_revision, preview_epoch, last_transaction, preview_lease}` |
| `list_assets` | `{store?}` | アセット一覧(id / 種別 / パス / store / 状態)。状態は `loaded`・`missing`・`declared`(inline の procedural)・`builtin`(engine の基本形、store `engine`) |
| `query_journal` | `{after_transaction_id?}` | 編集履歴(undo の種) |
| `can_edit` / `can_preview` | `{}` | 編集・プレビューの可否と理由 |
| `get_graph_instances` | `{}` | 共通グラフ文書の `source_reference` / `source_digest` / `instances[]`(`graph.instance` だけ)/ root の `nodes[]` 全体 / `definitions` 本体([get_graph_instances の実装](../../src/core/communication/renderconfigeditor.cpp#L2356)) |
| `get_graph_catalog` | `{}` | 登録済みグラフモジュールのカタログ(`pelican.graph_catalog` v1。`pelican_cli graph-catalog` と同じ値 — [第10章](10_tools.md) §10.4) |
| `get_graph_values` | `{instance}` | その instance の入力 port の実効値 `{port, value, source, connected\|parameter}` と `published` / `generation` / `frame`。**publish 時の snapshot を読むだけで再評価しません** |
| `get_graph_node_values` | `{instance?}` | 展開木の全ノードの出力値(`{node, expanded, outputs}`)と `source:"rendering"`。`instance` 省略で root から |

`declaration_index` は scene の `objects` 配列における 0 始まりの宣言位置です。直リンクで読んだ木との対応には `(scene_id, declaration_index)` を使います。`authoring_object_id` はセッション内で安定した別の参照なので、挿入・並べ替え後は両者が一致するとは限らず、どちらも応答に残ります。

`get_components` が返すコンポーネントには、**`schema`(フィールド名・型・範囲・単位)と `editable` フラグ**が含まれます。インスペクタのウィジェットはこれだけで組み立てられます(§13.9)。

ここでいう**解決**は、オーサリング文書の生 JSON を codec に通し、既定値を補ってランタイムの値にするところまでを指します。範囲外の数値・未知の鍵・型違いなど、利用者が書いた内容が契約に反していれば解決は失敗します。

**解決に失敗したコンポーネントも消えません(✅WP378)。** authored / generated のどちらの直下にも
`resolution_error` が付き、`get_components` は**成功応答のまま**それを返します
([editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp#L1680))。形は 2 通りで、
typed な違反は `{code, path(RFC6901), fields, message}`、まだ typed 化していない旧分岐は
`{legacy_branch_id, message}` です(正は
[`resolutionFailureJson()`](../../src/core/loader/resolutionresult.hpp#L114))。
**`resolution_error` を含む応答はキー順が保存されます** —— editor 側のハンドラが「応答木のどこかに
`resolution_error` があるか」を見る述語 `embedsResolutionError` を登録時に添え
([editorrpchandlers.cpp](../../src/core/communication/editorrpchandlers.cpp#L50))、`RpcServer` はその述語が真を返した応答だけを
ordered JSON で直列化するためです([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1200))。

### セッションと編集

| メソッド | params |
|---|---|
| `open_editor_session` / `resume_editor_session` | `{display_name?}` / `{reconnect_token}` |
| `edit` | `{actor_id, base_revision, operations[], coalesce_key?}` |
| `undo` / `redo` | `{actor_id, base_revision, transaction_id?}` |
| `get_edit_result` | `{ticket}` |

### プレビュー

| メソッド | params |
|---|---|
| `open_preview` / `update_preview` / `commit_preview` / `abort_preview` | `{actor_id, ...}`(§13.7) |
| `get_preview_result` | `{request_id}` |
| `eval_preview` | `{overrides?, queries?}` |
| `render_preview` | `{overrides?, capture{...}}` |

### スナップショットと保存

| メソッド | params |
|---|---|
| `export_scene_snapshot` | `{schema_version:2, allow_pending?}` |
| `import_scene_snapshot` | `{schema_version:2, semantic_scene_bytes, digest, current_scene_id, prefab_closure}` |
| `save_scene` | `{}`(フィールドを書くとエラー) |

### 一時停止と 1 歩ずつ進める(✅PM-1a)

設計は [design_play_mode.md](../design_play_mode.md)。この小見出しは `playing` と `paused` の間の話です。3 つ目の状態 `edit` は下の「edit 状態(✅PM-1b)」に書きます。`--start-state` を付けずに起動した engine は `playing` です。

| メソッド | params | 返すもの | 拒否 |
|---|---|---|---|
| `set_play_state` | `{state: "edit" \| "playing" \| "paused", keep?}`(`keep` は ✅PM-2。他の鍵は書けない) | `{state, previous}`。同じ状態への指示は何も変えずに成功 | それ以外の値や鍵は `-32602`。`"edit"` との間の移り方は下の「edit 状態」、`keep` と拒否は「再生のセッション」 |
| `advance_frames` | `{count}`(1〜600 の整数。鍵は `count` だけ) | 窓あり: すぐに `{scheduled, until_game_frame}` / headless: 歩き終えてから `{scheduled, until_game_frame, frame, t, edit_results}` | `paused` 以外では `-32000` の `play_state_mismatch`(`data` に `state` と `required: "paused"`)/ 範囲外の `count` は `-32602`(`data.code` 無し) |
| `step_frame` | `{}` | headless だけ。描画のフレームを 1 つ進める。game が進むのは `playing` のときだけ | 窓ありでは `-32000` の `step_frame_windowed`(message が `advance_frames` を示す) |

- **2 つの番号**: `get_status` の `frame`(と `step_frame` / `advance_frames` の `frame`)は**評価の番号**で、描画のフレームごとに必ず 1 増え、一時停止中も進みます。game が何歩進んだかは `get_status` の `play.game_frame` で読みます。`get_status` の `time`(と `t`)は game の時計で、一時停止中は止まります
- **`get_status.play`** は `{state, game_frame, game_time, pending_steps, view_camera, session, recorded_edits}` です(`view_camera` は ✅PM-1b。下の「edit 状態」。`session` と `recorded_edits` は ✅PM-2。下の「再生のセッション」)。`pending_steps` は `advance_frames` で頼んでまだ取っていない歩数です。一時停止しているかは `play.state` だけで示し、`get_status.audio` の形は変わりません
- **窓あり**の `advance_frames` は、次のフレームから 1 フレームに 1 歩ずつ進めます(入れ子のフレームは走らせません)。`until_game_frame` は歩き終えたときの `play.game_frame` なので、`get_status` をポーリングして `play.game_frame` がそこに着き `pending_steps` が 0 になるのを待ちます。**headless** は応答の前に `count` 回、描画のフレームを回します
- 1 歩の `dt` は `1/fps`(既定 60 fps)です。進み終わっても `paused` のままです
- 一時停止中に止まるもの: game の event の凍結と配送、`ctx.spawn` / `destroy` の着地、game の system(behavior、DLL の system、physics trigger)、game DLL が登録した animation の phase、`ctx.loadScene` の適用、game の UI の event(ポインタは追うが event を積まない)、音(全体をまとめて一時停止。offline の backend は無音を返し再生位置を進めない)。止まらないもの: 描画、入力の取り込み、RPC、編集の反映、engine 側の animation・seq・graph(止まった時刻で毎フレーム評価し直す)、組み込みの camera controller と modal transform(そのフレーム自身の `dt` で動く)
- `playing` に戻すと、最初のフレームの `dt` に一時停止していた時間は入りません。頼んでまだ取っていない歩は捨てます

### edit 状態(✅PM-1b)

`edit` は、scene を読み込んで描き、編集を受け付けるが、**game のコードを走らせない**状態です(設計は [design_play_mode.md](../design_play_mode.md) §2・§4.4・§5)。engine を `edit` で起こすのは `pelican_player --rpc --start-state edit` です(引数は [第10章](10_tools.md) §10.2。`--replay` / `--record-input` / `--bake-camera-output` / `--start-time` / `--rpc` の無い `--headless` とは併用できません)。✅PM-3b から、Studio は engine を `--start-state edit` で起動します(§13.1「Studio で game を再生・停止する」)。

| メソッド | params | 返すもの | 拒否 |
|---|---|---|---|
| `set_play_state` | `{state: "edit" \| "playing" \| "paused", keep?}` | `{state, previous}`(`keep` を書いた Stop は下の「再生のセッション」) | 上の表と同じ。`keep` と PM-2 の拒否は下の「再生のセッション(✅PM-2)」 |
| `set_view_camera` | `{camera: "editor" \| "game"}`(鍵は `camera` だけ) | `{camera, previous}`。同じ view への指示は何も変えずに成功 | `playing` では `-32000` の `play_state_mismatch`(`data` に `state` と `required: ["edit", "paused"]`)/ editor camera の無い起動で `"editor"` は `-32000` の `view_camera_unavailable`(`data` に `camera`)/ それ以外の値や鍵は `-32602`([`set_view_camera` の handler](../../src/core/communication/rpcserver.cpp#L2230)) |

**`edit` で何が効くか。**

- **behavior は眠っています。** 読み込むと attachment はでき(handle・type・params を持つ。`pending` ではありません)、instance は作られません。constructor・`onInit`・`onUpdate`・event・spawn はどれも走りません([`BehaviorAttachmentArena::setDormant()`](../../src/core/gamelogic/behaviorarena.hpp#L268))。game の system・game DLL の animation の phase・game の event の配送も止まり、game の UI は入力を受けません
- **game の時計は止まっています**(`dt = 0`)。`set_time` で動かせ、animation・seq・graph はその時刻で毎フレーム評価し直されます。`advance_frames` は `paused` 専用なので `edit` では `play_state_mismatch` です
- **behavior の編集は効きます。** params の変更は保存した params を替えるだけ、attach は instance を作らない眠った attachment を足すだけで、undo / redo もそのまま通ります。どれも constructor を呼びません
- **`load_scene` と game logic の reload(F5 / `reload_game_logic`)** は、眠ったまま runtime を作り直します。`edit` で作った runtime の `scene_start_time` は 0 なので、`scene_time` は Timeline の時刻と一致します

**`edit` から出る(Play)。**`set_play_state {state: "playing"}` か `"paused"` を送ると、まだ配っていない game の event を捨て、そのときの時刻を覚えて game の時計と `game_frame` を 0 に戻し、behavior を起こして、今の scene を `load_scene` と同じ経路で作り直します。ここで constructor と `onInit` が走ります。`"paused"` なら `onInit` まで走って game のフレーム 0 で止まり、`advance_frames` で 1 歩ずつ進められます。

**`edit` へ戻る(Stop)。**`playing` / `paused` から `set_play_state {state: "edit"}` を送ると、game の event を捨て、behavior を眠らせ、game の時計を Play の前の時刻(Play を経ずに起動した engine なら 0)に戻し、runtime を作り直します。behavior には `onDestroy` が走り、spawn した object は消えます。

- **✅PM-2 から、Stop で authoring は再生の前に戻ります。** 再生中に Inspector などで変えた scene の編集は、`keep` で残すと言わない限り消えます。何が戻り何が戻らないかは下の「再生のセッション」です。セッションの無い Stop(`--start-state` を省いて起動した engine の最初の Stop など)だけは、PM-1b のとおり今の authoring から作り直します
- Stop しても `play.game_frame` は 0 に戻りません。前のセッションの最後の値のまま残り、次の Play で 0 になります
- 作り直しが失敗したときは、状態・時計・behavior の眠りを元に戻して error を返します
- PM-1b の時点で残っていた穴(`edit` 中の `start_input_replay` / `start_input_record`)は、✅PM-2 で `offline.mode_conflict@1` で断るようになりました(下の「再生のセッション」)

**見る camera。**`--start-state edit` の engine は editor camera を持ちます(`--free-camera` を付けていなければ、既定の `blender` の free camera とその入力 overlay が足されます。`--editor-transform` は足されません)。`get_status.play.view_camera` が今の view(`"editor"` / `"game"`)です。

- `--start-state edit` で起動した engine では、状態が移るたびに view が切り替わります(`edit` → editor camera、`playing` / `paused` → game の camera)。`playing` の間は editor の入力 overlay(`free_camera` / `editor_transform`)を action の stack から外し、editor の system も走らせないので、同じ key が game に届きます。`edit` / `paused` に戻ると overlay は積み直されます
- `edit` と `paused` では `set_view_camera` で見る camera を替えられます。見ていない方の camera は位置と向きを覚えていて、戻すとそのまま出てきます
- editor camera を見ている間に game のコードの `ctx.render().setCamera(name)` や RPC の `set_camera` が scene の camera を選ぶと、先に game の view へ切り替わってから選ばれます。editor camera の名前を指すと editor の view になります
- `--start-state` を付けない起動では、`set_play_state` は view も overlay も変えません(`--free-camera` の engine なら `set_view_camera` は使えます)。既定の起動でも `edit` へ移ることはできます

### 再生のセッション(✅PM-2)

`edit` から `playing` / `paused` へ移ると**再生のセッション**が始まり、`edit` へ戻るとセッションが終わって scene が再生の前に戻ります(設計は [design_play_mode.md](../design_play_mode.md) §6・§7。実体は [`set_play_state` の handler](../../src/core/communication/rpcserver.cpp#L2035) と [`PlaySession`](../../src/core/communication/playsession.hpp#L51))。Studio の Play / Stop と Keep は ✅PM-3b です(§13.1「Studio で game を再生・停止する」)。

**Play でセッションが始まるとき。**

1. scene の編集の ticket が受け付けたまま commit されていない、または live preview が開いていると、`play_busy` で断ります(何も変わりません)
2. game logic の DLL を読んでいれば、**新しい写しを読み直します**。game の system の static な状態は、Play のたびに初めからです(第 8 章 §8.4 の「再生のセッションとゲームコード」)。`--replay` と golden の起動では読み直しません(その run の決定性を壊すため)。読み直しに失敗すると、その reload のエラーの code で断り、セッションは始まりません
3. authoring(全 scene と各 object の `authoring_object_id`)、今の scene、editor の履歴(journal・誰が最後に書いたか・actor ごとの undo / redo の stack)を覚えます。scene を読んでいない engine はセッションを作らず、PM-1b と同じく作り直すだけです
4. そのあとは上の「`edit` から出る(Play)」のとおりです

**再生中。**Inspector などの編集は今までどおり authoring と runtime の両方に効き、履歴にも積まれます。セッションはこの間に commit された編集を記録し、`get_status.play` に出します。

- `session`: セッションがあるか(bool)
- `recorded_edits`: 記録した編集の `[{transaction_id, actor}]`(並びは journal の順。セッションが無ければ `[]`)。undo / redo が記録の中の編集を戻すと、その行が消えます。記録に無い編集(セッションの前の編集など)の undo / redo は、それ自身が行になります。undo で消した行を redo すると、行は **redo の transaction_id** で戻ります
- `save_scene` / `load_scene` / `import_scene_snapshot` は `-32000` の `play_session_active`(`data` は `{code, method, state}`)で断られます。**セッションがあるときだけ**で、`--start-state` を省いて起動した engine の最初の `playing` のようにセッションが無ければ断りません。ImGui の inspector の Save ボタンは RPC を通らないので、この拒否の対象ではありません
- game のコードが `ctx.loadScene` で scene を替えてもかまいません。Stop ではセッションを始めた scene に戻ります

**Stop でセッションが終わるとき。**

| 戻るもの | 戻らないもの |
|---|---|
| scene の authoring の中身と `authoring_object_id`(全 scene) | ファイルへ直接書く編集: render の構成(Frame Plan / Pass Editor の編集、rendering 節、preset、graph 文書、authored pass)、material・shader・code・asset |
| 今の scene(`ctx.loadScene` で替えていても、始めた scene) | game が書いた Persistence(`saveData`) |
| editor の履歴(journal・誰が最後に書いたか・undo / redo の stack)。再生の前の編集の undo は Stop の後もそのまま効きます | 番号: `scene_revision` は戻らず、今の値より大きい新しい revision になります。次に振る `authoring_object_id` は、覚えた値と今の値の大きい方です。ticket・transaction の番号も戻りません |
| game の時計(Play の前の時刻) | |

- 設計(§6.4)は、再生中の `runtime_only_changes`(`update_transforms` / `load_gltf`)も runtime の作り直しで消えるとしています。test では確かめていません(未確認)
- 音はすべて止まります(`Audio::stopAll`)
- spawn した object は消え、behavior には `onDestroy` が走ります。runtime は眠った `edit` の runtime として作り直されます
- 戻したあとは editor の gate の epoch が進むので、Stop の前に受け付けた編集は commit できません
- Stop のときも、commit されていない ticket か開いた live preview があると `play_busy` です

**残す(`keep`)。**`set_play_state {state: "edit", keep: "all" | ["<transaction_id>", …]}` で、記録した編集のうち挙げたものを、戻した authoring の上に記録の順で当て直します。

- 当て直せた編集は履歴の **1 項目**(`operation_kind` が `play_keep`)にまとまり、当て直せた最後の編集をした actor の undo stack に載ります。その actor の undo 1 回で全部が外れます
- 再生中に作った object は、作ったときと同じ `authoring_object_id` で残ります
- 返り値は `{state, previous, not_kept, kept_transaction_id?}` です。`not_kept` は `keep` を書けば必ず付き、当て直せなかった編集を `[{transaction_id, code}]` で挙げます(`code` は当て直しを止めた editor のエラーの名前。例: 作る編集を残さずに、その object を触る編集だけを残すと `closure_unresolvable`)。当て直せないものがあっても Stop は成功します。`kept_transaction_id` は 1 つでも当て直せたときだけ付く、上の 1 項目の transaction_id です
- 記録に無い transaction_id を挙げると、`-32000` の `play_keep_unknown_transaction`(`data` は `{code, transaction_id, session, recorded_edits}`)で Stop ごと断られ、何も変わりません
- `keep` は `"all"` か transaction_id の配列だけで、`edit` 以外への移りに書くと `-32602` です
- game が runtime で動かした値を残したいときは、再生中にその値を読んで書く編集を送り、それを `keep` に挙げます(engine に専用の口はありません)

**`--start-state` を省いて起動した engine。**最初の `playing` はセッションを経ていないので、最初の Stop は何も戻さず、今の authoring から `edit` の runtime を作り直すだけです。このとき `keep: "all"` は `not_kept: []` を返し、transaction_id を挙げると `play_keep_unknown_transaction`(`data.session` が `false`)です。そこから先の Play と Stop は、ふつうのセッションになります。

**その他の拒否。**

- `play_busy` の `data` は `{code, state, requested, pending_ticket_ids, open_preview_lease}` です(`state` は今の状態、`requested` は頼んだ状態)
- `--start-state edit` で起動した engine では、`start_input_record` / `start_input_replay` がどの状態でも `-32000` の `offline.mode_conflict@1`(`data` は `{code, method, start_state: "edit"}`)で断られます。コマンドラインの `--record-input` / `--replay` と同じ扱いです

### spawn された object を読む(✅RO-1)

`get_runtime_objects {}` は、今の scene で game が spawn した生きている object を、spawn された順(boundary、その中の index の順)に返します([handler](../../src/core/communication/rpcserver.cpp#L1392))。authoring の object は `scene_tree` / `get_components` で読み、spawn された object はこちらで読みます。field を足すと `-32602` です。

```json
{"objects": [{"name": "spawn:2.0", "entity": {"index": 7, "generation": 1},
              "spawned_by": {"prefab": "probe"},
              "transform": {"local_trs": {...}, "world_trs": {...}}}]}
```

- `name` は名前付きの spawn ならその名前、名前の無い spawn は `spawn:<boundary>.<index>` です。
- `entity` は `pick_object` や gizmo の `runtime` 選択と同じ形です。
- `parent` は、親が authoring の object なら `{authoring_object_id, scene_id}`、spawn された object なら `{entity: {index, generation}}` です。今の spawn は親を持てない(spawn の準備が断る)ので、実際には省かれます。
- `spawned_by` は spawn が指定した prefab 名(`{prefab}`)だけです。spawn を起こした behavior は記録していません。
- `transform` は `get_components` の Transform の `runtime_json` と同じ形です。Transform の無い object では省きます。
- `edit` 状態と、再生のセッションを終えた後は `[]` です。
- Studio は ✅PM-3c から、これを Outliner の `(runtime)` の行に出します(§13.1「Studio で game を再生・停止する」)。

### shader を読み直させる(✅F-5)

| メソッド | params | 返すもの | 拒否(`error.data` の `{error_code, error_fields}`) |
|---|---|---|---|
| `reload_shaders` | `{}`(鍵を書くと invalid params) | 読み直した: `{committed: true, changed: [shader の参照…], cache_hits, cache_misses, refused_passes: [...]}`(`refused_passes` は ✅VI-1b。新しい shader を断った pass が無ければ `[]`。下の「pass が新しい shader を断る」)/ 何も変わっていない: `{committed: false, attempted: false}` | `shader_reload_failed@1`(compile 失敗。`error_fields.changed[]`、message は compile log で、`get_status` の `reload.runtime["pelican.shaders"].last_error` にも同じ文が残る。compile 失敗の理由が `PELICAN_DECLARE_VARIADIC_INPUTS` の 2 回使用なら `error_fields.shader_input_mismatch` も付く。✅VI-1b)/ `shader_reload_unavailable@1`(`error_fields.reason` が `replay` か `strict assets`。reload service の無い engine も同じ code) |

**file watch の gate は開けません。**`--rpc` で動く engine(Studio が起こす engine はすべてそう)は、reload gate が
`rpc driver` を理由に閉じていて、shader file を保存しても自分では読み直しません。`reload_shaders` はそれを開けずに、
**頼まれたその 1 回だけ** shader の runtime participant を当てます(`reload_game_logic` と同じ形。
[handler](../../src/core/communication/rpcserver.cpp#L1392))。閉じた gate を越えるのは明示の要求だけで、
replay と `--strict-assets` の engine は今までどおり断ります([越えてよいかの判定](../../src/core/watch/reloadservice.cpp#L692) /
[`ReloadGate::explicitShaderReloadRefusal()`](../../src/core/watch/reloadgate.cpp#L45))。file watch の poll の経路は `--rpc` の下で
今までどおり何もしません。

- **いつ効くか**: RPC は frame 境界で処理されるので、handler の中で即時に準備・commit し、新しい shader は次の frame の
  `render_start` で使われます(fullscreen pass の入力もそこで bind し直します)。preview 中なら先に preview を中止します。
- **何を読み直すか**: SPIR-V の sha256 が公開中と違う unit だけです([`ShaderLibrary::prepareReloadChanged()`](../../src/core/shader/shaderlibrary.cpp#L1216))。
  ただし前回の読み直しが失敗した unit は、中身が変わっていなくても 1 回公開し直します(それで失敗の記録が消えます)。
  判定のために compile はするので、呼ぶたびに全 unit を compile し直します(cache が効くので軽い。大きな project での時間は未測定)。
- Studio は shader file の保存を見て自分でこれを呼びます(§13.5「Studio のノードキャンバス」の「shader を外のエディタで保存すると画面が変わる」)。

**pass が新しい shader を断ることがあります(✅VI-1b)。**読み直しは、fullscreen pass と入力を持つ raster pass の
新しい pipeline ごとに、その pass が渡している入力を新しい shader の set 1 の宣言と照合します。規則は編集と load の
照合と同じ関数です(Q-14e / Q-14f / VI-1 の可変の規則。下の「fullscreen pass の入力と shader の宣言が合わない拒否」。
[読み直しの候補の照合](../../src/core/fullscreenpass/fullscreenpasscontainer.cpp#L749))。合わない pass の新しい pipeline は
公開せず、その pass は**前の pipeline と前の descriptor set で描き続けます**。同じ読み直しのほかの pipeline は公開されます
([候補を外す所](../../src/core/shader/pipelinefactory.cpp#L1986))。照合する経路は 3 つで、`reload_shaders`、file watch の
shader の batch、render pipeline の読み直しが shader の変更を一緒に抱える経路のどれも同じ照合を通ります
([結果を reload の記録へ渡す所](../../src/core/watch/reloadservice.cpp#L40))。

- **`reload_shaders` の答え**: 読み直し自体は公開されたので `committed: true` のままで(意味は変えていません。shader library は
  compile できた新しい shader を持ち、断られていない pipeline はそれで描きます)、`refused_passes` に断った pass が並びます
  ([答えを組む所](../../src/core/communication/rpcserver.cpp#L1433))。要素は `{pass, fragment, reason, shader_input_mismatch, declarations}`
  で、`reason` は下の拒否と同じ message、`shader_input_mismatch` は編集の拒否と同じ形(同じ関数で組みます)、`declarations` は
  **断られた新しい shader** の宣言 `[{binding, type, name, variadic}]` です([形](../../src/core/fullscreenpass/shaderinputmismatchjson.cpp#L48))。
  `committed: false` の答え(何も変わっていない)には `refused_passes` は付きません。
- **reload status**: `get_status` の `reload.runtime["pelican.shaders"]` は、この読み直しを `attempted` / `applied` / `failed` の
  3 つとも 1 つ増やします(公開はしたが、断った pass があるので失敗としても数える)。`error_code` は **`shader_reload_refused@1`**
  ([code](../../src/core/watch/reloadservice.hpp#L45))、`last_error` は pass ごとに
  `Shader reload refused for pass '<pass>', which keeps drawing with its previous shader: <reason>`(複数なら改行でつなぐ)、
  `error_fields.refused_passes` は上と同じ要素の配列です。file watch の経路でも同じものが残ります。
- **`details.refused_passes`**: 同じ status の `details` には、**今も前の shader で描いている pass の全部**が同じ形で並びます
  ([details に足す所](../../src/core/watch/reloadservice.cpp#L725))。`last_error` は後の(別の shader の)読み直しが成功すると
  消えますが、こちらはその pass の断りが解けるまで残ります。
- **断りが解けるとき**: その pass の新しい shader を受け入れる読み直しが通るか、編集で pass が登録し直されたとき。
  `get_frame_plan` の `pass_inputs.reload_refused` も同じときに消えます(下の「各 pass の set 1 の宣言」)。
- **受け入れた読み直し**は、次の frame の入力の bind し直しで、その pass の set 1 の descriptor set を新しい layout で
  作り直します(宣言を足した・広げた・可変にしたとき)。入力の無い fullscreen pass が読み直しで宣言を持った場合も、既定の
  resource(Q-14f)で埋めた set を bind します([bind し直す所](../../src/core/fullscreenpass/fullscreenpasscontainer.cpp#L1252))。
- **file は新しい shader のまま**です。engine は断った pass を前の shader で描き続けますが、shader library と disk は新しい
  shader を持つので、次の load と、その pass を登録し直す編集は、新しい shader を load / 編集の照合にかけて断られます
  (この順の専用の test はありません)。
- VI-1b の前は(`8cd374a5` の commit message の実測)、pass が渡している宣言を消した shader を読み直すと新しい pipeline が
  公開され、次の 1 frame が Q-14e の error で失敗し、その後の frame は古い set 1 の layout で作った descriptor set で描いていました
  (validation の VUID 00358 / 08600、出力は未定義)。入力の無い pass の shader が宣言を足した場合は set 1 が一度も bind されて
  いませんでした。
- ⚠ 照合しないもの: 入力の無い raster pass(load でも照合しないので、読み直しも同じ)と、XR mirror の output transform
  (pass の名前を持たない pipeline)。読み直しで固定の宣言が既定の resource で埋まっても、Q-14f の LOG_WARNING は出ません
  (warning を出すのは pass の compile だけです)。

### レンダー設定を編集する

**シーン編集とは実行モデルが違います。**混ぜると壊れます。違う理由は対象が違うからです —— シーン編集の対象がメモリ上の `AuthoringSceneDocument`(正は `SceneRevision`)なのに対し、こちらは**ディスク上の rendering config ファイルそのもの**で、正はその sha256 digest です。したがって版の比較も、undo の有無も、書き込みの作法も別建てになります。

| メソッド | 何をするか |
|---|---|
| `get_render_features` | 著作 config の現在の `features[]`、`source_reference`、`source_digest`、runtime の状態を返す |
| `list_render_features` | **engine が可用性を判定した**カタログ。各項目に `available` が付き、**`unavailable_reason` は `available` が false の項目にだけ現れる**(使える項目には鍵ごと無い)。`source` はその文書の出所で、engine 内蔵なら `"engine"`、`project://packages/<ns>/` に vendoring された package のものなら `"package"` の 2 値だけです([catalog entry](../../src/core/communication/renderconfigeditor.hpp#L51))。読めない package 文書は**カタログから消えるのではなく** `available:false` + `unavailable_reason` として並びます([package の列挙](../../src/core/communication/renderconfigeditor.cpp#L795)) |
| `edit_render_features` | `base_source_digest` と `operations[]` を受け、**チケットを返す**。結果は `get_edit_result` か `step_frame` の `edit_results[]` |
| `edit_render_features` / `materialize` | operationに`op: "materialize"`・`scope: "root"`または`"preset"`・`index`・`destination_reference`・`base_feature_digest`を指定。presetの場合は`base_preset_digest`も必要。具体値へ展開したfeatureを未作成のprojectファイルに保存し、元instanceをその参照へ置換する。featureのdigestは`get_render_features.feature_instances[].feature_document_digest`、presetのdigestは同entryの`source_digest`を使う。engine presetは先にforkする。Studio からの入口は Render Features の **Make editable...** |
| `get_scene_acceleration_structures` | 名前付きTLASの`definitions[]`、taskの接続先`task_connections[]`、編集元の`source_digest`、三角形座標系とレイアウトの編集可否・digestを含む`triangle_geometry`を返す |
| `edit_scene_acceleration_structures` | `base_source_digest`と`definitions[]`、任意の`task_connections` objectでTLASと接続先を置換。`triangle_space`と`base_layout_digest`を組で渡すとレイアウトの三角形座標系も同時保存し、既存の編集ticketを返す |
| `get_ray_callable_records` | taskごとのCallable schema、shader一覧、元宣言と実効初期値、rootの`source_digest`を返す |
| `edit_ray_callable_records` | `base_source_digest`と`task_records` objectで初期値の上書き指定全体を置換。既存の編集ticketを返す |
| `fork_render_pipeline` | `base_source_digest`・`base_preset_digest`・`destination_reference`で選択中presetを新しいprojectファイルへコピーし、rootの参照先も切り替える |
| `edit_region_replacement` | `base_source_digest`・`base_preset_digest`・`graph`・`region`でproject presetのタグ付き領域を指定。`provider`に登録済みprovider名を指定するとコンパイル時の差し替えを選択し、`null`なら差し替え要求を除去して元のノード構成を使用する。元のpass・設定・接続は保持する。現在の指定は`get_render_authoring_context`の各graphの`region_replacements`で取得できる。標準feature全体の展開操作ではない |
| `edit_preset_pass` | `base_source_digest`・`base_preset_digest`・`graph`・`pass_name`でproject presetのpassを指定。fullscreenは`input`・`output`・`shader`と任意の`ray_query`・`uses_light_data`・`push_constants`・`clear_colors`(後の 3 つは ✅G-3。省略は保持、`null` で削除。[G-3 の 3 field の受け取り](../../src/core/communication/renderconfigeditor.cpp#L3303))、materialは`input`・`output`・`ray_query`の1項目以上。rasterはさらに`shader`・`draw`・`raster_state`を部分編集できる。省略項目は保持、rasterの`raster_state: null`は既定状態へ戻す。書き先は 3 つ: project preset(`base_preset_digest`)/ project feature(`base_feature_digest`)/ **direct root 自身**(✅F-6。root が preset を選ばず、要求がどちらの digest も持たないとき。`base_source_digest` が root の digest を兼ね、root の `rendering.rendering_passes` の pass を編集する)。置換規則は 3 つとも同じ関数です([direct root の書き先](../../src/core/communication/renderconfigeditor.cpp#L3410)) |
| `get_compute_task_authoring` | compute/RT taskの元宣言・編集可否・保存先とdigestを取得する。各 task に出所の `origin`、graph node が作った task には `graph_node` が付く(✅MG-3b。下の「task の出所」) |
| `edit_compute_task` | `base_source_digest`・`base_task_source_digest`・`task_name`・`task`でtask宣言を置換する。名前は保持する |
| `add_compute_task` | `base_source_digest`・`destination_reference`・`task`で新規project featureにtaskを作り、rootのfeaturesへ追加する |
| `remove_compute_task` | `base_source_digest`・`base_task_source_digest`・`task_name`で元文書からtaskを削除する |
| `edit_render_targets` | `base_source_digest`・`ops[]`(✅F-1)。op は `{op:"add", target}` / `{op:"replace", name, target}`(`target.name` を変えれば改名)/ `{op:"remove", name}`。書くのは managed fragment `project://passes/authoring/render-targets.json` 1 つだけで、最初の要求で作って root の `features[]` に足す(空になっても消さない)。ops は順に当て、最後に 1 回 preflight して ticket を返す —— **全部入るか、何も入らないか**。名前付きの拒否は下の 6 種([edit_render_targets の実装](../../src/core/communication/renderconfigeditor.cpp#L3795)) |
| `add_authored_pass` | `base_source_digest`・`graph`・`insert`(authoring context の挿入位置の token)・`pass`。pass 1 つを managed fragment `project://passes/authoring/pass-<id>.json` に書き、root の `features[]` に足して preflight し、ticket を返す。受ける `type` は `fullscreen` と **`material`**(✅G-1)だけで、それ以外は `authored_pass_type_unavailable`(detail は `requested_type` と **`supported_types`**(配列)。[add_authored_pass の種類の門](../../src/core/communication/renderconfigeditor.cpp#L3527))。material pass の形の検査は下の段落。任意の **`material_route_default`**(`"auto"` / `"forward"`。✅G-4)を渡すと、pass と rendering 文書の root の既定を 1 つの ticket で変える(下の「material の route の既定」) |
| `set_material_route_default` | `base_source_digest`・`material_route_default`(`"auto"` / `"forward"`)。root の既定だけを変える。root 編集の ticket に乗り、commit の結果に `restart_required: true`(✅G-4。[`RenderConfigEditorService::setMaterialRouteDefault()`](../../src/core/communication/renderconfigeditor.cpp#L3041)) |
| `edit_graph_document` | `base_source_digest`と`ops[]`で共通グラフ文書を編集する(§13.5「グラフの装着を編集する」)。ops は順に候補文書へ当て、最後に 1 回だけ展開する — **全部通るか、文書ごと拒否か**のどちらか |
| `edit_graph_instances` | `base_source_digest`・`attach[]`・`detach[]`。owner を持つ root instance の装着 / 取り外しだけを扱い、中身は `edit_graph_document` の ops へ翻訳される([edit_graph_instances の実装](../../src/core/communication/renderconfigeditor.cpp#L2383)) |
| `export_render_feature` | `scope`(`root` / `preset`)と`index`で feature 1 つを `user://components/<name>/` へ書き出す。`engine://` の feature は `render_component_engine_reference` で拒否([export の実装](../../src/core/communication/renderconfigeditor.cpp#L1282)) |
| `import_render_feature` | `base_source_digest`・`source_dir`(project 外の絶対パス)・`destination_reference`(未作成の`project://`)。component が要求する render target を project が宣言していなければ `render_component_requires_missing` |
| `install_package` | `source_dir`(project 外の絶対パス)。`<project>/packages/<namespace>/` へ複製して`project.json`に記録し、catalog を作り直す。返すのは`namespace`・`package_version`・`sha256`・`feature_references`・`feature_names` |
| `uninstall_package` | `namespace`。root config と選択中 preset の全文字列を走査し、まだ参照が残っていれば`package_in_use`(参照を列挙)で**何も消しません** |
| `decompose_material_pass` | `base_source_digest`・`base_preset_digest?`・`pass`。一体版の material pass を `render.material_pass` + draw_loop の `graph.loop` + body の definition に 1 つの ticket で書き換える(✅LP-3b。下の「ループと分解の RPC」) |
| `get_render_documents` / `restore_render_documents` | render 文書を丸ごと読む / CAS の下で丸ごと戻す(作る・消すを含む)。render の編集の undo の土台(✅RD-1 / RD-1b。下の「ループと分解の RPC」) |
| `get_graph_templates` | `{}`。engine が同梱する分解版の雛形(`engine://graphs/*.json`)を `{templates: [{reference, title, description?, decomposes, document}]}` で返す(✅GT-1。下の「ループと分解の RPC」) |
| `apply_graph_template` | `base_source_digest`・`base_preset_digest?`・`template`。雛形の nodes / definitions を root の graph へ写し、`decomposes` に従って feature を外すか `pass_replacements` を書き、`shader_defines` を足すのを 1 つの ticket で行う(✅GT-1。下の「ループと分解の RPC」) |

**render target の宣言(✅F-1)。**`edit_render_targets` が触れるのは上の fragment に宣言された target だけです。
feature に置くのは、preset root が root に `render_targets` を置けず、direct root と preset root の両方で通る置き場が
feature だけだからです。拒否は次の 6 種で、どれも文書を 1 byte も変えません。

| code | いつ | 付くもの |
|---|---|---|
| `render_target_not_editable` | fragment の外(root / preset / 他の feature / engine)で宣言された target を replace / remove した。`remove_authored_pass` にこの fragment を渡したときも同じ code | `name`・`source` |
| `render_target_exists` | add した名前が、どこかで既に宣言されている | `name`・`source` |
| `render_target_referenced` | remove / 改名で消える名前を、まだ読む・書く pass / task がある | `name`・`users[]` |
| `render_target_reserved` | `display` / `swapchain`(engine の予約名) | `name` |
| `render_target_not_found` | その名前の target がどこにも宣言されていない | `name` |
| `render_pipeline_preflight_failed` | 宣言の形の誤り。文言は parser(`parseRenderTargetDefinitionsFromJson`)のまま、participant は `pelican.render_targets` | `participant`・`fragment_reference` |

使用者の検査は新しい走査器ではなく、frame planner と同じ `parseFrameGraphDefinitionsFromConfigJson` の読みから引きます
([使用者](../../src/core/communication/renderconfigeditor.cpp#L688))。

読む側は `get_render_authoring_context` です。`render_targets[]` は合成後の全 target を
`{name, declaration, editable, source}` で返し、`editable` なもの(= fragment のもの)には fragment に書いてある宣言
そのもの `authored_declaration` が付きます。並びは editable なものが先です([組み立て](../../src/core/communication/renderconfigeditor.cpp#L2187))。
`source` は `authoring`(fragment)/ `root` / `preset` / `feature:<reference>` / `engine`(`display` など)で、
**文書にも合成器の出所記録にも出所が無いときだけ `composed`** になります(graph の合成で出来た target など。
[出所の判定](../../src/core/communication/renderconfigeditor.cpp#L627))。ほかに `render_target_fragment`(fragment の参照)と
`render_target_vocabulary`(`formats` / `usage` は parser 自身の表、`format_classes` は `scene` / `display` / `data` /
`explicit`)が付きます。fragment は pass を持たないので `managed_fragments` には出ません。

**編集は、起動時に登録した graph の変種の全部で合成してから受けます(✅G-6)。**engine は起動時に flat と一緒に preview の
graph も組み、XR を始めたときは xr も組みます([第6章 §6.13](06_rendering.md))。render の編集の候補は、ticket を取る前に
このすべてで合成されます。flat と preview は常に、xr は起動時に XR を始めた engine だけです(xr は history を持つ feature を
拒否するので、XR を始めていない engine で xr を合成すると、TAA の project の編集がすべて断られてしまいます)。対象は root の
文書を変える編集(`edit_render_features`・`edit_preset_pass`・`edit_graph_document` など)と、authored pass の系統
(`add_authored_pass`・`remove_authored_pass`・`edit_render_targets`)です([変種を合成する preflight](../../src/core/communication/renderconfigeditor.cpp#L995))。
flat で合成できない候補は今までどおり flat の経路で断られます。flat が通って preview か xr が合成できない候補は
`render_pipeline_preflight_failed` に **`variant`**(`"preview"` / `"xr"`)が付いて断られ、文書は変わりません(✅LP-0d から、graph の名前付きの拒否と `<code>: <fields>` で運ばれる拒否は、その code のまま `variant` を添えて返ります。下の「ループと分解の RPC」)。`set_layout`
だけの `edit_graph_document`(再 compose しない編集)と、context の読み取りは合成しません。Studio はこの拒否を、Pass Editor と
Frame Plan の status 行に `The preview graph refused it: <code>: <message>` の 1 行で出します(以前の Pass Editor は
`authoring failed` とだけ出し、理由は tooltip にしかありませんでした。[拒否の 1 行を作る所](../../src/devstudio/view/frameplanedit.cpp#L1799))。
例: `history: true` の target `trail` を足す編集は通りますが(preview はその fragment ごと外します)、direct root の `present` に
`trail@history` を読ませる `edit_preset_pass` は preview で断られます。

**fullscreen pass の入力と shader の宣言が合わない拒否は、どの入力かを言います(✅Q-14e)。**fullscreen pass(と入力を持つ raster pass)の入力は、shader の set 1(`PELICAN_SET_PASS_INPUT`)の binding を `input` の位置で数えます —— texture の入力が binding 0 から、その後に buffer の入力です([第6章 §6.4](06_rendering.md))。engine は pass を組むときに、入力ごとにその binding の宣言を shader の reflection で探し、合わない最初の binding で断ります([入力と reflection の照合](../../src/core/fullscreenpass/fullscreenpasscontainer.cpp#L134))。逆向き —— shader が、pass の渡す入力の数以上の binding を宣言している —— は、埋められる種類なら engine が既定の resource で埋めて編集は成功し、埋められない種類なら同じ形で断ります(✅Q-14f。何を埋めるかは [第6章 §6.4](06_rendering.md) の「fullscreen pass の入力と shader の宣言の契約」)。どちらの向きかは `direction` で分かります。code は今までどおり `render_pipeline_preflight_failed` で、`error` に **`shader_input_mismatch`** が付きます([`shader_input_mismatch` を足す所](../../src/core/communication/renderconfigeditor.cpp#L102))。

| 鍵 | 中身 |
|---|---|
| `pass` | pass の名前 |
| `fragment` | pass の fragment の参照(書いたまま。例 `project://shaders/present`)。fragment の無い raster pass では `""` |
| `direction` | `"pass_input_undeclared"`(pass の入力を shader が宣言していない。Q-14e)/ `"shader_input_unsupplied"`(shader の宣言を pass が渡さず、engine も埋められない。✅Q-14f)/ `"variadic_layout_invalid"`(shader の宣言が可変の組の並べ方の規則を破っている。✅VI-1)/ `"variadic_capacity_exceeded"`(pass の texture の入力が、可変の組の終わり `first + max` より多い。✅VI-1)。[2 つの値](../../src/core/fullscreenpass/fullscreeninputbindingmismatch.hpp#L39)、[VI-1 の 2 つ](../../src/core/fullscreenpass/fullscreeninputbindingmismatch.hpp#L51) |
| `kind` | `pass_input_undeclared`: 入力の種類 `"texture"` / `"buffer"`。`shader_input_unsupplied`: 宣言の種類 `"texture"`(image・sampler・input attachment)/ `"buffer"`(storage・uniform・texel buffer)/ `"other"` |
| `binding` | `pass_input_undeclared`: 合わなかった set 1 の binding(texture の入力は 0 から、その後に buffer の入力)。`shader_input_unsupplied`: 埋められない宣言のうち最小の binding |
| `resource` | その入力の render target / buffer の名前(任意。引けなければ鍵ごと無い)。`shader_input_unsupplied` では名指す入力が無いので常に無い |
| `expected` | `pass_input_undeclared`: その binding に要る宣言 `"sampled_image"`(普通の texture の入力)/ `"input_attachment"`(local read の入力。[第6章](06_rendering.md) の「自動 tile-local attachment / same-pixel local read」)/ `"storage_buffer"`。`shader_input_unsupplied`: `"none"` |
| `pass_texture_inputs` / `pass_buffer_inputs` | pass の texture / buffer の入力の数 |
| `shader_pass_inputs` | shader が set 1 に宣言しているもの `[{binding, type, name, variadic}]`、binding の昇順。`type` は上の 3 つのほか descriptor の種類の snake_case(`uniform_buffer` など)。`name` は reflection が読んだ変数名(SPIR-V に名前が無ければ `""`)、`variadic` は可変の組の宣言かどうか(`name` と `variadic` は ✅VI-1) |
| `variadic_range` | shader の可変の組 `{first, max}`。組が無ければ `null`(鍵は常にある。✅VI-1) |
| `variadic_rule` | `variadic_layout_invalid` のときだけ。破った規則: `"multiple_variadic_groups"` / `"declaration_after_variadic_group"` / `"variadic_group_malformed"`(✅VI-1。下の段落) |

`pass_input_undeclared` の message は前からの文(texture は `Fullscreen pass input texture/local-read contract does not match shader reflection`、buffer は `Fullscreen pass input buffer does not match shader reflection`)で始まり、`: pass '<pass>' input <binding> ('<resource>') expects a sampled image at set 1 binding <binding>, but fragment '<fragment>' declares no pass input at that binding (shader declares <N> pass inputs)` が続きます。buffer の入力は `(buffer '<名前>')`、その binding に別の種類の宣言があれば `declares a uniform buffer there` のように言い、N が 1 なら `pass input)` です。pass の外から呼ばれたとき(XR mirror の output transform)は `pass` と `fragment` が `""` で、message は `pass '...'` を省いて `the shader` と言います([message を組む所](../../src/core/fullscreenpass/fullscreeninputbindingmismatch.cpp#L160))。例(Blank pipeline の、何も読まない `present` に入力を 1 つ足した):

```json
{"code":"render_pipeline_preflight_failed","message":"Fullscreen pass input texture/local-read contract does not match shader reflection: pass 'present' input 0 ('q14e_a') expects a sampled image at set 1 binding 0, but fragment 'project://shaders/present' declares no pass input at that binding (shader declares 0 pass inputs)","participant":"pelican.render_pipeline","ticket":"render-config-edit-1","shader_input_mismatch":{"pass":"present","fragment":"project://shaders/present","direction":"pass_input_undeclared","kind":"texture","binding":0,"resource":"q14e_a","expected":"sampled_image","pass_texture_inputs":1,"pass_buffer_inputs":0,"shader_pass_inputs":[]}}
```

`shader_input_unsupplied` の message は `Fullscreen shader declares a pass input the pass does not supply: pass '<pass>' supplies <M> pass inputs, but fragment '<fragment>' declares <宣言の種類> at set 1 binding <binding> (shader declares <N> pass inputs)` の形です。M・N が 1 なら `pass input` で、pass の外から呼ばれたときは `the pass supplies ...` / `the shader declares ...` と言います([逆向きの message](../../src/core/fullscreenpass/fullscreeninputbindingmismatch.cpp#L32))。例(入力を 1 つ持つ `present` の fragment を、binding 0 を読み binding 1 に uniform buffer を宣言する shader に替えた。`get_edit_result` の `error` のうち `code`・`message`・`shader_input_mismatch` を抜き出したもの):

```json
{"code":"render_pipeline_preflight_failed","message":"Fullscreen shader declares a pass input the pass does not supply: pass 'present' supplies 1 pass input, but fragment 'project://shaders/q14f_uniform_second' declares a uniform buffer at set 1 binding 1 (shader declares 2 pass inputs)","shader_input_mismatch":{"pass":"present","fragment":"project://shaders/q14f_uniform_second","direction":"shader_input_unsupplied","kind":"buffer","binding":1,"expected":"none","pass_texture_inputs":1,"pass_buffer_inputs":0,"shader_pass_inputs":[{"binding":0,"type":"sampled_image"},{"binding":1,"type":"uniform_buffer"}]}}
```

埋められる宣言だけが余っているときは断られず、同じ文の後に `; the engine binds a default resource (zeros) at set 1 binding(s) <埋めた binding>` を付けた LOG_WARNING が compile ごとに 1 行出ます(編集の結果には何も付きません)。

上の 2 つの例は VI-1 の前に取ったもので、VI-1 からは `shader_input_mismatch` に `variadic_range`(可変の組が無ければ `null`)が付き、`shader_pass_inputs` の要素にも `name` と `variadic` が付きます(形を組む関数は編集・読み直し・`get_frame_plan` で 1 つです。[`shaderInputMismatchJson()`](../../src/core/fullscreenpass/shaderinputmismatchjson.cpp#L21))。

**可変の入力の拒否(✅VI-1)。**shader が `PELICAN_DECLARE_VARIADIC_INPUTS(first, max)` で可変の組を宣言したとき(規則は [第6章 §6.4](06_rendering.md) の「可変の入力」)の拒否も、同じ `render_pipeline_preflight_failed` / `shader_input_mismatch` で来ます。

- **`variadic_layout_invalid`**: `binding` は規則を破った宣言の binding、`kind` はその宣言の種類、`expected` は `"none"`、`variadic_rule` が破った規則です([規則を読む所](../../src/core/fullscreenpass/passinputlayout.cpp#L97))。
  - `"multiple_variadic_groups"`: 可変の組が 2 つ以上。マクロを 2 回書いた shader は compile できない(マクロが置く定数 `pelican_variadic_first` の再定義)ので、engine は compile log からこれを読み取り、`binding` 0・`variadic_range` `null` で断ります。message は `Fullscreen shader declares more than one group of variadic pass inputs: fragment '<fragment>' uses PELICAN_DECLARE_VARIADIC_INPUTS more than once; a shader declares one group, after its fixed inputs`。compile は通り reflection で 2 つ目の組が見えた場合(stage ごとに違う組を宣言した、組の名前の宣言を手で書いたなど)は `... declares variadic inputs at set 1 bindings <a>-<b> and again elsewhere; ...` と言い、`variadic_range` は小さい方の組です。
  - `"declaration_after_variadic_group"`: 組の最初の binding 以降に buffer 以外の固定の宣言がある、または組の binding の中に固定の宣言(buffer を含む)がある。message は `Fullscreen shader declares a pass input after its variadic inputs: fragment '<fragment>' declares <種類> at set 1 binding <b>, but its variadic inputs take bindings <a>-<c>; fixed texture inputs come before the group and buffers start at binding <first+max>`。
  - `"variadic_group_malformed"`: 名前が組のもの(`pelican_variadic_input_<k>`)なのにマクロが作った形でない —— 2D の combined image sampler でない、binding が `first + k` でない、配列、k に抜けがある。message は `Fullscreen shader's variadic pass inputs are malformed: fragment '<fragment>' declares '<名前>' at set 1 binding <b>; declare variadic inputs only with PELICAN_DECLARE_VARIADIC_INPUTS(first, max)`。
- **`variadic_capacity_exceeded`**: pass の texture の入力が `first + max` 個より多い(溢れた入力の番号は最初の buffer の番号になるため)。`kind` は `"texture"`、`binding` は `first + max`(溢れた最初の入力)、`resource` はその入力の target の名前、`expected` は `"sampled_image"`。message は `Fullscreen pass supplies more texture inputs than its shader takes: pass '<pass>' supplies <N> texture inputs, but fragment '<fragment>' takes at most <first+max> (variadic inputs at set 1 bindings <a>-<c>); input <b> ('<target>') has no binding` です([message を組む所](../../src/core/fullscreenpass/fullscreeninputbindingmismatch.cpp#L136))。

可変の組の枠を pass が渡さないのは意図した使い方なので、そこを既定の resource で埋めても LOG_WARNING は出ません(固定の宣言を埋めたときは従来どおり出ます。[警告から除く所](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L2460))。

**pass ごとの `shader.defines` の拒否(✅VI-1)。**fullscreen / raster pass の `shader.defines`(第6章 §6.4)が読めないときも code は `render_pipeline_preflight_failed` で、`error` に **`pass_shader_define`** `{pass, define, name, index, reason}` が付き、文書は変わりません([付ける所](../../src/core/communication/renderconfigeditor.cpp#L111))。`define` は書いたままの要素(文字列でなければその JSON の文字列)、`name` は `=` の前の名前(識別子でなければ `""`)、`index` は配列の位置です。`reason` は 3 つです([読む所](../../src/core/renderingpass/passshaderdefines.cpp#L61))。

| `reason` | いつ | message(`Pass '<pass>' shader.defines entry <index> ('<define>') ` の後) |
|---|---|---|
| `reserved` | engine が pass の compile で自分で定義する名前 —— `PELICAN_PASS_TEXTURE_INPUT_COUNT` / `PELICAN_VARIADIC_INPUT_COUNT` / `PELICAN_VARIADIC_INPUTS_LIMIT` / `PELICAN_MULTIVIEW` / `PELICAN_VIEW_COUNT` / `PELICAN_OUTPUT_UNORM_FALLBACK`、`PELICAN_INPUT_` か `PELICAN_FEATURE_` で始まる名前([予約の表](../../src/core/renderingpass/passshaderdefines.cpp#L43)) | `defines <名前>, which the engine defines itself when it compiles the pass` |
| `malformed` | 文字列でない、`NAME` / `NAME=VALUE` の NAME が C の識別子でない、改行を含む | `is not NAME or NAME=VALUE with NAME a C identifier` |
| `duplicate` | 同じ pass で同じ名前を 2 回 | `defines <名前> a second time` |

`defines` が配列でないときは `pass_shader_define` は付かず、message は `Pass shader.defines must be an array: <pass>` です(編集 RPC でこの形を確かめた test は無く、code は未確認)。

**各 pass の set 1 の宣言を `get_frame_plan` で読む(✅VI-1 / VI-1b)。**`get_frame_plan` の `nodes[]` のうち fullscreen / raster pass の node には **`pass_inputs`** が付き、その pass が今描いている pipeline の shader の set 1 の宣言を、pass が渡しているかどうかに関わらず全部返します(Studio がソケットを宣言から描くためのもの。[組み立て](../../src/core/vkcore/renderer.cpp#L3954))。

| 鍵 | 中身 |
|---|---|
| `state` | `"declared"`。pass の pipeline が見つからないなどで読めなければ `"unavailable"` で、そのときは `fragment` と `reason`(例外の文)だけが付く |
| `fragment` | pass の fragment の参照(書いたまま。fragment の無い raster pass では `""`) |
| `declarations` | `[{binding, type, name, variadic}]`、binding の昇順(`shader_pass_inputs` と同じ形) |
| `variadic_range` | `{first, max}` か `null` |
| `pass_texture_inputs` / `pass_buffer_inputs` | pass が渡している texture / buffer の入力の数 |
| `buffer_binding_base` | pass の最初の buffer の入力が入る binding(可変の組があれば `first + max`、無ければ texture の入力の数) |
| `reload_refused` | ✅VI-1b。その pass が最後に受けた shader の読み直しを断っている間だけ付く: `{reason, shader_input_mismatch, declarations}`。`declarations` は**断った新しい shader** の宣言で、上の `declarations` は今描いている前の shader の宣言のまま |

例(`engine://passthrough` の pass に何もつないでいないとき。[test](../../test/vi1_variadic_inputs_test.cpp#L737) の期待値): `{"state":"declared","fragment":"engine://passthrough","declarations":[{"binding":0,"type":"sampled_image","name":"pelican_variadic_input_0","variadic":true}, ... 8 要素],"variadic_range":{"first":0,"max":8},"pass_texture_inputs":0,"pass_buffer_inputs":0,"buffer_binding_base":8}`。VI-1 の着地(`b9b0a79a`)の時点では、描いている shader が規則を破っていると `state: "refused"` を返していましたが、VI-1b で読み直しがそういう shader を断るようになり、なくなりました。

Studio の編集(`edit_preset_pass`)では要求自体は受け付けられ(`accepted` と ticket)、この拒否は `get_edit_result` の最終の `error` に来ます(Q-14e の報告。例の `ticket` はそのためです)。`render_pipeline_preflight_failed` を例外から作る所は全部同じ関数を通るので、受付の時点で断られる経路でも形は同じです。ticket の commit で断られて巻き戻しにも失敗した(`rollback_error` が空でない)ときと、ほかの理由の `render_pipeline_preflight_failed` には付きません。Studio がこれをどう読むかは §13.5 の「shader が読まない入力」の段落(✅Q-14)です。Studio は `direction` で行と操作を分けます(✅Q-15。[拒否を読む所](../../src/devstudio/view/shaderinputcontract.cpp#L152))。`"pass_input_undeclared"` と、`direction` の無い拒否(Q-14f より前の engine の形)には、Q-14 の 1 行(`... reads only <N> inputs. To change its inputs, change the shader too.`)と 2 つの操作(Use engine://passthrough / Open the shader。VI-3b より前の 1 つ目は Make a pass-through shader)を出します。`"shader_input_unsupplied"` には `<pass>'s shader (<fragment の stem>) declares an input the pass cannot supply (<型> at binding <b>). Remove that declaration from the shader.` を出します([逆向きの行を作る所](../../src/devstudio/view/shaderinputcontract.cpp#L113))。型は冠詞の無い語で(`_` は空白。上の例なら `uniform buffer at binding 1`)、`shader_pass_inputs` のうち `binding` が同じ要素の `type` から、無ければ message の `declares a <型> at set 1 binding <b>` から取ります。片方しか分からなければ `(binding <b>)` / `(<型>)`、どちらも無ければ括弧ごと出しません。操作は **Open the shader** だけで、Use engine://passthrough は出しません —— pass の入力を増やしても engine が埋められない型だからです([向きごとの操作](../../src/devstudio/view/shaderinputcontract.cpp#L93))。この Open the shader は file を desktop で開くだけで clipboard には何も入れず、行が `Remove the pass-input declaration at binding <b> from the shader and save it, and it reloads by itself. Then connect the input again.`(Pass Editor は最後が `Then press Save again.`)になります([開いた後の行](../../src/devstudio/view/shaderinputcontract.cpp#L130))。`shader_input_mismatch` の無い拒否でも、message が `Fullscreen shader declares a pass input the pass does not supply` を含めば逆向きとして読みます。engine が埋められる宣言を埋めたとき(編集は成功し LOG_WARNING が出るだけ)は、Studio は何も出しません。

⚠ 逆向きで Use engine://passthrough(Q-15 の時点では Make a pass-through shader)を隠し clipboard に入れないことの test は Pass Editor の側だけで、Frame Plan の側の分岐には test がありません(Q-15 の記録)。

**material pass を足す(✅G-1)。**`add_authored_pass` は fullscreen に加えて `type: "material"` を受けます(raster は
scene を描かないので受けません)。合成だけでは material pass の color の枚数・depth・format を見ないので、engine は
合成した候補の中の新しい pass を、load と同じ parser(`parsePassDefinitionFromJson`)に候補の render target で通します
([`validateAuthoredMaterialPass()`](../../src/core/communication/renderconfigeditor.cpp#L458))。contract と枚数が合わない、
depth が無い、swapchain に書く、format が違う、といった誤りは parser の文言のまま `render_pipeline_preflight_failed` で
断られ、文書は変わりません。この検査の target の大きさは仮の 1920×1080 です(parser が見るのは 1 pass の出力どうしの比
だけです)。`format_class: scene` の target の format は、running pipeline の有効な feature に `hdr` があるかで決まります。
その一覧は `get_render_authoring_context` の **`enabled_feature_names`** にも出ます
([context の enabled_feature_names](../../src/core/communication/renderconfigeditor.cpp#L2136))。Studio はこれで、新しく作る
G-buffer の target を HDR / SDR のどちらの format にするかを決めます(context が言わなければ HDR と読みます)。

**material の route の既定と、受け皿の無い material の拒否(✅G-4)。**rendering 文書の root の `material_route_default`
(`"auto"` / `"forward"`。意味と書ける場所は [第6章 §6.7](06_rendering.md))は編集 RPC からも変えられます。

- `add_authored_pass` に `material_route_default` を渡すと、pass の fragment と root の key の変更が**同じ候補文書集合**に入り、
  1 回の preflight・1 つの ticket になります([pass と route の既定を 1 つの候補に入れる所](../../src/core/communication/renderconfigeditor.cpp#L3534))。
  下の受け皿の検査は、新しい既定で計算し直した route で行います。今の値と同じなら key は書かず、再起動も求めません。
- `set_material_route_default` は既定だけを変えます(上の表)。
- engine はこの key を**起動時に 1 回しか読まない**ので、値を変えた ticket の commit の結果(`get_edit_result` /
  `step_frame` の `edit_results[]`)に **`restart_required: true`** と `restart_reason: {code: "material_route_default_changed"}`(✅RS-1 から)が付きます
  ([G-4 の restart_required と restart_reason を付ける所](../../src/core/communication/renderconfigeditor.cpp#L5095))。engine を起動し直すまで、
  route が変わる material は今の pipeline のまま描かれます。⚠ commit から再起動までの間に登録された material は、
  古い既定で lowering されます。
- runtime shader compiler の無い build で `"forward"` を頼むと、ticket を作る前に **`material_route_default_unavailable@1`**
  (`value: "forward"`・`reason: "runtime_shader_compiler_disabled"`)で断られます。engine の組込 material の forward 版を
  compile できないためです。

**`material_route_unserved@1`**: pipeline に material pass が 1 つでもあるのに、登録済みの material(engine の組込を含む)の
route を受ける pass が無い候補は、この code で断られ、文書は変わりません。**前の世代がその route を使っていたかどうかは
問いません** —— G-4 の前は、最初から forward の pass だけにする編集が commit を通り、次の起動で落ちていました。足りない
material を route ごとに全部集めて 1 回で断ります
([`MaterialGenerationPlanner::requireEveryMaterialServed()`](../../src/core/material/materialgenerationplanner.cpp#L781))。
`error` の直下に、他の拒否と同じく `ticket`・`participant` と並んで次の field が付きます。

| field | 中身 |
|---|---|
| `rendering_config` | 検査した rendering 文書の参照 |
| `unserved[]` | 受け皿の無い route ごとに `route`・`shader_contract`・`materials[]`・`hint`。`materials[]` の各項目は `name`(名前の無い material は `<unnamed>`)・`origin`(`builtin` / `document` / `asset` / `unknown`)・`source`(文書か asset の参照。組込には無い) |
| `hint` | route ごとの hint を `; ` でつないだもの |

route ごとの hint は 3 つです([route ごとの hint の文](../../src/project/renderpipeline.cpp#L688)):
`forward_opaque` → forward_opaque_v1 の material pass を足す / `deferred_geometry` → root の `material_route_default` を
`"forward"` にするか deferred_geometry_v1 の material pass を足す / `forward_transparent` → forward_transparent_v1 の
material pass を足す。`message` は load 時の fatal と同じ読める 1 文です。code と fields は renderer の文字列の経路を
`<code>: <JSON>` の形で通り、editor の completion handler が名前付きの拒否に戻します
([文字列から拒否に戻す所](../../src/core/communication/renderconfigeditor.cpp#L4979))。

**live の material が受けられない編集は、書いてから再起動を求めます(✅RS-1)。**runtime に行く ticket(`edit_graph_document`・
`add_authored_pass`・`set_material_route_default`・`restore_render_documents` など、候補を engine に渡して組み直すもの)は、組み直した
候補を生きている material にも当てます([`MaterialGenerationPlanner::validateCompatibility()`](../../src/core/material/materialgenerationplanner.cpp#L1202))。候補の pass が生きている
material を別の**物理の描画の契約**(color の format・depth の format・sample 数・local read の割り当て・attachment の state)で描く
ことになると、material の pipeline をその場で作り直せないので live には載せられません。RS-1 の前はこれを
`render_pipeline_preflight_failed` で断っていたので、blank の project に Graph の palette から最初の material pass を足す編集は
必ず断られました。RS-1 からは、**それが唯一の拒否のときだけ**、editor は文書を commit し、live の pipeline は今のまま残して、
ticket を次の結果で終えます([型付きの拒否のときだけ commit する所](../../src/core/communication/renderconfigeditor.cpp#L4940))。

| field | 中身 |
|---|---|
| `status` / `committed` | `"committed"` / `true` |
| `published_generation` | live の今の世代のまま(新しい世代を publish しません) |
| `restart_required` | `true` |
| `restart_reason` | `code: "material_physical_contract_changed"`・`material`(live の material の名前。空のことがあります)・`material_id`・`pass`(その material を別の契約で描く候補の pass)・`detail`(違う所の配列。`color_formats` / `depth_format` / `sample_count` / `local_read` / `attachment_state` のうち当たるものをこの順で)・`message`(拒否の文) |
| `documents` | RD-1 と同じく、書いた文書の `{reference: sha256 hex}`。`restore_render_documents` で戻せます |

- `restart_reason` は、最初に見つかった 1 つの material と 1 つの pass だけを言います。
- engine は物理の契約の拒否を、ほかの material の検査(受け皿の `material_route_unserved@1`、output schema の変更、`ray_query` の
  宣言漏れ)がすべての material で通った後にだけ投げます([最後に投げる所](../../src/core/material/materialgenerationplanner.cpp#L1290))。別の拒否が隠れたまま commit される
  ことはありません。ただしこの検査より後ろの検査 —— XR の variant の default pass と、publish の直前の準備(active な XR の
  variant が残っているか、graph の runtime の引き継ぎの準備)—— は、この経路では走りません。
- ほかの失敗は今までどおり断って、文書を元に戻します。
- commit は editor 自身が、runtime が呼ぶはずだったのと同じ preset の CAS と複数ファイルの commit で行います。その間に文書が外から
  変わっていれば `external_modification` で断ります。
- 再起動を待つ間の状態は持ちません。文書は commit した内容に進むので、次の編集も同じ live の material に当たり、同じ拒否で
  もう一度 commit と `restart_required` になります。material pass を消して live と合う形に戻せば、普通に live に載ります。engine を
  起動し直すまで、`get_frame_plan` は古い live の世代を言います。
- 上の G-4 の route の既定の再起動と両方が当たる ticket では、`restart_reason` は `material_physical_contract_changed` の方です
  ([RS-1 の restart_reason を載せる所](../../src/core/communication/renderconfigeditor.cpp#L5094))。
- `set_layout` だけの `edit_graph_document`(N-6。runtime に行かない)は対象外です。
- Studio がこの結果を受けて engine を起動し直す流れは、[Studio のノードキャンバス](#studio-のノードキャンバスgraph-タブ)の
  「engine の再起動が要る graph の編集(✅GL-1b)」にあります。
- ⚠ 残した穴: ディスクからの reload([`Renderer::reloadRenderPipelineFromDisk()`](../../src/core/vkcore/renderer.cpp#L3505))は live の material の検査を
  切り([`validate_live_materials = false` の所](../../src/core/vkcore/renderer.cpp#L3537))、material の pipeline を reload service の側で組み直します。file
  watcher が開いていれば、RS-1 が commit したファイルを拾って live に載せる可能性があります(未確認)。authored の経路でもこの
  組み直しを使う案は、利用者の判断待ちです。
- test: `renderconfigeditor_test` の
  [「RS-1 a candidate refused live only for a material's physical contract is committed with restart_required」](../../test/renderconfigeditor_test.cpp#L7407)
  (runtime を差し替えて、結果の field・2 回目の編集・ほかの拒否が何も書かないことを見る)と、`engine_primitives_test` の
  [「RS-1 a graph edit the live materials cannot take is committed and asks for a restart」](../../test/engine_primitives_test.cpp#L789)
  (GPU。blank の project に palette と同じ op で `render.material_pass`(deferred_geometry_v1)と G-buffer の image 6 枚を足し、
  起動し直すと frame plan にその pass が出ることまで見る)。

Render Featuresの **Parameters...**で、選択中 feature instance の scalar パラメータを編集できます
(`edit_render_features` の `set_parameters`)。欄は engine の `parameters` 宣言から作られ、
`get_render_features.feature_instances[].parameter_schema` がその宣言です。**materialize 済みの
feature でも `parameter_schema` は返ります** —— ただし宣言の `default` は、その派生が焼き込まれた
`bound_parameters` の値で上書きされます。したがって**元の engine 既定値は RPC からは見えません**
([上書き箇所](../../src/core/communication/renderconfigeditor.cpp#L1066))。
`parameter_schema` が `null` の feature は「編集できるパラメータを宣言していない」ので、
`reason` を表示するだけで dialog は開きません。

Render Featuresの **Make editable...**は materialize(§13.5 の表)です。押せるのは
`feature_instances[].materializable` が true の項目だけで、false のときは `materialize_reason` が出ます ——
engine preset の feature なら「先に project へ fork する」、すでに `bound_parameters` を持つ派生なら
「これはもう materialize 済み」の 2 通りです([materializable の判定](../../src/core/communication/renderconfigeditor.cpp#L1057))。
保存先は未作成の `project://` ファイルで、**Apply**で元 instance の参照がその派生に差し替わります。
materialize しても scalar の宣言は消えません —— 宣言を残したまま `bound_parameters` に値を持つので、
**materialize の後も Parameters... で編集できます**。

Render Features → **Fork preset...**でコピー元を確認し、`project://pipelines/custom.json`など
未作成の保存先を指定します。元presetの文書をコピーするので、既存featureの二重適用はしません。
project側の設定・feature・RT上書き指定は維持します。元presetかrootが外部変更された場合は、
Refresh sourceで再取得してください。直接記述のrendering configには対象presetがありません。

Render Features → **Region replacements...**でproject presetのタグ付き領域を選び、
差し替え先の登録済みprovider名を指定できます。欄を空にして **Apply**すると差し替えを解除し、
元のノード構成とその編集内容を使います。通常のコンパイラ最適化は維持します。
engine presetは先に **Fork preset...**でprojectへコピーしてください。
保存後に別の領域を編集する場合は **Refresh source**で現在の設定を取得します。

Render Featuresの **Scene AS...** にある **Triangle coordinate space** で三角形の座標系を選べます。
**Geometry space** は個体ごとの変換を使い、**World space** は変換を三角形へ適用して、条件が合う個体を同じBLASへまとめます。
worldではhit shaderのobject座標系もworldになり、ObjectToWorld/WorldToObjectはidentityになります。procedural AABBは変わりません。
project所有のray group layout(version 3。MG-2a から他の version は読めません)が編集対象です。**Apply**でAS定義・接続と一緒に保存し、失敗時は元設定を保持します。

fork後はFrame Planでfullscreen passを選び、**Edit preset pass...**で入出力とshader参照を編集できます。
material/raster passを選ぶと、Pass Editorで入力・color/depth出力と **Use scene ray query** / **Scene acceleration structure** を編集できます。
`scene`はシーン全体、それ以外は宣言済みTLAS名を指定します。チェックを外すとqueryを無効にします。
**Save preset pass**で保存・反映します。materialの描画契約やroutingは保持します。入力の追加・削除、color出力とdepth対象の変更ができます。
rasterではvertex/fragment shader参照、**Draw settings (JSON)**、**Raster state (JSON, optional)**も編集できます。
drawは例えば`{"vertex_count":6,"instance_count":2}`で頂点数と個体数を指定します。raster state欄を空にすると明示設定を削除し、既定状態へ戻します。編集しない項目は保持します。
color attachment objectは表示されるJSON欄で設定を保持・編集します。depth対象の変更は元のload/store等を保持します。
解決できない入力やTLAS名、描画契約やshaderと矛盾する変更はエンジンが拒否します。
名前・配置・その他の描画設定は保持します。保存は既存の編集ticketを使い、失敗時はpresetとruntimeを戻します。
元preset由来の編集可能なpassはauthoring contextに`editable_preset_pass: true`と
合成前の`preset_declaration`を持ちます。engine共有preset、feature由来pass、生成されたAS/anchor nodeは対象外です。

**direct root の pass(✅F-6)。**preset を選ばない root(Blank pipeline の project など、`config_kind` が `direct`)が
自分で宣言した pass は、authoring context で `editable_root_pass: true` と合成前の `root_declaration` を持ちます
([`editable_root_pass` を付ける所](../../src/core/communication/renderconfigeditor.cpp#L2016))。engine が足した pass(`output_transform` など)と
feature の pass は `false` で、root が宣言していても編集できる種類(fullscreen / material / raster と一部の補助 pass。
preset の pass と同じ判定)でなければ `false` です。これを `base_source_digest` だけで `edit_preset_pass` に渡すと、root の文書が
書き換わります —— root の `rendering_passes` の list を 1 つの値として書き直し、文書のほかの部分は byte のまま残します。
direct root での拒否は 2 つ増えました。

| code | いつ | 付くもの |
|---|---|---|
| `root_pass_not_found` | その graph の root に、その名前の編集できる pass の宣言が無い(feature の pass、engine が足した pass) | `graph`・`pass_name`・`provenance`(出所が引けなければ `null`) |
| `root_routing_not_editable` | `material_routing` を渡した(routing は preset のもので、direct root の pass では編集しない) | `graph`・`pass_name` |

preset root で digest を持たない要求は、今までどおり `project_preset_required` です(fork が要ります)。root の pass を
**消す**手段はありません。direct root の `material_routing` は Studio に表示されません。
終端の`output_transform`もエンジン生成ノードなので、このpreset編集の対象にはなりません。
**New pass**で新規追加モードへ戻れます。object形式のdepth attachmentはこのフォームでは未対応です。

Render Featuresの **Tasks...** またはFrame Planの **Edit task...** からcompute/RT taskを編集できます。
まず宣言JSONを直接編集する詳細画面として、shader・resource_ports・reads/writes・dispatch等の既存schemaを扱います。
選択taskの元ファイル（project feature、project preset、直接記述のroot）に保存します。
別task・他設定は保持し、compilerが受理しない変更や外部変更は適用しません。
task切替や失敗時には入力を保持し、**Refresh**で破棄・再取得します。保持した入力のdigestも維持するため、
他の保存が先行した場合は古い入力のまま上書きせず競合になります。
共有engine文書は読み取り専用です。Frame PlanのEdit taskはcompilerが公開する`authored_name`で元taskを選び、XR用の実行時名からも同じ宣言を開きます。

**task の出所(✅MG-3b)。**`get_compute_task_authoring` の `tasks[]` の各要素に `origin` が付きます。値は `"feature"` |
`"graph_node"` | `"preset"` | `"root"` の 4 つで、**この順に**決まります —— feature が供給した task、graph の `render.compute` node が
作った task、preset root の preset の task、それ以外([origin を決める所](../../src/core/communication/renderconfigeditor.cpp#L2282))。
`"root"` は本番では出ません。rendering 文書の root は task を宣言できず(MG-2b)、node の task は `graph_node` として先に拾われるので、
本番の resolver が root に帰す task は残りません。`"root"` は fake の resolver のための受け皿です。

`origin` が `"graph_node"` の task にだけ `graph_node: {scope, node, instance}` が付きます([graph_node を組む所](../../src/core/communication/renderconfigeditor.cpp#L2295))。

- `scope`: `"root"` か definition の名前。graph 編集 op の `scope` と同じ書き方です。
- `node`: その scope に書かれている node の id。
- `instance`: その node を置いた instance の path。末尾の `/` は付かず、root scope では `""` です。
- task の `name` は展開後の id です(例: definition `smooth` の node `step` を instance `left` が置くと `left/step`)。

✅GL-2a から engine は root を展開し直さず、合成が pass・task の出所に記録した graph の node(`get_frame_plan` の `origin` と同じもの)からこれを組みます。出力は変わりません。ループの回の task は今までどおり `graph_node` になりません(判定の順で `"preset"` か `"root"` に落ちます)。どのループのどの回かは `get_frame_plan` の node の `origin` / `loop_origin` で読みます。

graph node の task の `source_reference` は、root が preset を選んでいても**常に rendering 文書(root)**です(node が書かれているのは
root で、preset ではないため)。`read_only_reason` は「Graph タブで node を編集して」という文です。`editable: false` と、
edit / remove を送ったときの `compute_task_not_editable` は変わりません。

```json
{"name":"left/step","origin":"graph_node","source_reference":"passes/main.json","editable":false,"graph_node":{"scope":"smooth","node":"step","instance":"left"},"read_only_reason":"This task is made by a render.compute node in the graph. Edit the node in the Graph tab."}
```

**rendering 文書の root の task は名前付きで断ります(✅Q-10)。**MG-2b から rendering は `pelican.graph` root だけで、その root の
`rendering` 節には `compute_tasks` を書けません。engine は root の task を一覧に載せたまま、code の無い「編集できない」
(`editable: false` と英語の理由)として返し、実際に edit / remove を送ると `compute_task_not_editable` で断ります。
**この文が出るのは、`origin` が `"root"` の task と、`origin` を持たない古い応答だけです**(Q-12。graph node の task は下の段落の文になります)。
`origin` が `"root"` なら Studio がまだ root の reference を知らなくても root の task です(engine がそう言っているので)。`origin` が無い応答では
catalog の行の `source_reference` が rendering 文書そのもので `provider_feature` が空の task を root の task とみなし
([判定](../../src/devstudio/view/frameplanedit.cpp#L1059))、**何も送らずに**次の文で断ります(着地前は engine の英語の理由がそのまま出ていました):
`<task>: rendering root holds no tasks; use a feature or a render.compute node. The engine refuses an edit of it (compute_task_not_editable).`
([断りの文](../../src/devstudio/view/frameplanedit.cpp#L1066))。catalog の行が古く「編集できる」と言っていても、この判定が先です。

- **Frame Plan**: その task への接続・切断、dispatch、Delete がこの文で止まり、dispatch の帯も出ません。status 行は 90 文字で最初の
  1 文までなので、**status 行には前半の 1 文だけ**が出て、code を含む全文は status 行の tooltip にあります
  ([1 行にする規則 `oneLine()`](../../src/devstudio/view/panelhelp.hpp#L92))。rendering 文書の reference は authoring context の
  `source_reference` から取ります([root の reference を入れる所](../../src/devstudio/view/frameplanwidget.cpp#L1834))。
- **Task Editor**(Render Features の Tasks... / Frame Plan の Edit task...): root の task を選ぶと、下の notice に全文(code まで)が出て、
  宣言欄は読み取り専用、**Apply task**・**Delete task**・接続表・dispatch フォームは押せません([Task Editor の錠](../../src/devstudio/view/renderfeatureswidget.cpp#L403))。
  送る中身(add / edit / 断り)は [`computeTaskEditorRequest()`](../../src/devstudio/view/frameplanedit.cpp#L1139) が決めます。
- **新しい task は今までどおり**です。New compute / New ray tracing は `add_compute_task` で、指定した `project://` の feature に作ります。
- **分かっている穴**(着地時の記録): `origin` を持たない古い応答では、Task Editor が root の reference を知るのは
  Render Features が `get_render_features` の応答を読んだ後で、それより前は engine の「編集できない」と英語の理由のままです。
  (Q-10 の時点では `render.compute` node から来た task も同じ root の文になっていましたが、Q-12 で node の文に分かれました。)

**graph node が作った task は「Graph タブで編集」と言い、その node へ移れます(✅Q-12)。**Studio は task の錠を **origin を先に読んで**
決めます([錠の順](../../src/devstudio/view/frameplanedit.cpp#L1089)): `origin` が `"graph_node"` → root(`origin` が `"root"`、または
`origin` が無く上の判定に当たる)→ 読み取り専用(`editable: false`)→ 編集できる。Frame Plan の drag・帯・Delete も Task Editor の
Apply も、この 1 つの関数を通ります。graph node の task には、root の文ではなく engine の `read_only_reason` がそのまま出ます:
`This task is made by a render.compute node in the graph. Edit the node in the Graph tab.`

- **Frame Plan**: その task への接続・切断、dispatch、Delete はこの文で止まり、dispatch の帯も出ません。**status 行にはこの 2 文が丸ごと**
  出ます(`oneLine()` は最初の 1 文で切るので、断りに status 行用の 1 行を別に持たせています。[status 行用の 1 行](../../src/devstudio/view/frameplanedit.hpp#L32))。
  tooltip は `<task>: <上の文> Right-click the task and choose Open in Graph.` です。その task の上で右クリックすると、メニューの先頭に
  **Open in Graph** が出ます([右クリックに足す所](../../src/devstudio/view/frameplangraphics.cpp#L4122))。Q-12 の着地時は graph node の task の上でだけでした。✅GL-1 から、anchor 以外のすべての node の上に出ます。graph の node が作っていない pass では、Graph タブの畳んだ node を選びます(下の「graph の外から来た pass を畳んだ node で見る」)。✅GL-2b から、Passes / Resources / Physical plan / Decisions / Backends / Barriers のタブの行の右クリックにも出ます(下の「Frame Plan の表の行から開く」)。
- **Task Editor**: graph node の task を選ぶと、notice に上の文が出て、その横に **Open in Graph** ボタンが出ます(既存の task で
  `graph_node` を持つ行を選んだときだけ見えます)。宣言欄と Apply task などは root の task と同じく止まります。
- **Open in Graph** は Graph タブを前に出し、`graph_node` の scope を開いて node を選び、view の中央に置きます([node を見せる所](../../src/devstudio/view/graphcanvaswidget.cpp#L5391))。
  `instance` があれば root から 1 段ずつその instance へ降ります(instance をダブルクリックして降りるのと同じ)ので、パンくずと
  `Values from:` がその instance に合います。2 段下(`wrap/inner` など)では、パンくずが 2 つの definition を通り、`Values from:` は
  root の instance(`wrap`)です。path が scope に届かないときは scope を直接開き、path の最初の instance が `Values from:` の一覧に
  あればそれを選びます。
- Graph タブがまだその node を持っていない(文書を読んでいない、または古い)ときは、tab を切り替えず何も変えずに
  `The Graph tab has no node <node> in <scope> yet: refresh it.`(root scope は `the root`)を、Frame Plan では status 行、Task Editor では
  notice に出します([tab を切り替える所](../../src/devstudio/view/graphcanvaswidget.cpp#L5519))。
- **分かっている穴**(着地時の記録): tab の切り替えの test は同じ関数を `QTabWidget` で包んだもので、MainWindow 全体を RPC 越しに
  動かしてはいません。Task Editor のボタンには widget test がありません。`root` という名前の definition は root scope と見分けられません
  (engine の scope の書き方のままなので)。

Frame Planのtask詳細にある **Callable record settings** は、取得時点のCPU側実行時設定です。
同じ情報はRaw JSONの`runtime_callable_records`にも含まれます。
record indexごとにフィールド名・型・offset・値を示し、GameContextからの動的変更も含みます。
`source: runtime_cpu_settings`は次のdispatchで使う設定を意味し、GPU完了時の値のreadbackではありません。
保存する初期値は引き続きCallable Records画面で編集します。Frame Planを再取得すると観測値を更新できます。
**New compute / New ray tracing**では名前・既存shader参照・新しい`project://`保存先を設定してtaskを作成します。
保存先featureとrootへの参照追加は一緒に確定します。shaderファイルやresource自体は自動生成しません。
**Delete task**は選択した宣言を削除します。featureファイルと他の宣言は残り、依存関係が壊れる削除はcompilerが拒否します。

Tasksのdispatchフォームでは、computeのworkgroup数／RTのray数（X・Y・Z）、画像portからの寸法取得、
indirect bufferとbyte offsetを設定できます。computeは省略時の1×1×1にも戻せます。
**Update dispatch JSON**で草稿へ反映し、**Apply task**で保存します。その他のtask設定は保持します。
**View** の欄(✅LP-4b)は dispatch フォームの下に出ます。task の view family に選べる軸があるときだけで、選ぶと草稿の `view_select` だけを
書き換えます(§13.5「Studio のノードキャンバス」の「View の欄 —— Pass Editor と Task Editor」)。

Tasksの接続表は`resource_ports`のport名と型・アクセス設定、接続先resourceを表示します。
Frame Planでは **Shift＋左ドラッグ** で出力元ノードからcompute／RTタスクへ線を引くと、
そのタスクの編集画面を開きます。出力元が書くresourceだけを候補にして、portグループと接続先を選びます。
**Apply task**で保存するまでは草稿です。Escapeでドラッグを取り消せます。通常のドラッグは従来どおりノードを移動します。
⚠ **同じ dock の Graph タブは作法が違います** —— あちらはソケットからの**素の左ドラッグ**が接続で、
放した瞬間に `edit_graph_document` が 1 回飛びます(草稿も Apply もありません)。Frame Plan の
**Shift＋左ドラッグ**は編集画面を開くだけで、保存は **Apply task** です。
project presetのfullscreen/material/raster passへドラッグすると、そのpassの編集画面を開きます。
入力候補はドラッグ元の出力に絞られます。既存入力は保持され、**Add input**で追加し、保存で確定します。
適合する出力がない場合は候補なしと表示します。通常の **Edit preset pass** で開き直すと絞り込みを解除します。
グループ・自身・別graphへの接続は対象外です。共有定義やfullscreen/material/raster以外の描画passは既存の編集制約に従います。
接続図には合成後の画像・バッファ候補を表示します。左のportグループを選び、右のresourceをクリックすると接続線とJSON草稿を更新します。
宣言が見つからない接続先はunresolvedと表示します。既知の画像・バッファ間の変更は図で拒否し、詳細なABI適合性はApply task時にcompilerが検査します。
接続先を入力し、**Update connections JSON**で草稿へ反映してから**Apply task**で保存します。
同じresourceを使うportは1行にまとめ、関連するreads/writes、read_footprints、間接dispatchのbuffer参照も更新します。
port名と型・アクセス等は保持するため、groups_from/rays_fromのport指定は変わりません。
接続先の存在や型の適合性は保存時にcompilerが検査します。read_footprintsのキーが衝突する変更は草稿を変更せず診断します。
画像から寸法を取る場合はresource名ではなくport名を入力します。indirectのoffsetは4byte境界です。
不正なJSONは修正するまでフォームから上書きしません。StudioのRPCで正確に表せない整数を含む草稿は、丸めて送信せず診断を表示します。

**Frame Plan のノードも共通グラフと同じ見た目になりました(N-3)。**見出し帯に pass 名と種別、その下に GPU 計時の 1 行、
続けて**書く target の行(右端にソケット)と読む resource の行(左端にソケット)**が並びます
([組み立て](../../src/devstudio/view/frameplangraphics.cpp#L1202))。ソケットの形は resource の種類で、
`frame_plan.image`(丸)/ `frame_plan.buffer`(四角)/ `frame_plan.acceleration_structure`(菱形)/
`frame_plan.swapchain`(六角)/ `frame_plan.history`(縁の太い丸)を
[既定表の frame_plan.* 5 entry](../../src/devstudio/resources/graph_socket_styles.json#L28) が持ちます ——
**Graph タブと同じ表・同じ解決順**です([種類の判定](../../src/devstudio/view/frameplangraphics.cpp#L2244))。
辺は**ソケット中心からソケット中心**へ引かれ、対応するソケットを持たない辺(折り畳んだグループ、境界の stub、
resource を名指さない順序依存)だけがノード矩形へ落ちます([配線](../../src/devstudio/view/frameplangraphics.cpp#L291))。
辺のラベルは消えて tooltip になり、ノードを選ぶと触れていない辺が薄くなります
([不透明度](../../src/devstudio/view/frameplangraphics.cpp#L4923))。
**操作・RPC・model は変わっていません** —— ソケットはノード item の子なので、**Shift＋左ドラッグ**はソケットから
始めてもノードの本体から始めても同じ接続になります([起点の解決](../../src/devstudio/view/frameplangraphics.cpp#L4285))。
行の並びは resource の**名前順**で宣言順ではなく、長い名前は中略されます(幅の上限
[`FramePlanNodeMaximumWidth = 360`](../../src/devstudio/view/frameplangraphics.hpp#L168)。全名は同じ行の
ソケットの tooltip に `<resource> : <型>` として出ます)。shader が入力を宣言している fullscreen / raster pass の入力の行は、
✅VI-3a から名前順ではなく宣言の binding の順です(§13.5「Studio のノードキャンバス」の「ソケットは shader の宣言から出ます」)。

Frame Planの **Order nodes...** で現在のgraphのノードをUp/Downで並べ、Applyで保存できます。
データ依存とAS構築の必要順序は維持され、矛盾する並べ替えは拒否されます。
**Use dependency order**で当該graphの指定を解除します。名前付きAS同士の順序も変更できます。
画面が古い場合はFrame Planを再取得して開き直してください。順序指定に含まれるtaskを削除・改名する前には指定を解除します。
保存先はrootの`frame_node_order`で、RPCは`get_frame_node_order` / `edit_frame_node_order`です。

**シーン編集との違い:**

- **`actor_id` もセッションも要りません。**
- **undo / redo の RPC はありません。journal にも載りません。**戻すには、編集の前に `get_render_documents` で読んだ文書を `restore_render_documents` で書き戻します(✅RD-1 / RD-1b。下の「ループと分解の RPC」)。
- **CAS は SceneRevision ではなく、ルート設定ファイルの sha256 digest** です。
  **メモリ上の文書とディスク上の実物の両方**を照合します。**受付の時点**でどちらかが食い違えば `external_modification`。**受付のあと**、フレーム境界までの間に**別のチケットがメモリ上の文書を先に更新していれば `stale_source_digest`**、ディスク側が変わっていれば `external_modification` です。いずれも**何も書きません**。
- 書き込みは原子的置換で、**engine が文書を所有します。編集側はファイルに触れません。**
- **v1 は `operations` が最大 1 要素**。`op` は `add` / `remove` / `set_parameters` / `materialize` の 4 つで、
  それ以外は `edit_render_features operation op must be 'add', 'remove' or 'set_parameters'` です
  ([その throw](../../src/core/communication/renderconfigeditor.cpp#L1666))。`set_parameters` は既定が
  `scope: "root"` で、preset の feature を編集するときだけ `scope: "preset"` + `base_preset_digest` を添えます
  (engine preset のままなら `project_preset_required`)。
- **可用性の判定は engine 側**です。studio が理由を組み立てることはありません。
  モジュールの動的生成を要する feature は名指しで断られます([第6章](06_rendering.md) §6.5)。
- **no-op は runtime にもファイルにも届きません**(digest が一致すれば `no_change`)。
  同じ内容で 2 回適用しても世代は進まず、履歴も reset されません。

**編集面が存在しないプロジェクトがあります。**ルートの rendering config に
最上位 `features` 鍵が無いと、以前はサービスごと作られず 3 メソッドとも
「render config editor service is unavailable」で失敗しました。
現在は**初期化操作が `features: []` を byte-lossless に挿入する**ので、
手書きの config でも編集面が作られます。最上位が `pelican.graph` でない config(旧い legacy root)は、
MG-2b から起動時の読み込みで `render_config.legacy_root@1` になり、編集文書も同じ名前で断ります。

**挿入先は `rendering` の中です。**root は `schema: "pelican.graph"` で、
著作キー(`features` / `rendering_passes` / `pipeline` …)の在処を決めるのは
[`renderingSectionOf()`](../../src/project/renderconfigsection.hpp#L36) で、
**`rendering` オブジェクトの中**になります(読み書きの locator はこの 1 本ですが、byte-lossless の
書き込み側は同じ規則を [`isGraphRootKey()`](../../src/project/renderconfigdocument.cpp#L426) として
鍵の側から持っています。N-6 からは editor 用の `layout` も graph 側の root キーです。[第6章](06_rendering.md) §6.16)。したがって初期化は `rendering.features` に
`[]` を書き、**root 直下に `features` を生やしません**
([`initialize()`](../../src/project/renderconfigdocument.cpp#L593))。
`rendering` オブジェクトそのものが無い graph root は、書き込む場所が無いので
**名前付きで断られます** —— 受付の応答は `render_config_section_unlocated`
(**編集 op の経路だけ `{method}` が付きます** —— [renderconfigeditor.cpp:1325](../../src/core/communication/renderconfigeditor.cpp#L1731)。
pipeline の preflight([:887](../../src/core/communication/renderconfigeditor.cpp#L1276))と fork([:1753](../../src/core/communication/renderconfigeditor.cpp#L2249))は
同じ code を data 無しで返します)、
engine 内部の例外名は
[`RenderConfigSectionUnlocated`](../../src/project/renderconfigdocument.cpp#L414) です。
**読みは同じ状況で失敗しません** —— `renderingSectionOrEmpty()` が「ここには何も著作されていない」と
答えるので、`get_render_features` は空の一覧を返します。拒否されるのは編集だけです。

**適用は graph variant をまとめて再コンパイルします。**flat と preview は常に、**XR は `PELICAN_WITH_OPENXR` かつ実行中の XR セッションがあるときだけ**です。
したがって **preview を壊す候補は、feature を 1 つ足しただけでも preview 由来の理由で拒否されます。**

### グラフの装着を編集する

共通グラフ(`pelican.graph` root の `nodes` と `definitions`)は**レンダー設定と同じ 1 本のファイル**に載ります。
したがって実行モデルもレンダー設定と同じで、シーン編集とは別物です。何を書けるか(`value.*` / `render.*` /
`graph.instance` のノード語彙、`definitions`、owner の宣言)は
[第6章](06_rendering.md) と `projects/example/README.md` が正で、ここは**編集の作法**だけを扱います。

**編集の入口は 2 つ、読みは 4 つです。**

- 編集: `edit_graph_document{base_source_digest, ops[]}` と `edit_graph_instances{base_source_digest, attach[], detach[]}`。
  後者は owner 付き root instance の装着・取り外し専用で、**中では同じ `editGraphDocument()` の ops に翻訳されます**
  ([翻訳](../../src/project/graphcomposition.cpp#L2351))。検証を 2 度書かないための構造です。
- 読み: `get_graph_instances` / `get_graph_catalog` / `get_graph_values` / `get_graph_node_values`(§13.5 の読み取り表)。

#### `ops[]` と `scope`

| op | フィールド | 意味 |
|---|---|---|
| `add_node` | `node`(`id` を持つ node object)、任意の `index` | ノードを追加。同じ scope に同じ id があれば拒否。`index` は挿入位置(0〜現在の個数)、省略時は末尾 |
| `replace_node` | `node`(`id` を持つ node object) | 同じ id の既存ノードを丸ごと置換し、文書全体を検証。Undo で省略フィールドも含めて復元するために使う |
| `remove_node` | `node`(id 文字列) | ノードを削除。誰かが参照していれば拒否 |
| `set_input` | `node`, `port`, `source`(`{node, port}` ちょうど 2 鍵、✅LP-1b からは `{node, port, element}` の 3 鍵も、または `null`) | 入力の接続 / 切断。`null` は `inputs` からその port を消し、空になれば `inputs` ごと消す |
| `set_node_parameters` | `node`, `parameters`(object) | **merge ではなく置換**です。1 つだけ変えたいときも全体を読んで送り直します。空 object を送ると `parameters` が消えます |
| `set_node_label` | `node`, `label`(string) | 人が付けた表示名。識別子ではないので、**空文字が「消す」**です(専用の op は作られていません) |
| `set_layout` | `layout`(object か `null`) | その scope の editor 用の置き場所(`nodes{id: {x, y}}` / `reroutes[]{edge{node, port}, points[]}` / `ties[]{x, y, edges[]}`、N-8 からは `frame_plan{<graph>: {nodes, reroutes, ties, groups}}` も。こちらは形だけ検査)を丸ごと置換。**再 compose しない**(bytes だけ変わる)。名指した node / 辺が無ければ `graph.layout_invalid@1`(N-6) |
| `set_outputs` | `outputs`(`{"<出力名>": {node, port}, …}`) | ✅DO-1。その scope の公開の出力(root なら文書の `outputs`、definition ならその `outputs`)を丸ごと置換([適用](../../src/project/graphcomposition.cpp#L2251))。逆 op は前の `outputs` を持つ同じ op。endpoint は `{node, port}` ちょうど 2 鍵(`element` は取らない)。node はその op の時点で scope に在ること(無ければ `graph.node_missing@1`。node を足して出力にするなら `add_node` → `set_outputs`、出力から外して消すなら `set_outputs` → `remove_node` の順)。`graph.input` を指すなら port は `value`、instance / ループ以外の node なら宣言した出力 port。出力名は空でなく `/` を含まず、`[]` で終われない(ループの `p[]` の綴り)。形の誤りは `invalid_graph_document`。definition から消えた出力を外(instance の port、ループの `outputs` の `p` / `p[]` や `carry` の `from`、それを読む edge や参照側の scope の `outputs`)がまだ読んでいれば、**列の最後に** `graph.output_referenced@1` `{definition, output, node}` + `{consumer, port}` / `{scope_output}` / `{field: "outputs" / "carry"}`([検査](../../src/project/graphcomposition.cpp#L1896))。ループの分岐の body の出力がそろわなければ、最後の展開で `graph.loop_branch_interface@1`。Studio は ✅LP-4d2 から、ゾーンの右の縁の `+` でこの op を送ります(下の「縁をまたぐ線・carry・展開の表示」) |
| `add_definition` | `name`、任意の `definition`(`{nodes, outputs}`) | ✅LP-0c。definition を足す(`definition` を省けば空の body)。同じ名前があれば `graph.definition_duplicate@1` `{definition}`。`scope` は取らない |
| `remove_definition` | `name` | ✅LP-0c。definition を消す。無ければ `graph.definition_missing@1` `{definition}`、列の最後でまだ名指されていれば `graph.definition_referenced@1` `{definition, referenced_by}`。逆 op の本体は `get_graph_instances` の `definitions` から事前に読む(`base_source_digest` の CAS で同じ body だと保証される) |
| `rename_definition` | `from`, `to` | ✅LP-0c。改名し、名指している `graph.instance` の `definition` と `graph.loop` の `branches[].body` を同じ op で書き換える。`from` が無ければ `graph.definition_missing@1`(`definition` は `from`)、`to` があれば `graph.definition_duplicate@1` |

**全 op が任意の `scope` を取ります**([解決](../../src/project/graphcomposition.cpp#L1349))。既定は `"root"`(root 文書の `nodes`)、
definition 名を書くとその `definitions[<名前>].nodes` が対象です。**`root` は root scope の予約語**なので、
`root` という名前の definition は指せません。op は**順に**候補文書へ当たり、**最後に 1 回だけ展開**されます
([適用ループ](../../src/project/graphcomposition.cpp#L2330)、[最後の 1 回の展開](../../src/project/graphcomposition.cpp#L2347))。つまり `ops` は**全部通るか、文書ごと拒否か**のどちらかで、
途中まで書かれた文書は残りません。

✅LP-0c から、instance / ループの供給する port と definition の境界の一致、消した `graph.input` と definition がまだ名指されていないかの 3 つは、op ごとではなく**列の最後に 1 回**検査します(下の「ループと分解の RPC」)。

#### 名前付きの拒否

| code | いつ |
|---|---|
| `graph.node_duplicate@1` | 同じ scope に同じ id を追加した |
| `graph.node_missing@1` | 無い node を指した(正は throw 箇所 [graphcomposition.cpp:373](../../src/project/graphcomposition.cpp#L1341)。[宣言コメントの名前付きの失敗の一覧](../../src/project/graphcomposition.hpp#L169)も `40099e3f` 以降は 8 つ全部を挙げていて、この表と一致します) |
| `graph.input_unknown@1` | instance の definition が宣言していない port を `set_input` / `set_node_parameters` した |
| `graph.output_referenced@1` | 他ノードの入力、またはその scope の `outputs` が指しているノードを削除した。✅DO-1 から、`set_outputs` で消した definition の出力を外がまだ読んでいるときも(上の表の `set_outputs`) |
| `graph.input_referenced@1` | **instance がまだ値を供給している `graph.input`** を definition の中から削除した |
| `graph.nested_instance@1` | definition の中に `graph.instance` を足した(instance は root に立つもの) |
| `graph.input_scope@1` | root に `graph.input` を足した(境界 port は definition の中のもの) |
| `graph.definition_immutable@1` | id に `/` を含むノード(instance path 越しの指定)を **どの op でも**指した(`{reason: "instance_path"}`)。root scope の op が definition の中にいるノードを名指したときも同じ code で、`{definition}` が「どの scope を開けばよいか」を返します([`locateNode()`](../../src/project/graphcomposition.cpp#L1327)) |
| `graph.definition_duplicate@1` / `graph.definition_missing@1` / `graph.definition_referenced@1` | ✅LP-0c。definition の op(上の表) |
| `graph.loop_*@1` ほか LP 系の code | ✅LP-1a 以降。ループと、グラフで書く pass の形の拒否。一覧は [第6章 §6.18](06_rendering.md) の「名前付きの拒否の一覧」 |

`graph.input_referenced@1` が要るのは、**その値が編集中の body の外にある**からです —— definition を開いている
キャンバスからは、その port に値を入れている instance が見えません([走査](../../src/project/graphcomposition.cpp#L1461))。
op の形が壊れているだけのもの(`ops` が配列でない、`source` の鍵が違う、未知の `op` 名)は名前付きにならず、
`invalid_graph_document` として理由の文字列で返ります。

#### シーン編集との違い(こちら側です)

- **`actor_id` もセッションも要りません。**
- **scene journal とは別の履歴です。**Graph タブは成功した直近100操作を保持し、
  Undo / Redo ボタンとキャンバスの Ctrl+Z / Ctrl+Y（または Ctrl+Shift+Z）で逆編集・再編集を送ります。
  保存済み文書の digest が別の編集で変わった場合は履歴を破棄します。✅OT-1 から、engine の再起動(Studio から見れば再接続)では破棄しません。
  再接続の後に読んだ文書の digest が履歴を作った文書と同じなら Undo / Redo はそのまま効き、違えば破棄します([`GraphCanvasModel::clearContext()`](../../src/devstudio/model/graphcanvasmodel.cpp#L568))。
  別の project を開くと、digest が同じでも破棄します([`GraphCanvasModel::forgetHistory()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2199))。送信中だった編集は engine と一緒に捨てます。
  接続が切れている間は Undo / Redo を押せません。OT-1 の前は再接続のたびに破棄していたので、graph の編集が求めた再起動(下の「engine の再起動が要る graph の編集」)の後も Undo が消えていました。
  シーン・Features・Pass Editorを横断する統一Undoではありません。RPCからも同じ編集opを使えます。
- **CAS は `SceneRevision` ではなく `base_source_digest`** —— 文書全体の sha256 です。受付でずれていれば
  `external_modification`、受付のあとフレーム境界までの間に別のチケットが先に書けば `stale_source_digest`。
  どちらも**何も書きません**。
- **1 操作 = 1 op 列 = 1 チケット**。同じフレームに 2 本目を投げても、後から来た方が CAS で落ちます。
  結果は `get_edit_result`(ヘッドレスなら `step_frame` の `edit_results[]`)で受け取ります。
- 書き戻しは root の `nodes` が常に、`definitions` は**変化したときだけ**です([definitions の書き戻し](../../src/core/communication/renderconfigeditor.cpp#L2438))。✅DO-1 から、root の `outputs` も変化したときだけ書き戻します([root の outputs の書き戻し](../../src/core/communication/renderconfigeditor.cpp#L2442))。
  root だけを触った編集は、definition のバイト列を 1 文字も動かしません。

⚠ **`add_node` / `remove_node` で owner 付きの `graph.instance` を直接足す・消すことはできます。**
そのとき `edit_graph_instances` が持っている 2 つのガード(owner を持つ root instance であることの確認)を
迂回します。名前付きの拒否は増やさず記録する、という裁定です(Studio はこの経路を使いません)。

#### 「曲線で制御」が作るノードの集合

Inspector の導線(次節)は専用の RPC を持たず、**普通の `ops` を 1 回送るだけ**です。作られるのは次の 3 ノードと
1 本の接続で、正は [`curveControlOps()`](../../src/devstudio/model/graphattachmentmodel.cpp#L392) です。

1. `value.input{parameters:{name:"scene_time"}}` —— シーンの時計。**同じ scope に既にあれば作り直しません**
2. `value.playback{inputs:{elapsed: <上の value>}}`
3. `value.curve{inputs:{time: <playback の value>}, parameters:{keys:[{time:0,value:v},{time:1,value:v}]}}` ——
   **`v` はその port が今持っている値**です。曲線に変えた瞬間に数字が動かないように、2 点とも現在値で始まります
4. `set_input{node: <instance>, port, source:{node: <curve>, port:"value"}}`

曲線そのものの編集に専用 UI はありません。`keys` を書き換える `set_node_parameters` を送ります
(Graph タブの値の帯からも同じ op が出ます)。

### ループと分解の RPC —— engine の契約(✅LP-0a〜LP-Vb / RD-1 / RD-1b)

共通グラフのループ・グラフで書く pass・view の軸(書き方は [第6章 §6.18](06_rendering.md))に伴って、RPC に足された分の**契約だけ**をまとめます。Studio での見え方と操作(ループゾーン、分岐表、Decompose into loop、View の欄、undo)は Studio の節が扱います。

#### `get_frame_plan` に足されたもの

params は `{}` のほかに `{loop_elements: {node, offset?, limit?}}` を受けます(下の paging)。

| 場所 | 鍵 | 中身 |
|---|---|---|
| 最上位 | `loops[]` | ✅LP-1a。合成した graph の `graph.loop` ごとに `{node, tier, count, elements[{element, index, branch}], unmatched_branches}`。回が 0 のループも載り(`count: 0`)、ループが 1 つも無ければ鍵ごと無い。`tier` は `"expand"` / `"draw_loop"`、`unmatched_branches` はどの要素も選ばなかった分岐の 0 始まりの番号 |
| `loops[]` の draw_loop | `pass`・`total`・`offset`・`limit` | ✅LP-3a / LP-3b。`pass` はその draw_loop が描く `render.material_pass`。`elements[].element` は material の照合の鍵(文字列。第6章 §6.18)で、今の世代に登録された material を登録順に並べる。`over` / `where` の外の material は要素にならない。elements は 1 ページ分だけで、`total` が全件数(`count` も全件数) |
| node と resource | `loop_origin` | ✅LP-1a。ループが展開した pass・compute task・target・buffer に `{loop, element, node}`(node は body の definition の中の id)。Studio は展開名を分解せずにこれを読む |
| node | `origin` | ✅GL-2a。graph の node が作った pass・compute task(root の node・instance の中の node・ループの回・`pass_replacements` の置き換え先)に、作った node の `{graph, node, definition?, instance_path?, loop?, loop_branch?, element?}`。preset・feature・rendering 節の pass には付かない(preset が `engine://` だと graph の pass も preset の pass も `source: "engine"` なので、これで見分ける)。`graph` は rendering root 文書の参照(`get_compute_task_authoring` が node の task に返す `source_reference` と同じ値)、`node` はその scope に書かれた id、`definition` は node を持つ definition(root の node には無い)、`instance_path` は置いた instance(末尾の `/` は無い。root とループの body には無い)、`loop` / `loop_branch` / `element` はループの回だけ(`element` は `loop_origin.element` と同じ)([組む所](../../src/core/renderingpass/frameplanner.cpp#L1639)) |
| node | `replaces` | ✅GL-2a。feature の pass・task の代わりに入った node に `[{feature, pass}]`(instance が名指した順。1 つの node が複数を置き換えうるので配列)。ループでは、置き換えた pass と同じ pass family の回にだけ付く |
| 最上位 | `diagnostics` / `deactivated_graph_nodes` | ✅GL-2a。compiled pipeline の診断 `[{kind, subject, detail}]`(kind は `graph_variant_selected` / `pipeline_preset_resolved` / `feature_excluded` / `graph_node_deactivated`)と、変種で非活性にした graph の node `[{node, feature, reason}]`(reason は `target_ref` / `insert` / `order` / `pass_replacement` / `loop_output`。第6章 §6.18)。compiled pipeline のある plan では空でも鍵がある |
| `physical_target_plan` の `resources[]`・`alias_groups[]`・`decisions[]`・`backend_selection.candidates[]` | `from_nodes` | ✅GL-2a。その要素が関わる frame plan の node 名を plan の順で。resource は読む・書く・履歴を読む・attachment にする node、alias group はメンバーの resource の和、decision(最上位・`lowering_graph`・`planning_opportunities`・`backend_selection` の 4 か所)は `subject` を node → resource → alias group → scope の順で引く(lowering の node は元の node へ開く)。graph 全体の決定は `[]`。backend の候補は failures / diagnostics の `subject` から引くが、実装者の報告では実物で全部 `[]`(候補の拒否の `subject` は候補の名前のため)([`appendFramePlanPhysicalNodeReferences()`](../../src/core/renderingpass/frameplanner.cpp#L2183)) |
| render / compute / snapshot_copy の node | `view_select` | ✅LP-Va。宣言の `{<軸>: <値>}` |
| 同上 | `view_axes` | ✅LP-Va2。node の family の軸 `[{axis, length, runtime, labels?}]`。並びは provider の index の順(外側が先)、`length` は今の世代の長さで実行時の軸は `null`・`runtime: true`、`labels` は cube の `face` だけ(`+X` 〜 `-Z`)。`$main`・`$reflection/planar`・provider の無い family は `[]` |
| node | `no_selected_view: true` | ✅LP-Va2。直近に公開したフレームで `view_select` に一致する view が 1 つも無かった node にだけ付く。公開前と、走った node には鍵が無い |

**draw_loop の要素の paging(✅LP-3b)。**`{loop_elements: {node: "<graph.loop の id>", offset?, limit?}}` を渡すと、その draw_loop の `elements` が `[offset, offset + limit)` になります。`offset` の既定は 0、`limit` の既定は 256 で範囲は 1〜4096。名指さなかった draw_loop は先頭の 256 件です([`pageDrawLoopElements()`](../../src/project/materialdrawloop.cpp#L272))。形の誤りは JSON-RPC の invalid params、`node` が draw_loop でなければ同じ invalid params に `data: {code: "draw_loop_unknown", node}` が付きます。

#### `edit_preset_pass` の `view_select`(✅LP-Va2)

`edit_preset_pass`(root pass の編集を含む)は、どの pass の type の置換 payload でも `view_select` を受けます。**省略は保持、`null` は member の削除、値は置換**です。値は compiler と同じ規則([`parseRenderViewSelect()`](../../src/core/renderingpass/renderingpassjsonhelpers.cpp#L429))で読まれ、形の誤りは `render_pipeline_preflight_failed` で、message はその parse の文(`view_select must be a non-empty object of axis names to non-negative integers` など)で終わり、文書は変わりません。軸が family にあるかどうかは組み直しの検査(`graph.view_select_*@1`)です。

#### `edit_graph_document` の definition の op(✅LP-0c)と、ループの編集

上の「`ops[]` と `scope`」の表の `add_definition` / `remove_definition` / `rename_definition` の 3 つで、ループの body になる definition を作れます。

- definition の op は文書全体に当たるので `scope` を取りません(`"root"` と null 以外を書くと断る)。名前は node の id と同じ規則(空でなく `/` を含まない)で、`"root"` は op の `scope` と紛れるので断られます。`add_definition` の `definition` は `{nodes, outputs}` を持ち、中に `graph.instance` があれば `graph.nested_instance@1`、`graph.loop` があれば `graph.loop_nested@1`、id が重なれば `graph.node_duplicate@1` です。
- `rename_definition` は、その definition を名指す `graph.instance` の `definition` と `graph.loop` の `branches[].body` を**同じ op の中で**全部書き換えます。
- **列の最後の検査。**次の 3 つは op ごとではなく、列を全部当てた後の候補で 1 回だけ検査します([`runDeferredCheck()`](../../src/project/graphcomposition.cpp#L1951))。そのため「definition に `graph.input` を足す」と「ループや instance の `inputs` に足す」を、どちらの順でも 1 つの列で送れます。
  - 残った instance / ループが、definition に無い port を供給している → `graph.input_unknown@1`
  - 消した `graph.input` を、まだ誰かが供給している → `graph.input_referenced@1`
  - 消した definition を、まだ誰かが名指している → `graph.definition_referenced@1` `{definition, referenced_by}`
  op 自身が別の理由で断られたときは、その op が記録した検査を先に当てるので、1 op だけの列の結果は LP-0c の前と同じです。
- `graph.loop` は、`graph.instance` と同じく body の `graph.input` を `inputs` で供給する node として数えます(全分岐の body が対象)。
- `set_input` の `source` は `{node, port}` のほかに `{node, port, element}` を受けます(✅LP-1b。他のループの `p[]` の 1 回分を読む edge。`element` は整数か `{"$expr": …}`)。`element` が指す先と式は、最後の展開で検査されます。
- ループの形の拒否(`graph.loop_*@1`・`graph.definition_role@1`・`graph.input_index_invalid@1` など)は、編集の最後の展開で名前付きで返ります。一覧は [第6章 §6.18](06_rendering.md) の「名前付きの拒否の一覧」です。

#### `decompose_material_pass`(✅LP-3b)

rendering 節(または fork 済みの preset)の一体版の material pass を、それが意味するグラフの node に書き換えます。

- params: `{base_source_digest, base_preset_digest?, pass}`。`base_preset_digest` は preset root で preset の pass を分解するときに必須、direct root では書くと断られます(どちらも 64 桁の小文字 hex)。
- 1 つの ticket で、pass を rendering 節(または preset の `config`)から外し、root の graph に次を足します([`decomposeMaterialPass()`](../../src/project/materialpassdecompose.hpp#L74)):
  - `render.material_pass`(id は pass 名。field はそのまま parameters に。`material_filter` は下のループの `where.tags` へ移る)
  - attachment ごとの `render.target_ref`(同じ target を指すものが既にあれば使い回し、無ければ `<target>_ref`)
  - draw_loop の `graph.loop`(id `<pass>_draws`。`over.draw_items.contract` は pass の contract、`order` は `phase_default`、else の 1 分岐)と、その body の definition(名前 `<pass>_draw`。`draw.material` 1 つ、pass に `material_variant` があれば `draw.variant`)
  - 置き場所: 直後の pass の `before`(無ければ直前の pass の `after`)に **`declared: true`** を付けた `insert`。feature の合成より前に宣言の列へ置くので、feature が名指す pass(`rt_shadow_mask` の `after` や `pass_overrides` の対象)を分解しても合成が通ります。family に 1 本しか無ければ `insert` は付かず、family の末尾です。
- 受け付けると ticket の応答に `decomposed: {pass, family, insert, loop, definition, target_refs, added_target_refs, preset_reference?}` が付きます。
- 拒否:
  - engine の preset の pass は `project_preset_required` `{pass, preset}`(先に fork する)
  - 分解できない pass は `material_pass_not_decomposable` `{pass, reason, …}`。`reason` は `not_found`(rendering 節にも preset にも無い)/ `graph_pass`(既に graph の `render.material_pass`)/ `feature_pass`(feature の pass。`feature` が付く。feature の `pass_replacements` で置き換える)/ `not_material`(`type` が付く)/ `contract_unsupported`(`contract` が付く。legacy の G-buffer pass)/ `field_unsupported`(`field` が付く)/ `attachment_unsupported`(`socket` が付く。`{target, subresource}` の attachment、depth が無い、color の枚数が socket と合わない)
  - 足す id や definition の名前が既に使われていれば `graph.node_duplicate@1` `{node, scope: "root"}` / `graph.definition_duplicate@1` `{definition}`
  - digest のずれは `external_modification`(preset の digest がずれたときは `expected_preset_digest` / `provided_preset_digest`)
- 逆の操作の RPC はありません。戻すのは下の `restore_render_documents` です。

#### 同梱の分解版を入れる —— `get_graph_templates` / `apply_graph_template`(✅GT-1)

engine は、feature の一体版が描くものをグラフのループで描き直した雛形を `engine://graphs/` に同梱しています。bloom・cube capture・planar reflection の mip chain・directional の影の cascade の 4 つです。雛形の `decomposes` が、どの feature の分解版で、入れるときに何をするかを言います。engine の合成は `decomposes` を読みません。読むのは下の `apply_graph_template` だけです([`readGraphTemplateDecomposes()`](../../src/project/graphtemplate.hpp#L67))。

**`get_graph_templates {}` → `{templates: [{reference, title, description?, decomposes, document}]}`**([`RenderConfigEditorService::getGraphTemplates()`](../../src/core/communication/renderconfigeditor.cpp#L2661))

- 並びは engine の資源の登録順(`bloom_loop` / `cube_capture_loop` / `planar_mip_chain_loop` / `shadow_cascade_loop`)です。雛形は build 時の表ではなく、同梱の文書そのものを読みます。
- `title` は文書に無ければファイル名から作ります(`_` を空白にし、先頭を大文字にする。`bloom_loop.json` → `Bloom loop`)。`description` は文書にあるときだけ付きます。`document` は雛形の全文です。
- `decomposes` は `{feature, remove_feature?, pass_replacements?, shader_defines?}` です。`remove_feature: true` は feature を外す(bloom)、`pass_replacements` は feature の instance を残してその pass / task を graph の node に置き換える(cascade の `{"shadow_depth": "shadow_cascades"}` など)、`shader_defines` は feature が parameters から全 shader に与えていた define を rendering 節に足す、です。

**`apply_graph_template {base_source_digest, base_preset_digest?, template}`**([`RenderConfigEditorService::applyGraphTemplate()`](../../src/core/communication/renderconfigeditor.cpp#L2680))

- `template` は `engine://graphs/<名前>.json`。digest はどちらも 64 桁の小文字 hex です。`base_preset_digest` は、project の preset がその feature を列挙しているときに必須、direct root で渡すと invalid params です。
- 1 つの ticket で次を行い、preflight(変種を含む)と組み直しを通してから書きます。
  - 雛形の `definitions` と `nodes` を root の graph へ写す。名前が既にあれば `graph.definition_duplicate@1` `{definition}` / `graph.node_duplicate@1` `{node, scope: "root"}`
  - rendering 節と preset の `config` の `features` にあるその feature を、`remove_feature` なら外し、そうでなければ `pass_replacements` を instance に書く(同じ置き換えが既にあれば残す)
  - `shader_defines` のうち rendering 節にまだ無いものを足す
- 受け付けると ticket の応答に `applied: {template, feature, feature_documents, removed_feature, pass_replacements, added_shader_defines, definitions, nodes}` が付きます。commit の `operation` は `"apply_graph_template"` で、commit 結果の `documents`(下)で `restore_render_documents` の undo に乗ります。
- 拒否(雛形の手順のものは `template` の field が付く):
  - `graph_template_unknown` `{template}`(engine が同梱しない参照)
  - `graph_template_invalid` `{reason}`(雛形の `decomposes` が無い・feature を名指さない・型が違う)
  - `graph_template_feature_absent` `{feature}`(rendering 節にも preset にもその feature が無い)
  - `graph_template_pass_replacement_conflict` `{feature, pass, current, requested}`(instance が同じ pass を別の node で置き換えている)
  - `project_preset_required` `{feature, preset}`(engine の preset の feature。先に fork する)
  - `graph.node_duplicate@1` / `graph.definition_duplicate@1`(上)、digest のずれは `external_modification`、組み直しに通らなければ `render_pipeline_preflight_failed`(`method: "apply_graph_template"`)

#### render 文書を丸ごと読む・戻す —— `get_render_documents` / `restore_render_documents`(✅RD-1 / RD-1b)

render の編集(`edit_preset_pass`・`add_authored_pass`・`edit_render_targets`・`decompose_material_pass` …)の undo を、op ごとの逆 RPC ではなく「編集の前の文書を丸ごと戻す」形で行うための 2 本です。

**`get_render_documents {references?}` → `{documents: [{reference, kind, digest, content}]}`**([`getRenderDocuments()`](../../src/core/communication/renderconfigeditor.cpp#L4447))

- `references` を省くと、今の構成が書きうる文書を全部返します: root(`kind: "root"`)、project の(fork 済みの)preset(`"preset"`)、root・preset・合成結果・pass と資源の出所が名指す project の feature 文書(`"feature"`)。どれも project の中の通常のファイルで、中身がその種類の schema(`pelican.render_pipeline` / `pelican.render_feature`)のものだけです。`content` は bytes そのまま、`digest` はその sha256(64 桁の小文字 hex)。
- `references` で名指したものが上に無ければ `render_document_not_writable` `{method, reference, reason}`。`reason` は `engine_document`(`engine://`)/ `fragment_address`(`#` を含む)/ `outside_project` / `not_render_document`。
- ✅RD-1b: 名指した参照が project の中の path に解決され、そこに今ファイルが無い(編集が消した、undo で作り直す予定の文書など)ときは `render_document_missing` `{method, reference}`。その文書を `restore_render_documents` で作り直すときの `base_digest` は `null` です。

**`restore_render_documents {documents: [{reference, base_digest, content}]}`**([`restoreRenderDocuments()`](../../src/core/communication/renderconfigeditor.cpp#L4516))

- 3 つの鍵は常に書きます。✅RD-1b から **`content: null` は「その文書を消す」**、**`base_digest: null` は「今その文書は無い」**(作り直すときの CAS)を意味します。両方 `null` は形の誤り(invalid params)です。同じ文書を 2 度名指すのも形の誤りです。
- **書ける範囲**: 今の構成が書く文書(上の `get_render_documents` の範囲)に加えて、✅RD-1b から**今の root と、戻す先の root(とそれぞれの preset)が名指す** project の文書(authoring の fragment、project の preset、materialize / import が作る feature 文書)を作る・消す対象にできます。範囲の外は `render_document_not_writable`(reason は上と同じ)。
- ✅RD-1b の追加の拒否: 作る・消すのが予約の場所 —— `.pelican/`・`code/`・`assets/`・`scenes/`・`input/`・`ui/`・`packages/`・`build/`・`build-*/`・`out/`・`output/` と、`project.json` の `basic_config` が指す文書のディレクトリ(rendering config の root を含むディレクトリは除く)—— の下なら `render_document_not_writable` `{reason: "reserved_location", location}`([`reservedProjectLocation()`](../../src/core/communication/renderconfigeditor.cpp#L4346))。既にある文書の置き換え(RD-1 の形)はこの制限を受けません。root を消そうとすると `{reason: "root_document"}` です。
- **CAS**: 名指した全文書の今の digest が `base_digest` と一致したときだけ書きます(root は engine が持つ文書とディスクの両方)。root を名指さなくても、root がディスクで変わっていれば断ります。ずれは `external_modification` `{method, reference, expected_digest, provided_digest, mismatches[]}` で、`mismatches` に全部の `{reference, expected_digest, provided_digest}` が並びます。✅RD-1b から、ここでの `null` は「文書が無い」を表します(`base_digest: null` なのに在る、など)。
- 通ったら、編集と同じ preflight(変種を含む)と組み直しを書く前に通し、作る・置き換える・消すを**1 つの transaction** で行って ticket を返します。組み直しに通らない中身は `render_pipeline_preflight_failed` `{participant, method, reference}` で、何も書きません。`material_route_default` が変わる restore の commit 結果には、`add_authored_pass` と同じく `restart_required: true` が付きます。
- gate が閉じていれば `gate_closed`。

**commit 結果の `documents`(✅RD-1)。**render の編集の ticket の commit 結果(`get_edit_result` / `step_frame` の `edit_results[]`)に `documents: {<reference>: <sha256>}` が付きます。その commit が書いた文書だけで、作った文書は digest、消した文書は `null`、何も書かなかった(`no_change`)なら `{}` です([`writtenDocumentDigests()`](../../src/core/communication/renderconfigeditor.cpp#L4074))。

**undo の手順(契約から言えること)。**編集の前に `get_render_documents` で読み、編集の commit 結果の `documents` で「何が作られ・消されたか」を知り、戻すときは: 置き換えた文書は読んだ `content` と、`documents` の digest を `base_digest` にして送る / 作られた文書は `content: null`・`base_digest: <documents の digest>` で消す / 消された文書は読んだ `content` と `base_digest: null` で作り直す。RD-1 の時点で残っていた「ファイルを作る・消す編集(`add_authored_pass` / `remove_authored_pass`、fork、materialize、import)は戻せない」穴は、RD-1b で閉じました。

#### 変種と組み直しの拒否の形(✅LP-0a / LP-0d)

- 組み直し(ticket の commit)で graph が名前付きで断った拒否(`GraphRegistryError`。`graph.target_ref_unknown@1` など)は、`render_pipeline_preflight_failed` に畳まれず、**その code のまま**、`ticket` と `participant` の横に error_fields を並べて返ります(✅LP-0a)。
- 変種(preview / xr)の preflight で断られたときも、graph の名前付きの拒否と、`<code>: <fields>` の形で運ばれる拒否(`material_route_unserved@1` や `graph.view_select_*@1` など)は**その code のまま**で、fields の横に `variant`(`"preview"` / `"xr"`)が付きます(✅LP-0d。[`preflightGraphVariants()`](../../src/core/communication/renderconfigeditor.cpp#L995))。それ以外の拒否は今までどおり `render_pipeline_preflight_failed` + `variant` です。
- 組み直しで回ごとの node が名指された拒否には `loop_origin: {loop, element, node}` が付きます(✅LP-1b)。
- **view family の大きさの不一致(✅VF-1)**: 二次の view family(`$main` 以外)の raster pass は、書く target の大きさが全部同じでなければなりません(renderer が family の view の大きさを pass の target から決めるため)。組み直しで Vulkan の変種を compile するとき、view_select の検査の直後に、family ごとに宣言順で最初の raster pass が書く target を基準にして比べ、違う pass があれば `render.view_family_extent_mismatch@1` で断ります([`validateRenderViewFamilyExtents()`](../../src/core/renderer/viewfamilyproviderregistry.hpp#L184))。
  - fields は `{family, pass, target, extent, expected_pass, expected_target, expected_extent}` で、ループが複写した pass なら `loop_origin` / `expected_loop_origin` が付きます。`extent` は固定の大きさなら `[w, h]`、画面相対なら `{"output_scale": s}` です。固定と画面相対は、結果の大きさが同じでも不一致です。宣言の無い attachment(swapchain など)は比べません。
  - 編集の拒否では上の `graph.view_select_*@1` と同じく**その code のまま** fields を横に並べて返り、文書は変わりません([`isCarriedRefusalCode()`](../../src/core/communication/renderconfigeditor.cpp#L70))。読み込みもこの拒否で止まります。以前は cube の face のループの body に画面の大きさの pass を置くと、player が renderer の検査(`render view family '$capture/cube' spans multiple raster extents`)で止まっていました。renderer の検査は最後の砦として残っています。

### Studio のノードキャンバス(Graph タブ)

**置き場所**: `View > Panels > Frame Plan` の dock(`pelican.framePlanDock`)にある 3 枚目のタブ「Graph」です
([タブの追加](../../src/devstudio/view/mainwindow.cpp#L369))。dock は増えていません。起動時から開いておきたい場合は
`pelican_studio --project <dir> --tab graph`([第10章](10_tools.md) §10.7)。

**上段の操作列**(左から): `Scope:` のコンボ(`root` と definition 名)、その右のパンくず、領域の `Show:`(✅GL-1。下の「graph の外から来た pass を畳んだ node で見る」)、右端に
`Values from:` のコンボ、`Refresh`([構築](../../src/devstudio/view/graphcanvaswidget.cpp#L3661))。
その下に必要なときだけ 1 行の notice が出ます。本体は左がキャンバス、右が選択ノードの詳細表(`Field` / `Value`)で、
詳細表の上に `Label:` の入力欄・**Rename**・**Add node...** が並びます(選んだ node が `pelican.ui` の sink なら、その下に「UI node」の欄。✅Q-8、下の段落。`audio.playback` なら Sound / Bus / Wrap の欄。✅Q-10、下の段落)。

1. **scope を選ぶ** —— `root` は文書そのもの、definition 名はその本体です。root の `graph.instance` ノードを
   **ダブルクリック**すると、その definition の中へ降ります。パンくずで戻ります。
2. **値を重ねる** —— `Values from:` で owner を持つ root instance を選ぶと、その instance の入力 port に
   `get_graph_values` の値と供給元が重なり、**出力 port には `get_graph_node_values` の公開値が帯で重なります**(✅AG-3b。definition の中に降りても、その instance 経由の値が出ます)。`(none)` で外れます。`Live` で 250 ms ごとに読み直し(同時に飛ぶ読みは 1 組まで)、`Timeline` 帯を開くと選択ノードの数値 port(≤4)と owner の animation の phase / progress が貯まり、左ドラッグで `set_time` → `render_frame` の scrub です。
3. **ノードを読む** —— ノードは見出し帯 + ソケット行です。見出しにはラベル(無ければ id)と、右に小さく型名(`op`)。
   見出し左の三角でソケット欄を**折り畳め**ます(tooltip は `Fold the sockets away` / `Show the sockets`)。
   折り畳んだノードでは、辺は見出し帯の左右中央の 1 点に集まります。辺の tooltip は
   `<from>.<port> -> <to>.<port>  (select and press Delete to cut)` です。
4. **ソケットの形が型です** —— 色だけでなく形(`circle` / `diamond` / `square` / `hexagon`)でも型を表します。
   決め方は 1 か所で、**モジュールが自分の型 schema に書いた `display{}` → Studio の既定表 → traits → `unknown`**
   の順([解決順](../../src/devstudio/view/nodegraphics.hpp#L151)、入口は [`socketStyleFor()`](../../src/devstudio/view/nodegraphics.hpp#L153))。
   既定表の中はさらに 2 段で、**型名の完全一致 → `prefixes` の前方一致**です([nodegraphics.cpp:126](../../src/devstudio/view/nodegraphics.cpp#L135)) ——
   だから `pelican.render.` のような名前空間まるごとに 1 行で形を与えられます。既定表は
   [`graph_socket_styles.json`](../../src/devstudio/resources/graph_socket_styles.json#L1)(`pelican.studio.socket_styles` v1)で、
   **新しい型に形を与えるのに C++ を触る必要はありません** —— この JSON に名前を 1 行足すだけです。
5. **右の詳細表** —— `label` / `type` / `domain` / `evaluation` / `id` / `sink` と、あれば `definition` /
   `parameters` / `inputs` / `outputs`([組み立て](../../src/devstudio/view/graphcanvaswidget.cpp#L5069))。
   port の型が宣言されていなければ `(not declared)`、カタログをまだ取れていなければ `domain` / `evaluation` が
   `(catalog not fetched)` です。入力行の tooltip は、その port を**カタログが宣言した**(`declared by the catalog`)のか、
   **文書から読んだ**(`read from the document`)のかを区別します。

**編集はその場で反映されます(Apply ボタンはありません)。**

| 操作 | 送る op |
|---|---|
| 出力ソケットから入力ソケットへドラッグ | `set_input{source}` |
| 辺を選んで `Delete` / 入力ソケットから空所へドラッグ | `set_input{source: null}` |
| 値の帯(未接続で値のある port)をクリックしてその場編集 | `set_node_parameters`(既存の `parameters` を読み、1 鍵だけ差し替え) |
| 見出しをダブルクリック / `Label:` + **Rename** | `set_node_label` |
| node を drag / 辺を double-click(reroute)/ 制御点を選んで **Tie together** / **Untie** / **Auto layout** | `set_layout`(300 ms debounce で 1 回。N-6) |
| 右クリックまたは **Add node...** でパレットから選ぶ | `add_node`(必要なら複数) |
| ノードを選んで `Delete` | `remove_node` |

削除や切断では、不要になったノード位置・接続線の配置も同じ編集で除去します。
Undoはノードの宣言順・設定・接続と配置を一緒に復元します。`replace_node`は同じIDのノード全体を
検証付きで置換し、`add_node`の任意`index`は削除前の宣言位置へ戻すために使えます。
文字入力中のUndoは入力欄の履歴を使い、保存処理中のグラフへ重ねてUndoを送りません。

型が合わないソケットには吸着しません —— **どちらかの型が宣言されていなければ通し、両方宣言されていれば
同じ型のときだけ**通します([判定](../../src/devstudio/model/graphcanvasmodel.cpp#L2257))。

⚠ **2 つの注意。**
(1) **必須の scalar 入力を持つ op を足すと、種の定数も一緒に足されます。**未接続の必須入力を持つ文書は engine が
受け取らないので、パレットは必須の scalar 入力ごとに `value.constant` を**同じ ops 列で先に**足し、そこへ繋いだ
状態のノードを作ります([生成](../../src/devstudio/model/graphcanvasmodel.cpp#L2404))。1 操作 1 チケットは保たれます。
後で本物の producer を繋いでも、**この定数は自動では消えません**(手で消します)。
例外は `audio.playback` の `time` で、定数ではなくシーンの時計に繋ぎます(✅Q-11。下の「`audio.playback` の欄」)。
(2) **必須入力の切断は送られ、engine が断ります。**Studio は黙って定数を挿し直したりしません ——
`set_input{null}` をそのまま送り、`unconnected graph input` の拒否を notice に出します。値を持つ instance の port は
(戻り先の `parameters` が残っているので)切断できます。

**`pelican.ui` の sink の node を選ぶ(✅Q-8)**: palette の `ui.value` / `ui.visible`(GP-3 の `pelican.ui` module の 2 つの sink)は、
書き込む UI node の path を parameter `node` に持ちます。

- **置くと `node` が入っています。**palette で置くと、project の UI 文書の最初の path が `parameters.node` に入ります
  ([置くときに node を入れる所](../../src/devstudio/model/graphcanvasmodel.cpp#L2611))。`ui.value` は最初の gauge(engine が値を書けるのは
  gauge だけで、他の widget は `ui.property_mismatch@1` で断られます)、`ui.visible` は文書の最初の node(root)です。UI 文書が
  読めなければ `node` を書かずに送り、engine の名前付きの拒否になります。Q-8 より前は `node` が空のまま出て、置くと必ず
  `ui sink requires a node path` で断られていました。
- **詳細表の上に「UI node」の欄が出ます。**選んだ node が `ui.value` / `ui.visible` のときだけ、打つこともできる一覧
  (`pelican.graphCanvasUiNode`)が出ます([UI node の欄を合わせる所](../../src/devstudio/view/graphcanvaswidget.cpp#L4750))。項目は UI 文書の
  node の path(root からの id を `/` で繋いだもの)で、`ui.value` では gauge だけです
  ([一覧を gauge に絞る所](../../src/devstudio/model/graphcanvasmodel.cpp#L474))。一覧から選ぶか、打って Enter で `set_node_parameters`
  1 回です。一覧に無い path はそのまま送られ、engine が名前付きで断ります。
- **一覧の出所は project の UI 文書です。**UI の木を返す RPC は無いので、Studio が `project.json` の
  `basic_config.ui_config_json` が指す文書を engine と同じ project path resolver で読みます
  ([UI 文書を読む所](../../src/devstudio/model/graphcanvasmodel.cpp#L379))。欄を出す直前と palette を開く直前に読み直すので、開いた後に
  文書を書き換えても一覧はそれに従います。読めないときは欄の placeholder が理由(`project.json names no UI document
  (basic_config.ui_config_json)` など)を言います。

**`audio.playback` の欄(✅Q-10)**: AU-1 で `audio.playback` に入力と parameter が増えました。

- **増えた port は engine の catalog から来ます。**`position`・`source_begin`・`source_end`・`speed`・`delay` と `events` は Studio が
  他の op と同じく catalog から描き、必須は `time` だけです([engine の宣言](../../src/project/graphmodule.cpp#L587))。`events` は任意の
  入力なので、繋いでいなくても Studio は問題として扱いません。任意の数値の port に値を入れるのは、今は繋ぐことでだけです(既存の挙動)。
- **port でない 3 つの parameter は、詳細表の上の欄です**(`pelican.graphCanvasAudioRow`。選んだ node が `audio.playback` のときだけ。
  [欄を合わせる所](../../src/devstudio/view/graphcanvaswidget.cpp#L4816)):
  - **Sound**(`pelican.graphCanvasAudioAsset`、打つこともできる一覧)= parameter `asset`。一覧は project の中の `.wav` / `.ogg`
    (大文字小文字は問わない。`.` で始まる folder は見ない)を `project://...` で並べたものです([音声を探す所](../../src/devstudio/model/graphcanvasmodel.cpp#L412))。
    音声の一覧を返す RPC は無いので Studio が folder を歩きます。project を開いたとき、選ぶ node が変わったとき、palette を開く直前に探し直します。見つからなければ
    placeholder が理由(`no .wav or .ogg file in the project` など)を言います。打った path はそのまま送り、中身は engine が確かめます。
  - **Bus**(`pelican.graphCanvasAudioBus`、選ぶだけ)= `se` / `bgm`。engine が受け取るのはこの 2 つだけで、それを一覧にする RPC が無いので、
    **一覧は Studio が持っています**([bus の一覧](../../src/devstudio/model/graphcanvasmodel.cpp#L459))。文書に別の値があれば、それも一覧に出します(engine が名前付きで断ります)。
  - **Wrap**(`pelican.graphCanvasAudioWrap`、選ぶだけ)= `once` / `repeat` / `ping_pong`。`position` を繋いだ node(分解版)は wrap を
    持てない(持つと `audio.playback_mapping_conflict@1`)ので、そのときは欄が無効です。
  - 選ぶか Enter で `set_node_parameters` 1 回です。既に効いている値(Bus / Wrap は、鍵が無ければ一覧の先頭の既定値)を選んでも何も送りません
    ([送る所](../../src/devstudio/view/graphcanvaswidget.cpp#L4859))。
- **palette で置くと、project の最初の音声が `asset` に入ります**([置くときに asset を入れる所](../../src/devstudio/model/graphcanvasmodel.cpp#L2626))。
  任意の `events` / `position` は空のままです。
- **palette で置くと、必須の `time` はシーンの時計に繋がります(✅Q-11)。**上の (1) の種の定数(0)ではなく、今の scope に
  `value.input{parameters:{name:"scene_time"}}` があればそれを使い回し、無ければ 1 つ足して(id は `scene_time`。その id が今の scope で使われていれば `scene_time_2` …)、
  その `value` を `time` に繋ぎます([シーンの時計を繋ぐ所](../../src/devstudio/model/graphcanvasmodel.cpp#L2591) /
  [`GraphCanvasModel::sceneTimeNode()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1968))。時計の追加と node の配置は**1 回の編集**で送るので、
  **Undo 1 回で両方が戻ります**。AU-3 から engine は `events` も `position` も繋がっていない `audio.playback` を読み込み、owner の最初の
  frame から鳴らすので、**置いただけで読み込まれ、鳴ります**。
  - 使い回すのは**今の scope** の `scene_time` だけです。definition の中で置くと、root に `scene_time` があってもその definition に新しく 1 つ足します。
    名前が `scene_time` でない `value.input`(たとえば `time`)は使いません。
  - 足した `scene_time` の node は配置を持ちません(canvas の初期配置で描かれます)。
- 音声の format の拒否(`audio.format_unsupported@1`)は、この tab の notice に Q-6 の表の文で出ます(§13.1「Studioでcomponentを追加する」)。

**ループを置く —— graph.loop(✅LP-4c)**: Graph タブは共通グラフのループ `graph.loop`(書き方は [第6章 §6.18](06_rendering.md)、設計は [design_graph_loops](../design_graph_loops.md) §9)を、parameters の JSON ではなく専用の欄と表で組みます。
ループの RPC(definition の op、`loops[]` と `loop_origin`、`set_input` の `element`、`decompose_material_pass`、`get_graph_templates` / `apply_graph_template`)は、上の [「ループと分解の RPC —— engine の契約」](#ループと分解の-rpc--engine-の契約lp-0alp-vb--rd-1--rd-1b) にあります。
LP-4c の着地時はゾーン表示が無く、body は instance と同じく「中に入って」編集していました。✅LP-4d1 から、root の canvas はループを枠(ゾーン)で描き、枠の中の body をその場で編集します(下の「ループをゾーンで見る・編集する」)。「中に入る」道(Open body とダブルクリック)も残っています(下の「body に入る」)。

- **Add loop**(root だけ): キャンバスの右クリックの **Add loop**(`pelican.graphCanvas.addLoop`)は、空の definition を 1 つ作り、それを body にするループを root に置きます。
  - definition の名前は Studio が提案します(`loop_body`、使われていれば `loop_body_2` …)。打つものはありません。
  - `add_definition` と `add_node` を 1 つの列で送るので、Undo 1 回で両方が戻ります([`GraphCanvasModel::addLoopOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2811))。
  - 置いたループは `over: {range: {count: 1}}`・`branches: [{body: <definition>}]` です。
- **palette から**: palette(**Add node...**)の `graph.loop` は root にだけ出ます。選ぶと body の combo が出ます。
  - 先頭の **New definition…** では、名前の欄(提案で埋まっている。`pelican.graphCanvasPaletteNewDefinition`)で名前を変えられます。
  - 既にある definition を選ぶと、それが body になり、その出力が全部 `p`(最後の回の値)として宣言されます。
- **Add fullscreen pass**(`pelican.graphCanvas.addFullscreenPass`。右クリック。catalog に `render.fullscreen` があれば root でも definition の中でも出ます): `engine://passthrough` で描く fullscreen pass と、それが書く自前の `render.image`(id は `<pass>__color`)を 1 回で足します。空の body に最初の pass を置く手段です。
  - ⚠ LP-4c の着地時は、pass に `pass_set: "main_render"` を書き、置き場所の無いループの body に最初の pass を置くとループに `canonical_anchor: "post_main"` も足していました。RU-1(`fa413aeb`)から、どちらも書きません(engine が LP-1e で、置き場所の無い graph の pass とループの既定の位置を持ったため)。
  - ✅LP-4f2 から、**view の軸で回すループの body** に置くときは、image の大きさをそのループの family に合わせます([pass と image を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L2436)、[大きさを引く所](../../src/devstudio/model/graphcanvasmodel.cpp#L2956))。engine は 1 つの view family の raster pass が違う大きさに書くことを断る(`render.view_family_extent_mismatch@1`。下の「view family の大きさの拒否」)ので、画面の大きさの image を cube の 256×256 の family に混ぜないためです。
    - 基準は、engine が `get_frame_plan` の二次 family の node に載せる `family_extent` / `family_layers` です(✅LP-4f3。engine の ✅VF-2 で、VF-1 の拒否と同じ helper が決める基準)。LP-4f2 では Studio が `get_render_authoring_context` の `render_targets[]` を自分で辿っていましたが、LP-4f3 で外しました。family が `family_extent` を持たなければ、今までどおり画面の大きさで作り、engine の拒否の文に任せます。
    - 固定の大きさなら image に `width` / `height` を書き、`layers` が 2 以上ならそれも書きます(cube なら 256×256・6 layers)。画面相対なら `extent_scale` をその値にします。
    - pass には `view_family`(ループの family)を書き、固定の大きさなら `resolution_domain: "independent"` も書きます。
    - 基準が読めない(plan か target の宣言が無い)ときは今までどおり画面の大きさで作り、`view_family` だけを書きます。大きさが合わなければ engine が名前付きで断ります。

**ループの node(✅LP-4c)**: ✅LP-4d1 からの root の canvas では、ループはふつう下のゾーンで描かれ、この node の形になるのは、三角でゾーンを畳んだとき、同じ body を先のループがゾーンに描いているとき、body を持つ分岐が無いときです。見出しの右の型名の所に `↻ loop` が出ます。engine の frame plan の `loops[]` が届くと、`↻ expand ×3` のように段と回数に変わります([見出しの字を決める所](../../src/devstudio/model/graphcanvasmodel.cpp#L1678))。

- 入力ソケットは、body を持つ最初の分岐の body の `graph.input` です(型は付けません)。
- 出力ソケットはループが宣言した `p` / `p[]` で、`p[]` は `<名前>[] (each)` と出ます(回ごとの値の束)。
  - ⚠ LP-4c の着地時は、`p[]` の行の port と字が入れ替わっていて(port が `<名前>[] (each)`)、そこからの線がソケットに届いていませんでした。✅LP-4d1 から、字が `<名前>[] (each)`、port が `<名前>[]` です([行を組む所](../../src/devstudio/view/graphcanvasdrawing.cpp#L153))。
- parameters(`over`・`branches` …)はソケットにも値の帯にも出ず、詳細表にも `parameters` の行を出しません。詳細表の `body` の行は、入るときの body です。
- Graph タブは、文書にループがあるときだけ、文書を読むたびに `get_frame_plan` を 1 回読んで `loops[]` を使います。✅LP-4f1 から、ループが無くても root に view_select を取る render node があれば読み、同じ plan から node の `view_axes`(view family ごとの軸)と render target の名前も集めます(下の「view の軸で回す」。[読みを決める所](../../src/devstudio/model/graphcanvasmodel.cpp#L714))。✅Q-19 から、root の instance の definition に render node があるときも読み、同じ plan から pass の `pass_inputs` も集めます(下の「render node の入力は shader の宣言から」)。✅GL-1 から条件は無く、文書を読むたびに読みます(下の「graph の外から来た pass を畳んだ node で見る」)。

**ループをゾーンで見る・編集する(✅LP-4d1)**: root の canvas は、root の各 `graph.loop` を枠(ゾーン)で描き、枠の中に今の分岐の body の node を並べます(Blender のループゾーンの見え方。保存の形は変わらず、body は definition 名のままです)。枠の中の編集は body の definition へ、枠の移動は root へ書きます([ゾーンを計画する所](../../src/devstudio/view/graphcanvaswidget.cpp#L1285)、[`Builder::addZone()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L838))。

- **見出しの帯**: 左から三角、ループの label(無ければ id)、右端に段の字です。
  - 段の字は engine の `loops[]` の報告から作ります: `expand ×2` / `draw_loop N materials`。報告の前は `loop` です。
  - ✅LP-4f1: view の軸で回すループの段の字は `×6 (face)`(回数と軸)です。報告の前は frame plan の軸の長さを使い、それも無ければ `loop (face)` です。分岐が 1 つで長さが 1〜32 なら、タブの位置に軸のラベル(`+X` `-X` … 。ラベルの無い軸は番号)が小さく並びます(読むだけ。tooltip `Element <i> runs for this view`。[段の字を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L1393)、[ラベルを描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L974))。
  - body の definition を他の instance やループも使っていると、段の字の左に橙色で `shared (N users)` が出ます(tooltip は `Other instances or loops run <definition> too: an edit here changes them all.`)。N は使っている node の数で、✅Q-17 から、1 つのループは分岐がいくつ同じ body を使っていても 1 と数えます([`GraphCanvasModel::definitionUseCount()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1417))。Q-17 の前は、同じループの 2 つの分岐が同じ body を使うだけで `shared (2 users)` と出ていました。
- **分岐のタブ**: 分岐が 2 つ以上のときだけ、見出しの下に `if <条件>` / `elif <条件>` / `else` のタブが並びます。else だけのループにはタブがありません。
  - 条件の字は `element = 0` / `element in 1, 4` / `element not in …` です。draw_loop の分岐は material・`tags …`・contract・`skinned`・`has variant` などを ` & ` でつなぎ、式の条件は `expression` です([`zoneCondition()`](../../src/devstudio/view/graphcanvaswidget.cpp#L1182))。
  - タブを押すと、その分岐の body に描き替わります([`GraphCanvasModel::showLoopBranch()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1393))。どのタブかはセッションの状態で、文書には書きません。Open body で入る body も、タブが選んだ分岐のものです。
  - `"body": null` の分岐のタブは薄く描かれ、押しても変わりません(tooltip `This branch runs nothing`)。
- **左右の縁**: 1 行が 1 つの名前です。
  - 左の縁: 外側(枠の線の上)にループの入力のソケット、内側に body の `graph.input` のソケットです。body の `graph.input` は node としては描きません。内側のソケットから body の node の入力へ drag すると、その `graph.input` の `value` からの `set_input` が出ます。
  - 右の縁: 内側に body の出力(definition の `outputs`)の端、外側にループが宣言した `p` / `p[]` のソケットです。出力を作る body の port から内側の端へ破線が引かれます(描くだけです)。
  - ✅LP-4d2 から、編集できる文書では、左右の縁の行の最後に空いた口 `+` が 1 つずつあります。右の縁の内側の端は、carry を引く始点にもなります(下の「縁をまたぐ線・carry・展開の表示」)。
- **枠の大きさ**: body の node の外接に余白を足したもので、最小の幅と高さがあります([`layoutZone()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L723))。body の node を枠の左や上へ動かすと、枠がその側へ広がります。body が空でも枠は描かれます。✅LP-4d2 から、縁の行の数には `+` の口の行も数え、下端には展開の行と carry の矢印の通り道の分の高さが足されます。drag の間も枠が追従します(下)。
- **枠の中の編集**: body の node は、definition 自身の `layout.nodes` の位置(枠の内側の原点から)に描かれます。上の「送る op」の表の操作は、枠の中では全部 `scope: <definition>` で出ます([編集を body へ送る所](../../src/devstudio/view/graphcanvaswidget.cpp#L1828))。
  - node の drag は definition の `set_layout`、線を引く・切るは `set_input`、Delete は `remove_node`、見出しのダブルクリックは `set_node_label`、値の帯は `set_node_parameters` です。
  - 線は同じ scope の中だけで引けます。例外は ✅LP-4d2 の、root の node の出力を枠の中の node の入力へ直接落とす線で、ループの入力を足す 1 回の編集になります(下の「縁をまたぐ線・carry・展開の表示」)。それ以外では、body の node と root の node の間には引けません。
  - 位置の無い body の node(palette で足した直後など)は、枠の中で自動で並びます。
  - `Values from:` の出力の値の帯は、ゾーンの body の node には重なりません([外す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1507))。
  - body の node を選ぶと、右の詳細表と欄(draw step の欄を含む)はその definition の node を見せます。
- **body に足す**: 右クリックのメニューを枠の中で開くと、Add node... と Add fullscreen pass は body に足します。ゾーンを選んでいる(見出しを押した)か、枠の中の node を選んでいるときも、Add fullscreen pass と palette(Add node...)は body に足します([足す先を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L2121))。palette は body の scope の候補を出し、`graph.loop` と `graph.instance` は出しません。Add loop は枠の外で開いたメニューにだけ出ます。
- **ゾーンを選ぶ・動かす**: 枠の中の node の無い所か見出しを押すと、ゾーン(ループ)が選ばれ、Loop の欄が出ます。見出しを drag すると、root の `set_layout`(ループの位置)だけが出ます。body は枠の子として付いてきて、definition の `layout` は書きません。逆に枠の中の node の drag は definition の `set_layout` だけで、1 回の drag で両方が書かれることはありません。
  - **Auto layout** は root だけを並べ直します(ゾーンは 1 つの箱です)。body の配置は変えません([`GraphCanvasScene::autoLayout()`](../../src/devstudio/view/graphcanvaswidget.cpp#L522))。
- **三角**: 見出しの左の三角(tooltip `Fold the zone into the loop's node`)で、枠を LP-4c のループの node(ソケットつき)に畳みます。畳んだ node の三角(tooltip `Open the loop's zone`)で枠に戻ります。ループには見出しだけに畳む形はありません。どちらの形かはセッションの状態で、文書には書きません。
- **共有**: 同じ definition を 2 つのループが body にしていると、枠に描くのは文書で先のループだけで、もう一方は LP-4c のループの node で描かれます。枠の見出しには `shared (N users)` が出ます。
- **ダブルクリック**: 見出しは改名です。枠の中の node の無い所をダブルクリックすると、今までどおり body の scope に入ります(下の「body に入る」)。body の node のダブルクリックでは入りません([`GraphCanvasScene::mouseDoubleClickEvent()`](../../src/devstudio/view/graphcanvaswidget.cpp#L3259))。
- **Undo**: Graph タブの履歴のままです(下の「Undo」)。枠の中の編集は definition の scope の op、枠の移動は root の `set_layout` で、それぞれ 1 項目として戻ります。scope の違う `set_layout` が履歴に混ざっても、文書は byte 一致で戻ります(scene の test)。

**縁をまたぐ線・carry・展開の表示(✅LP-4d2)**: 枠の縁をまたいで線を引くと、ループの入力と出力が足されます。body の出力から body の入力へ戻る線は carry になり、枠の下側を回る矢印で描かれます。engine の報告が届くと、ループが何になったか(回の数か material の数)が枠の下端に出ます。どの操作も 1 回の編集で、Undo 1 回で戻ります。

- **外から入力を足す**: 編集できる文書では、左の縁の行の最後に空いた口 `+` があります(tooltip `Drop a line from outside here for a new loop input`)。root の node の出力をここへ落とすと、ループの新しい入力を足す列を 1 回で送ります([列を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L3482)、[落とし先を見分ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L2310))。
  - 列の中身は、ループの分岐が走る definition への `add_node graph.input`(同じ definition には 1 回だけ、分岐の順。既定値は付けません)と、root の `set_input`(ループの新しい入力 ← 落とした出力)です。今のタブの body だけでなく、全部の body に足します(分岐の body の入出力がそろっていないと engine が `graph.loop_branch_interface@1` で断るため)。
  - 枠の中の body の node の入力に直接落としても、同じ列を送ります。列の最後に、その definition の `set_input`(その入力 ← 新しい `graph.input` の `value`)が付きます。
  - ✅Q-19 から、落とした先が body の render node の宣言のソケットなら、列の最後は番号付きの port を足す `set_node_parameters` とその `set_input` です(下の「render node の入力は shader の宣言から」)。
  - 入力の名前は、落とした出力の port 名から Studio が提案します。`.` と `/` は `_` になり、末尾の `[]` は外します(空なら `input`)。ループの入力か body の node の id と重なれば `_2`、`_3` … です([`GraphCanvasModel::suggestLoopInputName()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3435))。打つものはありません。
  - 引けるのは、canvas が root を見ているときの、root の node の出力からだけです。ループ自身の出力からは引けません(循環になるため)。
  - ループが既に持つ入力の外側のソケットに落とすと、今までどおり root の `set_input` の差し替えだけです。
- **中から出力を足す**: 右の縁の行の最後にも `+` の口があります(tooltip `Drop a body node's output here for a new loop output`)。枠の中の body の node の出力をここへ落とすと、次の列を 1 回で送ります([列を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L3528))。
  - body ごとに 1 つの `set_outputs`(✅DO-1。上の op の表)。その body の `outputs` に新しい出力を足したものを丸ごと置きます。
  - 最後に、ループの `outputs` に `p` を足した root の `replace_node`。`p[]` はここでは足しません(Loop の欄の Outputs の check で足します)。
  - 出力の名前は port 名から入力と同じ規則で提案します(空なら `output`)。ループが宣言した `p` / `p[]` か、どれかの body の出力と重なれば `_2` … です。
  - 他の分岐の body は、同じ id の node の同じ port からその出力を出します(body を写したときの配線です)。`set_outputs` の endpoint は node と port の両方が要り、つながっていない出力は足せないためです。
  - 他の分岐の body にその node か port が無ければ、何も送らず、Graph タブの notice に 1 行で断ります([`GraphCanvasModel::addZoneOutput()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3573))。例: `Branch 1 of loop steps runs rest, which has no sum.value to give value from: add the output there first.`
- **carry の矢印**: 右の縁の内側の端(body の出力。tooltip `The body's output <出力> (drag to a boundary input to carry it)`)から、左の縁の内側の端(body の `graph.input`)へ線を引くと、ループの `carry` に `{from, to}` を 1 組足します。送るのは Loop の欄の Carry と同じで、parameters を丸ごと置く `set_node_parameters` 1 回です([`GraphCanvasModel::addLoopCarry()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3585))。同じ組が既にあれば何も送りません。
  - 矢印は、出力の端から右へ出て枠の下側の通り道を戻り、入力の端へ左から入ります([矢印を引く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1197))。通り道は組ごとに分かれ(下端の展開の行の上)、その分だけ枠が高くなります。
  - 矢印を押すと選ばれて太くなり、Delete でその組を `carry` から外します(これも `set_node_parameters` 1 回です。[`GraphCanvasModel::removeLoopCarry()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3598))。tooltip は `<from> carries into <to> (select and press Delete to remove)` です。
  - `to` の入力にループの入力の edge が無い組は、矢印が橙色になり、`!` の印が付きます。tooltip は、Loop の欄の Carry の赤い字と同じ `<to> has no loop input: connect one for the first element to start from.` です。
- **回ごとの入力の線**: source に `element` を持つ edge(下の「回ごとの入力 —— Per iteration」で張った入力)は破線で描かれ、線の中ほどに `[<element>]` のラベルが乗ります([線を描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L616))。
  - ラベルの字は、式(`{"$expr": …}`)ならその式、文字列ならそのまま、それ以外は JSON です。tooltip は `Each element reads element <element> of <node>.<port>` です。
  - 線を選んでも破線のままです。ラベルは線に付いて動きます。
- **展開の表示**: engine の `loops[]` の報告が届いたループでは、枠の下端に 1 行が出ます([出す所](../../src/devstudio/view/graphcanvaswidget.cpp#L1526))。報告の前は出ません。
  - expand のループ: `×N copies` と **Show in Frame Plan** のボタンです。押すと Frame Plan のタブが前に出て、frame plan を読み直し、届いたらそのループのグループを開いて、回の node を全部選んで中央に寄せます([グループを開いて選ぶ所](../../src/devstudio/view/frameplangraphics.cpp#L3711))。Frame Plan の status 行は `Loop <loop>: its copies are picked.` です。plan にそのループの回が無ければ `The frame plan has no copies of loop <loop>.` です。
  - draw_loop: `N materials`(1 つなら `1 material`)です。draw_loop は回を複製しないので、ボタンはありません。分岐のタブの字の後には、その分岐に当たった material の数が付きます(`if m#a (1)` / `else (2)`)。
- **Frame Plan からの Open in Graph**: 回の node の **Open in Graph** は、body の scope に入らず、root の canvas のまま、ゾーンのタブをその要素の分岐に合わせ、枠の中のその node を選んで中央に寄せます。畳んだゾーンは開きます([`GraphCanvasWidget::revealLoopOrigin()`](../../src/devstudio/view/graphcanvaswidget.cpp#L5448))。body がゾーンに描かれないとき(同じ definition を先のループのゾーンが描いている)は、LP-4c のとおり body の scope に入ります。
- **枠の追従**: body の node を drag している間も、枠がその node を囲むように広がり、縁のソケット・見出しの帯・下端の行が付いて動きます。ループへ出入りする root の線、右の縁への破線、carry の矢印も引き直します([`GraphCanvasScene::refitZoneLive()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2017))。離すと、今までどおり枠を描き直します。
- **値の帯の切り詰め**: ゾーンの中の node の値の帯(pill)は、node の幅に収まるように末尾を `…` で切り、全文を tooltip に出します([切る所](../../src/devstudio/view/graphcanvasdrawing.cpp#L527))。render 系に限らず、ゾーンの中の node 全部に効きます。
- **Undo**: 上の 3 つの操作(入力・出力・carry)は、それぞれ 1 項目として戻ります。`set_outputs` の逆は、前の `outputs`(無ければ空)を持つ同じ op です(下の「Undo」)。入力・出力・carry を足してから Undo 3 回と Redo 3 回をすると、文書は byte 一致で戻ります(scene の test)。

**render node の入力は shader の宣言から —— 線で番号付きの port を作る(✅Q-19)**: Graph タブの `render.fullscreen` / `render.raster` の node は、engine がその pass の shader の宣言を出していれば、入力のソケットを Frame Plan と同じ形で描きます(✅VI-3a。下の「ソケットは shader の宣言から出ます」)。ソケットへ線を落とすと、Graph タブが engine の番号付きの入力の port(✅LP-1d。`"binding": "input"` と `input_index`。[第6章 §6.18](06_rendering.md) の「番号付きの入力」)を書いてつなぎます。Q-19 の前は、render node に別の画像を読ませるには、parameters の JSON(pill)に port を打つしかありませんでした。

- **宣言の出所**: Graph タブが読む `get_frame_plan` の node の `pass_inputs` です([読む所](../../src/devstudio/model/graphcanvasmodel.cpp#L3933))。Graph タブの node と plan の pass は次のように対応づけます([`GraphCanvasModel::passInputs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3963)):
  - root の node: 名前がその id の pass
  - ループの body の node: plan の順で最初の、`loop_origin` の `node` がその id で、その要素の分岐がこの body を走らせる回の pass(どの回も同じ shader で描くため、1 つの回の宣言を使います)
  - instance の definition の node: `<instance>/<id>` の pass
- **形**: 固定の宣言は ○ 1 つずつで、字は宣言の変数名です(つながっていれば `<変数名> ← <node>.<port>`)。可変の組は、つながっている ○(字は `← <node>.<port>`)と下端の空の落とし先 `+ input` を半透明の帯が囲みます([ソケットを組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L288)、[帯を描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L503))。宣言のソケットは binding の順で、node のほかの行(書く port など)より上に並びます。buffer の宣言も ○ で描きます。
  - 番号付きの port はソケットのものになり、port の行としては出ません。宣言が取らない番号付きの port は、port 名の ○(`<port> ← <node>.<port>`)で残ります。線の行き先が無くならないようにです。
- **宣言が無い node は今の行のまま**です。plan にその pass が無い、`pass_inputs` が `declared` でない(pipeline がまだ無い、など)、宣言が 1 つも無い、Studio が読めない宣言のどれでも、Q-19 の前と同じ port の行で描きます([分ける所](../../src/devstudio/model/graphcanvasmodel.cpp#L1827))。
- **plan を読む時**: 上の「ループの node」の読みに加えて、root の instance の definition に render node があるときも読みます。✅GL-1 からは条件が無く、文書を読むたびに読みます([`GraphCanvasModel::requestLoopReports()`](../../src/devstudio/model/graphcanvasmodel.cpp#L714))。足した直後の pass は、engine がそれを持つ世代を出すまで plan に載りません。そのため、render node の pass が plan に無い(か宣言が `declared` でない)間は、文書を読むたびに最大 40 回まで、時計で plan を読み直します([読み直す所](../../src/devstudio/model/graphcanvasmodel.cpp#L838))。回数 0 と報告されたループの body は待ちません。

線を落とした先で起きることは次のとおりです。どれも 1 回の編集で、Undo 1 回で戻ります([列を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L4115))。入力のソケットから出力へ逆向きに引いても同じです。

| 落とした先 | 送るもの |
|---|---|
| 空の ○ か `+ input` で、binding が次の番号(0 から続く番号付きの port の数) | `set_node_parameters`(`ports` に `{"kind": "image", "type": …, "access": "read", "binding": "input", "input_index": <b>}` を足す)と `set_input`(その port ← 落とした出力) |
| つながっている ○(宣言が取らない port の ○ も) | その port の `set_input` の差し替え。source の型が今の `type` と違えば、先に `type` を直す `set_node_parameters` も送ります。同じ線なら何も送りません |

- **type**: engine は image の port に `type` を要るので、落とした出力から辿ります([`GraphCanvasModel::imageTypeOf()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4062))。pass の port の `type`、image などの `parameters.type`、body の `graph.input` ならそのループか instance に入っている線の source、instance やループの出力なら body の出力の source、の順です。分からなければ、Add fullscreen pass の image と同じ scene color の型(`pelican.render.color_signal@1`、`reference: scene`・`transfer: linear`・`range: extended`)です。
- **port の名前**: source が body の `graph.input` ならその id、それ以外は source の port 名から、ループの入力と同じ規則(`.` と `/` は `_`、末尾の `[]` は外す、空なら `input`)で提案します。node の ports か inputs と重なれば `_2`、`_3` … です([`GraphCanvasModel::suggestInputPortName()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4041))。打つものはありません。
- **外す**: 番号付きの port の線を選んで Delete するか、つながっている ○ を空所へ drag すると、`set_input{source: null}` と、port を消して後ろの port の `input_index` を 1 つずつ詰めた `set_node_parameters` を 1 回で送ります([`GraphCanvasModel::removeInputPortOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4231))。
- **断るもの**(送らず、notice に 1 行):
  - 飛ばし: `Connect binding <n>..<b-1> of <node> first: a pass's inputs fill the bindings in order.`(飛ばしたのが 1 つなら `Connect binding <n> of ...`)。可変の組の前の固定の宣言が埋まっていないときの `+ input` も同じです
  - 可変の組が max まで埋まっている: `<node>'s shader takes at most <max> inputs in its variadic group.`
  - buffer の宣言: `Binding <b> of <node> takes a buffer: a graph pass reads only images by number.`
  - 宣言に無い binding: `<node>'s shader declares no binding <b>.`
- **ゾーンの外から body のソケットへ**: root の node の出力を、枠の中の body の render node の宣言のソケットへ直接落とすと、上の「外から入力を足す」の列(`add_node graph.input` と root の `set_input`)の後に、その `graph.input` の `value` を読む番号付きの port の `set_node_parameters` と `set_input` を付けて、1 回で送ります([`GraphCanvasModel::addZoneInputToSocket()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4252))。port の名前は新しい `graph.input` の id、型は root の source から辿ります。飛ばしなどで断るときは、ループの入力も足しません。
- **carry で前の回の出力を読む**: 線だけで組めます。(1) root の pass の出力を左の縁の `+` へ(ループの入力)、(2) body の pass の出力を右の縁の `+` へ(ループの出力)、(3) 右の縁の内側の端を左の縁の内側の端へ(carry)、(4) 左の縁の内側の端(body の `graph.input`)を body の pass の宣言のソケットへ落とします。(4) で pass がその `graph.input` を番号付きの入力として読み、展開された 2 回目の pass が 1 回目の出力を読みます(frame plan の `loop/1/render_fullscreen` の `reads` に `loop/0/render_fullscreen__color` が入ります。シナリオ `loop_zone_read`)。
  - carry の矢印は左の縁の内側のソケットで終わり、その上に描かれます。Q-19 から、矢印の下のソケットを押すと、carry を選ぶのではなく線を引き始めます([押した所](../../src/devstudio/view/graphcanvaswidget.cpp#L2493))。Q-19 の前は、carry の付いた入力からは線を引けませんでした(実装者の報告)。
- **分かっている穴**(Q-19 の着地時の記録。実装者の報告):
  - ✅LP-6(`03725036`)で塞がりました。root の `render.image` から引いた線も通ります(engine の規則: image node から読む pass は、その image を書く graph の pass を待つ。待たない場合も含めて [第6章 §6.18](06_rendering.md) の「image node を読む pass の順」と [design_graph_loops.md](../design_graph_loops.md) §5.2)。Q-19 の着地時は、root の `render.image` の出力を body の pass のソケットへ引くと、engine が `Pass input target is not produced as an earlier output: <image> in pass: <pass>` で断り([その文を投げる所](../../src/core/renderingpass/frameplanner.cpp#L1860))、シナリオのように root の pass の出力 port から引いて避けていました。
  - **LP-6 が入るまでの一時的な案内(Q-20、Q-21 で外した)**: Q-20 から Q-21 までの間、Graph タブは上の拒否を受けると、notice の拒否の行の後ろに「その image を書く pass から線を引く」案内を足していました(`render.image` から読む線を持つ編集のときだけ。engine はこの拒否に専用の code を付けないので、判定は message の文字列でした)。LP-6(`03725036`)で engine が直ったので、Q-21(`5b4b3737`)で案内とその判定を外し、シナリオ `loop_zone_read` に root の `render.image` から body の pass へ引く線を足しました。
  - シナリオ `loop_zone_read` は、frame plan の回の pass を位置(`nodes[2]` / `nodes[3]`)で見ています(同じ位置の `name` が合うことも見ます)。
  - シナリオで `Graph change saved.` を待った直後に発火する action は、文書の読み直しの途中で disabled のことがありえます(`loop_zone_wire` も同じ手順です)。

自動化面の名前(表の正は [README](../../test/studio_scenarios/README.md)。[definition を前に付ける所](../../src/devstudio/automation/automationcommands.cpp#L291)):

| 名前 | 指すもの |
|---|---|
| `graph_node#<loop>` | 枠そのもの(ループ)。`click` は枠の中央ではなく見出しに当たります([当たる所](../../src/devstudio/view/graphcanvasdrawing.cpp#L856)。中央には body の node があるため) |
| `graph_zone#<loop>` | 枠の内側(body を描く所) |
| `graph_zone_label#<loop>/stage` / `graph_zone_label#<loop>/shared` | 段の字 / `shared (N users)` |
| `graph_zone_tab#<loop>/<分岐の番号>` | 分岐のタブ(0 から) |
| `graph_socket#<loop>/<port>` | ループ自身のソケット(左の縁に入力、右の縁に `p` / `p[]`) |
| `graph_socket#<definition>/<input>/value` | 左の縁の内側の端(body の `graph.input` `<input>`) |
| `graph_zone_socket#<loop>/out:<出力>` / `graph_zone_link#<definition>/<loop>/out:<出力>` | 右の縁の内側の端 / そこへの破線(描くだけ) |
| `graph_node#<definition>/<id>` | ゾーンの中の body の node。見出し・ソケット・値の帯・辺も同じ形で前に `<definition>/` が付きます(`graph_node_header#<definition>/<id>`、`graph_socket#<definition>/<id>/<port>` …) |
| `graph_zone_socket#<loop>/in:+` / `graph_zone_socket#<loop>/out:+` | ✅LP-4d2。左 / 右の縁の空いた口 `+` |
| `graph_zone_carry#<loop>/<from>/<to>` | ✅LP-4d2。carry の 1 組の矢印(押すと選ばれ、Delete で外す) |
| `graph_zone_carry_mark#<loop>/<from>/<to>` | ✅LP-4d2。その横の `!` の印(`<to>` にループの入力の edge が無いとき) |
| `graph_zone_label#<loop>/expansion` | ✅LP-4d2。下端の行(`×N copies` / `N materials`) |
| `graph_zone_label#<loop>/axis<i>` | ✅LP-4f1。view の軸で回すループの見出しの下の、i 番目の軸のラベル(`+X` …。0 から) |
| `graph_zone_button#<loop>/frame_plan` | ✅LP-4d2。Show in Frame Plan |
| `graph_edge_label#<node>/<port>` | ✅LP-4d2。回ごとの入力の破線のラベル(`<node>` / `<port>` は線の受け手) |
| `graph_socket#<id>/in@<binding>` | ✅Q-19。render node の宣言の ○、または可変の組のつながっている ○(ゾーンの中は `graph_socket#<definition>/<id>/in@<binding>`) |
| `graph_socket#<id>/in:variadic` | ✅Q-19。可変の帯の空の端(`+ input`) |
| `graph_socket#<id>/in:<port>` | ✅Q-19。宣言のソケットにつながっている番号付きの port の名前でも、その ○ に当たります(別名。`list` には出ません)。宣言が取らない番号付きの port の ○ は `graph_socket#<id>/<port>` が自分の名前です |
| `graph_node_label#<id>/in:band` | ✅Q-19。可変の帯そのもの(描くだけ) |

- definition 名にも id にも `/` は入らないので、最初の `/` が definition と id の区切りです。body の node の id が root の node の id と同じでも、名前は分かれます。
- Open body で body に入ったときの名前は今までどおりで、definition は付きません。
- ✅LP-4d2: 同じ名前の入力と出力を持つ node(fullscreen pass の `color` など)のソケットは、`graph_socket#<id>/<port>` では分けられません。そのため `graph_socket#<id>/in:<port>` / `graph_socket#<id>/out:<port>` の別名でも引けます(ゾーンの中は `graph_socket#<definition>/<id>/in:<port>` …)。別名は名前を解決するときだけ効き、`list` には出ません([別名を作る所](../../src/devstudio/automation/automationcommands.cpp#L310))。
  - ✅Q-19 から、宣言のソケットの `in:<port>` は、そこにつながっている番号付きの port の名前です(宣言のソケット自身の名前は `in@<binding>` なので)。

**分かっている穴**(LP-4d1 と LP-4d2 の着地時の記録):

- 見出しに当たる `click` の位置(hotspot)は、シナリオでは確かめていません(scene の test の変異で守っています)。
- carry の効き目は Frame Plan では見分けられません。シナリオの body の pass は書くだけで、前の回の値を読む port を持たないためです。読む port は、今の Graph タブでは JSON を打たないと作れません(実装者の報告)。✅Q-19 で塞がりました。線で番号付きの port を作れるようになり、シナリオ `loop_zone_read` が 2 回目の pass の `reads` で carry の効き目を見ます(上の「render node の入力は shader の宣言から」)。
- 新しいループが既存の node に重なることがありました。選んでいる node の横に、node の幅を 240 と見て置いていたためです(`GraphCanvasScene::placeNewNode()`。実装者の報告)。✅LP-4f1 で塞がりました(下の「新しい node の置き場所」)。
- draw_loop の要素から Open in Graph する道はありません。draw_loop は展開されないので、`loop_origin` を持つ回の node が Frame Plan に無いためです(実装者の報告)。
- `in:` / `out:` の別名には単体の test がありません(シナリオ `loop_zone_wire` が使っています)。
- LP-4d1 の着地時にあった 3 つの穴(drag の間に枠が追いつかない、ゾーンの中の長い値の帯が node からはみ出す、Frame Plan の Open in Graph が body の scope に入る)は、✅LP-4d2 で塞がりました(上の「縁をまたぐ線・carry・展開の表示」)。

**Loop の欄(✅LP-4c)**: root のループを選ぶと、詳細表の上に Loop の欄(`pelican.graphCanvasLoop`)が出ます([`GraphCanvasLoopFields::rebuild()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L473))。
1 行目は要約(`loop loop · expand ×3 · body loop_body`。報告の前は `not reported yet`)と **Open body** です。
欄を 1 つ変えるたびに、ループの parameters を 1 鍵だけ変えて丸ごと `set_node_parameters` 1 回で送ります(Graph タブの Undo で 1 つずつ戻ります)。

| 欄 | 中身 |
|---|---|
| Over | ✅LP-4f1。`Count`(`over.range`)/ `View axis`(`over.view_axis`)。下の「view の軸で回す」 |
| Count | 回数の spin(0〜4096。Enter か focus を外すと送る)。右の **ƒ** で式の欄に替わり、`{"$expr": …}` を送ります(式の欄に整数だけを打つと整数として送ります)。✅LP-4f1 から、式の欄の下に target の combo(frame plan の render target)と **mip_levels_of** / **layers_of** のボタンが出ます。押すと `mip_levels_of("<target>")` などを、欄が空か整数ならそれに置き換え、式ならカーソルの位置に挿して送ります([`insertCountFunction()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L966))。Over が View axis のときは出ません |
| View | ✅LP-4f1。Over が View axis のときだけ。family と軸の combo、回数 `×6 (face)`(読むだけ) |
| Order | `ascending` / `descending`。descending は `order: {by: element, direction: descending}` を書き、ascending は `order` を消します |
| Limit | **first** の check と spin(`limit`。並べた後の先頭 N 個) |
| Stage | `automatic`(`tier` を消す)/ `expand` / `draw_loop` |
| Place | 展開した回が pass の列のどこに入るか。`end of its pass family` か `after <anchor>`(`sprite`・`post_main`・`tonemap`・`post_ldr`・`pelican_ui`・`debug_draw`・`debug_text`・`imgui`)。選ぶと `insert` を消して `canonical_anchor` を書きます。`insert` を持つループは `insert <…>` の項目で今の値を見せます。body に fullscreen / raster の pass が無いループでは押せません |
| Where | `every element` / `element in` / `element not in` と整数の並び(`1, 4`。Enter で送る)。整数でない片があれば送らず、tooltip が `Integers, separated by commas` になります。✅LP-4f1 から、回数が分かっていて 1〜64 のとき(整数の Count か、view の軸の長さ)は、並びの欄の代わりに要素ごとの check が出ます(view の軸なら `2 (+Y)` の字。[check を組む所](../../src/devstudio/view/graphcanvasloopfields.cpp#L981))。check を付け外すたびに送ります。1 つも付いていない間は送りません |
| Outputs | body の出力ごとに `<名前>` と `<名前>[]` の check。変えるとループの node を丸ごと送り直します(`replace_node`) |
| Carry | 行ごとに「body の出力 → body の入力」の 2 つの combo と **Remove**、下に **Add carry**。to の入力にループの入力の edge が無い行には、赤で `<入力> has no loop input: connect one for the first element to start from.` が出ます(engine が断る形です) |

Outputs の区は body に出力があるときだけ、Carry の区は body に入力と出力の両方があるときだけ出ます。

**view の軸で回す —— Over と View(✅LP-4f1)**: cube capture の 6 面や影の cascade のように、1 つの view family が持つ view を 1 つずつ回すループを、JSON を打たずに組めます(engine の形は `over: {view_axis: {family, axis}}`。[第6章 §6.18](06_rendering.md))。

- **Over**(`pelican.graphCanvasLoop.over`): `Count` / `View axis`。View axis を選ぶと、frame plan の最初の選べる軸で `over.view_axis` を送ります([`GraphCanvasLoopFields::commitOver()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L891))。Count に戻すと、軸の長さ(分からなければ 1)を回数にした `over.range` を送るので、回の数は変わりません。
- **View の行**: family の combo(`viewFamily`)と軸の combo(`viewAxis`)、右に回数 `×6 (face)`(`viewCount`。読むだけ)です。family か軸を選ぶたびに `over` を丸ごと置き換えて送ります。ほかの parameters は残ります([`GraphCanvasModel::setLoopViewAxis()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2981))。
  - 候補は、Graph タブが読んだ frame plan の node の `view_axes` を family ごとに集めたものです(軸を持つ family だけ。`$main`・planar の反射・作者の family は出ません。[集める所](../../src/devstudio/model/graphcanvasmodel.cpp#L2894))。どの pass も走らない family は候補に出ず、欄の下に `Families no pass of the frame plan runs in are not listed.` が出ます。
  - 軸の項目は `face ×6` の形です。実行時に決まる軸(directional の影の `light`)は `light (decided each frame)` と出て選べません(tooltip `The engine decides this axis each frame: a loop cannot run over it.`)。
  - 文書が名指す family / 軸が plan に無ければ、`(not in the frame plan)` を付けて残します。
  - 選べる軸を持つ family が 1 つも無いときに View axis を選ぶと、何も送らずに Over を Count に戻し、欄の下に `No pass of the frame plan runs in a view family with an axis a loop can run over.` が出ます。
- view の軸のループでは、Count の行と挿入のボタンは出ません。Where と分岐表の check は軸のラベル付き(`2 (+Y)`)です。

**body の render node の View 欄 —— Each element(✅LP-4f1)**: Graph タブで render node を選ぶと、詳細表の上に LP-4b と同じ **View** の欄(`pelican.graphCanvasRender`。軸の行は `pelican.graphCanvasRender.view.<axis>`)が出ます([`GraphCanvasViewFields`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1525))。

- 対象の op は `render.fullscreen` / `render.raster` / `render.compute` / `render.material_pass` / `render.shadow_depth` / `render.snapshot_copy` です([`GraphCanvasModel::takesViewSelect()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2975))。
- どの family の軸を出すか([`GraphCanvasModel::viewField()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2989)):
  - view の軸で回すループの body の node は、そのループの family(node が別の `view_family` を名指していれば対象外)。見出しは `View (<family>) · in loop <loop>` です。
  - root の node は、frame plan のその node の family。見出しは `View (<family>)` です。
  - どちらでもなければ node 自身の `view_family`(frame plan がその family の軸を出しているとき)。
  - 軸が取れなければ欄は出ません。
- 行の中身は LP-4b の欄と同じ(`All views` と値、実行時の軸は字だけ)です。ループの body の node では、ループの軸の combo に **Each element (loop)** が足されます。選ぶと、その回の element の view で走る `{"$expr": "element"}` を書きます。ほかの軸・ループの外の node には出ません。
- 選ぶたびに `set_node_parameters` を 1 回送ります([`GraphCanvasModel::setViewSelect()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3044))。
  - 欄が出さない値(`element` 以外の式)はそのまま残します。どの軸も All views なら `view_select` を消します。
  - node が `view_family` を書いていなければ、family も一緒に書きます(書かない pass は frame の最初の family で走るため)。
- LP-4f1 の前は、Graph タブの render node の `view_select` は parameters の JSON を打つしかありませんでした。

**Loop の欄は編集の往復の間は押せません(✅Q-17)**: Loop の欄の form と行は 1 つの widget(`pelican.graphCanvasLoop.fields`)にまとまっていて、編集を送ってから文書を読み直すまでの間(model が次の編集を受けられない間)は全体が無効になり、読み直すと一度だけ有効に戻ります([`GraphCanvasLoopFields::followEditable()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L447))。

- 親を無効にするだけなので、欄ごとの理由で無効な欄(Place など)は、戻っても無効のままです。
- それでも model が断った分岐の選択(body・条件・draw_loop の条件の combo)は、文書の値に戻ります([`restoreBranch()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1161))。欄が送っていない値を見せたままにはなりません。
- Q-17 の前は、欄を作り直すのが文書・報告・配置が変わったときだけだったので、往復の間も欄は有効に見え、そこで選んだ分岐の body は model に黙って捨てられていました(シナリオ `material_loop_decompose` の間欠の赤。実装者の報告)。
- Q-17 の着地時は、material pass と draw step の欄(`pelican.graphCanvasDraw`)と上の View 欄に同じ問題が残っていました。✅Q-18 で同じ形になりました(次の段落)。

**Draw 欄と View 欄も往復の間は押せません(✅Q-18)**: material pass と draw step の欄(`pelican.graphCanvasDraw`)と render node の View 欄(`pelican.graphCanvasRender`)も、Loop の欄と同じく、編集を送ってから文書を読み直すまでの間は無効になり、読み直すと有効に戻ります([`GraphCanvasDrawFields::followEditable()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1403)、[`GraphCanvasViewFields::followEditable()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1586))。Graph タブは model が動くたびに、Loop の欄と一緒にこの 2 つを呼びます。

- model が断った選択(Contract・Variant・Cull・Depth test・Depth write・Blend)は、文書の値に戻ります([`GraphCanvasDrawFields::restore()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1413))。**Defines** に打ちかけた文字は戻しません(文字なので、Apply defines でもう一度送れます)。
- View 欄で断られた選択は、行を作り直さずに、今使った combo のまま文書の値に戻ります([`ViewSelectField::showSelection()`](../../src/devstudio/view/viewselectfield.cpp#L162))。automation の `select` が当たった combo が、そのまま今の値を読めるようにするためです。
- Q-18 の前は、この 2 つの欄は往復の間も有効に見え、そこで選んだ値は model に黙って捨てられていました。
- ⚠ 分かっている穴(Q-18 の着地時の記録): `draw.override` の欄を全部 `(material's)` にして Defines も空のときは、何も送らずに Cull の tooltip に理由(`An override replaces at least one render state or adds a define; …`)を出すだけで、欄は文書の値に戻りません。

**新しい node の置き場所(✅LP-4f1)**: palette・Add loop・Add fullscreen pass で足した node は、選んでいる node(ゾーンなら枠)の描かれた箱の右端の隣に置かれ、同じ scope の描かれた箱と重なる間は下へずれます([`GraphCanvasScene::placeNewNode()`](../../src/devstudio/view/graphcanvaswidget.cpp#L3059))。

- 新しいものの大きさは描くまで分からないので、ループなら枠の最小の大きさ、ほかは数行の node と見ます。
- 選んでいる node が無いときは右クリックした所が起点です。ゾーンの body に、メニューからでも選んだ node の横でもなく足した node は、今までどおり枠の中で自動で並びます。
- 自動配置で描かれていた node は、このとき今の位置でセッションに固定されます(新しい node が自動配置の列を取って、ほかの node を押し出さないように)。位置はどれもセッションの状態で、文書には書きません。
- LP-4f1 の前は、選んでいる node の横に node の幅を 240 と見て置いていたので、幅の広いゾーンの横に置くと重なりました。

**分岐表(✅LP-4c)**: Loop の欄の下の **Branches** が分岐の表です([分岐表を組む所](../../src/devstudio/view/graphcanvasloopfields.cpp#L704))。

- 1 行が 1 分岐です。条件の combo は `element =` / `element in` / `element not in`、右が整数の欄(Enter で送る)です。
  - `element =` は欄の最初の 1 つだけを使い、空なら 0 です。
  - 文書の条件が式なら `(expression)` と出て、値の欄は押せません。
  - ✅LP-4f1 から、`element =` の値は spin です(回数が分かっていれば 0〜回数−1。値を変えると送る)。`element in` / `not in` は、Where と同じく回数が 1〜64 なら要素ごとの check で、それ以外(回数が式か 64 を超える)は今までの整数の欄です([行を組む所](../../src/devstudio/view/graphcanvasloopfields.cpp#L738))。
- body の combo は `(nothing)`、候補の definition、**New definition…** です。
  - `(nothing)` は `"body": null` で、当たった要素では何もしません。
  - 候補は入出力がループと同じ definition です(ループにまだ body が無ければ全部)。
  - **New definition…** は名前を `<ループの id>_body` から提案し(使われていれば `_2` …)、ループの入出力を写した definition を `add_definition` で作ってその分岐に付けます(同じ列で 1 回)。写すのは body の `graph.input` と出力ですが、出力を `graph.input` 以外の node が作る body では本体ごと写します。
- ↑ / ↓ で並べ替え、× で消します。最後の行は `else` で、動かせず、消せず、条件を持ちません。
- **Add branch** は else の前に 1 行足します。条件は `element = 0`、body は else と同じです。
- 各行の下に、engine の報告(`loops[]` の `elements` の `branch`)から当たった要素が `elements: 0, 2` の形で出ます。
  - どの要素も当たらない行(`unmatched_branches`)は、赤で `no element takes this branch` です。
  - 報告の前は `elements: not reported yet` です。

**body に入る(✅LP-4c)**: ループをダブルクリックするか、Loop の欄の **Open body** で、選んでいる分岐の body の scope に入ります。✅LP-4d1 からの既定の見え方はゾーン(上)で、この「入る」道も残っています。ゾーンでは、枠の中の node の無い所のダブルクリックで入ります(見出しのダブルクリックは改名)。

- パンくずは instance と同じです(`root / loop_body`)。既定の分岐は、先頭から見て最初に body を持つ分岐です。
- 中にいる間は、パンくずの右に分岐の combo(`pelican.graphCanvasLoopBranch`)が出ます([分岐の combo を合わせる所](../../src/devstudio/view/graphcanvaswidget.cpp#L4649))。
  - 項目は `loop: element = 0 → loop_body` の形で、body を持つ分岐だけです。
  - 選ぶと、その分岐の body に切り替わります。
- 同じ definition を root の instance やループの分岐が 2 か所以上で使っていると、その右に橙色で `shared (N users)` が出ます(`pelican.graphCanvasShared`)。ここで直すと、使っている所が全部変わります。

**回ごとの入力 —— Per iteration(✅LP-4c)**: root のループの入力ソケット(か行の字)を右クリックすると、`p[]` を宣言した**他の**ループがあるときだけ **Per iteration…** が出ます。

- dialog(`pelican.graphCanvasPerIteration`)で、読む `p[]`(`<ループ>.<出力>[]`)と Element の式を選びます。式の既定は `element`(受け手のループの要素)です。
  - ✅LP-4f1 から、Element は `element` / `element - 1` / `element + 1` / **Custom…** の combo です。Custom… のときだけ式の欄(`pelican.graphCanvasPerIteration.element.custom`)が出て、空なら `element` です([`GraphCanvasElementChoice`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1484))。LP-4f1 の前は式を打つ欄でした。
- OK で、`set_input` の source に `element: {"$expr": <式>}` を付けて 1 回送ります([`GraphCanvasModel::connectPerIteration()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3377))。
- 張った入力の要約には `[element {"$expr":"element"}]` が付きます。
- ✅LP-4d2 から、canvas ではその線が破線で描かれ、線の中ほどに `[element]` のラベルが乗ります(上の「縁をまたぐ線・carry・展開の表示」)。

**Undo(✅LP-4c)**: Graph タブの Undo は、1 つの編集の op の列を、前の op を当てた後の文書で 1 つずつ逆にします([`GraphCanvasModel::inverseOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1997))。

- 同じ列で足した node を後の op が指していても戻せます(Add loop、Add fullscreen pass、New definition…)。
- definition の op も戻せます。`add_definition` の逆は `remove_definition`、`rename_definition` の逆は名前を戻す rename、`remove_definition` の逆は本体つきの `add_definition` です。
- ✅LP-4d2 から、`set_outputs` も戻せます。逆は、前の `outputs`(無ければ空)を持つ同じ op です([逆にする所](../../src/devstudio/model/graphcanvasmodel.cpp#L2088))。canvas が送るのは definition の `set_outputs` だけです。

**definition の改名と削除(✅Q-19)**: Graph タブから definition の名前を変え、どこからも使われていない definition を消せます。送るのは engine の `rename_definition` / `remove_definition`(✅LP-0c。上の「`ops[]` と `scope`」の表)の 1 op で、どちらも Undo 1 回で戻ります(上の「Undo」の逆の op)。Q-19 の前は、Studio にこの 2 つの入口がありませんでした。

- **入口**は右クリックの **Rename definition…**(`pelican.graphCanvas.renameDefinition`)/ **Delete definition**(`pelican.graphCanvas.deleteDefinition`)で、次の 3 か所に出ます。
  - パンくず: canvas の中のパンくずと上段のパンくずの字。今見ている definition が対象です([canvas のメニューに足す所](../../src/devstudio/view/graphcanvaswidget.cpp#L2713))。
  - ゾーンの見出し: そのタブが見せている body の definition が対象です。
  - 上段の `Scope:` の combo: 選んでいる definition が対象です(`root` では出ません)。
  - メニューを通さず名前で発火したときは、今見ている definition、root ならゾーン(か枠の中の node)を選んでいればその body の definition が対象です。対象が無いか編集できない間は押せません([対象を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L4393))。
- **Rename**: 上段の下に 1 行(`pelican.graphCanvasDefinitionRow`)が開きます。`Rename definition <name> to`、名前の欄(今の名前が入って全部選ばれた状態)、**Rename**、Cancel です([行を開く所](../../src/devstudio/view/graphcanvaswidget.cpp#L4423))。Enter で Rename、Esc で Cancel です。
  - Studio は送る前に名前を確かめ、送れない名前なら欄の右に赤で理由を出して Rename を押せなくします([名前を確かめる所](../../src/devstudio/model/graphcanvasmodel.cpp#L3322))。今の名前のままでも押せません(何も送りません)。
    - 空: `A definition needs a name.`
    - `/` を含む: `A definition's name cannot hold a slash: <name>.`
    - `root`: `A definition cannot be named root: that name is the root document's.`(`root` は root の scope の名前です。engine も断ります)
    - 既にある: `A definition named <name> already exists.`
  - 送るのは `rename_definition` 1 つだけです。engine が同じ op の中で、名指している `graph.instance` の `definition` とループの `branches[].body` も書き換えます(上の「`edit_graph_document` の definition の op」)。
  - 改名した definition の中にいれば、パンくずと今の scope は新しい名前へ移ります。Undo で名前が戻れば、元の名前へ戻ります([移す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1219))。
- **Delete**: 使われていれば、何も送らず行も開かずに、notice に 1 行を出します([断る所](../../src/devstudio/model/graphcanvasmodel.cpp#L3350))。`Definition <name> is used by <node>, <node>: make them run another definition first.` の `<node>` は、その definition を走らせる root の node(instance と、どれかの分岐がそれを body にするループ)を文書の順に 1 つずつ並べたものです。
  - 使われていなければ、上段の下の行が `Delete definition <name>? Nothing runs it.` と **Delete** / Cancel で開き、Delete で `remove_definition` を送ります。確認は dialog ではなくこの行です。
  - engine が断ったときの `graph.definition_referenced@1` の文は、下の拒否の表にあります。
- 開いている行の definition が文書から消えると、行は閉じます。
- 名前は打ちます(今の名前を全部選んだ状態で開くので、打つと置き換わります)。

**Frame Plan のループのグループ(✅LP-4c)**: Frame Plan は `loops[]` と、展開された pass・task・target・buffer の `loop_origin` を読みます。

- 1 つのループが展開した回の pass と task は、1 つのグループ `loop <id> ×N` になります([ループのグループを決める所](../../src/devstudio/view/frameplangraphics.cpp#L532))。
  - グループは最初に見えたときに畳まれ、その後は開いた・閉じたをそのまま保ちます([最初に畳む所](../../src/devstudio/view/frameplangraphics.cpp#L2504))。
  - region や feature のグループより先にループで分けます。lowering graph の無い plan でも、ループのグループは作ります。
- 回の node の詳細に `Loop origin` の行(`loop steps · element 2 · body node fill`)が出ます。target と buffer にも出ます。
- 回の node の右クリックの **Open in Graph** は、✅LP-4d2 から、root の canvas のままゾーンのタブをその要素の分岐に合わせ、枠の中のその node を選びます(上の「縁をまたぐ線・carry・展開の表示」)。body がゾーンに描かれないときは、その要素の分岐の body の scope に入り、body のその node を選びます(LP-4d2 の前はいつもこの形でした)。
- ✅LP-4d2: Graph タブのゾーンの **Show in Frame Plan** は、plan を読み直してから、このグループを開いて回の node を全部選びます(グループの枠そのものは選べないため。[`FramePlanGraphicsScene::selectLoopGroup()`](../../src/devstudio/view/frameplangraphics.cpp#L3711))。
- ✅Q-18 から、Graph タブでループを変えた後は Frame Plan が自分で読み直します(次の段落)。Q-18 の前は、Frame Plan の **Refresh** を押すまで読んだ時点の写しのままでした。

**Graph タブの編集の後は Frame Plan が自分で読み直します(✅Q-18)**: Graph タブの編集が commit すると、Frame Plan は `get_frame_plan` を読み直します。**Refresh** は要りません。

- 知らせるのは、配置(`set_layout` だけの op の列)ではない commit だけです。Graph タブの model がその回数を数え([`GraphCanvasModel::planCommitRevision()`](../../src/devstudio/model/graphcanvasmodel.hpp#L1229))、増えたら Graph タブが MainWindow に知らせ、MainWindow が Frame Plan に渡します([つなぐ所](../../src/devstudio/view/mainwindow.cpp#L490))。
  - 下の「配置の保存の後の編集も通ります」(Q-13)の共有の digest では知らせません。digest は配置の保存でも動き、Frame Plan の node の位置の保存も Graph の `set_layout` なので、それで読み直すと node を drag するたびに plan を丸ごと読み直すことになるためです(コードの注記と実装者の報告)。
- Frame Plan の側([`planChangedByCommit()`](../../src/devstudio/view/frameplanwidget.cpp#L3081)):
  - 見えていれば、すぐ読みます。
  - 前の読みが往復中なら、その返事の後にもう 1 回読みます(往復中の読みは commit より前の plan かもしれないため)。
  - 見えていなければ印だけ付け、次に表示したときに読みます。engine の RPC が無いときは何もしません。
- Graph タブ自身も、配置でない commit の後は、読み直した文書が配置だけ違うように見えても `get_frame_plan` を読み直します。ゾーンの `×N copies` などが回数の変更に追いつくためです([その分岐](../../src/devstudio/model/graphcanvasmodel.cpp#L1211))。
- シナリオ `graph_loop_expand` / `loop_view_axis` / `material_loop_decompose` は、Graph タブの編集の後に Frame Plan の Refresh を押さなくなりました。
- ⚠ MainWindow のつなぎを通して見ているのは GPU シナリオ(`graph_loop_expand`)だけです。CPU の test は、Graph タブが知らせることと Frame Plan が読み直すことを別々に見ます(Q-18 の着地時の記録)。

**ループと definition の拒否は 1 文で出ます(✅LP-4c / LP-4e)**: `graph.loop_*` と `graph.definition_*` の拒否(✅Q-19 から `graph.input_index_invalid@1` も)は、field の JSON のままではなく、1 文と末尾の code で出ます([`graphLoopRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L963)、[文の表](../../src/devstudio/model/inspectormodel.cpp#L844))。

| code | 文 |
|---|---|
| `graph.loop_branch_else_missing@1` | `Loop <node> has no else: its last branch must have no condition` |
| `graph.loop_branch_interface@1` | `Branch <branch> of loop <node> runs <definition>, whose inputs and outputs differ from the loop's at <port>` |
| `graph.loop_nested@1` | `A loop stands in the root document only, and its body cannot hold a loop (<node>)` |
| `graph.loop_feature_pending@1` | `Loop <node> uses <feature>, which this engine does not run` |
| `graph.loop_tier_unsupported@1` | `Loop <node> has no stage it can run in (<reason>)`(draw_loop の reason は下の 4 つの文) |
| `graph.loop_tier_mismatch@1` | `Loop <node> is fixed to <tier>, but it can only run as <actual>` |
| `graph.loop_order_unsupported@1` | `Loop <node> cannot be ordered that way in its stage` |
| `graph.loop_expression_invalid@1` | `An expression of loop <node> is not valid: <reason> at <pointer>` |
| `graph.loop_carry_type_mismatch@1` | `Loop <node> carries <from> into <to>, which takes another type` |
| `graph.loop_element_unknown@1` | `Input <port> of <node> reads element <element>, which the loop it reads does not have` |
| `graph.loop_output_empty@1` | `Output <port> of loop <node> is empty: the loop ran no element` |
| `graph.loop_expansion_limit@1` | `Loop <node> would make <count> copies; the limit is <limit>` |
| `graph.loop_expansion_referenced@1` | `<name> is a name a loop makes, which cannot be referred to; name the loop (<pointer>)` |
| `graph.loop_contract_unsupported@1` | `Loop <node> draws with <contract>, which its stage does not take` |
| `graph.loop_variant_missing@1` | `Loop <node>: material <material> has no <material_variant> variant`(✅LP-4d2 から。それまでは `<variant>` を読んでいました) |
| `graph.loop_phase_crossing@1` | `Loop <node>: material <material> draws in another phase` |
| `graph.loop_override_interface@1` | `Loop <node>: material <material> has no binding <binding>` |
| `graph.loop_override_needs_compiler@1` | `Loop <node> changes shader defines, which needs the runtime shader compiler` |
| `graph.loop_override_compile_limit@1` | `Loop <node> needs <count> shader compiles; the limit is <limit>` |
| `graph.loop_draws_unconnected@1` | `The draws of loop <node> reach <count> inputs: they must feed the draws of exactly one render.material_pass` |
| `graph.definition_role@1` | `Definition <definition> holds draw steps, so only a draw_loop runs it; the instance <node> cannot` |
| `graph.definition_referenced@1` | `Definition <definition> is still used by <referenced_by>` |
| `graph.definition_duplicate@1` | `A definition named <definition> already exists` |
| `graph.definition_missing@1` | `There is no definition named <definition>` |
| `graph.definition_immutable@1` | `<node> is made by an instance or a loop; edit it in its definition` |
| `graph.input_index_invalid@1` | ✅Q-19(engine の LP-1d)。`The numbered inputs of <node> are not 0, 1, 2...: input <input_index> is <reason>`(`reason` が `missing` / `duplicate` なら下の 2 つの文) |

- 行の形は `<文> (<code>).` です。field が無い所は `?` になります。
- `graph.loop_tier_unsupported@1` の `reason` が draw_loop の 4 つなら、何を直すかを言う文に替わります(✅LP-4e):
  - `carry_in_draw_loop`: `Loop <node> draws inside one material pass, where no material hands a value to the next: remove its carry`
  - `limit_in_draw_loop`: `Loop <node> draws every material of its pass: remove its limit`
  - `resource_in_draw_body`: `A body of loop <node> makes a resource, which a draw_loop body cannot: it holds draw steps only`
  - `mixed_body`: `The bodies of loop <node> mix draw steps with passes: a body holds draw steps only (draw_loop) or passes only (expand)`
- `graph.input_index_invalid@1` の `reason` も、それぞれの文に替わります(✅Q-19。[reason の文](../../src/devstudio/model/inspectormodel.cpp#L900)):
  - `missing`: `No port of <node> reads input <input_index>: its numbered inputs run from 0 without a gap`
  - `duplicate`: `Ports <ports> of <node> all read input <input_index>: each number is one port's`(`<ports>` は `a, b` の形)
- fields は、code の横、`error_fields`、別の拒否の message の中の `<code> {…}` / `<code>: {…}` のどこからでも読みます。✅RU-1b から、message の中の field を code の横の同じ名前のものより優先します。RU-1b の当時は、変種の preflight で code の横の `variant` が変種の名前になり、`graph.loop_variant_missing@1` 自身の `variant`(material の変種)が message の中にだけ残ったためです。今はこの field は `material_variant` で、`variant` という名前の field を持つ拒否はありません(下)。
- 出る所は、Graph タブの notice と、Frame Plan の status 行です。render target の form は、変種が断ったときと、ループの code の拒否のときにこの文を使います(後者は ✅RU-1b から。LP-2 で draw_loop の拒否 5 つが最上位の code で届くようになったためです)。
- **変種の前置き(LP-0d)**: 変種(preview / xr)の preflight が断ったときは、code が何であっても前に `The <variant> graph refused it: ` が付きます。LP-0d から、変種の graph の拒否が graph の code のまま `variant` 付きで返るためです(それまで Frame Plan の render target の form は、`render_pipeline_preflight_failed` のときだけ前置きを付けていました)。
  - `graph.loop_variant_missing@1` の material の変種は field `material_variant` で届きます(engine 5e246063)。code の横の最上位の `variant` は、どの拒否でも常に graph の変種で、前置きはそれで付けます(✅LP-4d2。[`graphRefusalVariant()`](../../src/devstudio/model/inspectormodel.cpp#L1020))。
  - ⚠ RU-1b の当時は、この拒否が material の変種を `variant` という名前で運んでいました。そのため、message の `<code>: {…}` に同じ `variant` があれば graph の変種と見なさず、前置きを付けない回避を入れていました(その前は `The outline graph refused it: ` のように付いていました)。LP-4d2 でこの回避を外しました。
- **赤い印**: 拒否が `graph.loop_*` の code で、`node` にループの id を持つとき、そのループの node の縁が赤くなり、hover で文が出ます。詳細表にも `refused` の行が出ます([`GraphCanvasModel::markRefusal()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3633))。
  - 拒否が `loop_origin` を持つとき(回の node が断られた)は、ループと、その要素の分岐の body の中のその node の両方が赤くなります。
  - 次に commit した編集で消えます。

**material pass をループに分解する —— Decompose into loop(✅LP-4e)**: rendering 節(か fork 済みの preset)の一体版の material pass を、`render.material_pass` と、material ごとの分岐を持つ draw_loop のループに書き換えます。書き換えの中身は上の「ループと分解の RPC」の `decompose_material_pass` です。

- 入口は 2 つです。どちらも同じ処理を呼びます。
  - Frame Plan の material pass の右クリックの **Decompose into loop...**(`pelican.framePlan.decomposeIntoLoop`)
  - Pass Editor で material pass を開いたときの **Decompose into loop...** のボタン(`pelican.fullscreenPass.decomposeIntoLoop`)
- 分解できないと分かる pass では、右クリックの項目が押せず、理由が tooltip に出ます([`framePlanDecomposeRefusal()`](../../src/devstudio/view/frameplanedit.cpp#L1529))。
  - feature の pass(feature の `pass_replacements` で置き換える)、material でない pass
  - 既に draw_loop が描いている pass(`<pass> is already a render.material_pass of the graph; its loop <loop> is edited in the Graph tab.`)
  - fork していない engine の preset の pass(`… fork the preset into the project first (Features tab, Fork), then decompose the pass there.`)
- 押すと確認のダイアログ(`pelican.framePlan.decomposeConfirm`)が出ます。ボタンは **Decompose** と Cancel で、既定は Cancel です。
  - 文は、分解で何ができるか(material の種類ごとに、別の variant・別の render state で描く、描かない)と、元に戻せるかを言います。
  - LP-4e の着地時は「まだ元に戻せない(RD-1 で Undo が付く)」と言っていました。RU-1(`fa413aeb`)からは `Undo takes it back (Undo on the Frame Plan, or Ctrl+Z there).` と言い、分解は render の編集の履歴に `Decompose <pass>` として残ります。
  - Cancel は何も送らず、status 行が `<pass> is left as it is.` を言います。
- Decompose を押した時点の context で要求を組み直して送ります([`framePlanDecomposeRequest()`](../../src/devstudio/view/frameplanedit.cpp#L1561))。direct root の pass は `base_source_digest` だけ、fork 済みの preset の pass は `base_preset_digest` も付けます。
- commit されると Graph タブが前に出て、できたループが選ばれます。Graph タブの文書がまだ古ければ読み直し、ループが届いてから選びます。
- engine の拒否は 1 文で、Frame Plan の status 行に出ます([`framePlanDecomposeRejection()`](../../src/devstudio/view/frameplanedit.cpp#L1599))。

| 拒否 | 文(末尾に ` (<code>).`) |
|---|---|
| `material_pass_not_decomposable`(`not_found`) | `Neither the rendering config nor its preset declares a pass named <pass>` |
| 同(`graph_pass`) | `<pass> is already a render.material_pass of the graph: edit its loop in the Graph tab` |
| 同(`feature_pass`) | `<pass> is a pass of the feature <feature>: a feature's pass is replaced through its pass_replacements, not decomposed` |
| 同(`not_material`) | `<pass> is a <type> pass; only a material pass decomposes into a loop` |
| 同(`contract_unsupported`) | `<pass> draws with <contract>, which no draw_loop runs over` |
| 同(`field_unsupported`) | `<pass> sets <field>, which a render.material_pass cannot carry: remove it first` |
| 同(`attachment_unsupported`) | `The graph cannot name <pass>'s <socket> attachment (a subresource, no depth, or more colours than the contract's sockets)` |
| `project_preset_required` | `<pass> is a pass of the engine preset <preset>: fork the preset into the project first (Features tab, Fork), then decompose the pass there` |
| `graph.node_duplicate@1` | `The graph already has a node named <node>, which decomposing would add: rename it first` |

それ以外の拒否は、Frame Plan の他の編集と同じ行(変種の前置きと、上の表の文)で出ます。

**feature をループ版に置き換える —— Replace with loop version…(✅LP-4f2)**: engine が分解版の雛形を同梱している feature(bloom・cube capture・planar reflection の mip chain・directional の影の cascade)を、その雛形のループに置き換えます。中身は上の「ループと分解の RPC」の `apply_graph_template` です。どの feature に分解版があるかは `get_graph_templates` の `decomposes.feature` で決まり、Studio は表を持ちません([`graphTemplatesFromResponse()`](../../src/devstudio/view/frameplanedit.cpp#L1650))。Render Features は一覧を読み直すたびにこれも読みます。`get_graph_templates` を持たない engine では、入口は出ません。

- 入口は 2 つで、どちらも Render Features の同じ処理を呼びます。
  - Render Features で分解版がある feature の行を選ぶと、一覧の下に **Replace with loop version…**(`pelican.renderFeatures.replaceWithLoop`)が出ます。行の右クリックにも同じ項目があります([出し分ける所](../../src/devstudio/view/renderfeatureswidget.cpp#L3228))。分解版の無い feature では出ません。
  - Frame Plan で feature のグループを右クリックすると、同じ名前の項目(`pelican.framePlan.replaceWithLoopVersion`)が出ます([項目を足す所](../../src/devstudio/view/frameplangraphics.cpp#L5016))。押すと Features タブが前に出て、その feature の行で同じ流れが始まります。
- 押せないときは理由が tooltip に出ます。fork していない engine の preset の feature(`<feature> is a feature of the engine preset <preset>: fork the preset into the project first (Features tab, Fork), then replace the feature there.`)、編集できない状態、Apply していない編集がある、のどれかです。
- 押すと確認のダイアログ(`pelican.renderFeatures.replaceConfirm`)が出ます。ボタンは **Replace** と Cancel で、既定は Cancel です([文を組む所](../../src/devstudio/view/frameplanedit.cpp#L1715))。
  - 1 行目は雛形の title(と description)です。例: `Bloom loop`。
  - 続けて何が起きるか: `The feature bloom is removed, and the graph draws what it drew.`(`pass_replacements` の雛形なら `The feature <feature> stays; N of its passes are replaced by <node>.`)、`The graph gains N nodes and M definitions.`、足す define があれば `Shader defines added: …`。
  - 最後に `Undo takes it back (Ctrl+Z here, or Undo on the Frame Plan).`
  - Cancel は何も送らず、`<feature> is left as it is.` を言います。
- Replace を押した時点の一覧から要求を組んで送ります。direct root の feature は `base_source_digest` だけ、project の preset が列挙する feature はその行の preset の digest も `base_preset_digest` に付けます([送る所](../../src/devstudio/view/renderfeatureswidget.cpp#L3154))。
- commit されると、Graph タブが前に出て、雛形が足したループ(応答の `applied.nodes` のうち雛形が `graph.loop` と宣言したもの)が選ばれます。Render Features の notice は緑で `Replaced bloom with Bloom loop. Undo takes it back.` を言い、一覧を読み直す間も残ります。
- 置き換えは render の編集の履歴に `Replace bloom with Bloom loop` として積まれ、Undo / Redo で戻せます(下の「render の編集を戻す」)。
- engine の拒否は 1 文で notice に出ます([`graphTemplateRejection()`](../../src/devstudio/view/frameplanedit.cpp#L1748))。

| 拒否 | 文(末尾に ` (<code>).`) |
|---|---|
| `graph_template_unknown` | `The engine does not ship the graph <template>` |
| `graph_template_invalid` | `The graph <template> cannot be taken in: <reason>` |
| `graph_template_feature_absent` | `Neither the rendering config nor its preset lists <feature>, which the graph replaces: add the feature, or refresh the list` |
| `graph_template_pass_replacement_conflict` | `The feature <feature> already replaces its pass <pass> with <current>, and the graph would replace it with <requested>: remove that replacement first` |
| `project_preset_required` | `<feature> is a feature of the engine preset <preset>: fork the preset into the project first (Features tab, Fork), then replace the feature there` |
| `graph.node_duplicate@1` | `The graph already has a node named <node>, which the loop version adds -- perhaps it is in already (Undo takes an earlier one back), or rename that node first` |
| `graph.definition_duplicate@1` | `The graph already has a definition named <definition>, which the loop version adds -- perhaps it is in already (Undo takes an earlier one back), or rename that definition first` |

`<feature>` は参照のファイル名の部分です(`engine://features/bloom.json` → `bloom`)。それ以外の拒否は render の編集の拒否の行で出ます。

⚠ 分かっている穴(LP-4f2 の着地時の記録): Frame Plan の feature のグループは lowering graph があるときだけ出るので、Frame Plan の入口もそのときだけです(実装者の報告)。影の cascade の置き換えは、影を持つ fixture が無いため CPU の test だけで、GPU シナリオでは通していません。

**draw_loop の Loop の欄と分岐の when(✅LP-4e)**: `over: {draw_items: …}` のループ(分解が作るもの)では、Loop の欄が段に合わせて変わります([欄を出し分ける所](../../src/devstudio/view/graphcanvasloopfields.cpp#L576))。

- 出さない欄: Count・Order・Limit・Place・Where(element)・Outputs・Carry。draw_loop の順は phase の既定で、limit と carry は engine が断ります。
- 出す欄:
  - **Contract**(読むだけ。`forward_opaque_v1 (the pass forward_opaque)` の形)。ループの draws がつながる material pass の契約です。
  - **Tags**: `where.tags` の any of(`include`)/ none of(`exclude`)/ all of(`require_all`)の 3 つの欄。tag は `,` か空白で区切り、Enter で送ります。3 つとも空なら `where` を消します。
- 分岐表の各行は 3 段です([`GraphCanvasLoopFields::addDrawBranchRow()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1073))。
  - 1 段目: body の combo と ↑ ↓ ×(else の行は body の combo だけ)
  - 2 段目: 条件の欄。**material**、**tags**、**contract**、**skinned**、**has variant**
  - 3 段目: 当たった material(`materials: <鍵>, …`。当たらなければ赤で `no material takes this branch`、報告の前は `materials: not reported yet`)
- 条件の欄の中身:
  - **material**: `(any)` と、報告の `elements` に出た material の照合の鍵(`materials/g1_flat.material.json#g1_flat` など)。鍵を打つことはありません。
  - **tags**: any of(`include`)だけを編集します。文書が持つ `exclude` / `require_all` はそのまま残ります。
  - **contract**: `(any)` / `gbuffer_v1` / `forward_scene_color_v1` / `legacy_gbuffer_v1`
  - **skinned** と **has variant**: `(any)` / `yes` / `no`
  - 選んだものを全部満たす material が当たります(AND)。全部 `(any)` にした行は送りません(条件の無い行は最後の else だけです)。
- material の候補は、報告が 1 ページに収まらない(`total` が `elements` より多い)とき、Graph タブが `loop_elements` で残りを読んで全部集めます([`GraphCanvasModel::requestNextLoopPage()`](../../src/devstudio/model/graphcanvasmodel.cpp#L734))。
- **Add branch** の新しい行の条件は、報告の最初の material の `material = <鍵>` です。報告の前なら `has_variant = yes` です。
- body の combo の `(nothing)` は、その分岐に当たった material を描かないことです。

**render.material_pass と draw.* の node(✅LP-4e)**:

- `render.material_pass` のソケットは契約から出ます([`GraphCanvasModel::materialPassColorSockets()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3784))。
  - forward(`forward_opaque_v1` / `forward_transparent_v1`)は `color`、G-buffer(`deferred_geometry_v1`)は `albedo` / `normal` / `material` / `worldpos` / `emissive` です。`material_outputs` を宣言した pass では、その出力の名前です。
  - 続けて `depth` と、任意の `draws` です。出力ソケットはありません(書いた target は `render.target_ref` で読みます)。
- palette の `render.material_pass` は root にだけ出ます。palette の Contract の combo(`pelican.graphCanvasPaletteContract`。`forward_opaque_v1` / `deferred_geometry_v1` / `forward_transparent_v1`)で契約を選びます。
  - ソケットごとの `render.image`(`<pass>__<socket>`)と depth の image を同じ列で足してつなぎます。`draws` は空けます(空けると一体版と同じ描き方です)。
  - image の format は、`material` のソケットだけ `R8G8B8A8_UNORM` で、それ以外の色のソケット(forward の `color`、G-buffer の `albedo` / `normal` / `worldpos` / `emissive`)は `R16G16B16A16_SFLOAT`、depth は `D32_SFLOAT` です([image を足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L2514))。コードのコメントでは、`material_outputs` の無い material pass を engine は互換の format の組と比べ、最初の attachment が `R16G16B16A16_SFLOAT` なら HDR の組を選び、その組で 8 bit なのは `material` だけです。
  - `0c252183` の前は `albedo` と `emissive` も `R8G8B8A8_UNORM` で作っていたので、deferred の pass は `Material pass color output format mismatch: <pass>__albedo` で断られ、engine が止まりました。commit の記録では、白紙の project に置いた文書を headless の player で走らせ、前は止まり後は描けることを確かめています。GPU シナリオはありません。
- material pass を選ぶと、詳細表の上に **Contract** の combo(`pelican.graphCanvasDraw.contract`)が出ます([`GraphCanvasDrawFields::showNode()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1314))。
- `draw.material` / `draw.variant` / `draw.override` は、draw_loop が走らせる body(か、既に draw step を持つ definition)の中でだけ palette に出ます。
  - 置いた step は body の列の**先頭**に入ります。今の先頭の `after` を新しい step につなぎ、`draws` を作る step は末尾のままです([draw step を置く所](../../src/devstudio/model/graphcanvasmodel.cpp#L2536))。
- `draw.variant` の欄は **Variant**(`pelican.graphCanvasDraw.variant`。打てる combo)です。候補は、ループが描く pass の `material_variant` です。置いたときもその値が入り、無ければ `variant` です。
- `draw.override` の欄:
  - **Cull**(`none` / `front` / `back`)、**Depth test** / **Depth write**(`on` / `off`)、**Blend**(`opaque` / `blend` / `additive`)。どれも先頭の `(material's)` は鍵を書かず、material の値のままにします。
  - **Defines** は 1 行に 1 つ(`NAME` か `NAME=VALUE`)で、**Apply defines** で送ります。
  - 何も替えない override は送りません。置いたときは `render_state: {cull: none}` が入っています。

**Frame Plan の material pass の loop の行(✅LP-4e)**: draw_loop は展開されず、1 つの material pass の中で走ります。

- draw_loop が描く material pass は、見出しの下に `loop: forward_opaque_draws (3 materials)` の 1 行を持ちます([`framePlanDrawLoopLine()`](../../src/devstudio/view/frameplanedit.cpp#L1521))。
- 詳細に `Draw loop <id>` の組が出て、分岐(`Branch 0` …)ごとに当たった material の鍵が並びます([詳細の material の一覧](../../src/devstudio/view/frameplanwidget.cpp#L3811))。
  - 当たらない分岐は `none: no material takes this branch` です。
  - plan が持つ要素が `total` より少ないときは、`<k> of <total> shown` と **Read more**(`pelican.framePlan.loopMaterialsMore`。256 件ずつ読む)が出ます。

**分かっている穴**(LP-4c / LP-4e の着地時の記録):

- definition の改名・削除の UI はありません(model には op があり、Undo もできます)。
- ✅Q-18 で塞がりました: この時点では、Graph タブの `↻ expand ×N` と Loop の欄の報告は Graph タブが文書を読むときにだけ読み直し、Frame Plan は Graph タブの編集の後に自分からは読み直さないので、**Refresh** が要りました。今は Graph タブの commit の後にどちらも読み直します(上の「Graph タブの編集の後は Frame Plan が自分で読み直します」)。
- Place は canonical anchor だけで、式の条件(`when` の式)は表では編集できません。
- 分岐の tags は any of(`include`)だけを編集できます。`has_variant` に文字列を書いた `when` は、表では表せません。
- Frame Plan の回の node からの **Open in Graph** は、Graph タブの側(body の node を開く所)だけを test していて、Frame Plan から Graph タブへのつなぎは test していません。詳細の **Read more** も、ボタンが出ることだけを見ていて、押して次のページを読む所は test していません。
- **Per iteration…** は CPU の test だけで、GPU シナリオでは通していません。
- `render.target_ref` の parameters は、まだ値の帯(pill)として出ます。
- palette で置く `render.material_pass` を GPU で描いた確認はありません。

**graph の外から来た pass を畳んだ node で見る(✅GL-1)**: Graph タブの root の canvas は、graph の node が作っていない pass も描きます。rendering 節の pass、preset の pass、feature の pass です。どれも読むだけの node で、文書には何も書きません(設計は [design_graph_layers](../design_graph_layers.md) §7 の GL-1)。

- **plan を毎回読みます。**Graph タブは文書を読むたびに `get_frame_plan` を 1 回読みます([`GraphCanvasModel::requestLoopReports()`](../../src/devstudio/model/graphcanvasmodel.cpp#L714))。GL-1 の前は、ループや view_select を取る render node などがあるときだけ読んでいました(上の「ループの node」)。読みが 1 つ増えます。
  - engine がまだ最初の plan を持たないときは、失敗した読みを繰り返します。回数の上限は 480 で、コードのコメントでは poll の 2 分ぶんです([待つ所](../../src/devstudio/model/graphcanvasmodel.cpp#L930))。
- **何が出るか**([`GraphCanvasModel::rootFoldedNodes()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4501)):
  - `provider_feature` を持つ pass は、feature ごとに 1 つの畳んだ node にまとまります。見出しは feature の名前(`engine://features/bloom.json` なら `bloom`)、型の所に `feature`、見出しの下に `N passes` です。
  - それ以外の pass は 1 pass 1 node です。型の所は plan の `kind` で、material pass は `material` です。
  - material pass かどうかは plan から決めます。`shader_resolution.state` が `material_owned`、`material_filter` がある、`material_variant` がある、のどれかです([`GraphCanvasModel::readPlanPasses()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4359))。
  - `kind` が `anchor` の node は出ません。
  - node の行は資源の名前です。右が外へ渡す資源(書く)、左が外から読む資源です。feature の中の pass 同士で受け渡す資源は出ません。
- **見た目**: 枠は破線で、地の色は graph の node より 1 段落ちます。theme の token は `derived_border` / `derived_body` です([theme の表](../../src/devstudio/resources/studio_themes.json#L65))。見出しの帯は Frame Plan の出所の色を薄くしたものです。
  - tooltip は `From the rendering section (<source>): read only here. Frame Plan and Features edit it.` です。潜れる node では `Double-click to look inside.` が続きます([`Builder::addDerivedNode()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L1315))。
- **置き場所(✅GL-1b / GL-1c)**: root の graph の node と畳んだ node を、1 回の配置でまとめて並べます。派生の線も配置の辺に入るので、列は plan の依存の最長経路で決まり、plan の順が左から右へ読めます。Frame Plan の Logical graph と同じ [`layoutNodeGraph()`](../../src/devstudio/view/nodelayout.cpp#L238) です([配置に入れる所](../../src/devstudio/view/graphcanvaswidget.cpp#L1567))。
  - 文書の root の `layout` が graph の node を 1 つでも置くと、その配置はもう graph の node の場所を表しません。このとき畳んだ node は、畳んだ node とその間の線だけで同じ配置をして、root に描いたものの左に並べます。
  - root に描いたもの(✅GL-1c)は、node とその子(ソケット、箱からはみ出す値の字)と、ゾーンの枠全体(見出し・縁の行・carry の矢印・下端の行)です([測る所](../../src/devstudio/view/graphcanvaswidget.cpp#L1665))。コードのコメントでは、1 回の配置の中でゾーンは枠の大きさの 1 つの箱なので、そこでも畳んだ node は枠に重なりません。
  - 並べた場所はセッションの間覚えます。覚えた場所のどれかが root に描いたものに重なる(後で body が埋まってゾーンが大きくなった、node をその上に置いた)か、場所の無い畳んだ node があれば、並べ直して覚え直します([判定](../../src/devstudio/view/graphcanvaswidget.cpp#L1697))。
  - 文書に配置が無い間に graph の node を drag すると、畳んだ node はそのときの場所を覚えます。
  - drag した畳んだ node は、セッションの間その場所に残ります。文書が変わっても戻りません。
  - 畳んだ node の場所は文書に書きません(`set_layout` に入りません)。Auto layout での扱いは下の「Auto layout」です。
  - GL-1 の着地時は、graph の node の左に 1 列、plan の順で縦に並べ、drag した場所も文書が変わると戻っていました。GL-1b の着地時は、Auto layout で覚えた場所を graph の node とだけ比べていたので、後で大きくなったゾーンの上に畳んだ node が乗りました(`loop_zone_wire` で、picking の畳んだ node がゾーンの Show in Frame Plan のボタンを覆い、押すと畳んだ node が選ばれた)。GL-1c で直しました。
  - 分かっている穴(GL-1c の着地時の統合側の報告): root の node の長い値の字(image の `type` の JSON など)は省略されず、左へ大きくはみ出します。コードの上では、はみ出した字も「描いたもの」に入るので、畳んだ node はその分さらに左に並びます。
- **選ぶと**、詳細表の上に 1 行の要約(`pelican.graphCanvasFoldedSummary`)が出ます。`<名前> (<型>): `、feature なら `N passes of <feature>.`、`Reads …; writes ….`、最後に上の tooltip の文です。
- **派生の線**: plan の `reads` / `writes` / `reads_history` から、資源の名前で破線を引きます([`GraphCanvasModel::derivedEdges()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4549))。
  - 書き手は、読む pass より前で最後にその資源を書いた pass です。history の読みでは frame 全体の最後の書き手です。
  - 資源を名乗る graph の node は、その資源を `parameters.name` に持つ `render.target_ref` と、同じ id の `render.image` / `render.buffer` / `render.attribute` です。graph の node の pass が読む側なら、線はその node に付け替えます。plan に書き手が無い資源は、その node から引きます。それ以外は、pass を作った graph の node に付けます。
  - 少なくとも片方の端が畳んだ node の線だけを引きます。同じ 2 つの node の間の資源は 1 本にまとめます。tooltip は `<資源>, …: from the frame plan (read only)` です([`Builder::addDerivedEdge()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L1411))。
  - 破線をダブルクリックしても制御点は入りません。それ以外の操作で op が出ないことは test では見ていません(未確認)。
- **潜る**: 畳んだ node をダブルクリックすると、中を読むだけで見せます([`GraphCanvasModel::enterFolded()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5116))。パンくずは `root / forward_opaque` のように畳んだ node の名前を足し、押すと 1 段ずつ戻ります。`Scope:` のコンボで選ぶと、本物の文書のその scope に戻ります。
  - **material pass**: `render.material_pass` 1 つと、draw_loop のゾーン 1 つです。ゾーンの id は `<pass>_draws` で、分岐は else 1 つ、body `<pass>_draw` は `draw.material` 1 つです(pass に `material_variant` があれば `draw.variant`)。Studio が plan からその場で組み、どこにも書きません([`GraphCanvasModel::foldedDocument()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4988))。
  - 契約は plan の `material_routing` から取ります。取れなければ、`render.material_pass` のソケットは `depth` と `draws` だけです(実装者の報告)。
  - plan の `loops[]` にこのループは無いので、ゾーンの段は `draw_loop` です。plan の `material_filter.resolved_draw_count` があれば、その数を要素の数にします。
  - **feature**: その feature の pass を plan の順に 1 つずつ、上と同じ読むだけの node で並べ、その間の派生の線も引きます。中の material pass にはさらに潜れます。
  - その feature の雛形が `get_graph_templates` にあれば、パンくずの下に **Show decomposed version** のボタンが出ます。押すと雛形の `nodes` と `definitions` を読むだけで描き(ループはゾーン)、パンくずに `<feature> (decomposed)` が足されます。
  - `get_graph_templates` は、plan に feature の pass があるとき、接続ごとに 1 回だけ読みます([読む口](../../src/devstudio/view/graphcanvasreadcapability.hpp#L59))。持たない engine ではボタンが出ません。
  - それ以外の 1 pass の node は潜れません(ダブルクリックしても何も起きません)。
  - 中の絵は、Studio が組んだ文書を本物と入れ替えて描いています。engine の返事を読む間だけ本物に戻し、読んだ後で組み直します(コードのコメント)。中にいる間に文書を読み直しても、同じ中身が組めればそのまま残ります。
- **中で編集しようとしたとき**: 線を引く・値を変える・node を足す・消すなど、op を送る操作は何も送りません。canvas の上に 1 行 `This is the engine's folded implementation: expand it to edit.` と **Expand to edit…** のリンクが出ます(`pelican.graphCanvasFoldedNotice`。[`GraphCanvasModel::refuseFoldedEdit()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5190))。
  - 配置だけの列(`set_layout`)は、何も言わずに捨てます。
  - リンク(action `pelican.graphCanvas.expandToEdit`)は、今ある流れを確認のダイアログ付きで始めます([`GraphCanvasModel::foldedExpand()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5168))。rendering 節の material pass なら Frame Plan の **Decompose into loop...**(上の「material pass をループに分解する」)、雛形のある feature(とその中)なら Render Features の **Replace with loop version…**(上の「feature をループ版に置き換える」)です。どちらにも当たらなければリンクは出ません。
  - 1 行は、畳んだ中から出ると消えます。
- **領域の filter**: 上段の操作列に `Show:` と 5 つの check が並びます(`Render` / `Pose` / `Value` / `Audio` / `UI`。名前は `pelican.graphCanvasShow.<領域>`)。既定は `Render` だけです([既定を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L3682))。状態はセッションの間だけで、文書には書きません。
  - 領域は op の接頭辞で決めます。`render.*` と `draw.*` は Render、`value.*` と `event.*` は Value、`pose.*` / `audio.*` / `ui.*` はそれぞれの領域です。それ以外(`graph.instance` と `graph.loop` を含む)は常に出ます([`GraphCanvasModel::opDomain()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5207))。
  - 効くのは root の canvas だけです。definition の中と畳んだ中では全部出ます。Render を外すと、畳んだ node も出ません。
  - 隠れた node と見えている node の間の線は描きません。代わりに、見えている側のソケットに小さな口が出ます。入力側は `← value (N)`、出力側は `→ pose (N)` の形で、N は隠れたその領域の node の数です。押すとその領域が filter に足されます([`GraphCanvasModel::domainStubs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5238))。
  - Inspector や Frame Plan から、隠れた領域の node を開くと、その領域が足されます。
- **Frame Plan の Open in Graph は全 pass に出ます**: graph の node が作っていない pass では、Graph タブを前に出し、本物の root に戻ります。その pass が描かれている node(feature の pass なら feature の畳んだ node、それ以外はその pass の node)を選び、中央に置きます([`GraphCanvasWidget::revealPlanPass()`](../../src/devstudio/view/graphcanvaswidget.cpp#L5291))。material pass でも潜りません。
  - Graph タブがまだその pass を読んでいなければ、Frame Plan の status 行が `The Graph tab has not read pass <pass> yet: refresh it.` を言います([分ける所](../../src/devstudio/view/frameplanwidget.cpp#L2972))。
  - ✅GL-2b から、行き先は plan の `origin` で決めます([`GraphCanvasModel::planNodeTarget()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4765))。instance の中の node の pass なら、instance を通ってその body に降り、node を選びます。ループの回ならゾーンの中の body の node で、ゾーンのタブをその要素が走った分岐に移します。それ以外は root の node か畳んだ node です。`origin` を出さない engine では、ループの回は `loop_origin` で、他は下の名前の一致で決めます。
- **graph の pass かどうか**は 1 か所で決めます([`GraphCanvasModel::graphOwnerOf()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4457))。
  - ✅GL-2b から、engine が `origin` を出していれば(✅GL-2a。§13.5 の「`get_frame_plan` に足されたもの」)それだけで決めます。`origin` の無い pass は、名前や `source` が何であっても、preset・feature・rendering 節の pass として畳みます。`origin` のある pass は、`instance_path` があればその先頭の root の instance、`loop` があればそのループ、どちらも無ければ `node` の下に描きます。
  - engine が `origin` を出すかどうかは、plan に `diagnostics` か `deactivated_graph_nodes` の鍵があるか、どれかの node に `origin` があるかで見ます([`GraphCanvasModel::readPlanLayers()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4649))。
  - それより古い engine では GL-1 の判定に戻り、文書の側から次の順に名前で当てます。
    - `loop_origin` のループ
    - root の `graph.instance` の id に `/` を付けた接頭辞
    - pass の名前と同じ id の root の node
    - `authored_name` と同じ id の root の render node
    - どれにも当たらない pass を、graph の外の pass として畳みます。
- **分かっている穴**(着地時の記録):
  - 判定は名前の一致です。instance の definition の中の node の `authored_name` には当たりません(比べるのは root の node だけ。コードで確かめました)。✅GL-2b から、`origin` を出す engine ではこの判定を使いません。
  - 設計 §7 の受け入れ「Release で `deferred_geometry` が Graph に出て潜れる」は、GPU シナリオでは forward の `forward_opaque` で見ています。`deferred_geometry` と Release では見ていません(未確認)。
  - feature の畳んだ node と Show decomposed version は model の test だけで、GPU シナリオでは通していません。

**下の層を node で見る(✅GL-2b)**: root の node の下に、plan で何になったかの帯を出し、詳細表の上に **Layers** の節を出します。どちらも読むだけです(設計は [design_graph_layers](../design_graph_layers.md) §3.2・§7 の GL-2b)。

- **出る条件**: 本物の root(definition の中や畳んだ中ではない)で、`Show:` が Render だけで、plan を読めているときです([`GraphCanvasModel::layersShown()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4822))。Render のほかの領域を足すと、帯も Layers も消えます。
- **node と plan の node の対応**: root の node ごとに、その下に描かれている plan の node を plan の順で集めます([`GraphCanvasModel::planNodesOf()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4810))。graph の node が作った pass はその node(instance の中の node なら root の instance、ループの回ならループ)、そうでない pass は畳んだ node です。anchor は数えません。
- **帯**([`GraphCanvasModel::layerBand()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4832)): 既定と違うことだけを ` · ` でつないだ 1 行で、node の枠の下に薄い字で描きます(畳んだ node にも付きます。自動操作の名前は `graph_layer_band#<node>`。[描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1491)、[付ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L1785))。言うことが無ければ出ません。
  - `×N copies`: node が plan の中で 2 つ以上の複製(instance の path・ループ・要素の組)になったとき。ループではゾーンの下端の行が回の数を言うので、帯には出しません(✅GL-1b から。GL-2b の着地時は出していました)。
  - `N barrier` / `N barriers`: その node の pass が端のどちらかにある barrier の数。
  - `aliased`: その node の pass が `from_nodes` に入る alias group があるとき。
  - backend の名前: pass の `execution_plan` の `selected_endpoint` が graph の backend と違うとき。graph の backend は pass の中で一番多い `selected_endpoint`、どの pass も言わなければ選ばれた候補の `endpoint` です([`GraphCanvasModel::graphBackend()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4746))。
  - `deactivated: <reason> (<feature>)`: plan の `deactivated_graph_nodes` にある root の node。この node は薄く(不透明度 0.45)描きます。
  - tooltip は `<帯> below the graph: select the node for its Layers` です。
- **Layers の節**(`pelican.graphCanvasLayersToggle` と `pelican.graphCanvasLayers`。[`syncLayers()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4541)): root の node を選ぶと見出しが `Layers: <node>`、何も選ばないと `Layers: the whole graph` です。見出しを押すと畳めます(`▾` / `▸`)。行が無ければ節ごと隠れます。definition やゾーンの中の node を選んでいる間は出ません。
  - 行は `L1 expanded` / `L2 physical` / `L3 backend` の 3 つの組に分かれます([`GraphCanvasModel::layerRows()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4876))。
  - L1: 展開した plan の node(`<名前> · <kind か material>`。ループの回なら ` · loop <loop> [<要素>]`、instance の中なら ` · instance <path>`)、`replaces` のある node の `<pass> replaces pass <pass> of <feature>`、`deactivated: <reason> (feature <feature>)`。feature の畳んだ node では、graph の node に置き換えられた pass を `pass <pass> is replaced by graph node <node>` で言います。
  - L2: その node の pass が `from_nodes` に入る resource(`resource <名前> · <format>`。alias group に入っていれば ` · alias <id>`)、alias group(`alias group <id>: <resource, …>`)、scope(`scope <id> (<kind>)`)、barrier(`<from> → <to> · <resource> · <kind>`。両端の `execution_plan` の `resource_uses` に access があれば ` · <from の access> → <to の access>`)。最上位の `barriers` は layout を持たないので、barrier の行に layout は出ません(実装者の報告)。
  - L3: `from_nodes` がその node の pass に当たる決定(`<id> · <subject> → <selected>`)と、graph の backend と違う pass の backend(`backend <endpoint> (<pass>)`)。
  - 何も選ばないときは graph 全体のもので、`from_nodes` が空の決定と、backend の候補のすべて(`candidate <名前> · <endpoint> · selected|feasible|rejected`)です([`GraphCanvasModel::graphLayerRows()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4966))。
  - plan の node を持つ行(L1 の pass・置き換えの行と L3 の backend の行)を押すと、Frame Plan が前に出て、Logical graph のタブでその node を選び、中央に置きます。その node を含むグループは開きます([選ぶ所](../../src/devstudio/view/frameplangraphics.cpp#L3743))。status 行は `<node> is picked.`、plan に無ければ `The frame plan has no node <node>: refresh it.` です。行の tooltip は `Click to pick <node> in the Frame Plan.` を足します。
- **診断**: plan の `diagnostics` が 1 つ以上あれば、canvas の上の notice の下に `▸ N diagnostics`(1 つなら `1 diagnostic`)の 1 行が出ます(`pelican.graphCanvasDiagnostics`)。押すと開いて、`<kind> · <subject>: <detail>` の一覧(`pelican.graphCanvasDiagnosticsList`)が出ます([`syncDiagnostics()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4593))。この 1 行は領域の filter によらず出ます。
- **分かっている穴**(着地時の記録。実装者の報告を含む):
  - 帯と Layers は `Show:` が Render だけのときだけです。
  - backend の候補は node に結ばず、何も選ばないときに全部出します。engine の候補の `from_nodes` は実物で全部 `[]` です(§13.5 の「`get_frame_plan` に足されたもの」)。
  - feature の pass が全部 graph の node に置き換わると、feature の畳んだ node が無いので、`is replaced by` の行も出ません。

**Frame Plan の表の行から開く(✅GL-2b)**: Frame Plan の Passes / Resources / Physical plan / Decisions / Backends / Barriers のタブで行を右クリックすると、**Open in Graph** が出ます([右クリックの所](../../src/devstudio/view/frameplanwidget.cpp#L2915))。行き先は上の「Frame Plan の Open in Graph は全 pass に出ます」と同じです。

- **行が名指す pass**: Passes はその pass、Resources は `from_nodes`(無い古い plan では書き手と読み手)、Physical plan は alias group の `from_nodes`・候補の組(pass の組か、resource の組ならその `from_nodes`)・scope の `nodes`・lowering の node の元・依存の両端、Decisions は `from_nodes`(subject の見出しの行は、その下の決定の和)、Backends は候補の `from_nodes` と拒否の `subject`、Barriers は両端です。plan に在る pass だけを残し(anchor は外します)、行が何も持たなければ親の行のものを使います。
- 名指す pass が 1 つなら項目 1 つ、2 つ以上なら **Open in Graph** の下に pass の名前の submenu です。1 つも無ければ押せない項目になり、tooltip は `This row names no pass: it is about the whole graph.` です。
- 名前の付いた action `pelican.framePlan.openRowInGraph` は、今のタブで選んだ行の最初の pass を開きます([action](../../src/devstudio/view/frameplanwidget.cpp#L1218))。行が pass を名指さなければ、status 行が `Select a row that names a pass first.` を言います。
- 各タブにも action `pelican.framePlan.tab.<タブの id>`(Barriers なら `pelican.framePlan.tab.barriers`)が付き、発火するとそのタブを前に出します。表にも名前が付きます(`pelican.framePlanPasses` / `pelican.framePlanResources` / `pelican.framePlanPhysical` / `pelican.framePlanDecisions` / `pelican.framePlanBackends` / `pelican.framePlanBarriers` / `pelican.framePlanMaterials`)。
- シナリオ `graph_layers_origin` は Barriers の 20 行目と 9 行目を開きます。この番号は preset の pass の並びに依ります(実装者の報告)。

**engine の再起動が要る graph の編集(✅GL-1b)**: engine の RS-1(`247ed20e`)は、生きている material の物理の描画の契約を変える graph の編集を断らずに書き、commit の結果に `restart_required: true` と `restart_reason` を付けます。`restart_reason` は `code` と、`detail`(`color_formats`・`depth_format` などの一覧)などを持ちます。

- Studio はこの commit の後に文書も plan も読まず、notice に `Restarting the engine to apply this change (<code>)...` を出して、Frame Plan の再起動と同じ [`MainWindow::requestEngineRestart()`](../../src/devstudio/view/mainwindow.cpp#L1420) に渡します([受ける所](../../src/devstudio/model/graphcanvasmodel.cpp#L1114))。未保存の scene の編集があれば、Frame Plan のときと同じ確認(✅G-5)が出ます。
- 確認で Cancel したとき(か、別の問いの最中で見送ったとき)は、notice が `The engine was not restarted. The setting is saved and applies the next time the engine starts.` を言い、走っている engine から文書と plan を読み直します([`GraphCanvasModel::engineRestartDeclined()`](../../src/devstudio/model/graphcanvasmodel.cpp#L665))。
- 読むのは `restart_reason.code` だけです。GL-1b の着地時は `detail` を文字列として読んでいたので、engine が送る一覧で `type must be string, but is array` の例外になり、編集が断られたように出て engine は再起動しませんでした(`43317a76` で直しました。統合側が Release を仮想モニタで操作して見つけました)。
- 再起動の後、文書が同じなら Graph タブの Undo はそのまま効きます(✅OT-1。§13.5 の「シーン編集との違い」)。
- RS-1 より前の engine は、この編集を `changes the physical rendering contract of live material` を含む文で断ります。Studio はその文の後に ` — this change needs an engine restart, which the engine does not offer for graph edits yet` を足し([句を足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1084))、notice の 1 行を切り詰めるときもこの句は残します。

**位置と折り畳みはセッション内の状態です。**`(scope, ノード id)` を鍵に覚えていて、**文書には書きません**。
revision が変わって消えたノードの位置は捨てられます。初期配置は最長経路による段組みで、辺は必ず左から右へ進みます。

**ズーム・パン・fit は 2 つのキャンバスで同じです(N-3)。**ホイールはカーソルの下の点を動かさずに拡大・縮小し、
倍率は **0.25〜4.0** で止まります([ホイール](../../src/devstudio/view/nodegraphics.cpp#L575))。パンは**中ボタン**ドラッグ、
または **Space を押しながら左**ドラッグ([判定](../../src/devstudio/view/nodegraphics.cpp#L676))。**`F`** キーと
右クリックメニューの **Fit view** で内容全体が view に収まります(Graph タブの右クリックは「Add node... / Fit view」の
2 項目です。[メニュー](../../src/devstudio/view/graphcanvaswidget.cpp#L2702))。この 3 つの操作は共有部品
[`NodeCanvasView`](../../src/devstudio/view/nodegraphics.hpp#L203) が 1 か所で持っていて、**Graph タブも Frame Plan の
Logical graph も同じ view** です。**まだ見ていない scope は自動で fit** され、戻ってきた scope は離れたときの倍率と
スクロール位置に復帰します([`showScope()`](../../src/devstudio/view/nodegraphics.cpp#L649)) —— 倍率も位置も文書には
書かない、セッション内の状態です。値の帯やラベルを編集している最中の `F` はただの文字です(開いている editor が
キーボードを持ちます。[条件](../../src/devstudio/view/nodegraphics.cpp#L717))。

**テーマはライトとダークの 2 つです(✅N-5)。**切り替えは **View ▸ Theme ▸ Light / Dark**
([メニューの構築](../../src/devstudio/view/mainwindow.cpp#L946))で、**再起動は要りません** —— 選んだ名前は
QSettings の [`studio/theme`](../../src/devstudio/view/studiotheme.cpp#L26) に残り、次に開いた窓は
そのテーマで始まります。追随するのは **Graph タブと Frame Plan の両方のキャンバス**で、背景・grid・辺・
ノードの body と縁・見出しの字・値の帯の色をテーマの表から引き直して描き直します。**ソケットの型別の色だけは
テーマに依存しません**(型の識別色なので、どちらのテーマでも同じ色です。テーマが決めるのは縁と、どの型とも
言えないソケットの色)。既定は `dark` で、`dark` のキャンバスの色は N-5 以前の色と同じ値です(完了記録
`6a26eef3`)。

**Auto layout(✅N-4)**: Graph と Frame Plan の右クリック menu「Auto layout」(`pelican.graphCanvas.autoLayout` / `pelican.framePlan.autoLayout`)で、その scope の手置き位置を捨てて層配置(最長経路の列、辺は lane を通って node を横切らない、列内の並びは重心法、行は最小間隔で重ならない)に戻します。動かした node は通常の refresh では保持されます。中ボタンのパンは描画が view に収まっていても動きます。✅GL-1b から、Graph の root では、その view の畳んだ node の場所(drag したものを含む)も捨てて、graph の node と同じ 1 回の配置に入れます。文書に書くのは graph の node の場所だけで、畳んだ node の場所はセッションの間だけ覚えます(上の「graph の外から来た pass を畳んだ node で見る」の置き場所)。

**応答性(Q-2)**: Studio は何も起きていない間は engine にほぼ問い合わせません(modal transform は gesture 中か engine 窓が focus を持つ間だけ、scene tree は revision が動かなければ 2 秒に 1 回、GPU 計時は panel が見えている間だけ)。Frame Plan の応答は 1 回だけ parse し、キャンバスは背景と grid を cache します。それでも Debug build は Release の 6〜19 倍遅いので、体感を見るなら Release 版で。project を開いた直後の固まりは Q-4 で消えました: Studio は engine の窓 thread が応答するまで埋め込みを待ち、その間は viewport に「Loading …(n s)」を出します。engine が log を書き続ける限り待ち、黙ったまま 120 秒(`PELICAN_STUDIO_ENGINE_LOAD_TIMEOUT_MS`)で停止します。Frame Plan の帯の editor も Graph と同じ規則(窓の瞬きで閉じない、再描画で残る、Escape で取消)です。

**engine の窓は Studio の外に出ません(✅EW-2)**: Studio は engine を `--embedded` 付きで起動します([引数の組み立て](../../src/devstudio/viewport/studioplayerarguments.cpp#L92))。engine はこの引数で窓を非表示・focus 無しで作る(EW-1)ので、project の読み込みの間(上の Loading の間)に Studio の外へ窓が出ません。project と一緒の起動・Restart Engine・project の切替のどれでも同じです。

- 窓の探し方: 同じ process で owner の無い top-level 窓のうち、engine が付ける property `PelicanEngineRenderWindow`(`src/core/os/enginewindowproperty.hpp` の `Pelican::engineRenderWindowProperty`)を持つものを選びます。見えているかは見ません([`findProcessWindow()`](../../src/devstudio/viewport/nativewindowhost.cpp#L58))。GLFW の helper 窓には property が付かないので選ばれません。
- 待ち方は Q-4 のままです(窓の thread が WM_NULL に応答してから埋め込み、その間は Studio が Loading を描きます)。
- 窓を見せるのは埋め込みの後です。`embed()` は SetParent の前に `WS_VISIBLE` を立てず、SetParent の後の `resize()` の `SWP_SHOWWINDOW` で初めて表示します([表示する所](../../src/devstudio/viewport/nativewindowhost.cpp#L203))。まだ top-level の窓に先に立てると、SetParent までの間だけ desktop に見えるためです(実装者の報告では、戻すと下のシナリオが約 50 ms 見える窓を捕まえて赤になりました)。
- `PELICAN_STUDIO_PLAYER_ARGUMENTS` に `--headless` があるときは `--embedded` を付けません(engine は 2 つ一緒を引数の誤りとして断るため)。付けなかったことを知らせる 1 行はありません。設定に `--embedded` があれば二重には付けません。
- EW-2 の前は、engine が見える窓を読み込みより先に作り、Studio は見えている窓を探していたので、読み込みの間は engine の窓が Studio の外に出ていました。

**Frame Plan のノード編集(N-8)**: Frame Plan でも出力ソケットから編集できる pass / task の入力ソケットへ**通常ドラッグ**で接続でき、入力ソケットを空所へドラッグするか辺を Delete で切断します(shader が入力を宣言している pass では、落とした ○ が binding を決めます。次の段落。✅VI-3a)。どれも既存の `edit_preset_pass` / `edit_compute_task` を宣言丸ごとで 1 回送るだけで、engine の pass や fork 前の preset、feature の pass は吸着せず理由を 1 行で出します。shader stem と task の dispatch は行の帯でその場編集(shader の帯は Pass Editor の Pick... と同じ一覧を部分一致で補完します。打った文字はそのままです。✅T-1。[帯の補完を付ける所](../../src/devstudio/view/frameplangraphics.cpp#L4503))、それ以外は node の double-click で今までの dialog。右クリック「Add fullscreen pass…」は Pass Editor をその node の後ろに prefill して開き、Delete は authored pass(`remove_authored_pass`)と task(`remove_compute_task`)を消します。Shift ドラッグは今までどおり dialog です。node の位置・reroute・tie・group は Graph の `layout` の `frame_plan` に一緒に保存されます。N-8 のときは rendering 文書が legacy 形(root が `pelican.graph` でない)なら保存せず、その旨と file 名を 1 行で出していました。MG-2b から engine は legacy root を読み込みで `render_config.legacy_root@1` と断るので、そういう project を Studio で開いて canvas に届くことはなく、この 1 行も出ません(Studio 側の分岐は残っています)。

**ソケットは shader の宣言から出ます —— 番号でつなぐ(✅VI-3a)**: fullscreen / raster pass の node は、engine が
`get_frame_plan` の `pass_inputs` でその pass の shader の set 1 の宣言を出していれば(§13.5「レンダー設定を編集する」の
「各 pass の set 1 の宣言を `get_frame_plan` で読む」)、入力ソケットをその宣言から描きます
([ソケットを組む所](../../src/devstudio/view/frameplanedit.cpp#L648))。pass の入力は `input` 配列の位置で binding が決まる
(texture の入力は 0 から、buffer の入力は `buffer_binding_base` から)ので、行は名前順ではなく binding の順です。

- **固定の ○**: 固定の宣言 1 つに ○ が 1 つです。行の字は宣言の変数名で、つながっていれば `<変数名> ← <resource>`
  (history の入力なら `<resource> (history)`)。何もつながっていない ○ は輪郭だけで、字は灰色です。tooltip は
  `binding <b>: <行の字> : <型>` です。buffer の宣言も ○ で、buffer だけで数えます。
- **可変の帯**: `PELICAN_DECLARE_VARIADIC_INPUTS` の組は 1 本の長いソケットです。つながっている入力の分だけ ○
  (字は `← <resource>`)が並び、下端に空の落とし先 `+ input` があり、全体を半透明の帯が囲みます
  ([帯を描く所](../../src/devstudio/view/frameplangraphics.cpp#L1601))。空いている枠(max まで)は描きません。Blank pipeline の
  `present` が使う `engine://passthrough` は宣言がすべて可変なので、何もつないでいなければソケットは `+ input` の 1 つだけです。
- **宣言が取らない入力**(断られた編集の後など)は、末尾に以前の形の行で残ります。線の行き先が無くならないようにです。
- **線はつながっている ○ ごとに 1 本**です(同じ resource を 2 つの binding に渡せば 2 本)。

出力ソケットを drag して、**落とした先で**何が起きるかが決まります
([番号でつなぐ要求](../../src/devstudio/view/frameplanedit.cpp#L768))。送るのはどれも `input` 配列を丸ごと持つ `edit_preset_pass` 1 回です。

| 落とした先 | 起きること |
|---|---|
| 空の固定の ○ で、binding が次の番号(今の texture の入力の数。buffer の ○ なら buffer の入力の数) | 末尾に足す |
| 空の固定の ○ で、それより先の番号 | 送らず、status 行に `Connect binding <n>..<b-1> of <pass> first: a pass's inputs fill the bindings in order.`(飛ばしたのが 1 つなら `Connect binding <n> of ...`) |
| つながっている ○ | その位置の入力を置き換える(ほかの入力は動かない) |
| 帯の `+ input` | 固定の宣言が全部つながっていれば末尾に足す。足りなければ上と同じ 1 行。可変の枠が max まで埋まっていれば `<pass>'s shader takes at most <max> inputs in its variadic group.` |
| ノードの本体(見出しなど、ソケットの外) | **足しません**。送らずに `Drop on a socket of <pass>: its inputs follow its shader's declarations.` |

○ は小さい(11px)ので、各行の左 4 割に落としても、その行の ○ に落としたことになります
([落とし先の判定](../../src/devstudio/view/frameplangraphics.cpp#L4341))。image を buffer の ○ に(またはその逆に)落とすと、
`<resource> is an image: binding <b> of <pass> takes a buffer.` のように型を言って断ります。同じ resource が別の binding に
既につながっていても断りません。engine の拒否の扱いは N-8 と同じです。

**外す**: つながっている ○ を空所へ drag するか、線を選んで Delete すると、その `input` の 1 つだけを外します。外した位置より
後ろの入力が 1 つ前に詰まるときは、commit の後の status 行が `Canvas change committed.` の代わりに
`<pass>: the inputs after binding <b> moved up by one.` を言い、次の編集を送るまで残ります
([外す要求](../../src/devstudio/view/frameplanedit.cpp#L856))。空の ○ や `+ input` を押しても切る操作にはならず、node を
押したのと同じです。⚠ 可変の組の無い pass では buffer の binding が texture の入力の数から始まるので、texture を外すと
buffer の binding も 1 つずれますが、この 1 行はそれを言いません。

**順を入れ替えたときの注意**: 宣言の変数名が `in_<X>` の形で、X が同じ pass の**別の** ○ につながっているのに、その ○ には
別の resource がつながっているとき、行の字の後に琥珀色の丸を描きます(tooltip は
`This declaration is named for <X>, but binding <b> is fed <Y>.`)。それ以外の名前は判定しません。

**読み直しで宣言が消えると、その線が赤くなります**: 外のエディタで shader を保存して、pass がつないでいる binding の宣言を
消すと、engine はその pass について読み直しを断り、前の shader で描き続けます(§13.5「shader を読み直させる」の
「pass が新しい shader を断ることがあります」。`pass_inputs.reload_refused`)。canvas は、断られた新しい shader の宣言
(`reload_refused.declarations`)に無い binding につながっている ○ を赤く塗り(字も赤くなり、tooltip に
`The reloaded shader no longer declares this binding.` が付きます)、その線の矢印の先も赤くします。status 行には赤字で
`<pass> keeps drawing with its previous shader: the new one no longer declares binding <b> (<resource>).` が出て、横に
2 つのボタンが並びます([行とボタンを出す所](../../src/devstudio/view/frameplanwidget.cpp#L2545))。

- **Disconnect**(`pelican.framePlan.reloadRefused.disconnect`): その入力を外します(上の「外す」と同じ要求)。pass が
  登録し直されるので断りが解け、pass は新しい shader で描きます。
- **Open the shader**(`pelican.framePlan.reloadRefused.openShader`): fragment の file を desktop で開きます。Q-14 の
  Open the shader と違い、clipboard には何も入れません。fragment が project の file でなければ押せません。宣言を戻して
  保存すれば、その読み直しが通って断りが解けます。

この行とボタンは、読み直しの直後と、その pass を選んだときに出て、断りが残る間は plan を読み直すたびに出し直します。断りが
解けると赤もボタンも消えます。断られている間、ソケットは今描いている前の shader の宣言で描くので、新しい shader が足した宣言は、
断りが解けた後の読み直しで空の ○ として出ます。

**今までどおりの node**: compute / ray tracing の task、pipeline が見つからない pass(`pass_inputs.state` が `"unavailable"`)、
`pass_inputs` の無い pass は、名前順の行のままで、node の本体に落とすと入力の末尾に足します(N-8 / F-7。
[宣言から描くかの判定](../../src/devstudio/view/frameplanedit.cpp#L624))。**入力を 1 つも宣言しない shader**(宣言も可変の組も
無い)の pass も行は名前順のままですが、✅VI-3b から、落としても送りません(下の「宣言の無い shader の pass に落とすと」)。
Shift＋ドラッグで編集画面を開く経路は変わっていません。

**可変の組を固定する —— Fix sockets / Make sockets variadic(✅VI-3b)**: 可変の帯のソケットを、resource の名前の付いた固定の
宣言に書き換えられます。押す所は 3 つで、どれも同じ動作です。

- 可変の帯の端(`+ input`)の横の **Fix** の丸いボタン(`node_socket_fix#<pass>`。[Fix を描く所](../../src/devstudio/view/frameplangraphics.cpp#L1649))
- 可変の組を持つ node の右クリックの **Fix sockets**(`pelican.framePlan.fixSockets`。選んでいる node にも効きます)
- Pass Editor の Fragment 欄の横の **Fix sockets**(`pelican.fullscreenPass.fixSockets`)。開いている pass の shader に可変の組が
  あるときだけ出ます。書き換えるのは Frame Plan と同じで、未保存の下書きではなく、今描いている shader(`pass_inputs.fragment`)です

shader の `PELICAN_DECLARE_VARIADIC_INPUTS(<first>, <max>);` の行が、次の塊に置き換わります
([書き換える所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L587))。

```glsl
// pelican: fixed sockets, was PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);
layout(set = PELICAN_SET_PASS_INPUT, binding = 0) uniform sampler2D in_gbuffer_albedo;
layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform sampler2D in_gbuffer_normal;
vec4 pelican_variadic_texture(int pelican_variadic_index, vec2 pelican_variadic_uv) {
    if (pelican_variadic_index == 0) return texture(in_gbuffer_albedo, pelican_variadic_uv);
    if (pelican_variadic_index == 1) return texture(in_gbuffer_normal, pelican_variadic_uv);
    return vec4(0.0);
}
const int pelican_variadic_input_count = 2;
// pelican: end fixed sockets
```

- 宣言は**今つながっている入力の数だけ**で、binding は `first` から順です。名前は `in_<resource>`(英数字以外は `_`、history の
  入力は `_history` が付く)で、shader の固定の宣言と重なれば `_2`… を付けます。
- 本文が `PELICAN_VARIADIC_TEXTURE` / `PELICAN_VARIADIC_INPUT_COUNT` を使っていても compile が通るよう、そのマクロが呼ぶ関数と
  定数も書きます。
- 1 行目は置き換えた行をそのまま持ちます(字下げの後から、`;` と行末の注釈まで)。行の字下げと改行は置き換えた行に揃えます。
- `PELICAN_DECLARE_INPUT_<i>` ではなく layout の行を直接書くので、**8 個を超えても固定できます**(固定のマクロは 0〜7 まで)。
- 断るとき(status 行に 1 行): 何もつながっていない(`Connect at least one input before fixing the sockets.`)、行が無いか 2 つ
  ある、**buffer を宣言している shader**(`<pass>'s shader declares a buffer after its variadic group: fixing the sockets would
  move its binding. Edit the shader by hand.`。可変の組がある間 buffer の binding は `first + max` から始まり、固定すると texture
  の数の後ろへ動くため)、fragment が `engine://passthrough` 以外の `engine://`(`... an engine shader Studio cannot open or copy.`)。

project の shader はその場で書き換えて保存し、status 行は `Fixed <pass>'s sockets in <fragment>; the shader reloads.` です。
外のエディタで保存したときと同じく読み直され、node のソケットは固定の ○(`in_gbuffer_albedo ← gbuffer_albedo` など)になります。

**Make sockets variadic**(`pelican.framePlan.makeSocketsVariadic`)は、pass の shader の file が Fix sockets の書いた 2 行の
目印を持つときだけ、node の右クリックに出ます。目印の間を、1 行目が持っている元の行に **byte 単位で**戻します
([元に戻す所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L655))。目印の無い shader(手で書いた固定の宣言)は触らず、
`<pass>'s shader has no sockets Studio fixed.` と言います。

**engine://passthrough は project に複製してから直します(✅VI-3b)**: `engine://passthrough` で描く pass で Fix sockets か
Open the shader(Q-14 の出口。下の「shader が読まない入力」)を押すと、Studio は engine の `passthrough.frag` を project の
`shaders/<pass>.frag` に書きます(T-1 の名前の畳み方。file があれば `<pass>_2`、`_3`… で、既存の file は上書き
しません。[複製の名前を決める所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L574))。複製の中身は、Studio が build 時に
engine と**同じ file** から埋め込んだものです(repo に写しは置きません)。

- Frame Plan の Fix sockets は、固定化した source を複製として書き、fragment を `project://shaders/<名前>` に替える
  `edit_preset_pass` を 1 回送ります。`shader.defines` はそのまま残るので、出している入力は変わりません
  ([複製して送る所](../../src/devstudio/view/frameplanwidget.cpp#L2020))。
- Frame Plan の Open the shader は、engine の file を書き換えずに複製して同じ 1 回を送り、複製を desktop で開きます
  ([Frame Plan で複製して開く所](../../src/devstudio/view/frameplanwidget.cpp#L2170))。
- Pass Editor の Open the shader は、複製を書いて Fragment 欄に入れ、開くだけです。送るのは Save です。

複製できるのは `engine://passthrough` だけです。ほかの `engine://` は今までどおり開けません。⚠ 詳細ツリーの shader の Open は、
`engine://passthrough` でも押せないままです。⚠ 複製は送る前に書くので、送信が断られても file は残ります。

**passthrough が出す入力を ○ で選ぶ(✅VI-3b)**: `engine://passthrough` は、pass の `shader.defines` の
`PELICAN_PASSTHROUGH_INPUT`(既定 0)番の入力を出します。その pass の可変の帯で、つながっている ○ を**クリック**(押した所から
4 px 以内で離す。drag ではない)すると、その入力を出すよう `edit_preset_pass` を 1 回送ります
([出す入力を送る要求](../../src/devstudio/view/frameplanedit.cpp#L968))。

- defines の `PELICAN_PASSTHROUGH_INPUT` を外し、k が 0 でなければ `PELICAN_PASSTHROUGH_INPUT=<k>` を足します(k は可変の組の
  中の番号 = binding − `first`)。0 なら define を消し、defines が空になれば `defines` ごと消えます。ほかの define と入力は
  そのままです。commit の後の status 行は `<pass> shows <resource>.` です。
- 今出している ○ は明るく塗られ白い輪で囲まれ、行の字の末尾に ` (shown)` が付きます(tooltip は
  `engine://passthrough shows this input.`)。今の値は宣言の `shader.defines` から読みます。
- 既に出している ○ のクリックは `<pass> already shows <resource>.` と言って送りません。`engine://passthrough` 以外の pass では
  クリックしても何も起きません。
- Pass Editor で、`engine://passthrough` の pass の Fragment を別の shader に替えて Save すると、`PELICAN_PASSTHROUGH_INPUT` を
  defines から落とします(ほかの define は残ります。[Save で落とす所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2471))。
  Fragment を替えない Save では落としません。⚠ canvas の shader の帯で fragment を替えたときは落としません。

**宣言の無い shader の pass に落とすと(✅VI-3b)**: engine が `pass_inputs` を出していて、shader が入力を 1 つも宣言しない
(宣言も可変の組も無い)pass の node に出力を落とすと、Studio は送らずに、status 行に `<pass>'s shader declares no inputs.` と、
下の Q-14 と同じ 2 つのボタンを出します([送らない所](../../src/devstudio/view/frameplanedit.cpp#L776))。
**Use engine://passthrough** は今の入力と落とした入力、`fragment: engine://passthrough` を 1 回で送り、**Open the shader** は今の
fragment を開いて足す行を clipboard に入れます。VI-3b より前は送ってから engine の拒否を受けていました。

自動化面の名前(表の正は [README](../../test/studio_scenarios/README.md)。[宣言のソケットの名前を付ける所](../../src/devstudio/automation/automationcommands.cpp#L267)):

| 名前 | 指すもの |
|---|---|
| `node_socket#<pass>/in@<binding>` | 宣言の ○、または可変の組のつながっている ○(空でもつながっていても) |
| `node_socket#<pass>/in:variadic` | 可変の帯の空の端(`+ input`) |
| `node_socket#<pass>/in:<resource>` | 以前の名前。その resource がつながっている ○ に当たります。`list` には出ません。宣言が取らない入力の行はこれが自分の名前です |
| `node_row_label#<pass>/in@<binding>` | 行の字(`<変数名> ← <resource>`) |
| `node_socket_band#<pass>` | 可変の帯そのもの(描くだけ) |
| `node_socket_warning#<pass>/in@<binding>` | 上の「順を入れ替えたときの注意」の丸 |
| `node_socket_fix#<pass>` | 可変の帯の端の横の Fix のボタン(✅VI-3b。Fix sockets) |
| `pelican.framePlan.fixSockets` | node の右クリックの Fix sockets(✅VI-3b。右クリックした node か選んでいる node に効く) |
| `pelican.framePlan.makeSocketsVariadic` | node の右クリックの Make sockets variadic(✅VI-3b。Fix sockets の目印があるときだけメニューに出る) |
| `pelican.framePlan.shaderContract.usePassthrough` | status 行の Use engine://passthrough(✅VI-3b。以前の `...makePassThrough`) |
| `pelican.fullscreenPass.shaderContract.usePassthrough` / `pelican.fullscreenPass.inputContract.usePassthrough` | Pass Editor の Use engine://passthrough(拒否の後 / Save の前) |
| `pelican.fullscreenPass.fixSockets` | Pass Editor の Fragment 欄の横の Fix sockets(✅VI-3b) |
| `node_view_select#<pass>` | 見出しの下の `view: …` の 1 行(✅LP-4a。下の「view の軸を選ぶ」) |
| `pelican.fullscreenPass.view` / `pelican.computeTasks.view` | Pass Editor / Task Editor の View の欄全体(✅LP-4b。軸が無ければ隠れる) |
| `pelican.fullscreenPass.view.<axis>` / `pelican.computeTasks.view.<axis>` | View の欄の軸の行(✅LP-4b)。選べる軸は combo、実行時に決まる軸は字だけ |
| `pelican.fullscreenPass.view.<axis>.warning` / `pelican.fullscreenPass.view.<axis>.remove`(Task Editor は `pelican.computeTasks.view.<axis>.…`) | 警告の字(family に無い軸、長さの外の値)/ family に無い軸の Remove(✅LP-4b) |
| `pelican.computeTasks.notice` | Task Editor の notice の行(✅LP-4a で名前が付いた) |
| `pelican.view.renderTools.frame_plan` / `.render_features` / `.graph` | View ▸ Render Tools の 3 つのタブを前に出す(✅LP-4c。menu は `pelican.renderToolsMenu`。`action` で発火) |
| `pelican.graphCanvas.addLoop` / `pelican.graphCanvas.addFullscreenPass` | Graph タブの右クリックの Add loop / Add fullscreen pass(✅LP-4c。`action` で発火) |
| `pelican.graphCanvas.renameDefinition` / `pelican.graphCanvas.deleteDefinition` | Graph タブの Rename definition… / Delete definition(✅Q-19。`action` で発火すると、今見ている definition か、選んでいるゾーンの body に効く) |
| `pelican.graphCanvasDefinitionRow` / `pelican.graphCanvasDefinitionCaption` / `pelican.graphCanvasDefinitionName` / `pelican.graphCanvasDefinitionApply` / `pelican.graphCanvasDefinitionCancel` / `pelican.graphCanvasDefinitionLine` | definition の改名・削除の行(✅Q-19): 行全体 / `Rename definition <name> to` などの字 / 新しい名前の欄 / Rename か Delete のボタン / Cancel / 赤い理由の字 |
| `pelican.graphCanvasLoop.<欄>` | Loop の欄(✅LP-4c): `summary`・`openBody`・`count`・`countExpression`・`countExpressionToggle`・`order`・`limitEnabled`・`limit`・`tier`・`place`・`whereKind`・`whereValues`・`output.<port>`・`carry<i>.from` / `.to` / `.remove` / `.warning`・`addCarry`・`addBranch`。draw_loop では `contract`・`whereTagsInclude`・`whereTagsExclude`・`whereTagsRequireAll`(✅LP-4e)。✅LP-4f1 で `over`・`viewFamily`・`viewAxis`・`viewCount`・`viewAxisNote`・`countTarget`・`insertMipLevels`・`insertLayers`・`whereChecks`・`whereValue<i>`(i 番目の要素の check)。✅Q-17 で欄全体を包む `fields` |
| `pelican.graphCanvasLoop.branch<i>.<列>` | 分岐表の i 行目(0 から): `when`・`values`・`body`・`up`・`down`・`remove`・`matched`。draw_loop では `material`・`tags`・`contract`・`skinned`・`hasVariant`(✅LP-4e)。✅LP-4f1 で `value`(`element =` の spin)・`checks`・`check<j>`(`element in` / `not in` の j 番目の要素の check) |
| `pelican.graphCanvasRender`(`.heading` / `.view.<axis>` / `.view.<axis>.warning` / `.view.<axis>.remove`) | Graph タブの render node の View 欄(✅LP-4f1。行の名前は LP-4b の View の欄と同じ形) |
| `pelican.graphCanvasLoopBranch` / `pelican.graphCanvasShared` | body の中のパンくずの右の分岐の combo / `shared (N users)` の印(✅LP-4c) |
| `pelican.graphCanvasPerIteration`(`.source` / `.element`) | Per iteration の dialog(✅LP-4c)。✅LP-4f1 から `.element` は combo で、Custom… の式の欄は `.element.custom` |
| `pelican.graphCanvasPaletteNewDefinition` / `pelican.graphCanvasPaletteContract` | palette の新しい body の名前(✅LP-4c)/ `render.material_pass` の契約(✅LP-4e) |
| `pelican.graphCanvasDraw.<欄>` | material pass と draw step の欄(✅LP-4e): `contract`・`variant`・`cull`・`depthTest`・`depthWrite`・`blend`・`defines`・`applyDefines` |
| `group#__pelican_group__:loop:<id>` / `group_label#__pelican_group__:loop:<id>` | Frame Plan のループのグループとその名前 `loop <id> ×N`(✅LP-4c) |
| `node_draw_loop#<pass>` | material pass の見出しの下の `loop: … (N materials)` の 1 行(✅LP-4e) |
| `pelican.framePlan.decomposeIntoLoop` / `pelican.fullscreenPass.decomposeIntoLoop` | Decompose into loop(Frame Plan の右クリックの項目 / Pass Editor のボタン。✅LP-4e) |
| `pelican.framePlan.decomposeConfirm`(`.ok` / `.cancel`) | Decompose の確認のダイアログと Decompose / Cancel(✅LP-4e) |
| `pelican.framePlan.loopMaterialsMore` | 詳細の draw_loop の material の Read more(✅LP-4e) |
| `pelican.renderFeatures.replaceWithLoop` / `pelican.renderFeatures.replaceWithLoopVersion` | Render Features の Replace with loop version…(一覧の下のボタン / 行の右クリックと同じ action。✅LP-4f2) |
| `pelican.renderFeatures.replaceConfirm`(`.ok` / `.cancel`) | 置き換えの確認のダイアログと Replace / Cancel(✅LP-4f2) |
| `pelican.framePlan.replaceWithLoopVersion` | Frame Plan の feature のグループの右クリックの Replace with loop version…(✅LP-4f2) |

`engine://passthrough` の pass では、`node_socket#<pass>/in@<binding>` への `click` が出す入力を選びます。出している ○ の
`node_row_label#<pass>/in@<binding>` の字は ` (shown)` で終わります。

**view の軸を選ぶ —— view_select(✅LP-4a / LP-4b)**: 影の cascade や cube capture の面のように、1 つの view family が
複数の view を持つとき、pass や task をその一部の view だけで走らせられます(宣言の `view_select: {<軸>: <値>}`。軸と engine の
扱いは [design_graph_loops](../design_graph_loops.md) §7)。Studio は `get_frame_plan` の node の `view_select`、`view_axes`
(軸ごとの名前・長さ・実行時に決まるか・ラベル)、`no_selected_view` を読みます([node で読む所](../../src/devstudio/model/frameplanmodel.cpp#L1199))。

- **canvas の 1 行**: `view_select` のある node(pass も task も)は、見出しの下、値の帯の上に `view: face 2 (+Y)` の 1 行を持ちます
  ([1 行を描く所](../../src/devstudio/view/frameplangraphics.cpp#L1520))。軸が複数なら `, ` でつなぎ、並びは軸の名前の辞書順です。
  値の後ろの括弧は、engine が `view_axes` の `labels` で名前を付けた軸(cube の face)にだけ付きます。cascade は `view: cascade 2` です。
  tooltip は `<pass> runs only for the views of <family> with face 2 (+Y).` です。
- **このフレームで走らなかった node は薄くなります**: engine が `no_selected_view: true` を付けた node は、直近に publish した
  フレームで `view_select` に一致する view が 1 つも無く、何も記録していません。その node は見出しの帯が薄くなり、見出しと node の
  tooltip に `No view of <family> matches view_select this frame.` が出ます([薄くする所](../../src/devstudio/view/frameplangraphics.cpp#L1415))。
- **詳細の行**: node の詳細に `View select`(例 `face 2 (+Y)`)の行が出ます。走らなかった node には `This frame`(上の文)の行も出ます
  ([詳細の行](../../src/devstudio/view/frameplanwidget.cpp#L3646))。Scopes の行は末尾に `; view: face 2` が付きます(ラベルは付きません)。

**view_select の拒否は 1 文で出ます(✅LP-4a)**: engine の `view_select` の拒否 4 種は、field の JSON のままではなく、1 文と末尾の
code で出ます([文を作る所](../../src/devstudio/model/inspectormodel.cpp#L771))。

| code | 文 |
|---|---|
| `graph.view_select_axis_unknown@1`(family に無い軸) | `<pass>: <family> has no view axis named <axis> (graph.view_select_axis_unknown@1).` |
| `graph.view_select_axis_unknown@1`(長さの外の値) | `<pass>: <axis> <value> is outside <family>'s <length> views (0..<length-1>) (graph.view_select_axis_unknown@1).` |
| `graph.view_select_runtime_axis@1` | `<pass>: <axis> of <family> is decided each frame and cannot be selected (graph.view_select_runtime_axis@1).` |
| `graph.view_select_primary_family@1` | `<pass> draws the main view (<family>), which has no axes to select (graph.view_select_primary_family@1).` |
| `graph.view_select_needs_target_plan@1` | `<pass>: view_select needs the physical target plan, which this configuration does not build (graph.view_select_needs_target_plan@1).` |

- 別の拒否(`render_pipeline_preflight_failed` など)の message の中に `<code>: {…}` の形で引用されて届いても、同じ文になります。
- graph variant が断ったときは、前に `The <variant> graph refused it: ` が付きます。
- 出る所は、Frame Plan の status 行、Pass Editor の拒否の行、Task Editor の notice(`pelican.computeTasks.notice`)です。
  Frame Plan と Pass Editor の行は 90 字で切れ、全文は tooltip にあります。
- Graph タブの拒否の読み方も同じ文を返します。

**view family の大きさ(✅LP-4f3)**: Frame Plan で view family(cube の面、影の cascade など)の node を選ぶと、詳細に `Family size: 256×256 ×6 layers (from cube_capture_geometry)` の 1 行が出ます(画面相対なら `screen ×1.0`)。中身は engine の `get_frame_plan` の `family_extent` / `family_layers` / `family_extent_pass`(✅VF-2)で、下の拒否の文と同じ書き方です。$main の node には出ません。

**view family の大きさの拒否(✅LP-4f2)**: engine の `render.view_family_extent_mismatch@1`(✅VF-1。1 つの view family の raster pass が違う大きさの target に書いた。上の「変種と組み直しの拒否の形」)も 1 文で出ます([`viewFamilyExtentRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L730))。上の view_select の文と同じ関数の先頭で読むので、出る所も同じです(Frame Plan の status 行、Pass Editor、Task Editor の notice、Graph タブの notice)。

```text
loop/0/face_pass (loop loop, body node face_pass) writes loop/0/face_pass__color at screen ×1.0, but $capture/cube draws at 256×256 (its first pass cube_capture_geometry writes cube_capture_albedo): the passes of one view family draw at one size (render.view_family_extent_mismatch@1).
```

- 大きさは、固定なら `256×256`、画面相対なら `screen ×1.0` です(倍率は小数 1 桁。要るときはそれ以上。`screen ×0.25`)。
- ループが複写した pass には `(loop <loop>, body node <node>)` が付きます(`loop_origin` / `expected_loop_origin`)。
- fields は、code の横、`error_fields`、別の拒否の message の中の `<code>: {…}` のどこからでも読みます。
- **赤い印**: 拒否が `loop_origin` を持つと、Graph タブはループと、その要素の分岐の body の中のその node を赤くします(上の「ループと definition の拒否」の赤い印と同じ規則。[`GraphCanvasModel::markRefusal()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3633))。⚠ 印が付くのは、fields が code の横か `error_fields` で届いたときだけです(message の中だけにあるときは、文は出ても印は付きません。LP-4f2 の着地時の記録)。
- この拒否を避けるため、view の軸で回すループの body の Add fullscreen pass は、image をその family の大きさで作ります(上の「ループを置く」の Add fullscreen pass)。

**View の欄 —— Pass Editor と Task Editor(✅LP-4b)**: engine が node の `view_axes` に軸を出している pass / task を開くと、
**View** の欄が出ます。Pass Editor では Name の下、Task Editor では dispatch のフォームの下で、軸ごとに 1 行です
([欄の行を作る所](../../src/devstudio/view/viewselectfield.cpp#L51))。

- **選べる軸**: 軸名と combo です。先頭の **All views** は「選ばない」(その軸の view を全部走る)で、続けて `0`〜`length-1` の値が
  並びます。engine がラベルを出す軸は `2 (+Y)` の形です。開いたときは今の `view_select` の値が選ばれています。
- **実行時に決まる軸**(directional の影の `light` など): `light: every view, decided each frame` と灰色の字で出て、選べません。
- **family に無い軸**: `view_select` に書いてあるのに family に無い軸は、`<axis> <value>` の字と **Remove** のボタンで出ます。
  - その下に琥珀色で `<family> has no view axis named <axis>: the engine will refuse it. Remove it.` が出ます。
  - 値は Remove を押すまでそのまま残ります。
  - この警告は、family に軸が 1 つ以上あるときだけ出ます(軸が無い family では欄ごと出ません)。
- **長さの外の値**(cascade が 3 つなのに `cascade 5`): combo にその数字の項目が 1 つ足されて選ばれ、
  `cascade 5 is outside its 3 views (0..2): the engine will refuse it.` が出ます。
- **欄が出ないとき**: family に軸が無いとき(`$main`、planar の反射、作者が宣言した family)、engine が `view_axes` を出さないとき、
  新しい pass / task の下書き、frame plan にその node が無いとき。
- Task Editor の軸は、Frame Plan が今持っている frame plan から引きます。
- Pass Editor では、欄が出ている間、`omitted: …` の行(`pelican.fullscreenPass.omittedKeys`。宣言にあって下書きに無い key)に
  `view_select` を数えません。

**canvas の View submenu(✅LP-4b)**: node の上で右クリックすると(見出しでも行でも、ノードの上ならどこでも)、軸のある node には
**View** の submenu が出ます([submenu を埋める所](../../src/devstudio/view/frameplangraphics.cpp#L4033))。

- 軸ごとに軸名の見出しがあり、その下に All views と値が排他の check 項目で並びます。今の値に印が付いています。
- 実行時に決まる軸は、押せない 1 行です。
- 選ぶと、その場で 1 回の編集を送ります([pass の要求](../../src/devstudio/view/frameplanedit.cpp#L1978))。
  - pass は `edit_preset_pass` です。fullscreen pass では、engine が必須にしている `input` / `output` / `shader` も宣言どおり一緒に送ります。
  - task は `edit_compute_task` で、宣言ごと送ります。
- 今と同じ値を選ぶと送らずに、`<name> already runs for face 2.` のように言います(All views なら `face All views`)。
- submenu の objectName は `pelican.framePlan.viewMenu` です。シナリオではまだ通していません。

**保存の規則(✅LP-4a / LP-4b)**:

- **Pass Editor の Save は、View の欄を変えたときだけ `view_select` を送ります**
  ([送るかを決める所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2420))。
  - `edit_preset_pass` は、渡されなかった member を保ちます。欄を触らない Save(shader や入力だけを変えた Save)では、今の値がそのまま残ります。
  - どの軸も All views に戻すと、`view_select: null` を送ります。engine は member を消します。canvas の View も同じです。
- **task は宣言ごと送ります。**
  - Task Editor の欄で選ぶと、JSON 草稿の `view_select` だけが書き換わります。notice は `View updated in the JSON draft. Apply task to save.` です。
  - 送るのは **Apply task** です。
  - どの軸も All views なら、草稿から `view_select` の key を消します(`null` は書きません)。canvas の View も同じです。
  - 草稿の JSON を手で直すと、欄がそれに合わせて読み直されます。
- canvas の接続・切断などの編集(N-8)も `view_select` を落としません。pass には送らず、task は宣言ごと送るためです。

**分かっている穴**(着地時の記録):

- dialog を開いている間に frame plan が変わっても、欄は追いません。
- Task Editor の欄と canvas の submenu は、GPU シナリオでは通していません(CPU の test だけ)。
- Graph タブの render node の parameters に書く `view_select` の欄は、LP-4b の着地時はありませんでした。✅LP-4f1 で入りました(上の「body の render node の View 欄 —— Each element」)。

**direct root の pass を Studio で編集する(✅F-6)**: Blank pipeline の project のように root が自分で pass を
宣言し preset を選ばない(`config_kind` が `direct`)とき、engine が `editable_root_pass` として出す pass(例: `present`)は、
Pass Editor でも canvas(drag での接続・切断、帯の編集)でも preset の pass と同じように編集でき、保存は
`base_source_digest` だけを付けた `edit_preset_pass` で root の文書に書きます。Pass Editor の Save の tooltip は
「Calls edit_preset_pass on the pass `<root の参照>` declares.」です。material routing の欄は preset の pass にだけ出ます。
fork は要りません(fork する preset がありません)。

**編集できないと言うのは、engine が編集を申し出ない pass だけになりました。**root が宣言していても `editable_root_pass` が
偽の pass(編集できる種類でないもの)を開くと、Pass Editor は保存できない draft を読み込まず、notice 行に
「`<pass>`: not editable in a direct root」、tooltip に root の参照と「..., which selects no preset, and the engine offers no
editable declaration of it. Edit that file by hand.」を出します
([direct root の Pass Editor の拒否](../../src/devstudio/view/fullscreenpasswidget.cpp#L2545))。canvas も同じ文で断り、
engine が足した pass には「is added by the engine; it has no declaration in the project to edit.」と言います。どちらも
fork は勧めません([direct root の canvas の拒否](../../src/devstudio/view/frameplanedit.cpp#L131))。Pass Editor は pass を
選ぶと最初に form を空にするので、断られた pass の下に前の pass の下書きが残ることはありません(✅F-7)。

**Frame Plan で render target を宣言する(✅F-1)**: toolbar の **New render target...**(`pelican.framePlanNewRenderTarget`)か、
canvas の空所の右クリックの同名 action(`pelican.framePlan.newRenderTarget`)で form(`pelican.renderTargetForm`)が開きます
([form](../../src/devstudio/view/rendertargetform.cpp#L36))。欄は名前・種類(Scene color / Display / Data / Explicit format と
format)・大きさ(Screen ratio = `extent_scale`、または Fixed size = `width` × `height`)・usage(engine が返した語彙の
checkbox)・history・clear color(`r, g, b, a`、任意)です。**名前だけ入れて OK** すれば、画面と同じ大きさの scene color
(`R16G16B16A16_SFLOAT`、`COLOR_ATTACHMENT` + `SAMPLED`)になります。Fixed size でも `extent_scale: 1` は書かれます
(parser が必須にしているため。[宣言の組み立て](../../src/devstudio/view/frameplanedit.cpp#L1384))。送るのは `edit_render_targets` 1 回です。
canvas は **Target**(`pelican.framePlanTarget`)で選んだ target を resource node として描き(自動化面の合成名は
`resource_node#<名前>`)、作った target は次の描画でそこに選ばれます(Target が **Whole frame** のときは選び直さず、
Whole frame のまま新しい target を描きます。下の段落)。その node の **double-click** で同じ form が
replace として開き、click で選んで **Delete** で remove します。replace は fragment の宣言を土台にするので、form に無い
欄(`mip_levels` など)は残ります。**他所で宣言された target は form が読み取り専用で開き**、出所を 1 行で出します
(「the screen output the engine provides」「a target of preset hybrid_v1」など)。Delete しても RPC は出ず、同じ出所の
文で断ります。engine の拒否は code ごとの 1 文になり、`render_target_referenced` なら使っている pass を並べます
([拒否の文](../../src/devstudio/view/frameplanedit.cpp#L1465))。

**Whole frame —— まだ誰も読まない target をつなぐ(✅F-7)**: Target combo の先頭は **Whole frame** です。まだ表示して
いない graph(project を開いた直後を含む)はこれで開き、refresh の後は選んでいたものが残ります。選択は Studio を
閉じると忘れます。Whole frame は**全 pass** と、**どの pass も書かない宣言済み target**(宣言しただけでまだ使っていない
`scene_color` など)を 1 枚に描きます([`populateWholeFrame()`](../../src/devstudio/view/frameplangraphics.cpp#L2451))。
後者は F-1 の resource node として、それを最初に読む pass の 1 列左(読む pass が無ければ描画全体の左)に置かれ、
右端に出力ソケット(自動化面の合成名 `node_socket#<名前>/out:<名前>`)を持ちます
([置き方](../../src/devstudio/view/frameplangraphics.cpp#L3043))。**このソケットから pass の入力ソケットへドラッグすると、
N-8 の接続**(`edit_preset_pass` 1 回。direct root 自身の pass もこれで root に書かれます)になります。pass が書いている
target は、書く pass の出力ソケットとして既に描かれているので、別の node にはなりません。Target で 1 つの target を
選ぶと、今までどおりそこへ届く上流だけ(Depth の深さまで)を描きます。status 行の target の欄は「whole frame」と出ます。

**Frame Plan で material pass を足す(✅G-1)**: toolbar の **Add material pass...**(`pelican.framePlanAddMaterialPass`)か、
canvas の右クリック(node の上でも空所でも)の同名 action(`pelican.framePlan.addMaterialPass`)で form
(`pelican.materialPassForm`)が開きます([material pass の form](../../src/devstudio/view/materialpassform.cpp#L24))。
pass は右クリックした / 選んだ node の後に入り、無ければ graph の先頭です(form の最初の行がどちらかを言います)。

- **Contract**: Forward opaque / Forward transparent / Deferred geometry (G-buffer) / Legacy G-buffer(Forward opaque で
  開きます)。選んだ contract が出力の枠を決めます([contract ごとの枠の表](../../src/devstudio/view/materialpassedit.cpp#L61)):
  forward は scene color 1 枠、G-buffer は albedo / normal / material / world position / emissive の 5 枠で、どちらにも
  depth 1 枠が付きます。枠の見出しには contract が要求する format が出ます。
- **各枠**は宣言済みの target を選ぶか、新しい名前を打つ combo です。横に `empty` / `declared` /
  `new: made as <format>` が出ます。
- **枠は開いた時点で埋まっています(✅T-1)。**form を開いたときと contract を変えたときに、全部の枠に名前が入ります
  ([開いた時点の枠を決める所](../../src/devstudio/view/materialpassedit.cpp#L141))。役割の既定名(下の `scene_color` など)の target が
  既に宣言されていて、format が枠の要求と合う(`format` が無ければ `format_class` が合う)ならそれを使い、それ以外の枠は
  Create missing targets と同じ新しい名前です。そのまま Save でき、どの枠も選び直す・消すことができます。T-1 より前は枠が
  空で開き、Create missing targets を押していました。
- **Create missing targets** は空いた枠だけを既定の名前で埋めます: `scene_color`、`gbuffer_albedo` / `gbuffer_normal` /
  `gbuffer_material` / `gbuffer_worldpos` / `gbuffer_emissive`、`scene_depth`(宣言済みか他の枠が使っていれば `_2`…)。
  作る format は contract の表どおりで、forward の color は `explicit(R16G16B16A16_SFLOAT)`、G-buffer の albedo と
  emissive は `format_class: scene`(HDR で `R16G16B16A16_SFLOAT`、SDR で `B8G8R8A8_SRGB`。hybrid_v1 と同じ宣言)、
  normal と world position は `R16G16B16A16_SFLOAT`、material は `R8G8B8A8_UNORM`、depth は `D32_SFLOAT` です。
- **Name** は contract から自動で付きます(`forward_opaque`、`deferred_geometry`…。使われていれば `_2`…)。contract を
  変えると付け直しますが、自分で打った名前は変えません。

**Save は 2 つの要求**です。新しい target があれば、まず `edit_render_targets` 1 回(add の batch。
[Save の 1 本目 edit_render_targets](../../src/devstudio/view/materialpassedit.cpp#L206))、その commit の後に届く新しい
context の digest で `add_authored_pass` 1 回([Save の 2 本目 add_authored_pass](../../src/devstudio/view/materialpassedit.cpp#L230))。
新しい target が無ければ後者だけです。**pass が断られても、先に作った target は宣言されたまま残ります** —— form はそれを
Save の前に言い、拒否の status 行も残った target の名前を並べます
([拒否で残る target の文](../../src/devstudio/view/frameplanwidget.cpp#L2329))。いらなければ node から Delete してください。
既存の target を枠に選んだとき、Studio は format を先に検査しません(engine の preflight が上の段落の検査をします)。
入力(`screen_inputs` など)の欄はまだありません。

**forward の既定を同じ Save で入れる(✅G-4)**: Contract が **Forward opaque** で、pipeline に `deferred_geometry_v1` /
`legacy_gbuffer_v1` の material pass が無いとき、form に checkbox **Draw this project's materials with forward**
(`pelican.materialPassForm.forwardDefault`、既定 on)が出ます([forward の既定の checkbox](../../src/devstudio/view/materialpassform.cpp#L64))。
automatic な material(glTF の material、基本形の灰色、engine の組込)の route は既定では `deferred_geometry` なので、
forward の pass だけの pipeline には受け皿が無く、engine は上の `material_route_unserved@1` で断ります。on のまま Save すると、
`add_authored_pass` が pass と `material_route_default: "forward"` を 1 回で運び
([payload に既定を載せる所](../../src/devstudio/view/materialpassedit.cpp#L259))、form の notice にも「pass と設定は一緒に
保存され、engine が再起動する」旨が足されます。

commit の結果に `restart_required` があると、Frame Plan の status 行に
`Restarting the engine to apply the material route setting...` を出し、**同じ project で engine を止めて起動し直します**
([`EmbeddedViewport::restartEngine()`](../../src/devstudio/viewport/embeddedviewport.cpp#L542))。gate はすぐ閉じ、新しい engine が
readiness に答えると Frame Plan が戻ります。

**保存していない scene の編集があれば、再起動の前に問います(✅G-5)**: project の切替と同じ Save / Discard / Cancel が
「Save your scene changes before the engine restarts?」の文で出ます
([`MainWindow::requestEngineRestart()`](../../src/devstudio/view/mainwindow.cpp#L1420))。未保存の編集が無ければ今までどおり
すぐ再起動します。

- **Save** は保存が終わってから再起動します。保存を始められなければ再起動しません。
- **Discard** はそのまま再起動します。新しい engine はディスクの scene を読むので、保存していない編集は消えます。
- **Cancel** は再起動せず、**今の engine が走り続けます**。設定はもう保存されているので、status bar と Frame Plan の status 行に
  `The engine was not restarted. The setting is saved and applies the next time the engine starts.` が出て、Frame Plan は
  今の engine から plan を読み直します。
- 別の問い(project の切替・窓を閉じる)が出ている最中に再起動が求められたときは、問わずに同じ文で再起動を見送ります。
- 自動化面の名前は、問いが `pelican.unsavedScenePrompt`、ボタンが `pelican.unsavedScenePrompt.save` / `.discard` /
  `.cancel` です(project の切替・窓を閉じるときの問いも同じ名前です)。

G-4 の時点では、保存していない scene の編集は確認なしにこの再起動で失われていました。

`material_route_unserved@1` で断られると、status 行は
`The engine refused the pass: No pass draws the <route> route, which <名前> take(s). To fix: <hint>.` の形で、material の名前
(組込には `(engine built-in)`)と直し方を言います([`materialRouteUnservedNotice()`](../../src/devstudio/view/materialpassedit.cpp#L281))。
G-1 のシナリオの fixture が持っていた、誰も読まない pass 2 つは G-4 で外しました
([fixture の README の G-4 の段落](../../test/fixtures/studio_material_pass_authoring/README.md#L21))。

**Pass Editor の form は scroll します(✅F-7)**: form は `QScrollArea`(`pelican.fullscreenPassScroll`)の中にあります。
以前は form の最小の高さ(1,000 px 近く)が下の dock 領域を押し上げ、Pass Editor を開くと viewport が 60〜80 px の帯に
潰れ、dock を閉じても戻りませんでした([置き場](../../src/devstudio/view/mainwindow.cpp#L382))。**その潰れた状態を
既に保存した layout は、そのままでは戻りません** —— splitter を引くか、**Layout > Reset to Default** で直してください。

**fullscreen pass を shader ごと作る(✅F-3)**: Pass Editor の Fragment 欄の横に **New…**
(`pelican.fullscreenPass.newFragmentShader`。project を開いていないと押せません)があります。押すと名前の行
(`pelican.fullscreenPass.shaderTemplateName`、既定は pass 名)が出て、**Create** で `<project>/shaders/<名前>.frag` を
雛形([resource](../../src/devstudio/resources/fullscreen_pass.frag.in))から書き、欄に `project://shaders/<名前>` を入れ、
desktop の既定のエディタで開きます。雛形は**その時点の入力の数だけ** `PELICAN_DECLARE_INPUT_i` と
`PELICAN_TEXTURE_2D_i` の行を持ち、最初の入力をそのまま出します。入力が無ければ UV の gradient です。buffer の入力と
9 個目以降の入力は宣言せず、注釈 1 行で残します。名前は英数字・`_`・`-` だけなので、`shaders` の外には書けません。
**既にファイルがあれば上書きしません** —— 「already exists」と **Open**(`pelican.fullscreenPass.openExistingShader`)が出て、
Open はそのファイルを使って開きます([書き込み](../../src/devstudio/view/fullscreenshadertemplate.cpp#L355))。
名前の行は、次に New pass を押すまで出たままです。

**雛形は 3 種類(✅G-3)**: 名前の行の左の combo(`pelican.fullscreenPass.shaderTemplateKind`)で、New… が書く出発点を選びます。

- **Pass-through** —— 上の F-3 の雛形です。
- **Deferred lighting** —— albedo / normal / material / world position / emissive と、任意の ambient occlusion を**入力の
  位置の順に**読み、`pelican_frame.glsl` の光源と `pelican_lighting_v1.glsl` で照らして scene-linear の色を出す短い shader
  です([Deferred lighting の雛形の lighting include](../../src/devstudio/resources/fullscreen_lighting_pass.frag.in#L9))。clustered
  lighting は light list に resource port が要るので `#undef` しています。選ぶと下の **Uses scene lights** と **Camera position**
  が入り(外しはしません)、入力の欄の下に役割名(`inputs: 0 albedo, 1 normal, ...`)が出ます。**入力が空なら自動で埋まります
  (✅T-1)**: G-1 の既定名 `gbuffer_albedo` / `gbuffer_normal` / `gbuffer_material` / `gbuffer_worldpos` / `gbuffer_emissive` を
  役割の順に入れ、その名前の target が無ければ、役割の語(`albedo`、`worldpos` か `world_position` など)を名前に含む宣言済み
  target が 1 つだけのときそれを入れます([入力を埋める所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L435))。**最初に
  当てはまらない役割で止まります** —— 入力は位置で読まれるので、1 つ飛ばすと後ろの役割がずれるためです。ambient occlusion は
  埋めません。既に入力があれば触りません。埋まらなかった役割は役割名の順に自分で Add してください。足りない役割は shader の中で
  名前付きの定数になるので、compile は通ります。T-1 より前は入力が自動では埋まりませんでした。
- **Tone map** —— scene-linear の色(入力 0)を Reinhard で表示用にします
  ([Tone map の雛形の Reinhard](../../src/devstudio/resources/fullscreen_tonemap_pass.frag.in#L31))。engine の `output_transform` は
  表示の encode だけで tone map しないので、tone map はここで 1 回です。project の `hdr` feature が有効なら `hdr_tonemap` が
  tone map するので、この雛形は素通しになります。

**打つものが減りました(✅G-3)**: Vertex 欄の既定は `engine://fullscreen` です。New… は、pass 名が空(か New… 自身が入れた
名前)なら雛形の名前(`deferred_lighting` / `tone_map`、Pass-through は `fullscreen_pass`。使われていれば `_2`…)を入れ、
shader 名を pass 名から付けます。自分で打った名前は上書きしません([雛形の名前の提案](../../src/devstudio/view/fullscreenpasswidget.cpp#L1445))。
保存した後も form はその名前を持ったままです。

**shader は一覧から選べます(✅T-1)。**Fragment と Vertex の欄の横に **Pick...** の一覧
(`pelican.fullscreenPass.shader.fragmentChoice` / `pelican.fullscreenPass.shader.vertexChoice`)があります
([一覧を欄の横に置く所](../../src/devstudio/view/fullscreenpasswidget.cpp#L1157))。並びは project の `shaders/` の `.frag`(Vertex は
`.vert`)を名前順に `project://shaders/<名前>` で、その後に engine の fullscreen shader(Fragment は `engine://passthrough`(✅VI-3a。
入力をいくつでも取る。[足した所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L409))/ `engine://fullscreen` /
`engine://scene_present` / `engine://output_transform` / `engine://ssao` / `engine://ssao_blur`、Vertex は `engine://fullscreen`)です
([一覧の中身](../../src/devstudio/view/fullscreenshadertemplate.cpp#L383))。選ぶとその stem が欄に入り、一覧は「Pick...」に戻ります。
欄には今までどおり何でも打て、欄の名前(`pelican.fullscreenPass.shader.fragment` など)も変わりません。一覧は New… で shader を
作った後と plan を読み直したときに読み直します。Frame Plan の shader の帯も同じ一覧で補完します(下の N-8 の段落)。

**出力の一覧から新しい target を作れます(✅T-1)。**color 出力の一覧の最後は `New target: <pass>_color...` です(pass 名の英数字
以外は `_`、pass 名が空なら `fullscreen_pass_color`、使われていれば `_2`…。[名前の提案](../../src/devstudio/view/fullscreenshadertemplate.cpp#L420))。
選ぶと Frame Plan の New render target の form(F-1)がその名前入りで開き、OK で target が commit されると、その target が
この pass の color 出力に足されます。plan の読み直しで変わったのがその target が増えたことだけなら、Pass Editor の下書きは
捨てません([下書きを残す判定](../../src/devstudio/view/fullscreenpasswidget.cpp#L3211))。

**既にある shader を読む pass の New…(✅G-5)**: pass が既に `shaders/<pass 名>.frag` を読んでいるとき(New… で作った
`shaders/present.frag` を読む `present` で、もう一度 New… を押したときなど)、名前の提案は `<pass 名>_<雛形>`(`present_tone_map` / `present_deferred_lighting`)で、
そのファイルもあれば `_2`、`_3`… を足します。Pass-through なら `<pass 名>_2` からです
([既存の file を避ける shader 名の提案](../../src/devstudio/view/fullscreenpasswidget.cpp#L1209))。以前は今読んでいるファイルの名前を
提案したので、Create が「already exists」になっていました。今のファイルは上書きされずに残ります。

**pass の設定欄(✅G-3)**: fullscreen pass の form に 2 つの欄が増えました(add / edit の両方)。

- **Frame data** の行(`pelican.fullscreenPass.interface`): **Uses scene lights**(`uses_light_data`)と push constants の
  combo(No push constants / Camera position / Projection and view = `push_constants` の `camera_position` /
  `projection_view`)です([Frame data の行](../../src/devstudio/view/fullscreenpasssettings.cpp#L119))。どちらも pass の
  contract と fingerprint に載る宣言で、GPU への供給は変わりません —— light の UBO(set 0 binding 2)はこの欄に関係なく
  すべての pass に bind され、camera の位置は push されず、雛形は `pelicanFrame.camera_position` を読みます
  ([第6章](06_rendering.md) §6.2 の fullscreen の項)。
- **color 出力ごとの行**(`pelican.fullscreenPass.outputOps`、color 出力の欄の下): Load(Default / Load / Clear /
  Don't care)・clear colour(`r, g, b[, a]`)・Store(Default / Store / Don't care)
  ([Load / Clear / Store の行](../../src/devstudio/view/fullscreenpasssettings.cpp#L267))。列の見出しはありません(左から
  出力名・Load・clear colour・Store)。**`load_op` / `store_op` の正は今までの `output.color` の JSON 欄**で、この行は JSON 欄を
  映し、変えると JSON 欄へ書き戻します(`subresource` などほかの field は残ります)。**clear colour は attachment object には
  書けない**(attachment object が受けるのは `target` / `subresource` / `load_op` / `store_op` だけ)ので、pass の
  **`clear_colors`**(出力名が鍵)に入ります。swapchain に書く出力の `clear_colors` の鍵が、swapchain → display の正規化の
  下でどうなるかは未確認です。
  clear colour の欄の横には **Colour...**(`pelican.fullscreenPass.outputClearColorPick.<番号>`。✅T-1)があり、OS の色選択
  (modal の `QColorDialog`、alpha つき)で選んだ色を欄に `r, g, b, a` で書きます
  ([色を選んで欄に書く所](../../src/devstudio/view/fullscreenpasssettings.cpp#L253))。色選択は自動化面から操作できないので、欄への
  直接入力も残っています。

保存は今までどおり `add_authored_pass` / `edit_preset_pass` で、edit は宣言から変わった field だけを送ります(変えずに
保存すれば送らず、外すと `null` で消します)。

保存が **shader の compile 失敗**で断られる(`render_pipeline_preflight_failed` の message に "Shader compile failed" が
ある)と、form の下の折りたためる欄(`pelican.fullscreenPass.compileLog`)に log 全文が等幅で出ます。行番号が付き、
log が名指す `file:line` は強調されます。notice 行は「shader compile failed; see the log below」だけで、**form に打った
値はそのまま残る**ので、shader を直してもう一度 Save できます。成功すると欄は消えます
([経路](../../src/devstudio/view/fullscreenpasswidget.cpp#L1532))。canvas の帯で直接 stem を変えて compile で断られた
ときは、今も status の 1 行だけです。

**shader が読まない入力を足すと、何をすればよいかを 1 行で言い、その場で直せます(✅Q-14)。**例えば `present` が何も読まない project の shader(入力を宣言せず gradient を書くだけの `shaders/gradient.frag` など)で描いているとき(Blank pipeline の `present` が使う `engine://passthrough` は入力がすべて可変なので、入力を足しても断られません。✅VI-2)、そこへ Pass Editor の Inputs で `gbuffer_albedo` を Add して Save すると、engine は pass の入力と shader の宣言が合わないとして断ります(上の「レンダー設定を編集する」の `shader_input_mismatch`。✅Q-14e)。正しい拒否ですが、以前は engine の文が出るだけで、次に何をすればよいかを言いませんでした。canvas で `present` の node に落としたときは、✅VI-3b から Studio が送る前に止め、`present's shader declares no inputs.` と下の同じ 2 つのボタンを出します(上の「宣言の無い shader の pass に落とすと」。VI-3b より前は送ってから、この拒否を受けていました)。

- **読める 1 行**: `<pass>'s shader (<fragment の stem>) reads only N inputs. To change its inputs, change the shader too.`(N が 0 なら `reads no inputs.`、1 なら `reads only 1 input.`、数が分からなければ `does not read the inputs the pass lists.`)([読める 1 行を作る所](../../src/devstudio/view/shaderinputcontract.cpp#L136))。Frame Plan では status 行(tooltip は engine の全文)、Pass Editor では compile log の欄の下(`pelican.fullscreenPass.shaderContract`)と notice 行に出ます。
- **当たる拒否**: `render_pipeline_preflight_failed` のうち、`error` に `shader_input_mismatch` があるか、message に `Fullscreen pass input` と `does not match shader reflection` の両方があるものだけです([入力の契約の拒否を読む所](../../src/devstudio/view/shaderinputcontract.cpp#L152))。ほかの拒否の出し方は変わりません。Frame Plan で出すのは、送った要求が `input` と `shader.fragment` を持つ `edit_preset_pass` のときだけです。
- **N の出所**: `shader_input_mismatch.shader_pass_inputs` の数 → 無ければ message の `(shader declares N pass inputs)` → 無ければ `get_frame_plan` がその pass に出した `pass_inputs` から数えた数(固定の宣言の数 + 可変の組の max。宣言がその fragment のもの —— 末尾の `.frag` の違いは見ない —— のときだけ。✅VI-3b。[宣言から数える所](../../src/devstudio/view/shaderinputcontract.cpp#L227))→ どれも無ければ数を言わず、`does not read the inputs the pass lists.` の形になります(`pass_inputs` の無い新しい pass など)。VI-3b より前の 3 番目は、fragment の file の宣言行を数えていました。pass と fragment も、engine が `shader_input_mismatch` で名指せばそちらを使います。
- **Use engine://passthrough**(✅VI-3b。`pelican.framePlan.shaderContract.usePassthrough` / `pelican.fullscreenPass.shaderContract.usePassthrough`): pass の fragment を `engine://passthrough` にします。入力がすべて可変(max 8)の engine の shader なので、今の入力(足そうとした入力を含む)をそのまま読めます。Frame Plan は入力の一覧と `shader.fragment` を **1 回の `edit_preset_pass`** で送ります。どちらか片方だけの変更は engine が断るためです([入力と shader を 1 回で送る要求](../../src/devstudio/view/frameplanedit.cpp#L425))。Pass Editor は Fragment 欄に入れて、拒否の後ならそのまま保存します。**file は書きません**。`PELICAN_PASSTHROUGH_INPUT` の define が無ければ出すのは入力 0 で、別の入力を出すには上の「passthrough が出す入力を ○ で選ぶ」です。VI-3b より前のこのボタンは **Make a pass-through shader**(`...makePassThrough`)で、F-3 の Pass-through の雛形から `shaders/<pass>_pass_through.frag` を書いていました。
- **Open the shader**(`pelican.framePlan.shaderContract.openShader` / `pelican.fullscreenPass.shaderContract.openShader`): fragment の file を desktop で開き、足す行を clipboard に入れます。固定の image の宣言の数(`pass_inputs` から。分からなければ 0)の番号から先の入力について、宣言 `PELICAN_DECLARE_INPUT_<i>(<名前>);` と main の中の `vec4 value<i> = PELICAN_TEXTURE_2D_<i>(<名前>, inUV);` で、名前は雛形が付ける名前と同じです。先頭には「shader に無ければ `#version` の後に足す」`#extension GL_GOOGLE_include_directive : require` と `pelican_sets.glsl` / `pelican_view.glsl` の include が注釈で付きます([clipboard に入れる行](../../src/devstudio/view/fullscreenshadertemplate.cpp#L290))。status は `The declarations are on the clipboard: add them to the shader and save, and it reloads by itself. Then connect the input again.`(Pass Editor は最後が `Then press Save again.`)で、保存すると下の F-5 の reload で反映されます。fragment が `engine://passthrough` なら、先に project へ複製してそれを開きます(✅VI-3b。上の「engine://passthrough は project に複製してから直します」。この経路は clipboard に何も入れません)。ほかの `engine://` など project の外の fragment では押せません。
- **Save の前の注意(Pass Editor)**: 入力の数が、engine がその pass に出した宣言(`get_frame_plan` の `pass_inputs`)の取れる数 —— 固定の宣言の数と可変の組の max の和 —— より多ければ、Save を押す前に入力の欄の下(`pelican.fullscreenPass.inputContract`)に同じ行と 2 つのボタンが出ます([Save の前の検査](../../src/devstudio/view/fullscreenpasswidget.cpp#L935)、[検査の 1 行を決める所](../../src/devstudio/view/shaderinputcontract.cpp#L261))。可変の組を持つ shader は、max までなら入力を足しても「読まない」ことになりません。ここでの Use engine://passthrough は Fragment 欄を埋めるだけで、送るのは Save です。**検査しないとき**: `pass_inputs` の無い pass(まだ保存していない新しい pass など)と、下書きの Fragment が今描いている shader と違うとき(宣言は今の shader のものなので)。どちらも engine の答えに任せます。✅VI-3b より前は、fragment の file の宣言行を正規表現で数えていました(`PELICAN_DECLARE_INPUT_<i>(` と手書きの `set = PELICAN_SET_PASS_INPUT, binding = <i>`)。今は file を読まないので、`#if 0` の中の宣言行を数えたり、可変の組を 0 と数えたりしません。canvas の drag は、shader が入力を宣言している pass では落とした ○ の binding を宣言と照らしてから送り(✅VI-3a)、宣言の無い shader の pass では送りません(✅VI-3b)。それ以外(`pass_inputs` の無い pass)は送ってから拒否を受けます。

⚠ Save の前の検査は型を見ません(buffer の入力も同じ数に入ります)。Open the shader の行は、宣言の番号が 0 から詰まっていると仮定します。

**shader を外のエディタで保存すると画面が変わる —— reload と失敗の印(F-3 / ✅F-5)**: Frame Plan は描いている node の
shader file を監視します。Studio は engine を `--rpc` で起こし、その reload gate は `rpc driver` を理由に閉じている
([gate](../../src/core/watch/reloadgate.cpp#L32))ので、engine は file の変更を自分では拾いません。そこで Studio は、
**書き込みが 200 ms 途切れたら `reload_shaders` を 1 回**送ります(同時に 1 本。返事を待つ間の書き込みは、返事の後に
1 回分だけ送り直します。[debounce](../../src/devstudio/view/shaderreloadfollower.hpp#L82))。定期の timer は無いので、
Q-2 の何もしない間の RPC は増えません。答えごとに:

- **読み直した**: node の印、Frame Plan の status 行の reload の文、Pass Editor の log 欄を消します。viewport は次の
  frame から新しい shader で描きます。✅VI-3a から Frame Plan は plan も読み直します(ソケットは shader の宣言から描くため)。
- **一部の pass が断った**(✅VI-3a。答えは `committed: true` でも `refused_passes` が空でない。engine 側は上の「shader を
  読み直させる」の「pass が新しい shader を断ることがあります」): 成功ではなく失敗として扱います
  ([`refused_passes` を読む所](../../src/devstudio/view/shaderreloadfollower.cpp#L165))。Frame Plan は plan を読み直して、上の
  「ソケットは shader の宣言から出ます」の赤い ○ と 1 行、Disconnect / Open the shader を出し、Pass Editor が開いていれば
  log 欄に最初の pass の `reason` が出ます。reload status の `error_code` が `shader_reload_refused@1` のときは、node に当たらない
  失敗の汎用の 1 行「Shader reload failed: ...」は出しません([出さない所](../../src/devstudio/view/frameplanwidget.cpp#L1492))。
- **compile 失敗**(`shader_reload_failed@1`): engine の log の path が node の shader に当たれば、その node の見出しの
  右上に赤い印(`node_shader_error#<pass>`、tooltip に log の先頭行)を描き、当たらない失敗は status 行に 1 行出します。
  **Pass Editor が開いていれば、form の下の log 欄**(保存の compile 失敗と同じ欄)にも log 全文が出ます。engine は前の
  shader で描き続けます。
- **engine が断った**(`shader_reload_unavailable@1`、replay / strict assets の engine): status 行に「A shader file
  changed; this engine does not reload shaders (<理由>).」。この文はこのときだけ出ます。
- それ以外の失敗: 「Could not ask the engine to reload shaders: ...」。

読み直しか失敗の後には `get_status` の `reload.runtime["pelican.shaders"]` を 1 回読み、印を engine の記録に合わせます
([答えの扱い](../../src/devstudio/view/frameplanwidget.cpp#L1505))。

**配線の制御点と置き場所(N-6)**: 辺の double-click で **reroute**(通過点)が入り、drag で動き、Delete で消えます。node を動かしても reroute は動きません。制御点や辺を 2 つ以上選んで右クリック **Tie together** で 1 つの **tie**(結束バンド)になり、辺は等間隔で平行に通ります(**Untie** で戻す、double-click で並びを反転)。**Auto layout** は lane を reroute と tie として出します。node の位置・reroute・tie は文書の scope ごとの `layout`(editor 用、engine は無視)に `set_layout` で残り、次に開いても同じ図です。Frame Plan の位置はまだセッション内です。

**配置の保存は canvas を描き直しません(✅Q-5)**: node を drag した後、配置の変更がまだ送られていない間(300 ms の
debounce の間)は、notice が **「Saving the graph change...」** になります(error の notice だけはこれより優先)
([配置が未送信の間の notice](../../src/devstudio/view/graphcanvaswidget.cpp#L5223))。保存が返ると「Graph change saved.」です。
以前は前の保存の「Graph change saved.」が残ったままだったので、その文を待つシナリオは今の保存を待てませんでした。
また、**配置だけが変わった読み**(前の読みと `layout` 以外が同じ)では、model は node の値を消して読み直さず
([配置だけの読みの判定](../../src/devstudio/model/graphcanvasmodel.cpp#L1184))、canvas が既にその配置を描いていれば
(自分の drag の保存が返ってきたとき)scene を作り直しません —— 選んでいる node・辺・reroute はそのまま残ります
([`GraphCanvasScene::followDocumentPlacement()`](../../src/devstudio/view/graphcanvaswidget.cpp#L764))。Undo や別の書き手が
違う配置を書いた読みは描き直し、ほかの変更が混じった読みは描き直して選択を引き継ぎます。Frame Plan の配置の保存には、
もともと描き直しがありません(読み戻しも選んだ node を保ちます)。
⚠ engine が配置を丸めて返すと、送ったものと一致しないので「Saving…」のまま残ります。編集できる状態にならないままでも
同じです。

**配置の保存の後の編集も通ります —— 文書の版を panel どうしで知らせ合います(✅Q-13)。**配置の保存(`set_layout`)も rendering 文書の commit で、`source_digest` を進めます。以前の Frame Plan は自分の配置の保存の後に context を読み直さず、次の Add material pass... などを古い digest で送って `external_modification: render config base digest no longer matches the engine-owned document` で断られていました(project を開き直すまで直りませんでした)。Graph タブで node を動かした後の Frame Plan の編集も同じでした。

- **共有の digest**: Studio は「engine が今持っている rendering 文書の digest」を 1 つ持ちます([共有の digest の型](../../src/devstudio/view/renderconfigdigest.hpp#L25))。Frame Plan・Pass Editor・Graph タブ・Render Features は、読んだ digest と commit した digest をそこへ知らせ、知らせた panel 以外は自分の context を読み直します([共有の digest を 4 つの panel に渡す所](../../src/devstudio/view/mainwindow.cpp#L575))。読んでいる最中・編集の最中なら、終わってからもう 1 回読みます。Pass Editor は form の下書きを残し、Add fullscreen pass で選んだ挿入位置も変えません。隠れている Frame Plan は、表に出たときに読みます。Render Features から開く Task Editor(`ComputeTaskDialog`)も、開くときに Render Features から共有の digest を受け取り、読んだ root の digest を知らせ、他の panel の commit を知らされると変えた下書きを残して読み直します(✅Q-15。[Task Editor が知らされる所](../../src/devstudio/view/renderfeatureswidget.cpp#L884))。notice は `Another panel changed the document; reading it again. Your edits are kept.` で、残した下書きは読み直した root の digest で送り、task 自身の digest(`base_task_source_digest`)はそのままです。要求の最中に知らされたら、応答の後に読み直します。
- **要求は panel 自身の digest で送ります**: 共有の digest が持つのは「今の版」と通知だけです。要求の `base_source_digest` は、その中身を組んだ context の digest のままです。古い context から組んだ中身を新しい版の名で送ると、他の panel の変更を上書きしうるためです。
- **それでも `external_modification` で断られたら、1 回だけ読み直して組み直します**: Frame Plan は編集(辺の接続・切断、帯、Delete、render target の作成・編集・削除、material pass の 2 段)を「context から要求を組む関数」として持ち、断られると context を読み直して同じ関数で組み直して送ります。断られた byte は送り直しません([読み直して組み直す所](../../src/devstudio/view/frameplanwidget.cpp#L1661))。Pass Editor は form を残して読み直し、Save / Remove を組み直します。Graph タブは配置だけの編集(op が全部 `set_layout`)を同じ op 列で 1 回送り直し、ほかの gesture は今までどおり捨てて読み直します([配置を送り直す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1059))。Render Features は staged の編集を、Task Editor は変えた下書きを残して、1 回送り直します。
- **2 回目も断られたら**: Frame Plan の status 行と Pass Editor の notice に `The document changed outside this panel. Press Reload to read it again.` が出て、横に **Reload**(`pelican.framePlan.reloadAuthoring` / `pelican.fullscreenPass.reloadAuthoring`)が出ます。Reload は context を読み直すだけで、変更はもう一度行います。Render Features と Task Editor は新しいボタンを持たず、`... Press Refresh to read it again.` で既存の **Refresh** を指します。

⚠ 共有の digest に乗っていないものがあります。Inspector のグラフ装着(`GraphAttachmentModel`)、procedural asset の dialog、Render Features から開く小さな dialog 4 つ(frame node の順・preset の編集・callable の初期値・scene の acceleration structure)は、開いたときに読むだけです。Render Features の新しい挙動には専用の test がありません(Q-13 の記録)。Task Editor は、要求が失敗したとき(`inspectorRpcFailed` か、応答を decode できないとき)は最中に知らされた読み直しを拾わず、次の応答か Refresh まで残ります。読み直しは editor に `setPlainText` で入れるので、editor の undo の履歴は消えます(Q-15 の記録)。

**render の編集を戻す —— Undo / Redo(✅RU-1 / RU-1b)**: Frame Plan・Pass Editor・Render Features(と、そこから開く Task Editor などの dialog)でした render の編集は、1 つの履歴に積まれ、Undo / Redo で戻せます([`RenderEditHistory`](../../src/devstudio/model/renderedithistory.hpp#L47))。Graph タブの Undo(上の「Undo(✅LP-4c)」)と、Inspector の scene の Undo は、それぞれ別の履歴です。

- **押す所**:
  - Frame Plan の toolbar の、Refresh の右の **Undo** / **Redo**(`pelican.framePlan.undoButton` / `.redoButton`)。tooltip が次に戻す項目を言います(`Undo Edit pass present`、`Nothing to undo` など)([ボタンを置く所](../../src/devstudio/view/frameplanwidget.cpp#L4477))。
  - **Edit ▸ Undo Render Edit** / **Redo Render Edit**(`pelican.edit.undoRenderEdit` / `.redoRenderEdit`)。
  - **Ctrl+Z**(Undo)、**Ctrl+Y** と **Ctrl+Shift+Z**(Redo)。キーは focus のある panel に従います([キーの範囲](../../src/devstudio/view/mainwindow.cpp#L858))。
    - Frame Plan・Pass Editor・Render Features と、Render Features から開く dialog(Task Editor・Fork・Regions・Records・Scene AS・frame node の順)の中では、この履歴です。
    - Graph タブでは Graph タブの Undo、Inspector では scene の Undo / Redo のボタンと同じです(Inspector のキーは RU-1 で足しました)。
    - focus がテキスト欄にあるときは、その欄の文字の Undo が先に取ります。
- **戻るもの**: render の文書を書く編集です([記録する編集の一覧](../../src/devstudio/model/renderedithistory.cpp#L24))。pass の編集、Decompose into loop、Replace with loop version…(✅LP-4f2。`apply_graph_template`)、task の追加・編集・削除、render target、feature、frame node の順、route の既定、callable の record、scene の acceleration structure、region の置き換え、そして文書を作る・消す編集(Add pass / Remove pass、Fork、Make editable、feature の import)です。
  - 項目の名前は `Edit pass present`、`Decompose forward_opaque`、`Add pass ru1_tint` の形です。Replace with loop version… は、engine の受理の応答の `applied.feature` と `applied.template` から `Replace bloom with Bloom loop` になります(応答の前は `Replace with Bloom loop`。title は雛形のファイル名から engine と同じ規則で作ります。[`RenderEditHistory::graphTemplateLabel()`](../../src/devstudio/model/renderedithistory.cpp#L308))。
  - 何も変えなかった編集と、断られた編集は積みません。新しい編集を積むと、Redo の側は空になります。
  - ✅RU-1b から、作る編集の Undo はできた文書(Add pass が書く `passes/authoring/` の fragment のファイルなど)を消し、消す編集の Undo は編集の前に読んだ中身で書き戻します。Redo はその逆です。
  - 戻すときは、今の中身を読んでから `restore_render_documents` を送ります。送る形と CAS は上の「ループと分解の RPC —— engine の契約」の「render 文書を丸ごと読む・戻す」と「undo の手順」のとおりです。
  - 戻した後は、Frame Plan が plan と context を読み直し、Pass Editor がフォームを文書の今の中身で作り直し、ほかの panel には共有の digest で知らせます([戻した後の読み直し](../../src/devstudio/view/mainwindow.cpp#L899))。route の既定のように engine の再起動が要る編集は、戻すともう一度再起動します。
  - 1 行(`Undid "Edit pass present".` など)が Frame Plan の status 行に出て、status bar にも 8 秒出ます。
- **node を動かした後でも戻せます(✅RU-1b)**: Frame Plan や Graph タブで node を動かすと root の `layout` が書き換わりますが、それより前の編集の Undo は通ります。今の root と「その編集が書いた root」の違いが `layout`(root のものと各 definition のもの)だけなら、戻す中身に後から変わった配置を載せ、今の digest を base にして送ります([配置を載せる所](../../src/devstudio/model/renderedithistory.cpp#L175))。動かした配置は残ります。配置のほかにも違いがあれば、下の「後の編集がある」で断られます。
- **断られるとき**: 後の編集がある(`"<項目>" cannot be undone: <文書> changed since. Undo the later edit first, or refresh.`。履歴は残ります)、予約の場所(`.pelican/`・`assets/` など)に作る・消すことになる、root を消すことになる、のどれかです。後の 2 つと、編集の前に文書を読めなかった編集では、その項目で Undo が止まり、tooltip が `Cannot undo past "<項目>": <理由>` を言います。
- **送信中は待ちます**: render の編集が engine の返事を待っている間は `Wait for the render edit in flight.`、Undo / Redo の最中は `Wait for the Undo or Redo in flight.` が出て、何も送りません。
- 別の project を開くと履歴は消えます。engine が切れたときは、送信中のものだけを捨てて履歴は残します。
- ⚠ 分かっている穴(RU-1 / RU-1b の着地時の記録): preset だけを戻したとき(root が変わらないとき)は、Render Features と Graph タブが自分からは読み直しません。`Undid …` の行は、続く Frame Plan の読み直しの status で上書きされることがあります(status bar には残ります)。Render Features と dialog の中のキーは test していません。

**パネルの文字(✅N-7)**: 各パネルの既定状態に出るのは見出し・ラベル・値・1 行の状態だけです。仕組みの説明は control の tooltip か、パネル右上の `?`(この章の該当節を開く)にあります。検査の一覧は記号(✓ / ✗ / –)で、文は hover。notice は言うことが無いときは消えます。文字の大きさとフォントはテーマ表(`studio_themes.json` の `font`)から取り、`devstudio_paneltext_test` がパネルごとの可視文字の上限を固定します。

**このタブが出せる RPC は 11 本だけです。**`GraphCanvasReadCapability`
([定義](../../src/devstudio/view/graphcanvasreadcapability.hpp#L31))は「汎用の RPC メソッド」を表現できない形をしていて、
次の 10 個だけを名前で持ちます(LP-4f2 で 11 個になり、LP-4f3 で `get_render_authoring_context` を外して 10 個に戻りました)。

- 読み: `get_graph_instances` / `get_graph_catalog` / `get_graph_values` / `get_graph_node_values` / `get_status` / `get_frame_plan`
  (`get_graph_node_values` と `get_status` は AG-3b の観測。definition の中の値と、Timeline の軸になる scene time。
  `get_frame_plan` は ✅LP-4c で入り、`loops[]` を読みます。文書にループがあるときだけ問い、✅LP-4e から draw_loop の
  material の続きのページ `{loop_elements: {node, offset, limit}}` も同じ名前で問います。✅LP-4f1 から、root に render node が
  あるときも問い、node の `view_axes` と render target の名前も読みます。
  ✅LP-4f3 から、二次 family の node の `family_extent` / `family_layers` も読みます)
- 文書の編集: `edit_graph_document` と、その結果を待つ `get_edit_result`
- Timeline の scrub: `set_time` と `render_frame`(1 つの「scrub」にまとめず、2 本の RPC として名前で持ちます)

それ以外を要求すると `The graph canvas cannot issue <method>.` になります
([分配](../../src/devstudio/view/graphcanvaswidget.cpp#L4333))。キャンバスから preset を fork したり pass を触ったりは
**できない**、というのが型で保証されています。

### Studio の自動化面(`--automation`)

**これは「GUI の動作確認を AI だけで完結させる」ための面です**(Q-1、利用者指示 2026-09-17)。
人が窓を見て確かめる代わりに、シナリオが実物の窓を実物の engine 付きで動かし、AI が結果の report を読んで
「緑 / 赤と理由」を判定します。**人のレビューは使い勝手に使います。**

**口は 1 つだけです。**`pelican_studio --automation <名前>` で `QLocalServer`(Windows では `\\.\pipe\<名前>`)が開き、
**改行区切りの JSON 1 行 = 1 命令**を受けます([flag](../../src/devstudio/main.cpp#L16) /
[構築](../../src/devstudio/view/uimain.cpp#L41))。

```text
-> {"id": 4, "cmd": "click", "name": "pelican.graphCanvasRefresh"}
<- {"id": 4, "ok": true, "result": {...}}
<- {"id": 4, "ok": false, "error": {"code": "automation.widget_missing@1", "message": "..."}}
```

**flag が無い起動では server object が作られないので、socket が存在しません**
([宣言のコメント](../../src/devstudio/automation/automationserver.hpp#L26)) —— 出荷時の挙動は変わりません。
チャネルは**逐次**で、命令の実行中に次の行が来たら `automation.busy@1` で断ります
([判定](../../src/devstudio/automation/automationserver.cpp#L102))。認証はありません(flag を付けたときだけ開く口です)。

**命令は 20 個**です([一覧](../../src/devstudio/automation/automationcommands.cpp#L2061)):
`list` / `click` / `double_click` / `drag` / `wheel` / `move` / `press` / `release` / `key` / `type` / `text` /
`action` / `menu_list` / `view_state` / `wait` / `wait_engine` / `rpc` /
`open_project` / `screenshot` / `quit`。`rpc` は `EmbeddedViewport` 経由で engine へ送り、応答をそのまま返します。
イベントは `QTest` の合成なので、**窓が前面にあるかどうかに依存しません**。拒否は名前付きで、
`automation.widget_missing@1` / `bad_request@1` / `unknown_command@1` / `engine_not_ready@1` / `rpc_failed@1` /
`wait_timeout@1` / `screenshot_failed@1` / `project_failed@1` / `busy@1` / `command_threw@1` の 10 種です
([定数](../../src/devstudio/automation/automationcommands.cpp#L214))。

**`wait_engine` は「今の」engine を待ちます(✅Q-9)。**Frame Plan の保存が engine の再起動を求めると(G-4)、Studio は再起動の
前に古い engine へ未保存の編集を問います(G-5)。その往復の間も古い engine は editor RPC に答えるので、以前の `wait_engine` は
その engine で通り、次の step が止まった engine に当たって `automation.engine_not_ready@1` で落ちていました。今は
**再起動は頼まれた時点で始まったと数え**、そこから新しい engine の process が最初の `get_status` に答えるまで、`wait_engine` は
通りません([wait_engine の判定](../../src/devstudio/automation/automationcommands.cpp#L1811)、
[再起動待ちを立てる所](../../src/devstudio/view/mainwindow.cpp#L1437))。Cancel などで再起動が見送られれば、すぐ今の engine で
通ります。再起動を待っている間に Studio が送る editor RPC は古い engine に届き(未保存の問いと Save はそこへ届く必要が
あるため)、engine が止まった後は既存の 1 行 `The engine viewport is not ready for editor RPC requests.` で断られます。
Stop ボタンや project の切り替えのような普通の停止は、process が終わるまで ready を落としません(`open_project` はその終わりを
待ちます)。

**指し方は objectName です。**キャンバスの中に描かれたもの(ノード、ソケット、値の帯、その場編集の editor)は
widget ではないので、`<view の objectName>::<種類>#<ノード id>[/<port>]` の合成名で指します ——
例: `pelican.graphCanvasView::graph_value_pill#left/gain`([名前の生成](../../src/devstudio/automation/automationcommands.cpp#L256))。
`list` は widget 名と合成名の両方を返すので、**ソースを読まずに一覧からシナリオを書けます**。
`type` / `key` / `text` は item が抱えている埋め込み widget へ届きます。`select{name, text|index}` は item view の行を本物の click で、combo は index を直接置いて選び、`wait{enabled}` は有効化を待ちます(AG-3b)。✅Q-17 から、`select` は無効な widget(祖先が無効なものを含む)を断り、combo が選んだ項目のままでなければ `"<name>" did not take "<text>"; it holds "<今の値>"` で失敗します(規則の正は [README の Q-17 の段落](../../test/studio_scenarios/README.md#L423))。出力 port の帯は `graph_value_band#<node>/<port>`([経路](../../src/devstudio/automation/automationcommands.cpp#L517))。
list や tree の行そのものは **`<view の objectName>::row#<表示の文字>`**(✅OL-1)で指します。対象は画面に出ている行だけで
(閉じた枝の中の行は見つかりません)、view をその行まで scroll してから行の中央を押すので、`drag` の始点・終点や
`wait{missing}` に使えます(例: `pelican.outliner::row#Floor`。[行の名前を引く所](../../src/devstudio/automation/automationcommands.cpp#L421))。

**✅Q-1b で 7 個増えました** —— `wheel` / `move` / `press` / `release` と `action` / `menu_list` / `view_state` です
(`drag` は `button` を取れるようになりました)。`press` / `move` / `release` は「いまどの button を押しているか」を
面で共有するので、**掴んだままの操作**(中ボタン pan など)がそのまま書けます。ポインタ系はすべて scroll area の
**viewport** に届くので、view の objectName を指した gesture がキャンバスの規則に着きます。`key` は
`route`(既定 `widget` / `window`)を取り、**`window` だけが shortcut map を通る**ので、`QAction` を shortcut で
発火できるのはこちらだけです。`list` の各項目は `shortcut` も返します。メニューバー・dock・gizmo の `QAction` には
objectName が付いていて(`pelican.file.*` / `pelican.view.panels.*` / `pelican.gizmo.*`)、`QAction` は widget では
ないので `list` には出ません —— **一覧は `menu_list`、発火は `action`** です(どちらも popup を開きません)。
`view_state` は `QGraphicsView` の `scale` / `center` などを読むもので、zoom / pan は「どちらへ動いたか」で
検査します(命令表と書式は [README](../../test/studio_scenarios/README.md) が正)。

**シナリオは JSON で、`test/studio_scenarios/<名前>.json` に置きます**(書式は
[README](../../test/studio_scenarios/README.md#L61))。step は「命令をそのまま送るもの」か「期待」で、期待は
`rpc_equals`(engine の応答の `path` が `value`)/ `result`(任意の命令を送り、応答の `path` を記録するか `value` と比べる)/
`text_equals` / `visible` / `list_count` / `screenshot`(保存のみ)の 6 種です([判定](../../tools/studio_drive.py#L886))。`{project}` / `{artifacts}` と `--var NAME=VALUE` は step のすべての文字列に差し込まれます。
`result` の比べ方 `op` は `eq`(既定)/ `ne` / `gt` / `ge` / `lt` / `le` / `contains` / **`excludes`**(✅F-7。文字列がその値を
含まない —— 「その文言は出なかった」の書き方。[比較](../../tools/studio_drive.py#L537))です。`result` も **`retry_ms`**(✅F-5)を受け、
条件を満たすか時間が尽きるまで 0.2 秒おきに問い直します(`rpc_equals` は前から受けていました)。shader を保存した後に
engine が描くものは engine の時計で届くので、`fullscreen_pass_from_template` は `screen` の screenshot をこれで待ちます
([問い直し](../../tools/studio_drive.py#L932))。
✅PM-3a から、`result` は `value_of` に数を足す **`plus`**(`"value_of": "frame", "plus": 1` は 1 歩後)と、覚えた値から比べる
文を作る **`value_format`**(`"Paused · frame {paused_frame}"`)も受けます([`plus` を足す所](../../tools/studio_drive.py#L916)。
書式の正は [README](../../test/studio_scenarios/README.md#L120))。
3 つ目の種類の **`file` step**(✅F-3)は Studio へ送らず、driver 自身が run の持つ directory の中の file を 1 つ読み書きします ——
人が Studio の操作の合間に自分のエディタで file を直す、の代わりです。`write` / `replace`(`old` がちょうど 1 か所のときだけ)/
`contains` / `exists` の 4 つです。書ける範囲は **`{project}`(fixture の複製)と `{artifacts}`(その run の成果物の置き場)の
2 つ**で、File > New Project でシナリオが作った project は `{artifacts}` の下にあるので、そこにも届きます(✅F-4)。
それ以外の path(`..` で外へ出るものを含む)は step の失敗です([file step の範囲の検査](../../tools/studio_drive.py#L770))。
driver の stdout / stderr は、cp932 の console で出せない文字があっても止まりません。
`"probe_no_automation": true` を書くと、**先に flag 無しの Studio を起こして socket が無いことを確かめて**から本番を起動します。
最初の失敗で run は止まります。

**回し方**: [`tools/studio_drive.py`](../../tools/studio_drive.py#L1253)(Python 3.11以上、標準ライブラリだけ)が Studio を
`--automation --project --tab` で起動し、step を順に送り、`report.json` を書きます。ctest からは
`studio_scenario_<名前>` として GPU lane で走ります([登録](../../test/CMakeLists.txt#L3374)) —— **実物の窓が要るので
`QT_QPA_PLATFORM=offscreen` は掛けません**。fixture の project は temp に 2 つ複製されるので、シナリオは
文書を書き換えてよく、「ディスクから読み直す」も試せます([複製](../../test/run_studio_scenario.cmake#L30))。
CTest登録にはStudio targetと`PELICAN_PYTHON_TESTS=ON`（またはPythonが見つかった`AUTO`）が必要です。
`uv`は不要で、完全なGPU gateは全scenarioの登録・実行を必須にします([CI運用](../ci.md))。

**run ごとに空の設定 directory を使います(✅F-5)。**Studio は環境変数 **`PELICAN_STUDIO_CONFIG_DIR`** があれば、利用者の
設定 directory の代わりにそこへ dock の layout を読み書きします([置き場の決定](../../src/devstudio/view/mainwindow.cpp#L103))。
driver は run ごとに `{artifacts}/studio_config` を空にして作り、この変数で渡します([渡し方](../../tools/studio_drive.py#L1302))。
以前のシナリオは、前の run(や人が使った Studio)が利用者の設定 directory に残した dock の layout を復元し、自分の
layout をそこに残していました。テーマの選択はこの directory ではなく `QSettings` に残るので、この変数の影響を受けません。

**Studio を置く画面を選べます(✅OT-1)。**環境変数 **`PELICAN_STUDIO_SCREEN`** に画面の番号(`QGuiApplication::screens()` の順、0 から)か
画面の名前(`QScreen::name()`。`\\.\DISPLAY5` など)を書くと、Studio は起動した窓をその画面の使える範囲(`availableGeometry`)の左上へ動かします
([`placeOnRequestedScreen()`](../../src/devstudio/view/studioscreen.cpp#L30)、[呼ぶ所](../../src/devstudio/view/uimain.cpp#L35))。値は先に番号として読み、
範囲内の番号でなければ名前として探します。保存された窓の位置と大きさを戻した後に動かし、大きさは変えず、枠が画面に入らないときだけ縮めます
(最大化していれば、通常の位置を動かしてから最大化し直します)。どの画面にも当たらなければ stderr に 1 行
`PELICAN_STUDIO_SCREEN names no screen: <値>; screens: <名前, …>` を出し、窓は動かしません。未設定なら何もしません。
ctest の run(`cmake -E env`)と driver(`dict(os.environ)`)は Studio に環境をそのまま渡すので、`PELICAN_STUDIO_SCREEN=1 ctest -L studio_scenario` で
全部のシナリオを 2 枚目の(仮想の)モニタで走らせ、人が使っている画面を空けておけます。OT-1 の着地時に統合側が、100 % の主モニタの右に置いた
125 % の仮想モニタで 40 本全部を緑で通しました。

**シナリオは無音です(✅Q-11)。**AU-3 から player は audio の backend を起動時に選べます(`--audio-backend device|offline|null` と
環境変数 `PELICAN_AUDIO_BACKEND`。`null` は device を開きません)。Studio の QProcess は環境を継承するので、driver は Studio を起動する環境に
`PELICAN_AUDIO_BACKEND=null` を入れます([無音にする所](../../tools/studio_drive.py#L1293))。呼び出し側が既に値を設定していれば
(`device` / `offline` など)その値を尊重し、**空文字は未設定として扱って** `null` に置き換えます(player と同じ扱い)。シナリオ
`graph_read` は `get_status` の `audio.backend` が `null` で、`audio` に `fallback_reason` が無い(鍵が `available` と `backend` の 2 つだけ)ことを確かめます。
Q-10 の時点では、埋め込んだ player は窓があるので実物の device を選び、シナリオの間も speaker から音が出ていました。

**report.json は AI が読む形です**([組み立て](../../tools/studio_drive.py#L1422)): `verdict`(`green` / `red`)と
`ok` / `failed_steps`、`summary`(step ごとに 1 行)、`failures`(送った命令・返ってきた応答・言葉での理由・
**失敗した瞬間のスクリーンショット**)、`steps`(全文)、`studio_log`(Studio と engine の console 出力)。
`studio_exit_code` と `studio_shutdown_forced` も記録します。最後の shutdown step は
**exit 0、強制終了なし**を要求し、異常終了・タイムアウトによる強制停止も report と driver の失敗になります。

現在のシナリオは **40 本**です(✅LP-4f1 の `loop_view_axis` と ✅LP-4f2 の `graph_template_bloom` で 31 本から 33 本に、✅EW-2 の `engine_window_embedded` で 34 本に、✅Q-19 の `loop_zone_read` で 35 本に、✅PM-3a の `play_pause_step` で 36 本に、✅PM-3b の `play_mode_roundtrip` と `play_view_camera` で 38 本に、✅PM-3c の `play_runtime_objects` で 39 本に、✅PM-3d で `play_view_camera` を `play_mode_roundtrip` にまとめて 38 本に、✅GL-1 の `graph_folded_features` で 39 本に、✅GL-2b の `graph_layers_origin` で 40 本になりました)。
`graph_read` は fixture を Graph タブで開き、`list` が objectName を返すこと・無い名前が
`automation.widget_missing@1` で断られること・キャンバスが描いたものが `get_graph_instances` の中身と合うことを見ます
([シナリオ](../../test/studio_scenarios/graph_read.json))。✅GL-1 から root は既定で Render の領域だけを描く(値の node は隠れる)ので、`graph_read` は `Show:` の Value を押してから、root の 2 つの node(値の node `clock` と instance の `left`)を名前で待ちます。GL-1 の前は canvas の node の数(2)を数えていましたが、畳んだ node が並ぶので数えなくなりました。`graph_edit_roundtrip` は**実物の engine で**値の帯を
click → type → Enter し、engine が `3.75` を持つこと、別の複製に切り替えると `2.5` のままであること、戻ってくると
`3.75` が残っていることを追います([シナリオ](../../test/studio_scenarios/graph_edit_roundtrip.json))。
✅Q-1b の 3 本は `canvas_zoom_pan`(Frame Plan のキャンバスを wheel / 素の move / 押したままの move / 中ボタン
drag で動かし、`view_state` で結果を読む)、`menu_actions`(popup を開かずにメニューバーを `menu_list` で読み、
`action` で発火する)、`graph_canvas_zoom_pan`(N-3 が入った Graph キャンバスの zoom / fit / pan)です。
加えて `theme_switch`、`graph_auto_layout`、`graph_reroute_tie`、`graph_observe_roundtrip`、
`frameplan_edit_roundtrip` がテーマ・配置・配線・値の観測・Frame Plan編集の往復を確認します。
✅VI-3a から、`frameplan_edit_roundtrip` が `ssao_blur` を足す先は `n8_probe_pass` の node ではなく binding 1 のソケット
(`node_socket#n8_probe_pass/in@1`)です。shader の `engine://fullscreen` が binding 1 も宣言していて、そこには `lit_color` が
つながっていないためです(待つ step が 1 つ増えて 29 step)。
✅F-2 の `new_blank_project` は File > New Project... の dialog にフォルダ(`{artifacts}` の下)・名前・Blank pipeline を
入れて Create し、開き直った Frame Plan に `present` と `output_transform` があること、`present` の fragment が
`engine://passthrough` であること(✅VI-2)、viewport の screen モードの
screenshot が 100 色を超えること(gradient が出ていること)、その screenshot の `capture` が `print_window` である
こと(✅F-5)を見ます([シナリオ](../../test/studio_scenarios/new_blank_project.json))。
✅F-1 の `render_target_authoring` は New render target... を action で開いて名前だけで作り、engine の
`render_targets[1]` が `source: "authoring"`・`extent_scale 1` であること、node の double-click で 0.5 にすると engine が
0.5 を持つこと、Delete で消えること、fixture の pass が書いている target の Delete がその pass を名指しして断られ、
target が残ることを追います([シナリオ](../../test/studio_scenarios/render_target_authoring.json))。
✅F-3 / F-5 の `fullscreen_pass_from_template`(47 step)は、`lighting_pass` の後に `gbuffer_albedo` を読んで `lit_color`
(画面に届く色)に書く pass を Add fullscreen pass → New… → Create で作り、`file` step で構文エラーを入れて Save が
compile log 付きで断られ form の値が残ること、直して Save すると node が描かれ viewport が albedo を描くことを見ます。
続けて(✅F-5)`file` step で shader を gradient に書き換えると Studio が `reload_shaders` を頼み viewport が gradient に
なること、typo を書くと node に印・Pass Editor の log 欄が typo を名指し・engine は前の gradient を描き続けること、
直すと印と log が消えて gradient に戻ることを追います
([シナリオ](../../test/studio_scenarios/fullscreen_pass_from_template.json))。自動化面の下では Studio は shader を
desktop で開かず、`automation: shader source open <path>` を log に書きます([差し替え](../../src/devstudio/view/uimain.cpp#L54))。
✅F-4 / F-6 の `render_pipeline_from_scratch`(97 step)は「0 から組める」第一段階の筋書きを端から端まで追います:
File > New Project... で `{artifacts}` に Blank pipeline の project を作り(画面は `present` の `engine://passthrough` が描く gradient)、New render
target... で `scene_color` を名前だけで宣言し、Add fullscreen pass... で `draw_pattern` を `present` の前に足して
`scene_color` に書かせ、New... の雛形を `file` step で 8 本の縞に書き換えて Save します。`present` に `scene_color` を
読ませる手順は、F-4 の時点では Studio で通らず `file` step の手書きで迂回していましたが、**✅F-6 から Studio の操作です**:
`present` を見出しで開き(editable な node の中ほどは shader の帯で、押すとその場編集になるため)、Save が root を書くこと・
engine が root の pass として出すことを確かめ、Inputs に `scene_color` を Add して Save します(root への `edit_preset_pass`)。
`present` の fragment は `engine://passthrough` のままで、入力 0 をそのまま出すので、`present` の shader は書きません
(✅VI-2。それまでは `present.frag` を `file` step で「入力 0 をそのまま出す」shader に書き換えていました)。
そのあと viewport の screenshot が 4 色以下で、同じ縞の上下が同じ色・隣の縞が違う色・手順 1 の gradient のその点とも
違う色であること、canvas と engine が `draw_pattern` → `present` の順であることを見て、もう一度別の project を開いて
戻っても同じであることを確かめます(Studio の再起動ではありません)([シナリオ](../../test/studio_scenarios/render_pipeline_from_scratch.json))。
✅F-7 の `whole_frame_first_steps`(33 step)は、既定 layout に戻してから、最初の frame plan が Whole frame で出ること、
Pass Editor を View > Panels で閉じた・開いた・閉じた後に viewport が帯にならず高さが戻ること、File > New Project... で
Blank pipeline に切り替えても新しい graph が Whole frame で開き、Engine Log(`pelican.engineLog`。gizmo の失敗は status bar と
同時にここへ残ります)に「polling failed」が無いこと(`excludes`)、New render target... で作った `scene_color` がその出力
ソケットごと `present` と同じ canvas に描かれ、canvas が Whole frame のままであることを見ます
([シナリオ](../../test/studio_scenarios/whole_frame_first_steps.json))。
✅G-1 の `material_pass_authoring`(44 step。G-4 で 38 から 43 に、VI-3a で 44 に増えました)は、model を 1 つ持つ direct root の fixture で、
Add material pass... を名前で発火し、form が Forward opaque・名前 `forward_opaque`・graph の先頭で開くこと、
Create missing targets が `scene_color` と `scene_depth` を contract の format で作ること、forward の既定の checkbox が出て
on であること(✅G-4)を見ます。Save の後は、Studio が engine を再起動し(status 行の「Restarting the engine」)、root に
`material_route_default` が書かれ、engine がその 2 つの target を authoring fragment から宣言し、`forward_opaque` を
forward opaque の material pass として最初に走らせることを見ます。続けて `present.frag` を `file` step で「入力 0 をそのまま
出す」shader に書き換え、読み直した shader が宣言する binding 0 の空のソケット(`node_socket#present/in@0`)が出るのを
待ってから、canvas で `scene_color` をそこへ drag し(✅VI-3a。それまでは `present` の node へ落としていました)、viewport が gradient から数色の絵になって中央の
box の色が背景とも前の gradient とも違うことを確かめます
([material pass のシナリオ](../../test/studio_scenarios/material_pass_authoring.json))。
✅G-3 の `lighting_pass_template`(41 step)は、G-buffer を持ち box 1 つと directional light 1 つを置いた fixture で、engine の
`lighting_pass` の後に Add fullscreen pass で、G-buffer 5 枚を読み `lit_color`(画面に届く色)に書く pass を New… の
Deferred lighting で作ります。viewport が 1 色でないこと(box が光源で照らされる)、`file` step で shader の light loop を
切ると画面が 1 色になり、戻すと照らされた box が戻ることを見ます
([lighting 雛形のシナリオ](../../test/studio_scenarios/lighting_pass_template.json))。
✅G-2 の `add_primitive_object`(57 step)は、File > New Project... で Blank pipeline の project を作り、Add Object > Cube を
名前で発火して、engine の scene に `Cube` が 3 つ目の object として入ること、それが選択され Inspector の model 欄が
`Cube (built-in)` を読むことを見てから Save Scene します。続けて Add material pass... で Forward opaque の pass を
Create missing targets で足し、forward の既定の checkbox を on のまま Save して engine の再起動を待ち(✅G-4)、再起動した
engine が保存済みの `Cube` を持つこと、`scene_color` を `present` の可変の帯の端(`node_socket#present/in:variadic`。✅VI-3a。
それまでは `present` の node へ落としていました)へ drag すると(`present` は `engine://passthrough` のままで
入力 0 をそのまま出すので、shader は書きません。✅VI-2)viewport の中央に背景とも前の gradient とも違う色の箱が出ることを確かめます。最後にもう一度 Add Object > Cube で
`Cube_001` ができて選ばれること、Inspector が読み終えてから Save し直すことを見ます(閉じるときに確認が出ないように)
([基本形のシナリオ](../../test/studio_scenarios/add_primitive_object.json))。G-2 の着地時は forward の pass だけでは箱が
描かれず、Deferred geometry の pass で迂回していました。G-4 で理由が分かり(material pass が 1 つも無いうちに置いた cube は
検査されず、forward の pass を足す編集も「前の世代が使っていた route の消失だけを断る」旧規則を通っていた)、forward に
戻っています。
✅OL-1 の `outliner_edit_roundtrip`(55 step)は、File > New Project... で Blank pipeline の project を作り、Add Object で
Cube と Floor を置きます(どちらも置いた時点で選ばれることを見ます)。Cube の行を選んで Duplicate を action で発火し、copy の
`Cube_copy1` が Cube の直後に宣言され(Floor が 1 つ下がる)、選ばれるのが Cube でも Floor の元の位置の object でもなく copy で
あることを見ます。`Cube_copy1` の行を `Floor` の行へ `drag` すると engine の scene で Floor の子になり、選択は copy のまま、
Inspector の Undo(`pelican.inspectorUndo`)で root に戻っても copy のままです。Delete は子孫が無いので問わずに消し、前の兄弟の
Cube が選ばれ、copy の行が消えることを見ます。Save Scene の後、保存した scene に copy が無いことを `file` step で見て、Cube の
authoring id を覚え、`pelican.viewportStop` / `pelican.viewportRestart` で engine を保存済みの scene で起動し直し、Cube が
同じ authoring id で 3 つ目にあり、選択が Cube のままで、Inspector が新しい engine から Cube を読むことを確かめます
([Outliner 編集のシナリオ](../../test/studio_scenarios/outliner_edit_roundtrip.json))。Alt の drag と行の間への drop は
シナリオでは通さず、widget test(`devstudio_outliner_edit_test`)だけです。
✅G-5 の 2 本は、第二段階の筋書きを Blank pipeline から Studio の操作だけで追います。
`forward_loop_from_scratch`(90 step)は File > New Project... で Blank pipeline の project を作り(名前は提案された
`project_001` のまま)、Add Object で Cube と Floor を置きます(Floor が箱の下にあり広いことを Inspector の欄で見ます)。
Add material pass... は枠が埋まった状態で開くので、Forward opaque を forward の既定つきでそのまま Save すると、scene が
未保存なので再起動の前に Save / Discard / Cancel が出て、Save で箱と床を保存してから engine が再起動します。続けて `present` を
見出しで開き、Inputs に `scene_color` を Add して Save すると、`present` の fragment は `engine://passthrough` のまま
(shader は書きません。✅VI-2)で、viewport の箱・床・背景が互いに違う色(前の gradient とも違う色)になります。
別の project を開いて戻っても同じ 3 色で、fragment も `engine://passthrough` のままです。VI-2 の前は New... の Tone map で
`present_tone_map` を作っていました(Fragment の Pick... の一覧の検査は deferred のシナリオへ移りました)
([forward ループのシナリオ](../../test/studio_scenarios/forward_loop_from_scratch.json))。
`deferred_loop_from_scratch`(116 step)は同じ出だしに加えて Cube を縦軸の回りに回し(`pelican.inspectorEuler.transform.rotation.1`
に `45` を打ち、quaternion の欄 `pelican.inspectorVector.transform.rotation.1` が `0.3827` になることを見ます)、
Add material pass... の Deferred geometry で G-buffer 5 枚と `scene_depth` を作ります(枠は埋まって開きます。route は既定の
ままなので再起動は求められません)。canvas の Target を **Whole frame** にして `deferred_geometry` を見出しで選び、
Add fullscreen pass... の New... で Deferred lighting を選ぶと、入力に G-buffer 5 枚が役割の順に自動で入ります。color 出力は
一覧の `New target: deferred_lighting_color...` で作ります(New render target の form が名前入りで開き、OK で pass の出力に
入ります)。この pass は `deferred_geometry` の後に置かれ、engine はこれを geometry と `present` の間で走らせます。`present` は
New... の Tone map で作る shader で `deferred_lighting_color` を読みます(照明の HDR をそのまま出すと 2 面の色の差が
保証できないため、passthrough のままにしません)。`present` は `engine://passthrough` で描いているので、提案される名前は
pass 名の `present` で(✅VI-2。以前は `present_tone_map`)、Fragment の Pick... の一覧からも同じ stem を選べることを見ます。光源が箱の 2 面を違う色に塗ること、Inspector で光源の向き
(`pelican.inspectorVector.light.direction.0`)を変えると光の離れた面の色が変わることを見ます
([deferred ループのシナリオ](../../test/studio_scenarios/deferred_loop_from_scratch.json))。
✅T-1 でこの 2 本を、T-1 が消した入力を使わない形に直しました。打つ欄は forward が 1 つ(New Project のフォルダ。以前は 5)、
deferred が 3 つ(フォルダ・回転の角度・光源の向き。以前は 9)です。
G-5 の時点では lighting の pass の置き場所だけを手で迂回していました(`present` の後に足してから、`file` step で fragment の
`insert` を書き換えていた)。preview の変種が geometry の fragment を `insert: "before:present"` の文字列で外し、それを anchor に
する lighting の fragment だけが残って、合成が `render feature pass insert anchor was not found` で落ちていたためです。✅G-6 で
preview の判定と除外の伝播が直り([第6章 §6.13](06_rendering.md))、迂回はなくなりました。G-6 の時点では、このシナリオは
最後に Inspector が busy のまま残って赤で終わっていました。原因は Inspector の欄の focus が始めた、誰も終わらせない編集で
(§13.1「Inspector の欄の編集と commit」)、✅Q-7 の後はこのシナリオと `forward_loop_from_scratch` が 2 回ずつ緑です
(Q-7 の commit の記録)。
`graph_reroute_tie` の node の drag は、✅Q-5 から窓の画素ではなく canvas の位置(`"scene"`)で終わります。F-5 から run は
毎回既定 layout で始まるので、窓の画素で置くと node が入力の列より左に来て、続く click が別の node に当たっていました
(bundle の drag はまだ窓の画素です)。
✅Q-13 の `frameplan_layout_then_edit`(41 step)は、配置の保存の後の編集を 2 回見ます。fixture で Graph タブの instance `left` を見出しで drag し、配置が保存された(`get_graph_instances` の `layout.nodes.left`)後に、Frame Plan の New render target... で `q13_after_graph` を作り、fragment の file に入ることを見ます。続けて Blank pipeline の project を作り、Frame Plan の `present` を drag して配置が保存された直後に Add material pass... → Deferred geometry → OK とし、engine が `present` の後に `deferred_geometry` を走らせ、target が 7 つ(6 枚と engine の display)あり、配置が残っていることを見ます。どちらでも status 行が `external_modification` を言わないこと(`excludes`)を確かめます([配置の後の編集のシナリオ](../../test/studio_scenarios/frameplan_layout_then_edit.json))。
✅Q-14 の `input_needs_shader`(69 step)は、Blank pipeline の project を作ったら、まず `file` step で入力を読まない `shaders/gradient.frag` を書き、Pass Editor の fragment の一覧(`pelican.fullscreenPass.shader.fragmentChoice`)で `present` に選んで Save します(Blank の `present` の `engine://passthrough` は入力が可変で、入力を足しても断られないため。✅VI-2)。続けて Cube と Deferred geometry の pass を足し、Target を Whole frame にして `gbuffer_albedo` の出力ソケットを `present` の node へ drag します(`gradient.frag` は入力を宣言しないので、`present` の行は名前順のままです)。✅VI-3b から Studio は送らず、status 行が `present's shader declares no inputs.` を含み、2 つのボタン(Use engine://passthrough / Open the shader)が出て押せること、engine の `present` の入力が 0 のままであることを見てから、Use engine://passthrough を押します。`present` が `gbuffer_albedo` を `engine://passthrough` で読むこと(1 回の編集)、`shaders/present_pass_through.frag` ができず `gradient.frag` は残っていること、ボタンが消えること、canvas が `in:gbuffer_albedo` を描くこと、viewport の中央が gradient でなくなり箱の albedo が背景と違う色になることを確かめます。VI-3b より前は engine の拒否(`present's shader (gradient) reads no inputs. ...`)を待ち、Make a pass-through shader が書いた `shaders/present_pass_through.frag` を見ていました。step 数は変わっていません([shader が読まない入力のシナリオ](../../test/studio_scenarios/input_needs_shader.json))。
✅VI-3a の `sockets_from_shader`(93 step)は、Blank pipeline の project で `present`(`engine://passthrough`)のソケットが可変の帯の端 `in:variadic` だけで、`in@0` が無いことを見てから、Cube と Deferred geometry の pass を足し、`gbuffer_albedo` の出力ソケットを帯の端へ drag します。`present` の入力が `gbuffer_albedo` 1 つだけになり、帯の中に `in@0` が出て(以前の名前 `in:gbuffer_albedo` でも引けます)、viewport が gradient でなくなることを見ます。`gbuffer_normal` を `present` の見出しへ落とすと、status 行が `Drop on a socket of present: its inputs follow its shader's declarations.` を言い、入力は 1 つのままです。続けて `file` step で固定の宣言 2 つ(`PELICAN_DECLARE_INPUT_0` / `_1`)を持つ `shaders/two.frag` を書き、Pass Editor の fragment の一覧で `present` に選んで Save すると、帯が消えて `in@1` が空のソケットで出ます。`gbuffer_normal` を `in@1` へ drag すると 2 つ目の入力になり、`deferred_geometry` から `present` への線が 2 本になります。`file` step で binding 1 の宣言を消すと、status 行が `present keeps drawing with its previous shader: the new one no longer declares binding 1 (gbuffer_normal).` を言い、Disconnect と Open the shader が出ること、engine の `pass_inputs.reload_refused.declarations` が 1 つで `declarations` は 2 つのままであることを見ます(赤い ○ は screenshot に撮るだけです)。Disconnect を押すと入力が `gbuffer_albedo` だけに戻り、`reload_refused` が消えて宣言が 1 つになり、Disconnect のボタンと `in@1` が消えて線が 1 本に戻ることを確かめます([宣言からのソケットのシナリオ](../../test/studio_scenarios/sockets_from_shader.json))。
✅VI-3b の `sockets_fix_and_show`(94 step)は、Blank pipeline の project で `present`(`engine://passthrough`)が帯の端 `in:variadic` だけで始まることを見て、Cube と Deferred geometry の pass を足し、`gbuffer_albedo` と `gbuffer_normal` を順に帯の端へ drag します。入力が 2 つになり、`in@0` の字が `← gbuffer_albedo (shown)` で終わることを見てから(define が無ければ入力 0 を出すため)、viewport の中央の色を覚えます。`in@1` を click すると、engine の `present` の `shader.defines` が `PELICAN_PASSTHROUGH_INPUT=1` の 1 つだけになり、入力は 2 つのまま、`in@1` の字が `(shown)` で終わって `in@0` からは消え、viewport の中央の色が変わります(normal の色を覚えます)。帯の横の Fix(`node_socket_fix#present`)を押すと、fragment が `project://shaders/present` になり、`PELICAN_PASSTHROUGH_INPUT=1` が残り、`file` step で `shaders/present.frag` が目印の 1 行と `layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform sampler2D in_gbuffer_normal;` を持つこと、ソケットの字が `in_gbuffer_albedo ← gbuffer_albedo` / `in_gbuffer_normal ← gbuffer_normal` で帯が無いこと、engine の `pass_inputs` が固定の宣言 2 つ(2 つ目が `in_gbuffer_normal`)で `variadic_range` が null であること、viewport の中央が覚えた normal の色のままであることを見ます。最後に `present` を見出しで選んで `pelican.framePlan.makeSocketsVariadic` を発火し、file に `PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);` が戻って目印が消えること、帯が戻り engine の `variadic_range.max` が 8 になること、入力が帯に残り viewport がnormal の色のままであることを確かめます([固定化と出す入力のシナリオ](../../test/studio_scenarios/sockets_fix_and_show.json))。
✅LP-4b の `view_select_field`(21 step)は、engine の `cube_capture` の feature を project の feature(`project://features/cube_capture.json`)として写した fixture を開きます(`test/fixtures/studio_view_select_field/`。写しは `parameters` を抜いて `$resolution` を 256 に書き下したもので、pipeline は `hybrid_v1`)。Whole frame で `cube_capture_ssao_blur` の node が出て、engine の `view_axes` の face の 2 番のラベルが `+Y` であることを見てから、見出しの double-click で Pass Editor に開き、View の欄(`pelican.fullscreenPass.view.face`)が `All views` であることを見ます。`2 (+Y)` を選んで Save すると、engine の node の `view_select.face` が 2 になり、canvas に `node_view_select#cube_capture_ssao_blur` の 1 行が出て、欄が `2 (+Y)` で読み直されることを見ます。`All views` に戻して Save すると、node から `view_select` が消え(`excludes`)、1 行も消えます。打つ欄はありません([view の軸のシナリオ](../../test/studio_scenarios/view_select_field.json))。
✅LP-4c の `graph_loop_expand`(json の step は 81。✅Q-18 で Frame Plan の Refresh の click を外して 82 から減りました。LP-4c の実装者の報告では 84 step で緑)は、Blank pipeline の project を作り、View ▸ Render Tools ▸ Graph で Graph タブを前に出して **Add loop** を発火します。root に `graph.loop` が立ち、`over.range.count` が 1 で、分岐の body が同じ編集で作られた空の `loop_body` であることを見ます。ループを選んで Loop の欄が出ること、parameters の pill が 0 個であることを確かめ、**Open body** でパンくずが `root / loop_body` になり分岐の combo が出たら、**Add fullscreen pass** で body に image と pass の 2 つを置きます。root に戻って Count に 3 を打って Enter すると、engine の `loops[0]` が `count` 3・要素 3 つ・`tier` `expand` になり、Loop の欄の要約が `expand ×3` を言います。Frame Plan を前に出すと、畳まれたグループ `loop loop ×3` が出ます(✅Q-18 から Refresh は押しません)。Count を 1 にすると要素が 1 つになり、**Add branch** で `element = 0` の分岐が else の前に入ります。その body を `(nothing)` にすると、要素 0 の `branch` が 0 になり、else が `unmatched_branches` に入り、表の else の行が `no element takes this branch` を言います。最後に Undo を 3 回押し、分岐の body・分岐の数・回数 3 が順に戻り、engine がまた 3 回展開することを見ます。打つ欄は New Project のフォルダと名前、Count の 2 回です([ループのシナリオ](../../test/studio_scenarios/graph_loop_expand.json))。
✅LP-4e の `material_loop_decompose`(json の step は 46。✅Q-18 で Refresh の click を外して 47 から減りました。LP-4e の実装者の報告では 49 手順で緑)は、root の rendering 節が forward の material pass `forward_opaque` を宣言する fixture を開きます(`test/fixtures/studio_material_loop_decompose/`。箱が 1 つと、平らな色の material `g1_flat`)。viewport の中央と隅の色と色の数を覚えてから、Frame Plan で `forward_opaque` を選んで **Decompose into loop...** を発火し、確認のダイアログが `Undo takes it back` を言うことを見て Decompose を押します(LP-4e の着地時は `cannot be undone yet` を見ていました。RU-1 で変わりました)。root に `forward_opaque_draws` のループと `render.material_pass` ができ、Graph タブが前に出てループが選ばれること、`loops[0].tier` が `draw_loop` で、Loop の欄が Contract に `forward_opaque_v1` を出して Count を出さないこと、viewport の 2 点の色と色の数が分解の前と同じことを確かめます。**Add branch** で最初の material(`pelican.builtin.transparent`)の分岐が入ったら、分岐の material の combo で箱の material `materials/g1_flat.material.json#g1_flat` を選び(鍵は打ちません)、body を `(nothing)` にします。中央が背景の色になって色の数が 1 になり、engine の `loops[0].elements[2]` がその material で `branch` 0、最初の built-in の material が else に移り、表の行がその鍵を言うことを見ます。最後に Frame Plan を前に出して(✅Q-18 から Refresh は押しません)、material pass に `node_draw_loop#forward_opaque` の 1 行があることを見ます。打つ欄はありません([分解のシナリオ](../../test/studio_scenarios/material_loop_decompose.json))。
✅RU-1 / RU-1b の `render_undo_roundtrip`(json の step は 127。実装者の報告では 129 手で緑)は、`studio_material_loop_decompose` を別の名前で写し、`scene_color` を反転して描く `shaders/ru1_invert.frag` を足した fixture を開きます(`test/fixtures/studio_render_undo_roundtrip/`)。まず Frame Plan で `present` を見出しで開き、Pass Editor の fragment の一覧で `project://shaders/ru1_invert` を選んで Save し、viewport の中央の色が変わることを見ます。Frame Plan の toolbar の **Undo** で fragment が `engine://passthrough` に戻り、中央と隅の色が編集の前と同じになり、**Redo** で反転に戻ることを確かめます(最初の Undo をキーでなくボタンで押すのは、この lane では窓が active にならずキーが届かないためです。キーの振り分けは `devstudio_render_undo_keys_test` が見ます)。続けて `forward_opaque` を Decompose into loop で分解し(確認の文が `Undo takes it back` を含むことを見ます)、**Edit ▸ Undo Render Edit** を発火して、root の node が 0 個になり、`forward_opaque` が root の material pass に戻り、Frame Plan に draw_loop の行が無いことを見ます。Redo でループが戻り、Undo でもう一度戻します。次に Add fullscreen pass で `ru1_tint` を足し、`get_render_documents` の文書が 2 つになって `passes/authoring/` に fragment のファイルができたら、Undo で `Undid "Add pass ru1_tint".` が出て、pass が canvas から消え、文書が 1 つに戻り、ファイルも消えることを確かめます。同じ名前でもう一度足せて同じファイルに書かれ、Undo の後の Redo でファイルが戻ります。最後に Pass Editor で `present` の fragment を `engine://passthrough` に戻して Save し、Frame Plan で `present` を見出しで drag して配置を root に書かせてから Undo し、`Undid "Edit pass present".` が出て fragment が反転に戻り、drag の配置が残ることを見ます。打つのは pass 名 `ru1_tint` の 2 回です([Undo のシナリオ](../../test/studio_scenarios/render_undo_roundtrip.json))。
✅LP-4d1 の `loop_zone_edit`(json の step は 66。実装者の報告では 68 手で緑)は、Blank pipeline の project を作り、Graph タブで **Add loop** を発火して、root に `graph.loop` が立ち body の `loop_body` が空であることを見てから、ループが枠(`graph_zone#loop`)と段の字(`graph_zone_label#loop/stage`)で描かれるのを待ちます。見出しを押してゾーンを選び(Loop の欄が出る)、**Add fullscreen pass** を発火すると、body が image と pass の 2 つを持ち、root はループ 1 つのままで、pass が枠の中に `graph_node#loop_body/render_fullscreen` として描かれ、`graph_node#render_fullscreen` の名前は無いことを見ます。見出しでゾーンを選び直して Count に 2 を打って Enter し、root の `layout` がまだ空のうちに、pass を見出しで枠の中へ drag します。definition の `layout.nodes` に `render_fullscreen` が入り、root の `layout` は空のままであることを見て、definition の配置を覚えます。engine の `loops[0].count` が 2 になり、段の字が `expand ×2` を言い、Frame Plan の Refresh の後にグループ `loop loop ×2` が出ることを見ます。Graph タブに戻って枠を見出しで drag すると、root の `layout.nodes` に `loop` が入って `render_fullscreen` は入らず、definition の配置は覚えた値のままで、pass は枠の中に描かれたままです。最後に Undo(`pelican.graphCanvas.undo`)を 2 回発火し、枠の移動が戻って root の `layout` が空になり(definition の配置はそのまま)、次に pass の移動が戻って definition から `layout` が消えることを見ます。Count を 2 にするのは、Frame Plan が 2 つ以上の回をグループにするためです(実装者の報告)。打つ欄は New Project のフォルダと名前、Count の 1 回です([ゾーンのシナリオ](../../test/studio_scenarios/loop_zone_edit.json))。
✅LP-4d2 の `loop_zone_wire`(json の step は 77。実装者の報告では緑)は、縁をまたぐ 3 本の線を引きます。Blank pipeline の project を作り、Graph タブで root に **Add fullscreen pass**(その image が枠の外の node です)、**Add loop** を発火し、新しいループが root の pass に重なるので **Auto layout** を発火します。ゾーンを選んで body に Add fullscreen pass を足し、Count を 2 にして、engine の `loops[0].count` が 2 になったら root の `nodes` と `definitions` を覚えます。このとき Frame Plan の `barriers` に、`loop/0/render_fullscreen` から `loop/1/render_fullscreen` への `write_after_write`(resource `render_fullscreen__color`)が無いことを見ます。(1) root の image の出力(`graph_socket#render_fullscreen__color/resource`)を、枠の中の body の pass の color 入力(`graph_socket#loop_body/render_fullscreen/in:color`)へ drag します。ループの入力 `resource` が root の image から来て、body に `graph.input` が立ち、pass の `color` がそれを読み、ループの外側のソケット `graph_socket#loop/resource` が描かれることを見ます。2 つの回が同じ image に描くようになるので、上の barrier が出ることも見ます。(2) body の pass の出力(`graph_socket#loop_body/render_fullscreen/out:color`)を右の縁の `graph_zone_socket#loop/out:+` へ drag し、body の `outputs.color` が pass を指し、ループの `outputs[0]` が `color` になることを見ます。(3) 右の縁の内側の端 `graph_zone_socket#loop/out:color` を左の縁の内側の端 `graph_socket#loop_body/resource/value` へ drag し、ループの `carry[0].to` が `resource` になり、矢印 `graph_zone_carry#loop/color/resource` が描かれることを見ます。下端の `graph_zone_label#loop/expansion` が `×2 copies` を言うのを待って **Show in Frame Plan**(`graph_zone_button#loop/frame_plan`)を押し、Frame Plan の status 行が `Loop loop: its copies are picked.` を言うことを見ます。最後に Graph タブへ戻って Undo を 3 回発火し、root の `nodes` と `definitions` が覚えた値と一致することを見ます。このシナリオでは carry の効き目は Frame Plan では見分けられません(上の「分かっている穴」。✅Q-19 の `loop_zone_read` が見ます)。打つ欄は New Project のフォルダと名前、Count の 1 回です([縁をまたぐ線のシナリオ](../../test/studio_scenarios/loop_zone_wire.json))。
✅Q-19 の `loop_zone_read`(json の step は 80。実装者の報告では 82 step で緑)は、線だけで、body の pass に前の回の出力を読ませます。Blank pipeline の project を作り、Graph タブで root に **Add fullscreen pass**、**Add loop**、**Auto layout** を発火し、ゾーンを選んで body に Add fullscreen pass を足して、Count を 2 にします。(1) root の pass の出力 `graph_socket#render_fullscreen/out:color` を左の縁の `graph_zone_socket#loop/in:+` へ drag し、ループの入力 `color` が root の pass から来ることを見ます(pass の出力 port から引くので、回の pass はその pass を待ちます。root の image から body の pass のソケットへ引く形は、Q-19 の着地時は engine が断っていました。✅LP-6 で塞がりました。上の「render node の入力は shader の宣言から」の分かっている穴)。(2) body の pass の出力を右の縁の `+` へ drag してループの出力 `color` を作り、(3) 右の縁の内側の端 `graph_zone_socket#loop/out:color` を左の縁の内側の端 `graph_socket#loop_body/color/value` へ drag して、carry と矢印を作ります。body の pass(`engine://passthrough`)のソケットが可変の帯の端 `graph_socket#loop_body/render_fullscreen/in:variadic` で描かれるのを待ち、frame plan の `loop/1/render_fullscreen` の `reads` に `loop/0/render_fullscreen__color` が無いことを見て、`definitions` を覚えます。(4) 左の縁の内側の端を帯の端へ drag すると、pass に port `color_2`(`binding` `input`・`input_index` 0。pass 自身の `color` と重なるので `_2`)ができて body の `graph.input` の `color` から読み、`in@0` の ○ が描かれ、`loop/1/render_fullscreen` の `reads` に `loop/0/render_fullscreen__color` が入ることを見ます。次にゾーンを選んで **Rename definition…** を発火し、名前の欄が `loop_body` で開くのを見て `stroke` を打って Enter すると、`definitions.stroke` ができ、ループの `branches[0].body` が `stroke` になります。Undo で `loop_body` に戻り、もう 1 回の Undo で `definitions` が覚えた値と一致することを見ます。打つ欄は New Project のフォルダと名前、Count の 1 回、definition の新しい名前の 1 回です([読みの線のシナリオ](../../test/studio_scenarios/loop_zone_read.json))。
✅LP-4f1 / LP-4f2 の `loop_view_axis`(json の step は 53。✅Q-18 で Refresh の click を外して 54 から減りました)は、`studio_view_select_field` と同じ文書(`hybrid_v1` と、project の feature に写した `cube_capture`)で root の graph が空の fixture を開きます(`test/fixtures/studio_loop_view_axis/`)。Graph タブで **Add loop** を発火し、ゾーンを見出しで選んで Loop の欄の要約が `expand ×1` を言うのを待ってから、Over で `View axis`、family で `$capture/cube` を選びます。文書の `over.view_axis` が `$capture/cube` / `face` になり、回数の字が `×6 (face)` になることを見ます。ゾーンを選び直して **Add fullscreen pass** を発火すると、body の image が `width` 256・`height` 256・`layers` 6 で作られ、pass の `view_family` が `$capture/cube` であることを見ます(✅LP-4f2。LP-4f1 の着地時は、画面の大きさの pass が engine を止めたため fixture の `cube_face` の definition を body にしていました)。pass を見出しで選ぶと View 欄の `face` が `All views` で出るので(✅Q-18 から、この `wait` は欄が有効になるのも待ちます)、**Each element (loop)** を選び、pass の `view_select.face` が `{"$expr": "element"}` になることを見ます。engine の `loops[0].count` が 6・`tier` が `expand` になり、frame plan の `loop/0/render_fullscreen` 〜 `loop/5/render_fullscreen` がそれぞれ `view_select.face` 0〜5 を持ち、ゾーンの段の字が `×6 (face)`、3 つ目の軸のラベル(`graph_zone_label#loop/axis2`)が `+Y` を言うことを確かめます。最後に Frame Plan を前に出して(✅Q-18 から Refresh は押しません)、グループ `loop loop ×6` が出ることを見ます。打つ欄はありません([view の軸のループのシナリオ](../../test/studio_scenarios/loop_view_axis.json))。
✅LP-4f2 の `graph_template_bloom`(json の step は 46)は、`studio_automation_graph` の rendering 文書(direct root の features に bloom)に、engine の OpenPBR の発光する material で光らせた箱を置いた fixture を開きます(`test/fixtures/studio_graph_template_bloom/`)。viewport の中央と隅の色と色の数を覚えてから、Render Features で `bloom` の行を選び、**Replace with loop version…** が出て押せることを見て押します。確認のダイアログが `Bloom loop`・`The feature bloom is removed`・`Undo takes it back` を含むことを見て Replace を押すと、root の features が 0 本になり、graph に `graph.loop` の `bloom_down` が入り、Graph タブが前に出てそのゾーンが描かれ、Loop の欄の要約が `bloom_down` を言い、Render Features の notice が `Replaced bloom with Bloom loop. Undo takes it back.` であることを見ます。engine の `loops[0].node` が `bloom_down` で、viewport の中央・隅の色と色の数が置き換える前と同じことを確かめ、Frame Plan を Refresh してループのグループを見ます。**Edit ▸ Undo Render Edit** で bloom が features に戻り graph が 2 node に戻ること、**Redo** でループが戻ることを見て、どちらでも絵が置き換える前と同じことを確かめます(実装者の報告では中央 `#ffffff`・隅 `#000000`・色数 1016)。打つ欄はありません([分解版のシナリオ](../../test/studio_scenarios/graph_template_bloom.json))。
✅EW-2 の `engine_window_embedded`(json の step は 12)は、engine の窓が Studio の外に一度も出ないことを見ます。シナリオが `"watch_engine_windows": true` を持つと、driver は Studio を起動した時から監視の thread を回し([`EngineWindowWatch`](../../tools/studio_drive.py#L260))、Studio を親に持つ process の top-level の窓を列挙して、見える窓を記録します(実装者の報告では約 25 ms ごと)。property 付きで owner の無い非表示の窓(読み込み中の engine の窓)を見た回数も engine ごとに数えます。expectation `engine_windows`([判定](../../tools/studio_drive.py#L1099))は、見える窓が 1 つも無いこと、`engines` 本以上の engine について非表示の窓を `min_hidden_samples`(既定 3)回以上見たこと(監視が読み込みを見逃したまま緑にならないように)、最新の engine の窓が見えていて、その root の窓が Studio の process のものであることを要求します。シナリオは project と一緒に起動した engine、Stop Engine → Restart Engine の engine、別の project(`{other_project}`)への切替の engine の 3 回でこれを見て、最後に `screen` モードで viewport を撮ります。実装者の報告では緑(約 19 秒)で、`--embedded` を外す変異では窓が 144 sample 見えて赤でした。⚠ 監視の間隔より短い表示は取り逃がしえます。撮った viewport は黒でした(それが正しい絵かは未確認)。打つ欄はありません([窓のシナリオ](../../test/studio_scenarios/engine_window_embedded.json))。
✅PM-3a の `play_pause_step`(json の step は 35。✅PM-3b で edit 起動に合わせて直しました)は、behavior `pm3a_mover`(game の 1 歩ごとに object の x を 0.25 進める。`test/fixtures/pm3a_mover/`)を付けた object `Mover` の fixture を開きます(`test/fixtures/studio_play_pause_step/`)。behavior は game logic の DLL なので、`test/CMakeLists.txt` に `pelican_studio_<シナリオ名>_game_logic` の target があれば、test がその DLL を run の directory に写し、Studio は `PELICAN_STUDIO_PLAYER_ARGUMENTS` の `--game-logic` で engine に渡します([写す所](../../test/run_studio_scenario.cmake#L47))。起動の直後は status bar の行が `Edit` で Pause が押せないことを見てから、**Play** を押します。行が `Playing · frame` を言い、Step が押せず、`get_components` の `Mover` の x が増えること、viewport の枠の左端が paused の色でないことを見ます。Pause を押すと行が `Paused · frame N` になり、N が `get_status` の `play.game_frame` と一致します(比べる文は、覚えた値から `value_format` で作ります)。評価の番号(`get_status.frame`)が 30 以上進んでも x と `game_frame` が変わらないこと、枠の左端が `#e0ac00` であることを見ます。Step を押すと `game_frame` がちょうど 1、x がちょうど 0.25 進み(`plus`)、行もその frame を言います。Pause をもう一度押す(Resume)と行が `Playing` に戻り、x がまた増え、枠の左端が `#e0ac00` でなくなります。最後に Stop を押し、行が `Edit` に戻って x が 0 に戻ることを見ます。打つ欄はありません([一時停止のシナリオ](../../test/studio_scenarios/play_pause_step.json))。

✅PM-3b の `play_mode_roundtrip`(PM-3b の着地時の json の step は 97。PM-3d で 118)は、`studio_render_undo_roundtrip` の project(Pass Editor で `present` を `shaders/ru1_invert.frag` に替えると背景が反転する)に `pm3a_mover` の `Mover` と directional light の `Sun` を足した fixture を開きます(`test/fixtures/studio_play_mode_roundtrip/`)。PM-3b の着地時は model を描きませんでした(PM-2b の前の穴)。PM-3d で箱と camera を足しています。起動が `Edit`・`play.session` が false・Play が押せて Pause が押せず Save Scene が押せること、評価の番号が 30 進んでも `Mover` の x が変わらないことを見ます。(1) Play で x が増え、`session` が true、Save Scene が押せず、render-tools の帯が出ること、Stop で x が起動時の値に戻り、Save Scene が押せて帯が消えることを見ます。(2) Play して Inspector で `Sun` の `intensity` を 7 にすると、Inspector の行が `reverts on Stop` を言い、`recorded_edits` が 1 件になり、Keep を付けずに Stop すると `authored_json.intensity` が 3 に戻り、Inspector の欄も `3.00` になります。(3) もう一度 Play して 5 にし、Play changes を開いて最初の行が `Edit light.intensity` であることを見て check を付けて Stop すると、5 が残り、要約が `Stopped: 0 reverted, 1 kept.` です。(4) Play して `Mover` を選び、x が 2 を超えてから Take from game を押すと、行が `Take Transform from game committed` を言い、`authored_json.pos[0]` が 2 を超え、`recorded_edits` が 1 件です。Stop の後、authoring と edit の runtime の x がその値のままです。(5) Play して Frame Plan から `present` を Pass Editor で開き、Pass Editor の帯が `Changes here are written to files and stay after Stop.` を言うことを見て、fragment を `project://shaders/ru1_invert` にして Save します。Stop の後も `get_render_authoring_context` の fragment がそのままで、viewport の隅の色が起動時と違うことを見ます。最後に edit で File ▸ Save Scene を発火し、`Scene saved` を待ちます。打つ欄は `intensity` の 2 回です([再生と停止のシナリオ](../../test/studio_scenarios/play_mode_roundtrip.json))。

✅PM-3b の `play_view_camera` は ✅PM-3d で `play_mode_roundtrip` にまとめ、消しました。今の `play_mode_roundtrip`(json の step は 118)の fixture は、箱(`CheckerBox`)と、`pm3a_mover`(`step` 0.05)で動く camera も持ち、edit では editor camera で描くこと・View: Game | Editor が edit と paused で出て playing では出ないこと・paused で Editor に切り替えると絵が game の camera と違うこと・Stop で editor camera に戻ること、そして model のある scene で Keep 付きの Stop の後に 30 フレーム進んでも engine が箱を描き続け、Keep した `intensity` が残ることを見ます(✅PM-2b の前の engine では、この Stop の直後に player が応答しなくなって赤)。

✅PM-3c の `play_runtime_objects`(json の step は 33)は、最初の歩で prefab `pm3c_shot` から `Shot` を (1.5, 2, -0.5) に spawn する behavior `pm3c_spawner`(`test/fixtures/pm3c_spawner/`)を付けた object `Spawner` の fixture を開きます(`test/fixtures/studio_play_runtime_objects/`)。edit では `get_runtime_objects` の `objects` が空で、Outliner に `pelican.outliner::row#Shot (runtime)` が無いことを見ます。Play すると engine が `Shot`(`spawned_by.prefab` が `pm3c_shot`)を 1 つ返し、Outliner に `Shot (runtime)` の行が出て、`Spawner` の行も残ります。その行を選ぶと、Inspector の選択の行が `Shot (runtime)` と `spawned from pm3c_shot` を言い、`pelican.inspectorRuntime.position` と `.worldPosition` が `1.5, 2, -0.5` で、欄(`pelican.inspectorProperties`)と Add component が見えず、Outliner の Delete と Duplicate が押せません。Stop で `objects` が空になり、行が消え、Inspector の選択が `None` になります。もう一度 Play すると行が戻り、最後に Stop して行が消えることを見ます。打つ欄はありません([runtime の行のシナリオ](../../test/studio_scenarios/play_runtime_objects.json))。

✅GL-1 の `graph_folded_features`(json の step は 29。実装者の報告では 31 手で緑)は、`studio_material_loop_decompose` を写した fixture を開きます(`test/fixtures/studio_graph_folded_features/`)。root の graph は node を持たず、rendering 節が forward の material pass `forward_opaque` と、`scene_color` を `engine://passthrough` で見せる `present` を宣言します。`get_graph_instances` の node が 0 個であることを見てから、Graph タブの root に `graph_node#forward_opaque` と `graph_node#present` の畳んだ node が出て、その間に破線 `graph_derived_edge#forward_opaque->present` が引かれ、`Show:` の Render の check(`pelican.graphCanvasShow.render`)が見えることを確かめます。`forward_opaque` をダブルクリックすると、パンくずが `root / forward_opaque` になり、ゾーン `graph_zone#forward_opaque_draws` と body の `graph_node#forward_opaque_draw/step`(Studio が `draw.material` として組む node)が出ます。中で **Add loop** を発火すると、`pelican.graphCanvasFoldedNotice` が `This is the engine's folded implementation: expand it to edit.` と `Expand to edit` を言い、文書の node は 0 個のままです。パンくずを押して `root` に戻り、畳んだ node が戻って 1 行が消えることを見ます。最後に Frame Plan で `forward_opaque` を選んで **Open in Graph** を発火し、Graph タブの要約の行が `forward_opaque (material)` を言い、node が見え、文書の node が 0 個のままであることを見ます。root と中の絵を 1 枚ずつ撮ります(`folded_root.png` / `folded_inside.png`)。打つ欄はありません([畳んだ node のシナリオ](../../test/studio_scenarios/graph_folded_features.json))。

✅GL-2b の `graph_layers_origin`(json の step は 30。GL-2b の着地時は 31 で、✅GL-1b でループの帯の `×3 copies` を待つ 1 手を外しました)は、engine の `hybrid_v1` の preset と、`shadow_directional` の feature の `shadow_depth` を graph のループ `cascades` で置き換える(`pass_replacements`)fixture を開きます(`test/fixtures/studio_graph_layers_origin/`)。root には、`tint_image` に描く fullscreen pass `tint` と、それを入力 0 に読んで `show_image` に描く `show`、definition `paint` の instance `left`、影の cascade を 3 回回すループ `cascades`(要素 0 は body `near`、他は else の body `cascade`)があります。engine の `nodes[2].origin.loop` が `cascades` であることを見てから、Graph タブでゾーン `graph_zone#cascades` の下の帯 `graph_layer_band#cascades` が `barriers` を言うこと、preset の pass `deferred_geometry` が畳んだ node で出ること、`pelican.graphCanvasDiagnostics` が `diagnostics` を言うこと、何も選ばない Layers の見出しが `Layers: the whole graph` であること、ゾーンが要素 0 の body の `graph_node#near/depth` を描くことを見ます。Frame Plan の Barriers(`pelican.framePlan.tab.barriers`)で `tint` から `show` への barrier 20 の行を選び、`pelican.framePlan.openRowInGraph` を発火すると、Graph タブの Layers の見出しが `Layers: tint` になり、`L2 physical` の組が出ます。次に `cascades/2/depth` からの barrier 9 の行で同じことをすると、ゾーンのタブが else に移り、`graph_node#cascade/depth` が見えて `graph_node#near/depth` が消えます。最後に文書の root の node が 7 個のままであることを見ます。絵を 3 枚撮ります(`layers_root.png` / `layers_tint.png` / `layers_copy.png`)。打つ欄はありません([層のシナリオ](../../test/studio_scenarios/graph_layers_origin.json))。

⚠ **2 つの注意。**(1) `open_project` に**今開いている directory** を渡すと何も起きません(`EmbeddedViewport::openProject`
がそう作られています)。ディスクから読み直させたいときは、別の project を開いてから戻ります。
(2) `screenshot` の既定(`widget` モード)は Qt の widget 木を描くので、**埋め込まれた engine の窓は写りません**
(別 HWND です)。engine が描いた絵が要るときは `"mode": "screen"` を使います。**✅F-5 から `screen` は、Studio の窓
自身の合成済みの内容を `PrintWindow`(`PW_RENDERFULLCONTENT`)で読んで切り出す**ので、別の application の窓が
viewport に重なっていても写りません([窓の内容の読み取り](../../src/devstudio/automation/automationcommands.cpp#L135))。
窓が print できないとき、または 1 色で出て**しかも desktop のその場所に Studio の窓が実際に見えているとき**だけ、以前の
desktop の grab に戻ります。見えているかは、撮る範囲の 5×5 の点で `WindowFromPoint` が Studio の窓(埋め込みの engine の窓を
含む)を指すかで見ます([desktop に窓が見えているかの判定](../../src/devstudio/automation/automationcommands.cpp#L191))。
別の窓が重なっていれば 1 色の print をそのまま使い、応答に **`desktop_obstructed: true`** を付けます。1 色は正しい絵でもあり得る
(消去されただけの target、空の scene の albedo)ので、重なった別の application の窓を撮るより print を残します(G-4 の後の修正
`5ca18ee1`。以前は 1 色なら必ず desktop に戻り、重なった窓を撮っていました)。
どちらで撮ったかは応答の **`capture`**(`print_window` / `screen`)に出ます。着地時に別の窓の重なりで 2 本のシナリオが
赤になったための変更です。
**倍率の違う 2 枚目のモニタでも同じ場所を撮ります(✅OT-1)。**OT-1 の前の `screen` は、widget の位置を画面の原点から論理 pixel で測って
窓の倍率を掛けていました。Qt の論理座標では各画面の原点は元の場所のまま中だけが拡大されるので、100 % の主モニタの右に 125 % のモニタがあると、
viewport の隣の panel を撮っていました。今は窓の client の原点から測った差にだけ倍率を掛け、辺ごとに四捨五入します(Qt と同じ丸め。
切り捨てると 125 % で隣の panel の 1 行が入ります。[`physicalScreenRect()`](../../src/devstudio/automation/automationcommands.cpp#L85))。PrintWindow からの切り出しと、desktop に窓が
見えているかの判定も同じ矩形を使います。desktop に戻るときは、`QScreen::grabWindow` ではなく desktop を `BitBlt` で native に読みます
([`desktopRegion()`](../../src/devstudio/automation/automationcommands.cpp#L101)、[使う所](../../src/devstudio/automation/automationcommands.cpp#L1995))。それも読めなければ、窓のある画面の `grabWindow` を、その画面の左上からの座標で呼びます。
`screenshot` の応答は `path` / `mode` / `capture` / `width` / `height`(と、上の場合だけ `desktop_obstructed`)に加えて **`distinct_colors`**(絵に含まれる色の数)を返します
(✅F-2、[screenshot の distinct_colors](../../src/devstudio/automation/automationcommands.cpp#L2038))。消去されただけの viewport は 1 色なので、
`result` step で `distinct_colors` を読めば「engine が何か描いたか」を PNG を開かずに判定できます。
**`"pixels": [[x, y], ...]`**(✅F-4)を付けると、応答にその各点の色が `"#rrggbb"` の配列 `pixels` で返ります
([screenshot の pixels の読み取り](../../src/devstudio/automation/automationcommands.cpp#L2045))。x / y は絵の幅・高さに
対する 0〜1 の割合なので、窓の大きさを知らずに書けます。配列でない・2 要素でない・範囲外なら `automation.bad_request@1` です。
`result` step の `remember` で 1 点の色を覚え、別の点を `value_of` と `op`(`eq` / `ne`)で比べれば「同じ縞の下も同じ色、隣の縞は違う色」の
ように、output transform がどんな色にしても絵の形を判定できます。

**この面が最初の往復で本物のバグを見つけました。**値の帯を click → type → Enter すると「Graph change saved.」と
出るのに古い値が保存される —— `QDoubleSpinBox` の `keyboardTracking=false` と、editor の filter が Enter を先に
消費する組合せで、scene test の合成 Enter では通っていました。修正は `commitInlineEdit()` で `interpretText()` を
呼ぶことです([修正](../../src/devstudio/view/graphcanvaswidget.cpp#L3018)) —— **`--automation` 無しでも挙動が変わる
唯一の変更**で、手で編集したときの取りこぼしもこれで直っています。

### Inspector のグラフ装着

Inspector の `Add component...` / `Ray geometry...` の下、**properties の scroll 領域より上**に
**Attached graphs** グループがあります([置き場](../../src/devstudio/view/inspectorwidget.cpp#L269)。
properties は [:219](../../src/devstudio/view/inspectorwidget.cpp#L336) の `pelican.inspectorProperties` です)。
選択中のオブジェクトを owner とする `graph.instance` の一覧と、**Attach...** / **Detach** / **Refresh** です。

**装着する**: **Attach...** で `Attach graph` ダイアログが開きます。`Definition` で definition を選ぶと、
`Instance name` に重複しない候補が入り、その definition の `graph.input` ごとに数値欄が並びます
(既定値は宣言の `default`、無ければ 0)。**Refresh source** は文書を取り直し、**Attach and save** が
`edit_graph_instances{attach}` を 1 回送ります。欄が数値でなければ送る前に止まります。

**入力を見る**: 一覧で instance を選ぶと **Instance inputs** グループが開き、port ごとに 3 つの表示が並びます。

| 列 | 意味 |
|---|---|
| mode | その port に**今どこから値が来ているか**。`connected`(辺)> `parameter`(文書に書かれた値)> `default`(definition の宣言)の順で決まります([判定](../../src/devstudio/model/graphattachmentmodel.cpp#L327)) |
| 値の欄 | **著作された値**(`parameters`、無ければ宣言の `default`)。編集できるのは値を持つ port だけです |
| live | **描かれた世代が実際に使った値**と、その出所を `<値> (<source>)` で。publish 前は `not published` |

**mode が `connected` でも、著作された値は消えていません。**接続を外せばその値に戻ります —— これが
「二重指定を拒否する」のではなく「優先順で決める」設計にした理由です。

**4 つの導線**(port ごとのボタン):

1. **Control with a curve** —— 前節のノード集合を作って繋ぎます(`edit_graph_document`)。
2. **Connect to another output** —— `Connect input` ダイアログで別ノードの出力を選び、**Connect** で `set_input`。
3. **Open the source** —— Graph タブを前に出し、その producer を選択します
   ([繋ぎ込み](../../src/devstudio/view/mainwindow.cpp#L445))。接続されていない port で押すと
   「これは開ける供給元を持たない、保存された値だ」と notice が出ます。
4. **Disconnect** —— `set_input{null}`。**mode が `connected` のときだけ押せます**
   ([有効化](../../src/devstudio/view/graphattachmentwidget.cpp#L261))。他の 3 つは常に押せます。

値の欄を書き換えて focus を外すと `set_node_parameters` が飛びます(`parameters` 全体を読み、その port だけ
差し替えて送ります)。**この GUI の操作はどれも、同じ ops を手で送ったときと同じバイト列になります** ——
Studio 専用の裏口はありません(D0)。

## 13.6 エラーの読み方 — 2 つのチャネル

**ここを取り違えるとクライアント実装が丸ごと壊れます。**

| チャネル | どのメソッド | どこに出るか |
|---|---|---|
| **A: JSON-RPC の `error`** | `scene_tree` / `get_components` / snapshot / `save_scene` / preview のスキーマ違反など | 通常の JSON-RPC エラー(`-32602` / `-32000`)。`data.code` に安定コード |
| **B: `result` の中の `error`** | **`edit` / `undo` / `redo` / `open_preview` / `update_preview` / `commit_preview` / `abort_preview` / `edit_render_features`** | **成功応答の中**。`result.status` が `"rejected"` か `"failed"`、`result.error` が `{code, message, payload}` |

```jsonc
// チャネル B の例(JSON-RPC 的には成功応答)
{"jsonrpc":"2.0","id":7,"result":{
  "ticket":"edit-3","actor_id":1,"status":"rejected",
  "error":{"code":"stale_revision","message":"base SceneRevision is stale",
           "payload":{"current_revision":3}}}}
```

- `rejected` = **受付の時点**で弾かれた / `failed` = **フレーム境界の実行時**に弾かれた、という区別です。
- **ただし `edit_render_features` はこの区別に従いません。**フレーム境界の preflight で落ちた場合も `status` は `"rejected"` です。また **追加情報は `payload` の中ではなく `error` の直下**に置かれます。
- **シーン編集**の安定コードは **43 種**です(正は
  [`EditorEditErrorCode`](../../src/core/communication/editorjournal.hpp#L50) と、その名前を返す
  [`editorEditErrorCodeName()`](../../src/core/communication/editorjournal.cpp#L3108)。**列挙子の名前とワイヤ上の
  コード名は一致しません** —— 新しいものは `@1` 付きです)。内訳:
  - 基本 21 種: `stale_revision` / `gate_closed` / `preview_lease_conflict` / `preview_lease_busy` / `not_lease_owner` / `ticket_not_found` / `undo_conflict` / `not_editable` / `schema_violation` / `component_unknown@1` / `duplicate_component` / `missing_component` / `name_conflict` / `parent_not_found` / `closure_unresolvable` / `cycle_detected` / `zero_scale` / `non_finite_transform` / `trs_unrepresentable` / `preserve_missing` / `method_unavailable`
  - preview: `editor_preview_unsupported@1`(✅WP375c。列挙子は `preview_unsupported`。**inactive なシーンの add / remove / reparent プレビュー**と、`duplicate` のプレビュー(✅OL-1)はこれで拒否されます)
  - prefab: `prefab_instance_id_collision` / `prefab_dependency_mismatch` / `prefab_provider_stale` / `prefab_parameter_required` / `prefab_parameter_unknown` / `prefab_parameter_type` / `unpack_must_be_singleton@1` / `prefab_digest_changed@1` / `prefab_provider_changed@1` / `prefab_behavior_dependency_changed@1`
  - origin 遷移(undo/redo の同一性検査): `origin_transition_*@1` 8 種
  - そのほか: `journal_integrity_error@1` / `behavior_type_unknown@1` / `runtime_binding_missing@1`
- **`not_editable` と `component_unknown@1` の切り分け(✅WP379/379a)**: `add_component` は
  production の component registry に問い合わせます。**登録されているが editor codec が無い**なら
  `not_editable`、**そもそも登録されていない**なら `component_unknown@1{"name": "…"}` です
  ([editorjournal.cpp](../../src/core/communication/editorjournal.cpp#L214))。以前は実 DLL が登録した
  codec 無しコンポーネントも一律「未登録」と報告していました。
- **`edit_render_features` はこの 21 種では尽きない別系統**です。共通するのは `gate_closed` だけで、ほかに `unknown_render_feature` / `restart_required_feature` / `render_feature_unavailable` / `render_feature_not_enabled` / `external_modification` / `stale_source_digest` / `render_pipeline_preflight_failed` / `render_config_rollback_failed` / `render_pipeline_commit_protocol_error` を返します(§13.5)。
  `set_parameters` と `materialize` はさらに `invalid_feature_parameters`(index が範囲外、`parameters` が
  object でない、参照を持たない entry を指した)と `project_preset_required`(engine preset のまま preset 側を編集した)、
  `feature_destination_exists` / `invalid_feature_destination`(materialize の保存先)を足します
  ([set_parameters の検査](../../src/core/communication/renderconfigeditor.cpp#L1568))。
- **グラフ編集の語彙はさらに別系統**です。`graph.` で始まる名前付きの拒否と、形が違うだけの
  `invalid_graph_document` / `invalid_graph_attachment`、それに render config と共通の
  `external_modification` / `stale_source_digest` / `render_config_section_unlocated` です(§13.5)。
- 保存・スナップショット側も別系統(`save_busy` / `runtime_only_data` / `external_modification` / `digest_mismatch` / `snapshot_too_large` など)。

### 編集がそもそもできない状態(gate)

`can_edit` が false を返す理由は 5 つだけです: **リプレイ中 / ゴールデンモード / `--strict-assets` / シーン遷移やリロードの最中 / プレビューリースの衝突**。`--rpc` や `--headless` は理由になりません(それらは編集を妨げません)。

## 13.7 プレビューの梯子(どこまでが「本物」か)

用途に応じて 4 段あります。

| 段 | メソッド | 実行中のシーンを触るか | 何ができるか |
|---|---|---|---|
| ① チケットプレビュー(§13.3 のチケットを開いたまま値を差し替え続け、`commit` で確定 / `abort` で破棄する) | `open/update/commit/abort_preview` | **触る**(実際に反映される) | ドラッグ操作のライブ反映。🚧 **`set_component_value` の `transform` と `light` だけ**。他は `method_unavailable` |
| ② 評価プレビュー | `eval_preview` | 触らない | 変更を仮に当てて**値を問い合わせる**(コンポーネント値・子孫のワールド変換・raycast・overlap・カメラ) |
| ③ 描画プレビュー | `render_preview` | 触らない | 画像を返す。🚧 **中身は模式図**(下記) |
| ④ 別プロセス | `export_scene_snapshot` → 別プロセスで `import_scene_snapshot` | 別プロセス | 完全に隔離した実験 |

- ①のリースは**1 セッションに 1 つ**です。開いている間に他の actor が衝突する編集を通すと、リースは**強制中止**され、`step_frame` の `edit_results[]` に `{"event":"ticket_forced_aborted", "reason":"base_revision_stale", ...}` が流れます。
- ②は実行の前後でエンジンの共有状態を照合し、**1 ビットでも変化していたら `state_changed` で失敗**します(「プレビューは副作用ゼロ」を機械的に守る仕組み)。

> 🚧 **`render_preview` の絵は実際のレンダリングではありません。** 現在の実装は Vulkan を一切通らず、**グリッド背景の上にオブジェクトの位置を円と十字でプロットした模式図**を返します(色でライト・カメラ・collider を区別)。隔離の契約(状態不変の検証・キャプチャのスキーマ・ゲートの二重チェック)は完成しているので、**将来ここを本物の描画に差し替えても API は変わりません**。今の用途は「配置の当たり確認」です。

キャプチャの指定は 6 フィールドすべて必須です:

```json
{"capture":{"width":96,"height":64,"pixel_encoding":"png",
            "camera":{"object_id":4},"graph_generation":3,"max_bytes":200000}}
```

`width` / `height` は 1〜2048、`max_bytes` は最大 16 MiB、`camera` は `{object_id}` か明示的な `{position,target,up,projection}` のどちらか一方です。

なお、プレビュー専用の**第 3 のレンダーグラフ variant**(flat / XR に続く `preview`)が起動時に必ずコンパイルされます。TAA・velocity・history・UI・XR ミラーなど、時間方向や UI・ミラーに依存する feature は除外されます。判定は feature の名前と、宣言した pass・RT の名前と種類によるもので(名前は語単位で照合)、pass の置き場所(`insert`)は `pelican_ui` の前後かどうかだけを見ます。swapchain に書くだけでは除外されません(preview では `preview_capture` への書き込みになります)。除外された feature の pass や RT を使う feature も、一緒に除外されます(✅G-6)。config に直書きした除外できない構成は**起動時にパス名入りで拒否**され、編集の候補を preview が合成できないときは `variant: "preview"` 付きで断られます(§13.5。規則の詳細は[第6章 §6.13](06_rendering.md))。

## 13.8 保存とスナップショット

### save_scene(✅WP166)

```jsonc
→ {"method":"save_scene","params":{}}
← {"status":"saved","scene_revision":5,"digest":{"algorithm":"sha256","hex":"..."},
   "byte_count":1234,"scene_hot_reload":false}
```

> **設計決定(全文書アトミック保存):** 保存は「シーンファイル 1 本を丸ごと・原子的に置き換える」。書き込み前後にディスクのダイジェストを照合し、一時ファイルへ書いて読み返し、パースし直して意味が一致することまで確認してから置換する。途中のどこで失敗しても**元のファイルは無傷**。

- **シーン JSON はホットリロードの対象外**です(`get_status.scene_source` が `{hot_reload:false, save_method:"save_scene"}` と明示します)。外部エディタでシーンファイルを書き換えても実行中には反映されません。逆に、エンジンが保存している間に外部がファイルを書き換えていた場合は `external_modification` で拒否されます。
- ⚠ **落とし穴**: `update_transforms` か `load_gltf` を**一度でも呼んだセッション**では、以後 `save_scene` が永久に `runtime_only_data` で失敗します(それらはオーサリング文書を経由しない「実行時だけの変更」なので、保存すると内容が食い違うため)。復帰するにはシーンを読み込み直します。
- 未完了の編集チケットやプレビューリースがあると `save_busy` です。
- 再生のセッションの間(✅PM-2)は、何もする前に `-32000` の `play_session_active`(`data` は `{code, method: "save_scene", state}`)で断られます。Stop で scene が再生の前に戻るので、先に Stop してから保存します(§13.5「再生のセッション」)。ImGui の Save ボタンは RPC を通らないので断られません。
- 保存ファイルには各 object の `authoring_object_id` と文書直下の `next_authoring_object_id` が入ります。旧文書も最初の保存で ID が記録され、object の削除・並べ替え後に再び開いても graph の装着先が維持されます。非表示のシーンも保存対象です。返される digest と `byte_count` はこの保存ファイルの値です([第4章の ID 規則](04_scene_ecs.md))。

### スナップショット(✅WP168)

`export_scene_snapshot` は文書の意味的なバイト列・ダイジェスト・`prefab_closure` を返し、`import_scene_snapshot` はそれを**検証してから**取り込みます。検証の順序(バージョン → サイズ上限 64 MiB → SHA-256 → パースと意味検証 → `prefab_closure` の照合 → シーンの存在)は契約として固定されています。

`schema_version` は **`2` だけ**です(SnapshotV2)。scene v1 の文書を運んでいた SnapshotV1 は MG-2b で export / import とも撤去され、`schema_version: 1` は他の鍵を見る前に `unsupported_snapshot_version` で断られます([版を断る関数 `rejectSnapshotVersion`](../../src/core/communication/editorcommandservice.cpp#L210))。**どの段階で失敗しても、通常のプロジェクトのシーンファイル・キャッシュ・revision は一切変化しません**。

再生のセッションの間(✅PM-2)は、`import_scene_snapshot` は検証の前に `-32000` の `play_session_active`(`data` は `{code, method: "import_scene_snapshot", state}`)で断られます。`load_scene` も同じ code で断られます(`method: "load_scene"`)。`export_scene_snapshot` は断られません。

snapshot は保存用の object ID を含みません。import は新しい ID を採番する操作で、同じ文書を保存して開き直す操作とは異なります。import 後に graph の装着先を指定するときは、新しい `scene_tree` の `authoring_object_id` を使ってください。snapshot の digest と保存ファイルの digest は、ID メタデータの分だけ異なります。

## 13.9 エンジン内蔵のエディタ UI(ImGui ✅WP159/164/167)

**開発者 UI は既定で表示されています。`F1` は開くキーではなくトグルで、
起動直後に押すと消えます。**メニュー項目の一覧と、UI が無効になる条件は
[第10章](10_tools.md) §10.6 が正本です。

この章で関係するのは 2 つだけです。

| ウィンドウ | 内容 |
|---|---|
| **Object Tree** | シーンのオブジェクト階層。選択すると Inspector に出る |
| **Inspector** | **編集可能**。コンポーネントの値をウィジェットで編集、behavior の attach / remove / params 編集、undo/redo、保存 |

> **設計決定(インスペクタはスキーマ駆動):** インスペクタは型ごとに専用 UI を手書きしない。`get_components` が返す**フィールドのスキーマ(型・範囲・単位・enum 候補)からウィジェットを組み立てる**。したがって behavior に新しいパラメータを足すと、**エンジンを 1 行も変えずにインスペクタへ現れます**。

- ドラッグ操作は**チケットプレビュー**(§13.7 ①)として反映され、離した時点でコミットされます。したがって現状ドラッグできるのは transform と light の値です。
- 編集は RPC の `edit` と**同じ関数**を呼びます(D0)。したがって GUI で行った操作も履歴(journal)に載り、undo できます。
- ⚠ 前述のとおり、**`--rpc` 付きで起動すると ImGui は無効**になります。

## 13.10 Python クライアント(✅WP160)

薄い JSON-RPC クライアントが [tools/pelican_rpc.py](../../tools/pelican_rpc.py) にあります。エージェントやスクリプトからの操作はこれが最短です。

```python
from tools.pelican_rpc import PelicanRpc   # (例)

with PelicanRpc("mygame") as rpc:   # 第 1 引数は project の dir。既定は --headless --rpc(窓ありは headless=False)
    session = rpc.call("open_editor_session", {"display_name": "script"})
    rev = rpc.call("get_scene_revision")["scene_revision"]
    ticket = rpc.call("edit", {
        "actor_id": session["actor_id"],
        "base_revision": rev,
        "operations": [{"op": "set_component_value", "object_id": 1,
                        "component_slot": "light", "field_path": "/intensity", "value": 2.0}],
    })["ticket"]
    rpc.call("step_frame")                      # ヘッドレスではここでコミットされる
    print(rpc.call("get_edit_result", {"ticket": ticket}))
```

実際に動く E2E の例は `test/pelican_rpc_smoke.py`(スナップショットの往復)にあります。

## 13.11 落とし穴のまとめ

1. **`--rpc` と ImGui は排他** — GUI と外部エージェントを同一プロセスで併用できません。
2. **ウィンドウモードの `step_frame` は `step_frame_windowed` で断られる**(✅PM-1a) — 結果は `get_edit_result` をポーリングして取ります。game を 1 歩ずつ進めるのは `set_play_state` で止めてからの `advance_frames` です。
3. **`stale_revision` は無関係な編集でも起きる** — revision だけ差し替えて再送せず、最新を取り直してやり直します。
4. **チケットプレビューは transform / light の値変更だけ** — 他の操作は `method_unavailable` です。
5. **`update_transforms` / `load_gltf` を使ったら、そのセッションでは保存できない**(`runtime_only_data`)。
6. **保存の JSON はキー順が正規化される** — 差分レビューのときに順序変更が出ます。
7. **編集の失敗は JSON-RPC エラーではなく `result.status`** に出ます(§13.6)。
8. **数値は 2^53−1 まで**。

## 13.12 実装状況

凡例: ✅実装済み / 🚧実装中(部分的に使える)/ 📐設計のみ(未実装)。付いている `WPnnn` は実装した作業単位の番号です([第11章 §11.1](11_status.md))。
2026-09 の作業単位は通し番号ではなく記号 ID(`N-1` / `P-1` など)なので、**記号とコミットの両方**を書きます。

| 機能 | 状態 |
|---|---|
| オーサリング文書・オブジェクト ID・revision | ✅WP149 |
| コンポーネント codec(受理・正規化・スキーマ公開) | ✅WP150/151 |
| 投影トランザクション(失敗時完全巻き戻し) | ✅WP152/158 |
| 読み取り RPC + スナップショット出力 | ✅WP154 |
| ウィンドウモード RPC ホスト | ✅WP156 |
| 編集 RPC + 履歴(journal) | ✅WP157 |
| undo / redo + チケットプレビュー | ✅WP161 |
| Asset Browser / Inspector(編集可) | ✅WP159/164/167 |
| 保存(アトミック全文書) | ✅WP166 |
| スナップショット取り込み | ✅WP168 |
| 変更検出トークン | ✅WP170 |
| 隔離プレビュー(評価) | ✅WP172 |
| 隔離プレビュー(描画) | 🚧WP172(契約は完成・**絵は模式図**) |
| チケットプレビューの対象拡大 | 🚧(transform / light のみ) |
| GUI と RPC の同居 | 📐未実装 |
| Qt Studio shell / native engine viewport | ✅WP249/251 |
| Qt Studio からの編集 RPC 利用(D2) | ✅WP266/275。Inspector と gizmo が同じ preview lease / undo / save を利用 |
| ID バッファピッキングの engine/RPC 基盤 | ✅WP262 |
| Qt Studio の viewport / Outliner 選択同期 | ✅WP264 |
| Qt Studio の Outliner の複製・削除・drag での付け替え(選択は authoring id で保つ) | ✅OL-1 `546a4e70` |
| 汎用 gizmo feature/RPC と Qt Studio のドラッグ操作 | ✅WP274/275 |
| 解決失敗を値で返す(`resolution_error`) | ✅WP378 |
| `not_editable` / `component_unknown@1` の切り分け | ✅WP379/379a |
| scene load の commit 後 effect 失敗を result で返す | ✅WP380b/380e |
| game DLL の startup / ロード前診断を status に出す | ✅WP381/381a |
| 可変長宣言(`List` / `$list`)の**行編集** | ✅RPCとQt Inspectorの追加・削除・移動・行内field編集 |
| `graph.instance` の装着と入力の供給元(Inspector の 4 導線) | ✅P-1 `3611e390` |
| feature パラメータの編集と materialize(`set_parameters` / `materialize`) | ✅P-2a-1c `e3a2d3d0` |
| authoring 面が graph root の `rendering` 節を読む / 共有部品の export・import | ✅P-2a-4a + P-2a-3 `7e983ed9` |
| package の install / uninstall(RPC と `pelican_cli package`) | ✅X-1 `64eae566`。**Studio に package の UI はありません**(カタログ経由で見えるだけ) |
| `get_status.animation` と `get_graph_node_values` | ✅AG-3a `32d9a20b` |
| 一時停止と 1 歩ずつ(`set_play_state` の `playing` / `paused`、`advance_frames`、`get_status.play`、窓ありの `step_frame` の拒否) | ✅PM-1a `c654e65c` |
| `edit` 状態(`--start-state edit`、眠った behavior、`set_play_state` の `edit`、`set_view_camera`、`get_status.play.view_camera`) | ✅PM-1b `17938ee3`。Stop で authoring を戻すのは ✅PM-2、Studio の Play / Stop は ✅PM-3b `9c169f61` |
| 再生のセッション(Stop で authoring・今の scene・editor の履歴を戻す、`keep`、`get_status.play` の `session` / `recorded_edits`、Play のたびの DLL の読み直し、`play_busy` / `play_session_active` / `play_keep_unknown_transaction`、`edit` 起動の input の記録と再生の拒否、Stop で音を全部止める) | ✅PM-2 `04666d59`(§13.5「再生のセッション」)。Studio の Play / Stop と keep のボタンは ✅PM-3b `9c169f61` |
| Studio の Pause / Step / Resume と再生の状態の表示(status bar の行、paused の枠と帯の色、`playing` の間は Timeline の scrub を止める) | ✅PM-3a `1eaf28a6`(§13.1「Studio で game を再生・停止する」) |
| Studio を edit で起動し、Play / Stop・edit からの Step・Play changes と Keep・Take from game・戻らない編集の帯・再生中の Save の無効・View: Game \| Editor・Reload game logic・再起動などの前の確認・playing の枠と帯の色 | ✅PM-3b `9c169f61`(§13.1「Studio で game を再生・停止する」) |
| 再生中に spawn された object を Outliner に `(runtime)` の行で出し、Inspector で読むだけの Transform を見せる | ✅PM-3c `e283aa42`(§13.1「Studio で game を再生・停止する」) |
| Studio の render-tools dock に Graph タブ(読む) | ✅N-1 `edafc41d` |
| ノードの見た目(見出し + 型付きソケット。形は JSON で足す) | ✅N-1b `6c47a8a4` |
| ノードキャンバスの編集(接続 / 切断 / 値 / 追加 / 削除 / ラベル / definition の中) | ✅N-2 `f2eccbfd` |
| Studio の自動化面(`--automation` + シナリオ + report。**実物の窓と engine を通す e2e**) | ✅Q-1 `c026002d` |
| Frame Plan のノードの見た目を共通グラフと揃える / 2 つのキャンバス共通のズーム・パン・fit | ✅N-3 `d8d7387d` |
| Studio のテーマ(View ▸ Theme のライト / ダーク。両キャンバスが追随、QSettings `studio/theme` に保存) | ✅N-5 `6a26eef3` |
| 時間軸 view・definition scope の値表示 | ✅AG-3b(`ce5e96c0`。出力 port の帯、Live、Timeline 帯と scrub。シナリオ `graph_observe_roundtrip`) |
| グラフ編集の undo / redo | ✅Graph タブのセッション内100操作。digest照合付き逆編集。scene journalとは独立 |

Qt Inspectorは再帰schemaのListを行IDと値の一覧で表示します。`Add`で行の値をJSON入力し、`Edit`で選択行の値を変更、`Remove`で削除、`Up`/`Down`で順序を変えます。`$row`や`next_id`を入力する必要はありません。移動は同じ行IDを保持し、通常のUndo/Redoと保存を使用します。省略されたListにも行を追加できます。generated・pending・read-onlyのcomponentとList内部のネストしたListは編集対象外です。

`Ray geometry...`は選択objectのgeometry候補、実効値、overrideのcomponent/行と適用状態、拒否理由を表示します。候補と追加先のray_instanceを選び、`Add override`で行を作成し、InspectorのListで値を編集します。編集やmodel reloadの後は`Refresh`で候補を再取得してください。

geometryを選択すると、mask・instance flags・custom index・geometry flagsの実効値、適用元、優先順を項目別に表示します。
最初の3項目は既定値→component→occurrence overrideで、未指定fieldは継承し、明示的な0は上書きです。
geometry flagsは既定値とtriangleのgeometry rule、proceduralではその既定値から決まります。layout v1(custom indexがrow qだった版)はMG-2aから読めず、`pelican.ray_group_layout.version_unsupported@1`で断られます(Studioの説明文には「Layout v1 uses row q as custom index.」がまだ残っています)。
rule番号とoverride行IDも既存scene-planの出所情報から表示します。GPU値の読み戻しや解決規則の変更ではありません。

`Related RT tasks...`は選択geometryのhit groupを使うtaskを、読み込み済みFrame Planから探します。
候補が1つならTask Editorへ進み、複数なら選択できます。Frame Plan未取得時は取得後に探します。
これはgroup名を手掛かりにした移動で、scene-planの値とGPUの動的値を合成する操作ではありません。

`Resolved binding`には確定済みscene rowのhit group index、material参照と名前、material/geometryテーブルindex、geometry flags、recordデータのバイト数を表示します。scene確定時点でテーブルindexが未割当の場合は`null`です。GPU用rowで後から割り当てられる番号や個別taskの物理SBTアドレス、record内の各field値はこの表示には含まれません。

RPCの行移動は `edit.operations` の `move_component_field` を使います。`address`は他の行編集と共通で`row_id`を指定し、`position`は`{"before":行ID}`または`{"end":true}`です。`leaf_path`は指定しません。行の値・ID・採番位置は変わりません。

## 13.13 シーン遷移とゲームロジックの結果(✅WP378/380b/380e/381/381a)

RPC クライアントから見て、2026-09-03〜04 に**応答の形が変わった**ところです。

### `load_scene` / `import_scene_snapshot` の `committed` と `effect_errors`

scene のロードは 1 つのトランザクションになりました([第4章](04_scene_ecs.md) §4.3)。
**commit よりも前に失敗したもの**と、**commit は済んだが behavior の副作用の flush に失敗したもの**は
別扱いです。後者は**エラーではなく成功応答の中身**として返ります。

```jsonc
// (例)load_scene の成功応答 —— 効果の失敗が 1 件あった場合。鍵の並びは実装どおり
{"jsonrpc":"2.0","id":4,"result":{
  "name":"main","committed":true,
  "effect_errors":[{"api":"saveData","error_code":"persistence_write_failed@1",
                    "error_fields":{"path":"user://saves/auto.json","reason":"…"},
                    "phase":"flush"}]}}
```

- `committed` は**成功応答では常に `true`** です(commit 前に失敗したら JSON-RPC の `error` になります)。
  「シーンは切り替わったが、`onInit` が呼んだ `playSound` / `saveData` / `setBusVolume` の一部が失敗した」
  ことを表すのが `effect_errors` で、**空配列なら全部成功**です。
- 1 要素は `{api, error_code, error_fields, phase}` の 4 鍵ちょうどです(正は
  [`effectErrorsJson()`](../../src/core/loader/scene.cpp#L512))。`api` は
  `playSound` / `saveData` / `loadScene` などの**公開 API 名**、`phase` は
  `activate` / `destroy` / `flush` の 3 値、`error_fields` はコードごとに決まった固定の鍵
  (`{path, reason}` / `{slot, reason}` / `{bus, reason}` / `{dropped}` など。正は
  [effecttransaction.cpp](../../src/core/loader/effecttransaction.cpp#L708))です。
- `import_scene_snapshot` の応答にも同じ `committed` / `effect_errors` が付きます
  ([editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp#L1846))。
  **`{"status":"imported","scene_revision":N,"current_scene_id":"…"}` だけを期待する
  厳密一致のクライアントは壊れます。**
- 代表的な `error_code`: `effect_outbox_full@1{dropped}`(1 回の scene load で 256 件を超えた分)/
  `persistence_read_failed@1{path, reason}`(壊れた save slot。以前は「slot 不在」に化けていました)。

### `get_status.participants.game_logic` の `error_code` / `error_fields`

`get_status` に `participants.game_logic` が入りました。**まだ 1 度も reload を試していない
起動直後でも、startup の初期化失敗がここに出ます**([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1299))。

- 試行済みなら `reload.runtime.game_logic` の内容(`configured` / `loaded` / `reloading` /
  `pending` / `source_busy_retries` / `generation` / `systems` / `source` / `loaded_copy` /
  `domain_error` / `error_code` / `error_fields`)がそのまま入ります
  ([reloadservice.cpp](../../src/core/watch/reloadservice.cpp#L769))。
- 未試行なら `{error_code, error_fields}` の 2 鍵だけで、値は起動時に保存された失敗です。
  成功すると `error_code` は `null` に戻ります(clear される)。
- `reload_game_logic` の失敗も同じ語彙で、`error.data` が `{error_code, error_fields}` です。

### ロード前に弾かれる game DLL(`game_logic_abi_stale@1` ほか)

ABI の照合が `LoadLibrary` の**前**に移りました(✅WP381)。DLL の PE import directory を
直接読んで、`pelican_game_logic_epoch_v2()` anchor と v2 入口が揃っているかを見ます
([gamelogicimports.hpp](../../src/core/gamelogic/gamelogicimports.hpp#L64))。したがって
**古い DLL の static initializer は 1 行も走りません**。エラーコードは
`game_logic_abi_stale@1{expected_epoch, found, legacy_symbols[]}` /
`game_logic_abi_unsupported_import@1{kind}` / `game_logic_abi_unreadable@1{reason}` /
`game_logic_load_failed@1{os_code}` です。

**ビルド中の DLL は「失敗」になりません(✅WP381a)。** ファイルが writer に掴まれている間は
`game_logic_source_busy@1{path, retries}` が **pending status** として出て、解除後の poll で
自動的に再試行されます。この間 active な DLL は差し替わりません。

## 関連文書

- [../design_editor_tooling.md](../design_editor_tooling.md) — エディタツール設計(v2.5・条件付き受理。レビュー条件 E-C1〜E-C5 と V22-C1〜C7 が本文に優先します)
- [../design_object_behaviors.md](../design_object_behaviors.md) — behavior 設計([第8章](08_gameplay.md) §8.5)
- [../design_devstudio_direction.md](../design_devstudio_direction.md) — D0(エディタ特権の禁止)と Qt Studio の段階
- [../design_graph_composition.md](../design_graph_composition.md) — 共通グラフ(`pelican.graph`)の合成と、instance の入力・装着編集
- [第4章 シーンと ECS](04_scene_ecs.md) / [第8章 ゲームロジック](08_gameplay.md) / [第10章 ツールリファレンス](10_tools.md) / [第11章 実装状況](11_status.md)
