# 第13章 エディタとエディタ RPC

対象: pelican2(2026-09-04 時点、HEAD=`d146b8bd`)/ このマニュアルはコードを正とする

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

1. **編集 RPC**(この章の §13.3〜§13.8)
2. **エンジン内蔵の ImGui パネル**(§13.9 — Object Tree / Inspector / Asset Browser)

### Studioでcomponentを追加する

Inspectorの **Add component...** で登録済みの型を選び、**Add**で追加します。
初期値はエンジンのcomponent codecから取得し、追加後は通常のInspectorで編集できます。
モデル・sprite・procedural geometryは登録assetの一覧から選び、**Use selected assets**で宣言JSONへ反映できます。
procedural geometryは複数選択でき、1件なら`asset`、複数なら`assets`を設定します。
選択はassetの項目だけを更新し、その他の入力は保持します。**Add**を押すまでは草稿です。
ファイルの無いassetも`missing`として表示し、inlineのprocedural assetは`declared`と表示します。
登録asset名などを宣言JSONに直接入力する操作も利用できます。
入力内容は通常のcomponent codecで検査します。
追加は既存のjournalに記録され、Undo/Redoで取り消し・再適用できます。ディスクへの保存にはSaveを使います。

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

- 文書 1 つ = **シーンファイル 1 本**です。`pelican.scene` v1 のエンベロープ丸ごと —— そのファイルの `scenes` が宣言する全シーン、その中の全オブジェクト、その全コンポーネントの生 JSON —— を保持します([authoringscenedocument.hpp](../../src/core/loader/authoringscenedocument.hpp#L109))。プロジェクトに複数のシーンファイルがあれば文書も複数で、`export_scene_snapshot` はその全部を返します。
- **`AuthoringObjectId`**(セッション内で安定・1 始まり)と **`SceneRevision`**(単調増加)が編集の座標系です。ランタイムの `EntityId` は付随情報で、**同一性の代用にはしません**(オブジェクトが再生成されると変わるため)。
- コンポーネントの受理・正規化は [第4章](04_scene_ecs.md) §4.2 の component codec が担当します。値を書くと**必ず codec の正規形に書き直されて**保存されます。ここでいう**正規形**は、書いた JSON をいったん codec で読み(`decodeAuthored()`)、その値からもう一度書き出した(`encodeCanonical()`)形のことです —— 鍵の並び・省略した既定値・数値の型が一意に決まるので、同じ意味の JSON は必ず同じバイト列になります([componentcodec.hpp](../../src/core/loader/componentcodec.hpp#L250))。

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

### 編集操作(operations)の 9 種

`edit` の `operations[]` には次の 9 種を並べます(正は
[`editorjournal.cpp` の op 分岐](../../src/core/communication/editorjournal.cpp#L2914))。1 回の `edit` は**全部成功か全部失敗**です。

| op | 主なフィールド | 意味 |
|---|---|---|
| `set_component_value` | `object_id`, `component_slot`, `field_path`(JSON pointer), `value` | 値の変更。`/name` への書き込みは拒否 |
| `add_component` | `object_id`, `component`(コンポーネント JSON) | コンポーネント追加 |
| `remove_component` | `object_id`, `component_slot`(behavior は `attachment_index` も必須) | コンポーネント削除 |
| `get_procedural_assets` | なし | procedural asset宣言と編集元digest |
| `edit_procedural_asset` | `base_source_digest`, `asset_name`。編集は`aabb`/`record`、新規時は`operation: "create"`と両方の値、削除は`operation: "delete"`のみ | procedural assetの新規作成・編集・削除・保存・runtime反映 |
| `spawn` | `object`(+ `scene_id`) | オブジェクト生成。ID は engine が採番 |
| `destroy` | `object_id` | 子孫ごと削除(逆操作用に全 JSON を保存) |
| `reparent` | `object_id`, `new_parent_id`(null = ルート), `preserve`(`"local"` / `"world"`) | 親子の付け替え |
| `set_prefab_parameter` | `object`(**`object_id` ではない**), `parameter`, `value` | prefab インスタンスのパラメータ上書き |
| `unset_prefab_parameter` | `object`, `parameter` | 上書きを外して prefab 側の既定へ戻す(既定が無ければ拒否) |
| `unpack_prefab` | `object`(このほかのキーは拒否) | prefab インスタンスを素のオブジェクトへ展開 |

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
| `list_assets` | `{store?}` | アセット一覧(id / 種別 / パス / 状態) |
| `query_journal` | `{after_transaction_id?}` | 編集履歴(undo の種) |
| `can_edit` / `can_preview` | `{}` | 編集・プレビューの可否と理由 |

`declaration_index` は scene の `objects` 配列における 0 始まりの宣言位置です。直リンクで読んだ木との対応には `(scene_id, declaration_index)` を使います。`authoring_object_id` はセッション内で安定した別の参照なので、挿入・並べ替え後は両者が一致するとは限らず、どちらも応答に残ります。

`get_components` が返すコンポーネントには、**`schema`(フィールド名・型・範囲・単位)と `editable` フラグ**が含まれます。インスペクタのウィジェットはこれだけで組み立てられます(§13.9)。

ここでいう**解決**は、オーサリング文書の生 JSON を codec に通し、既定値を補ってランタイムの値にするところまでを指します。範囲外の数値・未知の鍵・型違いなど、利用者が書いた内容が契約に反していれば解決は失敗します。

**解決に失敗したコンポーネントも消えません(✅WP378)。** authored / generated のどちらの直下にも
`resolution_error` が付き、`get_components` は**成功応答のまま**それを返します
([editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp#L1743))。形は 2 通りで、
typed な違反は `{code, path(RFC6901), fields, message}`、まだ typed 化していない旧分岐は
`{legacy_branch_id, message}` です(正は
[`resolutionFailureJson()`](../../src/core/loader/resolutionresult.hpp#L114))。
**`resolution_error` を含む応答はキー順が保存されます** —— `RpcServer` が応答木を走査して
`resolution_error` を見つけたら ordered JSON で直列化するためです
([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1111))。

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

### レンダー設定を編集する

**シーン編集とは実行モデルが違います。**混ぜると壊れます。違う理由は対象が違うからです —— シーン編集の対象がメモリ上の `AuthoringSceneDocument`(正は `SceneRevision`)なのに対し、こちらは**ディスク上の rendering config ファイルそのもの**で、正はその sha256 digest です。したがって版の比較も、undo の有無も、書き込みの作法も別建てになります。

| メソッド | 何をするか |
|---|---|
| `get_render_features` | 著作 config の現在の `features[]`、`source_reference`、`source_digest`、runtime の状態を返す |
| `list_render_features` | **engine が可用性を判定した**カタログ。各項目に `available` が付き、**`unavailable_reason` は `available` が false の項目にだけ現れる**(使える項目には鍵ごと無い) |
| `edit_render_features` | `base_source_digest` と `operations[]` を受け、**チケットを返す**。結果は `get_edit_result` か `step_frame` の `edit_results[]` |
| `edit_render_features` / `materialize` | operationに`op: "materialize"`・`scope: "root"`または`"preset"`・`index`・`destination_reference`・`base_feature_digest`を指定。presetの場合は`base_preset_digest`も必要。具体値へ展開したfeatureを未作成のprojectファイルに保存し、元instanceをその参照へ置換する。featureのdigestは`get_render_features.feature_instances[].feature_document_digest`、presetのdigestは同entryの`source_digest`を使う。engine presetは先にforkする |
| `get_scene_acceleration_structures` | 名前付きTLASの`definitions[]`、taskの接続先`task_connections[]`、編集元の`source_digest`、三角形座標系とレイアウトの編集可否・digestを含む`triangle_geometry`を返す |
| `edit_scene_acceleration_structures` | `base_source_digest`と`definitions[]`、任意の`task_connections` objectでTLASと接続先を置換。`triangle_space`と`base_layout_digest`を組で渡すとレイアウトの三角形座標系も同時保存し、既存の編集ticketを返す |
| `get_ray_callable_records` | taskごとのCallable schema、shader一覧、元宣言と実効初期値、rootの`source_digest`を返す |
| `edit_ray_callable_records` | `base_source_digest`と`task_records` objectで初期値の上書き指定全体を置換。既存の編集ticketを返す |
| `fork_render_pipeline` | `base_source_digest`・`base_preset_digest`・`destination_reference`で選択中presetを新しいprojectファイルへコピーし、rootの参照先も切り替える |
| `edit_region_replacement` | `base_source_digest`・`base_preset_digest`・`graph`・`region`でproject presetのタグ付き領域を指定。`provider`に登録済みprovider名を指定するとコンパイル時の差し替えを選択し、`null`なら差し替え要求を除去して元のノード構成を使用する。元のpass・設定・接続は保持する。現在の指定は`get_render_authoring_context`の各graphの`region_replacements`で取得できる。標準feature全体の展開操作ではない |
| `edit_preset_pass` | `base_source_digest`・`base_preset_digest`・`graph`・`pass_name`でproject presetのpassを指定。fullscreenは`input`・`output`・`shader`と任意の`ray_query`、materialは`input`・`output`・`ray_query`の1項目以上。rasterはさらに`shader`・`draw`・`raster_state`を部分編集できる。省略項目は保持、rasterの`raster_state: null`は既定状態へ戻す |
| `get_compute_task_authoring` | compute/RT taskの元宣言・編集可否・保存先とdigestを取得する |
| `edit_compute_task` | `base_source_digest`・`base_task_source_digest`・`task_name`・`task`でtask宣言を置換する。名前は保持する |
| `add_compute_task` | `base_source_digest`・`destination_reference`・`task`で新規project featureにtaskを作り、rootのfeaturesへ追加する |
| `remove_compute_task` | `base_source_digest`・`base_task_source_digest`・`task_name`で元文書からtaskを削除する |

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
project所有のray group layout v2/v3が編集対象です。**Apply**でAS定義・接続と一緒に保存し、失敗時は元設定を保持します。

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
- **v1 は `operations` が最大 1 要素**、`op` は `add` と `remove` のみ。
- **可用性の判定は engine 側**です。studio が理由を組み立てることはありません。
  モジュールの動的生成を要する feature は名指しで断られます([第6章](06_rendering.md) §6.5)。
- **no-op は runtime にもファイルにも届きません**(digest が一致すれば `no_change`)。
  同じ内容で 2 回適用しても世代は進まず、履歴も reset されません。

**編集面が存在しないプロジェクトがあります。**ルートの rendering config に
最上位 `features` 鍵が無いと、以前はサービスごと作られず 3 メソッドとも
「render config editor service is unavailable」で失敗しました。
現在は**初期化操作が `features: []` を byte-lossless に挿入する**ので、
手書きや legacy の config でも編集面が作られます。

**適用は graph variant をまとめて再コンパイルします。**flat と preview は常に、**XR は `PELICAN_WITH_OPENXR` かつ実行中の XR セッションがあるときだけ**です。
したがって **preview を壊す候補は、feature を 1 つ足しただけでも preview 由来の理由で拒否されます。**

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
  [`EditorEditErrorCode`](../../src/core/communication/editorjournal.hpp#L46) と、その名前を返す
  [`editorEditErrorCodeName()`](../../src/core/communication/editorjournal.cpp#L4567)。**列挙子の名前とワイヤ上の
  コード名は一致しません** —— 新しいものは `@1` 付きです)。内訳:
  - 基本 21 種: `stale_revision` / `gate_closed` / `preview_lease_conflict` / `preview_lease_busy` / `not_lease_owner` / `ticket_not_found` / `undo_conflict` / `not_editable` / `schema_violation` / `component_unknown@1` / `duplicate_component` / `missing_component` / `name_conflict` / `parent_not_found` / `closure_unresolvable` / `cycle_detected` / `zero_scale` / `non_finite_transform` / `trs_unrepresentable` / `preserve_missing` / `method_unavailable`
  - preview: `editor_preview_unsupported@1`(✅WP375c。列挙子は `preview_unsupported`。**inactive なシーンの add / remove / reparent プレビュー**はこれで拒否されます)
  - prefab: `prefab_instance_id_collision` / `prefab_dependency_mismatch` / `prefab_provider_stale` / `prefab_parameter_required` / `prefab_parameter_unknown` / `prefab_parameter_type` / `unpack_must_be_singleton@1` / `prefab_digest_changed@1` / `prefab_provider_changed@1` / `prefab_behavior_dependency_changed@1`
  - origin 遷移(undo/redo の同一性検査): `origin_transition_*@1` 8 種
  - そのほか: `journal_integrity_error@1` / `behavior_type_unknown@1` / `runtime_binding_missing@1`
- **`not_editable` と `component_unknown@1` の切り分け(✅WP379/379a)**: `add_component` は
  production の component registry に問い合わせます。**登録されているが editor codec が無い**なら
  `not_editable`、**そもそも登録されていない**なら `component_unknown@1{"name": "…"}` です
  ([editorjournal.cpp](../../src/core/communication/editorjournal.cpp#L225))。以前は実 DLL が登録した
  codec 無しコンポーネントも一律「未登録」と報告していました。
- **`edit_render_features` はこの 21 種では尽きない別系統**です。共通するのは `gate_closed` だけで、ほかに `unknown_render_feature` / `restart_required_feature` / `render_feature_unavailable` / `render_feature_not_enabled` / `external_modification` / `stale_source_digest` / `render_pipeline_preflight_failed` / `render_config_rollback_failed` / `render_pipeline_commit_protocol_error` を返します(§13.5)。
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

なお、プレビュー専用の**第 3 のレンダーグラフ variant**(flat / XR に続く `preview`)が起動時に必ずコンパイルされます。TAA・velocity・history・UI・XR ミラーなど「時間方向やスワップチェーンに依存する feature」は除外され、除外できない構成は**起動時にパス名入りで拒否**されます([第6章](06_rendering.md))。

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

### スナップショット(✅WP168)

`export_scene_snapshot` は文書の意味的なバイト列とダイジェストを返し、`import_scene_snapshot` はそれを**検証してから**取り込みます。検証の順序(バージョン → サイズ上限 16 MiB → SHA-256 → パースと意味検証 → シーンの存在)は契約として固定されています。**どの段階で失敗しても、通常のプロジェクトのシーンファイル・キャッシュ・revision は一切変化しません**。

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

Qt Inspectorは再帰schemaのListを行IDと値の一覧で表示します。`Add`で行の値をJSON入力し、`Edit`で選択行の値を変更、`Remove`で削除、`Up`/`Down`で順序を変えます。`$row`や`next_id`を入力する必要はありません。移動は同じ行IDを保持し、通常のUndo/Redoと保存を使用します。省略されたListにも行を追加できます。generated・pending・read-onlyのcomponentとList内部のネストしたListは編集対象外です。

`Ray geometry...`は選択objectのgeometry候補、実効値、overrideのcomponent/行と適用状態、拒否理由を表示します。候補と追加先のray_instanceを選び、`Add override`で行を作成し、InspectorのListで値を編集します。編集やmodel reloadの後は`Refresh`で候補を再取得してください。

geometryを選択すると、mask・instance flags・custom index・geometry flagsの実効値、適用元、優先順を項目別に表示します。
最初の3項目は既定値→component→occurrence overrideで、未指定fieldは継承し、明示的な0は上書きです。
geometry flagsは既定値とtriangleのgeometry rule、proceduralではその既定値から決まります。layout v1のcustom indexはrow qです。
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
  [`effectErrorsJson()`](../../src/core/loader/scene.cpp#L480))。`api` は
  `playSound` / `saveData` / `loadScene` などの**公開 API 名**、`phase` は
  `activate` / `destroy` / `flush` の 3 値、`error_fields` はコードごとに決まった固定の鍵
  (`{path, reason}` / `{slot, reason}` / `{bus, reason}` / `{dropped}` など。正は
  [effecttransaction.cpp](../../src/core/loader/effecttransaction.cpp#L686))です。
- `import_scene_snapshot` の応答にも同じ `committed` / `effect_errors` が付きます
  ([editorcommandservice.cpp](../../src/core/communication/editorcommandservice.cpp#L1968))。
  **`{"status":"imported","scene_revision":N,"current_scene_id":"…"}` だけを期待する
  厳密一致のクライアントは壊れます。**
- 代表的な `error_code`: `effect_outbox_full@1{dropped}`(1 回の scene load で 256 件を超えた分)/
  `persistence_read_failed@1{path, reason}`(壊れた save slot。以前は「slot 不在」に化けていました)。

### `get_status.participants.game_logic` の `error_code` / `error_fields`

`get_status` に `participants.game_logic` が入りました。**まだ 1 度も reload を試していない
起動直後でも、startup の初期化失敗がここに出ます**([rpcserver.cpp](../../src/core/communication/rpcserver.cpp#L1211))。

- 試行済みなら `reload.runtime.game_logic` の内容(`configured` / `loaded` / `reloading` /
  `pending` / `source_busy_retries` / `generation` / `systems` / `source` / `loaded_copy` /
  `domain_error` / `error_code` / `error_fields`)がそのまま入ります
  ([reloadservice.cpp](../../src/core/watch/reloadservice.cpp#L599))。
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
- [第4章 シーンと ECS](04_scene_ecs.md) / [第8章 ゲームロジック](08_gameplay.md) / [第10章 ツールリファレンス](10_tools.md) / [第11章 実装状況](11_status.md)
