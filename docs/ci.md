# CI 運用: CPU gate (CI0) と構成/clean-clone smoke (CI1)

## 役割

GitHub Actions の `CPU gate` は Windows/MSVC の configure、Debug 全ターゲット build、
GPU 不要 CTest を pull request ごとに検査する。統合ブランチ
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

## ローカルで同じ gate を実行する

Python 3.12 と Vulkan SDK 1.4.x を PATH / `VULKAN_SDK` から参照できる状態で実行する。
Python の場所は環境ごとに異なるため、リポジトリ内に絶対パスを保存しない。

```powershell
cmake -S . -B build -DSKIP_DEVSTUDIO=ON
cmake --build build --config Debug
python test/ci/run_cpu_gate.py `
  --build-dir build `
  --config Debug `
  --artifacts-dir build/ci-artifacts
```

最後のコマンドは次の CTest と SKIP policy 検査を一体で行う。

```powershell
ctest --test-dir build -C Debug -LE gpu --output-on-failure
```

GPU 対象だけを確認する場合は `ctest --test-dir build -C Debug -L gpu -N` を使う。

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
python -m unittest discover -s test/ci -p "test_*.py" -v
```

## 失敗時の調査

各 run は成功・失敗に関係なく `cpu-gate-<run id>-<attempt>` artifact を14日保存する。内容は
configure/build/CTestログ、CTest JUnit、SKIP policy 結果、`LastTest.log`、CMake configure log である。
初回失敗を artifact に残して赤のまま扱い、再実行で成功へ上書きする処理は設けない。

## CI1: 構成 matrix と clean clone

`.github/workflows/configuration-smoke.yml` は毎週土曜 16:17 UTC（日曜 01:17 JST）と
`workflow_dispatch` だけで実行する。pull request/push の常設 CPU gate には連結しない。

構成 matrix は Windows/MSVC Debug で次を独立 runner に分ける。

- `PELICAN_WITH_AUDIO/VAT/EXR/RPC/SEQPLAYER/IMGUI/OPENXR/RENDERDOC=OFF`
- `PELICAN_WITH_PHYSICS=OFF` と built-in/Jolt provider の排他構成
- `PELICAN_PROJECT=projects/example` の project-code build と、tracked fixture だけで作る
  一時 project の headless smoke

build-unit entry は同じ `test/run_build_units_smoke.cmake` に
`PELICAN_BUILD_UNIT_SMOKE_ONLY` を渡す。matrix の `fail-fast` は false で、ある構成の赤が
ほかの構成の一次結果を隠さない。自動 retry はない。各 entry の CMake 診断と smoke log は
`configuration-smoke-<entry>-<run id>-<attempt>` artifact に14日保存する。

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

Python の絶対パスはローカルコマンドにだけ渡し、workflowやCMake scriptには保存しない。

```powershell
$python = 'C:\path\to\python.exe'

# 9 entry を一括実行。個別実行は -DPELICAN_BUILD_UNIT_SMOKE_ONLY=audio 等を追加する。
cmake -DPELICAN_BUILD_UNIT_SMOKE_CONFIG=Debug `
  "-DPython3_EXECUTABLE=$python" `
  -P test/run_build_units_smoke.cmake

cmake -DPELICAN_PROJECT_CODE_SMOKE_CONFIG=Debug `
  "-DPython3_EXECUTABLE=$python" `
  -P test/run_project_code_smoke.cmake

cmake -S . -B build-clean-clone -DSKIP_DEVSTUDIO=ON `
  "-DPython3_EXECUTABLE=$python"
cmake --build build-clean-clone --config Debug
python test/ci/run_cpu_gate.py `
  --build-dir build-clean-clone `
  --config Debug `
  --artifacts-dir build-clean-clone/ci-artifacts
```
