# レンダラ構築コンパイラ: 論理型・ターゲット計画・物理実行計画(v1.1)

対象読者: レンダラ実装者、独自描画方式・最適化・Vulkan backend を実装する人。

ステータス: v1.1 設計方針(2026-07-24)。公開 ABI は未凍結。RPE1〜RPE9と
RPE10a runtime publication rootまで実装済み。
RPE6b0の純CPU logical graph、RPE6b1のcolor/depth screen-input contractに加え、
RPE6c0/1でdata-only topology/probe、`ResourcePattern`、desktop/tile physical plan fixture、
canonical/disposable lowering seamを追加した。汎用graphのruntime実行所有権は未移行。
RPE1〜RPE5で実装済みの
`RenderPipelineRequest` / `ResolvedRenderPipeline` / `CompiledRenderPipeline` と
draw queue 基盤を移行元とし、既存の flat 1x 描画結果を変えずに段階導入する。
v1.1 は CPU、GPU compute、将来の specialized device operation を縦の compiler 層として
増やさず、共通 typed dialect と横方向の backend domain へ接続する境界を明記した。異種
execution 全体の正は
[`design_heterogeneous_execution_graph.md`](design_heterogeneous_execution_graph.md) とする。

本書は [`design_render_pipeline_extensibility.md`](design_render_pipeline_extensibility.md)
の compiler / compiled plan / backend 境界を詳述する。関連文書:

- [`design_compute_task_graph.md`](design_compute_task_graph.md) — 現行フレームグラフの
  依存導出、安定スケジュール、plan dump
- [`design_heterogeneous_execution_graph.md`](design_heterogeneous_execution_graph.md) —
  typed dialect、target execution、CPU / Vulkan sibling lowering、fragment / closed forest
- [`design_material_shading.md`](design_material_shading.md) — material / `.surface` /
  OpenPBR / screen input 契約
- [`design_color_pipeline.md`](design_color_pipeline.md) — scene / display domain と
  output transform
- [`design_asset_hot_reload.md`](design_asset_hot_reload.md) — prepare / publish / rollback /
  retire
- [`design_openxr.md`](design_openxr.md) — view family と XR lifecycle

## 0. 決定事項

1. renderer の標準公開経路は **論理コンパイル**と**ターゲットコンパイル**の二段階
   facade とする。内部では target execution と backend physical lowering を分離可能にする。
2. 論理グラフと物理グラフの構造的一致や相互逆変換は要求しない。
3. 論理型は **少数の閉じた型構造 × 拡張可能な意味型**とする。
4. 型、ポート間制約、アクセス特性、物理表現の選好、Vulkan 記述を分離する。
5. 型パラメータ判定は文字列比較の散在でなく、正規化済み値と宣言的な制約式で行う。
6. material と light は image / buffer と同じ巨大 enum に入れず、個別の contract を持つ。
7. region は置換・診断用のタグであり、barrier・allocation・最適化境界ではない。
8. tile GPU は後付け preset でなく、同じ論理グラフに対する第一級の物理 lowering
   target とする。
9. 上級者は論理層を迂回して物理グラフを直接供給できる。物理 IR でも表現できない
   処理には、境界契約付き `NativeScope` を用意する。
10. 通常利用者の入口は引き続き一つの preset と少数設定であり、本書の型式や
    constraint DSL を毎回記述させない。
11. renderer の logical / Vulkan 経路は異種 execution graph の一 dialect / backend とする。
    CPU や specialized GPU 機能を `PassKind` の閉じた enum へ足し続けない。
12. 中間層では typed `GraphFragment`、全層では非連結 component を許す。publish 前の
    physical execution plan は connected でなく closed であることを要求する。

本設計は Vulkan を隠す RHI の設計ではない。現在の backend は Vulkan 専用であり、
物理計画も Vulkan の能力を完全に利用できる。論理層はその部分集合を移植可能に
記述するが、物理層全体を再表現しない。

## 1. renderer の二段階 facade と内部 compiler 境界

```text
project / preset / material / light / view intent
                         │
                         ▼
               resolve authoring intent
                         │
                         ▼
        [1] Logical Render Compiler
                         │
                         ▼
              CompiledLogicalGraph
                         │
       + target facts / user pins / cost policy
                         │
                         ▼
        [2] Vulkan Target Compiler
                         │
                         ▼
              VulkanPhysicalPlan
                         │
                  prepare GPU objects
                         │
                         ▼
              PreparedRenderPipeline
                         │
                         ▼
                    Runtime
```

この図は現行 renderer の互換 facade である。内部の目標境界では、`Vulkan Target
Compiler` を target-aware な domain / implementation 選択と Vulkan physical lowering に
分離する。

```text
CompiledLogicalGraph
        │
        ▼
TargetExecutionCompiler
        │
        ▼
execution.gpu fragment
        │
        ▼
VulkanLowerer
        │
        ▼
VulkanPhysicalPlan
```

CPU task が必要になった場合は `TargetExecutionCompiler` から sibling `CpuLowerer` へ分岐し、
Vulkan の下や上へ新しい縦層を挿さない。graphics / GPU compute / transfer は同じ Vulkan
fragment として全体最適化する。現行実装は両段を一つの `Vulkan Target Compiler` 関数群に
置いてよいが、RPE6c0 / RPE6c1 で data-only seam を作る。

### 1.1 論理コンパイラ

論理コンパイラは「何を計算し、どの意味の値を受け渡すか」を決める。

- preset / recipe の展開
- material / light / geometry / view 要求の収集
- material 集合の route / phase partition
- logical pass と typed port の構築
- feature、history、screen input、XR view family の意味的な依存
- 登録済み変換の挿入と型・制約検査
- 有限個の実装候補または variant family の保持

`VkFormat`、image usage、load/store、barrier、queue family、実 allocation、descriptor、
command buffer はここへ入れない。

### 1.2 ターゲットコンパイラ

ターゲットコンパイラは、論理グラフ、data-only な `TargetTopologySnapshot`、Vulkan
`BackendProbe` 結果から実行可能な物理計画を作る。単純な一対一変換ではなく、次を
行ってよい。

- logical pass の融合・分割・削除
- resolve / snapshot / copy / materialization の追加
- material route の再 partition
- G-buffer schema や lighting strategy の候補選択
- rendering scope、attachment、load/store、queue、同期、aliasing の決定
- same-pixel read の tile-local 化
- multiview / sequential、graphics / async compute variant の選択

Vulkan object の作成は意味選択を行わない backend code generation / prepare とし、
二つ目のコンパイラの後段に置く。

異種 execution へ拡張した最終形では、本節前半の candidate / domain / target-aware rewrite を
`TargetExecutionCompiler`、Vulkan enum、queue family、barrier、alias の具体化を
`VulkanLowerer` が所有する。CPU / external backend が存在しない構成では、この分離は
runtime object や追加 work を発生させない。

### 1.3 対応関係

要求するのは次の片方向の性質である。

- 対象 profile に対して有効な論理グラフは、物理化できるか理由付きで拒否される。
- 一つの論理グラフから複数の物理計画を生成できる。
- 物理計画には対応する論理グラフが存在しないものがあってよい。
- 物理計画から論理グラフへの損失なし逆変換は保証しない。

したがって、構造の同型ではなく **境界契約を保存する refinement** を検証する。
logical resource が物理 image にならない場合や、複数 logical pass が一つの shader /
rendering scope になる場合も正しい。

### 1.4 既存語彙への写像

```text
RenderPipelineRequest       ユーザー意図
ResolvedRenderPipeline      preset・policy・候補の解決結果
CompiledRenderPipeline      論理コンパイル結果を所有する root
  └ CompiledLogicalGraph    typed logical IR
VulkanPhysicalPlan          GPU object を持たない物理 IR
PreparedRenderPipeline      rollback 可能な GPU candidate
RenderRuntime               publish 済み plan の実行
```

現行 `CompiledRenderPipeline` は policy manifest の実装まで完了しており、
`CompiledLogicalGraph` を段階的に追加する。新 root 型へ一括 rename しない。

## 2. 論理型: 閉じた構造と開いた意味

### 2.1 型構造

core が理解する constructor は原則として次の五つに閉じる。

| constructor | 用途 | 例 |
|-------------|------|----|
| `Image` | 画素・voxel・view image | scene color、depth、motion、shadow |
| `Buffer` | index 可能な構造化データ | light records、cluster index、indirect args |
| `Stream` | 順序・partition を持つ仕事列 | draw stream、dispatch stream、ray work |
| `ObjectSet` | scene semantic 集合 | view family、geometry、material、light |
| `Value` | 小さな immutable 値 | exposure、camera parameters、quality intent |

acceleration structure のような Vulkan 固有実体を無理に constructor へ追加しない。
論理層では `ObjectSet<RayScene>` 等の意味契約として扱い、物理層で AS へ lower する。
物理操作そのものを公開したい場合は物理 IR を使う。

概念上の型は次の形を取る。

```cpp
struct LogicalType {
    LogicalTypeConstructor constructor;
    SemanticTypeId semantic;
    TypeArgumentSet identity_arguments;
    TypeArgumentSet refinements;
};
```

