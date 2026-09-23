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
| CPU gate(2026-09-23、`1bed0335`) | 1,793 件中 1,792 PASS、allowlisted skip 1 件、失敗 0。skip は directory symlink 作成権限が必要な `PathResolver rejects symlink escapes after canonicalization`。`v0-cpu-final4/` の exact SKIP policy も PASS |
| 全 GPU gate(2026-09-23、`1bed0335`) | 274 件中 273 PASS、allowlisted skip 1 件(Debug 専用の `wsi_fault_window_player`)、失敗 0。実窓 Studio scenario 10 件はすべて観測・通過。exact SKIP policy PASS。artifact は `v0-gpu-final4/` |
| Studio CPU gate(2026-09-23、`a0e7820d`、以後 Studio の code は無変更) | compiler ON の release 構成なので空の allowlist で実行。263 件中 263 PASS、skip 0、exact SKIP policy PASS。artifact は `v0-studio-final3/` |
| 全 GPU gate の経緯 | 09-22 初回は 4 失敗(`v0-gpu-final/`)、修正後の再実行は `studio_scenario_graph_observe_roundtrip` の 1 失敗(`v0-gpu-verified/`)。後者は viewport の resize による render pipeline の再 lowering が editor の遷移 epoch を動かし、受付済みの編集を `gate_closed` で捨てていた。`7652fb09` で遷移 epoch を participant の reload 結果だけから作るよう修正 |
| 配布 stage(2026-09-23、`1bed0335`) | Runtime install から出荷 sample 7 / 7(pathtrace_demo を追加)の起動、別 project 経由の再読込、capture、正常終了が成功。新規 project の位置編集・保存・完全終了・Studio 再起動でも authoring ID と保存値の持続を確認 |
| Debug 専用(2026-09-23、`1bed0335`) | 同じ tree の Debug build で `wsi_fault_window_player` と `crash_symbol_production_wiring` が通過 |

gate の artifact は `build-v0/test_artifacts/` 以下。配布物の操作ログと詳しい確認範囲は
[v0 の検証記録](v0_freeze.md#6-v010-tag-の手順) を参照する。CPU / GPU gate、配布 stage、Debug 専用の確認は
`1bed0335` で通過した。以後の revision で tag を付けるときは、その revision でやり直す。

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

`test/studio_scenarios/*.json` の全 scenario（現在10件）を正確な CTest 名で必須とし、
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
