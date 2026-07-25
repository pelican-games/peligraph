# レンダーパイプライン拡張境界とポリシー統合(v2.4)

対象読者: レンダラ実装者、独自描画方式を組み込むゲーム実装者。

ステータス: v2.4 実装方針(2026-07-24。v1: 2026-07-21)。`hybrid_v1` の
deferred/forward 基盤を出発点とする。v2 では renderer 構築を論理／ターゲットの
二段階コンパイラとして定義し、論理型、Vulkan 物理計画、物理グラフ直書き、
`NativeScope` の境界を追加した。詳細は
[`design_render_graph_compiler.md`](design_render_graph_compiler.md) を正とする。
v2.1 はこの renderer 二段階経路を互換 facade として維持しつつ、CPU / GPU compute /
external backend を縦層でなく横 domain として追加する境界へ接続した。異種 execution の
共通規則は
[`design_heterogeneous_execution_graph.md`](design_heterogeneous_execution_graph.md) を正とする。
本文の層・語彙・所有規則は以後の実装判断の正とするが、
公開 provider ABI は個別の実装 fixture が揃ったものから凍結する。
`DrawSortProviderV1` の v1 バイナリレイアウトは WP183 の public game-DLL fixture を
もって凍結済み(2026-07-22)である。WP184 は `DrawSortInputV1` の既存 prefix を変えず、
`struct_size` で検出する logical-view snapshot を末尾追加した。旧 provider が読む prefix
の byte extent は public header の static assertion で固定している。
RPE6b0 / WP186 では logical value の版と producer edge、nominal connection、access intent、
conversion implementation descriptor を追加した。RPE6b1 / WP187 はその型契約を
`hybrid_v1` の opaque snapshot と material descriptor へ接続した。RPE6c0 / WP188 と
RPE6c1 / WP189 はtarget topology/probe、`ResourcePattern`、desktop/tile physical plan fixture、
dialect legalityを純CPUで追加したが、汎用logical graphの実行所有権は引き続き既存
`FramePlan` / Vulkan executorにある。RPE7 / RPE8 runtime slice / WP190 は
`SampleCountPolicy` と capability resolve を typed compiled plan へ追加し、現在の
`FramePlan` / Vulkan executor に実 MSAA image、pipeline sample count、color/depth resolve
を接続した。WP191 はその runtime bridge を WP189 の `VulkanTargetPlan` へ統合し、
physical format / representation / sample-count contract を実行時にも同じ lowering
結果から消費する。RPE9 / WP192 は graph variant policy を同じ compiled plan へ統合した。
RPE10a / WP193 は pass、frame graph、logical/physical plan、route、sample、variant policy と
draw-sort provider 選択を一つの immutable runtime generation として原子的に公開する root を追加した。
RPE10b1 / WP194 は render-config が触る GPU registry 群に共通 checkpoint / rollback を追加し、
owner scope 付き immutable GPU arena manifest を同じ root へ原子的に公開する。
RPE10b2 / WP195 は同じ owner scope の置換、世代ローカルの typed resource binding、
generation-owned registry lease を追加した。RPE10b3 / WP196 はproject-backed config /
feature / preset watcher、flat/XR一括prepare・単一CAS、実submission fenceが保持する
generation leaseを接続した。旧 frameが参照したregistry membershipは、そのCPU frameが
終わった時点ではなく最後の対応GPU fence完了後にretireする。
RPE11a / WP200 は最初の公開 `PassImplementation` 境界として fullscreen shader pair
差し替えを実装し、builtinとgame DLLを同じtyped contract / owner-aware registry /
transaction snapshot経路へ通した。RPE11b / WP201 はgraph構造を変更する別境界として
tagged region / subgraph replacementを実装した。元のlogical graphを破壊せず、
置換後のlocal candidateをtyped graphへ再compileしてからtarget loweringへ渡す。
global transformとstrategyはこの二つの小さなABIへ押し込まず、RPE11c / RPE11dで
独立したextension pointとして実装した。
RPE12a / WP204 Phase A はさらに下流の target planning control と、compiled logical
graph fingerprint に束縛された versioned Vulkan backend decision pin を実装した。
自動 plan の dump は同じ物理層へ戻せる pin package を常に出し、古い graph、
未知 candidate、現在の device で infeasible な candidate は fallback せず reject する。
RPE12b / WP204 Phase B v1 はその下へverified physical fragmentを追加した。自動planを
基準にconservative resource materialization、split-only scope partition、compatibleな
alias groupだけをlinkし、logical/environment fingerprint、boundary、lifetime、feature
closureを再検証する。後続sliceではrender targetが宣言した`format_candidates`から
materialized-image formatを選び、実deviceのusage/sample/layer/external-depth capabilityを
再検証してruntime generationへ適用する経路まで追加した。load/store、queue/barrier、
scope fusionはaggressive follow-upまでcompiler-ownedに残す。

関連文書:

- [`design_render_graph_compiler.md`](design_render_graph_compiler.md) —
  二段階 compiler、論理型、target planning、物理 IR / NativeScope
- [`design_heterogeneous_execution_graph.md`](design_heterogeneous_execution_graph.md) —
  typed dialect、domain partition、CPU / Vulkan sibling lowering、fragment / closed forest
- [`design_render_feature_modules.md`](design_render_feature_modules.md) —
  purgeable feature と 1 行有効化
- [`design_material_shading.md`](design_material_shading.md) —
  material route / shader ABI / `.surface`
- [`design_compute_task_graph.md`](design_compute_task_graph.md) —
  graph の依存導出と実行計画
- [`design_asset_hot_reload.md`](design_asset_hot_reload.md) —
  prepare / publish / rollback / retire
- [`design_openxr.md`](design_openxr.md) — 論理 frame/view と OpenXR lifecycle
- [`design_physics_queries.md`](design_physics_queries.md) —
  versioned provider と game-DLL 差分実装の先行例

## 0. 決定したいこと

レンダリングには相反して見える要求がある。

1. 普通のプロジェクトでは preset と少数の設定だけでよい
2. material は性質から deferred / forward へ自動振り分けしたい
3. 詳しい利用者は全 content pass、描画順、XR variant、MSAA 等を変更したい
4. ゲームで不要な方式は runtime resource も配布 binary も可能な範囲で
   purge したい
5. Vulkan resource lifetime、barrier、swapchain、OpenXR session 等の実行機構は
   壊れやすいため、通常の pass 設定と同じ自由度では公開しない

回答は、普通の入口を **preset** に保ったまま、必要に応じて resource pattern、pass、
subgraph、global transform、renderer strategy、Vulkan physical plan、`NativeScope` まで
段階的に降りられる構造である。標準経路は **logical compile → target compile** の
renderer facade を維持し、物理層を直接所有する利用者は logical compile を迂回できる。
内部では target compile を `TargetExecutionCompiler` と `VulkanLowerer` の data-only seam
で分けられるようにし、CPU / external backend は sibling lowerer として追加する。

本設計の非目標:

- 全領域を継承する巨大な `IRenderPolicy` を作らない
- provider callback に Vulkan command recording や engine module 所有を渡さない
- 将来の WebGPU のために RHI を後付けしない
- MSAA、XR、透明 OIT、TAA jitter を同種の provider として扱わない
- JSON を runtime 内部 ABI にしない
- 論理グラフで全 Vulkan 物理グラフを再現しない
- 論理／物理グラフの一対一対応や損失なし逆変換を要求しない
- Vulkan の共通最小公倍数となる RHI を物理計画の前に置かない

## 1. 新発明ではなく既存設計の統合

既存コードには同じ問題を解いた型がすでにある。レンダリングでもその外枠を
再利用する。

