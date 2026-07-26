# 描画 authoring の使い勝手と回りくどさの棚卸し(v4)

対象読者: feature / material / shader をユーザー空間で書く人、およびその公開面を
実装するエンジン担当。

ステータス: **v4(2026-07-26)**。v1 の指摘を実コード・CPU test・headless Vulkanへ
再照合し、`extent_scale`、custom texture binding、graphics buffer input の事実誤認を
訂正した。v3のWP206a stable selectionに続き、v4ではWP206bのnamed material variantと
project-owned inverted-hull dogfoodによるU2解消を反映した。機構カバレッジは
[`render_mechanism_coverage.md`](render_mechanism_coverage.md)、監査記録は
[`design_reviews/2026-07-26_render_capability_authoring_audit_codex.md`](design_reviews/2026-07-26_render_capability_authoring_audit_codex.md)。

## 0. 評価方法

本書は「書けるか」ではなく、**正しい機能へ辿り着き、変更し、診断し、配布するまでが
扱いやすいか**を評価する。問題を次の4種類に分ける。

| 種類 | 例 |
|---|---|
| authoring | stable nameではなくdraw ordinalを指定させる |
| diagnostics | 合法な候補や最終bindingを知るためコードを読む |
| maintenance | 層と型の所有者が名前から判別しにくい |
| delivery | 開発buildで動くfeatureをshaderc OFFで配布できない |

機構不足そのものはカバレッジ文書で扱い、本書では重複して「使いにくい」と数えない。

## 1. 現在の問題と解消記録

### U1. feature 使用 project の shaderc OFF 配布が未完成

feature instance が存在し、`runtime_shader_compiler_enabled == false` の場合、
composer は起動を拒否する。

- [featurecompose.cpp:1581](../src/project/featurecompose.cpp:1581)
- [design_build_tiers.md:99](design_build_tiers.md)

`dist-config` は存在するが、実使用の stem × define 集合を事前compileして同梱する
`dist-bake` は未実装である。これは「書き味」より重い **delivery blocker** である。
PC開発をshaderc ONで続ける回避策はあるが、Quest/cross-buildや小さい配布物へ進む前に
解消が必要になる。

### U2. 安定したmaterial selectionとmultipass route（解消済み、WP206a/WP206b）

旧`material_range`はdraw-call ordinalであり、scene、sort、visibility、material routeの
変化へ追従できない。この問題に対する通常authoring経路として、materialの`tags`と
material passの`material_filter.include/exclude`を実装した。

- [materialformat.cpp](../src/project/materialformat.cpp)
- [materialpassinfojsonparser.cpp](../src/core/renderingpass/materialpassinfojsonparser.cpp)
- [drawqueuebuilder.cpp](../src/core/renderer/drawqueuebuilder.cpp)
- [materialrender.cpp](../src/core/renderer/materialrender.cpp)

tag membershipは登録順・sort順から独立し、DrawQueueBuilderでcompact rangeへ解決される。
Vulkan executorは文字列を扱わない。preview/XRも同じlogical filterを保持し、plan dumpは
stable filter ID、resolved draw count、unmatched tag、provenanceを表示する。
`material_range`は互換用の低レベルfixtureだけに残し、manualの推奨経路から外した。

同じmaterialを別surface/stateで再描画する経路は、materialの任意名`variants`とpassの
`material_variant`として実装した。別layoutのGPU record/pipelineはruntimeが所有し、
作者はentity/mesh/materialを複製しない。deferred↔forward opaque等の同一phase routeは
自動解決し、opaque/transparent phaseを跨ぐ誤ったsortは登録時に名前付きで拒否する。

残る使い勝手は、material全体ではなくinstance/draw単位で選ぶlayer拡張と、
phaseを跨ぐ必要が実際に出た場合のvariant-aware draw queueである。

### U3. raw fullscreen/compute inputはbinding順を人が合わせる

`.surface`のparams/custom texture/screen inputはgenerated includeが宣言とaccessorを作る。
一方、raw fullscreen/compute shaderはJSONのinput/reads順とset 1 bindingを人が一致させる。

