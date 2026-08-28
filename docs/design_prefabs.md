# プレファブ設計 — パラメータ付きオブジェクト束(v2)

状態: **草案 v2。v1 は codex 設計レビューで 9 指摘(破綻 5)・全段 No-Go。**
末尾 §12 に v1 の何が壊れたかを残す。
先行文書: `design_scene_format.md`(§4 未決 2)、`design_object_behaviors.md`、
台帳の設計ノート(2026-08-28)。

## 0. 要求と原則

利用者要求(v1 と同じ):
インスタンス前のコンポーネント束の再利用 / インスタンス調整の軽量化 /
AI・外部データからの一括流し込み / 将来の HDA・attribute。

原則(v1 から 2 つ追加。いずれもレビューが v1 の自己違反を突いた):

- 参照か、明示 unpack か。半連結の上書き追跡は作らない
- 上書き面は作者が宣言したパラメータだけ
- 文法は 1 つ。**型系も 1 つ: パラメータ型は `StructFieldSchema`
  (enum / vec2-4 / quat / string / unit / range)を正準に使う。**
  v1 は float/int/bool だけの独自小型集合を発明し、
  自分の「第二の文法を作らない」に違反した
- **文字列 sentinel を使わない。**`"$hp"` は文字列リテラルと区別できない
  (behavior params は string を実際に受ける)。**tagged node にする:**
  `{"$param": "hp"}` / `{"$object": "sensor"}`
- 名前は鋳造。展開は load 時。fail-fast

## 1. プレファブ文書

```json
{ "schema": "pelican.prefab", "version": 1, "name": "enemy_grunt",
  "parameters": [
    { "name": "hp",    "type": "int",   "range": [1, 9999], "default": 30 },
    { "name": "tint",  "type": "vec4",  "default": [1,1,1,1] },
    { "name": "shape", "type": "enum",  "choices": ["sphere","box"], "default": "sphere" },
    { "name": "body",  "kind": "asset", "asset_kind": "model", "default": "sotai" } ],
  "objects": [
    { "name": "root", "root": true,
      "components": [
        { "name": "transform" },
        { "name": "simplemodelview", "model": { "$param": "body" } },
        { "name": "collider", "shape": { "$param": "shape" }, "radius": 0.4 },
        { "name": "behavior", "type": "grunt_ai",
          "params": { "hp": { "$param": "hp" }, "target": { "$object": "sensor" } } } ] },
    { "name": "sensor", "parent": "root",
      "components": [ { "name": "transform" } ] } ] }
```

決定(v2 で変わった点に ★):

- ★ パラメータ宣言は `StructFieldSchema` の型・enum・unit・range をそのまま使う。
  asset パラメータは `kind: "asset"` + `asset_kind`(asset_data の表を選ぶ)
- ★ **束縛は tagged node のみ**。使用箇所は codec schema が宣言した field に限る
  (`simplemodelview.params` のような **opaque bag への束縛はエラー** ——
  検証は通るが runtime が読まない no-op 束縛を作らせない)
- ★ **宣言したパラメータは 1 箇所以上で使われなければエラー**(未使用宣言の禁止)
- ★ **objects は全件 named 必須**(鋳造は内部名を要求する)。
  **`root: true` をちょうど 1 個**(外部から instance 名で parent 参照されたときの
  anchor 先。instance の transform もここに適用)
- ★ **`{"$object": "内部名"}`**: behavior params 等がプレファブ内部の object を指す
  型付き参照。展開時に鋳造名へ、unpack 時に unpack 名へ**リンカが書き換える**。
  素の文字列で内部 object 名を書いても relocate されない(それは scene 全域の名前)
- 入れ子プレファブは v1 実装では禁止(名前付きエラー)。v2 課題
- 置き場所: project.json(`prefabs[]`)。著作文書の線に従う

## 2. scene 側とバージョンゲート ★

```json
{ "schema": "pelican.scene", "version": 2, ... }
```

