# プレファブ設計 — パラメータ付きコンポーネント束(v4)

状態: **v4。設計レビューはここで打ち切る**(v1: 9 指摘 / v2: 11 指摘 / v3: 10 指摘。
v2・v3 の各第 1 指摘はレビューテンプレートに残っていた別設計の目的による誤指摘で、
テンプレート側を修正済み。有効指摘は全て本文に反映し、残余は各 U の WP 仕様レビューで受ける)。
末尾 §10 に v3 有効指摘への答えを残す。

## 0. 要求と原則

利用者要求: インスタンス前のコンポーネント束の再利用 / インスタンス調整の軽量化 /
AI・外部データからの一括流し込み / 将来の HDA・attribute。

原則(v3 までに確定したもの):

- 参照か、明示 unpack か。半連結の上書き追跡は作らない
- 上書き面は作者が宣言したパラメータだけ。discriminant
  (必須 field 集合を変える enum: `collider.shape` / `camera.type` / `light.type`)は束縛不可
- 型系は `StructFieldSchema` を正準に使う(型語彙は実在のもの: `i32` / `f32` / enum / vec…)
- 置換の順序: raw prefab 上で placeholder 検証 → 置換 → **具体値を codec へ**。
  codec は tagged node を見ない
- 範囲は**単一オブジェクト**。多オブジェクトは §7 の別設計
- 展開は load 時。fail-fast

## 1. 実装の背骨: ResolvedScene seam(v3 レビューの中心指摘への答え)

**オブジェクトの 1:1 は保てても、component は 1:N に展開される**
(著作 1 component(transform)+ prefab 由来 N)。そして現行は
**生の scene JSON を読む消費者が複数ある** —— `SceneLoader` だけでなく、
**`Camera` subsystem は `sceneDocument().scenesJson()` を自分で再走査していた**
(v4 執筆時点の `camera.cpp:634,677`。**WP360 で解消済み** —— 現 HEAD の Camera は
ResolvedScene を読む。本節は seam を要求した歴史的根拠として残す)。editor runtime query は著作 component 数で配列を確保し
同じ添字で runtime 値を対応させる(`editorruntimefactory.cpp:1044`)。

**したがって展開は「SceneLoader の中」ではなく、名前のある単一 seam にする:**

```
(AuthoringSceneDocument, PrefabRegistrySnapshot)
    → ResolvedScene(展開済み objects/components)+ provenance map
      (component ごとに: authored | prefab(ref, digest, prefab 内 index))
```

- **全消費者がこれを読む**: SceneLoader / Camera / Light / behavior reload validator /
  editor projection / RPC query。**生 JSON の直接読取りを本 seam に置換することが
  U1 の実装本体である**(Camera の生読みは既存の潜在負債であり、ここで解消する)
- editor の component 列挙は provenance 付きで返る(著作 / prefab 由来の区別、
  prefab 由来は読み取り専用 + パラメータへの誘導)

## 2. プレファブ文書と束縛可能性

```json
{ "schema": "pelican.prefab", "version": 1, "name": "enemy_grunt",
  "parameters": [
    { "name": "hp",     "type": "i32",  "range": [1, 9999], "default": 30 },
    { "name": "tint",   "type": "vec4", "default": [1,1,1,1] },
    { "name": "body",   "kind": "asset", "asset_kind": "model", "default": "sotai" },
    { "name": "target", "kind": "object", "required_components": ["transform"] } ],
  "components": [
    { "name": "simplemodelview", "model": { "$param": "body" } },
    { "name": "sprite_view", "texture": "white", "color": { "$param": "tint" },
      "size": [1,1], "pivot": [0.5,0.5] },
    { "name": "collider", "shape": "capsule", "radius": 0.4 },
    { "name": "behavior", "type": "grunt_ai",
      "params": { "hp": { "$param": "hp" }, "target": { "$param": "target" } } } ] }
```

- **prefab は `transform` を含めない(禁止)。**instance が exactly-one の transform を
  持つ(配置)。資産固有の補正変換(intrinsic transform)は v1 非対応と明記する
  —— 対応するなら `parent × instance placement × prefab intrinsic` の合成設計が要る(§7)
