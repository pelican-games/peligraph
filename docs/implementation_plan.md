# 実装指示書(コーディングエージェント向け)

> 完了済み WP の逐語記録は [implementation_archive.md](implementation_archive.md) を参照。本書は未完了 WP と共通・運用規則だけを扱う。

対象読者: 実装を担当するコーディングエージェント。各 Work Package (WP) は独立に依頼できる単位として書かれている。

設計の正は以下の 3 文書。本書と矛盾したら設計文書を優先し、矛盾を発見したら作業を止めて報告すること。

- `docs/design_roadmap_renderworld.md` — 全体順序と ECS 境界
- `docs/design_headless_rendering.md` — ヘッドレス描画の設計(以下 [HL])
- `docs/design_shader_freedom_kit.md` — シェーダ基盤の設計(以下 [SF])
- `docs/design_project_format.md` — プロジェクト形式の設計(以下 [PF]。v6 凍結)
- `docs/design_project_format_web_profile.md` — 共通形式 Web プロファイル(以下 [PFW])

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
- モジュールは `DECLARE_MODULE(Name)` + `GET_MODULE(Name)`(container.hpp)。**新規コードではモジュール間依存を関数引数の依存構造体で明示する**(render_pass_dispatch.hpp の `XxxDependencies` 構造体が手本)
- ID 型は `PELICAN_DEFINE_HANDLE`(handle.hpp)、ログは quill の `LOG_INFO(logger, ...)` 系
- エラーは fail-fast(`throw std::runtime_error`)。ただしシェーダコンパイル失敗のみ result 返却([SF] §4.1)
- 外部ライブラリ追加はルート CMakeLists.txt の FetchContent 節に追記

### レイヤ規則

- `src/core/vkcore` から `src/core/renderingpass` 以上のレイヤへ新たな include を**追加しない**(通知はフラグ/イベントで上に渡し、編成は上位層が行う。WP8 が実例)

### テスト規約

- Catch2 v3。`test/CMakeLists.txt` の `pelican_define_test(<name> [libs...])` で登録
- GPU 必須テストは Vulkan デバイス列挙失敗時に `SKIP()` すること

### 禁止事項

- `src/core/ecs/` 以下の変更(担当者が別。必要が生じたら止めて報告)
- 無関係箇所のリフォーマット・リネーム(diff を見る人間のため)
- 挙動変更とリファクタの同一コミット混在
- 1 WP = 1 ブランチ = 1 PR。ブランチ名は `agent/wp<番号>-<短い説明>`

## 1. 現行 WP 一覧

| WP | 内容 | 状態 |
|----|------|------|
| 178 | VRMA-I0 — AnimationSource/graph 接続 + hot reload generation | 進行中(並走 worktree) |

完了済み WP の一覧・依存関係・本文は
[`implementation_archive.md`](implementation_archive.md) に逐語保存する。
(WP172 は 2026-07-18 に完了・検収済み — 本文は archive 参照。)

## 2. WP 詳細

### WP178: VRMA-I0 — AnimationSource/graph 接続 + hot reload generation

参照: **`docs/design_animation_graph.md` v2.1 §4(5 分割の第 5)+
WP177 レポート §6 の引き継ぎ表が正**
(`docs/design_reviews/2026-07-19_wp177_report.md` — R0 から渡すもの/
I0 で追加すべきもの/R0 で行っていないことの三列)。

- **typed AnimationSource / cursor**: `VrmaRetargetedClip` を既存
  AnimationSource 語彙(A1/A2 系)の一員として graph/timeline から
  消費可能に。caller-owned PoseView への転送・rig generation/stale
  検査
- **expression / gaze は joint Pose に畳まない**: R0 の typed sample を
  VRM-S1 の application service(expression/lookAt)へ **typed sink**
  として渡し、同一 frame revision で commit(§1-4 phase 写像)
- **authority**: graph 所有時は apply、timeline/shot 所有時は
  extract-only — source authority ごとの policy(暗黙の二重 writer
  禁止 — §3 の規範)
