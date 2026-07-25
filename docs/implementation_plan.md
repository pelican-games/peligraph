# 実装指示書(コーディングエージェント向け)

> 完了済み WP の逐語記録は [implementation_archive.md](implementation_archive.md) を参照。本書は未完了 WP と共通・運用規則だけを扱う。

対象読者: 実装を担当するコーディングエージェント。各 Work Package (WP) は独立に依頼できる単位として書かれている。

設計の正は以下の文書。本書と矛盾したら設計文書を優先し、矛盾を発見したら作業を止めて報告すること。

- `docs/design_roadmap_renderworld.md` — 全体順序と ECS 境界
- `docs/design_headless_rendering.md` — ヘッドレス描画の設計(以下 [HL])
- `docs/design_shader_freedom_kit.md` — シェーダ基盤の設計(以下 [SF])
- `docs/design_project_format.md` — プロジェクト形式の設計(以下 [PF]。v6 凍結)
- `docs/design_project_format_web_profile.md` — 共通形式 Web プロファイル(以下 [PFW])
- `docs/design_render_pipeline_extensibility.md` — renderer の拡張境界と
  段階実装(以下 [RPE])
- `docs/design_render_graph_compiler.md` — logical type / target planning /
  Vulkan physical plan の詳細(以下 [RGC])
- `docs/design_heterogeneous_execution_graph.md` — typed dialect / domain partition /
  CPU・Vulkan sibling lowering / fragment・closed forest の詳細(以下 [HEG])

改訂履歴: v2 で実装者レビューを反映し WP を再分割・採番し直した。旧番号との対応: 旧WP1→WP1、旧WP2→WP3、旧WP3→WP4+5+6、旧WP4→WP7、旧WP5→WP8、旧WP6→WP9、旧WP7→WP10、旧WP8→WP11、旧WP9→WP12、旧WP10→WP13、旧WP11→WP14、旧WP12→WP15、旧WP13→WP16。WP2(EngineTime)は新設。
v3(2026-07-02): WP1〜17 完了を受けて WP18(プロジェクト形式)・WP19(シェーダ stem)を追加。設計の正に [PF] / [PFW] を追加。web 側の対応作業(WW1〜3)は my_webpage リポジトリの `docs/implementation_plan_web.md` にある(本書の管轄外)。

## 0. 全 WP 共通規則

### ビルド・テスト

```sh
cmake . -B build -DCMAKE_PREFIX_PATH=<Qt install path>   # 初回のみ。Qt 不要の作業は -DSKIP_DEVSTUDIO
cmake --build ./build --config Debug
ctest --test-dir ./build -C Debug --output-on-failure
```

- 完了条件は常に「ビルド成功 + 全テストグリーン + `git diff --check` クリーン」
- 描画挙動に触れる WP は `pelican_player.exe` の短時間起動確認も行う(`docs/rendering_phase1_review.md` の Validation Run と同じ流儀)

### コード規約(既存コードから踏襲)

- C++20、vulkan.hpp(C API 直接使用禁止)、リソースは `vk::UniqueXxx` / `ImageWrapper` / `BufferWrapper`
- 型は CamelCase、関数は lowerCamelCase、ファイルは小文字連結(例 `rendertarget.cpp`)、メンバは snake_case
- モジュールは `DECLARE_MODULE(Name)` + `GET_MODULE(Name)`(container.hpp)。
  `GET_MODULE` は削除せず composition root / 薄い runtime adapter で使う。
  **純粋 compiler、policy、provider callback ではモジュール間依存を関数引数の
  依存構造体で明示する**(render_pass_dispatch.hpp の `XxxDependencies` が手本、
  [RPE] §10)
