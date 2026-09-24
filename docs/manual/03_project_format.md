# 第3章 プロジェクト形式

対象: pelican2(HEAD=`82054c05`・2026-09-17。2026-07-21 に全域再確認、2026-09-04 に §3.7 の「エンジン先行」段落と §3.10(宣言スキーマ v3)を追記、2026-09-17 に §3.2 の `requires` と §3.12(共有部品)を追記、2026-09-24 に §3.2 の `requires` の語彙と §3.11 の `requires.capabilities` を PG-1(`533d8d6c`)に合わせて更新)/ このマニュアルはコードを正とする

## この章で学ぶこと

- 「プロジェクト」という単位の定義と、標準ディレクトリ構成
- `project.json` の正確なスキーマ(必須/任意/既定値)と検証規則
- パス参照の分類(素の相対 / `project://` / `engine://` / `user://` / 絶対 / `#` フラグメント)と PathResolver の解決規則
- asset store(巨大なバイナリを git リポジトリの外へ置くための「置き場所の間接化」。[用語集](14_glossary.md))と `.pelican/local.json`
- シーン JSON の in-memory 表現(`AuthoringSceneDocument`)と保存の契約(エディタ経路)
- ユーザー定義コンポーネントの宣言スキーマ v3(`Flags` / `List` の行封筒 `$list` / `$row`)と `$` 予約
- なぜこの設計なのか — fail-fast、cwd 全廃、絶対パス拒否などの決定事項

正本となる設計文書は [../design_project_format.md](../design_project_format.md)([PF] v6.3・**凍結済み**)です。`[PF]` はこの章を通じてこの設計文書を指す略号で、**凍結済み**は文書側の見出しどおり「設計としてこれ以上改訂しない」の意味です。したがって形式そのものは固まっており、文書と実装が食い違ったときは実装が正になります(§3.8)。この章はその実装(`src/core/loader/` と `src/project/`)を基準に書いています。

## 3.1 プロジェクトとは

> **設計決定:** プロジェクト = ルートに `project.json` があるひとつのディレクトリ。これが「開く・実行する・配布する」の単位(Unity の Project、Godot の project.godot に相当)。エンジン実行時、プロジェクトディレクトリは**読み取り専用**であり、エンジンは何も書き込まない。書き込み先は `user://`(OS のユーザーディレクトリ)に分離される。これが複数インスタンス並行起動の安全性の根拠になっている。

標準ディレクトリ構成(`pelican_cli project init` が生成する形。[第2章](02_getting_started.md) 参照):

| パス | 内容 | 参照元 |
|---|---|---|
| `project.json` | マニフェスト+基本設定 | 起動時に必ず読む |
| `scenes/*.scene.json` | シーン定義(pelican.scene v1) | `basic_config.scene_data_json` |
| `assets/asset_data.json` | モデル登録表 | `basic_config.asset_data_json` |
| `assets/models/`, `assets/textures/`, `assets/audio/` | バイナリアセット置き場(規約) | asset_data.json 等から相対参照 |
| `passes/*.json` | レンダリングパイプライン定義(rendering config。root 形は 2 つ、下記) | `basic_config.rendering_config_json` |
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
| `.pelican/shader_cache/` | シェーダのコンパイルキャッシュ(✅WP82 — [第6章](06_rendering.md)) |
| `.pelican/assets-hash-cache.json` | assets manifest の差分ハッシュキャッシュ(§3.4) |
| `.pelican/logs/import-tools.log` | `pelican_cli import --rules` が起動した外部ツールのログ(§5.5) |
| `.pelican/render-authoring.transaction/` | rendering config の編集(Studio / RPC)が root 文書と feature 断片をまとめて書き換える間だけ在る作業ディレクトリ(`manifest.json`・差し替え前の控え・commit 済みの印 `committed`)。正常に終われば消えます。プロセスが途中で落ちて残っていれば、次の起動で commit 済みなら文書を確かめてから消し、commit 前なら文書を元に戻してから消します |

**旧形式の transaction ディレクトリ(`render_config.legacy_transaction_dir@1`)** ✅MG-2a

