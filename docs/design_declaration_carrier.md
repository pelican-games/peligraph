# 設計: 宣言の担体 —— 可変長の宣言と、authoring object から実行 row への join(v1、2026-09-02)

## 0. なぜ今か

RT3 設計(design_raytracing §11、3 巡)で確定した事実: **per-instance の宣言を運ぶ担体がエンジンに無い**。
RT3-1 は「固定 leaf の component + material 鍵の layout 規則」で担体を回避したが、
**per-geometry × per-instance の override** は先送りになった。同じ担体を **RT4(callable の per-in-flight record)・
RT5(変形 geometry)・RT7(ノード編集)・R5(typed 構築 factory)** が要求する。これは 1 WP ではなくプログラムである。

## 1. 実測(2026-09-02。書く前に数えたもの。RT3 の 3 巡レビューで検算済み)

| 事実 | 場所 |
|---|---|
| `StructFieldType` は `I8…U64 / F32 F64 / Vec2 Vec3 Vec4 Quat / String Bool Enum` の**固定 leaf のみ。可変長は無い** | [structfieldschema.hpp:21](../src/core/userpublic/details/schema/structfieldschema.hpp#L21) |
| `set_component_value` は既存 JSON pointer を `.at()` で要求し、**欠落 field も行も作れない** | [editorjournal.cpp:1057](../src/core/communication/editorjournal.cpp#L1057) |
| Studio の widget は固定 schema field だけ。**component を追加・削除する経路が無い**(`inspectorwidget.cpp:519` の `addComponent` は widget 配置 helper) | [inspectormodel.cpp:558](../src/devstudio/model/inspectormodel.cpp#L558) |
| core には `add_component` / `remove_component` の op が**ある** | [editorjournal.cpp:1135](../src/core/communication/editorjournal.cpp#L1135) |
| runtime kind は **4 種**(`Ecs / Camera / Light / Collider`) | [componentcodec.hpp:19](../src/core/loader/componentcodec.hpp#L19) |
| **1 object が 2 primitive を持てば 2 row**(`q` は instance×geometry)。scene object と TLAS instance は 1:1 でない | [polygoninstancecontainer.cpp:1061](../src/core/renderer/polygoninstancecontainer.cpp#L1061) |
| `{asset, mesh, primitive}` は row に**単射でない**(1 mesh を 2 node が参照、auto-split の variant、DCC 再出力での順序変化)。実 row は `node_index` まで持つ | [gltf.cpp:2261](../src/core/model/gltf.cpp#L2261) / [polygoninstancecontainer.cpp:1169](../src/core/renderer/polygoninstancecontainer.cpp#L1169) |
| `DrawItemSnapshot` に authoring object ID が無く、model asset reload は snapshot を再生成する(WP374 は authoring ID の運搬を足した) | [polygoninstancecontainer.cpp:903](../src/core/renderer/polygoninstancecontainer.cpp#L903) |
| BindableDescriptor は project 側の**手書き一覧**から始まり、codec は enrich するだけ。offline Studio も手書き一覧を使う | [prefabprovider.cpp:217](../src/core/loader/prefabprovider.cpp#L217) / [project.cpp:109](../src/devstudio/model/project.cpp#L109) |
| 外部 ECS との**登録名衝突**は拒否される(予約 namespace 無し) | [componentinfo.cpp:20](../src/core/ecs/componentinfo.cpp#L20) |

## 2. 決定

### 決定 1: 担体は **3 つの独立した能力**に分ける。どれも単独で価値がある

| 能力 | 今日無いもの | 単独で何が良くなるか |
|---|---|---|
| **C-1 可変長宣言** | schema に collection 型が無い | 「行の並び」を宣言できる(override list・callable の record list・deform の weight list) |
| **C-2 行を作れる編集** | `set_component_value` が `.at()` | 欠落 field と行の追加・削除が RPC で出来る(Studio はその上) |
| **C-3 authoring → row の join** | row に authoring ID + occurrence key が無い(WP374 が ID だけ足した) | per-geometry × per-instance の override が row に届く |

順序は **C-1 → C-2 → C-3**(C-3 は C-1 の宣言を運ぶので最後)。

### 決定 2(C-1): collection は **「同型の行の配列」1 種類だけ**。ネストしない

- `StructFieldType::List`(要素は**既存の固定 leaf の struct**)。**list の list は作らない**(必要になった実例が無い)
- 各行は **stable な行 key** を持つ(利用者が書く文字列。省略時は名前付き拒否。**添字を identity にしない** ——
  添字は編集で動く)
- decode / encode / schema fingerprint / provider descriptor を通す(`objectRef` と同じ配線。WP372c 裁定 5)
- 受け入れ: 実 `PELICAN_REGISTER_BEHAVIOR` の DLL から list を宣言し、scene JSON に書き、resolve と reload で往復する。
  同一 key の重複行は名前付き拒否

### 決定 3(C-2): 編集は **path ベースの 4 op**。「値の置換」と「構造の変更」を分ける

| op | 意味 | 既存との関係 |
|---|---|---|
| `set_component_value` | **既存** pointer の値置換(現行どおり `.at()`) | 変えない |
| `insert_component_field` | 欠落 field を既定で作る / list に行を追加(行 key 必須) | 新設 |
| `remove_component_field` | field を既定へ戻す(= 削除)/ list から行を削除 | 新設 |
| `add_component` / `remove_component` | 既存 | 変えない |

- inverse は既存 journal の形(forward / inverse、CAS、structural domain)に乗せる。**新しい journal 機構を作らない**
- 受け入れ: 実 RPC bytes で行を追加 → 値を置換 → 削除 → undo/redo で完全復元。**`.at()` の既存 op が行を作れない**
  ことは維持(既存の否定対照を壊さない)

### 決定 4(C-3): join の key は **`{authoring_object_id, node_index, importer_variant}`**

- `{asset, mesh, primitive}` は単射でないので使わない(RT3 第 2 巡 #3)
- row に **occurrence key** を運ぶ(WP374 が足した authoring ID の隣)。model asset reload を跨いで保存
- override の宣言側(scene の list 行)は **同じ key を書く**。key が現在の model に無ければ**名前付き拒否**
  (黙って defaults に落ちない)
- 受け入れ: **1 object × 2 primitive**を別 mask にして GPU 出力が変わる(RT3 設計 決定 5 の受け入れをここで満たす)。
  DCC 再出力で primitive の順序だけ変えた model に対し、**key が安定なら追随、消えた key は名前付きで落ちる**

### 決定 5: 予約 namespace

builtin の component 名は **`pelican_` 接頭辞を予約**する(`ray_instance` は WP374 で version gate により回避したが、
次からは接頭辞)。既存の外部 ECS が `pelican_*` を使っていれば登録時に名前付き拒否。
`ray_instance` の改名は**しない**(gate が既にあり、出荷物に無い。改名は別の互換問題を作る)。

### 決定 6: Studio(#9)は **C-2 の上に汎用 UI** として別設計

C-2 が RPC を持てば、Studio の「component 追加 / 行の追加・削除」は **schema catalog からの汎用 UI**。
D0(Studio は core を link しない)は catalog が project 層から来ることで保たれる。本文書の範囲外。

## 3. 段割り

```
C-1(可変長宣言)→ C-2(行を作れる編集)→ C-3(join と override)→ Studio 汎用 UI
                                      ↘ RT4 / RT5 / RT7 はここから
```

## 4. やらないこと

list のネスト / 添字 identity / 永続 ID(WP304)/ Studio の実装 / RT4 以降の本体。

## 5. 敵対レビューへの問い

(1) `StructFieldType::List` が既存の constexpr schema(`std::apply` の 3 本 fold、`materializeStructField`)に載るか。載らないなら何を変えるか
(2) 行 key を利用者が書く設計は Studio の UX として成立するか(自動採番との比較)
(3) `insert_component_field` の inverse が既存 CAS の structural domain で表現できるか
(4) occurrence key `{authoring_object_id, node_index, importer_variant}` が **本当に単射**か(instancing・LOD・skinned の variant)
(5) C-1 が単独で有用か(RT が来なくても誰が list を使うか)