| 既存の型 | 再利用する規則 |
|----------|----------------|
| `FeatureCompose` / `FramePlanner` | 入力データから決定的な結果を作る純粋段階。GPU module を参照しない |
| material semantic route | material は具体的 pass id でなく `deferred_geometry` 等の意味を選ぶ。graph variant が別 pass へ写像できる |
| Physics `ProviderV2` | `struct_size`、版、capability bits、名前、context、noexcept callback、engine 側検証 |
| `RegistrationOwner` / token | engine builtin と game DLL の登録を同じ registry に置き、世代付き owner で unload する |
| editor / asset reload transaction | prepare 後に frame boundary で publish、失敗時 rollback、旧 GPU 資源は in-flight 完了後 retire |
| `IFrameTarget` / `ILogicalFrameTarget` | flat / preview / XR の出力先と論理 frame を backend lifecycle から分離する |
| `XxxDependencies` | `GET_MODULE` を消さず、composition root で取得した依存を純粋処理へ明示注入する |

統一するのは **version / capabilities / ownership / resolution / transaction**
という外枠である。material route、draw sort、MSAA、XR を共通の基底クラスへ
押し込むことではない。

## 2. 正式な段階と語彙

```text
project JSON / preset / feature / settings
                 │
                 ▼
       RenderPipelineRequest          Authoring
                 │
       + RenderEnvironmentCapabilities
       + RenderPolicyRegistry snapshot
                 │
                 ▼
       ResolvedRenderPipeline          Resolve
                 │
                 ▼
       logical compile / validate      Logical compiler
                 │
                 ▼
       CompiledRenderPipeline          Immutable logical root
         └ CompiledLogicalGraph
                 │
       + VulkanTargetFacts / user pins
                 │
                 ▼
       target plan / lower / validate  Vulkan target compiler
                 │
                 ▼
       VulkanPhysicalPlan              Immutable physical plan
                 │
          prepare GPU resources        Backend code generation
                 │
                 ▼
       PreparedRenderPipeline          Rollbackable candidate
                 │ frame boundary publish
                 ▼
       RenderRuntime / VulkanBackend   Execution
                  └── OpenXRBackend owns XR lifecycle
```

この図は CPU / external task を持たない現行 renderer の経路である。将来も通常の render
利用者には同じ一つの compile 操作として見せる。内部では
`CompiledLogicalGraph -> TargetExecutionCompiler -> execution.gpu -> VulkanLowerer` と分け、
CPU を Vulkan compiler の上下へ挿入しない。graphics / GPU compute / transfer は同じ
Vulkan physical plan で resource と synchronization を全体解決する。

論理グラフは物理グラフの完全なモデルではない。一つの logical value が物理 image を
持たない場合、複数 logical pass が一つの rendering scope へ融合される場合、または
logical graph に対応しない user-authored physical plan があってよい。compiler が検証する
のは構造的一致でなく、外から観測できる typed boundary / effect の保存である。

### 2.1 名前の規則

今後の型名は次の意味に揃える。

| 接尾辞 / 接頭辞 | 意味 |
|-----------------|------|
| `...Request` | ユーザーの意図。未解決値や preset 参照を含み得る |
| `...Capabilities` | build / GPU / target / view family が可能なこと。選好を含めない |
| `...Policy` | 入力だけから選択する純粋規則 |
| `...Provider` | registry へ登録・交換できる policy 実装。状態を持つ場合も ABI 規律に従う |
| `...Contract` | 入出力の意味、effect、能力要求。実装や物理表現を所有しない |
| `...Pattern` | 物理表現の候補・既定・選好。具体 Vulkan 値を確定しない |
| `Resolved...` | request と capabilities から選ばれた有効値。必ず選択理由を持つ |
| `Compiled...` | typed logical value と依存へ変換済みの immutable data。JSON を要求しない |
| `...ExecutionPlan` | backend plan と cross-domain bridge を閉じた immutable forest |
| `...PhysicalPlan` | backend object 作成前の backend-specific resource / scope / synchronization plan |
| `Prepared...` | publish 前の GPU / runtime candidate。破棄して rollback できる |
| `...Runtime` | frame ごとに状態を進める engine 機構 |
| `...Backend` | Vulkan / OpenXR / OS 等の外部 API lifecycle |
| `...Resolver` | 名前や id の単純な解決。policy 選択には使わない |
| `...Adapter` | 二つの既存 lifecycle を橋渡しする薄い層 |

`Resolver`、`Policy`、`Provider` を同義語として増やさない。

### 2.2 各層の責務

1. **Authoring / Resolve**
   - `pipeline.preset`、feature、material、少数の settings を読む
   - 入力ファイルを書き換えず、詳細 config へ解決できる
2. **Logical compiler + Policy**
   - preset 展開、material / light contract、route、variant、typed dependency を解決
   - logical value の意味型、port constraint、resource use、read footprint を検証
   - capability 不一致と曖昧な writer を名前入りで拒否
   - Vulkan object、global module、frame 可変状態を持たない
3. **Compiled logical plan**
   - pass / resource / route の typed id、semantic domain、snapshot 要求、
     symbolic sample constraint、draw queue policy、診断を保持
   - 作成後 immutable。dump 時だけ JSON へ serialize する
4. **Vulkan target compiler / Physical plan**
   - target facts と user pin から候補を選び、pass の融合・分割、materialization、
     concrete format / sample、scope、load/store、queue、barrier、aliasing を決める
   - GPU handle を持たない immutable data とし、物理グラフ直書きの検証入口にも使う
   - 最終形では前半の候補 / domain 選択を `TargetExecutionCompiler`、Vulkan 具体化を
     `VulkanLowerer` が所有する。現行実装は互換 facade 内で一体でもよい
5. **Runtime / Backend**
   - GPU resource、descriptor、pipeline、command、in-flight lifetime を所有
   - swapchain / present と OpenXR acquire / wait / release / submit を所有
   - physical plan の意味を推測して補正しない

## 3. ユーザー拡張の段階

### 3.1 Preset — 普通の入口

`engine://render_pipelines/hybrid_v1.json` のような版付き preset と、少数の
settings を指定する。preset は material の semantic route を concrete pass へ
写像し、必要な target / pass / feature を展開する。

material 作者は通常、deferred / forward を指定しない。OpenPBR の base subset、
blend、screen input、custom lighting 等の性質から route policy が決め、
`ResolvedMaterialRoute` に理由を残す。

### 3.2 Eject — 詳細 config を所有する入口

preset 解決結果を project 側へ書き出し、その後は通常の verbose rendering
config として編集する。preset への曖昧な deep merge は増やさない。

eject は層別にする。物理 plan を論理 graph へ損失なく戻すことは保証しない。

必要な tooling:

- `dump-resolved-render-pipeline` — typed plan と選択理由を見る診断
- 実装済みの `physical_target_plan.ejectable_pin_package` — 現在選択された
  backend candidate を logical graph fingerprint 付きで同じ物理層へコピーする
- 実装済みの `physical_target_plan.ejectable_physical_fragment` — 自動planの
  resource/scope/alias記述をlogical/environment fingerprint付きで同じ物理層へコピーする
- 将来の `eject-render-pipeline` — resolved authoring / logical graph / Vulkan physical
  fragment / complete raw plan のいずれかを同じ層の編集形式で project へコピー
- 元 preset の名前・版・content hash を provenance として残す

最初の package schema は `pelican.vulkan_target_plan_pins` version 1 とし、
`graph`、`logical_graph_fingerprint`、`pins.backend_candidate` だけを持つ。
config の `vulkan_plan_pins.flat|preview|xr` へ貼り戻すと同じ candidate を選び、
適用結果は `applied_pin_package` で再観測できる。これは完全な physical plan の
公開形式ではなく、有限候補の選択だけを固定する狭い escape hatch である。

resource/scopeを編集する別schemaは`pelican.vulkan_physical_fragment` version 1である。
`vulkan_physical_fragments.flat|preview|xr`へ貼り戻し、適用結果を
`applied_physical_fragment`で再観測する。v1は自動planを安全側へmaterializeする変更、
scope split、verified alias、および宣言済み候補から実device検証済み
`materialized_image` formatへの変更を受ける。任意Vulkan同期値は受けない。

