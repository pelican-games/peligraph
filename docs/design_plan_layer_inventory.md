# plan の層の棚卸し: 実装待ちの器と、概念の拡張で増えただけの層を分ける

対象読者: エンジン担当(描画コンパイラ / renderer)。
ステータス: 改善案メモ(2026-10-16。議論の記録。WP 化・レビュー前)。2-1 と 2-2 は RC-2 で済み(2026-10-23、`e15e31bb5`。
下の数字と行番号は削除前のもの。fixture `example_frame_plan.json` は RC-2 で 3 種の鍵が抜けて 168,354 → 139,348 byte)。
前提: [design_render_stage_as_canvas_filter.md](design_render_stage_as_canvas_filter.md)(アンカー)、
[design_render_target_memory_reduction.md](design_render_target_memory_reduction.md)(RT の計測)、
[design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md)(`ExecutionPlan` の目標語彙、2026-07-23)、
マニュアル [第6章 §6.6 プランダンプ](manual/06_rendering.md)、[v1 開発計画](v1_development_plan.md)「保守上の整理候補」。
数字は `test/fixtures/devstudio/example_frame_plan.json`(example の実機の `get_frame_plan`、約 190 KB)。
行番号は `develop`(v0.1.0 + WR-1)時点。

## 0. 要約

- アンカーと同じ性質のもの(仕組みはあるが今は意味を持たない)を plan と runtime から拾い、**2 種類に分けた**。
  - **(1) 実装待ちの器**: 予定しているエンジン機能(第 2 backend = Metal、複数 queue、tile GPU、外部 provider、計測)が入れば
    役目が出るもの。残す。ただし plan ダンプと Studio には既定で出さない。
  - **(2) 概念の拡張で増えたが意味をなしていないもの**: 消費者がいない、または元の情報からいつでも再生成できるもの。**掃除する**。
- **決定(2026-10-17)**: 第 2 backend(Metal)は自前で書かず、**mac は MoltenVK で動かす**(実証計画は
  [design_macos_moltenvk_proof.md](design_macos_moltenvk_proof.md))。これに伴い `execution_plan` は (2) に戻り、**削除する**。
  掃除で消すのは `execution_plan` と `levels`。物理計画の固定ルート(Pin / Freeze)は **`target_planning` の policy に一本化**し、写し(pin package / fragment)を消す(2-3、方針決定)。
  `declared_bytes` は埋める。段階 2(`CompiledRenderPipeline`)は消すのではなく **typed な論理 graph として育てる**(Metal は理由から外れるが、純ロジック test と incremental の理由は残る)。
  transient / tile_local と scope 合流(3-2 / 3-3)は Apple GPU(tile ベース、memoryless)で効くので、**mac 実証で役目が出る器**に格上げ。
- 多くは `get_frame_plan` の鍵に触るので **v1**。先に「plan の各節には消費者があるか、無ければ診断と明示するか消す」を規則にする。

## 1. 判定基準

| 問い | (1) 器 | (2) 拡張の残り |
|---|---|---|
| 今、実行か判断に使う消費者がいるか | いない(将来の機能が使う) | いない |
| 消しても失う情報があるか | ある(入力・設定・provider の API・目標語彙) | **ない**(元の plan から導ける) |
| その機能は予定にあるか | ある(Metal、複数 queue、tile GPU、provider、計測) | 特定の機能に結びつかない |
| test や provider の実装があるか | ある(`headless_native_scope_test` 等) | 自分自身との一致検証だけ |

## 2. 項目ごとの判定

### 2-0 決定の状態(2026-10-17)