- ID 型は `PELICAN_DEFINE_HANDLE`(handle.hpp)、ログは quill の `LOG_INFO(logger, ...)` 系
- エラーは fail-fast(`throw std::runtime_error`)。ただしシェーダコンパイル失敗のみ result 返却([SF] §4.1)
- 通常の外部ライブラリ追加はルート CMakeLists.txt の FetchContent 節に追記する。
  ただしVulkan SDK等のplatform toolchain dependencyは明示providerとし、
  cross targetへ暗黙source-build fallbackを持ち込まない

### レイヤ規則

- `src/core/vkcore` から `src/core/renderingpass` 以上のレイヤへ新たな include を**追加しない**(通知はフラグ/イベントで上に渡し、編成は上位層が行う。WP8 が実例)

### テスト規約

- Catch2 v3。`test/CMakeLists.txt` の `pelican_define_test(<name> [libs...])` で登録
- GPU 必須テストは Vulkan デバイス列挙失敗時に `SKIP()` すること

### 禁止事項

- `src/core/ecs/` の無関係変更。変更禁止そのものは 2026-07-08 の
  ユーザー決定で解除済みだが、ECS 変更は WP の明示範囲に限定し、
  lifecycle / generation / failure-atomic 規約を回帰テストで固定する
- 無関係箇所のリフォーマット・リネーム(diff を見る人間のため)
- 挙動変更とリファクタの同一コミット混在
- 1 WP = 1 ブランチ = 1 PR。ブランチ名は `agent/wp<番号>-<短い説明>`

## 1. 現行 WP 一覧

| WP | 内容 | 状態 |
|----|------|------|
| WP203c | XR2b-c — OpenXR array swapchain / depth submit / GPU gate | 未着手 |

完了済み WP の一覧・依存関係・本文は
[`implementation_archive.md`](implementation_archive.md) に逐語保存する。
(最新完了: WP203b、2026-07-26。本文はarchive、完了レポートはdesign_reviews参照。)

## 2. WP 詳細

### XR2b 分割 WP の逐語条件と所有権

初回レビューの逐語条件:

### XR4 — demo、XR2b — multiview

XR4 は XR2a.0〜3 と XR3a/b 完了後に限る。XR2b は XR2a.0 の logical-frame/view contract を保ったまま
graph/pass の view dimension を追加し、XR2a と同じ semantic image、flat view_count=1 byte 一致、GPU 計測改善を
gate にする。XR2a の壊れた二回 `Renderer::render()` を互換契約として残してはならない。

分割後の所有権は次で固定する。WP203aはtarget-planningの前提だけを完了し、
下記の最終gateを満たしたとは扱わない。

| 条件 | 一意の所有 WP |
|------|---------------|
| logical-frame/view contractを維持したgraph/pass view dimension、flat view_count=1 byte一致、二回`Renderer::render()`を残さない | WP203b |
| XR2aと同じsemantic image、OpenXR array/depth submit、GPU計測改善 | WP203c |

### WP203c: XR2b-c — OpenXR array swapchain / depth submit / GPU gate

**目的**: WP203bのVulkan multiview実行をOpenXR compositionへ接続し、見た目と性能の
XR2b最終gateを満たす。

**実装範囲**:

1. stereo color swapchainを2D arrayとして作成し、各projection viewの
   `imageArrayIndex`とacquire/wait/releaseを一logical frameの契約へ接続する。
2. runtime対応時は`XR_KHR_composition_layer_depth`用depth swapchainと
   `XrCompositionLayerDepthInfoKHR` chainを追加し、非対応runtimeは理由付きでcolor-onlyへfallbackする。
3. mirror、submission fence、runtime-generation lease、history onceの既存保証を維持する。
4. sequentialとmultiviewで同じsemantic imageになることを検証し、GPU timestampで
   改善を計測する。改善しないdeviceでは`auto`選択をdevice profileへ反映できる証跡を残す。

**受け入れ条件**:

- OpenXR protocol fakeでarray index、call order、failure rollback、depth chainを固定する
- Meta XR Simulatorと実機チェックリストで左右、depth、mirror、session再作成が成功する
- XR2aと同じsemantic image
- 対象GPUでsequentialよりGPU計測が改善する
- 全CTest、OpenXR validation、`git diff --check`が成功する