### 3.3 Policy provider — アルゴリズムを交換する入口

draw sort のように「同じ execution mechanism の中で手法だけを交換」するものは
版付き provider にする。engine builtin と game DLL provider は同じ registry を
使う。provider を登録しなければ、その実装コードも状態も不要である。

### 3.4 Backend / source extension — 実行機構を交換する入口

swapchain、OpenXR session、Vulkan command recording、未知の pass kind 等を変える
場合は user-authored `VulkanPhysicalPlan`、境界契約付き `NativeScope`、backend extension、
または engine source の変更になる。これを通常の logical content pass と同じ安全性・
移植性・自動最適化範囲だとは扱わない。

したがって、**content pass はすべて eject 後に変更可能**だが、present、
canonical output transform、XR frame lifecycle、barrier 実行は普通の content
pass ではない。全 envelope を交換したい利用者には source / backend 境界を示す。

### 3.5 詳細な拡張 ladder

通常の preset から順に、`ResourcePattern` override、`PassImplementation` replacement、
tagged region / subgraph replacement、global `GraphTransform`、renderer-wide
`RenderStrategy`、`PhysicalLowering` / physical plan 直書き、`NativeScope` を独立した
入口にする。region は置換・診断のタグであり、barrier や最適化境界ではない。

logical policy provider、physical lowering、native extension は権限が異なるため、
一つの巨大 provider ABI に統合しない。公開 ABI は各段階の game-DLL fixture が成立して
から個別に凍結する。

## 4. `CompiledRenderPipeline` の契約

`CompiledRenderPipeline` は論理 compiler の immutable root とする。最終形では少なくとも
次を typed data で持つ。

- pipeline identity: preset ref / name / version / content hash
- flat / preview / optional XR の graph variant
- `CompiledLogicalGraph`: pass contract、logical value、typed port、resource use、依存
- semantic material route → pass id / `MaterialPassContract` の写像
- material / light contract snapshot と partition reason
- symbolic sample / extent / view constraints
- opaque / transparent の `ResolvedDrawSortPolicy`
- logical resource ごとの semantic type、read footprint、materialization requirement
- excluded feature と fallback を含む structured diagnostics

concrete format、resource lifetime、Vulkan barrier、load/store、resolve、queue、aliasing は
`VulkanPhysicalPlan` が持つ。現在の `FramePlanBarrier` は dependency の診断表現であり、
将来の Vulkan barrier plan と同一視しない。

WP181 / RPE2 で旧 `FramePlan::composition_metadata` にあった
`projection_jitter`、`material_routing`、`pipeline_preset`、`graph_variant` 等は
初期 `CompiledRenderPipeline` の typed field へ移行した。JSON は authoring input と
dump serialization に限定し、runtime がキー文字列を読んで動作を変える状態を
終了した。

この現行実装は v2 の policy manifest に相当する。`CompiledLogicalGraph` は shadow
compile / dump から追加し、既存 field を一括 rename・削除しない。現行
`PassDefinition` / `RenderTargetDefinition` に混在する logical dependency と `vk::*` は、
互換 adapter を通して logical contract / `VulkanPhysicalPlan` へ段階分離する。

移行中は `ResolvedRenderPipeline` が正規化済み JSON を一時的に保持してよい。
ただしその JSON を新しい runtime 契約として公開せず、typed field への移行表と
削除条件を各 WP に書く。

## 5. Render policy registry の共通規律

### 5.1 Provider envelope

公開 provider は Physics ABI と同じ規律を使う。

- 先頭に `struct_size` と descriptor version
- provider version と minimum engine provider version
- capability bits
- UTF-8 name + 明示長、opaque context、関数 pointer
- `noexcept` C ABI と status return。例外、STL container、Vulkan 型を越境させない
- register は世代付き handle を返し、unregister は owner / generation を検査
- game DLL 登録は `RegistrationOwner` に関連付け、reload 時に一括 retire
- 名前解決で得た lease は callback 完了まで owner DLL を保持し、unregister / owner
  release は in-flight lease の終了を待つ
- owner release は registry 側にも世代付き失効を残し、owner 台帳を無効化する直前の
  遅延登録が callback を再挿入できないようにする
- engine は callback 出力を検証・canonicalize してから使用

registry は owner-aware な小さな engine mechanism として残る。個々の provider
実装は別 translation unit / game DLL / optional build unit に置ける。

### 5.2 Provider callback の禁止事項

以下は draw sort、recipe、logical transform 等の **policy provider** に対する規則である。
`PhysicalLowering` は data-only target facts と physical descriptor を扱えるが GPU handle を
持たず、`NativeScope` / backend extension は別の unsafe 境界とする。

policy provider は次をしてはならない。

- `GET_MODULE`、Vulkan / OpenXR API、GPU resource への直接アクセス
- engine 所有 pointer を callback 後まで保持
- callback 内から provider の register / unregister を再入すること。これらは
  outstanding lease の終了を待つため、同一 callback から呼ぶと自己待機になる
- command buffer を並べ替えたり descriptor を作成すること
- wall clock、unordered iteration、未固定乱数に依存すること
- engine の validation を迂回して pass / resource id を捏造すること

provider の自由は **policy output** に限定し、resource と execution の所有は
engine に残す。

### 5.3 選択と診断

provider 名は config で明示するか preset の既定から解決する。結果には
request、selected provider、version、capability match、fallback / reject reason
を残す。未知名や capability 不足を黙って builtin へ戻さない。

## 6. 最初の実証: draw queue と透明ソート

### 6.1 解消したネック

RPE3 着手前は一つの `render_commands` を material、source material、skinned、view
visibility で直接 sort し、同じ関数内で material ごとの indirect range を作っていた。
WP182 で live inventory と queue materialization を分離し、WP183 で builtin
`state_batched_v1` と game-DLL provider を同じ registry / callback 経路へ移した。
WP184 以前の production は `mixed` phase / `shared` logical view の一つの queue を
`state_batched_v1` で構築していたため、次を同時には満たせなかった。

- opaque は state change を減らす
- transparent は view depth の back-to-front にする
- XR の左右眼で順序を安定させる
- custom policy を game DLL から交換する

WP184 / RPE5 では immutable inventory を維持したまま phase/view ごとの queue を純 CPU
compile し、最後に一つの indirect buffer へ canonical に連結する構成へ移した。単一
comparator の差し替えではなく、phase 選択、provider、logical view、range publication を
それぞれ明示した。

### 6.2 分離する型

1. `DrawItemSnapshot`
   - primitive draw の immutable inventory
   - stable draw identity、route / phase、pipeline / material key、world bounds、
     declaration ordinal、view mask を持つ
2. `DrawSortInput`
   - 対象 phase と論理 view snapshot を加えた data-only input
3. `DrawSortProviderV1`
   - item ごとの primary / secondary key を caller-owned buffer へ書く
4. `CompiledDrawQueue`
   - engine が provider key を検証し、stable identity を最終 tie-break にして
     indirect command / range を materialize した結果
5. `ModelPrimitiveBoundsSource`
   - indexed base AABB、morph target ごとの position delta AABB、weight offset を持つ
     reference-space envelope
6. `CompiledDrawQueueSet`
   - phase/view 別 `CompiledDrawQueue` を view-major、opaque → transparent の順で一つの
     indirect record 列へ連結し、各 range offset を rebased した frame publication

RPE3 / WP182 では `DrawItemSnapshot` と `CompiledDrawQueue`、純 CPU
`DrawQueueBuilder` を実装した。RPE4 / WP183 では data-only `DrawSortInputV1`、
`DrawSortProviderV1`、owner-aware `RenderPolicyRegistry`、provider key 後段の engine
stable tie-break を実装し、builtin も public descriptor と同じ経路で登録した。
snapshot は model-instance generation / scene epoch、mesh / primitive / node、declaration
ordinal、indexed draw 引数、material state key、route / phase、view mask を保持する。

