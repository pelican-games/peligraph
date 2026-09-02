# 設計: Studio の汎用 component 編集面 —— 追加・削除・行の編集を schema catalog から(v1、2026-09-02)

## 0. なぜ今か

WP372c レビューと RT3 設計で確定: **Studio には component を追加・削除する経路が無い**
(`inspectorwidget.cpp:519` の `addComponent` は widget 配置 helper であって RPC ではない)。
core には `add_component` / `remove_component` の op が既にある。**穴は Studio 側だけ**。
`ray_instance`(WP374)を含め、**新しい component を足すたびに Studio から使えない**状態が続く。
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

- **S-1**: Studio から `ray_instance` を object に追加 → `save_scene` → engine で開いて実効値が出る(WP374 の TLAS row で観測)。
  削除 → save → 消えている。**Studio のソースに component 名を持たない**ことは、
  「catalog に無い component を Studio が提示できない」対照で示す(catalog から 1 つ外すと一覧から消える)
- **S-2**: offline と online で **同じ一覧**(手書き一覧を消すと offline の一覧が空になる = 対照)
- **S-3**: 行の追加 → save → engine の list に行がある。担体 C-1/C-2 の受け入れを Studio 経由で再演
- 全段: D0 の link 境界不変、Studio のテスト(`SKIP_DEVSTUDIO=OFF` 構成)を壊さない

## 4. やらないこと

Studio 固有の undo / 衝突解決 / 既定値 / component の意味論。prefab parameter 表の編集(U2)。

## 5. 敵対レビューへの問い

(1) `get_components` の schema 経路は「追加できる component の全一覧」を出せるか(今は authored なものだけか)
(2) S-1 が単独で有用か —— `ray_instance` 以外に今日 Studio から足したい component は何か
(3) offline catalog を project 層から出すとき、behavior(DLL 由来の schema)はどう扱うか(WP361a の「Studio offline 乖離」と同件)
(4) `add_component` の既定値は誰が埋めるか(core の op が catalog 既定で埋めるのか、Studio が payload を送るのか)
