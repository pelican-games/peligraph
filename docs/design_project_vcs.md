# プロジェクトの VCS 運用: asset store・manifest・雛形(v1)

対象読者: エンジン担当・プロジェクトを git で運用する人。
ステータス: v1 ドラフト(2026-07-08。レビュー前)。
前提: [PF](PathResolver — **本書の store とフラグメント参照
(`design_asset_containers.md`)、user://(`design_persistence.md`)を
まとめて [PF] v6.3 改訂として提案**)、`design_asset_format_policy.md`。

## 0. 目的

「プロジェクトを git に上げやすくする」を 3 点セットで実現する:
①asset store(場所の間接化)②assets manifest(内容の検証)
③project init 雛形(事故防止の初期化)。

現状の問題: example の README 手書き sha256 表は実ファイルと不整合
(20 個中 7 個しか記載なし)、`.gitattributes` なしの CRLF 事故が
ユーザープロジェクトで再発し得る、大きいバイナリの置き場所標準がない。

## 1. asset store(場所の間接化)

### 1-1. 宣言(project.json — git に入る)

```json
"asset_stores": {
  "main": { "mount": "assets/", "manifest": "assets.manifest.json" }
}
```

- 意味論 = **マウント**: `project://assets/**` への参照をこの実体に張り付ける。
  **シーン等の参照記法は一切変わらない**(解決レイヤの機能。新スキーム不採用)
- 既定(宣言省略時)= `assets/` がプロジェクト内にそのまま在る現行挙動。
  **既存プロジェクトは無変更で動く**
- 複数 store 可(例: `main` + `shared`)。同一パスが複数 store に解決される
  場合は **hard error**(writes-writes 曖昧と同じ思想。フォールバック探索禁止)

### 1-2. ローカル上書き(.pelican/local.json — git に入らない)

```json
"asset_stores": { "main": "D:/shared_assets/mygame" }
```

- マシン固有の実パスはここだけ。project.json には**絶対パスを書けない**
  ([PF] の絶対パス拒否は維持 — 可搬性の生命線)
- local.json が無ければ project.json の mount 相対パス(プロジェクト内)
- **local.json は「パス辞書」に限定(重要)**: 書けるのは宣言済み store 名 →
  実パスの対応**だけ**。未知キー・project.json に無い store 名・
  ルール/レシピ等の意味論の上書きは**エラー**(黙って無視しない)。
  「場所はマシンごとに自由、意味は git だけが持つ」— works-on-my-machine
  事故(各人の手元で変換結果や参照解決が変わる)を構造的に封じる
- 起動時に store の解決結果(store 名 → 実パス)をログ + `get_status` に
  出す(差異のデバッグを一発にする)。manifest verify が「場所は違うが
  中身が古い/壊れている」を各人の手元で即検出する
- web プロファイル: mount → base URL の写像(アセットだけ CDN 配信)。
  サブセット原則は保たれる

## 2. assets manifest(内容の検証)

- `pelican_cli assets manifest` — store 走査 → sha256 + サイズの
  `assets.manifest.json` 生成(pelican.import manifest と同じ流儀の
  エンベロープ)。**冪等**(相対パスでソート済み出力)— 再生成の git diff が
  そのまま「増えた/消えた/変わった」の変更履歴になる。`.pelican/` に
  サイズ+mtime キャッシュを持ち**変更ファイルのみ再ハッシュ**(`--full` で
  全再計算)
- `pelican_cli assets verify` — 照合。欠落・不一致・manifest 外のファイルを
  **ファイル名込みで列挙**
- 起動前検証([PF] §5-6 の予告の実体化)。**検証はロードを止めない(原則)**:
  ファイル置換は開発中に積極的に行われるべき操作なので、不一致 = 警告 +
  そのままロード。警告には「manifest が古い場合は `assets manifest` を実行」の
  案内を含める(更新忘れは次の起動が教えてくれる)。エラーで止まるのは
  `--strict-assets` 明示時(CI 用)と dist ビルド(dist-config が導出)のみ
- **manifest なし = 検証なし**: manifest を作らなければ何も起きない
  (ハッカソン既定)。検証が欲しくなった日に 1 コマンドで始められる
- 更新の上位経路(将来): devstudio のアセットブラウザ操作時に自動再生成 /
  外部 DAM がツール側で生成(§4 の契約)。git hook への自動組み込みは
  しない(侵襲的 — やりたいプロジェクトが自分で書く)
- store(場所)と manifest(内容)は直交 — 外部 store + manifest =
  「どこに置いてもよいが中身は保証される」。将来 `assets fetch`(URL 取得
  キャッシュ)の土台
- **store 内の相対パス構造は全員共有の契約**(参照も manifest キーも
  相対パス)。マシンごとに違ってよいのは root の位置だけ — 構造の個人差は
  verify が名指しで検出する
- example の README 手書き表はこれで置き換える

## 3. project init(雛形)

`pelican_cli project init <dir>` が生成:

- project.json / scenes/ / assets/ / input/ / code/ の最小雛形(example 縮約)
- **`.gitattributes`**: `*.glb -text`、`*.vrm -text`、`*.png -text`、
  `*.wav -text`、`*.spv -text` 等 + LFS track 行(コメントアウトで同梱 —
  LFS 採用は選択。未決 2)
- **`.gitignore`**: `.pelican/`(local.json)、キャッシュ類
- CRLF 事故(WP21 で実際に踏んだ)と「うっかり 50MB コミット」を
  初期化時点で封じる

## 4. 大規模開発: 外部アセット管理ツールとの統合点

**統合点は API ではなくデータ契約**。エンジンが信じるのは
①store の実体(ファイル)②assets.manifest ③imports/ + pelican.import
manifest、の 3 つだけ。社内 DAM・Perforce・任意のパイプラインは、
この契約どおりにファイルと manifest を書けば **pelican_cli を置き換えて**
成立する(houdini-adapter と同じ疎結合原理 — エンジンへのプラグイン機構は
作らない)。pelican_cli は「契約を満たす最小のリファレンス実装」と位置づける。

## 5. 実装順(WP 候補)

| 段階 | 内容 | 前提 |
|------|------|------|
| V1 | store マウント(PathResolver 拡張 + local.json)+ 既存挙動不変テスト | **[PF] v6.3 承認** |
| V2 | assets manifest 生成/verify + 起動前検証 + example README 表置換 | V1 |
| V3 | project init 雛形(.gitattributes/.gitignore 込み) | なし(先行可) |

## 6. 未決事項

1. ~~起動前検証の既定~~ → **決定(2026-07-08 ユーザー)**: 開発 = 警告 +
   ロード続行(置換は開発の日常操作)、strict は明示フラグと dist のみ
2. LFS を雛形の既定で有効にするか。推奨: コメントアウト同梱(GitHub 無料枠
   1GB/月帯域の事情はプロジェクト次第)
3. store の read-only 保証(プロジェクト読み取り専有の原則を外部 store にも
   適用するか)。推奨: する — 書くのは import ツールだけ
