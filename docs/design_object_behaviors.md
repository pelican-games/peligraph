# オブジェクトへの振る舞い紐付け(behavior)(v2)

対象読者: エンジン担当・ゲームロジックを書く人。
ステータス: v2 ドラフト(2026-07-18)。v1 は敵対レビュー
`docs/design_reviews/2026-07-17_editor_behavior_review_codex.md` §2(以下
「レビュー」)で **Reject** — ①同名 component の反復は ECS が duplicate
拒否(B-1)②params 反射の既存機構は不在・EventPayloadSchema は
bool 拒否/全 required(B-2)③EventView は現行 E1 に存在しない(B-3)
④onInit のタイミングが entity publish/親配線と矛盾(B-4)⑤宣言順 =
EntityId 発行順は free-list 再利用で崩壊(B-5)⑥G2 の候補検証ロードで
onInit すると dangling(B-6)⑦「component に置けば reload 後も残る」は
WP90 全再構築と矛盾 — **虚偽につき削除**(B-7)。
v2 = レビュー §4.2 の逐語条件と再分割を採用した全面改稿。
前提: v1 と同じ + `design_editor_tooling.md` v2(ED-AUTH0/CODEC0)。

## 0. 正本の確定(v2 の中核決定)

**behavior の正本 = engine-owned attachment arena(special binder で
lower)。ECS component ではない。** light/collider と同じ special
dispatch の仲間であり、scene JSON の見た目(component 風)は維持する。

## 1. 宣言と登録

### 1-1. C++ 側

```cpp
class PlayerControl final : public Pelican::Behavior {
  public:
    struct Params {
        float speed = 4.0f;
        bool  can_jump = true;   // BEH-P0 で bool/default 対応
    };
    void onInit(Pelican::BehaviorContext &ctx) override;
    void onUpdate(Pelican::BehaviorContext &ctx) override;
    void onEvent(const DoorOpened &event,
                 Pelican::BehaviorContext &ctx);   // typed dispatch(§3)
    void onDestroy(Pelican::BehaviorContext &ctx) noexcept override;
};
PELICAN_REGISTER_BEHAVIOR(PlayerControl, "player_control", 1);
//                        C++ 型         永続名          schema_version
```

- **永続名と C++ 綴りを分離**(B-1 後段 — refactor で scene を壊さない。
  UI widget registry の先例に一致)
- **登録 = 宣言のみ**(factory/destroy/typed-event thunk/params schema/
  RegistrationOwner の記録)。static init・G2 候補検証中に instance
  生成・onInit を**一切しない**(B-6 — BEH0 の必須安全条件)

### 1-2. シーン側(見た目は v1 のまま・lowering が変わる)

```jsonc
{ "name": "behavior", "type": "player_control", "params": { "speed": 6.0 } }
```

- SceneLoader の **special binder** が反復 `behavior` record を
  declaration order のまま収集し、attachment arena へ lower(BEH0
  改訂条件逐語 — ECS の duplicate 拒否を回避)。behavior-only object
  にも有効な entity を生成
- 1 object 複数 behavior・同 type 複数 attachment を許可。各 attachment
  は engine-owned handle + `attachment_seq` を持つ
- `type` 未知: **active DLL が正常ロード済みなら hard error(typo)**・
  DLL unavailable 時だけ raw JSON pending + 名前入り WARN(B-2 後段の
  状態分離)

## 2. params(BEH-P0 — レビュー逐語)

> EventPayloadSchema の member-pointer field descriptor を generic
> StructFieldSchema へ抽出し、Bool/Enum、required/default、range、
> unit、canonical JSON encode/decode を持たせる。Params は default
> construct 後に存在 key だけを temporary へ適用し、全検証成功後に
> commit する。unknown key、型不一致、整数範囲、NaN/Inf、range、
> nested/unsupported type は stable error code+field path で拒否する。
> params 省略、部分指定、全型往復、二回 encode byte 一致、compile-fail
> unsupported field を gate とする。behavior/component/event で別々の
> field macro を新設しない。

- **同じ StructFieldSchema が ED-CODEC0(component codec)と
  インスペクタ schema を駆動**する — 三系統の反射を一本化
- 他オブジェクト参照は名前文字列 + onInit での use-time resolve
  (v1 のまま)

## 3. イベント(B-3 の解 — typed dispatch)

- **EventView 案は撤回**。system と同じ **typed overload**
  `onEvent(const DoorOpened&, BehaviorContext&)` を登録時に thunk 化し、
  **matching type の attachment だけ**へ宣言順配送(暗黙 broadcast
  禁止)
- 配送位置 = BehaviorSystem の system order 内(update と同じ位置規範 —
  「event だけ別順」の穴を作らない)
- owner unload 時に queued event の owner purge(既存 E1 の
  frozen queue との整合 — BEH1 fixture)