RPE5 / WP184 では importer / procedural upload が bounds source を primitive に永続化し、
frame freeze 時に current world bounds を一時 snapshot へ解決する。authoring の
`draw_sort` は `ResolvedRenderPipeline` から typed `CompiledDrawSorting` へ compile され、
production は phase と logical view を provider へ明示する。bounds source がない primitive、
不正 AABB、非 finite transform / weight、skinned item の palette 欠落は fail-fast する。

provider は item の移動や GPU buffer 作成を行わず、key だけを返す。engine の
stable tie-break により、同値 key でも replay が決定的になる。

### 6.3 Bounds と reference-envelope 規約

RPE5 の world bounds は culling 用の別所有物を live query せず、primitive が保持する
reference-space source と frame freeze 済み deformation state だけから求める。

- indexed primitive は実際に index から参照される POSITION だけで base AABB を作る
- morph は参照頂点の target delta AABB を current weight で base へ加える。負 weight は
  min/max の寄与を反転して処理する
- skin は current palette の各 joint で morph 後 envelope を変換し、その union を取る。
  glTF の非負・正規化 weight に対する保守的 envelope である
- VAT は static POSITION ではなく clip 全体の宣言 `bounds_min` / `bounds_max` を使う
- 最後に instance model matrix で 8 corner を変換するため、非一様／負 scale も扱う

custom vertex displacement を engine が shader から推測してはならない。v1 では、変位後の
頂点が上記 reference envelope 内に留まることを author の契約とする。任意変位には VAT の
ような宣言済み envelope を持つ geometry 経路、または bounds に依存しない custom sort
provider を使う。一般的な material-side bounds override はまだ公開していないため、envelope
外へ動く custom shader に `back_to_front_v1` の正確性を保証しない。

### 6.4 Builtin policy

| 名前 | 状態 | 用途 | 規則 |
|------|------|------|------|
| `state_batched_v1` | 実装済み | opaque 既定 | 現行 material/source/skinned/view grouping を互換維持 |
| `back_to_front_v1` | 実装済み | transparent 既定 | 論理 view から bounds center までの view depth 降順、state は副 key |
| `declaration_order_v1` | 未実装 | デバッグ・厳密順 | authoring / stable draw ordinal |
| `none_v1` | 未実装 | order-independent feature | stable identity のみ。非決定的な無順序にはしない |

opaque と transparent は別 queue を持つ。透明物で batching が分断されても、
正しい depth order を優先する。

`back_to_front_v1` は AABB center の view depth を使う決定的な標準解であり、交差する面、
大きく重なる bounds、自己交差する透明 mesh の厳密な pixel order までは解決しない。
その場合は mesh 分割、custom provider、または OIT feature を選ぶ。

### 6.5 Authoring、XR、publication

未指定時も次と同じ既定を得る。`hybrid_v1` はこの設定を preset 内で明示し、project は
必要な項目だけ同じ top-level `draw_sort` object で置換できる。

```json
{
  "pipeline": { "preset": "engine://render_pipelines/hybrid_v1.json" },
  "draw_sort": {
    "opaque": { "provider": "state_batched_v1" },
    "transparent": { "provider": "back_to_front_v1" },
    "xr_view_policy": "logical_view_center"
  }
}
```

provider 名は engine builtin と active game-DLL provider を同じ registry から解決する。
unknown key、空 provider、未知 XR policy は compile error、未登録 provider 名は frame build
時に名前入りで失敗し、builtin へ暗黙 fallback しない。

XR の既定は左右眼共通の `logical_view_center` で一度 sort する。左右眼で別順序に
すると stereo mismatch が出やすいためである。必要な preset だけ `per_view` を
明示し、左右別 queue と indirect record 複製の追加コストを受け入れる。flat / preview と
XR logical-center は sort view 一組、XR per-view は左右二組を compile し、各 render view は
自分の sort-view index で rebased range を選ぶ。

weighted blended OIT 等は pass / target / composite を増やす render feature であり、
sort provider ではない。将来の OIT feature が `none_v1` 相当を選ぶ場合も、provider 自体は
決定的な順序を返す。

### 6.6 Fullscreen `PassImplementation` v1

RPE11a / WP200 では、graph構造を変えず同じlogical contractを満たす実装差し替えの
最小fixtureとして `PassImplementationProviderV1` を公開した。fullscreen passは
次のようにproviderを選べる。

```json
{
  "name": "custom_composite",
  "type": "fullscreen",
  "implementation": {
    "provider": "game.custom_composite"
  },
  "shader": {
    "vertex": "fullscreen",
    "fragment": "fallback_composite"
  }
}
```

未指定時は `builtin.fullscreen_v1` を選び、authoringされたvertex / fragment shaderを
そのまま返す。builtinも特権経路を持たず、game DLL providerと同じregistry callbackを通る。
明示providerが未登録、version / capabilityが不一致、callback出力が不正な場合は、builtinへ
黙ってfallbackせずpass名とprovider名を含むcompile errorにする。

callbackへ渡す `pelican.render.fullscreen_pass@1` contractは、pass名、typed port pattern、
port relation、read/write、access intent、read footprintの種類と任意radius、fullscreen
interface flagを持つcanonical data-only snapshotである。providerが返せるのは版付き
implementation idとvertex / fragment shader referenceだけである。push constant、
light-data use、port、resource、effect、target plan、Vulkan handleは変更できない。
contract fingerprintとprovider owner / identity / generation / version / capabilityは
`PassImplementationSelection`としてcompiled passに残す。

flat / preview / XRを一括prepareするtransactionは、一つのimmutable registry snapshotを
全variantの解決からpublication CAS完了まで保持する。owner release / DLL unloadはsnapshot
解放を待つ。callback内からregister / unregisterを再入してはならず、入力pointerを保持しては
ならない。出力stringは入力rangeをaliasしてよい。それ以外はprovider unregisterまで有効な
immutable storageに置き、engineはlease解放前にcopyする。

v1はshader pairだけを差し替えるため、target planning完了後かつruntime shader/pipeline
生成前に解決してよい。将来、implementationがapplicabilityやplanning constraintを返す場合は
target compiler前の別版境界として設計し、このcallbackへphysical条件を後付けしない。
material pass、未知pass kind、region/subgraph、global transform、renderer strategyも非対象である。
エンジンが常設する`output_transform`はlogical node kindこそ専用だが、runtime実装は
fullscreen shader pairなので同じcontract経路を通る。通常はproviderを明示できずbuiltin
identityがauthoring shaderを保つため、この扱いはterminal構造や色invariantの改造権を
追加するものではない。

### 6.7 Tagged region / subgraph replacement v1

RPE11b / WP201 は、同じpassの実装差し替えでは足りず、ある区間を複数passへ展開したい場合の
独立した境界である。passは一つ以上のregion tagを持ち、graph側で置換対象と任意providerを
選ぶ。

```json
{
  "name": "main",
  "region_replacements": [
    {
      "region": "region.post",
      "provider": "game.custom_post"
    }
  ],
  "passes": [
    {
      "name": "tone",
      "type": "fullscreen",
      "regions": ["region.post"],
      "input": ["scene_linear"],
      "output": {"color": "display_linear", "depth": null}
    }
  ]
}
```

provider省略時は`builtin.identity_v1`を選び、authoringされたsubgraphを同じcallbackから返す。
engineは元configからtyped logical graphを一度作り、選択regionを横切るinput/output resource、
concrete logical type、direction、materializationから
`pelican.render.tagged_region@1` contractとstable fingerprintを作る。region名、graph名、
node名は選択・診断用でありfingerprintには含めない。

provider出力はpass array JSONである。engineは元configや公開済みlogical graphを直接変更せず、
local candidateへspliceし、全graphをtyped logical graphへ再compileする。resource集合、
logical type、materialization、region境界contractが元と完全一致した候補だけを
`VulkanTargetPlan`へloweringする。失敗したcandidateは捨てるため、部分的に書き換えられた
compiled logical graphは存在しない。