依存: WP203b。見積: 大。

### 完了地点

WP203aでlogical XR policyとdevice-dependentなVulkan view execution planningを分離した。
`auto` / `sequential` / required `multiview`、scopeの実行回数とview mask、resourceの
shared/sequential/layered layout、device capability/fact、理由付きfallbackまで実装済みである。
WP203bでarray image/view、per-view UBO、`gl_ViewIndex` reflection、typed pipeline/pass
view contract、view-masked dynamic renderingに加え、production fullscreen shader variant、
layered input descriptor、mixed-scope scheduler、Renderer view-family境界まで実装した。
通常OpenXR描画はtargetがeye別2D swapchainである間だけ安全にsequentialを維持する。
次の実装はWP203cのarray swapchain / composition depth / semantic・GPU計測gateである。

WP202bでrenderer-wide `RenderStrategy`を
preset展開後・feature composition前の独立ABIとして実装した。typed renderer facade、
builtin identity、game-DLL V1/V2 provider、failure-atomicな全config生成を
flat/preview/XR、logical graph、Vulkan target plan、transaction publicationへ接続した。
WP202aでは順序付きglobal `GraphTransform` chainを実装し、protected boundaryと
canonical nodeを維持したfull-config candidateだけを再compileして採用する。
WP201 でtagged region / subgraph replacementを
public game-DLL providerまで縦切りし、元graphを破壊しないcandidate再compile、
typed boundary不変検証、logical/target provenance、transaction snapshotへ接続した。
WP200 ではfullscreen passのshader pair差し替えを
`PassImplementationProviderV1`として公開し、builtin identityとgame DLL実装を
同じtyped logical contract / owner-aware registry / transaction snapshot経路へ接続した。
WP199 でPython製gateを既定OFFの明示opt-inへ変更し、
OpenXRの不要な任意Python探索を通常構成から隔離した。
WP198 でruntime shader compilerのproviderを
Vulkan SDKへ限定し、PC host compile / target precompiled-SPIR-V境界を固定した。
WP197 でPython/test-tool dependencyを通常buildから分離し、
Python testをAUTO/ON/OFF化、experimental SPIR-V linkerを既定OFFのbuild unit化した。
WP196 では project-backed rendering config / feature /
preset の同一 watcher-frame 変更を一つのtransactionへcoalesceし、preview + flat
(+ 起動中のXR)を全prepare後に一回のruntime generation CASで公開する経路を実装済みである。
window swapchain、offscreen、OpenXR各eye、独立desktop mirrorの実submission fenceが
使用generationを保持し、旧GPU resourceは最後の対応fence完了より前にretireされない。
失敗時はactive generation、registry、config cache、watch dependencyを維持する。
renderer の次候補は XR2b multiview / array-layer / depth-submit lowering、または
physical plan eject / direct authoring fixtureである。`NativeScope`は具体的な
Vulkan-only利用例を得てから進める。

## 3. トラック現況(WP 化待ちを含む)

- **【方針決定 2026-07-12】feature 層 = ユーザー空間**(ジッタ相談からの
  一般化 — ユーザー決定): テンポラル系のように**手法が発展し続ける領域は
  purgeable feature 層で吸収**し、ユーザーが「まぁまぁいじる」前提で
  境界を固定する。
  1. **三層の速度分離**: エンジン = 機構語彙(anchor / history /
     projection_jitter 枠 / snapshot / format_class 等 — **版付きで
     ゆっくり additive にだけ増える**)/ feature(JSON + シェーダ)=
     ユーザー空間(速い・自由)/ project config = 1 行有効化
  2. **同梱 feature(hdr / shadow / bloom / debug 系)= 特権なしの標準
     ライブラリ**(standard/toon lighting の dogfooding と同格)。
     `engine://features/*.json` をプロジェクトへコピーして改造したら
     自分のもの、が公式ワークフロー(ロードは ref ベースで既に
     プロジェクト相対を通せる — 保証テストを次の feature 系 WP に同梱)
  3. `pelican.render_feature` schema は実質の公開 API — [PF] と同じ
     凍結規律(additive・版付き)で扱う
  4. **いじらせない境界**(検証が名前入りエラーで防衛): canonical
     anchor の全順序(挿すのは自由・エンジン terminal の並べ替え不可)、
     output_transform 等の常設ノード、色 invariant、決定性ゲート
  5. 新手法対応の型: エンジンは機構語彙を 1 個足すだけ(例: 将来の
     アップスケーラ向けフェーズ数属性)→ ユーザー feature が組み合わせる

