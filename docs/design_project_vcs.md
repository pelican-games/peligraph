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
- mount は**プロジェクト外への相対パスを許可**(例 `"mount": "../myproj-assets/"`。
  絶対パス禁止は維持)。兄弟ディレクトリ配置なら **git worktree 全部が同じ親を
  共有するので追加設定なしで解決**する(2026-07-08 ユーザー案 — 非追跡
  local.json が worktree に付いてこない問題の既定解)。上書き手段は
  local.json(マシン別)と環境変数 `PELICAN_STORE_<NAME>`(CI/エージェント用)
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
  **ファイル名込みで列挙**。`--full` = INFO 級(内容不一致)も含めた完全照合
  (破損を疑った日に叩くお助けコマンド — 常用しない前提なので警告疲れと無縁)
- 起動前検証([PF] §5-6 の予告の実体化)。**検証はロードを止めない(原則)**。
  深刻度は「内容の変化 = 情報、構造の逸脱 = 警告」で区分する
  (2026-07-08 ユーザー決定):
  - **INFO(お助け機能)**: 内容ハッシュの不一致 = ファイル更新の追認。
    開発中の置換は正常な営みであり異常扱いしない。サマリ 1 行 +
    「確定したら `assets manifest` で追認」の案内
  - **WARNING(異常のサイン)**: 構造の逸脱のみ — manifest 記載なのに store に
    **存在しない**(欠落)/ 参照が解決できない / 大文字小文字の不一致
    (Linux CI で死ぬ予兆)/ 階層のズレ。「更新した」では説明がつかない
    状態だけを警告にする(警告 = 本当に何かおかしい、を維持し警告疲れを防ぐ)
  - **ERROR**: `--strict-assets` 明示時(CI 用)と dist ビルド
    (dist-config が導出)のみ
- **manifest なし = 検証なし**: manifest を作らなければ何も起きない
  (ハッカソン既定)。検証が欲しくなった日に 1 コマンドで始められる
- 更新の上位経路(将来): devstudio のアセットブラウザ操作時に自動再生成 /
  外部 DAM がツール側で生成(§4 の契約)。git hook への自動組み込みは
  しない(侵襲的 — やりたいプロジェクトが自分で書く)
- store(場所)と manifest(内容)は直交 — 外部 store + manifest =
  「どこに置いてもよいが中身は保証される」。将来 `assets fetch`(URL 取得
  キャッシュ)の土台
- **imports/ と store の関係(2026-07-08 明確化)**: imports/ は **store 内の
  一区画**(生成 PNG 等も assets.manifest に載る)。2 つの帳簿は直交 —
  assets.manifest = **内容の台帳**(何があるか)、pelican.import manifest =
  **出所の記録**(どこから・どのレシピで来たか = provenance)。二重管理ではない
- **store 運用の 3 段**(2026-07-08 ユーザー決定): 小規模 = in-repo + LFS
  (CI strict が完全に機能する推奨既定)/ 中間 = 外部 store + **人間運用**
  (メインのアセットに一番近い人が manifest を管理する — 役割であって機構では
  ない。CI からは store が見えない弱点を許容)/ 大規模 = DAM(§4 契約、
  検証もツール側)。**小規模で外部 store は非想定シナリオ**と明記
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

## 4.5 プロジェクト規約 lint(v2 予約 — エンジン非関与)

フォルダ規約(「何をどこに入れるか」)の検査。**エンジンのコア機能ではない**
(2026-07-08 ユーザー方針)— エンジンは参照解決だけを気にし、規約は
pelican_cli の lint に置く:

- 宣言: glob ベースの policy 表(git 管理。例: `assets/textures/ は png のみ`、
  命名規則、サイズ上限)
- **組み込み既定 = asset policy 二層モデルの機械化**: ランタイム層の閉集合に
  ない形式(.psd / .fbx / .exr 等)が assets/ 下に居たら「ソース層の
  置き場所間違い」として警告 — 最も現実的な誤りを既定で捕まえる
- 実行点は `pelican_cli lint` のみ。エンジンロード時は何もしない
  (§2 の「検証はロードを止めない」と同じ哲学)。既定 = 警告、CI = `--strict`