v0 より前のエンジンは、commit の決定を別ファイル `committed` ではなく `manifest.json` の `"status": "committed"` に書いていました。この形で残ったディレクトリは**もう読みません**。起動時の復旧と rendering config 編集の commit のどちらも `render_config.legacy_transaction_dir@1` で止まり、ディレクトリには手を付けません([判定](../../src/core/communication/renderconfigtransaction.cpp#L488))。

- 起動時: ログに `Pelican fatal error : render_config.legacy_transaction_dir@1: {"message":"…","path":"…"}` が出て、起動はそこで終わります。
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
    "camera": { "yfov": 0.7853981633974483, "znear": 0.1, "zfar": 1000, "up": [0.0, -1.0, 0.0] },
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
| `engine_min_version` | 任意 | `"X.Y.Z"`。エンジン現行版(`0.1.0`)より新しければ起動拒否。`--ignore-engine-version` で WARN に降格可 |
| `requires` | 任意 | **このプロジェクトが動くために必要なビルド機能ユニットの名前の配列**(下記「機能ユニットの要求」)。語彙は `animation` / `audio` / `physics` / `xr` / `vat` / `seqplayer` の 6 語(✅PG-1)。未知の名前は `project.requires_unknown@1`、その機能を OFF にしたビルドで開くと `<語>.module_unavailable@1`(例: `animation.module_unavailable@1`) |
| `basic_config` | 任意 | 省略したキーは既定値で埋まる(次節) |
| `asset_stores` | 任意 | asset store 宣言(§3.4) |
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
| 語彙 | **ビルド機能ユニットのレジストリに載っている名前**です。各語が指すオプションは上の表のとおりです([第2章 §2.2](02_getting_started.md))。今日の表は 6 行([projectformat.cpp:80](../../src/project/projectformat.cpp#L80) が `.def` を展開)で、`knownProjectRequirements()` がその一覧を返します |
| 検証の時 | **parse 時**。[parseProjectRequirements()](../../src/project/projectformat.cpp#L118)が配列であること・非空文字列であること・表にある名前であることを見ます |
| 未知の名前 | `project.requires_unknown@1: project.json requires 'network', which this engine does not know; known requirements: animation, audio, physics, xr, vat, seqplayer` —— **既知の一覧を添えて**拒否します(一覧の順は `.def` の行の順) |
| 満たせないビルド | 名前は新設せず、その語の `<語>.module_unavailable@1` を出します([projectformat.cpp:181](../../src/project/projectformat.cpp#L189))。例: `physics.module_unavailable@1: this build has PELICAN_WITH_PHYSICS=OFF, so project.json's requires: ["physics"] cannot be satisfied`。全要素が既知語かを先に確かめてから、最初に満たせない語を報告します。`pelican_player` はこれを捕まえて 1 行表示して exit 1 します |
| コーパス側 | **プロジェクト群を舐めるテスト・スモークも同じ値を読みます**。満たせない要求を持つプロジェクトは失敗ではなく**名前付きの skip**(`skip posegraph_demo: requires animation (PELICAN_WITH_ANIMATION=OFF)`)になります。`PELICAN_WITH_ANIMATION=OFF` build では `animation` を宣言した 3 つ分の行と、必要なら `skip editor lane: …`、最後に件数の要約が出ます([第2章 §2.2](02_getting_started.md))。`PELICAN_WITH_PHYSICS=OFF` build では `sprite_demo` が同じ形で飛ばされます |
| 宣言しなくても止まるもの | `requires` はプロジェクトを開く時点の前倒しです。宣言が無くても、機能を外したビルドでその機能のコンポーネントを持つオブジェクトは、シーンロードの prepare と、エディタの `add_component` / `spawn` で同じ `<語>.module_unavailable@1` になります(今日対応付けがあるのは `collider` → `physics`、`animation` → `animation`。[第4章 §4.3](04_scene_ecs.md)) |
| 出荷での宣言 | `animation`: `animgraph_demo` / `vrm_xr_demo` / `posegraph_demo`。`physics`: `sprite_demo`(PG-1)。`audio` / `xr` / `vat` / `seqplayer` を宣言した出荷プロジェクトはまだありません |

> **3 つの `requires` は別物です(混同注意):**
> 1. **project.json の `requires`**(この節)= **ビルド機能ユニット**の名前の配列。
> 2. **package manifest の `requires`**(§3.11)= `{engine_min_version, sdk_abi, capabilities}` という object。エンジン版と SDK エポックの要求です。ただし中の `capabilities` だけは、project.json の `requires` と**同じ 6 語の語彙**で install 時に検査されます(✅PG-1)。
> 3. **feature 文書の `integrations[].requires`**([第6章](06_rendering.md))= その統合を適用する条件としての**他の feature の名前**。
>
> 綴りが同じだけで、形も検証者も別です(語彙を共有するのは 1 と、2 の中の `capabilities` だけ)。

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
| `rendering_config_json` | 参照 | `"passes/main_rendering_config.json"` | レンダリング構成への参照(下記「rendering config の 2 つの root」) |
| `default_rendering_pass` | string | `"main_render"` | rendering config 内で使う `rendering_passes[].name`(graph root では `rendering.rendering_passes[].name`) |
| `ui_config_json` | 参照 | `"ui/ui_overlay.json"` | UI ドキュメント(pelican.ui)への参照 |
| `input_actions_json` | 参照 | **既定なし(任意)** | 入力アクション定義。未指定ならアクション層は未構成 |
| `input_profiles` | object | — | バインディングプロファイルの辞書(名前 → ファイル参照。✅WP91 — [第7章](07_input_ui.md)) |
| `input_profile` | string | — | 既定プロファイル名 |
| `sprite` | object | `{"pixels_per_unit": 100.0}` | 2D スプライトの寸法変換係数(✅WP106 — [第6章](06_rendering.md) §6.9) |
| `camera.sprite` | object | `{"pixel_perfect":"off","sort":"z"}` | スプライトの pixel perfect / ソートポリシー(camera 内。シーン側 camera コンポーネントにも書ける) |

`camera` は glTF のカメラ定義と同形です。`type` は `"perspective"`(既定)か `"orthographic"` のみ。perspective は `yfov`(**ラジアン**)/`znear`/`zfar` 必須・`aspect` 任意、orthographic は `xmag`/`ymag`/`znear`/`zfar` 必須です。

> **設計決定(fail-fast):** 旧キー名 `fov_y` / `near` / `far` は「黙って無視」ではなく `'fov_y' is not supported in v1; use 'yfov' (radians)` という**移行先を案内する名指しエラー**で拒否される(WP63)。「読めるふりをしない」— 警告止まりだと『動いたように見えて新機能の挙動だけ壊れる』事故になるため。互換受理の追加はプロジェクト規則で禁止されており、旧形式の変換は外部ツールの仕事。

### rendering config の 2 つの root(✅P-2a-4a/4b)

`rendering_config_json` が指すファイルは、**2 つの root 形**のどちらかです。どちらもエンジンが受理し、v0 の間はどちらも消えません。

- **legacy root**: `features` / `render_targets` / `rendering_passes` などがファイルの最上位に並ぶ形。出荷 `animgraph_demo` / `sprite_demo` / `vrm_xr_demo` / `raytracing_demo` の 4 つがこの形です。
- **`pelican.graph` root**: 最上位は共通グラフの 5 キー(`schema` / `version` / `definitions` / `nodes` / `outputs`)**だけ**で、rendering config 全体が `rendering` の下に入ります。出荷 **`example`(`passes/main_rendering_config.json`)と `posegraph_demo`(`passes/main.json`)**、および `pelican_cli project init` が生成する新規プロジェクトがこの形です。共通グラフのノード(`graph.instance` を含む)を書けるのは**この root の下だけ**なので、姿勢グラフを装着する `posegraph_demo` はこの形である必要があります([第4章 §4.1](04_scene_ecs.md))。

```json
{ "schema": "pelican.graph", "version": 1,
  "definitions": {}, "nodes": [], "outputs": {},
  "rendering": { "features": [...], "render_targets": [...], "rendering_passes": [...] } }
```

規則は 1 つです —— 上の 5 キーは共通グラフが所有し、**それ以外の rendering config のキーは全部 `rendering` 節に属します**。`rendering` 節の中身が同じなら描画結果は同じで、違いは「共通グラフのノードを同じファイルに持てるかどうか」だけです(legacy root は持てません)。キーごとの所有と移行手順は [第6章 §6.16](06_rendering.md) が正です。

`pelican.graph` root は web ビューア未対応です([第9章](09_web.md) §9.3)。

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

> **設計決定(検証はロードを止めない):** アセットの正当性検査は開発の補助であり、既定では警告に留める。厳格化は明示フラグ(開発 CI・配布検証)でのみ行う。

example の README にあった手書き sha256 表の役目は、この manifest([../../projects/example/assets.manifest.json](../../projects/example/assets.manifest.json)・26 エントリ)に置き換わりました。

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

「エンジン先行」の現在地(2026-09-04 にコードを読んで確認):**§3.10 の `Flags` / `List` 行封筒も、レイトレの `pelican_ray_instance`([第6章](06_rendering.md) §6.15)も、web 側パーサはまだ知りません。** web の scene パーサが closed schema を持つのは組み込み 7 コンポーネント(`transform` / `light` / `camera` / `simplemodelview` / `collider` / `animation` / `sprite_view`)だけで、codec を持たない名前のコンポーネントは検証せず `opaque` として素通しします(`my_webpage/packages/pelican-webgpu/src/pelican2/componentCodec.ts` の codec 集合と `sceneFormat.ts` の `bindPelicanScene()`)。したがって `pelican_ray_instance` や `List` フィールドを持つコンポーネントを書いた scene は、**native では scene load 時に検証・拒否されるのに web では黙って通る**状態で、「明示拒否まで実装済み」ではありません。サブセット原則の向きとしては web が緩い側なので、web 側の追随待ちの項目です(web リポジトリは pelican2 の外にあるため、この 1 段落だけは pelican2 のコードでは検証できません)。

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

編集操作・リビジョンの排他制御・プレビュー・rpc メソッドの詳細は [第10章](10_tools.md) と [第13章](13_editor.md) を参照してください。

## 3.10 コンポーネント宣言スキーマ v3 — `Flags` と `List` の wire(✅WP382a)

§3.9 まではドキュメント全体の話でした。この節は **1 個のコンポーネントの中身**をどう書くかです。ここでの **v3 はスキーマ宣言機構の版番号**で、シーン JSON の `version`(pelican.scene v1)とは別物です([用語集](14_glossary.md))。従来の登録経路が運べたのは平坦なフィールド列だけで、`Flags` / `List` のように**入れ子の語彙を持つ型**を運べるようにしたのが v3 です。対象は**ゲームコード側が登録するコンポーネント**(と event payload / behavior params)で、エンジン組み込みの codec 表のコンポーネント(既定 7 件 + ray layout v2 のときだけ増える `pelican_ray_instance`。[componentcodec.cpp](../../src/core/loader/componentcodec.cpp#L1982))はこの節の対象外です(平坦なフィールド列のまま)。

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
- `uninstall_package` が「まだ使われているか」を見るのは **rendering config の root 文書と、その preset だけ**です([renderconfigeditor.cpp:1119](../../src/core/communication/renderconfigeditor.cpp#L1381))。他の feature 文書の中から `project://packages/<ns>/` を参照していても検出されません。
- shader を持ち込む package は未対応です(feature が `engine://` shader だけを参照できる範囲)。

## 3.12 共有部品(render component)— feature 1 本を持ち出して別 project へ入れる(✅P-2a-3)

> **設計決定:** 部品を共有するとは「バイトを copy すること」ではない。feature の文書だけを渡しても、**それが宿主のプロジェクトに何を要求するか**(自分で宣言していないレンダーターゲット)が分からないので、**feature 文書 + 要求の契約**を 1 つのディレクトリに揃えて初めて持ち出せる形になる。持ち出し先は project の外(`user://`)で、取り込み先は project の中。

§3.11 の package が「部品一式のディストリビューション」なのに対し、**render component は feature 1 本の持ち出し**です。使う場面は「自分の project で materialize して手を入れた feature を、別の project でも使いたい」。正本は [rendercomponent.hpp](../../src/project/rendercomponent.hpp#L16)(`pelican.render_component` v1)です。

### 書き出す(export)

RPC [`export_render_feature{scope, index}`](../../src/core/communication/renderconfigeditor.cpp#L1155)(`scope` は `"root"` か `"preset"`、`index` は `features[]` の何番目か)が 1 本を書き出します。書き先は**固定で `user://components/<name>/`**([renderconfigeditor.cpp:944](../../src/core/communication/renderconfigeditor.cpp#L1206))、`<name>` は feature 文書の `name` です。プロジェクトには 1 バイトも書きません(設計決定 7 — [第1章 §1.5](01_overview.md))。

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

RPC [`import_render_feature{base_source_digest, source_dir, destination_reference}`](../../src/core/communication/renderconfigeditor.cpp#L1224) が、component ディレクトリを読んで **`destination_reference`(`project://…` の新規ファイル)に feature 文書を作り、`rendering.features` に足す**ところまでを **1 つのトランザクション**で行います(`base_source_digest` は CAS。[第13章](13_editor.md))。`source_dir` は**絶対パス**でなければならず、`destination_reference` は `project://` で始まる**新規**ファイル(`#` フラグメント不可)です。**この 3 つは別々の code で断られます** —— `project://` でない / `#` を含むは `invalid_feature_destination`([renderconfigeditor.cpp:974](../../src/core/communication/renderconfigeditor.cpp#L1236))、`source_dir` が絶対でないのは `render_component_invalid`([:980](../../src/core/communication/renderconfigeditor.cpp#L1242))、宛先が既にあるのは `feature_destination_exists`([:1026](../../src/core/communication/renderconfigeditor.cpp#L1288))です。

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
| `render_component_requires_missing` | 宿主が宣言していない RT がある(`render_targets` に不足を列挙 — [renderconfigeditor.cpp:1019](../../src/core/communication/renderconfigeditor.cpp#L1281)) |
| `render_component_write_failed` | staging ディレクトリの作成・2 ファイルの書き込み・公開 rename のどれかが失敗した([rendercomponent.cpp:37](../../src/project/rendercomponent.cpp#L37)、[:282](../../src/project/rendercomponent.cpp#L282))。受付では export / import の両方が `RenderComponentError` をそのままの code で返します([renderconfigeditor.cpp:955](../../src/core/communication/renderconfigeditor.cpp#L1217)、[:1050](../../src/core/communication/renderconfigeditor.cpp#L1217)) |
| `invalid_feature_destination` | `destination_reference` が `project://` で始まらない、または `#` を含む(取り込み先の 2 種の 1 つ) |
| `feature_destination_exists` | `destination_reference` のファイルが既にある(同上) |

### 記録(v0 で閉じていないこと)

- `requires.render_targets` は pass の入出力と `render_target_overrides` から作ります。**compute / ray の image binding は見ていません**。
- preset を併用する `pelican.graph` root では、`rendering.rendering_passes[i].region_replacements` が合成に届かない既存の挙動があります([第11章 §11.3](11_status.md))。
- package の `provides.components`(§3.11)はこの形式のディレクトリを指す想定ですが、**複製されるだけで engine 側の消費者はまだありません**。今日 component を読むのは `import_render_feature` だけです。
- **package が持ち込んだ feature は export できません。**vendoring が feature の `name` を `<namespace>.<元の名前>` に書き換えるのに([qualifiedPackageFeatureName()](../../src/project/packagemanifest.cpp#L320))、export 側は「1 つのディレクトリ名として使える名前」しか受けず **`.` を禁じている**ためです([rendercomponent.cpp:231](../../src/project/rendercomponent.cpp#L231))。持ち出したければ、まず `.` を含まない名前で派生を作ります。

## 関連文書

- [../design_project_format.md](../design_project_format.md) — [PF] v6.3(凍結)。形式・PathResolver の正
- [../design_editor_tooling.md](../design_editor_tooling.md) — AuthoringSceneDocument・保存・エディタ経路の設計(§3.9)
- [../design_project_format_web_profile.md](../design_project_format_web_profile.md) — [PFW] v1.3。サブセット原則
- [../design_project_interpretation_layer.md](../design_project_interpretation_layer.md) — 解釈レイヤの設計
- [../design_project_vcs.md](../design_project_vcs.md) — asset store・manifest・雛形
- [../design_persistence.md](../design_persistence.md) — user:// と設定/セーブ三区分
- [第4章 シーンと ECS](04_scene_ecs.md) / [第5章 アセット](05_assets.md) / [第9章 Web プロファイル](09_web.md) / [第13章 エディタ](13_editor.md)
