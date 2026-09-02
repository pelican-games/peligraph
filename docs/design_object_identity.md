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