`SemanticTypeId` は `namespace + name + major version` からなる。未知型を enum の
末尾へ追加する方式にせず、engine、project、game DLL が同じ registry 規則で登録する。

```text
pelican.render.color_signal@1
pelican.render.depth@1
pelican.scene.view_family@1
pelican.scene.material_set@1
game.water.thickness@1
plugin.volumetric.froxel_scattering@2
```

constructor 固有の構造も schema で固定する。`Image` は 2D / array / cube / volume 等の
logical topology、`Buffer` は element schema、`Stream` は item contract と順序保証、
`ObjectSet` は要素 contract、`Value` は record schema を持てる。実 extent、layer 数、
sample count、format は relation / target constraint / physical plan 側で決める。

### 2.2 型引数と、それ以外の値

型の内部へ入れるのは、意味上の同一性または適用範囲を変える値だけとする。

| 種類 | 置き場所 | 例 |
|------|----------|----|
| identity argument | `LogicalType` | scene/display、linear/encoded、depth representation |
| refinement | `LogicalType` | RGB/spectral、alpha convention、必要 feature set |
| port relation | `PortConstraint` | `same_extent(MainDepth)`、per-view、half-resolution |
| access / footprint | `ResourceUse` | read/write、same-pixel、neighborhood |
| target constraint | planning constraint | sample set、format feature、attachment budget |
| policy hint | `ResourcePattern` / planning policy | quality、bandwidth、latency 優先 |
| concrete value | `VulkanPhysicalPlan` | `vk::Format`、usage、tiling、load/store、barrier |

これにより、sample count や format の全組合せを論理型名として列挙しない。

### 2.3 型引数の表現

型引数は自由な JSON object や生文字列 map にしない。domain 登録時に parameter schema
を宣言し、compiler が default 補完・順序正規化・範囲検査を行う。

```cpp
using TypeArgumentValue = std::variant<
    bool,
    std::int64_t,
    std::uint64_t,
    Rational,
    EnumValueId,
    SemanticTypeId,
    IntegerInterval,
    EnumValueSet,
    SymbolId>;
```

- float の曖昧比較が必要な型引数には `Rational` または量子化済み整数を使う。
- omitted default と明示 default は canonicalize 後に同じ hash を持つ。
- 未知引数、重複引数、不正 enum、範囲外値は登録時または compile 時に拒否する。
- cache key と dump は canonical form を使う。

### 2.4 canonical alias

利用者と診断には読みやすい版付き alias を提供する。alias は opaque enum でなく
正規化された型式へ展開される。

```text
SceneLinearHdrV1
  = Image<color_signal@1,
          reference=scene, transfer=linear, range=extended>

DisplayLinearV1
  = Image<color_signal@1,
          reference=display, transfer=linear, range=normalized>

DisplayEncodedV1
  = Image<color_signal@1,
          reference=display, transfer=output_encoded, range=normalized>

DeviceDepthV1
  = Image<depth@1, representation=device>

LinearViewDepthV1
  = Image<depth@1, representation=linear_distance, space=view>
```

working primaries、reverse-Z convention 等、consumer の正しさに必要な値は identity /
refinement または `ViewFamily` との relation として追加する。bit depth、image tiling、
attachment usage はここへ入れない。

### 2.5 type pattern と判定結果

pass / algorithm は個別型名の `if` 連鎖でなく `TypePattern` と constraint expression を
宣言する。標準式は少なくとも次を持つ。

- exact / equals / not-equals
- enum `one_of`
- set contains / subset / intersection
- integer / rational range
- trait requirement
- symbol binding と `same_as`
- extent の整数比関係

判定は `bool` で情報を捨てず、次を返す。

```cpp
enum class MatchStatus { exact, convertible, deferred, rejected };

struct TypeMatchResult {
    MatchStatus status;
    TypeBindings bindings;
    ConversionPath conversion;
    DecisionReason reason;
};
```

`deferred` は `samples = same_as(MainColor)` のように、論理 compile 時点では正しいが
target compile まで値が決まらない状態である。未解決を `0` や unknown enum で表さない。

trait は候補選択に使えるが、代入互換性を与えない。`ColorLike` / `SceneReferred` が
一致しても、nominal type が異なる値は変換なしに接続できない。

この query builder / data schema は recipe・provider 実装と診断用であり、通常の project
設定や material ごとに記述させない。core の代入互換規則を任意 callback で上書きする
ことも許さない。custom domain は parameter schema と conversion を登録して参加する。
一つの candidate 内は正規化可能な制約の conjunction、分岐は明示的な有限 candidate
list とし、一般 SAT / SMT や再帰的 user expression を renderer 起動経路へ持ち込まない。

### 2.6 変換

異なる semantic type の接続は、登録済み `DomainConversion` を graph node として挟む。

- `automatic_safe`: 必要 context が揃い、意味と損失が規約上許容される変換
- `explicit_only`: tone map、gamut mapping、不可逆圧縮等、pipeline の意図を変える変換

例えば `DeviceDepthV1 -> LinearViewDepthV1` は view parameter があれば自動候補にできる。
`SceneLinearHdrV1 -> DisplayLinearV1` の tone map は explicit-only とし、terminal に
二重挿入されることを型検査で防ぐ。

変換探索は登録済み有限 graph に限定する。同順位の複数経路が残ったら暗黙選択せず、
provider 名または変換を pin させる。

conversion は型の辺だけで終わらせず、選択後に node へ materialize できる版付き operation
ID と provider fingerprint を持つ。builtin の静的 provider は owner identity / generation を
ともに 0、reload 可能な provider は両方を非 0 とし、片方だけの descriptor は拒否する。
registry から compile snapshot を採る際はこの descriptor を値として複製し、callback が必要な
provider lease は prepare 完了まで別途保持する。RPE6b0 は descriptor と検証まで、immutable
registry snapshot / lease は RPE6c0 で閉じる。

## 3. 型、ポート、resource use

型だけでは tile-local read、history、外部所有等を表せないため、三つの契約へ分ける。

```cpp
struct LogicalPortContract {
    TypePattern accepted_type;
    ConstraintSet relations;
};

struct LogicalResourceDesc {
    LogicalType type;
    MaterializationPolicy materialization;
};

struct LogicalResourceUse {
    optional<LogicalValueId> input_value;
    optional<LogicalValueId> output_value;
    AccessMode access;
    AccessIntent intent;
    ReadFootprint footprint;
};
```

### 3.1 SSA に近い値モデル

logical output は原則 immutable な新しい value とする。同じ `SceneLinearHDR` に
透明物を合成する場合も、意味上は `opaque_color -> composed_color` とする。
physical lowering は安全なら同一 image / attachment へ alias または in-place 化できる。

実装上の `LogicalValueId` は resource family と version の組である。一つの `(resource,
version)` には producer を高々一つだけ許し、consumer がその値を読むことで data edge を
導出する。version の大小だけでは依存を作らない。例えば `color#1` と `color#2` の writer は、
後者が前者を input として消費しない限り独立であり、宣言順を correctness edge に昇格しない。

graph 外から入る値は version 0 の明示 import とする。通常 input、previous epoch、external
ownership を区別する。現行 `FrameGraphDefinition` 互換 adapter だけは未明示初期値を
`legacy_implicit` として記録できるが、新しい実行 graph の authoring 入口では許可しない。

external target、history state、query 等の副作用は暗黙の名前順でなく effect として
宣言する。通常ユーザーに SSA 記法を要求せず、preset / recipe が生成する。

### 3.2 read footprint

最低限、次を区別する。

| footprint | 意味 | 代表例 |
|-----------|------|--------|
| `same_pixel` | 同じ画素位置だけ読む | deferred lighting、depth fade |
| `neighborhood` | 有限近傍・offset sample | 屈折、blur、reconstruction |
| `arbitrary` | 任意位置・scatter/gather | 一般 compute、global lookup |
| `temporal` | 過去 frame / persistent state | TAA、exposure history |

必要なら neighborhood radius、derivative、sample-frequency を refinement する。
footprint は resource の型でなく use ごとの性質である。

### 3.2.1 access intent

`AccessMode` が read / write の論理方向を表すのに対し、`AccessIntent` は sampled、attachment、
storage、transfer、host のどの利用形へ lower したいかを表す。`automatic` は operation
implementation に選択を委ねる既定値である。これは Vulkan stage / access mask を直接書く
欄ではない。target compiler は選択済み implementation と intent から最小 scope を導出し、
不可能な組み合わせだけを拒否する。

### 3.3 materialization

logical resource は既定で virtual とし、次の方針を持てる。

- `virtual`: 物理 image を要求しない
- `preferred`: 診断・再利用・cost policy 上の選好
- `required`: capture、外部 API、任意 sample 等により実体が必要
- `external`: swapchain / XR image / import resource 等、外部所有

`preferred` は correctness を変えず、planner が退けられる。`required` / `external` は
融合や tile-local 化を制約し、退けた最適化理由を診断へ残す。

### 3.4 region

logical node には `region:opaque`, `region:transparency/water`, `region:post/taa` のような
階層タグを付けられる。これは次のためだけに使う。

