# 技術的負債棚卸しへの敵対的レビュー（Codex）

日付: 2026-07-17

対象: `pelican2` 現行 HEAD、および OpenPBR 契約に限って sibling の `pelican-import-tools`

## 監査条件

これは実装を根拠にした静的監査である。指示どおり build、test、既存ファイルの変更は行わず、`build/` と `build-*` も参照していない。したがって「テストが通った」とは主張しない。回数や到達条件はコードから導いた値であり、動的再現結果ではない。

優先度は、発生確率 `P`、損害 `I`、修正費用 `C` を各 1（小）〜5（大）で見積もり、便宜的に `P×I÷C` を優先指数とした。指数は順序を議論する道具であって精密な確率ではない。既に完了した項目と、設計上意図された二方式を「重複だから」という理由だけで上位に置かない。

## 結論

Claude の棚卸しは、実バグ、未達の test gate、意図された設計、既に完了した作業、追跡不能な build folklore を同じ粒度で混ぜている。このまま wave 化すると順序を誤る。

- A1 は負債ではない。legacy renderer path は WP64 で既に撤去されている。先頭に R4 監査を置くのは、終わった仕事をもう一度始める計画である。
- A2 は実在するが、現行 example が自然に数分で壊れるという説明は誤りである。より重大なのは、失敗した `load_gltf` の rollback 欠如、model swap の remove-before-place、scene clear を跨ぐ stale ID である。
- A3 は最優先級で正しい。ただし「CI + D-P5」を一つの WP にすると、GPU runner の都合で CPU gate まで遅れる。段階分割すべきである。
- B1 の二重 GPU path は実在する。しかし先に直すべきは「byte-exact gate が production GPU path を一度も通していない」ことで、即時の全面統合ではない。
- B2 と C3 は「物理的に二方式ある」ことを「正本が二つある」ことと取り違えている。現行は single snapshot / single logical-frame commit に既に統一されている。
- B3 の `lifetime_token` は module DAG の欠陥を示す証拠ではなく、遅延 callback の owner guard である。generic topological teardown は問題の層を外している。
- B4 は本当に重複だが、固定 6 wrapper に汎用 variant 言語を導入する費用は便益を上回る。
- C1 は元評価より重い。reload 時に service registry は全 reset される一方、legacy `AnimationSystem` の別 registry は失効せず、anim graph evaluator は reset 後も `bound()` を返し得る。
- C2 の hard-coded count は美しくないが、単純削除は regression detection を弱める。先に GPU 不要の exact inventory gate が必要である。
- C4 は「意図的だから低優先」ではない。fresh clone の既定 player が再現不能になる設計上の結果がある。
- C5 の「COFF 破損 3 回」は repository 内の証拠では確認できず、PDB contention と混同した可能性がある。自動 retry は診断情報を消すので対策として不適切である。

## 元の負債項目の再順位

同一ラベル内で対策の費用が大きく異なるものは分けた。

| 順位 | 項目 | P | I | C | 指数 | 判定 |
|---:|---|---:|---:|---:|---:|---|
| 1 | C2a golden inventory / VAT OFF 整合 | 4 | 4 | 1 | 16.0 | 実在。小さく先行 |
| 2 | A3 CI0（Windows build + CPU tests + SKIP policy） | 5 | 5 | 3 | 8.3 | 実在。最優先級 |
| 3 | B1a production GPU 互換 gate + 共通 layout | 4 | 4 | 2 | 8.0 | gate 未達を過小評価 |
| 4 | C4 clean-clone の既定 project | 5 | 3 | 2 | 7.5 | 実在。中〜高 |
| 5 | A2 slot / handle / transaction | 4 | 5 | 4 | 5.0 | 実在。ただし原因範囲を拡張 |
| 6 | C1 skeletal reload と animation generation | 3 | 5 | 3 | 5.0 | 元評価は過小 |
| 7 | B3 exceptional teardown の queue drain | 2 | 4 | 2 | 4.0 | 局所修正なら中低 |
| 8 | C5 MSVC 失敗の計測・並列制御 | 2 | 3 | 2 | 3.0 | 事実根拠不足。retry は却下 |
| 9 | B4 wrapper contract gate の補強 | 2 | 2 | 2 | 2.0 | 実在するが封じ込め済み |
| 10 | C2b golden test harness 分割 | 3 | 2 | 3 | 2.0 | 保守性。correctness 後 |
| 11 | B1b debug text GPU path 全面統合 | 2 | 2 | 5 | 0.8 | 今すぐは過剰 |
| 12 | B2 matrix transport の一本化 | 1 | 2 | 5 | 0.4 | 非推奨 |
| 13 | C3 generic history storage | 1 | 2 | 5 | 0.4 | 一部事実誤認。非推奨 |
| 除外 | A1 legacy renderer removal | 1 | 1 | 1 | — | 完了済み |

## A 系: 構造リスク

### A1 — WP64 legacy render path: 反証成立、負債から削除

