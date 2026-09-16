# pelican の思想と現在地(2026-09-15)

利用者の製品思想を、類似ソフトウェアの系譜を探すための言葉にまとめ、続けて現在の実装とツール仕様を記す。
仕様の正本は台帳 `implementation_plan.md` と `design_*.md`、入口は `v1_development_plan.md`。本書は説明であり裁定ではない。

---

## 第 1 部 思想

### 1.1 一文で

**同じ scene と資産を、差し替え可能な描画方式(リアルタイムのラスタ、ハイブリッド RT、オフラインの RT)で、同じノードモデルのまま出せる、改造可能なグラフィックシステムかつゲームエンジン。**
3D の映像編集まで同じ土台で行い、2D はパージ可能な副機能群として載せる。

### 1.2 出発点になった不満

| 既存の分類 | 得意 | 不満 |
|---|---|---|
| 3DCG ソフト(オフライン RT) | 画質、レイトレの完成度 | 描画方式が固定で組み替えられない。軽量なリアルタイム描画へ切り替えられない |
| ゲームエンジン | 組み替え、リアルタイム | ラスタ前提でプリレンダに向かない。内部が一体化していて分解して学べない |
| 映像ソフト | 編集 | エージェント(自動化・AI)から操作しきれない。ワンタイムライン方式が窮屈 |

### 1.3 原則(設計文書と台帳に散らばっているものを束ねる)

1. **一つのグラフモデル**: 描画パイプライン、数値、音、属性、姿勢を同じ node / port / edge で記述する。編集の正本は文書(JSON)で、GUI は view。同じ接続に二つの正本を残さない。
2. **描画方式はモジュール**: 上のグラフと下のデータ(scene・資産・形式)は変わらず、描画方式だけが差し替わる。
3. **ノードへの分解**: 標準機能(影、ポストプロセス、AS 構築 …)は最適化した一体版と、編集できる分解版の両方を持ち、同じ設定を表現する。分解して学べることが目的の一つ。
4. **パージ可能**: 機能を外したとき、孤児のパスや資源やコードが残らない。build option と test で強制する。根幹は「組み合わせ系 + 登録契約」だけで、pose・IK・2D・映像出力はその上の登録モジュール。
5. **エージェント操作が一級**: 全文書が JSON、編集面は JSON-RPC、GUI にしか無い操作を作らない。web 側(別 repo)で開ける形式はエンジンで開ける形式の部分集合(subset 原則、拡張はエンジン先行)。
6. **時間は値**: ワンタイムラインを唯一の軸にしない。時計・再生・トリガー・イベントは port を流れる値で、個体ごとに独立。任意時刻 t での決定的評価がオフライン描画の橋になる。
7. **fail-fast と決定性**: 名前付きエラー(`name@1`)、黙って既定値に落ちない、同じ入力から同じ出力。時計・乱数・再利用ハンドル・登録順に依存しない。
8. **D0(エディタの特権禁止)**: Studio は pelican_project と RPC だけをリンクし、core をリンクしない。エディタが持てる情報はエージェントも持てる。
9. **transaction とパージ可能な寿命**: scene load、hot reload、frame の公開は「prepare(失敗可)→ noexcept publish → rollback / retire」で、旧世代は GPU の fence で退役する。
10. **証拠は本番経路を通す**: 受け入れ条件ごとに「機能が無いとき何が観測されるか」「何を取り消したら落ちるか」。代理条件を認めない。

### 1.4 系譜(外部リサーチ 2026-09-16 で確定した参照先)

pelican は「ノードベースのエンジン」より、**既存の機能を使うところから、中身を組み替え、コードで拡張し、再び部品として配るところまでが連続している制作環境**と捉える。

| pelican の領域 | 主な参照先 | 参考にする設計 |
|---|---|---|
| 標準機能の部品化・内部展開・再利用 | Houdini Digital Asset(HDA) | 内部ノード群を、公開パラメータと説明を持つ一つの道具として扱う。parameter promotion。編集許可と定義への復帰 |
| 数値・曲線・信号によるパラメータ制御 | TouchDesigner の Parameter Mode / Component Time | 値の供給元を切り替える操作を parameter 自体に持たせる。部品ごとに独立した時計 |
| シーン・画像・自動化をまたぐグラフ | Gaffer の Box / Reference / Context | 部品を参照して再利用し、「どの時刻・対象・条件で評価した値か」を扱う |
| 描画パスをコードで作り、グラフで組む | NVIDIA Falcor / Mogwai | パスの資源契約(`reflect`)と実行(`execute`)を分け、描画方式の開発を利用者のワークフローにする |
| 同じ論理機能を異なる描画実装へ落とす | MaterialX / Hydra | ノードの定義と実装の分離、scene 表現とレンダラ向け変換の分離 |
| 状態を持つ処理と時間 | Blender Simulation Zone | 前状態を受け取る領域と、出力・キャッシュ・ベイクの境界を明示 |
| 拡張の登録と発見 | VS Code の extension manifest / contribution point、vvvv のノード説明 | 読み込む前に宣言で判断できる。コード側の説明をノードブラウザへ反映 |

