# 設計: 宣言の担体 —— 可変長の宣言と、authoring object から実行 row への join(v1→v4、2026-09-02)

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


## 6. 設計レビュー第 1 巡(12 件・破綻 4)と v2

**v2 が本文書の正。§2 決定 1〜6 と §3 段割りは v2 で置換される。**

### 決定 1'(#1): **永続形式に session-local ID を書かない**

`AuthoringObjectId` は同一プロセス内の再 publish で採番が進む。row は **所有 object の内側**に置き、
`authoring_object_id` は永続形式から除いて **所有 object から実行時に注入**する。object 外から row を参照する形は作らない
(それには永続 object identity が要り、WP304 の領域)。

### 決定 2'(#2): occurrence key は importer が生成する **opaque な `occurrence_id`**

`{authoring_object_id, node_index, importer_variant}` は **同一 node の 2 primitive を区別できず**、asset 差し替えにも追随しない。
- importer が **`occurrence_id`** を生成する: **所有 model asset identity + 安定 primitive identity + semantic variant tag**
- source format に永続 primitive identity が無い場合(glTF)は **決定的な content fingerprint**(頂点数・index 数・material slot・
  bounds の量子化)で代替し、**衝突は `AmbiguousOccurrence` で名前付き拒否**(黙って先勝ちにしない)
- 受け入れ: **1 node × 2 primitive**を別 mask にして GPU 出力が変わる。DCC 再出力で配列順だけ変えた model で追随。
  **model を差し替えた場合は旧 key が `absent` で落ちる**(= 循環条件ではなく本番 importer の生成結果で検査)

### 決定 3'(#3・#4): schema は **再帰代数**。wire と fingerprint も同時に

- `Schema = Leaf | List{ element: Struct(Leaf…), key: leaf 名, limits{max_rows} }`。**list の list は無し**(v1 のまま)
- policy ごとに明示: **Behavior / Component は許可、Event は consteval 拒否**(今の「Bool/Enum でない」条件では
  `vector<Row>` が意図せず通る)
- **versioned な再帰 wire 型**を schema-leaf / project 層が所有し、RPC も同じ wire を使う
- **fingerprint は element の field 群・順序・presence/default・key 規則・limits まで含める**(同一 version で element だけ変えた
  DLL reload を検出する)

### 決定 4'(#5): ABI は**上げる**。登録は ABI 検査の後

`BehaviorRegistration` は公開 C++ 境界で `vector<StructFieldSchema>` を運ぶ。再帰 metadata を足せば layout が変わり、
**旧 DLL の static initializer が ABI 検査より前に旧 layout で登録を呼ぶ**(crash / UB / 部分登録)。
- **ABI-neutral な C descriptor**(`struct_size` + `version`)を導入し、**ABI 検査に通ってから明示 entrypoint で登録**する
- game-logic ABI を **v2 に上げる**。旧 DLL は**名前付きで拒否**し、旧 live DLL と登録状態を保つ(部分登録を残さない)

### 決定 5'(#6): **editor の row identity と semantic occurrence id を分ける**

