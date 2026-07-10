# マテリアル B/C 層設計・敵対的レビュー

レビュー日: 2026-07-10  
対象: `design_material_shading.md` v1.1、特に §3-1〜3-9。現行実装と WP59 も照合した。

## 結論

**総合判定: 設計差し戻し。M2 の基礎整備は進めてよいが、現仕様のまま M3
(SPIR-V ABI リンク、4 段フック、engine lib ABI)を本採用してはならない。**

WP59 が証明したのは、特定版の glslang/Slang/SPIRV-Tools と一つの Vulkan 環境で、
人工的に小さい fragment をリンクし、`spirv-val` と pipeline 生成を通せることだけである。
製品 ABI の将来互換性、意味の同値性、性能、デバッグ、web bake は未検証である。

### 重大リスク Top 5

| 順位 | 重大リスク | 判定 |
|---|---|---|
| 1 | **SPIR-V Linkage を中心 ABI に据える根拠が薄い。** SPIRV-Tools 自身が linker を “still under development” とする。スパイクは `OpName` 除去、型装飾除去、dummy main 除去、binding remap まで要求した。これは「薄い糊」ではなく、複数 compiler の出力差を吸収する compiler 中間層である。 | **P0 / 本採用停止** |
| 2 | **フック ABI の版管理と deferred 展望が成立していない。** `inout PelicanSurface` の構造体型は関数 ABI の一部なので field 追加は破壊変更である。さらに §3-7 は独自 BRDF を forward 専用、§3-9 は深さ 3 の同じ `.spv` を deferred に再利用可能としており矛盾する。 | **P0 / 再設計** |
| 3 | **C は「何でもできる」基盤ではなく、B の特権化を防げない。** raw shader を受け取れることと、任意 resource、draw input、attachment、固定機能、stage、同期、engine data を host が供給できることは別である。B template/copy pass/private descriptor だけが使えるなら dogfooding は破れる。 | **P0 / C を先行** |
| 4 | **`screen_inputs` の正しさが未定義。** copy 時点、透明物同士、sort、重なり、MSAA resolve、mipmap、色空間、履歴を決められない。frame graph は pass 間依存を扱えても、同じ pass の draw 途中には snapshot を挿入できない。 | **P0 / M3.5 分離** |
| 5 | **B の「ブログに貼れる」自己完結性は現形式では成立しない。** 実用 snippet は外部 material JSON、生成 params include、shim、lighting stem、texture、compiler flag に依存する。現 M1 parser は v1.1 の主要 key を保持せず、`params` の「宣言順」も実装では key 名順になる。 | **P1 / 単一ファイル形式** |

## 軸別の詳細

### 1. C 層: 「1 ドロー内で何でもできる」はまだ宣言でしかない

現 `MaterialInfo` は vert/frag と固定 4 texture (+ VAT 2)しか持たない
(`src/core/material/material.hpp:26-44`)。MaterialContainer は material pass の format、
depth、cull、vertex layout を固定する (`materialcontainer.cpp:31-43`)。PipelineFactory も
programmable stage を vertex + optional fragment に固定する
(`pipelinefactory.cpp:126-140`)。raw shader と raw pipeline を同一視してはいけない。

発注者が挙げた残り 5 壁の優先順位は次のとおり。