## 4. ライフサイクル(B-4/B-5 の解)

### 4-1. activation / deactivation barrier

```
scene load:
  第一段: 全 entity/component/special attachment/parent edge を
          transactionally publish(既存 ECS transaction のまま)
  第二段: scene activation barrier —
          attachment_seq 順に instance 生成 + onInit
          (onInit 失敗 = activation 全体 rollback・init 済みを
           逆順 onDestroy)
destroy:
  pre-destroy barrier — entity/component/parent が resolve 可能な
  うちに逆 attachment 順で onDestroy(noexcept 契約)→ instance 破棄
```

- callback 中の structural mutation(spawn/destroy/attach)は
  **次 boundary へ defer**。各 frame は開始時 snapshot を各 instance
  最大一回走査(B-4 末尾の規範)

### 4-2. 実行順(B-5 の解 — attachment_seq が正本)

> EntityId 数値/ECS storage 順を使用しない。scene load は
> `(object declaration index, component array index)`、runtime
> transaction は `(commit_seq, command index, attachment index)` から
> engine-owned monotonic `attachment_seq` を確定する。frame 中 spawn は
> 次 boundary から参加・destroy 済みは snapshot 上 skip。
> BehaviorSystem の固定 system order/name tie-break を文書化。

- 決定性 gate: 実行/イベント/lifecycle trace + RNG 結果を
  **新規 process 二回 + replay 二回で byte 一致**

## 5. G2 reload(B-6/B-7 の解)

### 5-1. reload 列(BEH1 改訂条件 — レビュー逐語)

```
candidate validation load/unload
   — registration count 以外の runtime/lifecycle trace を一切変えない
成功: old onDestroy/destroy → old unregister/unload →
      new register/activate → scene rebuild → new onInit
失敗: 旧 DLL/runtime 維持(rollback)
new scene/params decode 失敗: 旧 DLL reload + scene rebuild rollback
```

- owner 別 registry purge・owner 別 live instance destroy・
  GameLogicReloader の**全 failure/unload branch** からの unregister は
  **BEH0 の必須安全条件**(reload 未対応でも dangling callback を
  残さない — B-6 の境界訂正)

### 5-2. 状態の正本(B-7 の訂正 — v1 の虚偽を削除)

**WP90 full-reset を維持**する。従って:

> reload 後に復元されるのは **AuthoringSceneDocument に commit 済みの
> 値だけ**である。runtime で変化した component 値・未保存の behavior
> params・behavior 内部状態は**すべて消える**。

- 「残したい値は編集して Save する」(ED-AUTH0/SAVE0 経由)が v1 の
  作法。runtime snapshot を rebuild source にする案は **別 versioned
  migration WP**(BEH1 に混ぜない)

## 6. インスペクタ統合(BEH2 改訂条件 — レビュー逐語)

> behavior attach/remove/set-param は generic ECS component name だけで
> なく attachment handle/index を target にし、E-RPC transaction/
> journal へ参加する。attach は commit 後の次 activation boundary で
> onInit、remove は commit 前 pre-destroy で onDestroy、params edit は
> 「live instance へ atomic apply」を固定し、同一 frame の
> onEvent/onUpdate から見える版を規範化する。undo/redo、replay/golden
> reject、G2 同時発生、type combo の owner generation 更新、pending
> params 表示を fixture にする。

## 7. WP 分割(レビュー §4.2 の依存順を採用)

```
BEH-P0 + ED-AUTH0 → BEH0 → BEH1
BEH0 + E-RPC1/JOURNAL0 → BEH2
```

| WP | 内容 | 条件の添付元 |
|----|------|--------------|
| **BEH-P0** | StructFieldSchema 抽出(+Bool/Enum/default)— ED-CODEC0 と共通機構 | §2 逐語 |
| **BEH0** | 登録(宣言のみ)+ special binder/arena + barrier lifecycle + attachment_seq + typed event + **owner 安全(全 branch purge)** | §1/3/4 + BEH0 改訂条件逐語 |
| **BEH1** | 二世代 reload fixture(candidate 無副作用・成功列・rollback・pending 復旧・queued event purge・onInit fault) | §5 逐語 |
| **BEH2** | E-RPC/インスペクタ統合(attachment handle target) | §6 逐語 |
| 後続 | example playercontrol の behavior 移行(dogfooding・挙動 byte 一致)・runtime snapshot migration(別 WP) | — |

## 8. 未決事項

1. onFixedUpdate — fixed timestep 導入時に additive(v1 のまま)
2. behavior 間直接参照 — 不許可(E1 経由)。v2 で再評価(v1 のまま)
3. params の live apply と destroy/recreate の選択は BEH2 で
   「atomic apply」に固定済み — schema_version が変わる reload 時のみ
   recreate
