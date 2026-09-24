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

### Studio で新しい project を作る

**File > New Project...**(Open Project... の 1 つ上。✅F-2)は、フォルダ・名前・雛形を選ぶ dialog を開きます
([dialog](../../src/devstudio/view/newprojectdialog.cpp#L55))。作られるのは「フォルダ/名前」のディレクトリで、
dialog の Creates 欄にその path が出ます。雛形は **Standard 3D scene**(`pelican_cli project init` の既定と同じ)と
**Blank pipeline**(preset も features も持たず、fullscreen pass `present` 1 本が gradient を描くだけ)の 2 つで、
中身は `pelican_cli project init --template standard|blank` と同じ関数が書きます([第10章 §10.4](10_tools.md))。

- **既にファイルがあるフォルダは拒否します。**dialog は閉じず、`initializeProjectTemplate()` の文言
  (`project init target directory is not empty: <path>`)を dialog の下に出し、何も書きません。
  名前が空・`/` や `\` を含む・`.` / `..` の場合も dialog の中で断ります。
- **書いた project は Open Project と同じ経路で開きます**
  ([接続](../../src/devstudio/view/mainwindow.cpp#L736))。シーンに未保存の編集があれば上の
  Save / Discard / Cancel が出ます。ただし雛形は**確認より前にディスクへ書かれている**ので、Cancel しても
  作ったフォルダは残ります(開かないだけです)。
- **Browse...** は OS のフォルダ選択(modal の `QFileDialog`)です。自動化面からは操作できないので、
  シナリオはフォルダ欄(`pelican.newProject.location`)に直接 type します。

F-2 のときは、project を切り替えた直後の status bar に
`Modal transform polling failed: pelican_player exited before returning an RPC response` が出ることがありました
(gizmo の `get_modal_transform` polling が、切替で終了した古い engine に宛てた要求の失敗を出していたもの。
[文言](../../src/devstudio/model/gizmomodel.cpp#L1062))。**✅F-7 で出なくなりました**: 別の project へ切り替える前に
gizmo の session を終え、返答待ちの要求を忘れます([切替の前処理](../../src/devstudio/view/mainwindow.cpp#L1267))。
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
([使えない component の文の表](../../src/devstudio/model/inspectormodel.cpp#L511))、今ある code は `animation` / `audio` / `physics` /
`xr` / `vat` / `seqplayer` の `.module_unavailable@1` と `pelican.ray_instance_requires_layout_v2@1` の 7 つです。表に無い code は
code のまま出ます。

- **理由が code でない行は止めません。**`available: false` でも `unavailable_reason` が文(`simplemodelview` の
  `A model reference is required.` など。既定値だけでは足りず入力が要る、の意味)なら、その行は普通に並び、宣言に値を入れれば
  Add できます。code かどうかは「小文字・数字・`_` の `.` 区切りの名前 + `@` + 数字」の形で見分けます
  ([`isComponentUnavailableCode()`](../../src/devstudio/model/inspectormodel.cpp#L532))。
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
([Add Object の menu を作る所](../../src/devstudio/view/mainwindow.cpp#L983))。項目は **Empty** / **Cube (built-in)** /
**Plane (built-in)** / **Sphere (built-in)** で、自動化面の action 名は `pelican.outliner.addObject.empty` / `cube` /
`plane` / `sphere` です。

- **送るもの**: scene の `edit` 1 回に `spawn` op 1 つです([`addObjectDeclaration()`](../../src/devstudio/model/inspectormodel.cpp#L442))。
  transform は原点で、形なら `simplemodelview.model` に `engine://primitives/<kind>`、Empty は transform だけです。
  置き先の scene は選択中の object の scene(無ければ engine が最初に言った scene)で、選択中の object の子にはなりません。
- **名前**: 種類の名前(`Empty` / `Cube` / `Plane` / `Sphere`)が空いていればそれ、使われていれば `Cube_001`、`Cube_002`…
  の最小の空き番号です([`uniqueObjectName()`](../../src/devstudio/model/inspectormodel.cpp#L427))。Blender は `Cube.001` と
  書きますが、scene の object 名は `[a-zA-Z0-9_]` だけなので `_` にしています。
- **選択**: Outliner の読み直しを最短の間隔に縮め、新しい object が Outliner に出た時点でそれを選び、Inspector に出します
  ([`MainWindow::selectSpawnedObject()`](../../src/devstudio/view/mainwindow.cpp#L1078))。
- **Undo**: coalesce key を付けないので、Undo 1 回で Add 1 回が戻ります。
- **Inspector の編集の途中で押した Add Object は待ちます(✅Q-7)**: Inspector の値の編集が進行中(commit の途中を含む)に
  押すと、spawn は 1 本だけ待ち、Inspector の手が空いた時点で、その編集が残した scene revision の上に送られます
  ([`InspectorModel::spawnObject()`](../../src/devstudio/model/inspectormodel.cpp#L2348))。待っている間は status bar(8 秒)と
  Outliner の下の 1 行(`pelican.outlinerNotice`)に `Adding <名前> after the current edit.` が出ます
  ([Add Object の結果を status bar と Outliner に出す所](../../src/devstudio/view/mainwindow.cpp#L1057))。既に 1 本待っているときに
  押した 2 本目は、置き換えずに断ります。同じ 2 か所に
  `Could not add <名前>: <待っている名前> is already waiting to be added; add another once it is in.` が出るので、
  1 本目が入ってからもう一度押してください。Outliner の下の 1 行は、新しい object が選ばれたときに消えます。
  ⚠ 待っている spawn は engine が止まると捨てられ、そのことは言われません(1 行が消えるだけです)。
- Add Object は常に押せます。project が開いていない・engine の準備ができていないときは、status bar に
  `Add Object needs an open project with its engine running.` と出るだけです。
- ⚠ spawn は Studio が最後に見た scene revision で送ります。他の client が編集した直後は `stale_revision` で断られるので、
  もう一度押してください。また、engine が断った spawn の名前も Studio は使用中と数えたままなので、次の Add はその次の
  番号になります。Empty / Plane / Sphere は単体 test だけで、シナリオが通すのは Cube です。

**Inspector の model 欄は選ぶ欄です(✅G-2)。**`simplemodelview.model` は打つ欄ではなく combo
(`pelican.inspectorModelChoice`)です([model 欄の選択肢の並び](../../src/devstudio/model/inspectormodel.cpp#L473))。
並びは基本形 3 つ(`Cube (built-in)` などの名前で)→ 区切り → `list_assets` が言う project の model(engine の基本形は
二重に出しません)→ 今の値がそのどれでもなければその値、です。選ぶとすぐ commit します(enum の combo と同じ)。
Add component... の asset 一覧も同じ読み方で、基本形は `Cube (built-in)` の名前で先頭に並び、区切りの下に project の
asset が `id [status]` で並びます。

**Inspector の数値欄の名前(✅G-5)**: 自動化面から届くように、数値の欄は 1 つずつ名前を持ちます。vector の列は
`pelican.inspectorVector.<component>.<field>.<列>`(例: `pelican.inspectorVector.transform.scale.2`)、数 1 つは
`pelican.inspectorNumeric.<component>.<field>` です([数値欄の名前の付け方](../../src/devstudio/view/inspectorwidget.cpp#L1715))。
G-5 より前は、どの vector も最初の列が同じ名前でした。シナリオは欄が有効になるのを待って打ち、Return で commit し、
欄がその値を持つまで待ちます([シナリオの README](../../test/studio_scenarios/README.md#L334))。

**Inspector の欄の編集と commit(✅Q-7)**

- **focus だけでは編集になりません。**編集は、値を打つ(値が変わる)か欄の上で mouse の button を押したときに始まります
  ([focus で編集を始めなくなった所](../../src/devstudio/view/inspectorwidget.cpp#L1909))。欄に focus を置いたままでも
  **Refresh** は押せ、他の欄も止まりません。vector の中で列を移るあいだは 1 つの編集のままです。
- **busy の間に打った値は、手が空いてから送られます。**別の編集の commit の途中(Inspector が busy)は他の欄が止まりますが、
  focus のある欄は止まりません([busy でも focus のある欄は止めない所](../../src/devstudio/view/inspectorwidget.cpp#L1201))。
  そこへ打った値は欄に残り、欄を離れるか Return を押すと、Inspector の手が空いてから 1 回の編集として送られます
  ([手が空いてから値を送る所](../../src/devstudio/view/inspectorwidget.cpp#L540))。その間に欄が消えた(別の object を選んだなど)か、
  値が既に同じなら何も送りません。
- **commit の往復で欄は作り直さず、値だけ差し替えます**([値だけを差し替える所](../../src/devstudio/view/inspectorwidget.cpp#L622))。
  編集中の欄・送り待ちの値を持つ欄・打ちかけの欄(`1.` のように、表示がまだ値と一致しない欄)は上書きしません。作り直すのは
  描くものが変わったとき(別の object、component の追加・削除、欄の種類や範囲が変わるなど)だけです
  ([作り直すかどうかを決める所](../../src/devstudio/view/inspectorwidget.cpp#L568))。
  ⚠ list を持つ component(`pelican_ray_instance` など)は、list の key に scene revision が入っているので、今も commit のたびに
  作り直されます(打ちかけの値はそこでは消えます)。
- G-6 までは、commit のたびに欄を作り直し、focus だけで編集が始まっていました。commit の間に止まった欄から Qt が focus を
  次の欄へ送り、その focus が始めた編集を誰も終わらせないので、Inspector が busy のまま残り(Refresh も他の欄も止まり、
  窓を閉じると未保存の問いが出る)、窓が active のときだけ起きる間欠の故障になっていました。

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

- 文書 1 つ = **シーンファイル 1 本**です。`pelican.scene` v1 のエンベロープ丸ごと —— そのファイルの `scenes` が宣言する全シーン、その中の全オブジェクト、その全コンポーネントの生 JSON —— を保持します([authoringscenedocument.hpp](../../src/core/loader/authoringscenedocument.hpp#L114))。プロジェクトに複数のシーンファイルがあれば文書も複数で、`export_scene_snapshot` はその全部を返します。
- **`AuthoringObjectId`**(セッション内で安定・1 始まり)と **`SceneRevision`**(単調増加)が編集の座標系です。ランタイムの `EntityId` は付随情報で、**同一性の代用にはしません**(オブジェクトが再生成されると変わるため)。
- コンポーネントの受理・正規化は [第4章](04_scene_ecs.md) §4.2 の component codec が担当します。値を書くと**必ず codec の正規形に書き直されて**保存されます。ここでいう**正規形**は、書いた JSON をいったん codec で読み(`decodeAuthored()`)、その値からもう一度書き出した(`encodeCanonical()`)形のことです —— 鍵の並び・省略した既定値・数値の型が一意に決まるので、同じ意味の JSON は必ず同じバイト列になります([componentcodec.hpp](../../src/core/loader/componentcodec.hpp#L339))。

### 保存の往復について(weak round-trip)

保存時に書き出される JSON は、**キーの順序が辞書順に正規化**されます(配列の順序とスカラの型はそのまま)。手で書いた `project.json` 風の並び順は保たれません:

```json
// 入力
{"schema":"pelican.scene","version":1,"scenes":{"main":{"objects":[]}}}
// 保存・スナップショット出力
{"scenes":{"main":{"objects":[]}},"schema":"pelican.scene","version":1}
```

(実物: `test/fixtures/editor_command_service/export_scene_snapshot_v1.json`)

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

- **ウィンドウモードでは `step_frame` を呼ばないでください**。フレームは自然に進んでいるので、`step_frame` は余分に 1 フレーム進めてしまいます。代わりに `get_edit_result` / `get_scene_revision` をポーリングします。
- ヘッドレスでは `step_frame` を呼んで進め、応答の `edit_results[]` で結果を受け取るのが基本形です。

### 編集操作(operations)の 12 種

`edit` の `operations[]` には次の 12 種を並べます(正は
[`prepareRpcOperation()`](../../src/core/communication/editorjournal.cpp#L1598) の op 分岐)。1 回の `edit` は**全部成功か全部失敗**で、
表に無い `op` は `method_unavailable{method, adapter}` です。

| op | 主なフィールド | 意味 |
|---|---|---|
| `set_component_value` | `object_id`, `component_slot`, `field_path`(JSON pointer), `value` | 値の変更。`/name` への書き込みは拒否 |
| `add_component` | `object_id`, `component`(コンポーネント JSON) | コンポーネント追加 |
| `remove_component` | `object_id`, `component_slot`(behavior は `attachment_index` も必須) | コンポーネント削除 |
| `insert_component_field` | `address`, `value`, `position?`(behavior は `attachment_handle` / `attachment_seq`) | 可変長宣言(`List` / `$list`)に行を追加([insert の受理キー](../../src/core/communication/editorjournalops.cpp#L406)) |
| `remove_component_field` | `address`(同上) | 行、または行の中の 1 フィールドを削除([remove の受理キー](../../src/core/communication/editorjournalops.cpp#L600)) |
| `move_component_field` | `address`(`row_id`), `position`(`{"before":行ID}` か `{"end":true}`) | 行の並べ替え([move の受理キー](../../src/core/communication/editorjournalops.cpp#L567)。§13.12) |
| `spawn` | `object`(+ `scene_id`) | オブジェクト生成。ID は engine が採番 |
| `destroy` | `object_id` | 子孫ごと削除(逆操作用に全 JSON を保存) |
| `reparent` | `object_id`, `new_parent_id`(null = ルート), `preserve`(`"local"` / `"world"`) | 親子の付け替え |
| `set_prefab_parameter` | `object`(**`object_id` ではない**), `parameter`, `value` | prefab インスタンスのパラメータ上書き |
| `unset_prefab_parameter` | `object`, `parameter` | 上書きを外して prefab 側の既定へ戻す(既定が無ければ拒否) |
| `unpack_prefab` | `object`(このほかのキーは拒否) | prefab インスタンスを素のオブジェクトへ展開 |

`get_procedural_assets` / `edit_procedural_asset` は **`edit` の op ではなく独立した RPC メソッド**です
([登録](../../src/core/communication/editorrpchandlers.cpp#L246))。`edit_procedural_asset` は
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
| `get_graph_instances` | `{}` | 共通グラフ文書の `source_reference` / `source_digest` / `instances[]`(`graph.instance` だけ)/ root の `nodes[]` 全体 / `definitions` 本体([get_graph_instances の実装](../../src/core/communication/renderconfigeditor.cpp#L2210)) |
| `get_graph_catalog` | `{}` | 登録済みグラフモジュールのカタログ(`pelican.graph_catalog` v1。`pelican_cli graph-catalog` と同じ値 — [第10章](10_tools.md) §10.4) |
| `get_graph_values` | `{instance}` | その instance の入力 port の実効値 `{port, value, source, connected\|parameter}` と `published` / `generation` / `frame`。**publish 時の snapshot を読むだけで再評価しません** |
| `get_graph_node_values` | `{instance?}` | 展開木の全ノードの出力値(`{node, expanded, outputs}`)と `source:"rendering"`。`instance` 省略で root から |

`declaration_index` は scene の `objects` 配列における 0 始まりの宣言位置です。直リンクで読んだ木との対応には `(scene_id, declaration_index)` を使います。`authoring_object_id` はセッション内で安定した別の参照なので、挿入・並べ替え後は両者が一致するとは限らず、どちらも応答に残ります。

`get_components` が返すコンポーネントには、**`schema`(フィールド名・型・範囲・単位)と `editable` フラグ**が含まれます。インスペクタのウィジェットはこれだけで組み立てられます(§13.9)。

ここでいう**解決**は、オーサリング文書の生 JSON を codec に通し、既定値を補ってランタイムの値にするところまでを指します。範囲外の数値・未知の鍵・型違いなど、利用者が書いた内容が契約に反していれば解決は失敗します。

**解決に失敗したコンポーネントも消えません(✅WP378)。** authored / generated のどちらの直下にも
`resolution_error` が付き、`get_components` は**成功応答のまま**それを返します
([editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp#L1808))。形は 2 通りで、
typed な違反は `{code, path(RFC6901), fields, message}`、まだ typed 化していない旧分岐は
`{legacy_branch_id, message}` です(正は
[`resolutionFailureJson()`](../../src/core/loader/resolutionresult.hpp#L114))。
**`resolution_error` を含む応答はキー順が保存されます** —— editor 側のハンドラが「応答木のどこかに
`resolution_error` があるか」を見る述語 `embedsResolutionError` を登録時に添え
([editorrpchandlers.cpp](../../src/core/communication/editorrpchandlers.cpp#L50))、`RpcServer` はその述語が真を返した応答だけを
ordered JSON で直列化するためです([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1036))。

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
| `export_scene_snapshot` | `{schema_version:1, allow_pending?}` |
| `import_scene_snapshot` | `{schema_version:1, semantic_scene_bytes, digest, current_scene_id}` |
| `save_scene` | `{}`(フィールドを書くとエラー) |

### shader を読み直させる(✅F-5)

| メソッド | params | 返すもの | 拒否(`error.data` の `{error_code, error_fields}`) |
|---|---|---|---|
| `reload_shaders` | `{}`(鍵を書くと invalid params) | 読み直した: `{committed: true, changed: [shader の参照…], cache_hits, cache_misses}` / 何も変わっていない: `{committed: false, attempted: false}` | `shader_reload_failed@1`(compile 失敗。`error_fields.changed[]`、message は compile log で、`get_status` の `reload.runtime["pelican.shaders"].last_error` にも同じ文が残る)/ `shader_reload_unavailable@1`(`error_fields.reason` が `replay` か `strict assets`。reload service の無い engine も同じ code) |

**file watch の gate は開けません。**`--rpc` で動く engine(Studio が起こす engine はすべてそう)は、reload gate が
`rpc driver` を理由に閉じていて、shader file を保存しても自分では読み直しません。`reload_shaders` はそれを開けずに、
**頼まれたその 1 回だけ** shader の runtime participant を当てます(`reload_game_logic` と同じ形。
[handler](../../src/core/communication/rpcserver.cpp#L1214))。閉じた gate を越えるのは明示の要求だけで、
replay と `--strict-assets` の engine は今までどおり断ります([越えてよいかの判定](../../src/core/watch/reloadservice.cpp#L590) /
[`ReloadGate::explicitShaderReloadRefusal()`](../../src/core/watch/reloadgate.cpp#L45))。file watch の poll の経路は `--rpc` の下で
今までどおり何もしません。

- **いつ効くか**: RPC は frame 境界で処理されるので、handler の中で即時に準備・commit し、新しい shader は次の frame の
  `render_start` で使われます(fullscreen pass の入力もそこで bind し直します)。preview 中なら先に preview を中止します。
- **何を読み直すか**: SPIR-V の sha256 が公開中と違う unit だけです([`ShaderLibrary::prepareReloadChanged()`](../../src/core/shader/shaderlibrary.cpp#L1120))。
  ただし前回の読み直しが失敗した unit は、中身が変わっていなくても 1 回公開し直します(それで失敗の記録が消えます)。
  判定のために compile はするので、呼ぶたびに全 unit を compile し直します(cache が効くので軽い。大きな project での時間は未測定)。
- Studio は shader file の保存を見て自分でこれを呼びます(§13.5「Studio のノードキャンバス」の「shader を外のエディタで保存すると画面が変わる」)。

### レンダー設定を編集する

**シーン編集とは実行モデルが違います。**混ぜると壊れます。違う理由は対象が違うからです —— シーン編集の対象がメモリ上の `AuthoringSceneDocument`(正は `SceneRevision`)なのに対し、こちらは**ディスク上の rendering config ファイルそのもの**で、正はその sha256 digest です。したがって版の比較も、undo の有無も、書き込みの作法も別建てになります。

| メソッド | 何をするか |
|---|---|
| `get_render_features` | 著作 config の現在の `features[]`、`source_reference`、`source_digest`、runtime の状態を返す |
| `list_render_features` | **engine が可用性を判定した**カタログ。各項目に `available` が付き、**`unavailable_reason` は `available` が false の項目にだけ現れる**(使える項目には鍵ごと無い)。`source` はその文書の出所で、engine 内蔵なら `"engine"`、`project://packages/<ns>/` に vendoring された package のものなら `"package"` の 2 値だけです([catalog entry](../../src/core/communication/renderconfigeditor.hpp#L35))。読めない package 文書は**カタログから消えるのではなく** `available:false` + `unavailable_reason` として並びます([package の列挙](../../src/core/communication/renderconfigeditor.cpp#L705)) |
| `edit_render_features` | `base_source_digest` と `operations[]` を受け、**チケットを返す**。結果は `get_edit_result` か `step_frame` の `edit_results[]` |
| `edit_render_features` / `materialize` | operationに`op: "materialize"`・`scope: "root"`または`"preset"`・`index`・`destination_reference`・`base_feature_digest`を指定。presetの場合は`base_preset_digest`も必要。具体値へ展開したfeatureを未作成のprojectファイルに保存し、元instanceをその参照へ置換する。featureのdigestは`get_render_features.feature_instances[].feature_document_digest`、presetのdigestは同entryの`source_digest`を使う。engine presetは先にforkする。Studio からの入口は Render Features の **Make editable...** |
| `get_scene_acceleration_structures` | 名前付きTLASの`definitions[]`、taskの接続先`task_connections[]`、編集元の`source_digest`、三角形座標系とレイアウトの編集可否・digestを含む`triangle_geometry`を返す |
| `edit_scene_acceleration_structures` | `base_source_digest`と`definitions[]`、任意の`task_connections` objectでTLASと接続先を置換。`triangle_space`と`base_layout_digest`を組で渡すとレイアウトの三角形座標系も同時保存し、既存の編集ticketを返す |
| `get_ray_callable_records` | taskごとのCallable schema、shader一覧、元宣言と実効初期値、rootの`source_digest`を返す |
| `edit_ray_callable_records` | `base_source_digest`と`task_records` objectで初期値の上書き指定全体を置換。既存の編集ticketを返す |
| `fork_render_pipeline` | `base_source_digest`・`base_preset_digest`・`destination_reference`で選択中presetを新しいprojectファイルへコピーし、rootの参照先も切り替える |
| `edit_region_replacement` | `base_source_digest`・`base_preset_digest`・`graph`・`region`でproject presetのタグ付き領域を指定。`provider`に登録済みprovider名を指定するとコンパイル時の差し替えを選択し、`null`なら差し替え要求を除去して元のノード構成を使用する。元のpass・設定・接続は保持する。現在の指定は`get_render_authoring_context`の各graphの`region_replacements`で取得できる。標準feature全体の展開操作ではない |
| `edit_preset_pass` | `base_source_digest`・`base_preset_digest`・`graph`・`pass_name`でproject presetのpassを指定。fullscreenは`input`・`output`・`shader`と任意の`ray_query`・`uses_light_data`・`push_constants`・`clear_colors`(後の 3 つは ✅G-3。省略は保持、`null` で削除。[G-3 の 3 field の受け取り](../../src/core/communication/renderconfigeditor.cpp#L2730))、materialは`input`・`output`・`ray_query`の1項目以上。rasterはさらに`shader`・`draw`・`raster_state`を部分編集できる。省略項目は保持、rasterの`raster_state: null`は既定状態へ戻す。書き先は 3 つ: project preset(`base_preset_digest`)/ project feature(`base_feature_digest`)/ **direct root 自身**(✅F-6。root が preset を選ばず、要求がどちらの digest も持たないとき。`base_source_digest` が root の digest を兼ね、root の `rendering.rendering_passes` の pass を編集する)。置換規則は 3 つとも同じ関数です([direct root の書き先](../../src/core/communication/renderconfigeditor.cpp#L2822)) |
| `get_compute_task_authoring` | compute/RT taskの元宣言・編集可否・保存先とdigestを取得する |
| `edit_compute_task` | `base_source_digest`・`base_task_source_digest`・`task_name`・`task`でtask宣言を置換する。名前は保持する |
| `add_compute_task` | `base_source_digest`・`destination_reference`・`task`で新規project featureにtaskを作り、rootのfeaturesへ追加する |
| `remove_compute_task` | `base_source_digest`・`base_task_source_digest`・`task_name`で元文書からtaskを削除する |
| `edit_render_targets` | `base_source_digest`・`ops[]`(✅F-1)。op は `{op:"add", target}` / `{op:"replace", name, target}`(`target.name` を変えれば改名)/ `{op:"remove", name}`。書くのは managed fragment `project://passes/authoring/render-targets.json` 1 つだけで、最初の要求で作って root の `features[]` に足す(空になっても消さない)。ops は順に当て、最後に 1 回 preflight して ticket を返す —— **全部入るか、何も入らないか**。名前付きの拒否は下の 6 種([edit_render_targets の実装](../../src/core/communication/renderconfigeditor.cpp#L3223)) |
| `add_authored_pass` | `base_source_digest`・`graph`・`insert`(authoring context の挿入位置の token)・`pass`。pass 1 つを managed fragment `project://passes/authoring/pass-<id>.json` に書き、root の `features[]` に足して preflight し、ticket を返す。受ける `type` は `fullscreen` と **`material`**(✅G-1)だけで、それ以外は `authored_pass_type_unavailable`(detail は `requested_type` と **`supported_types`**(配列)。[add_authored_pass の種類の門](../../src/core/communication/renderconfigeditor.cpp#L2934))。material pass の形の検査は下の段落。任意の **`material_route_default`**(`"auto"` / `"forward"`。✅G-4)を渡すと、pass と rendering 文書の root の既定を 1 つの ticket で変える(下の「material の route の既定」) |
| `set_material_route_default` | `base_source_digest`・`material_route_default`(`"auto"` / `"forward"`)。root の既定だけを変える。root 編集の ticket に乗り、commit の結果に `restart_required: true`(✅G-4。[`RenderConfigEditorService::setMaterialRouteDefault()`](../../src/core/communication/renderconfigeditor.cpp#L2454)) |
| `edit_graph_document` | `base_source_digest`と`ops[]`で共通グラフ文書を編集する(§13.5「グラフの装着を編集する」)。ops は順に候補文書へ当て、最後に 1 回だけ展開する — **全部通るか、文書ごと拒否か**のどちらか |
| `edit_graph_instances` | `base_source_digest`・`attach[]`・`detach[]`。owner を持つ root instance の装着 / 取り外しだけを扱い、中身は `edit_graph_document` の ops へ翻訳される([edit_graph_instances の実装](../../src/core/communication/renderconfigeditor.cpp#L2241)) |
| `export_render_feature` | `scope`(`root` / `preset`)と`index`で feature 1 つを `user://components/<name>/` へ書き出す。`engine://` の feature は `render_component_engine_reference` で拒否([export の実装](../../src/core/communication/renderconfigeditor.cpp#L1171)) |
| `import_render_feature` | `base_source_digest`・`source_dir`(project 外の絶対パス)・`destination_reference`(未作成の`project://`)。component が要求する render target を project が宣言していなければ `render_component_requires_missing` |
| `install_package` | `source_dir`(project 外の絶対パス)。`<project>/packages/<namespace>/` へ複製して`project.json`に記録し、catalog を作り直す。返すのは`namespace`・`package_version`・`sha256`・`feature_references`・`feature_names` |
| `uninstall_package` | `namespace`。root config と選択中 preset の全文字列を走査し、まだ参照が残っていれば`package_in_use`(参照を列挙)で**何も消しません** |

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
([使用者](../../src/core/communication/renderconfigeditor.cpp#L598))。

読む側は `get_render_authoring_context` です。`render_targets[]` は合成後の全 target を
`{name, declaration, editable, source}` で返し、`editable` なもの(= fragment のもの)には fragment に書いてある宣言
そのもの `authored_declaration` が付きます。並びは editable なものが先です([組み立て](../../src/core/communication/renderconfigeditor.cpp#L2076))。
`source` は `authoring`(fragment)/ `root` / `preset` / `feature:<reference>` / `engine`(`display` など)で、
**文書にも合成器の出所記録にも出所が無いときだけ `composed`** になります(graph の合成で出来た target など。
[出所の判定](../../src/core/communication/renderconfigeditor.cpp#L537))。ほかに `render_target_fragment`(fragment の参照)と
`render_target_vocabulary`(`formats` / `usage` は parser 自身の表、`format_classes` は `scene` / `display` / `data` /
`explicit`)が付きます。fragment は pass を持たないので `managed_fragments` には出ません。

**編集は、起動時に登録した graph の変種の全部で合成してから受けます(✅G-6)。**engine は起動時に flat と一緒に preview の
graph も組み、XR を始めたときは xr も組みます([第6章 §6.13](06_rendering.md))。render の編集の候補は、ticket を取る前に
このすべてで合成されます。flat と preview は常に、xr は起動時に XR を始めた engine だけです(xr は history を持つ feature を
拒否するので、XR を始めていない engine で xr を合成すると、TAA の project の編集がすべて断られてしまいます)。対象は root の
文書を変える編集(`edit_render_features`・`edit_preset_pass`・`edit_graph_document` など)と、authored pass の系統
(`add_authored_pass`・`remove_authored_pass`・`edit_render_targets`)です([変種を合成する preflight](../../src/core/communication/renderconfigeditor.cpp#L899))。
flat で合成できない候補は今までどおり flat の経路で断られます。flat が通って preview か xr が合成できない候補は
`render_pipeline_preflight_failed` に **`variant`**(`"preview"` / `"xr"`)が付いて断られ、文書は変わりません。`set_layout`
だけの `edit_graph_document`(再 compose しない編集)と、context の読み取りは合成しません。Studio はこの拒否を、Pass Editor と
Frame Plan の status 行に `The preview graph refused it: <code>: <message>` の 1 行で出します(以前の Pass Editor は
`authoring failed` とだけ出し、理由は tooltip にしかありませんでした。[拒否の 1 行を作る所](../../src/devstudio/view/frameplanedit.cpp#L775))。
例: `history: true` の target `trail` を足す編集は通りますが(preview はその fragment ごと外します)、direct root の `present` に
`trail@history` を読ませる `edit_preset_pass` は preview で断られます。

**material pass を足す(✅G-1)。**`add_authored_pass` は fullscreen に加えて `type: "material"` を受けます(raster は
scene を描かないので受けません)。合成だけでは material pass の color の枚数・depth・format を見ないので、engine は
合成した候補の中の新しい pass を、load と同じ parser(`parsePassDefinitionFromJson`)に候補の render target で通します
([`validateAuthoredMaterialPass()`](../../src/core/communication/renderconfigeditor.cpp#L368))。contract と枚数が合わない、
depth が無い、swapchain に書く、format が違う、といった誤りは parser の文言のまま `render_pipeline_preflight_failed` で
断られ、文書は変わりません。この検査の target の大きさは仮の 1920×1080 です(parser が見るのは 1 pass の出力どうしの比
だけです)。`format_class: scene` の target の format は、running pipeline の有効な feature に `hdr` があるかで決まります。
その一覧は `get_render_authoring_context` の **`enabled_feature_names`** にも出ます
([context の enabled_feature_names](../../src/core/communication/renderconfigeditor.cpp#L2025))。Studio はこれで、新しく作る
G-buffer の target を HDR / SDR のどちらの format にするかを決めます(context が言わなければ HDR と読みます)。

**material の route の既定と、受け皿の無い material の拒否(✅G-4)。**rendering 文書の root の `material_route_default`
(`"auto"` / `"forward"`。意味と書ける場所は [第6章 §6.7](06_rendering.md))は編集 RPC からも変えられます。

- `add_authored_pass` に `material_route_default` を渡すと、pass の fragment と root の key の変更が**同じ候補文書集合**に入り、
  1 回の preflight・1 つの ticket になります([pass と route の既定を 1 つの候補に入れる所](../../src/core/communication/renderconfigeditor.cpp#L2941))。
  下の受け皿の検査は、新しい既定で計算し直した route で行います。今の値と同じなら key は書かず、再起動も求めません。
- `set_material_route_default` は既定だけを変えます(上の表)。
- engine はこの key を**起動時に 1 回しか読まない**ので、値を変えた ticket の commit の結果(`get_edit_result` /
  `step_frame` の `edit_results[]`)に **`restart_required: true`** が付きます
  ([commit の結果に付ける restart_required](../../src/core/communication/renderconfigeditor.cpp#L3653))。engine を起動し直すまで、
  route が変わる material は今の pipeline のまま描かれます。⚠ commit から再起動までの間に登録された material は、
  古い既定で lowering されます。
- runtime shader compiler の無い build で `"forward"` を頼むと、ticket を作る前に **`material_route_default_unavailable@1`**
  (`value: "forward"`・`reason: "runtime_shader_compiler_disabled"`)で断られます。engine の組込 material の forward 版を
  compile できないためです。

**`material_route_unserved@1`**: pipeline に material pass が 1 つでもあるのに、登録済みの material(engine の組込を含む)の
route を受ける pass が無い候補は、この code で断られ、文書は変わりません。**前の世代がその route を使っていたかどうかは
問いません** —— G-4 の前は、最初から forward の pass だけにする編集が commit を通り、次の起動で落ちていました。足りない
material を route ごとに全部集めて 1 回で断ります
([`MaterialGenerationPlanner::requireEveryMaterialServed()`](../../src/core/material/materialgenerationplanner.cpp#L779))。
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
([文字列から拒否に戻す所](../../src/core/communication/renderconfigeditor.cpp#L3562))。

Render Featuresの **Parameters...**で、選択中 feature instance の scalar パラメータを編集できます
(`edit_render_features` の `set_parameters`)。欄は engine の `parameters` 宣言から作られ、
`get_render_features.feature_instances[].parameter_schema` がその宣言です。**materialize 済みの
feature でも `parameter_schema` は返ります** —— ただし宣言の `default` は、その派生が焼き込まれた
`bound_parameters` の値で上書きされます。したがって**元の engine 既定値は RPC からは見えません**
([上書き箇所](../../src/core/communication/renderconfigeditor.cpp#L957))。
`parameter_schema` が `null` の feature は「編集できるパラメータを宣言していない」ので、
`reason` を表示するだけで dialog は開きません。

Render Featuresの **Make editable...**は materialize(§13.5 の表)です。押せるのは
`feature_instances[].materializable` が true の項目だけで、false のときは `materialize_reason` が出ます ——
engine preset の feature なら「先に project へ fork する」、すでに `bound_parameters` を持つ派生なら
「これはもう materialize 済み」の 2 通りです([materializable の判定](../../src/core/communication/renderconfigeditor.cpp#L948))。
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
([`editable_root_pass` を付ける所](../../src/core/communication/renderconfigeditor.cpp#L1905))。engine が足した pass(`output_transform` など)と
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
([組み立て](../../src/devstudio/view/frameplangraphics.cpp#L1078))。ソケットの形は resource の種類で、
`frame_plan.image`(丸)/ `frame_plan.buffer`(四角)/ `frame_plan.acceleration_structure`(菱形)/
`frame_plan.swapchain`(六角)/ `frame_plan.history`(縁の太い丸)を
[既定表の frame_plan.* 5 entry](../../src/devstudio/resources/graph_socket_styles.json#L28) が持ちます ——
**Graph タブと同じ表・同じ解決順**です([種類の判定](../../src/devstudio/view/frameplangraphics.cpp#L1912))。
辺は**ソケット中心からソケット中心**へ引かれ、対応するソケットを持たない辺(折り畳んだグループ、境界の stub、
resource を名指さない順序依存)だけがノード矩形へ落ちます([配線](../../src/devstudio/view/frameplangraphics.cpp#L253))。
辺のラベルは消えて tooltip になり、ノードを選ぶと触れていない辺が薄くなります
([不透明度](../../src/devstudio/view/frameplangraphics.cpp#L4215))。
**操作・RPC・model は変わっていません** —— ソケットはノード item の子なので、**Shift＋左ドラッグ**はソケットから
始めてもノードの本体から始めても同じ接続になります([起点の解決](../../src/devstudio/view/frameplangraphics.cpp#L3712))。
行の並びは resource の**名前順**で宣言順ではなく、長い名前は中略されます(幅の上限
[`FramePlanNodeMaximumWidth = 360`](../../src/devstudio/view/frameplangraphics.hpp#L133)。全名は同じ行の
ソケットの tooltip に `<resource> : <型>` として出ます)。

Frame Planの **Order nodes...** で現在のgraphのノードをUp/Downで並べ、Applyで保存できます。
データ依存とAS構築の必要順序は維持され、矛盾する並べ替えは拒否されます。
**Use dependency order**で当該graphの指定を解除します。名前付きAS同士の順序も変更できます。
画面が古い場合はFrame Planを再取得して開き直してください。順序指定に含まれるtaskを削除・改名する前には指定を解除します。
保存先はrootの`frame_node_order`で、RPCは`get_frame_node_order` / `edit_frame_node_order`です。

**シーン編集との違い:**

- **`actor_id` もセッションも要りません。**
- **undo / redo できません。journal にも載りません。**
- **CAS は SceneRevision ではなく、ルート設定ファイルの sha256 digest** です。
  **メモリ上の文書とディスク上の実物の両方**を照合します。**受付の時点**でどちらかが食い違えば `external_modification`。**受付のあと**、フレーム境界までの間に**別のチケットがメモリ上の文書を先に更新していれば `stale_source_digest`**、ディスク側が変わっていれば `external_modification` です。いずれも**何も書きません**。
- 書き込みは原子的置換で、**engine が文書を所有します。編集側はファイルに触れません。**
- **v1 は `operations` が最大 1 要素**。`op` は `add` / `remove` / `set_parameters` / `materialize` の 4 つで、
  それ以外は `edit_render_features operation op must be 'add', 'remove' or 'set_parameters'` です
  ([その throw](../../src/core/communication/renderconfigeditor.cpp#L1555))。`set_parameters` は既定が
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
手書きや legacy の config でも編集面が作られます。

**`pelican.graph` root では挿入先が 1 段下です。**root が `schema: "pelican.graph"` のとき、
著作キー(`features` / `rendering_passes` / `pipeline` …)の在処を決めるのは
[`renderingSectionOf()`](../../src/project/renderconfigsection.hpp#L37) で、
**`rendering` オブジェクトの中**になります(読み書きの locator はこの 1 本ですが、byte-lossless の
書き込み側は同じ規則を [`isGraphRootKey()`](../../src/project/renderconfigdocument.cpp#L418) として
鍵の側から持っています。N-6 からは editor 用の `layout` も graph 側の root キーです。[第6章](06_rendering.md) §6.16)。したがって初期化は `rendering.features` に
`[]` を書き、**root 直下に `features` を生やしません**
([`initialize()`](../../src/project/renderconfigdocument.cpp#L590))。
`rendering` オブジェクトそのものが無い graph root は、書き込む場所が無いので
**名前付きで断られます** —— 受付の応答は `render_config_section_unlocated`
(**編集 op の経路だけ `{method}` が付きます** —— [renderconfigeditor.cpp:1325](../../src/core/communication/renderconfigeditor.cpp#L1620)。
pipeline の preflight([:887](../../src/core/communication/renderconfigeditor.cpp#L1165))と fork([:1753](../../src/core/communication/renderconfigeditor.cpp#L2138))は
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
  ([翻訳](../../src/project/graphcomposition.cpp#L908))。検証を 2 度書かないための構造です。
- 読み: `get_graph_instances` / `get_graph_catalog` / `get_graph_values` / `get_graph_node_values`(§13.5 の読み取り表)。

#### `ops[]` と `scope`

| op | フィールド | 意味 |
|---|---|---|
| `add_node` | `node`(`id` を持つ node object)、任意の `index` | ノードを追加。同じ scope に同じ id があれば拒否。`index` は挿入位置(0〜現在の個数)、省略時は末尾 |
| `replace_node` | `node`(`id` を持つ node object) | 同じ id の既存ノードを丸ごと置換し、文書全体を検証。Undo で省略フィールドも含めて復元するために使う |
| `remove_node` | `node`(id 文字列) | ノードを削除。誰かが参照していれば拒否 |
| `set_input` | `node`, `port`, `source`(`{node, port}` ちょうど 2 鍵、または `null`) | 入力の接続 / 切断。`null` は `inputs` からその port を消し、空になれば `inputs` ごと消す |
| `set_node_parameters` | `node`, `parameters`(object) | **merge ではなく置換**です。1 つだけ変えたいときも全体を読んで送り直します。空 object を送ると `parameters` が消えます |
| `set_node_label` | `node`, `label`(string) | 人が付けた表示名。識別子ではないので、**空文字が「消す」**です(専用の op は作られていません) |
| `set_layout` | `layout`(object か `null`) | その scope の editor 用の置き場所(`nodes{id: {x, y}}` / `reroutes[]{edge{node, port}, points[]}` / `ties[]{x, y, edges[]}`、N-8 からは `frame_plan{<graph>: {nodes, reroutes, ties, groups}}` も。こちらは形だけ検査)を丸ごと置換。**再 compose しない**(bytes だけ変わる)。名指した node / 辺が無ければ `graph.layout_invalid@1`(N-6) |

**全 op が任意の `scope` を取ります**([解決](../../src/project/graphcomposition.cpp#L386))。既定は `"root"`(root 文書の `nodes`)、
definition 名を書くとその `definitions[<名前>].nodes` が対象です。**`root` は root scope の予約語**なので、
`root` という名前の definition は指せません。op は**順に**候補文書へ当たり、**最後に 1 回だけ展開**されます
([適用ループ](../../src/project/graphcomposition.cpp#L771)、[最後の 1 回の展開](../../src/project/graphcomposition.cpp#L904))。つまり `ops` は**全部通るか、文書ごと拒否か**のどちらかで、
途中まで書かれた文書は残りません。

#### 名前付きの拒否

| code | いつ |
|---|---|
| `graph.node_duplicate@1` | 同じ scope に同じ id を追加した |
| `graph.node_missing@1` | 無い node を指した(正は throw 箇所 [graphcomposition.cpp:373](../../src/project/graphcomposition.cpp#L378)。[宣言コメント](../../src/project/graphcomposition.hpp#L90)も `40099e3f` 以降は 8 つ全部を挙げていて、この表と一致します) |
| `graph.input_unknown@1` | instance の definition が宣言していない port を `set_input` / `set_node_parameters` した |
| `graph.output_referenced@1` | 他ノードの入力、またはその scope の `outputs` が指しているノードを削除した |
| `graph.input_referenced@1` | **instance がまだ値を供給している `graph.input`** を definition の中から削除した |
| `graph.nested_instance@1` | definition の中に `graph.instance` を足した(instance は root に立つもの) |
| `graph.input_scope@1` | root に `graph.input` を足した(境界 port は definition の中のもの) |
| `graph.definition_immutable@1` | id に `/` を含むノード(instance path 越しの指定)を **どの op でも**指した(`{reason: "instance_path"}`)。root scope の op が definition の中にいるノードを名指したときも同じ code で、`{definition}` が「どの scope を開けばよいか」を返します([`locateNode()`](../../src/project/graphcomposition.cpp#L364)) |

`graph.input_referenced@1` が要るのは、**その値が編集中の body の外にある**からです —— definition を開いている
キャンバスからは、その port に値を入れている instance が見えません([走査](../../src/project/graphcomposition.cpp#L423))。
op の形が壊れているだけのもの(`ops` が配列でない、`source` の鍵が違う、未知の `op` 名)は名前付きにならず、
`invalid_graph_document` として理由の文字列で返ります。

#### シーン編集との違い(こちら側です)

- **`actor_id` もセッションも要りません。**
- **scene journal とは別の履歴です。**Graph タブは成功した直近100操作を保持し、
  Undo / Redo ボタンとキャンバスの Ctrl+Z / Ctrl+Y（または Ctrl+Shift+Z）で逆編集・再編集を送ります。
  保存済み文書の digest が別の編集で変わった場合や再接続時は履歴を破棄します。
  シーン・Features・Pass Editorを横断する統一Undoではありません。RPCからも同じ編集opを使えます。
- **CAS は `SceneRevision` ではなく `base_source_digest`** —— 文書全体の sha256 です。受付でずれていれば
  `external_modification`、受付のあとフレーム境界までの間に別のチケットが先に書けば `stale_source_digest`。
  どちらも**何も書きません**。
- **1 操作 = 1 op 列 = 1 チケット**。同じフレームに 2 本目を投げても、後から来た方が CAS で落ちます。
  結果は `get_edit_result`(ヘッドレスなら `step_frame` の `edit_results[]`)で受け取ります。
- 書き戻しは root の `nodes` が常に、`definitions` は**変化したときだけ**です([definitions の書き戻し](../../src/core/communication/renderconfigeditor.cpp#L2291))。
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

### Studio のノードキャンバス(Graph タブ)

**置き場所**: `View > Panels > Frame Plan` の dock(`pelican.framePlanDock`)にある 3 枚目のタブ「Graph」です
([タブの追加](../../src/devstudio/view/mainwindow.cpp#L291))。dock は増えていません。起動時から開いておきたい場合は
`pelican_studio --project <dir> --tab graph`([第10章](10_tools.md) §10.7)。

**上段の操作列**(左から): `Scope:` のコンボ(`root` と definition 名)、その右のパンくず、右端に
`Values from:` のコンボ、`Refresh`([構築](../../src/devstudio/view/graphcanvaswidget.cpp#L2143))。
その下に必要なときだけ 1 行の notice が出ます。本体は左がキャンバス、右が選択ノードの詳細表(`Field` / `Value`)で、
詳細表の上に `Label:` の入力欄・**Rename**・**Add node...** が並びます。

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
   の順([解決順](../../src/devstudio/view/nodegraphics.hpp#L146)、入口は [`socketStyleFor()`](../../src/devstudio/view/nodegraphics.hpp#L148))。
   既定表の中はさらに 2 段で、**型名の完全一致 → `prefixes` の前方一致**です([nodegraphics.cpp:126](../../src/devstudio/view/nodegraphics.cpp#L135)) ——
   だから `pelican.render.` のような名前空間まるごとに 1 行で形を与えられます。既定表は
   [`graph_socket_styles.json`](../../src/devstudio/resources/graph_socket_styles.json#L1)(`pelican.studio.socket_styles` v1)で、
   **新しい型に形を与えるのに C++ を触る必要はありません** —— この JSON に名前を 1 行足すだけです。
5. **右の詳細表** —— `label` / `type` / `domain` / `evaluation` / `id` / `sink` と、あれば `definition` /
   `parameters` / `inputs` / `outputs`([組み立て](../../src/devstudio/view/graphcanvaswidget.cpp#L2609))。
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
同じ型のときだけ**通します([判定](../../src/devstudio/model/graphcanvasmodel.cpp#L1021))。

⚠ **2 つの注意。**
(1) **必須の scalar 入力を持つ op を足すと、種の定数も一緒に足されます。**未接続の必須入力を持つ文書は engine が
受け取らないので、パレットは必須の scalar 入力ごとに `value.constant` を**同じ ops 列で先に**足し、そこへ繋いだ
状態のノードを作ります([生成](../../src/devstudio/model/graphcanvasmodel.cpp#L1129))。1 操作 1 チケットは保たれます。
後で本物の producer を繋いでも、**この定数は自動では消えません**(手で消します)。
(2) **必須入力の切断は送られ、engine が断ります。**Studio は黙って定数を挿し直したりしません ——
`set_input{null}` をそのまま送り、`unconnected graph input` の拒否を notice に出します。値を持つ instance の port は
(戻り先の `parameters` が残っているので)切断できます。

**位置と折り畳みはセッション内の状態です。**`(scope, ノード id)` を鍵に覚えていて、**文書には書きません**。
revision が変わって消えたノードの位置は捨てられます。初期配置は最長経路による段組みで、辺は必ず左から右へ進みます。

**ズーム・パン・fit は 2 つのキャンバスで同じです(N-3)。**ホイールはカーソルの下の点を動かさずに拡大・縮小し、
倍率は **0.25〜4.0** で止まります([ホイール](../../src/devstudio/view/nodegraphics.cpp#L575))。パンは**中ボタン**ドラッグ、
または **Space を押しながら左**ドラッグ([判定](../../src/devstudio/view/nodegraphics.cpp#L676))。**`F`** キーと
右クリックメニューの **Fit view** で内容全体が view に収まります(Graph タブの右クリックは「Add node... / Fit view」の
2 項目です。[メニュー](../../src/devstudio/view/graphcanvaswidget.cpp#L1449))。この 3 つの操作は共有部品
[`NodeCanvasView`](../../src/devstudio/view/nodegraphics.hpp#L198) が 1 か所で持っていて、**Graph タブも Frame Plan の
Logical graph も同じ view** です。**まだ見ていない scope は自動で fit** され、戻ってきた scope は離れたときの倍率と
スクロール位置に復帰します([`showScope()`](../../src/devstudio/view/nodegraphics.cpp#L649)) —— 倍率も位置も文書には
書かない、セッション内の状態です。値の帯やラベルを編集している最中の `F` はただの文字です(開いている editor が
キーボードを持ちます。[条件](../../src/devstudio/view/nodegraphics.cpp#L717))。

**テーマはライトとダークの 2 つです(✅N-5)。**切り替えは **View ▸ Theme ▸ Light / Dark**
([メニューの構築](../../src/devstudio/view/mainwindow.cpp#L528))で、**再起動は要りません** —— 選んだ名前は
QSettings の [`studio/theme`](../../src/devstudio/view/studiotheme.cpp#L26) に残り、次に開いた窓は
そのテーマで始まります。追随するのは **Graph タブと Frame Plan の両方のキャンバス**で、背景・grid・辺・
ノードの body と縁・見出しの字・値の帯の色をテーマの表から引き直して描き直します。**ソケットの型別の色だけは
テーマに依存しません**(型の識別色なので、どちらのテーマでも同じ色です。テーマが決めるのは縁と、どの型とも
言えないソケットの色)。既定は `dark` で、`dark` のキャンバスの色は N-5 以前の色と同じ値です(完了記録
`6a26eef3`)。

**Auto layout(✅N-4)**: Graph と Frame Plan の右クリック menu「Auto layout」(`pelican.graphCanvas.autoLayout` / `pelican.framePlan.autoLayout`)で、その scope の手置き位置を捨てて層配置(最長経路の列、辺は lane を通って node を横切らない、列内の並びは重心法、行は最小間隔で重ならない)に戻します。動かした node は通常の refresh では保持されます。中ボタンのパンは描画が view に収まっていても動きます。

**応答性(Q-2)**: Studio は何も起きていない間は engine にほぼ問い合わせません(modal transform は gesture 中か engine 窓が focus を持つ間だけ、scene tree は revision が動かなければ 2 秒に 1 回、GPU 計時は panel が見えている間だけ)。Frame Plan の応答は 1 回だけ parse し、キャンバスは背景と grid を cache します。それでも Debug build は Release の 6〜19 倍遅いので、体感を見るなら Release 版で。project を開いた直後の固まりは Q-4 で消えました: Studio は engine の窓 thread が応答するまで埋め込みを待ち、その間は viewport に「Loading …(n s)」を出します。engine が log を書き続ける限り待ち、黙ったまま 120 秒(`PELICAN_STUDIO_ENGINE_LOAD_TIMEOUT_MS`)で停止します。Frame Plan の帯の editor も Graph と同じ規則(窓の瞬きで閉じない、再描画で残る、Escape で取消)です。

**Frame Plan のノード編集(N-8)**: Frame Plan でも出力ソケットから編集できる pass / task の入力ソケットへ**通常ドラッグ**で接続でき、入力ソケットを空所へドラッグするか辺を Delete で切断します。どれも既存の `edit_preset_pass` / `edit_compute_task` を宣言丸ごとで 1 回送るだけで、engine の pass や fork 前の preset、feature の pass は吸着せず理由を 1 行で出します。shader stem と task の dispatch は行の帯でその場編集、それ以外は node の double-click で今までの dialog。右クリック「Add fullscreen pass…」は Pass Editor をその node の後ろに prefill して開き、Delete は authored pass(`remove_authored_pass`)と task(`remove_compute_task`)を消します。Shift ドラッグは今までどおり dialog です。node の位置・reroute・tie・group は Graph の `layout` の `frame_plan` に一緒に保存されます。rendering 文書が legacy 形(root が `pelican.graph` でない)の project では保存されず、その旨と file 名を 1 行で出します。

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
([direct root の Pass Editor の拒否](../../src/devstudio/view/fullscreenpasswidget.cpp#L2070))。canvas も同じ文で断り、
engine が足した pass には「is added by the engine; it has no declaration in the project to edit.」と言います。どちらも
fork は勧めません([direct root の canvas の拒否](../../src/devstudio/view/frameplanedit.cpp#L126))。Pass Editor は pass を
選ぶと最初に form を空にするので、断られた pass の下に前の pass の下書きが残ることはありません(✅F-7)。

**Frame Plan で render target を宣言する(✅F-1)**: toolbar の **New render target...**(`pelican.framePlanNewRenderTarget`)か、
canvas の空所の右クリックの同名 action(`pelican.framePlan.newRenderTarget`)で form(`pelican.renderTargetForm`)が開きます
([form](../../src/devstudio/view/rendertargetform.cpp#L36))。欄は名前・種類(Scene color / Display / Data / Explicit format と
format)・大きさ(Screen ratio = `extent_scale`、または Fixed size = `width` × `height`)・usage(engine が返した語彙の
checkbox)・history・clear color(`r, g, b, a`、任意)です。**名前だけ入れて OK** すれば、画面と同じ大きさの scene color
(`R16G16B16A16_SFLOAT`、`COLOR_ATTACHMENT` + `SAMPLED`)になります。Fixed size でも `extent_scale: 1` は書かれます
(parser が必須にしているため。[宣言の組み立て](../../src/devstudio/view/frameplanedit.cpp#L657))。送るのは `edit_render_targets` 1 回です。
canvas は **Target**(`pelican.framePlanTarget`)で選んだ target を resource node として描き(自動化面の合成名は
`resource_node#<名前>`)、作った target は次の描画でそこに選ばれます(Target が **Whole frame** のときは選び直さず、
Whole frame のまま新しい target を描きます。下の段落)。その node の **double-click** で同じ form が
replace として開き、click で選んで **Delete** で remove します。replace は fragment の宣言を土台にするので、form に無い
欄(`mip_levels` など)は残ります。**他所で宣言された target は form が読み取り専用で開き**、出所を 1 行で出します
(「the screen output the engine provides」「a target of preset hybrid_v1」など)。Delete しても RPC は出ず、同じ出所の
文で断ります。engine の拒否は code ごとの 1 文になり、`render_target_referenced` なら使っている pass を並べます
([拒否の文](../../src/devstudio/view/frameplanedit.cpp#L738))。

**Whole frame —— まだ誰も読まない target をつなぐ(✅F-7)**: Target combo の先頭は **Whole frame** です。まだ表示して
いない graph(project を開いた直後を含む)はこれで開き、refresh の後は選んでいたものが残ります。選択は Studio を
閉じると忘れます。Whole frame は**全 pass** と、**どの pass も書かない宣言済み target**(宣言しただけでまだ使っていない
`scene_color` など)を 1 枚に描きます([`populateWholeFrame()`](../../src/devstudio/view/frameplangraphics.cpp#L2119))。
後者は F-1 の resource node として、それを最初に読む pass の 1 列左(読む pass が無ければ描画全体の左)に置かれ、
右端に出力ソケット(自動化面の合成名 `node_socket#<名前>/out:<名前>`)を持ちます
([置き方](../../src/devstudio/view/frameplangraphics.cpp#L2680))。**このソケットから pass の入力ソケットへドラッグすると、
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
- **Create missing targets** は空いた枠だけを既定の名前で埋めます: `scene_color`、`gbuffer_albedo` / `gbuffer_normal` /
  `gbuffer_material` / `gbuffer_worldpos` / `gbuffer_emissive`、`scene_depth`(宣言済みか他の枠が使っていれば `_2`…)。
  作る format は contract の表どおりで、forward の color は `explicit(R16G16B16A16_SFLOAT)`、G-buffer の albedo と
  emissive は `format_class: scene`(HDR で `R16G16B16A16_SFLOAT`、SDR で `B8G8R8A8_SRGB`。hybrid_v1 と同じ宣言)、
  normal と world position は `R16G16B16A16_SFLOAT`、material は `R8G8B8A8_UNORM`、depth は `D32_SFLOAT` です。
- **Name** は contract から自動で付きます(`forward_opaque`、`deferred_geometry`…。使われていれば `_2`…)。contract を
  変えると付け直しますが、自分で打った名前は変えません。

**Save は 2 つの要求**です。新しい target があれば、まず `edit_render_targets` 1 回(add の batch。
[Save の 1 本目 edit_render_targets](../../src/devstudio/view/materialpassedit.cpp#L184))、その commit の後に届く新しい
context の digest で `add_authored_pass` 1 回([Save の 2 本目 add_authored_pass](../../src/devstudio/view/materialpassedit.cpp#L208))。
新しい target が無ければ後者だけです。**pass が断られても、先に作った target は宣言されたまま残ります** —— form はそれを
Save の前に言い、拒否の status 行も残った target の名前を並べます
([拒否で残る target の文](../../src/devstudio/view/frameplanwidget.cpp#L1467))。いらなければ node から Delete してください。
既存の target を枠に選んだとき、Studio は format を先に検査しません(engine の preflight が上の段落の検査をします)。
入力(`screen_inputs` など)の欄はまだありません。

**forward の既定を同じ Save で入れる(✅G-4)**: Contract が **Forward opaque** で、pipeline に `deferred_geometry_v1` /
`legacy_gbuffer_v1` の material pass が無いとき、form に checkbox **Draw this project's materials with forward**
(`pelican.materialPassForm.forwardDefault`、既定 on)が出ます([forward の既定の checkbox](../../src/devstudio/view/materialpassform.cpp#L64))。
automatic な material(glTF の material、基本形の灰色、engine の組込)の route は既定では `deferred_geometry` なので、
forward の pass だけの pipeline には受け皿が無く、engine は上の `material_route_unserved@1` で断ります。on のまま Save すると、
`add_authored_pass` が pass と `material_route_default: "forward"` を 1 回で運び
([payload に既定を載せる所](../../src/devstudio/view/materialpassedit.cpp#L237))、form の notice にも「pass と設定は一緒に
保存され、engine が再起動する」旨が足されます。

commit の結果に `restart_required` があると、Frame Plan の status 行に
`Restarting the engine to apply the material route setting...` を出し、**同じ project で engine を止めて起動し直します**
([`EmbeddedViewport::restartEngine()`](../../src/devstudio/viewport/embeddedviewport.cpp#L491))。gate はすぐ閉じ、新しい engine が
readiness に答えると Frame Plan が戻ります。

**保存していない scene の編集があれば、再起動の前に問います(✅G-5)**: project の切替と同じ Save / Discard / Cancel が
「Save your scene changes before the engine restarts?」の文で出ます
([`MainWindow::requestEngineRestart()`](../../src/devstudio/view/mainwindow.cpp#L758))。未保存の編集が無ければ今までどおり
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
(組込には `(engine built-in)`)と直し方を言います([`materialRouteUnservedNotice()`](../../src/devstudio/view/materialpassedit.cpp#L259))。
G-1 のシナリオの fixture が持っていた、誰も読まない pass 2 つは G-4 で外しました
([fixture の README の G-4 の段落](../../test/fixtures/studio_material_pass_authoring/README.md#L21))。

**Pass Editor の form は scroll します(✅F-7)**: form は `QScrollArea`(`pelican.fullscreenPassScroll`)の中にあります。
以前は form の最小の高さ(1,000 px 近く)が下の dock 領域を押し上げ、Pass Editor を開くと viewport が 60〜80 px の帯に
潰れ、dock を閉じても戻りませんでした([置き場](../../src/devstudio/view/mainwindow.cpp#L304))。**その潰れた状態を
既に保存した layout は、そのままでは戻りません** —— splitter を引くか、**Layout > Reset to Default** で直してください。

**fullscreen pass を shader ごと作る(✅F-3)**: Pass Editor の Fragment 欄の横に **New…**
(`pelican.fullscreenPass.newFragmentShader`。project を開いていないと押せません)があります。押すと名前の行
(`pelican.fullscreenPass.shaderTemplateName`、既定は pass 名)が出て、**Create** で `<project>/shaders/<名前>.frag` を
雛形([resource](../../src/devstudio/resources/fullscreen_pass.frag.in))から書き、欄に `project://shaders/<名前>` を入れ、
desktop の既定のエディタで開きます。雛形は**その時点の入力の数だけ** `PELICAN_DECLARE_INPUT_i` と
`PELICAN_TEXTURE_2D_i` の行を持ち、最初の入力をそのまま出します。入力が無ければ UV の gradient です。buffer の入力と
9 個目以降の入力は宣言せず、注釈 1 行で残します。名前は英数字・`_`・`-` だけなので、`shaders` の外には書けません。
**既にファイルがあれば上書きしません** —— 「already exists」と **Open**(`pelican.fullscreenPass.openExistingShader`)が出て、
Open はそのファイルを使って開きます([書き込み](../../src/devstudio/view/fullscreenshadertemplate.cpp#L319))。
名前の行は、次に New pass を押すまで出たままです。

**雛形は 3 種類(✅G-3)**: 名前の行の左の combo(`pelican.fullscreenPass.shaderTemplateKind`)で、New… が書く出発点を選びます。

- **Pass-through** —— 上の F-3 の雛形です。
- **Deferred lighting** —— albedo / normal / material / world position / emissive と、任意の ambient occlusion を**入力の
  位置の順に**読み、`pelican_frame.glsl` の光源と `pelican_lighting_v1.glsl` で照らして scene-linear の色を出す短い shader
  です([Deferred lighting の雛形の lighting include](../../src/devstudio/resources/fullscreen_lighting_pass.frag.in#L9))。clustered
  lighting は light list に resource port が要るので `#undef` しています。選ぶと下の **Uses scene lights** と **Camera position**
  が入り(外しはしません)、入力の欄の下に役割名(`inputs: 0 albedo, 1 normal, ...`)が出ます。**入力は自動では埋まりません** ——
  役割名の順に自分で Add してください。足りない役割は shader の中で名前付きの定数になるので、compile は通ります。
- **Tone map** —— scene-linear の色(入力 0)を Reinhard で表示用にします
  ([Tone map の雛形の Reinhard](../../src/devstudio/resources/fullscreen_tonemap_pass.frag.in#L31))。engine の `output_transform` は
  表示の encode だけで tone map しないので、tone map はここで 1 回です。project の `hdr` feature が有効なら `hdr_tonemap` が
  tone map するので、この雛形は素通しになります。

**打つものが減りました(✅G-3)**: Vertex 欄の既定は `engine://fullscreen` です。New… は、pass 名が空(か New… 自身が入れた
名前)なら雛形の名前(`deferred_lighting` / `tone_map`、Pass-through は `fullscreen_pass`。使われていれば `_2`…)を入れ、
shader 名を pass 名から付けます。自分で打った名前は上書きしません([雛形の名前の提案](../../src/devstudio/view/fullscreenpasswidget.cpp#L1037))。
保存した後も form はその名前を持ったままです。

**既にある shader を読む pass の New…(✅G-5)**: pass が既に `shaders/<pass 名>.frag` を読んでいるとき(Blank pipeline の
`present` を開いて New… を押したときなど)、名前の提案は `<pass 名>_<雛形>`(`present_tone_map` / `present_deferred_lighting`)で、
そのファイルもあれば `_2`、`_3`… を足します。Pass-through なら `<pass 名>_2` からです
([既存の file を避ける shader 名の提案](../../src/devstudio/view/fullscreenpasswidget.cpp#L817))。以前は今読んでいるファイルの名前を
提案したので、Create が「already exists」になっていました。今のファイルは上書きされずに残ります。

**pass の設定欄(✅G-3)**: fullscreen pass の form に 2 つの欄が増えました(add / edit の両方)。

- **Frame data** の行(`pelican.fullscreenPass.interface`): **Uses scene lights**(`uses_light_data`)と push constants の
  combo(No push constants / Camera position / Projection and view = `push_constants` の `camera_position` /
  `projection_view`)です([Frame data の行](../../src/devstudio/view/fullscreenpasssettings.cpp#L115))。どちらも pass の
  contract と fingerprint に載る宣言で、GPU への供給は変わりません —— light の UBO(set 0 binding 2)はこの欄に関係なく
  すべての pass に bind され、camera の位置は push されず、雛形は `pelicanFrame.camera_position` を読みます
  ([第6章](06_rendering.md) §6.2 の fullscreen の項)。
- **color 出力ごとの行**(`pelican.fullscreenPass.outputOps`、color 出力の欄の下): Load(Default / Load / Clear /
  Don't care)・clear colour(`r, g, b[, a]`)・Store(Default / Store / Don't care)
  ([Load / Clear / Store の行](../../src/devstudio/view/fullscreenpasssettings.cpp#L225))。列の見出しはありません(左から
  出力名・Load・clear colour・Store)。**`load_op` / `store_op` の正は今までの `output.color` の JSON 欄**で、この行は JSON 欄を
  映し、変えると JSON 欄へ書き戻します(`subresource` などほかの field は残ります)。**clear colour は attachment object には
  書けない**(attachment object が受けるのは `target` / `subresource` / `load_op` / `store_op` だけ)ので、pass の
  **`clear_colors`**(出力名が鍵)に入ります。swapchain に書く出力の `clear_colors` の鍵が、swapchain → display の正規化の
  下でどうなるかは未確認です。

保存は今までどおり `add_authored_pass` / `edit_preset_pass` で、edit は宣言から変わった field だけを送ります(変えずに
保存すれば送らず、外すと `null` で消します)。

保存が **shader の compile 失敗**で断られる(`render_pipeline_preflight_failed` の message に "Shader compile failed" が
ある)と、form の下の折りたためる欄(`pelican.fullscreenPass.compileLog`)に log 全文が等幅で出ます。行番号が付き、
log が名指す `file:line` は強調されます。notice 行は「shader compile failed; see the log below」だけで、**form に打った
値はそのまま残る**ので、shader を直してもう一度 Save できます。成功すると欄は消えます
([経路](../../src/devstudio/view/fullscreenpasswidget.cpp#L1124))。canvas の帯で直接 stem を変えて compile で断られた
ときは、今も status の 1 行だけです。

**shader を外のエディタで保存すると画面が変わる —— reload と失敗の印(F-3 / ✅F-5)**: Frame Plan は描いている node の
shader file を監視します。Studio は engine を `--rpc` で起こし、その reload gate は `rpc driver` を理由に閉じている
([gate](../../src/core/watch/reloadgate.cpp#L32))ので、engine は file の変更を自分では拾いません。そこで Studio は、
**書き込みが 200 ms 途切れたら `reload_shaders` を 1 回**送ります(同時に 1 本。返事を待つ間の書き込みは、返事の後に
1 回分だけ送り直します。[debounce](../../src/devstudio/view/shaderreloadfollower.hpp#L75))。定期の timer は無いので、
Q-2 の何もしない間の RPC は増えません。答えごとに:

- **読み直した**: node の印、Frame Plan の status 行の reload の文、Pass Editor の log 欄を消します。viewport は次の
  frame から新しい shader で描きます。
- **compile 失敗**(`shader_reload_failed@1`): engine の log の path が node の shader に当たれば、その node の見出しの
  右上に赤い印(`node_shader_error#<pass>`、tooltip に log の先頭行)を描き、当たらない失敗は status 行に 1 行出します。
  **Pass Editor が開いていれば、form の下の log 欄**(保存の compile 失敗と同じ欄)にも log 全文が出ます。engine は前の
  shader で描き続けます。
- **engine が断った**(`shader_reload_unavailable@1`、replay / strict assets の engine): status 行に「A shader file
  changed; this engine does not reload shaders (<理由>).」。この文はこのときだけ出ます。
- それ以外の失敗: 「Could not ask the engine to reload shaders: ...」。

読み直しか失敗の後には `get_status` の `reload.runtime["pelican.shaders"]` を 1 回読み、印を engine の記録に合わせます
([答えの扱い](../../src/devstudio/view/frameplanwidget.cpp#L1148))。

**配線の制御点と置き場所(N-6)**: 辺の double-click で **reroute**(通過点)が入り、drag で動き、Delete で消えます。node を動かしても reroute は動きません。制御点や辺を 2 つ以上選んで右クリック **Tie together** で 1 つの **tie**(結束バンド)になり、辺は等間隔で平行に通ります(**Untie** で戻す、double-click で並びを反転)。**Auto layout** は lane を reroute と tie として出します。node の位置・reroute・tie は文書の scope ごとの `layout`(editor 用、engine は無視)に `set_layout` で残り、次に開いても同じ図です。Frame Plan の位置はまだセッション内です。

**配置の保存は canvas を描き直しません(✅Q-5)**: node を drag した後、配置の変更がまだ送られていない間(300 ms の
debounce の間)は、notice が **「Saving the graph change...」** になります(error の notice だけはこれより優先)
([配置が未送信の間の notice](../../src/devstudio/view/graphcanvaswidget.cpp#L2738))。保存が返ると「Graph change saved.」です。
以前は前の保存の「Graph change saved.」が残ったままだったので、その文を待つシナリオは今の保存を待てませんでした。
また、**配置だけが変わった読み**(前の読みと `layout` 以外が同じ)では、model は node の値を消して読み直さず
([配置だけの読みの判定](../../src/devstudio/model/graphcanvasmodel.cpp#L440))、canvas が既にその配置を描いていれば
(自分の drag の保存が返ってきたとき)scene を作り直しません —— 選んでいる node・辺・reroute はそのまま残ります
([`GraphCanvasScene::followDocumentPlacement()`](../../src/devstudio/view/graphcanvaswidget.cpp#L570))。Undo や別の書き手が
違う配置を書いた読みは描き直し、ほかの変更が混じった読みは描き直して選択を引き継ぎます。Frame Plan の配置の保存には、
もともと描き直しがありません(読み戻しも選んだ node を保ちます)。
⚠ engine が配置を丸めて返すと、送ったものと一致しないので「Saving…」のまま残ります。編集できる状態にならないままでも
同じです。

**パネルの文字(✅N-7)**: 各パネルの既定状態に出るのは見出し・ラベル・値・1 行の状態だけです。仕組みの説明は control の tooltip か、パネル右上の `?`(この章の該当節を開く)にあります。検査の一覧は記号(✓ / ✗ / –)で、文は hover。notice は言うことが無いときは消えます。文字の大きさとフォントはテーマ表(`studio_themes.json` の `font`)から取り、`devstudio_paneltext_test` がパネルごとの可視文字の上限を固定します。

**このタブが出せる RPC は 5 本だけです。**`GraphCanvasReadCapability`
([定義](../../src/devstudio/view/graphcanvasreadcapability.hpp#L31))は「汎用の RPC メソッド」を表現できない形をしていて、
`get_graph_instances` / `get_graph_catalog` / `get_graph_values` / `edit_graph_document` / `get_edit_result` の
5 つだけを名前で持ちます。それ以外を要求すると `The graph canvas cannot issue <method>.` になります
([分配](../../src/devstudio/view/graphcanvaswidget.cpp#L2682))。キャンバスから preset を fork したり pass を触ったりは
**できない**、というのが型で保証されています。

### Studio の自動化面(`--automation`)

**これは「GUI の動作確認を AI だけで完結させる」ための面です**(Q-1、利用者指示 2026-09-17)。
人が窓を見て確かめる代わりに、シナリオが実物の窓を実物の engine 付きで動かし、AI が結果の report を読んで
「緑 / 赤と理由」を判定します。**人のレビューは使い勝手に使います。**

**口は 1 つだけです。**`pelican_studio --automation <名前>` で `QLocalServer`(Windows では `\\.\pipe\<名前>`)が開き、
**改行区切りの JSON 1 行 = 1 命令**を受けます([flag](../../src/devstudio/main.cpp#L16) /
[構築](../../src/devstudio/view/uimain.cpp#L37))。

```text
-> {"id": 4, "cmd": "click", "name": "pelican.graphCanvasRefresh"}
<- {"id": 4, "ok": true, "result": {...}}
<- {"id": 4, "ok": false, "error": {"code": "automation.widget_missing@1", "message": "..."}}
```

**flag が無い起動では server object が作られないので、socket が存在しません**
([宣言のコメント](../../src/devstudio/automation/automationserver.hpp#L26)) —— 出荷時の挙動は変わりません。
チャネルは**逐次**で、命令の実行中に次の行が来たら `automation.busy@1` で断ります
([判定](../../src/devstudio/automation/automationserver.cpp#L102))。認証はありません(flag を付けたときだけ開く口です)。

**命令は 20 個**です([一覧](../../src/devstudio/automation/automationcommands.cpp#L1843)):
`list` / `click` / `double_click` / `drag` / `wheel` / `move` / `press` / `release` / `key` / `type` / `text` /
`action` / `menu_list` / `view_state` / `wait` / `wait_engine` / `rpc` /
`open_project` / `screenshot` / `quit`。`rpc` は `EmbeddedViewport` 経由で engine へ送り、応答をそのまま返します。
イベントは `QTest` の合成なので、**窓が前面にあるかどうかに依存しません**。拒否は名前付きで、
`automation.widget_missing@1` / `bad_request@1` / `unknown_command@1` / `engine_not_ready@1` / `rpc_failed@1` /
`wait_timeout@1` / `screenshot_failed@1` / `project_failed@1` / `busy@1` / `command_threw@1` の 10 種です
([定数](../../src/devstudio/automation/automationcommands.cpp#L166))。

**指し方は objectName です。**キャンバスの中に描かれたもの(ノード、ソケット、値の帯、その場編集の editor)は
widget ではないので、`<view の objectName>::<種類>#<ノード id>[/<port>]` の合成名で指します ——
例: `pelican.graphCanvasView::graph_value_pill#left/gain`([名前の生成](../../src/devstudio/automation/automationcommands.cpp#L205))。
`list` は widget 名と合成名の両方を返すので、**ソースを読まずに一覧からシナリオを書けます**。
`type` / `key` / `text` は item が抱えている埋め込み widget へ届きます。`select{name, text|index}` は item view の行を本物の click で、combo は index を直接置いて選び、`wait{enabled}` は有効化を待ちます(AG-3b)。出力 port の帯は `graph_value_band#<node>/<port>`([経路](../../src/devstudio/automation/automationcommands.cpp#L366))。

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
[README](../../test/studio_scenarios/README.md#L55))。step は「命令をそのまま送るもの」か「期待」で、期待は
`rpc_equals`(engine の応答の `path` が `value`)/ `result`(任意の命令を送り、応答の `path` を記録するか `value` と比べる)/
`text_equals` / `visible` / `list_count` / `screenshot`(保存のみ)の 6 種です([判定](../../tools/studio_drive.py#L685))。`{project}` / `{artifacts}` と `--var NAME=VALUE` は step のすべての文字列に差し込まれます。
`result` の比べ方 `op` は `eq`(既定)/ `ne` / `gt` / `ge` / `lt` / `le` / `contains` / **`excludes`**(✅F-7。文字列がその値を
含まない —— 「その文言は出なかった」の書き方。[比較](../../tools/studio_drive.py#L338))です。`result` も **`retry_ms`**(✅F-5)を受け、
条件を満たすか時間が尽きるまで 0.2 秒おきに問い直します(`rpc_equals` は前から受けていました)。shader を保存した後に
engine が描くものは engine の時計で届くので、`fullscreen_pass_from_template` は `screen` の screenshot をこれで待ちます
([問い直し](../../tools/studio_drive.py#L716))。
3 つ目の種類の **`file` step**(✅F-3)は Studio へ送らず、driver 自身が run の持つ directory の中の file を 1 つ読み書きします ——
人が Studio の操作の合間に自分のエディタで file を直す、の代わりです。`write` / `replace`(`old` がちょうど 1 か所のときだけ)/
`contains` / `exists` の 4 つです。書ける範囲は **`{project}`(fixture の複製)と `{artifacts}`(その run の成果物の置き場)の
2 つ**で、File > New Project でシナリオが作った project は `{artifacts}` の下にあるので、そこにも届きます(✅F-4)。
それ以外の path(`..` で外へ出るものを含む)は step の失敗です([file step の範囲の検査](../../tools/studio_drive.py#L569))。
driver の stdout / stderr は、cp932 の console で出せない文字があっても止まりません。
`"probe_no_automation": true` を書くと、**先に flag 無しの Studio を起こして socket が無いことを確かめて**から本番を起動します。
最初の失敗で run は止まります。

**回し方**: [`tools/studio_drive.py`](../../tools/studio_drive.py#L982)(Python 3.11以上、標準ライブラリだけ)が Studio を
`--automation --project --tab` で起動し、step を順に送り、`report.json` を書きます。ctest からは
`studio_scenario_<名前>` として GPU lane で走ります([登録](../../test/CMakeLists.txt#L3001)) —— **実物の窓が要るので
`QT_QPA_PLATFORM=offscreen` は掛けません**。fixture の project は temp に 2 つ複製されるので、シナリオは
文書を書き換えてよく、「ディスクから読み直す」も試せます([複製](../../test/run_studio_scenario.cmake#L30))。
CTest登録にはStudio targetと`PELICAN_PYTHON_TESTS=ON`（またはPythonが見つかった`AUTO`）が必要です。
`uv`は不要で、完全なGPU gateは全scenarioの登録・実行を必須にします([CI運用](../ci.md))。

**run ごとに空の設定 directory を使います(✅F-5)。**Studio は環境変数 **`PELICAN_STUDIO_CONFIG_DIR`** があれば、利用者の
設定 directory の代わりにそこへ dock の layout を読み書きします([置き場の決定](../../src/devstudio/view/mainwindow.cpp#L92))。
driver は run ごとに `{artifacts}/studio_config` を空にして作り、この変数で渡します([渡し方](../../tools/studio_drive.py#L1025))。
以前のシナリオは、前の run(や人が使った Studio)が利用者の設定 directory に残した dock の layout を復元し、自分の
layout をそこに残していました。テーマの選択はこの directory ではなく `QSettings` に残るので、この変数の影響を受けません。

**report.json は AI が読む形です**([組み立て](../../tools/studio_drive.py#L1136)): `verdict`(`green` / `red`)と
`ok` / `failed_steps`、`summary`(step ごとに 1 行)、`failures`(送った命令・返ってきた応答・言葉での理由・
**失敗した瞬間のスクリーンショット**)、`steps`(全文)、`studio_log`(Studio と engine の console 出力)。
`studio_exit_code` と `studio_shutdown_forced` も記録します。最後の shutdown step は
**exit 0、強制終了なし**を要求し、異常終了・タイムアウトによる強制停止も report と driver の失敗になります。

現在のシナリオは **20 本**です。
`graph_read` は fixture を Graph タブで開き、`list` が objectName を返すこと・無い名前が
`automation.widget_missing@1` で断られること・キャンバスが描いたものが `get_graph_instances` の中身と合うことを見ます
([シナリオ](../../test/studio_scenarios/graph_read.json))。`graph_edit_roundtrip` は**実物の engine で**値の帯を
click → type → Enter し、engine が `3.75` を持つこと、別の複製に切り替えると `2.5` のままであること、戻ってくると
`3.75` が残っていることを追います([シナリオ](../../test/studio_scenarios/graph_edit_roundtrip.json))。
✅Q-1b の 3 本は `canvas_zoom_pan`(Frame Plan のキャンバスを wheel / 素の move / 押したままの move / 中ボタン
drag で動かし、`view_state` で結果を読む)、`menu_actions`(popup を開かずにメニューバーを `menu_list` で読み、
`action` で発火する)、`graph_canvas_zoom_pan`(N-3 が入った Graph キャンバスの zoom / fit / pan)です。
加えて `theme_switch`、`graph_auto_layout`、`graph_reroute_tie`、`graph_observe_roundtrip`、
`frameplan_edit_roundtrip` がテーマ・配置・配線・値の観測・Frame Plan編集の往復を確認します。
✅F-2 の `new_blank_project` は File > New Project... の dialog にフォルダ(`{artifacts}` の下)・名前・Blank pipeline を
入れて Create し、開き直った Frame Plan に `present` と `output_transform` があること、viewport の screen モードの
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
desktop で開かず、`automation: shader source open <path>` を log に書きます([差し替え](../../src/devstudio/view/uimain.cpp#L50))。
✅F-4 / F-6 の `render_pipeline_from_scratch`(97 step)は「0 から組める」第一段階の筋書きを端から端まで追います:
File > New Project... で `{artifacts}` に Blank pipeline の project を作り(画面は `present` の gradient)、New render
target... で `scene_color` を名前だけで宣言し、Add fullscreen pass... で `draw_pattern` を `present` の前に足して
`scene_color` に書かせ、New... の雛形を `file` step で 8 本の縞に書き換えて Save します。`present` に `scene_color` を
読ませる手順は、F-4 の時点では Studio で通らず `file` step の手書きで迂回していましたが、**✅F-6 から Studio の操作です**:
`present` を見出しで開き(editable な node の中ほどは shader の帯で、押すとその場編集になるため)、Save が root を書くこと・
engine が root の pass として出すことを確かめ、Inputs に `scene_color` を Add し、`present.frag` を `file` step で
「入力 0 をそのまま出す」shader に書き換えてから Save します(root への `edit_preset_pass`。preflight と pipeline 再構築が
新しい `present.frag` を compile します)。
そのあと viewport の screenshot が 4 色以下で、同じ縞の上下が同じ色・隣の縞が違う色・手順 1 の gradient のその点とも
違う色であること、canvas と engine が `draw_pattern` → `present` の順であることを見て、もう一度別の project を開いて
戻っても同じであることを確かめます(Studio の再起動ではありません)([シナリオ](../../test/studio_scenarios/render_pipeline_from_scratch.json))。
✅F-7 の `whole_frame_first_steps`(33 step)は、既定 layout に戻してから、最初の frame plan が Whole frame で出ること、
Pass Editor を View > Panels で閉じた・開いた・閉じた後に viewport が帯にならず高さが戻ること、File > New Project... で
Blank pipeline に切り替えても新しい graph が Whole frame で開き、Engine Log(`pelican.engineLog`。gizmo の失敗は status bar と
同時にここへ残ります)に「polling failed」が無いこと(`excludes`)、New render target... で作った `scene_color` がその出力
ソケットごと `present` と同じ canvas に描かれ、canvas が Whole frame のままであることを見ます
([シナリオ](../../test/studio_scenarios/whole_frame_first_steps.json))。
✅G-1 の `material_pass_authoring`(43 step。G-4 で 38 から増えました)は、model を 1 つ持つ direct root の fixture で、
Add material pass... を名前で発火し、form が Forward opaque・名前 `forward_opaque`・graph の先頭で開くこと、
Create missing targets が `scene_color` と `scene_depth` を contract の format で作ること、forward の既定の checkbox が出て
on であること(✅G-4)を見ます。Save の後は、Studio が engine を再起動し(status 行の「Restarting the engine」)、root に
`material_route_default` が書かれ、engine がその 2 つの target を authoring fragment から宣言し、`forward_opaque` を
forward opaque の material pass として最初に走らせることを見ます。続けて `present.frag` を `file` step で「入力 0 をそのまま
出す」shader に書き換え、canvas で `scene_color` を `present` へ drag し、viewport が gradient から数色の絵になって中央の
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
engine が保存済みの `Cube` を持つこと、`present.frag` を「入力 0 をそのまま出す」に書き換えて `scene_color` を `present` へ
drag すると viewport の中央に背景とも前の gradient とも違う色の箱が出ることを確かめます。最後にもう一度 Add Object > Cube で
`Cube_001` ができて選ばれること、Inspector が読み終えてから Save し直すことを見ます(閉じるときに確認が出ないように)
([基本形のシナリオ](../../test/studio_scenarios/add_primitive_object.json))。G-2 の着地時は forward の pass だけでは箱が
描かれず、Deferred geometry の pass で迂回していました。G-4 で理由が分かり(material pass が 1 つも無いうちに置いた cube は
検査されず、forward の pass を足す編集も「前の世代が使っていた route の消失だけを断る」旧規則を通っていた)、forward に
戻っています。
✅G-5 の 2 本は、第二段階の筋書きを Blank pipeline から Studio の操作だけで追います。
`forward_loop_from_scratch`(112 step)は File > New Project... で Blank pipeline の project を作り、Add Object で Cube と Plane を
置いて、Plane を Inspector の transform の欄で床にします(`pelican.inspectorVector.transform.pos.1` に `-0.5`、`.scale.0` と
`.scale.2` に `8`)。Add material pass... で Forward opaque を Create missing targets と forward の既定つきで Save すると、scene が
未保存なので再起動の前に Save / Discard / Cancel が出て、Save で箱と床を保存してから engine が再起動します。続けて `present` を
見出しで開き、Inputs に `scene_color` を Add し、New... の Tone map で `present_tone_map` を作って Save すると、viewport の箱・床・
背景が互いに違う色(前の gradient とも違う色)になります。別の project を開いて戻っても同じ 3 色です
([forward ループのシナリオ](../../test/studio_scenarios/forward_loop_from_scratch.json))。
`deferred_loop_from_scratch`(148 step)は同じ出だしに加えて Cube を縦軸の回りに回し(`.transform.rotation.1` と `.3`)、
Add material pass... の Deferred geometry で G-buffer 5 枚と `scene_depth` を作ります(route は既定のままなので再起動は
求められません)。New render target... で `lit_color` を作り、canvas の Target を **Whole frame** にして `deferred_geometry` を
見出しで選び、Add fullscreen pass... で G-buffer 5 枚を読み `lit_color` に書く pass を New... の Deferred lighting で作ります
(置き場所は `deferred_geometry` の後で、engine はこの pass を geometry と `present` の間で走らせます)。`present` は Tone map で
`lit_color` を読みます。光源が箱の 2 面を違う色に塗ること、Inspector で光源の向き(`pelican.inspectorVector.light.direction.0`)を
変えると光の離れた面の色が変わることを見ます
([deferred ループのシナリオ](../../test/studio_scenarios/deferred_loop_from_scratch.json))。
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

⚠ **2 つの注意。**(1) `open_project` に**今開いている directory** を渡すと何も起きません(`EmbeddedViewport::openProject`
がそう作られています)。ディスクから読み直させたいときは、別の project を開いてから戻ります。
(2) `screenshot` の既定(`widget` モード)は Qt の widget 木を描くので、**埋め込まれた engine の窓は写りません**
(別 HWND です)。engine が描いた絵が要るときは `"mode": "screen"` を使います。**✅F-5 から `screen` は、Studio の窓
自身の合成済みの内容を `PrintWindow`(`PW_RENDERFULLCONTENT`)で読んで切り出す**ので、別の application の窓が
viewport に重なっていても写りません([窓の内容の読み取り](../../src/devstudio/automation/automationcommands.cpp#L80))。
窓が print できないとき、または 1 色で出て**しかも desktop のその場所に Studio の窓が実際に見えているとき**だけ、以前の
desktop の grab に戻ります。見えているかは、撮る範囲の 5×5 の点で `WindowFromPoint` が Studio の窓(埋め込みの engine の窓を
含む)を指すかで見ます([desktop に窓が見えているかの判定](../../src/devstudio/automation/automationcommands.cpp#L142))。
別の窓が重なっていれば 1 色の print をそのまま使い、応答に **`desktop_obstructed: true`** を付けます。1 色は正しい絵でもあり得る
(消去されただけの target、空の scene の albedo)ので、重なった別の application の窓を撮るより print を残します(G-4 の後の修正
`5ca18ee1`。以前は 1 色なら必ず desktop に戻り、重なった窓を撮っていました)。
どちらで撮ったかは応答の **`capture`**(`print_window` / `screen`)に出ます。着地時に別の窓の重なりで 2 本のシナリオが
赤になったための変更です。
`screenshot` の応答は `path` / `mode` / `capture` / `width` / `height`(と、上の場合だけ `desktop_obstructed`)に加えて **`distinct_colors`**(絵に含まれる色の数)を返します
(✅F-2、[screenshot の distinct_colors](../../src/devstudio/automation/automationcommands.cpp#L1820))。消去されただけの viewport は 1 色なので、
`result` step で `distinct_colors` を読めば「engine が何か描いたか」を PNG を開かずに判定できます。
**`"pixels": [[x, y], ...]`**(✅F-4)を付けると、応答にその各点の色が `"#rrggbb"` の配列 `pixels` で返ります
([screenshot の pixels の読み取り](../../src/devstudio/automation/automationcommands.cpp#L1827))。x / y は絵の幅・高さに
対する 0〜1 の割合なので、窓の大きさを知らずに書けます。配列でない・2 要素でない・範囲外なら `automation.bad_request@1` です。
`result` step の `remember` で 1 点の色を覚え、別の点を `value_of` と `op`(`eq` / `ne`)で比べれば「同じ縞の下も同じ色、隣の縞は違う色」の
ように、output transform がどんな色にしても絵の形を判定できます。

**この面が最初の往復で本物のバグを見つけました。**値の帯を click → type → Enter すると「Graph change saved.」と
出るのに古い値が保存される —— `QDoubleSpinBox` の `keyboardTracking=false` と、editor の filter が Enter を先に
消費する組合せで、scene test の合成 Enter では通っていました。修正は `commitInlineEdit()` で `interpretText()` を
呼ぶことです([修正](../../src/devstudio/view/graphcanvaswidget.cpp#L1718)) —— **`--automation` 無しでも挙動が変わる
唯一の変更**で、手で編集したときの取りこぼしもこれで直っています。

### Inspector のグラフ装着

Inspector の `Add component...` / `Ray geometry...` の下、**properties の scroll 領域より上**に
**Attached graphs** グループがあります([置き場](../../src/devstudio/view/inspectorwidget.cpp#L235)。
properties は [:219](../../src/devstudio/view/inspectorwidget.cpp#L261) の `pelican.inspectorProperties` です)。
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
   ([繋ぎ込み](../../src/devstudio/view/mainwindow.cpp#L326))。接続されていない port で押すと
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
  [`EditorEditErrorCode`](../../src/core/communication/editorjournal.hpp#L49) と、その名前を返す
  [`editorEditErrorCodeName()`](../../src/core/communication/editorjournal.cpp#L3013)。**列挙子の名前とワイヤ上の
  コード名は一致しません** —— 新しいものは `@1` 付きです)。内訳:
  - 基本 21 種: `stale_revision` / `gate_closed` / `preview_lease_conflict` / `preview_lease_busy` / `not_lease_owner` / `ticket_not_found` / `undo_conflict` / `not_editable` / `schema_violation` / `component_unknown@1` / `duplicate_component` / `missing_component` / `name_conflict` / `parent_not_found` / `closure_unresolvable` / `cycle_detected` / `zero_scale` / `non_finite_transform` / `trs_unrepresentable` / `preserve_missing` / `method_unavailable`
  - preview: `editor_preview_unsupported@1`(✅WP375c。列挙子は `preview_unsupported`。**inactive なシーンの add / remove / reparent プレビュー**はこれで拒否されます)
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
  ([set_parameters の検査](../../src/core/communication/renderconfigeditor.cpp#L1457))。
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
- 保存ファイルには各 object の `authoring_object_id` と文書直下の `next_authoring_object_id` が入ります。旧文書も最初の保存で ID が記録され、object の削除・並べ替え後に再び開いても graph の装着先が維持されます。非表示のシーンも保存対象です。返される digest と `byte_count` はこの保存ファイルの値です([第4章の ID 規則](04_scene_ecs.md))。

### スナップショット(✅WP168)

`export_scene_snapshot` は文書の意味的なバイト列とダイジェストを返し、`import_scene_snapshot` はそれを**検証してから**取り込みます。検証の順序(バージョン → サイズ上限 16 MiB → SHA-256 → パースと意味検証 → シーンの存在)は契約として固定されています。**どの段階で失敗しても、通常のプロジェクトのシーンファイル・キャッシュ・revision は一切変化しません**。

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

with PelicanRpc(["pelican_player", "--headless", "--rpc", "--project", "mygame"]) as rpc:
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
2. **ウィンドウモードで `step_frame` を呼ばない** — 余分にフレームが進みます。結果は `get_edit_result` をポーリングして取ります。
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
  [effecttransaction.cpp](../../src/core/loader/effecttransaction.cpp#L681))です。
- `import_scene_snapshot` の応答にも同じ `committed` / `effect_errors` が付きます
  ([editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp#L2033))。
  **`{"status":"imported","scene_revision":N,"current_scene_id":"…"}` だけを期待する
  厳密一致のクライアントは壊れます。**
- 代表的な `error_code`: `effect_outbox_full@1{dropped}`(1 回の scene load で 256 件を超えた分)/
  `persistence_read_failed@1{path, reason}`(壊れた save slot。以前は「slot 不在」に化けていました)。

### `get_status.participants.game_logic` の `error_code` / `error_fields`

`get_status` に `participants.game_logic` が入りました。**まだ 1 度も reload を試していない
起動直後でも、startup の初期化失敗がここに出ます**([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1121))。

- 試行済みなら `reload.runtime.game_logic` の内容(`configured` / `loaded` / `reloading` /
  `pending` / `source_busy_retries` / `generation` / `systems` / `source` / `loaded_copy` /
  `domain_error` / `error_code` / `error_fields`)がそのまま入ります
  ([reloadservice.cpp](../../src/core/watch/reloadservice.cpp#L656))。
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
