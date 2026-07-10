# ECS ライフサイクルと世代付き EntityId(v2)

対象読者: エンジン担当。
ステータス: v2 ドラフト(2026-07-10。**v1 は codex レビューで Reject —
`docs/design_reviews/2026-07-10_ecs_lifecycle_review_codex.md` の S0×2 + S1 を
全面受理して改稿。再レビュー待ち**)。
前提: ECS 凍結解除、監査 Q2/Q3、v1 レビュー(以下「レビュー」)。

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

- **全コンポーネントは allocate 時に値構築(`T{}` 相当)** — 現行の
  `vector<uint8_t>::resize` によるゼロ埋めに意味論が最も近い既定
  (レビュー 1.2)。trivial default constructible なら実装は memset/省略に
  最適化してよいが、**契約は常に「値構築済み」**
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

- 途中失敗時の rollback: init 済みのみ deinit、construct 済みのみ destroy、
  chunk count と ID slot を巻き戻す。**publish 前の ID は外部から観測不能**
  (現行の「commit 前に live list へ載せる」を廃止)
- `allocateRaw` の公開を廃止し transaction 内部へ隠す(公開を残す場合は
  コンポーネント毎の app-init bit が必要になり複雑化するため、隠す方を採る)
- transient glTF 経路も同順序へ統一

### 1-3. remove / clear / teardown

- remove(target ≠ 末尾): target を deinit → destroy、末尾を target へ
  relocate(trivially_copyable = memcpy、それ以外 = move_construct + 元を
  destroy のみ — moved-from を deinit しない)
- remove(target = 末尾): deinit → destroy(現行の self-memcpy を廃止)
- clear: 全 live slot を deinit → destroy
- **teardown フェーズの新設(レビュー 1.5)**: エンジン終了時、loop 終了後・
  モジュール逆順破棄の**前**に「scene/ECS の明示 clear」フェーズを置く。
  deinit が依存するモジュール(renderer 等)の生存を保証する。
  ECS のデストラクタから deinit を呼ぶ設計は**採らない**(GET_MODULE の
  再生成リスク)

### 1-4. 登録の一本化(保証の迂回口を塞ぐ)

型情報なしの `registerComponent(ComponentInfo)` 直接呼びを**廃止**し、全型を
typed 登録に通す(EntityId・benchmark の直接登録も移行 — レビュー 1.4)。
どうしても残す場合は「POD 専用」を型で明示し lifecycle/alignment メタデータの
完全指定を要求する API にするが、**第一案は廃止**。

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

- **`GameObjectId` と内部 `EntityId` を同一の `{uint32 index, uint32 generation}`
  value type に統一**(二重定義の解消 — レビュー 3.2 の前者案)。比較・
  ハッシュ・`index:gen` 表示を提供
- 無効値 = `index == UINT32_MAX` の定数 `invalidGameObjectId`(既存の
  `0` sentinel との衝突を回避 — playercontrol / gamesystem_test の `= 0` は
  移行リストに含める。レビュー 3.3 のコンパイル割れ箇所一覧を WP の
  作業リストにする)
- **`LocalTransformComponent::parent` も世代付き ID へ**(生 index のままだと
  階層参照だけ ABA が残る — レビュー 3.2)
- `liveObjects()` の別 vector を廃止し id_table の live 状態へ統合。
  stale handle の remove は **no-op + bool 戻り値**で規範化(二重 remove を
  エラーにしない — 決定性と使い勝手のバランス)

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

- `Binding` は `variant<GameObjectId, PhysWorldTransform(static)>` を保持
  (collider-only オブジェクトは static — entity を持たない binding が
  現存するため optional では不足。レビュー 3.3)
- query ごとに resolve して transform 値を構築(現行と同じ即時性)。
  死んだ ID は skip + 遅延 prune。**性能が問題になったら**スナップショット/
  キャッシュ化を計測付きの別 WP として起こし、その時は意味論変更
  (次フレーム可視)としてユーザー判断を仰ぐ
- SceneLoader の `ObjectBinding` は ID 1 本にし、transform と simplemodelview の
  **両方**を使う瞬間に resolve(現行は 2 本の生ポインタ)
- rpc update_transforms は毎回 resolve — 削除済みは名前入りエラー

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
- ASan/UBSan(MSVC /fsanitize=address)で remove 中間/末尾・slot 再利用・
  clear・teardown
- Q2 再現シナリオ: A 削除 → A は hit せず B は同位置/同 ID・rpc の
  削除済み名エラー・シーン切替後の旧 ID resolve 失敗(ABA テスト)
- alignment カナリア(§2)
- 既存テスト・golden 全維持(POD 経路の挙動不変の補助証明)
- 決定性: free list LIFO 込みの 2 回実行一致

## 8. 未決事項

1. benchmark(ecs/benchmark.cpp)の扱い — typed 登録へ移行か、R5 で削除して
   計測は別途書き直すか(推奨: 削除。生 API の最後の利用者を残さない)
2. clear の実装形: 全 live の gen 進め(推奨)か world epoch 混入か
3. R5-core の性能予算(値構築の追加コスト・resolve のハッシュ/添字コスト)—
   ゲートに入れる回帰閾値
