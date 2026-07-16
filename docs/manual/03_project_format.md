# 第3章 プロジェクト形式

対象: pelican2(2026-07-16 時点)/ このマニュアルはコードを正とする

## この章で学ぶこと

- 「プロジェクト」という単位の定義と、標準ディレクトリ構成
- `project.json` の正確なスキーマ(必須/任意/既定値)と検証規則
- パス参照の分類(素の相対 / `project://` / `engine://` / `user://` / 絶対 / `#` フラグメント)と PathResolver の解決規則
- asset store(置き場所の間接化)と `.pelican/local.json`
- なぜこの設計なのか — fail-fast、cwd 全廃、絶対パス拒否などの決定事項

正本となる設計文書は [../design_project_format.md](../design_project_format.md)([PF] v6.3・**凍結済み**)です。この章はその実装(`src/core/loader/` と `src/project/`)を基準に書いています。

## 3.1 プロジェクトとは

> **設計決定:** プロジェクト = ルートに `project.json` があるひとつのディレクトリ。これが「開く・実行する・配布する」の単位(Unity の Project、Godot の project.godot に相当)。エンジン実行時、プロジェクトディレクトリは**読み取り専用**であり、エンジンは何も書き込まない。書き込み先は `user://`(OS のユーザーディレクトリ)に分離される。これが複数インスタンス並行起動の安全性の根拠になっている。

標準ディレクトリ構成(`pelican_cli project init` が生成する形。[第2章](02_getting_started.md) 参照):

| パス | 内容 | 参照元 |
|---|---|---|
| `project.json` | マニフェスト+基本設定 | 起動時に必ず読む |
| `scenes/*.scene.json` | シーン定義(pelican.scene v1) | `basic_config.scene_data_json` |
| `assets/asset_data.json` | モデル登録表 | `basic_config.asset_data_json` |
| `assets/models/`, `assets/textures/`, `assets/audio/` | バイナリアセット置き場(規約) | asset_data.json 等から相対参照 |
| `passes/*.json` | レンダリングパイプライン定義 | `basic_config.rendering_config_json` |
| `input/actions.json` | 入力アクション定義 | `basic_config.input_actions_json` |
| `input/profiles/*.json` | バインディングプロファイル | `basic_config.input_profiles` |
| `ui/*.json` | UI ドキュメント(pelican.ui v1) | `basic_config.ui_config_json` |
| `code/` | C++ ゲームコード | CMake `-DPELICAN_PROJECT` |
| `imports/`(規約) | 外部ツールの納品物+manifest | `pelican_cli import` の入力 |
| `.pelican/local.json` | マシン固有のパス上書き(**git 非追跡**) | asset store の実パス差し替え |

## 3.2 project.json のスキーマ

実物([../../projects/example/project.json](../../projects/example/project.json) 全文):

```json
{
  "schema": "pelican.project",
  "version": 1,
  "name": "example",
  "generator": "hand-written",
  "engine_min_version": "0.1.0",
  "basic_config": {
    "window_title": "Pelican App",
    "window_size": { "width": 1920, "height": 1080 },
    "fullscreen": false,
    "framerate": 60,
    "camera": { "yfov": 0.7853981633974483, "znear": 0.1, "zfar": 1000, "up": [0.0, -1.0, 0.0] },
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main_rendering_config.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/ui_overlay.json",
    "input_actions_json": "input/actions.json"
  }
}
```

### トップレベルフィールド

検証の実装は [basicconfig.cpp](../../src/core/loader/basicconfig.cpp)(`validateProjectJson`)です。

| フィールド | 必須? | 検証・意味 |
|---|---|---|
| `schema` | **必須** | `"pelican.project"` 以外は起動拒否(hard error) |
| `version` | **必須** | 整数で **`1` ちょうど**。0・負・2 以上はすべて拒否(WP63 strict v1) |
| `name` | 実装上は任意 | `user://` スキームの project_id になる。無いと `user://` が使えない ※設計文書 [PF] §4 では必須と規定されているが、現行実装は検証しない |
| `generator` | 任意 | パーサは読まない(生成元の記録用メタデータ) |
| `engine_min_version` | 任意 | `"X.Y.Z"`。エンジン現行版(`0.1.0`)より新しければ起動拒否。`--ignore-engine-version` で WARN に降格可 |
| `basic_config` | 任意 | 省略したキーは既定値で埋まる(次節) |
| `asset_stores` | 任意 | asset store 宣言(§3.4) |

