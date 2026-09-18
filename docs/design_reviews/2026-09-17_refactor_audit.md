# リファクタ監査: 作りが悪い箇所・複雑すぎる箇所の確定リスト

日付: 2026-09-17(source `990b9eaa`)
入力: 領域別監査 6 体(Opus subagent、読み取り専用)の報告 → [raw](2026-09-17_refactor_audit_raw.md)。
判定: Fable。実害を伴う主張は本書の §1 に挙げたものを Fable が source を開いて再確認した。
それ以外の行数・重複行数は各 agent が実測した値で、Fable は追試していない。

範囲は `src/core/` 全体(約 19 万行)+ `src/player` `src/devcli` `src/spvlink` の入口。
`src/devstudio` と `test/` は対象外。build も test も走らせていない(静的監査)。

既知の負債(2026-07-17 の debt 議論、`v1_development_plan.md` の「保守上の整理候補」、
`v0_freeze.md` §4、2026-07-26 の census_vocabulary)と重なるものはその旨を書いた。

## 0. 全体像(1 段落)

**規律は高く、規模の抑制だけが効いていない。** TODO/FIXME/HACK は core 全体で 0 件、`#if 0` 0 件、
名前空間スコープの可変グローバル 0 件、raw `new` 2 件(どちらも意図的)、Vulkan handle は全面 RAII、
エラーには `pelican.*@1` の ID が付く。死んだコードもほぼ無い(`legacy_*` は診断付きの現役経路)。
問題は一貫して **「同型の処理を型でなく手作業で並べている」** ことに集約される。
新しい registry / adapter / 経路を足すときに既存をコピーして名前を置換する運用が定着していて、
共通化の受け皿(`renderingpassjsonhelpers.hpp`、`src/project/stablefingerprint.hpp`、
`RenderTargetDefinition`、`EditorProjectionAdapter`、`decodeStructObject`)が**既にあるのに使われていない**。
結果として「1 箇所直して 5 箇所忘れる」形の事故が構造的に仕込まれている。
そして実際に、コピー同士が既にドリフトしている箇所が複数ある(§1)。

複雑さは 4 つの神関数 / 神クラスに集中する:
`Renderer::renderLogicalFrame`(1229 行)、`executePlannedFrameGraph`(952 行・引数 16)、
`SwapchainFrameTarget::Impl`(2419 行)、`PolygonInstanceContainer`(109 メソッド)。
これに `materialcontainer.cpp`(5159 行)、`editorjournal.cpp`(6464 行、匿名名前空間 4374 行)、
`AnimationServiceRuntime::Impl`(1749 行)、`SceneLoader::load`(672 行)が続く。

## 1. 実害を伴う箇所(Fable が source で再確認したもの)

「作りが悪い」の中でも、既に挙動が食い違っている / 潜在バグになっているもの。修正は小さい。

| # | 場所 | 何が起きているか | 修正の大きさ |
|---|---|---|---|
| 1-1 | `src/core/shader/raygrouplayout.cpp:2155` | `std::to_string(id) + "\0"` は `const char*` の `strlen` が 0 なので**区切りが入らない**。geometry row digest の意図(id と payload の NUL 区切り)が未実装。隣の material 経路(1991-1993)は `'\0'` で正しくやっている。この書き方は repo 内でここ 1 箇所 | 1 行 |
| 1-2 | `src/core/communication/editorjournal.cpp:5732` | commit 成功結果を `results` / `completed` に公開した**直後**に `forceAbort("base_revision_stale")` を呼ぶ。`forceAbort` は `EditorPreviewAbortError`(`std::runtime_error` 派生、`editorjournal.hpp:99`)を投げ、同じ try の `catch (const std::exception&)` に落ちて **committed を failed で上書きし、`completed` に 2 件目を push する**。preview lease が生きている間の commit だけで起きる | 呼び出し位置を publish の前へ、または局所 try |
| 1-3 | `src/core/vkcore/renderer.cpp:3405-3428` vs `3690-3705` | reload 2 経路の publish ラムダ約 63 行がコピーで、`transaction->finalized = true` の位置が **A は outcome 代入の前、B は後**。今は挙動差なしだが、独立に編集された証拠。`graph_runtime_transfer->commit()`(3647)が B にだけあるのが意図か写し漏れかコードから判別できない | `RenderPipelineTransaction` にメソッド化 |
| 1-4 | `src/core/loader/componentcodec.cpp:423` vs `src/core/loader/editorpreviewprojection.cpp:155` | world 回転の合成が runtime は `parent * local`(正規化なし)、preview は `glm::normalize(parent * local)`(local 側も 146 で正規化)。**非正規化 quaternion を書いた scene で preview と runtime の world 回転がずれる**。「web で開ける ⊆ pelican で開ける」の前提が数式 2 本に依存している | 合成関数を 1 本に |
| 1-5 | `src/core/userpublic/render/*_abi_v1.hpp`、`animation/abi_v1.hpp`、`physics/query_types.hpp` | 7 ヘッダが同名の `enum class Status` を**別の数値**で定義。`invalid_argument` は draw_sort / animation / physics が 2、graph_transform / pass_implementation / render_strategy / subgraph_replacement が 1(draw_sort が `buffer_too_small = 1` を挿入したため以降が全部ずれた)。DLL 境界の数値なので `static_cast` 転送で静かに別エラーになる | 共通 `abi_common_v1.hpp` |
| 1-6 | `src/core/userpublic/details/schema/structfieldjson.hpp:619-626` vs `684-699` | encode の optional 枝と非 optional 枝がコピーで、**`std::optional<vec2/3/4/quat>` だけ成分の range 検証を飛ばす**。decode 側も 29 行完全一致の複製 | optional を剥がしたヘルパ 1 本 |
| 1-7 | `src/core/animation/animationservice.cpp:791, 827, 846, 870` | `runPhases` が `std::scoped_lock` を握ったまま client callback(827)を呼び、callback は同じ service に再入する。`mutex` が `std::recursive_mutex`(128)なのはこのため。**再入契約が undocumented** で、worker thread から呼ばれたら deadlock。加えて `catch (...)` が全例外を `Status::out_of_memory` に潰す(846, 870)。`releaseOwner`(756-787)は 4 map を tombstone するだけで erase しないので DLL hot reload ごとに map が単調増加 | callback を lock 外へ、例外→Status の明示対応 |
| 1-8 | `src/core/animation/posegraphruntime.cpp:346-353, 875` | evaluate の throw(572, 580, 589)を `catch (...)` で `failed = true` に潰し、875 で情報のない `"pose graph evaluation failed for <name>"` を投げ直す。コメント「owner thread re-raises it」は事実と違う。`framephase.cpp:188-192` でさらに `"animation phase evaluation failed"` に潰れる。**graph 作者に "pose port is unconnected: blend1.b" が届かない** | `std::exception_ptr` を保持して rethrow |
| 1-9 | `src/core/os/actionmap.cpp:180-262` ≡ `src/core/communication/rpcserver.cpp:778-861` | key 名 parser 82 行が逐語コピー(正規化 diff で差は関数名と `lowerAscii` 前処理だけ)。RPC 注入入力と live binding の key 名が**偶然でしか一致しない** | header に 1 本出す |
| 1-10 | `src/core/profiler.cpp:20-32` | 唯一の成功経路 log がコメントアウトされ、`logger` が null なら **stdout に zone ごと毎フレーム出力**。stdout は JSON-RPC の channel(`loop.cpp:394`) | 数行 |
| 1-11 | `src/core/gamelogic/behaviorarena.cpp:278-451` vs `503-734` | attachment edit と scene reconciliation の transaction 機構が 62 行逐語 + 約 130 行同型のコピーで、**既にドリフト**: publish で params update を全件適用するか `reused` だけか、rollback で swap→transfer を全件ずつか handle ごと交互か、が違う。scene load の rollback 経路 | `AttachmentTransaction<Policy>` に統合 |
| 1-12 | `src/core/renderingpass/computetask.cpp:3740-3792` vs `computetask.hpp:88-107` | `ComputeTaskRuntimeDependencies` で container を注入する契約なのに、`dispatch` / barrier は `GET_MODULE` でグローバルを引く。今は同一 instance だから動くが、**variant / reload で別 instance を渡した瞬間 id は同じで中身が違う container を引く** | 注入かグローバルか片方に倒す(実態はグローバル) |
| 1-13 | `src/core/animation/animgraph.cpp:679` | メンバ関数ローカルの `static TransitionV1 forced_transition;` を全 evaluator が共有しアドレスを返す。今は `duration == 0` で即消費されるので無害、duration を持たせた瞬間 dangling | 数行 |

## 2. 構造的な負債(領域別、agent の上位)

各領域の 12 件は raw を見る。ここでは「同じ形の問題」で束ねる。

### 2.1 同じ骨格の複製(受け皿があるのに使われていない)

- **provider registry 4 本 + 2 本**(`renderingpass/{graphtransform,subgraphreplacement,renderstrategy,passimplementation}registry.cpp`、`vulkannativescopeexecutor.cpp`、`renderer/renderpolicyregistry.cpp`)。lifecycle 147 行が 4 ファイルで正規化後に完全一致、ABI helper 70-90 行 × 4、`StableFingerprint` が 4 実装で `src/project/stablefingerprint.hpp` の `StableFingerprint64` と一字一句同じ。約 1,200 行の冗長。
- **projection adapter 6 class**(`loader/editorprojectionadapters.cpp:1204-1428`)。Camera と Light は 52/53 行で差分 13 行。`SnapshotProjectionAdapter<Module, Prepared, Kind, Mode>` で 5 行の派生になる。
- **descriptor set 構築**(`material/materialcontainer.cpp:4370-4528` ↔ `fullscreenpass/fullscreenpasscontainer.cpp:653-826`)。5 分岐の image view ladder 155 行、65 行が byte 一致。fullscreen 側にだけ `separate_view_layers` があり**既にドリフト**。`DescriptorVariant` struct も 2 箇所で同一。
- **DebugDraw / DebugText / Gizmo**(`renderer/`)が同じ private helper 9 個を 3 コピー(`retireRegistrations` は 20 行完全一致)。
- **JSON helper**: `appendUnique` 5 定義 / 4 ファイル(signature 不揃い)、`requireString` 3 実装、`parseStringList` 2 実装。`renderingpassjsonhelpers.hpp` が使われていない。
- **componentcodec**: 宣言的 schema(`:111-196`)は editor 向け metadata にしか使われず、decode/encode は手書き。collider の 10 field が **8 箇所**に書かれ、`radius` 既定値 0.5 の出所が 3 つ。同一ファイルに JSON decode 機構が 4 種。
- **`PreparedSceneState`**(`renderer/polygoninstancecontainer.hpp:405-441`)が本体 28 メンバの手書き shadow copy。1 field 追加で 6 箇所同期、`publishPreparedSceneModel` と `publishModelInstance` は 55 行同一、`prepareModelInstanceRebuild` はメンバと同名のローカル参照を 20 個作って意図的に shadowing。
- **frame の手順**: `appflow/loop.cpp:343-618` で advance/update/render/record が 3 回(headless / interactive / XR)書かれ、XR 側だけ `framerate_adjuster.wait()` を飛ばし flat 側だけ `selectGraphVariant` を飛ばす(意図か事故かコードに書かれていない)。`renderer.cpp:5661-5815` と `5817-6006` も同じ 90 行を view-family / sequential で 2 回。
- **gamelogicreload.cpp** の 7 subsystem provider 列挙が 4 回(rollback 側だけ構造が違う)。
- **physicsruntime.cpp** の query 7 本が同じ骨格、catch 3 連 9 行が 7 回、index 整列 span 4 本を 8 引数で引き回す。
- 小物: `importSceneSnapshot` v1/v2(21 行中差 1 行)、`finalizeJournal` / `finalizeRevertJournal`(20 行一致 + `deduplicate` ラムダ二重定義)、`setInputs` / `setTypedInputs`(vrmapplication、weight 検証 16 行)、`openxrsession.cpp` の `endFrame` 2 回、playback の `*_stub.cpp` が本物の boilerplate をコピー。

