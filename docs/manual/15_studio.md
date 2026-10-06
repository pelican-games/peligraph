# 第15章 Studio の操作

対象: pelican2(develop `668624f04` の第13章から 2026-10-06 に分けた章。本文は第13章の記述をそのまま移したもの)/ このマニュアルはコードを正とする

## この章で学ぶこと

- Pelican Studio(`pelican_studio`)の panel ごとの操作 —— Inspector / Outliner / Materials / Assets / Graph タブ / Frame Plan / Pass Editor / Render Features / Rendering settings / Ray layout / Layers / Problems / Play(この順に節を並べています)
- project を作る・開く・保存するときの確認
- Studio の操作が送る編集 RPC との対応(RPC の params・戻り値・拒否コードの正本は [第13章 編集 RPC リファレンス](13_editor.md))

`pelican_studio` の起動と CLI 引数は [第10章 §10.7](10_tools.md)、自動化面(`--automation`)の命令とシナリオの書き方は [test/studio_scenarios/README.md](../../test/studio_scenarios/README.md) にあります。

> この章の本文は第13章から移したもので、文は書き換えていません。本文中の「§13.1「…」」「§13.5 の「…」」は第13章にあったときの呼び方で、第13章の同じ見出しの節からこの章の節へリンクしています。「上の」「下の」は第13章での並びを指すことがあり、この章では節が前後していることがあります。

## 15.1 project を作る・開く・保存する

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
  ([接続](../../src/devstudio/view/mainwindow.cpp#L2317))。シーンに未保存の編集があれば上の
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
gizmo の session を終え、返答待ちの要求を忘れます([切替の前処理](../../src/devstudio/view/mainwindow.cpp#L3368))。
今開いている project の開き直しは engine がそのまま残るので、session も続きます。
なお、Studio で作った project でも `project.json` の `generator` は `"pelican_cli project init"` のままです。

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
  通るので、自動化面の `wait_engine` は新しい engine を待ちます(✅Q-9。[移行の後に engine を起動し直す所](../../src/devstudio/view/mainwindow.cpp#L2572))。
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

## 15.2 Inspector

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
([使えない component の文の表](../../src/devstudio/model/inspectormodel.cpp#L736))、今ある code は `animation` / `audio` / `physics` /
`xr` / `vat` / `seqplayer` の `.module_unavailable@1`、`pelican.ray_instance_requires_layout_v2@1`、`audio.format_unsupported@1` の 8 つです。表に無い code は
code のまま出ます。

- **音声の format の拒否も表に入りました(✅Q-10)。**`audio.format_unsupported@1`(AU-2。音声は WAV と Ogg Vorbis で、拡張子と先頭の
  bytes が食い違うとこの code)の文は `Sound extension and contents disagree (WAV or Ogg Vorbis)`、短い語は `— unsupported sound` です。
  ただし着地時の記録では、**今この code が Inspector や Add component に届く経路はありません**(音声を取る component が無く、
  engine の asset の一覧に音声の種別が無い)。今この文が出るのは **Graph タブの notice** で、graph の編集の preflight の拒否
  (`render_pipeline_preflight_failed`)が message の頭にこの code を持っているときです。表の code を、拒否の code か message の中に
  持っていれば表の文にします([`knownRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L1111)。Graph タブは拒否を notice に
  する [`diagnostic()`](../../src/devstudio/model/graphcanvasmodel.cpp#L24) でこれを通します)。

- **理由が code でない行は止めません。**`available: false` でも `unavailable_reason` が文(`simplemodelview` の
  `A model reference is required.` など。既定値だけでは足りず入力が要る、の意味)なら、その行は普通に並び、宣言に値を入れれば
  Add できます。code かどうかは「小文字・数字・`_` の `.` 区切りの名前 + `@` + 数字」の形で見分けます
  ([`isComponentUnavailableCode()`](../../src/devstudio/model/inspectormodel.cpp#L763))。
- ⚠ 止めているのは Add ボタンの有効・無効だけで、Add を押したときの処理は行を見直しません。

⚠ **Inspector にはもう 1 つ編集面があります。**`Add component...` / `Ray geometry...` の下、
properties の一覧より**上**にある **Attached graphs** / **Instance inputs** がそれで、そちらは component ではなく共通グラフの `graph.instance` を
扱い、**journal にも Undo にも載りません**(§13.5「Inspector のグラフ装着」)。

### Inspector の欄 —— model・数値・回転・光の向き・編集と commit

**Inspector の model 欄は選ぶ欄です(✅G-2)。**`simplemodelview.model` は打つ欄ではなく combo
(`pelican.inspectorModelChoice`)です([model 欄の選択肢の並び](../../src/devstudio/model/inspectormodel.cpp#L698))。
並びは基本形 3 つ(`Cube (built-in)` などの名前で)→ 区切り → `list_assets` が言う project の model(engine の基本形は
二重に出しません)→ 今の値がそのどれでもなければその値、です。選ぶとすぐ commit します(enum の combo と同じ)。
✅MI-1s から、combo の右に **Import…**(`pelican.inspectorModelImport`)があり、ファイルを選んで登録した model をこの欄に入れられます
(下の「Studio で外の model を持ち込む(✅MI-1s)」)。
Add component... の asset 一覧も同じ読み方で、基本形は `Cube (built-in)` の名前で先頭に並び、区切りの下に project の
asset が `id [status]` で並びます。

**Inspector の数値欄の名前(✅G-5)**: 自動化面から届くように、数値の欄は 1 つずつ名前を持ちます。vector の列は
`pelican.inspectorVector.<component>.<field>.<列>`(例: `pelican.inspectorVector.transform.scale.2`)、数 1 つは
`pelican.inspectorNumeric.<component>.<field>` です([数値欄の名前の付け方](../../src/devstudio/view/inspectorwidget.cpp#L2242))。
G-5 より前は、どの vector も最初の列が同じ名前でした。シナリオは欄が有効になるのを待って打ち、Return で commit し、
欄がその値を持つまで待ちます([シナリオの README](../../test/studio_scenarios/README.md#L593))。

**回転は角度で入れます(✅T-1)。**`transform.rotation` は X / Y / Z の 3 欄(度、小数 2 桁、自動化面の名前は
`pelican.inspectorEuler.transform.rotation.0` 〜 `.2`)で見せて編集します([回転の欄を作る所](../../src/devstudio/view/inspectorwidget.cpp#L2304))。
回転は X、Y、Z の順に固定軸の回りにかかります(Rz·Ry·Rx)。保存されるのは今までどおり quaternion `[x, y, z, w]` で、
変換は Studio がします([角度と quaternion の変換](../../src/devstudio/model/inspectormodel.cpp#L521))。読むときは X と Z が
(-180, 180]、Y が [-90, 90] で、Y が ±90 のときは X を 0 にして残りを Z に寄せます。欄の右の **Quaternion** の toggle を開くと、
quaternion の 4 欄(G-5 からの `pelican.inspectorVector.transform.rotation.<列>`)が出て、そちらでも編集できます。開閉は欄を
作り直しても保たれます。viewport の gizmo の回転は今までどおりです。

**表示の桁で同じなら、変えていません(✅T-1)。**数値欄は、保存された値を欄の桁で丸めたものと表示が同じなら「値は変わって
いない」と数えます([表示の桁で比べる所](../../src/devstudio/view/inspectorwidget.cpp#L837))。T-1 より前は、focus を置いて離れただけで
丸めた値(`0.38268343` の代わりに `0.3827`)が commit されていました。角度の欄も、同じ回転を表していれば送りません。

**光の向きは方位角と仰角で、camera の yaw と同じ基準です(✅UX-10 / ✅UX-14)。**component の vec3 の `direction`(`light.direction`)は、
方位角(`pelican.inspectorDirection.light.direction.azimuth`。(-180, 180] で回り込む)と仰角(`.elevation`。[-90, 90])の 2 欄(度、小数 2 桁)で
見せ、**Raw**(`pelican.inspectorDirectionRaw.light.direction`)を押すと保存されている x / y / z の欄が出ます
([方位角と仰角の欄を作る所](../../src/devstudio/view/inspectorwidget.cpp#L2367))。仰角は水平から上が正(負は下向き)、真上・真下の方位角は 0、
角度から作るベクトルは長さ 1 で、保存されるのは今までどおりベクトルです。camera の `controller.yaw` / `controller.pitch` は保存はラジアン、欄は度です。

- **基準**(✅UX-14、`aaba15a6`): 方位角は +Y の回りに、+Z を 0 として −X の側へ増えます([方位角を読む `directionAnglesDegrees()`](../../src/devstudio/model/inspectormodel.cpp#L635))。
  engine の ✅CC-1 の camera.controller の `yaw` / `pitch` と同じ式で、同じ角度は同じベクトルになります(UX-14 の test が engine の式と突き合わせます)。
  光ではそのベクトルが光の進む向き、orbit の camera では target から camera へ向かう向きです([第4章](04_scene_ecs.md) の camera.controller)。
  UX-10 の着地時から UX-14 の前までは、+Z から **+X** の側へ増えていました(同じベクトルの方位角は符号が逆)。文書は変わりません。
- **tooltip**: 方位角の欄は `Azimuth: the turn about +Y from +Z towards -X, in degrees (as a camera's yaw)`、仰角の欄は
  `Elevation: degrees above the horizon (negative points down)`、camera の yaw の欄は `Yaw: the turn about +Y from +Z towards -X, in degrees (a light's azimuth turns the same way)`、
  pitch の欄は `Pitch: degrees from the horizon towards +Y (negative looks down)` です([yaw / pitch の文 `inspectorAngleTip()`](../../src/devstudio/model/inspectormodel.cpp#L617)、
  [方位角 / 仰角の文 `inspectorDirectionAngleTip()`](../../src/devstudio/model/inspectormodel.cpp#L625))。
- 例(シナリオ `gui_settings_choose`): Blank の雛形の KeyLight の向き `(-0.5, -1, -0.25)`(engine の CC-1c `b0b2fc77` の後。−X の側へ、camera から離れる向き)は
  方位角 116.57 と読め、−116.57 を打つと x だけが反転して z は camera から離れる向きのまま、影が箱の左から右へ移ります(下の「Studio の自動化面」)。
- test: `devstudio_inspector_test` の [「UX-14 decision 3: a light's azimuth turns as a camera.controller's yaw does -- 0 is +Z, growing towards -X -- and the yaw, pitch and azimuth fields say so」](../../test/devstudio_inspector_test.cpp#L1900)・
  [「UX-10 a light's direction reads as azimuth and elevation, its colour as a swatch, and a sprite's texture is picked from the textures list_assets reports」](../../test/devstudio_inspector_test.cpp#L1939)(UX-14 で 90 度の期待を +X から −X に替えました)。

**Inspector の欄の編集と commit(✅Q-7)**

- **focus だけでは編集になりません。**編集は、値を打つ(値が変わる)か欄の上で mouse の button を押したときに始まります
  ([focus で編集を始めなくなった所](../../src/devstudio/view/inspectorwidget.cpp#L2647))。欄に focus を置いたままでも
  **Refresh** は押せ、他の欄も止まりません。vector の中で列を移るあいだは 1 つの編集のままです。
- **busy の間に打った値は、手が空いてから送られます。**別の編集の commit の途中(Inspector が busy)は他の欄が止まりますが、
  focus のある欄は止まりません([busy でも focus のある欄は止めない所](../../src/devstudio/view/inspectorwidget.cpp#L1632))。
  そこへ打った値は欄に残り、欄を離れるか Return を押すと、Inspector の手が空いてから 1 回の編集として送られます
  ([手が空いてから値を送る所](../../src/devstudio/view/inspectorwidget.cpp#L788))。その間に欄が消えた(別の object を選んだなど)か、
  値が既に同じなら何も送りません。
- **commit の往復で欄は作り直さず、値だけ差し替えます**([値だけを差し替える所](../../src/devstudio/view/inspectorwidget.cpp#L976))。
  編集中の欄・送り待ちの値を持つ欄・打ちかけの欄(`1.` のように、表示がまだ値と一致しない欄)は上書きしません。作り直すのは
  描くものが変わったとき(別の object、component の追加・削除、欄の種類や範囲が変わるなど)だけです
  ([作り直すかどうかを決める所](../../src/devstudio/view/inspectorwidget.cpp#L918))。
  ⚠ list を持つ component(`pelican_ray_instance` など)は、list の key に scene revision が入っているので、今も commit のたびに
  作り直されます(打ちかけの値はそこでは消えます)。
- G-6 までは、commit のたびに欄を作り直し、focus だけで編集が始まっていました。commit の間に止まった欄から Qt が focus を
  次の欄へ送り、その focus が始めた編集を誰も終わらせないので、Inspector が busy のまま残り(Refresh も他の欄も止まり、
  窓を閉じると未保存の問いが出る)、窓が active のときだけ起きる間欠の故障になっていました。

### Inspector の List の行と Ray geometry

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

Inspector の **Attached graphs** / **Instance inputs**(グラフの装着)は [第13章「Inspector のグラフ装着」](13_editor.md#inspector-のグラフ装着) にあります。

## 15.3 Outliner

### Studio で object を置く(✅G-2)

Outliner の右クリックと **Scene** toolbar の **Add Object** は同じ 1 つの menu です
([Add Object の menu を作る所](../../src/devstudio/view/mainwindow.cpp#L2721))。項目は **Empty** / **Cube (built-in)** /
**Plane (built-in)** / **Sphere (built-in)** / **Floor** で、自動化面の action 名は `pelican.outliner.addObject.empty` / `cube` /
`plane` / `sphere` / `floor` です。✅MI-1s から、その下に区切り線を挟んで **Model from file…**(`pelican.outliner.addObject.modelFromFile`)が
あり、project の外の `.glb` / `.vrm` を持ち込んで置けます(下の「Studio で外の model を持ち込む(✅MI-1s)」)。

- **Floor(✅T-1)** は床として置いた Plane です。Plane の基本形を原点の少し下(`pos` が (0, -0.5, 0))に、広く
  (`scale` が (8, 1, 8))置きます([`addObjectDeclaration()`](../../src/devstudio/model/inspectormodel.cpp#L475))。名前は `Floor`、
  `Floor_001`… です。T-1 より前は Plane を置いてから Inspector で位置と大きさを打っていました。

- **送るもの**: scene の `edit` 1 回に `spawn` op 1 つです(上の `addObjectDeclaration()`)。
  transform は原点(Floor だけは上のとおり)で、形なら `simplemodelview.model` に `engine://primitives/<kind>`(Floor は `plane`)、Empty は transform だけです。
  置き先の scene は選択中の object の scene(無ければ engine が最初に言った scene)で、選択中の object の子にはなりません。
- **名前**: 種類の名前(`Empty` / `Cube` / `Plane` / `Sphere` / `Floor`)が空いていればそれ、使われていれば `Cube_001`、`Cube_002`…
  の最小の空き番号です([`uniqueObjectName()`](../../src/devstudio/model/inspectormodel.cpp#L460))。Blender は `Cube.001` と
  書きますが、scene の object 名は `[a-zA-Z0-9_]` だけなので `_` にしています。
- **選択**: Outliner の読み直しを最短の間隔に縮め、committed の応答の `created_objects` が名乗る object が Outliner に
  出た時点でそれを選び、Inspector に出します([`MainWindow::selectCreatedObject()`](../../src/devstudio/view/mainwindow.cpp#L2928))。
  ✅OL-1 から authoring id で選びます(それまでは名前で選び直していました)。
- **Undo**: coalesce key を付けないので、Undo 1 回で Add 1 回が戻ります。
- **Inspector の編集の途中で押した Add Object は待ちます(✅Q-7)**: Inspector の値の編集が進行中(commit の途中を含む)に
  押すと、spawn は 1 本だけ待ち、Inspector の手が空いた時点で、その編集が残した scene revision の上に送られます
  ([`InspectorModel::spawnObject()`](../../src/devstudio/model/inspectormodel.cpp#L4467))。待っている間は status bar(8 秒)と
  Outliner の下の 1 行(`pelican.outlinerNotice`)に `Adding <名前> after the current edit.` が出ます
  ([Add Object の結果を status bar と Outliner に出す所](../../src/devstudio/view/mainwindow.cpp#L2904))。既に 1 本待っているときに
  押した 2 本目は、置き換えずに断ります。同じ 2 か所に
  `Could not add <名前>: <待っている名前> is already waiting to be added; add another once it is in.` が出るので、
  1 本目が入ってからもう一度押してください。Outliner の下の 1 行は、新しい object が選ばれたときに消えます。
  ⚠ 待っている spawn は engine が止まると捨てられ、そのことは言われません(1 行が消えるだけです)。
- Add Object は常に押せます。project が開いていない・engine の準備ができていないときは、status bar に
  `Add Object needs an open project with its engine running.` と出るだけです。
- ⚠ spawn は Studio が最後に見た scene revision で送ります。他の client が編集した直後は `stale_revision` で断られるので、
  もう一度押してください。また、engine が断った spawn の名前も Studio は使用中と数えたままなので、次の Add はその次の
  番号になります。Empty / Plane / Sphere は単体 test だけで、シナリオが通すのは Cube と Floor です。

### Studio で外の model を持ち込む(✅MI-1s)

✅MI-1s(`70298f88`)から、project の外にある `.glb` / `.vrm` を、Blank から作った project でも **GUI だけで** scene に置けます。
Studio がファイルを project の中に置き(外のファイルはコピー)、engine の ✅MI-1 の `register_model`(§13.5 の「project の中の model
ファイルを登録する・外す(✅MI-1)」)で登録し、その model を描く object を足します。MI-1s の前は、Add Object が置けるのは
Empty / Cube / Plane / Sphere / Floor だけで、Inspector の model 欄も `list_assets` の一覧から選ぶだけでした(仕様書の記述)。
ファイルをコピーするのは Studio、登録するのは engine という分担は、音・画像の取り込みと同じです(D0)。

- **Add Object > Model from file…**(決定 1。[Model from file… を menu に足す所](../../src/devstudio/view/mainwindow.cpp#L2747)):
  Outliner の右クリックと Scene の toolbar の Add Object の、基本形の下の区切り線の次の 1 行です。押すと Qt のファイルダイアログ
  (`pelican.modelImportDialog`。OS のダイアログではないので、自動化面がファイル名の欄 `pelican.modelImportDialog.fileName` を名指しできます)が開き、
  filter は `.glb` / `.gltf` / `.vrm` です([`MainWindow::chooseModelFile()`](../../src/devstudio/view/mainwindow.cpp#L2797))。
  project が開いていない・engine の準備ができていないときは、ほかの Add Object と同じく status bar に
  `Add Object needs an open project with its engine running.` と出るだけです。
- **ファイルの置き方**([`placeProjectModelFile()`](../../src/devstudio/model/modelimport.cpp#L66)):
  - project の中のファイルは、その場所のまま登録します(コピーしません)。
  - 外のファイルは `assets/models/` へコピーします。同じ名前のファイルが既にあれば `<stem>_2`、`_3`… の空いている名前で、上書きはしません
    ([`copyIntoProjectAssets()`](../../src/devstudio/model/graphcanvasvalues.cpp#L1044)。Graph の parameter の Import… と同じ関数です)。
  - **外の `.gltf` は断ります。**`.gltf` は隣の `.bin` と画像を読むので、1 つだけコピーすると読めなくなるためです。文は
    `A .gltf reads the .bin and images beside it: put it in the project folder with them first and choose it there (a model in one file is a .glb).`
    です。関連するファイルごと project のフォルダに置いてから選べば、その場所のまま登録します。
- **登録**: `register_model {path}` を `name` を付けずに送り、engine がファイルの stem を名前にします。`model.name_conflict@1` で断られたら、
  stem に `_001`、`_002`… を付けた `name` で送り直します(999 まで。`box_2.glb` なら `box_2_001`)。`model.path_conflict@1`(同じファイルが
  もう登録されている)なら、答えの `name` をそのまま使います([`ModelRegistrationSession::receiveRefusal()`](../../src/devstudio/model/modelimport.cpp#L253))。
  status bar には `Registered <path> as model <name>.`、登録済みだったときは `<path> is model <name> already.` が出ます。
- **object の名前と中身**: object の名前は model の名前から作り、`[a-zA-Z0-9_]` 以外の文字は `_` にします(`a.b-c` は `a_b_c`、`_` だけになれば
  `Model`。[`modelObjectBaseName()`](../../src/devstudio/model/inspectormodel.cpp#L498))。使われていれば Add Object と同じ `_001`、`_002`… です。
  中身は Cube と同じ原点の transform と、`simplemodelview.model` に登録した名前です([`modelObjectDeclaration()`](../../src/devstudio/model/inspectormodel.cpp#L492))。
  送り方・置き先の scene・選択・Inspector の編集中の待ち・Undo は上の「Studio で object を置く」の Add Object と同じ道です。
  置いた後に、Inspector の model 欄の一覧と、Assets のドックが使う登録済みファイルの一覧(`list_assets`)を読み直します。
- **`restart_required`**: 登録は書かれたが engine が次の起動まで使えないときは、object を足しません。Outliner の下の 1 行(`pelican.outlinerNotice`)と
  status bar に `<path> is registered as model <name>; restart the engine to use it: <理由>` が出て、その右に **Restart engine**
  (`pelican.outliner.restartEngine`)が出ます([`MainWindow::showOutlinerRestart()`](../../src/devstudio/view/mainwindow.cpp#L2855))。押すと Frame Plan などと同じ
  `MainWindow::requestEngineRestart()` で起こし直します。Inspector の Import… から来たときも、案内は Inspector ではなくこの Outliner の下に出ます
  (Inspector の notice は Inspector の model が持っていて外から書く口が無いため。実装者の報告)。
- **断られたとき**: 文は status bar(10 秒)と Outliner の下の 1 行に出ます(Inspector の Import… から来た拒否も同じ場所です)。コピーした後に
  断られたら、**この操作でコピーしたファイルだけ**を消し、もともと project にあったファイルは残します([`ModelRegistrationSession::refuse()`](../../src/devstudio/model/modelimport.cpp#L172))。
  答えが返らずに失敗したとき(登録されたか分からないとき)はコピーを残します。

**Inspector の Import…**(決定 2): `simplemodelview` の model 欄の combo の右のボタンです(`pelican.inspectorModelImport`。
[Import… を置く所](../../src/devstudio/view/inspectorwidget.cpp#L2183))。sprite の texture の欄には付けていません。流れは Model from file… と同じで、
その場で(live で)登録できたら、object を足す代わりにその欄の値を登録した名前にして 1 回 commit します(combo で選んだときと同じ
`set_component_value` の 1 回の編集。test が見ています)。
`restart_required` のときは commit せず、上と同じ案内を Outliner の下に出します。

**Undo**(決定 3): Model from file… の Undo は、今の Add Object の Undo のまま(足した object を消すだけ)です。登録とコピーしたファイルは
Undo では戻りません。登録を外すのは下の Remove from project… です。

**Assets のドックの Remove from project…**(決定 4):

- Assets のドックの行の右クリックに **Remove from project…**(`pelican.assets.removeModel`)が出るのは、その行のファイルが登録済みの
  model のファイルのときだけです([確認の行を出す所](../../src/devstudio/view/assetsdock.cpp#L591))。登録済みのファイルは、engine が起きたときと登録・解除の後に
  Studio が読む `list_assets` の `kind: "model"` から作ります(engine の基本形は除きます。[一覧を読む所](../../src/devstudio/view/mainwindow.cpp#L1234))。
  `list_assets` の `path` は engine が解決した絶対 path なので、Studio が project 相対に読み替えて行と比べます
  ([`projectRelativeAssetPath()`](../../src/devstudio/model/modelimport.cpp#L97))。→ engine の ✅EB-1(`aaa7b2da`)から `path` は asset_data に書かれた
  project 相対の綴りで、絶対 path は `resolved_path` です。この関数は相対の path を受けても正規化した相対を返すので、Studio は変えずに同じ行と比べます(コードの読み)。
- 押すと、先に `get_material_bindings` を読んでから、ドックの中に確認の行(`pelican.assets.removeModelConfirm`、modal のダイアログではありません)と
  **Remove**(`.removeModelConfirmed`)/ **Cancel**(`.removeModelCancel`)が出ます。文は `Remove model <name> from the project? <path> stays on disk.` で、
  その model が `material_bindings` を持っていれば `Its material assignments are removed with it and cannot be put back.` が続きます
  (engine の `register_model` は bindings を戻せないため。§13.5)。
- Remove で `unregister_model {name}` を送り([Remove を送る所](../../src/devstudio/view/mainwindow.cpp#L406))、結果をドックの notice の 1 文で言います。
  外れたら `Removed model <name> from the project; <path> stays on disk.`、`restart_required` ならその後ろに engine の理由(先頭を大文字にして `.` を付けたもの)、
  断られたら下の拒否の 1 文です。ファイルは消しません。この操作は Undo に載りません。

**拒否の 1 文**(決定 5。[model の拒否の 1 文の表 `ModelRefusalTexts`](../../src/devstudio/model/inspectormodel.cpp#L2060)、[`modelRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L2089)):
`register_model` / `unregister_model` の拒否は、どれも `<文> (<code>).` の 1 文です。`knownRefusalDescription()` からも同じ文が引けます。

| code | 文 |
|---|---|
| `model.path_invalid@1` | `<path> cannot name a model file: <理由>`(理由は `fragment` → `it names a part of a file (#)`、`dot_segment` → `it holds a . or .. folder`、`backslash` → `it uses \ where / goes`、`absolute` → `it is not inside the project`、`empty` → `it is empty`) |
| `model.format_unsupported@1` | `<path> is not a .glb, .gltf or .vrm file` |
| `model.file_missing@1` | `<path> is not a file in the project` |
| `model.path_conflict@1` | `<path> is registered already as model <name>`(登録のときは文にせず、その名前を使います) |
| `model.name_invalid@1` | `<name> cannot be a model name: <理由>`。基本形を外そうとしたとき(`engine_primitive`)は `<name> is one of the engine's own shapes, which stay` |
| `model.name_conflict@1` | `A model named <name> exists already`(登録のときは番号を振って送り直し、999 まで使い切ると `No free model name for <stem> (model.name_conflict@1).`) |
| `model.load_failed@1` | `<path> does not load: <engine の message>` |
| `model.unknown@1` | `No model is named <name> in this project` |
| `model.in_use@1` | `<name> is used by <N> objects: a, b, …`(engine が言う先頭 8 件の名前。数が多ければ末尾に `…`。1 件なら `is used by 1 object: a`) |
| `model.transaction_busy@1` | `The project's asset list is being written by another edit; try again` |
| `external_modification` | `<path> changed outside Studio since it was read; try again` |

例(test): 壊れた glb のコピーは `assets/models/broken.glb does not load: failed to load gltf file (Invalid glTF binary.) (model.load_failed@1).` で、
そのコピーは消えます。

**登録されていない model の edit**(決定 5): engine の ✅MI-1 の `payload.reason: "model_unknown"` の付いた edit の失敗は、
`Model <name> is not registered in this project. Choose its file with Import… beside the Inspector's model field, or Add Object > Model from file…`
の文になります([`modelUnknownEditDescription()`](../../src/devstudio/model/inspectormodel.cpp#L2129))。Inspector の notice にも、Outliner の
spawn の拒否の文にも、`method_unavailable - ` を前に付けずに出します。

**Problems の窓**(決定 6): 何も足していません。登録の拒否はその場の notice(Outliner の下と status bar、Assets のドックの notice)に出ます。

**仕様から外れた判断・残した穴**(実装者の報告):

- 再起動の案内は、Inspector の Import… から来たときも Outliner の下に出します(上のとおり)。拒否も同じです。
- Remove from project… が押せるかは、行を選び直したときに決まります。`list_assets` の答えが行の選択より後に来たときは、選び直すまで古いままです。
- `list_assets` の `path` は絶対 path で、`register_model` の答えと asset_data の project 相対の path と揃っていません。Studio の読み替えで回避しています(engine への要望)。
  → ✅EB-1(`aaa7b2da`)で engine の `path` が project 相対になり、揃いました(絶対 path は `resolved_path`)。
- 外した model の `material_bindings` を戻す口が engine に無いので、確認の文で「戻せない」と言っています(engine への要望。MI-1 の残る穴と同じ)。

- test: `devstudio_model_import_test`(新。`QT_OFFSCREEN`。test が engine の代わりに答えます)の
  [「MI-1s decision 1: a model file outside the project is copied into assets/models and registered by its path; a taken stem is numbered _001, _002; a refusal takes back only the copy this made」](../../test/devstudio_model_import_test.cpp#L78)・
  [「MI-1s decision 5: register_model's and unregister_model's refusals are one sentence each, and an edit naming a model the project has not registered says so and where it is registered from」](../../test/devstudio_model_import_test.cpp#L185)、
  `devstudio_inspector_widget_test` の [「MI-1s decision 2: Import... beside the model field registers a file through the window's action and sets the field to the name it was registered as, in one edit」](../../test/devstudio_inspector_widget_test.cpp#L585)、
  `devstudio_assets_dock_test` の [「MI-1s decision 4: a registered model's row offers Remove from project..., which asks first -- saying its file stays, and that its material assignments go with it when it has some -- and says how unregister_model went in one sentence」](../../test/devstudio_assets_dock_test.cpp#L617)。
  決定 3(Undo)は新しいコードが無いので test を足していません。
- シナリオ: `gui_model_from_file`(Blank から GUI だけで。§13.5「Studio の自動化面」)。VRM は、同じ流れの一時的なシナリオ(`projects/vrm_xr_demo` の
  `vrm_xr_character.vrm` を project の外に写す)を ctest から 1 回だけ流して緑でした(61 step、報告の数え方。登録は live、object `vrm_xr_character` が足され、
  `list_assets` は `loaded`、viewport の真ん中が背景と違う色、Save Scene も通る)。そのシナリオは repo に残していません。VRM で project を開き直す段は流していません(未確認)。

### Studio の Outliner で複製・削除・付け替える(✅OL-1)

Outliner の行の右クリックの menu の先頭に **Duplicate** と **Delete** があり、その下が Add Object です
(何も選んでいないときは 2 つとも押せません)。送るのは scene の `edit` 1 回に op 1 つで、Undo 1 回で戻ります(§13.3 の
`duplicate` / `destroy` / `reparent`)。object は**名前でも宣言の位置でもなく `authoring_object_id` で**指します
([Outliner の edit の中身](../../src/devstudio/model/outlineredit.cpp#L21))。

- **Duplicate(Ctrl+D)**: 選択中の object を子孫ごと複製します。copy の名前は engine が付けます(`Cube_copy1` など)。
  複製の後は、committed の応答の `created_objects` のうち、`source_object_id` が複製元のもの(copy の根)を選びます
  ([Duplicate を送る所](../../src/devstudio/view/mainwindow.cpp#L2981))。
- **Delete(Del)**: 選択中の object を消します。子孫も一緒に消えます。**子孫があるときだけ** 1 回問います
  (`Delete <名前>? The N objects under it are deleted too.`、既定の button は Cancel。自動化面の名前は
  `pelican.outliner.deleteConfirm` と `.delete` / `.cancel`)。子孫が無ければ問わずに消します(Undo で戻せるので)。
  消した後は親を、親が無ければ前に宣言された兄弟を選びます([Delete の確認](../../src/devstudio/view/mainwindow.cpp#L2995))。
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
  ([committed の後に選ぶ所](../../src/devstudio/view/mainwindow.cpp#L3090))。
- **断られたとき**は、status bar(8 秒)と Outliner の下の 1 行(`pelican.outlinerNotice`)に 1 文で出ます
  ([断りの文](../../src/devstudio/model/inspectormodel.cpp#L2363))。例: engine が `cycle_detected` で断った付け替えは `Cannot move <名前> under <親>: <親> is inside <名前>.`、
  preview 中は `Cannot duplicate <名前> while a live preview is open; finish the preview first.`、physics の無い build で
  collider を含む object の複製は Q-6 と同じ文で `Cannot duplicate <名前>: This build has no physics module (physics.module_unavailable@1).`、
  もう無い object は `<名前> is no longer in the scene.` です。
- **Inspector の編集が進行中(commit の途中を含む)のときは断ります。**待たせずに
  `Finish the edit in progress first, then <操作> again.` を同じ 2 か所に出します
  ([進行中なら断る所](../../src/devstudio/model/inspectormodel.cpp#L4499))。進行中の編集が同じ object を変えているかも
  しれないためで、1 本だけ待たせる Add Object(✅Q-7)とは扱いが違います。
- rename・複数選択の一括操作・scene をまたぐ移動はありません。

### Studio の Outliner と Inspector が読む一覧(✅UX-13c)

Outliner と Inspector は、scene の object の一覧を `scene_outline`(§13.5「一覧だけを読む `scene_outline`」)で読みます。
UX-13c(`4405641a`)の前は `scene_tree` で、Outliner は revision が変わっていなくても poll のたびに全 object の component 込みの木を
受け取っていました。Studio がそこから読むのは scene の id と revision、各 object の id・宣言の位置・名前・親だけです(仕様の背景)。

- **Outliner の poll**([`MainWindow::requestOutlinerRefresh()`](../../src/devstudio/view/mainwindow.cpp#L3145)): Studio がまだ scene を
  持っていない最初の 1 回は `scene_outline {}` です。scene を 1 つ持った後は `{scene_id, if_scene_revision: <持っている revision>}` を送り、
  engine が `not_modified: true` で答えれば何も作り直しません。revision が動いていれば一覧全体が来て、行を作り直します。
  持っている scene が 1 つでないときは条件を付けません(コードの読み)。
- poll の間隔は今までどおりで、250 ms から始まり、変わらない答えのたびに倍、最大 2,000 ms、変わった答えで 250 ms に戻ります
  ([間隔の定数](../../src/devstudio/view/mainwindow.cpp#L103)、[答えを読む `completeOutlinerRpc()`](../../src/devstudio/view/mainwindow.cpp#L3177))。
- poll が失敗したとき・答えが形として読めないとき(status bar に `Outliner scene_outline failed: …` / `Invalid scene_outline response: …`)は、
  次の 1 回を条件無しで問います([失敗の `failOutlinerRpc()`](../../src/devstudio/view/mainwindow.cpp#L3222))。持っていない revision への
  `not_modified` も読めない答えとして扱います。
- **親は id で引きます**: 親は `parent_authoring_object_id` で探すので、親が子より後ろで宣言されていても木になります。名前の無い object は
  今までどおり Studio が作る表示名で出し、付け替えも今までどおり断ります。宣言の位置が隙間なく並ぶこと・id が 0 でなく重複も無いこと・
  親の id が一覧にあることを検査し、外れた答えは使わず、持っている scene を残します([`ProjectOutlinerModel::updateScene()`](../../src/devstudio/model/project.cpp#L229))。
- **Inspector**: 選んだ object の id を確かめる問いが `scene_outline {scene_id}` になりました(条件無し)。詳細は今までどおり `get_components` です
  ([問いを積む所](../../src/devstudio/model/inspectormodel.cpp#L2833))。
- **古い engine**: `scene_outline` が `-32601`(method not found)で断られると、その engine の間は `scene_tree` で読みます(親は名前で引く
  今までの読み方)。Outliner と Inspector がそれぞれ 1 回ずつ問って覚え、engine に繋ぎ直すと問い直します
  ([Outliner が覚える所](../../src/devstudio/view/mainwindow.cpp#L1166)、[Inspector が覚える所](../../src/devstudio/model/inspectormodel.cpp#L3832))。
- engine に繋ぎ直しても Studio は持っている scene と revision を捨てないので、繋ぎ直した後の最初の poll も(scene が 1 つなら)条件付きです
  (コードの読み)。同じ revision の答えで作り直さないのは、UX-13c の前の `scene_tree` の読みでも同じでした。
- **重さ**: commit の記録では、1,000 object の test(Studio の test の fake の engine)で一覧全体の答えが約 104 KB、変わらない poll の答えが
  58 byte です。engine の側の時間は上の §13.5 の節です。シナリオ(`outliner_edit_roundtrip` など)は `scene_tree` を直接呼んで親の名前を見るので、
  変えていません。
- test: `devstudio_outliner_test` の [「UX-13c the Outliner hangs an object under its parent's authoring id」](../../test/devstudio_outliner_test.cpp#L719)・
  [「UX-13c a not_modified poll rebuilds nothing and a new revision rebuilds」](../../test/devstudio_outliner_test.cpp#L757)、
  `devstudio_outliner_edit_test` の [「UX-13c the Outliner polls scene_outline with the revision it holds」](../../test/devstudio_outliner_edit_test.cpp#L643)・
  [「UX-13c an engine without scene_outline is read with scene_tree, asked once」](../../test/devstudio_outliner_edit_test.cpp#L685)。

## 15.4 Materials

### Studio の Materials のドック —— surface の宣言から作る欄、New material…、object への割り当て(✅MT-1s)

**EB-6s の追加**: pass の宣言不足で material が `unusable` になったときは、保存済みであることと直す pass を表示します。**Open the pass** から宣言を直すと、engine を再起動せずに割り当て済みの material が使える状態へ戻ります。**Reload from file** は外で保存した登録済み material を読み直します。surface の routing は engine が返す候補から選びます。Problems の未解決 object material の行からは Inspector の Material へ移動できます。

material の修復待ちは、エラー文に加えて文書の digest で更新を判定します。material・scene の問題は render の fallback 通知とは分けて表示します。長い拒否の文は折り返し、ドックが viewport を押しつぶさないようにします。

✅MT-1s(`2d81ef51`)から、Studio に **Materials のドック**(`pelican.materialsDock`。中身は
[`MaterialsDock`](../../src/devstudio/view/materialsdock.hpp#L43)、RPC のやり取りは Qt を使わない
[`MaterialsDockSession`](../../src/devstudio/model/materialsmodel.hpp#L334))と、Inspector の **Material** の行
([`MaterialAssignWidget`](../../src/devstudio/view/materialassignwidget.hpp#L31)、やり取りは
[`MaterialAssignSession`](../../src/devstudio/model/materialsmodel.hpp#L600))があります。project の material を一覧し、
surface の宣言から作った欄で値を直し、新しい material を作り、object に割り当てる場所です。Studio は material の
ファイルを自分で書かず(D0)、engine の ✅MT-1 の 7 本(§13.5 の「material 文書を読み書きする(✅MT-1)」)と ✅MT-1b の
`list_surfaces`(下の New material…)、✅MT-2 の `set_object_material` / `get_object_materials`(下の「割り当て」)だけを送ります。
✅MT-2s(`7112072e`)から、Inspector の Material の行は**既定で選んだ object だけ**に割り当て(`set_object_material`)、
切り替えると MT-1s と同じ model 全部への割り当て(`bind_material`)になります。値の変更は engine が起こし直さずに当てるので、**Blank から作った project でも、
Cube を置いてその色を変えるまでが GUI だけで、再起動なしで済みます**(下のシナリオ)。MT-1s の前は、色 1 つに JSON を 3 つ手で書いて
engine を起こし直していました(仕様書の記述)。

- **開き方**: 既定の配置では Assets のドックの隣のタブです([既定の配置](../../src/devstudio/view/mainwindow.cpp#L3965))。
  **View > Materials**(`pelican.view.materials`)で前に出します([View > Materials](../../src/devstudio/view/mainwindow.cpp#L1910))。
  View > Panels でも開閉できます。
- **一覧**(決定 1): 上の木(`pelican.materials.list`。列は Material / Surface)は、`list_materials` の文書ごとの行と、その下の
  material の行です。`status` が `missing` / `invalid` の文書は灰色で、Surface の列に `<status>: <理由>` が出て、選べません。
  engine の ✅FO-1(`a4d7760a`)から、起動時に engine が断った文書の `status` と `reason`(JSON でなければ `line` / `column` も)は、ファイルを読んだ結果ではなく engine の状態です。ファイルを直しても、engine を起こし直すまで `invalid` / `missing` のままです。そうした行に「直したら起こし直す」の案内を出すのは、FO-1 の実装者が Studio 側(FO-1s)に挙げた項目です。
  → ✅FO-1s(`f186320f`)で、invalid の文書の行が選べるようになり、行・列の 1 文と Show the document が出ます(下の FO-1s の段落)。engine の ✅EB-2(`e1ba9058`)からは、直した文書を `register_material {path}` で起こし直さずに取り込めます(取り込んだ後は `loaded`)。
  → ✅MR-1s(`5c26d492`)から、Studio は Show the document の Save の後と Register again でこれを送り、再起動の案内は出しません(下の「壊れた material を直して Save で戻す(✅MR-1s)」)。
  → engine の ✅FO-2(`31c390c7`)から `status` に `unusable`(文書は読めるが、それを描く pass が material の使うものを宣言していない)が加わりました。✅RTL-3s(`c80bc6a1`)から、その行は選べて、理由の 1 文と **Open the pass** が出ます(下の MR-1s の段落の後の「pass が宣言していない material の行(✅RTL-3s)」)。
  この engine にまだ登録されていない material(`registered: false`)は名前が灰色で、tooltip が「engine の起動時に読まれる」と言います
  ([木を作る所](../../src/devstudio/view/materialsdock.cpp#L1006))。文書の行を選ぶと、その最初の material を選びます。
- **読み直し**: ドックが見えている間だけ、`list_materials` を 1 秒ごとに送ります([`PollMs`](../../src/devstudio/model/materialsmodel.hpp#L340)。
  隠れると止め、見え直したときに 1 回読みます)。答えが前と同じ(digest を含めて)なら何もしません。選んでいる material の文書の digest が、
  このドックの書き込みでないのに動いていれば(外で書かれた)、その material を読み直します([一覧の答えを読む所](../../src/devstudio/model/materialsmodel.cpp#L1114))。
  Assets のドックは時計で読まない(ファイルの監視)ので、間隔は揃えていません(実装者の報告)。

**エディタ —— surface の宣言から作る欄**(決定 2・3)

material を選ぶと、`get_material` で文書のままの material を、続けてその `surface` の `get_surface_schema` を読み、宣言の
`params` / `textures` から欄を作ります(見出しは `<名前> — <path>`)。param の欄の種類は
[`materialFieldKind()`](../../src/devstudio/model/materialsmodel.cpp#L334) が決めます。

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
  その文書について最後に知った digest を `base_material_digest` にします([送る所](../../src/devstudio/model/materialsmodel.cpp#L816))。
- **色**: encoding が `linear` の色は、swatch では sRGB に直して見せ、選んだ色を linear に戻して保存します(小数 6 桁)。`srgb` の色は
  見たままです([見せる色 `materialColorForDisplay()`](../../src/devstudio/model/materialsmodel.cpp#L392)、
  [保存する色 `materialColorFromDisplay()`](../../src/devstudio/model/materialsmodel.cpp#L404))。alpha は今の値のままで、swatch の文字は
  見せている色の `#rrggbb` です。色のダイアログ(`pelican.materials.colorDialog`)は Qt のダイアログで、hex の欄(`.hex`)と OK / Cancel
  (`.ok` / `.cancel`)に名前が付いているので、シナリオから打てます。
- **default との違い**: 文書が default と違う値を書いている param は、名前が太字です([`materialParamOverridden()`](../../src/devstudio/model/materialsmodel.cpp#L352))。
  名前の右クリックの **Reset to default**(`.reset`。default と違うときだけ押せる)は `values: {<名前>: null}` を送り、文書からその値を
  外します。名前の tooltip は型・hint・範囲と `Default: <値>` です。
- **テクスチャ**: `textures` の slot ごとに combo(`pelican.materials.texture.<名前>`)です。先頭の `None (<宣言の default>)` は `null` を
  送り(文書から外す)、続いて project の `assets/` の下の画像のうち、slot の `dimension` に合うものが `project://<論理 path>` の値で
  並びます(表示は `project://assets/` を外した path)。画像の次元は Studio がファイルから推します: KTX2 はヘッダの depth / layer / face の
  数から `2d` / `cube` / `cube_array` / `2d_array` / `3d`、`.png` / `.jpg` / `.jpeg` / `.tga` / `.bmp` / `.hdr` / `.exr` は `2d` です
  ([`textureFileDimension()`](../../src/devstudio/model/materialsmodel.cpp#L523))。tooltip は color space・role・dimension で、文書が
  名指す texture があれば slot の名前が太字です。
- **Surface・Tags・Alpha mode・Variants**(決定 3): **Surface** の combo は `list_surfaces` の一覧です(一覧に無い今の値はそのまま足して
  見せます)。**Tags** は UX-11 のタグ選びで、候補は一覧の material が持つタグ、`+ new tag` で新しいタグも足せます。**Alpha mode** は
  `opaque` / `mask` / `blend` の combo と **Double sided** の check で、`routing` を丸ごと送ります。**Variants** は名前の一覧を読むだけです
  (tooltip `Variants are edited in the document itself for now`)。どれも変更 1 回が `edit_material` 1 回です。MT-1 の engine は textures の変更と同じく
  書いたうえで `restart_required` を返していましたが、engine の ✅MT-3(`c6f94eb7`)から、engine が登録している material ではその場で
  登録し直して描いている model を組み直し、`applied: "rebuilt"`・`restart_required: null` を返します(§13.5 の規則)。Studio は
  `restart_required` が `null` でないときだけ起こし直すので([`MaterialsDockSession::writeResult()`](../../src/devstudio/model/materialsmodel.cpp#L1216))、
  この場合は起こし直しません。Alpha mode の変更は、engine が同じ書き込みでその material を指す binding の routing も書き直します。
  engine の ✅VR-1(`45864d55`)から、OpenPBR の surface では Surface だけ・Alpha mode / Double sided だけの変更も、engine が
  もう片方(routing / surface)を manifest の組に合わせて同じ書き込みで書きます(§13.5 の規則)。VR-1 の前は、Surface を別の variant に
  替えると routing と合わずに断られました(下の「MT-3s の残した穴」)。Studio はドックの書き込みの後に選んでいる material を読み直すので
  ([書いた答えの後に読み直す所](../../src/devstudio/model/materialsmodel.cpp#L1237))、読み直した文書の routing が Alpha mode / Double sided の欄に入ります
  ([routing を欄に入れる所](../../src/devstudio/view/materialsdock.cpp#L1568))。✅SK-1(`67fd3d29`)のシナリオ `gui_material_from_blank` で確かめました:
  Surface を `openpbr / opaque_double_sided` に替える(送るのは surface だけ)と、起こし直さずに(`get_status` の `instance_id` が同じ)Double sided に
  印が付き、Alpha mode は `opaque`、Surface の欄は double-sided のままです。ドックの Undo で surface と routing が両方戻り、Double sided の印が外れて
  Surface の欄も one-sided に戻ります(§13.5「Studio の自動化面」の SK-1 の段落)。Studio のドックのコードは SK-1 で変えていません。
- **`rebuilt` の答え(✅MT-3s、`19be8983`)**: ドックの書き込み(`edit_material` とその Undo)の答えが `applied: "rebuilt"` なら、ドックの notice が
  `Applied; rebuilt N models.`(N は `rebuilt_models` の数。1 なら `Applied; rebuilt 1 model.`)を言い、Restart engine のボタンは出ません
  ([rebuilt を読む所](../../src/devstudio/model/materialsmodel.cpp#L1351))。答えの `rewritten_bindings` に載った model ごとに、Inspector の Material の行が
  binding を読み直します(MainWindow が `InspectorWidget::materialBindingsChanged()` を呼ぶ。Undo が前の routing を送ったときも、engine が binding を戻すので同じです)。
  Surface・Tags・Alpha mode・Double sided の tooltip も、MT-3s で「engine がその場で登録し直す / binding も書き直す」旨に直しました(MT-1s では「engine の起動時に読む」)。
  答えを待つ間と、その後に material を読み直す間は、Surface と Alpha mode / Double sided を文書の値で埋め直さず、選んだ値を見せたままにします
  ([埋め直さない条件](../../src/devstudio/view/materialsdock.cpp#L1555))。MT-1s ではこの間に一瞬前の値へ戻って見え、シナリオの `select` が「値が入らなかった」で
  落ちていました(実装者の報告)。
- **`restart_required`**: 書く答えの `restart_required` が `null` でなければ、Studio はその場で engine を起こし直します(RSX と同じ
  `MainWindow::requestEngineRestart()`。未保存の scene があれば今までの Save / Discard / Cancel が出ます)。ドックの notice には
  `Restart the engine to apply: <理由>` と **Restart engine**(`pelican.materials.restartEngine`)が残ります(Assets のドックと同じ)。
  Inspector の Material の行の `restart_required` も同じ道です。

**New material…**(決定 4)

- ドックの上の **New material…**(`pelican.materials.newMaterial`)を押すと、木の上に行が開きます(modal のダイアログではありません)。
  **Surface**(`pelican.materials.new.surface`)は engine の `list_surfaces`(✅MT-1b。engine に登録された `surfaces/**.surface` と project の
  `*.surface`。[engine の契約の注記](../../src/core/communication/materialdocumentrpc.hpp#L45))から作り、**Studio は surface の一覧の写しを
  持ちません**。表示は、engine の manifest が名前を付けた variant なら `<family> / <variant 名>`(例 `openpbr / opaque_single_sided`)、ほかは
  `surfaces/` の下の path から `.surface` を外したもの(project のものは `shaders/toon` など)です([`surfaceChoiceLabel()`](../../src/devstudio/model/materialsmodel.cpp#L442))。
  読めない surface(`error` 付き)は灰色で選べず、tooltip に理由が出ます。最初に選ばれているのは
  `engine://surfaces/openpbr/opaque_single.surface` があればそれ、無ければ読める最初のものです([`defaultSurfaceChoice()`](../../src/devstudio/model/materialsmodel.cpp#L455))。
- **Name**(`pelican.materials.new.name`)には空いている名前が入って開きます。`material`、使われていれば `material_2`、`material_3`… の、
  一覧の material の名前・一覧の文書の path・project の `materials/<名前>.material.json` のファイルのどれとも重ならない最初のものです
  ([`suggestMaterialName()`](../../src/devstudio/model/materialsmodel.cpp#L497))。その下に書く先 `materials/<名前>.material.json`
  (`pelican.materials.new.path`)が出ます。名前が英数字と `_` でない・もうある名前のときは、行の中に 1 文が出て **Create** が押せません。
- **Create**(`pelican.materials.new.create`)は `create_material {path, surface, name}` を 1 回送ります。選んだ surface の `list_surfaces` の
  答えが `routing`(OpenPBR の variant の `alpha_mode` / `double_sided`)を持っていれば、それも `routing` に載せます
  ([`MaterialsDockSession::create()`](../../src/devstudio/model/materialsmodel.cpp#L868))。engine は live で登録するので、起こし直しは要りません。
  notice に `Created <名前> in <path>.` が出て、次の `list_materials` にその material が現れた時点でそれを選びます。
- engine が `list_surfaces` を知らない(`-32601`)ときは、行が `This engine does not list surfaces (list_surfaces), so no material can be made here yet.`
  を言い、Create は押せません。
- 作った material は Undo の対象ではありません(engine に `delete_material` が無いため)。確認なしで作ります。
  → engine は ✅EB-1(`aaa7b2da`)で `delete_material` / `register_material` / `rename_material` を持ちました(§13.5 の「material を消す・戻す・改名する」)。Studio はまだ呼びません。→ ✅FO-1s(`f186320f`)から、ドックの行の右クリックの Delete… が `delete_material` を呼び、その Undo が `register_material` です(下の FO-1s の段落)。

**割り当て**(決定 5。✅MT-2s で object ごとに)

- **Inspector の Material の行**: `simplemodelview` の `model` を持つ object を選ぶと、Inspector の Attached graphs の下(properties の scroll
  より上)に **Material** のグループ(`pelican.inspectorMaterial`)が出ます([置く所](../../src/devstudio/view/inspectorwidget.cpp#L330))。
  model の無い object と、再生中に spawn された object では出ません。
- **範囲の切り替え**(✅MT-2s の決定 1): グループの一番上の combo(`pelican.inspectorMaterialTarget`)で、選ぶ material が何に効くかを
  決めます。項目は **This object**(既定)と **Every <model>**(例 `Every Cube`。名前は `(built-in)` を外した model の名前。
  [`modelShortName()`](../../src/devstudio/model/materialsmodel.cpp#L602))です。切り替えは Material の行の session が持つので、
  別の object を選んでも選び直すまで残ります(シナリオの step 119 が、Cube から Cube_001 に移っても Every Cube のままであることを見ます)。
- **This object**: material を選ぶと、選んだ object の全 primitive の行を `set_object_material` 1 回で書きます。その object が primitive ごとの
  行を持っていれば、同じ 1 回でそれも外します(残すと primitive の行が勝ち、選んだ material にならないため。実装者の判断)
  ([`MaterialAssignSession::choose()`](../../src/devstudio/model/materialsmodel.cpp#L1749))。今と同じ material なら送りません。
  - **送り方**(決定 5): Material の行は自分で送らず、Inspector の scene の編集の session(`InspectorModel`)に渡します。session が、
    scene の編集で使っている `actor_id` と、最後に見た scene の revision を `base_scene_revision` に足し、自分の編集の 1 つとして送ります
    ([`InspectorModel::submitObjectMaterial()`](../../src/devstudio/model/inspectormodel.cpp#L4526)、
    [Material の行から渡す所](../../src/devstudio/view/inspectorwidget.cpp#L340))。そのため **scene の Undo**(Inspector の Undo のボタン。Ctrl+Z も RU-1 から
    同じ Undo です)で戻ります(下の「Undo」)。別の編集が進んでいる間は送らず、行に `Finish the edit in progress first, then choose the material again.` が出ます。
    答えが `changed: false` なら編集にせず、Inspector の notice は `<何を>: nothing to change.` です。
  - **今の状態**(決定 2): Material の行は選んだ object の `get_object_materials` を読み、combo の先頭(選べない項目)にその object が
    描いている material を出します。object 自身の行から来ていれば `<名前> (this object)`、model の既定なら `(model default: <名前>)` です
    (例 `material_2 (this object)`、Blank の Cube は `(model default: pelican.primitive.default)`。名前は `drawn`、instance が無ければ
    `material`。[`objectMaterialLabel()`](../../src/devstudio/model/materialsmodel.cpp#L656))。primitive ごとに違うときは
    `(differs per primitive)` です。続いて `list_materials` の名前が並びます。答えを待つ間は選んだ名前を示したまま combo を止め、
    Inspector の編集が終わってから読み直します。
  - **Use model default**(`pelican.inspectorMaterialUseModelDefault`。This object のときだけ見え、object が自分の行を持つときだけ押せる):
    object の行を全部 `material: null` で外す `set_object_material` を 1 回送ります([`MaterialAssignSession::useModelDefault()`](../../src/devstudio/model/materialsmodel.cpp#L1826))。
    combo の項目ではなくボタンにしたのは、combo の先頭が今の状態の表示で選べないためです(実装者の判断)。
  - 下の 1 行(`pelican.inspectorMaterialScope`)は `Applies to <object の名前> only.`、tooltip は `Every other <model> keeps the model's material` です
    ([`objectScopeLine()`](../../src/devstudio/model/materialsmodel.cpp#L620))。
  - **読み直し**: Inspector が持つ object の scene の revision が動くたび(自分の編集・Undo・ほかの client)に、`get_object_materials` と
    `get_material_bindings` を読み直します。Inspector の session に編集が進んでいる間は行が待ち、終わったら読み直します
    ([`MaterialAssignSession::setSubject()`](../../src/devstudio/model/materialsmodel.cpp#L1598)、
    [`MaterialAssignSession::setSceneBusy()`](../../src/devstudio/model/materialsmodel.cpp#L1630))。
  - `get_object_materials` を知らない engine(`-32601`)では、行が `This engine cannot give an object its own material (no get_object_materials); choose Every <model>.`
    と言います([その文](../../src/devstudio/model/materialsmodel.cpp#L1913))。
- **Every <model>**(MT-1s の割り当て): combo は **Model default**、区切り、`list_materials` の material の名前の順です。material を選ぶと
  `bind_material {model, material}`(model の全 primitive)を 1 回送ります。Model default は、割り当てを変えた primitive をそれぞれ model 自身の
  material(`get_material_bindings` の `slot`)に戻す `bindings` を送ります。primitive ごとに違う material を持つときは、combo は選べない項目
  `(differs per primitive)` を示します。MT-2s の着地時、engine の基本形(`engine://primitives/cube` など)では
  この Model default が engine に断られました。engine の ✅MT-2b(`213562bb`)から、engine はこのとき割り当ての宣言を外し、答えの
  `material_bindings` は `null` です。✅MT-2s-b(`9d15ddc0`)のシナリオで、基本形の Every Cube の Model default の後に `get_material_bindings` の
  `material_bindings` が `null`、primitive 0 が `pelican.primitive.default` になり、combo が `Model default` を言い、両方の箱が灰になることを確かめました。
  Studio は bind の答えの `material_bindings` を読まず、bind の後に `get_material_bindings` を読み直して、全 primitive の material が `slot` と
  同じなら combo を Model default にします([その判定](../../src/devstudio/model/materialsmodel.cpp#L1693))。全 primitive が既に `slot` のままなら
  Model default は何も送りません。送った後に別の client が外していて engine が `changed: false` を返したときは、Undo の項目を積まずに読み直すだけで、
  行はまた選べます([changed と以前の割り当てを確認する条件](../../src/devstudio/model/materialsmodel.cpp#L1879)。✅MT-2s-b の決定 3。GUI からは起こせないので test だけ)。
  - 下の 1 行は `Applies to every <model> in this project (N objects, M with their own material).` です(例
    `Applies to every Cube in this project (2 objects, 1 with their own material).`。[`modelScopeLine()`](../../src/devstudio/model/materialsmodel.cpp#L610))。
    tooltip は `A material is bound to the model (<model>); an object with its own keeps it` です。
  - **N と M**(決定 4): N は `get_material_bindings` の `objects`(その model を描く scene の object の数。engine は全 scene と prefab から
    生成された object も数えます)、M は `object_overrides`(そのうち自分の行を持つ数)です([`parseModelObjectCounts()`](../../src/devstudio/model/materialsmodel.cpp#L588))。
    答えに無ければその数を省きます。scene の revision が動くたびに読み直すので、同じ model の object を足すと N が増えます(シナリオの step 120)。
    MT-1s の `export_scene_snapshot` で数える道は消しました。置き換える前に実装者が両方を並べて 1 回流し、Cube 1 つで 1 = 1、
    Cube 2 つで 2 = 2(上書きが 1 つある状態でも)と同じ値でした(報告)。数え方の違い(snapshot は今の scene の prefab の外だけ)は
    Blank では差が出ません。
- **primitive が複数ある model**(決定 3): その下に `Mesh <n> · primitive <m>` の行ごとの combo(`pelican.inspectorMaterialPrimitive.<番号>`)が
  出ます。数は This object なら `get_object_materials` の、Every <model> なら `get_material_bindings` の `primitives` です。行も範囲に従います
  ([`MaterialAssignSession::choosePrimitive()`](../../src/devstudio/model/materialsmodel.cpp#L1783)):
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
  MT-2s から、Inspector は flat な `schema.fields` の `list` 型を欄にせず飛ばします([飛ばす所](../../src/devstudio/model/inspectormodel.cpp#L365))。
  list の行はもともと schema の `node` から作るもので、simplemodelview の `/materials` はそこでも汎用の行にせず、
  `Chosen in the Material rows above`(`pelican.inspectorListElsewhere`)の 1 行にします([`inspectorListShown()`](../../src/devstudio/model/inspectormodel.cpp#L2142))。
  実装者の報告では、engine は今 simplemodelview に `node` を送らないので、この 1 行は今は出ません(engine が送るようになったときの備えで、
  test だけが見ます)。

**Undo**(決定 6)

- ドックの **Undo**(action `pelican.materials.undo`。ボタンは木の上の右の `pelican.materials.undoButton`、ドックの中での Ctrl+Z も)は、
  ドックの書き込みを 1 回ずつ戻します。`edit_material` は答えの `previous` を、その書き込みの後の `material_digest` で同じ RPC に送り、
  Assign to selected object の `bind_material` は `previous.bindings` を `bindings` に送ります([`MaterialsDockSession::undo()`](../../src/devstudio/model/materialsmodel.cpp#L910))。
  Problems から開いたドック内の model の割り当て行も、この履歴に入ります。例えば Model default に替えてから material を削除した場合、Undo は文書の再登録、割り当ての復元の順です。
  ボタンの文字は `Undo <何を>` で、書き込みの答えを待つ間は押せません。project を替えると消え、engine の起こし直しでは残ります(コードの読み)。
- **This object の割り当て(`set_object_material`)は scene の Undo で戻ります**(✅MT-2s の決定 5)。Inspector の scene の編集の session の
  `actor_id` で送るので、Inspector の Undo(`pelican.inspectorUndo`)が engine の `undo {actor_id}` でそれを戻し、Redo も同じです
  (シナリオの step 202〜205 が、Use model default の後に Inspector の Undo で上書きが戻ることを見ます)。Material の行は revision が動いたのを
  見て読み直します。
- Inspector の Material の行の **Undo**(`pelican.inspectorMaterialUndo`)は、Every <model> の `bind_material` だけを `previous.bindings` で
  戻します。履歴に項目があるときだけ見えます。ドックとは別の履歴で、Edit メニューの Undo Render Edit や Inspector の scene の Undo とも別です。
  project を開くと、この行の履歴と未完了の応答を破棄します。同じ project 内の engine 再起動では履歴を保ちます。
  ✅MT-2s から、項目は送った先の model を覚え、押した時点で別の object を選んでいても、その model に送り返します
  ([`MaterialAssignSession::undo()`](../../src/devstudio/model/materialsmodel.cpp#L1837))。MT-1s では押した時点の model に送っていました
  (下の「残した穴」の MT-1s の項)。MT-2s の着地時、engine の基本形ではこの Undo が engine に断られました。
  ✅MT-2s-b のシナリオで、基本形でも Every Cube の Model default を行の Undo で戻すと model が `material_2` に戻り、両方の箱が赤に戻ることを確かめました
  (Undo の表示は 1 つ前の項目 `Undo material_2 on every Cube` になります)。
- `create_material` は戻せません(上)。

**拒否**(決定 7)

- MT-1 の拒否 13 種(`material.*@1`)、✅MT-2s で足した `material.object_unknown@1`(`Object <object> cannot take a material of its own: <理由>`。
  ✅MT-2s-b から、engine が `name` を付けたとき(空でない文字列)は `<name> cannot take a material of its own: <理由>`。
  [名前で言う所](../../src/devstudio/model/inspectormodel.cpp#L2049))、
  `external_modification` を、Studio の 1 文の表で `<文> (<code>).` にします
  (表 [`MaterialRefusalTexts`](../../src/devstudio/model/inspectormodel.cpp#L1983)、[`materialRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L2024))。
  例: `materials/material.material.json changed outside Studio since it was read; it was read again, try again (external_modification).`。
  表に無い code は engine の message のままです。
- ✅MT-3s で engine の ✅MT-3 の `material.routing_conflict@1`(binding に指されている material の routing を外す)を同じ表に足しました
  ([表の行](../../src/devstudio/model/inspectormodel.cpp#L1435)、[`bound_by` から model を並べる所](../../src/devstudio/model/inspectormodel.cpp#L2036))。
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
  ([`objectMaterialMissingLine()`](../../src/devstudio/model/materialsmodel.cpp#L662))。

**残した穴**(着地時の記録):

- **作った後に名前を変えられません。**仕様は「edit_material の name」で変えるとしていましたが、engine の `edit_material` の `name` は対象の
  material を選ぶ鍵で、改名ではありません。`delete_material` も無いので、作り損ねた material を GUI で消せません(engine に回しました)。
  → engine 側は ✅EB-1(`aaa7b2da`)の `rename_material` / `delete_material` で済みました(§13.5 の「material を消す・戻す・改名する」)。Studio の欄はまだありません。→ ✅FO-1s(`f186320f`)で、ドックの行の右クリックに Delete… と Rename… が入りました(下の FO-1s の段落。Rename は unit test だけ)。
- **variants は読むだけ**です(編集の欄はありません)。
- **テクスチャの候補は project の `assets/` の下だけ**です(asset store の別の mount は入りません)。次元は Studio の推定です。
- **再起動の道(tags / routing / surface / textures の `restart_required`)はシナリオで通していません**(test だけ)。engine の ✅MT-3 から、
  engine が登録している material ではこれらは `rebuilt` になり、再起動の道に入るのは登録していない material・登録し直しか組み直しの失敗の
  ときだけです。`rebuilt` の答えを受けた Studio の振る舞いを通した test・シナリオはありません(未確認)。
  → ✅MT-3s(`19be8983`)で test(下の「MT-3s decision 5」)とシナリオ(`gui_material_from_blank` の tag の段)が通りました(上の「`rebuilt` の答え」)。
  再起動の道(`restart_required`)は MT-3s の test が今までどおり再起動することを見ますが、シナリオでは通していません。
- engine の ✅MT-3 の `material.routing_conflict@1`(binding に指されている material の routing を外す)は、Studio の拒否の文の表
  ([`materialRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L2024))に無いので、engine の message がそのまま出ます(コードの読み)。
  → ✅MT-3s で表に足しました(上の「拒否」)。
- ドックの routing / surface の combo は、edit の答えが来るまで一瞬前の値に戻って見えます(答えで正しくなります。実装者の報告)。
  → ✅MT-3s で直りました(上の「`rebuilt` の答え」)。
- MT-1s の着地時に、この文書を書いた側がコードの読みで書いた 2 つ —— Inspector の Material の行の Undo が戻す割り当ての model を覚えず、
  押した時点の model に送ること、N が同じ model の object を足しても読み直さないこと —— は、✅MT-2s で直りました(上の「Undo」と「N と M」)。
- engine に回したもの: 改名と `delete_material`(✅EB-1 で入りました)、surface の variant の人が読む名前、model を使う object の数を `get_material_bindings` で
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
  [「MT-1s decision 1: the Materials dock lists the documents and their materials, a missing one greyed with its reason, and a poll whose digests are the same changes nothing」](../../test/devstudio_materials_test.cpp#L369)・
  [「MT-1s decision 2: the editor's rows are made from the surface's params -- a slider and a spin, a swatch, a spin, a texture combo by dimension -- and each change is one edit_material」](../../test/devstudio_materials_test.cpp#L409)・
  [「MT-1s decision 3: tags, routing and surface are one edit each, and restart_required restarts the engine as every other panel's edit does」](../../test/devstudio_materials_test.cpp#L501)・
  [「MT-1s decision 4: New material… offers the engine's surfaces, proposes a free name and its file, creates it and selects it」](../../test/devstudio_materials_test.cpp#L549)・
  [「MT-1s decision 5: the Inspector's Material combo binds every primitive of the model, says it is the model's, has a row per primitive when there are several; the dock assigns to the selected object」](../../test/devstudio_materials_test.cpp#L633)・
  [「MT-1s decision 6: Undo sends an edit's previous against the digest after it, and a bind's previous bindings; a new material is not in it」](../../test/devstudio_materials_test.cpp#L701)・
  [「MT-1s decision 7: a refused value is said under its row; any other refusal is one sentence in the dock's notice」](../../test/devstudio_materials_test.cpp#L773)。
  `devstudio_layoutpreset_test` はドックの数を 8 から 9 にし、古い保存済みの配置を戻しても Materials のドックが置き場を持つことを見ます。
  ✅MT-2s は同じ test の fake に `edit_object` を足し、MT-1s の decision 5 / 6 を新しい API(Every Cube に切り替えてから選ぶ)に合わせ、
  [「MT-2s decision 1: a choice applies to this object first (set_object_material through the Inspector's session); Every Cube is the model's bind_material」](../../test/devstudio_materials_test.cpp#L809)・
  [「MT-2s decision 2: the combo shows what is drawn and where it comes from; Use model default takes the object's own away」](../../test/devstudio_materials_test.cpp#L860)・
  [「MT-2s decision 3: with several primitives, each row follows the target -- This object sends set_object_material for that primitive」](../../test/devstudio_materials_test.cpp#L885)・
  [「MT-2s decision 4: N and M are get_material_bindings' objects and object_overrides, read again whenever the scene's revision moves」](../../test/devstudio_materials_test.cpp#L939)・
  [「MT-2s decision 5: an object's material is the scene's Undo; the rows' own Undo takes a bind back to the model it was sent to, whatever is selected by then」](../../test/devstudio_materials_test.cpp#L962)・
  [「MT-2s decision 6: a simplemodelview's materials list is chosen in the Material rows, not typed in the generic list」](../../test/devstudio_materials_test.cpp#L1058)・
  [「MT-2s decision 7: a refused set_object_material is one sentence in the rows; a material that is not loaded is marked」](../../test/devstudio_materials_test.cpp#L1079)
  の 7 本を足しました。`devstudio_inspector_test` の
  [「MT-2s decision 5: the Material rows' set_object_material is an edit of the Inspector's own session, so its Undo is the scene's Undo」](../../test/devstudio_inspector_test.cpp#L2762)
  は、`actor_id` と `base_scene_revision` が足されること、`changed: false` が編集にならないこと、拒否が未保存扱いにならないこと、
  commit の後の Undo が同じ actor で送られることを見ます。実装者の run で `ctest -R 'MT-1s|MT-2s'` 15 / 15、`studio_cpu` 702 / 702 が緑です。
  ✅MT-2s-b は同じ `devstudio_materials_test` に
  [「MT-2s-b decision 1: a refused object is called by its name when the engine gives one」](../../test/devstudio_materials_test.cpp#L1111)・
  [「MT-2s-b decision 2: Model default on Every Cube is taken off (material_bindings null), reads as Model default, and its Undo binds the material again」](../../test/devstudio_materials_test.cpp#L1158)・
  [「MT-2s-b decision 3: a Model default the engine has already (changed:false) leaves the rows choosable, with nothing to undo」](../../test/devstudio_materials_test.cpp#L1211)
  の 3 本を足しました(test が返す答えは engine の MT-2b の形です)。実装者の run で `ctest -R 'MT-1s|MT-2s' -LE gpu` 18 / 18、`studio_cpu` 705 / 705 が緑です。
  ✅MT-3s は同じ test に
  [「MT-3s decision 5: an edit the engine rebuilt in place restarts nothing and says how many models it rebuilt; a routing edit's rewritten bindings are read again, its Undo too; a bound material's routing taken off is one sentence」](../../test/devstudio_materials_test.cpp#L1239)
  の 1 本を足しました(tag の `rebuilt` で再起動せず `Applied; rebuilt 1 model.`、routing の `rebuilt` で `Applied; rebuilt 2 models.` と `rewritten_bindings` の model の読み直し、
  答えを待つ間 Alpha mode の combo が選んだ値のまま、Undo が前の routing を送り binding を読み直す、`restart_required` は今までどおり再起動、`material.routing_conflict@1` の 1 文)。
  実装者の run で MT-1s / MT-2s / MT-2s-b / MT-3s の 19 / 19、`studio_cpu` 711 / 711 が緑です(BS-1s と同じ run)。
- シナリオ: `gui_material_from_blank`(Blank から GUI だけで。§13.5「Studio の自動化面」)。

**Order… の拒否の 1 文、material の Delete… / Rename…、Problems から Materials へ(✅FO-1s、`f186320f`)**: engine の ✅FO-1(`a4d7760a`)と ✅EB-1(`aaa7b2da`)の Studio 側です。

- **Order… の拒否**(決定 1): 順序のダイアログ(Frame Plan の Order... と Graph の Order…。§13.5 の GL-5d の段落)の Apply が `frame_node_order.conflicts_dependency@1` で断られると、ダイアログは閉じず、並びはそのまま、notice が 1 文になり、並びが逆にした 2 行(`first` と `second`)の文字が赤くなります(tooltip も同じ文。[`refused()`](../../src/devstudio/view/renderfeatureswidget.cpp#L183))。行を動かすか送り直すと赤は消えます。
  - 文は `<second> must stay before <first>: <理由>` で、理由は `reason` ごとです([表の行](../../src/devstudio/model/inspectormodel.cpp#L1428)、[理由を作る `frameNodeOrderWhy()`](../../src/devstudio/model/inspectormodel.cpp#L1605))。`reads` なら `left must stay before present: present reads left_color, which left writes (frame_node_order.conflicts_dependency@1).`、`after` / `before` は「set to run after / before」、`via` があれば間の pass、`canonical_order` は「the stages of a frame run in a fixed order」です(実装者の報告の例)。
  - 文は拒否の表(`graphLoopRefusalDescription()`)に入れたので、起動時と reload の同じ拒否(Problems の行・帯)も同じ文です。
  - シナリオ `gui_graph_order` を広げました(json の step は 54): 依存の無い入れ替えが committed になって plan の順と画素を確かめ、`present` を前へ動かした並びが上の 1 文で断られ、engine の順が変わらないことを見ます。
- **起動時に断られた material 文書を直す道**(決定 2): Problems の error の行(`material.document_invalid@1`)を押すと Materials のドックが前に出て、その文書の行が選ばれ、編集欄の代わりに直す欄(`pelican.materials.broken`)が `<path> would not load at line L, column C: <理由>` を言います([`materialDocumentProblemLine()`](../../src/devstudio/model/materialsmodel.cpp#L171))。
  - **Show the document** で窓(`pelican.materials.documentDialog`)が開きます。生の文字で、カーソルは engine が言う行・列、JSON になるまで Save は押せず、Save はファイルを丸ごと書きます(CRLF は保つ)。material 文書は render 文書ではないので、Studio が直接書きます(PB-1s の判断と同じ)。
  - warning の行(`material.binding_unresolved@1`)は、ドックに足した model の bindings の行(`pelican.materials.model`。Inspector と同じ `MaterialAssignSession` の Every <model>)へ飛びます。
  - 直した文書を engine に取り込ませる所は、FO-1s の着地の時点では Register again と再起動の案内でした(engine が `material.path_conflict@1` で断ったため)。✅MR-1s(`5c26d492`)から、Show the document の Save が engine の ✅EB-2 の `register_material {path}` を送って起こし直さずに取り込み、窓が閉じてドックが「<name> is loaded again.」と言います。起動後に壊れた文書の行も加わりました(下の「壊れた material を直して Save で戻す(✅MR-1s)」)。
- **Delete… / Rename…**(決定 3): ドックの material の行の右クリックに **Delete…**(`pelican.materials.delete`)と **Rename…**(`pelican.materials.rename`)があります。
  - Delete… はドックの中の確認の行の後に `delete_material {name}` を送ります。通れば `Deleted <name>; <path> stays on disk.` で、Undo は `register_material {path}` です(シナリオで確かめた)。
  - `material.in_use@1` は `<name> is used by the bindings of Cube; take it off them first (material.in_use@1).` のような 1 文で、その下に場所ごとのボタン(`pelican.materials.uses.*`)が並びます([1 文を作る `materialDeleteRefusalDescription()`](../../src/devstudio/model/materialsmodel.cpp#L187))。bindings はドックの model の行(Model default で外せる。シナリオで使った)、objects は Outliner のその名前の行、render 文書はその文書の文字を material の名前の位置で開きます。objects と render 文書のボタンは unit test だけです。
  - `material.document_shared@1` は `<path> holds a and b: only a document of one material is deleted, so a stays (…)` です。
  - Rename… は今の名前が入った欄(`pelican.materials.rename.name`)で `rename_material` を送り、選択が新しい名前に移ります。Undo は逆の rename です。**Rename は unit test だけ**です(シナリオでは通していません)。
- **拒否の 1 文 3 種**(決定 4。**unit test だけ**): `graph.image_key_unknown@1` / `graph.buffer_key_unknown@1` は `Image tinted.tint has a key histroy the engine does not read; the nearest it reads is history (…)` のように、2 文字以内の綴り違いなら一番近い鍵、無ければ読む鍵の一覧を言います。Assets のドックの Update manifest が照合の最中に断られると `The verification of <store> is running — try again once it is done (assets_verification_running@1).` です。
- **`list_assets` の新しい形**(決定 5): EB-1 で `list_assets` に material の行が増え path が project 相対になっても、読む 2 か所(Inspector の model の欄と、登録済みの model のファイル)は `kind` で絞って相対の path を読むので、直す所はありませんでした(実装者の確認)。
- **手で打つ欄**: Rename… の新しい名前(今の名前が全選択で入る)と、Show the document の窓の生の文字(シナリオでは末尾の `}`)です。
- test(FO-1s の着地の時点。`FO-1s decision 2` の後半は MR-1s が書き換えています): `devstudio_task_editor_digest_test` の [「FO-1s decision 1: a refused node order is one sentence by its reason, and the two rows it put the wrong way round are red until a row moves」](../../test/devstudio_task_editor_digest_test.cpp#L641)、`devstudio_materials_test` の [「FO-1s decision 2: a material document the engine refused at its start is fixed from the Problems row -- …」](../../test/devstudio_materials_test.cpp#L1342) と [「FO-1s decision 3: Delete… and Rename… on a material's row -- in_use is one sentence with a button to each place, a delete is undone with register_material and a rename with the rename back」](../../test/devstudio_materials_test.cpp#L1486)、`devstudio_assets_dock_test` の [「FO-1s decision 4: Update manifest refused while a check runs says so in one sentence, and an image's or a buffer's key the engine does not read names the nearest one it reads」](../../test/devstudio_assets_dock_test.cpp#L701)。GPU シナリオは広げた `gui_graph_order` と新しい `gui_material_repair`(§13.5「Studio の自動化面」)です。実装者の run で `studio_cpu` 740 / 740 が緑でした。

**壊れた material を直して Save で戻す(✅MR-1s、`5c26d492`)**: engine の ✅EB-2(`e1ba9058`)の Studio 側です。利用者(2026-10-04)の「壊れた material を直すたびに engine の再起動が要るのは少し面倒」に答えたもので、利用者の操作は「直して Save」だけになりました。下では、確かめ方を「シナリオで通した」「unit test だけ」「未確認」に分けて書きます。

- **起動時に断られた文書**(決定 1): Show the document の窓の見出しは `<path> as it is on disk. Fix it and Save: the file is replaced whole and the engine takes it in.` です([見出し](../../src/devstudio/view/materialsdock.cpp#L876))。Save はファイルを丸ごと書いた後(FO-1s のまま)に `register_material {path}` を送り、窓は開いたまま答えを待ちます(notice `Saved <path>; the engine takes it in.`。[`MaterialsDockSession::documentSaved()`](../../src/devstudio/model/materialsmodel.cpp#L990))。
  - `recovered: true` なら窓が閉じ、ドックの notice が `<name> is loaded again.`(複数なら `a, b are loaded again.`。[`materialLoadedAgainLine()`](../../src/devstudio/model/materialsmodel.cpp#L282))になり、`list_materials` と Problems を読み直します([recovered を読む所](../../src/devstudio/model/materialsmodel.cpp#L1272))。再起動の案内は消えます。**シナリオで通した**(`gui_material_repair`。engine の process id が Save の前後で同じ)。
  - まだ読めなければ窓は開いたままで、parser の文を行・列の 1 文にします。例 `materials/paint.material.json would not load at line 3, column 5: syntax error while parsing value - invalid literal (material.document_invalid@1).`([`materialRegisterRefusalLine()`](../../src/devstudio/model/materialsmodel.cpp#L254))。**unit test だけ**です。
  - `material.path_conflict@1`(一覧に載っているのに loaded でない文書)は、`hot_reload` の watch が先に取り込んだ場合があるので、`list_materials` をもう一度読んで決めます。loaded なら「loaded again」、まだなら `The engine still holds <path> as it read it before; a restart reads the file again (material.path_conflict@1).` と Restart engine です([もう一度読んで決める所](../../src/devstudio/model/materialsmodel.cpp#L1126))。**unit test だけ**で、watch と Studio の `register_material` が競う実物は**未確認**です。
- **Register again**(決定 2): invalid の行に残しました(外のエディタで直した場合)。送るものと答えの扱いは Save と同じです。**ボタンそのものは unit test だけ**です。
  Problems の material の行(`document_kind` が `material` か code が `material.`)の id と文をつないだ signature が変わると、ドックは `list_materials` を読み直します。ドックの 1 秒の poll は見えている間だけなので、隠れていても読むための道です([`materialProblemsOf()`](../../src/devstudio/model/problemsmodel.cpp#L332)、[`observeProblems()`](../../src/devstudio/model/materialsmodel.cpp#L1035))。
- **起動後に壊れた文書**(決定 3): EB-2 の source `hot_reload` の `material.document_invalid@1` / `material.document_unknown@1` を Problems の行から拾い、ドックのその文書の行の 2 列目に赤で `the file on disk does not load; the engine keeps the last values`(tooltip に行・列と理由。[行の印](../../src/devstudio/view/materialsdock.cpp#L1031))を出します。直す欄の見出しは `<path> — the engine keeps the last values` で、Register again は出ません。
  - **Save は何も送りません**。登録済みの文書に `register_material` を送ると engine は `material.path_conflict@1` で、同じ値の `edit_material` は `changed: false` で読み直さず、合う RPC がありません。この問題は watch のある project でしか出ない(watch が無ければ engine は壊れたことに気づかない)ので、watch の取り込みを待ちます。その文書の hot_reload の問題が一覧から消えたら「loaded again」で窓を閉じ、文が変われば(別の行で断られた)その 1 文を窓に出します。**シナリオで通した**(Save から窓が閉じるまで 2.2 秒、同じ process id、箱は壊れている間も赤のまま。実装者の報告)。
  - `document_unknown`(ファイルが消えた)の印は **unit test だけ**です。
- **engine の process id**: シナリオが「再起動していない」を言うために、自動化面の `wait_engine` の答えに `process_id` を足しました(下の「Studio の自動化面」)。
- **engine に回したもの**(実装者の報告): (1) `list_materials` は登録済みの文書もファイルを毎回読むので、起動後に壊れると `invalid`・`materials: []`(EB-2 の報告の「loaded のまま」とずれる。Studio は問題の一覧の印を優先するのでどちらでも動く)。(2) 登録済みの文書を直したファイルから取り込む口が無い(watch の無い project で外で壊して直すと再起動しかない)。(3)(小)watch の失敗の問題に、断ったときのファイルの digest(同じ文の再拒否で窓が待ち続けないように)。
- **手で打つ欄**: 足していません(Show the document の生の文字は FO-1s のまま)。
- test: `devstudio_materials_test` の [「MR-1s decision 1: Save in Show the document takes a document refused at start in with register_material -- …」](../../test/devstudio_materials_test.cpp#L1720)・[「MR-1s decision 2: Register again takes a document fixed in another editor in, the engine's watch taking it first reads as loaded, …」](../../test/devstudio_materials_test.cpp#L1793)・[「MR-1s decision 3: a registered document the engine's watch refused after start is marked, …」](../../test/devstudio_materials_test.cpp#L1854)。`FO-1s decision 2` の後半は、Save で `register_material` が出る → path_conflict → 一覧がまだ invalid → 再起動の 1 文 → 窓は開いたまま、に書き換えました。実装者の run で `studio_cpu` 749 / 749 が緑でした。GPU シナリオは書き直した `gui_material_repair` です。

**pass が宣言していない material の行(✅RTL-3s、`c80bc6a1`)**: engine の ✅FO-2(`31c390c7`)で、文書は読めるのにそれを描く pass が material の使うもの(今は ray query)を宣言していないとき、`list_materials` の `status` は `unusable` で、`pass` と `requirement` が付きます([第6章](06_rendering.md) §6.7 の「pass の宣言漏れは別の code(✅FO-2)」)。FO-2 の着地の時点のドックは `unusable` の行を選べず、`unusable: <理由>` を 1 行出すだけでした(FO-2 の実装者のコードの読み)。

- RTL-3s から、その行は選べて、直す欄が `<path>: Material <名前> queries the scene's acceleration structure, and pass <pass>, which draws it, does not declare ray_query: turn Ray query on for the pass.` の 1 文を言います([1 文を作る `materialPassContractSentence()`](../../src/devstudio/model/materialsmodel.cpp#L138))。
- Show the document と Register again の代わりに **Open the pass**(`pelican.materials.openPass`)が出て、押すと Graph タブでその pass を選びます([Open the pass](../../src/devstudio/view/materialsdock.cpp#L839)、[`openPlanPassOnGraph()`](../../src/devstudio/view/mainwindow.cpp#L1708))。Problems の同じ行も同じ所へ行きます(下の Problems のドックの RTL-3s の項)。
- pass の Ray query を On にすると、engine は次の frame で取り込み直し、問題の一覧の変化でドックが `list_materials` を読み直して `loaded` になります(MR-1s の読み直しの道)。シナリオ `gui_material_ray_query_from_blank` で通しました(§13.5「Studio の自動化面」)。

## 15.5 Assets

### Studio の Assets のドック —— 外の保存を反映する素材と、素材の照合(✅HW-2s / ✅AV-1s)

✅HW-2s / AV-1s(`78737fdf`)から、Studio に **Assets のドック**(`pelican.assetsDock`。中身は
[`AssetsDock`](../../src/devstudio/view/assetsdock.hpp#L42)、RPC のやり取りは Qt を使わない
[`AssetsDockSession`](../../src/devstudio/model/assetsdockmodel.hpp#L144))があります。project の素材のフォルダを木で見せ、
行ごとに「外のツールで保存したら engine が読み直す」(project.json の `hot_reload`。engine の ✅HW-1)を切り替え、素材の中身を
manifest と照合する(`asset_stores` の manifest。engine の ✅AV-1)場所です。どちらの宣言も engine の ✅PJ-1 の
`edit_project_settings` で動いている engine に書かせるので、engine を起こし直さずに効きます(§13.5 の「project.json の宣言を
書き換える(✅PJ-1)」)。Blank / Standard の雛形はどちらの鍵も宣言しませんが([第3章](03_project_format.md) §3.4 の
「起動中の engine に宣言を足す」)、**Blank から作った project でも、このドックの操作だけで監視と照合を始められます**。
押さなければ project.json には何も書きません。

- **開き方**: 既定の配置では Project のドックの隣のタブです([既定の配置](../../src/devstudio/view/mainwindow.cpp#L3961))。
  **View > Assets**(`pelican.view.assets`)で前に出します([View > Assets](../../src/devstudio/view/mainwindow.cpp#L1893))。
  View > Panels でも開閉できます。木の列は Asset / Reload / Contents です。根は `assets/` と、`asset_stores` の mount のうち
  `assets/` の外にあるもので(場所は `get_status.stores`、無ければ project root からの mount。[`assetRoots()`](../../src/devstudio/model/assetsdockmodel.cpp#L165))、
  `.gitkeep` は出さず、フォルダが先・名前の順です。木は `QFileSystemWatcher`(フォルダの全部と、監視しているファイル 512 個まで)が
  変化を言ってから 250 ms 後に全体を読み直します。変化が無い間に時計で読むことはありません([読み直す所](../../src/devstudio/view/assetsdock.cpp#L369))。
- **行ごとの「Reload when saved outside」**: Reload の列の checkbox か、右クリックの **Reload when saved outside**
  (`pelican.assets.reloadWhenSavedOutside`)で、ファイルにもフォルダにも付けられます。押すと `get_project_settings` で今の
  digest と値を読み、`hot_reload` の配列を書き換えた `edit_project_settings` を 1 本送ります([送る所](../../src/devstudio/model/assetsdockmodel.cpp#L491))。
  フォルダを入れると、その下で個別に入っていたものは配列から外れます。最後の 1 つを外すと `hot_reload` は空の配列ではなく
  `null`(鍵ごと消す)になり、雛形の状態に戻ります。印は `get_status.reload.watch` から読みます。親のフォルダで効いている行は
  灰色の中間の印と `via textures` の形で言い(当たり判定は成分ごとの前方一致。[`assetWatchState()`](../../src/devstudio/model/assetsdockmodel.cpp#L133))、
  その行を外そうとすると `<path> is watched because <folder> is; untick <folder> to stop.` と言って何も送りません
  ([切り替えの入口](../../src/devstudio/view/assetsdock.cpp#L617))。engine が動いていないときも送らずに
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
  付きません([印を付ける所](../../src/devstudio/view/assetsdock.cpp#L489))。
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
  [1 文の表 `ProjectSettingsRefusalTexts`](../../src/devstudio/model/inspectormodel.cpp#L1961))。`external_modification` の後は project.json の値を読み直します。
- **`restart_required`**: 答えの `restart_required` が `null` でなければ、notice に `Restart the engine to apply: <理由>` と
  **Restart engine**(`pelican.assets.restartEngine`。Frame Plan と同じ `MainWindow::requestEngineRestart()`)が出ます。
- **shader の読み直しとの重なり**: 監視に shader を入れると、engine の watcher と Studio の `reload_shaders` が両方動きます。
  Studio の側の扱いは §13.5 の「shader を外のエディタで保存すると画面が変わる」の「何も変わっていない」です(✅HW-2s の決定 7)。

**残した穴**(着地時の記録):

- **起動時からある store は Undo で外せません。**engine を起こし直すと、Track で足した store も「起動時からある store」になり、
  その Undo は PJ-1 に断られて `<store> was there when the engine started, so it stays declared — only stores added since then can be undone (project.settings_change_unsupported@1).`
  と出ます([Undo のときの 1 文](../../src/devstudio/model/inspectormodel.cpp#L1979))。外すには project.json を手で直して engine を
  起こし直します。断られた Undo はドックの Undo に残ります(コードの読み)。
- **Restart engine のボタンは通っていません。**player は今 `restart_required` に `null` しか返さないので、test でも scenario でも
  この道を通していません(未確認)。
- **親のフォルダで効いている行は、その行だけ外せません。**engine の `hot_reload` に除外の書き方がないためで、行は上の 1 文で
  「親を外して」と言います。
- **監視を入れる前に保存した変更は読み直されません**(engine の穴。PJ-1 の「作り直した watcher は…」)。この engine の実行中に
  監視を入れたときだけ、監視の 1 行の tooltip に `Changes saved before now are not reloaded.` が足されます
  ([tooltip を作る所](../../src/devstudio/view/assetsdock.cpp#L674))。
- manifest を書いている間と照合の終わりを待つ間は frame が止まります(engine の穴。PJ-1)。
- 木は書き換えのたびに全体を走査し、Studio が見張るファイルは 512 個までです。大きい `assets/` での重さは測っていません(未確認)。

- test: `devstudio_assets_dock_test`(`QT_OFFSCREEN`。test が engine の代わりに答えます)の
  [「HW-2s decision 1: the Assets dock is the project's asset folders, without .gitkeep, and follows the disk」](../../test/devstudio_assets_dock_test.cpp#L168)・
  [「HW-2s decision 2: a row's switch is one edit of hot_reload, a folder's covers what is inside it, and Undo sends what was there before」](../../test/devstudio_assets_dock_test.cpp#L217)・
  [「HW-2s decision 3: the line above the tree says what is watched and what was read again last; a render document's row says the watch does not apply it」](../../test/devstudio_assets_dock_test.cpp#L299)・
  [「HW-2s decision 4: Track asset contents declares main at assets/ with a manifest in one edit, is offered only while no store declares one, and its Undo can be refused」](../../test/devstudio_assets_dock_test.cpp#L346)・
  [「AV-1s decision 5: what the check found is a line under the tree and a mark on each file's row; not_run shows nothing」](../../test/devstudio_assets_dock_test.cpp#L421)・
  [「AV-1s decision 6: Verify again and Update manifest wait while a check runs, and Update manifest says how many files change before it writes」](../../test/devstudio_assets_dock_test.cpp#L486)・
  [「HW-2s decision 7: with the engine's own watch open, a reload_shaders that found nothing new is followed as the watcher's commit」](../../test/devstudio_assets_dock_test.cpp#L563)。
  `devstudio_layoutpreset_test` はドックの数を 7 から 8 にし、古い保存済みの配置を戻しても Assets のドックが置き場を持つことを見ます。
- シナリオ: `gui_assets_watch_verify`(Blank から GUI だけで。§13.5「Studio の自動化面」)。

## 15.6 Graph タブ

### Studio のノードキャンバス(Graph タブ)

**置き場所**: `View > Panels > Frame Plan` の dock(`pelican.framePlanDock`)にある 4 枚目のタブ「Graph」です
([タブの追加](../../src/devstudio/view/mainwindow.cpp#L637))。dock は増えていません。✅RSX から、タブは Frame Plan / Features / Rendering settings / Graph の順です(Rendering settings は下の「Studio の Rendering settings タブ」。RSX の前は Graph が 3 枚目でした)。起動時から開いておきたい場合は
`pelican_studio --project <dir> --tab graph`([第10章](10_tools.md) §10.7)。

**上段の操作列**(左から): `Scope:` のコンボ(`root` と definition 名)、その右のパンくず、領域の `Show:`(✅GL-1。下の「graph の外から来た pass を畳んだ node で見る」)、右端に
`Values from:` のコンボ、`Refresh`([構築](../../src/devstudio/view/graphcanvaswidget.cpp#L5360))。
✅UX-1 から、Refresh の左(Undo / Redo の後ろ)に **Auto layout** と **Fit view** のボタン(`pelican.graphCanvas.autoLayoutButton` / `pelican.graphCanvas.fitViewButton`)があります。右クリックのメニューと同じ action を発火します([ボタンを置く所](../../src/devstudio/view/graphcanvaswidget.cpp#L5731))。UX-1 の前は右クリックのメニューにしかありませんでした(利用者の指摘「graph の自動レイアウトってどこ?」)。
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
   の順([解決順](../../src/devstudio/view/nodegraphics.hpp#L171)、入口は [`socketStyleFor()`](../../src/devstudio/view/nodegraphics.hpp#L173))。
   既定表の中はさらに 2 段で、**型名の完全一致 → `prefixes` の前方一致**です([nodegraphics.cpp:126](../../src/devstudio/view/nodegraphics.cpp#L135)) ——
   だから `pelican.render.` のような名前空間まるごとに 1 行で形を与えられます。既定表は
   [`graph_socket_styles.json`](../../src/devstudio/resources/graph_socket_styles.json#L1)(`pelican.studio.socket_styles` v1)で、
   **新しい型に形を与えるのに C++ を触る必要はありません** —— この JSON に名前を 1 行足すだけです。
5. **右の詳細表** —— `label` / `type` / `domain` / `evaluation` / `id` / `sink` と、あれば `definition` /
   `parameters` / `inputs` / `outputs`([組み立て](../../src/devstudio/view/graphcanvaswidget.cpp#L9636))。
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
同じ型のときだけ**通します([判定](../../src/devstudio/model/graphcanvasmodel.cpp#L2855))。

**線を引いている間、落とせるソケットが光り、近ければ吸い付きます(✅UX-1)**: Blender の Node Wrangler の lazy connect の感覚です(利用者の指摘)。

- 線を引き始めると、canvas の上のソケット(node のソケット、ゾーンの縁の口、上の「Graph から画面に出す」の pass の入力の口)ごとに、そこで離したら何が起きるかを判定します([`GraphCanvasScene::lightDropTargets()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2873))。落とせるソケットは 1.35 倍に、落とせないソケットは不透明度 0.3 に描きます。空いた口の `+` は、その口の判定に従います。
- 判定は、離したときに使う関数と同じ 1 つです([`GraphCanvasScene::dropAction()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2936))。型の判定は model の `canConnect()` と同じ中身を、描いた node の一覧に対して問います([`GraphCanvasModel::canConnectAmong()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2867))。第二の判定はありません。
- 吸着: カーソルから画面の上で 32 px 以内にある落とせるソケットのうち、一番近いものに線の先が寄り、そのソケットは 1.7 倍になります。半径は画面の px で、ズームに依りません(`GraphCanvasScene::GraphCanvasSnapRadius`。[寄せる所](../../src/devstudio/view/graphcanvaswidget.cpp#L2898))。
- 離すと、吸い付いていたソケットへ落とします。吸い付いていなければ、今までどおりカーソルの下のもので決めます([`GraphCanvasScene::finishConnection()`](../../src/devstudio/view/graphcanvaswidget.cpp#L3084))。
- test: `devstudio_graphcanvas_scene_test` の [「UX-1 a line being drawn lights the sockets it can land on and snaps to the nearest one in reach」](../../test/devstudio_graphcanvas_scene_test.cpp#L7930)。

⚠ **2 つの注意。**
(1) **必須の scalar 入力を持つ op を足すと、種の定数も一緒に足されます。**未接続の必須入力を持つ文書は engine が
受け取らないので、パレットは必須の scalar 入力ごとに `value.constant` を**同じ ops 列で先に**足し、そこへ繋いだ
状態のノードを作ります([生成](../../src/devstudio/model/graphcanvasmodel.cpp#L3060))。1 操作 1 チケットは保たれます。
後で本物の producer を繋いでも、**この定数は自動では消えません**(手で消します)。
例外は `audio.playback` の `time` で、定数ではなくシーンの時計に繋ぎます(✅Q-11。下の「`audio.playback` の欄」)。
(2) **必須入力の切断は送られ、engine が断ります。**Studio は黙って定数を挿し直したりしません ——
`set_input{null}` をそのまま送り、`unconnected graph input` の拒否を notice に出します。値を持つ instance の port は
(戻り先の `parameters` が残っているので)切断できます。

**palette はキーボードだけで使えます(✅GL-1d)**: 右クリックの **Add node...** と **Add** のボタンは、同じ関数で palette(`pelican.graphCanvasPalette`)を開きます([`openPalette()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8951))。

- 開いたとき、キーボードは絞り込みの欄(`pelican.graphCanvasPaletteFilter`)にあります。click せずにそのまま打てます。
- ↑ ↓ と PageUp / PageDown は一覧(`pelican.graphCanvasPaletteList`)の選択を動かします。キーボードは絞り込みの欄に残ります([キーを一覧へ回す所](../../src/devstudio/view/graphcanvaswidget.cpp#L4992))。
- Enter は OK(選んだ行を足す)、Esc は Cancel(何も送らない)です。
- **Add** のボタンは、右クリックのメニューと同じく event loop に回してから palette を開きます([ボタンの所](../../src/devstudio/view/graphcanvaswidget.cpp#L6434))。palette は modal のダイアログなので、GL-1d の前はボタンの click の中で開いていました。今は click が返ってから開くので、自動化(シナリオ)もボタンで palette を開けます。✅UX-9 から、palette は `exec()` ではなく window-modal の `open()` で開き、閉じた後に選んだものを event loop から足します。保存の最中でも開きます(下の「保存中でも palette が開く」)。
- test: `devstudio_graphcanvas_scene_test` の [「GL-1d the Add node palette opens with the keyboard on its filter」](../../test/devstudio_graphcanvas_scene_test.cpp#L3560)(開いた直後の focus が絞り込みの欄、click なしで打った字が欄に入る、↓ で一覧の行が 0 から 1 に動く、Esc で何も送らずに閉じる、Enter で ↓ の行が足される)。

**`pelican.ui` の sink の node を選ぶ(✅Q-8)**: palette の `ui.value` / `ui.visible`(GP-3 の `pelican.ui` module の 2 つの sink)は、
書き込む UI node の path を parameter `node` に持ちます。

- **置くと `node` が入っています。**palette で置くと、project の UI 文書の最初の path が `parameters.node` に入ります
  ([置くときに node を入れる所](../../src/devstudio/model/graphcanvasmodel.cpp#L3588))。`ui.value` は最初の gauge(engine が値を書けるのは
  gauge だけで、他の widget は `ui.property_mismatch@1` で断られます)、`ui.visible` は文書の最初の node(root)です。UI 文書が
  読めなければ `node` を書かずに送り、engine の名前付きの拒否になります。Q-8 より前は `node` が空のまま出て、置くと必ず
  `ui sink requires a node path` で断られていました。
- **詳細表の上に「UI node」の欄が出ます。**選んだ node が `ui.value` / `ui.visible` のときだけ、打つこともできる一覧
  (`pelican.graphCanvasUiNode`)が出ます([UI node の欄を合わせる所](../../src/devstudio/view/graphcanvaswidget.cpp#L8330))。項目は UI 文書の
  node の path(root からの id を `/` で繋いだもの)で、`ui.value` では gauge だけです
  ([一覧を gauge に絞る所](../../src/devstudio/model/graphcanvasmodel.cpp#L573))。一覧から選ぶか、打って Enter で `set_node_parameters`
  1 回です。一覧に無い path はそのまま送られ、engine が名前付きで断ります。
- **一覧の出所は project の UI 文書です。**UI の木を返す RPC は無いので、Studio が `project.json` の
  `basic_config.ui_config_json` が指す文書を engine と同じ project path resolver で読みます
  ([UI 文書を読む所](../../src/devstudio/model/graphcanvasmodel.cpp#L478))。欄を出す直前と palette を開く直前に読み直すので、開いた後に
  文書を書き換えても一覧はそれに従います。読めないときは欄の placeholder が理由(`project.json names no UI document
  (basic_config.ui_config_json)` など)を言います。

**`audio.playback` の欄(✅Q-10)**: AU-1 で `audio.playback` に入力と parameter が増えました。

- **増えた port は engine の catalog から来ます。**`position`・`source_begin`・`source_end`・`speed`・`delay` と `events` は Studio が
  他の op と同じく catalog から描き、必須は `time` だけです([engine の宣言](../../src/project/graphmodule.cpp#L817))。`events` は任意の
  入力なので、繋いでいなくても Studio は問題として扱いません。任意の数値の port に値を入れるのは、今は繋ぐことでだけです(既存の挙動)。
- **port でない 3 つの parameter は、詳細表の上の欄です**(`pelican.graphCanvasAudioRow`。選んだ node が `audio.playback` のときだけ。
  [欄を合わせる所](../../src/devstudio/view/graphcanvaswidget.cpp#L8811)):
  - **Sound**(`pelican.graphCanvasAudioAsset`、打つこともできる一覧)= parameter `asset`。一覧は project の中の `.wav` / `.ogg`
    (大文字小文字は問わない。`.` で始まる folder は見ない)を `project://...` で並べたものです([音声を探す所](../../src/devstudio/model/graphcanvasmodel.cpp#L511))。
    音声の一覧を返す RPC は無いので Studio が folder を歩きます。project を開いたとき、選ぶ node が変わったとき、palette を開く直前に探し直します。見つからなければ
    placeholder が理由(`no .wav or .ogg file in the project` など)を言います。打った path はそのまま送り、中身は engine が確かめます。
  - **Bus**(`pelican.graphCanvasAudioBus`、選ぶだけ)= `se` / `bgm`。engine が受け取るのはこの 2 つだけで、それを一覧にする RPC が無いので、
    **一覧は Studio が持っています**([bus の一覧](../../src/devstudio/model/graphcanvasmodel.cpp#L558))。文書に別の値があれば、それも一覧に出します(engine が名前付きで断ります)。
  - **Wrap**(`pelican.graphCanvasAudioWrap`、選ぶだけ)= `once` / `repeat` / `ping_pong`。`position` を繋いだ node(分解版)は wrap を
    持てない(持つと `audio.playback_mapping_conflict@1`)ので、そのときは欄が無効です。
  - 選ぶか Enter で `set_node_parameters` 1 回です。既に効いている値(Bus / Wrap は、鍵が無ければ一覧の先頭の既定値)を選んでも何も送りません
    ([送る所](../../src/devstudio/view/graphcanvaswidget.cpp#L8859))。
- **palette で置くと、project の最初の音声が `asset` に入ります**([置くときに asset を入れる所](../../src/devstudio/model/graphcanvasmodel.cpp#L3603))。
  任意の `events` / `position` は空のままです。
- **palette で置くと、必須の `time` はシーンの時計に繋がります(✅Q-11)。**上の (1) の種の定数(0)ではなく、今の scope に
  `value.input{parameters:{name:"scene_time"}}` があればそれを使い回し、無ければ 1 つ足して(id は `scene_time`。その id が今の scope で使われていれば `scene_time_2` …)、
  その `value` を `time` に繋ぎます([シーンの時計を繋ぐ所](../../src/devstudio/model/graphcanvasmodel.cpp#L3553) /
  [`GraphCanvasModel::sceneTimeNode()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2314))。時計の追加と node の配置は**1 回の編集**で送るので、
  **Undo 1 回で両方が戻ります**。AU-3 から engine は `events` も `position` も繋がっていない `audio.playback` を読み込み、owner の最初の
  frame から鳴らすので、**置いただけで読み込まれ、鳴ります**。
  - 使い回すのは**今の scope** の `scene_time` だけです。definition の中で置くと、root に `scene_time` があってもその definition に新しく 1 つ足します。
    名前が `scene_time` でない `value.input`(たとえば `time`)は使いません。
  - 足した `scene_time` の node は、✅UX-8 から主の node の左の、描かれた箱と重ならない所に置かれます(セッションの位置。下の「新しい node の置き場所」)。UX-8 の前は配置を持たず、canvas の初期配置で描かれていました。
- 音声の format の拒否(`audio.format_unsupported@1`)は、この tab の notice に Q-6 の表の文で出ます(§13.1「Studioでcomponentを追加する」)。
- ✅UX-8 から、Sound の欄の右に **Import…**(`pelican.graphCanvasAudioImport`)があり、`.wav` / `.ogg` を project の `assets/audio/` に写して、選んでいる `audio.playback` の `asset` に入れます。File ▸ Import Sound… と drag & drop も同じです(下の「音の node を置く・音声を Import する」)。✅UX-9 から、node の下の帯に voice の状態(`playing · 1.2 s`)が出ます(下の「保存中でも palette が開く」の voice の帯)。

**ループを置く —— graph.loop(✅LP-4c)**: Graph タブは共通グラフのループ `graph.loop`(書き方は [第6章 §6.18](06_rendering.md)、設計は [design_graph_loops](../design_graph_loops.md) §9)を、parameters の JSON ではなく専用の欄と表で組みます。
ループの RPC(definition の op、`loops[]` と `loop_origin`、`set_input` の `element`、`decompose_material_pass`、`get_graph_templates` / `apply_graph_template`)は、上の [「ループと分解の RPC —— engine の契約」](13_editor.md#ループと分解の-rpc--engine-の契約lp-0alp-vb--rd-1--rd-1b) にあります。
LP-4c の着地時はゾーン表示が無く、body は instance と同じく「中に入って」編集していました。✅LP-4d1 から、root の canvas はループを枠(ゾーン)で描き、枠の中の body をその場で編集します(下の「ループをゾーンで見る・編集する」)。「中に入る」道(Open body とダブルクリック)も残っています(下の「body に入る」)。

- **Add loop**(root だけ): キャンバスの右クリックの **Add loop**(`pelican.graphCanvas.addLoop`)は、空の definition を 1 つ作り、それを body にするループを root に置きます。
  - definition の名前は Studio が提案します(`loop_body`、使われていれば `loop_body_2` …)。打つものはありません。
  - `add_definition` と `add_node` を 1 つの列で送るので、Undo 1 回で両方が戻ります([`GraphCanvasModel::addLoopOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3846))。
  - 置いたループは `over: {range: {count: 1}}`・`branches: [{body: <definition>}]` です。
- **palette から**: palette(**Add node...**)の `graph.loop` は root にだけ出ます。選ぶと body の combo が出ます。
  - 先頭の **New definition…** では、名前の欄(提案で埋まっている。`pelican.graphCanvasPaletteNewDefinition`)で名前を変えられます。
  - 既にある definition を選ぶと、それが body になり、その出力が全部 `p`(最後の回の値)として宣言されます。
- **Add fullscreen pass**(`pelican.graphCanvas.addFullscreenPass`。右クリック。catalog に `render.fullscreen` があれば root でも definition の中でも出ます): `engine://passthrough` で描く fullscreen pass と、それが書く自前の `render.image`(id は `<pass>__color`)を 1 回で足します。空の body に最初の pass を置く手段です。
  - ⚠ LP-4c の着地時は、pass に `pass_set: "main_render"` を書き、置き場所の無いループの body に最初の pass を置くとループに `canonical_anchor: "post_main"` も足していました。RU-1(`fa413aeb`)から、どちらも書きません(engine が LP-1e で、置き場所の無い graph の pass とループの既定の位置を持ったため)。
  - image は `R16G16B16A16_SFLOAT`・画面の大きさ・usage `COLOR_ATTACHMENT` / `SAMPLED` です(✅UX-6a から。それまでは `R8G8B8A8_UNORM` で、usage に `TRANSFER_SRC` もありました。下の「Studio が作る image は float で、TRANSFER_SRC を付けません(✅UX-6a)」)。✅UX-3 から、palette の `render.raster` も同じ所で組み、同じ形に `draw: {vertex_count: 3}` を足して来ます(下の「palette で置ける render node」)。
  - ✅LP-4f2 から、**view の軸で回すループの body** に置くときは、image の大きさをそのループの family に合わせます([fullscreen / raster の pass と image を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L3329)、[大きさを引く所](../../src/devstudio/model/graphcanvasmodel.cpp#L4027))。engine は 1 つの view family の raster pass が違う大きさに書くことを断る(`render.view_family_extent_mismatch@1`。下の「view family の大きさの拒否」)ので、画面の大きさの image を cube の 256×256 の family に混ぜないためです。
    - 基準は、engine が `get_frame_plan` の二次 family の node に載せる `family_extent` / `family_layers` です(✅LP-4f3。engine の ✅VF-2 で、VF-1 の拒否と同じ helper が決める基準)。LP-4f2 では Studio が `get_render_authoring_context` の `render_targets[]` を自分で辿っていましたが、LP-4f3 で外しました。family が `family_extent` を持たなければ、今までどおり画面の大きさで作り、engine の拒否の文に任せます。
    - 固定の大きさなら image に `width` / `height` を書き、`layers` が 2 以上ならそれも書きます(cube なら 256×256・6 layers)。画面相対なら `extent_scale` をその値にします。
    - pass には `view_family`(ループの family)を書き、固定の大きさなら `resolution_domain: "independent"` も書きます。
    - 基準が読めない(plan か target の宣言が無い)ときは今までどおり画面の大きさで作り、`view_family` だけを書きます。大きさが合わなければ engine が名前付きで断ります。

**ループの node(✅LP-4c)**: ✅LP-4d1 からの root の canvas では、ループはふつう下のゾーンで描かれ、この node の形になるのは、三角でゾーンを畳んだとき、同じ body を先のループがゾーンに描いているとき、body を持つ分岐が無いときです。見出しの右の型名の所に `↻ loop` が出ます。engine の frame plan の `loops[]` が届くと、`↻ expand ×3` のように段と回数に変わります([見出しの字を決める所](../../src/devstudio/model/graphcanvasmodel.cpp#L1980))。

- 入力ソケットは、body を持つ最初の分岐の body の `graph.input` です(型は付けません)。
- 出力ソケットはループが宣言した `p` / `p[]` で、`p[]` は `<名前>[] (each)` と出ます(回ごとの値の束)。
  - ⚠ LP-4c の着地時は、`p[]` の行の port と字が入れ替わっていて(port が `<名前>[] (each)`)、そこからの線がソケットに届いていませんでした。✅LP-4d1 から、字が `<名前>[] (each)`、port が `<名前>[]` です([行を組む所](../../src/devstudio/view/graphcanvasdrawing.cpp#L158))。
- parameters(`over`・`branches` …)はソケットにも値の帯にも出ず、詳細表にも `parameters` の行を出しません。詳細表の `body` の行は、入るときの body です。
- Graph タブは、文書にループがあるときだけ、文書を読むたびに `get_frame_plan` を 1 回読んで `loops[]` を使います。✅LP-4f1 から、ループが無くても root に view_select を取る render node があれば読み、同じ plan から node の `view_axes`(view family ごとの軸)と render target の名前も集めます(下の「view の軸で回す」。[読みを決める所](../../src/devstudio/model/graphcanvasmodel.cpp#L832))。✅Q-19 から、root の instance の definition に render node があるときも読み、同じ plan から pass の `pass_inputs` も集めます(下の「render node の入力は shader の宣言から」)。✅GL-1 から条件は無く、文書を読むたびに読みます(下の「graph の外から来た pass を畳んだ node で見る」)。

**ループをゾーンで見る・編集する(✅LP-4d1)**: root の canvas は、root の各 `graph.loop` を枠(ゾーン)で描き、枠の中に今の分岐の body の node を並べます(Blender のループゾーンの見え方。保存の形は変わらず、body は definition 名のままです)。枠の中の編集は body の definition へ、枠の移動は root へ書きます([ゾーンを計画する所](../../src/devstudio/view/graphcanvaswidget.cpp#L1695)、[`Builder::addZone()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L856))。

- **見出しの帯**: 左から三角、ループの label(無ければ id)、右端に段の字です。
  - 段の字は engine の `loops[]` の報告から作ります: `expand ×2` / `draw_loop N materials`。報告の前は `loop` です。
  - ✅LP-4f1: view の軸で回すループの段の字は `×6 (face)`(回数と軸)です。報告の前は frame plan の軸の長さを使い、それも無ければ `loop (face)` です。分岐が 1 つで長さが 1〜32 なら、タブの位置に軸のラベル(`+X` `-X` … 。ラベルの無い軸は番号)が小さく並びます(読むだけ。tooltip `Element <i> runs for this view`。[段の字を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L1803)、[ラベルを描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1007))。
  - body の definition を他の instance やループも使っていると、段の字の左に橙色で `shared (N users)` が出ます(tooltip は `Other instances or loops run <definition> too: an edit here changes them all.`)。N は使っている node の数で、✅Q-17 から、1 つのループは分岐がいくつ同じ body を使っていても 1 と数えます([`GraphCanvasModel::definitionUseCount()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1719))。Q-17 の前は、同じループの 2 つの分岐が同じ body を使うだけで `shared (2 users)` と出ていました。
- **分岐のタブ**: 分岐が 2 つ以上のときだけ、見出しの下に `if <条件>` / `elif <条件>` / `else` のタブが並びます。else だけのループにはタブがありません。
  - 条件の字は `element = 0` / `element in 1, 4` / `element not in …` です。draw_loop の分岐は material・`tags …`・contract・`skinned`・`has variant` などを ` & ` でつなぎ、式の条件は `expression` です([`zoneCondition()`](../../src/devstudio/view/graphcanvaswidget.cpp#L1310))。
  - タブを押すと、その分岐の body に描き替わります([`GraphCanvasModel::showLoopBranch()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1695))。どのタブかはセッションの状態で、文書には書きません。Open body で入る body も、タブが選んだ分岐のものです。
  - `"body": null` の分岐のタブは薄く描かれ、押しても変わりません(tooltip `This branch runs nothing`)。
  - ✅UX-1 から、選んでいるタブはテーマの `tab_selected` の地に `tab_selected_text` の太字で描きます([選んだタブを描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L966))。UX-1 の前は枠の色で塗っていたので、選んだタブが見出しの帯と同じに見えました。token は `studio_themes.json` の `canvas` にあります(dark は `#f2c14e` の地に `#1b2027`、light は `#2f6fb5` の地に `#ffffff`)。
  - ✅UX-1 から、ほかの分岐と同じ body を走らせる分岐のタブには `· shared with else`(相手が else でなければ `· shared with branch <番号>`)が付き、tooltip がここを直すと両方が変わることを言います([判定する所](../../src/devstudio/view/graphcanvaswidget.cpp#L1842))。文書が定義していない body を指す分岐のタブは薄く描かれ、tooltip は `This branch runs <body>, which the document does not define` です。
  - UX-1 の前は、Add branch の分岐が else と同じ body を指したので、どのタブを押しても同じ body が出て、切り替わらないように見えました。今は Add branch が body を写します(下の「分岐表」)。選んだ分岐は、body が同じでもタブが選んだ番号で覚えます([`GraphCanvasModel::loopBranch()`](../../src/devstudio/model/graphcanvasmodel.cpp#L1658))。
- **左右の縁**: 1 行が 1 つの名前です。
  - 左の縁: 外側(枠の線の上)にループの入力のソケット、内側に body の `graph.input` のソケットです。body の `graph.input` は node としては描きません。内側のソケットから body の node の入力へ drag すると、その `graph.input` の `value` からの `set_input` が出ます。
  - 右の縁: 内側に body の出力(definition の `outputs`)の端、外側にループが宣言した `p` / `p[]` のソケットです。出力を作る body の port から内側の端へ破線が引かれます(描くだけです)。
  - ✅LP-4d2 から、編集できる文書では、左右の縁の行の最後に空いた口 `+` が 1 つずつあります。右の縁の内側の端は、carry を引く始点にもなります(下の「縁をまたぐ線・carry・展開の表示」)。
- **枠の大きさ**: body の node の外接に余白を足したもので、最小の幅と高さがあります([`layoutZone()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L741))。body の node を枠の左や上へ動かすと、枠がその側へ広がります。body が空でも枠は描かれます。✅LP-4d2 から、縁の行の数には `+` の口の行も数え、下端には展開の行と carry の矢印の通り道の分の高さが足されます。drag の間も枠が追従します(下)。
- **枠の中の編集**: body の node は、definition 自身の `layout.nodes` の位置(枠の内側の原点から)に描かれます。上の「送る op」の表の操作は、枠の中では全部 `scope: <definition>` で出ます([編集を body へ送る所](../../src/devstudio/view/graphcanvaswidget.cpp#L2307))。
  - node の drag は definition の `set_layout`、線を引く・切るは `set_input`、Delete は `remove_node`、見出しのダブルクリックは `set_node_label`、値の帯は `set_node_parameters` です。
  - 線は同じ scope の中だけで引けます。例外は ✅LP-4d2 の、root の node の出力を枠の中の node の入力へ直接落とす線で、ループの入力を足す 1 回の編集になります(下の「縁をまたぐ線・carry・展開の表示」)。それ以外では、body の node と root の node の間には引けません。
  - 位置の無い body の node(palette で足した直後など)は、枠の中で自動で並びます。
  - `Values from:` の出力の値の帯は、ゾーンの body の node には重なりません([外す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1809))。
  - body の node を選ぶと、右の詳細表と欄(draw step の欄を含む)はその definition の node を見せます。
- **body に足す**: 右クリックのメニューを枠の中で開くと、Add node... と Add fullscreen pass は body に足します。ゾーンを選んでいる(見出しを押した)か、枠の中の node を選んでいるときも、Add fullscreen pass と palette(Add node...)は body に足します([足す先を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L2616))。palette は body の scope の候補を出し、`graph.loop` と `graph.instance` は出しません。Add loop は枠の外で開いたメニューにだけ出ます。
- **ゾーンを選ぶ・動かす**: 枠の中の node の無い所か見出しを押すと、ゾーン(ループ)が選ばれ、Loop の欄が出ます。見出しを drag すると、root の `set_layout`(ループの位置)だけが出ます。body は枠の子として付いてきて、definition の `layout` は書きません。逆に枠の中の node の drag は definition の `set_layout` だけで、1 回の drag で両方が書かれることはありません。
  - **Auto layout** は root だけを並べ直します(ゾーンは 1 つの箱です)。body の配置は変えません([`GraphCanvasScene::autoLayout()`](../../src/devstudio/view/graphcanvaswidget.cpp#L615))。
  - ✅UX-1: 見出しで drag したゾーンが、離した後に元の所へ押し戻されたように見えることがありました(利用者の指摘)。文書には動かした位置が書かれていて、動いていたのは view です。view は viewport より小さい scene rect を中央に置くので、drag の間や描き直しの後に scene rect が変わるたび、スクロールバーが出るたびに、画面の上の全部がその変化の半分だけずれました(コードの注記)。今は scene rect を変えるとき、各 view に今見えている範囲を含めた rect を渡してスクロールをそのままにし([`GraphCanvasScene::applySceneRectNow()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2707))、Graph タブの view はスクロールバーを縦横とも常に出します([`configureGraphCanvasView()`](../../src/devstudio/view/graphcanvaswidget.cpp#L10344))。
  - `81b62382` から、drag(node・reroute・tie)の間は scene rect を広げるだけで、縮めません([`GraphCanvasScene::scheduleSceneRectUpdate()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2696))。それまでは drag の 1 歩ごとに描いたものから rect を計算し直したので、一番左の node(畳んだ feature など)を drag すると rect の端が view の下で動き、canvas 全体が滑りました。この変更に test は足していません(commit の stat)。
  - ✅UX-1: 文書が配置を持たない root で、枠の中の body の node を drag すると、root が描くたびに自動配置をやり直し、ゾーンが元の所へ戻りました。drag の間、root の graph の node は今描かれている場所をセッションに固定します。root の文書には書きません([`GraphCanvasScene::holdRootPlaces()`](../../src/devstudio/view/graphcanvaswidget.cpp#L717))。
  - test: `devstudio_graphcanvas_scene_test` の [「UX-1 a zone dragged by its heading ends where the hand let go, and stays there」](../../test/devstudio_graphcanvas_scene_test.cpp#L7759) と [「UX-1 a body node is dragged inside its zone, and the zone stays where it is」](../../test/devstudio_graphcanvas_scene_test.cpp#L7797)。
- **三角**: 見出しの左の三角(tooltip `Fold the zone into the loop's node`)で、枠を LP-4c のループの node(ソケットつき)に畳みます。畳んだ node の三角(tooltip `Open the loop's zone`)で枠に戻ります。ループには見出しだけに畳む形はありません。どちらの形かはセッションの状態で、文書には書きません。
- **共有**: 同じ definition を 2 つのループが body にしていると、枠に描くのは文書で先のループだけで、もう一方は LP-4c のループの node で描かれます。枠の見出しには `shared (N users)` が出ます。
- **ダブルクリック**: 見出しは改名です。枠の中の node の無い所をダブルクリックすると、今までどおり body の scope に入ります(下の「body に入る」)。body の node のダブルクリックでは入りません([`GraphCanvasScene::mouseDoubleClickEvent()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4581))。
- **Undo**: Graph タブの履歴のままです(下の「Undo」)。枠の中の編集は definition の scope の op、枠の移動は root の `set_layout` で、それぞれ 1 項目として戻ります。scope の違う `set_layout` が履歴に混ざっても、文書は byte 一致で戻ります(scene の test)。

**縁をまたぐ線・carry・展開の表示(✅LP-4d2)**: 枠の縁をまたいで線を引くと、ループの入力と出力が足されます。body の出力から body の入力へ戻る線は carry になり、枠の下側を回る矢印で描かれます。engine の報告が届くと、ループが何になったか(回の数か material の数)が枠の下端に出ます。どの操作も 1 回の編集で、Undo 1 回で戻ります。

- **外から入力を足す**: 編集できる文書では、左の縁の行の最後に空いた口 `+` があります(tooltip `Drop a line from outside here for a new loop input`)。root の node の出力をここへ落とすと、ループの新しい入力を足す列を 1 回で送ります([列を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L4605)、[落とし先を見分ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L3084))。
  - 列の中身は、ループの分岐が走る definition への `add_node graph.input`(同じ definition には 1 回だけ、分岐の順。既定値は付けません)と、root の `set_input`(ループの新しい入力 ← 落とした出力)です。今のタブの body だけでなく、全部の body に足します(分岐の body の入出力がそろっていないと engine が `graph.loop_branch_interface@1` で断るため)。
  - 枠の中の body の node の入力に直接落としても、同じ列を送ります。列の最後に、その definition の `set_input`(その入力 ← 新しい `graph.input` の `value`)が付きます。
  - ✅Q-19 から、落とした先が body の render node の宣言のソケットなら、列の最後は番号付きの port を足す `set_node_parameters` とその `set_input` です(下の「render node の入力は shader の宣言から」)。
  - 入力の名前は、落とした出力の port 名から Studio が提案します。`.` と `/` は `_` になり、末尾の `[]` は外します(空なら `input`)。ループの入力か body の node の id と重なれば `_2`、`_3` … です([`GraphCanvasModel::suggestLoopInputName()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4558))。打つものはありません。
  - 引けるのは、canvas が root を見ているときの、root の node の出力からだけです。ループ自身の出力からは引けません(循環になるため)。
  - ループが既に持つ入力の外側のソケットに落とすと、今までどおり root の `set_input` の差し替えだけです。
- **中から出力を足す**: 右の縁の行の最後にも `+` の口があります(tooltip `Drop a body node's output here for a new loop output`)。枠の中の body の node の出力をここへ落とすと、次の列を 1 回で送ります([列を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L4651))。
  - body ごとに 1 つの `set_outputs`(✅DO-1。上の op の表)。その body の `outputs` に新しい出力を足したものを丸ごと置きます。
  - 最後に、ループの `outputs` に `p` を足した root の `replace_node`。`p[]` はここでは足しません(Loop の欄の Outputs の check で足します)。
  - 出力の名前は port 名から入力と同じ規則で提案します(空なら `output`)。ループが宣言した `p` / `p[]` か、どれかの body の出力と重なれば `_2` … です。
  - 他の分岐の body は、同じ id の node の同じ port からその出力を出します(body を写したときの配線です)。`set_outputs` の endpoint は node と port の両方が要り、つながっていない出力は足せないためです。
  - 他の分岐の body にその node か port が無ければ、何も送らず、Graph タブの notice に 1 行で断ります([`GraphCanvasModel::addZoneOutput()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4696))。例: `Branch 1 of loop steps runs rest, which has no sum.value to give value from: add the output there first.`
- **carry の矢印**: 右の縁の内側の端(body の出力。tooltip `The body's output <出力> (drag to a boundary input to carry it)`)から、左の縁の内側の端(body の `graph.input`)へ線を引くと、ループの `carry` に `{from, to}` を 1 組足します。送るのは Loop の欄の Carry と同じで、parameters を丸ごと置く `set_node_parameters` 1 回です([`GraphCanvasModel::addLoopCarry()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4708))。同じ組が既にあれば何も送りません。
  - 矢印は、出力の端から右へ出て枠の下側の通り道を戻り、入力の端へ左から入ります([矢印を引く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1230))。通り道は組ごとに分かれ(下端の展開の行の上)、その分だけ枠が高くなります。
  - 矢印を押すと選ばれて太くなり、Delete でその組を `carry` から外します(これも `set_node_parameters` 1 回です。[`GraphCanvasModel::removeLoopCarry()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4721))。tooltip は `<from> carries into <to> (select and press Delete to remove)` です。
  - `to` の入力にループの入力の edge が無い組は、矢印が橙色になり、`!` の印が付きます。tooltip は、Loop の欄の Carry の赤い字と同じ `<to> has no loop input: connect one for the first element to start from.` です。
- **回ごとの入力の線**: source に `element` を持つ edge(下の「回ごとの入力 —— Per iteration」で張った入力)は破線で描かれ、線の中ほどに `[<element>]` のラベルが乗ります([線を描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L623))。
  - ラベルの字は、式(`{"$expr": …}`)ならその式、文字列ならそのまま、それ以外は JSON です。tooltip は `Each element reads element <element> of <node>.<port>` です。
  - 線を選んでも破線のままです。ラベルは線に付いて動きます。
- **展開の表示**: engine の `loops[]` の報告が届いたループでは、枠の下端に 1 行が出ます([出す所](../../src/devstudio/view/graphcanvaswidget.cpp#L1958))。報告の前は出ません。
  - expand のループ: `×N copies` と **Show in Frame Plan** のボタンです。押すと Frame Plan のタブが前に出て、frame plan を読み直し、届いたらそのループのグループを開いて、回の node を全部選んで中央に寄せます([グループを開いて選ぶ所](../../src/devstudio/view/frameplangraphics.cpp#L3754))。Frame Plan の status 行は `Loop <loop>: its copies are picked.` です。plan にそのループの回が無ければ `The frame plan has no copies of loop <loop>.` です。✅UX-12b から、この行は、回が選ばれたままの間(と次の編集を送るまで)残ります([選んだループの行を戻す所](../../src/devstudio/view/frameplanwidget.cpp#L446))。UX-12b の前は、plan の読みに続く authoring context と task の一覧の返事が Frame Plan を作り直すたびに、ふつうの status 行(`main_render · whole frame · …`)が数 ms で上書きしていました(commit の記録。シナリオ `loop_zone_wire` がこの行を 120 秒待って落ちていました)。
  - draw_loop: `N materials`(1 つなら `1 material`)です。draw_loop は回を複製しないので、ボタンはありません。分岐のタブの字の後には、その分岐に当たった material の数が付きます(`if m#a (1)` / `else (2)`)。
- **Frame Plan からの Open in Graph**: 回の node の **Open in Graph** は、body の scope に入らず、root の canvas のまま、ゾーンのタブをその要素の分岐に合わせ、枠の中のその node を選んで中央に寄せます。畳んだゾーンは開きます([`GraphCanvasWidget::revealLoopOrigin()`](../../src/devstudio/view/graphcanvaswidget.cpp#L10253))。body がゾーンに描かれないとき(同じ definition を先のループのゾーンが描いている)は、LP-4c のとおり body の scope に入ります。
- **枠の追従**: body の node を drag している間も、枠がその node を囲むように広がり、縁のソケット・見出しの帯・下端の行が付いて動きます。ループへ出入りする root の線、右の縁への破線、carry の矢印も引き直します([`GraphCanvasScene::refitZoneLive()`](../../src/devstudio/view/graphcanvaswidget.cpp#L2505))。離すと、今までどおり枠を描き直します。
- **値の帯の切り詰め**: node の値の帯(pill)は、node の幅に収まるように末尾を `…` で切り、全文を tooltip に出します([帯を切る所](../../src/devstudio/view/graphcanvasdrawing.cpp#L544))。render 系に限らず全部の node に効きます。LP-4d2 ではゾーンの中の node だけで、root の node の長い値(image の type の JSON など)は node の左へ大きくはみ出していました。`9ee2d54b` から root でも切ります。
- **Undo**: 上の 3 つの操作(入力・出力・carry)は、それぞれ 1 項目として戻ります。`set_outputs` の逆は、前の `outputs`(無ければ空)を持つ同じ op です(下の「Undo」)。入力・出力・carry を足してから Undo 3 回と Redo 3 回をすると、文書は byte 一致で戻ります(scene の test)。

**render node の入力は shader の宣言から —— 線で番号付きの port を作る(✅Q-19)**: Graph タブの `render.fullscreen` / `render.raster` の node は、engine がその pass の shader の宣言を出していれば、入力のソケットを Frame Plan と同じ形で描きます(✅VI-3a。下の「ソケットは shader の宣言から出ます」)。ソケットへ線を落とすと、Graph タブが engine の番号付きの入力の port(✅LP-1d。`"binding": "input"` と `input_index`。[第6章 §6.18](06_rendering.md) の「番号付きの入力」)を書いてつなぎます。Q-19 の前は、render node に別の画像を読ませるには、parameters の JSON(pill)に port を打つしかありませんでした。

- **宣言の出所**: Graph タブが読む `get_frame_plan` の node の `pass_inputs` です([読む所](../../src/devstudio/model/graphcanvasmodel.cpp#L5193))。Graph タブの node と plan の pass は次のように対応づけます([`GraphCanvasModel::passInputs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5223)):
  - root の node: 名前がその id の pass
  - ループの body の node: plan の順で最初の、`loop_origin` の `node` がその id で、その要素の分岐がこの body を走らせる回の pass(どの回も同じ shader で描くため、1 つの回の宣言を使います)
  - instance の definition の node: `<instance>/<id>` の pass
- **形**: 固定の宣言は ○ 1 つずつで、字は宣言の変数名です(つながっていれば `<変数名> ← <node>.<port>`)。可変の組は、つながっている ○(字は `← <node>.<port>`)と下端の空の落とし先 `+ input` を半透明の帯が囲みます([Graph の render node のソケットを宣言から組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L378)、[帯を描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L510))。宣言のソケットは binding の順で、node のほかの行(書く port など)より上に並びます。buffer の宣言も ○ で描きます。
  - 番号付きの port はソケットのものになり、port の行としては出ません。宣言が取らない番号付きの port は、port 名の ○(`<port> ← <node>.<port>`)で残ります。線の行き先が無くならないようにです。
- **宣言が無い node は今の行のまま**です。plan にその pass が無い、`pass_inputs` が `declared` でない(pipeline がまだ無い、など)、宣言が 1 つも無い、Studio が読めない宣言のどれでも、Q-19 の前と同じ port の行で描きます([分ける所](../../src/devstudio/model/graphcanvasmodel.cpp#L2152))。
- **plan を読む時**: 上の「ループの node」の読みに加えて、root の instance の definition に render node があるときも読みます。✅GL-1 からは条件が無く、文書を読むたびに読みます([`GraphCanvasModel::requestLoopReports()`](../../src/devstudio/model/graphcanvasmodel.cpp#L832))。足した直後の pass は、engine がそれを持つ世代を出すまで plan に載りません。そのため、render node の pass が plan に無い(か宣言が `declared` でない)間は、文書を読むたびに最大 40 回まで、時計で plan を読み直します([読み直す所](../../src/devstudio/model/graphcanvasmodel.cpp#L995))。回数 0 と報告されたループの body は待ちません。

線を落とした先で起きることは次のとおりです。どれも 1 回の編集で、Undo 1 回で戻ります([列を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L5403))。入力のソケットから出力へ逆向きに引いても同じです。

| 落とした先 | 送るもの |
|---|---|
| 空の ○ か `+ input` で、binding が次の番号(0 から続く番号付きの port の数) | `set_node_parameters`(`ports` に `{"kind": "image", "type": …, "access": "read", "binding": "input", "input_index": <b>}` を足す)と `set_input`(その port ← 落とした出力) |
| つながっている ○(宣言が取らない port の ○ も) | その port の `set_input` の差し替え。source の型が今の `type` と違えば、先に `type` を直す `set_node_parameters` も送ります。同じ線なら何も送りません |

- **type**: engine は image の port に `type` を要るので、落とした出力から辿ります([`GraphCanvasModel::imageTypeOf()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5345))。pass の port の `type`、image などの `parameters.type`、body の `graph.input` ならそのループか instance に入っている線の source、instance やループの出力なら body の出力の source、の順です。分からなければ、Add fullscreen pass の image と同じ scene color の型(`pelican.render.color_signal@1`、`reference: scene`・`transfer: linear`・`range: extended`)です。
- **port の名前**: source が body の `graph.input` ならその id、それ以外は source の port 名から、ループの入力と同じ規則(`.` と `/` は `_`、末尾の `[]` は外す、空なら `input`)で提案します。node の ports か inputs と重なれば `_2`、`_3` … です([`GraphCanvasModel::suggestInputPortName()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5324))。打つものはありません。
- **外す**: 番号付きの port の線を選んで Delete するか、つながっている ○ を空所へ drag すると、`set_input{source: null}` と、port を消して後ろの port の `input_index` を 1 つずつ詰めた `set_node_parameters` を 1 回で送ります([`GraphCanvasModel::removeInputPortOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5547))。
- **断るもの**(送らず、notice に 1 行):
  - 飛ばし: `Connect binding <n>..<b-1> of <node> first: a pass's inputs fill the bindings in order.`(飛ばしたのが 1 つなら `Connect binding <n> of ...`)。可変の組の前の固定の宣言が埋まっていないときの `+ input` も同じです
  - 可変の組が max まで埋まっている: `<node>'s shader takes at most <max> inputs in its variadic group.`
  - buffer の宣言: ✅UX-8 から断らず、名前付きの buffer の port を作ってつなぎます(下の「曲線を shader の値へ GUI でつなぐ」)。UX-8 の前は `Binding <b> of <node> takes a buffer: a graph pass reads only images by number.` で断っていました
  - 宣言に無い binding: `<node>'s shader declares no binding <b>.`
- **ゾーンの外から body のソケットへ**: root の node の出力を、枠の中の body の render node の宣言のソケットへ直接落とすと、上の「外から入力を足す」の列(`add_node graph.input` と root の `set_input`)の後に、その `graph.input` の `value` を読む番号付きの port の `set_node_parameters` と `set_input` を付けて、1 回で送ります([`GraphCanvasModel::addZoneInputToSocket()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5568))。port の名前は新しい `graph.input` の id、型は root の source から辿ります。飛ばしなどで断るときは、ループの入力も足しません。
- **carry で前の回の出力を読む**: 線だけで組めます。(1) root の pass の出力を左の縁の `+` へ(ループの入力)、(2) body の pass の出力を右の縁の `+` へ(ループの出力)、(3) 右の縁の内側の端を左の縁の内側の端へ(carry)、(4) 左の縁の内側の端(body の `graph.input`)を body の pass の宣言のソケットへ落とします。(4) で pass がその `graph.input` を番号付きの入力として読み、展開された 2 回目の pass が 1 回目の出力を読みます(frame plan の `loop/1/render_fullscreen` の `reads` に `loop/0/render_fullscreen__color` が入ります。シナリオ `loop_zone_read`)。
  - carry の矢印は左の縁の内側のソケットで終わり、その上に描かれます。Q-19 から、矢印の下のソケットを押すと、carry を選ぶのではなく線を引き始めます([押した所](../../src/devstudio/view/graphcanvaswidget.cpp#L3235))。Q-19 の前は、carry の付いた入力からは線を引けませんでした(実装者の報告)。
- **分かっている穴**(Q-19 の着地時の記録。実装者の報告):
  - ✅LP-6(`03725036`)で塞がりました。root の `render.image` から引いた線も通ります(engine の規則: image node から読む pass は、その image を書く graph の pass を待つ。待たない場合も含めて [第6章 §6.18](06_rendering.md) の「image node を読む pass の順」と [design_graph_loops.md](../design_graph_loops.md) §5.2)。Q-19 の着地時は、root の `render.image` の出力を body の pass のソケットへ引くと、engine が `Pass input target is not produced as an earlier output: <image> in pass: <pass>` で断り([その文を投げる所](../../src/core/renderingpass/frameplanner.cpp#L1929))、シナリオのように root の pass の出力 port から引いて避けていました。
  - **LP-6 が入るまでの一時的な案内(Q-20、Q-21 で外した)**: Q-20 から Q-21 までの間、Graph タブは上の拒否を受けると、notice の拒否の行の後ろに「その image を書く pass から線を引く」案内を足していました(`render.image` から読む線を持つ編集のときだけ。engine はこの拒否に専用の code を付けないので、判定は message の文字列でした)。LP-6(`03725036`)で engine が直ったので、Q-21(`5b4b3737`)で案内とその判定を外し、シナリオ `loop_zone_read` に root の `render.image` から body の pass へ引く線を足しました。
  - シナリオ `loop_zone_read` は、frame plan の回の pass を位置(`nodes[2]` / `nodes[3]`)で見ています(同じ位置の `name` が合うことも見ます)。
  - シナリオで `Graph change saved.` を待った直後に発火する action は、文書の読み直しの途中で disabled のことがありえます(`loop_zone_wire` も同じ手順です)。

自動化面の名前(表の正は [README](../../test/studio_scenarios/README.md)。[definition を前に付ける所](../../src/devstudio/automation/automationcommands.cpp#L320)):

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
- ✅LP-4d2: 同じ名前の入力と出力を持つ node(fullscreen pass の `color` など)のソケットは、`graph_socket#<id>/<port>` では分けられません。そのため `graph_socket#<id>/in:<port>` / `graph_socket#<id>/out:<port>` の別名でも引けます(ゾーンの中は `graph_socket#<definition>/<id>/in:<port>` …)。別名は名前を解決するときだけ効き、`list` には出ません([別名を作る所](../../src/devstudio/automation/automationcommands.cpp#L339))。
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
| Over | ✅LP-4f1。`Count`(`over.range`)/ `View axis`(`over.view_axis`)。下の「view の軸で回す」。✅GL-1d で `Draw items`(`over.draw_items`)。ループの `draws` を読む root の material pass があるときだけ選べます。無ければ項目は押せず、tooltip が `No material pass reads this loop's draws: connect its draws to a material pass first.` を言います([項目を出し分ける所](../../src/devstudio/view/graphcanvasloopfields.cpp#L617))。選ぶと、その pass の契約で `over: {draw_items: {contract}}` を書き、`tier: "draw_loop"`・`order: {by: "phase_default"}` にして、draw_loop が受けない `limit`・`carry`・`where`・`insert`・`canonical_anchor` を消して送ります([`GraphCanvasModel::setLoopDrawItems()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4077))。draw_loop になると Over の行は出ません(下の「draw_loop の Loop の欄と分岐の when」)。test: `devstudio_graphcanvas_test` の [「GL-1d a loop a material pass reads can run over its draw items」](../../test/devstudio_graphcanvas_test.cpp#L2097) |
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
- **View の行**: family の combo(`viewFamily`)と軸の combo(`viewAxis`)、右に回数 `×6 (face)`(`viewCount`。読むだけ)です。family か軸を選ぶたびに `over` を丸ごと置き換えて送ります。ほかの parameters は残ります([`GraphCanvasModel::setLoopViewAxis()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4052))。
  - 候補は、Graph タブが読んだ frame plan の node の `view_axes` を family ごとに集めたものです(軸を持つ family だけ。`$main`・planar の反射・作者の family は出ません。[集める所](../../src/devstudio/model/graphcanvasmodel.cpp#L3929))。どの pass も走らない family は候補に出ず、欄の下に `Families no pass of the frame plan runs in are not listed.` が出ます。
  - 軸の項目は `face ×6` の形です。実行時に決まる軸(directional の影の `light`)は `light (decided each frame)` と出て選べません(tooltip `The engine decides this axis each frame: a loop cannot run over it.`)。
  - 文書が名指す family / 軸が plan に無ければ、`(not in the frame plan)` を付けて残します。
  - 選べる軸を持つ family が 1 つも無いときに View axis を選ぶと、何も送らずに Over を Count に戻し、欄の下に `No pass of the frame plan runs in a view family with an axis a loop can run over.` が出ます。
- view の軸のループでは、Count の行と挿入のボタンは出ません。Where と分岐表の check は軸のラベル付き(`2 (+Y)`)です。

**body の render node の View 欄 —— Each element(✅LP-4f1)**: Graph タブで render node を選ぶと、詳細表の上に LP-4b と同じ **View** の欄(`pelican.graphCanvasRender`。軸の行は `pelican.graphCanvasRender.view.<axis>`)が出ます([`GraphCanvasViewFields`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1793))。

- 対象の op は `render.fullscreen` / `render.raster` / `render.compute` / `render.material_pass` / `render.shadow_depth` / `render.snapshot_copy` です([`GraphCanvasModel::takesViewSelect()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4046))。
- どの family の軸を出すか([`GraphCanvasModel::viewField()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4092)):
  - view の軸で回すループの body の node は、そのループの family(node が別の `view_family` を名指していれば対象外)。見出しは `View (<family>) · in loop <loop>` です。
  - root の node は、frame plan のその node の family。見出しは `View (<family>)` です。
  - どちらでもなければ node 自身の `view_family`(frame plan がその family の軸を出しているとき)。
  - 軸が取れなければ欄は出ません。
- 行の中身は LP-4b の欄と同じ(`All views` と値、実行時の軸は字だけ)です。ループの body の node では、ループの軸の combo に **Each element (loop)** が足されます。選ぶと、その回の element の view で走る `{"$expr": "element"}` を書きます。ほかの軸・ループの外の node には出ません。
- 選ぶたびに `set_node_parameters` を 1 回送ります([`GraphCanvasModel::setViewSelect()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4147))。
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

**新しい node の置き場所(✅LP-4f1)**: palette・Add loop・Add fullscreen pass で足した node は、選んでいる node(ゾーンなら枠)の描かれた箱の右端の隣に置かれ、同じ scope の描かれた箱と重なる間は下へずれます([`GraphCanvasScene::placeNewNode()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4002))。

- 新しいものの大きさは描くまで分からないので、ループなら枠の最小の大きさ、ほかは数行の node と見ます。
- 選んでいる node が無いときは右クリックした所が起点です。ゾーンの body に、メニューからでも選んだ node の横でもなく足した node は、今までどおり枠の中で自動で並びます。
- 自動配置で描かれていた node は、このとき今の位置でセッションに固定されます(新しい node が自動配置の列を取って、ほかの node を押し出さないように)。位置はどれもセッションの状態で、文書には書きません。
- LP-4f1 の前は、選んでいる node の横に node の幅を 240 と見て置いていたので、幅の広いゾーンの横に置くと重なりました。
- ✅UX-8 から、1 回の追加で主の node と一緒に来る node(scope の `scene_time`、必須の入力の種の定数、compute の書く image など)にも位置を与えます。主の node の左に縦に並べ、同じ scope の描かれた箱と重なる間は下へずらします([`GraphCanvasScene::placeCompanions()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4133))。UX-8 の前はこれらの node は配置を持たず、canvas の初期配置で描かれていました(仕様の決定 8 は「Auto layout を押さずに済む」ことを求めました)。

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
  - ✅UX-1 から、新しい分岐の body は else の body を写した新しい definition です。名前は Studio が `<ループの id>_body` から提案し(使われていれば `_2` …)、`add_definition` と分岐を足す `set_node_parameters` を 1 つの列で送ります。Undo 1 回で両方が戻ります([写す所](../../src/devstudio/model/graphcanvasmodel.cpp#L4273))。else に body が無い(か、文書が定義していない)ときは写さず、今までどおり分岐だけを足します。
  - UX-1 の前は body が else と同じ definition だったので、新しい分岐のタブを押しても同じ body が出て、そこを直すと else も変わりました(利用者の指摘「else タブは追加されても開いて編集できないのは仕様?」)。
  - test: `devstudio_graphcanvas_scene_test` の [「UX-1 Add branch gives the branch a body of its own, and the chosen tab is plain」](../../test/devstudio_graphcanvas_scene_test.cpp#L8038)。
- 各行の下に、engine の報告(`loops[]` の `elements` の `branch`)から当たった要素が `elements: 0, 2` の形で出ます。
  - どの要素も当たらない行(`unmatched_branches`)は、赤で `no element takes this branch` です。
  - 報告の前は `elements: not reported yet` です。

**body に入る(✅LP-4c)**: ループをダブルクリックするか、Loop の欄の **Open body** で、選んでいる分岐の body の scope に入ります。✅LP-4d1 からの既定の見え方はゾーン(上)で、この「入る」道も残っています。ゾーンでは、枠の中の node の無い所のダブルクリックで入ります(見出しのダブルクリックは改名)。

- パンくずは instance と同じです(`root / loop_body`)。既定の分岐は、先頭から見て最初に body を持つ分岐です。
- 中にいる間は、パンくずの右に分岐の combo(`pelican.graphCanvasLoopBranch`)が出ます([分岐の combo を合わせる所](../../src/devstudio/view/graphcanvaswidget.cpp#L8229))。
  - 項目は `loop: element = 0 → loop_body` の形で、body を持つ分岐だけです。
  - 選ぶと、その分岐の body に切り替わります。
- 同じ definition を root の instance やループの分岐が 2 か所以上で使っていると、その右に橙色で `shared (N users)` が出ます(`pelican.graphCanvasShared`)。ここで直すと、使っている所が全部変わります。

**回ごとの入力 —— Per iteration(✅LP-4c)**: root のループの入力ソケット(か行の字)を右クリックすると、`p[]` を宣言した**他の**ループがあるときだけ **Per iteration…** が出ます。

- dialog(`pelican.graphCanvasPerIteration`)で、読む `p[]`(`<ループ>.<出力>[]`)と Element の式を選びます。式の既定は `element`(受け手のループの要素)です。
  - ✅LP-4f1 から、Element は `element` / `element - 1` / `element + 1` / **Custom…** の combo です。Custom… のときだけ式の欄(`pelican.graphCanvasPerIteration.element.custom`)が出て、空なら `element` です([`GraphCanvasElementChoice`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1752))。LP-4f1 の前は式を打つ欄でした。
- OK で、`set_input` の source に `element: {"$expr": <式>}` を付けて 1 回送ります([`GraphCanvasModel::connectPerIteration()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4498))。
- 張った入力の要約には `[element {"$expr":"element"}]` が付きます。
- ✅LP-4d2 から、canvas ではその線が破線で描かれ、線の中ほどに `[element]` のラベルが乗ります(上の「縁をまたぐ線・carry・展開の表示」)。

**Undo(✅LP-4c)**: Graph タブの Undo は、1 つの編集の op の列を、前の op を当てた後の文書で 1 つずつ逆にします([`GraphCanvasModel::inverseOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2565))。

- 同じ列で足した node を後の op が指していても戻せます(Add loop、Add fullscreen pass、New definition…)。
- definition の op も戻せます。`add_definition` の逆は `remove_definition`、`rename_definition` の逆は名前を戻す rename、`remove_definition` の逆は本体つきの `add_definition` です。
- ✅LP-4d2 から、`set_outputs` も戻せます。逆は、前の `outputs`(無ければ空)を持つ同じ op です([逆にする所](../../src/devstudio/model/graphcanvasmodel.cpp#L2656))。canvas が送るのは definition の `set_outputs` だけです。

**definition の改名と削除(✅Q-19)**: Graph タブから definition の名前を変え、どこからも使われていない definition を消せます。送るのは engine の `rename_definition` / `remove_definition`(✅LP-0c。上の「`ops[]` と `scope`」の表)の 1 op で、どちらも Undo 1 回で戻ります(上の「Undo」の逆の op)。Q-19 の前は、Studio にこの 2 つの入口がありませんでした。

- **入口**は右クリックの **Rename definition…**(`pelican.graphCanvas.renameDefinition`)/ **Delete definition**(`pelican.graphCanvas.deleteDefinition`)で、次の 3 か所に出ます。
  - パンくず: canvas の中のパンくずと上段のパンくずの字。今見ている definition が対象です([canvas のメニューに足す所](../../src/devstudio/view/graphcanvaswidget.cpp#L3536))。
  - ゾーンの見出し: そのタブが見せている body の definition が対象です。
  - 上段の `Scope:` の combo: 選んでいる definition が対象です(`root` では出ません)。
  - メニューを通さず名前で発火したときは、今見ている definition、root ならゾーン(か枠の中の node)を選んでいればその body の definition が対象です。対象が無いか編集できない間は押せません([対象を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L7708))。
- **Rename**: 上段の下に 1 行(`pelican.graphCanvasDefinitionRow`)が開きます。`Rename definition <name> to`、名前の欄(提案の名前が入って全部選ばれた状態)、**Rename**、Cancel です([行を開く所](../../src/devstudio/view/graphcanvaswidget.cpp#L7780))。Enter で Rename、Esc で Cancel です。
  - ✅UX-3 から、欄に入る名前は Studio の提案です。その definition を走らせる最初の node の id に `_body` を付け、ほかの definition と重なれば `_2` … を足します。それが今の名前と同じときと、どこからも走らせていないときは今の名前です([提案を入れる所](../../src/devstudio/view/graphcanvaswidget.cpp#L7800)、[`GraphCanvasModel::proposeDefinitionName()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7872))。UX-3 の前は今の名前でした。
  - Studio は送る前に名前を確かめ、送れない名前なら欄の右に赤で理由を出して Rename を押せなくします([名前を確かめる所](../../src/devstudio/model/graphcanvasmodel.cpp#L4443))。今の名前のままでも押せません(何も送りません)。
    - 空: `A definition needs a name.`
    - `/` を含む: `A definition's name cannot hold a slash: <name>.`
    - `root`: `A definition cannot be named root: that name is the root document's.`(`root` は root の scope の名前です。engine も断ります)
    - 既にある: `A definition named <name> already exists.`
  - 送るのは `rename_definition` 1 つだけです。engine が同じ op の中で、名指している `graph.instance` の `definition` とループの `branches[].body` も書き換えます(上の「`edit_graph_document` の definition の op」)。
  - 改名した definition の中にいれば、パンくずと今の scope は新しい名前へ移ります。Undo で名前が戻れば、元の名前へ戻ります([移す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1514))。
- **Delete**: 使われていれば、何も送らず行も開かずに、notice に 1 行を出します([断る所](../../src/devstudio/model/graphcanvasmodel.cpp#L4471))。`Definition <name> is used by <node>, <node>: make them run another definition first.` の `<node>` は、その definition を走らせる root の node(instance と、どれかの分岐がそれを body にするループ)を文書の順に 1 つずつ並べたものです。
  - 使われていなければ、上段の下の行が `Delete definition <name>? Nothing runs it.` と **Delete** / Cancel で開き、Delete で `remove_definition` を送ります。確認は dialog ではなくこの行です。
  - engine が断ったときの `graph.definition_referenced@1` の文は、下の拒否の表にあります。
- 開いている行の definition が文書から消えると、行は閉じます。
- 提案のままなら Enter だけです。違う名前は打ちます(全部選んだ状態で開くので、打つと置き換わります)。

**Frame Plan のループのグループ(✅LP-4c)**: Frame Plan は `loops[]` と、展開された pass・task・target・buffer の `loop_origin` を読みます。

- 1 つのループが展開した回の pass と task は、1 つのグループ `loop <id> ×N` になります([ループのグループを決める所](../../src/devstudio/view/frameplangraphics.cpp#L533))。
  - グループは最初に見えたときに畳まれ、その後は開いた・閉じたをそのまま保ちます([最初に畳む所](../../src/devstudio/view/frameplangraphics.cpp#L2529))。
  - region や feature のグループより先にループで分けます。lowering graph の無い plan でも、ループのグループは作ります。
- 回の node の詳細に `Loop origin` の行(`loop steps · element 2 · body node fill`)が出ます。target と buffer にも出ます。
- 回の node の右クリックの **Open in Graph** は、✅LP-4d2 から、root の canvas のままゾーンのタブをその要素の分岐に合わせ、枠の中のその node を選びます(上の「縁をまたぐ線・carry・展開の表示」)。body がゾーンに描かれないときは、その要素の分岐の body の scope に入り、body のその node を選びます(LP-4d2 の前はいつもこの形でした)。
- ✅LP-4d2: Graph タブのゾーンの **Show in Frame Plan** は、plan を読み直してから、このグループを開いて回の node を全部選びます(グループの枠そのものは選べないため。[`FramePlanGraphicsScene::selectLoopGroup()`](../../src/devstudio/view/frameplangraphics.cpp#L3754))。
- ✅Q-18 から、Graph タブでループを変えた後は Frame Plan が自分で読み直します(次の段落)。Q-18 の前は、Frame Plan の **Refresh** を押すまで読んだ時点の写しのままでした。

**Graph タブの編集の後は Frame Plan が自分で読み直します(✅Q-18)**: Graph タブの編集が commit すると、Frame Plan は `get_frame_plan` を読み直します。**Refresh** は要りません。

- 知らせるのは、配置(`set_layout` だけの op の列)ではない commit だけです。Graph タブの model がその回数を数え([`GraphCanvasModel::planCommitRevision()`](../../src/devstudio/model/graphcanvasmodel.hpp#L1734))、増えたら Graph タブが MainWindow に知らせ、MainWindow が Frame Plan に渡します([つなぐ所](../../src/devstudio/view/mainwindow.cpp#L793))。
  - 下の「配置の保存の後の編集も通ります」(Q-13)の共有の digest では知らせません。digest は配置の保存でも動き、Frame Plan の node の位置の保存も Graph の `set_layout` なので、それで読み直すと node を drag するたびに plan を丸ごと読み直すことになるためです(コードの注記と実装者の報告)。
- Frame Plan の側([`planChangedByCommit()`](../../src/devstudio/view/frameplanwidget.cpp#L3768)):
  - 見えていれば、すぐ読みます。
  - 前の読みが往復中なら、その返事の後にもう 1 回読みます(往復中の読みは commit より前の plan かもしれないため)。
  - 見えていなければ印だけ付け、次に表示したときに読みます。engine の RPC が無いときは何もしません。
- Graph タブ自身も、配置でない commit の後は、読み直した文書が配置だけ違うように見えても `get_frame_plan` を読み直します。ゾーンの `×N copies` などが回数の変更に追いつくためです([その分岐](../../src/devstudio/model/graphcanvasmodel.cpp#L1506))。
- シナリオ `graph_loop_expand` / `loop_view_axis` / `material_loop_decompose` は、Graph タブの編集の後に Frame Plan の Refresh を押さなくなりました。
- ⚠ MainWindow のつなぎを通して見ているのは GPU シナリオ(`graph_loop_expand`)だけです。CPU の test は、Graph タブが知らせることと Frame Plan が読み直すことを別々に見ます(Q-18 の着地時の記録)。

**ループと definition の拒否は 1 文で出ます(✅LP-4c / LP-4e)**: `graph.loop_*` と `graph.definition_*` の拒否(✅Q-19 から `graph.input_index_invalid@1` も)は、field の JSON のままではなく、1 文と末尾の code で出ます([`graphLoopRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L1693)、[拒否の 1 文の表 `GraphLoopRefusalTexts` の宣言](../../src/devstudio/model/inspectormodel.cpp#L1339))。

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
- `graph.input_index_invalid@1` の `reason` も、それぞれの文に替わります(✅Q-19。[reason の文](../../src/devstudio/model/inspectormodel.cpp#L1522)):
  - `missing`: `No port of <node> reads input <input_index>: its numbered inputs run from 0 without a gap`
  - `duplicate`: `Ports <ports> of <node> all read input <input_index>: each number is one port's`(`<ports>` は `a, b` の形)
- fields は、code の横、`error_fields`、別の拒否の message の中の `<code> {…}` / `<code>: {…}` のどこからでも読みます。✅RU-1b から、message の中の field を code の横の同じ名前のものより優先します。RU-1b の当時は、変種の preflight で code の横の `variant` が変種の名前になり、`graph.loop_variant_missing@1` 自身の `variant`(material の変種)が message の中にだけ残ったためです。今はこの field は `material_variant` で、`variant` という名前の field を持つ拒否はありません(下)。
- 出る所は、Graph タブの notice と、Frame Plan の status 行です。render target の form は、変種が断ったときと、ループの code の拒否のときにこの文を使います(後者は ✅RU-1b から。LP-2 で draw_loop の拒否 5 つが最上位の code で届くようになったためです)。
- **変種の前置き(LP-0d)**: 変種(preview / xr)の preflight が断ったときは、code が何であっても前に `The <variant> graph refused it: ` が付きます。LP-0d から、変種の graph の拒否が graph の code のまま `variant` 付きで返るためです(それまで Frame Plan の render target の form は、`render_pipeline_preflight_failed` のときだけ前置きを付けていました)。
  - ✅PV-1(engine `a173e70c`)から engine は preview の変種で編集を断らないので、engine が `variant` を付けて返すのは xr だけです。`The preview graph refused it: ` は今の engine からは出ません(engine の側のコードの読み。Studio は変えていません)。
  - `graph.loop_variant_missing@1` の material の変種は field `material_variant` で届きます(engine 5e246063)。code の横の最上位の `variant` は、どの拒否でも常に graph の変種で、前置きはそれで付けます(✅LP-4d2。[`graphRefusalVariant()`](../../src/devstudio/model/inspectormodel.cpp#L2348))。
  - ⚠ RU-1b の当時は、この拒否が material の変種を `variant` という名前で運んでいました。そのため、message の `<code>: {…}` に同じ `variant` があれば graph の変種と見なさず、前置きを付けない回避を入れていました(その前は `The outline graph refused it: ` のように付いていました)。LP-4d2 でこの回避を外しました。
- **赤い印**: 拒否が `graph.loop_*` の code で、`node` にループの id を持つとき、そのループの node の縁が赤くなり、hover で文が出ます。詳細表にも `refused` の行が出ます([`GraphCanvasModel::markRefusal()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4826))。
  - 拒否が `loop_origin` を持つとき(回の node が断られた)は、ループと、その要素の分岐の body の中のその node の両方が赤くなります。
  - 次に commit した編集で消えます。

**material pass をループに分解する —— Decompose into loop(✅LP-4e)**: rendering 節(か fork 済みの preset)の一体版の material pass を、`render.material_pass` と、material ごとの分岐を持つ draw_loop のループに書き換えます。書き換えの中身は上の「ループと分解の RPC」の `decompose_material_pass` です。

- 入口は 2 つです。どちらも同じ処理を呼びます。
  - Frame Plan の material pass の右クリックの **Decompose into loop...**(`pelican.framePlan.decomposeIntoLoop`)
  - Pass Editor で material pass を開いたときの **Decompose into loop...** のボタン(`pelican.fullscreenPass.decomposeIntoLoop`)
- 分解できないと分かる pass では、右クリックの項目が押せず、理由が tooltip に出ます([`framePlanDecomposeRefusal()`](../../src/devstudio/view/frameplanedit.cpp#L1572))。
  - feature の pass(feature の `pass_replacements` で置き換える)、material でない pass
  - 既に draw_loop が描いている pass(`<pass> is already a render.material_pass of the graph; its loop <loop> is edited in the Graph tab.`)
  - fork していない engine の preset の pass(`… fork the preset into the project first (Features tab, Fork), then decompose the pass there.`)
- 押すと確認のダイアログ(`pelican.framePlan.decomposeConfirm`)が出ます。ボタンは **Decompose** と Cancel で、既定は Cancel です。
  - 文は、分解で何ができるか(material の種類ごとに、別の variant・別の render state で描く、描かない)と、元に戻せるかを言います。
  - LP-4e の着地時は「まだ元に戻せない(RD-1 で Undo が付く)」と言っていました。RU-1(`fa413aeb`)からは `Undo takes it back (Undo on the Frame Plan, or Ctrl+Z there).` と言い、分解は render の編集の履歴に `Decompose <pass>` として残ります。
  - Cancel は何も送らず、status 行が `<pass> is left as it is.` を言います。
- Decompose を押した時点の context で要求を組み直して送ります([`framePlanDecomposeRequest()`](../../src/devstudio/view/frameplanedit.cpp#L1604))。direct root の pass は `base_source_digest` だけ、fork 済みの preset の pass は `base_preset_digest` も付けます。
- commit されると Graph タブが前に出て、できたループが選ばれます。Graph タブの文書がまだ古ければ読み直し、ループが届いてから選びます。
- engine の拒否は 1 文で、Frame Plan の status 行に出ます([`framePlanDecomposeRejection()`](../../src/devstudio/view/frameplanedit.cpp#L1642))。

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

**feature をループ版に置き換える —— Replace with loop version…(✅LP-4f2)**: engine が分解版の雛形を同梱している feature(bloom・cube capture・planar reflection の mip chain・directional の影の cascade)を、その雛形のループに置き換えます。中身は上の「ループと分解の RPC」の `apply_graph_template` です。どの feature に分解版があるかは `get_graph_templates` の `decomposes.feature` で決まり、Studio は表を持ちません([`graphTemplatesFromResponse()`](../../src/devstudio/view/frameplanedit.cpp#L1693))。Render Features は一覧を読み直すたびにこれも読みます。`get_graph_templates` を持たない engine では、入口は出ません。

- 入口は 2 つで、どちらも Render Features の同じ処理を呼びます。
  - Render Features で分解版がある feature の行を選ぶと、一覧の下に **Replace with loop version…**(`pelican.renderFeatures.replaceWithLoop`)が出ます。行の右クリックにも同じ項目があります([出し分ける所](../../src/devstudio/view/renderfeatureswidget.cpp#L4394))。分解版の無い feature では出ません。
  - Frame Plan で feature のグループを右クリックすると、同じ名前の項目(`pelican.framePlan.replaceWithLoopVersion`)が出ます([項目を足す所](../../src/devstudio/view/frameplangraphics.cpp#L5069))。押すと Features タブが前に出て、その feature の行で同じ流れが始まります。
- 押せないときは理由が tooltip に出ます。fork していない engine の preset の feature(`<feature> is a feature of the engine preset <preset>: fork the preset into the project first (Features tab, Fork), then replace the feature there.`)、編集できない状態、Apply していない編集がある、のどれかです。
- 押すと確認のダイアログ(`pelican.renderFeatures.replaceConfirm`)が出ます。ボタンは **Replace** と Cancel で、既定は Cancel です([文を組む所](../../src/devstudio/view/frameplanedit.cpp#L1758))。
  - 1 行目は雛形の title(と description)です。例: `Bloom loop`。
  - 続けて何が起きるか: `The feature bloom is removed, and the graph draws what it drew.`(`pass_replacements` の雛形なら `The feature <feature> stays; N of its passes are replaced by <node>.`)、`The graph gains N nodes and M definitions.`、足す define があれば `Shader defines added: …`。
  - 最後に `Undo takes it back (Ctrl+Z here, or Undo on the Frame Plan).`
  - Cancel は何も送らず、`<feature> is left as it is.` を言います。
- Replace を押した時点の一覧から要求を組んで送ります。direct root の feature は `base_source_digest` だけ、project の preset が列挙する feature はその行の preset の digest も `base_preset_digest` に付けます([送る所](../../src/devstudio/view/renderfeatureswidget.cpp#L4320))。
- commit されると、Graph タブが前に出て、雛形が足したループ(応答の `applied.nodes` のうち雛形が `graph.loop` と宣言したもの)が選ばれます。Render Features の notice は緑で `Replaced bloom with Bloom loop. Undo takes it back.` を言い、一覧を読み直す間も残ります。
- 置き換えは render の編集の履歴に `Replace bloom with Bloom loop` として積まれ、Undo / Redo で戻せます(下の「render の編集を戻す」)。
- engine の拒否は 1 文で notice に出ます([`graphTemplateRejection()`](../../src/devstudio/view/frameplanedit.cpp#L1791))。

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
- material の候補は、報告が 1 ページに収まらない(`total` が `elements` より多い)とき、Graph タブが `loop_elements` で残りを読んで全部集めます([`GraphCanvasModel::requestNextLoopPage()`](../../src/devstudio/model/graphcanvasmodel.cpp#L856))。
- **Add branch** の新しい行の条件は、報告の最初の material の `material = <鍵>` です。報告の前なら `has_variant = yes` です。✅UX-1 から、body は上の「分岐表」と同じく else の body の写しです(`<ループの id>_body`)。
- body の combo の `(nothing)` は、その分岐に当たった material を描かないことです。

**render.material_pass と draw.* の node(✅LP-4e)**:

- `render.material_pass` のソケットは契約から出ます([`GraphCanvasModel::materialPassColorSockets()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5044))。
  - forward(`forward_opaque_v1` / `forward_transparent_v1`)は `color`、G-buffer(`deferred_geometry_v1`)は `albedo` / `normal` / `material` / `worldpos` / `emissive` です。`material_outputs` を宣言した pass では、その出力の名前です。
  - 続けて `depth` と、任意の `draws` です。
  - ✅GL-1f から、color のソケットと同名の出力と `depth` の出力があります(下の「pass の出力のソケット(✅GL-1f)」)。GL-1f の前は出力ソケットが無く、書いた target は image node か `render.target_ref` から読みました。
- palette の `render.material_pass` は root にだけ出ます。palette の Contract の combo(`pelican.graphCanvasPaletteContract`。`forward_opaque_v1` / `deferred_geometry_v1` / `forward_transparent_v1`)で契約を選びます。
  - ソケットごとの `render.image`(`<pass>__<socket>`)と depth の image を同じ列で足してつなぎます。`draws` には、同じ列で足す draw ループをつなぎます(✅GL-1d。下の「draw ループ付きで来ます」)。
  - image の format と usage は、Frame Plan の Add material pass… の form と同じ表 [`materialPassSlots()`](../../src/devstudio/model/frameplanmodel.cpp#L2263) から取ります(✅MC-1s、`1d10bd4c`。[表から image を足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L3468))。`hdr` の判定は [`materialPassHdr()`](../../src/devstudio/model/frameplanmodel.cpp#L2313) に、Graph タブが持つ authoring context(`get_render_authoring_context` の `enabled_feature_names`。palette を開くときにまだ無ければ読みます)を渡します。
    - G-buffer(`deferred_geometry_v1` / `legacy_gbuffer_v1`): `albedo` / `emissive` は `hdr` があれば `R16G16B16A16_SFLOAT`、無ければ `B8G8R8A8_SRGB`。`normal` / `worldpos` は `R16G16B16A16_SFLOAT`、`material` は `R8G8B8A8_UNORM` です。
    - forward の `color` は `hdr` に関わらず `R16G16B16A16_SFLOAT`、depth は `D32_SFLOAT` です。
    - context が `enabled_feature_names` を持たない(まだ届いていない)ときは、form と同じく HDR とみなします。表の色の slot の数と contract のソケットの数が合わなければ何も置きません。
    - `material_outputs` の無い material pass は、live の material が登録された物理の契約と照らされます。Blank には `hdr` が無いので live の material は SDR の表で、表どおりの format なら engine は起こし直しません(engine の MC-1。§13.5 の RS-1 の項)。
    - MC-1s の前は `hdr` を見ずに `material` 以外の色を常に `R16G16B16A16_SFLOAT` にしていたので、Blank に deferred の pass を置くと `material_physical_contract_changed`(`detail: ["color_formats"]`)で engine が起こし直していました(GL-5c の「分かっている穴」)。コードのコメントは「engine は最初の attachment が `R16G16B16A16_SFLOAT` なら HDR の組を選ぶ」と言っていましたが事実と違い、MC-1s で直しました。
    - 表 `MaterialPassSlot` / `materialPassSlots()` と `hdr` の判定は、MC-1s で view の `materialpassedit` から model の `frameplanmodel` に移し、form と palette が同じ 1 つを使います。
    - test: `devstudio_graphcanvas_test` の [「MC-1s the palette's material pass images take the form's formats for the pipeline's HDR switch」](../../test/devstudio_graphcanvas_test.cpp#L1936)(hdr なしと `["bloom","hdr"]` の context で 3 つの contract を置き、各ソケットの image の format と usage が `materialPassSlots(contract, hdr)` と値そのものに一致する。context が何も言わなければ HDR)。
  - `0c252183` の前は `albedo` と `emissive` も `R8G8B8A8_UNORM` で作っていたので、deferred の pass は `Material pass color output format mismatch: <pass>__albedo` で断られ、engine が止まりました。commit の記録では、白紙の project に置いた文書を headless の player で走らせ、前は止まり後は描けることを確かめています。GPU シナリオはありません(✅GL-1d からは、下の `graph_from_blank` が deferred の pass を Blank の project に足し、frame plan に出るまでを GPU で通します)。
  - **draw ループ付きで来ます(✅GL-1d)**: pass と image と同じ列で、次も足します。Undo 1 回で全部戻ります([ループと body を組む所](../../src/devstudio/model/graphcanvasmodel.cpp#L3411))。形は `decompose_material_pass` が書くもの(と GL-1 の畳んだ node の中身)と同じです。
    - definition `<pass>_draw`: node `step`(`draw.material`、出力 `draws`)が 1 つで、definition の出力 `draws` はその `draws` です。
    - root の `graph.loop` `<pass>_draws`: `over: {draw_items: {contract: <選んだ契約>}}`・`tier: "draw_loop"`・`order: {by: "phase_default"}`・else の分岐 1 つ(body `<pass>_draw`)・出力 `draws`。
    - pass の `draws` 入力 ← `<pass>_draws` の `draws`。
    - 列の順は definition → ループ → image → pass です。root には pass の横にループのゾーン(`draw_loop`)が描かれ、中に body の `draw.material` が出ます。分岐と body は上の「draw_loop の Loop の欄と分岐の when」で編集できます。
    - pass の id は `render_material_pass` です。その id の node、`<id>_draws` の node、`<id>_draw` の definition のどれかがあれば、`render_material_pass_2`、`_3` … にします。
    - GL-1d の前は `draws` を空けていました(一体版と同じ描き方)。material の draw ループは、rendering 節の material pass を Decompose into loop したときにしか作れませんでした。
    - test: `devstudio_graphcanvas_test` の [「GL-1d the palette's material pass arrives with its draw loop, body and draws edge」](../../test/devstudio_graphcanvas_test.cpp#L1867)(列の最初の 2 つの op、pass の `draws` の線、Undo の逆の列に `remove_definition` があること、名前が使われているときの `_2`)。
  - **forward の route の既定を一緒に送ります(✅GL-1d)**: 契約が `forward_opaque_v1` で、pipeline に deferred の material pass が無いとき、Studio は `edit_graph_document` の params に `material_route_default: "forward"` を付けます([付けるかを決める所](../../src/devstudio/model/graphcanvasmodel.cpp#L3649))。engine の側は [レンダー設定を編集する](13_editor.md#レンダー設定を編集する) の「material の route の既定と、受け皿の無い material の拒否」にある RS-2 です。
    - GL-1d の前は、Blank の project で palette の既定の契約(`forward_opaque_v1`)の pass を足すと、`material_route_unserved@1` で断られました(利用者が Release で当たりました)。
    - 判定は Frame Plan の **Add material pass...** の form(下の「forward の既定を同じ Save で入れる(✅G-4)」)と同じ関数です([`materialRouteOffersForwardDefault()`](../../src/devstudio/model/frameplanmodel.cpp#L2258))。deferred の pass とは、契約が `deferred_geometry_v1` か `legacy_gbuffer_v1`(契約が空のものを含む)の pass です([`materialContractTakesDeferredRoute()`](../../src/devstudio/model/frameplanmodel.cpp#L2247))。GL-1d でこの 2 つを `frameplanmodel` に移し、form と Graph タブが共有します。
    - Graph タブが数える pass は、最後に読んだ frame plan の material pass(`material_routing` が契約を言うものだけ)と、root の文書の `render.material_pass` です([`GraphCanvasModel::pipelineHasDeferredMaterialPass()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3659))。
    - Redo も付けて送ります。Undo は付けません。route の既定は list のものではなく project の設定なので、Undo で pass を消しても既定は戻りません([Undo のときは付けない所](../../src/devstudio/model/graphcanvasmodel.cpp#L2763))。
    - 既定が変われば、engine は commit の結果に `restart_required` を付けます。Graph タブはそれで engine を起動し直します(下の「engine の再起動が要る graph の編集(✅GL-1b)」)。
    - この param を知らない engine が `unsupported field` と `material_route_default` を含む文で断ったら、付けずに同じ列を 1 回送り直します。以後その model は付けません([`GraphCanvasModel::retryWithoutRouteDefault()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2773))。この「付けない」は Studio の session の間続きます(実装者の報告)。
    - その結果の `material_route_unserved@1` は、Frame Plan と同じ文(`No pass draws the <route> route, which <名前> take(s). To fix: <hint>.`)で notice に出ます。Graph タブの拒否の文を作る所が [`materialRouteUnservedNotice()`](../../src/devstudio/model/frameplanmodel.cpp#L2331) を使います([使う所](../../src/devstudio/model/graphcanvasmodel.cpp#L35))。GL-1d の前は engine の message がそのまま出ていました。
    - test: `devstudio_graphcanvas_test` の [「GL-1d a forward material pass sets the route default, and falls back without it」](../../test/devstudio_graphcanvas_test.cpp#L2010)(forward_opaque_v1 だけに付くこと、受ける engine では Undo が付けないこと、`unsupported field` の拒否で付けずに送り直し、拒否の文が Frame Plan と同じになり、次の追加でも付けないこと、ほかの失敗は送り直さないこと)と [「GL-1d a pipeline with a deferred material pass offers no route default」](../../test/devstudio_graphcanvas_test.cpp#L2075)。
- material pass を選ぶと、詳細表の上に **Contract** の combo(`pelican.graphCanvasDraw.contract`)が出ます([`GraphCanvasDrawFields::showNode()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1550))。
  - ✅UX-3 から、その下に **Variant**(`pelican.graphCanvasDraw.materialVariant`)が出ます。`(each material's own)` と、engine の material が持つ variant の和です。選ぶと `material_variant` を書き、`(each material's own)` で消します(`set_node_parameters` 1 回。[欄を置く所](../../src/devstudio/view/graphcanvasloopfields.cpp#L1465))。engine が variant を返さず、pass も `material_variant` を持たないときは出ません。
  - ✅UX-11 から、その下に Pass Editor の form の欄(`pelican.graphCanvasMaterial`。Tags・Range・出力・binding・Load / Clear / Store と Materials drawn)が出ます(下の「Graph の material pass の欄を Pass Editor の form で(✅UX-11)」)。
- `draw.material` / `draw.variant` / `draw.override` は、draw_loop が走らせる body(か、既に draw step を持つ definition)の中でだけ palette に出ます。
  - 置いた step は body の列の**先頭**に入ります。今の先頭の `after` を新しい step につなぎ、`draws` を作る step は末尾のままです([draw step を置く所](../../src/devstudio/model/graphcanvasmodel.cpp#L3488))。
- `draw.variant` の欄は **Variant**(`pelican.graphCanvasDraw.variant`。打てる combo)です。候補は、ループが描く pass の `material_variant` です。置いたときもその値が入り、無ければ `variant` です。
  - ✅UX-3 から、engine の material が variant を持つ間は、その和から選ぶだけの combo です(打てません。[選ぶ形にする所](../../src/devstudio/view/graphcanvasloopfields.cpp#L1613))。一覧が空なら今までの打てる combo です。
- `draw.override` の欄:
  - **Cull**(`none` / `front` / `back`)、**Depth test** / **Depth write**(`on` / `off`)、**Blend**(`opaque` / `blend` / `additive`)。✅UX-3 から **Depth compare**(`never` / `less` / `equal` / `less_equal` / `greater` / `not_equal` / `greater_equal` / `always`。[欄を置く所](../../src/devstudio/view/graphcanvasloopfields.cpp#L1515))も。どれも先頭の `(material's)` は鍵を書かず、material の値のままにします。
  - **Defines** は 1 行に 1 つ(`NAME` か `NAME=VALUE`)で、**Apply defines** で送ります。
  - 何も替えない override は送りません。置いたときは `render_state: {cull: none}` が入っています。

**Frame Plan の material pass の loop の行(✅LP-4e)**: draw_loop は展開されず、1 つの material pass の中で走ります。

- draw_loop が描く material pass は、見出しの下に `loop: forward_opaque_draws (3 materials)` の 1 行を持ちます([`framePlanDrawLoopLine()`](../../src/devstudio/view/frameplanedit.cpp#L1564))。
- 詳細に `Draw loop <id>` の組が出て、分岐(`Branch 0` …)ごとに当たった material の鍵が並びます([詳細の material の一覧](../../src/devstudio/view/frameplanwidget.cpp#L4609))。
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

- **plan を毎回読みます。**Graph タブは文書を読むたびに `get_frame_plan` を 1 回読みます([`GraphCanvasModel::requestLoopReports()`](../../src/devstudio/model/graphcanvasmodel.cpp#L832))。GL-1 の前は、ループや view_select を取る render node などがあるときだけ読んでいました(上の「ループの node」)。読みが 1 つ増えます。
  - engine がまだ最初の plan を持たないときは、失敗した読みを繰り返します。回数の上限は 480 で、コードのコメントでは poll の 2 分ぶんです([待つ所](../../src/devstudio/model/graphcanvasmodel.cpp#L1135))。
- **何が出るか**([`GraphCanvasModel::rootFoldedNodes()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5878)):
  - `provider_feature` を持つ pass は、feature ごとに 1 つの畳んだ node にまとまります。見出しは feature の名前(`engine://features/bloom.json` なら `bloom`)、型の所に `feature`、見出しの下に `N passes` です。
  - それ以外の pass は 1 pass 1 node です。型の所は plan の `kind` で、material pass は `material` です。
  - material pass かどうかは plan から決めます。`shader_resolution.state` が `material_owned`、`material_filter` がある、`material_variant` がある、のどれかです([`GraphCanvasModel::readPlanPasses()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5679))。
  - `kind` が `anchor` の node は出ません。
  - node の行は資源の名前です。右が外へ渡す資源(書く)、左が外から読む資源です。feature の中の pass 同士で受け渡す資源は出ません。
- **見た目**: 枠は破線で、地の色は graph の node より 1 段落ちます。theme の token は `derived_border` / `derived_body` です([theme の表](../../src/devstudio/resources/studio_themes.json#L65))。見出しの帯は Frame Plan の出所の色を薄くしたものです。
  - tooltip は出どころで替わります(✅GL-5a から。[`GraphCanvasFoldedNode::tooltip()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5665)): root の rendering 節が宣言した pass(Blank の `present` など)は `From the rendering section: edit it here.`、feature の pass は `From feature <name>: read only here. Expand to edit…`、preset の pass は `From preset <name>: read only here. Expand to edit…`(authoring context が preset の名前を言わなければ `From the preset (<source>): …`)です。GL-5a の前はどれも `From the rendering section (<source>): read only here. Frame Plan and Features edit it.` でした(rendering 節の pass の直し方は下の「rendering 節の pass を Graph の詳細欄で直す(✅GL-5a)」)。潜れる node では `Double-click to look inside.` が続きます([`Builder::addDerivedNode()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L1352))。
- **置き場所(✅GL-1b / GL-1c)**: root の graph の node と畳んだ node を、1 回の配置でまとめて並べます。派生の線も配置の辺に入るので、列は plan の依存の最長経路で決まり、plan の順が左から右へ読めます。Frame Plan の Logical graph と同じ [`layoutNodeGraph()`](../../src/devstudio/view/nodelayout.cpp#L238) です([配置に入れる所](../../src/devstudio/view/graphcanvaswidget.cpp#L2018))。
  - 文書の root の `layout` が graph の node を 1 つでも置くと、その配置はもう graph の node の場所を表しません。このとき畳んだ node は、畳んだ node とその間の線だけで同じ配置をして、root に描いたものの左に並べます。
  - root に描いたもの(✅GL-1c)は、node とその子(ソケット、箱からはみ出す値の字)と、ゾーンの枠全体(見出し・縁の行・carry の矢印・下端の行)です([測る所](../../src/devstudio/view/graphcanvaswidget.cpp#L2116))。コードのコメントでは、1 回の配置の中でゾーンは枠の大きさの 1 つの箱なので、そこでも畳んだ node は枠に重なりません。
  - 並べた場所はセッションの間覚えます。覚えた場所のどれかが root に描いたものに重なる(後で body が埋まってゾーンが大きくなった、node をその上に置いた)か、場所の無い畳んだ node があれば、並べ直して覚え直します([判定](../../src/devstudio/view/graphcanvaswidget.cpp#L2150))。
  - 文書に配置が無い間に graph の node を drag すると、畳んだ node はそのときの場所を覚えます。
  - drag した畳んだ node は、セッションの間その場所に残ります。文書が変わっても戻りません。
  - 畳んだ node の場所は文書に書きません(`set_layout` に入りません)。Auto layout での扱いは下の「Auto layout」です。
  - GL-1 の着地時は、graph の node の左に 1 列、plan の順で縦に並べ、drag した場所も文書が変わると戻っていました。GL-1b の着地時は、Auto layout で覚えた場所を graph の node とだけ比べていたので、後で大きくなったゾーンの上に畳んだ node が乗りました(`loop_zone_wire` で、picking の畳んだ node がゾーンの Show in Frame Plan のボタンを覆い、押すと畳んだ node が選ばれた)。GL-1c で直しました。
  - 分かっている穴(GL-1c の着地時の統合側の報告): root の node の長い値の字(image の `type` の JSON など)は省略されず、左へ大きくはみ出します。コードの上では、はみ出した字も「描いたもの」に入るので、畳んだ node はその分さらに左に並びます。
- **選ぶと**、詳細表の上に 1 行の要約(`pelican.graphCanvasFoldedSummary`)が出ます。`<名前> (<型>): `、feature なら `N passes of <feature>.`、`Reads …; writes ….`、最後に上の tooltip の文です。
- **派生の線**: plan の `reads` / `writes` / `reads_history` から、資源の名前で破線を引きます([`GraphCanvasModel::derivedEdges()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5953))。
  - 書き手は、読む pass より前で最後にその資源を書いた pass です。history の読みでは frame 全体の最後の書き手です。
  - 資源を名乗る graph の node は、その資源を `parameters.name` に持つ `render.target_ref` と、同じ id の `render.image` / `render.buffer` / `render.attribute` です。graph の node の pass が読む側なら、線はその node に付け替えます。plan に書き手が無い資源は、その node から引きます。それ以外は、pass を作った graph の node に付けます。
  - 少なくとも片方の端が畳んだ node の線だけを引きます。同じ 2 つの node の間の資源は 1 本にまとめます。tooltip は `<資源>, …: from the frame plan (read only)` です([`Builder::addDerivedEdge()`](../../src/devstudio/view/graphcanvasdrawing.cpp#L1505))。
  - ✅GL-1e から、rendering 節の pass の `input` が名指す資源は、その pass への破線を引かず、実線で引きます(下の「Graph から画面に出す(✅GL-1e)」)。
  - 破線をダブルクリックしても制御点は入りません。それ以外の操作で op が出ないことは test では見ていません(未確認)。
- **潜る**: 畳んだ node をダブルクリックすると、中を読むだけで見せます([`GraphCanvasModel::enterFolded()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6842))。パンくずは `root / forward_opaque` のように畳んだ node の名前を足し、押すと 1 段ずつ戻ります。`Scope:` のコンボで選ぶと、本物の文書のその scope に戻ります。
  - **material pass**: `render.material_pass` 1 つと、draw_loop のゾーン 1 つです。ゾーンの id は `<pass>_draws` で、分岐は else 1 つ、body `<pass>_draw` は `draw.material` 1 つです(pass に `material_variant` があれば `draw.variant`)。Studio が plan からその場で組み、どこにも書きません([`GraphCanvasModel::foldedDocument()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6714))。
  - 契約は plan の `material_routing` から取ります。取れなければ、`render.material_pass` のソケットは `depth` と `draws` だけです(実装者の報告)。
  - plan の `loops[]` にこのループは無いので、ゾーンの段は `draw_loop` です。plan の `material_filter.resolved_draw_count` があれば、その数を要素の数にします。
  - **feature**: その feature の pass を plan の順に 1 つずつ、上と同じ読むだけの node で並べ、その間の派生の線も引きます。中の material pass にはさらに潜れます。
  - その feature の雛形が `get_graph_templates` にあれば、パンくずの下に **Show decomposed version** のボタンが出ます。押すと雛形の `nodes` と `definitions` を読むだけで描き(ループはゾーン)、パンくずに `<feature> (decomposed)` が足されます。
  - `get_graph_templates` は、plan に feature の pass があるとき、接続ごとに 1 回だけ読みます([読む口](../../src/devstudio/view/graphcanvasreadcapability.hpp#L64))。持たない engine ではボタンが出ません。
  - それ以外の 1 pass の node は潜れません(ダブルクリックしても何も起きません)。
  - 中の絵は、Studio が組んだ文書を本物と入れ替えて描いています。engine の返事を読む間だけ本物に戻し、読んだ後で組み直します(コードのコメント)。中にいる間に文書を読み直しても、同じ中身が組めればそのまま残ります。
- **中で編集しようとしたとき**: 線を引く・値を変える・node を足す・消すなど、op を送る操作は何も送りません。canvas の上に 1 行 `This is the engine's folded implementation: expand it to edit.` と **Expand to edit…** のリンクが出ます(`pelican.graphCanvasFoldedNotice`。[`GraphCanvasModel::refuseFoldedEdit()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6936))。
  - 配置だけの列(`set_layout`)は、何も言わずに捨てます。
  - リンク(action `pelican.graphCanvas.expandToEdit`)は、今ある流れを確認のダイアログ付きで始めます([`GraphCanvasModel::foldedExpand()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6894))。rendering 節の material pass なら Frame Plan の **Decompose into loop...**(上の「material pass をループに分解する」)、雛形のある feature(とその中)なら Render Features の **Replace with loop version…**(上の「feature をループ版に置き換える」)です。どちらにも当たらなければリンクは出ません。
  - 1 行は、畳んだ中から出ると消えます。
- **領域の filter**: 上段の操作列に `Show:` と 5 つの check が並びます(`Render` / `Pose` / `Value` / `Audio` / `UI`。名前は `pelican.graphCanvasShow.<領域>`)。既定は `Render` だけです([既定を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L5381))。状態はセッションの間だけで、文書には書きません。
  - 領域は op の接頭辞で決めます。`render.*` と `draw.*` は Render、`value.*` と `event.*` は Value、`pose.*` / `audio.*` / `ui.*` はそれぞれの領域です。それ以外(`graph.instance` と `graph.loop` を含む)は常に出ます([`GraphCanvasModel::opDomain()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6953))。
  - 効くのは root の canvas だけです。definition の中と畳んだ中では全部出ます。Render を外すと、畳んだ node も出ません。
  - 隠れた node と見えている node の間の線は描きません。代わりに、見えている側のソケットに小さな口が出ます。入力側は `← value (N)`、出力側は `→ pose (N)` の形で、N は隠れたその領域の node の数です。押すとその領域が filter に足されます([`GraphCanvasModel::domainStubs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6984))。
  - Inspector や Frame Plan から、隠れた領域の node を開くと、その領域が足されます。
- **Frame Plan の Open in Graph は全 pass に出ます**: graph の node が作っていない pass では、Graph タブを前に出し、本物の root に戻ります。その pass が描かれている node(feature の pass なら feature の畳んだ node、それ以外はその pass の node)を選び、中央に置きます([`GraphCanvasWidget::revealPlanPass()`](../../src/devstudio/view/graphcanvaswidget.cpp#L10044))。material pass でも潜りません。
  - Graph タブがまだその pass を読んでいなければ、Frame Plan の status 行が `The Graph tab has not read pass <pass> yet: refresh it.` を言います([分ける所](../../src/devstudio/view/frameplanwidget.cpp#L3659))。
  - ✅GL-2b から、行き先は plan の `origin` で決めます([`GraphCanvasModel::planNodeTarget()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6363))。instance の中の node の pass なら、instance を通ってその body に降り、node を選びます。ループの回ならゾーンの中の body の node で、ゾーンのタブをその要素が走った分岐に移します。それ以外は root の node か畳んだ node です。`origin` を出さない engine では、ループの回は `loop_origin` で、他は下の名前の一致で決めます。
- **graph の pass かどうか**は 1 か所で決めます([`GraphCanvasModel::graphOwnerOf()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5834))。
  - ✅GL-2b から、engine が `origin` を出していれば(✅GL-2a。§13.5 の「`get_frame_plan` に足されたもの」)それだけで決めます。`origin` の無い pass は、名前や `source` が何であっても、preset・feature・rendering 節の pass として畳みます。`origin` のある pass は、`instance_path` があればその先頭の root の instance、`loop` があればそのループ、どちらも無ければ `node` の下に描きます。
  - engine が `origin` を出すかどうかは、plan に `diagnostics` か `deactivated_graph_nodes` の鍵があるか、どれかの node に `origin` があるかで見ます([`GraphCanvasModel::readPlanLayers()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6199))。
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

- **出る条件**: 本物の root(definition の中や畳んだ中ではない)で、`Show:` が Render だけで、plan を読めているときです([`GraphCanvasModel::layersShown()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6455))。Render のほかの領域を足すと、帯も Layers も消えます。
- **node と plan の node の対応**: root の node ごとに、その下に描かれている plan の node を plan の順で集めます([`GraphCanvasModel::planNodesOf()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6418))。graph の node が作った pass はその node(instance の中の node なら root の instance、ループの回ならループ)、そうでない pass は畳んだ node です。anchor は数えません。
- **帯**([`GraphCanvasModel::layerBand()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6465)): 既定と違うことだけを ` · ` でつないだ 1 行で、node の枠の下に薄い字で描きます(畳んだ node にも付きます。自動操作の名前は `graph_layer_band#<node>`。[描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1585)、[root の node に帯を付ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L2261))。言うことが無ければ出ません。
  - `×N copies`: node が plan の中で 2 つ以上の複製(instance の path・ループ・要素の組)になったとき。ループではゾーンの下端の行が回の数を言うので、帯には出しません(✅GL-1b から。GL-2b の着地時は出していました)。
  - `N barrier` / `N barriers`: その node の pass が端のどちらかにある barrier の数。
  - `aliased`: その node の pass が `from_nodes` に入る alias group があるとき。
  - backend の名前: pass の `execution_plan` の `selected_endpoint` が graph の backend と違うとき。graph の backend は pass の中で一番多い `selected_endpoint`、どの pass も言わなければ選ばれた候補の `endpoint` です([`GraphCanvasModel::graphBackend()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6344))。
  - `deactivated: <reason> (<feature>)`: plan の `deactivated_graph_nodes` にある root の node。この node は薄く(不透明度 0.45)描きます。
  - tooltip は `<帯> below the graph: select the node for its Layers` です。
- **Layers の節**(`pelican.graphCanvasLayersToggle` と `pelican.graphCanvasLayers`。[`syncLayers()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7924)): root の node を選ぶと見出しが `Layers: <node>`、何も選ばないと `Layers: the whole graph` です。見出しを押すと畳めます(`▾` / `▸`)。行が無ければ節ごと隠れます。definition やゾーンの中の node を選んでいる間は出ません。
  - 行は `L1 expanded` / `L2 physical` / `L3 backend` の 3 つの組に分かれます([`GraphCanvasModel::layerRows()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6546))。
  - L1: 展開した plan の node(`<名前> · <kind か material>`。ループの回なら ` · loop <loop> [<要素>]`、instance の中なら ` · instance <path>`)、`replaces` のある node の `<pass> replaces pass <pass> of <feature>`、`deactivated: <reason> (feature <feature>)`。feature の畳んだ node では、graph の node に置き換えられた pass を `pass <pass> is replaced by graph node <node>` で言います。
  - L2: その node の pass が `from_nodes` に入る resource(`resource <名前> · <format>`。alias group に入っていれば ` · alias <id>`)、alias group(`alias group <id>: <resource, …>`)、scope(`scope <id> (<kind>)`)、barrier(`<from> → <to> · <resource> · <kind>`。両端の `execution_plan` の `resource_uses` に access があれば ` · <from の access> → <to の access>`)。最上位の `barriers` は layout を持たないので、barrier の行に layout は出ません(実装者の報告)。
  - L3: `from_nodes` がその node の pass に当たる決定(`<id> · <subject> → <selected>`)と、graph の backend と違う pass の backend(`backend <endpoint> (<pass>)`)。
  - 何も選ばないときは graph 全体のもので、`from_nodes` が空の決定と、backend の候補のすべて(`candidate <名前> · <endpoint> · selected|feasible|rejected`)です([`GraphCanvasModel::graphLayerRows()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6670))。
  - plan の node を持つ行(L1 の pass・置き換えの行と L3 の backend の行)を押すと、Frame Plan が前に出て、Logical graph のタブでその node を選び、中央に置きます。その node を含むグループは開きます([選ぶ所](../../src/devstudio/view/frameplangraphics.cpp#L3786))。status 行は `<node> is picked.`、plan に無ければ `The frame plan has no node <node>: refresh it.` です。行の tooltip は `Click to pick <node> in the Frame Plan.` を足します。
- **診断**: plan の `diagnostics` が 1 つ以上あれば、canvas の上の notice の下に `▸ N diagnostics`(1 つなら `1 diagnostic`)の 1 行が出ます(`pelican.graphCanvasDiagnostics`)。押すと開いて、`<kind> · <subject>: <detail>` の一覧(`pelican.graphCanvasDiagnosticsList`)が出ます([`syncDiagnostics()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8118))。この 1 行は領域の filter によらず出ます。
- **分かっている穴**(着地時の記録。実装者の報告を含む):
  - 帯と Layers は `Show:` が Render だけのときだけです。
  - backend の候補は node に結ばず、何も選ばないときに全部出します。engine の候補の `from_nodes` は実物で全部 `[]` です(§13.5 の「`get_frame_plan` に足されたもの」)。
  - feature の pass が全部 graph の node に置き換わると、feature の畳んだ node が無いので、`is replaced by` の行も出ませんでした。✅UX-15b から、そのときは何も選ばないときの graph 全体の行(L1)に `pass <pass> of <feature> is replaced by graph node <node>` を 1 回ずつ出します([`GraphCanvasModel::graphLayerRows()` の頭で足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L6674))。feature の畳んだ node が残っていれば、今までどおりその node の行が言い、graph 全体の行には出しません。

**速くしたかが見える —— GPU 時間・plan の数・編集の前後の 1 行(✅UX-6b)**: GB-2 の `gui_build_optimize` で、速くしたつもりの編集が効いたかを ms では見分けられず、plan の数でしか見えなかったこと(仕様の背景)を受けて、Frame Plan と Graph タブに読むだけの行を足しました。

- **合計の 1 行**: Frame Plan の status 行の右(`pelican.framePlanPerf`)と、Graph タブの上段の操作列の下の行(`pelican.graphCanvasPerf`)に、`GPU 0.15 ms · 24 passes · 20 barriers · 2 alias groups` の形で出ます([`PlanPerfModel::summaryLine()`](../../src/devstudio/model/planperfmodel.cpp#L327))。
  - GPU は `get_gpu_timing` の node ごとの平均の和で、それを 1 度も読んでいない間は `get_status.gpu_timing` の窓の平均です。窓が 30 frame に満たない間は `GPU measuring…`、timing が無効なら `GPU timing off` です。数は `get_frame_plan` の `nodes` / `barriers` / `physical_target_plan.alias_groups` の数です。
  - 2 つの panel は同じ読み手 [`PlanPerfHub`](../../src/devstudio/view/planperfhub.hpp#L31) の値を出します([読み手を作る所](../../src/devstudio/view/mainwindow.cpp#L1036))。どちらかの panel が見えている間、`get_gpu_timing` を 2 秒ごとに送り、ほかの panel が送った `get_gpu_timing` と `get_frame_plan`(draw_loop の頁を除く)の返事も読みます。編集・ticket の問い・commit の後の読みのどれかが 3 秒以内にあった間は、自分からは何も送りません([`PlanPerfHub::poll()`](../../src/devstudio/view/planperfhub.cpp#L145))。返事の parse は thread pool でします。
- **node ごとの時間**: Graph の root の node の帯(上の「下の層を node で見る」の帯。出る条件も同じ)の最後に、その node の plan の node の時間の和を `0.02 ms` の形で足します([`GraphCanvasScene::layerBandText()`](../../src/devstudio/view/graphcanvaswidget.cpp#L1412))。新しい時間は、出ている帯の字だけを替え、canvas を描き直しません。Frame Plan の Logical graph では、倍率が 0.6 より小さいと node の時間の字を隠し、node の tooltip の `GPU:` の行で読みます([倍率の閾値](../../src/devstudio/view/frameplanwidget.cpp#L86))。
  - node ごとの値は、engine の ✅ST-1 の `get_status.gpu_timing.node_averages` があればその行(`barriers_ms` と、`body_supported` なら `body_ms` の和)で、無い engine では `nodes` の sample の和を `samples` で割ります([`PlanPerfModel::statusReading()`](../../src/devstudio/model/planperfmodel.cpp#L85))。
- **編集の前後の 1 行**(`pelican.framePlanPerfDelta` / `pelican.graphCanvasPerfDelta`): plan を動かしうる編集(`edit_graph_document`、`restore_render_documents`、render の編集の履歴が記録する method)が出た時の世代を「前」、その後の新しい世代の窓が 60 frame になったら「後」として、1 行を出します([`PlanPerfModel::resolve()`](../../src/devstudio/model/planperfmodel.cpp#L275))。
  - 形は `after draw_sort: passes 24→24, barriers 20→20, alias 2→2, GPU 0.15→0.14 ms · generation 3→4` です。数が全部同じなら先頭に `no change in the plan · ` が付き、GPU の差が「前」の窓の揺れ(10 と 90 percentile の間)より小さければ ` · GPU difference is within noise` が付きます。世代の初めの frame(窓が満ちていなければ最初の 20 frame、ただし 1/3 まで)は平均と揺れから外します(再起動の直後の compile のため。コードの注記)。
  - 名前は、`edit_rendering_keys` なら書いたキー(`draw_sort`)、graph の編集なら最初の op が触る node(`set_node_parameters` の鍵が 1 つなら `<node>.<鍵>`、ほかは `<node>`。`gui_build_optimize` では `loop` / `render_fullscreen__color`)、履歴が記録する編集なら履歴の項目の名前(`Replace with …`)、Undo / Redo は `Undo / Redo` です([`PlanPerfModel::editLabel()`](../../src/devstudio/model/planperfmodel.cpp#L355))。配置だけの編集(`set_layout`)は数えません。
  - 編集が「後」を待っている間だけ、`get_status` を 2 秒ごと(最長 30 秒)に読み、その世代の plan をどの panel も読んでいなければ `get_frame_plan` を 1 回送ります。
- **使われない出力と binding**: plan の `diagnostics` の `unread_output` と `unused_binding`(engine の ✅PO-1。§13.5 の「計画を詰める手がかり」)を、その pass を作った root の node の帯に `output unread` / `binding N unused` で出し、node を選ぶと右の欄に 1 文ずつ出します(`pelican.graphCanvasPlanNotes`。`<resource> is written but nothing reads it this frame — the pass may be removable` / `binding N (<resource>) is declared but the shader never reads it`。[`GraphCanvasModel::planNotes()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6515))。ループの回の資源は `<loop>/<要素>/` を外した名前で 1 回だけ言います。この 2 種は canvas の上の `▸ N diagnostics` の一覧には入りません。
- Graph タブの **Add** のボタンは、Add loop と同じく毎 tick 編集できるかに従います(commit の記録)。
- test: `devstudio_plan_perf_test` の [「UX-6b decision 1 and 2: the status line says the frame's GPU time and the plan's size, and each node its own time」](../../test/devstudio_plan_perf_test.cpp#L67)、[「UX-6b decision 3: an edit's before and after, read once its generation has 60 frames」](../../test/devstudio_plan_perf_test.cpp#L134)、[「UX-6b decision 2 (engine ST-1): get_status's node_averages are each node's time, in place of summing every sample of the window」](../../test/devstudio_plan_perf_test.cpp#L211)、`devstudio_graphcanvas_test` の [「UX-6b unread outputs and unused bindings are said on the graph node that made the pass」](../../test/devstudio_graphcanvas_test.cpp#L3306)、`devstudio_graphcanvas_scene_test` の [「UX-6b a root node's band carries its unread output and its GPU time」](../../test/devstudio_graphcanvas_scene_test.cpp#L540)。GPU シナリオは `gui_build_optimize`(下の「Studio の自動化面」)です。

**Frame Plan の表の行から開く(✅GL-2b)**: Frame Plan の Passes / Resources / Physical plan / Decisions / Backends / Barriers のタブで行を右クリックすると、**Open in Graph** が出ます([右クリックの所](../../src/devstudio/view/frameplanwidget.cpp#L3253))。行き先は上の「Frame Plan の Open in Graph は全 pass に出ます」と同じです。

- **行が名指す pass**: Passes はその pass、Resources は `from_nodes`(無い古い plan では書き手と読み手)、Physical plan は alias group の `from_nodes`・候補の組(pass の組か、resource の組ならその `from_nodes`)・scope の `nodes`・lowering の node の元・依存の両端、Decisions は `from_nodes`(subject の見出しの行は、その下の決定の和)、Backends は候補の `from_nodes` と拒否の `subject`、Barriers は両端です。plan に在る pass だけを残し(anchor は外します)、行が何も持たなければ親の行のものを使います。
- 名指す pass が 1 つなら項目 1 つ、2 つ以上なら **Open in Graph** の下に pass の名前の submenu です。1 つも無ければ押せない項目になり、tooltip は `This row names no pass: it is about the whole graph.` です。
- 名前の付いた action `pelican.framePlan.openRowInGraph` は、今のタブで選んだ行の最初の pass を開きます([action](../../src/devstudio/view/frameplanwidget.cpp#L1391))。行が pass を名指さなければ、status 行が `Select a row that names a pass first.` を言います。
- 各タブにも action `pelican.framePlan.tab.<タブの id>`(Barriers なら `pelican.framePlan.tab.barriers`)が付き、発火するとそのタブを前に出します。表にも名前が付きます(`pelican.framePlanPasses` / `pelican.framePlanResources` / `pelican.framePlanPhysical` / `pelican.framePlanDecisions` / `pelican.framePlanBackends` / `pelican.framePlanBarriers` / `pelican.framePlanMaterials`)。
- シナリオ `graph_layers_origin` は Barriers の 20 行目と 9 行目を開きます。この番号は preset の pass の並びに依ります(実装者の報告)。

**root の node を fold group にまとめる(✅GL-3a)**: Graph タブの root で node を選び、canvas の右クリックの **Fold into group…**(`pelican.graphCanvas.foldIntoGroup`)を押すと、選んだ node を 1 つの fold group として root の文書の `fold_groups` に書きます。送るのは `fold_nodes` 1 回で、決定は空(`{kind: "decisions", decisions: {}}`)です([`GraphCanvasFoldController::foldSelection()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L708))。engine の契約は §13.5 の「断片の決定を宣言する(✅FG-1)」、設計は [design_graph_layers](../design_graph_layers.md) §3.4〜§3.6 と §7 の GL-3 です。

- **名前は Studio が付け、問いません**: 選んだ node にループがあれば `<ループ>_fold`(使われていれば `_2`…)、無ければ `fold_1`、`fold_2`… の空き名です([`suggestGraphFoldGroupName()`](../../src/devstudio/model/graphfoldgroups.cpp#L165))。`nodes` は選んだ順ではなく文書の順です。notice は `Folded <node>, … into <group> (not verified: it runs as it is).` です。
- **選べない node**: 項目は本物の root(definition の中や畳んだ中ではない)で、engine の接続が `fold_nodes` を送れるときだけ出ます。ループのゾーンの中の node か、畳んだ node(feature や rendering 節の pass)を選んでいると項目は押せず、その下に理由が 1 行出ます(`<node> is inside a loop's zone (<definition>): fold the loop itself.` / `<node> is a folded feature or a pass of the rendering section, not a node of the graph.`。何も選んでいなければ `Pick the root nodes to fold first.`。[`graphFoldPickRefusal()`](../../src/devstudio/model/graphfoldgroups.cpp#L242))。
- **見え方**([`GraphCanvasFoldController::redraw()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L1033)): group の node を薄い色の角丸の枠(`graph_fold_group#<group>`)で囲み、上辺に見出しの帯(`graph_fold_heading#<group>`)を描きます。帯には名前、状態の字(`graph_fold_state#<group>`)、✅GL-4a の **Verify**、右端に **Fold** / **Open**(`graph_fold_toggle#<group>`)が並びます。node を動かすと枠も追います。
- **畳む・開くは見え方だけです**: **Fold** で group を 1 つの畳んだ node(`graph_node#fold:<group>`。型の所は `fold group`)にし、枠をまたぐ線ごとに `<node>.<port>` の行(`graph_derived_socket#fold:<group>/<node>.<port>`)を出して、外の node から行へ破線を引きます。**Open** か、畳んだ node のダブルクリックで戻ります。開閉はセッションの間の状態で、文書の `fold_groups` は変わりません。
- **状態の字**: Graph タブは文書を読むたびに、digest が変わっていれば `get_fold_groups` を読み、group があれば `get_render_documents` で root だけを読んで決定を重ねます(`get_fold_groups` は決定を返さないため。[`GraphCanvasFoldController::follow()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L468))。
  - `unverified`: 灰色の `not verified — decisions are not applied yet`
  - `stale`: 橙の印 1 つと `changed since verified — runs the open form`。tooltip に verified digest と current digest
  - `verified`: 字も印も出しません(設計 §4 の規則 5)
  - plan の diagnostics の `fold_unverified` / `fold_stale` はこの見出しで言うので、canvas の上の `▸ N diagnostics` の一覧からは外します([fold の診断と node の注を一覧から外す所](../../src/devstudio/view/graphcanvaswidget.cpp#L8125))。
- **group を選ぶ**: 見出しの帯か畳んだ node を押すと、canvas の選択を外して group を選び、枠が太くなります。右の欄(pass の欄の場所)に Decisions の節(`pelican.graphCanvasFold`)が出ます。canvas の node を選ぶと group の選択は外れます。
- **Decisions の節**([`GraphCanvasFoldFields::rebuild()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L168)): 見出し `Decisions · <group>`、✅GL-4a の **Verify** と **Quick check**、1 行の注(`pelican.graphCanvasFold.note`。`Decisions are requests: the engine applies them once the group is verified (Verify), and leaves them out while its nodes differ from what was verified.`)、状態の 1 行(`pelican.graphCanvasFold.state`。verified なら `verified: the engine applies them`)。その下に **Serial** / **Isolate** / **No alias** の一覧が並びます。
  - 一覧の行は主語を `node`、`node [要素]`、`node.port` の形で出し(`pelican.graphCanvasFold.<field>.row<i>`)、×(`.remove<i>`)で消します。
  - 一覧の下の Add の行は選ぶだけで、打つ欄はありません([`graphFoldNodeChoices()`](../../src/devstudio/model/graphfoldgroups.cpp#L199)): node の combo(`pelican.graphCanvasFold.<field>.node`)、node がループなら要素の combo(`.element`。`All elements` と 0…回の数−1。回の数は文書の Count か engine の報告)、No alias で書き手の node を選んだときは port の combo(`.port`)、**Add**(`.add`)。
  - Serial / Isolate で選べる node は group の全部です。No alias では `render.image` / `render.buffer` / `graph.loop` / `graph.instance` と、`render.*` の node のうち `render.image` / `render.buffer` / `render.target_ref` からつながる入力を持つもの(port を選ぶ)です。選べる node が無ければ Add は押せません。
  - 足す・消すたびに、同じ `nodes` と新しい `folded` で `fold_nodes` を 1 回送ります(notice は `<group>: decisions saved (not verified — decisions are not applied yet).`)。engine は書き直した group の印を消すので(§13.5 の FG-1 の節)、検査済みの group の決定を変えると `unverified` に戻ります。
- **見出しの右クリック**([`GraphCanvasFoldController::groupMenu()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L1404)): **Add selected nodes**(`pelican.graphCanvas.foldAddSelected`。canvas で選んでいる root の node を足す)、**Remove node…**(node の submenu。外した node についての決定も一緒に消します。node が 1 つの group では押せません)、**Fold / Open**(`pelican.graphCanvas.toggleFoldGroup`)、**Unfold (delete group)**(`pelican.graphCanvas.unfoldGroup`)。Unfold は確認(`pelican.graphCanvasFold.unfoldConfirm`。既定のボタンは Cancel)の後に `folded: null` を送り、group と決定を消します。node は残ります。
- **送り方**: fold の編集は同時に 1 つで、送信中は `Wait for the current change to finish.` を言います。CAS で `external_modification` が返れば、文書を読み直してから同じ操作を 1 回だけ送り直します([`GraphCanvasFoldController::refused()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L686))。commit の後は Graph タブのほかの render の編集と同じく、digest をほかの panel に知らせ、`restart_required` なら起動し直し、そうでなければ Frame Plan に plan を読み直させて文書を読み直します([`foldEditCommitted()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7408))。
- **効いた決定の見え方**: engine は検査済みの group の決定だけを効かせ、plan の `fold_decisions` に出します。Graph タブは、決定が当たった node の帯(上の「下の層を node で見る(✅GL-2b)」)に `isolate (fold <group>)` の形で足します([`GraphCanvasModel::foldDecisionParts()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6323))。node の決定はその node が作った pass に、資源の決定はその資源を出力として渡す node に付けます。Frame Plan の Decisions のタブには 5 列目 **From** が増え、`from_fold` のある決定に `fold <group>` を出します([Decisions の From の列](../../src/devstudio/view/frameplanwidget.cpp#L4673))。
- **拒否の文**(inspectormodel の 1 文の表。[fold の拒否の 1 文の表](../../src/devstudio/model/inspectormodel.cpp#L1421)): `graph.fold_decision_conflict@1` は `<group> asks <field> for <name>, but the graph-wide setting says otherwise`、`graph.fold_subject_outside@1` は `Fold group <group> names <subject>, which is not one of its nodes`(subject は Decisions の行と同じ `node [要素].port` の形)、`graph.fold_kind_unsupported@1` は `Fold group <group> is folded as <kind>; only decisions can be declared for now` です。
- **戻す**: `fold_nodes` は render の編集の履歴(下の「render の編集を戻す」)に `Fold group <group>` / `Unfold <group>` の名前で積まれます。fold の編集を履歴から戻す test はありません(未確認)。
- test: `devstudio_graph_fold_test`(Qt を使わない)の [「GL-3a decision 1: Fold into group takes root nodes, proposes a free name and sends a group with no decisions」](../../test/devstudio_graph_fold_test.cpp#L44)・[「GL-3a decision 3: get_fold_groups' state reads as the heading's words, and the root's decisions are laid over it」](../../test/devstudio_graph_fold_test.cpp#L79)・[「GL-3a decision 4: the Add row offers the group's nodes, a loop's elements and a writer's ports, nothing typed」](../../test/devstudio_graph_fold_test.cpp#L127)・[「GL-3a decision 6: a decision a fold group made is marked on the Frame Plan's row and on the node's band」](../../test/devstudio_graph_fold_test.cpp#L162)・[「GL-3a decision 7: the fold refusals read as sentences」](../../test/devstudio_graph_fold_test.cpp#L242)と、`devstudio_graph_fold_scene_test` の [「GL-3a the Graph tab folds picked root nodes into a group, says it is not verified, writes its decisions, folds it as it is looked at, and unfolds it」](../../test/devstudio_graph_fold_scene_test.cpp#L170)。GPU シナリオは `graph_fold_decisions`(下の「Studio の自動化面」)です。

**fold group を検査する —— Verify(✅GL-4a)**: 宣言した group の決定は、engine の `verify_fold` が印を書くまで効きません(§13.5 の「断片を検査して印を付ける —— `verify_fold`(✅FG-2)」)。Graph タブは見出しと Decisions の節から `verify_fold` を送ります([`GraphCanvasFoldController::verify()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L906))。

- **押す所**: 見出しの **Verify**(`graph_fold_verify#<group>`)と、Decisions の節の **Verify**(`pelican.graphCanvasFold.verify`)です。印のある(`verified`)group と、`verify_fold` を送れない接続では出ません(節のボタンは押せません)。`stale` の group では **Verify again** になり、見出しのボタンの縁が橙で、tooltip が `… (a loop's Count is part of what is compared) …` を言います。
- **Quick check**(`pelican.graphCanvasFold.quickCheck`): 外していれば `frames` と `warmup_seconds` を送らず、engine の既定(3 frame、30 s)です。付けると `frames: 2`・`warmup_seconds: 1` を送ります。`tolerance` は送りません(0 = byte の一致)([`graphFoldVerifyParams()`](../../src/devstudio/model/graphfoldgroups.cpp#L255))。tooltip は、NVIDIA の driver が最適化した shader に差し替える前に比べるので、まだ動く絵は unstable と読まれうることを言います。見出しの Verify も節の Quick check を使います。
- **走っている間**: 見出しの字が `verifying… (n s)` で 1 秒ごとに数え、Verify は出ません(節のボタンもその字で押せません)。`get_edit_result` を 500 ms ごとに問います。同時に検査できるのは 1 つで、別の group の Verify は `<検査中の group> is being verified; verify <group> after it.` で断ります。検査中の group への fold の編集は送らず、notice が `<group> is being verified; edits wait until it finishes` を言います。その group の node を選ぶと、右の欄(Loop・Draw・View・image・pass・Place の欄とラベル)が灰色になり、同じ文が選ぶたびに 1 回出ます([`GraphCanvasFoldController::gateNodeFields()`](../../src/devstudio/view/graphcanvasfoldgroups.cpp#L1011))。ほかの group とほかの node は編集できます。
- **答え**([`readGraphFoldVerifyOutcome()`](../../src/devstudio/model/graphfoldgroups.cpp#L278)):
  - **通った**(committed): notice が `<group> verified — its decisions now apply` を言い、fold の commit と同じく plan と文書を読み直します。見出しの字と印が消え、Decisions の各行の下に `applied → <展開名>`(`.applied<i>`)が出ます。当たる展開名が無い主語(Count を減らして無くなった回など)は灰色の `no longer matches anything` です([`graphFoldAppliedNames()`](../../src/devstudio/model/graphfoldgroups.cpp#L367))。
  - **`graph.fold_mismatch@1`**: 見出しに赤の印と 1 文(`frame F differs in N pixels` / `N validation errors (open form: M)` / `the decided form exited with E`)。notice は `<group> is not verified: <文> — its decisions still do not apply.` です。
  - **`graph.fold_verify_unstable@1`**: 見出しに黄の印と `the open form itself is not stable (frame F, N pixels) — not the decisions' fault`。notice は Quick check を外して(warm-up を長くして)もう一度、を言います。
  - 赤と黄の印は、その group の digest が検査したときのままで、`verified` でない間だけ残ります。
  - `external_modification`: `<group>: the graph changed while verifying; verify again.`。`fold_verify_failed` / `fold_verify_unavailable` / `fold_verify_busy` は inspectormodel の 1 文の表の文(`<group> could not be verified: its open form could not be drawn (<run> run)` / `This engine cannot verify a fold group now (<reason>)` / `<group> is being verified already; wait for it to finish`)です。どれも印は付けません。
- **戻す**: `verify_fold` も render の編集の履歴に `Verify <group>` で積まれる method の表に入りました(`running` の間は 500 ms ごとに問います)。履歴から戻す test はありません(未確認)。
- 子 process が描くのは project の既定の scene で、Studio が開いている scene ではありません(§13.5 の FG-2 の「残した穴」)。Quick check と既定のそれぞれで何秒かかるかは、この文書では確かめていません(未確認)。
- test: `devstudio_graph_fold_test` の [「GL-4a decision 1: Verify sends the engine's defaults, or 2 frames after 1 s with Quick check, never a tolerance」](../../test/devstudio_graph_fold_test.cpp#L296)・[「GL-4a decision 3: what a finished verify_fold says, read from get_edit_result」](../../test/devstudio_graph_fold_test.cpp#L307)・[「GL-4a decision 5: a verified decision names the expanded names it landed on, or nothing once its subject matches nothing」](../../test/devstudio_graph_fold_test.cpp#L372)と、`devstudio_graph_fold_scene_test` の [「GL-4a Verify from a group's heading runs verify_fold, holds the group's edits while it runs, and marks what it found; a stale group offers Verify again」](../../test/devstudio_graph_fold_scene_test.cpp#L304)。mismatch と unstable は手で作った `get_edit_result` の JSON で見ています。

**Frame Plan から graph 全体の決定を書く(✅GL-3a)**: Frame Plan の **Passes** と **Resources** のタブで行を右クリックすると、Open in Graph の上に、pass の行なら **Serial** / **Isolate**、資源の行なら **No alias** の check が出ます(`pelican.framePlan.planning.serial` / `.isolate` / `.noAlias`。[`addPlanningEntries()`](../../src/devstudio/view/frameplanwidget.cpp#L3292))。engine の契約は §13.5 の「graph 全体の計画の決定を書く(✅EL-2)」です。

- check の状態は root の rendering 節の `target_planning.graphs.<plan の graph>` です。Frame Plan は authoring context を読むたびに、`get_render_documents` で root を読みます([`requestPlanning()`](../../src/devstudio/view/frameplanwidget.cpp#L3443))。
- 押すと `edit_target_planning` を 1 回送ります。付けるときはその field を `true`、外すときは `null`(field を消す。`false` は書きません)です([`togglePlanning()`](../../src/devstudio/view/frameplanwidget.cpp#L3319))。status 行は `Isolate set on <名前>.` / `Isolate taken off <名前>.` の形です。
- ループの回の展開名(plan の `loop_origin` を持つ行)には check を出さず、押せない 1 行 `Fold the loop to set this per element` を出します(`pelican.framePlan.planning.loopLine`。engine の LP-1a が展開名を断るため。回ごとの決定は上の fold group で書きます)。authoring context をまだ読んでいない間と、Frame Plan の編集が送信中の間は押せません。
- preset が `target_planning` を持つ root での `rendering_key_not_allowed_here` は、`The pipeline preset declares target_planning, which this project cannot override: fork the preset to set it (rendering_key_not_allowed_here).` の 1 文です([preset が target_planning を持つときの文](../../src/devstudio/view/frameplanwidget.cpp#L3038))。
- Physical plan と Decisions のタブの行には出ません。`vulkan_plan_pins` と `vulkan_physical_fragments` は、✅UX-15b から Backends と Physical plan のタブで書きます(次の段落)。
- 戻す: render の編集の履歴に `Edit planning decisions` の名前で積まれます(戻す道の test は無く、未確認)。
- test: `devstudio_graph_fold_test` の [「GL-3a decision 8: the Frame Plan's Serial / Isolate / No alias read the root's target_planning and write edit_target_planning」](../../test/devstudio_graph_fold_test.cpp#L266)(読みと params の形。Frame Plan の右クリックは GPU シナリオ `graph_fold_decisions` の最後で通します)。

**下の層の決定を Frame Plan から書く —— backend の Pin / Unpin、物理の計画の Freeze / Remove / Eject again、常駐 BLAS、置き換えの来歴(✅UX-15b、`5c5110ef`)**: engine の ✅EL-2 の `edit_vulkan_plan_pins` と `put_vulkan_physical_fragment`(§13.5 の「graph 全体の計画の決定を書く(✅EL-2)」)を、Frame Plan の Backends と Physical plan のタブから送ります。UX-15b の前は、`ejectable_pin_package` / `ejectable_physical_fragment` を写して root の文書に貼る(第6章 §6.6)か、RPC を手で送るしかありませんでした。Raw JSON のタブは読むだけのままです。

- **Backends の Pin this candidate**: Selected か Feasible の候補の行の右クリックに **Pin this candidate**(`pelican.framePlan.backend.pinEntry`)が出ます([右クリックに足す所 `addLowerLayerEntries()`](../../src/devstudio/view/frameplanwidget.cpp#L3400))。plan の `ejectable_pin_package` の `pins.backend_candidate` だけをその候補に替えて、`edit_vulkan_plan_pins {base_source_digest, family, pin}` を 1 回送ります([`framePlanBackendPinRequest()`](../../src/devstudio/view/frameplanedit.cpp#L2191))。status 行は `Pinned <candidate>.` です。
  - 送れないときは項目が押せず、tooltip が理由を言います: `<candidate> was rejected by the planner: only a Selected or Feasible candidate can be pinned.`、`<candidate> is pinned already.`、`The plan ejects no pin package to pin a candidate with.`、authoring context の前は `The render authoring context has not arrived yet.`。送信中も押せません。
  - 名前の付いた action `pelican.framePlan.backend.pin` は Backends のタブで選んだ行に効きます(行を選んでいなければ status 行が `Pick a candidate row on the Backends tab first.`)。action はいつも押せる状態で、送れない理由は status 行に出ます(実装者の報告: 共有の action を無効にすると、名前での起動が黙って効かなくなるため。右クリックの項目は別の一時的な action です)。
  - 候補の行の tooltip は `Right-click: Pin this candidate, so the planner keeps it.` です。pin された候補の行の Status の列は `Selected, pinned`(Feasible の候補なら `Feasible, pinned`)になります。
- **Backends の帯**(`pelican.framePlan.backend.state`。木の先頭の、列をまたぐ行です。[`addBackendBand()`](../../src/devstudio/view/frameplanwidget.cpp#L4735)): plan に `applied_pin_package` があれば `Pinned: <candidate>` と **Unpin**(`pelican.framePlan.backend.unpinButton`。action は `pelican.framePlan.backend.unpin`)、無ければ `Not pinned: the planner picks the candidate. Right-click a Selected or Feasible row to pin it.` です。Unpin は `pin: null` に plan の `graph` を付けて送り、status 行は `Unpinned <candidate>: the planner picks the candidate again.` です。pin がある間は候補の行の右クリックにも Unpin が出ます。
- **Physical plan の帯**(`pelican.framePlan.physical.state`。[`addPhysicalBand()`](../../src/devstudio/view/frameplanwidget.cpp#L5066)): 凍結していなければ `Not frozen: the planner decides the physical plan each compile.` と **Freeze this physical plan**(`pelican.framePlan.physical.freezeButton`。action は `pelican.framePlan.physical.freeze`)、plan に `applied_physical_fragment` があれば `Frozen: every compile keeps the physical plan the document holds.` と **Remove**(`pelican.framePlan.physical.removeButton`。action は `pelican.framePlan.physical.remove`)です。
  - Freeze は plan の `ejectable_physical_fragment` を手を加えずに `put_vulkan_physical_fragment {base_source_digest, family, fragment}` で送ります([`framePlanPhysicalFreezeRequest()`](../../src/devstudio/view/frameplanedit.cpp#L2225))。status 行は `Physical plan frozen.`(既に凍結していれば `Physical plan frozen again from the current plan.`)です。Remove は `fragment: null` に `graph` を付け、`The frozen physical plan was removed.` です。
  - 帯を表の上の別の widget にせず木の先頭の行に置いたのは、タブの page が木でなくなると、`currentWidget()` を木として読む既存の操作(Open in Graph など)が効かなくなるためです(実装者の報告)。帯は pin や凍結が無いときも案内として出ます。
- **`physical_fragment_stale` と Eject again**: commit の組み直しで fragment の fingerprint が合わないと(§13.5 の EL-2 の段落)、status 行が `The frozen plan no longer matches the graph (<fingerprint>); Eject again (physical_fragment_stale).` を言います(`<fingerprint>` は `logical_graph` か `automatic_plan`。文は inspectormodel の拒否の 1 文の表に足しました。[`physical_fragment_stale` の 1 文](../../src/devstudio/model/inspectormodel.cpp#L1466))。status 行の横に **Eject again**(`pelican.framePlan.physical.ejectAgainButton`。action は `pelican.framePlan.physical.ejectAgain`)が出て、押すと画面の plan の `ejectable_physical_fragment` で置き換える 1 回の編集を送ります(Freeze と同じ要求です)。ボタンは次の送信か、接続する engine が替わったときに隠れます。
  - status 行は 90 文字で切るので、この 1 文(約 100 文字)は末尾が切れます。全文は tooltip と Copy message にあります(実装者の報告)。
- **family**: `get_frame_plan` は plan の family を言わないので、graph の名前が `#xr` で終われば `xr`、それ以外は `flat` と推定して送ります([`framePlanPackageFamily()`](../../src/devstudio/view/frameplanedit.cpp#L2153))。`preview` は送りません。
- **送り口と履歴**: Frame Plan の編集の送り口(`FramePlanEditCapability`)に `editVulkanPlanPins` / `putVulkanPhysicalFragment` の 2 本を足しました([足した所](../../src/devstudio/view/frameplaneditcapability.cpp#L52))。どちらも render の編集の履歴に積まれ、Undo 1 回で戻ります。項目の名前は `Pin backend <candidate>` / `Unpin backend` / `Freeze physical plan` / `Remove frozen physical plan` です([pin と凍結の項目の名前](../../src/devstudio/model/renderedithistory.cpp#L339))。
- **常駐 BLAS**(読むだけ): Physical plan のタブの最後に **Ray query acceleration structures** の節を出します([`populateRayQuery()`](../../src/devstudio/view/frameplanwidget.cpp#L5083))。見出しの行は `Requested` / `Not requested` と `<N> resident BLAS, <M> TLAS instances`、子は Active BLAS・BLAS variants・Named TLAS(名前ごとの instance の数)・Resident BLAS(`resident_blas_keys` の 1 つずつ: geometry kind、`allocation <id>`、`<n> geometries, flags 0x<flags>`)です。`get_frame_plan` の `ray_query_acceleration_structures` が `requested: false` で BLAS も無い project(ray tracing の無い project。Blank もそう)では節を出しません。
  - engine の ray-query の scene は 1 つで、BLAS は全部の TLAS が共有します。TLAS ごとの BLAS の一覧は wire に無いので、named TLAS ごとには instance の数だけを出します(実装者の報告)。
- **置き換えの来歴**(読むだけ。model は `physical_target_plan` の `subgraph_replacements` / `graph_transforms` / `render_strategy` をこの順に読みます。[`readPlanPackages()`](../../src/devstudio/model/frameplanmodel.cpp#L634)):
  - subgraph の置き換えが作った pass は、Passes の行(と Logical graph の詳細)の Plan facts に **Replaced by** の行が付き、行の tooltip が `Replaced by subgraph replacement <region> by <provider> (<implementation>)` を言います([`framePlanReplacedByLines()`](../../src/devstudio/model/frameplanmodel.cpp#L2237))。
  - Physical plan のタブの **Compile rewrites** の節([節を作る所](../../src/devstudio/view/frameplanwidget.cpp#L4880))に 3 種の全部を 1 行ずつ出します(名前、`Subgraph replacement` / `Graph transform` / `Render strategy`、provider、node のある置き換えは `<source の node> -> <置き換えの node>`、ほかは `render strategy <name> by <provider> (<implementation>), graph variant <variant>` の形)。
  - `graph_transforms` と `render_strategy` は node の一覧を持たない(graph 全体を書き換える)ので、pass ごとの来歴は subgraph の置き換えの分だけです。
- 帯と Layers の `is replaced by` の穴(上の「下の層を node で見る(✅GL-2b)」の分かっている穴)も UX-15b で直しました。
- **残した穴**(実装者の報告):
  - Eject again が効くのは、文書の fragment が画面の plan より古い場合(古い plan から Freeze したなど)です。stale の拒否が別の編集(graph の変更など)の commit で起きたときは、画面の plan の ejectable も変更前の graph のものなので、置き換えても同じ stale になりえます。そのときは Remove → 編集 → Freeze です。
  - stale の流れは unit test だけで、GPU シナリオでは fingerprint をずらせないので通していません。Frame Plan 以外のパネル(Graph など)が同じ拒否を受けたときは、1 文は出ますが Eject again のボタンは出ません。
  - `xr` の family で送る道は、推定の関数の test だけです。
  - シナリオは候補を名前 `pelican.vulkan.materialized_plan@1` で選びます(この候補が feasible でない device では選べません)。
  - engine に回したもの(`<作業ディレクトリ>/engine_backlog.md` に記録): `get_frame_plan` に family を明示する、文書の凍結した fragment が今の graph と合わないことを `get_frame_plan` の警告にも出す(編集と取り直しを 1 回で)、`graph_transforms` / `render_strategy` が作った node の一覧、named TLAS ごとの BLAS の対応。
- test: `devstudio_frameplan_graph_test` の [「UX-15b decision 1: Pin this candidate sends the ejected pin with that candidate; a pin the plan applies is a band with Unpin」](../../test/devstudio_frameplan_graph_test.cpp#L12612)・[「UX-15b decision 2: Freeze sends the ejected fragment as it is; Frozen offers Remove; a stale refusal is one sentence beside Eject again」](../../test/devstudio_frameplan_graph_test.cpp#L12686)・[「UX-15b decisions 4 and 5: the resident BLAS only for a ray-query scene, and the rewrite that put a pass in the plan」](../../test/devstudio_frameplan_graph_test.cpp#L12761)、`devstudio_render_history_test` の [「UX-15b a backend pin and a frozen physical plan are recorded, named, and undone」](../../test/devstudio_render_history_test.cpp#L868)、`devstudio_graphcanvas_test` の [「UX-15b a feature every pass of which a graph node replaced says so in the graph's own Layers rows」](../../test/devstudio_graphcanvas_test.cpp#L3269)。GPU シナリオは `gui_lower_layer_pin_freeze`(下の「Studio の自動化面」)です。

**engine の再起動が要る graph の編集(✅GL-1b)**: engine の RS-1(`247ed20e`)は、生きている material の物理の描画の契約を変える graph の編集を断らずに書き、commit の結果に `restart_required: true` と `restart_reason` を付けます。`restart_reason` は `code` と、`detail`(`color_formats`・`depth_format` などの一覧)などを持ちます。

- Studio はこの commit の後に文書も plan も読まず、notice に `Restarting the engine to apply this change (<code>)...` を出して、Frame Plan の再起動と同じ [`MainWindow::requestEngineRestart()`](../../src/devstudio/view/mainwindow.cpp#L2341) に渡します([restart_required を受ける所](../../src/devstudio/model/graphcanvasmodel.cpp#L1399))。未保存の scene の編集があれば、Frame Plan のときと同じ確認(✅G-5)が出ます。✅RSX から、読み方は全パネル共通の 1 つです(次の段落)。
- 確認で Cancel したとき(か、別の問いの最中で見送ったとき)は、notice が `The engine was not restarted. The setting is saved and applies the next time the engine starts.` を言い、走っている engine から文書と plan を読み直します([`GraphCanvasModel::engineRestartDeclined()`](../../src/devstudio/model/graphcanvasmodel.cpp#L779))。
- 読むのは `restart_reason.code` だけです。GL-1b の着地時は `detail` を文字列として読んでいたので、engine が送る一覧で `type must be string, but is array` の例外になり、編集が断られたように出て engine は再起動しませんでした(`43317a76` で直しました。統合側が Release を仮想モニタで操作して見つけました)。
- 再起動の後、文書が同じなら Graph タブの Undo はそのまま効きます(✅OT-1。§13.5 の「シーン編集との違い」)。
- RS-1 より前の engine は、この編集を `changes the physical rendering contract of live material` を含む文で断ります。Studio はその文の後に ` — this change needs an engine restart, which the engine does not offer for graph edits yet` を足し([句を足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1366))、notice の 1 行を切り詰めるときもこの句は残します。

**どのパネルの render の編集も、再起動が要れば同じ形で起動し直します(✅RSX)**: engine は RS-1 から、runtime に届く render config の ticket の完了すべてに、live に載せられないとき `restart_required: true` と `restart_reason` を付けます。RSX の前に Studio がこれを見ていたのは、Graph タブ・Frame Plan・render の編集の履歴(Undo / Redo)の 3 か所だけでした。Render Features は再起動が要る編集の後でも `Applied without restarting (runtime generation N).` を言っていました(engine session の調査)。

- **読むのは 1 か所です**([`readRenderEditCompletion()`](../../src/devstudio/model/rendereditcompletion.cpp#L15)、[`completeRenderEdit()`](../../src/devstudio/model/rendereditcompletion.cpp#L49))。ticket の最後の答え(`get_edit_result` か、送った時点で committed と答えた応答)から、`status`、committed か、`no_change`、`restart_required`、`restart_reason.code`、`published_generation`、`source_digest.hex` を読みます。`restart_reason.detail` は読みません(GL-1b の `43317a76` と同じ理由)。
- `restart_required` が真なら、パネルは文書も plan も読まずに、notice に `Restarting the engine to apply this change (<code>)...`(code が無ければ括弧ごと無し。[`renderEditRestartLine()`](../../src/devstudio/model/rendereditcompletion.cpp#L5))を出し、Graph タブ・Frame Plan と同じ [`MainWindow::requestEngineRestart()`](../../src/devstudio/view/mainwindow.cpp#L2341) に渡します。未保存の scene の編集があれば同じ確認(✅G-5)が出ます。
- この読みを通るのは、Graph タブ(graph の編集と、上の「Graph から画面に出す」の pass の入力の編集。[pass の入力の編集の所](../../src/devstudio/view/graphcanvaswidget.cpp#L6654))、Frame Plan、Pass Editor([受ける所](../../src/devstudio/view/fullscreenpasswidget.cpp#L3108))、Render Features とそこから開く dialog(Frame node order、Task Editor、Fork / Regions、Callable records、Scene AS。[共通の受け口](../../src/devstudio/view/renderfeatureswidget.cpp#L116))、Inspector のグラフ装着([受ける所](../../src/devstudio/model/graphattachmentmodel.cpp#L239))、下の Rendering settings タブ、render の編集の履歴の Undo / Redo と戻し([`MainWindow::renderDocumentsRestored()`](../../src/devstudio/view/mainwindow.cpp#L1473))です。各パネルの restart の action は MainWindow が渡します([restart の action を渡す所](../../src/devstudio/view/mainwindow.cpp#L743))。
- Render Features の `Applied without restarting (runtime generation N).` は、再起動しないときだけ出ます([再起動しない Apply の 1 文を出す所](../../src/devstudio/view/renderfeatureswidget.cpp#L4196))。
- 再起動が要るかを決めるのは engine です。material の物理の描画の契約が変わる編集は、その場で組み直さず、今までどおり engine を起動し直します(2026-10-01 の利用者の決定)。
- test: `devstudio_rendering_settings_test` の [「RSX decision 1: a render edit that commits with restart_required restarts the engine through the one reading, and Render features does not say it applied live」](../../test/devstudio_rendering_settings_test.cpp#L253)。

**Graph から画面に出す(✅GL-1e)**: Graph タブの root で、rendering 節の fullscreen pass(Blank pipeline の `present` など)は入力のソケットを出し、graph の node の出力の線を受けます。落とすと、Frame Plan のソケットへの drop と同じ `edit_preset_pass` を送ります(設計は [design_graph_layers](../design_graph_layers.md) §7 の GL-1e)。これで、Blank の project に palette で足した material pass の絵を、Graph タブだけで画面に出せます。

- **線を受ける pass**([`GraphCanvasScene::passInputSockets()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4322)): 次を全部満たす pass です。
  - 本物の root を見ている(definition の中や畳んだ中ではない)。
  - plan の `source` が `project` で、feature の pass ではなく、その pass 自身の畳んだ node で描かれている(graph の node が作った pass ではない)。
  - `get_render_authoring_context` の行が `editable_root_pass`(root が自分で宣言した pass。上の「direct root の pass(✅F-6)」)で、宣言の `type` が `fullscreen`。
  - preset と feature の pass、fullscreen でない pass は今までどおり読むだけです(preset と feature の pass は fork が要るため。設計の決定)。
- **読むもの**: Graph タブは plan を読むたびに、rendering 節の pass(`source` が `project`、feature の外、anchor でない、graph の node が作っていない pass)があれば、`get_render_authoring_context` を 1 回読みます([`GraphCanvasModel::requestRenderAuthoring()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6058))。これを持たない engine では、pass は線を受けません(何も言いません)。
- **ソケット**: その pass の畳んだ node は、読む資源の行の代わりに、`input` の項目ごとのソケットを出します([描く所](../../src/devstudio/view/graphcanvasdrawing.cpp#L1448))。並びと字は、Frame Plan の「ソケットは shader の宣言から出ます —— 番号でつなぐ(✅VI-3a)」と同じ関数で組みます。shader が入力を足せる(可変の組がある)ときは、最後に空いた口 `+ input` が出ます。`engine://passthrough` の `present` が何も読んでいなければ、`+ input` だけです。
  - plan が shader の宣言を出さない pass では、`input` の項目をその順に並べ、`+ input` は出しません。
  - 自動操作の名前は `graph_derived_socket#<pass>/<port>` です。`<port>` は、つながっている口では資源の名前、空いた固定の口では `in@<binding>`、`+ input` では `in:+` です。
  - tooltip は、`+ input` が `Drop an image here: <pass> reads one more input`、つながっている口が `<resource>: input <i> of <pass>; drop another image here to read that instead` です。
- **落とす**([`GraphCanvasScene::dropOnPassInput()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4413)): root の node の出力をソケットへ drag して落とします。
  - 落とした出力から資源の名前を決めます([`GraphCanvasModel::outputResource()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6118))。`render.image` / `render.buffer` / `render.attribute` の `resource` はその id、`render.target_ref` は `parameters.name`、render node の出力は同じ名前の入力につながった資源です(8 段まで辿ります)。決まらなければ送らず、notice が `<node>.<port> is not an image or a buffer <pass> can read.` を言います。
  - 送る params は、Frame Plan と同じ [`framePlanSocketConnectRequest()`](../../src/devstudio/view/frameplanedit.cpp#L783) が作ります([呼ぶ所](../../src/devstudio/view/graphcanvaswidget.cpp#L4367))。第二の書き方はありません。つながっている口に落とせばその位置の入力を置き換え、`+ input` に落とせば末尾に足します。空いた固定の口・飛ばした binding・max まで埋まった可変の組の扱いと断りの文も、Frame Plan の表と同じです。
  - `81b62382` から、pass が既に読んでいる image を可変の組の口(`+ input`)へ落とすと、何も送らずに notice が `<pass> already reads <image>.` を言います([断る所](../../src/devstudio/view/frameplanedit.cpp#L834))。それまでは同じ入力を 2 つ書いて送り、engine が `duplicate_input` で断っていました。要求を作る関数は Frame Plan と共有なので、Frame Plan の drop も同じです。test は `devstudio_frameplan_graph_test` の [「VI-3a a drop connects by number: at the next binding, in place, or not at all with the line that says why」](../../test/devstudio_frameplan_graph_test.cpp#L7435) に足した 1 節です。
  - 送るのは Graph タブ自身です([`sendPassEdit()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6490)。GL-1e の着地時の名前は `sendPassInputEdit()` で、✅GL-5a で詳細欄の `edit_preset_pass` と `edit_target_planning` も運ぶ形に広げて改名しました)。送った直後の notice は `<pass>: saving its inputs...`、commit の後は `Pass inputs saved.` です(要求が別の 1 行を持つときはその 1 行。下の「外す」で後ろの入力が詰まるときなど)。
  - commit の後は、ほかのパネルに新しい digest を知らせ、Frame Plan に plan を読み直させ、自分も文書・plan・authoring context を読み直します([`passEditCommitted()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6642))。
  - engine が `external_modification`(読んだ後に文書が変わった)で断ると、notice に `The document changed since the Graph read it; reading it again...` を出して読み直し、読んだ context から同じ操作の params を作り直して 1 回だけ送り直します([`refusePassEdit()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6592))。
  - 前の編集の返事を待っている間は、送らずに `Wait for the current change to finish.` を言います。
- **線**: pass の `input` の項目ごとに実線を 1 本引きます(`graph_pass_input_edge#<from>-><pass>`。[線を組む所](../../src/devstudio/view/graphcanvaswidget.cpp#L1559))。tooltip は `<pass> reads <resource> (input <i>). Delete takes it out.` です。同じ pass と資源の派生の破線は引きません([外す所](../../src/devstudio/view/graphcanvaswidget.cpp#L2222))。
  - 線の元は次の順で決めます。
    1. ✅GL-1f から: plan の順でその pass より前にその資源を書いた pass のうち、canvas の node の出力がその資源に解決される最後のもの(material pass の `color` など。[決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L1587))。下の「pass の出力のソケット(✅GL-1f)」です。
    2. その資源を名乗る root の node(`render.image` などの `resource`、`render.target_ref`)。
    3. 派生の線の元(畳んだ node なら資源の行、graph の node なら見出し)。
- **外す**: 線を click で選び(太く、選んだ色になります)、Delete(か Backspace)を押すか、右クリックの **Delete**(`pelican.graphCanvas.deletePassInput`)を選ぶと、その 1 項目を外した `edit_preset_pass` を送ります([`GraphCanvasScene::deletePassInput()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4403))。params は Frame Plan の [`framePlanSocketDisconnectRequest()`](../../src/devstudio/view/frameplanedit.cpp#L875) が作ります。
- **Undo**: この編集は、Frame Plan の編集と同じく render の編集の履歴に積まれます(下の「render の編集を戻す —— Undo / Redo(✅RU-1 / RU-1b)」)。Graph タブの Undo は、Graph タブが最後に送った編集がこの pass の編集である間だけ、その履歴の Undo を呼びます([`GraphCanvasScene::undoEdit()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4254))。graph の編集を送れば、次の Undo は graph の履歴に戻ります。Redo も同じ形です。Graph タブの Undo / Redo のボタンは、どちらかで戻せれば押せます。
  - Graph タブは、自分が送った pass の編集の数だけを数えます。その後に Frame Plan など別のパネルで render の編集をしたときに Graph タブの Undo が何を戻すかは、test では見ていません(未確認)。
  - 履歴の Undo / Redo は、window の Edit ▸ Undo Render Edit / Redo Render Edit の action を通して呼びます([渡す所](../../src/devstudio/view/mainwindow.cpp#L1435))。統合側の報告では、Graph タブの Ctrl+Z が graph の Undo であることは既存の test が固定しています。
- **古い engine の拒否**: RS-3 の前の engine は graph の pass を `present` の後ろに置くので、`Pass input target is not produced as an earlier output` で断ります。Studio はその文の後に ` — the graph pass that draws it runs after this pass` を足し([句](../../src/devstudio/view/graphcanvaswidget.cpp#L5020)、[graph の pass が書き手のときだけ句を足す else の所](../../src/devstudio/view/graphcanvaswidget.cpp#L6583))、notice の 1 行を切り詰めるときもこの句は残します。✅UX-5d から、句を足すのはその image の書き手が graph の node の pass のときだけです(書き手は frame plan で引きます)。feature の task や rendering 節の pass が書き手なら足しません。✅UX-12d から、拒否が engine の FR-3 の `feature.required_target_unwritten@1` なら、notice はその code の 1 文で、この句は足しません(上の「書き手を繋ぎ、1 本の edit_batch で足す(✅UX-12d)」の Graph の notice)。
- **engine の側**: 置き場所の無い graph の pass が、それを読む rendering 節の pass の前に入るのは RS-3 です。「present に graph の出力をつなぐ」が「その出力が解決される image の名前を present の `input` に書く」ことなのは RS-4 の項にあります。どちらも [第6章](06_rendering.md) §6.18(「graph の pass の置き場所」と「グラフで書く pass」)です。
- test: `devstudio_graphcanvas_scene_test` の [「GL-1e a rendering-section pass takes lines: + input, replace, append, the solid line, Delete」](../../test/devstudio_graphcanvas_scene_test.cpp#L6591)(空の present の `+ input` だけ、落とすと送る `edit_preset_pass` が Frame Plan の drop の要求と同じ、置き換えと末尾への追加、実線と破線が並ばないこと、選んで Delete、root が宣言しない pass は線を受けないこと)と [「GL-1e the Graph tab sends a pass's input edit, its Undo reaches the render history, and an engine before RS-3 says why it refused」](../../test/devstudio_graphcanvas_scene_test.cpp#L6741)(送る RPC、古い engine の拒否の句が切り詰めで残ること、`Pass inputs saved.` と読み直し、Undo が render の履歴に届くこと)。GPU シナリオは `graph_from_blank_forward` の後半です(下の「Studio の自動化面」のシナリオ)。
- **分かっている穴**(着地時の記録): 線の Delete と Undo は scene の test だけで、GPU シナリオでは通していません。GPU で見たのは forward の material pass から `present` の `+ input` への 1 本だけです。

**rendering 節の pass を Graph の詳細欄で直す —— Pass Editor と同じ部品と同じ組み立て、Serial / Isolate / No alias(✅GL-5a、`6565221b`)**: 設計 [design_graph_layers](../design_graph_layers.md) §7 の GL-5(Frame Plan を mode に)の 1 本目です。書き先は今のまま(rendering 節の pass は `edit_preset_pass`)で、入口だけを Graph に束ねます。GL-5a の前の畳んだ node は、GL-1e の `present` の入力のソケットを除いて読むだけで、直すには Frame Plan の Edit pass から Pass Editor を開いていました。

- **持ち主で分ける**: 畳んだ node のうち、authoring context が `editable_root_pass` と言う pass(root の rendering 節が自分で宣言した pass。Blank の `present` など)の 1 pass の node だけを、詳細欄で直せる node にします([`GraphCanvasModel::markFoldedOwner()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5923)、[`GraphCanvasModel::renderingSectionPass()`](../../src/devstudio/model/graphcanvasmodel.cpp#L5909))。preset と feature の pass は今どおり読むだけで、tooltip が `Expand to edit…` を言います(上の「何が出るか」の tooltip の段落)。選んだ node の 1 行の要約(`pelican.graphCanvasFoldedSummary`)にも同じ文が出ます。
- **詳細欄**(`pelican.graphCanvasFoldedPass`。[`GraphCanvasFoldedPassFields`](../../src/devstudio/view/graphcanvasfoldedpassfields.hpp#L67)): root の canvas でその node を選ぶと、右の欄に見出し `<pass> (<type>): from the rendering section` と、Pass Editor と同じ部品が出ます。部品は複製せず、Pass Editor の部品をそのまま持ちます。
  - fullscreen pass: **Vertex** / **Fragment**(`pelican.graphCanvasFoldedPass.vertex` / `.fragment`。Pass Editor と同じ shader の一覧から選ぶ combo で、engine の一覧が届けばそれに替わります)、**Inputs**(`.inputs`。読むだけの一覧。変えるのは GL-1e のソケットへの線です)、**Defines**(`ShaderDefinesForm`)、**Frame data**(`FullscreenPassInterfaceRow`)、**Attachments**(`AttachmentOpsForm`。color の Load / Clear / Store と clear colour)。
  - material pass: `MaterialSettingsForm` の 10 個の設定(✅UX-15a の GPU draws を含む)と、depth を含む `AttachmentOpsForm`。material pass の routing は出しません(Pass Editor も root の pass では routing を書きません。実装者の報告)。
  - fullscreen と material 以外の type の root の pass には欄を出しません。GL-5a の着地時は出力の target の付け替えもありませんでした(実装者の報告)。✅GL-5b から color 出力の行があります(下の GL-5b の段落)。
- **送り方**: 選ぶ欄は選んだときに、打つ欄(define、clear colour、material の設定)は打つ手が 500 ms 止まってから、`edit_preset_pass` を 1 回送ります。params は Pass Editor の Save と同じ [`presetPassEditParams()`](../../src/devstudio/view/presetpassedit.cpp#L12) が組みます(Pass Editor の `requestEditPresetPass()` の本体を切り出したもので、Frame Plan の Edit pass が開く Pass Editor も同じ関数です。[Pass Editor が呼ぶ所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2509))。読み込んだときと値が変わっていなければ送りません。
  - 送るのは Graph タブの GL-1e の口を広げた `sendPassEdit()` で、1 つずつ・ticket を待ち・render の編集の履歴に積みます。Graph タブの Undo 1 回で戻ります(上の GL-1e の「Undo」と同じ)。notice は送った直後が `<pass>: saving...`、commit の後が `<pass> saved.` です。
  - 値を作れないとき(engine が断る define、まだ色になっていない clear colour など)は送らず、notice が `<pass>: <理由>` を言います。拒否は GL-1e と同じく inspectormodel の 1 文の表で notice に出ます。
  - Pass Editor と同じく、編集した root の pass には `"ray_query": false` が書き足されます(実装者の報告。Pass Editor の Save と同じ動き)。
- **Serial / Isolate / No alias**: 詳細欄の **Decisions** に、pass の **Serial**(`.serial`)・**Isolate**(`.isolate`)と、pass が書く render target ごとの **No alias: <target>**(`.noAlias.<target>`)の check があります。押すと Frame Plan の右クリック(✅GL-3a)と同じ `graphTargetPlanningParams()` で組んだ `edit_target_planning` を 1 回送り、notice は `Serial set on <pass>.` / `Serial taken off <pass>.` の形です([`GraphCanvasFoldedPassFields::togglePlanning()`](../../src/devstudio/view/graphcanvasfoldedpassfields.cpp#L709))。check の状態は `get_render_documents` で読んだ root の `target_planning` で、commit か拒否の後に読み直します。present の画面(display)には No alias を出しません。送る口は `GraphCanvasReadCapability` に足した `requestEditTargetPlanning` です。
- **作らなかったもの**: 仕様の決定 3(rendering 節の compute task を Task Editor の部品で直す)は作っていません。root の rendering 節は `compute_tasks` を宣言できない(MG-2b)ので、畳んだ node になる task は全部 preset か feature のもので、読むだけだからです(実装者の報告。広げるかは裁定待ち)。GL-5b(Add fullscreen pass / Add compute task / Add material pass / New render target を palette と同じ関数に。詳細欄の出力の target の付け替えもここ)は ✅GL-5b(下の段落。Add は束ねていません)、GL-5c(Frame Plan の Edit pass / Edit task を Graph への委譲に)は GL-5a の着地時は残っていました。✅GL-5c で、Frame Plan の Edit pass と行のダブルクリックがこの詳細欄を開くようになりました(下の GL-5c の段落)。
- **手で打つ欄**: 新しくは足していません。詳細欄は Pass Editor の部品を持つので、部品が元から持つ打つ欄(define の名前と値、clear colour、material の出力の名前と `+ new tag`)が Graph にも出ます。
- test: `devstudio_graphcanvas_scene_test` の [「GL-5a a rendering-section pass picked on the Graph is edited in the detail column: a shader pick is one edit_preset_pass with Undo, Serial one edit_target_planning, and a refusal reads as the inspector's sentence」](../../test/devstudio_graphcanvas_scene_test.cpp#L6868)、`devstudio_fullscreen_pass_test` の [「GL-5a the Pass Editor and the Graph's detail column send the same edit_preset_pass for the same change of the root's pass」](../../test/devstudio_fullscreen_pass_test.cpp#L3036)(Frame Plan の Edit pass が開く Pass Editor で fragment を変えて Save した params と、Graph の詳細欄で同じ fragment を選んだ params が一致すること)。GL-1 の tooltip の test は新しい文に直しました。GPU シナリオは `gui_folded_pass_edit`(下の「Studio の自動化面」)です。

**New render target を Frame Plan と Graph で 1 つの関数に、詳細欄で出力の target を選び直す(✅GL-5b、`60ff3a2c`)**: 設計 [design_graph_layers](../design_graph_layers.md) §7 の GL-5(Frame Plan を mode に)の 2 本目です。GL-5a と同じく、書き先は今のままで入口だけを束ねます。

- **New render target は 1 つの関数です。**form を開くのは [`presentNewRenderTarget()`](../../src/devstudio/view/rendertargetform.cpp#L272)(create の form を、提案した名前・`RenderTargetFormValues` の既定の format と大きさ・engine の語彙で開く)、名前の提案は [`proposedRenderTargetName()`](../../src/devstudio/view/frameplanedit.cpp#L1429)(pass の出力から開くなら Pass Editor と同じ `<pass>_color`、ほかは `render_target`。宣言済みと重なれば `_2`、`_3`…)、送るのは今までの `framePlanRenderTargetAddRequest()` の `edit_render_targets` 1 回、拒否の文も今までの `framePlanRenderTargetRejection()` です。Frame Plan の toolbar・canvas の右クリック・Pass Editor の `New target: …` と、Graph の 2 つの入口(下)が同じ関数を通ります。Frame Plan の New render target... も、GL-5b から名前が入って開きます(下の「Frame Plan で render target を宣言する(✅F-1)」)。
- **palette の `render.target_ref` の New render target…**: target の combo(`pelican.graphCanvasPaletteTarget`)の最後に **New render target…** が付きます(engine の接続が `edit_render_targets` を送れるときだけ)。選ぶと note が `OK opens New render target; the reference is placed once the target is declared.` を言い、OK で palette が閉じて New render target の form が開きます。form の OK で target が宣言されると、Graph タブは authoring context を読み直し、その target が載った時点で、一覧から選んだときと同じく `<target>_ref` の node を置きます([palette の New render target… を受ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L6786))。form を Cancel すると何も置きません。GL-5b の前は、宣言が 1 つも無いと OK を押せず、Frame Plan へ案内していました(上の「palette で置ける render node」の表の `render.target_ref` の行)。
- **詳細欄の出力の行**: GL-5a の詳細欄(rendering 節の pass)に **Writes** の行が足され、color 出力ごとに combo(`pelican.graphCanvasFoldedPass.output.<n>`)が出ます。候補は宣言済みの target のうち depth でないもの(今の値が一覧に無ければ先頭に足す)と、最後の `New target: <pass>_color…` です([`GraphCanvasFoldedPassFields::fillOutputs()`](../../src/devstudio/view/graphcanvasfoldedpassfields.cpp#L372))。target を選ぶと、その出力の Load / Store と clear colour を連れて target を替え([`AttachmentOpsForm::retargetColor()`](../../src/devstudio/view/passforms/attachmentopsform.cpp#L198))、Pass Editor の Save と同じ `presetPassEditParams()` の `edit_preset_pass` を 1 回送ります。`New target: …` は New render target の form をその名前で開き、target が宣言されて context に載った後に、その出力をその target に替える `edit_preset_pass` を送ります。宣言と付け替えは別の 2 回の編集なので、Graph タブの Undo も 2 回分です(実装者の報告)。depth の出力は付け替えません。
- **拒否**: `edit_render_targets` の拒否は Frame Plan の New render target と同じ code ごとの 1 文です。詳細欄の編集が断られたときは、欄を root の内容で読み直します([`GraphCanvasFoldedPassFields::editRefused()`](../../src/devstudio/view/graphcanvasfoldedpassfields.cpp#L451)。断られた付け替えが combo に残って次の編集に混ざらないためで、GL-5a の shader の選び直しの拒否にも効きます。実装者の報告)。送る口は GL-5a の `sendPassEdit()` に `edit_render_targets` を足したもので、`GraphCanvasReadCapability` に `requestEditRenderTargets` が増えました。
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
- test: `devstudio_graphcanvas_scene_test` の [「GL-5b the target_ref palette's New render target... opens the Frame Plan's form on a proposed name, sends its one edit_render_targets, and places the reference once the target is declared」](../../test/devstudio_graphcanvas_scene_test.cpp#L7218)・[「GL-5b a rendering-section pass's output is picked again from the declared targets, or New target... declares one first: each is one edit_preset_pass」](../../test/devstudio_graphcanvas_scene_test.cpp#L7270)。`devstudio_frameplan_graph_test` の [「F-1 New render target sends one edit_render_targets from the form, and the target's node takes Delete」](../../test/devstudio_frameplan_graph_test.cpp#L9420) は、form が `render_target` の名前で開くことを 1 行見ます。GPU シナリオは `gui_graph_new_target`(下の「Studio の自動化面」)です。

**graph root では Frame Plan の Add を Graph の palette に、Edit pass とダブルクリックを Graph の詳細欄に(✅GL-5c、`9cd89c5c`)**: 設計 [design_graph_layers](../design_graph_layers.md) §7 の GL-5(Frame Plan を mode に)の 3 本目です。GL-5a / GL-5b は書き先を今のままにして入口だけを束ねました。GL-5c は**利用者の決定(2026-10-04)**により、graph root(Blank のように root が graph で、preset を選ばない project)では、Frame Plan の **Add material pass…** / **Add fullscreen pass…** / **Add compute task…** を Graph の palette と同じ道(graph の `nodes` に書く)に寄せました。preset を選ぶ root(Hybrid など)は今までどおり form と managed feature fragment(`pass-<id>.json`)です。

- **どの root で寄せるか**: Frame Plan の 3 つの Add は [`addThrough()`](../../src/devstudio/view/frameplanwidget.cpp#L3510) を通ります。authoring context の `config_kind` が `direct` で、Graph タブが「置く」と答えたときだけ Graph へ渡し、それ以外は今までの form・Pass Editor・Task Editor を開きます。Graph タブは、読んだ context で root が graph でないと分かっているときだけ断ります([MainWindow の配線](../../src/devstudio/view/mainwindow.cpp#L862))。Frame Plan が context をまだ読んでいなければ、status 行に `Reading the render authoring context...` を出し、読んでから決めます。repo の fixture にも root が graph で preset を選ばないもの(`studio_automation_graph` など)があり、それらも Blank と同じく Graph へ渡します(実装者の報告)。
- **Add material pass…**(toolbar のボタンと右クリック): Graph タブが前に出て、root の scope で、絞り込み欄に `render.material_pass` が入った palette が開きます([`GraphCanvasWidget::placeFromPalette()`](../../src/devstudio/view/graphcanvaswidget.cpp#L10125))。Add node… と同じ palette で、置くのも同じ `addFromPalette` です。contract を選んで OK(絞り込み欄の Return)を押すと、draw ループ付きの material pass(✅GL-1d)と自前の image が graph の `nodes` に入ります。`pass-<id>.json` は作らず、render target も宣言しません。material pass の form(`pelican.materialPassForm`)は開きません。
- **Add fullscreen pass…**(右クリック): 選ぶものが無いので palette を開かず、Graph タブ自身の Add fullscreen pass と同じく `render.fullscreen` を root に置きます([Graph タブが op を受ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L9500))。
- **Add compute task…**(右クリック): palette が `render.compute` で開きます。destination(新しい feature 文書の置き場)を打つ欄は出ません。root の rendering 節は `compute_tasks` を宣言できない(MG-2b)ので、graph の `nodes` が書き先です。
- **置いた node を選ぶ**: Frame Plan から開いた palette が root に置いた node は、編集が commit して文書がその node を持った時点で選ばれ、詳細欄が開きます([`placed()`](../../src/devstudio/view/graphcanvaswidget.cpp#L9490))。material pass では、その前に engine が起こし直すことがあります(route の既定。下の「分かっている穴」も。G-buffer の format による起こし直しは ✅MC-1s で無くなりました)。
- **Add の form が持っていた選択**: 出力の target は palette の material pass が自前の image を連れて来ます。付け替えは `render.target_ref` を color の入力に繋ぐか、`present` へは `+ input` に線を引きます。material の routing は palette が forward の route の既定を同じ編集で送ります(✅GL-1d)。差し込む位置は graph の線の向き(RS-3)で決まるので、palette にも詳細欄にも欄を足していません(実装者の報告)。
- **Edit pass / Edit task と行のダブルクリック**: 選んだ pass / task を Graph が編集するなら、今までの Open in Graph と同じ道で Graph タブのその node を選び、詳細欄を開きます([`editsInGraph()`](../../src/devstudio/view/frameplanwidget.cpp#L3596)、[ダブルクリックの分岐](../../src/devstudio/view/frameplanwidget.cpp#L1034))。Graph が編集するのは、ループの回の copy、graph の `render.compute` が作った task(✅Q-12 の `graph_node`)、Graph タブの [`GraphCanvasModel::planPassEditedHere()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6408) が真になる pass(instance やループの body の node、root の文書が持つ node、rendering 節の pass)です。rendering 節の pass は type を問わず Graph へ行きます(GL-5a の詳細欄は fullscreen と material 以外では欄を出しません)。preset と feature の pass、graph node の無い task は今までどおり Pass Editor / Task Editor です。Pass Editor は残しています。この委譲は root の種類を問いません(Graph が編集する pass かどうかだけで決まります。コードの読み)。
- **New render target の既定の種類**: GL-5b の分かっている穴の直しです。form を開く所が「target が forward の material pass の color 出力になる」と分かっているときは、Studio の material pass の slot の表([`materialPassSlots()`](../../src/devstudio/model/frameplanmodel.cpp#L2263)。Frame Plan の Add material pass… の Create missing targets と同じ表)の format / format_class / usage を宣言として読んだ値で form を開きます([`materialPassColorTargetValues()`](../../src/devstudio/view/materialpassedit.cpp#L50)。読むのは form が既存の宣言を読むのと同じ `renderTargetFormValues()`)。forward の color は HDR でも SDR でも **Explicit format の `R16G16B16A16_SFLOAT`** です。GL-5b までの既定の Scene color は、Blank の forward の material pass の color 出力として engine に `Material pass color output format mismatch` で断られていました。名前は呼び出し側の提案(`<pass>_color` など)のままです。効くのは Graph の 2 か所です。
  1. 詳細欄の `New target: <pass>_color…`: その rendering 節の pass の宣言が `type: material` なら、宣言の `material_contract` と出力の番号から決めます(`passOutputTargetKind()`。[決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L6811))。
  2. palette の `render.target_ref` の New render target…: palette を開いたときに選んでいた node が `render.material_pass` で、その contract の color 出力が 1 つ(forward)なら、その出力の種類です([`besideMaterialPassTargetKind()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6824))。G-buffer(color 5 つ)は、どの出力に繋ぐか分からないので今までの既定です。

  それ以外(Frame Plan の New render target…、Pass Editor の `New target: …`、fullscreen pass の出力、何も選ばずに開いた palette)は今までの既定(Scene color)です。form は [`presentNewRenderTarget()`](../../src/devstudio/view/rendertargetform.cpp#L272) の引数 `kind` でこの値を受けます。engine の拒否の文からは決めていません(拒否は format の名前を返さないので、表から決める方が確かなため。実装者の報告)。
- **道具の列の tooltip**: context を読んで graph root だと分かると、Add material pass…(ボタンと action)・Add fullscreen pass…・Add compute task… の tooltip が `Opens in the Graph: the palette's <op>, placed in the graph's nodes and picked there.` になります。preset root では今までの行き先(`The material pass form: the pass goes into a fragment of the project.` / `The Pass Editor, on a new pass after the node picked.` / `The Task Editor, on a new task.`)を言います。Edit pass / Edit task の tooltip も `Opens in the Graph: …` で始まり、Pass Editor / Task Editor に行く場合を続けて言います([`syncDelegationTips()`](../../src/devstudio/view/frameplanwidget.cpp#L3604))。
- **View > Pass Editor**(`pelican.view.passEditor`): Pass Editor の dock を出して前に出します([足した所](../../src/devstudio/view/mainwindow.cpp#L1903))。graph root では Frame Plan の Add fullscreen pass… が Pass Editor を開かなくなったので、Pass Editor 自身の **New pass** から入る入口として足しました。Pass Editor の dock は Frame Plan と同じ領域のタブで、View > Panels の切り替えでは前に出せないためです(実装者の報告)。Pass Editor の New pass と Save は、graph root でも今までどおり managed fragment に書きます(GL-5c は Frame Plan の入口だけを変えました)。→ ✅GL-5f(`42715e8f`)から、graph root の New pass は Graph の palette の `render.fullscreen` です(下の GL-5f の段落)。
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
  - Pass Editor の New pass は、Frame Plan の Add と違って frame plan を読み直しません(シナリオは Pass Editor の Refresh を押します)。New pass を Frame Plan の Add と同じ関数にする案は手を付けていません。→ ✅GL-5d でも切りました(下の GL-5d の段落の「切ったもの」)。→ ✅GL-5f(`42715e8f`)で、graph root の New pass は Frame Plan の Add fullscreen pass… と同じく Graph へ渡すようになりました。
  - 決定 4 の palette の推し量りは「palette を開いたときに選んでいた material pass」からです。何も選ばずに `render.target_ref` を置いて後で繋ぐ場合は今までの既定です。
  - 上の表の「出ていない」もの(Order…、Fix sockets、資源の編集・削除、右クリックの Expand to edit…)は移していません。Frame Plan の New render target… も Graph へは委譲していません。→ ✅GL-5d で、Order…・Fix sockets / Make sockets variadic・資源の編集と削除(Edit target… / Delete target)・右クリックの Expand to edit… を Graph に移しました(下の GL-5d の段落)。Shift ドラッグの接続は GL-5d でも移していません。
- **手で打つ欄**: 新しくは足していません。graph root では Add compute task… の destination の欄が出なくなり、palette の compute shader の combo で選びます。Frame Plan から開いた palette の絞り込み欄は op の名前が入った状態で開くので、打たずに Return でよいです。
- test: `devstudio_frameplan_graph_test` の [「GL-5c decisions 1, 2 and 5: on a root that selects no preset the Adds hand the Graph the palette's op and say so; on a preset's root they keep their forms」](../../test/devstudio_frameplan_graph_test.cpp#L8399)(direct の context で Add の 3 つが Graph に op を渡し、form・Pass Editor・Task Editor は開かない。Graph が置けない root と preset root では今までどおり。tooltip)、[「GL-5c decision 3: Edit pass and a double-click open a pass the Graph edits there; a pass it reads only opens in the Pass Editor」](../../test/devstudio_frameplan_graph_test.cpp#L8501)、[「GL-5c decision 4: New render target for a material pass's colour output opens on the kind the contract's table gives it, not on the Scene color default」](../../test/devstudio_frameplan_graph_test.cpp#L9686)(forward の color は Explicit format の `R16G16B16A16_SFLOAT`、deferred の albedo は scene、material は data の `R8G8B8A8_UNORM`)、`devstudio_graphcanvas_scene_test` の [「GL-5c decisions 1, 3 and 4: the Frame Plan's Add opens the palette in the root on its op, …」](../../test/devstudio_graphcanvas_scene_test.cpp#L7334)(palette が `render.material_pass` で開き、OK で root の `nodes` への `add_node` が 1 回、`edit_preset_pass` / `edit_render_targets` は 0 回、文書が node を持った時点で選ばれ詳細欄の Contract が出る、`editsPlanPass()` の真偽、選んだ forward の pass の横の palette の New render target… が Explicit format で開く)。GPU シナリオは新しい `gui_frameplan_delegates` と、期待を変えた既存の 15 本です(下の「Studio の自動化面」)。

**Frame Plan にしか無かった操作を Graph に —— Order…、Fix sockets / Make sockets variadic、Edit target… / Delete target、右クリックの Expand to edit…(✅GL-5d、`f818f10a`)**: 設計 [design_graph_layers](../design_graph_layers.md) §7 の GL-5(Frame Plan を mode に)の 4 本目です。GL-5c の「Frame Plan にしか無い操作」の表で「出ていない」としたものを、**利用者の決定(2026-10-04)**により Graph へ移しました。Graph の操作は Frame Plan と同じ関数を呼び、Frame Plan のボタンは残っています(入口を束ねるだけで、書き先は変わりません)。

- **出る所**: root の canvas で node を選ぶと、詳細欄の要約の行の下にボタンの列(`pelican.graphCanvasNodeActions`)が出て、その node に効く操作だけが並びます(`pelican.graphCanvasNodeActions.orderPasses` / `.fixSockets` / `.makeSocketsVariadic` / `.editTarget` / `.deleteTarget` / `.expandToEdit`。[ボタンの列を作る所](../../src/devstudio/view/graphcanvaswidget.cpp#L5870))。node の右クリックの menu の先頭にも、同じ名前付きの action(`pelican.graphCanvas.orderPasses` / `.fixSockets` / `.makeSocketsVariadic` / `.editTarget` / `.deleteTarget` / `.expandToEdit`)が並びます([`addNodeMenuEntries()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7010))。右クリックはその node を選んでから menu を出します。出す項目の無い node の右クリックは、今までどおり node を選びません。どの node に何が出るかは [`nodeSubject()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6879) と [`syncNodeActions()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6966) で決めます。
- **Frame Plan が隠れていても動きます**: Order… / Fix sockets / Make sockets variadic は Frame Plan の関数を呼ぶので、Graph タブは [`FramePlanWidget::whenRead()`](../../src/devstudio/view/frameplanwidget.cpp#L5438) を通します。Frame Plan は隠れている間 plan も context も自分では読まない(N-8)ので、`whenRead()` は plan と authoring context を読み直してから操作を走らせます。その操作が Frame Plan の status 行に出す文は、Graph の notice にも出ます([GL-5d の MainWindow の配線](../../src/devstudio/view/mainwindow.cpp#L932))。
- **Order…**(決定 1): root の rendering 節の畳んだ node にだけ出ます。graph の node(順は線と Place の欄で決まる)には出ません。押すと、Frame Plan の toolbar の **Order...**(`pelican.framePlanOrderNodes`)と同じ [`orderNodes()`](../../src/devstudio/view/frameplanwidget.cpp#L3532) が、plan の node を plan の順に並べて Features タブの順序のダイアログ(`pelican.frameNodeOrder`。§13.5「レンダー設定を編集する」の Frame Plan の **Order...** の段落)を開きます。Frame Plan の Order... のボタンも GL-5d からこの関数を呼びます。ダイアログの 1 行(`pelican.frameNodeOrder.notice`)に、GL-5d でシナリオが読めるよう名前を付けました。
  - **rendering 節の pass の入れ替え(✅FO-1 `a4d7760a` で engine 側が直りました)**: GL-5d の着地の時点では、入口が入っただけで、入れ替えはできませんでした。engine が rendering 節の pass を同じ canonical の区切りの中で宣言順に `after` でつなぎ(その 2 つの間に明示の `after` / `before` が無い限り)、`frame_node_order` はその上に隣どうしの `after` を足すだけだったためです。依存の無い 2 つの pass を入れ替えても `logical graph contains a dependency cycle: main_render (render_pipeline_preflight_failed)` で断られていました(GL-5d の実装者の報告。fixture の `left` / `right` でも、`studio_automation_graph` の 2 つの snapshot copy でも同じ)。
    FO-1 から、`frame_node_order` が名指した pass は宣言順の代わりに並びの順でつながり([`enforceCanonicalOrder()`](../../src/project/featurecompose.cpp#L411))、依存の無い 2 つの入れ替えは committed になって plan の順も入れ替わります。Order… が送る plan の全部の並びでも、入れ替える 2 つだけの並びでも通ります(engine の test)。
    依存と矛盾する並びは、ticket の結果(`get_edit_result` の `status: "rejected"`)の `error` で `frame_node_order.conflicts_dependency@1` です。fields は `graph`・`first`(並びが先に置いた node)・`second`(後に置いた node)・`reason`(`reads` なら `resource` も)・`via?`・`anchor?` です(形は [第6章](06_rendering.md) §6.6 の「pass の順を指定する」)。たとえば `present` が `left` の `left_color` を読む構成で `present` を先に置くと、`{first: "present", second: "left", reason: "reads", resource: "left_color"}` です。
    Studio が fields から 1 文を作ること(たとえば「present は left の left_color を読むので、left より先にできません」)と、`gui_graph_order` を入れ替えの形に広げることは FO-1s で、FO-1 の着地の時点では Studio は変わっていません。その直後の GL-5e(`74767e46`)から、順序のダイアログは断られた Apply を Studio の拒否の表の 1 文で出し、表に無い code なら engine の message と code のまま出します(`orderRefusal()`。コードの読み)。`frame_node_order.conflicts_dependency@1` は表にまだ無いので、engine の message のまま出る見込みです(実物では未確認)。
    → ✅FO-1s(`f186320f`)で表に入りました。拒否はダイアログの 1 文になり、並びが逆にした 2 行が赤くなります(§13.1 の「Studio の Materials のドック」の末尾の FO-1s の段落)。`gui_graph_order` は入れ替えの形に広がりました。
- **Fix sockets / Make sockets variadic**(決定 2): 出るのは次の 3 か所です。
  1. root の rendering 節の pass(GL-5a の詳細欄の node)。
  2. root の `render.fullscreen` / `render.raster` の node のうち、plan の pass を 1 つだけ作ったもの(ループの node が作った回の copy には出しません)。
  3. rendering 節の pass への線(GL-1e の実線)の右クリック。この menu に出るのは Fix sockets / Make sockets variadic と、前からの Delete だけです([線の右クリック](../../src/devstudio/view/graphcanvaswidget.cpp#L3407))。

  Fix sockets は pass の shader が可変の組を持つときに出て、押せるのは Frame Plan と同じ `framePlanSocketFix()` が断らないときです(断るときは tooltip がその 1 文)。Make sockets variadic は、pass の shader が project の file で Fix sockets の目印を持つときに出ます。書き換えは Frame Plan の `fixSockets()` / `makeSocketsVariadic()` そのものです(下の「可変の組を固定する(✅VI-3b)」)。
  - **graph の node が描く pass で `engine://passthrough` を固定するとき**は、Graph タブが Frame Plan の関数に口(`graph_fragment`)を渡します。Frame Plan は複製を書いた後に `edit_preset_pass` を送らず、Graph タブがその node の `shader.fragment` を複製に替える `set_node_parameters` を 1 回送ります([複製を node に載せる所](../../src/devstudio/view/graphcanvaswidget.cpp#L7084)、[Frame Plan の側で分ける所](../../src/devstudio/view/frameplanwidget.cpp#L2349))。`edit_preset_pass` は rendering 節が宣言した pass にしか届かないためです(コードのコメント)。Graph が編集中なら、終わってから送ります。送れなかったときは Frame Plan の文 `Wrote shaders/<名前>.frag, but the Graph could not move <pass> onto it.` が出ます(複製の file は残ります)。
- **Edit target… / Delete target**(決定 3): `render.target_ref` の node に出ます(`render.image` は node の欄で直すので対象外)。送るのは GL-5a の `sendPassEdit()` の道で、render の編集の履歴に積まれます。
  - **Edit target…**: Frame Plan の resource の double-click と同じ [`presentRenderTargetEdit()`](../../src/devstudio/view/rendertargetform.cpp#L283) で form を開きます(Frame Plan の double-click の中身を GL-5d でこの関数に切り出しました)。他所で宣言された target は読み取り専用で、出所の 1 行が出ます。宣言が無ければ notice が `No render target named <name> is declared: pick or make one in the reference first.` です。OK で Frame Plan と同じ `framePlanRenderTargetReplaceRequest()` の `edit_render_targets` を 1 回送り、notice は `Render target <name> saved.` です([form の OK を edit で分ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L6720))。
  - **Delete target**([`deletePickedTarget()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7129)): その target を plan の pass が読む・書く(history の読みを含む)か、参照 node に線があるときは、Frame Plan の Delete と同じ `framePlanRenderTargetRemoveRequest()` をそのまま送り、engine の拒否を 1 文で出します(`render_target_referenced` なら `<name> is still used by <users>: disconnect it there before deleting or renaming it.`)。それ以外は、先に参照 node を `remove_node`(graph の編集)で消し、それが commit してから宣言を消す `edit_render_targets` を送ります([commit を待つ所](../../src/devstudio/view/graphcanvaswidget.cpp#L7175))。参照 node が名前を持ったまま宣言だけを消すと、engine が合成で `graph.target_ref_unknown@1` と断るためです(実装者の報告)。この道の Undo は 2 回分(graph の Undo と render の編集の履歴)です。消えたときの notice は `Render target <name> deleted.` です。
  - 拒否の文の表に `graph.target_ref_unknown@1` の 1 文を足しました: `<name> is still named by the Graph's render.target_ref <node>: delete that node before deleting or renaming the target.`([GL-5d で足した文](../../src/devstudio/view/frameplanedit.cpp#L1523))。別の参照 node が同じ名前を持つときに出る想定で、実物では見ていません(実装者の報告)。graph の pass が書く target も `render_target_referenced` の `users` に入ることは、シナリオで見ています。
- **右クリックの Expand to edit…**(決定 4): root の canvas の畳んだ material pass(feature が作っていないもの)と、雛形のある feature の畳んだ node の右クリックと詳細欄のボタンに、notice の Expand to edit… と同じ action(`pelican.graphCanvas.expandToEdit`)が出ます。material pass では Frame Plan の Decompose into loop…、feature では Features タブの Replace with loop version… で、どちらも先に確認を聞きます([`expandToEdit()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7912))。root でどちらにするかは [`GraphCanvasModel::foldedNodeExpand()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6916) が、畳んだ形の中の notice と同じ規則で決めます。graph の node には出ません。
- **Frame Plan の tooltip**(決定 6): Frame Plan の Order...・Fix sockets・Make sockets variadic・New render target... の tooltip が、Graph に同じ操作があることを言います(Order... なら `The order of the plan's nodes (data dependencies still decide). The same as Order… on a pass of the rendering section in the Graph.`)。
- **切ったもの**(実装者の報告):
  - 決定 5(Shift ドラッグの接続): 調べた結果だけです。rendering 節の pass への接続は、Graph の通常のドラッグが Frame Plan の素のドロップと同じ関数を呼び(GL-1e)、graph の node の間は graph の線なので、意味が重なるため足していません。graph root には preset が無いので、Frame Plan にしか残らないのは project の feature の authored な task と pass への Shift ドラッグで、Graph ではそれらは読むだけの畳んだ node です(設計 §4 の規則 3)。
  - 決定 7(Pass Editor の New pass を graph root では Frame Plan の Add と同じ委譲に): GL-5c が Pass Editor の New pass の道に寄せた既存のシナリオ 4 本(`fullscreen_pass_from_template`・`lighting_pass_template`・`render_undo_roundtrip`・`render_pipeline_from_scratch`)の書き直しと GPU の再実行が要るので切りました。→ ✅GL-5f で入り、4 本を書き直しました。
  - 決定 5 の Shift ドラッグは、✅GL-5e で「畳んだ node への線の drop」として Graph に入りました(下の GL-5e の段落)。
- **graph root で Frame Plan にしか残っていない編集の操作**: GL-5d の着地の時点では、Shift ドラッグ、passthrough のつながっている ○ のクリック、rendering 節の pass の View、managed fragment の pass の Delete、shader の契約と reload の拒否のボタン、Physical plan / Backends の操作の 6 つが Frame Plan にしか無いと、実装者がコードを読んで挙げていました(test はありません)。✅GL-5e(`74767e46`)と ✅GL-5f(`42715e8f`)でそれらを移し、GL-5f の実装者が Frame Plan の入口を全部洗い直した表が次です(GL-5f の報告の「決定 6」。実装者がコードを読んで作った表で、行ごとの確かめ方は下の GL-5e / GL-5f の段落)。
  「同じ関数」は Frame Plan(か Frame Plan から開く editor)の関数そのものを Graph から呼ぶもの、「同じ edit」は要求を組み立てる関数と RPC が同じで、呼ぶ側だけが Graph のものです。

| Frame Plan の入口 | 関数 → RPC | Graph のどこから | 種類 |
|---|---|---|---|
| Add material pass… / Add fullscreen pass… / Add compute task… | `addThrough` → Graph の palette | palette の `render.material_pass` / `render.fullscreen`(`pelican.graphCanvas.addFullscreenPass`)/ `render.compute` | 同じ関数(GL-5c) |
| Edit pass / Edit task / ダブルクリック | `editsInGraph` → Open in Graph(Graph が編集しないものは Pass Editor / Task Editor) | Graph の詳細欄。feature の authored な task は **Edit task…**(`pelican.graphCanvas.editTask`)が Frame Plan の Edit task と同じ `RenderFeaturesWidget::editComputeTask` を開く | 同じ関数(GL-5c / GL-5f) |
| New render target… | `presentNewRenderTarget` → `edit_render_targets` の add | palette の `render.target_ref` の New render target…、詳細欄の New target… | 同じ edit(GL-5b) |
| 資源のダブルクリック / Delete | `presentRenderTargetEdit` / `framePlanRenderTargetRemoveRequest` | `pelican.graphCanvas.editTarget` / `.deleteTarget` | 同じ edit(GL-5d) |
| node の Delete(managed fragment の pass) | `removeNode` → `remove_authored_pass` | `pelican.graphCanvas.deletePass`(右クリック・詳細欄・Delete キー) | 同じ関数(GL-5e) |
| node の Delete(authored な compute task) | `removeNode` → `remove_compute_task` | 同じ `pelican.graphCanvas.deletePass` で、文言が **Delete task**。rendering 節の task は root の畳んだ node、project の feature の task は feature の fold に潜った先の node | 同じ関数(GL-5f) |
| Order... | `orderNodes` → 順序のダイアログ → `edit_frame_node_order` | `pelican.graphCanvas.orderPasses` | 同じ関数(GL-5d) |
| Fix sockets / Make sockets variadic | `fixSockets` / `makeSocketsVariadic` | `pelican.graphCanvas.fixSockets` / `.makeSocketsVariadic` | 同じ関数(GL-5d) |
| Decompose into loop… / Replace with loop version… | `decompose` / `replaceWithLoopVersion` | `pelican.graphCanvas.expandToEdit` | 同じ関数(GL-5d) |
| View の menu(pass) | `framePlanPassViewSelectRequest` → `edit_preset_pass` | 畳んだ pass の `pelican.graphCanvasFoldedPass.view.<axis>`、graph の node は View の欄 | 同じ edit(GL-5e) |
| View の menu(authored な task) | `framePlanTaskViewSelectRequest` → `edit_compute_task` | **Edit task…** で開く Task Editor の View の欄 | 同じ関数(GL-5f) |
| Shift ドラッグ | `editComputeTaskConnection` / `editPresetPassConnection` | 畳んだ node への線の drop | 同じ関数(GL-5e) |
| 宣言済みのソケットへの drop / ソケットを引き離す / 線の Delete | `framePlanSocketConnectRequest` / `…DisconnectRequest` | rendering 節の pass のソケットへの drop、`pelican.graphCanvas.deletePassInput`、Delete キー | 同じ edit(GL-1e) |
| 宣言の無い node への素の drop / task の線の足し引き | `changeResourceEdge` → `edit_preset_pass` / `edit_compute_task` | pass: 「shader declares no inputs」と契約のボタン(GL-5e)。task: **Edit task…** の Task Editor の reads / writes | 同じ関数(Task Editor) |
| passthrough の ○ のクリック | `framePlanPassthroughInputRequest` | Graph のソケットのクリック | 同じ edit(GL-5e) |
| 帯のその場編集 shader.vertex / fragment(rendering 節の fullscreen / material) | `framePlanPassShaderRequest` → `edit_preset_pass` | `pelican.graphCanvasFoldedPass.vertex` / `.fragment` | 同じ RPC(GL-5a。builder は Pass Editor の `presetPassEditParams`) |
| 帯のその場編集 shader と raster_state / draw(rendering 節の raster pass) | Pass Editor の raster の draft → `edit_preset_pass` | 同じ `.vertex` / `.fragment` と `pelican.graphCanvasFoldedPass.rasterState.*` / `.draw.*` | 同じ RPC(GL-5g。builder は `presetPassEditParams`) |
| 帯のその場編集 dispatch(authored な task) | `framePlanTaskDispatchRequest` → `edit_compute_task` | **Edit task…** の Task Editor の dispatch | 同じ関数(GL-5f) |
| Serial / Isolate / No alias | `graphTargetPlanningParams` → `edit_target_planning` | `pelican.graphCanvasFoldedPass.serial` / `.isolate` / `.noAlias.*`。root の graph の node と 1 pass の folded node(preset・feature の pass)にも | 同じ edit(rendering 節の pass は GL-5a、ほかは GL-5g) |
| 契約の Use engine://passthrough / Open the shader | `useEnginePassthrough` / `openContractShader` | `pelican.graphCanvas.usePassthrough` / `.openContractShader`(notice の下にも) | 同じ関数(GL-5e) |
| reload の拒否の Disconnect / Open shader | `disconnectRefusedInput` / `openRefusedShader` | `pelican.graphCanvas.disconnectRefused` / `.openRefusedShader` | 同じ関数(GL-5e) |
| Physical plan の Remove TRANSFER_SRC(root の image) | `removeImageTransferSource` | `pelican.graphCanvas.layers.removeTransferSource` | 同じ関数(GL-5e) |
| Physical plan の Remove TRANSFER_SRC(loop の copy) | `removeImageTransferSource(resource, loop, element, node)` | Layers の右クリック。loop の body の node を選ぶと Layers が「<node>'s copies」の L2 行を出す | 同じ関数(GL-5f) |
| Backends の Pin | `pinCandidate` | `pelican.graphCanvas.layers.pin` | 同じ関数(GL-5e) |
| Unpin / Freeze / Eject again / Remove(frozen) | `unpinBackend` / `freezePhysicalPlan` / `removeFrozenPlan` | Layers の右クリック。行が無いときも、何も選ばない間は Layers の見出し(`pelican.graphCanvasLayersToggle`)の右クリック | 同じ関数(GL-5e / GL-5f) |
| Undo / Redo | `pelican.edit.undoRenderEdit` / `redoRenderEdit` | 同じ action | 同じ |
| Materials タブ | 編集の入口は無い(表示と選択だけ) | — | 対象外(GL-5f が確かめた) |

  Frame Plan から開く editor では、Pass Editor の **New pass** が graph root では Graph の palette の `render.fullscreen` になりました(GL-5f)。Pass Editor の Save / Remove / New… などは Pass Editor の form の中の操作で、graph root で Pass Editor が開くのは preset と feature の pass だけです。Task Editor は Edit task… と畳んだ node への drop で同じ dialog が開きます。

  GL-5f の着地の時点で、graph root で Graph から届かずに残っていたのは次の 2 つでした。
  1. rendering 節の raster pass の shader(帯のその場編集)。Graph の詳細欄(GL-5a)の shader の欄は fullscreen と material だけでした。
  2. Serial / Isolate / No alias を rendering 節以外の pass(graph の node・feature の pass)が書く target に。Graph の詳細欄は rendering 節の pass にだけ出していました。

  → ✅GL-5g(`cfa9859f`)で 2 つとも埋めました(下の「rendering 節の raster pass の欄、graph の node の Serial / Isolate / No alias、Graph の Undo の順(✅GL-5g)」)。GL-5g の実装者が挙げた、graph root で Frame Plan にしか残らないものは次の 3 つです。
  1. status 行の Reload(断られた edit を読み直して送り直す)。編集というより失敗からの戻りなので対象外としています(GL-5f の実装者の判断)。
  2. loop の copy の pass への Serial / Isolate。plan の copy の名前は body の node の id と違うので、Graph で body の node を選んでも Decisions は出ません(Frame Plan の行の右クリックは copy にも出ます)。実装者の案は、GL-5f の Layers の copy の行と同じく loop origin から copy の名前を引くことです。
  3. feature のまとまり(複数の pass の fold)そのものを選んだときの Decisions。fold に潜れば pass ごとに出ます。
- **手で打つ欄**: 新しくは足していません。Edit target… は Frame Plan と同じ form なので、form の名前と clear colour(`r, g, b, a`)の欄が Graph からも出ます。
- test: `devstudio_graphcanvas_scene_test` の [「GL-5d Order..., Fix sockets and Make sockets variadic are the Frame Plan's on the picked pass's right-click and detail column; Edit target... and Delete target send the Frame Plan's edit_render_targets」](../../test/devstudio_graphcanvas_scene_test.cpp#L7469)(rendering 節の `present` で Order… が出て押すと owner の order が 1 回、graph の node には出ない。Fix sockets が `present` では Frame Plan の関数を `graph_fragment` なしで、graph の node では `graph_fragment` 付きで呼び、口を呼ぶと `set_node_parameters` 1 回で `shader.fragment` が複製になり、固定した shader では Make sockets variadic が出る。`present` への線の右クリックに Fix sockets と Delete があり Order… は無い。Edit target… の form の OK が `framePlanRenderTargetReplaceRequest` と同じ params。読まれている target の Delete target はそのまま remove を送って拒否が 1 文、誰も使わない target は `remove_node` → commit の後に remove)、`devstudio_frameplan_graph_test` の [「GL-5d the Graph's Order... and Fix sockets are the Frame Plan's own, run out of view once the plan and the context are read again; a graph node's pass takes the copy through the Graph」](../../test/devstudio_frameplan_graph_test.cpp#L12840)(隠れた Frame Plan で `whenRead()` が plan と context を読み直すまで待ち、来たら `orderNodes()` が plan の node の名前と generation で順序のダイアログの口を呼ぶ。graph の node の pass の Fix sockets は複製を書いて口に `project://shaders/wp343_mixed_a` を渡し、`edit_preset_pass` は 0 回、Frame Plan の文が Graph の側にも届く。Order... と Fix sockets の tooltip が `in the Graph` を含む)、`devstudio_graphcanvas_test` の GL-1 の model の test に `foldedNodeExpand()` の確認(material pass は decompose、雛形のある feature は replace、graph の node と畳んだ形の中は空)。GPU シナリオは新しい `gui_graph_only_edits` と `gui_graph_order`(下の「Studio の自動化面」)です。

**Frame Plan にしか無かった操作の残りを Graph に —— View、passthrough の ○ のクリック、managed な pass の削除、shader の契約、Layers の右クリック、畳んだ node への線(✅GL-5e、`74767e46`)**: 設計 [design_graph_layers](../design_graph_layers.md) §7 の GL-5 の 5 本目です。GL-5d の一覧で Frame Plan にしか残っていなかった操作を Graph に移しました。どれも Frame Plan の同じ関数か同じ edit を呼び、Frame Plan の入口は残っています。Frame Plan の関数を呼ぶものは、GL-5d と同じく `whenRead()` で plan と context を読み直してから走ります(Frame Plan の公開の `removePass()`・`runShaderContract()`・`runReloadRefusal()`・`pinBackendCandidate()`・`unpinBackend()`・`freezePhysicalPlan()`・`removeFrozenPhysicalPlan()` は中で Frame Plan の既存の関数を呼ぶだけです。[Frame Plan の関数を Graph へ渡す所(GL-5e)](../../src/devstudio/view/mainwindow.cpp#L946)、[Graph の側の口 `setFramePlanEdits()`](../../src/devstudio/view/graphcanvaswidget.cpp#L10116))。

- **rendering 節の pass の View**(決定 1): 詳細欄(GL-5a)に、plan がその pass に付けた軸ごとの View の欄(`pelican.graphCanvasFoldedPass.view.<axis>`。LP-4b の `ViewSelectField`)が出ます。選ぶと Frame Plan の View の menu と同じ `framePlanPassViewSelectRequest()` の `edit_preset_pass` を送ります。シナリオ `gui_graph_folded_edits` の (d) で、cube capture の face 軸を持つ `face_tint` を `2 (+Y)` にすると `face_tint saved.`、root の `view_select` と `get_frame_plan` の `view_select.face == 2` を見ています。
- **passthrough の ○ のクリック**(決定 2、[`GraphCanvasScene::clickInputSocket()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4462)): 押して同じ入力ソケットで離すとクリックです。
  - rendering 節の pass(畳んだ node)は Frame Plan と同じ `framePlanPassthroughInputRequest()` です。
  - `engine://passthrough` で描く graph の node は、`shader.defines` の `PELICAN_PASSTHROUGH_INPUT=<k>` を書き換える `set_node_parameters` を送ります(0 番目に戻すときは define を消す)。もう見せている入力なら `<node> already shows <出どころ>.` です。
  - 見せている入力のソケットの名前の後ろに ` (shown)` が付きます(Frame Plan のソケットと同じ表)。
  - `engine://passthrough` でない node のクリックは今までどおりです(ソケットから外へ離すと線が切れる)。
  - シナリオ `gui_graph_only_edits_2` の (b) で、graph の node の 2 つ目の入力のクリックで `defines[0]` が `PELICAN_PASSTHROUGH_INPUT=1` になり画素が変わり、1 つ目のクリックで戻ること、`present` の 2 つ目の入力のクリックで root に同じ define が入ることを見ています。
- **managed fragment の pass の削除**(決定 3): project が足した pass(`passes/authoring/pass-<id>.json` の managed fragment)の畳んだ node を選ぶと、右クリック・詳細欄のボタン・Delete キーに **Delete pass**(`pelican.graphCanvas.deletePass`)が出ます。Frame Plan の Delete と同じ関数で、fragment のファイルも消えます([`deletePickedPass()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7244))。engine の feature の fold には出ません。シナリオ `gui_graph_folded_edits` の (c) で、node が消え、render 文書が 1 つ減り、root から参照が消え、fragment のファイルが消え、画素が変わらないことを見ています。
- **shader の契約と reload の拒否**(決定 4): Graph の edit が `shader_input_mismatch` で断られたとき、詳細欄と notice の下(`pelican.graphCanvasNoticeActions.*`)に **Use engine://passthrough**(`pelican.graphCanvas.usePassthrough`)と **Open the shader**(`.openContractShader`)が出ます。reload が pass の入力を断っているときは **Disconnect**(`.disconnectRefused`)と **Open the shader**(`.openRefusedShader`)です。押すと Frame Plan の `useEnginePassthrough` / `openContractShader` / `disconnectRefusedInput` / `openRefusedShader` が走ります([出す条件 `syncGl5eActions()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7192))。notice の 1 文は Frame Plan の status 行と同じ `shaderInputContractNotice()` です。**確かめたのは unit test まで**です(Blank で shader の拒否を作るには shader のファイルを壊す手順が要るので、シナリオはありません)。
- **Layers の右クリック**(決定 5): Graph の Layers(GL-2b)の行の右クリックに、**Pin this candidate**(`pelican.graphCanvas.layers.pin`)・**Unpin**(`.unpin`)・**Freeze this physical plan**(`.freeze`)・**Eject again**(`.ejectAgain`)・**Remove the frozen plan**(`.removeFrozen`)・**Remove TRANSFER_SRC**(`.removeTransferSource`)が出ます([action を作る所](../../src/devstudio/view/graphcanvaswidget.cpp#L5980))。Pin は L3 の候補の行、Remove TRANSFER_SRC は root の `render.image` が `TRANSFER_SRC` を持つ L2 の資源の行で押せます。**確かめたのは unit test まで**です。GL-5e の時点では、Layers は行が 1 つも無いと隠れたので、そのときは Graph から届きませんでした(→ GL-5f で見出しの右クリックを足しました)。
- **畳んだ node への線**(決定 6、[`dropOnFoldedNode()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7362)): Frame Plan の Shift ドラッグの代わりです。出力から引いた線を root の畳んだ node の上で離すと、
  - project の feature の authored な compute task なら、Frame Plan の Shift ドラッグと同じ `editComputeTaskConnection` のダイアログ、
  - preset か feature の編集できる pass なら `editPresetPassConnection`(Pass Editor)、
  - engine の feature や engine の preset(plan の `provider_ref` が `engine://`)なら何も送らず、`pelican.graphCanvasFoldedNotice` に node の tooltip の 1 行(`From <owner>: read only here. …`)と、展開できるなら Expand to edit… を出します。
  - rendering 節の pass は今までどおりソケットへの drop だけです。
  - unit test が task のダイアログへの要求と engine の feature の読むだけを見ています。シナリオ `gui_graph_only_edits_2` の (a) は graph の node どうしの普通のドラッグだけで、Blank には feature も preset も無いので、ダイアログへ行く道はシナリオでは通っていません。
- **順序のダイアログの拒否**(決定 8): 断られた Apply の文を Studio の拒否の表(`knownRefusalDescription()`)から作り、表に無ければ engine の message と code のままにしました(`orderRefusal()`)。FO-1 の code の文は ✅FO-1s で入りました。
- **Assets のドックの登録済み model**(決定 9): engine の ✅EB-1 で `list_assets` に material の行が増え、path が project 相対になったのに合わせ、登録済みの model のファイルは `kind` が `model` の行だけを相対の path で拾います(`registeredModelFiles()`)。unit test で見ています。
- **切ったもの**: 決定 7(Pass Editor の New pass を graph root では Graph へ)は、口(`FullscreenPassWidget::setGraphPlaceAction()`)だけを作り、配線しませんでした。配線すると GL-5c の道に寄せた既存のシナリオ 4 本が赤になるためです(→ ✅GL-5f で配線し、4 本を書き直しました)。
- **手で打つ欄**: 新しくは足していません(View の欄は combo、ほかはボタンと右クリック)。`gui_graph_only_edits_2` で打つのは New Project の folder と name だけ、`gui_graph_folded_edits` は何も打ちません。
- test: `devstudio_graphcanvas_scene_test` の [「GL-5e a rendering-section pass's View field, a click on its passthrough socket, Delete pass on a managed fragment, the ways out of a refused shader, a line on a folded node and the Layers section's right-click are the Frame Plan's」](../../test/devstudio_graphcanvas_scene_test.cpp#L13568)(決定 1〜6)、`devstudio_assets_dock_test` の [「GL-5e decision 9: list_assets' project-relative path (engine EB-1) finds a registered model, and its material rows are not models」](../../test/devstudio_assets_dock_test.cpp#L774)。UX-5c の test の期待に ` (shown)` を足しました。GPU シナリオは新しい `gui_graph_only_edits_2` と `gui_graph_folded_edits`(下の「Studio の自動化面」)です。実装者の run で `studio_cpu` 733 / 733 と、既存のシナリオ 6 本(`gui_frameplan_delegates`・`gui_graph_only_edits`・`gui_graph_order`・`gui_lower_layer_pin_freeze`・`sockets_fix_and_show`・`view_select_field`)が緑でした。

**Pass Editor の New pass を Graph へ、authored な compute task の削除と Edit task…、loop の copy の Remove TRANSFER_SRC、Layers の見出しの右クリック(✅GL-5f、`42715e8f`)**: GL-5 の 6 本目で、graph root で Frame Plan にしか残らない編集を洗い直して埋めました(上の GL-5d の段落の表)。

- **Pass Editor の New pass**(決定 1、[`newPass()`](../../src/devstudio/view/fullscreenpasswidget.cpp#L2899)): authoring context の `config_kind` が `direct` で Graph が置けるときは、Frame Plan の Add fullscreen pass… と同じく Graph の palette の `render.fullscreen` が graph の `nodes` に置かれて選ばれます。managed fragment は作りません。preset root と、Graph が置けないときは今までの form です。context をまだ読んでいなければ、読んでから決めます。preset root での実物は確かめていません(既存の `view_select_field` は New pass を押さない)。
  - 書き直したシナリオ 4 本(`fullscreen_pass_from_template`・`lighting_pass_template`・`render_pipeline_from_scratch`・`render_undo_roundtrip`)は、New pass の後を Graph の道(palette の `render.target_ref` と線、Pass fields の New… の雛形)にしました(下の「Studio の自動化面」)。
  - これで graph root では、GUI から managed fragment を作る道がありません。GL-5e の Delete pass が相手にするのは、既にある project の fragment だけです(GL-5e の実装者の指摘)。
- **authored な compute task の削除**(決定 2): 1 つの compute task の畳んだ node で、project が書いた task(graph の node が作ったものでなく、provider も feature も `engine://` でないもの)には、GL-5e の Delete pass が **Delete task** の文言で出ます(右クリック・詳細欄・Delete キー。[task を見分ける `markAuthoredTask()`](../../src/devstudio/view/graphcanvaswidget.cpp#L6865))。Frame Plan の Delete と同じ `removeNode` の `remove_compute_task` です。rendering 節の task は root の畳んだ node、project の feature の task はその feature の fold に潜った先の node で出ます。feature の fold 自体と engine の feature の task には出ません。
  - Frame Plan は隠れている間 compute task の catalog も読まないので、Graph だけで task を消すと `The compute task catalog has not arrived yet.` で断られるはずでした。`whenRead()` に `tasks` の引数を足し、真なら catalog も読み直して届くまで待ちます([`whenRead()`](../../src/devstudio/view/frameplanwidget.cpp#L3557))。シナリオ `gui_graph_task_delete` が Frame Plan を一度も出さずに通るのがその確認です。
  - rendering 節の task の削除の実物は見ていません(engine は断るはずで、Frame Plan と同じ拒否の文が出る想定。未確認)。
- **Edit task…**(`pelican.graphCanvas.editTask`、詳細欄の `pelican.graphCanvasNodeActions.editTask`、右クリック。[`editPickedTask()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7250)): 同じ authored な task で、Frame Plan の Edit task と同じ `RenderFeaturesWidget::editComputeTask` の Task Editor を開きます。帯のその場編集の dispatch と、task の View・reads / writes を 1 つで埋めるために足しました。Task Editor の View の欄は Frame Plan の model から軸を読むので、Frame Plan が一度も plan を読んでいないと空のことがありえます(実装者の指摘。未確認)。
- **loop の copy の Remove TRANSFER_SRC**(決定 3): loop の body の node を選ぶ(zone の中でも、body を開いても)と、Layers が `Layers: <node>'s copies` の見出しで、その node の copy の L2 行を出します([`GraphCanvasModel::bodyLayerRows()`](../../src/devstudio/model/graphcanvasmodel.cpp#L6431)。L1 / L3 は出さない)。Remove TRANSFER_SRC は copy の loop origin で Frame Plan の `removeImageTransferSource` を呼びます([押せるかを決める `loopCopyHasTransferSource()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8025))。**model の行は unit test で見ています**。右クリックから loop origin が渡る所はコードの読みだけで、実物は確かめていません。
- **Layers の見出しの右クリック**(決定 4): 何も選ばない間、plan があれば行が 0 でも Layers の見出し(`pelican.graphCanvasLayersToggle`、`▾ Layers: the whole graph`)を出し、その右クリックに Unpin / Freeze this physical plan / Eject again / Remove the frozen plan を並べます([`syncLayers()`](../../src/devstudio/view/graphcanvaswidget.cpp#L7924)、[見出しの menu](../../src/devstudio/view/graphcanvaswidget.cpp#L8039))。unit test は行の無い Blank の plan で見出しが出て 4 つの action が Frame Plan の関数に届くことを見ます。popup そのものは自動化面で開けないので、action を名前で叩いています。
- **帯のその場編集と Materials タブ**(決定 5): 調べた結果、Materials タブには編集の入口がありませんでした。preset と feature の pass の shader は Graph では読むだけ(設計 §4 の規則 3。drop で Pass Editor)です。
- **手で打つ欄**: 新しくは足していません。書き直しで、4 本のシナリオの position・pass の名前・vertex の stem を打つ手が無くなりました。
- test: `devstudio_fullscreen_pass_test` の [「GL-5f New pass on a root that selects no preset is the Graph's render.fullscreen; on a preset's root, or where the Graph does not place it, the form is a new pass's」](../../test/devstudio_fullscreen_pass_test.cpp#L5324)、`devstudio_graphcanvas_scene_test` の [「GL-5f Delete task on a compute task the project authored -- inside its feature's fold, or the rendering section's -- is the Frame Plan's Delete: …」](../../test/devstudio_graphcanvas_scene_test.cpp#L13837)(Edit task… も)と [「GL-5f with no row in Layers, the heading stays with nothing picked and its right-click holds the whole graph's decisions: …」](../../test/devstudio_graphcanvas_scene_test.cpp#L13941)、`devstudio_graphcanvas_test` の [「GL-5f a loop body's image has Layers rows for the copies the loop made of it, each naming the copy's loop origin」](../../test/devstudio_graphcanvas_test.cpp#L4849)。GPU シナリオは新しい `gui_graph_task_delete` と書き直した 4 本です。実装者の run で `studio_cpu` 742 / 742 と、シナリオ 12 本(書き直し 4 本、新 1 本、既存 7 本)が緑でした。

**rendering 節の raster pass の欄、graph の node の Serial / Isolate / No alias、Graph の Undo の順(✅GL-5g、`cfa9859f`)**: GL-5 の 7 本目で、GL-5f の表の「残る 2 つ」を埋め、GL-5f の書き直しでシナリオから外れた 2 つの検査を Graph の道で戻しました。シナリオの「常に通る」確認の洗い出しは下の「Studio の自動化面」の「シナリオの確認が常に通る形(✅GL-5g)」です。

- **rendering 節の raster pass**(決定 1): 詳細欄が raster の pass も扱います。Vertex / Fragment(Pass Editor の一覧。今の値が一覧に無ければ足す)、Defines(`ShaderDefinesForm`)、Raster(Pass Editor の `RasterStateForm`。Pass Editor が raster に出すのと同じく depth と draw の行も)、Attachments です([Raster の欄を置く所](../../src/devstudio/view/graphcanvasfoldedpassfields.cpp#L122))。名前は `pelican.graphCanvasFoldedPass.rasterState.*` / `.draw.*` です。
  - 送るのは Pass Editor の raster の draft と同じ形を `presetPassEditParams` に渡した `edit_preset_pass` で、変わった鍵だけです(fragment を選ぶと shader だけ、頂点数を変えると draw だけ)。
  - Graph の node の `render.raster` と、shader の 2 つの combo・defines・raster_state は同じ部品です。違うのは depth(graph の node は image を選ぶ別の欄、こちらは宣言の `output.depth` と raster_state の depth 行)と draw(graph の node は個数の欄、こちらは RasterStateForm の draw 行)です。
  - `clip_y` は Pass Editor の raster の欄にも無いので足していません。
  - **unit test だけ**です(raster pass を root に持つ fixture がありません)。
- **Serial / Isolate / No alias を他の pass に**(決定 2): root の graph の node を選んで plan にその id の pass があるとき(見出し `<id>: its decisions`)と、1 pass の folded node(preset・feature の pass。feature の fold の中も。見出し `<pass> (<source>): its decisions`)で、詳細欄は編集の欄を隠し、見出しと Serial / Isolate / その pass が書く target ごとの No alias だけを出します([`showPlanningOnly()`](../../src/devstudio/view/graphcanvasfoldedpassfields.hpp#L85)、[選んだものから決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L8152))。送るのは rendering 節の pass と同じ `graphTargetPlanningParams()` → `edit_target_planning` です。fold group の Decisions(GL-3a)はそのままです。
  - engine の `edit_target_planning` は graph の node の名前を受けました(get_frame_plan と同じ、展開した後の名前で検査する)。シナリオ `gui_graph_only_edits_2` の (e) で、graph の node の Serial が root の `target_planning` に `"serial": true` を書くことを見ています。
  - verified の fold group の決定と root の `target_planning` の両方に書いたとき、効くのは「どちらかが true なら true」です。verified の group が true にしているものを root で明示の false にする edit は `graph.fold_decision_conflict@1` で断られます(実装者がコードを読んだ結果)。
  - feature / preset の folded node での実物と、No alias・Isolate の実物は**未確認**です(unit test は Serial と No alias の params が Frame Plan と同じことを見ます)。
- **shader の compile の失敗を Graph の notice に**(決定 3 の F-3): Graph の Pass fields の New… で作った shader を壊して保存すると、拒否の文は Frame Plan の node の印と Pass Editor の log(開いていれば)、Problems の `shader_reload_failed@1` には出ていましたが、Graph の notice には何も出ていませんでした。Frame Plan が Pass Editor に log を渡す同じ口から Graph にも渡し([`GraphCanvasWidget::showShaderReloadNotice()`](../../src/devstudio/view/graphcanvaswidget.cpp#L10498)、[MainWindow が渡す所](../../src/devstudio/view/mainwindow.cpp#L727))、notice が `render_fullscreen's shader did not reload: render_fullscreen.frag:18: error: 'vaule0' : undeclared identifier. The engine keeps drawing the previous version; the Problems window lists it.` のように言います(node は Graph が reload に渡す shader の file から、行は log の最初の error 行)。直った reload(空の log)では自分が出した行だけを下ろします。シナリオ `fullscreen_pass_from_template` に書き足しました。
- **ファイルを書く追加の Undo / Redo**(決定 3 の RU-1b): graph root で Graph からファイルを書く追加は、pass ではなく render target です(palette の `target_ref` の New render target… が project 最初の自前の target なら `passes/authoring/render-targets.json` を作る)。`render_undo_roundtrip` にこれと Graph の Undo 2 回・Redo 2 回を書き足したところ、**Studio の不具合が 2 つ見つかり、直しました**。
  1. **Graph の Undo が New render target… の target に届かなかった**: GL-1e の作りは「tab の最後の edit が render edit の間だけ Graph の Undo が render の編集の履歴へ行く」で、graph の edit(参照を置く)が来ると render edit を数えなくなっていました。New render target… は render edit → graph edit の 1 つの操作なので、参照を戻すと target が戻せませんでした。今は tab が送った edit をどちらの履歴かと一緒に**送った順に**覚え、Undo / Redo は一番新しいものから戻します([`GraphCanvasScene::undoEdit()`](../../src/devstudio/view/graphcanvaswidget.cpp#L4254))。
  2. **render edit の Undo / Redo の後に Graph の Redo が消えた**: graph の model は、読んだ文書の digest が履歴の digest と違うと履歴を捨てます。tab の render edit は root の digest を変えますが graph は変えません。今は tab が render edit を送った・戻した・やり直したとき、次に digest が変わった読みで nodes と definitions(配置を除く)が同じなら履歴を保ちます(違えば今までどおり捨てる。[`keepHistoryThroughNextDigest()`](../../src/devstudio/model/graphcanvasmodel.hpp#L1527))。
  - 直した後、`render_undo_roundtrip` と、Graph の Undo を使う既存の 8 本(`graph_loop_expand`・`gui_build_audio_reactive`・`gui_folded_pass_edit`・`gui_graph_new_target`・`loop_zone_edit`・`loop_zone_read`・`loop_zone_wire`・`render_reload_external`)が緑でした(実装者の run)。
- **graph root で Frame Plan にしか残らない編集**: 上の GL-5d の段落の 3 つです。
- **engine に回したもの**(実装者の報告。必須ではない): `get_frame_plan` の node に、効いている target_planning(fold group 由来かも)を出すこと。今はシナリオが `get_render_documents` の本文を文字列で探しています。
- **手で打つ欄**: 足していません(raster の欄は combo と spin、Decisions は check)。
- test: `devstudio_graphcanvas_scene_test` の [「GL-5g a raster pass of the rendering section is edited in the detail column: its shaders, defines, raster_state and draw, each change one edit_preset_pass of what changed」](../../test/devstudio_graphcanvas_scene_test.cpp#L14085)・[「GL-5g a graph node's pass shows Serial, Isolate and No alias in the detail column; each is the Frame Plan's edit_target_planning of the plan's names」](../../test/devstudio_graphcanvas_scene_test.cpp#L14161)。実装者の run で `studio_cpu` 748 / 748 が緑でした。

**pass の出力のソケット(✅GL-1f)**: engine の RS-4 で、`render.material_pass` / `render.shadow_depth` / `render.snapshot_copy` は、書いた image を出力の port として渡すようになりました(契約は [第6章](06_rendering.md) §6.18 の「グラフで書く pass」)。Graph タブはこの port を出力のソケットとして描き、fullscreen の出力と同じように線を引けます(設計は [design_graph_layers](../design_graph_layers.md) §7 の GL-1f)。

- **ソケット**:
  - `render.material_pass`: color のソケットと同名の出力(forward は `color`、G-buffer は `albedo` / `normal` / `material` / `worldpos` / `emissive`、`material_outputs` があればその名前)と `depth` です([出す所](../../src/devstudio/model/graphcanvasmodel.cpp#L2202))。`draws` は出しません。
  - `render.shadow_depth` は `depth`、`render.snapshot_copy` は `destination` です。どちらも catalog の `outputs` の `type_from_input` から出します([読む所](../../src/devstudio/model/graphcanvasmodel.cpp#L2224))。
  - 型は、同じ名前の入力のソケットにつないだ image / target_ref の型です。その入力が別の pass の出力から来ていれば、それを辿ります([`GraphCanvasModel::forwardedImageType()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2531))。何もつながっていなければ型は空です(実装者の報告では、空の型はどこへでもつなげます)。
  - 文書の `outputs` に書いていなくても、この port は出ているものとして扱います([判定](../../src/devstudio/model/graphcanvasmodel.cpp#L4547))。
- **線を引く**: この出力から graph の node の入力へ線を引くと、`set_input` の前に、書き手の node の `outputs` にその port を足す `replace_node` を同じ列で送ります。Undo 1 回で両方戻ります。既に `outputs` にあれば `set_input` だけです。
- **線を消す**: 線を消す(`set_input` を空に)、読む node を消す(`remove_node`)、body の出力を替える(`set_outputs`)ことで、その port を読むものが同じ scope に無くなれば、列の後ろに `outputs` から外す `replace_node` を足します。同じ scope のほかの node の入力か definition の出力が読んでいれば外しません。
  - 足す・外すは、Graph タブが送るすべての編集の列に掛かります([`GraphCanvasModel::withWriterOutputs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L2346)。[掛ける所](../../src/devstudio/model/graphcanvasmodel.cpp#L2342))。engine は `outputs` に無い port の読みを `unknown graph output port: <node>.<port>` で断りますが、読まない port は書かなくても通します。外すのは文書を綺麗に保つためです(設計の決定)。
- **`present` へ落とす**: 上の「Graph から画面に出す」の入力のソケットへ、この出力も落とせます。`present` の `input` には、その出力が解決される image の名前(ソケットにつないだ `render.image` の id など)を書きます。graph の文書は変わらず、material pass の `outputs` にも足しません(rendering 節の pass は名前で読むためです。コードで確かめました)。
  - 実線は、その image を書いた pass の出力から引きます(上の「線」の元の 1)。fullscreen の出力でも同じです。
- **image node からの線**: image node の `resource` から読む既存の文書は、今までどおり描き、編集できます。
- **palette**: palette の material pass は `outputs: []` で来ます([空の `outputs` を書く所](../../src/devstudio/model/graphcanvasmodel.cpp#L3284))。読む node ができたときに足します。draw ループ付きの形(✅GL-1d)は変わりません。
- GL-1f の前は、material pass に出力のソケットが無く、書いた image は image node か `render.target_ref` から横に線を引いて読みました。
- test: `devstudio_graphcanvas_test` の [「GL-1f a material pass, a shadow_depth and a snapshot_copy hand on what they wrote; a line from one lists the port in the writer's outputs, and the last one gone takes it out」](../../test/devstudio_graphcanvas_test.cpp#L3414)(3 種の出力と型、型の合わない線は引けないこと、線と `outputs` の足しが 1 つの列で Undo 1 回、2 本目の読みは `set_input` だけ、最後の読みを消すと外すこと)。LP-4e の [「LP-4e a material pass's sockets come from its contract, and the palette places one」](../../test/devstudio_graphcanvas_test.cpp#L1783) は、出力が空であることから `color` / `depth` があることへ変わりました。GL-1e の scene の test は、線の元が pass の出力であることと、pass の出力を `present` に落とす節を足しました。

**render.image の値を選んで決める(✅UX-2)**: Graph タブで `render.image` を選ぶと、詳細表の上(Loop・Draw・View の欄と同じ並び)に image の欄(`pelican.graphCanvasImage`)が出ます([`GraphCanvasImageFields`](../../src/devstudio/view/graphcanvasrenderfields.hpp#L117))。どの値も選んで決め、JSON や型名を打ちません(利用者の指摘「生 json とか生の型を書くよりかは、なるべく選択したい」)。

- **Format**(`pelican.graphCanvasImage.format`): engine の `get_render_authoring_context` の `render_target_vocabulary.formats`(上の「レンダー設定を編集する」)から選びます。depth の型なら depth の format(`D` と数字で始まる名前)だけ、それ以外の型なら depth 以外だけです([`GraphCanvasModel::imageFormatChoices()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7194))。
- **Type**(`pelican.graphCanvasImage.semantic`): graph catalog の型のうち constructor が `image` のものを、人の読める名前(`pelican.render.color_signal@1` → `Color signal`)で並べます。`legacy_untyped` の型は、今の値のときだけ出ます([`GraphCanvasModel::imageTypes()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7133))。
  - その下に、型の引数ごとの combo(`pelican.graphCanvasImage.argument.<名前>`)が出ます。取りうる値は catalog の型 schema の `parameters[].allowed` で、EL-1 の `types[].arguments.<名前>.values`(上の「レンダー設定を編集する」の EL-1 の段落)があればそれが優先です。既定値のある引数は先頭が `(default: <値>)` で、選ぶと鍵を消します。一覧の無い引数(`{min, max}` だけのもの)の combo は、既定値の項目と文書の値だけです([引数の欄を組む所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1026))。
  - 型を Depth と Color の間で替えると、format と usage も一緒に動きます。format は engine の一覧から depth なら `D32_SFLOAT`、color なら `R16G16B16A16_SFLOAT`(一覧に無ければ同じ側の format)を選び、usage の `COLOR_ATTACHMENT` と `DEPTH_STENCIL_ATTACHMENT` を入れ替えます。前の型と共通で新しい型が受ける引数は残します([`GraphCanvasModel::imageParametersWithSemantic()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7209))。
- **Usage**: `render_target_vocabulary.usage` の flag ごとの check(`pelican.graphCanvasImage.usage.<FLAG>`)です。
- **Size**: `Screen ×`(`extent_scale` の spin)か `Fixed`(`width` / `height` の spin)の radio と、`Layers` の spin です。Fixed のときは `extent_scale` を 1 にして `width` / `height` を書き、Screen × に戻すと `width` / `height` を消します。`layers` は 2 以上か、文書が既に持っているときだけ書きます([`GraphCanvasImageFields::commitSize()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1200))。
- どの変更も、node の parameters 全体を 1 回の `set_node_parameters` で送ります(Undo 1 回)。欄が知らない鍵はそのまま送ります([`GraphCanvasModel::setNodeParameters()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7044))。送れなかった(送信中、何も変わらない)ときは、欄が文書の値に戻ります。
- ✅PS-1s から **Lifetime**、✅IA-1s から **Initial contents** と **Import…**、**Size from the image** の欄があります(下の「一度だけ走る pass と、frame をまたいで残る image(✅PS-1s)」と「画像ファイルを image の初期内容に(✅IA-1s)」)。
- vocabulary は、届いていなければ接続ごとに 1 回だけ `get_render_authoring_context` を読んで取ります([`GraphCanvasModel::askRenderTargetVocabulary()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7099))。vocabulary を返さない engine では、Format は打てる combo(Enter で送る)、Usage は flag を `,` か空白で区切って打つ欄(`pelican.graphCanvasImage.usageText`)になり、欄の下に `The engine did not list <何>: those fields take what is typed, or show the document's value.` が出ます([欄を読み込む所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L628))。

**render pass の shader を選ぶ(✅UX-2)**: `render.fullscreen` / `render.raster` を選ぶと、pass の欄(`pelican.graphCanvasPass`)が出ます([`GraphCanvasPassFields`](../../src/devstudio/view/graphcanvasrenderfields.hpp#L253))。

- **Vertex** / **Fragment**(`pelican.graphCanvasPass.shader.vertex` / `.shader.fragment`): Pass Editor の Pick... と同じ一覧(project の `shaders/` と engine の fullscreen shader。下の「shader は一覧から選べます(✅T-1)」)から選びます。一覧は欄を出す直前に読み直します([一覧を渡す所](../../src/devstudio/view/graphcanvaswidget.cpp#L8431))。pass が `shader` にその段を持たなければ、その行は出ません。選ぶと `set_node_parameters` 1 回です。
- **Ports**: `ports` の宣言を 1 行ずつ読むだけで出します(`<名前>: kind access binding attachment`)。入力のソケットは shader の宣言から出るので(✅VI-1 / Q-19)、つなぐのは canvas の線です。
- ✅PS-1s から **Schedule**(Every frame / Once)と、once の pass の **Recompute** の行があります(下の「一度だけ走る pass と、frame をまたいで残る image(✅PS-1s)」)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-2 a render pass's shaders are picked from the Pass Editor's list, its ports shown as declared, and its shader pill opens the fragment list」](../../test/devstudio_graphcanvas_scene_test.cpp#L8634)。

**値の帯は一覧を開きます。打つ道は詳細表の JSON です(✅UX-2)**:

- 選べる値の帯は、押すとすぐその下に一覧の combo が開きます(打つ欄ではありません。[開く所](../../src/devstudio/view/graphcanvaswidget.cpp#L3316))。対象は `render.image` の `format` と `type`、`render.fullscreen` / `render.raster` の `shader`(fragment)です([`GraphCanvasModel::valueChoices()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7256))。type の項目は、上の型の替え方で format と usage も動かした parameters を持ちます。
- JSON を直接書く道は残ります。詳細表の parameters の行の値をダブルクリックすると、その鍵の値を JSON で打てます。JSON でなければ送らずに notice が `<key> was not changed: its value is not JSON.` を言います([打つ所](../../src/devstudio/view/graphcanvaswidget.cpp#L8372))。詳細表はほかの行では編集になりません。✅UX-12a から、catalog の表に載っている鍵は右の欄(下の「node の欄を catalog の表から作る」)でも書けます。
- ✅UX-12a から、帯のその場編集の部品は、catalog の `parameters` の表の kind を先に見て決めます([表の kind を先に見る所](../../src/devstudio/view/graphcanvaswidget.cpp#L3779))。`bool` は check、`number` / `integer` は spin(`integer` は小数無しで、整数のまま送ります。engine は `2.0` を整数として受けないため。コードの注記)、`string` / `asset` / `node_ref`(と `fields` の無い `shader`)は打つ欄、`enum` は表の `enum` の値の一覧、ほかの kind は JSON の欄です。文書にまだ値が無く表に `default` があれば、その値から始めます。今の値の JSON の型から推すのは、表に無い名前のときだけです。
- test: `devstudio_graphcanvas_scene_test` の [「UX-2 a render.image's format, type, usage and size are picked in its fields, each one set_node_parameters; its pills open the list; the details still take JSON; an engine without the vocabulary falls back to text」](../../test/devstudio_graphcanvas_scene_test.cpp#L8475)。GPU シナリオは `graph_from_blank_forward` の後半です(下の「Studio の自動化面」)。

**palette で置ける render node(✅UX-3)**: palette は、ports が parameters で決まる render node も、engine がそのまま受ける種(最初の値)を入れて置きます。どれも 1 回の編集(Undo 1 回)です([種を入れる所](../../src/devstudio/model/graphcanvasmodel.cpp#L3092))。UX-3 の前は、`render.fullscreen` と material pass と draw step のほかは palette に出ませんでした。

| op | palette で選ぶもの | 置く形 |
|---|---|---|
| `render.target_ref` | 宣言済みの target の combo(`pelican.graphCanvasPaletteTarget`。`get_render_authoring_context.render_targets[]` から graph 自身の `render.image` を除いたもの。tooltip は `declared by: <source>`) | id は `<target>_ref`。`type` は宣言の `type`、無ければ format(depth か)と `dimension: cube` / `layers` から作ります([`GraphCanvasModel::targetRefType()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7324))。✅GL-5b から combo の最後は **New render target…** で、選んで OK を押すと target を宣言してから置きます(下の GL-5b の段落)。`edit_render_targets` を送れない接続で宣言も無ければ OK を押せず、`No render target is declared yet: use render.image.` が出ます(GL-5b の前は、宣言が無ければいつもこうで、文は `… declare one (Frame Plan > New render target...) or use render.image.` でした) |
| `render.image`(単独) | 無し | 上の欄の既定: Color signal(scene / linear / extended)・`R16G16B16A16_SFLOAT`・画面の大きさ・usage `COLOR_ATTACHMENT` / `SAMPLED`。view の軸で回すループの body では、family の大きさです |
| `render.buffer` / `render.attribute` | element の combo・attribute の domain の combo(vertex / instance / index)・数の spin | buffer は `size` = 数 × element の byte 数(3 要素は 4 要素分)、attribute は `domain` と `count`。domain が index なら element は `uint` です |
| `render.raster` | 無し | 上の Add fullscreen pass と同じ形(`engine://fullscreen` + `engine://passthrough`、自前の image)に `draw: {vertex_count: 3}`。Pass Editor の一覧にその 2 つの shader があるときだけ palette に出ます |
| `render.compute` | UX-3 の着地時は無し。✅UX-5d / EL-5s から shader の combo(`pelican.graphCanvasPaletteComputeShader`。下の「project の compute / RT の shader を palette で置く」)、✅CT-6s から その横の **New…**(`pelican.graphCanvasPaletteComputeNew`。下の「RT を Graph で組む」) | UX-3 の着地時は、project の `shaders/*.comp` の最初(`project://shaders/compute` があればそれ)、`dispatch.groups` `[1, 1, 1]`、`ports` は空で、compute shader の無い project では palette に出ませんでした。今の置き方は EL-5s の段落のとおりです。行は、project の `.comp` か RT の組があるか、engine の shader の一覧がまだ届いていないか、engine に RT pipeline があれば出ます([palette に出す条件](../../src/devstudio/model/graphcanvasmodel.cpp#L4962))。Blank の project でも、RT pipeline を持つ engine なら出ます |
| `render.snapshot_copy` | `snapshot_after` の pass の combo(`pelican.graphCanvasPaletteSnapshotAfter`。plan の pass。既定は最後の pass) | `source` はその pass が書いたもの(root の node の出力、image node、宣言済みの target。target なら `render.target_ref` を同じ列で足す)。何も分からなければ自前の image `<id>__source` を足します。`destination` は source と同じ型・format・大きさの image `<id>__copy`(usage `TRANSFER_DST` / `SAMPLED`)を同じ列で足します([`GraphCanvasModel::snapshotCopySeedOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7452))。plan に pass が無ければ OK を押せません |
| `render.shadow_depth` | 無し | depth の image `<id>__depth`(`pelican.render.depth@1`・`D32_SFLOAT`・画面の大きさ)を同じ列で足して `depth` につなぎます |

- 種の combo と spin は palette の下に出ます([palette の欄](../../src/devstudio/view/graphcanvaswidget.cpp#L9026))。一覧は行を選ぶたびに読み直します(palette を開いた後に authoring context が届くことがあるため)。palette を開くときに authoring context が無ければ読みに行きます。
- draw_loop が走らせる body には、これらの op は出ません。
- どの op を palette に出すかは [`GraphCanvasModel::paletteOperationsIn()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4926) です。
- test: `devstudio_graphcanvas_scene_test` の [「UX-3 decision 1: the palette places target_ref, image, buffer, attribute, raster, compute, shadow_depth and snapshot_copy seeded, each in one edit the engine's graph validation takes」](../../test/devstudio_graphcanvas_scene_test.cpp#L8744)。GPU シナリオは `graph_palette_kinds`(下の「Studio の自動化面」)です。

**render node の置き場所 —— Place(✅UX-3)**: `render.fullscreen` / `render.raster` / `render.material_pass` / `render.shadow_depth` / `render.snapshot_copy` / `render.compute` を選ぶと、Place の欄(`pelican.graphCanvasPlace`)が出ます([`GraphCanvasPlaceFields::load()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2360))。UX-3 の前は、置き場所の欄はループの Loop の欄にしかありませんでした。

- **Place**(`pelican.graphCanvasPlace.place`): `automatic`(`insert` も `canonical_anchor` も書かない)、plan の pass ごとの `before <pass>` / `after <pass>`(anchor・ループの回・自分は除く)、`after anchor <anchor>`(`canonical_anchor`)です。文書の値が一覧に無ければ(plan にまだ無い pass、欄が表せない `insert`)、その値のまま 1 項目足して出します。
- **Family**(`pelican.graphCanvasPlace.passSet`): `(the frame's first)`(`pass_set` を書かない)と、authoring context の `graphs[].name` です。
- 選ぶと `insert` / `canonical_anchor` / `pass_set` を書き替えた parameters で `set_node_parameters` 1 回です。`insert` と `canonical_anchor` は片方だけ書きます。material pass が同じ場所のままなら `insert.declared: true` を残します([`GraphCanvasModel::setPlace()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7605))。
- compute task は全部の pass の後に走るので(graph の `insert_on_compute`)、欄は押せず、`A compute task runs after every pass; it takes no place.` が出ます。
- test: [「UX-3 decision 2: a render node's Place and family are picked, each one set_node_parameters; a compute task takes none」](../../test/devstudio_graphcanvas_scene_test.cpp#L8867)。

**入力ソケットの右クリック —— 読み方の設定(✅UX-3)**: `render.fullscreen` / `render.raster` / `render.compute` の入力のソケットを右クリックすると、メニューの先頭に `Socket <port>` の節が出ます。今まで `ports` の JSON に書いていたものを、ここで書きます([節を足す所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2659)、[メニューに入れる所](../../src/devstudio/view/graphcanvaswidget.cpp#L3436))。`ports` にその port の宣言が無ければ節は出ません。

| 項目(`pelican.graphCanvasSocket.<名前>`) | 書く鍵 | 押せるとき |
|---|---|---|
| **Previous frame**(`history`。check) | `history: true` | image を読む port で、attachment でない |
| **Filter**(`filter`。`default (linear)` / `linear` / `nearest`) | `sampling.filter` | image を読む port で、attachment でない(fullscreen / raster では storage でない、compute では `binding: sampled`)。✅UX-7 から番号付きの入力も押せます(UX-7 の前は「番号付きの入力でもない」も条件でした) |
| **Address**(`address`。`default (repeat)` / `repeat` / `mirrored_repeat` / `clamp_to_edge`) | `sampling.address` | 同上 |
| **Mip level** / **Layer**(`mip` / `layer`。spin) | `subresource.mip` / `.layer`(0 は鍵を消す) | image で、attachment でない(✅UX-7 から番号付きの入力も) |
| **Attachment location**(`location`。spin) | `location` | fullscreen / raster の color attachment |

- 押せない項目は灰色で、tooltip が理由を言います(`Only a colour attachment has a location.` など)。押せる条件は engine の graphmodule / rendergraphcomposition の規則をコードに写したものです(コードの注記。✅UX-7 で engine の RS-14 の後の規則に合わせました。下の「pass の欄の Inputs」。[`GraphCanvasModel::portSettings()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7635))。
- どの変更も、その時点の設定に 1 つだけ変えて `set_node_parameters` 1 回です([`GraphCanvasModel::setPortSettings()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7683))。
- test: [「UX-3 decision 3: a render node's socket menu writes its port's previous frame, sampling, mip / layer and location, each one set_node_parameters」](../../test/devstudio_graphcanvas_scene_test.cpp#L8919)。決定 4・5(Rename の提案、depth_compare、EL-1 の一覧)は [「UX-3 decisions 4 and 5: the engine's materials, tags and variants are picked in the draw_loop's fields, the draw steps' and the material pass's; draw.override's depth_compare is a combo」](../../test/devstudio_graphcanvas_scene_test.cpp#L9107) です。

**pass の欄の Inputs —— 読む image ごとの Filter / Address / mip(✅UX-6a)**: `render.fullscreen` / `render.raster` を選んだときの pass の欄(`pelican.graphCanvasPass`。上の「render pass の shader を選ぶ」)に **Inputs** の行が増えました。上の右クリックの節は残ります。UX-6a の前は、読み方の設定は右クリックの節にしか無く、シナリオから操作できませんでした(コードの注記)。

- 行は、`ports` の宣言のうち `kind: image`・`access: read` で `attachment` の無い port ごとに 1 行です。左が port の名前で、右に combo が 3 つ並びます([`GraphCanvasPassFields::loadInputSettings()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1910))。
  - **Filter**(`pelican.graphCanvasPass.input.<port>.filter`): `default (linear)` / `linear` / `nearest`
  - **Address**(`.address`): `default (repeat)` / `repeat` / `mirrored_repeat` / `clamp_to_edge`
  - **mip**(`.mip`): `mip 0`〜`mip 15`
- 押せる条件は右クリックの節と同じ `GraphCanvasModel::portSettings()` です。押せない combo の tooltip は、Filter / Address が `Only an image the shader samples by a binding of its own takes sampling.`、mip が `An attachment takes no mip of its own here.` です。
- **番号付きの入力も押せます(✅UX-7、`1fcd155e`)**: engine の ✅RS-14 が番号付きの入力(`binding: "input"`。shader の宣言から出るソケット)にも名前付きの port と同じ `sampling` / `subresource` を通すので([第6章](06_rendering.md) §6.18「番号付きの入力 —— `binding: "input"` と `input_index`」の RS-14 の段落)、Studio の判定から `binding != "input"` を外しました([判定の所](../../src/devstudio/model/graphcanvasmodel.cpp#L7660))。書く場所は名前付きの port と同じ port の宣言の `sampling` / `subresource` で、`default (…)` を選ぶとその鍵を消し、`mip 0` は `subresource.mip` を消します(何も設定しない pass の文書は前と同じ bytes)。UX-6a の着地時は、3 つとも出たまま押せず、tooltip が `A numbered input takes no sampling (the engine refuses it): it is read linear, repeat.` を言っていました。
  - **fullscreen の pass の新しい番号付きの入力は `clamp_to_edge` で書きます**: engine の既定は `repeat` のままなので、Studio が明示します。線を落として番号付きの port を作るとき(上の「render node の入力は shader の宣言から」)と、雛形の Create が入力をつなぐときに、`render.fullscreen` の port に `sampling: {address: "clamp_to_edge"}` を書きます([書く所](../../src/devstudio/model/graphcanvasmodel.cpp#L106))。`render.raster` の入力(vertex shader が読む displacement など)は既定のままです。既にある port は書き換えません。GB-2 の bloom の上端の回り込み(下端の明るい床が画面の上端に出る)は、これで `gui_build_full` の画素で消えたことを見ています(下の「Studio の自動化面」)。
- 選ぶと、その時点の設定を 1 つだけ変えて `set_node_parameters` 1 回です(右クリックと同じ `GraphCanvasModel::setPortSettings()` を通ります。[`GraphCanvasPassFields::commitInputSetting()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1978))。送れなければ、行を文書の値で作り直します。
- test: `devstudio_graphcanvas_scene_test` の [「UX-6a decision 5: the Pass fields show each read image's Filter, Address and mip as combos, each pick one set_node_parameters; UX-7 decision 1: a numbered input's are picked too」](../../test/devstudio_graphcanvas_scene_test.cpp#L9024)(UX-7 で名前と中身を書き換えました)。GPU シナリオは `gui_build_full` の段 4 と `gui_build_optimize` の bloom の body の pass の欄(`clamp_to_edge` が出て、combo が押せる)です。

**Studio が作る image は float で、TRANSFER_SRC を付けません(✅UX-6a)**: Add fullscreen pass、palette の `render.fullscreen` / `render.raster`、ループの body に足す pass が一緒に作る image(`<pass>__color`)は、`R16G16B16A16_SFLOAT`・usage `COLOR_ATTACHMENT` / `SAMPLED` です([Add fullscreen pass の image の既定](../../src/devstudio/model/graphcanvasmodel.cpp#L3359))。既にある node は変えません。

- UX-6a の前は `R8G8B8A8_UNORM` で、usage に `TRANSFER_SRC` もありました。8 bit では 1 を超える光が切れて bloom の明るい所の取り出しが効かず、engine は `TRANSFER_SRC` を「frame plan の外から読まれうる」と読んで alias の候補から外すので、Studio が作った image は alias されませんでした(GB-2 の着地時の description の記録)。
- palette の単独の `render.image`(上の「palette で置ける render node」の表)は、UX-6a の前から `TRANSFER_SRC` を付けていません。palette の `render.snapshot_copy` が自前で足す source の image(`<id>__source`)だけは、今も `TRANSFER_SRC` を付けます([snapshot_copy の自前の source の usage](../../src/devstudio/model/graphcanvasmodel.cpp#L7525))。
- image の欄の Usage の `TRANSFER_SRC` の check(`pelican.graphCanvasImage.usage.TRANSFER_SRC`)の tooltip が、付けると alias されないことを言います(`Lets the image be copied or read back from outside the frame plan. With it the image is not aliased.`。[TRANSFER_SRC の check の tooltip](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1124))。
- **Physical plan の Remove TRANSFER_SRC**: Frame Plan の Physical plan のタブに、`TRANSFER_SRC` のせいで alias されない image の組 **Not aliased for TRANSFER_SRC (N)** が出ます([`populateTransferSourceRows()`](../../src/devstudio/view/frameplanwidget.cpp#L5200))。行は名前・`Not aliasable`・plan の理由と、ボタン **Remove TRANSFER_SRC**(`pelican.framePlan.dropTransferSrc.<資源>`)です。
  - 組に入るのは、buffer でなく、plan に表現を持ち、alias できない資源のうち、理由に `usage TRANSFER_SRC is outside` を含むもの(engine の PO-1 の原因の文。上の「計画を詰める手がかり(✅PO-1)」)です。理由に `: ` の後の原因が無い(PO-1 の前の)engine では、資源の usage に `TRANSFER_SRC` があるかで決めます([`keptByTransferSource()`](../../src/devstudio/view/frameplanwidget.cpp#L5190))。
  - 押すと、Graph タブがその名前の `render.image` の node(ループの回の資源なら、`loop_origin` が指す body の node)の usage から `TRANSFER_SRC` を外し、`set_node_parameters` 1 回で送ります([`GraphCanvasWidget::removeImageTransferSource()`](../../src/devstudio/view/graphcanvaswidget.cpp#L10553))。graph の文書にその image が無い・`TRANSFER_SRC` が無い・Graph タブが今編集できないときは何も送らず、Frame Plan の status 行が `Could not take TRANSFER_SRC off <資源>: it is not an image the Graph tab can edit now.` を言います。
- test: `devstudio_frameplan_graph_test` の [「UX-6a decisions 2 and 4: the Frame Plan tells the Graph after a reload that moved declarations, and its Physical plan offers Remove TRANSFER_SRC on an image kept out of aliasing for it」](../../test/devstudio_frameplan_graph_test.cpp#L12440)。GPU シナリオは `gui_build_optimize`(下の「Studio の自動化面」)です。

**pass の欄の New… の名前は、雛形を選ぶたびに提案し直します(✅UX-6a)**: Graph の pass の欄の Fragment の横の **New…**(`pelican.graphCanvasPass.newFragmentShader`)は、雛形の combo(`.shaderTemplateKind`)・名前(`.shaderTemplateName`)・**Create**(`.createFragmentShader`)の行を開きます。雛形は Pass Editor の New… と同じです(下の「雛形は 3 種類」)。

- 名前の提案は雛形の名前(`deferred_lighting` / `tone_map`。Pass-through なら pass の id)で、project の `shaders/` に同じ名前のファイルがあれば `_2`、`_3`… を付けた空き名です([`GraphCanvasPassFields::proposeTemplateName()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2178))。欄が空か、Studio が前に提案した名前のままなら、雛形を選び直すたびに提案し直します。打った名前は上書きしません。
- ✅UX-6a から、Create で書いた名前(打った名前でも)を「Studio が提案した名前」として覚え直します([Create の後に名前を覚え直す所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2277))。そのため次の pass で雛形を選ぶと、欄は新しい雛形の名前に替わります。UX-6a の前は、Create の後も前の名前(`deferred_lighting`)が欄に残り、Tone map を選んでも替わらないので、打ち直さないと `shaders/deferred_lighting.frag is already in the project and was left as it is: ...` で断られました(GB-2 の着地時の description の記録)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-6a decision 1: after a Create, picking another template names the new shader after that template (a free name past the files already there); a typed name stays」](../../test/devstudio_graphcanvas_scene_test.cpp#L9531)。

**GB-3 で打った欄を pass と image の欄に(✅UX-7、`1fcd155e`)**: GB-3 の `gui_build_ocean` / `gui_build_ltc` が JSON を打った所と、GUI に欄が無かった所を、Graph の詳細の欄に足しました(番号付きの入力の sampling は上の「pass の欄の Inputs」)。どの欄も、変えると node の parameters 全体を `set_node_parameters` 1 回で送ります。

- **Defines**(fullscreen / raster の pass の欄): Pass Editor と同じ `ShaderDefinesForm` の表で、行が `shader.defines` の 1 つです(`pelican.graphCanvasPass.shaderDefines.table` / `.shaderDefines.add` / `.shaderDefines.remove`。上の「生の JSON の欄はフォームになりました」)。engine が断る行は赤くなり、その間は何も送りません([`GraphCanvasPassFields::commitDefines()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1779))。表が空なら `shader.defines` を消します。
- **Depth**(raster の pass の欄。`pelican.graphCanvasPass.depth`): `(none)` / scope の中の depth の image(format が `D16` / `D24` / `D32` / `X8_D24` で始まるか、type が `pelican.render.depth@1` の `render.image`)/ `New depth image (D32_SFLOAT)` から選びます([`GraphCanvasModel::setRasterDepth()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7769))。
  - New を選ぶと、1 つの編集で `<pass>__depth`(あれば `_2`…)の `render.image`(`D32_SFLOAT`、usage `DEPTH_STENCIL_ATTACHMENT` / `SAMPLED`、大きさは pass の color の image の `extent_scale` / `width` / `height` / `layers` を写す)を足し、pass に `attachment: "depth"` の port(`depth`)と、その入力の線と出力を足します。
  - depth を付けた pass は、`raster_state` が言っていなければ `depth_test` と `depth_write` を真にします。`(none)` に戻すと port・線・出力と `raster_state` の depth の鍵を消します(depth の無い pass の depth の state は engine が断るため。コードの注記)。
- **Raster**(raster の pass の欄): fullscreen の pass の欄と同じ Pass Editor の `RasterStateForm` を raster の pass にも出します(cull・blend、depth を付けた pass では depth test / write / compare)。raster の pass では `raster_state` だけを書き、Frame data と Output の行は出しません([`GraphCanvasPassFields::loadPassSettings()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2014))。
- **Draw**(raster の pass の欄。`pelican.graphCanvasPass.vertexCount` / `.instanceCount` / `.gridSize` / `.grid`): **Vertices** と **Instances** の spin、N の spin(2〜16385、既定 257)と **Grid N×N (triangles)** のボタンです。Grid は `draw.vertex_count` に 6·(N−1)² を入れて送ります(257 なら 393216)。Instances は 1 のときは鍵を書きません(既に鍵があれば書きます)([`GraphCanvasPassFields::commitDraw()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1878))。draw が `vertex_source` / `index_source` を持つか `operation: "indexed"` なら、数は source から来るので Vertices と Grid は押せません。UX-7 の前は、draw の帯に `{"vertex_count": 393219}` の JSON を打っていました(仕様の背景)。
- **Keep previous frame (history)**(`render.image` の欄。`pelican.graphCanvasImage.history`): 付けると image の parameters に `history: true`、外すと鍵を消します([check を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L485))。前の frame の image を読むのは、入力ソケットの右クリックの Previous frame(上の表)です。
- **送れないときは待って送ります**: graph が保存や読み直しの最中で編集を受けられないときに Defines / Depth / Draw / Raster を変えると、値を捨てずに覚え、編集できるようになった時に送ります([`GraphCanvasPassFields::deferWhileBusy()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1661))。✅LC-1s(`ed4ed5f9`)から、覚えている間の notice は `Saving the graph change...` です。また、Draw の spin に打って Enter も focus の移動もしないまま pass の欄が別の node に移る(描き直しで node が一瞬消える)ときは、その値を先に送り([`GraphCanvasPassFields::flushTyped()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1703)。✅UX-9(`4d11aa96`)で `flushDraw()` から替わり、`release()` から呼ばれて、打ちかけの define の cell と clear colour も送ります)、読み直しは打っている途中の spin を文書の値で上書きしません。LC-1s の前は、描き直しが打った Instances の 2 を落としていました(commit の記録)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-7 decision 2: a pass's Defines rows are its shader.defines, written in one set_node_parameters; a row the engine refuses is red and not sent」](../../test/devstudio_graphcanvas_scene_test.cpp#L10336)、[「UX-7 decisions 3 and 4: a raster pass's Depth is picked (a new D32_SFLOAT image in one edit the engine takes), its raster_state from the Raster form, and its draw counted, Grid NxN writing 6(N-1)^2 vertices」](../../test/devstudio_graphcanvas_scene_test.cpp#L10386)、[「UX-7 decision 6: a render.image keeps the previous frame by its check (history: true), one set_node_parameters each way」](../../test/devstudio_graphcanvas_scene_test.cpp#L10615)、[「UX-7 a draw count typed but not confirmed survives the Pass fields letting go, and the notice says Saving at once」](../../test/devstudio_graphcanvas_scene_test.cpp#L10502)(LC-1s で足した 1 本)。GPU シナリオは `gui_build_ocean`(Grid 257 と Instances 2、New の depth と depth compare `less_equal`)です。

**palette で置くときに、要る parameter を先に聞きます(✅UX-7)**: palette で `render.scalar`・`value.constant`・`value.input`(✅UX-8 から、engine の catalog が必須を出していればその op。`audio.volume` / `audio.pitch` も)を選ぶと、置く前に小さなダイアログ `<op> needs`(`pelican.graphCanvasRequired`)が出ます([必須のダイアログを開く所](../../src/devstudio/view/graphcanvaswidget.cpp#L9413))。OK(`.ok`)で、答えを parameters に入れた node を 1 回の編集で置きます(`voice` は parameter ではなく、選んだ `audio.playback` の `voice` からの線です)。Cancel(`.cancel`)なら置きません。後から node の欄で直せます。✅UX-12a から、聞く op と欄は engine の catalog の `parameters` の表(✅CT-3)から作ります(下の表の後の項)。

| op | 欄(`pelican.graphCanvasRequired.<名前>`) | 選び方 |
|---|---|---|
| `render.scalar` | `target` | root と今の scope の `render.buffer` の id(✅UX-8 から、frame plan の buffer —— `bloom.intensity` のような feature の buffer —— も。`@` を含む名前は出しません)から選ぶか、新しい名前を打つ combo。候補が 1 つも無ければ `value_buffer` が入っています。空の間は OK を押せません |
| `value.constant` | `value` | 表の kind(`number`)の spin。表に `default` が無いので 0 から始まります(✅UX-12a) |
| `value.input` | `name` | catalog の `input_names` の名前で、`scene_time` が先頭(✅UX-8)。catalog が出さなければ Studio の写しの `scene_time` / `time` / `delta_time` |
| `audio.volume` / `audio.pitch` | `target` か `voice`(どちらか 1 つ) | ✅UX-12a から、表の `target` の `alternatives`(`["voice"]`)に従って、`target`(`.target.choose`)と `voice`(`.voice.choose`)の radio で選びます。`voice` は scope の `audio.playback` の id の combo で、下に `or connect a line to voice after placing` が出ます。scope に `audio.playback` があれば `voice` が先に選ばれています |
| `value.parameter` | `name` / `default` | ✅UX-8。`name` は打つ欄(scope で空いている id の提案が入っている。空の間は OK を押せない)、`default` は数の spin |
| `pose.clip_sample` | `rig` / `clip` | ✅UX-12a(engine の CT-3 で必須になった 2 つ)。`rig` は project の `.gltf` / `.glb` / `.vrm` の combo と **Import…**(`.rig.import`)。`clip` は、選んだ rig の file の animation の名前の combo で、rig を選び直すと読み直します |

- 聞く op と欄は、✅UX-12a から catalog の各 op の `parameters` のうち `required` の付いたものです([`GraphCanvasModel::requiredParametersFor()`](../../src/devstudio/model/graphcanvasvalues.cpp#L754))。palette が自分で種を入れる op(`value.curve` の keys、`audio.playback` の asset、`ui.*` の node、`render.scalar` 以外の `render.*` と `draw.*`)は今までどおり聞きません。欄は node の欄と同じ部品(下の「node の欄を catalog の表から作る(✅UX-12a)」の `GraphParameterEditor`)を表の kind で選び、Studio が表より多くを知る 3 つだけ自分の部品にします: `render.scalar` の `target`(buffer の combo)、`value.input` の `name`(名前の combo。catalog の `input_names` から。[`GraphCanvasModel::valueInputNamesFor()`](../../src/devstudio/model/graphcanvasvalues.cpp#L841))、`pose.clip_sample` の `clip`(rig の animation)。`default` の無い数は 0(`integer` は表の `min`)、bool は false から始まるので、OK で見えている値のまま置けます([必須のダイアログを作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2429))。`alternatives` を持つ必須は「どちらか 1 つ」の radio で、op の入力 port の名前でもある候補(`voice`)は線で満たします。OK は、選んだ側の欄が空の間は押せません。
- UX-12a の前は、`required_parameters`(✅CT-1)を名前の規則で欄にしていて(`value.morph_weight` の `node` / `target` も数の spin になる規則でした)、catalog がそれを出さない engine では UX-7 の Studio の表(`render.scalar` / `value.constant` / `value.input` の 3 つ)を使っていました。UX-12a でこの表は消え、catalog が `parameters` を出さない engine ではダイアログは出ません(コードの読み)。今の規則では `value.morph_weight` の `node` / `target` は `integer` の spin(表の `min` の 0 から)です。CT-4 から `value.morph_weight` は owner のある instance の中にしか置けない op です(engine の CT-4 の commit の記録)。palette での見え方は確かめていません(未確認)。
- UX-7 の前は、palette から置いたこれらの node は parameters が空で、engine に名前で断られ(RS-15 からは `graph.render_node_parameters_missing@1`)、GUI で書く欄が無いので `gui_build_ltc` は面光源の角を shader の定数にしていました(シナリオの description の記録)。
- **自動化での書き方**: graph の node の parameter は、canvas の値の帯に打って書くか、✅UX-12a から右の欄(`pelican.graphCanvasParameters.<名前>`、`pose.clip_sample` は `pelican.graphCanvasPose.<名前>`)で書きます。詳細表の行の編集欄は自動化から開きません(表の editor は名前を持たないため)。UX-12a の前は、まだ持っていない鍵には帯が無いので、render node の鍵は専用の欄で、上の 3 つはこのダイアログで書いていました(UX-7 の決定 8。[README の「A node's parameters」](../../test/studio_scenarios/README.md#L727)。README は UX-12a で変わっていません)。
- test: [「UX-7 decision 5: render.scalar, value.constant and value.input ask for what they cannot be placed without, and are placed with it」](../../test/devstudio_graphcanvas_scene_test.cpp#L10549)、✅UX-12a から [「UX-12a decision 2: the palette's dialog is made from the table -- pose.clip_sample asks for its rig among the project's glTF files and its clip among the rig's animations, and is placed with them」](../../test/devstudio_graphcanvas_scene_test.cpp#L11822)。

**ループと分岐の後に足した node も描きます(✅UX-7)**: GB-3 で、ループと分岐を組んだ後に palette で足した `render.raster` が、文書にも frame plan にも入るのに canvas に描かれず、Refresh でも出ないことが 3 回ありました。canvas は値の帯のその場編集の editor が開いている間は描き直しを待っていて、その pick が帯に当たって editor が開いたままになっていたためです(シナリオの description の記録)。今は、文書の中身が変わった読みなら editor が開いていても描き直し、editor と打った字を新しい描画へ持ち越します([描き直しの判定](../../src/devstudio/view/graphcanvaswidget.cpp#L9839))。中身の変わらない読み(配置や報告だけ)は今までどおり editor を待ちます。test は [「UX-7 decision 7: a node the document gains is drawn while a pill's editor is open, the editor carried over the new drawing」](../../test/devstudio_graphcanvas_scene_test.cpp#L10644) です。

**ループの body の pass は回の番号を読めます —— 雛形と印(✅LC-1s、`ed4ed5f9`)**: engine の ✅LC-1 で、body の shader は `#include "pelican_loop.glsl"` の `pelican_loop_element()` / `pelican_loop_count()` で自分が何回目かを読めます(書き方の正は [第6章](06_rendering.md) §6.18「回の番号を shader に渡す」)。Studio はその入口と見え方を足しました。

- **雛形**: Graph の pass の欄の **New…** で、**ループの body の中**(分岐の body を含む。[`GraphCanvasModel::isLoopBodyScope()`](../../src/devstudio/model/graphcanvasmodel.cpp#L3991))の pass に雛形を作ると、`#include "pelican_loop.glsl"` と 1 行のコメント `// pelican_loop_element(): this element, 0-based; pelican_loop_count(): how many` を、`#include "pelican_sets.glsl"` の次の行(無ければ `#extension GL_GOOGLE_include_directive` か `#version` の次の行)に入れます([`withLoopElementInclude()`](../../src/devstudio/view/fullscreenshadertemplate.cpp#L365)、[呼ぶ所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2250))。既に include していれば何もしません。ループの外では雛形は変わりません。
  - 雛形はどれも自分の push constant の block を宣言しないので、`PELICAN_LOOP_CUSTOM_PUSH` の形にはしません(コードの注記)。Pass Editor の New… は include を入れません(呼ぶのは Graph の pass の欄だけ)。New… はもともと fullscreen の pass の欄にだけ出ます。
- **印**: frame plan の node の `reads_loop_index: true`(§13.5 の「`get_frame_plan` に足されたもの」)を読み、その node が作った pass のどれかが読めば([`GraphCanvasModel::loopElementRead()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4000)):
  - body の pass の node の下に、GL-2b の帯の位置で `reads element` を描きます。root のゾーンの中の body も、body に入った canvas も同じです([帯を足す所](../../src/devstudio/view/graphcanvaswidget.cpp#L1350))。ゾーンの帯は、同じ名前の root の node の GPU 時間(上の「速くしたかが見える」)を取りません。
  - pass の欄に `This shader reads the loop element (pelican_loop_element)` の 1 行(`pelican.graphCanvasPass.loopElement`)。ループに複製されない pass が読んでいれば `…; outside a loop it reads element 0` です([1 行を出す所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L1637))。plan が文書より後に来ることがあるので、欄を出すたびに読み直します。
- **拒否の 1 文**: engine の `shader.loop_push_overlap@1` を、Graph の拒否の 1 文の表に足しました: `<shader> declares a push constant that overlaps the loop element at <offset>..<offset+size> in the <stage> stage — use PELICAN_LOOP_CUSTOM_PUSH and PELICAN_LOOP_PUSH_FIELDS`([表の行](../../src/devstudio/model/inspectormodel.cpp#L1286)、[range を `a..b` にする所](../../src/devstudio/model/inspectormodel.cpp#L1737))。
- test: `devstudio_graphcanvas_test` の [「LC-1s a pass whose shader reads the loop element is known by its body node」](../../test/devstudio_graphcanvas_test.cpp#L3543) と [「LC-1s shader.loop_push_overlap@1 reads as a sentence」](../../test/devstudio_graphcanvas_test.cpp#L3571)、`devstudio_shadertemplate_compile_test` の [「LC-1s a template made in a loop's body includes pelican_loop.glsl and compiles」](../../test/devstudio_shadertemplate_compile_test.cpp#L546)。GPU シナリオは `gui_build_ocean`(FFT の段を 1 本の body の `ocean_fft.frag` が `pelican_loop_element()` で選ぶ。下の「Studio の自動化面」)です。

**曲線を shader の値へ GUI でつなぐ —— `render.buffer` の value と pass の Buffers(✅UX-8、`cabf7691`)**: GB-4 の `gui_build_music` は、曲線の値を shader に渡す道で止まりました。engine では動くことを統合側が手で JSON を書いて確かめていて(`value.curve` → `render.buffer`(float)の `value` → fullscreen pass の名前付きの buffer の port → shader。仕様の背景)、UX-8 でその手書きの所が線を引く操作になりました。

- **`render.buffer` の value のソケット**: `render.buffer` / `render.attribute` の node は、入力の先頭に `value` のソケットを描きます。engine はこの 2 つに任意の scalar の入力 `value` を持たせていて、その op の port は parameters から決まるので catalog に載りません。そのため Studio が足して描きます(コードの注記。[render.buffer に value のソケットを足す所](../../src/devstudio/model/graphcanvasmodel.cpp#L2090))。
  - 値の node(`value.curve` など)の出力をここへ落とすと `set_input` を送ります。`render.buffer` の `size` が 4 でなければ、同じ編集で `size: 4` にします(値の線は float 1 つを書くため)。
  - element が `float` でなければ送らず、`<node> holds <element>: a value's line writes one float. For two to four numbers, write them with a render.scalar.` を言います([value の線を 1 つの float に限る所](../../src/devstudio/model/graphcanvasmodel.cpp#L2924))。
- **pass の欄の Buffers**(`pelican.graphCanvasBuffers`。fullscreen / raster の pass): shader の宣言(Q-19 のソケットと同じ `pass_inputs`)の buffer の binding を 1 行ずつ並べます。線があれば `binding <b> <宣言> ← <node>.<port> (port <port>)`、無ければ `binding <b> <宣言>: drag a render.buffer's resource onto its socket` です。宣言が取らない名前付きの buffer の port も並べます([`GraphCanvasValueFields::loadBuffers()`](../../src/devstudio/view/graphcanvasvaluefields.cpp#L693))。UX-8 では `<port> ← <node>.<port>` の形でした。✅UX-13a から、線があり pass の宣言が読めていれば、engine がその port を bind する番号を付けて `binding <b> pelican_resource_<port> ← <node>.<port>` と出します(番号の決め方は下の「buffer の binding は engine の規則で(✅UX-13a)」)。線が無い port は `<port> (no line)` です。
- **buffer の宣言のソケットへ線を落とす**: `render.buffer` の `resource` を pass の buffer の宣言の ○ へ落とすと、名前付きの buffer の port(`{"kind": "buffer", "access": "read", "element": <buffer の element>}`)の `set_node_parameters` と、その port への `set_input` を 1 回の編集で送ります([`GraphCanvasModel::bufferPortOps()`](../../src/devstudio/model/graphcanvasvalues.cpp#L359))。
  - `element` は必ず書きます(engine の CT-1 は欠けると `graph.port_field_missing@1` で断るため)。
  - port の名前は、宣言の変数名から `pelican_resource_` を外したものです(`pelican_resource_level` なら `level`。[`GraphCanvasModel::bufferPortName()`](../../src/devstudio/model/graphcanvasvalues.cpp#L342))。engine は名前付きの port を `pelican_resource_<port>` の変数にだけ割り当てるからです(コードの注記)。
  - 断るもの(送らず、notice に 1 行): storage でない buffer の宣言は `Binding <b> of <node> is a <型>: a pass hands its shader storage buffers (declare it as a `readonly buffer` block).`、変数名が `pelican_resource_` で始まらない宣言は `… the engine reads a buffer port through a variable named pelican_resource_<port>. Name the block's variable so, or read it through pelican_resource_ports.glsl and + Buffer port.`、buffer でない資源は `<source> hands on no buffer: a buffer port reads a render.buffer or a render.attribute.` です([線を落とした buffer の宣言を名前付きの port にする分岐](../../src/devstudio/model/graphcanvasmodel.cpp#L5450))。
  - 外す: 名前付きの buffer の port の線を外すと、port の宣言も同じ編集で消します(線の無い port は engine が割り当てないため。[線と一緒に buffer の port を消す所](../../src/devstudio/model/graphcanvasmodel.cpp#L2957))。
- **+ Buffer port**(`pelican.graphCanvasBuffers.add`): shader が `pelican_resource_ports.glsl` で読む buffer は、port ができるまで反映に出ません。押すと、名前(`.name`。打てる combo)と読む buffer(`.source`。scope の `render.buffer` / `render.attribute`)の行が出て、**Add**(`.apply`)で上と同じ op の列を送ります(読む出力は `resource`)。名前の候補は、pass の shader が `project://shaders/...` なら、その file(拡張子無し・`.frag`・`.vert`・`.glsl`)の中の `PELICAN_RESOURCE_PORT_<名前>` / `pelican_load_<名前>` / `pelican_sample_<名前>` から拾います([`shaderResourcePortNames()`](../../src/devstudio/view/graphcanvasvaluefields.cpp#L784))。✅UX-13a から変数そのもの `pelican_resource_<名前>` も拾い、include の名前の `pelican_resource_ports` と engine の関数 `pelican_sample_table` は拾いません。
- **palette の初期値**: `value.curve` は `keys: [{time: 0, value: 0}, {time: 1, value: 1}]`(最初の 1 秒で 0 から 1)で置かれ([曲線を 0→1 の ramp で置く所](../../src/devstudio/model/graphcanvasmodel.cpp#L3586))、`value.curve` の `time` と `value.playback` の `elapsed` は Q-11 の `audio.playback` の `time` と同じく scope の `scene_time` につながって来ます([曲線と playback を scene の時計につなぐ所](../../src/devstudio/model/graphcanvasmodel.cpp#L3554))。
- test: `devstudio_graphcanvas_test` の [「UX-8 decisions 1 and 3: a curve from the palette is a ramp on the scene clock, and its line lands on a float buffer's value socket」](../../test/devstudio_graphcanvas_test.cpp#L3686)、[「UX-8 decision 2: a buffer binding of the shader is a named buffer port, added with its line and its element in one list; taking the line away takes the port」](../../test/devstudio_graphcanvas_test.cpp#L3750)、`devstudio_graphcanvas_scene_test` の [「UX-8 decision 2: a pass's Buffers list its shader's buffer bindings, and + Buffer port offers the names its shader reads through the port header」](../../test/devstudio_graphcanvas_scene_test.cpp#L10907)。GPU シナリオは `gui_build_curve`(下の「Studio の自動化面」)です。

**曲線の Keys —— 表・小さな曲線・Timeline の点(✅UX-8)**: Graph で `value.curve` を選ぶと、値の欄に **Keys** の節(`pelican.graphCanvasCurve`)が出ます。曲線は文書の `parameters.keys` に直に書きます(資産にはしません。仕様の利用者の決定)。

- **表**(`.keys`): 1 行が key 1 つで、Time と Value の数の spin(`.key<行>.time` / `.key<行>.value`)、Interpolation の combo(`.key<行>.interpolation`)、×(`.key<行>.remove`。key が 1 つなら押せない)です。補間は engine の `value.curve` が受ける `linear` / `step` / `cubic` です([`GraphCanvasModel::curveInterpolations()`](../../src/devstudio/model/graphcanvasvalues.cpp#L126))。spin は確定(Enter か focus の移動)で送ります。
  - **+ Key**(`.add`)は、選んだ行(無ければ最後の行)の後に、次の key との中間(最後なら 1 秒後)の時刻で、そこでの曲線の値の key を足します。値は cubic の区間も直線として読んだものです(コードの注記)。
  - `linear` は鍵を書きません(engine の既定)。表に出ない鍵(cubic の key の接線など)は、そのまま残して送ります。
- **小さな曲線**(`.graph`): 表の上に key を点で描きます。点を drag すると動き、Shift で時刻だけ、Ctrl で値だけです。drag の間は押した時の枠で測り、放すと 1 回の編集です([`GraphCanvasCurveGraph`](../../src/devstudio/view/graphcanvasvaluefields.hpp#L28))。
- **Timeline の点**: Graph の Timeline の帯は、選んでいる `value.curve` の key を帯の下端に ◇ で描き、横に drag すると時刻だけが動きます(値は欄で)。放すと 1 回の編集です([Timeline の key を横に動かす所](../../src/devstudio/view/graphcanvaswidget.cpp#L4807))。帯の時間の幅は key の時刻(と 0)も含めます。
- 1 回の drag・1 行の確定・+ Key・× は、それぞれ `set_node_parameters` 1 回(Undo 1 回)です([`GraphCanvasModel::setCurveKeys()`](../../src/devstudio/model/graphcanvasvalues.cpp#L219))。drag の時刻は隣の key から 0.001 離れた所で止めます(時刻の順を保つため)。key が 0 個・数でない値・engine が受けない補間・時刻が増えていない列は送らず、欄の下の 1 行(`pelican.graphCanvasValueFields.note`)が理由を言います([`GraphCanvasModel::curveKeysRefusal()`](../../src/devstudio/model/graphcanvasvalues.cpp#L159))。
- test: `devstudio_graphcanvas_test` の [「UX-8 decision 4: a curve's keys are edited a row or a drag at a time, held in order, and each change is one Undo step」](../../test/devstudio_graphcanvas_test.cpp#L3820)、`devstudio_graphcanvas_scene_test` の [「UX-8 decision 4: a curve's Keys table, its small curve and the Timeline's marks each write one edit」](../../test/devstudio_graphcanvas_scene_test.cpp#L10717)。

**value.playback の欄と render.scalar の欄(✅UX-8)**:

- **value.playback**(`pelican.graphCanvasPlayback`): **Wrap**(`.wrap`。`once` / `repeat` / `ping_pong`)と、**Duration**・**Source begin**・**Source end**・**Speed**・**Delay** の数(`.duration` / `.source_begin` / `.source_end` / `.speed` / `.delay`。既定は 1 / 0 / 1 / 1 / 0。[`GraphCanvasModel::playbackNumbers()`](../../src/devstudio/model/graphcanvasvalues.cpp#L241))です。
  - 欄は文書の値を、鍵が無ければ既定を示します。変えると、既定と同じ値でも鍵を書きます(欄の値と文書を同じにするため。コードの注記)。Duration は 0 より大きくなければ送らず、`A playback's duration has to be above 0.` です。
  - edge が来ている数は押せません(tooltip `An edge drives it.`)。「曲線で制御」が作る playback(上の「曲線で制御」が作るノードの集合)も同じ op なので、同じ欄が出ます。`audio.playback` にはこの欄は出ません(Q-10 の Sound / Bus / Wrap の欄)。
- **render.scalar**(`pelican.graphCanvasScalar`。engine の ✅RS-15): **Buffer**(`.target`。上の必須のダイアログと同じ候補)、**Value**(`.components`。`1 (on the value line)` / `vec2` / `vec3` / `vec4`)と 2〜4 個の数の spin(`.value0`〜`.value3`)、**Offset**(`.offset`。読むだけの `<n> bytes`)です。
  - 1 を選ぶと `parameters.value` を消し、値は `value` の線から来ます。vec2〜vec4 を選ぶと数を `parameters.value` に書き、`value` の線があれば同じ編集で外します(engine は配列を書いた node の `value` の edge を読まないため。[`GraphCanvasModel::setScalarValue()`](../../src/devstudio/model/graphcanvasvalues.cpp#L537))。
  - offset は Studio が割り当てます。同じ buffer に書く `render.scalar` を今の offset と id の順に並べ、byte 0 から隙間なく詰めた offset を、変わる sink の全部に同じ編集で書きます(0 は鍵を消す)。Buffer を替えると、新しい buffer の sink の後ろに付け、元の buffer の残りを詰め直します([`GraphCanvasModel::setScalarTarget()`](../../src/devstudio/model/graphcanvasvalues.cpp#L578))。
- test: [「UX-8 decision 5: a playback's wrap and numbers are written one at a time; a number an edge drives is not」](../../test/devstudio_graphcanvas_test.cpp#L3881)、[「UX-8 decision 12: render.scalar's numbers are packed with the other sinks on the buffer, and the engine's layout and port refusals read as sentences」](../../test/devstudio_graphcanvas_test.cpp#L4023)(どちらも `devstudio_graphcanvas_test`)。

**Timeline は root の node を instance 無しで読みます(✅UX-8)**: root の scope では、`Live` が入っていれば `Values from:` が `(none)` のままでも root の node の値を読みます(root の問いは instance を名指さない問いのため。[root を instance 無しで読む条件](../../src/devstudio/model/graphcanvasmodel.cpp#L813))。root の node だけの graph には名指す instance が無く、UX-8 の前の Timeline は `0 samples` のままでした(GB-4 は空の definition を MainCamera に装着して出所にしていました)。

- 読めないときは、Timeline の要約(`pelican.graphTimelineSummary`)が `<node>: 0 samples: <理由>` で理由を 1 つ言います —— `No engine is running.` / `Pick a node to follow over time.` / `A definition has no values of its own: pick the instance to read them through in Values from.` / `Turn Live on to read the values while the engine runs.` / `The engine has not published values for this graph yet.` / `<node> publishes no value (a sink or a render node is not read).`([`GraphCanvasModel::timelineReason()`](../../src/devstudio/model/graphcanvasvalues.cpp#L438))。
- test: `devstudio_graphcanvas_test` の [「UX-8 decision 6: the Timeline reads the root's nodes without an instance while Live is on, and says why when it cannot」](../../test/devstudio_graphcanvas_test.cpp#L3912)。

**音の node を置く・音声を Import する(✅UX-8)**:

- **audio.volume / audio.pitch**: palette で置く前のダイアログ(上の「palette で置くときに、要る parameter を先に聞きます」)が **voice**(scope の `audio.playback` の id)を聞き、その `voice` 出力から `voice` 入力への線と一緒に置きます。`value` には 1 の `value.constant` がつながって来ます(鳴っているままの音量。ほかの op の種の定数は今までどおり 0。[volume と pitch の種の定数を 1 にする所](../../src/devstudio/model/graphcanvasmodel.cpp#L3569))。音量のフェードは、palette の `value.curve` の `value` を `audio.volume` の `value` へ落とします(`gui_build_music`)。
- **Import**: **File ▸ Import Sound…**(`pelican.file.importSound`。[File メニューの Import Sound…](../../src/devstudio/view/mainwindow.cpp#L1837))と、`audio.playback` の Sound の欄の **Import…** は、ファイルのダイアログ(`pelican.audioImportDialog`。Qt のダイアログで、名前の欄は `pelican.audioImportDialog.fileName`)で選んだ `.wav` / `.ogg` を project の `assets/audio/` に写します([`importProjectAudio()`](../../src/devstudio/model/graphcanvasvalues.cpp#L861))。
  - 同じ名前があれば `<名前>_2`、`_3` … にし、上書きはしません。`.wav` / `.ogg` でない file は写さず、理由を notice に出します。
  - canvas と Sound の欄へ `.wav` / `.ogg` を drag & drop しても同じです([音声の drag & drop を受ける所](../../src/devstudio/view/graphcanvaswidget.cpp#L4948))。
  - 写した後に Sound の一覧を読み直し、最初の 1 つを、選んでいる `audio.playback` の `asset` に送ります(選んでいなければ写すだけ)。notice は `Imported project://assets/audio/<名前>.`(複数なら ` and <n> more`)です。
- test: `devstudio_graphcanvas_test` の [「UX-8 decisions 7 and 13: a sound sink is placed driving the voice the dialog picked, at full volume; the dialog asks what the catalog lists」](../../test/devstudio_graphcanvas_test.cpp#L3939) と [「UX-8 decision 9: sounds are copied into assets/audio, _2 beside a name already there」](../../test/devstudio_graphcanvas_test.cpp#L4206)、`devstudio_graphcanvas_scene_test` の [「UX-8 decisions 7 and 8: audio.volume asks which voice it drives; what one addition brings beside it is placed clear of what is drawn」](../../test/devstudio_graphcanvas_scene_test.cpp#L10785)。

**New definition…(✅UX-8)**: Graph の canvas の右クリック(編集できて、畳んだ view でないとき)に **New definition…**(`pelican.graphCanvas.newDefinition`)があります。押すと definition の名前の行(`pelican.graphCanvasDefinitionName`。`definition` か、使われていれば `_2` … の提案が入っている)が出て、**Create**(`pelican.graphCanvasDefinitionApply`)で空の definition(`nodes: []`、`outputs: {}`)を `add_definition` 1 回で足し、commit が届くと Scope をその definition に切り替えます([`GraphCanvasModel::newDefinitionOps()`](../../src/devstudio/model/graphcanvasvalues.cpp#L455))。UX-8 の前は、definition を作るには Add loop を通るしかありませんでした(仕様の背景)。

- 空で開いた definition の body は、最初の node が来たときに fit し直します(GB-4 で、空の body に fit したまま足した node が画面の外に出た件。[空で開いた scope を fit し直す所](../../src/devstudio/view/graphcanvaswidget.cpp#L8617))。✅UX-9 から、これは definition の body だけで、root は fit し直しません(root は project を読む前に空で描かれ、自分の倍率で開くため。コードの注記)。
- 仕様の決定 11(GB-4 の、ループを消した後に Scope で body を選ぶと canvas が空になる件)は、下の test が「ループを消した後に Scope で選んだ body の node が描かれ、画面の中にある」ことを見ます。原因と直した所は commit の記録に無く、未確認です。
- test: `devstudio_graphcanvas_test` の [「UX-8 decision 10: New definition adds an empty definition by a free name」](../../test/devstudio_graphcanvas_test.cpp#L4006)、`devstudio_graphcanvas_scene_test` の [「UX-8 decisions 10 and 11: New definition opens the definition it made; a loop's body is drawn when its loop is gone」](../../test/devstudio_graphcanvas_scene_test.cpp#L10837)。

**RS-15 の Studio 側 —— log2 のボタン、image の層と mip の式、拒否の 1 文(✅UX-8)**: engine の ✅RS-15(`log2`、body の image の大きさの式、`render.scalar` の vec と offset。[第6章](06_rendering.md) §6.18)を Graph の欄から書けるようにしました。

- **log2**: Loop の欄の Count の式の補完の列に **log2**(`pelican.graphCanvasLoop.insertLog2`)があります。Count の target が graph の大きさの決まった `render.image`(整数の `width` と `height`)のときだけ押せ、`log2(<幅と高さの大きい方>)` を式に入れて送ります。式が空か整数だけなら置き換え、それ以外なら cursor の所に足します([`GraphCanvasLoopFields::insertLog2()`](../../src/devstudio/view/graphcanvasloopfields.cpp#L1118))。
- **image の Mip levels と式**: image の欄に **Mip levels**(`pelican.graphCanvasImage.mipLevels`。既定 1)があります([Mip levels の spin を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L443))。UX-8 の着地時の spin は 0〜16 で、0 が `full` でした。✅UX-12e から、spin の前の combo(`pelican.graphCanvasImage.mipLevelsShape`)で **Count** と **Full chain** を選び、spin は 1〜16 で Count のときだけ出ます(下の「node の欄の残りを選ぶ形に(✅UX-12e)」)。Full chain は `"full"`、Count の 2 以上はその数を書き、1 は鍵を書きません(既に鍵があれば書きます)。大きさの ƒ で式に替えている間は、Layers と Mip levels も式の欄(`.layersExpression` / `.mipLevelsExpression`)になり、Enter で送ります。
- **拒否の 1 文**: `graph.render_scalar_layout_invalid@1` は `reason` ごとに `The render.scalar <node> writes <target> from byte <offset>, over the sink before it (which ends at byte <expected_offset>)`(overlap)/ `…, leaving bytes <expected_offset> up to <offset> unwritten`(gap)、`graph.render_scalar_type_mismatch@1` は `The render.scalar <node> writes <components> float(s) into <target>, whose elements are <element>`、CT-1 の `graph.port_field_missing@1` は `Port <port> of <node> needs <field>` です([render.scalar の 2 つの code の 1 文](../../src/devstudio/model/inspectormodel.cpp#L1404))。test は上の「value.playback の欄と render.scalar の欄」の decision 12 の test です。

**project の compute / RT の shader を palette で置く —— ports・image・線は shader の宣言から(✅EL-5s、`708c667d`)**: engine の ✅EL-5 の `get_shader_interface`(§13.5「project の shader の入出力を読む」)を読み、palette の `render.compute` で選んだ project の shader から ports と書く image と線を作ります。GB-3 では、palette の compute は port 無しで置かれ、descriptor を宣言する shader は置く時に断られ、後から書いた port もつながっていない入出力として断られて、compute の task を使えませんでした(`gui_build_ocean` の description の記録)。

- **一覧**: palette で `render.compute` を選ぶと出る shader の combo(`pelican.graphCanvasPaletteComputeShader`)は、project の `shaders/<名前>.comp`(✅UX-3)に加えて、`.rgen` と同じ名前の `.rmiss` / `.rchit` がそろう project の組を `<参照>  (ray tracing)` で並べます(その後に engine の RT pipeline。✅UX-5d。[project の RT の組を探す所](../../src/devstudio/view/graphcanvaswidget.cpp#L8477))。✅CT-6s から、combo の横の **New…** が RT の雛形を書いてその組を選びます(下の「RT を Graph で組む(✅CT-6s)」)。
- **置く**: project の shader を選んで OK を押すと、notice が `Reading <参照>'s ports...` を言い、Studio が `get_shader_interface` を聞きます(`building` なら 500 ms 後に聞き直す)。`ready` が来たら次を 1 回の編集で置きます([palette の compute を ports が読めてから置く所](../../src/devstudio/view/graphcanvaswidget.cpp#L8563)、[`GraphCanvasModel::computeShaderOps()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8753)):
  - `storage_image` の port ごとに `<node>__<port>` の `render.image` を足します。format は宣言の `format`、無ければ `R16G16B16A16_SFLOAT`、`extent_scale: 1`、usage は `STORAGE` と `SAMPLED` です(view の軸で回すループの body では family の大きさ)。port は `{"kind": "image", "access": "write", "binding": "storage", "type": <scene color の型>}` で、その image からの線を付け、port を出力にします。
  - dispatch は、書く image があり `local_size` の z が 1 なら最初の書く image の `groups_from`、それ以外は `groups: [1, 1, 1]` です(前から `groups` だけを持つ node はそれを残す)。RT の組は `ray_tracing` の `raygen` / `miss` / `closesthit` に同じ参照を書き、最初の書く image の `rays_from` です。書く image の無い RT の組は置きません。
  - `sampled_image` の port は書かず、空のソケットとして描きます(engine は線の来ない宣言の port を断るため。コードの注記)。そこへ線を落とすと、port(`{"kind": "image", "access": "read", "binding": "sampled", "sampling": {"filter": "nearest", "address": "clamp_to_edge"}}` に、落とした出力から辿った `type`)と線を 1 回の編集で送ります([compute の空のソケットに線を落とす所](../../src/devstudio/model/graphcanvasmodel.cpp#L2898))。
  - 置かないとき(notice に 1 行): 120 秒たっても読み終わらなければ `The engine did not finish reading <参照>'s ports.`、`failed` なら `The engine could not read <参照>'s ports: <error>`、compute の stage も書く image のある RT の組も無ければ `<参照> declares nothing a compute task can run: …` です。engine の shader と、`get_shader_interface` を送る口を持たない capability では、今までどおり置きます。
- **compute の欄**(`pelican.graphCanvasCompute`。`render.compute` を選ぶと右に出る): **Shader**(`.shader`)は palette と同じ一覧で、文書の値が一覧に無ければ先頭に足して見せます。別の shader を選ぶと、同じ規則で ports を作り直します —— 同じ名前で宣言される port は線を残し、新しい storage の port には image を作り、宣言から消えた port を読んでいた線を外します(`replace_node` を含む 1 回の編集。interface をまだ読んでいなければ、聞いてから送ります。[`GraphCanvasModel::setComputeShader()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8883))。`engine://` の shader を選ぶと `shader` だけを書きます。✅CT-6s から、Shader の横に RT の雛形を書く **New…** が付きます(下の「RT を Graph で組む(✅CT-6s)」)。
- **reflection_error**: 選んだ shader の stage に `reflection_error` があると、欄が `The engine could not read <参照>'s ports: <message の 1 行目>` を赤で言い(`.status`)、**Copy message**(`.copyMessage`)が message の全部を写します([`GraphCanvasComputeFields::syncStatus()`](../../src/devstudio/view/graphcanvascomputefields.cpp#L290))。message が `undeclared identifier` / `#error` / `undefined macro` を含めば `(the shader may need defines the reflection does not pass)` を添えます(engine の EL-5 は rendering 節の `shader_defines` を渡さずに compile するため。[`GraphCanvasModel::reflectionErrorSentence()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8900))。読んでいる間は灰色で `Reading the shader's ports...` です。
- **見張りと Update ports**: Graph は、compute の node が使う project の shader の file(`.comp`、`.rgen` / `.rmiss` / `.rchit`)を見張り、書き込みを 200 ms まとめて、使われている stem を全部 `get_shader_interface {refresh: true}` で聞き直します([compute の shader の書き込みをまとめる所](../../src/devstudio/view/graphcanvaswidget.cpp#L5551))。image の port の名前が前と違えば、notice が `<参照> changed its ports: + <足された> / − <消えた>` を言い([ports の変化の notice を作る所](../../src/devstudio/model/graphcanvasmodel.cpp#L8970))、notice の下に **Update ports**(`pelican.graphCanvasUpdatePorts`)が出ます。押すと、変わった node の全部を上の「別の shader を選ぶ」と同じ規則で 1 回の編集で作り直します([`GraphCanvasModel::updateComputePorts()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8978))。自動では書き換えません。
- test: `devstudio_graphcanvas_scene_test` の [「EL-5s a project compute shader from the palette arrives with its ports and images」](../../test/devstudio_graphcanvas_scene_test.cpp#L9991)、[「EL-5s a compute node's Shader picked again makes its ports the new shader's」](../../test/devstudio_graphcanvas_scene_test.cpp#L10072)、[「EL-5s a compute shader's reflection error is said in its fields with Copy message」](../../test/devstudio_graphcanvas_scene_test.cpp#L10137)、[「EL-5s a compute shader whose ports changed on disk is said, and Update ports follows it」](../../test/devstudio_graphcanvas_scene_test.cpp#L10189)。GPU シナリオは `gui_build_ocean_compute`(下の「Studio の自動化面」)です。

**RT を Graph で組む —— ray_tracing と dispatch を汎用の欄で、stage ごとの shader の候補、raygen / miss / closest hit の雛形(✅CT-6s、`827eddd7`)**: engine の ✅CT-6(`cbc9bb03`。catalog の `render.compute` の `ray_tracing` と `dispatch` が `fields` 付きの `object` になり、`parameters_complete` が `true` になった。[第6章](06_rendering.md) §6.17 の CT-6 の段落)の Studio 側です。CT-6s の前は、`ray_tracing` は専用の欄の受け持ちとして汎用の欄から外されていたのに、専用の欄は Shader の combo だけで、stage ごとの shader・複数の miss / hit・`hit_shaders`・callable・`acceleration_structure`・`max_recursion` の欄はありませんでした。New… で書ける shader は fullscreen の 3 種だけで、Blank の project から RT の task を組む道がありませんでした(仕様書の記述)。

- **ray_tracing は汎用の欄**: `render.compute` の専用の欄の受け持ちから `ray_tracing` を外しました([専用の欄が受け持つ名前の表の render.compute の行](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1612))。`ray_tracing` は、上の「node の欄を catalog の表から作る(✅UX-12a)」の **More parameters** の下に、畳める見出し(`pelican.graphCanvasParameters.ray_tracing.fold`)と field ごとの欄で出ます。Max recursion は uint32 の spin、`acceleration_structure` は `choices_from` の combo、`traced_ray_types` / `misses` / `hit_shaders` / `callable_record` / `callable_records` は object・array・union の汎用の部品です(実装者の報告)。専用の widget は作っていません。
  - 行が出るのは、文書の parameters に `ray_tracing` がある node だけです。shader で走る compute の task には出ません(2 つは排他で、どちらにするかは Shader の combo で選ぶ。[ray_tracing の行を隠す所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1780))。
  - → ✅RTL-2s(`c8fd51cb`)から、ray group layout を持つ project では汎用の `ray_tracing` の行を隠し、専用の RT の節(Raygen、Traced ray types の check、miss の行、hit shader の格子、callable)が出ます。layout の無い project では今までどおり汎用の欄で、その上に Enable hit groups… への 1 行が出ます(下の「Studio で RT を組む」)。
- **stage ごとの shader の候補**: 名前が `raygen` / `miss` / `closesthit` / `anyhit` / `intersection` / `callable` の field と `misses`(miss の shader の map)は、shader の combo の候補をその stage のものに絞ります([field の名前から stage を決める所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L928))。候補は project の `shaders/` の下の、その stage の拡張子(`.rgen` / `.rmiss` / `.rchit` / `.rahit` / `.rint` / `.rcall`)のファイルと、engine のその stage の shader です([stage で絞る所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1173))。`hit_shaders` の中の `closesthit` / `anyhit` / `intersection` にも効きます。`render.compute` 自身の `shader` は今までどおり compute の shader です。
- **union に Shader と List**: union の形の呼び方に、`shader` の形の `Shader` と `array` の形の `List` を足しました([形の呼び方](../../src/devstudio/view/graphcanvasparameterfields.cpp#L341))。これが無いと、文字列の `miss` が `(none)` と出ていました(実装者の報告)。1 本の shader を List に替えると、その shader を 1 行目に残します([1 本を List の 1 行目に残す所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L831))。Miss と Closest hit を複数にする専用の「+」は作っていません。List の行の **+** で足します。`dispatch.groups` も同じ union で、List / Custom です。
- **dispatch の port**: `dispatch` の欄は CT-6 の catalog からそのまま出ます(仕様書の記述では、CT-6 の前は JSON の 1 行でした)。`groups_from` / `rays_from` の `port` は打つ欄ではなく、その node の `ports` のうち image の port(`kind` が無いか `image`)の名前の combo です。buffer の port は出しません([task の image port を候補にする `taskPortChoices()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L189))。`indirect.buffer` は今までどおり `choices_from` の編集できる combo です。
- **New… —— RT の雛形**: compute の欄の **Shader** の横に **New…**(`pelican.graphCanvasCompute.newShader`)が付きました。押すと下に行が開き、種類の combo(`.shaderTemplateKind`。項目は `Ray tracing (raygen + miss + closest hit)` の 1 つ)、名前(`.shaderTemplateName`)、**Create**(`.createShader`)、1 行の notice(`.shaderTemplateNotice`)が出ます。
  - 名前には空いている名前を入れてあります。`ray_tracing` から始め、`.rgen` / `.rmiss` / `.rchit` のどれか 1 つでもある名前は `_2`、`_3`… と飛ばします([`rayTracingTemplateName()`](../../src/devstudio/view/fullscreenshadertemplate.cpp#L1030))。提案の上に打った名前はそのまま残ります([`GraphCanvasComputeFields::proposeTemplateName()`](../../src/devstudio/view/graphcanvascomputefields.cpp#L212))。打たずに Create を押せます。
  - Create は `shaders/<名前>.rgen` / `.rmiss` / `.rchit` の 3 つを全部書くか、1 つも書かないかのどちらかで、既にあるファイルは上書きしません([`writeRayTracingTemplate()`](../../src/devstudio/view/fullscreenshadertemplate.cpp#L1039))。1 つでもあれば notice が赤で `shaders/<名前> already has a .rgen, .rmiss or .rchit and was left as it is: name the new set otherwise.` を言います。名前が文字・数字・`_`・`-` 以外を含めば `A shader name is letters, digits, '_' and '-'.` です。
  - 書けたら一覧を読み直し、上の「別の shader を選ぶ」と同じ `GraphCanvasModel::setComputeShader()` でその組を trace させ(ports は `get_shader_interface` から作り直す)、notice が `Wrote shaders/<名前>.rgen, .rmiss and .rchit; the task traces with project://shaders/<名前>.` を言います([`GraphCanvasComputeFields::createTemplate()`](../../src/devstudio/view/graphcanvascomputefields.cpp#L228))。task が今受けられなければ `…, but the task could not take it now: pick it from the Shader list.` です。
  - project が開いていない間と、編集できない間は押せません。
- **palette の New…**: palette の `render.compute` の行の shader の combo の横にも **New…**(`pelican.graphCanvasPaletteComputeNew`)があります。こちらは名前の欄を出さず、提案した名前で 3 つを書いて、その組(`<参照>  (ray tracing)`)を combo で選びます([palette の New… の所](../../src/devstudio/view/graphcanvaswidget.cpp#L9258))。OK で置く道は上の EL-5s と同じです(engine が 3 つの stage を compile して読んでから置き、`color` の port に `<node>__color` の image が付き、`dispatch.rays_from` は `color`)。書けなければ palette の下の 1 行が `Could not write the shaders: <理由>` を言います。palette を Cancel しても、書いたファイルは残ります(Pass の欄の New… と同じ。実装者の報告)。
  - fullscreen の Pass の欄の New…(✅UX-5b)には RT を足していません。あちらは fragment を書く欄だからです(実装者の報告)。
- **雛形の中身**([`rayTracingShaderTemplate()`](../../src/devstudio/view/fullscreenshadertemplate.cpp#L900)): engine の `rt_shadow_mask_pipeline` と同じ、ray group layout の無い形です(✅RTL-2s から、layout のある project の New… は layout の形の雛形を書きます。下の「Studio で RT を組む」)。3 つは 1 つの stem で、task は `raygen` / `miss` / `closesthit` に同じ参照を書きます(stage がファイルを選ぶ)。
  - raygen: `pelican_sets.glsl` と `pelican_frame.glsl` を include し、書く image `pelican_resource_color`(`rgba16f`、storage)と、frame の set の ray query の binding(`PELICAN_RAY_QUERY_TLAS_BINDING`)の scene の acceleration structure を宣言します。画素ごとに camera(frame の view と projection)から 1 本 trace し、当たれば右と下の画素を通る 2 本も trace して、3 つの当たった位置が張る面の法線を色(`n * 0.5 + 0.5`)にします。当たらなければ、光線の上下の向きで明るさの変わる暗い空の色です。
  - miss: `hit.w = 0.0;` の 1 行だけです(当たらなかった印。空の色は raygen が作ります)。
  - closest hit: 当たった位置(`gl_WorldRayOriginEXT + gl_HitTEXT * gl_WorldRayDirectionEXT`)と当たった印を payload に返します([closest hit の雛形](../../src/devstudio/view/fullscreenshadertemplate.cpp#L993))。仕様は「法線を返す closest hit」でしたが、layout の無い形では closest hit が三角形の頂点を読めないので(頂点を読む `pelicanRayFetchNormal` は ray group layout の生成 header にしか無く、engine は position fetch の拡張も有効にしていない)、法線は raygen が隣の光線から作ります(実装者の報告)。
- **Task Editor の New ray tracing**: 同じ雛形を書いてその参照を名指す草稿になりました(§13.5「レンダー設定を編集する」の「New compute / New ray tracing」の段落)。
- **RT の拒否の 1 文**: compute task の parser の RT の拒否 16 種を、inspectormodel の 1 文の表にしました([16 種の表](../../src/devstudio/model/inspectormodel.cpp#L878)、[`rayTracingRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L937))。code が拒否の code か message の中にあれば、`<文> (<code>).` を返します。task の名前と、message が引用する field / stage / miss / ray type / group を文に入れます。`knownRefusalDescription()` から呼ぶので、Graph の notice もこの文です。
  - 対象: `pelican.ray_layout_mode_change_requires_restart@1` と、`pelican.ray_tracing.` の `legacy_syntax_with_layout@1` / `unknown_shader_stage@1` / `unsupported_shader_stage@1` / `shader_stage_required@1` / `invalid_shader_reference@1` / `traced_ray_types_invalid@1` / `miss_shader_missing@1` / `miss_shader_unknown@1` / `hit_shader_missing@1` / `hit_shader_untraced@1` / `hit_shader_unknown_group@1` / `invalid_recursion@1` / `invalid_trace_dispatch@1` / `rays_from_requires_pipeline@1` / `trace_extent_required@1`(どれも `@1`)。
  - layout の形の食い違い: layout のある project で `miss` / `closesthit` を書くと `This project has a ray group layout: Task <task> names its shaders in traced_ray_types, misses and hit_shaders, not miss and closesthit`。layout の無い project で `traced_ray_types` / `misses` / `hit_shaders` を書くと(engine は `unknown_shader_stage@1` で断る)`This project has no ray group layout: Task <task> names its shaders in miss and closesthit, not <field>` に読み替えます。
  - mode の変更: `ray_layout_mode_change_requires_restart@1` は `Whether the project has a ray group layout changes only when the engine restarts: restart it to apply this` です。仕様は「mode の変更は RSX の `restart_required` の道へ」でしたが、今の engine はこれを拒否で返し、文書は書かれません。Studio ができるのは 1 文にするところまでです(実装者の報告。engine に回しました。下)。→ ✅RTL-1b(`8034d096`)から、engine はこの変更を拒否ではなく `committed` と `restart_required`(`restart_reason.code: "ray_layout_mode_changed"`)で答えます(§13.5 の「ray の layout と bindings を読み書きする」)。
  - 表に無い code は今までどおり engine の message のままです。
- **ray_query**: raster / fullscreen の pass の `ray_query` の欄は変えていません(✅UX-12e の union のまま)。
- **残した穴**(実装者の報告):
  - 汎用の欄で `raygen` を別の stem に替えても、ports は作り直しません。ports を shader に合わせるのは Shader の combo(`setComputeShader()`)だけです。
  - Task Editor の New compute は、まだ無い `project://shaders/compute` を名指したままです(compute の雛形はありません)。
  - ray type / miss / group の名前(`hit_shaders` / `misses` の鍵、`traced_ray_types` の要素、`callable_record` の `name`)は、候補の無い打つ欄のままです。→ engine は ✅RTL-1b から、`callable_record` の `name` を除く 3 つの候補を catalog の `choices_from` / `key_choices_from` で出します。Studio が読むのは RTL-2s です。→ ✅RTL-2s から、layout のある project では RT の節が名前を check と combo で出し、layout の無い project の汎用の欄でも map の鍵が `key_choices_from` の編集できる combo になりました(打つこともできます)。
  - 既存のシナリオ `gui_build_ocean_compute`(palette の compute の行と compute の欄を通る)は、実装者は流していません。
- **手で打つ欄**: New… の名前(空いている名前が入っているので打たなくてよい。palette の New… には名前の欄が無い)。Max recursion は spin です。ray type / miss / group の名前は上のとおり打つ欄のままです。
- **engine に回したもの**(実装者の報告): (1) `ray_layout_mode_change_requires_restart@1` を拒否ではなく `restart_required`(`restart_reason.code` にこの code)で答える —— そうなれば Studio は RSX の道でそのまま再起動する。(2) layout の無い形の closest hit が法線を読める口(position fetch か、layout 無しでも頂点の表を読める include)—— そうなれば雛形を「closest hit が法線を返す」形にできる。(3) RT の task の parser の拒否に fields を付ける —— 今は `code: 文` の形で、Studio は message から `task '<名前>'` や `field '<名前>'` を拾っている。→ (1) と (3) は ✅RTL-1b(`8034d096`)で engine 側が済みました(§13.5 の「ray の layout と bindings を読み書きする」の再起動の形と「RT の拒否の code と fields」。message の文は変えていないので、今の Studio の 1 文はそのまま出ます)。(2) は調べただけで入っていません(別 WP の大きさ。同じ節の「残る穴」)。→ (1) の Studio 側は ✅RTL-2s で確かめました。Ray layout の節の Enable hit groups の `restart_required` から RS-1 の道で起こし直します(下の「Studio で RT を組む」)。(2) が無いままでも、layout のある project の雛形は `pelicanRayFetchNormal` で法線を読みます(RTL-2s の雛形。layout の無い形の雛形は今も raygen が隣の光線から法線を作ります)。
- test: `devstudio_graphcanvas_scene_test` の [「CT-6s decision 1: ray_tracing is the generic fields' -- each stage lists that stage's shaders, Max recursion a spin, a miss made a list keeps its shader -- and a task that runs a shader has none」](../../test/devstudio_graphcanvas_scene_test.cpp#L12960)・[「CT-6s decision 2: dispatch is the catalog's fields -- groups a list or x / y / z, rays_from's port one of the task's image ports, indirect's buffer the graph's dispatch buffers」](../../test/devstudio_graphcanvas_scene_test.cpp#L13026)・[「CT-6s decision 3: New... beside a compute task's Shader writes a ray tracing set (raygen + miss + closest hit) under a free name it proposes, and the task traces with it; nothing already there is overwritten」](../../test/devstudio_graphcanvas_scene_test.cpp#L13059)・[「CT-6s decision 5: a ray tracing task's refusal reads as one sentence -- the layout's form where the project has none and the other way round, a field the engine does not read, and the layout mode that changes only on a restart」](../../test/devstudio_graphcanvas_scene_test.cpp#L13136)(決定 4 の ray_query は変えていないので test はありません)。GPU シナリオは `gui_build_rt_from_blank`(下の「Studio の自動化面」)です。実装者の run で `studio_cpu`(`ctest -L studio_cpu -LE gpu`)は 677 / 677 が緑でした。

**保存中でも palette が開く、確定前の値を送る、voice の帯、起動時の致命の 1 文(✅UX-9、`4d11aa96`)**:

- **保存中でも palette が開く**: raster の pass の欄の Depth で `New depth image (D32_SFLOAT)` を選んだ後に **Add** を押しても、palette が出ないことがありました(`gui_build_ocean` の途中から作った再現。仕様)。commit の記録の原因は、node への click が値の帯のその場編集の editor を開き、Add が focus を取ったときに editor が変わっていない値を確定して送り、その保存の間に palette を開く関数が何も言わずに戻っていたことです。今は次のとおりです。
  - 帯のその場編集は、文書の値と同じなら何も送りません([帯の変わらない値を送らない所](../../src/devstudio/view/graphcanvaswidget.cpp#L3994))。
  - palette は保存の最中でも開きます(開かないのは、engine が無いか文書をまだ読んでいないときだけ。[palette を開く所 —— 保存の最中でも](../../src/devstudio/view/graphcanvaswidget.cpp#L8951))。選んだものは、保存が届いて編集できるようになってから 1 回足し、その間の notice は `Saving the graph change...` です([保存が届いてから palette の選択を足す所](../../src/devstudio/view/graphcanvaswidget.cpp#L9439))。
- **palette と必須のダイアログは自分の event loop を回しません**: palette(`pelican.graphCanvasPalette`)と `<op> needs` のダイアログ(`pelican.graphCanvasRequired`)は window-modal で `open()` し、閉じた後に event loop から選んだものを処理します([palette が閉じた後に選んだものを読む所](../../src/devstudio/view/graphcanvaswidget.cpp#L9333))。UX-9 の前は `exec()` で開いていて、シナリオの Return / OK の直後の wait が palette の loop の中で処理されて追加が止まることがあり、Return の後に 0.4 秒待たせていました(仕様の背景)。`gui_build_music` と `gui_build_curve` はその待ちを外しました。
- **確定前の値を手放す前に送る**: LC-1s の Draw の spin と同じく、pass の欄(Defines の表の打ちかけの cell・clear colour・Draw の spin)、image の欄(大きさ・層・mip・式・format・usage)、値の欄の spin(Keys の時刻と値・playback の数・render.scalar の数)は、欄が別の node に移る前に、focus のある打ちかけの値をその node へ送ります(上の「送れないときは待って送ります」)。
  - 保存の最中なら、その node を名指したまま覚えて、編集できるようになったときに古い順に送ります([`GraphCanvasHeldEdits::sendOrHold()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L114))。image と pass の欄の parameters の編集は、覚えた後に同じ node の別の鍵が変わっていても、変えた鍵だけを当てます(JSON patch。[`heldParametersEdit()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L140))。Place の欄の選択も同じく覚えます。
  - image・Place・値の欄も、確定した時と覚えている間に `Saving the graph change...` を言います(LC-1s では pass の欄だけでした)。
- **voice の帯**: `audio.playback` の node の下の帯(GL-2b の帯の位置)に、engine の `get_status.audio.voices`(✅CT-1。[第8章](08_gameplay.md) の「`get_status` の `audio`」)の状態を `playing · 1.2 s` / `paused · 1.2 s` の形で出します(秒は `position_seconds` の小数 1 桁)。`idle` は、play の state が `edit` の間は出さず、Play / Pause の間だけ `idle` と出します([`graphCanvasVoiceBands()`](../../src/devstudio/view/graphcanvaswidget.cpp#L1378))。読むのは、Play の帯が受けた UX-7 の 1 回 parse した `get_status` の返事です([voice の帯へ get_status を渡す所](../../src/devstudio/view/mainwindow.cpp#L2061))。
- **起動時の致命の 1 文**: engine が起動時に構成を読めずに終わると(✅CT-1 から、致命の行 `Pelican fatal error : graph.port_field_missing@1 {...}` に code が出る)、Frame Plan の status 行が `The engine stopped: <code の 1 文> (<code>) in <file>. Fix the file (or Reload from disk) to start it again.` を言い、tooltip が致命の行そのものです([`engineStoppedSentence()`](../../src/devstudio/model/inspectormodel.cpp#L2188)、[起動時の致命を 1 文にする所](../../src/devstudio/view/frameplanwidget.cpp#L1671))。
  - code は行の中の最初の `<名前空間>.<名前>@<版>` で、Studio が 1 文を持つ code のときだけこの形になります(持たなければ今までどおり行そのもの)。
  - file は、IA-1s の前は行の中の最後の `.json` の path でした。UX-9 の時点の engine の行は file を言わないので、project.json の `basic_config.rendering_config_json`(例 `passes/main.json`)を使い([`projectRenderDocument()`](../../src/devstudio/model/inspectormodel.cpp#L2175))、それも読めなければ `the project's render documents` です。形は UX-8 で入れ、UX-9 で実際の engine の行で確かめました(下の test)。
  - ✅CT-2(engine `51d1c2d8`)から、graph の拒否の致命の行には `"document":"project://passes/main.json"`(definition の body の node なら `definition` も)が入ります(第6章 §6.18 の CT-2 の項)。✅IA-1s から、file はまずこの `document` を `project://` を外して使い、`definition` があれば `definition <名前> of <文書>` にします(例 `... in definition blur of passes/main.json. ...`)。行の中の最後の `.json` の path は `document` の無い行のときだけ、project.json の推測はそれも無いときだけです([`engineStoppedSentence()` が document を読む所](../../src/devstudio/model/inspectormodel.cpp#L2188))。下の UX-9 の test は IA-1s で注記が「file は行が名指す文書(CT-2 の `document`)」に替わり、期待は `in passes/main.json` のままです(IA-1s の commit の差分)。
  - engine の ✅BS-1(`d9811bfc`)の後も、Studio は engine を `--render-fallback` 無しで起こすので、この道のままです。`--render-fallback` で起こして `get_status.render_pipeline` を読む側は未対応(作業中)です(§13.5 の「起動時に断られた構成を直す」)。→ ✅BS-1s(`19be8983`)から Studio は `--render-fallback` を付けるので、構成が組めなくても Studio の engine は止まらず、Frame Plan の status 行は `Showing a placeholder: …` を言います(下の「組めない構成でも engine を止めない(✅BS-1s)」)。この致命の行の読みは残してあり、フラグを知らない古い engine と、フラグを付けても致命のままのもの(root が JSON だが object でない、`pelican.graph` でない旧い形、material の登録で起きる `material_route_unserved@1`。最後の 1 つは BS-1s の実装者の報告)で使います。→ engine の ✅PB-1(`a55d1eaf`)から、起動の間の material の登録で起きる `material_route_unserved@1` は `--render-fallback` なら致命にならず fallback の世代に入ります(第6章 §6.16)。起動の後の登録とフラグの無い起動では致命のままです。WP298 の「Studio receives the running example fatal text while RPC stdout stays framed」は今もこの致命の行を期待していますが、PB-1 の着地の W1 には入っておらず、PB-1 の後に通るかは未確認です。→ ✅PB-1s(`93934318`)でこの test の期待は fallback に書き換わりましたが、engine が先へ進んで glTF の morph の上限で止まるので赤のままです(§13.1 の「Studio の Problems のドック」の test)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-9 decision 1: an unchanged pill sends nothing as it loses the keyboard, and Add opens the palette over a save in flight, adding its pick once the save lands」](../../test/devstudio_graphcanvas_scene_test.cpp#L10956)、[「UX-9 decision 2: a value typed into a side field goes to its node as the fields let go of it, held for that node while a save is in flight」](../../test/devstudio_graphcanvas_scene_test.cpp#L11078)、[「UX-9 decision 4: a voice's state reads as the band under its audio.playback -- playing and paused with where it is, idle only in a play session」](../../test/devstudio_graphcanvas_scene_test.cpp#L11159)、`devstudio_engine_failure_test` の「UX-9 decision 5: an engine stopped at start by a buffer port without its element says the code's sentence and the file in the Frame Plan's status」(engine を起動して、`Port data of sim needs element (graph.port_field_missing@1) in passes/main.json` を見る)。✅BS-1s でこの test は [「BS-1s decision 4 (UX-9 decision 5): an engine Studio starts on a buffer port without its element keeps running on its placeholder, and the Frame Plan's status says the code's sentence and the file」](../../test/devstudio_engine_failure_test.cpp#L700) に書き換わりました(engine が止まらず、致命の行が無く、最初の `get_status` が fallback で code が `graph.port_field_missing@1`・node が `sim`、Frame Plan の status 行が `Showing a placeholder: … Port data of sim needs element (graph.port_field_missing@1) in passes/main.json. Fix it, then Reload from disk.`)。致命の行の読みは `engineStoppedSentence()` の test と、WP298 の「Studio receives the running example fatal text while RPC stdout stays framed」(`material_route_unserved@1` はフラグを付けても致命)に残っています。→ ✅PB-1s で WP298 の case は致命の行を見なくなり、fallback の帯と Problems の行を見る期待になりました(engine 側の理由で赤のまま。§13.1 の「Studio の Problems のドック」の test)。

**一度だけ走る pass と、frame をまたいで残る image —— Schedule・Lifetime・once の帯・Recompute(✅PS-1s、`349af5e4`)**: engine の ✅PS-1(`render.image` の `lifetime`、pass / task の `schedule`、`rerun_once`、`get_frame_plan` の `last_run_generation` / `last_run_frame`。§13.5 の「`get_frame_plan` に足されたもの」、[第6章](06_rendering.md) §6.18「一度だけ走る pass と、frame をまたぐ image」)を Graph の欄から書けるようにしました。GB-3 の海は、初期スペクトル h0 を毎 frame 計算し直していました(仕様の背景)。

- **Schedule**: pass の欄の `pelican.graphCanvasPass.schedule` と compute の欄の `pelican.graphCanvasCompute.schedule` の combo です。fullscreen / raster は `Every frame` / `Once`、compute は `Every frame` / `Every view` / `Once` です([`GraphCanvasScheduleRow`](../../src/devstudio/view/graphcanvasrenderfields.hpp#L79))。`Every frame` は `schedule` の鍵を消し、ほかは `once` / `per_view` を書きます。
- **Once は書く image を同じ編集で Persistent に**: `Once` を選ぶと、pass が書く image(port の `access` が `write` / `read_write` か、`attachment` を持つ port。線を pass 越しに辿った先の `render.image`)のうち `lifetime: "persistent"` でないものに `lifetime: "persistent"` を書き、schedule と一緒に 1 回の編集で送ります(Undo 1 回。[`GraphCanvasModel::setNodeSchedule()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8202))。engine の `graph.once_writes_transient@1` を送る前に避けるためです。そのときの notice は `Graph change saved.` ではなく `Saved; <image> made Persistent: what a once pass writes is kept.` です。`Every frame` に戻すと schedule だけを消し、image の Lifetime は戻しません。
- **Run the body once**: Loop の欄の `Once` の行の check(`pelican.graphCanvasLoop.bodyOnce`)は、ループの全分岐の body の fullscreen / raster / compute の全部に `schedule: "once"` を書き、それぞれが書く image も上と同じく Persistent にします(1 回の編集。[`GraphCanvasModel::setLoopBodyOnce()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8254))。check が入って見えるのは body の pass と task が全部 once のときで、pass も task も無い body では押せません。外すと once の pass と task を毎 frame に戻し、`per_view` の task はそのままです。
- **Lifetime**: image の欄の combo(`pelican.graphCanvasImage.lifetime`)で、`Per frame`(鍵を消す。文書の `transient` もこれで出る)と `Persistent` です([Lifetime の combo を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L501))。
- **once の帯**: once の node の下の帯(GL-2b の帯の位置。root の node と、ループの body の node)は `once · ran gen <N>`(plan の `last_run_generation`)、まだ走っていなければ `once · not run yet` です。Persistent の image の帯は `persistent` です([once と persistent の帯の字を決める所](../../src/devstudio/view/graphcanvaswidget.cpp#L1343))。body の node は、そこから展開された回の pass のうち一番新しい世代と frame を出します([`GraphCanvasModel::onceRun()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8314))。詳細表には `once` の行(`ran in generation <N> · frame <M>` / `not run yet`)、Schedule の下には `Ran in generation <N>, frame <M>` / `Not run yet`(`.onceRun`)が出ます。
- **Recompute**: once の node の Schedule の横の **Recompute**(`pelican.graphCanvasPass.recompute` / `pelican.graphCanvasCompute.recompute`。once のときだけ出る)と、Loop の欄の **Recompute**(`pelican.graphCanvasLoop.recompute`。Run the body once が入っているときだけ出る)は `rerun_once {node}` を送ります。body の node は plan の node ではないので、その body を走らせるループの名前で送ります([`GraphCanvasModel::rerunOnce()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8372))。canvas の上のボタンの列(Undo / Redo / Auto layout / Fit view の列)の **Recompute once**(`pelican.graphCanvas.recomputeOnceButton`。action は `pelican.graphCanvas.recomputeOnce` の `Recompute all once passes`)は `rerun_once {}` で、engine につながっていて plan に once の pass があるときだけ出ます([Recompute all once passes の action を作る所](../../src/devstudio/view/graphcanvaswidget.cpp#L5748))。右クリックのメニューには入っていません。
  - 答えの notice は `Recompute: 1 once pass runs again on the next frame.`(2 つ以上なら `… once passes run again …`)です。`scheduled` が空なら `<node> runs every frame: there is nothing to recompute.`(`{}` なら `No pass runs once: there is nothing to recompute.`)、engine が `node_unknown` で断れば `<node> is not in the frame plan: there is nothing of it to recompute (node_unknown).`、ほかの失敗は `Recompute was refused: <message>` です([`GraphCanvasModel::receiveRerunOnce()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8395))。
  - 送った後は、送られた pass のどれかの `last_run_frame` が押した時の値より進むまで、Graph の poll ごとに `get_frame_plan` を読み直します(最大 40 回)。帯と欄の世代と frame はそれで進みます。
- **拒否の 1 文**: `graph.once_writes_transient@1` は `<node> runs once but writes <resource>, which is not kept between frames — make it Persistent`、`graph.once_reads_per_frame@1` は `<node> runs once but reads <input>, which changes every frame — it would freeze at the first value` です([once の 2 つの code の 1 文](../../src/devstudio/model/inspectormodel.cpp#L1440))。
- test: `devstudio_graphcanvas_test` の [「PS-1s Once keeps what the pass writes in the same edit, for a pass and a loop's body」](../../test/devstudio_graphcanvas_test.cpp#L4265)、[「PS-1s a once node says when it last ran, and Recompute asks rerun_once and follows it」](../../test/devstudio_graphcanvas_test.cpp#L4323)、[「PS-1s a once pass's refusals read as sentences」](../../test/devstudio_graphcanvas_test.cpp#L4400)。GPU シナリオは `gui_build_ocean` と `gui_build_ocean_compute` です(下の「Studio の自動化面」)。

**画像ファイルを image の初期内容に —— Initial contents・Size from the image・Import…・見張り、拒否の文書(✅IA-1s、`e2fd6dad`)**: engine の ✅IA-1(persistent の `render.image` を画像資産から始める `initial: {asset}`。[第6章](06_rendering.md) §6.18「persistent の image を画像資産から始める」)、IA-2(`cdcdebc1`。`reload_render_documents` が initial に使う asset の中身も比べ、答えに `changed_assets` を付ける)、✅CT-2(拒否の `document` / `definition`。§13.5 の「拒否の `document` と `definition`」)を Studio の操作にしました。GB-3 の LTC の面光源は、表(LUT)を読む口が無く、近似の式で代えていました(仕様の背景)。

- **Initial contents**(`pelican.graphCanvasImage.initial`): `None` と、project の中の `.ktx2` / `.png`(拡張子の大文字小文字は問わない。`.` で始まる directory は見ない)を project root からの path(`assets/images/ltc_1.ktx2`)で並べた combo です([`readProjectImageAssets()`](../../src/devstudio/model/graphcanvasvalues.cpp#L926))。一覧は欄を読み込むたびに読みます。Lifetime が `Persistent` の間だけ押せ、`Per frame` の間は灰色で、tooltip が `Make it Persistent to start from an image` です(engine は transient の image の initial を `graph.image_initial_requires_persistent@1` で断るため)。選ぶと `initial: {asset}` を書きます。`width` / `height` が無ければ同じ編集で `extent_scale` も消し、file の大きさを採らせます(engine は `width` / `height` を省いて `extent_scale` を書いた image を `graph.image_initial_extent_conflict@1` で断るため)。`None` に戻すと `initial` を消し、大きさを file から採っていたなら `extent_scale: 1` と file の幅・高さを書いて、今の大きさを残します([`GraphCanvasImageFields::commitInitial()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L864))。
- **Size from the image**(`pelican.graphCanvasImage.fromImage`): initial があるときだけ押せる check です。file の header(KTX2 の幅と高さ、PNG の IHDR)が読めれば `Size from the image (64 × 64)` と大きさを言います([`readImageAssetExtent()`](../../src/devstudio/model/graphcanvasvalues.cpp#L965))。入れると `width` / `height` / `extent_scale` を消し、外すと `extent_scale: 1` と file の大きさ(読めなければ spin の値)を書きます(どちらも 1 回の編集。[`GraphCanvasImageFields::commitFromImage()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L890))。入っている間は Size の欄が押せません。
- **Import…**(`pelican.graphCanvasImage.import`): Initial contents の横のボタンで、Persistent で project が開いているときだけ押せます。Qt のダイアログ(`pelican.imageImportDialog`、名前の欄 `pelican.imageImportDialog.fileName`。window-modal で、いくつでも選べる)で選んだ `.ktx2` / `.png` / `.hdr` を project の `assets/images/` に写します。同じ名前があれば `_2`、`_3` … を付け、上書きしません([`importProjectImage()`](../../src/devstudio/model/graphcanvasvalues.cpp#L999))。写した最初の file をその image の Initial contents にし、notice は `Imported <参照>.`(2 つ以上なら `Imported <参照> and <N> more.`)です。
  - `.hdr` は写した後、Studio が子 process で `pelican_cli image ktx2 --out <空いた名前の .ktx2> --hdr <写した .hdr>`([第10章](10_tools.md) §10.4)を走らせ、できた `.ktx2` を選びます。notice は `Imported <参照> (baked from <hdr の名前>).` です([`GraphCanvasImageFields::bakeHdr()`](../../src/devstudio/view/graphcanvasrenderfields.cpp#L976))。CLI は環境変数 `PELICAN_STUDIO_CLI`、無ければ Studio と同じ directory の `pelican_cli`(Windows は `.exe`)です([焼き込みの CLI の場所を決める所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L782))。起こせなければ `Could not start pelican_cli (<path>) to bake the .hdr into a .ktx2.`、失敗すれば `Could not bake <hdr の名前> into a .ktx2: <CLI の出力>` です。焼き終わったときに別の image を選んでいれば、写したことだけを言います。
  - `.hdr` から CLI を起こす所は、test もシナリオも通していません(下の decision 4 の test は写すことと変換の指示まで、`gui_build_ltc` は `.ktx2` を Import します)。
- **拒否の 1 文**(どれも末尾は `(… <code>).`):
  - `graph.image_initial_requires_persistent@1`: `<node> starts from an image but is not kept between frames — make it Persistent`
  - `graph.image_initial_extent_conflict@1`: `<node> takes its size from its image and from the screen at once — check From the image, or give it a fixed size`
  - `graph.image_initial_mismatch@1`: `<asset> is <actual> but <node> is <expected> — bake the image to match (pelican_cli image ktx2) or change the image's …` で、`what` ごとに最後が替わります。`extent` は `size`(大きさは `64×64` の形)、`format` は `format`(format の名前)、`color_space` は `format to the other color space`(`sRGB (<format>)` か `linear (<format>)`)です([mismatch の `what` ごとの文](../../src/devstudio/model/inspectormodel.cpp#L1485))。
- **Frame Plan**: Resources のタブの行の `Resource facts` に `Initial contents` の行が出ます。字は `initial: <asset> · sha256 <manifest の値の先頭 8 文字>`、manifest に無ければ `initial: <asset> · not in the manifest` です。書く pass の無い image では、Writers の列にも同じ字が出ます([Resources の行に initial を出す所](../../src/devstudio/view/frameplanwidget.cpp#L4808))。
- **見張り**: Graph は文書(root と各 definition の body)の `render.image` の `initial.asset` を集め(重複なし、並べて。[`GraphCanvasModel::initialAssets()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8354))、変わるたびに MainWindow の読み直し役に渡します([initial の asset を読み直し役へ渡す所](../../src/devstudio/view/mainwindow.cpp#L1555))。RL-1s の文書の見張り(下の「外のエディタで書き換えた render の文書を拾います(✅RL-1s)」)が、文書と並べてこれらの file も見張り、sha256 が変われば文書と同じく `reload_render_documents` を頼みます([`RenderDocumentReload::setInitialAssets()`](../../src/devstudio/model/renderdocumentreload.cpp#L136))。
  - 答えに `changed_assets` があれば(✅IA-2 の engine、`cdcdebc1`)、notice は `Picked up the change to <asset>.` です。`changed_assets` を持たない engine は `no_change` を返すので、notice は `<asset> changed — the engine reads it on its next start.`(Information)です([`RenderDocumentReload::committed()`](../../src/devstudio/model/renderdocumentreload.cpp#L367))。
  - 断られたとき、Studio が 1 文を持つ拒否(asset が image に合わない、など)なら、その文を detail の先頭に置きます。engine が切れたら、変わった asset の覚えを捨てます(起動する engine は今の file を読むため)。
- **拒否の文書(CT-2)**: graph の拒否の 1 文の末尾は、engine が `document` を付けていれば `(in <document>, <code>).`、definition の body の node なら `(in definition <definition> of <document>, <code>).` です。`<document>` は `project://` を外した path(`passes/main.json`)です([1 文に文書と definition を添える所](../../src/devstudio/model/inspectormodel.cpp#L1871))。文が自分で definition を名指す code(文に `<definition>` を持つもの。`graph.definition_*` など)には definition を添えません。起動時の致命の 1 文も同じ値を読みます(上の UX-9 の段落の CT-2 の項)。
- test: `devstudio_graphcanvas_scene_test` の [「IA-1s decision 1: Initial contents lists the project's images, is off on a per-frame image, and writes initial.asset with the size from the image; the check gives a fixed size back」](../../test/devstudio_graphcanvas_scene_test.cpp#L11205)、`devstudio_graphcanvas_test` の [「IA-1s decision 2: an image's initial-contents refusals read as sentences, a mismatch in the words of what differs」](../../test/devstudio_graphcanvas_test.cpp#L4441)・[「IA-1s decision 4: images are copied into assets/images, a .hdr to be baked into a .ktx2 beside it, and the .ktx2 and .png are what Initial contents lists」](../../test/devstudio_graphcanvas_test.cpp#L4485)・[「IA-1s decision 6: a graph refusal names the document and the definition the engine says its node was written in (CT-2), Studio's guess only without them」](../../test/devstudio_graphcanvas_test.cpp#L4520)、`devstudio_frameplan_test` の [「IA-1s decision 3: a physical resource names the asset it starts from and its manifest sha256」](../../test/devstudio_frameplan_test.cpp#L1411)、`devstudio_render_reload_test` の [「IA-1s decision 5: the images' initial assets are watched; changed_assets says it was picked up, an engine without it that it is read on the next start」](../../test/devstudio_render_reload_test.cpp#L435)。GPU シナリオは `gui_build_ltc` です(下の「Studio の自動化面」)。

**Graph の material pass の欄を Pass Editor の form で —— Tags・出力・binding・Load / Clear / Store、Materials drawn(✅UX-11、`0ac261f4`)**: UX-11 の前の Graph の `render.material_pass` の欄は Contract / Variant(上の LP-4e / UX-3 の Draw の欄)と Place / View だけで、`material_filter`・`material_range`・`material_outputs`・`screen_inputs` などと load / store / clear は、詳細表の JSON か帯で打つしかありませんでした(仕様の根拠)。UX-4a の Pass Editor の form(下の「生の JSON の欄はフォームになりました(✅UX-4a)」)を Graph に載せ、Tags を engine の tag から選ぶ形にしました。

- **欄**(`pelican.graphCanvasMaterial`。[`GraphCanvasMaterialPassFields`](../../src/devstudio/view/graphcanvasmaterialfields.hpp#L44)): material pass を選ぶと Draw の欄の下に出ます。中身は Pass Editor の [`MaterialSettingsForm`](../../src/devstudio/view/passforms/materialsettingsform.hpp#L42) と [`AttachmentOpsForm`](../../src/devstudio/view/passforms/attachmentopsform.hpp#L26) で、子の名前は Pass Editor の名前の prefix を `pelican.graphCanvasMaterial` に替えたもの(Tags の any of なら `pelican.graphCanvasMaterial.materialFilter.include`)です。Contract と Variant は Draw の欄にあるので、form のその 2 行は隠します(1 つの鍵に欄は 1 つ。[`MaterialSettingsForm::setContractAndVariantShown()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L556))。
- **書き方**: どの変更も node の parameters 全体を 1 回の `set_node_parameters` で送ります(Undo 1 回。form が知らない鍵はそのまま送る)。名前・binding・clear colour のように打つ欄は、打つ手が 500 ms 止まってから 1 回送ります(UX-11 の着地時は Other (JSON) もこの扱いでした。✅UX-15a で Other (JSON) は `gpu_draw_source` の欄に替わりました。下の「生の JSON の欄はフォームになりました(✅UX-4a)」の gpu_draw_source の欄)。保存の最中の値は UX-9 の `GraphCanvasHeldEdits` で覚え、欄が node を手放す前に打ちかけの値を送ります。
- **Load / Clear / Store**(`Attachments` の行): graph の material pass は出力の一覧を持たないので、color の行は pass の `color_load_op` / `color_store_op` / `clear_color`(color の socket の全部に共通の 1 つ)、depth の行は `depth_load_op` / `depth_store_op` に書きます。color の socket が複数あるとき、どれか 1 行を変えると pass の値が変わり、読み直すと全部の行がその値になります([`GraphCanvasMaterialPassFields::withForms()`](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L195))。depth の行は `depth` の socket につながった image の名前で出ます。
- **Tags**: Pass Editor と Graph の両方で、`material_filter` の any of / none of は Loop の欄と同じ tag の複数選択(✅UX-3 の [`GraphCanvasTagPicker`](../../src/devstudio/view/graphcanvastagpicker.hpp#L27)。UX-11 で自分の file に移りました)です。候補は engine の `get_render_authoring_context` の `material_tags` で(Graph は model が持つ authoring context から、Pass Editor は開くときに読む context から。[Pass Editor が material_tags を読む所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2727))、pass が名指す tag は一覧に無くても check の入った項目で残ります。一覧に無い tag は、menu の最後の **+ new tag** の行(`<picker の名前>.newTag`)に打って Enter で足します(tag を打つのはここだけです)。何も選んでいなければ、include は `any tag (every material)`、exclude は `leave none out` と出ます。UX-4a から UX-11 の前までは `,` 区切りで打つ欄でした。
- **Materials drawn**(`pelican.graphCanvasMaterial.drawn`): 欄の下の表で、frame plan の `material_routing` がこの pass へ送る route の行(`route <route>` と契約)と、engine の material のうち pass の Tags(any of のどれかを持ち、none of のどれも持たない)と Variant(書いてあればそれを持つ)が通すものの行(名前と tag。組み込みは `<名前> (built in)`)を並べます([Materials drawn の表を作る所](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L451))。Tags と Variant は plan が言う値を先に、無ければ文書の値を使います。UX-11 の時点の engine は pass ごとに描いた material を出さなかったので、これは近似で、見出しがそう言います: `Materials drawn -- those its Tags and Variant take`(plan が描画の数を出していれば `Materials drawn (<N> draw(s) this frame) -- those its Tags and Variant take`)。✅UX-12c から、plan がその pass の `drawn_materials`(engine の MP-1)を言えば実際に描いた material の表に替わり、近似は plan が言わない間だけで、見出しの末尾に `; the engine has not said what it drew` が付きます(下の「Materials drawn を実物で(✅UX-12c)」)。行を押すと Frame Plan が前に出て、**Materials** のタブで Target pass がこの pass の行が選ばれます([Materials のタブで pass の行を選ぶ所](../../src/devstudio/view/frameplanwidget.cpp#L3470))。routing がこの pass を名指さなければ、status 行が `The frame plan routes no material to <pass>.` を言います。
- **UX-11 の時点で選ぶ形でなかったもの**: `screen_inputs` / `surface_resources` の名前の候補は、UX-11 の時点では Studio の固定の表(`opaque_color` など 4 つと `directional_shadow` など 3 つ)でした。仕様は「engine の契約に載っていればそれを読む」としていましたが、UX-11 の commit はこの表を変えていません。✅UX-12c から、Graph の欄では engine の一覧(MP-1)から作り、この表([MP-1 の前の engine 用の名前の表 `fixedResourceNamesOf()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L71))は一覧の無い engine と Pass Editor の form のときだけです(下の UX-12c の段落)。仕様が短くする対象に挙げた `gui_build_full` / `gui_build_D` も、UX-11 の commit では変わっていません(`gui_build_D` は UX-12c で延びました)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-11 decisions 1 and 4: a material pass's Tags, outputs, bindings and attachment ops are the Pass Editor's forms on the Graph, each one set_node_parameters; Materials drawn lists the plan's routes and the materials its filter takes, and a row opens the Frame Plan's Materials」](../../test/devstudio_graphcanvas_scene_test.cpp#L11362)、`devstudio_pass_forms_test` の [「UX-11 decision 2: MaterialSettingsForm's Tags are picked among the engine's material tags, a tag the pass names stays checked, and a new one is typed in + new tag」](../../test/devstudio_pass_forms_test.cpp#L402)。GPU シナリオは `graph_from_blank` の後半です(下の「Studio の自動化面」)。

**Materials drawn を実物で、binding の名前を engine の一覧から —— drawn_materials・Drawn の列・Reads を型で絞る(✅UX-12c、`708c4055`)**: engine の ✅MP-1(`afd923e1`。§13.5 の「`get_frame_plan` に足されたもの」の material pass の node の行と、上の `material_pass_contracts` の `screen_inputs` / `surface_resources`)を Studio の欄にしました。UX-11 の Materials drawn は Tags と Variant から推した近似で、deferred の pass に組み込みの transparent が出るなど外れていました(仕様の背景)。

- **Materials drawn を実物に**: plan のその pass に `drawn_materials` があれば、Graph の material pass の欄の Materials drawn(`pelican.graphCanvasMaterial.drawn`)は、描いた material を 1 行ずつ並べます。列は `Material` と `Draws` です([描いた material の行を作る所](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L402))。
  - 行の名前は material の名前です。同じ名前の material が 2 つ以上描かれていれば `<名前> (material <id>)`、名前が無ければ `material <id>`(id が数字でなければ id そのもの)です([描いた material の呼び方 `framePlanDrawnMaterialLabel()`](../../src/devstudio/model/frameplanmodel.cpp#L1301))。engine の組み込みの material は後ろに ` (built in)` が付きます。行の tooltip は id です。例(シナリオ `gui_build_D`): Cube と Floor の基本形の material は名前が同じなので `pelican.primitive.default (material 1)` と `pelican.primitive.default (material 2)` の 2 行です。
  - 見出し(`pelican.graphCanvasMaterial.drawnHeading`)は `Materials drawn as of frame <N> -- <M> draw(s)` です。N は plan の `drawn_materials_frame`、M は draws の合計です。`drawn_materials_truncated` なら ` (first <件数> shown)` が付きます。その frame に何も描かなかった pass は `It drew no material in frame <N>` の 1 行です。
  - 行を押すと、UX-11 と同じく Frame Plan の **Materials** のタブでその pass の行が選ばれます(何も描かなかった行も同じです)。
  - plan が `drawn_materials` を言わない間(publish の後にまだ frame が無い、MP-1 の前の engine)は UX-11 の近似のままで、見出しの末尾に `; the engine has not said what it drew` が付きます。そのあいだ Graph は、poll の時計で `get_frame_plan` を読み直し(同時に 1 本、最大 12 回)、どれかの pass が `drawn_materials` を言った時点でやめます([描いたものが来るまで plan を読み直す `GraphCanvasModel::followDrawnMaterials()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7389))。
- **Frame Plan**: material pass の node の詳細に `Materials drawn` のグループが出ます。グループの行は `<N> material(s), <M> draw(s) (frame <F>)`(truncated なら `, the first <件数> listed` が付く)で、子の行が material ごとの draws です([Materials drawn のグループを足す所](../../src/devstudio/view/frameplanwidget.cpp#L5263))。**Materials** のタブには 6 列目 **Drawn** が増えました。route の行の Drawn は、その route の Target pass が描いたものの要約です。route の行の後に、描いた material ごとの行(Route の列に material の名前、Target pass の列に pass、Drawn の列に `drawn by <pass>, <N> draw(s)`)が並びます([Materials のタブに描いた material の行を足す所](../../src/devstudio/view/frameplanwidget.cpp#L5272))。route が 1 本も無い plan でも、描いた material の行は出ます。
- **binding の名前を engine の一覧から**: Graph の material pass の欄の binding の行(`MaterialSettingsForm` の **Add binding**)で、`screen_inputs` / `surface_resources` の名前の combo は catalog の `material_pass_contracts` の一覧から作ります(contract をまたいで名前ごとに 1 つ、catalog の順。[`GraphCanvasModel::materialPassInputs()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7372))。項目の tooltip は `reads a <source_type>` です。**Add binding** が足す最初の行の名前は、一覧の最初の screen input(一覧が無ければ `opaque_color`)です。一覧が無い(MP-1 の前の)engine では、今までの固定の表です。Pass Editor の form にはこの一覧を渡していないので(`setResourceNameCandidates()` を呼ぶのは Graph の欄だけ。コードの読み)、Pass Editor では今も固定の表です。
- **Reads を型で絞る**: 行の Reads(読む target)の combo は、名前の `source_type` と違う semantic type を持つ target を出しません([Reads を型で絞る `MaterialSettingsForm::fillResourceReads()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L677))。型の分からない target と、型の分からない名前の行では絞りません。target の型は、root の `render.image` / `render.target_ref` の `type.semantic`、無ければ authoring context の `render_targets` の宣言から読みます([`GraphCanvasModel::targetSemantic()`](../../src/devstudio/model/graphcanvasmodel.cpp#L7397))。
- **読む物の無い binding は送らない**: Graph で、Reads が空の binding の行は編集になりません(engine は空の target を断るため)。Reads を選んだときに 1 回送ります([読む物の無い binding を送らない所](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L216))。UX-12c の前は空の行も送り、いつも断られていました(commit の記録)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-12c decision 1: Materials drawn lists the materials the plan says the pass drew in its last frame, with their draws, and falls back to the Tags' approximation, saying so, while the plan does not say」](../../test/devstudio_graphcanvas_scene_test.cpp#L11506)・[「UX-12c decision 3: a binding's names are the engine's list for its kind, and its Reads offers the targets of the type the name reads」](../../test/devstudio_graphcanvas_scene_test.cpp#L11552)、`devstudio_frameplan_graph_test` の [「UX-12c decision 2: a material pass's row says how many materials it drew and their draws, and the Materials tab lists each, drawn by the pass」](../../test/devstudio_frameplan_graph_test.cpp#L12517)。GPU シナリオは `gui_build_D` の後半です(下の「Studio の自動化面」)。

**node の欄を catalog の表から作る —— 汎用の欄・必須の印・Reset・拒否の行(✅UX-12a、`ac7c5250`)**: engine の ✅CT-3(`0d215a68`。`get_graph_catalog` の各 op の `parameters` と `parameters_complete`、拒否の `graph.render_node_parameter_invalid@1`。[第6章](06_rendering.md) §6.17「各 op の parameter の形」)を Graph の右の欄にしました。UX-12a の前は、多くの op の parameter は詳細表の JSON の行か、値の帯の型の推定でしか書けませんでした(`pose.clip_sample` の start_time / root_motion、`value.trigger_clock`、置いた後の `render.buffer` の size / lifetime など。仕様の背景)。

- **欄**(`pelican.graphCanvasParameters`。[node の parameter の欄 `GraphCanvasParameterFields`](../../src/devstudio/view/graphcanvasparameterfields.hpp#L204)): node を選ぶと、その op の `parameters` の 1 件ごとに 1 行を作ります。見出し(`.heading`)は `Parameters (<op>)` です。表が空の op、ループ、instance の node には出ません。行の名前の部品は `pelican.graphCanvasParameters.<名前>` です。
- **専用の欄を持つ op**: `render.image` / `render.fullscreen` / `render.raster` / `render.compute` / `render.material_pass`、`value.curve` / `value.playback`、`render.scalar`、`audio.playback` / `audio.volume` / `audio.pitch`、`ui.value` / `ui.visible` は今までの欄が先です。汎用の欄は、畳んだ **More parameters**(`.more`)の下に、専用の欄が出さない名前だけを並べます([専用の欄が受け持つ名前の表](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1599))。Place の欄を持つ op では `insert` / `pass_set` / `canonical_anchor` も除き、View の欄がある node では `view_select` の行を隠します。`render.compute` の `ray_tracing` は、UX-12a の着地時は専用の欄の受け持ち(実際の欄は Shader の combo だけ)でしたが、✅CT-6s から More parameters に出ます(`ray_tracing` を持つ node だけ。上の「RT を Graph で組む(✅CT-6s)」)。`pose.clip_sample` は下の Pose の欄で出します。engine の ✅CC-2(`1501685a`)が catalog の `render.raster` に足した `clip_y`(enum `down` / `up`、既定 `down`。[第6章 §6.1](06_rendering.md) の「座標の規約」)は専用の欄の表に無いので、コードの読みでは More parameters に enum の欄として出ます(画面では未確認)。✅UX-14 の commit の記録も、More parameters に出ることと、Studio の雛形が NDC と uv の変換を自前でしないことを言います(どちらも見る test はありません。コードの読みでは、`src/devstudio` の雛形にある `* 2.0 - 1.0` は normal の decode だけです)。
- **kind ごとの部品**([1 つの parameter の欄 `GraphParameterEditor`](../../src/devstudio/view/graphcanvasparameterfields.hpp#L92)):
  - `number` / `integer`: spin(UX-10 の feature の Parameters と同じ部品)。`min` と `max` が両方あって幅が 1,000,000 以下なら slider(`.slider`)も付きます。spin は表の範囲で止めません —— 範囲の外は engine が名前で断り、下の拒否の行になります([数の欄を作る所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L524))。✅UX-12e から、表に `soft_min` / `soft_max` があれば slider はその範囲です(下の節)。
  - `bool`: check。`enum`: 表の値の combo(既定の値は `<値> (default)`、既定の無い任意の enum には `(none)`)。`string`: 打つ欄(Studio が値を知っている名前だけ combo。✅UX-12e から、表に `choices_from` があれば、その出どころの一覧の編集できる combo。下の節)。`color`: 色見本と R / G / B / A。`vec2`〜`vec4`: spin の並び。
  - `asset`: project の file の combo と **Import…**(`.import`)です。Import… は Qt のダイアログ(`pelican.assetImportDialog`、名前の欄 `pelican.assetImportDialog.fileName`)で選んだ file を project に写して、その `project://` の参照を値にします([`GraphParameterEditor::importAsset()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1491))。UX-12a の着地時は、並べる拡張子を op と名前と説明の語から推していました(`pose.*` と `rig` は model、`audio.*` と sound は音、image / texture は `.ktx2` / `.png`)。✅UX-12e から、表の `extensions` と `asset_kind` で決めます([`assetParameterExtensions()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L236)。下の節)。
  - `shader`: engine の shader(`get_engine_shaders` の一覧)と project の shader の combo。`node_ref`: 同じ scope のほかの node の id の combo。
  - `object` は、`fields` があれば畳める見出しの下に field ごとの欄(`.<field>`)、`items` があれば key と値の行と + / ×(`.add`、`.<i>.key`、`.<i>.remove`)。`array` は行と + / ×。
  - 表で `expression: true` の項目は横に ƒ(`.f`)が付き、式の欄に切り替わります(Loop の Count の ƒ と同じ)。`json` と、表の kind を部品にしない種類は JSON の行で、JSON でなければ赤くして送りません(UX-12a の着地時は CT-4 の `union` もここでした。✅UX-12e から `union` は形を選ぶ combo です。下の節)。
- **既定と Reset**: 文書に無い parameter は表の `default` を出します。`default` のある parameter が既定と違う値を持つと **Reset**(`.<名前>.reset`)が出て、押すと鍵を消します(engine の既定に戻ります)。
- **書き方**: 変更は node の `parameters` 全体を 1 回の `set_node_parameters` で送ります(Undo 1 回)。spin と slider の動きは 150 ms まとめて 1 回です([送る所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1941))。保存の最中は UX-9 の `GraphCanvasHeldEdits` で覚えます。断られて何も変わらなければ、欄は文書の値に戻ります。同名の入力 edge がつながった parameter は、名前に ` ←` が付いて押せず、tooltip が `An edge feeds <名前>: its value is used.` です。
- **必須の印**: 表の最上位の `required` の名前には ` *` が付きます。値が書かれておらず、edge も無く、`alternatives` のどれも書かれて(つながって)いなければ、名前が赤く、下に `Required: the engine does not take the node without it.`(alternatives があれば `Required, or <名前> (or connect a line).`)が出ます([必須と拒否を赤くする所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1883))。
- **拒否の行**: `graph.render_node_parameter_invalid@1` は、notice では `<node>.<parameter> must be <expected> (<why>)` の 1 文です。`<why>` は、`reason` が `type` なら `wrong type`、`enum` なら `one of: <allowed>`、`range` なら `range: <min>..<max>`(片方だけなら `range: at least <min>` / `range: at most <max>`)です([理由の語を作る所](../../src/devstudio/model/inspectormodel.cpp#L1748))。例(シナリオ `gui_node_parameters`): `render_buffer.size must be integer (range: at least 0)`。拒否が名指す node を、その編集をした scope で選んでいれば、その parameter の行が赤く、下(`.<名前>.problem`)に同じ文が出ます。入れ子の道(`size_from_extent.tile_width`)は、最初の名前の行を赤くし、中のその field も赤くします。赤は次の commit で消えます([parameter の拒否を覚える所](../../src/devstudio/model/graphcanvasmodel.cpp#L4861))。この拒否と `graph.render_node_parameters_missing@1` は、canvas のその node にも拒否の印(LP-4c)を付けます。
- **値の帯と palette**: 帯のその場編集も表の kind を先に見ます(上の「値の帯は一覧を開きます」の UX-12a の項)。palette の必須のダイアログもこの表から作ります(上の「palette で置くときに、要る parameter を先に聞きます」)。
- **Pose の欄**(`pelican.graphCanvasPose`。[`GraphCanvasPoseFields`](../../src/devstudio/view/graphcanvasposefields.hpp#L28)): `pose.clip_sample` の欄です。UX-10 で作った Rig / Clip / Speed / Loop の欄を、同じ汎用の欄の上に載せ替えました。子の名前は `pelican.graphCanvasPose.<parameter>`(`rig` / `clip` / `speed` / `start_time` / `loop` / `root_motion` / `root_node`)です。`rig` は asset の欄(Import… 付き)です。`clip` は表では string ですが、Studio が rig の file(glTF の JSON。`.glb` は最初の JSON の chunk)の `animations` の名前を読んで combo にします(Studio の知識で選ぶのはこの 1 つだけ。[rig の animation の名前を読む所](../../src/devstudio/view/graphcanvasposefields.cpp#L65))。rig を選び直すと読み直します。`start_time` / `root_motion` / `root_node` は UX-12a で欄になりました。
- **New definition…**: 名前の行が、名前を打つ前に閉じることがありました(canvas の更新が、まだ名前の無い作りかけの行を閉じていた。コードの注記)。今は作っている間は閉じません([作りかけの名前の行を閉じない所](../../src/devstudio/view/graphcanvaswidget.cpp#L9809))。
- **engine の表で足りない所**: `parameters_complete: false` の 4 つの op(`render.image` / `render.fullscreen` / `render.raster` / `render.material_pass`)は、どれも専用の欄を持つ op です(UX-12a の着地時は `render.compute` を含む 5 つでした。engine の ✅CT-6 で `render.compute` は `true` になり、`ray_tracing` と `dispatch` も表の欄で出ます)。表に無い鍵は今も詳細表の JSON の行で書きます。専用の欄を持たない op で `parameters_complete` が false なら、欄の下に `The engine reads more of <op> than it lists; the rest is in the details' JSON rows.`(`.incomplete`)が出ますが、今の engine にその op はありません。CT-4(`4d682338`)が表に足した `choices_from`・`asset_kind` / `extensions`・`soft_min` / `soft_max` と op の `placement` は、UX-12a の Studio は読みませんでした(UX-12a の commit のコードにこれらの名前はありません)。✅UX-12e から読みます(下の節)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-12a decision 1: a render.buffer's parameters are fields made from the catalog's table -- an enum a combo, an integer a spin, a bool a check, an object folded, an array rows -- each change one set_node_parameters (a spin's steps gathered), Reset back to the engine's default」](../../test/devstudio_graphcanvas_scene_test.cpp#L11744)・[「UX-12a decision 3: a value pill's editor is chosen by the table's kind before the stored value's JSON type」](../../test/devstudio_graphcanvas_scene_test.cpp#L11879)・[「UX-12a decision 4: graph.render_node_parameter_invalid@1 reads as one sentence and the field it names is red with it」](../../test/devstudio_graphcanvas_scene_test.cpp#L11910)(decision 2 は上の必須のダイアログの項)。GPU シナリオは `gui_node_parameters` です(下の「Studio の自動化面」)。

**node の欄の残りを選ぶ形に —— 値の出どころ・asset の種類・2 通りの形・実用の範囲・置ける場所(✅UX-12e、`eeacce3e`)**: engine の ✅CT-4(`4d682338`。[第6章](06_rendering.md) §6.17 の CT-4 の段落)が表に足したものを、上の UX-12a の汎用の欄と palette で読みます。UX-12a の報告で、string としか言われない選択肢・asset の種類・union・uint32 の上限・root に置けない op が残っていました(仕様の背景)。

- **値の出どころ —— `choices_from`**: string の parameter の表に `choices_from` があれば、欄は編集できる combo です([出どころの一覧の欄を作る `GraphParameterEditor::buildChoices()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L660))。一覧は表が言う所から作ります([choices_from から一覧を作る所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1101))。
  - `{rpc: "get_render_authoring_context", key, field?}`: Studio が持つ authoring context の返事を `key` の道でたどり、`field`(無ければ要素そのもの)を名前として集めます。途中の配列は要素ごとにたどり、同じ名前は 1 回です([`graphParameterChoicesFromReply()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L266))。context は、選んだ node の欄にこの形の parameter があるとき、文書を読むたびに 1 回だけ問います(欄ごとには問いません。[`GraphCanvasModel::askRenderAuthoringForChoices()`](../../src/devstudio/model/graphcanvasvalues.cpp#L808))。UX-12e の着地時は、ほかの `rpc` の一覧は空でした(当時の engine の `rpc` はどれも `get_render_authoring_context`)。✅UX-13b から `rpc: "get_graph_catalog"` は Studio が持つ catalog から作ります(下の UX-13b の節)。
  - `{graph_nodes: [<op>...]}`: 今の scope の、その op の node の id です(選んでいる node 自身は除く)。
  - 一覧に無い名前も打てます。打ったとおりに残り、横に薄い `not listed`(`.<名前>.unlisted`。tooltip は `Not among the names the engine lists here now; it is kept as typed, and the engine says if it does not know it.`)が出ます。
  - 例(test): `render.target_ref` の `name` は context の `render_targets` の名前、`audio.volume` の `target` は scope の `audio.playback` の id です。
  - palette の必須のダイアログの `render.scalar` の `target` は、UX-8 からの Studio の buffer の combo(新しい名前も打てる)です([必須のダイアログの buffer の combo](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2493))。シナリオ `gui_node_parameters` が `target` を選ぶのはこの combo です(コードの読み)。UX-12e の着地時の候補は graph の `render.buffer` と plan の buffer の全部でした。✅UX-13b から、書ける buffer と、置く編集で書けるようにする buffer(` (will be set to runtime_f32)` が付く)です(下の UX-13b の節)。
- **asset の種類 —— `asset_kind` / `extensions`**: asset の欄の combo は、project の file のうち表の `extensions` のもの(無ければ全部)です。**Import…** のダイアログもその拡張子だけを filter にし、ほかの拡張子の file は取り込みません。写す先は `asset_kind` で決めます([asset の種類ごとに写す先を決める所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1491))。
  - `audio`: Sound の一覧の Import… と同じ道で `assets/audio/` に(✅UX-8)。
  - `image`: `.jpg` 以外は image の Initial contents の Import… と同じ道で `assets/images/` に(✅IA-1s)。`.jpg` は `assets/images/` にそのまま写します。
  - `model` は `assets/models/`、`analysis_table` は `assets/analysis/` で、隣に `<file>.import.json` があればそれも写します。どれでもなければ `assets/files/` です。
- **2 通りの形 —— `union`**: kind `union` の parameter は、形を選ぶ小さな combo(`.<名前>`)と、その形の欄(`.<名前>.<kind>`)です([union の欄を作る `GraphParameterEditor::buildUnion()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L786))。`bool` の形は `Off` / `On` の 2 項目、値が 1 つの enum はその値(`"full"` は `Full chain`)、integer は名前に level か count を含めば `Count`、ほかは `Index`、string は `Name`、object は `Custom`、✅CT-6s から shader は `Shader`、array は `List` です([形の呼び方 `graphUnionVariantLabel()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L320))。`required` でも `default` も無ければ先頭に `(none)` が付きます。今の値の型から形を選んで出し([値の型から形を選ぶ `GraphParameterEditor::loadUnion()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L843))、形を替えると、その形の欄に値が無ければ表から決まる始めの値を入れて送ります。例(test): pass の `ray_query` は Off / On / Custom、`value.sample_table` の `row` は Index / Name です。
  - `render.image` の `mip_levels` は image の専用の欄で、spin の前の **Count** / **Full chain** の combo(`pelican.graphCanvasImage.mipLevelsShape`)です([Count と Full chain の combo を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L459))。UX-8 の「spin の 0 が `full`」は外しました。Full chain を選ぶと spin が隠れ、`"full"` を送ります。
- **実用の範囲 —— `soft_min` / `soft_max`**: 数の欄の slider は、表に `soft_min` / `soft_max` があればその範囲です。spin は今までどおり `min` / `max` まで打てます。`soft_max` を越えた値は残り、横に薄い `above the usual range`(`.<名前>.unusual`。`soft_min` を下回れば `below the usual range`)が出ます([範囲の外の印を出す `GraphParameterEditor::showMarks()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L883))。tooltip には `Usual: <soft_min>..<soft_max>` の行が付きます。`render.image` の専用の欄では、Fixed の width か height が `soft_max`(8192)を越えると `above the usual range`(`pelican.graphCanvasImage.sizeUnusual`)が出ます([image の大きさの印を作る所](../../src/devstudio/view/graphcanvasrenderfields.cpp#L389))。
- **置ける場所 —— `placement`**: root で palette を開くと、`placement` が `owned_instance` の op(`pose.*` と `value.morph_weight`)の行は灰色で、末尾に `   needs a definition` と **New definition…**(`pelican.graphCanvasPaletteNeedsDefinition.<op>`)が付きます([palette の灰色の行を作る `markPaletteNeedsDefinition()` の所](../../src/devstudio/view/graphcanvaswidget.cpp#L8913))。root での tooltip は `needs a definition — New definition…: <op> runs inside a definition attached to an object, not in the root` です。灰色の行を選んでいる間は OK が押せず、`<op> runs inside a definition attached to an object: New definition… makes one and opens this list there.` が出ます。UX-12e の着地時は、definition の中で開いた palette では灰色にしませんでした。✅UX-13b から、その definition を owner の無い instance も使うとき(engine の `definition_instances` の `owned_instances_only` が false)は definition の中でも灰色で、文が変わります(下の UX-13b の節の「definition の中の置ける場所」)。
  - **New definition…** を押すか灰色の行で Enter を押すと、palette が閉じて ✅UX-8 の New definition… の名前の行が開きます。名前を打って作ると、canvas はその definition に入り、palette がその op の名前を絞り込みの欄に入れて(選んだ状態で)開き直します([definition に入った後で palette を開き直す所](../../src/devstudio/view/graphcanvaswidget.cpp#L7771))。definition を object に装着するのは別の操作です(下の「Inspector のグラフ装着」)。
  - `graph.op_requires_owned_instance@1` は `<op> runs inside a definition attached to an object — make a definition and place it there` の 1 文です([placement の拒否の 1 文の行](../../src/devstudio/model/inspectormodel.cpp#L1399))。✅UX-13b から、拒否に `definition` が付くとき(definition の中の node)は別の 1 文です(下の UX-13b の節)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-12e decision 1: a string the table says is chosen from somewhere is an editable combo of that list -- the authoring context read once per reading of the document, the scope's nodes of the named ops -- and a name typed off the list is kept, marked」](../../test/devstudio_graphcanvas_scene_test.cpp#L12001)・[「UX-12e decision 2: an asset parameter lists the project's files of the kinds its table entry names, with Import... beside it」](../../test/devstudio_graphcanvas_scene_test.cpp#L12063)・[「UX-12e decision 3: a union is a combo of its shapes picked from the value's type -- ray_query Off / On / Custom, mip_levels Count / Full chain, a sample row Index / Name」](../../test/devstudio_graphcanvas_scene_test.cpp#L12109)・[「UX-12e decision 4: a number's slider goes up to the table's soft_max, the spin past it, and a value above it is marked」](../../test/devstudio_graphcanvas_scene_test.cpp#L12164)・[「UX-12e decision 5: in the root the palette greys an op that needs an owned instance, and its New definition... makes one, goes in and opens the palette there on that op」](../../test/devstudio_graphcanvas_scene_test.cpp#L12192)。GPU シナリオは `gui_node_parameters` の後半です(下の「Studio の自動化面」)。

**音の解析を焼いて描画に使う —— Bake analysis…・Use in Graph・Use in a shader・Re-bake(✅AB-1s、`149c0ec8`)**: engine の ✅AB-1(`892a1455`。§13.5 の「音の解析を焼く —— `bake_audio_analysis` と `list_derived_assets`」、読み方は [第6章](06_rendering.md) §6.18「焼いた音の解析の表を読む」)を Graph タブの操作にしました。GB-4 の目標 3「音に反応する描画」は、engine に解析の node が無く止まっていました。DCC 寄りの機能は入れるが完成時は焼き込む、という方針で、v0 の音の解析は焼き込みです(仕様の背景)。

- **Bake analysis…**: `audio.playback` を選んだときの Sound の行の Import… の横に **Bake analysis…**(`pelican.graphCanvasAudioBake`)があります([Bake analysis… のボタンを作る所](../../src/devstudio/view/graphcanvaswidget.cpp#L6087))。engine の接続が `bake_audio_analysis` を送れるときだけ出て、Sound が空か project が無ければ押せません。仕様は Inspector の Sound の欄にも挙げていましたが、AB-1s の commit は Inspector の widget を変えていません(コードの読み)。
  - 押すとダイアログ `Bake analysis of <sound>`(`pelican.audioBakeDialog`)が開きます([Bake のダイアログ `GraphCanvasBakeDialog`](../../src/devstudio/view/graphcanvasaudioanalysis.hpp#L36))。Features の **RMS** / **Peak** / **Bands** の check(既定は 3 つとも入)、**Bands** の数(1〜128、既定 8。Bands を外すと押せない)、**Hop**(1〜1000 ms、既定 10)、**Smoothing** の attack / release(0〜60000 ms、既定 0)、**Channels**(Mix / Left / Right)、**Table**(既定 `assets/analysis/<sound の file 名から拡張子を除いたもの>.ktx2`)です。子の名前は `pelican.audioBakeDialog.<rms|peak|bands|bandCount|hop|attack|release|channels|out|bake|cancel>` です。feature が 1 つも無いか、Table が `.ktx2` で終わらなければ **Bake** は押せません。解析の窓(hop の 2 倍)は送らず engine に任せます(コードの注記)。
  - **Bake** で `bake_audio_analysis {asset: <sound>, out: <Table>, settings}` を送ります。`settings` は `{features: [選んだ "rms" / "peak" / "bands"], hop_ms, smoothing: {attack_ms, release_ms}, channels}` で、Bands を選んだときだけ `bands` が付きます([Bake の settings を作る `audioBakeSettings()`](../../src/devstudio/model/audioanalysistables.hpp#L60))。
  - 焼いている間、notice の下の行(`pelican.graphCanvasBake.line`)が `Baking <sound>… (<秒> s)` を数え、250 ms ごとに ticket を `get_edit_result` で問います([焼いている間の行を数える所](../../src/devstudio/view/graphcanvasaudioanalysis.cpp#L329))。Graph の notice には始まりの 1 回だけ出ます。焼いている間にもう一度送ろうとすると `<out> is being baked; wait for it to finish.` です。
  - committed で `Baked <out>: <features> × <sample_count> samples` です。band が 3 つ以上なら `band:0..band:<N-1>` とまとめます([committed の 1 行を作る `bakedLine()`](../../src/devstudio/model/audioanalysistables.hpp#L64))。例(シナリオ `gui_build_audio_reactive`): `Baked assets/analysis/beat.ktx2: rms, band:0..band:3 × 101 samples`。
  - 断られると 1 文で、末尾に ` (<code>).` が付きます([焼く拒否の 1 文 `audioAnalysisRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L1925)): `audio.analysis_settings_invalid@1` は `The analysis cannot take <field> = <value> (<reason>)`、`audio.analysis_busy@1` は `<out> is being baked already; wait for it to finish`、`audio.analysis_unavailable@1` は `This engine cannot bake a sound's analysis (<reason>)`、`audio.format_unsupported@1` は `<asset> is not a sound the analysis reads: its extension and its contents must both say WAV, or both Ogg Vorbis`、`audio.analysis_failed@1` は `<asset> could not be baked into <out>: <message>` です。
  - ✅UX-14(`aaba15a6`。engine の ✅AB-2)から、`audio.analysis_failed@1` の `reason` が `output_locked`(engine のやり直しの間ずっと、表か manifest が開かれていた)なら、文は message の代わりに `<asset> could not be baked into <out>: the table is in use — try Bake again` で([output_locked を見分ける `audioAnalysisOutputLocked()`](../../src/devstudio/model/inspectormodel.cpp#L1919))、行に **Bake again**(`pelican.graphCanvasBake.again`。tooltip `Send the same bake once more, once the table is no longer in use.`)が出ます。押すと、前に送った `bake_audio_analysis` の params をそのまま送り直します(焼いている間は送りません。[Bake again の所](../../src/devstudio/view/graphcanvasaudioanalysis.cpp#L216))。ボタンは次の bake を送ると隠れ、ほかの拒否では出ません。Re-bake も同じ送り方なので、Re-bake が `output_locked` で断られても出ます(コードの読み。test は Bake analysis… の道だけ)。例(test): `sounds/hit.wav could not be baked into assets/analysis/hit.ktx2: the table is in use — try Bake again (audio.analysis_failed@1).`
- **Use in Graph**: committed の行に **Use in Graph**(`pelican.graphCanvasBake.useInGraph`)と **Use in a shader**(`.useInShader`)が出ます。Use in Graph は、今の scope に `value.sample_table` を `{asset: "project://<表>", row: "rms"}`(表に rms が無ければ最初の行)で、palette と同じ道で置きます([Use in Graph の `useTableInGraph()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8687))。`time` の入力は繋がないので、engine は `scene_time` で読みます。
  - palette で置く `value.sample_table` の必須のダイアログは、✅UX-13b から他の op と同じ汎用の欄です([表と行の欄を作る所(UX-13b の注記)](../../src/devstudio/view/graphcanvasrenderfields.cpp#L2481))。`asset`(`pelican.graphCanvasRequired.asset`)は表の `asset_kind: "analysis_table"` と `extensions: [".ktx2"]` で作る combo で、先頭が `(choose)`、その後に project の `.ktx2` の file が並びます(拡張子で絞るだけなので、焼いた表でない `.ktx2` も並びます。コードの読み)。`row` は union の形の combo(`pelican.graphCanvasRequired.row`)が `Name` で、名前の combo(`.row.string`)が選んだ表の隣の `.import.json` の `source.metadata.layout.features` です。表を選ぶと最初の行を選んだ状態になり(例(test): `rms`)、表を選び直すと行の一覧を読み直します。表を選ぶまで行は空で OK は押せません。
  - AB-1s の着地時(`149c0ec8`)は専用の combo で、`asset` は manifest の tool が `pelican.audio_analysis` の表だけ、`row` は rms があれば rms を選んだ状態でした。表が無いときの tooltip `No baked table yet: Bake analysis… on a sound (an audio.playback's Sound) makes one.` は UX-13b で無くなりました。置いた後の欄も同じ汎用の欄で、`row` の Name が同じ manifest の combo です(下の UX-13b の節の「表の行名」)。
  - `value.sample_table_unreadable@1` は `<node> cannot read the table <asset>: <reason>`、`value.sample_table_row_unknown@1` は `<asset> has no row <row>; its rows are <rows>` の 1 文です([value.sample_table の拒否の 1 文の行](../../src/devstudio/model/inspectormodel.cpp#L1461))。
- **Use in a shader**: 1 回の `edit_graph_document` で次を置きます([Use in a shader の op 列を作る `GraphCanvasModel::sampleTableShaderOps()`](../../src/devstudio/model/graphcanvasaudioanalysis.cpp#L43))。
  - 表の image: `render.image` `<表の名前>_table`(`R32_SFLOAT`、usage `SAMPLED`、`lifetime: "persistent"`、`initial: {asset: <表>}`。`initial.asset` は `project://` の無い project 相対の綴りで、大きさは書かず file の大きさになる)。
  - layout の buffer: `render.buffer` `<表の名前>_layout`(`element: "vec4"`、`size: 16`、`host_source: "runtime_f32"`)と、それを埋める 2 つの `render.scalar`。`<表の名前>_layout_values` は offset 0 に `[rows_per_feature, sample_count, hop_seconds]`、`<表の名前>_layout_time` は offset 12 に scene time です(scope に scene time の `value.input` が無ければ `scene_time` を足して繋ぐ)。
  - Graph で fullscreen か raster の pass を選んでいれば、その pass の shader の宣言を見て、次の番号の image の binding に表の image を、shader が `pelican_resource_<port>` の名前で読む buffer の port に layout の buffer を繋ぎます(pass の `ports` も同じ編集で足す)。✅UX-13a から port は binding の番号ではなく名前で選びます: 候補は反映に出ている `pelican_resource_<port>` の buffer の宣言と、pass の shader の file が `pelican_resource_ports.glsl` で読む名前(上の `shaderResourcePortNames()`。生成 include はまだ無い port を宣言しないので、反映には出ません)で、`<表の名前>_layout` があればそれ、無ければまだ port の無い最初の名前、それも無ければ最初の名前です。候補が無い方は繋がず、notice が `<pass>'s shader declares no image binding <n> or pelican_resource_<port> buffer; connect it by hand.` の形で言います。pass を選んでいなければ置くだけです。
  - 置いた後、行の下に shader に写す 1 行(`pelican.graphCanvasBake.shaderLine`。読むだけの欄)と **Copy**(`.copy`)が出ます。形は `#include "pelican_sample_table.glsl"  // and #include "pelican_resource_ports.glsl"; then: vec4 l = pelican_resource_<layout>.values[0]; float v = pelican_sample_table(<image>, <行>u, uint(l.x), uint(l.y), l.z, l.w);` で、行は band:0 があればその番号、無ければ 0 です([写す 1 行を作る `sampleTableShaderLine()`](../../src/devstudio/model/audioanalysistables.hpp#L93))。notice は `Read the table in the shader: #include "pelican_sample_table.glsl" and "pelican_resource_ports.glsl", and call pelican_sample_table (the line below).` です。UX-13a の前は 1 行に `pelican_resource_ports.glsl` の include が無く、notice も `pelican_sample_table.glsl` だけを言っていました。
  - 例(シナリオ): 表 `assets/analysis/beat.ktx2`(rms と 4 band、101 標本)で `beat_table` と `beat_layout` が置かれ、layout の値は `[1, 101, 0.01]`、写す 1 行は `pelican_sample_table(beat_table, 1u, …` を含みます。
- **古さの印と Re-bake**: Graph タブは `list_derived_assets` を、project を開いたとき、文書の digest が前に問うたときから変わったとき(読み直し・編集の後)、**Refresh** を押したときに問います([list_derived_assets を問う `readDerivedIfMoved()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8759))。音の file を書き換えても文書は変わらないので、印を出すには Refresh が要ります(コードの注記)。
  - `stale` の表を読む root の node(`value.sample_table` の `asset`、`render.image` の `initial.asset`)は、下の帯に橙の `stale: <reason>[, <reason>…]` が付きます([stale の帯を集める `syncStale()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8768))。
  - そうした node を選ぶと、右の欄に `pelican.graphCanvasStale` の行(`.line` に同じ文。tooltip `<表> was baked from <音>, which is not what it was baked from any more`)と **Re-bake**(`.rebake`)が出ます。
  - **Re-bake** は表の manifest を読み、その `source` の音・同じ `out`・manifest の `settings` で `bake_audio_analysis` を送ります([manifest から送り直す params を作る `rebakeParams()`](../../src/devstudio/model/audioanalysistables.hpp#L85))。manifest が読めなければ `The manifest of <表> cannot be read, so it cannot be baked again; bake it from its sound.` です。
  - committed の後、Studio は `list_derived_assets` を読み直し(帯が消える)、文書は編集しません(✅UX-13a)。書き換わった表は、Studio の読み直しの見張りが見ている file です(下の「Re-bake の後は reload が表を拾う(✅UX-13a)」)。
  - AB-1s の着地時(`149c0ec8`)は、committed の後に root でその表を読む `value.sample_table` の parameters を同じ値のまま `set_node_parameters` で書き直す 1 回の編集を送っていました。engine の AB-1 は表を graph を組む時に読むので、graph を組み直させて新しい表を読ませるためで、Undo に 1 段残りました。書き直すのは `value.sample_table` だけで、`render.image` の `initial` の表は対象外でした。UX-13a でこの編集と、それを作る関数 `GraphCanvasModel::rebuildSampleTableReaders()` は消えました(`0e0f4481`)。
- **シナリオ** `gui_build_audio_reactive`(下の「Studio の自動化面」)が、CPU の道(`value.sample_table`)と GPU の道(shader が表を `pelican_sample_table` で読む)の両方で、Timeline の時刻に合わせて viewport の画素が変わることを見ます。engine の AB-1 には GPU で描いて値を見る test がありません(§6.18)。
- test: `devstudio_graphcanvas_scene_test` の [「AB-1s decision 1: Bake analysis... asks for the features, bands, hop, smoothing, channels and table, sends bake_audio_analysis, says Baking while it runs and Baked when it is committed; a refusal is one sentence」](../../test/devstudio_graphcanvas_scene_test.cpp#L12489)・[「AB-1s decision 2 / UX-13b decision 5: value.sample_table's dialog is its table's own fields -- the project's analysis tables, and a row the manifest beside the one picked lists, its first taken -- and is placed with them」](../../test/devstudio_graphcanvas_scene_test.cpp#L12842)(UX-13b で名前を替えた。前は `AB-1s decision 2: value.sample_table's dialog offers the project's baked tables and the rows of the one picked, rms first, and is placed with them`)、`devstudio_graphcanvas_test` の [「AB-1s decision 3: Use in a shader places the table's image, its layout buffer and the two sinks that fill it in one edit, on the pass's sampled image and its pelican_resource_ buffer」](../../test/devstudio_graphcanvas_test.cpp#L4628)・[「AB-1s decision 4: list_derived_assets reads as the stale band; Re-bake asks for the manifest's own sound and settings (UX-13a: and edits nothing after it)」](../../test/devstudio_graphcanvas_test.cpp#L4692)(UX-13a で名前の後半を替えた。前は `; the readers are written again as they are`)、`devstudio_graphcanvas_scene_test` の [「UX-14 decision 5 (engine AB-2): a bake refused because the table is in use says to try Bake again, and Bake again sends the same bake once more」](../../test/devstudio_graphcanvas_scene_test.cpp#L12655)。

**buffer の binding は engine の規則で、雛形は include の形、Re-bake の後は reload が表を拾う(✅UX-13a、`0e0f4481`)**: engine の答え(graph から作った pass の buffer の読みは、読む資源の名前の順に bind される。[第6章](06_rendering.md) の「fullscreen pass の入力と shader の宣言の契約」の「`reads` は名前の順」)と ✅IA-3(`26473e75`)に、Studio の 3 か所を合わせました。

- **buffer の binding は engine の規則で**: fullscreen / raster の pass の名前付きの buffer の port は、engine が bind する順に数えます。読む元の buffer の資源名(線を辿った先の buffer の node の id。間の pass が渡すものは辿る)の辞書順で、`buffer_binding_base`(texture の入力の数、可変の組があれば `first + max`)から 1 つずつです。同じ buffer を読む 2 つの port は 1 つの binding です。線の無い port は engine が bind しないので数えず、名前の順で後ろに並びます([`GraphCanvasModel::bufferPortReads()`](../../src/devstudio/model/graphcanvasvalues.cpp#L300))。第6章の「名前の順」の「名前」は読む資源の名前で、port の名前ではありません。
  - UX-8 は port の名前の順と仮定していたので、port の名前の順と読む buffer の名前の順が逆のとき、ソケットと Buffers の欄の番号が engine とずれていました(UX-8 の commit の `namedBufferPorts()` は port 名で並べていた)。
  - ソケット: `pelican_resource_<port>` の名前の buffer の宣言は、どの binding にあってもその名前の port のものです(engine は port をその名前の変数に bind するため。RS-5)。ほかの名前の宣言は、上の規則でその binding に来る読みの port です([`bufferSocketPort()`](../../src/devstudio/model/graphcanvasmodel.cpp#L352))。
  - 上の UX-8 の節の Buffers の欄の番号と、AB-1s の節の Use in a shader の layout の port も、同じ規則と名前で決めます。
- **雛形は include の形**: New… の雛形(Pass-through / Deferred lighting / Tone map のどれも)は、buffer の入力を `layout(binding = N)` で書きません。最初の buffer の入力の注釈の行の頭に `#include "pelican_resource_ports.glsl"` を 1 回置き、`// input <i> (<resource>) is a buffer: map it to a buffer port of the pass and read it as pelican_resource_<port> (pelican_load_<port>(i)).` と言います([buffer の入力の行を書く所](../../src/devstudio/view/fullscreenshadertemplate.cpp#L175))。UX-13a の前の注釈は `// input <i> (<resource>) is a buffer: declare it yourself at set PELICAN_SET_PASS_INPUT, binding <i>.` で、書けと言う binding は入力の位置の番号でした。
  - Open the shader が clipboard に入れる行も同じ関数で作ります。足す入力に buffer があれば、先頭の「`#version` の後に足す」注釈に `#include "pelican_resource_ports.glsl"` が加わります。
  - Use in a shader の写す 1 行と notice も、この include を言います(上の AB-1s の節)。
- **Re-bake の後は reload が表を拾う**: Re-bake の後に送っていた同じ値の `set_node_parameters` を外しました(上の AB-1s の節)。代わりに、IA-1s の見張りに渡す一覧([`GraphCanvasModel::initialAssets()`](../../src/devstudio/model/graphcanvasmodel.cpp#L8354))が、`render.image` の `initial.asset` に加えて `value.sample_table` の表(`project://` を外した綴り)も集めます。Re-bake が表を書き換えると、ほかの file の書き換えと同じく `reload_render_documents` を頼み、engine の IA-3 が新しい世代で表を読み直し、notice が `Picked up the change to <表>.` を言います(上の「画像ファイルを image の初期内容に(✅IA-1s)」の見張り)。
  - 答えの `changed_assets` は `project://` を外して並べ、同じ file を 2 度言いません。image の `initial.asset` と `value.sample_table` の `asset` は同じ表を別の綴り(`assets/…` と `project://assets/…`)で書くためです([`changed_assets` の名前を揃える所](../../src/devstudio/model/renderdocumentreload.cpp#L393))。
- **シナリオ**: `gui_build_curve` と `gui_build_audio_reactive` の shader は include の形で書き、buffer の port は pass の Buffers の **+ Buffer port** で足します(下の「Studio の自動化面」)。`gui_build_audio_reactive` は、Re-bake の後に `Graph change saved.` が出ないこと、Graph の Undo が押せないこと、`Picked up the change to assets/analysis/beat.ktx2` が出ること、GPU の道の画素も変わることを見ます。
- test: `devstudio_graphcanvas_test` の [「UX-13a decision 1: a buffer binding is the read the engine binds there -- after the images, in the order of the buffers the ports read, not of the ports' names; a pelican_resource_<port> block is its port's wherever it sits」](../../test/devstudio_graphcanvas_test.cpp#L4763)・[「UX-13a decision 2: Use in a shader connects the layout by the name the shader reads through pelican_resource_ports.glsl, which no reflection shows before the port is there; the line to copy reads it through the include」](../../test/devstudio_graphcanvas_test.cpp#L4812)、`devstudio_graphcanvas_scene_test` の [「UX-13a decision 3: a Re-bake sends no edit of its own; the table it rewrites is watched beside the images' initial assets, for the reload to pick up (IA-3)」](../../test/devstudio_graphcanvas_scene_test.cpp#L12744)、`devstudio_shadertemplate_compile_test` の「F-3 the shader template declares each input with the engine's macros and compiles」の SECTION [「a buffer input is named for the author, not declared」](../../test/devstudio_shadertemplate_compile_test.cpp#L120)(include が 1 回、`binding 0` を書かない、compile が通る)。

**汎用の欄の残り —— 一覧の条件・catalog・表の行名、render.scalar の書き先を同じ編集で整える、拒否からの Fix、definition の中の置ける場所(✅UX-13b、`342142d5`)**: engine の ✅CT-5(`65f3a8a3`。[第6章](06_rendering.md) §6.5 の CT-5 の項と §6.17 の CT-5 の段落、§13.5 の `get_graph_instances` の `definition_instances`)が足したものを、上の UX-12a / UX-12e の汎用の欄と palette で読みます。

- **一覧の条件 —— `where`**: `choices_from` の `where` で候補を絞ります([`where` を確かめる `whereHolds()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L247))。RPC の答えの一覧なら各要素の member、`graph_nodes` なら各 node の parameters の値を見て、node が書いていない parameter は表の `default` として比べます。値の配列は「どれか 1 つ」です。例(test。部品を直接作る): `render.scalar` の `target` の部品は、`host_source: "runtime_f32"` で float の要素の buffer だけを並べ、`host_source` の無い buffer と `uint` の buffer は出しません。Graph の `render.scalar` の `target` は専用の欄(下の項)が受け持つので、Graph の欄でこの部品は出ません。
- **catalog から —— `rpc: "get_graph_catalog"`**: Studio が既に持つ catalog の `types` と `operations` から一覧を作り、engine には問い直しません([`GraphCanvasModel::catalogChoices()`](../../src/devstudio/model/graphcanvasvalues.cpp#L736))。例(test): `render.image` の `type.semantic` は constructor が image の型(`pelican.render.color_signal@1`・`pelican.render.depth@1` など)で、`pelican.value.scalar@1` は出ません。
- **表の行名 —— `asset_manifest`**: 同じ node の `parameter`(`value.sample_table` なら `asset`)が名指す file の隣の `<file>.import.json` を読み、`key` の道の名前を並べます([manifest を読む所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1108))。asset が変わると読み直します。今の値が新しい表に無ければ、値は残って `not listed` が付きます。必須の union で値がまだ無ければ、一覧が来た時点で、一覧を持つ形の最初の名前で始めます([必須の union を最初の名前で始める所](../../src/devstudio/view/graphcanvasparameterfields.cpp#L1271))。`value.sample_table` の必須のダイアログはこれで作ります(上の AB-1s の節)。Use in Graph は今までどおり rms(無ければ最初の行)で置きます。
- **Explicit…**: authoring context の `render_target_vocabulary.format_class_patterns` が書き方を持つ class(今は `explicit`)は、`format_class` の一覧で `Explicit…` と出ます。選ぶと横に引数の combo(`pelican.graphCanvasParameters.format_class.argument`。`argument_choices_from` の一覧で、今は format の一覧)が出て、`explicit(<format>)` を書きます([書き方を読む `GraphParameterEditor::patterns()`](../../src/devstudio/view/graphcanvasparameterfields.cpp#L704))。引数の始めは、文書の値の引数、無ければ image 自身の `format`、無ければ一覧の最初です。読み戻すと `Explicit…` と引数を出し、`not listed` は付きません。
- **render.scalar の書き先を同じ編集で整える**: 候補([`GraphCanvasModel::scalarTargetChoices()`](../../src/devstudio/model/graphcanvasvalues.cpp#L656))は、palette の必須のダイアログの `target`(`pelican.graphCanvasRequired.target`)と、置いた後の `render.scalar` の欄の Buffer(UX-8)で同じです([Buffer の combo を埋める所](../../src/devstudio/view/graphcanvasvaluefields.cpp#L643))。
  - まず graph の buffer(今の scope、次に root の `render.buffer` / `render.attribute`)のうち、その buffer 自身の parameters の編集で書けるようになるものです。要素が float / vec2 / vec3 / vec4 / mat4(要素を書いていない buffer も含む)で、`value` の線が来ておらず、`size_from_extent` や(pose のでない)`source` で大きさが決まらないもの([判定 `writableScalarBuffer()`](../../src/devstudio/model/graphcanvasvalues.cpp#L75))。
  - 変える所があれば、項目に ` (will be set to <変えるもの>)` が付きます。`host_source` が `runtime_f32` でなければ `runtime_f32`(大きさも同じ編集で揃える)、`host_source` は合っていて大きさだけ違えば `<n> bytes` です([項目の字 `scalarTargetLabel()`](../../src/devstudio/model/graphcanvasvalues.cpp#L687))。例(シナリオ `gui_node_parameters`): `render_buffer_2 (will be set to runtime_f32)`。
  - 次に、frame plan の buffer のうち graph の node でないもの(feature の `bloom.intensity` など。名前に `@` を含むものは除く)を印無しで並べます。これが合わなければ engine が断り、下の Fix の文になります。新しい名前も今までどおり打てます(graph の buffer でなければ整える編集は付きません)。
  - 選ぶと、置く `add_node` か、欄の `set_node_parameters` と同じ ops に、その buffer の `set_node_parameters`(`host_source: "runtime_f32"` と、その buffer に書く sink の合計の大きさ。`render.attribute` は `count`)が入ります([整える op `scalarTargetFixOps()`](../../src/devstudio/model/graphcanvasvalues.cpp#L692))。1 回の `edit_graph_document` なので Undo 1 回で戻ります。新しい sink は先にある sink の後ろの offset に置きます([置く所](../../src/devstudio/model/graphcanvasmodel.cpp#L3610))。値の数を替えると、その編集で buffer の大きさも変えます。別の buffer に移すと、移した先を整え、元の buffer をそこに残る sink の合計の大きさにします([移す所](../../src/devstudio/model/graphcanvasvalues.cpp#L611))。
  - 例(test): float 4 byte の `level` に 1 つ目の sink を置くと `runtime_f32` 4 byte、その値を vec3 にすると 12 byte、2 つ目の sink は offset 12 で `level` は 16 byte、2 つ目を `params` に移すと `level` は 12 byte に戻ります。
- **拒否からの Fix**: `graph.render_scalar_target_incompatible@1` は 1 文です([1 文を作る所](../../src/devstudio/model/inspectormodel.cpp#L1786))。graph の buffer(拒否に `node` がある)なら `The render.scalar <node> writes <target> (<actual>), which must be <expected> — Fix: make <target> runtime_f32, <size> bytes`、graph の外で宣言された buffer(`node` 無し)なら `A render.scalar writes <target> (<actual>), declared outside the graph, which must be <expected> — Fix: make <target> runtime_f32, <size> bytes where it is declared` です。`<actual>` / `<expected>` は `<host_source>, <size> bytes`(`host_source` が無ければ `no host_source`)で、末尾に CT-2 の文書と code が付きます。
  - graph の buffer のときだけ、notice の下に **Fix: make <target> runtime_f32, <size> bytes**(`pelican.graphCanvasFixScalarTarget`)が出ます([button を作る所](../../src/devstudio/view/graphcanvaswidget.cpp#L5530))。押すと engine が言った大きさで、その buffer の `set_node_parameters` を 1 回送ります([`GraphCanvasModel::fixScalarTarget()`](../../src/devstudio/model/graphcanvasvalues.cpp#L715))。graph の外の buffer は文だけで、button は出ません。button は次の commit で消え、別の拒否が来ればそれに置き換わります。
- **definition の中の置ける場所**: Studio は `get_graph_instances` の `definition_instances` を覚え、palette を definition の中で開いたとき、その definition の `owned_instances_only` が false(owner の無い `graph.instance` や `graph.loop` も使う)なら、`placement` が `owned_instance` の op を root と同じく灰色にして **New definition…** を付けます([`paletteNeedsDefinition()`](../../src/devstudio/view/graphcanvaswidget.cpp#L8898))。tooltip は `needs a definition — New definition…: <op> cannot stand here: this definition is also used by an instance with no owner`、選んでいる間の文は `<op> cannot stand here: this definition is also used by an instance with no owner: New definition… makes one and opens this list there.` です。使う node が全部 owner 付きの definition、誰も使わない definition、`definition_instances` を言わない engine では灰色にしません。
- **拒否の 1 文**: `graph.op_requires_owned_instance@1` に `definition` が付くときは `<op> runs only in a definition attached to an object; this definition is also used by an instance with no owner` です([1 文を選ぶ所](../../src/devstudio/model/inspectormodel.cpp#L1813))。例(test): `value.morph_weight runs only in a definition attached to an object; this definition is also used by an instance with no owner (in definition gain_curve of passes/main.json, graph.op_requires_owned_instance@1).`。Inspector のグラフ装着の `edit_graph_instances` の拒否も、✅CT-5 から engine が名前付きの code と `document` を返すので、Graph と同じ 1 文で読みます(§13.5「Inspector のグラフ装着」)。
- **変えていないもの**: `render.fullscreen` の `push_constants`(CT-5 で enum)は、汎用の欄ではなく pass の欄の専用の combo(`No push constants` / `Camera position` / `Projection and view`。[combo を作る所](../../src/devstudio/view/fullscreenpasssettings.cpp#L61))が持ち、UX-13b はこれを変えていません。
- **シナリオ** `gui_node_parameters`(下の「Studio の自動化面」)は、`render_buffer_2` の `host_source` を手で選ぶ手順をやめ、`render.scalar` の必須のダイアログで `render_buffer_2 (will be set to runtime_f32)` を選び、同じ編集で `render_buffer_2` が `{element: float, size: 4, host_source: runtime_f32}` になることを見ます。
- test: `devstudio_graphcanvas_scene_test` の [「UX-13b decision 1: a list narrowed by where, one read from the catalog Studio holds, and one read from the import manifest beside the asset its sibling names, again when it changes」](../../test/devstudio_graphcanvas_scene_test.cpp#L12275)・[「UX-13b decision 4: in a definition an instance with no owner also runs, the palette greys an op that needs an owned instance and says why; in one only owned instances run, it does not」](../../test/devstudio_graphcanvas_scene_test.cpp#L12344)・[「UX-13b decision 6: a format class with an argument is Explicit... and a combo of formats beside it, written explicit(<format>); a refusal inside a definition an unowned instance runs is one sentence」](../../test/devstudio_graphcanvas_scene_test.cpp#L12409)・上の AB-1s の節の「AB-1s decision 2 / UX-13b decision 5 …」、`devstudio_graphcanvas_test` の [「UX-13b decision 2: a render.scalar's target offers the buffers it can write once the same edit makes them runtime_f32 and as large as their sinks, and each edit of the sinks keeps them so」](../../test/devstudio_graphcanvas_test.cpp#L4099)・[「UX-13b decision 3: graph.render_scalar_target_incompatible@1 reads as one sentence with its Fix; a graph's buffer is fixed in one edit, one declared outside the graph is only said」](../../test/devstudio_graphcanvas_test.cpp#L4153)。UX-12e decision 3 の test は、CT-5 で `row` の Name が combo になって壊れていたのを UX-13b で直しました(commit の記録)。Inspector のグラフ装着の拒否の読みを見る test は、この commit にはありません。

### Graph タブが出せる RPC

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
  Graph タブが直接送ります([送る所](../../src/devstudio/view/graphcanvaswidget.cpp#L6490))。✅GL-5a から、詳細欄の rendering 節の pass の編集(`edit_preset_pass`)と Serial / Isolate / No alias(`edit_target_planning`。`GraphCanvasReadCapability::requestEditTargetPlanning`)も同じ所から送ります
- Timeline の scrub: `set_time` と `render_frame`(1 つの「scrub」にまとめず、2 本の RPC として名前で持ちます)
- engine の shader の一覧(UX-5b。一覧を開くときだけ問います): `get_engine_shaders`
- fold group(✅GL-3a / GL-4a): `get_fold_groups`、group の決定を読む `get_render_documents`(root の reference 1 つだけを渡す)、`fold_nodes`、`verify_fold`。結果は同じ `get_edit_result` で待ちます。この 4 本は下の分配を通らず、fold group の controller が直接送ります([fold group の 4 本の口](../../src/devstudio/view/graphcanvasreadcapability.hpp#L100))
- compute の shader の interface(✅EL-5s): `get_shader_interface`。once の走らせ直し(✅PS-1s): `rerun_once`(文書は変えない)
- 音の解析(✅AB-1s): `bake_audio_analysis`(結果は同じ `get_edit_result` で待つ)と `list_derived_assets`。文書は変えません。この 2 本も下の分配を通らず、音の解析の controller(`GraphCanvasAudioAnalysis`)が直接送ります([音の解析の 2 本の口](../../src/devstudio/view/graphcanvasreadcapability.hpp#L133))

それ以外を要求すると `The graph canvas cannot issue <method>.` になります
([分配](../../src/devstudio/view/graphcanvaswidget.cpp#L7633))。キャンバスから preset を fork することは
**できない**、というのが型で保証されています。pass の編集は ✅GL-1e から `edit_preset_pass` を持ちますが、型が絞るのは
RPC の名前だけで、params は絞りません。送るのが rendering 節の fullscreen pass の `input` だけなのは、呼ぶ側のコード(上の
「線を受ける pass」)によります。

## 15.7 キャンバスとパネルに共通の操作

### キャンバスの表示と配置

**位置と折り畳みはセッション内の状態です。**`(scope, ノード id)` を鍵に覚えていて、**文書には書きません**。
revision が変わって消えたノードの位置は捨てられます。初期配置は最長経路による段組みで、辺は必ず左から右へ進みます。

**ズーム・パン・fit は 2 つのキャンバスで同じです(N-3)。**ホイールはカーソルの下の点を動かさずに拡大・縮小し、
倍率は **0.25〜4.0** で止まります([ホイール](../../src/devstudio/view/nodegraphics.cpp#L590))。パンは**中ボタン**ドラッグ、
または **Space を押しながら左**ドラッグ([判定](../../src/devstudio/view/nodegraphics.cpp#L700))。**`F`** キーと
右クリックメニューの **Fit view** で内容全体が view に収まります(Graph タブの右クリックは「Add node... / Fit view」の
2 項目です。[メニュー](../../src/devstudio/view/graphcanvaswidget.cpp#L3525))。この 3 つの操作は共有部品
[`NodeCanvasView`](../../src/devstudio/view/nodegraphics.hpp#L223) が 1 か所で持っていて、**Graph タブも Frame Plan の
Logical graph も同じ view** です。**まだ見ていない scope は自動で fit** され、戻ってきた scope は離れたときの倍率と
スクロール位置に復帰します([`showScope()`](../../src/devstudio/view/nodegraphics.cpp#L673)) —— 倍率も位置も文書には
書かない、セッション内の状態です。値の帯やラベルを編集している最中の `F` はただの文字です(開いている editor が
キーボードを持ちます。[条件](../../src/devstudio/view/nodegraphics.cpp#L741))。

**テーマはライトとダークの 2 つです(✅N-5)。**切り替えは **View ▸ Theme ▸ Light / Dark**
([メニューの構築](../../src/devstudio/view/mainwindow.cpp#L1762))で、**再起動は要りません** —— 選んだ名前は
QSettings の [`studio/theme`](../../src/devstudio/view/studiotheme.cpp#L26) に残り、次に開いた窓は
そのテーマで始まります。追随するのは **Graph タブと Frame Plan の両方のキャンバス**で、背景・grid・辺・
ノードの body と縁・見出しの字・値の帯の色をテーマの表から引き直して描き直します。**ソケットの型別の色だけは
テーマに依存しません**(型の識別色なので、どちらのテーマでも同じ色です。テーマが決めるのは縁と、どの型とも
言えないソケットの色)。既定は `dark` で、`dark` のキャンバスの色は N-5 以前の色と同じ値です(完了記録
`6a26eef3`)。

**Auto layout(✅N-4)**: Graph と Frame Plan の右クリック menu「Auto layout」(`pelican.graphCanvas.autoLayout` / `pelican.framePlan.autoLayout`)で、その scope の手置き位置を捨てて層配置(最長経路の列、辺は lane を通って node を横切らない、列内の並びは重心法、行は最小間隔で重ならない)に戻します。動かした node は通常の refresh では保持されます。中ボタンのパンは描画が view に収まっていても動きます。✅GL-1b から、Graph の root では、その view の畳んだ node の場所(drag したものを含む)も捨てて、graph の node と同じ 1 回の配置に入れます。文書に書くのは graph の node の場所だけで、畳んだ node の場所はセッションの間だけ覚えます(上の「graph の外から来た pass を畳んだ node で見る」の置き場所)。

- ✅UX-1 から、Graph タブでは上段の **Auto layout** のボタンからも発火できます(上の「上段の操作列」)。
- ✅UX-1 から、画面に書くものと sink は、そこまでの道が短くても最後の列に立ちます。Graph タブでは sink の node、`display` か `swapchain` を書く畳んだ node(Blank の `present` など)、その pass を作った graph の node です([集める所](../../src/devstudio/view/graphcanvaswidget.cpp#L1999))。Frame Plan の Logical graph では、`display` / `swapchain` か `frame_target` の資源を書く node(グループなら member のどれか)です([集める所](../../src/devstudio/view/frameplangraphics.cpp#L3068))。
  - 共有の配置 `layoutNodeGraph` の `LayoutOptions::last_column` が受けます。名指した node と、そこから辿れる node を右へ動かします。行き先は「最後の列 − その node から出る最長の道の辺の数」で、元の列より左へは動かさないので、辺が逆を向くことはありません([動かす所](../../src/devstudio/view/nodelayout.cpp#L296))。
  - test: `devstudio_graphcanvas_scene_test` の [「UX-1 what writes to the screen and a sink stand in the last column, however short the path to them」](../../test/devstudio_graphcanvas_scene_test.cpp#L8167) と、ボタンの [「UX-1 Auto layout and Fit view stand in the Graph tab's row beside Undo and Redo」](../../test/devstudio_graphcanvas_scene_test.cpp#L8116)。

**配線の制御点と置き場所(N-6)**: 辺の double-click で **reroute**(通過点)が入り、drag で動き、Delete で消えます。node を動かしても reroute は動きません。制御点や辺を 2 つ以上選んで右クリック **Tie together** で 1 つの **tie**(結束バンド)になり、辺は等間隔で平行に通ります(**Untie** で戻す、double-click で並びを反転)。**Auto layout** は lane を reroute と tie として出します。node の位置・reroute・tie は文書の scope ごとの `layout`(editor 用、engine は無視)に `set_layout` で残り、次に開いても同じ図です。Frame Plan の位置はまだセッション内です。

**配置の保存は canvas を描き直しません(✅Q-5)**: node を drag した後、配置の変更がまだ送られていない間(300 ms の
debounce の間)は、notice が **「Saving the graph change...」** になります(error の notice だけはこれより優先)
([配置が未送信の間の notice](../../src/devstudio/view/graphcanvaswidget.cpp#L9945))。保存が返ると「Graph change saved.」です。
以前は前の保存の「Graph change saved.」が残ったままだったので、その文を待つシナリオは今の保存を待てませんでした。
また、**配置だけが変わった読み**(前の読みと `layout` 以外が同じ)では、model は node の値を消して読み直さず
([配置だけの読みの判定](../../src/devstudio/model/graphcanvasmodel.cpp#L1469))、canvas が既にその配置を描いていれば
(自分の drag の保存が返ってきたとき)scene を作り直しません —— 選んでいる node・辺・reroute はそのまま残ります
([`GraphCanvasScene::followDocumentPlacement()`](../../src/devstudio/view/graphcanvaswidget.cpp#L886))。Undo や別の書き手が
違う配置を書いた読みは描き直し、ほかの変更が混じった読みは描き直して選択を引き継ぎます。Frame Plan の配置の保存には、
もともと描き直しがありません(読み戻しも選んだ node を保ちます)。
⚠ engine が配置を丸めて返すと、送ったものと一致しないので「Saving…」のまま残ります。編集できる状態にならないままでも
同じです。

**配置の保存の後の編集も通ります —— 文書の版を panel どうしで知らせ合います(✅Q-13)。**配置の保存(`set_layout`)も rendering 文書の commit で、`source_digest` を進めます。以前の Frame Plan は自分の配置の保存の後に context を読み直さず、次の Add material pass... などを古い digest で送って `external_modification: render config base digest no longer matches the engine-owned document` で断られていました(project を開き直すまで直りませんでした)。Graph タブで node を動かした後の Frame Plan の編集も同じでした。

- **共有の digest**: Studio は「engine が今持っている rendering 文書の digest」を 1 つ持ちます([共有の digest の型](../../src/devstudio/view/renderconfigdigest.hpp#L25))。Frame Plan・Pass Editor・Graph タブ・Render Features は、読んだ digest と commit した digest をそこへ知らせ、知らせた panel 以外は自分の context を読み直します([共有の digest を 4 つの panel に渡す所](../../src/devstudio/view/mainwindow.cpp#L1026))。読んでいる最中・編集の最中なら、終わってからもう 1 回読みます。Pass Editor は form の下書きを残し、Add fullscreen pass で選んだ挿入位置も変えません。隠れている Frame Plan は、表に出たときに読みます。Render Features から開く Task Editor(`ComputeTaskDialog`)も、開くときに Render Features から共有の digest を受け取り、読んだ root の digest を知らせ、他の panel の commit を知らされると変えた下書きを残して読み直します(✅Q-15。[Task Editor が知らされる所](../../src/devstudio/view/renderfeatureswidget.cpp#L1020))。notice は `Another panel changed the document; reading it again. Your edits are kept.` で、残した下書きは読み直した root の digest で送り、task 自身の digest(`base_task_source_digest`)はそのままです。要求の最中に知らされたら、応答の後に読み直します。
- **要求は panel 自身の digest で送ります**: 共有の digest が持つのは「今の版」と通知だけです。要求の `base_source_digest` は、その中身を組んだ context の digest のままです。古い context から組んだ中身を新しい版の名で送ると、他の panel の変更を上書きしうるためです。
- **それでも `external_modification` で断られたら、1 回だけ読み直して組み直します**: Frame Plan は編集(辺の接続・切断、帯、Delete、render target の作成・編集・削除、material pass の 2 段)を「context から要求を組む関数」として持ち、断られると context を読み直して同じ関数で組み直して送ります。断られた byte は送り直しません([読み直して組み直す所](../../src/devstudio/view/frameplanwidget.cpp#L1936))。Pass Editor は form を残して読み直し、Save / Remove を組み直します。Graph タブは配置だけの編集(op が全部 `set_layout`)を同じ op 列で 1 回送り直し、ほかの gesture は今までどおり捨てて読み直します([配置を送り直す所](../../src/devstudio/model/graphcanvasmodel.cpp#L1332))。Render Features は staged の編集を、Task Editor は変えた下書きを残して、1 回送り直します。
- **2 回目も断られたら**: Frame Plan の status 行と Pass Editor の notice に `The document changed outside this panel. Press Reload to read it again.` が出て、横に **Reload**(`pelican.framePlan.reloadAuthoring` / `pelican.fullscreenPass.reloadAuthoring`)が出ます。Reload は context を読み直すだけで、変更はもう一度行います。Render Features と Task Editor は新しいボタンを持たず、`... Press Refresh to read it again.` で既存の **Refresh** を指します。

⚠ 共有の digest に乗っていないものがあります。Inspector のグラフ装着(`GraphAttachmentModel`)、procedural asset の dialog、Render Features から開く小さな dialog 4 つ(frame node の順・preset の編集・callable の初期値・scene の acceleration structure)は、開いたときに読むだけです。Render Features の新しい挙動には専用の test がありません(Q-13 の記録)。Task Editor は、要求が失敗したとき(`inspectorRpcFailed` か、応答を decode できないとき)は最中に知らされた読み直しを拾わず、次の応答か Refresh まで残ります。読み直しは editor に `setPlainText` で入れるので、editor の undo の履歴は消えます(Q-15 の記録)。

### 応答性と engine の窓

**応答性(Q-2)**: Studio は何も起きていない間は engine にほぼ問い合わせません(modal transform は gesture 中か engine 窓が focus を持つ間だけ、scene tree は revision が動かなければ 2 秒に 1 回、GPU 計時は panel が見えている間だけ)。Frame Plan の応答は 1 回だけ parse し、キャンバスは背景と grid を cache します。それでも Debug build は Release の 6〜19 倍遅いので、体感を見るなら Release 版で。project を開いた直後の固まりは Q-4 で消えました: Studio は engine の窓 thread が応答するまで埋め込みを待ち、その間は viewport に「Loading …(n s)」を出します。engine が log を書き続ける限り待ち、黙ったまま 120 秒(`PELICAN_STUDIO_ENGINE_LOAD_TIMEOUT_MS`)で停止します。Frame Plan の帯の editor も Graph と同じ規則(窓の瞬きで閉じない、再描画で残る、Escape で取消)です。

**engine の窓は Studio の外に出ません(✅EW-2)**: Studio は engine を `--embedded` 付きで起動します([引数の組み立て](../../src/devstudio/viewport/studioplayerarguments.cpp#L102))。engine はこの引数で窓を非表示・focus 無しで作る(EW-1)ので、project の読み込みの間(上の Loading の間)に Studio の外へ窓が出ません。project と一緒の起動・Restart Engine・project の切替のどれでも同じです。

- 窓の探し方: 同じ process で owner の無い top-level 窓のうち、engine が付ける property `PelicanEngineRenderWindow`(`src/core/os/enginewindowproperty.hpp` の `Pelican::engineRenderWindowProperty`)を持つものを選びます。見えているかは見ません([`findProcessWindow()`](../../src/devstudio/viewport/nativewindowhost.cpp#L58))。GLFW の helper 窓には property が付かないので選ばれません。
- 待ち方は Q-4 のままです(窓の thread が WM_NULL に応答してから埋め込み、その間は Studio が Loading を描きます)。
- 窓を見せるのは埋め込みの後です。`embed()` は SetParent の前に `WS_VISIBLE` を立てず、SetParent の後の `resize()` の `SWP_SHOWWINDOW` で初めて表示します([表示する所](../../src/devstudio/viewport/nativewindowhost.cpp#L203))。まだ top-level の窓に先に立てると、SetParent までの間だけ desktop に見えるためです(実装者の報告では、戻すと下のシナリオが約 50 ms 見える窓を捕まえて赤になりました)。
- `PELICAN_STUDIO_PLAYER_ARGUMENTS` に `--headless` があるときは `--embedded` を付けません(engine は 2 つ一緒を引数の誤りとして断るため)。付けなかったことを知らせる 1 行はありません。設定に `--embedded` があれば二重には付けません。
- EW-2 の前は、engine が見える窓を読み込みより先に作り、Studio は見えている窓を探していたので、読み込みの間は engine の窓が Studio の外に出ていました。

### パネルの文字と notice

**パネルの文字(✅N-7)**: 各パネルの既定状態に出るのは見出し・ラベル・値・1 行の状態だけです。仕組みの説明は control の tooltip か、パネル右上の `?`(この章の該当節を開く)にあります。検査の一覧は記号(✓ / ✗ / –)で、文は hover。notice は言うことが無いときは消えます。文字の大きさとフォントはテーマ表(`studio_themes.json` の `font`)から取り、`devstudio_paneltext_test` がパネルごとの可視文字の上限を固定します。

**notice の文は写せます(✅UX-1)**: パネルの notice の 1 行は、マウスで選べます。右クリックのメニューの **Copy message**(`pelican.panelNotice.copyMessage`)は、行が切り詰めて見せている文ではなく、全文を clipboard に入れます([`panelNoticeMessage()`](../../src/devstudio/view/panelhelp.hpp#L85))。

- 全文は tooltip の文です(engine の拒否なら code と message)。行が tooltip の頭を切ったものならその tooltip だけ、行と tooltip が別の内容なら、行と tooltip を改行でつないだものです。
- notice の行は全部 [`applyPanelNoticeStyle()`](../../src/devstudio/view/panelhelp.hpp#L106) で整えるので、ここで足したメニューはどのパネルの notice にも付きます(この関数を呼ぶのは Graph タブ、Frame Plan、Pass Editor、Render Features、Rendering settings、Inspector、project の移行の panel と MainWindow です)。
- test: `devstudio_graphcanvas_scene_test` の [「UX-1 a notice's Copy message puts the whole message on the clipboard」](../../test/devstudio_graphcanvas_scene_test.cpp#L8251)。

## 15.8 Frame Plan

### Frame Plan のノード編集とソケット

**Frame Plan のノード編集(N-8)**: Frame Plan でも出力ソケットから編集できる pass / task の入力ソケットへ**通常ドラッグ**で接続でき、入力ソケットを空所へドラッグするか辺を Delete で切断します(shader が入力を宣言している pass では、落とした ○ が binding を決めます。次の段落。✅VI-3a)。どれも既存の `edit_preset_pass` / `edit_compute_task` を宣言丸ごとで 1 回送るだけで、engine の pass や fork 前の preset、feature の pass は吸着せず理由を 1 行で出します。shader stem と task の dispatch は行の帯でその場編集(shader の帯は Pass Editor の Pick... と同じ一覧を部分一致で補完します。打った文字はそのままです。✅T-1。[帯の補完を付ける所](../../src/devstudio/view/frameplangraphics.cpp#L4546))、それ以外は node の double-click で今までの dialog(✅GL-5c から、Graph が編集する pass / task は Graph タブの詳細欄)。右クリック「Add fullscreen pass…」は Pass Editor をその node の後ろに prefill して開き(✅GL-5c から、graph root では Graph の `render.fullscreen` を置きます。§13.5 の「graph root では Frame Plan の Add を Graph の palette に(✅GL-5c)」)、Delete は authored pass(`remove_authored_pass`)と task(`remove_compute_task`)を消します。Shift ドラッグは今までどおり dialog です。node の位置・reroute・tie・group は Graph の `layout` の `frame_plan` に一緒に保存されます。N-8 のときは rendering 文書が legacy 形(root が `pelican.graph` でない)なら保存せず、その旨と file 名を 1 行で出していました。MG-2b から engine は legacy root を読み込みで `render_config.legacy_root@1` と断るので、そういう project を Studio で開いて canvas に届くことはなく、この 1 行も出ません(Studio 側の分岐は残っています)。

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
  ([帯を描く所](../../src/devstudio/view/frameplangraphics.cpp#L1618))。空いている枠(max まで)は描きません。Blank pipeline の
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
([落とし先の判定](../../src/devstudio/view/frameplangraphics.cpp#L4384))。image を buffer の ○ に(またはその逆に)落とすと、
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
2 つのボタンが並びます([行とボタンを出す所](../../src/devstudio/view/frameplanwidget.cpp#L2893))。

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

- 可変の帯の端(`+ input`)の横の **Fix** の丸いボタン(`node_socket_fix#<pass>`。[Fix を描く所](../../src/devstudio/view/frameplangraphics.cpp#L1666))
- 可変の組を持つ node の右クリックの **Fix sockets**(`pelican.framePlan.fixSockets`。選んでいる node にも効きます)
- Pass Editor の Fragment 欄の横の **Fix sockets**(`pelican.fullscreenPass.fixSockets`)。開いている pass の shader に可変の組が
  あるときだけ出ます。書き換えるのは Frame Plan と同じで、未保存の下書きではなく、今描いている shader(`pass_inputs.fragment`)です

✅GL-5d から、Graph タブでも押せます: rendering 節の pass と root の `render.fullscreen` / `render.raster` の node の詳細欄のボタン
(`pelican.graphCanvasNodeActions.fixSockets`)と右クリック(`pelican.graphCanvas.fixSockets`)、rendering 節の pass への線の右クリックです。
呼ぶのは Frame Plan の同じ関数です(§13.5 の「Frame Plan にしか無かった操作を Graph に(✅GL-5d)」)。

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
  ([複製して送る所](../../src/devstudio/view/frameplanwidget.cpp#L2349))。
  `edit_preset_pass` が届くのは rendering 節が宣言した pass(Blank の `present` など)です。✅GL-5d から、Graph タブの
  Fix sockets を graph の node(root の `render.fullscreen` / `render.raster`)が描く pass に使うと、同じ関数が複製を書いた後に
  `edit_preset_pass` を送らず、Graph タブがその node の `shader.fragment` を複製に替える `set_node_parameters` を 1 回送ります
  (§13.5 の「Frame Plan にしか無かった操作を Graph に(✅GL-5d)」)。Frame Plan 自身の Fix sockets(右クリック・Fix のボタン)と
  Pass Editor の Fix sockets はこの口を渡さないので、graph の node の pass に押すと今も `edit_preset_pass` を組みます。
  そのとき何が起きるか(Studio か engine が断るか)は確かめていません(未確認)。
- Frame Plan の Open the shader は、engine の file を書き換えずに複製して同じ 1 回を送り、複製を desktop で開きます
  ([Frame Plan で複製して開く所](../../src/devstudio/view/frameplanwidget.cpp#L2422))。
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

自動化面の名前(表の正は [README](../../test/studio_scenarios/README.md)。[宣言のソケットの名前を付ける所](../../src/devstudio/automation/automationcommands.cpp#L296)):

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
| `pelican.graphCanvas.fixSockets` / `pelican.graphCanvas.makeSocketsVariadic`、`pelican.graphCanvasNodeActions.fixSockets` / `.makeSocketsVariadic` | Graph タブの右クリックと詳細欄のボタンの Fix sockets / Make sockets variadic(✅GL-5d。Frame Plan の同じ関数を呼ぶ) |
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

### view の軸を選ぶ —— view_select

**view の軸を選ぶ —— view_select(✅LP-4a / LP-4b)**: 影の cascade や cube capture の面のように、1 つの view family が
複数の view を持つとき、pass や task をその一部の view だけで走らせられます(宣言の `view_select: {<軸>: <値>}`。軸と engine の
扱いは [design_graph_loops](../design_graph_loops.md) §7)。Studio は `get_frame_plan` の node の `view_select`、`view_axes`
(軸ごとの名前・長さ・実行時に決まるか・ラベル)、`no_selected_view` を読みます([node で読む所](../../src/devstudio/model/frameplanmodel.cpp#L1388))。

- **canvas の 1 行**: `view_select` のある node(pass も task も)は、見出しの下、値の帯の上に `view: face 2 (+Y)` の 1 行を持ちます
  ([1 行を描く所](../../src/devstudio/view/frameplangraphics.cpp#L1537))。軸が複数なら `, ` でつなぎ、並びは軸の名前の辞書順です。
  値の後ろの括弧は、engine が `view_axes` の `labels` で名前を付けた軸(cube の face)にだけ付きます。cascade は `view: cascade 2` です。
  tooltip は `<pass> runs only for the views of <family> with face 2 (+Y).` です。
- **このフレームで走らなかった node は薄くなります**: engine が `no_selected_view: true` を付けた node は、直近に publish した
  フレームで `view_select` に一致する view が 1 つも無く、何も記録していません。その node は見出しの帯が薄くなり、見出しと node の
  tooltip に `No view of <family> matches view_select this frame.` が出ます([薄くする所](../../src/devstudio/view/frameplangraphics.cpp#L1432))。
- **詳細の行**: node の詳細に `View select`(例 `face 2 (+Y)`)の行が出ます。走らなかった node には `This frame`(上の文)の行も出ます
  ([詳細の行](../../src/devstudio/view/frameplanwidget.cpp#L4440))。Scopes の行は末尾に `; view: face 2` が付きます(ラベルは付きません)。

**view_select の拒否は 1 文で出ます(✅LP-4a)**: engine の `view_select` の拒否 4 種は、field の JSON のままではなく、1 文と末尾の
code で出ます([文を作る所](../../src/devstudio/model/inspectormodel.cpp#L1266))。

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

**view family の大きさの拒否(✅LP-4f2)**: engine の `render.view_family_extent_mismatch@1`(✅VF-1。1 つの view family の raster pass が違う大きさの target に書いた。上の「変種と組み直しの拒否の形」)も 1 文で出ます([`viewFamilyExtentRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L1225))。上の view_select の文と同じ関数の先頭で読むので、出る所も同じです(Frame Plan の status 行、Pass Editor、Task Editor の notice、Graph タブの notice)。

```text
loop/0/face_pass (loop loop, body node face_pass) writes loop/0/face_pass__color at screen ×1.0, but $capture/cube draws at 256×256 (its first pass cube_capture_geometry writes cube_capture_albedo): the passes of one view family draw at one size (render.view_family_extent_mismatch@1).
```

- 大きさは、固定なら `256×256`、画面相対なら `screen ×1.0` です(倍率は小数 1 桁。要るときはそれ以上。`screen ×0.25`)。
- ループが複写した pass には `(loop <loop>, body node <node>)` が付きます(`loop_origin` / `expected_loop_origin`)。
- fields は、code の横、`error_fields`、別の拒否の message の中の `<code>: {…}` のどこからでも読みます。
- **赤い印**: 拒否が `loop_origin` を持つと、Graph タブはループと、その要素の分岐の body の中のその node を赤くします(上の「ループと definition の拒否」の赤い印と同じ規則。[`GraphCanvasModel::markRefusal()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4826))。⚠ 印が付くのは、fields が code の横か `error_fields` で届いたときだけです(message の中だけにあるときは、文は出ても印は付きません。LP-4f2 の着地時の記録)。
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
**View** の submenu が出ます([submenu を埋める所](../../src/devstudio/view/frameplangraphics.cpp#L4076))。

- 軸ごとに軸名の見出しがあり、その下に All views と値が排他の check 項目で並びます。今の値に印が付いています。
- 実行時に決まる軸は、押せない 1 行です。
- 選ぶと、その場で 1 回の編集を送ります([pass の要求](../../src/devstudio/view/frameplanedit.cpp#L2081))。
  - pass は `edit_preset_pass` です。fullscreen pass では、engine が必須にしている `input` / `output` / `shader` も宣言どおり一緒に送ります。
  - task は `edit_compute_task` で、宣言ごと送ります。
- 今と同じ値を選ぶと送らずに、`<name> already runs for face 2.` のように言います(All views なら `face All views`)。
- submenu の objectName は `pelican.framePlan.viewMenu` です。シナリオではまだ通していません。

**保存の規則(✅LP-4a / LP-4b)**:

- **Pass Editor の Save は、View の欄を変えたときだけ `view_select` を送ります**
  ([送るかを決める所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2477))。
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

### root の pass・render target・material pass

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
([direct root の Pass Editor の拒否](../../src/devstudio/view/fullscreenpasswidget.cpp#L2570))。canvas も同じ文で断り、
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
✅GL-5d から、Graph タブの `render.target_ref` の **Edit target…** / **Delete target** が同じ form と同じ remove の要求を使います
(double-click の中身は `presentRenderTargetEdit()` に切り出されました。§13.5 の「Frame Plan にしか無かった操作を Graph に(✅GL-5d)」)。

**Whole frame —— まだ誰も読まない target をつなぐ(✅F-7)**: Target combo の先頭は **Whole frame** です。まだ表示して
いない graph(project を開いた直後を含む)はこれで開き、refresh の後は選んでいたものが残ります。選択は Studio を
閉じると忘れます。Whole frame は**全 pass** と、**どの pass も書かない宣言済み target**(宣言しただけでまだ使っていない
`scene_color` など)を 1 枚に描きます([`populateWholeFrame()`](../../src/devstudio/view/frameplangraphics.cpp#L2476))。
後者は F-1 の resource node として、それを最初に読む pass の 1 列左(読む pass が無ければ描画全体の左)に置かれ、
右端に出力ソケット(自動化面の合成名 `node_socket#<名前>/out:<名前>`)を持ちます
([置き方](../../src/devstudio/view/frameplangraphics.cpp#L3086))。**このソケットから pass の入力ソケットへドラッグすると、
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
  開きます)。選んだ contract が出力の枠を決めます([contract ごとの枠の表](../../src/devstudio/model/frameplanmodel.cpp#L2263)。✅MC-1s から表は model の
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
([拒否で残る target の文](../../src/devstudio/view/frameplanwidget.cpp#L2675))。いらなければ node から Delete してください。
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
([`MainWindow::requestEngineRestart()`](../../src/devstudio/view/mainwindow.cpp#L2341))。未保存の編集が無ければ今までどおり
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
(組込には `(engine built-in)`)と直し方を言います([`materialRouteUnservedNotice()`](../../src/devstudio/model/frameplanmodel.cpp#L2331)。✅GL-1d で `materialpassedit.cpp` から移し、Graph タブの拒否の文と共有します)。
G-1 のシナリオの fixture が持っていた、誰も読まない pass 2 つは G-4 で外しました
([fixture の README の G-4 の段落](../../test/fixtures/studio_material_pass_authoring/README.md#L21))。

## 15.9 Pass Editor

### Pass Editor の form と fullscreen pass

**Pass Editor の form は scroll します(✅F-7)**: form は `QScrollArea`(`pelican.fullscreenPassScroll`)の中にあります。
以前は form の最小の高さ(1,000 px 近く)が下の dock 領域を押し上げ、Pass Editor を開くと viewport が 60〜80 px の帯に
潰れ、dock を閉じても戻りませんでした([置き場](../../src/devstudio/view/mainwindow.cpp#L676))。**その潰れた状態を
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
shader 名を pass 名から付けます。自分で打った名前は上書きしません([雛形の名前の提案](../../src/devstudio/view/fullscreenpasswidget.cpp#L1547))。
保存した後も form はその名前を持ったままです。

**shader は一覧から選べます(✅T-1)。**Fragment と Vertex の欄の横に **Pick...** の一覧
(`pelican.fullscreenPass.shader.fragmentChoice` / `pelican.fullscreenPass.shader.vertexChoice`)があります
([一覧を欄の横に置く所](../../src/devstudio/view/fullscreenpasswidget.cpp#L1189))。並びは project の `shaders/` の `.frag`(Vertex は
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
捨てません([下書きを残す判定](../../src/devstudio/view/fullscreenpasswidget.cpp#L3322))。

**既にある shader を読む pass の New…(✅G-5)**: pass が既に `shaders/<pass 名>.frag` を読んでいるとき(New… で作った
`shaders/present.frag` を読む `present` で、もう一度 New… を押したときなど)、名前の提案は `<pass 名>_<雛形>`(`present_tone_map` / `present_deferred_lighting`)で、
そのファイルもあれば `_2`、`_3`… を足します。Pass-through なら `<pass 名>_2` からです
([既存の file を避ける shader 名の提案](../../src/devstudio/view/fullscreenpasswidget.cpp#L1314))。以前は今読んでいるファイルの名前を
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

**生の JSON の欄はフォームになりました(✅UX-4a)**: Pass Editor の raster の draw と raster state、shader の defines、material pass の設定、preset の routing は、UX-4a の前は JSON を打つ欄でした。今はどれも選ぶ欄と表です([フォームを並べる所](../../src/devstudio/view/fullscreenpasswidget.cpp#L536))。送る RPC(`add_authored_pass` / `edit_preset_pass`)と値の形は変わりません。

- **部品**: フォームは `src/devstudio/view/passforms/` の独立した widget です。どれも宣言の今の値(JSON)を受け、変えた後の値(JSON)を返し、`changed` で知らせます。フォームが知らない鍵は、受けたまま返します([`PassForm`](../../src/devstudio/view/passforms/passform.hpp#L28))。Pass Editor に依存しないので、Graph タブの詳細欄にも同じ部品を埋め込める形です(✅UX-11 から、`MaterialSettingsForm` と `AttachmentOpsForm` は Graph タブの material pass の欄にも載ります。上の「Graph の material pass の欄を Pass Editor の form で(✅UX-11)」)。子の objectName は `<prefix>.<field>` で、Pass Editor の prefix は `pelican.fullscreenPass` です。
- **Raster**([`RasterStateForm`](../../src/devstudio/view/passforms/rasterstateform.hpp#L26)): `raster_state` の topology・cull・front face(行の名前は **Winding**)・depth test / write / compare と、color attachment ごとの blend と write mask。raster pass では `draw` の operation と数も出ます。どの combo も先頭の **Default** は鍵を書かず、engine が鍵の無いときに読む値のままにします。fullscreen pass では depth の欄と draw を出さず、color attachment は 1 つです。
- **Defines**([`ShaderDefinesForm`](../../src/devstudio/view/passforms/shaderdefinesform.hpp#L19)): `shader.defines` を 1 行 1 define(`NAME` か `NAME=VALUE`)の表で、**Add** / Remove で足し引きします。engine が断る行 —— engine が pass を compile するときに自分で定義する名前、C の識別子でない名前、同じ名前が 2 回 —— は赤く、engine の文で理由を出します(Studio は engine を link しないので、`passshaderdefines.cpp` の規則を写しています)。断られる行があれば、Pass Editor は下書きを作らず、その文を出します([止める所](../../src/devstudio/view/fullscreenpasswidget.cpp#L1810))。
- **Material**([`MaterialSettingsForm`](../../src/devstudio/view/passforms/materialsettingsform.hpp#L42)): material pass の `material_settings` の 10 個の設定です。`material_contract` は engine の契約の combo、`material_filter` は any of / none of の tag(✅UX-11 から engine の `material_tags` の複数選択で、無い tag は `+ new tag` の行に打つ。それまでは `,` 区切りで打つ欄)、`material_variant` はほかの pass が使う variant を候補にした欄、`material_range` は on / off と start / count、`material_outputs` と `material_output_states` は出力ごとの行(名前・型・source・blend・write mask)、`screen_inputs` / `surface_resources` / `material_resources` は binding ごとの行(3 つのどれか・名前・読む資源)です。UX-4a の時点では `gpu_draw_source` だけが **Other (JSON)** の欄に残っていました。✅UX-15a から `gpu_draw_source` も欄になり、Other (JSON) の欄は無くなりました(下の「gpu_draw_source の欄(✅UX-15a)」)。
- **gpu_draw_source の欄(✅UX-15a、`06a9cd2d`)**: **GPU draws** の行の check **Draw from GPU-written commands**(`<prefix>.gpuDrawSource`。[check を作る所](../../src/devstudio/view/passforms/materialsettingsform.cpp#L289))を付けると、その下に欄が出ます(`<prefix>.gpuDrawSource.fields`)。値の意味は第6章の `gpu_draw_source` です。打つ欄はありません。
  - **Layout**(`.gpuDrawSource.layout`): `Default (fixed_state_v1)` / `fixed_state_v1` / `draw_queue_segments_v1`。
  - **Commands** / **Count** / **Segments**(`.gpuDrawSource.commands` / `.count` / `.segments`): buffer の combo です。候補は Pass Editor では宣言済みの buffer([渡す所](../../src/devstudio/view/fullscreenpasswidget.cpp#L2595))、Graph の material pass の欄では root とその scope の `render.buffer` の node です([渡す所](../../src/devstudio/view/graphcanvasmaterialfields.cpp#L370))。pass が名指す buffer は候補に無くても項目に残ります([`MaterialSettingsForm::fillBufferChoices()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L430))。Segments は Layout が `draw_queue_segments_v1` のときだけ押せます。
  - **Max draws**(`.gpuDrawSource.maxDrawCount`。1 から、既定 1)、**Offsets**(`.gpuDrawSource.commandOffset` / `.countOffset`。byte で 4 刻み。0 で元の値に鍵が無ければ書きません)、**Fallback**(`Default (none)` / `cpu_draw_queue`)、**Execution**(`Default (automatic)` / `automatic` / `cpu`)。
  - check を付けると、buffer を名前から選びます: 名前に `count` を含む最初の buffer を Count に、それ以外で `segment` を含まない最初の buffer を Commands に(Count に当たる名前が無ければ Commands 以外の最初の buffer を Count に)。Layout を `draw_queue_segments_v1` にしたときに Segments が空なら、`segment` を含む最初の buffer を選びます([check を付けたときの選び方](../../src/devstudio/view/passforms/materialsettingsform.cpp#L366))。部分一致だけで、buffer の `command_layout` や `host_source` では絞りません。
  - 書く値は欄の値で、欄に無い鍵は pass が持っていたまま残します([`MaterialSettingsForm::gpuDrawSource()`](../../src/devstudio/view/passforms/materialsettingsform.cpp#L479))。check を外すと鍵ごと書きません。Commands か Count が空、または 2 つが同じ buffer(segments の layout で Segments が空)のときは値を作れず、Pass Editor では Save が押せず、Graph の欄では何も送りません(UX-4a の Other (JSON) が JSON でなかったときと同じ扱い)。
  - test: `devstudio_pass_forms_test` の [「UX-15a decision 6: MaterialSettingsForm's gpu_draw_source is fields -- its buffers picked among the buffers given, its layout, counts and offsets -- not JSON」](../../test/devstudio_pass_forms_test.cpp#L344)(読んだ値がそのまま戻ること、選んで変えた値、check を付けたときの名前からの選び方、buffer が無ければ値を作れないこと。実装者の報告から)。Pass Editor 側は `devstudio_fullscreen_pass_test` の既存の [「Studio edits material and raster connections and restores the fullscreen form」](../../test/devstudio_fullscreen_pass_test.cpp#L2435) で、buffer の無い pass で check を付けると Save が押せなくなることを見ます(UX-15a の前は Other (JSON) に `[]` を打って見ていた所)。Graph の欄の候補の結線には専用の test がありません。
  - **残した穴**(実装者の報告): Graph の material pass で `render.buffer` の node が 1 つも無いときに check を付けても、何も送らずに待つだけで、理由は tooltip にしか出ません。
- **Routing**([`MaterialRoutingForm`](../../src/devstudio/view/passforms/materialroutingform.hpp#L19)): preset の `material_routing` の 3 行(`deferred_geometry` / `forward_opaque` / `forward_transparent`)で、どれも preset の material pass を選びます。off なら routing の表を持ちません。
- **Load / Clear / Store** の行は上の G-3 の行を部品にしたもの([`AttachmentOpsForm`](../../src/devstudio/view/passforms/attachmentopsform.hpp#L26))です。
- **Skinned vertex stem** の横に、ほかの stem と同じ選ぶ一覧が付きました。`Engine default` と project の `.vert` です([一覧を置く所](../../src/devstudio/view/fullscreenpasswidget.cpp#L502))。
- **Ray scene** は打つ欄ではなく combo です。`scene`、project が宣言する scene acceleration structure(`get_scene_acceleration_structures` で読みます。`RenderPassAuthoringCapability` に足した読み)、ほかの pass の `ray_query` が読む structure の順です([`showSceneStructures()`](../../src/devstudio/view/fullscreenpasswidget.cpp#L1247))。
- ✅RSX から、N-7 の文字の上限(Pass Editor は 400)に収まるように、行の名前を短くしました: `Add define` → **Add**、`Raster state` → **Raster**、`Scene ray query` → **Ray query**、`Front face` → **Winding**。もとの言い方は tooltip にあります。commit の記録では、測った値は 398 / 400 です。
- test: `devstudio_pass_forms_test` の 5 本(部品ごとに 1 本。[「UX-4a RasterStateForm gives back the raster_state and draw it was given, and a choice is the key it names」](../../test/devstudio_pass_forms_test.cpp#L70)、[「UX-4a AttachmentOpsForm keeps output.color, clear_colors and the depth output as given, and writes the ops chosen」](../../test/devstudio_pass_forms_test.cpp#L142)、[「UX-4a ShaderDefinesForm is shader.defines a row each, and refuses what the engine refuses in its words」](../../test/devstudio_pass_forms_test.cpp#L195)、[「UX-4a MaterialSettingsForm gives back the ten settings it was given, and the rows are the outputs and bindings」](../../test/devstudio_pass_forms_test.cpp#L260)、[「UX-4a MaterialRoutingForm is the three routes, each a material pass」](../../test/devstudio_pass_forms_test.cpp#L439))。UX-4a は GPU シナリオを足していません(commit の stat)。

保存は今までどおり `add_authored_pass` / `edit_preset_pass` で、edit は宣言から変わった field だけを送ります(変えずに
保存すれば送らず、外すと `null` で消します)。

保存が **shader の compile 失敗**で断られる(`render_pipeline_preflight_failed` の message に "Shader compile failed" が
ある)と、form の下の折りたためる欄(`pelican.fullscreenPass.compileLog`)に log 全文が等幅で出ます。行番号が付き、
log が名指す `file:line` は強調されます。notice 行は「shader compile failed; see the log below」だけで、**form に打った
値はそのまま残る**ので、shader を直してもう一度 Save できます。成功すると欄は消えます
([経路](../../src/devstudio/view/fullscreenpasswidget.cpp#L1635))。canvas の帯で直接 stem を変えて compile で断られた
ときは、今も status の 1 行だけです。

**shader が読まない入力を足すと、何をすればよいかを 1 行で言い、その場で直せます(✅Q-14)。**例えば `present` が何も読まない project の shader(入力を宣言せず gradient を書くだけの `shaders/gradient.frag` など)で描いているとき(Blank pipeline の `present` が使う `engine://passthrough` は入力がすべて可変なので、入力を足しても断られません。✅VI-2)、そこへ Pass Editor の Inputs で `gbuffer_albedo` を Add して Save すると、engine は pass の入力と shader の宣言が合わないとして断ります(上の「レンダー設定を編集する」の `shader_input_mismatch`。✅Q-14e)。正しい拒否ですが、以前は engine の文が出るだけで、次に何をすればよいかを言いませんでした。canvas で `present` の node に落としたときは、✅VI-3b から Studio が送る前に止め、`present's shader declares no inputs.` と下の同じ 2 つのボタンを出します(上の「宣言の無い shader の pass に落とすと」。VI-3b より前は送ってから、この拒否を受けていました)。

- **読める 1 行**: `<pass>'s shader (<fragment の stem>) reads only N inputs. To change its inputs, change the shader too.`(N が 0 なら `reads no inputs.`、1 なら `reads only 1 input.`、数が分からなければ `does not read the inputs the pass lists.`)([読める 1 行を作る所](../../src/devstudio/view/shaderinputcontract.cpp#L136))。Frame Plan では status 行(tooltip は engine の全文)、Pass Editor では compile log の欄の下(`pelican.fullscreenPass.shaderContract`)と notice 行に出ます。
- **当たる拒否**: `render_pipeline_preflight_failed` のうち、`error` に `shader_input_mismatch` があるか、message に `Fullscreen pass input` と `does not match shader reflection` の両方があるものだけです([入力の契約の拒否を読む所](../../src/devstudio/view/shaderinputcontract.cpp#L152))。ほかの拒否の出し方は変わりません。Frame Plan で出すのは、送った要求が `input` と `shader.fragment` を持つ `edit_preset_pass` のときだけです。
- **N の出所**: `shader_input_mismatch.shader_pass_inputs` の数 → 無ければ message の `(shader declares N pass inputs)` → 無ければ `get_frame_plan` がその pass に出した `pass_inputs` から数えた数(固定の宣言の数 + 可変の組の max。宣言がその fragment のもの —— 末尾の `.frag` の違いは見ない —— のときだけ。✅VI-3b。[宣言から数える所](../../src/devstudio/view/shaderinputcontract.cpp#L227))→ どれも無ければ数を言わず、`does not read the inputs the pass lists.` の形になります(`pass_inputs` の無い新しい pass など)。VI-3b より前の 3 番目は、fragment の file の宣言行を数えていました。pass と fragment も、engine が `shader_input_mismatch` で名指せばそちらを使います。
- **Use engine://passthrough**(✅VI-3b。`pelican.framePlan.shaderContract.usePassthrough` / `pelican.fullscreenPass.shaderContract.usePassthrough`): pass の fragment を `engine://passthrough` にします。入力がすべて可変(max 8)の engine の shader なので、今の入力(足そうとした入力を含む)をそのまま読めます。Frame Plan は入力の一覧と `shader.fragment` を **1 回の `edit_preset_pass`** で送ります。どちらか片方だけの変更は engine が断るためです([入力と shader を 1 回で送る要求](../../src/devstudio/view/frameplanedit.cpp#L425))。Pass Editor は Fragment 欄に入れて、拒否の後ならそのまま保存します。**file は書きません**。`PELICAN_PASSTHROUGH_INPUT` の define が無ければ出すのは入力 0 で、別の入力を出すには上の「passthrough が出す入力を ○ で選ぶ」です。VI-3b より前のこのボタンは **Make a pass-through shader**(`...makePassThrough`)で、F-3 の Pass-through の雛形から `shaders/<pass>_pass_through.frag` を書いていました。
- **Open the shader**(`pelican.framePlan.shaderContract.openShader` / `pelican.fullscreenPass.shaderContract.openShader`): fragment の file を desktop で開き、足す行を clipboard に入れます。固定の image の宣言の数(`pass_inputs` から。分からなければ 0)の番号から先の入力について、宣言 `PELICAN_DECLARE_INPUT_<i>(<名前>);` と main の中の `vec4 value<i> = PELICAN_TEXTURE_2D_<i>(<名前>, inUV);` で、名前は雛形が付ける名前と同じです。先頭には「shader に無ければ `#version` の後に足す」`#extension GL_GOOGLE_include_directive : require` と `pelican_sets.glsl` / `pelican_view.glsl` の include が注釈で付きます(足す入力に buffer があれば、✅UX-13a から `pelican_resource_ports.glsl` も)([clipboard に入れる行](../../src/devstudio/view/fullscreenshadertemplate.cpp#L389))。status は `The declarations are on the clipboard: add them to the shader and save, and it reloads by itself. Then connect the input again.`(Pass Editor は最後が `Then press Save again.`)で、保存すると下の F-5 の reload で反映されます。fragment が `engine://passthrough` なら、先に project へ複製してそれを開きます(✅VI-3b。上の「engine://passthrough は project に複製してから直します」。この経路は clipboard に何も入れません)。ほかの `engine://` など project の外の fragment では押せません。
- **Save の前の注意(Pass Editor)**: 入力の数が、engine がその pass に出した宣言(`get_frame_plan` の `pass_inputs`)の取れる数 —— 固定の宣言の数と可変の組の max の和 —— より多ければ、Save を押す前に入力の欄の下(`pelican.fullscreenPass.inputContract`)に同じ行と 2 つのボタンが出ます([Save の前の検査](../../src/devstudio/view/fullscreenpasswidget.cpp#L967)、[検査の 1 行を決める所](../../src/devstudio/view/shaderinputcontract.cpp#L261))。可変の組を持つ shader は、max までなら入力を足しても「読まない」ことになりません。ここでの Use engine://passthrough は Fragment 欄を埋めるだけで、送るのは Save です。**検査しないとき**: `pass_inputs` の無い pass(まだ保存していない新しい pass など)と、下書きの Fragment が今描いている shader と違うとき(宣言は今の shader のものなので)。どちらも engine の答えに任せます。✅VI-3b より前は、fragment の file の宣言行を正規表現で数えていました(`PELICAN_DECLARE_INPUT_<i>(` と手書きの `set = PELICAN_SET_PASS_INPUT, binding = <i>`)。今は file を読まないので、`#if 0` の中の宣言行を数えたり、可変の組を 0 と数えたりしません。canvas の drag は、shader が入力を宣言している pass では落とした ○ の binding を宣言と照らしてから送り(✅VI-3a)、宣言の無い shader の pass では送りません(✅VI-3b)。それ以外(`pass_inputs` の無い pass)は送ってから拒否を受けます。

⚠ Save の前の検査は型を見ません(buffer の入力も同じ数に入ります)。Open the shader の行は、宣言の番号が 0 から詰まっていると仮定します。

## 15.10 Render Features

Render Features タブだけを扱う節はありません。この章では次の所に書いてあります。

- feature をループ版に置き換える操作: §15.6 の「feature をループ版に置き換える —— Replace with loop version…(✅LP-4f2)」の段落
- Render Features の編集の Undo / Redo: §15.14
- `edit_render_features` の契約: [第13章「レンダー設定を編集する」](13_editor.md#レンダー設定を編集する)

## 15.11 Rendering settings

### Studio の Rendering settings タブ(✅RSX)

**置き場所**: render-tools の dock の 3 枚目のタブ「Rendering settings」(`pelican.renderingSettings`)です([タブを足す所](../../src/devstudio/view/mainwindow.cpp#L638))。**View ▸ Render Tools ▸ Rendering settings**(`pelican.view.renderTools.rendering_settings`)か `--tab rendering_settings` で前に出せます。右上の `?` はこの章の「レンダー設定を編集する」を開きます。

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
- **Draw sort の XR の並べ方(✅UX-15a、`06a9cd2d`)**: Draw sort の combo の横の combo(`pelican.renderingSettings.draw_sort.xrViewPolicy`)で、`Logical view center` / `Per view` です([作る所](../../src/devstudio/view/renderingsettingswidget.cpp#L440))。Draw sort の項目は opaque / transparent の組だけを持ち、root の値は `xr_view_policy` を外して項目と照合します([`withoutViewPolicy()`](../../src/devstudio/view/renderingsettingswidget.cpp#L63))。組を選ぶと横の combo の値を `xr_view_policy` に必ず書き(`logical_view_center` も明示して書きます)、横の combo を変えると今の draw_sort に `xr_view_policy` だけを替えて送ります([`viewPolicyChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1053))。Draw sort が `Engine default` の間は押せません。root が一覧に無い policy を持てば `Custom: <policy>` の項目で出します。
- **Render strategy の name と parameters(✅UX-15a)**: engine は `render_strategy` に `name` を要ります(`render_strategy requires a name string`)。UX-10 から UX-15a の前までは、値の無い root で provider を選ぶと `{provider}` だけを送っていました(UX-15a の仕様が挙げた不具合)。今は provider から作る項目が、root の値に `name` が無ければ provider の名前を `name` に入れます(root に name があればそれを保ちます。[name を埋める所](../../src/devstudio/view/renderingsettingswidget.cpp#L154))。
  - combo の下に name の欄(`pelican.renderingSettings.render_strategy.name`)と parameters の欄(`.render_strategy.parameters`。JSON object の 1 行で、空なら `parameters` を書きません)があり、Enter か focus を外したときに送ります([`strategyFieldChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1027))。root が render_strategy を持たない間は押せません。name を空にすると送らずに `A render strategy needs a name.` を言って元の name に戻し、parameters が JSON object でなければ送らずに `The render strategy's parameters are not a JSON object.` を言います。
  - provider が parameters の schema を出さないので、parameters は打つ欄のままです(engine に回しました)。
- **Snapshots**: ✅UX-10 から JSON ではなく行です。行ごとに名前(`pelican.renderingSettings.snapshots.row<i>.name`)、after の combo(`.row<i>.after`)、削除(`.row<i>.remove`)があり、**+**(`pelican.renderingSettings.snapshots.add`)は `{name: "opaque_snapshot", after: <候補の先頭>}` の行を 1 つ足します。どの変更も、行の全部を `snapshots` の値にした `edit_rendering_keys` 1 回です。名前が空なら送らずに `A snapshot needs a name.` を言います。
  - ✅UX-12b から、after の combo は engine の `get_render_authoring_context` の `snapshot_after_candidates`(✅FR-1)の名前です([after の候補を読む所](../../src/devstudio/view/renderingsettingswidget.cpp#L729))。tab を読むたびに provider の一覧と並べて聞きます。`post_ldr` は `After opaque (post_ldr)`、ほかは `After <名前>` で、行が候補に無い名前を持っていれば `Custom: <名前>` の項目で出します。候補を返さない engine(と答えが来る前)では、UX-10 の 1 つ(`post_ldr`)だけです。
  - `snapshot.after_unknown@1` は `Snapshot <snapshot> cannot be taken after <after>: this pipeline takes it after <a>, <b> or <c>.`(候補が無ければ `…: this pipeline has no pass to take it after.`)の 1 文を、切らずに notice に出します([拒否の文 `snapshotAfterUnknownLine()`](../../src/devstudio/model/featurerefusalfix.hpp#L111))。
  - UX-12b の前の engine では、Blank から Graph で組んだ pipeline で + を押すと `unknown node: post_ldr` で断られていました。engine の FR-1 から `post_ldr` はその graph の anchor に解決され、`Saved snapshots.` で行が残ります(シナリオ `gui_settings_choose`)。
- **Graph transforms** / **XR**: RSX の着地時は、どちらも JSON の欄と **Apply**(`pelican.renderingSettings.<key>.apply`)でした。✅UX-10 から、XR は view execution の combo(`pelican.renderingSettings.xr.viewExecution`: `Auto (engine default)` / `Sequential` / `Multiview`)が前に出て、xr 全体の JSON の欄は **Advanced**(`pelican.renderingSettings.xr.advanced`)を押したときだけ見えます。✅UX-15a から Graph transforms は行になり、XR に Measured gate が足されました(次の 2 つ)。
- **Graph transforms の行(✅UX-15a)**: 行ごとに provider の combo(`pelican.renderingSettings.graph_transforms.row<i>.provider`。engine の `get_registered_providers` の `graph_transforms`。provider を書いていない既存の行に限って先頭に `Default (builtin.identity_v1)`)、name(`.row<i>.name`)、parameters(`.row<i>.parameters`。JSON object の 1 行で、空なら書きません)、↑ / ↓(`.row<i>.up` / `.down`。順を入れ替えます)、×(`.row<i>.remove`)があります([`fillTransforms()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1133))。
  - **+**(`pelican.renderingSettings.graph_transforms.add`)は、engine が最初に挙げる provider の行を、その provider の名前で足します。名前が preset の行か root の行と重なれば `_2`、`_3`… を付けます([`addTransform()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1254))。engine が provider を返さなければ `builtin.identity_v1` です。
  - どの変更も行の全部を `graph_transforms` の値にした `edit_rendering_keys` 1 回で、最後の 1 行を消すとキーを消します([`transformsChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1206))。欄に無い鍵は行が持っていたまま残します。name が空なら送らずに `A graph transform needs a name.`、parameters が JSON object でなければ `The parameters of graph transform <name> are not a JSON object.` を言います。名前の重複などで engine が断れば、engine の文がそのまま notice に出ます(Studio は先回りして検べません)。
  - graph_transforms 全体の JSON は **Advanced**(`pelican.renderingSettings.graph_transforms.advanced`)の下に読むだけで残ります(`pelican.renderingSettings.graph_transforms`)。
- **XR の Measured gate(✅UX-15a)**: view execution の combo の横に **Measured gate** の check(`pelican.renderingSettings.xr.multiviewAuto`)と minimum gain の spin(`pelican.renderingSettings.xr.minimumGainPercent`。0〜99.99 %、小数 2 桁)があります。
  - check を付けると xr に `multiview_auto: {}` を足し、外すと消します。ほかの鍵(`view_execution`、`profiles`)は保ち、xr が空になればキーを消します([`multiviewAutoChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1064))。spin は `multiview_auto.minimum_gain_percent` を書きます。鍵が書かれていなければ 0(engine の既定)と出し、0 のままなら書きません([`minimumGainChanged()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1078))。check が外れている間は spin を押せません。
  - xr 全体の JSON は Advanced の下で今も書けます(**Apply** は `pelican.renderingSettings.xr.apply`)。空で Apply すると xr を消し、JSON object でなければ送らずに `rendering.xr is not a JSON object.` を言います([`xrApplied()`](../../src/devstudio/view/renderingsettingswidget.cpp#L1093))。check・spin・JSON は同じ鍵を書き、commit のたびに root を読み直して 3 つに出すので、check を切り替えると JSON の表示も変わり、JSON で `multiview_auto` を消すと check が外れます。
  - `multiview_auto.profiles` は check や spin では書けず、Advanced の JSON に打ちます(profiles は `get_frame_plan` の値を写す実測値のため。減らす案は「今の auto_gate から profile を作る」ボタン。実装者の報告)。
  - UX-15a の着地の前の版では、この JSON を読むだけにしていて profiles と minimum gain が書けなくなっていました。統合側の指摘で、書ける形に戻してから着地しました(実装者の報告の追記)。
- **preset の値を灰色で見せる(✅UX-15a)**: preset を選ぶ root では、Shader defines・Snapshots・Draw sort・Graph transforms の欄の上に、preset の config の値を `Preset: …` の灰色の行で出します(`pelican.renderingSettings.<key>.preset.row<i>`。[`fillPreset()`](../../src/devstudio/view/renderingsettingswidget.cpp#L981))。行の tooltip は `Comes from preset <name>; fork the preset to change it` で、ここからは書けません(preset の config を書く RPC はありません)。
  - 値は `get_render_documents` の `kind` が `preset` の文書の `config` から読みます([読む所](../../src/devstudio/view/renderingsettingswidget.cpp#L789))。行の形は、snapshot が `<name>, after <after>`、graph transform が `<name> (<provider>)` と parameters、draw sort が `<opaque> / <transparent>, XR <policy>` で、root 自身が draw_sort を持てば ` (the root's below replaces it)` が付きます([`presetLines()`](../../src/devstudio/view/renderingsettingswidget.cpp#L931))。graph transform は engine が preset の分の後ろに root の分を足すので、preset の行が上、root の行が下です。
  - タブの上に 1 行(`pelican.renderingSettings.preset`)が出て、`This root selects preset <name>: what it holds shows in grey. Fork the preset to change it.` を言います。engine 同梱の preset(`engine://…`)は `get_render_documents` に文書が出ない(engine が `project://` の preset だけを列挙する。実装者の報告)ので値を出せず、この 1 行が `This root selects the engine's preset <reference>: its values are not listed here. Fork the preset to see and change them.` を言います。
- **Material route default**(✅UX-10): `Auto (engine default)` / `Forward` の combo で、`set_material_route_default` を送ります。✅UX-12b から、`material_route_unserved@1` で断られると、notice が `No forward material pass serves the route, so it stays on auto — add a forward material pass first.` の 1 文を切らずに出し、欄は root の値に戻ります([拒否を 1 文にする所](../../src/devstudio/view/renderingsettingswidget.cpp#L235))。
- **送り方**: 欄を変えるたびに `edit_rendering_keys` を 1 回送ります。値は `set: {<key>: <値>}`、既定に戻すと `remove: [<key>]` で、`base_source_digest` は読んだ root の digest です([送る所](../../src/devstudio/view/renderingsettingswidget.cpp#L1473))。送信中の変更は、終わって読み直してから送ります(後の変更が勝ちます)。notice は `Saving <key>...` → `Saved <key>.`(何も変わらなければ `<key> was already so.`)です。
  - `external_modification` で断られたら、`The document changed since this panel read it; reading it again...` を言って読み直し、1 回だけ送り直します。
  - ほかの拒否は notice に `<message> (<code>)` を出し、欄を root の値に戻します([断られた所](../../src/devstudio/view/renderingsettingswidget.cpp#L1546))。
- **preset を選ぶ root**(rendering 節が `pipeline` を持つ)では、Multisampling と Resolver version の欄が押せず、tooltip が engine の reason と文を言います(`preset_root_takes_pipeline_settings_msaa: ...` / `not_in_preset_root_keys: ...`。[reason の表](../../src/devstudio/view/renderingsettingswidget.cpp#L222))。engine が `rendering_key_not_allowed_here` で断ったキーも、その reason で押せなくします。
- **完了と再起動**: 完了は全パネル共通の読み(上の「どのパネルの render の編集も、再起動が要れば同じ形で起動し直します(✅RSX)」)を通ります。EL-3 のキーは世代を組むたびに読み直されるので、ふつうは再起動を求めません。組み直しが live の material の物理の契約だけで断られたときは、RS-1 と同じく `restart_required` が返り(上の EL-3 の段落)、このタブもほかのパネルと同じく起動し直します。この道を GPU で通す test はありません(EL-3 の段落と同じ。未確認)。
- **Undo / Redo**: 編集は render の編集の履歴に積まれ、項目の名前は `Set <key>` / `Remove <key>` です(上の「render の編集を戻す」)。Ctrl+Z / Ctrl+Y もこのタブで効きます([キーを渡す所](../../src/devstudio/view/mainwindow.cpp#L1441))。
- test: `devstudio_rendering_settings_test` の [「RSX decision 3: Rendering settings shows the root's rendering keys, sends one edit_rendering_keys per change, and switches off what a preset root refuses」](../../test/devstudio_rendering_settings_test.cpp#L310)、✅UX-12b の [「UX-12b decision 3: a snapshot's after is picked from the places the pipeline offers, and an unknown one is refused with them」](../../test/devstudio_rendering_settings_test.cpp#L1333)・[「UX-12b decision 5: material_route_unserved@1 is one sentence on what to do」](../../test/devstudio_rendering_settings_test.cpp#L1471)、✅UX-15a の決定ごとの 5 本 [「UX-15a decision 1: a render strategy picked on a root without one is sent with the name the engine requires (its provider's); the name and parameters are fields」](../../test/devstudio_rendering_settings_test.cpp#L833)・[「UX-15a decision 2: draw_sort's XR policy is a combo of its own beside the pair, and Per view is sent with the pair」](../../test/devstudio_rendering_settings_test.cpp#L881)・[「UX-15a decision 3: graph_transforms is a row each -- provider, name, parameters, up / down, x -- and + adds the first provider named after it; the JSON is read-only」](../../test/devstudio_rendering_settings_test.cpp#L909)・[「UX-15a decision 4: xr.multiview_auto is a check with its minimum gain a spin, and the xr JSON under Advanced writes the same key -- each shows what the other wrote」](../../test/devstudio_rendering_settings_test.cpp#L964)・[「UX-15a decision 5: a root that selects a project preset shows the preset's shader_defines, snapshots, draw_sort and graph_transforms as grey rows; the engine's own preset is said to be unlisted」](../../test/devstudio_rendering_settings_test.cpp#L1027)。UX-15a で既存の `UX-10 decision 7` は Draw sort の項目の数を 3 から 2 に、`RSX decision 3` は拒否を起こす手段を check と JSON + Apply の 2 通り(`GENERATE`)にしました。GPU シナリオは `rendering_settings_from_blank`(✅UX-15a で延長)と `gui_settings_choose`(下の「Studio の自動化面」)です。
- **残した穴と手で打つ欄(✅UX-15a。実装者の報告)**: engine 同梱の preset の値は見せられません(engine に回しました)。打つ欄として残るのは、Render strategy と Graph transforms の各行の name(既定は provider の名前が入るので、変えたいときだけ)と parameters(JSON の 1 行。provider が schema を出せばフォームにできる)、XR の Advanced の JSON(主に `multiview_auto.profiles`)です。

## 15.12 Ray layout

### Studio で RT を組む —— Ray layout の節、hit group の割り当て、Graph の RT の節(✅RTL-2s)

2026-10-04 の利用者決定「RT を GUI でしっかり組めるようにする(範囲は絞らない)」の Studio 側で、engine の ✅RTL-1a / RTL-1b / RTL-1c(上の「ray の layout と bindings を読み書きする」)を GUI にしました。RTL-2s の前は、Graph に置ける RT の task は ray group layout の無い形だけで(上の「RT を Graph で組む(✅CT-6s)」)、hit group を物ごとに分けるには layout と bindings の文書を手で書いて engine を起こし直すしかありませんでした。

着地は 3 回です。**RTL-2s a**(`16129867`): Rendering settings タブの Ray layout の節と hit group の割り当て。**RTL-2s b + c**(`c8fd51cb`): b が Graph の RT の task の節・layout の形の雛形・拒否の印・SBT の表示を作り、c が a と b を寄せて残りを埋めました(a と b は 2 人の実装者が並走し、c は途中で止めた作業を別の実装者が引き継いで仕上げました)。**RTL-3s**(`c80bc6a1`): engine の ✅EB-2 / ✅EB-3 / ✅FO-2 の答えに合わせ、Undo の穴を埋め、RTL-2s で unit test だけだった操作をシナリオで通しました(この節の末尾の RTL-3s の段落)。下では、確かめ方を「シナリオで通した」「unit test だけ」「未確認」に分けて書き、RTL-3s の後の状態を書きます。区別が書いていない所はシナリオで通したものです。

**入口**

- **Ray layout の節**(`pelican.rayLayout`。[`RayLayoutWidget`](../../src/devstudio/view/raylayoutwidget.hpp#L35)): Rendering settings タブの form の下にあります([MainWindow が Ray layout の節を置く所](../../src/devstudio/view/mainwindow.cpp#L643))。読むのは `get_ray_layout` と `get_render_documents`(project の RT の task。root の graph の全部の scope の `render.compute` と、feature の `compute_tasks`。[`rayTracingTasks()`](../../src/devstudio/model/raylayoutmodel.hpp#L179))です。タブを前に出すたびに読み直します(ディスクで変わった・壊れた layout を出すため。RTL-2s-c)。
  - layout が無い project では 1 行 `No ray layout: N ray tracing task(s) trace every object with the same closest hit. Enable hit groups to give objects hit groups of their own.` と **Enable hit groups**(`pelican.rayLayout.enable`)だけです。
  - layout がある project では、Ray types / Hit groups / Miss shaders / Record fields / Engine record の表と Hit group assignments が出ます([`fill()`](../../src/devstudio/view/raylayoutwidget.cpp#L681))。
- **Graph の RT の task から**: layout の無い project で RT の task を選ぶと、汎用の欄の上に `Every object hits the same closest-hit shader here. To give objects hit shaders of their own, enable hit groups.` と **Enable hit groups…**(`pelican.graphCanvasRayTracing.enableHitGroups`)が出て、押すと Rendering settings タブが前に出ます。
- **Inspector から**: Add component の一覧で `pelican_ray_instance` が `needs hit groups` と出て(使えない理由は `No ray group layout: enable hit groups first`)、ダイアログの **Enable hit groups...**(`pelican.componentAddEnableHitGroups`)が節の Enable を見せます(`gui_rt_full_from_blank`)。Material の行の **Assign hit group...**(`pelican.inspectorAssignHitGroup`)は、節の割り当ての該当行へ移って `Pick the hit group of <material>.` と言います(`gui_rt_hit_groups_from_blank`)。

**Enable hit groups / Disable hit groups**

- **Enable**([`planEnableHitGroups()`](../../src/devstudio/model/raylayoutmodel.hpp#L334)): 既定の layout(ray type `primary`、triangles の hit group `default`(既定の group)、miss `miss`、record の欄は無し、engine record は `material_index, geometry_index` —— 生成 header が `pelicanRayFetchNormal` を出す組)と、全部の RT の task を layout の形にしたものを 1 つの `edit_batch` で送ります。engine は `restart_required`(`ray_layout_mode_changed`)で答え、Studio は RS-1 の道で起こし直します。起こし直すまでは橙の `Written to <参照>; the engine runs it once it starts again.` と **Restart engine** です。
  - task に `closesthit` が複数あれば ray type を `primary`、`ray_2`… と作り、`miss` が複数あれば `miss`、`miss_2`… にします(仕様の「group を作る」から外した判断。layout の無い形で `closesthit[k]` を選ぶのは shader の `traceRayEXT` の sbtRecordOffset で、物ではないため。RTL-2s a の実装者の報告)。
  - shader は上書きしません。Studio の CT-6s の雛形の組は、横に layout の形の雛形 `shaders/<stem>_layout.{rgen,rmiss,rchit}` を書いてそちらへ向けます。それ以外の raygen は、整数リテラルの `traceRayEXT` の routing を `PELICAN_RAY_TYPE_*`・`pelican_ray_type_count`・`PELICAN_MISS_*` に書き換えた `<stem>_layout.<ext>` を横に書きます。書き換えられない shader はそのまま残し、notice で言います。前のファイルは変えないので、Undo で task が前のファイルに戻ります(新しいファイルはディスクに残ります)。
  - task の形を変える規則は 1 つで([`rayTracingInLayoutForm()`](../../src/devstudio/model/raylayoutmodel.hpp#L215))、Graph の palette が layout のある project に置く task も同じ規則です(最初の ray type を trace、全部の miss、全部の group のセルに同じ stem、procedural の group には intersection も)。
- **Disable**(`pelican.rayLayout.disable`): layout を外して全部の task を layout の無い形に戻します。戻せないとき(hit group が 2 つ以上、any hit か intersection がある、生成 include を使う shader で元のファイルが横に無い)は押せず、tooltip が何が残るかを言います。`gui_rt_layout_edits` で、hit group が 1 つになった後の Disable で layout が外れ、task が layout の無い形に戻り、engine が起こし直すことを見ています。

**表**(Ray types / Hit groups / Miss shaders / Record fields)

- 行ごとに名前の欄(`pelican.rayLayout.<rayType|group|miss>.<名前>.name`)、↑ ↓ ×(`.up` / `.down` / `.remove`)、group には型の combo(Triangles / Procedural。`.type`)と既定の印(`.default`)があり、表の下に **+**(`.add`)があります([`buildNames()`](../../src/devstudio/view/raylayoutwidget.cpp#L738))。名前は打つその場で、規則(`^[A-Za-z0-9_]{1,64}$`、重複なし、record の予約名でない)に外れると赤くなります。
- **足す**: ダイアログ(`pelican.rayLayout.addDialog`)は空いている名前(`group_2`・`shadow`・`miss_2`… の提案)を入れて開くので、打たずに Add できます。group は型を選びます。group と miss は「写す」(既定)か「雛形から」、ray type は「雛形から」(既定。hit group ごとに、その ray type の payload の closest hit を書く)か「写す」です。layout とそれを使う全部の task を 1 つの batch で送ります。
- **消す**: 確認の 1 文に、消える割り当ての数が出ます。1 つの batch です(`gui_rt_layout_edits`)。
- **並べ替え**: ↑ ↓ が layout の順を変えます(`gui_rt_layout_edits`)。
- **改名**: 欄を離れると、layout・全部の task・割り当てと、shader の `PELICAN_RAY_TYPE_<名前>` / `PELICAN_MISS_<名前>` を書き換えます。確認の 1 文にファイルごとの出現数が出ます([`planRename()`](../../src/devstudio/model/raylayoutmodel.hpp#L374))。shader を先に書き、batch が断られたら書き戻します。
  - ✅RTL-2s-c から、書き換えた shader は render の編集の履歴のその項目に載り、Undo 1 回で名前と shader が一緒に戻ります(`RenderEditHistory::attachFiles()`)。`gui_rt_full_from_blank` で `shadow` を `sun` にして絵が変わらず、Undo 1 回で layout・task・raygen の `PELICAN_RAY_TYPE_*` が戻ることを見ています。
- **Record fields**: + で名前と型(`i32` / `u32` / `f32` / `vec2` / `vec3` / `vec4`)を足すと、全部の割り当ての行と procedural の asset の record に 0 で入ります。✅RTL-2s-c から、traced の ray type の hit shader の Studio の区画(`// pelican: shader record` から `// pelican: end shader record`)も書き直します([`fitRayShaderRecords()`](../../src/devstudio/model/raylayoutmodel.hpp#L232))。RTL-2s-c の時点の engine は traced の ray type の hit shader に layout の record の宣言を求めたので、区画を持たない shader を名指して直し方を 1 文で言っていました。engine の ✅EB-3 から hit shader は record を宣言しなくてよくなったので(宣言するなら layout と同じ形。[第6章](06_rendering.md) §6.15 の「shader record の宣言(✅EB-3)」)、✅RTL-3s からはこの事前の注意書きを出さず、区画を書き直すだけです。本当に食い違えば engine の拒否の fields から 1 文を出します(下の「拒否」)。
  - **足す道**は `gui_rt_full_from_blank` で通しました(割り当ての値で cube が明るくなる)。
  - **外す道と Engine record の check** は ✅RTL-3s の `gui_rt_checks` で通しました: 確認の後に record field `value` が外れ、ball の record も消え、hit shader の区画から `float value;` が消えて pipeline が ready、Undo 1 回で戻り ball の値も戻る。Engine record の `geometry_kind` を check すると layout に入り、区画が書き直されて ready。

**Hit group assignments**([`buildAssignments()`](../../src/devstudio/view/raylayoutwidget.cpp#L925))

- `selectable_materials` の 1 件ごとに、label と hit group の combo(`pelican.rayLayout.assign.<名前>.group`)が並びます。先頭の `Default (<既定の group>)` は割り当て無しです。出すのは triangles の group だけです。record の欄があれば、型に合わせた spin(`.record.<欄>`)が付きます。
- procedural の asset の行は procedural の group の combo と asset の record の欄で、`procedural_records` で送ります。
- どの行にも当たらなかった割り当て(`unresolved_bindings`)は、橙の ● と `nothing in the scene uses this assignment's material.` と × です。
- `gui_rt_hit_groups_from_blank` と `gui_rt_full_from_blank` で、Cube と Sphere に別の material を割り当てて 2 つが別の色になることを見ています。
- 同じ `name` の material(基本形の既定 `pelican.primitive.default` が cube と sphere で 2 行)は行の objectName が重なり、自動操作で区別できません。
  ✅RTL-3s から、行の名前は engine の ✅EB-2 の `used_by` / `model` で人が見分けられる形です: `Default (cube) — used by Cube`、`Default (sphere) — used by A, B and 8 more`、`material_2 — used by nothing`(誰も使わない行は灰色)。tooltip は engine の label、EB-2 より前の engine では label のままです([`rayMaterialRowLabel()`](../../src/devstudio/model/raylayoutmodel.hpp#L104))。**unit test だけ**です。objectName は既存のシナリオの名前を変えないため重なったままです。

**そのほかの節の操作**

- **Acceleration structures...**(`pelican.rayLayout.accelerationStructures`): Features タブの既存のボタンを押して、Task Editor の AS の設定を開きます(`gui_rt_layout_edits` で窓が開くことを見ている)。
- **読めない layout**: 表の代わりに赤の `The ray layout <参照> cannot be read: <message> (<code>)` と **Show the document**(`pelican.rayLayout.showDocument`。BS-1s の直す窓)です。`gui_rt_layout_edits` で、ディスクで壊す → 赤の 1 文 → Show the document → `}` を打って Save → layout が戻り pipeline が ready、を見ています。
- **Undo**: 節の編集は `edit_batch` / `edit_ray_layout` として render の編集の履歴に積まれます(項目名 `Edit ray layout`)。
  - RTL-2s では `procedural_records` を書く編集だけは履歴から外し、節独自の **Undo record change** が前に読んだ layout・bindings・record を `edit_ray_layout` で送り直していました(unit test だけ)。
  - ✅RTL-3s から、`procedural_records` を書く編集も同じ履歴に積みます。asset_data は `get_render_documents` で読まず、commit の `procedural_records_before` の JSON 文字列を履歴項目ごとに保持します。Undo / Redo は `{reference, base_digest, content: null, procedural_records}` で record だけを復元し、restore が返す逆操作で次の往復を行います。節の Undo record change のボタンは外しました(`planPutBack()` とその unit test は残っています)。
  - 同じ project 内の engine 再起動後も、この逆操作を持つ履歴は使えます。逆操作を返さない旧 engine との互換経路では、従来どおり `procedural_records_only` の拒否時に説明を出してその項目で止まります。別 project を開いた場合は履歴を破棄します。
  - **シナリオで通した**(`gui_rt_checks`): ball の record の値を ↑ で 1 → Edit > Undo Render Edit で 0 → Redo で 1、どれも pipeline は ready。
  - 残した穴(実装者の報告): records の編集が bindings 文書を新しく作る場合も asset_data とみなしてしまいます。今の plan は record の編集で bindings を新しく作らないので、実害はありません。

**Graph の RT の節**(`pelican.graphCanvasRayTracing`。[`GraphCanvasRayTracingFields`](../../src/devstudio/view/graphcanvascomputefields.cpp#L390))

- layout のある project で RT の task を選ぶと、compute の欄の下に `Ray tracing (ray layout <参照>)` の節が出て、汎用の `ray_tracing` の行は隠れます。layout を読むのは上の節と同じ `get_ray_layout` で(`GraphCanvasModel::rayLayoutRead()`)、Studio が layout のファイルを直に読む所はありません(RTL-2s-c で b のファイル読みを消しました)。
  - **Raygen**(`.raygen`)の combo と New…。
  - **Traced ray types**: ray type ごとの check(`.traced.<type>`)。RTL-2s では、入れると新しい行は最初の traced の行のセルを写し、合わない shader を notice で名指していました(unit test だけ)。✅RTL-3s から([`rayTracedRowFill()`](../../src/devstudio/model/raylayoutmodel.hpp#L250)):
    - check を入れると、その ray type の行を layout の全 group 分作ります。最初の別の traced の ray type と payload が同じなら(雛形の規則では ray type 0 が vec4、それより後は float)その行を写し、違えば、その ray type の雛形の closest hit を group ごとに `shaders/<ray type>_<group>.rchit` に書きます。同じ中身のファイルが既にあればそれを使い(付け直してもファイルが増えない)、別の中身なら次の名前で書きます。procedural の group の intersection は保ちます。書いたファイルは節の notice に 1 文で出し、edit は 1 回です。
    - 外すと、その ray type の `hit_shaders` の行も同じ edit で消します(残すと engine が `hit_shader_untraced` で断る)。
    - RTL-2s の `rayTracedRowNotes()` の注意書きは消しました。engine の ✅EB-3 で、traced になった ray type の hit shader が record を宣言していなくても断られなくなったためです。
    - **シナリオで通した**(`gui_rt_checks`): shadow を check すると mesh / ball の行が 1 つの edit で揃い、ball の intersection は保たれ、`shadow_mesh.rchit` は occluded の payload で `shaderRecordEXT` を持たず、pipeline は ready・problems は 0。Trace shadow toward the light を入れても同じ。外すと traced は 1 つ・行も 1 つで ready。付け直すと同じ `shadow_mesh` を使い `shadow_mesh_2.rchit` はできない。
  - **Miss <名前>**: miss ごとの combo(`.miss.<名前>`)と New…。
  - **Hit shaders (ray type × hit group)**: traced の ray type と hit group の格子で、セル(`.hit.<type>.<group>`)ごとに closest hit・any hit と、procedural の group では intersection の combo と New…(`.hit.<type>.<group>.<stage>.new`)。
  - **Callable shaders**: 行・追加・New…。**Callable records…**(`.callableRecords`)は Render Features の既存の Records のダイアログを開くボタンで、task が `callable_record` を持つときだけ押せます。押せない時の状態は unit test です。**開く道は ✅RTL-3s の `gui_rt_checks` で通しました**(Records のダイアログで `strength` を 1 から `0.25` に打って Apply → `get_ray_callable_records` が 0.25)。
  - **raygen の 2 つの切り替え**: **Trace <2 つ目の ray type> toward the light**(`.traceSecond`)と **Call callable 0**(`.callCallable`)。Studio が書いた raygen の `#define TRACE_SECOND_RAY_TYPE` / `#define CALL_CALLABLE_0` を 0 / 1 に書き換えます(RTL-2s-c。b の着地時は shader を手で 1 文字直していました)。Studio が書いていない raygen では押せず、tooltip が New… を案内します。
    - RTL-2s では shader のファイルを直に書くだけで、Undo では戻りませんでした。✅RTL-3s から、書いた後に render の編集の履歴へ「ファイルだけの項目」(`TRACE_SECOND_RAY_TYPE on in start.rgen` のような名前)を積みます([`RenderEditHistory::recordFiles()`](../../src/devstudio/model/renderedithistory.hpp#L176))。Undo は前の中身を、Redo は後の中身をファイルに書き、engine に restore は送りません(shader の監視が読み直す)。
    - **Trace second の Undo / Redo はシナリオで通した**(`gui_rt_checks`: Edit > Undo Render Edit で `#define … 0`、Redo で 1、節の check は読み直して checked)。Call callable 0 は同じ道で**未確認**です。
  - Max recursion・`acceleration_structure`・`callable_record(s)` は節の中の汎用の部品です。
  - layout の有無が変わった直後で engine が起こし直す前は、`waitingForRestart` の 1 行が出ます。
- **layout の無い project の汎用の欄**: map の鍵が authoring context の `key_choices_from` の編集できる combo になりました(打つこともできます。context の無い engine では空の combo)。

**雛形**([raylayouttemplate](../../src/devstudio/model/raylayouttemplate.hpp#L13))

- layout の形の雛形は 1 組で、Graph の New…、節の Enable、節の「雛形から」が同じものを書きます(RTL-2s-c で a と b の 2 組を 1 つにしました)。stage は raygen / miss / closest hit / any hit / intersection / callable です([`rayLayoutShaderTemplate()`](../../src/devstudio/model/raylayouttemplate.hpp#L83))。
- どれも生成 include `__pelican_ray_group_layout.glsl` を include します。ray type 0(location 0)の payload は vec4(rgb が closest hit の色、w が当たった距離で miss では 0)、ray type i > 0 は float(光との間に物があれば 1)。miss i は ray type i の payload に答えます。
- raygen は最初の ray type を名前の macro で trace し、上の 2 つの切り替えを持ちます。ray type 0 の closest hit は、engine record に `geometry_index` があれば `pelicanRayFetchNormal` の法線で(無ければ当たった位置で)色を付け、既定でない group は色味を変え、record の最初の欄を足します。any hit は全部を受け、intersection は procedural の geometry の原点の半径 1 の球(record に `center` / `radius` があればそれ)、callable は渡された色に色味を付けます。
- hit shader は Studio が持つ record の区画を持ちます(上の Record fields)。既にあるファイルは上書きしません。
  ✅RTL-3s から、record を読まない雛形は record を宣言しません(engine の ✅EB-3 の後)。後の ray type の closest hit、any hit、record に `center` / `radius` を持たない layout の intersection がそうです。後の ray type の closest hit には `hitAttributeEXT vec2 barycentric;` を足しました。無いと procedural の group の intersection と食い違い、engine が `pelican.procedural_hit_attribute_mismatch@1` で断ります(RTL-3s の実装者がシナリオの 1 回目で実物の拒否を受けた)。
- `gui_rt_two_rays_two_misses` が shadow の ray type の miss / closest hit の New… と影で床が暗くなること、`gui_rt_procedural_callable` が procedural の球と callable の色を見ています。

**拒否**

- 1 文の表を広げました: layout / bindings の parser の code、`shader_record_abi_mismatch`、`payload_mismatch`、`generation_rolled_back`。`rayTracingRefusalDescription()` は fields から `(ray type shadow, hit group mesh)` のような場所を後ろに付けます([`rayTracingRefusalDescription()`](../../src/devstudio/model/inspectormodel.cpp#L941))。節の拒否も同じ表から作り、表に無ければ code と fields の 1 行です。
- **fields から作る 1 文(✅RTL-3s)**: engine の ✅EB-3 から `shader_record_abi_mismatch` / `payload_mismatch` に fields が付いたので(§13.5 の「RT の拒否の code と fields」)、1 文を `shader`(`project://` を外す)・`stage`・`expected`・`actual`・`caller`・`closesthit` から作ります。`error_fields` に入って渡された拒否も読みます。
  - `expected` が配列: `Shader shaders/wrong_record.rchit declares a shader record that is not the ray group layout's: declare its fields in order (material_index u32, geometry_index u32, value f32), or take the shaderRecordEXT declaration out` の形(シナリオで出た文)。
  - `expected` が `[]`: 「the ray group layout has none: take its shaderRecordEXT declaration out」。`null`: 「declares a shader record where none is written (its stage miss)」の形。
  - payload は受け取る側の型と caller を名指します。`expected` の無い拒否(EB-3 より前の engine)は従来の表の文です。
  - `pelican.procedural_hit_attribute_mismatch@1` の文も表に足しました(fields が無いので文だけで、セルは赤くなりません)。
- 節の編集が commit で task に断られると、MainWindow が Graph の model に渡して([`markRayTracingRefusal()`](../../src/devstudio/model/graphcanvasmodel.cpp#L4819))、Graph の RT の節の該当するセル・miss・check を赤くします。✅RTL-3s から、拒否の場所に `stage`(closesthit / anyhit)も入れ、セルの closest hit の combo だけが赤くなります。Problems の RT の行も同じ印を付けてから task の node へ飛びます(行き先は §13.1 の Problems のドックの RTL-3s の項)。
- RTL-2s ではどちらも unit test だけでした。**✅RTL-3s の `gui_rt_checks` で通しました**: 格子の shadow × mesh のセルを `wrong_record` に替えると、節の problem に上の 1 文と `(ray type shadow, hit group mesh, stage closesthit)`、そのセルの枠の画素が赤く(隣の ball のセルは赤くない)、engine はセルを元のまま保つ。同じ shader をディスクで root に書くと Problems に 1 行(`fields.stage` が closesthit)、行を選ぶと Graph で `trace` が選ばれ同じセルが赤く、Graph のセルで直すと No problems。
- 残した穴: callable の stage の拒否はセルを赤くしません(場所は `ray_tracing` のまま)。

**SBT の表示**

- Frame Plan の Physical plan の「Ray query acceleration structures」の下に **Hit groups (SBT)** の群が開いた状態で出て、object・hit group(割り当てが無ければ `<group> (default group, no assignment)`)・record の行が並びます([群を作る所](../../src/devstudio/view/frameplanwidget.cpp#L5120))。`gui_rt_layout_edits` で Cube と Floor の行を見ています。
  - RTL-2s では行を `scene_tree` の object ごとに `get_ray_occurrences` を問うて作り(最大 64 回)、64 件を超えた分は `N more objects` の 1 行でした。
  - ✅RTL-3s から、engine の ✅EB-3 の `get_ray_table {limit: 256, offset}` 1 回で読みます([読む所](../../src/devstudio/view/frameplanwidget.cpp#L3843))。`total` が読んだ行より多ければ `N more rows` の行に **Show more**(`pelican.framePlan.sbtShowMore`)が出て、次の offset を読みます。Refresh でも読み直します(それまでは scene の形が変わったときだけ読み直していたので、record の値の変化が表に出なかった。実装者がシナリオの画像で気づいた)。object と hit group の列は中身に合わせて広げます。**1 ページ目はシナリオで通した**(`gui_rt_checks` の Cube の行)。**Show more は未確認**(257 行以上の scene が要る)。
  - 名前付き TLAS ごとの BLAS(`TLAS <名前>: BLAS`)の行は、✅RTL-3s から 1 列目に TLAS の名前を出し、開いた状態にしました。`gui_rt_checks` の fixture の名前付き TLAS `actors` で「TLAS actors」の行があることを通しました(中身の BLAS の列は実装者が画像で見た)。Excluded の群は、除外が skinned / morph / VAT / BLAS 不可の geometry だけで小さい fixture で作れないので**未確認**です。
- Graph の Layers(GL-2b)で RT の task を選ぶと、L3 に `hit groups …`・`ray types traced … of …`・`misses …`・`acceleration structure …` の行が出ます([RT の task の L3 の行を作る所](../../src/devstudio/model/graphcanvasmodel.cpp#L6633))。`gui_rt_full_from_blank` で見ています。

**シナリオ**(下の「Studio の自動化面」に 1 本ずつ): `gui_rt_hit_groups_from_blank`(a)、`gui_rt_two_rays_two_misses` と `gui_rt_procedural_callable`(b)、`gui_rt_full_from_blank` と `gui_rt_layout_edits`(c)、既存の `gui_build_rt_from_blank`(layout の無い形。c で流し直して緑)、✅RTL-3s の `gui_rt_checks` と `gui_material_ray_query_from_blank`。

**残した穴**(RTL-3s の後。実装者の報告)

- RTL-2s で実物を確かめていなかった操作のうち、Callable records… の開く道、Graph の traced の check、Record fields を外す、Engine record の check、`procedural_records` の Undo、TLAS ごとの BLAS の行、節の拒否で Graph のセルが赤くなる所、Problems から Graph へ飛ぶ配線は、✅RTL-3s の `gui_rt_checks` で通しました(上の各項)。
- **未確認**のまま: SBT の表の Show more(257 行以上の scene)、Excluded の群(skinned / morph の fixture が要る)、Call callable 0 の Undo(Trace second と同じ道)。
- **unit test だけ**: 割り当ての行の名前(`used_by`)、preset / feature の pass への Problems の行き先、再起動の後の procedural の record の Undo の拒否の 1 文。
- callable の stage の拒否はセルを赤くしません。`pelican.procedural_hit_attribute_mismatch@1` は fields が無いのでセルを赤くできません(engine に回した)。
- 同じ名前の割り当ての行の objectName は重なったままです。
- Callable records は節の中の表ではなく、既存のダイアログ(数値を打つ欄)を開くボタンです。
- Enable の notes は起こし直しで消えます(restart の行が優先)。
- 実物を通していない道: 同じ文書に feature の task が 2 つあるとき、definition(loop の中)の node(RTL-2s a の報告)。

**手で打つ欄**

| 場所 | 打つもの | 既定値 | 減らす案(実装者の報告) |
|---|---|---|---|
| New Project の Location / Name | フォルダと名前 | — | 既存 |
| Graph の palette の絞り込み | `render.compute` | — | 既存 |
| Inspector の pos の x | 2 つの物を横に並べる値 | 0 | 置くときに重ならない位置へずらす(Add Object の改善) |
| Ray types の名前の欄 | 改名の新しい名前 | 今の名前 | 名前の入力なので除外 |
| 読めない layout の Show the document | 消えた `}` など | ディスクのまま | layout を既定の文書へ戻すボタンなら消せる |
| Callable records… の Records のダイアログ(✅RTL-3s の `gui_rt_checks`) | record の値(`0.25`) | 今の値 | 型が f32 なら spin にする(既存のダイアログの欄。RTL-3s では変えていない) |
| ray query を使う surface の本文(`gui_material_ray_query_from_blank`) | シナリオの file write(人が自分の editor で書く想定) | — | 「ray query を使う surface」の雛形を New material に置けば消せる |

追加のダイアログの名前は提案のまま打たずに済み、record の値は spin、JSON の欄と shader の手直しはありません。RTL-3s は新しい手入力の欄を足していません(check、combo、ボタンだけ)。

**engine に回したもの**(RTL-2s の実装者の報告。RTL-3s の後の状態)

1. `procedural_records` で書いた asset_data を `restore_render_documents` で戻す → ✅EB-2(`e1ba9058`)で入り、Studio は ✅RTL-3s から render の編集の履歴の Undo / Redo で使います(上の「Undo」)。
2. `selectable_materials` に使う object の数か名前 → ✅EB-2 の `used_by` / `model` で入り、Studio は ✅RTL-3s から割り当ての行の名前に使います(上の「Hit group assignments」)。
3. SBT の全行を 1 回で返す RPC → ✅EB-3(`31c390c7`)の `get_ray_table` で入り、Studio は ✅RTL-3s から Frame Plan の表に使います(上の「SBT の表示」)。
4. traced と untraced の record の宣言の非対称(traced の ray type の hit shader は layout の record の宣言が要り、untraced では宣言すると断られた)→ ✅EB-3 で、traced の hit shader は宣言しなくてよくなりました(宣言するなら layout と同じ。untraced の slot の宣言は今も断るが、task が untraced の行を書けないので利用者の shader は入らない。[第6章](06_rendering.md) §6.15 の「shader record の宣言(✅EB-3)」)。Studio は ✅RTL-3s から traced の check で shader を書き換えません。
5. `shader_record_abi_mismatch` / `payload_mismatch` を型付きの RT の拒否にして fields を付ける → ✅EB-3 で入り、Studio は ✅RTL-3s から 1 文とセルの位置を fields から作ります(上の「拒否」)。

**engine に回したもの**(RTL-3s の実装者の報告。どれも Studio 側で回避して進めた)

1. ディスクの root を live と同じ bytes に戻しても、reload の拒否の問題が消えない(観測。原因は確かめていない)。`gui_rt_checks` の fixture で shadow を traced にし、root の shadow × mesh の closest hit をディスクで `wrong_record` に書き換えて Problems に 1 行、同じ所を `shadow_mesh` に書き戻しても、60 秒たって `get_problems` は 1 件のまま。Graph の edit で直すと消える。シナリオは Graph のセルで直す道にした。
2. root の graph の task の reload の拒否で、問題の `document_kind` が `root_rendering` になる(推定。答えの本体は report に残らず、実物の値は見ていない)。graph の node の task なら `root_graph` にしてほしい(FO-2 の material の問題は `root_graph` と `node` が付く)。Studio は `root_rendering` でも Graph へ飛ばす。
3. project の surface(`render_state` の `cull: none`)に、`list_surfaces` が返す routing を `create_material` が `routing variant 'opaque_single_sided' does not match surface pipeline` で断る。Materials のドックで openpbr から project の surface に替える `edit_material` も routing が残って断られる。`list_surfaces` が surface の render_state に合う routing を返すか、create / edit が surface に合わせて組むこと。
4. `create_material` の後、pass の宣言漏れでその場で登録できないと `restart_required` になり、Studio の viewport が「not ready」のまま、起こし直すまで RPC を受けない。FO-2 の起動時と同じく `unusable` と問題の一覧に載せて live のまま続けてほしい(そうなれば起こし直さずに pass を直すだけで済む)。
5. `pelican.procedural_hit_attribute_mismatch@1` を `RayTracingRefusal` の fields 付きに(task / ray_type / group / stage / shader)。

**test**: `devstudio_raylayout_test` の [「RTL-2s decision 1: get_ray_layout as the Ray layout section reads it」](../../test/devstudio_raylayout_test.cpp#L124)・[「RTL-2s decision 2: Enable hit groups writes the layout and every ray tracing task in its form in one edit_batch, beside the shaders before」](../../test/devstudio_raylayout_test.cpp#L159)・[「RTL-2s decision 3: a hit group, a ray type, a miss and a record field reach every task, the bindings and the shaders」](../../test/devstudio_raylayout_test.cpp#L252)・[「RTL-2s decision 4: Hit group assignments from the selectable materials」](../../test/devstudio_raylayout_test.cpp#L336)・[「RTL-2s decision 7: an edit that rewrites procedural records is taken back by sending them again」](../../test/devstudio_raylayout_test.cpp#L362)・[「RTL-2s-c one rule for a task's layout form, the templates' ray type and record block, and the raygen's switches」](../../test/devstudio_raylayout_test.cpp#L383)、`devstudio_render_history_test` の [「RTL-2s-c a rename's shaders ride on its history entry: Undo writes them back before the restore, Redo writes them again」](../../test/devstudio_render_history_test.cpp#L933)、`devstudio_graphcanvas_scene_test` の [「RTL-2s b decision 1: with a ray group layout a ray tracing task's fields are its raygen, a check per ray type, a row per miss and the hit shaders' grid, …」](../../test/devstudio_graphcanvas_scene_test.cpp#L13239)・[「RTL-2s b decision 3: a ray tracing refusal reads as one sentence saying where, and the Graph's section has that place red; …」](../../test/devstudio_graphcanvas_scene_test.cpp#L13402)・[「RTL-2s b decision 2: New... in a project with a ray group layout writes the layout's form -- …」](../../test/devstudio_graphcanvas_scene_test.cpp#L13499)。`devstudio_inspector_test` の Q-6 の期待を「needs hit groups」の文に合わせました。最後の実装者(c)の run で `ctest -R 'RTL-2s' -LE gpu` 10 / 10、`studio_cpu` 741 / 741、シナリオ 6 本が緑でした(`gui_rt_layout_edits` は 1 回目がシナリオ側の文字列の誤りで赤、直して緑)。

**RTL-3s(✅、`c80bc6a1`)**: RT の Studio 側の仕上げです。engine の ✅EB-2 / ✅EB-3 / ✅FO-2 で RTL-2s c の engine への要望 5 件が全部済んだので、その答えに合わせ、Undo の穴を埋め、RTL-2s で unit test だけだった操作を実物で通しました。中身は上の各項に書いたので、ここでは決定ごとの確かめ方だけを並べます(実装者の報告の区別のまま)。

| 決定 | 中身(上の項) | シナリオで通した | unit test だけ | 未確認 |
|---|---|---|---|---|
| 1 SBT の表を `get_ray_table` 1 回で | SBT の表示 | 1 ページ目(`gui_rt_checks`) | — | Show more |
| 2 割り当ての行を見分ける | Hit group assignments | — | 行の名前 | — |
| 3 procedural の record の Undo を engine の restore に | Undo | Undo / Redo(`gui_rt_checks`) | 再起動後の拒否の 1 文 | — |
| 4 raygen の 2 つの check を Undo に | Graph の RT の節 | Trace second(`gui_rt_checks`) | — | Call callable 0 |
| 5 ray type を撃つ・撃たない | Traced ray types、雛形 | `gui_rt_checks` | — | — |
| 6 拒否を fields から | 拒否 | edit の拒否と Problems の行(`gui_rt_checks`) | — | — |
| 7 実物で通していない操作 | Record fields、Callable records、SBT の表示 | 除外の表示を除く全部(`gui_rt_checks`) | — | 除外の表示 |
| 8 Problems の行き先と Materials の `unusable` | §13.1 の Materials / Problems のドック | root の graph の node(`gui_material_ray_query_from_blank`) | preset / feature の pass | — |
| 9 Blank から ray query の material | 下の自動化面のシナリオ | `gui_material_ray_query_from_blank` | — | — |

- **実物で見つけて直した Studio の不具合**: root の graph の task の reload の拒否の Problems の行が Rendering settings に飛んでいた(決定 6。§13.1 の Problems のドックの RTL-3s の項)。
- **決定 9 で engine の答えを実物で確かめた所**: FO-2 の実装者が test していなかった「root の graph の node が作った pass」の分岐で、`get_problems[0]` が `document_kind: "root_graph"`・`node: "render_material_pass"`、`list_materials` が `unusable`・pass `render_material_pass` でした。
- **仕様からずらした点**(実装者の報告): 決定 9 の「cube に付ける」を Ray query を On にした後に移しました(取り込めていない material は bind できない。先に OpenPBR で作って surface を替える道は engine への要望 3 の routing で断られた)。
- test: `devstudio_raylayout_test` の [「RTL-3s decision 5: a ray type checked in Traced ray types takes a whole row in its own payload, …」](../../test/devstudio_raylayout_test.cpp#L460)・[「RTL-3s decision 2: assignment rows of one name read apart by their model and the objects using them, and one nothing uses is grey」](../../test/devstudio_raylayout_test.cpp#L531)、`devstudio_problems_test` の [「RTL-3s decision 6: a shader record's and a payload's refusal read their fields -- …」](../../test/devstudio_problems_test.cpp#L464)・[「RTL-3s decision 8: a material its pass does not declare enough for goes to the pass -- …」](../../test/devstudio_problems_test.cpp#L521)、`devstudio_render_history_test` の [「RTL-3s decision 3: an edit that rewrote procedural records is recorded -- …」](../../test/devstudio_render_history_test.cpp#L982)・[「RTL-3s decision 4: a raygen switch Studio wrote is an entry of its own -- …」](../../test/devstudio_render_history_test.cpp#L1062)。既存の `RTL-2s-c one rule …` の期待(shadow の雛形は record を宣言しない、record field の注意書きを出さない)と `RTL-2s b decision 1` の traced の check の期待を新しい挙動に合わせました。実装者の run で `ctest -R 'RTL-2s|RTL-3s|PB-1s|RU-1' -LE gpu` 34 / 34、`studio_cpu` 762 / 762、シナリオは新しい 2 本と既存の RT の 6 本が緑でした(`gui_rt_checks` は 9 回目、`gui_material_ray_query_from_blank` は 6 回目で緑。途中の赤はシナリオの待ち、上の不具合、engine への要望 1・3・4 の挙動)。

## 15.13 Layers

Layers は Graph タブの詳細表の上に出る節です。§15.6 の次の段落に書いてあります。

- 「下の層を node で見る(✅GL-2b)」 —— 帯と Layers の節
- 「下の層の決定を Frame Plan から書く —— …(✅UX-15b)」 —— Pin / Unpin、Freeze など
- 「Frame Plan にしか無かった操作の残りを Graph に —— …(✅GL-5e)」と「Pass Editor の New pass を Graph へ、…(✅GL-5f)」 —— Layers の右クリックと見出しの右クリック

## 15.14 履歴と Undo

### render の編集を戻す —— Undo / Redo

**render の編集を戻す —— Undo / Redo(✅RU-1 / RU-1b)**: Frame Plan・Pass Editor・Render Features(と、そこから開く Task Editor などの dialog)でした render の編集は、1 つの履歴に積まれ、Undo / Redo で戻せます([`RenderEditHistory`](../../src/devstudio/model/renderedithistory.hpp#L48))。Graph タブの Undo(上の「Undo(✅LP-4c)」)と、Inspector の scene の Undo は、それぞれ別の履歴です。

- **押す所**:
  - Frame Plan の toolbar の、Refresh の右の **Undo** / **Redo**(`pelican.framePlan.undoButton` / `.redoButton`)。tooltip が次に戻す項目を言います(`Undo Edit pass present`、`Nothing to undo` など)([ボタンを置く所](../../src/devstudio/view/frameplanwidget.cpp#L5667))。
  - **Edit ▸ Undo Render Edit** / **Redo Render Edit**(`pelican.edit.undoRenderEdit` / `.redoRenderEdit`)。
  - **Ctrl+Z**(Undo)、**Ctrl+Y** と **Ctrl+Shift+Z**(Redo)。キーは focus のある panel に従います([キーの範囲](../../src/devstudio/view/mainwindow.cpp#L1428))。
    - Frame Plan・Pass Editor・Render Features と、Render Features から開く dialog(Task Editor・Fork・Regions・Records・Scene AS・frame node の順)の中では、この履歴です。
    - Graph タブでは Graph タブの Undo、Inspector では scene の Undo / Redo のボタンと同じです(Inspector のキーは RU-1 で足しました)。
    - focus がテキスト欄にあるときは、その欄の文字の Undo が先に取ります。
- **戻るもの**: render の文書を書く編集です([記録する編集の method の表](../../src/devstudio/model/renderedithistory.cpp#L27))。✅UX-12d から `edit_batch` も記録し、batch ごと 1 回で戻ります(上の「書き手を繋ぎ、1 本の edit_batch で足す(✅UX-12d)」)。pass の編集、Decompose into loop、Replace with loop version…(✅LP-4f2。`apply_graph_template`)、task の追加・編集・削除、render target、feature、frame node の順、route の既定、callable の record、scene の acceleration structure、region の置き換え、✅RSX からの Rendering settings の `edit_rendering_keys`、そして文書を作る・消す編集(Add pass / Remove pass、Fork、Make editable、feature の import)です。
  - `edit_rendering_keys` の項目の名前は、1 つのキーを書けば `Set <key>`、1 つのキーを消せば `Remove <key>`、それ以外は `Edit rendering settings` です([名前を決める所](../../src/devstudio/model/renderedithistory.cpp#L319))。
  - ✅GL-3a / GL-4a から、記録する編集に `fold_nodes`(`Fold group <group>`、`folded: null` なら `Unfold <group>`)・`edit_target_planning`(`Edit planning decisions`)・`verify_fold`(`Verify <group>`)が入りました([fold の編集の項目の名前](../../src/devstudio/model/renderedithistory.cpp#L331))。結果が `running`(検査中の `verify_fold`)の間は、`pending` の 25 ms ではなく 500 ms ごとに問います。この 3 つを履歴から戻す test はありません(未確認)。
  - ✅UX-15b から、Frame Plan の `edit_vulkan_plan_pins`(`Pin backend <candidate>`、`pin: null` なら `Unpin backend`)と `put_vulkan_physical_fragment`(`Freeze physical plan`、`fragment: null` なら `Remove frozen physical plan`)も記録します(記録する method は 22 から 24 に)。この 2 つは `devstudio_render_history_test` の「UX-15b a backend pin and a frozen physical plan are recorded, named, and undone」が RD-1 の restore まで見ます。
  - 項目の名前は `Edit pass present`、`Decompose forward_opaque`、`Add pass ru1_tint` の形です。Replace with loop version… は、engine の受理の応答の `applied.feature` と `applied.template` から `Replace bloom with Bloom loop` になります(応答の前は `Replace with Bloom loop`。title は雛形のファイル名から engine と同じ規則で作ります。[`RenderEditHistory::graphTemplateLabel()`](../../src/devstudio/model/renderedithistory.cpp#L384))。
  - 何も変えなかった編集と、断られた編集は積みません。新しい編集を積むと、Redo の側は空になります。
  - ✅RU-1b から、作る編集の Undo はできた文書(Add pass が書く `passes/authoring/` の fragment のファイルなど)を消し、消す編集の Undo は編集の前に読んだ中身で書き戻します。Redo はその逆です。
  - 戻すときは、今の中身を読んでから `restore_render_documents` を送ります。送る形と CAS は上の「ループと分解の RPC —— engine の契約」の「render 文書を丸ごと読む・戻す」と「undo の手順」のとおりです。
  - 戻した後は、Frame Plan が plan と context を読み直し、Pass Editor がフォームを文書の今の中身で作り直し、ほかの panel には共有の digest で知らせます([戻した後の読み直し](../../src/devstudio/view/mainwindow.cpp#L1473))。route の既定のように engine の再起動が要る編集は、戻すともう一度再起動します。✅RSX から、その文は上の「どのパネルの render の編集も、再起動が要れば同じ形で起動し直します」と同じ `Restarting the engine to apply this change (<code>)...` です。
  - 1 行(`Undid "Edit pass present".` など)が Frame Plan の status 行に出て、status bar にも 8 秒出ます。
- **node を動かした後でも戻せます(✅RU-1b)**: Frame Plan や Graph タブで node を動かすと root の `layout` が書き換わりますが、それより前の編集の Undo は通ります。今の root と「その編集が書いた root」の違いが `layout`(root のものと各 definition のもの)だけなら、戻す中身に後から変わった配置を載せ、今の digest を base にして送ります([配置を載せる所](../../src/devstudio/model/renderedithistory.cpp#L209))。動かした配置は残ります。配置のほかにも違いがあれば、下の「後の編集がある」で断られます。
- **断られるとき**: 後の編集がある(`"<項目>" cannot be undone: <文書> changed since. Undo the later edit first, or refresh.`。履歴は残ります)、予約の場所(`.pelican/`・`assets/` など)に作る・消すことになる、root を消すことになる、のどれかです。後の 2 つと、編集の前に文書を読めなかった編集では、その項目で Undo が止まり、tooltip が `Cannot undo past "<項目>": <理由>` を言います。
- **送信中は待ちます**: render の編集が engine の返事を待っている間は `Wait for the render edit in flight.`、Undo / Redo の最中は `Wait for the Undo or Redo in flight.` が出て、何も送りません。
- 別の project を開くと履歴は消えます。engine が切れたときは、送信中のものだけを捨てて履歴は残します。
- ✅RL-1s から、外のエディタで書き換えた文書を Studio が読み直したときも履歴は消え、Frame Plan の status 行に `— reloaded from disk —` が出ます。Undo で読み直しの前へは戻れません(上の「外のエディタで書き換えた render の文書を拾います」。[履歴を切る所](../../src/devstudio/model/renderedithistory.cpp#L964))。
- ⚠ 分かっている穴(RU-1 / RU-1b の着地時の記録): preset だけを戻したとき(root が変わらないとき)は、Render Features と Graph タブが自分からは読み直しません。`Undid …` の行は、続く Frame Plan の読み直しの status で上書きされることがあります(status bar には残ります)。Render Features と dialog の中のキーは test していません。

Graph タブの編集の Undo は §15.6 の「Undo(✅LP-4c)」の段落にあります。

## 15.15 外で書き換えたファイルの読み直し

### shader と render の文書の読み直し

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
  (MainWindow が [`FramePlanWidget::observeEngineStatus()`](../../src/devstudio/view/frameplanwidget.cpp#L5419) で渡す)の両方から
  覚えます。HW-2s の前は、監視を入れた project で shader を保存しても plan とソケットが読み直されず、watcher の compile 失敗の印も
  読まれませんでした(実装者の報告)。監視が開いていれば、保存しても中身が変わっていなかったときも読み直します(コードの読み)。
  test は `devstudio_assets_dock_test` の「HW-2s decision 7 …」(engine の代わりに test が答えます)で、実際の engine で watcher が
  先に compile する順はシナリオでも通していません(未確認)。
- **一部の pass が断った**(✅VI-3a。答えは `committed: true` でも `refused_passes` が空でない。engine 側は上の「shader を
  読み直させる」の「pass が新しい shader を断ることがあります」): 成功ではなく失敗として扱います
  ([`refused_passes` を読む所](../../src/devstudio/view/shaderreloadfollower.cpp#L198))。Frame Plan は plan を読み直して、上の
  「ソケットは shader の宣言から出ます」の赤い ○ と 1 行、Disconnect / Open the shader を出し、Pass Editor が開いていれば
  log 欄に最初の pass の `reason` が出ます。reload status の `error_code` が `shader_reload_refused@1` のときは、node に当たらない
  失敗の汎用の 1 行「Shader reload failed: ...」は出しません([出さない所](../../src/devstudio/view/frameplanwidget.cpp#L1765))。
- **compile 失敗**(`shader_reload_failed@1`): engine の log の path が node の shader に当たれば、その node の見出しの
  右上に赤い印(`node_shader_error#<pass>`、tooltip に log の先頭行)を描き、当たらない失敗は status 行に 1 行出します。
  **Pass Editor が開いていれば、form の下の log 欄**(保存の compile 失敗と同じ欄)にも log 全文が出ます。engine は前の
  shader で描き続けます。
- **engine が断った**(`shader_reload_unavailable@1`、replay / strict assets の engine): status 行に「A shader file
  changed; this engine does not reload shaders (<理由>).」。この文はこのときだけ出ます。
- それ以外の失敗: 「Could not ask the engine to reload shaders: ...」。

読み直しか失敗の後には `get_status` の `reload.runtime["pelican.shaders"]` を 1 回読み、印を engine の記録に合わせます
([答えの扱い](../../src/devstudio/view/frameplanwidget.cpp#L1778))。

**Graph タブの pass の shader も監視します(✅UX-6a)**: `reload_shaders` を送るのは今も Frame Plan だけです(Studio の中で 1 か所)。✅UX-6a から、Frame Plan はその監視に Graph タブの文書の pass の shader を足します。

- Graph タブは文書を描き直すたびに、全 scope の `render.fullscreen` / `render.raster` の `shader.vertex` / `shader.fragment` のうち `project://` で始まるものを project の中の path にし(拡張子が無ければ `.vert` / `.frag` を足します)、node ごと(root は `<id>`、definition の中は `<definition>/<id>`)に Frame Plan へ渡します([`publishShaderSources()`](../../src/devstudio/view/graphcanvaswidget.cpp#L9748)、[送る役を Frame Plan に寄せる所](../../src/devstudio/view/mainwindow.cpp#L797))。
- Frame Plan は、plan を読んでいるか・前に出ているかによらず、それを監視する path に足します([Graph タブの shader を監視に足す所](../../src/devstudio/view/frameplanwidget.cpp#L1740))。
- 読み直しが通ったとき、または pass が新しい shader を断ったとき、Frame Plan は Graph タブに知らせ、Graph タブは Refresh と同じく文書と plan を読み直します(どのタブが前でも。[`GraphCanvasWidget::shadersReloaded()`](../../src/devstudio/view/graphcanvaswidget.cpp#L10464))。ソケットは shader の宣言から出るので、外のエディタで宣言を足すと、Refresh を押さずに Graph の node のソケットが増えます。
- UX-6a の前は、Graph タブだけで shader を書き換えても Frame Plan のタブを前に出さないと `reload_shaders` が出ず、Graph タブは Refresh を押すまで足した binding を出しませんでした(GB-2 の `gui_build_full` の着地時の description の記録)。
- test: 上の「Studio が作る image は float で、TRANSFER_SRC を付けません(✅UX-6a)」と同じ `devstudio_frameplan_graph_test` の 1 本(decisions 2 and 4)。GPU シナリオは `gui_build_full` の段 5(Graph タブを前にしたまま `shaders/tone_map.frag` を書き換える)です。

**外のエディタで書き換えた render の文書を拾います(✅RL-1s、`e58c8da9`)**: Studio の engine(`--rpc`)は file watcher の gate が閉じているので、Studio の外で root の JSON などを書き換えても engine は気づかず、次の GUI の編集が `external_modification` で断られていました(GB-2 の記録)。✅RL-1s から、Studio が engine の ✅RL-1 の `reload_render_documents` を呼んで取り込みます(engine の契約は §13.5 の「ディスクで書き換えた render 文書を取り込む」)。中身は Qt に依存しない [`RenderDocumentReload`](../../src/devstudio/model/renderdocumentreload.hpp#L52) で、つなぐのは MainWindow です([読み直しの役を MainWindow につなぐ所](../../src/devstudio/view/mainwindow.cpp#L1495))。

- **見張るもの**: engine が答え始めたら `get_render_documents` を読み、その文書(`engine://` と `#` 付きの reference は除く)の file を見張ります。見張り役は上の shader の監視と同じ Frame Plan の 1 つで、文書の file は shader とは別の口で知らせます([文書の file を足す所](../../src/devstudio/view/shaderreloadfollower.cpp#L85))。書き込みは 300 ms 途切れてから 1 回調べます。「消して書き直す」保存のエディタで見張りが外れないよう、変わるたびにその path を足し直します([足し直す所](../../src/devstudio/view/shaderreloadfollower.cpp#L98))。読めない file(置き換えの途中)は 3 回まで調べ直してから、それでも読み直しを頼みます。
- **自分の書き込みは頼みません**: 文書ごとに最後に見た digest(読んだ一覧、Studio の render の編集の commit の `documents`、読み直しの結果)を覚え、file の今の sha256 がそれと同じなら何もしません([`RenderDocumentReload::check()`](../../src/devstudio/model/renderdocumentreload.cpp#L192)、[commit の `documents` を覚える所](../../src/devstudio/model/renderdocumentreload.cpp#L343))。
- **頼む**: 違えば `reload_render_documents` を 1 本だけ送ります。返事が `reload_busy` なら 500 ms 後にもう一度で、理由が `preview_open` なら notice に `Close the scene preview to pick up the change to <file>.` を 1 回出します([`RenderDocumentReload::busy()`](../../src/devstudio/model/renderdocumentreload.cpp#L469))。ticket の結果は 100 ms ごとに問います。読んだ後にディスクがまた変わって `external_modification` が返ったら、もう一度調べ直します。
- **結果**([`RenderDocumentReload::committed()`](../../src/devstudio/model/renderdocumentreload.cpp#L367)):
  - committed: Graph タブの notice と status bar(8 秒)に `Picked up the change to <file>.`。Graph タブは自分の Undo の履歴を消して文書と plan を読み直し、render の編集の履歴(下の「render の編集を戻す」)は消して、Frame Plan の status 行に `— reloaded from disk —` を出し、ほかの panel は Undo の後と同じ道で読み直します([読み直しの後の処理](../../src/devstudio/view/mainwindow.cpp#L1720))。読み直しは undo の項目にならないので、Undo で読み直しの前へは戻れません。
  - `no_change`: 何も出しません。✅IA-1s から、変わったのが image の initial の asset で、engine が `changed_assets` を返さない(IA-2 の前の)ときだけ `<asset> changed — the engine reads it on its next start.` を出します(上の「画像ファイルを image の初期内容に(✅IA-1s)」)。
  - 断られた: notice に `Could not load <file>: fix the file and save again.`、tooltip と Copy message に `The engine keeps the previous version of <file>; fix the file and save again.` と engine の code・participant・reference・message です([`RenderDocumentReload::refuse()`](../../src/devstudio/model/renderdocumentreload.cpp#L430))。engine は前の版で描き続けます。直して保存すれば、もう一度見張りから始まります。file が engine の持つ版に戻っても notice は消えます。
  - `restart_required`: ほかの panel と同じ 1 か所の読み(上の「どのパネルの render の編集も、再起動が要れば同じ形で起動し直します(✅RSX)」)で起動し直します。
- **壊れたままの間**: 読み直しが断られたままの間に Graph などで編集すると engine は `external_modification` で断ります(engine は CAS を緩めません)。そのときの 1 文は `The file on disk changed and could not be loaded — fix it or use Reload from disk.` で、engine の文は 2 行目に続きます([1 文に替える所](../../src/devstudio/model/inspectormodel.cpp#L1116))。Graph の canvas の右クリックに **Reload from disk**(`pelican.graphCanvas.reloadFromDisk`)があり、digest に関係なく `reload_render_documents` を送ります([メニューに足す所](../../src/devstudio/view/graphcanvaswidget.cpp#L3521))。直したのに見張りが拾わなかったときや、断られた理由をもう一度聞くときに使います。
  → ✅RR-1(`4549cae8`)から engine 側が変わりました。断られた後の engine はディスクの版を持ち、その digest の編集と restore が通り、前の版の digest の編集が `external_modification` です(§13.5 の「reload に断られた版を持つ」)。✅RR-1s(`471476d0`)で Studio の側も確かめて直しました。拒否の後に帯が `get_graph_instances` を読み、`live: false`(`held_unparsed` でない)なら
  その `source_digest` を window の共有の digest に流すので、Graph・Frame Plan・Pass Editor・Features・Rendering settings は読み直してディスクの版の digest で送ります(下の「reload に断られた版を持つ間(✅RR-1s)」の決定 4)。
  RR-1s の前は、reload が断られてもどの panel も読み直さず、前の live の digest で送っていました(実装者の報告)。上の 1 文(`The file on disk changed and could not be loaded …`)は、RR-1s から
  読み直しの拒否が `render_document_syntax@1`(JSON でない)のときだけ出ます([印を立てる所(RR-1s)](../../src/devstudio/model/renderdocumentreload.cpp#L462))。中身の壊れでは `external_modification` は engine の文のままです。
- 別の project を開くと、見張るものと覚えた digest を捨てます。engine が切れたときは、送信中のものだけを捨てます。
- test: `devstudio_render_reload_test`(Qt を使わない)の [「RL-1s decision 1: the project's render documents are watched, engine:// is not」](../../test/devstudio_render_reload_test.cpp#L154)、[「RL-1s decision 2: a file holding what Studio last committed asks nothing」](../../test/devstudio_render_reload_test.cpp#L183)、[「RL-1s decision 3: a change asks once, and reload_busy asks again later」](../../test/devstudio_render_reload_test.cpp#L210)、[「RL-1s decision 4: committed, no_change, refused and restart_required」](../../test/devstudio_render_reload_test.cpp#L257)、[「RL-1s decision 4: the render undo history is cut at a reload」](../../test/devstudio_render_reload_test.cpp#L337)、[「RL-1s decision 5 / RR-1s decision 4: while the file stays refused as not JSON, external_modification reads as the disk's sentence; a refused version that is JSON is the editor's base, and the engine's words stay」](../../test/devstudio_render_reload_test.cpp#L384)(✅RR-1s で名前と中身を書き換え。着地時は「RL-1s decision 5: while the file stays refused, external_modification reads as the disk's sentence」)。GPU シナリオは `render_reload_external`(下の「Studio の自動化面」)です。

**組めない構成でも engine を止めない —— placeholder の帯・Reload from disk・Show the document(✅BS-1s、`19be8983`)**: engine の ✅BS-1(`d9811bfc`)/ BS-1b(`40318ffe`)の `--render-fallback`(§13.5 の「起動時に断られた構成を直す」、[第6章](06_rendering.md) §6.16 の「起動時に組めない構成と `--render-fallback`」)を Studio で使います。BS-1s の前は、project の rendering の構成が起動時に組めないと Studio の engine は致命で終わり、viewport は止まったままでした(上の UX-9 の「起動時の致命の 1 文」)。

- **起動引数**(決定 1): Studio が起こす engine の引数は、いつも `--rpc --render-fallback --project …` で始まります([`--render-fallback` を足す所](../../src/devstudio/viewport/studioplayerarguments.cpp#L91))。`PELICAN_STUDIO_PLAYER_ARGUMENTS` の同じ指定は外して 1 回だけ付け、追加引数に `--headless` があるときも付けます。
- **帯**(決定 2): `get_status.render_pipeline.state` が `fallback` の間、viewport の上(workspace の layout の中)に 1 行の帯 `pelican.renderFallbackNotice` が出ます([`RenderFallbackNotice`](../../src/devstudio/view/renderfallbacknotice.hpp#L38)。中身は Qt に依存しない [`RenderFallbackSession`](../../src/devstudio/model/renderfallback.hpp#L45))。帯が読むのは、Play の帯と同じ 1 回 parse した `get_status` の返事です([帯へ get_status を渡す所](../../src/devstudio/view/mainwindow.cpp#L2067))。帯が出たときと文が変わったときは status bar にも同じ文を 15 秒出し、消えて state が `ready` なら status bar が `The project's rendering setup runs again.` を 8 秒言います。
  - 文(`.text`)は `Showing a placeholder: the rendering setup was refused — <code の 1 文> in <file>. Fix it, then Reload from disk.` です([`renderFallbackSentence()`](../../src/devstudio/model/inspectormodel.cpp#L2276))。code の 1 文は graph の拒否の表、無ければ `knownRefusalDescription()` の表から取り、どちらにも無ければ engine の message の 1 行目です(message が code で始まらなければ後ろに `(<code>)`)。file は refusal の `document`(CT-2。`project://` を外す)、無ければ `root`、それも無ければ `the project's render documents` で、`definition` があれば `definition <名前> of <file>` です。シナリオの例: `Showing a placeholder: the rendering setup was refused — Port data of sim needs element (graph.port_field_missing@1) in passes/main_rendering_config.json. Fix it, then Reload from disk.`
  - tooltip(と Copy message)は、文・engine の message・`rendering root: <root>` です。同じ読みが続いても知らせ直しません。
  - ボタン: **Reload from disk**(`.reload`)、**Open in Graph**(`.openGraph`。refusal が `node` を名指すときだけ。Frame Plan の dock を前に出して Graph タブにし、その node を見せる)、**Open file**(`.openFile`。file を名指すときだけ。shader と同じ開き方で、自動化の下では log に `automation: shader source open <path>` の 1 行)、**Show the document**(`.showDocument`。下の JSON でない文書のときだけ)、**Restart engine**(`.restart`。下の再起動の案内のときだけで、そのときほかのボタンは隠れます)。
  - viewport は engine の一色(暗い赤紫。実装者の報告では rgb 134, 56, 111 前後)です。
  - ✅PB-1s(`93934318`)から、帯の文は末尾に ` See Problems.` が付きます([付ける所](../../src/devstudio/view/renderfallbacknotice.cpp#L343))。tooltip と Frame Plan の status 行の文には付けません。
    status bar の件数のボタンを押すと Problems のドックが前に出ます(§13.1 の「Studio の Problems のドック」)。
    また engine の ✅PB-1 から起動の間の `material_route_unserved@1` も fallback になるので、その文は Frame Plan の既存の文(どの route を描く pass が無いか、何を足すか)に
    `(material_route_unserved@1)` を付けたものです([route_unserved の文を使う所](../../src/devstudio/model/inspectormodel.cpp#L2294))。
  - ✅RR-1s(`471476d0`)から、同じ帯は reload に断られた版を engine が持つ間にも出ます。fallback と重なれば fallback の文が先です(下の「reload に断られた版を持つ間(✅RR-1s)」)。
- **Frame Plan の status 行**(決定 2): `get_frame_plan` の答えが `render_pipeline`(fallback の間だけ付く)を持つと、status 行が赤で帯と同じ文を言い、tooltip は文と engine の message です。そのとき編集用の文書(authoring context など)は読みません([fallback の plan を言う所](../../src/devstudio/view/frameplanwidget.cpp#L1684))。project の plan が来ればその行を消します。Frame Plan の図は fallback の世代の plan(pass 1 つ)です。
- **読み直し**(決定 3): fallback の間は、render の編集の答えが committed のとき(Graph などの ticket の poll の答えも)、読み直しの commit、読み直しの拒否の後に `get_status` を読み直し([`RenderFallbackSession::wantsStatusAfter()`](../../src/devstudio/model/renderfallback.cpp#L32))、`ready` を読むと帯が消えます。rejected / accepted の答えでは読みません。engine が切れたら帯を消します。
  - Reload from disk は RL-1s の `RenderDocumentReload` の読み直しを送ります。直す前は断られ(Graph の notice に `Could not load …`)、帯は残ります。外のエディタで直して保存すると RL-1s の見張りが先に拾って読み直すので、ボタンを押さずに `ready` に戻ります(シナリオ)。
- **JSON でない文書**(決定 7・8。engine の BS-1b): `render_document_syntax@1` は `<document> is not valid JSON at line <L>, column <C>: <reason> (render_document_syntax@1).` の 1 文です(`at line …` は行と列が両方あるときだけ。fields は code の横か、message の code の後ろから読みます。[`renderDocumentSyntaxDescription()`](../../src/devstudio/model/inspectormodel.cpp#L2318))。帯ではこの文が file を言うので `in <file>` は付けません。同じ文を `knownRefusalDescription()` も返し、RL-1s の読み直しの拒否では tooltip の先頭に出ます(notice の行 `Could not load <file>: …` は変わりません)。
  - **Show the document**(決定 8): `get_render_documents` で文書を読み、窓 `pelican.renderDocumentDialog` の `.text` に生の文字列を出して、parser が止まった行・列にカーソルを置きます([`RenderDocumentRepair`](../../src/devstudio/model/renderfallback.hpp#L156)、[窓を作る所](../../src/devstudio/view/renderfallbacknotice.cpp#L208))。見出しは `<reference> as it is on disk. Fix it and Save: the file is replaced whole. This is for a document that is not JSON at all; the Graph and the Frame Plan edit it otherwise.` です。
  - 窓の notice(`.notice`)は、打つたびに全体を parse して `Not valid JSON yet at line L, column C: <reason>` か `Valid JSON. Save writes it to <reference>.` を言い、JSON でない間は **Save**(`.save`)が押せません([`RenderFallbackNotice::checkDialogText()`](../../src/devstudio/view/renderfallbacknotice.cpp#L259))。
  - Save は `restore_render_documents {documents: [{reference, base_digest: <読んだ壊れた bytes の digest>, content}]}` を 1 回送り、ticket を 100 ms ごとに問います([`RenderDocumentRepair::save()`](../../src/devstudio/model/renderfallback.cpp#L227))。文書の改行が CRLF なら CRLF に戻して送ります。committed で窓が閉じます。commit は窓が飲み込まずに MainWindow の commit の処理へ流れ、各 panel が読み直し、`get_status` も読み直します。断られたら 1 文を窓の notice に出し、窓は開いたままです。**Cancel**(`.cancel`)で閉じます。
- **再起動の案内**(決定 4): 直した構成の読み直しが `Module '<型>' was first requested after the runtime module graph was frozen` で断られたら(module の要る feature を直した構成で初めて有効にする場合。engine の BS-1 の残る穴)、帯が `Restart the engine to enable <型の短い名前>: the rendering setup needs it, and this engine was started without it.` と **Restart engine** を出し、押すと RSX と同じ `MainWindow::requestEngineRestart()` で起こし直します([`renderRestartFeature()`](../../src/devstudio/model/inspectormodel.cpp#L2339))。名前は message の型の名前(例 `UiSystem`)で、feature 名(`ui` など)ではありません(engine の拒否が feature 名を言わないため)。シナリオでは通していません(test だけ)。
  → engine の ✅PB-1(`a55d1eaf`)で、本番の feature の reload の拒否の文面がこの文と一致していなかったことが分かりました。今はどちらも `render_feature_requires_restart@1 {feature?, module}` です。
  code で判定する形への直しは PB-1s(作業中)。§13.5 の「今の問題の一覧を読む」。
  → ✅PB-1s(`93934318`、決定 6)で直しました。[`renderRestartFeature()`](../../src/devstudio/model/inspectormodel.cpp#L2339) は code が
  `render_feature_requires_restart@1`([`RenderFeatureRequiresRestartCode`](../../src/devstudio/model/inspectormodel.hpp#L479))のときだけ、code の横・`error_fields`・message の code の後ろの fields から `feature` を読みます。
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
  - `live: false` のとき: 帯の文は `Showing the saved version; the picture is still the previous one — <Problems の 1 文>` で、Problems の 1 文が無ければ `… previous one.` で終わります([`live: false` の文](../../src/devstudio/model/renderfallback.cpp#L148))。
    1 文は Problems の一覧の engine の行のうち、error で、source が `startup` でなく、shader でない最初の行の文です([`RenderFallbackSession::observeProblems()`](../../src/devstudio/model/renderfallback.cpp#L72))。
    tooltip の 2 行目は `The Graph and the other panels show the version on disk and edit it; the engine draws the version before until a fixed version is taken in.` です。
    ボタンは Reload from disk と、行が node を名指せば Open in Graph、文書を名指せば Open file です。シナリオの例: `Showing the saved version; the picture is still the previous one — Port data of sim needs element …`。
  - `held_unparsed: true`(ディスクの root が JSON でない)のとき: `<file> is not valid JSON; the Graph shows the last version that was.` です([`held_unparsed` の文](../../src/devstudio/model/renderfallback.cpp#L135))。
    file は読みの `source_reference`(無ければ Problems の行の文書)。Show the document が出て、Problems の行が `render_document_syntax@1` の行・列を持てばそこにカーソルを置きます。tooltip の 2 行目は Problems の 1 文です。
  - 帯は 1 つのことだけを言います。並びは 再起動の案内 → fallback の文 → `held_unparsed` → `live: false` です([帯の文を組む所](../../src/devstudio/model/renderfallback.cpp#L97))。PB-1s の ` See Problems.` はどちらの文の末尾にも付きます。
  - RR-1 の前の engine(答えに `live` が無い)からは何も言いません。fallback から `ready` に戻ったときは覚えた読みを捨て、読み直すまで何も言いません。
  - 読む時: engine が答え始めた時、reload の拒否と commit、Show the document の restore の commit、fallback から `ready` に移った時、Problems の engine の行の id が変わった時、断られた版を持つ間に `committed` の答えが来た時です。読みは同時に 1 本で、途中の依頼は後に 1 回続けます。
  - 帯が消えて state が `ready` なら、status bar は BS-1s と同じ `The project's rendering setup runs again.` を 8 秒言います(コードの読み。[消えたときに言う所](../../src/devstudio/view/mainwindow.cpp#L352))。
- **Show the document は restore**(決定 2): Save は帯から開いても Problems から開いても `restore_render_documents {documents: [{reference, base_digest: <get_render_documents の digest>, content}]}` です(上の BS-1s の決定 8 と同じ道。[`RenderDocumentRepair::save()`](../../src/devstudio/model/renderfallback.cpp#L227))。Studio が render の文書を直接書く道はもうありません(§13.1 の「Studio の Problems のドック」の「Show the document の Save はいつも restore です」)。
- **断られた node は Graph で**(決定 3): Problems の node の行は、source に関係なく Graph タブでその node を選びます。Graph はディスクの版を読み直しているので、その node があります。definition の中で Graph が見せられないときだけ Show the document の node の位置へ飛び、JSON でない root の行は今までどおり Show the document の行・列です(§13.1 の「Studio の Problems のドック」のクリック)。
- **保存した版の digest で編集**(決定 4): 読みが `live: false`(`held_unparsed` でない)を返すと、帯は hook `held_digest` でその digest を渡し([渡す所](../../src/devstudio/view/renderfallbacknotice.cpp#L170))、MainWindow が window の `RenderConfigDigest` に publish します([つなぐ所](../../src/devstudio/view/mainwindow.cpp#L343))。
  購読している Graph・Frame Plan・Pass Editor・Features・Rendering settings が自分の文脈を読み直し、次の構造の編集はディスクの版の digest を base に送ります。今持っている digest と同じなら何も起きません(Q-13 の `RenderConfigDigest` の既存の挙動)。
  `held_unparsed` の読みと読みの失敗では渡しません(panel は前の読みのまま)。RR-1s の前は、reload が断られてもどの panel も読み直さず、RR-1 の後の engine では次の編集が `external_modification` で断られるところでした(実装者の報告)。
  実装者は、graphattachment・proceduralasset・material や preset の pass の編集は送るたびに文脈を読み直すか Frame Plan の文脈の digest を使い、live の digest を独自に覚えている所は無かった、と報告しています。
  - RL-1s の決定 5 の 1 文 `The file on disk changed and could not be loaded — fix it or use Reload from disk.` は、読み直しの拒否が `render_document_syntax@1` のときだけ出ます([印を立てる所](../../src/devstudio/model/renderdocumentreload.cpp#L462))。中身の壊れでは panel がディスクの版で編集できるので、`external_modification` は engine の文のままです。
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

## 15.16 Problems

### Studio の Problems のドック —— 今の問題の一覧と、直す場所へ飛ぶ行(✅PB-1s)

✅PB-1s(`93934318`)から、Studio に **Problems のドック**(`pelican.problemsDock`。中身は
[`ProblemsDock`](../../src/devstudio/view/problemsdock.hpp#L42)、一覧と読み取りは Qt を使わない
[`ProblemsSession`](../../src/devstudio/model/problemsmodel.hpp#L136)、1 行は [`ProblemRow`](../../src/devstudio/model/problemsmodel.hpp#L37))があります。
project の今の問題を 1 行 1 件で並べ、行を押すと直す場所へ飛びます。出どころは engine の ✅PB-1(`a55d1eaf`)の `get_problems`
(§13.5 の「今の問題の一覧を読む」)と、Assets のドックの照合(上の「Studio の Assets のドック」)の 2 つです。利用者の案(2026-10-04)は
VS Code の Problems のような一覧で、1 行に「どの JSON の何行目か・何が悪いか・Studio のどこで直すか」を出し、クリックで Graph の node や
生の文字の該当行に飛ぶ、というものです(仕様書の記述)。PB-1s の前は、問題は BS-1s の帯と Frame Plan の status 行、各 panel の notice、
shader の診断、Assets のドック、Frame Plan の致命の行にばらばらに出ていました(仕様書の記述)。

- **開き方**: 既定の配置では下の段のタブで、Output・Engine Log・**Problems**・Frame Plan・Pass Editor の順に並び、前に出るのは Frame Plan です
  ([既定の配置で Engine Log の次に置く所](../../src/devstudio/view/mainwindow.cpp#L3976))。**View > Problems**(`pelican.view.problems`)で前に出します
  ([View > Problems](../../src/devstudio/view/mainwindow.cpp#L1917))。View > Panels でも開閉できます(dock は 10 個になりました)。
- **列**(決定 1): Severity / Problem / Document / Line / Fix in の 5 列です([列の見出し](../../src/devstudio/view/problemsdock.cpp#L134))。
  - Severity は `Error` / `Warning` と印。Document は project の相対 path(`passes/main_rendering_config.json`。`engine://` の文書はそのまま)、
    Line は `12:5`(列が無ければ `12`、行が無ければ空)です。
  - Problem は code の 1 文で、各 panel が拒否を読むのと同じ表から作ります([`problemSentence()`](../../src/devstudio/model/problemsmodel.cpp#L55))。
    `render_document_syntax@1` は BS-1s の構文の 1 文、`graph.*` は graph の拒否の表(文書は列にあるので文には入れない)、
    `material_route_unserved@1` は Frame Plan の既存の文に `(material_route_unserved@1)`、
    `shader_reload_failed@1` は `<file> does not compile: <engine の message の 1 行目> (shader_reload_failed@1)`、
    `render_feature_requires_restart@1` は `The rendering setup turns on <feature>, whose runtime module <module> this engine was started without; restart the engine (render_feature_requires_restart@1).` です。
    ほかは `knownRefusalDescription()` / `materialRefusalDescription()` の表で、どれにも無ければ engine の message の 1 行目(code が入っていなければ後ろに `(<code>)`)です。
  - Fix in は直す場所の名前です: `Graph: <node>` / `Text: <node>` / `Text` / `Materials: <name>` / `Shader file` / `Assets: <file>` / `Rendering settings` / `Features` / `Restart the engine`。
    飛び先が無い行は `—` です([直す場所を決める `placeOf()`](../../src/devstudio/model/problemsmodel.cpp#L127))。
- **並べ替えと絞り込み**(決定 1): 見出しの click で並べ替えます。既定は Severity(error が先、同じ重さなら文書と行の順)、Document の列は文書・行・列の順、
  Line の列は数の順です([並べ方](../../src/devstudio/view/problemsdock.cpp#L38))。上の combo `.severity`(All / Errors / Warnings)で重さを、欄 `.filter` で文字を絞ります。
  文字は問題の文・文書・code・Fix in・engine の message のどれかに(大文字小文字を区別せず)含まれれば残ります。選んでいた行が隠れたら選択を外します。
- **選んだ行の 1 行**: 一覧の下の `.detail` が `<document>:<line>:<column> node <node> — <問題の文>` を言い、tooltip に engine の message と `Met: <source>` が続きます。
  件数は上の `.count` です。一覧が読めなかったときは `.notice` に `Could not read the engine's problems: …` が出ます。
- **集める**(決定 2):
  - engine の一覧: 誰が送った `get_status` の返事でも見て、`problems.revision` が前に読んだものと違うときだけ `get_problems` を送ります
    ([`ProblemsSession::observeStatus()`](../../src/devstudio/model/problemsmodel.cpp#L358))。読みは同時に 1 本で、途中で頼まれたら答えの後に 1 回だけ続けます
    ([`ProblemsSession::readNow()`](../../src/devstudio/model/problemsmodel.cpp#L370))。`get_status` に `problems` が無い engine(PB-1 の前)からは読みません。
  - revision を待たずに読む時: RL-1s の読み直しの答え(notice・拒否・commit)、Show the document の restore の commit、一覧に行がある間に `committed` を言う答え(render の編集など。RL-1s が `"committed"` を含む答えだけを parse する所で見る)の後です
    ([読み直しの答えの後に読む所(PB-1s の注記)](../../src/devstudio/view/mainwindow.cpp#L1541))。
  - 1 秒の間に誰も `get_status` を読まなければ、ドックが自分で送ります([`StatusPollMs`](../../src/devstudio/view/problemsdock.hpp#L62))。engine が自分の監視だけで出会う問題
    (`hot_reload` の source。shader の監視など)はこれで拾います。
  - Assets のドックの照合: 照合の結果を出すたびに hook `checked` で渡され([渡す所](../../src/devstudio/view/assetsdock.cpp#L796))、severity が `warning` / `error` の issue を行にします
    (`info` は入れません)。文は `<種類> against <manifest>: <message>` です([`assetProblemRows()`](../../src/devstudio/model/problemsmodel.cpp#L294))。
  - shader: engine の `shader_reload_failed@1` の行を正とし、Studio の shader の診断はそのままです(二重にしません)。
  - 入れないもの: 各 panel の編集の拒否(その場の notice のまま。engine の一覧にも入らない)と、engine が止まった後の致命の行(そのとき `get_problems` は無い)。
  - engine が切れると engine の行を消し、照合の行は残します。
- **クリック**(決定 3。[`MainWindow::openProblem()`](../../src/devstudio/view/mainwindow.cpp#L1589)):
  - node を名指す行(`document_kind` が無いか `root_graph` / `definition`): source に関係なく Graph タブでその node を選び(definition の中なら潜る)、詳細欄を開きます。
    Fix in は `Graph: <node>` です([node の行の飛び先を Graph にする所(RR-1s)](../../src/devstudio/model/problemsmodel.cpp#L154))。Graph がまだ文書を読んでいなければ、読んだ時に選びます。
    Graph が definition の中の node を見せられないときだけ、Show the document を開いて文字の `"<node>"` にカーソルを置きます([definition の中の node を文字で開く所](../../src/devstudio/view/mainwindow.cpp#L1622))。
    ✅RR-1s(`471476d0`)の前は、source が `reload_rpc` / `hot_reload` の node は Show the document の文字の位置へ飛び、Fix in は `Text: <node>` でした(下の「仕様から外れた判断」の 1)。
    RR-1s から、reload に断られた後の Graph は engine がディスクの版を返す読み(RR-1 の `live: false`)を読み直すので、その node があります(下の「reload に断られた版を持つ間(✅RR-1s)」)。
  - 行と文書がある行(JSON でない文書など): Show the document をその行・列で開きます([`RenderFallbackNotice::showDocumentAt()`](../../src/devstudio/view/renderfallbacknotice.cpp#L193))。
    BS-1s と同じ窓で、行が無ければ `find`(node の id)の最初の位置、それも無ければ末尾にカーソルを置きます。
  - `document_kind: "root_rendering"`(`material_route_unserved@1` など)は Rendering settings タブ、`feature` は Features タブを前に出します。
  - `shader`: shader の file を外のエディタで開きます(BS-1s の Open file と同じ開き方)。行には飛べないので、status bar が `<document>: line <N>` を言います。
  - `material` の文書か `material.*` の code: Materials のドックを前に出し、その material を選びます([`MaterialsDock::revealMaterial()`](../../src/devstudio/view/materialsdock.cpp#L1764))。
  - `material.pass_contract_unmet@1`(engine の ✅FO-2。✅RTL-3s `c80bc6a1` から): material の分岐より先に判定し、**pass の側**へ飛びます([pass の側へ決める所](../../src/devstudio/model/problemsmodel.cpp#L147))。FO-2 の着地の時点では `fields.material` があるので Materials のドックへ飛んでいました(FO-2 の実装者のコードの読み)。
    - `document_kind: "root_graph"` で `node` があれば Graph のその node(Fix in `Graph: <node>`)。
    - `fields.pass`(無ければ `passes` の最初)があれば、新しい行き先 `GraphPass`(Fix in `Graph: <pass>`)で、Graph タブでその pass を選びます(plan の pass を探し、無ければ root の node。[`openPlanPassOnGraph()`](../../src/devstudio/view/mainwindow.cpp#L1708))。
    - それ以外は `root_rendering` → Rendering settings、`feature` → Features、`engine://` でない文書 → Text です。
    - 問題の文は `Material <名前> queries the scene's acceleration structure, and pass <pass>, which draws it, does not declare ray_query: turn Ray query on for the pass (material.pass_contract_unmet@1).` です。
    - **root の graph の node の場合はシナリオで通した**(`gui_material_ray_query_from_blank`)。preset / feature の pass への行き先は **unit test だけ**です。
  - RT の task の拒否(✅RTL-3s): root の graph の task の reload の拒否の行が Rendering settings に飛んでいたので、task の RT の拒否は `document_kind` が root / `root_graph` / `root_rendering` / 空のどれでも Graph の task の node(`node` が無ければ `fields.task`)へ飛ぶようにしました。engine が root の graph の task の拒否に `root_rendering` を付けているらしい、というのは実装者がシナリオの挙動から推した話で、答えの値そのものは見ていません(未確認)。
  - 照合の行: Assets のドックを前に出し、そのファイルの行を開いて選びます([`AssetsDock::revealFile()`](../../src/devstudio/view/assetsdock.cpp#L875))。
  - `render_feature_requires_restart@1` の行: クリックではどこにも飛ばず、選ぶと一覧の下に **Restart engine**(`.restart`)が出ます。押すと `requestEngineRestart()` です。
  - 行は押した時に飛びます(`itemPressed`。keyboard の Return は `itemActivated`)。同じ行の 600 ms 以内の 2 回目は飛びません([押下で飛ぶ所](../../src/devstudio/view/problemsdock.cpp#L175))。
- **Show the document の Save はいつも restore です**(✅RR-1s、`471476d0`): Problems から開いた窓も帯から開いた窓も、Save は `restore_render_documents` を
  `get_render_documents` が返した digest を base にして送ります([Save が restore を送る所(RR-1s)](../../src/devstudio/model/renderfallback.cpp#L238))。engine の ✅RR-1 から、reload に断られた後の engine の base は
  ディスクの版なので、その digest の restore が通ります。Studio は render の文書のファイルを自分で書きません(D0)。
  PB-1s(`93934318`)の着地時は、source が `startup` でない行から開いた窓の Save は restore を送らずにファイルを丸ごと書き、RL-1s の監視が拾って `reload_render_documents` を送っていました
  (書けなければ窓の notice が `Could not write <reference>: <理由>`。下の「仕様から外れた判断」の 2)。RR-1s でこの道(`RenderDocumentRepair::open()` の `to_file`、`Transport::write_file`、MainWindow の書く hook)は外しました。
- **件数**(決定 4): status bar の右に件数のボタン `pelican.problemsCount` があり、`2 errors, 1 warning` / `1 error` / `No problems` を言います
  ([`problemsCountLine()`](../../src/devstudio/model/problemsmodel.cpp#L322)、[ボタンを作る所](../../src/devstudio/view/mainwindow.cpp#L614))。押すと Problems が前に出ます。engine が無い間は隠れます。
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
     → コードで確かめました。RR-1 の前の restore の CAS は root を editor が持つ版の digest とディスクの両方で比べ([restore の CAS の root の比べ](../../src/core/communication/renderconfigeditor.cpp#L7235))、
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
    → engine の ✅FO-1(`a4d7760a`)から、起動時に読めない material 文書は `document_kind: "material"`・`document: "project://<path>"`(path。material の名前は持たない)、`fields.path` で一覧に入ります。元の material に落とした model は `material.binding_unresolved@1` で、`document` を持たず `fields.model` で model を指します(§13.5 の「今の問題の一覧を読む」)。Studio の飛び先をこの形に合わせるのは FO-1s で、実物での飛び先は確かめていません(未確認)。`scene` はまだ出ません。
    → ✅FO-1s(`f186320f`)で、error の行は Materials のドックのその文書の行へ、warning の行はドックの model の bindings の行(`pelican.materials.model`)へ飛びます。シナリオ `gui_material_repair` が実物の engine で通しています(§13.1 の Materials のドックの FO-1s の段落)。
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
  [「PB-1s decision 1: one row a problem -- the severity, the code's sentence, the document, the line and column and where it is fixed -- sorted by severity or document, narrowed by severity and by words」](../../test/devstudio_problems_test.cpp#L190)・
  [「PB-1s decision 2: get_problems is read when get_status.problems.revision moves, and the contents check's warnings and errors are rows beside the engine's」](../../test/devstudio_problems_test.cpp#L244)・
  [「PB-1s decision 3 / RR-1s decisions 2 and 3: a click on a row goes where it is fixed -- the Graph's node for a start's refusal and a reload's alike, the document's text at the line or at the node, the shader file, the asset's row, the material, the rendering settings; Save restores the document」](../../test/devstudio_problems_test.cpp#L297)
  (✅RR-1s で名前と中身を書き換え。PB-1s の着地時は「PB-1s decision 3: a click on a row goes where it is fixed -- the Graph's node for a start's refusal, the document's text at the line or at the node, …」で、reload の node は文字の位置へ飛び、Save はファイルを書く期待でした)・
  [「PB-1s decision 4: the count says how many errors and warnings, and the placeholder's row says See Problems」](../../test/devstudio_problems_test.cpp#L394)・
  [「PB-1s decision 5: a row the engine no longer lists goes, and so does its selection」](../../test/devstudio_problems_test.cpp#L432)
  の 5 本。決定 6 は `devstudio_render_fallback_test` の書き換え(下の「組めない構成でも engine を止めない(✅BS-1s)」の再起動の案内)です。
  `devstudio_layoutpreset_test` はドックの数を 9 から 10 にし、古い保存済みの配置を戻しても Problems のドックが置き場を持つことを見ます。
  GPU の `devstudio_engine_failure_test` の WP298 の case は、止まる期待から fallback の期待(engine が止まらず、帯の文が `add a forward_transparent_v1 material pass`・`(material_route_unserved@1)`・`See Problems.` を含み、
  Problems の 1 行が source `startup`・`root_rendering`・Rendering settings)に書き換わりましたが、engine の morph の上限(上の engine に回したもの 1)で**赤のまま**です。
  → ✅MV-1(`f84a7d14`)でこの case は 2 本に分かれました。fatal の行を見る WP298 の case「Studio receives the running engine's fatal text while RPC stdout stays framed」は、example ではなく `test/fixtures/studio_automation_graph` の写しで `basic_config.camera` の `type` を `"fisheye"` にして起動し、
  project を読む段の `Pelican fatal error : basic_config.camera type must be 'perspective' or 'orthographic'`(rendering の文書より前なので `--render-fallback` でも致命)が Studio に届くことを見ます(対照は camera を戻した 600 frame の run)。
  上の fallback の期待は「The example with Alicia starts on its placeholder for a route no pass serves, and the band and the Problems row say why」に移り、example に Alicia を足した起動が止まらないことも見ます(test の詳細は[ソースコード読解ガイド 第7章](../source-code-guide/07_tools_rpc_tests.md) の `devstudio_engine_failure_test` の行)。
  実装者の run で `devstudio_problems_test` 5 / 5、`studio_cpu` 722 / 722 が緑です。
- シナリオ: `gui_problems_window`(Blank から GUI だけで。§13.5「Studio の自動化面」)。

## 15.17 Play

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
  [toolbar に置く所](../../src/devstudio/view/mainwindow.cpp#L2010))。
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
  `Viewport` の字の行は塗りません([塗る所](../../src/devstudio/view/mainwindow.cpp#L2216))。色は theme の token で、
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
  drag しても `set_time` を送りません([止める所](../../src/devstudio/model/graphcanvasmodel.cpp#L940))。edit と paused では
  scrub できます。

**再生中の scene の編集と Keep**(✅PM-3b)

再生中も Inspector・Outliner などの scene の編集はでき、engine はそれを記録します(`get_status.play.recorded_edits`)。
記録された編集は Stop で戻ります。残したい編集には Keep の印を付けます。

- **Inspector の行**: セッションの間に commit された編集は、Inspector の編集の行(`pelican.inspectorNotice`)の後ろに
  ` — reverts on Stop` が付き、右に **Keep** のリンク(`pelican.inspectorKeep`)が出ます。押すと Play changes でその編集に
  check が付き、行の後ろは ` — kept after Stop` になってリンクが消えます([行を組む所](../../src/devstudio/view/inspectorwidget.cpp#L1591))。
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
  ([`handleTakeFromGame()`](../../src/devstudio/model/inspectormodel.cpp#L3773))。commit すると行は
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
  [読み直す所](../../src/devstudio/view/mainwindow.cpp#L2264))。
- **未保存の扱い**: Stop は scene revision を戻さず、新しい revision で scene を戻します(§13.5「再生のセッション」)。そのままでは
  Stop のたびに未保存に見えるので、Studio は、Play の前(edit の間)に scene が保存済みで、Stop が 1 件も keep しなかったとき、
  Stop の後の最初の新しい revision を保存済みとして扱います([`InspectorModel::adoptRestoredRevisionAsSaved()`](../../src/devstudio/model/inspectormodel.cpp#L4585))。

**再生中に変わること**(✅PM-3b)

- **戻らない編集の帯**: セッションがある間、render-tools dock の上(Frame Plan・Features・Graph の 3 タブに共通の 1 本)と
  Pass Editor の上に、帯 `Changes here are written to files and stay after Stop.` が出ます。編集を送らなくても、セッションの間は
  出ています。帯の背景は theme の `play.paused_band` です([帯を作る所](../../src/devstudio/view/mainwindow.cpp#L2110))。
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
  ([`MainWindow::askToEndPlaySession()`](../../src/devstudio/view/mainwindow.cpp#L2170))。

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
  出ません。次の読みで値が変わっていれば、数も追います([読むだけの表示](../../src/devstudio/view/inspectorwidget.cpp#L627))。
- **編集はできません**: runtime の行を選んでいる間は Outliner の Duplicate / Delete が押せず、tooltip が
  `Objects spawned while the game runs cannot be edited.` になります([`OutlinerTree::setEditRefusal()`](../../src/devstudio/view/outlinertree.cpp#L83))。
  runtime の行は掴んで動かせません。その上に object を落とすと断られ、同じ文が status bar(8 秒)と Outliner の下の 1 行に
  出ます。Outliner に rename はもともとありません。
- Stop(と engine の停止・再起動)で行は消え、runtime の行を選んでいたら Inspector は `None` に戻ります。
- **viewport の選択とはつながっていません**。viewport で pick すると runtime の行の選択は外れます。`pick_object` は model
  instance を返し、`get_runtime_objects` の `entity` を返さないためです([pick の所](../../src/devstudio/view/mainwindow.cpp#L3302))。

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

## 関連文書

- [第13章 編集 RPC リファレンス](13_editor.md) — Studio が呼ぶ編集 RPC の params・戻り値・拒否コード
- [第10章 ツールリファレンス](10_tools.md) §10.7 — `pelican_studio` の起動と CLI
- [test/studio_scenarios/README.md](../../test/studio_scenarios/README.md) — 自動化面(`--automation`)の命令とシナリオの書き方
- [Studio シナリオの記録(2026-10)](../history/studio_scenarios_2026-10.md) — シナリオ 84 本の目録、画素を読む点と viewport の大きさの記録
- [Studio と編集 RPC の実装状況(2026-10)](../history/studio_status_2026-10.md) — 第13章 §13.12 にあった機能ごとの状態の表
- [../design_devstudio_direction.md](../design_devstudio_direction.md) — D0(エディタ特権の禁止)と Qt Studio の段階
- [../design_graph_layers.md](../design_graph_layers.md) — Graph が入口になる層の設計
