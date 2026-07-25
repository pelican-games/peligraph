# 異種実行グラフ: typed dialect・domain planning・backend lowering(v1)

対象読者: renderer / compute / runtime scheduler / backend 実装者、独自の CPU・GPU
実装方式を追加する人。

ステータス: v1 設計方針(2026-07-23)。公開 ABI は未凍結。rendererの
`FrameGraphDefinition` / `FramePlan`、RPE6b0のversioned logical value / producer edgeを持つ
diagnostic shadow graph、RPE6b1のtyped material screen-input縦切りに加え、RPE6c0/1の
topology/probe、immutable/disposable lowering seam、desktop/tile physical plan fixtureまで
実装済みである。汎用CPU task scheduler、異種execution linker、動画encode/decodeは未実装で
あり、必要性と計測を確認してから個別WPにする。

本書は、renderer 固有の [`design_render_graph_compiler.md`](design_render_graph_compiler.md)
と、現行 render / GPU compute 依存グラフの
[`design_compute_task_graph.md`](design_compute_task_graph.md) の一段外側を定義する。
logical color / depth、material、tile GPU、Vulkan physical plan の詳細は前者、現行 JSON と
安定トポロジカル順の互換規則は後者を正とする。

## 0. 決定事項

1. semantic から physical への lowering を**縦軸**、CPU / Vulkan / external API 等の
   execution domain を**横軸**として直交させる。CPU や特殊 GPU 機能を追加するたびに
   縦の compiler 層を増やさない。
2. 全層で共有するのは **typed IR kernel** であり、全層が同じ型語彙や巨大 node struct を
   使うのではない。logical、execution、CPU physical、Vulkan physical、bridge は別 dialect
   とする。
3. 目標の compiler 境界は `LogicalCompiler` → `TargetExecutionCompiler` → sibling
   `BackendLowerer` とし、最後に `ExecutionLinker` が backend plan を閉じる。現行の
   `VulkanTargetCompiler` は移行中、このうち target execution と Vulkan lowering を一体で
   実装してよい。
4. graphics / GPU compute / transfer は、resource、queue、barrier、alias、tile locality を
   全体で解くため、同じ Vulkan backend plan に置く。GPU compute は独立 backend ではない。
5. 論理的な計算 task は CPU / GPU compute 等の有限 implementation candidate を持てる。
   `compute` という authoring node kind と、選択済み execution domain を同一視しない。
6. execution domain や device engine を閉じた `CPU | Graphics | Compute | Transfer` enum に
   しない。名前空間付き capability pattern と endpoint descriptor で選び、動画、ray tracing、
   external accelerator 等を core enum 変更なしで追加できるようにする。
7. 中間層では typed import / export を持つ open `GraphFragment` を定義できる。すべての層で
   非連結 component を許す。最終 plan に要求するのは **connected** でなく **closed** である。
8. canonical logical graph は immutable / hashable に保つ。target compile は専用の一時
   `TargetLoweringGraph` を unique-own し、検証と provenance の範囲で破壊的に融合・分割・
   削除できる。
9. backend physical plan は object handle を持たない data-only artifact とする。device / OS
   object の作成、frame 実行、retire は compiler でなく runtime preparer / executor の責務と
   する。
10. renderer、CPU job、streaming、動画等を一つの万能 scheduler algorithm に統合しない。
    graph、型、effect、capability、provenance の機構を共有し、scheduler は dialect ごとに
    分ける。
11. すべての ECS system や常駐 service を frame graph へ入れない。graph resource / effect
    と同期する必要がある粗粒度 task だけを統合し、内部並列化は各 subsystem に残す。
12. 本設計は backend-neutral な最小公倍数 RHI を作るものではない。Vulkan physical plan は
    Vulkan 固有機能を完全に表せる。兄弟 backend は共通 logical / execution contract から
    別の physical plan を作る。
13. endpoint の能力は単体 `CapabilitySet` だけで判断しない。endpoint と、endpoint 間の
    memory / transfer / synchronization relation を持つ immutable `TargetTopologySnapshot` を
    target planning の入力にする。
14. target planner が候補を確定する前に、各 backend は純粋な `BackendProbe` で feasibility、
    refinement constraint、cost、bridge offer、拒否理由を返す。probe は graph を変更せず、
    backend object を作らない。
15. logical contract は作者の宣言を正とし、effect completeness の証明を要求しない。data
    dependency は exact な versioned value の producer / consumer port から導出し、state /
    external effect は必要な作者だけが宣言する。
    未宣言の semantic effect は「ない」という作者の主張として扱う。
16. lowering 後に上位 operation は残さないが、semantic boundary、origin、rewrite evidence は
    immutable annotation として残せる。下位 scheduler が上位 IR を再解釈することは禁止する。
17. 通常利用者には preset、標準 type alias、自動配線、単一 compile 診断だけを見せる。
    dialect、topology、probe、physical plan は高度な拡張段階へ降りた利用者だけが触る。
18. validation は trust-first とする。実行不能、未解決参照、選択した capability の不足、engine
    が発行する既知の API 違反だけを hard error とし、疑わしい effect / synchronization、
    portability、性能上の問題は既定で warning にする。warning の error 昇格は CI / project
    policy の opt-in とする。
19. 通常実行は **optimize-by-default** とする。宣言済み port / effect / boundary を完全な
    contract として信頼し、依存がなければ reorder / parallelize / fuse / alias の候補にする。
    `serial` / `exclusive` / `no_alias` / `isolate` は必要な作者だけが明示し、保守実行は
    debug / CI の診断 profile に限定する。

## 1. 用語