- **`kind: "object"` パラメータ(外部オブジェクト参照)を v1 に含める。**
  camera の follow target / AI の標的は単一オブジェクトでも必要
  (`camera.controller.target` が実在の根拠)。
  **展開後に存在・一意性・`required_components` を検証**(fail-fast)。
  rename との連動は U2 の picker と同時。壊れた参照は load 検証で名前付きに落ちる
- **束縛可能性は `BindableProvider` が決める(静的 inventory では足りない):**
  - component codec 側: bindable-path(JSON pointer + 型 + 種別)に加え、
    **applicability(固定 discriminant の値に条件づく)**を持つ ——
    `shape: "box"` の prefab で `/radius` に束縛したら**エラー**(runtime が読まない)
  - **behavior 側は codec ではない**: DLL の `BehaviorRegistration`
    (params schema + fingerprint)から供給する。**両者を同じ provider interface に統合**し、
    provider は fingerprint / generation を持つ(reload 跨ぎの stale 検出)
- **default の省略は「instance で必須」の意味**(規範例の `target` がこれ)。
  instance が値を与えなければ `prefab_parameter_required`。scalar/asset/object の別を問わない
- 文法の閉包: prefab 名・parameter 名の重複、非 behavior component の重複、
  未知 instance parameter は全て名前付きエラー(§6 の code 一覧)

## 3. scene 側: version 2 ゲートと互換 matrix

- prefab を使う scene は `version: 2`。gate と expander は同一 capability で原子的に
- **matrix(v3 の silent no-op セルを修正):**
  - 旧 engine + v2 → 拒否(成立済み)
  - 新 engine + v1(prefab キー無し)→ 従来どおり
  - **新 engine + v1 + `prefab` キー → `prefab_requires_scene_v2` で拒否**
    (黙って transform-only object にしない。v1 で `prefab` を独自メタデータに
    使っていた場合は壊れるが、fail-fast を優先する —— 出荷・fixture に使用例は無い)
  - 新 engine + v2 + prefab → 展開し、**実 ECS / behavior の解決値**を検査
- 全入口(engine load / studio project open / RPC snapshot import)が同じ分岐を通ること

## 4. 依存指紋: SnapshotV2(閉包を解決依存まで広げる)

- closure は `{prefab_name, digest}` だけでは**不足**: prefab bytes が同一でも
  behavior DLL の default 変更で解決値が変わる。**closure に加えるもの:**
  - prefab 内 behavior の `{stable_name, schema_version, params_schema_fingerprint}`
  - BindableProvider の semantics fingerprint(codec 側の束縛意味の版)
- read-set は現在 scene だけでなく**文書中の全 scene**を純粋 resolver で走査して作る
- Snapshot V1 は prefab 非参照 scene に限って維持。request / response / import の
  三 schema を同時に V2 化(正式手順どおり)
- `save_scene` baseline は `{scene_digest, prefab read-set}` の不可分 snapshot

## 5. エディタ: 新しい編集 op(既存 op には載らない)

**instance のパラメータ編集は `set_component_value` の拡張ではない**
(component_slot を要求し、既存 field を要求し、生 `components[]` を探す)。

- **`set_prefab_parameter` / `unset_prefab_parameter`** を新設:
  対象は `/objects/<id>/prefab/parameters/<name>`。
  commit は次の authoring 文書を**再展開**し、resolved component の差分を
  failure-atomic に適用する。request は scene revision に加えて
  **prefab digest と provider generation** を持つ(stale 編集の拒否)
- 表示: instance 選択で「解決値 + 出所(default か束縛か)」。prefab 由来 component は
  読み取り専用表示 + 「このパラメータで変わる」への誘導
- unpack は journal 1 op(prefab キー → 展開済み components への置換。名前不変)

## 6. エラーコード

`prefab_not_found / prefab_version_unsupported / prefab_requires_scene_v2 /
prefab_duplicate_name / prefab_parameter_unknown / prefab_parameter_duplicate /
prefab_parameter_type / prefab_parameter_unused / prefab_binding_not_bindable /
prefab_binding_inactive(discriminant で無効な path)/ prefab_object_ref_unresolved /
prefab_object_ref_missing_component / prefab_transform_forbidden /
prefab_dependency_mismatch / prefab_nested_unsupported / prefab_provider_stale /
prefab_parameter_required / prefab_instance_id_collision / prefab_generated_read_only /
prefab_generated_id_collision / prefab_instance_transform_missing /
prefab_instance_transform_duplicate / prefab_component_duplicate /
prefab_name_mismatch / prefab_persistence_unsupported / prefab_path_invalid /
prefab_registry_invalid / prefab_document_invalid / prefab_instance_invalid /
prefab_instance_id_invalid / prefab_component_key_duplicate`。
文脈: scene_id / instance / prefab / parameter / JSON pointer / (取込時)行 index。

