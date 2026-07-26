# 外部ツール群への設計要求書

対象読者: 動画作成ツール群(物理シミュレーション、ポーズ推定など)のプロジェクトで作業する人間およびコーディングエージェント(Claude / Codex 等)。

この文書は**自己完結**している。ツール側のリポジトリにコピーして使ってよい。**正本は pelican2 リポジトリの `docs/external_tools_requirements.md`(本ファイル)**であり、各ツールリポジトリのコピーは参照用。変更は正本を更新してから各コピーへ同期する。

## 0. 背景(これだけ読めば文脈が分かる)

ツール群の主目的は Blender 等での動画作成支援だが、同じ成果物を自作ゲームエンジン **Pelican2**(C++/Vulkan)でも利用する。Pelican2 側には以下が整備される予定:

- ヘッドレス描画(ウィンドウなしで PNG / 連番画像を出力)
- JSON-RPC ベースのテキストコマンド層(`load_gltf` / `set_time` / `step_frame` / `render_frame` / `capture`)
- ベイク済み transform 列の一括再生 API

つまりツール側は「標準フォーマットのファイルを書き出す」か「テキストプロトコルでエンジンに流し込む」かのどちらかができれば、Blender とエンジンの両方で成果物が使える。

## 1. アーキテクチャ要求(最重要)

**R1: コアをホスト非依存に作ること。**

```
tool-core (純粋ライブラリ: シミュ/推定ロジック、I/Oなし or ファイルI/Oのみ)
 ├─ tool-cli      (CLI。すべての機能がコマンドラインから叩ける)
 ├─ blender-addon (薄いアダプタ。UI と bpy 変換のみ。ロジック禁止)
 └─ (将来) pelican2-adapter (薄いアダプタ)
```

- Blender アドオンとしてしか動かない実装は不可。`bpy` への依存は addon 層に隔離する
- すべての機能は CLI から再現可能であること(エージェントがヘッドレスで開発・テストするための条件でもある)
- CLI の入出力は「ファイル + 終了コード + stderr ログ」を基本とし、対話状態を持たない

**R2: 決定性。** 同じ入力 + 同じシード + 同じバージョン → バイト一致でなくてよいが数値的に同一の出力。乱数はシード必須引数、時刻依存・環境依存の挙動を持たないこと。回帰テストの前提。

## 2. データフォーマット要求

**R3: 交換のハブは glTF 2.0。**

- ジオメトリ、スケルトン、スキンウェイト、アニメーションクリップは glTF (.glb) で出力する
- ポーズ推定の出力は「glTF のスケルタルアニメーションクリップ」に正規化する(独自の関節フォーマットを作らない。中間表現として持つのは可だが、外に出すのは glTF)

**R4: 剛体シミュレーション結果は「時刻サンプル付き transform 列」。**

glTF アニメーションとして表現できる場合はそれを優先。glTF に乗せにくい大規模な結果用に、以下の JSON Lines 形式を併用してよい:

```jsonl
{"schema": "pelican.transform_seq", "version": 1, "fps": 30.0, "objects": ["box_0", "box_1"]}
{"t": 0.0,    "transforms": [{"pos": [0,0,0], "rot": [0,0,0,1], "scale": [1,1,1]}, ...]}
{"t": 0.0333, "transforms": [...]}
```

- 1 行目がヘッダ(schema/version 必須)。rot はクォータニオン xyzw
- objects の並びと transforms の並びは対応。全フレームで全オブジェクトを出す(スパース化は version 2 以降の課題)
- 可視性(2026-06-12 追補): フレーム行のオプションフィールド `"hidden": [3, 17]`(ヘッダ `objects` 配列の添字リスト)でそのフレームの非表示オブジェクトを指定する。省略時は全可視。非表示オブジェクトの transforms にも有効な値(直前値の保持で可)を入れること。**scale `[0,0,0]` による非表示表現は採用しない**(法線行列が特異になる、culling/bounds/物理と意味が混線する、「見えない」と「存在しない」の区別が消えるため)

**R5: 頂点キャッシュ(布・流体・ソフトボディ)。** Blender レーンは Alembic を第一候補とする(Blender ランナーが直接書く)。Pelican2 レーンは **`pelican.vat` v1**(Vertex Animation Texture)を用いる(2026-06-12 追補)。いずれも必要になるまで実装しない。

`pelican.vat` v1 の要点(詳細仕様と根拠は pelican2 の `docs/dcc_integration_qa_2026-06-12.md` §6):

