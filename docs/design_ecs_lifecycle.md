# ECS ライフサイクルと世代付き EntityId(v2)

対象読者: エンジン担当。
ステータス: **v2.1 — 確定(実装可)**(2026-07-10。v1 = Reject、v2 = 条件付き
Accept(`docs/design_reviews/2026-07-10_ecs_lifecycle_v2_review_codex.md`)、
**条件 C1〜C7 を本文へ反映済み**)。
前提: ECS 凍結解除、監査 Q2/Q3、v1/v2 レビュー。

## 0. 目的とスコープ

三重故障(未構築 / memcpy 詰め替え / 未破棄)、ストレージの alignment 未保証、
生成経路の不整合、外部モジュールの生ポインタ保持、シーン切替での ID ABA を
解消する。**リリースゲートは 1 つ**(§6 — v1 の段階マージ案はレビュー指摘により
廃止: R5a だけ先に入れても dangling は残り、R5b だけ先に入れても UB は残る)。

スコープ外: camera 二重所有の統合(独立 WP)、アーキタイプ再設計、並列化、
PhysWorld のスナップショット最適化(§4 — 意味論変更を伴うため別途判断)。

## 1. ライフサイクル規約(v2)

### 1-1. 型特性は 4 つに分離(v1 の単一 trivially_copyable 判定を廃止)

| 特性 | 用途 |
|------|------|
| `std::is_trivially_copyable_v<T>` | relocate の memcpy 高速路の可否 |
| `std::is_trivially_default_constructible_v<T>` | 構築省略の可否 |
| `std::is_trivially_destructible_v<T>` | 破棄省略の可否 |
| `has_deinit`(登録時に明示) | アプリ資源解放の要否 |

- **全コンポーネントは allocate 時に値構築**(`std::construct_at(ptr)` 相当を
  全 slot で実行 — range 化してコンパイラ最適化に委ねる)。
  **「trivially default constructible なら省略可」は削除(C1)** —
  trivial でも `T{}` はゼロ値を保証するが未初期化 storage は indeterminate。
  高速路が要る場合のみ engine-owned trait
  `value_init_is_all_bits_zero<T>` への**明示 opt-in** + 型ごとの同値テストで
  正当化する。correctness を性能予算で緩めない
- builder の populate は copy assignment なので、`addComponent(const T&)` は
  copy-assignable な型に compile-time 制約し、move-only 型には
  forwarding/emplace API を別に設ける(C1 補)
- 登録要件(static_assert): default constructible / **noexcept move
  constructible** / noexcept destructible。**copy fallback は削除**
  (deinit 資源を持つ型で source leak を作るため — レビュー 1.3)。
  `deinit` も noexcept 契約に含める(throw したら slot 半破壊になるため)
- **`SimpleModelViewUpdateComponent` の special member 修正は本ゲートに含む**
  (独自 copy 代入が暗黙 move を消し、上記 assert に即座に落ちるため —
  R5c 送り不可。レビュー 1.3)

### 1-2. 生成 transaction(3 経路の統一)

現行は builder(allocate→代入→init)/ serializer(allocate→serializer→init)/
transient glTF(allocate→init→代入)で **init の位置が既に不一致**(レビュー 1.1)。
v2 の規約:

```
construct all → populate all(代入 or serializer)→ init all → handle publish
```

- **構造変更の非再入(C2)**: allocate/remove/clear/relocate を共通の
  mutation guard で囲み、construct/populate/init/deinit/destroy コールバック
  中の構造 API 再入は**状態を変える前に決定的に拒否**する
  (シングルスレッドでもコールバック再入は起きる)。将来 nested 生成が
  必要になったら command queue へ遅延する形で拡張
- rollback(C2): **逆順**で行う — 完了した init を逆順 deinit、構築済みを
  逆順 destroy。復元対象は count だけでなく **ID/free-list の順序・
  component version・transaction が作った fresh chunk と各キャッシュ**を含む
  (fresh chunk は「空のまま残す」弱保証を採る場合、その決定性とメモリ上限を
  テストで固定)。失敗した生成が後続の ID 列を変えないこと