| 用語 | 意味 |
|------|------|
| `TypedIrKernel` | type id、schema、pattern、constraint、effect、capability、hash、diagnostic の共通機構 |
| dialect | 一つの層が所有する operation / type / verifier の名前空間 |
| semantic operation | 実装場所でなく、何を入力して何を生成するかを表す logical node |
| implementation candidate | semantic contract を満たす CPU kernel、shader、backend operation 等の有限候補 |
| execution endpoint | host、特定 device、external service 等、選択後の実行先 |
| capability | endpoint / implementation が要求・提供する名前空間付き能力 |
| `GraphFragment` | typed import / export / effect を持つ、合成前でも保存可能な部分グラフ |
| closed graph | 必須 import、resource producer、宣言済みeffect、endpoint がその段階の規則まで解決済みのグラフ |
| forest | 相互依存のない複数 component を含む closed graph |
| bridge obligation | CPU↔GPU、backend↔backend 等を接続する必要があるという data-only 契約 |
| physical plan | backend object を作る直前まで具体化した immutable data |
| prepared plan | object 作成済みだが未 publish で、失敗時に破棄できる candidate |

本書で「tree」と呼ばないのは、実際の依存が fan-in / fan-out、共有 value、effect edge を
持つ DAG / hypergraph だからである。UI 上の所有階層や region tree は、実行依存とは別の
metadata とする。

## 2. 全体構造

```text
project / preset / feature / material / task GraphFragment
                         │
                         ▼
                  LogicalCompiler
                         │
                         ▼
              CanonicalLogicalGraph
                 immutable / hashable
                         │
        + target facts / policy / user pins
                         │
                         ▼
             TargetExecutionCompiler
                         │
       (mutable TargetLoweringGraph はここだけ)
                         │
                         ▼
              DomainPartitionedGraph
       ┌─────────────────┼──────────────────┐
       ▼                 ▼                  ▼
  CpuLowerer       VulkanLowerer      ExternalLowerer
       │                 │                  │
       ▼                 ▼                  ▼
 CpuPhysicalPlan  VulkanPhysicalPlan  ExternalPhysicalPlan
       └─────────────────┼──────────────────┘
                         ▼
                  ExecutionLinker
                         │
                         ▼
                ExecutionPlan
             frame / epoch closed forest
                         │
                    prepare all
                         │
                         ▼
            PreparedExecutionCandidate
                         │
              frame boundary publish
                         ▼
             Runtime domain executors
```

`ExecutionPlan` は共通語彙であり、renderer の frame-scoped specialization を
`FrameExecutionPlan` と呼ぶ。常駐 service や複数 frame にまたがる stream 全体を、一つの
巨大な frame plan に押し込まない。frame / epoch ごとの enqueue、resource lifetime、
completion と、persistent executor の状態を typed boundary で接続する。

`CanonicalLogicalGraph`、`DomainPartitionedGraph`、`ExecutionPlan` は目標語彙であり、現行型の
一括 rename を要求しない。現在の `CompiledRenderPipeline::logical_graph` は render dialect
fragment、
`FramePlan` は logical dependency / stable schedule の互換 artifact として段階移行する。

### 2.1 外から見える compiler の数

通常利用者には一つの compile 操作として見せる。内部の安定 artifact 境界は次の三つに
限定する。

1. `CanonicalLogicalGraph`
2. `DomainPartitionedGraph` または同等の target execution artifact
3. backend ごとの `...PhysicalPlan`

`ExecutionLinker` は新しい意味を発明する optimizer ではなく、backend が返した bridge
realization、lifetime、completion を検証して閉じる linker / finalizer である。

material / shader compiler はこの直列段階へ押し込まない。material compiler は logical
contract と finite implementation candidate、shader compiler は SPIR-V / reflection 等の
immutable artifact を作る兄弟 compiler とし、graph compiler は snapshot を参照する。

## 3. 共通 typed IR kernel と dialect

### 3.1 共通にする機構

すべての dialect は次を同じ実装規律で利用する。

- 安定した `DialectId` / `TypeId` / operation id と version
- parameter schema、default、range、set、symbolic binding
- `TypePattern` と constraint expression
- capability requirement / provision の照合
- resource / state / external effect の宣言
- canonical ordering、stable hash、serialize / dump
- source / provider / rewrite の provenance
- structured diagnostic と rejected candidate reason
- registry owner / generation snapshot

ただし、全 field を optional にした一つの `UniversalType` や `UniversalNode` は作らない。
dialect が合法な constructor、parameter の組、effect を verifier で定義する。

### 3.2 初期 dialect

| dialect | 所有する意味 |
|---------|--------------|
| `logical.render` | scene color / depth、material、light、view、render pass contract |
| `logical.compute` | domain 未確定の計算 contract、または現行 GPU compute authoring adapter |
| `execution` | implementation 選択、endpoint、resource version、bridge obligation、粗い schedule |
| `physical.cpu` | job、affinity、continuation、CPU memory effect |
| `physical.vulkan` | image / buffer、scope、queue、barrier、alias、pipeline / descriptor request |
| `physical.external.*` | 別 API / service 固有の physical operation |
| `bridge` | domain 間の ownership、visibility、completion、interop |

新しい semantic feature は private namespaced dialect から開始できる。二つ以上の独立実装が
共有し、engine が変換の意味を理解する必要が生じたものだけを canonical dialect へ昇格する。

### 3.3 隣接 dialect だけを同時に扱う

lowering pass は source と直下の target dialect だけを同時に扱う。

```text
開始時: logical のみ legal
変換中: logical + execution が legal
完了時: execution のみ legal

開始時: execution.gpu のみ legal
変換中: execution.gpu + physical.vulkan が legal
完了時: physical.vulkan のみ legal
```

概念上、各 stage は次を宣言する。

```text
reads_dialects      = { logical, execution }
creates_dialects    = { execution }
must_eliminate      = { logical }
preserves           = { boundary types, effects, user pins }
```

完了後の scheduler が上位 dialect object を参照し続けてはならない。診断用には stable origin
id と provenance を残す。下位でも必要な上位情報は、明示的な constraint / hint / effect へ
lower する。