- `.surface`生成: [surfacecompiler.cpp:55](../src/core/shader/surfacecompiler.cpp:55)
- fullscreen binding: [fullscreenpasscontainer.cpp:451](../src/core/fullscreenpass/fullscreenpasscontainer.cpp:451)
- 現manual: [manual/06_rendering.md](manual/06_rendering.md)

順番を挿し替えるとshader compileは通ってもresourceの意味が入れ替わり得る。

**改善方針**:

- logical resource name、descriptor kind、view dimensionからvirtual generated includeを作る
- 通常shaderはnamed accessorを使う
- raw `layout(set=1,binding=N)`はC-layer escape hatchとして維持する
- reflectionとauthoring manifestが不一致ならpipeline作成前にresource名付きでrejectする

### U4. 同じanchorへ挿すfeature間の関係が発見しにくい

`insert: before:/after:`は単なる配列挿入だけではなく、既に明示edgeへ変換される。
pass自身の`after`/`before`もframe plannerで有効である。

- [featurecompose.cpp:1402](../src/project/featurecompose.cpp:1402)
- [frameplanner.cpp:374](../src/core/renderingpass/frameplanner.cpp:374)

したがって「順序を指定できない」は誤りである。残る問題は、独立feature同士が同じanchorへ
挿入され、data edgeもexplicit edgeも無いとき、宣言順がstable tie-breakになることが
feature単体から見えない点である。

**改善方針**:

- 新しいnumeric orderを主契約にせず、既存のnamed `after`/`before`を優先する
- plan dumpにtie-break理由とfeature provenanceを出す
- 同anchor・無関係passは既定では決定的に許可し、strict/CI opt-in warningを出す
- output/resource hazardは従来どおりhard error

これはlogical authoringへ過剰な安全宣言を要求せず、既定を自動・高速に保つ。

### U5. material/geometryだけresource portの表現力が狭い

computeとgraphicsはframe planner内部で同じreads/writes graphへ正規化され、
fullscreenはcompute bufferを読める。文法が違うこと自体は各domainの自然な糖衣であり、
全面改名の理由にはならない。

残る非対称はmaterial passである。

- buffer inputは明示reject:
  [renderingpassvalidation.cpp:17](../src/core/renderingpass/renderingpassvalidation.cpp:17)
- image inputはbuiltin screen semanticsだけ:
  [materialscreeninput.cpp:10](../src/project/materialscreeninput.cpp:10)

**改善方針**:

- `input/output`と`reads/writes`のschema名を無理に統一しない
- compiler IRのtyped portは共通化したまま、material vertex/fragment向けportを追加する
- U3のnamed generated includeと同じWPで縦切りする

### U6. errorとcapability discoveryに候補一覧が不足

`Unknown pass type`、`Unknown format`、unknown material screen input等は、失敗した値は出すが
合法候補や対象deviceで利用できる代替を常に出すわけではない。

**改善方針**:

- static enum/registry由来のerrorは合法候補一覧を含める
- device-dependentなformat/sample/viewは「要求」「device evidence」「fallback候補」を
  physical plan dumpに出す
- feature compose → logical graph → target plan → runtime registrationのどの段で止まったかを
  diagnostic codeで固定する

独立した大規模diagnostic subsystemを作らず、各機構WPのreject testへ候補一覧を含める。

## 2. v1から取り下げた指摘

### R1. `extent_scale`がdisplay RTへ暗黙依存する

取り下げる。実際のrender target allocationはrendererから渡る`base_extent`に対する
output-relative scaleである。

- [renderingpassconfigregistration.cpp:547](../src/core/renderingpass/renderingpassconfigregistration.cpp:547)
- [renderingsamplecount.cpp:504](../src/core/renderingpass/renderingsamplecount.cpp:504)

v1が参照した`frameplanner.cpp:580`はsnapshotのestimated byte-sizeを計算する補助経路で、
実RTのextent決定ではない。upscale用のrender/output resolution contractも既に存在する。

### R2. custom texture binding番号を手計算させている

取り下げる。`.surface` compilerはcustom textureのdescriptor宣言と
`pelican_sample_<name>()`を生成する。

- [surfacecompiler.cpp:55](../src/core/shader/surfacecompiler.cpp:55)

手計算が残るのはU3のraw fullscreen/compute escape hatchである。