v1は安全に凍結できる最小範囲として、連続したfullscreen pass regionだけを対象にする。
replacementは1〜256個のfullscreen passで、既に宣言されたresourceだけを使える。
canonical anchorと`output_transform`は置換できない。region外の`after` / `before`は
replacement全体へ保守的に張り直し、共通region tagだけを継承する。compute、material、
snapshot、非連続／入れ子region、resource宣言の追加削除は後続版で具体例を得てから扱う。

region tag自体はbarrier、fusion、alias、allocation boundaryではない。置換後の通常graphを
plannerが再解析し、依存と型から最適化を決める。provider選択とsource/replacement node、
contract fingerprint、owner / identity / generation / version / capabilityはlogical graphと
target planのprovenanceへ残す。

subgraph provider snapshotはpass implementation snapshotより後、graph-transform snapshot、
render-strategy snapshotの順に取得し、四つを全variantのpublicationまで保持する。
DLL owner解放もpass→subgraph→graph-transform→render-strategyの順に行い、
shared/exclusive lockの順序逆転を避ける。

### 6.8 Global `GraphTransform` v1

RPE11c / WP202aは、feature compositionとgraph variant rewrite後のnormalized config全体を
順序付きchainで変換する。`GraphTransform`は既存graph-setのtyped boundaryを受け、
internal target/pass/buffer/compute taskを追加できるが、external/history/required resource、
material route、canonical anchor、terminalを変えられない。各candidateは全logical graphへ
再compileされ、検証済みの値だけがtagged subgraphとtarget loweringへ進む。

これはrendererをゼロから選ぶ入口ではなく、既に選ばれたrenderer graphにMSAAや追加buffer、
platform specialization等の構造変換を適用する境界である。

### 6.9 Renderer-wide `RenderStrategy` v1

RPE11d / WP202bはpreset展開後・feature composition前のrenderer seed config全体を生成する。

```json
{
  "render_strategy": {
    "name": "project.custom_renderer",
    "provider": "project.render_strategy",
    "parameters": {}
  }
}
```

provider未指定時は`builtin.authored_config_v1`がselectorを除いたseedをそのまま返す。
strategy出力はfeature composition、canonical color、flat/preview/XR policy、
`GraphTransform`、tagged subgraph、logical compile、Vulkan target loweringを迂回しない。
provider失敗や後段rejectはlocal candidateを捨て、元seedとactive generationを維持する。

V1 callbackはgraph variantと、現在利用可能なcompiler機構を示すtyped renderer facadeを
受ける。startup config compilerが所有していないlive material/light/geometry inventoryは
公開しない。これらが必要なrendererは、実際に消費可能なscene contractを追加する後続ABIで
扱う。詳細な契約とprovenanceは
[`design_render_graph_compiler.md`](design_render_graph_compiler.md) §12を正とする。

## 7. Material route、MSAA、XR はどう分けるか

### 7.1 Material route

material route は既存どおり concrete pass id ではなく semantic class とする。

- `deferred_geometry`
- `forward_opaque`
- `forward_transparent`

これは `hybrid_v1` の互換 route tag であり、renderer core が将来もこの三種類だけを
理解するという意味ではない。別 recipe / strategy は namespaced route tag または独自の
material partition を使える。

route policy は surface/material 能力と明示 override を解決する。graph variant が
route-to-pass map を持つ。custom exact pass は escape hatch として維持するが、
pass contract と phase の不一致は compile error にする。

### 7.2 MSAA は provider ではなく graph transform

MSAA は描画アルゴリズムの callback ではなく、logical port の sample constraint と、
attachment、pipeline sample count、resolve、後段 input を一括変更する target-aware
構造変換である。

```text
SampleCountRequest
  -> logical sample constraint
  -> VulkanTargetFacts と照合した ResolvedSamplePolicy(reason 付き)
  -> MsaaGraphTransform / PhysicalLowering
  -> VulkanPhysicalPlan
```

authoring v1 の最小形は次を想定する。

```json
{
  "pipeline": {
    "preset": "engine://render_pipelines/hybrid_v1.json",
    "settings": {
      "msaa": { "samples": 4, "fallback": "error" }
    }
  }
}
```

- 未指定は 1 sample で現状互換
- `fallback` は `error` を既定とし、明示時だけ `lower_supported` を許す
- `scope` は `geometry` を既定とし、`all` / `none`、および高度な
  `targets` 指定で attachment 連結成分単位に上書きできる
- authoring JSON は project compiler で `SampleCountPolicy` へ変換し、
  core / Vulkan runtime は JSON key を再解釈しない
- compiler が multisample target と single-sample resolve target を生成する
- pass の color/depth attachment sample count は一致させる
- resolve 後に sample する feature は single-sample resource を見る
- depth resolve / sampled depth / XR swapchain の能力差は capabilities で検証する
- 詳細な resolve mode や手書き target は後続版または eject 経路で拡張する

まず request 解決を純粋テストし、その後に現在 `e1` 固定の image / pipeline
作成を typed sample count へ配線する。設定 parser と Vulkan 改修を一つの巨大 WP
にしない。

### 7.3 XR / preview は graph variant policy

OpenXR backend は session、swapchain image、frame wait/acquire/release/submit を所有
し続ける。一方、WP192 では graph の選択を compile-time の
`CompiledGraphVariantPolicy` へ移した。

- feature の include / exclude
- history / projection jitter の許否
- view family と resource dimension の変換
- multiview / sequential の選択
- mirror 用 output の要求

XR / preview 固有 `include_feature` と JSON validation callback は削除し、typed
request / capabilities / feature decision / config validation へ移した。feature の
除外と reject は `GraphVariantFeatureDisposition` と
`GraphVariantFeatureReason` を持ち、文字列 callback の戻り値だけに理由を失わない。

初期 builtin の実行契約は次で固定する。

- flat: view 数は caller-defined。通常 1 view だが、汎用 logical-frame API の
  sequential multi-view 再利用を禁止しない
- preview: exact 1 view、request-local capture、history / jitter 禁止
- XR: exact 2 view、sequential 2D resource、history / jitter 禁止、left-eye mirror

XR のlogical contractは`GraphVariantViewExecution::sequential`のまま保持する。
WP203aではこれをdevice実行方式の別名にせず、後段の`VulkanViewExecutionPlan`へ
`auto` / forced sequential / required multiview、scopeごとのexecution/view mask、
resourceごとのshared/sequential/layered layoutを追加した。deviceとscope implementationの
両方が対応を明示した場合だけmultiviewを選ぶ。

WP203bではbuiltin fullscreen implementationだけがtyped capabilityをadvertiseし、
engine shader variantの最終SPIR-V reflectionを二つ目のgateにする。runtime compilerは
physical input dimensionとview contractをcompiled passへ残し、mixed scope schedulerが
single / sequential / multiviewを一つのlogical frame内で実行する。custom providerや
material passは明示対応をまだ持たないため逐次へ残り、誤ってmultiview pipelineとして
実行されない。

WP203c で OpenXR target は 2-layer color array attachment を提供し、
production view-family capability を有効化した。typed external depth export が
runtime の depth format/extent と一致する場合は optional 2-layer depth swapchain へ
copy して composition depth を chain し、不一致時は color-only へ戻る。
`auto` の性能判断は physical planner の data-only `multiview_auto` device profile で行い、
選択と実測証跡を `view_execution_plan.auto_gate` に残す。profile が無い device は
optimize-by-default、profile がある device は要求 gain を満たす場合だけ multiview とする。

custom providerやmaterial passは引き続き明示 capability を持たないため逐次scopeに残る。
public provider capability bitは、外部 provider で必要なABIと failure mode が具体化してから
追加する。

### 7.4 TAA jitter は無理に provider 化しない

現在の Halton/table jitter は feature data と純粋数値処理で交換できている。
registry、owner、hot unload を必要とする runtime algorithm が現れるまで、その
軽い仕組みを維持する。語尾が `provider` というだけで provider registry へ
移さない。

