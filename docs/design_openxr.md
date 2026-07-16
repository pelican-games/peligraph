# OpenXR 対応(v1)— Quest 3 で VRM キャラを見る

対象読者: エンジン担当・VR コンテンツを作る人。
ステータス: v1 ドラフト(2026-07-17。敵対レビュー前)。
前提: 2026-07-07 推進決定(①ループ主導権 ②フレームグラフ view 次元
③WITH_OPENXR ユニット必須)、`design_input_actions.md`(pose 型 action =
OpenXR 予約済み)、`design_build_tiers.md`(誕生時ユニット化要件)、
IFrameTarget(Swapchain/Offscreen の 2 実装 — 第 3 実装の受け口)、
アニメトラック完成(A0〜A2)・VRM S1a/S1b 進行中。

## 0. ターゲットの確定(最重要の分岐)

**MVP = PCVR(Windows + Meta Quest Link / Air Link)**。

| | PCVR(採用) | Quest 3 standalone(遠景) |
|---|---|---|
| エンジン | **Windows のまま**(MSVC/Vulkan 現行) | Android/ARM64 移植(NDK toolchain・asset 配送・入出力全部) |
| ランタイム | Meta の Windows OpenXR runtime に接続 | Quest 内蔵 runtime + APK |
| 到達距離 | XR2 完了で実機に絵が出る | エンジン移植という別プロジェクト |

standalone は本設計の対象外とし、Tier 6 バックログに「Android 移植
(OpenXR standalone 込み)」として独立記録する。PCVR で作る XR 層
(session/フレームループ/入力)は standalone でもそのまま使える —
無駄にならない。

## 1. ビルドユニットと既定

- `PELICAN_WITH_OPENXR`(既定 ON・dist プリセットは要求時のみ)。
  OFF スモークを既存 run_build_units_smoke に追加
- OpenXR loader は公式 `OpenXR-SDK` を FetchContent(新規リンクは
  この 1 個。ソース層ライブラリ禁止の対象外 — ランタイム API)
- **XR 無効・ランタイム不在・HMD 未接続はすべて名前入り INFO +
  フラット継続**(XR はフラットの上位互換 — 既存挙動と golden は
  一切変わらないことが全 WP の最重要 gate)

## 2. XR1 — セッションとループ主導権

### 2-1. セッション状態機械

instance → system → session → reference space(STAGE 優先・LOCAL
fallback)。XrSessionState(IDLE/READY/SYNCHRONIZED/VISIBLE/FOCUSED/
STOPPING…)を engine 側の明示状態機械で写し、遷移を get_status に出す。

### 2-2. ループ主導権(2026-07-07 決定の実装形)

- session が SYNCHRONIZED 以上のとき、**フレームペーシングは
  `xrWaitFrame` が握る**(FramerateAdjust は bypass)。フラット時は
  現行どおり
- `xrWaitFrame` の `predictedDisplayTime` を **EngineTime の描画用
  時刻**として供給。ゲームロジックの fixed-step 決定性は不変
  (シミュレーション時刻と表示予測時刻の二本立て — シミュは従来の
  刻み、描画補間だけが predicted time を使う。v1 は簡素に
  「シミュ時刻 = 表示時刻」で開始し、補間分離は未決 1)
- `xrBeginFrame`/`xrEndFrame` + layer 合成(projection layer 1 枚)

### 2-3. テスト戦略(ヘッドセットなし CI)

- **XrApi 関数表の注入点**を engine 側に設ける(loader 直呼びを
  しない)— テストは **fake runtime**(関数表のテストダブル)で
  session 状態機械・wait/begin/end の順序・swapchain 取得を決定的に
  検査する。実 OpenXR loader は player 経路のみ
- OFF スモーク + ランタイム不在時のフラット降格 fixture
- **実機 gate は手動チェックリスト**(§6)— CI は fake のみ

## 3. XR2 — 立体視描画(二段構え)

**方針: 最終形は multiview(2026-07-07 決定のまま)。ただし配達を
二段に分ける** — 実機で絵を見る時期を前倒しし、フレームグラフ改造
(大)を切り離すため。

### 3-1. XR2a — sequential stereo(MVP・実機初表示)

- **XrFrameTarget = IFrameTarget の第 3 実装**: XR swapchain
  (per-view image array)を acquire/release
- **フレームグラフは無改造**: 既存のフレームを **view ごとに 2 回
  実行**し、それぞれの出力を XR swapchain の左右へ。CPU/GPU 2 倍だが
  実装最小