- `init` は「throw 時にアプリ資源を一つも残さない」strong guarantee を契約に
  (fault injection は資源カウンタ検査込み)
- **publish 前の ID は外部から観測不能**(現行の「commit 前に live list へ
  載せる」を廃止)
- `allocateRaw` の公開を廃止し transaction 内部へ隠す
- transient glTF 経路も同順序へ統一
- **batch API の再設計(C3)**: 「先頭 ID + 連続ポインタ」は free-list 再利用・
  複数 chunk と両立しないため廃止。単体生成(1 transaction → 1 ID)/
  bulk 生成(内部で chunk 分割、`span<EntityId>` を返し、populate は
  **chunk ごとのコールバック内でのみ有効な span** で渡す)/
  low-level chunk batch(`count <= remaining_capacity` を precondition とする
  private API)の 3 段に分離。4095/4096/4097・10 万件・free/fresh 混在・
  k 番目 fault を受け入れテストに

### 1-3. remove / clear / teardown

- remove(target ≠ 末尾): target を deinit → destroy、末尾を target へ
  relocate(trivially_copyable = memcpy、それ以外 = move_construct + 元を
  destroy のみ — moved-from を deinit しない)
- remove(target = 末尾): deinit → destroy(現行の self-memcpy を廃止)
- clear: 全 live slot を deinit → destroy
- **teardown フェーズの新設(レビュー 1.5 + C6)**: エンジン終了時、
  モジュール逆順破棄の**前**に「scene/ECS の明示 clear」フェーズを置く。
  **scope guard/runner の no-throw フェーズ**として実装し、正常終了だけでなく
  **初期シーンロード失敗・loop/update/render の例外時にも一度だけ**呼ばれる
  ことを保証(現 run() は try 内ローカルの container が catch 前に逆順破棄
  される構造 — 単に removeAll() を足すだけでは例外経路で走らない)。
  teardown 自体は throw せず、失敗は記録して残りの cleanup を継続。
  ECS のデストラクタから deinit を呼ぶ設計は**採らない**(GET_MODULE の
  再生成リスク)。例外注入で `deinit → destroy → モジュール破棄` の
  イベント列をテスト

### 1-4. 登録の一本化(保証の迂回口を塞ぐ)

型情報なしの `registerComponent(ComponentInfo)` 直接呼びを**廃止**し、全型を
typed 登録に通す(EntityId・benchmark の直接登録も移行 — レビュー 1.4)。
**typed 登録はシリアライザの有無から分離する(C5)**: 現行 typed registerer は
ISerializable 制約付きで EntityId 等を通せない — lifecycle/alignment
メタデータは常に型から生成し、JSON serializer コールバックは optional にする。

## 2. ストレージ(alignment — 未決 2 の解)

- `ComponentInfo` に `alignment = alignof(T)` を追加(typed 登録から供給)
- コンポーネント配列は **aligned allocation**(`operator new(bytes,
  std::align_val_t{alignment})` 等)+ 固定 capacity 内の手動 lifetime 管理へ
  変更。`vector<uint8_t>` は base alignment を保証しないため**廃止**
  (レビュー §2 — stride は `sizeof(T) % alignof(T) == 0` により切上げ不要、
  問題は base のみ)
- `CHUNK_CAPACITY` を API 契約として固定し、batch allocate の上限検査を追加
  (現行は fresh chunk への数万件 batch が未検査)
- 検証: `alignas(64)` + `std::string` を持つカナリアを最低 2 slot、
  全ポインタの alignment・remove 中間/末尾・clear を sanitizer 付きで

## 3. 世代付き EntityId(v2)

### 3-1. ID 表の再設計

```
id_table[index] = { optional<ref> chunk_ref, uint32 generation, bool live }
free_indices: LIFO スタック(再利用順も決定的)
```