### 3.4 上位 operation と semantic evidence を分ける

`must_eliminate = { logical }` は、下位 scheduler が logical operation を再解釈できないという
意味である。次の immutable metadata まで削除するという意味ではない。

- origin fragment / node / port id
- boundary の正規化済み semantic type descriptor
- 適用した lowering / conversion rule id と version
- preserve した effect / pin
- candidate、probe、selection、rewrite の decision chain

これを `LoweringEvidence` と呼ぶ。physical scheduler は evidence を correctness の入力にせず、
dump、diagnostic、capture label、eject diff にだけ使う。下位最適化にも必要な意味は、単なる
annotation 参照でなく下位 dialect の constraint へ明示的に lower する。

## 4. compiler と linker の責務

### 4.1 LogicalCompiler — 何を達成するか

担当:

- preset / recipe / feature / fragment の展開と接続
- material / light / geometry / view / task requirement の収集
- semantic operation と typed port / value の構築
- target 非依存 conversion と default の挿入
- resource / state / external effect からの依存導出
- type、宣言済みeffect boundary、ambiguous writer、intra-epoch cycle の構造検証
- canonicalize、stable hash、logical decision log
- finite implementation candidate の保持

禁止:

- device / queue / module / OS handle の参照
- Vulkan format、layout、barrier、allocation の確定
- wall clock、runtime load、driver 列挙順を用いた非決定選択

出力は immutable とし、hot reload、別 target compile、dump、差分比較へ再利用する。

### 4.2 TargetExecutionCompiler — どこで、どの候補を使うか

入力は canonical graph、policy / pin、`TargetTopologySnapshot`、登録済み backend probe の
snapshot である。target compiler 自身が backend の詳細能力を推測しない。

担当:

- implementation candidate の feasibility / cost 評価
- host / device / external endpoint の選択
- graphics / compute / transfer 等の capability requirement の解決
- target-aware conversion、snapshot、resolve、upload / readback task の挿入
- logical node の融合・分割・削除、resource version の構築
- domain 内外の happens-before と並行可能性
- bridge obligation、residency / materialization requirement、粗い lifetime
- deterministic な candidate 選択と rejected reason

禁止:

- Vulkan enum、queue family index、CPU thread id の確定
- backend object の作成
- 未登録 implementation の発明や無制限 fixed-point 探索

target compiler は `TargetLoweringGraph` を unique-own し、canonical graph を変更しない。

### 4.3 BackendProbe — target / backend の循環を作らない

endpoint 単体の capability だけでは format、memory、interop、bridge の成立性を判定できない。
各 backend は object 作成や graph mutation を行わない probe を提供する。

```text
BackendProbeInput
  implementation candidate
  canonical boundary / resource requirements
  target topology snapshot
  shader / kernel interface facts
  policy / pin

BackendProbeResult
  status: feasible | rejected
  refinement constraints
  deterministic cost vector
  bridge offers
  required physical features
  reason / provenance / provider fingerprint
```

標準 planning は次の一方向手順に固定する。

1. recipe / strategy が rewrite recipe を含む有限 candidate を列挙
2. 各 candidate を対応 backend probe へ渡す
3. rejected candidate と理由を記録
4. refinement constraint と topology relation を解く
5. deterministic policy で一候補を選ぶ
6. target lowering workspace へ選択済み rewrite を適用
7. backend lowerer が probe result を具体化する

probe は compiler 層ではなく、target compiler が読む純粋な feasibility / cost provider である。
backend lowerer が probe で宣言していない capability 不足を後から発見した場合は、暗黙 fallback
や無制限 replan をせず provider / compiler inconsistency として名前入りで拒否する。device
object 作成時の一時的失敗は compile decision でなく prepare rollback とする。

### 4.4 BackendLowerer — endpoint の機構でどう実現するか

#### CPU

- job dependency、affinity class、continuation / future
- shared / exclusive CPU memory effect
- main-thread / worker / dedicated service requirement
- deterministic stage と backend completion token

CPU scheduler の対象は粗粒度 task とする。ECS system 内部や物理 library 内部の worker
分割を二重に管理しない。

#### Vulkan

- graphics / compute / transfer と将来の specialized device operation
- concrete image / buffer / memory / alias / transient plan
- render scope、tile-local relation、load/store/resolve
- queue capability から queue family / submission への割り当て
- layout、stage/access、barrier、semaphore
- shader / descriptor / pipeline request
- external acquire / release と device-local completion

graphics、GPU compute、transfer は同じ resource graph を共有するため、一つの Vulkan plan
として global に lower / validate する。specialized operation は capability / dialect を追加し、
すべてを compute dispatch に偽装しない。

#### External

別 GPU API、OS codec、network、storage 等を統合する場合の sibling plan である。backend
固有 object は runtime executor が所有し、compile artifact は versioned data-only descriptor
にする。

### 4.5 ExecutionLinker — backend plan を閉じる

linker は次を行う。

- bridge obligation と各 backend の realization を一対一に対応付ける
- cross-domain resource ownership / visibility / completion を検証する
- endpoint 間の lifetime を閉じる
- component の root / sink と liveness を検証する
- frame / epoch completion を定義する
- final plan hash、dump、provenance map を生成する

linker は不可能な bridge を黙って CPU copy 等へ置換しない。fallback は target compiler の
finite candidate として選び直し、理由を残す。

### 4.6 prepare / publish は compiler ではない

runtime preparer は physical plan から CPU executor state、Vulkan object、external session 等を
side state に作る。全 domain の prepare が成功した場合だけ frame boundary で一括 publish
し、失敗時は candidate だけを rollback する。旧 plan は各 domain の in-flight completion
後に retire する。

### 4.7 material / shader / graph artifact の循環を避ける

material、shader、graph、pipeline を相互 callback で compile しない。artifact dependency を
次に固定する。

