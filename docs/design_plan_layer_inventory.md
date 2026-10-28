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
| 2-3 | 物理計画の固定ルート(pin / freeze / complete) | **方針決定(2026-10-22): 固定のルートを `target_planning` の policy に一本化する**。細部は後で詰める | 同じ | 語彙、`require` / `prefer`、優先順位(§2-3)。**RC-6a1 で強さ・出所・優先(root > レシピ > 部品)と部品の宣言が入った**(`3fa229eba`、2026-10-28。§2-3 の [RC-6a1](#rc6a1-implementation))。**RC-6a2 で語彙 5 つ(`backend` / `representation` / `alias_group` / `scope_break` / `scope_join`)、候補ごとの反映、最終の検証が入った**(`a99725b5f`、2026-10-29。[RC-6a2](#rc6a2-implementation)) |
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
| 2 | 2-3: 固定ルートの policy 一本化(語彙 → 強度・優先順位 → Pin / Freeze の置換 → package と RPC の削除)。**強度・優先順位は RC-6a1 で済み(`3fa229eba`)。語彙は RC-6a2 で済み(`a99725b5f`)。残りは Pin / Freeze の置換と package・RPC の削除(RC-6b)。**3-2 / 3-3: 選ばれなかった候補と 1 node scope を既定で隠す | v1(物理計画 WP。RT メモの区間詰めと同じ場所) | policy → plan の golden、Frame Plan の scenario(Pin / Freeze の置換後) |
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
