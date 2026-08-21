# 第13章 エディタとエディタ RPC

対象: pelican2(2026-08-20 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- **`pelican_player` が「編集サーバ」になった**という今回いちばん大きな変化(✅WP149〜172)
- 編集の正本 `AuthoringSceneDocument` と、シーンファイルとの関係
- 編集の実行モデル — チケット・フレーム境界コミット・global CAS
- 編集 RPC のリファレンスとエラーの読み方(**総数は挙げない** —— メソッドが増えるたび腐るため)
- プレビューの 4 段梯子(どこまでが「本物の絵」か)
- 保存(`save_scene`)・スナップショット・ImGui のエディタパネル・Python クライアント

## 13.1 全体像 — 何ができるようになったか

2026-07-18〜19 のウェーブで、実行中の `pelican_player` に対して**外部からシーンを編集する API** が入りました。オブジェクトの生成・削除・親子付け替え・コンポーネントの追加削除・値の変更・undo/redo・プレビュー・保存が、すべて JSON-RPC のメソッドとして呼べます。

> **設計決定(D0 の具体化 — 編集 API は RPC ひとつだけ):** 編集操作は**まず RPC メソッドとして定義**され、エンジン内の ImGui インスペクタも、外部の Python クライアントも、将来の Qt エディタも、**同じ関数を呼ぶ**。エディタ専用の裏口 API は作らない。結果として「人間の GUI 操作」と「エージェントの自動操作」が同じ意味論・同じテストになる。

⚠ 用語の注意: この章でいう「エディタ」は次の 2 つを指します。Qt 版の Pelican
Studio(`pelican_studio`)は別 process player の native viewport を持ち、schema-driven Inspector と
gizmo からもこの章と同じ編集 RPC / preview lease を呼びます([第10章](10_tools.md) §10.7)。

1. **編集 RPC**(この章の §13.3〜§13.8)
2. **エンジン内蔵の ImGui パネル**(§13.9 — Object Tree / Inspector / Asset Browser)

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

- 文書は `pelican.scene` v1 のエンベロープ丸ごと(全シーン・全オブジェクト・全コンポーネントの生 JSON)を保持します。
- **`AuthoringObjectId`**(セッション内で安定・1 始まり)と **`SceneRevision`**(単調増加)が編集の座標系です。ランタイムの `EntityId` は付随情報で、**同一性の代用にはしません**(オブジェクトが再生成されると変わるため)。
- コンポーネントの受理・正規化は [第4章](04_scene_ecs.md) §4.2 の component codec が担当します。値を書くと**必ず codec の正規形に書き直されて**保存されます。

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

編集メソッドは**その場で編集しません**。「受け付けた」という**チケット**を返し、実際の適用は次のフレーム境界の一点で行われます。

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

### 編集操作(operations)の 6 種

`edit` の `operations[]` には次の 6 種を並べます。1 回の `edit` は**全部成功か全部失敗**です。

| op | 主なフィールド | 意味 |
|---|---|---|
| `set_component_value` | `object_id`, `component_slot`, `field_path`(JSON pointer), `value` | 値の変更。`/name` への書き込みは拒否 |
| `add_component` | `object_id`, `component`(コンポーネント JSON) | コンポーネント追加 |
| `remove_component` | `object_id`, `component_slot`(behavior は `attachment_index` も必須) | コンポーネント削除 |
| `spawn` | `object`(+ `scene_id`) | オブジェクト生成。ID は engine が採番 |
| `destroy` | `object_id` | 子孫ごと削除(逆操作用に全 JSON を保存) |
| `reparent` | `object_id`, `new_parent_id`(null = ルート), `preserve`(`"local"` / `"world"`) | 親子の付け替え |

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
- 編集メソッドには必ず `base_revision`(見ている文書のリビジョン)を添えます。**文書全体で 1 つの `SceneRevision` を使った CAS(Compare-And-Swap)**なので、他の誰かが**無関係なオブジェクト**を編集しただけでも `stale_revision` で弾かれます。これは v1 の意図的な制約です — 最新の revision を取り直し、**操作内容も作り直してから**再送してください(revision だけ差し替えて再送するのは誤りです)。
- 現在の revision は `get_scene_revision` で取れます(`preview_epoch` / 直近トランザクション / プレビューリースも一緒に返ります)。変化を検出する目的にはこれをポーリングします。

## 13.5 編集 RPC リファレンス

RPC は全 46 メソッドで、そのうち **23 個がエディタトラックで増えた分**です(残り 23 個の基盤メソッドは [第10章](10_tools.md) §10.3)。

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

### レンダー設定の `features[]` を編集する(3 メソッド)

**シーン編集とは実行モデルが違います。**混ぜると壊れます。

| メソッド | 何をするか |
|---|---|
| `get_render_features` | 著作 config の現在の `features[]`、`source_reference`、`source_digest`、runtime の状態を返す |
| `list_render_features` | **engine が可用性を判定した**カタログ。各項目に `available` と `unavailable_reason` が付く |
| `edit_render_features` | `base_source_digest` と `operations[]` を受け、**チケットを返す**。結果は `get_edit_result` か `step_frame` の `edit_results[]` |

**シーン編集との違い:**

- **`actor_id` もセッションも要りません。**
- **undo / redo できません。journal にも載りません。**
- **CAS は SceneRevision ではなく、ルート設定ファイルの sha256 digest** です。
  **メモリ上の文書とディスク上の実物の両方**を照合し、食い違えば `external_modification` で**何も書きません**。
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

**適用は 3 つの graph variant(flat / xr / preview)をまとめて再コンパイルします。**
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
- **シーン編集**の安定コードは 21 種(`stale_revision` / `gate_closed` / `preview_lease_conflict` / `preview_lease_busy` / `not_lease_owner` / `ticket_not_found` / `undo_conflict` / `not_editable` / `schema_violation` / `unknown_component_type` / `duplicate_component` / `missing_component` / `name_conflict` / `parent_not_found` / `closure_unresolvable` / `cycle_detected` / `zero_scale` / `non_finite_transform` / `trs_unrepresentable` / `preserve_missing` / `method_unavailable`)。
- **`edit_render_features` はこの 21 種では尽きない別系統**です。共通するのは `gate_closed` だけで、ほかに `unknown_render_feature` / `restart_required_feature` / `render_feature_unavailable` / `render_feature_not_enabled` / `external_modification` / `stale_source_digest` / `render_pipeline_preflight_failed` / `render_config_rollback_failed` / `render_pipeline_commit_protocol_error` を返します(§13.5)。
- 保存・スナップショット側も別系統(`save_busy` / `runtime_only_data` / `external_modification` / `digest_mismatch` / `snapshot_too_large` など)。

### 編集がそもそもできない状態(gate)

`can_edit` が false を返す理由は 5 つだけです: **リプレイ中 / ゴールデンモード / `--strict-assets` / シーン遷移やリロードの最中 / プレビューリースの衝突**。`--rpc` や `--headless` は理由になりません(それらは編集を妨げません)。

## 13.7 プレビューの梯子(どこまでが「本物」か)

用途に応じて 4 段あります。

| 段 | メソッド | 実行中のシーンを触るか | 何ができるか |
|---|---|---|---|
| ① チケットプレビュー | `open/update/commit/abort_preview` | **触る**(実際に反映される) | ドラッグ操作のライブ反映。🚧 **`set_component_value` の `transform` と `light` だけ**。他は `method_unavailable` |
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

## 関連文書

- [../design_editor_tooling.md](../design_editor_tooling.md) — エディタツール設計(v2.5・条件付き受理。レビュー条件 E-C1〜E-C5 と V22-C1〜C7 が本文に優先します)
- [../design_object_behaviors.md](../design_object_behaviors.md) — behavior 設計([第8章](08_gameplay.md) §8.5)
- [../design_devstudio_direction.md](../design_devstudio_direction.md) — D0(エディタ特権の禁止)と Qt Studio の段階
- [第4章 シーンと ECS](04_scene_ecs.md) / [第8章 ゲームロジック](08_gameplay.md) / [第10章 ツールリファレンス](10_tools.md) / [第11章 実装状況](11_status.md)