```text
material source
  -> MaterialSemanticArtifact
       logical contract
       finite implementation candidates
       declared ShaderInterfaceContract

shader source + compile request
  -> ShaderArtifact
       binary / reflection / content hash

MaterialSemanticArtifact + ShaderArtifact summaries
  -> Logical / Target compile
  -> selected implementation + Vulkan pipeline request
  -> pipeline prepare
```

target compiler は reflection から semantic type や effect を推測しない。implementation が
宣言した `ShaderInterfaceContract` を planning 入力とし、reflection は宣言との一致検証に使う。
optional candidate の shader compile が失敗した場合は、その candidate を reason 付き rejected
とする。candidate 集合は compile 開始時に snapshot し、途中で新候補を生成しない。

## 5. endpoint と capability

### 5.1 閉じた domain enum を避ける

論理 contract は「GPU queue 名」でなく capability pattern を要求する。

```text
semantic operation: culling

candidate A:
  endpoint_pattern = host
  requires = { cpu.simd }

candidate B:
  endpoint_pattern = device
  requires = { device.compute, resource.indirect_write }
```

endpoint は概念上次を持つ。

```text
ExecutionEndpoint
  stable endpoint id
  endpoint class: host / device / external
  capability set
  memory / interop domains
  owner generation
```

`endpoint class` は routing の粗い閉じた構造であり、機能一覧ではない。新機能は
namespaced capability と backend dialect で追加する。

### 5.2 TargetTopologySnapshot

`CapabilitySet` は endpoint 単体の能力しか表せない。複数 endpoint の共有・転送・同期可否は
directed relation として snapshot する。

```text
TargetTopologySnapshot
  endpoints[]
    id / class / backend
    capability set
    memory domains
    owner / generation / facts hash

  links[]
    from / to endpoint
    supported bridge mechanisms
    compatible resource / format patterns
    ownership and synchronization mechanisms
    copy / zero-copy / staging requirements
    latency / bandwidth cost class
    required extensions / external handle classes
```

同じ物理 GPU でも Vulkan と external API は別 endpoint / memory domain になり得る。zero-copy
可否を両 endpoint の capability の積から推測せず、link fact として明示する。複数 link が
成立する場合は finite bridge candidate として probe が cost と理由を返す。

snapshot は composition root が runtime device / OS 情報から作る data-only artifact であり、
device、queue、native handle を含めない。列挙順は stable id で canonicalize する。

### 5.3 requirement と selection を分ける

```text
logical requirement:
  one_of { host, device.compute }

selected execution:
  endpoint = device:0
  implementation = project://culling/gpu_v2

physical Vulkan selection:
  queue_family = ...
  queue_index = ...
```

logical authoring、selected endpoint、physical queue を一つの enum fieldへ詰めない。

### 5.4 現行 `kind` の扱い

`pelican.frame_plan` v1 の `nodes[*].kind = "render" | "compute"` は現行 config の
authoring category / diagnostic であり、execution endpoint ではない。互換のため v1 は
変更しない。

将来の dump schema は別 field として少なくとも次を持つ。

```text
semantic_dialect
selected_implementation
selected_endpoint
required_capabilities
provided_capabilities
bridge_ids
```

CPU 対応を単に `kind: "cpu"` の追加だけで完了させない。

## 6. GraphFragment、forest、closure

### 6.1 fragment boundary

すべての authorable fragment は概念上次を持つ。

```text
GraphFragment
  id / version / owner generation
  dialect
  typed imports
  typed exports
  nodes / values / internal effects
  external effect imports / exports
  constraints / preferences / pins
  provenance / region tags
  boundary policy
```

必須 input の producer が fragment 内にないこと自体は error ではない。undeclared な欠落を
禁止し、typed import として宣言させる。import は接続時に exact match、登録済み conversion、
optional/default、external binding のいずれかへ一意に解決する。

### 6.2 open fragment と closed forest

| 状態 | 未解決 import | 非連結 component | 保存 / 合成 | publish |
|------|---------------|------------------|-------------|---------|
| open fragment | 宣言済みなら可 | 可 | 可 | 不可 |
| linked logical graph | 原則不可。external / optional は契約化 | 可 | 可 | logical artifact として可 |
| domain-partitioned graph | bridge obligation として可 | 可 | compiler 内のみ | 不可 |
| backend physical fragment | physical boundary import として可 | 可 | direct authoring / lowering 内で可 | 不可 |
| final execution plan | 不可 | 可 | immutable | 可 |

最下層で禁止するのは disconnected graph ではなく unresolved graph である。複数 window、XR、
async upload、capture、readback、独立 simulation は自然に forest になる。

### 6.3 root、sink、liveness

各 closed component は少なくとも一つの観測可能 sink を持つ。

- present / external submit
- external write / packet / file
- history / persistent-state commit
- capture / readback
- completion signal / requested future
- 明示 `KeepAlive`

sink から逆到達できず、外部 effect もない node は dead code として削除または名前入り警告
にする。アルゴリズムが単一 root / sink を必要とする場合は virtual `PlanBegin` /
`PlanComplete` を診断上だけ追加し、component 間に偽の逐次依存を作らない。

### 6.4 fragment は最適化境界ではない

fragment / region は ownership、hot reload、置換、dump grouping の単位である。型・effect・pin
を守る限り、compiler は fragment を越えて fusion、alias、schedule、dead-code elimination を
行える。

境界を保護したい場合だけ、目的別に明示する。

- `FrozenRegion`: node 構造の置換を禁止
- `ManualRegion`: 内部 physical 計画を利用者が所有
- `NoFuse`: scope / operation 融合だけを禁止
- `IsolationBoundary`: cross-boundary alias / scheduling を禁止
- `Materialize`: 境界 value の物理実体を要求

一つの汎用 `opaque = true` へ異なる意味を詰めない。

### 6.5 effect の作者責務と低負担 default

effect を一つの層へ集めない。責務を次に分ける。