| # | 層 | 明言された決定 | 推奨 | 残っている判断 |
|---|---|---|---|---|
| 2-1 | `execution_plan` | **削除**(第 2 backend は書かない。mac は MoltenVK) | 同じ | なし(順番は §6)。**RC-2 で済み**(`e15e31bb5`、2026-10-23) |
| 2-2 | `levels` | **完全に消す**(2026-10-18) | 同じ | なし。**RC-2 で済み**(`e15e31bb5`、2026-10-23) |
| 2-3 | 物理計画の固定ルート(pin / freeze / complete) | **方針決定(2026-10-22): 固定のルートを `target_planning` の policy に一本化する**。細部は後で詰める | 同じ | 語彙、`require` / `prefer`、優先順位(§2-3)。**RC-6a1 で強さ・出所・優先(root > レシピ > 部品)と部品の宣言が入った**(`3fa229eba`、2026-10-28。§2-3 の [RC-6a1](#rc6a1-implementation))。**RC-6a2 で語彙 5 つ(`backend` / `representation` / `alias_group` / `scope_break` / `scope_join`)、候補ごとの反映、最終の検証が入った**(`a99725b5f`、2026-10-29。[RC-6a2](#rc6a2-implementation))。**RC-6b1 で Studio の Pin / Freeze が policy を書くようになり**(`a0367bd23`。[RC-6b1](#rc6b1-implementation))、**RC-6b2 で旧い pin / fragment の鍵・RPC・dump の欄を消した**(`4e21d7d03`。[RC-6b2](#rc6b2-implementation)) |
| 2-4 | `declared_bytes` = 0 | **埋める**(2026-10-22) | 同じ(RT メモ段 0 と同じ WP、v0 可) | なし |
| 2-5 | anchor と順序の二重化 | **段タグ + canvas のフィルター階層へ**(段メモ) | 同じ | 段メモ §7 の残り 1 件 |
| 2-6 | 段階 2 の typed policy | **強化する。core の JSON 二重 parse を解く**([design_render_graph_ir.md](design_render_graph_ir.md)。旧 [design_stage2_typed_pipeline.md](design_stage2_typed_pipeline.md) は置き換え済み) | 同じ | 段メモ側の未決 3 件(同メモ §6) |
| 2-7 | 文書のずれ | **メモに残す** | 文書修正だけ | なし |


### 2-1 `execution_plan`(WP238a) — (2) 削除(2026-10-17 決定。MoltenVK 採用で第 2 backend の理由が消えた)

- **RC-2 で済み(2026-10-23、`e15e31bb5`)**: 下の表の削除を全部やった(57 ファイル、+434 −4,540。丸ごと消したのは
  `src/project/executionplan.{hpp,cpp}`・`executionplanwire.{hpp,cpp}`、`src/core/renderingpass/frameexecutionadapter.{hpp,cpp}`、
  `test/frameexecutionplan_test.cpp`)。表に無かった分も消した: `FrameGraphNodeDefinition::semantic_dialect` /
  `execution_implementation`(raster pass に入れて、消した adapter だけが読んでいた欄)と、imgui の Plan Viewer の `level`
  (node の副題は `#<order>`、列は viewer が barrier から出す)。`targetrenderplanning.*` / `vulkanphysicalfragment.cpp` /
  `vulkanviewplanning.*` / `renderingsamplecount.*` には基点の時点で参照が無く、変えていない。Studio の scenario で
  `pelicanExecutionPlanState` / `execution_plan` を読むものも基点の時点で無かった(`git grep`)。帯の L3 と barrier 行の access 注記は
  置き換えずに消した(barrier の行は既に `from → to · resource · kind`)。Logical graph の縁は §6。
  ECS の `ExecutionPlan` と `view_execution_plan` は残る。以下は削除前の記録で、行番号も削除前のもの。
- 何か: plan ダンプに併記される backend 非依存 IR(`src/project/executionplan.hpp`)。dialect、implementation、endpoint、
  capability、resource use、依存、bridge、fingerprint。
- 出自: [design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md) §2 の目標語彙 `ExecutionPlan` の
  frame 版 `FrameExecutionPlan`。同設計では **FramePlan の方が「互換 artifact として段階移行する」側**で、
  Metal は「共通 logical / execution contract から別の physical plan を作る兄弟 backend」(決定 12)。
- 今の実態(削除前): `makeCompatibilityFrameExecutionPlan`(`frameexecutionadapter.cpp:560`。RC-2 でファイルごと削除)が FramePlan から
  表引きで導き、`:568` が FramePlan と一致を検証、以後はダンプだけ。
  設計の意図と**権威の向きが逆**(FramePlan が権威、`execution_plan` が写し)のまま止まっている。約 1,900 行、ダンプ 30 KB。
- 判定: 削除。Metal backend を書かない(MoltenVK で動かす)と決めたので、目標語彙として残す理由が無い。
  [design_heterogeneous_execution_graph.md](design_heterogeneous_execution_graph.md) は「兄弟 backend は当面作らない」と
  ステータスに追記し、`ExecutionPlan` / `ExecutionLinker` / `BackendProbe` の節は履歴として残す。
- 削除の対象(`develop` で全列挙):
- **RC-2b(2026-10-23、`7cee11733`)**: 消えた一致検証が FramePlan 自身の構造も守っていた(codex の敵対レビュー)。node 名の重複・order の連続・配列と order の一致を 1 つの検査にし、compiler の出口(variant の検証)と runtime の登録(`compileExecution()`)の両方で断る: `frame_plan.duplicate_node@1`、`frame_plan.order_gap@1`、`frame_plan.order_mismatch@1`。明示順序の `before` も単独の test で固定。差し替えた compiler が古い FramePlan と新しい計画を混ぜて返す穴は、FramePlan と論理 graph を同じものから導く RC-3 で閉じる。

| 層 | ファイル | 中身 |
|---|---|---|
| project | `src/project/executionplan.{hpp,cpp}`、`executionplanwire.{hpp,cpp}`(CMake 2 行) | 型、canonicalize、fingerprint、wire の検証 |
| core | `src/core/renderingpass/frameexecutionadapter.{hpp,cpp}`(CMake 1 行)、`framegraphruntime.{hpp,cpp}`(`execution_plan` の保持・互換生成・検証)、`rendercompilerprogram.{hpp,cpp}`(variant の `execution_plans` と一致検証)、`vulkanrendercompilerprogram.cpp`(`compileFrameExecutionPlan`)、`frameplanner.hpp:118`(dialect selection のコメント)、`renderer.cpp` のダンプ鍵 | 約 1,900 行 + 呼び出し |
| Studio | `frameplanmodel.{hpp,cpp}`(`FramePlanExecutionPlan`、`validateExecutionPlanWire`)、`frameplanwidget.cpp:4286 / 4655`(Execution plan の節: dialect、endpoint、resource uses)、`frameplangraphics.cpp:2617-2769, 3692`(unavailable の panel、`pelicanExecutionPlanState`)、`graphcanvasmodel.cpp:6375`(`graphBackend` と帯の L3「backend の名前」) | 帯の L3 と barrier 行の access 注記([第15章](manual/15_studio.md) 1317 / 1323 行)は消える。barrier の access は FramePlan の barrier から直接出せるので、要るなら置き換え |
| test | `frameexecutionplan_test`、`rendercompilerprogram_test`、`renderpipelinetransaction_test`、`vulkannativescopeexecutor_test`、`featurecompose_test`、`devstudio_frameplan_test`、`devstudio_frameplan_graph_test`、`devstudio_graphcanvas_test`、`devstudio_nodelayout_test`(該当節)、fixture `example_frame_plan.json` の `execution_plan` | |
| docs | 第6章(WP238a の段落、FP-1 の列挙)、第13章 2561 行、第15章 1317 / 1323 行、`11_status`、source-code-guide 06、`render_mechanism_coverage`、`render_evidence_ledger`、`link_anchors.json`、design 3 本のステータス | |

- `view_execution_plan`(`targetrenderplanning.hpp:508`、multi-view の実行回数)は別物。触らない。
- `get_frame_plan` の鍵が減るので v1。Studio の scenario(`pelicanExecutionPlanState` を読むもの)も同じ WP で直す。

### 2-2 `levels` — (2) 削除(決定 2026-10-18。`diagnostics` にも残さない)

- **RC-2 で済み(2026-10-23、`e15e31bb5`)**: `computeLevels()`、`FramePlanNode::level`、`FramePlan::levels`、ダンプの `levels` と
  node の `level` を消した。`diagnostics` にも出していない。`levels` を代理にしていた比較は、node 名を `order` で並べた列と
  `barriers` の比較にした(`frameplanner_test` の 2 件、`lp1c_decompose_support.hpp` の `lp1cComparablePlan` を使う
  `golden_bloom_upsample_harness`・`renderconfigeditor_test`・`featurecompose_test`、`featurecompose_test` の `lp0eComparablePlan`)。
  第6章の三層の説明は「順序付けとバリア導出」にした。以下は削除前の記録で、行番号も削除前のもの。
- 何か: `computeLevels()`(削除前の `frameplanner.cpp:1598`)が依存辺から node の深さ(ASAP)を出し、
  `:1899` で層にまとめて `:2125` で JSON へ。
- 消費者: runtime・Studio・web に無し。2-1 の一致検証が `level` を比べるだけ。マニュアル 06:1146 は「機械最適化で層を作る」と説明。
- 並列化のときに使えるか: 使えるのは半分だけ。queue への振り分けには ASAP(= `levels`)と ALAP の両方、資源の寿命(同時確保の山)、
  queue の capability、同期点(timeline semaphore、queue ownership transfer)のコストが要る。設計 §4.2 では
  「domain 内外の happens-before と並行可能性」は `TargetExecutionCompiler` の担当で、そこが自分の入力を作る。
  `computeLevels` は 10 行で、その時に書き直す方が安い。
- 判定: `computeLevels`、node の `level`、ダンプの `levels` を消す。`diagnostics` にも残さない(消費者の無い診断は誤読の供給源になる。マニュアルの「機械最適化が層を作る」がその実例)。
- 置き換え: `frameplanner_test` の 1589 / 2419 行と `golden_bloom_upsample_harness` の 1742 行は `levels` を「plan が変わった / 変わらない」の代理に使っているので、`order` と `barriers` の比較にする。
  第6章の三層の説明から「層別」を外し「順序付けとバリア導出」にする。
- 条件: 2-1 と同じ v1 WP に同梱。複数 queue の WP は「依存辺から ASAP / ALAP を計算し、割り当ての根拠をダンプに出す」ことを自分の受け入れ条件に持つ(値は消費者と一緒に生まれる)。

### 2-3 物理計画の固定ルート — policy に一本化する(方針決定 2026-10-22。細部は後で詰める)

- **出発点の違和感**: 物理計画の compile は決定的(同じ入力なら byte 一致。決定性ゲート)なのに、「固定する」ルートが 3 つある。
  Pin(候補の固定、`ejectable_pin_package` → `vulkan_plan_pins`)、Freeze(計画の写し、`ejectable_physical_fragment` → `vulkan_physical_fragments`)、
  complete package(runtime の検証済み artifact)。3 つの中身は同一ではない(fragment は 3 鍵への縮約、complete は検証用の全量、直下は説明付き)。

| 変わる入力 | 今の手段 | 判定 |
|---|---|---|
| ユーザーの意図(alias しない、この候補で) | `target_planning` の policy(`serial` / `isolate` / `no_alias`)と Pin | 要る。Pin は policy の 1 鍵で書ける |
| エンジンの版(planner の改善で答えが変わる) | Freeze(写し) | 要らない。版は `engine_min_version` で固定し、差は golden が検出する。「版を上げても古い計画を保つ」は feature 単位で追随する方針と逆向き |
| device(tile_local が可能か) | planner が device facts から決める | 固定ではなく再決定の対象 |

- Freeze の編集種類 V1〜V3(materialization、scope 分割、alias group、format、load / store、scope edit mode)は**どれも planner への制約として書ける**。
  写しとして持つから graph の変更で stale になり Eject again が要る(preset の fork と同じ構造の摩擦)。

**方針**

```text
入力:  target_planning(policy。名前で指す: node / resource / scope)
         serial / isolate / no_alias(今ある)
         + backend(Pin の代わり)
         + materialize / format / alias_group / scope_split / load_store(Freeze の編集種類の代わり)
       ↓ 決定的な planner(device facts を見る)
出力:  物理計画 1 つ(= 今の complete package。runtime が検証して消費)+ 説明(backend_selection / decisions / planning_opportunities)
```

- Studio の Pin は `backend` の policy を書く操作に、Freeze は「今の計画の決定を policy として書き出す」操作(eject as policy)になる。
  名前で指すので graph の変更に強く、消えた名前は purge の規則で名前付きの警告。
- `ejectable_pin_package` / `ejectable_physical_fragment` / `vulkan_plan_pins` / `vulkan_physical_fragments` / `put_vulkan_physical_fragment` / `edit_vulkan_plan_pins` は消える。
  → **RC-6b2 で消えた**(`ejectable_complete_physical_plan` も。下の [RC-6b2](#rc6b2-implementation))。
  complete package は残るが、ユーザーのルートではなく runtime の artifact(NativeScope の identity と検証)。ダンプは **plan + policy + 説明**。
- 線引き: **写し(eject)を許すのはユーザー空間の成果物(feature のコピー)だけ。導出物には許さない。** preset レシピ化と同じ向き。
- 失うもの: 版を上げたときに再決定せず古い答えを使う能力。版固定で代替する。policy で表せない決定が出たら、写しに戻らず語彙を足す。

**後で詰める細部**

1. 語彙: 初期集合は fragment の V1〜V3 の編集種類。名前は「planner への制約」として揃える(`scope_split` / `alias_group` は結果の指定に見えるが、planner が検証して従うなら実害なし)。
2. 強度: `require`(満たせなければ名前付きエラー。既定)/ `prefer`(tie-break)。
3. 優先順位: project > feature > engine 既定。衝突は名前付きエラー。preset の `target_planning` を root が上書きできない既存の合成規則と整合させる。
4. feature が自分の資源について policy を宣言できる形(fragment では不可能だった。feature 単位の追随と噛み合う)。
5. 版をまたいだ byte 一致は policy では約束しない、を文書に明記。
6. 置き場: v1 の物理計画 WP(RT メモの区間詰め alias と同じ場所)。順序は 語彙 → 強度と優先順位 → Pin / Freeze を policy 書き出しに置換 → package と RPC の削除。

<a id="rc6a1-implementation"></a>

**RC-6a1 で入った物(2026-10-28、`3fa229eba`、branch `agent/rc6`)。**仕様は `<作業ディレクトリ>/rc6_spec.md` §1、実装者の報告は `<作業ディレクトリ>/rc6a1_report.md` と修正の報告 3 本
(`rc6a1_fix_report.md` / `rc6a1_fix2_report.md` / `rc6a1_fix3_report.md`)。この段の行番号は `3fa229eba` の物。語彙は今の 4 つ(node の `serial` / `isolate` / `fuse`、
資源の `no_alias`)のままで、上の「後で詰める細部」の 2〜4 がこの形になった。利用者の決定(2026-10-28)は「planning は合成した全体で行い、部品(feature・構造の断片)は
自分の pass と資源の名前にだけヒントを書ける。root はどの名前にも書け、部品と食い違えば root が勝つ」。

- **値と強さ**: 各値は裸の bool か `{"value": bool, "strength": "require" | "prefer"}`。ほかの鍵、`value` の無い object、別の綴りの `strength` は組み直しの誤り
  ([包んだ値を読む所](../src/project/renderpipeline.cpp#L235))。裸の値の強さは出所の既定で、root(fold が足した物を含む)と engine は `require`、レシピと部品は `prefer`。
- **未宣言と宣言**: 内部の型は [`PlanningValue`](../src/project/targetplanning.hpp#L287) で、空が未宣言、中身が [`PlanningDeclaration`](../src/project/targetplanning.hpp#L279)
  (値・強さ・出所 `{kind, document, pointer}`)。前の `bool` の既定値で潰す形をやめ、明示の `false` と未宣言を区別する。planner が読むのは実効値(宣言の値、無ければ既定。
  `serial` / `isolate` / `no_alias` は false、`fuse` は true)で、planner の読み方は変えていない。
- **強さの今の効き方**: planner は強さで値の扱いを変えない(`prefer` の `no_alias: true` も資源を alias の組から外す。報告の B1)。強さで違うのは、engine の強制と食い違うときの
  扱い(下)と dump の `satisfied` だけ。上の 2 の「`prefer` は tie-break」の形は入っていない。
  → **RC-6a2 で変わった**: 失敗の原因になった `prefer` は外され(`infeasible_prefer`)、候補の選択では `prefer` が cost より先に効く(下の [RC-6a2](#rc6a2-implementation))。
- **出所と優先**: root > レシピ(`@2` の preset の `config.target_planning`)> 部品(feature の文書・構造の断片の文書の `target_planning`)。(graph, 対象, 鍵) ごとに一番上の出所の
  宣言が採られ、下の宣言は `overridden`([`resolvePlanningGraph()`](../src/project/renderpipeline.cpp#L981))。強さは優先を逆転させない。上の宣言の `false` は下の `true` を打ち消す。
  上の 3 の「project > feature > engine 既定」とは engine の位置が逆で、engine の強制はどの作者の宣言よりも上(作者からは上書きできない)。`required_capabilities` は全部の出所の和、
  `profile` は root だけ(レシピ・部品の `target_planning` に `profile` があれば `target_planning profile belongs to root` の誤り)。`@1` の preset の `target_planning` は今までどおり
  root から上書きできず、root の出所(`require`、document は preset の参照、pointer は `/config/target_planning/…`)として扱う。
- **部品が書ける名前(所有)**: レシピと部品は、自分の文書が宣言した pass・compute task・render target・buffer の名前にだけ書ける。他の名前(`required_targets` で借りた target を含む)は
  `target_planning.policy_foreign_name@1`(`document`・`pointer`・`target`。[`collectPartPlanningPolicy()`](../src/project/renderpipeline.cpp#L925))。feature は、採った integration の
  fragment が宣言する名前も自分の物になる([所有に足す所](../src/project/featurecompose.cpp#L3735))。採らなかった integration の名前は他人の名前。部品どうしが同じ名前を宣言すると
  今の合成が断るので、同じ対象に宣言できるのは root・レシピ・その部品の縦の 1 列だけで、部品どうしの衝突の規則は作っていない。
- **engine の強制**: `once` / `sprite` / `ui` / `imgui` の node の isolate([`isolatedNodeConstraints()`](../src/core/renderingpass/renderingsamplecount.cpp#L151))と、
  graph.instance の scope に入る node の isolate(前は合成が作者の `target_planning` の JSON に `true` を書き込んでいた。今は engine の出所の宣言として合成する。
  [足す所](../src/project/featurecompose.cpp#L3443))。採った作者の宣言がこれと逆(`isolate: false`)なら、`require` は `target_planning.policy_conflicts_engine@1`
  (graph・target・key と出所の document・pointer)、`prefer` は捨てて dump に `engine_forced` を残す。
- **集める順と結び付け**: (1) `fragment_overrides` の選択・feature の採否・parameter の束縛を今のまま確定する。(2) 採った文書だけから出所つきで宣言を集め、所有を検査する
  (選ばれなかった断片の宣言は集めない)。(3) 置き換えや inactive で対象が消える前に、宣言を対象に結び付ける([`bindPlanningPolicyTargets()`](../src/project/renderpipeline.cpp#L950))。
  root は最終名の完全一致を先に、無ければ作者名で、レシピと部品は作者名で探す。どちらの名前でも 2 つ以上の対象に当たれば `target_planning.policy_name_ambiguous@1`
  (`graph`・`name`・`candidates: [{author_name, final_name, owner}]`)。(4) `pass_replacements` / inactive は宣言を消さず、結び付いた対象が消えた印だけを付ける
  (前の `forgetPlanningNodes` は宣言を消していた。今は何もしない)。消えた対象の宣言を、置き換え先や同じ名前の別の対象へ付け直さず、graph.instance の展開先にも写さない。
  variant の graph(`#xr` など)には graph 名の接尾辞を付けて写す。最終の IR(variant の改名と engine の node の挿入の後)では、まだ結び付いていない宣言(engine やループが
  後で作った対象を指す物)だけを結び直す([`resolveFinalPlanningPolicy()`](../src/project/renderpipeline.cpp#L1120))。fold が足す宣言は、fold が provenance で選んだ作者名で、
  通常の宣言と同じ対象の一覧に結ぶ(修正の報告 3)。
- **消えた対象(absent)**: 宣言の対象が合成の結果に無いとき(graph ごと無い場合も)、root の宣言は warning `target_planning.name_absent@1`(`severity: "warning"`・`graph`・`name`・`key`)
  を pipeline の diagnostics に出して捨てる。`require` でも compile は止めない。レシピと部品の宣言は warning を出さず、dump に `absent` で残す(置き換えで消えるのは正常のため)。
- **旧い package との同居の拒否**: graph に作者の宣言(engine 以外の出所。`profile` と `required_capabilities` は数えない。status は問わない)が 1 つでもあり、同じ graph に
  `vulkan_plan_pins` / `vulkan_physical_fragments` / complete physical plan の install のどれかがあれば `target_planning.policy_with_legacy_package@1`(`graph`)
  ([`requireNoLegacyPlanningPolicy()`](../src/project/targetplanning.cpp#L45))。旧い package は planner の後で link / install されて policy を見ないので、同居させると `require` が破れる。
  断る所は物理計画の bridge([pin / fragment を見る所](../src/core/renderingpass/renderingsamplecount.cpp#L1682))、planner の入口、fragment の linker、complete の install
  ([install で断る所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L933))。Studio の Pin / Freeze は今も旧い package を書くので、policy のある graph では断られる
  (コードの読み。Studio の道を通した test は無い)。
  → **RC-6b2 で変わった**: pin と fragment は文書の鍵ごと無くなり、この拒否が出るのは complete の install だけ(下の [RC-6b2](#rc6b2-implementation))。
- **dump**: `get_frame_plan` の `physical_target_plan` に、宣言が 1 つでもある graph だけ `policy` の配列を出す(無いときは欄ごと無い。policy を書かない 16 構成の
  snapshot は 18a1807fc で記録した物と一致。報告の A5)。各行は `{graph, target, key, strength, origin: {kind, document, pointer}, status, value, satisfied?}`
  ([JSON にする所](../src/project/targetplanning.cpp#L26))。`status` は `adopted` / `overridden` / `engine_forced` / `absent`(型には `infeasible_prefer` もあるが今は出ない)。
  `satisfied` は採った `require` だけが持ち、今は planner が計画を作れた時点で `true` を付ける(選んだ計画を宣言と照らし直す検査はまだ無い)。
  graph.instance の scope 由来の engine の宣言も台帳に入るので、その graph では `origin.kind: "engine"` の行が出る(コードの読み)。policy は fingerprint に入らない
  (計画が同じなら fingerprint も同じ。報告の B1)。
  → **RC-6a2 で変わった**: `infeasible_prefer` が出るようになり、`satisfied` は選んだ計画を最終の検証で照らした結果になった(下の [RC-6a2](#rc6a2-implementation))。

<a id="rc6a2-implementation"></a>

**RC-6a2 で入った物(2026-10-29、`a99725b5f`、branch `agent/rc6`)。**仕様は `<作業ディレクトリ>/rc6_spec.md` §2、実装者の報告は `<作業ディレクトリ>/rc6a2_report.md` と修正の報告 2 本
(`rc6a2_fix_report.md` / `rc6a2_fix2_report.md`)。この段の行番号は `a99725b5f` の物。上の「後で詰める細部」の 1(語彙)と 2(強度の効き方)がこの形になった。
仕様レビューで、Freeze の編集種類のうち `format` と `store: discard` は外し、`alias_with`(許可の集合)は検証できる `alias_group` にした(仕様の冒頭)。

| 置き場 | 鍵 | 値 | 意味 |
|---|---|---|---|
| graph(`graphs.<g>.backend`) | `backend` | `"materialized"` / `"transient"` / `"tile_local"` | 3 つの候補のどれを使うか(Pin の代わり)。root とレシピだけが書ける |
| 資源 | `representation` | `"materialized"` だけ | その image を transient / tile-local にさせない |
| 資源 | `alias_group` | 空でない文字列(組名) | 同じ組の資源を同じ physical allocation に載せる。組名は書いた文書の中だけで通じる |
| node | `scope_break` | bool | true: FramePlan の順で直前の node と別の scope |
| node | `scope_join` | bool | true: FramePlan の順で直前の node と同じ scope |

- **読む所**: 値の形は RC-6a1 と同じ(裸か `{value, strength}`)で、5 つの鍵は [読む所](../src/project/renderpipeline.cpp#L257) が台帳(`PlanningGraphConstraints::declarations`)
  にだけ入れる。`nodes` / `resources` の bool の制約は作らない([作り直す所で bool 以外を飛ばす](../src/project/renderpipeline.cpp#L1079))。優先(root > レシピ > 部品)・
  absent・名前の結び付けは RC-6a1 の仕組みのまま。`scope_break` / `scope_join` の false は「関係を求めない」(未宣言と同じ。上の出所の false は下の true を打ち消す)。
- **`backend` は root とレシピだけ**: 部品(feature・構造の断片)の `backend` は読む時点で `target_planning.policy_invalid@1`([断る所](../src/project/renderpipeline.cpp#L269))。
  graph を指す宣言は所有の検査(`policy_foreign_name@1`)から外した([その所](../src/project/renderpipeline.cpp#L1000))ので、レシピの `backend` は通る。
- **`alias_group` の組は文書の中だけ**: 組は (宣言の出所の document, 組名) で決まる([`aliasPolicyKey()`](../src/project/targetrenderplanning.cpp#L3348))。部品 F と G が同じ組名を
  書いても別の組(報告の C5)。root の文書の組は graph 全体、レシピの組はレシピの文書の中。資源がどの組に入るかは、その資源の `alias_group` で採られた(一番上の出所の)
  宣言で決まる。`@1` の preset の宣言は出所が root でも document が preset の参照なので、root の文書の同じ組名とは別の組になる(コードの読み)。
- **`scope_break` / `scope_join` は隣の 2 node の関係**: FramePlan の順(RC-3d2 で固定)で直前の node との関係だけを言う。node の並べ替えはしない。
- **入力の誤り `target_planning.policy_invalid@1`**(`graph`・`target`・`key`・`document`・`pointer`。planner の入口の物は `reason` も)。planner の入口の 4 条件
  ([`validatePolicyInput()`](../src/project/targetrenderplanning.cpp#L1063)。採った宣言だけを、強さに関係なく見る): (1) FramePlan の先頭の node の `scope_join: true`、
  (2) 同じ node に `scope_break` と `scope_join` が両方 true、(3) external の資源か image でない資源(buffer など)への `representation`、(4) 部品の `backend`。
  値の形の誤り(包んだ形に `value` が無い、`strength` の綴り、型の違い、空の文字列、`backend` の 3 つ以外、`representation` の `"materialized"` 以外)も読む時点で同じ code。
- **候補ごとの反映**: 3 つの候補を作るたびに当てる([候補を作る所](../src/project/targetrenderplanning.cpp#L4007))。選んだ後に当てる形ではない(報告の C2)。1 つの候補の中の順:
  1. **表現**: [`policyMaterializes()`](../src/project/targetrenderplanning.cpp#L1084) が真の資源は、その候補でも tile-local / transient にしない(scope・寿命・alias を決める前。
     [使う所](../src/project/targetrenderplanning.cpp#L2700))。真になるのは `representation` を採った資源と、break をまたぐ値の資源。break をまたぐのは、同じ `LogicalValueId` の
     producer の次から consumer まで(consumer を含む)のどこかに `scope_break: true` の node がある値だけで、別の version の独立な write は対象にしない(修正の報告 1)。
  2. **scope**: [`buildPhysicalScopes()`](../src/project/targetrenderplanning.cpp#L2165) は `scope_break` の node で必ず新しい scope を始める。`scope_join` の node は tile 候補でなくても、
     `conservative_debug` でも直前の rendering scope への融合を試み、データの辺が無くても融合する([その所](../src/project/targetrenderplanning.cpp#L2207))。scope の種類・view execution・
     view family・view_select・sample 数・`serial` / `isolate`・辺の資源の表現・attachment の検査は今のまま効くので、成り立たない join はここで融合されない。
  3. **完成した scope の検査**: 採った `scope_break` / `scope_join` の true がある時だけ、2 node 以上の rendering scope が実行側の rendering instance の attachment の契約
     (attachment の並び、depth の view は 1 つ、間の Load / Store)を満たすかを見る([`executableScopeAttachments()`](../src/project/targetrenderplanning.cpp#L2049)、
     [呼ぶ所](../src/project/targetrenderplanning.cpp#L2973))。破れは候補の失敗 `pelican.plan.scope_attachment_contract@1`。scope が完成して local read が決まった後で見て、
     途中の prefix では判定しない(後の node の local read で合法になるため。修正の報告 2)。
  4. **alias**: [`applyCandidateAliasPolicy()`](../src/project/targetrenderplanning.cpp#L3481) が `alias_group` の組を、自動の greedy な組み分けより先に clique として組む。
     組の要素は `require` を先に名前の順で足し、足す要素は既に入った全要素と対で合法でなければならない。合法は今の alias の候補の判定で、profile を既定に戻して判定する
     ([その所](../src/project/targetrenderplanning.cpp#L3490))ので、`conservative_debug` / `hazard_stress` が間引いた対は組で取り戻せるが、`no_alias`・`isolate`・物理の互換は越えない。
     要素が 1 つしか残らない組は allocation を求めない。前は選んだ後に 1 回だけ作っていた `planning_opportunities` と alias の組を候補ごとに作り、選んだ候補の物を計画に入れる
     ([その所](../src/project/targetrenderplanning.cpp#L4111))。
  5. **結果の照合**: [`finishCandidatePolicy()`](../src/project/targetrenderplanning.cpp#L3530) が採った宣言(`require` も `prefer` も)を、その候補の結果で照らす(下の最終の検証と
     同じ [`policySatisfied()`](../src/project/targetrenderplanning.cpp#L3361))。成り立たなければ候補の失敗 `target_planning.requirement_unsatisfied@1` に原因の宣言を付ける。
  6. **物理の失敗の原因**: 台帳が空でない時だけ、scope-local な値の逃げ・attachment の契約・color attachment の予算の失敗に、関わる node の `scope_join` / `scope_break` /
     `serial` / `isolate` と、その node の間の値の `representation` を原因として付ける([`appendPhysicalFailureCauses()`](../src/project/targetrenderplanning.cpp#L3549))。
     原因の無い失敗(device や graph の事実)は、prefer を外しても直らない失敗として扱う(修正の報告 2 の high)。
- **prefer を外して作り直す(`infeasible_prefer`)**: 候補に失敗があれば失敗ごとの原因を見る。原因に `prefer` が 1 つも無い失敗(`require` だけ・原因なし)が 1 つでもあれば
  その候補は不成立。そうでなければ、全部の失敗の原因の `prefer` のうち台帳の順で最初の 1 つを `infeasible_prefer` にして作り直し、これを繰り返す
  ([その所](../src/project/targetrenderplanning.cpp#L4026))。外した `serial` / `isolate` / `fuse` / `no_alias` は bool の制約からも外す。原因に入らない `prefer` は残る。
  `infeasible_prefer` は候補ごとの印で、dump に出るのは選んだ候補の台帳。RC-6a1 の 4 つの鍵の `prefer` もこの対象になる(例: `alias_group` の `require` と食い違う `no_alias`
  の `prefer`、tile_local の `require` と食い違う `serial` の `prefer`。test)。
- **選び方(`prefer` は cost より先)**: `require` を採っていて 3 候補とも成り立たなければ `target_planning.requirement_unsatisfied@1 {graph, candidates: [{candidate, failures:
  [{id, target, reason}]}]}`([その所](../src/project/targetrenderplanning.cpp#L4064))。台帳が空でなければ、成り立つ候補のうち「その候補で採られたままの `backend` の
  `prefer` があるか」「採られたままの他の `prefer` の数」の組が最大の物だけを残し、その中を今の cost(最後に candidate ID)で選ぶ([その所](../src/project/targetrenderplanning.cpp#L4073))。
  だから `backend` の `prefer` は、指した候補が成り立てばより安い候補があってもそれが選ばれ(報告の C7)、成り立たなければ `infeasible_prefer` で cost の順に戻る。
  この時の `pelican.plan.backend_candidate_selected@1` の detail は `policy preference followed by deterministic cost tuple`。`backend` の `require` は他の 2 候補を不成立にする。
- **最終の検証**: 選んだ計画を [`validateVulkanTargetPlanRequirements()`](../src/project/targetrenderplanning.cpp#L3844)(header で公開し、test の口も兼ねる)で 1 回照らす
  ([呼ぶ所](../src/project/targetrenderplanning.cpp#L4247))。採った `require` 全部(RC-6a1 の `serial` / `isolate` / `no_alias` / `fuse` を含む)について計画の実物を読む:
  `backend` は選んだ候補、`representation` は `materialized_image`、`alias_group` は同じ組の要素が同じ allocation で、入った組の全部の対が合法、`scope_*` は直前の node との
  関係(先頭の node の break は成り立つ)と、scope の宣言がある時は全 scope の attachment の契約、`no_alias` は 2 つ以上の組に入っていない、`serial` / `isolate` は自分だけの
  scope で並列・融合の候補に出ない(`isolate` は使う資源も alias されない)、`fuse: false` は lowering で他の node と融合していない。false の値は(`fuse` を除き)いつも成り立つ。
  1 つでも成り立たなければ `requirement_unsatisfied@1 {graph, candidate, requirements: [...]}`。候補の反映を飛ばした計画を直接渡しても断る(報告の C4)。旧い package は §1 の
  同居の拒否で policy のある graph に届かないので、この検証が公開される計画の検証になる。
- **`satisfied`**: 最終の検証の結果で、採った `require` の行だけが持つ(`prefer` の行は持たない)。成り立たなければ compile が断るので、組めた計画の dump の値はいつも `true`
  (コードの読み)。
- **policy を書かない時**: 台帳が空なら、入口の検査・表現の決定・scope の反映・失敗の原因づけ・`prefer` の順位・最終の検証はどれも何もしない(scope の契約の検査は採った
  `scope_break` / `scope_join` の true がある時だけ)。16 構成の snapshot は 18a1807fc の記録と一致(報告の A5。修正の後も)。policy は fingerprint に入らない。
- **RPC**: `edit_target_planning` に `backend` を足した。送った値をそのまま `graphs.<g>.backend` に保存し、`null` で消す([その所](../src/core/communication/renderconfigeditor.cpp#L5532))。
  `backend` だけの要求も受ける。node と資源の新しい鍵は今の `nodes` / `resources` の混ぜ方で書いた形のまま保存し、値は組み直しで判定する。
- **残り**: Studio の Pin / Freeze の置き換え(報告の案では、Pin は `backend` の `require` / `null`、Freeze は `backend`・`scope_break` / `scope_join`・`representation`・
  `alias_group`・`no_alias` の `require` の書き出し)と旧い package・RPC の削除は RC-6b。新しい語彙を指定した
  GPU の描画、全数・scenario、XR 無効の build は未検証(報告)。
  → **RC-6b1 で Studio の Pin / Freeze の置き換えが入った**。報告の案と違い、Freeze は `no_alias` を書かない(下の [RC-6b1](#rc6b1-implementation))。
  → **旧い package・RPC は RC-6b2 で消した**(下の [RC-6b2](#rc6b2-implementation))。

<a id="rc6b1-implementation"></a>

**RC-6b1 で入った物(2026-10-29、`a0367bd23`、branch `agent/rc6`)。**仕様は `<作業ディレクトリ>/rc6_spec.md` §5 の RC-6b1、実装者の報告は `<作業ディレクトリ>/rc6b1_report.md` と修正の報告
`rc6b1_fix_report.md`。この段の行番号は `a0367bd23` の物。上の「方針」の「Studio の Pin は `backend` の policy を書く操作に、Freeze は今の計画の決定を policy として書き出す操作に」と、
「後で詰める細部」の 6 の 3 段目(Pin / Freeze を policy 書き出しに置換)がこの形になった。**固定のルートは Studio の側では root の policy の 1 本になった。**旧い package
(`vulkan_plan_pins` / `vulkan_physical_fragments`)と、それを書く `edit_vulkan_plan_pins` / `put_vulkan_physical_fragment`、dump の `ejectable_*` は今は残っているが、Studio は使わない。
→ **RC-6b2 で全部消えた**(下の [RC-6b2](#rc6b2-implementation))。

- **書く道は 1 本**: Pin / Unpin / Freeze / Remove と Frame Plan の Serial / Isolate / No alias の check は、どれも今の `edit_target_planning` を 1 回送る(新しい RPC は無い)。
  CAS も文書の履歴も 1 回で、render の編集の履歴の 1 項目(`Edit planning decisions`)を Undo 1 回で全部戻す。RPC で変えたのは 1 点だけで、合成の結果に無い名前でも、保存済みの
  葉だけを `null` にする要求を受ける([受ける所](../src/core/communication/renderconfigeditor.cpp#L5580))。Remove が消えた名前の葉を消すため。
- **作者の名前で書く**: graph 名は、表示中の variant の compiler が実際に付けた接尾辞だけを外す(`get_frame_plan` の最上位に足した `graph_variant: {name, name_suffix}`。
  [足す所](../src/core/vkcore/renderer.cpp#L4236)、[外す所](../src/devstudio/model/frameplanpolicy.hpp#L13))。作者が書いた `#xr` は残る。node 名は frame plan の `authored_name`。
  policy は variant に共通で、root の宣言は RC-6a1 の規則(最終名 → 作者名)で各 variant に結ぶ。XR の compute task を含む graph の Freeze が flat と XR の両方で同じ計画になる(test)。
- **Pin**: root の `backend` を `require` で書く / Unpin は `null`([`framePlanBackendPinRequest()`](../src/devstudio/view/frameplanedit.cpp#L2211))。「Pinned」の表示は root に `backend` の
  宣言があること(手で書いた `prefer` も含む)。
- **Freeze**: 表示中の物理計画から root に `require` で書き出す([`freezeFramePlanPolicy()`](../src/devstudio/model/frameplanpolicy.hpp#L23)、
  [`framePlanPhysicalFreezeRequest()`](../src/devstudio/view/frameplanedit.cpp#L2234))。
  - 書く物: `backend`、先頭以外の各 node の `scope_break` と `scope_join` の**両方**(直前と別の scope なら break true / join false、同じなら逆。下の出所や手書きの逆の宣言を打ち消し、
    片方だけで両方 true になる `policy_invalid@1` を避ける)、共有する allocation ごとの `alias_group: "frozen_<n>"`(`n` は dump の `alias_groups` の並びで 1 から、root にある組名と
    重なれば次へ)、`materialized_image` の資源の `representation: "materialized"`。
  - 書かない物: `no_alias`、format、load / store。
  - 書かない名前: ループの回の node(`loop_origin`。作者が指せない展開名)、runtime の合成だけが足す node(`imgui_pass` など)、文書が名指せない資源(Blank に runtime で足される
    picking の `picking_depth` / `picking_id`。修正の報告)。判別は authoring context の `planning_catalog`(RPC の preflight と同じ名前の一覧。[`planningNamesOf()`](../src/core/communication/renderconfigeditor.cpp#L3357))
    に名前があるか。ループの回を飛ばした数は status 行の文と Problems の Info の行(`target_planning.freeze_loops_skipped@1`)に出る。runtime だけの node の境界は engine の isolate が保つ
    (ImGui を有効にした graph で Freeze の前後の計画が一致する test)。
  - **保証の範囲**: Freeze の直後は、backend・node 名で見た scope の分け方・資源名で見た alias の組・representation が Freeze の前と同じ(test)。その後に graph を変えたときに保つのは
    書いた正の制約だけで、組に入っていない資源が後で共有される・transient / tile-local の扱いが変わる、は起こり得る。上の「後で詰める細部」の 5(版をまたいだ byte 一致は約束しない)
    のとおり、版をまたいで同じ計画を保つ物ではない(利用者の決定)。効いている証拠として、Freeze の後に自動の計画なら scope の融合が変わる合法な入力の変更(`view_select` を外す)を
    加えても Freeze の scope の分け方が保たれることを、Freeze なしでは変わることと一緒に test が見る(報告の D2)。
  - Studio は送る前に計画を予測しない。書き出した policy が成り立たなければ適用の前の検証で要求全体が断られ、一部だけ書いた Freeze にはならない。
- **Remove**: 固定用の 5 つの鍵(`backend` / `scope_break` / `scope_join` / `alias_group` / `representation`)を、手書きと Pin の分も含めて root の文書から数えて全部消す
  ([`framePlanPhysicalRemoveRequest()`](../src/devstudio/view/frameplanedit.cpp#L2264))。計画が失敗している間も押せる。`serial` / `isolate` / `fuse` / `no_alias` は残す。
  Undo は手で書いた形のまま戻す。状態の表示は「固定用 policy あり」で、Freeze が済んだ証拠としては扱わない。**Eject again は無くなった**(名前で指すので古くならない)。
- **check**([`framePlanPlanningCheck()`](../src/devstudio/view/frameplanedit.cpp#L2280)): checked は dump の `policy` の行で `adopted` の値。`infeasible_prefer` は unchecked で理由を
  tooltip に、root 以外の出所は ` (inherited)` と tooltip に出す。押すと root に今の表示の逆を `require` で書き、root に宣言がある時だけ「継承に戻す」(root の宣言を `null`)を別に出す。
  engine の強制の isolate と fold の決定(`origin.pointer` が `/fold_groups/`)の check は押せず理由を出す。
- **物理計画の dump は変えない**: engine の強制の情報は物理計画の台帳(`policy`)に足さない(policy の無い project で欄が出ると A5 の比較が壊れる)。Studio が読むのは
  authoring context に足した `planning_catalog.<graph>.engine_isolates`([足す所](../src/core/communication/renderconfigeditor.cpp#L3656))と、plan の `once` の node と `imgui_pass`。
  complete physical plan の install の有無も `physical_target_plan` の外の最上位 `legacy_complete_plan`([付ける所](../src/core/vkcore/renderer.cpp#L4312))。A5 の 16 構成の比較は
  記録し直さずに一致(報告)。
- **旧い package との同居**: RC-6a1 の同居の拒否と同じ理由で、旧い package(root の pin / physical fragment、applied の pin / fragment、complete の install)がある graph では Studio が
  Pin / Freeze を送る前に断る。移行の機能は作らない。
  → **RC-6b2 で変わった**: 見るのは `legacy_complete_plan` だけで、文は `This graph has a complete package: …`(下の [RC-6b2](#rc6b2-implementation))。
- **表示**: Plan Viewer(ImGui の Compiled Passes。[足す所](../src/core/imgui/compiledplanviewer.cpp#L163))と Studio の Frame Plan の Physical plan のタブに、dump の `policy` の行
  (出所・`status`・`satisfied`)を出す。
- **確かめた範囲**: シナリオ `gui_lower_layer_pin_freeze` は統合側の仮想モニタで緑(着地前の gate、シナリオ 26 本)。GPU の実機の描画・XR の実機は未検証。

<a id="rc6b2-implementation"></a>

**RC-6b2 で消した物(2026-10-29、`4e21d7d03`、branch `agent/rc6`)。**仕様は `<作業ディレクトリ>/rc6_spec.md` §5 の RC-6b2、実装者の報告は `<作業ディレクトリ>/rc6b2_report.md` と修正の報告
`rc6b2_fix_report.md`、消した symbol の一覧は `<作業ディレクトリ>/rc6b2_evidence/removed_symbols.txt`。上の「後で詰める細部」の 6 の 4 段目(package と RPC の削除)がこの形になった。
**物理計画を固定するルートは root の `target_planning` の policy の 1 本だけになった。**

- **消した物**: 文書の `vulkan_plan_pins` / `vulkan_physical_fragments`、RPC `edit_vulkan_plan_pins` / `put_vulkan_physical_fragment`(登録・`edit_batch` の表・Studio の送信の分岐・
  render の編集の履歴の分類と label。送ると method not found)、dump の `ejectable_pin_package` / `ejectable_physical_fragment` / `applied_pin_package` / `applied_physical_fragment` /
  `ejectable_complete_physical_plan`(wire の validator・Studio の model・Plan Viewer からも)、型 `VulkanTargetPlanPinPackage` / `VulkanPhysicalFragmentPackage` と fragment の
  resource / scope / alias / attachment の型・scope edit mode・stale の型、pin と fragment の parse / serialize / eject / link、`physical_fragment_stale` と
  `physical_fragment.reorders_frame_plan@1`、authoring context の `planning_catalog.<graph>.legacy_package`。ファイル丸ごとの削除は無い(共有の部分は `vulkanphysicalfragment.cpp` に残した)。
- **旧い鍵の拒否**: 文書(root の rendering 節・preset と `@2` のレシピの `config`・構造の断片)に 2 つの鍵のどちらかがあれば、値によらず(空・`null`・policy の有無を問わない)
  `target_planning.legacy_package_removed@1 {key, document, message}`。`message` は `Remove <key> from the document and reload it, then recreate the policy with Studio Pin / Freeze.`
  (読み込めない文書に Freeze を勧めない)。読む所の共通の guard は [`rejectRemovedPlanningPackages()`](../src/project/renderpipeline.cpp#L882)。`restore_render_documents` は
  消さない文書を 1 つずつ同じ guard に通す(復元の後に選ばれなくなる文書も。修正の報告)。黙って無視はしない。自動の移行は作らない。
- **無くなった能力**(project 空間での使用は 0 件): format の上書き、attachment の store の破棄、個々の load / store の編集、版をまたいで同じ計画を固定すること。
  backend の固定・scope の分割と融合・alias の組・materialize は RC-6a2 の policy で書ける(報告: RC-6a2 C1〜C7 と RC-6b1 D2 が同じ事実を持つ)。
- **残した物**: `validateVulkanPhysicalAttachmentPlans`・`vulkanAutomaticTargetPlanFingerprint` と helper・`validateVulkanPhysicalFeatureClosure`・`physicalNodeOrderOf`・最終の policy の
  検証・`VulkanPhysicalResourceFormatCapability`。complete physical plan の型・C++ の eject / verify / install と、policy との同居の拒否は NativeScope のために残し、dump には出さない。
  verification context の `automatic_plan` と install の時の同一性の検査も残した(compile の直後は `automatic_plan` と `compiled_plan` が同じ object)。Studio の Pin / Freeze の
  前提は `legacy_complete_plan` だけを見る(`This graph has a complete package: Pin / Freeze cannot add policy (target_planning.policy_with_legacy_package@1).`)。
- **test**: 消すかどうかは assertion / SECTION の単位で決め、17 test case と 5 SECTION を消した(一覧は報告に)。RC-5s の stale の test は complete の stale の検査だけを分けて残し、
  A5 は記録し直さずに、期待と実際の両方から廃止した欄(snapshot の `pins` / `physical_fragments` と dump の 5 欄)だけを明示して除いて比べる。
- **確かめた範囲**: 非 GPU の触った family・NativeScope の GPU 2 件・A5 は緑(報告)。GPU・非 GPU・シナリオの全数は統合側(報告の時点で未実施)。

<a id="rc6c-implementation"></a>

**RC-6c で入った物(寿命。2026-10-30、`be795c9a5`、branch `agent/rc6`)。**仕様は `<作業ディレクトリ>/rc6_spec.md` §6、実装者の報告は `<作業ディレクトリ>/rc6c_report.md`。この段の行番号は `be795c9a5` の物。
寿命の意味と 7 つの理由・example の数字は [RT メモ §2 の末尾](design_render_target_memory_reduction.md#rc6c-lifetime)に書いた。ここには plan の層として変わった所だけを書く。

- **寿命の計算は 1 つの純粋な関数**: [`calculateTargetResourceLifetimes()`](../src/project/targetrenderplanning.cpp#L1594)(入力は canonical graph・検査済みの `FramePlanNodeOrder`・資源名ごとの理由、
  出力は資源ごとの `used / first_use / last_use / retain`)。planner([`makeTargetLoweringGraph()`](../src/project/targetrenderplanning.cpp#L1625) の中。depth export の元を選んだ後に
  [計算し直す所](../src/project/targetrenderplanning.cpp#L3820))と complete verifier([その所](../src/project/vulkancompletephysicalplan.cpp#L1822))が、それぞれ自分の入力から計算する。
  前は 2 か所が別々に first / last を取り、別々に一律の末尾延長をしていた(planner は lowering の資源、verifier は自動計画の lowering の資源を見て延長)。
- **理由は信頼する入力**: 理由は `VulkanTargetPlanRequest::retain_reasons` で planner に入り、`VulkanTargetPlan::retain_reasons` に残る(寿命の出力とは別の欄で、
  dump にも package にも書かない)。verifier はこの欄を読み、package の `lifetime` の申告や自動計画の寿命を写さない。
- **complete package の `lifetime.retain`**: complete の resource の `lifetime` が `retain` を持ち、parse・serialize・比較の対象になった。比べるのは `TargetResourceLifetime` の `==`
  (`retain` を含む)で、`last_use` か `retain` を変えた package は stale な lifetime として断られる。complete package は NativeScope のための C++ の口のまま(RC-6b2)。
- **dump**: `physical_target_plan` の `resources[]` と `lowering_graph.resources[]` の `lifetime.retain`(理由が無ければ欄なし)。policy の台帳と違い、理由は fingerprint に入る
  ([`appendLifetimeFingerprint()`](../src/project/vulkanphysicalfragment.cpp#L301))。A5 は記録し直さず、寿命の欄と `automatic_plan_fingerprint` だけを除いて比べる(除く前に新しい寿命を期待値で検査。報告)。
- **触らなかった層**: alias の適格性・runtime の共有・登録・representation・attachment の load / store・peak の定義。中間 RT の名前の readback の opt-in(policy の `retain`)は RC-7 に移した
  (RC-6c では alias を変えないので中身は残る)。
  → RC-7a で入った(下)。

<a id="rc7a-implementation"></a>

**RC-7a で入った物(名前の readback の約束。2026-10-31、`2d1715cf3`、branch `agent/rc7`)。**仕様は `<作業ディレクトリ>/rc7_spec.md` §1、実装者の報告は `<作業ディレクトリ>/rc7a_report.md` と修正の報告 2 本
(`rc7a_fix_report.md` / `rc7a_fix2_report.md`)。この段の行番号は `2d1715cf3` の物。約束の意味・成り立たない条件・readback の拒否は
[RT メモ §2 の末尾の RC-7a](design_render_target_memory_reduction.md#rc7a-readback)に書いた。ここには plan の層として変わった所だけを書く。

- **鍵は RC-6a1 の台帳に 1 つ足しただけ**: 資源の `retain`(bool)を [読む所](../src/project/renderpipeline.cpp#L503) が台帳(`PlanningGraphConstraints::declarations`)に入れ、
  再シリアライズの列挙([その所](../src/project/renderpipeline.cpp#L569))にも足した。優先・absent・結び付け・所有の検査・complete package との同居の拒否は鍵を区別しない既存の道を通る
  (test「RC-7a F3 retain follows root recipe and part precedence and round trips」「RC-7a F3 retain ownership absence and complete exclusion」「RC-7a F3 replacing a same-name owner does not
  carry its retain」)。`policy_invalid@1` の入口の条件([`validatePolicyInput()`](../src/project/targetrenderplanning.cpp#L1019))に `retain` の分は無く、external・buffer への `retain` は
  入口ではなく下の条件で成り立たない(コードの読み)。
- **候補・再試行ごとの作り直し**: RC-6a2 の候補の lambda の中の `build`([その所](../src/project/targetrenderplanning.cpp#L3895))が、再試行のたびに request の写しから始め、その時に採っている
  `retain: true` ごとに理由 `readback` と automatic attachment の `store` を足し、寿命を [`calculateTargetResourceLifetimes()`](../src/project/targetrenderplanning.cpp#L1602) で計算し直した
  workspace の写しで候補を作る。`CandidateDraft` に `retain_reasons` と `attachments` を足し、選んだ候補の分を `VulkanTargetPlan::retain_reasons` と `plan.attachments` にする
  ([その所](../src/project/targetrenderplanning.cpp#L4125))。だから原因つきの再試行で外れた `prefer` の `retain` は、理由も store も残さない。materialize は `representation` と同じ扱いで、
  [`materializingPolicy()`](../src/project/targetrenderplanning.cpp#L1040) が `representation` と `retain: true` の両方を真にする(台帳に派生の行は足さない)。alias の組の失敗の原因
  ([その所](../src/project/targetrenderplanning.cpp#L3460))では `retain: true` が `no_alias: true` と同じ扱いで原因に入る。
- **実効の usage の出所**: `VulkanTargetPlanRequest::effective_image_usage` と `VulkanTargetPlan::effective_image_usage`(資源名 → `ImageUsage`。header の注記どおり compiler の事実で、
  dump にも package にも書かない)。`retain: true` があるときは、compiler program が先に全 variant を 1 度計画し、登録の `completeGenerationResources()` と同じ
  [`resolveCompilerGenerationResources()`](../src/core/renderingpass/rendercompilerprogram.cpp#L223)(RC-7a で `completeGenerationResources()` から取り出した generation の統合)の答えを使う
  ([その所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L672))。variant の宣言の和・history・XR の depth export・storage の分の engine の規則は resolver の中だけにあり、
  compiler 側に別の式を重ねない(1 回目の修正の報告の 1。XR だけが depth export をする構成で flat の候補も TRANSFER_SRC を持つ test「RC-7a fix generation usage includes XR depth export」)。
  先の 1 度の計画は `retain` を `representation: materialized` に置き換えて行う(登録の storage に合わせ、transfer の bit は作らない)。`retain` が無ければ先の計画はせず、
  表は宣言と `engineImageUsage()` から作る([その所](../src/core/renderingpass/renderingsamplecount.cpp#L1626))。planner の中では、選んだ depth export の元に TRANSFER_SRC を足す
  ([その所](../src/project/targetrenderplanning.cpp#L3883))。alias の適格性と device への問いは今までどおり宣言の usage を読む(2 回目の修正の報告の 1)。
- **最終の検証が見る物**: [`validateVulkanTargetPlanRequirements()`](../src/project/targetrenderplanning.cpp#L3786) は `policySatisfied()` に `plan.attachments` と `plan.effective_image_usage` を
  渡す([その所](../src/project/targetrenderplanning.cpp#L3800))。`retain` は [`retainContractFailure()`](../src/project/targetrenderplanning.cpp#L3343) で、資源の表現・format・sample と resolve・
  usage・寿命(末尾と理由 `readback`)・全部の attachment の書き手の store を、計画の実物から読む。書き手は論理 graph の use から数え(明示の attachment intent の全 node と、automatic intent の
  render / output_transform の node)、`plan.attachments` に行が無い書き手も破れにする(行を消した計画が素通りしない)。候補の段も同じ関数で判定し、`require` が 3 候補とも成り立たないと
  `target_planning.retain_unsatisfiable@1`([その所](../src/project/targetrenderplanning.cpp#L3971))。
- **readback の許可は program の計画**: 許可の判定 [`validateTargetReadbackRetention()`](../src/project/targetrenderplanning.cpp#L3771) は 1 つの `VulkanTargetPlan` だけを見る。generation の
  usage は variant の和でも、読めるかどうかは読む program の計画で決まる(コードの注記)。
- **A5 と 16 構成**: `retain` を書かない project は計画が byte 一致(RC-6 の A5 は除外を増やさず、`283aacc05` の 16 構成は fingerprint も含めて一致。報告)。
  → RC-7b から A5 と F4 は共有の派生の欄を除いて比べる(下)。

<a id="rc7b-implementation"></a>

**RC-7b で入った物(同じ形の共有の条件を広げる。2026-10-31、`0091c071c`、branch `agent/rc7b`)。**仕様は `<作業ディレクトリ>/rc7_spec.md` §3、実装者の報告は `<作業ディレクトリ>/rc7b_report.md` と修正の報告 2 本
(`rc7b_fix_report.md` / `rc7b_fix2_report.md`)。この段の行番号は `0091c071c` の物。条件の意味・example が変わらない理由・計画の差は
[RT メモ §2 の末尾の RC-7b](design_render_target_memory_reduction.md#rc7b-sharing)に書いた。ここには何がどこにあるかだけを書く。

- **planner(`src/project/targetrenderplanning.{hpp,cpp}`)**:
  - 資源ごとの適格: [`applyCandidateAliasPolicy()`](../src/project/targetrenderplanning.cpp#L3525) の先頭の 1 つのループが `aliasable` を書く唯一の所。理由を `pelican.plan.alias_ineligible@1` の decision にする。
    `buildCandidateDraft()` の `aliasable` の式(`require_store` を見ていた)と `applyResolvedSampleCounts()` の multisample の代入は消した。
  - 対ごとの適格: [`deriveLegalAliasCandidates()`](../src/project/targetrenderplanning.cpp#L3164) が scope の列・実効の usage の表・decision の出力先を受け、実効の usage の一致(`effective_usage`)と
    同じ scope の中の住人の切替(`same_scope`)を前からの形と寿命の検査に足した。最終の検証 [`validateVulkanTargetPlanRequirements()`](../src/project/targetrenderplanning.cpp#L3869) も
    `plan.scopes` と `plan.effective_image_usage` を渡して同じ関数を呼ぶ。
  - 組の失敗の原因: [`aliasConflictCauses()`](../src/project/targetrenderplanning.cpp#L3459) が、組の 2 つの資源の両方に触る scope の `scope_join: true` も原因に入れる。組の失敗の文には、
    最初に当たった不適格の decision を `alias.<条件>@1: <対象>` として足す([その所](../src/project/targetrenderplanning.cpp#L3610))。
  - 入力の欄: `VulkanTargetPlanRequest` に `generation_alias_exclusions`(資源名 → 外した理由)と `manual_native_resources`
    ([宣言](../src/project/targetrenderplanning.hpp#L347))。どちらも `VulkanTargetPlan` に写して([その所](../src/project/targetrenderplanning.cpp#L4210))complete の verifier が読む。JSON には書かない。
    `manual_native_resources` を本番の呼び出しで埋める所は src に無い(grep で確認)。
- **宣言の入口(`src/project/rendergraphir.cpp`)**: [`isRuntimeAliasEligible()`](../src/project/rendergraphir.cpp#L241) から usage の固定集合を外した。この関数は logical materialization の
  required / virtual([使う所](../src/project/rendergraphir.cpp#L714))と、core の `isRuntimeAliasCandidate()` を通して runtime pattern の `allow_alias` / `require_store` に効く。
  core の `runtimeAliasExclusion()`(PO-1 の原因の文)からは usage の 2 つの文と、それを作る `runtimeAliasSafeUsage()` / `joinNames()` を消した。
- **generation の合意(`src/project/generationresources.{hpp,cpp}`)**: [`resolveGenerationResources()`](../src/project/generationresources.cpp#L97) が `GenerationResources::alias_rejections`
  ([宣言](../src/project/generationresources.hpp#L127))に共有を外した資源と理由を入れる。variant が合意しない組([その所](../src/project/generationresources.cpp#L201))と、組の中で usage か storage が違う組
  ([その所](../src/project/generationresources.cpp#L256)。前は `alias_group_invalid@1` の例外)。
- **graph 間の合意(`src/core/renderingpass/renderingsamplecount.{hpp,cpp}`)**: `mergeRuntimeAliasGroups()`([定義](../src/core/renderingpass/renderingsamplecount.cpp#L1327))が外した資源の理由を返し、
  [`compileRenderingTargetPlans()`](../src/core/renderingpass/renderingsamplecount.cpp#L1493) が除外に足して自分を呼び直す([その所](../src/core/renderingpass/renderingsamplecount.cpp#L1961))。
  `compileRenderingTargetPlans()` / `compileRenderingTargetPlansForVulkanDevice()` は末尾に `generation_alias_exclusions`(既定は空)を取る。登録の前の [`imagePlacementsOf()`](../src/core/renderingpass/renderingsamplecount.cpp#L2075)
  の組の検査は物理計画の実効の usage を比べる([その所](../src/core/renderingpass/renderingsamplecount.cpp#L2314))。
- **compiler program(`src/core/renderingpass/vulkanrendercompilerprogram.cpp`)**: [`compileDefaultVulkanPhysical()`](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L509) は除外の表を受け、
  先の計画(`discover_placements`)では `retain` と `alias_group` を台帳から外す([その所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L532))。program の流れは、`retain` の有無によらず
  先の計画 → generation の usage → 本計画 → 合意の除外があれば計画し直し([その所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L678)、[計画し直す所](../src/core/renderingpass/vulkanrendercompilerprogram.cpp#L701))。
- **最終の `require` の照合(`src/core/renderingpass/rendercompilerprogram.cpp`)**: [`completeGenerationResources()`](../src/core/renderingpass/rendercompilerprogram.cpp#L249) の先頭
  ([その所](../src/core/renderingpass/rendercompilerprogram.cpp#L258))。最終の物理計画の台帳と最終の `GenerationResources` の組を照らし、`target_planning.alias_generation_contract@1`。
- **complete package の verifier(`src/project/vulkancompletephysicalplan.cpp`)**: 住人ごとの再計算 `verifyAliasMember`([その所](../src/project/vulkancompletephysicalplan.cpp#L2500))と、対ごとの
  `alias.image_contract@1` / `alias.same_scope@1`([その所](../src/project/vulkancompletephysicalplan.cpp#L2588))。形と寿命の前からの検査の文の頭に `alias.shape_or_lifetime@1` を付けた。
- **同期(`src/core/vkcore/render_target_layout_tracker.{hpp,cpp}`)**: [`RenderTargetLayoutTracker::beginFrame()`](../src/core/vkcore/render_target_layout_tracker.cpp#L352)(新規)と、
  [`transition()`](../src/core/vkcore/render_target_layout_tracker.cpp#L131) の観測(`AliasTransition` / `alias_transition_observer`。[宣言](../src/core/vkcore/render_target_layout_tracker.hpp#L22))。
  呼ぶのは renderer の `beginFrameResources()`([その所](../src/core/vkcore/renderer.cpp#L6319))。test の口として `VulkanManageCore::imageAllocationBytes()`([その所](../src/core/vkcore/core.hpp#L86))を足した。
- **test**: `targetrenderplanning_test` が include する [`rc7b_target_policy_test.inc`](../test/rc7b_target_policy_test.inc#L36)、`rendercompilerprogram_test` の
  [`rc7b_compiler_policy_test.inc`](../test/rc7b_compiler_policy_test.inc#L1)、GPU の `headless_render_test` の [`rc7b_alias_test.inc`](../test/rc7b_alias_test.inc#L29)、
  `rc5_recipe_compiler_test.inc` の [`rc7bComparable()`](../test/rc5_recipe_compiler_test.inc#L447) と B4([その所](../test/rc5_recipe_compiler_test.inc#L586)。`[.]`)。
- **触らなかった層**: 配置(offset 0・owner の allocation の共有)、barrier の形(ALL_COMMANDS の memory barrier + UNDEFINED)、container の検査、置き換えの transaction、memory_summary の数え方。

### 2-4 `declared_bytes` = 0 — 埋める(決定 2026-10-22)

- arena とは: [`RenderPipelineGpuArena`](../src/core/renderingpass/renderpipelinegpuarena.hpp)。**公開中の runtime 世代が所有する
  GPU 側オブジェクトの台帳**。owner scope(`render_pipeline/flat` = 本体、`render_pipeline/variants` = variant)ごとに、
  登録(kind: render_target / frame_graph_buffer / shader_bundle / pipeline / fullscreen_pass / compute_task / debug 系 /
  shadow_depth / velocity / acceleration_structure、handle、name、`declared_bytes`)と、型消去した lease(`shared_ptr<const void>`)を持つ。
  lease は同名差し替え(hot reload)のあと、前の世代のオブジェクトを参照中の frame が退くまで生かす。
  「owner を再コンパイルするとその scope だけ置き換わり、無関係な scope と program は使える」が狙い。example は 89 登録
  (RT 21、shader bundle 34、pipeline 17、fullscreen pass 17)。
- 今: [`renderpipelinegpuarena.cpp:198`](../src/core/renderingpass/renderpipelinegpuarena.cpp#L198) で `declared_bytes = 0` 固定。
  Studio の Frame Plan がその 0 を bytes 列に出す([`frameplanwidget.cpp:5183`](../src/devstudio/view/frameplanwidget.cpp#L5183))。
  つまり arena は「所有の台帳」としては動いていて、「VRAM の台帳」としては空。
- 判定: RT メモ段 0 の計測で埋める。render target は format × extent × layers × mips(MSAA と history の枚数込み)、frame graph buffer は size、
  shader bundle / pipeline / pass 系は 0 のまま(VRAM を占めない)。埋めるまで列は出さない。
- 置き場: 計算は project の planner 側(物理計画の resource から bytes を出す純ロジック)、arena は登録時にその値を写す。`get_frame_plan` の
  `memory_summary {total_bytes, peak_live_bytes, peak_node}` と同じ WP(RT メモ段 0)。v0 で可(鍵は増えず値が変わるだけ)。
- **RC-1 で済み(2026-10-23、`631caac21`)**: 計算は `src/project/targetrendermemory`、arena は登録時に render target の bytes を写す
  (frame graph buffer は以前から大きさ。shader bundle / pipeline / pass 系と acceleration structure は 0 のまま)。
  arena の値は登録時のもので、同じ id のまま image を作り直す経路(window 以外の resize、影の layer 数の変更)では更新されない。上の「今」の行は RC-1 の前の状態。
- **RC-1b(2026-10-23、`504a6492a`)**: arena の構造体の `declared_bytes` は登録時のままだが、`get_frame_plan` の dump に出す render target の `declared_bytes` は書き出す時点の metadata から数え直す
  (上の作り直しの経路の後も今の値。Studio の bytes 列は dump の値なのでそのまま今の値になる)。arena を直接読む消費者を足すと登録時の値を読む。
  `memory_summary.total_bytes` は計画の和から「公開中の世代の arena に登録された render target 全部(runtime の alias の組ごとに 1 回)」に変わり、
  `unused_bytes` / `unused_resources` / `peak_graph` の鍵が増えた(上の判定の「鍵は増えず」は RC-1 の時点のもの)。

### 2-5 anchor ノードと順序の二重化 — [段メモ](design_render_stage_as_canvas_filter.md) §3.2〜3.3 の通り

### 2-6 段階 2(`CompiledRenderPipeline` の typed policy) — 強化する(決定。計画は [design_stage2_typed_pipeline.md](design_stage2_typed_pipeline.md))

- 今: resolve と core の間で運ぶのは draw sorting / material routing / feature instances の 3 つ。render node の typed 化は
  core 側の [`parseRenderNodeFromJson`](../src/core/renderingpass/frameplanner.cpp#L669) がやり直している(段階 3)。
  だから段階 2 が薄く見える。
- 役に立つ場面:
  1. **Metal**: 設計の `CanonicalLogicalGraph`(immutable / hashable、device 非依存)に当たる場所。node の typed 化と
     構造検証を core から段階 2 に上げれば、Vulkan core と Metal core が同じ typed 入力を受ける。2-1 の (a) と対になる。
  2. **純ロジック test**: typed な論理 graph を GPU 無しで固定できる(今は core の JSON parse を通さないと node の型が出ない)。
  3. **incremental / hot reload**: 段階 2 の出力に stable hash があれば、変わっていない rendering pass の再 prepare を省ける。
  4. **Studio**: typed policy(draw sort、material route)を JSON の再解釈なしに表示・編集できる。
- 判定: 段階 2 を「typed な論理 graph + policy」に育て、core の JSON parse を消す。preset レシピ化と同じ時期。

### 2-7 文書と実態のずれ — 文書修正

- WAW: ガイドは「出さない」、コードに `addWriteAfterWriteBarriers`。
- v0.1.0 のタグ: 文書は未付与、実際は付いている。
- `levels` の「機械最適化」の説明(2-2)。
- `execution_plan` の「観測面」は正しいが、設計文書との権威の向きの違い(2-1)を書く。

## 3. (1) 器として残す(ただし既定で見せない)

| # | 層 | 証拠 | 待っている機能 | 今やること |
|---|---|---|---|---|
| 3-1 | compute queue | [`core.cpp:233-282`](../src/core/vkcore/core.cpp#L233) で選び `:1110` で取得、command pool も作る。submit なし | 複数 queue | 触らない。コードマップに「未使用」1 行 |
| 3-2 | backend 候補 transient / tile_local と `probeCandidate` | example で 0 / 0。desktop では sampler 読みがあると選ばれない | **Apple GPU(MoltenVK、memoryless texture)**、mobile | 候補の確率と理由はダンプに残し、**選ばれなかった候補の resource 全文は出さない**(2-3 と同じ WP) |
| 3-3 | physical scope(pass の合流単位) | 30 node → 30 scope、全部 1 node。合流条件は「前が `store`、次が `load`」([`:2027`](../src/project/targetrenderplanning.cpp#L2027))。`once` / `view_select` は隔離 | **Apple GPU** の render pass 合流、NativeScope の単位 | 残す。Studio では 1 node の scope を node と別に見せない |
| 3-4 | `native_scope_executors`(WP238d) | provider の API と `headless_native_scope_test`。ツリー内に実 provider なし | 外部 provider | 残す。「🚧 source-level の拡張」を維持 |
| 3-5 | arena の owner scope / lease | 2-4 参照 | hot reload と variants(実在) | 残す。2-4 で bytes を入れる |
| 3-6 | `hazard_stress` / `isolate` profile | [`targetplanning.cpp:739-991`](../src/project/targetplanning.cpp#L739) | test の揺さぶり(実在) | 残す |

## 4. 規則(再発を防ぐ)

1. **plan の各節には消費者を書く。** 無ければ `diagnostics` 配下に置いて表示専用と明記するか、出さない。
2. **写しは作らない。** 別の形が要る消費者が来たとき、その消費者が元から導く。一致検証で自分を守る層は持たない。
   目標語彙へ移行するなら、権威を移すところまでを 1 つの WP にする(2-1 の教訓)。
3. **将来の器は入力と API を残し、出力は既定で隠す。** ダンプと Studio は「今効いているもの」を見せる。
4. **0 固定の field は出さない。** 埋められる見込みがあるなら埋める WP を先に切る。

## 5. 段階

| 段 | 内容 | 振り分け | 検証 |
|---|---|---|---|
| 0 | 文書の修正(2-7)、コードマップに 3-1 の「未使用」 | v0 可 | 文書のみ |
| 1 | 2-4: `declared_bytes` を計測で埋める(RT メモ段 0 と同じ WP)。**RC-1 で済み(`631caac21`)** | v0 可(鍵は増えない) | planner 単体、手計算 120 / 81 MiB と一致 → 実測は 119.95 / 81.08 MiB(RT メモ §1) |
| 2 | 2-3: 固定ルートの policy 一本化(語彙 → 強度・優先順位 → Pin / Freeze の置換 → package と RPC の削除)。**強度・優先順位は RC-6a1 で済み(`3fa229eba`)。語彙は RC-6a2 で済み(`a99725b5f`)。残りは Pin / Freeze の置換と package・RPC の削除(RC-6b)。Studio の Pin / Freeze の置換は RC-6b1 で済み(`a0367bd23`)。package・RPC の削除は RC-6b2 で済み(`4e21d7d03`)。寿命を graph の外の読み手で決める形は RC-6c で済み(`be795c9a5`。2-3 の末尾)。名前の readback の約束の鍵 `retain` は RC-7a で足した(`2d1715cf3`。2-3 の末尾)。**3-2 / 3-3: 選ばれなかった候補と 1 node scope を既定で隠す | v1(物理計画 WP。RT メモの区間詰めと同じ場所) | policy → plan の golden、Frame Plan の scenario(Pin / Freeze の置換後) |
| 3 | 2-1: `execution_plan` の削除(上の表)。2-2: `levels` を契約から外す。**RC-2 で済み**(`e15e31bb5`、2026-10-23) | v1 | golden 不変、GPU gate、Studio scenario。RC-2 の実装者の報告では触った家族の非 GPU test 628 / 628 緑。GPU gate と Studio scenario は統合側で回す(この記録の時点で結果は未確認) |
| 4 | 2-6: 段階 2 を typed 論理 graph に育て、core の JSON parse を消す。2-5 と preset レシピ化に合流 | v1 | 純ロジック test、golden 不変 |

1 は Windows 無しでも planner 単体で進む。2〜4 は `get_frame_plan` を読む Studio と web が動く環境(Windows の `studio_cpu`)で確認する。

## 6. 未決(2026-10-17 更新)

解けたもの:

- **2-1 を消したあとの帯の L3(backend の名前)** → 第 2 backend を作らないので L3 は消す。barrier 行の access 注記は FramePlan の barrier(kind と resource)で足りる。
  access mask が要るなら runtime の `makeTransitionInfo`([`render_target_layout_tracker.cpp:9`](../src/core/vkcore/render_target_layout_tracker.cpp#L9))から出す。
- **2-2 の `level`** → Studio の自動配置([`nodelayout.cpp`](../src/devstudio/view/nodelayout.cpp))は自前で列を計算していて plan の `level` を読まない。契約から外して問題ない。
  (RC-2 で、imgui の Plan Viewer が `level` で列を決めていたと分かった。viewer が barrier から自分で列を出す形にした)
- **RC-2 の Logical graph の縁(順序だけの縁)** → RC-2 では付けず、Studio の Frame Plan の Logical graph の縁は plan の `barriers` だけにした。
  以前の縁は `execution_plan.dependencies` から引いていたので、資源を渡さない順序だけの縁(明示の `after` / `before` / `snapshot_after`。
  灰色の破線、ラベル `order only: <理由>`)とラベルの `same-pixel attachment` が出ていたが、execution plan と一緒に消えた。
  順序だけの縁を残すには dump に `after` / `before` を足す必要があり(今の dump の node に無い。`snapshot_after` は snapshot_copy の node にある)、
  削除の WP の範囲外として足さなかった。要るなら小さく足せる。
- **2-3 の形** → policy 一本化に発展(§2-3)。ダンプは plan + policy + 説明。web 鏡像は plan を読まないので制約なし。
- **3-2 / 3-3 を隠す** → Freeze は fragment、Unpin は pin package、`hazard_stress` は planner の profile で、どれも「選ばれなかった候補の resource 全文」を読まない。候補名と理由(`backend_selection`)は残す。

残るもの:

- **削除の順番**。2-1 の削除は Studio の Frame Plan と scenario に触るので、`develop` → `develop_v1` のマージ後に置くのは確定。
  カーネル抽出([canvas メモ](design_studio_node_canvas_kernel.md))より前か後かは、どちらも `frameplangraphics.cpp` に触るので同じ人が続けてやる前提で前に置く案。
  (2026-10-23: 削除は RC-2 として、develop を develop_v1 へ取り込んだ `5b79ac2d1` の後に済んだ。canvas メモのステータスはこの時点で「改善案メモ(WP 化・レビュー前)」のまま)
