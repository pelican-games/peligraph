# プレファブ設計 — パラメータ付きコンポーネント束(v3)

状態: **草案 v3。v1 は 9 指摘・v2 は 11 指摘で No-Go。**
**v3 の中心判断: 範囲を単一オブジェクトに絞る。**多オブジェクトの破綻 3 件
(1:1 不変量 / unpack 命名の非単射 / root・forest 規約)は v1 の範囲から消え、
別設計の前提として §6 に名指しで残る。
利用者は設計会話で「v1 は単一オブジェクトから、多オブジェクトは parent の
実戦検証を挟んでから」に既に同意している。
末尾 §9 に v2 の 11 指摘への答えを残す。

## 0. 原則

- 参照か、明示 unpack か。半連結の上書き追跡は作らない
- 上書き面は作者が宣言したパラメータだけ
- 型系は `StructFieldSchema` を正準に使う(型語彙もそのまま: `i32` / `f32` / enum /
  vec2-4 / string …。v2 の例は `"int"` という存在しない型名を書いていた)
- **置換の順序を規範化する(v2 の破綻 1 への答え):**
  1. raw prefab JSON 上で placeholder(`{"$param"}`)を走査・検証する
  2. 束縛値で置換する
  3. **具体値になった JSON を通常の codec に渡す**
  codec は tagged node を一切見ない。placeholder の検証は
  placeholder-aware な独自 pass であり、codec の closed-key 検査とは別物である
- **束縛可能な場所は codec が明示公開する inventory で決める(v2 の破綻 2 への答え)。**
  schema 投影は根拠にできない —— schema に載るのに runtime が読まない field
  (`simplemodelview.params`)と、schema に載らないのに読む field
  (`camera.controller.target`)が実在する。
  **各 codec に bindable-path の明示列挙(JSON pointer + 型 + 種別)を追加する**
- **discriminant は束縛不可(v2 の破綻 3 への答え)。**
  必須 field 集合を変える enum(`collider.shape` / `camera.type` / `light.type`)は
  値ではなく構造であり、v1 ではパラメータ化を禁止する(bindable inventory で
  non-bindable と宣言)。構造の差分は別プレファブで表現する
- 展開は load 時。fail-fast

## 1. 範囲: 単一オブジェクト(v3 の中心判断)

**プレファブ = 1 オブジェクト分のコンポーネント束 + 宣言パラメータ。**

これで消える困難(v2 レビューの破綻 3 件):

- **1:1 不変量が保たれる**: instance 1 = authored object 1 = runtime object 1。
  `AuthoringObjectId` の 1:N 投影(composite key の RPC 貫通)が不要になる
- **鋳造・リンカ・root 規約が不要**: 内部オブジェクトが無いので `{"$object"}` も
  親の書き換えも designated root も存在しない
- **unpack が自明**: 同一 object 上で `prefab` キーを展開済み `components` に置換する。
  名前は不変、R7 準拠、保存可能、undo は journal の逆操作 1 件

利用者の元の要望「インスタンス前の**コンポーネントの**プレファブ化」は
この範囲で満たされる。車 + 車輪のような複合体は §6(別設計)。

## 2. プレファブ文書

```json
{ "schema": "pelican.prefab", "version": 1, "name": "enemy_grunt",
  "parameters": [
    { "name": "hp",    "type": "i32", "range": [1, 9999], "default": 30 },
    { "name": "tint",  "type": "vec4", "default": [1,1,1,1] },
    { "name": "body",  "kind": "asset", "asset_kind": "model", "default": "sotai" } ],
  "components": [
    { "name": "transform" },
    { "name": "simplemodelview", "model": { "$param": "body" } },
    { "name": "sprite_view", "texture": "white", "color": { "$param": "tint" },
      "size": [1,1], "pivot": [0.5,0.5] },
    { "name": "collider", "shape": "capsule", "radius": 0.4 },
    { "name": "behavior", "type": "grunt_ai", "params": { "hp": { "$param": "hp" } } } ] }
```

(v2 の例の欠陥 3 件を修正: `int` → `i32` / `tint` を実際に使う /
discriminant `shape` はパラメータでなく固定値)

- 未使用パラメータ宣言はエラー。bindable inventory に無い場所への束縛はエラー
- 入れ子プレファブは禁止(名前付きエラー)
- 置き場所: project.json(`prefabs[]`)

## 3. scene 側: version 2 ゲートと互換 matrix

```json
{ "schema": "pelican.scene", "version": 2, ... }
{ "name": "grunt_01", "parent": "spawn_area",
  "components": [ { "name": "transform", "pos": [10,0,3] } ],
  "prefab": { "ref": "enemy_grunt", "parameters": { "hp": 45 } } }
```

- instance に同居できる components は `transform` のみ(配置)。
  プレファブ側 transform と衝突する field は instance 側が勝つ…ではなく、
  **プレファブに transform を含む場合、instance 側 transform との合成規則は
  「instance が field 単位で上書き」とせず、`transform` は instance 専有とする**
  (プレファブは transform 以外の components を束ねる。単純さを優先)
- **互換 matrix を受け入れ条件にする(v2 の指摘 4 への答え)**:
  旧 engine + v2 → 拒否(既に成立) / 新 engine + v1 → 従来どおり
  (v1 内の `prefab` キーは未知メタデータのまま保持・無視) /
  新 engine + v2 → 展開 / **version 2 の受理と expander の配線は同一 capability として
  原子的に入れる**(受理だけ先行する半端を作らない)