現行 renderer は frame graph が未登録なら例外を投げ、登録済みなら無条件に `executePlannedFrameGraph` を呼ぶ。legacy fallback 分岐はない（`src/core/vkcore/renderer.cpp:798-804`）。runtime record も `FramePlan` と node 列だけで、旧 `has_compute` flag を持たない（`src/core/renderingpass/framegraphruntime.hpp:27-30`）。実行 node と plan の name/kind 一致も検査される（`src/core/vkcore/renderer.cpp:589-594`）。golden gate は実行順と plan 順を比較する（`test/golden_image_test.cpp:4102-4105`）。

撤去報告も既に存在し、削除と残存参照 0 件を記録している（`docs/design_reviews/2026-07-10_wp64_report.md:11-16,118`）。履歴上も A/B trace gate の `c207fec` と撤去の `fa33a0e` があり、後者は現 branch に含まれる。

判定は「Claude の事実誤認」である。WP34/35 に暗黙吸収されたのではなく、独立 WP64 として完了している。必要なのは CI 上の既存 gate 維持だけで、R4 監査を debt wave の先頭に置く理由はない。

### A2 — instance slot 枯渇: 実在するが、再現説明と修正範囲が不十分

`maxModelInstances` と `maxRenderCommands` はともに 1024（`src/core/renderer/polygoninstancecontainer.cpp:22-23`）。`placeModelInstance()` は `model_instances_data.size()` を上限判定に使い、常に末尾へ append する（`src/core/renderer/polygoninstancecontainer.cpp:260-275`）。`removeModelInstance()` は draw command と各配列の内容を tombstone 化するだけで vector を縮めず、free-list にも戻さない（`src/core/renderer/polygoninstancecontainer.cpp:326-363`）。ECS entity の破棄はこの remove に到達する（`src/core/ecs/predefined/modelview.cpp:8-12`）。したがって dynamic model entity の churn が続けば、同時 live 数が少なくても最終的な枯渇確率は 1 である。

ただし「現行 demo の反復で間もなく再現する」は過大評価である。example scene の `simplemodelview` は 31 件（`projects/example/scenes/main.scene.json:55-865`）で、game code が guard 付きで一体だけ追加する（`projects/example/code/playercontrol.cpp:96-110,125-128`）。定常 high-water `S=32` から一体ずつ remove/recreate する仮想 workload なら、992 回は成功し 993 回目で失敗する。一般式は、1 cycle に `k` 体を再生成するとき `floor((1024-S)/k)` cycle が完走し、その次の途中で失敗、`k=1` の最初の失敗は `1025-S` 回目である。現行 example 自身には反復 despawn がないため、未変更の demo を放置してもこの理由では枯渇しない。

scene load は `clearRuntimeScene()` を通り（`src/core/loader/scene.cpp:215-238`）、instance container も clear する（`src/core/loader/scene.cpp:374-380`）。model hot reload も既存 instance の in-place rebuild である（`src/core/asset/model.cpp:330-333`）。ゆえに scene 往復や通常 hot reload を A2 の再現根拠にしてはいけない。

一方、実害は元の説明より広い。

1. `SimpleModelViewUpdateSystem` は旧 instance を remove してから新規 place する（`src/core/ecs/predefined/modelviewupdatesystem.cpp:31-40`）。容量失敗時には旧描画が消え、component は tombstone ID と `dirty=true` を保持する。容量拒否だけでなく strong exception guarantee が壊れている。
2. `loadTransientGltf()` は slot と ECS entity を作った後で name を bind する（`src/core/loader/scene.cpp:456-485`）。重複名は throw する（`src/core/loader/scene.cpp:388-394`）のに bind は rollback 用 `try` の外である。RPC はこの関数を直接呼ぶ（`src/core/communication/rpcserver.cpp:846-850`）。失敗応答でも live entity、slot、render command が残り、呼出側には削除用 ID がない。
3. `ModelInstanceId` は裸の `uint32_t` である（`src/core/renderer/modelinstance.hpp:7-9`）。`clear()` は generation 相当の配列まで捨てる（`src/core/renderer/polygoninstancecontainer.cpp:365-375`）。一方 `SeqPlayer` は ID を保持して毎 frame `setTrs()` する（`src/core/playback/seqplayer.cpp:326-346`）。scene load は seq update の直後に適用され（`src/core/appflow/framephase.cpp:132-133`）、翌 frame に新 scene の同じ index を誤操作するか range error になり得る。`setTrs()` は index range しか見ない（`src/core/renderer/polygoninstancecontainer.cpp:1136-1142`）。

したがって free-list だけでは危険が増える。再利用した slot を stale ID が正当な新 occupant として操作するからである。対策は次の単位に分けるべきである。

- 先行 WP: transient load の name preflight と全工程 rollback、model swap の stage-then-commit。
- 本体 WP: `{index, generation, alive, scene_epoch}` を検査する SlotMap handle。`clear()` 後も旧 handle を stale にする。
- ownership WP: scene-owned、playback-owned、transient-owned instance の寿命を明示し、scene load 時に `SeqPlayer` を再生成または無効化する。
- gate: 1 万回 churn、double remove、free/clear 後 stale access、重複 RPC rollback、容量失敗時の旧 model 維持、generation/epoch wrap 方針。

active draw command の同時上限 1024 は別契約であり、slot recycling と一緒に消してはいけない。`firstInstance=id.value` なので slot 再利用自体は GPU layout と両立する（`src/core/renderer/polygoninstancecontainer.cpp:303-310`）。