- **hot reload generation**: asset 単位 generation(WP147 の機構)に
  乗せ、`.vrma`/profile の reload で該当 clip の cursor/pose を
  generation 不一致として reset/rebind(旧 cursor の暗黙再利用禁止 —
  R0 §6 の指示どおり)。status/trace identity 付き
- デモ: projects/vrm_xr_demo(または自己完結 VRM project)に
  `.vrma` 由来モーションを 1 本追加し、既存 Idle/Walk と anim_graph で
  ブレンドが決定的に動くことを実証(合成 fixture 可 — 配布 vrma は
  ローカルのみ)

依存: WP176/177(済)+ WP147(済・generation 機構)+
VRM-S1(済)。見積: 中〜大。
排他: AnimationSource/graph 接続面 + typed sink 配線 + fixture。
**abi_v1.hpp は凍結(additive 公開面のみ可)。vrmadecoder/
vrmaretarget(WP176/177 成果)は消費のみ。renderer 実行系・
communication・editor 面に触らない**。

受け入れ = §4 逐語 + R0 §6 引き継ぎ表の全項 + ブレンド実証(二回
実行 byte 一致)+ reload generation fixture + 既存全テスト無変更 +
golden SKIP 0・byte 不変 + player 8 秒 + CI green

### WP177(済 2026-07-19): VRMA-R0 — versioned retarget/application profile

参照: **`docs/design_animation_graph.md` v2.1 §4 が正**(v2 レビュー
§4.5 の 5 WP 分割の第 4)+ WP176 成果物
(`src/core/loader/vrmadecoder.*`・`src/core/model/vrmaanimation.hpp` —
VrmaClip/typed channel/source rig が入力)。

- **versioned retarget/application profile**: source rig(VrmaClip が
  保持)→ target rig(VRM-S0 の humanoid map)への写像を、版付き
  profile として実装。humanoid bone 名対応・**rest/T-pose 正規化**
  (source と target の rest 姿勢差の吸収)・**optional bone**
  (target に無い bone の skip 規則)・**hips scale**(身長差の
  平行移動スケール)
- **hips translation の root motion 自動解釈は引き続き禁止**(hips は
  scale 適用の平行移動として retarget — 抽出 policy は将来の profile
  項目として枠だけ)
- 出力 = target rig 空間の evaluate 可能な中間表現(**適用系には
  接続しない** — AnimationSource 化・graph 接続は VRMA-I0)
- **DCC provenance を retarget profile にも保持**(v2 レビュー指摘 —
  source clip の provenance + profile version の合成)
- 数値検証: 恒等 rig(source=target)で retarget 結果が入力と一致
  (量子化誤差の許容を明示)・身長 2 倍 rig で hips translation が
  2 倍・optional bone 欠落で該当 track だけ skip・T-pose 差のある
  合成 rig で幾何的に正しい世界姿勢(手計算 fixture)

依存: WP176(済)+ VRM-S0(済 WP111)。見積: 大(リターゲットの
数学が本体 — 慎重に)。
排他: retarget 面(新設ファイル推奨)+ fixture。**アニメ実行系
(AnimationServiceV1/anim_graph/abi_v1.hpp 凍結)・renderer・
communication・editor 面に触らない**。

受け入れ = §4 逐語(rest/T-pose 正規化・optional bone・hips scale・
provenance)+ 数値 fixture 4 系統 + 既存全テスト無変更 + golden
SKIP 0・byte 不変 + player 8 秒 + CI green

### WP176(済 2026-07-19): VRMA-C0 — `.vrma` コンテナ decode + typed channel

参照: **`docs/design_animation_graph.md` v2.1 §4 が正**(条件付き
承認済み — v2 レビュー §4.5 の 5 WP 分割の第 3。VRM-S0/S1 は済):

- `.vrma` = **glb alias + `VRMC_vrm_animation`**。unnamed/first
  animation の既定規則 = `#animation/0`
