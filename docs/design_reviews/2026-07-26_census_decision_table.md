# コード品質センサス: 決定表(判断待ち)

日付: 2026-07-26
入力: センサス 3 本 — 選択肢の棚卸し / 互換・旧経路の棚卸し /
[語彙・型の重複](2026-07-26_census_vocabulary.md)

**本書は判断を含まない。** エージェントには「削除すべき」と書くことを禁じ、証拠だけを
埋めさせた。最後の「判断」列はオーナーが埋める。

## 0. 判断のしかた

各項目は 5 つの問いのうち答えの出たものだけを列に持つ。**難しいのは最後の 1 問だけ**:

1. 公開面から到達できるか
2. 実際に何通りの値が使われているか
3. テストがどの分岐を踏むか
4. golden に現れるか
5. **消せない理由が「再現できない環境条件」か** ← ここだけが本当の判断

環境条件で守られているもの(デバイス喪失・OOM・ドライバ癖・HMD 有無・
ネットワークドライブ)は §5 に分離した。**踏まれていなくても消してはいけない**。

## 1. 本番呼び出しがゼロ(テストのみ)

| 項目 | 証拠 | 判断 |
|---|---|---|
| `applyLoweredMaterial`(2 overload) | 本番 0 / テスト 5。**私も grep で確認** | |
| `applyLoweredMaterialForRoute`(2 overload) | 本番 0 / テスト 5。**新旧どちらの機構も本番から呼ばれていない**。実際の `MaterialInfo` は gltf/seqplayer/standardmaterialresource が既定値で構築 | |
| split sampler 方式(`7+2i`) | 本番 0。ゲートは `getenv("PELICAN_SPV_LINK")` で **src/ 内のどこもセットしない** + CMake 既定 OFF。**私も確認**。なお `shader_contract.md` は split を「標準形」と書くが実態は逆 | |
| `FrameGraphRuntimeContainer::registerExecutionPlan` / `find()` / `findProgram()` | 本番 0 / テスト 1〜6。**ただし `docs/source-code-guide/` は今もこれをランタイム結合の機構として説明** | |
| `lowerMaterialWithSnapshots` | 本番 0 / テスト 2 | |
| `payloadFields` | エンジン 0 / テスト 26。**公開 API なのでユーザゲーム側が使いうる**(`projects/` には使用なし) | |
| `ResourcePatternFallback::reject` | setter が本番 0・テスト 0 | |
| `drawSortInputV1LegacyPrefixSize` | 自身の `static_assert` 以外に参照 0 | |
| `SKIP_TEST`(CMake) | CI・スクリプトで使用 0。`BUILD_TESTING` へ移行済(WP197 記録あり) | |
| `*ForTesting` 7 個 | 本番 0・テスト 0。**うち 2 個は今日の進行中作業で作成**(残骸ではなく仕掛かり) | |

## 2. 実際に 1 通りしか使われていない選択肢(26 件・3 群)

### 2-1. 公開 ABI: エンジンは 1 値しか実装していないがヘッダは複数提供

`src/core/userpublic/animation/abi_v1.hpp` の 6 enum。決定的な証拠:
`animationservice.cpp:1312` が `BlendMode::additive` と `local` 以外の
`AdditiveSpace` を **`invalid_argument` で明示的に拒否**している。
`SidebandPolicy` / `RestFallback` は全呼び出し箇所でハードコード。
`ValueKind` は**一度も選択されていない**。

ただし `src/core/CMakeLists.txt:144` が `userpublic/` を SDK へ配置し、
`animation_abi_dll_test.cpp` が外部 DLL から消費する = **外部から選択可能**。

| 判断 | |
|---|---|

### 2-2. 内部で、代替分岐が一度も構築されない

| 項目 | 証拠 | 判断 |
|---|---|---|
| `VulkanProcessType::compute` | リポジトリ全体で出現 0。`core.cpp:708,778` が else 分岐を保持。**環境条件のガードも見当たらない** | |
| `LogicalPortRelationKind` | `same_extent` 以外を取りえない | |
| `ShaderCompileOptions::entry_point` | 常に `"main"`。しかも**シェーダキャッシュキーに混ぜられている** | |

### 2-3. 文書化された公開オプションだが、authoring での使用が 0

| 項目 | 証拠 | 判断 |
|---|---|---|
| UI の `Align` / `Justify` / `Visibility` | `docs/schemas/pelican.ui.schema.json` に公開・パーサ完備だが、**どの document もこのキーを書いていない** | |
| `render_state.depth_test` / `depth_write` | 全 author が `depth:` 短縮形を使う。**状態自体は使われており、キーが冗長な第二の綴り** | |

## 3. 記録が無い(なぜ入れたか docs に無い)