- **prefab を使う scene は `version: 2`。**現行エンジンは `version == 1` を要求して
  それ以外を拒否する(`sceneformat.cpp` の envelope 検査)ので、
  **旧エンジンは prefab 入り scene を黙って欠落表示するのではなく、確実に拒否する**。
  v2 は prefab 以外の点で v1 と同一
- 既存 v1 scene が `prefab` キーを未知メタデータとして保存している可能性への答え:
  version 1 のままなら従来どおり保持・無視(挙動不変)。version 2 で初めて構文になる
- インスタンス: `{ name, parent?, components: [transform のみ可], prefab: { ref, parameters } }`
- 外部 object の `parent: "インスタンス名"` は designated root へ解決

## 3. 展開・鋳造・リンク・上限

- runtime 鋳造名は `インスタンス名/内部名`(著作名クラス外の `/` で衝突構造的不可。
  **runtime 専用であり、authoring 文書には決して入らない** ——
  v1 はここで unpack を壊した)
- リンカの書き換え対象: 内部 `parent` と、全 component/behavior の `{"$object"}` ノード
- ★ **展開上限**: 展開前に「インスタンス数 × プレファブ object 数」を検査し、
  超過は既存 runtime を無変更のまま `prefab_expansion_limit` で拒否
- 展開順は文書順。決定的

## 4. unpack ★(authoring 名は鋳造名と別)

- unpack 時の authoring 名は **`インスタンス名__内部名` を基礎**に、既存名と衝突したら
  宣言順で決定的に接尾辞を付けて解消(R7 準拠 = 保存可能)
- `parent` と `{"$object"}` を unpack 名へ書き換え
- **journal に載せる前に、生成後の scene 全体を通常 validator に通す**
- 一方向。undo は journal の逆操作で(unpack 前のインスタンス宣言に戻る)

## 5. 依存指紋 ★

- **scene snapshot(export/import)は参照 closure `{prefab_name, digest}` を含み、
  import は不一致を `prefab_dependency_mismatch` で拒否する**
  (scene bytes が同一でも registry が違えば別の結果になる、を検出可能に)
- **`save_scene` の baseline 比較にも、読んだ prefab の digest を read-set として含める**

## 6. エラーコード ★

editor の stable enum の流儀に合わせ、最低限:
`prefab_not_found / prefab_version_unsupported / prefab_parameter_unknown /
prefab_parameter_type / prefab_parameter_unused / prefab_object_ref_unresolved /
prefab_root_missing / prefab_expansion_limit / prefab_dependency_mismatch /
prefab_nested_unsupported`。
文脈として scene_id / instance 名 / prefab 名 / parameter 名 / JSON pointer /
(取り込み時)行 index を必ず載せる。

## 7. 構造的バリエーション(正直な保留)★

レビューの指摘 4(「shape や color は変えたいのに変えられない」)には
**型面の拡大(StructFieldSchema 全型)で答える**。これで collider の shape(enum)、
light の color(vec)、sprite の size/pivot 等、**値の調整はすべてパラメータ化できる**。

**構造の差分**(elite だけ behavior を足す等)は v1 では扱わない:
答えは**別プレファブ**である。optional slot / variant は、
構造差分の需要が実測されてから設計する(未決 §13)。
**v1 の正直な射程: 「値のパラメータ化 + 資産差し替え」。それを明記して出荷する。**

## 8. 昇格と dry-run(U3。authoring transaction 基盤の後)★

- **前提基盤: `AuthoringDocumentKey` と文書別 revision/digest**。
  現行 journal は単一 SceneRevision 前提で、prefab 文書に revision が無い。
  昇格(2 文書編集)・クラッシュ復旧・undo の単位はこの基盤の上でしか成立しない
- **dry-run は allocator を一切呼ばない純粋 preflight**。
  現行 `prepareBatch` は前段の `normalizeBehaviorAttachmentIdentities` が
  attachment handle を実際に消費する(v1 の「構造に乗る」は偽だった)。
  受け入れ: **0 回 dry-run と N 回 dry-run の後の本 commit が、
  ID・journal・runtime trace までバイト一致**

## 9. spawn(U4。group lifecycle の後)★