- **body(humanoid joint)/ expression / gaze を typed channel** として
  decode(joint Pose に混ぜない — UAF の Pose+Curve+Attribute と
  同方向)
- **hips translation を自動で root motion と解釈しない**(抽出は
  import/clip profile の明示 policy — 本 WP では抽出しない)
- **DCC provenance**: Clip metadata に source URI・content hash・
  import profile・tool version を保持(v2 レビューで脱落指摘された
  必須項)
- Clip は model GLB から独立したリソース(source rig 参照を持つ)—
  §1-1 の Clip 規範に従う
- 範囲は **decode/storage/検証まで**。retarget(VRMA-R0)・graph/
  timeline 接続(VRMA-I0)は後続 WP — 適用系に触れない
- 検証: 実 `.vrma` 相当の合成 fixture(humanoid+expression+gaze)+
  拡張 version 検証・不正入力の名前入り reject。VRM-S0(WP111)の
  decoder/検証流儀に揃える

依存: VRM-S0(済 WP111)+ VRM-S1(済 WP121/123/134)。見積: 中。
排他: loader の vrma decode 面(新設)+ fixture。**アニメ実行系
(AnimationServiceV1/anim_graph)・renderer・communication・schema 三
header に触らない**(ABI v1 凍結)。

受け入れ = §4 逐語(alias/既定規則/typed channel/root motion 非自動/
provenance)+ 既存全テスト無変更 + golden SKIP 0・byte 不変 +
player 8 秒 + CI green

### WP175(済 2026-07-19): 負債 CONTRACT0 — 境界 gate

参照: **負債議論 `docs/design_reviews/2026-07-17_debt_discussion_codex.md`
の「WP-CONTRACT0」定義が正**:

> OpenPBR engine/importer 6 variant exact-set、projection consumer
> inventory、`engineMvp` の意味を次 ABI へ移す準備。

範囲(3 点とも関連 N 系発見を全文検索して対象特定):

1. **OpenPBR 6 variant exact-set gate**: engine 側(WP116/117 の
   wrapper-B ×6)と importer 側(usd レーン)の variant 集合が
   exact 一致することを機械検査(片側だけの追加/欠落 = 名前入り
   fail)。v1.1.1 exact pin の検証込み
2. **projection consumer inventory**: 投影行列(projection/jitter)を
   消費する全箇所の台帳化 + 「新 consumer は台帳に登録しないと
   fail-fast」の gate(TAA 一般式 `clip'.xy=clip.xy+jitter·clip.w` の
   適用漏れ防止)
3. **engineMvp の次 ABI 準備**: 現 `engineMvp` の意味論(どの空間・
   どの jitter 適用段か)を文書化し、次期 shader ABI で意味を移す
   ための準備(**現 ABI の変更・シェーダの挙動変更はしない** —
   文書 + 検査のみ。golden byte 不変)

依存: なし。見積: 中。
排他: material/shader contract の検査面 + docs + fixture。
**シェーダ実体・renderer 実行系の挙動変更禁止(検査とドキュメントの
み)。communication・schema 三 header・`.github` に触らない**。
golden byte 不変が gate。

受け入れ = 3 点の gate/台帳 + 既存全テスト無変更 + golden SKIP 0・
byte 不変 + player 8 秒 + CI green

## 3. 保留中のトラック(WP 化待ち)

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