### R3. computeとgraphicsが互いに接続できない

取り下げる。fullscreen buffer input、planner上の共通resource edge、実Vulkan fixtureが
存在する。

- [renderingpasstargetjsonparser.cpp:155](../src/core/renderingpass/renderingpasstargetjsonparser.cpp:155)
- [headless_render_test.cpp:131](../test/headless_render_test.cpp:131)

material/geometry portだけをU5として残す。

### R4. feature authoringの入口文書が無い

取り下げる。現在は次が入口として機能する。

- [manual/06_rendering.md](manual/06_rendering.md)
- [adding_features.md](adding_features.md)
- [shader_contract.md](shader_contract.md)

新しい重複referenceを作らず、機構追加時にこの3文書の現在形を更新する。

## 3. 内部の回りくどさ

### M1. plan/graph/pipeline型が多い

logical、target、Vulkan physical、compiled/runtimeを分ける設計自体は必要である。
型数だけをKPIに一括統合すると、ユーザーが求める「論理を自動compileしつつ物理層を
部分的に換骨奪胎する」境界を壊す。

**方針**:

- 横断rename/統合WPは作らない
- `Logical*` / `Target*` / `VulkanPhysical*` / `Runtime*`の層別codemapを維持する
- 新しい意味論を足す所有WP内でだけ重複変換を集約する
- same-layer copy structはcanonical dump/ABI/failure-atomicityの必要性を確認してから減らす

### M2. view execution語彙が近い

scope executionとpipeline view contractは保持する情報が違うため、enumの存在は妥当である。
変換のinline重複が見つかった場合はWP204/XRの所有変更内で1関数へ集約する。独立rename
だけのコミットは作らない。

### M3. design文書のstatusが履歴化している

manual/cookbookは現在形だが、RPE/RGC/HEGの冒頭statusはWP履歴が長い。これはauthoring
blockerではなくmaintenance負債である。

**方針**:

- 現契約を本文、完了証跡を`design_reviews/`と`implementation_archive.md`へ置く
- 機構WPで該当段落を触るときに履歴を移す
- 文書だけの大規模移動と挙動変更を同一commitにしない

### M4. pass kind / material contractは閉集合

現状は「providerで実装を差し替えられるが、authoring kindはv1閉集合」である。
この契約を正確に書き、具体的なcustom geometry dogfoodが出るまで汎用registry化しない。
inverted-hullは既存material kindのpass-local variantで実GPU検証済みである。
新しいcustom geometry workloadが必要になるまで汎用registry化しない。

## 4. 実装計画への反映

使い勝手だけの細切れWPを増やさず、機構の縦切りへ同梱する。

| 順 | mechanism WP | 同時に解消する使い勝手 |
|---|---|---|
| 1 | public shadow contract | resource名、feature provenance、失敗段のdiagnostic |
| 2 | draw tag + multipass material route | ✅ WP206a/WP206bでU2とpass-local surface/stateの発見性を解消 |
| 3 | compute/material typed resource port | U3、U5、named generated include |
| 4 | lighting data v2 + clustered dogfood | fixed light cap、format/capability diagnostic |
| 5 | texture dimension/subresource/sampler | sampler default、合法format/view候補 |
| 6 | indirect dispatch/draw | plan dumpのexecution provenance |
| parallel | `dist-bake` | U1。shaderc OFF delivery |

U4のanchor tie診断とU6の候補一覧は、それぞれの所有parser/compilerを触る最初のWPへ
小さく同梱する。

## 5. 受け入れの書き味

各後続WPは機能テストに加え、次のauthoring gateを持つ。

1. 最小project fixtureがengine内部ID/binding番号を記述しない。
2. copied builtin featureとproject-owned featureが同じ公開契約を使う。
3. errorはresource/pass/feature名、失敗段、合法候補または不足capabilityを出す。
4. `--dump-frame-plan`またはRPC plan dumpだけで最終order/resource/view/providerを追える。
5. feature未参照時は追加pass/resource/variantを持たない。
6. flat/preview/XR/hot reloadの影響範囲を明示し、必要なfixtureを通す。
7. manual/cookbookの既存入口を更新し、重複する新referenceを作らない。
