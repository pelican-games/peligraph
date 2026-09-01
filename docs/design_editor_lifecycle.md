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

---

## v2(第 1 巡・7 件への裁定。**v2 > v1。矛盾したら v2 が正**)

第 1 巡は全件が実在コードとの矛盾だった。**共通の根: v1 は多段でステートフルな
既存機構を「一回の変換」として扱っていた。** とくに、現行 transaction は
**「ECS token を逐次 publish し、inverse token で集約 rollback する」**という
解法を**既に持っている**(editorprojectiontransaction.cpp のコメントに明記)。
v1 の「全 prepare → PONR」はそれを見ずに書いた別解であり、撤回する。

### 裁定 1: 原子性の階層を宣言する(#1・#2 の答え)

「すべてを一つの成功または失敗に」を**撤回**し、**3 層**に分ける。

| 層 | 対象 | 契約 |
|---|---|---|
| **A: 原子的** | authoring 文書 + ResolvedScene + **journal record** | 全部成功か全部失敗。journal record・last-writer・undo entry・runtime binding は **Prepare で構築**し、Publish では splice/swap のみ(割り当てゼロ) |
| **B: noexcept 公開** | ECS 構造変更・arena attach/detach・specials 再投影 | A から**導出**される。**逐次 publish + inverse token で集約 rollback**(既存機構をそのまま使う)。事前確保により publish 自体は throw しない |
| **C: 最善努力 + 名前付き終端状態** | **behavior の activation(`onInit`)** | **既存の契約をそのまま維持**(`activateReadyEditorAttachments` が transaction 外で実行し、失敗を `activation_failed` として保持)。**transaction の rollback にしない**。応答に名前付きで載せる |

- `onInit` は契約上 fallible(`noexcept` は `onDestroy` のみ)。v1 が
  `Publish(noexcept)` に入れたのは**既存のテスト済み契約との矛盾**であり撤回
- `AfterPublication` fault は **A 層に対してのみ**維持(document を戻す)。
  B 層は inverse token、C 層は終端状態で表現する

### 裁定 2: PONR は callback と外部副作用にのみ適用(#2 の残り)

PONR は「全 publish の前」ではなく、**不可逆な callback(`onDestroy`)と
外部副作用の直前**に置く。順序:

```
Prepare(全 fallible + journal/binding の事前構築 + 参加者 epoch の採取)
  → 逐次 ECS publish(失敗は inverse token で巻き戻す。ここはまだ可逆)
  → [PONR] 参加者 epoch を再検証 → 逆順 onDestroy → 外部副作用
  → Publish(A 層の splice。割り当てなし)
  → activation は transaction 外(C 層)
```

`onDestroy` 中の deferred queue は PONR 後にしか積まれないので巻き戻し不要。

### 裁定 3: 参加者レジストリと epoch 再検証(#4 の答え)

snapshot 対から作るのは**論理的な desired diff だけ**。実適用には
`PreparedPublication` が**versioned な参加者 snapshot** を持つ:
ECS epoch / SceneLoader の bindings と name map / arena state と deferred watermark /
renderer・model の resource token / camera runtime overlay / **physics の
`active_trigger_pairs`**。**PONR 直前に epoch を再検証**し、ずれていたら
名前付き失敗(可逆な位置なので安全)。

### 裁定 4: `AuthoringComponentId` と `OriginTransitionMap`(#5 の答え)

- **session 安定な `AuthoringComponentId`** を導入する。authoring の structural stage が
  発行し、値・順序による matching をやめる(重複 component と同時 insert/remove で
  取り違える問題の根治)
- unpack は `OriginTransitionMap<old gid → new AuthoringComponentId>` を明示的に出す。
  **undo は逆写像を出す** —— v1 の「authored→generated は存在しない」は**誤り**
  (undo は必ずそれを通る)。撤回し、**双方向の写像を契約**とする
- §3-3 の「handle 維持・callback 無し」は**この写像がある場合にのみ**成立する。
  写像が無い遷移(値の一致に頼る推測)は**行わない**

### 裁定 5: `ObjectReferenceIndex`(#6 の答え)

rename の閉包は `name + parent + kind:"object" parameter` では**不足**。
camera の `controller.target` のように **component が直接 object を参照する field** がある。

- **codec descriptor に semantic `object_ref`(JSON path 付き)を宣言**させ、
  resolver が `ObjectReferenceIndex` を構築する
- rename はその index 全体 + SceneLoader の name map を**原子的に**更新
- **参照 field を宣言しない external component は、object rename の対象 scene に
  存在するだけで名前付き拒否**(黙って壊さない)。これは fail-fast の家風どおり

### 裁定 6: `runtime_resolution_error` の型付けと inactive scene(#7 の答え)

- 現在 string で保持されている resolution failure を**型付き named error に格上げ**し、
  dry-run はそれをそのまま返す / 実適用は **Prepare 段で同じ named error として拒否**
- **inactive scene の扱いを明示**: inactive scene の解決失敗は**拒否しない**
  (現行どおり保持)。current scene に切り替える瞬間に named 失敗させる

### 裁定 7: 段割りの修正

- **L1**: 裁定 1〜3 + WP361a 状態の既知の穴 4 件。**単独価値**: 4 件の穴が閉じ、
  別 scene 編集・onDestroy 生存・混合 batch・親子 spawn が正しくなる
- **L2**: 裁定 4〜6(`AuthoringComponentId` / `OriginTransitionMap` /
  `ObjectReferenceIndex` / エラー面統一)。**これが U2 を解放する**