### 2.2 神関数 / 神クラス(1 スコープに手順全部)

| 場所 | 規模 | 主な症状 |
|---|---|---|
| `vkcore/renderer.cpp:4840-6068` `renderLogicalFrame` | 1229 行、ラムダ 13(最大 153 行)、第 1 階層ローカル 57 個 | 全ラムダが `[&]` で同じ scope を掴み、どの変数がいつ確定するか読めない |
| `vkcore/renderer.cpp:1444-2395` `executePlannedFrameGraph` | 952 行、**引数 16**、brace 深さ 11 | `executeRenderingPasses`(引数 17)が 13 個をそのまま転送。同型引数の取り違えが型で止まらない |
| `vkcore/swapchainframetarget.cpp:469-2887` `Impl` | 2419 行、`pumpMaintenance` 305 行、`beginFrame` 282 行 | WSI 復旧の状態遷移が 5 段 `else if` の入れ子 |
| `renderer/polygoninstancecontainer.hpp:305-733` | public 109、メンバ約 90、`*ForTesting` 40 | 8 つの独立した関心事(instance / skin / morph / override / ray / draw queue / culling / history) |
| `material/materialcontainer.cpp` | 5159 行、`throw` 147、`prepareRuntimeGenerationReload` 540 行(ラムダ 392 行、並列 vector 5 本、深さ 7) | 既知(v1 計画の整理候補)。index 整列 5 本の不変条件が局所で検査できない |
| `communication/editorjournal.cpp` | 6464 行、匿名名前空間 4374 行に 118 定義、`Impl` はメンバ 27 で状態機械 5 つ | `commitPending` / `commitPendingPreview` 260/271 行、結果公開の 5 行定型が同一関数内に 6 回 |
| `communication/editorruntimefactory.cpp:1272-1870` `materializeEditorProjection` | 599 行、深さ 9、同型 `uint64_t` 引数 3 連続 | 位置引数の取り違えが型で止まらない(呼び出し 2 箇所) |
| `communication/renderconfigeditor.cpp` | public 35、メンバ 16、責務 5 種、`catch` 43 箇所 | `addAuthoredPass` / `removeAuthoredPass` が約 50 行同一(24 行 + 26 行が正規化後完全一致) |
| `loader/scene.cpp:546-1217` `SceneLoader::load` | 672 行、候補変数 30、rollback ラムダ 52 行、fault 注入分岐 12 | `EditorProjectionAdapter` という受け皿があるのに使わずベタ書き |
| `loader/resolvedscene.cpp:186-409` `resolveComponent` | 224 行、うち camera 特例 115 行 | 汎用 resolver に `if (name == "camera")`。loader 内で `"camera"` 39 回 `"transform"` 30 回の文字列比較 |
| `renderingpass/computetask.cpp` | 3947 行、`registerComputeTask` 414 行、GLSL が関数内に直書き(3171-3176) | class 境界は既にあるので 3 TU に割るのは機械的 |
| `renderingpass/renderingpassconfigregistration.cpp:676-1302` | 627 行、深さ 7、`bool include_preview` が 4 箇所で挙動を切替、`dependencies` の shadowing(1079) | GPU 確保 + transaction + rollback が 1 関数。**分割の等価性証明が高いので後回し** |
| `renderingpass/rendertargetcontainer.cpp:409-762` `registerRenderTarget` | **引数 18**(default 9) | 呼び出し側は `RenderTargetDefinition` を 18 個にバラして渡している(受け皿が既にある) |
| `renderingpass/rendertargetcontainer.hpp:153-212` | image view accessor 17 個 | `bool array_view` 版は enum 版への劣化 shim(cube を表現できない)。移行が途中で止まっている |
| `animation/animationservice.cpp:47-1795` `Impl` | 1749 行、nested struct 11、`unordered_map` 14、メンバ関数 55 | asset registry + handle table + pose arena + phase scheduler + C-ABI shim |
| `openxr/openxrcompositiontarget.cpp:471-1302` `Impl` | 832 行、frame 状態が**独立 bool 12 個** | `resetFrameFlags` が 7 個を覚えて消す。enum 1 個と遷移関数に置換可能 |
| `model/gltf.cpp:892-2645` `InternalGltfLoader` | 1754 行、メソッド 27、可変メンバ 32、`loadMesh` 342 行に `#if PELICAN_WITH_VAT` 4 箇所 | 一部の `#if` が `else if` chain の途中を開くので build 構成で chain の形が変わる |
| `shader/raygrouplayout.cpp:1794-2249` | 456 行、`profile_active` 1 bool で 2 layout、2182-2218 の `else` 本体が `if` と同じ indent | 6 phase 1 scope |
| `userpublic/gamecontext.hpp:26-142` | public 65、領域 18、エラー規約 5 通り | **公開 API**なので固まる前に触るべき唯一の場所。`raycastClosest` は同名 overload で戻り値型が違う、`overlapAll` は名前の vector を返す |

### 2.3 エラー規約の多重化

- loader: 型なし例外 170(`runtime_error` 89 / `invalid_argument` 54 / `logic_error` 27)+ ドメイン例外 8 種 + 結果 variant 4 種 + `ResolutionResult<T>` + `optional` + `bool`。`SceneLoader::load` は最終的に `catch (...)` で文字列に潰す。
- communication: 8 種類のチャネル。「不正な component 値」は `EditFailure` → `rejected()` JSON → `EditorCommandError` → `JsonRpcHandlerError` と **4 回詰め替え**られて wire に出る。`"render config editor service is unavailable"` が 29 回、`"editor RPC adapter is read-only"` が 34 回(4 層の素通し委譲)。
- renderer: 同一 class 内で例外 / 結果 struct / `bool` + out-param `noexcept` の 3 系統(`reloadRenderPipelineFromDisk` vs `applyRenderPipelineAuthoringCandidate`)。
- gamecontext: `bool` / `optional` / `Animation::Status` / 例外 / 無報告 `void` の 5 通り。無効 build では `#else return {}` で**黙って空**(22 箇所)。
- 対照的に良いもの: renderingpass(内部 throw / ABI 境界 Status / noexcept 境界 terminate の 3 層が崩れていない)、animgraph.cpp の Status 一貫返却、shader/ 全体。

### 2.4 その他

- **`*ForTesting` が production header に 219 個**(`PolygonInstanceContainer` 40、`Renderer` 12、`MaterialContainer` 14、`ProjectBasicConfig` に fault / hook 注入)。`friend struct XTestAccess` 1 個に集約できる。
- **thread_local の暗黙コンテキストが 7 個**(componentcodec 2、effecttransaction、scene、registerer、registrationowner、deterministicrng)。`SceneLoader::load` は 4 つを同時に install。game DLL の callback が読むのでコールスタックに現れない依存。
- **`ReloadParticipant`**(`watch/reloadservice.hpp:95-131`)が `std::function` 10 個で apply 経路 4 通りが相互排他に共存、`registerParticipant` が 72 行の実行時 validator。`applyRequests` は index 整列 vector 4 本を 4 pass で読み書きし、shader / material を**名前で特別扱い**。`claimants` は `participants_` への生ポインタ。
- **`RpcServer::handleLine`** が gamelogic の process global atomic を直接読み(1097)、応答 JSON 全体を `"resolution_error"` key 探索で再帰走査して serialize 経路を切り替える(1109-1129)。
- **`pelican_sets.hpp`**: `PELICAN_RAY_QUERY_TLAS_BINDING = 6` と `PELICAN_MATERIAL_BUFFER_BINDING = 6`、`LIGHT_UBO = 2` と `SKIN_PALETTE = 2` が衝突し、どの set に属するかが書かれていない。ray table の set 3 / binding 0,1,2 / 1024 が GLSL 文字列と C++ literal の 4 箇所に独立に書かれ、`rayBindlessTextureCapacity` を変えても GLSL は 1024 のまま。
- **書式**: `.clang-format` が無い。`materialcontainer.cpp` は非空行の 47% が 40 桁以下で最長 152 桁、`renderconfigeditor.cpp` は 100 桁超が 188 本で `editorjournal.cpp` は 3 本、`light/` だけ tab + `m_PascalCase` + 次行 brace。行数の指標は 1.5-2 倍に膨らんでいる。**一括 reformat は blame を壊すので、config を置いて触った区間だけに適用**。
- `gameobjects.hpp:9` が公開 header で `<iostream>` を引く(使用 0)。`gamecontext.hpp:20` が `nlohmann/json.hpp` を公開 API に漏らす。`initailCameraProperty` の綴り。
- `renderingsamplecount.cpp`(2548 行)は名前と中身が乖離(実体は target plan compiler)。4 種 assignment の 17 行ブロック × 4 は 2026-07-26 の census_vocabulary で既に指摘済み、未着手。
- `frameplanner.cpp:1235-1237` `passKind` は引数無名で常に `render` を返す死んだ抽象。`openxrmirrorsink.cpp:259-261` は空 block を囲む `ScopedCommandDebugLabel`。`componentcodec.cpp:1464-1481` は consteval で証明済みの一意性を O(n²) で再検査。

## 3. 良好な部分(触らない)

