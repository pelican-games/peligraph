# 機能追加バックログ(2026-07-24 時点・順序付き)

作成: セッション引き継ぎ用。WP195 / RPE10b2 GPU scope replacement 完了地点へ同期。
状態の正: active は `docs/implementation_plan.md`、完了済みは
`docs/implementation_archive.md` と `docs/design_reviews/*_wp*_report.md`、
挙動はコードとテスト。
既完了の目安: アニメ A0〜A2 + VRM/VRMA C0/R0/I0、HR0〜HR2-G +
per-asset animation generation、2D S2D-0a〜2、UI U2、TAA、OpenPBR/USD、
OpenXR sequential PCVR、editor authoring/RPC/save/preview、E2 trigger。
レンダリングは versioned `hybrid_v1`、semantic material route、OpenPBR base subset
deferred、forward opaque/transparent 合成、typed color/depth screen input まで実装済み。

凡例: ★ = 高レバレッジ(体験が一段変わる)、[設計済] = 設計文書に
仕様あり(WP 登録して派遣可能)、[要設計] = 私(設計担当)の起草 +
敵対レビューが先。

## Tier 1 — 走っているトラックの完成(そのまま派遣可能)

1. **S2D-1: strict pixel-perfect + flipbook dogfood** [完了: WP106・条件 C4] —
   ドット絵の見た目品質。方式選定(logical target vs quantization)込み
2. **S2D-P: sweep/shapeCast/MTD/filter クエリ** [完了: WP107・条件 C5] —
   platformer の物理下地。physquery の拡張(シミュ導入ではない)
3. **S2D-2: side-scroller vertical slice** [完了: WP109] — 非特権
   moveAndSlide + 接地/斜面/one-way/fixed-step replay の縦切り
4. ★ **HR2-S: shader hot reload の新基盤移行** [完了: WP108] — 1s poll を
   FileWatcher/ContentDigest へ移行。include/.surface reverse dependency、
   全 variant/pipeline、material layout の cross-file transaction を実装
5. **HR2-G: モデル/フラグメント hot reload** [完了: WP110 + WP147] —
   ModelAsset/Instance identity・fragment 一括 transaction・in-place rebuild・
   in-flight 資源解放を実装。animation は全体 reset ではなく対象 asset の
   generation 更新 + evaluator rebind
6. **HR2-I: input_actions/プロファイル hot reload** [設計済] — 小
7. **U3: UI hot reload transaction** [設計済(UI v8 §9)] — HR0 契約に
   乗せる。エディタ往復(D3)の前提

## Tier 2 — アニメの実用化(VRM/VRMA 5 連 + グラフ拡張)

8. ★ **VRM-S0: .vrm semantic decoder**(humanoid/expression/lookAt の
   保持)[完了: WP111] — 「VRM を一級で扱う」の入口
9. **VRM-S1: per-instance application sink**(morph/expression/gaze の
   実適用)[完了: WP121/122/122b/123b/134]
10. **VRMA-C0: .vrma コンテナ + typed channel** [完了: WP176]
11. **VRMA-R0: versioned humanoid retarget profile** [完了: WP177]
12. **VRMA-I0: AnimationSource 接続** [完了: WP178] — named source、
    graph blend、typed expression/gaze sink、reload generation/rebind が成立。
    自動 FileWatcher 配線は後続
13. **anim_graph v2 拡張**(layers・sync marker・trigger・2D blend
    space・inertialization)[要設計 — v2 予約キーの解禁順を決める]
14. **clip annotation/events sidecar** [要設計小] — 足音・ヒット判定を
    E1 イベントへ
15. **timeline director(SeqPlayer v2)** [要設計] — typed track 束ね
16. **live streaming AnimationSource** [設計済(§5)] — VMC 等の
    リアルタイム 3DCG プレイヤー用途の本丸。raw take / deterministic
    bake の二成果物
17. **SpringBone / secondary motion** [要設計 — design_cloth_simulation.md
    と統合] — VRM 揺れもの

## Tier 3 — 見た目の底上げ(レンダリング)