- override / subgraph replacement の範囲指定
- eject diff の安定した anchor
- resolver / provider の所有範囲
- profiler / plan dump の grouping

region 境界で resource を実体化したり barrier を入れたりしない。global graph transform、
target compiler、physical lowering は型・effect 契約を守る限り region を横断できる。

### 3.5 ResourcePattern

logical type と具体 Vulkan representation の間には、copy / override 可能な
`ResourcePattern` を置く。pattern は型ではなく、物理候補と選好の標準ライブラリである。

```text
engine://render_patterns/scene_hdr_quality@1
engine://render_patterns/main_depth@1
engine://render_patterns/history_color@1
project://render_patterns/water_scene_copy@1
```

pattern は次を宣言する。

- 適用できる `TypePattern`
- format class または順位付き concrete format 候補
- extent / layer / mip / sample constraint
- transient、host-visible、history、capture 等の residency / lifetime 選好
- capability 不足時の明示 fallback または error
- provider / asset version と provenance

image usage / buffer usage は logical `ResourceUse` と選ばれた physical mechanism から導出し、
pattern が過剰な usage bit を常時要求しない。通常 project は pattern 名だけを選び、変更時は
標準 pattern をコピーするか、型付き field pin で一部だけ固定する。曖昧な recursive deep
merge は導入しない。最終的に `vk::Format` や usage を完全固定したい場合は physical plan
override へ降りる。

## 4. scene、material、light の contract

### 4.1 Semantic Scene IR

論理グラフの入力には、GPU buffer に変換する前の semantic 集合を置く。

- `ObjectSet<ViewFamily>`
- `ObjectSet<GeometrySet>`
- `ObjectSet<MaterialSet>`
- `ObjectSet<LightSet>`
- `Value<QualityIntent>`

forward / deferred / path tracing はこの段階の型ではない。選択した recipe / strategy が
これらを draw stream、surface data、acceleration structure 等へ lower する。

### 4.2 MaterialContract

material は resource domain と別 registry の `MaterialContract` を持つ。

- closure / shading family と版
- opacity / blend / coverage model
- derivative、discard、custom lighting 等の effect capability
- encode 可能な surface schema の集合
- 必要な screen input type と read footprint
- forward/deferred/ray 等の実装候補と拒否理由
- shader ABI / layout identity

OpenPBR の base subset は standard G-buffer schema へ encode でき、coat、transmission、
custom lighting 等は別 route を要求できる。これは OpenPBR 型そのものを forward と
定義するのではなく、選択 strategy が contract を問い合わせた結果である。

### 4.3 LightContract

light も別の `LightContract` を持つ。

- emitter / spatial support
- radiometric quantity
- sampling interface
- shadow query / visibility requirement
- volumetric participation
- packing / clustering schema の候補

同じ light 集合を clustered forward、deferred、ray/path tracing が異なる方法で lower
できる。material / light contract は DomainRegistry と同じ version / ownership /
diagnostic 規律を再利用するが、一つの万能 Type enum には統合しない。

### 4.4 route label

algorithm が material 集合へ付ける route / partition label は有効である。ただし label を
resource type と混同しない。結果は `RouteDecision { tag, reason, contract_snapshot }` として
保持する。

現行 `MaterialRouteClass` は `hybrid_v1` の互換 route tag として維持する。将来の recipe は
名前空間付き `RouteTagId` を使い、標準 enum への追加を要求しない。

strategy 固有の G-buffer / reservoir / visibility buffer 型は private domain にできる。
複数の独立実装が共有し、変換の数学的意味を engine が検証する必要が生じたものだけを
canonical domain へ昇格する。

## 5. recipe、pass、strategy

役割を次のように分ける。

| 要素 | 責務 |
|------|------|
| `PipelineRecipe` | preset、scene contract、settings から logical graph / finite variants を生成 |
| `PassContract` | typed input/output、effect、必要 capability を宣言 |
| `PassImplementation` | contract を満たす shader / CPU / compute 実装 |
| `GraphTransform` | typed logical graph を別の typed logical graph へ変換 |
| `RenderStrategy` | hybrid / forward+ / path tracing 等、renderer 全体または大領域を生成 |
| `PhysicalLowering` | logical candidate を Vulkan physical nodes / resources へ変換 |

巨大な `IRenderPolicy` に全処理を集約しない。同じ contract の implementation 差し替えと、
graph 構造を変える transform、renderer 全体を変える strategy を別の extension point にする。

概念上の最小 descriptor は次の形である。

```cpp
struct PassContract {
    PassContractId id;
    std::vector<LogicalPortContract> inputs;
    std::vector<LogicalPortContract> outputs;
    EffectSet effects;
};

struct PassImplementationDescriptor {
    PassImplementationId id;
    PassContractId implements;
    CapabilityPredicate applicability;
    OwnerGeneration owner;
};
```

`inputs` / `outputs` から導出できるdata useを `effects` に重複記述しない。`effects` はhistory、
present、external write等がある場合だけ設定し、空なら追加semantic effectなしという作者の
宣言として受理する。

implementation は contract より狭い capability 条件を持てるが、port の意味を変更したり
具体 format を勝手に固定しない。必要な物理条件は planning constraint として返し、
target compiler が他候補と合わせて解決する。

標準 compiler は未知アルゴリズムを発明しない。recipe / provider が列挙した有限候補を
検査・順位付けする。

## 6. ターゲット計画と層横断最適化

### 6.1 topology / capability / probe facts

target compiler へ渡す facts は data-only snapshot とする。単体 device facts に加え、host、
Vulkan device、external endpoint 間の memory / transfer / synchronization relation を
`TargetTopologySnapshot` の directed link として持つ。

- Vulkan version / extension / feature bits
- format feature と sample count の表
- multiview、dynamic rendering local read、input attachment 等
- graphics / compute / transfer queue の能力
- tile-based / immediate の既知 profile
- transient / lazily allocated memory の可用性
- attachment / descriptor budget class
- XR view count、external image contract

実 device、queue、module、Vulkan handle は渡さない。portable Vulkan から取得できない
tile size 等を推測して correctness に使わない。vendor provider が明示する情報は
namespaced optional facts として扱う。

Vulkan backend probe は graph を変更せず、candidate ごとの feasibility、refinement
constraint、bridge offer、required physical features、cost、理由を返す。endpoint 間の
zero-copy / copy 可否は capability set の積から推測せず、topology link と probe で決める。

cost estimate は少なくとも feasibility、rendering scope 数、materialized image 数、
external store 数、transient bytes、bandwidth class、理由を返す。精密な時間予測を
必須にしない。

### 6.2 bounded planning

標準手順を次に固定する。

1. recipe / strategy が有限個の logical candidate を生成
2. user pin と logical contract で候補を絞る
3. topology snapshot と backend probe で feasibility、constraint、概算 cost を得る
4. rejected candidate と理由を記録する
5. deterministic policy で一つを選ぶ
6. target-aware graph transform を一回適用
7. selected probe result を physical lowering し、最終 validation を行う

無制限の fixed-point solver、任意 callback の総当たり、frame ごとの再 compile は行わない。
実行時に必要な少数 variant は prepare 済みにし、runtime は variant を選ぶだけにする。
lowering が probe 未宣言の必須 capability を後から要求した場合は暗黙 fallback せず、provider /
compiler inconsistency として拒否する。

logical type / effect は作者の宣言を正とし、engine が数学的意味や隠れた effect の完全性を
証明しない。hard error は未解決参照、ambiguous conversion、実現候補なし、final plan を
閉じられない場合、engine が発行する既知の Vulkan 違反に限定する。manual / native scope の
疑わしい同期、portability、性能問題は stable id 付き warning とし、CI の strict 化は opt-in
にする。

通常policyはoptimize-by-defaultとする。宣言されたdata / effect edgeがなければreorder、
parallel execution、scope fusion、transient aliasの候補とし、annotation不足を理由にglobal
barrierやmaterializationを足さない。`serial` / `isolate` / `no_alias`等のnegative constraintと
保守的diagnostic profileは明示時だけ適用する。

`conservative_debug` だけでは隠れた依存を覆い隠す場合があるため、CI / 診断用に
`hazard_stress(seed)` も用意する。これは宣言上合法な reorder、overlap、fusion、alias を
積極的に変化させ、固定 seed と選択結果を plan dump に残す。release の cost policyではなく、
contract 違反を再現可能に露出させる opt-in profile とする。

### 6.3 層横断変換

最適化は三種類に分ける。

1. physical-only: barrier、alias、queue、load/store、schedule
2. target-aware graph rewrite: scope fusion、snapshot / resolve 挿入、local read 化
3. semantic strategy: deferred / forward+、compact G-buffer、material partition 変更

2 と 3 は `CrossLayerTransform` が logical subgraph、backend facts、quality intent を受け、
typed replacement と decision log を返す。backend が logical graph を直接 mutation したり、
logical compiler が GPU handle を読む構造にはしない。

### 6.4 tile GPU を基準にした検証

同じ logical hybrid graph に対して、少なくとも次を成立させる。