- カメラ: active camera の transform = **ステージ原点(プレイヤーの
  立ち位置)**。XR runtime の per-view pose/fov を「原点からの
  オフセット」として合成し、view/proj を **render-frame snapshot に
  view 別で供給**(WP112 の snapshot 機構をそのまま流用 — カメラ
  API・コントローラは不変。ジッタ表と同じ consumer 規律)
- **TAA/ジッタは XR セッション中は自動 off**(v1 — timewarp との
  干渉回避。per-view TAA は将来)。velocity/history 系 feature は
  view 別に破綻しないことだけ fixture で確認し、v1 は無効を既定に
- UI(screen-space)は v1 では**フラットミラー側のみ**(HMD 内 UI は
  world-space パネルとして将来)

### 3-2. XR2b — multiview 最適化(XR2a 着地後)

- `VK_KHR_multiview` で scene ラスタ群を 1 パス 2 view 化。
  フレームグラフに view_count 属性(パス単位・additive)、FrameUBO の
  view 別行列配列化。フラット時は view_count=1 で従来と byte 一致
- fullscreen/post 系は layer ごと実行 or per-view 配列 RT —
  ここが設計の本丸なので XR2b の敵対レビューで詰める

### 3-3. ミラーウィンドウ

既存 swapchain ウィンドウには**左目をミラー表示**(デバッグ・収録用。
既存 golden/capture 経路はこのミラーではなくフラットパスのまま —
XR off で従来どおり)。

## 4. XR3 — 入力(アクション層の回収)

- I1 の設計どおり: `pelican.input_actions` の action を **XrAction に
  写像**(suggested bindings: Touch コントローラ profile を同梱)。
  button/axis1/axis2/pose が XrAction type に 1:1
- head / aim / grip pose を pose 型 action として入力スナップショットへ
  (L2 から上は無改造 — I1 の約束の回収)
- リプレイ/決定性: **XR セッション中は収録・リプレイ非対応(v1 明記)**。
  pose ストリームの収録は将来(input_seq v2)
- ハンドトラッキングは対象外(将来 action profile 追加)

## 5. XR4 — VRM キャラデモ(ゴールの実証)

`projects/vr_character_demo`: VRM 1.0 キャラ + anim_graph
(Walk/Run/Jump — WP101 のグラフ流用)+ ステージ。コントローラの
スティックでキャラが歩き、Space 相当(トリガー)でジャンプ。
S1b 着地後は表情も。**これが「Quest 3 で 3DCG キャラが動く」の
受け入れ実体**。

## 6. 実機 gate(手動チェックリスト — 私/ユーザーの検収枠)

CI は fake runtime のみのため、各 XR WP の受け入れに**実機チェック
リスト**を添付する: Link 接続 → session FOCUSED 到達 → 両目に正しい
立体視(IPD 反映・歪みなし)→ ヘッドトラッキング追従 → 15 分連続で
crash/validation error なし → フラット復帰。結果はレポートに記録
(スクリーンショット + ミラー録画可)。

## 7. WP 分割

| WP | 内容 | gate | 依存 |
|----|------|------|------|
| **XR1** | ユニット + loader 注入点 + session 状態機械 + wait/begin/end + EngineTime 接続 + fake runtime fixture + フラット降格 | fake で状態機械全遷移・OFF スモーク・既存 golden byte 不変 | なし |
| **XR2a** | XrFrameTarget + sequential stereo + view 別 snapshot 供給 + ミラー + TAA 自動 off | fake で 2 view acquire/submit 順序・**実機チェックリスト(初表示)** | XR1 |
| **XR3** | XrAction 写像 + Touch profile + pose action 接続 | fake で action 状態・実機で入力確認 | XR1(XR2a と並行可) |
| **XR4** | VRM キャラデモ project | 実機チェックリスト(キャラ+アニメ+入力) | XR2a・XR3・(表情は S1b 後) |
| XR2b | multiview 化(フレームグラフ view 次元) | フラット byte 一致 + 実機 + GPU 計測で 2 パス比改善 | XR2a |

## 8. 未決事項

1. シミュレーション時刻と predictedDisplayTime の分離(補間)—
   v1 は同一視で開始、酔い/カクつきの実機所見で判断
2. 深度 submit(runtime reprojection 品質向上)— XR2b と同時に検討
3. world-space UI パネル — UI トラックとの合流点として将来
4. standalone(Android 移植)— 別トラック(Tier 6)
5. コントローラの触覚(haptics)— XR3 の余力次第(action 型は予約)