- **レンダーパイプライン拡張境界(RPE、2026-07-23 v2.1 / HEG v1)**: versioned
  `hybrid_v1`、semantic material route、deferred + forward の scene-linear 合成は
  実装済み。以後は [RPE] / [RGC] の preset から physical/native までの拡張 ladder と
  Request / Resolved / Compiled / Prepared / Runtime 語彙へ揃える。現在の単一
  material-batched draw queue、sample count 1 固定、XR/preview ad-hoc callback を
  一度に直さず、RPE1(WP180)→ RPE2 typed plan(WP181) →
  RPE3 DrawQueueBuilder(WP182) → RPE4 provider registry(WP183) →
  RPE5 bounds / phase queue / transparent sort / XR view policy(WP184) →
  RPE6a logical type / shadow graph(WP185) → RPE6b0 versioned logical value /
  producer edge(WP186) → RPE6b1 typed screen input / opaque snapshot(WP187) →
  RPE6c0 topology / backend probe / optimize-by-default / advisory diagnostics(WP188) →
  RPE6c1 desktop/tile target plan(WP189) → typed sample-count resolve + 実 MSAA
  runtime vertical slice(WP190) → physical target planner runtime統合(WP191) →
  builtin graph variant policy(WP192)、runtime publication root(WP193)、
  append-only GPU registration transaction(WP194)、scope-aware replacement と
  generation-owned registry lease(WP195)、submission-fence lifetime と
  pipeline watcher publication(RPE10b3 / WP196)、fullscreen
  PassImplementation provider(RPE11a / WP200)、tagged region / subgraph replacement
  (RPE11b / WP201)、global GraphTransform(RPE11c / WP202a)、renderer-wide
  RenderStrategy(RPE11d / WP202b)まで完了。builtin identityと
  game DLL差し替えproviderは同じtyped
  boundary contractを使い、replacement candidateは全logical graphを再compileしてから
  target loweringへ進む。global transformは順序付きfull-config candidateとして実装し、
  renderer strategyはpreset後のseed config全体を生成する別ABIとして実装した。
  strategy V1のfacadeは現compilerが実際に消費できる機構だけを公開し、live
  material/light/geometry inventoryは後続versionへ残す。
  RPE6c0/1 では [HEG] の immutable canonical / disposable lowering seam、
  dialect legality、pairwise endpoint relationを置くが、汎用 CPU scheduler、execution linker、
  動画 backend は計測・具体需要まで実装しない。logical effectは作者を信頼する任意宣言、
  lower-level warningのstrict化はproject / CI opt-inとする。通常policyは宣言contractを信頼して
  parallel / fusion / aliasを許すoptimize-by-defaultとし、serial / isolation / conservative
  debugは明示時だけ有効にする

- **コマンド／エディタ層**: stdio JSON-RPC、`load_gltf` /
  `update_transforms`、typed editor query/edit、actor/CAS、undo/redo、
  atomic save、snapshot import、watch token、`eval_preview` /
  `render_preview`、薄い `pelican_rpc.py` まで実装済み(WP27/42/
  149〜172)。windowed host は bounded queue で engine thread の
  frame boundary に dispatch する。外部接続は引き続き 1 本で、
  複数クライアントは WebSocket 展開時の課題