```text
desktop profile:
  materialized G-buffer images
  sampled deferred lighting
  separate forward / post scopes

tile profile:
  transient attachments
  same-pixel G-buffer / depth local read
  lighting + compatible forward work の scope fusion 候補
  external consumer がなければ G-buffer store なし
```

depth fade は `same_pixel` なら local read 候補になる。屈折は通常
`neighborhood` なので opaque scene snapshot / materialization を要求する。TAA、bloom、
history は物理 image を必要とする。この違いを pass 名や「透明だから」という推測でなく、
`MaterialContract` と `ResourceUse` から導く。

local read 非対応 device では materialized sampled image へ明示 fallback し、選択理由を
dump する。まず CPU-only mock profile で両計画を検証し、その後 Vulkan 実機 path を足す。

## 7. 物理 IR と直接 authoring

### 7.1 VulkanPhysicalPlan

物理 IR は GPU object 作成前の immutable data とし、少なくとも次を持つ。

- concrete image / buffer description と ownership
- memory / alias group / transient intent
- rendering scope、attachment、subpass / local-read relation
- load/store、resolve、clear
- queue assignment と synchronization dependency
- shader / pipeline variant と descriptor binding map
- external acquire / release boundary
- logical boundary value への provenance

これは backend-neutral RHI ではない。Vulkan 固有機能を表せない共通最小公倍数へ
丸めない。将来別 backend を作る場合は、同じ logical graph から兄弟 target compiler が
別の physical plan を作る。

### 7.2 三つの入口

```text
通常:
  Request -> Logical -> VulkanPhysicalPlan -> Prepared

物理グラフ直書き:
  PhysicalPipelinePackage -> validate -> Prepared

生 Vulkan:
  Logical/Physical graph -> NativeScope -> Prepared/Runtime
```

`PhysicalPipelinePackage` は logical graph 全体を要求せず、外部へ公開する入力・出力・effect
の `BoundaryContract` を持つ。engine は boundary と外側の lifetime / synchronization を
検証し、内部を logical optimizer へ持ち上げない。

package は link 前の open physical fragment であってよいが、publish される plan では必須
import、resource ownership、completion がすべて解決済みでなければならない。独立した
physical component は正当であり、無関係な component 間へ偽の依存を追加しない。

### 7.3 NativeScope

`NativeScope` は次を明示する unsafe / non-portable escape hatch である。

- boundary input / output と semantic type(必須)
- endpoint / queue capability(既定queueで足りなければ明示)
- resource ownership と alias declaration(宣言boundary外の隠れたaccessなしとみなす)
- queue / stage / access effect(未指定はtyped useとoperation classからminimal scopeを導出)
- scope 内外の synchronization mode(`automatic`既定、`manual` / `unchecked`は明示)
- required Vulkan extensions / features(engineがobject / commandを発行する部分は必須)
- capture / device-loss / hot-reload 対応能力

scope 内の command、barrier、resource は実装側が所有できる。engine は宣言された境界の
外側だけを保証し、内部最適化や自動 alias を行わない。初期版は source extension とし、
boundary contractを完全なものとしてautomatic minimal syncを作る。詳細effectの未指定だけを
理由にscope全体を隔離したりalias / overlapを禁止しない。`manual` / `unchecked`内部の疑わしい
同期はadvisoryであり、作者がcorrectnessを所有する。保守的な全scope barrierはdebug診断profile
でだけ任意に選ぶ。安全性fixtureが揃う前に game-DLL ABI を凍結しない。

## 8. 拡張の段階

利用者が必要な深さだけ降りられるよう、次を別々の入口として提供する。

1. preset と少数 settings
2. `ResourcePattern` override
3. `PassImplementation` replacement
4. tagged region / subgraph replacement
5. global `GraphTransform`
6. renderer-wide `RenderStrategy`
7. `PhysicalLowering` または physical graph 直書き
8. `NativeScope`

上位の入口ほど portability、自動最適化、検証範囲が狭くなる。この損失を隠さず
diagnostic に表示する。

provider registry は既存の `RegistrationOwner`、generation、lease、version、capability、
data-only callback 規律を再利用する。ただし logical policy provider、physical lowering、
native backend extension は権限が違うため、同一 callback ABI へ統合しない。

## 9. diagnostics、pin、eject

すべての compiler decision は最低限次を保持する。

- decision id と対象 node / resource / material partition
- request / preset / user pin
- 検討した provider / strategy / representation
- selected value と選択理由
- rejected candidate と不足 capability / contract
- fallback、conversion、materialization、scope split の理由
- provider owner / version / content hash

user override は値を上書きするだけでなく、特定 decision を `pin` できる。pin が
capability と矛盾した場合は黙って解除せず、名前入り compile error にする。

eject / dump は層別にする。

- resolved authoring を eject
- logical graph と型・制約・decision provenance を eject
- Vulkan physical plan を eject
- `NativeScope` の boundary / source skeleton を eject

同じ層での parse -> dump -> parse round-trip は保証対象にできるが、physical plan を
logical graph へ戻す round-trip は保証しない。

## 10. 決定性、hot reload、purge

- registry snapshot、request、target facts、pins が同じなら byte-equivalent な plan を得る。
- unordered iteration、wall clock、GPU handle address、driver 列挙順を decision に使わない。
- custom rule は純粋・有限・版付きで、engine が出力を canonicalize / validate する。
- compiled type / contract は registry 内部 pointer を保持せず、正規化済み descriptor と
  owner / generation provenance を snapshot する。callback が必要な conversion / lowering は
  prepare 完了まで世代 lease を保持する。
- compile は side state で行い、logical + physical + material route + GPU candidate を一括
  prepare して frame boundary publish する。
- provider unload は新 lease を止め、compile / frame が持つ generation の終了後に retire
  する。
- 未選択 recipe / strategy / implementation は runtime resource を作らない。
- optional implementation の binary purge は別 build unit / game DLL で行う。

## 11. 現行実装からの移行

| 現行要素 | 当面の扱い | 目標 |
|----------|------------|------|
| `RenderPipelineRequest` / `ResolvedRenderPipeline` | 維持 | logical compiler の frontend |
| `CompiledRenderPipeline` | typed policy manifest として維持 | `CompiledLogicalGraph` を所有する root |
| `FrameGraphDefinition` / `FramePlan` | 既存 config の dependency graph | typed logical graph への互換 adapter |
| `FramePlanBarrier` | dependency の診断表現 | Vulkan barrier と区別し、physical plan で具体化 |
| `PassDefinition` | logical と `vk::*` が混在 | pass contract/use と Vulkan physical pass desc に分離 |
| `RenderTargetDefinition` | format / usage まで確定 | logical resource + pattern + Vulkan image desc に分離 |
| `compileRenderingPassRuntime` | resource 参照と runtime object を構築 | physical plan の backend prepare へ縮小 |
| `RenderTargetLayoutTracker` | 現行 Vulkan 遷移を所有 | physical synchronization plan の唯一の executor |
| `FeatureCompose` | verbose config を生成 | recipe / graph transform authoring への互換 frontend |
| `MaterialRouteClass` | `hybrid_v1` の固定 enum | namespaced route tag への adapter |

移行中に新旧 planner が同時に Vulkan barrier を発行してはならない。shadow compile は
plan dump と比較だけを行い、実行所有権を切り替える WP で単一 executor を選ぶ。

## 12. 段階実装

### RPE6a — logical type kernel / shadow graph

状態: **WP185 で実装済み(2026-07-23)**。`pelican.logical_render_graph` dump は
diagnostic-only であり、既存 `FramePlan` / Vulkan barrier executor を変更しない。

- `SemanticTypeId`、正規化済み `LogicalType`、parameter schema
- `TypePattern`、constraint expression、`TypeMatchResult`
- `LogicalPortContract` / `LogicalResourceUse`
- canonical color/depth aliases
- current `FrameGraphDefinition` から typed shadow graph を生成
- logical dump と decision provenance

gate:

- Vulkan device なしの純 CPU test
- exact / convertible / deferred / rejected の fixture
- parameter default・順序違いの canonical hash 一致
- unknown / ambiguous conversion の名前入り reject
- flat 1x の runtime /既存 dump は変更なし

### RPE6b0 — canonical logical value graph

状態: **WP186 で実装済み(2026-07-23)**。既存 `FramePlan` / Vulkan barrier executor は
変更せず、diagnostic shadow graph の schema を version 2 へ更新した。

- resource family + version の `LogicalValueId`、version 0 の明示 import
- output ごとの一意 producer と、exact input value から導出する data edge
- nominal semantic type を必須にした port connection
- `automatic` / sampled / attachment / storage / transfer / host の access intent
- conversion operation ID と provider identity / generation descriptor
- legacy graph を `legacy_implicit` / previous-epoch / external import へ写す adapter

gate:

- node 配列順に依存せず producer→consumer edge が得られる
- duplicate producer、未解決 input、data + explicit dependency cycle を名前入りで拒否
- 同じ resource family の異なる version に大小順を暗黙付与しない
- trait-only connection、access / intent 不整合、実装のない conversion を拒否
- shadow compile 前後で既存 `FramePlan` が不変

### RPE6b1 — hybrid screen input vertical slice

