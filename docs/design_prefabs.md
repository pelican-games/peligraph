# プレファブ設計 — パラメータ付きオブジェクト束(v1 草案)

状態: **草案。codex 設計レビュー前。**
先行文書: `design_scene_format.md`(§4 未決事項 2 が prefab を保留した)、
`design_object_behaviors.md`、台帳の設計ノート 3 本(2026-08-28)。

## 0. 要求と原則

利用者要求:
1. インスタンス前のコンポーネント束の再利用(プレファブ)
2. インスタンスしたコンポーネントの調整がゲーム制作の大半 —— そこを軽くする
3. AI・外部データからの複数コンポーネントへの一括流し込み
4. 将来: Houdini HDA のような手続き的資産の取り込み、attribute 的な自由データ

設計原則(すべて既存の家風から導出。趣味で発明しない):

- **参照か、明示 unpack か。半連結の上書き追跡は作らない**
  (render preset の copy/eject、physical fragment の一方向 eject と同じ)
- **上書き面は作者が宣言したパラメータだけ**(縛りは案内)
- **文法は 1 つ**: プレファブ文書の中身は scene の object 文法そのもの。
  パラメータ封筒は render feature の `parameters` と同じ 3 部形式に揃える
- **名前は鋳造する。置換しない**(view 系統複製の実測教訓)
- **展開は load 時。runtime はインスタンス同一性を持たない(v1)**
- **fail-fast**: 未知参照・未宣言パラメータ・型/範囲違反・循環は名前付きエラー

## 1. プレファブ文書

```json
{
  "schema": "pelican.prefab",
  "version": 1,
  "name": "enemy_grunt",
  "parameters": {
    "schema": "pelican.prefab_parameters",
    "version": 1,
    "scalars": [
      { "name": "hp",     "type": "int",   "range": [1, 9999], "default": 30 },
      { "name": "speed",  "type": "float", "range": [0.0, 50.0], "default": 3.5 }
    ],
    "assets": [
      { "name": "body_model", "kind": "model", "default": "sotai" }
    ]
  },
  "objects": [
    { "name": "root",
      "components": [
        { "name": "transform" },
        { "name": "simplemodelview", "model": "$body_model" },
        { "name": "collider", "shape": "capsule", "radius": 0.4 },
        { "name": "behavior", "type": "grunt_ai", "params": { "hp": "$hp", "speed": "$speed" } }
      ] }
  ]
}
```

決定:

- **`objects[]` は scene と同一文法**(`sceneformat.cpp` の検証をそのまま流用)。
  第二の文法を作らない
- **`$name` は完全一致置換のみ**(render feature と同じ。演算なし。
  型は parameter の JSON 型を保存する)
- パラメータ種別は v1 で 2 つ: `scalars`(float/int/bool。behavior params の
  型系に一致)と `assets`(asset_data の登録名を指す。`kind` で表を選ぶ)。
  render feature の `render_targets` パラメータに相当する枠だが、scene 領域では
  資産参照がそれに当たる
- **置き場所は project.json**(`prefab_data_json` または `prefabs[]`)。
  著作文書(scene/camera/material/animgraph)は project.json、
  メディア資産は asset_data.json という既存の線に従う

## 2. インスタンス参照(scene 側)

```json
{ "name": "grunt_01", "parent": "spawn_area",
  "components": [ { "name": "transform", "pos": [10, 0, 3] } ],
  "prefab": { "ref": "enemy_grunt", "parameters": { "hp": 45 } } }
```

決定:

- インスタンスは**通常の scene object + `prefab` キー**。
  同居できる components は **root の transform だけ**(配置はインスタンス所有であって
  上書きではない。Unity も root transform は instance 所有)。
  それ以外の components 同居は名前付きエラー
- **パラメータ束縛は宣言された名前だけ。**未宣言名・型違反・範囲外は load 時に
  名前付きエラー(render feature の束縛検査と同じ位置づけ)
- **入れ子(プレファブ文書内の prefab 参照)は v1 で禁止**し、名前付きエラー。
  v2 の課題として記録(循環検出と鋳造の再帰が必要になる)

## 3. 展開と名前の鋳造

- 展開位置: `SceneLoader::load` の先頭、`prepareSceneBindings` の前。
  authoring 文書は参照のまま(編集の真実)、runtime は素の objects を見る
- **鋳造規則: 内部オブジェクト名は `インスタンス名/内部名`。**
  著作名の文字クラスは `[a-zA-Z0-9_]` なので **`/` は著作名に現れ得ず、
  衝突が構造的に起きない**。鋳造名は著作不能(パーサが `/` 入りの著作名を拒否)
- **サブツリー内部の `parent` 参照は鋳造名に書き換える**(内部の辺の付け替え)
- 展開順は文書順。決定的
- 単一オブジェクト(S0)では鋳造は退化する(root がインスタンス名をそのまま使う)

## 4. エディタ