参考にしないもの: ワンタイムラインを唯一の軸にする方式、GUI にしか無い操作、プラグイン = DLL という前提。

---

## 第 2 部 現在の実装(2026-09-12 の作業ブランチ `codex/render-target-runtime-slice`)

### 2.1 構成と境界

```
src/project     純粋ロジック(JSON 形式の parse / 検証 / 合成。Vulkan 不要)。Studio と web が共有できる層
src/schema      宣言 schema の葉(nlohmann のみ)
src/core        エンジン本体: ecs / loader / renderer / vkcore / shader / renderingpass / material / model / animation / phys / audio / ui / os / openxr / watch / communication / persistence / gamelogic / userpublic(公開 API)
src/player      実行体(-DPELICAN_PROJECT=<dir>)。game logic DLL を epoch 付き ABI で読む
src/devstudio   Qt Widgets のエディタ(D0: project + rpc のみリンク)
src/devcli      配布支援 CLI(assets / import / dist-config / project …)
src/spvlink     SPIR-V リンカ(PELICAN_WITH_SPIRV_LINK)
```

- build option で機能をパージ: `PELICAN_WITH_OPENXR`、`PELICAN_RUNTIME_SHADER_COMPILER`、`PELICAN_WITH_IMGUI`、`PELICAN_WITH_RPC`、`SKIP_DEVSTUDIO`、`PELICAN_WITH_SPIRV_LINK` など。OFF 構成の build が受け入れ条件に含まれる。
- 出荷 project(`projects/`): example / animgraph_demo / sprite_demo / vrm_xr_demo(+ 作業中の showcase)。`project_catalog_headless_smoke` が実際に起動して画素を検査する。

### 2.2 文書形式(すべて `pelican.<name>` の envelope + version。厳密単一現行版が方針)

| 形式 | 役割 | 現在の版 |
|---|---|---|
| `project.json` | プロジェクト封筒、path 解決、mount、3 段合成(CLI / project / default) | — |
| `pelican.scene` | object と component の宣言。`version 2` は prefab 展開 | 1 / 2 |
| `pelican.prefab` | parameter 付きの再利用単位、Generated component として展開 | 1 |
| `pelican.asset_data` | models / materials / textures / **procedural_geometry**(1 AABB + 名前付き record) | 1 / 2 |
| rendering config(root)+ feature JSON | frame graph の論理層と物理層、feature の canonical anchor と挿入、view family、preset の fork / materialize、区域の差し替え | fragment v1〜v3 |
| `ray_group_layout` / `ray_bindings` | RT の group と record の layout、material / procedural の selector、AS 宣言 | layout v3 / bindings v2 |
| `pelican.anim_graph` | clip / blend1d の state machine(priority / interrupt / duration / conditions) | 1 |
| `pelican.value_graph` | 数値ノード(input / constant / add / multiply / playback / curve)と bindings(旧・独立文書) | 1 |
| `pelican.graph` | 共通グラフ: definitions / nodes / outputs / `graph.instance`(owner 付き) | 1 |
| 宣言 schema v3 | game DLL の behavior / event / component 宣言。`List<Row>`(`$list` / `$row`、永続 row id)、`Flags`(one-hot)、presence 3 値 | v2 / v3 |

### 2.3 実行時の骨格

- **ECS**: archetype / chunk、generation 付き EntityId、free list、`MutationScope`、compile 時の型 → ID。
- **scene load transaction**: prepare / early_publish / publish / activate / commit_metadata / retire / flush / finish の 8 phase。participant(renderer・procedural・audio・persistence …)が manifest に行を持ち、phase trace を記録。effect transaction(behavior の副作用は sink に溜めて commit 後に flush)。
- **hot reload**: file watcher → reload service → participant(model・shader・render pipeline・game DLL)。model reload は prepare token → noexcept publish → rollback。game DLL は shadow copy + epoch anchor + schema fingerprint で不一致を拒否。
- **GPU 寿命**: DeletionQueue は最初の実 submit 成功で batch を seal、旧世代は submission lease の fence で退役。frame は三相(`submitLogicalFrame` → publication callback(noexcept、1 回の flip)→ `publishLogicalFrame`)。
- **決定性**: `EngineTime` の frame index と `setTime`、PCG32、入力の record / replay、golden PNG。

