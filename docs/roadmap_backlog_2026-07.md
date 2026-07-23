# 機能追加バックログ(2026-07-23 時点・順序付き)

作成: セッション引き継ぎ用。WP189 / RPE6c1 desktop/tile target planner 完了地点へ同期。
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
    GraphVariantPolicy(WP192、完了)まで実装。
    次は pipeline transaction を独立 WP として扱う。
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

## 推奨の直近候補(WP192 完了後)

| 順 | 候補 | 理由／前提 |
|---|---|---|
| 次 WP 候補 | **RPE10: pipeline transaction** | WP192でlogical/physical/runtimeが読むvariant policyを統一済み。route/sample/providerをcandidateとしてprepareし、frame境界でatomic publishする |
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
| SA0 | **NDK/clang ツールチェーン spike**: 純ロジック部(pelican_project・physquery・animation jobs 等)を ARM64 でビルド + ctest | ここで MSVC 依存を洗い出す |
| SA1 | プラットフォーム層: android_native_app_glue(**GLFW 不要** — standalone XR は窓なし)・logging・アセット = APK/OBB パッケージング | 入力は OpenXR 直(XR3 再利用) |
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
