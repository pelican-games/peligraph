# コンテナアセット: サブアセット参照と展開レーン(v1)

対象読者: エンジン担当・DCC 連携を書く人。
ステータス: v1 ドラフト(2026-07-08。レビュー前)。
前提: `design_asset_format_policy.md`(二層モデル)、
`design_project_dcc_houdini.md`(pelican.import manifest)、
`design_project_vcs.md`(store / 外部ツール契約)、
`design_scene_format.md`、R6(glTF extras 規約)。
[PF] v6.3 改訂項目: **フラグメント参照構文**。

## 0. 目的と原則

glTF・PSD・アトラスのような「複数素材の入れ物」を、①パックのまま使う
②中身を個別に使う ③構造(シーン階層・レイヤーツリー)をゲームデータとして
使う、の 3 様で扱えるようにする。原則:

- **参照 = 存在**(render feature と同原理): 有効/無効の旗は立てない。
  丸ごと参照すればパック、フラグメントで参照すれば分解。コストは参照した分だけ
- **展開はエンジンに 1 バイトも入れない**: PSD 等のソース層コンテナの解釈は
  エンジン外のツールに完全隔離。エンジン側の表面は生成された JSON と
  フラグメント参照だけ
- **構造の焼き出しはテキスト**(収録→データ化の哲学): 階層・レイヤーツリーは
  git で diff できる JSON になる

## 1. フラグメント参照([PF] v6.3)

```
project://assets/models/city.glb#mesh/LampPost
project://assets/models/character.glb#animation/Walk
imports/ui_atlas.png#sprite/coin
```

- 構文: `<パス>#<種別>/<名前または連結パス>`。種別はコンテナごとに定義
  (glb: mesh / material / node / animation。アトラス: sprite。追加は
  エンジン先行 — サブセット原則)
- **正準形 = フルパス**(2026-07-08 決定): 内部の同一性は常にコンテナ内
  フルパス(`#node/Root/Arm/Cube`)で持つ。短い一意名(`#mesh/Cube`)は
  **糖衣構文** — ロード時にフルパスへ解決し、曖昧(複数一致)ならエラー。
  refs check はフルパスへの正規化を提案できる(書き方の分裂を防ぐ)。
  パス方式は USD prim パス・Unreal オブジェクトパス・Godot ノードパスと
  同系の業界本流。階層の組み換えを軽々しくやらないのは DCC 側の一般的規律
  (パスが壊れたら refs check が名指しする)
- 同一フルパスの重複は**開発ルール違反**(コンテナ内の同名回避はチームの
  規律 — refs check が指摘、エンジンはロード時エラーで名前 + パスを出す)。
  インデックス参照(`#mesh/3`)は不採用(DCC の並び替えで壊れる)
- フラグメントなし = 従来どおり丸ごと(**現行プロジェクト無変更**)
- 実装はコア(PathResolver の解析 + 各ローダの部分ロード)。依存追加なし

## 2. glTF シーン抽出(pelican_cli — 新規依存なし)

`pelican_cli import gltf --extract-scene <glb>` :

- ノード階層 → pelican.scene の objects(名前・transform・**親子**)。
  親子は scene v1 の小改訂(`objects[].parent` = 親 object の name、WP79 で確定)
- KHR_lights_punctual → light コンポーネント / カメラノード → camera
  コンポーネント(C1 で glTF 1:1 なので損失なし)/ extras → コンポーネント
  params(R6 既存規約)
- メッシュはフラグメント参照(§1)で**元 glb を指す** — glb は分解しない。
  構造だけがテキスト化される
- 実装は devcli(tinygltf を既にリンク)。devcli は配布ビルドに入らないので
  自然にオプション

## 3. PSD レーン(pelican-import-tools — エンジン外)

- 独立リポジトリ **pelican-import-tools** の最初の住人。
  **psd-tools(Python、活発・実戦豊富)**に乗る。自前 PSD パーサ禁止
- 出力: 各レイヤー(グループ単位も可)→ トリミング済み PNG + オフセット、
  レイヤーツリー(名前・位置・サイズ・不透明度・表示・グループ)→
  レイアウト JSON、全体を imports/ + pelican.import manifest(sha256)で着地
  — houdini-adapter と同一の契約構造
