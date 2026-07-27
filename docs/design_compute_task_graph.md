# フレームグラフ: 宣言的依存とスケジュール最適化(レンダー・コンピュート統一)

対象読者: エンジン担当。
ステータス: v2.5 ドラフト(2026-07-28。v2: 2026-07-04。
v1: compute タスクグラフとして起草。
v2: ユーザー方針によりレンダーパスにも同モデルを拡張 — 統一フレームグラフ化、
手詰め層の設計、既存 config の意味論保存移行を追加。
v2.1: `design_render_graph_compiler.md` の logical / physical 分離へ接続。
v2.2: `design_heterogeneous_execution_graph.md` の typed dialect / domain 分割へ接続し、
authoring `kind` と selected execution endpoint を分離。
v2.3: WP210a の typed indirect compute dispatch と自動 dependency/barrier を反映。
 v2.4: WP210b の fixed-state GPU-written indexed draw/count consumerを反映。
 v2.5: WP210f の per-view compute scheduling、view ABI、XR GPU cullingを反映)。
前提: [SF](実装済み)、`design_render_feature_modules.md`(v1.2 ドラフト)、
ロードマップ §3 の compute パス予約枠。GPU 計測(WP29 候補)と強く連携。

本書の `FrameGraphDefinition` / `FramePlan` は、現行 config から依存と安定順を導く
logical scheduling の実装である。concrete Vulkan resource、queue、barrier、scope の
最終決定は [`design_render_graph_compiler.md`](design_render_graph_compiler.md) の
`VulkanPhysicalPlan` へ段階移行する。`FramePlanBarrier` は依存診断であり、将来の
Vulkan barrier 記述そのものではない。

CPU、Vulkan、external backend を横方向に分ける最終構造、共通 typed IR kernel、open
fragment / closed forest の規則は
[`design_heterogeneous_execution_graph.md`](design_heterogeneous_execution_graph.md) を正と
する。本書の `render` / `compute` は現行 authoring node category であり、実行先を表す
閉じた domain enum ではない。

## 0. 要求(2026-07-04 ユーザー方針・v2 で拡張)

1. コンピュートタスクはアセットとして書け、**順序・バリア・スケジュールは
   ユーザーが手書きせず、依存宣言からエンジンが最適化する**
2. **レンダーパスも同じモデルにする**(v2): ユーザーは依存の一部を記し、
   プログラムが最適化する
3. **機械最適化の後に手で詰める余地**を残し、やりやすくする
4. **レンダーとコンピュートの操作感を揃える**

## 1. 三層モデル(操作感統一の中核)

両ノード種(render pass / compute task)に共通の三層:

| 層 | 役割 | 手段(両者共通) |
|----|------|----------------|
| 正しさ | 順序の制約を宣言する | データ依存(reads/writes)+ 明示エッジ(`after` / `before`) |
| 機械最適化 | 制約内でエンジンが計画する | logical: トポロジカルソート・層別、physical: barrier・scope 融合・queue 分割 |
| 手詰め | 機械の計画を人が微調整する | ①宣言順 = **安定タイブレーク** ②明示エッジの追加 ③統一プランダンプで結果を確認 |

手詰め層の設計判断(v2 の核心):

1. **宣言順は「正しさ」ではなく「タイブレーク」**。依存が許す複数の妥当な順序の
   中から、エンジンは宣言順を優先して決定的に選ぶ。つまり配列を並べ替えることが
   「意味論を壊さずに計画を動かす」第一の手詰め手段になる(依存に反する
   並べ替えは単に効果がない — 事故にならない)
2. 強い手詰めは **`after` / `before` の明示エッジ**(データ依存で表現できない
   意図 — 性能都合の順序、外部副作用の順序 — を宣言として残す。コメントではなく
   形式の一部なので、機械最適化が将来賢くなっても契約として保護される)
