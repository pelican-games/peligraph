# 第3章 プロジェクト形式

対象: pelican2(HEAD=`82054c05`・2026-09-17。2026-07-21 に全域再確認、2026-09-04 に §3.7 の「エンジン先行」段落と §3.10(宣言スキーマ v3)を追記、2026-09-17 に §3.2 の `requires` と §3.12(共有部品)を追記、2026-09-24 に §3.2 の `requires` の語彙と §3.11 の `requires.capabilities` を PG-1(`533d8d6c`)に合わせて更新、2026-09-25 に §3.1 の `.pelican/` の住人・§3.2 の `saved_by_engine` と `project.engine_too_old@1`・§3.13(前の版の project を移す)を MG-1(`28bf3b40`)に合わせて追記)/ このマニュアルはコードを正とする

## この章で学ぶこと

- 「プロジェクト」という単位の定義と、標準ディレクトリ構成
- `project.json` の正確なスキーマ(必須/任意/既定値)と検証規則
- パス参照の分類(素の相対 / `project://` / `engine://` / `user://` / 絶対 / `#` フラグメント)と PathResolver の解決規則
- asset store(巨大なバイナリを git リポジトリの外へ置くための「置き場所の間接化」。[用語集](14_glossary.md))と `.pelican/local.json`
- シーン JSON の in-memory 表現(`AuthoringSceneDocument`)と保存の契約(エディタ経路)
- ユーザー定義コンポーネントの宣言スキーマ v3(`Flags` / `List` の行封筒 `$list` / `$row`)と `$` 予約
- 前の版のエンジンで作った project を後の版へ移す仕組み(`pelican_cli project migrate`。§3.13)
- なぜこの設計なのか — fail-fast、cwd 全廃、絶対パス拒否などの決定事項

正本となる設計文書は [../design_project_format.md](../design_project_format.md)([PF] v6.3・**凍結済み**)です。`[PF]` はこの章を通じてこの設計文書を指す略号で、**凍結済み**は文書側の見出しどおり「設計としてこれ以上改訂しない」の意味です。形式そのものが変わらないという約束ではありません —— 形式は v0 の後も変わりえて、変わるときは前の版の project を移す手順が付きます(§3.13。2026-09-24 の利用者裁定)。文書と実装が食い違ったときは実装が正になります(§3.8)。この章はその実装(`src/core/loader/` と `src/project/`)を基準に書いています。

## 3.1 プロジェクトとは

> **設計決定:** プロジェクト = ルートに `project.json` があるひとつのディレクトリ。これが「開く・実行する・配布する」の単位(Unity の Project、Godot の project.godot に相当)。エンジン実行時、プロジェクトディレクトリは**読み取り専用**であり、エンジンは何も書き込まない。書き込み先は `user://`(OS のユーザーディレクトリ)に分離される。これが複数インスタンス並行起動の安全性の根拠になっている。

標準ディレクトリ構成(`pelican_cli project init` が生成する形。[第2章](02_getting_started.md) 参照):

| パス | 内容 | 参照元 |
|---|---|---|
| `project.json` | マニフェスト+基本設定 | 起動時に必ず読む |
| `scenes/*.scene.json` | シーン定義(pelican.scene v2) | `basic_config.scene_data_json` |
| `assets/asset_data.json` | モデル登録表 | `basic_config.asset_data_json` |
| `assets/models/`, `assets/textures/`, `assets/audio/` | バイナリアセット置き場(規約) | asset_data.json 等から相対参照 |
| `passes/*.json` | レンダリングパイプライン定義(rendering config。`pelican.graph` root、下記) | `basic_config.rendering_config_json` |
| `input/actions.json` | 入力アクション定義 | `basic_config.input_actions_json` |
| `input/profiles/*.json` | バインディングプロファイル | `basic_config.input_profiles` |
| `ui/*.json` | UI ドキュメント(pelican.ui v1) | `basic_config.ui_config_json` |
| `code/` | C++ ゲームコード | CMake `-DPELICAN_PROJECT` |
| `imports/`(規約) | 外部ツールの納品物+manifest | `pelican_cli import` の入力 |
| `.pelican/` | ツールとエンジンの生成物置き場(**git 非追跡**。`project init` の `.gitignore` に登録済み) | 下記 |

`.pelican/` の住人(すべて生成物で、リポジトリにコミットしません)。先頭のドットは「人が直接編集しない生成物置き場」を示す慣習で、この中の `local.json` の *local* は「このマシンでだけ有効な値」という意味です(§3.4):

