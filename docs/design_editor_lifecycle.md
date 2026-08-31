# エディタ lifecycle × runtime の合流設計(v1)

状態: **v1 草稿。敵対的設計レビュー前**(orchestration.md §A: 最大 3 巡 → 決定差分)。
この領域は WP361b → WP361b v2 と**同じ機構で 2 度 revert**しており(台帳
「裁定: エディタ lifecycle × arena の合流は『修正』ではなく『設計』だった」)、
**設計レビュー合格前の実装着手を禁止**する。

## 0. 要求

エディタの構造編集(spawn / destroy / undo・redo / unpack / rename / 一括流し込み)が、
authoring 文書・ECS・BehaviorArena・camera/light/physics 投影・journal を
**一つの成功または失敗**として更新すること。これが無いと U2(unpack・一括流し込み)は
起票できない(WP370 v1 レビュー #1 で実証)。

恒常制約: fail-fast / 名前付きエラー / 決定性 / パージ性を下げない /
実 producer request を通す対照(§4 規約 10)。

## 1. 仕様の種(3 本の失敗レビュー = 23 件。全て台帳に原文あり)

| 出所 | 確定した処方(本設計はこれを覆さない) |
|---|---|
| WP361a §10(9 件) | runtime adapter は **current scene の変更にのみ**生成(authoring は全 scene 適用、複数 scene batch は受理)/ onDestroy は entity 生存中(公開契約 08_gameplay.md)/ journal・arena・RPC の seq 一本化 / 同一 batch の parent→child spawn |
| WP361b §10(7 件) | **identity plan は operation 列からでなく final resolved scene から毎 execution 再構築**(undo/redo 含む)/ inverse に forward seq をコピーしない / seq 0 は sentinel にしない / 文法は単一 AST parser(`attachment_handle` は公開 CAS)/ 実 producer(inspectormodel・imgui inspector)request の捕捉 |
| WP361b v2 §10(7 件) | **ECS destroy token は publish 直前に entity から chunk/row を再解決**(複数 destroy の backfill 追随。放置すると二重 move の UB)/ onDestroy は**全 fallible 段の後の point of no return でのみ**実行し deferred queue を checkpoint / 別 scene 編集が現行 runtime を再構築しない / camera/light/collider の実値対照 |
| WP370 v1 レビュー | **generated → authored の origin 遷移**(unpack)の arena 意味論が未定義 / rename の閉包(`name + 全 parent refs + 全 object-parameter`)/ dry-run と実適用の named error 一致(resolver は cross-field failure を throw せず `runtime_resolution_error` に保存する) |

## 2. 現状の固定点(レビューで検算済みの事実)

- arena の再利用判定は authored↔authored / 同一 gid の generated↔generated のみ。
  **generated↔authored は必ず不一致 → 新 handle**(behaviorarena.cpp:419 系)
- 現行 runtime routing が prefab 用 adapter を付けるのは set/unset だけ
  (editorruntimefactory)。lifecycle 操作の behavior は adapter が明示 skip
- resolver は cross-field 失敗を throw せず `runtime_resolution_error` に保存して
  解決成功扱い(resolvedscene.cpp:280 系)。実適用側は adapter 失敗が
  `method_unavailable` へ落ちる —— **dry-run と実適用でエラー面が割れている**
- projection transaction は「複数 command を全適用 → 一度だけ resolve」ができる
  (editorprojectiontransaction.cpp:829 系)。**単一 PreparedOperation・複数 command・
  単一 inverse は構造的に可能**
- light-only / collider-only / empty object は ECS entity を持たない
  (scene.cpp:163 系)。出荷 example に light-only 14 個、vrm_xr に 1 個

## 3. 設計

### 3-1. 背骨: ReconciliationPlan(差分は snapshot の対から、一度だけ)

```
(prev ResolvedScene, next ResolvedScene, current_scene_id)
    → ReconciliationPlan
       { ecs: [create | destroy | move],
         behaviors: [attach | detach | set_params | keep],
         specials: [camera / light / physics の再投影(current scene のみ)],
         identity: 全 attachment の (entity, final origin) → handle・seq 対応表 }
```

- **全編集がこれを通る**: component 編集・lifecycle・prefab set/unset・unpack・
  rename・一括流し込み・undo・redo。**operation 列から plan を作る実装を禁止**
  (WP361b の敗因)
- plan は **next resolved scene だけを正**とし、毎 execution(undo/redo 含む)
  再構築する。inverse journal は seq を保存しない(order key は bytes から再導出)
- runtime 適用は `current_scene_id` の変更に限定。他 scene は authoring のみ

### 3-2. 二相と point of no return

```
Prepare(全 fallible: 割り当て・codec decode・plan 構築・検証)
  → [point of no return]
  → Publish(noexcept):
      逆順 onDestroy(entity 生存中)→ ECS publish(destroy token は
      ここで entity から chunk/row を再解決)→ attach/onInit → specials
  → finish
```

- **PONR 以後に fallible を置かない**。rollback は callback を「なかったこと」に
  できないため、PONR 前に callback を発火させる実装を禁止(WP361b v2 #1 の敗因)
- deferred queue(onDestroy 中の `ctx.createObject/removeObject`)は
  PONR 後にのみ積まれるので rollback 不要(構造的に)
- fault 注入点は Prepare / Publish / AfterPublication の 3 種を維持
  (WP362 の 12 fault point の流儀)

### 3-3. origin 遷移(unpack の前提。**本設計の新規決定**)

**決定: unpack の generated → authored は「同一 instance の origin 付け替え」とする。**
handle 維持・onDestroy/onInit を発火しない。identity 対応表が
`pre-execution origin → final origin` の写像を運び、arena は origin field だけを
書き換える。

- 根拠: unpack は論理的に同じ behavior が「編集可能になる」だけであり、
  ゲーム状態(instance の内部状態)を破壊する理由がない。DCC 的期待とも一致
- 対立案(destroy+create として扱い callback を発火)は、undo/redo で状態が
  消える点で劣る。ただし**レビューで攻撃されたい点**として §6 に残す
- 逆方向(authored → generated)は存在しない(re-pack を作らないため)

### 3-4. entity token の optional 化

runtime binding の無い object(light-only 等)の destroy/spawn は
**external projection(light container 等)のみを処理**し、ECS token を要求しない。
spawn 側も SceneLoader と同じ entity 生成条件に揃える(WP361b v2 #4 の処方)。

### 3-5. エラー面の統一(U2 dry-run の前提)

- resolver の `runtime_resolution_error` を**一級の named error として表面化**する:
  dry-run はこれをそのまま返し、実適用は **Prepare 段で同じ named error として拒否**
  する(adapter まで到達させて `method_unavailable` に潰さない)
- これにより「dry-run = 純 resolver 経路」の裁定(台帳)が成立する

### 3-6. rename の閉包(document 層だが、runtime adapter が絡むためここで確定)

rename は**単一 PreparedOperation・複数 command・単一 inverse**:
`name + 全 parent refs + 全 kind:"object" parameter override` を安定順序で更新。
old/new name を structural domain に含める(undo 衝突の予約)。
behavior の object target が変わる場合の runtime 反映は §3-1 の plan に乗る。

## 4. 出荷単位(段割りの目安。WP 分割は仕様レビューで確定)

- **L1**: ReconciliationPlan + 二相 + PONR + destroy token の row 再解決 +
  entity token optional。**単独価値**: WP361a 状態の既知の穴 4 件
  (別 scene 破壊 / onDestroy 死後 / 混合 batch 分裂 / 親子 spawn)が閉じる
- **L2**: origin 遷移 + エラー面統一 + rename 閉包。**これが U2 を解放する**

## 5. 受け入れオラクル(必須。全て「取り消したら落ちる」)

- 3 本の失敗レビューの**再現入力を全部 fixture 化**: 別 scene spawn で current の
  runtime snapshot 完全不変 / onDestroy 内 `isAlive == true` / 混合 batch の
  journal・arena・RPC 三者一致 / 同一 batch 親子 spawn / **出荷 example の
  DamagedHelmet + sponza 同時 destroy**(二重 move UB の再現系)/ light-only destroy /
  set→(失敗注入)→rollback→retry で **onDestroy ちょうど 1 回**
- **実 producer request の捕捉**(手書き JSON 複製は producer テストではない)
- unpack: 前後で behavior instance の内部状態が保存され、callback 回数 0、
  arena origin が generated→authored に変わり、`set_component_value` が通る。
  undo で gid・origin・handle が復元
- dry-run: 実適用と**同一の named error** / scene revision・ECS・arena が不動

## 6. 未決事項(レビューで攻撃してほしい点)

1. §3-3 の「handle 維持・callback 無し」は正しいか(ホットリロードや
   スクリプト側の観測と矛盾しないか)
2. ReconciliationPlan の粒度 —— specials(camera/light/physics)を毎回全再投影する
   コスト(速度非重視だが、編集レイテンシとして許容範囲か)
3. §3-5 で Prepare 段拒否に変えると、既存の「resolver は成功扱い + 実行時失敗」に
   依存した挙動(あれば)が壊れないか
4. 複数 actor の並行編集と structural domain の予約範囲
