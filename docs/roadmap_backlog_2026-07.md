# 機能追加バックログ(2026-07-16 時点・順序付き)

作成: セッション引き継ぎ用(tip b26d1a3 時点の全体棚卸し)。
状態の正: `docs/implementation_plan.md`(WP71〜110 登録済み)・
`docs/manual/`・各 `docs/design_*.md`。
既完了の目安: アニメ A0〜A2 完成 / HR = HR0/HR1/HR1-T/HR1-M/HR2-S/HR2-G 完成 /
2D = S2D-0a/0b/S2D-1/S2D-P/S2D-2 完成 / UI U2 / temporal T1+T2。

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
5. **HR2-G: モデル/フラグメント hot reload** [完了: WP110] —
   ModelAsset/Instance identity・fragment 一括 transaction・in-place rebuild・
   anim/temporal reset・in-flight 資源解放を実装
6. **HR2-I: input_actions/プロファイル hot reload** [設計済] — 小
7. **U3: UI hot reload transaction** [設計済(UI v8 §9)] — HR0 契約に
   乗せる。エディタ往復(D3)の前提

## Tier 2 — アニメの実用化(VRM/VRMA 5 連 + グラフ拡張)

8. ★ **VRM-S0: .vrm semantic decoder**(humanoid/expression/lookAt の
   保持)[設計済・5 分割の第 1] — 「VRM を一級で扱う」の入口
9. **VRM-S1: per-instance application sink**(morph/expression/gaze の
   実適用)[設計済] — renderer の per-instance 化を含む大物
10. **VRMA-C0: .vrma コンテナ + typed channel** [設計済]
11. **VRMA-R0: versioned humanoid retarget profile** [設計済] — 大物
12. **VRMA-I0: AnimationSource 接続** [設計済] — ここで「VRMA を
    どのモデルにも再生」が成立
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

18. ★ **TAA feature(ユーザー空間)** [要設計 — projection jitter 枠の
    API 形をこのとき確定(render 側 modifier・カメラ API から不可視)]。
    WP88 history/velocity + WP95 skinned velocity で材料は揃っている
19. **motion blur feature** [要設計小] — velocity buffer の第 2 の消費者
20. ★ **ライティング/IBL 設計**(保留中のトラック 3)[要設計] —
    punctual lights KHR 1:1・複数シャドウの RT 配分・IBL は import 焼き
21. **deferred パス** [設計済(material §3-9)] — ユーザー契約無傷で
    G-buffer 化。ライト数が増えてから
22. **スプライトライティング(surface 化)** [要設計小] — 2D の B 層接続
23. **M3b 昇格ゲート実施**(spv-link 本採用判定)[設計済・ゲート明記済]
24. **M4: B-web capability プロファイル** [設計済] — web を見るなら
25. **bindless バックエンド** [設計済(§3-5)] — GPU 駆動系の需要と同時
26. **compute パーティクル(WP36 系)** [要設計]

## Tier 4 — 開発体験・ツール

27. ★ **編集系 rpc(D2 前提)+ pelican_rpc.py** [要設計 — D0 原則で] —
    devstudio とエージェント駆動編集の共通入口
28. **devstudio D1(Qt 埋め込みビューポート + アウトライナ)** [設計済]
29. **devstudio D2(ピッキング/ギズモ)→ D3(保存 round-trip/undo)**
    [設計済] — D3 は HR/U3 が実行基盤
30. **WebSocket コマンド層** [設計済(§3 予約)] — 複数クライアント
31. **リロード履歴 ImGui パネル + get_status.reload の可視化**
    [設計済(HR §5)] — 小
32. **起動高速化第 2 弾** [要計測] — 現在 models 1.9s が支配的。
    KTX2 一括化(WP92 レシピ適用)+ モデルロード並列化
33. **R6: docs 分割**(implementation_plan 110KB → 台帳/アーカイブ)
    [計画済] — 人間とエージェント双方の読み込み効率
34. **CI 常設**(GitHub Actions: build + ctest + golden hash + OFF
    スモーク + UBSan)[要設計小] — 現在はローカルゲートのみ
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
41. **E2: 物理トリガーイベント** [設計済予約 — E1 済]
42. **物理シミュレーション判断**(Jolt 導入 vs 自前最小)[要設計 —
    S2D-P の実需を見てから。設計文書に Jolt 推奨メモあり]