### A3 — CI 不在: 正しいが、巨大 WP にしてはいけない

tracked workflow は存在せず、roadmap も local gate のみと明記する（`docs/roadmap_backlog_2026-07.md:80-83`）。merge 手順は full build、全 CTest、golden SKIP の目視、player 8 秒を人手に要求し、過去には実エラーが SKIP 扱いで通ったことまで記録している（`docs/agent_operations.md:86-96`）。テストは CMake/CTest に登録される（`CMakeLists.txt:448-451`, `test/CMakeLists.txt:11-25`）が、golden は Vulkan device がなければ成功扱いの `SKIP` になる（`test/golden_image_test.cpp:2932-2945`）。OFF smoke と project-code smoke は CTest 外である（`test/run_build_units_smoke.cmake:1-8`, `test/run_project_code_smoke.cmake:1-9`）。

よって A3 は最優先級である。ただし提案の「CI + D-P5」は分割する。

- CI0: Windows/MSVC configure + build、GPU 不要 test、許可済み SKIP の exact policy、失敗 artifact。自動 retry は入れない。
- CI1: feature OFF matrix、build-unit/project-code smoke、clean-clone の self-contained project。
- CI2: Vulkan が保証された runner で golden、validation layer、D-P5。GPU 不在を SKIP ではなく runner misconfiguration として fail させる。

GPU runner が未準備でも CI0 を止める理由はない。逆に `ctest` を一行置くだけでは golden が SKIP して偽緑になる。

## B 系: rendering architecture

### B1 — debug text / UI quad: 二重 path は実在、しかし先に gate を直す

`DebugText` は専用 image、sampler、descriptor pool、SSBO、pipeline を持ち（`src/core/renderer/debugtext.hpp:41-55`, `src/core/renderer/debugtext.cpp:134-172`）、glyph ごとに 6 頂点を作って non-indexed draw する（`src/core/renderer/debugtext.cpp:226-268,336-368`）。UI は 20-byte `QuadVertex`、4 頂点 + 6 index、run/scissor/page batching という別 ABI である（`src/core/ui/drawcommands.hpp:18-68`, `src/core/ui/drawcommands.cpp:31-47`, `src/core/renderer/uirenderer.cpp:79-121`）。atlas も両 feature 有効時には別 upload される（`src/core/renderer/debugtext.cpp:140-150`, `src/core/ui/module.cpp:147-154`, `src/core/renderer/uicontainer.cpp:10-21`）。

共有済みなのは font table で、layout ではない。UI は `BitmapFont::layout()` を使う（`src/core/ui/bitmapfont.cpp:87-121`, `src/core/ui/module.cpp:105-121`）が、DebugText は独自 cursor loop を持つ（`src/core/renderer/debugtext.cpp:294-328`）。共通 layout は `int64_t` で中間計算して int32 overflow を拒否する一方、DebugText は `int` を直接加算する（`src/core/ui/bitmapfont.cpp:91-120`, `src/core/renderer/debugtext.cpp:301-327`）。これは既に具体的な safety drift である。

しかし lifecycle の分離には意図もある。`GameContext::debugText()` は debug feature 未登録時 no-op で（`src/core/userpublic/gamecontext.cpp:289-291`）、その契約は test され、UI feature なしでも使える（`test/debugtext_test.cpp:22-40`, `src/core/vkcore/renderer.cpp:133-169`）。また現在の blend factor は両 path で一致し、古い blend 差の指摘は stale である（`src/core/renderer/debugtext.cpp:277-287`, `src/core/renderer/uirenderer.cpp:19-34`）。

WP93 の計画は旧 debug path と UI common path を同じ fixture で描いて byte-exact 比較するとしている（`docs/implementation_plan.md:2162-2164`）。実テストは production の `DebugText` も `UiRenderer` も呼ばず、test 内で再実装した legacy glyph placement と `BitmapFont::layout()` の CPU atlas copy を比較するだけである（`test/ui_foundation_test.cpp:320-364`）。shader、blend、clip/scissor、scale、float color と UNORM8 color、実 rasterization が壊れても通る。これは「gate 済み」ではない。

順序は次である。

1. 同一 Vulkan target へ両 production path を描く framebuffer A/B gate を追加する。fallback glyph、CR/LF/tab、scale、四辺 clip、8-bit に正確に落ちない色を含める。
2. DebugText の cursor loop を `BitmapFont::layout()` に寄せる。
3. その後に GPU owner の共有を判断する。`UiRenderer` は自分で dynamic rendering を begin/end するのに対し、DebugText は executor が開始した scope 内で draw する（`src/core/vkcore/render_pass_executor.cpp:20-49`, `src/core/renderer/uirenderer.cpp:90-121`）。単純な class 置換ではない。

`GameContext` を `UiModule` に直結させる全面統合は現時点では過剰である。共有するなら atlas owner と「既存 render scope 内で quad を描く」下位層までに留める。

### B2 / Q2 — matrix 配送二重系: 一本化しない