18. ★ **TAA feature(ユーザー空間)** [完了: WP112〜115] — jitter 機構、
    scalar parameter、resolve/composite、ユーザー定義 table pattern まで
    stdlib feature として実装
19. **motion blur feature** [要設計小] — velocity buffer の第 2 の消費者
20. ★ **ライティング/IBL 設計**(保留中のトラック 3)[要設計] —
    punctual lights KHR 1:1・複数シャドウの RT 配分・IBL は import 焼き
21. **hybrid deferred + forward 基盤** [完了] — versioned preset、semantic
    route、OpenPBR base subset の G-buffer 化、scene-linear forward 合成、tone map
    一回を実装。後続の正本は `design_render_pipeline_extensibility.md`。
    RPE1(WP180、完了) → RPE2 typed plan(WP181、完了) →
    RPE3 DrawQueueBuilder(WP182、完了) → RPE4 provider registry(WP183、完了) →
    RPE5 bounds / phase queue / transparent sort / XR view policy(WP184、完了)と
    RPE6a logical type / shadow graph(WP185、完了)、RPE6b0 versioned logical value /
    producer edge(WP186、完了)、RPE6b1 typed screen input / opaque snapshot
    (WP187、完了)、RPE6c0 topology / backend probe / planning policy
    (WP188、完了)、RPE6c1 ResourcePattern / desktop/tile target plan
    (WP189、完了)、RPE7/RPE8 sample-count + executable MSAA(WP190、完了)、
    physical target runtime統合(WP191、完了)、XR/preview builtin
    GraphVariantPolicy(WP192、完了)、immutable runtime generation /
    atomic publication root(WP193、完了)、append-only GPU registry rollback /
    owner-scope manifest(WP194、完了)、scope-aware replacement /
    generation-owned registry lease(WP195、完了)まで実装。
    次は submission fence retire / watcher接続をRPE10b3として扱う。
    論理型、material/light contract、Vulkan physical plan / NativeScope の詳細は
    `design_render_graph_compiler.md`。CPU / GPU compute / external backend を横 domain として
    接続する共通 typed dialect、fragment / closed forest は
    `design_heterogeneous_execution_graph.md` を正とする。RPE6c0/1 はその lowering seam、
    pairwise topology、optimize-by-default / advisory診断だけを置き、CPU scheduler と動画 encode/decode は
    計測・具体需要まで後回し
22. **スプライトライティング(surface 化)** [要設計小] — 2D の B 層接続
23. **M3b 昇格ゲート実施**(spv-link 本採用判定)[設計済・ゲート明記済]
24. **M4: B-web capability プロファイル** [設計済] — web を見るなら
25. **bindless バックエンド** [設計済(§3-5)] — GPU 駆動系の需要と同時
26. **compute パーティクル(WP36 系)** [要設計]

## Tier 4 — 開発体験・ツール

27. ★ **編集系 rpc(D2 前提)+ pelican_rpc.py** [完了: WP149〜161] —
    typed query/edit、CAS、journal、undo/redo、preview lease、windowed host、
    Python client が devstudio とエージェント駆動編集の共通入口
28. **devstudio D1(Qt 埋め込みビューポート + アウトライナ)** [設計済]
29. **devstudio D2/D3 共通実装** [部分完了: WP164/166〜172] —
    schema-driven Inspector、atomic save、behavior edit、snapshot/watch、
    isolated preview は済。Qt viewport の picking/gizmo は未
30. **WebSocket コマンド層** [設計済(§3 予約)] — 複数クライアント
31. **リロード履歴 ImGui パネル + get_status.reload の可視化**
    [設計済(HR §5)] — 小
32. **起動高速化第 2 弾** [要計測] — 現在 models 1.9s が支配的。
    KTX2 一括化(WP92 レシピ適用)+ モデルロード並列化
33. **R6: docs 分割**(implementation_plan → active ledger/archive)
    [完了: WP174]
34. **CI 常設** [部分完了: WP137/165] — Windows CPU gate、SKIP exact
    allowlist、構成/OFF matrix、clean-clone は済。GPU CI2・UBSan は未