判断に最も頭を使うのはここ。**思い出せないものが最も消しにくい。**

`applyLoweredMaterial` / `MaterialShaderContract::legacy_gbuffer_v1`(文字列が
docs に一度も出てこない)/ `allow_legacy_type_fallback` /
`queryFormatCapability` の compat 分岐 / `drawSortInputV1LegacyPrefixSize` /
`lowerMaterialWithSnapshots` / `serializeCompiledRenderPipelineMetadata` の
compat という位置づけ / **PlanViewer と CompiledPlanViewer を両方持つ理由** /
`renderer.cpp` の synthesized `fallback_schedule` / 廃止カメラフィールド拒否表の
2 重定義 / `LoweredMaterial::target_pass` の「older tooling」/
`test/material_absolute_override_fixture.hpp` / import manifest の
version 省略許容(`transform_seq` / `scene` / `layout` / `atlas`)

## 4. 型・語彙(判断対象は狭い)

センサスの結論: **型 160 個の大半は正当**。§1.2 の ABI 対(約 16 組)は
engine 型 → 公開 ABI 表現で **ABI 安定性に必須**、§1.1 の内部対もほぼ全てに
情報差があり変換は 1 関数に集約されている。

判断が要るのは以下だけ:

| 項目 | 証拠 | 判断 |
|---|---|---|
| `ResolvedSampleCountResource` / `RenderingSampleCountAssignment` | **唯一「専用変換関数がなく inline が 2 箇所に分裂」している対** | |
| `DebugDrawPassInfo` / `DebugTextPassInfo` | **フィールドが完全一致**。分離理由のコメントなし | |
| `RenderingTarget*Assignment` 4 型 | 全て `resource` + 値 1 個。値の型だけが違う。理由コメントなし | |
| `LogicalGraphTransformSelection` / `RenderStrategySelection` / `LogicalSubgraphReplacementSelection` | 共通 provenance を 3 型に重複定義。理由コメントなし | |
| `PlanningDiagnostic` / `BackendConstraintFailure` / `PlanningDecision` | `id`/`subject`/`detail` 共通。理由コメントなし | |
| `LogicalFrameNodeInvocation` | **名前が `Logical` だが physical scope plan から生成**(命名の誤り) | |

**命名規約の実態**: `Logical*` / `Target*` / `VulkanPhysical*` / `*Runtime*` は
**既に一貫している**(例外は上記 1 件)。層を示さないのは
`Resolved` / `Compiled` / `Plan` / `Graph` / `Pipeline` / `Pass` / `Program` /
`Request` の **8 語**。規約は半分できており、残り 8 語が層をまたいでいる。

## 5. 環境条件で守られているもの(踏まれていなくても消さない)

`DigestReadStatus`(ファイル監視の競合)/ `WatcherState::degraded`
(ネットワークドライブのハンドル喪失)/ `force_unorm_color_path_for_testing`
(sRGB swapchain が無い環境)/ XR・multiview 関連(HMD が要る)。

**今日 `DEVICE_LOST` の検出コードが「無い」ことが問題になったばかり**である。
この区別を誤ると、次に困るのは自分になる。

## 6. 文書とコードの乖離(修正対象は文書)

| 項目 |
|---|
| `docs/design_scene_format.md` — 旧 scene の自動解釈 + WARN を規定しているが、**コードは throw する**(2026-07-08 の凍結決定に追随済み・文書が取り残された) |
| `docs/shader_contract.md` — split/combined の標準・fallback 表記が実態と逆 |
| `docs/design_build_tiers.md` — `PELICAN_WITH_COMPUTE` / `_INPUT_ACTIONS` / `_HOT_RELOAD` の 3 ユニットを列挙しているが、**CMake にも C++ にも存在しない** |
| `docs/source-code-guide/` — 本番呼び出し 0 の `registerExecutionPlan` をランタイム結合の機構として説明 |

## 7. 方針判断が要る 1 件

**`pelican.vulkan_physical_fragment` が v1・v2・v3 を受理し、v1/v2 を
in-process でアップグレードする。** 3 つの WP204 レポートに意図的と明記されて
おり設計判断だが、**「ランタイムは v1 だけを読む」(2026-07-08 決定)との整合**を
どう扱うか。「新形式は成長期だから例外」とするのか、方針側を更新するのか。

関連: `PELICAN_RUNTIME_SHADER_COMPILER=OFF` はモバイル配布の tier として
文書化されているが、**CI がその構成を一度も組んでいない**(テストが自己無効化
するだけで OFF 経路を検証していない)。WP165 が
`PELICAN_WITH_PHYSICS=OFF` のリンク不能を実測で見つけた前例がある。