- **canonical 定義は 1 箇所(entity.hpp)に置き、デフォルト構築 = invalid(C4)**:

  ```cpp
  struct EntityId {
      uint32_t index = UINT32_MAX;   // EntityId{} は必ず invalid
      uint32_t generation = 0;
  };
  using GameObjectId = EntityId;     // 同一型(別 struct にしない)
  ```

  値構築された `LocalTransformComponent::parent` が最初の entity を指す事故を
  型レベルで排除。`EntityId{} == invalidGameObjectId`・8 バイト・
  trivially copyable・比較/hash/`index:gen` 表示をテストで固定。
  playercontrol / gamesystem_test の `= 0` は移行リストに含める
  (v1 レビュー 3.3 + v2 レビュー §9 の一覧が WP の作業リスト)
- **generation の状態遷移を規範化(C4)**: remove/clear は ref 無効化 +
  `live=false`、`generation == UINT32_MAX` なら**永久 retire**、それ以外は
  **1 回だけ**進めて free-list(LIFO)へ。allocate は free slot の generation を
  **進めず**その値を handle に写す。`id_table.size() == UINT32_MAX` は明示的な
  capacity エラー(invalid index を採番しない)。clear の走査順・push 順は
  index 順に固定(決定性)
- **`LocalTransformComponent::parent` も世代付き ID へ**(生 index のままだと
  階層参照だけ ABA が残る — レビュー 3.2)
- `liveObjects()` の別 vector を廃止し id_table の live 状態へ統合
- **stale ポリシーの全経路表(C4 — 再レビューで妥当と判定された no-op+bool を
  三層に伝播)**:

  | API | dead/invalid ID の挙動 |
  |-----|------------------------|
  | remove(ECSCore → GameObjects → GameContext) | **`[[nodiscard]] bool`** — invalid/dead/世代不一致は false、live 削除のみ true(debug assert は入れない — debug/release で有効入力を変えない。厳格版はテスト用 removeOrThrow) |
  | tryComponent | null |
  | 必須 component アクセサ | 名前/ID 入り例外 |
  | set/markChanged | 失敗を返す |
  | PhysWorld | skip + 遅延 prune |
  | rpc | オブジェクト名入り application error |

### 3-2. clear と wrap(v1 の「構造的排除」を本物にする)

- **`clearEntities` は id_table を捨てない**: 全 live slot の generation を
  進めて free に積む。旧シーンの ID は新シーンで必ず resolve 失敗する
  (v1 はここで ABA が即復活していた — レビュー S0)
- **generation が最大値に達した slot は永久 retire**(新 index を採番)。
  wrap を「低確率」として許容しない(レビュー 3.4)。32/32 採用
- `resolve(id)` を全経路に通す: remove / component access / markChanged /
  localTransform / setLocalTransform / SceneLoader / PhysWorld / rpc /
  chunk 末尾移動時の表更新

## 4. 外部参照の ID 化 — 即時可視を維持(v1 のスナップショット案を撤回)

**PhysWorld は「書き換え → 即 raycast で新位置」という WP47 の既存契約を
維持する**(レビュー S1 — physworld_test が固定している観測可能な API)。

- **Binding の形(C6 — variant は transform 源のみ。name/collider は現行の
  重複検査・raycast 結果が依存するため維持)**:

  ```cpp
  struct Binding {
      std::string name;
      ColliderComponent collider;
      std::variant<GameObjectId, PhysWorldTransform> transform_source;
  };
  ```

- query ごとに resolve して transform 値を構築(現行と同じ即時性)。
  static 枝は値保持。死んだ ID は skip + 遅延 prune — **prune の変異点を
  規定**: `collectColliders() const` では変異せず、**bind 時に同名の dead
  binding を先に prune**(query 前の名前再利用が duplicate エラーにならない
  ように)。**性能が問題になったら**スナップショット/キャッシュ化を計測付きの
  別 WP として起こし、その時は意味論変更(次フレーム可視)としてユーザー
  判断を仰ぐ
- SceneLoader の `ObjectBinding` は ID 1 本にし、transform と simplemodelview の
  **両方**を使う瞬間に resolve(現行は 2 本の生ポインタ)。
  `hasObjectTransform` も map 所属だけでなく resolve する(dead は false/prune)