- **`row_id`**: document transaction 内で**自動生成**される hidden な永続 ID(利用者は書かない。添字でもない)
- **`occurrence_id`**(決定 2'): renderer join 用。row の leaf として利用者が選ぶ(inventory から)
- RPC は **`{list_path, row_id, leaf_path}`** で指定し、server が **CAS の下で現在の添字に解決**する。
  `[A,B]` の B を編集した後に先頭へ X を挿入しても、同じ `row_id` は B を指す
- 既存 `set_component_value` は**変えない**が、list の leaf は `row_id` 経由でしか指せない(添字 path は名前付き拒否)

### 決定 6'(#7): inverse は **journal-only の完全復元**

- remove の inverse は **raw row・authored presence・前後の stable anchor(`row_id`)**を journal に保存する
  (既存 component inverse が full component と exact index を保存するのと同じ形:
  [editorjournal.cpp:1179](../src/core/communication/editorjournal.cpp#L1179))
- **insert は full row か patch を受ける**。required leaf を持つ schema で key だけの insert は **`MissingRequiredLeaf`** で拒否
- 受け入れ: **中央 row の remove → undo** で値・位置・presence が byte 一致

### 決定 7'(#8): inventory は **eligibility** を返す

`occurrence_inventory` は existence ではなく **`absent | ambiguous | present_but_not_ray_traced | eligible`** を返す
(skinned / morph / VAT / BLAS-ineligible は `present_but_not_ray_traced`)。
override が効かない宣言は**名前付きで落ちる**(fail-fast)。validation は **GPU buffer 書き込みの前**に置く
(model rebuild の途中失敗で旧新混在を作らない)。

### 決定 8'(#9): Studio へは **core の RPC 2 本**で供給する

- `component_schema_catalog`(登録済み全 component の再帰 schema。**未追加のものも**)と `occurrence_inventory`
- **wire 型は project / schema 層が所有**、**データは core の RPC**。Studio は RPC / project だけ link(D0)
- game DLL 由来 behavior の schema も catalog 経由(WP361a の offline 乖離はここで閉じる)

### 決定 9'(#10): **`ray_instance` を今 `pelican_ray_instance` に改名する**

未出荷の今が最安。version gate は「外部 `ray_instance` を持つ project が v2 を有効化すると builtin 登録で衝突する」
逆向きの問題を残す。`pelican_` 予約は **`ComponentInfo.owner` で owner-aware** に実施し、既存外部 `pelican_*` は
移行手順と名前付きエラーを定義する。**WP374 の後追い修正として起票**(RT3-1 の完了記録に負債として追記)。

### 決定 10'(#11): **最初の出荷単位は C-1 + C-2 + C-3 を束ね、消費者は RT3 の per-geometry override**

C-1 単独の消費者は立証できない。**既に仕様化済みの消費者**(RT3 設計 §11 の「1 object × 2 primitive を別設定」)を
最初の消費者として名指しし、**C-1〜C-3 を 1 つの出荷単位**にする。list 無し / 有りを**同じ本番経路**(RPC → save → reload → GPU)で比較。

### 決定 11'(#12): 受け入れ matrix とエラー契約

- C-1: **同一 version で element だけ変えた DLL reload が検出される** / 旧 DLL の名前付き拒否 / Event policy の consteval 拒否
- C-2: 編集後に先頭挿入 → 同じ `row_id` が同じ row / 中央 row の exact undo / 未指定 vs 明示 default の区別 / commit 途中失敗で旧状態保持
- C-3: **本番 importer が生成した `occurrence_id` の安定性**(再 import で同値)/ 同一テスト内 off・on / 実際に解決した occurrence と GPU 値
- 名前付きエラー: `DuplicateRowKey{key, first_index, second_index}` / `MissingRowKey` / `ListTooLarge{max}` /
  `MissingRequiredLeaf` / `AmbiguousOccurrence` / `OccurrenceNotRayTraced`。`StructFieldErrorCode` に追加(既存 6 値を保つ)
- 失敗時は旧状態を保持(atomic)。全段で出荷 4 project の bytes 不変

### 決定 12': 段割り(v2)

```
CARRIER-1 = C-1 + C-2 + C-3 + 決定 9'(改名) を 1 出荷単位、消費者 = RT3 の per-geometry override
CARRIER-2 = Studio 汎用 UI(design_studio_component_ui。core RPC 2 本の上)
```
CARRIER-1 は大きい。**仕様レビューで分割案が出たら、C-1+C-2(消費者: 名指しの behavior list)と C-3 に割る**ことを許す。


## 7. 設計レビュー第 2 巡(9 件・破綻 3)と v3(§8 の v4 で置換)

**v3 が本文書の正。§6 の決定 1'〜12' は v3 の修正を受ける。残余は各 WP の仕様レビューで受ける。**

### 修正 1(#1): `occurrence_id` の **byte 仕様と版**を定める。粗い fingerprint を identity にしない

v2 の式は **node occurrence identity を含まず**(同一 mesh/primitive を 2 node が参照すると同値)、世代間 alias も検出できない。
```
occurrence_id v1 = H( canonical_logical_asset_identity   // 宣言名でも URI でもなく、project の asset 表の canonical key
                     , stable_node_occurrence_identity    // glTF: node index + parent path。無ければ順序付き content digest
                     , stable_primitive_identity          // primitive index(+ extras の id があればそれ)
                     , semantic_variant_tag
                     , normalized_full_content_digest )   // source identity が無い場合のみ。頂点配置・topology まで含む
```
- 粗い fingerprint(頂点数・bounds)は**候補絞りにのみ**使い、identity にしない
- 衝突エラーは**同じ ID になった両方**の node path / mesh / primitive / material / variant を載せる
- **model 差し替え後の旧 key は `OccurrenceAbsent`**(修正 8)。世代間 alias は full-content digest で潰す

### 修正 2(#2): **RT3 row schema を先に仕様化**する。`Flags<Vocabulary>` を scalar leaf に足す

「RT3 は仕様化済み」は誤り。RT3 v4 は固定 leaf の component + material 規則であり、**row list の意味論は未定義**だった。
- 代数に **`Flags<EnumVocabulary>`** を scalar leaf として追加(flag 集合を list にしない → list の list を回避)
- RT3 row: `{ occurrence_id(required), mask?: U8, instance_flags?: Flags<InstanceFlag>, custom_index?: U24 }`
  (`geometry_flags` は **row で上書きしない** —— geometry 粒度は material 規則のまま)
- **field ごとの優先順位**: `defaults < material rule(geometry_flags のみ) < object-level ray_instance < occurrence row`。
  row で未指定の field は **object-level を継承**(schema default に戻さない)
- **flags の合成則は「置換」**(和集合・差分にしない。宣言した集合がそのまま実効値)
- authored absent と明示 default の runtime 意味は**同じ**(presence は編集 / undo のためだけに保持)

### 修正 3(#3): 旧 DLL は **ロード前**に弾く。SDK v2 は static initializer を廃止する

C descriptor が DLL 内にある限り、`LoadLibrary` 時点で旧 static initializer が先に走る。
1. **ABI / version を実行せずに読める場所**(sidecar `*.abi.json` か PE/ELF section)に置き、**ロード前に検証**
2. **SDK v2 は host state を変える static initializer を全廃**(Behavior だけでなく system / event / component も)し、
   **単一の explicit registration entrypoint** に集約
3. 候補登録は**一時 registry** に行い、全検証成功時だけ live registry と**原子的に交換**
4. 旧 DLL の検査を隔離する必要があれば helper process
これは **GLABI-2**(修正 9)として独立 WP にする。

### 修正 4(#4): `row_id` は **versioned な row envelope** に書く。raw と typed を分離する

scene save は `raw_document.dump()` そのもの。hidden ID は「row に書く(bytes が変わる)」か「side table(再起動で失う)」の二択だった。
- **row envelope**: `{ "$row": { "id": …, "v": 1 }, …authored fields… }`。metadata と authored value を分離
- **raw authored tree と typed materialized value を別に保持**(今の Behavior 編集は materialize → 全 field 再 encode で
  authored presence を失う: [editorjournal.cpp:232](../src/core/communication/editorjournal.cpp#L232))
- `row_id` は **document 内の決定的 allocator**(時計・乱数を使わない)。**list insert 時にのみ書く**。load / save だけでは migration しない
- duplicate / invalid ID は名前付き拒否。copy / paste は再採番。insert 応答で生成 ID を返す
- 出荷 4 project に list row は無いので bytes 不変は達成可能(ただし効能の対照にはならない)

### 修正 5(#5): offline は **build 時の schema manifest**。RPC では閉じない

`component_schema_catalog` は登録済み runtime にしか無い。offline Studio(player / core / game DLL 非起動)には届かない。
- **build 時に ABI / version / fingerprint 付き schema manifest を生成**し、**project closure に保存**
- online catalog と manifest の fingerprint が違えば **drift を名前付きで表示**(黙って片方を使わない)
- Studio 設計の S-2 はこの manifest を消費者とする(それが無ければ S-2 は起票しない、は据え置き)

### 修正 6(#6): 改名は **migration matrix**。自動移行は「builtin と証明できる」場合だけ

scene JSON の component 名に owner が無く、builtin の旧 `ray_instance` と外部 `ray_instance` を区別できない。
| 判定 | 動作 |
|---|---|
| project / layout version から **builtin と証明できる**旧名 | `pelican_ray_instance` へ移行(save 時 canonical) |
| 外部 owner が登録している旧名 | そのまま |
| 判定不能 | **`RayInstanceMigrationAmbiguous@1`**(load を止める) |
- RPC / catalog / error payload は **alias 期間**中は旧名を受理し、save は canonical name
- 旧名の hardcode(`predefined.cpp:35` / `componentcodec.cpp:1069` / loader / registry)を**全部**列挙して移す
- `pelican_*` を既に使う外部 component のエラーには owner / module を含める
これは **RAY-NAMESPACE**(修正 9)として独立 WP。**CARRIER-1 へ無条件に含めない**(v2 決定 9' を訂正)

### 修正 7(#7): **ID-B(scene transaction)を hard prerequisite** にする

「GPU 書き込み前検証」だけでは model publish / AS rebuild の失敗で旧新混在が残る。候補 document・catalog・occurrence map・
AS plan を **side state で構築し、全成功後に generation 単位で 1 回だけ publish**。これは identity 設計 v3 の ID-B そのもの。

### 修正 8(#8): エラー契約に **absent と運用診断**を足す

- **`OccurrenceResolutionErrorCode`**(resolver 専用): `OccurrenceAbsent{row_id, occurrence_id, last_known_provenance}` /
  `AmbiguousOccurrence{occurrence_id, candidates[]}` / `OccurrenceNotRayTraced{reason}`
- inventory entry に **display name / asset / node path / mesh・primitive / material / variant / eligibility reason**
- 受け入れ: off → on だけでなく、**row 削除後の on → off**、**active mapping が現存 row と完全一致**を同じテストで

### 修正 9(#9): **5 つに分ける**。CARRIER-1 は 1 WP ではない

| WP | 内容 | 単独価値 |
|---|---|---|
| **GLABI-2** | pre-load ABI negotiation、explicit transactional registration、旧 DLL の名前付き拒否 | **あり**(今日は旧 DLL が UB で落ちる) |
| **RAY-NAMESPACE** | `pelican_ray_instance` migration matrix、owner-aware reservation | **あり**(WP374 後追い負債) |
| **CARRIER-SCHEMA-TX** | 再帰 schema / wire / fingerprint、row envelope、presence、`row_id` 編集(insert / remove / CAS) | **実在する production Behavior consumer を名指しできない限り単独出荷しない** |
| **CARRIER-RT3** | occurrence identity / inventory、RT3 row schema、field 別 resolution、model reload / AS / GPU 受け入れ | SCHEMA-TX と**同じ enablement gate**(review / 実装 / rollback は分ける) |
| **CARRIER-STUDIO** | online catalog UI + offline manifest(design_studio_component_ui) | RT3 の後 |

順序: **GLABI-2 → RAY-NAMESPACE → (ID-B) → CARRIER-SCHEMA-TX + CARRIER-RT3 → CARRIER-STUDIO**。
**最初に起票するのは GLABI-2**(前提が無く、既存欠陥を直す)。

## 8. 設計レビュー第 3 巡(11 件・破綻 4)と v4 —— **v4 が最終。設計段はここで打ち切り、残余は GLABI-2 の仕様レビューへ**

**v4 が本文書の正。§7 の修正 1〜9 は v4 の修正 α〜θ を受ける。** 第 3 巡の結論は「GLABI-2 を最初の carrier WP に置く判断は妥当。
ただし起票に必要な契約が未確定」であり、本節はその**起票ゲート 6 条件**を規範として確定する。

### 修正 α(#1): `stable_primitive_identity` は **tagged union**。index は identity にしない

- `PrimitiveIdentity = ExplicitId{ extras.id 文字列 } | OrderedContentDigest{ 正規化 accessor の digest }`。
  **primitive index は診断にだけ載せ、H の入力にしない**(DCC 再出力で配列順が変わる)
- 同一 node 内で digest が一致する sibling は **`AmbiguousOccurrence`**(両 primitive の index・material・bounds を載せる)。
  利用者は `extras.id` か geometry で区別する。**黙って先勝ちにしない**(修正 1 を維持)

### 修正 β(#2): `occurrence_id v1` の **byte 仕様**と、asset key の強い型

```
occurrence_id_v1 = hex32( SHA-256( "pelican.occurrence.v1" ‖ F(asset) ‖ F(node) ‖ F(prim) ‖ F(variant) ‖ F(content) )[0:16] )
F(x) = u32le(len(bytes(x))) ‖ bytes(x)          // 全 component を length-prefix。空でも u32le(0) を書く
```
- 文字列は **NFC 正規化 UTF-8**。整数は **u32le / u64le**。float は **f32 bit pattern**、`-0.0 → +0.0`、**NaN / Inf は `OccurrenceIdUnrepresentable` で名前付き拒否**
- accessor 正規化: 量子化・normalized 型は **f32 へ dequantize**、index は **u32**、attribute は **semantic 名の byte 順で並べ**、
  未使用 attribute は含めない。sparse accessor は展開する
- `node` = glTF node index + 親 path(index 列)。`asset` = **`CanonicalModelAssetKey`(強い型)**。project の asset 表が
  採番し、**prepare / inspect / commit を同じ型で通す**(文字列で持ち回らない。宣言名・URI からの再構成は禁止)
- version は prefix 文字列に埋める。式を変えるときは `v2` を足し、v1 の再計算は**しない**(旧 row は `OccurrenceAbsent` で落ちるのが契約)

### 修正 γ(#3・#5・#6・起票ゲート 1〜6): GLABI-2 の **C ABI と transaction を完全固定**する

**起票ゲート 1: embedded section を規範**にする。sidecar は**廃止**(DLL と拘束できず、stale sidecar が preflight を通す)。
- PE: section `.pelabi` / ELF: note `PELICAN_ABI`。**raw file reader で読み、`LoadLibrary` / `dlopen` の前に検証**する
- 内容(すべて little-endian・固定 layout):
  `magic "PELABI\0\0"(8) ‖ major u16 ‖ minor u16 ‖ struct_size u32 ‖ capabilities u64 ‖ build_id[16] ‖ entry_symbol char[64]`
- `build_id` は SDK の generated header が **build ごとに**埋める 128 bit(時計は使わず、link 入力の digest)

**起票ゲート 2: 単一 C entrypoint を固定**する。
```c
typedef uint32_t pelican_gl_status;            /* 0 ok / 1 host_incompatible / 2 registration_rejected / 3 internal */
PELICAN_GL_EXPORT pelican_gl_status
pelican_game_logic_register_v2(const pelican_gl_host_api *host,        /* host が所有。呼出中のみ有効 */
                               pelican_gl_registration_sink *sink);    /* host が所有。呼出中のみ有効 */
```
- `extern "C"`、x64 の既定呼出規約、**`noexcept` 相当(例外は境界を越えない。越えたら `internal`)**
- `pelican_gl_host_api` / `pelican_gl_registration_sink` は先頭に `struct_size u32 ‖ version u32`。関数 pointer 表。**STL / `type_info` を跨がない**
- descriptor は **`sink->register_descriptor(kind u32, version u32, const uint8_t *bytes, size_t len)`** の
  **versioned length-delimited wire** で渡す。host が copy する。DLL 側 pointer の寿命は呼出中のみ
- 旧 `pelican_game_logic_abi_version` は **v2 では export しない**(section が代替)。v1 の symbol だけ持つ DLL は section 欠落で落ちる

**起票ゲート 3: extension contract**。`kind` は host の登録表で解決し、**未知 kind は `UnknownDescriptorKind{kind, version}` で拒否**
(skip しない。fail-fast)。kind の追加は **minor** を上げ、既存 kind の wire 変更は **新 kind**(major は据え置き)。
**`kind = schema_v1`(再帰 schema wire)は今ここで番号だけ予約**し、wire 本体は CARRIER-RT3 が定義する。
これで「flat 向け ABI v2 を固定すると SCHEMA-TX で v3 が要る」問題は消える(major を上げずに kind を足す)。

**起票ゲート 4: transaction 対象表**。`releaseGameLogicRegistrations`
([gamelogicreload.cpp:43](../src/core/gamelogic/gamelogicreload.cpp#L43))の **全 owner kind** を行にする:

| owner kind | candidate | commit | rollback |
|---|---|---|---|
| Behavior / GameSystem / Event / Component 登録 | candidate owner に登録 | owner を live に昇格 | owner ごと release |
| Animation owner | 同上 | 同上 | 同上 |
| render policy / pass / subgraph / graph transform / strategy provider | candidate owner に登録(**activate しない**) | `activateProviderOwner` | release |
| vulkan native scope provider | 同上 | 同上 | 同上 |
| physics provider(`PELICAN_WITH_PHYSICS`) | 同上 | 同上 | 同上 |
| registration owner 自体 | `allocateRegistrationOwner` | 旧 owner を release | candidate owner を release |

- 登録の検証は **canonical key 順**(登録順に依存しない)。重複・不正は名前付きで candidate 全体を落とす
- **`initialize()` は `shutdown()` の前に preflight**(section 検証 + candidate load)を置く
  ([gamelogicreload.cpp:209](../src/core/gamelogic/gamelogicreload.cpp#L209) は今 active を先に捨てる)。
  reload 経路([gamelogicreload.cpp:253](../src/core/gamelogic/gamelogicreload.cpp#L253))は既に validate → unload なので、そこに揃える

**起票ゲート 5: matrix**(initial load / reload / 再 initialize)×(missing section / stale v1 / forged v2 / valid v2):
- `GameLogicAbiMissing`(section 無し。**v1 DLL はここ**)/ `GameLogicAbiMalformed`(magic・struct_size 不整合)/
  `GameLogicAbiUnsupported{found_major, required_major}` / `GameLogicEntryMissing`(section は v2 だが entry 無し = forged)/
  `GameLogicArtifactMismatch`(修正 ζ の manifest 用)。**5 つは別々の名前付きエラー**
- どの行でも **旧 active handle・generation・全 registry の件数と key 集合が不変**。missing / stale / forged は **`LoadLibrary` を呼ばない**

**起票ゲート 6: 受け入れ**。「**v1 SDK で作った実 DLL(WP162 の fixture 系列)で、static constructor が host の sentinel を変更するもの**」
を候補にし、initial / reload / 再 initialize の 3 経路で **sentinel 不変・旧 active handle / generation / 全 registry 一致**を
production `GameLogicReloader` 経由で検査する。対照: 現行 loader は `LoadLibrary` で constructor を走らせるので sentinel が変わる。
**section 検証の呼出を消したら落ちる**。

### 修正 δ(#4・#7): row envelope は **container decoder が剥がす**。raw は上書きしない

- **`$` prefix を schema field 名から予約**(builder の `consteval` / `static_assert` と、runtime `ReservedFieldName` の両方)
- list container decoder が `$row` を剥がし、**user struct decoder には authored field だけを渡す**。encoder は envelope を先頭に戻す
- **raw authored tree は一切上書きしない**。`ResolvedComponent::source_json_exact` の Behavior 上書き
  ([resolvedscene.cpp:179](../src/core/loader/resolvedscene.cpp#L179))と journal の全再 encode
  ([editorjournal.cpp:1057](../src/core/communication/editorjournal.cpp#L1057))を、**raw patch → typed validation → raw / typed atomic publish** に置き換える
- `id`: **u32**、JSON number、scope = `{object, component, list_path}`。allocator = scope 内 `max + 1`(決定的)。
  overflow は `RowIdExhausted`。未知 `v` は `RowEnvelopeUnsupported`
- **envelope の無い手書き行は `RowEnvelopeMissing` で拒否**(暗黙 migration しない。row の唯一の writer は insert op)
- prefab: body は instance ごとに複製される([resolvedscene.cpp:822](../src/core/loader/resolvedscene.cpp#L822))が、
  scope が instance の component なので id は**そのまま**複製してよい(衝突しない)
- 受け入れ: **無関係な scalar `/params/speed` の編集後も、全 row envelope・省略 field・row 順序が byte 一致**

### 修正 ε(#8): `last_known_provenance` は **非権威の `provenance_hint`** に置き換え、**absent は正式契約**

- 選択時に envelope へ `"$row": { "id", "v", "hint": { asset, node_path, primitive } }` を**非権威**で保存する(join には使わない)
- `OccurrenceAbsent{row_id, occurrence_id, provenance_hint: optional}`。**`null` は正式**(再起動後に逆算できないため)

### 修正 ζ(#9): offline manifest は **exact artifact に拘束**

- manifest は `{ dll_sha256, build_id, target_triple, configuration, feature_vector, abi{major,minor}, fingerprint, descriptors[] }`
- **同じ registration entrypoint を headless host で走らせて生成**する(手書き一覧を許さない)
- offline Studio は project closure が指す exact artifact と照合し、不一致は **`SchemaManifestUnbound`** で UI を止める

### 修正 η(#10): ID-B は **identity 設計 v4 の executor**(§9)を前提にする

- v3 参照(修正 7)を **v4 §9 修正 B の scene 専用 executor** に差し替える
- CARRIER-RT3 が後から登録する **participant seam** を v4 の participant 表に足す:
  catalog / occurrence map / AS plan は **`prepare` で失敗可**、publish / rollback / retire は **`noexcept`**
- 受け入れ: carrier participant を **1 つずつ** bypass / drop した変異が production test で落ちる

### 修正 θ(#11): **4 つに分ける**。SCHEMA-TX と RT3 は 1 つの縦切り

「単独出荷しない」と本文が認める WP は WP ではない。**CARRIER-SCHEMA-TX + CARRIER-RT3 を 1 つの vertical WP(CARRIER-RT3)**に統合し、
内部 milestone(schema / envelope / row 編集 → occurrence / inventory / GPU)だけ分ける。消費者は RT3 の per-occurrence override。

| WP | 内容 | 単独価値 |
|---|---|---|
| **GLABI-2** | 修正 γ 全部 | あり(今日は v1 DLL の constructor が host state を変えてから拒否される) |
| **RAY-NAMESPACE** | rename + owner-aware policy(**migration / alias 無し**。修正 6 の matrix は WP379 v2 で撤回) | あり(WP379 起票済み) |
| **CARRIER-RT3** | 修正 α β δ ε η + RT3 row schema(修正 2)+ inventory(修正 7')。前提: GLABI-2 / RAY-NAMESPACE / ID-B(WP380) | あり(1 object × 2 primitive を別設定) |
| **CARRIER-STUDIO** | 修正 ζ + design_studio_component_ui | RT3 の後 |

順序: **GLABI-2 ∥ RAY-NAMESPACE(並列可。RAY-NAMESPACE は migration を持たないので GLABI-2 に依存しない)→ ID-B → CARRIER-RT3 → CARRIER-STUDIO**。

### §8 補遺(2026-09-02、WP381 仕様レビュー第 1 巡の結果。修正 γ の起票ゲート 1〜6 を**撤回**)

WP381 の仕様レビュー(13 件)が、修正 γ の機構そのものを壊した: embedded section と単一 C entrypoint は **enforcement ではない**
(新 entrypoint と旧 static registrar を同居させれば preflight を通って旧 constructor が走る)。完全に固定するには host / DLL 境界の全部を
C ABI にする必要があり、GLABI-2 の目的と釣り合わない。

**置き換え**: ABI は **DLL が import する host symbol の版付き名前**で表し、enforcement は OS loader(import 解決は static initializer より前)。
layout を変える入口は版を上げて旧名を消す。ロード前の raw import table 読みは**診断**(名前付きエラー)であって強制ではない。
section / `build_id` / C wire / `schema_v1` 予約 / candidate staging は作らない。CARRIER-RT3 の再帰 schema は `pelican_register_behavior_v3` になる。
offline manifest(修正 ζ)は **final artifact の SHA-256** に拘束する(`build_id` は無い)。

規範は WP381 v2(台帳)。設計段の 3 巡打ち切りは維持する —— これは設計を再開したのではなく、仕様レビューが設計の 1 決定を覆したので理由と共に記録したもの。