`src/core/ui/`(11 file、global 0、最長 109 行)、`src/core/audio/`、`src/core/asset/`、`src/core/persistence/`(全関数 70 行未満)、
`src/core/phys/physquery*.cpp`、`startup.cpp`、`src/player/main.cpp`、`src/spvlink/main.cpp`、
`shaderreflection.cpp`、`raytracingvalidation.cpp`、`shaderbindingtable.cpp`、`deletionqueue.hpp`、
`swapchainrecovery`、`modelinstanceslots`、`viewfamily`、`projectionjitter`、`render_target_layout_tracker`、
`editorrpchandlers.cpp`(60 件の均質な登録表)、`windowedrpchost.cpp`、`resolutionresult.hpp`、
`structfieldschema.hpp` の consteval DSL(設計は良い、使われ方が狭いだけ)、`vulkannativescopeexecutor.hpp` の型設計。

SPIR-V reader は 3 実装(`rtspirvinspection`、`spvlink`、`shadercompiler` の magic check)+ spirv-reflect だが、
答える問いが disjoint なので統合しない。`archetypemigration.cpp` の publish / rollback 手書き逆操作(assert 20 個、NDEBUG で消える)は
難しさの大半が本質的で、変異テストの体制が無い今は assert の格上げに留める。

## 4. 着手順(価値 ÷ リスク)

原則: **機能を触る WP に「その面の整理」を同梱する**(2026-09-05 利用者方針)。独立の大規模整理は起こさない。
ただし §1 の 1 行修正と、受け皿が既にある差し替えは独立の小 WP でよい。

### 段 A: 小さくて実害を消す(各 1 時間未満、独立 commit)

1. §1-1 `"\0"` → `push_back('\0')`。
2. §1-2 `forceAbort` を publish の前へ(または局所 try)。reason 文字列も実態に合わせる。
3. §1-10 profiler の stdout 出力を止める。
4. §1-9 key parser を `actionmap.hpp` に 1 本出し、rpcserver 側を消す。
5. §1-5 `Status` の数値統一(`abi_common_v1.hpp`)。**ABI 変更**なので v0 tag の前に入れるか後にするかは利用者裁定。
6. §1-6 optional 枝のヘルパ化(encode の検証欠落を塞ぐ)。
7. §1-13 `static forced_transition` をメンバへ。

### 段 B: 受け皿が既にあるものへの差し替え(半日単位、挙動不変、既存 test で確認)

1. `registerRenderTarget` を `RenderTargetDefinition` 受けに(18 引数 → 4)。呼び出し側 本番 2 + test 数箇所。
2. `StableFingerprint` 4 実装 → `StableFingerprint64`。約 140 行減、fingerprint test で等価確認。
3. registry 4 本の ABI helper(`nextGeneration` / `validHandle` / `validByteRange` / `statusName`)共通化。lifecycle 本体(147 行 × 4)は次の段。
4. `materializeEditorProjection` の `expected_*` 3 引数を struct に。
5. `renderconfigeditor.cpp` の CAS 前置き 24 行と ticket 後置き 26 行の抽出(正規化後完全一致を確認済み)。
6. descriptor set 構築の共通 builder(material ↔ fullscreen)。`*ForTesting` の bound image view accessor で等価確認。両側の drift(`separate_view_layers`)をここで揃える。
7. `setInputResourcesById` の並列 vector 5 本 → `FullscreenPassInput` 1 struct(13 引数・default 7 が消える)。6 の下地。
8. `SnapshotProjectionAdapter` template(Camera / Light / Collider 220 行 → 60 行)。
9. `world 回転合成` 1 本化(§1-4)+ 親子 DFS 2 実装の統合。
10. behaviorarena の transaction 統合(§1-11)。ドリフト 2 点はどちらが正か**先に裁定**してから。
11. renderer の publish / rollback ラムダを `RenderPipelineTransaction` のメソッドに(§1-3)。同時に `reloadRenderPipelineFromDisk` の戻り値を `applyRenderPipelineAuthoringCandidate` と同じ結果 struct に揃える。

### 段 C: 状態を型で表す(WP 単位、関連機能を触るときに同梱)

1. `PreparedSceneState` の 28 field → `ModelInstanceSlotTable` 1 値型 + 5 field。snapshot / publish の列挙 3 箇所と `next_*` コピー 50 行が消える。**renderer 側で最も費用対効果が高い**。
2. `componentcodec` の decode / encode を `decodeStructObject` 経由に 1 component ずつ移行(animation で実証済み、legacy_branch corpus が回帰検出)。済めば camera 特例 115 行も codec 側へ移り、loader の文字列比較 130 箇所が削れる。
3. `XrCompositionTarget` の bool 12 個 → `FrameStage` enum + 遷移関数。
4. `animationservice` の callback を lock 外へ + 例外→Status の明示対応 + `releaseOwner` の compaction(§1-7)。分割(PoseArena / SinkRegistry / PhaseScheduler)はその後。
5. `posegraphruntime` の `exception_ptr` 保持(§1-8)。
6. `Loop::run` を `FrameStep` 1 struct で 3 loop 共通化。XR / flat の差(wait、selectGraphVariant)を**宣言**にする。
7. `ReloadParticipant` の 4 経路 → `std::variant`。`registerParticipant` の 72 行 validator の大半が型で消える。
8. `GameContext` を `physics()` / `audio()` / `animation()` / `save()` facade に分割、重複 pair(`raycastClosest` 型違い overload、`overlapAll` 名前返し、`setGraphParameter` 二系統)を片方 `[[deprecated]]`。**v0 で公開 API を凍結する前に**。
9. `executePlannedFrameGraph` の引数 16 → `FrameGraphExecutionRequest` 1 struct。
10. `computetask.cpp` を 3 TU に(class 境界は既にある)。GLSL 直書きを `engine://` へ。

### 段 D: 神関数の分割(価値は最大、単独では危険。C の後)

- `renderLogicalFrame`(ラムダ → `LogicalFrameRecorder` の private method、2 分岐を 1 本に)。C-1 で状態が型になってから。
- `SwapchainFrameTarget::Impl`(`pumpMaintenance` の 3 分岐を 3 method に、worker を `SwapchainBuildWorker` へ)。
- `editorjournal.cpp`(`prepare*` 1200 行と unpack 整合性検証 300 行を別 TU へ、`Impl` から preview lease 8 field を抽出)。
- `SceneLoader::load`(参加者を `EditorProjectionAdapter` 型に載せ、driver 60 行に)。B-8 の後。
- `materialcontainer.cpp`(`MaterialTextureStore` / `MaterialDescriptorBuilder` / `MaterialGenerationPlanner`)。既知。
- `registerRenderingPassConfigVariantsData`(627 行)。GPU 確保 + transaction なので**最後**。
- `AnimationServiceRuntime::Impl` の 3 分割。C-4 の後。

### やらない

- 一括 clang-format(blame を壊す)。config を置いて触った区間だけ。
- SPIR-V reader 3 実装の統合。
- `archetypemigration.cpp` の書き換え(assert の格上げのみ)。
- 2026-07-17 で非推奨と裁定済みのもの(matrix transport 一本化、generic history storage、debug text GPU path 全面統合)。

## 5. この監査の限界

- 静的監査。build も test も走らせていない。行数 / 重複行数 / 深さは agent の実測で、Fable が再確認したのは §1 の 13 件。
- 「複雑すぎる」の閾値は関数 200 行 / 深さ 4 / 重複 20 行を目安にした。この repo は `.clang-format` が無く折り返しが極端に短い区間があるので、行数は 1.5-2 倍に膨らんで見える。順位はそれを割り引いて付けた。
- `src/devstudio` と `test/` は見ていない。test の `catch → SKIP`(gotchas §9.20)は別の問題として既知。

## 6. 段 B 以降の詳細(2026-09-18、利用者の求めに応じて)

各項目は「今の形 → 変える形 → 何で確認するか → リスク → 何が楽になるか」の順。
行番号は `990b9eaa` 時点。B は挙動不変の機械的差し替えなので独立 commit、C 以降は関連機能を触る WP に同梱する。

### 段 B: 受け皿が既にあるものへの差し替え

**B-1 `registerRenderTarget` を `RenderTargetDefinition` 受けに**
`rendertargetcontainer.cpp:409-762` は引数 18 個(default 9、`bool history` と `uint32_t samples` が隣接)。呼び出し側 `rendertargetconfigregistration.cpp:164-181` は `RenderTargetDefinition` を持っているのに 18 個にバラして渡す。`renderer_config.cpp:381-386` の XR mirror は 9 個だけ渡して残りは default。
→ `registerRenderTarget(const RenderTargetDefinition &, vk::Extent2D base_extent, vma::MemoryUsage, std::optional<uint64_t> alias_group_token)`。XR mirror はその場で `RenderTargetDefinition` を組む。旧 signature は削除(残すと 2 経路になる)。
確認: `rendertarget*` / `frameplan*` / `headless_render` の非 GPU test。呼び出し側は本番 2 + test 数箇所なので compile が落ちれば全部見つかる。
リスク: ほぼ無し。default 引数に頼っていた呼び出しが「何を default にしていたか」を明示する形になるだけ。
効果: 以後 render target に field を足す WP が 1 箇所の struct 追加で済む。RT 系の全 WP が軽くなる。

**B-2 `StableFingerprint` 4 実装 → `StableFingerprint64`**
`graphtransformregistry.cpp:184`、`subgraphreplacementregistry.cpp:182`、`renderstrategyregistry.cpp:228`、`passimplementationregistry.cpp:191` の class が `src/project/stablefingerprint.hpp:18-44` と一字一句同じ(整形差のみ)。4 ファイルとも `../../project/` 配下を既に include している。
→ 4 class を削除し `using StableFingerprint = Project::StableFingerprint64;`(または直接置換)。
確認: registry 4 本の test、`render_policy_dll_test`、WP382a frozen。fingerprint 値は変わらない(同じ FNV-1a、同じ append 順)。
リスク: 無し。約 140 行減。
効果: fingerprint は再現性契約の識別子なので、4 実装が将来ずれる可能性を消す。

**B-3 registry 4 本の ABI helper 共通化**
`nextGeneration` / `validHandle` / `validByteRange` / `validProvider` / `abiSize` / `statusName` が 4 ファイルに同一実装(70-90 行 × 4)。
→ `renderingpass/providerregistrycommon.hpp` に template(Status 型と ApiV1 型を template 引数に)。lifecycle 本体(`registerProvider` / `unregisterProvider` / `snapshot` / `activateOwner` / `releaseOwner` の 147 行 × 4)は**この段では触らない**。owner retire の規則は hot reload の安全性の中核で、「削った分岐は誰もテストしない」がそのまま当てはまる。
確認: B-2 と同じ family。
リスク: 低。helper は純関数。
効果: 約 300 行減。次の段で lifecycle を `ProviderRegistry<Traits>` にする下地。