状態: **WP187 で実装済み(2026-07-23)**。標準 alias は `hybrid_v1` の固定 ABI とし、
任意 alias / provider registry と target-aware materialization は RPE6c 以降へ残した。

- `hybrid_v1` の scene color / device depth / linear depth port
- material screen-input contract と descriptor binding
- depth linearization conversion
- depth fade(`same_pixel`)と屈折(`neighborhood`) fixture
- tone map explicit-only と terminal 一回 invariant

gate:

- 既存 material を無変更で描画
- 未定義・型不一致 screen input を compile 時に拒否
- frozen snapshot-refraction golden と、実 material quad の屈折／depth-fade headless fixture
- scene-linear から display への変換が exactly once

### RPE6c0 — planning contracts

状態: **WP188 で実装済み(2026-07-23)**。`pelican_project` の `targetplanning` は
Vulkan header / device / runtime objectを持たない。現行`FramePlan`とVulkan executorは
このcontractをまだ消費せず、実行所有権は変更していない。

- data-only `TargetTopologySnapshot` と directed endpoint link
- pure Vulkan `BackendProbeInput` / `BackendProbeResult`
- finite candidate / probe / deterministic selection のdecision dump
- advisory warning id と opt-in strict policy
- declared shader interface と reflection の一致fixture
- logical effect はoptional、data dependencyはportから導出
- optimize-by-default policyと明示`serial` / `isolate` / `no_alias` constraint
- immutable registry snapshot、conversion / lowering provider lease
- opt-in `conservative_debug` / `hazard_stress(seed)` profile

実装上、RPE6c0のalias入力はtarget側で合法性を確認済みの有限candidate pairとした。
`optimized`は全候補を維持し、`no_alias` / `isolate`だけが明示的に狭める。
`hazard_stress(seed)`はその合法集合から再現可能なsubset/orderを選ぶ。resource lifetimeから
candidate pairを導く責務はRPE6c1へ残し、RPE6c0が未検証aliasを発明しない。

gate:

- Vulkan deviceなしのmock topology / probe test
- endpoint capabilityが同じでもlink有無でbridge候補可否が変わる
- rejected candidateが名前・不足constraint・provider fingerprintを持つ
- warningは既定compileを失敗させず、指定idだけstrictでerror化できる
- 依存のないnodeは追加注釈なしでparallel / fusion候補になり、保守profileだけが抑止する
- hazard stress の同じ seed は同じ plan、異なる seed は合法な順序／alias候補を再現可能に変える
- flat 1x runtime / current planは変更なし

### RPE6c1 — target planner vertical slice

状態: **WP189で実装済み(2026-07-23)**。`pelican_project`の
`targetrenderplanning`はVulkan header / device / runtime objectを持たず、現行`FramePlan` /
executorから未参照のdata-only fixtureである。

- `ResourcePattern`、materialization、read footprint
- mock desktop / mock tile topology / probe facts
- immutable canonical graph と disposable `TargetLoweringGraph` の seam
- `logical + execution -> execution.gpu` と
  `execution.gpu + physical.vulkan -> physical.vulkan` の dialect legality 検証
- `GBuffer -> Lighting -> Forward -> ToneMap` の二つの physical plan
- local-read 不可時の materialized fallback
- physical plan dump と理由

実装した`ResourcePattern`は適用型、順位付きformat候補、transient / tile-local / alias /
store選好、fallback、provenanceを持ち、resource bindingと分離される。同じpatternを任意個の
G-buffer attachmentへ適用でき、plannerは標準G-buffer名や枚数を列挙しない。実上限は
`pelican.vulkan.max_color_attachments@1` target factで検査する。

workspaceはresourceごとの最大read footprintとlifetimeを導出し、compatibleかつ非重複の
pairだけをRPE6c0 alias policyへ渡す。`logical`、`execution.gpu`、`physical.vulkan`の各完了
境界では残存dialectを拒否し、lowering後のrequired featureがselected probe宣言集合を
超えることも拒否する。

gate:

- desktop は G-buffer materialize
- tile profile は same-pixel resource を virtual / transient にできる
- refraction を加えると opaque snapshot が materialize される
- region tag を越えた fusion が許可される
- 二回 compile で byte-equivalent plan
- selected probeからlowering後に未知required capabilityが生えない
- CPU / external / video backend を登録しなくても現行 runtime work と resource が増えない
- 追加 G-buffer attachment はplanner変更なしで計画され、endpoint budget超過だけが理由付きで
  rejectされる

### RPE7 / RPE8 runtime slice — typed sample count and executable MSAA

状態: **WP190で実装済み(2026-07-23)**。`pipeline.settings.msaa`をproject compilerで
`SampleCountPolicy`へ変換し、attachment連結成分と実device capabilityから共通sample数を
決定する。既存`FramePlan` / Vulkan executorにはmultisample attachment、single-sample
resolved image、pipeline sample count、color/depth resolveを接続した。未指定configは1xの
ままである。

任意G-buffer名・枚数はplannerへ固定せず、追加attachmentも同じ連結成分の制約resourceになる。
exact失敗とlower-supported fallbackは制約resource名を保持し、resolved planは
`CompiledFrameGraphExecution`とauthoring時の`currentFramePlanJson()`から参照できる。
compute / transferはresolved surfaceだけを使い、暗黙のsample expandが必要なraster `Load`は
明示的に拒否する。

この段階は実行可能性を先に検証したruntime bridgeであり、WP189のdata-only
`TargetLoweringGraph` / `VulkanPhysicalPlan`と変換を二重実装し続けることは意図しない。
この重複は WP191 で解消した。

gate:

- exact / lower-supportedのdeterministic resolutionと制約resource診断
- attachment連結成分へ追加G-bufferを入れてもplanner変更なし
- hybrid deferred + forwardのcolor/depthが同じ実sample数
- multisample attachmentからsingle-sample imageへresolve後、後段sample/readbackが成功
- integer colorは`SAMPLE_ZERO`、non-integer colorは`AVERAGE`
- depth resolve非対応deviceは候補から除外
- 1x互換、実4x headless、Vulkan validation / synchronization errorなし

### RPE8b — physical target planner runtime integration

状態: **WP191で実装済み(2026-07-23)**。`FrameGraphDefinition`をlogical shadow graphへ
変換し、WP189の`compileVulkanTargetPlan()`でformat、representation、attachment component、
sample count、resolve requirement、physical scope sample countを一括loweringする。
旧runtime bridgeのJSON再走査、pass type string判定、独自disjoint-setは削除した。

現runtime adapterは実deviceのformat sample count、depth resolve、color attachment budgetを
target factsへ変換する。Vulkan executorが実装済みの`materialized_image`、write-only /
single-sample / attachment-onlyに限定した`transient_attachment`、same-pixel fullscreen
subsetの`tile_local_attachment`、完全一致するmaterialized image alias groupだけをadvertiseし、
返されたphysical planのformat/representation/aliasを検証してから
`RenderTargetDefinition`へ適用する。
`CompiledFrameGraphExecution`がplan本体を所有し、従来の`ResolvedSampleCountPlan`は同じ
immutable planへのviewである。

gate:

- runtime側にattachment group構築アルゴリズムを重複させない
- 任意名・追加枚数のG-bufferがlogical outputだけからcomponentへ入る
- physical resource/scopeのsample countと実attachment metadataが一致する
- sample countが異なるtile scopeをfusionせず、alias compatibilityにもsample contractを含める
- current runtimeは明示したtransient/tile-local/alias subsetの外を実装済みと偽らない
- standard/hybrid headless、feature composition、XR回帰、全CTestが成功する

### RPE8c / WP218 — strategy-private material output ABI

状態: **実装済み（2026-07-28）**。WP191は任意名・追加枚数のtarget topologyを
physical planへ運べたが、material fragment shaderとgraphics pipelineには従来の
5-MRT ABIが残っていた。WP218でこの最後の固定境界を、版付き
`pelican.material_outputs` schemaへ置き換えた。

compile順序は次で固定する。

1. renderer strategy / material passが、順序付きoutput名、GLSL型、builtin/custom sourceを
   data-only schemaとして宣言する
2. graph compilerがschema順と`output.color`順、route variant間のschema同一性、
   target numeric classを照合する
3. target plannerがattachment component、physical format、sample count、resolveを決め、
   deviceの`maxColorAttachments`とformat/sample capabilityを検証する
4. surface compilerがschemaからfragment output struct/location/writeを生成し、
   shader reflectionが全location/typeを宣言へ照合する
5. material pipelineはcompiled routeの実color/depth format、MSAA、local-read mappingを
   snapshotする。hot reload candidateがこのcontractを変える場合はpublish前に拒否し、
   旧runtime generationを維持する

logical schemaにengine-ownedな枚数上限はない。Vulkan deviceの物理上限を超えた入力は
診断付きで失敗し、compilerが暗黙のmultipassへ分割してsemanticを変えない。
schema省略時だけ既存5-MRT/1-color pathを使うため、標準presetへ常時の詳細記述を要求しない。
integer output、target別clear、history初期化は最終formatのSINT/UINT classで値を作る。

