# CI 運用: CPU gate (CI0) と構成/clean-clone smoke (CI1)

## 役割

GitHub Actions の `CPU gate` は Windows/MSVC の configure、Debug 全ターゲット build、
GPU 不要 CTest を pull request ごとに検査する。加えて Qt 6.10 を持つ別 job が
`SKIP_DEVSTUDIO=OFF` / runtime shader compiler OFF で `studio_cpu` ラベルの
Studio model/widget テストを検査する。Qt widget は offscreen、実 HWND を使う
viewport の結合テストは Windows platform plugin を使う。統合ブランチ
`codex/rendering-phase1-refactor` への push でも実行し、同じ pull request / ref の古い run は
`concurrency.cancel-in-progress` で中止する。テストや build の自動 retry は行わない。

CI0 は CPU 回帰の常設 gate であり、次は引き続きローカル gate である。

- 全 CTest (`gpu` ラベルを含む)
- golden image と final RGBA8 byte gate
- player の短時間起動確認

feature OFF / build-unit / project-code smoke と clean-clone gate は、毎 push の
所要時間を増やさないよう週次/手動の CI1 に分離する。

Vulkan を保証した runner で golden、validation layer、player を fail-on-no-GPU で実行する責務は
将来の CI2 に置く。CI0 の runner に Vulkan device があることは期待しない。

## 2026-09-22 のローカル Release 検証

`build-v0` は Windows / MSVC、Qt 6.10.3、Python 3.12、Vulkan SDK 1.4.x、Studio ON、
`PELICAN_PYTHON_TESTS=ON`、runtime shader compiler ON の Release 構成。
Release build と絶対パスの prefix への Runtime install は成功した。

| gate / 確認 | 結果 |
|---|---|
| CPU gate(2026-09-24、`47cbf3cf`) | 1,793 件中 1,792 PASS、allowlisted skip 1 件、失敗 0。skip は directory symlink 作成権限が必要な `PathResolver rejects symlink escapes after canonicalization`。`v0-cpu-final6/` の exact SKIP policy も PASS |
| 全 GPU gate(2026-09-24、`47cbf3cf`) | 275 件中 274 PASS、allowlisted skip 1 件(Debug 専用の `wsi_fault_window_player`)、失敗 0。実窓 Studio scenario 10 件はすべて観測・通過。exact SKIP policy PASS。artifact は `v0-gpu-final6/` |
| Studio CPU gate(2026-09-24、`47cbf3cf`) | compiler ON の release 構成なので空の allowlist で実行。263 件中 263 PASS、skip 0、exact SKIP policy PASS。artifact は `v0-studio-final6/` |
| 全 GPU gate の経緯 | 09-22 初回は 4 失敗(`v0-gpu-final/`)、修正後の再実行は `studio_scenario_graph_observe_roundtrip` の 1 失敗(`v0-gpu-verified/`)。後者は viewport の resize による render pipeline の再 lowering が editor の遷移 epoch を動かし、受付済みの編集を `gate_closed` で捨てていた。`ba82495f` で遷移 epoch を participant の reload 結果だけから作るよう修正 |
| 配布 stage(2026-09-24、`47cbf3cf`) | `run_distribution_check.py` で 8 / 8。出荷 sample 7 本の起動、別 project 経由の再読込、capture、Frame Plan dock の表示、正常終了、project ごとの player log、終了時の surface lost 0 回を確認。新規 project の位置編集・保存・完全終了・Studio 再起動でも authoring ID と保存値の持続を確認。`distribution/run-20260924-003010-57332/report.json` |
| Debug 専用(2026-09-24、`47cbf3cf`) | 同じ tree の Debug build で `wsi_fault_window_player`(注入 6 回・surface 作り直し 5 回・公開 2 回)と `crash_symbol_production_wiring`(Studio の `47cbf3cf` の PDB が archive 済み)が通過 |
| `47cbf3cf` までの経緯 | `50f1fbda` の全 GPU gate で `devstudio_engine_failure_test` が赤(player の log の置き場を旧い `pelican.log` で固定していた。`777b4824` で project の log を読むよう修正)。CPU gate で `state_batched_v1 reproduces the legacy command bytes and view ranges` が赤(`RenderCommand` 末尾 2 バイトの padding を比べていた。この build では 40 回中 34 回失敗し、以前の緑は偶然。`47cbf3cf` で padding を名前付きの 0 初期化メンバーにして 0 / 40) |