43. **永続化残り**(settings/saveData の実運用 API)[設計済 — P1 の
    上物]

## Tier 6 — プラットフォーム(遠景)

44. **OpenXR**(xrWaitFrame ループ・multiview・XrFrameTarget)
    [方向決定済・要設計] — 入力アクション層は準備済み
45. **web ビルド案 B**(WASM ゲームコア + TS レンダラ接合)[方向
    決定済・遠景] — RHI 後付け禁止の規律のまま
46. **RT(レイトレ)** [遠景]
47. **main への統合ブランチ合流** [運用 — 節目で。merge commit 必須
    (squash/rebase 禁止)]

## デバッグ/プロファイリング/最適化トラック(2026-07-17 起草 — ユーザー要望)

正本 = `design_debug_profiling.md` v1(敵対レビュー待ち)。
D-P0(debug 命名)→ D-P1(RenderDoc capture)→ D-P2(GPU 計測 v2 +
VRAM + XR timing)→ D-P3(CPU フェーズ)→ D-P5(validation 常設)→
D-P4(Tracy ユニット・既定 OFF)→ D-P6(crash 診断)→
OPT-S(起動第 2 弾)/OPT-XR(Release 実機ベースライン)/OPT-MV
(multiview 評価)。最適化は必ず計測の数字を根拠に(WP82 流儀の標準化)。

## 推奨の直近ウェーブ(1 系統運用・上から順に)

| 順 | WP | 理由 |
|---|---|---|
| 済 | S2D-1 / WP106 | C4 を閉じ、strict pixel policy と flipbook dogfood 完了 |
| 済 | S2D-P / WP107 | C5 を閉じ、shapeCast/MTD/filter と provider V2 完了 |
| 済 | HR2-S / WP108 | include/.surface と material layout の一括差し替え完了 |
| 済 | S2D-2 / WP109 | 非特権 controller と side-scroller vertical slice 完了 |
| 済 | HR2-G / WP110 | モデル/全 fragment の原子的差し替えと live instance 再構築完了 |
| 済 | VRM-S0 / WP111 | VRM 1.0 semantic decoder 着地 |
| 済 | TAA/USD 設計二往復 | v1 Reject → v2 → 条件付き受理(v2.1 反映済み) |
| 済 | J1 / WP112・J1b / WP114 | jitter 機構 + スカラー parameter 配送着地 |
| 実装中 | T-TAA / WP113(再派遣) | TAA feature 本体(scalar blocker は WP114 で解消) |
| 1 | **J1c / WP115**(T-TAA 着地後) | ジッタ系列のユーザー定義(pattern:table)+ **adding_features.md へ temporal ユーザー管理枠の境界表を転載**(正本 = design_taa_jitter.md §0-1・2026-07-16 ユーザー指示) |
| 済 | M-PBR0a/WP116 → M-PBR0b/WP117 | OpenPBR エンジン側完成(3 stdlib surface 体制) |
| 済 | U-USD0a/WP118 → U-USD0b/WP119 | usd-core 26.5 採用・USD→描画の一本道開通 |
| 実装中 | WP121(VRM-S1a morph 描画)・WP124(U-USD0c マテリアル) | morph は WP120 監査停止からの分割 1 段目 |
| 1 | WP122(M-INST0)→ WP123(VRM-S1b) | instance override ABI → VRM 表情 sink |
| 2 | **OpenXR 設計レビュー → XR1 → XR2a → XR3 → XR4**(2026-07-17 ユーザー要望: Quest 3 で VRM キャラ) | `design_openxr.md` v1 起草済み。**MVP = PCVR(Link)**・sequential stereo 先行・multiview は XR2b。XR4 = VRM キャラデモが受け入れ実体 |
| 3 | ライティング/IBL 設計(私)→ WP 化 | 保留トラック 3 の再開(VR ステージの見栄えにも効く) |
| 4 | U-USD1a/1b・2a/2b(UsdSkel/camera/anim/instancer) | USD の残り |
| 遠景 | **Quest standalone(Android/ARM64 移植)— 2026-07-17 ユーザー方向表明** | 段階計画は下記 SA 節 |

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