**B-4 `materializeEditorProjection` の `expected_*` 3 引数 → struct**
`editorruntimefactory.cpp:1272-1280` の `expected_binding_epoch` / `expected_codec_generation` / `expected_behavior_generation` は同じ `uint64_t` で 3 連続。呼び出し側(2165、2213)は位置引数。
→ `struct EditorProjectionExpectation { uint64_t binding_epoch; uint64_t codec_generation; uint64_t behavior_generation; }` を designated initializer で渡す。
確認: `editorjournal_test`、`inspector_test`、`wp381` / `wp382a` 系。
リスク: 無し。599 行の本体分割は**やらない**(D で)。
効果: generation 不一致の拒否が「引数の順番」でなく「名前」で決まる。

**B-5 `renderconfigeditor.cpp` の CAS 前置き / ticket 後置きの抽出**
`1297-1320` と `2383-2406`(24 行、正規化後完全一致: resolver 有無 → digest 形式 → gate → base digest CAS → disk 再読込 CAS)、`2521-2547` と `2654-2680`(26 行完全一致: preflight → ticket enqueue → accepted 応答、差は `.operation` 文字列 1 つ)。3 箇所目が 2567 付近に近似形。
→ `std::optional<OrderedJson> checkAuthoringPreconditions(params, method, base_digest)`(拒否 JSON を返すか nullopt)と `OrderedJson enqueueAuthoredPassTicket(operation, root, candidate_documents, reference, gate, base_digest)`。`addAuthoredPass`(208 行)/ `removeAuthoredPass`(128 行)/ `queueRootEdit` が半減。
確認: `renderconfigeditor` の test 6 件、Studio 側の authored pass test(WP334b)。
リスク: ほぼ無し。完全一致を機械的に確認済み。
効果: 拒否理由コードの追加・変更が 1 箇所になる。Studio の編集面(N 系)が触る場所なので N 系の WP が楽になる。

**B-6 descriptor set 構築の共通 builder(material ↔ fullscreen)**
`materialcontainer.cpp:4370-4528` と `fullscreenpasscontainer.cpp:653-826`。`for (variant) for (parity < 2)` → descriptor set 確保 → 5 分岐の image view ladder(explicit subresource / cube / layered / frame / layer)→ `DescriptorImageInfo` → `bound_image_views[parity]` → `updateDescriptorSets`。65/158 行が byte 一致、cube 分岐は変数名以外同一。`ScreenInputDescriptor::DescriptorVariant`(materialcontainer.hpp:166-170)と `InputTextureInfo::DescriptorVariant`(fullscreenpasscontainer.hpp:33-37)も同一。**fullscreen 側にだけ `separate_view_layers`(687-696)があり既にドリフト**。
→ `PassInputDescriptorSetBuilder`: 入力は `std::span<const ResolvedPassInput>`(target、history、subresource、view_dimension、descriptor_dimension、sampler-or-local-read)+ resolver、出力は `std::vector<DescriptorVariant>`。両側は自分の検証と `ResolvedPassInput` への変換だけ残す。**先に `separate_view_layers` を material 側にも要るか裁定する**(要るなら builder に入れる、要らないなら fullscreen 固有として builder の外に出す)。
確認: 両側の `*ForTesting` の bound image view accessor(materialcontainer.hpp:456、fullscreenpasscontainer.hpp:133)が既にあるので、抽出前後で同じ view handle 列になることを直接比較できる。GPU test: `fullscreen*`、`material*`、golden。
リスク: 中。layer index / history parity / local-read layout は最もバグが出やすい部分。ただし現状は「2 箇所で別々に壊れる」状態で、1 箇所にする方が安全。
効果: 新しい view 種(次に cube array など)を足すときの編集箇所が 1 つになる。

**B-7 `setInputResourcesById` の並列 vector 5 本 → struct 1 本**
`fullscreenpasscontainer.cpp:347-839`、引数 13(default 7)。`input_rt_history` / `input_sampling` / `input_views` / `input_subresources` / `input_view_dimensions` が「空か `input_rts.size()` と同じ長さ」の不変条件で、5 個の同型検査(381-406)と 5 個の default 展開(434-476)を手書き。`setInputResources`(hpp:97-111)が `std::string` 版の双子。
→ `std::vector<FullscreenPassInput>`(target、history、sampling、view、subresource、view_dimension、local_read)1 本。長さ不変条件が構造で成立し、検査 5 + 展開 5 が消える。その上で validation / variant 数導出(536-651)/ descriptor 構築(652-826 = B-6 の builder)に分ける。
確認: B-6 と同じ。compile が通れば全呼び出し側が変換済み。
リスク: 低。default 引数 7 個に頼っていた呼び出しが明示になる。
効果: B-6 の builder が欲しい入力型そのもの。**B-7 → B-6 の順**が良い。

**B-8 `SnapshotProjectionAdapter` template**
`editorprojectionadapters.cpp:1204-1255`(Camera、52 行)と `1257-1309`(Light、53 行)は差分 13 行。`Impl` の field 型、`kind()` の enum、`name()` の prefix、`publicationMode()`、`prepare()` の 2-3 行だけ違う。`rollback()` / `finish()` / dtor / `name()` は Collider を含む 3 class で同一。同型 boilerplate が 590-604 / 830-841 / 951-961 / 1060-1071 にもあり、6 class × 4 = 24 個の定型関数。
→ `template <class Module, class Prepared, EditorProjectionAdapterKind Kind, EditorProjectionPublicationMode Mode> class SnapshotProjectionAdapter` に `old_state / next_state / published` 三点セットと rollback / finish を置き、`prepare` だけ CRTP フックか lambda で差す。Camera / Light は 5 行の派生。
確認: `editorprojection*` / `editorjournal_test` / `inspector_test`。挙動完全同一。
リスク: 無し。
効果: 参加者を足すたびの 50 行写経が消える。**D の `SceneLoader::load` を adapter 列に載せ替える下地**。

**B-9 world 回転合成の 1 本化 + 親子 DFS の統合(§1-4)**
`componentcodec.cpp:423`(正規化なし)vs `editorpreviewprojection.cpp:146, 155`(local も合成も正規化あり)。DFS は `scene.cpp:926-956`(0/1/2 の magic number)と `editorpreviewprojection.cpp:292-323`(`enum class Visit`)、`editorprojectionadapters.cpp:275-340` に 3 つ目。
→ `composeWorldTransform(parent, local)` を componentcodec 側に 1 本(正規化の有無は**ここで 1 回決める**。どちらに揃えるかは裁定: runtime に合わせて「正規化しない」なら非正規化 quaternion を書いた scene は loader で拒否する方が筋。preview に合わせるなら runtime の world 回転が微小に変わり golden が動く)。DFS は `resolveHierarchy(objects, get_parent, compose)`。
確認: golden(GPU)、`scene_test`、preview projection test。**正規化の裁定によって golden の更新が要る**。
リスク: 中(golden が動く可能性)。
効果: 「web ⊆ pelican」の前提を数式 1 本に戻す。

**B-10 behaviorarena の transaction 統合(§1-11)**
`behaviorarena.cpp:278-451`(edits)と `503-734`(reconciliation)。62 行逐語一致 + 約 130 行同型。**ドリフト 2 点は先に裁定**: (a) publish で params update を全件適用(edits 394-403)か `reused` だけ(reconciliation 622-624)か、(b) rollback で swap→transfer を全件ずつ(edits 412-438)か handle ごと交互(reconciliation 708-711)か。
→ `AttachmentTransaction<Policy>` に `detached / reused / params_updates / published / finished` と helper 5 個(`ParamsUpdate`、`transferRuntime`、`swapParamsAndActivation`、`releasePreparedParams`、`destroyDetached`)。2 利用者は追加 state(handle counter、`activated`、`deferred_base`)と publish / rollback hook だけ。
確認: `behavior*` / `gamelogicreload*` / scene load の rollback test。
リスク: 中。scene load の rollback 経路。裁定なしに統合すると「どちらかの挙動が黙って変わる」。
効果: rollback の順序修正が 1 箇所になる。

**B-11 renderer の publish / rollback を `RenderPipelineTransaction` のメソッドに(§1-3)**
`renderer.cpp:3364-3428` vs `3642-3705`(publish 63 行)、`3430-3455` vs `3707-3731`(rollback 25 行)。`finalized` の位置が逆、`graph_runtime_transfer->commit()` が B にだけ。
→ transaction に `publish()` / `rollback()`。差分(graph transfer commit の有無、失敗 message)は field か hook。同時に `reloadRenderPipelineFromDisk(std::string &error, ..., uint64_t *staged_generation)` の `bool` + out-param を `applyRenderPipelineAuthoringCandidate` と同じ `RenderPipelineAuthoringApplyResult` 戻り値に揃える(呼び出し側は watch / reloadservice と RPC)。
確認: `reloadtransaction_test`、`materialvaluesreload_test`、renderconfig の apply 系、hot reload の GPU test。
リスク: 低〜中。`commit()` の有無が意図かどうかを**先に確認**(reload from disk では graph transfer を作らないなら null で通るはず)。
効果: hot reload の 2 経路が同じ規則で publish される。

### 段 C: 状態を型で表す(WP 同梱)

**C-1 `PreparedSceneState` → `ModelInstanceSlotTable`**
`polygoninstancecontainer.hpp:405-441` は本体 private メンバ 28 個の手書き shadow copy。1 field 追加で 6 箇所同期(struct 定義 / `snapshotPreparedScene` 31 行 / `publishPreparedScene` 28 行 swap / `publishPreparedSceneModel` と `publishModelInstance` の 4 block / `prepareModelInstanceRebuild` の alias 20 + `next_*` コピー 15 + move-back 15)。`publishPreparedSceneModel`(640-698)と `publishModelInstance`(850-903)は `prepared.` 接頭辞と assert 以外同一。`prepareModelInstanceRebuild` はメンバと同名のローカル参照 20 個で意図的 shadowing。
→ slot 並列配列 16 本(`model_instances_data` 〜 `morph_history_valid`)を `ModelInstanceSlotTable` 値型 1 個に。`PreparedSceneState` は `{ slots, draw_inventory, compiled_draw_queue, ray_*, overrides }` の 5-6 field。snapshot / publish は既定コピーと `swap`。
確認: `polygoninstance*` / `modelhotreload_test` / skinning・morph の GPU test / golden。「publish 後に slot index が 1 本だけ古い」を検出する test を 1 本足す(変異: 16 本のうち 1 本を swap から外す)。
リスク: 中。純粋な機械変換だが触る面が広い。**renderer 側で最も費用対効果が高い**。
効果: 同期漏れがコンパイルエラーになる。D の `renderLogicalFrame` 分割の前提。