- **`SpawnedPrefabHandle`(group owner)を導入する**: 全 object/behavior の prepare →
  atomic publish、group destroy、`onInit` 失敗時の全体 rollback。
  これ無しでは多 object spawn の孤児(root だけ消えて identity 親で生き残る
  localtransform 等)を防げない
- 基盤を作らない選択をするなら S4 は単一 object 限定と明記する

## 10. 出荷単位(レビューの再編成を採用)★

```
U1  文書 + registry + version 2 ゲート + 単一 object 展開 + 依存指紋 + エラーコード
U2  多 object(全件 named / root 規約 / リンカ / 展開上限)+ エディタ表示 + 合法 unpack
    ※ 前提: parent 機構の実戦投入(出荷 scene での使用が今日 0 件)
U3  authoring transaction 基盤 → パラメータ昇格 + 純粋 dry-run + 一括取り込み面
U4  group lifecycle → 決定的 spawn
```

各 U の受け入れ条件(骨子。WP 化時に §4 規約 10 の形へ):

- U1: プレファブ無し / default / 束縛あり の 3 者を同じ本番ロードで実行し、
  **実 ECS / behavior が解決した値**を検査。version 2 を旧 validator が拒否すること。
  snapshot の依存不一致拒否。未使用宣言・opaque 束縛・型違反の名前付き拒否
- U2: 2 インスタンス + 内部 `{"$object"}` + 外部 parent + instance transform を同時検査。
  unpack → save → 新プロセスで reload → undo → redo が完走
- U3: 0 回対 N 回 dry-run のバイト一致。全 cross-document 失敗点からの復旧
- U4: 多 object spawn の全体 destroy / 途中失敗 rollback / リプレイ trace 同一

## 11. 将来拡張(v1 から変更なし)

HDA: authoring 時 cook → stamp(eject 系)。parm interface と parameters の写像。
attributes: behavior params 強化(入れ子・配列)の後に要否判断。
ジオメトリ属性はレンダラ ABI の別領域。

## 12. v1 レビューで壊れた点(記録)

| # | v1 | どう壊れたか | v2 の答え |
|---|---|---|---|
| 1 | version 据え置き | 旧エンジンが prefab scene を**黙って欠落表示** | scene version 2 ゲート |
| 2 | 鋳造名で unpack | `/` 入り名は R7 違反で**保存不能** | runtime 名と unpack 名を分離 |
| 3 | parent だけ書き換え | behavior の内部参照が relocate されず誤結合 / root 規約なし / 無名 object | 全件 named + root 指定 + `{"$object"}` + リンカ |
| 4 | scalar/asset のみ | enum/vec/quat/string が調整不能でプレファブ増殖か unpack 強制 | StructFieldSchema を正準採用。構造差分は正直に保留 |
| 5 | 文字列 `"$name"` | リテラルと衝突 / no-op 束縛が検証を通る | tagged node + opaque 束縛禁止 + 未使用宣言禁止 |
| 6 | 指紋なし | 同一 scene bytes + 別 registry が検出不能 | snapshot closure digest + save read-set |
| 7 | 昇格を S3 に | 文書別 revision / undo 単位 / 復旧が存在しない | authoring transaction 基盤を U3 の前提に |
| 8 | dry-run は「乗る想定」 | 現行配線は preflight で handle を消費する(前提が偽) | 純粋 preflight + 0対N バイト一致条件 |
| 9 | spawn に群の概念なし | 多 object の rollback / 一括 destroy が不可能 | SpawnedPrefabHandle か単一 object 限定 |

レビューが壊せなかった点(独立確認): runtime 鋳造名の対衝突 / 再帰禁止の封じ /
spawn が save を汚染しない / D0 は正しく分ければ成立 / 値調整用途での S0 の有用性。

## 13. 未決事項

1. 入れ子プレファブ(再帰鋳造と循環検出)
2. optional slot / variant(構造差分。需要の実測後)
3. authoring transaction 基盤の詳細設計(U3 の前提。別文書に値する規模)
4. attributes 層の要否
5. HDA live cook