## 8. Color domain と screen input

hybrid graph では deferred lighting と forward の出力を同じ scene-linear HDR
domain に合成し、tone mapping は一度だけ行う。この不変条件を JSON 名の慣習で
なく typed logical port contract にする。

最初の canonical alias:

- `SceneLinearHdrV1`
- `DisplayLinearV1`
- `DisplayEncodedV1`
- `DeviceDepthV1`
- `LinearViewDepthV1`

これらは flat enum でなく、`color_signal@1` / `depth@1` と scene/display、
linear/encoded、device/linear-distance 等の正規化済み型引数からなる。型引数の一致、
集合包含、range、symbol binding は共通 matcher で判定し、個別 pass の文字列比較を
増やさない。domain の詳細は `design_render_graph_compiler.md` §2 を正とする。

screen snapshot は logical source、capture point、semantic type、extent relation、
materialization requirement を compiled logical plan に持ち、sample count、format、
descriptor visibility は target compiler が physical plan へ解決する。material の
screen input は型に加えて `same_pixel` / `neighborhood` / `arbitrary` の read footprint を
宣言する。WP187 では target compiler 導入前の互換縦切りとして、`forward_opaque` 後の
scene-linear color / device-depth snapshot を `hybrid_v1` に固定し、名前付き pass input と
material descriptor set 1 を接続した。resize / shader reload 時は既存 target rebind 経路で
descriptor を再生成する。

標準 alias は `opaque_color`、`opaque_depth`、`scene_depth`、`linear_view_depth` である。
前者は `neighborhood`、depth 系は `same_pixel`。`linear_view_depth` は
`pelican.render.depth_linearize@1` を通し、投影行列の consumer inventory にも登録する。
未知 alias、source format と型の不一致、shader reflection と宣言 binding の不一致は
描画前に拒否する。任意 alias の registry と tile-local / materialized の選択は RPE6c で扱う。

depth fade の same-pixel read は tile-local 候補、offset を伴う屈折は opaque scene
snapshot の materialization 候補になる。この判断を「transparent pass だから」と
推測せず、material contract と resource use から導く。

SSAO / SSR / decal が forward object に効くかは feature ごとの input/output 契約で
決める。一般的な「forward にも自動で全部効く」とは推測しない。

## 9. Hot reload と publication

pipeline / policy reload は既存 transaction 規律へ揃える。

1. side decode / parse
2. resolve / logical compile / target plan / validate
3. GPU pipeline、descriptor、target、draw queue candidate を prepare
4. frame boundary で logical plan、physical plan、関連 material route を一括 publish
5. 失敗時は candidate のみ rollback
6. 旧 plan / provider generation / GPU resource は in-flight 完了後 retire

route/model/sample count の変更を material 単体で部分 publish しない。現在
fail-fast している route 変更は、この transaction が成立した後に解禁する。

game DLL provider unload 時は新 callback の取得を止め、frame が保持する provider
generation の参照が消えてから context を retire する。unregister と callback 実行を
競合させない。

RPE10a / WP193 で、上記のうち runtime publication root を実装した。
`PreparedRenderPipelineGeneration` は現在の世代を基準に candidate を構築し、
pass/node/barrier/material route binding の検証を publish 前に完了する。
`FrameGraphRuntimeContainer` と `RenderingPassContainer` は同じ publication state を読み、
base generation が一致する CAS 一回で active root を差し替える。renderer は論理フレーム
開始時に `shared_ptr` snapshot を一度だけ取得し、全 view の pass、frame graph、
sample/target plan、draw-sort provider 選択・variant・jitter policy をその snapshot から読む。旧 root は
最後の frame lease が消えるまで生存する。

RPE10b1 / WP194 では、この root に `RenderPipelineGpuArena` を追加した。
render target、frame-graph buffer、compute task、shader bundle、graphics/compute pipeline、
fullscreen/debug/shadow/velocity pass の各 registry は registration checkpoint を公開し、
`RenderPipelineGpuRegistrationArena` が一つの prepare 区間として束ねる。途中の例外、
明示 fault、runtime candidate 構築失敗、stale publication では arena の destructor が
逆依存順に新規登録だけを取り消す。publish 成功後だけ rollback を解除する。
legacy registry の checkpoint-to-publication 区間は直列化し、stale transaction が別
transaction の登録を巻き戻す競合を防ぐ。XR mirror intermediate の登録も同じ prepare
callback 内へ移した。

同時に `render_pipeline/flat`、`render_pipeline/xr` の owner scope と、その区間で増えた
resource kind / handle / name / declared bytes を immutable manifest にして
`RenderPipelineRuntimeGeneration` と同じ CAS で公開する。prepare 中の manifest は observer
から不可視であり、`currentFramePlanJson()` で runtime generation と対応付けて診断できる。
将来 registry が所有権を移せる場合に備え、scope は type-erased resource lease も保持できる。

RPE10b2 / WP195 では、同じ owner scope の再登録を replacement として扱う。prepare 開始時に
旧 scope の current-name facade だけを隠し、旧 registry entry と Vulkan payload は旧
generation lease が保持する。新 scope は同名 resource でも単調増加する別 handle を得る。
`CompiledFrameGraphExecution` は target / buffer の name-to-handle snapshot を所有し、
barrier、copy、fullscreen descriptor、compute task、XR mirror は実行中に mutable な
global name table を引き直さない。candidate publish 後にだけ新 scope lease を arm するため、
prepare failure / stale publication は name table と新規 membership を WP194 の checkpoint
へ戻せる。

runtime program は owner scope を持ち、replacement 時は同じ owner の旧 program 集合を
候補から一度除去する。同名 program は `RenderingPassId` と公開順を維持し、候補から消えた
program は新 generation から除去する。別 owner の program が旧 scope resource を参照する
可能性は、依存の明示化が完了するまで旧 scope lease をその program へ保守的に継承して守る。
最後の参照解放時は pass/descriptor から shader への逆依存順で exact handle membership を
retire し、Vulkan payload は利用可能なら既存 `DeletionQueue` へ渡す。共有
descriptor-set-layout cache は scope resource ではなく engine cache として残す。

RPE10b3 / WP196 では`ReloadService`へbatch participantを追加した。active flat programの
project-backed root / preset / feature参照をwatch sourceとし、同じwatcher frameの変更を
一度のreloadへcoalesceする。previewのCPU precompileとflat / XR全variantのGPU prepareを
完了し、default terminalとruntime module profileをcandidate上で検証してから、一回の
generation CASで公開する。flat / XRは一つのowner scopeを共有するため、片方だけ成功した
中間世代は存在しない。失敗時はactive root、registry membership、config cache、watch
dependency集合を更新しない。

rendererはlogical frame開始時のgeneration snapshotを、実際にsubmitするframe targetへ
渡す。window swapchainはin-flight slotごとのfence、offscreenは同期fence、OpenXRは各eye
submit、desktop mirrorは独立swapchain submitにleaseを保持し、それぞれの完了後だけ解放する。
OpenXRの部分失敗もsubmitted eyeを待ってから解放し、wait自体が失敗した場合はgeneration
teardownまで保持する。config registrationは引き続きengine owner threadの直列化区間で
呼び、mutable registryを任意workerから並行更新するAPIとは扱わない。

module graph凍結後に、初期化されていないruntime moduleを必要とする`ui` / `sprite` /
`gpu_timing`等をconfigだけで初回有効化する操作はpublish前に拒否する。startup時に
初期化済みなら編集・再有効化できる。runtime module profile自体のhot expansionは別課題である。
`RenderPolicyRegistry` の実 provider generation も root の所有物ではなく、queue 構築時の
個別 lease で保護される。

## 10. `GET_MODULE` の位置

`GET_MODULE` は便利なため削除しない。ただし使用位置を次に限定する。

- engine startup / shutdown の composition root
- module 自身の薄い runtime adapter
- legacy 経路から明示依存構造体を組み立てる場所