**C-2 componentcodec を宣言的 schema 経由に(1 component ずつ)**
`componentcodec.cpp:111-196` の schema は `materializeSchema`(209-214)で editor 向け metadata にしか使われず、decode / encode は手書き(383-1298)。`decodeStructObject` を使っているのは animation `parameters`(998)と ray override(1170)だけ。collider は 10 field が 8 箇所(schema、`requireClosed` の名前 list、`normalized` 既定値、`encodeCollider`、`collider.hpp` の struct 既定値、binary archive `ref`、`loadFromJsonArchive`、`validate`)。
→ transform → light → sprite → camera → collider の順に(cross-field 検証が少ない順)`decodeStructObject` / `encodeStructObject` へ。手書きは cross-field 検証(light の spot 限定 field、camera の nested projection)だけ残す。`requireClosed` の list は schema から生成。済んだら `resolveComponent`(resolvedscene.cpp:274-388)の camera 特例 115 行を `ComponentCodec::completeDefaults(value, defaults)` として codec 側へ。
確認: `componentcodec*` の legacy_branch corpus(到達性 corpus が既にある)、`schema_*` test、scene save/load の round trip。
リスク: 低(1 component ずつ、corpus が回帰検出)。
効果: field 追加が 1 箇所。editor が見せる schema と runtime が受け付ける JSON が同じ定義から出る。loader の `"camera"` 39 回等の文字列比較 130 箇所が削れる。

**C-3 `XrCompositionTarget` の bool 12 個 → `FrameStage` enum**
`openxrcompositiontarget.cpp:506-513` の `frame_prepared / logical_frame_begun / logical_frame_submitted / view_family_begun / view_family_ended / view_family_submission_complete / depth_submission_active / teardown_required` + view ごと 3 + swapchain ごと 1。`resetFrameFlags`(725-738)が 7 個を覚えて消し、`abortFrame`(768-829)が 6 個を読んで unwind を決める。
→ `enum class FrameStage { idle, prepared, begun, views_recorded, submitted }` と `transition(from, to)` 1 関数(不正遷移は throw)。`VulkanXrCompositionGraphics`(99-460、362 行)は別 TU へ。
確認: XR の test(`vrm_xr_demo_rpc`、XR forced-off の決定性 test、gotchas §9.16 の不変条件)。
リスク: 中(XR は実機がないと動的確認できない。不変条件 test が頼り)。
効果: 不正な組み合わせが起きない。frame protocol に段を足すとき flag と reset の追加が要らない。

**C-4 `animationservice` の lock 外 callback + 例外→Status + compaction(§1-7)**
→ `runPhases`(789-852)で `ordered` の phase list を copy してから lock を解放し、callback(827)は lock 外で呼ぶ(record は tombstone で erase されないので pointer は生きる)。`catch (...)` → `out_of_memory` を `std::bad_alloc` → `out_of_memory`、`std::invalid_argument` → `invalid_argument`、他 → `callback_failed` に分ける。`releaseOwner`(756-787)に 4 map の compaction。`recursive_mutex` は callback が lock 外になれば `mutex` に戻せる(戻すとき再入があれば deadlock で即発見できる)。
確認: `animation_abi_dll_test`、`animgraph_test`、posegraph の GPU test、game logic reload を 100 回回して map size が増えない test を 1 本。
リスク: 中。lock 範囲の変更。「callback 中に service が reset される」経路(hot reload と同時)を先に潰す。
効果: worker thread から callback を呼べる下地。Impl 分割(D)の前提。

**C-5 `posegraphruntime` の `exception_ptr` 保持(§1-8)**
`Attachment` の `failed` bool(346-353)を `std::exception_ptr` に、875 で `std::rethrow_exception`。`framephase.cpp:188-192` も message を潰さず connect する。evaluate(536-797、262 行)の `switch` 5 case を各 `Status evaluateBlend(...)` に分けるのは同時にやってよい(純粋な抽出)。
確認: pose graph の失敗 test で message に port 名が含まれることを assert。
リスク: 低。
効果: graph 作者に「どの port が未接続か」が届く。

**C-6 `Loop::run` の `FrameStep` 共通化**
`loop.cpp:343-618`(276 行、`#if` 6 区間、loop 3 本)。advance / update / render / record が 410-425、546-580、591-610 の 3 回。XR 側だけ `framerate_adjuster.wait()` を飛ばし、flat 側だけ `selectGraphVariant` を飛ばす。402-405 のコメント「One headless frame, spelled once」を他 2 経路が破っている。
→ `struct FrameStep { advance(); update(); render(); record(); }` を 1 つ、`runHeadless()` / `runInteractive()` / `runXrFrame()` は渡すものだけ違う。XR / flat の差 2 点は**宣言**(コメントと flag)にする。`finishLoopResources(..., bool)` の bool に名前を付ける。
確認: `--start-time` の replay(O-1、frame 等価 bytes)、headless RPC smoke、XR forced-off。
リスク: 中。frame phase の順序が変わると再現性が壊れる。O-1 の bytes 比較が gate。
効果: phase の追加・並べ替えが 1 箇所。

**C-7 `ReloadParticipant` の `std::variant` 化**
`reloadservice.hpp:95-131` の `std::function` 10 個 + `optional<RuntimeReloadParticipant>`。`enqueue / apply_batch / apply_with_companions / apply_authored_candidate` の 4 経路が相互排他で共存、`registerParticipant`(60-131)が 72 行・9 rule の実行時 validator。`applyRequests`(843-1022)は index 整列 vector 4 本(`results / claimants / shared_claims / consumed`)を 4 pass で読み書きし、958-985 で shader / material を**名前で**特別扱い。`claimants` は `participants_` への生 pointer(callback 中の register / unregister で dangling)。
→ `std::variant<EnqueueParticipant, BatchParticipant, CompanionParticipant, AuthoredCandidateParticipant>`。`applyRequests` は `struct RequestSlot { const ReloadRequest *req; std::string claimant; bool done; bool ok; }` 1 本。shader / material の特別扱いは「batch owner + companion」で表現し、専用 participant として登録し直す。`claimants` は index で持つ。
確認: `reloadtransaction_test`、`texturereload_test`、`modelhotreload_test`、`materialvaluesreload_test`、shader hot reload の GPU test。
リスク: 中。hot reload の全経路。
効果: validator 72 行の大半が型で消える。参加者の追加が「variant に 1 型足す」になる。

**C-8 `GameContext` の facade 分割 + 重複 pair の deprecate**
`gamecontext.hpp:26-142`、public 65、領域 18、エラー規約 5 通り(`[[nodiscard]] bool` / `optional<T>` / `Animation::Status` / 例外 / 無報告 `void`)。`raycastClosest(ray)` と `raycastClosest(ray, filter)` は同名 overload で戻り値型が違う(`ObjectRaycastHit` vs `RaycastQueryHit`)、`overlapAll(shape)` は名前の vector で `overlapAllHits(shape, filter)` は hit の vector、`setGraphParameter`(`bool`、SceneLoader の graph runtime)と `setAnimationGraphParameter`(`Animation::Status`、AnimationSystem)が別 subsystem。無効 build は `#else return {}` で黙って空(22 箇所)。
→ `ctx.physics()` / `ctx.audio()` / `ctx.animation()` / `ctx.save()` / `ctx.render()` の小 facade を返す形。既存 65 method は当面 facade への inline 転送で残し `[[deprecated]]`。重複 pair は片方を deprecate。無効 build は「黙って空」でなく `Status::unavailable` 相当を返すか、facade 自体を `optional` にする。
確認: corpus の game logic DLL 全部(`run_project_catalog_smoke`)、`game_logic_abi_test`、frozen v1/v2 の compile 互換(`schema_leaf_consumer` 等)。
リスク: 中。**公開 API**。deprecate で 1 版残すなら v0 で入れ、削除は v1。
効果: 「どの API を使うべきか」が名前空間で分かる。v0 で凍結する前にやる唯一の理由がここ。

**C-9 `executePlannedFrameGraph` の引数 16 → `FrameGraphExecutionRequest`**
`renderer.cpp:1444-1460`(引数 16、default 3、nullable out-param `nlohmann::json *node_trace`、`std::function` 2)。`executeRenderingPasses`(2397、引数 17)が 13 個をそのまま転送。`view_index` と `logical_view_count` はどちらも `uint32_t`。
→ struct 1 個(designated initializer)。`executeRenderingPasses` は依存構築だけして request を渡す。本体 952 行 / 深さ 11 の分割は D。
確認: golden、`headless_render_test`、frame plan の trace 比較 test(`execution_tracing_for_testing`)。
リスク: 低。
効果: 位置引数の取り違えが型で止まる。D の分割で「何が入力か」が見える。

**C-10 `computetask.cpp` を 3 TU に + GLSL 直書きの外出し**
3947 行。`FrameGraphResourceContainer`(buffer)、`parseFrameGraphBufferDefinitionsFromJson`(1928-2140)、`ComputeTaskContainer`、`createDescriptorSet`(2699-2924、引数 8)、`registerComputeTask`(3006-3419、414 行)、`makeComputeResourceInterface`(466-808、343 行)。3171-3176 に no-op closest-hit の GLSL が文字列直書き。§1-12 の `GET_MODULE` vs 注入の二重化もここ。
→ `framegraphbuffercontainer.cpp` / `computetaskdefinitionjson.cpp` / `computetaskcontainer.cpp`(class 境界は既にある)。`registerComputeTask` から `resolveRayTracingShaders()`(3149-3266)と `createTaskDescriptorSets()`(3344-3369)を抽出。GLSL は `engine://` resource(隣の `engine://procedural_no_report` が既にその形)。`ComputeTaskRuntimeDependencies` の `render_target_container` / `frame_graph_resources` は**消して**「このクラスは global module 前提」と明示(実態に合わせる方が安全)。
確認: compute task の全 test(`computetask*`、WP369 typed task、ray callable)、golden。
リスク: 低(TU 分割は機械的)。
効果: compute / ray / reload / multiview が交差する場所で「自分の変更がどの前提を壊したか」が読める。

### 段 D: 神関数の分割(C の後)

順番と前提だけ。各 1 WP。