3. **統一プランダンプ**: 導出された実行計画(レベル・順序・バリア・
   将来はキュー割当)を人間可読 + 機械可読(JSON)で出力する。
   rpc メソッド `get_frame_plan`(コマンド層に追加)と起動フラグ `--dump-frame-plan`。
   「機械が何をしたか見えないものは手で詰められない」への回答
4. **プラン比較テスト**: 計画そのものを fixture 化して回帰保護
   (バリア数・レベル数の意図しない悪化を CI が検知する。golden の実行計画版)

## 2. 依存の宣言(ノード種ごとの読み書き導出)

### compute task(新規・v1 のまま)

```json
{
  "name": "particle_sim",
  "shader": "shaders/particle_sim",
  "reads":  ["particle_state"],
  "writes": ["particle_state"],
  "dispatch": {"groups_from": "particle_count", "local_size": 64},
  "schedule": "per_frame"
}
```

GPU-produced dispatch は次の別形を取る。

```json
{
  "buffers": [{
    "name": "dispatch_arguments",
    "size": 12,
    "command_layout": "compute_dispatch"
  }],
  "compute_tasks": [{
    "name": "consume_dispatch",
    "shader": "shaders/consume_dispatch",
    "dispatch": {
      "indirect": {"buffer": "dispatch_arguments"}
    }
  }]
}
```

`command_layout` は Vulkan のusage bitをauthoringへ露出する欄ではなく、command bufferの
論理element contractである。physical lowererはこの型からindirect usageを付与する。
`dispatch.indirect`はshader portではないが実行系のresource readなので、plannerがread edgeを
自動導出する。producer shader writeからcommand processor readへのbarrierも同じedgeから
導出し、authorはstage/access maskを書かない。

### render pass(既存形式 — 追加キーなしで依存を導出できる)

既存の宣言が既に依存を含んでいる。導出規則:

- `input`(サンプル入力)→ **reads**
- `output.color` / `output.depth` → **writes**。ただし
  **`color_load_op` / depth load が `load` のものは reads + writes**
  (前内容の読み取り継承 — ping-pong や加算合成のチェーンはこれで繋がる)
- `after` / `before` は render pass にも書ける(新規・任意)
- material passの`gpu_draw_source.commands` / `count` → **reads**。typed
  `indexed_draw` / `draw_count` bufferとして物理化し、compute writeから
  `DrawIndirect` / `IndirectCommandRead`へ同期する

既存 config は無変更で新モデルに乗る。**配列順の意味は「正」から
「タイブレーク」に変わるが、既存の妥当な config では導出順 = 配列順になる**
(§5 の移行検証で機械的に保証する)。

WP210bではGPU draw consumerを1つの`material_range` entryへ固定する。これは
pipeline/material/vertex-layoutをCPU側で束ねたまま、geometry/instance commandとcountだけを
GPUへ移す最小の縦切りである。`scene_draw_commands_v1`はCPU DrawQueueをpacked
`VkDrawIndexedIndirectCommand`列としてcomputeへ渡すが、material segment metadataはまだ
公開しない。複数状態を扱う一般形は、segment tableまたはGPU-visible state keyを別のtyped
contractとして追加してから行う。

WP210cではcompanion host source `scene_draw_bounds_v1`を追加した。1要素は
`vec4 minimum` + `vec4 maximum`の32 byteで、`minimum.w`がvalid flagである。
command列とbounds列は同じframeの同じflattened DrawQueue順序・同じsource record数を持つ。
custom geometryに安全なboundsがない場合はvalid flag 0とし、culling shaderはそのcommandを
残す。buffer容量は個別に上限となるため、consumerは両bufferの短い方を候補数にする。

sampled image portには通常の`pelican_sample_<port>` / `pelican_size_<port>`に加え、
`pelican_sample_lod_<port>`、`pelican_size_lod_<port>`、
`pelican_mip_count_<port>`を生成する。これによりfull mip subresource viewを受け取る
project-owned shaderが、raw sampler bindingへ戻らずdepth pyramidのLODを選べる。
engineはAABB形式とtransportを所有するが、projection、mip選択、depth bias、
conservative keepなどのculling policyはproject shader側に残す。