二つの GPU transport はあるが、二つの正本はない。frame ごとの正本は `RenderFrameSnapshot` であり、同じ jittered snapshot から FrameUBO と material push constant を作る（`src/core/vkcore/renderer.cpp:187-214,764-794,1169-1191`）。material は `snapshot.view_projection_jittered` を `engineMvp` に push する（`src/core/renderer/materialrender.cpp:52-67`）。velocity は current/previous projection、view、object が必要なので FrameUBO/SSBO を読む（`src/core/resources/velocity.vert:16-22`, `src/core/resources/velocity_skinned.vert:23-28`）。sprite と SSAO も FrameUBO、shadow は light VP push、UI は resolution-only である（`src/core/resources/sprite.vert:33-55`, `src/core/resources/ssao.frag:50-72`, `src/core/renderer/materialrender.cpp:87-99`, `src/core/resources/ui.vert:12-17`）。WP112 の設計も「FrameUBO だけ」という旧案を撤回してこの transport fan-out を規範化した（`docs/design_taa_jitter.md:133-163`）。

全てを FrameUBO に移すと、offset 0..63 を engine MVP 領域とする公開 shader contract と reflection を壊す（`docs/shader_contract.md:57-70`, `src/core/shader/shaderreflection.cpp:207-225`）。標準 shader だけでなく raw/custom shader ABI、cache、hot reload、golden、XR を巻き込む。根拠のない大移行である。

具体的な負債は別にある。`engineMvp` の実体は MVP ではなく jittered VP で、shader は既に model transform 済みの world position に掛ける（`src/core/renderer/materialrender.cpp:58-61`, `src/core/resources/default.vert:29-32`, `src/core/resources/shaders/material/surface_v1.vert:58-60`）。また consumer gate は固定 `size()==9` と substring 検査なので、新しい 10 番目の consumer の追加漏れを検出できない（`test/projectionjitter_test.cpp:211-219`）。

Q2 への回答は「物理 transport は二重のまま、意味上の正本と exhaustive consumer inventory を統一する」である。現 ABI では rename せず `view_projection` であることを文書化し、次 ABI で alias/rename する。draw ごとの 64 byte push が本当に問題かは profiler で測ってから決める。

### B3 / Q3 — module teardown: generic DAG は問題を解かない

`lifetime_token` は確かに二箇所だけである（`src/core/model/vertbufcontainer.hpp:68-70`, `src/core/material/materialcontainer.hpp:107-110`）。ただし用途は一般 module dependency ではなく、`DeletionQueue` に積まれた callback が破棄済み owner を触らないための weak guard である（`src/core/model/vertbufcontainer.cpp:414-439`, `src/core/material/materialcontainer.cpp:608-619`）。

module container が記録する edge は constructor 実行中の `GET_MODULE` に限られる（`src/core/container.hpp:83-96,149-196`）。destructor は生成逆順であり（`src/core/container.hpp:258-280`）、constructor dependency については依存先が先に生成されるので既に dependent-first になる。runtime に enqueue された callback と owner の関係は graph に入らないため、この不完全 graph を topological sort しても B3 は直らない。それどころか「graph が完全」という誤った安全感を作る。

正常 loop 終了には wait-idle 後の queue flush がある（`src/core/appflow/loop.cpp:265-273`）。queue destructor も wait + flush safety net を持つ（`src/core/vkcore/deletionqueue.cpp:21-43`）。一方、例外 path の明示 teardown は wait-idle、physics、ECS、instance clear までで、queue drain を明示しない（`src/core/appflow/teardown.cpp:31-42`）。ここが先に検証すべき具体的な穴である。

Q3 への回答は「generic topological module teardown への変更リスクは高く、現状の根拠には見合わない」である。先に例外 teardown でも ECS/instance clear 後に queue を drain し、owner callback を shared state に閉じ込め、queue と owner の二つの生成順を test する。token は gate が揃うまで残す。

### B4 — `.surface` variant: 重複は真、言語拡張は過剰

parser の既知 top-level は `language/params/textures/screen_inputs/render_state` で、variant/inheritance はない（`src/project/surfaceformat.cpp:849-888`）。OpenPBR の 6 wrapper は各 55 行で、宣言部と hook が重複し、主な差は render state と二つの define である（`src/core/resources/surfaces/openpbr/opaque_single.surface:4-55`, `src/core/resources/surfaces/openpbr/blend_double.surface:42-55`）。

しかしこれは build generator より薄い wrapper を選んだ明示的 trade-off である（`docs/design_usd_openpbr.md:64-77`）。全 6 件の AST/lowering/runtime-compiler gate もある（`test/surfaceformat_test.cpp:178-233`, `test/surfacecompiler_test.cpp:172-238`）。現在の比較 helper が `hint` を見ず、`screen_inputs` と wrapper code 全体の同値も保証しない穴はある（`src/project/surfaceformat.hpp:35-45`, `test/surfaceformat_test.cpp:43-68`）。

対策は汎用文法ではなく、canonical wrapper を正規化して variant-specific 行以外の完全一致を検査し、`hint`、`screen_inputs`、hook を含めることである。二つ目の surface family が同じ複製を始めるか variant 数が増えた時点で、checked-in output + `--check` generator を検討すればよい。resolver、diagnostics、hot reload、cache identity、配布形式まで増やす汎用 variant 言語を固定 6 file のために導入しない。

## C 系: 保守性と再現性

### C1 — animation raw pointer registry: 元評価より重大