- **ゲームロジック(ネイティブ C++)**: スクリプトを特権化せず、C++ の
  G1a/G1b/G2、behavior attachment/edit、二世代 DLL reload まで実装済み
  (WP43/45/90/155/162/167)。ゲーム固有ロジックは公開 service と
  `PELICAN_PROJECT` 経路で差分実装する
- **devstudio(Qt) / エディタ**: D0「エディタ特権の禁止」に従う
  typed service/RPC、ImGui Object Tree・schema-driven Inspector・
  Asset Browser、保存/undo/preview の共通実装は WP149〜172 で完成。
  Qt DevStudio の埋め込みビューポート、ピッキング、ギズモは未実装。
  Qt/ImGui/外部クライアントは同じ `EditorCommandService` を消費する
- **カメラシステム**: glTF 1:1 の perspective/orthographic、複数
  camera / atomic `set_camera` と公開 API だけで動く orbit/follow/fly は
  実装済み(WP48/50)。残りは glb round-trip と transform_seq v2 の
  camera track。カット/ブレンド/シェイクはユーザー空間の controller で
  実装する
- **2D ゲーム機能 + 2D⇔3D 相互変換**: sprite contract/GPU world quad、
  pixel-perfect/flipbook、shapeCast、side-scroller vertical slice まで実装済み
  (WP103/104/106/107/109)。UI と描画基盤を共有し上物を分離する。
  2D scene の 3D 空間配置・3D scene の 2D 投影編集は未設計
- **アニメーショングラフ**: A0〜A2、VRM-S0/S1、VRMA-C0/R0/I0
  まで実装済み(WP94〜102/111/121〜134/176〜178)。VRMA は
  body/expression/gaze を typed source として graph へ接続し、
  generation mismatch は明示 `rebind()` する。未実装は root-motion
  policy、automatic `.vrma` watcher 配線、graph v2、timeline/live source、
  SpringBone
- **物理クエリ／イベント**: raycast / overlap / shapeCast、Builtin/Jolt/
  game-DLL provider、purgeable OFF stub、決定的 `OverlapEnter/Exit` まで
  実装済み(WP46/47/107/165/179)。Jolt は query provider であり、
  rigid-body world/step/constraint は未実装。rpc query・mesh/BVH も P3
- **OpenXR トラック**: XR0〜XR4 と Simulator blocker 修正まで実装済み
  (WP125〜138)。Vulkan bootstrap、session loop、左右眼 sequential
  composition、action/pose、reference space、feature policy、left-eye
  mirror、VRM demo を Meta XR Simulator で検証済み。残りは XR2b
  multiview/depth submit、物理 HMD gate、Quest standalone SA0〜SA3
- **bindless バックエンド**: 2026-07-08 方向決定 — classic(set 2)と併用(`design_material_shading.md` §3-5)。生成アクセサが差を吸収、M2 のデータ形(SSBO + 参照)が前提工事。実装は M2 の後・GPU 駆動系(WP36 パーティクル・大規模シーン)の需要と同時に WP 化。**web/モバイルの床に PC を縛らせない**(web は将来やるとしてもシンプルな 3D/2D — ユーザー確認)
- **web ビルド(WASM)**: 将来の可能性としてのみ保持(2026-07-08)。守るべき不変条件は全部現行規律(純ロジック規律・データ契約が抽象・classic 床・dist-bake/WGSL レーン)— 特別な保全作業なし。進めるときは案 B(WASM ゲームコア + TS レンダラ接合)→ 案 A(C++ WebGPU 実行系、データ契約の兄弟執行器)。**RHI の後付けは禁止**(本体の C++ インターフェースへの制約源にしない)
- **アセットホットリロード**: HR0〜HR2-G 完了。WP147 で model
  reload 時の animation 全体 reset を対象 asset generation + evaluator
  rebind へ置換し、WP162 で game DLL の二世代 side-decode を固定、
  WP178 で VRMA reload generation entry point を追加した。確定規約
  (リプレイ/strict/rpc 中無効・自己書き込み `(AssetKey, hash, epoch)`
  token)と単一 FileWatcher 経路を維持する。WP196で rendering config /
  render feature / pipeline preset の一括publicationを接続済み。残りは
  HR2-I(input/profile)、U3(UI)、`.vrma` watcher の自動配線