WP210dでは`scene_draw_segments_v1`を追加し、複数のCPU-bound描画stateを同じ
GPU culling taskで処理できるようにした。1要素は32 byteで、source command範囲、
重ならないoutput command範囲、count slot、sort view、phase、visibility view、
material filter slotを持つ。material/pipeline handle自体はGPU ABIへ入れない。

material passで`gpu_draw_source.layout: "draw_queue_segments_v1"`とsegment host bufferを
指定すると、rendererは選択された各DrawQueue rangeについて1回ずつ
`drawIndexedIndirectCount`を発行する。pipeline、material descriptor、static/skinned
vertex layoutは各rangeを処理する直前にCPUがbindする。segment host publicationや
commands/count bufferの容量が選択rangeを収容できない場合は、一部だけGPU実行せず
pass全体をCPU DrawQueueへfallbackする。

view/filter別rangeはsource commandを共有し得るため、segmentごとのoutput範囲とcount slotは
必ず分離する。このためoutput command容量はsource command数より大きくなり得る。
project compute shaderはsegment bufferのzero-filled tailを`command_capacity == 0`として
無視し、runtime array lengthでcommands、bounds、output、countの各境界を検証する。

WP210eではこの構成を既存hot reload transactionへ載せた。rendering config変更は
commands/count/segments、compute task、pass、pipelineを候補GPU arenaへ全て構築してから、
1つの`RendererRuntimeGeneration`としてpublishする。segment strideやbuffer容量の検証に
失敗した候補はpublic name bindingを一切変えず、旧generationを継続する。

shaderソースだけの変更は別のshader/pipeline transactionで処理する。成功時もgraph
generationとbuffer IDは維持し、shader bundle versionと依存pipelineだけを一括更新する。
compile失敗時はbundle versionを進めない。この二分により、graph ABI変更では完全な資源
差し替えを行い、algorithm実装だけの変更ではgraph再compileを避ける。

WP210fでは`compute_tasks[].schedule`をtyped enumにし、既定`per_frame`に加えて
`per_view`を追加した。`per_view` taskはlogical viewごとに1回実行し、他のrendering
scopeがmultiviewでもcompute invocation自体はsequentialのまま混在できる。
FrameUBOの`pelican_view_index()` / `pelican_view_count()`が現在のlogical viewを公開する。

`per_view` image portの物理形は1つに固定しない。flat/sharedまたは1枚を眼ごとに再利用する
`sequential_2d`はscalar 2D accessor、複数layerを持つ`sequential_2d`は眼別descriptor、
`layered_2d_array`はindexed array accessorへlowerする。したがってproject shaderは
Vulkan image-viewの選択をハードコードせず、同じlogical portをflat、sequential XR、
mixed multiviewで利用できる。

現plannerのRAW導出は宣言順上の直前writerを使う。render pass nodeがcompute task nodeより
先に組み立てられる現行adapterでは、draw command producerはconsumer passへの`before`
（または逆向きの`after`）を明示する。read edgeとbarrier自体は
`gpu_draw_source`から自動導出される。

### 共通規則

1. リソース名前空間は共有(render_targets + `buffers`)。compute の出力を
   パスが `input` で読める・パス出力を compute が読める(配置は依存から自動)
2. **順序が導出不能な writes-writes は hard error**(読みで繋がっていない
   2 ノードが同じリソースを書く構成。曖昧さを形式レベルで禁止し、
   スケジューラの自由度を契約で守る)。解消手段は明示エッジ or 中間リソース
3. shader は stem 規約(compute は `.comp` / web は `cs_main`)。
   feature fragment(`design_render_feature_modules.md`)は compute_tasks も
   passes も追加でき、挿入アンカーは**明示エッジの糖衣**として再定義する
   (`insert: "before:present"` ≡ `before: ["present"]`)

