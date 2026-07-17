# CI 運用: CPU gate (CI0)

## 役割

GitHub Actions の `CPU gate` は Windows/MSVC の configure、Debug 全ターゲット build、
GPU 不要 CTest を pull request ごとに検査する。統合ブランチ
`codex/rendering-phase1-refactor` への push でも実行し、同じ pull request / ref の古い run は
`concurrency.cancel-in-progress` で中止する。テストや build の自動 retry は行わない。

CI0 は CPU 回帰の常設 gate であり、次は引き続きローカル gate である。

- 全 CTest (`gpu` ラベルを含む)
- golden image と final RGBA8 byte gate
- player の短時間起動確認
- feature OFF / build-unit / project-code smoke (将来の CI1)

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