| 壁 | 評価と必要な変更 |
|---|---|
| user SSBO の口 | **P0。** set 3 を `FREE` と名付けただけで、buffer の生成・更新・寿命・access・frame graph resource との対応・descriptor write がない。SSBO に限らず uniform/storage buffer、sampled/storage image、sampler、array を宣言する公開 `resources` manifest が要る。 |
| engine 供給データ | **P0。** set 0 binding 0 は material/shadow で `ObjectBuffer`、fullscreen で `LightUBO` と型が違い、安定 ABI ではない (`shader_contract.md:9-18,69-75`)。camera、current/previous transform、object/material/instance/draw ID、time、viewport、jitter、clip-space を versioned semantic accessor にする。 |
| 固定機能 key | **P1。** 設計の blend/depth_write/cull では不足。実装は polygon fill、MSAA 1x、stencil off、depth bias off、全 attachment 同一 blend、全 channel write 固定 (`pipelinefactory.cpp:236-270`)。depth compare/stencil/attachment 別 blend・mask/topology/front face/depth bias/sample state まで capability 検証付きで公開する。 |
| 新 pipeline stage | **P2。** material と PipelineFactory が vert/frag を型として固定する。tessellation/geometry/mesh/task は stage 配列と device requirement を持つ C 用 `pelican.pipeline` に分離する。ray tracing は「1 draw」の外である。 |
| GPU→CPU readback | **P2。** shader/material ABI ではない。frame graph の readback node、staging ring、fence、latency、format/row pitch を持つ非同期 API にする。 |

見落とした壁も重大である。

- 任意 vertex/index buffer、vertex layout、instance rate、indirect draw、draw count。現在は
  `use_engine_vertex_layout` でない vertex-input shader を拒否する (`pipelinefactory.cpp:103-106`)。
- MRT/format/depth/load-store/resolve/feedback-loop の宣言。`pass` は既存 pass を選ぶだけである。
- sampler の filter/address/aniso/compare/LOD と texture dimension/cube/array/MSAA。現 sampler は
  `maxLod = 0`、anisotropy off 固定 (`materialcontainer.cpp:60-77`)。
- subgroup/barycentric/interlock/descriptor indexing/16・64-bit 等の device requirement と fallback。
- untrusted shader の GPU hang、巨大 resource、descriptor 数、compile time を抑える trust policy。

C は最低でも二分すべきである。

1. **C-material**: engine が発行する既存 geometry draw 内で shader/resource/state を差し替える。
2. **C-pipeline/pass**: stage、vertex/draw input、attachment、resource、frame graph node まで定義する。

multi-pass、CPU readback、ray pipeline まで「1 ドローで何でも」に含めるのは分類誤りである。

### 2. B は本当に C の糖衣か

**原則は書かれているが、構成的に保証されていない。** standard/toon を公開 shader library
だけで書くのは必要条件だが十分条件ではない。B template が engine 内部 descriptor、
depth/velocity/shadow variant、`screen_inputs` の copy/reroute、custom varying、
params/textures allocation を暗黙に受け取るなら B は C より強い。

現 binder は固定 texture しか bind せず、set 0 descriptor set は最初の default pipeline layout
から一つだけ確保する (`materialcontainer.cpp:158-178,239-247`)。異なる set 0 layout の raw
shader に同じ set を bind する設計は Vulkan layout 互換性で破綻する。B template が既定 layout
に合う間だけ動くなら、それは B の隠れた特権である。

不変条件を次の形でテストすべきである。

> すべての B material は公開 `pelican.material + pelican.pipeline + resources` だけからなる
> C 記述へ lowering でき、B runtime 専用分岐は存在しない。

CI は `dump-lowered-material` を出し、その C 記述を直接ロードした結果と pipeline layout、
frame plan、最終 SPIR-V、画像を比較する。standard/toon の source だけを dogfood しても、
binder/scheduler の特権は検出できない。

「最深フック優先」も危険である。`pelican_surface` と `pelican_brdf` を同居させると前者が
黙って無効になる。vertex hook とは別に terminal hook (surface/brdf/lighting) は一つだけ許し、
複数は error にすべきである。

### 3. B snippet の自己完結性

現設計で自己完結するのは params/texture/lighting/screen input を使わない短い関数だけである。
実用例には material JSON、`<stem>.params.glsl`、言語別 shim、正しい version、texture asset、
glslang の `--keep-uncalled`、Slang の `[noinline]` が別途要る。Shadertoy/Godot 的な
「貼れば動く」とは別物である。

Godot は uniform の型/default/editor hint を shader 自身へ置く。Pelican は値から型を推論し、
宣言を外部 JSON へ置くため、code 単体から interface を復元できない。