service 側の `ObjectRecord` は `SkeletalModelData*` と `AnimationAsset*` を保持する（`src/core/animation/animationservice.cpp:57-63`）。`AnimationAssetRegistry` も `SkeletalModelData*` を key にし、asset/clip が source raw pointer を持つ（`src/core/animation/animationjobs.hpp:42-64`）。model reload は skeletal が関係すれば process-global service を全 reset し（`src/core/asset/model.cpp:336-349`）、object、rig、clip、sink、source、phase、owner を消す（`src/core/animation/animationservice.cpp:249-271`）。これは dangling 防止ではあるが、無関係な animation まで落とす粗い invalidation である。

さらに legacy `AnimationSystem` は別個の `AnimationAssetRegistry` を所有し（`src/core/ecs/predefined/animationsystem.cpp:19-25`）、毎 frame `getOrCreate()` を使う（`src/core/ecs/predefined/animationsystem.cpp:68-70`）。reload 時にこの registry を clear する経路が見当たらない。旧 pointer entry が増え、allocator が過去の address を再利用すれば旧 asset を返し得る。

anim graph 側も、`EvaluatorV1::bound()` は内部 bool だけを返し service の registration generation を確認しない（`src/core/userpublic/animation/animgraph.cpp:758-772`）。demo は `bound()==false` のときだけ bind する（`projects/animgraph_demo/code/animgraphdemo.cpp:31-53`, `projects/vrm_xr_demo/code/vrmxrdemo.cpp:169-201`）。service 全 reset 後も evaluator が bound のまま、消えた phase に再登録せず animation が黙って止まる可能性がある。

短期 WP は legacy registry の reload invalidation と、service generation 変更を検知する evaluator rebind である。同一 skeletal model を最低 3 回 reloadし、legacy component と anim graph が継続し、別 model が止まらない gate を置く。本命は `ModelAssetId + content/compatibility generation` key と asset 単位 invalidation であり、全 registry reset を恒久策にしない。

### C2 / Q4 — hard-coded golden count と巨大 file

現在の count は VAT ON 49 / OFF 48（`test/golden_image_test.cpp:3521-3529`）。`golden_image_test.cpp` は 4199 行、`implementation_plan.md` は 3646 行であり、元の約 4174 行という値は既に古い（`test/golden_image_test.cpp:4199`, `docs/implementation_plan.md:3646`）。肥大の指摘自体は正しい。

ただし count の単純削除には反対する。通常 mode の RGBA hash gate は discovered name を manifest の `.at(name)` で引き、最後に件数も一致させるため、追加、rename、削除は count がなくても検出する（`test/golden_image_test.cpp:4055-4069`）。しかし fixture-update mode は captured set で manifest を書き直す（`test/golden_image_test.cpp:4050-4068`）ので、case directory の誤削除と manifest の縮小が同時に起き得る。現時点の hard-coded count はその事故に対する独立した防波堤である。

しかも count test より前に Vulkan probe がある（`test/golden_image_test.cpp:3521-3524`）ため、GPU のない CI では inventory 自体が SKIP される。VAT OFF では discovery が `vat_playback` を除く（`test/golden_image_test.cpp:152-155`）のに、hash/trace manifest は VAT entry を保持しており（`test/fixtures/wp73_rgba8_hashes.json:43-50`, `test/fixtures/renderer_execution_traces.json:13089`）、末尾の exact size 比較（`test/golden_image_test.cpp:4069,4118`）とは静的に不整合である。

Q4 への回答は次である。

- 今日 count を消すと、特に update mode で silent loss 検出力が下がる。
- 先に GPU 不要の `golden inventory` test を作り、case name 集合、hash manifest key、trace 対象、`case.json` / `expected.png` / `tolerance.json` の存在、feature-gated inclusion を exact set 比較する。
- update command は inventory membership を変更せず、membership 変更は別 manifest の明示 diff を要求する。
- VAT OFF の filtering を同時に直した後で hard-coded 数値を削除する。

4199 行の test は fixture builder、render harness、XR/TAA/velocity、hash/trace updater を helper library と分野別 target に分ける価値がある。一方 `implementation_plan.md` の R6 は active ledger と completed archive の分離で十分で、correctness WP より後でよい。

### C3 — history buffer 非統一: 一部事実誤認

model matrix、skin palette、morph、material override の current/previous は既に同じ `PolygonInstanceContainer` に集約される（`src/core/renderer/polygoninstancecontainer.hpp:201-239`）。advance/reset も `advanceTemporalHistoryAfterRender()` を境界にまとめている（`src/core/renderer/polygoninstancecontainer.cpp:509-564`）。logical frame 終端では RT history、instance history、camera snapshots を連続して一度だけ commit する（`src/core/vkcore/renderer.cpp:1225-1229`）。stereo gate も history flip と instance advance が logical frame ごとに一回であることを検査する（`test/golden_image_test.cpp:3672-3686,3708-3723`）。coordination は既に統一済みである。

RT history は二つの image surface の flip と clear で（`src/core/renderingpass/rendertargetcontainer.cpp:202-219`）、CPU vector + GPU buffer copy の object/palette history とは寿命と hazard が異なる。「同じ storage abstraction に入れる」こと自体が目的化している。また棚卸しが想定する GPU particle 二面化は現 source に存在せず、将来課題に留まる（`docs/design_postprocess_temporal.md:116-119`）。

