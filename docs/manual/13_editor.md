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
  ([接続](../../src/devstudio/view/mainwindow.cpp#L1979))。シーンに未保存の編集があれば上の
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
gizmo の session を終え、返答待ちの要求を忘れます([切替の前処理](../../src/devstudio/view/mainwindow.cpp#L2930))。
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
([使えない component の文の表](../../src/devstudio/model/inspectormodel.cpp#L716))、今ある code は `animation` / `audio` / `physics` /
`xr` / `vat` / `seqplayer` の `.module_unavailable@1`、`pelican.ray_instance_requires_layout_v2@1`、`audio.format_unsupported@1` の 8 つです。表に無い code は
code のまま出ます。

- **音声の format の拒否も表に入りました(✅Q-10)。**`audio.format_unsupported@1`(AU-2。音声は WAV と Ogg Vorbis で、拡張子と先頭の
  bytes が食い違うとこの code)の文は `Sound extension and contents disagree (WAV or Ogg Vorbis)`、短い語は `— unsupported sound` です。
  ただし着地時の記録では、**今この code が Inspector や Add component に届く経路はありません**(音声を取る component が無く、
  engine の asset の一覧に音声の種別が無い)。今この文が出るのは **Graph タブの notice** で、graph の編集の preflight の拒否
  (`render_pipeline_preflight_failed`)が message の頭にこの code を持っているときです。表の code を、拒否の code か message の中に
  持っていれば表の文にします([`knownRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L871)。Graph タブは拒否を notice に
  する [`diagnostic()`](../../src/devstudio/model/graphcanvasmodel.cpp#L24) でこれを通します)。

- **理由が code でない行は止めません。**`available: false` でも `unavailable_reason` が文(`simplemodelview` の
  `A model reference is required.` など。既定値だけでは足りず入力が要る、の意味)なら、その行は普通に並び、宣言に値を入れれば
  Add できます。code かどうかは「小文字・数字・`_` の `.` 区切りの名前 + `@` + 数字」の形で見分けます
  ([`isComponentUnavailableCode()`](../../src/devstudio/model/inspectormodel.cpp#L741))。
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
([Add Object の menu を作る所](../../src/devstudio/view/mainwindow.cpp#L2380))。項目は **Empty** / **Cube (built-in)** /
**Plane (built-in)** / **Sphere (built-in)** / **Floor** で、自動化面の action 名は `pelican.outliner.addObject.empty` / `cube` /
`plane` / `sphere` / `floor` です。

- **Floor(✅T-1)** は床として置いた Plane です。Plane の基本形を原点の少し下(`pos` が (0, -0.5, 0))に、広く
  (`scale` が (8, 1, 8))置きます([`addObjectDeclaration()`](../../src/devstudio/model/inspectormodel.cpp#L469))。名前は `Floor`、
  `Floor_001`… です。T-1 より前は Plane を置いてから Inspector で位置と大きさを打っていました。

- **送るもの**: scene の `edit` 1 回に `spawn` op 1 つです(上の `addObjectDeclaration()`)。
  transform は原点(Floor だけは上のとおり)で、形なら `simplemodelview.model` に `engine://primitives/<kind>`(Floor は `plane`)、Empty は transform だけです。
  置き先の scene は選択中の object の scene(無ければ engine が最初に言った scene)で、選択中の object の子にはなりません。
- **名前**: 種類の名前(`Empty` / `Cube` / `Plane` / `Sphere` / `Floor`)が空いていればそれ、使われていれば `Cube_001`、`Cube_002`…
  の最小の空き番号です([`uniqueObjectName()`](../../src/devstudio/model/inspectormodel.cpp#L454))。Blender は `Cube.001` と
  書きますが、scene の object 名は `[a-zA-Z0-9_]` だけなので `_` にしています。
- **選択**: Outliner の読み直しを最短の間隔に縮め、committed の応答の `created_objects` が名乗る object が Outliner に
  出た時点でそれを選び、Inspector に出します([`MainWindow::selectCreatedObject()`](../../src/devstudio/view/mainwindow.cpp#L2490))。
  ✅OL-1 から authoring id で選びます(それまでは名前で選び直していました)。
- **Undo**: coalesce key を付けないので、Undo 1 回で Add 1 回が戻ります。
- **Inspector の編集の途中で押した Add Object は待ちます(✅Q-7)**: Inspector の値の編集が進行中(commit の途中を含む)に
  押すと、spawn は 1 本だけ待ち、Inspector の手が空いた時点で、その編集が残した scene revision の上に送られます
  ([`InspectorModel::spawnObject()`](../../src/devstudio/model/inspectormodel.cpp#L4042))。待っている間は status bar(8 秒)と
  Outliner の下の 1 行(`pelican.outlinerNotice`)に `Adding <名前> after the current edit.` が出ます
  ([Add Object の結果を status bar と Outliner に出す所](../../src/devstudio/view/mainwindow.cpp#L2466))。既に 1 本待っているときに
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
(`pelican.inspectorModelChoice`)です([model 欄の選択肢の並び](../../src/devstudio/model/inspectormodel.cpp#L678))。
並びは基本形 3 つ(`Cube (built-in)` などの名前で)→ 区切り → `list_assets` が言う project の model(engine の基本形は
二重に出しません)→ 今の値がそのどれでもなければその値、です。選ぶとすぐ commit します(enum の combo と同じ)。
Add component... の asset 一覧も同じ読み方で、基本形は `Cube (built-in)` の名前で先頭に並び、区切りの下に project の
asset が `id [status]` で並びます。

**Inspector の数値欄の名前(✅G-5)**: 自動化面から届くように、数値の欄は 1 つずつ名前を持ちます。vector の列は
`pelican.inspectorVector.<component>.<field>.<列>`(例: `pelican.inspectorVector.transform.scale.2`)、数 1 つは
`pelican.inspectorNumeric.<component>.<field>` です([数値欄の名前の付け方](../../src/devstudio/view/inspectorwidget.cpp#L2173))。
G-5 より前は、どの vector も最初の列が同じ名前でした。シナリオは欄が有効になるのを待って打ち、Return で commit し、
欄がその値を持つまで待ちます([シナリオの README](../../test/studio_scenarios/README.md#L567))。

**回転は角度で入れます(✅T-1)。**`transform.rotation` は X / Y / Z の 3 欄(度、小数 2 桁、自動化面の名前は
`pelican.inspectorEuler.transform.rotation.0` 〜 `.2`)で見せて編集します([回転の欄を作る所](../../src/devstudio/view/inspectorwidget.cpp#L2235))。
回転は X、Y、Z の順に固定軸の回りにかかります(Rz·Ry·Rx)。保存されるのは今までどおり quaternion `[x, y, z, w]` で、
変換は Studio がします([角度と quaternion の変換](../../src/devstudio/model/inspectormodel.cpp#L501))。読むときは X と Z が
(-180, 180]、Y が [-90, 90] で、Y が ±90 のときは X を 0 にして残りを Z に寄せます。欄の右の **Quaternion** の toggle を開くと、
quaternion の 4 欄(G-5 からの `pelican.inspectorVector.transform.rotation.<列>`)が出て、そちらでも編集できます。開閉は欄を
作り直しても保たれます。viewport の gizmo の回転は今までどおりです。

**表示の桁で同じなら、変えていません(✅T-1)。**数値欄は、保存された値を欄の桁で丸めたものと表示が同じなら「値は変わって
いない」と数えます([表示の桁で比べる所](../../src/devstudio/view/inspectorwidget.cpp#L809))。T-1 より前は、focus を置いて離れただけで
丸めた値(`0.38268343` の代わりに `0.3827`)が commit されていました。角度の欄も、同じ回転を表していれば送りません。

**光の向きは方位角と仰角で、camera の yaw と同じ基準です(✅UX-10 / ✅UX-14)。**component の vec3 の `direction`(`light.direction`)は、
方位角(`pelican.inspectorDirection.light.direction.azimuth`。(-180, 180] で回り込む)と仰角(`.elevation`。[-90, 90])の 2 欄(度、小数 2 桁)で
見せ、**Raw**(`pelican.inspectorDirectionRaw.light.direction`)を押すと保存されている x / y / z の欄が出ます
([方位角と仰角の欄を作る所](../../src/devstudio/view/inspectorwidget.cpp#L2298))。仰角は水平から上が正(負は下向き)、真上・真下の方位角は 0、
角度から作るベクトルは長さ 1 で、保存されるのは今までどおりベクトルです。camera の `controller.yaw` / `controller.pitch` は保存はラジアン、欄は度です。

- **基準**(✅UX-14、`aaba15a6`): 方位角は +Y の回りに、+Z を 0 として −X の側へ増えます([方位角を読む `directionAnglesDegrees()`](../../src/devstudio/model/inspectormodel.cpp#L615))。
  engine の ✅CC-1 の camera.controller の `yaw` / `pitch` と同じ式で、同じ角度は同じベクトルになります(UX-14 の test が engine の式と突き合わせます)。
  光ではそのベクトルが光の進む向き、orbit の camera では target から camera へ向かう向きです([第4章](04_scene_ecs.md) の camera.controller)。
  UX-10 の着地時から UX-14 の前までは、+Z から **+X** の側へ増えていました(同じベクトルの方位角は符号が逆)。文書は変わりません。
- **tooltip**: 方位角の欄は `Azimuth: the turn about +Y from +Z towards -X, in degrees (as a camera's yaw)`、仰角の欄は
  `Elevation: degrees above the horizon (negative points down)`、camera の yaw の欄は `Yaw: the turn about +Y from +Z towards -X, in degrees (a light's azimuth turns the same way)`、
  pitch の欄は `Pitch: degrees from the horizon towards +Y (negative looks down)` です([yaw / pitch の文 `inspectorAngleTip()`](../../src/devstudio/model/inspectormodel.cpp#L597)、
  [方位角 / 仰角の文 `inspectorDirectionAngleTip()`](../../src/devstudio/model/inspectormodel.cpp#L605))。
- 例(シナリオ `gui_settings_choose`): Blank の雛形の KeyLight の向き `(-0.5, -1, -0.25)`(engine の CC-1c `b0b2fc77` の後。−X の側へ、camera から離れる向き)は
  方位角 116.57 と読め、−116.57 を打つと x だけが反転して z は camera から離れる向きのまま、影が箱の左から右へ移ります(下の「Studio の自動化面」)。
- test: `devstudio_inspector_test` の [「UX-14 decision 3: a light's azimuth turns as a camera.controller's yaw does -- 0 is +Z, growing towards -X -- and the yaw, pitch and azimuth fields say so」](../../test/devstudio_inspector_test.cpp#L1900)・
  [「UX-10 a light's direction reads as azimuth and elevation, its colour as a swatch, and a sprite's texture is picked from the textures list_assets reports」](../../test/devstudio_inspector_test.cpp#L1939)(UX-14 で 90 度の期待を +X から −X に替えました)。

**Inspector の欄の編集と commit(✅Q-7)**

- **focus だけでは編集になりません。**編集は、値を打つ(値が変わる)か欄の上で mouse の button を押したときに始まります
  ([focus で編集を始めなくなった所](../../src/devstudio/view/inspectorwidget.cpp#L2578))。欄に focus を置いたままでも
  **Refresh** は押せ、他の欄も止まりません。vector の中で列を移るあいだは 1 つの編集のままです。
- **busy の間に打った値は、手が空いてから送られます。**別の編集の commit の途中(Inspector が busy)は他の欄が止まりますが、
  focus のある欄は止まりません([busy でも focus のある欄は止めない所](../../src/devstudio/view/inspectorwidget.cpp#L1591))。
  そこへ打った値は欄に残り、欄を離れるか Return を押すと、Inspector の手が空いてから 1 回の編集として送られます
  ([手が空いてから値を送る所](../../src/devstudio/view/inspectorwidget.cpp#L760))。その間に欄が消えた(別の object を選んだなど)か、
  値が既に同じなら何も送りません。
- **commit の往復で欄は作り直さず、値だけ差し替えます**([値だけを差し替える所](../../src/devstudio/view/inspectorwidget.cpp#L948))。
  編集中の欄・送り待ちの値を持つ欄・打ちかけの欄(`1.` のように、表示がまだ値と一致しない欄)は上書きしません。作り直すのは
  描くものが変わったとき(別の object、component の追加・削除、欄の種類や範囲が変わるなど)だけです
  ([作り直すかどうかを決める所](../../src/devstudio/view/inspectorwidget.cpp#L890))。
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
  ([Duplicate を送る所](../../src/devstudio/view/mainwindow.cpp#L2543))。
- **Delete(Del)**: 選択中の object を消します。子孫も一緒に消えます。**子孫があるときだけ** 1 回問います
  (`Delete <名前>? The N objects under it are deleted too.`、既定の button は Cancel。自動化面の名前は
  `pelican.outliner.deleteConfirm` と `.delete` / `.cancel`)。子孫が無ければ問わずに消します(Undo で戻せるので)。
  消した後は親を、親が無ければ前に宣言された兄弟を選びます([Delete の確認](../../src/devstudio/view/mainwindow.cpp#L2557))。
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
  同じ object が選ばれたまま・開いた行は開いたままです。選択は Outliner が scene の一覧を読み直すたびに(✅UX-13c から `scene_outline`。下の「Studio の Outliner と Inspector が読む一覧」)id で引き直し、その object が
  消えたときだけ外れます([id で選択を引き直す所](../../src/devstudio/model/selection.cpp#L71))。保存した scene を engine が
  読み直したとき(engine の再起動)も、id が同じなので選択は残ります。**別の project を開くと選択は外れます**。
  Undo / Redo で object が戻ってきたとき(Delete の Undo など)は、その応答の `created_objects` の object を選びます
  ([committed の後に選ぶ所](../../src/devstudio/view/mainwindow.cpp#L2652))。
- **断られたとき**は、status bar(8 秒)と Outliner の下の 1 行(`pelican.outlinerNotice`)に 1 文で出ます
  ([断りの文](../../src/devstudio/model/inspectormodel.cpp#L1944))。例: engine が `cycle_detected` で断った付け替えは `Cannot move <名前> under <親>: <親> is inside <名前>.`、
  preview 中は `Cannot duplicate <名前> while a live preview is open; finish the preview first.`、physics の無い build で
  collider を含む object の複製は Q-6 と同じ文で `Cannot duplicate <名前>: This build has no physics module (physics.module_unavailable@1).`、
  もう無い object は `<名前> is no longer in the scene.` です。
- **Inspector の編集が進行中(commit の途中を含む)のときは断ります。**待たせずに
  `Finish the edit in progress first, then <操作> again.` を同じ 2 か所に出します
  ([進行中なら断る所](../../src/devstudio/model/inspectormodel.cpp#L4074))。進行中の編集が同じ object を変えているかも
  しれないためで、1 本だけ待たせる Add Object(✅Q-7)とは扱いが違います。
- rename・複数選択の一括操作・scene をまたぐ移動はありません。

### Studio の Outliner と Inspector が読む一覧(✅UX-13c)

Outliner と Inspector は、scene の object の一覧を `scene_outline`(§13.5「一覧だけを読む `scene_outline`」)で読みます。
UX-13c(`4405641a`)の前は `scene_tree` で、Outliner は revision が変わっていなくても poll のたびに全 object の component 込みの木を
受け取っていました。Studio がそこから読むのは scene の id と revision、各 object の id・宣言の位置・名前・親だけです(仕様の背景)。

- **Outliner の poll**([`MainWindow::requestOutlinerRefresh()`](../../src/devstudio/view/mainwindow.cpp#L2710)): Studio がまだ scene を
  持っていない最初の 1 回は `scene_outline {}` です。scene を 1 つ持った後は `{scene_id, if_scene_revision: <持っている revision>}` を送り、
  engine が `not_modified: true` で答えれば何も作り直しません。revision が動いていれば一覧全体が来て、行を作り直します。
  持っている scene が 1 つでないときは条件を付けません(コードの読み)。
- poll の間隔は今までどおりで、250 ms から始まり、変わらない答えのたびに倍、最大 2,000 ms、変わった答えで 250 ms に戻ります
  ([間隔の定数](../../src/devstudio/view/mainwindow.cpp#L100)、[答えを読む `completeOutlinerRpc()`](../../src/devstudio/view/mainwindow.cpp#L2739))。
- poll が失敗したとき・答えが形として読めないとき(status bar に `Outliner scene_outline failed: …` / `Invalid scene_outline response: …`)は、
  次の 1 回を条件無しで問います([失敗の `failOutlinerRpc()`](../../src/devstudio/view/mainwindow.cpp#L2784))。持っていない revision への
  `not_modified` も読めない答えとして扱います。
- **親は id で引きます**: 親は `parent_authoring_object_id` で探すので、親が子より後ろで宣言されていても木になります。名前の無い object は
  今までどおり Studio が作る表示名で出し、付け替えも今までどおり断ります。宣言の位置が隙間なく並ぶこと・id が 0 でなく重複も無いこと・
  親の id が一覧にあることを検査し、外れた答えは使わず、持っている scene を残します([`ProjectOutlinerModel::updateScene()`](../../src/devstudio/model/project.cpp#L229))。
- **Inspector**: 選んだ object の id を確かめる問いが `scene_outline {scene_id}` になりました(条件無し)。詳細は今までどおり `get_components` です
  ([問いを積む所](../../src/devstudio/model/inspectormodel.cpp#L2410))。
- **古い engine**: `scene_outline` が `-32601`(method not found)で断られると、その engine の間は `scene_tree` で読みます(親は名前で引く
  今までの読み方)。Outliner と Inspector がそれぞれ 1 回ずつ問って覚え、engine に繋ぎ直すと問い直します
  ([Outliner が覚える所](../../src/devstudio/view/mainwindow.cpp#L945)、[Inspector が覚える所](../../src/devstudio/model/inspectormodel.cpp#L3409))。
- engine に繋ぎ直しても Studio は持っている scene と revision を捨てないので、繋ぎ直した後の最初の poll も(scene が 1 つなら)条件付きです
  (コードの読み)。同じ revision の答えで作り直さないのは、UX-13c の前の `scene_tree` の読みでも同じでした。
- **重さ**: commit の記録では、1,000 object の test(Studio の test の fake の engine)で一覧全体の答えが約 104 KB、変わらない poll の答えが
  58 byte です。engine の側の時間は上の §13.5 の節です。シナリオ(`outliner_edit_roundtrip` など)は `scene_tree` を直接呼んで親の名前を見るので、
  変えていません。
- test: `devstudio_outliner_test` の [「UX-13c the Outliner hangs an object under its parent's authoring id」](../../test/devstudio_outliner_test.cpp#L719)・
  [「UX-13c a not_modified poll rebuilds nothing and a new revision rebuilds」](../../test/devstudio_outliner_test.cpp#L757)、
  `devstudio_outliner_edit_test` の [「UX-13c the Outliner polls scene_outline with the revision it holds」](../../test/devstudio_outliner_edit_test.cpp#L643)・
  [「UX-13c an engine without scene_outline is read with scene_tree, asked once」](../../test/devstudio_outliner_edit_test.cpp#L685)。

### Studio で旧版の project を開く(✅MG-1)

前の版の engine が書いた文書を持つ project を開くと、Studio は viewport の代わりに**移行の画面**
(`pelican.projectMigration`)を出し、計画を見せてから適用します。移行の仕組み(文書の `(schema, version)`、plan / apply、
退避先、CLI)は [第3章 §3.13](03_project_format.md#313-前の版の-project-を移すmg-1) を見てください。v0 の時点では production に
移行の手順が 1 本も無いので、この画面が出るのは手順が登録されてからです。

- **Studio が気づく経路は 2 つです。**1 つは Studio 自身が project を読むとき(offline open)で、parser が文書を読む前に
  player と同じ検出を走らせ、移行が要る文書があれば読まずに止めます([offline open の検出](../../src/devstudio/model/project.cpp#L110))。
  検出は読むだけで、落ちたプロセスが残した transaction の復旧はしません(復旧は書くので player の側です)。もう 1 つは
  player が stderr に書く `project.migration_required@1: {...}` の行です。移行が要る project では player は engine を起動せず
  窓も出さないので、Studio はこの行を見た時点で窓の探索をやめ、同じ画面に回ります
  ([gate で止まった player を見分ける所](../../src/devstudio/viewport/embeddedviewport.cpp#L380))。どちらのときも status bar に
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
  通るので、自動化面の `wait_engine` は新しい engine を待ちます(✅Q-9。[移行の後に engine を起動し直す所](../../src/devstudio/view/mainwindow.cpp#L2231))。
  残った 2 行は別の project を開くと消えます。
- **plan と apply は、engine を起動していない player にだけ送ります**(MG-1b)。apply は最初に transaction の復旧を走らせ、
  別のプロセスが commit している途中の編集を巻き戻しうるためです。同じ project で engine が走っていれば先に止め、起動し直した
  player が移行の入口で止まってから計画を問います([移行の RPC を gate の player にだけ送る所](../../src/devstudio/viewport/embeddedviewport.cpp#L642))。
  Studio の検出が「移行が要る」と言うのに player が普通に開いたとき(Studio と player の build が食い違っている)は、再起動を
  繰り返さずに `The engine opened this project without migrating it; Studio and its player disagree on the project's format.` と
  言います。移行だけを受ける player は窓が無いので、閉じるときは標準入力を閉じて終わらせます。
- 移行の取り消し(退避先からの復元)の UI はありません。
- **分かっている穴**(着地時の記録): 移行の画面を出している間も、Frame Plan・Pass・Graph の panel は前の project の root を
  持ったままです。移行の後の offline open が parse error になると、移行だけの player は開き直すか Don't Open まで残ります。
  player の窓を 15 秒で諦める timeout は gate で止めていますが、その test はありません。

### Studio の Assets のドック —— 外の保存を反映する素材と、素材の照合(✅HW-2s / ✅AV-1s)

✅HW-2s / AV-1s(`78737fdf`)から、Studio に **Assets のドック**(`pelican.assetsDock`。中身は
[`AssetsDock`](../../src/devstudio/view/assetsdock.hpp#L40)、RPC のやり取りは Qt を使わない
[`AssetsDockSession`](../../src/devstudio/model/assetsdockmodel.hpp#L144))があります。project の素材のフォルダを木で見せ、
行ごとに「外のツールで保存したら engine が読み直す」(project.json の `hot_reload`。engine の ✅HW-1)を切り替え、素材の中身を
manifest と照合する(`asset_stores` の manifest。engine の ✅AV-1)場所です。どちらの宣言も engine の ✅PJ-1 の
`edit_project_settings` で動いている engine に書かせるので、engine を起こし直さずに効きます(§13.5 の「project.json の宣言を
書き換える(✅PJ-1)」)。Blank / Standard の雛形はどちらの鍵も宣言しませんが([第3章](03_project_format.md) §3.4 の
「起動中の engine に宣言を足す」)、**Blank から作った project でも、このドックの操作だけで監視と照合を始められます**。
押さなければ project.json には何も書きません。

- **開き方**: 既定の配置では Project のドックの隣のタブです([既定の配置](../../src/devstudio/view/mainwindow.cpp#L3523))。
  **View > Assets**(`pelican.view.assets`)で前に出します([View > Assets](../../src/devstudio/view/mainwindow.cpp#L1567))。
  View > Panels でも開閉できます。木の列は Asset / Reload / Contents です。根は `assets/` と、`asset_stores` の mount のうち
  `assets/` の外にあるもので(場所は `get_status.stores`、無ければ project root からの mount。[`assetRoots()`](../../src/devstudio/model/assetsdockmodel.cpp#L165))、
  `.gitkeep` は出さず、フォルダが先・名前の順です。木は `QFileSystemWatcher`(フォルダの全部と、監視しているファイル 512 個まで)が
  変化を言ってから 250 ms 後に全体を読み直します。変化が無い間に時計で読むことはありません([読み直す所](../../src/devstudio/view/assetsdock.cpp#L324))。
- **行ごとの「Reload when saved outside」**: Reload の列の checkbox か、右クリックの **Reload when saved outside**
  (`pelican.assets.reloadWhenSavedOutside`)で、ファイルにもフォルダにも付けられます。押すと `get_project_settings` で今の
  digest と値を読み、`hot_reload` の配列を書き換えた `edit_project_settings` を 1 本送ります([送る所](../../src/devstudio/model/assetsdockmodel.cpp#L491))。
  フォルダを入れると、その下で個別に入っていたものは配列から外れます。最後の 1 つを外すと `hot_reload` は空の配列ではなく
  `null`(鍵ごと消す)になり、雛形の状態に戻ります。印は `get_status.reload.watch` から読みます。親のフォルダで効いている行は
  灰色の中間の印と `via textures` の形で言い(当たり判定は成分ごとの前方一致。[`assetWatchState()`](../../src/devstudio/model/assetsdockmodel.cpp#L133))、
  その行を外そうとすると `<path> is watched because <folder> is; untick <folder> to stop.` と言って何も送りません
  ([切り替えの入口](../../src/devstudio/view/assetsdock.cpp#L538))。engine が動いていないときも送らずに
  `The engine is not running; the watch is set when it is.` と言います(押した切り替えは覚えておきません。コードの読み)。
- **Undo**: 1 回の切り替えと 1 回の Track asset contents が、それぞれドックの Undo の 1 段です。Undo は木の上の右の
  **Undo <何を>** のボタン(`pelican.assets.undo`)、ドックの中での Ctrl+Z、右クリックの Undo で、答えの `previous` のうち
  その編集が変えた鍵の値をそのまま `edit_project_settings` で送ります([積む所](../../src/devstudio/model/assetsdockmodel.cpp#L525))。
  Edit メニューの Undo Render Edit や Inspector の Undo とは別の履歴で、project を替えると消え、engine の起こし直しでは残ります。
  値で戻すので、project.json の digest は元に戻りません(PJ-1 の 2 空白の indent)。
- **監視の 1 行**: 何かを監視している間だけ、木の上に `Watching 1 folder and 1 file · last reload: textures/rock.ktx2` の形の
  1 行が出ます([`assetsWatchLine()`](../../src/devstudio/model/assetsdockmodel.cpp#L324))。tooltip は監視している path の一覧です。
  「最後に読み直した素材」は **Studio の推定**です。engine は読み直したファイル名を返さないので、Studio の watcher が監視の中で
  書き換わったファイルを見たら `get_status` を 400 ms ごとに最大 20 回読み、`reload.hashed_files` が増えた時点で、書き換わった
  中で最新のファイルを出します([推定する所](../../src/devstudio/model/assetsdockmodel.cpp#L657))。木に出る render 文書の行は、
  tooltip で「外の保存は監視ではなく Graph の Reload from disk が取り込む」と言います(engine の RL-1)。
- **Track asset contents…**(`pelican.assets.trackContents`): engine の設定を読めて、どの store も manifest を宣言していない
  ときだけ木の下に出ます。押すと確認なしに 1 回の `edit_project_settings` で
  `asset_stores.main = {mount: "assets/", manifest: "assets.manifest.json"}` を宣言し、engine が manifest を書いて照合を始めます。
  store が既にあれば、manifest の無い store ごとに manifest だけを足します(最初の 1 つが `assets.manifest.json`、それが使われて
  いれば `assets.<store>.manifest.json`。[`assetStoresWithManifest()`](../../src/devstudio/model/assetsdockmodel.cpp#L351))。
  照合の終わりは通知されないので、`running` の間は `get_status` を 300 ms ごとに(最大 600 回)読み、終わったら
  `get_assets_verification` を読みます。宣言した後はボタンが消えます。tooltip は manifest を project と一緒に commit するよう言います。
- **照合の印と一覧**(AV-1s): `get_status.assets_verification` が `running` か `verified` のときだけ、木の下に 1 行が出ます
  (`not_run` では何も出ません)。`Checking asset contents…` / `Asset contents match the manifest (N files).` /
  `3 files differ from the manifest: 1 content differs, 1 missing, 1 extra.` の形です([`assetsVerificationLine()`](../../src/devstudio/model/assetsdockmodel.cpp#L288))。
  食い違いが 1 件以上なら枠と太字になり、**Details**(`pelican.assets.showIssues`)で store ごとの一覧(種類・ファイル・message)が
  開きます。該当するファイルの行の Contents の列には `content differs` / `missing` / `extra` / `case differs` / `unreadable`
  (manifest 自体が読めなければ `manifest unreadable`)が出て、tooltip が engine の message です。新しく印の付いたファイルの
  フォルダは 1 度だけ開きます。disk に無いファイル(`missing`)は、そのフォルダの下に灰色の斜体の行として出て、切り替えは
  付きません([印を付ける所](../../src/devstudio/view/assetsdock.cpp#L444))。
- **Verify again / Update manifest**: 帯の **Verify again**(`pelican.assets.verifyAgain`)は `verify_assets` を送ります。
  manifest を持つ store ごとに `<store> — <manifest>` と **Update manifest**(`pelican.assets.updateManifest.<store>`)の行が出ます。
  Update manifest を押すと、まず帯の中に `1 file will change in assets.manifest.json.` の確認の 1 行と **Update** / **Cancel** が
  出ます(modal のダイアログではありません。何も違わなければ `Nothing differs; <manifest> is written again as it is.`。
  [`manifestUpdateLine()`](../../src/devstudio/model/assetsdockmodel.cpp#L315))。Update で `update_assets_manifest {store}` を
  送ります。照合が走っている間と、Track・Verify again・Update の答えを待つ間は、どちらも押せません
  ([`verificationRunning()`](../../src/devstudio/model/assetsdockmodel.cpp#L567))。hot reload で素材を読み直しても照合の結果は
  変わらないので、確かめるときは Verify again を押します(AV-1 の仕様どおり。Verify again の tooltip もそう言います)。
- **拒否は 1 文**: `edit_project_settings` と AV-1 の RPC の名前付きの拒否 9 種を `… (<code>).` の 1 文にして、ドックの notice に
  出します(例 `project.json changed outside Studio since it was read; try again (external_modification).`。
  [1 文の表](../../src/devstudio/model/inspectormodel.cpp#L1632))。`external_modification` の後は project.json の値を読み直します。
- **`restart_required`**: 答えの `restart_required` が `null` でなければ、notice に `Restart the engine to apply: <理由>` と
  **Restart engine**(`pelican.assets.restartEngine`。Frame Plan と同じ `MainWindow::requestEngineRestart()`)が出ます。
- **shader の読み直しとの重なり**: 監視に shader を入れると、engine の watcher と Studio の `reload_shaders` が両方動きます。
  Studio の側の扱いは §13.5 の「shader を外のエディタで保存すると画面が変わる」の「何も変わっていない」です(✅HW-2s の決定 7)。

**残した穴**(着地時の記録):

- **起動時からある store は Undo で外せません。**engine を起こし直すと、Track で足した store も「起動時からある store」になり、
  その Undo は PJ-1 に断られて `<store> was there when the engine started, so it stays declared — only stores added since then can be undone (project.settings_change_unsupported@1).`
  と出ます([Undo のときの 1 文](../../src/devstudio/model/inspectormodel.cpp#L1647))。外すには project.json を手で直して engine を
  起こし直します。断られた Undo はドックの Undo に残ります(コードの読み)。
- **Restart engine のボタンは通っていません。**player は今 `restart_required` に `null` しか返さないので、test でも scenario でも
  この道を通していません(未確認)。
- **親のフォルダで効いている行は、その行だけ外せません。**engine の `hot_reload` に除外の書き方がないためで、行は上の 1 文で
  「親を外して」と言います。
- **監視を入れる前に保存した変更は読み直されません**(engine の穴。PJ-1 の「作り直した watcher は…」)。この engine の実行中に
  監視を入れたときだけ、監視の 1 行の tooltip に `Changes saved before now are not reloaded.` が足されます
  ([tooltip を作る所](../../src/devstudio/view/assetsdock.cpp#L595))。
- manifest を書いている間と照合の終わりを待つ間は frame が止まります(engine の穴。PJ-1)。
- 木は書き換えのたびに全体を走査し、Studio が見張るファイルは 512 個までです。大きい `assets/` での重さは測っていません(未確認)。

- test: `devstudio_assets_dock_test`(`QT_OFFSCREEN`。test が engine の代わりに答えます)の
  [「HW-2s decision 1: the Assets dock is the project's asset folders, without .gitkeep, and follows the disk」](../../test/devstudio_assets_dock_test.cpp#L167)・
  [「HW-2s decision 2: a row's switch is one edit of hot_reload, a folder's covers what is inside it, and Undo sends what was there before」](../../test/devstudio_assets_dock_test.cpp#L216)・
  [「HW-2s decision 3: the line above the tree says what is watched and what was read again last; a render document's row says the watch does not apply it」](../../test/devstudio_assets_dock_test.cpp#L298)・
  [「HW-2s decision 4: Track asset contents declares main at assets/ with a manifest in one edit, is offered only while no store declares one, and its Undo can be refused」](../../test/devstudio_assets_dock_test.cpp#L345)・
  [「AV-1s decision 5: what the check found is a line under the tree and a mark on each file's row; not_run shows nothing」](../../test/devstudio_assets_dock_test.cpp#L420)・
  [「AV-1s decision 6: Verify again and Update manifest wait while a check runs, and Update manifest says how many files change before it writes」](../../test/devstudio_assets_dock_test.cpp#L485)・
  [「HW-2s decision 7: with the engine's own watch open, a reload_shaders that found nothing new is followed as the watcher's commit」](../../test/devstudio_assets_dock_test.cpp#L562)。
  `devstudio_layoutpreset_test` はドックの数を 7 から 8 にし、古い保存済みの配置を戻しても Assets のドックが置き場を持つことを見ます。
- シナリオ: `gui_assets_watch_verify`(Blank から GUI だけで。§13.5「Studio の自動化面」)。

### Studio の Materials のドック —— surface の宣言から作る欄、New material…、object への割り当て(✅MT-1s)

✅MT-1s(`2d81ef51`)から、Studio に **Materials のドック**(`pelican.materialsDock`。中身は
[`MaterialsDock`](../../src/devstudio/view/materialsdock.hpp#L41)、RPC のやり取りは Qt を使わない
[`MaterialsDockSession`](../../src/devstudio/model/materialsmodel.hpp#L238))と、Inspector の **Material** の行
([`MaterialAssignWidget`](../../src/devstudio/view/materialassignwidget.hpp#L31)、やり取りは
[`MaterialAssignSession`](../../src/devstudio/model/materialsmodel.hpp#L403))があります。project の material を一覧し、
surface の宣言から作った欄で値を直し、新しい material を作り、object に割り当てる場所です。Studio は material の
ファイルを自分で書かず(D0)、engine の ✅MT-1 の 7 本(§13.5 の「material 文書を読み書きする(✅MT-1)」)と ✅MT-1b の
`list_surfaces`(下の New material…)、✅MT-2 の `set_object_material` / `get_object_materials`(下の「割り当て」)だけを送ります。
✅MT-2s(`7112072e`)から、Inspector の Material の行は**既定で選んだ object だけ**に割り当て(`set_object_material`)、
切り替えると MT-1s と同じ model 全部への割り当て(`bind_material`)になります。値の変更は engine が起こし直さずに当てるので、**Blank から作った project でも、
Cube を置いてその色を変えるまでが GUI だけで、再起動なしで済みます**(下のシナリオ)。MT-1s の前は、色 1 つに JSON を 3 つ手で書いて
engine を起こし直していました(仕様書の記述)。

- **開き方**: 既定の配置では Assets のドックの隣のタブです([既定の配置](../../src/devstudio/view/mainwindow.cpp#L3527))。
  **View > Materials**(`pelican.view.materials`)で前に出します([View > Materials](../../src/devstudio/view/mainwindow.cpp#L1584))。
  View > Panels でも開閉できます。
- **一覧**(決定 1): 上の木(`pelican.materials.list`。列は Material / Surface)は、`list_materials` の文書ごとの行と、その下の
  material の行です。`status` が `missing` / `invalid` の文書は灰色で、Surface の列に `<status>: <理由>` が出て、選べません。
  この engine にまだ登録されていない material(`registered: false`)は名前が灰色で、tooltip が「engine の起動時に読まれる」と言います
  ([木を作る所](../../src/devstudio/view/materialsdock.cpp#L359))。文書の行を選ぶと、その最初の material を選びます。
- **読み直し**: ドックが見えている間だけ、`list_materials` を 1 秒ごとに送ります([`PollMs`](../../src/devstudio/model/materialsmodel.hpp#L244)。
  隠れると止め、見え直したときに 1 回読みます)。答えが前と同じ(digest を含めて)なら何もしません。選んでいる material の文書の digest が、
  このドックの書き込みでないのに動いていれば(外で書かれた)、その material を読み直します([一覧の答えを読む所](../../src/devstudio/model/materialsmodel.cpp#L707))。
  Assets のドックは時計で読まない(ファイルの監視)ので、間隔は揃えていません(実装者の報告)。

**エディタ —— surface の宣言から作る欄**(決定 2・3)

material を選ぶと、`get_material` で文書のままの material を、続けてその `surface` の `get_surface_schema` を読み、宣言の
`params` / `textures` から欄を作ります(見出しは `<名前> — <path>`)。param の欄の種類は
[`materialFieldKind()`](../../src/devstudio/model/materialsmodel.cpp#L166) が決めます。

| 宣言 | 欄 | 自動化面の名前(`pelican.materials.param.<名前>` の後ろ) |
|---|---|---|
| `float` で `min` と `max` の両方がある | slider と spin(spin はその範囲) | `.slider` / `.spin` |
| `float` で範囲が揃っていない | spin | `.spin` |
| `int` | spin(hint が `bool` / `toggle` なら check) | `.spin` / `.check` |
| `color`、または hint が `color` の `vec3` / `vec4` | 色の swatch(押すと色のダイアログ) | `.swatch` |
| ほかの `vec2` / `vec3` / `vec4` | 成分ごとの spin | `.0` / `.1` … |
| それ以外 | 値を JSON の文字で出すだけ(編集しない) | `.value` |

- **送り方**: 変更 1 回が `edit_material` の `values` 1 回です。spin は確定したとき(Return か focus を外す)、slider は drag の間は数字だけを
  動かして離したとき、色はダイアログの OK で送ります。今の値と同じなら送りません。書く要求はドックの中で 1 本ずつ並べ、送る直前に、
  その文書について最後に知った digest を `base_material_digest` にします([送る所](../../src/devstudio/model/materialsmodel.cpp#L595))。
- **色**: encoding が `linear` の色は、swatch では sRGB に直して見せ、選んだ色を linear に戻して保存します(小数 6 桁)。`srgb` の色は
  見たままです([見せる色 `materialColorForDisplay()`](../../src/devstudio/model/materialsmodel.cpp#L224)、
  [保存する色 `materialColorFromDisplay()`](../../src/devstudio/model/materialsmodel.cpp#L236))。alpha は今の値のままで、swatch の文字は
  見せている色の `#rrggbb` です。色のダイアログ(`pelican.materials.colorDialog`)は Qt のダイアログで、hex の欄(`.hex`)と OK / Cancel
  (`.ok` / `.cancel`)に名前が付いているので、シナリオから打てます。
- **default との違い**: 文書が default と違う値を書いている param は、名前が太字です([`materialParamOverridden()`](../../src/devstudio/model/materialsmodel.cpp#L184))。
  名前の右クリックの **Reset to default**(`.reset`。default と違うときだけ押せる)は `values: {<名前>: null}` を送り、文書からその値を
  外します。名前の tooltip は型・hint・範囲と `Default: <値>` です。
- **テクスチャ**: `textures` の slot ごとに combo(`pelican.materials.texture.<名前>`)です。先頭の `None (<宣言の default>)` は `null` を
  送り(文書から外す)、続いて project の `assets/` の下の画像のうち、slot の `dimension` に合うものが `project://<論理 path>` の値で
  並びます(表示は `project://assets/` を外した path)。画像の次元は Studio がファイルから推します: KTX2 はヘッダの depth / layer / face の
  数から `2d` / `cube` / `cube_array` / `2d_array` / `3d`、`.png` / `.jpg` / `.jpeg` / `.tga` / `.bmp` / `.hdr` / `.exr` は `2d` です
  ([`textureFileDimension()`](../../src/devstudio/model/materialsmodel.cpp#L332))。tooltip は color space・role・dimension で、文書が
  名指す texture があれば slot の名前が太字です。
- **Surface・Tags・Alpha mode・Variants**(決定 3): **Surface** の combo は `list_surfaces` の一覧です(一覧に無い今の値はそのまま足して
  見せます)。**Tags** は UX-11 のタグ選びで、候補は一覧の material が持つタグ、`+ new tag` で新しいタグも足せます。**Alpha mode** は
  `opaque` / `mask` / `blend` の combo と **Double sided** の check で、`routing` を丸ごと送ります。**Variants** は名前の一覧を読むだけです
  (tooltip `Variants are edited in the document itself for now`)。どれも変更 1 回が `edit_material` 1 回です。MT-1 の engine は textures の変更と同じく
  書いたうえで `restart_required` を返していましたが、engine の ✅MT-3(`c6f94eb7`)から、engine が登録している material ではその場で
  登録し直して描いている model を組み直し、`applied: "rebuilt"`・`restart_required: null` を返します(§13.5 の規則)。Studio は
  `restart_required` が `null` でないときだけ起こし直すので([`MaterialsDockSession::writeResult()`](../../src/devstudio/model/materialsmodel.cpp#L747))、
  この場合は起こし直しません。Alpha mode の変更は、engine が同じ書き込みでその material を指す binding の routing も書き直します。
  engine の ✅VR-1(`45864d55`)から、OpenPBR の surface では Surface だけ・Alpha mode / Double sided だけの変更も、engine が
  もう片方(routing / surface)を manifest の組に合わせて同じ書き込みで書きます(§13.5 の規則)。VR-1 の前は、Surface を別の variant に
  替えると routing と合わずに断られました(下の「MT-3s の残した穴」)。Studio はドックの書き込みの後に選んでいる material を読み直すので
  ([書いた答えの後に読み直す所](../../src/devstudio/model/materialsmodel.cpp#L768))、読み直した文書の routing が Alpha mode / Double sided の欄に入ります
  ([routing を欄に入れる所](../../src/devstudio/view/materialsdock.cpp#L819))。✅SK-1(`67fd3d29`)のシナリオ `gui_material_from_blank` で確かめました:
  Surface を `openpbr / opaque_double_sided` に替える(送るのは surface だけ)と、起こし直さずに(`get_status` の `instance_id` が同じ)Double sided に
  印が付き、Alpha mode は `opaque`、Surface の欄は double-sided のままです。ドックの Undo で surface と routing が両方戻り、Double sided の印が外れて
  Surface の欄も one-sided に戻ります(§13.5「Studio の自動化面」の SK-1 の段落)。Studio のドックのコードは SK-1 で変えていません。
- **`rebuilt` の答え(✅MT-3s、`19be8983`)**: ドックの書き込み(`edit_material` とその Undo)の答えが `applied: "rebuilt"` なら、ドックの notice が
  `Applied; rebuilt N models.`(N は `rebuilt_models` の数。1 なら `Applied; rebuilt 1 model.`)を言い、Restart engine のボタンは出ません
  ([rebuilt を読む所](../../src/devstudio/model/materialsmodel.cpp#L802))。答えの `rewritten_bindings` に載った model ごとに、Inspector の Material の行が
  binding を読み直します(MainWindow が `InspectorWidget::materialBindingsChanged()` を呼ぶ。Undo が前の routing を送ったときも、engine が binding を戻すので同じです)。
  Surface・Tags・Alpha mode・Double sided の tooltip も、MT-3s で「engine がその場で登録し直す / binding も書き直す」旨に直しました(MT-1s では「engine の起動時に読む」)。
  答えを待つ間と、その後に material を読み直す間は、Surface と Alpha mode / Double sided を文書の値で埋め直さず、選んだ値を見せたままにします
  ([埋め直さない条件](../../src/devstudio/view/materialsdock.cpp#L806))。MT-1s ではこの間に一瞬前の値へ戻って見え、シナリオの `select` が「値が入らなかった」で
  落ちていました(実装者の報告)。
- **`restart_required`**: 書く答えの `restart_required` が `null` でなければ、Studio はその場で engine を起こし直します(RSX と同じ
  `MainWindow::requestEngineRestart()`。未保存の scene があれば今までの Save / Discard / Cancel が出ます)。ドックの notice には
  `Restart the engine to apply: <理由>` と **Restart engine**(`pelican.materials.restartEngine`)が残ります(Assets のドックと同じ)。
  Inspector の Material の行の `restart_required` も同じ道です。

**New material…**(決定 4)

- ドックの上の **New material…**(`pelican.materials.newMaterial`)を押すと、木の上に行が開きます(modal のダイアログではありません)。
  **Surface**(`pelican.materials.new.surface`)は engine の `list_surfaces`(✅MT-1b。engine に登録された `surfaces/**.surface` と project の
  `*.surface`。[engine の契約の注記](../../src/core/communication/materialdocumentrpc.hpp#L38))から作り、**Studio は surface の一覧の写しを
  持ちません**。表示は、engine の manifest が名前を付けた variant なら `<family> / <variant 名>`(例 `openpbr / opaque_single_sided`)、ほかは
  `surfaces/` の下の path から `.surface` を外したもの(project のものは `shaders/toon` など)です([`surfaceChoiceLabel()`](../../src/devstudio/model/materialsmodel.cpp#L271))。
  読めない surface(`error` 付き)は灰色で選べず、tooltip に理由が出ます。最初に選ばれているのは
  `engine://surfaces/openpbr/opaque_single.surface` があればそれ、無ければ読める最初のものです([`defaultSurfaceChoice()`](../../src/devstudio/model/materialsmodel.cpp#L284))。
- **Name**(`pelican.materials.new.name`)には空いている名前が入って開きます。`material`、使われていれば `material_2`、`material_3`… の、
  一覧の material の名前・一覧の文書の path・project の `materials/<名前>.material.json` のファイルのどれとも重ならない最初のものです
  ([`suggestMaterialName()`](../../src/devstudio/model/materialsmodel.cpp#L306))。その下に書く先 `materials/<名前>.material.json`
  (`pelican.materials.new.path`)が出ます。名前が英数字と `_` でない・もうある名前のときは、行の中に 1 文が出て **Create** が押せません。
- **Create**(`pelican.materials.new.create`)は `create_material {path, surface, name}` を 1 回送ります。選んだ surface の `list_surfaces` の
  答えが `routing`(OpenPBR の variant の `alpha_mode` / `double_sided`)を持っていれば、それも `routing` に載せます
  ([`MaterialsDockSession::create()`](../../src/devstudio/model/materialsmodel.cpp#L647))。engine は live で登録するので、起こし直しは要りません。
  notice に `Created <名前> in <path>.` が出て、次の `list_materials` にその material が現れた時点でそれを選びます。
- engine が `list_surfaces` を知らない(`-32601`)ときは、行が `This engine does not list surfaces (list_surfaces), so no material can be made here yet.`
  を言い、Create は押せません。
- 作った material は Undo の対象ではありません(engine に `delete_material` が無いため)。確認なしで作ります。

**割り当て**(決定 5。✅MT-2s で object ごとに)

- **Inspector の Material の行**: `simplemodelview` の `model` を持つ object を選ぶと、Inspector の Attached graphs の下(properties の scroll
  より上)に **Material** のグループ(`pelican.inspectorMaterial`)が出ます([置く所](../../src/devstudio/view/inspectorwidget.cpp#L320))。
  model の無い object と、再生中に spawn された object では出ません。
- **範囲の切り替え**(✅MT-2s の決定 1): グループの一番上の combo(`pelican.inspectorMaterialTarget`)で、選ぶ material が何に効くかを
  決めます。項目は **This object**(既定)と **Every <model>**(例 `Every Cube`。名前は `(built-in)` を外した model の名前。
  [`modelShortName()`](../../src/devstudio/model/materialsmodel.cpp#L395))です。切り替えは Material の行の session が持つので、
  別の object を選んでも選び直すまで残ります(シナリオの step 119 が、Cube から Cube_001 に移っても Every Cube のままであることを見ます)。
- **This object**: material を選ぶと、選んだ object の全 primitive の行を `set_object_material` 1 回で書きます。その object が primitive ごとの
  行を持っていれば、同じ 1 回でそれも外します(残すと primitive の行が勝ち、選んだ material にならないため。実装者の判断)
  ([`MaterialAssignSession::choose()`](../../src/devstudio/model/materialsmodel.cpp#L1113))。今と同じ material なら送りません。
  - **送り方**(決定 5): Material の行は自分で送らず、Inspector の scene の編集の session(`InspectorModel`)に渡します。session が、
    scene の編集で使っている `actor_id` と、最後に見た scene の revision を `base_scene_revision` に足し、自分の編集の 1 つとして送ります
    ([`InspectorModel::submitObjectMaterial()`](../../src/devstudio/model/inspectormodel.cpp#L4101)、
    [Material の行から渡す所](../../src/devstudio/view/inspectorwidget.cpp#L330))。そのため **scene の Undo**(Inspector の Undo のボタン。Ctrl+Z も RU-1 から
    同じ Undo です)で戻ります(下の「Undo」)。別の編集が進んでいる間は送らず、行に `Finish the edit in progress first, then choose the material again.` が出ます。
    答えが `changed: false` なら編集にせず、Inspector の notice は `<何を>: nothing to change.` です。
  - **今の状態**(決定 2): Material の行は選んだ object の `get_object_materials` を読み、combo の先頭(選べない項目)にその object が
    描いている material を出します。object 自身の行から来ていれば `<名前> (this object)`、model の既定なら `(model default: <名前>)` です
    (例 `material_2 (this object)`、Blank の Cube は `(model default: pelican.primitive.default)`。名前は `drawn`、instance が無ければ
    `material`。[`objectMaterialLabel()`](../../src/devstudio/model/materialsmodel.cpp#L449))。primitive ごとに違うときは
    `(differs per primitive)` です。続いて `list_materials` の名前が並びます。答えを待つ間は選んだ名前を示したまま combo を止め、
    Inspector の編集が終わってから読み直します。
  - **Use model default**(`pelican.inspectorMaterialUseModelDefault`。This object のときだけ見え、object が自分の行を持つときだけ押せる):
    object の行を全部 `material: null` で外す `set_object_material` を 1 回送ります([`MaterialAssignSession::useModelDefault()`](../../src/devstudio/model/materialsmodel.cpp#L1190))。
    combo の項目ではなくボタンにしたのは、combo の先頭が今の状態の表示で選べないためです(実装者の判断)。
  - 下の 1 行(`pelican.inspectorMaterialScope`)は `Applies to <object の名前> only.`、tooltip は `Every other <model> keeps the model's material` です
    ([`objectScopeLine()`](../../src/devstudio/model/materialsmodel.cpp#L413))。
  - **読み直し**: Inspector が持つ object の scene の revision が動くたび(自分の編集・Undo・ほかの client)に、`get_object_materials` と
    `get_material_bindings` を読み直します。Inspector の session に編集が進んでいる間は行が待ち、終わったら読み直します
    ([`MaterialAssignSession::setSubject()`](../../src/devstudio/model/materialsmodel.cpp#L962)、
    [`MaterialAssignSession::setSceneBusy()`](../../src/devstudio/model/materialsmodel.cpp#L994))。
  - `get_object_materials` を知らない engine(`-32601`)では、行が `This engine cannot give an object its own material (no get_object_materials); choose Every <model>.`
    と言います([その文](../../src/devstudio/model/materialsmodel.cpp#L1267))。
- **Every <model>**(MT-1s の割り当て): combo は **Model default**、区切り、`list_materials` の material の名前の順です。material を選ぶと
  `bind_material {model, material}`(model の全 primitive)を 1 回送ります。Model default は、割り当てを変えた primitive をそれぞれ model 自身の
  material(`get_material_bindings` の `slot`)に戻す `bindings` を送ります。primitive ごとに違う material を持つときは、combo は選べない項目
  `(differs per primitive)` を示します。MT-2s の着地時、engine の基本形(`engine://primitives/cube` など)では
  この Model default が engine に断られました。engine の ✅MT-2b(`213562bb`)から、engine はこのとき割り当ての宣言を外し、答えの
  `material_bindings` は `null` です。✅MT-2s-b(`9d15ddc0`)のシナリオで、基本形の Every Cube の Model default の後に `get_material_bindings` の
  `material_bindings` が `null`、primitive 0 が `pelican.primitive.default` になり、combo が `Model default` を言い、両方の箱が灰になることを確かめました。
  Studio は bind の答えの `material_bindings` を読まず、bind の後に `get_material_bindings` を読み直して、全 primitive の material が `slot` と
  同じなら combo を Model default にします([その判定](../../src/devstudio/model/materialsmodel.cpp#L1057))。全 primitive が既に `slot` のままなら
  Model default は何も送りません。送った後に別の client が外していて engine が `changed: false` を返したときは、Undo の項目を積まずに読み直すだけで、
  行はまた選べます([Undo を積む条件](../../src/devstudio/model/materialsmodel.cpp#L1243)。✅MT-2s-b の決定 3。GUI からは起こせないので test だけ)。
  - 下の 1 行は `Applies to every <model> in this project (N objects, M with their own material).` です(例
    `Applies to every Cube in this project (2 objects, 1 with their own material).`。[`modelScopeLine()`](../../src/devstudio/model/materialsmodel.cpp#L403))。
    tooltip は `A material is bound to the model (<model>); an object with its own keeps it` です。
  - **N と M**(決定 4): N は `get_material_bindings` の `objects`(その model を描く scene の object の数。engine は全 scene と prefab から
    生成された object も数えます)、M は `object_overrides`(そのうち自分の行を持つ数)です([`parseModelObjectCounts()`](../../src/devstudio/model/materialsmodel.cpp#L381))。
    答えに無ければその数を省きます。scene の revision が動くたびに読み直すので、同じ model の object を足すと N が増えます(シナリオの step 120)。
    MT-1s の `export_scene_snapshot` で数える道は消しました。置き換える前に実装者が両方を並べて 1 回流し、Cube 1 つで 1 = 1、
    Cube 2 つで 2 = 2(上書きが 1 つある状態でも)と同じ値でした(報告)。数え方の違い(snapshot は今の scene の prefab の外だけ)は
    Blank では差が出ません。
- **primitive が複数ある model**(決定 3): その下に `Mesh <n> · primitive <m>` の行ごとの combo(`pelican.inspectorMaterialPrimitive.<番号>`)が
  出ます。数は This object なら `get_object_materials` の、Every <model> なら `get_material_bindings` の `primitives` です。行も範囲に従います
  ([`MaterialAssignSession::choosePrimitive()`](../../src/devstudio/model/materialsmodel.cpp#L1147)):
  - This object: 選ぶと `primitive: {mesh, primitive}` 付きの `set_object_material` です。その primitive が object の行から描いているときだけ、
    combo に `Model default (<model の割り当て>)` が出ます。選ぶと、その primitive の行を外します。その primitive の行が無く全 primitive の行が
    効いているときは、外すだけでは model に戻らないので、その primitive の行に model の割り当てを名指して書きます(実装者の判断)。
  - Every <model>: 選ぶとその primitive だけの `bindings` の `bind_material` です(MT-1s のまま)。
  - Blank の Cube は primitive が 1 つなので、この行はシナリオでは通っていません(test だけ)。
- **ドックの Assign to selected object**(`pelican.materials.assignSelected`): ドックで選んだ material を、Inspector が今出している object の
  model に `bind_material {model, material}` で割り当てます(MT-2s でも model 単位のままです)。model を持つ object を選んでいなければ
  `Pick an object that shows a model in the Outliner first.` と言います。割り当ての後は Inspector の Material の行が読み直します。
- **Inspector の汎用の list の行と二重にしない**(決定 6): engine の ✅MT-2 から simplemodelview の `schema.fields` に
  `{"name": "materials", "type": "list"}` が入りました。Inspector はこの型を読めず、model を持つ object を選ぶと欄が 1 つも出なくなっていました
  (`44b7b50a` から `7112072e` の前までの develop。実装者の報告では、MT-1s の `gui_material_from_blank` も step 20 で落ちていました)。
  MT-2s から、Inspector は flat な `schema.fields` の `list` 型を欄にせず飛ばします([飛ばす所](../../src/devstudio/model/inspectormodel.cpp#L359))。
  list の行はもともと schema の `node` から作るもので、simplemodelview の `/materials` はそこでも汎用の行にせず、
  `Chosen in the Material rows above`(`pelican.inspectorListElsewhere`)の 1 行にします([`inspectorListShown()`](../../src/devstudio/model/inspectormodel.cpp#L1723))。
  実装者の報告では、engine は今 simplemodelview に `node` を送らないので、この 1 行は今は出ません(engine が送るようになったときの備えで、
  test だけが見ます)。

**Undo**(決定 6)

- ドックの **Undo**(action `pelican.materials.undo`。ボタンは木の上の右の `pelican.materials.undoButton`、ドックの中での Ctrl+Z も)は、
  ドックの書き込みを 1 回ずつ戻します。`edit_material` は答えの `previous` を、その書き込みの後の `material_digest` で同じ RPC に送り、
  Assign to selected object の `bind_material` は `previous.bindings` を `bindings` に送ります([`MaterialsDockSession::undo()`](../../src/devstudio/model/materialsmodel.cpp#L677))。
  ボタンの文字は `Undo <何を>` で、書き込みの答えを待つ間は押せません。project を替えると消え、engine の起こし直しでは残ります(コードの読み)。
- **This object の割り当て(`set_object_material`)は scene の Undo で戻ります**(✅MT-2s の決定 5)。Inspector の scene の編集の session の
  `actor_id` で送るので、Inspector の Undo(`pelican.inspectorUndo`)が engine の `undo {actor_id}` でそれを戻し、Redo も同じです
  (シナリオの step 202〜205 が、Use model default の後に Inspector の Undo で上書きが戻ることを見ます)。Material の行は revision が動いたのを
  見て読み直します。
- Inspector の Material の行の **Undo**(`pelican.inspectorMaterialUndo`)は、Every <model> の `bind_material` だけを `previous.bindings` で
  戻します。履歴に項目があるときだけ見えます。ドックとは別の履歴で、Edit メニューの Undo Render Edit や Inspector の scene の Undo とも別です。
  ✅MT-2s から、項目は送った先の model を覚え、押した時点で別の object を選んでいても、その model に送り返します
  ([`MaterialAssignSession::undo()`](../../src/devstudio/model/materialsmodel.cpp#L1201))。MT-1s では押した時点の model に送っていました
  (下の「残した穴」の MT-1s の項)。MT-2s の着地時、engine の基本形ではこの Undo が engine に断られました。
  ✅MT-2s-b のシナリオで、基本形でも Every Cube の Model default を行の Undo で戻すと model が `material_2` に戻り、両方の箱が赤に戻ることを確かめました
  (Undo の表示は 1 つ前の項目 `Undo material_2 on every Cube` になります)。
- `create_material` は戻せません(上)。

**拒否**(決定 7)

- MT-1 の拒否 13 種(`material.*@1`)、✅MT-2s で足した `material.object_unknown@1`(`Object <object> cannot take a material of its own: <理由>`。
  ✅MT-2s-b から、engine が `name` を付けたとき(空でない文字列)は `<name> cannot take a material of its own: <理由>`。
  [名前で言う所](../../src/devstudio/model/inspectormodel.cpp#L1717))、
  `external_modification` を、Studio の 1 文の表で `<文> (<code>).` にします
  (表 [`MaterialRefusalTexts`](../../src/devstudio/model/inspectormodel.cpp#L1651)、[`materialRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L1692))。
  例: `materials/material.material.json changed outside Studio since it was read; it was read again, try again (external_modification).`。
  表に無い code は engine の message のままです。
- ✅MT-3s で engine の ✅MT-3 の `material.routing_conflict@1`(binding に指されている material の routing を外す)を同じ表に足しました
  ([表の行](../../src/devstudio/model/inspectormodel.cpp#L1192)、[`bound_by` から model を並べる所](../../src/devstudio/model/inspectormodel.cpp#L1704))。
  文は `<material> keeps its routing: <models> draw it through their bindings; bind them to another material first (material.routing_conflict@1).` で、
  `<models>` は `bound_by` の `model` を `, ` でつないだもの(無ければ `models`)、`material` が無ければ `The material` です。
- `material.value_invalid@1` だけは、その param の行の下に赤い 1 行(`.error`。`<param> cannot take that value: <理由> (material.value_invalid@1).`)
  で出ます。ほかはドックの notice(Inspector の行が受けた拒否は、その行の notice)です。断られた書き込みの後ろに並んでいた書き込みは捨て、
  一覧と material を読み直します。
- **`set_object_material` の拒否**(✅MT-2s の決定 7): MT-2 の拒否 4 種(`material.object_unknown@1` / `material.model_unknown@1` /
  `material.name_unknown@1` / `material.binding_invalid@1`)は同じ表の 1 文で、Material の行の notice(`pelican.inspectorMaterialNotice`)と
  Inspector の notice の両方に同じ文が出ます。Material の行は object を読み直します。`error_code` 付きの拒否は何も書いていないので、
  Inspector は scene を未保存扱いにしません(`InspectorModel::receiveRpcFailure()` の `refused`。届かなかった答えは今までどおり未保存扱い)。
- **名前の解けない上書き**(決定 7): This object のとき、`get_object_materials` の行に `resolved: false` があれば、combo の下に
  `⚠ Material <名前> is missing; drawing the model default`(`pelican.inspectorMaterialMissing`)が出ます
  ([`objectMaterialMissingLine()`](../../src/devstudio/model/materialsmodel.cpp#L455))。

**残した穴**(着地時の記録):

- **作った後に名前を変えられません。**仕様は「edit_material の name」で変えるとしていましたが、engine の `edit_material` の `name` は対象の
  material を選ぶ鍵で、改名ではありません。`delete_material` も無いので、作り損ねた material を GUI で消せません(engine に回しました)。
- **variants は読むだけ**です(編集の欄はありません)。
- **テクスチャの候補は project の `assets/` の下だけ**です(asset store の別の mount は入りません)。次元は Studio の推定です。
- **再起動の道(tags / routing / surface / textures の `restart_required`)はシナリオで通していません**(test だけ)。engine の ✅MT-3 から、
  engine が登録している material ではこれらは `rebuilt` になり、再起動の道に入るのは登録していない material・登録し直しか組み直しの失敗の
  ときだけです。`rebuilt` の答えを受けた Studio の振る舞いを通した test・シナリオはありません(未確認)。
  → ✅MT-3s(`19be8983`)で test(下の「MT-3s decision 5」)とシナリオ(`gui_material_from_blank` の tag の段)が通りました(上の「`rebuilt` の答え」)。
  再起動の道(`restart_required`)は MT-3s の test が今までどおり再起動することを見ますが、シナリオでは通していません。
- engine の ✅MT-3 の `material.routing_conflict@1`(binding に指されている material の routing を外す)は、Studio の拒否の文の表
  ([`materialRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L1692))に無いので、engine の message がそのまま出ます(コードの読み)。
  → ✅MT-3s で表に足しました(上の「拒否」)。
- ドックの routing / surface の combo は、edit の答えが来るまで一瞬前の値に戻って見えます(答えで正しくなります。実装者の報告)。
  → ✅MT-3s で直りました(上の「`rebuilt` の答え」)。
- MT-1s の着地時に、この文書を書いた側がコードの読みで書いた 2 つ —— Inspector の Material の行の Undo が戻す割り当ての model を覚えず、
  押した時点の model に送ること、N が同じ model の object を足しても読み直さないこと —— は、✅MT-2s で直りました(上の「Undo」と「N と M」)。
- engine に回したもの: 改名と `delete_material`、surface の variant の人が読む名前、model を使う object の数を `get_material_bindings` で
  (✅MT-2 の `objects` で入り、MT-2s が使います)、画像の一覧の dimension。

**MT-2s の残した穴**(着地時の記録。実装者の報告):

- 着地時の記録の 2 つ —— engine の基本形で Every <model> の Model default と Material の行の Undo が `material.binding_invalid@1` で断られたこと
  (シナリオは Model default を使わず `material_2` で代わりをしていました)、`material.object_unknown@1` の 1 文が object の id で読めたこと —— は、
  engine の ✅MT-2b(`213562bb`)と Studio の ✅MT-2s-b(`9d15ddc0`)で済みました(上の「Every <model>」「Undo」「拒否」)。
- ドックの **Assign to selected object** は今も model 単位の `bind_material` です(仕様に無いので触っていません)。
- primitive が複数ある model の行(This object)、拒否の道 4 種、missing の印はシナリオで通していません(test だけ)。
- engine の MT-2 で simplemodelview の flat な `schema.fields` に `list` の型が入りました。Studio は飛ばすようにしましたが、同じ形の欄を
  読む別の client があれば同じ所で落ちます(実装者の報告。engine に知らせました)。

**MT-2s-b の残した穴**(着地時の記録。実装者の報告):

- 名前で言う 1 文と `changed: false` の道は test だけです(拒否の道と `changed: false` は GUI から作れません)。
- primitive が複数ある model の Every <model> の Model default は test だけです(MT-2s と同じ)。

**MT-3s の残した穴**(着地時の記録。実装者の報告):

- ドックの Surface を OpenPBR の別の variant(double-sided など)に替えると、engine が `routing variant 'opaque_single_sided' does not match surface pipeline` で断ります。
  surface と routing は 1 回の `edit_material` で揃わないためです。シナリオは surface の代わりに tag の追加で `rebuilt` を見ています。
  surface を替えるときに routing も一緒に替える道(または create と同じく variant の routing を採る)を engine に回しました。
  → engine の ✅VR-1(`45864d55`)で済みました。surface だけを送れば routing が、routing だけを送れば surface が manifest の組に合わせて
  同じ書き込みで書かれます(§13.5 の「`edit_material`」の VR-1 の項)。ドックの Surface を替える道は ✅SK-1(`67fd3d29`)で
  `gui_material_from_blank` が通しました(opaque_single ↔ opaque_double。上の「Surface・Tags・Alpha mode・Variants」)。Alpha mode の欄から routing だけを送り
  engine が surface を替える道はシナリオでは通していません(engine の unit test だけ。SK-1 の実装者の報告)。
- `rewritten_bindings` で読み直すのは Inspector の Material の行だけです。`material.routing_conflict@1` の 1 文は test だけで、GUI の操作からは作っていません。

- test: `devstudio_materials_test`(`QT_OFFSCREEN`。test が engine の代わりに答えます)の
  [「MT-1s decision 1: the Materials dock lists the documents and their materials, a missing one greyed with its reason, and a poll whose digests are the same changes nothing」](../../test/devstudio_materials_test.cpp#L357)・
  [「MT-1s decision 2: the editor's rows are made from the surface's params -- a slider and a spin, a swatch, a spin, a texture combo by dimension -- and each change is one edit_material」](../../test/devstudio_materials_test.cpp#L397)・
  [「MT-1s decision 3: tags, routing and surface are one edit each, and restart_required restarts the engine as every other panel's edit does」](../../test/devstudio_materials_test.cpp#L489)・
  [「MT-1s decision 4: New material… offers the engine's surfaces, proposes a free name and its file, creates it and selects it」](../../test/devstudio_materials_test.cpp#L537)・
  [「MT-1s decision 5: the Inspector's Material combo binds every primitive of the model, says it is the model's, has a row per primitive when there are several; the dock assigns to the selected object」](../../test/devstudio_materials_test.cpp#L621)・
  [「MT-1s decision 6: Undo sends an edit's previous against the digest after it, and a bind's previous bindings; a new material is not in it」](../../test/devstudio_materials_test.cpp#L689)・
  [「MT-1s decision 7: a refused value is said under its row; any other refusal is one sentence in the dock's notice」](../../test/devstudio_materials_test.cpp#L761)。
  `devstudio_layoutpreset_test` はドックの数を 8 から 9 にし、古い保存済みの配置を戻しても Materials のドックが置き場を持つことを見ます。
  ✅MT-2s は同じ test の fake に `edit_object` を足し、MT-1s の decision 5 / 6 を新しい API(Every Cube に切り替えてから選ぶ)に合わせ、
  [「MT-2s decision 1: a choice applies to this object first (set_object_material through the Inspector's session); Every Cube is the model's bind_material」](../../test/devstudio_materials_test.cpp#L797)・
  [「MT-2s decision 2: the combo shows what is drawn and where it comes from; Use model default takes the object's own away」](../../test/devstudio_materials_test.cpp#L848)・
  [「MT-2s decision 3: with several primitives, each row follows the target -- This object sends set_object_material for that primitive」](../../test/devstudio_materials_test.cpp#L873)・
  [「MT-2s decision 4: N and M are get_material_bindings' objects and object_overrides, read again whenever the scene's revision moves」](../../test/devstudio_materials_test.cpp#L927)・
  [「MT-2s decision 5: an object's material is the scene's Undo; the rows' own Undo takes a bind back to the model it was sent to, whatever is selected by then」](../../test/devstudio_materials_test.cpp#L950)・
  [「MT-2s decision 6: a simplemodelview's materials list is chosen in the Material rows, not typed in the generic list」](../../test/devstudio_materials_test.cpp#L992)・
  [「MT-2s decision 7: a refused set_object_material is one sentence in the rows; a material that is not loaded is marked」](../../test/devstudio_materials_test.cpp#L1013)
  の 7 本を足しました。`devstudio_inspector_test` の
  [「MT-2s decision 5: the Material rows' set_object_material is an edit of the Inspector's own session, so its Undo is the scene's Undo」](../../test/devstudio_inspector_test.cpp#L2760)
  は、`actor_id` と `base_scene_revision` が足されること、`changed: false` が編集にならないこと、拒否が未保存扱いにならないこと、
  commit の後の Undo が同じ actor で送られることを見ます。実装者の run で `ctest -R 'MT-1s|MT-2s'` 15 / 15、`studio_cpu` 702 / 702 が緑です。
  ✅MT-2s-b は同じ `devstudio_materials_test` に
  [「MT-2s-b decision 1: a refused object is called by its name when the engine gives one」](../../test/devstudio_materials_test.cpp#L1045)・
  [「MT-2s-b decision 2: Model default on Every Cube is taken off (material_bindings null), reads as Model default, and its Undo binds the material again」](../../test/devstudio_materials_test.cpp#L1092)・
  [「MT-2s-b decision 3: a Model default the engine has already (changed:false) leaves the rows choosable, with nothing to undo」](../../test/devstudio_materials_test.cpp#L1145)
  の 3 本を足しました(test が返す答えは engine の MT-2b の形です)。実装者の run で `ctest -R 'MT-1s|MT-2s' -LE gpu` 18 / 18、`studio_cpu` 705 / 705 が緑です。
  ✅MT-3s は同じ test に
  [「MT-3s decision 5: an edit the engine rebuilt in place restarts nothing and says how many models it rebuilt; a routing edit's rewritten bindings are read again, its Undo too; a bound material's routing taken off is one sentence」](../../test/devstudio_materials_test.cpp#L1173)
  の 1 本を足しました(tag の `rebuilt` で再起動せず `Applied; rebuilt 1 model.`、routing の `rebuilt` で `Applied; rebuilt 2 models.` と `rewritten_bindings` の model の読み直し、
  答えを待つ間 Alpha mode の combo が選んだ値のまま、Undo が前の routing を送り binding を読み直す、`restart_required` は今までどおり再起動、`material.routing_conflict@1` の 1 文)。
  実装者の run で MT-1s / MT-2s / MT-2s-b / MT-3s の 19 / 19、`studio_cpu` 711 / 711 が緑です(BS-1s と同じ run)。
- シナリオ: `gui_material_from_blank`(Blank から GUI だけで。§13.5「Studio の自動化面」)。

### Studio の Problems のドック —— 今の問題の一覧と、直す場所へ飛ぶ行(✅PB-1s)

✅PB-1s(`93934318`)から、Studio に **Problems のドック**(`pelican.problemsDock`。中身は
[`ProblemsDock`](../../src/devstudio/view/problemsdock.hpp#L42)、一覧と読み取りは Qt を使わない
[`ProblemsSession`](../../src/devstudio/model/problemsmodel.hpp#L111)、1 行は [`ProblemRow`](../../src/devstudio/model/problemsmodel.hpp#L36))があります。
project の今の問題を 1 行 1 件で並べ、行を押すと直す場所へ飛びます。出どころは engine の ✅PB-1(`a55d1eaf`)の `get_problems`
(§13.5 の「今の問題の一覧を読む」)と、Assets のドックの照合(上の「Studio の Assets のドック」)の 2 つです。利用者の案(2026-10-04)は
VS Code の Problems のような一覧で、1 行に「どの JSON の何行目か・何が悪いか・Studio のどこで直すか」を出し、クリックで Graph の node や
生の文字の該当行に飛ぶ、というものです(仕様書の記述)。PB-1s の前は、問題は BS-1s の帯と Frame Plan の status 行、各 panel の notice、
shader の診断、Assets のドック、Frame Plan の致命の行にばらばらに出ていました(仕様書の記述)。

- **開き方**: 既定の配置では下の段のタブで、Output・Engine Log・**Problems**・Frame Plan・Pass Editor の順に並び、前に出るのは Frame Plan です
  ([既定の配置で Engine Log の次に置く所](../../src/devstudio/view/mainwindow.cpp#L3538))。**View > Problems**(`pelican.view.problems`)で前に出します
  ([View > Problems](../../src/devstudio/view/mainwindow.cpp#L1591))。View > Panels でも開閉できます(dock は 10 個になりました)。
- **列**(決定 1): Severity / Problem / Document / Line / Fix in の 5 列です([列の見出し](../../src/devstudio/view/problemsdock.cpp#L134))。
  - Severity は `Error` / `Warning` と印。Document は project の相対 path(`passes/main_rendering_config.json`。`engine://` の文書はそのまま)、
    Line は `12:5`(列が無ければ `12`、行が無ければ空)です。
  - Problem は code の 1 文で、各 panel が拒否を読むのと同じ表から作ります([`problemSentence()`](../../src/devstudio/model/problemsmodel.cpp#L54))。
    `render_document_syntax@1` は BS-1s の構文の 1 文、`graph.*` は graph の拒否の表(文書は列にあるので文には入れない)、
    `material_route_unserved@1` は Frame Plan の既存の文に `(material_route_unserved@1)`、
    `shader_reload_failed@1` は `<file> does not compile: <engine の message の 1 行目> (shader_reload_failed@1)`、
    `render_feature_requires_restart@1` は `The rendering setup turns on <feature>, whose runtime module <module> this engine was started without; restart the engine (render_feature_requires_restart@1).` です。
    ほかは `knownRefusalDescription()` / `materialRefusalDescription()` の表で、どれにも無ければ engine の message の 1 行目(code が入っていなければ後ろに `(<code>)`)です。
  - Fix in は直す場所の名前です: `Graph: <node>` / `Text: <node>` / `Text` / `Materials: <name>` / `Shader file` / `Assets: <file>` / `Rendering settings` / `Features` / `Restart the engine`。
    飛び先が無い行は `—` です([直す場所を決める `placeOf()`](../../src/devstudio/model/problemsmodel.cpp#L93))。
- **並べ替えと絞り込み**(決定 1): 見出しの click で並べ替えます。既定は Severity(error が先、同じ重さなら文書と行の順)、Document の列は文書・行・列の順、
  Line の列は数の順です([並べ方](../../src/devstudio/view/problemsdock.cpp#L38))。上の combo `.severity`(All / Errors / Warnings)で重さを、欄 `.filter` で文字を絞ります。
  文字は問題の文・文書・code・Fix in・engine の message のどれかに(大文字小文字を区別せず)含まれれば残ります。選んでいた行が隠れたら選択を外します。
- **選んだ行の 1 行**: 一覧の下の `.detail` が `<document>:<line>:<column> node <node> — <問題の文>` を言い、tooltip に engine の message と `Met: <source>` が続きます。
  件数は上の `.count` です。一覧が読めなかったときは `.notice` に `Could not read the engine's problems: …` が出ます。
- **集める**(決定 2):
  - engine の一覧: 誰が送った `get_status` の返事でも見て、`problems.revision` が前に読んだものと違うときだけ `get_problems` を送ります
    ([`ProblemsSession::observeStatus()`](../../src/devstudio/model/problemsmodel.cpp#L226))。読みは同時に 1 本で、途中で頼まれたら答えの後に 1 回だけ続けます
    ([`ProblemsSession::readNow()`](../../src/devstudio/model/problemsmodel.cpp#L238))。`get_status` に `problems` が無い engine(PB-1 の前)からは読みません。
  - revision を待たずに読む時: RL-1s の読み直しの答え(notice・拒否・commit)、Show the document の restore の commit、一覧に行がある間に `committed` を言う答え(render の編集など。RL-1s が `"committed"` を含む答えだけを parse する所で見る)の後です
    ([読み直しの答えの後に読む所](../../src/devstudio/view/mainwindow.cpp#L1287))。
  - 1 秒の間に誰も `get_status` を読まなければ、ドックが自分で送ります([`StatusPollMs`](../../src/devstudio/view/problemsdock.hpp#L62))。engine が自分の監視だけで出会う問題
    (`hot_reload` の source。shader の監視など)はこれで拾います。
  - Assets のドックの照合: 照合の結果を出すたびに hook `checked` で渡され([渡す所](../../src/devstudio/view/assetsdock.cpp#L717))、severity が `warning` / `error` の issue を行にします
    (`info` は入れません)。文は `<種類> against <manifest>: <message>` です([`assetProblemRows()`](../../src/devstudio/model/problemsmodel.cpp#L186))。
  - shader: engine の `shader_reload_failed@1` の行を正とし、Studio の shader の診断はそのままです(二重にしません)。
  - 入れないもの: 各 panel の編集の拒否(その場の notice のまま。engine の一覧にも入らない)と、engine が止まった後の致命の行(そのとき `get_problems` は無い)。
  - engine が切れると engine の行を消し、照合の行は残します。
- **クリック**(決定 3。[`MainWindow::openProblem()`](../../src/devstudio/view/mainwindow.cpp#L1319)):
  - node を名指す行(`document_kind` が無いか `root_graph` / `definition`): source に関係なく Graph タブでその node を選び(definition の中なら潜る)、詳細欄を開きます。
    Fix in は `Graph: <node>` です([node の行の飛び先を Graph にする所(RR-1s)](../../src/devstudio/model/problemsmodel.cpp#L124))。Graph がまだ文書を読んでいなければ、読んだ時に選びます。
    Graph が definition の中の node を見せられないときだけ、Show the document を開いて文字の `"<node>"` にカーソルを置きます([definition の中の node を文字で開く所](../../src/devstudio/view/mainwindow.cpp#L1343))。
    ✅RR-1s(`471476d0`)の前は、source が `reload_rpc` / `hot_reload` の node は Show the document の文字の位置へ飛び、Fix in は `Text: <node>` でした(下の「仕様から外れた判断」の 1)。
    RR-1s から、reload に断られた後の Graph は engine がディスクの版を返す読み(RR-1 の `live: false`)を読み直すので、その node があります(下の「reload に断られた版を持つ間(✅RR-1s)」)。
  - 行と文書がある行(JSON でない文書など): Show the document をその行・列で開きます([`RenderFallbackNotice::showDocumentAt()`](../../src/devstudio/view/renderfallbacknotice.cpp#L193))。
    BS-1s と同じ窓で、行が無ければ `find`(node の id)の最初の位置、それも無ければ末尾にカーソルを置きます。
  - `document_kind: "root_rendering"`(`material_route_unserved@1` など)は Rendering settings タブ、`feature` は Features タブを前に出します。
  - `shader`: shader の file を外のエディタで開きます(BS-1s の Open file と同じ開き方)。行には飛べないので、status bar が `<document>: line <N>` を言います。
  - `material` の文書か `material.*` の code: Materials のドックを前に出し、その material を選びます([`MaterialsDock::revealMaterial()`](../../src/devstudio/view/materialsdock.cpp#L969))。
  - 照合の行: Assets のドックを前に出し、そのファイルの行を開いて選びます([`AssetsDock::revealFile()`](../../src/devstudio/view/assetsdock.cpp#L794))。
  - `render_feature_requires_restart@1` の行: クリックではどこにも飛ばず、選ぶと一覧の下に **Restart engine**(`.restart`)が出ます。押すと `requestEngineRestart()` です。
  - 行は押した時に飛びます(`itemPressed`。keyboard の Return は `itemActivated`)。同じ行の 600 ms 以内の 2 回目は飛びません([押下で飛ぶ所](../../src/devstudio/view/problemsdock.cpp#L175))。
- **Show the document の Save はいつも restore です**(✅RR-1s、`471476d0`): Problems から開いた窓も帯から開いた窓も、Save は `restore_render_documents` を
  `get_render_documents` が返した digest を base にして送ります([Save が restore を送る所(RR-1s)](../../src/devstudio/model/renderfallback.cpp#L234))。engine の ✅RR-1 から、reload に断られた後の engine の base は
  ディスクの版なので、その digest の restore が通ります。Studio は render の文書のファイルを自分で書きません(D0)。
  PB-1s(`93934318`)の着地時は、source が `startup` でない行から開いた窓の Save は restore を送らずにファイルを丸ごと書き、RL-1s の監視が拾って `reload_render_documents` を送っていました
  (書けなければ窓の notice が `Could not write <reference>: <理由>`。下の「仕様から外れた判断」の 2)。RR-1s でこの道(`RenderDocumentRepair::open()` の `to_file`、`Transport::write_file`、MainWindow の書く hook)は外しました。
- **件数**(決定 4): status bar の右に件数のボタン `pelican.problemsCount` があり、`2 errors, 1 warning` / `1 error` / `No problems` を言います
  ([`problemsCountLine()`](../../src/devstudio/model/problemsmodel.cpp#L214)、[ボタンを作る所](../../src/devstudio/view/mainwindow.cpp#L519))。押すと Problems が前に出ます。engine が無い間は隠れます。
  BS-1s の帯の文は末尾に ` See Problems.` が付きます(下の「組めない構成でも engine を止めない(✅BS-1s)」。Frame Plan の status 行の文には付けません)。
  ✅RR-1s から、reload に断られた版を engine が持つ間も同じ帯が出て、この一覧の最初の error の 1 文を言います(下の「reload に断られた版を持つ間(✅RR-1s)」)。
- **消える**(決定 5): 読むたびに engine の行を置き換えます。選んでいた行が残っていれば(id で)選択を保ち、消えたら選択も外れて `.detail` が隠れます。0 件なら `.count` は `No problems` です。
- **保存してから一覧に出るまで**: `--rpc` の engine は render 文書の保存を自分では読み直さないので(HW-1)、RL-1s の監視が `reload_render_documents` を送り、その答えの後に読みます。
  シナリオの実測(実装者の報告。ファイルを書いてから件数が変わるまで)は、構文の壊れで 397 ms、node の壊れで 492 ms、直した保存で行が消えるまで 713 ms / 92 ms で、大半は RL-1s の 300 ms のまとめです。
  実装者は、render 文書については Studio から見て PB-2(engine が自分で保存を拾う)は要らないと判断しています。
- **仕様から外れた判断**(実装者の報告):
  1. reload で断られた node は Graph ではなく文字の node の位置へ飛びます。reload が断られている間、Graph(`get_graph_instances`)は読み込み済みの保存前の版を返し、その間の編集は `external_modification` で断られるためです。
     → ✅RR-1(`4549cae8`)から、engine は断られたディスクの版を `get_graph_instances` などで `live: false` 付きで返し、その digest の編集が通ります(§13.5 の「reload に断られた版を持つ」)。Studio の飛び先はまだ変えていません。
     → ✅RR-1s(`471476d0`)で、reload で断られた node も Graph で選ぶ形に戻しました(上の「クリック」)。
  2. reload で断られた文書の Save はファイルを書きます(上)。engine の restore は「base の digest が読み込んだ版の digest」と「disk が読み込んだ版」の両方を求めるので、reload が断られた後の root の restore は必ず `external_modification` になる、というのが実装者の調べです(engine のコードはこの文書では未確認)。
     → コードで確かめました。RR-1 の前の restore の CAS は root を editor が持つ版の digest とディスクの両方で比べ([restore の CAS の root の比べ](../../src/core/communication/renderconfigeditor.cpp#L6184))、
     断られた後の editor は前の版を持っていたので、root の restore はどの digest でも `external_modification` でした。✅RR-1(`4549cae8`)から editor は断られたディスクの版を持つので、
     `get_render_documents` の root の `digest` を base にした restore で直せます(test)。Studio の Save はまだファイルを書く形のままです。
     → ✅RR-1s(`471476d0`)で Save は restore に戻り、ファイルを書く道は無くなりました(上の「Show the document の Save はいつも restore です」)。
  3. クリックは押下で飛びます。押した時に `.detail` が出て一覧の高さが変わると、Qt の click が来ないことがありました(test で再現)。
  4. Restart の行はクリックで再起動しません(一覧の下の Restart engine で)。
  5. 帯の See Problems は文の末尾に足すだけで、帯にボタンは足していません(押す先は status bar の件数)。
  6. 件数は engine が無い間は隠し、0 件は `No problems` です。
  7. hot reload の宣言(Assets のドック)はシナリオで使っていません(render 文書の保存は宣言に関係なく RL-1s が見る)。
  8. シナリオで起動時の fallback の node を見る段は、同じ project の開き直しが何もしないので、別の project を開いてから戻ります。
- **残した穴**(実装者の報告):
  - material / scene の `document_kind` はまだ engine が出しません。Materials のドックへ飛ぶ道は fake の test だけで、material の問題の `document` の形(path か名前か)は engine の実物で確かめる必要があります。
  - shader の行は外のエディタで file を開くだけで、行には飛べません(自動化の下では log に 1 行)。
  - `engine://` の文書(feature / preset の engine 側)の行は、Show the document を開いても engine の一覧に無いと断られます(窓に理由が出る)。
  - 1 秒の `get_status` は engine がいる間いつも回ります(他の panel が読んでいれば送らない)。
  - 照合の行は Assets のドックが照合の結果を出したときだけ変わるので、project を替えた直後に古い照合の行が一瞬残り得ます。
  - 一覧は読むたびに全部作り直します(数百件を超える一覧は想定していない)。
- **engine に回したもの**(実装者の報告):
  1. WP298 の例の起動(sponza と Alicia)が `glTF morph vertex metadata exceeds renderer capacity of 262144 static vertices` で止まる。PB-1 で `material_route_unserved@1` が致命でなくなり、起動が先へ進んで表に出た(下の test の WP298)。
     → ✅MV-1(`f84a7d14`)で engine 側を直しました。morph の頂点の記録は頂点 buffer と同じ件数で一緒に伸びるので、example に Alicia を足した起動はこの上限で止まりません([第5章](05_assets.md) の「モーフターゲット」)。
  2. `restore_render_documents` は reload が断られた後の root を直せない(上の 2)。disk の digest を base にした restore を受けるかは engine の判断待ち。
     → ✅RR-1(`4549cae8`)で engine が受けるようになり、Studio は ✅RR-1s(`471476d0`)からその restore を使います。
  3. reload が断られている間、`get_graph_instances` は保存前の版を返すので、disk の版の node を Graph で見せる道が無い。
     → ✅RR-1 で engine は断られたディスクの版を `live: false` で返し、✅RR-1s で Graph はその版の node を見せます。
  4. warning を出す道(MT-2 の解けない上書きなど)ができたら、fake の test を本物に替える。
- **手で打つ欄**: 絞り込みの欄 `.filter`(任意。空で全部見える)と、Show the document の窓で壊れた JSON を直す欄(BS-1s の既存。シナリオでは末尾の `}`)です。
- test: `devstudio_problems_test`(`QT_OFFSCREEN`。実物のドックに、test が hook で engine の代わりに答える)の
  [「PB-1s decision 1: one row a problem -- the severity, the code's sentence, the document, the line and column and where it is fixed -- sorted by severity or document, narrowed by severity and by words」](../../test/devstudio_problems_test.cpp#L188)・
  [「PB-1s decision 2: get_problems is read when get_status.problems.revision moves, and the contents check's warnings and errors are rows beside the engine's」](../../test/devstudio_problems_test.cpp#L242)・
  [「PB-1s decision 3 / RR-1s decisions 2 and 3: a click on a row goes where it is fixed -- the Graph's node for a start's refusal and a reload's alike, the document's text at the line or at the node, the shader file, the asset's row, the material, the rendering settings; Save restores the document」](../../test/devstudio_problems_test.cpp#L295)
  (✅RR-1s で名前と中身を書き換え。PB-1s の着地時は「PB-1s decision 3: a click on a row goes where it is fixed -- the Graph's node for a start's refusal, the document's text at the line or at the node, …」で、reload の node は文字の位置へ飛び、Save はファイルを書く期待でした)・
  [「PB-1s decision 4: the count says how many errors and warnings, and the placeholder's row says See Problems」](../../test/devstudio_problems_test.cpp#L392)・
  [「PB-1s decision 5: a row the engine no longer lists goes, and so does its selection」](../../test/devstudio_problems_test.cpp#L430)
  の 5 本。決定 6 は `devstudio_render_fallback_test` の書き換え(下の「組めない構成でも engine を止めない(✅BS-1s)」の再起動の案内)です。
  `devstudio_layoutpreset_test` はドックの数を 9 から 10 にし、古い保存済みの配置を戻しても Problems のドックが置き場を持つことを見ます。
  GPU の `devstudio_engine_failure_test` の WP298 の case は、止まる期待から fallback の期待(engine が止まらず、帯の文が `add a forward_transparent_v1 material pass`・`(material_route_unserved@1)`・`See Problems.` を含み、
  Problems の 1 行が source `startup`・`root_rendering`・Rendering settings)に書き換わりましたが、engine の morph の上限(上の engine に回したもの 1)で**赤のまま**です。
  → ✅MV-1(`f84a7d14`)でこの case は 2 本に分かれました。fatal の行を見る WP298 の case「Studio receives the running engine's fatal text while RPC stdout stays framed」は、example ではなく `test/fixtures/studio_automation_graph` の写しで `basic_config.camera` の `type` を `"fisheye"` にして起動し、
  project を読む段の `Pelican fatal error : basic_config.camera type must be 'perspective' or 'orthographic'`(rendering の文書より前なので `--render-fallback` でも致命)が Studio に届くことを見ます(対照は camera を戻した 600 frame の run)。
  上の fallback の期待は「The example with Alicia starts on its placeholder for a route no pass serves, and the band and the Problems row say why」に移り、example に Alicia を足した起動が止まらないことも見ます(test の詳細は[ソースコード読解ガイド 第7章](../source-code-guide/07_tools_rpc_tests.md) の `devstudio_engine_failure_test` の行)。
  実装者の run で `devstudio_problems_test` 5 / 5、`studio_cpu` 722 / 722 が緑です。
- シナリオ: `gui_problems_window`(Blank から GUI だけで。§13.5「Studio の自動化面」)。

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
  出します(名指すのは最初に見つかった引数です。[1 行を出す所](../../src/devstudio/viewport/embeddedviewport.cpp#L711))。
  同じ変数に `--start-state` があれば、それをそのまま使い、何も言いません。

**ボタンとキー**

- Gizmo の toolbar(`pelican.gizmoToolbar`。Move / Rotate / Scale の並び)の Scale の右に、区切りの後に **▶ Play**・
  **❚❚ Pause**・**▶| Step** が並び、その後ろに `View: Game | Editor`(edit と paused のとき)、**Play changes**、`⋯` の
  メニューが続きます([ボタンとキーを作る所](../../src/devstudio/view/playcontrols.cpp#L61)、
  [toolbar に置く所](../../src/devstudio/view/mainwindow.cpp#L1684))。
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
  `Viewport` の字の行は塗りません([塗る所](../../src/devstudio/view/mainwindow.cpp#L1878))。色は theme の token で、
  `play.playing_frame` / `play.playing_band` が Dark `#3d8fe0` / `#1f3f60`、Light `#2f7fd0` / `#b9d6f3`、
  `play.paused_frame` / `play.paused_band` が Dark `#e0ac00` / `#5e4b10`、Light `#c99200` / `#f3d98a` です
  ([theme の表](../../src/devstudio/resources/studio_themes.json#L84))。
- **読む時**: Studio には `get_status` を周期で読む時計がありません(Q-2 で止めてあります)。表示は次の読みで動きます
  ([`PlayStateModel`](../../src/devstudio/model/playstatemodel.hpp#L133)):
  - Studio の誰かが送った `get_status` の返事のすべて。engine の準備を確かめる最初の問い合わせも含みます。viewport が
    `engineStatusRead` で配ります([配る所](../../src/devstudio/viewport/embeddedviewport.cpp#L835))
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
  drag しても `set_time` を送りません([止める所](../../src/devstudio/model/graphcanvasmodel.cpp#L937))。edit と paused では
  scrub できます。

**再生中の scene の編集と Keep**(✅PM-3b)

再生中も Inspector・Outliner などの scene の編集はでき、engine はそれを記録します(`get_status.play.recorded_edits`)。
記録された編集は Stop で戻ります。残したい編集には Keep の印を付けます。

- **Inspector の行**: セッションの間に commit された編集は、Inspector の編集の行(`pelican.inspectorNotice`)の後ろに
  ` — reverts on Stop` が付き、右に **Keep** のリンク(`pelican.inspectorKeep`)が出ます。押すと Play changes でその編集に
  check が付き、行の後ろは ` — kept after Stop` になってリンクが消えます([行を組む所](../../src/devstudio/view/inspectorwidget.cpp#L1543))。
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
  ([`handleTakeFromGame()`](../../src/devstudio/model/inspectormodel.cpp#L3350))。commit すると行は
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
  [読み直す所](../../src/devstudio/view/mainwindow.cpp#L1926))。
- **未保存の扱い**: Stop は scene revision を戻さず、新しい revision で scene を戻します(§13.5「再生のセッション」)。そのままでは
  Stop のたびに未保存に見えるので、Studio は、Play の前(edit の間)に scene が保存済みで、Stop が 1 件も keep しなかったとき、
  Stop の後の最初の新しい revision を保存済みとして扱います([`InspectorModel::adoptRestoredRevisionAsSaved()`](../../src/devstudio/model/inspectormodel.cpp#L4160))。

**再生中に変わること**(✅PM-3b)

- **戻らない編集の帯**: セッションがある間、render-tools dock の上(Frame Plan・Features・Graph の 3 タブに共通の 1 本)と
  Pass Editor の上に、帯 `Changes here are written to files and stay after Stop.` が出ます。編集を送らなくても、セッションの間は
  出ています。帯の背景は theme の `play.paused_band` です([帯を作る所](../../src/devstudio/view/mainwindow.cpp#L1772))。
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
  ([`MainWindow::askToEndPlaySession()`](../../src/devstudio/view/mainwindow.cpp#L1832))。

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
  2 行です([行を足す所](../../src/devstudio/view/runtimeoutliner.cpp#L144))。
- Transform の値だけが変わった読みでは、行を作り直しません(開いたメニューや押している行が消えないように)。作り直すのは、
  object の組・名前・prefab・scene が変わったときです。
- 行を選ぶと、Inspector は欄・Add component・ray の一覧・グラフの装着の欄を隠し、枠 `Transform (runtime, read-only)`
  (`pelican.inspectorRuntime`)に `Position` / `Rotation (x, y, z, w)` / `Scale`(`local_trs`)と `World position`(`world_trs.pos`)を
  数だけで出します。選択の行は `<name> (runtime)` と `spawned from <prefab> while the game runs` の 2 行です。Take from game は
  出ません。次の読みで値が変わっていれば、数も追います([読むだけの表示](../../src/devstudio/view/inspectorwidget.cpp#L599))。
- **編集はできません**: runtime の行を選んでいる間は Outliner の Duplicate / Delete が押せず、tooltip が
  `Objects spawned while the game runs cannot be edited.` になります([`OutlinerTree::setEditRefusal()`](../../src/devstudio/view/outlinertree.cpp#L83))。
  runtime の行は掴んで動かせません。その上に object を落とすと断られ、同じ文が status bar(8 秒)と Outliner の下の 1 行に
  出ます。Outliner に rename はもともとありません。
- Stop(と engine の停止・再起動)で行は消え、runtime の行を選んでいたら Inspector は `None` に戻ります。
- **viewport の選択とはつながっていません**。viewport で pick すると runtime の行の選択は外れます。`pick_object` は model
  instance を返し、`get_runtime_objects` の `entity` を返さないためです([pick の所](../../src/devstudio/view/mainwindow.cpp#L2864))。

**分かっている穴**(着地時の記録。実装者の報告)

- engine の窓が keyboard の focus を持っている間は、play のキーが効きません(PM-3a)。
- `playing` の間の `frame N` は止まって見えます(上の「読む時」)。
- gizmo の drag と Graph attachment の編集は、Studio が送った編集でも Play changes では `Actor N · <transaction_id>` と出ます。
- ✅PM-2b(`fc7a0eb1`)の前は、model を描く scene で Keep を付けて Stop すると engine が assert で落ちました
  (slot の表の写しが free list の予約を落としていた)。そのため PM-3b の GPU シナリオは、model の無い
  `play_mode_roundtrip` と、model があり Keep を使わない `play_view_camera` に分けていました。✅PM-3d で、model のある fixture の `play_mode_roundtrip` 1 本にまとめ、Keep 付きの Stop の後も engine が描き続けることを見ています。
- 確認のダイアログの CPU の test は、Studio を閉じる場合だけです。
- セッションの間に自動化面の `quit` を送ると、確認のダイアログで止まっていました(30 秒で強制終了)。✅UX-7 から、自動化面の `quit` は Play / Pause の間なら先に keep なしで Stop を送り、run が終わってから閉じます(下の「Studio の自動化面」の「Play の間の `quit`(✅UX-7)」)。人が窓を閉じるときの確認は変わりません。

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
- コンポーネントの受理・正規化は [第4章](04_scene_ecs.md) §4.2 の component codec が担当します。値を書くと**必ず codec の正規形に書き直されて**保存されます。ここでいう**正規形**は、書いた JSON をいったん codec で読み(`decodeAuthored()`)、その値からもう一度書き出した(`encodeCanonical()`)形のことです —— 鍵の並び・省略した既定値・数値の型が一意に決まるので、同じ意味の JSON は必ず同じバイト列になります([componentcodec.hpp](../../src/core/loader/componentcodec.hpp#L342))。

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
([登録](../../src/core/communication/editorrpchandlers.cpp#L315))。`edit_procedural_asset` は
`base_source_digest` と `asset_name` を取り、編集は `aabb` / `record`、新規は `operation: "create"` と両方の値、
削除は `operation: "delete"` だけです。実行モデルはレンダー設定側(§13.5)と同じ digest CAS です。
✅MI-1 から、ほかの RPC が asset_data を書いて engine が publish した後も `external_modification` になりません(§13.5 の
「project の中の model ファイルを登録する・外す」)。

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

(`ticket` / `base_revision` / `committed_revision` / `transaction_id` の値は説明用です。鍵と並びは [実装](../../src/core/communication/editorjournal.cpp#L2610) のとおり。)

#### committed の応答に付く 2 つの鍵(✅OL-1)

`edit` / `undo` / `redo` の committed の応答には、どの op でも次の 2 つが付きます([`appendCommittedIdentities()`](../../src/core/communication/editorjournal.cpp#L2610))。

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
| `scene_outline` | `{scene_id?, if_scene_revision?}`(`if_scene_revision` は `scene_id` と一緒に) | authoring の object の軽い一覧 `{scene_id, scene_revision, not_modified, objects?: [{authoring_object_id, declaration_index, name?, parent_authoring_object_id}]}`。component は含まない。revision が合えば `objects` を省く(下の「一覧だけを読む `scene_outline`」) |
| `get_components` | `{authoring_object_id}` または `{name}` | オブジェクト 1 件の詳細(`declaration_index` 込み。**両方指定・両方省略はエラー**) |
| `get_scene_revision` | `{}` | `{scene_revision, preview_epoch, last_transaction, preview_lease}` |
| `list_assets` | `{store?}` | アセット一覧(id / 種別 / パス / store / 状態)。状態は `loaded`・`missing`・`declared`(inline の procedural)・`builtin`(engine の基本形、store `engine`) |
| `query_journal` | `{after_transaction_id?}` | 編集履歴(undo の種) |
| `can_edit` / `can_preview` | `{}` | 編集・プレビューの可否と理由 |
| `get_graph_instances` | `{}` | 共通グラフ文書の `source_reference` / `source_digest` / `instances[]`(`graph.instance` だけ)/ root の `nodes[]` 全体 / `definitions` 本体([get_graph_instances の実装](../../src/core/communication/renderconfigeditor.cpp#L2913))。✅CT-5(`65f3a8a3`)から `definition_instances: {<definition>: {owned_instances_only, instances: [{id, op, owned}]}}`: 各 definition を使う root の node です。`graph.instance` は `owner` があれば `owned: true`、`graph.loop` は分岐の body でその definition を回すもので、いつも `owned: false`(展開した写しは root に立つため)。`owned_instances_only` は使う node が全部 owned のとき true で、誰も使わない definition は `instances: []`・true です。`placement` が `owned_instance` の op(`pose.*`・`value.morph_weight`)をその definition の中に置けるかの目安です([CT-5 の `graphDefinitionInstances()`](../../src/core/communication/renderconfigeditor.cpp#L131)。test: `renderconfigeditor_test` の [「CT-5 get_graph_instances says which definitions only owned instances run」](../../test/renderconfigeditor_test.cpp#L1640))。✅RR-1(`4549cae8`)から `live: bool`(返した版が live の世代を組んだ版か)と、文法の壊れた版を持つ間の `held_unparsed: true`(§13.5 の「reload に断られた版を持つ」) |
| `get_graph_catalog` | `{}` | 登録済みグラフモジュールのカタログ(`pelican.graph_catalog` v1。`modules` などは `pelican_cli graph-catalog` と同じ値 — [第10章](10_tools.md) §10.4。✅EL-4 から RPC にだけ `material_pass_contracts` / `material_output_formats` が足される。EL-4 で同じく足した `engine_shaders` は ✅EL-4b で外れ、`get_engine_shaders` へ移った —— 下の「material pass の format と engine の shader も、engine の一覧から読めます(✅EL-4 / EL-4b)」)。✅RS-4 から、op の `outputs` の要素は `{port, type}` のほかに `{port, type_from_input}`(`type` の鍵は無く、名指した入力 socket に入る edge の型になる。`render.shadow_depth` の `depth`、`render.snapshot_copy` の `destination`)がある([第6章](06_rendering.md) §6.17)。✅EL-1 から、`modules[].types[]` の各 type に `arguments {<引数名>: {kind, required, values | min, max, default}}` が付く。`kind` は値の種類(`enum_value` / `boolean` / `signed_integer` / `unsigned_integer` など)、`required` は既定値が無いとき true、`values` は enum の取れる値、`min` / `max` は整数の範囲、`default` は省いたときの値(enum は文字列)。type の検査が照合するのと同じ schema の表から読む(`pelican.render` の型は `builtinRenderTypeSchemas()`、`render.image` の `type.arguments` を照合する表)ので、並んだ値はどれも通り、外の値は今までどおり `unsupported enum value: <値>` で断られる([arguments を組む所](../../src/project/graphmodule.cpp#L518))。既存の `parameters` はそのまま。✅CT-1 から、どの op にも `required_parameters`(`[{name}]` / `[{one_of: [...]}]`。必須の無い op は `[]`。`graph.render_node_parameters_missing@1` の検査と同じ表)が、`value.input` にだけ `input_names: [{name, type, description}]`(`time` / `delta_time` / `scene_time`)が付く([第6章](06_rendering.md) §6.17「catalog の読み方」)。✅CT-3(`0d215a68`)から、どの op にも `parameters: [{name, kind, required?, alternatives?, default?, enum?, min?, max?, items?, fields?, expression?, description?}]` と `parameters_complete` が付く。`required_parameters` と RS-9 の検査は同じ表の `required` から作り、`fields` の中の `required` は parser の条件の表示。`parameters_complete: false` は `render.image` / `render.fullscreen` / `render.raster` / `render.compute` / `render.material_pass`(`ports` の中の形などは出していない)。kind と書き方は [第6章](06_rendering.md) §6.17「各 op の parameter の形」。✅MP-1(`afd923e1`)から、`material_pass_contracts` の各 contract に `screen_inputs` / `surface_resources`(`[{name, source_type}]`。下の表)。✅CT-4(`4d682338`)から、`parameters` の要素に `choices_from?`(`{rpc, key, field?}` か `{graph_nodes: [<op>...]}`。値を選ぶ一覧の出どころ)・`asset_kind?` / `extensions?`・`soft_min?` / `soft_max?`(slider の範囲。検査しない)と、2 通りの形を取る値の kind `union` と `variants` が、各 op に `placement`(`anywhere` / `owned_instance`)が付く。`owned_instance` の op(`pose.*`・`value.morph_weight`)を `owner` 付きの `graph.instance` の外に置くと `graph.op_requires_owned_instance@1`(下の「グラフの装着を編集する」の名前付きの拒否)。意味と一覧は [第6章](06_rendering.md) §6.17 の CT-4 の段落 |
| `get_engine_shaders` | `{}`(✅EL-4b。field を足すと `-32602`) | ✅EL-4c からすぐ返り、`{status: "building"}`(反映の途中)/ `{status: "ready", engine_shaders: [...]}` / `{status: "failed", error}` のどれか。`engine_shaders` の要素は下の「material pass の format と engine の shader も、engine の一覧から読めます」の表の `engine_shaders` の行。反映は process に 1 つの worker thread が裏で 1 回だけ行い、ready になった後は process が終わるまで同じ答えを返す。frame と RPC は反映を待たない(EL-4b までは最初の呼び出しの中で反映し、その間 frame と RPC が止まっていた)。✅LC-1 から、reflection の push constant の range が `[112, 120)`(`pelican_loop.glsl` の回の番号)を含む shader の項目に `reads_loop_index: true`([reads_loop_index を付ける所](../../src/core/communication/engineauthoringlists.cpp#L266)。[第6章](06_rendering.md) §6.18「回の番号を shader に渡す」) |
| `get_shader_interface` | `{reference, refresh?}`(✅EL-5。`reference` は stem の参照、`refresh` は真偽。ほかの鍵・型違いは `-32602`) | すぐ返り、`{status: "building"}` / `{status: "ready", reference, stages: [...]}` / `{status: "failed", reference, error}` のどれか。`stages` の要素は `get_engine_shaders` の要素と同じ形で、project の shader の stem を stage ごとに反映したもの。stage の file が 1 つも無い stem はすぐ `failed` で `error: "shader_not_found"`。下の「project の shader の入出力を読む」 |
| `get_registered_providers` | `{}`(✅EL-6。field を足すと `-32602`) | `{draw_sort, pass_implementation, region_replacements, graph_transforms, render_strategy}`。各値は `[{name, source: "engine" \| "game", provider_version}]` を名前順で、今登録されている provider の名前。呼ぶたびに registry から読む。下の「登録済みの provider の名前を読む」 |
| `get_graph_values` | `{instance}` | その instance の入力 port の実効値 `{port, value, source, connected\|parameter}` と `published` / `generation` / `frame`。**publish 時の snapshot を読むだけで再評価しません** |
| `get_graph_node_values` | `{instance?}` | 展開木の全ノードの出力値(`{node, expanded, outputs}`)と `source:"rendering"`。`instance` 省略で root から |

`declaration_index` は scene の `objects` 配列における 0 始まりの宣言位置です。直リンクで読んだ木との対応には `(scene_id, declaration_index)` を使います。`authoring_object_id` はセッション内で安定した別の参照なので、挿入・並べ替え後は両者が一致するとは限らず、どちらも応答に残ります。

`get_components` が返すコンポーネントには、**`schema`(フィールド名・型・範囲・単位)と `editable` フラグ**が含まれます。インスペクタのウィジェットはこれだけで組み立てられます(§13.9)。

**ほかの field の値で読まれ方が変わる field(✅SC-1、`14e10b19`)。**`schema.fields[]` の要素は `{name, type, range?, unit?, enum?}` ですが、codec がある field を別の field の値によってだけ読むときは、その要素に次の鍵が足されます([鍵を足す所](../../src/core/communication/editorcommandservice.cpp#L100))。同じ鍵は `scene_tree` の各 component の `schema.fields` と、`get_component_add_candidates` の `schema_fields` にも出ます。

| 鍵 | 中身 |
|---|---|
| `discriminant` | 読まれ方を決める field の名前(今は `controller.type` だけ) |
| `applies_to` | この field を読む `discriminant` の値の列 |
| `required_for` | そのうち、この field が無いと codec が拒む値の列。無ければ鍵ごと出ません |
| `default` | `applies_to` のうち必須でない値で、書かなかったときに読まれる値。`applies_to` の全部で必須なら出ません |

今これを持つのは camera の `controller.*` だけです(ほかの component の field は変わりません)。controller の各 field の表は [第4章](04_scene_ecs.md) §4.2 の camera の項です。例(`get_components` の test の期待値): `{"name": "controller.sensitivity", "type": "f32", "range": [0.0, 1e12], "default": 1.0, "discriminant": "controller.type", "applies_to": ["orbit", "fly"], "required_for": ["fly"]}`。test: `editorcommandservice_test` の [「SC-1 get_components describes the camera controller by type」](../../test/editorcommandservice_test.cpp#L264)(`get_components` の要素と、`get_component_add_candidates` の `schema_fields` が同じであること)。

ここでいう**解決**は、オーサリング文書の生 JSON を codec に通し、既定値を補ってランタイムの値にするところまでを指します。範囲外の数値・未知の鍵・型違いなど、利用者が書いた内容が契約に反していれば解決は失敗します。

**解決に失敗したコンポーネントも消えません(✅WP378)。** authored / generated のどちらの直下にも
`resolution_error` が付き、`get_components` は**成功応答のまま**それを返します
([editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp#L1801))。形は 2 通りで、
typed な違反は `{code, path(RFC6901), fields, message}`、まだ typed 化していない旧分岐は
`{legacy_branch_id, message}` です(正は
[`resolutionFailureJson()`](../../src/core/loader/resolutionresult.hpp#L114))。
**`resolution_error` を含む応答はキー順が保存されます** —— editor 側のハンドラが「応答木のどこかに
`resolution_error` があるか」を見る述語 `embedsResolutionError` を登録時に添え
([editorrpchandlers.cpp](../../src/core/communication/editorrpchandlers.cpp#L50))、`RpcServer` はその述語が真を返した応答だけを
ordered JSON で直列化するためです([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1188))。

### 一覧だけを読む `scene_outline`

`scene_outline {}` は現在のsceneのauthoring objectを宣言順に返します。
`scene_outline {"scene_id":"main"}` でsceneを明示できます。
既存の `scene_tree` / `get_components` の応答は変わりません。

```json
{
  "scene_id": "main",
  "scene_revision": 41,
  "not_modified": false,
  "objects": [
    {"authoring_object_id": 7, "declaration_index": 0, "name": "Root", "parent_authoring_object_id": null},
    {"authoring_object_id": 9, "declaration_index": 1, "parent_authoring_object_id": 7}
  ]
}
```

- `authoring_object_id` は `get_components` と同じ安定IDで、0にはなりません。
- `objects` は宣言順です。`declaration_index` は0始まりで、識別子としては使いません。
- 親は名前ではなく `parent_authoring_object_id` で返し、親がない場合は `null` です。
- 名前がないobjectでは `name` の鍵を省きます。空文字や `null` は補いません。
- componentの詳細、schema、runtime値は含みません。選択時に `get_components` で取得します。
- Play中もauthoringの一覧です。実行中にspawnされたobjectは `get_runtime_objects` で取得します。

同じ一覧をpollするときは、受け取ったsceneとrevisionを両方指定できます。

```json
{"scene_id":"main","if_scene_revision":41}
```

一致した場合は `{"scene_id":"main","scene_revision":41,"not_modified":true}` を返し、
`objects` の鍵を省きます。不一致なら通常の一覧と `not_modified:false` を返します。
存在しないsceneはrevisionが一致していても `scene_not_found` です。
`if_scene_revision` だけの指定は `-32602` です。sceneの切り替えと空の一覧を
「変更なし」と混同しないよう、クライアントはsceneごとにキャッシュします。
engineへ再接続したときはキャッシュを捨て、条件なしで取得してください。
revisionはauthoringの変更を表し、runtimeやプレビュー値の監視には使いません。
`if_scene_revision` は0〜2^53−1の整数tokenです。未知のfieldや型違いも `-32602` です。

- 実装([`EditorCommandService::sceneOutline()`](../../src/core/communication/editorcommandservice.cpp#L794)、[登録 `scene_outline`](../../src/core/communication/editorrpchandlers.cpp#L77)): authoring の resolved scene の object の情報(id・宣言の位置・名前・親の名前)だけを読み、親の名前をその scene の名前から id に引き直します。object ごとの `queryObject`(component の JSON・schema・runtime の値を作る所)は呼びません(test が runtime と schema の問い合わせが 0 回であることを見ます)。`setHandler` で登録しているので、応答の鍵の順は保たれません(上の例の鍵の順は読みやすさのためです)。
- 作者と計測: 利用者の codex が書いた変更です(handoff 02、`b29ccfea`)。commit の説明では、10,000 object で 758 ms → 44 ms(Release、3 回)。計測の script `tools/scene_outline_benchmark.py` は、同じ Release の player で `scene_tree` と `scene_outline` を比べるものです(時間は pipe と engine の分と、client の JSON の decode の分を別に出します。GUI の操作の待ち時間ではありません。script の注記)。
- test: `editorcommandservice_test` の [「scene_outline returns stable hierarchy in declaration order without detail queries」](../../test/editorcommandservice_test.cpp#L1320)、[「scene_outline conditional reads require scene identity and observe new revisions」](../../test/editorcommandservice_test.cpp#L1380)、[「scene_outline rejects malformed selectors on the registered RPC wire」](../../test/editorcommandservice_test.cpp#L1433)。

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
| `set_view_camera` | `{camera: "editor" \| "game"}`(鍵は `camera` だけ) | `{camera, previous}`。同じ view への指示は何も変えずに成功 | `playing` では `-32000` の `play_state_mismatch`(`data` に `state` と `required: ["edit", "paused"]`)/ editor camera の無い起動で `"editor"` は `-32000` の `view_camera_unavailable`(`data` に `camera`)/ それ以外の値や鍵は `-32602`([`set_view_camera` の handler](../../src/core/communication/rpcserver.cpp#L2315)) |

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

`edit` から `playing` / `paused` へ移ると**再生のセッション**が始まり、`edit` へ戻るとセッションが終わって scene が再生の前に戻ります(設計は [design_play_mode.md](../design_play_mode.md) §6・§7。実体は [`set_play_state` の handler](../../src/core/communication/rpcserver.cpp#L2120) と [`PlaySession`](../../src/core/communication/playsession.hpp#L51))。Studio の Play / Stop と Keep は ✅PM-3b です(§13.1「Studio で game を再生・停止する」)。

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

`get_runtime_objects {}` は、今の scene で game が spawn した生きている object を、spawn された順(boundary、その中の index の順)に返します([handler](../../src/core/communication/rpcserver.cpp#L1380))。authoring の object は `scene_tree` / `get_components` で読み、spawn された object はこちらで読みます。field を足すと `-32602` です。

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
[handler](../../src/core/communication/rpcserver.cpp#L1380))。閉じた gate を越えるのは明示の要求だけで、
replay と `--strict-assets` の engine は今までどおり断ります([越えてよいかの判定](../../src/core/watch/reloadservice.cpp#L727) /
[`ReloadGate::explicitShaderReloadRefusal()`](../../src/core/watch/reloadgate.cpp#L66))。file watch の poll の経路は `--rpc` の下で
今までどおり何もしません。

**Studio の下で外の保存を反映するには `hot_reload` を宣言します(✅HW-1)。**上の「gate が `rpc driver` で閉じる」は、project.json に
`hot_reload` を書いていない project の話です。`hot_reload` に素材やフォルダ(例 `["assets/textures", "shaders"]`)を書くと、Studio の
engine(`--rpc`)でもそこだけは file watcher が監視し、外のツールで保存した texture・material の値・model・shader が
engine を起こし直さずに反映されます([gate の 1 か所](../../src/core/watch/reloadgate.cpp#L37))。書き方と拒否は
[第3章](03_project_format.md) の「hot reload で監視するもの」です。

- render 文書(rendering config / preset / feature)は宣言しても watcher からは反映されません。`--rpc` の下では Studio の
  `reload_render_documents`(下の「外のエディタで書き換えた render の文書を拾います」)が持ちます。
- watcher が先に shader を反映していれば、後から届く Studio の `reload_shaders` は `{committed: false, attempted: false}` になる、
  とコードからは読めます(HW-1 の報告。engine でこの順を見る test はありません。未確認)。✅HW-2s から、Studio は監視が
  開いているときのこの答えを watcher の読み直しとして扱います(§13.5「Studio のノードキャンバス」の「shader を外のエディタで
  保存すると画面が変わる」の「何も変わっていない」)。
- 宣言は engine の起動時に読みます。✅PJ-1(`89643cd7`)から、動いている engine には `edit_project_settings` で宣言を
  差し替えられ、engine を起こし直さずに watcher が作り直されます(下の「project.json の宣言を書き換える」)。
  ✅HW-2s(`78737fdf`)から、**Studio の Assets のドックで素材・フォルダの行ごとに宣言を切り替えられます**(§13.1 の
  「Studio の Assets のドック」)。project.json を手で書いたときは engine を起こし直します。宣言しているかどうかと監視の様子は `get_status.reload` の
  `watch` / `error` / `scans` / `hashed_files` / `hashed_bytes` で見えます([第10章](10_tools.md) §10.5)。

- **いつ効くか**: RPC は frame 境界で処理されるので、handler の中で即時に準備・commit し、新しい shader は次の frame の
  `render_start` で使われます(fullscreen pass の入力もそこで bind し直します)。preview 中なら先に preview を中止します。
- **何を読み直すか**: SPIR-V の sha256 が公開中と違う unit だけです([`ShaderLibrary::prepareReloadChanged()`](../../src/core/shader/shaderlibrary.cpp#L1214))。
  ただし前回の読み直しが失敗した unit は、中身が変わっていなくても 1 回公開し直します(それで失敗の記録が消えます)。
  判定のために compile はするので、呼ぶたびに全 unit を compile し直します(cache が効くので軽い。大きな project での時間は未測定)。
- Studio は shader file の保存を見て自分でこれを呼びます(§13.5「Studio のノードキャンバス」の「shader を外のエディタで保存すると画面が変わる」)。

**pass が新しい shader を断ることがあります(✅VI-1b)。**読み直しは、fullscreen pass と入力を持つ raster pass の
新しい pipeline ごとに、その pass が渡している入力を新しい shader の set 1 の宣言と照合します。規則は編集と load の
照合と同じ関数です(Q-14e / Q-14f / VI-1 の可変の規則。下の「fullscreen pass の入力と shader の宣言が合わない拒否」。
[読み直しの候補の照合](../../src/core/fullscreenpass/fullscreenpasscontainer.cpp#L749))。合わない pass の新しい pipeline は
公開せず、その pass は**前の pipeline と前の descriptor set で描き続けます**。同じ読み直しのほかの pipeline は公開されます
([候補を外す所](../../src/core/shader/pipelinefactory.cpp#L2014))。照合する経路は 3 つで、`reload_shaders`、file watch の
shader の batch、render pipeline の読み直しが shader の変更を一緒に抱える経路のどれも同じ照合を通ります
([結果を reload の記録へ渡す所](../../src/core/watch/reloadservice.cpp#L54))。

- **`reload_shaders` の答え**: 読み直し自体は公開されたので `committed: true` のままで(意味は変えていません。shader library は
  compile できた新しい shader を持ち、断られていない pipeline はそれで描きます)、`refused_passes` に断った pass が並びます
  ([答えを組む所](../../src/core/communication/rpcserver.cpp#L1421))。要素は `{pass, fragment, reason, shader_input_mismatch, declarations}`
  で、`reason` は下の拒否と同じ message、`shader_input_mismatch` は編集の拒否と同じ形(同じ関数で組みます)、`declarations` は
  **断られた新しい shader** の宣言 `[{binding, type, name, variadic}]` です([形](../../src/core/fullscreenpass/shaderinputmismatchjson.cpp#L48))。
  `committed: false` の答え(何も変わっていない)には `refused_passes` は付きません。
- **reload status**: `get_status` の `reload.runtime["pelican.shaders"]` は、この読み直しを `attempted` / `applied` / `failed` の
  3 つとも 1 つ増やします(公開はしたが、断った pass があるので失敗としても数える)。`error_code` は **`shader_reload_refused@1`**
  ([code](../../src/core/watch/reloadservice.hpp#L45))、`last_error` は pass ごとに
  `Shader reload refused for pass '<pass>', which keeps drawing with its previous shader: <reason>`(複数なら改行でつなぐ)、
  `error_fields.refused_passes` は上と同じ要素の配列です。file watch の経路でも同じものが残ります。
- **`details.refused_passes`**: 同じ status の `details` には、**今も前の shader で描いている pass の全部**が同じ形で並びます
  ([details に足す所](../../src/core/watch/reloadservice.cpp#L760))。`last_error` は後の(別の shader の)読み直しが成功すると
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

### project.json の宣言を書き換える(✅PJ-1)

| メソッド | params | 返すもの | 拒否(`error.data` の `{error_code, error_fields}`) |
|---|---|---|---|
| `get_project_settings` | `{}` | `{project_digest, hot_reload, asset_stores}`。2 つの値は project.json に書かれているままで、鍵が無ければ `null` | なし |
| `edit_project_settings` | `{base_project_digest, hot_reload?, asset_stores?}`(ほかの鍵は `-32602`) | `{committed: true, changed, previous_project_digest, project_digest, applied, previous, asset_stores_change, assets_verification, restart_required}` | `project.settings_change_unsupported@1 {key, store?, reason}` / `external_modification` / `project.hot_reload_invalid@1` / `project.settings_invalid@1` / `project.settings_transaction_busy@1` |

Blank / Standard の雛形は `asset_stores` も `hot_reload` も宣言しないので、HW-1 の監視(上の段落)と AV-1 の照合は、
project.json を手で書いて engine を起こし直さないと効きませんでした。PJ-1 の 2 method は、その 2 つの鍵を engine に
書かせ、その場で反映します([handler の登録(PJ-1)](../../src/core/communication/projectsettingsrpc.cpp#L253))。
鍵を省けば今の値のまま、`null` で鍵ごと消します。書き込みは project の文書の transaction で `base_project_digest` を CAS に
使い、ほかの鍵と並びは保ちます。反映は resolver の store と宣言の差し替え、file watcher の作り直し、asset の照合
(新しく宣言した manifest のファイルが無ければ書いて、再検証を始める)の 3 つです。

- 受けるのは store の追加と、manifest の無い store への manifest の宣言だけです。mount の移動と、起動時からある store・
  manifest の削除や差し替えは `project.settings_change_unsupported@1` で断ります(project.json は変わりません)。
- 戻すときは返り値の `previous` を同じ RPC で送ります。外せるのは、この実行中にこの RPC で足したものだけです。
- 1 回書くと project.json は 2 空白の indent に展開されるので、値を戻しても digest は戻りません。
- 照合が終わった通知は無いので、`get_status.assets_verification.state` を見ます。
- 2026-10-03 に利用者が「Studio のボタンで宣言を足す」形を承認しました(HW-2 / AV-1s)。✅HW-2s / AV-1s(`78737fdf`)から、
  **Studio の Assets のドックがこの RPC を呼びます**(行ごとの Reload when saved outside、Track asset contents…、その Undo。
  §13.1 の「Studio の Assets のドック」)。

返り値の各鍵・拒否の条件・残る穴は [第10章](10_tools.md) §10.3 の「project.json の宣言を書き換える」、受ける変更と断る変更の
規則は [第3章](03_project_format.md) §3.4 の「起動中の engine に宣言を足す」です。

### material 文書を読み書きする(✅MT-1)

| メソッド | params | 返すもの | 拒否(`error.data` の `{error_code, error_fields}`) |
|---|---|---|---|
| `list_materials` | `{}` | `{asset_data_digest, documents: [{path, material_digest, status, reason?, materials: [{name, surface, tags, id, registered}]}]}` | なし |
| `get_material` | `{path, name?}` | `{path, material_digest, name, names, registered, material}`(`material` は文書のままの object) | `material.document_unknown@1` / `material.document_invalid@1` / `material.name_unknown@1` |
| `get_surface_schema` | `{surface}` | `{surface, params: [{name, type, default, min?, max?, hint?, encoding}], textures: [{name, default, color_space, role, dimension}]}` | `material.surface_unknown@1` |
| `list_surfaces`(✅MT-1b) | `{}`(ほかは `-32602`) | `{surfaces: [{reference, source: "engine" \| "project", name?, params, textures, routing?: {alpha_mode, double_sided}, render_state?: {blend, cull, depth_test, depth_write, depth_compare}, error?}]}` | なし(読めない surface は `error` 付きで並ぶ) |
| `get_material_bindings` | `{model}` | `{model, material_bindings, bindings_digest, primitives: [{mesh, primitive, slot, material, usd_path}], objects, object_overrides}`(`objects` / `object_overrides` は ✅MT-2) | `material.model_unknown@1` / `material.binding_invalid@1` |
| `edit_material` | `{base_material_digest, path, name?, values?, textures?, tags?, routing?, variants?, surface?}` | `{committed: true, changed, path, name, material_digest, previous_material_digest, previous, applied: "live" \| "rebuilt" \| "restart_required", restart_required, rewritten_bindings?, rebuilt_models?}`(`rewritten_bindings` は `changed: true` のとき必ず、`rebuilt_models` は `rebuilt` のとき。どちらも ✅MT-3) | `external_modification` / `material.value_invalid@1` / `material.param_unknown@1` / `material.document_unknown@1` / `material.document_invalid@1` / `material.name_unknown@1` / `material.surface_unknown@1` / `material.transaction_busy@1` / `material.binding_invalid@1`(✅MT-3) / `material.routing_conflict@1`(✅MT-3) |
| `create_material` | `{path, surface, name?, values?, tags?, routing?}` | `{committed: true, changed: true, path, name, material_digest, asset_data_digest, previous_asset_data_digest, applied, restart_required}` | `material.path_invalid@1` / `material.path_conflict@1` / `material.name_invalid@1` / `material.name_conflict@1` / `material.surface_unknown@1` / `material.value_invalid@1` / `material.param_unknown@1` / `material.document_invalid@1` / `external_modification` / `material.transaction_busy@1` |
| `bind_material` | `{model, material, primitive?: {mesh, primitive} \| slot?}` か `{model, bindings: [{mesh, primitive, material}]}` | `{committed: true, changed, model, material_bindings, bindings_digest, previous_bindings_digest, previous: {bindings}, asset_data_digest?, previous_asset_data_digest?, applied, restart_required}` | `material.model_unknown@1` / `material.binding_invalid@1` / `material.name_unknown@1` / `material.document_invalid@1` / `external_modification` / `material.transaction_busy@1` |
| `set_object_material`(✅MT-2) | `{actor_id, object, scene_id?, base_scene_revision?, material: 名前 \| null, primitive?: {mesh, primitive} \| slot?}` か `{actor_id, object, scene_id?, base_scene_revision?, materials: [{material: 名前 \| null, primitive?}]}` | `edit` の答え + `{changed: true, object, scene_id, model, previous: {materials: [{material \| null, primitive?}]}}`。変化が無ければ `edit` を出さず `{changed: false, object, scene_id, model, previous}` | `material.object_unknown@1` / `material.model_unknown@1` / `material.name_unknown@1` / `material.binding_invalid@1` |
| `get_object_materials`(✅MT-2) | `{object, scene_id?}` | `{object, scene_id, model, materials: [{row_id, material, primitive?, resolved}], primitives: [{mesh, primitive, slot, material, source: "object" \| "model", model_material, drawn \| null}]}` | `material.object_unknown@1` / `material.model_unknown@1` |

MT-1(`976c410d`)の前は、Blank の project で物体の色を変えるには JSON を 3 つ手で書いて engine を起こし直すしかありませんでした
(仕様書の記述)。7 本は material 文書・`.surface` の宣言・model の割り当てを engine に読ませて書かせます
([MT-1 の 7 method の登録](../../src/core/communication/materialdocumentrpc.cpp#L1979))。

- 書く 3 本は、読んだときの digest を CAS に project の文書の transaction で書き、返る前に engine に当てます。
  当てられないときは書いたうえで `applied: "restart_required"` と `restart_required: {reason}` を返します。
- **`edit_material` は `values` だけなら live** です(material の値の reload で、次の frame から効く)。`textures` / `tags` /
  `routing` / `variants` / `surface` が実際に変われば、✅MT-3(`c6f94eb7`)から engine はその material を登録し直し、前の登録で
  描いていた model をその場で組み直して `applied: "rebuilt"`・`rebuilt_models` を返します(MT-3 の前は書いたうえで restart_required)。
  engine が登録していない material、要る container の無い host、登録し直しか組み直しの失敗は、書いたうえで restart_required です。
- **`routing` の変更は binding も書き直します(✅MT-3)**: その material を指す asset_data の `models[]` の material_bindings の行の
  `routing` を同じ transaction で書き、答えの `rewritten_bindings: [{model, material_bindings, bindings_digest, previous_bindings_digest}]`
  (無ければ `[]`)で知らせます。`previous` を送り返すと binding も戻ります。割り当て済みの material の `routing` を外す要求は
  `material.routing_conflict@1 {path, material, routing: null, bound_by: [{model, routing}]}` で断り、何も書きません。
  `bind_material` が binding に書く routing は、MT-3 から material の文書の値です。
- **`surface` か `routing` の片方だけなら、もう片方を合わせます(✅VR-1、`45864d55`)**: engine の OpenPBR の surface は manifest の
  variant ごとに routing と組で、合わない組は `material.document_invalid@1` です。`surface` だけを送ると、その surface が manifest に載り
  material が `routing` を持つとき `routing` をその variant の値に、`routing` だけ(object)を送ると、今の surface が manifest に載るとき
  同じ manifest でその routing の variant の surface に替えて、同じ edit で書きます([`pairSurfaceVariant()`](../../src/core/communication/materialdocumentrpc.cpp#L935))。
  補った鍵も `previous` に入るので、送り返すと両方が戻ります。補った routing が前と違えば MT-3 の binding の書き直しと `rebuilt` も
  今までどおりです。両方を送ったときは送られたとおりで、組でなければ断り、何も書きません。project の surface と、`routing` を持たない
  material には補いません。VR-1 の前は片方だけの変更が同じ code で断られ、Studio から surface を替えられませんでした(実装者の報告)。
- `create_material` は文書と asset_data の `materials[]` を 1 つの transaction で書き、live で登録します。
- `bind_material` は model の material_bindings を書いて model を読み直します。割り当ては **model 単位**で、
  `engine://primitives/cube` のような宣言の無い基本形には asset_data の `models[]` に宣言を足します。
- **object ごとの上書き(✅MT-2、`44b7b50a`)**: `set_object_material` は scene の object 1 つだけの material を、その
  `simplemodelview.materials`(第4章 §4.2)に書きます。model の割り当てが既定で、効く順は「その primitive の行」>「全 primitive の行」>
  model の既定です。書くのは 1 本の `edit`(`base_revision` の CAS、undo / redo)で、`actor_id` は必須、`base_scene_revision` を省くと
  今の revision です。`slot` はその glTF の material を持つ primitive の行に展開して保存します。戻すときは返り値の `previous.materials` を
  `materials` に送ります。`get_object_materials` は primitive ごとの実効の material・出所(`object` / `model`)・model の割り当て
  (`model_material`)・今描いている material(`drawn`)を返します
  ([MT-2 の 2 method の登録](../../src/core/communication/editorrpchandlers.cpp#L179))。
- **surface の一覧(✅MT-1b、`a6373c01`)**: `list_surfaces` は engine の登録済みの `surfaces/**.surface` と、project の下の
  `*.surface`(隠しフォルダと symlink は入らない)を並べます。数は `get_surface_schema` と同じ読み方で、OpenPBR の manifest の variant に
  載る surface には `name` と `routing`、`//! render_state:` を宣言する surface には `render_state` が付きます。
- 戻すときは返り値の `previous` を同じ RPC で送ります(`edit_material` は新しい `material_digest` で、`bind_material` は
  `previous.bindings` を `bindings` に)。`delete_material` はありません。
- ✅MT-2b(`213562bb`)から、`bind_material` で送り返した bindings が全 primitive を model の元の material(glTF の slot、基本形は
  `pelican.primitive.default`)に戻すときは、binding を書かずに、bindings の文書・asset_data の `material_bindings` の鍵・MT-1 が基本形に
  足した `models[]` の項目を 1 つの transaction で外し、model を読み直します(返り値の `material_bindings` / `bindings_digest` は `null`。
  宣言が無い状態で同じものを送ると `changed: false`)。一部の primitive だけを既定に戻すときは、今までどおり slot の名前の行を書きます。
- `material.object_unknown@1` の `error_fields` は ✅MT-2b から `{object, scene_id?, name?, reason}` です(`scene_id` は object が
  見つかったとき、`name` はさらに authoring の名前があるとき)。上の表は code だけで、各 code の fields は [第10章](10_tools.md) §10.3 の表です。
- `edit_batch` の対象ではありません。material の文書は RD-1 の render 文書の集合の外にあるためです(commit の記録)。
- ✅MT-1s(`2d81ef51`)から、**Studio の Materials のドックと Inspector の Material の行がこの 7 本を呼びます**(surface の一覧は ✅MT-1b の
  `list_surfaces`。§13.1 の「Studio の Materials のドック」)。MT-1s の前は、`src/devstudio` に呼ぶ所がありませんでした。
  ✅MT-2s(`7112072e`)から、Inspector の Material の行は既定で `set_object_material`(Inspector の scene の編集の actor で送り、scene の Undo で
  戻る)と `get_object_materials` を呼び、`get_material_bindings` の `objects` / `object_overrides` を「N objects, M with their own material」に使います。

各鍵の意味・拒否の条件の表・実例は [第10章](10_tools.md) §10.3 の「material 文書を読み書きする」、live / rebuilt / restart_required の
規則と残る穴は [第6章](06_rendering.md) §6.7 の「RPC で material を読み書きする」、engine が書くファイルの形は
[第5章](05_assets.md) §5.2 です。

### project の中の model ファイルを登録する・外す(✅MI-1)

| メソッド | params | 返すもの | 拒否(`error.data` の `{error_code, error_fields}`) |
|---|---|---|---|
| `register_model` | `{path, name?}`(ほかの鍵は `-32602`) | `{committed: true, name, path, asset_data_digest, previous_asset_data_digest, applied: "live" \| "restart_required", restart_required: {reason} \| null}` | `model.path_invalid@1` / `model.format_unsupported@1` / `model.file_missing@1` / `model.path_conflict@1` / `model.name_invalid@1` / `model.name_conflict@1` / `model.load_failed@1` / `model.transaction_busy@1` / `external_modification` |
| `unregister_model` | `{name}`(ほかの鍵は `-32602`) | `{committed: true, name, removed: {name, path, material_bindings?}, asset_data_digest, previous_asset_data_digest, applied, restart_required}` | `model.name_invalid@1` / `model.unknown@1` / `model.in_use@1` / `model.transaction_busy@1` / `external_modification` |

MI-1(`61600f9e`)の前は、Blank から GUI で置ける model は engine の基本形(cube / plane / sphere)だけで、外の glb を使うには
ファイルを手でコピーし、asset_data の `models[]` を手で書いて engine を起こし直すしかありませんでした(仕様書の記述)。2 本は、
**project の中に既にある** `.glb` / `.gltf` / `.vrm` を `models[]` に載せて engine にその場で登録し、外すときは `models[]` から外して
engine からも外します([MI-1 の 2 method の登録](../../src/core/communication/materialdocumentrpc.cpp#L2009))。

- **ファイルのコピーは呼び手の役目です**(D0。Studio は project の文書を直接書かず RPC だけを使い、ファイルのコピーは Studio がする。
  音・画像の取り込みと同じ分担。仕様書の記述)。engine は project の外の path を受けず、model のファイルを書きも消しもしません。
- `path` は project 相対(`project://` は外して書く)。`name` を省くとファイル名の stem で、既にある名前と重なれば
  `model.name_conflict@1` です(番号を振るのは呼び手)。同じファイルが既に登録されていれば `model.path_conflict@1 {path, name}` の
  `name` が既にある登録の名前なので、呼び手はそれをそのまま使えます。
- 書く前に、起動時に `models[]` の entry を読むのと同じ検査([`ModelAssetContainer::preflightModel()`](../../src/core/asset/model.cpp#L936)。
  宣言を解き、ファイルを読み、`GltfLoader::inspect()`、glTF の material の名前と project の material の照合。GPU は確保しない)を
  通します。壊れた glb は `model.load_failed@1` で、asset_data は変わりません。
- 書き込みは asset_data の置き換え 1 件の transaction で、CAS の digest は handler が読んだものです(呼び手は渡さない)。
  鍵の順と改行コードは保ちます。
- **live と restart_required**:
  - `register_model` は、書いた後に engine の asset_data の写しを替え、[`ModelAssetContainer::registerModel()`](../../src/core/asset/model.cpp#L943)
    で 1 件登録できれば `applied: "live"` です(container がまだ無い host は、替えた写しから container を作るので live)。
    写しを替えられないか登録が失敗すれば、書いたうえで `restart_required` です。
  - `unregister_model` は、model に animation の rig が無く、生きている instance が描いていなければ engine から外して live です
    ([`ModelAssetContainer::unregisterModel()`](../../src/core/asset/model.cpp#L970))。rig のある model(skin を持つ model。VRM はほぼこれ)と、
    文書に無い instance(game logic が spawn したものなど)が描いている model は、外さずに `restart_required`
    (`the engine keeps the model until it starts again: …`)です。どちらでも `list_assets` からは消えます。
  - 前の `unregister_model` が restart_required で engine に残っている model を、同じ名前・同じファイルで登録し直すと、残っているものを使って live です。
- **戻し方**: `unregister_model` の答えの `removed` から `register_model {path: removed.path, name: removed.name}` を呼びます。
  `removed.material_bindings` は戻りません(bindings を持つ model は `bind_material` を呼び直す)。scene の object が描いている model の
  `unregister_model` は `model.in_use@1 {name, count, objects}` で断られるので、先に object を消します。
- **edit の ticket の `payload.model`**: `models[]` に無い名前を `simplemodelview.model` に書いた `edit`(`spawn` など)は、今までどおり
  ticket の後に `error.code: "method_unavailable"` で失敗します。✅MI-1 から、その `error.payload` に `model` と `reason: "model_unknown"` が
  付きます([`editorjournal.cpp` の MI-1 の payload](../../src/core/communication/editorjournal.cpp#L1715))。code の名前は変えていないので、
  呼び手は文(`unknown model asset: <name>`)を読まずに `reason` で「model が未登録」と分かります。実例(test。登録する前の `checker_box` を spawn):

  ```json
  {"status": "failed", "error": {"code": "method_unavailable", "message": "unknown model asset: checker_box",
   "payload": {"adapter": "/adapters/object_lifecycle.default_scene.3", "method": "spawn",
               "model": "checker_box", "reason": "model_unknown"}}}
  ```

- **procedural asset の editor が publish 済みの asset_data を採る**: `get_procedural_assets` / `edit_procedural_asset`(§13.3)の editor は、
  asset_data の bytes を editor を作ったときに 1 回だけ持ち、ファイルの digest と比べていました。そのため、ほかの RPC
  (MT-1 の `create_material` / `bind_material` も)が asset_data を書いた後は、`edit_procedural_asset` が `external_modification` で
  断られ続けていました(実装者がコードで確かめた)。✅MI-1 から、`get` / `edit` の前にファイルを読み、その bytes を読み込んだものが
  engine の publish 済みの写しと同じなら、それを新しい元として採ります([`adoptPublishedSource()`](../../src/core/communication/proceduralasseteditor.cpp#L25))。
  engine が publish していない外からの書き換えは、今までどおり `external_modification` です。test では `register_model` の後の
  `get_procedural_assets` が `external_modification: false` と新しい digest を返し、その digest の `edit_procedural_asset` が digest の
  検査を通ります。
- `edit_batch` の対象ではありません。
- **Studio からこの 2 本を呼ぶ画面はまだありません**(MI-1s で足す予定)。

各鍵・拒否の条件の表・実例・残る穴は [第10章](10_tools.md) §10.3 の「project の中の model ファイルを登録する・外す」、asset_data の
`models[]` の形は [第5章](05_assets.md) §5.2 です。

### レンダー設定を編集する

**シーン編集とは実行モデルが違います。**混ぜると壊れます。違う理由は対象が違うからです —— シーン編集の対象がメモリ上の `AuthoringSceneDocument`(正は `SceneRevision`)なのに対し、こちらは**ディスク上の rendering config ファイルそのもの**で、正はその sha256 digest です。したがって版の比較も、undo の有無も、書き込みの作法も別建てになります。

| メソッド | 何をするか |
|---|---|
| `get_render_features` | 著作 config の現在の `features[]`、`source_reference`、`source_digest`、runtime の状態を返す |
| `list_render_features` | **engine が可用性を判定した**カタログ。各項目に `available` が付き、**`unavailable_reason` は `available` が false の項目にだけ現れる**(使える項目には鍵ごと無い)。`source` はその文書の出所で、engine 内蔵なら `"engine"`、`project://packages/<ns>/` に vendoring された package のものなら `"package"` の 2 値だけです([catalog entry](../../src/core/communication/renderconfigeditor.hpp#L62))。読めない package 文書は**カタログから消えるのではなく** `available:false` + `unavailable_reason` として並びます([package の列挙](../../src/core/communication/renderconfigeditor.cpp#L879))。✅FR-1 から各項目に `required_targets: [{name, type, usage?}]`、✅FR-2 から `insertion: [{pass, anchor, position}]` が付きます(文書が読めない項目はどちらも `[]`。[FR-1 / FR-2 の 2 つの鍵を足す所](../../src/core/communication/renderconfigeditor.cpp#L1291)。下の「feature と snapshot を足す前に読む」)。✅FR-3 から `required_targets` の各要素に `needs_writer` が付きます |
| `edit_render_features` | `base_source_digest` と `operations[]` を受け、**チケットを返す**。結果は `get_edit_result` か `step_frame` の `edit_results[]` |
| `edit_render_features` / `materialize` | operationに`op: "materialize"`・`scope: "root"`または`"preset"`・`index`・`destination_reference`・`base_feature_digest`を指定。presetの場合は`base_preset_digest`も必要。具体値へ展開したfeatureを未作成のprojectファイルに保存し、元instanceをその参照へ置換する。featureのdigestは`get_render_features.feature_instances[].feature_document_digest`、presetのdigestは同entryの`source_digest`を使う。engine presetは先にforkする。Studio からの入口は Render Features の **Make editable...** |
| `get_scene_acceleration_structures` | 名前付きTLASの`definitions[]`、taskの接続先`task_connections[]`、編集元の`source_digest`、三角形座標系とレイアウトの編集可否・digestを含む`triangle_geometry`を返す |
| `edit_scene_acceleration_structures` | `base_source_digest`と`definitions[]`、任意の`task_connections` objectでTLASと接続先を置換。`triangle_space`と`base_layout_digest`を組で渡すとレイアウトの三角形座標系も同時保存し、既存の編集ticketを返す |
| `get_ray_callable_records` | taskごとのCallable schema、shader一覧、元宣言と実効初期値、rootの`source_digest`を返す |
| `edit_ray_callable_records` | `base_source_digest`と`task_records` objectで初期値の上書き指定全体を置換。既存の編集ticketを返す |
| `fork_render_pipeline` | `base_source_digest`・`base_preset_digest`・`destination_reference`で選択中presetを新しいprojectファイルへコピーし、rootの参照先も切り替える |
| `edit_region_replacement` | `base_source_digest`・`base_preset_digest`・`graph`・`region`でproject presetのタグ付き領域を指定。`provider`に登録済みprovider名を指定するとコンパイル時の差し替えを選択し、`null`なら差し替え要求を除去して元のノード構成を使用する。元のpass・設定・接続は保持する。現在の指定は`get_render_authoring_context`の各graphの`region_replacements`で取得できる。標準feature全体の展開操作ではない |
| `edit_preset_pass` | `base_source_digest`・`base_preset_digest`・`graph`・`pass_name`でproject presetのpassを指定。fullscreenは`input`・`output`・`shader`と任意の`ray_query`・`uses_light_data`・`push_constants`・`clear_colors`(後の 3 つは ✅G-3。省略は保持、`null` で削除。[G-3 の 3 field の受け取り](../../src/core/communication/renderconfigeditor.cpp#L4689))、materialは`input`・`output`・`ray_query`の1項目以上。rasterはさらに`shader`・`draw`・`raster_state`を部分編集できる。省略項目は保持、rasterの`raster_state: null`は既定状態へ戻す。書き先は 3 つ: project preset(`base_preset_digest`)/ project feature(`base_feature_digest`)/ **direct root 自身**(✅F-6。root が preset を選ばず、要求がどちらの digest も持たないとき。`base_source_digest` が root の digest を兼ね、root の `rendering.rendering_passes` の pass を編集する)。置換規則は 3 つとも同じ関数です([direct root の書き先](../../src/core/communication/renderconfigeditor.cpp#L4797)) |
| `get_compute_task_authoring` | compute/RT taskの元宣言・編集可否・保存先とdigestを取得する。各 task に出所の `origin`、graph node が作った task には `graph_node` が付く(✅MG-3b。下の「task の出所」) |
| `edit_compute_task` | `base_source_digest`・`base_task_source_digest`・`task_name`・`task`でtask宣言を置換する。名前は保持する |
| `add_compute_task` | `base_source_digest`・`destination_reference`・`task`で新規project featureにtaskを作り、rootのfeaturesへ追加する |
| `remove_compute_task` | `base_source_digest`・`base_task_source_digest`・`task_name`で元文書からtaskを削除する |
| `edit_render_targets` | `base_source_digest`・`ops[]`(✅F-1)。op は `{op:"add", target}` / `{op:"replace", name, target}`(`target.name` を変えれば改名)/ `{op:"remove", name}`。書くのは managed fragment `project://passes/authoring/render-targets.json` 1 つだけで、最初の要求で作って root の `features[]` に足す(空になっても消さない)。ops は順に当て、最後に 1 回 preflight して ticket を返す —— **全部入るか、何も入らないか**。名前付きの拒否は下の 6 種([edit_render_targets の実装](../../src/core/communication/renderconfigeditor.cpp#L5189)) |
| `add_authored_pass` | `base_source_digest`・`graph`・`insert`(authoring context の挿入位置の token)・`pass`。pass 1 つを managed fragment `project://passes/authoring/pass-<id>.json` に書き、root の `features[]` に足して preflight し、ticket を返す。受ける `type` は `fullscreen` と **`material`**(✅G-1)だけで、それ以外は `authored_pass_type_unavailable`(detail は `requested_type` と **`supported_types`**(配列)。[add_authored_pass の種類の門](../../src/core/communication/renderconfigeditor.cpp#L4920))。material pass の形の検査は下の段落。任意の **`material_route_default`**(`"auto"` / `"forward"`。✅G-4)を渡すと、pass と rendering 文書の root の既定を 1 つの ticket で変える(下の「material の route の既定」) |
| `set_material_route_default` | `base_source_digest`・`material_route_default`(`"auto"` / `"forward"`)。root の既定だけを変える。root 編集の ticket に乗り、commit の結果に `restart_required: true`(✅G-4。[`RenderConfigEditorService::setMaterialRouteDefault()`](../../src/core/communication/renderconfigeditor.cpp#L3906)) |
| `edit_rendering_keys` | `base_source_digest`・`set?`(`{キー: 値}`)・`remove?`(キーの配列)。root の rendering 節のうち、ほかの RPC が書かない 8 つのキー(`draw_sort` など)を 1 つの ticket で丸ごと書く / 消す(✅EL-3。下の「rendering 節のほかのキーを書く」) |
| `edit_target_planning` / `edit_vulkan_plan_pins` / `put_vulkan_physical_fragment` | root の rendering 節の `target_planning`(graph ごとの node の `serial` / `isolate` と資源の `no_alias` を名前単位で)・`vulkan_plan_pins`・`vulkan_physical_fragments` を 1 つの ticket で書く / 消す(✅EL-2。下の「graph 全体の計画の決定を書く」) |
| `get_fold_groups` / `fold_nodes` | `get_fold_groups {}` は root の graph 文書の `fold_groups` を group ごとに `{group, nodes, kind, digest, verified?, state}` で返す。`fold_nodes {base_source_digest, group, nodes, folded}` は group 1 つを宣言する / 置き換える / 消す(`folded: null`)1 つの ticket(✅FG-1。下の「断片の決定を宣言する」) |
| `verify_fold` | `base_source_digest`・`group`・`frames?`・`warmup_seconds?`・`tolerance?`。group の開いた形と決定ありの形を engine 自身の player に headless で描かせて比べ、通れば `folded.verified` を書く非同期の ticket(✅FG-2。下の「断片を検査して印を付ける」) |
| `bake_audio_analysis` | `asset`・`out`(`.ktx2`)・`settings?`。音の解析を worker で焼き、表と `<out>.import.json` を書く非同期の ticket。render 文書は書かない(✅AB-1。下の「音の解析を焼く」)。読む側が出力を開いている間は置き換えをやり直し、開かれたままなら `audio.analysis_failed@1` の `reason: "output_locked"`(✅AB-2) |
| `list_derived_assets` | `{}`。asset store の `*.import.json` が言う「何から作った file か」と、古いかどうかを配列で返す読み(✅AB-1。同上) |
| `edit_graph_document` | `base_source_digest`と`ops[]`で共通グラフ文書を編集する(§13.5「グラフの装着を編集する」)。ops は順に候補文書へ当て、最後に 1 回だけ展開する — **全部通るか、文書ごと拒否か**のどちらか。任意の **`material_route_default`**(`"auto"` / `"forward"`。✅RS-2)を渡すと、graph の変更と rendering 文書の root の既定を 1 つの ticket で変える(下の「material の route の既定」) |
| `edit_graph_instances` | `base_source_digest`・`attach[]`・`detach[]`。owner を持つ root instance の装着 / 取り外しだけを扱い、中身は `edit_graph_document` の ops へ翻訳される([edit_graph_instances の実装](../../src/core/communication/renderconfigeditor.cpp#L2942))。任意の `material_route_default`(✅RS-2)は `edit_graph_document` と同じ |
| `export_render_feature` | `scope`(`root` / `preset`)と`index`で feature 1 つを `user://components/<name>/` へ書き出す。`engine://` の feature は `render_component_engine_reference` で拒否([export の実装](../../src/core/communication/renderconfigeditor.cpp#L1464)) |
| `import_render_feature` | `base_source_digest`・`source_dir`(project 外の絶対パス)・`destination_reference`(未作成の`project://`)。component が要求する render target を project が宣言していなければ `render_component_requires_missing` |
| `install_package` | `source_dir`(project 外の絶対パス)。`<project>/packages/<namespace>/` へ複製して`project.json`に記録し、catalog を作り直す。返すのは`namespace`・`package_version`・`sha256`・`feature_references`・`feature_names` |
| `uninstall_package` | `namespace`。root config と選択中 preset の全文字列を走査し、まだ参照が残っていれば`package_in_use`(参照を列挙)で**何も消しません** |
| `decompose_material_pass` | `base_source_digest`・`base_preset_digest?`・`pass`。一体版の material pass を `render.material_pass` + draw_loop の `graph.loop` + body の definition に 1 つの ticket で書き換える(✅LP-3b。下の「ループと分解の RPC」) |
| `get_render_documents` / `restore_render_documents` | render 文書を丸ごと読む / CAS の下で丸ごと戻す(作る・消すを含む)。render の編集の undo の土台(✅RD-1 / RD-1b。下の「ループと分解の RPC」)。✅RR-1 から `get_render_documents` の答えは `{documents, live}` |
| `reload_render_documents` | `{}`。ディスクの root とそれが読む render 文書を読み直し、組み直して公開する ticket。ディスクには書かない(✅RL-1。下の「ループと分解の RPC」の「ディスクで書き換えた render 文書を取り込む」)。✅IA-2 から image の `initial` の asset の中身も比べ、committed の結果に `changed_assets` が付く |
| `get_graph_templates` | `{}`。engine が同梱する分解版の雛形(`engine://graphs/*.json`)を `{templates: [{reference, title, description?, decomposes, document}]}` で返す(✅GT-1。下の「ループと分解の RPC」) |
| `apply_graph_template` | `base_source_digest`・`base_preset_digest?`・`template`。雛形の nodes / definitions を root の graph へ写し、`decomposes` に従って feature を外すか `pass_replacements` を書き、`shader_defines` を足すのを 1 つの ticket で行う(✅GT-1。下の「ループと分解の RPC」) |
| `edit_batch` | `base_source_digest`・`edits[]`(`{method, params}`)。文書を書き換える 23 の編集 method(FR-3 の 20 に ✅FR-4 の `edit_preset_pass` / `edit_compute_task` / `remove_compute_task`)を順に 1 つの候補へ当て、組み立てと preflight を最後に 1 回だけ行う 1 つの ticket。RD-1 の `documents` も 1 回分で、`restore_render_documents` 1 回で戻る(✅FR-3。下の「複数の編集を 1 つの ticket で —— `edit_batch`」) |

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
([使用者](../../src/core/communication/renderconfigeditor.cpp#L772))。

読む側は `get_render_authoring_context` です。`render_targets[]` は合成後の全 target を
`{name, declaration, editable, source}` で返し、`editable` なもの(= fragment のもの)には fragment に書いてある宣言
そのもの `authored_declaration` が付きます。並びは editable なものが先です([組み立て](../../src/core/communication/renderconfigeditor.cpp#L2676))。
`source` は `authoring`(fragment)/ `root` / `preset` / `feature:<reference>` / `engine`(`display` など)で、
**文書にも合成器の出所記録にも出所が無いときだけ `composed`** になります(graph の合成で出来た target など。
[出所の判定](../../src/core/communication/renderconfigeditor.cpp#L711))。ほかに `render_target_fragment`(fragment の参照)と
`render_target_vocabulary`(`formats` / `usage` は parser 自身の表、`format_classes` は `scene` / `display` / `data` /
`explicit`)が付きます。fragment は pass を持たないので `managed_fragments` には出ません。
✅CT-4(`4d682338`)から `view_families`(`$main` と、登録された view family の provider の family の名前)と
`canonical_anchors`(合成が pass を置く 8 つの anchor。pipeline の順)も付きます。`get_graph_catalog` の parameter の
`choices_from` が指す一覧です([第6章](06_rendering.md) §6.17 の CT-4 の段落)。
✅CT-5(`65f3a8a3`)から `acceleration_structures`(ray_query の `acceleration_structure` に書ける名前。先頭が `"scene"`、その後に合成された
`scene_acceleration_structures` の各 `name`)と、`render_target_vocabulary.format_class_patterns`(引数を取る format class の書き方。今は
`[{name: "explicit", form: "explicit(<format>)", argument_choices_from: {rpc: "get_render_authoring_context", key: "render_target_vocabulary.formats"}}]`)
も付きます([第6章](06_rendering.md) §6.17 の CT-5 の段落)。

**draw_loop と region の差し替えが受ける名前も、context から読めます(✅EL-1)。**Studio が利用者に名前を打たせず、
engine の一覧から選ばせるための 3 つの鍵です([組み立て](../../src/core/communication/renderconfigeditor.cpp#L2716))。

| 鍵 | 中身 |
|---|---|
| `materials` | `[{id, name, tags, variants, builtin}]`。今動いている engine に登録された material(名前付き variant のための登録は除く)を `GlobalMaterialId` の昇順で([一覧を作る所](../../src/core/material/materialcontainer.cpp#L2408))。読み方は draw_loop の表が判定に使うものと同じです([`MaterialContainer::drawLoopSubject()`](../../src/core/material/materialcontainer.cpp#L2430))。`id` は `when.material` が照合するキーで、project の material 文書のものは `<文書>#<name>`、glTF のものは `<asset>#material[<i>]`、それ以外は登録名です([キーの作り方](../../src/project/materialdrawloop.cpp#L17))。`tags` は `when.tags` と `where` が見るもの、`variants` は `draw.variant` と `has_variant` が受ける名前(名前順)。engine 自身の material も並び、`builtin`(名前が `pelican.builtin.` で始まる)は常に付きます |
| `material_tags` | `materials` の `tags` の和。重複なし、辞書順 |
| `region_providers` | `[{name, builtin, provider_version}]`。`edit_region_replacement` の `provider` に書ける名前を、`resolveRegion` が名前で引くのと同じ表から名前順に([`SubgraphReplacementRegistry::providerListing()`](../../src/core/renderingpass/subgraphreplacementregistry.cpp#L1194))。game の provider が engine のものを同じ名前で覆っていれば、覆った方だけが出ます。`builtin` は engine が登録したもの(`builtin.identity_v1` など)。provider がどの region を受けるかは registry が持たないので出しません |

engine が動いていないとき(service に engine の一覧を読む口が無いとき)は 3 つとも `[]` です。既存の鍵は変わりません。
test: `engine_primitives_test` の
[「EL-1 get_render_authoring_context lists the materials a draw_loop judges, by the names it takes」](../../test/engine_primitives_test.cpp#L1724)
(GPU。tag と variant を持つ material 3 つを足し、一覧の `id` / `tags` / `variants` / `builtin`、`material_tags` の並び、
`builtin.identity_v1` を見てから、一覧の名前だけで `has_variant` / `tags` / `material` の分岐を書いた draw_loop を `edit_graph_document` で
commit し、`get_frame_plan` で各 material が名前で選んだ分岐に入ることを見る)と、`renderconfigeditor_test` の
[「EL-1 get_graph_catalog lists the argument values render.image accepts, and only those」](../../test/renderconfigeditor_test.cpp#L10386)
(`pelican.render.color_signal@1` と `pelican.render.depth@1` の `arguments` の値を 1 つずつ使った `render.image` を 1 回の編集で commit し、
一覧に無い値は文書を変えずに断られることを見る)。

**material pass の format と engine の shader も、engine の一覧から読めます(✅EL-4 / EL-4b)。**Studio が format や shader の名前を打たせず、
engine の一覧から選ばせるための読みです。`get_graph_catalog` に 2 つの鍵が足され([足す所](../../src/core/communication/renderconfigeditor.cpp#L3790))、engine の shader の一覧は読むだけの RPC `get_engine_shaders` が返し([返す所](../../src/core/communication/renderconfigeditor.cpp#L3802)、[登録](../../src/core/communication/editorrpchandlers.cpp#L301))、
この context の `render_target_vocabulary` に `format_aspects` が足されます。どれも engine が検査に使う表から作り、検査の側もその表を使うように寄せてあります([一覧を作る所](../../src/core/communication/engineauthoringlists.hpp#L13))。

**`get_engine_shaders {}`(✅EL-4b、`3a7df62d`。✅EL-4c、`8e00ae90` から答えに `status`)。**params は空の object だけで、field を足すと `-32602`(`get_engine_shaders contains unsupported field '<鍵>'`)。呼び出しはすぐ返り、答えは次の 3 つの形のどれかです([答えを作る所](../../src/core/communication/engineauthoringlists.cpp#L404))。

- `{status: "building"}` —— 反映の途中です。聞き直せば、出来上がった後は `ready` か `failed` が返ります
- `{status: "ready", engine_shaders: [...]}` —— 要素は下の表の `engine_shaders` の行と同じです。出来上がった後は process が終わるまで同じ答えです
- `{status: "failed", error}` —— worker 自体が失敗したとき(thread を作れない、反映の途中の例外)。shader 1 本が反映できないのはその要素の `reflection_error` で、`failed` にはなりません

反映は process に 1 つの worker thread が行います([worker](../../src/core/communication/engineauthoringlists.cpp#L346))。engine は RPC の endpoint を作った直後に worker を裏で始め(Studio が聞く前に一覧が出来ているように。[始める所](../../src/core/communication/rpcserver.cpp#L2823))、endpoint が閉じるときに止めて join します([止める所](../../src/core/communication/rpcserver.cpp#L2828))。止めるのは shader 1 本ごとの区切りで、作りかけの一覧は捨て、次に始めるときは最初から作ります([1 本ごとに見る所](../../src/core/communication/engineauthoringlists.cpp#L334))。worker が動いておらず答えも無いときに `get_engine_shaders` が来れば、その呼び出しが worker を始めて `building` を返します。worker は自分専用の compiler([`ShaderCompiler::detached()`](../../src/core/shader/shadercompiler.hpp#L91))を使います。この compiler は module を読まず、project の disk の shader cache を持たず、`StartupMetrics` に記録しないので、反映は `get_status` の `startup`(shader の時間と cache の数)に入らず、frame の側の compiler の cache とも共有しません。読むのは engine に組み込まれた shader だけです。

EL-4b までは、一覧を process の最初の呼び出しの中で作っていました(engine の GLSL の source を compile して反映します。commit の記録では十数本で、その間 frame と RPC が数秒止まっていました)。EL-4 では同じ一覧を `get_graph_catalog` の最初の呼び出しの中で作っていたので、起動時に catalog を読む Studio の次の `get_status` が 5 秒で timeout していました(commit の記録)。EL-4b から `get_graph_catalog` は反映も compile もしません。✅UX-5d(`fe276fc4`)から Studio の Graph は、`building` を受けると 500 ms 後に poll から聞き直し、`failed` は `error` の文を notice に出して一覧を空として扱います(palette の「Reading...」はそこで止まります)。`status` の無い古い答えは今までどおり読みます(`src/devstudio/model/graphcanvasmodel.cpp`)。palette の render.compute で選んだ ray tracing の pipeline の task が書く image は、`resource_ports` の `format` があればその format で作ります(shadow mask なら `R8_UNORM`。無ければ `R16G16B16A16_SFLOAT`)。

| 鍵 | 中身 |
|---|---|
| `material_pass_contracts`(catalog) | `[{contract, material_outputs, sockets: [{socket, aspect, formats}], combinations: [{<socket>: <format>, ...}]}]`。`material_contract` の 4 つ(`legacy_gbuffer_v1` / `deferred_geometry_v1` / `forward_opaque_v1` / `forward_transparent_v1`)ごとに、`material_outputs` を書かない pass の socket(graph の `render.material_pass` と同じ名前を location の順に。最後に `depth`)が受ける format を並べます([組み立て](../../src/core/communication/engineauthoringlists.cpp#L49))。`material_outputs` はその contract の pass が `material_outputs` を書けるか(`legacy_gbuffer_v1` だけ false)。G-buffer 系は 1 枚目(`albedo`)の format で SDR と HDR の表が切り替わるので、socket ごとの `formats` は 2 つの表の和で、socket ごとに選んだものが組として通るとは限りません。通る組は `combinations` で見ます(G-buffer 系は 2 組、forward 系は 1 組。[表と切り替えの判定](../../src/core/renderingpass/materialpassattachments.hpp#L50))。`depth` は `D32_SFLOAT` だけです。✅MP-1(`afd923e1`)から、各 contract に `screen_inputs` と `surface_resources` が付きます。どちらも `[{name, source_type}]` で、pass の同名の field に書ける名前と、つなぐ render target が持つべき semantic type です([MP-1 で contract に足す所](../../src/core/communication/engineauthoringlists.cpp#L110))。`screen_inputs` は `opaque_color`(`pelican.render.color_signal@1`)/ `opaque_depth` / `scene_depth` / `linear_view_depth`、`surface_resources` は `directional_shadow` / `point_shadow` / `spot_shadow` で、`opaque_color` 以外は `pelican.render.depth@1` です。engine の 2 つの一覧から作り、入力の契約を作る側も同じ一覧の外を断ります([第6章](06_rendering.md) §6.18「グラフで書く pass」)。pass の parser は contract によって名前を変えないので、4 つの contract とも同じ一覧です |
| `material_output_formats`(catalog) | `{color: {<型>: [<format>...]}, depth: [<format>...]}`。`material_outputs` を書く pass の、出力の型(`float` 〜 `uvec4` の 12 個)ごとに書ける color の format(parser の format の表のうち aspect が color で、数の種類 —— 浮動小数 / 符号付き整数 / 符号なし整数 —— が型と同じもの)と、depth に使える format(aspect が `depth` か `depth_stencil` のもの)([組み立て](../../src/core/communication/engineauthoringlists.cpp#L116)) |
| `engine_shaders`(`get_engine_shaders` の `ready`) | `[{reference, stage, forms, loads, resource_port_include?, reflected_from \| reflection_error, pass_inputs?, variadic_inputs?, resource_ports, fragment_outputs?, local_size?}]`。engine に組み込まれた shader を `engine://<stem>` と stage ごとに 1 件([組み立て](../../src/core/communication/engineauthoringlists.cpp#L236))。`forms` は組み込まれた形(`source` / `spirv`)、`loads` は pass の stem の参照が実際に読む方(runtime compiler のある build で source があれば `source`、それ以外は `spirv`。compiler の無い build では spirv の無い shader は出ません)。反映は worker thread が 1 process に 1 回だけ取ります(✅EL-4c。[一度だけ作る所](../../src/core/communication/engineauthoringlists.cpp#L346))。source が `pelican_resource_ports.glsl` を include する shader は `resource_port_include: true` です。その include は pass の `ports` から生成されます。一覧の反映は engine の空の include で compile するので(✅RS-11。[第6章](06_rendering.md) §6.6 の生成 include の段落)、port を任意にした shader は source から反映され(`engine://bloom_composite` は `reflected_from: "source"`、入力 2 つ、`resource_ports: []`)、条件無しにアクセサを呼ぶ shader は compile に失敗して、spirv が組み込まれていればそちらを反映し(`reflected_from: "spirv"`)、無ければ `reflection_error`(compile の log)になります。RS-11 の前は `engine://bloom_composite` も `reflection_error` でした。vertex / fragment には `pass_inputs`(`get_frame_plan` の `pass_inputs.declarations` と同じ形。読めなければ `pass_inputs_error`)と `variadic_inputs`(`{first, max}` か `null`)、反映できた shader には `resource_ports [{port, binding, type, format?}]`(set 1 の `pelican_resource_<port>`。compute / RT task はこの名前で binding を決めます —— ✅RS-5、[第6章](06_rendering.md) §6.6。✅EL-4c から、storage image の port は GLSL の layout qualifier から読んだ format を `render_target_vocabulary` の `formats` と同じ名前で `format` に持ちます。例: `engine://rt_shadow_mask_pipeline` の raygen の `shadow_mask`(`layout(r8, ...)`)は `R8_UNORM`。qualifier の無い port、`formats` に無い format、storage image 以外の port には `format` がありません。[付ける所](../../src/core/communication/engineauthoringlists.cpp#L290)、[qualifier の読み](../../src/core/shader/shaderreflection.cpp#L84))、fragment には `fragment_outputs [{location, name}]`、compute には `local_size` |
| `format_aspects`(`render_target_vocabulary`) | `{<format>: "color" \| "depth" \| "depth_stencil" \| "stencil"}`。`formats` の名前すべてに、Vulkan の format の成分(D と S)から読んだ aspect を付けます([判定](../../src/core/renderingpass/renderingpassjsonhelpers.cpp#L104)、[足す所](../../src/core/communication/renderconfigeditor.cpp#L2692))。`formats` は今までどおり名前の配列 |

一覧には、どの値を選ぶと再起動が要るかは付きません(要るかは編集の commit の `restart_required` で分かります)。image の `usage` について、型ごとに要る usage の一覧はありません。`render.image` の lowering は `usage` を足さず書かれた値を render target に渡すだけで([lowering](../../src/project/rendergraphcomposition.cpp#L798))、要る usage は使われ方ごとに別の所で見ています(pass の出力の [`COLOR_ATTACHMENT` の検査](../../src/core/renderingpass/renderingpassvalidation.cpp#L59)、compute の port の [usage の検査](../../src/core/renderingpass/computetaskcontainer.cpp#L309) など)。catalog の 2 つの鍵と `get_engine_shaders` の一覧は `pelican_cli graph-catalog` には出ません(CLI はモジュールのカタログだけを印字します。[第10章](10_tools.md) §10.4)。✅UX-5b(`baf1748a`)から Studio の Graph は `material_pass_contracts`・`format_aspects`・`get_engine_shaders` の一覧を読みます(`material_output_formats` は読みません。`src/devstudio/model/graphcanvasmodel.cpp`)。
test: `renderconfigeditor_test` の
[「EL-4 get_graph_catalog lists material pass formats, material output formats and engine shaders」](../../test/renderconfigeditor_test.cpp#L10523)
(contract ごとの socket と組、`deferred_geometry_v1` の 2 組の `albedo` が `B8G8R8A8_SRGB` と `R16G16B16A16_SFLOAT`、型ごとの format と depth の `D16_UNORM` / `D24_UNORM_S8_UINT` / `D32_SFLOAT`、`format_aspects`、`engine://passthrough` の `variadic_inputs` が `{first: 0, max: 8}`、
`engine://ray_deform` の `local_size` が `[64, 1, 1]`、`engine://bloom_composite` が `resource_port_include: true` で source から反映され入力 2 つ・port 無しであること(✅RS-11 から。EL-4 の時点では `reflection_error`)。✅EL-4b から、catalog に `engine_shaders` が無く、catalog を読んでも一覧を作りに行かないこと(呼ばれた回数 [`engineShadersRequestCount()`](../../src/core/communication/engineauthoringlists.cpp#L605) で見る)、`get_engine_shaders` が知らない field を断ること。✅EL-4c から、worker を最初の shader の前で止めておき、`get_engine_shaders` が `{status: "building"}` をすぐ返し、その間も `get_graph_catalog` が答えること、止めを外すと `ready` になり、以後同じ答えが続くこと、`engine://rt_shadow_mask_pipeline` の `shadow_mask` が `storage_image` で `format: "R8_UNORM"`(vocabulary の `formats` にある)、`normal` が `sampled_image` で `format` が無いこと)と、`engine_primitives_test` の
[「EL-4 the material pass formats and engine shaders get_graph_catalog lists are the ones the engine takes」](../../test/engine_primitives_test.cpp#L1860)
(GPU。✅EL-4b から shader の一覧は `get_engine_shaders` で読み、catalog に `engine_shaders` が無いことも見る。✅EL-4c から、endpoint が始めた worker を止めておいた間に `get_engine_shaders` が `building`、`get_status` が答えることを見て、止めを外した後は `step_frame` を回しながら `ready` まで聞き直す。blank の project で G-buffer の contract の 2 組をそれぞれ image ごと commit し、一覧に無い depth と color の format は文書を変えずに断られ、一覧の `engine://fullscreen` と `engine://passthrough` の fullscreen pass が `restart_required` 無しで commit されて frame plan に出て、forward の組も commit されることを見る)。
MP-1 の `screen_inputs` / `surface_resources` は `renderconfigeditor_test` の
[「MP-1 get_graph_catalog's material pass contracts list the screen inputs and surface resources the engine takes」](../../test/renderconfigeditor_test.cpp#L10473)
(4 つの contract とも同じ名前の列で、`source_type` が入力の契約の semantic type と同じこと、`forward_opaque_v1` の先頭が `{name: "opaque_color", source_type: "pelican.render.color_signal@1"}` と `{name: "directional_shadow", source_type: "pelican.render.depth@1"}` であること、一覧の外の名前(`lit_color`)が契約にならないこと)。

**project の shader の入出力を読む —— `get_shader_interface`(✅EL-5、`59e5d792`)。**`get_engine_shaders` は engine に組み込まれた shader だけを返すので、利用者が project に書いた shader(`project://shaders/...`)の port を Studio が読む口がありませんでした(仕様書の記述では、GB-3 で手で書いた)。`get_shader_interface` は 1 つの stem の参照について、`get_engine_shaders` と同じ形の要素を返します([引数を読む所](../../src/core/communication/renderconfigeditor.cpp#L3818)、[答えを作る所](../../src/core/communication/engineauthoringlists.cpp#L609)、[登録](../../src/core/communication/editorrpchandlers.cpp#L304))。

```json
{"method": "get_shader_interface", "params": {"reference": "project://shaders/el5_probe"}}
→ {"status": "building"}
→ {"status": "ready", "reference": "project://shaders/el5_probe",
   "stages": [{"reference": "project://shaders/el5_probe", "stage": "compute", "forms": ["source"],
               "loads": "source", "resource_port_include": false, "reflected_from": "source",
               "resource_ports": [{"port": "source", "binding": 0, "type": "sampled_image"},
                                  {"port": "target", "binding": 1, "type": "storage_image",
                                   "format": "R16G16B16A16_SFLOAT"}],
               "local_size": [8, 4, 1], "reads_loop_index": true, ...}]}
```

(`renderconfigeditor_test` の EL-5 の値。`...` は省略)

- **params**: `reference`(必須の文字列)と `refresh`(任意の真偽)だけで、ほかは `-32602` です。
- **stage の集め方**: shader library が stem の参照を読むときと同じ候補(stage ごとに source と `.spv`)を同じ検査で探します([`shaderStemCandidateReference()` / `existingShaderStemCandidate()`](../../src/core/shader/shaderlibrary.cpp#L555))。runtime compiler の無い build では、`.spv` の無い stage は出ません。どの stage の file も無ければ、worker を待たずに `{status: "failed", reference, error: "shader_not_found"}` です。
- **stage の失敗**: compile も反映もできない stage は、答えは `ready` のまま、その要素に `reflection_error`(compile の log)が入ります。log の中の機械の絶対パスは file の参照(`project://shaders/<file>`)に置き換えます([置き換える所](../../src/core/communication/engineauthoringlists.cpp#L178))。test では `project://shaders/el5_broken.frag:3: ...` の形です。`failed` になるのは worker の中の例外のときです。
- **include**: project の file は、その file の directory、`<その directory>/shaders/include`、engine の資源(`<名前>` と `shaders/include/<名前>`)の順に解決します([`ShaderCompiler::compileFile()`](../../src/core/shader/shadercompiler.cpp#L761)、include の解決は [`resolveInclude()`](../../src/core/shader/shadercompiler.cpp#L105))。pass の `ports` から生成される `pelican_resource_ports.glsl` を include する shader は、この順のとおり、project の側に同名の file が無ければ engine 同梱の空の include で compile されます(`get_engine_shaders` と同じ。要素の `resource_port_include: true`)。
- **worker**: 反映は自分専用の worker thread が 1 本で行い、依頼の順に 1 つずつ処理します。compiler は `get_engine_shaders` と同じ [`ShaderCompiler::detached()`](../../src/core/shader/shadercompiler.hpp#L91) の別の instance で、frame の側の shader library と project の cache には触らず、disk と engine の資源を読むだけです([`ShaderInterfaceReflection`](../../src/core/communication/engineauthoringlists.cpp#L461))。worker は最初の依頼で始まり、engine の RPC の endpoint が閉じるときに `get_engine_shaders` の worker と一緒に止めて join します([止める所](../../src/core/communication/rpcserver.cpp#L2830))。止めるのは stage 1 つごとの区切りで、出来上がった答えは残し、待ち行列と作りかけは捨てます。まだ始まっていない同じ reference の依頼は、新しい依頼で置き換わります(待ち行列の最後に入り直します)。
- **作り直し**: 答えは reference ごとに、stem の各 stage の file の中身の SHA-256 を key に持ちます。呼ぶたびに RPC の thread で file を読んで key を作り直し、変わっていれば新しく作らせて `building` を返します。**include の変更は追いません。**include を書き換えたら `refresh: true` で呼びます(持っている答えを使わずに作り直します)。
- **`engine://` の reference**: `get_engine_shaders` の一覧の、その stem の要素をそのまま返します(一覧が作りかけなら `building`。`refresh` は効きません。一覧に無い stem は `shader_not_found`)。
- **分かっている穴**: rendering 節の `shader_defines` を compile に渡していません(仕様書の記述。コードの読みでは `compileFile()` に define を渡していません)。define で port が変わる shader では、pass が実際に使う形と違いえます。
- test: `renderconfigeditor_test` の [「EL-5 get_shader_interface reflects a project shader stem on a worker and rebuilds it when its files change」](../../test/renderconfigeditor_test.cpp#L10787)(worker を止めておいて `building`、`ready` の port と `local_size` と `reads_loop_index`、本体の書き換えで作り直し、include の書き換えは `refresh` まで追わない、compile できない fragment の `reflection_error` が参照の名前で始まる、止めた間に頼んだ 2 つが順に作られる、無い stem、引数の誤り、`engine://bloom_composite` が `get_engine_shaders` の要素と同じ)。`engine_primitives_test` の EL-4 の GPU の test も、`get_shader_interface {reference: "engine://passthrough"}` の `stages` に `get_engine_shaders` の要素が入ることを見ます。

**登録済みの provider の名前を読む —— `get_registered_providers`(✅EL-6、`5582bb93`)。**文書が provider を名前で指す欄(draw_sort・render_strategy・graph_transforms など)に、今の engine が引ける名前を、game の DLL が登録したものも含めて返す読みです([`RenderConfigEditorService::getRegisteredProviders()`](../../src/core/communication/renderconfigeditor.cpp#L3811)、[答えを作る `registeredProvidersJson()`](../../src/core/communication/engineauthoringlists.cpp#L668))。params は空の object だけで、field を足すと `-32602` です。

| 鍵 | 文書のどの欄の名前か | engine の provider(test で確かめた名前) |
|---|---|---|
| `draw_sort` | `draw_sort.opaque.provider` / `draw_sort.transparent.provider` | `state_batched_v1`、`back_to_front_v1` |
| `pass_implementation` | pass の `implementation.provider` | `builtin.fullscreen_v1` |
| `region_replacements` | `region_replacements[].provider` | `builtin.identity_v1` |
| `graph_transforms` | `graph_transforms[].provider` | `builtin.identity_v1` |
| `render_strategy` | `render_strategy.provider` | `builtin.authored_config_v1` |

- 各要素は `{name, source, provider_version}` の 3 鍵で、名前順です。`source` は engine が登録したものが `"engine"`、それ以外(game の DLL の登録)が `"game"` です。game の provider が engine のものを同じ名前で覆っていれば、覆った game の 1 件だけが出ます(registry が名前で引くときと同じ。[一覧を作る `listing()`](../../src/core/renderingpass/providerregistrycommon.hpp#L406))。
- cache しません。呼ぶたびに registry を読むので、game logic の reload で登録・解除された provider は次の呼び出しに出ます。owner が active になる前の候補の登録は出ません。
- `description` / `parameters` は出しません。これらの registry の ABI に、説明や parameter の宣言の欄が無いためです(コードの注記)。
- 対象外の registry: `VulkanNativeScopeExecutorRegistry`、`RenderViewFamilyProviderRegistry`、`CompilerProviderRegistry`(統合側の報告。この RPC はこの 3 つを読みません)。
- `region_replacements` は ✅EL-1 の `get_render_authoring_context` の `region_providers` と同じ表から作りますが、鍵の形が違います(`region_providers` は `builtin`、こちらは `source`)。
- test: `renderconfigeditor_test` の [「EL-6 get_registered_providers lists each render provider registry's names, a game owner's as it changes」](../../test/renderconfigeditor_test.cpp#L10706)(5 つの鍵、名前順、engine の provider の 3 鍵、game の owner の render_strategy の provider が候補の間は出ず、active で `source: "game"`、解除で消えて最初の答えに戻る、余分な field の拒否)と、`render_policy_dll_test` の [「EL-6 registered providers list the game DLL's providers as the game's across a reload」](../../test/render_policy_dll_test.cpp#L617)(test 用の game の DLL の 5 つの provider が読み込みで `game` として出て、reload の後も出て、shutdown で消える)。

**feature と snapshot を足す前に読む —— `required_targets` / `insertion` / `snapshot_after_candidates`(✅FR-1 / FR-2 / FR-3)。**Graph で組んだ pipeline(Blank から始めたものなど)に feature や snapshot を足して合成が通らないとき、engine は target や pass を黙って足さず、名前付きで断ります。Studio が足す前に案内し、断られても 1 回の編集で直せるように、読む側と拒否の両方に材料を載せました(FR-1 `45ca769b`、FR-2 `0b15fb62`、FR-3 `55390f4b`)。意味の正は [第6章](06_rendering.md) §6.5「feature が pipeline に求める target と挿入先」と §6.8「`after` に書ける名前」です。

| 読む所 | 鍵 | 中身 |
|---|---|---|
| `list_render_features` の各項目 | `required_targets` | `[{name, type, usage?}]`。feature が読み書きして自分では宣言しない target と、`render.target_ref` に書く型。例(test): `sky_ambient` は `lit_color` と `scene_depth`(`{"semantic": "pelican.render.depth@1", "arguments": {"representation": "device", "space": "projection"}}`、`usage: ["SAMPLED"]`) |
| 同上 | `required_targets[].needs_writer` | ✅FR-3。`true` は、feature がその target をこの frame に読むだけで、自分では書かないもの(pipeline の誰かが先に描く必要がある)。例(test): `sky_ambient` は `lit_color` が `false`、`scene_depth` が `true` |
| 同上 | `insertion` | `[{pass, anchor, position: "after" \| "before"}]`。feature の pass の挿入先。例(test): `sky_ambient` は `[{"pass": "sky_background", "anchor": "deferred_lighting", "position": "after"}]` |
| `get_render_authoring_context` | `snapshot_after_candidates` | `rendering.snapshots` の `after` に書ける名前の配列([FR-1 で context に足す所](../../src/core/communication/renderconfigeditor.cpp#L2629))。例(test): Blank 相当の graph の pipeline では `["post_ldr"]` |

編集(`edit_render_features` の `add`、`edit_graph_document` など)が合成で断られると、ticket は rejected で、`error` に code と次の fields が並びます。文書は書かれません。

| code | いつ | fields |
|---|---|---|
| `feature.required_target_missing@1` | feature が要る target を、pipeline・feature・snapshot のどれも宣言していない | `{feature, target, type, document}`。`type` は `required_targets` と同じ型、`document` は feature 文書の参照(`engine://features/sky_ambient.json` など) |
| `feature.required_target_unwritten@1` | ✅FR-3。feature が読む target は宣言されているが、pipeline の誰も書いていない(FR-3 の前は frame planner の `Pass input target is not produced as an earlier output: <target> in pass: <pass>`) | `{feature, target, type, writer_candidates: [{node?, pass, port, resource}], document}`。`writer_candidates` は、feature の読む pass と同じ family でその前にあり、同じ種類(depth / color)の target を書いている出力。`view_family` を書いた pass(shadow map など)と feature の pass は入らない。root の graph の node が作った pass なら `node` と、その node の graph の port が入る |
| `feature.insertion_anchor_missing@1` | feature の pass の `after:` / `before:` の名前が pipeline に無い | `{feature, anchor, candidates, document}`。`candidates` は、その pipeline で `after:` / `before:` がちょうど 1 つの frame node に解決する名前(pipeline の並びの順) |
| `snapshot.after_unknown@1` | snapshot の `after` が pipeline に無い | `{snapshot, after, resolved?, candidates}`。`resolved` は anchor 名のときの `__anchor_<anchor>`、`candidates` は `snapshot_after_candidates` と同じ作り方の名前 |
| `graph.target_ref_type_mismatch@1`(既存) | `render.target_ref` の型が合成後の target と食い違う | 今までの fields に `expected: {type, layers?}`(✅FR-1)。`expected.type` に書き換えると通る型、`layers` は配列の target の層の数 |

- 1 つの feature では `feature.required_target_missing@1` の検査が先で、両方が欠けていればこちらが出ます。`feature.required_target_unwritten@1` は合成の終わりの検査なので、この 2 つのどちらかが出る間は出ません。
- `feature.*` と `snapshot.*` は `graph.` で始まらないので、CT-2 の `document`(root 文書)と `definition` は付きません。`feature.*` の `document` は feature 文書の参照で、意味が違います。
- 直し方の例(test): Blank 相当の graph の pipeline に `sky_ambient` を足すと `{"code": "feature.required_target_missing@1", "feature": "sky_ambient", "target": "scene_depth", "type": <上の depth の型>, "document": "engine://features/sky_ambient.json"}`。`scene_depth` の宣言と、その型の `render.target_ref` を足すとこの拒否は消えますが、それだけでは通りません。宣言しただけで誰も書いていない target は読めないので、✅FR-3 からは `feature.required_target_unwritten@1` で断られます(下の例)。`deferred_lighting` の pass が無い pipeline では `feature.insertion_anchor_missing@1` になり、`candidates` に `lighting_pass` や `present` が並びます(`deferred_lighting` は並びません)。
- 書き手の直し方の例(✅FR-3。test は UX-12b の GUI session が Blank から Graph で組んだ deferred の pipeline を fixture にしたもの): `sky_ambient` の target(`lit_color` / `scene_depth`)と target_ref(`lit_color_ref` / `scene_depth_ref`)を足した後で `sky_ambient` を `add` すると、`{"code": "feature.required_target_unwritten@1", "feature": "sky_ambient", "target": "scene_depth", "type": <上の depth の型>, "writer_candidates": [{"node": "render_material_pass", "pass": "render_material_pass", "port": "depth", "resource": "render_material_pass__depth"}], "document": "engine://features/sky_ambient.json"}` です。候補の port を target_ref へ繋ぎ替える 1 回の編集 `edit_graph_document {ops: [{"op": "set_input", "node": "render_material_pass", "port": "depth", "source": {"node": "scene_depth_ref", "port": "resource"}}]}` を送ると通り、次の `sky_ambient` の `add` が committed になります。frame plan では `render_material_pass` が `scene_depth` を書き、`sky_background` が読みます。実 Renderer を RPC で動かす GPU の test でも同じです。
- test: `renderconfigeditor_test` の [「FR-1 the catalog gives each feature's required targets and adding one without them is refused by name」](../../test/renderconfigeditor_test.cpp#L11523) と [「FR-2 the catalog gives each feature's insertion points and adding one whose anchor is missing is refused by name」](../../test/renderconfigeditor_test.cpp#L11593)。FR-3 は `renderconfigeditor_test` の [「FR-3 a feature's target that no pass writes is refused with the passes that could write it, and connecting one composes」](../../test/renderconfigeditor_test.cpp#L11821) と、`engine_primitives_test` の [「FR-3 sky_ambient on the Blank-built deferred pipeline names the material pass's depth as its writer, and connecting it publishes, through the running renderer」](../../test/engine_primitives_test.cpp#L3352)(GPU)。

**複数の編集を 1 つの ticket で —— `edit_batch`(✅FR-3、`55390f4b`)。**render の文書を書き換える編集を、順に同じ候補の文書へ当て、最後に 1 回だけ組み立てて preflight し、1 つの ticket で公開します([`RenderConfigEditorService::editBatch()`](../../src/core/communication/renderconfigeditor.cpp#L2053))。2 つの編集がどちらを先に送っても単独では通らないとき(下の cascade の例)と、Studio の 1 回の操作を 1 回の undo にしたいときのための口です。

- **引数**: `{base_source_digest, edits: [{method, params}...]}`。各要素の鍵は `method` と `params` だけです。`params` は、その method を単独で送るときの params から `base_source_digest` を除いたものです(batch の digest が唯一の digest で、各 edit には engine が直前の候補の digest を入れます)。
- **返り値**: 受けると `{ticket, status: "accepted", base_source_digest, methods}`(`methods` は送った順の method 名)。ticket の id は `render-config-edit-<N>` です。commit の結果は `operation: "edit_batch"` と `methods` を持ち、RD-1 の `documents` は batch 全体で 1 回分です。なので `restore_render_documents` 1 回で batch ごと戻せます(test)。各 edit の結果の追加の鍵は、同じ鍵なら先の edit の値が残ります。
- **受ける 23 の method**([edit_batch が受ける method の表](../../src/core/communication/renderconfigeditor.cpp#L2012)。この順で一覧にします): `edit_render_features`・`edit_render_targets`・`add_authored_pass`・`remove_authored_pass`・`edit_graph_document`・`edit_graph_instances`・`decompose_material_pass`・`apply_graph_template`・`fold_nodes`・`set_material_route_default`・`edit_rendering_keys`・`edit_frame_node_order`・`edit_target_planning`・`edit_vulkan_plan_pins`・`put_vulkan_physical_fragment`・`fork_render_pipeline`・`edit_region_replacement`・`add_compute_task`・`edit_ray_callable_records`・`edit_scene_acceleration_structures`・`edit_preset_pass`・`edit_compute_task`・`remove_compute_task`。どれも 1 つの ticket で文書を書き、書き換える文書を editor が持つもの(前の edit が作った候補)から読むものです(コードの注記)。最後の 3 つは ✅FR-4(`2eb08d21`)で入りました(下の「project の preset と feature を書き換える編集(✅FR-4)」)。FR-3 のときは 20 でした。
- **外した method**([外した method とその理由の注記](../../src/core/communication/renderconfigeditor.hpp#L836)): 自分の ticket で文書を書かないもの、非同期のもの(`verify_fold` など)、`restore_render_documents` / `reload_render_documents` です。`edit_batch` 自身も入れられません。FR-3 では、書き換える文書をディスクから読む `edit_compute_task` / `remove_compute_task` / `edit_preset_pass` も外していましたが、FR-4 で入りました。
- **順と検査**:
  1. 最初に 1 回だけ、gate と `base_source_digest` を見て、managed fragment をディスクと比べます(違えば `external_modification` `{fragment_reference}`)。各 edit はこの比べ直しをしません。
  2. edit を順に当てます。各 edit の自分の検査は単独のときと同じで、合成と変種の preflight だけを飛ばします。当てられない edit があれば、その edit の拒否をそのまま返し、`error` に `batch_index`(0 始まり)を足します([batch_index を足す所](../../src/core/communication/renderconfigeditor.cpp#L2141))。文書は書かれず、ticket も残りません。
  3. 最後の候補を 1 回組み立て(`add_authored_pass` が求める検査もここで当てる)、変種の preflight を通します。ここで断られた拒否には `batch_index` は付きません。
- **invalid params**(JSON-RPC の `-32602`): 一覧の外の method(文に受ける method の一覧が入る)、`params` に `base_source_digest` があるもの、`edits` が空か配列でないもの、要素が object でない・`method` / `params` 以外の鍵がある・`method` が文字列でない・`params` が object でないもの、64 桁の小文字の hex でない `base_source_digest`。各 edit 自身の invalid params も batch の invalid params になり、文は `edit_batch edits[<番号>] (<method>): <元の文>` です。
- **例(test。上の FR-3 の例と同じ Blank から組んだ deferred の pipeline)**: `shadow_directional` の `cascade_count` を 3 にすると `shadow_map` は 3 層の配列になります。`edit_render_features` で `cascade_count` 3 を単独で送ると、2D の `shadow_map_ref` と食い違って `graph.target_ref_type_mismatch@1`。`shadow_map_ref` の型と lighting の pass の port の型を 2D array の depth にする `edit_graph_document` を単独で送っても、今の shadow map(cascade 1 の 2D)と食い違って同じ code です。
  - `edits: [{method: "edit_render_features", params: <cascade_count 3>}, {method: "edit_graph_document", params: {ops: [<shadow_map_ref の type を two_d_array の depth に>, <deferred_lighting の ports.shadow_map.type を同じ型に>]}}]` を送ると、1 つの ticket で committed になり、`documents` は root 文書 1 つです。
  - graph の編集を 2 つの edit に分けると通りません。`shadow_map_ref` の型だけを替える `edit_graph_document` は、その edit の所で `batch_index: 1`・`invalid_graph_document`(`graph type mismatch at deferred_lighting.shadow_map from shadow_map_ref.resource`)で断られます(test)。`edit_graph_document` は自分の ops を当てた graph を自分で検査するので、lighting の port の型は同じ `ops` に入れます。
- test: `renderconfigeditor_test` の [「FR-3 edit_batch applies edits in order to one candidate as one ticket, names the edit it refuses, and RD-1 undoes it」](../../test/renderconfigeditor_test.cpp#L11887)(単独の 2 つの拒否、`batch_index` 付きの拒否で文書も公開も変わらないこと、1 ticket・1 回の公開・RD-1 の 1 文書と restore、外の method・`params` の digest・空の `edits`・edit 自身の invalid params)と、`engine_primitives_test` の [「FR-3 edit_batch takes cascade_count 3 with the 2D array shadow map types in one ticket, through the running renderer」](../../test/engine_primitives_test.cpp#L3436)(RPC と実 Renderer。GPU。外の method は `-32602`)。

**project の preset と feature を書き換える編集(✅FR-4、`2eb08d21`)。**`edit_preset_pass`(root・project の preset・project の feature のどの行き先も)、`edit_compute_task`、`remove_compute_task` は、書き換える文書を editor が持つ形で読みます([FR-4 の `heldDocument()`](../../src/core/communication/renderconfigeditor.cpp#L1089))。FR-3 のときは、この 3 つはディスクから読むので batch に入れられませんでした。

- **batch の中で読む文書**: 同じ batch の前の edit が同じ preset / feature を書いていれば、その edit が作った bytes を読みます。書いていなければディスクの bytes です。なので、同じ preset に `edit_preset_pass` → `edit_compute_task` と送ると、後の edit は前の edit の結果の上に書きます(test)。
- **digest は batch の前に読んだ値**: 各 edit の `base_preset_digest` / `base_feature_digest` / `base_task_source_digest` には、batch を送る前に読んだ文書の digest をそのまま入れます(batch の途中の digest ではありません)。前の edit が同じ文書を書いた後でも、比べる相手は batch の前の digest です。違えば単独のときと同じ `external_modification` です。
- **例(空を present に映す。test。上の FR-3 の例と同じ Blank から組んだ deferred の pipeline に `sky_ambient` を足した後)**: 2 つの edit が 1 つの ticket で committed になり、`documents` は root 文書 1 つ、frame plan では `deferred_lighting` と `sky_background` が `lit_color` を書き、`present` が `["lit_color"]` を読みます。`restore_render_documents` 1 回で両方が戻ります(`present` は `deferred_lighting__color` を読む形に戻る)。FR-4 の前は 2 つの ticket と 2 回の undo が要りました。

  ```json
  {"base_source_digest": "<root>",
   "edits": [
     {"method": "edit_graph_document",
      "params": {"ops": [{"op": "set_input", "node": "deferred_lighting", "port": "color",
                          "source": {"node": "lit_color_ref", "port": "resource"}}]}},
     {"method": "edit_preset_pass",
      "params": {"graph": "main_render", "pass_name": "present", "input": ["lit_color"],
                 "output": {"color": ["display"], "depth": null},
                 "shader": {"vertex": "engine://fullscreen", "fragment": "engine://passthrough"}}}]}
  ```

  `lit_color_ref` の型には `list_render_features` の `sky_ambient` の `lit_color` の型(✅FR-4 から `color_signal`。[第6章](06_rendering.md) §6.5「色の target の型の宣言」)をそのまま書けば、lighting の pass の color port の型と同じなので、型を書き換える op は要りません。
- **残る穴**: 同じ batch の中で、`edit_region_replacement` / `decompose_material_pass` / `apply_graph_template` が前の edit で書き換わった preset を読むと、`external_modification` で断られます(統合側の記録)。この 3 つは今の候補の preset の bytes の digest を、呼び手の送った batch の前の digest と比べるためです(コードの読み。この組み合わせの test はありません)。
- test: `renderconfigeditor_test` の [「FR-4 edit_batch takes edit_graph_document and edit_preset_pass as one ticket, and RD-1 undoes both」](../../test/renderconfigeditor_test.cpp#L11986) と [「FR-4 edit_batch takes edit_preset_pass with a compute task edit on one project preset or feature, each on what the edit before it made」](../../test/renderconfigeditor_test.cpp#L12068)(preset では pass → task の変更、feature では pass → task の削除。どちらも restore 1 回で戻る)、`engine_primitives_test` の [「FR-4 edit_batch puts the sky on present with edit_graph_document and edit_preset_pass in one ticket, through the running renderer」](../../test/engine_primitives_test.cpp#L3515)(RPC と実 Renderer。GPU)。

> **✅PV-1(`a173e70c`)で変わりました。**編集の preflight は preview を組みません。下の段落の「preview で断られる」は PV-1 の前の記述です。今は、preview だけが組めない編集(例の `trail@history` を読ませる `edit_preset_pass` など)も committed になり、結果の `preview` が `{available: false, reason: {code: "preview.unsupported_authoring@1", feature: "history_read", graph, pass, message}}` になります。preview の状態は `get_status` / `get_frame_plan` の `preview` でも読めます。`variant` 付きで断られるのは、今は xr だけです。詳しくは [第6章 §6.13](06_rendering.md) の PV-1 の段落。

**編集は、起動時に登録した graph の変種の全部で合成してから受けます(✅G-6)。**engine は起動時に flat と一緒に preview の
graph も組み、XR を始めたときは xr も組みます([第6章 §6.13](06_rendering.md))。render の編集の候補は、ticket を取る前に
このすべてで合成されます。flat と preview は常に、xr は起動時に XR を始めた engine だけです(xr は history を持つ feature を
拒否するので、XR を始めていない engine で xr を合成すると、TAA の project の編集がすべて断られてしまいます)。対象は root の
文書を変える編集(`edit_render_features`・`edit_preset_pass`・`edit_graph_document` など)と、authored pass の系統
(`add_authored_pass`・`remove_authored_pass`・`edit_render_targets`)です([変種を合成する preflight](../../src/core/communication/renderconfigeditor.cpp#L1141))。
flat で合成できない候補は今までどおり flat の経路で断られます。flat が通って preview か xr が合成できない候補は
`render_pipeline_preflight_failed` に **`variant`**(`"preview"` / `"xr"`)が付いて断られ、文書は変わりません(✅LP-0d から、graph の名前付きの拒否と `<code>: <fields>` で運ばれる拒否は、その code のまま `variant` を添えて返ります。下の「ループと分解の RPC」)。`set_layout`
だけの `edit_graph_document`(再 compose しない編集。`material_route_default` を付けたものは除く ✅RS-2)と、context の読み取りは合成しません。Studio はこの拒否を、Pass Editor と
Frame Plan の status 行に `The preview graph refused it: <code>: <message>` の 1 行で出します(以前の Pass Editor は
`authoring failed` とだけ出し、理由は tooltip にしかありませんでした。[拒否の 1 行を作る所](../../src/devstudio/view/frameplanedit.cpp#L1834))。
例: `history: true` の target `trail` を足す編集は通りますが(preview はその fragment ごと外します)、direct root の `present` に
`trail@history` を読ませる `edit_preset_pass` は preview で断られます。

**fullscreen pass の入力と shader の宣言が合わない拒否は、どの入力かを言います(✅Q-14e)。**fullscreen pass(と入力を持つ raster pass)の入力は、shader の set 1(`PELICAN_SET_PASS_INPUT`)の binding を `input` の位置で数えます —— texture の入力が binding 0 から、その後に buffer の入力です([第6章 §6.4](06_rendering.md))。engine は pass を組むときに、入力ごとにその binding の宣言を shader の reflection で探し、合わない最初の binding で断ります([入力と reflection の照合](../../src/core/fullscreenpass/fullscreenpasscontainer.cpp#L134))。逆向き —— shader が、pass の渡す入力の数以上の binding を宣言している —— は、埋められる種類なら engine が既定の resource で埋めて編集は成功し、埋められない種類なら同じ形で断ります(✅Q-14f。何を埋めるかは [第6章 §6.4](06_rendering.md) の「fullscreen pass の入力と shader の宣言の契約」)。どちらの向きかは `direction` で分かります。code は今までどおり `render_pipeline_preflight_failed` で、`error` に **`shader_input_mismatch`** が付きます([`shader_input_mismatch` を足す所](../../src/core/communication/renderconfigeditor.cpp#L174))。

| 鍵 | 中身 |
|---|---|
| `pass` | pass の名前 |
| `fragment` | pass の fragment の参照(書いたまま。例 `project://shaders/present`)。fragment の無い raster pass では `""` |
| `direction` | `"pass_input_undeclared"`(pass の入力を shader が宣言していない。Q-14e)/ `"shader_input_unsupplied"`(shader の宣言を pass が渡さず、engine も埋められない。✅Q-14f)/ `"variadic_layout_invalid"`(shader の宣言が可変の組の並べ方の規則を破っている。✅VI-1)/ `"variadic_capacity_exceeded"`(pass の texture の入力が、可変の組の終わり `first + max` より多い。✅VI-1)。[2 つの値](../../src/core/fullscreenpass/fullscreeninputbindingmismatch.hpp#L43)、[VI-1 の 2 つ](../../src/core/fullscreenpass/fullscreeninputbindingmismatch.hpp#L55) |
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

可変の組の枠を pass が渡さないのは意図した使い方なので、そこを既定の resource で埋めても LOG_WARNING は出ません(固定の宣言を埋めたときは従来どおり出ます。[警告から除く所](../../src/core/renderingpass/renderingpassruntimecompiler.cpp#L2506))。

**pass ごとの `shader.defines` の拒否(✅VI-1)。**fullscreen / raster pass の `shader.defines`(第6章 §6.4)が読めないときも code は `render_pipeline_preflight_failed` で、`error` に **`pass_shader_define`** `{pass, define, name, index, reason}` が付き、文書は変わりません([付ける所](../../src/core/communication/renderconfigeditor.cpp#L183))。`define` は書いたままの要素(文字列でなければその JSON の文字列)、`name` は `=` の前の名前(識別子でなければ `""`)、`index` は配列の位置です。`reason` は 3 つです([読む所](../../src/core/renderingpass/passshaderdefines.cpp#L61))。

| `reason` | いつ | message(`Pass '<pass>' shader.defines entry <index> ('<define>') ` の後) |
|---|---|---|
| `reserved` | engine が pass の compile で自分で定義する名前 —— `PELICAN_PASS_TEXTURE_INPUT_COUNT` / `PELICAN_VARIADIC_INPUT_COUNT` / `PELICAN_VARIADIC_INPUTS_LIMIT` / `PELICAN_MULTIVIEW` / `PELICAN_VIEW_COUNT` / `PELICAN_OUTPUT_UNORM_FALLBACK`、`PELICAN_INPUT_` か `PELICAN_FEATURE_` で始まる名前([予約の表](../../src/core/renderingpass/passshaderdefines.cpp#L43)) | `defines <名前>, which the engine defines itself when it compiles the pass` |
| `malformed` | 文字列でない、`NAME` / `NAME=VALUE` の NAME が C の識別子でない、改行を含む | `is not NAME or NAME=VALUE with NAME a C identifier` |
| `duplicate` | 同じ pass で同じ名前を 2 回 | `defines <名前> a second time` |

`defines` が配列でないときは `pass_shader_define` は付かず、message は `Pass shader.defines must be an array: <pass>` です(編集 RPC でこの形を確かめた test は無く、code は未確認)。

**各 pass の set 1 の宣言を `get_frame_plan` で読む(✅VI-1 / VI-1b)。**`get_frame_plan` の `nodes[]` のうち fullscreen / raster pass の node には **`pass_inputs`** が付き、その pass が今描いている pipeline の shader の set 1 の宣言を、pass が渡しているかどうかに関わらず全部返します(Studio がソケットを宣言から描くためのもの。[組み立て](../../src/core/vkcore/renderer.cpp#L4167))。

| 鍵 | 中身 |
|---|---|
| `state` | `"declared"`。pass の pipeline が見つからないなどで読めなければ `"unavailable"` で、そのときは `fragment` と `reason`(例外の文)だけが付く |
| `fragment` | pass の fragment の参照(書いたまま。fragment の無い raster pass では `""`) |
| `declarations` | `[{binding, type, name, variadic}]`、binding の昇順(`shader_pass_inputs` と同じ形) |
| `variadic_range` | `{first, max}` か `null` |
| `pass_texture_inputs` / `pass_buffer_inputs` | pass が渡している texture / buffer の入力の数 |
| `buffer_binding_base` | pass の最初の buffer の入力が入る binding(可変の組があれば `first + max`、無ければ texture の入力の数) |
| `reload_refused` | ✅VI-1b。その pass が最後に受けた shader の読み直しを断っている間だけ付く: `{reason, shader_input_mismatch, declarations}`。`declarations` は**断った新しい shader** の宣言で、上の `declarations` は今描いている前の shader の宣言のまま |

例(`engine://passthrough` の pass に何もつないでいないとき。[test](../../test/vi1_variadic_inputs_test.cpp#L740) の期待値): `{"state":"declared","fragment":"engine://passthrough","declarations":[{"binding":0,"type":"sampled_image","name":"pelican_variadic_input_0","variadic":true}, ... 8 要素],"variadic_range":{"first":0,"max":8},"pass_texture_inputs":0,"pass_buffer_inputs":0,"buffer_binding_base":8}`。VI-1 の着地(`b9b0a79a`)の時点では、描いている shader が規則を破っていると `state: "refused"` を返していましたが、VI-1b で読み直しがそういう shader を断るようになり、なくなりました。

Studio の編集(`edit_preset_pass`)では要求自体は受け付けられ(`accepted` と ticket)、この拒否は `get_edit_result` の最終の `error` に来ます(Q-14e の報告。例の `ticket` はそのためです)。`render_pipeline_preflight_failed` を例外から作る所は全部同じ関数を通るので、受付の時点で断られる経路でも形は同じです。ticket の commit で断られて巻き戻しにも失敗した(`rollback_error` が空でない)ときと、ほかの理由の `render_pipeline_preflight_failed` には付きません。Studio がこれをどう読むかは §13.5 の「shader が読まない入力」の段落(✅Q-14)です。Studio は `direction` で行と操作を分けます(✅Q-15。[拒否を読む所](../../src/devstudio/view/shaderinputcontract.cpp#L152))。`"pass_input_undeclared"` と、`direction` の無い拒否(Q-14f より前の engine の形)には、Q-14 の 1 行(`... reads only <N> inputs. To change its inputs, change the shader too.`)と 2 つの操作(Use engine://passthrough / Open the shader。VI-3b より前の 1 つ目は Make a pass-through shader)を出します。`"shader_input_unsupplied"` には `<pass>'s shader (<fragment の stem>) declares an input the pass cannot supply (<型> at binding <b>). Remove that declaration from the shader.` を出します([逆向きの行を作る所](../../src/devstudio/view/shaderinputcontract.cpp#L113))。型は冠詞の無い語で(`_` は空白。上の例なら `uniform buffer at binding 1`)、`shader_pass_inputs` のうち `binding` が同じ要素の `type` から、無ければ message の `declares a <型> at set 1 binding <b>` から取ります。片方しか分からなければ `(binding <b>)` / `(<型>)`、どちらも無ければ括弧ごと出しません。操作は **Open the shader** だけで、Use engine://passthrough は出しません —— pass の入力を増やしても engine が埋められない型だからです([向きごとの操作](../../src/devstudio/view/shaderinputcontract.cpp#L93))。この Open the shader は file を desktop で開くだけで clipboard には何も入れず、行が `Remove the pass-input declaration at binding <b> from the shader and save it, and it reloads by itself. Then connect the input again.`(Pass Editor は最後が `Then press Save again.`)になります([開いた後の行](../../src/devstudio/view/shaderinputcontract.cpp#L130))。`shader_input_mismatch` の無い拒否でも、message が `Fullscreen shader declares a pass input the pass does not supply` を含めば逆向きとして読みます。engine が埋められる宣言を埋めたとき(編集は成功し LOG_WARNING が出るだけ)は、Studio は何も出しません。

⚠ 逆向きで Use engine://passthrough(Q-15 の時点では Make a pass-through shader)を隠し clipboard に入れないことの test は Pass Editor の側だけで、Frame Plan の側の分岐には test がありません(Q-15 の記録)。

**material pass を足す(✅G-1)。**`add_authored_pass` は fullscreen に加えて `type: "material"` を受けます(raster は
scene を描かないので受けません)。合成だけでは material pass の color の枚数・depth・format を見ないので、engine は
合成した候補の中の新しい pass を、load と同じ parser(`parsePassDefinitionFromJson`)に候補の render target で通します
([`validateAuthoredMaterialPass()`](../../src/core/communication/renderconfigeditor.cpp#L542))。contract と枚数が合わない、
depth が無い、swapchain に書く、format が違う、といった誤りは parser の文言のまま `render_pipeline_preflight_failed` で
断られ、文書は変わりません。この検査の target の大きさは仮の 1920×1080 です(parser が見るのは 1 pass の出力どうしの比
だけです)。`format_class: scene` の target の format は、running pipeline の有効な feature に `hdr` があるかで決まります。
その一覧は `get_render_authoring_context` の **`enabled_feature_names`** にも出ます
([context の enabled_feature_names](../../src/core/communication/renderconfigeditor.cpp#L2622))。Studio はこれで、新しく作る
G-buffer の target を HDR / SDR のどちらの format にするかを決めます(context が言わなければ HDR と読みます)。

**material の route の既定と、受け皿の無い material の拒否(✅G-4)。**rendering 文書の root の `material_route_default`
(`"auto"` / `"forward"`。意味と書ける場所は [第6章 §6.7](06_rendering.md))は編集 RPC からも変えられます。

- `add_authored_pass` に `material_route_default` を渡すと、pass の fragment と root の key の変更が**同じ候補文書集合**に入り、
  1 回の preflight・1 つの ticket になります([pass と route の既定を 1 つの候補に入れる所](../../src/core/communication/renderconfigeditor.cpp#L4927))。
  下の受け皿の検査は、新しい既定で計算し直した route で行います。今の値と同じなら key は書かず、再起動も求めません。
- `set_material_route_default` は既定だけを変えます(上の表)。
- `edit_graph_document` と `edit_graph_instances` も任意の `material_route_default` を受けます(✅RS-2)。規則は `add_authored_pass` と
  同じで、graph の変更と root の key の変更が同じ ticket・同じ候補に入り、preflight と live の判定は新しい既定で計算し直した route で
  行います([graph の編集に route の既定を乗せる所](../../src/core/communication/renderconfigeditor.cpp#L3877))。今の値と同じなら key は書かず、再起動も求めません。
  値が変われば commit の結果に `restart_required: true` が付きます(`restart_reason` は下の RS-1 の優先の規則のとおり)。
  - 値の検査と書き込みは `add_authored_pass`・`set_material_route_default` と同じ関数です([値の検査](../../src/core/communication/renderconfigeditor.cpp#L3846)・
    [書き込み](../../src/core/communication/renderconfigeditor.cpp#L3865))。`"auto"` / `"forward"` 以外は invalid params、runtime shader compiler の無い build の `"forward"` は下の
    `material_route_default_unavailable@1`、rendering 節の無い root は `render_config_section_unlocated`(`method` はその RPC の名前)です。
  - 書き先は root の rendering 節だけで、preset の中には書きません(`add_authored_pass`・`set_material_route_default` と同じ)。
  - 置き場所だけ(`set_layout` だけ)の op 列でも、`material_route_default` が付けば候補を組み直して判定します
    ([置き場所だけの扱いを外す所](../../src/core/communication/renderconfigeditor.cpp#L3898))。
  - 既定を変えずに blank の project へ forward の material pass を足すと、今までどおり `material_route_unserved@1` で断られます
    (deferred の route を受ける pass が無いため)。同じ ticket に `"forward"` を載せれば commit され、再起動を求めます。
  - `apply_graph_template` と `decompose_material_pass` はこの key を受けません(知らない key として invalid params)。
  - test: `renderconfigeditor_test` の
    [「RS-2 edit_graph_document and edit_graph_instances carry material_route_default in their ticket」](../../test/renderconfigeditor_test.cpp#L8172)
    (runtime を差し替えて、判定に使われた既定・`restart_reason`・同じ値の 2 回目・`edit_graph_instances`・値の誤りを見る)と、`engine_primitives_test` の
    [「RS-2 edit_graph_document carries material_route_default for a forward material pass」](../../test/engine_primitives_test.cpp#L1099)
    (GPU。blank の project に、`render.image` 2 枚(color / depth)・body が `draw.material` 1 つの definition・`over.draw_items(contract)`・
    `order: phase_default`・else の分岐 1 つの `graph.loop`・inputs が `color` / `depth` / `draws` の `render.material_pass`(forward_opaque_v1)を
    送り、既定無しは `material_route_unserved@1` で文書が変わらないこと、`"forward"` 付きは commit と `restart_required`、起動し直すと
    frame plan にその pass が出ることを見る。`restart_reason` の code は `material_physical_contract_changed` と `material_route_default_changed`
    のどちらも受けています)。
- engine はこの key を**起動時に 1 回しか読まない**ので、値を変えた ticket の commit の結果(`get_edit_result` /
  `step_frame` の `edit_results[]`)に **`restart_required: true`** と `restart_reason: {code: "material_route_default_changed"}`(✅RS-1 から)が付きます
  ([G-4 の restart_required と restart_reason を付ける所](../../src/core/communication/renderconfigeditor.cpp#L6996))。engine を起動し直すまで、
  route が変わる material は今の pipeline のまま描かれます。⚠ commit から再起動までの間に登録された material は、
  古い既定で lowering されます。
- runtime shader compiler の無い build で `"forward"` を頼むと、ticket を作る前に **`material_route_default_unavailable@1`**
  (`value: "forward"`・`reason: "runtime_shader_compiler_disabled"`)で断られます。engine の組込 material の forward 版を
  compile できないためです。

**`material_route_unserved@1`**: pipeline に material pass が 1 つでもあるのに、登録済みの material(engine の組込を含む)の
route を受ける pass が無い候補は、この code で断られ、文書は変わりません。**前の世代がその route を使っていたかどうかは
問いません** —— G-4 の前は、最初から forward の pass だけにする編集が commit を通り、次の起動で落ちていました。足りない
material を route ごとに全部集めて 1 回で断ります
([`MaterialGenerationPlanner::requireEveryMaterialServed()`](../../src/core/material/materialgenerationplanner.cpp#L794))。
`error` の直下に、他の拒否と同じく `ticket`・`participant` と並んで次の field が付きます。

| field | 中身 |
|---|---|
| `rendering_config` | 検査した rendering 文書の参照 |
| `unserved[]` | 受け皿の無い route ごとに `route`・`shader_contract`・`materials[]`・`hint`。`materials[]` の各項目は `name`(名前の無い material は `<unnamed>`)・`origin`(`builtin` / `document` / `asset` / `unknown`)・`source`(文書か asset の参照。組込には無い) |
| `hint` | route ごとの hint を `; ` でつないだもの |

route ごとの hint は 3 つです([route ごとの hint の文](../../src/project/renderpipeline.cpp#L687)):
`forward_opaque` → forward_opaque_v1 の material pass を足す / `deferred_geometry` → root の `material_route_default` を
`"forward"` にするか deferred_geometry_v1 の material pass を足す / `forward_transparent` → forward_transparent_v1 の
material pass を足す。`message` は load 時の fatal と同じ読める 1 文です。code と fields は renderer の文字列の経路を
`<code>: <JSON>` の形で通り、editor の completion handler が名前付きの拒否に戻します
([文字列から拒否に戻す所](../../src/core/communication/renderconfigeditor.cpp#L6837))。

**live の material が受けられない編集は、書いてから再起動を求めます(✅RS-1)。**runtime に行く ticket(`edit_graph_document`・
`add_authored_pass`・`set_material_route_default`・`restore_render_documents` など、候補を engine に渡して組み直すもの)は、組み直した
候補を生きている material にも当てます([`MaterialGenerationPlanner::validateCompatibility()`](../../src/core/material/materialgenerationplanner.cpp#L1215))。候補の pass が生きている
material を別の**物理の描画の契約**(color の format・depth の format・sample 数・local read の割り当て・attachment の state)で描く
ことになると、material の pipeline をその場で作り直せないので live には載せられません。RS-1 の前はこれを
`render_pipeline_preflight_failed` で断っていたので、blank の project に Graph の palette から最初の material pass を足す編集は
必ず断られました。RS-1 からは、**それが唯一の拒否のときだけ**、editor は文書を commit し、live の pipeline は今のまま残して、
ticket を次の結果で終えます([型付きの拒否のときだけ commit する所](../../src/core/communication/renderconfigeditor.cpp#L6790))。

| field | 中身 |
|---|---|
| `status` / `committed` | `"committed"` / `true` |
| `published_generation` | live の今の世代のまま(新しい世代を publish しません) |
| `restart_required` | `true` |
| `restart_reason` | `code: "material_physical_contract_changed"`・`material`(live の material の名前。空のことがあります)・`material_id`・`pass`(その material を別の契約で描く候補の pass)・`detail`(違う所の配列。`color_formats` / `depth_format` / `sample_count` / `local_read` / `attachment_state` のうち当たるものをこの順で)・`message`(拒否の文) |
| `documents` | RD-1 と同じく、書いた文書の `{reference: sha256 hex}`。`restore_render_documents` で戻せます |

- `restart_reason` は、最初に見つかった 1 つの material と 1 つの pass だけを言います。
- engine は物理の契約の拒否を、ほかの material の検査(受け皿の `material_route_unserved@1`、output schema の変更、`ray_query` の
  宣言漏れ)がすべての material で通った後にだけ投げます([最後に投げる所](../../src/core/material/materialgenerationplanner.cpp#L1303))。別の拒否が隠れたまま commit される
  ことはありません。ただしこの検査より後ろの検査 —— XR の variant の default pass と、publish の直前の準備(active な XR の
  variant が残っているか、graph の runtime の引き継ぎの準備)—— は、この経路では走りません。
- ほかの失敗は今までどおり断って、文書を元に戻します。
- commit は editor 自身が、runtime が呼ぶはずだったのと同じ preset の CAS と複数ファイルの commit で行います。その間に文書が外から
  変わっていれば `external_modification` で断ります。
- 再起動を待つ間の状態は持ちません。文書は commit した内容に進むので、次の編集も同じ live の material に当たり、同じ拒否で
  もう一度 commit と `restart_required` になります。material pass を消して live と合う形に戻せば、普通に live に載ります。engine を
  起動し直すまで、`get_frame_plan` は古い live の世代を言います。
- 上の G-4 の route の既定の再起動と両方が当たる ticket では、`restart_reason` は `material_physical_contract_changed` の方です
  ([RS-1 の restart_reason を載せる所](../../src/core/communication/renderconfigeditor.cpp#L6995))。
- `set_layout` だけの `edit_graph_document`(N-6。runtime に行かない)は対象外です。`material_route_default` を付けたもの(✅RS-2)は runtime に行くので対象です。
- Studio がこの結果を受けて engine を起動し直す流れは、[Studio のノードキャンバス](#studio-のノードキャンバスgraph-タブ)の
  「engine の再起動が要る graph の編集(✅GL-1b)」にあります。
- ディスクからの読み直しとの関係: [`Renderer::reloadRenderPipelineFromDisk()`](../../src/core/vkcore/renderer.cpp#L3705) は live の material の照合を
  外し([`validate_live_materials = false` の所](../../src/core/vkcore/renderer.cpp#L3740))、同じ transaction の中で material の shader と pipeline を
  作り直してから公開し、作り直しが失敗すれば候補世代ごと捨てて旧世代を保ちます([第6章 §6.7](06_rendering.md) の
  「任意 G-buffer / material output ABI・attachment state」)。RS-1 が commit した文書を、その engine 自身の file watcher が拾うことは
  ありません。RS-1 の editor は `--rpc` で起こした engine の中でだけ動き([RPC を受け始める所](../../src/core/appflow/loop.cpp#L701))、
  `--rpc` の engine では watcher の gate が閉じているためです([gate の条件](../../src/core/watch/reloadgate.cpp#L46))。`hot_reload` を宣言して
  gate が開いていても、`--rpc` の下では watcher の変更は render pipeline の参加者に届きません(✅HW-1)。同じ project を
  `--rpc` 無しの窓ありで開いている別の engine があれば、その watcher がファイルを拾い、ディスクの経路で組み直しを試みます
  (この道を端から端まで通す test は見当たりません。未確認)。
- `--rpc` の engine(Studio の engine)に、ディスクで書き換えた render 文書を取り込ませる道は `reload_render_documents`(✅RL-1)です。file watcher の
  経路ではなく編集 RPC の候補として組み直すので、live の material の物理の契約だけが変わる文書は、上の RS-1 と同じく commit して `restart_required` になります
  (コードの読み。test はこの道を通していません。§13.5 の「ループと分解の RPC」の「ディスクで書き換えた render 文書を取り込む」)。
- 編集 RPC の経路でもディスクの経路と同じ作り直しを使い、起動し直さずにその場で載せる案は採りません。起動し直しのままです
  (2026-10-01 利用者の決定)。
- ✅MC-1(`82665619`): Blank(`hdr` feature 無し)の live の material は、G-buffer の互換の表のうち SDR の表(albedo / emissive が `B8G8R8A8_SRGB`)を持ちます。
  graph に足す G-buffer の image の format は `hdr` に合わせます。合わせれば deferred の material pass は起こし直さずに live に載り、HDR の表(`R16G16B16A16_SFLOAT`)で足すと
  `detail: ["color_formats"]` の `restart_required` です([第6章](06_rendering.md) §6.7 の「live の material が持つ G-buffer の format」)。本当に契約が変わる場合をその場で組み直すことは、
  v0 では行わず v0 の後の候補です(2026-10-04 利用者の判断)。test: `engine_primitives_test` の
  [「MC-1 the palette's deferred material pass on the graph's nodes is applied without a restart」](../../test/engine_primitives_test.cpp#L1009)(GPU。SDR の表と HDR の表の 2 つの SECTION)。
  Studio の Graph の palette は ✅MC-1s(`1d10bd4c`)から、Frame Plan の form と同じ表で `hdr` に合わせた format の image を足すので、Blank で deferred の
  material pass を置いても起こし直しません([Studio のノードキャンバス](#studio-のノードキャンバスgraph-タブ)の「render.material_pass と draw.* の node(✅LP-4e)」)。
- test: `renderconfigeditor_test` の
  [「RS-1 a candidate refused live only for a material's physical contract is committed with restart_required」](../../test/renderconfigeditor_test.cpp#L7922)
  (runtime を差し替えて、結果の field・2 回目の編集・ほかの拒否が何も書かないことを見る)と、`engine_primitives_test` の
  [「RS-1 a graph edit the live materials cannot take is committed and asks for a restart」](../../test/engine_primitives_test.cpp#L807)
  (GPU。blank の project に palette と同じ op で `render.material_pass`(deferred_geometry_v1)と G-buffer の image 6 枚を足し、
  起動し直すと frame plan にその pass が出ることまで見る)。

**rendering 節のほかのキーを書く(✅EL-3)。**`edit_rendering_keys {base_source_digest, set?, remove?}` は、root の rendering 節のうち
ほかの RPC が書かないキーを、1 つの ticket で丸ごと書くか消します([`RenderConfigEditorService::editRenderingKeys()`](../../src/core/communication/renderconfigeditor.cpp#L4011))。
それまでこれらのキーを変える道は、文書を丸ごと戻す `restore_render_documents` しかありませんでした。

- 受けるキーは `draw_sort`・`shader_defines`・`snapshots`・`graph_transforms`・`render_strategy`・`xr`・`multisampling`・`resolver_version`
  の 8 つです([受けるキーの表](../../src/core/communication/renderconfigeditor.cpp#L3937))。コードの注記では、どれも世代を組むたびに読み直されるキーで、このキーを変えた ticket は
  それだけでは再起動を求めません。
- `set` の値は丸ごと置き換えます(配列や object を混ぜ合わせません)。無いキーは足します。`remove` はそのメンバーを前後のカンマごと消し、
  ほかのバイトはそのまま残します([消す所](../../src/project/renderconfigdocument.cpp#L776))。同じキーを `set` と `remove` の両方に書く要求と、
  どちらにもキーが無い要求は invalid params です。
- root の文書のバイトが変わらないとき(無いキーの `remove` だけ、など)は、commit の結果に `no_change: true` が付き、`documents` は空です
  ([no_change を返す所](../../src/core/communication/renderconfigeditor.cpp#L7098))。
- 書き先は root の rendering 節だけです。preset を選ぶ root(rendering 節に `pipeline` がある root)では、`multisampling` と
  `resolver_version` を **`rendering_key_not_allowed_here`**(`key`・`reason`)で断ります。`reason` は `multisampling` が
  `preset_root_takes_pipeline_settings_msaa`(preset を選ぶ root の MSAA は `pipeline.settings.msaa` に書く)、`resolver_version` が
  `not_in_preset_root_keys`(preset の config か direct root の rendering 節に書く)です([断る所](../../src/core/communication/renderconfigeditor.cpp#L3965))。
  ほかの 6 つは preset を選ぶ root の rendering 節にも書けます。
- ほかの RPC が書くキーは **`rendering_key_owned`**(`key`・`method`)で断り、`method` に書く RPC の名前を返します([表](../../src/core/communication/renderconfigeditor.cpp#L3946))。

  | キー | `method` |
  |---|---|
  | `features` | `edit_render_features` |
  | `rendering_passes` / `material_routing` | `edit_preset_pass` |
  | `material_route_default` | `set_material_route_default` |
  | `frame_node_order` | `edit_frame_node_order` |
  | `scene_acceleration_structures` / `ray_task_acceleration_structures` | `edit_scene_acceleration_structures` |
  | `ray_task_callable_records` | `edit_ray_callable_records` |
  | `pipeline` | `fork_render_pipeline` |
  | `render_targets` | `edit_render_targets` |
  | `target_planning` | `edit_target_planning`(✅EL-2) |
  | `vulkan_plan_pins` | `edit_vulkan_plan_pins`(✅EL-2) |
  | `vulkan_physical_fragments` | `put_vulkan_physical_fragment`(✅EL-2) |

  EL-3 の時点では 3 つとも、まだ登録されていない `edit_target_planning` を返していました。EL-2 で 3 本の RPC が入り、表もそれぞれの名前に直りました。

- それ以外のキー(`ray_group_layout` や graph root のキーなど)は **`rendering_key_not_editable`**(`key`)です。
- 要求のキーは書く前に全部判定し、1 つでも断れば何も書きません(ticket も取りません)。
- 値の形を見る専用の検査はありません。ほかの authored の編集と同じく、ticket を取る前の preflight(候補の合成と graph の変種の合成)と、
  commit のときの engine の組み直しで判定します([ticket を取る所](../../src/core/communication/renderconfigeditor.cpp#L4830))。preflight で断られた値は、ticket を取らずに
  `render_pipeline_preflight_failed`(`method: "edit_rendering_keys"`)で返ります。preflight を通って組み直しで断られた値は、ticket の結果が
  `committed: false` と `render_pipeline_preflight_failed` になります。どちらも文書は変わりません。test では、preset を選ぶ root の
  `draw_sort: "el3_not_an_object"` が前者、direct root の `draw_sort: {"opaque": {"provider": 5}}` が後者でした。
- ticket の id は authored pass と同じ `render-authored-pass-<N>` で、結果は `get_edit_result` / `step_frame` の `edit_results[]` に来ます。
  commit の結果は RD-1 の `documents` を持つので、`restore_render_documents` で戻せます。
- 組み直しが live の material の物理の契約だけで断られたときは、上の RS-1 と同じく commit して `restart_required` を返します
  (RS-1 の判定は ticket の種類を問わないため。EL-3 の ticket でこれを通す test はありません)。
- test: `renderconfigeditor_test` の
  [「EL-3 edit_rendering_keys writes a direct root's keys whole, removes one, and RD-1 restores them」](../../test/renderconfigeditor_test.cpp#L10952)
  (`draw_sort` と `shader_defines` を書いて組み直しと公開が 1 回ずつ起き、`restart_required` が付かないこと、`remove` がメンバーとカンマだけを
  消すこと、`restore_render_documents` で元のバイトに戻ること)と
  [「EL-3 edit_rendering_keys refuses by name, and one refused key writes nothing」](../../test/renderconfigeditor_test.cpp#L10993)
  (direct root と preset を選ぶ root の 2 SECTION。3 種の拒否、書けるキーと書けないキーを混ぜた要求が何も書かないこと、値の誤りの 2 つの断られ方)。

**graph 全体の計画の決定を書く(✅EL-2)。**第6章 §6.6 の `target_planning`・`vulkan_plan_pins`・`vulkan_physical_fragments` を書く RPC が 3 本あります。
それまでこの 3 つは `edit_rendering_keys` でも書けず(`rendering_key_owned`)、変える道は `restore_render_documents` で文書を丸ごと戻すことだけでした。
3 本とも書き先は root の rendering 節だけで、1 回の要求が 1 つの ticket です(共通の流れは [`queuePlanningKeyEdit()`](../../src/core/communication/renderconfigeditor.cpp#L4223))。
✅GL-3a から、Studio の Frame Plan の Passes / Resources の行の右クリックが `edit_target_planning` を送ります(下の「Frame Plan から graph 全体の決定を書く(✅GL-3a)」)。✅UX-15b から、Frame Plan の Backends タブの Pin this candidate / Unpin が `edit_vulkan_plan_pins` を、Physical plan タブの Freeze / Remove / Eject again が `put_vulkan_physical_fragment` を送ります(下の「下の層の決定を Frame Plan から書く(✅UX-15b)」)。UX-15b の前は、この 2 本を送る UI は Studio にありませんでした。

- **`edit_target_planning {base_source_digest, graph, nodes?, resources?}`**([`editTargetPlanning()`](../../src/core/communication/renderconfigeditor.cpp#L4333))。`nodes` は
  `{<node 名>: {serial?, isolate?} | null}`、`resources` は `{<資源名>: {no_alias?} | null}` で、`target_planning.graphs.<graph>` に名前ごとに混ぜます
  ([混ぜる所](../../src/core/communication/renderconfigeditor.cpp#L4133))。
  - 値が `null` の名前は entry ごと消します。object なら書いた field だけを置き換え、書かなかった field は残します(field の値が `null` ならその field を消します)。
  - field が無くなった entry、名前が無くなった `nodes` / `resources`、中身が無くなった graph と `graphs` は消えます。`target_planning` 自体も空になれば消えます
    (`profile` や `diagnostics` が残っていれば残ります。この 2 つを書く RPC は無く、文書を直すか `restore_render_documents` です)。
  - `nodes` と `resources` のどちらにも名前が無い要求、名前の値が object でも `null` でもない要求は invalid params です。field の名前と値の型はここでは見ず、
    下の「値の誤り」と同じく組み直しで判定します。
  - 名前は `get_frame_plan` が見せる展開名です。今の合成の frame graph に無い名前は、ticket を取らずに **`target_planning_name_unknown`**(`graph`・`kind`・`name`。
    `kind` は `node` / `resource` / `graph`)で断ります([断る所](../../src/core/communication/renderconfigeditor.cpp#L4362))。
  - 名前の表は、合成した config を frame graph として読んだ段のものです([`planningNamesOf()`](../../src/core/communication/renderconfigeditor.cpp#L4111))。node は frame graph の node(pass と compute task)、
    資源は宣言した target・history の target・node が書く資源と、node が読んでいれば `swapchain` です。`graph_transforms`・`render_strategy`・subgraph の置き換え
    (region の provider)はこの後の compile の段で当たり、`__scene_acceleration_structure` も compile が足す node なので、これらが作る node の名前は
    `target_planning_name_unknown` になります(コードの読み。test はありません)。
  - 消すとき(`null`)は緩く、今の合成に無い名前でも、root の entry がその名前を持っていれば消せます(組み直しで名前が無くなった後の掃除のため)。
  - ループの回の node(`<loop>/<element>/<node>`)は合成した config に在るので名前の表は通りますが、`target_planning` の node に展開名を書くことは LP-1a が
    `graph.loop_expansion_referenced@1` で断ります([第6章](06_rendering.md) の「展開名は書けません」)。コードの読みでは、この要求は組み直しの preflight で
    断られます(test は無く、未確認)。ループの回ごとの決定は、root の `fold_groups` にローカル名(`{node: <ループ>, element: N}`)で書きます(✅FG-1。下の「断片の決定を宣言する」)。
  - ✅FG-1 から、検査済みの fold group が同じ graph・同じ名前・同じ field を `true` に決めているとき、その field に `false` を書く要求は、ticket を取らずに
    **`graph.fold_decision_conflict@1`**(`group`・`name`・`field`)で断ります([EL-2 から断る所](../../src/core/communication/renderconfigeditor.cpp#L4395))。比べる相手は、今の合成が
    fold group から足した決定です。
- **`edit_vulkan_plan_pins {base_source_digest, family, pin, graph?}`** と **`put_vulkan_physical_fragment {base_source_digest, family, fragment, graph?}`**
  ([`editVulkanPlanPins()`](../../src/core/communication/renderconfigeditor.cpp#L4416) / [`putVulkanPhysicalFragment()`](../../src/core/communication/renderconfigeditor.cpp#L4433))。
  - `family` は `flat` / `preview` / `xr` のどれかです。package(`pin` / `fragment`)は compiler が読むのと同じ parser で形を見ます([形を見る所](../../src/core/communication/renderconfigeditor.cpp#L4164))。
  - package は、その family の配列のうち同じ `graph` の package を置き換え、無ければ末尾に足します([置き換える所](../../src/core/communication/renderconfigeditor.cpp#L4193))。
  - `null` は、`graph` があればその family のその graph の package を、無ければその family の package を全部消します。package を渡すときに `graph` を付けると
    invalid params です(package が自分の `graph` を持つため)。family の配列が空になればその family を、family が 1 つも無くなればキーごと消します。
  - fragment の fingerprint が今の plan と合うかは、commit の組み直しで runtime の linker が判定します。合わなければ ticket の結果(`get_edit_result` /
    `step_frame` の `edit_results[]`)が **`physical_fragment_stale`**(`family`・`graph`・`fingerprint`・`expected`・`actual`)になり、文書は変わりません
    ([組み直しの結果から作る所](../../src/core/communication/renderconfigeditor.cpp#L6867)、[`physicalFragmentStaleRefusal()`](../../src/core/communication/renderconfigeditor.cpp#L4299))。
    `fingerprint` は `logical_graph`(論理 graph が変わった)か `automatic_plan`(自動計画の前提が変わった)、`expected` は今の plan の値、`actual` は package の値です
    ([linker が投げる所](../../src/project/vulkanphysicalfragment.cpp#L2857))。`family` は root の節が同じ graph の package を持つ family で、preset が持つ package
    のときは付きません。ticket の前の preflight は fragment を link しないので、この拒否は ticket を取った後にだけ来ます。
  - pin の食い違い(第6章の `pin package is stale` / `pinned backend candidate is infeasible`)には専用の code がなく、下の「値の誤り」と同じ形で返ります(コードの読み)。
- 3 本に共通:
  - preset を選ぶ root で、preset の `config` が同じキーを持っていれば **`rendering_key_not_allowed_here`**(`key`・`reason: "preset_declares_key"`)です。
    preset を fork(`fork_render_pipeline`)してそちらを直します([断る所](../../src/core/communication/renderconfigeditor.cpp#L3992))。preset が持っていなければ、preset を選ぶ root の rendering 節にも書けます。
  - 値の形の誤り(受けない field、`family` の綴り、parser が断る package など)は invalid params で、ticket を取りません。意味の誤りは EL-3 と同じく、
    preflight か commit の組み直しの `render_pipeline_preflight_failed` です。
  - ticket の id は `render-authored-pass-<N>`、結果の `operation` はメソッド名です。commit の結果は RD-1 の `documents` を持つので、`restore_render_documents`
    で戻せます。戻せば frame plan も戻ります(下の GPU の test)。
- frame plan(`get_frame_plan.physical_target_plan`)での見え方:
  - `isolate` した node が書く target と、`no_alias` の資源は `alias_groups` から外れます。
  - pin は `applied_pin_package` と `backend_selection.selected_candidate`、fragment は `applied_physical_fragment` に出ます。
  - `decisions[]` の要素(`{id, subject, selected, detail}` と GL-2a の `from_nodes`)には、その決定が `target_planning` から来たことを示す印はありません。
    ✅FG-1 から、fold group が足した決定は `pelican.plan.fold_decision_applied@1` の要素として並び、`from_fold` に group の名前が付きます(下の「断片の決定を宣言する」)。
  - `isolate` で rendering scope が分かれるのは tile backend の候補が選ばれたときだけで、desktop の GPU では scope の差として見えません(GPU の test の注記)。
- test: `renderconfigeditor_test` の
  [「EL-2 edit_target_planning merges a graph's names into the root, erases them, and refuses an unknown one」](../../test/renderconfigeditor_test.cpp#L11070)
  (direct root: 混ぜる・field の置き換え・3 種の `target_planning_name_unknown`・形の誤りが invalid params で何も書かないこと・`null` で名前とキーが消えること・
  `restore_render_documents` で戻ること。preset が `target_planning` を持つ root: `rendering_key_not_allowed_here`)と、`engine_primitives_test` の
  [「EL-2 target planning, plan pins and physical fragments written over RPC reach the frame plan」](../../test/engine_primitives_test.cpp#L2087)
  (GPU。blank の project に同じ形の target 3 枚の鎖を足し、`isolate` / `no_alias` で alias が外れて `null` で戻ること、eject した pin と fragment を書き戻すと
  `applied_*` に出ること、fingerprint を 1 文字変えた fragment が `physical_fragment_stale` で文書も plan も変わらないこと、restore で文書と plan が戻ること)。
  着地の W1 で 72 件が緑(統合側の報告)。変異の確認は検証の方針により行っていません。

**断片の決定を宣言する(✅FG-1)。**root の graph 文書に `fold_groups` を書くと、root の node の組(断片)に計画の決定 —— `serial` / `isolate` / `no_alias` —— を
**ローカル名で**付けられます。組み立てのたびに engine が展開名へ写して `target_planning` に足すので、ループの回の数が変わっても宣言を書き直す必要はありません。
設計は [design_graph_layers](../design_graph_layers.md) §5.2、読みと写しは [`graphfold.hpp`](../../src/project/graphfold.hpp#L80) です。✅GL-3a から、Studio の Graph タブがこれを読み・送ります(下の「root の node を fold group にまとめる(✅GL-3a)」)。

```json
"fold_groups": {
  "post": {
    "nodes": ["fold_a", "fold_b", "fold_image", "blur"],
    "folded": {
      "kind": "decisions",
      "decisions": {
        "isolate":  [{ "node": "blur", "element": 1 }],
        "no_alias": [{ "node": "fold_image" }]
      },
      "verified": { "digest": "<64 桁の小文字 hex>", "at": "2026-10-02T12:00:00Z", "frames": 3 }
    }
  }
}
```

(`blur` は `graph.loop`、`fold_image` は `render.image`。FG-1 の test の文書と同じ名前です。)

- **形**: `fold_groups.<group> = {nodes: [root の node id], folded: {kind: "decisions", decisions?: {serial?, isolate?, no_alias?}, verified?: {digest, at, frames?}}}`
  ([`parseGraphFoldGroups()`](../../src/project/graphfold.cpp#L128))。`nodes` は空でない配列で、同じ id は 2 回書けません。知らない鍵、`verified.digest` が 64 桁の小文字 hex でないなどの
  形の誤りは文書の誤りです(`fold_nodes` では invalid params)。`nodes` の node が文書に在るかはここでは見ません(消えた node は下の digest を変え、group は `stale` になります)。
- **主語** `{node, element?, port?}`。`node` は group の `nodes` のどれかで、外を指すと **`graph.fold_subject_outside@1`**(`group`・`subject`)です。
  - `serial` / `isolate`: node が作った pass と compute task。`graph.loop` と `graph.instance` の node はその展開の全部、`element` を付けるとループのその回だけ。
    `render.image` / `render.buffer` の node は pass を作らないので、書いても何も足しません。
  - `no_alias`: 資源。`render.image` / `render.buffer` の node 自身、ループ / instance の node ならそれが展開した target と buffer の全部(`element` でその回)、
    `{node: <書き手>, port}` ならその node の入力 `port` につながった `render.image` / `render.buffer`(`render.target_ref` なら指す target)。`port` を書けるのは `no_alias` だけです。
- `kind` は `decisions` だけです。ほか(`replacement` など)は **`graph.fold_kind_unsupported@1`**(`group`・`kind`)。
- **写す段**([`applyGraphFoldGroups()`](../../src/project/graphfold.cpp#L269)): 合成が GL-2a の `graph_origin` を provenance に載せた直後、`target_planning` を compile する前に
  呼ばれます([呼ぶ所](../../src/project/featurecompose.cpp#L4242))。pass と task は provenance の `loop_origin` / `graph_origin` から作った root の node を引き、target と buffer は
  `loop_origin` か展開名の先頭(`<node>/…`)で引きます。feature の provenance は見ません。足す先は、pass ならその pass の family の graph、compute task と資源は全 family の graph の
  `target_planning.graphs.<graph>.nodes.<展開名>.<field>` / `.resources.<展開名>.no_alias` で、値は `true` です。どこにも当たらない主語(ループにもう無い回など)は何も足しません。
  合成した config の `target_planning` が同じ entry を `false` にしていれば、組み立てが **`graph.fold_decision_conflict@1`**(`group`・`name`・`field`)で断られます。
- **digest**([`graphFoldGroupDigest()`](../../src/project/graphfold.cpp#L185)): group の `nodes` の root の node(id の順)を、入力のうち group の他の node から来る線だけを残して入れ、
  それらが使う definition(instance の definition とループの各分岐の body。中の node が使う definition も辿る。definition の `layout` は除く)を並べた JSON を、鍵を整列して
  dump した sha256(64 桁の小文字 hex)です。root の `layout` と group の外の node は入りません。node の `parameters` は入るので、ループの `count` を変えると digest が変わり、
  印は古くなります(決定の宣言は古くならず、検査し直すまで効かないだけです)。文書に無い node は `null` として入ります。
- **印と失効**: `verified.digest` が今の digest と同じ group(`state: "verified"`)の決定だけが効きます。`verified` が無い(`unverified`)・digest が違う(`stale`)group は
  決定を捨てて開いた形で走り、frame plan の `diagnostics` に `{kind: "fold_unverified" | "fold_stale", subject: <group>, detail: {verified_digest?, current_digest}}` が出ます
  (`verified_digest` は `fold_stale` だけ。組み立ては断りません。[診断にする所](../../src/project/renderpipeline.cpp#L1404))。
- **frame plan での見え方**: 効いた決定は、最上位の `fold_decisions` `[{group, kind: "node" | "resource", name, field}]`(その frame graph の分。無ければ鍵ごと無い)と、
  `physical_target_plan.decisions[]` の `{id: "pelican.plan.fold_decision_applied@1", subject: <展開名>, selected: <field>, detail, from_nodes, from_fold: <group>}` に出ます
  ([足す所](../../src/core/renderingpass/frameplanner.cpp#L2427))。`detail` は `fold group <group> declared it in target_planning nodes` / `… resources` です。
- **`get_fold_groups {}`** → `{source_digest, fold_groups: [{group, nodes, kind, digest, verified?, state}]}`(group 名の順。`state` は `verified` / `stale` / `unverified`。
  読むだけ。[`getFoldGroups()`](../../src/core/communication/renderconfigeditor.cpp#L3040))。
- **`fold_nodes {base_source_digest, group, nodes, folded}`**([`foldNodes()`](../../src/core/communication/renderconfigeditor.cpp#L3079)): group 1 つを書く root 編集の ticket
  (id は `render-config-edit-<N>`、結果の `operation` は `fold_nodes`。RD-1 の `documents` を持つので `restore_render_documents` で戻せます)。
  - `folded` は `{kind, decisions?}` で、**`verified` を書くと invalid params** です(書くのは `verify_fold` だけ)。`nodes` は root の node id で、root に無い id は invalid params。
  - 同じ名前の group が在れば丸ごと置き換えます。書き直すと `verified` も消えます。
  - `folded: null`(`nodes` は書かない)で group を消し、最後の group が消えると `fold_groups` の鍵ごと消えます(ほかのバイトはそのまま)。
  - 名前付きの拒否(`graph.fold_kind_unsupported@1` / `graph.fold_subject_outside@1`)は、その code と fields のまま返り、文書は変わりません。
- test: `rendercompilerprogram_test` の
  [「FG-1 a verified fold group's decisions reach target_planning and the frame plan by the expanded names」](../../test/rendercompilerprogram_test.cpp#L2090)
  (本番の compiler。ループの `isolate` が回の数 3 と 2 のどちらでも展開名で効き、`element` で 1 回だけ、`render.image` の `no_alias` で alias から外れ、決定が `from_fold` 付きで出ること。
  root と definition の `layout`・group の外の node が digest を変えないこと。group の node の parameter を変えると `fold_stale`、印を消すと `fold_unverified` で決定が効かないこと。
  3 種の名前付きの拒否)と、`renderconfigeditor_test` の
  [「FG-1 fold_nodes declares a fold group, get_fold_groups reads its digest and state, and RD-1 takes it back」](../../test/renderconfigeditor_test.cpp#L11174)
  (宣言と `unverified`、RD-1 で戻ること、`verified` を書く要求と無い node が invalid params、名前付きの拒否、印のある group と EL-2 の `false` の衝突、`folded: null`)。
  着地の W1 で 106 件が緑(統合側の報告)。

**断片を検査して印を付ける —— `verify_fold`(✅FG-2)。**`fold_nodes` は印(`verified`)を書かないので、宣言した直後の group は `unverified` で、決定は効きません。
印を書くのは `verify_fold` だけです。group の開いた形と決定ありの形を engine 自身の player に headless で描かせ、絵が同じで、Vulkan validation の error が増えなければ印を書きます。
設計は [design_graph_layers](../design_graph_layers.md) §5.3 です。✅GL-4a から、Studio の Graph タブの Verify がこれを送ります(下の「fold group を検査する —— Verify(✅GL-4a)」)。

- **要求** `verify_fold {base_source_digest, group, frames?, warmup_seconds?, tolerance?}`([`verifyFold()`](../../src/core/communication/renderconfigeditor.cpp#L3135))。
  `frames` は比べる frame の数(1〜600、既定 3)、`warmup_seconds` は描いて捨てる時間(0〜3600、既定 30)、`tolerance` は 1 frame の中で違ってよい画素の数(既定 0 = byte の一致)。
  範囲の外・形の誤り・root が宣言していない `group` は invalid params です。
- **答え**: すぐ `{ticket, status: "accepted", base_source_digest}` を返し(ticket は `render-config-edit-<N>`)、frame も RPC も止めません。描いている間の `get_edit_result` は
  `{ticket, status: "running", operation: "verify_fold", group}` です。描き終わった後の frame の境界で判定し、通れば `folded.verified = {digest, at, frames}` を同じ ticket の番号の
  root 編集として書きます(その間は `pending` と読めることがあります)。committed の結果には `group` と `verified` が付き、RD-1 の `documents` を持つので
  `restore_render_documents` で戻せます。印の ticket は、既に並んでいる ticket より先に commit されます([`harvestFoldVerifies()`](../../src/core/communication/renderconfigeditor.cpp#L3224))。
- **すぐ断るもの**:
  - **`fold_verify_unavailable`**(`reason`): `headless` / `golden` / `replay`(`--replay` か入力の再生中)/ `executable_unknown`(自分の exe の path を取れない。Windows 以外は常にこれ)/
    `no_runner`(子を起こす口を持たない構成)([判定](../../src/core/communication/editorruntimefactory.cpp#L2364))。コードには engine の終了で止めた run の `shutdown` もありますが、
    終了の時は editor ごと消えるので、答えとして読む道は見当たりません(コードの読み)。
  - **`fold_verify_busy`**(`group`・`ticket`): 同じ group を検査している間。ほかの group と、ほかの編集は止めません。
  - `gate_closed`・`external_modification`(ほかの編集と同じ)。
- **走らせ方**([`runFoldVerifyProcesses()`](../../src/core/communication/graphfoldverify.cpp#L438)):
  - 受けた時点の root(`base_source_digest` の文書)から 2 つの形を作ります —— group の `verified` を外した形(開いた形)と、今の digest で `verified` を付けた形(決定ありの形)。
    ほかの文書は editor が持つ managed fragment を写します。
  - project を丸ごと(最上位の `.git` を除く)system の temp の下の `pelican_fold_verify_<pid>_…` に写し、root を書き換えながら同じ exe を 3 回、順に起こします:
    開いた形 ×2(対照)、決定ありの形 ×1。終われば写しを消します。
  - 子の引数は `--headless --project <写し> --frames <frames> --fps <親の fps> [--size <project の窓の大きさ>] [--start-time <warmup_seconds>] --render-out <run>/frame_%d.png
    --vulkan-validation on --log-file <run>/pelican.log` で、親が `--allow-absolute-paths` / `--game-logic` で起きていればそれも付けます
    ([組み立て](../../src/core/communication/graphfoldverify.cpp#L291)、[親から読む所](../../src/core/communication/editorruntimefactory.cpp#L1976))。
    warm-up は player の `--start-time` で、その時刻までの fixed-step の frame を描いて捨てます(時刻ではなく frame の数で数えます)。
- **比べ方**([`judgeFoldVerifyRuns()`](../../src/core/communication/graphfoldverify.cpp#L388)): 各 frame の PNG を RGBA8 に読み、4 byte のどれかが違う画素を数えます(大きさが違えば
  大きい方の全画素)。validation の error は、子の stdout と stderr にある `Validation Error` の数です。判定は次の順で、最初に当たったものが答えです。
  1. 写しが作れない → **`fold_verify_failed`**(`run: "setup"`、`detail: {message}`)。対照のどちらかが描けない(0 以外で終わる・frame が無い・時間切れ)→ `fold_verify_failed`
     (`run: "open"`、`detail: {exit_code, message}`)。何も比べていません
  2. 対照どうしが、ある frame で `tolerance` を超えて違う → **`graph.fold_verify_unstable@1`**(`detail: {frame, differing_pixels}`)。決定のせいではなく、比べられません
  3. 決定ありの形が描けない → **`graph.fold_mismatch@1`**(`what: "compile"`、`detail: {exit_code, message}`)
  4. 決定ありの形が 1 回目の対照と `tolerance` を超えて違う → `graph.fold_mismatch@1`(`what: "pixels"`、`detail: {frame, differing_pixels}`。`frame` は 1 から)
  5. 決定ありの形の validation の error が、対照の多い方より多い → `graph.fold_mismatch@1`(`what: "validation"`、`detail: {validation_errors, control_validation_errors}`)

  どの拒否にも `group` と `ticket` が付き、文書は変わりません。
- 描いている間に root が変われば、印の ticket が CAS で **`external_modification`** になり、何も書きません。
- **子の終わらせ方**: 子は 1 つずつ kill-on-close の job object に入れ、標準入出力はファイルにします。900 s を超えた子と、engine の終了時(猶予 3 s)にまだ走っている子は、
  その job の handle を閉じて終わらせます(名前や pid で探しません。[`runChild()`](../../src/core/communication/graphfoldverify.cpp#L157))。editor は終わるときに worker を待ちます。
- **時間**: 実装者の報告では、160x90・Debug・validation・`frames: 2`・`warmup_seconds: 0.5` で約 17 s でした(未確認)。既定値(30 s の warm-up、3 frame)は測っていません。
- **残した穴**: 子は project の既定の scene を描きます(引数に scene を渡しません)。warm-up は frame の数なので、速い Release では実時間で短くなります(NVIDIA の二段の
  compile の差し替えより前に描きうる。対照どうしの比較が違いを捕まえます)。子を起こせるのは Windows だけです。
- test: `renderconfigeditor_test` の
  [「FG-2 verify_fold draws the open form twice and the decided form once, and marks the group verified only when the pictures agree」](../../test/renderconfigeditor_test.cpp#L11348)
  (runner を差し替えて、通れば印と RD-1 の 1 文書・次の組み立てで決定が効くこと、子に渡す 2 つの root、4 種の拒否と `tolerance`)、
  [「FG-2 verify_fold refuses a group that is being verified, and drops runs of a document that changed」](../../test/renderconfigeditor_test.cpp#L11466)
  (`no_runner`、`fold_verify_busy`、無い group と `frames: 0` が invalid params、走っている間の編集で `external_modification`、終わるときに worker を待つこと)と、GPU の
  [`fg2_verify_fold_windowed.py`](../../test/fg2_verify_fold_windowed.py)(窓ありの engine から実際に子を 3 回起こし、描いている間も frame が進み、committed の後に frame plan に
  `fold_decision_applied@1` が出ること。headless の engine では `fold_verify_unavailable {reason: "headless"}`)。着地の W1 で 33 件が緑(統合側の報告)。

**計画を詰める手がかり(✅PO-1)。**Blank から組んだ project を最適化しようとして(GB-2)見えなかった 3 つを、読む側に足しました。判定の規則は変えていません。

- **alias の候補から外れた理由**: `physical_target_plan.resources[].reason`(と `pelican.plan.resource_representation@1` の `detail`)が
  `logical materialization requirement is required` か `ResourcePattern requires contents to be stored` のとき、その後に `: <原因>` が付きます(原因が複数なら `; ` で区切る)。
  原因の文と fingerprint への影響は [第6章](06_rendering.md) §6.6 の「alias から外れた理由(✅PO-1)」。
- **frame plan の `diagnostics`** に `unread_output`(誰も読まない出力)と `unused_binding`(shader が読まない入力)。形は下の「`get_frame_plan` に足されたもの」の表です。
- **`get_status.gpu_timing`** に `generation`(平均の窓が始まった runtime の世代)と `samples`(窓の論理フレームの数)。新しい世代が公開されると窓は空から始まります
  ([第6章](06_rendering.md) §6.14 の「GPU タイミング」)。
- test: `renderingsamplecount_test` の
  [「PO-1 a target kept out of runtime aliasing by its usage names the usage in the plan's reason」](../../test/renderingsamplecount_test.cpp#L1675)、`frameplanner_test` の
  [「PO-1 a write nothing consumes is an unread_output diagnostic of the frame plan」](../../test/frameplanner_test.cpp#L619)、`engine_primitives_test` の
  [「PO-1 the frame plan names a binding the shader never reads and an output nobody reads, through the running renderer」](../../test/engine_primitives_test.cpp#L1590)
  と [「PO-1 get_status's GPU time starts a new window when an edit publishes a new generation」](../../test/engine_primitives_test.cpp#L1624)(後の 2 本は GPU)。
  着地の W1 で 115 件が緑(統合側の報告)。

**`get_status` の GPU 時間を軽く(✅ST-1、`1efecd29`)。**`get_status.gpu_timing.nodes` は最後の 1 論理フレームの sample だけになり、窓の node ごとの平均は新しい `node_averages` で読みます(`get_gpu_timing` の `nodes` と同じ行・同じ値。`get_gpu_timing` は変わっていません)。ST-1 の前は履歴の全部の sample を並べていて、ループを展開した graph で Play の後の `get_status` が Studio の 5 秒の制限を越えていました。形と経緯は [第6章](06_rendering.md) §6.14 の「GPU タイミング」。

**音の解析を焼く —— `bake_audio_analysis` と `list_derived_assets`(✅AB-1、`892a1455`)。**音の rms・peak・帯域の値を表(`R32_SFLOAT` の KTX2)に焼き、隣に manifest `<out>.import.json` を書く RPC と、焼いた file が古いかを読む RPC です。表・manifest・settings の形は [第5章](05_assets.md) §5.8「音の解析を焼いた表」、読み方は [第6章](06_rendering.md) §6.18「焼いた音の解析の表を読む」です。

- **要求** `bake_audio_analysis {asset, out, settings?}`([`RenderConfigEditorService::bakeAudioAnalysis()`](../../src/core/communication/renderconfigaudioanalysis.cpp#L36))。`asset` と `out` は空でない文字列で、project の参照として解きます(`PathResolver::resolveProjectRef`。test は project からの相対 `sounds/tone.wav` / `tables/tone.ktx2`)。`out` は `.ktx2` で終わる名前です。この 3 つの外の鍵、`asset` / `out` の欠けと `.ktx2` でない `out` は invalid params です。
- **答え**: すぐ `{ticket, status: "accepted", asset, out}` を返します(ticket は `render-config-edit-<N>`)。焼くのは `std::async` の thread で、frame と RPC は止まりません([worker を始める所](../../src/core/communication/renderconfigaudioanalysis.cpp#L77))。`get_edit_result` は焼いている間 `{ticket, status: "running", operation: "bake_audio_analysis", asset, out}` で、焼き終わった後の frame の境界で committed か rejected になります([`harvestAudioBakes()`](../../src/core/communication/renderconfigaudioanalysis.cpp#L113)。`commitPending()` が最初に呼びます)。
- **committed**: `{ticket, status: "committed", committed: true, operation: "bake_audio_analysis", asset, out, manifest, layout}`。`manifest` は `<out>.import.json`(要求の綴りのまま)、`layout` は `{width, rows_per_feature, sample_count, hop_seconds, features}` です。例(test。`settings: {"bands": 4}`): `{"out": "tables/tone.ktx2", "manifest": "tables/tone.ktx2.import.json", "layout": {"width": 4096, "rows_per_feature": 1, "sample_count": 101, "hop_seconds": 0.01, "features": ["rms", "peak", "band:0", "band:1", "band:2", "band:3"]}}`。
- render 文書は書かず、gate と `base_source_digest` の CAS も取りません(要求にその鍵がありません。コードの読み)。焼いた表は、graph を組み直した新しい世代で値の node に届きます。✅IA-3(`26473e75`)から `reload_render_documents` が表と manifest の中身も比べるので、焼き直した後の reload で新しい世代になれば届きます(下の「ディスクで書き換えた render 文書」の IA-3 の項)。
- **すぐ断るもの**(`{status: "rejected", committed: false, error: {code, message, ...}}`): `audio.analysis_settings_invalid@1` `{field, reason, value?}`、同じ `out`(解いた path)を焼いている間の `audio.analysis_busy@1` `{out, ticket}`、`PELICAN_WITH_AUDIO=OFF` の build の `audio.analysis_unavailable@1` `{reason: "built_without_audio"}`。
- **ticket で断るもの**: `audio.format_unsupported@1`(WAV / Ogg Vorbis でない。AU-2 の fields に要求の `asset` / `out`)、`audio.analysis_failed@1` `{asset, out, message}`(読めない・decode できない・書けない)。どちらも `ticket` が付き、表は書きません(test は mp3 の名前の file で確かめています)。
- **出力が開かれているとき(✅AB-2、`b3003b2a`)**: 表と manifest は `<out>.tmp` などに書いてから置き換え、Windows で読む側(file の見張り・hash・表や texture の読み込み)が開いていて置き換えが断られると、50 ms から倍々に合計 2 秒までやり直します。開かれたままなら `audio.analysis_failed@1` `{asset, out, message, reason: "output_locked", path}` の rejected です([RPC で `path` を要求の綴りに直す所](../../src/core/communication/renderconfigaudioanalysis.cpp#L90))。`path` は開かれていた方で、要求の `out` か `<out>.import.json` の綴りです。例(test。rejected の `error`): `{"code": "audio.analysis_failed@1", "reason": "output_locked", "path": "tables/tone.ktx2", "asset": "sounds/tone.wav", "out": "tables/tone.ktx2", ...}`。一時 file は残らず、元の表と manifest はそのままです。やり直しの規則と読む側の直しは [第5章](05_assets.md) §5.8 の「読む側が開いている間の置き換え」です。
- editor が終わるときは、走っている bake の終わりを待ちます。

**`list_derived_assets {}`**([`RenderConfigEditorService::listDerivedAssets()`](../../src/core/communication/renderconfigaudioanalysis.cpp#L146))は配列を返します。params は空の object か省略だけで、ほかは invalid params です。

```json
[{"asset": "tables/tone.ktx2", "manifest": "tables/tone.ktx2.import.json",
  "tool": {"name": "pelican.audio_analysis", "version": "1"}, "source": "sounds/tone.wav",
  "derived_from": {"source": "sounds/tone.wav", "tool": {"name": "pelican.audio_analysis", "version": "1"}},
  "stale": false, "stale_reasons": []}]
```

(test の値)。project の asset store の root の下(store が無ければ project root)の `*.import.json` を再帰で全部読み、path の順に並べます。音の解析のものに限らず、読める `pelican.import` の manifest は全部出ます(`asset` は manifest の最初の output)。path は project root の中なら相対(`/` 区切り)、外なら絶対です。`stale_reasons` は `source_missing` / `source_changed` / `output_missing` / `output_changed` / `settings_changed` / `tool_version`(最後の 2 つは `pelican.audio_analysis` の manifest だけ)です。test では、元の音を書き換えると `["source_changed"]`、焼き直すと `stale: false`、manifest の `settings` だけを書き換えると `["settings_changed"]` になります。読むだけで何も書きません。

`get_frame_plan` の `initial_asset` にも、asset の隣に manifest があれば同じ `derived_from` / `stale` / `stale_reasons` が付きます(下の「`get_frame_plan` に足されたもの」の表)。

test: `ab1_audio_analysis_test` の [「AB-1 bake_audio_analysis runs on a ticket, and list_derived_assets calls the table stale once its source or its settings change」](../../test/ab1_audio_analysis_test.cpp#L259)(非 GPU。accepted → committed と `layout`、`list_derived_assets` の値、stale の 3 つの場合、settings と形式の拒否)。

Render Featuresの **Parameters...**で、選択中 feature instance の scalar パラメータを編集できます
(`edit_render_features` の `set_parameters`)。欄は engine の `parameters` 宣言から作られ、
`get_render_features.feature_instances[].parameter_schema` がその宣言です。**materialize 済みの
feature でも `parameter_schema` は返ります** —— ただし宣言の `default` は、その派生が焼き込まれた
`bound_parameters` の値で上書きされます([上書き箇所](../../src/core/communication/renderconfigeditor.cpp#L1219))。
SC-1 の前は、元の engine 既定値は RPC から見えませんでした。✅SC-1(`14e10b19`)から、materialize 済みの feature の宣言のうち
元の `default` を持つものには、その元の値が **`engine_default`** で並びます(`default` の意味は変わりません。materialize していない feature の宣言には付きません。
[元の default を `engine_default` に写す所](../../src/core/communication/renderconfigeditor.cpp#L1231))。
例(test の期待値): `sky_ambient` の `color_r` を 0.7 で materialize した後の宣言は `default: 0.7`・`engine_default: 0.12`。
test: `renderconfigeditor_test` の [「SC-1 a materialized feature's parameter schema keeps the engine default」](../../test/renderconfigeditor_test.cpp#L4130)。
`parameter_schema` が `null` の feature は「編集できるパラメータを宣言していない」ので、
`reason` を表示するだけで dialog は開きません。

Render Featuresの **Make editable...**は materialize(§13.5 の表)です。押せるのは
`feature_instances[].materializable` が true の項目だけで、false のときは `materialize_reason` が出ます ——
engine preset の feature なら「先に project へ fork する」、すでに `bound_parameters` を持つ派生なら
「これはもう materialize 済み」の 2 通りです([materializable の判定](../../src/core/communication/renderconfigeditor.cpp#L1208))。
保存先は未作成の `project://` ファイルで、**Apply**で元 instance の参照がその派生に差し替わります。
materialize しても scalar の宣言は消えません —— 宣言を残したまま `bound_parameters` に値を持つので、
**materialize の後も Parameters... で編集できます**。

**feature が要るものを足す前に言い、1 回の押下で直す —— Add with targets・Place … in the Graph・Fix(✅UX-12b、`99f645fb`)**: engine の ✅FR-1 / FR-2(上の「feature と snapshot を足す前に読む」)が返す材料を、Render Features の操作にしました。UX-10 の `gui_settings_choose` で、Blank から Graph で組んだ pipeline に `sky_ambient` を足すことと `shadow_directional` の `cascade_count` を 3 にすることが engine に断られていました(仕様の背景)。ボタンは notice の下の列(`pelican.renderFeatures.offers`)に出ます。

- **足す前に読む**: catalog で選んで **Add** を押すと、今までどおり `Pending add: <名前>` を出して Apply を待ちます。その feature の catalog の項目に `required_targets` か `insertion` があれば、続けて `get_render_authoring_context` と `get_graph_instances` を読み、次の 2 つを見ます([Add の後に要るものを見る所](../../src/devstudio/view/renderfeatureswidget.cpp#L2910))。
  - **挿入先が無い**(先に見る): `insertion` の anchor が、context の `graphs[].passes` の名前にも Graph の root の node の id にも無ければ、notice が `<feature> goes after <anchor>, which this pipeline does not have. Place <名前> first.` を言い、**Place <名前> in the Graph**(`pelican.renderFeatures.placeAnchor`)が出ます。engine の canonical anchor(`sprite` / `post_main` / `tonemap` / `post_ldr` / `pelican_ui` / `debug_draw` / `debug_text` / `imgui`)は、どの pipeline にもあるものとして数えます([無い挿入先を数える `missingInsertionAnchors()`](../../src/devstudio/model/featurerefusalfix.hpp#L79))。`<名前>` は、anchor と同じ名前の fullscreen の雛形があればその表示名(`deferred_lighting` なら `Deferred lighting`)、無ければ anchor です。target だけを足してもまだ断られるので、挿入先を先に言います(コードの注記)。
  - **target が足りない**: 挿入先があって、`required_targets` のうち context の `render_targets` に無いものがあれば、notice が `<feature> needs <target> (<種類>). Add it with the feature?`(2 つ以上なら `sky_ambient needs lit_color (color) and scene_depth (depth). Add them with the feature?` の形)を言い、**Add with targets**(`pelican.renderFeatures.addWithTargets`)が出ます([問いの文 `requiredTargetsQuestion()`](../../src/devstudio/model/featurerefusalfix.hpp#L49))。`<種類>` は `pelican.render.depth@1` なら `depth`、`color_signal` / `legacy_opaque_image` なら `color`、ほかは `image` です。
- **Add with targets**: 押すと staged の add を捨て、文書を読み直してから、次の 3 つの編集を送ります([target の宣言の op を作る所](../../src/devstudio/model/featurerefusalfix.cpp#L105))。UX-12b の着地時は、これを順に 1 つずつ送ってそれぞれの commit を待っていました。✅UX-12d から、3 つは 1 本の `edit_batch` の edits で、ticket は 1 つです(下の「書き手を繋ぎ、1 本の edit_batch で足す(✅UX-12d)」)。
  1. `edit_render_targets` で、足りない target を宣言します。depth は `D32_SFLOAT`・`DEPTH_STENCIL_ATTACHMENT`、ほかは `R16G16B16A16_SFLOAT`・`COLOR_ATTACHMENT` と `SAMPLED` で、どちらも `extent_scale: 1` に `required_targets` の `usage` を足します。
  2. `edit_graph_document` で、Graph の root に、その型の `render.target_ref` を足します(id は `<target>_ref`、使われていれば `<target>_ref_2` …。同じ名前の target_ref が既にあれば足しません。[target_ref を足す op を作る所](../../src/devstudio/model/featurerefusalfix.cpp#L127))。
  3. `edit_render_features` で feature を `add` します。通れば `Added <feature> with the render targets it needs.` です。
  - UX-12b の着地時は、2 か 3 が断られたら、足した target_ref を外し、宣言した target を消してから(逆の順)、断られた文と、その拒否が名指す直し方のボタンを出していました(戻すのが断られたら `<feature> was refused, and what was added for it could not be taken back (<理由>); Edit > Undo Render Edit takes it back.`)。✅UX-12d でこの取り消しの編集は外しました。断られた `edit_batch` は engine が何も書かないので、Studio は拒否の文と直し方のボタンだけを出します。
- **断られた後のボタン**: Apply が送った編集が断られたときも、拒否の code を見て同じボタンを出します([拒否から直し方を選ぶ所](../../src/devstudio/view/renderfeatureswidget.cpp#L3231))。
  - `feature.required_target_missing@1`: **Add with targets**(名指された target を、catalog の `required_targets` に足して扱います)。
  - `feature.insertion_anchor_missing@1`(FR-2 の無い engine では、文の `insert anchor was not found: <anchor>`): **Place <名前> in the Graph**。
  - `graph.target_ref_type_mismatch@1` に `expected` があれば: **Fix: make <target_ref> <形> <種類>( (<N> layers))**(`pelican.renderFeatures.fixTargetRef`)。`<形>` は topology が `two_d_array` なら `2D array`、`cube` なら `cube`、無ければ `2D` です(例: `Fix: make shadow_map_ref 2D array depth (3 layers)`。[ボタンの字 `targetRefTypeFixLabel()`](../../src/devstudio/model/featurerefusalfix.hpp#L99))。押すと `get_graph_instances` を読み、その target_ref の `parameters.type` を `expected.type` に替えた `replace_node` を 1 回の `edit_graph_document` で送ります(root を先に、無ければ definition の body を探す)。断られたのが Parameters の変更(`set_parameters`)だったなら、続けてそれをもう一度送ります(UX-12b の着地時の形。✅UX-12d から、その target_ref を読む port の型も同じ ops で替え、Parameters の変更と 1 本の `edit_batch` で送ります。下の節)。通れば `<target_ref> takes the type the engine expects.` です。
- **Place <名前> in the Graph**: Graph タブを前に出し、root に id が anchor の `render.fullscreen` を足して(あれば選ぶだけ)選び、その Pass の欄の New… を、anchor と同じ名前の雛形(あれば)を選んだ状態で開きます(✅UX-5b の New… と同じ道。[Graph に名前付きの pass を置く `GraphCanvasWidget::placeNamedFullscreenPass()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8846))。Graph が保存や読み直しの最中なら、編集できるようになってから置きます。Render Features の notice は `<anchor> is placed in the Graph: create its shader there, then add the feature again.` です。Create は人が押し、feature はもう一度 Add します。
- **UX-12b の着地時に止まっていた所(✅UX-12d で通ります)**: Blank から Graph で組んだ pipeline(`gui_settings_choose`)では、UX-12b の操作だけでは次の 2 つが最後まで通りませんでした。engine の ✅FR-3(`55390f4b`)と、それを使う Studio の ✅UX-12d(`6c7dc8bb`。下の節)で、どちらも committed になります。シナリオ `gui_settings_choose` は、この 2 つの拒否を待つ形から、最後まで通して画素を見る形に書き直しました。
  - `sky_ambient`: UX-12b の着地時は、Place Deferred lighting in the Graph → Create → Add → Add with targets で、target(`lit_color` / `scene_depth`)と target_ref は入るものの、feature の追加が `Pass input target is not produced as an earlier output: scene_depth` で断られ、パネルが足したものを戻していました。宣言しただけで誰も書かない `scene_depth` は、`sky_background` が読めません(この graph の material pass は depth を自分の image に書いています)。✅UX-12d から、Add with targets は `render_material_pass.depth` を `scene_depth_ref` に繋ぐ op を同じ `edit_batch` に入れるので通ります。
  - `cascade_count` を 3: UX-12b の着地時は、`graph.target_ref_type_mismatch@1` で断られて **Fix: make shadow_map_ref 2D array depth (3 layers)** が出るものの、Fix の `edit_graph_document` が `graph type mismatch at deferred_lighting.shadow_map from shadow_map_ref.resource` で断られていました。lighting の pass が 2D の depth の入力を宣言していて、engine は型を今の shadow map(cascade 1 の 2D)と照らすので、型の書き換えと `cascade_count` の変更は、どちらを先に送っても単独では通りません。✅UX-12d から、Fix は target_ref の型・lighting の pass の port の型・`cascade_count` の変更を 1 本の `edit_batch` で送るので通ります。
- test: `devstudio_rendering_settings_test` の [「UX-12b decision 1: a feature's required targets are offered before it is added, and Add with targets declares them, references them in the Graph and adds it, in order」](../../test/devstudio_rendering_settings_test.cpp#L1151)・[「UX-12b decision 2: a feature with no pass to go after is refused as such, nothing added for it stays, and Place offers the pass on the Graph tab」](../../test/devstudio_rendering_settings_test.cpp#L1256)・[「UX-12b decision 4 / UX-12d decision 3: a target_ref the engine expects another type of offers Fix, which writes that type and the reading ports' and sends the refused change again, in one edit_batch」](../../test/devstudio_rendering_settings_test.cpp#L1385)(decision 2 と 4 の test は ✅UX-12d で名前と中身を今の形に書き換えました)。GPU シナリオは `gui_settings_choose` の後半です(下の「Studio の自動化面」)。

**書き手を繋ぎ、1 本の edit_batch で足す —— Write … from …・Connect … to present・Fix の batch(✅UX-12d、`6c7dc8bb`)**: engine の ✅FR-3(`55390f4b`。上の「feature と snapshot を足す前に読む」の `needs_writer` と `feature.required_target_unwritten@1`、「複数の編集を 1 つの ticket で —— `edit_batch`」)を、UX-12b の Render Features の操作に入れました。Blank から Graph で組んだ pipeline で、`sky_ambient` の追加と `cascade_count` 3 が最後まで通ります(シナリオ `gui_settings_choose`)。

- **足す前に書き手を言う**: UX-12b の「target が足りない」の問いを出すとき、足りない target のうち catalog の `required_targets[].needs_writer` が `true` のもの(feature が読むだけで、自分では書かないもの)について、Graph の root の node から書き手の候補を探します([書き手の候補を探す `graphWriterCandidates()`](../../src/devstudio/model/featurerefusalfix.hpp#L151))。候補は、root の node の入力のうち `render.image` から来るもので、その node の `parameters.ports` が `access: "read"` と宣言していない(= その node が描き込む)もの、image の型が target と同じ種類(depth か color)のものです。候補があれば、問いの末尾が `…, and write scene_depth from render_material_pass.depth?` の形になります(足りない target が 1 つなら `write it from …`。[書き手を足した問いの文を作る `requiredTargetsQuestion()`](../../src/devstudio/model/featurerefusalfix.hpp#L157))。候補が 2 つ以上なら combo(`pelican.renderFeatures.writer`)で選び、問いの文も選んだものに替わります。例(シナリオ `gui_settings_choose`): `sky_ambient needs lit_color (color) and scene_depth (depth). Add them with the feature, and write scene_depth from render_material_pass.depth?`
- **1 本の edit_batch で足す**: **Add with targets** は、UX-12b の 3 つの編集を 1 本の `edit_batch` の edits にして送ります([Add with targets の batch を作る `sendAddBatch()`](../../src/devstudio/view/renderfeatureswidget.cpp#L3027))。edits は順に、`edit_render_targets`(足りない target の宣言。無ければ省く)、`edit_graph_document`(root の `render.target_ref` を足す op と、書き手があればその繋ぎ替えの op を 1 つの `ops` に)、`edit_render_features` の `add` です。
  - 繋ぎ替えの op は、target の target_ref(同じ edit で足すものを含む)が無ければまず足し、最後に書き手の port を target_ref の `resource` から読む `set_input` です([繋ぎ替えの op を作る `writerConnectOps()`](../../src/devstudio/model/featurerefusalfix.hpp#L167))。
  - 書き手の port の parameters が型を宣言していて、target_ref の型が `pelican.render.legacy_opaque_image@1`(✅FR-4 の前の engine が色の target に付けていた型)でその port の型と違うときだけ、間に target_ref の型を port の型にする `set_node_parameters` が入ります([型を書き換える条件](../../src/devstudio/model/featurerefusalfix.cpp#L455))。これは ✅UX-13d(`b289d706`)からです。UX-12d の時点では、型が port の型と違えば必ず書き換えていました。FR-4 の engine は色の target(commit の記録では `sky_ambient` と `bloom` の `lit_color`、`rt_shadow_mask` の `gbuffer_worldpos` / `gbuffer_normal`)を `color_signal` で返し、Add with targets はその型のまま target_ref を足すので、書き換えは起きず `set_input` だけになります。
  - 通れば `Added <feature> with the render targets it needs; <node>.<port> writes <target>.` です(書き手が無ければ UX-12b と同じ `Added <feature> with the render targets it needs.`)。
  - 断られた batch の拒否に `batch_index` があれば、notice は `<feature> was refused at <段>: <拒否の文>` です。`<段>` は `declaring the render targets` / `the Graph's target references`(書き手があれば `the Graph's target references and <target>'s writer`)/ `adding the feature` です([batch の拒否に段の名前を足す `chainRefused()`](../../src/devstudio/view/renderfeatureswidget.cpp#L3150))。続けて、その拒否が名指す直し方のボタンを UX-12b と同じ規則で出します。
- **断られた後の Write … from …**: `feature.required_target_unwritten@1` で断られると(Apply が送った add でも)、notice は `<feature> reads <target>, which no pass writes before it. Write it from <node>.<port>.` です(候補が 2 つ以上なら `Write it from one of the passes below.`。[拒否の文 `requiredTargetUnwrittenLine()`](../../src/devstudio/model/featurerefusalfix.hpp#L143))。`writer_candidates` のうち `node` のあるもの(root の graph の node が作った pass)だけを **Write <target> from <node>.<port>**(`pelican.renderFeatures.writeFrom`。2 つ以上なら前に combo `pelican.renderFeatures.writer`)にします。rendering 節の pass は graph の編集で繋ぎ替えられないので出しません。押すと、繋ぎ替えの `edit_graph_document` と feature の `add` を、上と同じ形の 1 本の `edit_batch` で送ります([拒否から書き手のボタンを出す所](../../src/devstudio/view/renderfeatureswidget.cpp#L3236))。
- **Graph の notice**: Graph タブで同じ拒否を受けたときも、notice は frame planner の文ではなくこの code の 1 文です([Graph の notice で FR-3 の code の文を使う所](../../src/devstudio/view/graphcanvaswidget.cpp#L6132))。
- **見えない target を画面に出す —— Connect <target> to <present>**: feature を足した後(Add with targets が通った後と、Apply の add が再起動せずに通った後)、その feature の catalog の項目に `required_targets` があれば、authoring context と Graph を読み直し、feature の pass が color に書く target のうち、同じ graph のどの pass も読まないものを探します([見せる手順を作る `showTargetPlan()`](../../src/devstudio/model/featurerefusalfix.hpp#L191))。あれば notice の末尾に `<feature> draws into <target>, which nothing reads. Connect it to <present> to see it.` が付き、**Connect <target> to <present>**(`pelican.renderFeatures.showTarget`)が出ます。`<present>` は、同じ graph で display か swapchain に書き、root か fork した preset で入力を編集できる pass です。
  - 押すと 2 つの編集を作ります。1 つ目は、feature の pass の挿入先(`insertion` の anchor)が root の graph の node で、target の target_ref があれば、その node の color の出力を target_ref に繋ぐ `edit_graph_document` の ops(上の繋ぎ替えと同じ op。型を書き換えるのは legacy の型のときだけ)。2 つ目は、present の最初の入力を target に替える `edit_preset_pass` です。
  - ✅UX-13d(`b289d706`。engine の ✅FR-4 で `edit_preset_pass` が `edit_batch` に入った後)からは、graph の op があれば `[edit_graph_document, edit_preset_pass]` の順の 1 本の `edit_batch`(ticket 1 つ)で送り、graph の op が無ければ `edit_preset_pass` だけを送ります([UX-13d で present の編集を送る `sendShow()`](../../src/devstudio/view/renderfeatureswidget.cpp#L3435))。batch の `base_source_digest` は手順を作るときに読んだ root の digest で、`edit_preset_pass` の preset(か feature)の digest も batch の前に読んだ値のままです(コードの注記。engine の規則は上の「project の preset と feature を書き換える編集(✅FR-4)」)。Edit > Undo Render Edit の 1 回で両方が戻り、Redo の 1 回で両方が戻ります(シナリオが文書と frame plan で見ます)。
  - UX-12d の時点では `edit_preset_pass` は `edit_batch` が受けない method だったので、先に `edit_graph_document` を送り、その committed の後に `edit_preset_pass` を送る 2 つの ticket で、Undo も 2 回でした。
  - 通れば `<present> shows <target>.` です。1 本の ticket なので、この文は他の panel の読み直しの `Refreshing…` に上書きされずに残ります(シナリオがこの文を待ちます。UX-12d の時点では上書きされることがあり、シナリオは frame plan を問うていました)。
  - 断られると、拒否に `batch_index` があれば `Connecting <target> to <present> was refused at <段>: <拒否の文>` です。`<段>` は `the pass before it drawing into <target>` / `<present> reading it` です。`batch_index` が無ければ(`edit_preset_pass` だけを送ったときなど)拒否の文だけです([断られたときの段の名前を足す所](../../src/devstudio/view/renderfeatureswidget.cpp#L3173))。断られた batch は何も書かないので、取り消しの案内はありません。UX-12d の時点では、2 つ目が断られても 1 つ目は残っていて、`<target> is drawn now, but <present> could not read it (<理由>); Edit > Undo Render Edit takes it back.` でした。
  - 例(シナリオ `gui_settings_choose`): `sky_ambient` を足した後、**Connect lit_color to present** で `deferred_lighting` が `lit_color` に描き、`present` がそれを読みます。✅UX-13d から、押す前の root 文書を覚えておき、Undo 1 回でそれと同じに戻ること、Redo 1 回で `present` がまた `lit_color` を読み `lit_color_ref` への繋ぎが戻ることを見ます。viewport の左上の隅(何も描かれず黒かった所)の画素が変わり、`sky_ambient` の Parameters で `color_r` に 4 を打つとまた変わります。
- **Fix の batch**: `graph.target_ref_type_mismatch@1` の **Fix: make …** は、target_ref の型の書き換え(`replace_node`)に、同じ scope でその target_ref を入力に持ち、`parameters.ports` のその port が `type` を宣言している node の port の型の書き換え(`set_node_parameters`)を、同じ `ops` に足します([読む port の型も替える所](../../src/devstudio/model/featurerefusalfix.cpp#L246))。断られたのが Parameters の変更なら、それを先にして `[edit_render_features の set_parameters, edit_graph_document]` の 1 本の `edit_batch` で送ります([Fix の batch を送る所](../../src/devstudio/view/renderfeatureswidget.cpp#L3338))。断られると `Fixing <target_ref> was refused at <段>: …` です(`<段>` は `<feature>'s change` / `the type in the Graph`)。例(シナリオ `gui_settings_choose`): `cascade_count` 3 → **Fix: make shadow_map_ref 2D array depth (3 layers)** で、`cascade_count` 3、`shadow_map_ref` の型、それを読む 2 つの lighting の pass の `shadow_map` の port の型が 1 つの ticket で committed になり、箱の右の床は影のままです。
- **Undo**: `edit_batch` は render の編集の記録に入り、Edit > Undo Render Edit の 1 回で batch ごと戻ります。Undo の項目の名前は、batch の edits のうち記録する method の最後のものの名前です([edit_batch の Undo の名前を付ける所](../../src/devstudio/model/renderedithistory.cpp#L330))。
- **FR-4 の残る穴との関係**(✅UX-13d): Studio が `edit_batch` を送るのは Add with targets・Fix・Connect … to present の 3 か所で、どの edits にも `edit_region_replacement` / `decompose_material_pass` / `apply_graph_template` は入りません(コードの読み。上の FR-4 の「残る穴」には当たりません)。
- 仕様は、UX-12b の取り消しの後に空の fragment(`render-targets.json`)が root に残ることも、これで出なくなるのを確かめるよう求めていました。確かめた test や記録は見当たりません(未確認)。
- test: `devstudio_rendering_settings_test` の [「UX-12d decision 1: a target no pass would draw is asked about with the output that draws it, connected in the same edit; a refusal that names writers offers Write from」](../../test/devstudio_rendering_settings_test.cpp#L1532)・[「UX-12d decision 2: a refused edit_batch names the edit it met (batch_index), and nothing is taken back」](../../test/devstudio_rendering_settings_test.cpp#L1644)・[「UX-12d decision 4 / UX-13d decision 1: a target the feature draws that nothing reads is offered to present, which reads it, and the pass the feature goes after draws into it first, as one edit_batch」](../../test/devstudio_rendering_settings_test.cpp#L1674)(UX-13d で名前に `/ UX-13d decision 1` と `as one edit_batch` が付きました)・[「UX-13d decisions 2 and 3: a colour target the catalog types as a colour signal keeps its type when a pass is connected to it; only a legacy image takes the port's」](../../test/devstudio_rendering_settings_test.cpp#L1786)。decision 3 は上の UX-12b の decision 4 の test です。GPU シナリオは `gui_settings_choose` の後半です(下の「Studio の自動化面」)。

Render Features → **Fork preset...**でコピー元を確認し、`project://pipelines/custom.json`など
未作成の保存先を指定します。元presetの文書をコピーするので、既存featureの二重適用はしません。
project側の設定・feature・RT上書き指定は維持します。元presetかrootが外部変更された場合は、
Refresh sourceで再取得してください。直接記述のrendering configには対象presetがありません。

Render Features → **Region replacements...**でproject presetのタグ付き領域を選び、
差し替え先の登録済みprovider名を指定できます。欄を空にして **Apply**すると差し替えを解除し、
元のノード構成とその編集内容を使います。通常のコンパイラ最適化は維持します。
✅UX-3 から、engine が `region_providers`(上の EL-1 の段落)を返す間は、provider は打つ欄ではなく combo(**Replacement provider**)です。先頭の `(authored nodes)` が差し替えの解除で、組込の provider は `<name> (built in)` と出ます([combo を埋める所](../../src/devstudio/view/renderfeatureswidget.cpp#L1413))。一覧が空(EL-1 の前の engine)なら今までの打つ欄です。
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
rasterではvertex/fragment shader参照、drawとraster stateも編集できます。
drawは例えば`{"vertex_count":6,"instance_count":2}`で頂点数と個体数を指定します。raster stateの欄を全部Defaultにすると明示設定を削除し、既定状態へ戻します。編集しない項目は保持します。
✅UX-4a から、draw・raster state・color attachment objectの欄はJSONではなく選ぶフォームです(下の「生の JSON の欄はフォームになりました(✅UX-4a)」)。UX-4a の前は **Draw settings (JSON)**、**Raster state (JSON, optional)**、color attachment objectのJSON欄でした。depth対象の変更は元のload/store等を保持します。
解決できない入力やTLAS名、描画契約やshaderと矛盾する変更はエンジンが拒否します。
名前・配置・その他の描画設定は保持します。保存は既存の編集ticketを使い、失敗時はpresetとruntimeを戻します。
元preset由来の編集可能なpassはauthoring contextに`editable_preset_pass: true`と
合成前の`preset_declaration`を持ちます。engine共有preset、feature由来pass、生成されたAS/anchor nodeは対象外です。

**direct root の pass(✅F-6)。**preset を選ばない root(Blank pipeline の project など、`config_kind` が `direct`)が
自分で宣言した pass は、authoring context で `editable_root_pass: true` と合成前の `root_declaration` を持ちます
([`editable_root_pass` を付ける所](../../src/core/communication/renderconfigeditor.cpp#L2502))。engine が足した pass(`output_transform` など)と
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
作った task、preset root の preset の task、それ以外([origin を決める所](../../src/core/communication/renderconfigeditor.cpp#L2839))。
`"root"` は本番では出ません。rendering 文書の root は task を宣言できず(MG-2b)、node の task は `graph_node` として先に拾われるので、
本番の resolver が root に帰す task は残りません。`"root"` は fake の resolver のための受け皿です。

`origin` が `"graph_node"` の task にだけ `graph_node: {scope, node, instance}` が付きます([graph_node を組む所](../../src/core/communication/renderconfigeditor.cpp#L2852))。

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
([判定](../../src/devstudio/view/frameplanedit.cpp#L1078))、**何も送らずに**次の文で断ります(着地前は engine の英語の理由がそのまま出ていました):
`<task>: rendering root holds no tasks; use a feature or a render.compute node. The engine refuses an edit of it (compute_task_not_editable).`
([断りの文](../../src/devstudio/view/frameplanedit.cpp#L1085))。catalog の行が古く「編集できる」と言っていても、この判定が先です。

- **Frame Plan**: その task への接続・切断、dispatch、Delete がこの文で止まり、dispatch の帯も出ません。status 行は 90 文字で最初の
  1 文までなので、**status 行には前半の 1 文だけ**が出て、code を含む全文は status 行の tooltip にあります
  ([1 行にする規則 `oneLine()`](../../src/devstudio/view/panelhelp.hpp#L128))。rendering 文書の reference は authoring context の
  `source_reference` から取ります([root の reference を入れる所](../../src/devstudio/view/frameplanwidget.cpp#L2078))。
- **Task Editor**(Render Features の Tasks... / Frame Plan の Edit task...): root の task を選ぶと、下の notice に全文(code まで)が出て、
  宣言欄は読み取り専用、**Apply task**・**Delete task**・接続表・dispatch フォームは押せません([Task Editor の錠](../../src/devstudio/view/renderfeatureswidget.cpp#L479))。
  送る中身(add / edit / 断り)は [`computeTaskEditorRequest()`](../../src/devstudio/view/frameplanedit.cpp#L1158) が決めます。
- **新しい task は今までどおり**です。New compute / New ray tracing は `add_compute_task` で、指定した `project://` の feature に作ります。✅CT-6s から、New ray tracing の草稿は Graph の New… と同じ RT の雛形を project に書いて、それを名指します(下の「New compute / New ray tracing」の段落)。
- **分かっている穴**(着地時の記録): `origin` を持たない古い応答では、Task Editor が root の reference を知るのは
  Render Features が `get_render_features` の応答を読んだ後で、それより前は engine の「編集できない」と英語の理由のままです。
  (Q-10 の時点では `render.compute` node から来た task も同じ root の文になっていましたが、Q-12 で node の文に分かれました。)

**graph node が作った task は「Graph タブで編集」と言い、その node へ移れます(✅Q-12)。**Studio は task の錠を **origin を先に読んで**
決めます([錠の順](../../src/devstudio/view/frameplanedit.cpp#L1108)): `origin` が `"graph_node"` → root(`origin` が `"root"`、または
`origin` が無く上の判定に当たる)→ 読み取り専用(`editable: false`)→ 編集できる。Frame Plan の drag・帯・Delete も Task Editor の
Apply も、この 1 つの関数を通ります。graph node の task には、root の文ではなく engine の `read_only_reason` がそのまま出ます:
`This task is made by a render.compute node in the graph. Edit the node in the Graph tab.`

- **Frame Plan**: その task への接続・切断、dispatch、Delete はこの文で止まり、dispatch の帯も出ません。**status 行にはこの 2 文が丸ごと**
  出ます(`oneLine()` は最初の 1 文で切るので、断りに status 行用の 1 行を別に持たせています。[status 行用の 1 行](../../src/devstudio/view/frameplanedit.hpp#L32))。
  tooltip は `<task>: <上の文> Right-click the task and choose Open in Graph.` です。その task の上で右クリックすると、メニューの先頭に
  **Open in Graph** が出ます([右クリックに足す所](../../src/devstudio/view/frameplangraphics.cpp#L4164))。Q-12 の着地時は graph node の task の上でだけでした。✅GL-1 から、anchor 以外のすべての node の上に出ます。graph の node が作っていない pass では、Graph タブの畳んだ node を選びます(下の「graph の外から来た pass を畳んだ node で見る」)。✅GL-2b から、Passes / Resources / Physical plan / Decisions / Backends / Barriers のタブの行の右クリックにも出ます(下の「Frame Plan の表の行から開く」)。
- **Task Editor**: graph node の task を選ぶと、notice に上の文が出て、その横に **Open in Graph** ボタンが出ます(既存の task で
  `graph_node` を持つ行を選んだときだけ見えます)。宣言欄と Apply task などは root の task と同じく止まります。
- ✅GL-5c から、Frame Plan で graph node の task を選んで **Edit task** を押すか行をダブルクリックすると、Task Editor ではなく Open in Graph と同じ道で Graph タブのその node が選ばれます([Edit task の分岐](../../src/devstudio/view/frameplanwidget.cpp#L761))。graph node の無い task は今までどおり Task Editor です(下の「graph root では Frame Plan の Add を Graph の palette に(✅GL-5c)」)。
- **Open in Graph** は Graph タブを前に出し、`graph_node` の scope を開いて node を選び、view の中央に置きます([node を見せる所](../../src/devstudio/view/graphcanvaswidget.cpp#L8906))。
  `instance` があれば root から 1 段ずつその instance へ降ります(instance をダブルクリックして降りるのと同じ)ので、パンくずと
  `Values from:` がその instance に合います。2 段下(`wrap/inner` など)では、パンくずが 2 つの definition を通り、`Values from:` は
  root の instance(`wrap`)です。path が scope に届かないときは scope を直接開き、path の最初の instance が `Values from:` の一覧に
  あればそれを選びます。
- Graph タブがまだその node を持っていない(文書を読んでいない、または古い)ときは、tab を切り替えず何も変えずに
  `The Graph tab has no node <node> in <scope> yet: refresh it.`(root scope は `the root`)を、Frame Plan では status 行、Task Editor では
  notice に出します([tab を切り替える所](../../src/devstudio/view/graphcanvaswidget.cpp#L9060))。
- **分かっている穴**(着地時の記録): tab の切り替えの test は同じ関数を `QTabWidget` で包んだもので、MainWindow 全体を RPC 越しに
  動かしてはいません。Task Editor のボタンには widget test がありません。`root` という名前の definition は root scope と見分けられません
  (engine の scope の書き方のままなので)。

Frame Planのtask詳細にある **Callable record settings** は、取得時点のCPU側実行時設定です。
同じ情報はRaw JSONの`runtime_callable_records`にも含まれます。
record indexごとにフィールド名・型・offset・値を示し、GameContextからの動的変更も含みます。
`source: runtime_cpu_settings`は次のdispatchで使う設定を意味し、GPU完了時の値のreadbackではありません。
保存する初期値は引き続きCallable Records画面で編集します。Frame Planを再取得すると観測値を更新できます。
**New compute / New ray tracing**では名前・既存shader参照・新しい`project://`保存先を設定してtaskを作成します。
保存先featureとrootへの参照追加は一緒に確定します。New compute は shader ファイルも resource も作りません(草稿は今も、まだ無い `project://shaders/compute` を名指します)。
✅CT-6s(`827eddd7`)から、**New ray tracing** の草稿は、project にまだ RT の task が無ければ、Graph の New… と同じ雛形(`shaders/<名前>.rgen` / `.rmiss` / `.rchit`。下の §13.5「RT を Graph で組む(✅CT-6s)」)を空いている名前で書き、その参照を `raygen` / `miss` / `closesthit` に入れます([草稿が雛形を書く所 `rayTracingTemplateReference()`](../../src/devstudio/view/renderfeatureswidget.cpp#L390))。
CT-6s の前の草稿は、存在しない `project://shaders/raygen` / `miss` / `hit` を名指していました。

- 草稿の残り: `resource_ports.color` が `<名前>_color`(`access: storage`)、`writes` がその 1 つ、`dispatch` が `{"rays_from": {"port": "color"}}` です([草稿を作る `initializeNewDraft()`](../../src/devstudio/view/renderfeatureswidget.cpp#L410))。`<名前>_color` は project に宣言がありません。Connections で付け替えるか target を宣言します(notice の文 `Wrote <参照> (.rgen, .rmiss and .rchit) for this task. Point its port at a storage image in Connections, set the destination, and apply.`)。
- 書くのは 1 つのダイアログにつき 1 回です。書けなければ notice が `Could not write the ray tracing shaders: <理由>` です(project が開いていなければ `No project is open to write the shaders into.`)。
- 既に RT の task を持つ project では、今までどおりその task を写した草稿で、何も書きません。
- コードの読みでは、書くのは草稿を出した時(**New ray tracing** を押すか、task の一覧で `(New ray tracing task)` を選んだ時)で、Apply の前です。Apply しなくても 3 つのファイルは残ります(test はこの点を見ていません)。
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
**Graph タブと同じ表・同じ解決順**です([種類の判定](../../src/devstudio/view/frameplangraphics.cpp#L2260))。
辺は**ソケット中心からソケット中心**へ引かれ、対応するソケットを持たない辺(折り畳んだグループ、境界の stub、
resource を名指さない順序依存)だけがノード矩形へ落ちます([配線](../../src/devstudio/view/frameplangraphics.cpp#L291))。
辺のラベルは消えて tooltip になり、ノードを選ぶと触れていない辺が薄くなります
([不透明度](../../src/devstudio/view/frameplangraphics.cpp#L4965))。
**操作・RPC・model は変わっていません** —— ソケットはノード item の子なので、**Shift＋左ドラッグ**はソケットから
始めてもノードの本体から始めても同じ接続になります([起点の解決](../../src/devstudio/view/frameplangraphics.cpp#L4327))。
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
  ([その throw](../../src/core/communication/renderconfigeditor.cpp#L1848))。`set_parameters` は既定が
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
書き込み側は同じ規則を [`isGraphRootKey()`](../../src/project/renderconfigdocument.cpp#L497) として
鍵の側から持っています。N-6 からは editor 用の `layout` も graph 側の root キーです。[第6章](06_rendering.md) §6.16)。したがって初期化は `rendering.features` に
`[]` を書き、**root 直下に `features` を生やしません**
([`initialize()`](../../src/project/renderconfigdocument.cpp#L664))。
`rendering` オブジェクトそのものが無い graph root は、書き込む場所が無いので
**名前付きで断られます** —— 受付の応答は `render_config_section_unlocated`
(**編集 op の経路だけ `{method}` が付きます** —— [renderconfigeditor.cpp:1325](../../src/core/communication/renderconfigeditor.cpp#L1935)。
pipeline の preflight([:887](../../src/core/communication/renderconfigeditor.cpp#L1458))と fork([:1753](../../src/core/communication/renderconfigeditor.cpp#L2806))は
同じ code を data 無しで返します)、
engine 内部の例外名は
[`RenderConfigSectionUnlocated`](../../src/project/renderconfigdocument.cpp#L485) です。
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

- 編集: `edit_graph_document{base_source_digest, ops[], material_route_default?}` と `edit_graph_instances{base_source_digest, attach[], detach[], material_route_default?}`(`material_route_default` は ✅RS-2)。
  後者は owner 付き root instance の装着・取り外し専用で、**中では同じ `editGraphDocument()` の ops に翻訳されます**
  ([翻訳](../../src/project/graphcomposition.cpp#L2526))。検証を 2 度書かないための構造です。
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
| `set_outputs` | `outputs`(`{"<出力名>": {node, port}, …}`) | ✅DO-1。その scope の公開の出力(root なら文書の `outputs`、definition ならその `outputs`)を丸ごと置換([適用](../../src/project/graphcomposition.cpp#L2426))。逆 op は前の `outputs` を持つ同じ op。endpoint は `{node, port}` ちょうど 2 鍵(`element` は取らない)。node はその op の時点で scope に在ること(無ければ `graph.node_missing@1`。node を足して出力にするなら `add_node` → `set_outputs`、出力から外して消すなら `set_outputs` → `remove_node` の順)。`graph.input` を指すなら port は `value`、instance / ループ以外の node なら宣言した出力 port。出力名は空でなく `/` を含まず、`[]` で終われない(ループの `p[]` の綴り)。形の誤りは `invalid_graph_document`。definition から消えた出力を外(instance の port、ループの `outputs` の `p` / `p[]` や `carry` の `from`、それを読む edge や参照側の scope の `outputs`)がまだ読んでいれば、**列の最後に** `graph.output_referenced@1` `{definition, output, node}` + `{consumer, port}` / `{scope_output}` / `{field: "outputs" / "carry"}`([検査](../../src/project/graphcomposition.cpp#L2071))。ループの分岐の body の出力がそろわなければ、最後の展開で `graph.loop_branch_interface@1`。Studio は ✅LP-4d2 から、ゾーンの右の縁の `+` でこの op を送ります(下の「縁をまたぐ線・carry・展開の表示」) |
| `add_definition` | `name`、任意の `definition`(`{nodes, outputs}`) | ✅LP-0c。definition を足す(`definition` を省けば空の body)。同じ名前があれば `graph.definition_duplicate@1` `{definition}`。`scope` は取らない |
| `remove_definition` | `name` | ✅LP-0c。definition を消す。無ければ `graph.definition_missing@1` `{definition}`、列の最後でまだ名指されていれば `graph.definition_referenced@1` `{definition, referenced_by}`。逆 op の本体は `get_graph_instances` の `definitions` から事前に読む(`base_source_digest` の CAS で同じ body だと保証される) |
| `rename_definition` | `from`, `to` | ✅LP-0c。改名し、名指している `graph.instance` の `definition` と `graph.loop` の `branches[].body` を同じ op で書き換える。`from` が無ければ `graph.definition_missing@1`(`definition` は `from`)、`to` があれば `graph.definition_duplicate@1` |

**全 op が任意の `scope` を取ります**([解決](../../src/project/graphcomposition.cpp#L1524))。既定は `"root"`(root 文書の `nodes`)、
definition 名を書くとその `definitions[<名前>].nodes` が対象です。**`root` は root scope の予約語**なので、
`root` という名前の definition は指せません。op は**順に**候補文書へ当たり、**最後に 1 回だけ展開**されます
([適用ループ](../../src/project/graphcomposition.cpp#L2505)、[最後の 1 回の展開](../../src/project/graphcomposition.cpp#L2522))。つまり `ops` は**全部通るか、文書ごと拒否か**のどちらかで、
途中まで書かれた文書は残りません。

✅LP-0c から、instance / ループの供給する port と definition の境界の一致、消した `graph.input` と definition がまだ名指されていないかの 3 つは、op ごとではなく**列の最後に 1 回**検査します(下の「ループと分解の RPC」)。

#### 名前付きの拒否

| code | いつ |
|---|---|
| `graph.node_duplicate@1` | 同じ scope に同じ id を追加した |
| `graph.node_missing@1` | 無い node を指した(正は throw 箇所 [graphcomposition.cpp:373](../../src/project/graphcomposition.cpp#L1516)。[宣言コメントの名前付きの失敗の一覧](../../src/project/graphcomposition.hpp#L213)も `40099e3f` 以降は 8 つ全部を挙げていて、この表と一致します) |
| `graph.input_unknown@1` | instance の definition が宣言していない port を `set_input` / `set_node_parameters` した |
| `graph.output_referenced@1` | 他ノードの入力、またはその scope の `outputs` が指しているノードを削除した。✅DO-1 から、`set_outputs` で消した definition の出力を外がまだ読んでいるときも(上の表の `set_outputs`) |
| `graph.input_referenced@1` | **instance がまだ値を供給している `graph.input`** を definition の中から削除した |
| `graph.nested_instance@1` | definition の中に `graph.instance` を足した(instance は root に立つもの) |
| `graph.input_scope@1` | root に `graph.input` を足した(境界 port は definition の中のもの) |
| `graph.definition_immutable@1` | id に `/` を含むノード(instance path 越しの指定)を **どの op でも**指した(`{reason: "instance_path"}`)。root scope の op が definition の中にいるノードを名指したときも同じ code で、`{definition}` が「どの scope を開けばよいか」を返します([`locateNode()`](../../src/project/graphcomposition.cpp#L1502)) |
| `graph.definition_duplicate@1` / `graph.definition_missing@1` / `graph.definition_referenced@1` | ✅LP-0c。definition の op(上の表) |
| `graph.loop_*@1` ほか LP 系の code | ✅LP-1a 以降。ループと、グラフで書く pass の形の拒否。一覧は [第6章 §6.18](06_rendering.md) の「名前付きの拒否の一覧」 |
| `graph.render_node_parameters_missing@1` | ✅RS-9(`5d065436`)。render の node の `parameters` に op が要るものが無い。`{node, op, missing}`(`missing` は欠けた名前)。`parameters` が無い・`null` の node は空として扱う。op ごとの必須は [第6章 §6.18](06_rendering.md) の「名前付きの拒否の一覧」の下。✅RS-15 から `render.scalar` / `value.*` / `audio.*` / `ui.*` の op と `render.attribute` の(`count` か `source`)にも広がり、名前は同名の入力 edge でも満たせる。✅CT-1 から `render.buffer` の `element`、`render.image` の `type.semantic` などの欠けもこの code(`missing` にその名前)。同じ表を `get_graph_catalog` の `required_parameters` が出す |
| `graph.port_field_missing@1` | ✅CT-1(`ac8d836b`)。render の pass の `parameters.ports.<port>` に要るものが無い。`{node, port, field}`(`field` は `access` / `element` / `type` / `type.semantic` / `type.arguments` / `location`)。編集では rejected で engine は動き続ける。詳しくは [第6章 §6.18](06_rendering.md) の「名前付きの拒否の一覧」の下 |
| `graph.render_node_parameter_invalid@1` | ✅CT-3(`0d215a68`)。node の parameter の値が、`get_graph_catalog` の `parameters` の kind・enum・範囲に合わない。`{node, op, parameter, expected, reason: type｜enum｜range}`。`enum` では `allowed`、`range` では `min` / `max` が付く。`parameter` は入れ子なら道筋(`keys[0].interpolation`)。`null`、`$expr` / `$format` の式、表に無い鍵は断らない。必須の欠けは先に `graph.render_node_parameters_missing@1`。詳しくは [第6章 §6.18](06_rendering.md) の「名前付きの拒否の一覧」の下の CT-3 の項 |
| `graph.op_requires_owned_instance@1` | ✅CT-4(`4d682338`)。catalog の `placement` が `owned_instance` の op(`pose.*` の 6 つと `value.morph_weight`)を、`owner` 付きの `graph.instance` の外(root や owner の無い instance の中)に置いた。`{node, op}`(`node` は展開名)。検査は graph の partition で、編集でも読み込みでも同じ所を通る。owner を付ければ通る。詳しくは [第6章 §6.17](06_rendering.md) の CT-4 の段落 |
| `graph.render_scalar_layout_invalid@1` / `graph.render_scalar_type_mismatch@1` | ✅RS-15(`7b16e9c8`)。同じ buffer に書く `render.scalar` が byte 0 から隙間なく並ばない `{target, node, offset, expected_offset, reason: overlap｜gap}` / 整数の要素の buffer に float の値を書く `{node, target, element, components}`([第6章](06_rendering.md) §6.5) |
| `graph.render_scalar_target_incompatible@1` | ✅CT-5(`65f3a8a3`)。`render.scalar` の書き込み先の buffer が `host_source: "runtime_f32"` でない、または `size` が書く sink の合計と違う。`{node?, target, expected: {host_source, size?}, actual: {host_source, size?}}`。graph の中の buffer は graph の partition(編集でも読み込みでも)、graph の外で宣言された buffer は世代を組む登録の段で断り、後者には `node` が付かない。`value` の線で繋いだ buffer は partition が `size: 4` と `runtime_f32` を補う。前は名前の無い `incompatible frame graph output buffer: <名前>`([第6章](06_rendering.md) §6.5 の CT-5 の項) |
| `graph.image_initial_requires_persistent@1` / `graph.image_initial_extent_conflict@1` / `graph.image_initial_mismatch@1` | ✅IA-1(`c6690eba`)。`render.image` の `initial: {asset}` を transient の image に書いた `{node}` / `width`・`height` を省いて `extent_scale` を書いた `{node}` / asset が image に合わない `{node, asset, what: extent｜format｜color_space, expected, actual}`。[第6章 §6.18](06_rendering.md)「persistent の image を画像資産から始める」 |
| `shader.loop_push_overlap@1` | ✅LC-1(`158abae9`)。shader の push constant の range が `pelican_loop.glsl` の `[112, 120)` に一部だけ重なる。`{shader, stage, range: {offset, size}}`。組み直しで pipeline を作る時に断られ、編集の結果にはこの code のまま fields が並ぶ([第6章 §6.18](06_rendering.md)「回の番号を shader に渡す」) |

**拒否の `document` と `definition`(✅CT-2、`51d1c2d8`)。**`graph.` で始まる code の拒否には、上の表の field に加えて
`document`(rendering の root 文書の `project://<path>`。例 `project://passes/main.json`)と、definition の body の node なら
`definition`(その definition の名前)が付きます。graph の node を持つのは root 文書だけなので、`document` はいつも root 文書です。
ticket の rejected の `error` では field と message の両方に入ります。例(test。`scope: "gain_curve"` の `add_node` で buffer の port に
`element` が無い): `{code: "graph.port_field_missing@1", node: "left/sim", port: "data", field: "element", document: "project://passes/main.json", definition: "gain_curve", ...}`
(`node` は展開名)。拒否がもう同じ名前の field を持つとき(`graph.definition_missing@1` の `definition` など)はその値を残し、
分からない field は出しません。`edit_graph_instances` の `invalid_graph_attachment` には付きません(✅CT-5 から、`edit_graph_instances` は graph の名前付きの拒否を
`invalid_graph_attachment` に畳まず、その code と field に `document` を付けて返します。`invalid_graph_attachment` に残るのは名前の無い拒否だけです)。規則の全部は
[第6章 §6.18](06_rendering.md) の「名前付きの拒否の一覧」の下の CT-2 の項です([CT-2 で ticket の rejected に足す所](../../src/core/communication/renderconfigeditor.cpp#L1116))。

`graph.input_referenced@1` が要るのは、**その値が編集中の body の外にある**からです —— definition を開いている
キャンバスからは、その port に値を入れている instance が見えません([走査](../../src/project/graphcomposition.cpp#L1636))。
op の形が壊れているだけのもの(`ops` が配列でない、`source` の鍵が違う、未知の `op` 名)は名前付きにならず、
`invalid_graph_document` として理由の文字列で返ります。

#### シーン編集との違い(こちら側です)

- **`actor_id` もセッションも要りません。**
- **scene journal とは別の履歴です。**Graph タブは成功した直近100操作を保持し、
  Undo / Redo ボタンとキャンバスの Ctrl+Z / Ctrl+Y（または Ctrl+Shift+Z）で逆編集・再編集を送ります。
  保存済み文書の digest が別の編集で変わった場合は履歴を破棄します。✅OT-1 から、engine の再起動(Studio から見れば再接続)では破棄しません。
  再接続の後に読んだ文書の digest が履歴を作った文書と同じなら Undo / Redo はそのまま効き、違えば破棄します([`GraphCanvasModel::clearContext()`](../../src/devstudio/model/graphcanvasmodel.cpp#L665))。
  別の project を開くと、digest が同じでも破棄します([`GraphCanvasModel::forgetHistory()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2760))。送信中だった編集は engine と一緒に捨てます。
  接続が切れている間は Undo / Redo を押せません。OT-1 の前は再接続のたびに破棄していたので、graph の編集が求めた再起動(下の「engine の再起動が要る graph の編集」)の後も Undo が消えていました。
  シーン・Features・Pass Editorを横断する統一Undoではありません。RPCからも同じ編集opを使えます。
- **CAS は `SceneRevision` ではなく `base_source_digest`** —— 文書全体の sha256 です。受付でずれていれば
  `external_modification`、受付のあとフレーム境界までの間に別のチケットが先に書けば `stale_source_digest`。
  どちらも**何も書きません**。
- **1 操作 = 1 op 列 = 1 チケット**。同じフレームに 2 本目を投げても、後から来た方が CAS で落ちます。
  結果は `get_edit_result`(ヘッドレスなら `step_frame` の `edit_results[]`)で受け取ります。
- 書き戻しは root の `nodes` が常に、`definitions` は**変化したときだけ**です([definitions の書き戻し](../../src/core/communication/renderconfigeditor.cpp#L3012))。✅DO-1 から、root の `outputs` も変化したときだけ書き戻します([root の outputs の書き戻し](../../src/core/communication/renderconfigeditor.cpp#L3016))。
  root だけを触った編集は、definition のバイト列を 1 文字も動かしません。

⚠ **`add_node` / `remove_node` で owner 付きの `graph.instance` を直接足す・消すことはできます。**
そのとき `edit_graph_instances` が持っている 2 つのガード(owner を持つ root instance であることの確認)を
迂回します。名前付きの拒否は増やさず記録する、という裁定です(Studio はこの経路を使いません)。

#### 「曲線で制御」が作るノードの集合

Inspector の導線(次節)は専用の RPC を持たず、**普通の `ops` を 1 回送るだけ**です。作られるのは次の 3 ノードと
1 本の接続で、正は [`curveControlOps()`](../../src/devstudio/model/graphattachmentmodel.cpp#L409) です。

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
| node | `origin` | ✅GL-2a。graph の node が作った pass・compute task(root の node・instance の中の node・ループの回・`pass_replacements` の置き換え先)に、作った node の `{graph, node, definition?, instance_path?, loop?, loop_branch?, element?}`。preset・feature・rendering 節の pass には付かない(preset が `engine://` だと graph の pass も preset の pass も `source: "engine"` なので、これで見分ける)。`graph` は rendering root 文書の参照(`get_compute_task_authoring` が node の task に返す `source_reference` と同じ値)、`node` はその scope に書かれた id、`definition` は node を持つ definition(root の node には無い)、`instance_path` は置いた instance(末尾の `/` は無い。root とループの body には無い)、`loop` / `loop_branch` / `element` はループの回だけ(`element` は `loop_origin.element` と同じ)([組む所](../../src/core/renderingpass/frameplanner.cpp#L1646)) |
| node | `replaces` | ✅GL-2a。feature の pass・task の代わりに入った node に `[{feature, pass}]`(instance が名指した順。1 つの node が複数を置き換えうるので配列)。ループでは、置き換えた pass と同じ pass family の回にだけ付く |
| node | `added_after` | ✅RS-6。engine がその pass の `after` に足した graph の compute task(✅RS-6b から feature と integration の compute task も)の名前を、足した順に(その pass が task の書く image を読むため。[第6章](06_rendering.md) §6.18「graph の pass の置き場所」)。足していない node には鍵が無い([出す所](../../src/core/renderingpass/frameplanner.cpp#L2030)) |
| 最上位の `resources[]` | `added_usage` | ✅RS-6b。engine が合成した config の render target の宣言の `usage` に足した flag を、足した順に(今は `SAMPLED` だけ。task が書く render target を rendering 節・preset の pass が `input` で読むため。graph の `render.image` と feature の render target。[第6章](06_rendering.md) §6.18 の同じ項)。`usage` にはもう入っている。feature の文書は書き換えない。足していない resource には鍵が無い([出す所](../../src/core/renderingpass/frameplanner.cpp#L2159)) |
| 最上位 | `diagnostics` / `deactivated_graph_nodes` | ✅GL-2a。compiled pipeline の診断 `[{kind, subject, detail}]`(kind は `graph_variant_selected` / `pipeline_preset_resolved` / `feature_excluded` / `graph_node_deactivated`。✅FG-1 の `fold_stale` / `fold_unverified` と ✅PO-1 の `unread_output` / `unused_binding` は下の行)と、変種で非活性にした graph の node `[{node, feature, reason}]`(reason は `target_ref` / `insert` / `order` / `pass_replacement` / `loop_output`。第6章 §6.18)。compiled pipeline のある plan では空でも鍵がある |
| 最上位 | `fold_decisions` | ✅FG-1。検査済みの fold group がこの frame graph の `target_planning` に足した決定 `[{group, kind: "node" \| "resource", name, field}]`。無ければ鍵ごと無い([出す所](../../src/core/renderingpass/frameplanner.cpp#L2211))。§13.5 の「断片の決定を宣言する」 |
| `diagnostics` の `fold_stale` / `fold_unverified` | `{kind, subject: <group>, detail: {verified_digest?, current_digest}}` | ✅FG-1。開いた形で走っている fold group。`detail` は文字列ではなく object で、`verified_digest` は `fold_stale` だけ |
| `diagnostics` の `unread_output` | `{kind, subject: <node>, detail: {resource}}` | ✅PO-1。usage を宣言した image を node が書き、同じ frame の後の node が読みも書きもせず、次の frame に history として読む node も無い。`display`、usage に `TRANSFER_SRC` を持つ image、ほかの出力と一緒に書く depth(`DEPTH_STENCIL_ATTACHMENT`)は出さない。buffer と宣言の無い資源も出さない。同じ資源を後でまた書けば、最後に書いた node だけが出る([`findFramePlanUnreadOutputs()`](../../src/core/renderingpass/frameplanner.cpp#L1689)) |
| `diagnostics` の `unused_binding` | `{kind, subject: <pass>, detail: {binding, resource}}` | ✅PO-1。fullscreen / raster の pass が set 1 に渡す入力のうち、shader が宣言しているがコードが読まない binding(spirv-reflect の静的な使用。どれかの stage が読めば読んだことになる)。`resource` は target 名(history の入力は `<名前>@history`)か buffer 名。compute task と material pass は対象外。renderer が `get_frame_plan` を組むときに足す([`appendUnusedBindingDiagnostics()`](../../src/core/vkcore/renderer.cpp#L4212)) |
| `physical_target_plan.decisions[]` | `pelican.plan.fold_decision_applied@1` と `from_fold` | ✅FG-1。`fold_decisions` の 1 件ごとに `{id: "pelican.plan.fold_decision_applied@1", subject: <展開名>, selected: <field>, detail, from_nodes, from_fold: <group>}`([足す所](../../src/core/renderingpass/frameplanner.cpp#L2427)) |
| `physical_target_plan` の `resources[]`・`alias_groups[]`・`decisions[]`・`backend_selection.candidates[]` | `from_nodes` | ✅GL-2a。その要素が関わる frame plan の node 名を plan の順で。resource は読む・書く・履歴を読む・attachment にする node、alias group はメンバーの resource の和、decision(最上位・`lowering_graph`・`planning_opportunities`・`backend_selection` の 4 か所)は `subject` を node → resource → alias group → scope の順で引く(lowering の node は元の node へ開く)。graph 全体の決定は `[]`。backend の候補は failures / diagnostics の `subject` から引くが、実装者の報告では実物で全部 `[]`(候補の拒否の `subject` は候補の名前のため)([`appendFramePlanPhysicalNodeReferences()`](../../src/core/renderingpass/frameplanner.cpp#L2307)) |
| render / compute / snapshot_copy の node | `view_select` | ✅LP-Va。宣言の `{<軸>: <値>}` |
| 同上 | `view_axes` | ✅LP-Va2。node の family の軸 `[{axis, length, runtime, labels?}]`。並びは provider の index の順(外側が先)、`length` は今の世代の長さで実行時の軸は `null`・`runtime: true`、`labels` は cube の `face` だけ(`+X` 〜 `-Z`)。`$main`・`$reflection/planar`・provider の無い family は `[]` |
| node | `no_selected_view: true` | ✅LP-Va2。直近に公開したフレームで `view_select` に一致する view が 1 つも無かった node にだけ付く。公開前と、走った node には鍵が無い |
| fullscreen / raster / compute の node | `reads_loop_index: true` | ✅LC-1。その pass / task の pipeline のどれかの stage が push constant の `[112, 120)`(`pelican_loop.glsl` の回の番号と回数)を宣言している。宣言していなければ鍵が無い([fullscreen / raster](../../src/core/vkcore/renderer.cpp#L4609)、[compute](../../src/core/vkcore/renderer.cpp#L4625)。[第6章](06_rendering.md) §6.18「回の番号を shader に渡す」) |
| fullscreen / raster / compute の node | `schedule: "once"`・`last_run_generation`・`last_run_frame` | ✅PS-1。`schedule: "once"` の pass / task にだけ付く(per_frame の node には鍵が無い。[`schedule` を出す所](../../src/core/renderingpass/frameplanner.cpp#L2082))。`last_run_generation` はその node が最後に走った runtime の世代、`last_run_frame` はその frame の評価の番号で、まだ走っていなければどちらも `null`。記録はその frame を publish した後に残る([出す所](../../src/core/vkcore/renderer.cpp#L4551))。次の frame で走らせ直すのは `rerun_once {node?}` → `{scheduled: [...]}`([第10章](10_tools.md) §10.3。走る条件は[第6章](06_rendering.md) §6.18「一度だけ走る pass と、frame をまたぐ image」) |
| `physical_target_plan.resources[]` | `initial_asset` | ✅IA-1(`c6690eba`)。graph の `render.image` の `initial: {asset}` を持つ image の要素にだけ `{asset, sha256}`。`asset` は書いた参照のまま、`sha256` はその file を持つ asset store の `assets.manifest` の値(載っていなければ `null`)。要素は `logical_resource` で引く([`initial_asset` を出す所](../../src/core/vkcore/renderer.cpp#L4961))。例(test): `{"asset": "assets/luts/ltc.ktx2", "sha256": "<manifest の値>"}`。✅AB-1 から、asset の file の隣に `<file>.import.json` があれば `derived_from: {source, tool}`・`stale`・`stale_reasons` も付きます(`list_derived_assets` と同じ値。上の「音の解析を焼く」)。[第6章](06_rendering.md) §6.18「persistent の image を画像資産から始める」 |
| material pass の node | `drawn_materials`・`drawn_materials_frame`・`drawn_materials_truncated?` | ✅MP-1(`afd923e1`)。`drawn_materials: [{material?, id, draws}]` は、最後に公開した logical frame でその pass が描いた material。`id` は variant と draw_loop の step を解決した後の実効の material、`material` はその名前(名前の無い material では鍵が無い)、並びは最初に描いた順。`draws` は indirect draw の command の数を pass の呼び出し全部にわたって足したもの(GPU が数を書く draw でも CPU の draw queue の range の数)。`drawn_materials_frame` はその frame の `EngineTime::evaluationIndex()`。今の program と世代の frame がまだ公開されていなければ鍵が無く、その frame に描かなかった pass は `[]`。256 件を超えた分は出さず `drawn_materials_truncated: true`([MP-1 で plan に足す所](../../src/core/vkcore/renderer.cpp#L4696))。例(test): `{"material": "glass", "id": <id>, "draws": 1}`。[第6章](06_rendering.md) §6.18「グラフで書く pass」 |

**同じ世代では返り値の大半を使い回します(✅FP-1、`eab549b6`)。**engine は、公開した世代で決まる部分(plan・`execution_plan`・`physical_target_plan`・`native_scope_executors`・`sample_count_plan`・`window_output`・`gpu_resource_arena` など)をその世代で最初に呼ばれたときに 1 回だけ組み、呼ぶたびに読む部分(`shader_resolution`・`preview`・上の表の PS-1 / MP-1 / LP-3a の field・material filter・`runtime_resolution`・照明・`sprite`・`initial_asset` など)を足して返します。返り値の bytes は FP-1 の前と同じで、上の表の鍵も変わりません。組み直すのは世代・世代の番号・rendering pass の id・rendering の root の参照のどれかが変わったときです。分け方と計測は [第6章](06_rendering.md) §6.6「プランダンプ」の FP-1 の段落です。✅FP-2(`0efa856c`)からは、RPC の応答の直列化も result を写さずに文字列をつなぐ形になり、bytes は同じです(commit の記録では Debug の往復 55 → 37 ms)。

**draw_loop の要素の paging(✅LP-3b)。**`{loop_elements: {node: "<graph.loop の id>", offset?, limit?}}` を渡すと、その draw_loop の `elements` が `[offset, offset + limit)` になります。`offset` の既定は 0、`limit` の既定は 256 で範囲は 1〜4096。名指さなかった draw_loop は先頭の 256 件です([`pageDrawLoopElements()`](../../src/project/materialdrawloop.cpp#L272))。形の誤りは JSON-RPC の invalid params、`node` が draw_loop でなければ同じ invalid params に `data: {code: "draw_loop_unknown", node}` が付きます。

#### `edit_preset_pass` の `view_select`(✅LP-Va2)

`edit_preset_pass`(root pass の編集を含む)は、どの pass の type の置換 payload でも `view_select` を受けます。**省略は保持、`null` は member の削除、値は置換**です。値は compiler と同じ規則([`parseRenderViewSelect()`](../../src/core/renderingpass/renderingpassjsonhelpers.cpp#L464))で読まれ、形の誤りは `render_pipeline_preflight_failed` で、message はその parse の文(`view_select must be a non-empty object of axis names to non-negative integers` など)で終わり、文書は変わりません。軸が family にあるかどうかは組み直しの検査(`graph.view_select_*@1`)です。

#### `edit_graph_document` の definition の op(✅LP-0c)と、ループの編集

上の「`ops[]` と `scope`」の表の `add_definition` / `remove_definition` / `rename_definition` の 3 つで、ループの body になる definition を作れます。

- definition の op は文書全体に当たるので `scope` を取りません(`"root"` と null 以外を書くと断る)。名前は node の id と同じ規則(空でなく `/` を含まない)で、`"root"` は op の `scope` と紛れるので断られます。`add_definition` の `definition` は `{nodes, outputs}` を持ち、中に `graph.instance` があれば `graph.nested_instance@1`、`graph.loop` があれば `graph.loop_nested@1`、id が重なれば `graph.node_duplicate@1` です。
- `rename_definition` は、その definition を名指す `graph.instance` の `definition` と `graph.loop` の `branches[].body` を**同じ op の中で**全部書き換えます。
- **列の最後の検査。**次の 3 つは op ごとではなく、列を全部当てた後の候補で 1 回だけ検査します([`runDeferredCheck()`](../../src/project/graphcomposition.cpp#L2126))。そのため「definition に `graph.input` を足す」と「ループや instance の `inputs` に足す」を、どちらの順でも 1 つの列で送れます。
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

**`get_graph_templates {}` → `{templates: [{reference, title, description?, decomposes, document}]}`**([`RenderConfigEditorService::getGraphTemplates()`](../../src/core/communication/renderconfigeditor.cpp#L3489))

- 並びは engine の資源の登録順(`bloom_loop` / `cube_capture_loop` / `planar_mip_chain_loop` / `shadow_cascade_loop`)です。雛形は build 時の表ではなく、同梱の文書そのものを読みます。
- `title` は文書に無ければファイル名から作ります(`_` を空白にし、先頭を大文字にする。`bloom_loop.json` → `Bloom loop`)。`description` は文書にあるときだけ付きます。`document` は雛形の全文です。
- `decomposes` は `{feature, remove_feature?, pass_replacements?, shader_defines?}` です。`remove_feature: true` は feature を外す(bloom)、`pass_replacements` は feature の instance を残してその pass / task を graph の node に置き換える(cascade の `{"shadow_depth": "shadow_cascades"}` など)、`shader_defines` は feature が parameters から全 shader に与えていた define を rendering 節に足す、です。

**`apply_graph_template {base_source_digest, base_preset_digest?, template}`**([`RenderConfigEditorService::applyGraphTemplate()`](../../src/core/communication/renderconfigeditor.cpp#L3508))

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

**`get_render_documents {references?}` → `{documents: [{reference, kind, digest, content}]}`**([`getRenderDocuments()`](../../src/core/communication/renderconfigeditor.cpp#L5885))

- `references` を省くと、今の構成が書きうる文書を全部返します: root(`kind: "root"`)、project の(fork 済みの)preset(`"preset"`)、root・preset・合成結果・pass と資源の出所が名指す project の feature 文書(`"feature"`)。どれも project の中の通常のファイルで、中身がその種類の schema(`pelican.render_pipeline` / `pelican.render_feature`)のものだけです。`content` は bytes そのまま、`digest` はその sha256(64 桁の小文字 hex)。
- `references` で名指したものが上に無ければ `render_document_not_writable` `{method, reference, reason}`。`reason` は `engine_document`(`engine://`)/ `fragment_address`(`#` を含む)/ `outside_project` / `not_render_document`。
- ✅RD-1b: 名指した参照が project の中の path に解決され、そこに今ファイルが無い(編集が消した、undo で作り直す予定の文書など)ときは `render_document_missing` `{method, reference}`。その文書を `restore_render_documents` で作り直すときの `base_digest` は `null` です。
- ✅RR-1(`4549cae8`)から、答えの最上位に `live: bool` が付きます。`false` は fallback の間(BS-1)と、reload に断られた版を editor が持っている間で、`content` と `digest` はどちらでもディスクの bytes です(下の「reload に断られた版を持つ」)。

**`restore_render_documents {documents: [{reference, base_digest, content}]}`**([`restoreRenderDocuments()`](../../src/core/communication/renderconfigeditor.cpp#L5956))

- 3 つの鍵は常に書きます。✅RD-1b から **`content: null` は「その文書を消す」**、**`base_digest: null` は「今その文書は無い」**(作り直すときの CAS)を意味します。両方 `null` は形の誤り(invalid params)です。同じ文書を 2 度名指すのも形の誤りです。
- **書ける範囲**: 今の構成が書く文書(上の `get_render_documents` の範囲)に加えて、✅RD-1b から**今の root と、戻す先の root(とそれぞれの preset)が名指す** project の文書(authoring の fragment、project の preset、materialize / import が作る feature 文書)を作る・消す対象にできます。範囲の外は `render_document_not_writable`(reason は上と同じ)。
- ✅RD-1b の追加の拒否: 作る・消すのが予約の場所 —— `.pelican/`・`code/`・`assets/`・`scenes/`・`input/`・`ui/`・`packages/`・`build/`・`build-*/`・`out/`・`output/` と、`project.json` の `basic_config` が指す文書のディレクトリ(rendering config の root を含むディレクトリは除く)—— の下なら `render_document_not_writable` `{reason: "reserved_location", location}`([`reservedProjectLocation()`](../../src/core/communication/renderconfigeditor.cpp#L5784))。既にある文書の置き換え(RD-1 の形)はこの制限を受けません。root を消そうとすると `{reason: "root_document"}` です。
- **CAS**: 名指した全文書の今の digest が `base_digest` と一致したときだけ書きます(root は engine が持つ文書とディスクの両方)。root を名指さなくても、root がディスクで変わっていれば断ります。ずれは `external_modification` `{method, reference, expected_digest, provided_digest, mismatches[]}` で、`mismatches` に全部の `{reference, expected_digest, provided_digest}` が並びます。✅RD-1b から、ここでの `null` は「文書が無い」を表します(`base_digest: null` なのに在る、など)。
- 通ったら、編集と同じ preflight(変種を含む)と組み直しを書く前に通し、作る・置き換える・消すを**1 つの transaction** で行って ticket を返します。組み直しに通らない中身は `render_pipeline_preflight_failed` `{participant, method, reference}` で、何も書きません。✅BS-1b(`40318ffe`)から、JSON でない `content` は `render_pipeline_preflight_failed` ではなく `render_document_syntax@1` `{method, reference, document, line?, column?, reason}` です([restore の BS-1b の検査](../../src/core/communication/renderconfigeditor.cpp#L6236)。下の「起動時に断られた構成を直す」)。`material_route_default` が変わる restore の commit 結果には、`add_authored_pass` と同じく `restart_required: true` が付きます。
- gate が閉じていれば `gate_closed`。

**commit 結果の `documents`(✅RD-1)。**render の編集の ticket の commit 結果(`get_edit_result` / `step_frame` の `edit_results[]`)に `documents: {<reference>: <sha256>}` が付きます。その commit が書いた文書だけで、作った文書は digest、消した文書は `null`、何も書かなかった(`no_change`)なら `{}` です([`writtenDocumentDigests()`](../../src/core/communication/renderconfigeditor.cpp#L5469))。

**undo の手順(契約から言えること)。**編集の前に `get_render_documents` で読み、編集の commit 結果の `documents` で「何が作られ・消されたか」を知り、戻すときは: 置き換えた文書は読んだ `content` と、`documents` の digest を `base_digest` にして送る / 作られた文書は `content: null`・`base_digest: <documents の digest>` で消す / 消された文書は読んだ `content` と `base_digest: null` で作り直す。RD-1 の時点で残っていた「ファイルを作る・消す編集(`add_authored_pass` / `remove_authored_pass`、fork、materialize、import)は戻せない」穴は、RD-1b で閉じました。

**ディスクで書き換えた render 文書を取り込む —— `reload_render_documents`(✅RL-1)。**`--rpc` で起こした engine(Studio の engine)は file watcher の gate が閉じているので
([第6章](06_rendering.md) §6.7 の「ディスクからの読み直し」)、Studio の外で root(やそれが読む preset・feature・authored の fragment)を書き換えても engine は気づかず、
次の編集が `external_modification` で断られていました。`reload_render_documents {}` はそれらをディスクから読み直して取り込みます
([`reloadRenderDocuments()`](../../src/core/communication/renderconfigeditor.cpp#L6450))。

- params は空の object だけです。答えは ticket(`render-config-reload-<N>`)で、結果は `get_edit_result` / `step_frame` の `edit_results[]` に来ます。
- 読み直すのは `get_render_documents` が返すのと同じ文書です(root、project の preset、project の feature 文書と managed fragment。`engine://` と project の外のものは入りません)。
  読んだ bytes を候補に固定して組み直し(graph の変種を含む)、公開し、editor が持つ digest を進めます。**ディスクには何も書きません。**commit の時の source commit は、
  読んだ bytes がまだディスクに在るかを照らすだけで、間に変わっていれば `external_modification` です。
- committed の結果は `{operation: "reload_render_documents", published_generation, source_commit_called, source_digest, documents: {<reference>: <sha256>}, changed: [<reference>]}` です。
  `documents` は読んだ全部の文書、`changed` はそのうち editor が知っていた digest と違うもの(editor が書いていない文書は、live の世代を組んだ文書の digest と比べます)。
  何も違わなければ公開せず、`no_change: true`・`changed: []`・`source_commit_called: false` です。
- ✅IA-2(`cdcdebc1`)から、文書のほかに、live の世代が `render.image` の `initial` に使う asset の中身(世代を組むときに decode の前に記録した sha256)も比べます。
  どれかが違えば、文書が同じでも組み直して公開します。committed の結果には毎回 `changed_assets: [<asset>]`(書いた参照のまま、辞書順、重複なし、空のこともある)が付き、
  何も違わなければ `no_change: true`・`changed_assets: []` です([結果に `changed_assets` を入れる所](../../src/core/communication/renderconfigeditor.cpp#L6972))。
  文書の変更で新しく足した asset は `changed_assets` に出ず、その文書が `changed` に出ます。asset が image に合わなければ、受けた後の ticket の結果が
  `graph.image_initial_mismatch@1` の rejected で、live は前のままです。詳しくは [第6章](06_rendering.md) の「asset だけを書き換えた reload(✅IA-2)」。
- ✅IA-3(`26473e75`)から、live の世代の値の graph の `value.sample_table` が読んだ表(KTX2)と、その隣の `.import.json` の中身も比べます。
  `changed_assets` には表の参照(node の `asset` に書いた綴りのまま)が出ます。manifest だけが変わったときも、出るのは表の名前です。
  新しい世代は表を読み直すので、node の値は新しい表のものになります。依存の名前は `published_initial_assets` から `published_assets` に変わりました。
  詳しくは [第6章](06_rendering.md) の「表を書き換えた reload(✅IA-3)」。
- ✅PV-1(`a173e70c`)から、committed の結果に公開した世代の `preview`(`{available: true}` か `{available: false, reason}`)が付きます。preview だけが組めない文書も読み直して公開します([第6章 §6.13](06_rendering.md) の PV-1 の段落)。
- 拒否は 2 段です。ディスクの root が読めない・parse できない、組み立てや変種の preflight が通らないものは、ticket を取らずにすぐ返ります(`render_pipeline_preflight_failed` で
  `method: "reload_render_documents"`、読めない文書には `reference`)。✅BS-1b(`40318ffe`)から、root が JSON として読めないときは `render_pipeline_preflight_failed` ではなく `render_document_syntax@1` `{method, document, line?, column?, reason}` で、これも ticket を取らずにすぐ返ります([reload の BS-1b の検査(RR-1 で持つ形に)](../../src/core/communication/renderconfigeditor.cpp#L6507))。runtime の組み直しで断られるもの(無い shader など)は ticket の結果で返ります。
  どちらでも live の世代は前のままで、ディスクの bytes もそのままです。RL-1 の時点では editor の文書も前のままでしたが、✅RR-1(`4549cae8`)から、文書の都合の拒否(PB-1 が一覧に入れるもの)では editor は読んだディスクの版を持ちます(下の「reload に断られた版を持つ」)。
- live の material の物理の契約だけが変わる文書は RS-1 と同じく commit して `restart_required`、`material_route_default` が変わる文書は G-4 と同じく `restart_required: true` です
  (コードの読み。test はこの道を通していません)。
- **`reload_busy`**(`method`・`reason: "ticket_pending" | "preview_open"`・`tickets`): render の編集の ticket が待っているか処理中、または scene の preview が開いているとき。
  gate が閉じていれば `gate_closed` です。
- RL-1 の時点では、ディスクが組めないままの間、続く編集は `external_modification` で断られ、ディスクを元の bytes に戻せば元の digest で編集が通りました。
  ✅RR-1 から、断られた後の editor の base はディスクの版です。前の live の digest の編集は `external_modification` のまま(CAS は緩めません)で、ディスクの版の digest を base にした
  restore と構造の編集が通ります。ディスクを元の bytes に戻したときは一度 `reload_render_documents` を送ります。その reload は `no_change` にならずに組み直して committed になり、
  `live` が `true` に戻り、その後で元の digest の編集が通ります(GPU の test)。
- engine は undo の履歴を持たず、commit の記録では読み直しを undo の項目にしません。✅RL-1s(`e58c8da9`)から、Studio はディスクの文書が自分の書いたものでないと見るとこの RPC を呼びます(下の「外のエディタで書き換えた render の文書を拾います(✅RL-1s)」)。
- test: `renderconfigeditor_test` の
  [「RL-1 reload_render_documents takes in a preset changed on disk without writing it」](../../test/renderconfigeditor_test.cpp#L9355)
  (fork した preset を外で書き換え、`changed` がその preset だけで、公開が 1 回進み、ディスクを書かず、次の preset の編集の CAS が通ること。`no_change`。待っている ticket での `reload_busy`)と、
  `engine_primitives_test` の
  [「RL-1 reload_render_documents takes in a root rewritten on disk, and refuses what cannot compose」](../../test/engine_primitives_test.cpp#L2246)
  (GPU。`no_change`、外で書いた `target_planning` が読み直しで alias を外し、次の編集が通ること、待っている ticket での `reload_busy`、parse できない root と無い shader が断られて
  live が前のままであること。✅RR-1 から、断られるたびに `get_render_documents` が `live: false` で断られた bytes を返すこと、元の bytes に戻した後の reload が
  `no_change` でない committed で、`source_digest` が元の digest、`live: true` に戻ること)。着地の W1 で 59 件が緑(FG-2 との衝突を解消した後。統合側の報告)。

**reload に断られた版を持つ —— 読みの `live` と `held_unparsed`(✅RR-1 `4549cae8`)。**RR-1 の前は、`reload_render_documents` が断られても editor は前の版を持ったままでした。
restore の CAS は root を editor が持つ版の digest とディスクの両方で比べるので([restore の CAS が root を比べる所](../../src/core/communication/renderconfigeditor.cpp#L6184))、利用者が保存して断られた root は
`restore_render_documents` でどの digest でも `external_modification` になり、その間の `get_graph_instances` などは保存前の版を返し、その間の編集も断られていました
(PB-1s の報告。上の §13.1 の「Studio の Problems のドック」の「仕様から外れた判断」)。RR-1 から、断られたディスクの版が editor の base になり、live の世代は前のまま描き続けます。
fallback(BS-1 / BS-1b)の editor が組めないディスクの版を持つのと同じ持ち方です。

- **持つ時**: reload の拒否のうち文書の都合のもの(PB-1 が一覧に入れるもの。`reload_busy`・`gate_closed`・`external_modification`・`stale_source_digest` は除く)。
  ticket を取らずにすぐ返る拒否(文法の検査、組み立て、文書の読み、`material_route_default` の読み、変種の preflight)も、ticket の結果の拒否(runtime の組み直し)も同じです
  ([`RenderConfigEditorService::holdRefusedDocuments()`](../../src/core/communication/renderconfigeditor.cpp#L6383)。ticket の結果は [`RenderConfigEditorService::noteHeldDocuments()`](../../src/core/communication/renderconfigeditor.cpp#L6418))。
- **持つもの**: root の bytes と digest(JSON として読めなければ BS-1b と同じく読めない bytes のまま)、root が名指す managed fragment をディスクから読み直したもの、読んだ preset・feature の digest。
- **持たないもの**: root のファイルが読めない拒否と、JSON だが `pelican.graph` の object でない root(旧い形の `render_config.legacy_root@1` など)の拒否では、editor は前の版のままです(コードの読み。test はありません)。
- **読みの印**: 構造の読み 8 つ —— `get_graph_instances`・`get_render_authoring_context`・`get_render_features`・`get_fold_groups`・`get_frame_node_order`・
  `get_compute_task_authoring`・`get_ray_callable_records`・`get_scene_acceleration_structures` —— は、`source_digest` の隣に常に `live: bool` を持ちます。
  返すのは editor が持つ版のうち JSON として読める一番新しい版で、`live` はその返した版が live の世代を組んだ版かどうかです
  ([`RenderConfigEditorService::readDocument()`](../../src/core/communication/renderconfigeditor.cpp#L6405) / [`RenderConfigEditorService::readLive()`](../../src/core/communication/renderconfigeditor.cpp#L6409))。
  - 中身の壊れ(JSON だが組めない)を持っている間: ディスクの版を `live: false` で返します。`source_digest` はディスクの digest です(test)。
  - 文法の壊れ(JSON でない)を持っている間: その前に読めた版を返し、`held_unparsed: true` を足します([`RenderConfigEditorService::markRead()`](../../src/core/communication/renderconfigeditor.cpp#L6413))。
    前の版が live なら `live: true` です(test)。中身の壊れで断られた版の後に文法を壊したときは、その断られた版を `live: false` で返します(コードの読み)。
  - 読める版を捨てるのは、ticket が committed になった時と、parse できる版で reload が断られた時です。
  - `--render-fallback` で文法の壊れた root から起動したときは読める版が無いので、これらの読みは今までどおり parse で失敗します(BS-1b と同じ)。
- **`get_render_documents`** の答えは `{documents, live}` です([`live` を足す所(RR-1)](../../src/core/communication/renderconfigeditor.cpp#L5953))。`content` と `digest` は今までどおりディスクの bytes で、`held_unparsed` は付きません。
  `live: false` は fallback の間(BS-1)と、断られた版を持っている間です([`RenderConfigEditorService::documentsLive()`](../../src/core/communication/renderconfigeditor.cpp#L6440))。
- **`get_graph_values` には付けません**。値は live の世代の snapshot です。
- **直す**: restore の `base_digest` と編集の `base_source_digest` は `get_render_documents` の root の `digest` です(中身の壊れでは `get_graph_instances.source_digest.hex` と同じ)。
  - 前の live の digest の編集は `external_modification` です。
  - 構造の編集は、組めれば committed → publish、組めなければ名前付きで断り、何も書きません(test は欄の足りない node のまま `set_node_label` が `graph.port_field_missing@1` で断られ、
    `set_node_parameters` で欄を足すと committed)。
  - 文法の壊れを持っている間: 壊れた bytes の digest の構造の編集は `render_document_syntax@1`、読みが返す live の digest の編集は `external_modification` です
    ([編集の事前条件の RR-1 の注記](../../src/core/communication/renderconfigeditor.cpp#L1872)。test)。Studio の RL-1s の決定 5 は `external_modification` を「ディスクの file が読み込めなかった」の 1 文に読み替えます
    (下の「外のエディタで書き換えた render の文書を拾います(✅RL-1s)」)。直す道は、壊れた bytes の digest を base にした restore か、ディスクを直して reload です(test は restore)。
- **持っている間の reload**: 変わった文書が無くても毎回組み直し、`no_change` にしません(fallback の間と同じ。[`no_change` を決める所(RR-1)](../../src/core/communication/renderconfigeditor.cpp#L6675))。
  素通りすると、直っていないのに PB-1 の問題が消えるためです。直さないままの reload は同じ拒否で、一覧の 1 件も残ります(test)。
- **`live` が `true` に戻る時**: publish する reload か編集(`source_only` でも `no_change` でもないもの)が committed になった時で、PB-1 の問題が消える時と同じです。
  配置だけの編集(`source_only`)はディスクに書いても `live: false` のままで、問題も残ります。RS-1 の `restart_required` の committed でも `true` に戻します
  (PB-1 に揃えたもので、厳密には live の世代と違う版です。実装者の報告)。
- **既存の契約の変更**: RL-1 の「断られた後、ディスクを元の bytes に戻せば元の digest の編集が通る」は、「戻した後に一度 reload が要る」に変わりました(上の RL-1 の項)。
- **残る穴**(実装者の報告): `material_route_default` の比べ(G-4)は editor が持つ root と比べるので、断られた版で値が変わっていると、次の reload の `restart_required` がずれる可能性があります(コードの読み)。
  feature・preset の文書だけが壊れたときの `live: false` の test はありません。
- **Studio**: ✅RR-1s(`471476d0`)から Studio は `live` / `held_unparsed` を読みます。viewport の上の帯が自分で `get_graph_instances` を読み、`live: false` なら
  `Showing the saved version; the picture is still the previous one — <Problems の 1 文>`、`held_unparsed: true` なら `<file> is not valid JSON; the Graph shows the last version that was.` を出します。
  `live: false`(`held_unparsed` でない)の読みの `source_digest` は window の共有の digest に流れ、Graph・Frame Plan・Pass Editor・Features・Rendering settings が読み直して、その digest を base に編集を送ります。
  Show the document の Save はいつも restore で、reload で断られた node は Graph で開きます(下の「Studio のノードキャンバス(Graph タブ)」の「reload に断られた版を持つ間(✅RR-1s)」)。
  RR-1s の前の Studio はこれらを読まず、PB-1s の回避(reload で断られた node は文字の位置へ飛ぶ、Save はファイルを書く)を使っていました。
- test: `bs1_render_fallback_test` の `[rr1]` の 3 本(GPU、Windows だけ。一覧は第6章 §6.16 の「reload に断られた版を editor が持つ」)。着地の W1 は 75/75 が緑
  (`studio_scenario_render_reload_external` の全 60 段を含む。実装者の報告)。

**起動時に断られた構成を直す —— `--render-fallback` の engine(✅BS-1 `d9811bfc` / BS-1b `40318ffe`)。**`--rpc --render-fallback` で起こした engine は、起動時に rendering の構成が組めなくても終わらず、
engine 内蔵の一色の世代で動き続けます。世代の中身、引数を付けない起動が今までどおり致命であること、残る穴は [第6章](06_rendering.md) §6.16 の「起動時に組めない構成と `--render-fallback`」です。
client が読むもの・送るものは次のとおりです。

- **状態**: `get_status.render_pipeline` が `{state: "ready", root}` か `{state: "fallback", root, refusal: {code, ...fields, document?, message}}` です
  ([BS-1 の get_status に足す所](../../src/core/communication/rpcserver.cpp#L1673))。`refusal` は編集の rejected の `error` と同じ形で、graph の拒否には CT-2 の `document` が付きます。
  `root` は project.json の書き方のままの参照です。`--render-fallback` を付けない起動でも、`get_status.render_pipeline` は `{state: "ready", root}` で出ます(コードの読み)。
  `get_frame_plan` は fallback の間だけ、最上位に同じ `render_pipeline` を持ちます。
- **読む**: `get_render_documents` は、組めない root でも root と、その rendering 節が名指す文書のうちディスクにあるものを返します。最初の要素が root で、`content` はディスクの bytes そのものです(test)。
- **直す**: どれも project の構成を publish した時点で `refusal` が消え(`state: "ready"`)、`get_frame_plan` の `render_pipeline` も消えます。
  1. ディスクを直して `reload_render_documents {}` → committed。fallback の間の reload は、変わった文書が無くても毎回組み直し、`no_change` で素通りしません。
     直す前の reload は同じ拒否で(BS-1 の test では `error.code` が `graph.port_field_missing@1`)、fallback のままです。
  2. `get_render_documents` の root の `digest`(壊れた bytes の digest)を `base_source_digest` にした編集。test は `edit_graph_document` の `remove_node` です。
  3. `restore_render_documents` で root を丸ごと置き換える(下の BS-1b)。
- **JSON でない root(✅BS-1b)**: editor は読めない root を bytes のまま抱えて作られます([読めない root を bytes のまま抱える所](../../src/core/communication/renderconfigeditor.cpp#L946))。
  BS-1b の前は、ここで engine が致命で終わっていました(実装者の報告)。`get_status.render_pipeline.refusal` は `render_document_syntax@1` `{document, line?, column?, reason}` です。
  `get_render_documents` は root を `{reference, kind: "root", digest, content}` で返し、`content` は生の文字列、`digest` は壊れた bytes の sha256(64 桁)です。
  rendering 節が名指す feature / preset が読めないときも、その文書が一覧に入ります。
  - 断られるもの: base digest を照らす構造の編集の全部(`edit_graph_document` など、事前条件の検査 [`checkAuthoringPreconditions()` の BS-1b の検査](../../src/core/communication/renderconfigeditor.cpp#L1898) を通るもの)、
    root が読めないままの `reload_render_documents`(ticket を取らずにすぐ)、JSON でない `content` の `restore_render_documents`(`reference` も付く)。
    どれも `error` は `{method, document, line?, column?, reason, code: "render_document_syntax@1", message}` で、何も書きません([`RenderConfigEditorService::syntaxRejection()`](../../src/core/communication/renderconfigeditor.cpp#L1904))。
  - 直る道: ディスクを直して reload するか、`restore_render_documents {documents: [{reference: <root の参照>, base_digest: <壊れた bytes の digest>, content: <正しい JSON>}]}`。restore は committed になり、ディスクに書きます(test)。
  - 既存の契約の変更: `--render-fallback` の有無に関わらず、root が JSON でない reload と JSON でない `content` の restore は、`render_pipeline_preflight_failed` から
    `render_document_syntax@1` に変わりました(上の RL-1 と RD-1 の項。RL-1 と RD-1 の test の期待も書き換えています)。
  - 範囲外: root が JSON だが object でないもの、`pelican.graph` でない旧い形は、`--render-fallback` でも致命のままです(実装者の報告)。
- **Studio(✅BS-1s、`19be8983`)**: Studio は engine をいつも `--render-fallback` 付きで起こし、`get_status.render_pipeline` を読んで、viewport の上の帯と
  Frame Plan の status 行に `Showing a placeholder: …` の 1 文を出します。直す道の 1(Reload from disk と外のエディタの保存)と 2(Graph などの編集)はそのまま使え、
  3 の `restore_render_documents` は帯の **Show the document** の窓が送ります(下の [Studio のノードキャンバス](#studio-のノードキャンバスgraph-タブ)の
  「組めない構成でも engine を止めない(✅BS-1s)」)。BS-1s の前は、Studio の engine が起動時に構成を読めなければ engine は終わり、Frame Plan の status 行が
  致命の 1 文を言っていました(下の「起動時の致命の 1 文」)。
- test: `bs1_render_fallback_test` の 4 本(GPU、Windows だけ。一覧は第6章の同じ節)。

**今の問題の一覧を読む —— `get_problems`(✅PB-1 `a55d1eaf`)。**engine は、起動時の拒否、engine 自身の disk からの読み直しの拒否、`reload_render_documents` の拒否、
shader の読み直しの失敗を、直るまで一覧で持ちます。1 件の形・入る時と消える時・`document_kind` の決め方・例は [第6章](06_rendering.md) §6.16 の「engine が持つ今の問題の一覧」です。
client が読むもの:

- **件数**: `get_status.problems` が `{revision, count, errors, warnings}` です。`revision` は一覧が変わったときだけ増えるので、client は毎回の `get_status` でこれを見て、動いたときだけ全部を読み直せます。
- **全部**: `get_problems {}` → `{revision, problems: [{id, severity, source, code, message, fields, document?, line?, column?, node?, port?, definition?, scope?, document_kind?, at_frame?}]}`。
  `id` は code と場所から決まる 16 桁の hex で、`source` を含まないので、同じ問題が reload でまた出ても同じ id です(行の選択を保つのに使えます)。
- **source**: `startup` / `hot_reload` / `reload_rpc`。編集 RPC の拒否は入りません(その RPC の答えだけ)。`reload_busy`・`gate_closed`・`external_modification`・`stale_source_digest` も入りません。
- **飛び先の材料**: `document`(`project://<path>`)、`line` / `column`(JSON でない文書と shader)、`node` / `port`(と `definition` / `scope`)、`document_kind`
  (`root` / `root_graph` / `definition` / `root_rendering` / `preset` / `feature` / `shader`)。node は拒否が名指すときだけで、engine は推定しません。
- **`--rpc` の下の hot reload**: Studio の engine は render 文書の disk の変更を自分では読み直さない(HW-1)ので、外のエディタで壊した文書が一覧に出るのは、
  RL-1s の監視が `reload_render_documents` を呼んだ後です。shader は監視していれば engine が読み直し、`source: "hot_reload"` で入ります。
- **Studio の判定が本番で外れていた件**: BS-1s の再起動の案内([`renderRestartFeature()`](../../src/devstudio/model/inspectormodel.cpp#L1920))は message の `was first requested after the runtime module graph was frozen` を探しますが、
  module の要る feature の reload の拒否の本番の文面は `Render pipeline hot reload cannot enable feature '<feature>' … missing runtime module <module>` で、一致していませんでした
  (BS-1s の test は hook で engine の代わりに container の文を答えていました)。PB-1 から、この拒否も container の汎用の文も `render_feature_requires_restart@1 {feature?, module}` になったので、
  Studio は code と `fields.feature` で判定する形に直します(PB-1s の決定 6、作業中)。それまでは、どちらの文面でも案内は出ない見込みです(コードの読み)。
  → ✅PB-1s(`93934318`)で直しました。[`renderRestartFeature()`](../../src/devstudio/model/inspectormodel.cpp#L1920) は code が `render_feature_requires_restart@1` のときだけ、
  code の横・`error_fields`・message の code の後ろの fields から `feature` を読み、文面は読みません(下の「組めない構成でも engine を止めない(✅BS-1s)」の再起動の案内)。
- Studio の Problems の窓(一覧・件数・クリックで Graph の node や文書の行へ飛ぶ)は PB-1s で作業中です。
  → ✅PB-1s(`93934318`)から、Studio はこの一覧を **Problems のドック**で読みます(§13.1 の「Studio の Problems のドック」)。`get_status.problems.revision` が動いたときと、
  `reload_render_documents` の答えの後に `get_problems` を読み直します。外のエディタで壊した render 文書が一覧に出るまでは、実装者のシナリオの実測で 0.4〜0.5 秒です。
- test: `pb1_problems_test` の 6 本(GPU、Windows だけ。一覧は第6章の同じ節)。

#### 変種と組み直しの拒否の形(✅LP-0a / LP-0d)

- 組み直し(ticket の commit)で graph が名前付きで断った拒否(`GraphRegistryError`。`graph.target_ref_unknown@1` など)は、`render_pipeline_preflight_failed` に畳まれず、**その code のまま**、`ticket` と `participant` の横に error_fields を並べて返ります(✅LP-0a)。
- 変種(preview / xr)の preflight で断られたときも、graph の名前付きの拒否と、`<code>: <fields>` の形で運ばれる拒否(`material_route_unserved@1` や `graph.view_select_*@1` など)は**その code のまま**で、fields の横に `variant`(`"preview"` / `"xr"`)が付きます(✅LP-0d。[`preflightGraphVariants()`](../../src/core/communication/renderconfigeditor.cpp#L1141))。それ以外の拒否は今までどおり `render_pipeline_preflight_failed` + `variant` です。✅PV-1(`a173e70c`)から編集の preflight は preview を組まないので、`variant: "preview"` の拒否は出なくなり、今 `variant` が付くのは xr だけです。preview だけが組めない候補は committed になり、結果の `preview.available` が `false` になります([第6章 §6.13](06_rendering.md) の PV-1 の段落)。
- 組み直しで回ごとの node が名指された拒否には `loop_origin: {loop, element, node}` が付きます(✅LP-1b)。
- **view family の大きさの不一致(✅VF-1)**: 二次の view family(`$main` 以外)の raster pass は、書く target の大きさが全部同じでなければなりません(renderer が family の view の大きさを pass の target から決めるため)。組み直しで Vulkan の変種を compile するとき、view_select の検査の直後に、family ごとに宣言順で最初の raster pass が書く target を基準にして比べ、違う pass があれば `render.view_family_extent_mismatch@1` で断ります([`validateRenderViewFamilyExtents()`](../../src/core/renderer/viewfamilyproviderregistry.hpp#L184))。
  - fields は `{family, pass, target, extent, expected_pass, expected_target, expected_extent}` で、ループが複写した pass なら `loop_origin` / `expected_loop_origin` が付きます。`extent` は固定の大きさなら `[w, h]`、画面相対なら `{"output_scale": s}` です。固定と画面相対は、結果の大きさが同じでも不一致です。宣言の無い attachment(swapchain など)は比べません。
  - 編集の拒否では上の `graph.view_select_*@1` と同じく**その code のまま** fields を横に並べて返り、文書は変わりません([`isCarriedRefusalCode()`](../../src/core/communication/renderconfigeditor.cpp#L78))。読み込みもこの拒否で止まります。以前は cube の face のループの body に画面の大きさの pass を置くと、player が renderer の検査(`render view family '$capture/cube' spans multiple raster extents`)で止まっていました。renderer の検査は最後の砦として残っています。

### Studio のノードキャンバス(Graph タブ)

**置き場所**: `View > Panels > Frame Plan` の dock(`pelican.framePlanDock`)にある 4 枚目のタブ「Graph」です
([タブの追加](../../src/devstudio/view/mainwindow.cpp#L542))。dock は増えていません。✅RSX から、タブは Frame Plan / Features / Rendering settings / Graph の順です(Rendering settings は下の「Studio の Rendering settings タブ」。RSX の前は Graph が 3 枚目でした)。起動時から開いておきたい場合は
`pelican_studio --project <dir> --tab graph`([第10章](10_tools.md) §10.7)。

**上段の操作列**(左から): `Scope:` のコンボ(`root` と definition 名)、その右のパンくず、領域の `Show:`(✅GL-1。下の「graph の外から来た pass を畳んだ node で見る」)、右端に
`Values from:` のコンボ、`Refresh`([構築](../../src/devstudio/view/graphcanvaswidget.cpp#L5113))。
✅UX-1 から、Refresh の左(Undo / Redo の後ろ)に **Auto layout** と **Fit view** のボタン(`pelican.graphCanvas.autoLayoutButton` / `pelican.graphCanvas.fitViewButton`)があります。右クリックのメニューと同じ action を発火します([ボタンを置く所](../../src/devstudio/view/graphcanvaswidget.cpp#L5379))。UX-1 の前は右クリックのメニューにしかありませんでした(利用者の指摘「graph の自動レイアウトってどこ?」)。
その下に必要なときだけ 1 行の notice が出ます。本体は左がキャンバス、右が選択ノードの詳細表(`Field` / `Value`)で、
詳細表の上に `Label:` の入力欄・**Rename**・**Add**(palette を開くボタン。`pelican.graphCanvasAddNode`)が並びます(選んだ node が `pelican.ui` の sink なら、その下に「UI node」の欄。✅Q-8、下の段落。`audio.playback` なら Sound / Bus / Wrap の欄。✅Q-10、下の段落)。✅UX-8 から、`value.curve` / `value.playback` / `render.scalar` と fullscreen・raster の pass を選ぶと値の欄(`pelican.graphCanvasValueFields`)、✅EL-5s から `render.compute` を選ぶと compute の欄(`pelican.graphCanvasCompute`)も出ます(下の「曲線を shader の値へ GUI でつなぐ」と「project の compute / RT の shader を palette で置く」)。✅UX-12a から、engine の catalog が `parameters` の表を出す op の node を選ぶと、その表から作った欄(`pelican.graphCanvasParameters`。専用の欄を持つ op では畳んだ **More parameters** の下)も出ます(下の「node の欄を catalog の表から作る(✅UX-12a)」)。

1. **scope を選ぶ** —— `root` は文書そのもの、definition 名はその本体です。root の `graph.instance` ノードを
   **ダブルクリック**すると、その definition の中へ降ります。パンくずで戻ります。
2. **値を重ねる** —— `Values from:` で owner を持つ root instance を選ぶと、その instance の入力 port に
   `get_graph_values` の値と供給元が重なり、**出力 port には `get_graph_node_values` の公開値が帯で重なります**(✅AG-3b。definition の中に降りても、その instance 経由の値が出ます)。`(none)` で外れます(✅UX-8 から、root の scope では `Live` が入っていれば `(none)` のままでも root の node の値を読みます。下の「Timeline は root の node を instance 無しで読みます」)。`Live` で 250 ms ごとに読み直し(同時に飛ぶ読みは 1 組まで)、`Timeline` 帯を開くと選択ノードの数値 port(≤4)と owner の animation の phase / progress が貯まり、左ドラッグで `set_time` → `render_frame` の scrub です。
3. **ノードを読む** —— ノードは見出し帯 + ソケット行です。見出しにはラベル(無ければ id)と、右に小さく型名(`op`)。
   見出し左の三角でソケット欄を**折り畳め**ます(tooltip は `Fold the sockets away` / `Show the sockets`)。
   折り畳んだノードでは、辺は見出し帯の左右中央の 1 点に集まります。辺の tooltip は
   `<from>.<port> -> <to>.<port>  (select and press Delete to cut)` です。
4. **ソケットの形が型です** —— 色だけでなく形(`circle` / `diamond` / `square` / `hexagon`)でも型を表します。
   決め方は 1 か所で、**モジュールが自分の型 schema に書いた `display{}` → Studio の既定表 → traits → `unknown`**
   の順([解決順](../../src/devstudio/view/nodegraphics.hpp#L152)、入口は [`socketStyleFor()`](../../src/devstudio/view/nodegraphics.hpp#L154))。
   既定表の中はさらに 2 段で、**型名の完全一致 → `prefixes` の前方一致**です([nodegraphics.cpp:126](../../src/devstudio/view/nodegraphics.cpp#L135)) ——
   だから `pelican.render.` のような名前空間まるごとに 1 行で形を与えられます。既定表は
   [`graph_socket_styles.json`](../../src/devstudio/resources/graph_socket_styles.json#L1)(`pelican.studio.socket_styles` v1)で、
   **新しい型に形を与えるのに C++ を触る必要はありません** —— この JSON に名前を 1 行足すだけです。
5. **右の詳細表** —— `label` / `type` / `domain` / `evaluation` / `id` / `sink` と、あれば `definition` /
   `parameters` / `inputs` / `outputs`([組み立て](../../src/devstudio/view/graphcanvaswidget.cpp#L8397))。
   port の型が宣言されていなければ `(not declared)`、カタログをまだ取れていなければ `domain` / `evaluation` が
   `(catalog not fetched)` です。入力行の tooltip は、その port を**カタログが宣言した**(`declared by the catalog`)のか、
   **文書から読んだ**(`read from the document`)のかを区別します。

**編集はその場で反映されます(Apply ボタンはありません)。**

| 操作 | 送る op |
|---|---|
| 出力ソケットから入力ソケットへドラッグ | `set_input{source}` |
| 辺を選んで `Delete` / 入力ソケットから空所へドラッグ | `set_input{source: null}` |
| 値の帯(未接続で値のある port)をクリックしてその場編集 | `set_node_parameters`(既存の `parameters` を読み、1 鍵だけ差し替え) |
| 右の Parameters / More parameters の欄で値を変える・**Reset**(✅UX-12a) | `set_node_parameters`(node の `parameters` 全体を 1 回。spin と slider の動きは 150 ms まとめる) |
| 見出しをダブルクリック / `Label:` + **Rename** | `set_node_label` |
| node を drag / 辺を double-click(reroute)/ 制御点を選んで **Tie together** / **Untie** / **Auto layout** | `set_layout`(300 ms debounce で 1 回。N-6) |
| 右クリックの **Add node...** か **Add** のボタンでパレットから選ぶ | `add_node`(必要なら複数) |
| ノードを選んで `Delete` | `remove_node` |

削除や切断では、不要になったノード位置・接続線の配置も同じ編集で除去します。
Undoはノードの宣言順・設定・接続と配置を一緒に復元します。`replace_node`は同じIDのノード全体を
検証付きで置換し、`add_node`の任意`index`は削除前の宣言位置へ戻すために使えます。
文字入力中のUndoは入力欄の履歴を使い、保存処理中のグラフへ重ねてUndoを送りません。

型が合わないソケットには吸着しません —— **どちらかの型が宣言されていなければ通し、両方宣言されていれば
同じ型のときだけ**通します([判定](../../src/devstudio/model/graphcanvasmodel.cpp#L2818))。

**線を引いている間、落とせるソケットが光り、近ければ吸い付きます(✅UX-1)**: Blender の Node Wrangler の lazy connect の感覚です(利用者の指摘)。

- 線を引き始めると、canvas の上のソケット(node のソケット、ゾーンの縁の口、上の「Graph から画面に出す」の pass の入力の口)ごとに、そこで離したら何が起きるかを判定します([`GraphCanvasScene::lightDropTargets()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2852))。落とせるソケットは 1.35 倍に、落とせないソケットは不透明度 0.3 に描きます。空いた口の `+` は、その口の判定に従います。
- 判定は、離したときに使う関数と同じ 1 つです([`GraphCanvasScene::dropAction()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2915))。型の判定は model の `canConnect()` と同じ中身を、描いた node の一覧に対して問います([`GraphCanvasModel::canConnectAmong()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2830))。第二の判定はありません。
- 吸着: カーソルから画面の上で 32 px 以内にある落とせるソケットのうち、一番近いものに線の先が寄り、そのソケットは 1.7 倍になります。半径は画面の px で、ズームに依りません(`GraphCanvasScene::GraphCanvasSnapRadius`。[寄せる所](../../src/devstudio/view/graphcanvaswidget.cpp#L2877))。
- 離すと、吸い付いていたソケットへ落とします。吸い付いていなければ、今までどおりカーソルの下のもので決めます([`GraphCanvasScene::finishConnection()`](../../src/devstudio/view/graphcanvaswidget.cpp#L3042))。
- test: `devstudio_graphcanvas_scene_test` の [「UX-1 a line being drawn lights the sockets it can land on and snaps to the nearest one in reach」](../../test/devstudio_graphcanvas_scene_test.cpp#L7644)。

⚠ **2 つの注意。**
(1) **必須の scalar 入力を持つ op を足すと、種の定数も一緒に足されます。**未接続の必須入力を持つ文書は engine が
受け取らないので、パレットは必須の scalar 入力ごとに `value.constant` を**同じ ops 列で先に**足し、そこへ繋いだ
状態のノードを作ります([生成](../../src/devstudio/model/graphcanvasmodel.cpp#L3023))。1 操作 1 チケットは保たれます。
後で本物の producer を繋いでも、**この定数は自動では消えません**(手で消します)。
例外は `audio.playback` の `time` で、定数ではなくシーンの時計に繋ぎます(✅Q-11。下の「`audio.playback` の欄」)。
(2) **必須入力の切断は送られ、engine が断ります。**Studio は黙って定数を挿し直したりしません ——
`set_input{null}` をそのまま送り、`unconnected graph input` の拒否を notice に出します。値を持つ instance の port は
(戻り先の `parameters` が残っているので)切断できます。

**palette はキーボードだけで使えます(✅GL-1d)**: 右クリックの **Add node...** と **Add** のボタンは、同じ関数で palette(`pelican.graphCanvasPalette`)を開きます([`openPalette()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7727))。

- 開いたとき、キーボードは絞り込みの欄(`pelican.graphCanvasPaletteFilter`)にあります。click せずにそのまま打てます。
- ↑ ↓ と PageUp / PageDown は一覧(`pelican.graphCanvasPaletteList`)の選択を動かします。キーボードは絞り込みの欄に残ります([キーを一覧へ回す所](../../src/devstudio/view/graphcanvaswidget.cpp#L4809))。
- Enter は OK(選んだ行を足す)、Esc は Cancel(何も送らない)です。
- **Add** のボタンは、右クリックのメニューと同じく event loop に回してから palette を開きます([ボタンの所](../../src/devstudio/view/graphcanvaswidget.cpp#L5990))。palette は modal のダイアログなので、GL-1d の前はボタンの click の中で開いていました。今は click が返ってから開くので、自動化(シナリオ)もボタンで palette を開けます。✅UX-9 から、palette は `exec()` ではなく window-modal の `open()` で開き、閉じた後に選んだものを event loop から足します。保存の最中でも開きます(下の「保存中でも palette が開く」)。
- test: `devstudio_graphcanvas_scene_test` の [「GL-1d the Add node palette opens with the keyboard on its filter」](../../test/devstudio_graphcanvas_scene_test.cpp#L3552)(開いた直後の focus が絞り込みの欄、click なしで打った字が欄に入る、↓ で一覧の行が 0 から 1 に動く、Esc で何も送らずに閉じる、Enter で ↓ の行が足される)。

**`pelican.ui` の sink の node を選ぶ(✅Q-8)**: palette の `ui.value` / `ui.visible`(GP-3 の `pelican.ui` module の 2 つの sink)は、
書き込む UI node の path を parameter `node` に持ちます。

- **置くと `node` が入っています。**palette で置くと、project の UI 文書の最初の path が `parameters.node` に入ります
  ([置くときに node を入れる所](../../src/devstudio/model/graphcanvasmodel.cpp#L3551))。`ui.value` は最初の gauge(engine が値を書けるのは
  gauge だけで、他の widget は `ui.property_mismatch@1` で断られます)、`ui.visible` は文書の最初の node(root)です。UI 文書が
  読めなければ `node` を書かずに送り、engine の名前付きの拒否になります。Q-8 より前は `node` が空のまま出て、置くと必ず
  `ui sink requires a node path` で断られていました。
- **詳細表の上に「UI node」の欄が出ます。**選んだ node が `ui.value` / `ui.visible` のときだけ、打つこともできる一覧
  (`pelican.graphCanvasUiNode`)が出ます([UI node の欄を合わせる所](../../src/devstudio/view/graphcanvaswidget.cpp#L7133))。項目は UI 文書の
  node の path(root からの id を `/` で繋いだもの)で、`ui.value` では gauge だけです
  ([一覧を gauge に絞る所](../../src/devstudio/model/graphcanvasmodel.cpp#L571))。一覧から選ぶか、打って Enter で `set_node_parameters`
  1 回です。一覧に無い path はそのまま送られ、engine が名前付きで断ります。
- **一覧の出所は project の UI 文書です。**UI の木を返す RPC は無いので、Studio が `project.json` の
  `basic_config.ui_config_json` が指す文書を engine と同じ project path resolver で読みます
  ([UI 文書を読む所](../../src/devstudio/model/graphcanvasmodel.cpp#L476))。欄を出す直前と palette を開く直前に読み直すので、開いた後に
  文書を書き換えても一覧はそれに従います。読めないときは欄の placeholder が理由(`project.json names no UI document
  (basic_config.ui_config_json)` など)を言います。

**`audio.playback` の欄(✅Q-10)**: AU-1 で `audio.playback` に入力と parameter が増えました。

- **増えた port は engine の catalog から来ます。**`position`・`source_begin`・`source_end`・`speed`・`delay` と `events` は Studio が
  他の op と同じく catalog から描き、必須は `time` だけです([engine の宣言](../../src/project/graphmodule.cpp#L817))。`events` は任意の
  入力なので、繋いでいなくても Studio は問題として扱いません。任意の数値の port に値を入れるのは、今は繋ぐことでだけです(既存の挙動)。
- **port でない 3 つの parameter は、詳細表の上の欄です**(`pelican.graphCanvasAudioRow`。選んだ node が `audio.playback` のときだけ。
  [欄を合わせる所](../../src/devstudio/view/graphcanvaswidget.cpp#L7587)):
  - **Sound**(`pelican.graphCanvasAudioAsset`、打つこともできる一覧)= parameter `asset`。一覧は project の中の `.wav` / `.ogg`
    (大文字小文字は問わない。`.` で始まる folder は見ない)を `project://...` で並べたものです([音声を探す所](../../src/devstudio/model/graphcanvasmodel.cpp#L509))。
    音声の一覧を返す RPC は無いので Studio が folder を歩きます。project を開いたとき、選ぶ node が変わったとき、palette を開く直前に探し直します。見つからなければ
    placeholder が理由(`no .wav or .ogg file in the project` など)を言います。打った path はそのまま送り、中身は engine が確かめます。
  - **Bus**(`pelican.graphCanvasAudioBus`、選ぶだけ)= `se` / `bgm`。engine が受け取るのはこの 2 つだけで、それを一覧にする RPC が無いので、
    **一覧は Studio が持っています**([bus の一覧](../../src/devstudio/model/graphcanvasmodel.cpp#L556))。文書に別の値があれば、それも一覧に出します(engine が名前付きで断ります)。
  - **Wrap**(`pelican.graphCanvasAudioWrap`、選ぶだけ)= `once` / `repeat` / `ping_pong`。`position` を繋いだ node(分解版)は wrap を
    持てない(持つと `audio.playback_mapping_conflict@1`)ので、そのときは欄が無効です。
  - 選ぶか Enter で `set_node_parameters` 1 回です。既に効いている値(Bus / Wrap は、鍵が無ければ一覧の先頭の既定値)を選んでも何も送りません
    ([送る所](../../src/devstudio/view/graphcanvaswidget.cpp#L7635))。
- **palette で置くと、project の最初の音声が `asset` に入ります**([置くときに asset を入れる所](../../src/devstudio/model/graphcanvasmodel.cpp#L3566))。
  任意の `events` / `position` は空のままです。
- **palette で置くと、必須の `time` はシーンの時計に繋がります(✅Q-11)。**上の (1) の種の定数(0)ではなく、今の scope に
  `value.input{parameters:{name:"scene_time"}}` があればそれを使い回し、無ければ 1 つ足して(id は `scene_time`。その id が今の scope で使われていれば `scene_time_2` …)、
  その `value` を `time` に繋ぎます([シーンの時計を繋ぐ所](../../src/devstudio/model/graphcanvasmodel.cpp#L3516) /
  [`GraphCanvasModel::sceneTimeNode()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2284))。時計の追加と node の配置は**1 回の編集**で送るので、
  **Undo 1 回で両方が戻ります**。AU-3 から engine は `events` も `position` も繋がっていない `audio.playback` を読み込み、owner の最初の
  frame から鳴らすので、**置いただけで読み込まれ、鳴ります**。
  - 使い回すのは**今の scope** の `scene_time` だけです。definition の中で置くと、root に `scene_time` があってもその definition に新しく 1 つ足します。
    名前が `scene_time` でない `value.input`(たとえば `time`)は使いません。
  - 足した `scene_time` の node は、✅UX-8 から主の node の左の、描かれた箱と重ならない所に置かれます(セッションの位置。下の「新しい node の置き場所」)。UX-8 の前は配置を持たず、canvas の初期配置で描かれていました。
- 音声の format の拒否(`audio.format_unsupported@1`)は、この tab の notice に Q-6 の表の文で出ます(§13.1「Studioでcomponentを追加する」)。
- ✅UX-8 から、Sound の欄の右に **Import…**(`pelican.graphCanvasAudioImport`)があり、`.wav` / `.ogg` を project の `assets/audio/` に写して、選んでいる `audio.playback` の `asset` に入れます。File ▸ Import Sound… と drag & drop も同じです(下の「音の node を置く・音声を Import する」)。✅UX-9 から、node の下の帯に voice の状態(`playing · 1.2 s`)が出ます(下の「保存中でも palette が開く」の voice の帯)。

**ループを置く —— graph.loop(✅LP-4c)**: Graph タブは共通グラフのループ `graph.loop`(書き方は [第6章 §6.18](06_rendering.md)、設計は [design_graph_loops](../design_graph_loops.md) §9)を、parameters の JSON ではなく専用の欄と表で組みます。
ループの RPC(definition の op、`loops[]` と `loop_origin`、`set_input` の `element`、`decompose_material_pass`、`get_graph_templates` / `apply_graph_template`)は、上の [「ループと分解の RPC —— engine の契約」](#ループと分解の-rpc--engine-の契約lp-0alp-vb--rd-1--rd-1b) にあります。
LP-4c の着地時はゾーン表示が無く、body は instance と同じく「中に入って」編集していました。✅LP-4d1 から、root の canvas はループを枠(ゾーン)で描き、枠の中の body をその場で編集します(下の「ループをゾーンで見る・編集する」)。「中に入る」道(Open body とダブルクリック)も残っています(下の「body に入る」)。

- **Add loop**(root だけ): キャンバスの右クリックの **Add loop**(`pelican.graphCanvas.addLoop`)は、空の definition を 1 つ作り、それを body にするループを root に置きます。
  - definition の名前は Studio が提案します(`loop_body`、使われていれば `loop_body_2` …)。打つものはありません。
  - `add_definition` と `add_node` を 1 つの列で送るので、Undo 1 回で両方が戻ります([`GraphCanvasModel::addLoopOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3809))。
  - 置いたループは `over: {range: {count: 1}}`・`branches: [{body: <definition>}]` です。
- **palette から**: palette(**Add node...**)の `graph.loop` は root にだけ出ます。選ぶと body の combo が出ます。
  - 先頭の **New definition…** では、名前の欄(提案で埋まっている。`pelican.graphCanvasPaletteNewDefinition`)で名前を変えられます。
  - 既にある definition を選ぶと、それが body になり、その出力が全部 `p`(最後の回の値)として宣言されます。
- **Add fullscreen pass**(`pelican.graphCanvas.addFullscreenPass`。右クリック。catalog に `render.fullscreen` があれば root でも definition の中でも出ます): `engine://passthrough` で描く fullscreen pass と、それが書く自前の `render.image`(id は `<pass>__color`)を 1 回で足します。空の body に最初の pass を置く手段です。
  - ⚠ LP-4c の着地時は、pass に `pass_set: "main_render"` を書き、置き場所の無いループの body に最初の pass を置くとループに `canonical_anchor: "post_main"` も足していました。RU-1(`fa413aeb`)から、どちらも書きません(engine が LP-1e で、置き場所の無い graph の pass とループの既定の位置を持ったため)。
  - image は `R16G16B16A16_SFLOAT`・画面の大きさ・usage `COLOR_ATTACHMENT` / `SAMPLED` です(✅UX-6a から。それまでは `R8G8B8A8_UNORM` で、usage に `TRANSFER_SRC` もありました。下の「Studio が作る image は float で、TRANSFER_SRC を付けません(✅UX-6a)」)。✅UX-3 から、palette の `render.raster` も同じ所で組み、同じ形に `draw: {vertex_count: 3}` を足して来ます(下の「palette で置ける render node」)。
  - ✅LP-4f2 から、**view の軸で回すループの body** に置くときは、image の大きさをそのループの family に合わせます([fullscreen / raster の pass と image を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L3292)、[大きさを引く所](../../src/devstudio/model/graphcanvasmodel.cpp#L3990))。engine は 1 つの view family の raster pass が違う大きさに書くことを断る(`render.view_family_extent_mismatch@1`。下の「view family の大きさの拒否」)ので、画面の大きさの image を cube の 256×256 の family に混ぜないためです。
    - 基準は、engine が `get_frame_plan` の二次 family の node に載せる `family_extent` / `family_layers` です(✅LP-4f3。engine の ✅VF-2 で、VF-1 の拒否と同じ helper が決める基準)。LP-4f2 では Studio が `get_render_authoring_context` の `render_targets[]` を自分で辿っていましたが、LP-4f3 で外しました。family が `family_extent` を持たなければ、今までどおり画面の大きさで作り、engine の拒否の文に任せます。
    - 固定の大きさなら image に `width` / `height` を書き、`layers` が 2 以上ならそれも書きます(cube なら 256×256・6 layers)。画面相対なら `extent_scale` をその値にします。
    - pass には `view_family`(ループの family)を書き、固定の大きさなら `resolution_domain: "independent"` も書きます。
    - 基準が読めない(plan か target の宣言が無い)ときは今までどおり画面の大きさで作り、`view_family` だけを書きます。大きさが合わなければ engine が名前付きで断ります。

**ループの node(✅LP-4c)**: ✅LP-4d1 からの root の canvas では、ループはふつう下のゾーンで描かれ、この node の形になるのは、三角でゾーンを畳んだとき、同じ body を先のループがゾーンに描いているとき、body を持つ分岐が無いときです。見出しの右の型名の所に `↻ loop` が出ます。engine の frame plan の `loops[]` が届くと、`↻ expand ×3` のように段と回数に変わります([見出しの字を決める所](../../src/devstudio/model/graphcanvasmodel.cpp#L1950))。

- 入力ソケットは、body を持つ最初の分岐の body の `graph.input` です(型は付けません)。
- 出力ソケットはループが宣言した `p` / `p[]` で、`p[]` は `<名前>[] (each)` と出ます(回ごとの値の束)。
  - ⚠ LP-4c の着地時は、`p[]` の行の port と字が入れ替わっていて(port が `<名前>[] (each)`)、そこからの線がソケットに届いていませんでした。✅LP-4d1 から、字が `<名前>[] (each)`、port が `<名前>[]` です([行を組む所](../../src/devstudio/view/graphcanvasdrawing.cpp#L158))。
- parameters(`over`・`branches` …)はソケットにも値の帯にも出ず、詳細表にも `parameters` の行を出しません。詳細表の `body` の行は、入るときの body です。
- Graph タブは、文書にループがあるときだけ、文書を読むたびに `get_frame_plan` を 1 回読んで `loops[]` を使います。✅LP-4f1 から、ループが無くても root に view_select を取る render node があれば読み、同じ plan から node の `view_axes`(view family ごとの軸)と render target の名前も集めます(下の「view の軸で回す」。[読みを決める所](../../src/devstudio/model/graphcanvasmodel.cpp#L830))。✅Q-19 から、root の instance の definition に render node があるときも読み、同じ plan から pass の `pass_inputs` も集めます(下の「render node の入力は shader の宣言から」)。✅GL-1 から条件は無く、文書を読むたびに読みます(下の「graph の外から来た pass を畳んだ node で見る」)。

**ループをゾーンで見る・編集する(✅LP-4d1)**: root の canvas は、root の各 `graph.loop` を枠(ゾーン)で描き、枠の中に今の分岐の body の node を並べます(Blender のループゾーンの見え方。保存の形は変わらず、body は definition 名のままです)。枠の中の編集は body の definition へ、枠の移動は root へ書きます([ゾーンを計画する所](../../src/devstudio/view/graphcanvaswidget.cpp#L1683)、[`Builder::addZone()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L856))。

- **見出しの帯**: 左から三角、ループの label(無ければ id)、右端に段の字です。
  - 段の字は engine の `loops[]` の報告から作ります: `expand ×2` / `draw_loop N materials`。報告の前は `loop` です。
  - ✅LP-4f1: view の軸で回すループの段の字は `×6 (face)`(回数と軸)です。報告の前は frame plan の軸の長さを使い、それも無ければ `loop (face)` です。分岐が 1 つで長さが 1〜32 なら、タブの位置に軸のラベル(`+X` `-X` … 。ラベルの無い軸は番号)が小さく並びます(読むだけ。tooltip `Element <i> runs for this view`。[段の字を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L1791)、[ラベルを描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1007))。
  - body の definition を他の instance やループも使っていると、段の字の左に橙色で `shared (N users)` が出ます(tooltip は `Other instances or loops run <definition> too: an edit here changes them all.`)。N は使っている node の数で、✅Q-17 から、1 つのループは分岐がいくつ同じ body を使っていても 1 と数えます([`GraphCanvasModel::definitionUseCount()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1689))。Q-17 の前は、同じループの 2 つの分岐が同じ body を使うだけで `shared (2 users)` と出ていました。
- **分岐のタブ**: 分岐が 2 つ以上のときだけ、見出しの下に `if <条件>` / `elif <条件>` / `else` のタブが並びます。else だけのループにはタブがありません。
  - 条件の字は `element = 0` / `element in 1, 4` / `element not in …` です。draw_loop の分岐は material・`tags …`・contract・`skinned`・`has variant` などを ` & ` でつなぎ、式の条件は `expression` です([`zoneCondition()`](../../src/devstudio/view/graphcanvaswidget.cpp#L1298))。
  - タブを押すと、その分岐の body に描き替わります([`GraphCanvasModel::showLoopBranch()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1665))。どのタブかはセッションの状態で、文書には書きません。Open body で入る body も、タブが選んだ分岐のものです。
  - `"body": null` の分岐のタブは薄く描かれ、押しても変わりません(tooltip `This branch runs nothing`)。
  - ✅UX-1 から、選んでいるタブはテーマの `tab_selected` の地に `tab_selected_text` の太字で描きます([選んだタブを描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L966))。UX-1 の前は枠の色で塗っていたので、選んだタブが見出しの帯と同じに見えました。token は `studio_themes.json` の `canvas` にあります(dark は `#f2c14e` の地に `#1b2027`、light は `#2f6fb5` の地に `#ffffff`)。
  - ✅UX-1 から、ほかの分岐と同じ body を走らせる分岐のタブには `· shared with else`(相手が else でなければ `· shared with branch <番号>`)が付き、tooltip がここを直すと両方が変わることを言います([判定する所](../../src/devstudio/view/graphcanvaswidget.cpp#L1830))。文書が定義していない body を指す分岐のタブは薄く描かれ、tooltip は `This branch runs <body>, which the document does not define` です。
  - UX-1 の前は、Add branch の分岐が else と同じ body を指したので、どのタブを押しても同じ body が出て、切り替わらないように見えました。今は Add branch が body を写します(下の「分岐表」)。選んだ分岐は、body が同じでもタブが選んだ番号で覚えます([`GraphCanvasModel::loopBranch()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1628))。
- **左右の縁**: 1 行が 1 つの名前です。
  - 左の縁: 外側(枠の線の上)にループの入力のソケット、内側に body の `graph.input` のソケットです。body の `graph.input` は node としては描きません。内側のソケットから body の node の入力へ drag すると、その `graph.input` の `value` からの `set_input` が出ます。
  - 右の縁: 内側に body の出力(definition の `outputs`)の端、外側にループが宣言した `p` / `p[]` のソケットです。出力を作る body の port から内側の端へ破線が引かれます(描くだけです)。
  - ✅LP-4d2 から、編集できる文書では、左右の縁の行の最後に空いた口 `+` が 1 つずつあります。右の縁の内側の端は、carry を引く始点にもなります(下の「縁をまたぐ線・carry・展開の表示」)。
- **枠の大きさ**: body の node の外接に余白を足したもので、最小の幅と高さがあります([`layoutZone()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L741))。body の node を枠の左や上へ動かすと、枠がその側へ広がります。body が空でも枠は描かれます。✅LP-4d2 から、縁の行の数には `+` の口の行も数え、下端には展開の行と carry の矢印の通り道の分の高さが足されます。drag の間も枠が追従します(下)。
- **枠の中の編集**: body の node は、definition 自身の `layout.nodes` の位置(枠の内側の原点から)に描かれます。上の「送る op」の表の操作は、枠の中では全部 `scope: <definition>` で出ます([編集を body へ送る所](../../src/devstudio/view/graphcanvaswidget.cpp#L2295))。
  - node の drag は definition の `set_layout`、線を引く・切るは `set_input`、Delete は `remove_node`、見出しのダブルクリックは `set_node_label`、値の帯は `set_node_parameters` です。
  - 線は同じ scope の中だけで引けます。例外は ✅LP-4d2 の、root の node の出力を枠の中の node の入力へ直接落とす線で、ループの入力を足す 1 回の編集になります(下の「縁をまたぐ線・carry・展開の表示」)。それ以外では、body の node と root の node の間には引けません。
  - 位置の無い body の node(palette で足した直後など)は、枠の中で自動で並びます。
  - `Values from:` の出力の値の帯は、ゾーンの body の node には重なりません([外す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1779))。
  - body の node を選ぶと、右の詳細表と欄(draw step の欄を含む)はその definition の node を見せます。
- **body に足す**: 右クリックのメニューを枠の中で開くと、Add node... と Add fullscreen pass は body に足します。ゾーンを選んでいる(見出しを押した)か、枠の中の node を選んでいるときも、Add fullscreen pass と palette(Add node...)は body に足します([足す先を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L2604))。palette は body の scope の候補を出し、`graph.loop` と `graph.instance` は出しません。Add loop は枠の外で開いたメニューにだけ出ます。
- **ゾーンを選ぶ・動かす**: 枠の中の node の無い所か見出しを押すと、ゾーン(ループ)が選ばれ、Loop の欄が出ます。見出しを drag すると、root の `set_layout`(ループの位置)だけが出ます。body は枠の子として付いてきて、definition の `layout` は書きません。逆に枠の中の node の drag は definition の `set_layout` だけで、1 回の drag で両方が書かれることはありません。
  - **Auto layout** は root だけを並べ直します(ゾーンは 1 つの箱です)。body の配置は変えません([`GraphCanvasScene::autoLayout()`](../../src/devstudio/view/graphcanvaswidget.cpp#L609))。
  - ✅UX-1: 見出しで drag したゾーンが、離した後に元の所へ押し戻されたように見えることがありました(利用者の指摘)。文書には動かした位置が書かれていて、動いていたのは view です。view は viewport より小さい scene rect を中央に置くので、drag の間や描き直しの後に scene rect が変わるたび、スクロールバーが出るたびに、画面の上の全部がその変化の半分だけずれました(コードの注記)。今は scene rect を変えるとき、各 view に今見えている範囲を含めた rect を渡してスクロールをそのままにし([`GraphCanvasScene::applySceneRectNow()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2695))、Graph タブの view はスクロールバーを縦横とも常に出します([`configureGraphCanvasView()`](../../src/devstudio/view/graphcanvaswidget.cpp#L9054))。
  - `81b62382` から、drag(node・reroute・tie)の間は scene rect を広げるだけで、縮めません([`GraphCanvasScene::scheduleSceneRectUpdate()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2684))。それまでは drag の 1 歩ごとに描いたものから rect を計算し直したので、一番左の node(畳んだ feature など)を drag すると rect の端が view の下で動き、canvas 全体が滑りました。この変更に test は足していません(commit の stat)。
  - ✅UX-1: 文書が配置を持たない root で、枠の中の body の node を drag すると、root が描くたびに自動配置をやり直し、ゾーンが元の所へ戻りました。drag の間、root の graph の node は今描かれている場所をセッションに固定します。root の文書には書きません([`GraphCanvasScene::holdRootPlaces()`](../../src/devstudio/view/graphcanvaswidget.cpp#L711))。
  - test: `devstudio_graphcanvas_scene_test` の [「UX-1 a zone dragged by its heading ends where the hand let go, and stays there」](../../test/devstudio_graphcanvas_scene_test.cpp#L7473) と [「UX-1 a body node is dragged inside its zone, and the zone stays where it is」](../../test/devstudio_graphcanvas_scene_test.cpp#L7511)。
- **三角**: 見出しの左の三角(tooltip `Fold the zone into the loop's node`)で、枠を LP-4c のループの node(ソケットつき)に畳みます。畳んだ node の三角(tooltip `Open the loop's zone`)で枠に戻ります。ループには見出しだけに畳む形はありません。どちらの形かはセッションの状態で、文書には書きません。
- **共有**: 同じ definition を 2 つのループが body にしていると、枠に描くのは文書で先のループだけで、もう一方は LP-4c のループの node で描かれます。枠の見出しには `shared (N users)` が出ます。
- **ダブルクリック**: 見出しは改名です。枠の中の node の無い所をダブルクリックすると、今までどおり body の scope に入ります(下の「body に入る」)。body の node のダブルクリックでは入りません([`GraphCanvasScene::mouseDoubleClickEvent()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4398))。
- **Undo**: Graph タブの履歴のままです(下の「Undo」)。枠の中の編集は definition の scope の op、枠の移動は root の `set_layout` で、それぞれ 1 項目として戻ります。scope の違う `set_layout` が履歴に混ざっても、文書は byte 一致で戻ります(scene の test)。

**縁をまたぐ線・carry・展開の表示(✅LP-4d2)**: 枠の縁をまたいで線を引くと、ループの入力と出力が足されます。body の出力から body の入力へ戻る線は carry になり、枠の下側を回る矢印で描かれます。engine の報告が届くと、ループが何になったか(回の数か material の数)が枠の下端に出ます。どの操作も 1 回の編集で、Undo 1 回で戻ります。

- **外から入力を足す**: 編集できる文書では、左の縁の行の最後に空いた口 `+` があります(tooltip `Drop a line from outside here for a new loop input`)。root の node の出力をここへ落とすと、ループの新しい入力を足す列を 1 回で送ります([列を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L4568)、[落とし先を見分ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L3042))。
  - 列の中身は、ループの分岐が走る definition への `add_node graph.input`(同じ definition には 1 回だけ、分岐の順。既定値は付けません)と、root の `set_input`(ループの新しい入力 ← 落とした出力)です。今のタブの body だけでなく、全部の body に足します(分岐の body の入出力がそろっていないと engine が `graph.loop_branch_interface@1` で断るため)。
  - 枠の中の body の node の入力に直接落としても、同じ列を送ります。列の最後に、その definition の `set_input`(その入力 ← 新しい `graph.input` の `value`)が付きます。
  - ✅Q-19 から、落とした先が body の render node の宣言のソケットなら、列の最後は番号付きの port を足す `set_node_parameters` とその `set_input` です(下の「render node の入力は shader の宣言から」)。
  - 入力の名前は、落とした出力の port 名から Studio が提案します。`.` と `/` は `_` になり、末尾の `[]` は外します(空なら `input`)。ループの入力か body の node の id と重なれば `_2`、`_3` … です([`GraphCanvasModel::suggestLoopInputName()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4521))。打つものはありません。
  - 引けるのは、canvas が root を見ているときの、root の node の出力からだけです。ループ自身の出力からは引けません(循環になるため)。
  - ループが既に持つ入力の外側のソケットに落とすと、今までどおり root の `set_input` の差し替えだけです。
- **中から出力を足す**: 右の縁の行の最後にも `+` の口があります(tooltip `Drop a body node's output here for a new loop output`)。枠の中の body の node の出力をここへ落とすと、次の列を 1 回で送ります([列を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L4614))。
  - body ごとに 1 つの `set_outputs`(✅DO-1。上の op の表)。その body の `outputs` に新しい出力を足したものを丸ごと置きます。
  - 最後に、ループの `outputs` に `p` を足した root の `replace_node`。`p[]` はここでは足しません(Loop の欄の Outputs の check で足します)。
  - 出力の名前は port 名から入力と同じ規則で提案します(空なら `output`)。ループが宣言した `p` / `p[]` か、どれかの body の出力と重なれば `_2` … です。
  - 他の分岐の body は、同じ id の node の同じ port からその出力を出します(body を写したときの配線です)。`set_outputs` の endpoint は node と port の両方が要り、つながっていない出力は足せないためです。
  - 他の分岐の body にその node か port が無ければ、何も送らず、Graph タブの notice に 1 行で断ります([`GraphCanvasModel::addZoneOutput()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4659))。例: `Branch 1 of loop steps runs rest, which has no sum.value to give value from: add the output there first.`
- **carry の矢印**: 右の縁の内側の端(body の出力。tooltip `The body's output <出力> (drag to a boundary input to carry it)`)から、左の縁の内側の端(body の `graph.input`)へ線を引くと、ループの `carry` に `{from, to}` を 1 組足します。送るのは Loop の欄の Carry と同じで、parameters を丸ごと置く `set_node_parameters` 1 回です([`GraphCanvasModel::addLoopCarry()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4671))。同じ組が既にあれば何も送りません。
  - 矢印は、出力の端から右へ出て枠の下側の通り道を戻り、入力の端へ左から入ります([矢印を引く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1230))。通り道は組ごとに分かれ(下端の展開の行の上)、その分だけ枠が高くなります。
  - 矢印を押すと選ばれて太くなり、Delete でその組を `carry` から外します(これも `set_node_parameters` 1 回です。[`GraphCanvasModel::removeLoopCarry()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4684))。tooltip は `<from> carries into <to> (select and press Delete to remove)` です。
  - `to` の入力にループの入力の edge が無い組は、矢印が橙色になり、`!` の印が付きます。tooltip は、Loop の欄の Carry の赤い字と同じ `<to> has no loop input: connect one for the first element to start from.` です。
- **回ごとの入力の線**: source に `element` を持つ edge(下の「回ごとの入力 —— Per iteration」で張った入力)は破線で描かれ、線の中ほどに `[<element>]` のラベルが乗ります([線を描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L623))。
  - ラベルの字は、式(`{"$expr": …}`)ならその式、文字列ならそのまま、それ以外は JSON です。tooltip は `Each element reads element <element> of <node>.<port>` です。
  - 線を選んでも破線のままです。ラベルは線に付いて動きます。
- **展開の表示**: engine の `loops[]` の報告が届いたループでは、枠の下端に 1 行が出ます([出す所](../../src/devstudio/view/graphcanvaswidget.cpp#L1946))。報告の前は出ません。
  - expand のループ: `×N copies` と **Show in Frame Plan** のボタンです。押すと Frame Plan のタブが前に出て、frame plan を読み直し、届いたらそのループのグループを開いて、回の node を全部選んで中央に寄せます([グループを開いて選ぶ所](../../src/devstudio/view/frameplangraphics.cpp#L3753))。Frame Plan の status 行は `Loop <loop>: its copies are picked.` です。plan にそのループの回が無ければ `The frame plan has no copies of loop <loop>.` です。✅UX-12b から、この行は、回が選ばれたままの間(と次の編集を送るまで)残ります([選んだループの行を戻す所](../../src/devstudio/view/frameplanwidget.cpp#L446))。UX-12b の前は、plan の読みに続く authoring context と task の一覧の返事が Frame Plan を作り直すたびに、ふつうの status 行(`main_render · whole frame · …`)が数 ms で上書きしていました(commit の記録。シナリオ `loop_zone_wire` がこの行を 120 秒待って落ちていました)。
  - draw_loop: `N materials`(1 つなら `1 material`)です。draw_loop は回を複製しないので、ボタンはありません。分岐のタブの字の後には、その分岐に当たった material の数が付きます(`if m#a (1)` / `else (2)`)。
- **Frame Plan からの Open in Graph**: 回の node の **Open in Graph** は、body の scope に入らず、root の canvas のまま、ゾーンのタブをその要素の分岐に合わせ、枠の中のその node を選んで中央に寄せます。畳んだゾーンは開きます([`GraphCanvasWidget::revealLoopOrigin()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8963))。body がゾーンに描かれないとき(同じ definition を先のループのゾーンが描いている)は、LP-4c のとおり body の scope に入ります。
- **枠の追従**: body の node を drag している間も、枠がその node を囲むように広がり、縁のソケット・見出しの帯・下端の行が付いて動きます。ループへ出入りする root の線、右の縁への破線、carry の矢印も引き直します([`GraphCanvasScene::refitZoneLive()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2493))。離すと、今までどおり枠を描き直します。
- **値の帯の切り詰め**: node の値の帯(pill)は、node の幅に収まるように末尾を `…` で切り、全文を tooltip に出します([帯を切る所](../../src/devstudio/view/graphcanvasdrawing.cpp#L544))。render 系に限らず全部の node に効きます。LP-4d2 ではゾーンの中の node だけで、root の node の長い値(image の type の JSON など)は node の左へ大きくはみ出していました。`9ee2d54b` から root でも切ります。
- **Undo**: 上の 3 つの操作(入力・出力・carry)は、それぞれ 1 項目として戻ります。`set_outputs` の逆は、前の `outputs`(無ければ空)を持つ同じ op です(下の「Undo」)。入力・出力・carry を足してから Undo 3 回と Redo 3 回をすると、文書は byte 一致で戻ります(scene の test)。

**render node の入力は shader の宣言から —— 線で番号付きの port を作る(✅Q-19)**: Graph タブの `render.fullscreen` / `render.raster` の node は、engine がその pass の shader の宣言を出していれば、入力のソケットを Frame Plan と同じ形で描きます(✅VI-3a。下の「ソケットは shader の宣言から出ます」)。ソケットへ線を落とすと、Graph タブが engine の番号付きの入力の port(✅LP-1d。`"binding": "input"` と `input_index`。[第6章 §6.18](06_rendering.md) の「番号付きの入力」)を書いてつなぎます。Q-19 の前は、render node に別の画像を読ませるには、parameters の JSON(pill)に port を打つしかありませんでした。

- **宣言の出所**: Graph タブが読む `get_frame_plan` の node の `pass_inputs` です([読む所](../../src/devstudio/model/graphcanvasmodel.cpp#L5083))。Graph タブの node と plan の pass は次のように対応づけます([`GraphCanvasModel::passInputs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5113)):
  - root の node: 名前がその id の pass
  - ループの body の node: plan の順で最初の、`loop_origin` の `node` がその id で、その要素の分岐がこの body を走らせる回の pass(どの回も同じ shader で描くため、1 つの回の宣言を使います)
  - instance の definition の node: `<instance>/<id>` の pass
- **形**: 固定の宣言は ○ 1 つずつで、字は宣言の変数名です(つながっていれば `<変数名> ← <node>.<port>`)。可変の組は、つながっている ○(字は `← <node>.<port>`)と下端の空の落とし先 `+ input` を半透明の帯が囲みます([Graph の render node のソケットを宣言から組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L376)、[帯を描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L510))。宣言のソケットは binding の順で、node のほかの行(書く port など)より上に並びます。buffer の宣言も ○ で描きます。
  - 番号付きの port はソケットのものになり、port の行としては出ません。宣言が取らない番号付きの port は、port 名の ○(`<port> ← <node>.<port>`)で残ります。線の行き先が無くならないようにです。
- **宣言が無い node は今の行のまま**です。plan にその pass が無い、`pass_inputs` が `declared` でない(pipeline がまだ無い、など)、宣言が 1 つも無い、Studio が読めない宣言のどれでも、Q-19 の前と同じ port の行で描きます([分ける所](../../src/devstudio/model/graphcanvasmodel.cpp#L2122))。
- **plan を読む時**: 上の「ループの node」の読みに加えて、root の instance の definition に render node があるときも読みます。✅GL-1 からは条件が無く、文書を読むたびに読みます([`GraphCanvasModel::requestLoopReports()`](../../src/devstudio/model/graphcanvasmodel.cpp#L830))。足した直後の pass は、engine がそれを持つ世代を出すまで plan に載りません。そのため、render node の pass が plan に無い(か宣言が `declared` でない)間は、文書を読むたびに最大 40 回まで、時計で plan を読み直します([読み直す所](../../src/devstudio/model/graphcanvasmodel.cpp#L992))。回数 0 と報告されたループの body は待ちません。

線を落とした先で起きることは次のとおりです。どれも 1 回の編集で、Undo 1 回で戻ります([列を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L5293))。入力のソケットから出力へ逆向きに引いても同じです。

| 落とした先 | 送るもの |
|---|---|
| 空の ○ か `+ input` で、binding が次の番号(0 から続く番号付きの port の数) | `set_node_parameters`(`ports` に `{"kind": "image", "type": …, "access": "read", "binding": "input", "input_index": <b>}` を足す)と `set_input`(その port ← 落とした出力) |
| つながっている ○(宣言が取らない port の ○ も) | その port の `set_input` の差し替え。source の型が今の `type` と違えば、先に `type` を直す `set_node_parameters` も送ります。同じ線なら何も送りません |

- **type**: engine は image の port に `type` を要るので、落とした出力から辿ります([`GraphCanvasModel::imageTypeOf()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5235))。pass の port の `type`、image などの `parameters.type`、body の `graph.input` ならそのループか instance に入っている線の source、instance やループの出力なら body の出力の source、の順です。分からなければ、Add fullscreen pass の image と同じ scene color の型(`pelican.render.color_signal@1`、`reference: scene`・`transfer: linear`・`range: extended`)です。
- **port の名前**: source が body の `graph.input` ならその id、それ以外は source の port 名から、ループの入力と同じ規則(`.` と `/` は `_`、末尾の `[]` は外す、空なら `input`)で提案します。node の ports か inputs と重なれば `_2`、`_3` … です([`GraphCanvasModel::suggestInputPortName()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5214))。打つものはありません。
- **外す**: 番号付きの port の線を選んで Delete するか、つながっている ○ を空所へ drag すると、`set_input{source: null}` と、port を消して後ろの port の `input_index` を 1 つずつ詰めた `set_node_parameters` を 1 回で送ります([`GraphCanvasModel::removeInputPortOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5437))。
- **断るもの**(送らず、notice に 1 行):
  - 飛ばし: `Connect binding <n>..<b-1> of <node> first: a pass's inputs fill the bindings in order.`(飛ばしたのが 1 つなら `Connect binding <n> of ...`)。可変の組の前の固定の宣言が埋まっていないときの `+ input` も同じです
  - 可変の組が max まで埋まっている: `<node>'s shader takes at most <max> inputs in its variadic group.`
  - buffer の宣言: ✅UX-8 から断らず、名前付きの buffer の port を作ってつなぎます(下の「曲線を shader の値へ GUI でつなぐ」)。UX-8 の前は `Binding <b> of <node> takes a buffer: a graph pass reads only images by number.` で断っていました
  - 宣言に無い binding: `<node>'s shader declares no binding <b>.`
- **ゾーンの外から body のソケットへ**: root の node の出力を、枠の中の body の render node の宣言のソケットへ直接落とすと、上の「外から入力を足す」の列(`add_node graph.input` と root の `set_input`)の後に、その `graph.input` の `value` を読む番号付きの port の `set_node_parameters` と `set_input` を付けて、1 回で送ります([`GraphCanvasModel::addZoneInputToSocket()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5458))。port の名前は新しい `graph.input` の id、型は root の source から辿ります。飛ばしなどで断るときは、ループの入力も足しません。
- **carry で前の回の出力を読む**: 線だけで組めます。(1) root の pass の出力を左の縁の `+` へ(ループの入力)、(2) body の pass の出力を右の縁の `+` へ(ループの出力)、(3) 右の縁の内側の端を左の縁の内側の端へ(carry)、(4) 左の縁の内側の端(body の `graph.input`)を body の pass の宣言のソケットへ落とします。(4) で pass がその `graph.input` を番号付きの入力として読み、展開された 2 回目の pass が 1 回目の出力を読みます(frame plan の `loop/1/render_fullscreen` の `reads` に `loop/0/render_fullscreen__color` が入ります。シナリオ `loop_zone_read`)。
  - carry の矢印は左の縁の内側のソケットで終わり、その上に描かれます。Q-19 から、矢印の下のソケットを押すと、carry を選ぶのではなく線を引き始めます([押した所](../../src/devstudio/view/graphcanvaswidget.cpp#L3176))。Q-19 の前は、carry の付いた入力からは線を引けませんでした(実装者の報告)。
- **分かっている穴**(Q-19 の着地時の記録。実装者の報告):
  - ✅LP-6(`03725036`)で塞がりました。root の `render.image` から引いた線も通ります(engine の規則: image node から読む pass は、その image を書く graph の pass を待つ。待たない場合も含めて [第6章 §6.18](06_rendering.md) の「image node を読む pass の順」と [design_graph_loops.md](../design_graph_loops.md) §5.2)。Q-19 の着地時は、root の `render.image` の出力を body の pass のソケットへ引くと、engine が `Pass input target is not produced as an earlier output: <image> in pass: <pass>` で断り([その文を投げる所](../../src/core/renderingpass/frameplanner.cpp#L1929))、シナリオのように root の pass の出力 port から引いて避けていました。
  - **LP-6 が入るまでの一時的な案内(Q-20、Q-21 で外した)**: Q-20 から Q-21 までの間、Graph タブは上の拒否を受けると、notice の拒否の行の後ろに「その image を書く pass から線を引く」案内を足していました(`render.image` から読む線を持つ編集のときだけ。engine はこの拒否に専用の code を付けないので、判定は message の文字列でした)。LP-6(`03725036`)で engine が直ったので、Q-21(`5b4b3737`)で案内とその判定を外し、シナリオ `loop_zone_read` に root の `render.image` から body の pass へ引く線を足しました。
  - シナリオ `loop_zone_read` は、frame plan の回の pass を位置(`nodes[2]` / `nodes[3]`)で見ています(同じ位置の `name` が合うことも見ます)。
  - シナリオで `Graph change saved.` を待った直後に発火する action は、文書の読み直しの途中で disabled のことがありえます(`loop_zone_wire` も同じ手順です)。

自動化面の名前(表の正は [README](../../test/studio_scenarios/README.md)。[definition を前に付ける所](../../src/devstudio/automation/automationcommands.cpp#L315)):

| 名前 | 指すもの |
|---|---|
| `graph_node#<loop>` | 枠そのもの(ループ)。`click` は枠の中央ではなく見出しに当たります([当たる所](../../src/devstudio/view/graphcanvasdrawing.cpp#L874)。中央には body の node があるため) |
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
- ✅LP-4d2: 同じ名前の入力と出力を持つ node(fullscreen pass の `color` など)のソケットは、`graph_socket#<id>/<port>` では分けられません。そのため `graph_socket#<id>/in:<port>` / `graph_socket#<id>/out:<port>` の別名でも引けます(ゾーンの中は `graph_socket#<definition>/<id>/in:<port>` …)。別名は名前を解決するときだけ効き、`list` には出ません([別名を作る所](../../src/devstudio/automation/automationcommands.cpp#L334))。
  - ✅Q-19 から、宣言のソケットの `in:<port>` は、そこにつながっている番号付きの port の名前です(宣言のソケット自身の名前は `in@<binding>` なので)。

**分かっている穴**(LP-4d1 と LP-4d2 の着地時の記録):

- 見出しに当たる `click` の位置(hotspot)は、シナリオでは確かめていません(scene の test の変異で守っています)。
- carry の効き目は Frame Plan では見分けられません。シナリオの body の pass は書くだけで、前の回の値を読む port を持たないためです。読む port は、今の Graph タブでは JSON を打たないと作れません(実装者の報告)。✅Q-19 で塞がりました。線で番号付きの port を作れるようになり、シナリオ `loop_zone_read` が 2 回目の pass の `reads` で carry の効き目を見ます(上の「render node の入力は shader の宣言から」)。
- 新しいループが既存の node に重なることがありました。選んでいる node の横に、node の幅を 240 と見て置いていたためです(`GraphCanvasScene::placeNewNode()`。実装者の報告)。✅LP-4f1 で塞がりました(下の「新しい node の置き場所」)。
- draw_loop の要素から Open in Graph する道はありません。draw_loop は展開されないので、`loop_origin` を持つ回の node が Frame Plan に無いためです(実装者の報告)。
- `in:` / `out:` の別名には単体の test がありません(シナリオ `loop_zone_wire` が使っています)。
- LP-4d1 の着地時にあった 3 つの穴(drag の間に枠が追いつかない、ゾーンの中の長い値の帯が node からはみ出す、Frame Plan の Open in Graph が body の scope に入る)は、✅LP-4d2 で塞がりました(上の「縁をまたぐ線・carry・展開の表示」)。

**Loop の欄(✅LP-4c)**: root のループを選ぶと、詳細表の上に Loop の欄(`pelican.graphCanvasLoop`)が出ます([`GraphCanvasLoopFields::rebuild()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L552))。
1 行目は要約(`loop loop · expand ×3 · body loop_body`。報告の前は `not reported yet`)と **Open body** です。
欄を 1 つ変えるたびに、ループの parameters を 1 鍵だけ変えて丸ごと `set_node_parameters` 1 回で送ります(Graph タブの Undo で 1 つずつ戻ります)。

| 欄 | 中身 |
|---|---|
| Over | ✅LP-4f1。`Count`(`over.range`)/ `View axis`(`over.view_axis`)。下の「view の軸で回す」。✅GL-1d で `Draw items`(`over.draw_items`)。ループの `draws` を読む root の material pass があるときだけ選べます。無ければ項目は押せず、tooltip が `No material pass reads this loop's draws: connect its draws to a material pass first.` を言います([項目を出し分ける所](../../src/devstudio/view/graphcanvasloopfields.cpp#L617))。選ぶと、その pass の契約で `over: {draw_items: {contract}}` を書き、`tier: "draw_loop"`・`order: {by: "phase_default"}` にして、draw_loop が受けない `limit`・`carry`・`where`・`insert`・`canonical_anchor` を消して送ります([`GraphCanvasModel::setLoopDrawItems()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4040))。draw_loop になると Over の行は出ません(下の「draw_loop の Loop の欄と分岐の when」)。test: `devstudio_graphcanvas_test` の [「GL-1d a loop a material pass reads can run over its draw items」](../../test/devstudio_graphcanvas_test.cpp#L2051) |
| Count | 回数の spin(0〜4096。Enter か focus を外すと送る)。右の **ƒ** で式の欄に替わり、`{"$expr": …}` を送ります(式の欄に整数だけを打つと整数として送ります)。✅LP-4f1 から、式の欄の下に target の combo(frame plan の render target)と **mip_levels_of** / **layers_of** のボタンが出ます。押すと `mip_levels_of("<target>")` などを、欄が空か整数ならそれに置き換え、式ならカーソルの位置に挿して送ります([`insertCountFunction()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1085))。Over が View axis のときは出ません |
| View | ✅LP-4f1。Over が View axis のときだけ。family と軸の combo、回数 `×6 (face)`(読むだけ) |
| Order | `ascending` / `descending`。descending は `order: {by: element, direction: descending}` を書き、ascending は `order` を消します |
| Limit | **first** の check と spin(`limit`。並べた後の先頭 N 個) |
| Stage | `automatic`(`tier` を消す)/ `expand` / `draw_loop` |
| Place | 展開した回が pass の列のどこに入るか。`end of its pass family` か `after <anchor>`(`sprite`・`post_main`・`tonemap`・`post_ldr`・`pelican_ui`・`debug_draw`・`debug_text`・`imgui`)。選ぶと `insert` を消して `canonical_anchor` を書きます。`insert` を持つループは `insert <…>` の項目で今の値を見せます。body に fullscreen / raster の pass が無いループでは押せません |
| Where | `every element` / `element in` / `element not in` と整数の並び(`1, 4`。Enter で送る)。整数でない片があれば送らず、tooltip が `Integers, separated by commas` になります。✅LP-4f1 から、回数が分かっていて 1〜64 のとき(整数の Count か、view の軸の長さ)は、並びの欄の代わりに要素ごとの check が出ます(view の軸なら `2 (+Y)` の字。[check を組む所](../../src/devstudio/view/graphcanvasloopfields.cpp#L1131))。check を付け外すたびに送ります。1 つも付いていない間は送りません |
| Outputs | body の出力ごとに `<名前>` と `<名前>[]` の check。変えるとループの node を丸ごと送り直します(`replace_node`) |
| Carry | 行ごとに「body の出力 → body の入力」の 2 つの combo と **Remove**、下に **Add carry**。to の入力にループの入力の edge が無い行には、赤で `<入力> has no loop input: connect one for the first element to start from.` が出ます(engine が断る形です) |

Outputs の区は body に出力があるときだけ、Carry の区は body に入力と出力の両方があるときだけ出ます。

**view の軸で回す —— Over と View(✅LP-4f1)**: cube capture の 6 面や影の cascade のように、1 つの view family が持つ view を 1 つずつ回すループを、JSON を打たずに組めます(engine の形は `over: {view_axis: {family, axis}}`。[第6章 §6.18](06_rendering.md))。

- **Over**(`pelican.graphCanvasLoop.over`): `Count` / `View axis`。View axis を選ぶと、frame plan の最初の選べる軸で `over.view_axis` を送ります([`GraphCanvasLoopFields::commitOver()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L999))。Count に戻すと、軸の長さ(分からなければ 1)を回数にした `over.range` を送るので、回の数は変わりません。
- **View の行**: family の combo(`viewFamily`)と軸の combo(`viewAxis`)、右に回数 `×6 (face)`(`viewCount`。読むだけ)です。family か軸を選ぶたびに `over` を丸ごと置き換えて送ります。ほかの parameters は残ります([`GraphCanvasModel::setLoopViewAxis()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4015))。
  - 候補は、Graph タブが読んだ frame plan の node の `view_axes` を family ごとに集めたものです(軸を持つ family だけ。`$main`・planar の反射・作者の family は出ません。[集める所](../../src/devstudio/model/graphcanvasmodel.cpp#L3892))。どの pass も走らない family は候補に出ず、欄の下に `Families no pass of the frame plan runs in are not listed.` が出ます。
  - 軸の項目は `face ×6` の形です。実行時に決まる軸(directional の影の `light`)は `light (decided each frame)` と出て選べません(tooltip `The engine decides this axis each frame: a loop cannot run over it.`)。
  - 文書が名指す family / 軸が plan に無ければ、`(not in the frame plan)` を付けて残します。
  - 選べる軸を持つ family が 1 つも無いときに View axis を選ぶと、何も送らずに Over を Count に戻し、欄の下に `No pass of the frame plan runs in a view family with an axis a loop can run over.` が出ます。
- view の軸のループでは、Count の行と挿入のボタンは出ません。Where と分岐表の check は軸のラベル付き(`2 (+Y)`)です。

**body の render node の View 欄 —— Each element(✅LP-4f1)**: Graph タブで render node を選ぶと、詳細表の上に LP-4b と同じ **View** の欄(`pelican.graphCanvasRender`。軸の行は `pelican.graphCanvasRender.view.<axis>`)が出ます([`GraphCanvasViewFields`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1793))。

- 対象の op は `render.fullscreen` / `render.raster` / `render.compute` / `render.material_pass` / `render.shadow_depth` / `render.snapshot_copy` です([`GraphCanvasModel::takesViewSelect()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4009))。
- どの family の軸を出すか([`GraphCanvasModel::viewField()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4055)):
  - view の軸で回すループの body の node は、そのループの family(node が別の `view_family` を名指していれば対象外)。見出しは `View (<family>) · in loop <loop>` です。
  - root の node は、frame plan のその node の family。見出しは `View (<family>)` です。
  - どちらでもなければ node 自身の `view_family`(frame plan がその family の軸を出しているとき)。
  - 軸が取れなければ欄は出ません。
- 行の中身は LP-4b の欄と同じ(`All views` と値、実行時の軸は字だけ)です。ループの body の node では、ループの軸の combo に **Each element (loop)** が足されます。選ぶと、その回の element の view で走る `{"$expr": "element"}` を書きます。ほかの軸・ループの外の node には出ません。
- 選ぶたびに `set_node_parameters` を 1 回送ります([`GraphCanvasModel::setViewSelect()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4110))。
  - 欄が出さない値(`element` 以外の式)はそのまま残します。どの軸も All views なら `view_select` を消します。
  - node が `view_family` を書いていなければ、family も一緒に書きます(書かない pass は frame の最初の family で走るため)。
- LP-4f1 の前は、Graph タブの render node の `view_select` は parameters の JSON を打つしかありませんでした。

**Loop の欄は編集の往復の間は押せません(✅Q-17)**: Loop の欄の form と行は 1 つの widget(`pelican.graphCanvasLoop.fields`)にまとまっていて、編集を送ってから文書を読み直すまでの間(model が次の編集を受けられない間)は全体が無効になり、読み直すと一度だけ有効に戻ります([`GraphCanvasLoopFields::followEditable()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L515))。

- 親を無効にするだけなので、欄ごとの理由で無効な欄(Place など)は、戻っても無効のままです。
- それでも model が断った分岐の選択(body・条件・draw_loop の条件の combo)は、文書の値に戻ります([`restoreBranch()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1368))。欄が送っていない値を見せたままにはなりません。
- Q-17 の前は、欄を作り直すのが文書・報告・配置が変わったときだけだったので、往復の間も欄は有効に見え、そこで選んだ分岐の body は model に黙って捨てられていました(シナリオ `material_loop_decompose` の間欠の赤。実装者の報告)。
- Q-17 の着地時は、material pass と draw step の欄(`pelican.graphCanvasDraw`)と上の View 欄に同じ問題が残っていました。✅Q-18 で同じ形になりました(次の段落)。

**Draw 欄と View 欄も往復の間は押せません(✅Q-18)**: material pass と draw step の欄(`pelican.graphCanvasDraw`)と render node の View 欄(`pelican.graphCanvasRender`)も、Loop の欄と同じく、編集を送ってから文書を読み直すまでの間は無効になり、読み直すと有効に戻ります([`GraphCanvasDrawFields::followEditable()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1669)、[`GraphCanvasViewFields::followEditable()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1854))。Graph タブは model が動くたびに、Loop の欄と一緒にこの 2 つを呼びます。

- model が断った選択(Contract・Variant・Cull・Depth test・Depth write・Blend)は、文書の値に戻ります([`GraphCanvasDrawFields::restore()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1680))。**Defines** に打ちかけた文字は戻しません(文字なので、Apply defines でもう一度送れます)。
- View 欄で断られた選択は、行を作り直さずに、今使った combo のまま文書の値に戻ります([`ViewSelectField::showSelection()`](../../src/devstudio/view/viewselectfield.cpp#L162))。automation の `select` が当たった combo が、そのまま今の値を読めるようにするためです。
- Q-18 の前は、この 2 つの欄は往復の間も有効に見え、そこで選んだ値は model に黙って捨てられていました。
- ⚠ 分かっている穴(Q-18 の着地時の記録): `draw.override` の欄を全部 `(material's)` にして Defines も空のときは、何も送らずに Cull の tooltip に理由(`An override replaces at least one render state or adds a define; …`)を出すだけで、欄は文書の値に戻りません。

**新しい node の置き場所(✅LP-4f1)**: palette・Add loop・Add fullscreen pass で足した node は、選んでいる node(ゾーンなら枠)の描かれた箱の右端の隣に置かれ、同じ scope の描かれた箱と重なる間は下へずれます([`GraphCanvasScene::placeNewNode()`](../../src/devstudio/view/graphcanvaswidget.cpp#L3917))。

- 新しいものの大きさは描くまで分からないので、ループなら枠の最小の大きさ、ほかは数行の node と見ます。
- 選んでいる node が無いときは右クリックした所が起点です。ゾーンの body に、メニューからでも選んだ node の横でもなく足した node は、今までどおり枠の中で自動で並びます。
- 自動配置で描かれていた node は、このとき今の位置でセッションに固定されます(新しい node が自動配置の列を取って、ほかの node を押し出さないように)。位置はどれもセッションの状態で、文書には書きません。
- LP-4f1 の前は、選んでいる node の横に node の幅を 240 と見て置いていたので、幅の広いゾーンの横に置くと重なりました。
- ✅UX-8 から、1 回の追加で主の node と一緒に来る node(scope の `scene_time`、必須の入力の種の定数、compute の書く image など)にも位置を与えます。主の node の左に縦に並べ、同じ scope の描かれた箱と重なる間は下へずらします([`GraphCanvasScene::placeCompanions()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4048))。UX-8 の前はこれらの node は配置を持たず、canvas の初期配置で描かれていました(仕様の決定 8 は「Auto layout を押さずに済む」ことを求めました)。

**分岐表(✅LP-4c)**: Loop の欄の下の **Branches** が分岐の表です([分岐表を組む所](../../src/devstudio/view/graphcanvasloopfields.cpp#L812))。

- 1 行が 1 分岐です。条件の combo は `element =` / `element in` / `element not in`、右が整数の欄(Enter で送る)です。
  - `element =` は欄の最初の 1 つだけを使い、空なら 0 です。
  - 文書の条件が式なら `(expression)` と出て、値の欄は押せません。
  - ✅LP-4f1 から、`element =` の値は spin です(回数が分かっていれば 0〜回数−1。値を変えると送る)。`element in` / `not in` は、Where と同じく回数が 1〜64 なら要素ごとの check で、それ以外(回数が式か 64 を超える)は今までの整数の欄です([行を組む所](../../src/devstudio/view/graphcanvasloopfields.cpp#L846))。
- body の combo は `(nothing)`、候補の definition、**New definition…** です。
  - `(nothing)` は `"body": null` で、当たった要素では何もしません。
  - 候補は入出力がループと同じ definition です(ループにまだ body が無ければ全部)。
  - **New definition…** は名前を `<ループの id>_body` から提案し(使われていれば `_2` …)、ループの入出力を写した definition を `add_definition` で作ってその分岐に付けます(同じ列で 1 回)。写すのは body の `graph.input` と出力ですが、出力を `graph.input` 以外の node が作る body では本体ごと写します。
- ↑ / ↓ で並べ替え、× で消します。最後の行は `else` で、動かせず、消せず、条件を持ちません。
- **Add branch** は else の前に 1 行足します。条件は `element = 0` です。
  - ✅UX-1 から、新しい分岐の body は else の body を写した新しい definition です。名前は Studio が `<ループの id>_body` から提案し(使われていれば `_2` …)、`add_definition` と分岐を足す `set_node_parameters` を 1 つの列で送ります。Undo 1 回で両方が戻ります([写す所](../../src/devstudio/model/graphcanvasmodel.cpp#L4236))。else に body が無い(か、文書が定義していない)ときは写さず、今までどおり分岐だけを足します。
  - UX-1 の前は body が else と同じ definition だったので、新しい分岐のタブを押しても同じ body が出て、そこを直すと else も変わりました(利用者の指摘「else タブは追加されても開いて編集できないのは仕様?」)。
  - test: `devstudio_graphcanvas_scene_test` の [「UX-1 Add branch gives the branch a body of its own, and the chosen tab is plain」](../../test/devstudio_graphcanvas_scene_test.cpp#L7752)。
- 各行の下に、engine の報告(`loops[]` の `elements` の `branch`)から当たった要素が `elements: 0, 2` の形で出ます。
  - どの要素も当たらない行(`unmatched_branches`)は、赤で `no element takes this branch` です。
  - 報告の前は `elements: not reported yet` です。

**body に入る(✅LP-4c)**: ループをダブルクリックするか、Loop の欄の **Open body** で、選んでいる分岐の body の scope に入ります。✅LP-4d1 からの既定の見え方はゾーン(上)で、この「入る」道も残っています。ゾーンでは、枠の中の node の無い所のダブルクリックで入ります(見出しのダブルクリックは改名)。

- パンくずは instance と同じです(`root / loop_body`)。既定の分岐は、先頭から見て最初に body を持つ分岐です。
- 中にいる間は、パンくずの右に分岐の combo(`pelican.graphCanvasLoopBranch`)が出ます([分岐の combo を合わせる所](../../src/devstudio/view/graphcanvaswidget.cpp#L7032))。
  - 項目は `loop: element = 0 → loop_body` の形で、body を持つ分岐だけです。
  - 選ぶと、その分岐の body に切り替わります。
- 同じ definition を root の instance やループの分岐が 2 か所以上で使っていると、その右に橙色で `shared (N users)` が出ます(`pelican.graphCanvasShared`)。ここで直すと、使っている所が全部変わります。

**回ごとの入力 —— Per iteration(✅LP-4c)**: root のループの入力ソケット(か行の字)を右クリックすると、`p[]` を宣言した**他の**ループがあるときだけ **Per iteration…** が出ます。

- dialog(`pelican.graphCanvasPerIteration`)で、読む `p[]`(`<ループ>.<出力>[]`)と Element の式を選びます。式の既定は `element`(受け手のループの要素)です。
  - ✅LP-4f1 から、Element は `element` / `element - 1` / `element + 1` / **Custom…** の combo です。Custom… のときだけ式の欄(`pelican.graphCanvasPerIteration.element.custom`)が出て、空なら `element` です([`GraphCanvasElementChoice`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1752))。LP-4f1 の前は式を打つ欄でした。
- OK で、`set_input` の source に `element: {"$expr": <式>}` を付けて 1 回送ります([`GraphCanvasModel::connectPerIteration()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4461))。
- 張った入力の要約には `[element {"$expr":"element"}]` が付きます。
- ✅LP-4d2 から、canvas ではその線が破線で描かれ、線の中ほどに `[element]` のラベルが乗ります(上の「縁をまたぐ線・carry・展開の表示」)。

**Undo(✅LP-4c)**: Graph タブの Undo は、1 つの編集の op の列を、前の op を当てた後の文書で 1 つずつ逆にします([`GraphCanvasModel::inverseOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2535))。

- 同じ列で足した node を後の op が指していても戻せます(Add loop、Add fullscreen pass、New definition…)。
- definition の op も戻せます。`add_definition` の逆は `remove_definition`、`rename_definition` の逆は名前を戻す rename、`remove_definition` の逆は本体つきの `add_definition` です。
- ✅LP-4d2 から、`set_outputs` も戻せます。逆は、前の `outputs`(無ければ空)を持つ同じ op です([逆にする所](../../src/devstudio/model/graphcanvasmodel.cpp#L2626))。canvas が送るのは definition の `set_outputs` だけです。

**definition の改名と削除(✅Q-19)**: Graph タブから definition の名前を変え、どこからも使われていない definition を消せます。送るのは engine の `rename_definition` / `remove_definition`(✅LP-0c。上の「`ops[]` と `scope`」の表)の 1 op で、どちらも Undo 1 回で戻ります(上の「Undo」の逆の op)。Q-19 の前は、Studio にこの 2 つの入口がありませんでした。

- **入口**は右クリックの **Rename definition…**(`pelican.graphCanvas.renameDefinition`)/ **Delete definition**(`pelican.graphCanvas.deleteDefinition`)で、次の 3 か所に出ます。
  - パンくず: canvas の中のパンくずと上段のパンくずの字。今見ている definition が対象です([canvas のメニューに足す所](../../src/devstudio/view/graphcanvaswidget.cpp#L3451))。
  - ゾーンの見出し: そのタブが見せている body の definition が対象です。
  - 上段の `Scope:` の combo: 選んでいる definition が対象です(`root` では出ません)。
  - メニューを通さず名前で発火したときは、今見ている definition、root ならゾーン(か枠の中の node)を選んでいればその body の definition が対象です。対象が無いか編集できない間は押せません([対象を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L6673))。
- **Rename**: 上段の下に 1 行(`pelican.graphCanvasDefinitionRow`)が開きます。`Rename definition <name> to`、名前の欄(提案の名前が入って全部選ばれた状態)、**Rename**、Cancel です([行を開く所](../../src/devstudio/view/graphcanvaswidget.cpp#L6745))。Enter で Rename、Esc で Cancel です。
  - ✅UX-3 から、欄に入る名前は Studio の提案です。その definition を走らせる最初の node の id に `_body` を付け、ほかの definition と重なれば `_2` … を足します。それが今の名前と同じときと、どこからも走らせていないときは今の名前です([提案を入れる所](../../src/devstudio/view/graphcanvaswidget.cpp#L6765)、[`GraphCanvasModel::proposeDefinitionName()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7648))。UX-3 の前は今の名前でした。
  - Studio は送る前に名前を確かめ、送れない名前なら欄の右に赤で理由を出して Rename を押せなくします([名前を確かめる所](../../src/devstudio/model/graphcanvasmodel.cpp#L4406))。今の名前のままでも押せません(何も送りません)。
    - 空: `A definition needs a name.`
    - `/` を含む: `A definition's name cannot hold a slash: <name>.`
    - `root`: `A definition cannot be named root: that name is the root document's.`(`root` は root の scope の名前です。engine も断ります)
    - 既にある: `A definition named <name> already exists.`
  - 送るのは `rename_definition` 1 つだけです。engine が同じ op の中で、名指している `graph.instance` の `definition` とループの `branches[].body` も書き換えます(上の「`edit_graph_document` の definition の op」)。
  - 改名した definition の中にいれば、パンくずと今の scope は新しい名前へ移ります。Undo で名前が戻れば、元の名前へ戻ります([移す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1484))。
- **Delete**: 使われていれば、何も送らず行も開かずに、notice に 1 行を出します([断る所](../../src/devstudio/model/graphcanvasmodel.cpp#L4434))。`Definition <name> is used by <node>, <node>: make them run another definition first.` の `<node>` は、その definition を走らせる root の node(instance と、どれかの分岐がそれを body にするループ)を文書の順に 1 つずつ並べたものです。
  - 使われていなければ、上段の下の行が `Delete definition <name>? Nothing runs it.` と **Delete** / Cancel で開き、Delete で `remove_definition` を送ります。確認は dialog ではなくこの行です。
  - engine が断ったときの `graph.definition_referenced@1` の文は、下の拒否の表にあります。
- 開いている行の definition が文書から消えると、行は閉じます。
- 提案のままなら Enter だけです。違う名前は打ちます(全部選んだ状態で開くので、打つと置き換わります)。

**Frame Plan のループのグループ(✅LP-4c)**: Frame Plan は `loops[]` と、展開された pass・task・target・buffer の `loop_origin` を読みます。

- 1 つのループが展開した回の pass と task は、1 つのグループ `loop <id> ×N` になります([ループのグループを決める所](../../src/devstudio/view/frameplangraphics.cpp#L532))。
  - グループは最初に見えたときに畳まれ、その後は開いた・閉じたをそのまま保ちます([最初に畳む所](../../src/devstudio/view/frameplangraphics.cpp#L2528))。
  - region や feature のグループより先にループで分けます。lowering graph の無い plan でも、ループのグループは作ります。
- 回の node の詳細に `Loop origin` の行(`loop steps · element 2 · body node fill`)が出ます。target と buffer にも出ます。
- 回の node の右クリックの **Open in Graph** は、✅LP-4d2 から、root の canvas のままゾーンのタブをその要素の分岐に合わせ、枠の中のその node を選びます(上の「縁をまたぐ線・carry・展開の表示」)。body がゾーンに描かれないときは、その要素の分岐の body の scope に入り、body のその node を選びます(LP-4d2 の前はいつもこの形でした)。
- ✅LP-4d2: Graph タブのゾーンの **Show in Frame Plan** は、plan を読み直してから、このグループを開いて回の node を全部選びます(グループの枠そのものは選べないため。[`FramePlanGraphicsScene::selectLoopGroup()`](../../src/devstudio/view/frameplangraphics.cpp#L3753))。
- ✅Q-18 から、Graph タブでループを変えた後は Frame Plan が自分で読み直します(次の段落)。Q-18 の前は、Frame Plan の **Refresh** を押すまで読んだ時点の写しのままでした。

**Graph タブの編集の後は Frame Plan が自分で読み直します(✅Q-18)**: Graph タブの編集が commit すると、Frame Plan は `get_frame_plan` を読み直します。**Refresh** は要りません。

- 知らせるのは、配置(`set_layout` だけの op の列)ではない commit だけです。Graph タブの model がその回数を数え([`GraphCanvasModel::planCommitRevision()`](../../src/devstudio/model/graphcanvasmodel.hpp#L1704))、増えたら Graph タブが MainWindow に知らせ、MainWindow が Frame Plan に渡します([つなぐ所](../../src/devstudio/view/mainwindow.cpp#L673))。
  - 下の「配置の保存の後の編集も通ります」(Q-13)の共有の digest では知らせません。digest は配置の保存でも動き、Frame Plan の node の位置の保存も Graph の `set_layout` なので、それで読み直すと node を drag するたびに plan を丸ごと読み直すことになるためです(コードの注記と実装者の報告)。
- Frame Plan の側([`planChangedByCommit()`](../../src/devstudio/view/frameplanwidget.cpp#L3645)):
  - 見えていれば、すぐ読みます。
  - 前の読みが往復中なら、その返事の後にもう 1 回読みます(往復中の読みは commit より前の plan かもしれないため)。
  - 見えていなければ印だけ付け、次に表示したときに読みます。engine の RPC が無いときは何もしません。
- Graph タブ自身も、配置でない commit の後は、読み直した文書が配置だけ違うように見えても `get_frame_plan` を読み直します。ゾーンの `×N copies` などが回数の変更に追いつくためです([その分岐](../../src/devstudio/model/graphcanvasmodel.cpp#L1476))。
- シナリオ `graph_loop_expand` / `loop_view_axis` / `material_loop_decompose` は、Graph タブの編集の後に Frame Plan の Refresh を押さなくなりました。
- ⚠ MainWindow のつなぎを通して見ているのは GPU シナリオ(`graph_loop_expand`)だけです。CPU の test は、Graph タブが知らせることと Frame Plan が読み直すことを別々に見ます(Q-18 の着地時の記録)。

**ループと definition の拒否は 1 文で出ます(✅LP-4c / LP-4e)**: `graph.loop_*` と `graph.definition_*` の拒否(✅Q-19 から `graph.input_index_invalid@1` も)は、field の JSON のままではなく、1 文と末尾の code で出ます([`graphLoopRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L1368)、[拒否の 1 文の表 `GraphLoopRefusalTexts` の先頭](../../src/devstudio/model/inspectormodel.cpp#L1096))。

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
| `graph.render_node_parameter_invalid@1` | ✅UX-12a(engine の CT-3)。`<node>.<parameter> must be <expected> (<why>)`。`<why>` は下の「node の欄を catalog の表から作る(✅UX-12a)」の拒否の行の項 |

- 行の形は `<文> (<code>).` です。field が無い所は `?` になります。✅IA-1s から、engine が `document` を付けた拒否(✅CT-2)は `<文> (in <document>, <code>).`、definition の body の node なら `<文> (in definition <definition> of <document>, <code>).` です(下の「画像ファイルを image の初期内容に(✅IA-1s)」の拒否の文書の項)。
- `graph.loop_tier_unsupported@1` の `reason` が draw_loop の 4 つなら、何を直すかを言う文に替わります(✅LP-4e):
  - `carry_in_draw_loop`: `Loop <node> draws inside one material pass, where no material hands a value to the next: remove its carry`
  - `limit_in_draw_loop`: `Loop <node> draws every material of its pass: remove its limit`
  - `resource_in_draw_body`: `A body of loop <node> makes a resource, which a draw_loop body cannot: it holds draw steps only`
  - `mixed_body`: `The bodies of loop <node> mix draw steps with passes: a body holds draw steps only (draw_loop) or passes only (expand)`
- `graph.input_index_invalid@1` の `reason` も、それぞれの文に替わります(✅Q-19。[reason の文](../../src/devstudio/model/inspectormodel.cpp#L1271)):
  - `missing`: `No port of <node> reads input <input_index>: its numbered inputs run from 0 without a gap`
  - `duplicate`: `Ports <ports> of <node> all read input <input_index>: each number is one port's`(`<ports>` は `a, b` の形)
- fields は、code の横、`error_fields`、別の拒否の message の中の `<code> {…}` / `<code>: {…}` のどこからでも読みます。✅RU-1b から、message の中の field を code の横の同じ名前のものより優先します。RU-1b の当時は、変種の preflight で code の横の `variant` が変種の名前になり、`graph.loop_variant_missing@1` 自身の `variant`(material の変種)が message の中にだけ残ったためです。今はこの field は `material_variant` で、`variant` という名前の field を持つ拒否はありません(下)。
- 出る所は、Graph タブの notice と、Frame Plan の status 行です。render target の form は、変種が断ったときと、ループの code の拒否のときにこの文を使います(後者は ✅RU-1b から。LP-2 で draw_loop の拒否 5 つが最上位の code で届くようになったためです)。
- **変種の前置き(LP-0d)**: 変種(preview / xr)の preflight が断ったときは、code が何であっても前に `The <variant> graph refused it: ` が付きます。LP-0d から、変種の graph の拒否が graph の code のまま `variant` 付きで返るためです(それまで Frame Plan の render target の form は、`render_pipeline_preflight_failed` のときだけ前置きを付けていました)。
  - ✅PV-1(engine `a173e70c`)から engine は preview の変種で編集を断らないので、engine が `variant` を付けて返すのは xr だけです。`The preview graph refused it: ` は今の engine からは出ません(engine の側のコードの読み。Studio は変えていません)。
  - `graph.loop_variant_missing@1` の material の変種は field `material_variant` で届きます(engine 5e246063)。code の横の最上位の `variant` は、どの拒否でも常に graph の変種で、前置きはそれで付けます(✅LP-4d2。[`graphRefusalVariant()`](../../src/devstudio/model/inspectormodel.cpp#L1929))。
  - ⚠ RU-1b の当時は、この拒否が material の変種を `variant` という名前で運んでいました。そのため、message の `<code>: {…}` に同じ `variant` があれば graph の変種と見なさず、前置きを付けない回避を入れていました(その前は `The outline graph refused it: ` のように付いていました)。LP-4d2 でこの回避を外しました。
- **赤い印**: 拒否が `graph.loop_*` の code で、`node` にループの id を持つとき、そのループの node の縁が赤くなり、hover で文が出ます。詳細表にも `refused` の行が出ます([`GraphCanvasModel::markRefusal()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4730))。
  - 拒否が `loop_origin` を持つとき(回の node が断られた)は、ループと、その要素の分岐の body の中のその node の両方が赤くなります。
  - 次に commit した編集で消えます。

**material pass をループに分解する —— Decompose into loop(✅LP-4e)**: rendering 節(か fork 済みの preset)の一体版の material pass を、`render.material_pass` と、material ごとの分岐を持つ draw_loop のループに書き換えます。書き換えの中身は上の「ループと分解の RPC」の `decompose_material_pass` です。

- 入口は 2 つです。どちらも同じ処理を呼びます。
  - Frame Plan の material pass の右クリックの **Decompose into loop...**(`pelican.framePlan.decomposeIntoLoop`)
  - Pass Editor で material pass を開いたときの **Decompose into loop...** のボタン(`pelican.fullscreenPass.decomposeIntoLoop`)
- 分解できないと分かる pass では、右クリックの項目が押せず、理由が tooltip に出ます([`framePlanDecomposeRefusal()`](../../src/devstudio/view/frameplanedit.cpp#L1564))。
  - feature の pass(feature の `pass_replacements` で置き換える)、material でない pass
  - 既に draw_loop が描いている pass(`<pass> is already a render.material_pass of the graph; its loop <loop> is edited in the Graph tab.`)
  - fork していない engine の preset の pass(`… fork the preset into the project first (Features tab, Fork), then decompose the pass there.`)
- 押すと確認のダイアログ(`pelican.framePlan.decomposeConfirm`)が出ます。ボタンは **Decompose** と Cancel で、既定は Cancel です。
  - 文は、分解で何ができるか(material の種類ごとに、別の variant・別の render state で描く、描かない)と、元に戻せるかを言います。
  - LP-4e の着地時は「まだ元に戻せない(RD-1 で Undo が付く)」と言っていました。RU-1(`fa413aeb`)からは `Undo takes it back (Undo on the Frame Plan, or Ctrl+Z there).` と言い、分解は render の編集の履歴に `Decompose <pass>` として残ります。
  - Cancel は何も送らず、status 行が `<pass> is left as it is.` を言います。
- Decompose を押した時点の context で要求を組み直して送ります([`framePlanDecomposeRequest()`](../../src/devstudio/view/frameplanedit.cpp#L1596))。direct root の pass は `base_source_digest` だけ、fork 済みの preset の pass は `base_preset_digest` も付けます。
- commit されると Graph タブが前に出て、できたループが選ばれます。Graph タブの文書がまだ古ければ読み直し、ループが届いてから選びます。
- engine の拒否は 1 文で、Frame Plan の status 行に出ます([`framePlanDecomposeRejection()`](../../src/devstudio/view/frameplanedit.cpp#L1634))。

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

**feature をループ版に置き換える —— Replace with loop version…(✅LP-4f2)**: engine が分解版の雛形を同梱している feature(bloom・cube capture・planar reflection の mip chain・directional の影の cascade)を、その雛形のループに置き換えます。中身は上の「ループと分解の RPC」の `apply_graph_template` です。どの feature に分解版があるかは `get_graph_templates` の `decomposes.feature` で決まり、Studio は表を持ちません([`graphTemplatesFromResponse()`](../../src/devstudio/view/frameplanedit.cpp#L1685))。Render Features は一覧を読み直すたびにこれも読みます。`get_graph_templates` を持たない engine では、入口は出ません。

- 入口は 2 つで、どちらも Render Features の同じ処理を呼びます。
  - Render Features で分解版がある feature の行を選ぶと、一覧の下に **Replace with loop version…**(`pelican.renderFeatures.replaceWithLoop`)が出ます。行の右クリックにも同じ項目があります([出し分ける所](../../src/devstudio/view/renderfeatureswidget.cpp#L4341))。分解版の無い feature では出ません。
  - Frame Plan で feature のグループを右クリックすると、同じ名前の項目(`pelican.framePlan.replaceWithLoopVersion`)が出ます([項目を足す所](../../src/devstudio/view/frameplangraphics.cpp#L5058))。押すと Features タブが前に出て、その feature の行で同じ流れが始まります。
- 押せないときは理由が tooltip に出ます。fork していない engine の preset の feature(`<feature> is a feature of the engine preset <preset>: fork the preset into the project first (Features tab, Fork), then replace the feature there.`)、編集できない状態、Apply していない編集がある、のどれかです。
- 押すと確認のダイアログ(`pelican.renderFeatures.replaceConfirm`)が出ます。ボタンは **Replace** と Cancel で、既定は Cancel です([文を組む所](../../src/devstudio/view/frameplanedit.cpp#L1750))。
  - 1 行目は雛形の title(と description)です。例: `Bloom loop`。
  - 続けて何が起きるか: `The feature bloom is removed, and the graph draws what it drew.`(`pass_replacements` の雛形なら `The feature <feature> stays; N of its passes are replaced by <node>.`)、`The graph gains N nodes and M definitions.`、足す define があれば `Shader defines added: …`。
  - 最後に `Undo takes it back (Ctrl+Z here, or Undo on the Frame Plan).`
  - Cancel は何も送らず、`<feature> is left as it is.` を言います。
- Replace を押した時点の一覧から要求を組んで送ります。direct root の feature は `base_source_digest` だけ、project の preset が列挙する feature はその行の preset の digest も `base_preset_digest` に付けます([送る所](../../src/devstudio/view/renderfeatureswidget.cpp#L4267))。
- commit されると、Graph タブが前に出て、雛形が足したループ(応答の `applied.nodes` のうち雛形が `graph.loop` と宣言したもの)が選ばれます。Render Features の notice は緑で `Replaced bloom with Bloom loop. Undo takes it back.` を言い、一覧を読み直す間も残ります。
- 置き換えは render の編集の履歴に `Replace bloom with Bloom loop` として積まれ、Undo / Redo で戻せます(下の「render の編集を戻す」)。
- engine の拒否は 1 文で notice に出ます([`graphTemplateRejection()`](../../src/devstudio/view/frameplanedit.cpp#L1783))。

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

**draw_loop の Loop の欄と分岐の when(✅LP-4e)**: `over: {draw_items: …}` のループ(分解が作るもの)では、Loop の欄が段に合わせて変わります([欄を出し分ける所](../../src/devstudio/view/graphcanvasloopfields.cpp#L671))。

- 出さない欄: Count・Order・Limit・Place・Where(element)・Outputs・Carry。draw_loop の順は phase の既定で、limit と carry は engine が断ります。
- 出す欄:
  - **Contract**(読むだけ。`forward_opaque_v1 (the pass forward_opaque)` の形)。ループの draws がつながる material pass の契約です。
  - **Tags**: `where.tags` の any of(`include`)/ none of(`exclude`)/ all of(`require_all`)の 3 つの欄。tag は `,` か空白で区切り、Enter で送ります。3 つとも空なら `where` を消します。
    - ✅UX-3 から、engine が `material_tags` を返す(EL-1。上の「レンダー設定を編集する」の「draw_loop と region の差し替えが受ける名前も、context から読めます」)間は、3 つとも tag の複数選択です(`pelican.graphCanvasLoop.whereTagsIncludePick` など。押すと tag の check の一覧が開き、check を変えるたびに送ります。[`GraphCanvasTagPicker`](../../src/devstudio/view/graphcanvasloopfields.cpp#L452))。文書が名指す tag は、どの material も持たなくても一覧に残ります。一覧が空(EL-1 の前の engine、engine が動いていない)なら今までの打つ欄です。
- 分岐表の各行は 3 段です([`GraphCanvasLoopFields::addDrawBranchRow()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1255))。
  - 1 段目: body の combo と ↑ ↓ ×(else の行は body の combo だけ)
  - 2 段目: 条件の欄。**material**、**tags**、**contract**、**skinned**、**has variant**
  - 3 段目: 当たった material(`materials: <鍵>, …`。当たらなければ赤で `no material takes this branch`、報告の前は `materials: not reported yet`)
- 条件の欄の中身:
  - **material**: `(any)` と、報告の `elements` に出た material の照合の鍵(`materials/g1_flat.material.json#g1_flat` など)。鍵を打つことはありません。
    - ✅UX-3 から、engine の `materials[]`(EL-1)を先に名前で並べます(組込は `<name> (built in)`。項目の値と tooltip は照合の鍵 `id`)。続けて、一覧に無く報告に出た鍵、文書だけが名指す鍵の順です([material の行を組む所](../../src/devstudio/view/graphcanvasloopfields.cpp#L1255))。
  - **tags**: any of(`include`)だけを編集します。文書が持つ `exclude` / `require_all` はそのまま残ります。✅UX-3 から、engine が `material_tags` を返す間は tag の複数選択です(上の Tags と同じ部品)。
  - **contract**: `(any)` / `gbuffer_v1` / `forward_scene_color_v1` / `legacy_gbuffer_v1`
  - **skinned**: `(any)` / `yes` / `no`
  - **has variant**: `(any)` / `yes` / `no`。✅UX-3 から、その後に engine の material が持つ variant の和を `= <variant>` で並べ、選ぶと `has_variant` にその名前を書きます([`fillHasVariant()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1230))。
  - 選んだものを全部満たす material が当たります(AND)。全部 `(any)` にした行は送りません(条件の無い行は最後の else だけです)。
- material の候補は、報告が 1 ページに収まらない(`total` が `elements` より多い)とき、Graph タブが `loop_elements` で残りを読んで全部集めます([`GraphCanvasModel::requestNextLoopPage()`](../../src/devstudio/model/graphcanvasmodel.cpp#L853))。
- **Add branch** の新しい行の条件は、報告の最初の material の `material = <鍵>` です。報告の前なら `has_variant = yes` です。✅UX-1 から、body は上の「分岐表」と同じく else の body の写しです(`<ループの id>_body`)。
- body の combo の `(nothing)` は、その分岐に当たった material を描かないことです。

**render.material_pass と draw.* の node(✅LP-4e)**:

- `render.material_pass` のソケットは契約から出ます([`GraphCanvasModel::materialPassColorSockets()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4934))。
  - forward(`forward_opaque_v1` / `forward_transparent_v1`)は `color`、G-buffer(`deferred_geometry_v1`)は `albedo` / `normal` / `material` / `worldpos` / `emissive` です。`material_outputs` を宣言した pass では、その出力の名前です。
  - 続けて `depth` と、任意の `draws` です。
  - ✅GL-1f から、color のソケットと同名の出力と `depth` の出力があります(下の「pass の出力のソケット(✅GL-1f)」)。GL-1f の前は出力ソケットが無く、書いた target は image node か `render.target_ref` から読みました。
- palette の `render.material_pass` は root にだけ出ます。palette の Contract の combo(`pelican.graphCanvasPaletteContract`。`forward_opaque_v1` / `deferred_geometry_v1` / `forward_transparent_v1`)で契約を選びます。
  - ソケットごとの `render.image`(`<pass>__<socket>`)と depth の image を同じ列で足してつなぎます。`draws` には、同じ列で足す draw ループをつなぎます(✅GL-1d。下の「draw ループ付きで来ます」)。
  - image の format と usage は、Frame Plan の Add material pass… の form と同じ表 [`materialPassSlots()`](../../src/devstudio/model/frameplanmodel.cpp#L2238) から取ります(✅MC-1s、`1d10bd4c`。[表から image を足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L3431))。`hdr` の判定は [`materialPassHdr()`](../../src/devstudio/model/frameplanmodel.cpp#L2294) に、Graph タブが持つ authoring context(`get_render_authoring_context` の `enabled_feature_names`。palette を開くときにまだ無ければ読みます)を渡します。
    - G-buffer(`deferred_geometry_v1` / `legacy_gbuffer_v1`): `albedo` / `emissive` は `hdr` があれば `R16G16B16A16_SFLOAT`、無ければ `B8G8R8A8_SRGB`。`normal` / `worldpos` は `R16G16B16A16_SFLOAT`、`material` は `R8G8B8A8_UNORM` です。
    - forward の `color` は `hdr` に関わらず `R16G16B16A16_SFLOAT`、depth は `D32_SFLOAT` です。
    - context が `enabled_feature_names` を持たない(まだ届いていない)ときは、form と同じく HDR とみなします。表の色の slot の数と contract のソケットの数が合わなければ何も置きません。
    - `material_outputs` の無い material pass は、live の material が登録された物理の契約と照らされます。Blank には `hdr` が無いので live の material は SDR の表で、表どおりの format なら engine は起こし直しません(engine の MC-1。§13.5 の RS-1 の項)。
    - MC-1s の前は `hdr` を見ずに `material` 以外の色を常に `R16G16B16A16_SFLOAT` にしていたので、Blank に deferred の pass を置くと `material_physical_contract_changed`(`detail: ["color_formats"]`)で engine が起こし直していました(GL-5c の「分かっている穴」)。コードのコメントは「engine は最初の attachment が `R16G16B16A16_SFLOAT` なら HDR の組を選ぶ」と言っていましたが事実と違い、MC-1s で直しました。
    - 表 `MaterialPassSlot` / `materialPassSlots()` と `hdr` の判定は、MC-1s で view の `materialpassedit` から model の `frameplanmodel` に移し、form と palette が同じ 1 つを使います。
    - test: `devstudio_graphcanvas_test` の [「MC-1s the palette's material pass images take the form's formats for the pipeline's HDR switch」](../../test/devstudio_graphcanvas_test.cpp#L1890)(hdr なしと `["bloom","hdr"]` の context で 3 つの contract を置き、各ソケットの image の format と usage が `materialPassSlots(contract, hdr)` と値そのものに一致する。context が何も言わなければ HDR)。
  - `0c252183` の前は `albedo` と `emissive` も `R8G8B8A8_UNORM` で作っていたので、deferred の pass は `Material pass color output format mismatch: <pass>__albedo` で断られ、engine が止まりました。commit の記録では、白紙の project に置いた文書を headless の player で走らせ、前は止まり後は描けることを確かめています。GPU シナリオはありません(✅GL-1d からは、下の `graph_from_blank` が deferred の pass を Blank の project に足し、frame plan に出るまでを GPU で通します)。
  - **draw ループ付きで来ます(✅GL-1d)**: pass と image と同じ列で、次も足します。Undo 1 回で全部戻ります([ループと body を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L3374))。形は `decompose_material_pass` が書くもの(と GL-1 の畳んだ node の中身)と同じです。
    - definition `<pass>_draw`: node `step`(`draw.material`、出力 `draws`)が 1 つで、definition の出力 `draws` はその `draws` です。
    - root の `graph.loop` `<pass>_draws`: `over: {draw_items: {contract: <選んだ契約>}}`・`tier: "draw_loop"`・`order: {by: "phase_default"}`・else の分岐 1 つ(body `<pass>_draw`)・出力 `draws`。
    - pass の `draws` 入力 ← `<pass>_draws` の `draws`。
    - 列の順は definition → ループ → image → pass です。root には pass の横にループのゾーン(`draw_loop`)が描かれ、中に body の `draw.material` が出ます。分岐と body は上の「draw_loop の Loop の欄と分岐の when」で編集できます。
    - pass の id は `render_material_pass` です。その id の node、`<id>_draws` の node、`<id>_draw` の definition のどれかがあれば、`render_material_pass_2`、`_3` … にします。
    - GL-1d の前は `draws` を空けていました(一体版と同じ描き方)。material の draw ループは、rendering 節の material pass を Decompose into loop したときにしか作れませんでした。
    - test: `devstudio_graphcanvas_test` の [「GL-1d the palette's material pass arrives with its draw loop, body and draws edge」](../../test/devstudio_graphcanvas_test.cpp#L1821)(列の最初の 2 つの op、pass の `draws` の線、Undo の逆の列に `remove_definition` があること、名前が使われているときの `_2`)。
  - **forward の route の既定を一緒に送ります(✅GL-1d)**: 契約が `forward_opaque_v1` で、pipeline に deferred の material pass が無いとき、Studio は `edit_graph_document` の params に `material_route_default: "forward"` を付けます([付けるかを決める所](../../src/devstudio/model/graphcanvasmodel.cpp#L3612))。engine の側は [レンダー設定を編集する](#レンダー設定を編集する) の「material の route の既定と、受け皿の無い material の拒否」にある RS-2 です。
    - GL-1d の前は、Blank の project で palette の既定の契約(`forward_opaque_v1`)の pass を足すと、`material_route_unserved@1` で断られました(利用者が Release で当たりました)。
    - 判定は Frame Plan の **Add material pass...** の form(下の「forward の既定を同じ Save で入れる(✅G-4)」)と同じ関数です([`materialRouteOffersForwardDefault()`](../../src/devstudio/model/frameplanmodel.cpp#L2233))。deferred の pass とは、契約が `deferred_geometry_v1` か `legacy_gbuffer_v1`(契約が空のものを含む)の pass です([`materialContractTakesDeferredRoute()`](../../src/devstudio/model/frameplanmodel.cpp#L2222))。GL-1d でこの 2 つを `frameplanmodel` に移し、form と Graph タブが共有します。
    - Graph タブが数える pass は、最後に読んだ frame plan の material pass(`material_routing` が契約を言うものだけ)と、root の文書の `render.material_pass` です([`GraphCanvasModel::pipelineHasDeferredMaterialPass()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3622))。
    - Redo も付けて送ります。Undo は付けません。route の既定は list のものではなく project の設定なので、Undo で pass を消しても既定は戻りません([Undo のときは付けない所](../../src/devstudio/model/graphcanvasmodel.cpp#L2733))。
    - 既定が変われば、engine は commit の結果に `restart_required` を付けます。Graph タブはそれで engine を起動し直します(下の「engine の再起動が要る graph の編集(✅GL-1b)」)。
    - この param を知らない engine が `unsupported field` と `material_route_default` を含む文で断ったら、付けずに同じ列を 1 回送り直します。以後その model は付けません([`GraphCanvasModel::retryWithoutRouteDefault()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2743))。この「付けない」は Studio の session の間続きます(実装者の報告)。
    - その結果の `material_route_unserved@1` は、Frame Plan と同じ文(`No pass draws the <route> route, which <名前> take(s). To fix: <hint>.`)で notice に出ます。Graph タブの拒否の文を作る所が [`materialRouteUnservedNotice()`](../../src/devstudio/model/frameplanmodel.cpp#L2306) を使います([使う所](../../src/devstudio/model/graphcanvasmodel.cpp#L35))。GL-1d の前は engine の message がそのまま出ていました。
    - test: `devstudio_graphcanvas_test` の [「GL-1d a forward material pass sets the route default, and falls back without it」](../../test/devstudio_graphcanvas_test.cpp#L1964)(forward_opaque_v1 だけに付くこと、受ける engine では Undo が付けないこと、`unsupported field` の拒否で付けずに送り直し、拒否の文が Frame Plan と同じになり、次の追加でも付けないこと、ほかの失敗は送り直さないこと)と [「GL-1d a pipeline with a deferred material pass offers no route default」](../../test/devstudio_graphcanvas_test.cpp#L2029)。
- material pass を選ぶと、詳細表の上に **Contract** の combo(`pelican.graphCanvasDraw.contract`)が出ます([`GraphCanvasDrawFields::showNode()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1550))。
  - ✅UX-3 から、その下に **Variant**(`pelican.graphCanvasDraw.materialVariant`)が出ます。`(each material's own)` と、engine の material が持つ variant の和です。選ぶと `material_variant` を書き、`(each material's own)` で消します(`set_node_parameters` 1 回。[欄を置く所](../../src/devstudio/view/graphcanvasloopfields.cpp#L1465))。engine が variant を返さず、pass も `material_variant` を持たないときは出ません。
  - ✅UX-11 から、その下に Pass Editor の form の欄(`pelican.graphCanvasMaterial`。Tags・Range・出力・binding・Load / Clear / Store と Materials drawn)が出ます(下の「Graph の material pass の欄を Pass Editor の form で(✅UX-11)」)。
- `draw.material` / `draw.variant` / `draw.override` は、draw_loop が走らせる body(か、既に draw step を持つ definition)の中でだけ palette に出ます。
  - 置いた step は body の列の**先頭**に入ります。今の先頭の `after` を新しい step につなぎ、`draws` を作る step は末尾のままです([draw step を置く所](../../src/devstudio/model/graphcanvasmodel.cpp#L3451))。
- `draw.variant` の欄は **Variant**(`pelican.graphCanvasDraw.variant`。打てる combo)です。候補は、ループが描く pass の `material_variant` です。置いたときもその値が入り、無ければ `variant` です。
  - ✅UX-3 から、engine の material が variant を持つ間は、その和から選ぶだけの combo です(打てません。[選ぶ形にする所](../../src/devstudio/view/graphcanvasloopfields.cpp#L1613))。一覧が空なら今までの打てる combo です。
- `draw.override` の欄:
  - **Cull**(`none` / `front` / `back`)、**Depth test** / **Depth write**(`on` / `off`)、**Blend**(`opaque` / `blend` / `additive`)。✅UX-3 から **Depth compare**(`never` / `less` / `equal` / `less_equal` / `greater` / `not_equal` / `greater_equal` / `always`。[欄を置く所](../../src/devstudio/view/graphcanvasloopfields.cpp#L1515))も。どれも先頭の `(material's)` は鍵を書かず、material の値のままにします。
  - **Defines** は 1 行に 1 つ(`NAME` か `NAME=VALUE`)で、**Apply defines** で送ります。
  - 何も替えない override は送りません。置いたときは `render_state: {cull: none}` が入っています。

**Frame Plan の material pass の loop の行(✅LP-4e)**: draw_loop は展開されず、1 つの material pass の中で走ります。

- draw_loop が描く material pass は、見出しの下に `loop: forward_opaque_draws (3 materials)` の 1 行を持ちます([`framePlanDrawLoopLine()`](../../src/devstudio/view/frameplanedit.cpp#L1556))。
- 詳細に `Draw loop <id>` の組が出て、分岐(`Branch 0` …)ごとに当たった material の鍵が並びます([詳細の material の一覧](../../src/devstudio/view/frameplanwidget.cpp#L4400))。
  - 当たらない分岐は `none: no material takes this branch` です。
  - plan が持つ要素が `total` より少ないときは、`<k> of <total> shown` と **Read more**(`pelican.framePlan.loopMaterialsMore`。256 件ずつ読む)が出ます。

**分かっている穴**(LP-4c / LP-4e の着地時の記録):

- definition の改名・削除の UI はありません(model には op があり、Undo もできます)。
- ✅Q-18 で塞がりました: この時点では、Graph タブの `↻ expand ×N` と Loop の欄の報告は Graph タブが文書を読むときにだけ読み直し、Frame Plan は Graph タブの編集の後に自分からは読み直さないので、**Refresh** が要りました。今は Graph タブの commit の後にどちらも読み直します(上の「Graph タブの編集の後は Frame Plan が自分で読み直します」)。
- Place は canonical anchor だけで、式の条件(`when` の式)は表では編集できません。
- 分岐の tags は any of(`include`)だけを編集できます。`has_variant` に文字列を書いた `when` は、表では表せません(✅UX-3 で塞がりました: has variant の combo の `= <variant>` で表せます)。
- Frame Plan の回の node からの **Open in Graph** は、Graph タブの側(body の node を開く所)だけを test していて、Frame Plan から Graph タブへのつなぎは test していません。詳細の **Read more** も、ボタンが出ることだけを見ていて、押して次のページを読む所は test していません。
- **Per iteration…** は CPU の test だけで、GPU シナリオでは通していません。
- `render.target_ref` の parameters は、まだ値の帯(pill)として出ます。✅UX-12a から、選ぶと右に catalog の表から作った `name`(打つ欄)と `type`(畳める見出しの下の `semantic` と `arguments`)の欄も出ます。帯はそのままです。
- palette で置く `render.material_pass` を GPU で描いた確認はありません。✅GL-1d のシナリオ `graph_from_blank` / `graph_from_blank_forward` は、足した pass が engine の frame plan に出て、ループが `draw_loop` で走ることまでを見ます。viewport の色は見ていません。✅GL-1e から、`graph_from_blank_forward` は forward の pass の `color` を `present` の入力へつなぎ、viewport の中央の色が変わることまで見ます(下の「Studio の自動化面」のシナリオ)。deferred の pass の色は見ていません。

**graph の外から来た pass を畳んだ node で見る(✅GL-1)**: Graph タブの root の canvas は、graph の node が作っていない pass も描きます。rendering 節の pass、preset の pass、feature の pass です。どれも読むだけの node で、文書には何も書きません(設計は [design_graph_layers](../design_graph_layers.md) §7 の GL-1)。✅GL-1e から、root が自分で宣言した fullscreen pass だけは入力の線を受け、`edit_preset_pass` を送ります(下の「Graph から画面に出す(✅GL-1e)」)。

- **plan を毎回読みます。**Graph タブは文書を読むたびに `get_frame_plan` を 1 回読みます([`GraphCanvasModel::requestLoopReports()`](../../src/devstudio/model/graphcanvasmodel.cpp#L830))。GL-1 の前は、ループや view_select を取る render node などがあるときだけ読んでいました(上の「ループの node」)。読みが 1 つ増えます。
  - engine がまだ最初の plan を持たないときは、失敗した読みを繰り返します。回数の上限は 480 で、コードのコメントでは poll の 2 分ぶんです([待つ所](../../src/devstudio/model/graphcanvasmodel.cpp#L1130))。
- **何が出るか**([`GraphCanvasModel::rootFoldedNodes()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5767)):
  - `provider_feature` を持つ pass は、feature ごとに 1 つの畳んだ node にまとまります。見出しは feature の名前(`engine://features/bloom.json` なら `bloom`)、型の所に `feature`、見出しの下に `N passes` です。
  - それ以外の pass は 1 pass 1 node です。型の所は plan の `kind` で、material pass は `material` です。
  - material pass かどうかは plan から決めます。`shader_resolution.state` が `material_owned`、`material_filter` がある、`material_variant` がある、のどれかです([`GraphCanvasModel::readPlanPasses()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5569))。
  - `kind` が `anchor` の node は出ません。
  - node の行は資源の名前です。右が外へ渡す資源(書く)、左が外から読む資源です。feature の中の pass 同士で受け渡す資源は出ません。
- **見た目**: 枠は破線で、地の色は graph の node より 1 段落ちます。theme の token は `derived_border` / `derived_body` です([theme の表](../../src/devstudio/resources/studio_themes.json#L65))。見出しの帯は Frame Plan の出所の色を薄くしたものです。
  - tooltip は出どころで替わります(✅GL-5a から。[`GraphCanvasFoldedNode::tooltip()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5555)): root の rendering 節が宣言した pass(Blank の `present` など)は `From the rendering section: edit it here.`、feature の pass は `From feature <name>: read only here. Expand to edit…`、preset の pass は `From preset <name>: read only here. Expand to edit…`(authoring context が preset の名前を言わなければ `From the preset (<source>): …`)です。GL-5a の前はどれも `From the rendering section (<source>): read only here. Frame Plan and Features edit it.` でした(rendering 節の pass の直し方は下の「rendering 節の pass を Graph の詳細欄で直す(✅GL-5a)」)。潜れる node では `Double-click to look inside.` が続きます([`Builder::addDerivedNode()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L1352))。
- **置き場所(✅GL-1b / GL-1c)**: root の graph の node と畳んだ node を、1 回の配置でまとめて並べます。派生の線も配置の辺に入るので、列は plan の依存の最長経路で決まり、plan の順が左から右へ読めます。Frame Plan の Logical graph と同じ [`layoutNodeGraph()`](../../src/devstudio/view/nodelayout.cpp#L238) です([配置に入れる所](../../src/devstudio/view/graphcanvaswidget.cpp#L2006))。
  - 文書の root の `layout` が graph の node を 1 つでも置くと、その配置はもう graph の node の場所を表しません。このとき畳んだ node は、畳んだ node とその間の線だけで同じ配置をして、root に描いたものの左に並べます。
  - root に描いたもの(✅GL-1c)は、node とその子(ソケット、箱からはみ出す値の字)と、ゾーンの枠全体(見出し・縁の行・carry の矢印・下端の行)です([測る所](../../src/devstudio/view/graphcanvaswidget.cpp#L2104))。コードのコメントでは、1 回の配置の中でゾーンは枠の大きさの 1 つの箱なので、そこでも畳んだ node は枠に重なりません。
  - 並べた場所はセッションの間覚えます。覚えた場所のどれかが root に描いたものに重なる(後で body が埋まってゾーンが大きくなった、node をその上に置いた)か、場所の無い畳んだ node があれば、並べ直して覚え直します([判定](../../src/devstudio/view/graphcanvaswidget.cpp#L2138))。
  - 文書に配置が無い間に graph の node を drag すると、畳んだ node はそのときの場所を覚えます。
  - drag した畳んだ node は、セッションの間その場所に残ります。文書が変わっても戻りません。
  - 畳んだ node の場所は文書に書きません(`set_layout` に入りません)。Auto layout での扱いは下の「Auto layout」です。
  - GL-1 の着地時は、graph の node の左に 1 列、plan の順で縦に並べ、drag した場所も文書が変わると戻っていました。GL-1b の着地時は、Auto layout で覚えた場所を graph の node とだけ比べていたので、後で大きくなったゾーンの上に畳んだ node が乗りました(`loop_zone_wire` で、picking の畳んだ node がゾーンの Show in Frame Plan のボタンを覆い、押すと畳んだ node が選ばれた)。GL-1c で直しました。
  - 分かっている穴(GL-1c の着地時の統合側の報告): root の node の長い値の字(image の `type` の JSON など)は省略されず、左へ大きくはみ出します。コードの上では、はみ出した字も「描いたもの」に入るので、畳んだ node はその分さらに左に並びます。
- **選ぶと**、詳細表の上に 1 行の要約(`pelican.graphCanvasFoldedSummary`)が出ます。`<名前> (<型>): `、feature なら `N passes of <feature>.`、`Reads …; writes ….`、最後に上の tooltip の文です。
- **派生の線**: plan の `reads` / `writes` / `reads_history` から、資源の名前で破線を引きます([`GraphCanvasModel::derivedEdges()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5842))。
  - 書き手は、読む pass より前で最後にその資源を書いた pass です。history の読みでは frame 全体の最後の書き手です。
  - 資源を名乗る graph の node は、その資源を `parameters.name` に持つ `render.target_ref` と、同じ id の `render.image` / `render.buffer` / `render.attribute` です。graph の node の pass が読む側なら、線はその node に付け替えます。plan に書き手が無い資源は、その node から引きます。それ以外は、pass を作った graph の node に付けます。
  - 少なくとも片方の端が畳んだ node の線だけを引きます。同じ 2 つの node の間の資源は 1 本にまとめます。tooltip は `<資源>, …: from the frame plan (read only)` です([`Builder::addDerivedEdge()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L1505))。
  - ✅GL-1e から、rendering 節の pass の `input` が名指す資源は、その pass への破線を引かず、実線で引きます(下の「Graph から画面に出す(✅GL-1e)」)。
  - 破線をダブルクリックしても制御点は入りません。それ以外の操作で op が出ないことは test では見ていません(未確認)。
- **潜る**: 畳んだ node をダブルクリックすると、中を読むだけで見せます([`GraphCanvasModel::enterFolded()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6638))。パンくずは `root / forward_opaque` のように畳んだ node の名前を足し、押すと 1 段ずつ戻ります。`Scope:` のコンボで選ぶと、本物の文書のその scope に戻ります。
  - **material pass**: `render.material_pass` 1 つと、draw_loop のゾーン 1 つです。ゾーンの id は `<pass>_draws` で、分岐は else 1 つ、body `<pass>_draw` は `draw.material` 1 つです(pass に `material_variant` があれば `draw.variant`)。Studio が plan からその場で組み、どこにも書きません([`GraphCanvasModel::foldedDocument()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6510))。
  - 契約は plan の `material_routing` から取ります。取れなければ、`render.material_pass` のソケットは `depth` と `draws` だけです(実装者の報告)。
  - plan の `loops[]` にこのループは無いので、ゾーンの段は `draw_loop` です。plan の `material_filter.resolved_draw_count` があれば、その数を要素の数にします。
  - **feature**: その feature の pass を plan の順に 1 つずつ、上と同じ読むだけの node で並べ、その間の派生の線も引きます。中の material pass にはさらに潜れます。
  - その feature の雛形が `get_graph_templates` にあれば、パンくずの下に **Show decomposed version** のボタンが出ます。押すと雛形の `nodes` と `definitions` を読むだけで描き(ループはゾーン)、パンくずに `<feature> (decomposed)` が足されます。
  - `get_graph_templates` は、plan に feature の pass があるとき、接続ごとに 1 回だけ読みます([読む口](../../src/devstudio/view/graphcanvasreadcapability.hpp#L64))。持たない engine ではボタンが出ません。
  - それ以外の 1 pass の node は潜れません(ダブルクリックしても何も起きません)。
  - 中の絵は、Studio が組んだ文書を本物と入れ替えて描いています。engine の返事を読む間だけ本物に戻し、読んだ後で組み直します(コードのコメント)。中にいる間に文書を読み直しても、同じ中身が組めればそのまま残ります。
- **中で編集しようとしたとき**: 線を引く・値を変える・node を足す・消すなど、op を送る操作は何も送りません。canvas の上に 1 行 `This is the engine's folded implementation: expand it to edit.` と **Expand to edit…** のリンクが出ます(`pelican.graphCanvasFoldedNotice`。[`GraphCanvasModel::refuseFoldedEdit()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6712))。
  - 配置だけの列(`set_layout`)は、何も言わずに捨てます。
  - リンク(action `pelican.graphCanvas.expandToEdit`)は、今ある流れを確認のダイアログ付きで始めます([`GraphCanvasModel::foldedExpand()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6690))。rendering 節の material pass なら Frame Plan の **Decompose into loop...**(上の「material pass をループに分解する」)、雛形のある feature(とその中)なら Render Features の **Replace with loop version…**(上の「feature をループ版に置き換える」)です。どちらにも当たらなければリンクは出ません。
  - 1 行は、畳んだ中から出ると消えます。
- **領域の filter**: 上段の操作列に `Show:` と 5 つの check が並びます(`Render` / `Pose` / `Value` / `Audio` / `UI`。名前は `pelican.graphCanvasShow.<領域>`)。既定は `Render` だけです([既定を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L5134))。状態はセッションの間だけで、文書には書きません。
  - 領域は op の接頭辞で決めます。`render.*` と `draw.*` は Render、`value.*` と `event.*` は Value、`pose.*` / `audio.*` / `ui.*` はそれぞれの領域です。それ以外(`graph.instance` と `graph.loop` を含む)は常に出ます([`GraphCanvasModel::opDomain()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6729))。
  - 効くのは root の canvas だけです。definition の中と畳んだ中では全部出ます。Render を外すと、畳んだ node も出ません。
  - 隠れた node と見えている node の間の線は描きません。代わりに、見えている側のソケットに小さな口が出ます。入力側は `← value (N)`、出力側は `→ pose (N)` の形で、N は隠れたその領域の node の数です。押すとその領域が filter に足されます([`GraphCanvasModel::domainStubs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6760))。
  - Inspector や Frame Plan から、隠れた領域の node を開くと、その領域が足されます。
- **Frame Plan の Open in Graph は全 pass に出ます**: graph の node が作っていない pass では、Graph タブを前に出し、本物の root に戻ります。その pass が描かれている node(feature の pass なら feature の畳んだ node、それ以外はその pass の node)を選び、中央に置きます([`GraphCanvasWidget::revealPlanPass()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8775))。material pass でも潜りません。
  - Graph タブがまだその pass を読んでいなければ、Frame Plan の status 行が `The Graph tab has not read pass <pass> yet: refresh it.` を言います([分ける所](../../src/devstudio/view/frameplanwidget.cpp#L3536))。
  - ✅GL-2b から、行き先は plan の `origin` で決めます([`GraphCanvasModel::planNodeTarget()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6219))。instance の中の node の pass なら、instance を通ってその body に降り、node を選びます。ループの回ならゾーンの中の body の node で、ゾーンのタブをその要素が走った分岐に移します。それ以外は root の node か畳んだ node です。`origin` を出さない engine では、ループの回は `loop_origin` で、他は下の名前の一致で決めます。
- **graph の pass かどうか**は 1 か所で決めます([`GraphCanvasModel::graphOwnerOf()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5723))。
  - ✅GL-2b から、engine が `origin` を出していれば(✅GL-2a。§13.5 の「`get_frame_plan` に足されたもの」)それだけで決めます。`origin` の無い pass は、名前や `source` が何であっても、preset・feature・rendering 節の pass として畳みます。`origin` のある pass は、`instance_path` があればその先頭の root の instance、`loop` があればそのループ、どちらも無ければ `node` の下に描きます。
  - engine が `origin` を出すかどうかは、plan に `diagnostics` か `deactivated_graph_nodes` の鍵があるか、どれかの node に `origin` があるかで見ます([`GraphCanvasModel::readPlanLayers()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6066))。
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

- **出る条件**: 本物の root(definition の中や畳んだ中ではない)で、`Show:` が Render だけで、plan を読めているときです([`GraphCanvasModel::layersShown()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6286))。Render のほかの領域を足すと、帯も Layers も消えます。
- **node と plan の node の対応**: root の node ごとに、その下に描かれている plan の node を plan の順で集めます([`GraphCanvasModel::planNodesOf()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6274))。graph の node が作った pass はその node(instance の中の node なら root の instance、ループの回ならループ)、そうでない pass は畳んだ node です。anchor は数えません。
- **帯**([`GraphCanvasModel::layerBand()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6296)): 既定と違うことだけを ` · ` でつないだ 1 行で、node の枠の下に薄い字で描きます(畳んだ node にも付きます。自動操作の名前は `graph_layer_band#<node>`。[描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1585)、[root の node に帯を付ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L2249))。言うことが無ければ出ません。
  - `×N copies`: node が plan の中で 2 つ以上の複製(instance の path・ループ・要素の組)になったとき。ループではゾーンの下端の行が回の数を言うので、帯には出しません(✅GL-1b から。GL-2b の着地時は出していました)。
  - `N barrier` / `N barriers`: その node の pass が端のどちらかにある barrier の数。
  - `aliased`: その node の pass が `from_nodes` に入る alias group があるとき。
  - backend の名前: pass の `execution_plan` の `selected_endpoint` が graph の backend と違うとき。graph の backend は pass の中で一番多い `selected_endpoint`、どの pass も言わなければ選ばれた候補の `endpoint` です([`GraphCanvasModel::graphBackend()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6200))。
  - `deactivated: <reason> (<feature>)`: plan の `deactivated_graph_nodes` にある root の node。この node は薄く(不透明度 0.45)描きます。
  - tooltip は `<帯> below the graph: select the node for its Layers` です。
- **Layers の節**(`pelican.graphCanvasLayersToggle` と `pelican.graphCanvasLayers`。[`syncLayers()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6887)): root の node を選ぶと見出しが `Layers: <node>`、何も選ばないと `Layers: the whole graph` です。見出しを押すと畳めます(`▾` / `▸`)。行が無ければ節ごと隠れます。definition やゾーンの中の node を選んでいる間は出ません。
  - 行は `L1 expanded` / `L2 physical` / `L3 backend` の 3 つの組に分かれます([`GraphCanvasModel::layerRows()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6377))。
  - L1: 展開した plan の node(`<名前> · <kind か material>`。ループの回なら ` · loop <loop> [<要素>]`、instance の中なら ` · instance <path>`)、`replaces` のある node の `<pass> replaces pass <pass> of <feature>`、`deactivated: <reason> (feature <feature>)`。feature の畳んだ node では、graph の node に置き換えられた pass を `pass <pass> is replaced by graph node <node>` で言います。
  - L2: その node の pass が `from_nodes` に入る resource(`resource <名前> · <format>`。alias group に入っていれば ` · alias <id>`)、alias group(`alias group <id>: <resource, …>`)、scope(`scope <id> (<kind>)`)、barrier(`<from> → <to> · <resource> · <kind>`。両端の `execution_plan` の `resource_uses` に access があれば ` · <from の access> → <to の access>`)。最上位の `barriers` は layout を持たないので、barrier の行に layout は出ません(実装者の報告)。
  - L3: `from_nodes` がその node の pass に当たる決定(`<id> · <subject> → <selected>`)と、graph の backend と違う pass の backend(`backend <endpoint> (<pass>)`)。
  - 何も選ばないときは graph 全体のもので、`from_nodes` が空の決定と、backend の候補のすべて(`candidate <名前> · <endpoint> · selected|feasible|rejected`)です([`GraphCanvasModel::graphLayerRows()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6467))。
  - plan の node を持つ行(L1 の pass・置き換えの行と L3 の backend の行)を押すと、Frame Plan が前に出て、Logical graph のタブでその node を選び、中央に置きます。その node を含むグループは開きます([選ぶ所](../../src/devstudio/view/frameplangraphics.cpp#L3785))。status 行は `<node> is picked.`、plan に無ければ `The frame plan has no node <node>: refresh it.` です。行の tooltip は `Click to pick <node> in the Frame Plan.` を足します。
- **診断**: plan の `diagnostics` が 1 つ以上あれば、canvas の上の notice の下に `▸ N diagnostics`(1 つなら `1 diagnostic`)の 1 行が出ます(`pelican.graphCanvasDiagnostics`)。押すと開いて、`<kind> · <subject>: <detail>` の一覧(`pelican.graphCanvasDiagnosticsList`)が出ます([`syncDiagnostics()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6951))。この 1 行は領域の filter によらず出ます。
- **分かっている穴**(着地時の記録。実装者の報告を含む):
  - 帯と Layers は `Show:` が Render だけのときだけです。
  - backend の候補は node に結ばず、何も選ばないときに全部出します。engine の候補の `from_nodes` は実物で全部 `[]` です(§13.5 の「`get_frame_plan` に足されたもの」)。
  - feature の pass が全部 graph の node に置き換わると、feature の畳んだ node が無いので、`is replaced by` の行も出ませんでした。✅UX-15b から、そのときは何も選ばないときの graph 全体の行(L1)に `pass <pass> of <feature> is replaced by graph node <node>` を 1 回ずつ出します([`GraphCanvasModel::graphLayerRows()` の頭で足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L6471))。feature の畳んだ node が残っていれば、今までどおりその node の行が言い、graph 全体の行には出しません。

**速くしたかが見える —— GPU 時間・plan の数・編集の前後の 1 行(✅UX-6b)**: GB-2 の `gui_build_optimize` で、速くしたつもりの編集が効いたかを ms では見分けられず、plan の数でしか見えなかったこと(仕様の背景)を受けて、Frame Plan と Graph タブに読むだけの行を足しました。

- **合計の 1 行**: Frame Plan の status 行の右(`pelican.framePlanPerf`)と、Graph タブの上段の操作列の下の行(`pelican.graphCanvasPerf`)に、`GPU 0.15 ms · 24 passes · 20 barriers · 2 alias groups` の形で出ます([`PlanPerfModel::summaryLine()`](../../src/devstudio/model/planperfmodel.cpp#L327))。
  - GPU は `get_gpu_timing` の node ごとの平均の和で、それを 1 度も読んでいない間は `get_status.gpu_timing` の窓の平均です。窓が 30 frame に満たない間は `GPU measuring…`、timing が無効なら `GPU timing off` です。数は `get_frame_plan` の `nodes` / `barriers` / `physical_target_plan.alias_groups` の数です。
  - 2 つの panel は同じ読み手 [`PlanPerfHub`](../../src/devstudio/view/planperfhub.hpp#L31) の値を出します([読み手を作る所](../../src/devstudio/view/mainwindow.cpp#L815))。どちらかの panel が見えている間、`get_gpu_timing` を 2 秒ごとに送り、ほかの panel が送った `get_gpu_timing` と `get_frame_plan`(draw_loop の頁を除く)の返事も読みます。編集・ticket の問い・commit の後の読みのどれかが 3 秒以内にあった間は、自分からは何も送りません([`PlanPerfHub::poll()`](../../src/devstudio/view/planperfhub.cpp#L145))。返事の parse は thread pool でします。
- **node ごとの時間**: Graph の root の node の帯(上の「下の層を node で見る」の帯。出る条件も同じ)の最後に、その node の plan の node の時間の和を `0.02 ms` の形で足します([`GraphCanvasScene::layerBandText()`](../../src/devstudio/view/graphcanvaswidget.cpp#L1400))。新しい時間は、出ている帯の字だけを替え、canvas を描き直しません。Frame Plan の Logical graph では、倍率が 0.6 より小さいと node の時間の字を隠し、node の tooltip の `GPU:` の行で読みます([倍率の閾値](../../src/devstudio/view/frameplanwidget.cpp#L86))。
  - node ごとの値は、engine の ✅ST-1 の `get_status.gpu_timing.node_averages` があればその行(`barriers_ms` と、`body_supported` なら `body_ms` の和)で、無い engine では `nodes` の sample の和を `samples` で割ります([`PlanPerfModel::statusReading()`](../../src/devstudio/model/planperfmodel.cpp#L85))。
- **編集の前後の 1 行**(`pelican.framePlanPerfDelta` / `pelican.graphCanvasPerfDelta`): plan を動かしうる編集(`edit_graph_document`、`restore_render_documents`、render の編集の履歴が記録する method)が出た時の世代を「前」、その後の新しい世代の窓が 60 frame になったら「後」として、1 行を出します([`PlanPerfModel::resolve()`](../../src/devstudio/model/planperfmodel.cpp#L275))。
  - 形は `after draw_sort: passes 24→24, barriers 20→20, alias 2→2, GPU 0.15→0.14 ms · generation 3→4` です。数が全部同じなら先頭に `no change in the plan · ` が付き、GPU の差が「前」の窓の揺れ(10 と 90 percentile の間)より小さければ ` · GPU difference is within noise` が付きます。世代の初めの frame(窓が満ちていなければ最初の 20 frame、ただし 1/3 まで)は平均と揺れから外します(再起動の直後の compile のため。コードの注記)。
  - 名前は、`edit_rendering_keys` なら書いたキー(`draw_sort`)、graph の編集なら最初の op が触る node(`set_node_parameters` の鍵が 1 つなら `<node>.<鍵>`、ほかは `<node>`。`gui_build_optimize` では `loop` / `render_fullscreen__color`)、履歴が記録する編集なら履歴の項目の名前(`Replace with …`)、Undo / Redo は `Undo / Redo` です([`PlanPerfModel::editLabel()`](../../src/devstudio/model/planperfmodel.cpp#L355))。配置だけの編集(`set_layout`)は数えません。
  - 編集が「後」を待っている間だけ、`get_status` を 2 秒ごと(最長 30 秒)に読み、その世代の plan をどの panel も読んでいなければ `get_frame_plan` を 1 回送ります。
- **使われない出力と binding**: plan の `diagnostics` の `unread_output` と `unused_binding`(engine の ✅PO-1。§13.5 の「計画を詰める手がかり」)を、その pass を作った root の node の帯に `output unread` / `binding N unused` で出し、node を選ぶと右の欄に 1 文ずつ出します(`pelican.graphCanvasPlanNotes`。`<resource> is written but nothing reads it this frame — the pass may be removable` / `binding N (<resource>) is declared but the shader never reads it`。[`GraphCanvasModel::planNotes()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6346))。ループの回の資源は `<loop>/<要素>/` を外した名前で 1 回だけ言います。この 2 種は canvas の上の `▸ N diagnostics` の一覧には入りません。
- Graph タブの **Add** のボタンは、Add loop と同じく毎 tick 編集できるかに従います(commit の記録)。
- test: `devstudio_plan_perf_test` の [「UX-6b decision 1 and 2: the status line says the frame's GPU time and the plan's size, and each node its own time」](../../test/devstudio_plan_perf_test.cpp#L67)、[「UX-6b decision 3: an edit's before and after, read once its generation has 60 frames」](../../test/devstudio_plan_perf_test.cpp#L134)、[「UX-6b decision 2 (engine ST-1): get_status's node_averages are each node's time, in place of summing every sample of the window」](../../test/devstudio_plan_perf_test.cpp#L211)、`devstudio_graphcanvas_test` の [「UX-6b unread outputs and unused bindings are said on the graph node that made the pass」](../../test/devstudio_graphcanvas_test.cpp#L3240)、`devstudio_graphcanvas_scene_test` の [「UX-6b a root node's band carries its unread output and its GPU time」](../../test/devstudio_graphcanvas_scene_test.cpp#L532)。GPU シナリオは `gui_build_optimize`(下の「Studio の自動化面」)です。

**Frame Plan の表の行から開く(✅GL-2b)**: Frame Plan の Passes / Resources / Physical plan / Decisions / Backends / Barriers のタブで行を右クリックすると、**Open in Graph** が出ます([右クリックの所](../../src/devstudio/view/frameplanwidget.cpp#L3197))。行き先は上の「Frame Plan の Open in Graph は全 pass に出ます」と同じです。

- **行が名指す pass**: Passes はその pass、Resources は `from_nodes`(無い古い plan では書き手と読み手)、Physical plan は alias group の `from_nodes`・候補の組(pass の組か、resource の組ならその `from_nodes`)・scope の `nodes`・lowering の node の元・依存の両端、Decisions は `from_nodes`(subject の見出しの行は、その下の決定の和)、Backends は候補の `from_nodes` と拒否の `subject`、Barriers は両端です。plan に在る pass だけを残し(anchor は外します)、行が何も持たなければ親の行のものを使います。
- 名指す pass が 1 つなら項目 1 つ、2 つ以上なら **Open in Graph** の下に pass の名前の submenu です。1 つも無ければ押せない項目になり、tooltip は `This row names no pass: it is about the whole graph.` です。
- 名前の付いた action `pelican.framePlan.openRowInGraph` は、今のタブで選んだ行の最初の pass を開きます([action](../../src/devstudio/view/frameplanwidget.cpp#L1362))。行が pass を名指さなければ、status 行が `Select a row that names a pass first.` を言います。
- 各タブにも action `pelican.framePlan.tab.<タブの id>`(Barriers なら `pelican.framePlan.tab.barriers`)が付き、発火するとそのタブを前に出します。表にも名前が付きます(`pelican.framePlanPasses` / `pelican.framePlanResources` / `pelican.framePlanPhysical` / `pelican.framePlanDecisions` / `pelican.framePlanBackends` / `pelican.framePlanBarriers` / `pelican.framePlanMaterials`)。
- シナリオ `graph_layers_origin` は Barriers の 20 行目と 9 行目を開きます。この番号は preset の pass の並びに依ります(実装者の報告)。

**root の node を fold group にまとめる(✅GL-3a)**: Graph タブの root で node を選び、canvas の右クリックの **Fold into group…**(`pelican.graphCanvas.foldIntoGroup`)を押すと、選んだ node を 1 つの fold group として root の文書の `fold_groups` に書きます。送るのは `fold_nodes` 1 回で、決定は空(`{kind: "decisions", decisions: {}}`)です([`GraphCanvasFoldController::foldSelection()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L708))。engine の契約は §13.5 の「断片の決定を宣言する(✅FG-1)」、設計は [design_graph_layers](../design_graph_layers.md) §3.4〜§3.6 と §7 の GL-3 です。

- **名前は Studio が付け、問いません**: 選んだ node にループがあれば `<ループ>_fold`(使われていれば `_2`…)、無ければ `fold_1`、`fold_2`… の空き名です([`suggestGraphFoldGroupName()`](../../src/devstudio/model/graphfoldgroups.cpp#L165))。`nodes` は選んだ順ではなく文書の順です。notice は `Folded <node>, … into <group> (not verified: it runs as it is).` です。
- **選べない node**: 項目は本物の root(definition の中や畳んだ中ではない)で、engine の接続が `fold_nodes` を送れるときだけ出ます。ループのゾーンの中の node か、畳んだ node(feature や rendering 節の pass)を選んでいると項目は押せず、その下に理由が 1 行出ます(`<node> is inside a loop's zone (<definition>): fold the loop itself.` / `<node> is a folded feature or a pass of the rendering section, not a node of the graph.`。何も選んでいなければ `Pick the root nodes to fold first.`。[`graphFoldPickRefusal()`](../../src/devstudio/model/graphfoldgroups.cpp#L242))。
- **見え方**([`GraphCanvasFoldController::redraw()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L1030)): group の node を薄い色の角丸の枠(`graph_fold_group#<group>`)で囲み、上辺に見出しの帯(`graph_fold_heading#<group>`)を描きます。帯には名前、状態の字(`graph_fold_state#<group>`)、✅GL-4a の **Verify**、右端に **Fold** / **Open**(`graph_fold_toggle#<group>`)が並びます。node を動かすと枠も追います。
- **畳む・開くは見え方だけです**: **Fold** で group を 1 つの畳んだ node(`graph_node#fold:<group>`。型の所は `fold group`)にし、枠をまたぐ線ごとに `<node>.<port>` の行(`graph_derived_socket#fold:<group>/<node>.<port>`)を出して、外の node から行へ破線を引きます。**Open** か、畳んだ node のダブルクリックで戻ります。開閉はセッションの間の状態で、文書の `fold_groups` は変わりません。
- **状態の字**: Graph タブは文書を読むたびに、digest が変わっていれば `get_fold_groups` を読み、group があれば `get_render_documents` で root だけを読んで決定を重ねます(`get_fold_groups` は決定を返さないため。[`GraphCanvasFoldController::follow()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L468))。
  - `unverified`: 灰色の `not verified — decisions are not applied yet`
  - `stale`: 橙の印 1 つと `changed since verified — runs the open form`。tooltip に verified digest と current digest
  - `verified`: 字も印も出しません(設計 §4 の規則 5)
  - plan の diagnostics の `fold_unverified` / `fold_stale` はこの見出しで言うので、canvas の上の `▸ N diagnostics` の一覧からは外します([fold の診断と node の注を一覧から外す所](../../src/devstudio/view/graphcanvaswidget.cpp#L6958))。
- **group を選ぶ**: 見出しの帯か畳んだ node を押すと、canvas の選択を外して group を選び、枠が太くなります。右の欄(pass の欄の場所)に Decisions の節(`pelican.graphCanvasFold`)が出ます。canvas の node を選ぶと group の選択は外れます。
- **Decisions の節**([`GraphCanvasFoldFields::rebuild()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L168)): 見出し `Decisions · <group>`、✅GL-4a の **Verify** と **Quick check**、1 行の注(`pelican.graphCanvasFold.note`。`Decisions are requests: the engine applies them once the group is verified (Verify), and leaves them out while its nodes differ from what was verified.`)、状態の 1 行(`pelican.graphCanvasFold.state`。verified なら `verified: the engine applies them`)。その下に **Serial** / **Isolate** / **No alias** の一覧が並びます。
  - 一覧の行は主語を `node`、`node [要素]`、`node.port` の形で出し(`pelican.graphCanvasFold.<field>.row<i>`)、×(`.remove<i>`)で消します。
  - 一覧の下の Add の行は選ぶだけで、打つ欄はありません([`graphFoldNodeChoices()`](../../src/devstudio/model/graphfoldgroups.cpp#L199)): node の combo(`pelican.graphCanvasFold.<field>.node`)、node がループなら要素の combo(`.element`。`All elements` と 0…回の数−1。回の数は文書の Count か engine の報告)、No alias で書き手の node を選んだときは port の combo(`.port`)、**Add**(`.add`)。
  - Serial / Isolate で選べる node は group の全部です。No alias では `render.image` / `render.buffer` / `graph.loop` / `graph.instance` と、`render.*` の node のうち `render.image` / `render.buffer` / `render.target_ref` からつながる入力を持つもの(port を選ぶ)です。選べる node が無ければ Add は押せません。
  - 足す・消すたびに、同じ `nodes` と新しい `folded` で `fold_nodes` を 1 回送ります(notice は `<group>: decisions saved (not verified — decisions are not applied yet).`)。engine は書き直した group の印を消すので(§13.5 の FG-1 の節)、検査済みの group の決定を変えると `unverified` に戻ります。
- **見出しの右クリック**([`GraphCanvasFoldController::groupMenu()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L1401)): **Add selected nodes**(`pelican.graphCanvas.foldAddSelected`。canvas で選んでいる root の node を足す)、**Remove node…**(node の submenu。外した node についての決定も一緒に消します。node が 1 つの group では押せません)、**Fold / Open**(`pelican.graphCanvas.toggleFoldGroup`)、**Unfold (delete group)**(`pelican.graphCanvas.unfoldGroup`)。Unfold は確認(`pelican.graphCanvasFold.unfoldConfirm`。既定のボタンは Cancel)の後に `folded: null` を送り、group と決定を消します。node は残ります。
- **送り方**: fold の編集は同時に 1 つで、送信中は `Wait for the current change to finish.` を言います。CAS で `external_modification` が返れば、文書を読み直してから同じ操作を 1 回だけ送り直します([`GraphCanvasFoldController::refused()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L686))。commit の後は Graph タブのほかの render の編集と同じく、digest をほかの panel に知らせ、`restart_required` なら起動し直し、そうでなければ Frame Plan に plan を読み直させて文書を読み直します([`foldEditCommitted()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6375))。
- **効いた決定の見え方**: engine は検査済みの group の決定だけを効かせ、plan の `fold_decisions` に出します。Graph タブは、決定が当たった node の帯(上の「下の層を node で見る(✅GL-2b)」)に `isolate (fold <group>)` の形で足します([`GraphCanvasModel::foldDecisionParts()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6179))。node の決定はその node が作った pass に、資源の決定はその資源を出力として渡す node に付けます。Frame Plan の Decisions のタブには 5 列目 **From** が増え、`from_fold` のある決定に `fold <group>` を出します([Decisions の From の列](../../src/devstudio/view/frameplanwidget.cpp#L4464))。
- **拒否の文**(inspectormodel の 1 文の表。[fold の拒否の 1 文の表](../../src/devstudio/model/inspectormodel.cpp#L1178)): `graph.fold_decision_conflict@1` は `<group> asks <field> for <name>, but the graph-wide setting says otherwise`、`graph.fold_subject_outside@1` は `Fold group <group> names <subject>, which is not one of its nodes`(subject は Decisions の行と同じ `node [要素].port` の形)、`graph.fold_kind_unsupported@1` は `Fold group <group> is folded as <kind>; only decisions can be declared for now` です。
- **戻す**: `fold_nodes` は render の編集の履歴(下の「render の編集を戻す」)に `Fold group <group>` / `Unfold <group>` の名前で積まれます。fold の編集を履歴から戻す test はありません(未確認)。
- test: `devstudio_graph_fold_test`(Qt を使わない)の [「GL-3a decision 1: Fold into group takes root nodes, proposes a free name and sends a group with no decisions」](../../test/devstudio_graph_fold_test.cpp#L44)・[「GL-3a decision 3: get_fold_groups' state reads as the heading's words, and the root's decisions are laid over it」](../../test/devstudio_graph_fold_test.cpp#L79)・[「GL-3a decision 4: the Add row offers the group's nodes, a loop's elements and a writer's ports, nothing typed」](../../test/devstudio_graph_fold_test.cpp#L127)・[「GL-3a decision 6: a decision a fold group made is marked on the Frame Plan's row and on the node's band」](../../test/devstudio_graph_fold_test.cpp#L162)・[「GL-3a decision 7: the fold refusals read as sentences」](../../test/devstudio_graph_fold_test.cpp#L242)と、`devstudio_graph_fold_scene_test` の [「GL-3a the Graph tab folds picked root nodes into a group, says it is not verified, writes its decisions, folds it as it is looked at, and unfolds it」](../../test/devstudio_graph_fold_scene_test.cpp#L170)。GPU シナリオは `graph_fold_decisions`(下の「Studio の自動化面」)です。

**fold group を検査する —— Verify(✅GL-4a)**: 宣言した group の決定は、engine の `verify_fold` が印を書くまで効きません(§13.5 の「断片を検査して印を付ける —— `verify_fold`(✅FG-2)」)。Graph タブは見出しと Decisions の節から `verify_fold` を送ります([`GraphCanvasFoldController::verify()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L903))。

- **押す所**: 見出しの **Verify**(`graph_fold_verify#<group>`)と、Decisions の節の **Verify**(`pelican.graphCanvasFold.verify`)です。印のある(`verified`)group と、`verify_fold` を送れない接続では出ません(節のボタンは押せません)。`stale` の group では **Verify again** になり、見出しのボタンの縁が橙で、tooltip が `… (a loop's Count is part of what is compared) …` を言います。
- **Quick check**(`pelican.graphCanvasFold.quickCheck`): 外していれば `frames` と `warmup_seconds` を送らず、engine の既定(3 frame、30 s)です。付けると `frames: 2`・`warmup_seconds: 1` を送ります。`tolerance` は送りません(0 = byte の一致)([`graphFoldVerifyParams()`](../../src/devstudio/model/graphfoldgroups.cpp#L255))。tooltip は、NVIDIA の driver が最適化した shader に差し替える前に比べるので、まだ動く絵は unstable と読まれうることを言います。見出しの Verify も節の Quick check を使います。
- **走っている間**: 見出しの字が `verifying… (n s)` で 1 秒ごとに数え、Verify は出ません(節のボタンもその字で押せません)。`get_edit_result` を 500 ms ごとに問います。同時に検査できるのは 1 つで、別の group の Verify は `<検査中の group> is being verified; verify <group> after it.` で断ります。検査中の group への fold の編集は送らず、notice が `<group> is being verified; edits wait until it finishes` を言います。その group の node を選ぶと、右の欄(Loop・Draw・View・image・pass・Place の欄とラベル)が灰色になり、同じ文が選ぶたびに 1 回出ます([`GraphCanvasFoldController::gateNodeFields()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L1008))。ほかの group とほかの node は編集できます。
- **答え**([`readGraphFoldVerifyOutcome()`](../../src/devstudio/model/graphfoldgroups.cpp#L278)):
  - **通った**(committed): notice が `<group> verified — its decisions now apply` を言い、fold の commit と同じく plan と文書を読み直します。見出しの字と印が消え、Decisions の各行の下に `applied → <展開名>`(`.applied<i>`)が出ます。当たる展開名が無い主語(Count を減らして無くなった回など)は灰色の `no longer matches anything` です([`graphFoldAppliedNames()`](../../src/devstudio/model/graphfoldgroups.cpp#L367))。
  - **`graph.fold_mismatch@1`**: 見出しに赤の印と 1 文(`frame F differs in N pixels` / `N validation errors (open form: M)` / `the decided form exited with E`)。notice は `<group> is not verified: <文> — its decisions still do not apply.` です。
  - **`graph.fold_verify_unstable@1`**: 見出しに黄の印と `the open form itself is not stable (frame F, N pixels) — not the decisions' fault`。notice は Quick check を外して(warm-up を長くして)もう一度、を言います。
  - 赤と黄の印は、その group の digest が検査したときのままで、`verified` でない間だけ残ります。
  - `external_modification`: `<group>: the graph changed while verifying; verify again.`。`fold_verify_failed` / `fold_verify_unavailable` / `fold_verify_busy` は inspectormodel の 1 文の表の文(`<group> could not be verified: its open form could not be drawn (<run> run)` / `This engine cannot verify a fold group now (<reason>)` / `<group> is being verified already; wait for it to finish`)です。どれも印は付けません。
- **戻す**: `verify_fold` も render の編集の履歴に `Verify <group>` で積まれる method の表に入りました(`running` の間は 500 ms ごとに問います)。履歴から戻す test はありません(未確認)。
- 子 process が描くのは project の既定の scene で、Studio が開いている scene ではありません(§13.5 の FG-2 の「残した穴」)。Quick check と既定のそれぞれで何秒かかるかは、この文書では確かめていません(未確認)。
- test: `devstudio_graph_fold_test` の [「GL-4a decision 1: Verify sends the engine's defaults, or 2 frames after 1 s with Quick check, never a tolerance」](../../test/devstudio_graph_fold_test.cpp#L296)・[「GL-4a decision 3: what a finished verify_fold says, read from get_edit_result」](../../test/devstudio_graph_fold_test.cpp#L307)・[「GL-4a decision 5: a verified decision names the expanded names it landed on, or nothing once its subject matches nothing」](../../test/devstudio_graph_fold_test.cpp#L372)と、`devstudio_graph_fold_scene_test` の [「GL-4a Verify from a group's heading runs verify_fold, holds the group's edits while it runs, and marks what it found; a stale group offers Verify again」](../../test/devstudio_graph_fold_scene_test.cpp#L304)。mismatch と unstable は手で作った `get_edit_result` の JSON で見ています。

**Frame Plan から graph 全体の決定を書く(✅GL-3a)**: Frame Plan の **Passes** と **Resources** のタブで行を右クリックすると、Open in Graph の上に、pass の行なら **Serial** / **Isolate**、資源の行なら **No alias** の check が出ます(`pelican.framePlan.planning.serial` / `.isolate` / `.noAlias`。[`addPlanningEntries()`](../../src/devstudio/view/frameplanwidget.cpp#L3236))。engine の契約は §13.5 の「graph 全体の計画の決定を書く(✅EL-2)」です。

- check の状態は root の rendering 節の `target_planning.graphs.<plan の graph>` です。Frame Plan は authoring context を読むたびに、`get_render_documents` で root を読みます([`requestPlanning()`](../../src/devstudio/view/frameplanwidget.cpp#L3387))。
- 押すと `edit_target_planning` を 1 回送ります。付けるときはその field を `true`、外すときは `null`(field を消す。`false` は書きません)です([`togglePlanning()`](../../src/devstudio/view/frameplanwidget.cpp#L3263))。status 行は `Isolate set on <名前>.` / `Isolate taken off <名前>.` の形です。
- ループの回の展開名(plan の `loop_origin` を持つ行)には check を出さず、押せない 1 行 `Fold the loop to set this per element` を出します(`pelican.framePlan.planning.loopLine`。engine の LP-1a が展開名を断るため。回ごとの決定は上の fold group で書きます)。authoring context をまだ読んでいない間と、Frame Plan の編集が送信中の間は押せません。
- preset が `target_planning` を持つ root での `rendering_key_not_allowed_here` は、`The pipeline preset declares target_planning, which this project cannot override: fork the preset to set it (rendering_key_not_allowed_here).` の 1 文です([preset が target_planning を持つときの文](../../src/devstudio/view/frameplanwidget.cpp#L2967))。
- Physical plan と Decisions のタブの行には出ません。`vulkan_plan_pins` と `vulkan_physical_fragments` は、✅UX-15b から Backends と Physical plan のタブで書きます(次の段落)。
- 戻す: render の編集の履歴に `Edit planning decisions` の名前で積まれます(戻す道の test は無く、未確認)。
- test: `devstudio_graph_fold_test` の [「GL-3a decision 8: the Frame Plan's Serial / Isolate / No alias read the root's target_planning and write edit_target_planning」](../../test/devstudio_graph_fold_test.cpp#L266)(読みと params の形。Frame Plan の右クリックは GPU シナリオ `graph_fold_decisions` の最後で通します)。

**下の層の決定を Frame Plan から書く —— backend の Pin / Unpin、物理の計画の Freeze / Remove / Eject again、常駐 BLAS、置き換えの来歴(✅UX-15b、`5c5110ef`)**: engine の ✅EL-2 の `edit_vulkan_plan_pins` と `put_vulkan_physical_fragment`(§13.5 の「graph 全体の計画の決定を書く(✅EL-2)」)を、Frame Plan の Backends と Physical plan のタブから送ります。UX-15b の前は、`ejectable_pin_package` / `ejectable_physical_fragment` を写して root の文書に貼る(第6章 §6.6)か、RPC を手で送るしかありませんでした。Raw JSON のタブは読むだけのままです。

- **Backends の Pin this candidate**: Selected か Feasible の候補の行の右クリックに **Pin this candidate**(`pelican.framePlan.backend.pinEntry`)が出ます([右クリックに足す所 `addLowerLayerEntries()`](../../src/devstudio/view/frameplanwidget.cpp#L3344))。plan の `ejectable_pin_package` の `pins.backend_candidate` だけをその候補に替えて、`edit_vulkan_plan_pins {base_source_digest, family, pin}` を 1 回送ります([`framePlanBackendPinRequest()`](../../src/devstudio/view/frameplanedit.cpp#L2183))。status 行は `Pinned <candidate>.` です。
  - 送れないときは項目が押せず、tooltip が理由を言います: `<candidate> was rejected by the planner: only a Selected or Feasible candidate can be pinned.`、`<candidate> is pinned already.`、`The plan ejects no pin package to pin a candidate with.`、authoring context の前は `The render authoring context has not arrived yet.`。送信中も押せません。
  - 名前の付いた action `pelican.framePlan.backend.pin` は Backends のタブで選んだ行に効きます(行を選んでいなければ status 行が `Pick a candidate row on the Backends tab first.`)。action はいつも押せる状態で、送れない理由は status 行に出ます(実装者の報告: 共有の action を無効にすると、名前での起動が黙って効かなくなるため。右クリックの項目は別の一時的な action です)。
  - 候補の行の tooltip は `Right-click: Pin this candidate, so the planner keeps it.` です。pin された候補の行の Status の列は `Selected, pinned`(Feasible の候補なら `Feasible, pinned`)になります。
- **Backends の帯**(`pelican.framePlan.backend.state`。木の先頭の、列をまたぐ行です。[`addBackendBand()`](../../src/devstudio/view/frameplanwidget.cpp#L4526)): plan に `applied_pin_package` があれば `Pinned: <candidate>` と **Unpin**(`pelican.framePlan.backend.unpinButton`。action は `pelican.framePlan.backend.unpin`)、無ければ `Not pinned: the planner picks the candidate. Right-click a Selected or Feasible row to pin it.` です。Unpin は `pin: null` に plan の `graph` を付けて送り、status 行は `Unpinned <candidate>: the planner picks the candidate again.` です。pin がある間は候補の行の右クリックにも Unpin が出ます。
- **Physical plan の帯**(`pelican.framePlan.physical.state`。[`addPhysicalBand()`](../../src/devstudio/view/frameplanwidget.cpp#L4857)): 凍結していなければ `Not frozen: the planner decides the physical plan each compile.` と **Freeze this physical plan**(`pelican.framePlan.physical.freezeButton`。action は `pelican.framePlan.physical.freeze`)、plan に `applied_physical_fragment` があれば `Frozen: every compile keeps the physical plan the document holds.` と **Remove**(`pelican.framePlan.physical.removeButton`。action は `pelican.framePlan.physical.remove`)です。
  - Freeze は plan の `ejectable_physical_fragment` を手を加えずに `put_vulkan_physical_fragment {base_source_digest, family, fragment}` で送ります([`framePlanPhysicalFreezeRequest()`](../../src/devstudio/view/frameplanedit.cpp#L2217))。status 行は `Physical plan frozen.`(既に凍結していれば `Physical plan frozen again from the current plan.`)です。Remove は `fragment: null` に `graph` を付け、`The frozen physical plan was removed.` です。
  - 帯を表の上の別の widget にせず木の先頭の行に置いたのは、タブの page が木でなくなると、`currentWidget()` を木として読む既存の操作(Open in Graph など)が効かなくなるためです(実装者の報告)。帯は pin や凍結が無いときも案内として出ます。
- **`physical_fragment_stale` と Eject again**: commit の組み直しで fragment の fingerprint が合わないと(§13.5 の EL-2 の段落)、status 行が `The frozen plan no longer matches the graph (<fingerprint>); Eject again (physical_fragment_stale).` を言います(`<fingerprint>` は `logical_graph` か `automatic_plan`。文は inspectormodel の拒否の 1 文の表に足しました。[`physical_fragment_stale` の 1 文](../../src/devstudio/model/inspectormodel.cpp#L1223))。status 行の横に **Eject again**(`pelican.framePlan.physical.ejectAgainButton`。action は `pelican.framePlan.physical.ejectAgain`)が出て、押すと画面の plan の `ejectable_physical_fragment` で置き換える 1 回の編集を送ります(Freeze と同じ要求です)。ボタンは次の送信か、接続する engine が替わったときに隠れます。
  - status 行は 90 文字で切るので、この 1 文(約 100 文字)は末尾が切れます。全文は tooltip と Copy message にあります(実装者の報告)。
- **family**: `get_frame_plan` は plan の family を言わないので、graph の名前が `#xr` で終われば `xr`、それ以外は `flat` と推定して送ります([`framePlanPackageFamily()`](../../src/devstudio/view/frameplanedit.cpp#L2145))。`preview` は送りません。
- **送り口と履歴**: Frame Plan の編集の送り口(`FramePlanEditCapability`)に `editVulkanPlanPins` / `putVulkanPhysicalFragment` の 2 本を足しました([足した所](../../src/devstudio/view/frameplaneditcapability.cpp#L52))。どちらも render の編集の履歴に積まれ、Undo 1 回で戻ります。項目の名前は `Pin backend <candidate>` / `Unpin backend` / `Freeze physical plan` / `Remove frozen physical plan` です([pin と凍結の項目の名前](../../src/devstudio/model/renderedithistory.cpp#L317))。
- **常駐 BLAS**(読むだけ): Physical plan のタブの最後に **Ray query acceleration structures** の節を出します([`populateRayQuery()`](../../src/devstudio/view/frameplanwidget.cpp#L4874))。見出しの行は `Requested` / `Not requested` と `<N> resident BLAS, <M> TLAS instances`、子は Active BLAS・BLAS variants・Named TLAS(名前ごとの instance の数)・Resident BLAS(`resident_blas_keys` の 1 つずつ: geometry kind、`allocation <id>`、`<n> geometries, flags 0x<flags>`)です。`get_frame_plan` の `ray_query_acceleration_structures` が `requested: false` で BLAS も無い project(ray tracing の無い project。Blank もそう)では節を出しません。
  - engine の ray-query の scene は 1 つで、BLAS は全部の TLAS が共有します。TLAS ごとの BLAS の一覧は wire に無いので、named TLAS ごとには instance の数だけを出します(実装者の報告)。
- **置き換えの来歴**(読むだけ。model は `physical_target_plan` の `subgraph_replacements` / `graph_transforms` / `render_strategy` をこの順に読みます。[`readPlanPackages()`](../../src/devstudio/model/frameplanmodel.cpp#L634)):
  - subgraph の置き換えが作った pass は、Passes の行(と Logical graph の詳細)の Plan facts に **Replaced by** の行が付き、行の tooltip が `Replaced by subgraph replacement <region> by <provider> (<implementation>)` を言います([`framePlanReplacedByLines()`](../../src/devstudio/model/frameplanmodel.cpp#L2212))。
  - Physical plan のタブの **Compile rewrites** の節([節を作る所](../../src/devstudio/view/frameplanwidget.cpp#L4671))に 3 種の全部を 1 行ずつ出します(名前、`Subgraph replacement` / `Graph transform` / `Render strategy`、provider、node のある置き換えは `<source の node> -> <置き換えの node>`、ほかは `render strategy <name> by <provider> (<implementation>), graph variant <variant>` の形)。
  - `graph_transforms` と `render_strategy` は node の一覧を持たない(graph 全体を書き換える)ので、pass ごとの来歴は subgraph の置き換えの分だけです。
- 帯と Layers の `is replaced by` の穴(上の「下の層を node で見る(✅GL-2b)」の分かっている穴)も UX-15b で直しました。
- **残した穴**(実装者の報告):
  - Eject again が効くのは、文書の fragment が画面の plan より古い場合(古い plan から Freeze したなど)です。stale の拒否が別の編集(graph の変更など)の commit で起きたときは、画面の plan の ejectable も変更前の graph のものなので、置き換えても同じ stale になりえます。そのときは Remove → 編集 → Freeze です。
  - stale の流れは unit test だけで、GPU シナリオでは fingerprint をずらせないので通していません。Frame Plan 以外のパネル(Graph など)が同じ拒否を受けたときは、1 文は出ますが Eject again のボタンは出ません。
  - `xr` の family で送る道は、推定の関数の test だけです。
  - シナリオは候補を名前 `pelican.vulkan.materialized_plan@1` で選びます(この候補が feasible でない device では選べません)。
  - engine に回したもの(`<作業ディレクトリ>/engine_backlog.md` に記録): `get_frame_plan` に family を明示する、文書の凍結した fragment が今の graph と合わないことを `get_frame_plan` の警告にも出す(編集と取り直しを 1 回で)、`graph_transforms` / `render_strategy` が作った node の一覧、named TLAS ごとの BLAS の対応。
- test: `devstudio_frameplan_graph_test` の [「UX-15b decision 1: Pin this candidate sends the ejected pin with that candidate; a pin the plan applies is a band with Unpin」](../../test/devstudio_frameplan_graph_test.cpp#L12604)・[「UX-15b decision 2: Freeze sends the ejected fragment as it is; Frozen offers Remove; a stale refusal is one sentence beside Eject again」](../../test/devstudio_frameplan_graph_test.cpp#L12678)・[「UX-15b decisions 4 and 5: the resident BLAS only for a ray-query scene, and the rewrite that put a pass in the plan」](../../test/devstudio_frameplan_graph_test.cpp#L12753)、`devstudio_render_history_test` の [「UX-15b a backend pin and a frozen physical plan are recorded, named, and undone」](../../test/devstudio_render_history_test.cpp#L865)、`devstudio_graphcanvas_test` の [「UX-15b a feature every pass of which a graph node replaced says so in the graph's own Layers rows」](../../test/devstudio_graphcanvas_test.cpp#L3203)。GPU シナリオは `gui_lower_layer_pin_freeze`(下の「Studio の自動化面」)です。

**engine の再起動が要る graph の編集(✅GL-1b)**: engine の RS-1(`247ed20e`)は、生きている material の物理の描画の契約を変える graph の編集を断らずに書き、commit の結果に `restart_required: true` と `restart_reason` を付けます。`restart_reason` は `code` と、`detail`(`color_formats`・`depth_format` などの一覧)などを持ちます。

- Studio はこの commit の後に文書も plan も読まず、notice に `Restarting the engine to apply this change (<code>)...` を出して、Frame Plan の再起動と同じ [`MainWindow::requestEngineRestart()`](../../src/devstudio/view/mainwindow.cpp#L2003) に渡します([restart_required を受ける所](../../src/devstudio/model/graphcanvasmodel.cpp#L1380))。未保存の scene の編集があれば、Frame Plan のときと同じ確認(✅G-5)が出ます。✅RSX から、読み方は全パネル共通の 1 つです(次の段落)。
- 確認で Cancel したとき(か、別の問いの最中で見送ったとき)は、notice が `The engine was not restarted. The setting is saved and applies the next time the engine starts.` を言い、走っている engine から文書と plan を読み直します([`GraphCanvasModel::engineRestartDeclined()`](../../src/devstudio/model/graphcanvasmodel.cpp#L777))。
- 読むのは `restart_reason.code` だけです。GL-1b の着地時は `detail` を文字列として読んでいたので、engine が送る一覧で `type must be string, but is array` の例外になり、編集が断られたように出て engine は再起動しませんでした(`43317a76` で直しました。統合側が Release を仮想モニタで操作して見つけました)。
- 再起動の後、文書が同じなら Graph タブの Undo はそのまま効きます(✅OT-1。§13.5 の「シーン編集との違い」)。
- RS-1 より前の engine は、この編集を `changes the physical rendering contract of live material` を含む文で断ります。Studio はその文の後に ` — this change needs an engine restart, which the engine does not offer for graph edits yet` を足し([句を足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1347))、notice の 1 行を切り詰めるときもこの句は残します。

**どのパネルの render の編集も、再起動が要れば同じ形で起動し直します(✅RSX)**: engine は RS-1 から、runtime に届く render config の ticket の完了すべてに、live に載せられないとき `restart_required: true` と `restart_reason` を付けます。RSX の前に Studio がこれを見ていたのは、Graph タブ・Frame Plan・render の編集の履歴(Undo / Redo)の 3 か所だけでした。Render Features は再起動が要る編集の後でも `Applied without restarting (runtime generation N).` を言っていました(engine session の調査)。

- **読むのは 1 か所です**([`readRenderEditCompletion()`](../../src/devstudio/model/rendereditcompletion.cpp#L15)、[`completeRenderEdit()`](../../src/devstudio/model/rendereditcompletion.cpp#L49))。ticket の最後の答え(`get_edit_result` か、送った時点で committed と答えた応答)から、`status`、committed か、`no_change`、`restart_required`、`restart_reason.code`、`published_generation`、`source_digest.hex` を読みます。`restart_reason.detail` は読みません(GL-1b の `43317a76` と同じ理由)。
- `restart_required` が真なら、パネルは文書も plan も読まずに、notice に `Restarting the engine to apply this change (<code>)...`(code が無ければ括弧ごと無し。[`renderEditRestartLine()`](../../src/devstudio/model/rendereditcompletion.cpp#L5))を出し、Graph タブ・Frame Plan と同じ [`MainWindow::requestEngineRestart()`](../../src/devstudio/view/mainwindow.cpp#L2003) に渡します。未保存の scene の編集があれば同じ確認(✅G-5)が出ます。
- この読みを通るのは、Graph タブ(graph の編集と、上の「Graph から画面に出す」の pass の入力の編集。[pass の入力の編集の所](../../src/devstudio/view/graphcanvaswidget.cpp#L6206))、Frame Plan、Pass Editor([受ける所](../../src/devstudio/view/fullscreenpasswidget.cpp#L3082))、Render Features とそこから開く dialog(Frame node order、Task Editor、Fork / Regions、Callable records、Scene AS。[共通の受け口](../../src/devstudio/view/renderfeatureswidget.cpp#L116))、Inspector のグラフ装着([受ける所](../../src/devstudio/model/graphattachmentmodel.cpp#L239))、下の Rendering settings タブ、render の編集の履歴の Undo / Redo と戻し([`MainWindow::renderDocumentsRestored()`](../../src/devstudio/view/mainwindow.cpp#L1217))です。各パネルの restart の action は MainWindow が渡します([restart の action を渡す所](../../src/devstudio/view/mainwindow.cpp#L623))。
- Render Features の `Applied without restarting (runtime generation N).` は、再起動しないときだけ出ます([再起動しない Apply の 1 文を出す所](../../src/devstudio/view/renderfeatureswidget.cpp#L4143))。
- 再起動が要るかを決めるのは engine です。material の物理の描画の契約が変わる編集は、その場で組み直さず、今までどおり engine を起動し直します(2026-10-01 の利用者の決定)。
- test: `devstudio_rendering_settings_test` の [「RSX decision 1: a render edit that commits with restart_required restarts the engine through the one reading, and Render features does not say it applied live」](../../test/devstudio_rendering_settings_test.cpp#L253)。

**Graph から画面に出す(✅GL-1e)**: Graph タブの root で、rendering 節の fullscreen pass(Blank pipeline の `present` など)は入力のソケットを出し、graph の node の出力の線を受けます。落とすと、Frame Plan のソケットへの drop と同じ `edit_preset_pass` を送ります(設計は [design_graph_layers](../design_graph_layers.md) §7 の GL-1e)。これで、Blank の project に palette で足した material pass の絵を、Graph タブだけで画面に出せます。

- **線を受ける pass**([`GraphCanvasScene::passInputSockets()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4219)): 次を全部満たす pass です。
  - 本物の root を見ている(definition の中や畳んだ中ではない)。
  - plan の `source` が `project` で、feature の pass ではなく、その pass 自身の畳んだ node で描かれている(graph の node が作った pass ではない)。
  - `get_render_authoring_context` の行が `editable_root_pass`(root が自分で宣言した pass。上の「direct root の pass(✅F-6)」)で、宣言の `type` が `fullscreen`。
  - preset と feature の pass、fullscreen でない pass は今までどおり読むだけです(preset と feature の pass は fork が要るため。設計の決定)。
- **読むもの**: Graph タブは plan を読むたびに、rendering 節の pass(`source` が `project`、feature の外、anchor でない、graph の node が作っていない pass)があれば、`get_render_authoring_context` を 1 回読みます([`GraphCanvasModel::requestRenderAuthoring()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5947))。これを持たない engine では、pass は線を受けません(何も言いません)。
- **ソケット**: その pass の畳んだ node は、読む資源の行の代わりに、`input` の項目ごとのソケットを出します([描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1448))。並びと字は、Frame Plan の「ソケットは shader の宣言から出ます —— 番号でつなぐ(✅VI-3a)」と同じ関数で組みます。shader が入力を足せる(可変の組がある)ときは、最後に空いた口 `+ input` が出ます。`engine://passthrough` の `present` が何も読んでいなければ、`+ input` だけです。
  - plan が shader の宣言を出さない pass では、`input` の項目をその順に並べ、`+ input` は出しません。
  - 自動操作の名前は `graph_derived_socket#<pass>/<port>` です。`<port>` は、つながっている口では資源の名前、空いた固定の口では `in@<binding>`、`+ input` では `in:+` です。
  - tooltip は、`+ input` が `Drop an image here: <pass> reads one more input`、つながっている口が `<resource>: input <i> of <pass>; drop another image here to read that instead` です。
- **落とす**([`GraphCanvasScene::dropOnPassInput()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4308)): root の node の出力をソケットへ drag して落とします。
  - 落とした出力から資源の名前を決めます([`GraphCanvasModel::outputResource()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5985))。`render.image` / `render.buffer` / `render.attribute` の `resource` はその id、`render.target_ref` は `parameters.name`、render node の出力は同じ名前の入力につながった資源です(8 段まで辿ります)。決まらなければ送らず、notice が `<node>.<port> is not an image or a buffer <pass> can read.` を言います。
  - 送る params は、Frame Plan と同じ [`framePlanSocketConnectRequest()`](../../src/devstudio/view/frameplanedit.cpp#L783) が作ります([呼ぶ所](../../src/devstudio/view/graphcanvaswidget.cpp#L4262))。第二の書き方はありません。つながっている口に落とせばその位置の入力を置き換え、`+ input` に落とせば末尾に足します。空いた固定の口・飛ばした binding・max まで埋まった可変の組の扱いと断りの文も、Frame Plan の表と同じです。
  - `81b62382` から、pass が既に読んでいる image を可変の組の口(`+ input`)へ落とすと、何も送らずに notice が `<pass> already reads <image>.` を言います([断る所](../../src/devstudio/view/frameplanedit.cpp#L834))。それまでは同じ入力を 2 つ書いて送り、engine が `duplicate_input` で断っていました。要求を作る関数は Frame Plan と共有なので、Frame Plan の drop も同じです。test は `devstudio_frameplan_graph_test` の [「VI-3a a drop connects by number: at the next binding, in place, or not at all with the line that says why」](../../test/devstudio_frameplan_graph_test.cpp#L7435) に足した 1 節です。
  - 送るのは Graph タブ自身です([`sendPassEdit()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6046)。GL-1e の着地時の名前は `sendPassInputEdit()` で、✅GL-5a で詳細欄の `edit_preset_pass` と `edit_target_planning` も運ぶ形に広げて改名しました)。送った直後の notice は `<pass>: saving its inputs...`、commit の後は `Pass inputs saved.` です(要求が別の 1 行を持つときはその 1 行。下の「外す」で後ろの入力が詰まるときなど)。
  - commit の後は、ほかのパネルに新しい digest を知らせ、Frame Plan に plan を読み直させ、自分も文書・plan・authoring context を読み直します([`passEditCommitted()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6195))。
  - engine が `external_modification`(読んだ後に文書が変わった)で断ると、notice に `The document changed since the Graph read it; reading it again...` を出して読み直し、読んだ context から同じ操作の params を作り直して 1 回だけ送り直します([`refusePassEdit()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6147))。
  - 前の編集の返事を待っている間は、送らずに `Wait for the current change to finish.` を言います。
- **線**: pass の `input` の項目ごとに実線を 1 本引きます(`graph_pass_input_edge#<from>-><pass>`。[線を組む所](../../src/devstudio/view/graphcanvaswidget.cpp#L1547))。tooltip は `<pass> reads <resource> (input <i>). Delete takes it out.` です。同じ pass と資源の派生の破線は引きません([外す所](../../src/devstudio/view/graphcanvaswidget.cpp#L2210))。
  - 線の元は次の順で決めます。
    1. ✅GL-1f から: plan の順でその pass より前にその資源を書いた pass のうち、canvas の node の出力がその資源に解決される最後のもの(material pass の `color` など。[決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L1575))。下の「pass の出力のソケット(✅GL-1f)」です。
    2. その資源を名乗る root の node(`render.image` などの `resource`、`render.target_ref`)。
    3. 派生の線の元(畳んだ node なら資源の行、graph の node なら見出し)。
- **外す**: 線を click で選び(太く、選んだ色になります)、Delete(か Backspace)を押すか、右クリックの **Delete**(`pelican.graphCanvas.deletePassInput`)を選ぶと、その 1 項目を外した `edit_preset_pass` を送ります([`GraphCanvasScene::deletePassInput()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4298))。params は Frame Plan の [`framePlanSocketDisconnectRequest()`](../../src/devstudio/view/frameplanedit.cpp#L875) が作ります。
- **Undo**: この編集は、Frame Plan の編集と同じく render の編集の履歴に積まれます(下の「render の編集を戻す —— Undo / Redo(✅RU-1 / RU-1b)」)。Graph タブの Undo は、Graph タブが最後に送った編集がこの pass の編集である間だけ、その履歴の Undo を呼びます([`GraphCanvasScene::undoEdit()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4163))。graph の編集を送れば、次の Undo は graph の履歴に戻ります。Redo も同じ形です。Graph タブの Undo / Redo のボタンは、どちらかで戻せれば押せます。
  - Graph タブは、自分が送った pass の編集の数だけを数えます。その後に Frame Plan など別のパネルで render の編集をしたときに Graph タブの Undo が何を戻すかは、test では見ていません(未確認)。
  - 履歴の Undo / Redo は、window の Edit ▸ Undo Render Edit / Redo Render Edit の action を通して呼びます([渡す所](../../src/devstudio/view/mainwindow.cpp#L1179))。統合側の報告では、Graph タブの Ctrl+Z が graph の Undo であることは既存の test が固定しています。
- **古い engine の拒否**: RS-3 の前の engine は graph の pass を `present` の後ろに置くので、`Pass input target is not produced as an earlier output` で断ります。Studio はその文の後に ` — the graph pass that draws it runs after this pass` を足し([句](../../src/devstudio/view/graphcanvaswidget.cpp#L4837)、[graph の pass が書き手のときだけ句を足す else の所](../../src/devstudio/view/graphcanvaswidget.cpp#L6138))、notice の 1 行を切り詰めるときもこの句は残します。✅UX-5d から、句を足すのはその image の書き手が graph の node の pass のときだけです(書き手は frame plan で引きます)。feature の task や rendering 節の pass が書き手なら足しません。✅UX-12d から、拒否が engine の FR-3 の `feature.required_target_unwritten@1` なら、notice はその code の 1 文で、この句は足しません(上の「書き手を繋ぎ、1 本の edit_batch で足す(✅UX-12d)」の Graph の notice)。
- **engine の側**: 置き場所の無い graph の pass が、それを読む rendering 節の pass の前に入るのは RS-3 です。「present に graph の出力をつなぐ」が「その出力が解決される image の名前を present の `input` に書く」ことなのは RS-4 の項にあります。どちらも [第6章](06_rendering.md) §6.18(「graph の pass の置き場所」と「グラフで書く pass」)です。
- test: `devstudio_graphcanvas_scene_test` の [「GL-1e a rendering-section pass takes lines: + input, replace, append, the solid line, Delete」](../../test/devstudio_graphcanvas_scene_test.cpp#L6583)(空の present の `+ input` だけ、落とすと送る `edit_preset_pass` が Frame Plan の drop の要求と同じ、置き換えと末尾への追加、実線と破線が並ばないこと、選んで Delete、root が宣言しない pass は線を受けないこと)と [「GL-1e the Graph tab sends a pass's input edit, its Undo reaches the render history, and an engine before RS-3 says why it refused」](../../test/devstudio_graphcanvas_scene_test.cpp#L6733)(送る RPC、古い engine の拒否の句が切り詰めで残ること、`Pass inputs saved.` と読み直し、Undo が render の履歴に届くこと)。GPU シナリオは `graph_from_blank_forward` の後半です(下の「Studio の自動化面」のシナリオ)。
- **分かっている穴**(着地時の記録): 線の Delete と Undo は scene の test だけで、GPU シナリオでは通していません。GPU で見たのは forward の material pass から `present` の `+ input` への 1 本だけです。

**rendering 節の pass を Graph の詳細欄で直す —— Pass Editor と同じ部品と同じ組み立て、Serial / Isolate / No alias(✅GL-5a、`6565221b`)**: 設計 [design_graph_layers](../design_graph_layers.md) §7 の GL-5(Frame Plan を mode に)の 1 本目です。書き先は今のまま(rendering 節の pass は `edit_preset_pass`)で、入口だけを Graph に束ねます。GL-5a の前の畳んだ node は、GL-1e の `present` の入力のソケットを除いて読むだけで、直すには Frame Plan の Edit pass から Pass Editor を開いていました。

- **持ち主で分ける**: 畳んだ node のうち、authoring context が `editable_root_pass` と言う pass(root の rendering 節が自分で宣言した pass。Blank の `present` など)の 1 pass の node だけを、詳細欄で直せる node にします([`GraphCanvasModel::markFoldedOwner()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5812)、[`GraphCanvasModel::renderingSectionPass()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5798))。preset と feature の pass は今どおり読むだけで、tooltip が `Expand to edit…` を言います(上の「何が出るか」の tooltip の段落)。選んだ node の 1 行の要約(`pelican.graphCanvasFoldedSummary`)にも同じ文が出ます。
- **詳細欄**(`pelican.graphCanvasFoldedPass`。[`GraphCanvasFoldedPassFields`](../../src/devstudio/view/graphcanvasfoldedpassfields.hpp#L57)): root の canvas でその node を選ぶと、右の欄に見出し `<pass> (<type>): from the rendering section` と、Pass Editor と同じ部品が出ます。部品は複製せず、Pass Editor の部品をそのまま持ちます。
  - fullscreen pass: **Vertex** / **Fragment**(`pelican.graphCanvasFoldedPass.vertex` / `.fragment`。Pass Editor と同じ shader の一覧から選ぶ combo で、engine の一覧が届けばそれに替わります)、**Inputs**(`.inputs`。読むだけの一覧。変えるのは GL-1e のソケットへの線です)、**Defines**(`ShaderDefinesForm`)、**Frame data**(`FullscreenPassInterfaceRow`)、**Attachments**(`AttachmentOpsForm`。color の Load / Clear / Store と clear colour)。
  - material pass: `MaterialSettingsForm` の 10 個の設定(✅UX-15a の GPU draws を含む)と、depth を含む `AttachmentOpsForm`。material pass の routing は出しません(Pass Editor も root の pass では routing を書きません。実装者の報告)。
  - fullscreen と material 以外の type の root の pass には欄を出しません。GL-5a の着地時は出力の target の付け替えもありませんでした(実装者の報告)。✅GL-5b から color 出力の行があります(下の GL-5b の段落)。
- **送り方**: 選ぶ欄は選んだときに、打つ欄(define、clear colour、material の設定)は打つ手が 500 ms 止まってから、`edit_preset_pass` を 1 回送ります。params は Pass Editor の Save と同じ [`presetPassEditParams()`](../../src/devstudio/view/presetpassedit.cpp#L12) が組みます(Pass Editor の `requestEditPresetPass()` の本体を切り出したもので、Frame Plan の Edit pass が開く Pass Editor も同じ関数です。[Pass Editor が呼ぶ所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2500))。読み込んだときと値が変わっていなければ送りません。
  - 送るのは Graph タブの GL-1e の口を広げた `sendPassEdit()` で、1 つずつ・ticket を待ち・render の編集の履歴に積みます。Graph タブの Undo 1 回で戻ります(上の GL-1e の「Undo」と同じ)。notice は送った直後が `<pass>: saving...`、commit の後が `<pass> saved.` です。
  - 値を作れないとき(engine が断る define、まだ色になっていない clear colour など)は送らず、notice が `<pass>: <理由>` を言います。拒否は GL-1e と同じく inspectormodel の 1 文の表で notice に出ます。
  - Pass Editor と同じく、編集した root の pass には `"ray_query": false` が書き足されます(実装者の報告。Pass Editor の Save と同じ動き)。
- **Serial / Isolate / No alias**: 詳細欄の **Decisions** に、pass の **Serial**(`.serial`)・**Isolate**(`.isolate`)と、pass が書く render target ごとの **No alias: <target>**(`.noAlias.<target>`)の check があります。押すと Frame Plan の右クリック(✅GL-3a)と同じ `graphTargetPlanningParams()` で組んだ `edit_target_planning` を 1 回送り、notice は `Serial set on <pass>.` / `Serial taken off <pass>.` の形です([`GraphCanvasFoldedPassFields::togglePlanning()`](../../src/devstudio/view/graphcanvasfoldedpassfields.cpp#L546))。check の状態は `get_render_documents` で読んだ root の `target_planning` で、commit か拒否の後に読み直します。present の画面(display)には No alias を出しません。送る口は `GraphCanvasReadCapability` に足した `requestEditTargetPlanning` です。
- **作らなかったもの**: 仕様の決定 3(rendering 節の compute task を Task Editor の部品で直す)は作っていません。root の rendering 節は `compute_tasks` を宣言できない(MG-2b)ので、畳んだ node になる task は全部 preset か feature のもので、読むだけだからです(実装者の報告。広げるかは裁定待ち)。GL-5b(Add fullscreen pass / Add compute task / Add material pass / New render target を palette と同じ関数に。詳細欄の出力の target の付け替えもここ)は ✅GL-5b(下の段落。Add は束ねていません)、GL-5c(Frame Plan の Edit pass / Edit task を Graph への委譲に)は GL-5a の着地時は残っていました。✅GL-5c で、Frame Plan の Edit pass と行のダブルクリックがこの詳細欄を開くようになりました(下の GL-5c の段落)。
- **手で打つ欄**: 新しくは足していません。詳細欄は Pass Editor の部品を持つので、部品が元から持つ打つ欄(define の名前と値、clear colour、material の出力の名前と `+ new tag`)が Graph にも出ます。
- test: `devstudio_graphcanvas_scene_test` の [「GL-5a a rendering-section pass picked on the Graph is edited in the detail column: a shader pick is one edit_preset_pass with Undo, Serial one edit_target_planning, and a refusal reads as the inspector's sentence」](../../test/devstudio_graphcanvas_scene_test.cpp#L6860)、`devstudio_fullscreen_pass_test` の [「GL-5a the Pass Editor and the Graph's detail column send the same edit_preset_pass for the same change of the root's pass」](../../test/devstudio_fullscreen_pass_test.cpp#L3036)(Frame Plan の Edit pass が開く Pass Editor で fragment を変えて Save した params と、Graph の詳細欄で同じ fragment を選んだ params が一致すること)。GL-1 の tooltip の test は新しい文に直しました。GPU シナリオは `gui_folded_pass_edit`(下の「Studio の自動化面」)です。

**New render target を Frame Plan と Graph で 1 つの関数に、詳細欄で出力の target を選び直す(✅GL-5b、`60ff3a2c`)**: 設計 [design_graph_layers](../design_graph_layers.md) §7 の GL-5(Frame Plan を mode に)の 2 本目です。GL-5a と同じく、書き先は今のままで入口だけを束ねます。

- **New render target は 1 つの関数です。**form を開くのは [`presentNewRenderTarget()`](../../src/devstudio/view/rendertargetform.cpp#L272)(create の form を、提案した名前・`RenderTargetFormValues` の既定の format と大きさ・engine の語彙で開く)、名前の提案は [`proposedRenderTargetName()`](../../src/devstudio/view/frameplanedit.cpp#L1429)(pass の出力から開くなら Pass Editor と同じ `<pass>_color`、ほかは `render_target`。宣言済みと重なれば `_2`、`_3`…)、送るのは今までの `framePlanRenderTargetAddRequest()` の `edit_render_targets` 1 回、拒否の文も今までの `framePlanRenderTargetRejection()` です。Frame Plan の toolbar・canvas の右クリック・Pass Editor の `New target: …` と、Graph の 2 つの入口(下)が同じ関数を通ります。Frame Plan の New render target... も、GL-5b から名前が入って開きます(下の「Frame Plan で render target を宣言する(✅F-1)」)。
- **palette の `render.target_ref` の New render target…**: target の combo(`pelican.graphCanvasPaletteTarget`)の最後に **New render target…** が付きます(engine の接続が `edit_render_targets` を送れるときだけ)。選ぶと note が `OK opens New render target; the reference is placed once the target is declared.` を言い、OK で palette が閉じて New render target の form が開きます。form の OK で target が宣言されると、Graph タブは authoring context を読み直し、その target が載った時点で、一覧から選んだときと同じく `<target>_ref` の node を置きます([palette の New render target… を受ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L6321))。form を Cancel すると何も置きません。GL-5b の前は、宣言が 1 つも無いと OK を押せず、Frame Plan へ案内していました(上の「palette で置ける render node」の表の `render.target_ref` の行)。
- **詳細欄の出力の行**: GL-5a の詳細欄(rendering 節の pass)に **Writes** の行が足され、color 出力ごとに combo(`pelican.graphCanvasFoldedPass.output.<n>`)が出ます。候補は宣言済みの target のうち depth でないもの(今の値が一覧に無ければ先頭に足す)と、最後の `New target: <pass>_color…` です([`GraphCanvasFoldedPassFields::fillOutputs()`](../../src/devstudio/view/graphcanvasfoldedpassfields.cpp#L307))。target を選ぶと、その出力の Load / Store と clear colour を連れて target を替え([`AttachmentOpsForm::retargetColor()`](../../src/devstudio/view/passforms/attachmentopsform.cpp#L198))、Pass Editor の Save と同じ `presetPassEditParams()` の `edit_preset_pass` を 1 回送ります。`New target: …` は New render target の form をその名前で開き、target が宣言されて context に載った後に、その出力をその target に替える `edit_preset_pass` を送ります。宣言と付け替えは別の 2 回の編集なので、Graph タブの Undo も 2 回分です(実装者の報告)。depth の出力は付け替えません。
- **拒否**: `edit_render_targets` の拒否は Frame Plan の New render target と同じ code ごとの 1 文です。詳細欄の編集が断られたときは、欄を root の内容で読み直します([`GraphCanvasFoldedPassFields::editRefused()`](../../src/devstudio/view/graphcanvasfoldedpassfields.cpp#L386)。断られた付け替えが combo に残って次の編集に混ざらないためで、GL-5a の shader の選び直しの拒否にも効きます。実装者の報告)。送る口は GL-5a の `sendPassEdit()` に `edit_render_targets` を足したもので、`GraphCanvasReadCapability` に `requestEditRenderTargets` が増えました。
- **Add は束ねていません**(仕様の決定 4)。入口ごとの書き先は次のとおりです(実装者がコードを読んで作った表。test はありません)。

| 入口 | RPC | 書き先 |
|---|---|---|
| Frame Plan の New render target...、Graph の New render target…(palette と詳細欄) | `edit_render_targets` `{op: add}` | managed feature fragment `project://passes/authoring/render-targets.json`(初回は root の `features[]` に足す) |
| Frame Plan の Add material pass... | 新しい target があれば先に `edit_render_targets`、次に `add_authored_pass` | managed feature fragment `project://passes/authoring/pass-<id>.json`(root の `features[]` に足す) |
| Frame Plan の Add fullscreen pass...(右クリック) | `add_authored_pass` | 同じ managed feature fragment |
| Frame Plan の Add compute task...(右クリック。Features タブの Task Editor へ移る) | `add_compute_task` | 新しい feature 文書(置き場は利用者が打つ。root の `features[]` に足す) |
| Graph の palette の `render.material_pass` / `render.fullscreen` / `render.raster` / `render.compute` / `render.image` / `render.target_ref` | `edit_graph_document` | graph の `nodes`(material pass は draw の body の definition も。`render.target_ref` は宣言済みの target を名前で指すだけ) |

  書き先が同じなのは New render target の組だけで、上のとおり 1 つの関数に束ねました。Frame Plan の Add の 3 つ(feature に書く)と palette(graph の `nodes` に書く)は書き先が違うので束ねず、「Blank で Frame Plan の Add material pass... を押すと palette と同じものが置かれる」という仕様の目標にも届いていません。どちらに寄せるかは GL-6 の判断で、統合側は「graph root では Frame Plan の Add を palette に寄せる」を利用者に提案しています(2026-10-03)。rendering 節に書く Add は Frame Plan にも Graph にもありません(実装者の報告)。この表は GL-5b の着地時のものです。✅GL-5c で、利用者の決定(2026-10-04)により、graph root では Frame Plan の Add の 3 つが palette と同じ graph の `nodes` に書くようになりました(下の GL-5c の段落。preset root では表のまま)。
- **分かっている穴**: New render target の既定の種類(Scene color)の target は、Blank の forward の material pass の color 出力にすると engine に `Material pass color output format mismatch` で断られます(シナリオは種類で Explicit format を選び、`R16G16B16A16_SFLOAT` にしています)。出どころに合わせて既定を変える案は GL-5c 以降です。GL-5c(Frame Plan の Edit pass / Edit task を Graph への委譲に。Frame Plan の New render target... を Graph の入口へ委譲するかもここ)は残っています(GL-5b の着地時の記録)。✅GL-5c で、forward の material pass の出力から開く Graph の 2 つの入口は Explicit format の `R16G16B16A16_SFLOAT` で開くようになりました。Frame Plan の New render target... は委譲せず、今までどおり Frame Plan の form です(下の GL-5c の段落)。
- **手で打つ欄**: 新しくは足していません。Graph から開く form は Frame Plan と同じなので、form の名前(提案入りで、打たずに OK でよい)と clear colour の欄が Graph からも出ます。
- test: `devstudio_graphcanvas_scene_test` の [「GL-5b the target_ref palette's New render target... opens the Frame Plan's form on a proposed name, sends its one edit_render_targets, and places the reference once the target is declared」](../../test/devstudio_graphcanvas_scene_test.cpp#L7177)・[「GL-5b a rendering-section pass's output is picked again from the declared targets, or New target... declares one first: each is one edit_preset_pass」](../../test/devstudio_graphcanvas_scene_test.cpp#L7229)。`devstudio_frameplan_graph_test` の [「F-1 New render target sends one edit_render_targets from the form, and the target's node takes Delete」](../../test/devstudio_frameplan_graph_test.cpp#L9420) は、form が `render_target` の名前で開くことを 1 行見ます。GPU シナリオは `gui_graph_new_target`(下の「Studio の自動化面」)です。

**graph root では Frame Plan の Add を Graph の palette に、Edit pass とダブルクリックを Graph の詳細欄に(✅GL-5c、`9cd89c5c`)**: 設計 [design_graph_layers](../design_graph_layers.md) §7 の GL-5(Frame Plan を mode に)の 3 本目です。GL-5a / GL-5b は書き先を今のままにして入口だけを束ねました。GL-5c は**利用者の決定(2026-10-04)**により、graph root(Blank のように root が graph で、preset を選ばない project)では、Frame Plan の **Add material pass…** / **Add fullscreen pass…** / **Add compute task…** を Graph の palette と同じ道(graph の `nodes` に書く)に寄せました。preset を選ぶ root(Hybrid など)は今までどおり form と managed feature fragment(`pass-<id>.json`)です。

- **どの root で寄せるか**: Frame Plan の 3 つの Add は [`addThrough()`](../../src/devstudio/view/frameplanwidget.cpp#L3454) を通ります。authoring context の `config_kind` が `direct` で、Graph タブが「置く」と答えたときだけ Graph へ渡し、それ以外は今までの form・Pass Editor・Task Editor を開きます。Graph タブは、読んだ context で root が graph でないと分かっているときだけ断ります([MainWindow の配線](../../src/devstudio/view/mainwindow.cpp#L742))。Frame Plan が context をまだ読んでいなければ、status 行に `Reading the render authoring context...` を出し、読んでから決めます。repo の fixture にも root が graph で preset を選ばないもの(`studio_automation_graph` など)があり、それらも Blank と同じく Graph へ渡します(実装者の報告)。
- **Add material pass…**(toolbar のボタンと右クリック): Graph タブが前に出て、root の scope で、絞り込み欄に `render.material_pass` が入った palette が開きます([`GraphCanvasWidget::placeFromPalette()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8835))。Add node… と同じ palette で、置くのも同じ `addFromPalette` です。contract を選んで OK(絞り込み欄の Return)を押すと、draw ループ付きの material pass(✅GL-1d)と自前の image が graph の `nodes` に入ります。`pass-<id>.json` は作らず、render target も宣言しません。material pass の form(`pelican.materialPassForm`)は開きません。
- **Add fullscreen pass…**(右クリック): 選ぶものが無いので palette を開かず、Graph タブ自身の Add fullscreen pass と同じく `render.fullscreen` を root に置きます([Graph タブが op を受ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L8270))。
- **Add compute task…**(右クリック): palette が `render.compute` で開きます。destination(新しい feature 文書の置き場)を打つ欄は出ません。root の rendering 節は `compute_tasks` を宣言できない(MG-2b)ので、graph の `nodes` が書き先です。
- **置いた node を選ぶ**: Frame Plan から開いた palette が root に置いた node は、編集が commit して文書がその node を持った時点で選ばれ、詳細欄が開きます([`placed()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8260))。material pass では、その前に engine が起こし直すことがあります(route の既定。下の「分かっている穴」も。G-buffer の format による起こし直しは ✅MC-1s で無くなりました)。
- **Add の form が持っていた選択**: 出力の target は palette の material pass が自前の image を連れて来ます。付け替えは `render.target_ref` を color の入力に繋ぐか、`present` へは `+ input` に線を引きます。material の routing は palette が forward の route の既定を同じ編集で送ります(✅GL-1d)。差し込む位置は graph の線の向き(RS-3)で決まるので、palette にも詳細欄にも欄を足していません(実装者の報告)。
- **Edit pass / Edit task と行のダブルクリック**: 選んだ pass / task を Graph が編集するなら、今までの Open in Graph と同じ道で Graph タブのその node を選び、詳細欄を開きます([`editsInGraph()`](../../src/devstudio/view/frameplanwidget.cpp#L3473)、[ダブルクリックの分岐](../../src/devstudio/view/frameplanwidget.cpp#L1012))。Graph が編集するのは、ループの回の copy、graph の `render.compute` が作った task(✅Q-12 の `graph_node`)、Graph タブの [`GraphCanvasModel::planPassEditedHere()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6264) が真になる pass(instance やループの body の node、root の文書が持つ node、rendering 節の pass)です。rendering 節の pass は type を問わず Graph へ行きます(GL-5a の詳細欄は fullscreen と material 以外では欄を出しません)。preset と feature の pass、graph node の無い task は今までどおり Pass Editor / Task Editor です。Pass Editor は残しています。この委譲は root の種類を問いません(Graph が編集する pass かどうかだけで決まります。コードの読み)。
- **New render target の既定の種類**: GL-5b の分かっている穴の直しです。form を開く所が「target が forward の material pass の color 出力になる」と分かっているときは、Studio の material pass の slot の表([`materialPassSlots()`](../../src/devstudio/model/frameplanmodel.cpp#L2238)。Frame Plan の Add material pass… の Create missing targets と同じ表)の format / format_class / usage を宣言として読んだ値で form を開きます([`materialPassColorTargetValues()`](../../src/devstudio/view/materialpassedit.cpp#L50)。読むのは form が既存の宣言を読むのと同じ `renderTargetFormValues()`)。forward の color は HDR でも SDR でも **Explicit format の `R16G16B16A16_SFLOAT`** です。GL-5b までの既定の Scene color は、Blank の forward の material pass の color 出力として engine に `Material pass color output format mismatch` で断られていました。名前は呼び出し側の提案(`<pass>_color` など)のままです。効くのは Graph の 2 か所です。
  1. 詳細欄の `New target: <pass>_color…`: その rendering 節の pass の宣言が `type: material` なら、宣言の `material_contract` と出力の番号から決めます(`passOutputTargetKind()`。[決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L6346))。
  2. palette の `render.target_ref` の New render target…: palette を開いたときに選んでいた node が `render.material_pass` で、その contract の color 出力が 1 つ(forward)なら、その出力の種類です([`besideMaterialPassTargetKind()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6359))。G-buffer(color 5 つ)は、どの出力に繋ぐか分からないので今までの既定です。

  それ以外(Frame Plan の New render target…、Pass Editor の `New target: …`、fullscreen pass の出力、何も選ばずに開いた palette)は今までの既定(Scene color)です。form は [`presentNewRenderTarget()`](../../src/devstudio/view/rendertargetform.cpp#L272) の引数 `kind` でこの値を受けます。engine の拒否の文からは決めていません(拒否は format の名前を返さないので、表から決める方が確かなため。実装者の報告)。
- **道具の列の tooltip**: context を読んで graph root だと分かると、Add material pass…(ボタンと action)・Add fullscreen pass…・Add compute task… の tooltip が `Opens in the Graph: the palette's <op>, placed in the graph's nodes and picked there.` になります。preset root では今までの行き先(`The material pass form: the pass goes into a fragment of the project.` / `The Pass Editor, on a new pass after the node picked.` / `The Task Editor, on a new task.`)を言います。Edit pass / Edit task の tooltip も `Opens in the Graph: …` で始まり、Pass Editor / Task Editor に行く場合を続けて言います([`syncDelegationTips()`](../../src/devstudio/view/frameplanwidget.cpp#L3481))。
- **View > Pass Editor**(`pelican.view.passEditor`): Pass Editor の dock を出して前に出します([足した所](../../src/devstudio/view/mainwindow.cpp#L1577))。graph root では Frame Plan の Add fullscreen pass… が Pass Editor を開かなくなったので、Pass Editor 自身の **New pass** から入る入口として足しました。Pass Editor の dock は Frame Plan と同じ領域のタブで、View > Panels の切り替えでは前に出せないためです(実装者の報告)。Pass Editor の New pass と Save は、graph root でも今までどおり managed fragment に書きます(GL-5c は Frame Plan の入口だけを変えました)。
- **Frame Plan にしか無い操作**(仕様の決定 5): 実装者がコードを読んで作った表です(test はありません)。この WP では移していません。右の列は次の WP の案です。

| Frame Plan の操作 | 呼ぶ関数 | Graph に同じ関数で出ているか | 移すなら(案) |
|---|---|---|---|
| Order…(道具の列) | `RenderFeaturesWidget::editFrameNodeOrder`(Features タブ) | 出ていない。Graph の順は線(データ依存、RS-3)と Place の欄で決まる | rendering 節の pass(畳んだ node)の右クリックに Order… を足して同じ関数を開く。graph の node には順が無いので出さない |
| Decompose into loop… | `FramePlanWidget::decompose()`(確認の後に `decompose_material_pass`) | 右クリックには無い。畳んだ material pass の notice の Expand to edit…(`pelican.graphCanvas.expandToEdit`)が同じ `decomposeMaterialPass` を呼ぶ | 畳んだ material pass の右クリックに Expand to edit… と同じ action を足す |
| Replace with loop version… | `RenderFeaturesWidget::replaceWithLoopVersion` | 右クリックには無い。feature の畳んだ node の Expand to edit… が同じ関数を呼ぶ | 同上(feature の畳んだ node の右クリック) |
| Fix sockets / Make sockets variadic | `FramePlanWidget::fixSockets()` / `makeSocketsVariadic()`(Pass Editor の Fix sockets も Frame Plan のもの) | 出ていない | GL-5a の詳細欄(rendering 節の pass)に置き、`fixSockets()` を呼ぶ |
| Shift ドラッグの接続 | task: `RenderFeaturesWidget::editComputeTaskConnection` / pass: `FullscreenPassWidget::editPresetPassConnection` | 別の関数。Graph の通常のドラッグは、rendering 節の pass のソケットへなら Frame Plan のソケットへの drop と同じ関数(GL-1e)、graph の node の間なら graph の線 | Graph の線と意味が重なるので移さない案。残すなら Graph の線で足りない場合(feature の task の入力)だけ |
| 資源の編集(resource のダブルクリックで RenderTargetForm の replace、Delete で remove) | `FramePlanWidget::openRenderTargetForm` / `framePlanRenderTargetRemoveRequest` | New render target… は同じ関数(GL-5b)。既存の target の編集・削除は出ていない | `render.target_ref` の詳細欄に Edit target… / Delete target を置き、GL-5a の `sendPassEdit()` で送る |
| (参考)Serial / Isolate / No alias | `graphTargetPlanningParams()` | 詳細欄(GL-5a)に同じ関数 | 移し済み |

- **分かっている穴**(着地時の記録):
  - graph root で **deferred の material pass を palette で置くと、engine が `material_physical_contract_changed` で起こし直します**。Frame Plan の form の道では起こし直しませんでした(実装者の報告)。保存していない scene があれば G-5 の問い(Save / Discard / Cancel)が出ます。Studio は RS-1 の `restart_required` に従っているだけで、望ましい挙動かは engine の側が MC-1 で調べています(統合側の記録、2026-10-04。未確認)。→ engine の ✅MC-1(`82665619`)で原因が分かりました。Graph の palette が `hdr` を見ずに、常に HDR の G-buffer の表の format を送っていて、`hdr` の無い Blank の live の material(SDR の表)と color の format が違っていました。Frame Plan の form は `hdr` を見て表を選ぶので同じ契約になり、起こし直しませんでした。engine の判定は正しく、直しは Studio 側で行います(§13.5 の RS-1 の項の MC-1)。既存のシナリオはこの問いに Save で答える手順を足しました。→ ✅MC-1s(`1d10bd4c`)で直りました。palette は form と同じ表で `hdr` に合わせた format の image を足し、Blank では engine が起こし直しません(上の「render.material_pass と draw.* の node(✅LP-4e)」)。Save で答える手順は `input_needs_shader` と `deferred_loop_from_scratch` から外しました(下の「Studio の自動化面」の MC-1s の段落)。`sockets_from_shader` と `sockets_fix_and_show` には MC-1s の後も残っていて赤になっていましたが、✅SK-1(`67fd3d29`)で外しました(同じ所の SK-1 の文)。
  - Pass Editor の New pass は、Frame Plan の Add と違って frame plan を読み直しません(シナリオは Pass Editor の Refresh を押します)。New pass を Frame Plan の Add と同じ関数にする案は手を付けていません。
  - 決定 4 の palette の推し量りは「palette を開いたときに選んでいた material pass」からです。何も選ばずに `render.target_ref` を置いて後で繋ぐ場合は今までの既定です。
  - 上の表の「出ていない」もの(Order…、Fix sockets、資源の編集・削除、右クリックの Expand to edit…)は移していません。Frame Plan の New render target… も Graph へは委譲していません。
- **手で打つ欄**: 新しくは足していません。graph root では Add compute task… の destination の欄が出なくなり、palette の compute shader の combo で選びます。Frame Plan から開いた palette の絞り込み欄は op の名前が入った状態で開くので、打たずに Return でよいです。
- test: `devstudio_frameplan_graph_test` の [「GL-5c decisions 1, 2 and 5: on a root that selects no preset the Adds hand the Graph the palette's op and say so; on a preset's root they keep their forms」](../../test/devstudio_frameplan_graph_test.cpp#L8399)(direct の context で Add の 3 つが Graph に op を渡し、form・Pass Editor・Task Editor は開かない。Graph が置けない root と preset root では今までどおり。tooltip)、[「GL-5c decision 3: Edit pass and a double-click open a pass the Graph edits there; a pass it reads only opens in the Pass Editor」](../../test/devstudio_frameplan_graph_test.cpp#L8501)、[「GL-5c decision 4: New render target for a material pass's colour output opens on the kind the contract's table gives it, not on the Scene color default」](../../test/devstudio_frameplan_graph_test.cpp#L9686)(forward の color は Explicit format の `R16G16B16A16_SFLOAT`、deferred の albedo は scene、material は data の `R8G8B8A8_UNORM`)、`devstudio_graphcanvas_scene_test` の [「GL-5c decisions 1, 3 and 4: the Frame Plan's Add opens the palette in the root on its op, …」](../../test/devstudio_graphcanvas_scene_test.cpp#L7293)(palette が `render.material_pass` で開き、OK で root の `nodes` への `add_node` が 1 回、`edit_preset_pass` / `edit_render_targets` は 0 回、文書が node を持った時点で選ばれ詳細欄の Contract が出る、`editsPlanPass()` の真偽、選んだ forward の pass の横の palette の New render target… が Explicit format で開く)。GPU シナリオは新しい `gui_frameplan_delegates` と、期待を変えた既存の 15 本です(下の「Studio の自動化面」)。

**pass の出力のソケット(✅GL-1f)**: engine の RS-4 で、`render.material_pass` / `render.shadow_depth` / `render.snapshot_copy` は、書いた image を出力の port として渡すようになりました(契約は [第6章](06_rendering.md) §6.18 の「グラフで書く pass」)。Graph タブはこの port を出力のソケットとして描き、fullscreen の出力と同じように線を引けます(設計は [design_graph_layers](../design_graph_layers.md) §7 の GL-1f)。

- **ソケット**:
  - `render.material_pass`: color のソケットと同名の出力(forward は `color`、G-buffer は `albedo` / `normal` / `material` / `worldpos` / `emissive`、`material_outputs` があればその名前)と `depth` です([出す所](../../src/devstudio/model/graphcanvasmodel.cpp#L2172))。`draws` は出しません。
  - `render.shadow_depth` は `depth`、`render.snapshot_copy` は `destination` です。どちらも catalog の `outputs` の `type_from_input` から出します([読む所](../../src/devstudio/model/graphcanvasmodel.cpp#L2194))。
  - 型は、同じ名前の入力のソケットにつないだ image / target_ref の型です。その入力が別の pass の出力から来ていれば、それを辿ります([`GraphCanvasModel::forwardedImageType()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2501))。何もつながっていなければ型は空です(実装者の報告では、空の型はどこへでもつなげます)。
  - 文書の `outputs` に書いていなくても、この port は出ているものとして扱います([判定](../../src/devstudio/model/graphcanvasmodel.cpp#L4510))。
- **線を引く**: この出力から graph の node の入力へ線を引くと、`set_input` の前に、書き手の node の `outputs` にその port を足す `replace_node` を同じ列で送ります。Undo 1 回で両方戻ります。既に `outputs` にあれば `set_input` だけです。
- **線を消す**: 線を消す(`set_input` を空に)、読む node を消す(`remove_node`)、body の出力を替える(`set_outputs`)ことで、その port を読むものが同じ scope に無くなれば、列の後ろに `outputs` から外す `replace_node` を足します。同じ scope のほかの node の入力か definition の出力が読んでいれば外しません。
  - 足す・外すは、Graph タブが送るすべての編集の列に掛かります([`GraphCanvasModel::withWriterOutputs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2316)。[掛ける所](../../src/devstudio/model/graphcanvasmodel.cpp#L2312))。engine は `outputs` に無い port の読みを `unknown graph output port: <node>.<port>` で断りますが、読まない port は書かなくても通します。外すのは文書を綺麗に保つためです(設計の決定)。
- **`present` へ落とす**: 上の「Graph から画面に出す」の入力のソケットへ、この出力も落とせます。`present` の `input` には、その出力が解決される image の名前(ソケットにつないだ `render.image` の id など)を書きます。graph の文書は変わらず、material pass の `outputs` にも足しません(rendering 節の pass は名前で読むためです。コードで確かめました)。
  - 実線は、その image を書いた pass の出力から引きます(上の「線」の元の 1)。fullscreen の出力でも同じです。
- **image node からの線**: image node の `resource` から読む既存の文書は、今までどおり描き、編集できます。
- **palette**: palette の material pass は `outputs: []` で来ます([空の `outputs` を書く所](../../src/devstudio/model/graphcanvasmodel.cpp#L3247))。読む node ができたときに足します。draw ループ付きの形(✅GL-1d)は変わりません。
- GL-1f の前は、material pass に出力のソケットが無く、書いた image は image node か `render.target_ref` から横に線を引いて読みました。
- test: `devstudio_graphcanvas_test` の [「GL-1f a material pass, a shadow_depth and a snapshot_copy hand on what they wrote; a line from one lists the port in the writer's outputs, and the last one gone takes it out」](../../test/devstudio_graphcanvas_test.cpp#L3348)(3 種の出力と型、型の合わない線は引けないこと、線と `outputs` の足しが 1 つの列で Undo 1 回、2 本目の読みは `set_input` だけ、最後の読みを消すと外すこと)。LP-4e の [「LP-4e a material pass's sockets come from its contract, and the palette places one」](../../test/devstudio_graphcanvas_test.cpp#L1737) は、出力が空であることから `color` / `depth` があることへ変わりました。GL-1e の scene の test は、線の元が pass の出力であることと、pass の出力を `present` に落とす節を足しました。

**render.image の値を選んで決める(✅UX-2)**: Graph タブで `render.image` を選ぶと、詳細表の上(Loop・Draw・View の欄と同じ並び)に image の欄(`pelican.graphCanvasImage`)が出ます([`GraphCanvasImageFields`](../../src/devstudio/view/graphcanvasrenderfields.hpp#L117))。どの値も選んで決め、JSON や型名を打ちません(利用者の指摘「生 json とか生の型を書くよりかは、なるべく選択したい」)。

- **Format**(`pelican.graphCanvasImage.format`): engine の `get_render_authoring_context` の `render_target_vocabulary.formats`(上の「レンダー設定を編集する」)から選びます。depth の型なら depth の format(`D` と数字で始まる名前)だけ、それ以外の型なら depth 以外だけです([`GraphCanvasModel::imageFormatChoices()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6970))。
- **Type**(`pelican.graphCanvasImage.semantic`): graph catalog の型のうち constructor が `image` のものを、人の読める名前(`pelican.render.color_signal@1` → `Color signal`)で並べます。`legacy_untyped` の型は、今の値のときだけ出ます([`GraphCanvasModel::imageTypes()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6909))。
  - その下に、型の引数ごとの combo(`pelican.graphCanvasImage.argument.<名前>`)が出ます。取りうる値は catalog の型 schema の `parameters[].allowed` で、EL-1 の `types[].arguments.<名前>.values`(上の「レンダー設定を編集する」の EL-1 の段落)があればそれが優先です。既定値のある引数は先頭が `(default: <値>)` で、選ぶと鍵を消します。一覧の無い引数(`{min, max}` だけのもの)の combo は、既定値の項目と文書の値だけです([引数の欄を組む所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1026))。
  - 型を Depth と Color の間で替えると、format と usage も一緒に動きます。format は engine の一覧から depth なら `D32_SFLOAT`、color なら `R16G16B16A16_SFLOAT`(一覧に無ければ同じ側の format)を選び、usage の `COLOR_ATTACHMENT` と `DEPTH_STENCIL_ATTACHMENT` を入れ替えます。前の型と共通で新しい型が受ける引数は残します([`GraphCanvasModel::imageParametersWithSemantic()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6985))。
- **Usage**: `render_target_vocabulary.usage` の flag ごとの check(`pelican.graphCanvasImage.usage.<FLAG>`)です。
- **Size**: `Screen ×`(`extent_scale` の spin)か `Fixed`(`width` / `height` の spin)の radio と、`Layers` の spin です。Fixed のときは `extent_scale` を 1 にして `width` / `height` を書き、Screen × に戻すと `width` / `height` を消します。`layers` は 2 以上か、文書が既に持っているときだけ書きます([`GraphCanvasImageFields::commitSize()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1177))。
- どの変更も、node の parameters 全体を 1 回の `set_node_parameters` で送ります(Undo 1 回)。欄が知らない鍵はそのまま送ります([`GraphCanvasModel::setNodeParameters()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6820))。送れなかった(送信中、何も変わらない)ときは、欄が文書の値に戻ります。
- ✅PS-1s から **Lifetime**、✅IA-1s から **Initial contents** と **Import…**、**Size from the image** の欄があります(下の「一度だけ走る pass と、frame をまたいで残る image(✅PS-1s)」と「画像ファイルを image の初期内容に(✅IA-1s)」)。
- vocabulary は、届いていなければ接続ごとに 1 回だけ `get_render_authoring_context` を読んで取ります([`GraphCanvasModel::askRenderTargetVocabulary()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6875))。vocabulary を返さない engine では、Format は打てる combo(Enter で送る)、Usage は flag を `,` か空白で区切って打つ欄(`pelican.graphCanvasImage.usageText`)になり、欄の下に `The engine did not list <何>: those fields take what is typed, or show the document's value.` が出ます([欄を読み込む所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L628))。

**render pass の shader を選ぶ(✅UX-2)**: `render.fullscreen` / `render.raster` を選ぶと、pass の欄(`pelican.graphCanvasPass`)が出ます([`GraphCanvasPassFields`](../../src/devstudio/view/graphcanvasrenderfields.hpp#L252))。

- **Vertex** / **Fragment**(`pelican.graphCanvasPass.shader.vertex` / `.shader.fragment`): Pass Editor の Pick... と同じ一覧(project の `shaders/` と engine の fullscreen shader。下の「shader は一覧から選べます(✅T-1)」)から選びます。一覧は欄を出す直前に読み直します([一覧を渡す所](../../src/devstudio/view/graphcanvaswidget.cpp#L7234))。pass が `shader` にその段を持たなければ、その行は出ません。選ぶと `set_node_parameters` 1 回です。
- **Ports**: `ports` の宣言を 1 行ずつ読むだけで出します(`<名前>: kind access binding attachment`)。入力のソケットは shader の宣言から出るので(✅VI-1 / Q-19)、つなぐのは canvas の線です。
- ✅PS-1s から **Schedule**(Every frame / Once)と、once の pass の **Recompute** の行があります(下の「一度だけ走る pass と、frame をまたいで残る image(✅PS-1s)」)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-2 a render pass's shaders are picked from the Pass Editor's list, its ports shown as declared, and its shader pill opens the fragment list」](../../test/devstudio_graphcanvas_scene_test.cpp#L8342)。

**値の帯は一覧を開きます。打つ道は詳細表の JSON です(✅UX-2)**:

- 選べる値の帯は、押すとすぐその下に一覧の combo が開きます(打つ欄ではありません。[開く所](../../src/devstudio/view/graphcanvaswidget.cpp#L3246))。対象は `render.image` の `format` と `type`、`render.fullscreen` / `render.raster` の `shader`(fragment)です([`GraphCanvasModel::valueChoices()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7032))。type の項目は、上の型の替え方で format と usage も動かした parameters を持ちます。
- JSON を直接書く道は残ります。詳細表の parameters の行の値をダブルクリックすると、その鍵の値を JSON で打てます。JSON でなければ送らずに notice が `<key> was not changed: its value is not JSON.` を言います([打つ所](../../src/devstudio/view/graphcanvaswidget.cpp#L7175))。詳細表はほかの行では編集になりません。✅UX-12a から、catalog の表に載っている鍵は右の欄(下の「node の欄を catalog の表から作る」)でも書けます。
- ✅UX-12a から、帯のその場編集の部品は、catalog の `parameters` の表の kind を先に見て決めます([表の kind を先に見る所](../../src/devstudio/view/graphcanvaswidget.cpp#L3694))。`bool` は check、`number` / `integer` は spin(`integer` は小数無しで、整数のまま送ります。engine は `2.0` を整数として受けないため。コードの注記)、`string` / `asset` / `node_ref`(と `fields` の無い `shader`)は打つ欄、`enum` は表の `enum` の値の一覧、ほかの kind は JSON の欄です。文書にまだ値が無く表に `default` があれば、その値から始めます。今の値の JSON の型から推すのは、表に無い名前のときだけです。
- test: `devstudio_graphcanvas_scene_test` の [「UX-2 a render.image's format, type, usage and size are picked in its fields, each one set_node_parameters; its pills open the list; the details still take JSON; an engine without the vocabulary falls back to text」](../../test/devstudio_graphcanvas_scene_test.cpp#L8183)。GPU シナリオは `graph_from_blank_forward` の後半です(下の「Studio の自動化面」)。

**palette で置ける render node(✅UX-3)**: palette は、ports が parameters で決まる render node も、engine がそのまま受ける種(最初の値)を入れて置きます。どれも 1 回の編集(Undo 1 回)です([種を入れる所](../../src/devstudio/model/graphcanvasmodel.cpp#L3055))。UX-3 の前は、`render.fullscreen` と material pass と draw step のほかは palette に出ませんでした。

| op | palette で選ぶもの | 置く形 |
|---|---|---|
| `render.target_ref` | 宣言済みの target の combo(`pelican.graphCanvasPaletteTarget`。`get_render_authoring_context.render_targets[]` から graph 自身の `render.image` を除いたもの。tooltip は `declared by: <source>`) | id は `<target>_ref`。`type` は宣言の `type`、無ければ format(depth か)と `dimension: cube` / `layers` から作ります([`GraphCanvasModel::targetRefType()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7100))。✅GL-5b から combo の最後は **New render target…** で、選んで OK を押すと target を宣言してから置きます(下の GL-5b の段落)。`edit_render_targets` を送れない接続で宣言も無ければ OK を押せず、`No render target is declared yet: use render.image.` が出ます(GL-5b の前は、宣言が無ければいつもこうで、文は `… declare one (Frame Plan > New render target...) or use render.image.` でした) |
| `render.image`(単独) | 無し | 上の欄の既定: Color signal(scene / linear / extended)・`R16G16B16A16_SFLOAT`・画面の大きさ・usage `COLOR_ATTACHMENT` / `SAMPLED`。view の軸で回すループの body では、family の大きさです |
| `render.buffer` / `render.attribute` | element の combo・attribute の domain の combo(vertex / instance / index)・数の spin | buffer は `size` = 数 × element の byte 数(3 要素は 4 要素分)、attribute は `domain` と `count`。domain が index なら element は `uint` です |
| `render.raster` | 無し | 上の Add fullscreen pass と同じ形(`engine://fullscreen` + `engine://passthrough`、自前の image)に `draw: {vertex_count: 3}`。Pass Editor の一覧にその 2 つの shader があるときだけ palette に出ます |
| `render.compute` | UX-3 の着地時は無し。✅UX-5d / EL-5s から shader の combo(`pelican.graphCanvasPaletteComputeShader`。下の「project の compute / RT の shader を palette で置く」)、✅CT-6s から その横の **New…**(`pelican.graphCanvasPaletteComputeNew`。下の「RT を Graph で組む」) | UX-3 の着地時は、project の `shaders/*.comp` の最初(`project://shaders/compute` があればそれ)、`dispatch.groups` `[1, 1, 1]`、`ports` は空で、compute shader の無い project では palette に出ませんでした。今の置き方は EL-5s の段落のとおりです。行は、project の `.comp` か RT の組があるか、engine の shader の一覧がまだ届いていないか、engine に RT pipeline があれば出ます([palette に出す条件](../../src/devstudio/model/graphcanvasmodel.cpp#L4852))。Blank の project でも、RT pipeline を持つ engine なら出ます |
| `render.snapshot_copy` | `snapshot_after` の pass の combo(`pelican.graphCanvasPaletteSnapshotAfter`。plan の pass。既定は最後の pass) | `source` はその pass が書いたもの(root の node の出力、image node、宣言済みの target。target なら `render.target_ref` を同じ列で足す)。何も分からなければ自前の image `<id>__source` を足します。`destination` は source と同じ型・format・大きさの image `<id>__copy`(usage `TRANSFER_DST` / `SAMPLED`)を同じ列で足します([`GraphCanvasModel::snapshotCopySeedOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7228))。plan に pass が無ければ OK を押せません |
| `render.shadow_depth` | 無し | depth の image `<id>__depth`(`pelican.render.depth@1`・`D32_SFLOAT`・画面の大きさ)を同じ列で足して `depth` につなぎます |

- 種の combo と spin は palette の下に出ます([palette の欄](../../src/devstudio/view/graphcanvaswidget.cpp#L7802))。一覧は行を選ぶたびに読み直します(palette を開いた後に authoring context が届くことがあるため)。palette を開くときに authoring context が無ければ読みに行きます。
- draw_loop が走らせる body には、これらの op は出ません。
- どの op を palette に出すかは [`GraphCanvasModel::paletteOperationsIn()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4816) です。
- test: `devstudio_graphcanvas_scene_test` の [「UX-3 decision 1: the palette places target_ref, image, buffer, attribute, raster, compute, shadow_depth and snapshot_copy seeded, each in one edit the engine's graph validation takes」](../../test/devstudio_graphcanvas_scene_test.cpp#L8452)。GPU シナリオは `graph_palette_kinds`(下の「Studio の自動化面」)です。

**render node の置き場所 —— Place(✅UX-3)**: `render.fullscreen` / `render.raster` / `render.material_pass` / `render.shadow_depth` / `render.snapshot_copy` / `render.compute` を選ぶと、Place の欄(`pelican.graphCanvasPlace`)が出ます([`GraphCanvasPlaceFields::load()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2337))。UX-3 の前は、置き場所の欄はループの Loop の欄にしかありませんでした。

- **Place**(`pelican.graphCanvasPlace.place`): `automatic`(`insert` も `canonical_anchor` も書かない)、plan の pass ごとの `before <pass>` / `after <pass>`(anchor・ループの回・自分は除く)、`after anchor <anchor>`(`canonical_anchor`)です。文書の値が一覧に無ければ(plan にまだ無い pass、欄が表せない `insert`)、その値のまま 1 項目足して出します。
- **Family**(`pelican.graphCanvasPlace.passSet`): `(the frame's first)`(`pass_set` を書かない)と、authoring context の `graphs[].name` です。
- 選ぶと `insert` / `canonical_anchor` / `pass_set` を書き替えた parameters で `set_node_parameters` 1 回です。`insert` と `canonical_anchor` は片方だけ書きます。material pass が同じ場所のままなら `insert.declared: true` を残します([`GraphCanvasModel::setPlace()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7381))。
- compute task は全部の pass の後に走るので(graph の `insert_on_compute`)、欄は押せず、`A compute task runs after every pass; it takes no place.` が出ます。
- test: [「UX-3 decision 2: a render node's Place and family are picked, each one set_node_parameters; a compute task takes none」](../../test/devstudio_graphcanvas_scene_test.cpp#L8575)。

**入力ソケットの右クリック —— 読み方の設定(✅UX-3)**: `render.fullscreen` / `render.raster` / `render.compute` の入力のソケットを右クリックすると、メニューの先頭に `Socket <port>` の節が出ます。今まで `ports` の JSON に書いていたものを、ここで書きます([節を足す所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2636)、[メニューに入れる所](../../src/devstudio/view/graphcanvaswidget.cpp#L3351))。`ports` にその port の宣言が無ければ節は出ません。

| 項目(`pelican.graphCanvasSocket.<名前>`) | 書く鍵 | 押せるとき |
|---|---|---|
| **Previous frame**(`history`。check) | `history: true` | image を読む port で、attachment でない |
| **Filter**(`filter`。`default (linear)` / `linear` / `nearest`) | `sampling.filter` | image を読む port で、attachment でない(fullscreen / raster では storage でない、compute では `binding: sampled`)。✅UX-7 から番号付きの入力も押せます(UX-7 の前は「番号付きの入力でもない」も条件でした) |
| **Address**(`address`。`default (repeat)` / `repeat` / `mirrored_repeat` / `clamp_to_edge`) | `sampling.address` | 同上 |
| **Mip level** / **Layer**(`mip` / `layer`。spin) | `subresource.mip` / `.layer`(0 は鍵を消す) | image で、attachment でない(✅UX-7 から番号付きの入力も) |
| **Attachment location**(`location`。spin) | `location` | fullscreen / raster の color attachment |

- 押せない項目は灰色で、tooltip が理由を言います(`Only a colour attachment has a location.` など)。押せる条件は engine の graphmodule / rendergraphcomposition の規則をコードに写したものです(コードの注記。✅UX-7 で engine の RS-14 の後の規則に合わせました。下の「pass の欄の Inputs」。[`GraphCanvasModel::portSettings()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7411))。
- どの変更も、その時点の設定に 1 つだけ変えて `set_node_parameters` 1 回です([`GraphCanvasModel::setPortSettings()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7459))。
- test: [「UX-3 decision 3: a render node's socket menu writes its port's previous frame, sampling, mip / layer and location, each one set_node_parameters」](../../test/devstudio_graphcanvas_scene_test.cpp#L8627)。決定 4・5(Rename の提案、depth_compare、EL-1 の一覧)は [「UX-3 decisions 4 and 5: the engine's materials, tags and variants are picked in the draw_loop's fields, the draw steps' and the material pass's; draw.override's depth_compare is a combo」](../../test/devstudio_graphcanvas_scene_test.cpp#L8815) です。

**pass の欄の Inputs —— 読む image ごとの Filter / Address / mip(✅UX-6a)**: `render.fullscreen` / `render.raster` を選んだときの pass の欄(`pelican.graphCanvasPass`。上の「render pass の shader を選ぶ」)に **Inputs** の行が増えました。上の右クリックの節は残ります。UX-6a の前は、読み方の設定は右クリックの節にしか無く、シナリオから操作できませんでした(コードの注記)。

- 行は、`ports` の宣言のうち `kind: image`・`access: read` で `attachment` の無い port ごとに 1 行です。左が port の名前で、右に combo が 3 つ並びます([`GraphCanvasPassFields::loadInputSettings()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1887))。
  - **Filter**(`pelican.graphCanvasPass.input.<port>.filter`): `default (linear)` / `linear` / `nearest`
  - **Address**(`.address`): `default (repeat)` / `repeat` / `mirrored_repeat` / `clamp_to_edge`
  - **mip**(`.mip`): `mip 0`〜`mip 15`
- 押せる条件は右クリックの節と同じ `GraphCanvasModel::portSettings()` です。押せない combo の tooltip は、Filter / Address が `Only an image the shader samples by a binding of its own takes sampling.`、mip が `An attachment takes no mip of its own here.` です。
- **番号付きの入力も押せます(✅UX-7、`1fcd155e`)**: engine の ✅RS-14 が番号付きの入力(`binding: "input"`。shader の宣言から出るソケット)にも名前付きの port と同じ `sampling` / `subresource` を通すので([第6章](06_rendering.md) §6.18「番号付きの入力 —— `binding: "input"` と `input_index`」の RS-14 の段落)、Studio の判定から `binding != "input"` を外しました([判定の所](../../src/devstudio/model/graphcanvasmodel.cpp#L7436))。書く場所は名前付きの port と同じ port の宣言の `sampling` / `subresource` で、`default (…)` を選ぶとその鍵を消し、`mip 0` は `subresource.mip` を消します(何も設定しない pass の文書は前と同じ bytes)。UX-6a の着地時は、3 つとも出たまま押せず、tooltip が `A numbered input takes no sampling (the engine refuses it): it is read linear, repeat.` を言っていました。
  - **fullscreen の pass の新しい番号付きの入力は `clamp_to_edge` で書きます**: engine の既定は `repeat` のままなので、Studio が明示します。線を落として番号付きの port を作るとき(上の「render node の入力は shader の宣言から」)と、雛形の Create が入力をつなぐときに、`render.fullscreen` の port に `sampling: {address: "clamp_to_edge"}` を書きます([書く所](../../src/devstudio/model/graphcanvasmodel.cpp#L104))。`render.raster` の入力(vertex shader が読む displacement など)は既定のままです。既にある port は書き換えません。GB-2 の bloom の上端の回り込み(下端の明るい床が画面の上端に出る)は、これで `gui_build_full` の画素で消えたことを見ています(下の「Studio の自動化面」)。
- 選ぶと、その時点の設定を 1 つだけ変えて `set_node_parameters` 1 回です(右クリックと同じ `GraphCanvasModel::setPortSettings()` を通ります。[`GraphCanvasPassFields::commitInputSetting()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1955))。送れなければ、行を文書の値で作り直します。
- test: `devstudio_graphcanvas_scene_test` の [「UX-6a decision 5: the Pass fields show each read image's Filter, Address and mip as combos, each pick one set_node_parameters; UX-7 decision 1: a numbered input's are picked too」](../../test/devstudio_graphcanvas_scene_test.cpp#L8732)(UX-7 で名前と中身を書き換えました)。GPU シナリオは `gui_build_full` の段 4 と `gui_build_optimize` の bloom の body の pass の欄(`clamp_to_edge` が出て、combo が押せる)です。

**Studio が作る image は float で、TRANSFER_SRC を付けません(✅UX-6a)**: Add fullscreen pass、palette の `render.fullscreen` / `render.raster`、ループの body に足す pass が一緒に作る image(`<pass>__color`)は、`R16G16B16A16_SFLOAT`・usage `COLOR_ATTACHMENT` / `SAMPLED` です([Add fullscreen pass の image の既定](../../src/devstudio/model/graphcanvasmodel.cpp#L3322))。既にある node は変えません。

- UX-6a の前は `R8G8B8A8_UNORM` で、usage に `TRANSFER_SRC` もありました。8 bit では 1 を超える光が切れて bloom の明るい所の取り出しが効かず、engine は `TRANSFER_SRC` を「frame plan の外から読まれうる」と読んで alias の候補から外すので、Studio が作った image は alias されませんでした(GB-2 の着地時の description の記録)。
- palette の単独の `render.image`(上の「palette で置ける render node」の表)は、UX-6a の前から `TRANSFER_SRC` を付けていません。palette の `render.snapshot_copy` が自前で足す source の image(`<id>__source`)だけは、今も `TRANSFER_SRC` を付けます([snapshot_copy の自前の source の usage](../../src/devstudio/model/graphcanvasmodel.cpp#L7301))。
- image の欄の Usage の `TRANSFER_SRC` の check(`pelican.graphCanvasImage.usage.TRANSFER_SRC`)の tooltip が、付けると alias されないことを言います(`Lets the image be copied or read back from outside the frame plan. With it the image is not aliased.`。[TRANSFER_SRC の check の tooltip](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1101))。
- **Physical plan の Remove TRANSFER_SRC**: Frame Plan の Physical plan のタブに、`TRANSFER_SRC` のせいで alias されない image の組 **Not aliased for TRANSFER_SRC (N)** が出ます([`populateTransferSourceRows()`](../../src/devstudio/view/frameplanwidget.cpp#L4930))。行は名前・`Not aliasable`・plan の理由と、ボタン **Remove TRANSFER_SRC**(`pelican.framePlan.dropTransferSrc.<資源>`)です。
  - 組に入るのは、buffer でなく、plan に表現を持ち、alias できない資源のうち、理由に `usage TRANSFER_SRC is outside` を含むもの(engine の PO-1 の原因の文。上の「計画を詰める手がかり(✅PO-1)」)です。理由に `: ` の後の原因が無い(PO-1 の前の)engine では、資源の usage に `TRANSFER_SRC` があるかで決めます([`keptByTransferSource()`](../../src/devstudio/view/frameplanwidget.cpp#L4920))。
  - 押すと、Graph タブがその名前の `render.image` の node(ループの回の資源なら、`loop_origin` が指す body の node)の usage から `TRANSFER_SRC` を外し、`set_node_parameters` 1 回で送ります([`GraphCanvasWidget::removeImageTransferSource()`](../../src/devstudio/view/graphcanvaswidget.cpp#L9195))。graph の文書にその image が無い・`TRANSFER_SRC` が無い・Graph タブが今編集できないときは何も送らず、Frame Plan の status 行が `Could not take TRANSFER_SRC off <資源>: it is not an image the Graph tab can edit now.` を言います。
- test: `devstudio_frameplan_graph_test` の [「UX-6a decisions 2 and 4: the Frame Plan tells the Graph after a reload that moved declarations, and its Physical plan offers Remove TRANSFER_SRC on an image kept out of aliasing for it」](../../test/devstudio_frameplan_graph_test.cpp#L12432)。GPU シナリオは `gui_build_optimize`(下の「Studio の自動化面」)です。

**pass の欄の New… の名前は、雛形を選ぶたびに提案し直します(✅UX-6a)**: Graph の pass の欄の Fragment の横の **New…**(`pelican.graphCanvasPass.newFragmentShader`)は、雛形の combo(`.shaderTemplateKind`)・名前(`.shaderTemplateName`)・**Create**(`.createFragmentShader`)の行を開きます。雛形は Pass Editor の New… と同じです(下の「雛形は 3 種類」)。

- 名前の提案は雛形の名前(`deferred_lighting` / `tone_map`。Pass-through なら pass の id)で、project の `shaders/` に同じ名前のファイルがあれば `_2`、`_3`… を付けた空き名です([`GraphCanvasPassFields::proposeTemplateName()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2155))。欄が空か、Studio が前に提案した名前のままなら、雛形を選び直すたびに提案し直します。打った名前は上書きしません。
- ✅UX-6a から、Create で書いた名前(打った名前でも)を「Studio が提案した名前」として覚え直します([Create の後に名前を覚え直す所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2254))。そのため次の pass で雛形を選ぶと、欄は新しい雛形の名前に替わります。UX-6a の前は、Create の後も前の名前(`deferred_lighting`)が欄に残り、Tone map を選んでも替わらないので、打ち直さないと `shaders/deferred_lighting.frag is already in the project and was left as it is: ...` で断られました(GB-2 の着地時の description の記録)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-6a decision 1: after a Create, picking another template names the new shader after that template (a free name past the files already there); a typed name stays」](../../test/devstudio_graphcanvas_scene_test.cpp#L9239)。

**GB-3 で打った欄を pass と image の欄に(✅UX-7、`1fcd155e`)**: GB-3 の `gui_build_ocean` / `gui_build_ltc` が JSON を打った所と、GUI に欄が無かった所を、Graph の詳細の欄に足しました(番号付きの入力の sampling は上の「pass の欄の Inputs」)。どの欄も、変えると node の parameters 全体を `set_node_parameters` 1 回で送ります。

- **Defines**(fullscreen / raster の pass の欄): Pass Editor と同じ `ShaderDefinesForm` の表で、行が `shader.defines` の 1 つです(`pelican.graphCanvasPass.shaderDefines.table` / `.shaderDefines.add` / `.shaderDefines.remove`。上の「生の JSON の欄はフォームになりました」)。engine が断る行は赤くなり、その間は何も送りません([`GraphCanvasPassFields::commitDefines()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1756))。表が空なら `shader.defines` を消します。
- **Depth**(raster の pass の欄。`pelican.graphCanvasPass.depth`): `(none)` / scope の中の depth の image(format が `D16` / `D24` / `D32` / `X8_D24` で始まるか、type が `pelican.render.depth@1` の `render.image`)/ `New depth image (D32_SFLOAT)` から選びます([`GraphCanvasModel::setRasterDepth()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7545))。
  - New を選ぶと、1 つの編集で `<pass>__depth`(あれば `_2`…)の `render.image`(`D32_SFLOAT`、usage `DEPTH_STENCIL_ATTACHMENT` / `SAMPLED`、大きさは pass の color の image の `extent_scale` / `width` / `height` / `layers` を写す)を足し、pass に `attachment: "depth"` の port(`depth`)と、その入力の線と出力を足します。
  - depth を付けた pass は、`raster_state` が言っていなければ `depth_test` と `depth_write` を真にします。`(none)` に戻すと port・線・出力と `raster_state` の depth の鍵を消します(depth の無い pass の depth の state は engine が断るため。コードの注記)。
- **Raster**(raster の pass の欄): fullscreen の pass の欄と同じ Pass Editor の `RasterStateForm` を raster の pass にも出します(cull・blend、depth を付けた pass では depth test / write / compare)。raster の pass では `raster_state` だけを書き、Frame data と Output の行は出しません([`GraphCanvasPassFields::loadPassSettings()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1991))。
- **Draw**(raster の pass の欄。`pelican.graphCanvasPass.vertexCount` / `.instanceCount` / `.gridSize` / `.grid`): **Vertices** と **Instances** の spin、N の spin(2〜16385、既定 257)と **Grid N×N (triangles)** のボタンです。Grid は `draw.vertex_count` に 6·(N−1)² を入れて送ります(257 なら 393216)。Instances は 1 のときは鍵を書きません(既に鍵があれば書きます)([`GraphCanvasPassFields::commitDraw()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1855))。draw が `vertex_source` / `index_source` を持つか `operation: "indexed"` なら、数は source から来るので Vertices と Grid は押せません。UX-7 の前は、draw の帯に `{"vertex_count": 393219}` の JSON を打っていました(仕様の背景)。
- **Keep previous frame (history)**(`render.image` の欄。`pelican.graphCanvasImage.history`): 付けると image の parameters に `history: true`、外すと鍵を消します([check を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L485))。前の frame の image を読むのは、入力ソケットの右クリックの Previous frame(上の表)です。
- **送れないときは待って送ります**: graph が保存や読み直しの最中で編集を受けられないときに Defines / Depth / Draw / Raster を変えると、値を捨てずに覚え、編集できるようになった時に送ります([`GraphCanvasPassFields::deferWhileBusy()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1638))。✅LC-1s(`ed4ed5f9`)から、覚えている間の notice は `Saving the graph change...` です。また、Draw の spin に打って Enter も focus の移動もしないまま pass の欄が別の node に移る(描き直しで node が一瞬消える)ときは、その値を先に送り([`GraphCanvasPassFields::flushTyped()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1680)。✅UX-9(`4d11aa96`)で `flushDraw()` から替わり、`release()` から呼ばれて、打ちかけの define の cell と clear colour も送ります)、読み直しは打っている途中の spin を文書の値で上書きしません。LC-1s の前は、描き直しが打った Instances の 2 を落としていました(commit の記録)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-7 decision 2: a pass's Defines rows are its shader.defines, written in one set_node_parameters; a row the engine refuses is red and not sent」](../../test/devstudio_graphcanvas_scene_test.cpp#L10042)、[「UX-7 decisions 3 and 4: a raster pass's Depth is picked (a new D32_SFLOAT image in one edit the engine takes), its raster_state from the Raster form, and its draw counted, Grid NxN writing 6(N-1)^2 vertices」](../../test/devstudio_graphcanvas_scene_test.cpp#L10092)、[「UX-7 decision 6: a render.image keeps the previous frame by its check (history: true), one set_node_parameters each way」](../../test/devstudio_graphcanvas_scene_test.cpp#L10321)、[「UX-7 a draw count typed but not confirmed survives the Pass fields letting go, and the notice says Saving at once」](../../test/devstudio_graphcanvas_scene_test.cpp#L10208)(LC-1s で足した 1 本)。GPU シナリオは `gui_build_ocean`(Grid 257 と Instances 2、New の depth と depth compare `less_equal`)です。

**palette で置くときに、要る parameter を先に聞きます(✅UX-7)**: palette で `render.scalar`・`value.constant`・`value.input`(✅UX-8 から、engine の catalog が必須を出していればその op。`audio.volume` / `audio.pitch` も)を選ぶと、置く前に小さなダイアログ `<op> needs`(`pelican.graphCanvasRequired`)が出ます([必須のダイアログを開く所](../../src/devstudio/view/graphcanvaswidget.cpp#L8183))。OK(`.ok`)で、答えを parameters に入れた node を 1 回の編集で置きます(`voice` は parameter ではなく、選んだ `audio.playback` の `voice` からの線です)。Cancel(`.cancel`)なら置きません。後から node の欄で直せます。✅UX-12a から、聞く op と欄は engine の catalog の `parameters` の表(✅CT-3)から作ります(下の表の後の項)。

| op | 欄(`pelican.graphCanvasRequired.<名前>`) | 選び方 |
|---|---|---|
| `render.scalar` | `target` | root と今の scope の `render.buffer` の id(✅UX-8 から、frame plan の buffer —— `bloom.intensity` のような feature の buffer —— も。`@` を含む名前は出しません)から選ぶか、新しい名前を打つ combo。候補が 1 つも無ければ `value_buffer` が入っています。空の間は OK を押せません |
| `value.constant` | `value` | 表の kind(`number`)の spin。表に `default` が無いので 0 から始まります(✅UX-12a) |
| `value.input` | `name` | catalog の `input_names` の名前で、`scene_time` が先頭(✅UX-8)。catalog が出さなければ Studio の写しの `scene_time` / `time` / `delta_time` |
| `audio.volume` / `audio.pitch` | `target` か `voice`(どちらか 1 つ) | ✅UX-12a から、表の `target` の `alternatives`(`["voice"]`)に従って、`target`(`.target.choose`)と `voice`(`.voice.choose`)の radio で選びます。`voice` は scope の `audio.playback` の id の combo で、下に `or connect a line to voice after placing` が出ます。scope に `audio.playback` があれば `voice` が先に選ばれています |
| `value.parameter` | `name` / `default` | ✅UX-8。`name` は打つ欄(scope で空いている id の提案が入っている。空の間は OK を押せない)、`default` は数の spin |
| `pose.clip_sample` | `rig` / `clip` | ✅UX-12a(engine の CT-3 で必須になった 2 つ)。`rig` は project の `.gltf` / `.glb` / `.vrm` の combo と **Import…**(`.rig.import`)。`clip` は、選んだ rig の file の animation の名前の combo で、rig を選び直すと読み直します |

- 聞く op と欄は、✅UX-12a から catalog の各 op の `parameters` のうち `required` の付いたものです([`GraphCanvasModel::requiredParametersFor()`](../../src/devstudio/model/graphcanvasvalues.cpp#L754))。palette が自分で種を入れる op(`value.curve` の keys、`audio.playback` の asset、`ui.*` の node、`render.scalar` 以外の `render.*` と `draw.*`)は今までどおり聞きません。欄は node の欄と同じ部品(下の「node の欄を catalog の表から作る(✅UX-12a)」の `GraphParameterEditor`)を表の kind で選び、Studio が表より多くを知る 3 つだけ自分の部品にします: `render.scalar` の `target`(buffer の combo)、`value.input` の `name`(名前の combo。catalog の `input_names` から。[`GraphCanvasModel::valueInputNamesFor()`](../../src/devstudio/model/graphcanvasvalues.cpp#L841))、`pose.clip_sample` の `clip`(rig の animation)。`default` の無い数は 0(`integer` は表の `min`)、bool は false から始まるので、OK で見えている値のまま置けます([必須のダイアログを作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2406))。`alternatives` を持つ必須は「どちらか 1 つ」の radio で、op の入力 port の名前でもある候補(`voice`)は線で満たします。OK は、選んだ側の欄が空の間は押せません。
- UX-12a の前は、`required_parameters`(✅CT-1)を名前の規則で欄にしていて(`value.morph_weight` の `node` / `target` も数の spin になる規則でした)、catalog がそれを出さない engine では UX-7 の Studio の表(`render.scalar` / `value.constant` / `value.input` の 3 つ)を使っていました。UX-12a でこの表は消え、catalog が `parameters` を出さない engine ではダイアログは出ません(コードの読み)。今の規則では `value.morph_weight` の `node` / `target` は `integer` の spin(表の `min` の 0 から)です。CT-4 から `value.morph_weight` は owner のある instance の中にしか置けない op です(engine の CT-4 の commit の記録)。palette での見え方は確かめていません(未確認)。
- UX-7 の前は、palette から置いたこれらの node は parameters が空で、engine に名前で断られ(RS-15 からは `graph.render_node_parameters_missing@1`)、GUI で書く欄が無いので `gui_build_ltc` は面光源の角を shader の定数にしていました(シナリオの description の記録)。
- **自動化での書き方**: graph の node の parameter は、canvas の値の帯に打って書くか、✅UX-12a から右の欄(`pelican.graphCanvasParameters.<名前>`、`pose.clip_sample` は `pelican.graphCanvasPose.<名前>`)で書きます。詳細表の行の編集欄は自動化から開きません(表の editor は名前を持たないため)。UX-12a の前は、まだ持っていない鍵には帯が無いので、render node の鍵は専用の欄で、上の 3 つはこのダイアログで書いていました(UX-7 の決定 8。[README の「A node's parameters」](../../test/studio_scenarios/README.md#L642)。README は UX-12a で変わっていません)。
- test: [「UX-7 decision 5: render.scalar, value.constant and value.input ask for what they cannot be placed without, and are placed with it」](../../test/devstudio_graphcanvas_scene_test.cpp#L10255)、✅UX-12a から [「UX-12a decision 2: the palette's dialog is made from the table -- pose.clip_sample asks for its rig among the project's glTF files and its clip among the rig's animations, and is placed with them」](../../test/devstudio_graphcanvas_scene_test.cpp#L11528)。

**ループと分岐の後に足した node も描きます(✅UX-7)**: GB-3 で、ループと分岐を組んだ後に palette で足した `render.raster` が、文書にも frame plan にも入るのに canvas に描かれず、Refresh でも出ないことが 3 回ありました。canvas は値の帯のその場編集の editor が開いている間は描き直しを待っていて、その pick が帯に当たって editor が開いたままになっていたためです(シナリオの description の記録)。今は、文書の中身が変わった読みなら editor が開いていても描き直し、editor と打った字を新しい描画へ持ち越します([描き直しの判定](../../src/devstudio/view/graphcanvaswidget.cpp#L8596))。中身の変わらない読み(配置や報告だけ)は今までどおり editor を待ちます。test は [「UX-7 decision 7: a node the document gains is drawn while a pill's editor is open, the editor carried over the new drawing」](../../test/devstudio_graphcanvas_scene_test.cpp#L10350) です。

**ループの body の pass は回の番号を読めます —— 雛形と印(✅LC-1s、`ed4ed5f9`)**: engine の ✅LC-1 で、body の shader は `#include "pelican_loop.glsl"` の `pelican_loop_element()` / `pelican_loop_count()` で自分が何回目かを読めます(書き方の正は [第6章](06_rendering.md) §6.18「回の番号を shader に渡す」)。Studio はその入口と見え方を足しました。

- **雛形**: Graph の pass の欄の **New…** で、**ループの body の中**(分岐の body を含む。[`GraphCanvasModel::isLoopBodyScope()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3954))の pass に雛形を作ると、`#include "pelican_loop.glsl"` と 1 行のコメント `// pelican_loop_element(): this element, 0-based; pelican_loop_count(): how many` を、`#include "pelican_sets.glsl"` の次の行(無ければ `#extension GL_GOOGLE_include_directive` か `#version` の次の行)に入れます([`withLoopElementInclude()`](../../src/devstudio/view/fullscreenshadertemplate.cpp#L365)、[呼ぶ所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2227))。既に include していれば何もしません。ループの外では雛形は変わりません。
  - 雛形はどれも自分の push constant の block を宣言しないので、`PELICAN_LOOP_CUSTOM_PUSH` の形にはしません(コードの注記)。Pass Editor の New… は include を入れません(呼ぶのは Graph の pass の欄だけ)。New… はもともと fullscreen の pass の欄にだけ出ます。
- **印**: frame plan の node の `reads_loop_index: true`(§13.5 の「`get_frame_plan` に足されたもの」)を読み、その node が作った pass のどれかが読めば([`GraphCanvasModel::loopElementRead()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3963)):
  - body の pass の node の下に、GL-2b の帯の位置で `reads element` を描きます。root のゾーンの中の body も、body に入った canvas も同じです([帯を足す所](../../src/devstudio/view/graphcanvaswidget.cpp#L1338))。ゾーンの帯は、同じ名前の root の node の GPU 時間(上の「速くしたかが見える」)を取りません。
  - pass の欄に `This shader reads the loop element (pelican_loop_element)` の 1 行(`pelican.graphCanvasPass.loopElement`)。ループに複製されない pass が読んでいれば `…; outside a loop it reads element 0` です([1 行を出す所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1614))。plan が文書より後に来ることがあるので、欄を出すたびに読み直します。
- **拒否の 1 文**: engine の `shader.loop_push_overlap@1` を、Graph の拒否の 1 文の表に足しました: `<shader> declares a push constant that overlaps the loop element at <offset>..<offset+size> in the <stage> stage — use PELICAN_LOOP_CUSTOM_PUSH and PELICAN_LOOP_PUSH_FIELDS`([表の行](../../src/devstudio/model/inspectormodel.cpp#L1185)、[range を `a..b` にする所](../../src/devstudio/model/inspectormodel.cpp#L1412))。
- test: `devstudio_graphcanvas_test` の [「LC-1s a pass whose shader reads the loop element is known by its body node」](../../test/devstudio_graphcanvas_test.cpp#L3477) と [「LC-1s shader.loop_push_overlap@1 reads as a sentence」](../../test/devstudio_graphcanvas_test.cpp#L3505)、`devstudio_shadertemplate_compile_test` の [「LC-1s a template made in a loop's body includes pelican_loop.glsl and compiles」](../../test/devstudio_shadertemplate_compile_test.cpp#L546)。GPU シナリオは `gui_build_ocean`(FFT の段を 1 本の body の `ocean_fft.frag` が `pelican_loop_element()` で選ぶ。下の「Studio の自動化面」)です。

**曲線を shader の値へ GUI でつなぐ —— `render.buffer` の value と pass の Buffers(✅UX-8、`cabf7691`)**: GB-4 の `gui_build_music` は、曲線の値を shader に渡す道で止まりました。engine では動くことを統合側が手で JSON を書いて確かめていて(`value.curve` → `render.buffer`(float)の `value` → fullscreen pass の名前付きの buffer の port → shader。仕様の背景)、UX-8 でその手書きの所が線を引く操作になりました。

- **`render.buffer` の value のソケット**: `render.buffer` / `render.attribute` の node は、入力の先頭に `value` のソケットを描きます。engine はこの 2 つに任意の scalar の入力 `value` を持たせていて、その op の port は parameters から決まるので catalog に載りません。そのため Studio が足して描きます(コードの注記。[render.buffer に value のソケットを足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L2060))。
  - 値の node(`value.curve` など)の出力をここへ落とすと `set_input` を送ります。`render.buffer` の `size` が 4 でなければ、同じ編集で `size: 4` にします(値の線は float 1 つを書くため)。
  - element が `float` でなければ送らず、`<node> holds <element>: a value's line writes one float. For two to four numbers, write them with a render.scalar.` を言います([value の線を 1 つの float に限る所](../../src/devstudio/model/graphcanvasmodel.cpp#L2887))。
- **pass の欄の Buffers**(`pelican.graphCanvasBuffers`。fullscreen / raster の pass): shader の宣言(Q-19 のソケットと同じ `pass_inputs`)の buffer の binding を 1 行ずつ並べます。線があれば `binding <b> <宣言> ← <node>.<port> (port <port>)`、無ければ `binding <b> <宣言>: drag a render.buffer's resource onto its socket` です。宣言が取らない名前付きの buffer の port も並べます([`GraphCanvasValueFields::loadBuffers()`](../../src/devstudio/view/graphcanvasvaluefields.cpp#L693))。UX-8 では `<port> ← <node>.<port>` の形でした。✅UX-13a から、線があり pass の宣言が読めていれば、engine がその port を bind する番号を付けて `binding <b> pelican_resource_<port> ← <node>.<port>` と出します(番号の決め方は下の「buffer の binding は engine の規則で(✅UX-13a)」)。線が無い port は `<port> (no line)` です。
- **buffer の宣言のソケットへ線を落とす**: `render.buffer` の `resource` を pass の buffer の宣言の ○ へ落とすと、名前付きの buffer の port(`{"kind": "buffer", "access": "read", "element": <buffer の element>}`)の `set_node_parameters` と、その port への `set_input` を 1 回の編集で送ります([`GraphCanvasModel::bufferPortOps()`](../../src/devstudio/model/graphcanvasvalues.cpp#L359))。
  - `element` は必ず書きます(engine の CT-1 は欠けると `graph.port_field_missing@1` で断るため)。
  - port の名前は、宣言の変数名から `pelican_resource_` を外したものです(`pelican_resource_level` なら `level`。[`GraphCanvasModel::bufferPortName()`](../../src/devstudio/model/graphcanvasvalues.cpp#L342))。engine は名前付きの port を `pelican_resource_<port>` の変数にだけ割り当てるからです(コードの注記)。
  - 断るもの(送らず、notice に 1 行): storage でない buffer の宣言は `Binding <b> of <node> is a <型>: a pass hands its shader storage buffers (declare it as a `readonly buffer` block).`、変数名が `pelican_resource_` で始まらない宣言は `… the engine reads a buffer port through a variable named pelican_resource_<port>. Name the block's variable so, or read it through pelican_resource_ports.glsl and + Buffer port.`、buffer でない資源は `<source> hands on no buffer: a buffer port reads a render.buffer or a render.attribute.` です([線を落とした buffer の宣言を名前付きの port にする分岐](../../src/devstudio/model/graphcanvasmodel.cpp#L5340))。
  - 外す: 名前付きの buffer の port の線を外すと、port の宣言も同じ編集で消します(線の無い port は engine が割り当てないため。[線と一緒に buffer の port を消す所](../../src/devstudio/model/graphcanvasmodel.cpp#L2920))。
- **+ Buffer port**(`pelican.graphCanvasBuffers.add`): shader が `pelican_resource_ports.glsl` で読む buffer は、port ができるまで反映に出ません。押すと、名前(`.name`。打てる combo)と読む buffer(`.source`。scope の `render.buffer` / `render.attribute`)の行が出て、**Add**(`.apply`)で上と同じ op の列を送ります(読む出力は `resource`)。名前の候補は、pass の shader が `project://shaders/...` なら、その file(拡張子無し・`.frag`・`.vert`・`.glsl`)の中の `PELICAN_RESOURCE_PORT_<名前>` / `pelican_load_<名前>` / `pelican_sample_<名前>` から拾います([`shaderResourcePortNames()`](../../src/devstudio/view/graphcanvasvaluefields.cpp#L784))。✅UX-13a から変数そのもの `pelican_resource_<名前>` も拾い、include の名前の `pelican_resource_ports` と engine の関数 `pelican_sample_table` は拾いません。
- **palette の初期値**: `value.curve` は `keys: [{time: 0, value: 0}, {time: 1, value: 1}]`(最初の 1 秒で 0 から 1)で置かれ([曲線を 0→1 の ramp で置く所](../../src/devstudio/model/graphcanvasmodel.cpp#L3549))、`value.curve` の `time` と `value.playback` の `elapsed` は Q-11 の `audio.playback` の `time` と同じく scope の `scene_time` につながって来ます([曲線と playback を scene の時計につなぐ所](../../src/devstudio/model/graphcanvasmodel.cpp#L3517))。
- test: `devstudio_graphcanvas_test` の [「UX-8 decisions 1 and 3: a curve from the palette is a ramp on the scene clock, and its line lands on a float buffer's value socket」](../../test/devstudio_graphcanvas_test.cpp#L3620)、[「UX-8 decision 2: a buffer binding of the shader is a named buffer port, added with its line and its element in one list; taking the line away takes the port」](../../test/devstudio_graphcanvas_test.cpp#L3684)、`devstudio_graphcanvas_scene_test` の [「UX-8 decision 2: a pass's Buffers list its shader's buffer bindings, and + Buffer port offers the names its shader reads through the port header」](../../test/devstudio_graphcanvas_scene_test.cpp#L10613)。GPU シナリオは `gui_build_curve`(下の「Studio の自動化面」)です。

**曲線の Keys —— 表・小さな曲線・Timeline の点(✅UX-8)**: Graph で `value.curve` を選ぶと、値の欄に **Keys** の節(`pelican.graphCanvasCurve`)が出ます。曲線は文書の `parameters.keys` に直に書きます(資産にはしません。仕様の利用者の決定)。

- **表**(`.keys`): 1 行が key 1 つで、Time と Value の数の spin(`.key<行>.time` / `.key<行>.value`)、Interpolation の combo(`.key<行>.interpolation`)、×(`.key<行>.remove`。key が 1 つなら押せない)です。補間は engine の `value.curve` が受ける `linear` / `step` / `cubic` です([`GraphCanvasModel::curveInterpolations()`](../../src/devstudio/model/graphcanvasvalues.cpp#L126))。spin は確定(Enter か focus の移動)で送ります。
  - **+ Key**(`.add`)は、選んだ行(無ければ最後の行)の後に、次の key との中間(最後なら 1 秒後)の時刻で、そこでの曲線の値の key を足します。値は cubic の区間も直線として読んだものです(コードの注記)。
  - `linear` は鍵を書きません(engine の既定)。表に出ない鍵(cubic の key の接線など)は、そのまま残して送ります。
- **小さな曲線**(`.graph`): 表の上に key を点で描きます。点を drag すると動き、Shift で時刻だけ、Ctrl で値だけです。drag の間は押した時の枠で測り、放すと 1 回の編集です([`GraphCanvasCurveGraph`](../../src/devstudio/view/graphcanvasvaluefields.hpp#L28))。
- **Timeline の点**: Graph の Timeline の帯は、選んでいる `value.curve` の key を帯の下端に ◇ で描き、横に drag すると時刻だけが動きます(値は欄で)。放すと 1 回の編集です([Timeline の key を横に動かす所](../../src/devstudio/view/graphcanvaswidget.cpp#L4624))。帯の時間の幅は key の時刻(と 0)も含めます。
- 1 回の drag・1 行の確定・+ Key・× は、それぞれ `set_node_parameters` 1 回(Undo 1 回)です([`GraphCanvasModel::setCurveKeys()`](../../src/devstudio/model/graphcanvasvalues.cpp#L219))。drag の時刻は隣の key から 0.001 離れた所で止めます(時刻の順を保つため)。key が 0 個・数でない値・engine が受けない補間・時刻が増えていない列は送らず、欄の下の 1 行(`pelican.graphCanvasValueFields.note`)が理由を言います([`GraphCanvasModel::curveKeysRefusal()`](../../src/devstudio/model/graphcanvasvalues.cpp#L159))。
- test: `devstudio_graphcanvas_test` の [「UX-8 decision 4: a curve's keys are edited a row or a drag at a time, held in order, and each change is one Undo step」](../../test/devstudio_graphcanvas_test.cpp#L3754)、`devstudio_graphcanvas_scene_test` の [「UX-8 decision 4: a curve's Keys table, its small curve and the Timeline's marks each write one edit」](../../test/devstudio_graphcanvas_scene_test.cpp#L10423)。

**value.playback の欄と render.scalar の欄(✅UX-8)**:

- **value.playback**(`pelican.graphCanvasPlayback`): **Wrap**(`.wrap`。`once` / `repeat` / `ping_pong`)と、**Duration**・**Source begin**・**Source end**・**Speed**・**Delay** の数(`.duration` / `.source_begin` / `.source_end` / `.speed` / `.delay`。既定は 1 / 0 / 1 / 1 / 0。[`GraphCanvasModel::playbackNumbers()`](../../src/devstudio/model/graphcanvasvalues.cpp#L241))です。
  - 欄は文書の値を、鍵が無ければ既定を示します。変えると、既定と同じ値でも鍵を書きます(欄の値と文書を同じにするため。コードの注記)。Duration は 0 より大きくなければ送らず、`A playback's duration has to be above 0.` です。
  - edge が来ている数は押せません(tooltip `An edge drives it.`)。「曲線で制御」が作る playback(上の「曲線で制御」が作るノードの集合)も同じ op なので、同じ欄が出ます。`audio.playback` にはこの欄は出ません(Q-10 の Sound / Bus / Wrap の欄)。
- **render.scalar**(`pelican.graphCanvasScalar`。engine の ✅RS-15): **Buffer**(`.target`。上の必須のダイアログと同じ候補)、**Value**(`.components`。`1 (on the value line)` / `vec2` / `vec3` / `vec4`)と 2〜4 個の数の spin(`.value0`〜`.value3`)、**Offset**(`.offset`。読むだけの `<n> bytes`)です。
  - 1 を選ぶと `parameters.value` を消し、値は `value` の線から来ます。vec2〜vec4 を選ぶと数を `parameters.value` に書き、`value` の線があれば同じ編集で外します(engine は配列を書いた node の `value` の edge を読まないため。[`GraphCanvasModel::setScalarValue()`](../../src/devstudio/model/graphcanvasvalues.cpp#L537))。
  - offset は Studio が割り当てます。同じ buffer に書く `render.scalar` を今の offset と id の順に並べ、byte 0 から隙間なく詰めた offset を、変わる sink の全部に同じ編集で書きます(0 は鍵を消す)。Buffer を替えると、新しい buffer の sink の後ろに付け、元の buffer の残りを詰め直します([`GraphCanvasModel::setScalarTarget()`](../../src/devstudio/model/graphcanvasvalues.cpp#L578))。
- test: [「UX-8 decision 5: a playback's wrap and numbers are written one at a time; a number an edge drives is not」](../../test/devstudio_graphcanvas_test.cpp#L3815)、[「UX-8 decision 12: render.scalar's numbers are packed with the other sinks on the buffer, and the engine's layout and port refusals read as sentences」](../../test/devstudio_graphcanvas_test.cpp#L3957)(どちらも `devstudio_graphcanvas_test`)。

**Timeline は root の node を instance 無しで読みます(✅UX-8)**: root の scope では、`Live` が入っていれば `Values from:` が `(none)` のままでも root の node の値を読みます(root の問いは instance を名指さない問いのため。[root を instance 無しで読む条件](../../src/devstudio/model/graphcanvasmodel.cpp#L811))。root の node だけの graph には名指す instance が無く、UX-8 の前の Timeline は `0 samples` のままでした(GB-4 は空の definition を MainCamera に装着して出所にしていました)。

- 読めないときは、Timeline の要約(`pelican.graphTimelineSummary`)が `<node>: 0 samples: <理由>` で理由を 1 つ言います —— `No engine is running.` / `Pick a node to follow over time.` / `A definition has no values of its own: pick the instance to read them through in Values from.` / `Turn Live on to read the values while the engine runs.` / `The engine has not published values for this graph yet.` / `<node> publishes no value (a sink or a render node is not read).`([`GraphCanvasModel::timelineReason()`](../../src/devstudio/model/graphcanvasvalues.cpp#L438))。
- test: `devstudio_graphcanvas_test` の [「UX-8 decision 6: the Timeline reads the root's nodes without an instance while Live is on, and says why when it cannot」](../../test/devstudio_graphcanvas_test.cpp#L3846)。

**音の node を置く・音声を Import する(✅UX-8)**:

- **audio.volume / audio.pitch**: palette で置く前のダイアログ(上の「palette で置くときに、要る parameter を先に聞きます」)が **voice**(scope の `audio.playback` の id)を聞き、その `voice` 出力から `voice` 入力への線と一緒に置きます。`value` には 1 の `value.constant` がつながって来ます(鳴っているままの音量。ほかの op の種の定数は今までどおり 0。[volume と pitch の種の定数を 1 にする所](../../src/devstudio/model/graphcanvasmodel.cpp#L3532))。音量のフェードは、palette の `value.curve` の `value` を `audio.volume` の `value` へ落とします(`gui_build_music`)。
- **Import**: **File ▸ Import Sound…**(`pelican.file.importSound`。[File メニューの Import Sound…](../../src/devstudio/view/mainwindow.cpp#L1511))と、`audio.playback` の Sound の欄の **Import…** は、ファイルのダイアログ(`pelican.audioImportDialog`。Qt のダイアログで、名前の欄は `pelican.audioImportDialog.fileName`)で選んだ `.wav` / `.ogg` を project の `assets/audio/` に写します([`importProjectAudio()`](../../src/devstudio/model/graphcanvasvalues.cpp#L861))。
  - 同じ名前があれば `<名前>_2`、`_3` … にし、上書きはしません。`.wav` / `.ogg` でない file は写さず、理由を notice に出します。
  - canvas と Sound の欄へ `.wav` / `.ogg` を drag & drop しても同じです([音声の drag & drop を受ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L4765))。
  - 写した後に Sound の一覧を読み直し、最初の 1 つを、選んでいる `audio.playback` の `asset` に送ります(選んでいなければ写すだけ)。notice は `Imported project://assets/audio/<名前>.`(複数なら ` and <n> more`)です。
- test: `devstudio_graphcanvas_test` の [「UX-8 decisions 7 and 13: a sound sink is placed driving the voice the dialog picked, at full volume; the dialog asks what the catalog lists」](../../test/devstudio_graphcanvas_test.cpp#L3873) と [「UX-8 decision 9: sounds are copied into assets/audio, _2 beside a name already there」](../../test/devstudio_graphcanvas_test.cpp#L4140)、`devstudio_graphcanvas_scene_test` の [「UX-8 decisions 7 and 8: audio.volume asks which voice it drives; what one addition brings beside it is placed clear of what is drawn」](../../test/devstudio_graphcanvas_scene_test.cpp#L10491)。

**New definition…(✅UX-8)**: Graph の canvas の右クリック(編集できて、畳んだ view でないとき)に **New definition…**(`pelican.graphCanvas.newDefinition`)があります。押すと definition の名前の行(`pelican.graphCanvasDefinitionName`。`definition` か、使われていれば `_2` … の提案が入っている)が出て、**Create**(`pelican.graphCanvasDefinitionApply`)で空の definition(`nodes: []`、`outputs: {}`)を `add_definition` 1 回で足し、commit が届くと Scope をその definition に切り替えます([`GraphCanvasModel::newDefinitionOps()`](../../src/devstudio/model/graphcanvasvalues.cpp#L455))。UX-8 の前は、definition を作るには Add loop を通るしかありませんでした(仕様の背景)。

- 空で開いた definition の body は、最初の node が来たときに fit し直します(GB-4 で、空の body に fit したまま足した node が画面の外に出た件。[空で開いた scope を fit し直す所](../../src/devstudio/view/graphcanvaswidget.cpp#L7393))。✅UX-9 から、これは definition の body だけで、root は fit し直しません(root は project を読む前に空で描かれ、自分の倍率で開くため。コードの注記)。
- 仕様の決定 11(GB-4 の、ループを消した後に Scope で body を選ぶと canvas が空になる件)は、下の test が「ループを消した後に Scope で選んだ body の node が描かれ、画面の中にある」ことを見ます。原因と直した所は commit の記録に無く、未確認です。
- test: `devstudio_graphcanvas_test` の [「UX-8 decision 10: New definition adds an empty definition by a free name」](../../test/devstudio_graphcanvas_test.cpp#L3940)、`devstudio_graphcanvas_scene_test` の [「UX-8 decisions 10 and 11: New definition opens the definition it made; a loop's body is drawn when its loop is gone」](../../test/devstudio_graphcanvas_scene_test.cpp#L10543)。

**RS-15 の Studio 側 —— log2 のボタン、image の層と mip の式、拒否の 1 文(✅UX-8)**: engine の ✅RS-15(`log2`、body の image の大きさの式、`render.scalar` の vec と offset。[第6章](06_rendering.md) §6.18)を Graph の欄から書けるようにしました。

- **log2**: Loop の欄の Count の式の補完の列に **log2**(`pelican.graphCanvasLoop.insertLog2`)があります。Count の target が graph の大きさの決まった `render.image`(整数の `width` と `height`)のときだけ押せ、`log2(<幅と高さの大きい方>)` を式に入れて送ります。式が空か整数だけなら置き換え、それ以外なら cursor の所に足します([`GraphCanvasLoopFields::insertLog2()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1118))。
- **image の Mip levels と式**: image の欄に **Mip levels**(`pelican.graphCanvasImage.mipLevels`。既定 1)があります([Mip levels の spin を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L443))。UX-8 の着地時の spin は 0〜16 で、0 が `full` でした。✅UX-12e から、spin の前の combo(`pelican.graphCanvasImage.mipLevelsShape`)で **Count** と **Full chain** を選び、spin は 1〜16 で Count のときだけ出ます(下の「node の欄の残りを選ぶ形に(✅UX-12e)」)。Full chain は `"full"`、Count の 2 以上はその数を書き、1 は鍵を書きません(既に鍵があれば書きます)。大きさの ƒ で式に替えている間は、Layers と Mip levels も式の欄(`.layersExpression` / `.mipLevelsExpression`)になり、Enter で送ります。
- **拒否の 1 文**: `graph.render_scalar_layout_invalid@1` は `reason` ごとに `The render.scalar <node> writes <target> from byte <offset>, over the sink before it (which ends at byte <expected_offset>)`(overlap)/ `…, leaving bytes <expected_offset> up to <offset> unwritten`(gap)、`graph.render_scalar_type_mismatch@1` は `The render.scalar <node> writes <components> float(s) into <target>, whose elements are <element>`、CT-1 の `graph.port_field_missing@1` は `Port <port> of <node> needs <field>` です([render.scalar の 2 つの code の 1 文](../../src/devstudio/model/inspectormodel.cpp#L1161))。test は上の「value.playback の欄と render.scalar の欄」の decision 12 の test です。

**project の compute / RT の shader を palette で置く —— ports・image・線は shader の宣言から(✅EL-5s、`708c667d`)**: engine の ✅EL-5 の `get_shader_interface`(§13.5「project の shader の入出力を読む」)を読み、palette の `render.compute` で選んだ project の shader から ports と書く image と線を作ります。GB-3 では、palette の compute は port 無しで置かれ、descriptor を宣言する shader は置く時に断られ、後から書いた port もつながっていない入出力として断られて、compute の task を使えませんでした(`gui_build_ocean` の description の記録)。

- **一覧**: palette で `render.compute` を選ぶと出る shader の combo(`pelican.graphCanvasPaletteComputeShader`)は、project の `shaders/<名前>.comp`(✅UX-3)に加えて、`.rgen` と同じ名前の `.rmiss` / `.rchit` がそろう project の組を `<参照>  (ray tracing)` で並べます(その後に engine の RT pipeline。✅UX-5d。[project の RT の組を探す所](../../src/devstudio/view/graphcanvaswidget.cpp#L7280))。✅CT-6s から、combo の横の **New…** が RT の雛形を書いてその組を選びます(下の「RT を Graph で組む(✅CT-6s)」)。
- **置く**: project の shader を選んで OK を押すと、notice が `Reading <参照>'s ports...` を言い、Studio が `get_shader_interface` を聞きます(`building` なら 500 ms 後に聞き直す)。`ready` が来たら次を 1 回の編集で置きます([palette の compute を ports が読めてから置く所](../../src/devstudio/view/graphcanvaswidget.cpp#L7339)、[`GraphCanvasModel::computeShaderOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8513)):
  - `storage_image` の port ごとに `<node>__<port>` の `render.image` を足します。format は宣言の `format`、無ければ `R16G16B16A16_SFLOAT`、`extent_scale: 1`、usage は `STORAGE` と `SAMPLED` です(view の軸で回すループの body では family の大きさ)。port は `{"kind": "image", "access": "write", "binding": "storage", "type": <scene color の型>}` で、その image からの線を付け、port を出力にします。
  - dispatch は、書く image があり `local_size` の z が 1 なら最初の書く image の `groups_from`、それ以外は `groups: [1, 1, 1]` です(前から `groups` だけを持つ node はそれを残す)。RT の組は `ray_tracing` の `raygen` / `miss` / `closesthit` に同じ参照を書き、最初の書く image の `rays_from` です。書く image の無い RT の組は置きません。
  - `sampled_image` の port は書かず、空のソケットとして描きます(engine は線の来ない宣言の port を断るため。コードの注記)。そこへ線を落とすと、port(`{"kind": "image", "access": "read", "binding": "sampled", "sampling": {"filter": "nearest", "address": "clamp_to_edge"}}` に、落とした出力から辿った `type`)と線を 1 回の編集で送ります([compute の空のソケットに線を落とす所](../../src/devstudio/model/graphcanvasmodel.cpp#L2861))。
  - 置かないとき(notice に 1 行): 120 秒たっても読み終わらなければ `The engine did not finish reading <参照>'s ports.`、`failed` なら `The engine could not read <参照>'s ports: <error>`、compute の stage も書く image のある RT の組も無ければ `<参照> declares nothing a compute task can run: …` です。engine の shader と、`get_shader_interface` を送る口を持たない capability では、今までどおり置きます。
- **compute の欄**(`pelican.graphCanvasCompute`。`render.compute` を選ぶと右に出る): **Shader**(`.shader`)は palette と同じ一覧で、文書の値が一覧に無ければ先頭に足して見せます。別の shader を選ぶと、同じ規則で ports を作り直します —— 同じ名前で宣言される port は線を残し、新しい storage の port には image を作り、宣言から消えた port を読んでいた線を外します(`replace_node` を含む 1 回の編集。interface をまだ読んでいなければ、聞いてから送ります。[`GraphCanvasModel::setComputeShader()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8643))。`engine://` の shader を選ぶと `shader` だけを書きます。✅CT-6s から、Shader の横に RT の雛形を書く **New…** が付きます(下の「RT を Graph で組む(✅CT-6s)」)。
- **reflection_error**: 選んだ shader の stage に `reflection_error` があると、欄が `The engine could not read <参照>'s ports: <message の 1 行目>` を赤で言い(`.status`)、**Copy message**(`.copyMessage`)が message の全部を写します([`GraphCanvasComputeFields::syncStatus()`](../../src/devstudio/view/graphcanvascomputefields.cpp#L259))。message が `undeclared identifier` / `#error` / `undefined macro` を含めば `(the shader may need defines the reflection does not pass)` を添えます(engine の EL-5 は rendering 節の `shader_defines` を渡さずに compile するため。[`GraphCanvasModel::reflectionErrorSentence()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8660))。読んでいる間は灰色で `Reading the shader's ports...` です。
- **見張りと Update ports**: Graph は、compute の node が使う project の shader の file(`.comp`、`.rgen` / `.rmiss` / `.rchit`)を見張り、書き込みを 200 ms まとめて、使われている stem を全部 `get_shader_interface {refresh: true}` で聞き直します([compute の shader の書き込みをまとめる所](../../src/devstudio/view/graphcanvaswidget.cpp#L5291))。image の port の名前が前と違えば、notice が `<参照> changed its ports: + <足された> / − <消えた>` を言い([ports の変化の notice を作る所](../../src/devstudio/model/graphcanvasmodel.cpp#L8730))、notice の下に **Update ports**(`pelican.graphCanvasUpdatePorts`)が出ます。押すと、変わった node の全部を上の「別の shader を選ぶ」と同じ規則で 1 回の編集で作り直します([`GraphCanvasModel::updateComputePorts()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8738))。自動では書き換えません。
- test: `devstudio_graphcanvas_scene_test` の [「EL-5s a project compute shader from the palette arrives with its ports and images」](../../test/devstudio_graphcanvas_scene_test.cpp#L9697)、[「EL-5s a compute node's Shader picked again makes its ports the new shader's」](../../test/devstudio_graphcanvas_scene_test.cpp#L9778)、[「EL-5s a compute shader's reflection error is said in its fields with Copy message」](../../test/devstudio_graphcanvas_scene_test.cpp#L9843)、[「EL-5s a compute shader whose ports changed on disk is said, and Update ports follows it」](../../test/devstudio_graphcanvas_scene_test.cpp#L9895)。GPU シナリオは `gui_build_ocean_compute`(下の「Studio の自動化面」)です。

**RT を Graph で組む —— ray_tracing と dispatch を汎用の欄で、stage ごとの shader の候補、raygen / miss / closest hit の雛形(✅CT-6s、`827eddd7`)**: engine の ✅CT-6(`cbc9bb03`。catalog の `render.compute` の `ray_tracing` と `dispatch` が `fields` 付きの `object` になり、`parameters_complete` が `true` になった。[第6章](06_rendering.md) §6.17 の CT-6 の段落)の Studio 側です。CT-6s の前は、`ray_tracing` は専用の欄の受け持ちとして汎用の欄から外されていたのに、専用の欄は Shader の combo だけで、stage ごとの shader・複数の miss / hit・`hit_shaders`・callable・`acceleration_structure`・`max_recursion` の欄はありませんでした。New… で書ける shader は fullscreen の 3 種だけで、Blank の project から RT の task を組む道がありませんでした(仕様書の記述)。

- **ray_tracing は汎用の欄**: `render.compute` の専用の欄の受け持ちから `ray_tracing` を外しました([専用の欄が受け持つ名前の表の render.compute の行](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1562))。`ray_tracing` は、上の「node の欄を catalog の表から作る(✅UX-12a)」の **More parameters** の下に、畳める見出し(`pelican.graphCanvasParameters.ray_tracing.fold`)と field ごとの欄で出ます。Max recursion は uint32 の spin、`acceleration_structure` は `choices_from` の combo、`traced_ray_types` / `misses` / `hit_shaders` / `callable_record` / `callable_records` は object・array・union の汎用の部品です(実装者の報告)。専用の widget は作っていません。
  - 行が出るのは、文書の parameters に `ray_tracing` がある node だけです。shader で走る compute の task には出ません(2 つは排他で、どちらにするかは Shader の combo で選ぶ。[ray_tracing の行を隠す所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1723))。
- **stage ごとの shader の候補**: 名前が `raygen` / `miss` / `closesthit` / `anyhit` / `intersection` / `callable` の field と `misses`(miss の shader の map)は、shader の combo の候補をその stage のものに絞ります([field の名前から stage を決める所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L932))。候補は project の `shaders/` の下の、その stage の拡張子(`.rgen` / `.rmiss` / `.rchit` / `.rahit` / `.rint` / `.rcall`)のファイルと、engine のその stage の shader です([stage で絞る所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1127))。`hit_shaders` の中の `closesthit` / `anyhit` / `intersection` にも効きます。`render.compute` 自身の `shader` は今までどおり compute の shader です。
- **union に Shader と List**: union の形の呼び方に、`shader` の形の `Shader` と `array` の形の `List` を足しました([形の呼び方](../../src/devstudio/view/graphcanvasparameterfields.cpp#L348))。これが無いと、文字列の `miss` が `(none)` と出ていました(実装者の報告)。1 本の shader を List に替えると、その shader を 1 行目に残します([1 本を List の 1 行目に残す所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L835))。Miss と Closest hit を複数にする専用の「+」は作っていません。List の行の **+** で足します。`dispatch.groups` も同じ union で、List / Custom です。
- **dispatch の port**: `dispatch` の欄は CT-6 の catalog からそのまま出ます(仕様書の記述では、CT-6 の前は JSON の 1 行でした)。`groups_from` / `rays_from` の `port` は打つ欄ではなく、その node の `ports` のうち image の port(`kind` が無いか `image`)の名前の combo です。buffer の port は出しません([task の image port を候補にする `taskPortChoices()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L188))。`indirect.buffer` は今までどおり `choices_from` の編集できる combo です。
- **New… —— RT の雛形**: compute の欄の **Shader** の横に **New…**(`pelican.graphCanvasCompute.newShader`)が付きました。押すと下に行が開き、種類の combo(`.shaderTemplateKind`。項目は `Ray tracing (raygen + miss + closest hit)` の 1 つ)、名前(`.shaderTemplateName`)、**Create**(`.createShader`)、1 行の notice(`.shaderTemplateNotice`)が出ます。
  - 名前には空いている名前を入れてあります。`ray_tracing` から始め、`.rgen` / `.rmiss` / `.rchit` のどれか 1 つでもある名前は `_2`、`_3`… と飛ばします([`rayTracingTemplateName()`](../../src/devstudio/view/fullscreenshadertemplate.cpp#L1030))。提案の上に打った名前はそのまま残ります([`GraphCanvasComputeFields::proposeTemplateName()`](../../src/devstudio/view/graphcanvascomputefields.cpp#L186))。打たずに Create を押せます。
  - Create は `shaders/<名前>.rgen` / `.rmiss` / `.rchit` の 3 つを全部書くか、1 つも書かないかのどちらかで、既にあるファイルは上書きしません([`writeRayTracingTemplate()`](../../src/devstudio/view/fullscreenshadertemplate.cpp#L1039))。1 つでもあれば notice が赤で `shaders/<名前> already has a .rgen, .rmiss or .rchit and was left as it is: name the new set otherwise.` を言います。名前が文字・数字・`_`・`-` 以外を含めば `A shader name is letters, digits, '_' and '-'.` です。
  - 書けたら一覧を読み直し、上の「別の shader を選ぶ」と同じ `GraphCanvasModel::setComputeShader()` でその組を trace させ(ports は `get_shader_interface` から作り直す)、notice が `Wrote shaders/<名前>.rgen, .rmiss and .rchit; the task traces with project://shaders/<名前>.` を言います([`GraphCanvasComputeFields::createTemplate()`](../../src/devstudio/view/graphcanvascomputefields.cpp#L202))。task が今受けられなければ `…, but the task could not take it now: pick it from the Shader list.` です。
  - project が開いていない間と、編集できない間は押せません。
- **palette の New…**: palette の `render.compute` の行の shader の combo の横にも **New…**(`pelican.graphCanvasPaletteComputeNew`)があります。こちらは名前の欄を出さず、提案した名前で 3 つを書いて、その組(`<参照>  (ray tracing)`)を combo で選びます([palette の New… の所](../../src/devstudio/view/graphcanvaswidget.cpp#L8034))。OK で置く道は上の EL-5s と同じです(engine が 3 つの stage を compile して読んでから置き、`color` の port に `<node>__color` の image が付き、`dispatch.rays_from` は `color`)。書けなければ palette の下の 1 行が `Could not write the shaders: <理由>` を言います。palette を Cancel しても、書いたファイルは残ります(Pass の欄の New… と同じ。実装者の報告)。
  - fullscreen の Pass の欄の New…(✅UX-5b)には RT を足していません。あちらは fragment を書く欄だからです(実装者の報告)。
- **雛形の中身**([`rayTracingShaderTemplate()`](../../src/devstudio/view/fullscreenshadertemplate.cpp#L900)): engine の `rt_shadow_mask_pipeline` と同じ、ray group layout の無い形です。3 つは 1 つの stem で、task は `raygen` / `miss` / `closesthit` に同じ参照を書きます(stage がファイルを選ぶ)。
  - raygen: `pelican_sets.glsl` と `pelican_frame.glsl` を include し、書く image `pelican_resource_color`(`rgba16f`、storage)と、frame の set の ray query の binding(`PELICAN_RAY_QUERY_TLAS_BINDING`)の scene の acceleration structure を宣言します。画素ごとに camera(frame の view と projection)から 1 本 trace し、当たれば右と下の画素を通る 2 本も trace して、3 つの当たった位置が張る面の法線を色(`n * 0.5 + 0.5`)にします。当たらなければ、光線の上下の向きで明るさの変わる暗い空の色です。
  - miss: `hit.w = 0.0;` の 1 行だけです(当たらなかった印。空の色は raygen が作ります)。
  - closest hit: 当たった位置(`gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT`)と当たった印を payload に返します([closest hit の雛形](../../src/devstudio/view/fullscreenshadertemplate.cpp#L993))。仕様は「法線を返す closest hit」でしたが、layout の無い形では closest hit が三角形の頂点を読めないので(頂点を読む `pelicanRayFetchNormal` は ray group layout の生成 header にしか無く、engine は position fetch の拡張も有効にしていない)、法線は raygen が隣の光線から作ります(実装者の報告)。
- **Task Editor の New ray tracing**: 同じ雛形を書いてその参照を名指す草稿になりました(§13.5「レンダー設定を編集する」の「New compute / New ray tracing」の段落)。
- **RT の拒否の 1 文**: compute task の parser の RT の拒否 16 種を、inspectormodel の 1 文の表にしました([16 種の表](../../src/devstudio/model/inspectormodel.cpp#L780)、[`rayTracingRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L834))。code が拒否の code か message の中にあれば、`<文> (<code>).` を返します。task の名前と、message が引用する field / stage / miss / ray type / group を文に入れます。`knownRefusalDescription()` から呼ぶので、Graph の notice もこの文です。
  - 対象: `pelican.ray_layout_mode_change_requires_restart@1` と、`pelican.ray_tracing.` の `legacy_syntax_with_layout@1` / `unknown_shader_stage@1` / `unsupported_shader_stage@1` / `shader_stage_required@1` / `invalid_shader_reference@1` / `traced_ray_types_invalid@1` / `miss_shader_missing@1` / `miss_shader_unknown@1` / `hit_shader_missing@1` / `hit_shader_untraced@1` / `hit_shader_unknown_group@1` / `invalid_recursion@1` / `invalid_trace_dispatch@1` / `rays_from_requires_pipeline@1` / `trace_extent_required@1`(どれも `@1`)。
  - layout の形の食い違い: layout のある project で `miss` / `closesthit` を書くと `This project has a ray group layout: Task <task> names its shaders in traced_ray_types, misses and hit_shaders, not miss and closesthit`。layout の無い project で `traced_ray_types` / `misses` / `hit_shaders` を書くと(engine は `unknown_shader_stage@1` で断る)`This project has no ray group layout: Task <task> names its shaders in miss and closesthit, not <field>` に読み替えます。
  - mode の変更: `ray_layout_mode_change_requires_restart@1` は `Whether the project has a ray group layout changes only when the engine restarts: restart it to apply this` です。仕様は「mode の変更は RSX の `restart_required` の道へ」でしたが、今の engine はこれを拒否で返し、文書は書かれません。Studio ができるのは 1 文にするところまでです(実装者の報告。engine に回しました。下)。
  - 表に無い code は今までどおり engine の message のままです。
- **ray_query**: raster / fullscreen の pass の `ray_query` の欄は変えていません(✅UX-12e の union のまま)。
- **残した穴**(実装者の報告):
  - 汎用の欄で `raygen` を別の stem に替えても、ports は作り直しません。ports を shader に合わせるのは Shader の combo(`setComputeShader()`)だけです。
  - Task Editor の New compute は、まだ無い `project://shaders/compute` を名指したままです(compute の雛形はありません)。
  - ray type / miss / group の名前(`hit_shaders` / `misses` の鍵、`traced_ray_types` の要素、`callable_record` の `name`)は、候補の無い打つ欄のままです。
  - 既存のシナリオ `gui_build_ocean_compute`(palette の compute の行と compute の欄を通る)は、実装者は流していません。
- **手で打つ欄**: New… の名前(空いている名前が入っているので打たなくてよい。palette の New… には名前の欄が無い)。Max recursion は spin です。ray type / miss / group の名前は上のとおり打つ欄のままです。
- **engine に回したもの**(実装者の報告): (1) `ray_layout_mode_change_requires_restart@1` を拒否ではなく `restart_required`(`restart_reason.code` にこの code)で答える —— そうなれば Studio は RSX の道でそのまま再起動する。(2) layout の無い形の closest hit が法線を読める口(position fetch か、layout 無しでも頂点の表を読める include)—— そうなれば雛形を「closest hit が法線を返す」形にできる。(3) RT の task の parser の拒否に fields を付ける —— 今は `code: 文` の形で、Studio は message から `task '<名前>'` や `field '<名前>'` を拾っている。
- test: `devstudio_graphcanvas_scene_test` の [「CT-6s decision 1: ray_tracing is the generic fields' -- each stage lists that stage's shaders, Max recursion a spin, a miss made a list keeps its shader -- and a task that runs a shader has none」](../../test/devstudio_graphcanvas_scene_test.cpp#L12666)・[「CT-6s decision 2: dispatch is the catalog's fields -- groups a list or x / y / z, rays_from's port one of the task's image ports, indirect's buffer the graph's dispatch buffers」](../../test/devstudio_graphcanvas_scene_test.cpp#L12732)・[「CT-6s decision 3: New... beside a compute task's Shader writes a ray tracing set (raygen + miss + closest hit) under a free name it proposes, and the task traces with it; nothing already there is overwritten」](../../test/devstudio_graphcanvas_scene_test.cpp#L12765)・[「CT-6s decision 5: a ray tracing task's refusal reads as one sentence -- the layout's form where the project has none and the other way round, a field the engine does not read, and the layout mode that changes only on a restart」](../../test/devstudio_graphcanvas_scene_test.cpp#L12842)(決定 4 の ray_query は変えていないので test はありません)。GPU シナリオは `gui_build_rt_from_blank`(下の「Studio の自動化面」)です。実装者の run で `studio_cpu`(`ctest -L studio_cpu -LE gpu`)は 677 / 677 が緑でした。

**保存中でも palette が開く、確定前の値を送る、voice の帯、起動時の致命の 1 文(✅UX-9、`4d11aa96`)**:

- **保存中でも palette が開く**: raster の pass の欄の Depth で `New depth image (D32_SFLOAT)` を選んだ後に **Add** を押しても、palette が出ないことがありました(`gui_build_ocean` の途中から作った再現。仕様)。commit の記録の原因は、node への click が値の帯のその場編集の editor を開き、Add が focus を取ったときに editor が変わっていない値を確定して送り、その保存の間に palette を開く関数が何も言わずに戻っていたことです。今は次のとおりです。
  - 帯のその場編集は、文書の値と同じなら何も送りません([帯の変わらない値を送らない所](../../src/devstudio/view/graphcanvaswidget.cpp#L3909))。
  - palette は保存の最中でも開きます(開かないのは、engine が無いか文書をまだ読んでいないときだけ。[palette を開く所 —— 保存の最中でも](../../src/devstudio/view/graphcanvaswidget.cpp#L7727))。選んだものは、保存が届いて編集できるようになってから 1 回足し、その間の notice は `Saving the graph change...` です([保存が届いてから palette の選択を足す所](../../src/devstudio/view/graphcanvaswidget.cpp#L8209))。
- **palette と必須のダイアログは自分の event loop を回しません**: palette(`pelican.graphCanvasPalette`)と `<op> needs` のダイアログ(`pelican.graphCanvasRequired`)は window-modal で `open()` し、閉じた後に event loop から選んだものを処理します([palette が閉じた後に選んだものを読む所](../../src/devstudio/view/graphcanvaswidget.cpp#L8103))。UX-9 の前は `exec()` で開いていて、シナリオの Return / OK の直後の wait が palette の loop の中で処理されて追加が止まることがあり、Return の後に 0.4 秒待たせていました(仕様の背景)。`gui_build_music` と `gui_build_curve` はその待ちを外しました。
- **確定前の値を手放す前に送る**: LC-1s の Draw の spin と同じく、pass の欄(Defines の表の打ちかけの cell・clear colour・Draw の spin)、image の欄(大きさ・層・mip・式・format・usage)、値の欄の spin(Keys の時刻と値・playback の数・render.scalar の数)は、欄が別の node に移る前に、focus のある打ちかけの値をその node へ送ります(上の「送れないときは待って送ります」)。
  - 保存の最中なら、その node を名指したまま覚えて、編集できるようになったときに古い順に送ります([`GraphCanvasHeldEdits::sendOrHold()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L114))。image と pass の欄の parameters の編集は、覚えた後に同じ node の別の鍵が変わっていても、変えた鍵だけを当てます(JSON patch。[`heldParametersEdit()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L140))。Place の欄の選択も同じく覚えます。
  - image・Place・値の欄も、確定した時と覚えている間に `Saving the graph change...` を言います(LC-1s では pass の欄だけでした)。
- **voice の帯**: `audio.playback` の node の下の帯(GL-2b の帯の位置)に、engine の `get_status.audio.voices`(✅CT-1。[第8章](08_gameplay.md) の「`get_status` の `audio`」)の状態を `playing · 1.2 s` / `paused · 1.2 s` の形で出します(秒は `position_seconds` の小数 1 桁)。`idle` は、play の state が `edit` の間は出さず、Play / Pause の間だけ `idle` と出します([`graphCanvasVoiceBands()`](../../src/devstudio/view/graphcanvaswidget.cpp#L1366))。読むのは、Play の帯が受けた UX-7 の 1 回 parse した `get_status` の返事です([voice の帯へ get_status を渡す所](../../src/devstudio/view/mainwindow.cpp#L1735))。
- **起動時の致命の 1 文**: engine が起動時に構成を読めずに終わると(✅CT-1 から、致命の行 `Pelican fatal error : graph.port_field_missing@1 {...}` に code が出る)、Frame Plan の status 行が `The engine stopped: <code の 1 文> (<code>) in <file>. Fix the file (or Reload from disk) to start it again.` を言い、tooltip が致命の行そのものです([`engineStoppedSentence()`](../../src/devstudio/model/inspectormodel.cpp#L1769)、[起動時の致命を 1 文にする所](../../src/devstudio/view/frameplanwidget.cpp#L1638))。
  - code は行の中の最初の `<名前空間>.<名前>@<版>` で、Studio が 1 文を持つ code のときだけこの形になります(持たなければ今までどおり行そのもの)。
  - file は、IA-1s の前は行の中の最後の `.json` の path でした。UX-9 の時点の engine の行は file を言わないので、project.json の `basic_config.rendering_config_json`(例 `passes/main.json`)を使い([`projectRenderDocument()`](../../src/devstudio/model/inspectormodel.cpp#L1756))、それも読めなければ `the project's render documents` です。形は UX-8 で入れ、UX-9 で実際の engine の行で確かめました(下の test)。
  - ✅CT-2(engine `51d1c2d8`)から、graph の拒否の致命の行には `"document":"project://passes/main.json"`(definition の body の node なら `definition` も)が入ります(第6章 §6.18 の CT-2 の項)。✅IA-1s から、file はまずこの `document` を `project://` を外して使い、`definition` があれば `definition <名前> of <文書>` にします(例 `... in definition blur of passes/main.json. ...`)。行の中の最後の `.json` の path は `document` の無い行のときだけ、project.json の推測はそれも無いときだけです([`engineStoppedSentence()` が document を読む所](../../src/devstudio/model/inspectormodel.cpp#L1769))。下の UX-9 の test は IA-1s で注記が「file は行が名指す文書(CT-2 の `document`)」に替わり、期待は `in passes/main.json` のままです(IA-1s の commit の差分)。
  - engine の ✅BS-1(`d9811bfc`)の後も、Studio は engine を `--render-fallback` 無しで起こすので、この道のままです。`--render-fallback` で起こして `get_status.render_pipeline` を読む側は未対応(作業中)です(§13.5 の「起動時に断られた構成を直す」)。→ ✅BS-1s(`19be8983`)から Studio は `--render-fallback` を付けるので、構成が組めなくても Studio の engine は止まらず、Frame Plan の status 行は `Showing a placeholder: …` を言います(下の「組めない構成でも engine を止めない(✅BS-1s)」)。この致命の行の読みは残してあり、フラグを知らない古い engine と、フラグを付けても致命のままのもの(root が JSON だが object でない、`pelican.graph` でない旧い形、material の登録で起きる `material_route_unserved@1`。最後の 1 つは BS-1s の実装者の報告)で使います。→ engine の ✅PB-1(`a55d1eaf`)から、起動の間の material の登録で起きる `material_route_unserved@1` は `--render-fallback` なら致命にならず fallback の世代に入ります(第6章 §6.16)。起動の後の登録とフラグの無い起動では致命のままです。WP298 の「Studio receives the running example fatal text while RPC stdout stays framed」は今もこの致命の行を期待していますが、PB-1 の着地の W1 には入っておらず、PB-1 の後に通るかは未確認です。→ ✅PB-1s(`93934318`)でこの test の期待は fallback に書き換わりましたが、engine が先へ進んで glTF の morph の上限で止まるので赤のままです(§13.1 の「Studio の Problems のドック」の test)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-9 decision 1: an unchanged pill sends nothing as it loses the keyboard, and Add opens the palette over a save in flight, adding its pick once the save lands」](../../test/devstudio_graphcanvas_scene_test.cpp#L10662)、[「UX-9 decision 2: a value typed into a side field goes to its node as the fields let go of it, held for that node while a save is in flight」](../../test/devstudio_graphcanvas_scene_test.cpp#L10784)、[「UX-9 decision 4: a voice's state reads as the band under its audio.playback -- playing and paused with where it is, idle only in a play session」](../../test/devstudio_graphcanvas_scene_test.cpp#L10865)、`devstudio_engine_failure_test` の「UX-9 decision 5: an engine stopped at start by a buffer port without its element says the code's sentence and the file in the Frame Plan's status」(engine を起動して、`Port data of sim needs element (graph.port_field_missing@1) in passes/main.json` を見る)。✅BS-1s でこの test は [「BS-1s decision 4 (UX-9 decision 5): an engine Studio starts on a buffer port without its element keeps running on its placeholder, and the Frame Plan's status says the code's sentence and the file」](../../test/devstudio_engine_failure_test.cpp#L700) に書き換わりました(engine が止まらず、致命の行が無く、最初の `get_status` が fallback で code が `graph.port_field_missing@1`・node が `sim`、Frame Plan の status 行が `Showing a placeholder: … Port data of sim needs element (graph.port_field_missing@1) in passes/main.json. Fix it, then Reload from disk.`)。致命の行の読みは `engineStoppedSentence()` の test と、WP298 の「Studio receives the running example fatal text while RPC stdout stays framed」(`material_route_unserved@1` はフラグを付けても致命)に残っています。→ ✅PB-1s で WP298 の case は致命の行を見なくなり、fallback の帯と Problems の行を見る期待になりました(engine 側の理由で赤のまま。§13.1 の「Studio の Problems のドック」の test)。

**一度だけ走る pass と、frame をまたいで残る image —— Schedule・Lifetime・once の帯・Recompute(✅PS-1s、`349af5e4`)**: engine の ✅PS-1(`render.image` の `lifetime`、pass / task の `schedule`、`rerun_once`、`get_frame_plan` の `last_run_generation` / `last_run_frame`。§13.5 の「`get_frame_plan` に足されたもの」、[第6章](06_rendering.md) §6.18「一度だけ走る pass と、frame をまたぐ image」)を Graph の欄から書けるようにしました。GB-3 の海は、初期スペクトル h0 を毎 frame 計算し直していました(仕様の背景)。

- **Schedule**: pass の欄の `pelican.graphCanvasPass.schedule` と compute の欄の `pelican.graphCanvasCompute.schedule` の combo です。fullscreen / raster は `Every frame` / `Once`、compute は `Every frame` / `Every view` / `Once` です([`GraphCanvasScheduleRow`](../../src/devstudio/view/graphcanvasrenderfields.hpp#L79))。`Every frame` は `schedule` の鍵を消し、ほかは `once` / `per_view` を書きます。
- **Once は書く image を同じ編集で Persistent に**: `Once` を選ぶと、pass が書く image(port の `access` が `write` / `read_write` か、`attachment` を持つ port。線を pass 越しに辿った先の `render.image`)のうち `lifetime: "persistent"` でないものに `lifetime: "persistent"` を書き、schedule と一緒に 1 回の編集で送ります(Undo 1 回。[`GraphCanvasModel::setNodeSchedule()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7978))。engine の `graph.once_writes_transient@1` を送る前に避けるためです。そのときの notice は `Graph change saved.` ではなく `Saved; <image> made Persistent: what a once pass writes is kept.` です。`Every frame` に戻すと schedule だけを消し、image の Lifetime は戻しません。
- **Run the body once**: Loop の欄の `Once` の行の check(`pelican.graphCanvasLoop.bodyOnce`)は、ループの全分岐の body の fullscreen / raster / compute の全部に `schedule: "once"` を書き、それぞれが書く image も上と同じく Persistent にします(1 回の編集。[`GraphCanvasModel::setLoopBodyOnce()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8030))。check が入って見えるのは body の pass と task が全部 once のときで、pass も task も無い body では押せません。外すと once の pass と task を毎 frame に戻し、`per_view` の task はそのままです。
- **Lifetime**: image の欄の combo(`pelican.graphCanvasImage.lifetime`)で、`Per frame`(鍵を消す。文書の `transient` もこれで出る)と `Persistent` です([Lifetime の combo を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L501))。
- **once の帯**: once の node の下の帯(GL-2b の帯の位置。root の node と、ループの body の node)は `once · ran gen <N>`(plan の `last_run_generation`)、まだ走っていなければ `once · not run yet` です。Persistent の image の帯は `persistent` です([once と persistent の帯の字を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L1331))。body の node は、そこから展開された回の pass のうち一番新しい世代と frame を出します([`GraphCanvasModel::onceRun()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8090))。詳細表には `once` の行(`ran in generation <N> · frame <M>` / `not run yet`)、Schedule の下には `Ran in generation <N>, frame <M>` / `Not run yet`(`.onceRun`)が出ます。
- **Recompute**: once の node の Schedule の横の **Recompute**(`pelican.graphCanvasPass.recompute` / `pelican.graphCanvasCompute.recompute`。once のときだけ出る)と、Loop の欄の **Recompute**(`pelican.graphCanvasLoop.recompute`。Run the body once が入っているときだけ出る)は `rerun_once {node}` を送ります。body の node は plan の node ではないので、その body を走らせるループの名前で送ります([`GraphCanvasModel::rerunOnce()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8148))。canvas の上のボタンの列(Undo / Redo / Auto layout / Fit view の列)の **Recompute once**(`pelican.graphCanvas.recomputeOnceButton`。action は `pelican.graphCanvas.recomputeOnce` の `Recompute all once passes`)は `rerun_once {}` で、engine につながっていて plan に once の pass があるときだけ出ます([Recompute all once passes の action を作る所](../../src/devstudio/view/graphcanvaswidget.cpp#L5396))。右クリックのメニューには入っていません。
  - 答えの notice は `Recompute: 1 once pass runs again on the next frame.`(2 つ以上なら `… once passes run again …`)です。`scheduled` が空なら `<node> runs every frame: there is nothing to recompute.`(`{}` なら `No pass runs once: there is nothing to recompute.`)、engine が `node_unknown` で断れば `<node> is not in the frame plan: there is nothing of it to recompute (node_unknown).`、ほかの失敗は `Recompute was refused: <message>` です([`GraphCanvasModel::receiveRerunOnce()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8171))。
  - 送った後は、送られた pass のどれかの `last_run_frame` が押した時の値より進むまで、Graph の poll ごとに `get_frame_plan` を読み直します(最大 40 回)。帯と欄の世代と frame はそれで進みます。
- **拒否の 1 文**: `graph.once_writes_transient@1` は `<node> runs once but writes <resource>, which is not kept between frames — make it Persistent`、`graph.once_reads_per_frame@1` は `<node> runs once but reads <input>, which changes every frame — it would freeze at the first value` です([once の 2 つの code の 1 文](../../src/devstudio/model/inspectormodel.cpp#L1197))。
- test: `devstudio_graphcanvas_test` の [「PS-1s Once keeps what the pass writes in the same edit, for a pass and a loop's body」](../../test/devstudio_graphcanvas_test.cpp#L4199)、[「PS-1s a once node says when it last ran, and Recompute asks rerun_once and follows it」](../../test/devstudio_graphcanvas_test.cpp#L4257)、[「PS-1s a once pass's refusals read as sentences」](../../test/devstudio_graphcanvas_test.cpp#L4334)。GPU シナリオは `gui_build_ocean` と `gui_build_ocean_compute` です(下の「Studio の自動化面」)。

**画像ファイルを image の初期内容に —— Initial contents・Size from the image・Import…・見張り、拒否の文書(✅IA-1s、`e2fd6dad`)**: engine の ✅IA-1(persistent の `render.image` を画像資産から始める `initial: {asset}`。[第6章](06_rendering.md) §6.18「persistent の image を画像資産から始める」)、IA-2(`cdcdebc1`。`reload_render_documents` が initial に使う asset の中身も比べ、答えに `changed_assets` を付ける)、✅CT-2(拒否の `document` / `definition`。§13.5 の「拒否の `document` と `definition`」)を Studio の操作にしました。GB-3 の LTC の面光源は、表(LUT)を読む口が無く、近似の式で代えていました(仕様の背景)。

- **Initial contents**(`pelican.graphCanvasImage.initial`): `None` と、project の中の `.ktx2` / `.png`(拡張子の大文字小文字は問わない。`.` で始まる directory は見ない)を project root からの path(`assets/images/ltc_1.ktx2`)で並べた combo です([`readProjectImageAssets()`](../../src/devstudio/model/graphcanvasvalues.cpp#L926))。一覧は欄を読み込むたびに読みます。Lifetime が `Persistent` の間だけ押せ、`Per frame` の間は灰色で、tooltip が `Make it Persistent to start from an image` です(engine は transient の image の initial を `graph.image_initial_requires_persistent@1` で断るため)。選ぶと `initial: {asset}` を書きます。`width` / `height` が無ければ同じ編集で `extent_scale` も消し、file の大きさを採らせます(engine は `width` / `height` を省いて `extent_scale` を書いた image を `graph.image_initial_extent_conflict@1` で断るため)。`None` に戻すと `initial` を消し、大きさを file から採っていたなら `extent_scale: 1` と file の幅・高さを書いて、今の大きさを残します([`GraphCanvasImageFields::commitInitial()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L864))。
- **Size from the image**(`pelican.graphCanvasImage.fromImage`): initial があるときだけ押せる check です。file の header(KTX2 の幅と高さ、PNG の IHDR)が読めれば `Size from the image (64 × 64)` と大きさを言います([`readImageAssetExtent()`](../../src/devstudio/model/graphcanvasvalues.cpp#L965))。入れると `width` / `height` / `extent_scale` を消し、外すと `extent_scale: 1` と file の大きさ(読めなければ spin の値)を書きます(どちらも 1 回の編集。[`GraphCanvasImageFields::commitFromImage()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L890))。入っている間は Size の欄が押せません。
- **Import…**(`pelican.graphCanvasImage.import`): Initial contents の横のボタンで、Persistent で project が開いているときだけ押せます。Qt のダイアログ(`pelican.imageImportDialog`、名前の欄 `pelican.imageImportDialog.fileName`。window-modal で、いくつでも選べる)で選んだ `.ktx2` / `.png` / `.hdr` を project の `assets/images/` に写します。同じ名前があれば `_2`、`_3` … を付け、上書きしません([`importProjectImage()`](../../src/devstudio/model/graphcanvasvalues.cpp#L999))。写した最初の file をその image の Initial contents にし、notice は `Imported <参照>.`(2 つ以上なら `Imported <参照> and <N> more.`)です。
  - `.hdr` は写した後、Studio が子 process で `pelican_cli image ktx2 --out <空いた名前の .ktx2> --hdr <写した .hdr>`([第10章](10_tools.md) §10.4)を走らせ、できた `.ktx2` を選びます。notice は `Imported <参照> (baked from <hdr の名前>).` です([`GraphCanvasImageFields::bakeHdr()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L976))。CLI は環境変数 `PELICAN_STUDIO_CLI`、無ければ Studio と同じ directory の `pelican_cli`(Windows は `.exe`)です([焼き込みの CLI の場所を決める所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L782))。起こせなければ `Could not start pelican_cli (<path>) to bake the .hdr into a .ktx2.`、失敗すれば `Could not bake <hdr の名前> into a .ktx2: <CLI の出力>` です。焼き終わったときに別の image を選んでいれば、写したことだけを言います。
  - `.hdr` から CLI を起こす所は、test もシナリオも通していません(下の decision 4 の test は写すことと変換の指示まで、`gui_build_ltc` は `.ktx2` を Import します)。
- **拒否の 1 文**(どれも末尾は `(… <code>).`):
  - `graph.image_initial_requires_persistent@1`: `<node> starts from an image but is not kept between frames — make it Persistent`
  - `graph.image_initial_extent_conflict@1`: `<node> takes its size from its image and from the screen at once — check From the image, or give it a fixed size`
  - `graph.image_initial_mismatch@1`: `<asset> is <actual> but <node> is <expected> — bake the image to match (pelican_cli image ktx2) or change the image's …` で、`what` ごとに最後が替わります。`extent` は `size`(大きさは `64×64` の形)、`format` は `format`(format の名前)、`color_space` は `format to the other color space`(`sRGB (<format>)` か `linear (<format>)`)です([mismatch の `what` ごとの文](../../src/devstudio/model/inspectormodel.cpp#L1234))。
- **Frame Plan**: Resources のタブの行の `Resource facts` に `Initial contents` の行が出ます。字は `initial: <asset> · sha256 <manifest の値の先頭 8 文字>`、manifest に無ければ `initial: <asset> · not in the manifest` です。書く pass の無い image では、Writers の列にも同じ字が出ます([Resources の行に initial を出す所](../../src/devstudio/view/frameplanwidget.cpp#L4599))。
- **見張り**: Graph は文書(root と各 definition の body)の `render.image` の `initial.asset` を集め(重複なし、並べて。[`GraphCanvasModel::initialAssets()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8130))、変わるたびに MainWindow の読み直し役に渡します([initial の asset を読み直し役へ渡す所](../../src/devstudio/view/mainwindow.cpp#L1299))。RL-1s の文書の見張り(下の「外のエディタで書き換えた render の文書を拾います(✅RL-1s)」)が、文書と並べてこれらの file も見張り、sha256 が変われば文書と同じく `reload_render_documents` を頼みます([`RenderDocumentReload::setInitialAssets()`](../../src/devstudio/model/renderdocumentreload.cpp#L136))。
  - 答えに `changed_assets` があれば(✅IA-2 の engine、`cdcdebc1`)、notice は `Picked up the change to <asset>.` です。`changed_assets` を持たない engine は `no_change` を返すので、notice は `<asset> changed — the engine reads it on its next start.`(Information)です([`RenderDocumentReload::committed()`](../../src/devstudio/model/renderdocumentreload.cpp#L358))。
  - 断られたとき、Studio が 1 文を持つ拒否(asset が image に合わない、など)なら、その文を detail の先頭に置きます。engine が切れたら、変わった asset の覚えを捨てます(起動する engine は今の file を読むため)。
- **拒否の文書(CT-2)**: graph の拒否の 1 文の末尾は、engine が `document` を付けていれば `(in <document>, <code>).`、definition の body の node なら `(in definition <definition> of <document>, <code>).` です。`<document>` は `project://` を外した path(`passes/main.json`)です([1 文に文書と definition を添える所](../../src/devstudio/model/inspectormodel.cpp#L1542))。文が自分で definition を名指す code(文に `<definition>` を持つもの。`graph.definition_*` など)には definition を添えません。起動時の致命の 1 文も同じ値を読みます(上の UX-9 の段落の CT-2 の項)。
- test: `devstudio_graphcanvas_scene_test` の [「IA-1s decision 1: Initial contents lists the project's images, is off on a per-frame image, and writes initial.asset with the size from the image; the check gives a fixed size back」](../../test/devstudio_graphcanvas_scene_test.cpp#L10911)、`devstudio_graphcanvas_test` の [「IA-1s decision 2: an image's initial-contents refusals read as sentences, a mismatch in the words of what differs」](../../test/devstudio_graphcanvas_test.cpp#L4375)・[「IA-1s decision 4: images are copied into assets/images, a .hdr to be baked into a .ktx2 beside it, and the .ktx2 and .png are what Initial contents lists」](../../test/devstudio_graphcanvas_test.cpp#L4419)・[「IA-1s decision 6: a graph refusal names the document and the definition the engine says its node was written in (CT-2), Studio's guess only without them」](../../test/devstudio_graphcanvas_test.cpp#L4454)、`devstudio_frameplan_test` の [「IA-1s decision 3: a physical resource names the asset it starts from and its manifest sha256」](../../test/devstudio_frameplan_test.cpp#L1411)、`devstudio_render_reload_test` の [「IA-1s decision 5: the images' initial assets are watched; changed_assets says it was picked up, an engine without it that it is read on the next start」](../../test/devstudio_render_reload_test.cpp#L435)。GPU シナリオは `gui_build_ltc` です(下の「Studio の自動化面」)。

**Graph の material pass の欄を Pass Editor の form で —— Tags・出力・binding・Load / Clear / Store、Materials drawn(✅UX-11、`0ac261f4`)**: UX-11 の前の Graph の `render.material_pass` の欄は Contract / Variant(上の LP-4e / UX-3 の Draw の欄)と Place / View だけで、`material_filter`・`material_range`・`material_outputs`・`screen_inputs` などと load / store / clear は、詳細表の JSON か帯で打つしかありませんでした(仕様の根拠)。UX-4a の Pass Editor の form(下の「生の JSON の欄はフォームになりました(✅UX-4a)」)を Graph に載せ、Tags を engine の tag から選ぶ形にしました。

- **欄**(`pelican.graphCanvasMaterial`。[`GraphCanvasMaterialPassFields`](../../src/devstudio/view/graphcanvasmaterialfields.hpp#L44)): material pass を選ぶと Draw の欄の下に出ます。中身は Pass Editor の [`MaterialSettingsForm`](../../src/devstudio/view/passforms/materialsettingsform.hpp#L42) と [`AttachmentOpsForm`](../../src/devstudio/view/passforms/attachmentopsform.hpp#L26) で、子の名前は Pass Editor の名前の prefix を `pelican.graphCanvasMaterial` に替えたもの(Tags の any of なら `pelican.graphCanvasMaterial.materialFilter.include`)です。Contract と Variant は Draw の欄にあるので、form のその 2 行は隠します(1 つの鍵に欄は 1 つ。[`MaterialSettingsForm::setContractAndVariantShown()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L556))。
- **書き方**: どの変更も node の parameters 全体を 1 回の `set_node_parameters` で送ります(Undo 1 回。form が知らない鍵はそのまま送る)。名前・binding・clear colour のように打つ欄は、打つ手が 500 ms 止まってから 1 回送ります(UX-11 の着地時は Other (JSON) もこの扱いでした。✅UX-15a で Other (JSON) は `gpu_draw_source` の欄に替わりました。下の「生の JSON の欄はフォームになりました(✅UX-4a)」の gpu_draw_source の欄)。保存の最中の値は UX-9 の `GraphCanvasHeldEdits` で覚え、欄が node を手放す前に打ちかけの値を送ります。
- **Load / Clear / Store**(`Attachments` の行): graph の material pass は出力の一覧を持たないので、color の行は pass の `color_load_op` / `color_store_op` / `clear_color`(color の socket の全部に共通の 1 つ)、depth の行は `depth_load_op` / `depth_store_op` に書きます。color の socket が複数あるとき、どれか 1 行を変えると pass の値が変わり、読み直すと全部の行がその値になります([`GraphCanvasMaterialPassFields::withForms()`](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L195))。depth の行は `depth` の socket につながった image の名前で出ます。
- **Tags**: Pass Editor と Graph の両方で、`material_filter` の any of / none of は Loop の欄と同じ tag の複数選択(✅UX-3 の [`GraphCanvasTagPicker`](../../src/devstudio/view/graphcanvastagpicker.hpp#L27)。UX-11 で自分の file に移りました)です。候補は engine の `get_render_authoring_context` の `material_tags` で(Graph は model が持つ authoring context から、Pass Editor は開くときに読む context から。[Pass Editor が material_tags を読む所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2718))、pass が名指す tag は一覧に無くても check の入った項目で残ります。一覧に無い tag は、menu の最後の **+ new tag** の行(`<picker の名前>.newTag`)に打って Enter で足します(tag を打つのはここだけです)。何も選んでいなければ、include は `any tag (every material)`、exclude は `leave none out` と出ます。UX-4a から UX-11 の前までは `,` 区切りで打つ欄でした。
- **Materials drawn**(`pelican.graphCanvasMaterial.drawn`): 欄の下の表で、frame plan の `material_routing` がこの pass へ送る route の行(`route <route>` と契約)と、engine の material のうち pass の Tags(any of のどれかを持ち、none of のどれも持たない)と Variant(書いてあればそれを持つ)が通すものの行(名前と tag。組み込みは `<名前> (built in)`)を並べます([Materials drawn の表を作る所](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L451))。Tags と Variant は plan が言う値を先に、無ければ文書の値を使います。UX-11 の時点の engine は pass ごとに描いた material を出さなかったので、これは近似で、見出しがそう言います: `Materials drawn -- those its Tags and Variant take`(plan が描画の数を出していれば `Materials drawn (<N> draw(s) this frame) -- those its Tags and Variant take`)。✅UX-12c から、plan がその pass の `drawn_materials`(engine の MP-1)を言えば実際に描いた material の表に替わり、近似は plan が言わない間だけで、見出しの末尾に `; the engine has not said what it drew` が付きます(下の「Materials drawn を実物で(✅UX-12c)」)。行を押すと Frame Plan が前に出て、**Materials** のタブで Target pass がこの pass の行が選ばれます([Materials のタブで pass の行を選ぶ所](../../src/devstudio/view/frameplanwidget.cpp#L3414))。routing がこの pass を名指さなければ、status 行が `The frame plan routes no material to <pass>.` を言います。
- **UX-11 の時点で選ぶ形でなかったもの**: `screen_inputs` / `surface_resources` の名前の候補は、UX-11 の時点では Studio の固定の表(`opaque_color` など 4 つと `directional_shadow` など 3 つ)でした。仕様は「engine の契約に載っていればそれを読む」としていましたが、UX-11 の commit はこの表を変えていません。✅UX-12c から、Graph の欄では engine の一覧(MP-1)から作り、この表([MP-1 の前の engine 用の名前の表 `fixedResourceNamesOf()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L71))は一覧の無い engine と Pass Editor の form のときだけです(下の UX-12c の段落)。仕様が短くする対象に挙げた `gui_build_full` / `gui_build_D` も、UX-11 の commit では変わっていません(`gui_build_D` は UX-12c で延びました)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-11 decisions 1 and 4: a material pass's Tags, outputs, bindings and attachment ops are the Pass Editor's forms on the Graph, each one set_node_parameters; Materials drawn lists the plan's routes and the materials its filter takes, and a row opens the Frame Plan's Materials」](../../test/devstudio_graphcanvas_scene_test.cpp#L11068)、`devstudio_pass_forms_test` の [「UX-11 decision 2: MaterialSettingsForm's Tags are picked among the engine's material tags, a tag the pass names stays checked, and a new one is typed in + new tag」](../../test/devstudio_pass_forms_test.cpp#L402)。GPU シナリオは `graph_from_blank` の後半です(下の「Studio の自動化面」)。

**Materials drawn を実物で、binding の名前を engine の一覧から —— drawn_materials・Drawn の列・Reads を型で絞る(✅UX-12c、`708c4055`)**: engine の ✅MP-1(`afd923e1`。§13.5 の「`get_frame_plan` に足されたもの」の material pass の node の行と、上の `material_pass_contracts` の `screen_inputs` / `surface_resources`)を Studio の欄にしました。UX-11 の Materials drawn は Tags と Variant から推した近似で、deferred の pass に組み込みの transparent が出るなど外れていました(仕様の背景)。

- **Materials drawn を実物に**: plan のその pass に `drawn_materials` があれば、Graph の material pass の欄の Materials drawn(`pelican.graphCanvasMaterial.drawn`)は、描いた material を 1 行ずつ並べます。列は `Material` と `Draws` です([描いた material の行を作る所](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L402))。
  - 行の名前は material の名前です。同じ名前の material が 2 つ以上描かれていれば `<名前> (material <id>)`、名前が無ければ `material <id>`(id が数字でなければ id そのもの)です([描いた material の呼び方 `framePlanDrawnMaterialLabel()`](../../src/devstudio/model/frameplanmodel.cpp#L1276))。engine の組み込みの material は後ろに ` (built in)` が付きます。行の tooltip は id です。例(シナリオ `gui_build_D`): Cube と Floor の基本形の material は名前が同じなので `pelican.primitive.default (material 1)` と `pelican.primitive.default (material 2)` の 2 行です。
  - 見出し(`pelican.graphCanvasMaterial.drawnHeading`)は `Materials drawn as of frame <N> -- <M> draw(s)` です。N は plan の `drawn_materials_frame`、M は draws の合計です。`drawn_materials_truncated` なら ` (first <件数> shown)` が付きます。その frame に何も描かなかった pass は `It drew no material in frame <N>` の 1 行です。
  - 行を押すと、UX-11 と同じく Frame Plan の **Materials** のタブでその pass の行が選ばれます(何も描かなかった行も同じです)。
  - plan が `drawn_materials` を言わない間(publish の後にまだ frame が無い、MP-1 の前の engine)は UX-11 の近似のままで、見出しの末尾に `; the engine has not said what it drew` が付きます。そのあいだ Graph は、poll の時計で `get_frame_plan` を読み直し(同時に 1 本、最大 12 回)、どれかの pass が `drawn_materials` を言った時点でやめます([描いたものが来るまで plan を読み直す `GraphCanvasModel::followDrawnMaterials()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7165))。
- **Frame Plan**: material pass の node の詳細に `Materials drawn` のグループが出ます。グループの行は `<N> material(s), <M> draw(s) (frame <F>)`(truncated なら `, the first <件数> listed` が付く)で、子の行が material ごとの draws です([Materials drawn のグループを足す所](../../src/devstudio/view/frameplanwidget.cpp#L4993))。**Materials** のタブには 6 列目 **Drawn** が増えました。route の行の Drawn は、その route の Target pass が描いたものの要約です。route の行の後に、描いた material ごとの行(Route の列に material の名前、Target pass の列に pass、Drawn の列に `drawn by <pass>, <N> draw(s)`)が並びます([Materials のタブに描いた material の行を足す所](../../src/devstudio/view/frameplanwidget.cpp#L5002))。route が 1 本も無い plan でも、描いた material の行は出ます。
- **binding の名前を engine の一覧から**: Graph の material pass の欄の binding の行(`MaterialSettingsForm` の **Add binding**)で、`screen_inputs` / `surface_resources` の名前の combo は catalog の `material_pass_contracts` の一覧から作ります(contract をまたいで名前ごとに 1 つ、catalog の順。[`GraphCanvasModel::materialPassInputs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7148))。項目の tooltip は `reads a <source_type>` です。**Add binding** が足す最初の行の名前は、一覧の最初の screen input(一覧が無ければ `opaque_color`)です。一覧が無い(MP-1 の前の)engine では、今までの固定の表です。Pass Editor の form にはこの一覧を渡していないので(`setResourceNameCandidates()` を呼ぶのは Graph の欄だけ。コードの読み)、Pass Editor では今も固定の表です。
- **Reads を型で絞る**: 行の Reads(読む target)の combo は、名前の `source_type` と違う semantic type を持つ target を出しません([Reads を型で絞る `MaterialSettingsForm::fillResourceReads()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L677))。型の分からない target と、型の分からない名前の行では絞りません。target の型は、root の `render.image` / `render.target_ref` の `type.semantic`、無ければ authoring context の `render_targets` の宣言から読みます([`GraphCanvasModel::targetSemantic()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7173))。
- **読む物の無い binding は送らない**: Graph で、Reads が空の binding の行は編集になりません(engine は空の target を断るため)。Reads を選んだときに 1 回送ります([読む物の無い binding を送らない所](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L216))。UX-12c の前は空の行も送り、いつも断られていました(commit の記録)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-12c decision 1: Materials drawn lists the materials the plan says the pass drew in its last frame, with their draws, and falls back to the Tags' approximation, saying so, while the plan does not say」](../../test/devstudio_graphcanvas_scene_test.cpp#L11212)・[「UX-12c decision 3: a binding's names are the engine's list for its kind, and its Reads offers the targets of the type the name reads」](../../test/devstudio_graphcanvas_scene_test.cpp#L11258)、`devstudio_frameplan_graph_test` の [「UX-12c decision 2: a material pass's row says how many materials it drew and their draws, and the Materials tab lists each, drawn by the pass」](../../test/devstudio_frameplan_graph_test.cpp#L12509)。GPU シナリオは `gui_build_D` の後半です(下の「Studio の自動化面」)。

**node の欄を catalog の表から作る —— 汎用の欄・必須の印・Reset・拒否の行(✅UX-12a、`ac7c5250`)**: engine の ✅CT-3(`0d215a68`。`get_graph_catalog` の各 op の `parameters` と `parameters_complete`、拒否の `graph.render_node_parameter_invalid@1`。[第6章](06_rendering.md) §6.17「各 op の parameter の形」)を Graph の右の欄にしました。UX-12a の前は、多くの op の parameter は詳細表の JSON の行か、値の帯の型の推定でしか書けませんでした(`pose.clip_sample` の start_time / root_motion、`value.trigger_clock`、置いた後の `render.buffer` の size / lifetime など。仕様の背景)。

- **欄**(`pelican.graphCanvasParameters`。[node の parameter の欄 `GraphCanvasParameterFields`](../../src/devstudio/view/graphcanvasparameterfields.hpp#L200)): node を選ぶと、その op の `parameters` の 1 件ごとに 1 行を作ります。見出し(`.heading`)は `Parameters (<op>)` です。表が空の op、ループ、instance の node には出ません。行の名前の部品は `pelican.graphCanvasParameters.<名前>` です。
- **専用の欄を持つ op**: `render.image` / `render.fullscreen` / `render.raster` / `render.compute` / `render.material_pass`、`value.curve` / `value.playback`、`render.scalar`、`audio.playback` / `audio.volume` / `audio.pitch`、`ui.value` / `ui.visible` は今までの欄が先です。汎用の欄は、畳んだ **More parameters**(`.more`)の下に、専用の欄が出さない名前だけを並べます([専用の欄が受け持つ名前の表](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1549))。Place の欄を持つ op では `insert` / `pass_set` / `canonical_anchor` も除き、View の欄がある node では `view_select` の行を隠します。`render.compute` の `ray_tracing` は、UX-12a の着地時は専用の欄の受け持ち(実際の欄は Shader の combo だけ)でしたが、✅CT-6s から More parameters に出ます(`ray_tracing` を持つ node だけ。上の「RT を Graph で組む(✅CT-6s)」)。`pose.clip_sample` は下の Pose の欄で出します。engine の ✅CC-2(`1501685a`)が catalog の `render.raster` に足した `clip_y`(enum `down` / `up`、既定 `down`。[第6章 §6.1](06_rendering.md) の「座標の規約」)は専用の欄の表に無いので、コードの読みでは More parameters に enum の欄として出ます(画面では未確認)。✅UX-14 の commit の記録も、More parameters に出ることと、Studio の雛形が NDC と uv の変換を自前でしないことを言います(どちらも見る test はありません。コードの読みでは、`src/devstudio` の雛形にある `* 2.0 - 1.0` は normal の decode だけです)。
- **kind ごとの部品**([1 つの parameter の欄 `GraphParameterEditor`](../../src/devstudio/view/graphcanvasparameterfields.hpp#L92)):
  - `number` / `integer`: spin(UX-10 の feature の Parameters と同じ部品)。`min` と `max` が両方あって幅が 1,000,000 以下なら slider(`.slider`)も付きます。spin は表の範囲で止めません —— 範囲の外は engine が名前で断り、下の拒否の行になります([数の欄を作る所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L528))。✅UX-12e から、表に `soft_min` / `soft_max` があれば slider はその範囲です(下の節)。
  - `bool`: check。`enum`: 表の値の combo(既定の値は `<値> (default)`、既定の無い任意の enum には `(none)`)。`string`: 打つ欄(Studio が値を知っている名前だけ combo。✅UX-12e から、表に `choices_from` があれば、その出どころの一覧の編集できる combo。下の節)。`color`: 色見本と R / G / B / A。`vec2`〜`vec4`: spin の並び。
  - `asset`: project の file の combo と **Import…**(`.import`)です。Import… は Qt のダイアログ(`pelican.assetImportDialog`、名前の欄 `pelican.assetImportDialog.fileName`)で選んだ file を project に写して、その `project://` の参照を値にします([`GraphParameterEditor::importAsset()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1440))。UX-12a の着地時は、並べる拡張子を op と名前と説明の語から推していました(`pose.*` と `rig` は model、`audio.*` と sound は音、image / texture は `.ktx2` / `.png`)。✅UX-12e から、表の `extensions` と `asset_kind` で決めます([`assetParameterExtensions()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L243)。下の節)。
  - `shader`: engine の shader(`get_engine_shaders` の一覧)と project の shader の combo。`node_ref`: 同じ scope のほかの node の id の combo。
  - `object` は、`fields` があれば畳める見出しの下に field ごとの欄(`.<field>`)、`items` があれば key と値の行と + / ×(`.add`、`.<i>.key`、`.<i>.remove`)。`array` は行と + / ×。
  - 表で `expression: true` の項目は横に ƒ(`.f`)が付き、式の欄に切り替わります(Loop の Count の ƒ と同じ)。`json` と、表の kind を部品にしない種類は JSON の行で、JSON でなければ赤くして送りません(UX-12a の着地時は CT-4 の `union` もここでした。✅UX-12e から `union` は形を選ぶ combo です。下の節)。
- **既定と Reset**: 文書に無い parameter は表の `default` を出します。`default` のある parameter が既定と違う値を持つと **Reset**(`.<名前>.reset`)が出て、押すと鍵を消します(engine の既定に戻ります)。
- **書き方**: 変更は node の `parameters` 全体を 1 回の `set_node_parameters` で送ります(Undo 1 回)。spin と slider の動きは 150 ms まとめて 1 回です([送る所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1881))。保存の最中は UX-9 の `GraphCanvasHeldEdits` で覚えます。断られて何も変わらなければ、欄は文書の値に戻ります。同名の入力 edge がつながった parameter は、名前に ` ←` が付いて押せず、tooltip が `An edge feeds <名前>: its value is used.` です。
- **必須の印**: 表の最上位の `required` の名前には ` *` が付きます。値が書かれておらず、edge も無く、`alternatives` のどれも書かれて(つながって)いなければ、名前が赤く、下に `Required: the engine does not take the node without it.`(alternatives があれば `Required, or <名前> (or connect a line).`)が出ます([必須と拒否を赤くする所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1823))。
- **拒否の行**: `graph.render_node_parameter_invalid@1` は、notice では `<node>.<parameter> must be <expected> (<why>)` の 1 文です。`<why>` は、`reason` が `type` なら `wrong type`、`enum` なら `one of: <allowed>`、`range` なら `range: <min>..<max>`(片方だけなら `range: at least <min>` / `range: at most <max>`)です([理由の語を作る所](../../src/devstudio/model/inspectormodel.cpp#L1423))。例(シナリオ `gui_node_parameters`): `render_buffer.size must be integer (range: at least 0)`。拒否が名指す node を、その編集をした scope で選んでいれば、その parameter の行が赤く、下(`.<名前>.problem`)に同じ文が出ます。入れ子の道(`size_from_extent.tile_width`)は、最初の名前の行を赤くし、中のその field も赤くします。赤は次の commit で消えます([parameter の拒否を覚える所](../../src/devstudio/model/graphcanvasmodel.cpp#L4765))。この拒否と `graph.render_node_parameters_missing@1` は、canvas のその node にも拒否の印(LP-4c)を付けます。
- **値の帯と palette**: 帯のその場編集も表の kind を先に見ます(上の「値の帯は一覧を開きます」の UX-12a の項)。palette の必須のダイアログもこの表から作ります(上の「palette で置くときに、要る parameter を先に聞きます」)。
- **Pose の欄**(`pelican.graphCanvasPose`。[`GraphCanvasPoseFields`](../../src/devstudio/view/graphcanvasposefields.hpp#L28)): `pose.clip_sample` の欄です。UX-10 で作った Rig / Clip / Speed / Loop の欄を、同じ汎用の欄の上に載せ替えました。子の名前は `pelican.graphCanvasPose.<parameter>`(`rig` / `clip` / `speed` / `start_time` / `loop` / `root_motion` / `root_node`)です。`rig` は asset の欄(Import… 付き)です。`clip` は表では string ですが、Studio が rig の file(glTF の JSON。`.glb` は最初の JSON の chunk)の `animations` の名前を読んで combo にします(Studio の知識で選ぶのはこの 1 つだけ。[rig の animation の名前を読む所](../../src/devstudio/view/graphcanvasposefields.cpp#L65))。rig を選び直すと読み直します。`start_time` / `root_motion` / `root_node` は UX-12a で欄になりました。
- **New definition…**: 名前の行が、名前を打つ前に閉じることがありました(canvas の更新が、まだ名前の無い作りかけの行を閉じていた。コードの注記)。今は作っている間は閉じません([作りかけの名前の行を閉じない所](../../src/devstudio/view/graphcanvaswidget.cpp#L8566))。
- **engine の表で足りない所**: `parameters_complete: false` の 4 つの op(`render.image` / `render.fullscreen` / `render.raster` / `render.material_pass`)は、どれも専用の欄を持つ op です(UX-12a の着地時は `render.compute` を含む 5 つでした。engine の ✅CT-6 で `render.compute` は `true` になり、`ray_tracing` と `dispatch` も表の欄で出ます)。表に無い鍵は今も詳細表の JSON の行で書きます。専用の欄を持たない op で `parameters_complete` が false なら、欄の下に `The engine reads more of <op> than it lists; the rest is in the details' JSON rows.`(`.incomplete`)が出ますが、今の engine にその op はありません。CT-4(`4d682338`)が表に足した `choices_from`・`asset_kind` / `extensions`・`soft_min` / `soft_max` と op の `placement` は、UX-12a の Studio は読みませんでした(UX-12a の commit のコードにこれらの名前はありません)。✅UX-12e から読みます(下の節)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-12a decision 1: a render.buffer's parameters are fields made from the catalog's table -- an enum a combo, an integer a spin, a bool a check, an object folded, an array rows -- each change one set_node_parameters (a spin's steps gathered), Reset back to the engine's default」](../../test/devstudio_graphcanvas_scene_test.cpp#L11450)・[「UX-12a decision 3: a value pill's editor is chosen by the table's kind before the stored value's JSON type」](../../test/devstudio_graphcanvas_scene_test.cpp#L11585)・[「UX-12a decision 4: graph.render_node_parameter_invalid@1 reads as one sentence and the field it names is red with it」](../../test/devstudio_graphcanvas_scene_test.cpp#L11616)(decision 2 は上の必須のダイアログの項)。GPU シナリオは `gui_node_parameters` です(下の「Studio の自動化面」)。

**node の欄の残りを選ぶ形に —— 値の出どころ・asset の種類・2 通りの形・実用の範囲・置ける場所(✅UX-12e、`eeacce3e`)**: engine の ✅CT-4(`4d682338`。[第6章](06_rendering.md) §6.17 の CT-4 の段落)が表に足したものを、上の UX-12a の汎用の欄と palette で読みます。UX-12a の報告で、string としか言われない選択肢・asset の種類・union・uint32 の上限・root に置けない op が残っていました(仕様の背景)。

- **値の出どころ —— `choices_from`**: string の parameter の表に `choices_from` があれば、欄は編集できる combo です([出どころの一覧の欄を作る `GraphParameterEditor::buildChoices()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L664))。一覧は表が言う所から作ります([choices_from から一覧を作る所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1055))。
  - `{rpc: "get_render_authoring_context", key, field?}`: Studio が持つ authoring context の返事を `key` の道でたどり、`field`(無ければ要素そのもの)を名前として集めます。途中の配列は要素ごとにたどり、同じ名前は 1 回です([`graphParameterChoicesFromReply()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L273))。context は、選んだ node の欄にこの形の parameter があるとき、文書を読むたびに 1 回だけ問います(欄ごとには問いません。[`GraphCanvasModel::askRenderAuthoringForChoices()`](../../src/devstudio/model/graphcanvasvalues.cpp#L808))。UX-12e の着地時は、ほかの `rpc` の一覧は空でした(当時の engine の `rpc` はどれも `get_render_authoring_context`)。✅UX-13b から `rpc: "get_graph_catalog"` は Studio が持つ catalog から作ります(下の UX-13b の節)。
  - `{graph_nodes: [<op>...]}`: 今の scope の、その op の node の id です(選んでいる node 自身は除く)。
  - 一覧に無い名前も打てます。打ったとおりに残り、横に薄い `not listed`(`.<名前>.unlisted`。tooltip は `Not among the names the engine lists here now; it is kept as typed, and the engine says if it does not know it.`)が出ます。
  - 例(test): `render.target_ref` の `name` は context の `render_targets` の名前、`audio.volume` の `target` は scope の `audio.playback` の id です。
  - palette の必須のダイアログの `render.scalar` の `target` は、UX-8 からの Studio の buffer の combo(新しい名前も打てる)です([必須のダイアログの buffer の combo](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2470))。シナリオ `gui_node_parameters` が `target` を選ぶのはこの combo です(コードの読み)。UX-12e の着地時の候補は graph の `render.buffer` と plan の buffer の全部でした。✅UX-13b から、書ける buffer と、置く編集で書けるようにする buffer(` (will be set to runtime_f32)` が付く)です(下の UX-13b の節)。
- **asset の種類 —— `asset_kind` / `extensions`**: asset の欄の combo は、project の file のうち表の `extensions` のもの(無ければ全部)です。**Import…** のダイアログもその拡張子だけを filter にし、ほかの拡張子の file は取り込みません。写す先は `asset_kind` で決めます([asset の種類ごとに写す先を決める所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1440))。
  - `audio`: Sound の一覧の Import… と同じ道で `assets/audio/` に(✅UX-8)。
  - `image`: `.jpg` 以外は image の Initial contents の Import… と同じ道で `assets/images/` に(✅IA-1s)。`.jpg` は `assets/images/` にそのまま写します。
  - `model` は `assets/models/`、`analysis_table` は `assets/analysis/` で、隣に `<file>.import.json` があればそれも写します。どれでもなければ `assets/files/` です。
- **2 通りの形 —— `union`**: kind `union` の parameter は、形を選ぶ小さな combo(`.<名前>`)と、その形の欄(`.<名前>.<kind>`)です([union の欄を作る `GraphParameterEditor::buildUnion()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L790))。`bool` の形は `Off` / `On` の 2 項目、値が 1 つの enum はその値(`"full"` は `Full chain`)、integer は名前に level か count を含めば `Count`、ほかは `Index`、string は `Name`、object は `Custom`、✅CT-6s から shader は `Shader`、array は `List` です([形の呼び方 `graphUnionVariantLabel()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L327))。`required` でも `default` も無ければ先頭に `(none)` が付きます。今の値の型から形を選んで出し([値の型から形を選ぶ `GraphParameterEditor::loadUnion()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L847))、形を替えると、その形の欄に値が無ければ表から決まる始めの値を入れて送ります。例(test): pass の `ray_query` は Off / On / Custom、`value.sample_table` の `row` は Index / Name です。
  - `render.image` の `mip_levels` は image の専用の欄で、spin の前の **Count** / **Full chain** の combo(`pelican.graphCanvasImage.mipLevelsShape`)です([Count と Full chain の combo を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L459))。UX-8 の「spin の 0 が `full`」は外しました。Full chain を選ぶと spin が隠れ、`"full"` を送ります。
- **実用の範囲 —— `soft_min` / `soft_max`**: 数の欄の slider は、表に `soft_min` / `soft_max` があればその範囲です。spin は今までどおり `min` / `max` まで打てます。`soft_max` を越えた値は残り、横に薄い `above the usual range`(`.<名前>.unusual`。`soft_min` を下回れば `below the usual range`)が出ます([範囲の外の印を出す `GraphParameterEditor::showMarks()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L887))。tooltip には `Usual: <soft_min>..<soft_max>` の行が付きます。`render.image` の専用の欄では、Fixed の width か height が `soft_max`(8192)を越えると `above the usual range`(`pelican.graphCanvasImage.sizeUnusual`)が出ます([image の大きさの印を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L389))。
- **置ける場所 —— `placement`**: root で palette を開くと、`placement` が `owned_instance` の op(`pose.*` と `value.morph_weight`)の行は灰色で、末尾に `   needs a definition` と **New definition…**(`pelican.graphCanvasPaletteNeedsDefinition.<op>`)が付きます([palette の灰色の行を作る `markPaletteNeedsDefinition()` の所](../../src/devstudio/view/graphcanvaswidget.cpp#L7689))。root での tooltip は `needs a definition — New definition…: <op> runs inside a definition attached to an object, not in the root` です。灰色の行を選んでいる間は OK が押せず、`<op> runs inside a definition attached to an object: New definition… makes one and opens this list there.` が出ます。UX-12e の着地時は、definition の中で開いた palette では灰色にしませんでした。✅UX-13b から、その definition を owner の無い instance も使うとき(engine の `definition_instances` の `owned_instances_only` が false)は definition の中でも灰色で、文が変わります(下の UX-13b の節の「definition の中の置ける場所」)。
  - **New definition…** を押すか灰色の行で Enter を押すと、palette が閉じて ✅UX-8 の New definition… の名前の行が開きます。名前を打って作ると、canvas はその definition に入り、palette がその op の名前を絞り込みの欄に入れて(選んだ状態で)開き直します([definition に入った後で palette を開き直す所](../../src/devstudio/view/graphcanvaswidget.cpp#L6736))。definition を object に装着するのは別の操作です(下の「Inspector のグラフ装着」)。
  - `graph.op_requires_owned_instance@1` は `<op> runs inside a definition attached to an object — make a definition and place it there` の 1 文です([placement の拒否の 1 文の行](../../src/devstudio/model/inspectormodel.cpp#L1156))。✅UX-13b から、拒否に `definition` が付くとき(definition の中の node)は別の 1 文です(下の UX-13b の節)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-12e decision 1: a string the table says is chosen from somewhere is an editable combo of that list -- the authoring context read once per reading of the document, the scope's nodes of the named ops -- and a name typed off the list is kept, marked」](../../test/devstudio_graphcanvas_scene_test.cpp#L11707)・[「UX-12e decision 2: an asset parameter lists the project's files of the kinds its table entry names, with Import... beside it」](../../test/devstudio_graphcanvas_scene_test.cpp#L11769)・[「UX-12e decision 3: a union is a combo of its shapes picked from the value's type -- ray_query Off / On / Custom, mip_levels Count / Full chain, a sample row Index / Name」](../../test/devstudio_graphcanvas_scene_test.cpp#L11815)・[「UX-12e decision 4: a number's slider goes up to the table's soft_max, the spin past it, and a value above it is marked」](../../test/devstudio_graphcanvas_scene_test.cpp#L11870)・[「UX-12e decision 5: in the root the palette greys an op that needs an owned instance, and its New definition... makes one, goes in and opens the palette there on that op」](../../test/devstudio_graphcanvas_scene_test.cpp#L11898)。GPU シナリオは `gui_node_parameters` の後半です(下の「Studio の自動化面」)。

**音の解析を焼いて描画に使う —— Bake analysis…・Use in Graph・Use in a shader・Re-bake(✅AB-1s、`149c0ec8`)**: engine の ✅AB-1(`892a1455`。§13.5 の「音の解析を焼く —— `bake_audio_analysis` と `list_derived_assets`」、読み方は [第6章](06_rendering.md) §6.18「焼いた音の解析の表を読む」)を Graph タブの操作にしました。GB-4 の目標 3「音に反応する描画」は、engine に解析の node が無く止まっていました。DCC 寄りの機能は入れるが完成時は焼き込む、という方針で、v0 の音の解析は焼き込みです(仕様の背景)。

- **Bake analysis…**: `audio.playback` を選んだときの Sound の行の Import… の横に **Bake analysis…**(`pelican.graphCanvasAudioBake`)があります([Bake analysis… のボタンを作る所](../../src/devstudio/view/graphcanvaswidget.cpp#L5652))。engine の接続が `bake_audio_analysis` を送れるときだけ出て、Sound が空か project が無ければ押せません。仕様は Inspector の Sound の欄にも挙げていましたが、AB-1s の commit は Inspector の widget を変えていません(コードの読み)。
  - 押すとダイアログ `Bake analysis of <sound>`(`pelican.audioBakeDialog`)が開きます([Bake のダイアログ `GraphCanvasBakeDialog`](../../src/devstudio/view/graphcanvasaudioanalysis.hpp#L36))。Features の **RMS** / **Peak** / **Bands** の check(既定は 3 つとも入)、**Bands** の数(1〜128、既定 8。Bands を外すと押せない)、**Hop**(1〜1000 ms、既定 10)、**Smoothing** の attack / release(0〜60000 ms、既定 0)、**Channels**(Mix / Left / Right)、**Table**(既定 `assets/analysis/<sound の file 名から拡張子を除いたもの>.ktx2`)です。子の名前は `pelican.audioBakeDialog.<rms|peak|bands|bandCount|hop|attack|release|channels|out|bake|cancel>` です。feature が 1 つも無いか、Table が `.ktx2` で終わらなければ **Bake** は押せません。解析の窓(hop の 2 倍)は送らず engine に任せます(コードの注記)。
  - **Bake** で `bake_audio_analysis {asset: <sound>, out: <Table>, settings}` を送ります。`settings` は `{features: [選んだ "rms" / "peak" / "bands"], hop_ms, smoothing: {attack_ms, release_ms}, channels}` で、Bands を選んだときだけ `bands` が付きます([Bake の settings を作る `audioBakeSettings()`](../../src/devstudio/model/audioanalysistables.hpp#L60))。
  - 焼いている間、notice の下の行(`pelican.graphCanvasBake.line`)が `Baking <sound>… (<秒> s)` を数え、250 ms ごとに ticket を `get_edit_result` で問います([焼いている間の行を数える所](../../src/devstudio/view/graphcanvasaudioanalysis.cpp#L329))。Graph の notice には始まりの 1 回だけ出ます。焼いている間にもう一度送ろうとすると `<out> is being baked; wait for it to finish.` です。
  - committed で `Baked <out>: <features> × <sample_count> samples` です。band が 3 つ以上なら `band:0..band:<N-1>` とまとめます([committed の 1 行を作る `bakedLine()`](../../src/devstudio/model/audioanalysistables.hpp#L64))。例(シナリオ `gui_build_audio_reactive`): `Baked assets/analysis/beat.ktx2: rms, band:0..band:3 × 101 samples`。
  - 断られると 1 文で、末尾に ` (<code>).` が付きます([焼く拒否の 1 文 `audioAnalysisRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L1596)): `audio.analysis_settings_invalid@1` は `The analysis cannot take <field> = <value> (<reason>)`、`audio.analysis_busy@1` は `<out> is being baked already; wait for it to finish`、`audio.analysis_unavailable@1` は `This engine cannot bake a sound's analysis (<reason>)`、`audio.format_unsupported@1` は `<asset> is not a sound the analysis reads: its extension and its contents must both say WAV, or both Ogg Vorbis`、`audio.analysis_failed@1` は `<asset> could not be baked into <out>: <message>` です。
  - ✅UX-14(`aaba15a6`。engine の ✅AB-2)から、`audio.analysis_failed@1` の `reason` が `output_locked`(engine のやり直しの間ずっと、表か manifest が開かれていた)なら、文は message の代わりに `<asset> could not be baked into <out>: the table is in use — try Bake again` で([output_locked を見分ける `audioAnalysisOutputLocked()`](../../src/devstudio/model/inspectormodel.cpp#L1590))、行に **Bake again**(`pelican.graphCanvasBake.again`。tooltip `Send the same bake once more, once the table is no longer in use.`)が出ます。押すと、前に送った `bake_audio_analysis` の params をそのまま送り直します(焼いている間は送りません。[Bake again の所](../../src/devstudio/view/graphcanvasaudioanalysis.cpp#L216))。ボタンは次の bake を送ると隠れ、ほかの拒否では出ません。Re-bake も同じ送り方なので、Re-bake が `output_locked` で断られても出ます(コードの読み。test は Bake analysis… の道だけ)。例(test): `sounds/hit.wav could not be baked into assets/analysis/hit.ktx2: the table is in use — try Bake again (audio.analysis_failed@1).`
- **Use in Graph**: committed の行に **Use in Graph**(`pelican.graphCanvasBake.useInGraph`)と **Use in a shader**(`.useInShader`)が出ます。Use in Graph は、今の scope に `value.sample_table` を `{asset: "project://<表>", row: "rms"}`(表に rms が無ければ最初の行)で、palette と同じ道で置きます([Use in Graph の `useTableInGraph()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7463))。`time` の入力は繋がないので、engine は `scene_time` で読みます。
  - palette で置く `value.sample_table` の必須のダイアログは、✅UX-13b から他の op と同じ汎用の欄です([表と行の欄を作る所(UX-13b の注記)](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2458))。`asset`(`pelican.graphCanvasRequired.asset`)は表の `asset_kind: "analysis_table"` と `extensions: [".ktx2"]` で作る combo で、先頭が `(choose)`、その後に project の `.ktx2` の file が並びます(拡張子で絞るだけなので、焼いた表でない `.ktx2` も並びます。コードの読み)。`row` は union の形の combo(`pelican.graphCanvasRequired.row`)が `Name` で、名前の combo(`.row.string`)が選んだ表の隣の `.import.json` の `source.metadata.layout.features` です。表を選ぶと最初の行を選んだ状態になり(例(test): `rms`)、表を選び直すと行の一覧を読み直します。表を選ぶまで行は空で OK は押せません。
  - AB-1s の着地時(`149c0ec8`)は専用の combo で、`asset` は manifest の tool が `pelican.audio_analysis` の表だけ、`row` は rms があれば rms を選んだ状態でした。表が無いときの tooltip `No baked table yet: Bake analysis… on a sound (an audio.playback's Sound) makes one.` は UX-13b で無くなりました。置いた後の欄も同じ汎用の欄で、`row` の Name が同じ manifest の combo です(下の UX-13b の節の「表の行名」)。
  - `value.sample_table_unreadable@1` は `<node> cannot read the table <asset>: <reason>`、`value.sample_table_row_unknown@1` は `<asset> has no row <row>; its rows are <rows>` の 1 文です([value.sample_table の拒否の 1 文の行](../../src/devstudio/model/inspectormodel.cpp#L1218))。
- **Use in a shader**: 1 回の `edit_graph_document` で次を置きます([Use in a shader の op 列を作る `GraphCanvasModel::sampleTableShaderOps()`](../../src/devstudio/model/graphcanvasaudioanalysis.cpp#L43))。
  - 表の image: `render.image` `<表の名前>_table`(`R32_SFLOAT`、usage `SAMPLED`、`lifetime: "persistent"`、`initial: {asset: <表>}`。`initial.asset` は `project://` の無い project 相対の綴りで、大きさは書かず file の大きさになる)。
  - layout の buffer: `render.buffer` `<表の名前>_layout`(`element: "vec4"`、`size: 16`、`host_source: "runtime_f32"`)と、それを埋める 2 つの `render.scalar`。`<表の名前>_layout_values` は offset 0 に `[rows_per_feature, sample_count, hop_seconds]`、`<表の名前>_layout_time` は offset 12 に scene time です(scope に scene time の `value.input` が無ければ `scene_time` を足して繋ぐ)。
  - Graph で fullscreen か raster の pass を選んでいれば、その pass の shader の宣言を見て、次の番号の image の binding に表の image を、shader が `pelican_resource_<port>` の名前で読む buffer の port に layout の buffer を繋ぎます(pass の `ports` も同じ編集で足す)。✅UX-13a から port は binding の番号ではなく名前で選びます: 候補は反映に出ている `pelican_resource_<port>` の buffer の宣言と、pass の shader の file が `pelican_resource_ports.glsl` で読む名前(上の `shaderResourcePortNames()`。生成 include はまだ無い port を宣言しないので、反映には出ません)で、`<表の名前>_layout` があればそれ、無ければまだ port の無い最初の名前、それも無ければ最初の名前です。候補が無い方は繋がず、notice が `<pass>'s shader declares no image binding <n> or pelican_resource_<port> buffer; connect it by hand.` の形で言います。pass を選んでいなければ置くだけです。
  - 置いた後、行の下に shader に写す 1 行(`pelican.graphCanvasBake.shaderLine`。読むだけの欄)と **Copy**(`.copy`)が出ます。形は `#include "pelican_sample_table.glsl"  // and #include "pelican_resource_ports.glsl"; then: vec4 l = pelican_resource_<layout>.values[0]; float v = pelican_sample_table(<image>, <行>u, uint(l.x), uint(l.y), l.z, l.w);` で、行は band:0 があればその番号、無ければ 0 です([写す 1 行を作る `sampleTableShaderLine()`](../../src/devstudio/model/audioanalysistables.hpp#L93))。notice は `Read the table in the shader: #include "pelican_sample_table.glsl" and "pelican_resource_ports.glsl", and call pelican_sample_table (the line below).` です。UX-13a の前は 1 行に `pelican_resource_ports.glsl` の include が無く、notice も `pelican_sample_table.glsl` だけを言っていました。
  - 例(シナリオ): 表 `assets/analysis/beat.ktx2`(rms と 4 band、101 標本)で `beat_table` と `beat_layout` が置かれ、layout の値は `[1, 101, 0.01]`、写す 1 行は `pelican_sample_table(beat_table, 1u, …` を含みます。
- **古さの印と Re-bake**: Graph タブは `list_derived_assets` を、project を開いたとき、文書の digest が前に問うたときから変わったとき(読み直し・編集の後)、**Refresh** を押したときに問います([list_derived_assets を問う `readDerivedIfMoved()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7535))。音の file を書き換えても文書は変わらないので、印を出すには Refresh が要ります(コードの注記)。
  - `stale` の表を読む root の node(`value.sample_table` の `asset`、`render.image` の `initial.asset`)は、下の帯に橙の `stale: <reason>[, <reason>…]` が付きます([stale の帯を集める `syncStale()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7544))。
  - そうした node を選ぶと、右の欄に `pelican.graphCanvasStale` の行(`.line` に同じ文。tooltip `<表> was baked from <音>, which is not what it was baked from any more`)と **Re-bake**(`.rebake`)が出ます。
  - **Re-bake** は表の manifest を読み、その `source` の音・同じ `out`・manifest の `settings` で `bake_audio_analysis` を送ります([manifest から送り直す params を作る `rebakeParams()`](../../src/devstudio/model/audioanalysistables.hpp#L85))。manifest が読めなければ `The manifest of <表> cannot be read, so it cannot be baked again; bake it from its sound.` です。
  - committed の後、Studio は `list_derived_assets` を読み直し(帯が消える)、文書は編集しません(✅UX-13a)。書き換わった表は、Studio の読み直しの見張りが見ている file です(下の「Re-bake の後は reload が表を拾う(✅UX-13a)」)。
  - AB-1s の着地時(`149c0ec8`)は、committed の後に root でその表を読む `value.sample_table` の parameters を同じ値のまま `set_node_parameters` で書き直す 1 回の編集を送っていました。engine の AB-1 は表を graph を組む時に読むので、graph を組み直させて新しい表を読ませるためで、Undo に 1 段残りました。書き直すのは `value.sample_table` だけで、`render.image` の `initial` の表は対象外でした。UX-13a でこの編集と、それを作る関数 `GraphCanvasModel::rebuildSampleTableReaders()` は消えました(`0e0f4481`)。
- **シナリオ** `gui_build_audio_reactive`(下の「Studio の自動化面」)が、CPU の道(`value.sample_table`)と GPU の道(shader が表を `pelican_sample_table` で読む)の両方で、Timeline の時刻に合わせて viewport の画素が変わることを見ます。engine の AB-1 には GPU で描いて値を見る test がありません(§6.18)。
- test: `devstudio_graphcanvas_scene_test` の [「AB-1s decision 1: Bake analysis... asks for the features, bands, hop, smoothing, channels and table, sends bake_audio_analysis, says Baking while it runs and Baked when it is committed; a refusal is one sentence」](../../test/devstudio_graphcanvas_scene_test.cpp#L12195)・[「AB-1s decision 2 / UX-13b decision 5: value.sample_table's dialog is its table's own fields -- the project's analysis tables, and a row the manifest beside the one picked lists, its first taken -- and is placed with them」](../../test/devstudio_graphcanvas_scene_test.cpp#L12548)(UX-13b で名前を替えた。前は `AB-1s decision 2: value.sample_table's dialog offers the project's baked tables and the rows of the one picked, rms first, and is placed with them`)、`devstudio_graphcanvas_test` の [「AB-1s decision 3: Use in a shader places the table's image, its layout buffer and the two sinks that fill it in one edit, on the pass's sampled image and its pelican_resource_ buffer」](../../test/devstudio_graphcanvas_test.cpp#L4562)・[「AB-1s decision 4: list_derived_assets reads as the stale band; Re-bake asks for the manifest's own sound and settings (UX-13a: and edits nothing after it)」](../../test/devstudio_graphcanvas_test.cpp#L4626)(UX-13a で名前の後半を替えた。前は `; the readers are written again as they are`)、`devstudio_graphcanvas_scene_test` の [「UX-14 decision 5 (engine AB-2): a bake refused because the table is in use says to try Bake again, and Bake again sends the same bake once more」](../../test/devstudio_graphcanvas_scene_test.cpp#L12361)。

**buffer の binding は engine の規則で、雛形は include の形、Re-bake の後は reload が表を拾う(✅UX-13a、`0e0f4481`)**: engine の答え(graph から作った pass の buffer の読みは、読む資源の名前の順に bind される。[第6章](06_rendering.md) の「fullscreen pass の入力と shader の宣言の契約」の「`reads` は名前の順」)と ✅IA-3(`26473e75`)に、Studio の 3 か所を合わせました。

- **buffer の binding は engine の規則で**: fullscreen / raster の pass の名前付きの buffer の port は、engine が bind する順に数えます。読む元の buffer の資源名(線を辿った先の buffer の node の id。間の pass が渡すものは辿る)の辞書順で、`buffer_binding_base`(texture の入力の数、可変の組があれば `first + max`)から 1 つずつです。同じ buffer を読む 2 つの port は 1 つの binding です。線の無い port は engine が bind しないので数えず、名前の順で後ろに並びます([`GraphCanvasModel::bufferPortReads()`](../../src/devstudio/model/graphcanvasvalues.cpp#L300))。第6章の「名前の順」の「名前」は読む資源の名前で、port の名前ではありません。
  - UX-8 は port の名前の順と仮定していたので、port の名前の順と読む buffer の名前の順が逆のとき、ソケットと Buffers の欄の番号が engine とずれていました(UX-8 の commit の `namedBufferPorts()` は port 名で並べていた)。
  - ソケット: `pelican_resource_<port>` の名前の buffer の宣言は、どの binding にあってもその名前の port のものです(engine は port をその名前の変数に bind するため。RS-5)。ほかの名前の宣言は、上の規則でその binding に来る読みの port です([`bufferSocketPort()`](../../src/devstudio/model/graphcanvasmodel.cpp#L350))。
  - 上の UX-8 の節の Buffers の欄の番号と、AB-1s の節の Use in a shader の layout の port も、同じ規則と名前で決めます。
- **雛形は include の形**: New… の雛形(Pass-through / Deferred lighting / Tone map のどれも)は、buffer の入力を `layout(binding = N)` で書きません。最初の buffer の入力の注釈の行の頭に `#include "pelican_resource_ports.glsl"` を 1 回置き、`// input <i> (<resource>) is a buffer: map it to a buffer port of the pass and read it as pelican_resource_<port> (pelican_load_<port>(i)).` と言います([buffer の入力の行を書く所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L175))。UX-13a の前の注釈は `// input <i> (<resource>) is a buffer: declare it yourself at set PELICAN_SET_PASS_INPUT, binding <i>.` で、書けと言う binding は入力の位置の番号でした。
  - Open the shader が clipboard に入れる行も同じ関数で作ります。足す入力に buffer があれば、先頭の「`#version` の後に足す」注釈に `#include "pelican_resource_ports.glsl"` が加わります。
  - Use in a shader の写す 1 行と notice も、この include を言います(上の AB-1s の節)。
- **Re-bake の後は reload が表を拾う**: Re-bake の後に送っていた同じ値の `set_node_parameters` を外しました(上の AB-1s の節)。代わりに、IA-1s の見張りに渡す一覧([`GraphCanvasModel::initialAssets()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8130))が、`render.image` の `initial.asset` に加えて `value.sample_table` の表(`project://` を外した綴り)も集めます。Re-bake が表を書き換えると、ほかの file の書き換えと同じく `reload_render_documents` を頼み、engine の IA-3 が新しい世代で表を読み直し、notice が `Picked up the change to <表>.` を言います(上の「画像ファイルを image の初期内容に(✅IA-1s)」の見張り)。
  - 答えの `changed_assets` は `project://` を外して並べ、同じ file を 2 度言いません。image の `initial.asset` と `value.sample_table` の `asset` は同じ表を別の綴り(`assets/…` と `project://assets/…`)で書くためです([`changed_assets` の名前を揃える所](../../src/devstudio/model/renderdocumentreload.cpp#L384))。
- **シナリオ**: `gui_build_curve` と `gui_build_audio_reactive` の shader は include の形で書き、buffer の port は pass の Buffers の **+ Buffer port** で足します(下の「Studio の自動化面」)。`gui_build_audio_reactive` は、Re-bake の後に `Graph change saved.` が出ないこと、Graph の Undo が押せないこと、`Picked up the change to assets/analysis/beat.ktx2` が出ること、GPU の道の画素も変わることを見ます。
- test: `devstudio_graphcanvas_test` の [「UX-13a decision 1: a buffer binding is the read the engine binds there -- after the images, in the order of the buffers the ports read, not of the ports' names; a pelican_resource_<port> block is its port's wherever it sits」](../../test/devstudio_graphcanvas_test.cpp#L4697)・[「UX-13a decision 2: Use in a shader connects the layout by the name the shader reads through pelican_resource_ports.glsl, which no reflection shows before the port is there; the line to copy reads it through the include」](../../test/devstudio_graphcanvas_test.cpp#L4746)、`devstudio_graphcanvas_scene_test` の [「UX-13a decision 3: a Re-bake sends no edit of its own; the table it rewrites is watched beside the images' initial assets, for the reload to pick up (IA-3)」](../../test/devstudio_graphcanvas_scene_test.cpp#L12450)、`devstudio_shadertemplate_compile_test` の「F-3 the shader template declares each input with the engine's macros and compiles」の SECTION [「a buffer input is named for the author, not declared」](../../test/devstudio_shadertemplate_compile_test.cpp#L120)(include が 1 回、`binding 0` を書かない、compile が通る)。

**汎用の欄の残り —— 一覧の条件・catalog・表の行名、render.scalar の書き先を同じ編集で整える、拒否からの Fix、definition の中の置ける場所(✅UX-13b、`342142d5`)**: engine の ✅CT-5(`65f3a8a3`。[第6章](06_rendering.md) §6.5 の CT-5 の項と §6.17 の CT-5 の段落、§13.5 の `get_graph_instances` の `definition_instances`)が足したものを、上の UX-12a / UX-12e の汎用の欄と palette で読みます。

- **一覧の条件 —— `where`**: `choices_from` の `where` で候補を絞ります([`where` を確かめる `whereHolds()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L254))。RPC の答えの一覧なら各要素の member、`graph_nodes` なら各 node の parameters の値を見て、node が書いていない parameter は表の `default` として比べます。値の配列は「どれか 1 つ」です。例(test。部品を直接作る): `render.scalar` の `target` の部品は、`host_source: "runtime_f32"` で float の要素の buffer だけを並べ、`host_source` の無い buffer と `uint` の buffer は出しません。Graph の `render.scalar` の `target` は専用の欄(下の項)が受け持つので、Graph の欄でこの部品は出ません。
- **catalog から —— `rpc: "get_graph_catalog"`**: Studio が既に持つ catalog の `types` と `operations` から一覧を作り、engine には問い直しません([`GraphCanvasModel::catalogChoices()`](../../src/devstudio/model/graphcanvasvalues.cpp#L736))。例(test): `render.image` の `type.semantic` は constructor が image の型(`pelican.render.color_signal@1`・`pelican.render.depth@1` など)で、`pelican.value.scalar@1` は出ません。
- **表の行名 —— `asset_manifest`**: 同じ node の `parameter`(`value.sample_table` なら `asset`)が名指す file の隣の `<file>.import.json` を読み、`key` の道の名前を並べます([manifest を読む所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1062))。asset が変わると読み直します。今の値が新しい表に無ければ、値は残って `not listed` が付きます。必須の union で値がまだ無ければ、一覧が来た時点で、一覧を持つ形の最初の名前で始めます([必須の union を最初の名前で始める所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1220))。`value.sample_table` の必須のダイアログはこれで作ります(上の AB-1s の節)。Use in Graph は今までどおり rms(無ければ最初の行)で置きます。
- **Explicit…**: authoring context の `render_target_vocabulary.format_class_patterns` が書き方を持つ class(今は `explicit`)は、`format_class` の一覧で `Explicit…` と出ます。選ぶと横に引数の combo(`pelican.graphCanvasParameters.format_class.argument`。`argument_choices_from` の一覧で、今は format の一覧)が出て、`explicit(<format>)` を書きます([書き方を読む `GraphParameterEditor::patterns()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L708))。引数の始めは、文書の値の引数、無ければ image 自身の `format`、無ければ一覧の最初です。読み戻すと `Explicit…` と引数を出し、`not listed` は付きません。
- **render.scalar の書き先を同じ編集で整える**: 候補([`GraphCanvasModel::scalarTargetChoices()`](../../src/devstudio/model/graphcanvasvalues.cpp#L656))は、palette の必須のダイアログの `target`(`pelican.graphCanvasRequired.target`)と、置いた後の `render.scalar` の欄の Buffer(UX-8)で同じです([Buffer の combo を埋める所](../../src/devstudio/view/graphcanvasvaluefields.cpp#L643))。
  - まず graph の buffer(今の scope、次に root の `render.buffer` / `render.attribute`)のうち、その buffer 自身の parameters の編集で書けるようになるものです。要素が float / vec2 / vec3 / vec4 / mat4(要素を書いていない buffer も含む)で、`value` の線が来ておらず、`size_from_extent` や(pose のでない)`source` で大きさが決まらないもの([判定 `writableScalarBuffer()`](../../src/devstudio/model/graphcanvasvalues.cpp#L75))。
  - 変える所があれば、項目に ` (will be set to <変えるもの>)` が付きます。`host_source` が `runtime_f32` でなければ `runtime_f32`(大きさも同じ編集で揃える)、`host_source` は合っていて大きさだけ違えば `<n> bytes` です([項目の字 `scalarTargetLabel()`](../../src/devstudio/model/graphcanvasvalues.cpp#L687))。例(シナリオ `gui_node_parameters`): `render_buffer_2 (will be set to runtime_f32)`。
  - 次に、frame plan の buffer のうち graph の node でないもの(feature の `bloom.intensity` など。名前に `@` を含むものは除く)を印無しで並べます。これが合わなければ engine が断り、下の Fix の文になります。新しい名前も今までどおり打てます(graph の buffer でなければ整える編集は付きません)。
  - 選ぶと、置く `add_node` か、欄の `set_node_parameters` と同じ ops に、その buffer の `set_node_parameters`(`host_source: "runtime_f32"` と、その buffer に書く sink の合計の大きさ。`render.attribute` は `count`)が入ります([整える op `scalarTargetFixOps()`](../../src/devstudio/model/graphcanvasvalues.cpp#L692))。1 回の `edit_graph_document` なので Undo 1 回で戻ります。新しい sink は先にある sink の後ろの offset に置きます([置く所](../../src/devstudio/model/graphcanvasmodel.cpp#L3573))。値の数を替えると、その編集で buffer の大きさも変えます。別の buffer に移すと、移した先を整え、元の buffer をそこに残る sink の合計の大きさにします([移す所](../../src/devstudio/model/graphcanvasvalues.cpp#L611))。
  - 例(test): float 4 byte の `level` に 1 つ目の sink を置くと `runtime_f32` 4 byte、その値を vec3 にすると 12 byte、2 つ目の sink は offset 12 で `level` は 16 byte、2 つ目を `params` に移すと `level` は 12 byte に戻ります。
- **拒否からの Fix**: `graph.render_scalar_target_incompatible@1` は 1 文です([1 文を作る所](../../src/devstudio/model/inspectormodel.cpp#L1461))。graph の buffer(拒否に `node` がある)なら `The render.scalar <node> writes <target> (<actual>), which must be <expected> — Fix: make <target> runtime_f32, <size> bytes`、graph の外で宣言された buffer(`node` 無し)なら `A render.scalar writes <target> (<actual>), declared outside the graph, which must be <expected> — Fix: make <target> runtime_f32, <size> bytes where it is declared` です。`<actual>` / `<expected>` は `<host_source>, <size> bytes`(`host_source` が無ければ `no host_source`)で、末尾に CT-2 の文書と code が付きます。
  - graph の buffer のときだけ、notice の下に **Fix: make <target> runtime_f32, <size> bytes**(`pelican.graphCanvasFixScalarTarget`)が出ます([button を作る所](../../src/devstudio/view/graphcanvaswidget.cpp#L5270))。押すと engine が言った大きさで、その buffer の `set_node_parameters` を 1 回送ります([`GraphCanvasModel::fixScalarTarget()`](../../src/devstudio/model/graphcanvasvalues.cpp#L715))。graph の外の buffer は文だけで、button は出ません。button は次の commit で消え、別の拒否が来ればそれに置き換わります。
- **definition の中の置ける場所**: Studio は `get_graph_instances` の `definition_instances` を覚え、palette を definition の中で開いたとき、その definition の `owned_instances_only` が false(owner の無い `graph.instance` や `graph.loop` も使う)なら、`placement` が `owned_instance` の op を root と同じく灰色にして **New definition…** を付けます([`paletteNeedsDefinition()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7674))。tooltip は `needs a definition — New definition…: <op> cannot stand here: this definition is also used by an instance with no owner`、選んでいる間の文は `<op> cannot stand here: this definition is also used by an instance with no owner: New definition… makes one and opens this list there.` です。使う node が全部 owner 付きの definition、誰も使わない definition、`definition_instances` を言わない engine では灰色にしません。
- **拒否の 1 文**: `graph.op_requires_owned_instance@1` に `definition` が付くときは `<op> runs only in a definition attached to an object; this definition is also used by an instance with no owner` です([1 文を選ぶ所](../../src/devstudio/model/inspectormodel.cpp#L1488))。例(test): `value.morph_weight runs only in a definition attached to an object; this definition is also used by an instance with no owner (in definition gain_curve of passes/main.json, graph.op_requires_owned_instance@1).`。Inspector のグラフ装着の `edit_graph_instances` の拒否も、✅CT-5 から engine が名前付きの code と `document` を返すので、Graph と同じ 1 文で読みます(§13.5「Inspector のグラフ装着」)。
- **変えていないもの**: `render.fullscreen` の `push_constants`(CT-5 で enum)は、汎用の欄ではなく pass の欄の専用の combo(`No push constants` / `Camera position` / `Projection and view`。[combo を作る所](../../src/devstudio/view/fullscreenpasssettings.cpp#L61))が持ち、UX-13b はこれを変えていません。
- **シナリオ** `gui_node_parameters`(下の「Studio の自動化面」)は、`render_buffer_2` の `host_source` を手で選ぶ手順をやめ、`render.scalar` の必須のダイアログで `render_buffer_2 (will be set to runtime_f32)` を選び、同じ編集で `render_buffer_2` が `{element: float, size: 4, host_source: runtime_f32}` になることを見ます。
- test: `devstudio_graphcanvas_scene_test` の [「UX-13b decision 1: a list narrowed by where, one read from the catalog Studio holds, and one read from the import manifest beside the asset its sibling names, again when it changes」](../../test/devstudio_graphcanvas_scene_test.cpp#L11981)・[「UX-13b decision 4: in a definition an instance with no owner also runs, the palette greys an op that needs an owned instance and says why; in one only owned instances run, it does not」](../../test/devstudio_graphcanvas_scene_test.cpp#L12050)・[「UX-13b decision 6: a format class with an argument is Explicit... and a combo of formats beside it, written explicit(<format>); a refusal inside a definition an unowned instance runs is one sentence」](../../test/devstudio_graphcanvas_scene_test.cpp#L12115)・上の AB-1s の節の「AB-1s decision 2 / UX-13b decision 5 …」、`devstudio_graphcanvas_test` の [「UX-13b decision 2: a render.scalar's target offers the buffers it can write once the same edit makes them runtime_f32 and as large as their sinks, and each edit of the sinks keeps them so」](../../test/devstudio_graphcanvas_test.cpp#L4033)・[「UX-13b decision 3: graph.render_scalar_target_incompatible@1 reads as one sentence with its Fix; a graph's buffer is fixed in one edit, one declared outside the graph is only said」](../../test/devstudio_graphcanvas_test.cpp#L4087)。UX-12e decision 3 の test は、CT-5 で `row` の Name が combo になって壊れていたのを UX-13b で直しました(commit の記録)。Inspector のグラフ装着の拒否の読みを見る test は、この commit にはありません。

**位置と折り畳みはセッション内の状態です。**`(scope, ノード id)` を鍵に覚えていて、**文書には書きません**。
revision が変わって消えたノードの位置は捨てられます。初期配置は最長経路による段組みで、辺は必ず左から右へ進みます。

**ズーム・パン・fit は 2 つのキャンバスで同じです(N-3)。**ホイールはカーソルの下の点を動かさずに拡大・縮小し、
倍率は **0.25〜4.0** で止まります([ホイール](../../src/devstudio/view/nodegraphics.cpp#L575))。パンは**中ボタン**ドラッグ、
または **Space を押しながら左**ドラッグ([判定](../../src/devstudio/view/nodegraphics.cpp#L685))。**`F`** キーと
右クリックメニューの **Fit view** で内容全体が view に収まります(Graph タブの右クリックは「Add node... / Fit view」の
2 項目です。[メニュー](../../src/devstudio/view/graphcanvaswidget.cpp#L3440))。この 3 つの操作は共有部品
[`NodeCanvasView`](../../src/devstudio/view/nodegraphics.hpp#L204) が 1 か所で持っていて、**Graph タブも Frame Plan の
Logical graph も同じ view** です。**まだ見ていない scope は自動で fit** され、戻ってきた scope は離れたときの倍率と
スクロール位置に復帰します([`showScope()`](../../src/devstudio/view/nodegraphics.cpp#L658)) —— 倍率も位置も文書には
書かない、セッション内の状態です。値の帯やラベルを編集している最中の `F` はただの文字です(開いている editor が
キーボードを持ちます。[条件](../../src/devstudio/view/nodegraphics.cpp#L726))。

**テーマはライトとダークの 2 つです(✅N-5)。**切り替えは **View ▸ Theme ▸ Light / Dark**
([メニューの構築](../../src/devstudio/view/mainwindow.cpp#L1436))で、**再起動は要りません** —— 選んだ名前は
QSettings の [`studio/theme`](../../src/devstudio/view/studiotheme.cpp#L26) に残り、次に開いた窓は
そのテーマで始まります。追随するのは **Graph タブと Frame Plan の両方のキャンバス**で、背景・grid・辺・
ノードの body と縁・見出しの字・値の帯の色をテーマの表から引き直して描き直します。**ソケットの型別の色だけは
テーマに依存しません**(型の識別色なので、どちらのテーマでも同じ色です。テーマが決めるのは縁と、どの型とも
言えないソケットの色)。既定は `dark` で、`dark` のキャンバスの色は N-5 以前の色と同じ値です(完了記録
`6a26eef3`)。

**Auto layout(✅N-4)**: Graph と Frame Plan の右クリック menu「Auto layout」(`pelican.graphCanvas.autoLayout` / `pelican.framePlan.autoLayout`)で、その scope の手置き位置を捨てて層配置(最長経路の列、辺は lane を通って node を横切らない、列内の並びは重心法、行は最小間隔で重ならない)に戻します。動かした node は通常の refresh では保持されます。中ボタンのパンは描画が view に収まっていても動きます。✅GL-1b から、Graph の root では、その view の畳んだ node の場所(drag したものを含む)も捨てて、graph の node と同じ 1 回の配置に入れます。文書に書くのは graph の node の場所だけで、畳んだ node の場所はセッションの間だけ覚えます(上の「graph の外から来た pass を畳んだ node で見る」の置き場所)。

- ✅UX-1 から、Graph タブでは上段の **Auto layout** のボタンからも発火できます(上の「上段の操作列」)。
- ✅UX-1 から、画面に書くものと sink は、そこまでの道が短くても最後の列に立ちます。Graph タブでは sink の node、`display` か `swapchain` を書く畳んだ node(Blank の `present` など)、その pass を作った graph の node です([集める所](../../src/devstudio/view/graphcanvaswidget.cpp#L1987))。Frame Plan の Logical graph では、`display` / `swapchain` か `frame_target` の資源を書く node(グループなら member のどれか)です([集める所](../../src/devstudio/view/frameplangraphics.cpp#L3067))。
  - 共有の配置 `layoutNodeGraph` の `LayoutOptions::last_column` が受けます。名指した node と、そこから辿れる node を右へ動かします。行き先は「最後の列 − その node から出る最長の道の辺の数」で、元の列より左へは動かさないので、辺が逆を向くことはありません([動かす所](../../src/devstudio/view/nodelayout.cpp#L296))。
  - test: `devstudio_graphcanvas_scene_test` の [「UX-1 what writes to the screen and a sink stand in the last column, however short the path to them」](../../test/devstudio_graphcanvas_scene_test.cpp#L7881) と、ボタンの [「UX-1 Auto layout and Fit view stand in the Graph tab's row beside Undo and Redo」](../../test/devstudio_graphcanvas_scene_test.cpp#L7830)。

**応答性(Q-2)**: Studio は何も起きていない間は engine にほぼ問い合わせません(modal transform は gesture 中か engine 窓が focus を持つ間だけ、scene tree は revision が動かなければ 2 秒に 1 回、GPU 計時は panel が見えている間だけ)。Frame Plan の応答は 1 回だけ parse し、キャンバスは背景と grid を cache します。それでも Debug build は Release の 6〜19 倍遅いので、体感を見るなら Release 版で。project を開いた直後の固まりは Q-4 で消えました: Studio は engine の窓 thread が応答するまで埋め込みを待ち、その間は viewport に「Loading …(n s)」を出します。engine が log を書き続ける限り待ち、黙ったまま 120 秒(`PELICAN_STUDIO_ENGINE_LOAD_TIMEOUT_MS`)で停止します。Frame Plan の帯の editor も Graph と同じ規則(窓の瞬きで閉じない、再描画で残る、Escape で取消)です。

**engine の窓は Studio の外に出ません(✅EW-2)**: Studio は engine を `--embedded` 付きで起動します([引数の組み立て](../../src/devstudio/viewport/studioplayerarguments.cpp#L102))。engine はこの引数で窓を非表示・focus 無しで作る(EW-1)ので、project の読み込みの間(上の Loading の間)に Studio の外へ窓が出ません。project と一緒の起動・Restart Engine・project の切替のどれでも同じです。

- 窓の探し方: 同じ process で owner の無い top-level 窓のうち、engine が付ける property `PelicanEngineRenderWindow`(`src/core/os/enginewindowproperty.hpp` の `Pelican::engineRenderWindowProperty`)を持つものを選びます。見えているかは見ません([`findProcessWindow()`](../../src/devstudio/viewport/nativewindowhost.cpp#L58))。GLFW の helper 窓には property が付かないので選ばれません。
- 待ち方は Q-4 のままです(窓の thread が WM_NULL に応答してから埋め込み、その間は Studio が Loading を描きます)。
- 窓を見せるのは埋め込みの後です。`embed()` は SetParent の前に `WS_VISIBLE` を立てず、SetParent の後の `resize()` の `SWP_SHOWWINDOW` で初めて表示します([表示する所](../../src/devstudio/viewport/nativewindowhost.cpp#L203))。まだ top-level の窓に先に立てると、SetParent までの間だけ desktop に見えるためです(実装者の報告では、戻すと下のシナリオが約 50 ms 見える窓を捕まえて赤になりました)。
- `PELICAN_STUDIO_PLAYER_ARGUMENTS` に `--headless` があるときは `--embedded` を付けません(engine は 2 つ一緒を引数の誤りとして断るため)。付けなかったことを知らせる 1 行はありません。設定に `--embedded` があれば二重には付けません。
- EW-2 の前は、engine が見える窓を読み込みより先に作り、Studio は見えている窓を探していたので、読み込みの間は engine の窓が Studio の外に出ていました。

**Frame Plan のノード編集(N-8)**: Frame Plan でも出力ソケットから編集できる pass / task の入力ソケットへ**通常ドラッグ**で接続でき、入力ソケットを空所へドラッグするか辺を Delete で切断します(shader が入力を宣言している pass では、落とした ○ が binding を決めます。次の段落。✅VI-3a)。どれも既存の `edit_preset_pass` / `edit_compute_task` を宣言丸ごとで 1 回送るだけで、engine の pass や fork 前の preset、feature の pass は吸着せず理由を 1 行で出します。shader stem と task の dispatch は行の帯でその場編集(shader の帯は Pass Editor の Pick... と同じ一覧を部分一致で補完します。打った文字はそのままです。✅T-1。[帯の補完を付ける所](../../src/devstudio/view/frameplangraphics.cpp#L4545))、それ以外は node の double-click で今までの dialog(✅GL-5c から、Graph が編集する pass / task は Graph タブの詳細欄)。右クリック「Add fullscreen pass…」は Pass Editor をその node の後ろに prefill して開き(✅GL-5c から、graph root では Graph の `render.fullscreen` を置きます。§13.5 の「graph root では Frame Plan の Add を Graph の palette に(✅GL-5c)」)、Delete は authored pass(`remove_authored_pass`)と task(`remove_compute_task`)を消します。Shift ドラッグは今までどおり dialog です。node の位置・reroute・tie・group は Graph の `layout` の `frame_plan` に一緒に保存されます。N-8 のときは rendering 文書が legacy 形(root が `pelican.graph` でない)なら保存せず、その旨と file 名を 1 行で出していました。MG-2b から engine は legacy root を読み込みで `render_config.legacy_root@1` と断るので、そういう project を Studio で開いて canvas に届くことはなく、この 1 行も出ません(Studio 側の分岐は残っています)。

**ソケットは shader の宣言から出ます —— 番号でつなぐ(✅VI-3a)**: fullscreen / raster pass の node は、engine が
`get_frame_plan` の `pass_inputs` でその pass の shader の set 1 の宣言を出していれば(§13.5「レンダー設定を編集する」の
「各 pass の set 1 の宣言を `get_frame_plan` で読む」)、入力ソケットをその宣言から描きます
([ソケットを組む所](../../src/devstudio/view/frameplanedit.cpp#L663))。pass の入力は `input` 配列の位置で binding が決まる
(texture の入力は 0 から、buffer の入力は `buffer_binding_base` から)ので、行は名前順ではなく binding の順です。

- **固定の ○**: 固定の宣言 1 つに ○ が 1 つです。行の字は宣言の変数名で、つながっていれば `<変数名> ← <resource>`
  (history の入力なら `<resource> (history)`)。何もつながっていない ○ は輪郭だけで、字は灰色です。tooltip は
  `binding <b>: <行の字> : <型>` です。buffer の宣言も ○ で、buffer だけで数えます。
- **可変の帯**: `PELICAN_DECLARE_VARIADIC_INPUTS` の組は 1 本の長いソケットです。つながっている入力の分だけ ○
  (字は `← <resource>`)が並び、下端に空の落とし先 `+ input` があり、全体を半透明の帯が囲みます
  ([帯を描く所](../../src/devstudio/view/frameplangraphics.cpp#L1617))。空いている枠(max まで)は描きません。Blank pipeline の
  `present` が使う `engine://passthrough` は宣言がすべて可変なので、何もつないでいなければソケットは `+ input` の 1 つだけです。
- **宣言が取らない入力**(断られた編集の後など)は、末尾に以前の形の行で残ります。線の行き先が無くならないようにです。
- **線はつながっている ○ ごとに 1 本**です(同じ resource を 2 つの binding に渡せば 2 本)。

出力ソケットを drag して、**落とした先で**何が起きるかが決まります
([番号でつなぐ要求](../../src/devstudio/view/frameplanedit.cpp#L783))。送るのはどれも `input` 配列を丸ごと持つ `edit_preset_pass` 1 回です。

| 落とした先 | 起きること |
|---|---|
| 空の固定の ○ で、binding が次の番号(今の texture の入力の数。buffer の ○ なら buffer の入力の数) | 末尾に足す |
| 空の固定の ○ で、それより先の番号 | 送らず、status 行に `Connect binding <n>..<b-1> of <pass> first: a pass's inputs fill the bindings in order.`(飛ばしたのが 1 つなら `Connect binding <n> of ...`) |
| つながっている ○ | その位置の入力を置き換える(ほかの入力は動かない) |
| 帯の `+ input` | 固定の宣言が全部つながっていれば末尾に足す。足りなければ上と同じ 1 行。可変の枠が max まで埋まっていれば `<pass>'s shader takes at most <max> inputs in its variadic group.` |
| ノードの本体(見出しなど、ソケットの外) | **足しません**。送らずに `Drop on a socket of <pass>: its inputs follow its shader's declarations.` |

○ は小さい(11px)ので、各行の左 4 割に落としても、その行の ○ に落としたことになります
([落とし先の判定](../../src/devstudio/view/frameplangraphics.cpp#L4383))。image を buffer の ○ に(またはその逆に)落とすと、
`<resource> is an image: binding <b> of <pass> takes a buffer.` のように型を言って断ります。同じ resource が別の binding に
既につながっていても断りません。engine の拒否の扱いは N-8 と同じです。

**外す**: つながっている ○ を空所へ drag するか、線を選んで Delete すると、その `input` の 1 つだけを外します。外した位置より
後ろの入力が 1 つ前に詰まるときは、commit の後の status 行が `Canvas change committed.` の代わりに
`<pass>: the inputs after binding <b> moved up by one.` を言い、次の編集を送るまで残ります
([外す要求](../../src/devstudio/view/frameplanedit.cpp#L875))。空の ○ や `+ input` を押しても切る操作にはならず、node を
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
2 つのボタンが並びます([行とボタンを出す所](../../src/devstudio/view/frameplanwidget.cpp#L2822))。

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
[宣言から描くかの判定](../../src/devstudio/view/frameplanedit.cpp#L639))。**入力を 1 つも宣言しない shader**(宣言も可変の組も
無い)の pass も行は名前順のままですが、✅VI-3b から、落としても送りません(下の「宣言の無い shader の pass に落とすと」)。
Shift＋ドラッグで編集画面を開く経路は変わっていません。

**可変の組を固定する —— Fix sockets / Make sockets variadic(✅VI-3b)**: 可変の帯のソケットを、resource の名前の付いた固定の
宣言に書き換えられます。押す所は 3 つで、どれも同じ動作です。

- 可変の帯の端(`+ input`)の横の **Fix** の丸いボタン(`node_socket_fix#<pass>`。[Fix を描く所](../../src/devstudio/view/frameplangraphics.cpp#L1665))
- 可変の組を持つ node の右クリックの **Fix sockets**(`pelican.framePlan.fixSockets`。選んでいる node にも効きます)
- Pass Editor の Fragment 欄の横の **Fix sockets**(`pelican.fullscreenPass.fixSockets`)。開いている pass の shader に可変の組が
  あるときだけ出ます。書き換えるのは Frame Plan と同じで、未保存の下書きではなく、今描いている shader(`pass_inputs.fragment`)です

shader の `PELICAN_DECLARE_VARIADIC_INPUTS(<first>, <max>);` の行が、次の塊に置き換わります
([書き換える所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L796))。

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
([元に戻す所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L864))。目印の無い shader(手で書いた固定の宣言)は触らず、
`<pass>'s shader has no sockets Studio fixed.` と言います。

**engine://passthrough は project に複製してから直します(✅VI-3b)**: `engine://passthrough` で描く pass で Fix sockets か
Open the shader(Q-14 の出口。下の「shader が読まない入力」)を押すと、Studio は engine の `passthrough.frag` を project の
`shaders/<pass>.frag` に書きます(T-1 の名前の畳み方。file があれば `<pass>_2`、`_3`… で、既存の file は上書き
しません。[複製の名前を決める所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L783))。複製の中身は、Studio が build 時に
engine と**同じ file** から埋め込んだものです(repo に写しは置きません)。

- Frame Plan の Fix sockets は、固定化した source を複製として書き、fragment を `project://shaders/<名前>` に替える
  `edit_preset_pass` を 1 回送ります。`shader.defines` はそのまま残るので、出している入力は変わりません
  ([複製して送る所](../../src/devstudio/view/frameplanwidget.cpp#L2295))。
- Frame Plan の Open the shader は、engine の file を書き換えずに複製して同じ 1 回を送り、複製を desktop で開きます
  ([Frame Plan で複製して開く所](../../src/devstudio/view/frameplanwidget.cpp#L2356))。
- Pass Editor の Open the shader は、複製を書いて Fragment 欄に入れ、開くだけです。送るのは Save です。

複製できるのは `engine://passthrough` だけです。ほかの `engine://` は今までどおり開けません。⚠ 詳細ツリーの shader の Open は、
`engine://passthrough` でも押せないままです。⚠ 複製は送る前に書くので、送信が断られても file は残ります。

**passthrough が出す入力を ○ で選ぶ(✅VI-3b)**: `engine://passthrough` は、pass の `shader.defines` の
`PELICAN_PASSTHROUGH_INPUT`(既定 0)番の入力を出します。その pass の可変の帯で、つながっている ○ を**クリック**(押した所から
4 px 以内で離す。drag ではない)すると、その入力を出すよう `edit_preset_pass` を 1 回送ります
([出す入力を送る要求](../../src/devstudio/view/frameplanedit.cpp#L987))。

- defines の `PELICAN_PASSTHROUGH_INPUT` を外し、k が 0 でなければ `PELICAN_PASSTHROUGH_INPUT=<k>` を足します(k は可変の組の
  中の番号 = binding − `first`)。0 なら define を消し、defines が空になれば `defines` ごと消えます。ほかの define と入力は
  そのままです。commit の後の status 行は `<pass> shows <resource>.` です。
- 今出している ○ は明るく塗られ白い輪で囲まれ、行の字の末尾に ` (shown)` が付きます(tooltip は
  `engine://passthrough shows this input.`)。今の値は宣言の `shader.defines` から読みます。
- 既に出している ○ のクリックは `<pass> already shows <resource>.` と言って送りません。`engine://passthrough` 以外の pass では
  クリックしても何も起きません。
- Pass Editor で、`engine://passthrough` の pass の Fragment を別の shader に替えて Save すると、`PELICAN_PASSTHROUGH_INPUT` を
  defines から落とします(ほかの define は残ります。[Save で落とす所](../../src/devstudio/view/presetpassedit.cpp#L57)。✅GL-5a から、この組み立ては Pass Editor と Graph の詳細欄が共有する `presetPassEditParams()` の中にあるので、詳細欄で Fragment を替えたときも同じです)。
  Fragment を替えない Save では落としません。⚠ canvas の shader の帯で fragment を替えたときは落としません。

**宣言の無い shader の pass に落とすと(✅VI-3b)**: engine が `pass_inputs` を出していて、shader が入力を 1 つも宣言しない
(宣言も可変の組も無い)pass の node に出力を落とすと、Studio は送らずに、status 行に `<pass>'s shader declares no inputs.` と、
下の Q-14 と同じ 2 つのボタンを出します([送らない所](../../src/devstudio/view/frameplanedit.cpp#L791))。
**Use engine://passthrough** は今の入力と落とした入力、`fragment: engine://passthrough` を 1 回で送り、**Open the shader** は今の
fragment を開いて足す行を clipboard に入れます。VI-3b より前は送ってから engine の拒否を受けていました。

自動化面の名前(表の正は [README](../../test/studio_scenarios/README.md)。[宣言のソケットの名前を付ける所](../../src/devstudio/automation/automationcommands.cpp#L291)):

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
| `pelican.graphCanvasLoop.<欄>` | Loop の欄(✅LP-4c): `summary`・`openBody`・`count`・`countExpression`・`countExpressionToggle`・`order`・`limitEnabled`・`limit`・`tier`・`place`・`whereKind`・`whereValues`・`output.<port>`・`carry<i>.from` / `.to` / `.remove` / `.warning`・`addCarry`・`addBranch`。draw_loop では `contract`・`whereTagsInclude`・`whereTagsExclude`・`whereTagsRequireAll`(✅LP-4e)。✅LP-4f1 で `over`・`viewFamily`・`viewAxis`・`viewCount`・`viewAxisNote`・`countTarget`・`insertMipLevels`・`insertLayers`・`whereChecks`・`whereValue<i>`(i 番目の要素の check)。✅Q-17 で欄全体を包む `fields`。✅UX-8 で `insertLog2`(Count の式に `log2(<大きさ>)` を入れる)。✅PS-1s で `bodyOnce`(Run the body once)・`recompute` |
| `pelican.graphCanvasLoop.branch<i>.<列>` | 分岐表の i 行目(0 から): `when`・`values`・`body`・`up`・`down`・`remove`・`matched`。draw_loop では `material`・`tags`・`contract`・`skinned`・`hasVariant`(✅LP-4e)。✅LP-4f1 で `value`(`element =` の spin)・`checks`・`check<j>`(`element in` / `not in` の j 番目の要素の check) |
| `pelican.graphCanvasRender`(`.heading` / `.view.<axis>` / `.view.<axis>.warning` / `.view.<axis>.remove`) | Graph タブの render node の View 欄(✅LP-4f1。行の名前は LP-4b の View の欄と同じ形) |
| `pelican.graphCanvasLoopBranch` / `pelican.graphCanvasShared` | body の中のパンくずの右の分岐の combo / `shared (N users)` の印(✅LP-4c) |
| `pelican.graphCanvasPerIteration`(`.source` / `.element`) | Per iteration の dialog(✅LP-4c)。✅LP-4f1 から `.element` は combo で、Custom… の式の欄は `.element.custom` |
| `pelican.graphCanvasPaletteNewDefinition` / `pelican.graphCanvasPaletteContract` | palette の新しい body の名前(✅LP-4c)/ `render.material_pass` の契約(✅LP-4e) |
| `pelican.graphCanvasAddNode` / `pelican.graphCanvasPalette` / `pelican.graphCanvasPaletteFilter` / `pelican.graphCanvasPaletteList` | palette を開く **Add** のボタン / palette のダイアログ / 絞り込みの欄 / 一覧(✅GL-1d のシナリオが使います) |
| `pelican.graphCanvasDraw.<欄>` | material pass と draw step の欄(✅LP-4e): `contract`・`variant`・`cull`・`depthTest`・`depthWrite`・`blend`・`defines`・`applyDefines` |
| `pelican.graphCanvasMaterial.<欄>` | material pass の Pass Editor の form の欄(✅UX-11): 子は Pass Editor の `pelican.fullscreenPass.<欄>` と同じ名前(`materialFilter.include` / `.exclude` と、その menu の `.menu`・`.<tag>`・`.newTag`、`outputLoad.<i>`、`depthLoad` など)。`fields`(form 全体)・`drawn`(Materials drawn の表)・`drawnHeading` |
| `pelican.graphCanvasMaterial.drawn::row#<行の名前>` / `pelican.graphCanvasMaterial.materialResources.add` / `.materialResources.<i>.name` / `.materialResources.<i>.remove` | Materials drawn の行(✅UX-12c。行の名前は `pelican.primitive.default (material 1)` の形)/ Add binding と binding の行の名前・削除(シナリオ `gui_build_D` が使う) |
| `pelican.graphCanvasParameters`(`.heading` / `.more` / `.incomplete`)と `pelican.graphCanvasParameters.<名前>`(`.label` / `.problem` / `.reset` / `.slider` / `.import` / `.f` / `.add` / `.<field>` / `.<i>` / `.<i>.key` / `.<i>.remove`) | node の欄を catalog の表から作った欄(✅UX-12a)。`<名前>` は parameter の名前で、その子は kind の部品 |
| `pelican.graphCanvasPose.<名前>` / `pelican.graphCanvasPose.note` | `pose.clip_sample` の欄(✅UX-12a から子の名前は上と同じ形。`rig` / `clip` / `speed` / `start_time` / `loop` / `root_motion` / `root_node`) |
| `pelican.graphCanvasRequired.<名前>.choose` / `pelican.assetImportDialog`(`.fileName`) | 必須のダイアログの「どちらか 1 つ」の radio(✅UX-12a)/ asset の欄の Import… のダイアログと名前の欄(✅UX-12a) |
| `pelican.renderFeatures.offers` / `.addWithTargets` / `.placeAnchor` / `.fixTargetRef` | Render Features の notice の下のボタンの列と、Add with targets / Place … in the Graph / Fix: make …(✅UX-12b) |
| `pelican.renderFeatures.writer` / `.writeFrom` / `.showTarget` | 書き手の候補の combo(2 つ以上のときだけ見える)/ Write <target> from <node>.<port> / Connect <target> to <present>(✅UX-12d) |
| `pelican.graphCanvasParameters.<名前>.unlisted` / `.<名前>.unusual` / `.<名前>.<kind>` | 一覧に無い値の `not listed` / 範囲の外の `above the usual range` / union の形の欄(✅UX-12e。union の形の combo は `.<名前>` そのもの) |
| `pelican.graphCanvasImage.mipLevelsShape` / `pelican.graphCanvasImage.sizeUnusual` | image の Mip levels の Count / Full chain / 大きさの `above the usual range`(✅UX-12e) |
| `pelican.graphCanvasPaletteNeedsDefinition.<op>` | root の palette の灰色の行の New definition…(✅UX-12e。✅UX-13b から owner の無い instance も使う definition の中でも) |
| `pelican.graphCanvasParameters.<名前>.argument` | 引数を取る class(`format_class` の `Explicit…`)の引数の combo(✅UX-13b) |
| `pelican.graphCanvasFixScalarTarget` | `graph.render_scalar_target_incompatible@1` の Fix: make <target> runtime_f32, <size> bytes(graph の buffer のときだけ。✅UX-13b) |
| `pelican.graphCanvasAudioBake` / `pelican.audioBakeDialog`(`.rms` / `.peak` / `.bands` / `.bandCount` / `.hop` / `.attack` / `.release` / `.channels` / `.out` / `.bake` / `.cancel`) | Sound の行の Bake analysis… と、そのダイアログ(✅AB-1s) |
| `pelican.graphCanvasBake`(`.line` / `.useInGraph` / `.useInShader` / `.shaderLine` / `.copy`)/ `pelican.graphCanvasStale`(`.line` / `.rebake`) | notice の下の bake の行と、選んだ node の stale の行(✅AB-1s) |
| `pelican.renderingSettings.snapshots.add` / `.snapshots.row<i>.name` / `.snapshots.row<i>.after` / `.snapshots.row<i>.remove` | Rendering settings の Snapshots の + と行(✅UX-10。after の combo の項目は ✅UX-12b から `snapshot_after_candidates`) |
| `pelican.renderingSettings.render_strategy.name` / `.render_strategy.parameters` / `.draw_sort.xrViewPolicy` / `.graph_transforms.add` / `.graph_transforms.row<i>.provider` / `.name` / `.parameters` / `.up` / `.down` / `.remove` / `.graph_transforms.advanced` / `.xr.multiviewAuto` / `.xr.minimumGainPercent` / `.xr.apply` / `.preset` / `.<key>.preset.row<i>` | Rendering settings の render_strategy の name と parameters、draw_sort の XR の並べ方、graph transform の + と行、XR の Measured gate と minimum gain と JSON の Apply、preset を選ぶ root の 1 行と灰色の行(✅UX-15a) |
| `<prefix>.gpuDrawSource` / `.gpuDrawSource.fields` / `.layout` / `.commands` / `.count` / `.segments` / `.maxDrawCount` / `.commandOffset` / `.countOffset` / `.fallback` / `.execution` | material の form の GPU draws の check と欄(✅UX-15a。`<prefix>` は Pass Editor が `pelican.fullscreenPass`、Graph が `pelican.graphCanvasMaterial`、GL-5a の詳細欄が `pelican.graphCanvasFoldedPass`) |
| `pelican.framePlan.backend.pin` / `.backend.pinEntry` / `.backend.unpin` / `.backend.state` / `.backend.unpinButton` / `pelican.framePlan.physical.freeze` / `.physical.remove` / `.physical.ejectAgain` / `.physical.state` / `.physical.freezeButton` / `.physical.removeButton` / `.physical.ejectAgainButton` | Frame Plan の Backends の Pin this candidate(名前の action は選んだ行に効く / 右クリックの項目)・Unpin・帯、Physical plan の Freeze / Remove / Eject again・帯(✅UX-15b) |
| `pelican.graphCanvasFoldedPass`(`.heading` / `.vertex` / `.fragment` / `.inputs` / `.planning` / `.serial` / `.isolate` / `.noAlias.<target>` と部品の子) | Graph の詳細欄の rendering 節の pass の欄(✅GL-5a) |
| `pelican.graphCanvas.recomputeOnce` / `pelican.graphCanvas.recomputeOnceButton` | Recompute all once passes の action / ボタンの列の Recompute once(✅PS-1s) |
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
(軸ごとの名前・長さ・実行時に決まるか・ラベル)、`no_selected_view` を読みます([node で読む所](../../src/devstudio/model/frameplanmodel.cpp#L1363))。

- **canvas の 1 行**: `view_select` のある node(pass も task も)は、見出しの下、値の帯の上に `view: face 2 (+Y)` の 1 行を持ちます
  ([1 行を描く所](../../src/devstudio/view/frameplangraphics.cpp#L1536))。軸が複数なら `, ` でつなぎ、並びは軸の名前の辞書順です。
  値の後ろの括弧は、engine が `view_axes` の `labels` で名前を付けた軸(cube の face)にだけ付きます。cascade は `view: cascade 2` です。
  tooltip は `<pass> runs only for the views of <family> with face 2 (+Y).` です。
- **このフレームで走らなかった node は薄くなります**: engine が `no_selected_view: true` を付けた node は、直近に publish した
  フレームで `view_select` に一致する view が 1 つも無く、何も記録していません。その node は見出しの帯が薄くなり、見出しと node の
  tooltip に `No view of <family> matches view_select this frame.` が出ます([薄くする所](../../src/devstudio/view/frameplangraphics.cpp#L1431))。
- **詳細の行**: node の詳細に `View select`(例 `face 2 (+Y)`)の行が出ます。走らなかった node には `This frame`(上の文)の行も出ます
  ([詳細の行](../../src/devstudio/view/frameplanwidget.cpp#L4231))。Scopes の行は末尾に `; view: face 2` が付きます(ラベルは付きません)。

**view_select の拒否は 1 文で出ます(✅LP-4a)**: engine の `view_select` の拒否 4 種は、field の JSON のままではなく、1 文と末尾の
code で出ます([文を作る所](../../src/devstudio/model/inspectormodel.cpp#L1023))。

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

**view family の大きさの拒否(✅LP-4f2)**: engine の `render.view_family_extent_mismatch@1`(✅VF-1。1 つの view family の raster pass が違う大きさの target に書いた。上の「変種と組み直しの拒否の形」)も 1 文で出ます([`viewFamilyExtentRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L982))。上の view_select の文と同じ関数の先頭で読むので、出る所も同じです(Frame Plan の status 行、Pass Editor、Task Editor の notice、Graph タブの notice)。

```text
loop/0/face_pass (loop loop, body node face_pass) writes loop/0/face_pass__color at screen ×1.0, but $capture/cube draws at 256×256 (its first pass cube_capture_geometry writes cube_capture_albedo): the passes of one view family draw at one size (render.view_family_extent_mismatch@1).
```

- 大きさは、固定なら `256×256`、画面相対なら `screen ×1.0` です(倍率は小数 1 桁。要るときはそれ以上。`screen ×0.25`)。
- ループが複写した pass には `(loop <loop>, body node <node>)` が付きます(`loop_origin` / `expected_loop_origin`)。
- fields は、code の横、`error_fields`、別の拒否の message の中の `<code>: {…}` のどこからでも読みます。
- **赤い印**: 拒否が `loop_origin` を持つと、Graph タブはループと、その要素の分岐の body の中のその node を赤くします(上の「ループと definition の拒否」の赤い印と同じ規則。[`GraphCanvasModel::markRefusal()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4730))。⚠ 印が付くのは、fields が code の横か `error_fields` で届いたときだけです(message の中だけにあるときは、文は出ても印は付きません。LP-4f2 の着地時の記録)。
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
**View** の submenu が出ます([submenu を埋める所](../../src/devstudio/view/frameplangraphics.cpp#L4075))。

- 軸ごとに軸名の見出しがあり、その下に All views と値が排他の check 項目で並びます。今の値に印が付いています。
- 実行時に決まる軸は、押せない 1 行です。
- 選ぶと、その場で 1 回の編集を送ります([pass の要求](../../src/devstudio/view/frameplanedit.cpp#L2073))。
  - pass は `edit_preset_pass` です。fullscreen pass では、engine が必須にしている `input` / `output` / `shader` も宣言どおり一緒に送ります。
  - task は `edit_compute_task` で、宣言ごと送ります。
- 今と同じ値を選ぶと送らずに、`<name> already runs for face 2.` のように言います(All views なら `face All views`)。
- submenu の objectName は `pelican.framePlan.viewMenu` です。シナリオではまだ通していません。

**保存の規則(✅LP-4a / LP-4b)**:

- **Pass Editor の Save は、View の欄を変えたときだけ `view_select` を送ります**
  ([送るかを決める所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2468))。
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
([direct root の Pass Editor の拒否](../../src/devstudio/view/fullscreenpasswidget.cpp#L2561))。canvas も同じ文で断り、
engine が足した pass には「is added by the engine; it has no declaration in the project to edit.」と言います。どちらも
fork は勧めません([direct root の canvas の拒否](../../src/devstudio/view/frameplanedit.cpp#L131))。Pass Editor は pass を
選ぶと最初に form を空にするので、断られた pass の下に前の pass の下書きが残ることはありません(✅F-7)。

✅GL-5c から、Frame Plan で root が宣言した pass(`present` など)を選んで **Edit pass** を押すか、行をダブルクリックすると、Pass Editor ではなく Graph タブの畳んだ node が選ばれ、詳細欄(✅GL-5a)が開きます。Pass Editor 自体は残っていて、**View > Pass Editor** で前に出せます(前に出した Pass Editor でこの pass を選んで直せるかは、この WP の test では見ていません。未確認)(§13.5 の「graph root では Frame Plan の Add を Graph の palette に(✅GL-5c)」)。

**Frame Plan で render target を宣言する(✅F-1)**: toolbar の **New render target...**(`pelican.framePlanNewRenderTarget`)か、
canvas の空所の右クリックの同名 action(`pelican.framePlan.newRenderTarget`)で form(`pelican.renderTargetForm`)が開きます
([form](../../src/devstudio/view/rendertargetform.cpp#L36))。欄は名前・種類(Scene color / Display / Data / Explicit format と
format)・大きさ(Screen ratio = `extent_scale`、または Fixed size = `width` × `height`)・usage(engine が返した語彙の
checkbox)・history・clear color(`r, g, b, a`、任意)です。名前は ✅GL-5b から空いている `render_target`(使われていれば `render_target_2`…)が
入って開くので(それまでは空)、**そのまま OK** すれば、画面と同じ大きさの scene color
(`R16G16B16A16_SFLOAT`、`COLOR_ATTACHMENT` + `SAMPLED`)になります。Fixed size でも `extent_scale: 1` は書かれます
(parser が必須にしているため。[宣言の組み立て](../../src/devstudio/view/frameplanedit.cpp#L1403))。送るのは `edit_render_targets` 1 回です。
✅GL-5b から、同じ form が Graph タブの palette の `render.target_ref` と詳細欄の出力の行からも、同じ関数で開きます(§13.5 の「New render target を Frame Plan と Graph で 1 つの関数に(✅GL-5b)」)。
canvas は **Target**(`pelican.framePlanTarget`)で選んだ target を resource node として描き(自動化面の合成名は
`resource_node#<名前>`)、作った target は次の描画でそこに選ばれます(Target が **Whole frame** のときは選び直さず、
Whole frame のまま新しい target を描きます。下の段落)。その node の **double-click** で同じ form が
replace として開き、click で選んで **Delete** で remove します。replace は fragment の宣言を土台にするので、form に無い
欄(`mip_levels` など)は残ります。**他所で宣言された target は form が読み取り専用で開き**、出所を 1 行で出します
(「the screen output the engine provides」「a target of preset hybrid_v1」など)。Delete しても RPC は出ず、同じ出所の
文で断ります。engine の拒否は code ごとの 1 文になり、`render_target_referenced` なら使っている pass を並べます
([拒否の文](../../src/devstudio/view/frameplanedit.cpp#L1500))。

**Whole frame —— まだ誰も読まない target をつなぐ(✅F-7)**: Target combo の先頭は **Whole frame** です。まだ表示して
いない graph(project を開いた直後を含む)はこれで開き、refresh の後は選んでいたものが残ります。選択は Studio を
閉じると忘れます。Whole frame は**全 pass** と、**どの pass も書かない宣言済み target**(宣言しただけでまだ使っていない
`scene_color` など)を 1 枚に描きます([`populateWholeFrame()`](../../src/devstudio/view/frameplangraphics.cpp#L2475))。
後者は F-1 の resource node として、それを最初に読む pass の 1 列左(読む pass が無ければ描画全体の左)に置かれ、
右端に出力ソケット(自動化面の合成名 `node_socket#<名前>/out:<名前>`)を持ちます
([置き方](../../src/devstudio/view/frameplangraphics.cpp#L3085))。**このソケットから pass の入力ソケットへドラッグすると、
N-8 の接続**(`edit_preset_pass` 1 回。direct root 自身の pass もこれで root に書かれます)になります。pass が書いている
target は、書く pass の出力ソケットとして既に描かれているので、別の node にはなりません。Target で 1 つの target を
選ぶと、今までどおりそこへ届く上流だけ(Depth の深さまで)を描きます。status 行の target の欄は「whole frame」と出ます。

**Frame Plan で material pass を足す(✅G-1)**: toolbar の **Add material pass...**(`pelican.framePlanAddMaterialPass`)か、
canvas の右クリック(node の上でも空所でも)の同名 action(`pelican.framePlan.addMaterialPass`)で form
(`pelican.materialPassForm`)が開きます([material pass の form](../../src/devstudio/view/materialpassform.cpp#L24))。
pass は右クリックした / 選んだ node の後に入り、無ければ graph の先頭です(form の最初の行がどちらかを言います)。

> **✅GL-5c から、この form は preset を選ぶ root のものです。**Blank のように root が graph で preset を選ばない project
> (authoring context の `config_kind` が `direct`)では、Add material pass... は Graph タブの palette を `render.material_pass` で開き、
> pass は graph の `nodes` に draw ループと自前の image ごと入ります(managed fragment も target の宣言も作りません)。
> 下の Create missing targets・forward の既定の checkbox・Save の 2 つの要求は、preset root(と、Graph タブが root を graph と読まなかったとき)での動きです
> (§13.5 の「graph root では Frame Plan の Add を Graph の palette に(✅GL-5c)」)。

- **Contract**: Forward opaque / Forward transparent / Deferred geometry (G-buffer) / Legacy G-buffer(Forward opaque で
  開きます)。選んだ contract が出力の枠を決めます([contract ごとの枠の表](../../src/devstudio/model/frameplanmodel.cpp#L2238)。✅MC-1s から表は model の
  `frameplanmodel` にあり、Graph の palette の `render.material_pass` も同じ表で自前の image を作ります):
  forward は scene color 1 枠、G-buffer は albedo / normal / material / world position / emissive の 5 枠で、どちらにも
  depth 1 枠が付きます。枠の見出しには contract が要求する format が出ます。
- **各枠**は宣言済みの target を選ぶか、新しい名前を打つ combo です。横に `empty` / `declared` /
  `new: made as <format>` が出ます。
- **枠は開いた時点で埋まっています(✅T-1)。**form を開いたときと contract を変えたときに、全部の枠に名前が入ります
  ([開いた時点の枠を決める所](../../src/devstudio/view/materialpassedit.cpp#L104))。役割の既定名(下の `scene_color` など)の target が
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
[Save の 1 本目 edit_render_targets](../../src/devstudio/view/materialpassedit.cpp#L169))、その commit の後に届く新しい
context の digest で `add_authored_pass` 1 回([Save の 2 本目 add_authored_pass](../../src/devstudio/view/materialpassedit.cpp#L193))。
新しい target が無ければ後者だけです。**pass が断られても、先に作った target は宣言されたまま残ります** —— form はそれを
Save の前に言い、拒否の status 行も残った target の名前を並べます
([拒否で残る target の文](../../src/devstudio/view/frameplanwidget.cpp#L2606))。いらなければ node から Delete してください。
既存の target を枠に選んだとき、Studio は format を先に検査しません(engine の preflight が上の段落の検査をします)。
入力(`screen_inputs` など)の欄はまだありません。

**forward の既定を同じ Save で入れる(✅G-4)**: Contract が **Forward opaque** で、pipeline に `deferred_geometry_v1` /
`legacy_gbuffer_v1` の material pass が無いとき、form に checkbox **Draw this project's materials with forward**
(`pelican.materialPassForm.forwardDefault`、既定 on)が出ます([forward の既定の checkbox](../../src/devstudio/view/materialpassform.cpp#L64))。
automatic な material(glTF の material、基本形の灰色、engine の組込)の route は既定では `deferred_geometry` なので、
forward の pass だけの pipeline には受け皿が無く、engine は上の `material_route_unserved@1` で断ります。on のまま Save すると、
`add_authored_pass` が pass と `material_route_default: "forward"` を 1 回で運び
([payload に既定を載せる所](../../src/devstudio/view/materialpassedit.cpp#L222))、form の notice にも「pass と設定は一緒に
保存され、engine が再起動する」旨が足されます。

commit の結果に `restart_required` があると、Frame Plan の status 行に
`Restarting the engine to apply the material route setting...` を出し、**同じ project で engine を止めて起動し直します**
([`EmbeddedViewport::restartEngine()`](../../src/devstudio/viewport/embeddedviewport.cpp#L543))。gate はすぐ閉じ、新しい engine が
readiness に答えると Frame Plan が戻ります。

**保存していない scene の編集があれば、再起動の前に問います(✅G-5)**: project の切替と同じ Save / Discard / Cancel が
「Save your scene changes before the engine restarts?」の文で出ます
([`MainWindow::requestEngineRestart()`](../../src/devstudio/view/mainwindow.cpp#L2003))。未保存の編集が無ければ今までどおり
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
(組込には `(engine built-in)`)と直し方を言います([`materialRouteUnservedNotice()`](../../src/devstudio/model/frameplanmodel.cpp#L2306)。✅GL-1d で `materialpassedit.cpp` から移し、Graph タブの拒否の文と共有します)。
G-1 のシナリオの fixture が持っていた、誰も読まない pass 2 つは G-4 で外しました
([fixture の README の G-4 の段落](../../test/fixtures/studio_material_pass_authoring/README.md#L21))。

**Pass Editor の form は scroll します(✅F-7)**: form は `QScrollArea`(`pelican.fullscreenPassScroll`)の中にあります。
以前は form の最小の高さ(1,000 px 近く)が下の dock 領域を押し上げ、Pass Editor を開くと viewport が 60〜80 px の帯に
潰れ、dock を閉じても戻りませんでした([置き場](../../src/devstudio/view/mainwindow.cpp#L559))。**その潰れた状態を
既に保存した layout は、そのままでは戻りません** —— splitter を引くか、**Layout > Reset to Default** で直してください。

**fullscreen pass を shader ごと作る(✅F-3)**: Pass Editor の Fragment 欄の横に **New…**
(`pelican.fullscreenPass.newFragmentShader`。project を開いていないと押せません)があります。押すと名前の行
(`pelican.fullscreenPass.shaderTemplateName`、既定は pass 名)が出て、**Create** で `<project>/shaders/<名前>.frag` を
雛形([resource](../../src/devstudio/resources/fullscreen_pass.frag.in))から書き、欄に `project://shaders/<名前>` を入れ、
desktop の既定のエディタで開きます。雛形は**その時点の入力の数だけ** `PELICAN_DECLARE_INPUT_i` と
`PELICAN_TEXTURE_2D_i` の行を持ち、最初の入力をそのまま出します。入力が無ければ UV の gradient です。buffer の入力と
9 個目以降の入力は宣言せず、注釈 1 行で残します。buffer の入力の注釈は、✅UX-13a から binding の番号を言わず、`pelican_resource_ports.glsl` の `pelican_resource_<port>` で読むよう言い、最初の buffer の行の頭にその include を置きます(上の「buffer の binding は engine の規則で、雛形は include の形、…(✅UX-13a)」)。名前は英数字・`_`・`-` だけなので、`shaders` の外には書けません。
**既にファイルがあれば上書きしません** —— 「already exists」と **Open**(`pelican.fullscreenPass.openExistingShader`)が出て、
Open はそのファイルを使って開きます([書き込み](../../src/devstudio/view/fullscreenshadertemplate.cpp#L458))。
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
  target が 1 つだけのときそれを入れます([入力を埋める所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L618))。**最初に
  当てはまらない役割で止まります** —— 入力は位置で読まれるので、1 つ飛ばすと後ろの役割がずれるためです。ambient occlusion は
  埋めません。既に入力があれば触りません。埋まらなかった役割は役割名の順に自分で Add してください。足りない役割は shader の中で
  名前付きの定数になるので、compile は通ります。T-1 より前は入力が自動では埋まりませんでした。
  - **shadow mask(✅UX-6a)**: Deferred lighting の雛形は、入力の後ろに任意の入力 `shadow_mask` を 1 つ宣言します(R8 の mask。r が 1 なら照らす、0 なら影)。主光源(最初の directional light)の影は、shadow map の影と mask の暗い方です([主光源の影を mask と比べる所](../../src/devstudio/resources/fullscreen_lighting_pass.frag.in#L110))。
    - 入力の名前が `shadow_mask`(か `_shadow_mask` で終わる名前)なら、それを mask として宣言します。そうでなく入力が 8 個未満なら、入力の次の番号に mask を宣言だけします([mask の binding を入力の後ろに足す所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L226))。Graph の node には、その番号の空いたソケットが出ます(✅VI-3a の「ソケットは shader の宣言から出ます」)。G-buffer 5 枚と `shadow_map` を読む pass なら binding 6 です。
    - 何もつながない binding は alpha も含めて 0 を読むので、雛形は alpha が 0.5 を超えるときだけ r を mask として使い、それ以外は mask 無し(1)と読みます。
    - Pass Editor の New… も同じ関数で shader を書きます。
    - UX-6a の前は、GB-2 の `gui_build_full` で `rt_shadow_mask_pipeline` の mask を読ませるのに、`shaders/deferred_lighting.frag` を外のエディタで 2 か所(binding 6 の宣言と主光源の影の式)書き換えていました(着地時の description の記録)。
    - test: `devstudio_shadertemplate_compile_test` の「G-3 the deferred lighting template reads a G-buffer and the scene lights, and compiles」の SECTION [「UX-6a decision 6: the optional shadow mask is the binding after the inputs, and the key light takes the darker of it and the shadow map」](../../test/devstudio_shadertemplate_compile_test.cpp#L248)。
- **Tone map** —— scene-linear の色(入力 0)を Reinhard で表示用にします
  ([Tone map の雛形の Reinhard](../../src/devstudio/resources/fullscreen_tonemap_pass.frag.in#L31))。engine の `output_transform` は
  表示の encode だけで tone map しないので、tone map はここで 1 回です。project の `hdr` feature が有効なら `hdr_tonemap` が
  tone map するので、この雛形は素通しになります。

**打つものが減りました(✅G-3)**: Vertex 欄の既定は `engine://fullscreen` です。New… は、pass 名が空(か New… 自身が入れた
名前)なら雛形の名前(`deferred_lighting` / `tone_map`、Pass-through は `fullscreen_pass`。使われていれば `_2`…)を入れ、
shader 名を pass 名から付けます。自分で打った名前は上書きしません([雛形の名前の提案](../../src/devstudio/view/fullscreenpasswidget.cpp#L1539))。
保存した後も form はその名前を持ったままです。

**shader は一覧から選べます(✅T-1)。**Fragment と Vertex の欄の横に **Pick...** の一覧
(`pelican.fullscreenPass.shader.fragmentChoice` / `pelican.fullscreenPass.shader.vertexChoice`)があります
([一覧を欄の横に置く所](../../src/devstudio/view/fullscreenpasswidget.cpp#L1181))。並びは project の `shaders/` の `.frag`(Vertex は
`.vert`)を名前順に `project://shaders/<名前>` で、その後に engine の fullscreen shader(Fragment は `engine://passthrough`(✅VI-3a。
入力をいくつでも取る。[足した所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L520))/ `engine://fullscreen` /
`engine://scene_present` / `engine://output_transform` / `engine://ssao` / `engine://ssao_blur`、Vertex は `engine://fullscreen`)です
([一覧の中身](../../src/devstudio/view/fullscreenshadertemplate.cpp#L486))。選ぶとその stem が欄に入り、一覧は「Pick...」に戻ります。
欄には今までどおり何でも打て、欄の名前(`pelican.fullscreenPass.shader.fragment` など)も変わりません。一覧は New… で shader を
作った後と plan を読み直したときに読み直します。Frame Plan の shader の帯も同じ一覧で補完します(下の N-8 の段落)。

**出力の一覧から新しい target を作れます(✅T-1)。**color 出力の一覧の最後は `New target: <pass>_color...` です(pass 名の英数字
以外は `_`、pass 名が空なら `fullscreen_pass_color`、使われていれば `_2`…。[名前の提案](../../src/devstudio/view/fullscreenshadertemplate.cpp#L603))。
選ぶと Frame Plan の New render target の form(F-1)がその名前入りで開き、OK で target が commit されると、その target が
この pass の color 出力に足されます。plan の読み直しで変わったのがその target が増えたことだけなら、Pass Editor の下書きは
捨てません([下書きを残す判定](../../src/devstudio/view/fullscreenpasswidget.cpp#L3296))。

**既にある shader を読む pass の New…(✅G-5)**: pass が既に `shaders/<pass 名>.frag` を読んでいるとき(New… で作った
`shaders/present.frag` を読む `present` で、もう一度 New… を押したときなど)、名前の提案は `<pass 名>_<雛形>`(`present_tone_map` / `present_deferred_lighting`)で、
そのファイルもあれば `_2`、`_3`… を足します。Pass-through なら `<pass 名>_2` からです
([既存の file を避ける shader 名の提案](../../src/devstudio/view/fullscreenpasswidget.cpp#L1306))。以前は今読んでいるファイルの名前を
提案したので、Create が「already exists」になっていました。今のファイルは上書きされずに残ります。

**pass の設定欄(✅G-3)**: fullscreen pass の form に 2 つの欄が増えました(add / edit の両方)。

- **Frame data** の行(`pelican.fullscreenPass.interface`): **Uses scene lights**(`uses_light_data`)と push constants の
  combo(No push constants / Camera position / Projection and view = `push_constants` の `camera_position` /
  `projection_view`)です([Uses scene lights と Camera position の行](../../src/devstudio/view/fullscreenpasssettings.cpp#L56))。どちらも pass の
  contract と fingerprint に載る宣言で、GPU への供給は変わりません —— light の UBO(set 0 binding 2)はこの欄に関係なく
  すべての pass に bind され、camera の位置は push されず、雛形は `pelicanFrame.camera_position` を読みます
  ([第6章](06_rendering.md) §6.2 の fullscreen の項)。
- **color 出力ごとの行**(`pelican.fullscreenPass.outputOps`、color 出力の欄の下): Load(Default / Load / Clear /
  Don't care)・clear colour(`r, g, b[, a]`)・Store(Default / Store / Don't care)
  ([Load / Clear / Store の行](../../src/devstudio/view/passforms/attachmentopsform.cpp#L241))。列の見出しはありません(左から
  出力名・Load・clear colour・Store)。✅UX-4a(`ad1e0771`)から、この行は Pass Editor の部品 `AttachmentOpsForm` で、
  `output.color` の各 attachment object の `load_op` / `store_op` を直接書きます(UX-4a の前は `output.color` の JSON 欄が正で、
  この行はそれを映していました。その JSON 欄は UX-4a で無くなりました)。`subresource` などほかの field は残ります。**clear colour は attachment object には
  書けない**(attachment object が受けるのは `target` / `subresource` / `load_op` / `store_op` だけ)ので、pass の
  **`clear_colors`**(出力名が鍵)に入ります。swapchain に書く出力の `clear_colors` の鍵が、swapchain → display の正規化の
  下でどうなるかは未確認です。
  clear colour の欄の横には **Colour...**(`pelican.fullscreenPass.outputClearColorPick.<番号>`。✅T-1)があり、OS の色選択
  (modal の `QColorDialog`、alpha つき)で選んだ色を欄に `r, g, b, a` で書きます
  ([色を選んで欄に書く所](../../src/devstudio/view/passforms/attachmentopsform.cpp#L333))。色選択は自動化面から操作できないので、欄への
  直接入力も残っています。

**生の JSON の欄はフォームになりました(✅UX-4a)**: Pass Editor の raster の draw と raster state、shader の defines、material pass の設定、preset の routing は、UX-4a の前は JSON を打つ欄でした。今はどれも選ぶ欄と表です([フォームを並べる所](../../src/devstudio/view/fullscreenpasswidget.cpp#L531))。送る RPC(`add_authored_pass` / `edit_preset_pass`)と値の形は変わりません。

- **部品**: フォームは `src/devstudio/view/passforms/` の独立した widget です。どれも宣言の今の値(JSON)を受け、変えた後の値(JSON)を返し、`changed` で知らせます。フォームが知らない鍵は、受けたまま返します([`PassForm`](../../src/devstudio/view/passforms/passform.hpp#L28))。Pass Editor に依存しないので、Graph タブの詳細欄にも同じ部品を埋め込める形です(✅UX-11 から、`MaterialSettingsForm` と `AttachmentOpsForm` は Graph タブの material pass の欄にも載ります。上の「Graph の material pass の欄を Pass Editor の form で(✅UX-11)」)。子の objectName は `<prefix>.<field>` で、Pass Editor の prefix は `pelican.fullscreenPass` です。
- **Raster**([`RasterStateForm`](../../src/devstudio/view/passforms/rasterstateform.hpp#L26)): `raster_state` の topology・cull・front face(行の名前は **Winding**)・depth test / write / compare と、color attachment ごとの blend と write mask。raster pass では `draw` の operation と数も出ます。どの combo も先頭の **Default** は鍵を書かず、engine が鍵の無いときに読む値のままにします。fullscreen pass では depth の欄と draw を出さず、color attachment は 1 つです。
- **Defines**([`ShaderDefinesForm`](../../src/devstudio/view/passforms/shaderdefinesform.hpp#L19)): `shader.defines` を 1 行 1 define(`NAME` か `NAME=VALUE`)の表で、**Add** / Remove で足し引きします。engine が断る行 —— engine が pass を compile するときに自分で定義する名前、C の識別子でない名前、同じ名前が 2 回 —— は赤く、engine の文で理由を出します(Studio は engine を link しないので、`passshaderdefines.cpp` の規則を写しています)。断られる行があれば、Pass Editor は下書きを作らず、その文を出します([止める所](../../src/devstudio/view/fullscreenpasswidget.cpp#L1802))。
- **Material**([`MaterialSettingsForm`](../../src/devstudio/view/passforms/materialsettingsform.hpp#L42)): material pass の `material_settings` の 10 個の設定です。`material_contract` は engine の契約の combo、`material_filter` は any of / none of の tag(✅UX-11 から engine の `material_tags` の複数選択で、無い tag は `+ new tag` の行に打つ。それまでは `,` 区切りで打つ欄)、`material_variant` はほかの pass が使う variant を候補にした欄、`material_range` は on / off と start / count、`material_outputs` と `material_output_states` は出力ごとの行(名前・型・source・blend・write mask)、`screen_inputs` / `surface_resources` / `material_resources` は binding ごとの行(3 つのどれか・名前・読む資源)です。UX-4a の時点では `gpu_draw_source` だけが **Other (JSON)** の欄に残っていました。✅UX-15a から `gpu_draw_source` も欄になり、Other (JSON) の欄は無くなりました(下の「gpu_draw_source の欄(✅UX-15a)」)。
- **gpu_draw_source の欄(✅UX-15a、`06a9cd2d`)**: **GPU draws** の行の check **Draw from GPU-written commands**(`<prefix>.gpuDrawSource`。[check を作る所](../../src/devstudio/view/passforms/materialsettingsform.cpp#L289))を付けると、その下に欄が出ます(`<prefix>.gpuDrawSource.fields`)。値の意味は第6章の `gpu_draw_source` です。打つ欄はありません。
  - **Layout**(`.gpuDrawSource.layout`): `Default (fixed_state_v1)` / `fixed_state_v1` / `draw_queue_segments_v1`。
  - **Commands** / **Count** / **Segments**(`.gpuDrawSource.commands` / `.count` / `.segments`): buffer の combo です。候補は Pass Editor では宣言済みの buffer([渡す所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2586))、Graph の material pass の欄では root とその scope の `render.buffer` の node です([渡す所](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L370))。pass が名指す buffer は候補に無くても項目に残ります([`MaterialSettingsForm::fillBufferChoices()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L430))。Segments は Layout が `draw_queue_segments_v1` のときだけ押せます。
  - **Max draws**(`.gpuDrawSource.maxDrawCount`。1 から、既定 1)、**Offsets**(`.gpuDrawSource.commandOffset` / `.countOffset`。byte で 4 刻み。0 で元の値に鍵が無ければ書きません)、**Fallback**(`Default (none)` / `cpu_draw_queue`)、**Execution**(`Default (automatic)` / `automatic` / `cpu`)。
  - check を付けると、buffer を名前から選びます: 名前に `count` を含む最初の buffer を Count に、それ以外で `segment` を含まない最初の buffer を Commands に(Count に当たる名前が無ければ Commands 以外の最初の buffer を Count に)。Layout を `draw_queue_segments_v1` にしたときに Segments が空なら、`segment` を含む最初の buffer を選びます([check を付けたときの選び方](../../src/devstudio/view/passforms/materialsettingsform.cpp#L366))。部分一致だけで、buffer の `command_layout` や `host_source` では絞りません。
  - 書く値は欄の値で、欄に無い鍵は pass が持っていたまま残します([`MaterialSettingsForm::gpuDrawSource()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L479))。check を外すと鍵ごと書きません。Commands か Count が空、または 2 つが同じ buffer(segments の layout で Segments が空)のときは値を作れず、Pass Editor では Save が押せず、Graph の欄では何も送りません(UX-4a の Other (JSON) が JSON でなかったときと同じ扱い)。
  - test: `devstudio_pass_forms_test` の [「UX-15a decision 6: MaterialSettingsForm's gpu_draw_source is fields -- its buffers picked among the buffers given, its layout, counts and offsets -- not JSON」](../../test/devstudio_pass_forms_test.cpp#L344)(読んだ値がそのまま戻ること、選んで変えた値、check を付けたときの名前からの選び方、buffer が無ければ値を作れないこと。実装者の報告から)。Pass Editor 側は `devstudio_fullscreen_pass_test` の既存の [「Studio edits material and raster connections and restores the fullscreen form」](../../test/devstudio_fullscreen_pass_test.cpp#L2435) で、buffer の無い pass で check を付けると Save が押せなくなることを見ます(UX-15a の前は Other (JSON) に `[]` を打って見ていた所)。Graph の欄の候補の結線には専用の test がありません。
  - **残した穴**(実装者の報告): Graph の material pass で `render.buffer` の node が 1 つも無いときに check を付けても、何も送らずに待つだけで、理由は tooltip にしか出ません。
- **Routing**([`MaterialRoutingForm`](../../src/devstudio/view/passforms/materialroutingform.hpp#L19)): preset の `material_routing` の 3 行(`deferred_geometry` / `forward_opaque` / `forward_transparent`)で、どれも preset の material pass を選びます。off なら routing の表を持ちません。
- **Load / Clear / Store** の行は上の G-3 の行を部品にしたもの([`AttachmentOpsForm`](../../src/devstudio/view/passforms/attachmentopsform.hpp#L26))です。
- **Skinned vertex stem** の横に、ほかの stem と同じ選ぶ一覧が付きました。`Engine default` と project の `.vert` です([一覧を置く所](../../src/devstudio/view/fullscreenpasswidget.cpp#L497))。
- **Ray scene** は打つ欄ではなく combo です。`scene`、project が宣言する scene acceleration structure(`get_scene_acceleration_structures` で読みます。`RenderPassAuthoringCapability` に足した読み)、ほかの pass の `ray_query` が読む structure の順です([`showSceneStructures()`](../../src/devstudio/view/fullscreenpasswidget.cpp#L1239))。
- ✅RSX から、N-7 の文字の上限(Pass Editor は 400)に収まるように、行の名前を短くしました: `Add define` → **Add**、`Raster state` → **Raster**、`Scene ray query` → **Ray query**、`Front face` → **Winding**。もとの言い方は tooltip にあります。commit の記録では、測った値は 398 / 400 です。
- test: `devstudio_pass_forms_test` の 5 本(部品ごとに 1 本。[「UX-4a RasterStateForm gives back the raster_state and draw it was given, and a choice is the key it names」](../../test/devstudio_pass_forms_test.cpp#L70)、[「UX-4a AttachmentOpsForm keeps output.color, clear_colors and the depth output as given, and writes the ops chosen」](../../test/devstudio_pass_forms_test.cpp#L142)、[「UX-4a ShaderDefinesForm is shader.defines a row each, and refuses what the engine refuses in its words」](../../test/devstudio_pass_forms_test.cpp#L195)、[「UX-4a MaterialSettingsForm gives back the ten settings it was given, and the rows are the outputs and bindings」](../../test/devstudio_pass_forms_test.cpp#L260)、[「UX-4a MaterialRoutingForm is the three routes, each a material pass」](../../test/devstudio_pass_forms_test.cpp#L439))。UX-4a は GPU シナリオを足していません(commit の stat)。

保存は今までどおり `add_authored_pass` / `edit_preset_pass` で、edit は宣言から変わった field だけを送ります(変えずに
保存すれば送らず、外すと `null` で消します)。

保存が **shader の compile 失敗**で断られる(`render_pipeline_preflight_failed` の message に "Shader compile failed" が
ある)と、form の下の折りたためる欄(`pelican.fullscreenPass.compileLog`)に log 全文が等幅で出ます。行番号が付き、
log が名指す `file:line` は強調されます。notice 行は「shader compile failed; see the log below」だけで、**form に打った
値はそのまま残る**ので、shader を直してもう一度 Save できます。成功すると欄は消えます
([経路](../../src/devstudio/view/fullscreenpasswidget.cpp#L1627))。canvas の帯で直接 stem を変えて compile で断られた
ときは、今も status の 1 行だけです。

**shader が読まない入力を足すと、何をすればよいかを 1 行で言い、その場で直せます(✅Q-14)。**例えば `present` が何も読まない project の shader(入力を宣言せず gradient を書くだけの `shaders/gradient.frag` など)で描いているとき(Blank pipeline の `present` が使う `engine://passthrough` は入力がすべて可変なので、入力を足しても断られません。✅VI-2)、そこへ Pass Editor の Inputs で `gbuffer_albedo` を Add して Save すると、engine は pass の入力と shader の宣言が合わないとして断ります(上の「レンダー設定を編集する」の `shader_input_mismatch`。✅Q-14e)。正しい拒否ですが、以前は engine の文が出るだけで、次に何をすればよいかを言いませんでした。canvas で `present` の node に落としたときは、✅VI-3b から Studio が送る前に止め、`present's shader declares no inputs.` と下の同じ 2 つのボタンを出します(上の「宣言の無い shader の pass に落とすと」。VI-3b より前は送ってから、この拒否を受けていました)。

- **読める 1 行**: `<pass>'s shader (<fragment の stem>) reads only N inputs. To change its inputs, change the shader too.`(N が 0 なら `reads no inputs.`、1 なら `reads only 1 input.`、数が分からなければ `does not read the inputs the pass lists.`)([読める 1 行を作る所](../../src/devstudio/view/shaderinputcontract.cpp#L136))。Frame Plan では status 行(tooltip は engine の全文)、Pass Editor では compile log の欄の下(`pelican.fullscreenPass.shaderContract`)と notice 行に出ます。
- **当たる拒否**: `render_pipeline_preflight_failed` のうち、`error` に `shader_input_mismatch` があるか、message に `Fullscreen pass input` と `does not match shader reflection` の両方があるものだけです([入力の契約の拒否を読む所](../../src/devstudio/view/shaderinputcontract.cpp#L152))。ほかの拒否の出し方は変わりません。Frame Plan で出すのは、送った要求が `input` と `shader.fragment` を持つ `edit_preset_pass` のときだけです。
- **N の出所**: `shader_input_mismatch.shader_pass_inputs` の数 → 無ければ message の `(shader declares N pass inputs)` → 無ければ `get_frame_plan` がその pass に出した `pass_inputs` から数えた数(固定の宣言の数 + 可変の組の max。宣言がその fragment のもの —— 末尾の `.frag` の違いは見ない —— のときだけ。✅VI-3b。[宣言から数える所](../../src/devstudio/view/shaderinputcontract.cpp#L227))→ どれも無ければ数を言わず、`does not read the inputs the pass lists.` の形になります(`pass_inputs` の無い新しい pass など)。VI-3b より前の 3 番目は、fragment の file の宣言行を数えていました。pass と fragment も、engine が `shader_input_mismatch` で名指せばそちらを使います。
- **Use engine://passthrough**(✅VI-3b。`pelican.framePlan.shaderContract.usePassthrough` / `pelican.fullscreenPass.shaderContract.usePassthrough`): pass の fragment を `engine://passthrough` にします。入力がすべて可変(max 8)の engine の shader なので、今の入力(足そうとした入力を含む)をそのまま読めます。Frame Plan は入力の一覧と `shader.fragment` を **1 回の `edit_preset_pass`** で送ります。どちらか片方だけの変更は engine が断るためです([入力と shader を 1 回で送る要求](../../src/devstudio/view/frameplanedit.cpp#L425))。Pass Editor は Fragment 欄に入れて、拒否の後ならそのまま保存します。**file は書きません**。`PELICAN_PASSTHROUGH_INPUT` の define が無ければ出すのは入力 0 で、別の入力を出すには上の「passthrough が出す入力を ○ で選ぶ」です。VI-3b より前のこのボタンは **Make a pass-through shader**(`...makePassThrough`)で、F-3 の Pass-through の雛形から `shaders/<pass>_pass_through.frag` を書いていました。
- **Open the shader**(`pelican.framePlan.shaderContract.openShader` / `pelican.fullscreenPass.shaderContract.openShader`): fragment の file を desktop で開き、足す行を clipboard に入れます。固定の image の宣言の数(`pass_inputs` から。分からなければ 0)の番号から先の入力について、宣言 `PELICAN_DECLARE_INPUT_<i>(<名前>);` と main の中の `vec4 value<i> = PELICAN_TEXTURE_2D_<i>(<名前>, inUV);` で、名前は雛形が付ける名前と同じです。先頭には「shader に無ければ `#version` の後に足す」`#extension GL_GOOGLE_include_directive : require` と `pelican_sets.glsl` / `pelican_view.glsl` の include が注釈で付きます(足す入力に buffer があれば、✅UX-13a から `pelican_resource_ports.glsl` も)([clipboard に入れる行](../../src/devstudio/view/fullscreenshadertemplate.cpp#L389))。status は `The declarations are on the clipboard: add them to the shader and save, and it reloads by itself. Then connect the input again.`(Pass Editor は最後が `Then press Save again.`)で、保存すると下の F-5 の reload で反映されます。fragment が `engine://passthrough` なら、先に project へ複製してそれを開きます(✅VI-3b。上の「engine://passthrough は project に複製してから直します」。この経路は clipboard に何も入れません)。ほかの `engine://` など project の外の fragment では押せません。
- **Save の前の注意(Pass Editor)**: 入力の数が、engine がその pass に出した宣言(`get_frame_plan` の `pass_inputs`)の取れる数 —— 固定の宣言の数と可変の組の max の和 —— より多ければ、Save を押す前に入力の欄の下(`pelican.fullscreenPass.inputContract`)に同じ行と 2 つのボタンが出ます([Save の前の検査](../../src/devstudio/view/fullscreenpasswidget.cpp#L959)、[検査の 1 行を決める所](../../src/devstudio/view/shaderinputcontract.cpp#L261))。可変の組を持つ shader は、max までなら入力を足しても「読まない」ことになりません。ここでの Use engine://passthrough は Fragment 欄を埋めるだけで、送るのは Save です。**検査しないとき**: `pass_inputs` の無い pass(まだ保存していない新しい pass など)と、下書きの Fragment が今描いている shader と違うとき(宣言は今の shader のものなので)。どちらも engine の答えに任せます。✅VI-3b より前は、fragment の file の宣言行を正規表現で数えていました(`PELICAN_DECLARE_INPUT_<i>(` と手書きの `set = PELICAN_SET_PASS_INPUT, binding = <i>`)。今は file を読まないので、`#if 0` の中の宣言行を数えたり、可変の組を 0 と数えたりしません。canvas の drag は、shader が入力を宣言している pass では落とした ○ の binding を宣言と照らしてから送り(✅VI-3a)、宣言の無い shader の pass では送りません(✅VI-3b)。それ以外(`pass_inputs` の無い pass)は送ってから拒否を受けます。

⚠ Save の前の検査は型を見ません(buffer の入力も同じ数に入ります)。Open the shader の行は、宣言の番号が 0 から詰まっていると仮定します。

**shader を外のエディタで保存すると画面が変わる —— reload と失敗の印(F-3 / ✅F-5)**: Frame Plan は描いている node の
shader file を監視します。Studio は engine を `--rpc` で起こし、その reload gate は `rpc driver` を理由に閉じている
([gate](../../src/core/watch/reloadgate.cpp#L52))ので、engine は file の変更を自分では拾いません(project.json に `hot_reload` を
宣言した場所は ✅HW-1 から engine が自分で拾います。上の「Studio の下で外の保存を反映するには `hot_reload` を宣言します」)。そこで Studio は、
**書き込みが 200 ms 途切れたら `reload_shaders` を 1 回**送ります(同時に 1 本。返事を待つ間の書き込みは、返事の後に
1 回分だけ送り直します。[debounce](../../src/devstudio/view/shaderreloadfollower.hpp#L82))。定期の timer は無いので、
Q-2 の何もしない間の RPC は増えません。答えごとに:

- **読み直した**: node の印、Frame Plan の status 行の reload の文、Pass Editor の log 欄を消します。viewport は次の
  frame から新しい shader で描きます。✅VI-3a から Frame Plan は plan も読み直します(ソケットは shader の宣言から描くため)。
- **何も変わっていない**(`{committed: false, attempted: false}`): engine の監視が閉じている(`rpc driver`)ときは何もしません。
  ✅HW-2s(`78737fdf`)から、engine の監視が開いている(`get_status` の `reload.runtime["pelican.shaders"].details.enabled` が
  true。project.json の `hot_reload` が効いている。上の Assets のドック)ときは、**読み直したのと同じに扱います**
  ([扱いを替える所](../../src/devstudio/view/shaderreloadfollower.cpp#L139))。監視が開いていると engine の watcher が Studio の
  `reload_shaders` より先に compile することがあり、その後に届く `reload_shaders` は何も新しいものを見つけないためです。
  plan と Graph のソケットを読み直し、status を 1 回読んで印を engine の記録に合わせます(watcher の compile が失敗していれば
  そこで印が付きます)。監視が開いているかは、Frame Plan 自身の status の読みと、ほかの panel が読んだ `get_status`
  (MainWindow が [`FramePlanWidget::observeEngineStatus()`](../../src/devstudio/view/frameplanwidget.cpp#L5149) で渡す)の両方から
  覚えます。HW-2s の前は、監視を入れた project で shader を保存しても plan とソケットが読み直されず、watcher の compile 失敗の印も
  読まれませんでした(実装者の報告)。監視が開いていれば、保存しても中身が変わっていなかったときも読み直します(コードの読み)。
  test は `devstudio_assets_dock_test` の「HW-2s decision 7 …」(engine の代わりに test が答えます)で、実際の engine で watcher が
  先に compile する順はシナリオでも通していません(未確認)。
- **一部の pass が断った**(✅VI-3a。答えは `committed: true` でも `refused_passes` が空でない。engine 側は上の「shader を
  読み直させる」の「pass が新しい shader を断ることがあります」): 成功ではなく失敗として扱います
  ([`refused_passes` を読む所](../../src/devstudio/view/shaderreloadfollower.cpp#L198))。Frame Plan は plan を読み直して、上の
  「ソケットは shader の宣言から出ます」の赤い ○ と 1 行、Disconnect / Open the shader を出し、Pass Editor が開いていれば
  log 欄に最初の pass の `reason` が出ます。reload status の `error_code` が `shader_reload_refused@1` のときは、node に当たらない
  失敗の汎用の 1 行「Shader reload failed: ...」は出しません([出さない所](../../src/devstudio/view/frameplanwidget.cpp#L1725))。
- **compile 失敗**(`shader_reload_failed@1`): engine の log の path が node の shader に当たれば、その node の見出しの
  右上に赤い印(`node_shader_error#<pass>`、tooltip に log の先頭行)を描き、当たらない失敗は status 行に 1 行出します。
  **Pass Editor が開いていれば、form の下の log 欄**(保存の compile 失敗と同じ欄)にも log 全文が出ます。engine は前の
  shader で描き続けます。
- **engine が断った**(`shader_reload_unavailable@1`、replay / strict assets の engine): status 行に「A shader file
  changed; this engine does not reload shaders (<理由>).」。この文はこのときだけ出ます。
- それ以外の失敗: 「Could not ask the engine to reload shaders: ...」。

読み直しか失敗の後には `get_status` の `reload.runtime["pelican.shaders"]` を 1 回読み、印を engine の記録に合わせます
([答えの扱い](../../src/devstudio/view/frameplanwidget.cpp#L1738))。

**Graph タブの pass の shader も監視します(✅UX-6a)**: `reload_shaders` を送るのは今も Frame Plan だけです(Studio の中で 1 か所)。✅UX-6a から、Frame Plan はその監視に Graph タブの文書の pass の shader を足します。

- Graph タブは文書を描き直すたびに、全 scope の `render.fullscreen` / `render.raster` の `shader.vertex` / `shader.fragment` のうち `project://` で始まるものを project の中の path にし(拡張子が無ければ `.vert` / `.frag` を足します)、node ごと(root は `<id>`、definition の中は `<definition>/<id>`)に Frame Plan へ渡します([`publishShaderSources()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8509)、[送る役を Frame Plan に寄せる所](../../src/devstudio/view/mainwindow.cpp#L677))。
- Frame Plan は、plan を読んでいるか・前に出ているかによらず、それを監視する path に足します([Graph タブの shader を監視に足す所](../../src/devstudio/view/frameplanwidget.cpp#L1700))。
- 読み直しが通ったとき、または pass が新しい shader を断ったとき、Frame Plan は Graph タブに知らせ、Graph タブは Refresh と同じく文書と plan を読み直します(どのタブが前でも。[`GraphCanvasWidget::shadersReloaded()`](../../src/devstudio/view/graphcanvaswidget.cpp#L9160))。ソケットは shader の宣言から出るので、外のエディタで宣言を足すと、Refresh を押さずに Graph の node のソケットが増えます。
- UX-6a の前は、Graph タブだけで shader を書き換えても Frame Plan のタブを前に出さないと `reload_shaders` が出ず、Graph タブは Refresh を押すまで足した binding を出しませんでした(GB-2 の `gui_build_full` の着地時の description の記録)。
- test: 上の「Studio が作る image は float で、TRANSFER_SRC を付けません(✅UX-6a)」と同じ `devstudio_frameplan_graph_test` の 1 本(decisions 2 and 4)。GPU シナリオは `gui_build_full` の段 5(Graph タブを前にしたまま `shaders/tone_map.frag` を書き換える)です。

**外のエディタで書き換えた render の文書を拾います(✅RL-1s、`e58c8da9`)**: Studio の engine(`--rpc`)は file watcher の gate が閉じているので、Studio の外で root の JSON などを書き換えても engine は気づかず、次の GUI の編集が `external_modification` で断られていました(GB-2 の記録)。✅RL-1s から、Studio が engine の ✅RL-1 の `reload_render_documents` を呼んで取り込みます(engine の契約は §13.5 の「ディスクで書き換えた render 文書を取り込む」)。中身は Qt に依存しない [`RenderDocumentReload`](../../src/devstudio/model/renderdocumentreload.hpp#L52) で、つなぐのは MainWindow です([読み直しの役を MainWindow につなぐ所](../../src/devstudio/view/mainwindow.cpp#L1239))。

- **見張るもの**: engine が答え始めたら `get_render_documents` を読み、その文書(`engine://` と `#` 付きの reference は除く)の file を見張ります。見張り役は上の shader の監視と同じ Frame Plan の 1 つで、文書の file は shader とは別の口で知らせます([文書の file を足す所](../../src/devstudio/view/shaderreloadfollower.cpp#L85))。書き込みは 300 ms 途切れてから 1 回調べます。「消して書き直す」保存のエディタで見張りが外れないよう、変わるたびにその path を足し直します([足し直す所](../../src/devstudio/view/shaderreloadfollower.cpp#L98))。読めない file(置き換えの途中)は 3 回まで調べ直してから、それでも読み直しを頼みます。
- **自分の書き込みは頼みません**: 文書ごとに最後に見た digest(読んだ一覧、Studio の render の編集の commit の `documents`、読み直しの結果)を覚え、file の今の sha256 がそれと同じなら何もしません([`RenderDocumentReload::check()`](../../src/devstudio/model/renderdocumentreload.cpp#L192)、[commit の `documents` を覚える所](../../src/devstudio/model/renderdocumentreload.cpp#L334))。
- **頼む**: 違えば `reload_render_documents` を 1 本だけ送ります。返事が `reload_busy` なら 500 ms 後にもう一度で、理由が `preview_open` なら notice に `Close the scene preview to pick up the change to <file>.` を 1 回出します([`RenderDocumentReload::busy()`](../../src/devstudio/model/renderdocumentreload.cpp#L460))。ticket の結果は 100 ms ごとに問います。読んだ後にディスクがまた変わって `external_modification` が返ったら、もう一度調べ直します。
- **結果**([`RenderDocumentReload::committed()`](../../src/devstudio/model/renderdocumentreload.cpp#L358)):
  - committed: Graph タブの notice と status bar(8 秒)に `Picked up the change to <file>.`。Graph タブは自分の Undo の履歴を消して文書と plan を読み直し、render の編集の履歴(下の「render の編集を戻す」)は消して、Frame Plan の status 行に `— reloaded from disk —` を出し、ほかの panel は Undo の後と同じ道で読み直します([読み直しの後の処理](../../src/devstudio/view/mainwindow.cpp#L1394))。読み直しは undo の項目にならないので、Undo で読み直しの前へは戻れません。
  - `no_change`: 何も出しません。✅IA-1s から、変わったのが image の initial の asset で、engine が `changed_assets` を返さない(IA-2 の前の)ときだけ `<asset> changed — the engine reads it on its next start.` を出します(上の「画像ファイルを image の初期内容に(✅IA-1s)」)。
  - 断られた: notice に `Could not load <file>: fix the file and save again.`、tooltip と Copy message に `The engine keeps the previous version of <file>; fix the file and save again.` と engine の code・participant・reference・message です([`RenderDocumentReload::refuse()`](../../src/devstudio/model/renderdocumentreload.cpp#L421))。engine は前の版で描き続けます。直して保存すれば、もう一度見張りから始まります。file が engine の持つ版に戻っても notice は消えます。
  - `restart_required`: ほかの panel と同じ 1 か所の読み(上の「どのパネルの render の編集も、再起動が要れば同じ形で起動し直します(✅RSX)」)で起動し直します。
- **壊れたままの間**: 読み直しが断られたままの間に Graph などで編集すると engine は `external_modification` で断ります(engine は CAS を緩めません)。そのときの 1 文は `The file on disk changed and could not be loaded — fix it or use Reload from disk.` で、engine の文は 2 行目に続きます([1 文に替える所](../../src/devstudio/model/inspectormodel.cpp#L876))。Graph の canvas の右クリックに **Reload from disk**(`pelican.graphCanvas.reloadFromDisk`)があり、digest に関係なく `reload_render_documents` を送ります([メニューに足す所](../../src/devstudio/view/graphcanvaswidget.cpp#L3436))。直したのに見張りが拾わなかったときや、断られた理由をもう一度聞くときに使います。
  → ✅RR-1(`4549cae8`)から engine 側が変わりました。断られた後の engine はディスクの版を持ち、その digest の編集と restore が通り、前の版の digest の編集が `external_modification` です(§13.5 の「reload に断られた版を持つ」)。✅RR-1s(`471476d0`)で Studio の側も確かめて直しました。拒否の後に帯が `get_graph_instances` を読み、`live: false`(`held_unparsed` でない)なら
  その `source_digest` を window の共有の digest に流すので、Graph・Frame Plan・Pass Editor・Features・Rendering settings は読み直してディスクの版の digest で送ります(下の「reload に断られた版を持つ間(✅RR-1s)」の決定 4)。
  RR-1s の前は、reload が断られてもどの panel も読み直さず、前の live の digest で送っていました(実装者の報告)。上の 1 文(`The file on disk changed and could not be loaded …`)は、RR-1s から
  読み直しの拒否が `render_document_syntax@1`(JSON でない)のときだけ出ます([印を立てる所(RR-1s)](../../src/devstudio/model/renderdocumentreload.cpp#L453))。中身の壊れでは `external_modification` は engine の文のままです。
- 別の project を開くと、見張るものと覚えた digest を捨てます。engine が切れたときは、送信中のものだけを捨てます。
- test: `devstudio_render_reload_test`(Qt を使わない)の [「RL-1s decision 1: the project's render documents are watched, engine:// is not」](../../test/devstudio_render_reload_test.cpp#L154)、[「RL-1s decision 2: a file holding what Studio last committed asks nothing」](../../test/devstudio_render_reload_test.cpp#L183)、[「RL-1s decision 3: a change asks once, and reload_busy asks again later」](../../test/devstudio_render_reload_test.cpp#L210)、[「RL-1s decision 4: committed, no_change, refused and restart_required」](../../test/devstudio_render_reload_test.cpp#L257)、[「RL-1s decision 4: the render undo history is cut at a reload」](../../test/devstudio_render_reload_test.cpp#L337)、[「RL-1s decision 5 / RR-1s decision 4: while the file stays refused as not JSON, external_modification reads as the disk's sentence; a refused version that is JSON is the editor's base, and the engine's words stay」](../../test/devstudio_render_reload_test.cpp#L384)(✅RR-1s で名前と中身を書き換え。着地時は「RL-1s decision 5: while the file stays refused, external_modification reads as the disk's sentence」)。GPU シナリオは `render_reload_external`(下の「Studio の自動化面」)です。

**組めない構成でも engine を止めない —— placeholder の帯・Reload from disk・Show the document(✅BS-1s、`19be8983`)**: engine の ✅BS-1(`d9811bfc`)/ BS-1b(`40318ffe`)の `--render-fallback`(§13.5 の「起動時に断られた構成を直す」、[第6章](06_rendering.md) §6.16 の「起動時に組めない構成と `--render-fallback`」)を Studio で使います。BS-1s の前は、project の rendering の構成が起動時に組めないと Studio の engine は致命で終わり、viewport は止まったままでした(上の UX-9 の「起動時の致命の 1 文」)。

- **起動引数**(決定 1): Studio が起こす engine の引数は、いつも `--rpc --render-fallback --project …` で始まります([`--render-fallback` を足す所](../../src/devstudio/viewport/studioplayerarguments.cpp#L91))。`PELICAN_STUDIO_PLAYER_ARGUMENTS` の同じ指定は外して 1 回だけ付け、追加引数に `--headless` があるときも付けます。
- **帯**(決定 2): `get_status.render_pipeline.state` が `fallback` の間、viewport の上(workspace の layout の中)に 1 行の帯 `pelican.renderFallbackNotice` が出ます([`RenderFallbackNotice`](../../src/devstudio/view/renderfallbacknotice.hpp#L38)。中身は Qt に依存しない [`RenderFallbackSession`](../../src/devstudio/model/renderfallback.hpp#L45))。帯が読むのは、Play の帯と同じ 1 回 parse した `get_status` の返事です([帯へ get_status を渡す所](../../src/devstudio/view/mainwindow.cpp#L1741))。帯が出たときと文が変わったときは status bar にも同じ文を 15 秒出し、消えて state が `ready` なら status bar が `The project's rendering setup runs again.` を 8 秒言います。
  - 文(`.text`)は `Showing a placeholder: the rendering setup was refused — <code の 1 文> in <file>. Fix it, then Reload from disk.` です([`renderFallbackSentence()`](../../src/devstudio/model/inspectormodel.cpp#L1857))。code の 1 文は graph の拒否の表、無ければ `knownRefusalDescription()` の表から取り、どちらにも無ければ engine の message の 1 行目です(message が code で始まらなければ後ろに `(<code>)`)。file は refusal の `document`(CT-2。`project://` を外す)、無ければ `root`、それも無ければ `the project's render documents` で、`definition` があれば `definition <名前> of <file>` です。シナリオの例: `Showing a placeholder: the rendering setup was refused — Port data of sim needs element (graph.port_field_missing@1) in passes/main_rendering_config.json. Fix it, then Reload from disk.`
  - tooltip(と Copy message)は、文・engine の message・`rendering root: <root>` です。同じ読みが続いても知らせ直しません。
  - ボタン: **Reload from disk**(`.reload`)、**Open in Graph**(`.openGraph`。refusal が `node` を名指すときだけ。Frame Plan の dock を前に出して Graph タブにし、その node を見せる)、**Open file**(`.openFile`。file を名指すときだけ。shader と同じ開き方で、自動化の下では log に `automation: shader source open <path>` の 1 行)、**Show the document**(`.showDocument`。下の JSON でない文書のときだけ)、**Restart engine**(`.restart`。下の再起動の案内のときだけで、そのときほかのボタンは隠れます)。
  - viewport は engine の一色(暗い赤紫。実装者の報告では rgb 134, 56, 111 前後)です。
  - ✅PB-1s(`93934318`)から、帯の文は末尾に ` See Problems.` が付きます([付ける所](../../src/devstudio/view/renderfallbacknotice.cpp#L343))。tooltip と Frame Plan の status 行の文には付けません。
    status bar の件数のボタンを押すと Problems のドックが前に出ます(§13.1 の「Studio の Problems のドック」)。
    また engine の ✅PB-1 から起動の間の `material_route_unserved@1` も fallback になるので、その文は Frame Plan の既存の文(どの route を描く pass が無いか、何を足すか)に
    `(material_route_unserved@1)` を付けたものです([route_unserved の文を使う所](../../src/devstudio/model/inspectormodel.cpp#L1875))。
  - ✅RR-1s(`471476d0`)から、同じ帯は reload に断られた版を engine が持つ間にも出ます。fallback と重なれば fallback の文が先です(下の「reload に断られた版を持つ間(✅RR-1s)」)。
- **Frame Plan の status 行**(決定 2): `get_frame_plan` の答えが `render_pipeline`(fallback の間だけ付く)を持つと、status 行が赤で帯と同じ文を言い、tooltip は文と engine の message です。そのとき編集用の文書(authoring context など)は読みません([fallback の plan を言う所](../../src/devstudio/view/frameplanwidget.cpp#L1651))。project の plan が来ればその行を消します。Frame Plan の図は fallback の世代の plan(pass 1 つ)です。
- **読み直し**(決定 3): fallback の間は、render の編集の答えが committed のとき(Graph などの ticket の poll の答えも)、読み直しの commit、読み直しの拒否の後に `get_status` を読み直し([`RenderFallbackSession::wantsStatusAfter()`](../../src/devstudio/model/renderfallback.cpp#L32))、`ready` を読むと帯が消えます。rejected / accepted の答えでは読みません。engine が切れたら帯を消します。
  - Reload from disk は RL-1s の `RenderDocumentReload` の読み直しを送ります。直す前は断られ(Graph の notice に `Could not load …`)、帯は残ります。外のエディタで直して保存すると RL-1s の見張りが先に拾って読み直すので、ボタンを押さずに `ready` に戻ります(シナリオ)。
- **JSON でない文書**(決定 7・8。engine の BS-1b): `render_document_syntax@1` は `<document> is not valid JSON at line <L>, column <C>: <reason> (render_document_syntax@1).` の 1 文です(`at line …` は行と列が両方あるときだけ。fields は code の横か、message の code の後ろから読みます。[`renderDocumentSyntaxDescription()`](../../src/devstudio/model/inspectormodel.cpp#L1899))。帯ではこの文が file を言うので `in <file>` は付けません。同じ文を `knownRefusalDescription()` も返し、RL-1s の読み直しの拒否では tooltip の先頭に出ます(notice の行 `Could not load <file>: …` は変わりません)。
  - **Show the document**(決定 8): `get_render_documents` で文書を読み、窓 `pelican.renderDocumentDialog` の `.text` に生の文字列を出して、parser が止まった行・列にカーソルを置きます([`RenderDocumentRepair`](../../src/devstudio/model/renderfallback.hpp#L156)、[窓を作る所](../../src/devstudio/view/renderfallbacknotice.cpp#L208))。見出しは `<reference> as it is on disk. Fix it and Save: the file is replaced whole. This is for a document that is not JSON at all; the Graph and the Frame Plan edit it otherwise.` です。
  - 窓の notice(`.notice`)は、打つたびに全体を parse して `Not valid JSON yet at line L, column C: <reason>` か `Valid JSON. Save writes it to <reference>.` を言い、JSON でない間は **Save**(`.save`)が押せません([`RenderFallbackNotice::checkDialogText()`](../../src/devstudio/view/renderfallbacknotice.cpp#L259))。
  - Save は `restore_render_documents {documents: [{reference, base_digest: <読んだ壊れた bytes の digest>, content}]}` を 1 回送り、ticket を 100 ms ごとに問います([`RenderDocumentRepair::save()`](../../src/devstudio/model/renderfallback.cpp#L223))。文書の改行が CRLF なら CRLF に戻して送ります。committed で窓が閉じます。commit は窓が飲み込まずに MainWindow の commit の処理へ流れ、各 panel が読み直し、`get_status` も読み直します。断られたら 1 文を窓の notice に出し、窓は開いたままです。**Cancel**(`.cancel`)で閉じます。
- **再起動の案内**(決定 4): 直した構成の読み直しが `Module '<型>' was first requested after the runtime module graph was frozen` で断られたら(module の要る feature を直した構成で初めて有効にする場合。engine の BS-1 の残る穴)、帯が `Restart the engine to enable <型の短い名前>: the rendering setup needs it, and this engine was started without it.` と **Restart engine** を出し、押すと RSX と同じ `MainWindow::requestEngineRestart()` で起こし直します([`renderRestartFeature()`](../../src/devstudio/model/inspectormodel.cpp#L1920))。名前は message の型の名前(例 `UiSystem`)で、feature 名(`ui` など)ではありません(engine の拒否が feature 名を言わないため)。シナリオでは通していません(test だけ)。
  → engine の ✅PB-1(`a55d1eaf`)で、本番の feature の reload の拒否の文面がこの文と一致していなかったことが分かりました。今はどちらも `render_feature_requires_restart@1 {feature?, module}` です。
  code で判定する形への直しは PB-1s(作業中)。§13.5 の「今の問題の一覧を読む」。
  → ✅PB-1s(`93934318`、決定 6)で直しました。[`renderRestartFeature()`](../../src/devstudio/model/inspectormodel.cpp#L1920) は code が
  `render_feature_requires_restart@1`([`RenderFeatureRequiresRestartCode`](../../src/devstudio/model/inspectormodel.hpp#L442))のときだけ、code の横・`error_fields`・message の code の後ろの fields から `feature` を読みます。
  帯は `Restart the engine to enable <feature>: the rendering setup needs it, and this engine was started without it.` で、名前は feature 名(例 `debug_draw`)です。
  code の無い文面(container の `Module '<型>' was first requested after the runtime module graph was frozen` を含む)からは何も読みません(test)。
  同じ問題は Problems の行(Fix in が `Restart the engine`)にもなり、行を選ぶと一覧の下の Restart engine で起こし直せます。
- **残した穴**(着地時の記録。実装者の報告):
  - 再起動の案内は読み直しの拒否のときだけです。Graph などの構造の編集が同じ理由で断られても、その panel の拒否の文のままです。
  - fallback の間は Graph の `get_render_authoring_context` を engine が断るので、Graph の帯に拒否の文が出ます(engine の BS-1 の範囲)。
  - 既存の `render_reload_external`(二重の comma の段)は流していません。BS-1b で code が `render_document_syntax@1` になっても notice の行(`Could not load …`)は変わらず、tooltip に構文の 1 文が増える見込みです(未確認)。
  - Show the document の窓の構文の検査は打つたびに文書全体を parse します(大きな文書では重い)。窓は自動化の `type` で `QPlainTextEdit` に打つ前提で作りました。
- **engine に回したもの**(実装者の報告): module が frozen の拒否に code と feature 名を付けること(今は message の文字列を読んでいる。例 `render.feature_needs_restart@1 {feature}`)。`material_route_unserved@1`(material の登録で起きる起動時の拒否)は `--render-fallback` でも致命のままなので、fallback に含めるか。
  → どちらも engine の ✅PB-1(`a55d1eaf`)で入りました。frozen の拒否は `render_feature_requires_restart@1 {feature?, module}`、起動の間の `material_route_unserved@1` は `--render-fallback` なら fallback の世代に入ります
  ([第6章](06_rendering.md) §6.16 の「engine が持つ今の問題の一覧」)。
- **手で打つ欄**: Show the document の窓で壊れた JSON を手で直す欄(`.text`。シナリオでは末尾の `}`)。普段の編集の道ではなく壊れた文書の逃げ道で、末尾の括弧の欠けのような決まった形を「足して直す」ボタンにする案は入れていません。
- test: `devstudio_render_fallback_test`(`QT_OFFSCREEN`。実物の帯の widget に、test が hook で engine の代わりに答える)の
  [「BS-1s decision 2: the placeholder is one sentence above the viewport -- the code's, the file and the node -- with Open in Graph and Open file」](../../test/devstudio_render_fallback_test.cpp#L118)・
  [「BS-1s decision 3: Reload from disk asks the reload; a reload or an edit that committed on the placeholder reads get_status again, and ready takes the notice away」](../../test/devstudio_render_fallback_test.cpp#L175)・
  [「BS-1s decision 4 / PB-1s decision 6: a fixed setup whose feature needs a module the engine started without is told by its code, render_feature_requires_restart@1, and its feature -- on the row above the viewport, in the reload's refusal and on the Problems row -- and Restart engine restarts it」](../../test/devstudio_render_fallback_test.cpp#L218)(✅PB-1s で名前と中身を書き換え。着地時は「BS-1s decision 4: … says Restart the engine to enable it, and Restart engine restarts it」で、container の文面を読んでいました)・
  [「BS-1s decision 7: a render document that is not JSON is one sentence -- the file, the line and the column -- wherever the refusal is read」](../../test/devstudio_render_fallback_test.cpp#L333)・
  [「BS-1s decision 8: Show the document opens the bytes on disk at the line and the column, Save waits for JSON and restores it against the broken digest, a refusal is one sentence, and the commit closes the window」](../../test/devstudio_render_fallback_test.cpp#L370)
  の 5 本。`devstudio_viewport_test` の [「Studio player arguments force public editor feature and input overlays」](../../test/devstudio_viewport_test.cpp#L134) は、引数の完全一致の list に `--render-fallback` を足し、重複しないこと・`--headless` でも付くことを見ます。GPU の `devstudio_engine_failure_test` の書き換えは上の UX-9 の項です。実装者の run で `studio_cpu` 711 / 711、GPU の `devstudio_engine_failure_test` 2 / 2(BS-1b 入りの player)が緑です。GPU シナリオは `gui_render_fallback`(下の「Studio の自動化面」)です。

**reload に断られた版を持つ間 —— 保存した版の帯・Show the document は restore・断られた node は Graph・保存した版の digest で編集(✅RR-1s、`471476d0`)**: engine の ✅RR-1(`4549cae8`)から、外のエディタで保存した render の文書が reload で断られても、engine は前の版で描き続けながら、構造の読み(`get_graph_instances` など)はディスクの版を `live: false` で返し、その digest の編集と restore を受けます(§13.5 の「reload に断られた版を持つ」)。RR-1s で Studio がそれを読みます。RR-1s の前の Studio は `live` / `held_unparsed` を読まず、PB-1s の回避(断られた node は文字の位置へ、Show the document の Save はファイルを書く)を使っていました。

- **帯**(決定 1): 上の BS-1s の帯 `pelican.renderFallbackNotice` が自分で `get_graph_instances` を読み、答えの `live`・`held_unparsed`・`source_reference`・`source_digest` を覚えます([読む所](../../src/devstudio/view/renderfallbacknotice.cpp#L128)、[`RenderFallbackSession::observeAuthoringRead()`](../../src/devstudio/model/renderfallback.cpp#L48))。
  - `live: false` のとき: 帯の文は `Showing the saved version; the picture is still the previous one — <Problems の 1 文>` で、Problems の 1 文が無ければ `… previous one.` で終わります([`live: false` の文](../../src/devstudio/model/renderfallback.cpp#L144))。
    1 文は Problems の一覧の engine の行のうち、error で、source が `startup` でなく、shader でない最初の行の文です([`RenderFallbackSession::observeProblems()`](../../src/devstudio/model/renderfallback.cpp#L72))。
    tooltip の 2 行目は `The Graph and the other panels show the version on disk and edit it; the engine draws the version before until a fixed version is taken in.` です。
    ボタンは Reload from disk と、行が node を名指せば Open in Graph、文書を名指せば Open file です。シナリオの例: `Showing the saved version; the picture is still the previous one — Port data of sim needs element …`。
  - `held_unparsed: true`(ディスクの root が JSON でない)のとき: `<file> is not valid JSON; the Graph shows the last version that was.` です([`held_unparsed` の文](../../src/devstudio/model/renderfallback.cpp#L131))。
    file は読みの `source_reference`(無ければ Problems の行の文書)。Show the document が出て、Problems の行が `render_document_syntax@1` の行・列を持てばそこにカーソルを置きます。tooltip の 2 行目は Problems の 1 文です。
  - 帯は 1 つのことだけを言います。並びは 再起動の案内 → fallback の文 → `held_unparsed` → `live: false` です([帯の文を組む所](../../src/devstudio/model/renderfallback.cpp#L93))。PB-1s の ` See Problems.` はどちらの文の末尾にも付きます。
  - RR-1 の前の engine(答えに `live` が無い)からは何も言いません。fallback から `ready` に戻ったときは覚えた読みを捨て、読み直すまで何も言いません。
  - 読む時: engine が答え始めた時、reload の拒否と commit、Show the document の restore の commit、fallback から `ready` に移った時、Problems の engine の行の id が変わった時、断られた版を持つ間に `committed` の答えが来た時です。読みは同時に 1 本で、途中の依頼は後に 1 回続けます。
  - 帯が消えて state が `ready` なら、status bar は BS-1s と同じ `The project's rendering setup runs again.` を 8 秒言います(コードの読み。[消えたときに言う所](../../src/devstudio/view/mainwindow.cpp#L349))。
- **Show the document は restore**(決定 2): Save は帯から開いても Problems から開いても `restore_render_documents {documents: [{reference, base_digest: <get_render_documents の digest>, content}]}` です(上の BS-1s の決定 8 と同じ道。[`RenderDocumentRepair::save()`](../../src/devstudio/model/renderfallback.cpp#L223))。Studio が render の文書を直接書く道はもうありません(§13.1 の「Studio の Problems のドック」の「Show the document の Save はいつも restore です」)。
- **断られた node は Graph で**(決定 3): Problems の node の行は、source に関係なく Graph タブでその node を選びます。Graph はディスクの版を読み直しているので、その node があります。definition の中で Graph が見せられないときだけ Show the document の node の位置へ飛び、JSON でない root の行は今までどおり Show the document の行・列です(§13.1 の「Studio の Problems のドック」のクリック)。
- **保存した版の digest で編集**(決定 4): 読みが `live: false`(`held_unparsed` でない)を返すと、帯は hook `held_digest` でその digest を渡し([渡す所](../../src/devstudio/view/renderfallbacknotice.cpp#L170))、MainWindow が window の `RenderConfigDigest` に publish します([つなぐ所](../../src/devstudio/view/mainwindow.cpp#L340))。
  購読している Graph・Frame Plan・Pass Editor・Features・Rendering settings が自分の文脈を読み直し、次の構造の編集はディスクの版の digest を base に送ります。今持っている digest と同じなら何も起きません(Q-13 の `RenderConfigDigest` の既存の挙動)。
  `held_unparsed` の読みと読みの失敗では渡しません(panel は前の読みのまま)。RR-1s の前は、reload が断られてもどの panel も読み直さず、RR-1 の後の engine では次の編集が `external_modification` で断られるところでした(実装者の報告)。
  実装者は、graphattachment・proceduralasset・material や preset の pass の編集は送るたびに文脈を読み直すか Frame Plan の文脈の digest を使い、live の digest を独自に覚えている所は無かった、と報告しています。
  - RL-1s の決定 5 の 1 文 `The file on disk changed and could not be loaded — fix it or use Reload from disk.` は、読み直しの拒否が `render_document_syntax@1` のときだけ出ます([印を立てる所](../../src/devstudio/model/renderdocumentreload.cpp#L453))。中身の壊れでは panel がディスクの版で編集できるので、`external_modification` は engine の文のままです。
- **仕様から外れた判断**(実装者の報告):
  1. 読みは帯が自分で `get_graph_instances` を送ります。panel の読みを横取りせず、`get_render_documents`(全文書の中身を返すので重い)も使いません。`get_graph_instances` は `held_unparsed`・root の名前・digest が 1 回で取れるためです。
  2. 仕様の文の末尾に「.」を付けました(後ろに ` See Problems.` が続くため)。
  3. `live: false` の帯の 1 文は、Problems の engine の error のうち、source が `startup` でなく shader でない最初の行の文です。
  4. ボタンは `live: false` の帯で Reload from disk / Open in Graph / Open file、`held_unparsed` の帯で Reload from disk / Open file / Show the document です。
  5. シナリオの中身の壊れは Graph の Delete で直しました(決定 4 を本番の道で通すため)。そのため後の起動時の fallback の段の前に、`sim` を外から入れ直す 2 step を足しました。
  6. 「could not be loaded」の印は code(`render_document_syntax@1`)で決め、読みの `held_unparsed` とは結び付けていません(RL-1s の印は reload の拒否の時点で決まるため)。
- **残した穴**(実装者の報告):
  - 断られた版を持つ間も、Graph の notice(RL-1s)は `Could not load main_rendering_config.json: fix the file and save again.` のままです。中身の壊れでは Graph で直せるので、文が古くなっています(シナリオの `gui_problems_window_held.png` に写る)。RL-1s の範囲なので触っていません。
  - live の変化を engine が自分から知らせる口はありません。Studio は上の時に読み直し、Studio を通らない reload(別の client の RPC など)は Problems の revision が動いた時(1 秒の `get_status`)に拾います。
  - root でない文書(fragment)の構文の拒否が同じ code で来ても印は立ちます。そのときの engine の扱いは、engine の hold が root 前提なので未確認です。
  - 読みの publish と panel の commit が競ると、余分な読み直しが 1 回起き得ます(結果は収束する)。
- **engine に回したもの**(実装者の報告): 必須のものはありません。あると良いものは `get_status` の `live`(または hold の revision)で、Studio の読み直しのきっかけを 1 つにできます。
- **手で打つ欄**: 増えていません。Show the document の窓で壊れた JSON を直す欄(BS-1s の既存)は文法の壊れの逃げ道として残り、中身の壊れは Graph で直せるようになったので、打つ場面は減りました(実装者の報告)。
- test: `devstudio_render_fallback_test` の
  [「RR-1s decision 1: while a refused version is held the row says the picture is the previous one, with the Problems list's sentence; a version that is not JSON says the Graph shows the last one that was; the placeholder's sentence comes first」](../../test/devstudio_render_fallback_test.cpp#L525)・
  [「RR-1s decision 4: the digest of the held version is told to every panel, so a structural edit is based on it; a live read and a version that is not JSON tell nothing」](../../test/devstudio_render_fallback_test.cpp#L606) の 2 本と、
  書き換えた `devstudio_problems_test` の「PB-1s decision 3 / RR-1s decisions 2 and 3 …」(§13.1 の「Studio の Problems のドック」)と `devstudio_render_reload_test` の「RL-1s decision 5 / RR-1s decision 4 …」(上の RL-1s の項)です。
  実装者の run で、触った test 13 / 13、`studio_cpu` 724 / 724、GPU シナリオの `gui_problems_window`(81 / 81 step、報告の数え方、27.5 s)・`gui_render_fallback`(64 / 64)・`render_reload_external`(60 / 60)が緑です(`PELICAN_STUDIO_SCREEN=1`、1 本ずつ -j1)。

**配線の制御点と置き場所(N-6)**: 辺の double-click で **reroute**(通過点)が入り、drag で動き、Delete で消えます。node を動かしても reroute は動きません。制御点や辺を 2 つ以上選んで右クリック **Tie together** で 1 つの **tie**(結束バンド)になり、辺は等間隔で平行に通ります(**Untie** で戻す、double-click で並びを反転)。**Auto layout** は lane を reroute と tie として出します。node の位置・reroute・tie は文書の scope ごとの `layout`(editor 用、engine は無視)に `set_layout` で残り、次に開いても同じ図です。Frame Plan の位置はまだセッション内です。

**配置の保存は canvas を描き直しません(✅Q-5)**: node を drag した後、配置の変更がまだ送られていない間(300 ms の
debounce の間)は、notice が **「Saving the graph change...」** になります(error の notice だけはこれより優先)
([配置が未送信の間の notice](../../src/devstudio/view/graphcanvaswidget.cpp#L8665))。保存が返ると「Graph change saved.」です。
以前は前の保存の「Graph change saved.」が残ったままだったので、その文を待つシナリオは今の保存を待てませんでした。
また、**配置だけが変わった読み**(前の読みと `layout` 以外が同じ)では、model は node の値を消して読み直さず
([配置だけの読みの判定](../../src/devstudio/model/graphcanvasmodel.cpp#L1446))、canvas が既にその配置を描いていれば
(自分の drag の保存が返ってきたとき)scene を作り直しません —— 選んでいる node・辺・reroute はそのまま残ります
([`GraphCanvasScene::followDocumentPlacement()`](../../src/devstudio/view/graphcanvaswidget.cpp#L880))。Undo や別の書き手が
違う配置を書いた読みは描き直し、ほかの変更が混じった読みは描き直して選択を引き継ぎます。Frame Plan の配置の保存には、
もともと描き直しがありません(読み戻しも選んだ node を保ちます)。
⚠ engine が配置を丸めて返すと、送ったものと一致しないので「Saving…」のまま残ります。編集できる状態にならないままでも
同じです。

**配置の保存の後の編集も通ります —— 文書の版を panel どうしで知らせ合います(✅Q-13)。**配置の保存(`set_layout`)も rendering 文書の commit で、`source_digest` を進めます。以前の Frame Plan は自分の配置の保存の後に context を読み直さず、次の Add material pass... などを古い digest で送って `external_modification: render config base digest no longer matches the engine-owned document` で断られていました(project を開き直すまで直りませんでした)。Graph タブで node を動かした後の Frame Plan の編集も同じでした。

- **共有の digest**: Studio は「engine が今持っている rendering 文書の digest」を 1 つ持ちます([共有の digest の型](../../src/devstudio/view/renderconfigdigest.hpp#L25))。Frame Plan・Pass Editor・Graph タブ・Render Features は、読んだ digest と commit した digest をそこへ知らせ、知らせた panel 以外は自分の context を読み直します([共有の digest を 4 つの panel に渡す所](../../src/devstudio/view/mainwindow.cpp#L806))。読んでいる最中・編集の最中なら、終わってからもう 1 回読みます。Pass Editor は form の下書きを残し、Add fullscreen pass で選んだ挿入位置も変えません。隠れている Frame Plan は、表に出たときに読みます。Render Features から開く Task Editor(`ComputeTaskDialog`)も、開くときに Render Features から共有の digest を受け取り、読んだ root の digest を知らせ、他の panel の commit を知らされると変えた下書きを残して読み直します(✅Q-15。[Task Editor が知らされる所](../../src/devstudio/view/renderfeatureswidget.cpp#L969))。notice は `Another panel changed the document; reading it again. Your edits are kept.` で、残した下書きは読み直した root の digest で送り、task 自身の digest(`base_task_source_digest`)はそのままです。要求の最中に知らされたら、応答の後に読み直します。
- **要求は panel 自身の digest で送ります**: 共有の digest が持つのは「今の版」と通知だけです。要求の `base_source_digest` は、その中身を組んだ context の digest のままです。古い context から組んだ中身を新しい版の名で送ると、他の panel の変更を上書きしうるためです。
- **それでも `external_modification` で断られたら、1 回だけ読み直して組み直します**: Frame Plan は編集(辺の接続・切断、帯、Delete、render target の作成・編集・削除、material pass の 2 段)を「context から要求を組む関数」として持ち、断られると context を読み直して同じ関数で組み直して送ります。断られた byte は送り直しません([読み直して組み直す所](../../src/devstudio/view/frameplanwidget.cpp#L1896))。Pass Editor は form を残して読み直し、Save / Remove を組み直します。Graph タブは配置だけの編集(op が全部 `set_layout`)を同じ op 列で 1 回送り直し、ほかの gesture は今までどおり捨てて読み直します([配置を送り直す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1313))。Render Features は staged の編集を、Task Editor は変えた下書きを残して、1 回送り直します。
- **2 回目も断られたら**: Frame Plan の status 行と Pass Editor の notice に `The document changed outside this panel. Press Reload to read it again.` が出て、横に **Reload**(`pelican.framePlan.reloadAuthoring` / `pelican.fullscreenPass.reloadAuthoring`)が出ます。Reload は context を読み直すだけで、変更はもう一度行います。Render Features と Task Editor は新しいボタンを持たず、`... Press Refresh to read it again.` で既存の **Refresh** を指します。

⚠ 共有の digest に乗っていないものがあります。Inspector のグラフ装着(`GraphAttachmentModel`)、procedural asset の dialog、Render Features から開く小さな dialog 4 つ(frame node の順・preset の編集・callable の初期値・scene の acceleration structure)は、開いたときに読むだけです。Render Features の新しい挙動には専用の test がありません(Q-13 の記録)。Task Editor は、要求が失敗したとき(`inspectorRpcFailed` か、応答を decode できないとき)は最中に知らされた読み直しを拾わず、次の応答か Refresh まで残ります。読み直しは editor に `setPlainText` で入れるので、editor の undo の履歴は消えます(Q-15 の記録)。

**render の編集を戻す —— Undo / Redo(✅RU-1 / RU-1b)**: Frame Plan・Pass Editor・Render Features(と、そこから開く Task Editor などの dialog)でした render の編集は、1 つの履歴に積まれ、Undo / Redo で戻せます([`RenderEditHistory`](../../src/devstudio/model/renderedithistory.hpp#L47))。Graph タブの Undo(上の「Undo(✅LP-4c)」)と、Inspector の scene の Undo は、それぞれ別の履歴です。

- **押す所**:
  - Frame Plan の toolbar の、Refresh の右の **Undo** / **Redo**(`pelican.framePlan.undoButton` / `.redoButton`)。tooltip が次に戻す項目を言います(`Undo Edit pass present`、`Nothing to undo` など)([ボタンを置く所](../../src/devstudio/view/frameplanwidget.cpp#L5332))。
  - **Edit ▸ Undo Render Edit** / **Redo Render Edit**(`pelican.edit.undoRenderEdit` / `.redoRenderEdit`)。
  - **Ctrl+Z**(Undo)、**Ctrl+Y** と **Ctrl+Shift+Z**(Redo)。キーは focus のある panel に従います([キーの範囲](../../src/devstudio/view/mainwindow.cpp#L1172))。
    - Frame Plan・Pass Editor・Render Features と、Render Features から開く dialog(Task Editor・Fork・Regions・Records・Scene AS・frame node の順)の中では、この履歴です。
    - Graph タブでは Graph タブの Undo、Inspector では scene の Undo / Redo のボタンと同じです(Inspector のキーは RU-1 で足しました)。
    - focus がテキスト欄にあるときは、その欄の文字の Undo が先に取ります。
- **戻るもの**: render の文書を書く編集です([記録する編集の method の表](../../src/devstudio/model/renderedithistory.cpp#L25))。✅UX-12d から `edit_batch` も記録し、batch ごと 1 回で戻ります(上の「書き手を繋ぎ、1 本の edit_batch で足す(✅UX-12d)」)。pass の編集、Decompose into loop、Replace with loop version…(✅LP-4f2。`apply_graph_template`)、task の追加・編集・削除、render target、feature、frame node の順、route の既定、callable の record、scene の acceleration structure、region の置き換え、✅RSX からの Rendering settings の `edit_rendering_keys`、そして文書を作る・消す編集(Add pass / Remove pass、Fork、Make editable、feature の import)です。
  - `edit_rendering_keys` の項目の名前は、1 つのキーを書けば `Set <key>`、1 つのキーを消せば `Remove <key>`、それ以外は `Edit rendering settings` です([名前を決める所](../../src/devstudio/model/renderedithistory.cpp#L297))。
  - ✅GL-3a / GL-4a から、記録する編集に `fold_nodes`(`Fold group <group>`、`folded: null` なら `Unfold <group>`)・`edit_target_planning`(`Edit planning decisions`)・`verify_fold`(`Verify <group>`)が入りました([fold の編集の項目の名前](../../src/devstudio/model/renderedithistory.cpp#L309))。結果が `running`(検査中の `verify_fold`)の間は、`pending` の 25 ms ではなく 500 ms ごとに問います。この 3 つを履歴から戻す test はありません(未確認)。
  - ✅UX-15b から、Frame Plan の `edit_vulkan_plan_pins`(`Pin backend <candidate>`、`pin: null` なら `Unpin backend`)と `put_vulkan_physical_fragment`(`Freeze physical plan`、`fragment: null` なら `Remove frozen physical plan`)も記録します(記録する method は 22 から 24 に)。この 2 つは `devstudio_render_history_test` の「UX-15b a backend pin and a frozen physical plan are recorded, named, and undone」が RD-1 の restore まで見ます。
  - 項目の名前は `Edit pass present`、`Decompose forward_opaque`、`Add pass ru1_tint` の形です。Replace with loop version… は、engine の受理の応答の `applied.feature` と `applied.template` から `Replace bloom with Bloom loop` になります(応答の前は `Replace with Bloom loop`。title は雛形のファイル名から engine と同じ規則で作ります。[`RenderEditHistory::graphTemplateLabel()`](../../src/devstudio/model/renderedithistory.cpp#L362))。
  - 何も変えなかった編集と、断られた編集は積みません。新しい編集を積むと、Redo の側は空になります。
  - ✅RU-1b から、作る編集の Undo はできた文書(Add pass が書く `passes/authoring/` の fragment のファイルなど)を消し、消す編集の Undo は編集の前に読んだ中身で書き戻します。Redo はその逆です。
  - 戻すときは、今の中身を読んでから `restore_render_documents` を送ります。送る形と CAS は上の「ループと分解の RPC —— engine の契約」の「render 文書を丸ごと読む・戻す」と「undo の手順」のとおりです。
  - 戻した後は、Frame Plan が plan と context を読み直し、Pass Editor がフォームを文書の今の中身で作り直し、ほかの panel には共有の digest で知らせます([戻した後の読み直し](../../src/devstudio/view/mainwindow.cpp#L1217))。route の既定のように engine の再起動が要る編集は、戻すともう一度再起動します。✅RSX から、その文は上の「どのパネルの render の編集も、再起動が要れば同じ形で起動し直します」と同じ `Restarting the engine to apply this change (<code>)...` です。
  - 1 行(`Undid "Edit pass present".` など)が Frame Plan の status 行に出て、status bar にも 8 秒出ます。
- **node を動かした後でも戻せます(✅RU-1b)**: Frame Plan や Graph タブで node を動かすと root の `layout` が書き換わりますが、それより前の編集の Undo は通ります。今の root と「その編集が書いた root」の違いが `layout`(root のものと各 definition のもの)だけなら、戻す中身に後から変わった配置を載せ、今の digest を base にして送ります([配置を載せる所](../../src/devstudio/model/renderedithistory.cpp#L188))。動かした配置は残ります。配置のほかにも違いがあれば、下の「後の編集がある」で断られます。
- **断られるとき**: 後の編集がある(`"<項目>" cannot be undone: <文書> changed since. Undo the later edit first, or refresh.`。履歴は残ります)、予約の場所(`.pelican/`・`assets/` など)に作る・消すことになる、root を消すことになる、のどれかです。後の 2 つと、編集の前に文書を読めなかった編集では、その項目で Undo が止まり、tooltip が `Cannot undo past "<項目>": <理由>` を言います。
- **送信中は待ちます**: render の編集が engine の返事を待っている間は `Wait for the render edit in flight.`、Undo / Redo の最中は `Wait for the Undo or Redo in flight.` が出て、何も送りません。
- 別の project を開くと履歴は消えます。engine が切れたときは、送信中のものだけを捨てて履歴は残します。
- ✅RL-1s から、外のエディタで書き換えた文書を Studio が読み直したときも履歴は消え、Frame Plan の status 行に `— reloaded from disk —` が出ます。Undo で読み直しの前へは戻れません(上の「外のエディタで書き換えた render の文書を拾います」。[履歴を切る所](../../src/devstudio/model/renderedithistory.cpp#L867))。
- ⚠ 分かっている穴(RU-1 / RU-1b の着地時の記録): preset だけを戻したとき(root が変わらないとき)は、Render Features と Graph タブが自分からは読み直しません。`Undid …` の行は、続く Frame Plan の読み直しの status で上書きされることがあります(status bar には残ります)。Render Features と dialog の中のキーは test していません。

**パネルの文字(✅N-7)**: 各パネルの既定状態に出るのは見出し・ラベル・値・1 行の状態だけです。仕組みの説明は control の tooltip か、パネル右上の `?`(この章の該当節を開く)にあります。検査の一覧は記号(✓ / ✗ / –)で、文は hover。notice は言うことが無いときは消えます。文字の大きさとフォントはテーマ表(`studio_themes.json` の `font`)から取り、`devstudio_paneltext_test` がパネルごとの可視文字の上限を固定します。

**notice の文は写せます(✅UX-1)**: パネルの notice の 1 行は、マウスで選べます。右クリックのメニューの **Copy message**(`pelican.panelNotice.copyMessage`)は、行が切り詰めて見せている文ではなく、全文を clipboard に入れます([`panelNoticeMessage()`](../../src/devstudio/view/panelhelp.hpp#L85))。

- 全文は tooltip の文です(engine の拒否なら code と message)。行が tooltip の頭を切ったものならその tooltip だけ、行と tooltip が別の内容なら、行と tooltip を改行でつないだものです。
- notice の行は全部 [`applyPanelNoticeStyle()`](../../src/devstudio/view/panelhelp.hpp#L106) で整えるので、ここで足したメニューはどのパネルの notice にも付きます(この関数を呼ぶのは Graph タブ、Frame Plan、Pass Editor、Render Features、Rendering settings、Inspector、project の移行の panel と MainWindow です)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-1 a notice's Copy message puts the whole message on the clipboard」](../../test/devstudio_graphcanvas_scene_test.cpp#L7965)。

**このタブが出せる RPC は 22 本だけです。**`GraphCanvasReadCapability`
([定義](../../src/devstudio/view/graphcanvasreadcapability.hpp#L36))は「汎用の RPC メソッド」を表現できない形をしていて、
次の 22 個だけを名前で持ちます(LP-4f2 で 11 個になり、LP-4f3 で `get_render_authoring_context` を外して 10 個に戻り、✅GL-1 の `get_graph_templates` で 11 個、✅GL-1e の `get_render_authoring_context` と `edit_preset_pass` で 13 個に、UX-5b の `get_engine_shaders` で 14 個に、✅GL-3a / GL-4a の `get_fold_groups`・`get_render_documents`・`fold_nodes`・`verify_fold` で 18 個に、✅EL-5s の `get_shader_interface` と ✅PS-1s の `rerun_once` で 20 個に(この 2 本はこの段落に書き足されていませんでした。✅AB-1s の時点で struct を数え直しました)、✅AB-1s の `bake_audio_analysis` と `list_derived_assets` で 22 個になりました)。

- 読み: `get_graph_instances` / `get_graph_catalog` / `get_graph_values` / `get_graph_node_values` / `get_status` / `get_frame_plan`
  (`get_graph_node_values` と `get_status` は AG-3b の観測。definition の中の値と、Timeline の軸になる scene time。
  `get_frame_plan` は ✅LP-4c で入り、`loops[]` を読みます。文書にループがあるときだけ問い、✅LP-4e から draw_loop の
  material の続きのページ `{loop_elements: {node, offset, limit}}` も同じ名前で問います。✅LP-4f1 から、root に render node が
  あるときも問い、node の `view_axes` と render target の名前も読みます。
  ✅LP-4f3 から、二次 family の node の `family_extent` / `family_layers` も読みます)、`get_graph_templates`(✅GL-1。feature の畳んだ node の
  Show decomposed version)、`get_render_authoring_context`(✅GL-1e。rendering 節の pass の入力)
- 文書の編集: `edit_graph_document` と、その結果を待つ `get_edit_result`
- rendering 節の pass の入力の編集(✅GL-1e): `edit_preset_pass`。結果は同じ `get_edit_result` で待ちます。params は Frame Plan の
  ソケットへの drop と同じ関数が作ります(上の「Graph から画面に出す(✅GL-1e)」)。`edit_preset_pass` とその `get_edit_result` は、下の分配を通らず、
  Graph タブが直接送ります([送る所](../../src/devstudio/view/graphcanvaswidget.cpp#L6046))。✅GL-5a から、詳細欄の rendering 節の pass の編集(`edit_preset_pass`)と Serial / Isolate / No alias(`edit_target_planning`。`GraphCanvasReadCapability::requestEditTargetPlanning`)も同じ所から送ります
- Timeline の scrub: `set_time` と `render_frame`(1 つの「scrub」にまとめず、2 本の RPC として名前で持ちます)
- engine の shader の一覧(UX-5b。一覧を開くときだけ問います): `get_engine_shaders`
- fold group(✅GL-3a / GL-4a): `get_fold_groups`、group の決定を読む `get_render_documents`(root の reference 1 つだけを渡す)、`fold_nodes`、`verify_fold`。結果は同じ `get_edit_result` で待ちます。この 4 本は下の分配を通らず、fold group の controller が直接送ります([fold group の 4 本の口](../../src/devstudio/view/graphcanvasreadcapability.hpp#L96))
- compute の shader の interface(✅EL-5s): `get_shader_interface`。once の走らせ直し(✅PS-1s): `rerun_once`(文書は変えない)
- 音の解析(✅AB-1s): `bake_audio_analysis`(結果は同じ `get_edit_result` で待つ)と `list_derived_assets`。文書は変えません。この 2 本も下の分配を通らず、音の解析の controller(`GraphCanvasAudioAnalysis`)が直接送ります([音の解析の 2 本の口](../../src/devstudio/view/graphcanvasreadcapability.hpp#L129))

それ以外を要求すると `The graph canvas cannot issue <method>.` になります
([分配](../../src/devstudio/view/graphcanvaswidget.cpp#L6600))。キャンバスから preset を fork することは
**できない**、というのが型で保証されています。pass の編集は ✅GL-1e から `edit_preset_pass` を持ちますが、型が絞るのは
RPC の名前だけで、params は絞りません。送るのが rendering 節の fullscreen pass の `input` だけなのは、呼ぶ側のコード(上の
「線を受ける pass」)によります。

### Studio の Rendering settings タブ(✅RSX)

**置き場所**: render-tools の dock の 3 枚目のタブ「Rendering settings」(`pelican.renderingSettings`)です([タブを足す所](../../src/devstudio/view/mainwindow.cpp#L543))。**View ▸ Render Tools ▸ Rendering settings**(`pelican.view.renderTools.rendering_settings`)か `--tab rendering_settings` で前に出せます。右上の `?` はこの章の「レンダー設定を編集する」を開きます。

**何を書くか**: root の rendering 節のうち、ほかの RPC が書かない 8 つのキーです([`RenderingSettingsWidget`](../../src/devstudio/view/renderingsettingswidget.hpp#L58)、[キーの並び](../../src/devstudio/view/renderingsettingswidget.cpp#L257))。書くのは engine の `edit_rendering_keys`(✅EL-3。上の「レンダー設定を編集する」の「rendering 節のほかのキーを書く(✅EL-3)」)です。RSX の前は、これらのキーを Studio から変える道がありませんでした。

- **読むもの**: `get_render_documents` の `kind` が `root` の文書の `rendering` 節です。ほかのパネルの commit や Undo は共有の digest(✅Q-13)で知って読み直します。✅RL-1s から、外のエディタで書き換えた文書を Studio が読み直したときも、root の digest が変わっていれば同じ道で読み直します(上の「外のエディタで書き換えた render の文書を拾います」)。**Refresh** でも読みます。
- **Shader defines**(`rendering.shader_defines`): UX-4a の `ShaderDefinesForm` の表です(`pelican.renderingSettings.shaderDefines.add` など。上の「生の JSON の欄はフォームになりました」)。rendering 節の defines はすべての shader の compile に足されるので、engine が pass の compile で自分で定義する名前もここでは断りません(feature の `PELICAN_FEATURE_*` の define がこの list に入るため)。C の識別子でない名前と、同じ名前の 2 回は断ります。
- **Draw sort** / **Render strategy** / **Resolver version** / **Multisampling**(`pelican.renderingSettings.<key>`): 選ぶだけの combo です。先頭の項目はキーを書かない(engine の既定)です。
  - Draw sort: `Engine default`、`State batched / back to front`(opaque が `state_batched_v1`、transparent が `back_to_front_v1`)。RSX の着地時は `xr_view_policy` も項目が持ち、`State batched / back to front, sorted per view (XR)`(`per_view`)の項目もありました。✅UX-15a から `xr_view_policy` は横の別の combo です(下の「Draw sort の XR の並べ方」)。
  - Render strategy: `Engine default (builtin.authored_config_v1)` だけです(engine が provider を返さないとき。下の UX-10 の段落)。
  - Resolver version: `Engine default (2)` と `2`。
  - Multisampling: `Off (engine default)`、`2 samples` / `4 samples` / `8 samples`(`{samples: N}`)、`Automatic`(`{mode: "automatic"}`)。
  - engine はこれらの値の一覧を出していません(EL-3 は値を丸ごと受け、候補を組んで判定します)。項目は第6章が名指す値を Studio が持っているものです([項目の表](../../src/devstudio/view/renderingsettingswidget.cpp#L68))。root が一覧に無い値を持っていれば `Custom: <JSON>` の項目で出します。
  - ✅UX-10 から、Draw sort と Render strategy は、engine の `get_registered_providers`(✅EL-6)が名前を返せばそこから作ります([provider から項目を作る所](../../src/devstudio/view/renderingsettingswidget.cpp#L128))。Draw sort は `Engine default` と、opaque / transparent の provider の全部の組 `<opaque> / <transparent>`(例 `state_batched_v1 / back_to_front_v1`。game の provider は `(game)` 付き)です。上の固定の項目は、engine がその種類の provider を 1 つも返さないときだけです。
  - UX-10 から UX-15a の前までは、provider から作る Draw sort の項目が `xr_view_policy: "logical_view_center"` だけを持ったので、engine が provider を返すと `per_view` を選ぶ道がありませんでした(UX-15a の仕様が挙げた不具合)。
- **Draw sort の XR の並べ方(✅UX-15a、`06a9cd2d`)**: Draw sort の combo の横の combo(`pelican.renderingSettings.draw_sort.xrViewPolicy`)で、`Logical view center` / `Per view` です([作る所](../../src/devstudio/view/renderingsettingswidget.cpp#L437))。Draw sort の項目は opaque / transparent の組だけを持ち、root の値は `xr_view_policy` を外して項目と照合します([`withoutViewPolicy()`](../../src/devstudio/view/renderingsettingswidget.cpp#L63))。組を選ぶと横の combo の値を `xr_view_policy` に必ず書き(`logical_view_center` も明示して書きます)、横の combo を変えると今の draw_sort に `xr_view_policy` だけを替えて送ります([`viewPolicyChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1048))。Draw sort が `Engine default` の間は押せません。root が一覧に無い policy を持てば `Custom: <policy>` の項目で出します。
- **Render strategy の name と parameters(✅UX-15a)**: engine は `render_strategy` に `name` を要ります(`render_strategy requires a name string`)。UX-10 から UX-15a の前までは、値の無い root で provider を選ぶと `{provider}` だけを送っていました(UX-15a の仕様が挙げた不具合)。今は provider から作る項目が、root の値に `name` が無ければ provider の名前を `name` に入れます(root に name があればそれを保ちます。[name を埋める所](../../src/devstudio/view/renderingsettingswidget.cpp#L154))。
  - combo の下に name の欄(`pelican.renderingSettings.render_strategy.name`)と parameters の欄(`.render_strategy.parameters`。JSON object の 1 行で、空なら `parameters` を書きません)があり、Enter か focus を外したときに送ります([`strategyFieldChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1022))。root が render_strategy を持たない間は押せません。name を空にすると送らずに `A render strategy needs a name.` を言って元の name に戻し、parameters が JSON object でなければ送らずに `The render strategy's parameters are not a JSON object.` を言います。
  - provider が parameters の schema を出さないので、parameters は打つ欄のままです(engine に回しました)。
- **Snapshots**: ✅UX-10 から JSON ではなく行です。行ごとに名前(`pelican.renderingSettings.snapshots.row<i>.name`)、after の combo(`.row<i>.after`)、削除(`.row<i>.remove`)があり、**+**(`pelican.renderingSettings.snapshots.add`)は `{name: "opaque_snapshot", after: <候補の先頭>}` の行を 1 つ足します。どの変更も、行の全部を `snapshots` の値にした `edit_rendering_keys` 1 回です。名前が空なら送らずに `A snapshot needs a name.` を言います。
  - ✅UX-12b から、after の combo は engine の `get_render_authoring_context` の `snapshot_after_candidates`(✅FR-1)の名前です([after の候補を読む所](../../src/devstudio/view/renderingsettingswidget.cpp#L724))。tab を読むたびに provider の一覧と並べて聞きます。`post_ldr` は `After opaque (post_ldr)`、ほかは `After <名前>` で、行が候補に無い名前を持っていれば `Custom: <名前>` の項目で出します。候補を返さない engine(と答えが来る前)では、UX-10 の 1 つ(`post_ldr`)だけです。
  - `snapshot.after_unknown@1` は `Snapshot <snapshot> cannot be taken after <after>: this pipeline takes it after <a>, <b> or <c>.`(候補が無ければ `…: this pipeline has no pass to take it after.`)の 1 文を、切らずに notice に出します([拒否の文 `snapshotAfterUnknownLine()`](../../src/devstudio/model/featurerefusalfix.hpp#L111))。
  - UX-12b の前の engine では、Blank から Graph で組んだ pipeline で + を押すと `unknown node: post_ldr` で断られていました。engine の FR-1 から `post_ldr` はその graph の anchor に解決され、`Saved snapshots.` で行が残ります(シナリオ `gui_settings_choose`)。
- **Graph transforms** / **XR**: RSX の着地時は、どちらも JSON の欄と **Apply**(`pelican.renderingSettings.<key>.apply`)でした。✅UX-10 から、XR は view execution の combo(`pelican.renderingSettings.xr.viewExecution`: `Auto (engine default)` / `Sequential` / `Multiview`)が前に出て、xr 全体の JSON の欄は **Advanced**(`pelican.renderingSettings.xr.advanced`)を押したときだけ見えます。✅UX-15a から Graph transforms は行になり、XR に Measured gate が足されました(次の 2 つ)。
- **Graph transforms の行(✅UX-15a)**: 行ごとに provider の combo(`pelican.renderingSettings.graph_transforms.row<i>.provider`。engine の `get_registered_providers` の `graph_transforms`。provider を書いていない既存の行に限って先頭に `Default (builtin.identity_v1)`)、name(`.row<i>.name`)、parameters(`.row<i>.parameters`。JSON object の 1 行で、空なら書きません)、↑ / ↓(`.row<i>.up` / `.down`。順を入れ替えます)、×(`.row<i>.remove`)があります([`fillTransforms()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1128))。
  - **+**(`pelican.renderingSettings.graph_transforms.add`)は、engine が最初に挙げる provider の行を、その provider の名前で足します。名前が preset の行か root の行と重なれば `_2`、`_3`… を付けます([`addTransform()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1249))。engine が provider を返さなければ `builtin.identity_v1` です。
  - どの変更も行の全部を `graph_transforms` の値にした `edit_rendering_keys` 1 回で、最後の 1 行を消すとキーを消します([`transformsChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1201))。欄に無い鍵は行が持っていたまま残します。name が空なら送らずに `A graph transform needs a name.`、parameters が JSON object でなければ `The parameters of graph transform <name> are not a JSON object.` を言います。名前の重複などで engine が断れば、engine の文がそのまま notice に出ます(Studio は先回りして検べません)。
  - graph_transforms 全体の JSON は **Advanced**(`pelican.renderingSettings.graph_transforms.advanced`)の下に読むだけで残ります(`pelican.renderingSettings.graph_transforms`)。
- **XR の Measured gate(✅UX-15a)**: view execution の combo の横に **Measured gate** の check(`pelican.renderingSettings.xr.multiviewAuto`)と minimum gain の spin(`pelican.renderingSettings.xr.minimumGainPercent`。0〜99.99 %、小数 2 桁)があります。
  - check を付けると xr に `multiview_auto: {}` を足し、外すと消します。ほかの鍵(`view_execution`、`profiles`)は保ち、xr が空になればキーを消します([`multiviewAutoChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1059))。spin は `multiview_auto.minimum_gain_percent` を書きます。鍵が書かれていなければ 0(engine の既定)と出し、0 のままなら書きません([`minimumGainChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1073))。check が外れている間は spin を押せません。
  - xr 全体の JSON は Advanced の下で今も書けます(**Apply** は `pelican.renderingSettings.xr.apply`)。空で Apply すると xr を消し、JSON object でなければ送らずに `rendering.xr is not a JSON object.` を言います([`xrApplied()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1088))。check・spin・JSON は同じ鍵を書き、commit のたびに root を読み直して 3 つに出すので、check を切り替えると JSON の表示も変わり、JSON で `multiview_auto` を消すと check が外れます。
  - `multiview_auto.profiles` は check や spin では書けず、Advanced の JSON に打ちます(profiles は `get_frame_plan` の値を写す実測値のため。減らす案は「今の auto_gate から profile を作る」ボタン。実装者の報告)。
  - UX-15a の着地の前の版では、この JSON を読むだけにしていて profiles と minimum gain が書けなくなっていました。統合側の指摘で、書ける形に戻してから着地しました(実装者の報告の追記)。
- **preset の値を灰色で見せる(✅UX-15a)**: preset を選ぶ root では、Shader defines・Snapshots・Draw sort・Graph transforms の欄の上に、preset の config の値を `Preset: …` の灰色の行で出します(`pelican.renderingSettings.<key>.preset.row<i>`。[`fillPreset()`](../../src/devstudio/view/renderingsettingswidget.cpp#L976))。行の tooltip は `Comes from preset <name>; fork the preset to change it` で、ここからは書けません(preset の config を書く RPC はありません)。
  - 値は `get_render_documents` の `kind` が `preset` の文書の `config` から読みます([読む所](../../src/devstudio/view/renderingsettingswidget.cpp#L784))。行の形は、snapshot が `<name>, after <after>`、graph transform が `<name> (<provider>)` と parameters、draw sort が `<opaque> / <transparent>, XR <policy>` で、root 自身が draw_sort を持てば ` (the root's below replaces it)` が付きます([`presetLines()`](../../src/devstudio/view/renderingsettingswidget.cpp#L926))。graph transform は engine が preset の分の後ろに root の分を足すので、preset の行が上、root の行が下です。
  - タブの上に 1 行(`pelican.renderingSettings.preset`)が出て、`This root selects preset <name>: what it holds shows in grey. Fork the preset to change it.` を言います。engine 同梱の preset(`engine://…`)は `get_render_documents` に文書が出ない(engine が `project://` の preset だけを列挙する。実装者の報告)ので値を出せず、この 1 行が `This root selects the engine's preset <reference>: its values are not listed here. Fork the preset to see and change them.` を言います。
- **Material route default**(✅UX-10): `Auto (engine default)` / `Forward` の combo で、`set_material_route_default` を送ります。✅UX-12b から、`material_route_unserved@1` で断られると、notice が `No forward material pass serves the route, so it stays on auto — add a forward material pass first.` の 1 文を切らずに出し、欄は root の値に戻ります([拒否を 1 文にする所](../../src/devstudio/view/renderingsettingswidget.cpp#L235))。
- **送り方**: 欄を変えるたびに `edit_rendering_keys` を 1 回送ります。値は `set: {<key>: <値>}`、既定に戻すと `remove: [<key>]` で、`base_source_digest` は読んだ root の digest です([送る所](../../src/devstudio/view/renderingsettingswidget.cpp#L1468))。送信中の変更は、終わって読み直してから送ります(後の変更が勝ちます)。notice は `Saving <key>...` → `Saved <key>.`(何も変わらなければ `<key> was already so.`)です。
  - `external_modification` で断られたら、`The document changed since this panel read it; reading it again...` を言って読み直し、1 回だけ送り直します。
  - ほかの拒否は notice に `<message> (<code>)` を出し、欄を root の値に戻します([断られた所](../../src/devstudio/view/renderingsettingswidget.cpp#L1541))。
- **preset を選ぶ root**(rendering 節が `pipeline` を持つ)では、Multisampling と Resolver version の欄が押せず、tooltip が engine の reason と文を言います(`preset_root_takes_pipeline_settings_msaa: ...` / `not_in_preset_root_keys: ...`。[reason の表](../../src/devstudio/view/renderingsettingswidget.cpp#L222))。engine が `rendering_key_not_allowed_here` で断ったキーも、その reason で押せなくします。
- **完了と再起動**: 完了は全パネル共通の読み(上の「どのパネルの render の編集も、再起動が要れば同じ形で起動し直します(✅RSX)」)を通ります。EL-3 のキーは世代を組むたびに読み直されるので、ふつうは再起動を求めません。組み直しが live の material の物理の契約だけで断られたときは、RS-1 と同じく `restart_required` が返り(上の EL-3 の段落)、このタブもほかのパネルと同じく起動し直します。この道を GPU で通す test はありません(EL-3 の段落と同じ。未確認)。
- **Undo / Redo**: 編集は render の編集の履歴に積まれ、項目の名前は `Set <key>` / `Remove <key>` です(上の「render の編集を戻す」)。Ctrl+Z / Ctrl+Y もこのタブで効きます([キーを渡す所](../../src/devstudio/view/mainwindow.cpp#L1185))。
- test: `devstudio_rendering_settings_test` の [「RSX decision 3: Rendering settings shows the root's rendering keys, sends one edit_rendering_keys per change, and switches off what a preset root refuses」](../../test/devstudio_rendering_settings_test.cpp#L310)、✅UX-12b の [「UX-12b decision 3: a snapshot's after is picked from the places the pipeline offers, and an unknown one is refused with them」](../../test/devstudio_rendering_settings_test.cpp#L1333)・[「UX-12b decision 5: material_route_unserved@1 is one sentence on what to do」](../../test/devstudio_rendering_settings_test.cpp#L1471)、✅UX-15a の決定ごとの 5 本 [「UX-15a decision 1: a render strategy picked on a root without one is sent with the name the engine requires (its provider's); the name and parameters are fields」](../../test/devstudio_rendering_settings_test.cpp#L833)・[「UX-15a decision 2: draw_sort's XR policy is a combo of its own beside the pair, and Per view is sent with the pair」](../../test/devstudio_rendering_settings_test.cpp#L881)・[「UX-15a decision 3: graph_transforms is a row each -- provider, name, parameters, up / down, x -- and + adds the first provider named after it; the JSON is read-only」](../../test/devstudio_rendering_settings_test.cpp#L909)・[「UX-15a decision 4: xr.multiview_auto is a check with its minimum gain a spin, and the xr JSON under Advanced writes the same key -- each shows what the other wrote」](../../test/devstudio_rendering_settings_test.cpp#L964)・[「UX-15a decision 5: a root that selects a project preset shows the preset's shader_defines, snapshots, draw_sort and graph_transforms as grey rows; the engine's own preset is said to be unlisted」](../../test/devstudio_rendering_settings_test.cpp#L1027)。UX-15a で既存の `UX-10 decision 7` は Draw sort の項目の数を 3 から 2 に、`RSX decision 3` は拒否を起こす手段を check と JSON + Apply の 2 通り(`GENERATE`)にしました。GPU シナリオは `rendering_settings_from_blank`(✅UX-15a で延長)と `gui_settings_choose`(下の「Studio の自動化面」)です。
- **残した穴と手で打つ欄(✅UX-15a。実装者の報告)**: engine 同梱の preset の値は見せられません(engine に回しました)。打つ欄として残るのは、Render strategy と Graph transforms の各行の name(既定は provider の名前が入るので、変えたいときだけ)と parameters(JSON の 1 行。provider が schema を出せばフォームにできる)、XR の Advanced の JSON(主に `multiview_auto.profiles`)です。

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
([宣言のコメント](../../src/devstudio/automation/automationserver.hpp#L30)) —— 出荷時の挙動は変わりません。
チャネルは**逐次**で、命令の実行中に次の行が来たら `automation.busy@1` で断ります
([判定](../../src/devstudio/automation/automationserver.cpp#L102))。認証はありません(flag を付けたときだけ開く口です)。

**命令は 23 個**です([一覧](../../src/devstudio/automation/automationcommands.cpp#L2233)):
`list` / `click` / `double_click` / `drag` / `wheel` / `move` / `press` / `release` / `key` / `type` / `text` / `select` /
`action` / `menu_list` / `view_state` / `layout_stats` / `wait` / `wait_engine` / `rpc` /
`open_project` / `screenshot` / `viewport_size` / `quit`。`rpc` は `EmbeddedViewport` 経由で engine へ送り、応答をそのまま返します。
この文書は長く 20 個と書いていましたが、一覧には `select`(AG-3b。下の段落)と `layout_stats`(名前を指した view の scene で自動配置が達成したものを読む。コードのコメントの読み)が既にあり、
✅PX-2(`28bc5ef9`)で `viewport_size`(下の「viewport の大きさを決める(✅PX-2)」)が増えました。
イベントは `QTest` の合成なので、**窓が前面にあるかどうかに依存しません**。拒否は名前付きで、
`automation.widget_missing@1` / `bad_request@1` / `unknown_command@1` / `engine_not_ready@1` / `rpc_failed@1` /
`wait_timeout@1` / `screenshot_failed@1` / `project_failed@1` / `busy@1` / `command_threw@1` の 10 種です
([定数](../../src/devstudio/automation/automationcommands.cpp#L238))。

**`wait_engine` は「今の」engine を待ちます(✅Q-9)。**Frame Plan の保存が engine の再起動を求めると(G-4)、Studio は再起動の
前に古い engine へ未保存の編集を問います(G-5)。その往復の間も古い engine は editor RPC に答えるので、以前の `wait_engine` は
その engine で通り、次の step が止まった engine に当たって `automation.engine_not_ready@1` で落ちていました。今は
**再起動は頼まれた時点で始まったと数え**、そこから新しい engine の process が最初の `get_status` に答えるまで、`wait_engine` は
通りません([wait_engine の判定](../../src/devstudio/automation/automationcommands.cpp#L1900)、
[再起動待ちを立てる所](../../src/devstudio/view/mainwindow.cpp#L2022))。Cancel などで再起動が見送られれば、すぐ今の engine で
通ります。再起動を待っている間に Studio が送る editor RPC は古い engine に届き(未保存の問いと Save はそこへ届く必要が
あるため)、engine が止まった後は既存の 1 行 `The engine viewport is not ready for editor RPC requests.` で断られます。
Stop ボタンや project の切り替えのような普通の停止は、process が終わるまで ready を落としません(`open_project` はその終わりを
待ちます)。

**指し方は objectName です。**キャンバスの中に描かれたもの(ノード、ソケット、値の帯、その場編集の editor)は
widget ではないので、`<view の objectName>::<種類>#<ノード id>[/<port>]` の合成名で指します ——
例: `pelican.graphCanvasView::graph_value_pill#left/gain`([名前の生成](../../src/devstudio/automation/automationcommands.cpp#L280))。
`list` は widget 名と合成名の両方を返すので、**ソースを読まずに一覧からシナリオを書けます**。
`type` / `key` / `text` は item が抱えている埋め込み widget へ届きます。`select{name, text|index}` は item view の行を本物の click で、combo は index を直接置いて選び、`wait{enabled}` は有効化を待ちます(AG-3b)。✅Q-17 から、`select` は無効な widget(祖先が無効なものを含む)を断り、combo が選んだ項目のままでなければ `"<name>" did not take "<text>"; it holds "<今の値>"` で失敗します(規則の正は [README の Q-17 の段落](../../test/studio_scenarios/README.md#L452))。出力 port の帯は `graph_value_band#<node>/<port>`([経路](../../src/devstudio/automation/automationcommands.cpp#L541))。
list や tree の行そのものは **`<view の objectName>::row#<表示の文字>`**(✅OL-1)で指します。対象は画面に出ている行だけで
(閉じた枝の中の行は見つかりません)、view をその行まで scroll してから行の中央を押すので、`drag` の始点・終点や
`wait{missing}` に使えます(例: `pelican.outliner::row#Floor`。[行の名前を引く所](../../src/devstudio/automation/automationcommands.cpp#L445))。

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

**`list` の `checked`(✅SK-1、`67fd3d29`)**: `list` の widget の項目は `name` / `class` / `visible` / `enabled` / `geometry` / `text` / `shortcut` を返し、
check できるボタン(`QAbstractButton` で `isCheckable()` が真のもの。`QCheckBox` など)だけ `checked`(真偽)も返します
([`checked` を足す所](../../src/devstudio/automation/automationcommands.cpp#L962))。check box の `text` は caption(例 `Double sided`)なので、
印が付いているかはこちらで読みます(例 `{"cmd": "list", "pattern": "pelican.materials.doubleSided"}` の `widgets[0].checked`)。
check できない widget の項目には `checked` の鍵がありません。test は `devstudio_automation_input_test` の
[「SK-1: list says whether a check box is ticked」](../../test/devstudio_automation_input_test.cpp#L623)(`QCheckBox` の false → true が `checked` に出て、
`QPushButton` の項目には無い)です。なお今の `list` は、名前の付いた `QAction` も `class` が `QAction` の項目(`visible` / `enabled` / `checked` /
`text`)で返します([`QAction` の項目](../../src/devstudio/automation/automationcommands.cpp#L993))。上の「`QAction` は `list` には出ません」は
Q-1b の着地時の記述で、今のコードとは違います(コードの読み)。発火は今も `action` です。

**シナリオは JSON で、`test/studio_scenarios/<名前>.json` に置きます**(書式は
[README](../../test/studio_scenarios/README.md#L61))。step は「命令をそのまま送るもの」か「期待」で、期待は
`rpc_equals`(engine の応答の `path` が `value`)/ `result`(任意の命令を送り、応答の `path` を記録するか `value` と比べる)/
`text_equals` / `visible` / `list_count` / `screenshot`(保存のみ)の 6 種です([判定](../../tools/studio_drive.py#L920))。`{project}` / `{artifacts}` と `--var NAME=VALUE` は step のすべての文字列に差し込まれます。
`result` の比べ方 `op` は `eq`(既定)/ `ne` / `gt` / `ge` / `lt` / `le` / `contains` / **`excludes`**(✅F-7。文字列がその値を
含まない —— 「その文言は出なかった」の書き方。[比較](../../tools/studio_drive.py#L548))です。`result` も **`retry_ms`**(✅F-5)を受け、
条件を満たすか時間が尽きるまで 0.2 秒おきに問い直します(`rpc_equals` は前から受けていました)。shader を保存した後に
engine が描くものは engine の時計で届くので、`fullscreen_pass_from_template` は `screen` の screenshot をこれで待ちます
([問い直し](../../tools/studio_drive.py#L966))。
✅PM-3a から、`result` は `value_of` に数を足す **`plus`**(`"value_of": "frame", "plus": 1` は 1 歩後)と、覚えた値から比べる
文を作る **`value_format`**(`"Paused · frame {paused_frame}"`)も受けます([`plus` を足す所](../../tools/studio_drive.py#L950)。
書式の正は [README](../../test/studio_scenarios/README.md#L129))。
3 つ目の種類の **`file` step**(✅F-3)は Studio へ送らず、driver 自身が run の持つ directory の中の file を 1 つ読み書きします ——
人が Studio の操作の合間に自分のエディタで file を直す、の代わりです。`write` / `replace`(`old` がちょうど 1 か所のときだけ)/
`contains` / `exists` と、✅IA-1s の `copy`(`from` の file を `path` へ byte のまま写して上書きする。`write` は UTF-8 の文字しか書けないので、人が外で焼き直した binary の file を project の file に上書きする代わり。[copy の所](../../tools/studio_drive.py#L820))の 5 つです。書ける範囲は **`{project}`(fixture の複製)と `{artifacts}`(その run の成果物の置き場)の
2 つ**で、File > New Project でシナリオが作った project は `{artifacts}` の下にあるので、そこにも届きます(✅F-4)。
それ以外の path(`..` で外へ出るものを含む)は step の失敗です([file step の範囲の検査](../../tools/studio_drive.py#L795))。`copy` は `from` もこの 2 つの中でなければ失敗です。
driver の stdout / stderr は、cp932 の console で出せない文字があっても止まりません。
`"probe_no_automation": true` を書くと、**先に flag 無しの Studio を起こして socket が無いことを確かめて**から本番を起動します。
最上位の **`"viewport": [w, h] | null`**(✅PX-2)は、driver が最初の step の前に `viewport_size` で固定する engine の viewport の大きさ(画の画素)です。
省くと 720x450、`null` なら固定しません(下の「viewport の大きさを決める(✅PX-2)」)。
最初の失敗で run は止まります。

**回し方**: [`tools/studio_drive.py`](../../tools/studio_drive.py#L1336)(Python 3.11以上、標準ライブラリだけ)が Studio を
`--automation --project --tab` で起動し、✅PX-2 から Studio とつないだ直後・最初の step の前に `viewport_size` で viewport を 720x450 に固定し
([固定する所](../../tools/studio_drive.py#L1443))、step を順に送り、`report.json` を書きます。ctest からは
`studio_scenario_<名前>` として GPU lane で走ります([登録](../../test/CMakeLists.txt#L3646)) —— **実物の窓が要るので
`QT_QPA_PLATFORM=offscreen` は掛けません**。fixture の project は temp に 2 つ複製されるので、シナリオは
文書を書き換えてよく、「ディスクから読み直す」も試せます([複製](../../test/run_studio_scenario.cmake#L30))。✅IA-1s から、シナリオの隣に `<名前>.prepare.py` があれば、ctest の口([prepare の script を走らせる所](../../test/run_studio_scenario.cmake#L68))が driver を起こす前に`<名前>.prepare.py --out <run の directory>/incoming --cli <pelican_cli>` を走らせます。人が Studio を開く前に外で作る file の代わりで、シナリオは `{artifacts}/incoming/` の file を Studio の Import で取り込みます(今は `gui_build_ltc` と ✅AB-1s の `gui_build_audio_reactive` の 2 本。[第10章](10_tools.md) §10.7)。
CTest登録にはStudio targetと`PELICAN_PYTHON_TESTS=ON`（またはPythonが見つかった`AUTO`）が必要です。
`uv`は不要で、完全なGPU gateは全scenarioの登録・実行を必須にします([CI運用](../ci.md))。

**run ごとに空の設定 directory を使います(✅F-5)。**Studio は環境変数 **`PELICAN_STUDIO_CONFIG_DIR`** があれば、利用者の
設定 directory の代わりにそこへ dock の layout を読み書きします([置き場の決定](../../src/devstudio/view/mainwindow.cpp#L117))。
driver は run ごとに `{artifacts}/studio_config` を空にして作り、この変数で渡します([渡し方](../../tools/studio_drive.py#L1385))。
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
`PELICAN_AUDIO_BACKEND=null` を入れます([無音にする所](../../tools/studio_drive.py#L1376))。呼び出し側が既に値を設定していれば
(`device` / `offline` など)その値を尊重し、**空文字は未設定として扱って** `null` に置き換えます(player と同じ扱い)。シナリオ
`graph_read` は `get_status` の `audio.backend` が `null` で、`audio` に `fallback_reason` が無い(鍵が `available`・`backend`・`voices` の 3 つだけ)ことを確かめます。
engine の ✅CT-1(`ac8d836b`)から `audio` には `voices` が常に付くので(audio を外した build を除く。[第8章](08_gameplay.md) の「`get_status` の `audio`」)、`9ca2d3ce` でシナリオの `audio.$len` の期待を 2 から 3 にしました。この文書ではシナリオを走らせて確かめていません(未確認)。
Q-10 の時点では、埋め込んだ player は窓があるので実物の device を選び、シナリオの間も speaker から音が出ていました。

**report.json は AI が読む形です**([組み立て](../../tools/studio_drive.py#L1508)): `verdict`(`green` / `red`)と
`ok` / `failed_steps`、`summary`(step ごとに 1 行)、`failures`(送った命令・返ってきた応答・言葉での理由・
**失敗した瞬間のスクリーンショット**)、`steps`(全文)、`studio_log`(Studio と engine の console 出力)。
`studio_exit_code` と `studio_shutdown_forced` も記録します。✅PX-2 から `steps` には `viewport` の step(`the viewport is held at 720x450`)が入り、`steps_planned` もそれを数えます(`"viewport": null` のシナリオを除く)。最後の shutdown step は
**exit 0、強制終了なし**を要求し、異常終了・タイムアウトによる強制停止も report と driver の失敗になります。

**Play の間の `quit`(✅UX-7)**: 自動化面の `quit` は、閉じる前に MainWindow の [`stopPlayThen()`](../../src/devstudio/view/mainwindow.cpp#L1792) を通ります([渡す所](../../src/devstudio/view/uimain.cpp#L49))。Play の帯が Playing か Paused(か run がある)と言う間は、帯が送れる時に keep なしの Stop を送り(2 秒ごとに送り直します)、run が終わってから閉じます。15 秒たっても終わらなければ、待つのをやめて閉じる処理へ進みます(Stop に engine が答えないときに止まらないため。そのときに確認のダイアログが出るかは確かめていません。未確認)。UX-7 の前は、Play の間の `quit` が窓の close の「再生を終える確認」のダイアログで止まり、30 秒で強制終了になっていました(`gui_build_ocean` の間欠の失敗)。人が窓を閉じるときは今も確認が出ます。

**`get_status` の返事は 1 回だけ parse します(✅UX-7)**: viewport は、誰が送った `get_status` の返事でも、生の bytes の signal `engineStatusRead` の後に、1 回 parse した JSON を `engineStatusParsed(request_id, status)` で渡します([渡す所](../../src/devstudio/viewport/embeddedviewport.cpp#L834))。Play の帯・Outliner の runtime の行・`wait_engine`([`render.published_frames` を読む所](../../src/devstudio/automation/automationcommands.cpp#L1826))はこれを読みます。UX-7 の前はそれぞれが GUI の thread で parse していて、ループを 16 回展開した graph で約 2 MB の返事が GUI を止めていました(仕様の背景。engine の ✅ST-1 で返事自体も小さくなりました)。UX-6b の `PlanPerfHub` は生の bytes を受けて thread pool で parse します。

現在のシナリオは **70 本**です(`test/studio_scenarios/*.json` の数。✅PB-1s の `gui_problems_window` で 70 本になりました。✅PX-2 は新しいシナリオを足さず、`gui_settings_choose`・`gui_build_C`・`gui_build_full`・`gui_build_optimize` の画素を読む点を選び直し、`whole_frame_first_steps` に `"viewport": null` を足しました(下の「viewport の大きさを決める(✅PX-2)」)。✅RR-1s も新しいシナリオを足さず、`gui_problems_window` を 64 step から 79 step に延ばしました。PB-1s は `gui_render_fallback` の帯の文の期待も 1 か所直しました(末尾の ` See Problems.`)。✅PX-1 は新しいシナリオを足さず、`gui_build_C` と `gui_settings_choose` の画素を読む点を選び直しました(下の「画素を読む点と窓の大きさ(✅PX-1)」)。それまでの数え方: ✅BS-1s の `gui_render_fallback` で 69 本になりました。✅MC-1s は新しいシナリオを足さず、GL-5c が再起動を前提にした `input_needs_shader`・`deferred_loop_from_scratch`・`frameplan_layout_then_edit` を再起動しない流れに戻しました。✅MT-3s は `gui_material_from_blank` を延ばしました。✅SK-1 も新しいシナリオを足さず、MC-1s が戻さなかった `sockets_from_shader`・`sockets_fix_and_show` を再起動しない流れに戻し、`gui_material_from_blank` を延ばしました。それまでの数え方: ✅GL-5c の `gui_frameplan_delegates` で 68 本になりました。GL-5c は既存の 15 本の期待も変えました(下の `gui_frameplan_delegates` の後の段落)。それまでの数え方: ✅GL-5b の `gui_graph_new_target` で 67 本になりました。✅MT-2s と ✅MT-2s-b は新しいシナリオを足さず、`gui_material_from_blank` を延ばしました。それまでの数え方: ✅MT-1s の `gui_material_from_blank` で 66 本になりました。それまでの数え方: ✅CT-6s の `gui_build_rt_from_blank` で 65 本になりました。✅UX-15b の `gui_lower_layer_pin_freeze` で 63 本に、✅GL-5a の `gui_folded_pass_edit` で 64 本になりました。✅UX-15a は新しいシナリオを足さず、`rendering_settings_from_blank` を延ばしました。それまでの数え方: ✅HW-2s / AV-1s の `gui_assets_watch_verify` で 62 本になりました。それまでの数え方: ✅AB-1s の `gui_build_audio_reactive` で 61 本になりました。✅UX-12e は `gui_node_parameters` を、✅UX-12d と ✅UX-13d は `gui_settings_choose` を延ばしました。✅UX-14 は、CC-1 の後の絵に合わせて `gui_settings_choose`・`gui_build_ocean`・`gui_build_ocean_compute`・`gui_build_ltc` の期待を直しました(`gui_settings_choose` は CC-1c の後に `a954d38c` でもう一度)。それまでの数え方: ✅UX-10 の `gui_settings_choose` で 59 本に、✅UX-12a の `gui_node_parameters` で 60 本になりました。✅UX-12c は `gui_build_D` を、✅UX-12b は `gui_settings_choose` を延ばし、全シナリオの run で古くなっていた 4 本の期待(`deferred_loop_from_scratch` の光の向きの Raw、`menu_actions` の File の 5 項目、`gui_build_optimize` の provider 名の Draw sort、`loop_zone_wire` の Frame Plan の status 行。最後の 1 本は Studio の側を直しました)を直しました。それまでの数え方: ✅UX-8 の `gui_build_curve` で 57 本に、✅EL-5s の `gui_build_ocean_compute` で 58 本になりました。✅PS-1s / IA-1s / UX-11 は新しいシナリオを足さず、`gui_build_ocean`・`gui_build_ocean_compute`・`gui_build_ltc`・`graph_from_blank` を延ばしました。それまでの数え方: ✅GB-3 の `gui_build_ocean` と `gui_build_ltc` で 54 本に、✅RL-1s の `render_reload_external` で 55 本に、✅GB-4 の `gui_build_music` で 56 本になりました。それまでの数え方: ✅UX-3 の `graph_palette_kinds` で 43 本に、✅GB-1 の `gui_build_A`〜`gui_build_E` で 48 本に、✅RSX の `rendering_settings_from_blank` で 49 本に、✅GB-2 の `gui_build_full` と `gui_build_optimize` で 51 本に、✅GL-3a の `graph_fold_decisions` で 52 本になりました。それまでの数え方: ✅LP-4f1 の `loop_view_axis` と ✅LP-4f2 の `graph_template_bloom` で 31 本から 33 本に、✅EW-2 の `engine_window_embedded` で 34 本に、✅Q-19 の `loop_zone_read` で 35 本に、✅PM-3a の `play_pause_step` で 36 本に、✅PM-3b の `play_mode_roundtrip` と `play_view_camera` で 38 本に、✅PM-3c の `play_runtime_objects` で 39 本に、✅PM-3d で `play_view_camera` を `play_mode_roundtrip` にまとめて 38 本に、✅GL-1 の `graph_folded_features` で 39 本に、✅GL-2b の `graph_layers_origin` で 40 本に、✅GL-1d の `graph_from_blank` と `graph_from_blank_forward` で 42 本になりました)。
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
つながっていないためです(待つ step が 1 つ増えて 29 step)。✅GL-5c から、最後の Add fullscreen pass... は Pass Editor を開かず、Graph に `render_fullscreen` が置かれて pass の欄が開くことを見ます(31 step。下の「GL-5c で期待を変えた既存の 15 本」)。
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
desktop で開かず、`automation: shader source open <path>` を log に書きます([差し替え](../../src/devstudio/view/uimain.cpp#L57))。
✅GL-5c から、この fixture の root は graph で preset を選ばないので、pass は Frame Plan の Add fullscreen pass ではなく
**View > Pass Editor** と Pass Editor の **New pass**・Refresh・position で足します(json の step は 49。下の「GL-5c で期待を変えた既存の 15 本」)。
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
✅GL-5c から(json の step は 93)、`draw_pattern` は View > Pass Editor と Pass Editor の New pass で足し、`present` に `scene_color` を
読ませるのは Pass Editor ではなく Frame Plan のソケットへの drag(Whole frame で `draw_pattern/out:scene_color` → `present/in:variadic`)です。
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
✅GL-5c から(json の step は 38)、この fixture は direct root なので Add material pass... は Graph の palette になり、form・
Create missing targets・`scene_color` の宣言は見ません。palette の `forward_opaque_v1` が graph の `nodes` に入り、`present` が
その自前の image を読むと model が画面に出ることと、`render-targets.json` が作られないことを見ます(form の道は preset root のものになりました)。
✅G-3 の `lighting_pass_template`(41 step)は、G-buffer を持ち box 1 つと directional light 1 つを置いた fixture で、engine の
`lighting_pass` の後に Add fullscreen pass で、G-buffer 5 枚を読み `lit_color`(画面に届く色)に書く pass を New… の
Deferred lighting で作ります。viewport が 1 色でないこと(box が光源で照らされる)、`file` step で shader の light loop を
切ると画面が 1 色になり、戻すと照らされた box が戻ることを見ます
([lighting 雛形のシナリオ](../../test/studio_scenarios/lighting_pass_template.json))。
✅GL-5c から(json の step は 43)、pass は Frame Plan の Add fullscreen pass ではなく View > Pass Editor と Pass Editor の New pass・
Refresh・position(`lighting_pass` の後の anchor)で足します。
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
戻っています。✅GL-5c から(json の step は 56)、Add material pass... は Graph の palette の `forward_opaque_v1` で(form と
Create missing targets は通りません)、`present` へは Graph で `render_material_pass/out:color` を `+ input` へ drag し、
`managed_fragments` が 0 件であることを見ます。
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
✅GL-5c から(json の step は 88)、Add material pass... は Graph の palette の `forward_opaque_v1` で、`present` の入力は Pass Editor
ではなく Graph の drag です(期待の名前は `render_material_pass` と `render_material_pass__color`)。
`deferred_loop_from_scratch`(118 step。✅UX-12b の前は 116)は同じ出だしに加えて Cube を縦軸の回りに回し(`pelican.inspectorEuler.transform.rotation.1`
に `45` を打ち、quaternion の欄 `pelican.inspectorVector.transform.rotation.1` が `0.3827` になることを見ます)、
Add material pass... の Deferred geometry で G-buffer 5 枚と `scene_depth` を作ります(枠は埋まって開きます。route は既定の
ままなので再起動は求められません)。canvas の Target を **Whole frame** にして `deferred_geometry` を見出しで選び、
Add fullscreen pass... の New... で Deferred lighting を選ぶと、入力に G-buffer 5 枚が役割の順に自動で入ります。color 出力は
一覧の `New target: deferred_lighting_color...` で作ります(New render target の form が名前入りで開き、OK で pass の出力に
入ります)。この pass は `deferred_geometry` の後に置かれ、engine はこれを geometry と `present` の間で走らせます。`present` は
New... の Tone map で作る shader で `deferred_lighting_color` を読みます(照明の HDR をそのまま出すと 2 面の色の差が
保証できないため、passthrough のままにしません)。`present` は `engine://passthrough` で描いているので、提案される名前は
pass 名の `present` で(✅VI-2。以前は `present_tone_map`)、Fragment の Pick... の一覧からも同じ stem を選べることを見ます。光源が箱の 2 面を違う色に塗ること、Inspector で光源の向き
(`pelican.inspectorVector.light.direction.0`)を変えると光の離れた面の色が変わることを見ます。✅UX-12b から、その前に
**Raw**(`pelican.inspectorDirectionRaw.light.direction`)を押します。UX-10 から光の向きは方位角と仰角の欄で、x / y / z の欄は
Raw を開くまで隠れているためです(全シナリオの run で、この欄が見えるようにならずに落ちていました。仕様の記録)
([deferred ループのシナリオ](../../test/studio_scenarios/deferred_loop_from_scratch.json))。
✅GL-5c から(json の step は 100)、G-buffer は Graph の palette の `deferred_geometry_v1` で、lighting は Frame Plan の Add fullscreen
pass...(Graph が `render_fullscreen` を置いて選ぶ)と Graph の pass の欄の New… > Deferred lighting(✅UX-5a で G-buffer の線が自動で
引かれる)で作ります。`present` は Tone map の雛形を作らず、lighting の出力を Graph で `present` の `+ input` へ drag して passthrough で出します。
✅MC-1s から(json の step は 97)、G-buffer を置いた後の engine の起こし直しと未保存の scene の問いに Save で答える 4 手を外し、viewport の status が
`Restarting the engine` を含まないことを見ます(GL-5c の前と同じ確かめ方)。
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
✅Q-13 の `frameplan_layout_then_edit`(41 step)は、配置の保存の後の編集を 2 回見ます。fixture で Graph タブの instance `left` を見出しで drag し、配置が保存された(`get_graph_instances` の `layout.nodes.left`)後に、Frame Plan の New render target... で `q13_after_graph` を作り、fragment の file に入ることを見ます。続けて Blank pipeline の project を作り、Frame Plan の `present` を drag して配置が保存された直後に Add material pass... → Deferred geometry → OK とし、engine が `present` の後に `deferred_geometry` を走らせ、target が 7 つ(6 枚と engine の display)あり、配置が残っていることを見ます。どちらでも status 行が `external_modification` を言わないこと(`excludes`)を確かめます([配置の後の編集のシナリオ](../../test/studio_scenarios/frameplan_layout_then_edit.json))。✅GL-5c から(json の step は 49)、Blank の側の Add material pass... は Graph の palette の `deferred_geometry_v1` で、pass は graph の `nodes` に入り、配置の後の digest で通ることを Graph の編集で見ます。✅MC-1s から(json の step は 50)、ゾーンを待つ「after a restart, if …」の 180 秒を、再起動しない前提の 60 秒に戻し、viewport の status が `Restarting the engine` を含まないことを見ます。
✅Q-14 の `input_needs_shader`(69 step)は、Blank pipeline の project を作ったら、まず `file` step で入力を読まない `shaders/gradient.frag` を書き、Pass Editor の fragment の一覧(`pelican.fullscreenPass.shader.fragmentChoice`)で `present` に選んで Save します(Blank の `present` の `engine://passthrough` は入力が可変で、入力を足しても断られないため。✅VI-2)。続けて Cube と Deferred geometry の pass を足し、Target を Whole frame にして `gbuffer_albedo` の出力ソケットを `present` の node へ drag します(`gradient.frag` は入力を宣言しないので、`present` の行は名前順のままです)。✅VI-3b から Studio は送らず、status 行が `present's shader declares no inputs.` を含み、2 つのボタン(Use engine://passthrough / Open the shader)が出て押せること、engine の `present` の入力が 0 のままであることを見てから、Use engine://passthrough を押します。`present` が `gbuffer_albedo` を `engine://passthrough` で読むこと(1 回の編集)、`shaders/present_pass_through.frag` ができず `gradient.frag` は残っていること、ボタンが消えること、canvas が `in:gbuffer_albedo` を描くこと、viewport の中央が gradient でなくなり箱の albedo が背景と違う色になることを確かめます。VI-3b より前は engine の拒否(`present's shader (gradient) reads no inputs. ...`)を待ち、Make a pass-through shader が書いた `shaders/present_pass_through.frag` を見ていました。step 数は変わっていません([shader が読まない入力のシナリオ](../../test/studio_scenarios/input_needs_shader.json))。✅GL-5c から(json の step は 70)、`gradient.frag` は Pass Editor ではなく、`present` のダブルクリックで開く Graph の詳細欄の Fragment で選び、Deferred geometry は Graph の palette で足し、drag するのはその自前の albedo の image(`render_material_pass/out:render_material_pass__albedo`)です。✅MC-1s から(json の step は 71)、GL-5c が足した再起動の 4 手(未保存の scene の問い・Save・新しい engine の起動待ち・起動後の `wait_engine`)を外し、Add material pass… の前に覚えた `get_status` の `instance_id` が置いた後も同じであること(engine が同じ process のまま)、viewport の status が `Restarting the engine` を含まないこと、`get_graph_instances` の `nodes[1]` が `render_material_pass__albedo` で format が `B8G8R8A8_SRGB`(SDR の表)であることを見ます。
✅VI-3a の `sockets_from_shader`(93 step)は、Blank pipeline の project で `present`(`engine://passthrough`)のソケットが可変の帯の端 `in:variadic` だけで、`in@0` が無いことを見てから、Cube と Deferred geometry の pass を足し、`gbuffer_albedo` の出力ソケットを帯の端へ drag します。`present` の入力が `gbuffer_albedo` 1 つだけになり、帯の中に `in@0` が出て(以前の名前 `in:gbuffer_albedo` でも引けます)、viewport が gradient でなくなることを見ます。`gbuffer_normal` を `present` の見出しへ落とすと、status 行が `Drop on a socket of present: its inputs follow its shader's declarations.` を言い、入力は 1 つのままです。続けて `file` step で固定の宣言 2 つ(`PELICAN_DECLARE_INPUT_0` / `_1`)を持つ `shaders/two.frag` を書き、Pass Editor の fragment の一覧で `present` に選んで Save すると、帯が消えて `in@1` が空のソケットで出ます。`gbuffer_normal` を `in@1` へ drag すると 2 つ目の入力になり、`deferred_geometry` から `present` への線が 2 本になります。`file` step で binding 1 の宣言を消すと、status 行が `present keeps drawing with its previous shader: the new one no longer declares binding 1 (gbuffer_normal).` を言い、Disconnect と Open the shader が出ること、engine の `pass_inputs.reload_refused.declarations` が 1 つで `declarations` は 2 つのままであることを見ます(赤い ○ は screenshot に撮るだけです)。Disconnect を押すと入力が `gbuffer_albedo` だけに戻り、`reload_refused` が消えて宣言が 1 つになり、Disconnect のボタンと `in@1` が消えて線が 1 本に戻ることを確かめます([宣言からのソケットのシナリオ](../../test/studio_scenarios/sockets_from_shader.json))。✅GL-5c から(json の step は 95)、Deferred geometry は Graph の palette で足し、drag するのはその自前の albedo / normal の image で、`two.frag` を選ぶのも `present` のダブルクリックで開く Graph の詳細欄です。✅SK-1 から(json の step は 94)、GL-5c が足した再起動の 4 手(未保存の scene の問い・Save・新しい engine の起動待ち・起動後の `wait_engine`)を外し、Add material pass… の前(step 19)に `get_status` の `instance_id` を覚えて、置いた後も同じであること(step 29)、viewport の status が `Restarting the engine` を含まないこと(step 30)を見ます。ゾーン `graph_zone#render_material_pass_draws` と `wait_engine` の待ちは 180 秒から 60 秒に戻しました(MC-1s の `input_needs_shader` と同じ形)。
✅VI-3b の `sockets_fix_and_show`(94 step)は、Blank pipeline の project で `present`(`engine://passthrough`)が帯の端 `in:variadic` だけで始まることを見て、Cube と Deferred geometry の pass を足し、`gbuffer_albedo` と `gbuffer_normal` を順に帯の端へ drag します。入力が 2 つになり、`in@0` の字が `← gbuffer_albedo (shown)` で終わることを見てから(define が無ければ入力 0 を出すため)、viewport の中央の色を覚えます。`in@1` を click すると、engine の `present` の `shader.defines` が `PELICAN_PASSTHROUGH_INPUT=1` の 1 つだけになり、入力は 2 つのまま、`in@1` の字が `(shown)` で終わって `in@0` からは消え、viewport の中央の色が変わります(normal の色を覚えます)。帯の横の Fix(`node_socket_fix#present`)を押すと、fragment が `project://shaders/present` になり、`PELICAN_PASSTHROUGH_INPUT=1` が残り、`file` step で `shaders/present.frag` が目印の 1 行と `layout(set = PELICAN_SET_PASS_INPUT, binding = 1) uniform sampler2D in_gbuffer_normal;` を持つこと、ソケットの字が `in_gbuffer_albedo ← gbuffer_albedo` / `in_gbuffer_normal ← gbuffer_normal` で帯が無いこと、engine の `pass_inputs` が固定の宣言 2 つ(2 つ目が `in_gbuffer_normal`)で `variadic_range` が null であること、viewport の中央が覚えた normal の色のままであることを見ます。最後に `present` を見出しで選んで `pelican.framePlan.makeSocketsVariadic` を発火し、file に `PELICAN_DECLARE_VARIADIC_INPUTS(0, 8);` が戻って目印が消えること、帯が戻り engine の `variadic_range.max` が 8 になること、入力が帯に残り viewport がnormal の色のままであることを確かめます([固定化と出す入力のシナリオ](../../test/studio_scenarios/sockets_fix_and_show.json))。✅GL-5c から(json の step は 97)、Deferred geometry は Graph の palette で足し、帯へ落とすのはその自前の albedo / normal の image です。Fix sockets が作る宣言の名前は `in_render_material_pass_albedo` / `in_render_material_pass_normal` になりました(資源名の `__` が `_` に詰まります)。✅SK-1 から(json の step は 96)、`sockets_from_shader` と同じ置き換えです: 再起動の 4 手を外し、step 19 で覚えた `instance_id` が置いた後も同じこと(step 29)と、viewport の status が `Restarting the engine` を含まないこと(step 30)を見ます。
✅LP-4b の `view_select_field`(21 step)は、engine の `cube_capture` の feature を project の feature(`project://features/cube_capture.json`)として写した fixture を開きます(`test/fixtures/studio_view_select_field/`。写しは `parameters` を抜いて `$resolution` を 256 に書き下したもので、pipeline は `hybrid_v1`)。Whole frame で `cube_capture_ssao_blur` の node が出て、engine の `view_axes` の face の 2 番のラベルが `+Y` であることを見てから、見出しの double-click で Pass Editor に開き、View の欄(`pelican.fullscreenPass.view.face`)が `All views` であることを見ます。`2 (+Y)` を選んで Save すると、engine の node の `view_select.face` が 2 になり、canvas に `node_view_select#cube_capture_ssao_blur` の 1 行が出て、欄が `2 (+Y)` で読み直されることを見ます。`All views` に戻して Save すると、node から `view_select` が消え(`excludes`)、1 行も消えます。打つ欄はありません([view の軸のシナリオ](../../test/studio_scenarios/view_select_field.json))。
✅LP-4c の `graph_loop_expand`(json の step は 81。✅Q-18 で Frame Plan の Refresh の click を外して 82 から減りました。LP-4c の実装者の報告では 84 step で緑)は、Blank pipeline の project を作り、View ▸ Render Tools ▸ Graph で Graph タブを前に出して **Add loop** を発火します。root に `graph.loop` が立ち、`over.range.count` が 1 で、分岐の body が同じ編集で作られた空の `loop_body` であることを見ます。ループを選んで Loop の欄が出ること、parameters の pill が 0 個であることを確かめ、**Open body** でパンくずが `root / loop_body` になり分岐の combo が出たら、**Add fullscreen pass** で body に image と pass の 2 つを置きます。root に戻って Count に 3 を打って Enter すると、engine の `loops[0]` が `count` 3・要素 3 つ・`tier` `expand` になり、Loop の欄の要約が `expand ×3` を言います。Frame Plan を前に出すと、畳まれたグループ `loop loop ×3` が出ます(✅Q-18 から Refresh は押しません)。Count を 1 にすると要素が 1 つになり、**Add branch** で `element = 0` の分岐が else の前に入ります(✅UX-1 から、その分岐の body は else の body を写した `loop_body_2` で、step はそれを見ます。UX-1 の前は `loop_body` でした)。その body を `(nothing)` にすると、要素 0 の `branch` が 0 になり、else が `unmatched_branches` に入り、表の else の行が `no element takes this branch` を言います。最後に Undo を 3 回押し、分岐の body・分岐の数・回数 3 が順に戻り、engine がまた 3 回展開することを見ます。打つ欄は New Project のフォルダと名前、Count の 2 回です([ループのシナリオ](../../test/studio_scenarios/graph_loop_expand.json))。
✅LP-4e の `material_loop_decompose`(json の step は 46。✅Q-18 で Refresh の click を外して 47 から減りました。LP-4e の実装者の報告では 49 手順で緑)は、root の rendering 節が forward の material pass `forward_opaque` を宣言する fixture を開きます(`test/fixtures/studio_material_loop_decompose/`。箱が 1 つと、平らな色の material `g1_flat`)。viewport の中央と隅の色と色の数を覚えてから、Frame Plan で `forward_opaque` を選んで **Decompose into loop...** を発火し、確認のダイアログが `Undo takes it back` を言うことを見て Decompose を押します(LP-4e の着地時は `cannot be undone yet` を見ていました。RU-1 で変わりました)。root に `forward_opaque_draws` のループと `render.material_pass` ができ、Graph タブが前に出てループが選ばれること、`loops[0].tier` が `draw_loop` で、Loop の欄が Contract に `forward_opaque_v1` を出して Count を出さないこと、viewport の 2 点の色と色の数が分解の前と同じことを確かめます。**Add branch** で最初の material(`pelican.builtin.transparent`)の分岐が入ったら、分岐の material の combo で箱の material を選び(鍵は打ちません。✅UX-3 から、engine の material の一覧の名前 `g1_flat` で選びます。UX-3 の前は鍵 `materials/g1_flat.material.json#g1_flat` の項目でした)、body を `(nothing)` にします。中央が背景の色になって色の数が 1 になり、engine の `loops[0].elements[2]` がその material で `branch` 0、最初の built-in の material が else に移り、表の行がその鍵を言うことを見ます。最後に Frame Plan を前に出して(✅Q-18 から Refresh は押しません)、material pass に `node_draw_loop#forward_opaque` の 1 行があることを見ます。打つ欄はありません([分解のシナリオ](../../test/studio_scenarios/material_loop_decompose.json))。
✅RU-1 / RU-1b の `render_undo_roundtrip`(json の step は 127。実装者の報告では 129 手で緑)は、`studio_material_loop_decompose` を別の名前で写し、`scene_color` を反転して描く `shaders/ru1_invert.frag` を足した fixture を開きます(`test/fixtures/studio_render_undo_roundtrip/`)。まず Frame Plan で `present` を見出しで開き、Pass Editor の fragment の一覧で `project://shaders/ru1_invert` を選んで Save し、viewport の中央の色が変わることを見ます。Frame Plan の toolbar の **Undo** で fragment が `engine://passthrough` に戻り、中央と隅の色が編集の前と同じになり、**Redo** で反転に戻ることを確かめます(最初の Undo をキーでなくボタンで押すのは、この lane では窓が active にならずキーが届かないためです。キーの振り分けは `devstudio_render_undo_keys_test` が見ます)。続けて `forward_opaque` を Decompose into loop で分解し(確認の文が `Undo takes it back` を含むことを見ます)、**Edit ▸ Undo Render Edit** を発火して、root の node が 0 個になり、`forward_opaque` が root の material pass に戻り、Frame Plan に draw_loop の行が無いことを見ます。Redo でループが戻り、Undo でもう一度戻します。次に Add fullscreen pass で `ru1_tint` を足し、`get_render_documents` の文書が 2 つになって `passes/authoring/` に fragment のファイルができたら、Undo で `Undid "Add pass ru1_tint".` が出て、pass が canvas から消え、文書が 1 つに戻り、ファイルも消えることを確かめます。同じ名前でもう一度足せて同じファイルに書かれ、Undo の後の Redo でファイルが戻ります。最後に Pass Editor で `present` の fragment を `engine://passthrough` に戻して Save し、Frame Plan で `present` を見出しで drag して配置を root に書かせてから Undo し、`Undid "Edit pass present".` が出て fragment が反転に戻り、drag の配置が残ることを見ます。打つのは pass 名 `ru1_tint` の 2 回です([Undo のシナリオ](../../test/studio_scenarios/render_undo_roundtrip.json))。✅GL-5c から(json の step は 127 のまま)、`present` の fragment は 2 回とも Pass Editor ではなく、`present` のダブルクリックで開く Graph の詳細欄で選び、`ru1_tint` は View > Pass Editor と Pass Editor の New pass・Refresh・position で足します。Frame Plan の Undo / Redo と `Undid "Edit pass present".` は変わりません。
✅LP-4d1 の `loop_zone_edit`(json の step は 66。実装者の報告では 68 手で緑)は、Blank pipeline の project を作り、Graph タブで **Add loop** を発火して、root に `graph.loop` が立ち body の `loop_body` が空であることを見てから、ループが枠(`graph_zone#loop`)と段の字(`graph_zone_label#loop/stage`)で描かれるのを待ちます。見出しを押してゾーンを選び(Loop の欄が出る)、**Add fullscreen pass** を発火すると、body が image と pass の 2 つを持ち、root はループ 1 つのままで、pass が枠の中に `graph_node#loop_body/render_fullscreen` として描かれ、`graph_node#render_fullscreen` の名前は無いことを見ます。見出しでゾーンを選び直して Count に 2 を打って Enter し、root の `layout` がまだ空のうちに、pass を見出しで枠の中へ drag します。definition の `layout.nodes` に `render_fullscreen` が入り、root の `layout` は空のままであることを見て、definition の配置を覚えます。engine の `loops[0].count` が 2 になり、段の字が `expand ×2` を言い、Frame Plan の Refresh の後にグループ `loop loop ×2` が出ることを見ます。Graph タブに戻って枠を見出しで drag すると、root の `layout.nodes` に `loop` が入って `render_fullscreen` は入らず、definition の配置は覚えた値のままで、pass は枠の中に描かれたままです。最後に Undo(`pelican.graphCanvas.undo`)を 2 回発火し、枠の移動が戻って root の `layout` が空になり(definition の配置はそのまま)、次に pass の移動が戻って definition から `layout` が消えることを見ます。Count を 2 にするのは、Frame Plan が 2 つ以上の回をグループにするためです(実装者の報告)。打つ欄は New Project のフォルダと名前、Count の 1 回です([ゾーンのシナリオ](../../test/studio_scenarios/loop_zone_edit.json))。
✅LP-4d2 の `loop_zone_wire`(json の step は 77。実装者の報告では緑)は、縁をまたぐ 3 本の線を引きます。Blank pipeline の project を作り、Graph タブで root に **Add fullscreen pass**(その image が枠の外の node です)、**Add loop** を発火し、新しいループが root の pass に重なるので **Auto layout** を発火します。ゾーンを選んで body に Add fullscreen pass を足し、Count を 2 にして、engine の `loops[0].count` が 2 になったら root の `nodes` と `definitions` を覚えます。このとき Frame Plan の `barriers` に、`loop/0/render_fullscreen` から `loop/1/render_fullscreen` への `write_after_write`(resource `render_fullscreen__color`)が無いことを見ます。(1) root の image の出力(`graph_socket#render_fullscreen__color/resource`)を、枠の中の body の pass の color 入力(`graph_socket#loop_body/render_fullscreen/in:color`)へ drag します。ループの入力 `resource` が root の image から来て、body に `graph.input` が立ち、pass の `color` がそれを読み、ループの外側のソケット `graph_socket#loop/resource` が描かれることを見ます。2 つの回が同じ image に描くようになるので、上の barrier が出ることも見ます。(2) body の pass の出力(`graph_socket#loop_body/render_fullscreen/out:color`)を右の縁の `graph_zone_socket#loop/out:+` へ drag し、body の `outputs.color` が pass を指し、ループの `outputs[0]` が `color` になることを見ます。(3) 右の縁の内側の端 `graph_zone_socket#loop/out:color` を左の縁の内側の端 `graph_socket#loop_body/resource/value` へ drag し、ループの `carry[0].to` が `resource` になり、矢印 `graph_zone_carry#loop/color/resource` が描かれることを見ます。下端の `graph_zone_label#loop/expansion` が `×2 copies` を言うのを待って **Show in Frame Plan**(`graph_zone_button#loop/frame_plan`)を押し、Frame Plan の status 行が `Loop loop: its copies are picked.` を言うことを見ます。最後に Graph タブへ戻って Undo を 3 回発火し、root の `nodes` と `definitions` が覚えた値と一致することを見ます。このシナリオでは carry の効き目は Frame Plan では見分けられません(上の「分かっている穴」。✅Q-19 の `loop_zone_read` が見ます)。打つ欄は New Project のフォルダと名前、Count の 1 回です([縁をまたぐ線のシナリオ](../../test/studio_scenarios/loop_zone_wire.json))。
✅Q-19 の `loop_zone_read`(json の step は 80。実装者の報告では 82 step で緑)は、線だけで、body の pass に前の回の出力を読ませます。Blank pipeline の project を作り、Graph タブで root に **Add fullscreen pass**、**Add loop**、**Auto layout** を発火し、ゾーンを選んで body に Add fullscreen pass を足して、Count を 2 にします。(1) root の pass の出力 `graph_socket#render_fullscreen/out:color` を左の縁の `graph_zone_socket#loop/in:+` へ drag し、ループの入力 `color` が root の pass から来ることを見ます(pass の出力 port から引くので、回の pass はその pass を待ちます。root の image から body の pass のソケットへ引く形は、Q-19 の着地時は engine が断っていました。✅LP-6 で塞がりました。上の「render node の入力は shader の宣言から」の分かっている穴)。(2) body の pass の出力を右の縁の `+` へ drag してループの出力 `color` を作り、(3) 右の縁の内側の端 `graph_zone_socket#loop/out:color` を左の縁の内側の端 `graph_socket#loop_body/color/value` へ drag して、carry と矢印を作ります。body の pass(`engine://passthrough`)のソケットが可変の帯の端 `graph_socket#loop_body/render_fullscreen/in:variadic` で描かれるのを待ち、frame plan の `loop/1/render_fullscreen` の `reads` に `loop/0/render_fullscreen__color` が無いことを見て、`definitions` を覚えます。(4) 左の縁の内側の端を帯の端へ drag すると、pass に port `color_2`(`binding` `input`・`input_index` 0。pass 自身の `color` と重なるので `_2`)ができて body の `graph.input` の `color` から読み、`in@0` の ○ が描かれ、`loop/1/render_fullscreen` の `reads` に `loop/0/render_fullscreen__color` が入ることを見ます。次にゾーンを選んで **Rename definition…** を発火し、名前の欄が `loop_body` で開くのを見て `stroke` を打って Enter すると、`definitions.stroke` ができ、ループの `branches[0].body` が `stroke` になります。Undo で `loop_body` に戻り、もう 1 回の Undo で `definitions` が覚えた値と一致することを見ます。打つ欄は New Project のフォルダと名前、Count の 1 回、definition の新しい名前の 1 回です([読みの線のシナリオ](../../test/studio_scenarios/loop_zone_read.json))。
✅LP-4f1 / LP-4f2 の `loop_view_axis`(json の step は 53。✅Q-18 で Refresh の click を外して 54 から減りました)は、`studio_view_select_field` と同じ文書(`hybrid_v1` と、project の feature に写した `cube_capture`)で root の graph が空の fixture を開きます(`test/fixtures/studio_loop_view_axis/`)。Graph タブで **Add loop** を発火し、ゾーンを見出しで選んで Loop の欄の要約が `expand ×1` を言うのを待ってから、Over で `View axis`、family で `$capture/cube` を選びます。文書の `over.view_axis` が `$capture/cube` / `face` になり、回数の字が `×6 (face)` になることを見ます。ゾーンを選び直して **Add fullscreen pass** を発火すると、body の image が `width` 256・`height` 256・`layers` 6 で作られ、pass の `view_family` が `$capture/cube` であることを見ます(✅LP-4f2。LP-4f1 の着地時は、画面の大きさの pass が engine を止めたため fixture の `cube_face` の definition を body にしていました)。pass を見出しで選ぶと View 欄の `face` が `All views` で出るので(✅Q-18 から、この `wait` は欄が有効になるのも待ちます)、**Each element (loop)** を選び、pass の `view_select.face` が `{"$expr": "element"}` になることを見ます。engine の `loops[0].count` が 6・`tier` が `expand` になり、frame plan の `loop/0/render_fullscreen` 〜 `loop/5/render_fullscreen` がそれぞれ `view_select.face` 0〜5 を持ち、ゾーンの段の字が `×6 (face)`、3 つ目の軸のラベル(`graph_zone_label#loop/axis2`)が `+Y` を言うことを確かめます。最後に Frame Plan を前に出して(✅Q-18 から Refresh は押しません)、グループ `loop loop ×6` が出ることを見ます。打つ欄はありません([view の軸のループのシナリオ](../../test/studio_scenarios/loop_view_axis.json))。
✅LP-4f2 の `graph_template_bloom`(json の step は 46)は、`studio_automation_graph` の rendering 文書(direct root の features に bloom)に、engine の OpenPBR の発光する material で光らせた箱を置いた fixture を開きます(`test/fixtures/studio_graph_template_bloom/`)。viewport の中央と隅の色と色の数を覚えてから、Render Features で `bloom` の行を選び、**Replace with loop version…** が出て押せることを見て押します。確認のダイアログが `Bloom loop`・`The feature bloom is removed`・`Undo takes it back` を含むことを見て Replace を押すと、root の features が 0 本になり、graph に `graph.loop` の `bloom_down` が入り、Graph タブが前に出てそのゾーンが描かれ、Loop の欄の要約が `bloom_down` を言い、Render Features の notice が `Replaced bloom with Bloom loop. Undo takes it back.` であることを見ます。engine の `loops[0].node` が `bloom_down` で、viewport の中央・隅の色と色の数が置き換える前と同じことを確かめ、Frame Plan を Refresh してループのグループを見ます。**Edit ▸ Undo Render Edit** で bloom が features に戻り graph が 2 node に戻ること、**Redo** でループが戻ることを見て、どちらでも絵が置き換える前と同じことを確かめます(実装者の報告では中央 `#ffffff`・隅 `#000000`・色数 1016)。打つ欄はありません([分解版のシナリオ](../../test/studio_scenarios/graph_template_bloom.json))。
✅EW-2 の `engine_window_embedded`(json の step は 12)は、engine の窓が Studio の外に一度も出ないことを見ます。シナリオが `"watch_engine_windows": true` を持つと、driver は Studio を起動した時から監視の thread を回し([`EngineWindowWatch`](../../tools/studio_drive.py#L271))、Studio を親に持つ process の top-level の窓を列挙して、見える窓を記録します(実装者の報告では約 25 ms ごと)。property 付きで owner の無い非表示の窓(読み込み中の engine の窓)を見た回数も engine ごとに数えます。expectation `engine_windows`([判定](../../tools/studio_drive.py#L1133))は、見える窓が 1 つも無いこと、`engines` 本以上の engine について非表示の窓を `min_hidden_samples`(既定 3)回以上見たこと(監視が読み込みを見逃したまま緑にならないように)、最新の engine の窓が見えていて、その root の窓が Studio の process のものであることを要求します。シナリオは project と一緒に起動した engine、Stop Engine → Restart Engine の engine、別の project(`{other_project}`)への切替の engine の 3 回でこれを見て、最後に `screen` モードで viewport を撮ります。実装者の報告では緑(約 19 秒)で、`--embedded` を外す変異では窓が 144 sample 見えて赤でした。⚠ 監視の間隔より短い表示は取り逃がしえます。撮った viewport は黒でした(それが正しい絵かは未確認)。打つ欄はありません([窓のシナリオ](../../test/studio_scenarios/engine_window_embedded.json))。
✅PM-3a の `play_pause_step`(json の step は 35。✅PM-3b で edit 起動に合わせて直しました)は、behavior `pm3a_mover`(game の 1 歩ごとに object の x を 0.25 進める。`test/fixtures/pm3a_mover/`)を付けた object `Mover` の fixture を開きます(`test/fixtures/studio_play_pause_step/`)。behavior は game logic の DLL なので、`test/CMakeLists.txt` に `pelican_studio_<シナリオ名>_game_logic` の target があれば、test がその DLL を run の directory に写し、Studio は `PELICAN_STUDIO_PLAYER_ARGUMENTS` の `--game-logic` で engine に渡します([写す所](../../test/run_studio_scenario.cmake#L47))。起動の直後は status bar の行が `Edit` で Pause が押せないことを見てから、**Play** を押します。行が `Playing · frame` を言い、Step が押せず、`get_components` の `Mover` の x が増えること、viewport の枠の左端が paused の色でないことを見ます。Pause を押すと行が `Paused · frame N` になり、N が `get_status` の `play.game_frame` と一致します(比べる文は、覚えた値から `value_format` で作ります)。評価の番号(`get_status.frame`)が 30 以上進んでも x と `game_frame` が変わらないこと、枠の左端が `#e0ac00` であることを見ます。Step を押すと `game_frame` がちょうど 1、x がちょうど 0.25 進み(`plus`)、行もその frame を言います。Pause をもう一度押す(Resume)と行が `Playing` に戻り、x がまた増え、枠の左端が `#e0ac00` でなくなります。最後に Stop を押し、行が `Edit` に戻って x が 0 に戻ることを見ます。打つ欄はありません([一時停止のシナリオ](../../test/studio_scenarios/play_pause_step.json))。

✅PM-3b の `play_mode_roundtrip`(PM-3b の着地時の json の step は 97。PM-3d で 118)は、`studio_render_undo_roundtrip` の project(Pass Editor で `present` を `shaders/ru1_invert.frag` に替えると背景が反転する)に `pm3a_mover` の `Mover` と directional light の `Sun` を足した fixture を開きます(`test/fixtures/studio_play_mode_roundtrip/`)。PM-3b の着地時は model を描きませんでした(PM-2b の前の穴)。PM-3d で箱と camera を足しています。起動が `Edit`・`play.session` が false・Play が押せて Pause が押せず Save Scene が押せること、評価の番号が 30 進んでも `Mover` の x が変わらないことを見ます。(1) Play で x が増え、`session` が true、Save Scene が押せず、render-tools の帯が出ること、Stop で x が起動時の値に戻り、Save Scene が押せて帯が消えることを見ます。(2) Play して Inspector で `Sun` の `intensity` を 7 にすると、Inspector の行が `reverts on Stop` を言い、`recorded_edits` が 1 件になり、Keep を付けずに Stop すると `authored_json.intensity` が 3 に戻り、Inspector の欄も `3.00` になります。(3) もう一度 Play して 5 にし、Play changes を開いて最初の行が `Edit light.intensity` であることを見て check を付けて Stop すると、5 が残り、要約が `Stopped: 0 reverted, 1 kept.` です。(4) Play して `Mover` を選び、x が 2 を超えてから Take from game を押すと、行が `Take Transform from game committed` を言い、`authored_json.pos[0]` が 2 を超え、`recorded_edits` が 1 件です。Stop の後、authoring と edit の runtime の x がその値のままです。(5) Play して Frame Plan から `present` を Pass Editor で開き、Pass Editor の帯が `Changes here are written to files and stay after Stop.` を言うことを見て、fragment を `project://shaders/ru1_invert` にして Save します。Stop の後も `get_render_authoring_context` の fragment がそのままで、viewport の隅の色が起動時と違うことを見ます。最後に edit で File ▸ Save Scene を発火し、`Scene saved` を待ちます。打つ欄は `intensity` の 2 回です([再生と停止のシナリオ](../../test/studio_scenarios/play_mode_roundtrip.json))。

✅PM-3b の `play_view_camera` は ✅PM-3d で `play_mode_roundtrip` にまとめ、消しました。今の `play_mode_roundtrip`(json の step は 118。✅GL-5c で 117。再生中の `present` の fragment は Pass Editor ではなく Graph の詳細欄で選び、帯は render tools の `renderBanner.renderTools` を見ます)の fixture は、箱(`CheckerBox`)と、`pm3a_mover`(`step` 0.05)で動く camera も持ち、edit では editor camera で描くこと・View: Game | Editor が edit と paused で出て playing では出ないこと・paused で Editor に切り替えると絵が game の camera と違うこと・Stop で editor camera に戻ること、そして model のある scene で Keep 付きの Stop の後に 30 フレーム進んでも engine が箱を描き続け、Keep した `intensity` が残ることを見ます(✅PM-2b の前の engine では、この Stop の直後に player が応答しなくなって赤)。

✅PM-3c の `play_runtime_objects`(json の step は 33)は、最初の歩で prefab `pm3c_shot` から `Shot` を (1.5, 2, -0.5) に spawn する behavior `pm3c_spawner`(`test/fixtures/pm3c_spawner/`)を付けた object `Spawner` の fixture を開きます(`test/fixtures/studio_play_runtime_objects/`)。edit では `get_runtime_objects` の `objects` が空で、Outliner に `pelican.outliner::row#Shot (runtime)` が無いことを見ます。Play すると engine が `Shot`(`spawned_by.prefab` が `pm3c_shot`)を 1 つ返し、Outliner に `Shot (runtime)` の行が出て、`Spawner` の行も残ります。その行を選ぶと、Inspector の選択の行が `Shot (runtime)` と `spawned from pm3c_shot` を言い、`pelican.inspectorRuntime.position` と `.worldPosition` が `1.5, 2, -0.5` で、欄(`pelican.inspectorProperties`)と Add component が見えず、Outliner の Delete と Duplicate が押せません。Stop で `objects` が空になり、行が消え、Inspector の選択が `None` になります。もう一度 Play すると行が戻り、最後に Stop して行が消えることを見ます。打つ欄はありません([runtime の行のシナリオ](../../test/studio_scenarios/play_runtime_objects.json))。

✅GL-1 の `graph_folded_features`(json の step は 29。実装者の報告では 31 手で緑)は、`studio_material_loop_decompose` を写した fixture を開きます(`test/fixtures/studio_graph_folded_features/`)。root の graph は node を持たず、rendering 節が forward の material pass `forward_opaque` と、`scene_color` を `engine://passthrough` で見せる `present` を宣言します。`get_graph_instances` の node が 0 個であることを見てから、Graph タブの root に `graph_node#forward_opaque` と `graph_node#present` の畳んだ node が出て、その間に線 `graph_pass_input_edge#forward_opaque->present` が引かれ(✅GL-1e から。`present` の `input` が `scene_color` を名指すので、派生の破線ではなく実線です。json の 7 番目の step で、統合側の報告の数え方では step 8。GL-1 の着地時は破線 `graph_derived_edge#forward_opaque->present` を待っていました)、`Show:` の Render の check(`pelican.graphCanvasShow.render`)が見えることを確かめます。`forward_opaque` をダブルクリックすると、パンくずが `root / forward_opaque` になり、ゾーン `graph_zone#forward_opaque_draws` と body の `graph_node#forward_opaque_draw/step`(Studio が `draw.material` として組む node)が出ます。中で **Add loop** を発火すると、`pelican.graphCanvasFoldedNotice` が `This is the engine's folded implementation: expand it to edit.` と `Expand to edit` を言い、文書の node は 0 個のままです。パンくずを押して `root` に戻り、畳んだ node が戻って 1 行が消えることを見ます。最後に Frame Plan で `forward_opaque` を選んで **Open in Graph** を発火し、Graph タブの要約の行が `forward_opaque (material)` を言い、node が見え、文書の node が 0 個のままであることを見ます。root と中の絵を 1 枚ずつ撮ります(`folded_root.png` / `folded_inside.png`)。打つ欄はありません([畳んだ node のシナリオ](../../test/studio_scenarios/graph_folded_features.json))。

✅GL-2b の `graph_layers_origin`(json の step は 30。GL-2b の着地時は 31 で、✅GL-1b でループの帯の `×3 copies` を待つ 1 手を外しました)は、engine の `hybrid_v1` の preset と、`shadow_directional` の feature の `shadow_depth` を graph のループ `cascades` で置き換える(`pass_replacements`)fixture を開きます(`test/fixtures/studio_graph_layers_origin/`)。root には、`tint_image` に描く fullscreen pass `tint` と、それを入力 0 に読んで `show_image` に描く `show`、definition `paint` の instance `left`、影の cascade を 3 回回すループ `cascades`(要素 0 は body `near`、他は else の body `cascade`)があります。engine の `nodes[2].origin.loop` が `cascades` であることを見てから、Graph タブでゾーン `graph_zone#cascades` の下の帯 `graph_layer_band#cascades` が `barriers` を言うこと、preset の pass `deferred_geometry` が畳んだ node で出ること、`pelican.graphCanvasDiagnostics` が `diagnostics` を言うこと、何も選ばない Layers の見出しが `Layers: the whole graph` であること、ゾーンが要素 0 の body の `graph_node#near/depth` を描くことを見ます。Frame Plan の Barriers(`pelican.framePlan.tab.barriers`)で `tint` から `show` への barrier 20 の行を選び、`pelican.framePlan.openRowInGraph` を発火すると、Graph タブの Layers の見出しが `Layers: tint` になり、`L2 physical` の組が出ます。次に `cascades/2/depth` からの barrier 9 の行で同じことをすると、ゾーンのタブが else に移り、`graph_node#cascade/depth` が見えて `graph_node#near/depth` が消えます。最後に文書の root の node が 7 個のままであることを見ます。絵を 3 枚撮ります(`layers_root.png` / `layers_tint.png` / `layers_copy.png`)。打つ欄はありません([層のシナリオ](../../test/studio_scenarios/graph_layers_origin.json))。

✅GL-1d の `graph_from_blank`(json の step は 48。GL-1d の着地時は 35 で、実装者の報告では 37 手で緑)は、Graph タブだけで Blank の project に描画の pass を組みます。File > New Project... で Blank pipeline の project を作り、Frame Plan に `present` が出て engine が frame を出すのを待ってから、Graph タブで文書の node が 0 個であることを見ます。**Add** のボタン(`pelican.graphCanvasAddNode`)を click して palette を開き、絞り込みの欄に `material_pass` を打ち、Contract の combo で `deferred_geometry_v1` を選んで、絞り込みの欄で Enter を押します(Enter が OK。上の「palette はキーボードだけで使えます」)。root にゾーン `graph_zone#render_material_pass_draws` が描かれ(engine が再起動を求めれば、その後に)、engine が frame を出し、その横に `graph_node#render_material_pass`、ゾーンの中に `graph_node#render_material_pass_draw/step` が出ることを待ちます。文書では、`nodes[0]` がループ `render_material_pass_draws` で `over.draw_items.contract` が `deferred_geometry_v1`、分岐 0 の body が `render_material_pass_draw`、その definition の最初の node が `draw.material`、`nodes[7]`(間に image 6 枚)が `render.material_pass` で `inputs.draws` がループから来ることを見ます。engine の `loops[0].tier` が `draw_loop` であることを見て、Graph タブを撮り(`graph_from_blank.png`)、Frame Plan を前に出して `node#render_material_pass` が出ることを見て撮ります(`graph_from_blank_frame_plan.png`)。打つ欄は New Project のフォルダと名前、palette の絞り込みの 1 回です([Blank から組むシナリオ](../../test/studio_scenarios/graph_from_blank.json))。⚠ このシナリオは、engine が起動し直したかどうかを見ていません(統合側の報告)。✅UX-11 から、Graph タブを撮った後、上の Frame Plan の確かめの前に、material pass を選び、Pass Editor と同じ Attachments の行(`pelican.graphCanvasMaterial.depthLoad`)と Tags の picker(`pelican.graphCanvasMaterial.materialFilter.include`)が出ること、albedo の Load(`pelican.graphCanvasMaterial.outputLoad.0`)を `Clear` にすると文書の `nodes[7]` の `color_load_op` が `clear` になり、`material_contract` は `deferred_geometry_v1` のままであることを見ます。30 frame の後に Materials drawn(`pelican.graphCanvasMaterial.drawn`)が出ることを見て撮り(`graph_from_blank_material_fields.png`)、その最初の行を選ぶと Frame Plan の Materials のタブ(`pelican.framePlanMaterials`)が出ることを見ます。そこから Logical graph のタブに戻して、上の Frame Plan の確かめに続きます。

✅GL-1d の `graph_from_blank_forward`(json の step は 69。GL-1d で統合側が 35 で足し、✅GL-1e で 51 に、✅UX-2 で 69 になりました。GL-1e の統合側の報告では 53 手で緑)は、`graph_from_blank` と同じ手順を、palette の既定の契約で利用者が Blank から最初に試した `forward_opaque_v1` で走らせます。Blank の project は material を deferred の route で描くので、Studio は同じ編集に `material_route_default: "forward"` を付け(上の「forward の route の既定を一緒に送ります」)、engine は commit して再起動を求めます。文書の `over.draw_items.contract` が `forward_opaque_v1`、pass が `nodes[3]`(間に image 2 枚)であるほかは、見るものは `graph_from_blank` と同じです。統合側の報告では、走らせた後の project の rendering 文書に `material_route_default: "forward"` があり、engine の log に `LOG_ERROR` はありませんでした。この 2 つはシナリオの step では見ていません。
✅GL-1e から、Frame Plan を撮った後に次を続けます(上の「Graph から画面に出す(✅GL-1e)」)。Outliner の Add Object の Cube(`pelican.outliner.addObject.cube`)で箱を 1 つ置き、engine の `scene_tree` の 3 つ目の object が `Cube` になるのを待ち、File > Save Scene で保存します(Studio を閉じるときに問われないように)。Outliner で `MainCamera` を選び(箱の上に gizmo を描かないように)、Graph タブを前に出して Fit view を発火し、`present` に空いた口 `graph_derived_socket#present/in:+` が出るのを待ちます。viewport の中央の画素を覚えてから、material pass の `color` の出力(`graph_socket#render_material_pass/out:color`。✅GL-1f から。GL-1e の着地時は image node `render_material_pass__color` の `resource` で、線は `graph_pass_input_edge#render_material_pass__color->present` でした)を `present` の `+ input` へ drag します。notice が `Pass inputs saved.` を言い、実線 `graph_pass_input_edge#render_material_pass->present` が出て、viewport の中央の画素が覚えた色と違うことを見ます(統合側の実測では、passthrough のグラデーションの `#bcbcbc` から pass が描いた `#746e62` へ)。最後に Graph タブを撮ります(`graph_from_blank_present_graph.png`)。viewport の絵は `graph_from_blank_present.png` です。打つ欄は `graph_from_blank` と同じで、増えていません([forward のシナリオ](../../test/studio_scenarios/graph_from_blank_forward.json))。2 本とも、統合側が仮想モニタ(`PELICAN_STUDIO_SCREEN=1`)で緑を確かめました。✅GL-1f の着地でも、`graph_from_blank_forward` / `graph_from_blank` / `graph_folded_features` / `loop_zone_read` / `material_loop_decompose` が仮想モニタで緑でした(統合側の報告)。
✅UX-2 から、Graph タブを撮った後に次を続けます(json の 52〜69 番目。上の「render.image の値を選んで決める(✅UX-2)」)。**Add** で palette を開き、絞り込みに `render.fullscreen` を打って Enter で足し、文書の `nodes[4]` が `render_fullscreen__color` で format が `R16G16B16A16_SFLOAT`(✅UX-6a からの Add fullscreen pass の image の format)であることを見ます。Fit view して image の node を click で選び、image の欄に engine の usage の flag の check(`pelican.graphCanvasImage.usage.STORAGE`)が出る(vocabulary が届いた)のを待ってから、Format の combo(`pelican.graphCanvasImage.format`)で `R8G8B8A8_UNORM` を選びます(打ちません)。engine が commit して `nodes[4].parameters.format` がその値になり、usage(`usage[1]` の `SAMPLED`)がそのまま残り、notice が `Graph change saved.` を言い、欄が文書を読み直してその値を出すことを見て、Graph タブを撮ります(`graph_from_blank_image_fields.png`)。打つ欄は palette の絞り込みが 1 回増えました。UX-2 の着地時は逆向き(`R8G8B8A8_UNORM` から `R16G16B16A16_SFLOAT` へ)で、欄が出さない鍵として `usage[2]` の `TRANSFER_SRC` が残ることを見ていました。✅UX-6a で image の既定が替わったので、向きと見る鍵を直しました(json の step の数は 69 のまま)。

✅UX-3 の `graph_palette_kinds`(json の step は 61)は、Blank pipeline の project で、palette が種を入れて置く render node を 3 つ置きます(上の「palette で置ける render node」)。Frame Plan の **New render target...** で名前だけ `ux3_scene` を打って宣言し、engine の `render_targets[0].name` がそれになるのを見ます。Graph タブの **Add** で palette を開いて `target_ref` を打つと、宣言済みの target の combo(`pelican.graphCanvasPaletteTarget`)が出るので `ux3_scene` を選んで Enter を押します。canvas に `ux3_scene_ref` が出て、文書の `parameters.name` が `ux3_scene`、型が宣言から読んだ `pelican.render.color_signal@1` であることを見ます。次に `render.raster` を置き、`draw.vertex_count` が 3、fragment が `engine://passthrough`、`color` が一緒に来た image `render_raster__color` から来ることと、Frame Plan に `render_raster` が出ることを見ます。最後に `snapshot_copy` を置き、`snapshot_after` の combo(`pelican.graphCanvasPaletteSnapshotAfter`)で `render_raster` を選びます。copy の `snapshot_after` が `render_raster`、`source` が raster pass から来て、一緒に来た image の usage の先頭が `TRANSFER_DST` であること、Frame Plan に `render_snapshot_copy` が出ることを見ます。Graph タブと Frame Plan を 1 枚ずつ撮ります(`graph_palette_kinds.png` / `graph_palette_kinds_frame_plan.png`)。打つ欄は New Project のフォルダと名前、target の名前、palette の絞り込みの 3 回です([palette の種のシナリオ](../../test/studio_scenarios/graph_palette_kinds.json))。

✅RSX の `rendering_settings_from_blank`(json の step は 30)は、Blank pipeline の project で Rendering settings のタブを使います(上の「Studio の Rendering settings タブ」)。既定 layout に戻してから project を作り、Frame Plan の Undo が押せない(履歴が空)ことを見ます。**View ▸ Render Tools ▸ Rendering settings** で前に出し、`get_render_documents` の root に `RSX_TINT` が無いことを見てから、**Add**(`pelican.renderingSettings.shaderDefines.add`)で行を開き、editor に `RSX_TINT` を打って Return を押します。notice が `Saved shader_defines.` を言い、root の文書が `RSX_TINT` を持ち、engine が描き続けることを見て、タブを撮ります(`rendering_settings_from_blank.png`)。Frame Plan の Undo が押せるようになったら **Edit ▸ Undo Render Edit** を発火し、status 行が `Undid "Set shader_defines".` を言い、root から `RSX_TINT` が消え、タブが読み直して編集できることを見て、もう 1 枚撮ります。打つ欄は New Project のフォルダと名前、define の名前の 1 回です([Rendering settings のシナリオ](../../test/studio_scenarios/rendering_settings_from_blank.json))。✅UX-15a で後ろに延びました(json の step は 30 → 50)。Undo の後の root が `render_strategy` を持たないことを見てから、Render strategy の combo で `builtin.authored_config_v1` を選び、notice の `Saved render_strategy.`、root が `render_strategy` を持つこと、name の欄が root から読み戻した `builtin.authored_config_v1` であること(打っていない)、engine が描き続けることを見ます。次に Graph transforms の **+** を押し、`Saved graph_transforms.`、root が `graph_transforms` を持つこと、1 行目の name が `builtin.identity_v1` であること、engine が描き続けることを見て撮ります(`rendering_settings_from_blank_transforms.png`)。最後に **Edit ▸ Undo Render Edit** を 2 回発火し、`Undid "Set graph_transforms".` で root から `graph_transforms` と行が消え、`Undid "Set render_strategy".` で `render_strategy` が消え、engine が描き続けることを見ます。延ばした分に打つ欄はありません。実装者の run では緑で、手順の数は 52 と報告されています(run の数え方は json の step の数と合いません)。

✅GB-1 の `gui_build_A`〜`gui_build_E` の 5 本は、File > New Project > Blank pipeline から、利用者がマウスとキーボードでできることだけで描画を組みます(`rpc` は読むだけ)。A と B は最後まで組み、C・D・E は今の GUI と engine で行ける所まで進んで、止まる所を json の description に書いています。commit の記録では、止まる所は engine の RS-6〜RS-11 と GB-1 の後の Studio の WP が受けます(どの止まりがどの WP かは、この文書では未確認です)。

- **`gui_build_A`(forward。json の step は 41)**: Graph タブの palette で `forward_opaque_v1` の material pass を足し(route の既定のための再起動の後にゾーンが描かれるのを待ちます)、Add Object の Cube を置いて Save Scene し、`MainCamera` を選んでから、material pass の `color` の出力を `present` の `+ input` へ drag します。notice が `Pass inputs saved.` を言い、実線が出て、viewport の中央が前のグラデーションの色とも隅の色とも違う(箱が描かれた)ことを見ます。打つ欄は New Project のフォルダと名前、palette の絞り込みです([A](../../test/studio_scenarios/gui_build_A.json))。
- **`gui_build_B`(deferred と lighting。json の step は 58)**: 通る道は Frame Plan です。Cube を置いて保存し、**Add material pass...** の Deferred geometry(枠は開いた時点で埋まっています)を Save し、Whole frame で `deferred_geometry` を見出しで選んで **Add fullscreen pass** を発火します。New... で Deferred lighting を選ぶと入力 5 つと Camera position が入り、color 出力を一覧の `New target: deferred_lighting_color...` で作って Create と Save をします。engine が lighting の pass を geometry の後に走らせ、`uses_light_data` が真であることを見てから、Frame Plan の canvas で lighting の出力を `present` の可変の帯の端へ drag し、root の `present` の入力が `deferred_lighting_color` になることを見ます。viewport が 64 色以下で 2 色以上、中央が隅と違う色である(光で照らされた箱)ことを見ます。打つ欄は New Project のフォルダと名前 `gb_b` です。Graph タブの palette で deferred の material pass を足す道は止まります: その後に Pass Editor で足した pass は `present` の後ろに入り、`present` は動かせません(`render_pipeline_preflight_failed`。description の記録)([B](../../test/studio_scenarios/gui_build_B.json))。
- **`gui_build_C`(bloom のループ。json の step は 83)**: A と同じく forward の material pass と Cube を置き、root に **Add fullscreen pass** で明るい所を取り出す pass(`engine://passthrough` のまま。threshold の shader を選ぶ道が無いため)を足して、material pass の `color` をその可変の帯の端へ drag します。**Add loop**、body に Add fullscreen pass、Count に 4、明るい pass の出力を body の pass の帯の端へ(ループの入力になります)、body の出力を右の縁の `+` へ、右の縁の内側の端を左の縁の内側の端へ(carry)と線を引き、body の image の `extent_scale` の帯に `0.5` を打ちます。engine がループを 4 回展開し、`loop/1/render_fullscreen` が `loop/0/render_fullscreen__color` を読む(carry)ことを見て、Graph タブを撮ります。ここで止まります(description の記録): `extent_scale` を回ごと(`1 / pow2(element + 1)`)に帯からは書けない。body の pass の shader の一覧(UX-2)に engine の bloom の shader が無い(engine は ✅EL-4 から `get_graph_catalog` の `engine_shaders` に一覧を出します —— §13.5 の「material pass の format と engine の shader も、catalog から読めます(✅EL-4)」。Studio の一覧はまだそれを読みません)。engine の `bloom_highpass` は engine が予約する `PELICAN_FEATURE_*` の define を要る。合成には buffer の port と加算の blend が要るが、それを出す欄が無い。ループの出力を `present` の `+ input` に落とすと、`present` がループの入力の image を読んでしまう。打つ欄は New Project のフォルダと名前、palette の絞り込み、Count の 4、`0.5` です([C](../../test/studio_scenarios/gui_build_C.json))。
  → この段落は GB-1 の着地時の形です。今の json は 164 step で、description は bloom の shader・`present` への loop の出力・合成の pass・回ごとの `extent_scale` と、bloom が箱の外へ広がることまでを書いています(この文書では段ごとの追記はしていません)。
  ✅PX-1(`e3627606`)で、広がりを読む点を箱の上下(真ん中の列の高さ 0.25 / 0.75)だけにしました(下の「画素を読む点と窓の大きさ(✅PX-1)」)。
  → ✅PX-2(`28bc5ef9`)で viewport が 720x450 に固定され、上下の点を高さ 0.243 / 0.756 に選び直しました(6 step。下の「viewport の大きさを決める(✅PX-2)」)。
- **`gui_build_D`(影。json の step は 99。✅UX-12c で 85 から延びました)**: json の description の今の形です。B と同じく Graph タブの palette で `deferred_geometry_v1` の material pass を足し、Add Object の Cube と Floor(箱の下の板)を置き、Uses scene lights を付けた Add fullscreen pass で lighting の pass を足します。Features で `shadow_directional` を catalog から選んで Add・Apply すると、RS-10 から deferred の pipeline でも通り、frame plan が `shadow_map` を最初に描きます。Graph タブに戻って lighting の pass の New… > Deferred lighting > Create(UX-5b)で影の部分を持つ雛形を書き、その色を `present` の `+ input` へ引いて、床の箱の左側の点が右側の点と違う色である(影が落ちている)ことを viewport の画素で見ます。✅UX-12c から続けて、`get_frame_plan` の material pass の `drawn_materials` の 2 件目が `pelican.primitive.default` であること、material pass の欄の Materials drawn の見出しが `as of frame` を言い、行が `pelican.primitive.default (material 1)` と `(material 2)` の 2 つで、組み込みの transparent の行(`pelican.builtin.transparent (built in)`)が無いことを見ます。**Add binding** の行の名前を engine の一覧から `linear_view_depth` に選び、欄を撮ってから、何も読まないその行(送られていない)を削除します。影の depth の pass は feature(`shadow_directional`)のもので、palette の `render.shadow_depth` から手で組んではいません。打つ欄は New Project のフォルダと名前 `gb_d`、palette の絞り込みの `material_pass` です([D](../../test/studio_scenarios/gui_build_D.json))。GB-1 の着地時の形(73 step。forward の pass、Add loop と View axis、Replace with loop version…)は、✅UX-5c / UX-5d(`fe276fc4`)で今の deferred の形(85 step)に替わりました(json の履歴)。
- **`gui_build_E`(RT の mask。json の step は 69)**: json の description の今の形です。Graph タブの palette で `deferred_geometry_v1` の material pass を足し、Add Object の Cube と Floor を置いて保存します。palette の `render.compute` の行で engine の shader の一覧を読ませ(行を選んだときだけ `get_engine_shaders` を聞く。✅UX-5d)、`engine://rt_shadow_mask_pipeline  (ray tracing)` を選んで置くと、task は raygen / miss / closesthit を持ち、shader が宣言する ports で届きます(`world_position` と `normal` は material pass の `worldpos` と `normal` から名前で、`shadow_mask` は隣の自前の image(`R8_UNORM`。✅EL-4c で engine が shader から読む format)に、`dispatch.rays_from` はその image)。task の `shadow_mask` を `present` の `+ input` へ落とすと、frame plan が trace を material pass の後・`present` の前に走らせ(✅RS-6)、viewport の中央がグラデーションと違い、箱の左の床(影の中)が右の明るい床と違う値であることを見ます。ray tracing の pipeline を持つ device が要ります。打つ欄は New Project のフォルダと名前 `gb_e`、palette の絞り込みの `material_pass` と `render.compute` です([E](../../test/studio_scenarios/gui_build_E.json))。CT-6s の実装者の run でも緑でした。
  - GB-1 の着地時の形(40 step)は、Frame Plan の Deferred geometry と Features の `rt_shadow_mask_pipeline` の Add・Apply で mask を書かせ、`present` の手前で止まっていました。その時点で止まった理由の記録は、mask を `present` の `+ input` へ落とすと `render_pipeline_preflight_failed`、Graph タブの palette が `render.compute` を出さない(project の `shaders/*.comp` があるときだけ出していて、Blank の雛形は `shaders/` に何も書かない)、Tasks > New ray tracing が無い raygen / miss / hit の shader を名指す、の 3 つです。1 つ目は ✅RS-6 / RS-6b で、2 つ目は ✅UX-5d / EL-5s で外れ、今の形になりました(json の履歴 `fe276fc4`)。3 つ目は ✅CT-6s で外れました(Task Editor の草稿が雛形を書く)。Blank から project の RT の shader を書いて組む道は、別のシナリオ `gui_build_rt_from_blank` です(下)。

✅GB-2 の `gui_build_full` と `gui_build_optimize` は、GB-1 が 5 本に分けて組んだものを 1 つの project に組み、次にそれを今の GUI にある方法で速くして測ります。どちらも File > New Project > Blank pipeline から、Studio の GUI だけで進みます(`rpc` は読むだけ)。止まった所と打った欄は json の description に書いてあり、✅UX-6a(`1d8644fc`)がそのいくつかを外しました。この文書では 2 本を走らせた結果を確かめていません(未確認)。

- **`gui_build_full`(組む。json の step は 284。GB-2 の着地時は 314、UX-6a で 282)**: 段 1 で Graph タブの palette から `deferred_geometry_v1` の material pass(draw ループごと)、Add Object の Cube と Floor、Inspector で KeyLight の強さ 6、Uses scene lights を付けた Add fullscreen pass。段 2 で Features の `shadow_directional` と、lighting の pass の New… > Deferred lighting > Create。その色を `present` の `+ input` に落とし、照らされた箱と床の影を viewport の画素で見ます。段 3 で palette の `render.compute` の行から `engine://rt_shadow_mask_pipeline` を置き、その mask を lighting の node の binding 6 に落とすと、床の箱の横が mask の硬い影になります。段 4 で bloom(`engine://bloom_highpass` → Add loop の 4 回のループと carry、body の image を回ごとに半分 → `engine://bloom_composite`)を `present` に。段 5 で New… > Tone map > Create の pass を `present` に落とし、光で白く飛んだ床に階調が戻ります。最後に plan の順(trace が lighting の前、lighting が shadow map と mask を読む、4 回の複製と carry、composite、tone map、present)を見ます。
  - ✅UX-6a で変わったこと(description の記録): lighting の雛形が `shadow_mask` を宣言するので shader を手で書き換える 2 か所が無くなり、Frame Plan のタブへ回る道と Graph タブの Refresh も無くなりました。Add fullscreen pass の image が float で来るので Format を選び直す手が無くなり、2 回目の New… の名前の欄が `tone_map` を言うので打ち直しも無くなりました。各操作の前の約 2.5 秒の待ちも外しました。代わりに段 5 で、Graph タブを前にしたまま `shaders/tone_map.frag` を外から書き換え、絵が変わることと、tone map の node に 2 つ目の宣言の入力(binding 1)が Refresh なしに出ることを見ます。
  - ✅UX-7 で足したこと: 段 4 の bloom の前と後で画面の上端の中央(高さ 1.2 %)の画素を読み、同じであること —— fullscreen の pass の番号付きの入力が `clamp_to_edge` で書かれるので、明るい床が上端に回り込まないこと(上の「pass の欄の Inputs」の UX-7)を見ます。
  - 打つ欄は、New Project のフォルダと名前 `gb_full`、palette の絞り込み(`material_pass` と `render.compute`)、光の強さの 6、Count の 4、tone map の shader の 3 か所の書き換え(UX-6a の決定 2 を見るためのもの)です([全部を組むシナリオ](../../test/studio_scenarios/gui_build_full.json))。
- **`gui_build_optimize`(速くして測る。json の step は 488。GB-2 の着地時は 511、UX-6a で 463)**: `gui_build_full` と同じ手順で組んでから、GUI にある速くする方法を 1 つずつ当て、同じ物差しで読みます —— `get_status` の `gpu_timing`(engine の 120 frame の窓の平均)、`get_frame_plan` の barrier / scope / alias group / pass の数、Frame Plan の status 行、Logical graph の絵。数は覚えるだけで、変わるべき所だけを要求します。
  - 基準: Graph の node の帯(GL-2b)、ループと lighting の pass の Layers、Physical plan・Barriers・Decisions・Resources のタブ。
  - Rendering settings の Draw sort を state batched に: 保存されるが plan は動かない(barrier・scope・pass の数が同じ)。GUI のどこも「効かなかった」とは言いません(description の記録)。✅UX-12b から、選ぶ項目の字は provider 名の `state_batched_v1 / back_to_front_v1` です(UX-10 で combo が engine の provider の一覧から作られるようになり、`State batched / back to front` の項目が無くなっていました)。
  - Features の `shadow_directional` の Replace with loop version…: 影の pass が `shadow_cascades/0/depth` になり、plan の大きさは変わらない。畳んだ形に戻す道は Edit ▸ Undo Render Edit だけです。
  - `TRANSFER_SRC`: ✅UX-6a から、Add fullscreen pass の image は最初から alias の候補です。lit の image に `TRANSFER_SRC` の check を付けて alias から外し、Physical plan の **Remove TRANSFER_SRC** で外すと、alias group の数が前と同じに戻り、帯が aliased を言い、絵は同じです。GB-2 の着地時は、Studio が作った 5 枚の image の `TRANSFER_SRC` を 1 枚ずつ外していました。
  - ✅UX-6a の決定 5: bloom の body の pass の欄に入力の Filter / Address / mip が出ます。✅UX-7 から、その Address は Studio が書いた `clamp_to_edge` で、combo が押せることを見ます(上の「pass の欄の Inputs」)。json の description は、✅UX-9 で「番号付きの入力も sampling を取るので combo が押せ、bloom の body の番号付きの入力は Studio が線と一緒に書いた `clamp_to_edge` を読む」に直りました(UX-9 の前は UX-6a の時点の「既定の repeat が見え、番号付きの入力なので押せない」のままでした)。
  - ✅UX-6b: 基準で Frame Plan の status 行(`pelican.framePlanPerf`)が ` ms · ` と `alias group` を言うまで待って読み、ST-1 の `get_status.gpu_timing.node_averages` が行を持つこと、lighting の pass の帯が ` ms` を言うこと、Graph の上の行が同じことを言うことを見ます。各最適化の後に前後の 1 行を待って覚えます —— draw sort は `after draw_sort` で `no change in the plan` を含み、Replace with loop version は `after Replace with`、TRANSFER_SRC の check と Remove は `after render_fullscreen__color:`(2 本の行は世代の数で違う)、Count 3 は `after loop:` で `no change in the plan` を含まず、Multisampling 4 は `after multisampling`。数は覚えるだけです。RT の mask を入れた後の plan の `diagnostics` も覚えるだけで、shadow の pass の `output unread` は要求しません(lighting の雛形が shadow map を mask と並べて読み続けるため。step の記録)。
  - bloom のループの Count を 3 に: barrier・scope・pass が 1 つずつ減り、ゾーンが `×3 copies` を言います。
  - Multisampling 4: engine が起動し直し、frame の GPU 時間が上がります(変化があれば測りが捉えることの確認)。
  - Regions: direct の rendering 設定の project には preset が無いので、選ぶ region がありません。
  - description は、複数の node を畳む・決定を宣言する・等価性の印を「設計だけ(GUI 無し)」としています。この 3 つは ✅GL-3a / GL-4a で GUI に入りましたが、このシナリオは通していません。
  - 打つ欄は `gui_build_full` と同じ(tone map の書き換えを除く)と、Count の 3 です([最適化のシナリオ](../../test/studio_scenarios/gui_build_optimize.json))。

✅GL-3a / GL-4a の `graph_fold_decisions`(json の step は 103)は、Blank pipeline の project で fold group を作り、検査して決定を効かせ、消します(上の「root の node を fold group にまとめる(✅GL-3a)」「fold group を検査する —— Verify(✅GL-4a)」「Frame Plan から graph 全体の決定を書く(✅GL-3a)」)。Graph タブで Add fullscreen pass を 2 回し、1 つ目の色を 2 つ目の `+ input` へ、2 つ目の色を `present` の `+ input` へ線で引いて、`get_fold_groups` が空であることを見ます。2 つの pass を見出しで選び(2 つ目は Ctrl)、**Fold into group…** を発火すると、notice が `Folded render_fullscreen, render_fullscreen_2 into fold_1` を言い、`get_fold_groups` に `fold_1`(node 2 つ、`unverified`)が出て、枠と見出しの `not verified — decisions are not applied yet` が描かれます。Decisions の Isolate の node の combo で `render_fullscreen_2` を選んで Add し、root の文書が `isolate` を持ち、まだ `unverified` で、Frame Plan の Raw JSON に `fold_unverified` があることを見ます。**Fold** で畳んだ node `graph_node#fold:fold_1` と行 `graph_derived_socket#fold:fold_1/render_fullscreen_2.color` が出て pass が消え、group の nodes が 2 のままであることを見て、**Open** で戻します。見出しで group を選び、Quick check を付けて **Verify** を押すと、見出しの字が `verifying` を言い、notice が `fold_1 verified` を言い(待ちの上限は 600 s)、`get_fold_groups` が `verified` になり、見出しの字が消え、2 つ目の pass の帯が `isolate (fold fold_1)` を言い、Decisions の行が `applied → render_fullscreen_2` を言い、Frame Plan の Raw JSON が `"from_fold": "fold_1"` を持つことを見ます。次に Unfold を確認して group と枠が消えることを見ます。最後に Frame Plan の Resources で `render_fullscreen_2__color` の行を選んで `pelican.framePlan.planning.noAlias` を発火し、status 行が `No alias set on render_fullscreen_2__color.` を言い、root の文書が `"no_alias": true` を持つことを見ます。絵を 4 枚撮ります(`graph_fold_decisions_folded.png` / `graph_fold_decisions_open.png` / `graph_fold_decisions_verified.png` / `graph_fold_decisions_window.png`)。打つ欄は New Project のフォルダと名前だけです([fold group のシナリオ](../../test/studio_scenarios/graph_fold_decisions.json))。この文書では走らせた結果を確かめていません(未確認)。

✅GB-3(`ba3b8a72`)の `gui_build_ocean` と `gui_build_ltc`、✅GB-4(`e056fa25`)の `gui_build_music` は、File > New Project > Blank pipeline から Studio の GUI だけで、GB-2 より難しいもの(FFT の海・面光源・音楽)を組みます。shader は、人が自分のエディタで書くのと同じく、`file` step で project の `shaders/` に書きます。止まった所と打った欄は json の description にあり、この文書では 3 本を走らせた結果を確かめていません(未確認)。✅UX-8 の `gui_build_curve` と ✅EL-5s の `gui_build_ocean_compute` も同じ流儀で、下の最後の 2 項目です。

- **`gui_build_ocean`(FFT の海。json の step は 259。GB-3 の着地時は 529、UX-7 で 552、LC-1s で 235)**: どの段も、Add fullscreen pass で Fixed の 256×256 `R32G32B32A32_SFLOAT` の image に描く fullscreen pass です(format を選び Fixed を押す)。h0(Phillips の spectrum)、scene の時刻での spectrum(shader が `pelicanFrame.time_delta.x` を読む。graph から pass に値を渡す道が無いため)、逆 FFT の 16 回の `graph.loop`(Count は ƒ で `2 * log2(256)`。engine の RS-15 の `log2`)と carry、`(-1)^(x+y)` を掛ける displacement の pass、palette の `render.raster` が displacement を vertex の段で読んで海を `present` に描きます。
  - ✅LC-1s から、FFT の段は 1 本の body の pass が Fragment の一覧から選ぶ `ocean_fft.frag` で、`pelican_loop_element()` で段を決めます。body の pass の帯が `reads element`、pass の欄が回の番号を読むことを言うのを見ます。GB-3 の着地時は、段ごとに分岐を 1 本ずつ(16 本)作り、分岐ごとに `ocean_fft_<k>` を選び分けていました。
  - ✅UX-7 から、raster の pass の draw は pass の欄の Grid 257×257(6×256×256 の頂点、index buffer 無し)と Instances の 2(instance 1 は遠い面の空)、depth は Depth の欄の New(`D32_SFLOAT`)と Raster の欄の depth compare `less_equal` です。GB-3 の着地時は draw を JSON で打ち、行を遠い方から描く順で重なりを避けていました。raster の pass はループより先に足します(ループの後に足すと canvas に描かれなかった件は、上の「ループと分岐の後に足した node も描きます」で直りました)。最後にもう一度 Play し、Play の間の `quit`(上の「Play の間の `quit`」)を通ります。
  - ✅PS-1s から、h0 の pass の Schedule(`pelican.graphCanvasPass.schedule`)を `Once` にし、notice が `Saved; render_fullscreen__color made Persistent` を言うこと、pass の帯が `once · ran gen`、image の帯が `persistent`、image の Lifetime の欄が `Persistent` を言うことを見ます。`get_frame_plan` の `nodes[0]`(h0)が `schedule: "once"` で `last_run_generation` を持ち、`get_status` の `frame` が 30 進んでも `last_run_frame` が変わらないこと(毎 frame は走っていない)、pass の欄が `Ran in generation` を言い、**Recompute** を押すと notice が `Recompute: 1 once pass runs again` を言って `last_run_frame` が押す前より進むことを見ます。画素の確かめ(次の項目)の条件は変わっていません。
  - 画素: 海はグラデーションでも 1 色でもない。edit では 30 frame 離れた 2 回の読みが同じ、Play で波が動き、Pause で止まり、Stop で edit に戻る。
  - compute の task は使えませんでした(description の記録): palette の compute は port 無しで置かれ、descriptor を宣言する shader は置く時に断られ、後から書く port はつながっていない入出力として断られる。✅EL-5s で palette から project の `.comp` を ports と image ごと置けるようになり、h0 を compute にした版が下の `gui_build_ocean_compute` です(`gui_build_ocean` 自身は fullscreen の pass のままで、description のこの記録も残っています)。
  - ✅UX-14(`aaba15a6`)から、海と太陽は camera が向く −Z の側です(vertex shader の `EXTENT_Z` を `(-2.5, 140)` から `(2.5, -140)` に、fragment の `SUN` の z を 1 から −1 に)。engine の CC-1 で Blank の雛形の MainCamera が −Z を向くようになったためです。`gui_build_ocean_compute` も同じ shader を書くので同じく直しました。
  - 打つ欄は、New Project のフォルダと名前、palette の絞り込みの `render.raster`、Count の式 `2 * log2(256)`、Instances の 2 です([海のシナリオ](../../test/studio_scenarios/gui_build_ocean.json))。
- **`gui_build_ltc`(LTC の面光源。json の step は 192。GB-3 の着地時は 96)**: `gui_build_D` の deferred の道(palette の `deferred_geometry_v1` の material pass、Cube と Floor、Uses scene lights の Add fullscreen pass、New… > Deferred lighting > Create、その色を `present` の `+ input` へ)に、project に書いた `shaders/ltc_lighting.frag` を Fragment の一覧から選びます。まず LTC の表を使わず、GGX の lobe を正規化した clamped cosine で代えた形で、床の光の映り込みの所が雛形の色と違い、映り込みの左右も雛形の色でない(光の形が残る)ことを画素で見ます。✅IA-1s から、続けて表を使う形に替えます(GB-3 の着地時は、image の file を render target に読み込む道が無く、ここまででした)。
  - 表は Studio の外で作ります: シナリオの隣の [`gui_build_ltc.prepare.py`](../../test/studio_scenarios/gui_build_ltc.prepare.py) が、Studio を起こす前に 64×64 の RGBA 32 bit float の表 2 枚を閉じた式から raw の float で書き、`pelican_cli image ktx2 --format R32G32B32A32_SFLOAT --raw` で `.ktx2` にします(上の「回し方」の prepare の口)。公開の LTC の表は持ち込まず、式は Heitz 2016 の fitting の代わりの `ltc_1 = (1, 0, min(tanθ, 4)(1 − a), a)`、`ltc_2 = (1 − a/2, (1 − cosθ)^5 / 4, 0, 1)`、`a = max(r², 0.02)` です(列が roughness r、行が `sqrt(1 − cosθ)`。script の注記)。表の値の正しさは問わず、読めて shader に届き、絵が変わることを見ます。
  - palette の `render.image` を 2 つ置き、それぞれ Format を `R32G32B32A32_SFLOAT`、Usage の `COLOR_ATTACHMENT` を外し、Lifetime を `Persistent` にします(Per frame の間は Initial contents が押せないことも見る)。Import… のダイアログに表の path を打って、`assets/images/ltc_1.ktx2` / `ltc_2.ktx2` が写り、文書の `initial.asset` になり、Initial contents の欄がそれを言い、Size from the image が `64 × 64` を言うことを見ます。
  - `file` step で書いた `shaders/ltc_lut.frag` を lighting の pass の Fragment に選び、2 つの image をその宣言 5 と 6 のソケットへ drag します。映り込みが近似の形と違うこと、左右が違う色(光の形が残る)こと、Inspector で光の強さを 2 にすると映り込みが変わることを画素で見ます。
  - 最後に、Studio の外で焼き直した表 2(`ltc_2_rebaked`。Fresnel の分を一様に 1/2 にした表)を `file` step の `copy` で project の `assets/images/ltc_2.ktx2` に上書きします。見張りが `reload_render_documents` を頼み、Graph の notice が `Picked up the change to assets/images/ltc_2.ktx2` を言い(IA-2 の `changed_assets`)、映り込みが変わることを見ます。
  - ✅UX-14(`aaba15a6`)から、矩形の光は z = −4 にあり camera の側を向きます(UX-14 の前は z = +4。角の順も替えた)。CC-1 の後の絵から、映り込みと暗い床を読む点を選び直しました。
  - 矩形の角は shader の定数で、輝度は KeyLight の色と強さです。GB-3 の着地時は、palette の `render.scalar` / `value.constant` / `value.input` が欠けた `target` / `value` / `name` で断られ、書く欄も無いので角を定数にしました(上の「palette で置くときに、要る parameter を先に聞きます(✅UX-7)」で欄はできましたが、このシナリオは書き直していません)。打つ欄は、New Project のフォルダと名前、palette の絞り込みの `material_pass` と `render.image`、Import のダイアログの表の path(2 回)、光の強さの 2 です([LTC のシナリオ](../../test/studio_scenarios/gui_build_ltc.json))。この文書では走らせた結果を確かめていません(未確認)。
- **`gui_build_music`(音楽。json の step は 132。GB-4 の着地時は 83、UX-8 で 120)**: 2 秒の小さな WAV(8 bit PCM で、どの byte も ASCII なので `file` step の文字の書き込みで byte が保たれる。約 −24 dBFS)を、人がディスクに file を持っている代わりに `file` step で project の外に書き、✅UX-8 から **File ▸ Import Sound…** のダイアログに path を打って project の `assets/audio` に写します(GB-4 の着地時は `file` step で `assets/audio` に直に置いていました)。palette の `audio.playback`(その音と scene の時計がつながって来る。✅Q-10 / Q-11)を置いて、Bus を `bgm`、Wrap を `repeat` にキーボードで選び、Sound の欄の **Import…** で同じ file をもう一度写すと `pulse_2.wav` になり、voice がそれを鳴らします。palette の `audio.volume` は voice を聞くダイアログで `audio_playback` を選んで 1 で置かれ、palette の `value.curve`(1 秒で 0 から 1)を volume の `value` へ線で落とします(フェードイン)。音が出ているかは聞かず(driver は null の backend を名指しし、`get_status` の `audio` もそう言う)、`get_graph_node_values` の `audio_playback.voice`(voice の経過秒)で読みます: edit では 0、Play で進んで scene の時計と同じ、Pause で止まり、Stop で 0 に戻ります。フェードの曲線も一緒に 0 から 1 へ上がります。✅UX-8 から Timeline は `Values from:` に何も選ばずに voice を追い、Live で Pause の間に帯の左端を押すと scene の時計と voice が一緒に戻ります(GB-4 の着地時は、空の definition(Add loop)を MainCamera に装着して帯の出所にしていました)。✅UX-9 から、palette と voice のダイアログの後に待ちを置かず、`get_status` の `audio.voices[0].state` が edit で `idle`、Play で `playing` であること、`audio_playback` の帯が `playing · <秒> s` を言うことを見て、Play → Stop → Play し、2 回目の Play の始めの `position_seconds` を読んで覚えます(要求はしません。頭から鳴り直すはず、と description は言います)。打つ欄は、New Project のフォルダと名前、Import のダイアログの path(2 回)、palette の絞り込みです([音楽のシナリオ](../../test/studio_scenarios/gui_build_music.json))。GB-4 の着地時に GUI で止まった曲線 → shader の道(`render.buffer` に値のソケットが無い、pass に名前付きの buffer の port が無い)は、UX-8 の `gui_build_curve` が通します。
- **`gui_build_curve`(曲線 → shader。json の step は 94。✅UX-8 の着地時は 90、UX-13a の前は 88)**: File > New Project > Blank pipeline から Studio の GUI だけで、曲線で shader の値を動かします。シナリオが fragment shader を書きます(✅UX-13a から、`pelican_resource_ports.glsl` を include し、port `level` を `#ifdef PELICAN_RESOURCE_PORT_level` の中で `pelican_resource_level` として読み、その float を 0.5 の帯の下に灰色で描く。port が無い間は 0。UX-13a の前は最初の pass の入力の binding に storage buffer を手で宣言していました)。fullscreen pass でそれを `present` に描き、palette で `render.buffer`(float、1 要素)と `value.curve`(scene の時計の上の 0 から 1 の ramp。描かれたものと重ならない所に置かれる)を置きます。GB-4 で手で書いた所は GUI の操作です: 曲線を buffer の value のソケットへ drag、pass の Buffers の **+ Buffer port** で shader が読む名前 `level` が出ることを見て、読む buffer に `render_buffer` を選んで Add(名前付きの buffer の port と element と線が 1 回の編集)。その後、生成 include が `pelican_resource_level` を宣言し、pass の binding 0 のソケットが出ることを見ます(UX-13a の前は buffer を binding 0 のソケットへ drag していました)。Keys の表で ramp を 0.1 → 0.9 にし、`Values from:` に何も選ばずに Timeline が曲線を追うこと、帯の右端を押すと scene time 1 が描かれて灰色が明るく、左端で暗く戻ること、Play で時計が進むと明るくなることを画素で見ます。✅UX-9 から、palette の Return の後に待ちを置きません。打つ欄は、New Project のフォルダと名前、palette の 2 つの絞り込み、2 つの key の値です([曲線のシナリオ](../../test/studio_scenarios/gui_build_curve.json))。この文書では走らせた結果を確かめていません(未確認)。
- **`gui_build_audio_reactive`(音に反応する描画。json の step は 159。✅AB-1s の着地時は 148、UX-13a で延びました)**: GB-4 の目標 3 です。シナリオの隣の [`gui_build_audio_reactive.prepare.py`](../../test/studio_scenarios/gui_build_audio_reactive.prepare.py) が、Studio を起こす前に project の外に 2 つの WAV を書きます(1 秒、8 kHz、16 bit mono。62.5 Hz の音を振幅 0.1、ピークで −20 dBFS)。`beat.wav` は前半 0.5 秒が無音で後半が音、`beat_changed.wav` はその逆です。
  - Blank pipeline から **File ▸ Import Sound…** で `beat.wav` を `assets/audio` に写し、palette の `audio.playback` で鳴らします。Sound の行の **Bake analysis…** のダイアログで、Table の既定が `assets/analysis/beat.ktx2` であることを見て、Peak を外し、Bands を 8 から 4 にキーボードで下げて Bake すると、行が `Baked assets/analysis/beat.ktx2: rms, band:0..band:3 × 101 samples` を言い、表と manifest が project にあることを見ます。
  - シナリオが fragment shader `shaders/audio_level.frag` を書きます(人が自分のエディタで書く代わり)。右半分は表の band 0 を GPU で `pelican_sample_table` で読み、左半分は `render.buffer` が渡す数(CPU の道)を、0.5 の帯の下に灰色で描きます。✅UX-13a から、2 つの buffer(`beat_layout` と `value_level`)は `pelican_resource_ports.glsl` を通して `#ifdef PELICAN_RESOURCE_PORT_<port>` の中で読み、port の無い間は既定の値です(binding は手で書きません。image の `beat_table` だけ binding 0 を宣言)。
  - **Use in Graph** で `value.sample_table`(`row: "rms"`、`time` の線無し)が置かれます。Add fullscreen pass に shader を選んで色を `present` の `+ input` へ線で引き、pass を選んで **Use in a shader** を押すと、表の image(`beat_table`)と layout の buffer(`beat_layout`、値は `[1, 101, 0.01]` と scene time)が 1 回の編集で置かれ、image は shader の binding 0 に、layout は shader の text が読む名前の port `beat_layout` に繋がり、写す 1 行が出ます。その後、生成 include が layout を宣言し、反映で binding 1 のソケットが出ることを見ます(UX-13a の前は「binding 0 と 1 に繋がる」でした)。
  - (b) GPU の道: `value.sample_table` を選んで Timeline を Live にし、帯の左端(scene time 0、無音)と右端(1 秒、音)を押すと viewport の右半分の画素が変わり、左端に戻すと音の時の色ではなくなることを見ます。
  - (a) CPU の道: palette の `render.buffer` を置き、`value.sample_table` の出力をその value へ drag し、pass を選んで Buffers の **+ Buffer port** で、shader が読むまだ port の無い名前 `value_level` が出ることを見て、読む buffer に `render_buffer` を選んで Add します。engine の規則で `beat_layout` の後の binding 2 のソケットが出て、Buffers の欄が `binding 2 pelican_resource_value_level` を言うことを見ます(UX-13a の決定 1)。同じように左半分の画素が変わり、無音に戻すと最初の色に戻ることを見ます。UX-13a の前は buffer を pass の binding 2 へ drag していました。
  - 古さ: `file` step の `copy` で `beat_changed.wav` を project の `beat.wav` に上書きし、Graph の **Refresh** を押すと、`value_sample_table` と `beat_table` の帯が `stale: source_changed` を言い、`value_sample_table` を選ぶと Re-bake が出ます。Re-bake で焼き直すと、✅UX-13a から、3 秒の間 `Graph change saved.` が出ないこと(編集を送らない)、`value_sample_table` の帯が消え `beat_table` の帯が stale を言わないこと、notice が `Picked up the change to assets/analysis/beat.ktx2` を言うこと(engine の IA-3)、Graph の Undo が押せないことを見ます。その後、scene time 0(新しい音では音のある所)で、左半分(CPU の道)も右半分(GPU の道)も、無音のときに覚えた画素ではなくなることを見ます。UX-13a の前は、読み手を書き直す編集の保存を待ち、右半分は画素を読むだけでした。
  - engine は pass の buffer の読みを、image の後に読む buffer の名前の順(`beat_layout`、次に `render_buffer`)に bind し、ソケットと Buffers の欄がそれを示します(description の記録)。UX-13a の前の description は、shader が binding 1 と 2 を手で書くので、`beat_layout` が `render_buffer` より先に bind されるよう音の名前を `beat.wav` にした、と書いていました。打つ欄は、New Project のフォルダと名前、import のダイアログの path、palette の絞り込みです([音に反応する描画のシナリオ](../../test/studio_scenarios/gui_build_audio_reactive.json))。実装者の run では緑でした。
- **`gui_build_ocean_compute`(compute の h0。json の step は 268。✅EL-5s の着地時は 244)**: `gui_build_ocean` の最初の段 h0 を compute の task にした版です。シナリオが `shaders/ocean_h0.comp`(image `pelican_resource_h0` を `rgba32f` で宣言し、`ocean_h0.frag` が描くのと同じ数を計算する)を書き、palette の `render.compute` の一覧から選びます。Studio が `get_shader_interface` を聞いて、task と port、その port につながった `R32G32B32A32_SFLOAT` の image(`STORAGE` / `SAMPLED`)、その image の `dispatch.groups_from` を 1 回の編集で置くこと(絞り込みのほかに打つものはありません)を見ます。✅PS-1s から、置いた task の Schedule(`pelican.graphCanvasCompute.schedule`)を `Once` にして image が同じ編集で Persistent になること、frame plan が 30 frame 後も h0 を走らせ直していないこと、compute の欄の **Recompute**(`pelican.graphCanvasCompute.recompute`)で走り直すことを、`gui_build_ocean` と同じ形で見ます。image を Fixed 256×256 にし、task の Shader の欄が shader を名指して reflection error を言わないことを見て、task の `h0` を spectrum の pass の `+ input` に落とします。後は `gui_build_ocean` と同じで(spectrum が 1 つ目の fullscreen pass、displacement が 2 つ目)、画素の確かめも同じ条件です(海はグラデーションでも 1 色でもない、edit で止まり、Play で動き、Pause で止まる)。commit の記録は「fullscreen 版と同じ画素」と言いますが、シナリオは fullscreen 版の画素と比べてはいません。打つ欄は、New Project のフォルダと名前、palette の絞り込みの `render.raster` と `render.compute`、Count の式 `2 * log2(256)`、raster の pass の Instances の 2 です([compute の海のシナリオ](../../test/studio_scenarios/gui_build_ocean_compute.json))。この文書では走らせた結果を確かめていません(未確認)。

✅UX-10 の `gui_settings_choose` と ✅UX-12a の `gui_node_parameters` は、型が決めている設定を打たずに選ぶことを、File > New Project > Blank pipeline から GUI だけで見ます。止まった所と打った欄は json の description にあります。

- **`gui_node_parameters`(node の欄。json の step は 164。✅UX-12a の着地時は 122、UX-12e で 169、UX-13b で 164)**: palette で `render.buffer` を置き、選ぶと欄が表のとおりであること(`element` が表の値の combo、`lifetime` が engine の既定の `persistent` を出す)を見ます。`size` の整数の spin に 64 を打って文書に入り、`lifetime` で `transient` を選び、**Reset** でその鍵が文書から消えて欄が既定に戻ることを見ます。`size` に表の範囲(0 以上)の外の −4 を打つと、Graph の notice が `render_buffer.size must be integer (range: at least 0)` を言い、`size` の行の下(`pelican.graphCanvasParameters.size.problem`)にも同じ文が出て、文書は 64 のままです。128 を打つと通り、行の文が消えます。続けて palette の `value.trigger_clock` の `paused` に 1 を打ちます。
  - ✅UX-12e で延ばした所: palette で 2 つ目の `render.buffer`(`render_buffer_2`)を置き、palette の `render.scalar` の必須のダイアログで `target` を combo から選び、文書の `target` が `render_buffer_2` になることを見ます(この combo は UX-8 からの Studio の buffer の combo です。上の「node の欄の残りを選ぶ形に(✅UX-12e)」)。UX-12e の着地時は、先に `render_buffer_2` の `host_source` に `runtime_f32` を手で選んでいました。✅UX-13b から、`render_buffer_2` は置いたまま(`{element: float, size: 4}`)で、combo の `render_buffer_2 (will be set to runtime_f32)` を選び、置く編集が同時に `host_source: "runtime_f32"` を書くことを見ます(上の「汎用の欄の残り(✅UX-13b)」)。palette の `render.image` を置いて選び、Mip levels の combo が `Count` を出していること、`Full chain` を選ぶと文書の `mip_levels` が `"full"` になり、数の spin が隠れることを見ます。
  - 最後に、root で palette を開いて `pose.clip_sample` を絞り込むと、行が灰色で末尾に **New definition…** が出ます(✅UX-12e)。押すと palette が閉じて definition の名前の行が開き、`walk_pose` を打って Enter で作ると、canvas が `walk_pose` に入り、palette が `pose.clip_sample` を絞り込んだ状態で開き直します。UX-12a の着地時は、New definition… の action(`pelican.graphCanvas.newDefinition`)で `walk_pose` を作ってから palette を開いていました。Enter で `pose.clip_sample` を選ぶと必須のダイアログが出て、`rig` の **Import…** のダイアログに project の外に書いた rig 付きの glTF の path を打つと `project://assets/models/hero.gltf` に写り、`clip` を rig の animation の `Run` から選んで置きます。Pose の欄で speed 2、loop を外す(check に Space)、start time 0.5 が文書に入ることを見ます。
  - JSON はどこにも打ちません。打つ欄は、New Project のフォルダと名前、palette の絞り込み(UX-12e で `render.buffer`・`render.scalar`・`render.image` が増えた)、definition の名前、import のダイアログの path、数の 64・−4・128・1・2・0.5 です([node の欄のシナリオ](../../test/studio_scenarios/gui_node_parameters.json))。UX-12a の時点も ✅UX-12e の延ばした形も、✅UX-13b の形も、実装者の run では緑でした。
- **`gui_settings_choose`(選ぶ設定。json の step は 206。✅UX-10 の着地時は 136、UX-12b で 175、UX-12d で 197、UX-13d で 205)**: `gui_build_D` の deferred の pipeline(lighting の pass と `shadow_directional`)を同じ手順で組み、Features の `shadow_directional` の Parameters が schema から作った欄であること(strength の slider を End で端まで押すと Apply 無しで送られ影の床の色が変わる、pcf_radius の spin に 2)、Inspector の KeyLight の向きが方位角と仰角の欄であることを見ます。✅UX-14 から方位角は camera の yaw と同じ基準で(上の「光の向きは方位角と仰角で、camera の yaw と同じ基準です」)、engine の CC-1c(`b0b2fc77`)の後の雛形の光(−X の側へ、camera から離れる向き)は 116.57 と読め、−116.57 を打つと x だけが反転し、z は camera から離れる向きのまま(`direction[2] < -0.15`)で、影が箱の左から右へ移ります(`a954d38c`)。影は箱の脇の細い楔になったので、影と照らされた床を読む点を `(0.31, 0.7)` / `(0.69, 0.7)` から `(0.325, 0.695)` / `(0.675, 0.695)` に選び直しました(読むすべての段で)。→ ✅PX-1(`e3627606`)で、viewport が縦に伸びるとこの点が箱の中に入ったので、`(0.27, 0.71)` / `(0.73, 0.71)` にもう一度選び直しました(下の「画素を読む点と窓の大きさ(✅PX-1)」)。→ ✅PX-2(`28bc5ef9`)で viewport が 720x450 に固定されたので、影の真ん中に寄せた `(0.31, 0.7)` / `(0.69, 0.7)`(`gui_build_D` などと同じ点)にしました(30 か所。下の「viewport の大きさを決める(✅PX-2)」)。UX-10 の着地時は旧い基準で −63.43 から 63.43、UX-14(`aaba15a6`。CC-1c の前の雛形の光)では 63.43 から −63.43 でした。Rendering settings では、Material route default に Forward を選ぶと engine が `material_route_unserved@1` で断り、✅UX-12b から notice が `No forward material pass serves the route, so it stays on auto — add a forward material pass first.` を言って欄が Auto に戻ること、Snapshots の + が `Saved snapshots.` になり、行の after が `After opaque (post_ldr)` であることを見ます(UX-12b の前は `unknown node: post_ldr` で断られる期待でした)。✅UX-12b で延ばした後半: Features で `sky_ambient` を Add すると、何も送る前に notice が `sky_ambient goes after deferred_lighting, which this pipeline does not have. Place Deferred lighting first.` を言い、**Place Deferred lighting in the Graph** で Graph に `deferred_lighting` の pass が入り、その New… が Deferred lighting の雛形で開くので Create します。もう一度 Add すると、✅UX-12d から `sky_ambient needs lit_color (color) and scene_depth (depth). Add them with the feature, and write scene_depth from render_material_pass.depth?` が出て、**Add with targets** を押します。
  - ✅UX-12d で延ばした後半: Add with targets の 1 本の `edit_batch` が committed になり、`get_render_features` に `sky_ambient` が入り、文書に `scene_depth_ref` があることを見ます。続けて notice が `sky_ambient draws into lit_color, which nothing reads. Connect it to present to see it.` を言い、**Connect lit_color to present** を押すと、frame plan の `present` が `lit_color` を読み、viewport の左上の隅(押す前は何も描かれず黒)の画素が変わります。✅UX-13d から、押す前に root 文書を `get_render_documents` で覚え、notice が `present shows lit_color.` を言うのを待ち、文書に `lit_color_ref` への繋ぎがあること、Edit > Undo Render Edit の 1 回で root 文書が押す前と同じに戻ること(2 つの ticket なら graph の編集が残る)、Redo の 1 回で `present` がまた `lit_color` を読み繋ぎも戻ることを見ます。`sky_ambient` の Parameters で `color_r` に 4 を打つと、その画素がまた変わります。shadow_directional の Parameters で `cascade_count` に 3 を打つと **Fix: make shadow_map_ref 2D array depth (3 layers)** が出て、押すと 1 本の `edit_batch`(`cascade_count` 3 と、1 つの graph の編集で `shadow_map_ref` の型とそれを読む 2 つの lighting の pass の `shadow_map` の port の型)が committed になり、`cascade_count` が 3 で、箱の右の床が照らされた床の色ではない(影のまま)ことを見ます(上の「書き手を繋ぎ、1 本の edit_batch で足す(✅UX-12d)」)。
  - UX-12b の着地時は、Add with targets の後に feature が `Pass input target is not produced as an earlier output: scene_depth` で断られ(root に `sky_ambient` も `scene_depth_ref` も残らない)、Fix は `graph type mismatch at deferred_lighting.shadow_map from shadow_map_ref.resource` で断られる(`cascade_count` は既定のまま)ことを、期待として待っていました。✅UX-12d でその期待は外しました。
  - 打つ欄は、New Project のフォルダと名前 `gsc`、palette の絞り込みの `material_pass`(`gui_build_D` と同じ)、数の 2・−116.57・4・3 です([選ぶ設定のシナリオ](../../test/studio_scenarios/gui_settings_choose.json))。UX-12b の形(挿入先を先に置いてから Add with targets)は、統合側で develop の build に載せて仮想モニタで緑でした(177 step、`99f645fb`)。✅UX-12d の形(197 step)は、実装者の run で `gui_build_D` と合わせて緑でした。✅UX-13d の形(205 step)は、実装者の run で `gui_build_E` と合わせて緑でした(CC-1 の前)。UX-14 の実装者の全シナリオの run(CC-1 の後)では、61 本中 7 本(`add_primitive_object`・`forward_loop_from_scratch`・`gui_build_A` / `B` / `C` / `full` / `optimize`。Blank の箱の正面を読むもの)が、雛形の光が箱の奥の面を照らすために赤でした(commit の記録は engine の雛形の光の向きとし、Studio では直していません)。engine の CC-1c の後の統合側の run では、この 7 本が緑、`gui_settings_choose` が赤で、`a954d38c` の直しの後に 2 回緑でした(作業の log)。

✅RL-1s の `render_reload_external`(json の step は 58)は、外のエディタで root の JSON を書き換える人の代わりに、`file` step で `passes/main_rendering_config.json` を書き換えます(上の「外のエディタで書き換えた render の文書を拾います(✅RL-1s)」)。Blank pipeline の project で Graph タブの Add fullscreen pass の色を `present` の `+ input` へ線で引き、image の format を Graph で `R8G8B8A8_UNORM` に選んで(Graph の Undo が押せる)、file がその値を持つことを見ます。次に file の format を `R16G16B16A16_SFLOAT` に書き戻すと、notice が `Picked up the change to main_rendering_config.json.` を言い、engine と Graph の欄がその値になり、Graph の Undo が押せなくなります。file に `,,` を書いて壊すと `Could not load main_rendering_config.json: fix the file and save again.` が出て engine は前の版のまま描き続け、その間の Graph の編集は `could not be loaded` を言って engine は何も取らず、Reload from disk は同じ拒否を言い直します。`,` を直す(engine が持っていない format で)と拾われ、Graph の編集がまた通り、その commit の後に読み直しが起きない(notice が `Graph change saved.` のまま)ことを見ます。打つ欄は New Project のフォルダと名前だけです([外の書き換えのシナリオ](../../test/studio_scenarios/render_reload_external.json))。この文書では走らせた結果を確かめていません(未確認)。

✅HW-2s / AV-1s の `gui_assets_watch_verify`(json の step は 46)は、Assets のドック(§13.1 の「Studio の Assets のドック」)を File > New Project > Blank pipeline から GUI だけで使います。`get_project_settings` で雛形が `hot_reload` を宣言していないこと、**View > Assets** でドックが前に出て監視の 1 行が無く **Track asset contents…** が出ていることを見て、`textures` の行を選んで右クリックの **Reload when saved outside** を押すと、`get_status.reload.watch` と project.json の `hot_reload` が `["assets/textures"]` になり、監視の 1 行が `Watching 1 folder` を言います。**Track asset contents…** で `Asset contents match the manifest` になり、`asset_stores.main.manifest` が `assets.manifest.json`、`get_assets_verification` の issue が 0 件で、ボタンが消えます。次に、Studio の外で焼いた texture(シナリオの隣の [`gui_assets_watch_verify.prepare.py`](../../test/studio_scenarios/gui_assets_watch_verify.prepare.py) が `pelican_cli image ktx2 --raw` で作る 4×4 の `R32G32B32A32_SFLOAT`)を `file` step で `assets/textures/stray.ktx2` に置くと、engine が読み直して監視の 1 行が `last reload: textures/stray.ktx2` を言います。**Verify again** で `1 file differs from the manifest: 1 extra.` になり、`stray.ktx2` の行が開いて見え、engine の issue の `kind` も `extra` です。**Update manifest** は `1 file will change in assets.manifest.json.` と確かめてから書き、照合が 0 件に戻ります。最後にドックの Undo を 2 回押し、1 回目で `asset_stores` が `null` に戻って Track asset contents… がまた出て、2 回目で `reload.watch` と `hot_reload` が `null` に戻り監視の 1 行が消えることを見ます。打つ欄は New Project のフォルダと名前だけです([Assets のドックのシナリオ](../../test/studio_scenarios/gui_assets_watch_verify.json))。実装者の run では 3 回目に緑でした(1・2 回目の赤は、閉じたフォルダの中の行を待っていたシナリオの手順の誤りで、実装に「新しく印の付いたファイルのフォルダを 1 度だけ開く」を足し、読み直しを待ってから Verify again を押す順にしました)。shader の読み直しとの重なり(決定 7)はこのシナリオでは通していません。

✅UX-15b の `gui_lower_layer_pin_freeze`(json の step は 48)は、File > New Project > Blank pipeline から GUI だけで、Frame Plan の Backends と Physical plan のタブを使います(上の「下の層の決定を Frame Plan から書く(✅UX-15b)」)。`get_frame_plan` が `ejectable_pin_package`(`pelican.vulkan_target_plan_pins`)を出し、root が `vulkan_plan_pins` を持たないことを見てから、Backends のタブの帯が `Not pinned` を言うのを待ち、`pelican.vulkan.materialized_plan@1` の行を選んで action `pelican.framePlan.backend.pin` を発火します。status 行の `Pinned pelican.vulkan.materialized_plan@1.`、root の `vulkan_plan_pins`、plan の `applied_pin_package.pins.backend_candidate`、帯の `Pinned: pelican.vulkan.materialized_plan@1` を見て撮ります(`gui_lower_layer_pinned.png`)。次に Physical plan のタブで **Freeze this physical plan** を押し、`Physical plan frozen.`、root の `vulkan_physical_fragments`、plan の `applied_physical_fragment`、帯の `Frozen:` を見て撮り(`gui_lower_layer_frozen.png`)、**Remove** で root から消えて帯が `Not frozen` に戻ることを見ます。Backends に戻って **Unpin** を押し、root から pin が消えて帯が `Not pinned` に戻ったら、Frame Plan の Undo(`pelican.framePlan.undoButton`)で Unpin を戻し、root がまた pin を持ち、engine が再び当てることを見ます。打つ欄は New Project のフォルダと名前だけです([下の層の決定のシナリオ](../../test/studio_scenarios/gui_lower_layer_pin_freeze.json))。実装者の run では緑でした(報告は 50 手と数えています)。`physical_fragment_stale` と Eject again、常駐 BLAS(Blank は ray tracing を持たない)、置き換えの来歴はこのシナリオでは通していません。

✅GL-5a の `gui_folded_pass_edit`(json の step は 42)は、File > New Project > Blank pipeline から GUI だけで、Graph タブの詳細欄で `present` を直します(上の「rendering 節の pass を Graph の詳細欄で直す(✅GL-5a)」)。Graph タブで `present` の畳んだ node を押すと、要約が `From the rendering section: edit it here.` を言い、詳細欄の Fragment が `engine://passthrough` です。engine の shader の一覧ができるのを待ってから Fragment で `engine://shader_lab_hello` を選び、notice の `present saved.`、root がその fragment を持つこと、viewport の中央の画素が変わることを見ます。Graph タブの Undo(`pelican.graphCanvas.undo`。render の編集の履歴の Undo)で root が `engine://passthrough` に戻り、画素が元に戻り、詳細欄も文書に従うことを見ます。最後に詳細欄の **Serial** を押し、`Serial set on present.`、root の `"serial": true`、`get_frame_plan` の `target_planning.graphs.main_render.nodes.present.serial` が `true` であること、Frame Plan の Raw JSON に `"serial": true` が出ることを見ます。打つ欄は New Project のフォルダと名前だけです([詳細欄のシナリオ](../../test/studio_scenarios/gui_folded_pass_edit.json))。実装者の run では 2 回目に緑でした(1 回目は Serial の check が欄の幅いっぱいに伸びていて、自動操作の click が文字の外に当たった。check を文字の幅にして直しました)。仕様は Serial を Frame Plan の Decisions のタブで見るとしていましたが、engine は graph 全体の serial を `decisions[]` の行に出さないので、plan の `target_planning` と Raw JSON で見ています(実装者の報告)。

✅CT-6s の `gui_build_rt_from_blank`(json の step は 79)は、File > New Project > Blank pipeline から GUI だけで、project の RT の shader を書いて trace の task を組みます(上の「RT を Graph で組む(✅CT-6s)」)。Add Object の Cube を置いて保存し(光線が当たる物)、`MainCamera` を選んでから Graph タブの palette を開き、絞り込みに `render.compute` を打ちます。shader の combo の横の **New…**(`pelican.graphCanvasPaletteComputeNew`)を押すと、combo が `project://shaders/ray_tracing  (ray tracing)` になり、project に `shaders/ray_tracing.rgen` / `.rmiss` / `.rchit` があり、`.rgen` が `#include "pelican_frame.glsl"` を持つことを見ます。Return で置くと、engine が 3 つの stage を読んでから `render_compute` とその image `render_compute__color` が描かれ、`get_graph_instances` の task が `ray_tracing` の `raygen` / `miss` / `closesthit` にその組を、`dispatch.rays_from.port` に `color` を持ち、image の format が `R16G16B16A16_SFLOAT` であることを見ます。compute の欄の Shader がその組を示し、reflection error の行が無いことを見てから、task の `color` を `present` の `+ input` へ drag し、notice の `Pass inputs saved.` と、frame plan の順(`__scene_acceleration_structure` → `render_compute` → `present`、`present` が `render_compute__color` を読む)を見ます。viewport の中央が前のグラデーションと違い、隅(miss の空)とも違う(cube の法線の色)ことを見て撮ります。最後に task の **More parameters** → `ray_tracing` の見出しを開き、`raygen` の欄がその組を示すこと、Max recursion に `2` を打って Return で `get_graph_instances` の `max_recursion` が 2 になり、`raygen` が残り、engine が trace を描き続ける(中央がグラデーションと違う)ことを見ます。打つ欄は New Project のフォルダと名前 `gb_rt`、palette の絞り込みの `render.compute`、Max recursion の `2` です([RT を Blank から組むシナリオ](../../test/studio_scenarios/gui_build_rt_from_blank.json))。ray tracing の pipeline を持つ device が要ります。実装者の run では 3 回目に緑でした(報告は 81 step と数えています。1・2 回目の赤は、シナリオの node の添字の書き違い —— Blank の graph は node が 0 個で、frame plan の先頭は `__scene_acceleration_structure` —— を直したもの)。報告の画素は、中央が `#bcbbff`、隅が `#65788b`、置く前のグラデーションの中央が `#bcbcbc` です(シナリオは値ではなく「違うこと」を見ます)。

✅MT-1s の `gui_material_from_blank`(json の step は 91。✅MT-2s で 190、✅MT-2s-b で 225)は、File > New Project > Blank pipeline から GUI だけで、箱の色を変えます(§13.1 の「Studio の Materials のドック」)。Add Object の Cube を置いて保存し、Frame Plan の **Add material pass…** で Forward opaque の pass `forward_opaque`(Create missing targets で `scene_color`、forward の既定にする)を足して engine の起こし直しを待ち、`scene_color` を `present` の可変の帯へ drag して、viewport に箱が出る(色が 64 以下で、中央が隅の背景とも前のグラデーションとも違う)ことを見ます(`add_primitive_object` と同じ道。✅GL-5c から、Add material pass... は Graph の palette の `forward_opaque_v1` で、`present` へは pass の color の出力を `+ input` へ drag します。json の step は 225 のまま)。**View > Materials** でドックを前に出し、`list_materials` の文書が 0 件であることを見てから **New material…** を押し、Surface で `openpbr / opaque_single_sided` を選びます。名前は打たずに `material`、path は `materials/material.material.json` です。Create で engine の一覧にその文書が `registered: true` で入り、エディタの見出しがそれを示します。`base_color` の swatch で色のダイアログを開き、hex に `#ff0000` を打って OK で、文書の `base_color` が `[1.0, 0.0, 0.0, 1.0]` になります。Outliner で Cube を選び、Inspector の Material の範囲が `This object` で、combo が `(model default: pelican.primitive.default)` であることを見てから(✅MT-2s)、範囲を `Every Cube` に替え、combo が `Model default` で、その下の 1 行が `Applies to every Cube in this project (1 object, 0 with their own material)` を言うことを見て、combo で `material` を選ぶと、`get_material_bindings` の primitive 0 がそれになり、**起こし直さずに**箱の中央が赤寄り(`rgb` の赤が緑と青より 40 以上大きい)になり、Restart engine のボタンが出ていないことを見ます。続けて `specular_roughness` の spin に `0.8` を打って Return で文書がその値を持ち、ドックの Undo を 2 回押すと、1 回目で roughness が、2 回目で赤が文書から外れ(文書に `base_color` が無い。割り当ては残したまま)、箱の中央が赤寄りでなくなります。打つ欄は New Project のフォルダと名前 `material_project`、色の hex の `#ff0000`、roughness の `0.8` です([Materials のドックのシナリオ](../../test/studio_scenarios/gui_material_from_blank.json))。実装者の run では緑でした(報告は 93 step と数え、23.7 s、`PELICAN_STUDIO_SCREEN=1`)。報告の画素は、箱の中央が割り当て前 `#756f63`、割り当て後 `rgb(74, 1, 0)`、Undo の後 `rgb(66, 62, …)` です。`restart_required` の道、テクスチャ、primitive ごとの行、拒否はこのシナリオでは通していません。

✅MT-2s は `gui_material_from_blank` を延ばし、object ごとの割り当てを通します(§13.1 の「Studio の Materials のドック」の「割り当て」)。ドックの Undo の後、Outliner で Cube を選び直して Inspector の model の欄が `Cube (built-in)` を読み、汎用の list の行(`pelican.inspectorListRows`)が 0 であることを見ます(MT-2 の後に欄が出なくなっていた件の直し)。New material… をもう一度押し、名前は打たずに `material_2` を作って swatch で `#ff0000` にし、Every Cube は `material`(灰)のままであることを見ます。Add Object で 2 つ目の Cube(`Cube_001`)を足し、Transform の x に `1.1` を打って横に置くと、範囲は `Every Cube` のままで、1 行が `2 objects` を言います(新しい revision で読み直す)。✅MT-2s-b からは、ここに Every Cube の Model default と行の Undo の手順が入ります(下の段落)。範囲を `This object` にして combo が `(model default: material)` を言うことを見てから `material_2` を選ぶと、`get_material_bindings` の `object_overrides` が 1、combo が `material_2 (this object)`、model の割り当ては `material` のままです。MainCamera を選んで、**起こし直さずに** viewport の x 0.88 の点(Cube_001)が赤寄り(赤が緑と青より 40 以上大きい)で、中央(Cube)が赤寄りでないことを見ます。Cube_001 で `Every Cube` にすると 1 行が `Applies to every Cube in this project (2 objects, 1 with their own material).` で、`This object` に戻すと Cube_001 は `material_2 (this object)` のままです。**Use model default** で `object_overrides` が 0、combo が `(model default: material)` になり、Inspector の Undo(scene の Undo)で `object_overrides` が 1 に戻り、combo が `material_2 (this object)` に戻ります。File > Save Scene で保存した scene に `material_2` があり、別の project を開いてからこの project を開き直すと、`object_overrides` が 1、model の割り当てが `material` で、Cube_001 が赤寄り、Cube が赤寄りでないことを見ます。新しく打つ欄は Transform の x の `1.1` と、2 つ目の色の hex の `#ff0000` です。実装者の run では緑でした(報告は 192 step と数え、37 s、`PELICAN_STUDIO_SCREEN=1`)。報告の画素は、2 つ並んだときの Cube_001 が `rgb(74, 1, 1)`、Cube が赤 66・緑 62 です。MT-2s の着地時、シナリオは「Every Cube を Model default に戻す」を使わず(engine の基本形で断られたため)、Cube_001 が自分の `material_2` を持った後に Every Cube を `material_2` にして両方の箱の赤を見てから `material` に戻していました。この手順は ✅MT-2s-b で下の手順に置き換わりました。primitive が複数ある model、拒否、missing の印はこのシナリオでは通していません。

✅MT-2s-b(`9d15ddc0`)は `gui_material_from_blank` に、engine の MT-2b の後の基本形の Every Cube の Model default と Material の行の Undo を足しました(json の step は 225)。Cube_001 を足して 1 行が `2 objects` を言った後(step 120。範囲は `Every Cube`、model は `material`)、`material_2` を選ぶと model がそれになり、MainCamera を選んで中央(Cube)と x 0.88 の点(Cube_001)の両方が赤寄りであることを見ます(step 122〜133)。Cube_001 を選び直して combo で **Model default** を選ぶと、`get_material_bindings` の `material_bindings` が `null`、primitive 0 の material が `pelican.primitive.default` になり、combo が `Model default`、行の Undo が `Undo Model default on every Cube` を言います(step 139〜144)。両方の箱が赤寄りでなくなり(step 147〜150)、行の Undo を押すと model が `material_2` に戻り、Undo の表示が 1 つ前の `Undo material_2 on every Cube` になり、両方がまた赤寄りです(step 155〜167)。続けて Every Cube を `material` に戻し(step 172〜173)、上の This object の手順に入ります。新しく打つ欄はありません。実装者の run では緑でした(報告は 227 step と数え、40.7 s、`PELICAN_STUDIO_SCREEN=1`)。報告の画素は、両方が赤のとき `rgb` ≒ (74, 1, 0)、Model default の後の灰が赤 117・緑 111 です。名前で言う拒否の 1 文と `changed: false` の道は GUI から作れないので、このシナリオでは通していません(test だけ)。

✅MT-3s(`19be8983`)は `gui_material_from_blank` に、engine の MT-3 の `rebuilt` を 1 つ足しました(json の step は 233)。Every Cube を `material_2` にして `get_material_bindings` の primitive 0 がそれになった後(step 123)、`get_status` の `instance_id` を覚え、ドックの見出しが `material_2` のままであることを見てから、Tags の `+ new tag` の行に `shiny` を打って Return します(`edit_material` の `tags` 1 回)。`get_material` の文書の `tags[0]` が `shiny` になり、ドックの notice が `Applied; rebuilt` を含み、Restart engine のボタンが出ず、`instance_id` が覚えた値と同じ(起こし直していない)ことを見ます(step 124〜131)。その後は MT-2s-b の手順(両方の箱が赤)へ続きます。新しく打つ欄は tag 名の `shiny` です(UX-11 の tag を打つ唯一の場所)。実装者の run では緑でした(報告は 235 step と数えています)。仕様は surface か tags を変える手順でしたが、surface を double-sided の variant に替えると engine が routing の variant と合わないと断ったので、tag にしました(上の「MT-3s の残した穴」)。

✅SK-1(`67fd3d29`)は `gui_material_from_blank` に、engine の VR-1 の段を足しました(json の step は 253。足したのは 20 手で、それより後ろの手の番号は上の MT-2s-b / MT-3s の段落の数より 20 大きくなります。例: MT-3s の tag の段は step 144〜151)。ドックの Undo で赤が文書から外れ、箱が赤寄りでなくなった後(step 93)、`get_status` の `instance_id` を覚え、Surface の欄が `openpbr / opaque_single_sided` で Double sided に印が無いことを見てから(step 94〜96)、Surface の欄で `openpbr / opaque_double_sided` を選びます(`edit_material` は surface だけ)。`get_material` の `surface` が `engine://surfaces/openpbr/opaque_double.surface`、`routing.double_sided` が `true`(engine が manifest の組で補った)になり、notice が `Applied; rebuilt` を含み、Restart engine のボタンが出ず、`instance_id` が同じであることを見ます(step 97〜102)。ドックの Double sided に印が付き(自動化面の `list` の `checked`)、Alpha mode が `opaque`、Surface の欄が double-sided のままです(step 103〜105)。ドックの Undo(前の surface と routing を送る)で、`surface` が `engine://surfaces/openpbr/opaque_single.surface`・`routing.double_sided` が `false` に戻り、Double sided の印が外れ、Surface の欄が one-sided に戻り、Undo が空になり、`instance_id` は同じままです(step 106〜113)。その後は Outliner で Cube を選ぶ MT-2s の手順へ続きます。新しく打つ欄はありません。実装者の run では緑でした(44 s、`PELICAN_STUDIO_SCREEN=1`)。Materials のドックのコードは変えていません。Double sided の check の状態をシナリオから読む口が無かったので、SK-1 は `list` に `checked` を足しました(下の「`list` の `checked`」)。Alpha mode の欄から routing だけを送る道はこのシナリオでは通していません。

✅GL-5b の `gui_graph_new_target`(json の step は 60)は、File > New Project > Blank pipeline から GUI だけで、Graph タブの中で render target を宣言して使います(上の「New render target を Frame Plan と Graph で 1 つの関数に(✅GL-5b)」)。Graph タブの palette を `target_ref` に絞り、target の combo で **New render target…** を選ぶと note が `OK opens New render target` を言い、Return で palette が閉じて New render target の form が名前 `render_target` 入りで開きます(打ちません)。種類で Explicit format を選んで format が `R16G16B16A16_SFLOAT` であることを見て OK を押すと、`get_render_authoring_context` の `render_targets[0]` が `render_target` になり、`render_target_ref` の node が置かれ、文書の `nodes[0].parameters.name` がそれを名指します。続けて palette を `material_pass` に絞り、contract `forward_opaque_v1` で置くと engine が起こし直し(route default)、draw ループのゾーンが描かれます。`render_target_ref` の resource の出力を material pass の color のソケットへ drag して文書の `nodes[4].inputs.color.node` が `render_target_ref` になり、同じ出力を `present` の `+` 入力へ drag すると `Pass inputs saved.` と、target を書く pass から `present` への線(`render_material_pass->present`。GL-1f)が出て、中央の画素がグラデーションと違います。Graph タブの Undo でその線が消え、画素が前と同じに戻ることを見ます。打つ欄は New Project のフォルダと名前 `graph_target`、palette の絞り込みの `target_ref` と `material_pass` です([Graph で New render target のシナリオ](../../test/studio_scenarios/gui_graph_new_target.json))。実装者の run では 3 回目に緑でした(23.6 s。1 回目は既定の Scene color の target を material pass の color に繋ぐ編集が format mismatch で断られ、form で Explicit format を選ぶ手順を足した。2 回目は `present` への線の名前の書き違い)。報告の画素は、中央が `#bcbcbc` → `#000000` → Undo で `#bcbcbc` です。詳細欄の出力の付け替えと `New target: …` はこのシナリオでは通していません(unit test だけ)。既存の `gui_folded_pass_edit` と `graph_palette_kinds` も、実装者の run で緑でした。

✅GL-5c の `gui_frameplan_delegates`(json の step は 69)は、File > New Project > Blank pipeline から GUI だけで、Frame Plan の入口が Graph へ渡ることを見ます(上の「graph root では Frame Plan の Add を Graph の palette に(✅GL-5c)」)。Add Object の Cube を置いて Save Scene し(material pass が求める起こし直しで問いが出ないように)、`MainCamera` を選んで置く前の絵を撮ります。Frame Plan のタブで toolbar の **Add material pass...** を押すと、material pass の form ではなく Graph タブの palette が出て、絞り込み欄が `render.material_pass` で、contract の combo が出ます。`forward_opaque_v1` を選んで Return で置くと、form は開かず、engine の起こし直し(route の既定)の後に draw ループのゾーンと `render_material_pass` が描かれ、詳細欄の Contract が `forward_opaque_v1` を示します(置いた node が選ばれている)。`get_graph_instances` の `nodes[3]` が `render.material_pass` の `render_material_pass`、`get_render_authoring_context` の `managed_fragments` が 0 件、root のファイルが `render.material_pass` を持つことを見ます。material pass の `color` の出力を `present` の `+ input` へ drag して、`Pass inputs saved.`、実線 `render_material_pass->present`、viewport の中央がグラデーションと違うことを見ます。Frame Plan に戻って `present` の行の見出しをダブルクリックすると、Graph タブの詳細欄の出力の行(`pelican.graphCanvasFoldedPass.output.0`)と要約の `From the rendering section: edit it here.` が出て、Pass Editor の名前の欄は `present` になりません。material pass の行をダブルクリックすると、その node の Contract の欄が出て `present` の欄は消えます。最後に、選んだ forward の pass の横で Graph の **Add** から palette を `target_ref` に絞り、New render target… で form を開くと、種類が `Explicit format`、format が `R16G16B16A16_SFLOAT` で開くことを見て Cancel し、何も宣言されないことを確かめます。打つ欄は New Project のフォルダと名前 `delegates`、palette の絞り込みの `target_ref` です([Frame Plan の委譲のシナリオ](../../test/studio_scenarios/gui_frameplan_delegates.json))。実装者の run では 2 回目に緑でした(1 回目は「Pass Editor が開かなかった」を名前の欄の visible で見ていたが、Pass Editor の dock は既定の layout で常にタブとして見えているため、「名前が `present` でない」に直した)。報告の画素は、中央が `#bbbcbc` → `#756f63` です。preset root の `view_select_field`(Frame Plan のダブルクリックで Pass Editor)は変えずに緑でした(実装者の報告)。

✅GL-5c で期待を変えた既存の 15 本は次のとおりです。書き先が「Frame Plan の form → managed fragment `pass-<id>.json` と宣言した target」から「Graph の palette → graph の `nodes`(pass は `render_material_pass` / `render_fullscreen`、出力は自前の image `render_material_pass__color` など)」に変わりました。実装者の run では 15 本とも緑です。下の各シナリオの段落は着地時の手順で書いてあり、GL-5c の変更はこの表が正です(json の step は GL-5c の後の数)。

| シナリオ | 変えた所 | json の step |
|---|---|---|
| `add_primitive_object` | Add material pass... の form(Create missing targets、forward の既定)→ palette の `forward_opaque_v1`。Frame Plan の `forward_opaque` の出力から `present` への drag → Graph の `render_material_pass/out:color` から `present` の `+ input` へ。`managed_fragments` が 0 件であることを見る | 57 → 56 |
| `forward_loop_from_scratch` | 同上。`present` の入力は Pass Editor(ダブルクリック)ではなく Graph の drag。期待の名前 `scene_color` → `render_material_pass__color`、`forward_opaque` → `render_material_pass` | 90 → 88 |
| `gui_material_from_blank` | `add_primitive_object` と同じ置き換えと、後で Frame Plan を前に戻す 1 手 | 225(変わらず) |
| `material_pass_authoring`(fixture、direct root) | form のシナリオだったが、この root では Add が palette なので palette に。`scene_color` の宣言の確認 → 自前の image。`render-targets.json` は作られない | 44 → 38 |
| `frameplan_layout_then_edit` | Blank の側の Add material pass... を palette(`deferred_geometry_v1`)に。Q-13(配置の commit の直後の編集が新しい digest を使う)は Graph の編集で見る | 42 → 49 |
| `input_needs_shader` | `present` の shader は Pass Editor ではなく、ダブルクリックで開く Graph の詳細欄の Fragment で選ぶ。Deferred geometry は palette。drag 元は `render_material_pass/out:render_material_pass__albedo` | 69 → 70 |
| `sockets_from_shader` | 同上。`two.frag` を選ぶのも Graph の詳細欄 | 93 → 95 |
| `sockets_fix_and_show` | 同上。Fix sockets が作る宣言の名前は `in_render_material_pass_albedo` / `in_render_material_pass_normal`(資源名の `__` が `_` に詰まる) | 94 → 97 |
| `deferred_loop_from_scratch` | G-buffer は palette。lighting は Frame Plan の Add fullscreen pass...(Graph が `render_fullscreen` を置いて選ぶ)と Graph の pass の欄の New… > Deferred lighting(✅UX-5a で G-buffer の線が自動で引かれる)。`present` は Tone map の雛形をやめ、lighting の出力を Graph で `present` の `+ input` へ(passthrough) | 118 → 100 |
| `frameplan_edit_roundtrip`(fixture) | 最後の Add fullscreen pass...: 「Pass Editor が anchor に立つ」→「Graph に `render_fullscreen` が置かれ pass の欄が開く」 | 29 → 31 |
| `fullscreen_pass_from_template`(fixture) | Frame Plan の Add fullscreen pass... → **View > Pass Editor** と Pass Editor の **New pass**・Refresh・position(lighting_pass の後の anchor)。後の Pass Editor の手順は同じ(managed fragment に書く) | 47 → 49 |
| `lighting_pass_template`(fixture) | 同上 | 41 → 43 |
| `render_pipeline_from_scratch` | `draw_pattern` の追加は同上。`present` の入力(F-6)は Pass Editor ではなく Frame Plan のソケットへの drag(Whole frame にして `draw_pattern/out:scene_color` → `present/in:variadic`) | 97 → 93 |
| `render_undo_roundtrip`(fixture) | `present` の fragment(2 回)は Pass Editor ではなく Graph の詳細欄(RU-1 の Undo / Redo は Frame Plan の道具の列のまま通り、`Undid "Edit pass present".` も同じ)。`ru1_tint` の追加(2 回)は View > Pass Editor と New pass・Refresh・position | 127(変わらず) |
| `play_mode_roundtrip` | 再生中の `present` の fragment は Pass Editor ではなく Graph の詳細欄。帯は Pass Editor の `renderBanner.passEditor` ではなく render tools の `renderBanner.renderTools` | 118 → 117 |

deferred の material pass を palette で置くシナリオのうち 3 本は、engine の起こし直しが未保存の scene の問いで止まって赤になったので、問いに Save で答える手順を足しました(実装者の報告。どの 3 本かは報告に名前がありません。上の GL-5c の段落の「分かっている穴」)。repo の fixture の root も graph で preset を選ばないので、仕様が想定した Add material pass... の 9 本より多く、Add fullscreen pass... を使う 6 本と、`present` のダブルクリックで Pass Editor を開いていた 7 本も期待が変わりました(実装者の報告)。コミットの文と実装者の報告は「16 本」と書いていますが、`git show 9cd89c5c` で変わった既存の json は上の 15 本です(16 本目は新しい `gui_frameplan_delegates`。報告の表の `view_select_field` は確認だけで変えていません)。
→ `git show 9cd89c5c` の json では、問いに Save で答える手順が入ったのは `input_needs_shader`・`deferred_loop_from_scratch`・`sockets_from_shader`・`sockets_fix_and_show` の 4 本です(`forward_loop_from_scratch` の同じ手順は GL-5c の前からあり、route の既定による起こし直しのためです)。

✅MC-1s(`1d10bd4c`)で、Graph の palette の G-buffer の image は Blank では SDR の表の format になり、engine は起こし直さなくなりました(上の「render.material_pass と draw.* の node(✅LP-4e)」)。GL-5c が再起動を前提にした 3 本を戻しました: `input_needs_shader`(70 → 71 step。`instance_id` が置く前と後で同じこと、albedo の image の format が `B8G8R8A8_SRGB`)、`deferred_loop_from_scratch`(100 → 97)、`frameplan_layout_then_edit`(49 → 50。ゾーンの待ちを 60 秒に)。どれも viewport の status が `Restarting the engine` を含まないことを見ます(各シナリオの段落の MC-1s の文)。実装者の run では、この 3 本と、変えていない `gui_frameplan_delegates`(forward。route の既定による起こし直しはそのまま)が緑でした(順に 21 s・33 s・105 s・22 s、`PELICAN_STUDIO_SCREEN=1`、1 本ずつ)。**`sockets_from_shader` と `sockets_fix_and_show` は MC-1s で変えておらず、Blank で palette の `deferred_geometry_v1` を置いた後に、未保存の scene の問いを 120 秒待つ手順が残っています。**MC-1s の後は起こし直しが起きないので、問いが出ずにこの待ちで赤になりました(統合側の run。2 本とも step 27 で `pelican.unsavedScenePrompt` を 120 秒待って `automation.wait_timeout@1`)。→ ✅SK-1(`67fd3d29`)で 2 本も戻しました: `sockets_from_shader`(95 → 94 step)と `sockets_fix_and_show`(97 → 96)。再起動の 4 手を外し、`input_needs_shader` と同じく置く前後の `instance_id` が同じことと、viewport の status が `Restarting the engine` を含まないことを見ます(各シナリオの段落の SK-1 の文)。実装者の run では 2 本とも緑でした(どちらも 28 s、`PELICAN_STUDIO_SCREEN=1`、1 本ずつ)。

✅BS-1s の `gui_render_fallback`(json の step は 62)は、起動時に組めない構成の project を開いても Studio の engine が止まらず、GUI で直せることを見ます(上の「組めない構成でも engine を止めない(✅BS-1s)」)。準備の `gui_render_fallback.prepare.py` が `pelican_cli project init --template blank` で `{artifacts}/incoming/` に 2 つの project を作ります。`broken` は root(`passes/main_rendering_config.json`)の `nodes` に、buffer の port に `element` の無い `render.compute` の node `sim` を 1 行で書いたもの(CT-1 の例。engine の BS-1 の test と同じ形)、`unclosed` は root の最後の `}` を落としたものです。
- **構造の拒否**(step 1〜35): `broken` を開くと engine は止まらず、帯の文が `Showing a placeholder: the rendering setup was refused — Port data of sim needs element (graph.port_field_missing@1) in passes/main_rendering_config.json. Fix it, then Reload from disk.` に一致し、`get_status` の `render_pipeline.state` が `fallback`・`refusal.code` が `graph.port_field_missing@1`、engine が frame を出し続け、Frame Plan の status 行も同じ文であることを見ます。Reload from disk・Open in Graph・Open file が見え、Restart engine は見えず、viewport の中央が赤 > 緑で中央と隅が同じ赤であることを見て撮ります。Open in Graph で Graph タブに `sim` の node が出ます。壊れたまま Reload from disk を押すと Graph の notice が `Could not load` を言い、state は `fallback` のまま、帯も残ります。`file` step で `sim` の行を root から消す(外のエディタで直す人の代わり)と、Graph の notice が `Picked up the change to main_rendering_config.json.` を言い、state が `ready` になり、帯が消え、Frame Plan に `present` が描かれ、viewport が Blank のグラデーション(右上が左上よりずっと赤い)に戻ります。
- **JSON でない root**(step 36〜62): `unclosed` を開くと、帯が `passes/main_rendering_config.json is not valid JSON at line ` と `(render_document_syntax@1)` を含み、engine の `refusal.code` も同じで、Show the document が見え Open in Graph は見えません。Show the document で窓が開き、notice が `Not valid JSON yet at line ` を含み、Save が押せないことを見ます。Ctrl+End で末尾に行き `}` を打つと notice が `Valid JSON` を含み Save が押せ、Save で窓が閉じ(`restore_render_documents` が commit)、state が `ready`、帯が消え、ディスクの root が `  }\n}` を含み、`present` が描かれ、グラデーションに戻ります。
- 打つ欄は窓の `}` だけです(project は準備が作るので New Project の欄はありません)。実装者の run では 64 / 64 step で緑でした(報告の数え方。`PELICAN_STUDIO_SCREEN=1`、-j1)。再起動の案内の道(決定 4)はこのシナリオでは通していません(test だけ)。仕様は「直した後に Reload from disk を押す」でしたが、ファイルを直すと RL-1s の見張りが先に読み直して `ready` に戻るので、Reload from disk は壊れたままの段で押して、断られて帯が残ることを見ています(実装者の報告)。`test/studio_scenarios/README.md` に、帯と窓の名前と、壊れた project を準備の `<name>.prepare.py` で作ることを書きました。
✅PB-1s で、step 5 の帯の文の `text_equals` の期待の末尾に ` See Problems.` が付きました(PB-1s の実装者の run で 64 / 64 step 緑、報告の数え方)。

✅PB-1s の `gui_problems_window`(json の step は 79。PB-1s の着地時は 64、✅RR-1s で延びました)は、Problems のドック(§13.1 の「Studio の Problems のドック」)を File > New Project > Blank pipeline から GUI だけで使います([Problems のシナリオ](../../test/studio_scenarios/gui_problems_window.json))。
✅RR-1s から、reload に断られた版を持つ間の帯(上の「reload に断られた版を持つ間(✅RR-1s)」)もこのシナリオで見ます。

- **開く**(step 1〜15): New Project で Blank を作り、**View > Problems** でドックが前に出て、status bar の `pelican.problemsCount` と `.count` が `No problems` です。
- **JSON でない root**(step 16〜39): `file` step で root(`passes/main_rendering_config.json`)の最後の `}` を落とすと、RL-1s の読み直しが断られ、`.count` と status bar が `1 error` になり、
  `get_problems` の 1 件目は source `reload_rpc`・code `render_document_syntax@1`、engine は前の版で `ready` のままです。
  ✅RR-1s から、帯の文が `passes/main_rendering_config.json is not valid JSON; the Graph shows the last version that was. See Problems.` に一致し、`get_graph_instances` の `held_unparsed` が `true` です。
  行を選ぶと `.detail` が `passes/main_rendering_config.json:` と `is not valid JSON at line ` を含み、Show the document が開いて notice が `Not valid JSON yet at line ` を言います。
  Ctrl+End で `}` を打ち、Save(✅RR-1s から `get_render_documents` の digest を base にした restore。Studio はファイルを書きません)で窓が閉じ、
  `.count` が `No problems`、`get_problems` が 0 件、選択も消え、帯が消えて `get_graph_instances` の `live` が `true` です。
- **node の壊れ**(step 40〜59): buffer の port に `element` の無い `render.compute` の node `sim` を root に書くと、読み直しの拒否が 1 行(`get_problems` の `node` が `sim`、`document_kind` が `root_graph`)になります。
  ✅RR-1s から、帯が `Showing the saved version; the picture is still the previous one — ` と `— Port data of sim needs element` を含み、`get_graph_instances` は `live: false` で `nodes[0].id` が `sim`、`get_status` の `render_pipeline.state` は `ready`(絵は前の版)です。
  行を選ぶと `.detail` が `node sim` を含み、Graph タブに `sim` の node が見えます(ディスクの版。`gui_problems_window_held.png` を撮る)。Graph で `sim` を押して Delete すると(断られた版の digest での構造の編集)、
  Graph の notice が `Graph change saved.`、`get_graph_instances` が `live: true` で node が 0 件になり、status bar が `No problems`、帯が消えます。
- **起動時の fallback**(step 60〜74): `sim` を外からもう一度書いて 1 行に戻し(RR-1s で足した 2 step)、別の project を開いてから壊れたままの project を開き直すと、engine は止まらず帯が出て、文が `Fix it, then Reload from disk. See Problems.` を含み、
  status bar が `1 error`、`get_problems` の source が `startup` です。件数のボタンを押すと Problems が前に出て、行を選ぶと Graph タブに `sim` の node が見えます。
- **直す**(step 75〜79): `sim` を root から外して保存すると、`render_pipeline.state` が `ready` に戻り、行と帯が消えます。
- PB-1s の着地時の形(64 step)では、JSON でない root の Save は Studio がファイルを書き、node の壊れの行は Show the document の文字の位置で開いて Cancel で閉じていました。
- 打つ欄は New Project のフォルダと名前、Show the document の窓の `}` です。✅RR-1s の実装者の run では 1 回目に緑でした(81 / 81 step、報告の数え方。27.5 秒、`PELICAN_STUDIO_SCREEN=1`、-j1)。
  PB-1s の着地時の run は 3 回目に緑でした(66 / 66 step、報告の数え方。26.0 秒、`PELICAN_STUDIO_SCREEN=1`、-j1)。
  1 回目は窓の Save が通らず(engine の restore が reload で断られた後の root を断る。§13.1 の Problems のドックの「仕様から外れた判断」の 2 で直した)、
  2 回目は保存した文字の末尾の改行の数で「disk の root が閉じる」の確認が外れた(シナリオの誤りで、その step は外して `get_problems` の 0 件で見る)そうです(実装者の報告)。
  再起動の案内の行はこのシナリオでは通していません(test だけ)。

**画素を読む点と窓の大きさ(✅PX-1、`e3627606`)**: 統合側の全シナリオの run(2026-10-04、develop `67fd3d29`、69 本中 67 本が緑、42 分)で、`gui_build_C` の step 162 と
`gui_settings_choose` の step 83 が画素の比べで赤でした。絵は正しく、変わったのは viewport の縦横比です(実装者の報告)。

- Studio の窓の高さが伸び(実装者の計測で 1175 → 1278 px)、viewport が緑の run の 700x426 / 725x426 から 731x566 / 702x566 になりました。
  camera の縦の画角は固定なので、箱と影の位置は画の高さを単位にすると変わりませんが、シナリオの点は画の**幅**に対する割合(上の screenshot の `pixels`)なので、
  viewport が縦に伸びると「箱の横」に置いた点が箱の中に入りました。
- run は窓の大きさを固定しません(実装者の報告: 窓を置く所は位置だけを合わせ、`resize(1280, 800)` は dock の最小の大きさで押し広げられる)。同じ run の中でも窓の高さはシナリオごとに 1101〜1319 px と違いました。
  どの dock の変更で最小の高さが増えたかは切り分けていません(未確認)。
- `gui_settings_choose`: 影と照らされた床を読む点を `(0.325, 0.695)` / `(0.675, 0.695)` から `(0.27, 0.71)` / `(0.73, 0.71)` に選び直しました(15 か所。step 数は 206 のまま)。
  画の高さの単位では箱の外の影の中に当たり、縦横比が約 1.15:1〜1.7:1 の間なら影の側 / 照らされた側に落ちます。光を反転した後の右の影と cascade の確認(step 205)も同じ点を読みます。
  json の description に、この選び方の理由を 1 文足しました。
- `gui_build_C`: bloom の広がりを、真ん中の列の画の高さ 0.25 と 0.75(箱の上下の縁の 4〜5 px 外)で読むようにしました。広がる前はそこが隅の色(黒)と同じこと、広がった後は明るいことを見ます。
  箱の左右の点 `(0.655, 0.5)` / `(0.345, 0.5)` は外しました(左右の縁は縦横比で動き、広がりは 8〜24 px しかないので、固定の割合では選べない。実装者の報告)。step 数は 166 → 164 です。
  description の「four edges」も上下と理由に書き換えました。
- Studio と engine のコードは変えていません。実装者の run(`PELICAN_STUDIO_SCREEN=1`、1 本ずつ -j1)で `gui_settings_choose`・`gui_build_C`・`gui_build_D`・`gui_build_full` が緑でした。
- 残した穴(実装者の報告): `gui_build_D` / `gui_build_E` / `gui_build_full` / `gui_build_optimize` の影の点 `(0.31, 0.7)` / `(0.69, 0.7)` は、731x566 で箱の縁から 2〜3 px しか離れていません。
  根本(run が窓と viewport の大きさを固定しない)は PX-2 で扱います(統合側の予定。この文書の時点で未着手)。
  → ✅PX-2(`28bc5ef9`)で run は viewport を 720x450 に固定します。この 4 本の影の点は、720x450 で影の中の縁から 14 px、照らされた床で 28 px、箱の縁から 29 px です(下の「viewport の大きさを決める(✅PX-2)」)。

**viewport の大きさを決める(✅PX-2、`28bc5ef9`)**: 上の PX-1 の根本(run が窓と viewport の大きさを固定しない)を直しました。シナリオの run は、engine の viewport を **720x450**(画の画素)に固定します。

- **命令 `viewport_size`**: `{"cmd": "viewport_size", "width": 720, "height": 450}` は、`pelican.viewportHost`(engine の窓を埋め込む native の widget。`name` で別の widget も指せます)に固定の大きさをかけ、
  窓をその差の分だけ大きく / 小さくします([`commandViewportSize()`](../../src/devstudio/automation/automationcommands.cpp#L1969))。周りのドックの大きさはそのままで、
  後でドックを開け閉めしても、場所はドックか窓から取られ、viewport からは取られません(unit test)。窓が最大化か全画面のときは窓の大きさを変えません(コードの読み)。
  - width / height は画の画素(物理画素。engine が描く大きさで、`screen` の screenshot が測る単位)で、widget の devicePixelRatio で割って Qt の論理画素にします。
  - widget がその大きさになるまで待ってから答えます(既定 5 秒、`timeout_ms`)。答えは `name`・`fixed`・engine に渡る大きさの `width` / `height`(viewport が engine に渡すのと同じ `embeddedViewportPixelExtent()` の丸め)・`logical`・`device_pixel_ratio`・`window` です。
  - 拒否: 大きさに届かなければ `automation.wait_timeout@1`(`the viewport stayed WxH instead of WxH`)、width / height が無いか 0 以下なら `automation.bad_request@1`、名前の widget が無ければ `automation.widget_missing@1` です。
  - `{"free": true}` は、最初に固定する前の最小・最大の大きさに戻します(窓の大きさは戻しません。コードの読み)。
- **driver**: `tools/studio_drive.py` は Studio とつないだ直後・最初の step の前に `viewport_size` を送り([`fix_viewport()`](../../tools/studio_drive.py#L1250))、report に `viewport` の step として残します。
  答えの大きさが要求から 1 画素を超えてずれたか、断られたら、その run は step を 1 つも送らずに赤です。既定は [`DEFAULT_VIEWPORT`](../../tools/studio_drive.py#L55) の `(720, 450)` です。
- **シナリオの `"viewport"`**: 最上位に `"viewport": [w, h]` と書くとその大きさ、`"viewport": null` で固定しません(`viewport_size` を送らない)。2 つの正の整数でなければ run は赤です。
  今 `null` を書くのは `whole_frame_first_steps` だけで、「Pass Editor を閉じると viewport の高さが戻る」を見るためです(固定するとその確認に中身が無くなる)。
  書式の正の [README](../../test/studio_scenarios/README.md) には `"viewport"` はまだ書かれていません(PX-2 の commit は README を変えていません)。
- **720x450 の理由**(実装者の報告):
  - 縦横比 1.6 は、多くの点が作られた 700x426(1.64)/ 725x426(1.70)に 16:9(1.78)より近く、幅の割合で書いた点の、高さ単位での動きが一番小さい。
  - 既定の layout が viewport に与えていた範囲(700〜731 x 426〜566)の中にあり、窓とドックがほとんど動かない。125 % の仮想モニタ(2560x1440、論理 2048x1152、作業領域の高さ 1104)では窓は既に画面の高さ近くあり、800x450 や 800x500 では窓が画面の下にはみ出しうる。
  - 125 % で割り切れる(論理 576x360 → 物理 720x450)ので、engine の framebuffer もちょうど 720x450 になる(Studio の状態行は `host 720x450 px, engine 720x450 px`)。
- **`screen` の screenshot は 719x449 です**(実装者の報告): engine は 720x450 で描きますが、viewport が窓の中で端数のある物理位置にあると、
  [`physicalScreenRect()`](../../src/devstudio/automation/automationcommands.cpp#L85) が丸ごと中にある画素だけを取る(左上は切り上げ、右下は切り捨て。コードで確かめた)ので、端の 1 画素が欠けます。
  点の割合から画素への写像は高々 1 画素ずれるだけで、点の選び直しはこの 719x449 の png で測りました。
- **中心からの座標は足していません**: 仕様は `pixels` に「画の高さを単位にした、中心からの座標」を足すかを判断させていました。大きさが固定されれば幅の割合の点も毎回同じ画素に落ちるので、
  座標の書き方を 2 つにしない、が実装者の判断です。
- **点の測り直し**(実装者の報告): 画素を読む 33 本(`grep -l '"pixels"' test/studio_scenarios/*.json`)を固定した大きさで流して 33 本とも緑でした。各 png で点ごとに、色と
  「その色から各 channel で 12 を超えて違う画素までの距離」を測り、距離 20 未満の点は png を見て意味を確かめました。直した点:
  - `gui_settings_choose`: `(0.27, 0.71)` / `(0.73, 0.71)` → `(0.31, 0.7)` / `(0.69, 0.7)`(30 か所)。旧い点は影の楔の端から 7 px、新しい点は影の中で縁から 14 px、照らされた床で 28 px です。
    `gui_build_D` / `E` / `full` / `optimize` と同じ点になりました。PX-1 が 0.27 / 0.73 を選んだのは縦横比が動くからで、固定したので影の真ん中に寄せました。description の 1 文も書き換えました。
  - `gui_build_C`: bloom の広がりの上下 `(0.5, 0.25)` / `(0.5, 0.75)` → `(0.5, 0.243)` / `(0.5, 0.756)`(6 step)。旧い点はにじみから 2〜3 px、新しい点はにじみからも広がりの先からも 5〜6 px です。description の 1 文も書き換えました。
  - `gui_build_full` / `gui_build_optimize` の bloom の段: 床の左右の縁の上の暗がり `(0.2, 0.56)` / `(0.8, 0.56)` → `(0.2, 0.57)` / `(0.8, 0.57)`(光の先から 2 px → 6 px)、
    覚えるだけの箱の右 / 左 `(0.655, 0.5)` / `(0.345, 0.5)` → `(0.69, 0.5)` / `(0.31, 0.5)`(箱の縁から 2〜3 px → 25〜29 px。比較には使っていない)。
  - 見たが直さなかった点(gizmo の輪の内側の穴を読む中央、45° 回した箱の稜線の上の中央、gradient の点、海の模様、`gui_build_ltc` の比較に使わない点、`play_pause_step` の枠の色)は、読む値が変わらないか比較に使わない、という理由です。
- **⚠ 既定の大きさを変えると、画素を読む 33 本の点を全部測り直すことになります。**点は 720x450 の絵の上で選んだ割合なので、`DEFAULT_VIEWPORT` を変えると全部の点が別の画素に落ちます。
  変えるなら、33 本の点を測り直す WP にしてください(実装者の報告の注意)。
- 実装者の run(`PELICAN_STUDIO_SCREEN=1`、1 本ずつ -j1): 画素を読む 33 本、点を直した 4 本(と点をもう 1 か所直した 2 本)、画素を読まない残り 37 本、つまり 70 本すべてが固定した viewport で緑でした。`ctest -L studio_cpu -LE gpu` は 723 / 723 です。
- test: `devstudio_automation_input_test` の [「PX-2: viewport_size holds the viewport while panels come and go」](../../test/devstudio_automation_input_test.cpp#L926)
  (左にドックのある `QMainWindow` で 300x200 に固定し、横のドックの幅が変わらないこと、下にドックを足しても窓を変えても閉じても 300x200 のままのこと、width / height の無い要求と無い名前の拒否、
  対照として `free` の後は窓に従って大きさが変わり最小が戻ること)と、`studio_drive_shutdown_unit`(`test/ci/test_studio_drive.py`)の [`StudioDriveViewportTest`](../../test/ci/test_studio_drive.py#L44) の 3 本
  (既定の大きさを送る / シナリオが自分の大きさか `null` を書ける / 大きさが入らない・形が違う・断られたら run を止める)です。既存の shutdown の test の mock は、`viewport_size` に大きさで答える形になりました。
- 手で打つ欄は増えていません(Studio の UI は変えていない)。

⚠ **2 つの注意。**(1) `open_project` に**今開いている directory** を渡すと何も起きません(`EmbeddedViewport::openProject`
がそう作られています)。ディスクから読み直させたいときは、別の project を開いてから戻ります。
(2) `screenshot` の既定(`widget` モード)は Qt の widget 木を描くので、**埋め込まれた engine の窓は写りません**
(別 HWND です)。engine が描いた絵が要るときは `"mode": "screen"` を使います。**✅F-5 から `screen` は、Studio の窓
自身の合成済みの内容を `PrintWindow`(`PW_RENDERFULLCONTENT`)で読んで切り出す**ので、別の application の窓が
viewport に重なっていても写りません([窓の内容の読み取り](../../src/devstudio/automation/automationcommands.cpp#L159))。
窓が print できないとき、または 1 色で出て**しかも desktop のその場所に Studio の窓が実際に見えているとき**だけ、以前の
desktop の grab に戻ります。見えているかは、撮る範囲の 5×5 の点で `WindowFromPoint` が Studio の窓(埋め込みの engine の窓を
含む)を指すかで見ます([desktop に窓が見えているかの判定](../../src/devstudio/automation/automationcommands.cpp#L215))。
別の窓が重なっていれば 1 色の print をそのまま使い、応答に **`desktop_obstructed: true`** を付けます。1 色は正しい絵でもあり得る
(消去されただけの target、空の scene の albedo)ので、重なった別の application の窓を撮るより print を残します(G-4 の後の修正
`5ca18ee1`。以前は 1 色なら必ず desktop に戻り、重なった窓を撮っていました)。
どちらで撮ったかは応答の **`capture`**(`print_window` / `screen`)に出ます。着地時に別の窓の重なりで 2 本のシナリオが
赤になったための変更です。
**倍率の違う 2 枚目のモニタでも同じ場所を撮ります(✅OT-1)。**OT-1 の前の `screen` は、widget の位置を画面の原点から論理 pixel で測って
窓の倍率を掛けていました。Qt の論理座標では各画面の原点は元の場所のまま中だけが拡大されるので、100 % の主モニタの右に 125 % のモニタがあると、
viewport の隣の panel を撮っていました。今は窓の client の原点から測った差にだけ倍率を掛け、辺ごとに四捨五入します(Qt と同じ丸め。
切り捨てると 125 % で隣の panel の 1 行が入ります。[`physicalScreenRect()`](../../src/devstudio/automation/automationcommands.cpp#L85))。→ 今のコードは、端数の倍率で縁が画素の中に落ちるとき、丸ごと中にある画素だけを取ります(左上は切り上げ、右下は切り捨て。コードの読み)。✅PX-2 の 720x450 の viewport が `screen` では 719x449 に写るのはこのためです(上の「viewport の大きさを決める(✅PX-2)」)。PrintWindow からの切り出しと、desktop に窓が
見えているかの判定も同じ矩形を使います。desktop に戻るときは、`QScreen::grabWindow` ではなく desktop を `BitBlt` で native に読みます
([`desktopRegion()`](../../src/devstudio/automation/automationcommands.cpp#L125)、[使う所](../../src/devstudio/automation/automationcommands.cpp#L2161))。それも読めなければ、窓のある画面の `grabWindow` を、その画面の左上からの座標で呼びます。
`screenshot` の応答は `path` / `mode` / `capture` / `width` / `height`(と、上の場合だけ `desktop_obstructed`)に加えて **`distinct_colors`**(絵に含まれる色の数)を返します
(✅F-2、[screenshot の distinct_colors](../../src/devstudio/automation/automationcommands.cpp#L2204))。消去されただけの viewport は 1 色なので、
`result` step で `distinct_colors` を読めば「engine が何か描いたか」を PNG を開かずに判定できます。
**`"pixels": [[x, y], ...]`**(✅F-4)を付けると、応答にその各点の色が `"#rrggbb"` の配列 `pixels` で返ります
([screenshot の pixels の読み取り](../../src/devstudio/automation/automationcommands.cpp#L2211))。x / y は絵の幅・高さに
対する 0〜1 の割合なので、窓の大きさを知らずに書けます。配列でない・2 要素でない・範囲外なら `automation.bad_request@1` です。
ただし run は窓の大きさを固定しないので、viewport の縦横比は dock の配置で変わります。camera の縦の画角が固定の 3D の絵では、物の左右の位置は幅の割合で動きます(✅PX-1。上の「画素を読む点と窓の大きさ」)。
→ ✅PX-2(`28bc5ef9`)から、run は viewport を 720x450 に固定するので、幅の割合の点も毎回同じ画素に落ちます(上の「viewport の大きさを決める(✅PX-2)」)。
`result` step の `remember` で 1 点の色を覚え、別の点を `value_of` と `op`(`eq` / `ne`)で比べれば「同じ縞の下も同じ色、隣の縞は違う色」の
ように、output transform がどんな色にしても絵の形を判定できます。
✅MT-1s から、同じ点の色を `[r, g, b]`(0〜255)で並べた **`rgb`** も返ります([rgb を足す所](../../src/devstudio/automation/automationcommands.cpp#L2226)。
`pixels` はそのまま)。`rgb[0][1]` を `remember` し、`rgb[0][0]` を `op: gt` と `plus` で比べれば、色の濃さを知らずに「赤が緑より 40 以上大きい」
を言えます(`gui_material_from_blank`。シナリオの README の screenshot の段落)。

**この面が最初の往復で本物のバグを見つけました。**値の帯を click → type → Enter すると「Graph change saved.」と
出るのに古い値が保存される —— `QDoubleSpinBox` の `keyboardTracking=false` と、editor の filter が Enter を先に
消費する組合せで、scene test の合成 Enter では通っていました。修正は `commitInlineEdit()` で `interpretText()` を
呼ぶことです([修正](../../src/devstudio/view/graphcanvaswidget.cpp#L3851)) —— **`--automation` 無しでも挙動が変わる
唯一の変更**で、手で編集したときの取りこぼしもこれで直っています。

### Inspector のグラフ装着

Inspector の `Add component...` / `Ray geometry...` の下、**properties の scroll 領域より上**に
**Attached graphs** グループがあります([置き場](../../src/devstudio/view/inspectorwidget.cpp#L298)。
properties は [:219](../../src/devstudio/view/inspectorwidget.cpp#L394) の `pelican.inspectorProperties` です)。
選択中のオブジェクトを owner とする `graph.instance` の一覧と、**Attach...** / **Detach** / **Refresh** です。

**装着する**: **Attach...** で `Attach graph` ダイアログが開きます。`Definition` で definition を選ぶと、
`Instance name` に重複しない候補が入り、その definition の `graph.input` ごとに数値欄が並びます
(既定値は宣言の `default`、無ければ 0)。**Refresh source** は文書を取り直し、**Attach and save** が
`edit_graph_instances{attach}` を 1 回送ります。欄が数値でなければ送る前に止まります。

**入力を見る**: 一覧で instance を選ぶと **Instance inputs** グループが開き、port ごとに 3 つの表示が並びます。

| 列 | 意味 |
|---|---|
| mode | その port に**今どこから値が来ているか**。`connected`(辺)> `parameter`(文書に書かれた値)> `default`(definition の宣言)の順で決まります([判定](../../src/devstudio/model/graphattachmentmodel.cpp#L344)) |
| 値の欄 | **著作された値**(`parameters`、無ければ宣言の `default`)。編集できるのは値を持つ port だけです |
| live | **描かれた世代が実際に使った値**と、その出所を `<値> (<source>)` で。publish 前は `not published` |

**mode が `connected` でも、著作された値は消えていません。**接続を外せばその値に戻ります —— これが
「二重指定を拒否する」のではなく「優先順で決める」設計にした理由です。

**4 つの導線**(port ごとのボタン):

1. **Control with a curve** —— 前節のノード集合を作って繋ぎます(`edit_graph_document`)。
2. **Connect to another output** —— `Connect input` ダイアログで別ノードの出力を選び、**Connect** で `set_input`。
3. **Open the source** —— Graph タブを前に出し、その producer を選択します
   ([繋ぎ込み](../../src/devstudio/view/mainwindow.cpp#L628))。接続されていない port で押すと
   「これは開ける供給元を持たない、保存された値だ」と notice が出ます。
4. **Disconnect** —— `set_input{null}`。**mode が `connected` のときだけ押せます**
   ([有効化](../../src/devstudio/view/graphattachmentwidget.cpp#L261))。他の 3 つは常に押せます。

値の欄を書き換えて focus を外すと `set_node_parameters` が飛びます(`parameters` 全体を読み、その port だけ
差し替えて送ります)。**この GUI の操作はどれも、同じ ops を手で送ったときと同じバイト列になります** ——
Studio 専用の裏口はありません(D0)。

**断られたとき**(✅UX-13b): 拒否は、graph の名前付きの code(✅CT-5 から `edit_graph_instances` は `invalid_graph_attachment` に畳まず、
code と `document` のまま返します)なら Graph タブと同じ 1 文、Studio が文を持つほかの code ならその文、どちらでもなければ engine の
message に ` (<code>)` を付けたものです([拒否を読む所](../../src/devstudio/model/graphattachmentmodel.cpp#L16))。UX-13b の前は message と code を
そのまま出していました。この読みを見る test は UX-13b の commit にはありません。

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
  [`editorEditErrorCodeName()`](../../src/core/communication/editorjournal.cpp#L3115)。**列挙子の名前とワイヤ上の
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
  ([set_parameters の検査](../../src/core/communication/renderconfigeditor.cpp#L1750))。
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

なお、プレビュー専用の**第 3 のレンダーグラフ variant**(flat / XR に続く `preview`)が起動時に必ずコンパイルされます。TAA・velocity・history・UI・XR ミラーなど、時間方向や UI・ミラーに依存する feature は除外されます。判定は feature の名前と、宣言した pass・RT の名前と種類によるもので(名前は語単位で照合)、pass の置き場所(`insert`)は `pelican_ui` の前後かどうかだけを見ます。swapchain に書くだけでは除外されません(preview では `preview_capture` への書き込みになります)。除外された feature の pass や RT を使う feature も、一緒に除外されます(✅G-6)。config に直書きした除外できない構成(`history: true` の target、`projection_jitter`、`@history` を読み書きする pass など)は、✅PV-1(`a173e70c`)から起動を止めません。通常の再生は起動し、preview だけを「利用できない」と記録して、起動時に warning を 1 行出します。編集の候補を preview が合成できないときも断らず、committed の結果の `preview` が `available: false` になります(PV-1 の前は、前者は起動時にパス名入りで拒否、後者は `variant: "preview"` 付きで断っていました。規則の詳細は[第6章 §6.13](06_rendering.md))。

**preview が利用できないとき(✅PV-1)。**`get_status` と `get_frame_plan` の `preview` が `{available: false, reason: {code: "preview.unsupported_authoring@1", feature, target?, graph?, pass?, cause?, message}}` で(利用できれば `{available: true}`)、`render_preview` は capture を読む前に JSON-RPC の error(`-32000`)で断ります。`data` は `{code: "preview.unavailable@1", method: "render_preview", reason: <同じ reason>}` です。`feature` は `history` / `projection_jitter` / `unsafe_target_name` / `unsafe_pass` / `history_read` / `history_write` / `composition` のどれかです(意味は[第6章 §6.13](06_rendering.md) の PV-1 の段落)。

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

`schema_version` は **`2` だけ**です(SnapshotV2)。scene v1 の文書を運んでいた SnapshotV1 は MG-2b で export / import とも撤去され、`schema_version: 1` は他の鍵を見る前に `unsupported_snapshot_version` で断られます([版を断る関数 `rejectSnapshotVersion`](../../src/core/communication/editorcommandservice.cpp#L246))。**どの段階で失敗しても、通常のプロジェクトのシーンファイル・キャッシュ・revision は一切変化しません**。

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
| Graph の線の吸着と強調、ゾーンと body の node の drag、分岐ごとの body、Auto layout / Fit view のボタン、画面に書くものを最後の列に、notice の Copy message | ✅UX-1 `7620fe1f`(drag の間の scene rect と、読んでいる image の二重の接続の拒否は `81b62382`) |
| Graph の `render.image` の欄(format / type と引数 / usage / 大きさ)、render pass の shader の欄、値の帯の一覧 | ✅UX-2 `5f048297` |
| Graph の palette の render node の種、Place の欄、入力ソケットの設定、Rename の提案、`depth_compare`、EL-1 の一覧(tag・material・variant・region の provider) | ✅UX-3 `f34adcc7`(シナリオ `graph_palette_kinds`) |
| Pass Editor の JSON の欄をフォームの部品に(`passforms/`)、skinned の stem の一覧、Ray scene の combo | ✅UX-4a `ad1e0771`(Graph への埋め込みは ✅UX-11) |
| render の編集の完了を 1 か所で読み、どのパネルでも `restart_required` で起動し直す / Rendering settings タブ(`edit_rendering_keys`) | ✅RSX `6a1069c8`(シナリオ `rendering_settings_from_blank`) |
| Blank から GUI だけで描画を組むシナリオ 5 本 | ✅GB-1 `3c6442a9`(A・B は最後まで、C・D・E は止まる所まで) |
| Blank から GUI だけで全部(deferred・影・RT の mask・bloom のループ・tone map)を組むシナリオと、今の GUI でできる最適化を測るシナリオ | ✅GB-2 `ed615734`(`gui_build_full` / `gui_build_optimize`) |
| Graph の pass の欄の Inputs(Filter / Address / mip)、New… の名前の提案し直し、Graph タブの pass の shader の監視、Studio が作る image を float・`TRANSFER_SRC` 無しに、Physical plan の Remove TRANSFER_SRC、Deferred lighting の雛形の `shadow_mask` | ✅UX-6a `1d8644fc` |
| Graph で root の node を fold group にまとめ、決定を一覧から選び、Verify で効かせる / Frame Plan の行から graph 全体の Serial・Isolate・No alias | ✅GL-3a / GL-4a `f5579426`(シナリオ `graph_fold_decisions`。pins と physical fragment の UI は無し) |
| Blank から GUI だけで FFT の海と LTC の面光源を組むシナリオ | ✅GB-3 `ba3b8a72`(`gui_build_ocean` / `gui_build_ltc`) |
| 外のエディタで書き換えた render の文書を見張って `reload_render_documents` で取り込む、Graph の Reload from disk、読み直しで履歴を切る | ✅RL-1s `e58c8da9`(シナリオ `render_reload_external`) |
| 番号付きの入力の Filter / Address / mip と fullscreen の新しい入力の `clamp_to_edge`、pass の Defines、raster の Depth / Raster / Draw(Grid)、palette の必須の parameter のダイアログ、image の history、開いた editor の下での描き直し、Play の間の `quit`、`get_status` の parse を 1 回 | ✅UX-7 `1fcd155e` |
| Frame Plan と Graph の GPU 時間・plan の数の 1 行、node の帯の GPU 時間、編集の前後の 1 行、`output unread` / `binding N unused` | ✅UX-6b `b1ed3356`(シナリオ `gui_build_optimize`) |
| Blank から GUI だけで音楽を鳴らすシナリオ | ✅GB-4 `e056fa25`(`gui_build_music`。曲線から shader への道は UX-8) |
| ループの body の雛形に `pelican_loop.glsl`、`reads element` の帯と pass の欄の 1 行、`shader.loop_push_overlap@1` の 1 文、海の FFT を 1 本の body に | ✅LC-1s `ed4ed5f9`(シナリオ `gui_build_ocean`) |
| `render.buffer` の value のソケット、pass の Buffers と名前付きの buffer の port、palette の初期値(曲線の ramp と時計、volume / pitch の 1)と catalog の必須を読むダイアログ、`value.curve` の Keys の表・小さな曲線・Timeline の点、`value.playback` / `render.scalar` の欄、root の Timeline、`audio.volume` / `audio.pitch` を voice につないで置く、音声の Import、一緒に来る node を重ねない、New definition…、log2 のボタンと image の層・mip の式、RS-15 / CT-1 の拒否の 1 文 | ✅UX-8 `cabf7691`(シナリオ `gui_build_curve` / `gui_build_music`) |
| palette の `render.compute` で project の `.comp` と RT の組を置き、`get_shader_interface` から ports・image・線、compute の欄の Shader と reflection error、shader の見張りと Update ports | ✅EL-5s `708c667d`(シナリオ `gui_build_ocean_compute`) |
| 保存中の palette、変わらない帯の値を送らない、確定前の値を手放す前に送る、palette と必須のダイアログを `open()` に、`audio.playback` の帯の voice の状態、起動時の致命の 1 文、definition の body だけ fit し直す | ✅UX-9 `4d11aa96`(シナリオ `gui_build_music` / `gui_build_curve`) |
| pass / task の Schedule(Once で書く image を同じ編集で Persistent に)と Run the body once、image の Lifetime、once の帯と `persistent` の帯、Recompute(`rerun_once`)と Recompute once、once の 2 つの拒否と `node_unknown` の 1 文 | ✅PS-1s `349af5e4`(シナリオ `gui_build_ocean` / `gui_build_ocean_compute`) |
| image の Initial contents・Size from the image・Import…(`.hdr` は `pelican_cli image ktx2` で焼く)、initial の 3 つの拒否の 1 文、Frame Plan の Resources の initial、initial の asset の見張りと `changed_assets`、拒否と起動時の致命が CT-2 の `document` / `definition` を読む、シナリオの prepare の口と `file` step の `copy` | ✅IA-1s `e2fd6dad`(シナリオ `gui_build_ltc`) |
| Graph の material pass の欄に Pass Editor の `MaterialSettingsForm` / `AttachmentOpsForm`、Tags を engine の `material_tags` から選ぶ(Graph と Pass Editor)、Materials drawn(近似)から Frame Plan の Materials へ | ✅UX-11 `0ac261f4`(シナリオ `graph_from_blank`。`screen_inputs` / `surface_resources` の名前は固定の表のまま —— ✅UX-12c で Graph の欄は engine の一覧に) |
| Materials drawn を engine の `drawn_materials` から(無い間は近似と言う)、Frame Plan の material pass の描いた数と Materials のタブの Drawn の列、binding の名前を `material_pass_contracts` の一覧から、Reads を semantic type で絞る | ✅UX-12c `708c4055`(engine の MP-1。シナリオ `gui_build_D`) |
| Graph の node の欄を catalog の `parameters` の表から(kind ごとの部品・必須の印・Reset・More parameters)、必須のダイアログを表から(Studio の表を外す)、値の帯は表の kind を先に、`graph.render_node_parameter_invalid@1` の 1 文と赤い行、`pose.clip_sample` の欄を汎用の欄の上に | ✅UX-12a `ac7c5250`(engine の CT-3。シナリオ `gui_node_parameters`) |
| feature の `required_targets` / `insertion` を足す前に読む(Add with targets・Place … in the Graph)、`graph.target_ref_type_mismatch@1` の Fix、Snapshots の after を `snapshot_after_candidates` から、`snapshot.after_unknown@1` と `material_route_unserved@1` の 1 文、Frame Plan の `Loop <loop>: its copies are picked.` が上書きされない | ✅UX-12b `99f645fb`(engine の FR-1 / FR-2。シナリオ `gui_settings_choose` ほか 4 本の期待。着地時に最後まで通らなかった `sky_ambient` と `cascade_count` 3 は ✅UX-12d で通る) |
| `choices_from` の編集できる combo(一覧に無い値は `not listed`)、asset の欄を表の `extensions` と `asset_kind` で、`union` の形の combo、image の Mip levels の Count / Full chain、`soft_max` までの slider と `above the usual range`、root の palette で `owned_instance` の op を灰色にして New definition… から definition の中の palette へ、`graph.op_requires_owned_instance@1` の 1 文 | ✅UX-12e `eeacce3e`(engine の CT-4。シナリオ `gui_node_parameters`) |
| 音の Bake analysis… のダイアログ(`bake_audio_analysis`、Baking / Baked の行、5 つの拒否の 1 文)、Use in Graph(`value.sample_table`)と必須のダイアログの焼いた表と行、Use in a shader(persistent の image・layout の buffer・2 つの `render.scalar` を 1 回の編集で)、`list_derived_assets` の stale の帯と Re-bake | ✅AB-1s `149c0ec8`(engine の AB-1。シナリオ `gui_build_audio_reactive`。Re-bake の後の同じ値の `set_node_parameters` は UX-13a で外した) |
| `needs_writer` を見て書き手を問いに入れる、Add with targets と cascade の Fix を 1 本の `edit_batch` に(取り消しの編集を外す、`batch_index` の段の文)、`feature.required_target_unwritten@1` の Write … from …、Connect <target> to present、`edit_batch` の Undo | ✅UX-12d `6c7dc8bb`(engine の FR-3。シナリオ `gui_settings_choose` で `sky_ambient` と `cascade_count` 3 が最後まで通る) |
| pass の buffer の port の binding を engine の規則(読む buffer の名前の順、`buffer_binding_base` から)で数える(ソケット・Buffers の欄・Use in a shader)、雛形と写す 1 行を `pelican_resource_ports.glsl` の形に、Re-bake の後の編集を外して表を見張りに入れる | ✅UX-13a `0e0f4481`(engine の IA-3。シナリオ `gui_build_curve` / `gui_build_audio_reactive`) |
| `choices_from` の `where`・`get_graph_catalog`・`asset_manifest`、`format_class` の Explicit…、`render.scalar` の書き先を `(will be set to runtime_f32)` で選んで同じ編集で整える、`graph.render_scalar_target_incompatible@1` の 1 文と Fix、definition の中の palette の灰色(`definition_instances`)、`value.sample_table` の必須のダイアログを汎用の欄に、`edit_graph_instances` の名前付きの拒否の 1 文 | ✅UX-13b `342142d5`(engine の CT-5。シナリオ `gui_node_parameters`) |
| Outliner と Inspector が `scene_tree` の代わりに `scene_outline` を読む(revision 付きの poll と `not_modified`、親を id で、`-32601` の engine は `scene_tree`) | ✅UX-13c `4405641a`(engine の `scene_outline` `b29ccfea`) |
| Connect <target> to present を 1 本の `edit_batch` に(Undo 1 回、断られたら段の名前)、繋ぎ替えで target_ref の型を書き換えるのは legacy の型のときだけ | ✅UX-13d `b289d706`(engine の FR-4。シナリオ `gui_settings_choose`) |
| light の方位角を camera の yaw と同じ基準に(+Z が 0、−X の側へ)、方位角・仰角・yaw・pitch の欄の tooltip、`output_locked` の 1 文と Bake again、CC-1 の後の viewport の期待(海・LTC・選ぶ設定) | ✅UX-14 `aaba15a6`(engine の CC-1 / AB-2。`gui_settings_choose` は CC-1c の後に `a954d38c` で直した) |
| Assets のドック(View > Assets、行ごとの Reload when saved outside とドックの Undo、監視の 1 行、Track asset contents…、照合の印と一覧、Verify again / Update manifest の確認の 1 行、`edit_project_settings` と AV-1 の拒否の 1 文)、監視が開いているときの `reload_shaders` の `committed: false` を読み直しとして扱う | ✅HW-2s / AV-1s `78737fdf`(engine の HW-1 / AV-1 / PJ-1。シナリオ `gui_assets_watch_verify`) |
| Frame Plan の Backends の Pin this candidate / Unpin(`edit_vulkan_plan_pins`)、Physical plan の Freeze / Remove / Eject again(`put_vulkan_physical_fragment`)と `physical_fragment_stale` の 1 文、常駐 BLAS の節、置き換えの来歴(Replaced by と Compile rewrites)、Layers の `is replaced by` の穴の直し | ✅UX-15b `5c5110ef`(engine の EL-2。シナリオ `gui_lower_layer_pin_freeze`) |
| Rendering settings の render_strategy の name と parameters、draw_sort の XR の並べ方(Per view)、graph_transforms の行、XR の Measured gate と minimum gain、preset の値の灰色の行、material の `gpu_draw_source` の欄(Other (JSON) を外す) | ✅UX-15a `06a9cd2d`(シナリオ `rendering_settings_from_blank` を延長) |
| rendering 節の pass を Graph の詳細欄で Pass Editor と同じ部品・同じ `presetPassEditParams()` で直す、詳細欄の Serial / Isolate / No alias、畳んだ node の tooltip を出どころごとに | ✅GL-5a `6565221b`(シナリオ `gui_folded_pass_edit`。task の欄は未着手。GL-5c は着地時は未着手で、✅GL-5c で着地(下の行)。GL-5b は下の行) |
| `render.compute` の `ray_tracing` を汎用の欄に(stage ごとの shader の候補、union の Shader / List)、`dispatch` の `groups_from` / `rays_from` の port を task の image port の combo に、compute の欄と palette の New… で RT の雛形(raygen / miss / closest hit)、Task Editor の New ray tracing も同じ雛形、RT の拒否 16 種の 1 文 | ✅CT-6s `827eddd7`(engine の CT-6。シナリオ `gui_build_rt_from_blank`。mode の変更は再起動の道に乗らず拒否の 1 文まで) |
| Materials のドック(View > Materials、`list_materials` の一覧と見えている間の poll、surface の宣言から作る欄 —— slider / spin / 色の swatch(linear は sRGB で見せる)/ check / テクスチャの combo、default と違う値の太字と Reset to default、Tags / Alpha mode / Surface と `restart_required` の再起動、`list_surfaces` から New material…、Assign to selected object、ドックの Undo、`material.*` と `external_modification` の 1 文)、Inspector の Material の行(model 単位の 1 行と使う object の数、primitive ごとの行、自分の Undo)、screenshot の `rgb` | ✅MT-1s `2d81ef51`(engine の MT-1 / MT-1b。シナリオ `gui_material_from_blank`。object ごとの割り当ては下の MT-2s の行) |
| New render target を Frame Plan と Graph で 1 つの関数に(`presentNewRenderTarget()` / `proposedRenderTargetName()`)、palette の `render.target_ref` の combo の New render target…、詳細欄の color 出力の行(宣言済みの target から選び直す・`New target: <pass>_color…`)、詳細欄の編集が断られたら root から読み直す、Add の入口と書き先の表 | ✅GL-5b `60ff3a2c`(シナリオ `gui_graph_new_target`。Add は束ねていない —— GL-5b の着地時は寄せ方は GL-6 の判断で、GL-5c は未着手だった。graph root の Add は ✅GL-5c で palette に寄せた(下の行)) |
| Inspector の Material の行を object ごとに: 範囲の切り替え(This object が既定で `set_object_material`、Every <model> は `bind_material`)、`get_object_materials` から描いている material と `(this object)` / `(model default: …)`、Use model default、primitive の行も範囲に従う、`objects` / `object_overrides` の「N objects, M with their own material」を revision ごとに読み直す、object の割り当ては scene の Undo、Material の行の Undo は送った model へ、`material.object_unknown@1` の 1 文と missing の印、Inspector が simplemodelview の list 型の欄で止まっていた件の直し | ✅MT-2s `7112072e`(engine の MT-2。シナリオ `gui_material_from_blank` を延ばした。ドックの Assign to selected object は model 単位のまま。基本形の Model default / Undo は着地時に engine が断った —— engine の MT-2b `213562bb` の後、✅MT-2s-b でシナリオが通す) |
| 基本形の Every <model> の Model default と Material の行の Undo をシナリオで通す、`material.object_unknown@1` を engine の `name` で言う、`changed: false` の答えで行が止まらない | ✅MT-2s-b `9d15ddc0`(engine の MT-2b。シナリオ `gui_material_from_blank` の `material_2` の代わりの手順を置き換えた) |
| graph root(preset を選ばない root)で Frame Plan の Add material pass… / Add fullscreen pass… / Add compute task… を Graph の palette と同じ道(graph の `nodes`)に、Edit pass / Edit task と行のダブルクリックを Graph が編集する pass なら Graph の詳細欄に、Graph の New render target… の既定の種類を material pass の slot の表から(forward の color は Explicit `R16G16B16A16_SFLOAT`)、委譲した入口の tooltip「Opens in the Graph」、View > Pass Editor、Frame Plan にしか無い操作の表 | ✅GL-5c `9cd89c5c`(利用者の決定 2026-10-04。シナリオ `gui_frameplan_delegates` と、期待を変えた既存の 15 本。preset root は form のまま。graph root で deferred の material pass を palette で置くと engine が `material_physical_contract_changed` で起こし直す件は engine の MC-1 で調査中。→ ✅MC-1s で直った) |
| Graph の palette の `render.material_pass` が作る image の format を Frame Plan の form と同じ表 `materialPassSlots(contract, hdr)` から(表と `materialPassHdr()` を model の `frameplanmodel` に移した)。`hdr` の無い Blank では albedo / emissive が `B8G8R8A8_SRGB` で、deferred の pass を置いても engine が起こし直さない | ✅MC-1s `1d10bd4c`(engine の MC-1。シナリオ `input_needs_shader`・`deferred_loop_from_scratch`・`frameplan_layout_then_edit` を再起動しない流れに戻した。契約が本当に変わる場合の再起動は v0 では今のまま、その場の組み直しは v0 の後 —— 2026-10-04 利用者の判断) |
| engine をいつも `--render-fallback` で起こす、起動時に組めない構成を viewport の上の帯と Frame Plan の status 行の 1 文に(Reload from disk / Open in Graph / Open file)、`render_document_syntax@1` の 1 文と Show the document(`restore_render_documents`)、module が frozen の拒否で Restart engine。Materials のドックの `rebuilt`(`Applied; rebuilt N models.`、再起動しない)、`rewritten_bindings` で Inspector の Material の行を読み直す、`material.routing_conflict@1` の 1 文、答えを待つ間 combo が戻らない | ✅BS-1s / MT-3s `19be8983`(engine の BS-1 / BS-1b / MT-3。シナリオ `gui_render_fallback` と `gui_material_from_blank` の tag の段。UX-9 の GPU test を fallback の形に書き換えた) |
| 自動化面の `list` が check できるボタンに `checked` を返す。シナリオ: Materials のドックの Surface を別の variant に替えて engine が補った routing が欄に出ることと Undo、MC-1s の後に赤だった `sockets_from_shader`・`sockets_fix_and_show` を再起動しない流れに | ✅SK-1 `67fd3d29`(engine の VR-1。シナリオ `gui_material_from_blank` の VR-1 の段。ドックのコードは変えていない) |
| シナリオの画素を読む点を、viewport の縦横比が変わっても箱の外に落ちる所へ(`gui_settings_choose` の影と床、`gui_build_C` の bloom の広がりを上下で) | ✅PX-1 `e3627606`(シナリオだけ。run が窓と viewport の大きさを固定しない根本は PX-2) |
| Problems のドック(View > Problems、Severity / Problem / Document / Line / Fix in の列、並べ替え、重さと文字の絞り込み)、engine の `get_problems` を `get_status.problems.revision` が動いたときと読み直しの答えの後に読む、Assets のドックの照合の warning / error の行、行の押下で直す場所へ(起動時の node は Graph、reload で断られた node と構文は Show the document、material は Materials、shader は外のエディタ、照合は Assets、route_unserved は Rendering settings)、status bar の件数、帯の ` See Problems.`、再起動の案内を `render_feature_requires_restart@1` と `fields.feature` で、reload で断られた文書の Show the document の Save はファイルを書く | ✅PB-1s `93934318`(engine の PB-1。シナリオ `gui_problems_window`。WP298 の GPU test は期待を書き換えたが engine の morph の上限で赤。→ reload で断られた node は Graph、Save は restore に、が ✅RR-1s) |
| シナリオの run は engine の viewport を 720x450 に固定する(自動化面の命令 `viewport_size`、driver が最初の step の前に送る、シナリオの最上位の `"viewport": [w, h] \| null`)、画素を読む 33 本の点を固定した大きさで測り直し、縁に近い点を選び直す | ✅PX-2 `28bc5ef9`(`whole_frame_first_steps` は `"viewport": null`。既定の大きさを変えると 33 本の点を全部測り直す) |
| reload に断られた版を engine が持つ間の帯(`live: false` の `Showing the saved version; …`、`held_unparsed` の `… is not valid JSON; the Graph shows the last version that was.`、fallback の帯が優先)、Show the document の Save をいつも restore に、断られた node は Graph で、断られた後に panel が保存した版の digest で読み直す、「could not be loaded」は `render_document_syntax@1` の間だけ | ✅RR-1s `471476d0`(engine の RR-1。シナリオ `gui_problems_window` を 79 step に延ばした) |

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
  ([editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp#L2033))。
  **`{"status":"imported","scene_revision":N,"current_scene_id":"…"}` だけを期待する
  厳密一致のクライアントは壊れます。**
- 代表的な `error_code`: `effect_outbox_full@1{dropped}`(1 回の scene load で 256 件を超えた分)/
  `persistence_read_failed@1{path, reason}`(壊れた save slot。以前は「slot 不在」に化けていました)。

### `get_status.participants.game_logic` の `error_code` / `error_fields`

`get_status` に `participants.game_logic` が入りました。**まだ 1 度も reload を試していない
起動直後でも、startup の初期化失敗がここに出ます**([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1287))。

- 試行済みなら `reload.runtime.game_logic` の内容(`configured` / `loaded` / `reloading` /
  `pending` / `source_busy_retries` / `generation` / `systems` / `source` / `loaded_copy` /
  `domain_error` / `error_code` / `error_fields`)がそのまま入ります
  ([reloadservice.cpp](../../src/core/watch/reloadservice.cpp#L809))。
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