- ベース glb の POSITION(と任意で NORMAL)を置換する再生方式。トポロジ・インデックス・UV・頂点数・頂点順序はベース glb と完全固定
- 位置テクスチャ: 幅 = 頂点数(v1 上限 8192、超過時はベイカー側でメッシュ分割)、高さ = フレーム数、RGBA16F、`(pos - bounds_min) / (bounds_max - bounds_min)` で正規化。座標はメッシュノードのローカル空間(glTF 規約)
- サンプリングは NEAREST + シェーダ内で隣接 2 フレーム行を手動 lerp(ハードウェア線形フィルタは頂点方向に滲むため禁止)
- テクスチャ実体は glb バッファ内の bufferView として格納(EXR/KTX2 等の追加フォーマット依存を作らない)。メタは**対象 mesh primitive の extras** `pelican.vat`(schema/version/generator/fps/frame_count/vertex_count/bounds_min/bounds_max/loop/position_view/normal_view?)。**1 primitive につき 1 クリップ**(複数クリップは v1 禁止、別 glb にする。VAT 付き primitive が 1 glb に複数あるのは可)

## 3. 座標系・単位規約

**R6: ファイルに書き出す時点で glTF 規約に統一する。**

- 右手系、+Y up、-Z forward、単位はメートル、時間は秒
- Blender(+Z up)からの変換は**ツール側エクスポータの責務**。Pelican2 側インポータは glTF 規約のデータが来る前提で書かれる
- クォータニオンの格納順は glTF に合わせて xyzw

**R7: 命名。** オブジェクト/ジョイント名は `[a-zA-Z0-9_]` のみ、スペース・日本語・記号不可(プロトコルとファイルパスの両方で安全にするため)。

## 4. エンジン連携プロトコル(将来の直結用)

**R8: ライブ連携は JSON-RPC 2.0。フレーミングは NDJSON(1 行 = 1 オブジェクト、改行を含まない UTF-8 JSON)。トランスポートは stdio を先行し、TCP(localhost)は後続。**

Pelican2 が公開予定のコマンド(ツール側はこれを呼ぶクライアントになる):

| メソッド | 概要 |
|----------|------|
| `load_gltf {path}` | glTF をシーンにロード |
| `set_time {t}` | 仮想時刻を設定(秒) |
| `step_frame {}` | 1 フレーム進めて描画 |
| `render_frame {}` | 現時刻で描画 |
| `capture {path}` | 直近フレームを PNG 保存 |
| `update_transforms {objects, transforms}` | R4 と同形の transform を直接送る |

- スキーマが固まるまでは**ファイル経由の連携を正**とし、ライブ連携は「あれば便利」の位置づけで設計だけ崩さないようにする(R1 を守っていれば自然に対応できる)
- プロトコル定義(JSON Schema)は将来共有リポジトリに置き、両プロジェクトが同じ定義から実装する
- 最初の実装(エンジン側コマンド層 stage 2)も**独自の行プロトコルではなく stdio NDJSON 上の JSON-RPC 2.0** とする(メソッドは `set_time` / `step_frame` / `render_frame` / `capture` の 4 つのみ)。ロードマップの「JSON-RPC サーバはまだ作らない」は「TCP 常駐サーバはまだ」の意味であり、エンベロープは最初から JSON-RPC 2.0 を用いる
- notification(id なし)は使わない。全呼び出しが request/response。エラーは JSON-RPC error object、アプリ固有 code は -32000〜-32099

## 5. 品質・テスト要求

**R9: ツールごとに以下を CI で回せること。**

- 決定性テスト: 同一入力 2 回実行で出力が数値一致
- スキーマテスト: 出力 JSON/glTF がスキーマバリデーションを通る(glTF は `gltf-validator`)
- 小さな golden データ: 数フレーム・数オブジェクトの入力 → 期待出力の回帰テスト

**R10: バージョニング。** 出力ファイルには必ず schema 名 + version + 生成ツール名 + ツールバージョンを埋め込む(glTF は `asset.generator`、JSONL はヘッダ行)。後方互換を壊す変更は version を上げ、読み手(Blender アドオン・Pelican2)の対応を待ってからデフォルト化する。

`pelican.import` manifest は version 1 とし、`outputs[]` でも形式ごとの版を
明示する。`pelican.transform_seq` / `pelican.scene` / `pelican.layout` /
`pelican.atlas` / `pelican.material` は `version: 1`、`khronos.ktx2` は
コンテナ版の `version: 2` が必須である。省略や非現行版は受理されない。
非versioned形式の `gltf` / `png` は `outputs[]` に version を書かない。
`tool.version` は互換判定用ではなく、生成元を示す自由文字列である。

## 6. エージェント向け補足

- 本書の R 番号要求と矛盾する実装を行わないこと。矛盾が必要だと判断した場合は実装せず、理由を添えて人間に確認する
- 「Blender で動けばよい」という近道(bpy 依存のロジック、Blender 内部単位のままの出力)は R1/R6 違反であり、Pelican2 側の資産化を壊す
- 迷ったら「CLI 単体で再現できるか」「出力ファイルだけ見て第三者が解釈できるか」を判定基準にする