35. **import-tools 拡充**: msdf-atlas-gen(本格テキストの前提)・
    gh repo create(import-tools / houdini-adapter のリモート化)

## Tier 5 — ゲーム機能の横展開

36. **本格テキスト(SDF フォント)** [要設計 — msdf atlas + UI 拡張] —
    現在は bitmap グリフのみ
37. **UI レイアウト v2(Yoga 予約分)+ PSD→UI(K3+layout)** [設計済
    予約] — ツール調 UI の量産
38. **TileMap 形式**(チャンク・衝突・オートタイル)[要設計 —
    S2D-2 の後、2D ゲームの実需で]
39. **オーディオ A2**(空間音響・BGM フェード等)[要設計 — A1 は済]
40. **シーン S2(非同期ロード)** [設計済(scene_flow)]
41. **E2: 物理トリガーイベント** [完了: WP179] — 決定的な対称
    Enter/Exit、stay 抑止、destroy/remove Exit、scene reset no-Exit
42. **物理シミュレーション判断**(Jolt 導入 vs 自前最小)[要設計 —
    S2D-P の実需を見てから。設計文書に Jolt 推奨メモあり]
43. **永続化 P1** [完了: WP65] — settings/saveData/listSaves と atomic
    write は済。delete/cloud/async 等は需要時に別設計

## Tier 6 — プラットフォーム(遠景)

44. **OpenXR PCVR sequential stereo** [完了: WP125〜138] — Vulkan
    bootstrap、session loop、composition target、action/pose、mirror、VRM demo、
    Meta XR Simulator gate は済。XR2b multiview/depth・物理 HMD・SA は未
45. **web ビルド案 B**(WASM ゲームコア + TS レンダラ接合)[方向
    決定済・遠景] — RHI 後付け禁止の規律のまま
46. **RT(レイトレ)** [遠景]
47. **main への統合ブランチ合流** [運用 — 節目で。merge commit 必須
    (squash/rebase 禁止)]

## 負債ウェーブ(2026-07-17 確定 — codex ultra 議論の合意順)

正本 = `docs/design_reviews/2026-07-17_debt_discussion_codex.md`
(Claude 棚卸し→敵対議論の全記録。A1=WP64 完了済みの事実誤認訂正・
N1〜N10 の新規発見を含む)。推奨順:

CI0(WP137)→ GOLDEN0(WP141)→ LIGHT0(WP142)→ TRANSIENT0(WP144)→
INSTANCE0(WP146)→ ANIM0(WP147)→ ECS0(WP148)→ ECS1(WP163)→
CI1(WP165)→ DTXT0(WP169)→ LIFETIME0(WP171)→ PORT0(WP173)→
CONTRACT0(WP175)→ TEST0(WP174) は完了。**未完了は CI2(GPU gate /
D-P5)だけ**。

**WP 化しないと合意**: R4 再監査(完了済み)・行列配送一本化・
generic history・module topological teardown・汎用 .surface variant
言語・debug_text 無条件全面統合。

## デバッグ/プロファイリング/最適化トラック(2026-07-17 起草 — ユーザー要望)

正本 = `design_debug_profiling.md`。D-P0(debug 命名/WP139)→
D-P1(RenderDoc capture/WP140)→ D-P2(GPU 計測 + VRAM + XR timing/
WP143/145)は完了。残りは D-P3(CPU フェーズ)→
D-P5(validation 常設)→ D-P4(Tracy ユニット・既定 OFF)→ D-P6(crash 診断)→
OPT-S(起動第 2 弾)/OPT-XR(Release 実機ベースライン)/OPT-MV
(multiview 評価)。最適化は必ず計測の数字を根拠に(WP82 流儀の標準化)。

## 推奨の直近候補(WP195 完了後)