- **manifest にツールバージョンを記録**(2026-07-08): provenance の自然な
  拡張。バイト決定性は強制しない(ツールバージョン共通化はチームの通常運用に
  委ねる — 生成物が揺れたら manifest のバージョン欄が原因を示す)
- レイアウト JSON の意味論(ブレンドモード・座標系)の正は **2D 設計
  (起草残)が持つ**。本書は「位置とツリーだけの最小 v1」に留め、
  スキーマ名 `pelican.layout` v1 を予約
- アトラスパック(細かい PNG 群 → 1 枚 + `#sprite/名前`)も同ツールの
  レシピとして提供 — パックと分解が同じ参照構文で閉じる

## 4. import ルール(フォルダ/ファイル単位の制御)

per-file サイドカー(.meta 方式)は不採用。**glob → レシピの中央ルール表**:

```json
// imports.rules.json(git に入る)
{ "schema": "pelican.import_rules", "version": 1,
  "rules": [
    { "match": "ui/**/*.psd",      "recipe": "psd_layers" },
    { "match": "levels/*.glb",     "recipe": "extract_scene" },
    { "match": "sprites/**/*.png", "recipe": "atlas_pack", "options": { "max_size": 2048 } }
  ] }
```

- ルールにないファイルは**何もされない**(既定 = 最速。ハッカソンは
  ルール表なしで glb/png 直参照 — 今と同じゼロ手数)
- **設定の 3 層**(上が優先、**すべて git に入る**): imports.rules.json の
  rules > 同ファイルの `defaults` 区画(拡張子既定レシピの上書き/無効化)>
  エンジン組み込み既定。**既定値も含めて全部データ** — 大規模開発で
  外部アセット管理ツールがこれらを自前生成してよい
- **ローカル層(.pelican/local.json)には import の意味論を置かない**:
  ローカルに許すのは store の実パスだけ(`design_project_vcs.md` §1-2)。
  ルール・レシピ・既定がマシンごとに変わると、生成物の差分がコミットに
  混入して全員を汚染する(works-on-my-machine の構造的封じ)
- 外部ツール統合は `design_project_vcs.md` §4 の契約に従う
  (pelican_cli を置き換える。エンジンにプラグインしない)

## 5. 再インポートと手編集の衝突(規約)

- 生成物(抽出シーン・レイアウト JSON・アトラス)は**再生成で上書きされる**
  読み取り専用扱い。provenance は pelican.import manifest が持つ(既存)
- 手編集は生成物に対して行わない — 別シーンとして fork するか、
  オーバーライド(生成シーンを参照し差分を重ねる。シーン合成は未決 2)
- 生成物を直接編集した状態での再 import は sha256 で機械検出し、
  **上書き拒否 + `--force` 要求**(2026-07-08 改訂: 警告では作業喪失を
  防げない。「ロードは止めない、破壊は止める」)

## 6. 実装順(WP 候補)

| 段階 | 内容 | 前提 |
|------|------|------|
| K1 | フラグメント参照(glb: mesh/material、パーサ + ローダ部分ロード + エラー系) | [PF] v6.3 承認 |
| K2 | glTF シーン抽出(scene v1 親子改訂と同時) | K1 |
| K3 | pelican-import-tools 創設 + PSD レイヤー展開 + アトラスパック | K1(参照側)。2D 設計と並行可 |
| K4 | imports.rules.json + 既定レシピ層 | K2 or K3 の後 |

## 7. 未決事項

1. scene v1 の親子(objects[].parent)— 形式改訂の版数と、Transform 階層の
   ランタイム表現(LocalTransform は既存)
2. シーン合成/オーバーライド(生成シーン + 手編集差分)の形式 — 需要確定まで
   予約のみ
3. glb 内アニメーション参照(#animation/)の再生系は WP38(スケルタル)と
   同時に設計
4. psd-tools のブレンドモード忠実度は 2D 設計時に検証(v1 は normal のみ)