現行 `reads` / `writes` は data dependency の互換 authoring であり、一般的な effect を
すべて列挙する欄ではない。typed port 移行後は data use をportから導出する。history、external
write等のsemantic effectはlogical作者が必要な場合だけ宣言し、未記述はeffectなしという
作者の主張として受理する。engineはcustom shader / callbackの隠れたeffectを推測・証明しない。

RPE6b0 の diagnostic adapter は現行名ベースの `reads` / `writes` を resource family + version
の logical value へ写し、一意 producer と exact consumer から data edge を導出する。同じ
resource の version 番号が大きいだけでは順序を作らない。この adapter はまだ
`pelican.frame_plan` の schedule / runtime / barrier 所有権を変更せず、未明示の初期値は
移行専用 `legacy_implicit` import として可視化する。

## 3. スケジューラ

### v1 — 単一キュー内の自動化

1. DAG 構築(§2 の導出 + 明示エッジ)→ トポロジカルソート(宣言順タイブレーク)→
   互いに依存しないノードの層別
2. バリア自動挿入 + 融合(層境界で必要な遷移を計算し 1 回に束ねる。
   手書きバリアは存在しない — 正しさの根拠を一元化)
3. render pass のアタッチメント遷移は既存の RenderTargetLayoutTracker の
   管轄と統合する(二重管理にしない)
4. GPU 計測(WP29)とノード単位で統合 — 最適化は測れないと評価できない

### v2 — 予告

- **非同期 compute キュー**: 宣言順に意味がない契約がここで効く
  (エンジンがキュー分割を自由にできる)。pickQueues 拡張 +
  タイムラインセマフォ。導入判断は GPU 計測で graphics 飽和が見えてから
- transient リソースのメモリエイリアシング、フレーム跨ぎ(`latency: 1`)、
  `on_demand` スケジュール(ベイク等の単発実行、fence 追跡)

graphics、GPU compute、transfer は同じ Vulkan physical resource graph へ lower し、queue、
barrier、alias を全体で解く。CPU 実装候補を持つ logical compute が将来追加されても、GPU
compute scheduler の下へ CPU scheduler を置かない。target execution planner が domain を
選び、CPU / Vulkan sibling lowerer へ分ける。

## 4. パージ可能性・web との関係

- `buffers` / `compute_tasks` / 明示エッジを書かなければ従来と完全に同じ
  (素通り原則)
- web: レンダー側の依存導出は WebGPU 互換層にも同じ純ロジックを移植する
  (現行 web validation の「input は先行パスの出力」規則は「循環がないこと」に
  緩和される)。compute_tasks・未知キーを含む config は対応まで **hard error**
  (features と同じ意味論)。[PFW] は実装時に版を上げて追記

## 5. 移行計画(意味論保存を機械的に証明する)

| 段階 | 内容 | 依存 |
|------|------|------|
| F0 | **シャドー検証**: プランナ(純ロジック)を実装し、既存の全 config(example・golden 全ケース・shader lab)で「導出順 = 現行配列順」を assert するテストを追加。**実行系には触れない** | 18 |
| F1 | compute 実行系(バッファ確保・compute パイプライン・バリア発行・パスグラフ接続)。golden で「compute なし構成の挙動不変」 | F0, 13 |
| F2 | render pass の実行順をプランナ由来に切替(F0 により既存 config では同一計画 = 挙動不変が構成的に保証されている)。プランダンプ + `get_frame_plan` + プラン比較テスト | F0 |
| F3 | 実証: GPU パーティクル最小(sim + 描画接続)。WP22(pointcache)の再生側と合流可 | F1, F2, WP28 |

F0 が要: 「新モデルは既存の意味を変えない」を主張ではなく**テストで**示してから
実行系を動かす。プランナが純ロジックなので、この検証は GPU 不要で回る。

## 6. CPU タスクへの拡張(予約 — 2026-07-04 検討)

CPU タスク(アニメサンプリング・物理・ストリーミング等)の最適化層も、共通 typed IR /
effect / dependency model 上で将来統合する。ただし CPU scheduler 自体は Vulkan scheduler
とは別 backend とし、導入は測定条件付き:

1. **今は作らない**。負荷の実証がなく、CPU 並列はデータ競合という
   より重い正しさ負担を持ち込む。判断材料は WP29 の計測
   (GPU に加えて **CPU フレーム内訳の計測も WP29 に含める**)
2. 導入順: ①計測 → ②アセットストリーミング専用スレッド(汎用グラフ不要、
   ローダースレッド + 完了キュー)→ ③粗粒度 CPU タスクグラフ
   (メインスレッド飽和が測定で見えたら)
3. **決定性を破らない**ことを設計制約の第一に置く(同じ入力 → 同じ出力。
   ロジックの決定的順序を保証し、並列化は副作用のないステージ内に限る)
4. 粒度は ECS 境界の外側(粗いタスク)から。ECS システム内部の並列化は
   ECS 側の領分
5. **今やる唯一のこと**: F0 のプランダンプ JSON スキーマにある
   `"kind": "render" | "compute"` を authoring category として固定し、execution domain
   には流用しない。将来の schema v2 では `semantic_dialect`、
   `selected_implementation`、`selected_endpoint`、`required_capabilities`、`bridge_ids` を
   別 field として追加する。CPU 対応を `kind: "cpu"` の一値追加だけで実装しない

CPU implementation はdata / effect依存がなければ既定でparallel / reentrant候補にする。
`serial` / `non_reentrant` / `main_thread` / `exclusive` / `blocking`は必要な実装だけが制約として
宣言する。未宣言global stateは作者側のcontract違反であり、その可能性だけを理由にengineが
全taskを直列化しない。保守的な直列実行はdebug / CIの診断profileとして任意に選べる。

## 7. 未決事項

1. `dispatch.groups_from` の名前付きパラメータ注入の正確な API
   (コマンド層 stage 3 / ゲームロジック設計と同時に確定)
2. compute 結果の CPU readback(rpc `capture` 類似)— 需要が出てから
3. プランダンプの JSON スキーマ(プラン比較テストの fixture 形式)— **F0 v1 は解決済み**:
   F0 では `pelican.frame_plan` v1 とし、`schema` / `version` / `graph` /
   `nodes` / `levels` / `barriers` を持つ。`nodes[*].kind` は
   `"render" | "compute"` の authoring category。異種 execution の選択結果は schema v2 の
   独立 field とし、v1 へ詰め込まない。
4. 非同期キュー導入時期(v2)

### 7-4. プランダンプ JSON スキーマ(F0 確定)

F0 のプラン比較 fixture は次の形を正とする。

```json
{
  "schema": "pelican.frame_plan",
  "version": 1,
  "graph": "main_render",
  "nodes": [
    {
      "name": "gbuffer_pass",
      "kind": "render",
      "declaration_index": 0,
      "order": 0,
      "level": 0,
      "reads": [],
      "writes": ["gbuffer_albedo"]
    }
  ],
  "levels": [["gbuffer_pass"]],
  "barriers": [
    {
      "kind": "read_after_write",
      "resource": "gbuffer_albedo",
      "from": "gbuffer_pass",
      "to": "lighting_pass"
    }
  ]
}
```

規則:

1. `nodes` は導出実行順。`declaration_index` は元 config 内の宣言順で、
   トポロジカルソートの安定タイブレークに使う。
2. `levels` は依存のないノードを同じ層にまとめたもの。各層内の並びも
   導出実行順に従う。
3. `barriers` は F0 では計画上の依存可視化に留める。実バリア発行は F1 以降で
   既存 `RenderTargetLayoutTracker` と統合し、二重管理にしない。
4. `kind` は `"render" | "compute"` を v1 の有効値とする。これは authoring category
   であり、host / device endpoint や graphics / compute queue assignment を表さない。
   CPU / external task 統合時は schema version を上げ、namespaced semantic dialect と
   selected endpoint を別 field で表現する。