- 全入口(engine load / studio project open / RPC snapshot import)が
  同じ version 分岐を通ることを検査する

## 4. 依存指紋は SnapshotV2 として(v2 の破綻 5 への答え)

- 現行 Snapshot V1 の import request は strict schema で、closure field を足すと拒否される。
  **正式手順どおり request / response / import の三 schema を同時に上げる `SnapshotV2` を
  定義し、sorted unique な `{prefab_name, digest}` closure を digest 対象に含める**
- V1 は prefab 非参照 scene に限って受理を維持
- `save_scene` の baseline は `{scene_digest, prefab read-set}` の不可分 snapshot に

## 5. 出荷単位(v2 の指摘 6 への答え: U1 に利用者価値を入れる)

```
U1  文書 + registry + version 2 ゲート(expander と原子的)+ 展開 + SnapshotV2 指紋
    + bindable inventory + **studio: instance の表示・解決値表示・パラメータ編集**
U2  unpack(単一 object 版)+ 一括取り込み面 + 純粋 dry-run
U3  authoring transaction 基盤(文書別 revision)→ パラメータ昇格
U4  ctx.spawn(単一 object。group lifecycle 不要になったため独立に成立)
```

受け入れ条件の骨子:

- U1: プレファブ無し / default / 束縛あり を同じ本番ロードで実行し、
  **実 ECS / behavior の解決値**を検査。互換 matrix(§3)。SnapshotV2 の不一致拒否。
  未使用宣言・inventory 外束縛・discriminant 束縛・型違反の名前付き拒否。
  **studio で instance を選択 → 解決値が表示され、パラメータ編集が journal に載る**
- U2: **unpack → undo → redo → save → 新プロセス reload**
  (v2 の指摘 8 の順序修正: journal はプロセス内なので undo は save の前)。
  dry-run は allocator を呼ばない純粋 preflight、0 回対 N 回のバイト一致
- U4: spawn した object の destroy が 1 entity で完結(単一なので群規約不要)、
  リプレイ trace 同一

## 6. 多オブジェクト(別設計。前提を名指しして送る)

複合体(車 + 車輪)は**この文書の範囲外**とし、着手前に次の基盤設計を要する:

1. **1:N 投影**: `(instance AuthoringObjectId, internal name)` composite key を
   RPC / editor / transaction まで貫通させる(現行は authored:runtime 1:1 を強制)
2. **root / forest 規約と transform 合成式**(external_parent × instance × root の順序、
   非可換な回転・非一様 scale での判別テスト込み)
3. **単射な unpack 命名**(`__` 基礎は `a + b__c` と `a__b + c` が衝突する。
   escape か length-prefix、total order、journal への確定 name map 記録)
4. `parent` 機構の実戦投入(出荷 scene 使用 0 件)

## 7. D0 の層割り(v2 の指摘 11 への答え)

- **placeholder 文法 + パラメータ宣言 + prefab 文書の構文検証**: 共有 library
  (`pelican_project`)。studio は offline でもこれで文書を検証できる
- **bindable inventory と codec 検証**: core。**RPC で inventory を公開**し、
  studio は live engine からそれを取得して束縛 UI を作る
- engine 不在時の studio: 構文検証まで(束縛先の妥当性は「未検証」と表示。黙って通さない)

## 8. 将来拡張

HDA: authoring 時 cook → stamp。parm interface ↔ parameters 写像。
attributes: behavior params 強化の後に要否判断。
多オブジェクト: §6 の基盤が揃ってから。

## 9. v2 レビュー(11 指摘)への答え

| # | 指摘 | v3 の答え |
|---|---|---|
| 1 | tagged node が codec を通らない | 置換順序の規範化: raw 上で検証 → 置換 → 具体値を codec へ |
| 2 | opaque 判定の根拠が偽(schema は文法でない) | codec が bindable-path inventory を明示公開 |
| 3 | discriminant で「値調整」が破綻(自例が自壊) | discriminant は束縛不可。例を修正(i32 / tint 使用 / shape 固定) |
| 4 | v2 ゲートの互換 matrix 欠落 | §3 の matrix を受け入れ条件化。gate と expander を原子的に |
| 5 | closure が Snapshot V1 wire に載らない | SnapshotV2(三 schema 同時)+ save read-set |
| 6 | U1 に単独価値なし | U1 に表示・解決値・パラメータ編集を含めた |
| 7 | 1:N が authored:runtime 1:1 不変量に反する | **範囲外に**(単一 object)。§6 で前提を名指し |
| 8 | U2 条件が文字どおり永久に通らない | undo → save → reload の順に修正 |
| 9 | unpack 命名が非単射・順序未定義 | **範囲外に**(単一 object は名前不変)。§6 に記録 |
| 10 | root:true が anchor にならない | **範囲外に**。§6 に記録 |
| 11 | D0 の実装所有者が不在 | §7 で層割りを名指し |

壊せなかった点(2 巡通算): v2 拒否ゲートの成立 / runtime 名の対衝突(多 object 時)/
v1 未知メタデータの保持可能性 / U3 の 0対N 条件 / U4 の条件の非代理性。

## 10. 未決事項

1. 多オブジェクト(§6 の基盤 4 件が前提)
2. optional slot / variant(構造差分。需要の実測後)
3. authoring transaction 基盤の詳細(U3 の前提。別文書)
4. attributes 層 / HDA live cook