- rpc update_transforms は**parse 時と flush 時の両方で resolve**(pending の
  間に entity が消え得る — C6)。削除済みは名前入りエラー

## 5. 規約(contributor 規則へ昇格)

1. フレームを跨いでコンポーネントへの生ポインタ/参照を保持しない
   (保持は ID、使用時 resolve)。system query が受け取る配列ポインタも
   フレーム内限定
2. **イベント payload にコンポーネントへのポインタを入れない**
   (event 機構は relocate の対象外だが、この穴は規約で塞ぐ — レビュー §5)
3. `SceneUnloaded` イベントは**新設しない**(clear の所有者は teardown
   フェーズとシーン切替 — 必要になったら E 系で別途設計)

## 6. 実装順(v2 — 単一ゲート)

**R5-core(1 つの統合ゲート、内部コミットは分割可)**:
ストレージ alignment 化 + LifecycleOps + 生成 transaction + teardown フェーズ +
id_table/世代 + 全経路 resolve + PhysWorld/SceneLoader/rpc の ID 化 +
`SimpleModelViewUpdateComponent` special members 修正 + カナリア/シナリオテスト。

**R5-後続(ゲート通過後)**: modelview 統合(絆創膏 + 隠し互換コンポーネント
撤去)。camera 二重所有統合は**独立 WP**(本ゲートに依存しない — レビュー 6.1)。

## 7. 検証(golden は補助にすぎない — レビュー 6.2)

- カナリア型で**イベント列を assert**(`construct → init → deinit → destroy`、
  relocate 時の `source move → source destroy(deinit なし)`)— 回数だけでは
  順序の誤りを見逃す
- fault injection: populate / init が throw するケースの rollback 検証
  (**外部資源カウンタの検査込み** — init が資源獲得後に throw するケース)
- **C7 の追加ケース**: trivially-default-constructible な int メンバが
  populate なしで `T{}` の値になる / move-only 型の relocation と builder の
  compile-time 制約 / **コールバックからの構造 API 再入が状態変更前に
  拒否される** / batch の chunk 境界(4095/4096/4097・10 万件・free/fresh
  混在・k 番目 fault)/ `EntityId{}` と parent の invalid / MAX generation
  retire / index 枯渇 / stale 全分類と bool 伝播 / dead binding の
  skip・prune・同名 rebind / 正常終了と例外終了の teardown 順序
- ASan(MSVC /fsanitize=address)+ **別途 Clang/GCC 系の UBSan ジョブ**
  (MSVC の ASan は UBSan を含まない — alignment 検査は UBSan 側)で
  remove 中間/末尾・slot 再利用・clear・teardown
- Q2 再現シナリオ: A 削除 → A は hit せず B は同位置/同 ID・rpc の
  削除済み名エラー・シーン切替後の旧 ID resolve 失敗(ABA テスト)
- alignment カナリア(§2)
- 既存テスト・golden 全維持(POD 経路の挙動不変の補助証明)
- 決定性: free list LIFO 込みの 2 回実行一致

## 8. 決定事項(v2.1 で未決を解消)

1. **benchmark は削除せず typed/bounded batch へ書き直す(C7 — 再レビューの
   反対を受理)**: 数万件 workload は値構築・resolve の回帰を測る唯一の資産。
   ただし現行の数値は baseline にしない(capacity 超過 batch + assert なし)。
   Release・固定 seed・warm-up・median/p95、規模別(1/4096/4097/10 万)、
   POD/string/over-aligned/free-list 混在別、工程別(生成/relocate/
   resolve hit・miss/PhysWorld query)に計測し、**R5 実装前に有効な baseline を
   取って許容予算を WP に記録**する
2. clear の実装形 = **全 live の generation 進め**(world epoch は不採用 —
   32/32 + retire の証明が単純になる方を採る)
3. resolve は `id_table[index]` の O(1) 添字(hot path にハッシュを入れない)。
   POD 値構築は帯域コストが主 — range construction + chunk 分割を先に計測し、
   足りない場合のみ C1 の opt-in 高速路を追加
