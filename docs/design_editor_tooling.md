# 編集系 rpc とエンジン内インスペクタ/アセットブラウザ(v1)

対象読者: エンジン担当・エディタ/ツールを使う人。
ステータス: v1 ドラフト(2026-07-17。敵対レビュー前)。
前提: `design_devstudio_direction.md` D0(エディタ特権禁止 — 編集系
rpc が本当の API)、WP27/42(stdio JSON-RPC・get_status/
update_transforms/load_gltf)、WP85/86(ImGui・Plan viewer の
エンジン内ツール流儀)、scene v1・ComponentInfoManager、
アセット HR(ファイルが唯一の真実)、WP144(load_gltf transaction)。

## 0. 三層(D0 の適用)

| 層 | 内容 | 帰属 |
|----|------|------|
| **編集意味論(E-RPC)** | scene query/edit・asset query の rpc メソッド群 | エンジン(版付き・これが唯一の編集 API) |
| **エンジン内 UI** | ImGui インスペクタ/アウトライナ/アセットブラウザ | エンジンツール(**E-RPC と同一の内部関数を呼ぶ** — 裏口禁止) |
| **外部クライアント** | devstudio(Qt)・pelican_rpc.py・エージェント | 公開プロトコルのみ |

原則: ImGui パネルは「E-RPC ハンドラが呼ぶのと同じ関数」だけを呼ぶ。
パネルにしかできない操作を作らない(作りたくなったら rpc メソッドを
先に足す)。

## 1. E-RPC v1 — メソッド群

### 1-1. query(読み取り — 決定性に無害)

- `scene_tree` → objects[] の {name, parent, entity_id{index,gen},
  components[](name のみ)} の階層
- `get_components(object)` → component ごとの **typed 値の JSON**
  (§1-3 の反射で)
- `list_assets(store?)` → 宣言済み asset {id, kind, path, status
  (loaded/missing/reloading)}。HR の watcher 状態(get_status.reload)
  と整合
- 既存: get_status / get_frame_plan は現行のまま

### 1-2. edit(書き込み — フレーム境界適用・journal 化)

- `set_component_value(object, component, path, value)` —
  §1-3 の型検証付き
- `add_component(object, component, values?)` /
  `remove_component(object, component)`
- `spawn_object(name, parent?, components)` /
  `destroy_object(object)` / `reparent(object, new_parent)`
- 全 edit は **EditCommand として journal に積まれ、次フレーム先頭で
  一括適用**(FrameInput/E1 と同じ位相規律)。適用結果(成功/名前入り
  エラー)を応答
- **journal が D3 undo の土台**: command は {op, target, before, after}
  を持ち逆適用可能な形で記録(undo 実装自体は D3 — v1 は形だけ規範化)

### 1-3. コンポーネント値の反射(インスペクタの土台)

- 正本 = **scene v1 の component JSON**。既に全 component は
  ComponentInfoManager 経由で JSON から構築できる —
  逆方向(現在値 → JSON)の serialize を component 登録に追加
  (WP62 の typed 登録で serializer は optional 化済み —
  **serializer を持つ component だけが編集可能**。持たないものは
  インスペクタで read-only 表示 + 「編集不可」明示)
- 値の型・range・enum 候補は component 側の宣言(additive な
  schema key)。未宣言は型のみ検証

### 1-4. 決定性との関係

- リプレイ/strict 中は edit 系を名前入り reject(HR gate と同じ
  中央 gate を購読)
- edit は input_seq に記録**しない**(編集はゲーム入力ではない)。
  編集を含む再現が要る場合は「編集後に保存 → シーンから再起動」が正
  (ファイルが唯一の真実)

## 2. エンジン内 UI(ImGui — F2 で開閉)

### 2-1. アウトライナ + インスペクタ

- アウトライナ: scene_tree と同じデータの木。選択で
  インスペクタに get_components 相当を表示
- インスペクタ: 値編集は set_component_value と同じ関数へ。
  型に応じたウィジェット(float ドラッグ・vec3・color・enum combo・
  bool)。read-only component はグレーアウト
- **選択の相互運用**: 選択 entity_id を get_status に出す(将来の
  D2 ピッキングと同じ語彙)

### 2-2. アセットブラウザ

- list_assets と同じデータ: store 別ツリー・kind フィルタ・
  status バッジ(HR の reloading/missing)
- v1 は**閲覧 + 参照コピー**(asset id をクリップボードへ)まで。
  ドラッグ &ドロップ配置は D2(ピッキングと同時)
- import manifest(pelican.import)がある asset は provenance
  (ツール・version・source)をツールチップ表示

### 2-3. 保存(v1 の割り切り)

- 「Save Scene」ボタン = journal 適用済みの現在シーンを
  **pelican.scene v1 として書き出し**(round-trip の第一歩 —
  完全な D3 round-trip 保証は別 WP。v1 は「書き出せる component のみ
  警告付きで」)
- 書き出し先はプロジェクトの scene ファイル(HR が拾って
  次回ロードと一致 — ファイルが唯一の真実)

## 3. WP 分割

| WP | 内容 | gate | 依存 |
|----|------|------|------|
| **E-RPC0** | §1-1 query 群 + §1-3 serialize(optional serializer の活用) | rpc fixture(tree/components の決定的 JSON)・serializer なし component の read-only 表示情報 | WP42/62(済) |
| **E-RPC1** | §1-2 edit 群 + journal + フレーム境界適用 + gate 連動 | edit 正負 fixture・リプレイ中 reject・2 回実行一致(query のみ) | E-RPC0 |
| **UI-INS0** | §2-1 アウトライナ/インスペクタ(ImGui) | E-RPC と同一関数の呼出であることの検査(裏口 grep fixture)・スクリーンショット | E-RPC0/1 |
| **UI-AB0** | §2-2 アセットブラウザ | list_assets fixture・HR status 連動 | E-RPC0 |
| **SAVE0** | §2-3 シーン書き出し | 書き出し → 再ロードで scene_tree 一致(round-trip の弱い版) | E-RPC1 |

## 4. 未決事項

1. 選択・ギズモ(D2)のピッキングは ID バッファ render feature —
   本設計の外(devstudio 文書の既定どおり)
2. undo/redo の実装時期 — journal の形だけ v1 で固定し、D3 で実装
3. WebSocket 展開 — E-RPC が固まってから(メソッドは transport 非依存)
4. behavior(スクリプト)の params 編集は
   `design_object_behaviors.md` の反射に相乗り