- **最小コマンド層**: (1) ファイル連携済み → (2) WP27 実装済み → (3) `load_gltf` / `update_transforms` は **2026-07-07 に実装 GO 決定**。前提はすべて充足(宛先 = scene v1 の objects[].name / アセット意味論 = WP21)。設計時要件: **複数インスタンス運用**(エージェントが複数エンジンを並行駆動する使い方) — stdio rpc は 1 プロセス 1 クライアントの現行構造を維持しつつ、`get_status`(instance id・project・フレーム番号)を追加してインスタンス識別可能に。プロジェクトは読み取り専有なので並行起動は安全(書き込み系操作を入れる際に排他を設計)。複数クライアント同時接続は TCP/WebSocket 展開時の課題として分離
- **ゲームロジック(ネイティブ C++)**: `design_game_logic_native.md`(2026-07-07 方針決定 — スクリプト不採用、C++ 複数ファイル。G1a システム登録 API → G1b PELICAN_PROJECT ビルド取り込み → G2 DLL ホットリロード)
- **devstudio(Qt)**: 2026-07-07 決定 — `design_devstudio_direction.md`。D1(埋め込みビューポート + アウトライナ)→ D2(ピッキング/ギズモ/編集)→ D3(保存 round-trip/undo)。前提 = 解釈レイヤのターゲット分離(WP44 済)。**D0 追加(2026-07-08 ユーザー決定): エディタ特権の禁止 — 標準 UI もツール。編集系 rpc を D2 の前提として先に定義。WebSocket 展開の優先度引き上げ。`pelican_rpc.py`(薄い rpc クライアント)を小 WP 候補に**
- **カメラシステム**: 2026-07-07 方向決定 — **glTF カメラと同等以上**。glTF の perspective/orthographic を損失なく読み書き(拡張は extras に載せ round-trip 可能に)し、その上にコントローラ(orbit/follow/fly)・カットとブレンド・transform_seq v2 カメラトラックとの統合を積む。設計文書はコントローラ API(GameContext との接続)込みで起草する
- **2D ゲーム機能 + 2D⇔3D 相互変換**: 2026-07-07 方向決定 — 2D ゲーム向け機能(スプライト・直交投影・2D 物理の要否)は必要。UI システムとは**描画基盤(2D パス)を共有し上物を分離**する方針を提案(UI = レイアウト/イベント、2D ゲーム = スプライト/カメラ/物理で要件が異なるため)。**2D⇔3D 相互変換**(2D シーンの 3D 空間配置・3D シーンの 2D 投影編集)は設計文書のスコープ課題として最初に定義すること
- **アニメーショングラフ**: `design_animation_graph.md` **v2.1**(2026-07-12 — ultra リサーチ + 敵対レビューで条件付き承認)。実装順 A0(=WP94 登録済み)→ A1 → A1.5 → A2(受入条件 = レビュー §4.3/§4.4 逐語)→ VRM 5 WP(VRM-S0/S1・VRMA-C0/R0/I0)。A2 以降は A0 の着地を見てから WP 化
- **物理クエリ**: 2026-07-07 **必須決定** — raycast / overlap を GameContext(G1a)とエディタピッキング(D2)の両方に供給する。休眠中の phys モジュール・collision ブランチの再評価から着手。コリジョン形状は glb 内規約(フォーマット方針 §4 予約)と同時に設計
- **OpenXR トラック**: **2026-07-07 に推進決定**。入力(アクション層・pose 型)は準備済み。残り = ①ランタイム統合(xrWaitFrame とループ主導権・EngineTime 統合)②描画(フレームグラフに view 次元 = multiview、XrFrameTarget を IFrameTarget の第 3 実装として追加)③PELICAN_WITH_OPENXR ユニット必須・ヘッドセットなし環境のテスト戦略。設計文書を書いてから WP 化
- **bindless バックエンド**: 2026-07-08 方向決定 — classic(set 2)と併用(`design_material_shading.md` §3-5)。生成アクセサが差を吸収、M2 のデータ形(SSBO + 参照)が前提工事。実装は M2 の後・GPU 駆動系(WP36 パーティクル・大規模シーン)の需要と同時に WP 化。**web/モバイルの床に PC を縛らせない**(web は将来やるとしてもシンプルな 3D/2D — ユーザー確認)
- **web ビルド(WASM)**: 将来の可能性としてのみ保持(2026-07-08)。守るべき不変条件は全部現行規律(純ロジック規律・データ契約が抽象・classic 床・dist-bake/WGSL レーン)— 特別な保全作業なし。進めるときは案 B(WASM ゲームコア + TS レンダラ接合)→ 案 A(C++ WebGPU 実行系、データ契約の兄弟執行器)。**RHI の後付けは禁止**(本体の C++ インターフェースへの制約源にしない)
- **アセットホットリロード**: v2 の HR0/HR1/HR1-T/HR1-M/HR2-S(WP108)/HR2-G(WP110)まで完了。確定規約(リプレイ/strict/rpc 中無効・自己書き込み `(AssetKey, hash, epoch)` token)と単一 FileWatcher 経路を維持する。残りは HR2-I(input/profile)
- **イベント層**: `design_event_layer.md` v1 ドラフト(2026-07-08)。**API 意味論(emit/購読の書き味・フレーム境界配送)のユーザーレビューを経てから E1 を WP 化**。E2(物理トリガー)は E1 後
- **永続化(user:// + 設定/セーブ)**: `design_persistence.md` v1 ドラフト(2026-07-08)。**user:// スキーム追加 = [PF] v6.3 の凍結改訂が必要 — ユーザー承認待ち**。承認後 P1 を WP 化
- **[PF] v6.3 改訂案(一括)**: ①user://(persistence)②asset store マウント + .pelican/local.json(`design_project_vcs.md`)③#フラグメント参照(`design_asset_containers.md`)。**3 点まとめてユーザー承認を取り、1 回の版数改訂で凍結文書へ反映**。承認後の WP: P1 / V1〜V3 / K1〜K4
- **コンテナアセット**: `design_asset_containers.md` v1 ドラフト。K1 フラグメント参照 → K2 glTF シーン抽出(scene v1 親子改訂と同時)→ K3 pelican-import-tools 創設(PSD = psd-tools、アトラスパック)→ K4 import ルール表。PSD 系はエンジン非リンク(外部ツール契約)
- **RenderWorld / ECS**: (2026-07-02 方針変更)統合ブランチ系列を開発本線として独自に進める。**(2026-07-08 改訂・ユーザー決定)`src/core/ecs/` の変更禁止を解除** — 凍結の代償(PhysWorld 生ポインタ・カメラ二重所有・dummy コンポーネント・cb_deinit 不呼出で非自明型が memcpy 移動される)が 2026-07-08 リファクタ監査で顕在化したため。以後 ECS コアに世代付き EntityId・construct/move/destroy 規約等を入れてよい。main との合流は「随時追従 merge」から「将来の逆提案(こちらの ECS 改良を main へ提案)」へ位置づけ変更。**互換受理の追加禁止(同日決定)**: ランタイムは v1 だけを読む。旧形式の変換が要る場合は外部ツールで行う — 以後の WP が新旧両対応の受理コードを足すことを §0 違反とする
- **コマンド層の WebSocket 展開**: stage 2(stdio JSON-RPC)実装後、同じメソッド群を WebSocket に載せると devstudio と web viewer(my_webpage)が同一プロトコルでエンジンを叩ける([PFW] §7)。stage 2 の後に設計文書を書いてから WP 化
- **asset manifest(sha256)**: `assets.manifest.json` + 起動前検証([PF] §5-6 の予告)。WP18c の README 一覧表で当面代替し、需要(=黒背景事故の再発 or web 側キャッシュ検証の要求)が出たら WP 化
- **naga 変換のビルドスクリプト化**: [PFW] §4-3(a) の WGSL→SPIR-V 一括変換を node CLI 化(web repo 側作業。実績コードは `apps/site/src/lib/shader/nagaSpirvCompiler.ts`)。WW3 の後
- **プロジェクト解釈レイヤ(pelican_project 分離)**: `design_project_interpretation_layer.md`(v1 ドラフト)。解釈(パース・検証・パス解決・正規化)をエンジン非依存の静的ライブラリに分離し、エンジン側は薄いバインダにする。WP19 で rendering config の解釈から着手し、CMake ターゲット分離は後続の小 WP(移動+委譲のみ、WP3 の流儀)。WP21(pelican_cli import)がライブラリの最初のエンジン外利用者になる予定
- compute パス / GPU 計測 / bindless / RT: それぞれ設計文書を書いてから WP 化(ロードマップ §2 の順)

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