- **表示**: インスタンスは Outliner で 1 ノード + 展開で鋳造された子(読み取り専用)。
  frame plan の provider_feature / WP341 グループ表示と同じ発想。
  鋳造された子には provenance(どのプレファブの何か)を出す
- **unpack**: 明示・一方向の編集 op。展開と同じコードパスで素の objects に変換し、
  journal に載せる(undo 可能)
- **パラメータ昇格**: インスタンス上の「このフィールドを変えたい」操作 1 回で、
  (a) プレファブ文書にパラメータ宣言を追加、(b) インスタンスに束縛を追加。
  **2 文書にまたがる編集**なので、プレファブ文書の保存が失敗したら scene 側を
  変更しない(順序: プレファブ先、scene 後)。原子性の詳細は実装設計で詰める(未決)

## 5. 一括流し込みと AI 編集

土台は既存(調査で確認済み):
`edit` の batch(all-or-nothing、逆操作で 1 undo)、機械可読スキーマ
(`get_components` の schema_fields)、op ごとの名前付き検証、外部 RPC クライアント。

追加するもの:

- **プレファブパラメータ = 取り込み契約。**表データ 1 行 = インスタンス 1 個の束縛。
  外部データは任意のフィールドパスではなく宣言された表に流す
  (プレファブ改訂で黙って壊れず、束縛検査で名前付きに割れる)
- **batch の dry-run**(検証のみ、commit しない)。AI エージェントのループを安全にする。
  `prepareBatch` が準備と commit を分けている構造に乗る想定(実装設計で確認)
- studio の取り込み面(表 → 束縛 batch の変換とプレビュー)は S3

## 6. runtime spawn(S4)

- `ctx.spawn("enemy_grunt", params)` — registry から決定的に生成。
  鋳造名は決定的(scene epoch 内の連番)。リプレイの時系列に乗る
- 生成物は runtime 状態(authoring 文書に入らない。behavior が今日 `GameObjects::add`
  で作るのと同じ位置づけ。`save_scene` を汚染しない)

## 7. Unity と意図的に変える点

台帳の設計ノート(2026-08-28)の表を正とする。要点:
上書き面は宣言パラメータのみ + 明示 unpack / 参照は名前 + fail-fast /
コードとデータの分離維持 / spawn は決定的 / 取込は宣言的登録のみ / 文法は 1 つ。

失うもの(正直に): 「後からその場で、予測していなかったフィールドを変える」自由。
緩和: パラメータ昇格(§4)と unpack。残る本物の損失は一度きりの雑な微調整で、
そこは unpack が受け皿。

## 8. 将来拡張(v1 では実装しない。設計の穴だけ空けておく)

### 8.1 Houdini HDA

HDA = 手続きノード網 + 宣言されたパラメータ面、という構造は
**プレファブ文書 + parameters と同型**。対応は 2 段階が考えられる:

- **authoring 時 cook → bake(eject 系)**: Houdini Engine で cook した結果
  (メッシュ・インスタンス群)を資産として登録し、プレファブ文書に stamp する。
  runtime に Houdini 依存を持ち込まない。決定的・purgeable。家風に一致
- HDA の parm interface → prefab parameters への写像は素直
  (両方とも型付き宣言パラメータ)
- live cook(編集中の再 cook)は将来。**隠れキャッシュを作らない**制約だけ先に固定

### 8.2 attribute 的な自由データ

Houdini の attribute(要素に付く名前付き型付き値)の scene 版は
「オブジェクトに付く open な型付き key-value」。**closed-schema の家風と緊張する**ので
v1 では作らないが、住み分けの構図だけ固定する:

```
宣言された層   prefab parameters / behavior params   契約。検証される。取り込み面
自由な層       attributes(将来)                     探索用。型は値が自己申告
```

- 需要が先に立つのは behavior params の強化(入れ子・配列)である可能性が高い。
  そちらで足りるなら attributes は不要 —— **params 強化を先に、attributes は
  その後に判断**
- ジオメトリレベル(点・頂点)の attribute はレンダラの ABI の話であり別領域
  (material/shader 側。ここでは扱わない)

## 9. 実装の段(各段が挙動を変えること)

```
S0  文書 + registry + load 時展開(単一オブジェクト)+ 束縛検査     engine
S1  多オブジェクト + 鋳造 + 内部 parent 書き換え                    engine
    ※ 前提: parent 機構を出荷シーンのどれかで実戦させる(使用 0 件のため)
S2  エディタ表示(1 ノード + 読み取り専用の子)+ unpack op          engine RPC + studio
S3  パラメータ昇格 + 一括取り込み面 + batch dry-run                 engine RPC + studio
S4  ctx.spawn(決定的 runtime 生成)                                 engine
```

## 10. 未決事項

1. 入れ子プレファブ(v2。循環検出と再帰鋳造)
2. パラメータ昇格の 2 文書原子性の詳細
3. `prefab_data_json`(一括)か `prefabs[]`(個別列挙)か
4. attributes 層の要否(behavior params 強化の後に判断)
5. HDA live cook の形
