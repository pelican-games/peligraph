# WP221 top-level render compiler program 実装レポート

日付: 2026-07-29

状態: **内部runtime slice完了**

## 結論

flat/XRのrender pipeline compileを、一つの`RenderCompilerProgram`がvariant family単位で
制御する構造へ移した。既定利用者の設定は増えず、null selectionでは従来の
logical resolve / strategy / transform / subgraph / Vulkan target planning列を使う。

共通plannerは必須言語ではなく、built-in programが再利用する標準ライブラリである。
source-level custom programは次のどちらも選べる。

- built-in programへ委譲し、前後または一部のalgorithmだけを変更する
- 共通plannerを呼ばず、Vulkan固有のphysical packageを直接構築する

どちらも別runtime bypassを作らず、同じbackend package verifier、GPU registration arena、
prepared generation、単一publication、rollback / retireへ収束する。

## 実装した境界

### 一つのprogram root

`RenderCompilerProgram::compile()`は、同時公開するvariant request列、render config、
provider registry snapshot、path resolver、runtime compiler availability、backend contextを
一回で受け取る。program selectionは次を持つ。

- schema version
- program name
- implementation id
- openなbackend identity
- `portable | mixed | backend_native` mode

selectionはprogram出力を信用して保持するのではなく、engine hostが検証後の
`CompiledRenderPipeline` cloneへstampする。

### open backend package

共通`RenderCompilerBackendContext` /
`RenderCompilerBackendPhysicalPackage`に、閉じたbackend enumや共通最小公倍数のphysical
nodeを置いていない。Vulkanは別の`VulkanRenderCompilerBackendContext` /
`VulkanRenderCompilerPhysicalPackage`で次を所有する。

- output format / extent / physical device context
- physical確定後のrender-target definition
- `RenderingTargetPlanCompilation`
- graph名からimmutable `VulkanTargetPlan`へのindex

future Metal backendは兄弟packageを追加でき、Vulkan formatやscopeへ変換する必要がない。
共有したいcandidate planning / cost logicだけを共通ライブラリとして利用できる。

### engine-owned transaction

programへ移していない責務は次である。

- provider generation snapshotの取得とlease
- backend/device contextの取得
- GPU registry checkpoint / rollback
- runtime pass / pipeline / descriptorのprepare
- generation validationと単一publication
- in-flight lease後のretire

custom programはGPU objectやlive engine moduleを返さない。backend-nativeであっても
immutable CPU packageを返す。

## verifier

GPU mutation前に次をhard errorにする。

- selection schema、name、implementation、backend、modeの不正
- runtime backendとselection / physical package backendの不一致
- variant数、順序、compiled graph variantの不一致
- null pipeline / physical package、非object normalized config
- buffer definitionと名前indexの不一致
- frame planの空名、key/name不一致
- Vulkan target planのnull、空名、重複
- compilation planと名前indexの欠落またはpointer identity不一致
- frame graph集合とVulkan target graph集合の不一致
- Vulkan render-target definitionの空名、重複

後段のpass implementation / shader / GPU object検証は既存のrollback可能なprepare内で
引き続き行う。

## 使い勝手

普通のprojectは何も指定しない。`render_compiler_program == nullptr`がbuilt-in
`engine.default / builtin.default_vulkan_v1 / mixed`を選ぶ。

engine改造者は`RenderCompilerProgram`を一つ実装し、
`RenderingPassConfigRegistrationDependencies::Options`へpointerを渡す。同一publicationの
flat/XRは同じprogram objectでなければならないため、variantごとの隠れたalgorithm混在はない。

programが全variantを一度に見るので、flat/XR間の共通resource契約、将来のMSAA / mirror /
upscale variant、program-wide cost判断を一つの制御algorithmに書ける。helperの実装ファイルを
分割しても、program rootから見た処理は一本のままである。

現時点の境界は内部C++ source seamである。game DLLへC++ virtual interfaceを公開せず、
ABI version、owner lease、noexcept status、reload fixtureが揃ってから別の公開ABIを設計する。

## 検証

Debug / OpenXR ON buildで次を確認した。

- `rendercompilerprogram_test`: 13 assertions / 2 cases
  - common plannerを呼ばないbackend-native Vulkan package
  - trusted provenance stamp
  - backend mismatch / target graph index mismatch reject
- `renderpipeline_resolve_test`: 140 assertions / 15 cases
- `renderingpass_helpers_test`: 480 assertions / 67 cases
- `renderingsamplecount_test`: 181 assertions / 18 cases
- headless `[hybrid]`: 90 assertions / 1 case
  - built-in programで既存hybrid描画を維持
- headless `[gpu-arena]`: 107 assertions / 1 case
  - custom delegating programをregistrationへ注入
  - compile invocationが一回
  - published pipelineのprogram provenance
  - fault rollback、scope replacement、generation-owned resource retireを維持

## 意図的に残した境界

1. preview compilerはまだ別経路であり、flat/XRと同じinvocationへ未統合
2. backend-native programも現runtime adapter向けnormalized config、frame plan、
   buffer / compute definitionを返す必要がある
3. complete raw Vulkan physical plan builderは未公開
4. `NativeScope`とraw Vulkan command/resource ownership contractは未実装
5. Metal context/packageと共有planner fixtureは未実装
6. CPU / external physical packageとheterogeneous execution linkerは未実装
7. coordinated graph + surface + material pipeline hot reloadは別候補
8. stable game-DLL compiler-program ABIは未設計

## 次の順序

1. previewを同じprogram rootへ入れ、variant familyの完全性を揃える
2. graph + surface + material pipelineのcoordinated candidateをprogram output transactionへ
   統合する
3. 具体的なVulkan-only使用例からcomplete raw package verifierまたは`NativeScope`を設計する
4. Metal着手時に兄弟backend packageを追加し、どのplanning helperが本当に共有できるか
   fixtureで決める
5. CPU/external taskの具体例が二つ以上得られてからexecution linkerを実装する