### basic_config と「3 段合成」

> **設計決定:** 設定の優先順位は **CLI 設定 > project.json > 埋め込み既定値(`engine://default_config.json`)** の 3 段で、キー単位に上位が勝つ。プロジェクトは差分だけ書けばよい。3 段すべてに無い必須キーは `config not found: ...` で hard error。

※実装上の注意: 3 段の「CLI 層」の実体はエンジン組み込み API(`PelicanCore(settings_str)`)に渡す JSON であり、`pelican_player` は常に空 `{}` を渡します。つまり player の CLI フラグ(`--size` など)は JSON 合成ではなく `EngineLaunchConfig` 経由の**別経路**で、ヘッドレス系にのみ効きます。

| キー | 型 | 既定値 | 意味 |
|---|---|---|---|
| `window_title` | string | `"Pelican App"` | ウィンドウタイトル |
| `window_size.width` / `.height` | int | 1920 / 1080 | ウィンドウサイズ |
| `fullscreen` | bool | false | フルスクリーン |
| `framerate` | number | 60 | ウィンドウモードのフレームレート目標 |
| `seed` | 非負整数 | 0 | 決定的乱数のシード([第8章](08_gameplay.md))。負値はエラー |
| `camera` | object | 下記 | 既定カメラ(glTF 同形。[第8章](08_gameplay.md)) |
| `default_scene_id` | string | `"default_scene"` | 起動時にロードするシーン ID |
| `scene_data_json` | 参照 | `"scenes/main.scene.json"` | シーン定義 JSON への参照 |
| `asset_data_json` | 参照 | `"assets/asset_data.json"` | アセット登録 JSON への参照 |
| `rendering_config_json` | 参照 | `"passes/main_rendering_config.json"` | レンダリング構成への参照 |
| `default_rendering_pass` | string | `"main_render"` | rendering config 内で使う `rendering_passes[].name` |
| `ui_config_json` | 参照 | `"ui/ui_overlay.json"` | UI ドキュメント(pelican.ui)への参照 |
| `input_actions_json` | 参照 | **既定なし(任意)** | 入力アクション定義。未指定ならアクション層は未構成 |
| `input_profiles` | object | — | バインディングプロファイルの辞書(名前 → ファイル参照。✅WP91 — [第7章](07_input_ui.md)) |
| `input_profile` | string | — | 既定プロファイル名 |
| `sprite` | object | `{"pixels_per_unit": 100.0}` | 2D スプライトの寸法変換係数(✅WP106 — [第6章](06_rendering.md) §6.9) |
| `camera.sprite` | object | `{"pixel_perfect":"off","sort":"z"}` | スプライトの pixel perfect / ソートポリシー(camera 内。シーン側 camera コンポーネントにも書ける) |

`camera` は glTF のカメラ定義と同形です。`type` は `"perspective"`(既定)か `"orthographic"` のみ。perspective は `yfov`(**ラジアン**)/`znear`/`zfar` 必須・`aspect` 任意、orthographic は `xmag`/`ymag`/`znear`/`zfar` 必須です。

> **設計決定(fail-fast):** 旧キー名 `fov_y` / `near` / `far` は「黙って無視」ではなく `'fov_y' is not supported in v1; use 'yfov' (radians)` という**移行先を案内する名指しエラー**で拒否される(WP63)。「読めるふりをしない」— 警告止まりだと『動いたように見えて新機能の挙動だけ壊れる』事故になるため。互換受理の追加はプロジェクト規則で禁止されており、旧形式の変換は外部ツールの仕事。

## 3.3 パス参照と PathResolver

