# 第7章 ツール・RPC・テスト

[索引へ戻る](README.md) / [前章](06_rendering_vulkan_shader.md) / [次章](08_class_interface_index.md)

この章では、engine 本体の周辺にある3つの入口を扱います。

- `pelican_cli`: project 作成、asset manifest、外部納品物 import、配布 feature 導出。
- `pelican_studio`: **Qt Widgets** 製の開発 UI。静的ライブラリ 8 本 + exe(ほかに resource の embed proxy target が 1 つ)で、`src/devstudio` は **31,588 行**(`.cpp` / `.hpp` 73 ファイルの合計)。8 本目は Q-1 が足した [`pelican_studio_automation`](../../src/devstudio/automation/CMakeLists.txt#L8) です。QML は 1 行も使っていません —— 足すとしても葉パネルの内部に限る、というのが §7.6 の規律です。
- RPC と test: headless engine を外から決定論的に操作し、仕様を固定する仕組み。

## 7.1 `pelican_cli` の全体像

[`src/devcli/main.cpp`](../../src/devcli/main.cpp#L15) は第1引数を見て9系統へそのまま dispatch します。dispatch の順が
[usage 行](../../src/devcli/main.cpp#L43)の順です。

```text
pelican_cli
├─ assets <manifest|verify|status>
├─ bake-camera ...                       # replayからcameraパスをbake
├─ import <delivery_dir> --project <dir|project.json>   # --rulesサブモード付き
├─ dist-config <project> [--with rpc,seqplayer] [--out file]
├─ package <install|list|inspect>        # X-1: packageのvendoringと検査(§7.5b)
├─ project init <directory>
├─ dump-lowered-material ...             # material lowering結果のダンプ
├─ vrm ...                               # VRM semanticのダンプ/検査
└─ graph-catalog                         # 登録済みgraph moduleのcatalogをJSONで(§7.5c)
```

執筆時点から5系統増えました。

- `bake-camera`([`bakecameracommand.cpp`](../../src/devcli/bakecameracommand.cpp))は入力 replay から camera パスを bake します(player の `--bake-camera-output` と対。テスト: [`run_devcli_bake_camera.cmake`](../../test/run_devcli_bake_camera.cmake))。
- `dump-lowered-material`([`materialcommand.cpp`](../../src/devcli/materialcommand.cpp))は material lowering 結果をダンプします(テスト: [`run_dump_lowered_material.cmake`](../../test/run_dump_lowered_material.cmake))。
- `vrm`([`vrmcommand.cpp`](../../src/devcli/vrmcommand.cpp))は VRM semantic のダンプ/検査です(テスト: [`run_devcli_vrm_dump.cmake`](../../test/run_devcli_vrm_dump.cmake))。
- `package`([`packagecommand.cpp`](../../src/devcli/packagecommand.cpp#L193))は package ディレクトリを project へ vendoring し、install 済みの digest を照合し、install せずに manifest を読みます(§7.5b。テスト: `package_install_smoke` —— [`package_install_smoke` の登録は `if(PELICAN_WITH_RPC)` の中](../../test/CMakeLists.txt#L2849)で登録され、GPU lease と 300 秒の intrinsic timeout が付きます —— と ctest 未登録の [`run_package_template_smoke.cmake`](../../test/run_package_template_smoke.cmake))。
- `graph-catalog`([`graphcatalogcommand.cpp`](../../src/devcli/graphcatalogcommand.cpp#L10))は `builtinGraphModuleRegistry().catalog()` をそのまま印字します(§7.5c。テスト: [`run_devcli_graph_catalog.cmake`](../../test/run_devcli_graph_catalog.cmake))。
- 既存の `import` には `--rules` サブモード(ルールベース import、[`rulesimport.cpp`](../../src/devcli/rulesimport.cpp) + [`importrules.hpp`](../../src/project/importrules.hpp)、テスト: [`run_devcli_rules_import.cmake`](../../test/run_devcli_rules_import.cmake))と glTF scene 抽出([`gltfsceneextract.cpp`](../../src/devcli/gltfsceneextract.cpp)、テスト: [`run_devcli_gltf_extract.cmake`](../../test/run_devcli_gltf_extract.cmake))が加わりました。

`--rules` から外部ツールを起動する経路のために [`DevCli::runProcess(ProcessOptions)`](../../src/devcli/processrunner.hpp#L34) が追加されました。ヘッダのコメントが規範です。

> Runs one child in its own process group, captures stdout/stderr, and kills the
> complete group on timeout or cancellation. Arguments are filesystem::path so
> Windows callers retain their native UTF-16 representation through CreateProcessW.

`ProcessOptions{executable, arguments, timeout, cancelled, log_path, name}` と `ProcessResult{exit_code, timed_out, cancelled, process_id, stdout_text, stderr_text}` の組で、テストは [`processrunner_test.cpp`](../../test/processrunner_test.cpp) + [`process_fixture_child.cpp`](../../test/process_fixture_child.cpp)、結合は [`run_devcli_import_process.cmake`](../../test/run_devcli_import_process.cmake) です。

command ごとに独立した `run...Command(argc, argv)` を持つ構成で、巨大な application class はありません。argument parse には `argparse`、JSON には `nlohmann::json` を使います。[`src/devcli/CMakeLists.txt`](../../src/devcli/CMakeLists.txt#L1) を見ると、CLI は `pelican_project` へ依存しますが、Vulkan renderer 全体へは依存していません。

## 7.2 `project init`: 実行可能な最小 project の生成器

[`runProjectInitCommand()`](../../src/devcli/projectinit.cpp#L27) は `--template standard|blank`(既定 `standard`)を読んで [`initializeProjectTemplate()`](../../src/project/projecttemplate.cpp#L382) を呼ぶだけで、空であることを確認した directory に template file 群を書くのはこちらです。F-2 で雛形は `pelican_cli` から `pelican_project` の [`projecttemplate.{hpp,cpp}`](../../src/project/projecttemplate.hpp) へ移り、Studio の File > New Project... も同じ関数を呼びます。template の一覧は [`templateFiles()`](../../src/project/projecttemplate.cpp#L329) にあります。

主な生成物は次です。

| 生成物 | 役割 |
|---|---|
| `project.json` | project schema、basic config、default scene、input actions |
| `scenes/main.scene.json` | 最初の scene |
| `assets/asset_data.json` | model/audio などの asset catalog |
| `input/actions.json` | action map |
| `input/profiles/keyboard.json` | input profile |
| `passes/main_rendering_config.json` | 描画 pipeline preset と feature の選択(次項) |
| `ui/ui_overlay.json` | 2D UI overlay |
| `code/CMakeLists.txt`, `code/game.cpp` | game system を組み込む project code。生成される CMakeLists.txt は `pelican_game_sources()` を呼ぶため、game code は **DLL**(`pelican_game_logic`)としてビルドされます。X-1 以降は `pelican_packages()` の 1 行が**その後ろに**続きます([template](../../src/project/projecttemplate.cpp#L265)) —— package の object は、先に作られた game logic DLL へ合流するので順序が効きます |
| `.gitattributes`, `.gitignore`, `README.md` | asset/LFS と利用案内 |

template は外部 resource file ではなく、[`projecttemplate.cpp` 内の raw string](../../src/project/projecttemplate.cpp#L89) です。schema を変更した場合、example project だけでなくここも更新しないと、新規 project が古い形式で生成されます。

この command の end-to-end 仕様は [`run_devcli_project_init.cmake`](../../test/run_devcli_project_init.cmake) です。file が揃っているかの確認だけでなく、生成した project directory を **prebuilt の `pelican_player`** に `--headless --frames 2 --render-out` で食わせ、exit 0・出力に `Validation Error` / `VUID-` が出ないこと・PNG が空でないことまで見ます(生成された `code/game.cpp` を build する経路ではありません)。空でない directory を拒否し、そのエラーが directory 名を含むことも同じ script が固定しています。実 device を要するので CTest では `gpu` label 付きです([`test/CMakeLists.txt`](../../test/CMakeLists.txt) の `devcli_project_init_command`)。

**`blank` 雛形(F-2)。**`--template blank` は上の 16 エントリに `shaders/present.frag` を足し、`passes/main_rendering_config.json` の `rendering` 節だけを差し替えます。中身は `pipeline` キーの無い direct config で、preset・`features`・`render_targets` を持たず、fullscreen pass `present` 1 本が `display` へ UV の gradient を書きます([`blank_rendering_config_json`](../../src/project/projecttemplate.cpp#L211))。同じ `run_devcli_project_init.cmake` がこの project を headless で 2 frame 描かせ、PNG が 100 色以上で、最も多い 1 色以外の画素が 9 割以上(最多色が 1 割以下)であることを `pelican_test_png_nonuniform_check` で見ます([検査](../../test/run_devcli_project_init.cmake#L224))。ファイルの形は GPU 無しの `devcli_project_templates`([`run_devcli_project_templates.cmake`](../../test/run_devcli_project_templates.cmake))が見ます —— 既定と `--template standard` の 16 ファイルが F-2 前の出力と SHA-256 で一致すること、blank の形、知らない template 名の拒否です。

### 生成される rendering config は preset 参照の `pelican.graph` root(WP240a → P-2a-4b)

`passes/main_rendering_config.json` の template は 111 行の手書き設定から数行になり、さらに
**`pelican.graph` root** になりました。ソースに置かれているのは「節の中身」だけです
([`rendering_config_json`](../../src/project/projecttemplate.cpp#L198))。

```json
{
  "pipeline": { "preset": "engine://render_pipelines/hybrid_v1.json" },
  "features": [
    "engine://features/shadow_directional.json",
    "engine://features/sky_ambient.json"
  ]
}
```

生成されるファイルはこれを [`wrapLegacyRenderingConfig()`](../../src/project/renderconfigsection.hpp#L62) に通した結果で、
`schema: "pelican.graph"` / `version: 1` / 空の `definitions` / `nodes` / `outputs` と、上の object を
`rendering` メンバとして持ちます([生成](../../src/project/projecttemplate.cpp#L249))。**包み方が 1 関数しかない**ので、
出荷 project と fixture が「同じ規則で包まれたか」を値の比較で言えます —— 慣習として 2 箇所に書くのをやめた形です。
どのキーがどちらの層に属するかは [`isGraphRootKey()`](../../src/project/renderconfigdocument.hpp#L15) が決めます。

旧 template は `render_targets` 8 本(gbuffer 5 枚 + depth + ssao 2 枚)と `rendering_passes` を自分で書き下し、pass は gbuffer_pass / ssao_pass / ssao_blur_pass / present の 4 つでした。末尾の `present` が `uses_light_data: true` を持ち、ライティングと present を兼ねていました。新 template はこの 2 つの member を **持ちません**。

読む位置が一段動いたのがこの変更の要点です。

- `project init` が **何を書くか** → [`rendering_config_json`](../../src/project/projecttemplate.cpp#L198) を `wrapLegacyRenderingConfig()` に通した `pelican.graph` root(上の block はその `rendering` 節の中身で、ファイルの全文ではありません)
- 生成された project が **実際にどの pass を回すか** → [`src/core/resources/render_pipelines/hybrid_v1.json`](../../src/core/resources/render_pipelines/hybrid_v1.json) と [`src/core/resources/features/shadow_directional.json`](../../src/core/resources/features/shadow_directional.json)

preset 側の `main_render` グラフは deferred_geometry / ssao_pass / ssao_blur_pass / deferred_lighting / forward_opaque / `__snapshot_opaque_color` / `__snapshot_opaque_depth` / forward_transparent / scene_present で、`shadow_depth` は preset ではなく feature 側が足します。つまり新規 project は最初から forward・半透明・opaque snapshot・directional shadow を持ちます。preset と feature をどう解決して 1 本のグラフにするかは[第6章](06_rendering_vulkan_shader.md)の担当です。

テスト側は既存 1 本の拡張と新規 1 本です。どちらも `gpu` label です。

| テスト | 見ているもの |
|---|---|
| `devcli_project_init_command`(既存を拡張。[`run_devcli_project_init.cmake`](../../test/run_devcli_project_init.cmake#L48)) | 生成された config が **`pelican.graph` root で `rendering` が object** であること、`rendering.pipeline.preset` が hybrid_v1、`rendering.features` が shadow_directional と sky_ambient のちょうど 2 本、`rendering` の中に `render_targets` / `rendering_passes` が **無い**こと、`code/CMakeLists.txt` が `pelican_game_sources` の後に `pelican_packages()` を呼ぶこと。**root の形を先に確かめる順序が効きます** —— 節ごと消えたときに禁止メンバの検査が真空で通るのを防ぎます |
| `animgraph_demo_preset_headless_player`(新規。[`run_preset_project_headless.cmake`](../../test/run_preset_project_headless.cmake)) | 同じ preset へ移行した `projects/animgraph_demo` を `--dump-frame-plan` 付きで 3 frame 回し、frame plan に `"graph": "main_render"` と 8 個の node 名が現れること |

後者が数える 8 node は shadow_depth / deferred_geometry / deferred_lighting / forward_opaque / `__snapshot_opaque_color` / `__snapshot_opaque_depth` / forward_transparent / scene_present です(ssao の 2 本は検査に入れていません)。「preset を参照するだけの config が本当に完全なグラフへ展開されるか」を、生成物ではなく実在の project で押さえる位置にいます。禁止 member の検査(`render_targets` / `rendering_passes` が **無い**こと)が両方に入っているのは、preset へ移したはずの記述が config 側へ再び生えてくるのを防ぐためです。

## 7.3 `assets`: store と manifest の保守

入口は [`runAssetsCommand()`](../../src/devcli/assetscommand.cpp#L363) です。project の `asset_stores` と `.pelican/local.json` を読み、store ごとの実 directory と manifest を解決します。

| subcommand | 実装 | 動作 |
|---|---|---|
| `manifest` | [`runManifestCommand()`](../../src/devcli/assetscommand.cpp#L274) | store を走査して manifest と hash cache を更新 |
| `verify` | [`runVerifyCommand()`](../../src/devcli/assetscommand.cpp#L294) | manifest と filesystem を比較。`--full` なら内容も再 hash |
| `status` | [`runStatusCommand()`](../../src/devcli/assetscommand.cpp#L323) | store/manifest/asset の missing と大小文字不一致を軽量確認 |

hash と path の純粋ロジックは [`src/project/assetsmanifest.cpp`](../../src/project/assetsmanifest.cpp#L1)、起動時の runtime 検証は [`src/core/loader/assetsverification.cpp`](../../src/core/loader/assetsverification.cpp#L1) に分かれます。CLI は manifest を「作る・事前確認する」側、core は project 起動時に「利用可能か確認する」側です。

`--full` でない manifest/verify は `.pelican/assets-hash-cache.json` を利用します。file metadata で cache hit できる場合は内容全体を読み直さず、大規模 store の反復確認を軽くします。厳密な配布前確認では `--full` を使う前提です。

仕様テストは [`assetsmanifest_test.cpp`](../../test/assetsmanifest_test.cpp#L1)、[`assetsverification_test.cpp`](../../test/assetsverification_test.cpp#L1)、CLI 結合テストは [`run_devcli_assets.cmake`](../../test/run_devcli_assets.cmake#L1) です。

## 7.4 `import`: 外部 delivery を安全に project catalog へ登録

[`importDelivery()`](../../src/devcli/importcommand.cpp#L337) は、project 内に置かれた delivery directory の `manifest.json` を読みます。大まかな transaction は次です。

```text
project / delivery pathをcanonicalize
  -> deliveryがproject root内か確認
  -> manifest schemaをparse
  -> output pathがdelivery外へescapeしないか確認
  -> 全outputのSHA-256を確認
  -> gltf schemaかつ.glbだけを抽出
  -> asset_data.modelsへ未登録分を追加
  -> asset_data JSONを書き戻す
```

安全性の要点は3つあります。

1. [`resolveProjectFile()`](../../src/devcli/importcommand.cpp#L165) は absolute path、backslash、未知 scheme、`..` による project root escape を拒否します。
2. [`verifyManifestOutputs()`](../../src/devcli/importcommand.cpp#L273) は登録前に全 output の SHA-256 を検証します。途中まで catalog を変更しません。
3. [`registerImportedModels()`](../../src/devcli/importcommand.cpp#L288) は既存の model 名または path と重複すれば skip するため、同じ delivery を再実行しても重複登録しません。

なお「transaction」は catalog 書き換え前の検証順という意味です。file write 自体は [`writeTextFile()`](../../src/devcli/importcommand.cpp#L148) の truncate write であり、temp file + rename の atomic replace ではありません。

manifest の純粋 parser は [`src/project/importmanifest.cpp`](../../src/project/importmanifest.cpp#L1)、fixture 仕様は [`importmanifest_test.cpp`](../../test/importmanifest_test.cpp#L1)、CLI の実 filesystem 動作は [`run_devcli_import.cmake`](../../test/run_devcli_import.cmake#L1) が固定しています。

## 7.5 `dist-config`: project 内容から配布用 feature を導出

[`deriveDistConfig()`](../../src/devcli/distconfig.cpp#L833) は project を静的走査し、配布 build に必要な optional feature を決めます。

| feature | 導出方法 |
|---|---|
| `PELICAN_WITH_VAT` | asset data と import manifest にある GLB の JSON chunk を読み、VAT(Vertex Animation Texture — 頂点ごとの動きをテクスチャへ焼き込んで再生する方式)metadata を検査 |
| `PELICAN_WITH_EXR` | asset/UI JSON 内の `path` / `file` に `.exr` reference があるか走査 |
| `PELICAN_WITH_RPC` | 自動推測せず `--with rpc` で明示 |
| `PELICAN_WITH_SEQPLAYER` | 自動推測せず `--with seqplayer` で明示 |

GLB 候補は asset catalog と project 以下の import manifest の両方から集めます。入口は [`collectAssetDataGlbs()`](../../src/devcli/distconfig.cpp#L370) と [`collectImportManifestGlbs()`](../../src/devcli/distconfig.cpp#L403) です。単に拡張子を見るだけでなく、GLB JSON chunk と VAT primitive metadata を読み、壊れた VAT declaration を「feature なし」として黙殺しない設計です。

結果は [`renderDistConfigPreset()`](../../src/devcli/distconfig.cpp#L867) が `set(PELICAN_WITH_... CACHE BOOL ... FORCE)` 形式の CMake include file にします。各判定の理由も comment へ出るため、「なぜこの依存が配布物に入ったか」を追跡できます。

command line は [`runDistConfigCommand()`](../../src/devcli/distconfig.cpp#L900)、結合仕様は [`run_devcli_dist_config.cmake`](../../test/run_devcli_dist_config.cmake#L1) です。

> 🧩 **難所 — 前方一致では判定しない**([`isWithinRoot()`](../../src/devcli/distconfig.cpp#L121) / [`pathComponents()`](../../src/devcli/distconfig.cpp#L113))
>
> **何をする所か**: 走査中に見つけた参照先が本当に project root の下にあるかを判定します。外れていれば `escapes project root` を投げて `dist-config` ごと失敗させます。
>
> **前提**: [`pathString()`](../../src/devcli/distconfig.cpp#L59) は `std::filesystem::path` を区切り文字ごと 1 本の文字列へ戻すだけの補助関数(実体は `path.string()`)で、このファイルでは主に例外メッセージの組み立てに使われます。以下の「前方一致」は、この 1 本の文字列同士を `starts_with` で比べる書き方のことです。
>
> **素朴に読むと**: 見た目は「Windows の大小無視のためにパスをコンポーネント分解している」だけに見えます。しかし本体はサンドボックス判定(外部由来の参照が指定ディレクトリの外を指していないかの検査)で、`pathString()` の文字列比較にしなかった理由は大小無視ではありません。前方一致だと root `C:/Foo` の下に `C:/Foobar/model.glb` が入っていると判定してしまいます — 一致の境界がたまたま区切り文字と一致しないからです。コンポーネント単位なら `"foo" != "foobar"` で確実に落ちます。Windows で各コンポーネントを小文字化する [`comparableComponent()`](../../src/devcli/distconfig.cpp#L98) は、この比較に付随する処理にすぎません。もう一つの前提は呼び出し側にあります。この関数は `..` を畳まないので、正規化前のパスを渡すと `root/../secret` の先頭コンポーネント列が root と一致し、「root の下」と判定されます。
>
> **骨子**:
> ```text
> isWithinRoot(root, candidate):
>   candidate のコンポーネント数 < root のコンポーネント数 -> false
>   先頭から root のコンポーネント数ぶんだけ 1 対 1 で比較(Windows は小文字化して比較)
>   1つでも不一致 -> false / 全一致 -> true
> ```
>
> **手がかり**: 呼び出しは2箇所だけで、どちらも直前に [`weaklyCanonicalOrThrow()`](../../src/devcli/distconfig.cpp#L70) を通した絶対パスを渡しています — [`resolveProjectRef()`](../../src/devcli/distconfig.cpp#L205) と [`addMaybeExistingGlbCandidate()`](../../src/devcli/distconfig.cpp#L354)。root 側も [`loadProjectFiles()`](../../src/devcli/distconfig.cpp#L270) が `canonicalDirectoryOrThrow()` で正規化済みにしています。つまり `..` を実際に潰しているのは `weakly_canonical` であって `isWithinRoot()` ではありません。
>
> **不変条件**: 両引数は正規化済みの絶対パスであること。これはコード上どこにも明文化されていない暗黙の前提で、新しい呼び出し箇所を足すときの最大の落とし穴です。パスの包含判定に `pathString()` の前方一致を使わない。

## 7.5b `package`: project へ vendoring する(X-1)

[`runPackageCommand()`](../../src/devcli/packagecommand.cpp#L193) は `install` / `list` / `inspect` の 3 つを
分けるだけの薄い入口で、規則は全部 [`src/project/packagemanifest.hpp`](../../src/project/packagemanifest.hpp#L13) にあります。
header の冒頭コメントが規範です。

> X-1: a package is a directory with a `pelican.package` manifest.  It is the
> one carrier that can leave the engine tree: the author writes it anywhere,
> and installing it copies the whole directory into
> `<project>/packages/<namespace>/`.  Nothing in the engine resolves a package
> outside a project, so "where it came from" stops mattering the moment it is
> vendored.

| subcommand | 実装 | 動作 |
|---|---|---|
| `install <dir> --project <p>` | [`runInstall()`](../../src/devcli/packagecommand.cpp#L88) | `installPackage()` を呼び、`{status: "installed", namespace, package_version, sha256, destination, feature_references, feature_names}` の 7 鍵を印字 |
| `list --project <p>` | [`runList()`](../../src/devcli/packagecommand.cpp#L117) | `project.json` の記録を読み、**実バイトの sha256 を計算し直して**照合 |
| `inspect <dir>` | [`runInspect()`](../../src/devcli/packagecommand.cpp#L170) | install せずに manifest を検証して dump。`sdk_abi_expected` も添える |

設計上の要点は 3 つです。

1. **検査は 1 byte も書く前に終わります。**install の拒否(manifest 不在・不正、engine が古い、依存不足、
   feature 文書が不正、同名が install 済み、SDK ABI 不一致)は全部名前付きで、複製は staging ディレクトリを
   rename して行われます。
2. **digest は書かれるだけでなく読まれます。**`list` が [`packageTreeDigest()`](../../src/project/packagemanifest.hpp#L100)
   を計算し直し([再計算](../../src/devcli/packagecommand.cpp#L142))、ずれていれば `status: "modified"` + stderr の
   `package_digest_mismatch` で非 0 終了です([記録した digest との比較](../../src/devcli/packagecommand.cpp#L144))。
   vendoring された package を手で編集したことが見えます。**この失敗旗は digest 違いだけのものではありません** ——
   vendoring 先のディレクトリごと消えていれば `status: "missing"` が同じ旗を立て、同じ
   `package_digest_mismatch` の行で終わります([不在の側](../../src/devcli/packagecommand.cpp#L137))。
3. **拒否理由は例外が運びます。**[`PackageError`](../../src/project/packagemanifest.hpp#L27) は `code()` と、
   数え上げるべき名前(欠けた依存、まだ package を指している参照)を `names()` で持ちます。CLI も RPC も
   メッセージから理由を再構成しません。

## 7.5c `graph-catalog`: module 登録の可視化

[`runGraphCatalogCommand()`](../../src/devcli/graphcatalogcommand.cpp#L10) は引数を取らず、
`builtinGraphModuleRegistry().catalog()` を印字するだけの 9 行です。出力は
[`GraphModuleRegistry::catalog()`](../../src/project/graphmodule.cpp#L227) が組み立てる `pelican.graph_catalog` v1 で、
module → types / operations / sinks の順に並びます。**並びは名前順**です(module と operation は名前を鍵にした
`std::map`、types と sinks は明示的に sort)。登録順に依存しないので、module を足しても既存の行は動きません。

[`run_devcli_graph_catalog.cmake`](../../test/run_devcli_graph_catalog.cmake#L23)(ctest 名 `devcli_graph_catalog`)は
module 数(既定 4。`PELICAN_WITH_ANIMATION=OFF` と `PELICAN_WITH_AUDIO=OFF` でそれぞれ 1 減る)、各 operation が `domain` / `evaluation` /
**`change.default`**(`change` は入れ子の object で、script が読むのはその `default`。[読む位置](../../test/run_devcli_graph_catalog.cmake#L59))/ `sink` を
持つこと、`value.trigger_clock` の評価分類が `replay`、`render.buffer` の parameter change が `rebuild` であることを
見ます。**さらに `docs/v0_freeze.md` の操作表と突き合わせます** —— 表は `<!-- graph-op-table:begin -->` /
`<!-- graph-op-table:end -->` のコメントで区切られており、script の中のコメントがその理由です。

> A copy nobody compares is a second source of truth, so the same run that reads the catalog
> reads the table: the documented set is exactly what the ON build registers, and a superset
> of what the OFF build does -- the difference being the pose operations the disabled module
> owns.

区切りを heading ではなくコメントにしてあるのは、**前後の散文を書き直しても比較が黙って空にならない**ようにするためです。

## 7.6 Pelican Studio の現在位置

Studio の起点は [`src/devstudio/main.cpp`](../../src/devstudio/main.cpp#L5) です。Qt application を作る [`uimain()`](../../src/devstudio/view/uimain.cpp#L24) から [`MainWindow`](../../src/devstudio/view/mainwindow.hpp#L49) を表示します。

**「full editor ではない」という言い方はもう正確ではありません。**scene(object / component / gizmo)、
render config(`features[]` と feature パラメータ)、preset・pass・compute task、そして共通グラフの
authoring が Studio から通ります。engine 内部への裏口はどれも通らず、公開 RPC だけを呼びます。

```mermaid
flowchart LR
    Main["devstudio main"] --> UI["uimain / QApplication"]
    UI --> Window["MainWindow / QMainWindow"]
    UI --> Automation["AutomationServer / --automation only (Q-1)"]
    Automation --> Window
    Automation --> Viewport
    Automation --> Driver["tools/studio_drive.py / scenario + report.json"]
    Window --> Workspace["central workspace"]
    Workspace --> Viewport["EmbeddedViewport / native host"]
    Viewport --> Process["EngineProcess / QProcess"]
    Process --> Player["pelican_player / foreign child HWND"]
    Viewport --> Rpc["pick / query / edit / watch / frame-plan JSON-RPC"]
    Rpc --> Selection["SelectionModel / public identity"]
    Docks --> Selection
    Selection --> Inspector["InspectorWidget"]
    Inspector --> InspectorModel["InspectorModel / schema + edit state"]
    InspectorModel --> Rpc
    Process --> LogBuffer["EngineLogBuffer / bounded history"]
    LogBuffer --> LogDock["Engine Log dock"]
    Inspector --> Attach["GraphAttachmentWidget / Attached graphs"]
    Attach --> AttachModel["GraphAttachmentModel / mode + stored + live, op lists"]
    AttachModel --> Rpc
    Window --> Tabs["pelican.renderTools / QTabWidget (3 tabs)"]
    Tabs --> PlanWidget["FramePlanWidget / 9 tool tabs"]
    Tabs --> Features["RenderFeaturesWidget / features, params, materialize"]
    Tabs --> Canvas["GraphCanvasWidget + GraphCanvasScene"]
    PlanWidget --> PlanModel["FramePlanModel / compact projection"]
    PlanModel --> PlanDock["Frame Plan dock"]
    Features --> Rpc
    Canvas --> CanvasModel["GraphCanvasModel / Qt-free, 4 reads + 1 edit"]
    CanvasModel --> CanvasCap["GraphCanvasReadCapability"]
    CanvasCap --> Rpc
    Window --> PassForm["FullscreenPassWidget / fullscreen, material, raster"]
    PassForm --> PlanCap["FramePlanReadCapability / get_frame_plan + context"]
    PassForm --> PassCap["RenderPassAuthoringCapability / context, add, remove, edit_preset_pass, edit_result"]
    PlanCap --> Rpc
    PassCap --> Rpc
    Window --> Docks["QDockWidget panels (7)"]
    Window --> Layout["LayoutPresetManager"]
    Layout --> Files["versioned named presets"]
```

図の右下の 3 つが **capability struct** です。`EmbeddedViewport` の「任意のメソッドを送れる」面を、
widget ごとに**名前の付いた少数の関数だけを持つ struct** へ絞ってから渡します
([`GraphCanvasReadCapability`](../../src/devstudio/view/graphcanvasreadcapability.hpp#L31) /
[`RenderPassAuthoringCapability`](../../src/devstudio/view/renderpassauthoringcapability.hpp#L19) /
`FramePlanReadCapability`)。**汎用の RPC メソッドを表現できない型**なので、その widget から別の面が
漏れ出すことが構造上ありません。`FramePlanWidget` と `RenderFeaturesWidget` は歴史的な理由で
`EmbeddedViewport *` を直接受け取ります(絞り込みはこの 2 つには入っていません)。

[`MainWindow::MainWindow()`](../../src/devstudio/view/mainwindow.cpp#L156) は Project / Outliner /
Inspector / Output / Engine Log / Frame Plan / Pass Editor の7パネルを stable object name を持つ dock として作ります。パネルは
移動、float、タブ化でき、`View > Panels` から再表示できます。シェル責務は Widgets に固定し、QML を
追加する場合も `QQuickWidget` に載せた葉パネルの内部だけに限定します。

**Frame Plan dock の中身は 3 枚の tab を持つ `QTabWidget`** です(object name `pelican.renderTools`)。
`FramePlanWidget` / `RenderFeaturesWidget` / `GraphCanvasWidget` がこの順で入ります
([構築](../../src/devstudio/view/mainwindow.cpp#L273))。**Graph は新しい dock ではありません** ——
ソースのコメントが理由です: dock を 1 つ増やすことは「見失う場所」を 1 つ増やすことで、3 つの tab は
同じレンダー設定を別の角度から見ているだけだからです。

tab は CLI から選べます。`pelican_studio --project <dir> --tab frame_plan|render_features|graph` で、
[名前の一覧](../../src/devstudio/view/mainwindow.cpp#L696)は `MainWindow` の static 関数にあり、
**window を作る前**に [`main.cpp`](../../src/devstudio/main.cpp#L36) が綴りを検査して、未知なら console に
理由を出して **exit 2** します。適用は `openProject()` の**後**です([順序](../../src/devstudio/view/uimain.cpp#L71))
—— project を開くとその project の tool layout が復元され、前面の tab が変わるためです。

中央の [`EmbeddedViewport`](../../src/devstudio/viewport/embeddedviewport.hpp#L29) は
`pelican_player` を [`EngineProcess`](../../src/devstudio/viewport/engineprocess.hpp#L24) で別 process
として起動・監視し、実 render window を Qt の native host HWND へ再親付けします。
[`NativeWindowHost`](../../src/devstudio/viewport/nativewindowhost.hpp#L38) が Windows style、parent、
focus、physical-pixel resize だけを扱い、renderer や swapchain を直接再生成しません。したがって
resize は player の通常の GLFW framebuffer callback から既存の Surface/Swapchain epoch 経路へ
入ります。通常の resize は最後の寸法イベントから 50 ms 静止するまで native child を凍結し、
最終 physical extent だけを 1 回適用します。DPR 変更だけは物理画素寸法を壊さないよう即時適用です。
凍結中に child が覆わない部分は native host が黒で自動消去するため、過去の画素は残りません。

player が終了しても Studio は残り、viewport から再起動できます。逆方向は Windows の
kill-on-close Job Object で固定し、player を process 作成時点から所属させます。そのため Studio の
通常終了だけでなく強制終了でも player は残りません。

child の stdout は行単位で JSON-RPC 応答と通常出力に分け、stderr は別 channel のまま読みます。
対応する request id の応答だけを RPC signal へ送り、通常 stdout と stderr は一つの未切り詰め
output signal として viewport 経由で Engine Log dock へ送ります。RPC は 5 秒で明示 timeout し、
process 終了時も pending request を失敗にするため、入力 pipe や feature の問題を黙殺しません。
[`EngineLogBuffer`](../../src/devstudio/viewport/enginelogbuffer.hpp#L7)
は UTF-8 換算 1 MiB の末尾だけを保持し、通常は最古の途中行も捨てます。この大きさは診断用の長い
履歴を残しながら、model と `QPlainTextEdit` に複製される text の常駐量を長時間起動でも制限するため
です。単一の巨大行だけでも末尾を残し、UTF-8 の途中 byte から復号しません。上限規則と両 stream の
到達は [`devstudio_viewport_test.cpp`](../../test/devstudio_viewport_test.cpp#L106) が view 表示なしで検査します。

読む前に、応答の階層だけ掴んでおきます。`get_frame_plan` の 1 応答の中に、名前の層と物理の層が入れ子で
入っています([組み立ては `Renderer::currentFramePlanJson()`](../../src/core/vkcore/renderer.cpp#L3889))。

```text
get_frame_plan 応答 = pelican.frame_plan v1
  ├ nodes / levels / barriers / resources : FramePlan — planner が決めた名前と実行順(第6章 §6.3)
  └ physical_target_plan                  : その名前を実画像へ落とした物理側(attachments / resources / alias / native scope)
```

Graph タブが読む応答も同じ読み方をします。1 応答に「今の版」と「文書の 2 つの層」が入っています
([組み立ては `getGraphInstances()`](../../src/core/communication/renderconfigeditor.cpp#L2185))。

```text
get_graph_instances 応答
  ├ source_reference / source_digest : 編集の CAS に添える版(文書全体の sha256)
  ├ instances                        : root の nodes のうち op == "graph.instance" のものだけ
  ├ nodes                            : root の nodes 配列そのもの(producer を探し、id の重複を避けるため)
  └ definitions                      : definition 本体(scope を降りて描くのに要る)
```

`instances` が `nodes` の部分集合として**別に**返るのは、Inspector の装着一覧が root の全ノードを
知らなくても済むようにするためで、キャンバスの側は `nodes` と `definitions` を使います。

[`FramePlanModel`](../../src/devstudio/model/frameplanmodel.hpp) は `get_frame_plan` の
`pelican.frame_plan` v1 応答を、順序付き pass / compute task、入出力と履歴入力、resource、barrier、
attachment load/store、material route、backend 非依存 execution facts へ縮約する Qt 非依存 model です。
6,000 行級になる raw 応答や `physical_target_plan` 全体は保持せず、ImGui Plan Viewer と同じ調査の
入口になる項目だけを値型へ写します。attachment ops と resource format は公開応答内の
`physical_target_plan.attachments/resources` から昇格しますが、native scope、alias、physical decision
など Compiled Plan Viewer 相当の低レベル情報は UI へ出しません。

[`FullscreenPassWidget`](../../src/devstudio/view/fullscreenpasswidget.hpp#L22) は **下書き専用ではありません**。
2 つの capability を取り([capability 2 つを受ける ctor](../../src/devstudio/view/fullscreenpasswidget.hpp#L29))、読みは
`FramePlanReadCapability`、書きは [`RenderPassAuthoringCapability`](../../src/devstudio/view/renderpassauthoringcapability.hpp#L19)
(`get_render_authoring_context` / `add_authored_pass` / `remove_authored_pass` / `edit_preset_pass` /
`get_edit_result` の 5 つだけ。context は `requestContext` という別名で持ちます。[結線](../../src/devstudio/view/renderpassauthoringcapability.cpp#L30))です。
フォームは `fullscreen` 固定でもありません —— material の契約・設定・routing、raster の `Draw settings (JSON)` と
`Raster state (JSON, optional)` の欄を持ちます([組み立て](../../src/devstudio/view/fullscreenpasswidget.cpp#L384))。
ボタンは **Save authored pass** / **Remove authored pass** / **New pass**
([構築](../../src/devstudio/view/fullscreenpasswidget.cpp#L590))で、preset 由来の pass を開いているときは
save の文言が **Save preset**(feature 由来なら **Save feature**)に変わります(N-7 で短くなった)
([切り替え](../../src/devstudio/view/fullscreenpasswidget.cpp#L2137))。
変わっていないのは所有の向きです: 対象候補は `get_frame_plan` の `resources[]` から作り、フィールド所有・
同一 graph の名前衝突・target 名／`frame_target` を Studio 単独で検査し、target usage や shader stem 解決は
軸名付きの `Not checked` として残します。**ファイルは engine が書きます。**

[`FramePlanWidget`](../../src/devstudio/view/frameplanwidget.hpp#L23) はその model を **9 枚の tool tab** に投影します ——
Logical graph / Passes / Resources / Physical plan / Decisions / Backends / Barriers / Materials / Raw JSON
([登録](../../src/devstudio/view/frameplanwidget.cpp#L869))。tab は掴んで並べ替えられ、その順序は保存されます。
**読み取り専用でもありません**: toolbar に **Order nodes...** / **Edit task...** / **Edit pass...** があり、
`Shift-drag: connect resources` のラベルどおり **Shift + 左ドラッグ**で出力元から task / preset pass へ線を引くと、
その編集画面が開きます([toolbar](../../src/devstudio/view/frameplanwidget.cpp#L552))。保存自体は
`RenderFeaturesWidget` と `FullscreenPassWidget` が持つ編集 RPC を通ります。
初期状態は順序、名前、種別、入力、出力の pass 行だけで、
attachment ops、resource use、material filter、前後 barrier は折りたたみ配下です。filter も全 tree の
要約と子項目に掛かるため、raw JSON を全部展開しなくても流れを追えます。取得契機は **player の RPC が
利用可能になった直後の1回と Refresh ボタンだけ**です。毎 frame の取得は大きな JSON の pipe 転送、
二度の parse、tree 再構築を描画周期ごとに行うため採りません。hot reload 後など変化を確認したい時は
明示 refresh します。player 未起動・停止・RPC 失敗は空表示にせず、理由と再開方法を status 行へ出し、
停止時には古い snapshot も消します。応答からの構造化と current-version-only 検証は
[`devstudio_frameplan_test.cpp`](../../test/devstudio_frameplan_test.cpp) が GUI なしで固定します。

### 共通グラフの 3 層(N-1 / N-1b / N-2)

[`GraphCanvasModel`](../../src/devstudio/model/graphcanvasmodel.hpp#L167) は **Qt に依存しません**。
`GraphAttachmentModel` と同じ「request / read の 2 slot」形で、呼び手が `takeRpcRequests()` を drain し、
好きな transport で送り、`receiveRpcResult()` / `receiveRpcFailure()` で答えを戻します。slot が 2 つあるのは、
**値の問い合わせが次の文書再取得を待たせないため**で、遅れて届いた古い答えは捨てられます。
提供するのは scope 一覧(`root` + definition 名)、scope ごとの node と edge、root scope の `graph.instance` へ
重ねる値の overlay、そして `contentRevision()` / `reportRevision()` の 2 つの世代です
([宣言](../../src/devstudio/model/graphcanvasmodel.hpp#L475))。編集側は 6 つの操作
(`connectInput` / `disconnectInput` / `setNodeParameter` / `removeNode` / `setNodeLabel` / `addNode`)で、
**どれも 1 回で 1 つの `edit_graph_document`** になります([submitOps](../../src/devstudio/model/graphcanvasmodel.cpp#L915))。
`addNodeOps()` だけは複数 op を返します —— 必須の scalar 入力を持つ操作は、未接続のままでは engine が受け取らないので、
**同じ ops 列で `value.constant` を先に足して**そこへ繋ぎます([生成](../../src/devstudio/model/graphcanvasmodel.cpp#L1129))。
種の定数は後で本物の producer を繋いでも自動では消えません(記録された穴)。

[`view/nodegraphics.{hpp,cpp}`](../../src/devstudio/view/nodegraphics.hpp#L35) は
`frameplangraphics.cpp` から**共有部分を抜き出した**ものです。`MovableNodeItem`、`CurveGeometry` と
`curveGeometry(Between)`、`routeLogicalBundle` / `rerouteConnectedBundles`、`findNodeItem` /
`findEndpointArtifact`、`nodePath` / `roundedEntityPath`、配置定数、そして
[`longestPathColumns()`](../../src/devstudio/view/nodegraphics.hpp#L190)(model に依存しない最長経路の段組み)です。
item data の role 番号は **Frame Plan が既に公開していた値のまま**で、挙動を変えないための選択です。
N-1b でソケットの語彙が加わりました: [`SocketShape` / `SocketStyle` / `socketStyleFor()` /
`socketPath()`](../../src/devstudio/view/nodegraphics.hpp#L117)。型を形でも表すのは、**利用者が型を足せるキャンバスでは
色相が足りない**からで、形は色覚と暗いテーマの両方を越えます。解決順は 1 箇所に集約されていて、
(i) catalog の型 schema が宣言した `display{}` → (ii) Studio の既定表 → (iii) traits → (iv) `unknown` です。
既定表は [`resources/graph_socket_styles.json`](../../src/devstudio/resources/graph_socket_styles.json#L1)
(`pelican.studio.socket_styles` v1)で、**型に形を与えるのに C++ を触りません**。
`resources/CMakeLists.txt` は `b_embed_proxy_target(pelican_studio_view pelican_studio_resources)` で
proxy target を作ります —— view の `.cpp` を直接 compile する scene test も、同じ埋め込みテーブルを link できるようにするためです。
抽出が Frame Plan の挙動を変えていないことは、`devstudio_frameplan_graph_test`(2,133 assertion)と
`devstudio_fullscreen_pass_test`(803 assertion)が**全数不変**であることで押さえました
(assertion 数と「不変」の判定は N-1 の完了記録 `edafc41d` による。本章で実測し直したものではありません)。

N-5 で色が 1 枚の表に集まりました。[`StudioTheme`](../../src/devstudio/view/studiotheme.hpp#L44) は
`{name, label, style, palette, canvas{background, grid, edge, edge_selected, node_body, node_border,
heading_text, dim_text, pill_bg}, domain_tint, source_tint}` で、描画側はどこも
[`currentStudioTheme()`](../../src/devstudio/view/studiotheme.hpp#L65) 越しに引きます。値の出所は
埋め込みの [`resources/studio_themes.json`](../../src/devstudio/resources/studio_themes.json#L1)
(`pelican.studio.themes` v1、`light` / `dark` の 2 preset で既定は `dark`)なので、3 つ目のテーマを足すのに
C++ は要りません。切り替えの通知は Qt signal ではなく**素の関数リスト**
([理由はヘッダのコメント](../../src/devstudio/view/studiotheme.hpp#L89)) —— scene test が view の `.cpp` を
moc 無しで compile するためで、`GraphCanvasWidget` は [repopulate](../../src/devstudio/view/graphcanvaswidget.cpp#L890)、
Frame Plan は [グラフの再描画](../../src/devstudio/view/frameplangraphics.cpp#L1952) で追随します
(item は作られた時の色を抱えるので、描き直す以外に追随する道がありません)。**ソケットの型別の fill だけは
テーマから引きません** —— 型の識別色は `graph_socket_styles.json` のままで、テーマが決めるのは縁と
「どの型とも言えない」場合の fill です([分岐](../../src/devstudio/view/nodegraphics.cpp#L110))。

[`GraphCanvasWidget`](../../src/devstudio/view/graphcanvaswidget.hpp) と `GraphCanvasScene` が view です。
上段は `Scope:` の combo とパンくず、`Values from:` の combo、`Refresh`
([構築](../../src/devstudio/view/graphcanvaswidget.cpp#L2143))、本体は左がキャンバス、右が
`Field` / `Value` の詳細表です。node は見出し帯 + 出力欄 + 入力欄の 3 段で、見出しの三角はソケット欄を
折り畳みます(折り畳むと辺は見出し帯の左右中央 1 点へ集まります)。編集はすべて**即時**で Apply ボタンがなく、
ソケット間ドラッグ = `set_input`、辺の `Delete` = `set_input{null}`、値の帯のその場編集 =
`set_node_parameters`、見出しの double-click = `set_node_label`、右クリックの palette = `add_node`、
node の `Delete` = `remove_node` です。位置と折り畳みは `(scope, node id)` を鍵にした**セッション内の状態**で、
文書には書かず、revision で消えた node の分は捨てられます。
engine へ出せるのは [`GraphCanvasReadCapability`](../../src/devstudio/view/graphcanvasreadcapability.hpp#L31) が
名前で持つ 5 つ(`get_graph_instances` / `get_graph_catalog` / `get_graph_values` / `edit_graph_document` /
`get_edit_result`)だけで、それ以外は `The graph canvas cannot issue %1.` になります
([分配](../../src/devstudio/view/graphcanvaswidget.cpp#L2423))。規範は struct の宣言そのもの
([メンバ 5 本](../../src/devstudio/view/graphcanvasreadcapability.hpp#L40))で、その上の宣言コメントは
**読みを 3 つと数えたまま**です —— メンバは `requestGraphInstances` / `requestGraphCatalog` /
`requestGraphValues` / `requestEditResult` の 4 読み + `requestEditGraphDocument` の 1 編集なので、
数えるならコメントではなくメンバを数えてください。コメントのうち今も正しいのは後半です。

> ... deliberately cannot express a generic RPC method, so nothing else can leave through
> this seam. The edit is edit_graph_document alone: the canvas cannot attach an instance,
> fork a preset or touch a pass from here.

[`RenderFeaturesWidget`](../../src/devstudio/view/renderfeatureswidget.hpp#L17) は render-tools の 2 枚目で、
`EmbeddedViewport *` を直接受け取る(capability struct を通さない)側です。扱うのは feature の追加・削除、
feature パラメータ(**Parameters...**)、materialize(**Make editable...**)、preset の fork、region replacement、
compute / RT task、callable record、scene acceleration structure、frame node order、procedural asset で、
ボタンの構築は [1 箇所](../../src/devstudio/view/renderfeatureswidget.cpp#L1847)に並んでいます。
dialog は `pelican.featureMaterialize`(+ `.destination`)と `pelican.featureParameters`(+ `.<name>`)です。
**export / import と package の UI はここにありません** —— 対応する RPC(§7.7)は engine 側にだけあります。

[`LayoutPresetManager`](../../src/devstudio/layoutpreset.hpp#L26) は view から独立した Qt Core の
ライブラリです。ファイル版と `QMainWindow` state 版をともに現行値へ固定し、版違い、破損、Qt に
よる state 拒否のどれでも saved state を適用せず既定配置 callback へ落とします。全体 preset は
`QStandardPaths::AppConfigLocation/layouts` に置き、project 単位の `user://` resolver には触れません。
保存・復元・fallback と、6 dock の頃に保存した state を **7 dock 構成**(Pass Editor が増えた後)で読めることは
`test/devstudio_layoutpreset_test.cpp` の TEST_CASE
[`Saved devstudio layouts remain restorable as later docks are added`](../../test/devstudio_layoutpreset_test.cpp#L117)
が画面表示なしで検査します。新 dock 追加のたびに state 版を上げないため、保存時に存在しなかった
stable object name は Qt が既存 dock の復元とは独立に扱い、既定配置に残ります。

リンク面では [`pelican_assert_link_boundary()`](../../cmake/devstudio_link_boundary.cmake#L72) が
`pelican_project` の直接リンクを必須にし、`src/core` 配下 target への推移 link path を configure
時に拒否します。`test/run_devstudio_boundary_negative.cmake` が実装する ctest
[`devstudio_d0_boundary_negative`](../../test/run_devstudio_boundary_negative.cmake#L1)
は wrapper target 経由の `pelican_core` も拒否されることを固定します。これは「Studio だけが使える
engine 内部面」を偶然持ち込めないようにする D0 の build-level gate です。

[`ProjectOutlinerModel`](../../src/devstudio/model/project.hpp#L44) は view と Qt から独立した読み取り専用
model です。`pelican_project` の project envelope、純粋 path resolver、sceneformat 検証を使って
project と scene 文書を開き、scene と object の木を作ります。object の同一性は名前ではなく
`(scene_id, declaration_index)` で、無名 object の表示名だけを engine と共有する
`pelican://scene/<id>/authoring-object/<n>` 規則から作ります。

[`MainWindow::populateOutliner()`](../../src/devstudio/view/mainwindow.cpp#L926) は model の索引を Qt item の
data role に保持して Outliner dock へ写すだけです。project 読み込みと 2 scene・46/2 object、無名
object の非圧縮、親子投影は [`devstudio_outliner_test.cpp`](../../test/devstudio_outliner_test.cpp#L120) が
GUI なしで検査します。RPC の `scene_tree` / `get_components` も 0 始まりの
`declaration_index` を返すため、直リンク木と RPC 木は `(scene_id, declaration_index)` で
対応付けられます。

[`SelectionModel`](../../src/devstudio/model/selection.hpp) は view/Qt から独立し、選択をこの公開
identity 1 個だけで保持します。viewport pick と Outliner のどちらも同じ instance を更新し、
`EmbeddedViewport` もコピーせず非所有参照します。遅い pick が新しい Outliner 選択を巻き戻さない
revision、背景での解除、feature/RPC 失敗時の選択維持を純粋 model で決めています。選択は engine の
共有 game state ではなく client-session の focus なので、Studio 専用 API は作りません。別ツールも
`pick_object` と同じ identity だけで同じ意味論を実装できます。

[`InspectorModel`](../../src/devstudio/model/inspectormodel.hpp) も view/Qt から独立した公開 RPC client
です。選択の `(scene_id, declaration_index)` を `scene_tree` の `authoring_object_id` へ解決してから
`get_components` を読み、component 応答の `schema.fields` だけから整数、浮動小数、真偽、列挙、文字列、
vector、quaternion の [`InspectorWidgetDescriptor`](../../src/devstudio/model/inspectormodel.hpp) を作ります。
component 名や transform の field 名を view 側で型判定しません。

数値操作は `open_preview` / `update_preview` / `commit_preview`、通常の確定操作は `edit` を使い、
`get_preview_result` / `get_edit_result` が terminal success を返すまで accepted を成功表示しません。
`undo` / `redo` も同じ actor と scene revision で送ります。watch token は idle 時だけ 500 ms ごとに
問い合わせ、preview 保持中、widget 編集中、または直接編集の terminal result 待ちでは refresh を保留します。既に飛んでいた
`get_components` 応答が編集中に戻った場合も捨て、解除後に取り直します。一方、idle 時の token 変更は
通常どおり再取得するため、巻き戻り防止と外部変更反映を分けています。これらの状態遷移は
[`devstudio_inspector_test.cpp`](../../test/devstudio_inspector_test.cpp) が GUI なしで検査します。

[`InspectorWidget`](../../src/devstudio/view/inspectorwidget.hpp) は descriptor の `kind` を Qt control へ
投影するだけで、engine 内部 symbol を使いません。同じ child process の RPC transport を
`EmbeddedViewport` から借りるため、別 editor runtime は作られません。windowed player は通常 loop で
毎 frame `updateFrameState()` を通るので、RPC queue に accepted された編集は次の player frame で適用され、
viewport の描画へ到達します。

Inspector の properties の下には [`GraphAttachmentWidget`](../../src/devstudio/view/graphattachmentwidget.hpp) が
載ります([置き場](../../src/devstudio/view/inspectorwidget.cpp#L214))。`Attached graphs` グループ
(list + **Attach...** / **Detach** / **Refresh**)と `Instance inputs` グループ(port ごとに mode / 著作値 /
実測値と 4 つのボタン)で、object name は `pelican.graphAttachments` /
`pelican.graphPort.<kind>.<port>` の規則です([構築](../../src/devstudio/view/graphattachmentwidget.cpp#L54))。
[`GraphAttachmentModel`](../../src/devstudio/model/graphattachmentmodel.hpp) が Qt 非依存の相方で、
mode は**文書から**決めます(edge があれば `connected`、なければ `parameter` か `default`)
([判定](../../src/devstudio/model/graphattachmentmodel.cpp#L327)) —— 展開結果から読まないのは、
接続された port には読み取れる node が残らないからです。
GUI の 1 操作は必ず 1 つの op 列 + 1 ticket + 1 CAS です([submitOps](../../src/devstudio/model/graphattachmentmodel.cpp#L464))。
「曲線で制御」が作る node の集合は [`curveControlOps()`](../../src/devstudio/model/graphattachmentmodel.cpp#L392) に
固定されていて、id は呼び手側で決まります —— **同じ ops を script が送れば同じ文書バイト列になる**ようにするためです。
ticket と generation の規律は [`design_graph_composition.md`](../design_graph_composition.md) が正です。

project を開くと viewport は同じ root を `--rpc --project` で再起動します。native child 上の
左クリックを物理 client 座標へ変換し、`EngineProcess` の stdio JSON-RPC から `pick_object` を
呼びます。成功は Outliner と Inspector に反映し、背景なら解除します。picking feature が無い場合は
選択を維持し、viewport/status/log に有効化方法を明示します。property 編集は Inspector から同じ
child の編集 RPC へ接続済みです。

[`GizmoModel`](../../src/devstudio/model/gizmomodel.hpp) は選択と toolbar の mode を `set_gizmo` へ送り、
Outliner の `(scene_id, declaration_index)` を `kind:declaration` の厳密なタグ付き selection に包みます。
左押下時の `query_gizmo_handle` 1 回だけで handle と engine 射影済み drag 契約を確定します。以後は
応答の `content_scale` で物理差分を論理差分へ直し、
`dot(logical_pointer_delta, drag_direction) * value_per_logical_pixel` の一式で pointer 移動を軸へ
拘束して Inspector と同じ preview lease へ値を渡します。release は commit、選択・mode
変更や player 停止は abort になり、Inspector の数値表示もドラッグ中に追従します。handle の外側は
従来の object pick へ戻ります。この Qt 非依存状態遷移は
[`devstudio_gizmo_test.cpp`](../../test/devstudio_gizmo_test.cpp) が headless に検査します。
同期、無名 object、stale 応答、背景、feature 無効の意味論は
[`devstudio_outliner_test.cpp`](../../test/devstudio_outliner_test.cpp) が headless に固定し、stdio の
応答/通常 log 分離は [`devstudio_viewport_test.cpp`](../../test/devstudio_viewport_test.cpp) が検査します。

### 自動化面: `--automation`(Q-1)

Studio に**外から命令を入れる口**がここで 1 つだけ増えました。`--automation <名前>`
([引数](../../src/devstudio/main.cpp#L16))が付いたときだけ、[`uimain()`](../../src/devstudio/view/uimain.cpp#L24) が
[`AutomationServer`](../../src/devstudio/automation/automationserver.hpp#L36) を作り、`QLocalServer` を listen して
`automation listening: <endpoint>` を stdout に出します(listen 失敗は **exit 3**)。**flag が無ければ server object が
存在しない**ので socket も無く、出荷の起動経路は 1 命令も通りません。window より**先に**開くのは、driver が
繋いでから engine の起動を待てるようにするためです。

library は [`src/devstudio/automation/`](../../src/devstudio/automation/CMakeLists.txt#L8) の 2 対
(`automationserver` / `automationcommands`)で、`Qt6::Network` と **`Qt6::Test`**(イベント合成のためだけ。
Catch2 / QTest の test binary ではありません)を link します。window への手は
[`AutomationHost`](../../src/devstudio/automation/automationserver.hpp#L20) —— `QWidget *window`、
`EmbeddedViewport *viewport`、`open_project` の 3 つを**渡される**形なので、この library は view library を
link せず、link の輪ができません。

protocol は**改行区切り JSON 1 行 = 1 命令**で、`{id, cmd, ...}` に `{id, ok, result}` か
`{id, ok:false, error:{code, message}}` を返します([組み立て](../../src/devstudio/automation/automationserver.cpp#L18))。
命令は 20 個([一覧](../../src/devstudio/automation/automationcommands.cpp#L1843))、拒否は 10 種の名前付きコード
([定数](../../src/devstudio/automation/automationcommands.cpp#L166))。`quit` は**答えてから**イベントループを畳み
([順序](../../src/devstudio/automation/automationserver.cpp#L110))、`rpc` と `wait` 系は入れ子のイベントループを
回すので、実行中に届いた次の行は `automation.busy@1` で断ります
([逐次](../../src/devstudio/automation/automationserver.cpp#L102)) —— 割り込ませて状態を混ぜるより、正直に断る側です。
指す名前は widget の `objectName` か、キャンバスの中に描かれたものの合成名
`<view>::<kind>#<node>[/<port>]`([生成](../../src/devstudio/automation/automationcommands.cpp#L205))で、`list` が
両方を publish します。`type` / `key` / `text` は `QGraphicsProxyWidget` が抱える埋め込み widget へ直接届くので、
**どの窓が activate されているかに依存しません**([理由](../../src/devstudio/automation/automationcommands.cpp#L362))。
`text` は描かれた字ではなく item が data role に入れた値を優先します
([優先](../../src/devstudio/automation/automationcommands.cpp#L378))。

driver は [`tools/studio_drive.py`](../../tools/studio_drive.py#L982)(stdlib のみ)。シナリオ JSON の step を順に送り、
`rpc_equals` / `text_equals` / `visible` / `list_count` / `screenshot` の期待を検査し、`verdict` / `summary` /
`failures` / `steps` / `studio_log` を持つ `report.json` を書きます([組み立て](../../tools/studio_drive.py#L1136))。
最後の `shutdown` step で終了コード 0 と強制終了されていないことを検査します。異常終了や終了待ちのタイムアウトは
失敗になり、`studio_exit_code` / `studio_shutdown_forced` と `failures` に結果を記録します。
書式の正本は [`test/studio_scenarios/README.md`](../../test/studio_scenarios/README.md#L1) です。

**Q-1b が足したのは 7 命令**(gesture 系の `wheel` / `move` / `press` / `release` と、
`action` / `menu_list` / `view_state`)で、`drag` は `button` を取るようになりました。
`press` / `move` / `release` は「押している button」を面の状態として共有し、ポインタ系はすべて scroll area の
**viewport** へ配送されます(frame 宛ては `QAbstractScrollArea` が捨てるため、Q-1b 以前は view の objectName を
指した gesture が無音で消えていました)。`key` の `route`(`widget` 既定 / `window`)は届け先を分け、
shortcut map を通る `window` だけが `QAction` を shortcut で発火できます。`list` の各項目は `shortcut` を持ち、
`QAction` 自体は widget ではないので `menu_list`(一覧)と `action`(発火)が別命令です —— どちらも popup の
modal ループに入りません。メニュー・dock・gizmo の action は `pelican.file.*` / `pelican.view.panels.*` /
`pelican.gizmo.*` の objectName で指します。

### Studio のテストは何を見ているか

登録の条件は「Studio の static lib が target として存在するか」で、**囲いは 1 つではありません** ——
下の表のテストは lib ごとの 7 つの `if()` に分かれて登録されます
([layout](../../test/CMakeLists.txt#L329) / [outliner](../../test/CMakeLists.txt#L340) /
[inspector model](../../test/CMakeLists.txt#L376) / [gizmo model](../../test/CMakeLists.txt#L389) /
[frame plan](../../test/CMakeLists.txt#L437) / [viewport + frame plan](../../test/CMakeLists.txt#L447) /
[viewport](../../test/CMakeLists.txt#L2332))。`SKIP_DEVSTUDIO=ON` の build ではこれらの lib が作られないので、
**そのどれも登録されません** —— 緑でも走っていない、という形になるので、Studio を触った変更は
Studio 入りの build で全数を見ます。**例外が 1 つ**あります: `devstudio_d0_boundary_negative` は
どの囲いの中にもいない素の [`add_test()`](../../test/CMakeLists.txt#L263) なので、`SKIP_DEVSTUDIO=ON` でも
存在します(そもそも「Studio が `src/core` へリンクできないこと」を configure 時に確かめる gate で、
Studio の target を必要としません)。

| テスト | 何を固定するか |
|---|---|
| `devstudio_viewport_test` | stdio の応答 / 通常 log の分離、`EngineLogBuffer` の上限 |
| `devstudio_outliner_test` / `devstudio_inspector_test` / `devstudio_gizmo_test` | 選択の identity、schema からの widget、preview lease の状態遷移(いずれも Qt 非依存 model) |
| `devstudio_layoutpreset_test` | 版・破損・Qt の拒否からの fallback、古い dock state の読み |
| `devstudio_frameplan_test` / `devstudio_frameplan_graph_test`(2,133 assertion)/ `devstudio_fullscreen_pass_test`(803 assertion) | Frame Plan の投影とノードキャンバス、pass 著作フォーム |
| `devstudio_graphattachment_test` | 装着・instance 入力の mode・4 導線が作る op 列 |
| `devstudio_graphcanvas_test`(139 assertion) | `GraphCanvasModel`。**対照**は「reply から `definitions` を落とす」「catalog を返さない」「値の overlay を撤去する」の 3 変異 |
| `devstudio_graphcanvas_scene_test`(62 assertion、`QT_OFFSCREEN`) | `GraphCanvasScene`。**対照**は「column を固定にする」「位置保持を撤去」「消えた node の位置を prune しない」「double-click を撤去」。N-2 で 8 case が加わり、その対照は「`canConnect` を常に true」「折り畳みを無効化」「parameters の全 key を送る」 |
| `devstudio_engine_failure_test`(`GPU` + `GOLDEN` フラグ。ctest の label は `gpu` **だけ**で、`GOLDEN` が足すのは [`RESOURCE_LOCK pelican_gpu`](../../test/CMakeLists.txt#L177) です) | **失敗経路で実 `pelican_player` を起動する Studio テスト**。sandbox に複製した `projects/example` を壊して起動し、fatal 行が Studio に届くこと・RPC の stdout が枠を保つことを見ます。**実 player を起こす唯一のテストではありません** —— Q-1 のシナリオ群のほか、`if(NOT SKIP_DEVSTUDIO)` の中で Studio の引数 probe と実 player を回す [`vulkan_validation_runtime_player`](../../test/CMakeLists.txt#L2890) が Q-1 以前からいます |
| `devstudio_d0_boundary_negative` | link boundary(下記) |
| `studio_scenario_*`(`gpu`、Q-1 / Q-1b / N-5。**10 本**: `graph_read` / `graph_edit_roundtrip` / `canvas_zoom_pan` / `menu_actions` / `graph_canvas_zoom_pan` / `theme_switch` / `graph_auto_layout` / `graph_reroute_tie` / `graph_observe_roundtrip` / `frameplan_edit_roundtrip`) | **実物の窓 + 実物の engine の e2e**。`--automation` 無しで socket が無いこと、`list` が objectName を返すこと、無い名前が `automation.widget_missing@1` になること、キャンバスが描いたものと `get_graph_instances` の一致、値の帯の click → type → Enter が engine に届き、**ディスク経由で再読込しても残る**こと。**対照**は「期待するノード数を 1 ずらす」と「`setNodeParameter` を止める」(後者は `wait_timeout`)。Q-1b の 3 本は gesture(wheel / 押したままの move / 中ボタン drag)を `view_state` の数値の**向き**で、メニューを `menu_list` / `action` で確かめます。N-5 の `theme_switch` は View ▸ Theme で light / dark を切り替えて両方を撮ります |

表の assertion 数(2,133 / 803 / 139 / 62)と各行の**対照**は、いずれも当該 WP の完了記録による報告値です
(N-1 = `edafc41d`、N-2 = `f2eccbfd`)。本章で走らせ直して数えたものではありません。

ここから、閉じた穴と残っている穴が読めます。

- **Studio は `get_status` を呼びません**(`src/devstudio` に呼び出しが 1 つもありません)。AG-3a が足した
  `get_status.animation` を Studio の画面で見る経路はまだありません(AG-3b)。
- **authoring の e2e は Q-1 で 1 本通りました。**`studio_scenario_graph_edit_roundtrip` が、実物の窓の値の帯を
  click → type → Enter して engine に届かせ、別 project へ切り替えて戻る(= engine がディスクから読み直す)ところまで
  1 本で通します。**それ以外の authoring 側 Studio テストは今も canned reply のハーネスと mock の往復**で、
  装着(Inspector)側の e2e はまだありません。
- **シナリオは GPU lane に登録します。**[scenario の登録条件](../../test/CMakeLists.txt#L2938)は
  Studio target と `PELICAN_PYTHON_TESTS=ON`、またはPython 3.11以上が見つかった `AUTO` です。
  `uv` は不要です。`test/studio_scenarios/*.json` から CTest を作り、完全な `run_gpu_gate.py` は
  全scenarioの正確な名前を必須にします。未登録・未実行・skipを成功とは扱いません。
  Studio無しの限定確認には `--engine-only` があり、release判定とは区別します。
- **絵は比較していません。**スクリーンショットは保存と目視(AI と人)のためで、hash 比較はありません。
  `widget` モードの絵に埋め込み engine の窓は写らず、`mode: screen` は記録上**未検証**です。

### 検証の穴(記録)

- **P-2a-3 / P-2a-4a(`7e983ed9`)は `SKIP_DEVSTUDIO=ON` の worktree で実装されたため、Features dock は
  未検証のまま着地しました**(完了記録 `7e983ed9` による。本章で再現したわけではありません)。engine 側の
  RPC は非 GPU / GPU 両方で見ていますが、dock の側は別 session の Studio 入り build を待ちました。
- **`test/fixtures/devstudio/example_frame_plan.json` は再凍結されていません**(P-2b `e1c326c1` の記録)。
  production 経路で取り直した plan は resource 22 → 23(`bloom.intensity`)、arena 89 → 90、decision 131 → 133 です。
  追随が要る数値は、現物を数えると `devstudio_frameplan_test.cpp` の **9 箇所**
  ([L476](../../test/devstudio_frameplan_test.cpp#L476) / [L588](../../test/devstudio_frameplan_test.cpp#L588) /
  [L686](../../test/devstudio_frameplan_test.cpp#L686) / L689 / L722–L724 / L793 / L841)と
  `devstudio_frameplan_graph_test.cpp` の **2 箇所**
  ([Target combo の項目数の検査(F-7 から Whole frame の 1 件を足して 23)](../../test/devstudio_frameplan_graph_test.cpp#L5144) /
  [L5896](../../test/devstudio_frameplan_graph_test.cpp#L5831))で、
  多い方は graph test ではなく frameplan test です。同じ commit で `devstudio_*` の 4 テストが
  **読みだけで編集され、実行されていない**ことも同じ完了記録 `e1c326c1` の申告で(Studio 無しの
  worktree では走らないため、primary の全数で見る、という運用です)、本章で実行を確かめてはいません。

> **設計決定:** **エンジン側の編集面 — 編集系 RPC 60 メソッド(§7.7)と ImGui inspector / asset browser(§7.12) — が正準です**。Qt Inspector も別の編集実装を持たず、その RPC を呼ぶ公開 client です。ImGui は同じ [`EditorCommandService`](../../src/core/communication/editorcommandservice.hpp#L282) を process 内 adapter から呼びますが、Studio は `pelican_project` と JSON-RPC だけへリンクする D0 境界を保ちます。

## 7.7 JSON-RPC を2層に分けて読む

RPC は build option `PELICAN_WITH_RPC` で切り替わります。無効時は stub が「未対応」を明示します。

有効時の起動形態は **2 つ**になりました(WP156 / E-HOST0)。`--rpc` はもう `--headless` を要求しません。

| 形態 | 起動 | 動作 |
|---|---|---|
| headless RPC | `--rpc --headless` | `Loop` が通常 loop の代わりに [`runEngineRpcServer(std::cin, std::cout)`](../../src/core/communication/rpcserver.hpp#L181) を実行し、EOF までブロックする |
| windowed RPC | `--rpc`(headless なし) | 通常の描画 loop を回しつつ、[`WindowedRpcHost`](../../src/core/communication/rpcserver.hpp#L158) が **フレーム境界でのみ** リクエストを処理する |

詳細な frame 境界の位置は [第2章](02_runtime_lifecycle.md)を参照してください。windowed RPC では ImGui UI が無効になる点は §7.12 と [第9章](09_black_magic_and_gotchas.md)で扱います。

### protocol 層: `pelican_project`

[`jsonrpc.hpp`](../../src/project/jsonrpc.hpp#L13) と [`jsonrpc.cpp`](../../src/project/jsonrpc.cpp#L148) は engine module を知りません。

- JSON-RPC 2.0 request の parse。
- `-32700` parse error、`-32600` invalid request、`-32601` method not found、`-32602` invalid params、`-32000` application error、`-32010` RenderDoc capture error。
- success/error response の serialize。
- injected input/event parameter の純粋 parse。

この層は Vulkan なしで [`jsonrpc_test.cpp`](../../test/jsonrpc_test.cpp#L33) から直接テストできます。

### engine binding 層: `pelican_core`

[`RpcServer`](../../src/core/communication/rpcserver.hpp#L55) は `istream` / `ostream` と method handler map を持ちます。1行を1 request とし、[`handleLine()`](../../src/core/communication/rpcserver.cpp#L1083) で parse → handler → response serialize を行います。[`run()`](../../src/core/communication/rpcserver.cpp#L1077) は EOF まで1行ずつ読み、必ず1行の response を flush します。1行だけを処理する [`processLine()`](../../src/core/communication/rpcserver.cpp#L1008) が公開されているのが windowed 経路の土台です。

transport が socket ではなく stream interface なのがポイントです。production 起動では stdin/stdout、unit test では stringstream を差し替えられます。

その上に 2 つの class が乗ります。

| class | 役割 |
|---|---|
| [`EngineRpcEndpoint`](../../src/core/communication/rpcserver.hpp#L140) | 「1 個の状態付きエンジン RPC ディスパッチャを所有」。headless は `run()`、windowed host は frame 境界でだけ `processLine()` を呼ぶ |
| [`WindowedRpcHost`](../../src/core/communication/rpcserver.hpp#L158) | `processFrameBoundary()` / `queuedRequestCount()` / `readerFinished()`。reader スレッドは行を **queue へ積むだけ**で、dispatch は engine スレッドが行う |

キュー容量は [`defaultWindowedRpcQueueCapacity = 64`](../../src/core/communication/rpcserver.hpp#L179) です。溢れたリクエストには reader スレッドが即座に `-32000` を返します(`data.reason == "busy"`)。

[`JsonRpcHandlerError`](../../src/core/communication/rpcserver.hpp#L37) には構造化 `data` が付きました([3 引数コンストラクタ](../../src/core/communication/rpcserver.hpp#L44)、取得は [`data()`](../../src/core/communication/rpcserver.hpp#L49))。`capture_gpu` 失敗時の実例です([`EngineRpcEndpoint::EngineRpcEndpoint()`](../../src/core/communication/rpcserver.cpp#L2071))。

```cpp
throw JsonRpcHandlerError{
    JsonRpcErrorCodes::renderDocCaptureError,
    "capture_gpu failed: " + std::string{error.what()},
    {{"reason", error.reason()},
     {"state", renderDocCaptureStateName(state.state)},
     {"source", "rpc"}}};
```

engine method の登録は [`configureEngineRpcHandlers()`](../../src/core/communication/rpcserver.cpp#L1300) と、そこから呼ぶ [`configureEditorRpcHandlers()`](../../src/core/communication/editorrpchandlers.cpp#L66) に集約されています。現在 **89 メソッド**で、実行制御・診断系が 29、編集系が 60 です。数え方は、両関数(および前者が呼ぶ 2 つの補助関数 [`configureGameLogicReloadRpcHandler()`](../../src/core/communication/rpcserver.cpp#L1315) と [`configurePhysicsRpcHandlers()`](../../src/core/communication/rpcserver.cpp#L1090))の中で `server.setHandler()` **または `server.setOrderedHandler()`** に渡す一意な公開 method 文字列を 1 メソッドとします。後者は `nlohmann::ordered_json` を返す版([宣言](../../src/core/communication/rpcserver.hpp#L97))で、使っているのは編集系の 3 つ(`get_components` / `get_ray_occurrences` / `resolve_ray_occurrence`)だけです —— **`setHandler` だけを数えると 86 本にしかなりません**。表で `start` / `stop` のように同じ行へ併記した名前も、それぞれ 1 メソッドです。条件コンパイルでの出し分けはこの 2 関数の中にはありません。

#### 実行制御・診断系(29)

| method | 実装行 | 状態変更 |
|---|---|---|
| `reload_game_logic` | [再ロード](../../src/core/communication/rpcserver.cpp#L1149) | game DLL(`pelican_game_logic`)を再ロード |
| `get_status` | [状態取得](../../src/core/communication/rpcserver.cpp#L1369) | instance、project、scene、frame/time、seed、store 状態と各種診断を返す |
| `get_gpu_timing` | [GPU 時間平均](../../src/core/communication/rpcserver.cpp#L1511) | 直近 30 フレームの node 別 `body_ms` / `barriers_ms` 平均と対応状況を軽量に返す |
| `set_seed` | [seed 設定](../../src/core/communication/rpcserver.cpp#L1570) | deterministic RNG を reseed |
| `set_input_profile` | [profile 切替](../../src/core/communication/rpcserver.cpp#L1578) | input profile を切替 |
| `inject_input` | [入力注入](../../src/core/communication/rpcserver.cpp) | canonical input queue へ key/mouse/axis event を積む |
| `start_input_record` / `stop_input_record` | [記録開始](../../src/core/communication/rpcserver.cpp#L1602) / [記録終了](../../src/core/communication/rpcserver.cpp#L1614) | 入力記録の開始/終了 |
| `start_input_replay` / `stop_input_replay` | [再生開始](../../src/core/communication/rpcserver.cpp#L1626) / [再生終了](../../src/core/communication/rpcserver.cpp#L1650) | 入力再生の開始/終了。開始側は再生だけでなく `EngineTime` を記録時 fps の fixed step へ切り替え、reload gate を閉じ、preview lease を強制 abort します |
| `inject_event` | [event 注入](../../src/core/communication/rpcserver.cpp) | 名前から登録済み event layer へ JSON payload を積む |
| `set_time` | [時刻設定](../../src/core/communication/rpcserver.cpp#L1638) | time を直接設定。frame index は進めない |
| `update_transforms` | [pending 積み](../../src/core/communication/rpcserver.cpp#L1705) | update を pending queue へ積む |
| `load_gltf` | [glTF 追加](../../src/core/communication/rpcserver.cpp) | transient glTF を scene に追加 |
| `load_scene` | [scene 差替](../../src/core/communication/rpcserver.cpp#L1725) | scene を clear/load、pending transforms を破棄 |
| `set_camera` | [camera 指定](../../src/core/communication/rpcserver.cpp#L1719) | 名前付き object を active camera にする |
| `step_frame` | [1 tick 進行](../../src/core/communication/rpcserver.cpp#L1704) | pending flush → time advance → 5 phase update → render |
| `render_frame` | [再描画](../../src/core/communication/rpcserver.cpp#L1755) | time/frame を進めず、pending flush → seq update → render |
| `capture_gpu` | [GPU 捕捉](../../src/core/communication/rpcserver.cpp) | `render_frame` と同型の1回描画を明示 Start/End で capture し、新規 index の `.rdc` path を返す |
| `get_frame_plan` | [plan 取得](../../src/core/communication/rpcserver.cpp#L1755) | planner の JSON を返す |
| `pick_object` | [ID 読み出し](../../src/core/communication/rpcserver.cpp) | feature が作った `picking_id` の左上原点座標を同期読み出しし、同一フレーム token を WP258 の宣言 identity へ解決 |
| `set_gizmo` | [表示要求](../../src/core/communication/rpcserver.cpp) | 宣言または runtime object のタグ付き selection と mode をギズモ表示へ設定。`selection:null` で解除 |
| `query_gizmo_drag_basis` | [ドラッグ基底](../../src/core/communication/rpcserver.cpp#L1828) | selection / mode / axis から、軸または view-plane の論理 pixel 当たりドラッグ基底を状態変更なしで返す |
| `get_modal_transform` | [modal 状態](../../src/core/communication/rpcserver.cpp#L1914) | player 側 modal transform の現在 snapshot を返す |
| `cancel_modal_transform` | [modal cancel](../../src/core/communication/rpcserver.cpp#L1926) | player 側 modal transform に cancel を要求する |
| `ack_modal_transform` | [modal ack](../../src/core/communication/rpcserver.cpp#L1950) | operation / revision が一致する modal transform snapshot を acknowledge する |
| `query_gizmo_handle` | [状態なし hit query](../../src/core/communication/rpcserver.cpp) | リクエスト自身の selection/mode/物理 pixel を共通投影幾何へ渡し、contract 2 の `content_scale` と、handle に ID/axis/射影方向/論理 px 当たり変化量または `null` を返す |
| `raycast` | [ray 問い合わせ](../../src/core/communication/rpcserver.cpp#L1091) | `origin` / `direction` / `max_distance` の 3 鍵ちょうどで `PhysWorld` に問い合わせる。登録は `configureEngineRpcHandlers()` が呼ぶ `configurePhysicsRpcHandlers()` の側 |
| `capture` | [`EngineRpcEndpoint::run()`](../../src/core/communication/rpcserver.cpp#L2081) | 最後の frame を PNG 保存 |

二つのギズモ RPC は [`engine://features/gizmo.json`](../../src/core/resources/features/gizmo.json) が
active graph に無ければ application error です。`parseGizmoSelection()` は object のキーを
`kind:declaration + scene_id + declaration_index` または
`kind:runtime + object_id:{index,generation}` のどちらか一つへ限定します。混在、どちらでもない形、
死んだ runtime ID は `error.data.code` にそれぞれ名前を持つ `-32602` で、一方を優先しません。
`query_gizmo_handle` は `Gizmo::displayRequest()` を読まず、指定 selection/mode から
[`buildGizmoGeometry()`](../../src/core/renderer/gizmo.cpp) を直接呼ぶため、問い合わせの前後で
active/drag 状態は生まれません。座標と `drag_direction` は output の左上原点、掴み代は 10 論理 px の
DPI 追従値です。応答は同じ計算で使った `content_scale` を明示し、client は掴み代から逆算しません。
正面退化した軸には drag 契約が無く `handle:null` を返します。

`set_seed` と replay は役割が別です。`set_seed` は `DeterministicRng` の種を撒き直すだけで、時間の刻みも入力も固定しません。再現可能な実行は「seed」「fixed step の時間」「記録済み入力」の3つが揃って初めて成立し、後ろ2つを与えるのが `start_input_replay` です。

#### 編集系(60) ✅実装済み(WP153〜WP172 で始まり、range はそこを大きく越えます)

すべて [`EditorCommandRpcAdapter`](../../src/core/communication/editorcommandservice.hpp#L400) へ委譲され、実体は [`EditorCommandService`](../../src/core/communication/editorcommandservice.hpp#L282) です。

| method | 実装行 | 概要 |
|---|---|---|
| `scene_tree` | [登録 `scene_tree`](../../src/core/communication/editorrpchandlers.cpp#L74) | オブジェクト木(`authoring_object_id` + 0 始まりの `declaration_index`)と component メタデータ |
| `get_scene_revision` | [登録 `get_scene_revision`](../../src/core/communication/editorrpchandlers.cpp#L77) | `EditorWatchToken{scene_revision, preview_epoch}` + 最終トランザクション + preview lease |
| `get_components` | [登録 `get_components`](../../src/core/communication/editorrpchandlers.cpp#L83) | 1 オブジェクトの `declaration_index` + authored / runtime JSON + schema |
| `list_assets` | [登録 `list_assets`](../../src/core/communication/editorrpchandlers.cpp#L92) | asset カタログ(`id` / `kind` / `path` / `store` / `status`) |
| `export_scene_snapshot` | [登録 `export_scene_snapshot`](../../src/core/communication/editorrpchandlers.cpp#L95) | semantic scene bytes + sha256 digest |
| `import_scene_snapshot` | [登録 `import_scene_snapshot`](../../src/core/communication/editorrpchandlers.cpp#L101) | digest 検証つき置換 |
| `save_scene` | [登録 `save_scene`](../../src/core/communication/editorrpchandlers.cpp#L112) | 原子的な全文書保存 |
| `open_editor_session` / `resume_editor_session` | [登録 `open_editor_session`](../../src/core/communication/editorrpchandlers.cpp#L126) / [登録 `resume_editor_session`](../../src/core/communication/editorrpchandlers.cpp#L129) | actor 登録・再接続 |
| `can_edit` / `can_preview` | [登録 `can_edit`](../../src/core/communication/editorrpchandlers.cpp#L132) / [preview 可否](../../src/core/communication/editorrpchandlers.cpp#L135) | 編集ゲート判定 |
| `eval_preview` | [登録 `eval_preview`](../../src/core/communication/editorrpchandlers.cpp#L138) | 公開せずリクエストローカルに評価 |
| `render_preview` | [preview 描画](../../src/core/communication/editorrpchandlers.cpp#L141) | preview グラフでキャプチャ(第6章 §6.19) |
| `edit` | [登録 `edit`](../../src/core/communication/editorrpchandlers.cpp#L144) | 正準コマンド列の適用(`base_revision` による CAS。ズレていれば `stale_revision` で弾きます) |
| `undo` / `redo` | [登録 `undo`](../../src/core/communication/editorrpchandlers.cpp#L162) / [登録 `redo`](../../src/core/communication/editorrpchandlers.cpp#L165) | actor 単位 |
| `open_preview` / `update_preview` / `commit_preview` / `abort_preview` | [lease 発行](../../src/core/communication/editorrpchandlers.cpp#L168) 〜 [lease 破棄](../../src/core/communication/editorrpchandlers.cpp#L177) | preview ticket(lease)の発行・更新・確定・破棄 |
| `get_render_features` | [feature 構成取得](../../src/core/communication/editorrpchandlers.cpp#L180) | authored feature 構成と編集 token を返す |
| `list_render_features` | [feature catalog](../../src/core/communication/editorrpchandlers.cpp#L183) | 追加可能な render feature の catalog を返す |
| `edit_render_features` | [feature 編集](../../src/core/communication/editorrpchandlers.cpp#L198) | render feature 構成を編集する |
| `get_scene_acceleration_structures` / `edit_scene_acceleration_structures` | 名前付きASの取得・編集 | AS定義とdigestを取得し、既存render config ticketで変更・保存する |
| `get_render_authoring_context` | [authoring context](../../src/core/communication/editorrpchandlers.cpp#L201) | authored pass 編集に必要な target / resource context を返す |
| `add_authored_pass` | [pass 追加](../../src/core/communication/editorrpchandlers.cpp#L270) | authored pass を追加する |
| `remove_authored_pass` | [pass 削除](../../src/core/communication/editorrpchandlers.cpp#L273) | authored pass を削除する |
| `get_edit_result` / `get_preview_result` | [edit 結果](../../src/core/communication/editorrpchandlers.cpp#L279) / [登録 `get_preview_result`](../../src/core/communication/editorrpchandlers.cpp#L282) | 非同期結果取得 |
| `query_journal` | [登録 `query_journal`](../../src/core/communication/editorrpchandlers.cpp#L285) | ジャーナル照会 |
| `get_graph_instances` | [instance 一覧](../../src/core/communication/editorrpchandlers.cpp#L225) | 共通グラフ文書の `source_digest` / `instances[]` / root の `nodes[]` / `definitions` 本体 |
| `edit_graph_instances` | [装着編集](../../src/core/communication/editorrpchandlers.cpp#L228) | owner 付き root instance の attach / detach。中では `editGraphDocument()` の ops へ翻訳 |
| `edit_graph_document` | [文書編集](../../src/core/communication/editorrpchandlers.cpp#L231) | `ops[]` を順に当てて最後に 1 回展開。全部通るか文書ごと拒否か |
| `get_graph_values` | [instance 入力の値](../../src/core/communication/editorrpchandlers.cpp#L234) | publish 済み snapshot の読み。再評価しない |
| `get_graph_node_values` | [node 出力の値](../../src/core/communication/editorrpchandlers.cpp#L237) | 展開木の node ごとの出力。`source: "rendering"` |
| `get_graph_catalog` | [module catalog](../../src/core/communication/editorrpchandlers.cpp#L240) | `builtinGraphModuleRegistry().catalog()` をそのまま(§7.5c と同じ値) |
| `export_render_feature` | [部品の書き出し](../../src/core/communication/editorrpchandlers.cpp#L186) | feature 1 つを `user://components/<name>/` へ |
| `import_render_feature` | [部品の取り込み](../../src/core/communication/editorrpchandlers.cpp#L189) | project 外の component ディレクトリを未作成の `project://` feature として取り込む |
| `install_package` | [package の vendoring](../../src/core/communication/editorrpchandlers.cpp#L192) | `<project>/packages/<ns>/` へ複製し catalog を作り直す |
| `uninstall_package` | [package の削除](../../src/core/communication/editorrpchandlers.cpp#L195) | 参照が残っていれば `package_in_use` で何も消さない |
| `get_component_add_candidates` | [追加候補](../../src/core/communication/editorrpchandlers.cpp#L80) | `get_components` と同じ object 指定で、足せる component の候補を返す(manual §13.2) |
| `get_ray_occurrences` / `resolve_ray_occurrence` | [occurrence 一覧](../../src/core/communication/editorrpchandlers.cpp#L86) / [1 件の解決](../../src/core/communication/editorrpchandlers.cpp#L89) | RT の occurrence(行)の読み。どちらも `setOrderedHandler` 側 |
| `set_prefab_parameter` / `unset_prefab_parameter` / `unpack_prefab` | [上書き](../../src/core/communication/editorrpchandlers.cpp#L147) / [上書き解除](../../src/core/communication/editorrpchandlers.cpp#L152) / [展開](../../src/core/communication/editorrpchandlers.cpp#L157) | prefab インスタンスのパラメータ上書き・解除と、素のオブジェクトへの展開 |
| `fork_render_pipeline` | [preset の fork](../../src/core/communication/editorrpchandlers.cpp#L204) | 選択中 preset を project ファイルへ複製し、root の参照も切り替える |
| `edit_preset_pass` | [preset pass 編集](../../src/core/communication/editorrpchandlers.cpp#L207) | project preset の pass(fullscreen / material / raster)を部分編集する |
| `edit_region_replacement` | [領域の差し替え](../../src/core/communication/editorrpchandlers.cpp#L210) | project preset のタグ付き領域に provider を指定 / 解除する |
| `get_compute_task_authoring` / `edit_compute_task` / `add_compute_task` / `remove_compute_task` | [取得](../../src/core/communication/editorrpchandlers.cpp#L213) / [置換](../../src/core/communication/editorrpchandlers.cpp#L243) / [追加](../../src/core/communication/editorrpchandlers.cpp#L252) / [削除](../../src/core/communication/editorrpchandlers.cpp#L255) | compute / RT task 宣言の著作面 |
| `get_frame_node_order` / `edit_frame_node_order` | [順序の取得](../../src/core/communication/editorrpchandlers.cpp#L216) / [順序の保存](../../src/core/communication/editorrpchandlers.cpp#L219) | root の `frame_node_order`(手で決めた node 順)の読みと保存 |
| `get_procedural_assets` / `edit_procedural_asset` | [一覧](../../src/core/communication/editorrpchandlers.cpp#L246) / [編集](../../src/core/communication/editorrpchandlers.cpp#L249) | procedural asset の宣言。`edit` の op ではなく独立した RPC です |
| `get_ray_callable_records` / `edit_ray_callable_records` | [callable の読み](../../src/core/communication/editorrpchandlers.cpp#L258) / [初期値の置換](../../src/core/communication/editorrpchandlers.cpp#L261) | task ごとの callable record schema と初期値上書き |

上 11 行(19 メソッド)は P-2a 系・RT 系・prefab 系で足された分で、params と戻り値の正本は
[manual 第13章](../manual/13_editor.md)です。この表と `editorrpchandlers.cpp` の登録を突き合わせるときは、
行ではなくメソッド名を数えてください(併記した行が 1 行 2〜4 メソッドあります)。

#### 共通グラフの編集面(P-1 / N-2)

シーン編集と**同じ表に並んでいますが、実行モデルは render config 側**です。文書はディスク上の rendering
config そのもので、CAS は `SceneRevision` ではなく **文書全体の sha256**(`base_source_digest`)、
`actor_id` もセッションも journal も undo もありません。1 回の呼び出しが 1 つの op 列・1 チケット・1 CAS です。

`ops[]` は 5 種で、正は [`editGraphDocument()`](../../src/project/graphcomposition.cpp#L757) です。

| op | 形 |
|---|---|
| `add_node` | `{node: {id, op, ...}}` |
| `remove_node` | `{node: "<id>"}` |
| `set_input` | `{node, port, source: {node, port} \| null}` |
| `set_node_parameters` | `{node, parameters}`(**置換**。空 object は鍵ごと削除) |
| `set_node_label` | `{node, label}`(空文字で削除) |
| `set_layout` | `{layout: {nodes, reroutes, ties}}` か `{layout: null}`(scope の置き場所を丸ごと置換。再 compose 無し。N-6) |

N-2 で **全 op が任意の `scope` を取る**ようになりました
([解決](../../src/project/graphcomposition.cpp#L386))。既定は `"root"`、definition 名を書くとその本体が対象で、
`root` は予約語なので同名の definition は指せません。これに合わせて
`edit_graph_document` の書き戻しも 2 段になり、root の `nodes` は常に、`definitions` は
**変化したときだけ**書き戻します([判定](../../src/core/communication/renderconfigeditor.cpp#L2266)) ——
root だけの編集が definition のバイト列を動かさないようにするためです。

名前付きの拒否は `GraphRegistryError` が運びます: `graph.node_duplicate@1` / `graph.node_missing@1` /
`graph.input_unknown@1` / `graph.output_referenced@1` / `graph.input_referenced@1` /
`graph.nested_instance@1` / `graph.input_scope@1` / `graph.definition_immutable@1` / `graph.layout_invalid@1`(N-6、`set_layout` の検証)。N-2(`f2eccbfd`)が足したのは
**この並びの 5〜7 番目**、つまり `graph.input_referenced@1` / `graph.nested_instance@1` / `graph.input_scope@1` で、
末尾の `graph.definition_immutable@1` はそれより前(`3611e390`)からあります。N-2 の 3 つは
「instance は root に立ち、境界 port は definition の中にある」という片側ずつの所属を名前で言うためのものです
([分岐](../../src/project/graphcomposition.cpp#L788))。形が壊れているだけのものは名前を持たず
`invalid_graph_document` に落ちます。

利用者向けの手順と GUI の対応は [manual 第13章](../manual/13_editor.md) §13.5 が正です。

#### catalog の enumerator は throw してはならない(X-1)

`list_render_features` が返す各 entry には `source` が付きます。値は **`"engine"` と `"package"` の 2 つだけ**で、
`project://features/*.json` に手で置いた文書は catalog に載りません
([entry の定義](../../src/core/communication/renderconfigeditor.hpp#L31))。package 側の列挙は
[`enumeratePackageRenderFeatureDocuments()`](../../src/core/communication/renderconfigeditor.cpp#L678) で、
**`noexcept` で、`std::exception` の失敗を entry として報告します** —— `project.json` が読めなければ package は 0 個、
package の文書が壊れていれば `available: false` + `unavailable_reason` の行として並びます。ただし
**「全部の失敗が行になる」わけではありません**: `std::exception` を継承しない例外を捕まえる
[package の文書を読む `readPackageFeatureDocuments` の呼び出しを包む `catch (...)`](../../src/core/communication/renderconfigeditor.cpp#L695) は `continue` するだけなので、
その package は entry を 1 つも残さずに catalog から落ちます(記録した穴)。

これは行儀の問題ではありません。catalog は **editor service の構築時**に作られるので、ここで例外が抜けると
**player が起動しきらず、Studio がその project を開けなくなります**。X-1 の対照実験(enumerator を fatal に
戻す変異)では player が戻らず、**ctest が 1,560 秒で timeout しました**(完了記録 `64eae566`)。
「壊れたデータは名前付きの行になる。列挙そのものは失敗しない」が不変条件です。

> 🧩 **難所 — 曖昧な重なり判定**([`stablePathsOverlap()`](../../src/core/communication/editorjournal.cpp#L2584) / [`structuralDomainsOverlap()`](../../src/core/communication/editorjournal.cpp#L2655) / [`recordOverlaps()`](../../src/core/communication/editorjournal.cpp#L2749))
>
> **何をする所か**: 上の表のうち `edit` / `undo` / `redo` / `*_preview` が共通で踏む土台です。2つの編集(あるいは編集とpreview lease)が同じ対象を触っているかを、安定パス集合(`write_set`)と構造ドメイン(`structural_domain`)の2系統で判定します。
>
> **素朴に読むと**: 「同じ `object_id` なら衝突」で済みそうに見えます。しかし編集の影響範囲は3種類あって表現が違います — 値編集はパス(`/authoring_objects/7/components/transform/pos`)、構造編集は集合(部分木のobject群・宣言index群・名前予約)、親子付け替えは辺(child / old_parent / new_parent / descendants)。`stablePathsOverlap` が**接頭辞一致**を取り、しかも境界が `/` であることを明示的に確かめているのはこのためです。単なる `starts_with` にすると `/a/b` と `/a/bc` が衝突扱いになってundoが通らなくなり、逆に完全一致だけにすると `.../transform` の付け替えと `.../transform/pos` の値編集がすり抜けて静かにロストアップデート(lost update — 2つの更新が重なったとき、片方の変更が気づかれないまま上書きされて消えること)します。`structuralDomainsOverlap` が名前と宣言indexを**同一scene内でのみ**比較するのも同様で、sceneが違えば同名でも別物です。判定は意図的に保守側(疑わしきは衝突)に倒してあります。
>
> **骨子**:
> ```text
> recordOverlaps(record, writes, domains):
>   record.write_set × writes         で stablePathsOverlap が真 -> 衝突
>   record.structural_domain × domains で structuralDomainsOverlap が真 -> 衝突
> structuralDomainsOverlap(l, r):
>   object集合(object/root/child/old_parent/new_parent/objects/descendants)が交差 -> 真
>   同一 scene_id なら name_reservation と declaration_index の交差も見る
>   両方が component_slot|value_field で object と slot が同じ -> 真
> ```
>
> **手がかり**: [`domainObjects()`](../../src/core/communication/editorjournal.cpp#L2593) が拾うキー名の一覧は、**op の `kind` ごとに違うフィールド名の総和**です — `value_field` / `component_slot` は `object`、`object_existence`(spawn)はプリフライト後に埋め戻される `object`、`object_subtree`(destroy)は `root` と配列 `objects`、`parent_edge`(reparent)は `child` / `old_parent` / `new_parent` と配列 `descendants`。単数フィールドと配列フィールドを別扱いで読むので、新しいopを足すときにここへ追記するのは、**そのopが既存にない名前でobject IDを持つ場合だけ**です。粒度が意図的に不揃いな例として、spawnの `write_set` は `/scenes/<id>/objects` という**粗い**パス、`read_set` は `/scenes/<id>/name_reservations/<name>` という細かいパスです。behaviorは [`stableBehaviorTarget()`](../../src/core/communication/editorjournal.cpp#L526) がhandleがあれば `handles/<h>`、無ければ `indices/<i>` を使い分けます(indexは他の編集でずれるのでhandle優先)。テストは `test/editorjournal_test.cpp` の [`WP161 writer history supports multi-level undo and redo on one path`](../../test/editorjournal_test.cpp#L850) と、TEST_CASE `WP161 two-actor structural overlap makes the whole undo a no-op` の中の [二アクター重なりの事例](../../test/editorjournal_test.cpp#L954)。
>
> **不変条件**: 判定は保守側へ倒す(検出漏れは静かなロストアップデート、過検出は明示的な `undo_conflict` / `preview_lease_conflict` で済む)。パス比較は必ず `/` 境界を見る。

> **設計決定:** 編集セッションを production で組み立てるのは [`makeEditorRuntimeService()`](../../src/core/communication/editorruntimefactory.hpp#L144) の 1 箇所だけです。RPC endpoint と interactive ImGui runtime の **どちらか一方** が使い、決定的ドライバ(golden / replay)は interactive runtime を作らないため編集面自体が存在しません。ticket・CAS・ゲートの落とし穴は [第9章](09_black_magic_and_gotchas.md)を参照してください。

> 🧩 **難所 — `edit` の逐次プリフライト**([`prepareBatch()`](../../src/core/communication/editorjournal.cpp#L2029))
>
> **何をする所か**: `edit` の生 operation 配列を、正準forward / inverse・安定ターゲット・read/write set を持つ `PreparedOperation` 列へ変換します。その過程で各 operation を**使い捨ての文書**へ本物の `EditorProjectionTransaction` で実際に適用してみます。
>
> **素朴に読むと**: 「なぜ同じコマンドを二度実行するのか」が最大の壁です。理由は3つあります。(1) N番目のoperationは 0..N-1 適用後の文書を見なければ正しく準備できません(spawnした直後に同じobjectへsetする、など)。(2) [`makeReplaceComponentCommand()`](../../src/core/communication/editorjournal.cpp#L317) のようなコマンドのラムダは準備時の `object_index` / `component_index` を**値でキャプチャ**していて、実行時にその位置が別物なら `"stable component location disappeared"` を投げます — プリフライトはこの整合を受理前に確かめる場です。(3) spawnの `AuthoringObjectId` は文書側の割り当て器が決めるので、**プリフライトを通すまで確定しません**。だから `structural_changes` から `Insert` を探して `forward["object_id"]` と inverse の `object_ids` を後から埋め戻します。live文書を汚さないのは [`LocalDocumentTarget`](../../src/core/communication/editorjournal.cpp#L469) が `source.stage(source.rawJson(), revision+1)` でコピーを作るからで、adapter配列が空のままcommitしているのが「文書だけの試行」の印です。
>
> **骨子**:
> ```text
> local = LocalDocumentTarget(source)          # revision+1 の使い捨てコピー
> for raw in operations:
>     prepared = prepareRpcOperation(raw, local.projectionDocument(), scene)
>     result   = EditorProjectionTransaction(local, ...).commit(prepared.commands, {})
>     未commit -> projectionFailure(受理前に型付き失敗へ変換)
>     spawn   -> Insert から object_id を確定して埋め戻す
>     destroy -> removed_objects を declaration_index 昇順に並べて inverse.closures へ
> batch.inverse = reverse(各 prepared.inverse)
> ```
>
> **手がかり**: 並び順が2つ出てきますが、向きが違うのは**対象が forward と inverse で別だから**です。[`subtreeObjects()`](../../src/core/communication/editorjournalops.cpp#L1111) が並べるのは forward の削除命令列で、`object_index`(scene内の宣言index)の**降順** — 後ろから消さないと残りのindexがずれます。closuresが並ぶのは inverse(`restore_objects`)の再生順で、`declaration_index` の**昇順** — 前から挿し戻さないと同じ理由でずれます。「indexで位置を指すリストは末尾から消し、先頭から挿す」という一つの規則の表と裏で、逆向きに見えるのはそのためです。`normalizeBehaviorAttachmentIdentities()` は `prepareBatch` の**前**に走り、`base_revision+1` とcommand index / attachment index の三つ組からhandleとseqを採番します(同じ入力なら同じidentity)。テストは [`editorjournal_test.cpp` 内](../../test/editorjournal_test.cpp#L743)「JOURNAL0 is complete and mechanical replay is three-way equivalent」。
>
> **不変条件**: プリフライトはlive文書とliveランタイムに副作用を持たない。inverseは必ずforwardの逆順。spawnの `object_id` は必ずプリフライト結果から取る(自前で採番しない)。

> 🧩 **難所 — undo前提の三段検証**([`requireRevertPreconditions()`](../../src/core/communication/editorjournal.cpp#L3447))
>
> **何をする所か**: `undo` / `redo` を「逆命令の普通のトランザクション」として実行してよいかを、実行前に3つの独立した条件で検査します。
>
> **素朴に読むと**: undoは「巻き戻し」ではなく**前向きの新規トランザクション**です。対象のjournal recordを書いた後に誰かが同じ領域を触っていたら、逆命令を流すと他人の編集を消します。ところが検査が3つあり、それぞれ別のすり抜けを塞いでいるのが読みにくい箇所です。(1) journal走査 — `source` より後のrevisionで、かつ**別actor**のrecordが重なっていたら衝突(同一actorをスキップするのは、自分の連続編集を自分でundoできなくしないため)。(2) `last_writers` — write pathごとの最終書き手が、いま巻き戻そうとしている record の**トランザクションそのもの**(actorではなく `transaction_id` 一致)でなければ衝突。(1)がrecord単位なのに対しこちらは**パス単位の最終書き手**なので、同一actorが別ticketで上書きした場合も拾えます。(3) `postconditionsHold` — 実行時に採取した `forward_postcondition` と、いまの文書から再計算した `targetState` の厳密比較。ここだけが「journalに現れない経路(reload・import・save後)で文書が変わった」を検出できます。どれか一つでも落とすと、undoが他人の編集を静かに巻き戻します。
>
> **骨子**:
> ```text
> for candidate in journal:
>     candidate.revision <= source.revision または actor が同じ -> skip
>     recordOverlaps(candidate, source.write_set, source.structural_domain) -> undo_conflict
> for write in 各 command.write_set:
>     last_writers[write].transaction_id != source.transaction_id -> undo_conflict
> postconditionsHold(source, document()) が偽 -> undo_conflict
> ```
>
> **手がかり**: `throwUndoConflict()` のpayloadは `{domain, owner_txn, revision}` で、`domain` は「衝突した領域」を人が読める形で示すための欄です。渡す値は呼び出し側ごとに違い、(1)(3)は record 全体を代表させて `structural_domain` の**先頭要素**(空なら `write_set` 全体)、(2)は落ちた command 自身の `structural_domain` です。どれも「誰が何を触ったせいでundoできないか」をクライアントが出すための材料です。undo / redo スタックの先頭が対象トランザクションと一致するかの検査は別にあり、受理時([`enqueueRevert`](../../src/core/communication/editorjournal.cpp#L4416))と実行時([`commitPending`](../../src/core/communication/editorjournal.cpp#L4835))の**二重**になっています。テストはいずれも `test/editorjournal_test.cpp` で、[`WP161 actor undo and redo are ordinary atomic transactions`](../../test/editorjournal_test.cpp#L811) / [`WP161 writer history supports multi-level undo and redo on one path`](../../test/editorjournal_test.cpp#L850) / TEST_CASE `WP161 two-actor structural overlap makes the whole undo a no-op` の中の [二アクター重なりの事例](../../test/editorjournal_test.cpp#L954)。
>
> **不変条件**: 3検査はAND。順番は変えてよいが、どれも消してはいけない。undoが成功したら `undo_stack.pop_back()` と `redo_stack.push_back()` は必ず対で動かす(片方だけだとredoが別トランザクションを指します)。

> 🧩 **難所 — preview leaseの状態機械**([`commitPendingPreview()`](../../src/core/communication/editorjournal.cpp#L3714) / [`forceAbort()`](../../src/core/communication/editorjournal.cpp#L3669))
>
> **何をする所か**: `open_preview` / `update_preview` / `commit_preview` / `abort_preview` をフレーム境界でまとめて処理し、lease(排他権)の取得・維持・破棄と、外部要因による強制破棄を行います。
>
> **素朴に読むと**: 状態が `preview_reservation`(受理済み・未開通) / `preview_lease`(開通中) / `preview_tombstones`(終了済み)の3つに分かれ、遷移が「受理時」と「フレーム境界」の2箇所にまたがっています。予約が要るのは、`open_preview` を受理してから実際に開くまでの間に別actorの `open_preview` や重なる `edit` を通してはいけないからで、`conflictingLease()` はleaseとreservationの**両方**を見ます。失敗経路で予約を消し忘れるとpreviewが永久に開けなくなるため、明示的な `reset()` が各失敗経路に置かれています。tombstone(実体を消しても「このticketは確かに存在して終了した」という痕跡だけを残すレコード)は `abort_preview` の冪等性 — 同じ操作を何度行っても結果が変わらない性質 — のためで、既に終了したticketへのabortは「成功 + `final_status`」を返します。`forceAbort()` が `noexcept` で、leaseが無いときも失敗したときも `false` を返すだけなのは、ゲート閉鎖・シーン遷移・base revision陳腐化のいずれからも呼ばれるからです。呼び出し元の [`commitPending()`](../../src/core/communication/editorjournal.cpp#L4835) 自体が `noexcept` のフレーム境界フックとして登録されていて、ここから例外が漏れれば `std::terminate` でプロセスごと落ちます(「フレームの一部だけ失敗する」ではありません)。だから `forceAbort()` の唯一の逃げ道は `false` を返して次のフレーム境界に委ねることです。
>
> **骨子**:
> ```text
> commitPending():                                   # フレーム境界フック
>   commitPendingPreview()
>   lease あり かつ (ゲート閉 or scene が変わった) -> forceAbort(理由)
>   ticket ごとにゲートepoch / scene id / base revision を受理時の値と再照合
>   edit 成功後に lease が残っていれば forceAbort("base_revision_stale")
> commitPendingPreview():
>   open   : lease 占有中なら preview_lease_busy、成功で reservation -> lease、epoch++
>   update : 所有者一致 + write_set が完全一致(sameStableSet)でなければ拒否
>   commit : base revision 不一致なら reject して即 forceAbort("stale_revision")
>            一致すれば journal へ確定し、tombstone を置いて lease を落とす
>   abort  : ライブ状態を committed へ復元し、tombstone を置いて lease を落とす
> ```
>
> **手がかり**: [`gateSnapshot()`](../../src/core/communication/editorjournal.cpp#L3301) は観測値が変わったときだけ `gate_epoch` を進めます。つまりepochは「ゲートの状態が変わった回数」であって時刻ではなく、受理時epochと実行時epochの比較が「閉じて開き直した」ケースも捕まえます。[`sameStableSet()`](../../src/core/communication/editorjournal.cpp#L2835) が完全一致を要求するのでupdateでleaseの範囲を広げられません(広げられると、受理時に通した衝突判定の結論が後から嘘になります)。[`requireLivePreviewCapability()`](../../src/core/communication/editorjournal.cpp#L2819) は transform / light の `set_component_value` 以外を全部弾きます。テストは [`editorjournal_test.cpp` 内](../../test/editorjournal_test.cpp#L983)(lease matrix)と [開通後の runtime 検証](../../test/editorjournal_test.cpp#L1058)。
>
> **不変条件**: `forceAbort()` の内部でthrowさせない。予約は成功でも失敗でも必ず落とす。leaseの `write_set` はopenで確定しupdateで変えない。editがcommitしたらpreview leaseは必ず落とす。

> 🧩 **難所 — previewの状態不変性検証**([`isolated()`](../../src/core/communication/editorpreviewservice.cpp#L144) / [`requireStateUnchanged()`](../../src/core/communication/editorpreviewservice.cpp#L131))
>
> **何をする所か**: `eval_preview` / `render_preview` の前後で共有エンジン状態のスナップショットを取り、**厳密一致**しなければ `state_changed` を投げます。成功時も例外時も検査します。
>
> **素朴に読むと**: `before != after` の一行に見えますが、二点が効いています。第一に、catch側でも検査してから元の例外を再送出します。検査が落ちれば元の例外は**捨てられ** `state_changed` に置き換わります。これは意図的で、「リクエストが失敗した」より「エンジン状態を汚した」の方が重い障害だからです。ここを「元の例外を優先」に直すと、状態漏れが失敗の陰に隠れます。第二に、比較が寛容な近似ではなく `OrderedJson` の完全一致であることです。previewは [`prepareEditorPreviewProjection()`](../../src/core/loader/editorpreviewprojection.cpp#L740) が `stage()` した文書を**公開しない**ことで成立していて、浮動小数1ビットの差でも「どこかで公開してしまった」の証拠になります。
>
> **骨子**:
> ```text
> isolated(method, snapshot, invoke):
>     before = snapshot()
>     try:   result = invoke(); requireStateUnchanged(before, snapshot()); return result
>     catch: failure = current_exception()
>            requireStateUnchanged(before, snapshot())   # ここで throw したら元例外は失われる
>            rethrow(failure)
> ```
>
> **手がかり**: 両メソッドはゲートを**2回**消費します(受理時のepochを実行直前に照合するので、prepare中に閉じて開き直したゲートも捕まります)。`prepareEditorPreviewProjection()` のコメント「候補revisionを使うが決してpublishしない」が、この検査が守っている性質そのものです。`projection_fault_hook` / `execution_fault_hook` はprepareやqueryの途中で任意にthrowさせる注入点で、テストはこれで「途中で失敗しても状態が動かない」を叩きます([`editorpreview_test.cpp` 内](../../test/editorpreview_test.cpp#L179) / [`editorpreviewprojection_test.cpp` 内](../../test/editorpreviewprojection_test.cpp#L134))。
>
> **不変条件**: preview経路は `SceneRevision` を進めない。例外経路でも状態検査を通す(状態漏れの報告が元エラーより優先)。

### `get_status` の応答

[`get_status`](../../src/core/communication/rpcserver.cpp#L1333) は診断のハブです。既存の `reload.runtime.pelican.shaders.details`(第6章 6.10)に加え、次が載ります。

| キー | 内容 |
|---|---|
| `scene_source` | `{"hot_reload": false, "save_method": "save_scene"}` |
| `renderdoc` / `diagnostics.renderdoc` | 状態文字列と `{state, reason, api_version, source}`(第6章 §6.18) |
| `debug_utils` | `{available, enabled, reason, capabilities{object_name, command_label, queue_label}}`(第6章 §6.16) |
| `gpu_timing` / `memory` | GPU timing 集計と VRAM 診断(第6章 §6.17) |
| `modules` | `{phase, creation_frozen, initialized, dependencies, initialized_after_runtime_start}` |
| `color` | `{contract: 2, swapchain_format, path, readback_encoding, capture}` |
| `startup` | 起動フェーズごとの所要 ms と shader cache hit 率 |

### `step_frame` と `render_frame` の違い

これは RPC 利用時に最も間違えやすい点です。

- `step_frame` は simulation の1 tick です。`EngineTime::advance()` と通常の5 phase executor を通るため、input、game system、physics、ECS が更新されます。
- `render_frame` は現在時刻の再描画です。通常 game/ECS update は行わず、sequence player を現在時刻へ sample して描画します。
- `set_time` → `render_frame` は、任意時刻を sampling/capture する用途です。
- `inject_input` → `step_frame` は、入力がその frame の game system へ届く標準経路です。

transform update も即適用ではなく pending です。複数 update をまとめてから `step_frame` / `render_frame` の境界で [`flushPendingTransforms()`](../../src/core/communication/rpcserver.cpp#L753) します。

### protocol 上の注意

- stdout は JSON-RPC 専用です。通常 log を stdout へ混ぜると client の1行 protocol を壊します。
- `capture` は headless の `OffscreenFrameTarget` に加え、windowed でも surface が TRANSFER_SRC を持てば readback 可能です([`swapchainframetarget.cpp` 内](../../src/core/vkcore/swapchainframetarget.cpp#L449))。不可の場合は `capture unavailable_windowed` エラーになります。
- `inject_event` は名前で登録された event type にだけ届きます。payload は登録型の binder が解釈します。
- method handler の通常例外は application error `-32000` に正規化されます。[`RpcServer::handleLine()` の `std::exception` catch](../../src/core/communication/rpcserver.cpp#L1067) を参照してください。`JsonRpcHandlerError` を投げれば code と構造化 `data` を指定できます。
- windowed RPC ではリクエストが **フレーム境界でしか処理されません**。queue 容量 64 を超えた分は reader スレッドが `-32000` / `data.reason == "busy"` で即答します。落とし穴は [第9章](09_black_magic_and_gotchas.md)にまとめてあります。

## 7.8 テスト構成: test を実装の仕様書として読む

[`test/CMakeLists.txt`](../../test/CMakeLists.txt#L1) は Catch2 executable と subprocess test を一か所で登録します。`pelican_define_test(name [GROUP group] [GOLDEN] [GPU] [QT] [RUNTIME_SHADER] libs...)` は、`GROUP` 無しなら `test/<name>.cpp` ごとの executable、`GROUP` 有りならソース単位の object target とリンク閉包単位の named executable を作り、compiler option にかかわらず `catch_discover_tests()` で各 `TEST_CASE` を CTest へ公開します。

signature にフラグとグループ名が入ります([`GROUP` を解析する `cmake_parse_arguments`](../../test/CMakeLists.txt#L82))。

```cmake
cmake_parse_arguments(PELICAN_TEST
  "GOLDEN;GPU;QT;RUNTIME_SHADER" "GROUP" "" ${ARGN})
```

`RUNTIME_SHADER` は runtime shader 依存のテストを同じ discovery properties の executable にまとめるための [`discovery_signature`](../../test/CMakeLists.txt#L111) に含まれますが、CTest 登録を抑止するフラグではありません。`PELICAN_RUNTIME_SHADER_COMPILER=OFF` でも discovery 前に return せず、[`catch_discover_tests()`](../../test/CMakeLists.txt#L207) まで到達します。

したがって OFF 構成でも、コンパイラ不在時の logical source 投影、precompiled candidate の診断、renderer/Studio consumer の経路を CTest で検査します。shaderc を実行する分岐だけは build macro で分け、OFF の緑でテスト数自体が減っていないことも discovery 検査で保証します。

`GOLDEN` を付けたテスト(および `debugtext_ui_compat_test`)は、`GPU` フラグ付きのテストと**同じ** `RESOURCE_LOCK pelican_gpu` を持ちます([付与箇所](../../test/CMakeLists.txt#L177))。WP385 で lock 名が `pelican_golden_gpu` から統合されました。コメントが理由です。

> Golden GPU fixtures are part of the one GPU serialization domain.

`RESOURCE_LOCK` が直列化するのは 1 つの `ctest` 実行の中だけなので、process を跨ぐ側は launcher が持ちます。`gpu` label の付いた CTest は [`pelican_gpu_lease_run`](../../test/CMakeLists.txt#L27) 経由で起動され(Catch2 が discover した分は build 後に [`add_test()` 行を書き換える](../../test/patch_catch_gpu_lease_tests.cmake#L89)、手書き `add_test()` は `COMMAND` に直接書く — [launcher を渡す箇所](../../test/CMakeLists.txt#L241))、この launcher が Windows では `Global\` 名前空間の machine-wide mutex を取ります([lease 本体](../../test/gpu_lease_run.cpp#L157))。待ち時間が締め切りを越えたら skip ではなく `gpu_lease_timeout@1{holder_pid=...}` / exit 124 の失敗で、CTest の `TIMEOUT` は `lease_timeout_max + intrinsic + grace` で生成されます(数値の出所は [`gpu_lease_config.cmake`](../../test/gpu_lease_config.cmake#L3) の 1 箇所)。この lease があるので、別 worktree の `ctest` を同時に走らせても GPU は 1 プロセスずつ使われます([`tools/worktree.sh`](../../tools/worktree.sh#L2) が worktree ごとの build と manifest 検査を担当します)。

登録経路は 3 本あり、label が違います。

| 経路 | 登録するもの | label |
|---|---|---|
| `pelican_define_test()` | Catch2 executable。`GPU` フラグで `gpu` | 任意で `gpu` |
| `add_test()` 直書き | cmake / ps1 script による process integration | 個別に `set_tests_properties` |
| [`pelican_define_python_test()`](../../test/CMakeLists.txt#L3198) | Python gate(contract / golden inventory / skip policy / rpc smoke) | 常に `python`(+ 必要なら `gpu`) |

3 本目は `PELICAN_PYTHON_TESTS`(既定 **OFF**、他に `AUTO` / `ON`)が有効なときだけ登録されます。CPU gate の workflow が configure に `-DPELICAN_PYTHON_TESTS=ON` を渡しているのはこのためで、手元の既定 configure では **これらのテストは CTest に存在しません**。`pelican_rpc_smoke` だけは `LABELS "gpu;python"` なので、CPU gate ではなく GPU gate の側に入ります。

テストは4段階に分類すると読みやすくなります。

| 段階 | 例 | 何を保証するか |
|---|---|---|
| pure parser/value | [`sceneformat_test.cpp`](../../test/sceneformat_test.cpp#L120)、[`materialformat_test.cpp`](../../test/materialformat_test.cpp#L104)、[`jsonrpc_test.cpp`](../../test/jsonrpc_test.cpp#L33) | schema、型変換、error 文言。GPU 不要 |
| subsystem unit | [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L204)、[`inputstate_test.cpp`](../../test/inputstate_test.cpp#L9)、[`deletionqueue_test.cpp`](../../test/deletionqueue_test.cpp#L30) | lifecycle、generation、frame 境界、遅延破棄 |
| headless runtime | [`headless_render_test.cpp`](../../test/headless_render_test.cpp#L96)、[`vulkan_headless_test.cpp`](../../test/vulkan_headless_test.cpp#L13) | window なし Vulkan、render/readback |
| process integration | [`run_rpc_headless.cmake`](../../test/run_rpc_headless.cmake#L1)、[`run_compute_headless.cmake`](../../test/run_compute_headless.cmake#L1)、devcli scripts | 実 executable、stdin/stdout、filesystem、終了 code |

### 執筆時点以降に増えた主なテスト群

| 領域 | テスト |
|---|---|
| XR unit | `xractivation_test` / `xrdiscovery_test` / `xrsession_test` / `xraction_test` / `xrviewspace_test` / `xrcompositiontarget_test` / `xrfeaturepolicy_test`(GPU なしの検証は [`synthetic_stereo_target.hpp`](../../test/synthetic_stereo_target.hpp)) |
| VRM | `vrmsemantic_test` / `vrmapplication_test` / `vrmfirstperson_test` / `vrm_xr_demo_test` |
| animation | `animgraph_test` / `skeletalanimation_test` / `animation_abi_dll_test`(DLL ABI fixture) |
| temporal / 描画 | `temporal_test` / `taa_resolve_test` / `color_pipeline_test` / `shader_cache_test` / `texturereload_test` / `surfacecompiler_test` / `spvlink_test` |
| render target / plan | `targetplanning_test` / `renderingsamplecount_test`(いずれも GPU 不要)。WP238e の実 Vulkan command 記録は [`headless_native_scope_test.cpp`](../../test/headless_native_scope_test.cpp) だが、実行体は `headless_render_test` に混ざる(CTest 名は TEST_CASE 名 `WP238e NativeScope records Vulkan commands and rebuilds through renderer generations`。下記) |
| 2D / UI | `sprite_foundation_test` / `ui_foundation_test` |
| event | `eventpayloadschema_test` + compile-time fixture([`run_event_schema_compile.cmake`](../../test/run_event_schema_compile.cmake)) |
| process integration | [`run_game_logic_reload.ps1`](../../test/run_game_logic_reload.ps1) / [`run_input_record_replay_headless.cmake`](../../test/run_input_record_replay_headless.cmake) / [`run_vrm_xr_demo_rpc.cmake`](../../test/run_vrm_xr_demo_rpc.cmake) / [`run_spvlink_golden.cmake`](../../test/run_spvlink_golden.cmake) / run_devcli_{bake_camera, vrm_dump, rules_import, gltf_extract}.cmake |
| editor RPC | `editorcommandservice_test` / `editorjournal_test` / `editorpreview_test` / `editorpreviewprojection_test` / `editorprojectiontransaction_test` / `componentcodec_test` / `windowedrpchost_test` / `renderconfigeditor_test`(render config・graph・共有部品の RPC。N-2 の定義 scope 編集と P-2a-4a の節 locator もここ。**package は入っていません** —— このファイルに `package` の語は 1 つもなく、manifest 側は [`packagemanifest_test`](../../test/CMakeLists.txt#L2605)、install の e2e は `package_install_smoke` が持ちます) |
| Studio(Studio の target がある build にだけ登録。`SKIP_DEVSTUDIO=ON` では**存在しません**) | `devstudio_viewport_test` / `devstudio_outliner_test` / `devstudio_inspector_test` / `devstudio_gizmo_test` / `devstudio_layoutpreset_test` / `devstudio_frameplan_test` / `devstudio_frameplan_graph_test` / `devstudio_fullscreen_pass_test` / `devstudio_graphattachment_test` / `devstudio_graphcanvas_test` / `devstudio_graphcanvas_scene_test` / `devstudio_nodelayout_test` / `devstudio_automation_input_test` / `devstudio_engine_failure_test`(GPU)/ `studio_scenario_*`(GPU、10 本。Studio targetとPython製テスト有効時だけ)。何を固定するかは §7.6 の表。**`devstudio_d0_boundary_negative` だけは Studio の target 無しでも登録されます**(§7.6) |
| ImGui panel | `assetbrowser_test` / `inspector_test`(`PELICAN_WITH_IMGUI` 時のみ) |
| behavior | `behaviorarena_test` / `behavior_determinism_four_processes`(4 プロセス) / [`run_behavior_dll_reload.ps1`](../../test/run_behavior_dll_reload.ps1)(9 ケース) / [`run_behavior_project.ps1`](../../test/run_behavior_project.ps1) |
| ECS | `ecs_migration_test` / `ecs_scheduler_test` / `registration_lifetime_test` |
| schema | `structfieldschema_test` + `struct_schema_compile_*`(pass 3 / fail 6 のコンパイル fixture) |
| lifetime | `lifetime_teardown_test` |
| VRMA | `vrmadecoder_test` / `vrmaretarget_test` / `vrmasource_test`(GPU) |
| 診断 | `debugutils_test` / `rendertiming_test` / `memorydiagnostics_test` / `renderdoccapture_test` |
| 描画 / instance | `modelinstance_slotmap_test`(GPU) / `atlas_descriptor_pool_test`(GPU) / `lightpolicy_test` / `debugtext_ui_compat_test` |
| 物理 | [`run_physics_trigger_behavior.ps1`](../../test/run_physics_trigger_behavior.ps1)(`physics_trigger_behavior_e2e`、GPU ラベル) |
| CLI | `processrunner_test` / [`run_devcli_import_process.cmake`](../../test/run_devcli_import_process.cmake) / [`run_preset_project_headless.cmake`](../../test/run_preset_project_headless.cmake)(§7.2) / [`run_devcli_graph_catalog.cmake`](../../test/run_devcli_graph_catalog.cmake)(§7.5c。catalog と `docs/v0_freeze.md` の表の照合を含む) |
| package(X-1) | `packagemanifest_test`(manifest の純粋 parser と digest)/ `package_install_smoke`(**`PELICAN_WITH_RPC` のときだけ登録**。GPU lease + 300 秒。install → catalog → `edit_render_features` → 描画 → uninstall を実 player で)/ ctest 未登録の [`run_package_template_smoke.cmake`](../../test/run_package_template_smoke.cmake)(package の code を含む一時 project を configure から build。`-DPELICAN_PACKAGE_TEMPLATE_SMOKE_DEPS=<primary>/build/_deps` で primary の依存ソースを借ります) |
| project corpus | ctest 名 [`project_catalog_headless_smoke`](../../test/CMakeLists.txt#L3029)、実装は [`run_project_catalog_smoke.cmake`](../../test/run_project_catalog_smoke.cmake)(登録は `if(TARGET pelican_test_studio_player_arguments)` の中 = **`SKIP_DEVSTUDIO=ON` では存在しません**。`PELICAN_TEST_PROJECTS_DIR` が空なら skip。出荷 project の走査。`project.json` の `requires` を満たせない project は**名前を出して skip** し、player の中で落ちません) |
| RPC | [`test/pelican_rpc_smoke.py`](../../test/pelican_rpc_smoke.py) / [`run_rpc_scene_flow_normalization_fixtures.cmake`](../../test/run_rpc_scene_flow_normalization_fixtures.cmake) + `test/fixtures/rpc_scene_flow/*.ndjson` |
| 契約 gate | [`test/contract_boundary_gate.py`](../../test/contract_boundary_gate.py) + [`test/ci/test_contract_boundary_gate.py`](../../test/ci/test_contract_boundary_gate.py)(fixture: `test/fixtures/contract0/*.json`) |
| CI gate 自体 | [`test/ci/test_skip_policy.py`](../../test/ci/test_skip_policy.py)(CTest 名 `ci_skip_policy_unit`)/ [`test/ci/test_golden_inventory.py`](../../test/ci/test_golden_inventory.py)(同 `golden_inventory_negative_fixtures`)。§7.9 の gate 自身を検査する |
| golden | `golden_cases_test` / `golden_temporal_test` / `golden_timing_test` / `golden_framegraph_test` |

なお [`gltf_scene_extract_test`](../../test/gltf_scene_extract_test.cpp) は `SceneLoader` roundtrip case が Vulkan instance を要求するため **GPU ラベル** に変わりました。`xrsession_test` は `get_status` 投影を検証するので `PELICAN_WITH_RPC` でも囲まれています。

**executable 名とファイル名は 1 対 1 ではありません。** `GROUP` を持つ source はソース別 object target を保ったまま、同じリンク閉包・discovery properties を持つ連続区間ごとの `pelican_test_group_<group>` executable に入ります。また `target_sources()` で別の `TEST_CASE` を足している箇所もあります。WP238e の [`headless_native_scope_test.cpp`](../../test/headless_native_scope_test.cpp) が後者で、`headless_render_test` の実行体へ混ぜられています(したがって `gpu` label もそちらの discovery properties から継承します)。ただし CTest 名まで消えるわけではありません。[`pelican_define_test()`](../../test/CMakeLists.txt#L81) は `catch_discover_tests()` で登録するので **CTest 名は常に TEST_CASE の文字列**で、この case は `WP238e NativeScope records Vulkan commands and rebuilds through renderer generations` という独立した名前を持ちます。逆に `headless_render_test` という CTest 名は存在しないため、`ctest -R headless_native_scope` も `ctest -R headless_render_test` も 1 件も引っかかりません。ファイル名でも実行体名でもテストを引けないのがここの落とし穴です。なお `gltf_scene_extract_test` / `processrunner_test` の `target_sources()` は `src/devcli/*.cpp`(TEST_CASE を持たない実装コード)を link するだけで、これとは別の形です。

### subsystem ごとの「最初に読むテスト」

| 調べたいもの | 推奨テスト |
|---|---|
| ECS の生成・削除・rollback・世代 | [`ecs_lifecycle_test.cpp`](../../test/ecs_lifecycle_test.cpp#L204) |
| game system 順序と GameContext | [`gamesystem_test.cpp`](../../test/gamesystem_test.cpp#L109) |
| event の frame boundary | [`eventlayer_test.cpp`](../../test/eventlayer_test.cpp#L48) |
| input queue、edge、borrow lifetime | [`inputstate_test.cpp`](../../test/inputstate_test.cpp#L14) |
| action map と layer consumption | [`inputactions_test.cpp`](../../test/inputactions_test.cpp#L107) |
| project/path/security | [`projectconfig_test.cpp`](../../test/projectconfig_test.cpp#L168)、[`pathresolver_test.cpp`](../../test/pathresolver_test.cpp#L157) |
| physics の幾何と world binding | [`physquery_test.cpp`](../../test/physquery_test.cpp#L112)、[`physworld_test.cpp`](../../test/physworld_test.cpp#L55) |
| frame graph の順序/異常系 | [`frameplanner_test.cpp`](../../test/frameplanner_test.cpp#L256) |
| rendering JSON | [`renderingpass_helpers_test.cpp`](../../test/renderingpass_helpers_test.cpp#L21) |
| shader compile/reflection/reload | [`shader_compiler_reflection_test.cpp`](../../test/shader_compiler_reflection_test.cpp#L32)、[`shader_library_test.cpp`](../../test/shader_library_test.cpp#L94) |
| feature composition | [`featurecompose_test.cpp`](../../test/featurecompose_test.cpp#L359) |
| persistence の atomic save/path | [`persistence_test.cpp`](../../test/persistence_test.cpp#L81) |

### Golden image test

WP174 / TEST0 で `golden_image_test.cpp` は **分割・廃止** されました。現在は大型の共有ハーネス [`golden_harness.cpp`](../../test/golden_harness.cpp)+ [`golden_harness.hpp`](../../test/golden_harness.hpp)と、それを呼ぶだけの薄い実行体 6 本という構成です。

| 実行体 | 主な内容 |
|---|---|
| [`golden_cases_test`](../../test/golden_cases_test.cpp) | [`runGoldenImages()`](../../test/golden_harness.hpp#L18) が画像許容差・RGBA8 hash・renderer trace / plan order を 1 回の inventory 走査で検証するほか、GPU draw / shadow / reflection 系 |
| [`golden_temporal_test`](../../test/golden_temporal_test.cpp) | jitter / TAA / stereo / velocity 系 |
| [`golden_timing_test`](../../test/golden_timing_test.cpp) | GPU timing の identity / ring / compute / sprite |
| [`golden_framegraph_test`](../../test/golden_framegraph_test.cpp) | [`runFullscreenRebind()`](../../test/golden_harness.hpp#L32) で hot reload / resize 後の descriptor 再結合 |
| [`golden_ssao_test`](../../test/golden_ssao_test.cpp) | [`runSsaoFlatGolden()`](../../test/golden_harness.hpp#L33) 以下、SSAO の flat / XR sequential / XR multiview / cube / planar |
| [`golden_ssao_blur_test`](../../test/golden_ssao_blur_test.cpp) | [`runSsaoBlurFlatOracle()`](../../test/golden_harness.hpp#L38) 以下、SSAO blur の oracle 比較 |

`test/CMakeLists.txt` の [`pelican_golden_test_sources`](../../test/CMakeLists.txt#L1669) がこの 6 本を列挙し、全て `pelican_define_test(... GROUP golden_runtime GOLDEN GPU RUNTIME_SHADER pelican_golden_harness)` で登録されます。`GOLDEN` は `RESOURCE_LOCK pelican_gpu` を付けます([付与箇所](../../test/CMakeLists.txt#L177)。WP385 の統合前は `pelican_golden_gpu` という別名でした)。なお `golden_bloom_upsample_test` だけは `GROUP golden_bloom_upsample_runtime` の別登録で、このリストには入りません([登録](../../test/CMakeLists.txt#L1684))。

inventory の各 case は labels off / on を 1 回ずつ描き、off の同じ `RenderedCase` から画像・hash・trace を検証します。labels の byte 不変条件は全 case で維持され、TAA と GPU timing の独立した決定性テストは別実行のままです。

### golden inventory は manifest が正

case の一覧は **テストコードのハードコード件数ではなく** [`test/golden/inventory.json`](../../test/golden/inventory.json)(`"schema": "pelican.golden_inventory"`, `"version": 1`)が正になりました(WP141 / GOLDEN0)。現在 **52 case** です。各 case は次の形です(先頭 case、実物引用)。

```json
{
  "name": "clear",
  "mode": "clear",
  "files": ["case.json", "expected.png", "tolerance.json"],
  "expected_png_sha256": "2d6f3715483b91e4444c11085fd13444ac343803574403008ade21672a299e78",
  "tolerance": true,
  "vat": "on_and_off",
  "traces": ["rgba8"]
}
```

trace のソースも inventory が宣言します。

| trace 名 | ファイル |
|---|---|
| `canonical_frame_plan` | [`test/fixtures/canonical_frame_plan_trace.txt`](../../test/fixtures/canonical_frame_plan_trace.txt) |
| `renderer_execution` | [`test/fixtures/renderer_execution_traces.json`](../../test/fixtures/renderer_execution_traces.json) |
| `rgba8` | [`test/fixtures/wp73_rgba8_hashes.json`](../../test/fixtures/wp73_rgba8_hashes.json) |

> **設計決定:** inventory の整合は **GPU なしで** [`python -B test/golden_inventory.py --repo-root .`](../../test/golden_inventory.py) が検査し、CPU gate に組み込まれています。さらに [`test/ci/test_golden_inventory.py`](../../test/ci/test_golden_inventory.py) がそのチェッカー自体を単体テストします。「golden ファイルを足したがテストに登録し忘れた」を GPU ランナーを待たずに検出するのが目的です。

golden 更新は見た目が変わったから機械的に受け入れるのではなく、frame plan、layout trace、pixel 差の理由を確認してから行うべきです。

## 7.9 テストの実行方法

repository の基本手順は [`README.md`](../../README.md#L10) にあります。

```powershell
cmake . -B build -DCMAKE_PREFIX_PATH=<Qtの場所>
cmake --build build
ctest --test-dir build --output-on-failure
```

Qt Studio が不要なら configure option は repository README の `SKIP_DEVSTUDIO` を使えます。特定テストだけなら CTest の正規表現 filter が便利です。

```powershell
ctest --test-dir build -R ecs --output-on-failure
ctest --test-dir build -R frameplanner --output-on-failure
ctest --test-dir build -R rpc_ --output-on-failure
```

Visual Studio など multi-config generator では `cmake --build build --config Debug` と `ctest --test-dir build -C Debug ...` のように config をそろえます。

headless test でも Vulkan loader と対応 device/driver は必要です。runtime shader compiler は build option によって test 内の実行分岐が変わり、VAT、EXR、RPC、SeqPlayer は test 自体が conditional になるため、「CTest が緑」だけでなく configure 時にどの option が ON だったかも確認してください。

CI は CPU回帰と構成/clean-cloneの **2 段**です。workflow は [`run_cpu_gate.py`](../../test/ci/run_cpu_gate.py) と [`run_studio_gate.py`](../../test/ci/run_studio_gate.py) を使います。CI1 の clean-clone も CPU gate を再利用します。[`run_gpu_gate.py`](../../test/ci/run_gpu_gate.py) は現時点ではworkflowを持たず、Vulkanと実窓を利用できる環境で回すコマンドです。

### CI0: CPU gate(毎 PR)

GitHub Actions の Windows CPU gate([`.github/workflows/cpu-gate.yml`](../../.github/workflows/cpu-gate.yml)、WP137)があり、GPU を要するテストを `gpu` label で除外した CTest を PR ごとに実行します。skip の判定は [`test/ci/run_cpu_gate.py`](../../test/ci/run_cpu_gate.py) 自身ではなく、両 gate 共通の [`test/ci/skip_policy.py`](../../test/ci/skip_policy.py) にあります(下記)。`run_cpu_gate.py` は 17 行しかなく、[`run_gate()`](../../test/ci/skip_policy.py#L157) に label 選択と allowlist を渡すだけです。

同じ workflow の `windows-msvc-studio` job は Qt 6.10、`SKIP_DEVSTUDIO=OFF`、runtime shader
compiler OFF を明示し、WP354 の frame-plan model と widget の2件を
`QT_QPA_PLATFORM=offscreen` で毎 PR 実行します。engine 側の全 CPU gate と CI1 clean clone は
従来どおり `SKIP_DEVSTUDIO=ON` です。

その後 2 ステップが追加されました。

- `python -B -m unittest discover -s test/ci -p "test_*.py"`(policy checker 自体のテスト。[`test_skip_policy.py`](../../test/ci/test_skip_policy.py) / [`test_golden_inventory.py`](../../test/ci/test_golden_inventory.py) / [`test_contract_boundary_gate.py`](../../test/ci/test_contract_boundary_gate.py))
- `python -B test/golden_inventory.py --repo-root .`(golden inventory の整合)

### CI1: 構成・clean-clone smoke(週次) ✅追加(WP165)

[`.github/workflows/configuration-smoke.yml`](../../.github/workflows/configuration-smoke.yml) は **PR には繋がりません**。`workflow_dispatch` と週次 cron(`17 16 * * 6`)だけで動きます。

- matrix: [`cmake/pelican_feature_registry.cmake`](../../cmake/pelican_feature_registry.cmake) から生成した全対照のbuild-unitジョブ + `PELICAN_PROJECT` のproject-code smoke。各対照は共通基準から対象と宣言済み連動だけを動かします。
- `fail-fast: false`、自動 retry なし。
- 別ジョブ `clean-clone` が「新規 clone から golden inventory → CI policy checker → configure → build → `test/ci/run_cpu_gate.py`」を順に走らせます。

> **設計決定:** 「optional feature を OFF にすると壊れる」は毎 PR で検出する必要がない代わりに、検出が遅れると原因コミットの特定が難しくなる種類の回帰です。週次かつ `fail-fast: false` で **どの構成が壊れたかを一度に全部出す** のがこの workflow の狙いです。運用の正は [`docs/ci.md`](../ci.md) です。

### exact SKIP policy: 3 つ目の結果を塞ぐ

両 gate が共有する判定は [`test/ci/skip_policy.py`](../../test/ci/skip_policy.py) にあります(`03d134d` で `run_cpu_gate.py` から切り出されました)。module docstring が規範です。

> A test has three outcomes, and the dangerous one is the third: a failing test is
> red and gets looked at, but a skipped test stays green while not existing.

テストの結果は「通る / 落ちる / 飛ばされる」の3つで、危険なのは3つ目です。落ちたテストは赤くなって人が見にきますが、**飛ばされたテストは緑のまま、存在しないのと同じ**になります。CTest の summary は skip を失敗として数えないので、`ctest` が「0 failed」と言っている実行の中でテストが丸ごと消えていることがありえます。

そこで gate は「飛ばしてよいものを名指しする」方式を取ります。allowlist は完全一致の CTest 名だけで、ワイルドカードは書けません。そして **列挙したのに一度も現れなかった名前もエラー**にします — こうしないと、テストが消えたり改名されたりしたときに allowlist だけが残り、「かつて skip を許した何か」を無期限に許し続けます。

> 🧩 **難所 — 許可リストが両方向に効く**([`validate_skip_policy()`](../../test/ci/skip_policy.py#L74))
>
> **何をする所か**: CTest が出した JUnit report と allowlist を突き合わせ、policy 違反があれば [`SkipPolicyError`](../../test/ci/skip_policy.py#L23) を投げます。各gateの駆動部は [`run_gate()`](../../test/ci/skip_policy.py#L157) へ label 選択・allowlist・必須テスト集合などを渡します。共通policyがJUnitを検査し、GPU gateはさらに出荷Studio scenario全件の実行を要求します。
>
> **素朴に読むと**: 「allowlist に無い skip を弾く」だけの関数に見えます。実際には集合の差を **2 方向** に取っており、後ろ側(`allowed - reported`)が読み飛ばされやすい所です。前者は「許していない skip が起きた」、後者は「許した名前が実行のどこにも現れなかった」で、後者もエラーにするのは allowlist の腐敗(stale — 元のテストが消えたり改名されたりして、その行が何も指さなくなった状態)を防ぐためです。名指しの許可は、名指しの対象が実在し続けることを確認できて初めて意味を持ちます。もう一つ読み落としやすいのが「report が空なら成功ではなくエラー」で、これも同じ性格 — 何も走らなかった実行を緑にしないためです。
>
> **骨子**:
> ```text
> validate_skip_policy(junit, allowlist, gate_name, bulk_skip_hint):
>   allowed           = load_allowlist(allowlist)   # 完全一致のみ。* ? [ を含む行は即エラー
>   reported, skipped = read_junit(junit)           # testcase 全数 / skip した名前
>   allowed - reported が空でない -> エラー(allowlist が腐っている)
>   skipped - allowed  が空でない -> エラー(許可していない skip)
>       その件数 * 2 >= reported の件数 なら bulk_skip_hint を先頭行に付ける
>   return skipped
> ```
>
> **手がかり**: [`read_junit()`](../../test/ci/skip_policy.py#L47) は `<skipped>` 子要素と `status` 属性(`notrun` / `skipped`)の **どちらか**で skip と見なします — CTest は "Not Run" を `status="notrun"` で書くので両方見る必要があります。testcase が 1 件も無い report は `SkipPolicyError` です。bulk hint の閾値が「過半数」なのは、1 件が skip したのと全部が一斉に skip したのは**別の診断**だからで、後者を 100 件の名前の羅列に埋もれさせないためだけの分岐です(テスト: [`test/ci/test_skip_policy.py`](../../test/ci/test_skip_policy.py))。[`run_gate()`](../../test/ci/skip_policy.py#L157) は `ctest` を回した**後**に policy を見て、違反なら ctest 自身の exit code に関係なく **2** を返します。逆に言えば policy が通った場合だけ ctest の exit code がそのまま gate の exit code です。成果物は `--artifacts-dir` 配下の `ctest-junit.xml` / `ctest.log` / `skip-policy.txt` の3つで、最後のものに PASS/FAIL と観測した skip 名が残ります。[`run_ctest()`](../../test/ci/skip_policy.py#L119) が `--no-tests=error` を渡しているので、label 選択が 1 件も選ばなかった場合もそこで落ちます。
>
> **不変条件**: allowlist は完全一致の CTest 名のみ(重複行もエラー)。列挙した名前は必ず一度は現れること。**gate は `ctest` が緑でも落としうる**。

### GPU gate(`run_gpu_gate.py`)✅追加(`03d134d`)

CI0 は `ctest -LE gpu` なので、`gpu` label の側 — golden 画像比較、validation layer、player の process integration — は **これまで常設の回帰網の外**にありました。[`test/ci/run_gpu_gate.py`](../../test/ci/run_gpu_gate.py) はその反対側を回す gate です。**workflow はまだありません**(`.github/workflows/` は CPU gate と configuration smoke の2本だけ)。Vulkan device のある機械で手で叩きます。運用の正は [`docs/ci.md`](../ci.md) です。

```powershell
python -B test/ci/run_gpu_gate.py --build-dir build --config Debug --artifacts-dir build/ci-artifacts-gpu
```

SKIP判定はCPU gateと共有し、GPU gateは出荷Studio scenario全件の未登録・未実行も拒否します。選択と診断の違いは次のとおりです。

| | CPU gate | GPU gate |
|---|---|---|
| label 選択 | `-LE gpu` | `-L gpu` |
| allowlist | [`cpu_skip_allowlist.txt`](../../test/ci/cpu_skip_allowlist.txt)(1 件。directory symlink を作れない runner のための `PathResolver` テスト) | [`gpu_skip_allowlist.txt`](../../test/ci/gpu_skip_allowlist.txt)(**意図的に空**) |
| 一斉 skip の診断 | なし | [`NO_DEVICE_HINT`](../../test/ci/run_gpu_gate.py#L16) |

GPU の無い機械で走らせると大半が skip して gate は落ちます。これは意図どおりで、`docs/ci.md` が言う「fail-on-no-GPU」の実体です。skip が過半数のときだけ「no Vulkan device」の診断行が先頭に出ます。

#### 2026-07-31 の初回実行: `ctest` は緑、gate は FAIL

この gate を最初に回した日の結果を事実として記録します。**同じ実行で `ctest` 自身は「122 件中 0 失敗、100% tests passed」と報告し、gate は 4 件の非許可 skip を検出して FAIL しました。** 同じ検証で回した CPU gate は 942/942、allowlist 済みの skip 1 件で通っています。

4 件はいずれも **テスト本体全体**を `try { ... } catch (const std::exception &error) { SKIP(...) }` で囲んでおり、engine 自身の fail-fast エラーを "unavailable" と報告していました。このエンジンは異常時に `throw` して止まる設計なので、この idiom は **検出すべき回帰をそのまま silent skip に変換します**。握り潰されていたのは 2 系統です。

| skip していた CTest 名 | 実装 | 握り潰されていたエラー |
|---|---|---|
| RPC load_gltf publishes once and preserves inventory on preflight and GPU failure | [`rpc_color_contract_test.cpp`](../../test/rpc_color_contract_test.cpp) | `render-pipeline candidate has no compatible pass for live material 0 (route 'deferred_geometry', shader contract 'gbuffer_v1')` |
| HR1-M updates one same-layout material and rolls back invalid candidates | [`materialvaluesreload_test.cpp`](../../test/materialvaluesreload_test.cpp) | `material texture 'albedo_detail' is absent from shader reflection at binding 7` |
| HR1-M watcher gate and 1000 reloads keep resources bounded | 同上 | 同上 |
| WP206b named variant owns its GPU record and reloads atomically with its base | 同上 | 同上 |

GPU 不在による skip ではありません。**同じ実行で他の 118 件は実 device 上で通っています。** 最後の 1 件は名前に WP206b を冠しますが、[`render_evidence_ledger.md`](../render_evidence_ledger.md) が WP206b を **E3 +E5** と判定した根拠に挙げているのは `headless_render_test` の "project-owned material variant renders a second opaque pass" の方で、そちらは同じ実行で通っています。skip していたのは material reload 側の別 TEST_CASE です。

`gpu_skip_allowlist.txt` が **意図的に空**なのはここに繋がります。この4件を列挙すれば gate は緑になりますが、それは **gate が捕まえるために存在するものを祝福する**ことになります。WP241([`docs/implementation_plan.md`](../implementation_plan.md))は allowlist を増やさず4件を修正し、2026-08-01 の再実行は GPU 125/125、skip 0 で通過しました。

**では capability 不足の skip はどう書くか。** WP241 は [`vulkan_test_support.hpp`](../../test/vulkan_test_support.hpp) に判定を集約しました。テスト開始時の `requireVulkanDevice()` は `No suitable Vulkan physical device found` だけを skip し、それ以外を再送出します。後始末のため本体を `try` で囲む既存テストも `skipIfVulkanDeviceUnavailable()` の厳密な判定後に必ず `throw;` します。「device を用意できなかった」と「用意できた上で落ちた」を分けるのが契約です。

## 7.10 変更時のテスト選択

最小単位だけで終わらせず、変更が越えた境界まで一段ずつ広げます。

```text
pure parser変更
  -> 対応fixture test
  -> config/runtime binding test
  -> 必要ならheadless/golden

ECS lifecycle変更
  -> ecs_lifecycle
  -> sceneformat / gamesystem
  -> headless player integration

frame graph / shader変更
  -> frameplanner / shader tests
  -> golden_image
  -> compute/RPC headless subprocess

CLI変更
  -> project libraryのpure test
  -> 対応run_devcli_*.cmake
```

Pelican の test suite は単なる関数単体確認ではありません。pure data format と runtime binding を分け、最後に headless executable を実際に動かす構造が、production code の層分けそのものを映しています。

## 7.11 Python RPC クライアント `tools/pelican_rpc.py` ✅実装済み(WP160)

エディタ側や実験スクリプトから engine を叩くための薄いクライアントです。置き場所は `src/` でも `test/` でもなく [`tools/pelican_rpc.py`](../../tools/pelican_rpc.py) です。

```python
from pelican_rpc import PelicanRpc

with PelicanRpc("projects/example") as rpc:
    print(rpc.get_status()["frame"])
    rpc.step_frame()
    tree = rpc.scene_tree()
```

- [`PelicanRpc(project_dir, exe_path=None)`](../../tools/pelican_rpc.py#L23) が `pelican_player` を `--rpc --headless --project <dir>` で起動します。
- 実行体の既定探索は `build/dist/Debug/pelican_player.exe` を、リポジトリルート → cwd → cwd の各祖先の順に探します([`_resolve_executable()` 全体](../../tools/pelican_rpc.py#L54))。別の build/config は明示指定し、見つからなければ探索した全 path を並べた `FileNotFoundError` になります。
- [`call(method, params)`](../../tools/pelican_rpc.py#L78) は 1 行 1 リクエストの NDJSON を書き、1 行読み、`id` 一致と `jsonrpc == "2.0"` を検証してから `result` を **そのまま** 返します。エラーは [`PelicanRpcError(code, message, data)`](../../tools/pelican_rpc.py#L13) です。
- 薄いショートカットが `get_status` / `step_frame` / `set_time` / `render_frame` / `capture` / `load_scene` / `scene_tree` / `get_components` / `pick_object` / `set_gizmo` / `query_gizmo_handle` / `list_assets` / `export_scene_snapshot` / `import_scene_snapshot` / `eval_preview` / `render_preview` に用意されています。
- `terminate()`(別名 `close`)と `with` 文をサポートします。

スモークテストは [`test/pelican_rpc_smoke.py`](../../test/pelican_rpc_smoke.py) です。

## 7.12 ImGui の inspector / asset browser ✅実装済み(WP159 / WP164 / WP167 / WP245)

engine 内蔵の開発者 UI に、読み取り専用の [`AssetBrowserPanel`](../../src/core/imgui/assetbrowser.hpp#L36) と schema 駆動の [`InspectorPanel`](../../src/core/imgui/inspector.hpp#L167) が加わりました。表示は `ImGuiSystem` のメニュー `Asset Browser` / `Inspector` から切り替えます([`imguisystem.cpp` 内](../../src/core/imgui/imguisystem.cpp#L373))。

> **設計決定:** **両パネルとも `EditorCommandService` を経由します。** RPC とまったく同じ typed サービスを呼ぶのが設計上の要点で、そのために [`EditorCommandImGuiFakeAdapter`](../../src/core/communication/editorcommandservice.hpp#L491) が用意されています。コメントが規範です。
>
> The ImGui WP consumes the same typed service. This fake is deliberately kept
> free of ImGui headers so equivalence is testable in the CPU-only suite.

inspector のウィジェットは component schema から決まります。[`makeInspectorWidgetPlan(const EditorComponentQueryResult &)`](../../src/core/imgui/inspector.hpp#L76) が [`InspectorWidgetKind`](../../src/core/imgui/inspector.hpp#L53) の 8 種(`SignedIntegerDrag` / `UnsignedIntegerDrag` / `FloatingPointDrag` / `BooleanCheckbox` / `EnumCombo` / `StringInput` / `VectorDrag` / `QuaternionDrag`)を割り当て、編集対象の位置は [`inspectorJsonPointer(schema_field_name)`](../../src/core/imgui/inspector.hpp#L74) が JSON pointer(RFC 6901 の標準記法 — `/a/b/0` のようにスラッシュ区切りで JSON 文書内の 1 箇所を指す書き方)で表します。schema 側の宣言は [第5章 §5.14](05_gameplay_and_services.md) です。

外部からの変更検知は watch token のポーリングです。[`inspectorWatchPollFrameInterval = 30`](../../src/core/imgui/inspector.hpp#L25) フレームごとに [`pollInspectorWatch(state, refresh_blocked, query, refresh)`](../../src/core/imgui/inspector.hpp#L42) が [`InspectorWatchState`](../../src/core/imgui/inspector.hpp#L32) を更新します。ただし preview lease の保持中または schema widget の編集中は [`inspectorRefreshBlocked()`](../../src/core/imgui/inspector.hpp#L27) がポーリングと snapshot refresh を止め、編集中の `authored_json` を保持します。保留した refresh は編集終了後に実行し、自分の commit / abort / save 後は同時に新しい watch token を観測します。非編集中の外部変更は従来どおり refresh されます。この2経路は [`WP245 active inspector edits suppress watch refresh and retain the dragged value`](../../test/inspector_test.cpp#L700) と [`WP245 idle inspector watch still refreshes external scene changes`](../../test/inspector_test.cpp#L754) で別々に固定しています。

### ゲート: `--rpc` を付けると ImGui は動かない

両パネルの呼び出しは `invokeAssetBrowserPanelCallback` / `invokeInspectorPanelCallback` を通り、いずれも [`isImGuiRuntimeEnabled(config)`](../../src/core/imgui/imguiruntime.cpp#L9) を見ます。

```cpp
return !config.headless && !config.rpc && !config.input_replay && !config.golden_mode &&
       !config.xr_active;
```

つまり **`--rpc` を付けた windowed セッションでは ImGui UI(したがって inspector)は動きません**。排他の実体は「リクエストを処理する間だけ UI を止める」「stdin 読み取りでブロックする」といった実行時の調停ではなく、**config を見るだけの一枚のゲート**です。同じ述語は frame graph の合成時にも通るため([`renderingpassconfigregistration.cpp` 内](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L180))、`--rpc` のセッションには `imgui_pass` がそもそも合成グラフに入りません。実行時も [`resolveFrameStateModules()`](../../src/core/appflow/framephase.cpp#L61) が毎フレーム同じ述語を評価し、偽なら `ImGuiSystem` を frame state に載せないので、パネルの callback は一度も呼ばれません。ヘッダのコメント「Deterministic drivers therefore skip callbacks, instead of running an invisible ImGui frame.」がこの並び(headless / rpc / replay / golden)の意図です。ゲートが**実行中に**閉じうるのは XR activation と replay 開始で、そのとき開始済みの ImGui フレームは `endFrameIfStarted()` で閉じられます。XR を除外している理由だけは別で、実装側のコメントにあるとおり「XR グラフに ImGui pass が無いので、開始した ImGui フレームに対応する Render/EndFrame が無くなる」ためです。

テストは [`test/assetbrowser_test.cpp`](../../test/assetbrowser_test.cpp) と [`test/inspector_test.cpp`](../../test/inspector_test.cpp) です。

> 🧩 **難所 — dirty を先に吐き切る**([`drivePreview()`](../../src/core/imgui/inspector.cpp#L712) / [`pollPreview()`](../../src/core/imgui/inspector.cpp#L723))
>
> **何をする所か**: ドラッグ中のフィールド編集を preview lease(§7.7)へ流す、クライアント側の1本キューです。[`PreviewState`](../../src/core/imgui/inspector.cpp#L412) の旗を見て、次に送るのが `update` / `commit` / `abort` のどれかを決めます。
>
> **素朴に読むと**: `PreviewState` には `outstanding_request` / `dirty` / `release_requested` / `abort_requested` と旗が4つあり、`drivePreview()` の if-else 3段が優先順位を決めています。この順序に意味があるようには見えませんが、入れ替えると編集値が失われます。ImGui のドラッグは毎フレーム新しい値を作るのに対し、サービスへ投げられるリクエストは同時に1本だけです(`outstanding_request` が空でなければ何も送りません)。そこで `latest_operation` と `sent_operation` の差を `dirty` として畳み、送れるようになった時点で最新値だけを1回送ります — これがコアレッシング(coalescing — 連続して届く更新をまとめ、中間値を捨てて最新の1件だけを送る手法)です。マウスを離すと `release_requested` が立ちますが、そのとき未送信の `dirty` が残っていることは普通にあります。`abort > dirty > release` という順序は「中断は最優先」「確定の前に必ず最新値を送り切る」を意味し、`dirty` より `release` を先にすると `commit_preview` がサーバ側の**古い値**で確定し、最後のドラッグ分が黙って消えます。
>
> **骨子**:
> ```text
> draw(): pollPreview() -> ウィジェット描画(latest_operation と dirty を更新) -> drivePreview()
> drivePreview():
>   outstanding_request が空でない -> 何も送らない
>   abort_requested -> Abort / dirty -> Update / release_requested -> Commit
> pollPreview():
>   accepted のまま -> 待つ
>   open|updated   -> outstanding_request を空にして drivePreview() を呼び直す(ポンプ)
>   committed|abort成功 -> preview.reset() して refresh()
> ```
>
> **手がかり**: `dirty` は立てっぱなしの旗ではなく [`latest_operation != sent_operation`](../../src/core/imgui/inspector.cpp#L808) の再評価です(値を元へ戻せば消えます)。応答受領後に `drivePreview()` を呼び直しているのがポンプで、これで dirty→Update→dirty→…→Commit と自然に並びます。`open_preview` が `method_unavailable` で落ちたときだけ preview を諦め、`field_key` を `preview_unavailable` に記録して通常の編集キュー([`enqueueEdit()`](../../src/core/imgui/inspector.cpp#L594))へ流す退避経路があり、preview 非対応のフィールドでも編集自体は通ります。サーバ側 lease の状態機械(§7.7 の難所)とは別物で、こちらは in-flight を1本に保つクライアント側の話です。テストは `test/inspector_test.cpp` の [`WP164 UI and RPC adapters preserve query edit undo and preview results`](../../test/inspector_test.cpp#L357)。
>
> **不変条件**: in-flight は常に高々1本。`commit` の前に `dirty` を必ず吐き切る。失敗応答では `preview` を必ず `reset()` する(lease を握ったまま旗だけ残さない)。