| 順 | 候補 | 理由／前提 |
|---|---|---|
| 次 WP 候補 | **RPE10b3 / WP196: submission-aware pipeline reload** | WP195でsame-name scope replacementと旧generation resource leaseは成立。submission fence完了後のretireとFileWatcher/ReloadGateからのframe-boundary publicationを接続する |
| 2 | **HR2-I: input_actions/profile hot reload** | 設計済みで小さく、残る hot-reload 基本型を閉じる |
| 3 | **U3: UI hot reload transaction** | HR0 watcher/reconcile を共有し、editor save 後の UI 往復を完成させる |
| 4 | **VRMA watcher integration** | WP178 の generation/rebind entry point を FileWatcher/loader へ接続。WP 化前に asset identity と profile reload 範囲を固定する |
| 5 | **CI2 / D-P5** | 負債ウェーブで唯一残った GPU/validation 常設 gate。runner 方針の決定が前提 |
| 6 | **anim_graph v2 / clip events** | VRMA 基盤完成後の layers・sync marker・annotation/event sidecar。先に設計レビュー |
| 7 | **XR2b multiview/depth** | RPE9 の graph variant 境界と WP143/145 の計測値を基準に sequential stereo との差を評価してから実装 |
| 8 | ライティング/IBL、U-USD1/2 | 見た目と USD の次段。いずれも設計／受け入れ条件の登録が先 |
| 遠景 | Quest standalone(Android/ARM64) | SA0〜SA3 の段階計画は下記 |

## Quest standalone(SA トラック — 遠景・段階計画)

**方針(2026-07-17)**: PC 以外の standalone もやりたい(ユーザー)。
PCVR で作る XR 層(session/ループ/入力/stereo)は standalone と同一
API なので先行投資は全部生きる。移植の本体は「ツールチェーン +
プラットフォーム層 + 配送」。