### 2.4 描画

- Vulkan 1.3 / SPIR-V 1.5。frame graph は論理層(名前・型・materialization)と物理層(format・sample・alias)、scope-execution-major の schedule、view family(flat / XR multiview / XR sequential / shadow / cube)。
- feature = 純粋な graph 変換の JSON(標準 18 個同梱、特権なし)。挿入は canonical anchor、パラメータは define と define の materialize。preset の fork / materialize / 区域差し替えを Studio と RPC から編集。
- material: 宣言 → lowering → surface compile → variant。`.surface` は GLSL 断片 + `//!` ヘッダ。bindless の texture / sampler、descriptor ABI の世代管理。
- 影: directional cascade + point / spot(punctual)、PCF radius / bias / strength を feature parameter で公開。
- ポストプロセス: TAA、velocity、bloom、SSAO、color pipeline(sRGB / linear、OETF / EOTF)。
- **RT**(design_raytracing §8 / §11 の段):
  - RT1 / RT2: raygen / miss / closest-hit / any-hit、SBT addressing、material ABI、`q = instance × geometry` の行。
  - RT3-1: `pelican_ray_instance`(mask / flags / custom index)、geometry flags、BLAS variant 鍵。
  - CARRIER-RT3(WP382a〜d): 再帰 schema、行 id による編集、occurrence identity(`occurrence_id_v1` = asset / node / primitive / variant / content の SHA-256)、`get_ray_occurrences` / `resolve_ray_occurrence`、occurrence override。
  - RT3-2(WP386a / b): static AABB + intersection shader、procedural asset の runtime 作成 / 編集、SPIR-V の命令列 / NoContraction 分類、球の oracle(数値 oracle の拡張は保留)。
  - RT4: callable と動的 record(submission 所有の SBT 寿命)、GameContext からの更新。
  - RT5: skin / morph / VAT の compute 変形と BLAS の build / refit。
  - RT6: 3D と間接 dispatch、名前付き TLAS を frame graph node に、複数 geometry BLAS、AS 構築と consumer の交互配置。
  - RT7: task / 接続 / dispatch / AS 順序 / callable 初期値の編集 RPC と Studio。
- 補助描画: debug draw、debug text、gizmo、picking(ID buffer)、sprite。それぞれ ray query を宣言できる。
- OpenXR: session / action / composition(depth layer、mirror)、multiview と sequential。

### 2.5 アニメーション

- 資産: glTF clip、`.vrma`(VRM animation、retarget と phase modifier)、`pelican.anim_graph`、`animation/curve.hpp`(ScalarCurve + 再生設定 + trigger clock)。
- 実行: AnimationService(arena の pose、layout の世代、DLL からの ABI `abi_v1` / `pose_staging_v1` / `vrm_application_v1`)、`EvaluatorV1`(bind / prepareTick / setParameter / forceState / unbind / rebind、決定性 trace)。
- 装着: scene の animation component(`clip XOR graph`、`parameters` List)。名前付き object は scene sink、無名は component 所有。GameContext の `setAnimationGraphParameter` / `forceAnimationGraphState`。
- 共通グラフとの接続: 評価済み姿勢を `render.attribute` の source `{asset, pose_node}` で shader へ、変形後 GPU 属性を `source.deformed_node`、morph 差分と個体の weight を `value.morph_weight`。
- 未了: 正本の一本化(component と `graph.instance`)、value port → parameter、event の出力、root motion、layer / additive / IK、timeline(採らない方針)、物理 → graph。

### 2.6 共通グラフ(`pelican.graph`)

- 展開: definitions と `graph.instance`(owner = authoring object)、port 型は `LogicalTypeRegistry` に登録した semantic type(`pelican.value.scalar@1` など)、op の契約は `GraphOperationContracts` に登録。型不一致・不明 port・必須欠落は compile 前に名前付きで拒否。
- node kind: `value.*`(input / constant / add / multiply / playback / curve / trigger_clock / morph_weight)、`render.*`(scalar / buffer / compute / image / attribute / fullscreen / raster)、`audio.*`(playback / volume / pitch、voice 所有)、draw の vertex / instance / index source。
- 実行: 数値は描画世代に保持して通常 Renderer が評価、数値 → runtime_f32 → GPU compute、owner の生存に沿った停止と再開、個体削除時の GPU 資源回収、装着更新時の状態引き継ぎ(`transferState`)、model と graph 属性の共同 reload。
- 編集: `get_graph_instances` / `edit_graph_instances`(CAS: `base_source_digest`)、Inspector の装着 GUI。scene ごとの graph 選択と scope 表示での edge 編集は未了。

### 2.7 ゲーム制作面