- **`renderLogicalFrame`**(1229 行): 13 個の `[&]` ラムダを `LogicalFrameRecorder` の private method に(capture をメンバに昇格)。5661-5815(view family)と 5817-6006(sequential)の同じ 90 行を `recordOneExecution(context, view_index, view_count, is_view_family)` に。**C-1 の後**(state が型になってから)。gate は golden + trace 比較。
- **`SwapchainFrameTarget::Impl`**(2419 行): `pumpMaintenance`(305 行)の completion 3 分岐を `publishCompletedEpoch()` / `deferStaleCandidate()` / `handlePreparationFailure()` に(純粋抽出)。worker(`workerMain` / mutex / request / completion)を `SwapchainBuildWorker` へ。`deferRetry` の `1` / `30` を定数に。gate は WSI 復旧の test(`swapchainrecovery`、device-lost 注入)。
- **`editorjournal.cpp`**(6464 行): `prepare*` family(1689-2900)と unpack 整合性検証(4108-4400)を別 TU へ(どちらも `Impl` に触らず、入力は `Json` + `AuthoringSceneDocument`)。`commitPending` / `commitPendingPreview` の結果公開 5 行定型を `publishResult` / `publishFailure` に(§1-2 の修正で一部済む)。`Impl` から preview lease 8 field を `PreviewLeaseState` へ。12 択 variant の手展開 6 箇所には `else static_assert` を置く。
- **`SceneLoader::load`**(672 行): 参加者(renderer / procedural / light / …)を `EditorProjectionAdapter` と同型の `prepare / publish / rollback / finish` に載せ、`load` は「順に prepare → publish → commit、失敗時は逆順 rollback」の driver 60 行に。fault 注入は参加者の decorator。**B-8 の後**。
- **`materialcontainer.cpp`**(5159 行): `MaterialTextureStore`(texture + sampler + reload handler)/ `MaterialDescriptorBuilder`(B-6 の builder + pool)/ `MaterialGenerationPlanner`(`validateRuntimeGenerationCompatibility` / `prepareRuntimeGenerationReload` / `materialBindingsForGeneration`、既に free static)。`prepareRuntimeGenerationReload`(540 行)は `planScreenInputAbi` / `planResourcePortAbi` の 2 関数に。既知(v1 計画の整理候補)。**B-6 / B-7 の後**。
- **`registerRenderingPassConfigVariantsData`**(627 行): `prepare → allocate → registerVariants → publish` の 4 段。`include_preview` は flag でなく variant list。`dependencies` の shadowing(1079)は先に rename。GPU 確保 + transaction なので**最後**。
- **`AnimationServiceRuntime::Impl`**(1749 行): `PoseArena` / `SinkRegistry` / `PhaseScheduler` の 3 分割、C-ABI shim は薄い転送層に。**C-4 の後**。
- **`InternalGltfLoader`**(1754 行): `GltfAccessorReader`(546-890、既に独立 struct)を別 TU + test。`loadMesh`(342 行)の per-primitive 本体を free function に、`#if PELICAN_WITH_VAT` 4 箇所を 1 つに。

### 進め方の注意

- B は「挙動不変」が主張なので、**抽出前後で同じ値になることを既存の `*ForTesting` accessor で直接比較**する(B-1、B-6、B-11)。golden だけに頼らない。
- 裁定が要る 4 点は着手前に決める: B-6 の `separate_view_layers`、B-9 の正規化、B-10 のドリフト 2 点、B-11 の `commit()` の有無。
- 1 WP 1 段。B の項目は独立なので 2 つまで並走可(worktree 別)。C 以降は直列。
- `.clang-format` は B-1 のような小さい WP の**最初の commit**で config だけ置く。触った区間にだけ掛ける。

## 7. 段 A の実施記録(2026-09-18)

| # | 変更 | 備考 |
|---|---|---|
| A-1 | `raygrouplayout.cpp` geometry row digest の NUL 区切りを `push_back('\0')` に | digest 値が変わる。pin している test は無い(rt2b:1176 の hash は GLSL include 文字列) |
| A-2 | `editorjournal.cpp` `commitPending` / `commitPendingPreview`(stale_revision 経路)で `forceAbort` を結果公開の**前**に移し、失敗は `preview_abort_error` field として結果に添付 | `commit_failed` 経路は公開前なので無害、据え置き。test 1 本追加(WP375c) |
| A-3 | `profiler.cpp` の stdout 出力 5 箇所を削除 | logger 無しでは何もしない |
| A-4 | `keyboardControlToKey` を `actionmap.hpp` で公開し、`rpcserver.cpp` の 85 行コピーを 1 行転送に | 正規化 diff 0 行を確認。定義は `actionmap.cpp` の無名 namespace(15-784 行)の中にあったので外へ出した(agent の「無名 namespace ではない」は誤りで、link で発覚) |
| A-5 | render provider ABI 5 本の `Status` は**番号を変えない**。`gamelogicreload.cpp` に `static_assert` 11 本、5 header に comment、`v0_freeze.md` §1.4 に行追加 | frozen fixture の render header は休眠(どの fixture も include していない)で、これまで機械的 guard は無かった。番号統一は `abiVersionV1` を上げる別 WP |
| A-6 | `structfieldjson.hpp` の optional / 非 optional 枝を共通ヘルパに。optional vec の encode 検証欠落を修正 | frozen fixture は不変 |
| A-7 | `animgraph.cpp` の関数内 `static TransitionV1` を `EvaluatorV1::Impl` のメンバに | 2 行 |

## 8. 段 B の実施記録(2026-09-19)

| # | 変更 | 備考 |
|---|---|---|
| B-1 | `registerRenderTarget` を `RenderTargetDefinition` 受けに(18 → 4 引数)。`physicalRenderTargetHistoryClearColor` は definition 層へ下ろし、依存を definition → container → config registration の一方向に。`.clang-format` を config だけ追加(触った区間にのみ適用する方針) | 呼び出し側は本番 2 + test 7 箇所 |
| B-2 | `StableFingerprint` 4 実装を `StableFingerprint64` に置換 | 空白正規化で byte 一致を確認。値は不変 |
| B-3 | registry 4 本の ABI helper(`nextGeneration` / `validHandle` / `validByteRange` / `validProvider` / `abiSize` / `statusName`)を `providerregistrycommon.hpp` に | lifecycle 本体は未着手(D)。net −218 行 |
| B-4 | `materializeEditorProjection` の `expected_*` 3 引数を `EditorProjectionExpectation` に | test 側にも呼び出し 8 箇所(監査の見落とし) |
| B-5 | `renderconfigeditor.cpp` の CAS 前置き(3 箇所)と ticket 後置き(2 箇所)を helper に | remove 経路の disk CAS 拒否だけ digest payload を欠いていたのはドリフトと裁定し揃えた(意図的 wire 変更 1 件)。root CAS の remove 経路は test が無かったので 1 case 追加 |
| B-7 | `setInputResources` / `setInputResourcesById` の並列 vector 5 本を `FullscreenPassInput` に(13 → 7 引数)。descriptor 構築を別関数に抽出 | 部分的に埋まった authored 配列の拒否は compiler 側 helper に残した |
| B-6a | `PassInputDescriptorVariant` を共有 header に、cube 分岐と write / update の tail を `passinputdescriptorvariant.{hpp,cpp}` に共有。material の `boundScreenInputImageViewsForTesting` に `view_index` | **ladder 本体は共有しない**(下の 13 点の差。同じものではなかった)。`WriteDescriptorSet` の pointer 取得を `submit()` 時に移し、`reserve` 依存を消した |
| B-8 | Camera / Light / Collider adapter を `SnapshotProjectionAdapter<Policy>` に | 重複していた 21 関数本体 → 7。SceneObject / BehaviorScene / EcsCodec / RendererModel は形が違うので対象外 |
| B-9 | world 回転合成を `composeWorldTransform` 1 本(正規化あり)に統一。DFS は機構(tri-state walk)だけ共有し、訪問集合と error 語彙は呼び手ごとに維持 | **`projects/example` の 31 object が非正規 quaternion(\|q\|≈0.99985)で、runtime は 0.03% の shear 付きで描いていた。** golden は全部 unit なので動かない |
| B-10 | behaviorarena の transaction 機構を `BehaviorAttachmentTransaction` に統合 | ドリフト 2 点はどちらも edits 側が正(reconciliation 側では到達不能)。test 1 case 追加(同一 handle への update 2 回、attach + set_params) |
| B-11 | renderer の publish / rollback を `RenderPipelinePublicationTransaction` の member に。`reloadRenderPipelineFromDisk` は結果 struct を返す | 差異 2 点は観測不能と確認(`graph_runtime_transfer` は reload 経路で常に null、`finalized` と `outcome` の間に何も走らない)。net −79 |

監査の訂正: `reloadtransaction_test` / `renderconfigeditor_test` は renderer の reload 経路を通らない(watch 層 / config 層の test)。renderer 側の実 coverage は `headless_render_test`、`golden_harness`(reload の `domain_error` を検査する唯一の場所)、`skeletal_gltf_test`。

### B-6b / D の入力: material と fullscreen の descriptor ladder の差 13 点

material = `materialcontainer.cpp` の screen input 構築、fullscreen = `fullscreenpasscontainer.cpp` の `buildInputDescriptorVariants`。5 と 12 は B-6a で解消。

| # | material | fullscreen | 判定 |
|---|---|---|---|
| 1 | subresource 分岐で退化した `family_2d_array`(base 0 / count 1)を全配列に展開。`base_array_layer += variant` はするが `layer_count` は変えない | `base_array_layer += variant; layer_count = 1`(`separate_view_layers` のとき)。family 展開なし | 両方正当(仕事が違う)。ただし material が base を offset して count を狭めないのは要再確認 |
| 2 | array dispatch の key が `descriptor_dimension == two_d_array` | key が `view == family_2d_array` | **fullscreen が正**。descriptor 次元で family と layered/multiview を混同。material は multiview が無いので露見していない |
| 3 | multiview 分岐なし(`GraphicsPipelineViewContract` は material 経路に来ない) | `multiview && layered_2d_array` 分岐あり | 両方正当。共有 builder は `logical_view_count` を optional で受ける |
| 4 | 分岐順: subresource → cube → array → shared_2d → else **layer view** | subresource → cube → family → multiview → separate layers → else **full view** | 等価だが同一でない(`getImageViewForFrame` = `getImageLayerViewForFrame(…, 0, …)`)。variant 0 では一致、6 の場面だけ分かれる |
| 6 | `separate_view_layers` の fallback なし。sequential/layered input の `arrayLayers` が `variant_count` と**厳密一致**しないと throw | `selected_layers < variant_count` または `variant_count == 1` なら 1 layer を全 variant で再利用 | fullscreen が高機能、material が保守的。どちらも防御可能。capability gap であって silent bug ではない |
| 7 | variant 数は生の `arrayLayers(target)`(subresource を無視)。ladder は subresource を variant で offset | 両方 subresource 込みの `selected_layers` | **fullscreen が正**。subresource 付き sequential input で off-by-one の種 |
| 8 | sampler 4 段(input attachment → material resource → clamp_to_edge の cache → material resource)。`ShaderResourcePortSampling` | `input_samplers.at(samplerIndex(sampling))`。`FullscreenInputSampling` | 両方正当、還元不能。builder は sampler resolver を受ける |
| 9 | buffer は image loop 内(`isBuffer() → continue`)、binding は `resource.binding` | buffer は第 2 loop、binding は `inputs.size() + i` | 両方正当。write 層は B-6a で統一、binding の出所は呼び手ごと |
| 10 | write type は保存済み `resource.descriptor_type` | `local_read` から都度導出 | **material が正** |
| 11 | `validImageSubresourceRange` で mip / layer 範囲を検証、error は resource 名付き | 範囲検証なし、error は位置番号 | **material が正**(両方)。fullscreen 側に範囲検証を足すのは builder と独立の小修正候補 |
| 13 | `bindResource` は `logical_view_count == variants.size()` を要求 | `<=` と `view_index < logical_view_count` | 6 と一体で再考する |