gate:

- 6枚目の`R32_UINT` custom G-bufferをgenerated `.surface` shaderから実GPU描画する
- schemaのlocation/type、pass output順、physical plan targetを相互照合する
- MSAA requestを同じattachment componentへlowerし、resolve後のUINT sampleを確認する
- target別UINT clearとmaterial-written IDを同じframeで区別してreadbackする
- incompatible hot reloadはcandidateをrollbackし、同一の旧generationで次frameも描画する
- flat/XR schema不一致、device上限超過、numeric class不一致をGPU pipeline作成前に拒否する

後続境界は、coordinated graph+surface hot reload、shaderc OFF向けdist-bake、
material local-read input ABI、attachment別blend/write-maskである。これらはoutput枚数を
再固定せず、同じschema/physical-contract seamへ追加する。

### それ以後

RPE9 / WP192 では XR / preview の ad-hoc callback を typed
`CompiledGraphVariantPolicy` へ移し、現行 XR を exact 2-view sequential として
固定した。multiview は未実装であり、同じ値の別名にはしていない。

WP203aでは、このlogical graph contractを変えずにdevice-dependentな
`VulkanViewExecutionPlan`を追加した。projectの`xr.view_execution`は
`auto` / `sequential` / `multiview`を受理し、endpointのversioned multiview capability、
最大view数、scope implementationの明示対応を使って
`single_view` / `sequential` / `multiview`へloweringする。scopeには実行回数とview mask、
image resourceには`shared_2d` / `sequential_2d` / `layered_2d_array`とlayer数を残す。
fusionとaliasもこのview contractが一致する場合だけ許す。

WP203bで、physical resourceのarray layer assignmentをinternal image allocationへ
接続し、layer別2D viewと2D-array view、per-view FrameUBO、`gl_ViewIndex` shader helper /
reflection、typed graphics pipeline / compiled pass view contract、dynamic renderingの
`viewMask`を追加した。runtime compilerはimmutable target planをpassごとに受け取り、
input edgeを`shared_2d` / `sequential_2d` / `layered_2d_array`へ注釈して、同じengine
fullscreen sourceからscalarまたはarray sampler variantを生成する。

view-family schedulerはphysical scopeをdependency順のnode-major invocationへloweringする。
`single_view`は一回、`sequential`はview数回、`multiview`はview mask付きで一回である。
barrierとtiming rangeはnodeの最初/最後のinvocationへ対応し、frame/history publicationは
logical frameに一回だけ残る。未対応material/custom passはadvertiseされず、required
multiviewではplannerまたはruntime compileが名前付きで拒否する。

synthetic Vulkan fixtureではruntime pass compilation、layered descriptor、2-view一回描画を
通し、sequential referenceと各layerがbyte一致する。

WP203c で physical plan の external depth export を実 resource と OpenXR composition
target へ接続した。OpenXR は一個の 2-layer color array swapchain を使い、sequential
scope は各 layer、multiview scope は array view へ描く。depth extension と
format/extent/usage が成立するときは typed depth producer から optional 2-layer depth
swapchain へ copy し、それ以外は color-only へ安全に縮退する。

device-dependent `auto` は compile 入力の Vulkan device identity と
`xr.multiview_auto` の測定 profile を純粋に解決する。physical plan の
`view_execution_plan.auto_gate` は selection、matched profile、graph、GPU measurement、
gain、理由を保持するため、runtime は JSON を再解釈しない。hot reload は同じ compiler
境界を通り、active device の profile を再評価する。synthetic semantic gate は済み、
Meta XR Simulator/物理 HMD と対象 GPU の実測 gate は WP203c の外部受け入れ作業として残る。

RPE10a / WP193 では compiled pass、frame graph、logical/physical plan、route、
sample-count、variant policy、draw-sort provider選択を一つの immutable runtime generationへ束ね、
base-generation CASでpublishする境界を実装した。frameは同じgeneration leaseを全viewで
保持する。

RPE10b1 / WP194 では render-config compilation が追加する render target、buffer、
compute task、shader、pipeline、fullscreen/debug/shadow/velocity pass を一つの
append-only registration arena で checkpoint した。prepare failure は全 registry の
新規 membership を逆依存順に取り消し、成功時は owner scope 付き immutable manifest を
runtime root と同じ CAS で公開する。manifest は将来の type-erased resource lease を
保持できる。

RPE10b2 / WP195 では同じ owner scope の manifest と program 集合を replacement できる。
target / buffer / compute / pass / pipeline / shader の registry handle は再利用せず、
新 generation の同名 resource に新しい handle を割り当てる。frame graph execution と
compute/fullscreen descriptor は compile 時の typed handle snapshot を保持するため、
旧 generation は mutable name facade が新 scope を指した後も旧 resource を使い続ける。
scope の type-erased lease が exact registry membership を所有し、最後の runtime
generation 参照が消えた時に逆依存順で retire する。Vulkan payload は既存
`DeletionQueue` へ委譲する。別 owner program の resource dependency は現状保守的に旧 lease を
継承し、今後の明示 dependency graph まで dangling reference を避ける。

同じ owner の同名 program は pass ID と順序を維持し、候補から消えた program は新
generation から除去する。prepare failure / stale publication は active generation、
current name facade、全 registry membership を不変に保つ。
実際のdraw-sort provider generationも別registryのqueue構築時leaseであり、このrootが
所有するのはcompiled policy内のprovider名までである。

RPE10b3 / WP196では、project-backed rendering config / feature / presetの変更を
`ReloadService`のbatch participantへ集約し、preview、flat、起動中のXRを全prepareしてから
一回のruntime generation CASで公開する。flat / XRは同じGPU owner scopeを使い、一方だけを
公開しない。parse、compile、target plan、GPU registration、prepared validationの失敗は
active rootと全registry membershipを不変にする。

logical frameが取得したgeneration snapshotは、window swapchainのin-flight slot、
offscreen submit、OpenXRの各eye、独立desktop mirror submitへleaseとして渡される。
各targetは対応fence完了後だけleaseを解放するため、CPU側のactive root置換とGPU側の
旧resource退役が正しく分離される。OpenXRの部分失敗ではsubmit済みeyeをabort経路で待ち、
wait不能時はtarget teardownまでleaseを保持する。

### RPE11a — fullscreen PassImplementation provider

状態: **WP200で実装済み(2026-07-24)**。最初の`PassImplementation` fixtureは
fullscreen shader pairだけを対象にする。authoringのpassへ任意のprovider名を指定でき、
未指定時はbuiltin identity providerを同じregistry経路で解決する。

provider入力は`PassContractV1`であり、portのnominal type pattern / relation、direction、
access、intent、read footprintとradius、fullscreen interface flagをcanonical byte rangeで
渡す。callbackはimplementation idとshader referenceだけを返し、logical port、resource use、
effect、physical representationを変更できない。contract fingerprintとprovider
owner / identity / generation / version / capabilityをcompiled passへ保持する。

このv1はphysical planningへ制約を返さないため、`VulkanTargetPlan`が保持するlogical nodeから
contractをsnapshotし、runtime shader/pipeline生成前に解決する。将来のapplicability /
planning constraintはtarget compiler前の別版入力・出力として追加し、shader-pair ABIに
Vulkan値や遅い逆依存を混ぜない。複数passの解決結果は全callback成功後だけ適用する。

registry snapshotはflat / preview / XR一括transactionのpublicationまで同じものを保持し、
owner releaseはin-flight compile完了を待つ。game DLL fixtureでV1→V2切替、invalid ABI、
prepared rebuild失敗のrollback、shutdown後の失効を固定した。

gate:

- builtinとgame DLLが同じcontract callbackを通る
- providerが変更できるのはfullscreen shader pairだけ
- footprint radiusを含むcontract fingerprintが決定的
- unknown / stale / wrong-owner / invalid outputを名前入りでreject
- 二つ目のpass失敗で一つ目を部分適用しない
- registry snapshot中はowner release / DLL unloadしない
- production configの未指定passはbuiltin identityで既存挙動を維持

### RPE11b — tagged region / subgraph replacement

状態: **WP201で実装済み(2026-07-24)**。authoring passの`regions`とgraphの
`region_replacements`から置換対象を選ぶ。provider未指定時のbuiltin identityと
game DLL providerは、`RegionContractV1`を受けてreplacement pass-array JSONを返す
同じowner-aware registry経路を使う。

置換はcompiled logical graphへのin-place mutationではない。元のnormalized configを
typed logical graphへcompileして境界contractを作り、provider結果をlocal config candidateへ
spliceし、candidate全体をもう一度typed logical graphへcompileする。resource集合、type、
materialization、boundary portとfingerprintが一致した場合だけtarget loweringへ進む。
結果のprovider/source/replacement provenanceは`CompiledLogicalRenderGraph`と
`VulkanTargetPlan`に残る。

v1は連続fullscreen region、既存resource、protected terminal/anchor非対象に限定した。
置換passへ元regionの共通tagを強制し、region外の明示順序をreplacement全体へ張り直す。
region tagは最適化境界にしない。provider出力不正、非連続、型／境界変更、unknown ownerは
候補全体を捨て、active generationを変更しない。