generic history framework WP は作らない。第三の history participant が実装され、reset/advance 漏れが実害化した時点で participant interface を検討する。今は current orchestration の gate を維持する方が安い。

### C4 — example assets untracked: 意図的だが再現性は壊れている

`.gitignore` は example の GLB/VRM/PNG を明示的に除外する（`.gitignore:8-12`）。README も非追跡を意図した設計として説明する（`projects/example/README.md:14-32`）。しかし player は project 指定がなければ近傍の `projects/example` を暗黙選択する（`src/player/main.cpp:165-188`）。manifest は path、size、hash を持つだけで取得 URL や provenance を持たない（`projects/example/assets.manifest.json:1-46`）。fresh clone の既定起動と agent gate は再現不能であり、発生確率は 100% である。

解決は 80MB 超の binary を無批判に git へ入れることではない。再配布可能な小さい self-contained smoke project を既定にし、heavy showcase は LFS/DAM/fetch recipe、license、provenance を持つ別導線にする。manifest の verify は供給済み file の同一性を証明するが、入手可能性は証明しない。

### C5 — MSVC COFF corruption: 根拠不足、自動 retry は反対

repository で確認できるのは MSVC 全体に `/MP` と `/FS` が設定されていること（`CMakeLists.txt:9-13`）、および `/FS` の理由が同一 PDB 書込み安定化と記録されていること（`docs/design_reviews/2026-07-11_wp74_report.md:125-134`）である。「COFF corruption が 3 回」の error code、対象 `.obj`、generator、parallelism、log は tracked evidence にない。PDB contention と COFF object corruption を混同している可能性を排除できない。

無条件 retry で二回目が通れば green にする案は却下する。初回 failure を消し、再現率を下げ、artifact を残さないからである。まず error code、failure target、command line、generator、`/MP` と `cmake --build --parallel` の組合せ、対象 obj/PDB を artifact 化する。再現するなら二重並列を制限し、shared build tree を同時に触る target を serialize する。retry を置くとしても、新しい clean build directory で一回だけ、初回 failure を flaky failure として赤または quarantine 扱いにして可視化する。

## 元の棚卸しが見落とした負債

以下は A〜C の言い換えではなく、現行 code path に根拠がある追加項目である。

### N1 — core renderer が magic name の light を毎 frame 改変する

renderer は毎 frame `LightContainer::updateAnimation()` を無条件に呼ぶ（`src/core/vkcore/renderer.cpp:182-185,1125`）。その関数は `KeyLight`、`FillLight`、`PointLight1`、`SpotLight1` という名前だけを条件に、方向、強度、位置を time 依存で書き換える（`src/core/light/lightcontainer.cpp:200-247`）。これは example 演出が engine core に漏れたもので、別 project が普通の名前として同名 light を author すると、schema 上の opt-in なしに値が変わる。

発生確率 5、損害 4、費用 1 の即時修正候補である。core から削除し、example game code または明示的 animation component に移す。

### N2 — light 上限超過を silent truncate し、shadow frustum も world 固定

上限は directional 8、point 16、spot 8（`src/core/light/light.hpp:8-10`）。loader は個数上限を検査せず全件を vector に積み（`src/core/light/lightcontainer.cpp:104-163`）、GPU update 時に `min()` で黙って切り捨てる（`src/core/light/lightcontainer.cpp:249-278`）。author はどの light が捨てられたか分からない。さらに directional shadow は world origin 中心、eye distance 10、ortho ±6、near/far 0.1/30 に固定される（`src/core/light/lightcontainer.cpp:77-88`）。camera や caster bounds と無関係なので、scene scale が変わるだけで shadow が消える。

短期は load 時の hard error または dropped-name warning と schema limit。shadow は camera/caster bounds fitting を別 WP にする。

### N3 / Q5 — ECS scheduler が既に収集した read/write hazard を無視する

system 登録時に component の read/write index を収集する（`src/core/userpublic/details/ecs/coretemplate.hpp:173-203`）。しかし execution level は明示 `depends_list` だけで作り（`src/core/userpublic/details/ecs/coretemplate.cpp:386-417`）、同じ level の system を並列 schedule する（`src/core/userpublic/details/ecs/coretemplate.cpp:421-435`）。read/write conflict の ordering も assertion もない。predefined system の順序も手書き edge に依存する（`src/core/ecs/predefined.cpp:30-44`）。

新 system が一つ edge を忘れるだけで同じ component への write/write または read/write data race、非決定描画、稀な corruption を作れる。Q5 の「最大の構造リスク」はこれである。resource access metadata が既にあるため、まず graph が hazard を全て順序付けているか fail-fast 検査し、次に必要なら conflict から automatic serialization edge を生成する。意図的並列 read/read は維持する。

### N4 — ECS system unregister が dependent を永久停止させ、raw owner lifetime も無契約