→ D の materialcontainer 分割で `MaterialDescriptorBuilder` を作るとき、2 / 7 / 11 は fullscreen 側または material 側の「正」に寄せ、6 / 13 は裁定してから 1 本にする。

## 9. C-2(componentcodec の宣言 schema 化)の実施記録(2026-09-19)

監査 §2.1 / raw の所見 2・4 に対する着手。**5 component のうち宣言経路に移せたのは transform と light の 2 つだけ**で、
残り 3 つは着手前に見えていなかった構造上の壁で止めた。分水嶺は単純で、
**schema が runtime 型そのものに対して宣言されているか、別の `*SchemaFields` 鏡像に対して宣言されているか**である。

| schema | 宣言先の型 | decode の権威 | 判定 |
|---|---|---|---|
| `transform_schema` | `TransformCodecData`(runtime 型) | 同じ | **移した** |
| `light_schema` | `LightCodecData`(runtime 型) | 同じ | **移した** |
| `sprite_schema` | `SpriteSchemaFields`(editor 専用の鏡像) | `SpriteViewComponent::loadFromJsonArchive` | 不可 |
| `camera_schema` | `CameraSchemaFields`(editor 専用の平坦鏡像) | `decodeCamera`(nested / alias / 型推論) | 不可 |
| `collider_schema` | `ColliderSchemaFields`(editor 専用の鏡像) | `ColliderComponent::ref()` | 不可 |

### 移せなかった 3 つの理由(再挑戦する前に読む)

- **sprite / collider: legacy branch が消える。** `sprite_view.*` 20 件と `collider.*` 15 件は
  `SpriteViewComponent` / `ColliderComponent` 自身の parser が産む安定 id で、`decodeStructObject` に通すと
  全部が `*.schema_validation` 1 件に潰れる。`legacy_corpus.json` はこの 35 件を名指しで到達させるので、
  移行は corpus の削除と同義になる。移すなら先に「legacy branch id を宣言 schema 側から出す機構」が要る。
- **camera: schema が wire 形式を記述していない。** `camera_schema` は editor が見せる平坦な 10 field で、
  実際の受理形は nested `perspective` / `orthographic`、flat との二重受理、`xmag` の有無による type 推論、
  alias(`fov_y` / `near` / `far`)の名指し拒否、`controller` の別 decoder を持つ。
  `CameraSchemaFields` は runtime の `CameraCodecData`(全 projection field が `std::optional<float>`)と型が違う。
  schema を書き直せば移せるが、それは editor metadata(`schemaNode()` / `fieldSchema()` / prefab descriptor の
  json pointer 対応表)の変更であり、別 WP。

### やったこと

1. `requireClosed` の名前 list を**全 component で schema から生成**(`closedFieldNames`)。
   dotted な schema 名(`sprite.pixel_perfect`)は最初の `.` までが wire key。
   schema field でない鍵(`name` envelope、nested object、拒否専用の alias `size` / `height`)だけが literal で残る。
   collider の「10 field が 8 箇所」は 7 箇所になった。
2. transform / light の decode と encode を `decodeStructObject` / `encodeStructObject` 経由に。
   手書きで残したのは cross-field 規則だけ: transform は退化四元数(`requireNonZeroQuat`)と
   `rotation_specified` の presence、light は type による presence 規則 4 つと `range > 0` と `inner <= outer`。
3. camera の既定値補完 115 行を `resolveComponent` から `ComponentCodec::completeDefaults` へ。
   `ResolvedSceneDefaults` は componentcodec.hpp へ移した。dispatch は codec 名ではなく `runtime_kind` で、
   `ScopedComponentCodecCatalog` で camera codec を差し替える test(WP378)でも補完が外れない。
4. loader の文字列比較: `resolvedscene.cpp` の `== "camera"` 3 箇所が消えた(1 は completion の移動で消滅、
   2 は `isCameraComponent()` = `runtime_kind` 判定へ)。codec が手元に無い比較(raw JSON の `"transform"` 数え、
   `prefabprovider` の `provider_name`)は名前のまま残した。

### 意図した挙動差(いずれも同じ legacy branch の中)

- **light の `intensity` / `range` の上限が `FLT_MAX` から schema の `1e12` になった。** editor が広告していた
  range を runtime が enforce するようになった側の変更で、監査が指摘した「editor の schema と runtime の受理が
  静かにずれる」の実例そのもの。
- transform / light の encode が schema の finite / range 検査を通るようになった(以前は無検査)。
  NaN を含む runtime transform は `null` を書き出す代わりに拒否される。
- 宣言経路が出す検査メッセージは schema 側の語彙になる(`must be a vec3 array` → `has an invalid array shape`、
  `must be finite F32` → `must be finite` / `is not representable as F32`)。error code と受理集合は同じ。
- 1 つの payload に複数の誤りがあるとき、どれが先に名指しされるかは変わりうる(presence 検査が値検査より前に集まった)。
  誤りが 1 つの payload では code もメッセージも従来どおり。

## 10. 段 C の実施記録(2026-09-19)

| # | 変更 | 備考 |
|---|---|---|
| C-1 | `PreparedSceneState` の per-slot 配列 17 本を `ModelInstanceSlotTable` 1 値型に。field 28 → 12。`publishPreparedSceneModel` 55 → 8 行、`prepareModelInstanceRebuild` 186 → 132 行(同名 shadowing 20 個と `next_*` コピー 30 行が消えた) | 配列の列挙は 9 箇所 → table の 6 操作だけ。GPU test 1 case(publish 前後で全配列の数が一致) |
| C-2 | transform / light の decode / encode を `decodeStructObject` 経由に。`requireClosed` の名前 list は 6 codec 全部で schema から生成。camera の既定値補完 115 行を `ComponentCodec::completeDefaults` へ | **sprite / collider / camera は保留**(§9: legacy branch id が規則単位で 35 個、camera は schema が wire 形と不一致)。挙動差: light の上限 1e12、encode 時の finite 検証、error 文言 |
| C-3 | `XrCompositionTarget` の frame 状態 bool 6 個 → `FrameStage` 7 状態 + 遷移関数。`VulkanXrCompositionGraphics` は別 TU | 残した flag(depth / teardown / per-view / per-swapchain)は独立な資源の状態。CPU test 1 case(不正遷移の拒否) |
| C-4 | `runPhases` の callback を lock 外で呼ぶ(sink は handle で再解決)。例外 → Status は型で対応(bad_alloc / invalid_argument / それ以外 = callback_failed)。`releaseOwner` は `sources` だけ compaction | `owners` / `phases` / `cursors` は `invalid_handle` vs `stale_generation` が契約なので tombstone 維持。`recursive_mutex` 維持(C ABI 全面が callback から到達可能)。VRM との lock 順逆転は phase 経路では解消。非 GPU test 2 case |
| C-5 | pose graph の失敗を `exception_ptr` で保持し、`framephase.cpp` が generic 例外を投げる前に rethrow。`evaluate` 262 行を 5 関数に分割 | 「pose port is unconnected」の literal は document からは到達不能(`validateGraphPorts` が先に弾く)。到達可能な in-phase 失敗で test(GPU) |
| C-6 | `Loop::run` 276 → 62 行。`FrameStep{advance, update, render, record}` 1 本と 3 driver。3 経路の差は enum 3 個で毎回明示 | 監査の 2 点の他に 4 点の差があった(update clock の位置、`updateFrameState` vs interactive、input clear、frame limit)。`#if` は 14 → 11。gate は O-1 replay(bytes) |
| C-7 | `ReloadParticipant` を `{file, runtime, authored}` の role に、file の apply は 3 択 variant。`applyRequests` は `RequestSlot` 1 本(index 保持)。shader / material の名指し pass を削除し、shader を companion owner として登録。owner の順序は companion 辺の topological 順 | validator 10 規則 → 6 検査。死んだ callback 2 個削除。観測可能な変更 1 件: shader と material の両方が claim する key は shared claim(今日は存在しない)。CPU test 1 case(登録順に依らない) |
| C-8 | `GameContext` を root + `physics()` / `audio()` / `animation()` / `save()` / `render()` に分割(65 → root 32 + facade 33)。重複 pair: `raycastClosest` は filter 付き 1 本(旧 unfiltered 経路の決定的 tie-break を移植)、`overlapAll` は hit を id 順で返す 1 本。無効 build の physics は throw(audio と同じ helper、probe 追加) | frozen v2 header で compile する `wp380b_effect` fixture が唯一の壁 → **v3 snapshot を追加して向け直し**(v1 / v2 は byte 不変、epoch 2 のまま)。`public_api_inventory` の tool / manifest / JSON を追随。`PhysWorld` 内部の同名 overload は未削除(follow-up) |
| C-9 | `executePlannedFrameGraph` の引数 16 → `FrameGraphExecutionRequest` + 2、`executeRenderingPasses` 17 → 3 | 本体 952 行の分割は D |
| C-10 | `computetask.cpp` 3947 行を `framegraphbuffercontainer` / `computetaskdefinitionjson` / `computetaskcontainer` の 3 TU + `deferredresourceretire.hpp` に。`registerComputeTask` 414 → 252 行。GLSL 直書きは `engine://ray_group_noop`。`ComputeTaskRuntimeDependencies` の注入 field 2 つを撤去(global module 前提を明示) | 純移動は sorted-line diff で確認。`appendUnique` の死んだ複製 1 つ削除 |