| 情報 | 所有者 | 未指定時 |
|------|--------|----------|
| typed input / output の data use | compiler が port から導出 | 必須 port がなければ structural error |
| logical state / external effect | logical contract 作者 | effect なしという作者の宣言として受理 |
| CPU scheduling restriction | implementation 作者 | 追加制約なし。依存がなければ parallel / reentrant 候補 |
| GPU / Vulkan access、layout、sync | backend lowerer | logical / execution use から自動導出 |
| NativeScope 内部 effect | native 作者 | 宣言 boundary が完全とみなし、追加 isolation なし |

logical 作者が必要に応じて宣言する semantic effect は少数に保つ。

- `StateRead` / `StateWrite`: history、persistent state、session
- `ExternalRead` / `ExternalWrite`: present、file、network、OS / external API
- `NondeterministicInput`: wall clock、unseeded source 等を意図的に利用
- explicit ordering edge: data / state では表せない観測順

通常の image / buffer read/write はportとresource useから導出し、同じ情報を `effects` に
二重記述させない。effect field を必須にせず、engine は custom shader / callback の数学的
意味が宣言どおりか証明しない。

CPU task は data / effect edge が競合しなければ並列 lane の候補にする。`serial`、
`non_reentrant`、`main_thread`、`exclusive`、`blocking` は実際に必要な implementation だけが
制約として宣言する。未宣言 global state や隠れた同期があれば作者側の contract 違反であり、
engine はその可能性だけを理由に全 task を直列化しない。

Native / manual physical scope も宣言 boundary を完全な contract として扱う。automatic mode は
typed resource use と operation class から最小限の外部同期を導出し、詳細 annotation がない
ことだけを理由に全 scope barrier、alias 禁止、queue overlap 禁止を挿入しない。model 外の
access / synchronization が必要な作者は `manual`、内部検証も不要なら局所 `unchecked` を選び、
その範囲の correctness を所有する。

### 6.6 state と epoch

frame / stream を跨ぐ state は同一 epoch の cycle にせず、versioned value / effect とする。

```text
State<N> + Input<N> -> Output<N> + State<N+1>
```

history、encoder session、streaming residency 等はこの一般形へ接続できる。ただし各 feature
固有の状態 schema と runtime executor は需要時に別設計する。

### 6.7 fragment変更と再compile

fragment は最適化境界ではないため、一 fragment の変更が scope fusion、alias、endpoint、
bridge、resource lifetime を全体で変え得る。初期版は incremental compile を約束せず、
canonical graph から全 target / backend candidate を side compile して atomic publish する。

dump には fragment input/output fingerprint と decision dependency を残すが、部分再compile は
同じ結果を得られる invalidation rule が実証されてから導入する。利用者が局所安定性を必要と
する場合は `FrozenRegion` / `IsolationBoundary` / pin を使い、compiler が暗黙に最適化範囲を
狭めない。

## 7. scheduler の切り分け

| scheduler / planner | 入力 | 決めること | 決めないこと |
|---------------------|------|------------|----------------|
| logical scheduler | typed value / effect dependency | DAG、level、安定 tie-break | endpoint、queue、barrier |
| target execution planner | logical graph、candidate、facts | implementation、endpoint、bridge、coarse overlap | Vulkan enum、thread id |
| CPU scheduler | CPU physical tasks | job dependency、affinity class、continuation | GPU barrier |
| Vulkan GPU scheduler | GPU physical work | scope、queue、barrier、alias、submission | CPU worker scheduling |
| execution linker | backend summary / bridge | cross-domain completion、lifetime、final closure | backend 内部最適化 |

共通化するのは deterministic graph traversal、resource version、effect edge、topological
validation、plan dump 等の library である。cost model、queue selection、work stealing を一つの
generic scheduler callback にしない。

依存のない component の順序は意味上 unspecified とし、宣言順は同じ妥当解の stable
tie-break にだけ使う。性能のために順序を固定する場合は explicit scheduling constraint、
正しさのためなら data / state / external effect edge を使う。

### 7.1 validation severity

既定 policy は `advisory` とし、compiler を利用者の意図を上書きする安全装置にしない。

hard error は、engine が計画を構築・実行できない次に限定する。

- malformed schema / id / version、未解決の必須 import / reference
- 一意に解けない writer / conversion、schedule を作れない intra-epoch cycle
- required capability / exact user pin を満たす candidate がない
- resource ownership / lifetime / bridge が閉じず final plan を作れない
- engine が生成・発行することになる既知の backend API 違反

次は既定でwarningまたはinfoにする。

- manual / native scope の疑わしい synchronization / effect
- portability の低下、fallback、過剰 materialization、queue ping-pong
- deterministic でない可能性、未使用 output、dead component
- 明示 `serial` / `isolate` / `no_alias` による parallelism、fusion、alias の損失

logical effect の未記述だけを理由にwarningを出さない。宣言は作者の責任であり、engine が
隠れたeffectを知っているふりをしない。静的に矛盾を検出できた場合だけ stable diagnostic id
付きで通知する。

project / CI は個別 warning id を `strict` としてerrorへ昇格できる。warningはcompileごとに
一回だけ出し、fragmentまたはprojectで理由付き抑止ができる。全warningを常時errorにする
engine既定は置かない。`NativeScope` / manual physical region は局所 `unchecked` を選べるが、
その内部はengineのcorrectness保証、automatic synchronization、portability対象外になる。

診断用の `conservative_debug` profile は、CPU直列化、alias / fusion無効化、広いbarrierを
一時的に選べる。ただし通常profileへ自動fallbackせず、plan hash / dumpに明示し、releaseの
既定性能を変えない。

逆方向の診断として `hazard_stress(seed)` を用意する。宣言上合法な順序、overlap、fusion、
alias 候補を seed に従って積極的に揺らし、通常順序が偶然隠していた undeclared dependency を
露出させる。同じ seed は byte-equivalent な plan を再現し、seed と decision を dump に残す。
これは cost-optimal な release policy ではなく opt-in の CI / debug policy である。