すべてのコンテンツ参照(JSON 内・CLI のコンテンツ引数)は **PathResolver** モジュール([pathresolver.cpp](../../src/core/loader/pathresolver.cpp))を通ります。

### 参照の分類

| 書き方 | 解決先 | 状態 |
|---|---|---|
| `assets/a.glb`(素の相対) | プロジェクトルート基準 | ✅実装済み |
| `project://assets/a.glb` | 素の相対と**完全に等価**(接頭辞を剥がすだけ) | ✅実装済み |
| `engine://<id>` | エンジン埋め込みリソース(後述) | ✅実装済み |
| `user://<rel>` | `%APPDATA%/pelican/<name>/<rel>`(Windows) | ✅実装済み(読み書き API も ✅WP65 — [第8章](08_gameplay.md)) |
| 絶対パス / UNC | JSON 内では**常に拒否**。CLI 由来のみ `--allow-absolute-paths` で許可+WARN | ✅実装済み |
| `assets/a.glb#mesh/Cube`(フラグメント) | コンテナ内サブアセット | ✅実ロード対応(WP77。`mesh` / `node` / `material` / `animation` の 4 種 — [第5章](05_assets.md)) |

共通規則: 空文字列・バックスラッシュ(`\`)・未知スキーム(`foo://`)はすべて拒否。パス区切りはスラッシュのみです。

> **設計決定(相対パスの基準):** JSON の中に書く相対パスは、**その JSON ファイルの場所ではなく常にプロジェクトルート基準**。理由: JSON を別ディレクトリに移動しても参照が壊れず、解決規則が 1 つだけになり実装・デバッグが単純になる。

> **設計決定(cwd 全廃):** コンテンツ参照の解決に作業ディレクトリを一切使わない。旧実装は「exe のあるディレクトリから起動しないと即死」で、DCC ブリッジのサブプロセス起動で必ず踏む罠だった。例外は `--project` / `--render-out` / `--user-dir` のような **locator 引数**(プロジェクトの外を指すのが本務)で、これらは PathResolver の管轄外。

> **設計決定(絶対パスの文脈分離):** 永続化される JSON(プロジェクト内のファイル)に絶対パスを書くことは**どんなフラグでも許可されない**。配布したプロジェクトが他人のマシンで壊れる事故を形式レベルで防ぐため。CLI 引数由来の参照だけが `--allow-absolute-paths` で通り、そのたびに WARN が出る。

### 脱出防止

解決結果はパス正規化(シンボリックリンク・`..` 込み)後に、プロジェクトルート(asset store 使用時は mount root)の**内側にあることをパス成分単位で検査**されます。外に出る参照は `escapes project root: ...` で拒否されます。Windows ではパス成分ごとに大文字小文字を無視して比較し、`C:\proj` と `C:\project2` のような前方一致の誤判定も防いでいます。

### engine:// と EngineResourceRegistry

`engine://<id>` はエンジンバイナリに埋め込まれたリソース(既定設定・標準シェーダ・feature 定義・フォント等)を指します。id は埋め込み元 `src/core/resources/` からの相対パスで、現在 47〜48 エントリが [engineresources.cpp](../../src/core/loader/engineresources.cpp) に登録されています。

- 未知の id を参照すると、**登録済み id の全一覧付き**でエラーになります(タイポの即時発見)。
- id 一覧はテストフィクスチャ `test/fixtures/project_format/engine_resources.json` と単体テストで同期が強制されます。web ビューアのレジストリはこの**鏡像サブセット**です([第9章](09_web.md))。

> **設計決定(暗黙のシャドーイング禁止):** プロジェクト内に同名ファイルがあっても `engine://` は常に埋め込みを読む。どちらを読んでいるかが参照文字列を見れば常に分かる。差し替えたいときは参照側を書き換える。

### user://

- 解決先は `%APPDATA%/pelican/<project.json の name>/`(Windows)。非 Windows では `$XDG_DATA_HOME` → `~/.local/share` にフォールバックします(※設計文書には Windows のみ記載)。
- `--user-dir <dir>` でルートを差し替えられます(テスト・複数インスタンス分離用)。
- `name` が無い project.json では `user://` は使えません(名指しのエラー)。
- ✅WP65 で**読み書き API まで実装済み**です: 設定は `user://settings.json`(`pelican.settings` v1)、セーブは `user://saves/<slot>.json`。書き込みは tmp ファイル + rename の atomic 方式(Windows は `MoveFileExW`)。API と使い方は [第8章](08_gameplay.md) を参照してください。

## 3.4 asset store — 置き場所の間接化(✅実装済み・WP55)

巨大なバイナリアセットを git リポジトリの外(共有ドライブ・別リポジトリ等)に置くための仕組みです。正本は [../design_project_vcs.md](../design_project_vcs.md)。

### 宣言(project.json、git にコミットする)

```json
"asset_stores": {
  "main": { "mount": "assets/" }
}
```

- 意味論は**マウント**: `assets/**` への参照を store の実体ディレクトリに張り替えます。**シーン等の参照の書き方は一切変わりません**(解決レイヤだけの機能)。
- 宣言を省略すれば従来どおり(`assets/` はプロジェクト内)。既存プロジェクトは無変更で動きます。
- `mount` は相対パスのみ。`"../mygame-assets/"` のような**プロジェクト外への相対は許可**(git worktree の兄弟配置が想定解)。絶対パスは拒否。マウント点の入れ子・重なりは宣言時に hard error。
- 脱出防止の基準点は store 経由の参照では mount root に付け替わります。

### マシン別の実パス上書き(`.pelican/local.json`、git に入れない)

```json
{ "asset_stores": { "main": "D:/shared_assets/mygame" } }
```

> **設計決定(パス辞書限定):** `.pelican/local.json` に書けるのは「宣言済み store 名 → 実パス」だけ。未知のキー・未宣言の store 名・文字列以外の値はすべて**黙って無視せずエラー**。「場所はマシンごとに自由、意味(何がプロジェクトに属すか)は git だけが持つ」を構造的に強制し、works-on-my-machine 事故を封じるため。

解決結果は起動時に `asset stores resolved: main=<実パス>` と INFO ログに出て、rpc の `get_status.stores` でも確認できます。

### assets manifest(✅実装済み・WP66)

バイナリアセットの sha256 台帳です。store 宣言に `manifest` キーで紐付けます(example の実物):

```json
"asset_stores": {
  "main": { "mount": "assets/", "manifest": "assets.manifest.json" }
}
```

manifest 本体(`pelican.assets` v1)は `pelican_cli assets manifest` が生成します:

```json
{
  "schema": "pelican.assets",
  "version": 1,
  "tool": { "name": "pelican_cli assets manifest" },
  "files": [
    { "file": "models/sponza.glb", "size": 52608696, "sha256": "8eade0d6..." }
  ]
}
```

- `files[]` は store 相対パスの**一意ソート必須**(生成は冪等 — 2 回実行で byte 一致)。差分ハッシュのキャッシュは `.pelican/assets-hash-cache.json`(git 非追跡)。
- CLI: `pelican_cli assets manifest / verify / status --project <dir>`([第10章](10_tools.md))。
- **起動時検証**: manifest 宣言のある store は player 起動時に照合されます。深刻度は「内容の変化 = INFO、欠落・参照不能・大文字小文字違い = WARNING」で、**ロードは止めません**。`--strict-assets` 指定時のみ全 issue が ERROR に昇格して起動中止になります。

> **設計決定(検証はロードを止めない):** アセットの正当性検査は開発の補助であり、既定では警告に留める。厳格化は明示フラグ(開発 CI・配布検証)でのみ行う。

example の README にあった手書き sha256 表の役目は、この manifest([../../projects/example/assets.manifest.json](../../projects/example/assets.manifest.json)・26 エントリ)に置き換わりました。

## 3.5 解釈レイヤ — pelican_project とバインダの分離

> **設計決定(3 層構造):** プロジェクト形式の解釈は次の 3 層に分かれる。
> ```
> プロジェクト形式(ファイル群)
>   ↓ 解釈レイヤ: pelican_project 静的ライブラリ(エンジン非依存の純ロジック。依存は nlohmann_json のみ)
>   ↓ バインダ: src/core/loader のエンジン側変換(GET_MODULE を呼べるのはここと起動配線のみ)
> ゲームエンジン本体
> ```
> 形式に機能を足すときは「解釈レイヤにパーサ+バインダに変換」を対で追加し、エンジン内部へ波及させない。パーサ登録は静的な表で行い、プラグイン機構は作らない(過剰抽象化の防止)。

[src/project/](../../src/project) が解釈レイヤの実体で(CMake ターゲット `pelican_project`、WP44 で分離完了 ✅)、現在の住人は `sceneformat`(pelican.scene)/ `importmanifest`(pelican.import)/ `materialformat` + `surfaceformat` + `materiallowering`(マテリアル)/ `featurecompose`(feature 合成)/ `jsonrpc`(JSON-RPC エンベロープ)/ `assetsmanifest`(pelican.assets)/ `importrules`(pelican.import_rules)などです。vulkan・quill・モジュール機構・リソース埋め込みへの依存は禁止されています。この分離により、`pelican_cli` や将来のエディタは**エンジンをリンクせずに**プロジェクトを読めます。

## 3.6 バージョン管理(VCS)の方針

- エンジンリポジトリは example の JSON と README のみコミットし、バイナリは `.gitignore` で除外(クローンを重くしない)。ユーザープロジェクトでの LFS 利用は自由です。
- store 運用の 3 段構え: 小規模 = リポジトリ内+LFS / 中規模 = 外部 store+人間運用 / 大規模 = DAM(デジタルアセット管理)。共有 store にブランチという概念は持ち込みません(バージョン管理は本業ツールに委譲)。
- 大規模統合は「API ではなくデータ契約」: エンジンが信じるのは ① store の実体、② assets manifest(✅WP66)、③ `imports/` + pelican.import manifest の 3 つだけです。DAM や Perforce は、契約どおりにファイルを書けば `pelican_cli` を置き換えられます。

## 3.7 サブセット原則(web との関係)

> **設計決定(サブセット原則):** 「web で開けるプロジェクト ⊆ pelican で開けるプロジェクト」。形式の拡張は常にエンジン側が先行し、web は「無視 → 対応」の順で追随する。web 専用のキー・参照形式・`engine://` id は作らない。web が pelican より緩い受理をしたら規約違反。

この原則の詳細と web ビューアでの開き方は [第9章](09_web.md) を参照してください。

## 3.8 既知の食い違い(設計文書 vs 実装)

調査(2026-07-10)で判明した主な差分です。**コードが正**として読んでください。全体の一覧は [第11章](11_status.md) にあります。

- `--play-seq` / `--seq-mesh` の相対パスは設計([PF] はプロジェクトルート基準)に反し **cwd 基準**で解決され、絶対パスもフラグなしで通ります(`--play-vat` は設計どおり)。
- 設計上必須の `name` を実装は検証しません(欠落すると黙って `user://` が無効になります)。
- `PathResolver::resolveCliRef`(CLI 文脈用 API)は本番の呼び出し元がまだありません。
- [PF] 本文のコード例(`ResolvedRef` の variant 要素数、レジストリのエントリ数など)は v6.3 の追加機能に未追随の箇所があります。

## 関連文書

- [../design_project_format.md](../design_project_format.md) — [PF] v6.3(凍結)。形式・PathResolver の正
- [../design_project_format_web_profile.md](../design_project_format_web_profile.md) — [PFW] v1.3。サブセット原則
- [../design_project_interpretation_layer.md](../design_project_interpretation_layer.md) — 解釈レイヤの設計
- [../design_project_vcs.md](../design_project_vcs.md) — asset store・manifest・雛形
- [../design_persistence.md](../design_persistence.md) — user:// と設定/セーブ三区分
- [第4章 シーンと ECS](04_scene_ecs.md) / [第5章 アセット](05_assets.md) / [第9章 Web プロファイル](09_web.md)