gate の artifact は `build-v0/test_artifacts/` 以下。配布物の操作ログと詳しい確認範囲は
[v0 の検証記録](v0_freeze.md#6-v010-tag-の手順) を参照する。CPU / GPU gate、配布 stage、Debug 専用の確認は
`47cbf3cf` で通過した。以後の revision で tag を付けるときは、その revision でやり直す。

Release では Debug 専用の `wsi_fault_window_player` を明示 skip とし、
`crash_symbol_production_wiring` は Debug 構成だけを対象にする。両者は同じ tree の Debug build で確認した。
player の validation on/off 対照や fixture の symbol archive 契約テストは、
Debug の故障注入・実 Studio の PDB 配線の確認とは分けて扱う。

同じ logical frame を再描画したときの GPU timing sample ID 重複による fatal は修正済み。
Release の runtime ログも file + stderr へ戻した。後者により Studio / テストへ診断が届くようになったため、
過去の file のみの構成での成功件数を現行 gate の成功根拠にしない。

## ローカルで同じ gate を実行する

Python 3.12 と Vulkan SDK 1.4.x を PATH / `VULKAN_SDK` から参照できる状態で実行する。
Python の場所は環境ごとに異なるため、リポジトリ内に絶対パスを保存しない。

```powershell
cmake -S . -B build -DSKIP_DEVSTUDIO=ON `
  -DPELICAN_PYTHON_TESTS=ON `
  -DPELICAN_WITH_SPIRV_LINK=ON
cmake --build build --config Debug
python -B test/ci/run_cpu_gate.py `
  --build-dir build `
  --config Debug `
  --artifacts-dir build/ci-artifacts
```

最後のコマンドは次の CTest と SKIP policy 検査を一体で行う。

```powershell
ctest --test-dir build -C Debug -LE gpu --output-on-failure --no-tests=error
```

Python policy の self-test、public API inventory、golden inventory は CTest に登録する。
workflow の前段で同じチェックを重ねて実行しない。

`-j16` は測定に基づく。この tier は直列で 124 秒、`-j16` で 43.8 秒である。
GPU を含む全数は `-j4`(674 秒 → 346 秒)。それ以上は
`pelican_gpu` の resource lock が臨界経路になるため伸びず、
`-j64` では GPU が SEGFAULT した。

GPU 対象だけを確認する場合は `ctest --test-dir build -C Debug -L gpu -N` を使う。

## devstudio の Qt gate

常設 CI0 の engine job と CI1 clean-clone job は **Qt を導入せず
`-DSKIP_DEVSTUDIO=ON` を維持する**。一方、CI0 の `windows-msvc-studio` job は Qt 6.10 を導入し、
`-DSKIP_DEVSTUDIO=OFF -DPELICAN_RUNTIME_SHADER_COMPILER=OFF` で
`pelican_studio_cpu_tests` target をビルドし、`studio_cpu` ラベルの全ケースを実行する。
対象は GPU を必要としない `devstudio_*` テストで、特定 WP の名前には限定しない。
D0 リンク境界の負例テストは Qt 非依存なので、
engine job にも引き続き登録する。

devstudio のコンパイル、レイアウト永続化、frame plan 表示 model のテストは、Qt 6.10 を用意した
開発環境で次の明示構成を使う。`devstudio_layoutpreset_test` は GUI を操作せず、名前付き保存・復元、
版違い、破損 state の既定配置 fallback と後から追加された dock の互換を検査する。
`devstudio_frameplan_test` は `get_frame_plan` 応答の構造化を view と live player から切り離して検査する。

```powershell
cmake -S . -B build-studio -DSKIP_DEVSTUDIO=OFF `
  -DCMAKE_PREFIX_PATH=<Qt-install-path> `
  -DPELICAN_RUNTIME_SHADER_COMPILER=OFF -DPELICAN_WITH_SPIRV_LINK=OFF `
  -DPELICAN_PYTHON_TESTS=OFF
cmake --build build-studio --config Debug --target pelican_studio_cpu_tests
python -B test/ci/run_studio_gate.py --build-dir build-studio --config Debug `
  --artifacts-dir build-studio/ci-artifacts
```

CTest の各テストが必要な Qt platform を設定する。`studio_skip_allowlist.txt` は
compiler OFF で動かせない WP344 planar prefilter / WP348 cube capture の2件だけを許可する。
それ以外の skip、未実行、0件選択は gate の失敗になる。compiler ON で同じ gate を使う場合は、
空の allowlist ファイルを `--allowlist <path>` で渡す。RT1a の compiler/SPIRV_LINK OFF
試験も独立して維持する。

### GPU gate(`run_gpu_gate.py`)

CI2 は存在しないが、**gate 自体は名前の付いたコマンドとして用意してある**。Vulkan device の
ある機械で次を実行する。完全な gate は Studio の実窓 scenario も対象とするため、
Qt と Python 3.11 以上を用意し、`SKIP_DEVSTUDIO=OFF`、`PELICAN_PYTHON_TESTS=ON`、
`PELICAN_RUNTIME_SHADER_COMPILER=ON` の build を使う。CI2 が立つときはこれを呼ぶだけでよい。

```powershell
python -B test/ci/run_gpu_gate.py `
  --build-dir build `
  --config Debug `
  --artifacts-dir build/ci-artifacts-gpu
```

CPU gate と同じ exact SKIP policy を `test/ci/gpu_skip_allowlist.txt` に対して適用する。
判定ロジックは `test/ci/skip_policy.py` に両 gate 共通で置き、self-test は
`test/ci/test_skip_policy.py`(`python -m unittest test_skip_policy`)。
既定allowlistはWindowsかつStudio有効の構成に対応する。別のplatformやStudioを外す限定検証は
対応するファイルを`--allowlist`で明示し、登録されないテスト名を自動で除外しない。

`test/studio_scenarios/*.json` の全 scenario を正確な CTest 名で必須とし(件数はこの文書では追わない。
2026-10-05 の時点で 84 件。2026-09-24 の gate の 10 件は当時の数)、
未登録・未実行・skip も失敗にする。scenario の CTest 登録と起動は CMake が検出した
Python を直接使い、`uv` の有無には依存しない。driver は step の成功に加え、
Studio の exit 0 と強制終了されていないことを確認する。

Studio を含めない限定確認には `--engine-only` を指定できる。この場合は
`studio_scenario` ラベルを除外し、必須 scenario 検査を外す。**release gate の代用にはしない**。
Studio の GPU 不要テストは上の `run_studio_gate.py` でも確認し、compiler ON の release
構成には空の allowlist を渡す。

`project_catalog_headless_smoke` の corpus は clone / worktree の外に置き、configure 時に
`-DPELICAN_TEST_PROJECTS_DIR=<projects>` で指定する。未指定時は理由
`PELICAN_PROJECT_CATALOG_SMOKE_SKIP_NO_PROJECTS_DIR` を持つ skip になり、その CTest 名だけを
GPU allowlist で許可する。entry を外したまま未指定 gate を走らせると unexpected skip として失敗する。

#### project catalog smoke の検査の中身(2026-10-05、EB-5b `85d0b06b` の後)

`project_catalog_headless_smoke`([script](../test/run_project_catalog_smoke.cmake))は corpus の project を名前順に回し、
project ごとに次を見る。最初の失敗で止まるので、後ろの project と editor lane は走らない。

- **絵の閾値**: player を `--size 160x90` で 10 frame 走らせ、frame 10 の PNG を `pelican_test_png_nonuniform_check` に
  掛ける。project の `catalog_smoke.json` の `minimum_distinct_colors`(色の数)と `minimum_non_modal_ratio`
  (最頻色でない画素の割合)の両方に届かなければ赤。
- **animation**: animation を宣言する project(project の下に `*.anim_graph.json` がある、または rendering config の
  `pelican.graph` root が `"pose.` の op を持つ。出荷 7 本では animgraph_demo・posegraph_demo・vrm_xr_demo)だけ、
  同じ game logic・同じ frame 数で **1920x1080** の player をもう 1 回走らせ、frame 1 と frame 10 で RGBA が違う画素が
  **1,200 以上**あることを `png_nonuniform_check --changed` で見る([大きさと最小の画素数](../test/run_project_catalog_smoke.cmake))。
  閾値の検査は 160x90 のまま。
  - EB-5b の前は 160x90 の frame 1 と frame 10 の byte 比較だった。vrm_xr_demo の Idle の上下動は frame 10 で 0.015 単位、
    160x90 では **0.33 画素**しかなく、画素の中心をまたぐ縁が無いので 2 枚は byte 一致していた(以前通っていたのは、
    縁 1 行が偶然またいでいたから。EB-5b の実装者が CC-1 の前の配置を再現して確かめた)。つまり検査になっていなかった。
  - 1920x1080 では同じ動きが 3.9 画素(実測 2,556 画素が変わる。動く縁は 1 行ごとに約 640 画素)。1,200 は約 2 行分で、
    縁 1 行が偶然またいだだけでは届かない。animgraph_demo は 27,035、posegraph_demo は 55,444、動かない project は 0。
    負対照(vrm_xr_demo から animation を動かす game logic を外す)はこの検査で赤になる(実装者の run)。
  - 1,200 は vrm_xr_demo の今の Idle で決めた値で、主役が小さく動きの少ない animation の project を足すと落ちうる
    (そのときは 1920x1080 の changed を測って見直す。実装者の注意)。animation の 3 project で 1920x1080 の player が
    1 回ずつ増える分、smoke の所要が延びる(秒数は測っていない。test の timeout は 600 s)。
- **MG-2b の root の検査**: rendering config が `pelican.graph` root でなければ名前で赤。
- **editor lane**(editor の設定のある project): `scene_tree` → `pick_object` → `set_gizmo` → `render_frame` → capture で、
  選択の有無で capture が違うことを見る。答えは行の top-level の `id` を JSON で読んで照らす([`response_for_id`](../test/run_project_catalog_smoke.cmake))。

**09-22 から動いていなかった検査**: be35b7de8(2026-09-22、requires の判定を `cmake/pelican_project_requirements.cmake` へ移した commit)で
`file(READ "${project_manifest}" project_manifest_text)` が消え、変数が未定義のままだった。そのため posegraph_demo の animation の
検査(posegraph_demo の宣言は rendering config の `pose.` だけ)と MG-2b の root の検査が、09-22 から一度も走っていなかった。
EB-5b で `file(READ)` を戻した。出荷 7 project の root はすべて `pelican.graph` で、戻した後もこの検査で落ちるものは無い。

**editor lane の取り違え**: `response_for_id` は行の中のどこかの `"id":N` を正規表現で探して最後に当たった行を返していた。
MP-1(`afd923e1`、10-02)で `get_frame_plan` の `drawn_materials` に `{"draws":2,"id":2,…}` が入ってから、id 2(`set_gizmo`)を探すと
id 9 の `get_frame_plan` の行が返り、`result visible` が無いと赤になった(vrm_xr_demo が閾値で先に止まっていたので、10-05 の全数では表に出ていない)。

**vrm_xr_demo の閾値の記録し直し**: `minimum_distinct_colors` 50 → **20**、`minimum_non_modal_ratio` 0.04 → **0.09**。
50 / 0.04 は a4a12da7(2026-08-14、WP295)で入ったきり変わっておらず、CC-1(`c8367b87`、10-03)の前の絵に合っていた。
CC-1 の前の vrm_xr_demo は、上下が反転して描かれる engine で正立して見えるよう、主役と床を y>0 に逆さに置き、camera は床の
**裏**(下向きの光が当たらない黒)を見上げていた。色数は床の手前の縁の specular の横の階調(約 60 色)から来ていた。
CC-1 で配置が正立し、光が床の上面を一様に照らすようになって 36 色になった(今の絵が sample の意図どおりであることは
EB-5b の実装者が 960x540 で確かめた)。新しい値は負対照から決めた(160x90 の frame 10):

| 絵 | distinct | non-modal | 20 / 0.09 |
|---|---|---|---|
| 今の絵(game logic あり) | 36 | 0.124 | 通る |
| game logic なし(表情なし) | 37 | 0.118 | 通る(色数では区別できない) |
| 主役なし | 34 | 0.071 | 落ちる(non-modal) |
| 床なし | 3 | 0.060 | 落ちる(両方) |
| CC-1 の前の配置の再現 | 96 | 0.0715 | 落ちる(non-modal) |

CC-1 の前の絵は古い commit を build せずに、今の engine で CC-1 の前の配置を描いて上下を反転した再現で、当時の engine の
実物ではない。10-02 の緑の run の vrm の測定値は log に残っていない。

**出荷 sample の閾値の余裕**(EB-5b の実装者の 7 project の run。160x90 の frame 10。animation は 1920x1080 の changed):

| project | distinct / 最小 | 比 | non-modal / 最小 | 比 | animation |
|---|---|---|---|---|---|
| animgraph_demo | 24 / 20 | 1.20 | 0.1094 / 0.08 | 1.37 | 27,035 |
| example | 3127 / 1000 | 3.13 | 0.9612 / 0.90 | 1.07 | — |
| pathtrace_demo | 4676 / 350 | 13.4 | 0.8512 / 0.50 | 1.70 | — |
| posegraph_demo | 25 / 20 | 1.25 | 0.1078 / 0.08 | 1.35 | 55,444 |
| raytracing_demo | 817 / 350 | 2.33 | 0.8367 / 0.50 | 1.67 | — |
| sprite_demo | 58 / 20 | 2.90 | 0.0412 / 0.02 | 2.06 | — |
| vrm_xr_demo | 36 / 20 | 1.80 | 0.1240 / 0.09 | 1.38 | 2,556 |

余裕が薄いのは animgraph_demo / posegraph_demo の色数(+4 / +5 色)と example の non-modal(+0.06)。絵が少し変わる変更で
ここが先に赤になりうる。EB-5b の確かめは worktree で script を `cmake -P` で直接流したもので(worktree では smoke が登録されない)、
primary の ctest 経由の実行は着地の時点で未確認。

#### package install smoke の照合(EB-5b)

`package_install_smoke`([script](../test/run_package_install_smoke.cmake))は、`list_render_features` の catalog の entry を、
鍵の並びを固定した正規表現で照らしていた。FR-1 の `required_targets`・FR-2 の `insertion` が entry に入ると外れた
(production の答えは正しかった)。1 つ直すと、壊れた package の entry の `"available":false,[^}]*"source":"package"` が
`insertion` の `}` で止まって次で落ちることも実物で確かめた。EB-5b から、transcript の行を JSON として読み、top-level の `id`
の行の `result` から `reference` の一致する entry を取り出して、欄を名前で比べる(`rpc_result` / `catalog_entry` / `require_field`。
[欄を名前で比べる関数](../test/run_package_install_smoke.cmake))。`source_digest` の照合も同じ形にした。
部分文字列の有無だけを見る照合(`"status":"accepted"` など)は並びに依らないので残した。

**`ctest` が緑でも gate は落ちうる。** それがこの gate の存在理由である。2026-07-31 の実行では
`ctest` が「122 件中 0 失敗」と報告した一方、gate は 4 件の非許可 skip を検出して FAIL した
(詳細は `gpu_skip_allowlist.txt` のコメントと WP241)。skip されたテストは赤くならないので、
落ちたテストより見つけにくい。

WP241 はこの4件を allowlist に加えず修正した。2026-08-01 の再実行は GPU 125/125、
非許可 skip 0、`SKIP exact policy: PASS`。現在は外部corpus未指定のproject catalogに加え、
Releaseで実行できないDebug専用WSI故障注入1件をallowlistに記録する。正確なテスト名は
`wsi_fault_window_player`で、`test/ci/gpu_skip_allowlist.txt`が正本である。Releaseでは明示skipし、Debug故障注入の検証済みとはしない。
専用の`vulkan_validation_runtime_player`が行うvalidation on/off対照とは別の境界である。
WP241で除いた例外catchは戻していない。

Releaseのruntimeログもファイルとstderrへ出力する。StudioのEngine Logやテストの捕捉stderrへ
診断が届くため、ファイルだけを見ていない呼び手もエラーを検出できる。JSON-RPCのstdoutはログに使わない。

GPU の無い機械で走らせると大半が skip して gate は落ちる。これは意図どおりで、
上の「fail-on-no-GPU」の実体である。その場合は「no Vulkan device」の診断行が先頭に出る。

Pythonはエンジン／ゲームの実行依存ではない。`BUILD_TESTING=OFF`、または
`PELICAN_PYTHON_TESTS=OFF`（既定）では、Python製テスト用の探索を行わない。
手元で任意登録する場合だけ`AUTO`、完全なgateを要求するCI0/clean-clone/releaseは
`PELICAN_PYTHON_TESTS=ON`でPython 3.11以上を要求する。Studio E2Eの登録もこの条件を使う。
ただしOpenXR-SDK-Sourceのdispatch header生成は
このテスト設定と独立してPythonを必要とする場合がある。Pythonなしの構成では
`PELICAN_WITH_OPENXR=OFF`と`PELICAN_WITH_SPIRV_LINK=OFF`も指定する。
Python製CTestには`python`ラベルを付け、
実行時は`-B` / `PYTHONDONTWRITEBYTECODE=1`でsource treeへ`__pycache__`を作らない。
GPU lease 用 Catch discovery の書き換えは CMake だけで行い、Python OFF 構成でも有効である。

experimental SPIR-V linkerは`PELICAN_WITH_SPIRV_LINK=OFF`が既定である。ON時だけ
pinned SPIRV-Toolsと`pelican-spv-link`をbuild graphへ加える。この依存の生成処理も
Pythonを使うため、完全なCI gateはPython testと同時にON、通常／配布buildはOFFにする。

### 配布物の確認(`run_distribution_check.py`)

Runtime install を利用者と同じ形で確かめる。PATH を install の `bin` と System32 だけにして、
開発 build や Qt SDK の DLL が欠けたファイルの代わりにならないようにする。出荷 sample(`projects/` の
`project.json` を持つ directory すべて)ごとに、install の Studio で複製を開き、最初の frame を待って
`scene_tree` / `get_frame_plan` / capture、別 project へ切替えて戻し、再度 capture し、Frame Plan dock が失敗表示で
止まっていないことを見て、終了コード 0 で終わることを確かめる。そのあと project ごとの player log があること、
Studio 終了時に生きていた player が surface を作り直し続けていないこと(`ErrorSurfaceLostKHR` 3 件以下)を見る。
最後に `pelican_cli project init` の新規 project を editor RPC で編集、File メニューで保存、Studio を完全に
終了・再起動して値が残ることを確かめる。

```powershell
cmake --install build-v0 --config Release --component Runtime --prefix <絶対パス>/build-v0/stage
build-v0/test/Release/pelican_gpu_lease_run.exe -- python -B test/ci/run_distribution_check.py `
  --stage build-v0/stage --artifacts build-v0/test_artifacts/distribution
```

他の GPU 作業と同時に走らせうるので GPU lease の下で起動する。`--sample <名前>`(繰り返し可)で sample を絞り、
`--skip-new-project` で新規 project の確認を外せる。結果は `--artifacts` の下の `run-<時刻>-<pid>/report.json` に
sample ごとの成否・失敗理由・capture とともに書く。engine は最初の frame より先に RPC に答えるので、
確認は automation の `wait_engine` に `frames: 1` を付けて描画済みの frame を待つ。

## `gpu` ラベル

CI0 の選択条件はテスト名の grep や workflow 内のテスト一覧ではなく、CTest の `gpu` ラベルだけである。
Vulkan device を初期化する Catch2 executable は
`pelican_define_test(<name> GPU ...)` で登録する。player / GPU script を直接 `add_test` する場合は、
その登録直後に `set_tests_properties(<name> PROPERTIES LABELS gpu)` を置く。

GPU を使う新規テストにラベルを付け忘れると device のない CI0 で赤になる。GPU 不在を通常成功として
扱うテストを CI0 に混ぜない。CPU-only の shader compiler / SPIR-V golden は名前に `golden` があっても
`gpu` にはしない。

## SKIP exact policy

CTest の JUnit 出力に `Skipped` / `Not Run` が一件でもあれば、
`test/ci/cpu_skip_allowlist.txt` の完全一致名だけを許可する。ワイルドカード、重複、CPU gate に存在しない
古い allowlist 名は policy error になる。現在の許可は Windows の directory symlink 作成権限に依存する
次の一件だけであり、権限がある runner では通常どおり PASS してよい。

```text
PathResolver rejects symlink escapes after canonicalization
```

許可を追加するときは、CPU 環境で本質的に実行不能である根拠をレビューし、CTest の完全一致名を一行だけ
追加する。Vulkan 不在や実エラーを allowlist に入れない。policy checker 自体は次で検査できる。

```powershell
python -B -m unittest discover -s test/ci -p "test_*.py" -v
```

## 失敗時の調査

各 run は成功・失敗に関係なく `cpu-gate-<run id>-<attempt>` artifact を14日保存する。内容は
configure/build/CTestログ、CTest JUnit、SKIP policy 結果、`LastTest.log`、CMake configure log である。
初回失敗を artifact に残して赤のまま扱い、再実行で成功へ上書きする処理は設けない。

## CI1: 構成 matrix と clean clone

`.github/workflows/configuration-smoke.yml` は毎週土曜 16:17 UTC（日曜 01:17 JST）と
`workflow_dispatch` だけで実行する。pull request/push の常設 CPU gate には連結しない。

構成 matrix は Windows/MSVC Debug で次を独立 runner に分ける。

- [`cmake/pelican_feature_registry.cmake`](../cmake/pelican_feature_registry.cmake) が生成する
  全 feature contrast。各行は registry の共通基準から対象だけを対照値へ動かし、
  Jolt 等で連動する値も registry 宣言に限定する。[ビルド機能台帳](build_features.md) が生成表で、
  configure と契約テストはこの表を検証する。WP の巨大な実装履歴は読み込まない
- `PELICAN_PROJECT=projects/example` の project-code build と、tracked fixture だけで作る
  一時 project の headless smoke

build-unit entry は同じ `test/run_build_units_smoke.cmake` に
`PELICAN_BUILD_UNIT_SMOKE_ONLY` を渡す。matrix の `fail-fast` は false で、ある構成の赤が
ほかの構成の一次結果を隠さない。自動 retry はない。各 entry の CMake 診断と smoke log は
`configuration-smoke-<entry>-<run id>-<attempt>` artifact に14日保存する。
build-unit nested configureはC++のOFF機能probeを使うため`BUILD_TESTING=ON`のまま
`PELICAN_PYTHON_TESTS=OFF`とする。基準の`PELICAN_WITH_SPIRV_LINK=ON`にはPython、
基準の`SKIP_DEVSTUDIO=OFF`にはQtが必要なのでworkflowが両方を用意する。
project-code nested configureは別の製品境界であり、従来どおり`BUILD_TESTING=OFF`、
`SKIP_DEVSTUDIO=ON`、`PELICAN_WITH_SPIRV_LINK=OFF`、
`CMAKE_DISABLE_FIND_PACKAGE_Python3=TRUE`とする。

clean-clone job は `actions/checkout` の clean checkout を使い、最初に
`projects/example/assets` の ignored binary が 0 件であることを機械確認する。その後、
golden inventory、CI policy checker、configure、全 Debug build、
`test/ci/run_cpu_gate.py`（`ctest -LE gpu` + exact SKIP policy）を順に実行する。
許可 SKIP は CI0 と同じ `test/ci/cpu_skip_allowlist.txt` だけであり、CI1 固有の
allowlist は作らない。失敗 artifact と retry なしの規律も CI0 と同じである。

大容量 example asset の取得、ライセンス、provenance は
[`example_assets.md`](example_assets.md)を正とする。CI1 の一時 project と CPU test は
その 22 binary を使わない。

## 配布の構成(hot reload OFF)の test

`PELICAN_WITH_HOT_RELOAD`(AV-1 `5f7c5cdb`、既定 ON)は OFF で監視・DLL の poll・起動時の asset の照合を止める。
`pelican_cli dist-config` は常に OFF を出すので、**配布する構成は OFF** である([ビルド機能台帳](build_features.md))。

- **CI が見ているもの**: CI1 の構成 matrix の `PELICAN_WITH_HOT_RELOAD` の行は、OFF の tree を `BUILD_TESTING=ON` で全体 build し、
  player の `--strict-assets` が照合して止まることと、`--rpc --headless` の `get_status` が監視と照合を止めた理由を言うことを見るだけである
  ([hot reload の smoke](../test/run_build_units_smoke.cmake))。**OFF の構成で ctest は回していない**。CI0 と clean-clone は既定(ON)の構成。
  CI が OFF の test を回すようにはしていない(EB-4 の実装者の指摘。workflow は変えていない)。
- **手元で回した結果(EB-4 `5df8b9d2`、2026-10-05)**: OFF の別 build(`-DPELICAN_WITH_HOT_RELOAD=OFF -DSKIP_DEVSTUDIO=ON`、ほかは
  Debug / Ninja / `PELICAN_WITH_SPIRV_LINK=ON` / `PELICAN_PYTHON_TESTS=ON`)で、非 GPU の全数を変更前に 1 回回すと 1,917 件中 11 件が赤だった。
  - OFF が原因の 7 件: 6 件(HW-1 の gate 2 本、central gate、AV-1 の 3 本)は gate が開く・監視が動く・起動時の照合が走ることが前提の test で、
    OFF では `SKIP("PELICAN_WITH_HOT_RELOAD=OFF: …")` の名前付き skip にした(Catch2 の SKIP は ctest で Skipped。登録は変えない)。
    WP331 は期待する理由の文字列だけが違ったので、構成で切り替えた。
  - GPU は OFF で意味の変わる 38 件を回して 4 件が赤: F-5 の `reload_shaders` は期待を構成で切り替え、EB-2 の watch と HW-1 の texture の
    watch は名前付き skip、`devcli_assets_command` は `-DHOT_RELOAD` を受けて OFF の期待(strict でない起動に `assets manifest:` の行が出ない)を見る。
  - **名前付き skip は合計 8 件**(非 GPU 6、GPU 2)。skip の代わりに配布の構成の契約を見る、**OFF でだけ登録する test を 3 本**足した:
    filewatcher の gate(どの宣言・driver でも開かず理由は `hot reload build disabled`)、`ReloadService::setup()` の本番の道の 6 通り
    (`state: disabled`)、起動時の照合(strict でなければ `not_run`、`verify_assets` は走る、strict は照合して止まる)。
  - OFF でだけ落ちる production の不具合は無かった。残りの 4 件は ON でも赤の develop の既存の赤で、EB-5a(`7725297f`)で直した(下の「全数の記録」)。
  - 変更後に OFF の非 GPU 全数は回し直していない(触った家族と元の赤の 15 件 → 緑 9 / 名前付き skip 6 / 赤 0、GPU の 4 件 → 緑 2 / skip 2)。
    ON の構成では、変えた test が今までどおり登録され緑であることを確かめた。
- **OFF でも毎 frame 動くもの**(EB-4 の実装者の報告。直していない): `src/core/ecs/predefined/animationsystem.cpp` の
  `AnimationSystem::updateGraphs()` は、animation graph を持つ entity ごとに毎 frame `last_write_time` で file を見て、変わっていれば読み直す
  (開発用の hot reload と同じ働きで、配布でも毎 frame の stat)。`src/core/audio/audio.cpp` の `prepareSound()` は decode の cache の鍵に
  file の更新時刻と大きさを使う(再生のたびに stat。poll ではない)。
- **golden の orbit の case**(EB-4): golden の test の exe は `pelican_core` を `/WHOLEARCHIVE` せずに link するので、built-in の camera controller の
  登録が落ち、この case の golden はずっと既定の camera の絵だった。何もしない anchor `internal::linkBuiltinCameraControllerSystem()` を harness から
  呼ぶようにし、golden と hash を作り直した。player は whole-archive なので本番は無事。

## 全数の記録

全数(非 GPU・GPU・Studio の scenario)は着地ごとには回さず、段の終わりに回す。下は「その時点の途中の基準」であり、
release の gate(上の GPU gate と [v0 の検証記録](v0_freeze.md#6-v010-tag-の手順))の代わりにはしない。

### 2026-10-05(develop `31c390c7`、統合側の run)

primary の Debug build(Studio ON)。log は統合側の `<作業ディレクトリ>/int_full_1005/`。

| 部 | 結果 | 赤 |
|---|---|---|
| 非 GPU(`-LE gpu`) | 2,663 / 2,667(skip 1 は既存の symlink 権限) | 4 件 → EB-5a `7725297f` |
| GPU(scenario を除く) | 397 / 399 | `package_install_smoke`、`project_catalog_headless_smoke` → EB-5b `85d0b06b` |
| Studio の scenario | 80 / 82 | `gui_build_C`(→ `85d0b06b` で期待を直した)、`gui_rt_full_from_blank`(間欠。QA-1 で対応中) |

計 3,148 件。直した後の全数は回していない(この文書の時点)。

**家族の外の赤**(2026-10-04〜05 に見つかったもの)。どれも、壊した WP が自分の家族の test だけを回して着地し、全数を回していなかったので、
後の WP や全数で初めて見えた。production が正しく test が追従していなかったものと、production の後退の両方がある。

| test | 壊した commit | 見つけた所 | 中身 | 直した commit |
|---|---|---|---|---|
| `WP332 catalog and apply share the engine availability decision` | PB-1 `a55d1eaf` | RTL-1c の実装者 | `render_feature_requires_restart@1` の拒否が運ぶ形(`<code>: {JSON}`)になったのに、test が catalog の人の文と文字列で比べていた | RTL-1c `8034d096`(test) |
| `WP240c project material names are unique across documents` | FO-1 `a4d7760a` | EB-2 の実装者 | 起動時に登録できない文書を外して起動するようになったのに、test が throw を期待していた | EB-2 `e1ba9058`(test) |
| `RT6 named TLAS tasks select independent scene subsets with shared records` の 1 section | FO-1 `a4d7760a` | EB-3 の実装者(GPU の家族) | **production の後退**。FO-1 の握り方が広すぎて、pass が `ray_query` を宣言していない(pass 側の原因)のを「material 文書を読めない」として起動を続けた | FO-2 `31c390c7`(production と test) |
| `renderer-owned WP354 append covers every family …` | RTL-1b `8034d096` | 10-05 の全数 | frame plan に足した `named_tlas_blas` が基準の fixture に無かった | EB-5a `7725297f` |
| `logical shadow compilation preserves every existing frame plan` | RTL-2s `c8fd51cb` | 10-05 の全数 | 総なめが graph root の rendering 節だけを legacy 形で読み、lowering の作る target を読む Studio の fixture で落ちた | EB-5a |
| `frame planner preserves existing rendering config order` | RTL-2s `c8fd51cb`、GL-5e `74767e46` | 10-05 の全数 | 同上と、project の feature だけが宣言する target に描く fixture | EB-5a |
| `WP357 shipping fullscreen manifest keeps only the three bloom upsample blends explicit` | FR-4 `2eb08d21`(10-03) | 10-05 の全数 | features の `required_targets[].type` が object になったのに、test の走査が全 object の `type` を文字列で読んだ | EB-5a |
| `package_install_smoke` | FR-1 `45ca769b` / FR-2 `0b15fb62` | 10-05 の全数 | catalog の entry の鍵の並びを固定した正規表現(上の「package install smoke の照合」) | EB-5b `85d0b06b` |
| `project_catalog_headless_smoke` | CC-1 `c8367b87`(閾値)、MP-1 `afd923e1`(editor lane)、be35b7de8(09-22、死んだ検査) | 10-05 の全数 | 上の「project catalog smoke の検査の中身」 | EB-5b |
| `studio_scenario_gui_build_C` step 116 | GL-5e `74767e46` | 10-05 の全数 | 見せている入力のソケットの名前に付く ` (shown)` を期待が知らなかった | `85d0b06b`(統合側が json の 1 行) |
| `studio_scenario_gui_rt_full_from_blank` step 55 | (競合) | 10-05 の全数 | combo の選択肢が engine の答えで埋まる前に選んだ(`has no entry Every Sphere`)。単独では緑 | QA-1 で対応中(統合側の記録) |

EB-5a の 4 件はどれも、直前の非 GPU 全数(10-02)より後の commit で壊れた(EB-5a の実装者の補足)。GPU の 2 つの smoke は、
最後の緑が 10-02 04:07 の全数(CC-1 の前。FR-1 / FR-2 の前)で、その後に全数が回ったのは 10-05 が初めてだった(EB-5b の実装者の調べ)。

## CI1 をローカルで再現する

build-unit smokeはQtとPython 3.12、project-code smokeはどちらも不要である。
clean-cloneの完全なCPU gateにもPython 3.12を用意する。

```powershell
# registry の全対照を一括実行。個別実行は -DPELICAN_BUILD_UNIT_SMOKE_ONLY=audio 等を追加する。
cmake -DPELICAN_BUILD_UNIT_SMOKE_CONFIG=Debug `
  -DCMAKE_PREFIX_PATH=<Qt install path> `
  -DPython3_EXECUTABLE=<Python 3.12 executable> `
  -P test/run_build_units_smoke.cmake

cmake -DPELICAN_PROJECT_CODE_SMOKE_CONFIG=Debug `
  -P test/run_project_code_smoke.cmake

cmake -S . -B build-clean-clone -DSKIP_DEVSTUDIO=ON `
  -DPELICAN_PYTHON_TESTS=ON `
  -DPELICAN_WITH_SPIRV_LINK=ON
cmake --build build-clean-clone --config Debug
python -B test/ci/run_cpu_gate.py `
  --build-dir build-clean-clone `
  --config Debug `
  --artifacts-dir build-clean-clone/ci-artifacts
```