| 段階 | 内容 | 備考 |
|------|------|------|
| SA0 ✅ | **NDK/clang ツールチェーン spike**: 純ロジック部(pelican_project・physquery・animation jobs 等)を ARM64 でビルド + ctest | ここで MSVC 依存を洗い出す。**着地 `ec7efeeaa`(2026-11-01)**: `PELICAN_ANDROID_SA0` の道(`cmake/pelican_android_sa0.cmake`)で schema・project と core の physquery / animation の切り出しを NDK r27c の clang で build し、Snapdragon 8 Elite の端末で test の実行ファイル 37 個を走らせて 36 個が成功。残る 1 個 `projectmigration_test` は、Android が hard link を禁じていて `projectdocumenttransaction.cpp` の排他的な作成(`create_hard_link`)が通らないため。直す WP はまだ無い(端末上の保存・移行が動かない。player が読むだけなら影響なし) |
| SA1 ✅ | プラットフォーム層: スマホの NativeActivity で `pelican_player` が画面に描く。窓は **GLFW を Windows に残し、Android だけ android_native_app_glue の上の薄い実装**(`window_android.cpp`・`windowsurface_android.cpp` の `vkCreateAndroidSurfaceKHR`。SDL3 にしない)。log は quill の Android sink と file。asset は APK の assets に project を入れて internal storage へ展開(`.pelican/` は除く)。Gradle を使わない APK。**XR で窓なしの形は Quest の時(SA2)** | **着地 `fc0c09bf6`(2026-11-01)**: minSdk 33、NDK r27c、16 KB page。shaderc v2025.1・glslang・SPIRV-Tools・Vulkan-Headers(vulkan-sdk-1.4.309.0)を pin して source から build し、実行時に compile する。IMGUI / OpenXR / RPC / hot reload / RenderDoc / audio は OFF。Android の swapchain は preTransform IDENTITY・FIFO。lifecycle は TERM_WINDOW で surface を同期に解放する入口と状態 `no_surface`([design_wsi_epoch_recovery.md](design_wsi_epoch_recovery.md) §7.1・§14.3 の SA1 追補)。端末(validation 有効、error 0)で headless の絵が Windows と一致、窓の 2688x1216 が正しい向き、home と復帰 ×3・消灯と点灯 ×3(手動)・起動中の home・作り直し中の home・画面分割・back・完了の証明が無い道の強制が通った。手順は `tools/android/README.sa1b.md` と `README.sa1-lifecycle.md`、manual は [第2章 §2.9](manual/02_getting_started.md#29-android-で-player-を動かす)。**SA1 の後に残った事**: (1) タッチ入力は **SA1f**(`Window::process()` は入力を積まない)。(2) **validation 無しの性能は未測定**(frame の時間は validation と synchronization validation を有効にしたままの CPU wall time。posegraph_demo で p50 約 61 ms)。(3) **IDENTITY の回転の代償**: 回転を compositor に任せるので、この端末(currentTransform = ROTATE_90)では swapchain を作るたびに validation が preTransform の不一致を warning に出す。compositor の回転の費用は未測定、pre-rotation は未検証。(4) Windows の窓の capture が validation で `SYNC-HAZARD-WRITE-AFTER-PRESENT` を出すのは SA1 以前からある別件(SA1 では直していない)。ほか未検証: 他の端末、Quest、page size が実際に 16 KB の端末、pump の 3 s・fence の 1 s・DESTROY の 2 s の上限を過ぎる道の端末での注入 |
| SA1f ✅ | Android の入力を今の action map へ: `onInputEvent` の queue から、キーボード(AKEYCODE → 今の `KeyCode`。`KeyCode` は増やさない)、BACK(離した時に activity を終える。manifest で予測型の戻るを切る)、ゲームパッド(16 スロット、順序付きのボタンの event、GLFW と同じ値の規約、トリガーの休止値 -1)、タッチ(指ごとの event と snapshot)、ペン(接触だけを筆圧なしのタッチとして)。UI は指ごとのポインタ(番号 1 + Android の id)と指ごとの消費。binding `touch:region` / `touch:tap`(profile の属性 `region`、250 ms / 8 dp)。`pelican.input_seq` はタッチを含む記録だけ v3、rpc `inject_input` にタッチ | **着地 `bbedafc88`(2026-11-02)**: 端末 NX789J で adb から入れたキー・パッドのボタンの長押し・指の tap と定点の長押し・UI の tap と長押し・ペンの tap・BACK の終了が通った(validation 有効、error 0)。2 本指・スロット・軸の値・polling の切替・MOVE の履歴・ペンの hover は CPU のテスト。Windows の GLFW の道は変えていない。manual は [第7章「Android の入力」](manual/07_input_ui.md#android-input) と [第2章 §2.9](manual/02_getting_started.md#29-android-で-player-を動かす)。**残した事**: ゲームパッドの抜き差しを検出しない(JNI を使わない。抜いた時の押下が残る)、文字の入力とソフトキーボード、実物のパッドの軸・実物のキーボードとマウス・2 本以上の指・一時停止の後の入力は端末で未検証 |
| SA1g | **feature が action に値を出す口**(仮想機器 `virtual:<name>`)。仮想スティック・仮想ボタン・ジェスチャ・振動は、その口に載る後の feature | **後回し**(SA1f の仕様レビューで切り出し)。`InputSnapshot` / `InputEvent` の trivially copyable・standard layout の約束のもとで、名前の表・収録の二重・replay への混入の設計が要る。input_seq の virtual もこちら |
| SA2 | OpenXR standalone session(XR1〜3 をそのまま)+ Adreno 実機検証 | Vulkan validation・タイルベース特性 |
| SA3 | 性能: **multiview(XR2b)必須級**・dist-bake シェーダ(B4)必須・FFR(foveated)を jitter と同じ「版付き語彙 + ユーザー feature」で | 72/90Hz 維持が gate |

**door-keeper(いまから守る安い規律)**:

1. **clang コンパイル smoke を CI 候補に**(Tier 4 #34 の CI 常設に
   含める — MSVC 拡張への依存を増やさない)
2. Windows 専用コード(Win32 watcher・DLL reload・GLFW)は既に
   ユニット/モジュール境界の内側 — 新規コードもこの規律を維持
   (ホットリロード系は dev 専用なので Android では OFF でよい)
3. B3(embed)/B4(dist-bake)は SA の前提 — 優先度をモバイル文脈で
   再評価
4. レンダラの新機能は「タイルベース GPU で成立するか」を設計時に
   一言書く(RMW 多段ポストの乱造をしない)

設計(私の作業)が必要なものは敵対レビュー往復(codex)を挟むこと。
条件付き Accept の条件は必ず WP に逐語添付する(確立済みの運用)。