さらに「宣言順 std140」は現実装と JSON の性質に反する。現 parser は `nlohmann::json` object を
iterate する (`materialformat.cpp:328-347`)。既定 object は `std::map` なので入力順ではなく
辞書順になる
([nlohmann/json Object Order](https://json.nlohmann.me/features/object_order/))。JSON object 自体も
unordered である。`MaterialParamKind` は int を持たず、
全 number を scalar double として読む (`materialformat.hpp:12-27`,
`materialformat.cpp:300-324`)。

`.surface` は version/language/terminal hook、params の名前・明示型・default・UI hint の
ordered array、texture/sampler の型/default/色空間、screen input、capability、source 本体を持つ
自己記述 container にすべきである。shim は作者に include させず compiler が自動注入し、
source map 上は仮想 header とする。合格条件は「新規 project に一ファイルを置き、material から
stem を一行参照するだけ」と機械的にテストすることである。

### 4. SPIR-V ABI link の落とし穴

#### 4.1 依存と保守範囲

[SPIRV-Tools README](https://github.com/KhronosGroup/SPIRV-Tools) は linker を
“still under development” と明記する。同 README は validator が未完で、optimizer pass は
個々の metric を改善すると保証しないとも述べる。したがって `spirv-val OK` と
`vkCreateGraphicsPipelines OK` は必要条件でしかない。

WP59 は既に、関数を debug name から探す処理、LinkageAttributes 注入、dummy entry point と
stale `OpName`/`OpMemberName` の除去、Slang の `Offset` decoration 除去、producer 別 descriptor
remap、early-inline/dead-strip 抑止を必要とした。SPIRV-Reflect は reflection/binding rewrite には
使えても、関数 body、entry point、type decoration、debug graph の一般編集器ではない。
SPIRV-Tools の public Linker/Optimizer だけで足りなければ、内部 IR API か独自 rewriter を所有する。
「text rewrite でなく API」は実装手段であり、保守コストを消さない。

SPIRV-Headers/Tools/Reflect は system Vulkan SDK に追従せず、既知の組で pin/vendor すべきである。
cache key には compiler 名・版・全 option、target env、shim/template/lib hash、linker/optimizer 版、
ABI version、pass、hook set、bindless mode が要る。§5 の
`(shader stem, defines, pass)` だけでは stale binary を防げない。

#### 4.2 compiler 更新耐性

スパイクは glslang 15.2.0、Slang 2025.6.1、SPIRV-Tools v2025.1 の一点だけを検証した。
既に両 producer は simple struct と sampler を異なる形で出力した。将来変化で危険なのは、
function 名/mangling と `OpName`、early inline/dead strip、function control、block/member/
precision/non-uniform decoration、pointer storage class、matrix stride、combined/split sampler、
NonSemantic debug info、target capability/extension である。

glslang は 2026 年に HLSL front-end 廃止予定を
[公表しており](https://github.com/KhronosGroup/glslang)、compiler surface は実際に変わる。
Pelican が GLSL front-end だけを使っても、CLI/output が固定とは仮定できない。latest/latest-1
matrix、保存済み実物 SPIR-V corpus、normalization 前後の差分、semantic image test を upgrade
gate にすべきである。

#### 4.3 debug と性能

スパイクは stale reference を避けるため debug name を一括除去した
(`experiments/spvlink/REPORT.md:75-78`)。関数発見を debug metadata に依存しながら最終 artifact
では捨てるのは自己矛盾である。RenderDoc は SPIR-V debugging と NonSemantic debug info を扱えるが、
link/inline 後の命令を user file へ戻す map がなければ、巨大 template の逆 assembly しか見えない。

- **debug profile**: inline/DCE を抑制し、`OpLine` と NonSemantic debug info を保持・再 map。
  shim/template/user source を別 virtual file にし、debug printf を許可する。
- **release profile**: full optimize。ただし link map、最終 SPIR-V、exact tool command を保存する。

`--inline-entry-points-exhaustive` と aggressive DCE の成功は単一 compilation と同じ最適化品質を
証明しない。monolithic baseline と linked path の GPU time、driver pipeline compile、最終 ISA の
instruction/register/spill/occupancy、hot reload latency を AMD/NVIDIA/Intel/MoltenVK で測る必要が
ある。pipeline が一度作れたことを性能証明として扱ってはならない。

### 5. フック梯子と ABI 進化

`PELICAN_SURFACE_V1` macro は source 再 compilation の分岐にはなるが、配布済み B `.spv` の ABI
を守らない。SPIR-V function type が異なれば linker は型を合流できず、無理な pointer bitcast が
invalid になることを WP59 自身が示した。

現実的な版管理は次のいずれかである。

1. symbol を `pelican_surface_v1`、`pelican_surface_v2` と版別に永久保持し、template が v1 を
   v2 へ adapter する。v1 struct には二度と field を足さない。
2. scalar/vec accessor 関数または固定 slot (`vec4 slots[N]`)を安定 ABI にし、意味は版付き関数で
   増やす。inline 後の overhead は実測する。

「深さ 4 は compile/動作が壊れない」という保証も過剰である。library version、device capability、
resource limit、attachment、engine feature が変わる以上、保証できるのは旧 symbol/layout を
定めた期間保持し互換 test を通すことまでである。

深さ 3 の deferred 自動再利用には effect 制約が必要である。任意 user function は derivative、
discard、screen texture、storage write、subgroup、fragment builtin を使える。それを G-buffer model
ID と fullscreen light pass へ機械的に移せない。「任意 BRDF」と有限 bit の model ID も、登録上限、
per-pixel dispatch、variant 数を決めない限り両立しない。v1 は depth 1〜2 だけを deferred 候補、
depth 3〜4 を forward 既定にし、深さ 3 deferred は pure/effect allowlist を通る opt-in とすべきである。

### 6. `screen_inputs`

framebuffer を読みながら同じ image へ書く feedback hazard を snapshot/copy で避ける判断自体は妥当。
しかし最低でも次を契約にしなければならない。

- snapshot point: after opaque / before transparent / named pass / previous frame
- transparent を含めるか。含めるなら sort 途中で何回 snapshot するか
- snapshot の共有/chain、MSAA/depth resolve、format/color space/HDR
- mipmap/filter/resolution/viewport/eye/layer
- full/region copy、region 外 read、read-write cycle の診断

Godot は最初の利用時だけ full-screen copy するため、2D の重なる screen shader は後の shader が
前の結果を見ない。3D は opaque 後・transparent 前に一度 copy するため transparent は写らず、
screen texture 使用 material 自身も透明扱いになる。region 外 read は undefined である
([Godot screen-reading shaders](https://docs.godotengine.org/en/stable/tutorials/shaders/screen-reading_shaders.html))。
Pelican の「自動配線」はこの trade-off の解決ではなく、隠して再実装するだけである。

material が resource 名を並べるのでなく、render config が named snapshot を定義し material が
参照すべきである。opaque 後 snapshot を複数 material が共有し、透明同士の逐次屈折は v1 非対応と
明記する。自動 pass split は frame-plan dump に出し、copy byte 数と resolve/mip cost を表示する。
material 単位の暗黙 graph mutation は禁止する。

### 7. 現行実装との drift

設計書は v1.1 を「M1 実装済み」とするが、parser が認識する material key は
`name/base/shader/defines/params` だけである (`src/project/materialformat.cpp:350-372`)。
`textures`、`surface`、`lighting`、`screen_inputs`、`render_state`、`pass` は保持されず
unknown warning になる。status は「M1 は旧 v1 部分のみ」へ訂正すべきである。

ほかにも次の矛盾がある。

- §3 冒頭と §4/M2 は params UBO、§3-5 は全 material struct の SSBO とし個別 UBO を禁じる。
- custom texture の後端と現 VAT binding 4/5 が衝突し得る。0〜5 の予約か完全 remap が必要。
- MaterialContainer pipeline key は vert/frag の二つだけ (`materialcontainer.cpp:26-29`)。
  render state、format、defines、bindless mode を足すと誤共有する。
- `.spv` C は defines 非対応の固定一 variant なのに、§3-3 は C に depth/velocity variant を要求する。
  pass 別 artifact/entry point の命名規約がない。
- PelicanSurface/lib/hook version が §5 の cache key に含まれない。

## 業界前例との比較

| 前例 | 選んだ境界 | Pelican の異例な選択と、避けられた理由の推測 |
|---|---|---|
| Unity Surface Shader | HLSL surface function から同一 compiler/codegen が forward/deferred/影 pass を生成。現在は Built-in Render Pipeline のみで URP/HDRP/Custom SRP 非対応。 | Pelican は任意 producer SPIR-V と engine library を binary ABI 化する。Unity の surface abstraction が pipeline 内部と強く結合し別 SRP へ移れなかった事実は、深い hook の長期安定が難しい実例である。 |
| Unreal Material/Function | engine 所有 IR/graph と列挙 shading model。Custom HLSL は式 node の範囲。現在も `EMaterialShadingModel` は enum。 | Pelican は任意 BRDF/lighting を engine light loop と結合し deferred へ自動参加させる。Unreal は G-buffer、Lumen/影、permutation、platform compiler を統制するため、自由より閉じた model 集合と最適化可能 IR を選んだ。 |
| Godot gdshader | engine 所有の一言語、vertex/fragment/light hook、built-in、render_mode、自己宣言 uniform/hint。 | Pelican は複数言語、外部 JSON 型推論、4 段 binary hook を選ぶ。Godot は一言語・一ファイルの制約で診断、editor introspection、default、portable snippet を得た。 |
| Bevy/naga_oil | GLSL/WGSL を個別に Naga IR へ parse し、module IR を合成・tree-shake。 | Pelican は最終寄り SPIR-V を decoration 正規化して link する。naga_oil は producer 差より手前の共通 IR で名前、型、diagnostic、resource を管理し、cross-compiler binary ABI を背負わない。 |
| Slang module/interface | 一つの front-end/IR/linker/target codegen で module、interface、generic、link-time specialization、reflection を提供。 | Pelican は複数 producer 出力を後段 SPIR-V で同列に扱う。Slang は同一 compiler 内で型検査、specialization、source map、layout を一貫させるため、任意 producer 差を public ABI にしない。 |

一次資料:
[Unity Surface Shader](https://docs.unity3d.com/Manual/SL-SurfaceShaders.html)、
[Unreal shading models](https://dev.epicgames.com/documentation/en-us/unreal-engine/shading-models-in-unreal-engine)、
[Godot shading language](https://docs.godotengine.org/en/stable/tutorials/shaders/shader_reference/shading_language.html)、
[naga_oil](https://docs.rs/crate/naga_oil/latest)、
[Slang modules](https://shader-slang.org/slang/user-guide/modules)、
[Slang link-time specialization](https://shader-slang.org/slang/user-guide/link-time-specialization)。

## 図書館員回答

### a. SPIR-V Linkage の実運用前例

**主要な市販/OSS real-time engine の material hook で、SPIR-V `LinkageAttributes`
Import/Export を公開 ABI として常用する確実な前例は見つけられなかった。** 不存在の証明ではなく、
公開資料から確認できなかった、という回答である。

確認できる実用は SPIRV-Tools の `spirv-link`、OpenCL/offload toolchain、複数 translation unit の
offline link など compiler/toolchain 側が中心である。[SPIR-V 仕様](https://registry.khronos.org/SPIR-V/specs/unified1/SPIRV.html)
自体は import/export を定義し、
[OpenCL offline compilation](https://www.khronos.org/blog/offline-compilation-of-opencl-kernels-into-spir-v-using-open-source-tooling)
にも `spirv-link` の例がある。しかし graphics engine の user material ABI とは trust、debug、
resource binding、hot reload の条件が違う。

前例が乏しい理由は次と推測する。

1. GPU API が受け取る最終 shader は entry point/resource interface が閉じた module であり、
   driver-level の標準 dynamic link UX はない。結局 engine tool が全責任を持つ。
2. SPIR-V は source-level type/module/diagnostic を失った後の interchange 形式であり、その地点で
   producer を混ぜる利益が小さい。
3. graphics pipeline は function ABI だけでなく descriptor、stage IO、state、attachment、variant と
   結合する。function link を解いても engine 統合の大半が残る。
4. source composition、engine IR、Slang module の方が source map と optimization を一元化できる。

### b. Slang module/interface と本設計。Slang 一本化しなかったのは誤りか

| | Slang module/interface | Pelican SPIR-V link |
|---|---|---|
| 型安全 | 同一 front-end が interface/generic を型検査 | Pelican 独自の scalar/vec/simple-struct ABI と正規化に依存 |
| 診断/debug | source/module を保持しやすい | producer 別 debug info を link/inline 後に再構築 |
| 最適化 | link-time specialization と target codegen が一体 | spirv-opt + driver。品質を別途測定 |
| 言語 | Slang/HLSL 系へ寄る | SPIR-V producer なら参加可能 |
| web | WGSL target はあるが公式に work in progress | linked SPIR-V→Naga という追加変換が必要 |
| lock-in | Slang compiler/IR へ集中 | SPIRV-Tools と各 front-end の組合せへ分散 |

**Slang 一本化しなかったこと自体は誤りではない。** 「B も任意言語」が中核価値なら Slang-only は
要求を満たさない。ただし出荷安定性、debug、blog 共有が優先なら、B v1 を一つの canonical language
(Slang または GLSL source template)に絞る方が合理的である。

誤りなのは Slang native module path と同じ corpus で比較せず、SPIR-V link を第一候補から本採用へ
昇格したこと。推奨は **Slang/source backend を production baseline、cross-producer SPIR-V link を
experimental backend** として同じ B API の裏で競争させ、性能・診断・web・upgrade 工数の実測後に
一本化を判断することである。

### c. naga spv-in で B の web bake に刺さる制限

最新 Naga SPIR-V front-end は `SUPPORTED_CAPABILITIES`/`SUPPORTED_EXTENSIONS` の allowlist を持ち、
`strict_capabilities` は既定 true である。Linkage capability は allowlist にないため、
**最終 executable から Import/Export と Linkage capability を完全除去できなければ失敗**する。
また Naga README 上 WGSL output は secondary support であり、SPIR-V を読めたことと browser が受理する
WGSL を書けることは同義でない。

具体的な危険は次である。

- Vulkan では合法だが WebGPU/WGSL にない capability/extension/type。B ABI が小さくても function
  body は自由なので、64-bit、一部 atomic/subgroup/barycentric 等が入り得る。
- push constant と WebGPU binding model。web template では uniform/immediate への lowering と host
  binding 変更が必要で、「同じ template を link して Naga」だけでは済まない。
- set/binding を WebGPU の bind-group/stage limit、dynamic indexing、writable storage 制約へ再検証
  する必要がある。split sampler は必要条件だが十分条件ではない。
- SPIR-V front-end は coordinate-space adjustment を既定で行う。host でも反転していれば二重反転する。
- Naga IR は row-major matrix を native に持たず access 時 transpose へ変換する。ABI から matrix を
  禁じても user resource block の layout は別問題である。
- `NonSemantic.Shader.DebugInfo.100` は front-end が無視するため、native と同じ source debug は残らない。

web B は native B の無条件な subset とせず **B-web capability profile** を定義する。dist-bake は
link → Naga parse → Naga validate(WebGPU capability) → WGSL write → Dawn/wgpu/browser validation を
hard gate にする。Naga の現 allowlist は
[公式 source](https://github.com/gfx-rs/wgpu/blob/trunk/naga/src/front/spv/mod.rs)、WGSL target の差は
[Slang WGSL 制約](https://shader-slang.org/slang/user-guide/wgsl-target-specific)も参考になる。

### d. Godot SCREEN_TEXTURE / hint_screen_texture が踏んだ問題

本質は API 名ではなく snapshot semantics である。

1. 2D は最初の使用前に full-screen copy を一度だけ行い、重なる後続 screen shader が先行 shader の
   結果を見ない。
2. 3D は opaque 後、transparent 前に一度 copy するため transparent object が含まれない。
   `hint_screen_texture` material 自身も透明扱いで、他 material の screen texture に写らない。
3. scene order と copy timing が暗黙に結合し、必要なら `BackBufferCopy` node の明示挿入が要る。
4. region 外 sample は undefined。mipmap/filter は追加生成 cost を持つ。
5. screen/depth/normal-roughness の提供可否が renderer 依存で、normal-roughness は Forward+ 限定。

Godot 4 が built-in `SCREEN_TEXTURE` を uniform + `hint_screen_texture` へ変えて sampler/filter を
明示しやすくしても、順序と copy cost は消えなかった。Pelican は簡潔な表面だけを模倣せず、
この制約を契約へ露出すべきである。

## 推奨する設計変更

### 実装順を組み替える

1. **M2a: 契約 drift を解消。** v1.1 parser を実装し、unknown key を hard error または明示 extension
   とする。params は ordered array + explicit type にする。UBO/SSBO、binding range、version を確定。
2. **M2b: C substrate を先行。** 公開 resource manifest、semantic engine data、render state、
   pass/stage interface、capability validation、完全な cache key、`dump-lowered-material` を実装。
3. **M3a: B production baseline を source include または Slang module で作る。** 単一 `.surface`
   自己記述形式、shim 自動注入、source map、terminal hook 一つ、B→C lowering equivalence を完成。
4. **M3b: SPIR-V link は experimental feature flag で追加。** 下記 gate 完了まで既定にしない。
5. **M3.5: screen snapshot を pass-level 設計として独立。** opaque snapshot だけから開始し、
   transparent-chain は非対応と明記。
6. **M4: web profile を別契約として bake。** capability report と不対応理由を artifact に残す。

### ABI と形式の具体変更

- `PelicanSurfaceV1` は凍結し symbol に version を含める。macro だけで版管理しない。
- terminal hook は surface/brdf/lighting の一つだけ。vertex displacement は直交 hook。
- depth 3/4 は forward 既定。deferred は effect-validated opt-in。
- engine shader library は小さな versioned semantic function 群にし、旧版の保持期限を宣言。
- B/C 共通 `resources` manifest を導入し、B template だけの private descriptor を禁止。
- B compiler の最終 C 記述、binding table、frame plan、link map、最終 SPIR-V を dump 可能にする。
- precompiled C は pass/stage/feature 別 artifact map を持ち、defines 非対応との矛盾を解く。

### SPIR-V link 本採用の受け入れ gate

以下が一つでも欠けるなら本採用しない。

1. GLSL/Slang/HLSL 各 10 以上の corpus。struct、array resource、split sampler、control flow、
   derivative、discard、debug printf、複数 hook version を含む。
2. compiler latest/latest-1 と pinned version の CI、normalization 前後の golden、旧 B `.spv` 互換 test。
3. `spirv-val` に加え AMD/NVIDIA/Intel/MoltenVK の pipeline + image test、Naga web bake test。
4. RenderDoc で user source/line/variable が確認できる debug profile。
5. monolithic baseline に対する GPU time、ISA/register、pipeline compile、hot reload の閾値。
6. fuzzer/reducer による malformed/unexpected producer SPIR-V、resource/capability limit test。
7. toolchain と全入力 hash を含む再現可能 cache。artifact から exact command/version を復元可能。

この順なら SPIR-V link が失敗しても B/C の公開モデルは残る。現在の順では linker の失敗が
B の UX、C の権限、web、deferred、screen input をまとめて巻き込む。そこが本設計の最大の構造的
欠陥である。