gate:

- builtin identityとgame DLLが同じtyped region contractを使う
- 1 passを2 passへ展開してもinput/output boundary、resource type、materializationが不変
- resource追加削除、境界変更、malformed JSON、非連続regionを理由付きでreject
- logical graphとtarget planへprovider/source/replacement provenanceを保持
- provider snapshot解放前にowner registration / DLLをretireしない
- pass→subgraphのregistry lock順をcompileとowner releaseで統一する
- production GPU registrationでbuiltin identity regionをpublishできる
- V1→V2 reload、invalid candidate、rebuild rollback、shutdown後失効を維持する

### RPE11c — global GraphTransform

状態: **WP202aで実装済み(2026-07-24)**。`graph_transforms`はfeature合成・graph variant
rewrite・ImGui合成後、tagged region置換とtarget loweringより前に実行する有限の順序付き
chainである。各entryは一意な`name`、任意の`provider`、object `parameters`を持ち、
provider未指定時は`builtin.identity_v1`を選ぶ。preset overlayからも同じ配列を追加できる。

public C ABI `RenderGraphTransform::ProviderV1`は、全graphのcanonical logical JSON、
canonical normalized config JSON、parameters、graph-set contractとfingerprintを受け、
完全な候補config JSONとsemantic implementation idを返す。STL、engine pointer、Vulkan型は
ABIを越えない。provider出力はlocal candidateへparseし、全logical graphを再compileして
からだけ採用する。

graph-set contractはgraph entrypoint名と、既存のexternal/history/graph-inputおよび
materialization-required resourceの具体logical typeを保持する。transformは内部pass、
compute task、buffer、runtime-materialized render targetを追加できるが、既存のprotected
boundaryを削除・改型できず、新しいexternal/history importも作れない。pipeline policy、
material routing、graph-local control、canonical anchor列、`output_transform`は変更不可で、
flat/preview/XR policyを変換後に再検証する。

選択結果はtransform名とchain index、provider owner / identity / generation / version /
capability、boundary fingerprint、入力・出力graph fingerprintを
`CompiledRenderPipeline`、`CompiledLogicalRenderGraph`、`VulkanTargetPlan`へ残す。
pass→subgraph→graph-transformの順でsnapshot lockとowner releaseを統一し、全variantの
publication完了まで同じleaseを保持する。

gate:

- builtin identityとgame DLLが同じgraph-set callbackを通る
- 変換chainの順序と各段のinput/output fingerprintが一致する
- internal target/pass追加を許し、external/history/既存required境界変更をrejectする
- malformed JSON、control再導入、policy/material route/terminal変更を理由付きでrejectする
- original configとactive runtimeを途中状態へ変更しない
- logical/pipeline/target planへproviderとgraph fingerprint provenanceを保持する
- preset overlay、production GPU registration、V1→V2 reload、rollback、shutdownを維持する

### RPE11d — renderer-wide RenderStrategy

状態: **WP202bで実装済み(2026-07-25)**。`RenderStrategy`は既存graphの一部を
変形する`GraphTransform`ではなく、preset展開済みのrenderer seed config全体から
新しいrenderer configを生成する独立した拡張点である。

```json
{
  "render_strategy": {
    "name": "project.path_traced_preview",
    "provider": "project.render_strategy",
    "parameters": {}
  }
}
```

provider未指定時は`builtin.authored_config_v1`がselectorを除いたseedをそのまま返す。
selector自体が無ければcallbackを通さず、既存configとdumpを変えない。preset overlayから
strategyを追加できるが、preset自身がstrategyを持つ場合は暗黙overrideを許さず、presetを
copy/ejectしてから編集する。

実行位置はpreset展開後、feature parsing / compositionより前である。出力は通常どおり
feature composition、canonical color pipeline、flat/preview/XR graph variant、
global `GraphTransform`、tagged subgraph replacement、typed logical compile、
Vulkan target loweringをすべて通る。strategy出力が`render_strategy`または展開済みの
`pipeline` controlを再導入すること、非object／malformed JSON、または後段compilerに
不正なconfigを渡すことは候補全体の失敗になり、元seedとactive runtimeを変更しない。

public C ABI `RenderStrategy::ProviderV1`が受けるrenderer facade contractは、現時点で
実際に利用可能な機構だけをversioned capabilityとして公開する。

- feature composition / canonical color pipeline
- graph variant policy(history、jitter、view family/execution、resource layout、terminal、
  mirror、view count)
- typed logical compile
- global graph transform / tagged subgraph
- Vulkan target lowering
- runtime shader compiler availability

V1はstartup config compilerが所有していないlive material / light / geometry inventoryを
偽って公開しない。したがってV1でrenderer構造全体の生成はできるが、scene inventoryを
走査して完全なpath tracerを組み立てるcontractは後続ABIの対象である。将来追加する場合も
V1 facade capabilityへ破壊的に足さず、新しいversioned contractとして公開する。

選択結果はstrategy名、provider / implementation、facade/output contract、graph variant、
input/output config fingerprint、owner / identity / generation / version / capabilityを
`CompiledRenderPipeline`、`CompiledLogicalRenderGraph`、`VulkanTargetPlan`へ残す。
pass→subgraph→graph-transform→render-strategyの順でsnapshot lockとowner releaseを統一し、
flat/preview/XRの全候補がpublicationを終えるまで同じprovider generationをleaseする。
preview compilerとplan viewerも同じstrategy解決経路を使う。

gate:

- builtin identityとgame DLLが同じtyped renderer-facade callbackを通る
- preset展開後かつfeature composition前に一回だけ実行する
- strategy出力を通常のfeature/variant/logical/physical compilerへ戻す
- malformed JSON、非object、control再導入、後段compiler rejectをfailure-atomicに扱う
- pipeline/logical/target planへcontractとconfig fingerprint provenanceを保持する
- flat/preview/XR、production headless、V1→V2 reload、invalid ABI、rollback、
  shutdown後失効を維持する

### RPE12a — target policy authoring / Vulkan decision pin

状態: **WP204 Phase Aで実装済み(2026-07-26)**。通常経路を手書き physical plan に
置き換える前に、既存 compiler の portable control と target-specific pin を別の型として
公開した。

`target_planning` は pipeline-wide profile/diagnostic policy と graph-scoped
node/resource constraint を `TargetPlanningPolicy` へcompileする。graph 単位にしたのは、
flat/preview/XR や複数 entrypoint 間で同名でない node/resource へ制約を誤適用しないためで
ある。XR variantではgraph名だけを `#xr` へ解決し、node/resource名は論理名のまま使う。

`VulkanTargetPlanPinPackage` v1 は次だけを持つ。

- package schema/version
- compiled logical graph名
- canonical logical JSONから得た `fnv1a64` fingerprint
- versioned backend candidate pin

自動 plan dump は同じ層へ戻せる `ejectable_pin_package` を常に出す。config の
`vulkan_plan_pins.flat/preview/xr[]` は現在の immutable graph variantに対応するpackageだけを
選び、target bridgeがgraphごとに `VulkanTargetPlanRequest` へ渡す。適用済みpackageは
`applied_pin_package`へ残す。fingerprint不一致、有限候補集合に無いpin、probe不成立の
candidateはすべてcompile errorであり、pinを黙って解除しない。

renderer-wide strategyはphysical controlの意味を所有しない。このため
`target_planning` / `vulkan_plan_pins`をstrategy ABI seedとinput/output config fingerprint
から除外し、callback完了後に元のcontrolを復元する。providerが同名controlを生成した場合は
責務衝突として候補全体をrejectする。これにより、ejectしたpackageを貼ったこと自体で
logical fingerprintが変わる自己参照を防ぐ。

このv1はbackend decision pinであり、resource/scope/Vulkan値をuncheckedに上書きする
direct physical planではない。resource/scope編集はpin schemaを広げず、次のRPE12bで
独立したphysical fragment schemaとverifier/linkerへ分離する。

gate:

- target control無しの既定 plan/deterministic selectionを維持する
- profile、graph-scoped constraint、strict warningがruntime target compilerまで届く
- pin packageのsame-layer parse/dumpとlogical fingerprintが決定的
- pinで自動cost選択と異なるfeasible candidateを選べる
- stale/unknown/infeasible pinと未知graph/node/resourceをprepare前に拒否する
- Plan Viewer/RPCのphysical planからejectでき、適用provenanceを再観測できる
- strategy有無でlower-layer control追加前後のlogical fingerprintが変わらない

### RPE12b — verified Vulkan physical fragment

状態: **WP204 Phase B v1 + verified alternate-format / attachment-operation /
transient / tile-local / alias / dependency-safe-scope runtime sliceで実装済み
(2026-07-26)**。自動target compilerを通常経路に残したまま、同じphysical層の一部だけを
編集して自動planへ戻す境界を実装した。
dependency-safe scopeの実装・検証台帳は
[`design_reviews/2026-07-26_wp204_scope_execution_report.md`](design_reviews/2026-07-26_wp204_scope_execution_report.md)
を参照する。