- **イベント層**: E1(WP56)と typed payload schema(WP71)に加え、
  ユーザーレビュー済み v1.1 の E2 `OverlapEnter/Exit` を WP179 で
  実装済み。Stay は需要が出た場合だけ additive に追加する
- **永続化(user:// + 設定/セーブ)**: [PF] v6.3 の `user://` と P1
  settings/saveData/loadData/listSaves を実装済み(WP55/65)。save delete、
  cloud sync、非同期 I/O は需要時に additive WP 化する
- **[PF] v6.3**: `user://`、asset store + `.pelican/local.json`、
  `#fragment` を承認・凍結済み。V1/WP55、V2/WP66、V3/WP57、
  K1〜K4 は WP77/79/81/84 で実装済み
- **コンテナアセット**: fragment load、glTF scene extraction、
  `pelican-import-tools` の PSD/atlas、`imports.rules.json` まで実装済み
  (WP77/79/81/84)。PSD 系は引き続きエンジン非リンクの外部ツール契約
- **RenderWorld / ECS**: (2026-07-02 方針変更)統合ブランチ系列を開発本線として独自に進める。**(2026-07-08 改訂・ユーザー決定)`src/core/ecs/` の変更禁止を解除** — 凍結の代償(PhysWorld 生ポインタ・カメラ二重所有・dummy コンポーネント・cb_deinit 不呼出で非自明型が memcpy 移動される)が 2026-07-08 リファクタ監査で顕在化したため。以後 ECS コアに世代付き EntityId・construct/move/destroy 規約等を入れてよい。main との合流は「随時追従 merge」から「将来の逆提案(こちらの ECS 改良を main へ提案)」へ位置づけ変更。**互換受理の追加禁止(同日決定)**: ランタイムは v1 だけを読む。旧形式の変換が要る場合は外部ツールで行う — 以後の WP が新旧両対応の受理コードを足すことを §0 違反とする
- **コマンド層の WebSocket 展開**: stage 2(stdio JSON-RPC)実装後、同じメソッド群を WebSocket に載せると devstudio と web viewer(my_webpage)が同一プロトコルでエンジンを叩ける([PFW] §7)。stage 2 の後に設計文書を書いてから WP 化
- **asset manifest(sha256)**: `assets.manifest.json` の生成・照合・起動時検証と
  asset store mount を WP55/66 で実装済み。通常起動では診断し、
  `--strict-assets` 指定時は不一致を起動前 error にする
- **naga 変換のビルドスクリプト化**: [PFW] §4-3(a) の WGSL→SPIR-V 一括変換を node CLI 化(web repo 側作業。実績コードは `apps/site/src/lib/shader/nagaSpirvCompiler.ts`)。WW3 の後
- **プロジェクト解釈レイヤ(`pelican_project`)**: パース・検証・パス解決・
  正規化をエンジン非依存 target へ分離済み(WP44)。engine は薄い binder、
  `pelican_cli import` は engine 外 consumer としてこの target を使う
- **compute / GPU 計測 / bindless / RT**: compute task graph/実行一本化は
  WP33〜35/64、stereo-safe GPU timing・VRAM/XR timing は WP143/145 で
  実装済み。bindless と RT は需要・設計合意後に WP 化する

## 4. マルチエージェント運用(ブランチとマージ)

### 規則

1. **main を常にグリーンに保つ**: 受け入れ基準を満たした PR だけが main に入る。マージ後に `ctest` が割れたら最優先で revert
2. **1 WP = 1 ブランチ(`agent/wpN-...`)= 1 PR = squash マージ**: main の履歴が「1 コミット ≒ 1 WP」になり、bisect と revert が WP 単位でできる
3. **WP ブランチは統合ブランチ(下記 5)から切る。WP 同士のスタック禁止**: 依存 WP が未マージなら着手しない(待ち時間は別の独立 WP を取る)。マージはレビュー通過後ただちに(長生きブランチを作らない)
4. **契約文書(`external_tools_requirements.md` / schemas)は WP の PR に混ぜない**: 専用 PR+人間レビュー+ツール側コピー同期(`dcc_integration_qa_2026-06-12.md` §3 の規約)
5. **(2026-06-12 改訂)`codex/rendering-phase1-refactor` を当面の統合ブランチとする**: main へのマージを待たずに WP を開始できるようにするため。WP ブランチはここから切り、PR のベースもここへ向ける。根拠: このブランチは origin/main(ECS 最新)を取り込み済み(6bc26c2)で、フルビルド+全テスト+player 起動を検証済み=「main の上位互換かつグリーン」。運用条件:
   - **main への取り込みは merge commit で行う(squash / rebase 禁止)**。履歴を書き換えると、ここから切った WP ブランチ全部のベースが無効になるため
   - main(ECS 側)に新コミットが入ったら統合ブランチへ随時 merge して追従する(取り込み手順は 6bc26c2 と同じ: 重複ファイル確認 → merge → ビルド+ctest)
   - main へのマージ後は統合ブランチを廃止し、以降の WP ブランチは main から切る(規則 3 に戻る)

### 競合が予想されるファイルと作法

| ファイル | 触る WP | 作法 |
|---|---|---|
| `appflow/loop.cpp` | 2, 5, 17 | 直列にスケジュール(下のウェーブ)。同時に走らせない |
| `vkcore/renderer.cpp` | 2, 8, 13, 14 | 同上 |
| ルート CMakeLists.txt(FetchContent 節) | 6, 10 | 追記のみ・アルファベット順。競合しても自明に解決できる形を保つ |
| `test/CMakeLists.txt` | ほぼ全 WP | `pelican_define_test` の追記のみ |
| `core/loader/*`・`player/main.cpp` | 18, 19 | WP18 の分割 PR(a→b→c)は直列。WP19 は WP18 マージ後 |

### ウェーブ(依存を満たしつつ並列度を上げる依頼順)

| ウェーブ | 並列依頼 | 備考 |
|---|---|---|
| 1 | WP1, WP3, WP9, WP10 | 互いに独立。最大 4 エージェント |
| 2 | WP2, WP4, WP11(+WP8) | WP8 は renderer.cpp が WP2 と重なるため WP2 マージ後に開始 |
| 3 | WP5, WP12, WP17 | WP17 は loop.cpp が WP5 と重なるため WP5 マージ後に開始 |
| 4 | WP6, WP13 | |
| 5 | WP7, WP14, WP16 | WP7 完了 = headless 検証基盤(統合チェックポイント) |
| 6 | WP15 | 大物・高リスク。単独で走らせ、他 WP と並走させない |
| 7 | WP18a → WP18b | 完了済み(2026-07-02 レビュー合格)。web 側 WW1 も完了 |
| 8 | WP19, WP25, WP26(+ WW2 別リポジトリ) | WP18b マージ後に並列 3 本。競合回避: basicconfig.{hpp,cpp} は WP19 専有(WP25 は触らない規約)、WP26 は画像経路と CMake FetchContent 節のみ |
| 9 | WP18c | **WP19・WP25 マージ後**(example の shader 参照を stem 形式・scene を v1 形式という最終形で一度に書くため)。WW3(web stem)もこのウェーブから開始可 |
| 10 | WP20(a→b), WP21(+ WW5・houdini-adapter は別リポジトリ) | WP20 と WP21 は並列可。競合: test/CMakeLists.txt(追記のみ)とルート CMakeLists FetchContent 節(WP21 のみ追記)。WP20 = model/playback/shader 系、WP21 = devcli/loader 系で分離 |
| 11 | WP28, WP33, WP37 | 並列 3 本。WP28 = featurecompose + shader/pipelinefactory 系(renderingpassconfigloader は WP28 専有)、WP33 = frameplanner 新設 + テストのみ(**実行系変更禁止**)、WP37 = os/入力系。共有追記は test/CMakeLists.txt のみ |
| 12 | WP29, WP30 | WP28 マージ後。WP29 = renderer 計測系、WP30 = feature アセット中心で接触面小 |
| 13 | WP34 | 実行系の大物。単独で走らせる(完了 — render 切替も先取り実装) |
| 14 | WP35, WP39(+ WW6 別リポジトリ) | 並列 3 本。WP35 = rpc/framegraphruntime 周辺、WP39 = os/入力 + loader 小、WW6 = web。共有は test/CMakeLists.txt 追記のみ |
| 15 | WP40 | ビルドユニット化(完了) |
| 16 | WP41, WP42, WP43 | 並列 3 本。専有: WP41 = devcli、WP42 = communication + loader/scene バインダ(loop.cpp 禁止)、WP43 = appflow/loop + userpublic(communication 禁止)。共有は test/CMakeLists.txt 追記のみ |
| 17 | WP44(解釈レイヤのターゲット分離 — 移動+委譲のみ、単独)、WP31/32/36/38、入力 I2〜I4 | 着手前に詳細登録 |
| 18 | WP44, WP46 | 完了(2026-07-07) |
| 19 | WP47, WP48 | 完了(2026-07-07)。専有: WP47 = phys/scene バインダ/debugdraw、WP48 = renderer/camera + rpcserver |
| 20 | WP31, WP49(I2), WP50(C2) | 並列 3 本。専有: WP31 = resources/features + シェーダ + renderingpass/pipelinefactory、WP49 = communication/jsonrpc + 入力注入(os/input)、WP50 = userpublic システム + renderer/camera。**3 本とも golden ケースを追加するため件数 REQUIRE は統合時に調整(各自は自分の追加分のみ数える)**。共有は test/CMakeLists.txt 追記のみ |
| 21 | WP51(音), WP52(シーン遷移), WP53(乱数), WP54(debug_text) | 並列 4 本。専有: WP51 = core/audio 新設 + ルート CMakeLists FetchContent、WP52 = loader/scene + GameObjects 破棄経路、WP53 = 乱数(新規ファイル)、WP54 = features/レンダラ。**gamecontext.{hpp,cpp} は 4 本全部が追記する — 各自ファイル末尾に足し、統合時に調整**。rpcserver は WP52/53 が両方 get_status を触る(小競合予定)。golden 追加は WP54 のみ |
| 22 | WP55([PF] v6.3 リゾルバ), WP56(イベント E1), WP57(init 雛形), WP58(マテリアル M1) | 並列 4 本。専有: WP55 = loader/pathresolver、WP56 = userpublic イベント + gamecontext(単独)、WP57 = devcli、WP58 = src/project 新規 + docs/shader_contract.md。**rpcserver は WP55(get_status stores)と WP56(inject_event)が交差 — 追記形で書き統合時に調整**。golden 追加なし |

統合チェックポイント: ウェーブ 1 完了後と WP7 完了後に、人間が pelican_player の手動起動確認
(`rendering_phase1_review.md` の Validation Run と同じ流儀)を行う。WP16 以降は
golden テストが回帰を機械的に守る。

## 5. 依頼時のテンプレート

```
リポジトリ: pelican2 / ブランチ: main から agent/wpN-xxx を作成
タスク: docs/implementation_plan.md の WPN を実装してください。
設計文書 docs/design_*.md の該当節を必ず読むこと。
完了条件: WPN の受け入れ基準 + §0 の共通規則。
逸脱・不明点があれば実装せずに質問すること。
```