| パス | 内容 |
|---|---|
| `.pelican/local.json` | マシン固有のパス上書き(asset store の実パス差し替え。§3.4) |
| `.pelican/shader_cache/` | シェーダのコンパイルキャッシュ(✅WP82 — [第6章](06_rendering.md))。環境変数 `PELICAN_SHADER_CACHE_DIR` を設定するとそちらに置かれ、ここには作られません(✅TS-1。[第10章](10_tools.md) §10.5) |
| `.pelican/assets-hash-cache.json` | assets manifest の差分ハッシュキャッシュ(§3.4) |
| `.pelican/logs/import-tools.log` | `pelican_cli import --rules` が起動した外部ツールのログ(§5.5) |
| `.pelican/render-authoring.transaction/` | project の文書を何本かまとめて書き換える間だけ在る作業ディレクトリ(`manifest.json`・差し替え前の控え・commit 済みの印 `committed`)。書き手は 2 つで、rendering config の編集(Studio / RPC。root 文書と feature 断片)と、`project migrate` の apply(§3.13)です。ディレクトリは project に 1 つなので、この 2 つの commit は同時に走れません(後から commit しようとした方が `another render authoring transaction or recovery is active` で止まります)。名前は MG-1 で共用になる前のもので、据え置きです([projectdocumenttransaction.hpp](../../src/project/projectdocumenttransaction.hpp#L18) の冒頭コメント)。正常に終われば消えます。プロセスが途中で落ちて残っていれば、次の起動で commit 済みなら文書を確かめてから消し、commit 前なら文書を元に戻してから消します |
| `.pelican/migration/<時刻>/` | `project migrate` の apply が書き換える前の文書の控え(§3.13)。apply が成功したときだけ残ります。消す手段は用意していません(手で消します) |

**旧形式の transaction ディレクトリ(`render_config.legacy_transaction_dir@1`)** ✅MG-2a

v0 より前のエンジンは、commit の決定を別ファイル `committed` ではなく `manifest.json` の `"status": "committed"` に書いていました。この形で残ったディレクトリは**もう読みません**。起動時の復旧と、この transaction を使う commit(rendering config の編集と `project migrate` の apply)のどれも `render_config.legacy_transaction_dir@1` で止まり、ディレクトリには手を付けません([判定](../../src/project/projectdocumenttransaction.cpp#L403))。

- 起動時(`pelican_player` が project.json を持つ project を開くとき): MG-1 から、parser より前の入口(§3.13)が同じ復旧を先に走らせます。stderr に `render_config.legacy_transaction_dir@1: {"message":"…","path":"…"}` が 1 行出て、player は exit 1 で終わります([入口の catch](../../src/player/main.cpp#L753))。この経路はコードを読んで確かめたもので、実行では未確認です。
- 起動時(入口を通らずに `PelicanCore::run()` の復旧に届いたとき): ログに `Pelican fatal error : render_config.legacy_transaction_dir@1: {"message":"…","path":"…"}` が出て、起動はそこで終わります。
- `project migrate` の apply: 先頭で同じ復旧を走らせるので、同じ名前で止まります(CLI は stderr に出して -1)。`--dry-run`(plan)は復旧を走らせません。
- 編集の commit 時: ticket の結果は `committed: false` で、`error.code` がこの名前、`error.path` がディレクトリのパスです。文書は書き換わりません。

片付けの手順(メッセージの文面は「文書は commit 済みなので、この dir を手で消してから開き直す」):

1. engine / Studio を閉じます。
2. `path` に出たディレクトリ(`<project>/.pelican/render-authoring.transaction/`)を**丸ごと**消します。消すのはこのディレクトリだけで、`.pelican/` のほかの住人や project の文書には触りません。
3. project を開き直します。

**なぜ消して安全か**: 旧形式が `status` を `"committed"` に書き換えたのは、すべての文書を新しい内容で書き終えた後(commit の決定点)です。つまり文書はすでに commit 後の内容で、ディレクトリに残っているのは片付け前の控えだけです。旧形式のエンジンもこの状態では、各文書が新しい内容のままかを確かめてからディレクトリを消すだけで、文書を戻すことはしませんでした。手で消して省かれるのはこの確認だけです(commit の後で文書を手で変えていても、止まらずにその内容のまま開きます)。

## 3.2 project.json のスキーマ

実物([../../projects/example/project.json](../../projects/example/project.json) 全文):

```json
{
  "schema": "pelican.project",
  "version": 1,
  "name": "example",
  "generator": "hand-written",
  "engine_min_version": "0.1.0",
  "asset_stores": {
    "main": { "mount": "assets/", "manifest": "assets.manifest.json" }
  },
  "basic_config": {
    "window_title": "Pelican App",
    "window_size": { "width": 1920, "height": 1080 },
    "fullscreen": false,
    "framerate": 60,
    "camera": { "yfov": 0.7853981633974483, "znear": 0.1, "zfar": 1000 },
    "default_scene_id": "default_scene",
    "scene_data_json": "scenes/main.scene.json",
    "asset_data_json": "assets/asset_data.json",
    "rendering_config_json": "passes/main_rendering_config.json",
    "default_rendering_pass": "main_render",
    "ui_config_json": "ui/ui_overlay.json",
    "input_actions_json": "input/actions.json",
    "input_profiles": {
      "keyboard": "input/profiles/keyboard.json",
      "gamepad": "input/profiles/gamepad.json",
      "arcade": "input/profiles/arcade.json",
      "touch": "input/profiles/touch.json"
    },
    "input_profile": "keyboard"
  }
}
```

### トップレベルフィールド

pelican の JSON は先頭 2 キー `schema` / `version` で「どの形式の第何版か」を名乗ります(この 2 キーの組を**エンベロープ**と呼びます。[用語集](14_glossary.md))。`schema` の値は `pelican.<形式名>` の形で、`pelican.project`(この章)・`pelican.scene`([第4章](04_scene_ecs.md))・`pelican.asset_data`([第5章](05_assets.md))のように形式ごとに 1 つずつあります。検証の正本は `pelican_project` の [projectformat.cpp](../../src/project/projectformat.cpp)(`parseProjectEnvelopeJson` / `parseProjectEnvelopeText`)です。player、engine の `basicconfig.cpp`、`pelican_cli dist-config` はこの同じ実装を呼び、schema/version/`engine_min_version` 規則を持ちません。

| フィールド | 必須? | 検証・意味 |
|---|---|---|
| `schema` | **必須** | `"pelican.project"` 以外は起動拒否(hard error) |
| `version` | **必須** | 整数で **`1` ちょうど**。0・負・2 以上はすべて拒否(WP63 strict v1) |
| `name` | 実装上は任意 | `user://` スキームの project_id になる。無いと `user://` が使えない ※設計文書 [PF] §4 では必須と規定されているが、現行実装は検証しない |
| `generator` | 任意 | パーサは読まない(生成元の記録用メタデータ) |
| `engine_min_version` | 任意 | `"X.Y.Z"`。エンジン現行版(`0.1.0`。[`pelicanEngineVersion`](../../src/project/projectformat.hpp#L16))より新しければ `project.engine_too_old@1: project.json engine_min_version <値> is newer than this engine (0.1.0)` で起動拒否(名前は MG-1 から)。`--ignore-engine-version` で WARN に降格可 |
| `saved_by_engine` | 任意 | **記録だけ**(✅MG-1)。最後に project.json を書いたエンジンの版 `"X.Y.Z"`。書くのは `pelican_cli project init`(雛形に入る)と `project migrate` の apply(§3.13)の 2 つだけで、scene の保存などでは書きません。何かを決めるのには使いません(移行するかどうかは各文書の `schema` / `version` で決まります)。文字列でない値は拒否せず無視し、今のエンジンより新しい版が書いてあっても拒否しません([`ProjectEnvelope`](../../src/project/projectformat.hpp#L39) のコメント)。キーを足しただけで `pelican.project` は version 1 のままで、MG-1 より前の player もこのキー入りの project.json を開けます(MG-1 の着地時に実物で確認) |
| `requires` | 任意 | **このプロジェクトが動くために必要なビルド機能ユニットの名前の配列**(下記「機能ユニットの要求」)。語彙は `animation` / `audio` / `physics` / `xr` / `vat` / `seqplayer` の 6 語(✅PG-1)。未知の名前は `project.requires_unknown@1`、その機能を OFF にしたビルドで開くと `<語>.module_unavailable@1`(例: `animation.module_unavailable@1`) |
| `basic_config` | 任意 | 省略したキーは既定値で埋まる(次節) |
| `asset_stores` | 任意 | asset store 宣言(§3.4) |
| `hot_reload` | 任意 | **hot reload が監視する素材とフォルダ**(project root からの論理 path の配列。下記「hot reload で監視するもの」。✅HW-1)。書かなければ今までどおり。形が崩れていれば `project.hot_reload_invalid@1` で起動拒否 |
| `packages` | 任意 | install 済み package の記録 `{namespace, package_version, sha256}` の配列(§3.11)。`pelican_cli package install` が書く |

### 機能ユニットの要求(`requires`)✅P-2b

> **設計決定:** 「このプロジェクトはエンジンのどの部分が無いと成立しないか」を **project.json に 1 行で宣言する**。判定はプロジェクトを開いた瞬間(parse)に済ませ、足りなければ**そのエンジンが既に使っている名前**で止める。新しい名前を 3 本目として作らない。

```json
{ "schema": "pelican.project", "version": 1, "name": "Pose Graph Demo",
  "requires": ["animation"] }
```

語彙は次の 6 語です(✅PG-1 で `animation` 以外の 5 語が入りました)。行の正本は `src/project/projectrequirements.def` の 1 ファイルで、C++ 側の表・CMake 側のコーパス選別・コンポーネントの `required_feature`・package の `requires.capabilities` の検査が同じ行を読みます。

| 語 | 指すビルドオプション | そのオプションを OFF にしたビルドで開くと |
|---|---|---|
| `animation` | `PELICAN_WITH_ANIMATION` | `animation.module_unavailable@1` |
| `audio` | `PELICAN_WITH_AUDIO` | `audio.module_unavailable@1` |
| `physics` | `PELICAN_WITH_PHYSICS`(物理クエリサービスと collider world) | `physics.module_unavailable@1` |
| `xr` | `PELICAN_WITH_OPENXR` | `xr.module_unavailable@1` |
| `vat` | `PELICAN_WITH_VAT` | `vat.module_unavailable@1` |
| `seqplayer` | `PELICAN_WITH_SEQPLAYER` | `seqplayer.module_unavailable@1` |

物理 provider の選択(`PELICAN_WITH_BUILTIN_PHYSICS` / `PELICAN_WITH_JOLT_PHYSICS`)は語彙にありません。`physics` が指すのはクエリサービスの有無だけです。

読み方と規則:

| 事項 | 中身 |
|---|---|
| 語彙 | **ビルド機能ユニットのレジストリに載っている名前**です。各語が指すオプションは上の表のとおりです([第2章 §2.2](02_getting_started.md))。今日の表は 6 行([projectformat.cpp:80](../../src/project/projectformat.cpp#L81) が `.def` を展開)で、`knownProjectRequirements()` がその一覧を返します |
| 検証の時 | **parse 時**。[parseProjectRequirements()](../../src/project/projectformat.cpp#L119)が配列であること・非空文字列であること・表にある名前であることを見ます |
| 未知の名前 | `project.requires_unknown@1: project.json requires 'network', which this engine does not know; known requirements: animation, audio, physics, xr, vat, seqplayer` —— **既知の一覧を添えて**拒否します(一覧の順は `.def` の行の順) |
| 満たせないビルド | 名前は新設せず、その語の `<語>.module_unavailable@1` を出します([projectformat.cpp:181](../../src/project/projectformat.cpp#L264))。例: `physics.module_unavailable@1: this build has PELICAN_WITH_PHYSICS=OFF, so project.json's requires: ["physics"] cannot be satisfied`。全要素が既知語かを先に確かめてから、最初に満たせない語を報告します。`pelican_player` はこれを捕まえて 1 行表示して exit 1 します |
| コーパス側 | **プロジェクト群を順に調べるテスト・スモークも同じ値を読みます**。満たせない要求を持つプロジェクトは失敗ではなく**名前付きの skip**(`skip posegraph_demo: requires animation (PELICAN_WITH_ANIMATION=OFF)`)になります。`PELICAN_WITH_ANIMATION=OFF` build では `animation` を宣言した 3 つ分の行と、必要なら `skip editor lane: …`、最後に件数の要約が出ます([第2章 §2.2](02_getting_started.md))。`PELICAN_WITH_PHYSICS=OFF` build では `sprite_demo` が同じ形で飛ばされます |
| 宣言しなくても止まるもの | `requires` はプロジェクトを開く時点の前倒しです。宣言が無くても、機能を外したビルドでその機能のコンポーネントを持つオブジェクトは、シーンロードの prepare と、エディタの `add_component` / `spawn` で同じ `<語>.module_unavailable@1` になります(今日対応付けがあるのは `collider` → `physics`、`animation` → `animation`。[第4章 §4.3](04_scene_ecs.md)) |
| 出荷での宣言 | `animation`: `animgraph_demo` / `vrm_xr_demo` / `posegraph_demo`。`physics`: `sprite_demo`(PG-1)。`audio` / `xr` / `vat` / `seqplayer` を宣言した出荷プロジェクトはまだありません |

> **3 つの `requires` は別物です(混同注意):**
> 1. **project.json の `requires`**(この節)= **ビルド機能ユニット**の名前の配列。
> 2. **package manifest の `requires`**(§3.11)= `{engine_min_version, sdk_abi, capabilities}` という object。エンジン版と SDK エポックの要求です。ただし中の `capabilities` だけは、project.json の `requires` と**同じ 6 語の語彙**で install 時に検査されます(✅PG-1)。
> 3. **feature 文書の `integrations[].requires`**([第6章](06_rendering.md))= その統合を適用する条件としての**他の feature の名前**。
>
> 綴りが同じだけで、形も検証者も別です(語彙を共有するのは 1 と、2 の中の `capabilities` だけ)。

### hot reload で監視するもの(`hot_reload`)✅HW-1

> **設計決定(2026-10-02 利用者):** hot reload を**素材やフォルダごとに有効にできる**形にする。既定(何も宣言していない project を Studio で開いたとき)は監視しない。外のツール(DCC)で書き換える場所だけを作者が宣言し、Studio の下でもそこだけを監視する。

Studio は engine を `--rpc` で起こし、`--rpc` の engine は宣言が無ければファイルを監視しません([第10章](10_tools.md) §10.5)。そのため Studio で開いている間は、外で保存した texture・model・material が反映されませんでした。`hot_reload` に書いた場所は、`--rpc` の下でも監視されます。

```json
{ "schema": "pelican.project", "version": 1, "name": "My Game",
  "hot_reload": ["assets/textures", "assets/models/hero.glb", "shaders"] }
```

| `hot_reload` | 単体の player(`--rpc` なし、窓あり) | `--rpc`(Studio の engine) |
|---|---|---|
| 書かない | 今までどおり: project root と全 store を監視 | 今までどおり: 監視しない(`get_status.reload.error` が `"rpc driver"`) |
| 配列 | 列挙したものだけ監視 | 列挙したものだけ監視 |
| 空の配列 `[]` | 監視しない(`"hot_reload empty"`) | 同左 |

- **読み方**: 区切りは `/`(`\` も区切りとして読みます)。`.` と空の成分は落とすので、`"./shaders"` は `"shaders"`、`"assets/textures/"` は `"assets/textures"` になります([`parseProjectHotReload()`](../../src/project/projectformat.cpp#L168))。`get_status.reload.watch` に出るのはこの正規化した形です。
- **当たり判定**: 素材の論理 path(store の mount の下も mount を含めて書きます。例 `art/characters`)が要素と等しいか、要素をフォルダとしてその下にあれば監視します。比べるのは path の成分ごとで、`assets/tex` は `assets/textures/a.png` に当たりません。大文字小文字は区別します([`watchPathCovers()`](../../src/core/watch/filewatcher.cpp#L383))。
- 宣言の外のファイルは走査せず、hash も reload もしません。宣言に届かない mount の store は監視自体を張りません。
- 存在しない path は拒否しません。`.pelican/` の下は宣言しても監視しません(engine のキャッシュ置き場は常に対象外。[第10章](10_tools.md) §10.5)。
- `--replay` / `--strict-assets` / headless では、宣言があっても監視しません([gate の 1 か所](../../src/core/watch/reloadgate.cpp#L37))。
- **配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`。`pelican_cli dist-config` が常に出す)では、宣言は読みます(下の拒否は同じ)が効きません**。監視そのものが無く、宣言があれば起動時に INFO を 1 行 `project.json hot_reload is ignored: this binary was built with PELICAN_WITH_HOT_RELOAD=OFF` 出して、そのまま開きます(✅AV-1。[第10章](10_tools.md) §10.5)。
- `--rpc` の下で宣言した場所から反映されるのは、texture・material の値の文書・model・shader とその依存です。**render 文書(rendering config / preset / feature)は watcher からは反映されません**。`--rpc` の engine では render 文書は `reload_render_documents`(Studio が送ります。[第13章](13_editor.md))が持つためです([除外する所](../../src/core/watch/reloadservice.cpp#L920))。
- **宣言は起動時に読みます**([`ReloadService::setup()`](../../src/core/watch/reloadservice.cpp#L882))。project.json を手で書き換えたときは engine を起こし直してください。✅PJ-1(`89643cd7`)から、**動いている engine には RPC `edit_project_settings` で宣言を差し替えられます**。engine が project.json に書き、その場で watcher を作り直します(§3.4 の「起動中の engine に宣言を足す」)。✅HW-2s(`78737fdf`)から、Studio では **Assets のドック**(View > Assets)の行ごとの **Reload when saved outside** がこれを呼びます([第13章](13_editor.md) §13.1 の「Studio の Assets のドック」)。

拒否は 1 つの code `project.hot_reload_invalid@1` で、文言が種類を言います。検査は封筒の parser(`parseProjectEnvelopeJson()`)の中の 1 か所なので、player の起動・engine の `basicconfig.cpp`・`pelican_cli dist-config`・Studio の project の読み込み([project.cpp](../../src/devstudio/model/project.cpp#L145))など、この parser を通る読み手はどれも同じ文言で断ります。

| 書いたもの | 文言 |
|---|---|
| 配列でない | `project.hot_reload_invalid@1: project.json hot_reload must be an array of project-relative paths` |
| 文字列でない要素 | `project.hot_reload_invalid@1: project.json hot_reload[1] must be a string` |
| 正規化すると空(`""`・`"."`・`"./"`) | `project.hot_reload_invalid@1: project.json hot_reload[0] is empty; name a folder or file under the project root` |
| 絶対 path(先頭が `/` か `\`、`C:` の形、`://` を含む。`project://assets` も) | `project.hot_reload_invalid@1: project.json hot_reload[0] '/assets' must be a path relative to the project root` |
| `..` を含む | `project.hot_reload_invalid@1: project.json hot_reload[0] 'assets/../outside' must not contain '..'` |
| 正規化して重複 | `project.hot_reload_invalid@1: project.json hot_reload[1] 'assets/textures/' repeats hot_reload[0] 'assets/textures'` |

`"."`(project 全体)は空として断ります。project 全体を監視するのは「書かない」ときの単体の player だけで、`--rpc` の下で広く監視したいときは最上位のフォルダを並べます。web ビューアがこのキーをどう扱うかは確かめていません(未確認)。

### basic_config と「3 段合成」

> **設計決定:** 設定の優先順位は **CLI 設定 > project.json > 埋め込み既定値(`engine://default_config.json`)** の 3 段で、キー単位に上位が勝つ。プロジェクトは差分だけ書けばよい。3 段すべてに無い必須キーは `config not found: ...` で hard error。

ここでいう**合成**はファイルの連結ではなく、**キーごとに 1 つの値を選ぶ**ことです([用語集](14_glossary.md))。たとえば project.json の `basic_config` に `window_title` だけを書けば、`window_title` はプロジェクトの値、`framerate` や `window_size` は埋め込み既定値(60 / 1920×1080)になります。

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
| `rendering_config_json` | 参照 | `"passes/main_rendering_config.json"` | レンダリング構成への参照(下記「rendering config の root」) |
| `default_rendering_pass` | string | `"main_render"` | rendering config 内で使う `rendering_passes[].name`(graph root では `rendering.rendering_passes[].name`) |
| `ui_config_json` | 参照 | `"ui/ui_overlay.json"` | UI ドキュメント(pelican.ui)への参照 |
| `input_actions_json` | 参照 | **既定なし(任意)** | 入力アクション定義。未指定ならアクション層は未構成 |
| `input_profiles` | object | — | バインディングプロファイルの辞書(名前 → ファイル参照。✅WP91 — [第7章](07_input_ui.md)) |
| `input_profile` | string | — | 既定プロファイル名 |
| `sprite` | object | `{"pixels_per_unit": 100.0}` | 2D スプライトの寸法変換係数(✅WP106 — [第6章](06_rendering.md) §6.9) |
| `camera.sprite` | object | `{"pixel_perfect":"off","sort":"z"}` | スプライトの pixel perfect / ソートポリシー(camera 内。シーン側 camera コンポーネントにも書ける) |

`camera` は glTF のカメラ定義と同形です。`type` は `"perspective"`(既定)か `"orthographic"` のみ。perspective は `yfov`(**ラジアン**)/`znear`/`zfar` 必須・`aspect` 任意、orthographic は `xmag`/`ymag`/`znear`/`zfar` 必須です。

**`camera.up` は撤去されました(✅CC-1、`c8367b87`)。** camera の向きは glTF と同じ規約になり、回転なしの camera は −Z を向き、up は transform の回転の +Y です(world の上は常に +Y。[第6章 §6.1](06_rendering.md) の「座標の規約」)。`up` を書いた project.json は、どの値でも(配列でなくても)次の名前で開けません。検査は envelope の `schema` / `version` の検査の直後です([project.json で断る所](../../src/project/projectformat.cpp#L250)。project.json 以外の経路で渡った config も [`rejectRemovedCameraUp()`](../../src/core/loader/basicconfig.cpp#L247) が同じ名前で断ります):

```
project.camera_up_removed@1: project.json basic_config.camera.up was removed: the camera convention changed (a camera looks along -Z of its rotation, its up is the rotation's +Y and the world's up is +Y). Convert the project with `uv run tools/convert_camera_convention.py <project dir>`, which removes the key and turns its cameras to look the way they did
```

CC-1 より前の project.json は `up` が必須だったので、古い project はここで必ず止まります。`pelican.project` の `version` は 1 のままで、移行の手順(§3.13)も登録していません(v0 の前なので。[設計 §4](../design_coordinate_convention.md))。直し方は §3.13 の「camera の向きの規約の変換」です。scene に camera が無いときの既定の視点は、原点から −Z を見る向きです(CC-1 より前は +X)。

> **設計決定(fail-fast):** 旧キー名 `fov_y` / `near` / `far` は「黙って無視」ではなく `'fov_y' is not supported in v1; use 'yfov' (radians)` という**移行先を案内する名指しエラー**で拒否される(WP63)。「読めるふりをしない」— 警告止まりだと『動いたように見えて新機能の挙動だけ壊れる』事故になるため。互換受理の追加はプロジェクト規則で禁止されており、旧形式の変換は外部ツールの仕事。

### rendering config の root(✅P-2a-4a/4b, MG-2b)

`rendering_config_json` が指すファイルは **`pelican.graph` root** です。最上位は共通グラフの 5 キー(`schema` / `version` / `definitions` / `nodes` / `outputs`)**だけ**で、rendering config 全体が `rendering` の下に入ります。出荷 7 本すべてと、`pelican_cli project init` が生成する新規プロジェクトがこの形です。共通グラフのノード(`graph.instance` を含む)は 5 キーの側に書きます([第4章 §4.1](04_scene_ecs.md))。

```json
{ "schema": "pelican.graph", "version": 1,
  "definitions": {}, "nodes": [], "outputs": {},
  "rendering": { "features": [...], "render_targets": [...], "rendering_passes": [...] } }
```

規則は 1 つです —— 上の 5 キーは共通グラフが所有し、**それ以外の rendering config のキーは全部 `rendering` 節に属します**。キーごとの所有は [第6章 §6.16](06_rendering.md) にあります。

- **`features` / `render_targets` / `rendering_passes` などを最上位に直接並べた旧い形(legacy root)は、MG-2b から読めません。** 起動時の読み込み([renderingpassconfigloader.cpp](../../src/core/renderingpass/renderingpassconfigloader.cpp#L24))、`--feature-overlay` の重ね合わせ、ray tracing の設定の読み込み、Studio / RPC の編集文書のどれもが `render_config.legacy_root@1` で断ります([`RenderConfigLegacyRoot`](../../src/project/renderconfigdocument.hpp#L37))。直すには、旧いファイル全体を `rendering` の値にして上の 5 キーを足します。`rendering` の中身は旧い root と同じ意味で読まれます。
- **`rendering` 節には `buffers` / `compute_tasks` を書けません。** この 2 つは共通グラフのノード(`render.buffer` / `render.compute`)の接続から lowering が作るので、節に書くと `render graph derives field from connections: buffers` のように拒否されます([lowerRenderGraphDocument()](../../src/project/rendergraphcomposition.cpp#L1354))。buffer と compute task は graph node か task feature で書きます。

`pelican.graph` root は web ビューア未対応です。web は v0 形式への追従中(WEB-1)で、それまで web は新しい出荷 sample を開けません([第9章](09_web.md) §9.3)。

## 3.3 パス参照と PathResolver

すべてのコンテンツ参照(JSON 内・CLI のコンテンツ引数)は同じ解決規則を通ります。ここでいう **resolver(解決器)** は、JSON に書かれた参照文字列 1 本(`assets/models/sponza.glb` など)を実ファイルの絶対パス 1 本に変換する部品のことです。規則の正本は `pelican_project` の [ProjectPathResolver](../../src/project/projectpathresolver.hpp)(実装: [projectpathresolver.cpp](../../src/project/projectpathresolver.cpp))です。engine の **PathResolver** モジュール([pathresolver.cpp](../../src/core/loader/pathresolver.cpp))はこれへ委譲し、module寿命、quillログ、`engine://`埋め込みresource loaderだけを接続します。

この分離によりdevstudioや外部ツールは`pelican_core`をリンクせず、engineと同じproject root、asset store、escape防止、fragment規則を使えます。絶対CLI pathを許可したwarningも純粋層の戻り値であり、純粋層自身はログを出しません。

`ProjectPathResolver::setup()` はraw JSONではなく解析済み`ProjectEnvelope`を受けます。`name`と`asset_stores`の既定解釈をpath層で繰り返さないためです。

### 参照の分類

| 書き方 | 解決先 | 状態 |
|---|---|---|
| `assets/a.glb`(素の相対) | プロジェクトルート(= `project.json` があるディレクトリ。§3.1)基準 | ✅実装済み |
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

`engine://<id>` はエンジンバイナリに埋め込まれたリソース(既定設定・標準シェーダ・feature 定義・フォント等)を指します。id は埋め込み元 `src/core/resources/` からの相対パスで、現在 **104 エントリ**が [engineresources.cpp](../../src/core/loader/engineresources.cpp) に登録されています(実数はフィクスチャ `test/fixtures/project_format/engine_resources.json` の `ids` 配列が正)。

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

- 意味論は**マウント**: `assets/**` への参照を store の実体ディレクトリに張り替えます。**シーン等の参照の書き方は一切変わりません**(解決レイヤだけの機能)。具体例: 上の宣言に加えて後述の `.pelican/local.json` で `main` の実体を `D:/shared_assets/mygame` にすると、JSON に書かれた `assets/models/sponza.glb` は先頭の `assets/` が剥がされ、`D:/shared_assets/mygame/models/sponza.glb` を読みます([projectpathresolver.cpp](../../src/project/projectpathresolver.cpp) の store 分岐)。store 宣言が無ければ同じ参照が `<プロジェクトルート>/assets/models/sponza.glb` になります。
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
  - ✅AV-1(`5f7c5cdb`)から、照合の結果は store ごとに engine に残り、RPC で読めます(`get_status.assets_verification` の件数、`get_assets_verification` の全件、走らせ直す `verify_assets`、manifest を書き直す `update_assets_manifest`。形は [第10章](10_tools.md) §10.3 の「起動時の asset 検証の結果」)。**配布 build(`PELICAN_WITH_HOT_RELOAD=OFF`)は起動時に照合しません**。`--strict-assets` を付けたときだけ照合し、食い違えば止まります(2026-10-02 利用者決定)。照合しなかったときの `get_status.assets_verification` は `{"state":"not_run","reason":"PELICAN_WITH_HOT_RELOAD=OFF"}` です。
  - ✅AH-1(`0f93b5b8`)から、hash は file を 1 MiB ずつ読んで計算し、Windows では BCrypt の SHA-256 を使います([`assetFileSha256()`](../../src/project/assetsmanifest.cpp#L162)。計算そのものは ✅FW-1 から hot reload の file watcher と共有する [`Sha256Stream`](../../src/project/sha256stream.hpp#L15) にあります)。digest は前と同じなので、今の manifest と hash cache はそのまま使えます。commit の記述では、hash cache の無い状態の Debug の `projects/example`(約 70 MB)で、検証が 23〜31 s から 0.5〜0.75 s になりました。それまでは Studio が engine の窓を見つけるまでの待ち(15 s)より長く、cache の無い worktree で Studio の test「running example fatal text」を走らせると、致命の行が出る前に engine が kill されていました。
  - hash を始める前に、hash の要る file があれば `assets manifest [<store>] verifying <N> assets (<X> MB), <M> cached` を 1 行出します(N は hash する数、M は cache が当たった数。全部 cache が当たったときは出しません。[出す所](../../src/core/loader/assetsverification.cpp#L54))。

> **設計決定(検証はロードを止めない):** アセットの正当性検査は開発の補助であり、既定では警告に留める。厳格化は明示フラグ(開発 CI・配布検証)でのみ行う。

example の README にあった手書き sha256 表の役目は、この manifest([../../projects/example/assets.manifest.json](../../projects/example/assets.manifest.json)・26 エントリ)に置き換わりました。

### 起動中の engine に宣言を足す(✅PJ-1)

✅PJ-1(`89643cd7`)から、動いている engine に RPC で `asset_stores` と `hot_reload` を書かせ、engine を起こし直さずに反映できます。読むのは `get_project_settings {}`、書くのは `edit_project_settings {base_project_digest, hot_reload?, asset_stores?}` です。返り値と拒否の形は [第10章](10_tools.md) §10.3 の「project.json の宣言を書き換える」にあります([PJ-1 の書き込みと反映の handler](../../src/core/communication/projectsettingsrpc.cpp#L93))。

背景: `pelican_cli project init` の Blank / Standard の雛形は `assets/` を作るだけで、`asset_stores` も `hot_reload` も宣言しません。そのため hot reload の宣言(HW-1)と素材の照合(AV-1)は、project.json を手で書いて engine を起こし直さないと効きませんでした。2026-10-03 に利用者が「Studio のボタンで宣言を足す」形を承認し(HW-2 / AV-1s)、PJ-1 はその engine 側です。Studio の側は ✅HW-2s / AV-1s(`78737fdf`)の **Assets のドック**で、行ごとの Reload when saved outside が `hot_reload` を、**Track asset contents…** が `asset_stores.main = {mount: "assets/", manifest: "assets.manifest.json"}`(store が既にあれば manifest の無い store に manifest だけ)を、それぞれ 1 回の `edit_project_settings` で書き、ドックの Undo が答えの `previous` を送り返します([第13章](13_editor.md) §13.1 の「Studio の Assets のドック」)。

- **書き方**: 鍵を省けば今の値のまま、`null` を渡せば鍵ごと消します。`hot_reload` は配列の置き換えで、HW-1 と同じ検査(上の「hot reload で監視するもの」)を通した正規化の形で書きます。`asset_stores` は宣言の全体を渡します。
- **project.json への書き込み**は project の文書の transaction で、`base_project_digest` を CAS に使います。ほかの鍵と並びは保ち、新しい鍵は末尾に足します。書くと全体が 2 空白の indent に展開されるので、値を元に戻しても digest は元に戻りません。改行コードは元のファイルのものを使います([書く形を作る所](../../src/core/communication/projectsettingsrpc.cpp#L62))。値が変わらない要求は書かず、`changed: false` を返します。
- **受ける変更**は次の 2 つだけです。
  - store を足す。足した store の root は、mount が名指す project のフォルダでなければなりません。そうであれば、mount の下の参照は store を足す前と同じファイルに解けます。`.pelican/local.json` で別の場所に向けた store は断ります。
  - 起動時から manifest の無い store に manifest を宣言する。manifest のファイルがまだ無ければ engine が今のディスクから書き(`update_assets_manifest` と同じ書き方)、照合を走らせ直します([第10章](10_tools.md) §10.3)。
- **断る変更**(`project.settings_change_unsupported@1 {key, store?, reason}`。project.json は変わりません)は、engine が開いたときからある宣言を動かすものです: mount の移動、`.pelican/local.json` による store の root の移動、起動時からある store の削除、起動時からある manifest の削除と差し替え。どれも、参照の解ける先か照合の基準が変わるためです。理由の文言はどれも `restart the engine` で終わります([断る判定](../../src/project/projectpathresolver.cpp#L544))。
- **戻す**: 返り値の `previous` をそのまま渡せば戻ります。外せるのは、この engine の実行中にこの RPC で足した store と manifest だけです(store と一緒に足した manifest は store ごと外れます)。
- 照合が終わったことの通知はありません。`get_status.assets_verification.state` が `running` でなくなるのを待ちます。

残る穴(着地時の記録):

- manifest を書いている間と、走っている照合の終わりを待つ間は frame が止まります。
- 作り直した watcher は、作り直す前に見張っていなかった間の外の変更を読み直しません。
- mount で始まり `..` で mount の外へ出る参照(例 `assets/../scenes/a.json`)は、store を足すと解けなくなります。store を通る参照は store の root の外に出られないためです(`Asset store path escapes mount root ...`。上の「脱出防止の基準点は store 経由の参照では mount root に付け替わります」)。
- resolver の宣言の差し替えは lock を取りません。main thread の frame の境目で呼ぶことが前提です。

## 3.5 解釈レイヤ — pelican_project とバインダの分離

> **設計決定(3 層構造):** プロジェクト形式の解釈は次の 3 層に分かれる。
> ```
> プロジェクト形式(ファイル群)
>   ↓ 解釈レイヤ: pelican_project 静的ライブラリ(エンジン非依存の純ロジック。依存は nlohmann_json と picosha2 のみ)
>   ↓ バインダ: src/core/loader のエンジン側変換(GET_MODULE を呼べるのはここと起動配線のみ)
> ゲームエンジン本体
> ```
> 形式に機能を足すときは「解釈レイヤにパーサ+バインダに変換」を対で追加し、エンジン内部へ波及させない。パーサ登録は静的な表で行い、プラグイン機構は作らない(過剰抽象化の防止)。

[src/project/](../../src/project) が解釈レイヤの実体で(CMake ターゲット `pelican_project`、WP44/WP248)です。現在の住人は `projectformat`(pelican.project封筒)/ `projectpathresolver`(path・asset store)/ `sceneformat`(pelican.scene)/ `importmanifest`(pelican.import)/ `materialformat` + `surfaceformat` + `materiallowering`(マテリアル)/ `featurecompose`(feature 合成)/ `jsonrpc`(JSON-RPC エンベロープ)/ `assetsmanifest`(pelican.assets)/ `importrules`(pelican.import_rules)などです。vulkan・quill・モジュール機構・リソース埋め込みへの依存は禁止されています。`project_library_boundary`テストは、製品ライブラリとして`pelican_project`だけをリンクした実行ファイルがexampleのproject.jsonを開き、scene一覧とasset一覧を取得できることを固定します。

## 3.6 バージョン管理(VCS)の方針

- エンジンリポジトリは example の JSON と README のみコミットし、バイナリは `.gitignore` で除外(クローンを重くしない)。ユーザープロジェクトでの LFS 利用は自由です。
- store 運用の 3 段構え: 小規模 = リポジトリ内+LFS / 中規模 = 外部 store+人間運用 / 大規模 = DAM(デジタルアセット管理)。共有 store にブランチという概念は持ち込みません(バージョン管理は本業ツールに委譲)。
- 大規模統合は「API ではなくデータ契約」: エンジンが信じるのは ① store の実体、② assets manifest(✅WP66)、③ `imports/` + pelican.import manifest の 3 つだけです。DAM や Perforce は、契約どおりにファイルを書けば `pelican_cli` を置き換えられます。

## 3.7 サブセット原則(web との関係)

> **設計決定(サブセット原則):** 「web で開けるプロジェクト ⊆ pelican で開けるプロジェクト」。形式の拡張は常にエンジン側が先行し、web は「無視 → 対応」の順で追随する。web 専用のキー・参照形式・`engine://` id は作らない。web が pelican より緩い受理をしたら規約違反。

この原則の詳細と web ビューアでの開き方は [第9章](09_web.md) を参照してください。

「エンジン先行」の現在地(2026-09-04 にコードを読んで確認):**§3.10 の `Flags` / `List` 行封筒も、レイトレの `pelican_ray_instance`([第6章](06_rendering.md) §6.15)も、web 側パーサはまだ知りません。** web の scene パーサが closed schema を持つのは組み込み 7 コンポーネント(`transform` / `light` / `camera` / `simplemodelview` / `collider` / `animation` / `sprite_view`)だけで、codec を持たない名前のコンポーネントは検証せず `opaque` として素通しします(`my_webpage/packages/pelican-webgpu/src/pelican2/componentCodec.ts` の codec 集合と `sceneFormat.ts` の `bindPelicanScene()`。2026-09-25 に web リポジトリの作業 branch `agent/ww10a` で関数と `opaque` の分岐があることを再確認。web の `main` にはこのファイル自体がまだ merge されていません)。したがって `pelican_ray_instance` や `List` フィールドを持つコンポーネントを書いた scene は、**native では scene load 時に検証・拒否されるのに web では黙って通る**状態で、「明示拒否まで実装済み」ではありません。サブセット原則の向きとしては web が緩い側なので、web 側の追随待ちの項目です(web リポジトリは pelican2 の外にあるため、この 1 段落だけは pelican2 のコードでは検証できません)。

## 3.8 既知の食い違い(設計文書 vs 実装)

調査(2026-07-10)で判明した主な差分です。**コードが正**として読んでください。全体の一覧は [第11章](11_status.md) にあります。

- `--play-seq` / `--seq-mesh` の相対パスは設計([PF] はプロジェクトルート基準)に反し **cwd 基準**で解決され、絶対パスもフラグなしで通ります(`--play-vat` は設計どおり)。
- 設計上必須の `name` を実装は検証しません(欠落すると黙って `user://` が無効になります)。
- `PathResolver::resolveCliRef`(CLI 文脈用 API)は本番の呼び出し元がまだありません。
- [PF] 本文のコード例(`ResolvedRef` の variant 要素数、レジストリのエントリ数など)は v6.3 の追加機能に未追随の箇所があります。

## 3.9 シーンドキュメントと保存(エディタ経路)(✅WP149/158/166/168)

ここまでの節はすべて「ディスク上のファイル形式」の話でした。この節は、その形式をエンジンが**メモリ上でどう保持し、どう書き戻すか**を扱います。以下 **authoring(オーサリング)側**は「人やエディタが編集する JSON そのもの」を、**runtime(ランタイム)側**は「エンジンが実行のために作った ECS の状態」を指します([用語集](14_glossary.md))。**プロジェクト形式(JSON の書き方)は一切変わりません**。エディタ機能を使わないなら読み飛ばして構いません。

> **設計決定(編集の正本は authored JSON):** 編集の正本は `basic_config.scene_data_json` が指すシーン JSON の内容そのもの(`AuthoringSceneDocument`)であり、ランタイムの ECS はその**投影**である。保存はランタイム状態の serialize ではなく、authored ドキュメント全体の決定的エンコードとファイル置換で行う。これにより、エンジンが解釈しないコンポーネント(codec 未登録のもの)の raw JSON も保存で失われない。

`ProjectBasicConfig`([basicconfig.hpp](../../src/core/loader/basicconfig.hpp))が唯一の保持者です。

- **起動時**: PathResolver で読んだシーン JSON の bytes をそのまま `AuthoringSceneDocument` にし、同時にその**ディスク baseline digest** を記録します。
- `SceneRevision` は**ドキュメント全体に 1 つ**の版番号で、編集がコミットされるたびに単調増加します(オブジェクト単位ではありません)。`AuthoringObjectId` はセッション内で安定した object 識別子で、無名オブジェクト・ライトのみ・コライダのみのオブジェクトにも付きます(ランタイムの `EntityId` は nullable な投影であり、同一性の代用にはしません)。
- API は `sceneDocument()` / `updateSceneDocument()` / `importSceneDocument(bytes, reload)` / `saveSceneDocument()` の 4 本です。

### 保存 — 全文書のアトミック置換

`saveSceneDocument()`([basicconfig.cpp](../../src/core/loader/basicconfig.cpp))の手順です。

1. ドキュメント全体を 1 回だけエンコードする(以降のすべての段はこの同じ bytes を消費します)。
2. 記録済み baseline digest と**現在のディスク**の digest を照合する。
3. 同じディレクトリの一時ファイルへ書き出して flush する。
4. 読み返して byte 一致を検査する。
5. 一時ファイルを再 parse + 意味検証し、元ドキュメントと一致するか検査する。
6. 次リビジョンのドキュメントを準備し、digest と byte 数を確定する。
7. **置換直前にもう一度**ディスクの digest を照合する(TOCTOU 窓を閉じる)。
8. アトミックにファイルを置換し、例外を出さない区間でキャッシュとリビジョンを公開する。

エラーは 3 種です(`SceneSaveErrorCode`):

| コード | 意味 |
|---|---|
| `ExternalModification` | エディタの外でシーンファイルが書き換わっていた(2 と 7 のどちらで検出しても同じコード)。**上書きせずに失敗します** |
| `Unavailable` | baseline digest が確立していない(ファイル由来でないシーンソースなど) |
| `IoFailure` | 一時ファイルの書き戻し検査・意味検証に失敗した |

> **設計決定(黙って上書きしない):** 保存はディスクの現在値を 2 回照合し、食い違いがあれば `ExternalModification` で中止する。エディタと外部エディタ/git 操作の競合を「後勝ちの静かな消失」にしないため。

### 保存で変わること・変わらないこと

- **キー順は辞書順に正規化されます**(オブジェクトのキー順は保存されません)。**配列の宣言順と JSON のスカラ型はそのまま保存されます**。手書きの JSON をエディタで一度保存すると、キーの並びだけが変わります。
- codec が登録されていないコンポーネントも、raw JSON がそのまま保持されます(ドキュメント全体をエンコードするため構造的にデータ欠落が起きません)。

### スナップショットの取り込み

`importSceneDocument(bytes, reload)` は、外部から渡されたシーン JSON を**メモリ上のシーンソースとして差し替えて**リロードします。**ディスクの baseline は保持され、ファイルには何も書きません**。取り込み先で `AuthoringObjectId` は採番し直されます。

### シーンはホットリロード対象ではない

シーン JSON はファイル監視の対象外です([第10章](10_tools.md) §10.5)。書き戻しの経路は保存 API(rpc の `save_scene`)だけで、これは rpc の `get_status` にも `scene_source: {"hot_reload": false, "save_method": "save_scene"}` として現れます。

### transform を 1 つ変える編集は、受付の検査を実行時に使い回す(`8bde3c2f`)

利用者の codex が書いた変更です(handoff 01)。編集(`edit`)の ticket は、受付の時に操作を使い捨ての文書へ当ててみて検査し、フレーム境界で実行するときにもう一度同じ検査をしていました。次の条件をすべて満たす編集では、受付で作った検査済みの結果を実行時にそのまま使います([受付で検査結果を残す所](../../src/core/communication/editorjournal.cpp#L4506)、[実行時に使い回す所](../../src/core/communication/editorjournal.cpp#L4181))。

- 操作が 1 つで、`set_component_value` の `component_slot: "transform"`、宛先を `object_id` で指す(`address` ではない)
- シーン文書が prefab を使っていない
- 受付の前後と実行時で、authoring の文書と resolved scene の組が同じもの

組が同じかは revision や resolver の世代の番号ではなく、組を作るたびに新しく作る識別子([`snapshot_identity_`](../../src/core/loader/resolvedscene.hpp#L331))で見ます。番号が同じ別の組に置き換わっていれば、今までどおり検査し直します。受付の検査は全部残り、実行時の gate・scene・revision・CAS・preview lease の検査と、実際の投影の commit も残ります(コードの注記)。構造の変更・behavior・prefab・複数の操作の編集は今までの道です。

codex の報告と commit の説明では、10,000 object のシーンで transform の編集 1 回(受付 + `step_frame` での適用 + 結果の取得)が 1,102 ms → 751 ms です(Release、前後 3 組の中央値)。改善後も数百 ms かかり、対象を絞った作り直しは今後の課題と報告にあります。test: `editorjournal_test` の [「Transform preflight reuse preserves commit journal undo and redo」](../../test/editorjournal_test.cpp#L2454) ほか `Transform preflight` で始まる 5 本(失敗の扱い、番号が同じ別の組、古くなった待ちの編集、受付の後に取られた preview lease)。

編集操作・リビジョンの排他制御・プレビュー・rpc メソッドの詳細は [第10章](10_tools.md) と [第13章](13_editor.md) を参照してください。

## 3.10 コンポーネント宣言スキーマ v3 — `Flags` と `List` の wire(✅WP382a)

§3.9 まではドキュメント全体の話でした。この節は **1 個のコンポーネントの中身**をどう書くかです。ここでの **v3 はスキーマ宣言機構の版番号**で、シーン JSON の `version`(pelican.scene v2)とは別物です([用語集](14_glossary.md))。従来の登録経路が運べたのは平坦なフィールド列だけで、`Flags` / `List` のように**入れ子の語彙を持つ型**を運べるようにしたのが v3 です。対象は**ゲームコード側が登録するコンポーネント**(と event payload / behavior params)で、エンジン組み込みの codec 表のコンポーネント(既定 7 件 + ray layout v2 のときだけ増える `pelican_ray_instance`。[componentcodec.cpp](../../src/core/loader/componentcodec.cpp#L2009))はこの節の対象外です(平坦なフィールド列のまま)。

登録側の `schema` に `Flags<E>` か `List<Row>` が 1 つでも含まれると、登録は自動的に **v3 の入口**へ切り替わります([registerer.hpp](../../src/core/userpublic/details/component/registerer.hpp#L188))。判定は `schemaRequiresV3()` のコンパイル時定数なので、含まないコンポーネントの wire は 1 バイトも変わりません。scene JSON からの入口は従来と同じ「コンポーネント JSON をそのまま渡す」経路です([`ComponentInfoManager::loadByJson()`](../../src/core/ecs/componentinfo.cpp#L189))。

`Flags<E>` / `List<Row>` は C++ 側の**フィールド型**です。フィクスチャ `test/fixtures/wp382a_schema/v3_fixture.cpp` の宣言を抜粋すると、下の JSON 例と 1 対 1 に対応します:

```cpp
enum class InstanceFlag : std::uint32_t { CullDisable = 1u, FlipFacing = 2u };

struct OverrideRow {                      // List の 1 行ぶんの型
    std::string occurrence_id;
    std::optional<std::uint8_t> mask;
    std::optional<Pelican::Flags<InstanceFlag>> instance_flags;
    static constexpr auto schema = Pelican::structFields(/* ... */);
};

struct CarrierParams {
    Pelican::List<OverrideRow> overrides;  // ← この型が v3 の入口を選ばせる
    static constexpr auto schema = Pelican::structFields(/* ... */);
};
```

つまり `E` は名前を宣言した enum、`Row` は行スキーマを持つ struct です。

### `Flags` — 宣言済み名の文字列配列

```json
{"name": "my_component", "instance_flags": ["cull_disable", "flip_facing"]}
```

空配列 `[]` も「明示的に空」という値で、キーごと省略するのとは別物です。重複する名前・宣言されていない名前はどちらも `InvalidEnum` で拒否されます([structfieldjson.hpp](../../src/core/userpublic/details/schema/structfieldjson.hpp#L423))。

### `List` — object コンテナ + 行封筒

素の配列は書けません。`List` フィールドは必ず次の形です:

```json
"overrides": {
  "$list": {"v": 1, "next_id": 3},
  "rows": [
    {"$row": {"id": 1, "v": 1, "hint": {"node": "chair"}}, "occurrence_id": "occ-a", "mask": 0},
    {"$row": {"id": 2, "v": 1}, "occurrence_id": "occ-b", "custom_index": 7}
  ]
}
```

- コンテナ直下に置けるキーは `$list` と `rows` の 2 つだけ。`rows` は配列必須です。
- `$list` は `v` と `next_id` のちょうど 2 キー。`$row` は `id` / `v` 必須 + `hint`(object)任意。
- 行の中身は `$row` を剥がした残りのキーで、行スキーマとして検証されます。
- `hint` は object のまま保存され、エンジンは中身を解釈しません(オーサリング側の覚え書き用)。

> **設計決定(行の同一性は id であって添字ではない):** 行の identity は配列添字ではなく `$row.id` が持つ。`$list.next_id` は「次に採番する id」で、**現存するどの行 id よりも大きい**ことが検証される。添字を identity にすると、行の挿入や並べ替えが外部からの参照を静かに壊すため。

失敗は名前付きです([structfieldjson.hpp](../../src/core/userpublic/details/schema/structfieldjson.hpp#L589)):

| コード | 起きる条件 |
|---|---|
| `ListContainerMissing` | `List` フィールドが object でない(素の配列を書いた) |
| `RowEnvelopeMissing` | `$list` が無い / `$list` に `v` か `next_id` が無い / 行に `$row` が無い / `$row` に `id` か `v` が無い |
| `RowEnvelopeUnsupported` | `$list.v` または `$row.v` が 1 以外 |
| `DuplicateRowId` | 同じ `$row.id` を持つ行が 2 つ以上ある |
| `OutOfRange` | `$row.id` が 0 / `next_id` が最大 id 以下 / id・`next_id` が U32 を超える |
| `UnknownField` | コンテナ・`$list`・`$row` のいずれかの封筒に未知キーがある |
| `TypeMismatch` | `rows` が配列でない / `id`・`next_id` が非負整数でない / `hint` が object でない |

### `$` で始まるフィールド名はエンジン予約

スキーマ宣言側で `$` 始まりの名前を使うとコンパイル時に弾かれ([structfieldschema.hpp](../../src/core/userpublic/details/schema/structfieldschema.hpp#L494))、JSON 側に未知の `$` キーがあると `UnknownField` ではなく `ReservedFieldName` で拒否されます([structfieldjson.hpp](../../src/core/userpublic/details/schema/structfieldjson.hpp#L774))。将来増える封筒キーを、利用者のフィールド名と衝突させないためです。

保存でこの形は往復します。エンコードは `$list` → `rows` の順、各行は `$row` → authored フィールドの順に書き出します([structfieldjson.hpp](../../src/core/userpublic/details/schema/structfieldjson.hpp#L589))。

> WP382dで`pelican_ray_instance.overrides`が`List`の利用者になりました。geometryごとの上書きを行ID付きで保存し、行を削除するとobject/layoutの値へ戻ります([第6章](06_rendering.md) §6.15)。

## 3.11 package — 本体ツリーの外から持ち込む(✅X-1)

> **設計決定:** 外から持ち込んだ部品は **project に複製(vendoring)してから使う**。engine は project の外を探しません。`packages/<namespace>/` に丸ごと複製し、`project.json.packages` に `{namespace, package_version, sha256}` を記録します。複製元を消してもプロジェクトは動きます。

### package とは

**package** は `package.json`(エンベロープ `pelican.package` v1)を持つ 1 つのディレクトリです。本体ツリーのどこにも置く必要がなく、別 repo として配布できます。雛形は [test/fixtures/package_template/](../../test/fixtures/package_template/) にあります。

```json
{
  "schema": "pelican.package",
  "version": 1,
  "namespace": "acme.glowkit",
  "package_version": "1.0.0",
  "description": "...",
  "requires": {
    "engine_min_version": "0.1.0",
    "sdk_abi": 2,
    "capabilities": []
  },
  "dependencies": [{"namespace": "acme.base", "package_version": "1.0.0"}],
  "provides": {
    "features": ["features/glow.json"],
    "graphs": ["graphs/pulse.json"],
    "components": [],
    "code": true
  }
}
```

| フィールド | 必須? | 検証・意味 |
|---|---|---|
| `schema` / `version` | **必須** | `"pelican.package"` / `1` ちょうど |
| `namespace` | **必須** | `.` 区切りの識別子(`[A-Za-z_][A-Za-z0-9_]*`)。複製先ディレクトリ名になり、feature 名の前置詞にもなる |
| `package_version` | **必須** | `"X.Y.Z"` |
| `description` | 任意 | 文字列 |
| `requires.engine_min_version` | **必須** | `"X.Y.Z"`。`project.json` と同じ規則で比較する([projectformat.cpp](../../src/project/projectformat.cpp) の `compareProjectVersions`) |
| `requires.sdk_abi` | code を持つときのみ**必須** | 整数。engine の `PELICAN_GAME_LOGIC_ABI_VERSION`(現在 **2**)と一致しなければ install で拒否 |
| `requires.capabilities` | 任意 | 文字列配列。§3.2 の `requires` と同じ 6 語の語彙で install 時に検査します(✅PG-1)。知らない語は `package.capability_unknown@1`、このビルドが OFF にした機能の語は `<語>.module_unavailable@1`。GPU capability の強制はここではなく feature 側の `required_capabilities` |
| `dependencies` | 任意 | `{namespace, package_version}` の配列。install 時に「その namespace が install 済みか」だけを見る |
| `provides.features` / `graphs` / `components` | 任意 | package 内の**相対パス**。`..`・絶対・`\`・スキーム付きは名前付き拒否 |
| `provides.code` | 任意 | `true` なら `code/CMakeLists.txt` を持つ |

未知のキーは読み飛ばさず `package_manifest_invalid` で拒否します(`pelican.import` と同じ厳しさ)。`project.json` の寛容さ(未知キーは読み飛ばし)とは意図的に違います。

### install — project への複製

```sh
pelican_cli package install <package dir> --project <project dir>
pelican_cli package list    --project <project dir>
pelican_cli package inspect <package dir>
```

エディタ経路では RPC `install_package{source_dir}` / `uninstall_package{namespace}` が同じことをします(`source_dir` は project の外の絶対パス)。

検査は**複製の前に全部**走り、どれも名前付きで拒否されます。

| 拒否名 | いつ |
|---|---|
| `package_manifest_missing` | `package.json` が無い |
| `package_manifest_invalid` | エンベロープ・型・未知キー・重複 |
| `package_namespace_invalid` | `namespace` が `.` 区切り識別子でない |
| `package_path_invalid` | `provides` のパスが相対でない / `..` を含む / スキーム付き |
| `package_engine_too_new` | `requires.engine_min_version` が engine より新しい |
| `package_sdk_abi_mismatch` | `requires.sdk_abi` が engine の SDK epoch と違う |
| `package.capability_unknown@1` | `requires.capabilities` に §3.2 の語彙に無い語がある(既知の一覧をメッセージに添える。✅PG-1) |
| `<語>.module_unavailable@1` | `requires.capabilities` の語が、このビルドで OFF の機能を指す(例: `PELICAN_WITH_AUDIO=OFF` で `audio`。未知語の検査を全要素について済ませてから出す。✅PG-1) |
| `package_dependency_missing` | `dependencies` の namespace が未 install(列挙する) |
| `package_feature_invalid` | `provides.features` の文書が `pelican.render_feature` v1 でない / `name` が無い / `name` が識別子でない / 名前が package 内で重複 |
| `package_already_installed` | 同じ namespace がもう入っている |
| `package_source_inside_project` | RPC に project 内のディレクトリを渡した(それは `project://` で参照するもの) |
| `package_in_use` | uninstall しようとしたが `project://packages/<ns>/...` への参照が残っている(列挙する) |
| `package_not_installed` | uninstall 対象が無い |
| `package_digest_mismatch` | 複製後の再計算が食い違った |

複製は `packages/<namespace>.tmp-<pid>` に書いてから rename します。中断しても半端な `packages/<namespace>/` は残りません。

### namespace は「文書の中」に入る

feature 名を比べる場所は catalog だけではありません。合成時の無音 dedupe・`integrations[].requires` の完全一致・`== "hdr"` の直値比較は、どれも**文書の `name` フィールド**を読みます([featurecompose.cpp](../../src/project/featurecompose.cpp))。そこで install は複製した feature 文書の `name` を書き換えます。

```
features/glow.json   "name": "glow"   →   packages/acme.glowkit/features/glow.json   "name": "acme.glowkit.glow"
```

shader define の前置詞も同じ名前から作られるので、`.` は `_` に写ります。

```
PELICAN_FEATURE_ACME_GLOWKIT_GLOW_<PARAM>
```

engine 同梱 shader は `PELICAN_FEATURE_BLOOM_THRESHOLD` のように**自分の名前で define を要求する**ものがあります。名前が変わる以上、そういう shader を package の feature から使い回すことはできません。雛形の `glow.json` が define を要求しない engine shader だけを使っているのはそのためです。

### 使う

複製先を `project://` で参照します。

```json
{
  "pipeline": { "preset": "engine://render_pipelines/hybrid_v1.json" },
  "features": [
    "engine://features/shadow_directional.json",
    "project://packages/acme.glowkit/features/glow.json"
  ]
}
```

`list_render_features` は engine 同梱の集合に加えて install 済み package の feature を返します。各エントリは `source` を持ち、値は `"engine"` か `"package"` です。package 側の列挙は**絶対に例外を投げません**。壊れた package 文書は `available: false` と `unavailable_reason` になるだけで、エディタサービスの構築は成功します(利用者が持ち込んだ bytes で project が開けなくなる、を構造で塞いでいます)。

`provides.graphs` は複製先を示すだけです。graph の `definitions` は**同一文書内でしか参照できません**(`identifier()` が `/` を拒否するので、file 参照は構造的に書けません。[graphcomposition.cpp](../../src/project/graphcomposition.cpp))。使いたい定義は project 自身の graph 文書へ複写してください。`get_graph_catalog` は package を見ません(catalog に載るのは組み込み module だけ、記録)。

### code を持つ package

`code/CMakeLists.txt` は `pelican_package_sources(...)` を呼びます。ホスト project 側の `code/CMakeLists.txt` は `pelican_packages()` を 1 行呼ぶだけです(`pelican_cli project init` の雛形はこの 2 行を最初から書きます)。この 3 つの関数は `cmake/` ではなく **[src/player/CMakeLists.txt](../../src/player/CMakeLists.txt#L107)** にあります(`pelican_game_sources` :106 / [`pelican_package_sources`](../../src/player/CMakeLists.txt#L205) :203 / [`pelican_packages`](../../src/player/CMakeLists.txt#L262) :260)。

```cmake
# <project>/code/CMakeLists.txt
pelican_game_sources(game.cpp)
pelican_packages()
```

```cmake
# <project>/packages/acme.glowkit/code/CMakeLists.txt
pelican_package_sources(system.cpp)
```

- `pelican_packages()` は `<project>/packages/*/code/CMakeLists.txt` を探して `add_subdirectory(<絶対パス> <binary dir>)` します。project の `code/CMakeLists.txt` は `src/player` から `include()` されるので、相対パスでは本体ツリー側を探してしまいます。
- **`pelican_game_sources(...)` より後に呼びます。** package のオブジェクトは project の game logic DLL に合流します(package ごとの DLL は作りません)。先に呼ぶと `pelican_packages() has to come after pelican_game_sources(...)` で止まります。
- `pelican_package_sources()` が作るのは OBJECT ライブラリで、その engine 側 include パスは**公開 SDK だけ**です。`src/core` は載りません。

> **判定であって約束ではない:** project 自身の `code/` には `src/core`(内部ヘッダ全部)が無条件に載ります。package だけは載りません。だから package のソースに `#include "vkcore/renderer.hpp"` と書くと **コンパイルエラー**になります。「公開 SDK だけで build できる」はこの 1 点で判定されています([run_package_template_smoke.cmake](../../test/run_package_template_smoke.cmake) の対照 1)。
>
> 公開 SDK の中身は `PELICAN_AUTO_STAGE_SDK` が配布するものと同じ集合です —— `src/core/userpublic/**`、`src/core/ecs/**`、`handle.hpp`、`phys/physquery.hpp`。実体は build ツリーに生成される転送ヘッダ 1 枚ずつで、本物のヘッダはその場に残ります。

`requires.sdk_abi` は install 時の早期拒否であって、本物の gate ではありません。本物の gate は DLL ロード時に loader が解決する epoch シンボル(`pelican_game_logic_epoch_v2`)です。

### uninstall

RPC `uninstall_package{namespace}` は `packages/<ns>/` を消し、`project.json.packages` から entry を落とします。rendering config(root 文書)かその preset のどこかに `project://packages/<ns>/` で始まる文字列が残っていれば、**何も消さずに** `package_in_use` で拒否し、残っている参照を列挙します。

### 記録(v0 で閉じていないこと)

- `project.json` の `packages` を読むのは package 経路だけです。`ProjectEnvelope` は lossy なので、将来 envelope から `project.json` を書き出す writer を作ると `packages` が消えます。
- catalog に載るのは **engine 同梱 + install 済み package** だけです。project 内の素の `project://features/*.json`(出荷 `raytracing_demo/features/spheres.json` を含む)は今日も載りません。
- pass 名・render target 名には namespace が付きません。同じ pass 名を持つ package が 2 つ入ると合成で衝突します。
- `sha256` は `package list` が再計算して照合します。エンジン起動時には照合しません。
- `provides.components` は複製されるだけで、engine 側の消費者はまだありません。この `components` は **`pelican.render_component`**(§3.12)のディレクトリを指す語で、**ECS のコンポーネント([第4章](04_scene_ecs.md))とも、`project.json` の他の語とも無関係**です。今日それを読むのは `import_render_feature` だけで、package 経由では読まれません。
- **install が途中で死ぬと `packages/<namespace>.tmp-<pid>` が残ります**。install 自体は失敗時に自分の staging を消しますが、プロセスごと消えた場合の残骸を**誰も掃除しません**(export 側の `user://components/*.tmp-*` は次の export が掃除します — §3.12。install 側にその機構はありません)。残骸は `pelican_packages()` が飛ばすのでビルドは壊れませんが、手で消す必要があります —— **glob(`packages/*/package.json`)自体は残骸も拾い**、拾ってから `if(package_name MATCHES "\\.tmp-") continue()` で落とす形です([src/player/CMakeLists.txt:277](../../src/player/CMakeLists.txt#L279))。
- `uninstall_package` が「まだ使われているか」を見るのは **rendering config の root 文書と、その preset だけ**です([renderconfigeditor.cpp:1119](../../src/core/communication/renderconfigeditor.cpp#L1656))。他の feature 文書の中から `project://packages/<ns>/` を参照していても検出されません。
- shader を持ち込む package は未対応です(feature が `engine://` shader だけを参照できる範囲)。

## 3.12 共有部品(render component)— feature 1 本を持ち出して別 project へ入れる(✅P-2a-3)

> **設計決定:** 部品を共有するとは「バイトを copy すること」ではない。feature の文書だけを渡しても、**それが宿主のプロジェクトに何を要求するか**(自分で宣言していないレンダーターゲット)が分からないので、**feature 文書 + 要求の契約**を 1 つのディレクトリに揃えて初めて持ち出せる形になる。持ち出し先は project の外(`user://`)で、取り込み先は project の中。

§3.11 の package が「部品一式のディストリビューション」なのに対し、**render component は feature 1 本の持ち出し**です。使う場面は「自分の project で materialize して手を入れた feature を、別の project でも使いたい」。正本は [rendercomponent.hpp](../../src/project/rendercomponent.hpp#L16)(`pelican.render_component` v1)です。

### 書き出す(export)

RPC [`export_render_feature{scope, index}`](../../src/core/communication/renderconfigeditor.cpp#L1430)(`scope` は `"root"` か `"preset"`、`index` は `features[]` の何番目か)が 1 本を書き出します。書き先は**固定で `user://components/<name>/`**([renderconfigeditor.cpp:944](../../src/core/communication/renderconfigeditor.cpp#L1481))、`<name>` は feature 文書の `name` です。プロジェクトには 1 バイトも書きません(設計決定 7 — [第1章 §1.5](01_overview.md))。

ディレクトリの中身は 2 ファイルです(以下は例):

```
user://components/glow_bloom/
├── feature.json     # 元の feature 文書のバイトそのまま
└── component.json   # 封筒 + 契約
```

```json
{
  "schema": "pelican.render_component",
  "version": 1,
  "name": "glow_bloom",
  "feature": "feature.json",
  "sha256": "…",
  "source": { "project": "mygame", "reference": "project://features/glow_bloom.json" },
  "requires": { "render_targets": ["lit_color", "swapchain"] }
}
```

| キー | 意味 |
|---|---|
| `name` | feature 文書の `name`。ディレクトリ名 1 つとして使える文字であること(`/` `\` `:` `.` を含むと `render_component_invalid`) |
| `feature` | 隣のファイル名(常に `feature.json`) |
| `sha256` | `feature.json` のダイジェスト。読み戻すときに照合します |
| `source` | どのプロジェクトのどの参照から出たか(**記録**であって、取り込み側の解決には使いません) |
| `requires.render_targets` | **この feature が読み書きするのに自分では宣言していないレンダーターゲット**。宿主に対する要求です([renderComponentRequiredTargets()](../../src/project/rendercomponent.hpp#L46)) |

`requires.render_targets` の出し方は「使っている名前 − 自分が宣言している名前」です。feature 自身の `render_targets` と `buffers` の宣言は差し引かれます(P-2a-2 の `bloom.intensity` のように feature が持つバッファは宿主の義務ではないため)。**`swapchain` は列挙されますが、取り込み側は常に存在するものとして扱います**(エンジンの出力先なので、不足として報告されることはありません)。

**書き出しは公開の瞬間まで見えません。** `<name>.tmp-<pid>` に 2 ファイルを書き、**ディレクトリごと rename** して初めて本来の名前になります([writeRenderComponent()](../../src/project/rendercomponent.hpp#L59))。途中で死ねば `.tmp-<pid>` が残るだけで、半分書けた component は存在しません。残骸は**次の export が掃除します**(同じ親ディレクトリの `*.tmp-<数字>` のうち、自分の pid のものは実行中かもしれないので残す — [removeStaleStagingDirectories()](../../src/project/rendercomponent.cpp#L134)。✅R-1)。

### 取り込む(import)

RPC [`import_render_feature{base_source_digest, source_dir, destination_reference}`](../../src/core/communication/renderconfigeditor.cpp#L1499) が、component ディレクトリを読んで **`destination_reference`(`project://…` の新規ファイル)に feature 文書を作り、`rendering.features` に足す**ところまでを **1 つのトランザクション**で行います(`base_source_digest` は CAS。[第13章](13_editor.md))。`source_dir` は**絶対パス**でなければならず、`destination_reference` は `project://` で始まる**新規**ファイル(`#` フラグメント不可)です。**この 3 つは別々の code で断られます** —— `project://` でない / `#` を含むは `invalid_feature_destination`([renderconfigeditor.cpp:974](../../src/core/communication/renderconfigeditor.cpp#L1511))、`source_dir` が絶対でないのは `render_component_invalid`([:980](../../src/core/communication/renderconfigeditor.cpp#L1517))、宛先が既にあるのは `feature_destination_exists`([:1026](../../src/core/communication/renderconfigeditor.cpp#L1563))です。

**複製であって参照ではありません** —— 取り込んだ後は component ディレクトリを消してもプロジェクトは描けます。

契約の検査は取り込みの前に走ります。`component.json` の `requires.render_targets` のうち、**取り込み先の rendering config(`rendering` 節)とその preset が宣言していない**ものがあれば、名前を列挙して拒否します。他の feature が作る RT は数えません(保守的側に倒しています)。

### 名前付きの拒否(10 種)

`render_component_*` が 8 種、取り込み先(destination)の 2 種が別系統です。

| コード | いつ |
|---|---|
| `render_component_engine_reference` | `engine://` の feature を export しようとした(共有済み。まず project 側の派生を作る) |
| `render_component_project_reference` | feature 文書の**どこかの文字列**が `project://` を指したまま(全文字列を走査します — [rendercomponent.cpp:237](../../src/project/rendercomponent.cpp#L237))。持ち出した先で解決できない参照を含む部品は作らせません |
| `render_component_invalid` | 封筒・`name`・JSON・`requires.render_targets` の形が不正 |
| `render_component_destination_exists` | `user://components/<name>/` が既にある(上書きしません) |
| `render_component_digest_mismatch` | `feature.json` が `sha256` と一致しない |
| `render_component_source_inside_project` | `source_dir` が取り込み先プロジェクトの中(それは `project://` で参照するものであって import ではありません) |
| `render_component_requires_missing` | 宿主が宣言していない RT がある(`render_targets` に不足を列挙 — [renderconfigeditor.cpp:1019](../../src/core/communication/renderconfigeditor.cpp#L1556)) |
| `render_component_write_failed` | staging ディレクトリの作成・2 ファイルの書き込み・公開 rename のどれかが失敗した([rendercomponent.cpp:37](../../src/project/rendercomponent.cpp#L37)、[:282](../../src/project/rendercomponent.cpp#L282))。受付では export / import の両方が `RenderComponentError` をそのままの code で返します([renderconfigeditor.cpp:955](../../src/core/communication/renderconfigeditor.cpp#L1492)、[:1050](../../src/core/communication/renderconfigeditor.cpp#L1492)) |
| `invalid_feature_destination` | `destination_reference` が `project://` で始まらない、または `#` を含む(取り込み先の 2 種の 1 つ) |
| `feature_destination_exists` | `destination_reference` のファイルが既にある(同上) |

### 記録(v0 で閉じていないこと)

- `requires.render_targets` は pass の入出力と `render_target_overrides` から作ります。**compute / ray の image binding は見ていません**。
- preset を併用する `pelican.graph` root では、`rendering.rendering_passes[i].region_replacements` が合成に届かない既存の挙動があります([第11章 §11.3](11_status.md))。
- package の `provides.components`(§3.11)はこの形式のディレクトリを指す想定ですが、**複製されるだけで engine 側の消費者はまだありません**。今日 component を読むのは `import_render_feature` だけです。
- **package が持ち込んだ feature は export できません。**vendoring が feature の `name` を `<namespace>.<元の名前>` に書き換えるのに([qualifiedPackageFeatureName()](../../src/project/packagemanifest.cpp#L320))、export 側は「1 つのディレクトリ名として使える名前」しか受けず **`.` を禁じている**ためです([rendercomponent.cpp:231](../../src/project/rendercomponent.cpp#L231))。持ち出したければ、まず `.` を含まない名前で派生を作ります。

## 3.13 前の版の project を移す(✅MG-1)

> **設計決定(2026-09-24 利用者裁定):** 形式は v0 の後も変わってよい。v0 は「前の版の project を後の版のソフトへ移す仕組み」がある最初の版。移す単位は engine の版ではなく**各文書が持つ `(schema, version)`** で、手で編集した文書や git の merge で来た文書でも手順が二重に掛かったり飛んだりしない。([設計 §1](../design_v0_production_gaps.md)、約束の文面は [v0 形式の一覧と移行の約束](../v0_freeze.md) §1)

実装は [projectmigration.hpp](../../src/project/projectmigration.hpp)(`pelican_project`)。**v0 の engine が登録している手順は 0 本**なので、今日この節の仕組みが project に対して何かをすることはありません(下の「手順が 0 本の今」)。次の版で文書の `version` を上げる WP が、同じ WP で手順を 1 本登録します(台帳の規約)。

### 流れ

1. **手順の登録**: 1 本の手順は [`DocumentMigrationStep`](../../src/project/projectmigration.hpp#L94)`{id, schema, from_version, to_version, description, plan}` で、「`schema` の `from_version` 版の文書を `to_version` 版にする」関数 `plan(文書, ProjectContext) -> StepResult{document, manual_items}` を持ちます。`manual_items` は自動で直せなかった箇所の一覧 `{code, pointer(RFC 6901), message}` です。`plan` が返した文書の `version` は登録表が `to_version` に書き換えます。登録表 [`DocumentMigrationRegistry`](../../src/project/projectmigration.hpp#L103) は受け取った手順を丸ごと受けるか、名前付きで拒否するかのどちらかです([`registerStep()`](../../src/project/projectmigration.cpp#L466)):

   | 名前 | 拒否する手順 |
   |---|---|
   | `project.migration_step_invalid@1` | `id` か `schema` が空、版が 1 未満、`plan` が無い |
   | `project.migration_step_duplicate@1` | 同じ `id`、または同じ `(schema, from_version)` がもう登録されている |
   | `project.migration_step_cycle@1` | `to_version <= from_version` |
   | `project.migration_step_gap@1` | `to_version > from_version + 1`(1 版ずつしか上げられない) |

2. **検出(開く前の入口)**: `pelican_player` は project.json を持つ project を開くとき、**parser が文書を読む前に 1 か所だけ**入口を通ります([`requireProjectMigrationNotRequired()`](../../src/project/projectmigration.cpp#L563)、呼ぶのは [main.cpp](../../src/player/main.cpp#L728) の `parseProjectEnvelopeText` の手前)。Studio が起動する player(`--rpc`)も同じ入口を通ります。入口はまず、落ちたプロセスが残した transaction を復旧し(§3.1 の `.pelican/render-authoring.transaction/`)、次に project root の下の `*.json` を全部歩いて、**最上位の `schema`(文字列)と `version`(整数)だけ**を SAX で生のまま読みます。`.` で始まるディレクトリ(`.pelican` / `.git` など)と symlink のディレクトリには入りません。オブジェクトでない JSON や、`schema` / `version` を持たない JSON は文書として数えません。登録された手順の `from` に当たる文書が 1 本でもあれば、parser に渡さず次の名前で止めます:

   ```
   project.migration_required@1: {"documents":[{"path":"scenes/main.scene.json","schema":"pelican.scene","from":2,"to":3}]}
   ```

   (`to` は登録された手順を辿った終点の版。上の例の版は説明用で、v0 にこの手順はありません。)`--rpc` でない player は、この行に続けて `` run `pelican_cli project migrate --project <root>` to move these documents to this engine's versions `` を stderr に出して exit 1 します。`--rpc` の player は engine を起動せず、移行の 2 method だけに答える endpoint になります([第10章 §10.3](10_tools.md))。parser の version 検査より前に走るので、project.json 自身の版が上がった場合も同じ名前で止まります。

3. **plan(dry-run)**: [`planProjectMigration()`](../../src/project/projectmigration.cpp#L614) は何も書きません。対象の文書ごとに手順を順に当て、書き換え後の bytes、通った手順の `id`、`manual_items`、元の bytes の sha256(`digest`)を作ります。project.json は、手順の対象でなくても `saved_by_engine` を今のエンジンの版に書き換えた形を用意します(そのキーがあれば値だけを置き換え、無ければ `version` の直後に足します。ほかの bytes は変えません)。手順が投げた、オブジェクトでない値を返した、`schema` を変えた、のどれかは `project.migration_step_failed@1 {path, step, message}` です。

4. **apply**: [`applyProjectMigration()`](../../src/project/projectmigration.cpp#L707) は 排他 → plan → CAS → 退避 → 置換 の順です。
   - **排他(✅MG-1b)**: 最初に transaction のディレクトリ(§3.1 の `.pelican/render-authoring.transaction/`)を**自分で作って**取ります([`ProjectDocumentTransactionLock`](../../src/project/projectdocumenttransaction.hpp#L157)。`create_directory` で取る方式で、エディタの commit と同じ)。取った状態で plan を読み、置換の commit までそのまま持ちます。ディレクトリが既にあれば取れず、**中を読まず、復旧もせず、待たずに**次の名前で止まります:

     ```
     project.migration_transaction_busy@1: {"path":"<project>/.pelican/render-authoring.transaction","message":"… Close every engine and Studio that has the project open and apply again; opening the project resolves a directory that was left behind"}
     ```

     ディレクトリには持ち主が書かれていないので、「同じ project を開いている engine や Studio が今 commit している」のと「落ちたプロセスの残骸」は区別できず、どちらも同じ名前で止まります。**その project を開いている engine と Studio をすべて閉じてから apply し直してください。** 残骸なら、project を一度開けば(開く入口の復旧で)片付きます。MG-2a の旧形式のディレクトリ(§3.1)も、apply は中を読まないので同じ名前で止まります。MG-1 の apply は最初に transaction の復旧を走らせていたので、commit 途中の別プロセスの編集を巻き戻しうりましたが、MG-1b(`09f9151d`)で復旧をやめてこの排他に替えました。書くものが無い apply は、作ったディレクトリを消して終わります。
   - **CAS**: 呼び手が plan で受け取った `expected`(書き換える全文書 + project.json の `{path: sha256}`)を渡すと、今の plan の組と完全に一致しなければ何も書かずに `project.migration_plan_stale@1 {expected, actual}` で止まります(RPC は必ず渡します。CLI は plan と apply を 1 回で行うので渡しません)。
   - **退避**: 書き換える文書(project.json を含む)の今の bytes を `.pelican/migration/<UTC 時刻 YYYYMMDDTHHMMSSZ>/` の下に同じ相対パスで複製し、plan の記録を `migration.json`(`schema: "pelican.project_migration_backup"`, `version: 1`)として置きます。同じ名前が既にあれば `-2`, `-3` … を付け、上書きはしません。
   - **置換**: 対象の文書と project.json(`saved_by_engine` の更新)を**1 つの transaction** で置き換えます。transaction は §3.1 の rendering config の編集と同じもの(同じディレクトリ・同じ形式)で、途中で失敗すれば全文書が元の bytes に戻り、退避先も消えて `project.migration_apply_failed@1 {message}` になります。退避の書き込みに失敗したときも同じ名前です。手順で書き換えた文書は 2 space の字下げで書き直されます(改行コードは元の文書に合わせます)。
   - 移すものが無ければ何も書きません(`saved_by_engine` も書き換えません)。

5. 移した後は project を新しいプロセスで開き直します。動いていたプロセスが前に読んだ内容は古いままです(RPC の応答の `restart_required`)。

そのほかの名前: project root がディレクトリでない、または project.json が読めない / オブジェクトでないときは `project.migration_project_missing@1 {project_root, message}`。`--rpc` の移行専用 endpoint で apply を済ませた後に別の method を呼ぶと `project.migration_restart_required@1`。

### 手順が 0 本の今

production の登録表([`makeBuiltinDocumentMigrationRegistry()`](../../src/project/projectmigration.cpp#L532))は空です。空のときの検出は**ディレクトリを歩かずに**すぐ「無し」を返すので、開くときの入口で起きるのは transaction の復旧だけです。`pelican_cli project migrate` は `required: false`、`documents: []` を出して何も書きません(`saved_by_engine` も書きません)。`devcli_project_migrate` が、`project init` の直後の project と v0 の corpus 7 本のそれぞれで、dry-run と apply のどちらでもファイルが 1 バイトも変わらないことを確かめています。手順の働きは、test binary にだけ登録する手順(`pelican.scene` の最上位の key を 1 つ改名する from 2 → 3)で確かめています。

**回帰素材**: v0 の出荷 project 7 本の JSON・shader・code(計 105 ファイル。gitignored な大容量資産は含めない)を `test/fixtures/project_corpus/v0/` に固定しています。後の版の手順はこの corpus を移して開けることを test で保ちます。

### 残した穴(MG-1 の時点)

- Studio の offline open と移行の画面は、MG-1 の GUI 側(`52547bee`)で入りました。旧版の project を Studio で開くと、plan を見せてから apply します([第 13 章「Studio で旧版の project を開く」](13_editor.md#studio-で旧版の-project-を開くmg-1))。
- `pelican_cli` のほかのサブコマンド(project の文書を直接読み書きするもの)には入口がありません。
- 退避先 `.pelican/migration/<時刻>/` を消す手段はありません(手で消します)。
- 検出が歩くのは project root の下だけで、**root の外に mount した asset store の中の文書は見ません**。
- ~~apply が最初に transaction の復旧を走らせ、別のプロセスの commit 途中の編集を巻き戻しうる~~ → **解消(MG-1b、`09f9151d`)**。apply は復旧せず、transaction のディレクトリを取れなければ `project.migration_transaction_busy@1` で止まります(上の 4.)。
- 開く入口の復旧は、排他を取らずにディレクトリを消します(MG-1 より前からの性質で、MG-1b でも変えていません)。

### camera の向きの規約の変換(✅CC-1、`c8367b87`)

CC-1 で camera の向きの規約が変わりました(回転なしの camera は +Z ではなく −Z を向き、up は `basic_config.camera.up` ではなく回転の +Y。[第6章 §6.1](06_rendering.md) の「座標の規約」)。文書の `version` は上げておらず、上の移行の枠の手順でもありません(v0 の前なので。[設計 §4](../design_coordinate_convention.md))。代わりに `up` を名前で断り(§3.2 の `project.camera_up_removed@1`)、変換は repo の script で行います:

```sh
uv run tools/convert_camera_convention.py [--dry-run] [--assume-up X,Y,Z] <project dir>...
```

[`tools/convert_camera_convention.py`](../../tools/convert_camera_convention.py#L518) は project の下(`.` で始まるディレクトリを除く)の `*.json` / `*.jsonl` を読み、変える値だけをその場で書き換えます(ほかの bytes は変えません)。

| 文書 | 書き換え |
|---|---|
| `project.json` | `basic_config.camera.up` を消す(`camera` がそれだけなら `camera` ごと) |
| `pelican.scene` | `camera` を持つ object の回転を、前と同じ方向(前の前方 = R·(0,0,+1))を向き up が world の +Y になる回転にする。前方がほぼ鉛直(\|前方·Y\| > 0.999)なら、自分の Y 軸まわりに 180° 回すだけ。前の回転にロールがあれば捨て、`camera_roll_dropped` に載せる。`transform` の無い camera には Y 軸まわり 180° の `transform` を足す。orbit の controller の `yaw` / `pitch`(`*_degrees` も)を同じ方向を指す値に書き換え、controller の `up` を消す |
| `pelican.prefab` | controller だけを書き換える。prefab は transform を持たないので、camera を持つ prefab は `camera_prefab_instances` に載せて手で直す |
| `pelican.transform_seq`(`.jsonl`) | camera bake の出力(`generator: pelican2-camera-bake`)だけ、各 `rot` に Y 軸まわり 180° を右から掛ける。ほかの生成元のものは `transform_seq_not_camera_bake` に載せて触らない |

- 標準出力は JSON の報告 `{projects: [{project, dry_run, old_up, changed, camera_roll_dropped, camera_prefab_instances, transform_seq_not_camera_bake, manual}]}` です。`--dry-run` は何も書きません。
- `basic_config.camera.up` が無い project は断ります(変換済みか、もともと書いていない)。それでも変換するときは `--assume-up X,Y,Z` で前の up を与えます(前の engine の既定は `0,-1,0`)。断った project には何も書かず、exit 1 です。
- 絵の変わり方(設計 §4 の規則): 前の `up` が −Y の project(前の `blank` の雛形、example)は、変換後に前の絵の左右反転になります。+Y の project は、前は上下逆さまに描かれていたので、変換後は正立します。
- 出荷の 7 project と `test/fixtures/project_corpus/v0/` の corpus、camera を持つ test の fixture は CC-1 で変換済みです。world の X を直接動かす game code の左右(example の `playercontrol.cpp`)と、鏡像を打ち消すために置いていた補正(vrm_xr_demo の主役の Z 軸 180°)は手で直しました。sprite_demo の game code は直す必要がありませんでした。
- test: `convert_camera_convention_unit`([test_convert_camera_convention.py](../../test/ci/test_convert_camera_convention.py))。

## 関連文書

- [../design_project_format.md](../design_project_format.md) — [PF] v6.3(凍結)。形式・PathResolver の正
- [../design_editor_tooling.md](../design_editor_tooling.md) — AuthoringSceneDocument・保存・エディタ経路の設計(§3.9)
- [../design_project_format_web_profile.md](../design_project_format_web_profile.md) — [PFW] v1.3。サブセット原則
- [../design_project_interpretation_layer.md](../design_project_interpretation_layer.md) — 解釈レイヤの設計
- [../design_project_vcs.md](../design_project_vcs.md) — asset store・manifest・雛形
- [../design_persistence.md](../design_persistence.md) — user:// と設定/セーブ三区分
- [第4章 シーンと ECS](04_scene_ecs.md) / [第5章 アセット](05_assets.md) / [第9章 Web プロファイル](09_web.md) / [第13章 エディタ](13_editor.md)