`VulkanPhysicalFragmentPackage`のschemaは
`pelican.vulkan_physical_fragment` version 1 / 2 / 3である。version 1は次を持ち、
version 2はさらにsparse attachment operation override、version 3は明示的な
`scope_edit_mode`を持つ。現在のejectはversion 3 / `dependency_safe`を生成する。

- graphとcompiled logical graph fingerprint
- canonical target/device/provider環境を含むautomatic plan fingerprint
- 自動選択済みbackend candidate
- sparse resource override
- 任意のcomplete scope partition
- 任意のalias group集合
- version 2以降では`(node, logical_resource)`ごとの任意の`load_op` / `store_op`
- version 3では`split_only | dependency_safe`のscope編集mode

自動plan dumpは全resource/scope/aliasと、契約を持つ全attachmentを含む
`ejectable_physical_fragment`を出す。
configの`vulkan_physical_fragments.flat|preview|xr[]`は現在のimmutable graph variantに
対応するpackageだけを選び、graphごとの重複を拒否する。適用結果は
`applied_physical_fragment`として再観測できる。package無しの経路は従来のautomatic
planをそのまま使う。

automatic plan fingerprintはcanonical topology/device facts、backend probeと
provider generation、lowering graph、required feature、resource/scope/alias、
sample/view execution、external depth contractを束縛する。logical graphが同じでも
device factsやprovider generationが変わったpackageはstaleとしてrejectし、別planへ
黙ってfallbackしない。

全versionが共通して受理するresource / alias編集は意図的に狭い。

- resource representationはautomatic値の維持、またはautomatic
  transient/tile-local imageの`materialized_image`化だけ
- format fieldはautomatic formatのcanonical round-trip、またはrender targetの
  `format_candidates`で宣言済みの`materialized_image` format。alternate formatは
  target固有のrequired image usage、sample count、array layer上限、external depth
  transfer-source capability evidenceをすべて満たす場合だけ
- alias groupはaliasable resource、単一所属、同一representation/format/sample/view/
  extent、lifetime非重複をすべて満たす場合だけ

v1/v2のscopeは従来どおり全nodeのexact ordered partitionかつ単一automatic scopeのsplitだけ
である。version 3の`split_only`も同じ意味を保つ。`dependency_safe`はdata edgeと明示
`after` / `before`をすべて保つexact partitionに限ってscope/node順を変更し、編集後の
resource lifetimeを再計算してalias groupを再検証する。schedulerとrendererは元node index
ではなく、この検証済みphysical scope orderを実行する。

version 3で異なるautomatic scopeを融合できるのは、internal materialized imageだけを使う
single-sample rendering scopeで、kind/sample/view contractとordered attachment集合が一致する
場合に限る。2 pass目以降はLoad、非終端passはStoreでなければならず、UI/ImGui、swapchain、
MSAA、sampled/storage/transfer依存は拒否する。成立したscopeには
`single_rendering_instance`を付け、runtimeはscope全体で一度だけdynamic renderingを開始する。
`local_read_scope`は同じ実行機構のうちinput-attachment mappingも必要な狭いsubsetになった。

version 2以降のattachment overrideはlogical readとLoadの一致を維持し、対象を
materialized/external resourceへ限定する。StoreからDiscardへの手動変更は、別MSAA resolveが
論理値を保存し、同じmultisample surfaceを後続attachmentがLoadしない場合だけ許可する。
automatic transient loweringによるDiscardはこの手動overrideとは別に、write-only
resourceの候補選択時に証明する。

linkerは編集後にdependency order、scope-resource boundary、resource lifetimeを再計算し、
tile/transient resourceのscope越境、
不正なsampled dependency、未知resource/node、node重複/欠落を拒否する。resourceごとの
format、sample plan、required feature、external-depth contractを同期し、selected endpoint
capabilityと同じbackend candidateのfeature closureを再検証してから`VulkanTargetPlan`を
返す。runtime bridgeは同じdevice capability snapshotからformat assignmentを作り、
同名targetを共有するgraphおよびflat/XR variant間のformat競合をGPU登録前に拒否する。
runtime adapterはtile-local / alias / materialized scope fusion結果のうち、後述する
検証済みsubsetだけを受理する。
それ以外はparser成功と実行可能性を混同せず、runtime capability gateでrejectする。

automatic planningにはmaterialized/tile-localと独立した
`pelican.vulkan.transient_plan@1`を追加した。attachment-only、non-history、
single-sample、write-onlyのvirtual resourceで、対象device/formatがtransient usageを
受理するときだけ選び、automatic StoreをDiscardへloweringする。runtimeはrepresentationを
typed storage modeへ変換し、imageへ`VK_IMAGE_USAGE_TRANSIENT_ATTACHMENT_BIT`を付け、
lazily allocated memoryを優先する。`conservative_debug`、非対応format、後段readでは
materialized + Storeへ戻る。physical fragmentはtransientをmaterializeしてStoreを増やす
保守的なescape hatchを維持する。

tile-local loweringは、non-history、single-sample attachmentのproducerと、`same_pixel`だけを
読むfullscreen consumerがextent/view契約を共有し、device/formatがdynamic rendering local
readを受理する場合に限る。physical scope fusion、input attachment shader ABI、location/index
mapping、BY_REGION dependency、single-view/sequential/multiview実行を同じplanから生成する。
material/custom/raster consumer、neighborhood read、MSAA、非対応device/formatでは
materialized candidateへ戻す。

alias runtime loweringは、non-history、single-sample、materialized、
`COLOR_ATTACHMENT | SAMPLED`の同一image契約を持ち、compiled lifetimeが重ならないresourceに
限定する。複数graph variantを一つのruntime target集合へmergeするときは、いずれかのmemberへ
触れる全planが同じ完全groupを持つ場合だけassignmentを残す。不一致時は共有を無効化する。
runtimeはVMA `CAN_ALIAS`とVulkan `IMAGE_CREATE_ALIAS_BIT`で別VkImageを同一allocationへbindし、
alias member切替時にmemory dependencyを発行して新しいlogical imageを`Undefined`から遷移する。
registration generation固有tokenにより、hot reload candidateは旧in-flight allocationを
共有しない。

renderer strategyはこのlower-layer controlを生成・解釈しない。
`vulkan_physical_fragments`も`target_planning` / `vulkan_plan_pins`と同様にstrategy ABI
seedとconfig fingerprintから退避し、callback後にfailure-atomicに復元する。

gate:

- full ejectとsparse packageが同じparse/canonical dump/link経路を通る
- malformed schema、stale logical/environment fingerprint、candidate不一致をrejectする
- undeclared/device非対応alternate format、aggressive representation変更、
  illegal scope fusion/boundaryをrejectする
- alias compatibilityとlifetime overlap、feature/capability growthをrejectする
- flat/preview/XR variant選択とduplicate graph packageを検証する
- OpenXR OFF / ON、headless Vulkan runtimeで既定経路とfragment routeを維持する
- headless hot reloadでalternate formatの実image/pipeline世代交換と出力一致を検証する
- write-only transientのdevice/format gate、Store elision、実allocation、hot reloadを検証する
- tile-localのscope fusion、shader ABI、single-view/multiview実行とfallbackを検証する
- aliasのvariant合意、実allocation共有、memory dependency、generation/rollback/recreateを検証する
- dependency-safe reorderのdata/after/before維持、lifetime再計算、非連続node index scheduleを検証する
- headless hot reloadで独立compute scopeのreorder、materialized rendering scopeの単一instance化、
  Load保持pixelを実描画し、synthetic Vulkan multiview回帰を維持する

次の候補:

1. WP203c の Meta XR Simulator/物理 HMD と対象 GPU 実測 gate
2. WP204 後続 — MSAA/external/異種attachmentを含む広いscope fusion、一般のload-store /
   queue / barrierのaggressive physical verifier、MSAA/history/depth/storage/transfer/bufferを
   含むalias範囲拡張、対象GPU実測gate
3. `NativeScope` は具体的な Vulkan-only 使用例が得られてから ABI 設計
4. CPU / external domain は計測と具体的な二候補 task が得られてから
   `design_heterogeneous_execution_graph.md` の HEG3 / HEG4 として実装

## 13. north-star acceptance scenarios

1. **普通のゲーム**: `hybrid_v1` 一行で現行と同じ絵・順序・resource を得る。
2. **水だけ改造**: transparency/water region と material contract を置換し、必要な
   scene color / depth だけ compiler が接続する。
3. **tile GPU**: 同じ logical recipe から local-read / transient plan を生成し、
   neighborhood refraction だけ snapshot を強制する。
4. **renderer 全交換**: custom path-tracing `RenderStrategy` が material / light /
   geometry contract を消費し、標準 forward/deferred route を使わない。
5. **物理最適化実験**: user-authored Vulkan physical plan が logical internals を持たず、
   typed boundary だけで present / XR envelope と接続できる。
6. **未知 Vulkan 実験**: `NativeScope` が必要 extension と effect を宣言し、通常 graph の
   外側 lifetime を壊さずに実行できる。

この六つを同じ API の万能 callback で満たそうとしない。各段階を独立した fixture で
実証し、実証済みの境界だけを public ABI として凍結する。