- v1 の「ECS batch token の新設」は**不要**(既存の逐次 publish + inverse で足りる)

### v2 の未決(第 2 巡で攻撃してほしい点)

1. 裁定 1 の C 層(activation を transaction 外に残す)で、U2 の一括流し込みの
   「all-or-nothing」は本当に成立するか(authoring は原子的だが activation は別、
   という二段の結果を利用者にどう見せるか)
2. 裁定 3 の epoch 再検証で、どの参加者が「ずれ」を検出できないか
3. 裁定 5 の「宣言しない external component は拒否」が既存 project を壊さないか
   (出荷 4 project と core resources での実測が要る)
4. `AuthoringComponentId` の発行・永続化範囲(scene 文書に書くのか session 内のみか)
   —— WP304「安定 ID」が過去に不合格になった領域と重なる

---

## v3(第 2 巡・7 件への裁定。**v3 > v2 > v1。これが最終**)

第 2 巡は「成立不能 3・決定不能 4」。積み重ねが一つの結論を指している ——
**完全な原子性は engine の 3 つの性質と両立しない**:
`onInit` は契約上 fallible で任意の副作用を持つ / ECS の `MutationScope` は非再入 /
journal は post-commit の live state から構築される。
これらを変えるのは **3 つの別設計**であって、1 つの設計ではない。

### 裁定 1(最重要): **設計を 2 つに割る。U2 は原子性の完成を待たない**

実測した事実:

- `set_params` は attach/detach とは**別の edit kind**(behaviorarena.cpp:363)
- **`onInit` は activation 時にのみ呼ばれる**(同 :722)
- generated↔authored を弾いているのは `sameSceneBehaviorOrigin` という**限定的な
  照合関数**(同 :417 付近)

したがって **U2 の操作(unpack / rename / 一括流し込み / dry-run)は
entity も behavior も作らず壊さない**。第 2 巡が「成立不能」とした 3 件
(C 層と all-or-nothing / PONR と非再入 scope / PONR 後の rollback)は
**すべて spawn・destroy の話**であり、**U2 は通らない**。

| 設計 | 範囲 | 状態 |
|---|---|---|
| **E1(U2 の前提。小)** | **create/destroy を含まない構造編集**: origin 遷移(unpack)・`ObjectReferenceIndex`(rename)・エラー面統一(dry-run) | **本文書の残りはこれ**。WP 起票可能 |
| **E2(大。別文書)** | **lifecycle の原子性**: spawn / destroy / それらの undo。ECS aggregate protocol + activation 契約 + journal 境界の 3 つを決める必要がある | **未着手。WP361a 状態の既知の穴 4 件はここ** |

**E1 は新しい原子性契約を作らない。** 既存の WP360 pair(AuthoringSceneDocument +
ResolvedScene の原子公開)と、**既存の journal 経路(set/unset と同一)**をそのまま使う。
v2 の裁定 1〜3(3 層・PONR・epoch 再検証)は **E2 へ移す**。

### 裁定 2: origin 遷移は照合関数の拡張で行う(callback を通らない)

`sameSceneBehaviorOrigin` が **その execution の `OriginTransitionMap` を参照**して
generated(gid)↔ authored(component index)を同一と判定できるようにする。
これにより unpack は **attach/detach を一度も通らず**、handle・instance 状態・
`activation` 状態がそのまま維持される。**`onInit` も `onDestroy` も発火しない。**

- 受け入れ: unpack 前後で attach/detach callback が**ちょうど 0 回**、
  handle 不変、behavior instance の内部状態が保存される

### 裁定 3: `AuthoringComponentId` を**撤回**(第 2 巡 #6 の答え)

永続 authored component ID は scene 形式・journal・undo・snapshot import に跨り、
**WP304「安定 ID」が不合格になった領域そのもの**。E1 には不要である:

- `OriginTransitionMap` は **その 1 回の execution 内でのみ有効な写像**でよい
  (unpack: gid → その execution の authored index / undo: その逆)
- 写像は **journal の forward/inverse に保存**する(値の一致に頼る推測はしない)
- scene bytes には何も足さない。snapshot import の再採番とも衝突しない

### 裁定 4: E1 の受け入れ(v2 の受け入れ節はこれで置換)

- **unpack**: callback 0 回 / handle 不変 / arena の origin が変わる /
  `set_component_value` が通るようになる(**codec がある component のみ**。
  第 1 巡 #3 のとおり「authored だから編集可能」は偽) / undo で origin と gid が復元
- **byte 保存の限定**: prefab body の `generated:true` 等、authored では拒否される
  marker は **unpack 時に除去**する。「展開 bytes 完全保存」は**撤回**し、
  除去対象を規範表に列挙する(第 1 巡 #3)
- **rename**: `ObjectReferenceIndex`(codec の semantic `object_ref` 宣言由来)
  + name map を原子的に更新。**参照 field を宣言しない external component が
  対象 scene に存在したら名前付き拒否**。**出荷 4 project と core resources で
  実測**して壊さないことを確認(第 2 巡が要求)
- **dry-run**: `runtime_resolution_error` を型付き named error に格上げし、
  dry-run と実適用が同一 code。inactive scene の失敗は保持(current 切替時に named)
- 全て「取り消したら落ちる」対照。実 producer request の捕捉

### v3 の残余(WP 仕様レビューで受ける)

- `ObjectReferenceIndex` の codec 宣言面の詳細(JSON path の文法)
- unpack で除去する marker の完全な規範表
- E2 の起票時期(U2 完了後でよい)