## 8. bridge contract

### 8.1 obligation

target compiler は具体 API call でなく、次のような obligation を作る。

```text
BridgeObligation
  producer endpoint / value version
  consumer endpoint / required visibility
  transfer / shared-memory candidates
  ownership requirement
  completion semantics
  latency / blocking constraint
  fallback policy
```

### 8.2 代表例

```text
CPU -> Vulkan:
  host write -> flush or upload -> device-visible value -> queue dependency

Vulkan -> CPU:
  device result -> copy/readback -> timeline/fence -> invalidate -> CPU future

Vulkan queue -> Vulkan queue:
  release/acquire or shared ownership -> semaphore/barrier

Vulkan -> external backend:
  exportable resource / copy -> external ownership -> interop completion
```

同じ物理 device 上でも API / memory domain が異なる場合は bridge を省略しない。zero-copy は
capability が証明された候補であり、暗黙の仮定ではない。
bridge候補の成立性は `TargetTopologySnapshot.links` と両 backend のprobeでtarget選択前に
確認する。`ExecutionLinker` は新しいcopy / interop方式を発明せず、選択済みofferの両端が
一致することだけを検証する。

## 9. target compile の破壊的 rewrite

```text
Authored fragments
  -> elaborate / validate / canonicalize
CanonicalLogicalGraph              immutable
  -> materialize target workspace
TargetLoweringGraph                unique-owned / mutable
  -> destructive rewrite / lower
DomainPartitionedGraph             immutable boundary artifact
```

破壊可能かは「engine が追加した node か」だけで決めない。

| 分類 | 許可 |
|------|------|
| pure operation | equivalence と contract 保存を証明できれば融合・削除可 |
| compiler synthetic | obligation が下位表現へ移った後に削除可 |
| external / state effect | 観測可能な順序と結果を保存しなければ削除不可 |
| capture / debug probe | requested sink である限り削除不可 |
| user pin / frozen / manual boundary | 宣言した保護規則を越える rewrite 禁止 |

全 rewrite は origin id、rule id/version、入力 facts、選択理由、置換先を記録する。compile は
side state で行い、失敗時に canonical graph や現行 runtime plan を破壊しない。

cache key は少なくとも canonical graph hash、registry generations、target facts、policy / pin、
compiler version を含む。一時 lowering graph の pointer identity を使わない。

## 10. 特殊 GPU 機能を後から追加する条件

新機能は次の要素を個別に追加できれば、共通 compiler を改造せず統合できる。

1. semantic operation / type の dialect descriptor
2. typed input / output / state / external effect contract
3. finite implementation candidate と capability requirement
4. target cost / feasibility provider
5. backend physical operation / lowering
6. bridge realization(必要な場合)
7. verifier、dump、executor、purge fixture

ray tracing、acceleration structure build、sparse residency、specialized copy 等をすべて
`compute` に変換しない。Vulkan が直接表せる機能は `physical.vulkan` 内の operation とし、
別 API が必要なら sibling `physical.external.*` と bridge を使う。

### 10.1 動画は実装せず、構造だけ検証可能にする

動画 encode/decode は本書の実装ロードマップへまだ入れない。core に codec、chroma、rate
control、session enum を予約しない。一方、将来次を core enum 変更なしで表現できることを
設計 acceptance とする。

- namespaced `video encode` capability を要求する semantic operation
- Vulkan specialized operation、external hardware backend、CPU fallback の候補
- frame image から backend input への typed conversion / materialization
- versioned persistent session state
- async completion と入力 resource lifetime
- bounded queue、backpressure、drop / block policy
- bitstream を file / network / CPU consumer へ渡す external sink

動画固有 schema は需要時に `logical.video` 等の private dialect で開始し、共通化が実証される
まで logical core へ昇格しない。フレームグラフは enqueue / completion / resource lifetime を
扱い、長時間生存する codec session は persistent executor が所有する。

## 11. determinism、hot reload、purge

- logical / target / backend / linker は同じ snapshot 入力から byte-equivalent plan を作る。
- provider registry は owner、generation、version、content hash を snapshot する。
- candidate 探索は有限で、同 cost の tie-break は stable id / declaration order で固定する。
- compile / prepare / publish は全 domain を一つの transaction generation として扱う。
- provider unload は新規 lease を止め、compile / prepared candidate / in-flight execution の
  lease 解放後に retire する。
- 未選択 implementation は runtime object、thread、queue、sessionを作らない。
- optional backend は別 build unit / game DLL に分けられ、binary purge できる。
- typed IR kernel、validation、plan linker の小さな mechanism は core に残る。

## 12. 使いやすさと progressive disclosure

compiler / dialect の内部構造を通常 authoring syntax に露出させない。利用者ごとの入口を
次に固定する。

| 利用者 | 通常触るもの | 触らなくてよいもの |
|--------|--------------|--------------------|
| ゲーム作者 | preset、少数settings、material / feature参照 | dialect、effect、endpoint、probe、physical plan |
| shader / feature作者 | 標準type alias、typed port、fragment、必要時だけlogical effect | Vulkan sync、queue、topology |
| renderer改造者 | strategy / transform、candidate、pin、plan dump | backend object lifecycle |
| backend作者 | topology facts、probe、lowerer、physical verifier / executor | project preset UX |

### 12.1 standard alias と推論

普通のfragmentでは完全修飾type schemaを毎回書かせない。

```text
SceneLinearHdr
DeviceDepth
LinearViewDepth
VisibilitySet
```

等のversioned standard aliasと、pass / material contractからのport推論を用意する。完全修飾
`TypeId`、parameter、constraintはejectまたはadvanced builderでだけ見せる。custom typeは
project namespaceを一度宣言し、その後はaliasで参照できる。