system wrapper は raw `void* system_ref` と dependency ID を保持する（`src/core/userpublic/details/ecs/coretemplate.hpp:138-155`）。`unregisterSystem()` は対象が依存していた側の `depended_by` から自分を外すだけで、対象に依存している consumer を拒否も更新もせず erase する（`src/core/userpublic/details/ecs/coretemplate.cpp:379-384`）。consumer の `depends_list.size()` は残るが predecessor は存在しないため、topological pass で indegree が 0 にならず、その system はエラーなしに実行されなくなる（`src/core/userpublic/details/ecs/coretemplate.cpp:391-417`）。

RAII registration token と owner generation を導入し、dependent がある unregister は明示拒否または明示 cascade にする。update 後に未実行 node があれば missing dependency/cycle として fail する。

### N5 — ECS component ID の collision/capacity を登録時に拒否しない

`DECLARE_COMPONENT` は任意の手書き ID を受け取り、mask は 64 bit である（`src/core/userpublic/details/ecs/componentdeclare.hpp:10-23`）。registry は ID 位置を resize/overwrite し、name map も `insert_or_assign` するだけで duplicate ID/name や `>=64` を拒否しない（`src/core/ecs/componentinfo.cpp:9-18`）。system 登録は component index を検査せず `1ULL << idx` する（`src/core/userpublic/details/ecs/coretemplate.hpp:178-188`）。ID 64 以上なら entity/chunk の後段検査より前に undefined shift に到達し得る。

中央 registry で `id < MAX_COMPONENTS`、duplicate ID/name、型/lifecycle metadata の不一致を拒否する。game DLL ABI を跨ぐなら generated registry または namespaced allocation を検討する。

### N6 — camera の projection 選択と pose 選択が別の「最初」を使う

`Camera::loadSceneCameras()` は scene JSON の最初の camera から projection/sprite policy を選ぶ（`src/core/renderer/camera.cpp:505-540`）。一方 `CameraSystem::process()` は渡された chunk の `count` に関係なく index 0 だけを camera pose に書く（`src/core/ecs/predefined/camerasystem.cpp:13-23`）。複数 archetype chunk があれば各 chunk の最初が順次書き、最後の chunk が勝ち得る。explicit `setActiveCamera()` 後でなければ pose write を lock しない（`src/core/renderer/camera.cpp:552-563,609-618`）。

複数 camera や archetype 差で、projection は JSON の一体、pose は別 entity という split-brain が起こる。active camera entity/name を唯一の正本にし、projection と pose を同時適用する。複数 active camera を error にする gate が必要である。

### N7 — Atlas descriptor pool を最大値で一括確保する

`AtlasAssetResource` は constructor で combined-image-sampler descriptor と `maxSets` を `uint16_max * 2 = 131070` 個まとめて確保する（`src/core/renderer/atlasassetresource.cpp:105-115`）。実際は page ごとに nearest/linear の 2 set ずつしか増えない（`src/core/renderer/atlasassetresource.cpp:149-168`）。driver 内部 memory と descriptor limit に不必要な圧力をかけ、低資源 device では UI/sprite 初期化の portability risk になる。

小さい pool から始め、必要時に追加 pool を作る。B1 で debug atlas をこの owner に寄せるなら先に直す。

### N8 — OpenPBR variant 契約が engine と importer で split-brain

engine 側には 6 variant の manifest がある（`src/core/resources/surfaces/openpbr/manifest.json:10-17`）。importer はそれを読まず、`alpha_mode + side` から path を文字列合成し、pin define も独自に複製する（`../pelican-import-tools/src/pelican_import_tools/usd.py:1737-1750`）。importer test は `opaque_double` 一例だけを固定する（`../pelican-import-tools/tests/test_usd.py:264-271`）。engine 側の rename/additionが importer 成功・runtime load 失敗として現れ得る。

manifest を cross-repository fixture の正本にし、importer 出力の 6 組と exact set 比較する。これは汎用 `.surface` variant より低コストで、実際の境界を守る。

### N9 — importer subprocess が Windows ANSI path と無限待機を使う

Windows path は `CreateProcessA` と `STARTUPINFOA` を使い（`src/devcli/rulesimport.cpp:417-435`）、終了待ちは `WaitForSingleObject(..., INFINITE)` である（`src/devcli/rulesimport.cpp:436-444`）。日本語を含む project/tool path が code page で表現できなければ起動に失敗または文字化けし、importer が hang すれば CLI も永久停止する。POSIX 側も `std::system()` で cancellation がない（`src/devcli/rulesimport.cpp:445-459`）。

Windows は `CreateProcessW` + UTF-16、全 platform で timeout/cancel、process-tree termination、stdout/stderr/version capture を実装する。

### N10 — compute config が未実装語彙を受理して fail-open する

buffer `lifetime: transient` は parse され `persistent=false` になる（`src/core/renderingpass/computetask.cpp:215-233`）が、allocation は `definition.persistent` を参照せず同じ長寿命 map に入れる（`src/core/renderingpass/computetask.cpp:287-305`）。dispatch の `groups_from` と `local_size` も parse される（`src/core/renderingpass/computetask.cpp:185-190`）一方、task record と実 dispatch は `groups_x/y/z` だけを使う（`src/core/renderingpass/computetask.cpp:410-429,486-498`）。author が機能していると誤解する schema である。

実装するまでこれらの非 default 指定を hard error にするか、明示的に未対応と schema/version で拒否する。受理して無視するのが最悪である。