## 7. 出荷単位と、多オブジェクトへの送り

```
U1  leaf library(型語彙 + placeholder 文法 + 文書検証)+ ProjectEnvelope の prefabs 保持
    + PrefabRegistrySnapshot + ResolvedScene seam(全消費者の付け替え込み)
    + version 2 ゲート + BindableProvider + SnapshotV2 指紋
    + studio: 表示(解決値 + 出所)と set_prefab_parameter 編集
U2  unpack + object picker(rename 連動)+ 一括取り込み面 + 純粋 dry-run
U3  authoring transaction 基盤(文書別 revision)→ パラメータ昇格
U4  ctx.spawn(単一 object。1 spawn = 1 entity、リプレイ trace 同一)
```

U1 が大きいのは意図的である —— ResolvedScene seam が実装本体であり、
それ無しの「engine だけ展開」は Camera に見えない component を作る(v3 レビュー実証)。

**多オブジェクト(車 + 車輪)は別設計**。前提: `(instance AuthoringObjectId,
internal name)` composite key の RPC 貫通 / root・forest 規約と変換合成式 /
単射な unpack 命名 / `parent` の実戦投入。

## 8. D0 の層割り(所有者まで名指し)

- **新 leaf library(`pelican_project` 配下または独立)**: `StructFieldType` の型 enum・
  文字列変換・parameter JSON 検証・placeholder 文法・prefab 文書構文。
  **core の `StructFieldSchema` はこれを使う側に回る**(現行は core 所有で
  studio から届かない —— 変換 helper が core の RPC 実装内にあるのを移す)
- `ProjectEnvelope` が `prefabs[]` を保持し、engine / studio が同じ parsed envelope から
  immutable `PrefabRegistrySnapshot` を作る
- BindableProvider の実体(codec + behavior)は core。**studio へは RPC で inventory を
  JSON 公開**。engine 不在時の studio は構文検証まで(束縛妥当性は「未検証」表示)

## 9. 将来拡張

HDA: authoring 時 cook → stamp。attributes: behavior params 強化の後。
多オブジェクト: §7 の前提が揃ってから。

## 10. v3 有効指摘(2〜10)への答え

| # | 指摘 | v4 の答え |
|---|---|---|
| 2 | component の 1:N 未設計。Camera は生 JSON を再走査 | ResolvedScene seam(§1)。全消費者を付け替え、U1 の本体に |
| 3 | 静的 inventory では applicability と behavior を表せない | BindableProvider(§2): codec + behavior 統合、discriminant 条件つき、fingerprint 持ち |
| 4 | parameter 編集が set_component_value に載らない | set_prefab_parameter / unset を新設(§5)。再展開 + failure-atomic |
| 5 | transform 規則が自例と矛盾、sprite/model が黙って消える | prefab は transform 禁止、instance に exactly-one 必須。intrinsic は v1 非対応と明記 |
| 6 | 単一 object でも外部参照が要る | kind:"object" パラメータ(§2)。展開後検証、picker は U2 |
| 7 | v1 + prefab キーが silent no-op | prefab_requires_scene_v2 で拒否(§3) |
| 8 | closure が解決依存を覆わない | behavior schema fingerprint + provider fingerprint を閉包に(§4) |
| 9 | 正準型の所有者不在 | leaf library へ移設、ProjectEnvelope が prefabs を保持(§8) |
| 10 | registry 文法と error の閉包不足 | §2 末尾 + §6 の code 一覧 |

3 巡で壊せなかった点: v2 拒否ゲート / 置換順序(7 codec で成立) /
単一 object 化による鋳造・root・1:N(object)の解消 / U2 の op 順序 /
D0 境界方針 / U4 の条件の非代理性。

## 11. 未決事項

1. 多オブジェクト(§7 前提 4 件)
2. optional slot / variant(構造差分。需要の実測後)
3. authoring transaction 基盤の詳細(U3 前提。別文書)
4. intrinsic transform の合成(多オブジェクト設計と同時)
5. attributes 層 / HDA live cook