- game logic: native DLL(hot reload)、behavior / system / event / component を宣言 schema で登録、`GameContext`(input、sound、render values、graph parameter / event、animation graph、physics query、debug draw)。
- 物理: query world(raycast / overlap / shapeCast: sphere / box / capsule、GJK / EPA、三角メッシュ BVH、mesh の scene / import / reload 連動、trigger)、CharacterController(one-way、step-up、coyote time、skin width)。剛体シミュレーションは無い。
- 音: bus と voice、per-voice volume / pitch / pause、`soundState`。
- 入力 / UI: action map(deadzone、合成)、record / replay、fixed / content / fill の layout、ImGui(パージ可能)。
- 保存: persistence participant、原子的置換、TOCTOU 対策。
- 2D: sprite view と 2D game layer 設計(`design_2d_game_layer.md`)。

### 2.8 エディタと RPC

- JSON-RPC 2.0(stdio)。scene の CAS 編集(`set_component_value` / `insert_component_field` / `remove_component_field` / add・remove component / prefab op / graph instance)、journal の undo / redo(structural domain の衝突判定、actor 単位)、ticket と preview lease、`get_status` / `get_frame_plan` / `get_ray_occurrences` / `get_components`(recursive v3 catalog)/ physics raycast。
- Studio(Qt): 埋め込み viewport(別プロセス + 再親付け)、outliner、Inspector(宣言 JSON 入力、List 行の追加 / 削除 / 移動、override 候補、graph 装着、asset 選択)、Frame Plan(node / edge / 統合差分 / 生存区間 / callable record / intersection slot)、render config editor(preset fork、feature materialize、区域差し替え、task と AS の編集)、gizmo、視点プリセット。
- web(別 repo `my_webpage`): 共通形式の閲覧(subset。未知 component は opaque 素通しで、名前付き拒否は未実装)。

### 2.9 ツールと検証

- CTest 1864 件(GPU label は `pelican_gpu_lease_run` で machine 全体を直列化。`PELICAN_PYTHON_TESTS=AUTO` で python gate)。golden PNG、headless render、mutation(変異)による受け入れの検出力確認、negative compile fixture、DLL fixture(frozen v2 / v3)、`dumpbin` による symbol gate(production に fault setter が無い)。
- `tools/worktree.sh configure|build|test`(deps remap、`MSBUILDDISABLENODEREUSE=1`、manifest)、`tools/codex_bg_build.sh`、`tools/doclink.py`(docs の行リンクを追従、pre-commit で検査)、`tools/public_api_inventory.py`、RenderDoc capture、Vulkan validation layer の実行時選択。
- 文書: `docs/manual/`(14 章 + 用語集)、`docs/source-code-guide/`(11 章 + 難所索引 + 前提知識 143 項目)、`docs/design_*.md`(54 本)、台帳 `implementation_plan.md`(WP ごとの仕様・レビュー・完了記録)。

### 2.10 進め方(体制)

- 1 WP = 1 branch = 1 squash。設計 → 仕様の敵対レビュー(codex、2 巡で打ち切り)→ 実装(codex)→ 統合側の変異 → 着地 → コードの敵対レビュー。連鎖は致命後退に 1 回だけ延長。
- 役割: 実装・検証・統合・報告は codex、重要な設計判断の独立相談役が Fable(Claude)。2026-09-06 以降 codex が自分で commit。
- 直近の方針: 「全体グラフを正本にする」「ノードへの分解を優先する」「数値検証基盤の拡張は停止(WP386c 保留)」「保守的な積み重ねで読みにくい箇所は触るときに整理」。

---

## 第 3 部 方向(2026-09-16。利用者と確認済み、細部は未確定。正本は `v1_development_plan.md` の「v0 の約束と『往復』」)

- 根幹 = グラフの組み合わせ系 + module の登録契約(型 / op / sink)+ 時間が port 値 + 「GUI 専用操作を作らない」。pose は最初の登録モジュール、IK・layer・2D・映像出力はその上のパージ可能モジュール。
- v0 の約束 = 4 シナリオ(作る / 出力する / 拡張して持ち出す / 外す・失敗させる)を test に落としたもの。契約の凍結は文書・port・接続の正本・評価分類・package manifest に限り、native ABI の永久互換と任意 unload は約束しない。
- 利用者の往復「使う → 手続き化する → 開く → コードで変える → 再び部品にする」を、Bloom と曲線駆動の blend の 2 本で通す。パラメータの供給元は parameter の mode(固定値 / 一方向の接続)で、Inspector の接続は共通グラフの edge そのもの。
- 段: AG-0 → AG-1 → P-1 → P-2 → AG-2 → O-1 → X-1 → AG-3 → R 群。
