# ビルド機能の検証台帳

機能フラグの基準値・対照値・連動条件を一覧にする。宣言の正本は
[`cmake/pelican_feature_registry.cmake`](../cmake/pelican_feature_registry.cmake) で、
下表は同じ registry から生成する。CMake configure、CI の構成 matrix、
台帳の契約テストはこの小さな文書を読み、WP の実装履歴には依存しない。

**基準値は開発検証用の構成**であり、すべての option の通常既定値ではない。
たとえば `PELICAN_WITH_SPIRV_LINK` の検証基準は `ON`、通常既定値は `OFF`。
対照構成で変更できるのは対象フラグと registry が宣言した連動フラグだけである。

<!-- PELICAN_FEATURE_REGISTRY_BEGIN -->
| フラグ | 基準値 | 対照値 | 対照で連動するフラグ |
|---|---:|---:|---|
| `PELICAN_RUNTIME_SHADER_COMPILER` | `ON` | `OFF` | — |
| `PELICAN_WITH_SPIRV_LINK` | `ON` | `OFF` | — |
| `PELICAN_WITH_AUDIO` | `ON` | `OFF` | — |
| `PELICAN_WITH_VAT` | `ON` | `OFF` | — |
| `PELICAN_WITH_EXR` | `ON` | `OFF` | — |
| `PELICAN_WITH_RPC` | `ON` | `OFF` | — |
| `PELICAN_WITH_ANIMATION` | `ON` | `OFF` | — |
| `PELICAN_WITH_SEQPLAYER` | `ON` | `OFF` | — |
| `PELICAN_WITH_IMGUI` | `ON` | `OFF` | — |
| `PELICAN_WITH_PHYSICS` | `ON` | `OFF` | `PELICAN_WITH_JOLT_PHYSICS=OFF`<br>`PELICAN_WITH_BUILTIN_PHYSICS=OFF` |
| `PELICAN_WITH_OPENXR` | `ON` | `OFF` | — |
| `PELICAN_WITH_RENDERDOC` | `ON` | `OFF` | — |
| `PELICAN_WITH_HOT_RELOAD` | `ON` | `OFF` | — |
| `PELICAN_WITH_STANDARD_RENDER_ALGORITHMS` | `ON` | `OFF` | — |
| `PELICAN_WITH_JOLT_PHYSICS` | `OFF` | `ON` | `PELICAN_WITH_BUILTIN_PHYSICS=OFF` |
| `PELICAN_WITH_BUILTIN_PHYSICS` | `ON` | `OFF` | — |
| `SKIP_DEVSTUDIO` | `OFF` | `ON` | — |
<!-- PELICAN_FEATURE_REGISTRY_END -->

変更時は registry を編集し、リポジトリの root から既存の生成・検証を実行する。

```sh
cmake -P cmake/update_feature_ledger.cmake
cmake -P cmake/verify_feature_registry.cmake
cmake -P cmake/emit_feature_smoke_matrix.cmake
```

`feature_registry_ledger_contract`、`feature_registry_smoke_matrix_contract`、
`feature_registry_mutation_contract` が内容・matrix・不正変更の拒否を確認する。
影響範囲に応じた実ビルドの選び方は [開発運用](v1_development_plan.md)、
CI の実行条件は [ci.md](ci.md) を参照する。
