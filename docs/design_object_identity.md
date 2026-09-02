# 設計: object identity —— 名前を実行時の identity キーから降ろす(v1、2026-09-02)

## 0. なぜ今か

WP372c(rename)を中止した裁定が根拠である。**object 名は少なくとも 6 subsystem の identity キー**であり、
rename は編集操作ではなく **rekey** だった。同じ事実は E2(destroy)・prefab の参照追随・将来の
全構造編集に響く。決定性と「宣言データとして開く」(user-space 境界の教訓)に反する。

本文書は **rename を復活させるための設計ではない**。**名前が identity である状態を終える**設計である。
rename はその帰結として(別 WP で)自然に書けるようになる。

## 1. 実測(2026-09-02。書く前に数えたもの)

| 事実 | 場所 |
|---|---|
| 実行時は **既に安定 id を持っている**: `SceneRuntimeObjectBinding{authoring_object_id, object_id}` | [scene.hpp:27](../src/core/loader/scene.hpp#L27) |
| `SceneLoader::object_bindings` は **名前 → `{GameObjectId}`** の map。**名前なのは key だけ**、値は id | [scene.hpp:39-43](../src/core/loader/scene.hpp#L39) |
| 名前で object を引く**公開 API は 3 つ**: `objectId` / `hasObjectTransform` / `objectTransform` | [scene.hpp:72-74](../src/core/loader/scene.hpp#L72) |
| その外部呼び出し側は **3 ファイル**: camera controller / rpcserver / animation | 実測(grep) |
| `src/core/light` / `phys` / `animation` は **`AuthoringObjectId` を一切知らない**(0 files) | 実測 |
| Light は名前 key の map を **6 つ**持つ(`directional_names` / `point_names` / `spot_names` + `m_*NameMap` ×3) | [lightcontainer.hpp:32](../src/core/light/lightcontainer.hpp#L32)・:116-122 |
| `Camera::scene_cameras` は名前 key | [camera.hpp:59](../src/core/renderer/camera.hpp#L59) |
| Physics の collider identity と query hit の公開名が object 名 | [physworld.cpp:285](../src/core/phys/physworld.cpp#L285) |
| Animation の `objects` cache は名前 key で、**古い cache を先に返す** | [animationservice.cpp:432](../src/core/animation/animationservice.cpp#L432) |
| Controller は camera 名を map key、**target 名を signature に含める** | [cameracontrollersystem.cpp:346](../src/core/userpublic/cameracontrollersystem.cpp#L346) |
| `object_bindings` は RPC `load_gltf` の **transient と同じ map を共有** | [rpcserver.cpp:1439](../src/core/communication/rpcserver.cpp#L1439) |
| WP304(永続の安定 ID)は不合格。理由は**再採番・snapshot import・改名で変わる ID** | 台帳 |

**結論を先に**: 当初見積もり(WP372c 裁定)より**小さい**。新しい ID を作る必要は無い。
**各 subsystem の map の key を、既にある `AuthoringObjectId` に据え替える**問題である。

## 2. 決定

### 決定 1: **新しい ID を作らない。`AuthoringObjectId` を実行時 identity にする**

- WP304 が不合格になった理由(永続 ID の再採番・import・改名)は、**authoring 側の ID の永続性**の話だった。
  本設計は**実行時**の identity の話であり、`AuthoringObjectId` は **scene 文書の load 1 回の中で安定**であれば足りる
- 既に `SceneRuntimeObjectBinding` がそれを運んでいる。**足すのは配線であって型ではない**

### 決定 2: 生の名前解決 API を **非公開**にし、**opaque な参照型 1 つ**に集約する

WP372c 第 2 巡 #5 の「網羅は実装不能」は、`SceneLoader::objectId(std::string_view)` が公開だからだった。

- `ObjectRef`(= `AuthoringObjectId` の薄い型。名前を持たない)を **唯一の解決入口**にする
- `SceneLoader::objectId / hasObjectTransform / objectTransform` の **`std::string_view` 版を削除**し、
  `ObjectRef` 版に置換する。**名前 → `ObjectRef` の変換は load 時と RPC 境界の 2 箇所だけ**
  (`resolveObjectName(scene, name) → ObjectRef` を **1 関数**に閉じる)
- 外部呼び出し 3 ファイル(camera controller / rpcserver / animation)を移行する。**3 ファイルなので網羅は grep ではなく
  コンパイルで保証される**(`std::string_view` 版が無ければ呼べない)

### 決定 3: subsystem の map は **`AuthoringObjectId` を key** にする。名前は表示用の値へ降ろす

| subsystem | 現状 | 後 |
|---|---|---|
| `SceneLoader::object_bindings` | `map<string, {GameObjectId}>` | `map<AuthoringObjectId, {GameObjectId, display_name}>` |
| Light の 6 map | `map<string, uint32>` | `map<AuthoringObjectId, uint32>`。3 種の `*_names` と `m_*NameMap` の**重複も同時に解消**(6 → 3) |
| `Camera::scene_cameras` | `map<string, SceneCamera>` | `map<AuthoringObjectId, SceneCamera>` |
| Physics collider identity / query hit の公開名 | object 名 | `ObjectRef`(+ 表示名は別 field) |
| Animation `objects` cache | 名前 key、stale 優先 | `AuthoringObjectId` key。**stale cache を先に返す挙動は別欠陥として同時に直す** |
| Controller signature | target **名** | target の `ObjectRef` |

### 決定 4: `load_gltf` の transient は **別の名前空間**へ分離する

`object_bindings` を authored と transient が共有しているのは rename 以前の欠陥
(transient `New` が authored の name reservation をすり抜ける)。**transient は `TransientObjectId` で別 map** に置く。

### 決定 5: 段割り

| 段 | 内容 | 単独で何が良くなるか |
|---|---|---|
| **ID-1** | `ObjectRef` + 名前解決の 1 関数化 + 3 外部呼び出しの移行 + `string_view` 版 API の削除 | 「名前引きが公開」が終わる。**新しい解決者はコンパイルで弾かれる** |
| **ID-2** | `SceneLoader` / Light / Camera の map を `AuthoringObjectId` key へ。transient 分離 | 6 map の重複が消え、rename が「3 map の key 更新」になる |
| **ID-3** | Physics / Animation / Controller の identity を `ObjectRef` へ。Animation の stale cache 修正 | rename が全 subsystem で閉じる。**rename WP はこの後**(文書 + 宣言済み参照 + 3 map、adapter 1 つ) |

## 3. 受け入れの骨(各段の WP へ)

- **ID-1**: `SceneLoader` に `std::string_view` を受ける解決 API が**存在しない**(存在すればコンパイルで落ちる形の対照:
  テストが `objectId("Chair")` を呼んで**コンパイルできないこと**を CMake の negative-compile で示す)。
  RPC から名前で object を指す既存経路(`set_component_value` の `object` 等)が**変わらず動く**
- **ID-2**: 同名 object を含む scene(scene A/B が同じ `Chair`)で Light / Camera が**混同しない**
  (今日は名前 key なので混同する = 対照)。`load_gltf` transient `New` が authored `New` と**衝突を名前付きで拒否**
- **ID-3**: Animation の cache が **reload 後に古い object を返さない**(今日は返す = 対照)。
  Controller の target が rename 相当の key 更新で追随する(rename WP の受け入れの前借り)
- 全段: 出荷 4 project の bytes 不変、golden 不変、全数を壊さない

## 4. やらないこと

永続の安定 ID(WP304 の領域)/ rename の RPC(ID-3 の後の別 WP)/ prefab の参照 index(担体プログラム)/
Studio の変更。

## 5. 敵対レビューへの問い

(1) `AuthoringObjectId` は本当に load 1 回の中で安定か(snapshot import・prefab 展開・spawn で再採番されないか)
(2) `string_view` 版 API の削除で、利用者コード(game DLL)が壊れないか —— 公開 API なら移行期間が要る
(3) Light の 6 map が本当に重複か。3 種の `*_names` と `m_*NameMap` の役割の違い
(4) Physics の query hit が名前を返す契約は利用者向け API か
(5) ID-1 が単独で有用か(rename 無しで何が良くなるか)


## 6. 設計レビュー第 1 巡(8 件・破綻 4)と v2

**v2 が本文書の正。§2 決定 1〜5・§3 受け入れは v2 で置換される。§1 の実測は次の訂正を受ける。**

訂正(第 1 巡が壊した実測):
- 名前で object を引く公開 API は 3 つではなく **4 つ**(`applyObjectTransform(std::string_view, …)` が漏れていた:
  [scene.hpp:72](../src/core/loader/scene.hpp#L72))
- Light の 6 map は**重複ではない**。staged な `PreparedLoad` の 3 種と live な `m_*NameMap` の 3 種で、
  snapshot / publish / rollback の旧・新 state([lightcontainer.cpp:294](../src/core/light/lightcontainer.cpp#L294))。
  「6 → 3 に減る」は撤回
- `SceneLoader` の `string_view` API は配布 DLL の主公開面ではないが、**`GameContext` の Light / Physics / Camera の
  名前 API、Animation V1 ABI の `object_name`、Physics の `ColliderIdentity` の名前は配布対象**
  ([gamecontext.hpp:22](../src/core/userpublic/gamecontext.hpp#L22)、[abi_v1.hpp:299](../src/core/userpublic/animation/abi_v1.hpp#L299)、
  [physquery.hpp:52](../src/core/phys/physquery.hpp#L52))
- **全 authored object が `GameObjectId` を持つわけではない**(light-only object は ECS 化前に別経路:
  [scene.cpp:211](../src/core/loader/scene.cpp#L211))

### 決定 1'(#1): `ObjectRef` は **authority つき**。文書ローカル ID の薄いラッパーにしない

scene A と B の先頭 object はどちらも `AuthoringObjectId{1}` になり得る。ID の値だけでは
「同じ ID の別 authority」と「同一 object」を区別できず、誤参照の軸が名前から整数へ移るだけだった。

```
ObjectRef { authority: AuthoringAuthorityId(= load 世代。runtime_scene_epoch で可), id: AuthoringObjectId }
```
- snapshot import・別 scene load 後は旧 authority の ref を **`stale_authority`** で名前付き拒否
- 受け入れ: A の ref を B の load 後に使うと `stale_authority`(今日は同じ整数で別物を引く = 対照)

### 決定 2'(#3): 参照は **2 種類**。authored と transient を型で分け、両方を受ける所だけ tagged union

transient glTF(`load_gltf`)、free camera、static collider、Animation の runtime 登録は **authoring ID を持たない**。
- `AuthoredObjectRef`(決定 1')と `TransientObjectRef`(runtime 採番 + 同じ authority)
- 中央 registry の値は **`optional<GameObjectId>` + capability 集合**(`transform / light / camera / physics / …`)。
  light-only object を格納できる
- 両方を扱う API だけ `AnyObjectRef = variant<Authored, Transient>`

### 決定 3'(#2): **順序を逆転する**。consumer と ABI を先に、`string` API の削除は最後

ID-1「`string_view` API を削除」は単独では成立しない —— controller は毎フレーム `std::string target` を解決し、
Animation V1 も任意時点で `object_name` を渡す。削除すればビルドが壊れるか、consumer から `resolveObjectName` を
呼ばせて「load / RPC 限定」を破るかの二択だった。

| 段 | 内容(改) | 単独の価値 |
|---|---|---|
| **ID-0** | `ObjectRef`(authority つき)と registry(capability・`optional<GameObjectId>`)の**追加**。名前 API は残す | registry が存在し、名前 → ref の変換が **1 関数**になる(まだ呼び出し側は名前でよい) |
| **ID-1** | **scene load を 1 transaction 化**(決定 6')。registry / Light / Camera / Physics / Animation・controller state を prepare → publish、失敗で旧 snapshot へ復帰 | **既存欠陥の修正**(第 1 巡 #7): 今日は publish 途中の失敗で世代が混在する |
| **ID-2** | consumer を ref へ: controller の target、Animation の sink、projection adapter の `old_by_name`、camera の active / controlled order / controller state、Physics の identity | 名前 join が**全部**消える(map だけでなく selection / cache / rollback 保持まで) |
| **ID-3** | 公開面: `GameContext` に ref 版を追加、名前版は**互換 adapter として 1 バージョン残す**。Animation は **V2 descriptor**(V1 の layout 不変)。Physics hit は既存型不変 + 別型 | 旧 DLL を壊さない移行 |
| **ID-4** | 内部 `string` API 4 つの削除(negative compile は**全公開 operation の name overload 不在**を同一テストで) | 新しい解決者がコンパイルで弾かれる |

### 決定 4'(#4): 棚卸しは map ではなく **join・selection・state cache・rollback 保持**

第 1 巡が挙げた残り: `active_camera_name` / `controlled_scene_camera_order` / controller state の名前 key
([camera.hpp:59](../src/core/renderer/camera.hpp#L59))、projection adapter の `old_by_name`
([editorprojectionadapters.cpp:397](../src/core/loader/editorprojectionadapters.cpp#L397)。rename すると旧 collider の identity 継承に失敗し新規扱い)。
ID-2 の対象表はこれらを**全部**含める。projection adapter には **authored owner ref を渡す**。

### 決定 5'(#5): 公開 DLL 面の互換

- `ObjectRef` の公開型は **userpublic の中立 header**に置く(loader の内部 header ではない)。Studio 向け wire DTO は project / RPC 層
- ABI v1 は上げない。旧ヘッダで作った client と(可能なら)prebuilt DLL、export-symbol manifest で互換試験
- 出荷 example(`playercontrol.cpp:31`)の名前呼び出しは互換 adapter で動き続ける

### 決定 6'(#7): **scene load の transactional publish**(既存欠陥)

loader は runtime を clear した直後に Light / Camera を publish し、その後 behavior を activate する。
`onInit` が投げると catch は `clearRuntimeScene()` するが Light / Camera の旧 prepared state を戻さない
([scene.cpp:369](../src/core/loader/scene.cpp#L369)・:546・:584)。snapshot import も document は戻すが runtime を reload しない
([basicconfig.cpp:734](../src/core/loader/basicconfig.cpp#L734))。**document は A、Light / Camera は B、ECS は途中**という世代混在が今日成立する。

ID-1 として、registry・Light・Camera・Physics・Animation / controller state を **1 つの scene-runtime transaction**で
prepare / publish し、publish 後の例外では**全 state を旧 prepared snapshot へ戻す**。
台帳の「single production executor seam」「rollback primitive」(WP372a の scene reconciler)と同じ形を loader に置く。
受け入れ: B の behavior `onInit` を投げさせ、**Light / Camera / ECS / document が全部 A のまま**(今日は混在 = 対照)

### 決定 7'(#8): 結果型と capability

`ObjectRef` の解決は名前付き結果: `stale_authority / inactive_scene / wrong_domain(authored↔transient) /
not_materialized / missing_capability / name_ambiguous`。エラーは operation・ref の authority・要求 capability を持つ。
削除は **registry の全 index を purge**(fan-out を規定)。

### 決定 8'(#6): 受け入れの差し替え

| v1 の条件 | 問題 | v2 |
|---|---|---|
| `objectId("Chair")` の negative compile | 4 つ目の API と resolver 合成を検出しない | **全公開 operation の name overload 不在**を同一 compile test で |
| `set_component_value` が従来どおり | 通常の対象は数値 `object_id`。何を検査するか不明 | 削除 |
| A/B 同名 Chair が「今日は混ざる」 | 今日は load ごとに runtime を clear し prepared state を swap するので**混ざらない**(誤対照) | 決定 1' の `stale_authority` 対照へ |
| authored / transient `New` の衝突 | transform 付き authored は今日も拒否 | **light-only authored `New`** と transient `New` の衝突(今日は素通り)を対照に |
| controller の「rename 相当 key 更新」 | 本物の rename を通らない代理条件 | **production の rename command**(ID-4 の後の rename WP)で ID 不変・target 継続 |

### 決定 9': rename WP は **ID-4 の後**。本文書の範囲外のまま

## 7. v2 の受け入れの骨(各段)

- **ID-0**: 名前 → ref の変換関数が 1 つ。registry が light-only object を capability つきで持つ
- **ID-1**: `onInit` 例外で全 state が旧 A に戻る(混在 = 対照)。snapshot import の失敗でも同じ
- **ID-2**: rename 相当の key 更新なしに、`active_camera_name` 等の**名前文字列が code から消える**
  (= 名前 join が無い証明は「名前を変えても選択が追随する」ではなく、「名前が identity として読まれる箇所が 0」を
  ref 化した consumer の変異(ref を名前に戻す)が失敗することで示す)
- **ID-3**: 旧ヘッダの client が link し動く。V1 layout の static_assert
- **ID-4**: negative compile(全 name overload)。`stale_authority` / `wrong_domain` / `missing_capability` の各名前付き結果
- 全段: 出荷 4 project の bytes 不変、golden 不変、全数を壊さない、D0(型の配置を明記)


## 8. 設計レビュー第 2 巡(11 件・破綻 4)と v3 —— **v3 が最終。設計段は 3 巡で打ち切る**

**v3 が本文書の正。§6 の決定 1'〜9' は v3 の修正を受ける。残余は最初の WP(ID-1 = scene transaction)の仕様レビューで受ける。**

### 修正 1(#1): authority は **`next_authority` と `active_authority` を分ける**

`runtime_scene_epoch` は成功末尾でしか進まないので、失敗した candidate B の authority が次の C に再利用される(ABA)。
- **candidate authority は prepare 前に 1 回だけ消費し、失敗しても再利用しない**(`next_authority` 単調増加)
- B の callback は **transaction-local registry / context** で B を参照する(active A を参照させない)
- authority は **process 単位の run id を含める**(epoch は process ごとに 0 から始まる)。Studio の wire ref は
  **接続 session 内でのみ有効**と宣言し、再起動を跨ぐ ref は `stale_authority`

### 修正 2(#2): undo / restore は **同じ論理 object の復活**。epoch を進めない

journal は restore 時に元の `AuthoringObjectId` を保存し、`restoreObject` はそれを再利用する。
- 削除は **terminal stale ではない**(v1 に object incarnation は持ち込まない)
- 受け入れ: `{authority, id}` を保持 → destroy → `not_materialized` → undo → **同じ ref が再び有効**
- 削除時の「全 index purge」は registry の値(capability・`GameObjectId`)を落とすことであり、ref の恒久失効ではない

### 修正 3(#3): domain は **ECS entity の有無ではなく由来**で決める

resolved scene 由来の collider-only object は `EcsObjectLoad` に `AuthoringObjectId` を持つ。
- **`AuthoredObjectRef`** = resolved scene 由来(collider-only / light-only を含む)
- **`TransientObjectRef`** = RPC(`load_gltf`)/ runtime 注入(free camera、Animation runtime 登録、runtime collider)由来のみ

### 修正 4(#4): canonical resolver は **`(authority, name, expected_domain, required_capability)`**。衝突表を置く

| 同名の組 | 規則 |
|---|---|
| authored × authored(同 authority) | load 時に**名前付き拒否**(今日も transform 付きは拒否。light-only / collider-only も含める) |
| authored × transient | transient の load を**名前付き拒否**(authored が先勝ち。`name_reserved_by_authored`) |
| transient × transient | 後の load を拒否 |
| generic resolve(domain 未指定)で複数候補 | `name_ambiguous` |

- **free camera は名前で再同定しない**。engine-owned の stable key(`TransientObjectRef` を Camera projection の
  re-prepare を跨いで保持)
- **互換 adapter が第三境界にならない条件**(全部満たす): raw name を canonical resolver に **1 回だけ**渡す /
  operation 固有 map・cache-first・fallback・lazy 登録を**持たない** / 解決後は name を捨て ref API だけを呼ぶ /
  resolver の named error を**変換せず返す** / 互換期間を **release と ABI version で区切る**
- **Animation V1 の cache-first + lazy 登録(`animationservice.cpp:918`)はこの条件に反する。ID-3 で除去**

### 修正 5(#5): **scene hierarchy の parent join** を inventory に加える

loader は `unordered_map<string, GameObjectId>` で parent を解決する([scene.cpp:381](../src/core/loader/scene.cpp#L381))。
- source schema の `parent` は名前のままでよいが、**`ResolvedSceneResolver` で 1 回だけ `AuthoringObjectId` に正規化**し、
  runtime DTO は `parent_authoring_id` を持つ。loader は名前 map を作らない

### 修正 6(#6): registry は **WP375 の projection に参加**する

- **`ObjectRegistryProjectionAdapter`** を WP375 の route matrix に追加(capability を変える add / remove、transform の
  materialization、spawn / destroy / remove / restore、prefab route を old → new で反映)
- 受け入れ: `makeEditorRuntimeService → RPC → edit` で、add 前 `missing_capability` / add 後に**実際に変わった Light 値** /
  remove 後の拒否を**同一テスト**で

### 修正 7(#7): scene-load の `onInit` は **transaction-aware な制限 context** で走る

`BehaviorContext` は `GameContext` を継承し、RNG・audio・scene request・event・persistence を持つ。B の `onInit` が
`ctx.emit` / `ctx.random()` / `ctx.loadScene("C")` / `ctx.saveData` をしてから throw すると transaction 外へ漏れる。
- **event / scene request / RNG は checkpoint または buffer**(commit で flush、rollback で破棄)
- **audio / persistence / 外部 I/O は commit 後まで遅延**
- participant に **Behavior arena・event queue・RNG・pending scene・audio/persistence** を加える
- 受け入れ: Light / Camera / ECS / document に加え、**behavior handles・pending event 数・RNG の次値・pending scene・
  renderer instance・保存データ非生成**まで照合

### 修正 8(#8): `PreparedSceneRuntime` の participant token と順序を固定する

- Light / Camera: **`old = snapshot`、`next = prepare`、`publishPrepared` を 1 回**(既存 adapter の雛形:
  [editorprojectionadapters.cpp:1024](../src/core/loader/editorprojectionadapters.cpp#L1024))。外側から `load()` /
  `loadSceneCameras()` を**呼ばない**(内部で即 publish する)
- ECS: entity mutation token は non-reentrant なので、**新しい bulk candidate world** を作るか、
  **WP375 と同じ inverse-token の early publication + observer gate** を採る(v3 は後者。WP375 と形を揃える)
- Physics: `PreparedState` に **`active_trigger_pairs` の snapshot** を加える
- 順序: registry → ECS → Physics → Light → Camera → Behavior(activate は最後。失敗で全部 rollback)

### 修正 9(#9): 段割りを **5 段**に組み直す(各段が常時 build し単独価値を持つ)

| 段 | 内容 | 単独の価値 |
|---|---|---|
| **ID-A** | 共通型を `src/core/userpublic/object_ref.hpp` へ移設、registry、**WP375 participant**、**全既存 name entry の canonical adapter 化**(GameContext / Animation V1 / Physics) | 名前解決が 1 関数を通る。第三境界が消える |
| **ID-B** | **scene transaction**(修正 7・8) | **既存欠陥の修正**(世代混在) |
| **ID-C** | internal consumers / maps を ref 化(hierarchy 含む)。legacy adapter は**この段で既に ref API を呼ぶ** | 名前 join が消える |
| **ID-D** | public ref API + **Animation V2 ABI** を出荷し、**互換 release を 1 回出す** | 旧 DLL を壊さない移行 |
| **ID-E** | 次の明示的 breaking release で name adapter と内部 string API を除去 | 新しい解決者がコンパイルで弾かれる |

**v2 の ID-0〜ID-4 は撤回**(ID-0 に単独価値が無く、ID-2 が ID-3 を先取りし、ID-3 と ID-4 が矛盾していた)。

### 修正 10(#10): 型配置と Animation V2

- `AuthoringObjectId` を loader-private header から **`src/core/userpublic/object_ref.hpp`** へ移す(D0: Studio は
  `pelican_project` の wire DTO だけを見る)
- Animation は **service tail の `resolve_sink_v2` + `struct_size / capability`** 方式(旧 client は tail を読まない)

### 修正 11(#11): 受け入れの書き直し

- negative compile は **overload ごとに独立**(`static_assert(!requires { loader.objectId(name); })` を 4 本)
- 各 named error(`stale_authority` / `missing_capability` / `name_ambiguous` / `wrong_domain`)は**同じ production test 内に成功側**
  (current ref で実際に解決された Light / transform / collider 値)を持つ。恒常エラー実装を排除
- ID-C の主張は **「型と inventory の移行完了」**に縮め、identity 非依存の最終証明は **production の rename test**(ID-E の後の rename WP)に置く
- ID-B の変異 = **transaction bypass を compilable に**(旧 `load()` 経路へ戻す)、production composition で全 participant の実値差を検査

### 最初の WP

**ID-B(scene transaction)を最初に起票する** —— 既存欠陥を直し、ID-A の registry が無くても価値がある(第 2 巡が確認)。
ID-A は WP375 の着地後(participant を足す先が要る)。