自動配線は、scope内に型とeffectが一致する候補が一つだけある場合に限る。候補が複数なら
推測で選ばず、候補名と接続例を示す。明示wireは常に自動解決より優先する。

### 12.2 未指定は注釈要求でなく既定動作

- logical effect未指定: data dependency以外のeffectなしとみなし、独立nodeを自由に並べ替える
- CPU scheduling restriction未指定: worker / parallel / reentrant候補
- implementation pin未指定:標準deterministic policyが選ぶ
- materialization未指定: virtual / compiler choice
- native boundary詳細未指定: 宣言済みtyped useからminimal syncを導出し、追加isolationなし
- alias / fusion policy未指定: legalityとcostが許せば有効
- diagnostic policy未指定: advisory
- conservative debug policy未指定: 無効
- hazard stress policy未指定: 無効

通常の高速経路にpositiveな `parallel_safe` boilerplateを要求しない。遅くする必要がある人だけ
`serial` / `exclusive` / `no_alias` / `isolate` を追加する。特殊queue、zero-copy interop等の
target固有hintは追加最適化のために任意で指定できるが、一般的な並列化・fusionの前提にしない。

### 12.3 一つのcompile結果と説明可能性

通常APIは一回のcompile requestと一つのresultにまとめる。内部stageごとの例外を直接返さず、
diagnosticには少なくとも次を含める。

- 元project / fragment / node / port
- expected / actual typeと使用したalias
- selected implementation / endpoint / bridge
- rejected candidateと理由
- warning id、抑止方法、strict時の扱い
- 修正候補またはejectできる層

toolingは層別dumpに加え、次の質問へ答えられる形を目標にする。

```text
why was this implementation selected?
why was this resource materialized?
why did these components serialize?
why was zero-copy / tile-local rejected?
what changes if this decision is pinned?
```

plan dumpを読まない利用者には要約だけを出し、詳細decision chainは要求時に展開する。

### 12.4 compilerを書かせない拡張

新しいlogical featureの通常作業はtype / contract / implementation descriptor / shaderまたはCPU
kernelの追加であり、compiler一式の実装ではない。target最適化はnamed rewrite一つ、Vulkan
特殊機能はprobe + lowering operation一つ、新backendだけが`BackendLowerer`一式を必要とする。

通常のgraphics / compute implementationはstandard `ResourcePattern`、shader interface、
capability constraintをdataで宣言し、共通Vulkan probeを利用する。passごとにprobe callbackを
書かせない。custom probeが必要なのは、新しいphysical mechanism、特殊queue、external interop
等を追加する場合だけとする。

標準実装も同じdescriptor / registryをdogfoodするが、通常feature作者へprovider ABI、lease、
topology構築を要求しない。project asset、C++ builder、eject結果は同じcanonical logical artifact
へ収束させる。

## 13. direct authoring と escape hatch

各層で同じ深さだけを編集できる入口を用意する。

1. preset / settings
2. logical fragment / transform / strategy
3. target policy / implementation pin
4. CPU または Vulkan physical fragment
5. complete backend physical plan
6. boundary contract 付き native / external scope

physical fragment も open boundary を持てるが、final execution plan へ link する前に閉じる。
native scope 内部は engine optimizer の対象外とし、入力・出力、宣言されたeffect / ownership /
completion、required capabilityだけを検証する。未指定情報は隠れた制約なしという作者の宣言
として扱い、宣言済みboundaryからautomatic minimal syncを作る。`manual` / 局所`unchecked`
なら内部correctnessと同期責任を作者へ移す。

上位を bypass するほど portability、自動最適化、診断、hot reload 保証が減る。eject / dump は
同じ層の round-trip のみ保証し、physical plan から logical graph を復元しない。

2026-07-26 の WP204 Phase A では、この梯子の 3 と 4 の間に最小の実行可能な境界を置いた。
portable な `target_planning` は profile / node / resource constraintを通常compilerへ渡し、
Vulkan固有の `pelican.vulkan_target_plan_pins` v1 はcompiled logical fingerprintと有限な
backend candidateだけを同じphysical層へeject/importする。これはphysical fragmentそのもの
ではない。

同日のWP204 Phase B v1では梯子の4を狭いverified subsetとして実装した。
`pelican.vulkan_physical_fragment` v1はautomatic physical planを基準に、conservative
resource materialization、automatic scope内のsplit、compatibleなalias groupを
same-layerでeject/importする。logical graphに加えてtarget/device factsとprovider
generationをfingerprintへ束縛し、scope boundary、lifetime、sample/view/extent、
required capability closureをlink時に再検証する。これはopen fragmentやcomplete raw
physical planではない。

WP204の次sliceでは、logical `ResourcePattern`へrender targetの`format_candidates`を
運び、別formatを選ぶresourceだけをtarget固有のdevice evidenceへ照合する境界を追加した。
required image usage、sample count、array layer、external-depth transfer-sourceを満たす
`materialized_image`だけを許可し、選択結果をsample planとruntime GPU resourceへ適用する。
続くattachment-operation sliceではversion 2 physical fragmentのper-attachment load/storeを
logical dependencyとMSAA resolve契約へ照合し、dynamic renderingへ接続した。
transient-runtime sliceではwrite-only、single-sample、attachment-only resourceだけに独立した
backend候補を与え、device/format gate後にStore discard、transient image usage、lazy-memory
preferenceへloweringした。一般のmaterialized Store elision、scope fusion、queue/barrier、
native/external boundaryは対応verifierができるまで受理しない。

## 14. 段階導入

### HEG0 — 設計予約(本書)

- typed dialect / capability / topology / probe / endpoint / fragment / forest の語彙を固定
- 現行 `kind` と selected execution domain を分離
- trust-first effect / optimize-by-default / advisory validation / progressive disclosure を固定
- CPU / 動画 runtime は追加しない

### HEG1a — RPE6b0 canonical logical value fixture(実装済み)