## 五つの質問への直接回答

| 質問 | 回答 |
|---|---|
| Q1: A2 の正確な影響と回数 | high-water `S`、cycle 当たり再生成 `k` なら `floor((1024-S)/k)` cycle 完走後に失敗。example は `S=32`, `k=1` の仮想 churn で 992 回成功、993 回目失敗。ただし現行 demo にその loop はない。実害の強い到達路は dynamic entity、失敗 `load_gltf`、remove-before-place、scene/SeqPlayer 境界である。 |
| Q2: matrix delivery を統一する価値 | ない。single `RenderFrameSnapshot` から用途別 transport に fan-out しており、意味上は統一済み。ABI を壊す一本化より、consumer inventory と `engineMvp` の命名/契約を直す。 |
| Q3: topological module teardown のリスク | 高い。graph は constructor dependency しか知らず、問題の deferred callback edge を持たない。generic DAG 化は根治にならない。明示 queue drain、shared callback state、両生成順 test を先行する。 |
| Q4: golden count REQUIRE を消すと silent loss 検出は落ちるか | 今日消せば update mode で落ちる。通常 mode では hash/trace key set が大半を検出する。GPU 不要の immutable inventory exact-set gate と VAT 条件を作った後なら削除できる。 |
| Q5: 最大の構造リスク | ECS scheduler が read/write metadata を収集しながら dependency validation に使わず、同 level を並列実行すること。edge 一つの欠落が data race と非決定性になる。 |

## 提案されていた wave への反対意見

「R4 audit → instance generations → CI + D-P5 → debug text / teardown / R6」という並びには四つの問題がある。

1. R4 は既に完了しており、critical path の先頭に置く根拠がない。
2. A2 を free-list/generation だけで閉じると RPC rollback、remove-before-place、scene epoch、SeqPlayer ownership を残し、stale handle の被害だけを増やす。
3. CPU CI と GPU validation を一括すると、runner 調達が全 gate を止める。CI0/CI1/CI2 に分けるべきである。
4. debug text 全面統合、generic module DAG、generic history、surface variant language は、production gate と局所 correctness を直す前の framework work である。

また R6 は repository health には効くが、実行時 corruption、silent animation freeze、fresh-clone failure を先送りしてまで行う作業ではない。docs archive は最後でよい。

## 合意できる負債ウェーブの推奨順（WP 粒度）

1. **WP-CI0 — 必須 CPU gate**: Windows/MSVC configure/build、GPU 不要 CTest、SKIP exact allowlist、artifact 保存。retry なし。
2. **WP-GOLDEN0 — GPU 不要 inventory**: case/file/hash/trace の exact set、VAT ON/OFF 条件、update mode membership lock。完了後に count REQUIRE を削除。
3. **WP-LIGHT0 — authoring boundary**: magic-name light animation を core から example/component へ移し、light cap 超過を fail/warn。shadow fitting は別 acceptance criteria に切り出す。
4. **WP-TRANSIENT0 — strong transaction**: `load_gltf` name preflight/rollback、model swap の stage-commit、失敗時に entity/slot/draw command が不変である gate。
5. **WP-INSTANCE0 — SlotMap handle**: free-list + generation + alive + scene epoch、1 万回 churn、stale/double-remove/clear/capacity gate。scene/playback/transient ownership も明文化。
6. **WP-ANIM0 — reload generation**: legacy registry invalidation、Evaluator rebind、asset 単位 generation、複数回 reload 継続 gate。
7. **WP-ECS0 — scheduler safety**: read/write hazard と declared DAG の照合、missing dependency/cycle/unexecuted-node fail-fast、必要な automatic serialization。
8. **WP-ECS1 — registration lifetime/ABI**: unregister token、dependent policy、raw owner generation、component ID `<64` と duplicate rejection。
9. **WP-CI1 — 構成・clean-clone matrix**: feature OFF、build-unit/project-code smoke、self-contained default project。heavy example は fetch/LFS + license/provenance へ分離。
10. **WP-CI2 — GPU gate / D-P5**: Vulkan 保証 runner、golden no-SKIP、validation layer、repeat/artifact。MSVC failure は計測し、retry で隠さない。
11. **WP-DTXT0 — 本物の互換 gate**: production DebugText/UI framebuffer A/B と共通 `BitmapFont::layout()`。GPU path 全面統合はこの結果と profiler を見て別決定。
12. **WP-LIFETIME0 — exceptional teardown**: explicit queue drain、owner callback shared state、module 生成順を変えた test。generic topological container は導入しない。
13. **WP-PORT0 — portability quick fixes**: growable atlas descriptor pool、`CreateProcessW`、import timeout/cancel/log capture。
14. **WP-CONTRACT0 — 境界 gate**: OpenPBR engine/importer 6 variant exact-set、projection consumer inventory、`engineMvp` の意味を次 ABI へ移す準備。
15. **WP-TEST0 — 保守性**: golden harness の helper/target 分割、active implementation ledger と completed archive の分離。

現時点で WP 化しないものは、A1/R4 再監査、matrix transport 一本化、generic history storage、generic module topological teardown、汎用 `.surface` variant 言語、debug text GPU path の無条件全面統合である。