`resolveRenderPipeline`、graph planning、sort policy、provider callback、validation
では使わない。composition root が `GET_MODULE` で取得した service を
`XxxDependencies` と data snapshot で渡す。

これにより service locator の利便性、純粋 compiler のテスト容易性、game DLL
provider の unload 安全性を同時に保つ。

## 11. Purge の定義

「purgeable」は二段階に分けて検証する。

1. **runtime purge**
   - 未参照 feature / preset branch は pass、target、descriptor、dispatch を作らない
   - sample count 1 の `MsaaGraphTransform` は identity
   - XR 無効時は XR variant / backend resource を作らない
   - 未選択 provider は context を生成せず callback もしない
2. **binary purge**
   - game provider は game DLL を外せば消える
   - OpenXR 等は既存 `PELICAN_WITH_*` build unit で消える
   - 大きな builtin policy は別 target / build unit 化をサイズ計測後に判断する

registry、typed plan、validation の小さな mechanism 自体は renderer core に残る。
「コードが 1 byte も残らない」と「GPU work/resource が 0」を混同しない。

## 12. 段階実装

各段階は挙動変更と構造変更を同じ commit に混ぜない。

| 段階 | 内容 | 主な gate |
|------|------|-----------|
| RPE0 | 本文書と既存文書の統合 | 用語・所有権・順序の合意 |
| RPE1 / WP180（済 2026-07-21） | `RenderPipelineRequest`、`RenderEnvironmentCapabilities`、`ResolvedRenderPipeline` と純粋 resolve 境界を抽出 | flat/preview/XR の既存 plan dump byte-equivalent、GPU mutation なし |
| RPE2 / WP181（済 2026-07-22） | typed `CompiledRenderPipeline` を導入し、`composition_metadata` の runtime 読みを撤去 | JSON は dump のみ、既存 golden 不変 |
| RPE3 / WP182（済 2026-07-22） | inventory と queue materialization を `DrawQueueBuilder` へ分離し、`state_batched_v1` で現行順を再現 | indirect bytes / draw ranges 不変、二回実行一致 |
| RPE4 / WP183（済 2026-07-22） | owner-aware `RenderPolicyRegistry` + `DrawSortProviderV1`、builtin も同じ経路へ | game DLL register/unregister/reload、stale generation reject |
| RPE5 / WP184（済 2026-07-22） | `back_to_front_v1`、phase 別 queue、XR logical-center/per-view | 混在 scene、安定 tie-break、左右眼 fixture |
| RPE6a / WP185（済 2026-07-23） | logical type kernel、parameter matcher、port/use contract、typed shadow graph | CPU-only exact/convertible/deferred/rejected、canonical hash、runtime 不変 |
| RPE6b0 / WP186（済 2026-07-23） | versioned logical value / producer edge、nominal connection、access intent、conversion implementation descriptor | 宣言順非依存、duplicate/missing producer reject、schema v2、runtime 不変 |
| RPE6b1 / WP187（済 2026-07-23） | typed color/depth domain + hybrid screen-input descriptor binding | 屈折/深度 fade fixture、tone map 一回、型不一致 reject |
| RPE6c0 / WP188（済 2026-07-23） | data-only target topology / directed links、pure Vulkan backend probe、immutable registry snapshot、optimize-by-default / advisory diagnostics | deviceなしprobe、link有無のbridge可否、reason付きreject、opt-in strict/hazard stress、追加注釈なしのparallel/fusion候補、runtime不変 |
| RPE6c1 / WP189（済 2026-07-23） | `ResourcePattern` / read footprint / materialization + mock desktop/tile target planner。canonical / disposable lowering seam と dialect legality を追加 | desktop materialize、tile local-read、追加 G-buffer、屈折 snapshot、physical dump、optional domain zero-cost |
| RPE7 / WP190（済 2026-07-23） | `SampleCountPolicy` / capabilities / deterministic resolution の純粋段階 | exact / lower-supported、limiting resource 診断、typed compiled plan |
| RPE8 runtime slice / WP190（済 2026-07-23） | attachment-connected MSAA transform + image/pipeline sample count + color/depth resolve | 1x互換、実4x hybrid headless、depth capability gate、validation errorなし |
| RPE8b / WP191（済 2026-07-23） | WP189 physical target planner と WP190 runtime bridge の single lowering path | JSON/DSU重複削除、physical resource/scope sample contract、materialized runtime consume、追加G-buffer、desktop/tile/headless |
| RPE9 / WP192（済 2026-07-23） | XR / preview callback を builtin `GraphVariantPolicy` へ移行 | sequential XR/preview plan 不変、OpenXR lifecycle 非依存 test |
| RPE10a / WP193（済 2026-07-24） | immutable runtime generation、prepare/rollback、base-generation CAS publish、frame lease | route/sample/draw-sort provider 選択/pass/plan 同時変更の atomic fixture、stale candidate reject、CPU-side retire |
| RPE10b1 / WP194（済 2026-07-24） | append-only GPU registration arena、cross-registry rollback、owner-scope manifest の root 同時公開 | 5段 fault injectionで全registry membership復元、candidate不可視、lease rollback、hybrid診断 |
| RPE10b2 / WP195（済 2026-07-24） | scope-aware replacement、generation-owned registry lease、世代ローカル typed binding | same-name reload、pass/target削除、旧世代frameから旧resource参照、最後のlease解放後のexact retire |
| RPE10b3 / WP196（済 2026-07-24） | submission fence retire、project config / feature / preset watcher、flat/XR一括publication | in-flight完了前の破棄なし、同一frame変更coalesce、失敗時active世代不変、実Vulkan reload |
| RPE11a / WP200（済 2026-07-24） | fullscreen `PassImplementationProviderV1`、typed logical contract、builtin/game DLL同一registry | shader pair限定差し替え、contract不変、owner/generation provenance、failure atomic、reload/rollback/unload |
| RPE11b / WP201（済 2026-07-24） | tagged region、typed boundary contract、builtin/game DLL subgraph replacement | immutable candidate再compile、1→N fullscreen展開、境界不変、provenance、failure atomic、reload/rollback/unload |
| RPE11c / WP202a（済 2026-07-24） | global `GraphTransformProviderV1`、graph-set boundary、順序付き変換chain | full-config candidate再compile、internal target/pass追加、external/history/required境界と常設node保護、provenance、reload/rollback/unload |
| RPE11d / WP202b（済 2026-07-25） | renderer-wide `RenderStrategyProviderV1`、typed renderer facade、preset後の全config生成 | feature/variant/logical/physical再compile、config fingerprint provenance、preview/headless、reload/rollback/unload |
| RPE12a / WP204 Phase A（済 2026-07-26） | graph-scoped target planning control、logical fingerprint付きVulkan backend decision pin/eject | variant別package、round-trip、stale/unknown/infeasible reject、strategy fingerprint非自己参照、headless runtime観測 |
| RPE12b / WP204 Phase B v1（済 2026-07-26） | environment-bound Vulkan physical fragment、pure verifier/linker、runtime route | conservative materialize、split-only scope、alias/lifetime/capability検証、variant別round-trip、OpenXR OFF/ON |
| RPE12b / WP204 alternate-format slice（済 2026-07-26） | authored format candidate、target固有device evidence、runtime format assignment | undeclared/unsupported reject、sample/layer/external-depth再検証、flat/XR競合reject、Vulkan hot reload実描画 |

### 12.1 いま着手する範囲

RPE1 / WP180 から RPE9 / WP192 まで完了した。authoring resolve、immutable typed
pipeline plan、draw inventory / queue materialization、versioned draw-sort provider registry、
world bounds、phase/view 別 queue に加え、Vulkan 非依存の logical type / port-use kernel と
現行 `FrameGraphDefinition` の diagnostic shadow graph が分離済みである。shadow graph は
resource family + version の値と exact producer edge を持ち、宣言順を correctness に使わない。

