# 設計: Studio の汎用 component 編集面 —— 追加・削除・行の編集を schema catalog から(v1、2026-09-02)

## 0. なぜ今か

WP372c レビューと RT3 設計で確定: **Studio には component を追加・削除する経路が無い**
(`inspectorwidget.cpp:519` の `addComponent` は widget 配置 helper であって RPC ではない)。
core には `add_component` / `remove_component` の op が既にある。**穴は Studio 側だけ**。
`pelican_ray_instance`(WP374/WP379)を含め、**新しい component を足すたびに Studio から使えない**状態が続く。
「web で開ける ⊆ pelican で開ける」は満たしているが、Studio は常に一歩遅れる。

## 1. 実測(2026-09-02)

| 事実 | 場所 |
|---|---|
| Studio が使う RPC は `edit` / `get_components` / `scene_tree` / `save_scene` / `set_component_value`。**`add_component` は 0** | `src/devstudio` の grep |
| inspector は固定 schema field だけを widget 化し、未 authored field は編集拒否 | [inspectormodel.cpp:237](../src/devstudio/model/inspectormodel.cpp#L237)・:558 |
| schema は `inspectormodel` が RPC catalog 経由で受ける(Studio は `pelican_core` を link しない = D0) | [src/devstudio/CMakeLists.txt:46](../src/devstudio/CMakeLists.txt#L46) |
| offline Studio の BindableDescriptor は project 側の**手書き一覧** | [project.cpp:109](../src/devstudio/model/project.cpp#L109) |
| prefab parameter 表は `NoEditTriggers`(読み取り専用) | [inspectorwidget.cpp:468](../src/devstudio/view/inspectorwidget.cpp#L468) |
| core の `add_component` op と、担体設計 C-2 の `insert_component_field` / `remove_component_field`(未実装) | [editorjournal.cpp:1135](../src/core/communication/editorjournal.cpp#L1135) / design_declaration_carrier §2 |

## 2. 決定

### 決定 1: UI は **schema catalog から生成する汎用面**。component 名を Studio に書かない

- 「追加できる component の一覧」は **RPC の schema catalog**(既存の `get_components` の schema 経路と同じ供給元)から取る。
  Studio のソースに component 名の表を持たない(第二 registry を作らない —— WP375 決定 4 と同じ原則)
- 追加時の初期値は **catalog の既定値**(既定値の所在は一箇所)。Studio 側で既定を持たない

### 決定 2: 操作は **core の既存 op にそのまま写像**する。Studio 固有の意味論を作らない

| Studio の操作 | 送る op | 前提 |
|---|---|---|
| component を追加 | `add_component`(既存) | 今すぐ可能 |
| component を削除 | `remove_component`(既存) | 今すぐ可能 |
| 値の変更 | `set_component_value`(既存) | 現行 |
| list の行を追加・削除 / 欠落 field の作成 | `insert_component_field` / `remove_component_field` | **担体 C-2 の後** |

undo / redo・CAS・衝突は core の journal のまま。Studio は結果を表示するだけ。

### 決定 3: **D0 を守る形**

Studio が知ってよいのは **wire の schema**(型・既定・範囲・enum・list の要素 schema)だけ。
codec の C++ 型・`ComponentCodecRuntimeKind`・BindableDescriptor の C++ 定義には触れない。
offline(engine 非接続)では、**project 層が同じ schema catalog を JSON で供給**する
(今の手書き一覧を **catalog 生成に置き換える**。WP372c 第 2 巡が要求した「BindableDescriptor は project 所有の
同一 schema から生成」と同件)。

### 決定 4: 段割り

| 段 | 内容 | 前提 |
|---|---|---|
| **S-1** | component の追加・削除 UI(catalog 由来の一覧 + 既定値、`add_component` / `remove_component`) | 無し(今すぐ) |
| **S-2** | offline catalog の生成(手書き一覧の置換) | project 層の schema export |
| **S-3** | list 行の追加・削除と欠落 field の作成 UI | 担体 C-2 |

## 3. 受け入れの骨

- **S-1**: Studio から `pelican_ray_instance` を object に追加 → `save_scene` → engine で開いて実効値が出る(WP374 の TLAS row で観測)。
  削除 → save → 消えている。**Studio のソースに component 名を持たない**ことは、
  「catalog に無い component を Studio が提示できない」対照で示す(catalog から 1 つ外すと一覧から消える)
- **S-2**: offline と online で **同じ一覧**(手書き一覧を消すと offline の一覧が空になる = 対照)
- **S-3**: 行の追加 → save → engine の list に行がある。担体 C-1/C-2 の受け入れを Studio 経由で再演
- 全段: D0 の link 境界不変、Studio のテスト(`SKIP_DEVSTUDIO=OFF` 構成)を壊さない

## 4. やらないこと

Studio 固有の undo / 衝突解決 / 既定値 / component の意味論。prefab parameter 表の編集(U2)。

## 5. 敵対レビューへの問い

(1) `get_components` の schema 経路は「追加できる component の全一覧」を出せるか(今は authored なものだけか)
(2) S-1 が単独で有用か —— `pelican_ray_instance` 以外に今日 Studio から足したい component は何か
(3) offline catalog を project 層から出すとき、behavior(DLL 由来の schema)はどう扱うか(WP361a の「Studio offline 乖離」と同件)
(4) `add_component` の既定値は誰が埋めるか(core の op が catalog 既定で埋めるのか、Studio が payload を送るのか)


## 6. 設計レビュー第 1 巡(9 件)と v2

**v2 が本文書の正。§2 決定 1〜4・§3 受け入れは v2 で置換される。「S-1 は前提無し・今すぐ」は撤回する。**

### 決定 1'(#1): catalog は表示 schema ではなく **構築契約**

`add_component` は未完成 component を挿入して後から編集する API ではなく、**受信時に完全な component を canonicalize**する
([editorjournal.cpp:1141](../src/core/communication/editorjournal.cpp#L1141))。`sprite_view` は `texture`、`animation` は `clip`、
`simplemodelview` は `model` が必須で、`{"name":"pelican_ray_instance"}` も field 不在なら `ray_instance_empty` で拒否される。
現在 RPC に出る `StructFieldSchema` は型・範囲・enum だけで **required/defaulted と default 値を失っている**
([editorcommandservice.cpp:70](../src/core/communication/editorcommandservice.cpp#L70))。

- catalog entry は **`{required/defaulted, canonical default, picker kind(asset / object / enum / scalar)}`** を持つ
- Studio は **required 値を draft で集め**、完全な payload を `add_component` に送る。core は canonicalize する。
  **既定値の所在は core の canonical default 1 箇所**(Studio は catalog から受けた値を表示するだけ)

### 決定 2'(#2): 全体 catalog とは別に **object ごとの可否**を core が返す

generated `collider` を持つ prefab instance では、一覧には出るが read-only、削除は authored 側に無いので missing、
追加は merged prefab の重複規則で拒否、唯一の authored `transform` の削除は resolve で失敗する。
- **`object_component_capabilities(object)`** → `[{entry_id, occurrence, can_add, can_remove, reason, origin, multiplicity}]`
- Studio は意味論を持たず、`can_*` と `reason` を**そのまま**表示する。generated には **`unpack_prefab` への導線**(既存 RPC)

### 決定 3'(#3): behavior の entry は component 名ではなく **`{slot:"behavior", subtype, schema_version, fingerprint, multiplicity, constructor_schema}`**

`behavior` は同名を複数持て、追加には DLL 登録由来の `type`、削除には `attachment_index` が要る。
UI model は **attachment identity と順序**を持つ。

### 決定 4'(#4): catalog に **generation と entry fingerprint**。add は **`expected_catalog_generation`** を必須にする

DLL reload を跨いだ add は `catalog_stale@1` で拒否し、**draft は保持**する(利用者が一覧で見たものだけを追加できる)。

### 決定 5'(#5): S-2 は **供給元を D0-safe な leaf / 生成物に移す**ことが前提。目的を限定する

canonical builtin schema / default は core の `componentcodec.cpp` にあり、`pelican_project` は schema leaf にしか link しない。
- canonical descriptor / default を **D0-safe な leaf または生成 artifact** に移し、**core codec と project exporter の双方がそこから生成**する
  (例外契約設計の「既定値の所在は一箇所」、担体設計の「schema-leaf / project 層が wire 型を所有」と同件)
- S-2 の目的は **offline authoring の parity**(engine 未接続で同じ一覧・同じ default)に限定する。
  **今日の Inspector は engine 接続・actor・snapshot が揃わないと編集できない**
  ([inspectormodel.cpp:1625](../src/devstudio/model/inspectormodel.cpp#L1625))ので、消費者が無ければ S-2 は起票しない

### 決定 6'(#6): 構造編集の ticket は **selection から独立**した session-level tracker で terminal まで追う

`applySelection` は `pending_edits` を消し、旧 generation の結果を捨てる。add 直後に別 object を選ぶと結果が消える。
- structural ticket は selection 変更で消さず、terminal(committed / rejected)まで poll し、完了後に対象 object を再取得

### 決定 7'(#7): structured error を **field / action に束縛**し、失敗時の draft を保持する

core は `payload` に object / slot / field path / detail を返す([editorjournal.cpp:2121](../src/core/communication/editorjournal.cpp#L2121))。
- Inspector は error を該当 field / 追加ボタンに結び付ける(例外契約設計の `resolution_error` と同型)
- **`last_saved_revision` と dirty / unknown-pending** を追跡し、stop / restart / open / close で **Save・Discard・Cancel**
  (今日は「Stop Engine」で session state が消える: [embeddedviewport.cpp:216](../src/devstudio/viewport/embeddedviewport.cpp#L216))

### 決定 8'(#9): **前提を認める**。S-1 は担体の RPC 2 本と改名の後

- catalog RPC は担体設計 v2 決定 8' の **`component_schema_catalog` / `occurrence_inventory`** と同じもの。**別に作らない**
- **RPC capability / version handshake**: 新 Studio が旧 engine に接続したら `incompatible_engine@1` と **read-only fallback**
  (既存 `get_components` への黙った fallback は authored-only の不完全一覧になるので禁止)
- `pelican_ray_instance` は **WP379 の改名後の canonical 名**を受け入れへ載せる(旧名を固定しない)
- **「S-1 は前提無し・今すぐ」は撤回**。順序: CARRIER-1(catalog RPC + 改名)→ S-1 → S-3(行編集)

### 決定 9'(#8): 受け入れの差し替え(false positive を潰す)

| v1 | 問題 | v2 |
|---|---|---|
| `pelican_ray_instance` を追加 → TLAS row で観測 | remove 後も row は残り fallback 値になる | **同一テスト**: component 無し fallback → UI から **`mask=17 / force_opaque / custom_index=0x123456`** を追加 → 実 resolved TLAS 値 → remove → fallback 復帰 |
| catalog から外すと一覧から消える | `intersect(catalog, {"pelican_ray_instance"})` の hardcode でも通る | **実 production registry に、Studio ソースに無い一意な test component を登録**し、実 RPC → 実 UI → add / save / reload。**登録無し側も同じテスト**で |
| offline / online で同じ一覧 | 両方空でも通る。名前だけ一致でも通る | **非空 sentinel + 完全な canonical descriptor** を比較し、**両経路で実際に解決された値**を比較 |
| 行の存在 | 既定行・手書き fixture で通る | 事前不在 → 固有値で追加 → engine の list に固有値 → 削除 |

### 段割り(v2)

```
CARRIER-1(catalog RPC 2 本 + pelican_ray_instance 改名)
   → S-1(追加・削除。構築契約 + object 可否 + ticket tracker + error 束縛 + handshake)
   → S-3(行編集。CARRIER-1 の C-2 の上)
S-2 は消費者(offline authoring の parity)が名指しされたときだけ起票
```

**この文書の 1 巡目レビューはここまで。2 巡目は CARRIER-1 の仕様確定後**(catalog RPC の形が決まってから)。
