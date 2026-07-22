# レンダーパイプライン拡張境界とポリシー統合(v2)

対象読者: レンダラ実装者、独自描画方式を組み込むゲーム実装者。

ステータス: v2 実装方針(2026-07-23。v1: 2026-07-21)。`hybrid_v1` の
deferred/forward 基盤を出発点とする。v2 では renderer 構築を論理／ターゲットの
二段階コンパイラとして定義し、論理型、Vulkan 物理計画、物理グラフ直書き、
`NativeScope` の境界を追加した。詳細は
[`design_render_graph_compiler.md`](design_render_graph_compiler.md) を正とする。
本文の層・語彙・所有規則は以後の実装判断の正とするが、
公開 provider ABI は個別の実装 fixture が揃ったものから凍結する。
`DrawSortProviderV1` の v1 バイナリレイアウトは WP183 の public game-DLL fixture を
もって凍結済み(2026-07-22)である。WP184 は `DrawSortInputV1` の既存 prefix を変えず、
`struct_size` で検出する logical-view snapshot を末尾追加した。旧 provider が読む prefix
の byte extent は public header の static assertion で固定している。

関連文書:

- [`design_render_graph_compiler.md`](design_render_graph_compiler.md) —
  二段階 compiler、論理型、target planning、物理 IR / NativeScope
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
二段階とし、物理層を直接所有する利用者は logical compile を迂回できる。

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
| `...PhysicalPlan` | GPU object 作成前の backend-specific resource / scope / synchronization plan |
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
- 将来の `eject-render-pipeline` — resolved authoring / logical graph / Vulkan physical
  plan のいずれかを同じ層の編集形式で project へコピー
- 元 preset の名前・版・content hash を provenance として残す

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
し続ける。一方、次は compile-time の `GraphVariantPolicy` へ移す。

- feature の include / exclude
- history / projection jitter の許否
- view family と resource dimension の変換
- multiview / sequential の選択
- mirror 用 output の要求

現行の XR / preview 固有 `include_feature` と JSON validation callback は、typed
request / capabilities / decision へ移す。初期は builtin policy として挙動を完全
維持し、public provider 化は typed plan の検証規則が固まってから行う。

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
宣言する。現状の snapshot graph 機構はあるが、
`hybrid_v1` の material-pass descriptor binding は未接続で fail-fast する。これは
transparent sort の次に閉じる hybrid の機能穴とする。

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
| RPE6b | typed color/depth domain + hybrid screen-input descriptor binding | 屈折/深度 fade golden、tone map 一回、型不一致 reject |
| RPE6c | `ResourcePattern` / read footprint / materialization + mock desktop/tile target planner | desktop materialize、tile local-read 候補、屈折 snapshot、physical dump |
| RPE7 | `SampleCountRequest` / capabilities / resolution の純粋段階 | unsupported/fallback 診断 fixture |
| RPE8 | MSAA graph transform + image/pipeline sample count + resolve | 1x byte 不変、2x/4x headless Vulkan、depth capability gate |
| RPE9 | XR / preview callback を builtin `GraphVariantPolicy` へ移行 | sequential XR/preview plan 不変、OpenXR lifecycle 非依存 test |
| RPE10 | pipeline hot reload の prepare/publish/rollback/retire | route/sample/provider 同時変更の atomic fixture、in-flight retire |

### 12.1 いま着手する範囲

RPE1 / WP180 から RPE6a / WP185 まで完了した。authoring resolve、immutable typed
pipeline plan、draw inventory / queue materialization、versioned draw-sort provider registry、
world bounds、phase/view 別 queue に加え、Vulkan 非依存の logical type / port-use kernel と
現行 `FrameGraphDefinition` の diagnostic shadow graph が分離済みである。

次は RPE6b で `hybrid_v1` material-pass の screen-input descriptor binding を閉じ、
屈折／depth fade と tone-map 一回の invariant を fixture で固定する。RPE6c は mock
desktop/tile facts で target planning を実証する。MSAA image / pipeline / resolve の
Vulkan 変更は RPE7 / RPE8 まで混ぜない。各段階の詳細 gate は
`design_render_graph_compiler.md` §12 を正とする。

### 12.2 後回しにするもの

- OIT の方式選定
- `PassInfo` の未知 custom pass kind ABI
- public `GraphVariantProvider` の ABI 凍結
- physical plan eject / direct authoring の公開形式
- `NativeScope` の game-DLL ABI
- bindless / GPU-driven sort
- forward object へ SSR / SSAO / decal を適用する個別方式
- RHI / WebGPU backend 共通化

これらは RPE6a〜RPE6c の typed graph / physical plan fixture と具体的な利用要求を得てから
公開形式・ABI を凍結する。Vulkan physical plan と `NativeScope` という入口自体は本設計で
予約済みであり、論理型へ押し込んで代替しない。

## 13. 横断受け入れ条件

すべての RPE WP は個別条件に加えて次を満たす。

- explicit dependencies の純粋 test は Vulkan device / module 初期化なしで動く
- 同じ request / target facts / pins / provider generation から byte-equivalent な logical /
  physical plan / sort key を得る
- fallback、exclude、自動 route のすべてに machine-readable reason がある
- builtin と project/game 実装に特権差がない
- unknown name、version、capability、stale owner を名前入りで reject する
- logical connection は nominal type 一致または登録済み conversion を要求し、trait 一致だけで
  接続しない
- region tag を barrier / allocation / optimization boundary として扱わない
- physical-only graph は typed boundary を検証するが、logical graph への逆変換を要求しない
- flat 1x の既存 golden と frame-plan dump を意図なく変更しない
- optional build OFF と clean-clone gate を維持する
- hot reload candidate の失敗で live plan を部分変更しない