- resource family + version の value と version 0 import を導入
- exact producer / consumer port から data edge を導出
- version の大小や node 配列順を dependency とみなさない
- nominal connection、access intent、conversion implementation descriptor を検証
- runtime / Vulkan execution ownership は変更しない

### HEG1b — RPE6b1 screen-input logical fixture(実装済み WP187)

- screen input の source/sample type、read footprint、depth conversion を render dialect の
  実例として実装
- `hybrid_v1` の named pass input、opaque color/depth snapshot、material descriptor set へ接続
- tone-map / display encode は explicit-only conversion と terminal 一回 fixture で固定
- generic fragment/provider framework は導入せず、追加 alias / provider registry は RPE6c 以降へ残す

### HEG2a — RPE6c0 planning contracts(実装済み WP188)

- data-only `TargetTopologySnapshot` と directed endpoint link
- pure `BackendProbeInput` / `BackendProbeResult` と有限候補選択
- stable warning id、advisory / opt-in strict policy
- logical data dependencyの自動導出、optional semantic effect、parallel eligible CPU defaultの型予約
- shader interface declarationとreflection一致のfixture
- runtime / Vulkan execution ownershipは変更せず、CPU / video APIを追加しない
- conversion / target-lowering providerを同じdata descriptorでsnapshotし、reloadable世代だけ
  compile snapshotがgeneration leaseを保持
- `optimized` / `conservative_debug` / `hazard_stress(seed)`を純CPU decision reportとして実装。
  alias集合のlifetime導出、domain partition、bridge実行はHEG2bへ残す

### HEG2b — target planner vertical slice / runtime接続(実装済み WP189 / WP191)

- immutable canonical graph と disposable `TargetLoweringGraph` を分離
- mock desktop / tile topologyとVulkan probeでtarget-aware rewrite / physical planを検証
- target execution と Vulkan lowering の内部 seam、dialect legality verifier を置く
- selected probeからlowering後に新しいrequired capabilityが生えないことを検証
- CPU / external lowerer は null fixture または型予約に留める
- `ResourcePattern`とresource bindingを分離し、追加G-bufferを固定enumなしで計画
- read footprint / materialization / lifetimeからtile-local、snapshot、alias候補を導出
- WP191でmaterialized imageのformat / sample-count contractだけを現Vulkan runtimeへ接続
- WP204 Phase Aでgraph-scoped target policyと、logical fingerprint付きbackend decision
  pinのeject/importをruntime target compilerへ接続
- WP204 Phase B v1でenvironment-bound physical fragment、conservative resource
  materialization、split-only scope、alias verifierを同じruntime target compilerへ接続
- WP204 alternate-format sliceで宣言済み候補、device capability evidence、
  runtime format assignmentとhot-reload世代交換を同じ経路へ接続
- WP204 attachment/transient sliceでverified load/storeとwrite-only transient imageを接続
- tile-local / aliasのruntime実行、一般のload/store等のaggressive physical control、NativeScope、
  CPU・external・video runtime workはまだ追加しない

### HEG3 — 実証後の異種 domain

- CPU frame profile で粗粒度 task の必要性を確認
- CPU / GPU の二候補を持つ一つの実用 task で domain partition を実証
- upload / readback bridge、CPU physical plan、execution linker を追加
- ECS 内部並列化や常駐 loader を無理に移行しない

### HEG4 — 具体的 external / specialized backend

- 実需要がある一機能で namespaced capability と backend dialect を追加
- bridge / purge / hot reload / device-loss fixture を成立させてから public ABI を凍結
- 動画はこの段階の候補にできるが、優先機能とはしない

## 15. acceptance scenarios

1. **現行 renderer**: CPU / external task を登録しない `hybrid_v1` が現行と同じ plan と絵を
   生成し、異種基盤の runtime cost を持たない。
2. **GPU compute**: render outputをGPU computeが読み、その結果をrenderが読む構成を一つの
   Vulkan planでbarrier / queue / aliasまで解決する。
3. **CPU/GPU候補**: 同じ culling contract のCPU実装とGPU compute実装をtarget factsで選び、
   logical graphを書き換えずに二計画を生成する。
4. **closed forest**: present、async upload、captureが非連結componentでも、各sinkとlifetimeが
   閉じていればfinal planとして受理する。
5. **fragment composition**: feature / game DLL fragmentのtyped importを名前付きで接続し、
   未接続import、曖昧変換、宣言済みexternal/state effect boundaryの不整合をcompile時に
   拒否する。隠れたeffectの完全性は検査しない。
6. **特殊GPU機能**: namespaced capabilityとbackend operationをplugin/build unitで追加し、
   common domain enumやuniversal nodeを変更しない。
7. **動画の将来追加**: codec実装なしの設計fixtureで、persistent state、async sink、backend
   candidate、bridge、backpressureを既存の共通契約だけから構成できる。
8. **直接改造**: user-authored physical fragmentをtyped boundaryで標準planへlinkし、内部を
   logical optimizerへ逆変換しない。
9. **決定性とreload**:同じsnapshotから同じforest/hashを得て、全backend candidateのprepare
   成功時だけ一括publishする。
10. **topology**: 同じendpoint capabilityでもbridge linkの有無で候補可否が変わり、理由を
    target選択前に得る。
11. **probe一方向性**: selected candidateがbackend loweringで未知capabilityを要求せず、linkerが
    fallbackを発明しない。
12. **低負担authoring**: preset利用者はeffect / topology / probeを書かず、feature作者も標準
    aliasとtyped portだけで純粋passを追加できる。依存のないnodeは追加注釈なしで並列化・
    fusion候補になり、warningはcompile時のadvisoryに留まる。

この十二を一つの万能node、万能scheduler、万能provider ABIで満たそうとしない。共通 kernel、
隣接 dialect lowering、backend別 verifierを小さく保ち、実証済みの境界だけを公開する。