積み残し(段 C から D への申し送り): C-2 の sprite / collider / camera、C-8 の `PhysWorld` 内部 overload と `setCamera` の `void`、C-4 の outer `catch(...)` → `out_of_memory` 2 箇所、C-3 / C-6 / C-10 が動かした doclink。

追記(C-10 の GLSL 外出しは取り下げ): `engine://` に置いた 4 行の no-op closest-hit は `engineShaderUniverseSha256()`(`shadercompiler.cpp`、whitelist 拡張子の embedded shader 全部を 1 digest に畳む)に加わり、全 shader cache key を動かして WP364 の frozen parent oracle(`test/fixtures/wp364/parent_baseline.json`、過去 2 回 `9e4081b4` / `62f09ce0` で再凍結)を再び再凍結させる。4 行の literal のためにその費用は払わないと裁定し、GLSL は `computetaskcontainer.cpp` 内に戻した(理由の comment 付き)。副産物の指摘: whitelist は `.rint` / `.rcall` を含まないので、intersection / callable の embedded source を編集しても cache key が動かない(hit 系は全部動く)。別の小 item。

## 11. 段 D 第 1 波の実施記録(2026-09-19)

| # | 変更 | 備考 |
|---|---|---|
| D-1 | `renderLogicalFrame` 1244 → 26 行。lambda 13 個は private nested `LogicalFrameRecorder` の member に(field は「どの step で有効になるか」を comment で宣言)。view family / sequential の 90 行 × 2 は `recordOneExecution(view_index, logical_view_count, is_view_family, authored_schedule)` 1 本、旧差異 14 点は全部 parameter か `is_view_family` 分岐 | 純移動。`resolveFrameEngineTime()` と `activeTemporalHistory()` だけ constructor に前倒し(純 lookup) |
| D-2 | `SwapchainFrameTarget::Impl` 2419 → 2198 行。`pumpMaintenance` 305 → 87(completion 3 分岐を method に、action 5 段 ladder は default 無しの `switch`)。worker は `SwapchainBuildWorker` class、epoch / build record 型は file scope へ(byte 一致を 3 手法で確認)。retry tick 定数 2 個 | 決定表は前後で同一(report に表あり)。唯一の実 test は `wsi_fault_window_player`(Windows + Debug) |
| D-3 | `editorjournal.cpp` 6482 → 4892 行。prepare* 1251 行 → `editorjournalops.cpp`、unpack integrity 315 行 → `editorjournalintegrity.cpp`(byte 一致)。`Impl` 27 → 20 member(`PreviewLeaseState`)。結果公開 5 行 × 6 → `publishResult` / `publishFailure`、finalizer の 20 行重複解消、`importSceneSnapshot` v1 / v2 の共通部抽出。variant chain 5 本に `static_assert` 終端(不完全なものは無し) | `prepareRpcOperation` は prefab preparer に依存するので残置 |
| D-4 | `SceneLoader::load` 677 → 184 行。participant 18 個(`SceneLoadParticipant`、verb 9 個)+ phase 表 9 本 + loop。fault 注入は `SceneLoadMutationGate` 1 箇所(mutation 名 12 個は不変)。**rollback 順は publish の逆順ではない**(renderer → procedural が loader state の swap 後)ので明示表に | `EditorProjectionAdapter` は流用せず(context と early publish / activate が無い)。net +718 行 |
| D-5 | `materialcontainer.cpp` 5124 → 2407 行。`MaterialTextureStore` / `MaterialDescriptorBuilder` / `MaterialGenerationPlanner`。`prepareRuntimeGenerationReload` 540 → 331(lambda 392 → 179、`planScreenInputAbi` / `planResourcePortAbi`)。重複 4 種を解消(`resolveLiveMaterialBindings`、`inheritFromBase` template、`forEachCompiledPass`、view 次元 lookup + policy 表)。ladder は `resolvePassInputImageView` 1 本 + 呼び手の `layer_binding` policy | §8 の 13 点: 2 / 7 / 1 は fullscreen 側、10 / 11 は material 側に統一、6 / 13 / 8 / 9 は呼び手 policy。row 1 の挙動変更は「現状 validation error になる入力」だけ。11 の resource 名付き error は resolver に name accessor が無く follow-up |
| D-7 | `AnimationServiceRuntime::Impl` 1839 → 1608 行。`PoseArena` / `SinkRegistry` / `PhaseScheduler` の 3 部品、shim 26 entry は「検証 → lock → 転送」の定型 | 2 部品以上に跨る関数(phase driver、staged frame commit、publish from source 等)は `Impl` に残置。第 4 部品 `AssetTable` が follow-up |
| D-8 | `GltfAccessorReader` を別 TU + CPU test 24 section。`loadMesh` 342 → 68 行(`buildPrimitive` が skin / morph / VRM / material registry の context を明示で受ける)。`#if PELICAN_WITH_VAT` 4 → 0(seam 1 箇所 `parseVatPrimitive`)。`InternalGltfLoader` 1754 → 1265 | tinygltf / stb の `IMPLEMENTATION` define は `tinygltf_implementation.cpp` に移した(`gltf.cpp` が accessor header 経由で `<tiny_gltf.h>` を 2 回含み、実装 block が include guard の外にある) |

第 2 波(未着手): D-6 `registerRenderingPassConfigVariantsData`(627 行、GPU 確保 + transaction)、registry lifecycle の `ProviderRegistry<Traits>`、DebugDraw / DebugText / Gizmo の共通 registry、physicsruntime の query 骨格、gamelogicreload の 4 列挙、frameplanner の 22 行 × 4 と node 2 経路、JSON helper(`appendUnique` / `requireString`)、vrmapplication `setInputs`、playback stub、openxrsession `endFrame`、mirror sink の catch、`RenderTargetContainer` accessor 17、`pelican_sets.hpp`、`RpcServer` の domain 知識、`*ForTesting` 219 個の friend 集約、D-7 の `AssetTable`。

## 12. 段 D 第 2 波の実施記録(2026-09-19)

| item | 変更 | 備考 |
|---|---|---|
| D-6 | `registerRenderingPassConfigVariantsData` 627 行 → `prepare` / `allocate` / `registerVariants` / `publish` + 16 行の driver。inner loop の attribute upload + graph state attach + value graph 検証は `attachProgramGraphState` に。`include_preview` は request list に(preview は artifact 種別で見つける、順序同一)。shadowing の `dependencies` は rename | rollback の窓(GPU arena の RAII、`allocate` 以降)は同一で、stage 境界がそれを言う。深さ 7 → 5。value graph 検証は publish に動かさない(throw 順が変わる) |
| frameplanner / JSON / RT accessor | attachment block 4 → `appendAttachment` 1。`passKind` 削除。`appendUnique` 4 定義・`requireString` 2・`parseStringList` 2 → `renderingpassjsonhelpers` に 1 つずつ(error 文は接頭辞を context 引数に移して byte 同一)。`bool array_view` shim 2 本 + resolver の死んだ転送を削除、23 call site を enum に | JSON node 経路は `PassDefinition` に流せない(登録前の authored 名で resource を指す)ので builder 2 本は残置。`RenderTargetFrameSelector` は 123 call site で打ち止め |
| registry lifecycle | `ProviderRegistry<Traits>` に 5 registry(graph transform / subgraph / strategy / pass implementation / render policy)。正規化 token diff で lifecycle 規則の差は 0。native scope executor は retirement 規則が無い別物なので対象外 | net −599 行。owner retirement の不変条件は文書に無かったので template の comment に書いた。pin する test は §(report) の 6 本 |
| renderer | DebugDraw / DebugText / Gizmo の plumbing 9 関数 → `LineOverlayPassRegistry` 1 つ(差は `LineOverlayRegistryTraits` 値)。`materialrender` の indirect buffer 選択は 3 箇所 → `resolveDrawSource`、`camera` の既定 state 2 箇所 → 1。`PolygonInstanceContainer` の `*ForTesting` 30 → 5(25 は `test/polygoninstancecontainer_testaccess.hpp` の friend、5 は production が読む) | DebugText と UI renderer は統合しない(2026-07 裁定どおり)。net −233 |
| physics / gamelogic | query 7 本の骨格 → `runQuery` + `MeshQuerySet` + `invokePrimitiveProvider` + `decodeHits`(catch 3 連 7 → 1)。mesh provider の `toAbi` 3 → 1。`PhysWorld` の未 filter overload 撤去、17 call site を `every_collider_filter`(全 layer / mask)に。gamelogicreload の provider 列挙 4 → 表 1 本(順序同一、rollback guard は引数) | 既定 filter は layer bit 0 なので「filter 無し」の忠実な置換ではない → 全 collider filter を新設 |
| animation / openxr / playback | `vrmapplication` の `validateCommonInput` / `readWeights` / `prepareInputLocked` 共有(`setInputs` → `setTypedInputs` の委譲は tail の挙動差(frame 退役 / `phase_order_error`)があり不採用)。animation の outer catch は型で対応(それ以外は `out_of_memory` を保守的既定と明記)。`AssetTable` は候補 24 のうち 9 が他 part に既に依存し純移動にならないので見送り(comment に記録)。`openxrsession` の `endFrame` 2 → 1、mirror sink は RAII guard 3 個 + catch 1 つ + 空 label 削除。camera override は `applyLaunchCameraOverride` 1 本を起動経路で 1 回、playback の stub は `throwBuildFeatureDisabled` だけに | 挙動変更 1 件: SEQPLAYER / VAT OFF build でも `--camera` が効くようになった(旧 stub は no-op) |
| rpc / shader | `RpcServer` は reload gate を注入で受け、順序保持は handler 登録時の述語(3 handler が同じ `embedsResolutionError` を登録 = wire 同一)。`pelican_sets.hpp` は set 別 block(衝突 2 組は別 set と判明)。ray table の set 3 / binding 0,1,2 / 1024 は `raygrouplayout.hpp` の定数 1 箇所から GLSL と C++ の両方へ(include 文字列は byte 同一、rt2b の hash が pin) | |

未着手で残るもの: C-2 の sprite / collider / camera(legacy branch id の機構が先)、animation の `AssetTable`(純移動でない)、`RenderTargetFrameSelector`(123 call site)、`getAttachmentImageSubresourceView` の bool、fullscreen 側の resource 名付き error(resolver に name accessor が無い)、`MaterialContainer` / `Renderer` の `*ForTesting` friend 集約、`docs/implementation_plan.md:22996` の死んだ link(完了済み台帳記録の中)。