- 実装優先度は低(V1〜V3 の後)。本節は器の予約

## 5. 実装順(WP 候補)

| 段階 | 内容 | 前提 |
|------|------|------|
| V1 | store マウント(PathResolver 拡張 + local.json)+ 既存挙動不変テスト | **[PF] v6.3 承認** |
| V2 | assets manifest 生成/verify + 起動前検証 + example README 表置換 | V1 |
| V3 | project init 雛形(.gitattributes/.gitignore 込み) | なし(先行可) |

## 5.5 リスクと緩和(2026-07-08 レビューで洗い出し)

| リスク | 緩和 |
|--------|------|
| **共有 store にはブランチがない**(git ブランチごとに manifest は違うのに実体は一つ。切替で警告の嵐・誤った絵) | **バージョン管理は本業ツールに委ねる(自作しない)**: ブランチでアセットが分岐するチームは in-repo + git LFS(ブランチ追従は git の仕事)。共有 store は「共有された最新版」モードと明示(実質 trunk-only)+ 加算的運用(大変更は上書きでなく新ファイル名)。大規模は Perforce/DAM(§4 契約)。CAS 自作は不採用(LFS の再発明・人間が触れない・書き込み儀式が「積極的置換」方針と矛盾)— CAS は将来の fetch ローカルキャッシュ内部形式としてのみ許容 |
| 警告疲れ(置換が日常 → 不一致警告が常態化 → 本物の破損も無視される) | **深刻度区分で根治**(§2): 内容不一致は INFO に降格(お助け機能)、WARNING は構造の逸脱だけ — 「警告が鳴る = 本当に異常」を維持。起動時は 1 行サマリ(件数 + store 解決先)+ 詳細はログ。CI strict が最後の砦 |
| **再インポートで手編集が消える**(データ喪失) | 警告では足りない唯一の例外: 生成後の編集を sha256 で検出したら**上書き拒否 + --force 要求**(`design_asset_containers.md` §5 に反映)。原則は「ロードは止めない、破壊は止める」 |
| clone 直後に動かない(テキストだけ・アセット欠落の初回体験) | `pelican_cli assets status`(欠落と入手元の一覧)。init が README 雛形に「最初にやること」を書き込む。根本解は fetch |
| CI strict が外部 store を見られない / manifest 鮮度の強制力がない | **運用 3 段で解消(§2)**: 小規模 = in-repo+LFS(CI 完全機能)、中間 = 人間運用(メインに近い人が manifest 管理)、大規模 = DAM。小規模×外部 store は非想定と明記 |
| 破損と意図的更新を区別できない(内容不一致 = INFO のため) | 受け入れるトレードオフ。ローダの既存エラーが底、疑った日は `verify --full`(完全照合) |
| 非追跡 local.json が git worktree に付いてこない(エージェント運用直撃) | mount のプロジェクト外相対パス許可(兄弟配置なら全 worktree 自動解決)+ 環境変数 `PELICAN_STORE_<NAME>` |
| フラグメント参照が DCC の改名で壊れる(逆引き困難) | `pelican_cli refs check`(全参照の解決テスト、lint 同居)。エラーに参照元ファイル名を必ず含める |
| local.json が古いことに本人が気づかない | 警告サマリに store 解決先 1 行(§1-2 の get_status 露出と併用) |
| 大文字小文字(Windows/NAS 非区別 vs Linux CI 区別) | manifest 照合は case-sensitive — 手元で先に検出させる |
| manifest のコミット競合(同時アセット追加) | ソート済み出力で解決を自明に。頻発するなら DAM 移行の動機(§4) |

## 6. 未決事項

1. ~~起動前検証の既定~~ → **決定(2026-07-08 ユーザー)**: 開発 = 警告 +
   ロード続行(置換は開発の日常操作)、strict は明示フラグと dist のみ
2. LFS を雛形の既定で有効にするか。推奨: コメントアウト同梱(GitHub 無料枠
   1GB/月帯域の事情はプロジェクト次第)
3. store の read-only 保証(プロジェクト読み取り専有の原則を外部 store にも
   適用するか)。推奨: する — 書くのは import ツールだけ
