# ECS ライフサイクルと世代付き EntityId(v1)

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-10。**codex 敵対的レビュー前**)。
前提: ECS 凍結解除(2026-07-08 ユーザー決定)、リファクタ監査 Q2/Q3
(`docs/design_reviews/2026-07-08_refactor_qa_codex.md` — 再現シナリオと
棚卸しの一次資料)、旧 `ecs-speedup` ブランチの世代付き ID の発想
(コードは不採用 — 監査判定)。

## 0. 目的とスコープ

三重故障(生成時に placement new なし / 詰め替えが memcpy / 破棄で
cb_deinit・デストラクタとも不呼出)と、外部モジュールの生ポインタ保持
(PhysWorld・SceneLoader・rpc — use-after-lifetime 実在)を解消する。

スコープ外: camera/modelview の二重所有統合(R5c として後続 —
本書の基盤の上で行う)、アーキタイプ再設計、並列化。

## 1. ライフサイクル規約(登録 = 保証)

登録表(ComponentInfo)に **LifecycleOps** を追加する:

| op | 内容 |
|----|------|
| default_construct | placement new(スロット確保時) |
| move_construct | 詰め替え時(move 不可な型は copy construct) |
| destroy | C++ デストラクタ |
| deinit | アプリ資源の解放(GPU インスタンス等 — 既存 cb_deinit の実体化) |
| is_trivial | `std::is_trivially_copyable_v<T>` で自動判定 — true なら memcpy 高速路 |

経路の規約:

1. **allocate**: ストレージ確保後、各スロットを default_construct
   (trivial は省略可 — 値は従来どおり後続の代入/serializer が与える)
2. **remove(target ≠ 末尾)**: target を deinit → destroy、末尾を target へ
   move_construct、**末尾は destroy のみ**(moved-from を二重 deinit しない)
3. **remove(target = 末尾)**: deinit → destroy のみ
4. **clear**: 全 live スロットを deinit → destroy してからストレージ解放
5. コンポーネント型の要件: **noexcept move constructible + noexcept
   destructible を static_assert**(例外安全を単純化 — 満たせない型は登録不可)
6. serializer 経路(JSON ロード)は「construct 済みスロットへの代入」と
   再定義(現状の「生バイトへ serializer → initComponent」の順序を是正)

**「登録できるが呼ばれない API」の根絶(cb_deinit の教訓)**: カナリア型
(construct/move/destroy/deinit 回数を数えるテスト用コンポーネント)で
全経路(allocate・remove 中間/末尾・clear・シーン切替)の呼出回数を
ユニットテストで固定する。登録 = テストで保証された契約。

## 2. 世代付き EntityId

- 形式: `{ uint32 index, uint32 generation }`(64bit)。スロット再利用時に
  generation をインクリメント。free list は **LIFO 固定**(再利用順も
  決定的 — リプレイ資産を守る)
- `resolve(id)`: index のスロットが同 generation なら有効、違えば
  「死んだ ID」として明確に失敗(nullopt)。**ABA 問題(削除→再利用→
  偶然同じ index)を構造的に排除**
- 公開面: `GameObjectId` を世代付きに置換(userpublic の handle)。
  ゲームコードは createObject の戻り値を保持して使うだけなので影響は小。
  比較・ハッシュ・ログ表示(index:gen)を提供
- generation 枯渇(2^32 回再利用)は実用上無視。wrap 時の誤判定確率も
  許容(文書化のみ)

## 3. 外部参照の ID 化(生ポインタ保持の禁止)

**規約: フレームを跨いでコンポーネントへの生ポインタを保持してはならない。
保持するのは EntityId、使う瞬間に resolve する**(contributor 規則へ昇格)。

- **PhysWorld**: `Binding` の `const TransformComponent *` を EntityId に置換。
  フレーム頭に 1 回スナップショット構築(生存確認 + transform 値コピー、
  O(M log M))、以後の raycast/overlap は O(M) — 監査 Q2 の計算量設計を採用。
  死んだ ID の binding は skip + 遅延 prune。「transform 更新の可視化は
  次フレーム」と固定(決定的)
- **SceneLoader**: `object_bindings` の `void*` を EntityId に置換
  (allocateRaw の戻り値を保持)。rpc update_transforms は毎回 resolve —
  削除済みオブジェクト名は名前入りエラー
- **ModelViewUpdate の絆創膏撤去**: §1 の本修正後、手動文字列コピーと
  隠し `simplemodelviewupdate` 互換生成を統合(simplemodelview へ一本化 —
  監査の「二つの component を統合する方が自然」判定を採用)。
  `SimpleModelViewComponent::deinit`(GPU インスタンス解放)が remove で
  確実に呼ばれることをカナリアと leak カウンタで検証

## 4. 実装順(WP 候補)

| 段階 | 内容 | 依存 |
|------|------|------|
| R5a | LifecycleOps + 全経路の規約実装 + カナリアテスト + noexcept static_assert。既存 POD は memcpy 高速路で挙動不変(golden 全維持) | なし |
| R5b | 世代付き EntityId + free list LIFO + PhysWorld/SceneLoader/rpc の ID 化 + Q2 の再現シナリオをテスト化(A 削除 → A は hit せず B は同位置/同 ID で hit・末尾削除・スロット再利用) | R5a |
| R5c | modelview 統合(絆創膏撤去)+ camera 二重所有統合(dummy component 廃止 → 実データ化 or Camera モジュール一本化 — 着手時に小設計) | R5a, R5b |

## 5. 検証

- カナリア型による呼出回数の固定(全経路)
- ASan/UBSan ビルドでの remove/reuse/clear シナリオ(MSVC /fsanitize=address)
- Q2 再現シナリオ一式(ghost collider・rpc の別オブジェクト破壊)を
  回帰テストとして常設
- 既存テスト・golden 全維持(POD 高速路の挙動不変証明)
- 決定性: ID 再利用順を含む 2 回実行一致

## 6. 未決事項

1. generation のビット幅(32/32 か 48/16 か)— メモリより単純さ優先で 32/32 案
2. アラインメント: 現チャンクはバイト列に詰めるだけ — 非 trivial 型解禁で
   alignas 要求をどう満たすか(スロットサイズの align 切上げで足りるか)
3. マルチチャンク/アーキタイプ横断の詰め替え経路が他にないか(実装時に監査)
4. clearEntities とシーン切替イベント(SceneUnloaded)の関係 — E 系と整合