`hybrid_v1` material-pass の typed screen-input descriptor、opaque color/depth snapshot、
depth linearization、tone-map 一回の fixture は RPE6b1 で固定した。RPE6c0ではtopology /
probe / warning・planning profileをruntime非変更で固定した。RPE6c1では`ResourcePattern`、
resource lifetime、desktop materialization、tile-local read、屈折snapshot、dialect legalityを
data-only physical planとして実証した。G-buffer名や枚数はplannerへ固定せず、endpointの
attachment budget factで上限を検査する。WP190 は authoring の
`pipeline.settings.msaa` を typed `SampleCountPolicy` へcompileし、実デバイスのformat /
depth-resolve能力でattachment連結成分ごとの共通sample数を解決する。Vulkan runtimeは
multisample attachmentとsingle-sample resolved imageを分離し、後段sample/copyはresolved
imageだけを見る。WP191ではこの連結成分解決を`VulkanTargetPlan`へ移し、runtimeは
`FrameGraphDefinition`からlogical shadow graphを経て得たphysical format /
representation / sample-count contractを検証・適用する。現runtime adapterは実装済みの
materialized imageだけをtarget topologyへadvertiseし、tile-local / transient / alias planを
誤って実行しない。WP192ではflat / preview / XRを
`GraphVariantPolicyRequest + GraphVariantPolicyCapabilities` から
`CompiledGraphVariantPolicy`へ純粋compileし、feature decision、history/jitter、
view execution、resource layout、terminal、mirror、pass suffixを一つの値に集約した。
registrationとrendererはこのcompiled policyを読み、OpenXR session stateをcompilerへ
持ち込まない。WP193では`CompiledRenderingPass`、`CompiledFrameGraphExecution`、
`CompiledRenderPipeline`、`VulkanTargetPlan`、material route binding、enabled featureを
`RenderPipelineRuntimeGeneration`へ束ねた。prepare中のcandidateはobserverから不可視で、
publishはbase generation付きCAS一回、frameは同じrootを全viewで保持する。WP194では
GPU registrationのappend-only checkpoint/rollbackとowner-scope manifestを同じrootへ接続した。
WP195では同じowner scopeを置換し、programの維持/削除、target/bufferの世代ローカルhandle、
registry membershipのgeneration lease所有を接続した。WP196ではroot / feature / presetの
FileWatcher batchをpreview + flat (+ XR)の一括transactionへ接続し、一回のCASで公開する。
swapchain slot、OpenXR eye、offscreen、desktop mirrorの実submission fenceがgeneration
leaseを保持するため、旧resourceのexact retireは最後のGPU完了より前に起きない。
WP200ではfullscreen passのshader pair実装を、target plan由来のtyped logical contractを
受けるowner-aware providerへ分離した。builtin identityとgame DLL差し替えは同じABIを使い、
全variant transactionは同じregistry snapshotを保持する。WP201ではpassのregion tagから
typed input/output境界を作り、builtin identityまたはgame DLL providerが返すsubgraphを
local config candidateへspliceして全logical graphを再compileする経路を追加した。
元graphやactive runtimeは破壊せず、resource/type/materialization/境界が一致した候補だけを
target loweringとpublicationへ進める。v1は連続fullscreen regionと既存resourceに限定する。
WP202aではその外側に順序付きglobal `GraphTransform` chainを追加し、external/history/
required boundaryと常設nodeを守ったfull-config candidateだけを再compileして採用する。
WP202bではさらに上流のpreset展開後・feature composition前へrenderer-wide
`RenderStrategy`を追加した。strategyはtyped graph variantと利用可能なcompiler capabilityを
持つrenderer facadeを受け、renderer seed config全体を生成する。出力は通常の
feature/variant/logical/physical compilerを迂回しない。V1はstartup config compilerが
所有しないlive material/light/geometry inventoryを公開せず、後続contractへ残す。
WP204 Phase Aでは下流の`target_planning`をtyped policyとしてcompileし、自動
`VulkanTargetPlan`からversion 1のdecision pinをejectしてvariant別configへ戻す経路を
追加した。pinはlogical graph fingerprintを持ち、hot reload後のstale packageや
device/provider上で成立しないcandidateを黙って別案へ変えない。lower-layer controlは
renderer strategyのseed/output fingerprintから除外し、貼り戻したpinが自分自身の
fingerprintを変える循環を防ぐ。WP204 Phase B v1では同じautomatic planからversion 1の
physical fragmentをejectし、conservative representation変更、scope split、alias groupを
pure verifier/linkerで再検証してruntime target compilerへ戻す経路を追加した。
environment fingerprintはdevice factsとprovider generationを含み、logical graphが同じでも
実行前提が変わったartifactをstaleとして拒否する。現runtime未対応のtile-local/aliasは
明示capability gateを維持する。alternate-format sliceでは、render targetの
`format_candidates`をlogical `ResourcePattern`へ運び、対象resource/formatごとの
image usage、sample count、array layer、external-depth transfer capabilityを実deviceから
snapshotしてlinkする。選択formatはsample planとGPU target/pipeline登録へ同時に適用し、
同名targetを共有するgraph/variantの競合はpublish前にrejectする。
各段階の詳細gateは
`design_render_graph_compiler.md` §12 を正とする。

### 12.2 後回しにするもの

- OIT の方式選定
- `PassInfo` の未知 custom pass kind ABI
- public `GraphVariantProvider` の ABI 凍結
- load-store / scope-fusion / queue-barrier physical fragment
- complete raw physical plan の公開形式
- `NativeScope` の game-DLL ABI
- bindless / GPU-driven sort
- forward object へ SSR / SSAO / decal を適用する個別方式
- RHI / WebGPU backend 共通化
- 汎用 CPU task scheduler / execution linker(計測と具体的二候補 task が先)
- 動画 encode/decode dialect / backend(codec、session、backpressure は需要時に設計)

backend candidateだけを固定するWP204 Phase Aのpinと、自動planへ安全な部分編集を戻す
Phase B v1 fragment、宣言済み候補からdevice検証済みmaterialized-image formatを選ぶ
runtime sliceは実装済みである。それより強いload-store / scope-fusion / queue-barrier、
complete raw plan、`NativeScope`は、現在のverifierを
具体的な利用要求と対象GPU fixtureで拡張してから公開形式・ABIを凍結する。
Vulkan physical plan と `NativeScope` という入口自体は本設計で予約済みであり、
論理型へ押し込んで代替しない。

## 13. 横断受け入れ条件

すべての RPE WP は個別条件に加えて次を満たす。

- explicit dependencies の純粋 test は Vulkan device / module 初期化なしで動く
- 同じ request / target facts / pins / provider generation から byte-equivalent な logical /
  physical plan / sort key を得る
- fallback、exclude、自動 route のすべてに machine-readable reason がある
- builtin と project/game 実装に特権差がない
- unknown name、version、capability、stale owner を名前入りで reject する
- target選択はdata-only topology / backend probeを使い、linkerやlowererが未知fallbackを
  発明しない
- logical connection は nominal type 一致または登録済み conversion を要求し、trait 一致だけで
  接続しない
- logical effectは作者の任意宣言とし、portから導出できるdata useを二重記述させない。
  effect未記述だけをerror / warningにしない
- 宣言contractを完全なものとして信頼し、依存がなければparallel / fusion / alias候補にする。
  `serial` / `isolate` / `no_alias`と保守debug profileは明示時だけ適用する
- manual / nativeの同期・portability・性能診断は既定advisoryとし、個別warning idのstrict化は
  project / CI opt-inにする
- region tag を barrier / allocation / optimization boundary として扱わない
- open fragment は typed import / export を要求し、publish plan は connected でなく closed
  であることを検証する
- physical-only graph は typed boundary を検証するが、logical graph への逆変換を要求しない
- flat 1x の既存 golden と frame-plan dump を意図なく変更しない
- optional build OFF と clean-clone gate を維持する
- hot reload candidate の失敗で live plan を部分変更しない
