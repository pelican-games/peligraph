# 第11章 実装状況と文書マップ

対象: pelican2(2026-07-31 時点・WP238e 完了地点、HEAD=`d1c8081`、branch
`codex/render-target-runtime-slice`)/ このマニュアルはコードを正とする

## この章で学ぶこと

- WP(Work Package)全台帳 — 何が実装済みで、何が登録のみ・予約なのか
- 設計文書マップ — docs/ の各文書と本マニュアルの章の対応、状態(凍結/ドラフト/古い)
- 既知の「設計文書とコードの食い違い」一覧
- 本マニュアル自体の更新ルール

## 11.1 WP 全台帳

判定は「実装コミット + test 登録 + design_reviews の完了レポート +
implementation archive」によります(WP1〜179 は 2026-07-19 調査、WP180 以降は
2026-07-31 にコミットと `src/` 上の型の実在で再判定)。

凡例: **済** = マージ済みで機能する / **済(外部 gate 待ち)** = 実装と自動テストは
完了しているが実機・実測の受け入れが未消化 / **未** = 登録済み・未実装 /
**予約** = 番号予約のみ。

### WP1〜63(基盤期。詳細は各章)

| WP | 内容 | 状態 |
|---|---|---|
| 1 | EngineLaunchConfig + player CLI | 済 |
| 2 | EngineTime(仮想時刻の注入) | 済 |
| 3 | RenderTarget の facade 化 | 済 |
| 4 | surface なし Vulkan 起動 | 済 |
| 5 | OffscreenFrameTarget | 済 |
| 6 | readback + PNG 出力 + CLI 完成 | 済 |
| 7 | headless 描画テスト | 済 |
| 8 | リサイズ時の RT 追従 | 済 |
| 9 | 遅延破棄機構(DeletionQueue) | 済 |
| 10 | shaderc / spirv-reflect / stb 導入 | 済 |
| 11 | ShaderCompiler + ShaderReflection | 済 |
| 12 | ShaderLibrary | 済 |
| 13 | PipelineFactory | 済 |
| 14 | シェーダホットリロード | 済 |
| 15 | Material/UiRenderer 移行 + descriptor set 規約 | 済 |
| 16 | ゴールデンイメージテスト基盤 | 済 |
| 17 | SeqPlayer(transform_seq 再生) | 済 |
| 18 | プロジェクト形式(project.json + PathResolver + example 切り出し) | 済 |
| 19 | シェーダ stem 解決 + rendering config の struct 化 | 済 |
| 20 | VAT 再生(pelican.vat v1) | 済 |
| 21 | devcli `import`(pelican.import manifest) | 済 |
| 22 | pointcache | 予約 |
| 23 | KTX2 | (WP92 として実装済み) |
| 24 | 音声 | (WP51 = A1 として実装済み) |
| 25 | pelican.scene v1 | 済 |
| 26 | EXR リーダ(tinyexr・限定スコープ) | 済 |
| 27 | コマンド層 stage 2(stdio JSON-RPC 2.0) | 済 |
| 28 | feature 合成基盤 | 済 |
| 29 | debug_draw feature + GPU/CPU 計測 | 済 |
| 30 | HDR / トーンマップ feature | 済 |
| 31 | shadow directional feature | 済 |
| 32 | IBL | 予約 |
| 33 | フレームグラフ F0(プランナ) | 済 |
| 34 | フレームグラフ F1(compute 実行系) | 済 |
| 35 | フレームグラフ F2(プランダンプ + rpc) | 済 |
| 36 | GPU パーティクル | 予約 |
| 37 | 入力システム(スナップショット) | 済 |
| 38 | スケルタルアニメーション(クリップ v1) | 済(2026-07-12。`#animation/` 参照 + matrix palette) |
| 39 | 入力アクション層 I1 | 済 |
| 40 | ビルドユニット化 B1(PELICAN_WITH_* + OFF スモーク) | 済 |
| 41 | `pelican_cli dist-config` B2 | 済 |
| 42 | コマンド層 stage 3(get_status / update_transforms / load_gltf) | 済 |
| 43 | ゲームシステム登録 API G1a | 済 |
| 44 | pelican_project ターゲット分離(解釈レイヤ) | 済 |
| 45 | プロジェクトコード取り込み G1b(PELICAN_PROJECT) | 済 |
| 46 | 物理クエリ P1(純ロジック) | 済 |
| 47 | 物理クエリ P2(collider シーン接続) | 済 |
| 48 | カメラ C1(glTF 1:1 投影 + set_camera) | 済 |
| 49 | 入力 I2(rpc inject_input) | 済 |
| 50 | カメラ C2(orbit / follow / fly) | 済 |
| 51 | オーディオ A1(WAV + バス + null バックエンド) | 済 |
| 52 | シーン遷移 S1(loadScene) | 済 |
| 53 | 決定性乱数(PCG32) | 済 |
| 54 | debug_text feature | 済 |
| 55 | [PF] v6.3 リゾルバ拡張(user:// / asset store / #フラグメント構文) | 済 |
| 56 | イベント層 E1 | 済 |
| 57 | project init 雛形 V3 | 済 |
| 58 | マテリアル M1(pelican.material パーサ + shader_contract.md) | 済(レンダラ接続は WP68/70/76/78 で完了) |
| 59 | スパイク pelican-spv-link | 済(スパイク。本実装は WP80 = experimental) |
| 60 | リファクタ R1(死荷重削除。`--project-settings` 廃止等) | 済 |
| 61 | リファクタ R2(カメラキー正規形移行・データのみ) | 済 |
| 62 | リファクタ R5-core(ECS ライフサイクル + 世代 ID) | 済 |
| 63 | リファクタ R3(strict v1 化 — 旧キー・レガシー受理の全廃) | 済 |

### WP64〜111(機能拡張期 2026-07-10〜16)

| WP | タイトル | 状態 | 1行説明 | ユーザーから見える面 |
|---|---|---|---|---|
| 64 | リファクタ R4 — legacy 実行系の撤去 | 済 | 全 rendering config が planned frame graph 実行に一本化(A/B trace 差分ゲート付き) | 純 render 構成でも `before`/`after` 明示エッジが効く(挙動修正) |
| 65 | 永続化 P1(user:// 設定 + セーブ) | 済 | pelican.settings v1(engine/game 区画・atomic 書き込み)+ user://saves/ | API: `GameContext::saveSettings()` / `saveData(slot,json)` / `loadData(slot)` / `listSaves()` |
| 66 | assets manifest V2 | 済 | store 走査 → sha256 manifest 生成・照合・起動時検証(INFO/WARN/ERROR、ロードは止めない) | CLI: `pelican_cli assets manifest/verify/status`、起動フラグ `--strict-assets` |
| 67 | modelview 統合(R5 後続) | 済 | SimpleModelView 2 コンポーネントを統合、隠し自動生成を撤去 | scene JSON の `simplemodelview.model` の書き味は不変 |
| 68 | マテリアル M2a — .surface パーサ | 済 | `//! pelican.surface v1` ヘッダ(ordered params / textures / screen_inputs)+ material JSON の values 方式 | JSON: `.surface` ヘッダ形式、`.material.json` の `values` |
| 69 | FrameInput(入力の順序付きイベント化) | 済 | ordered InputEvent + event_seq + 5 段フレーム位相 + 消費マスク API | API: `FrameInput{ordered_events, snapshot}`(通常/rpc loop 位相同一) |
| 70 | マテリアル M2b-1 — FrameUBO | 済 | set0 b0 FrameUBO 新設(time が全マテリアルへ)、material データの SSBO 化、push constant 64B/64B 分割 | シェーダ契約: FrameUBO(time/dt/frame_index/resolution/camera/view/proj)。shader_contract.md の差分 3 件解消 |
| 71 | EventPayloadSchema | 済 | イベント payload の explicit descriptor + 構築前 JSON 検証 | Opaque の by-name emit は廃止(typed C++ emit のみ)。rpc emit の payload 検証規則が確定 |
| 72 | 色 C0 — 色空間監査 | 済 | コード変更なしの全域監査 | 成果物: `docs/color_migration_manifest.json` |
| 73 | 色 C1a — canonical anchor(構造のみ) | 済 | canonical anchor 列 / format_class / output_transform 常設ノード(golden byte 不変) | frame plan に anchor / output_transform が現れる |
| 74 | 色 C1b — 色意味論の一括移行 | 済 | SRGB swapchain + linear workflow へ全面移行、golden 再基準化(resolver_version: 2) | 画の色が正しくなる(一度きりの再基準化)。SRGB/UNORM の role 別 view |
| 75 | UI U0 — 純 CPU の UI 基盤 | 済 | pelican.ui schema / layout / 入力状態機械 / semantic validator | JSON: `pelican.ui` v1(schema 正本 = `docs/schemas/pelican.ui.schema.json` ほか) |
| 76 | マテリアル M2b-2 — values バインダ | 済 | .surface params → std140 SSBO、textures 辞書 binding(SRGB/UNORM view)、render_state | CLI: `pelican_cli dump-lowered-material <surface>` |
| 77 | コンテナ K1 — #フラグメント実ロード | 済 | glb の `#mesh/#material/#node/#animation` 部分ロード | asset_data / scene から `foo.glb#mesh/名前` 参照が使える |
| 78 | マテリアル M3a — B 層ソース経路 | 済 | .surface コードスニペット → テンプレート合成(`pelican_*_v1` フック反射、engine:// GLSL ライブラリ) | .surface に `pelican_surface_v1` 等を書くだけでカスタムシェーダ。standard/toon は同経路の dogfood |
| 79 | コンテナ K2 — glTF シーン抽出 + 親子 | 済 | scene v1 に `objects[].parent` 追加 + glTF ノード階層の抽出 | CLI: `pelican_cli import gltf --extract-scene <glb>`。JSON: scene v1 `parent` キー |
| 80 | マテリアル M3b — spv-link | 済 | SPIR-V リンクの experimental バックエンド(既定は WP78 ソース経路) | build: `PELICAN_WITH_SPIRV_LINK=ON` + runtime: `PELICAN_SPV_LINK=experimental`。昇格判定は保留 |
| 81 | コンテナ K3 — pelican-import-tools | 済(別リポジトリ) | Python 製外部ツール: psd_extract + atlas_pack(決定的パッキング) | 出力: `pelican.atlas` v1 JSON(`#sprite/<名前>` の解決先) |
| 82 | 起動高速化 | 済 | シェーダディスクキャッシュ(`.pelican/shader_cache/`)+ モデルロード並列化(決定的) | 起動 1 行レポート + `get_status.startup`。2 回目以降の起動が大幅短縮 |
| 83 | マテリアル M3.5 — スクリーンスナップショット | 済 | rendering config `"snapshots"` + `screen_inputs` 宣言で forward 自動振り分け | JSON: `"snapshots":[{"name","after"}]`、.surface `//! screen_inputs: [...]`、シェーダ `pelican_screen_<name>(uv)` |
| 84 | コンテナ K4 — import ルール表 | 済 | `imports.rules.json`(glob → レシピ)。外部ツール契約で起動 | CLI: `pelican_cli import --rules` |
| 85 | ImGui 導入 | 済 | PELICAN_WITH_IMGUI ユニット(dist は OFF)。golden/replay/headless では非実行 | F1(actions 経由)でデバッグ UI トグル |
| 86 | Plan viewer(ImGui) | 済 | frame plan のノードグラフ可視化(パス = ノード、RT = エッジ) | ImGui 内ツール。データ源は get_frame_plan と同一意味論 |
| 87 | UI U1 — GPU 描画 | 済 | quad buffer(20B 頂点)+ pelican.atlas 接続 + nested clip + ui の purgeable feature 化 | `engine://features/ui.json`。UI の `#sprite/` 参照。**旧 ui_overlay images 形式は廃止** |
| 88 | temporal T1+T2 — history + velocity | 済 | RT 宣言 `"history": true`(2 面自動管理・`@history` 読み)+ velocity feature(RG16F) | JSON: RT の `history` キー、`@history` 参照。TAA stdlib は WP112〜115 で実装済み |
| 89 | 入力 I3 — 収録とリプレイ | 済 | ordered InputEvent 列を `pelican.input_seq` v1(JSONL)へ収録・リプレイ(2 回 byte 一致) | rpc: `start_input_record`/`stop_input_record`、CLI: `--replay <file>`、`pelican_cli bake-camera --replay` |
| 90 | G2 — ゲームロジック DLL ホットリロード | 済 | 実行中の再ロード(v1 = 全リセット方式)。ABI 版数ゲート・失敗時は旧 DLL 継続 | 保存 → 数秒で反映。リロード中 rpc/replay 拒否 |
| 91 | 入力 I4 — ゲームパッド + プロファイル | 済 | GLFW gamepad API。actions を「アクション定義」と「バインディングプロファイル」に分離 | JSON: `profiles/*.json`(起動引数/設定/rpc で切替)。pad 入力は収録リプレイ対応 |
| 92 | KTX2 テクスチャレーン | 済 | KTX2 制限サブセット自前パース(RGBA8 / BC7 / BC5、supercompression なし・全ミップ必須) | KTX2 が material textures 辞書・UI atlas で使える |
| 93 | UI U2 — 対話ウィジェット | 済 | bitmap label / button + E1 emit(WP71 照合・次フレーム配送)+ rpc click/replay の決定性 | UI JSON の emit 文法 |
| 94 | アニメ A0 — 公開契約の凍結 | 済 | animation ABI v1 の凍結(handle/struct_size/negotiation) | 正本: `src/core/userpublic/animation/abi_v1.hpp` + [animation_abi_v1.md](../animation_abi_v1.md) |
| 95 | skinned velocity の previous palette | 済 | 骨だけ動く場合の deformation velocity = 0 を解消 | velocity buffer が骨変形を正しく反映 |
| 96 | アセット HR0 — watcher/reconcile 基盤 | 済 | Win32 FileWatcher 状態機械 + ContentDigest + gate epoch(リプレイ/strict/rpc 中無効) | `get_status.reload`(watcher 状態) |
| 97 | アニメ A1 — 機構 jobs | 済 | 凍結 ABI の実体化(samplePoseAt / advanceCursor / N-way blend / commit) | ABI v1 の全関数が実動作。既存 clip component の挙動不変 |
| 98 | アセット HR1 — identity/transaction 基盤 | 済 | LogicalAssetId / generation / content_revision の三分離 + staged commit + rollback | `get_status.reload` に resource counters/errors |
| 99 | アニメ A1.5 — 敵対 fixture | 済 | 別 rig 誤ブレンド検出・loop 跨ぎ・stale handle 等の敵対検証 | (テスト資産のみ) |
| 100 | アセット HR1-T — テクスチャ差し替え | 済 | png/EXR/KTX2 の watch → in-place / descriptor 再バインド | 画像保存 → 実行中の絵が更新。失敗時は旧絵継続 |
| 101 | アニメ A2 — anim_graph v1 + 標準評価器 | 済 | `pelican.anim_graph` v1(state/blend1d/crossfade/interrupt)+ ユーザー空間標準評価器。※公開語彙不足で一度実装前停止 → WP102 着地後に完了 | JSON: `pelican.anim_graph` v1。example に歩き↔走り + ジャンプ割込みデモ |
| 102 | アニメ A1.1 — 公開 service surface | 済 | versioned service table を additive 追加(sink claim/release・phase callback 自動失効) | 第三者 game DLL が公開面だけで評価器を書ける |
| 103 | 2D S2D-0a — スプライト契約 + CPU 基盤 | 済 | `sprite_view` コンポーネント + SpriteCommand ABI + sort total key(GPU なし) | JSON: scene の `sprite_view`(asset 宣言 + `#sprite/` 参照) |
| 104 | 2D S2D-0b — GPU world quad パス | 済 | world 空間スプライト描画(3D 不透明の後、depth test ON / write OFF)+ billboard | スプライトが 3D と遮蔽し合って表示される |
| 105 | アセット HR1-M — material values 差し替え | 済 | `.material.json` の watch → 同 layout なら SSBO 値のみ update | material JSON 保存 → 実行中に値反映 |
| 106 | 2D S2D-1 — strict pixel policy + flipbook | 済 | render_only_quantization 方式の pixel perfect + 公開 FlipbookClip | JSON: `basic_config.sprite.pixels_per_unit`、camera の `sprite.pixel_perfect/sort` |
| 107 | 2D S2D-P — shapeCast + Provider ABI V2 | 済 | 固定姿勢 sweep(sphere/box/capsule 全 9 組・TOI/MTD)+ 物理 Provider ABI V2 | API: GameContext に `shapeCast` all/closest。collider に `layer/mask/trigger/one_way` |
| 108 | アセット HR2-S — shader/surface transaction | 済 | 単一 FileWatcher 経路化 + 全 variant/pipeline の atomic 差し替え + cross-file transaction | シェーダ/.surface 保存 → 実行中に atomic 反映 |
| 109 | 2D S2D-2 — side-scroller vertical slice | 済 | 非特権 `moveAndSlide`(`src/core/userpublic/platformer/`)+ 接地/斜面/one-way、replay 2 回 bit 一致 | `projects/sprite_demo` が操作可能なプラットフォーマーに |
| 110 | アセット HR2-G — model/fragment hot reload | 済 | 同一 container の全 fragment 一括 transaction + live instance の bulk rebuild | glb 保存 → 配置済みインスタンスが実行中に差し替わる |
| 111 | VRM-S0 — .vrm semantic decoder | 済(2026-07-16) | VRM 1.0(`VRMC_vrm`)の humanoid/expressions/lookAt を保持・検証・canonical dump(renderer 適用なし)。VRM 0.x は素の GLB + INFO | CLI: `pelican_cli vrm dump`。実装 `src/core/model/vrmsemantic.*` |

### WP112〜179(TAA / OpenPBR / XR / editor / VRMA / trigger、2026-07-16〜19)

| WP | タイトル | 状態 | 1行説明 | ユーザーから見える面 |
|---|---|---|---|---|
| 112 | J1 — projection jitter 機構 + feature named binding | 済 | Halton23 系列・consumer 別配送(主 raster = jittered / velocity・SSAO・sprite = FrameUBO / shadow・culling・rpc = 不変)+ FrameUBO に jitter_ndc/epoch ペアを additive 追加 | feature JSON: `projection_jitter` 宣言 + instance `{ref, parameters}`。既定 off(golden byte 不変) |
| 113 | T-TAA — 標準 TAA feature(ユーザー空間 stdlib) | 済(再派遣) | 一度実装前停止(スカラー配送不能)→ WP114 着地後に完了。resolve + composite の二パス・エンジン本体無変更 | `engine://features/taa.json`(コピーして改造 = 自分のもの)。1 行で有効化 |
| 114 | J1b — feature スカラー parameter → shader defines | 済 | instance `parameters` の float/int/bool を型・range 検証し per-feature define へ lower(GPU ABI 無変更) | feature JSON: parameter 宣言に `type` / `range` / `default` |
| 115 | J1c — ジッタ系列のユーザー定義 | 済 | `pattern:"table"` + `offsets_px` 直書き(1..64・[-0.5,0.5) 検証)。halton23 と同数表で全 byte 一致 = 名前付き系列は特権なしの証明 | feature JSON: `pattern:"table"` / `offsets_px` |
| 116 | M-PBR0a — OpenPBR 表現/routing ABI | 済 | lighting hook + forward 経路・per-material custom texture override・`{opaque,mask,blend}×{single,double}` 6 variant routing・primitive binding ABI | JSON: pelican.material に `alpha_mode` / `double_sided` / texture override(additive)+ `pelican.material_bindings` v1 |
| 117 | M-PBR0b — openpbr surface + 写像表 | 済 | wrapper-B(薄い `.surface` ×6 + 単一 GLSL include)で OpenPBR Surface 1.1.1 subset を特権なし実装 | 第 3 の stdlib surface `openpbr`(standard/toon と同格)。example にサンプル球 |
| 118 | U-USD0a — USD ツール選定 spike | 済(レポートのみ) | **usd-core==26.5 採用**。UsdMtlx は wheel 非同梱 → MaterialX は authored 値のみに縮退確定 | 成果物 = spike レポート + import-tools の probe |
| 119 | U-USD0b — USD コンポジション/静的ジオメトリ変換 | 済 | import-tools に production `usd` レシピ(USD/USDZ → GLB + scene v1 断片。決定的・SHA-256 二回一致) | import-tools CLI。エンジン側は fixture + golden |
| 120 | VRM-S1 — application sink | 監査停止 → 分割 | morph 描画機構の全経路不在を検出した正しい停止 → S1a/M-INST0/S1b に再編 | (停止レポートが S1a/S1b 仕様の正) |
| 121 | VRM-S1a — glTF morph target 描画機構 | 済 | POSITION/NORMAL/TANGENT delta の共有 GPU storage + per-instance weight(N/N-1)を全 vertex path で skinning 前に適用(VRM 非依存) | glb の `primitive.targets` + `mesh/node.weights` がそのまま描画に効く |
| 122 | M-INST0 — per-instance material override ABI | 済 | instance 単位の factor/UV multiplier override(N/N-1・generation)。override なし instance はコスト 0 | renderer 内部 ABI(公開面は WP123b 経由) |
| 122b | M-INST1 — material 別 absolute override | 済 | `(instance, glTF material index)` 単位の版付き absolute override + 初期値 metadata | renderer 内部 ABI(VRM materialColorBinds の根拠) |
| 123 | VRM-S1b — per-instance expression sink | 停止 → 済(123b) | 4 回目の正しい停止(WP122 が絶対 target を表現不能)→ WP122b 先行 + 再スコープで完了 | **公開 versioned application service** を additive 追加 — VRM 1.0 の表情が per-instance で動く([第8章](08_gameplay.md)) |
| 124 | U-USD0c — USD マテリアル/binding 変換 | 済 | UsdPreviewSurface → OpenPBR 写像 → pelican.material、PNG 抽出 + colorspace 記録、subset binding → binding document | import-tools usd レシピ拡張。USD → OpenPBR 描画の golden |
| 125 | XR0 — OpenXR build unit / activation | 済 | `PELICAN_WITH_OPENXR`(dev 既定 ON・dist 既定 OFF)+ activation 三値の edge 行列 | CLI: `--xr off/auto/on`、`dist-config --with openxr` |
| 126 | XR1a — discovery + Vulkan bootstrap | 済 | GPU リソース作成前に XrInstance/XrSystem + `XR_KHR_vulkan_enable2` で VkDevice 生成。失敗時は一度だけ flat 降格 | `--xr auto/on` の実挙動(auto 不在 = INFO+flat / on 不在 = hard error) |
| 127 | XR1b — session 状態機械 + ループ | 済 | event 駆動状態機械・running 中の wait/begin/end 毎フレーム一組・display timing を EngineTime から分離 | 内部(headless/rpc/golden/replay は running に入らない) |
| 128 | XR2a.0 — renderer 論理フレーム/view 分離 | 済 | `renderLogicalFrame()` + per-view の camera/projection/FrameUBO/graph 実行(in-flight × view slot)。view_count=1 で golden byte 一致 | 内部(既存 render() は互換アダプタ) |
| 129 | XR2a.1 — XR composition target / swapchain | 済 | 別 interface `IXrCompositionTarget`: per-view swapchain ×2・per-swapchain 状態機械・単一 xrEndFrame | 内部(`src/core/openxr`) |
| 130 | XR3a — action set / suggested bindings | 済 | `pelican.input_actions` を XrAction 化 + Touch suggested bindings + 非 FOCUSED = inactive | 既存 actions.json が**そのまま Touch controller で効く** |
| 131 | XR2a.2 — view snapshot / reference space | 済 | 凍結行列規範 + reference space 優先順 STAGE → LOCAL_FLOOR → LOCAL | rpc: `get_status.xr`。camera API 不変 |
| 132 | XR3b — pose provider | 済 | L1 に frame-typed pose レーン(aim/grip = action space・head = synthetic)。pose の record/replay は v1 非対応(名前入り error) | API: `actionPose` が実動作に(旧「常に throw」から変更) |
| 133 | XR2a.3 — feature policy + mirror(実機初表示) | 済 | flat/XR 両 graph を起動時 precompile(XR graph = TAA/jitter/velocity/history/UI 除外)+ 左眼 mirror(best-effort) | `--xr on` で HMD 表示 + ウィンドウにミラー |
| 134 | S1c — bone lookAt + firstPerson | 済 | application service に版付き pose staging + VRM bone lookAt(rangeMap)+ firstPerson(auto head-split・per-view visibility) | XR first-person view で頭部非表示・視線追従 |
| 135 | XR4 — VRM キャラデモ(flat 先行) | 済 | `projects/vrm_xr_demo/` 新設(エンジン変更なし): 決定的 VRM + anim_graph + 表情サイクル + 視線追従。flat で全検証・XR は起動フラグのみ | 新デモ [projects/vrm_xr_demo](../../projects/vrm_xr_demo)(自分の .vrm への差し替え手順付き) |
| 136 | XR Simulator smoke | 済 | Meta XR Simulator v201.0 を署名/hash検証して実 runtime smoke | 配布・起動・runtime選定手順をレポート化 |
| 137 | 負債 CI0 — Windows CPU ゲート | 済(2026-07-17) | GitHub Actions の CPU-only テストゲート(gpu ラベル除外・SKIP は完全一致 allowlist・リトライなし) | `.github/workflows/`(push/PR で自動実行) |
| 138 | XRSIM blocker 修正 | 済 | ImGui frame、timeline semaphore、mirror extent、XR diagnostics を修正 | Simulator 1000 frame、3分超、validation 0 |
| 139 | D-P0a — debug-utils/labels | 済 | optional `VK_EXT_debug_utils` と object/command/queue label | `--gpu-labels`、既定 OFF |
| 140 | D-P1a — RenderDoc capture | 済 | 注入済み RenderDoc のみ受動利用し一フレーム capture | F11 / RPC `capture_gpu` |
| 141 | GOLDEN0 | 済 | golden 正本を件数定数から `inventory.json` へ移行 | CPU gate でfixture/hashを照合 |
| 142 | LIGHT0 | 済 | core の magic-name light animation を撤去し user system 化 | light cap超過を名前付きWARN |
| 143 | D-P2a — stereo-safe GPU timing | 済 | logical frame/variant/view/node/mirror 別 timestamp | RPC/ImGui timing snapshot |
| 144 | TRANSIENT0 | 済 | `load_gltf` を inspect/stage/single-publish transaction 化 | 失敗時に既存ID/資源を不変化 |
| 145 | D-P2b — VRAM/XR timing | 済 | driver heap と engine usage を分離、XR frame counters/QPC変換 | `get_status.memory/xr` |
| 146 | INSTANCE0 | 済 | `ModelInstanceId` を generation + scene epoch SlotMap 化 | stale/ABA操作を拒否 |
| 147 | ANIM0 | 済 | model reloadをasset単位generation + evaluator rebindへ変更 | 別assetを巻き込む全体resetを撤去 |
| 148 | ECS0 | 済 | scheduler hazard/DAG照合、既定serial化・strict fail-fast | conflictをsystem/component名付き診断 |
| 149 | ED-AUTH0 | 済 | raw semantic JSONを唯一正本とする `AuthoringSceneDocument` | stable revision/authoring ID |
| 150 | STRUCT-SCHEMA0 | 済 | Event/Behavior/Component共通field schema + use-site policy | Inspector/codecの型情報基盤 |
| 151 | ED-CODEC0 | 済 | 7 componentのdecode/encode/schema/apply/project五つ組 | schema-driven editing |
| 152 | ECS-MUT0 | 済 | 同じEntityIdを保つfailure-atomic archetype migration | component add/remove transaction |
| 153 | E-PROJTX0 | 済 | documentと全runtime adapterのaggregate transaction | prepare後noexcept publish/rollback |
| 154 | E-RPC0-base | 済 | scene tree/components/assets/snapshot queryをtyped service化 | RPCとImGuiが同じserviceを利用 |
| 155 | BEH0 | 済 | engine-owned behavior attachment arena/lifecycle/event | project DLL behavior |
| 156 | E-HOST0 | 済 | windowed stdio RPCをbounded queueでframe boundary dispatch | `--rpc` windowed対応 |
| 157 | E-RPC1a | 済 | ActorId/CAS/edit RPC/JOURNAL0 | durable editor transaction |
| 158 | E-PROJTX0b | 済 | structural stage、prepared ECS/adapter token、commit hook | WP157のproduction基盤 |
| 159 | UI-AB0 | 済 | read-only ImGui Asset Browser | provenance/store/load status表示 |
| 160 | `pelican_rpc.py` | 済 | Python 3.12標準ライブラリだけのthin RPC client | process lifecycle + helper API |
| 161 | E-RPC1b | 済 | actor別undo/redo、ticket/live preview lease | preview epoch/CAS gate |
| 162 | BEH1 | 済 | game DLL二世代side-decode後のatomic reload | 不適合候補は旧runtime不変で拒否 |
| 163 | ECS1 | 済 | generation付きregistration tokenとowner dependency purge | DLL unload前の依存検証 |
| 164 | UI-INS0 | 済 | Object Tree + schema-driven Inspector | edit/preview/undo/save UI |
| 165 | CI1 | 済 | 9 build-unit + project code + clean-clone matrix | PHYSICS OFF stubも固定 |
| 166 | SAVE0 | 済 | document正本のatomic whole-scene save | digest/CAS/validation付き `save_scene` |
| 167 | BEH2 | 済 | behavior attach/remove/set-paramをeditor transactionへ統合 | Inspectorから編集可能 |
| 168 | SNAPSHOT0 | 済 | version/hash/size検証付きscene snapshot import | file非変更のscratch session |
| 169 | DTXT0 | 済 | DebugText/UI bitmap layoutを共通化 | production A/B RGBA8完全一致 |
| 170 | WATCH0 | 済 | scene revision + preview epoch watch token | Inspector/Object Tree差分refresh |
| 171 | LIFETIME0 | 済 | explicit teardown drain順とcallback lease | 例外後も後続drainを継続 |
| 172 | PREVIEW0 | 済 | live不変の`eval_preview`と独立`render_preview` graph | request-local evaluation/capture |
| 173 | PORT0 | 済 | growable atlas pool、CreateProcessW、import timeout/cancel/log | portability quick fixes |
| 174 | TEST0 | 済 | golden harness分割、active/archive台帳、memory-budget flaky修正 | test/docs保守性 |
| 175 | CONTRACT0 | 済 | OpenPBR exact set、projection inventory、engineMvp意味 gate | GPU不要contract CTest |
| 176 | VRMA-C0 | 済 | `.vrma` GLB aliasをbody/expression/gaze typed channelへdecode | provenance/hash付きimmutable clip |
| 177 | VRMA-R0 | 済 | versioned humanoid retarget profile | rest/T-pose、optional bone、hips scale |
| 178 | VRMA-I0 | 済 | typed AnimationSource/graph/sink + generation/rebind | VRMA body/表情/視線を同revision適用 |
| 179 | E2 — physics trigger | 済 | 決定的な対称Enter/Exit、stay抑止、destroy/remove Exit | typed behaviorへ通常E1配送 |

### WP180〜202b(RPE — レンダーパイプライン拡張境界、2026-07-22〜25)

| WP | タイトル | 状態 | 1行説明 | ユーザーから見える面 |
|---|---|---|---|---|
| 180 | RPE1 — typed render-pipeline resolve boundary | 済 | registration/preview/XR に散っていた authoring 解決を GPU mutation 前の純粋境界 `resolveRenderPipeline()` へ集約 | 見えない(挙動不変・byte 一致が受け入れ条件) |
| 181 | RPE2 — typed immutable `CompiledRenderPipeline` | 済 | runtime が JSON key を読むのをやめ、`FramePlan::composition_metadata` を撤去 | frame-plan dump は typed plan からその場 serialize(byte 不変) |
| 182 | RPE3 — `DrawQueueBuilder` | 済 | primitive snapshot / state sort / indirect materialize を Vulkan 非依存の純 CPU builder へ分離 | 見えない |
| 183 | RPE4 — owner-aware `RenderPolicyRegistry` + draw sort provider | 済 | draw sort 手法を engine builtin / game DLL で交換できる versioned provider 境界 | 公開 ABI `src/core/userpublic/render/draw_sort_abi_v1.hpp`([第6章](06_rendering.md) §6.2)|
| 184 | RPE5 — world bounds / phase 別 queue / 透明 sort / XR view policy | 済 | opaque=`state_batched_v1` と transparent=`back_to_front_v1` を別 queue 化、XR は `logical_view_center` / `per_view` | rendering config の `draw_sort`。透明物が奥→手前で描かれる |
| 185 | RPE6a — logical type kernel / typed shadow graph | 済 | `Image/Buffer/Stream/ObjectSet/Value` の意味型・`TypePattern`・conversion・logical port を純データ型として実装 | 新 dump schema `pelican.logical_render_graph` |
| 186 | RPE6b0 — canonical logical value graph | 済 | `(resource, version)` の producer 一意化、data edge 導出、`LogicalAccessIntent` | logical dump v2 |
| 187 | RPE6b1 — hybrid typed screen input | 済 | `opaque_color` / `opaque_depth` / `scene_depth` / `linear_view_depth` を typed contract 化 | `.surface` の名前付き `screen_inputs`。depth-fade / 屈折 material |
| 188 | RPE6c0 — target planning contracts | 済 | data-only な topology snapshot / backend probe / diagnostic ID / 3 profile | rendering config の `target_planning`([第6章](06_rendering.md) §6.6)|
| 189 | RPE6c1 — desktop/tile target planner | 済 | read footprint から desktop materialized / tile-local transient を決定的に lowering | 見えない(CPU fixture 段階)|
| 190 | RPE7/RPE8 — typed sample count + executable MSAA | 済 | `SampleCountPolicy` を実 device capability → Vulkan image/pipeline/resolve へ接続 | **MSAA が実際に効く**。`multisampling` / `pipeline.settings.msaa` |
| 191 | physical target planner runtime integration | 済 | data-only plan と runtime bridge を単一 lowering へ統合、runtime の JSON 再走査を削除 | G-buffer の名前・枚数を変えても planner 変更が不要 |
| 192 | RPE9 — builtin `GraphVariantPolicy` | 済 | flat/preview/XR の判定を immutable typed policy へ | 見えない |
| 193 | RPE10a — runtime publication root | 済 | compiled pass/graph/plan/route/sample/variant を一 generation として prepare → 単一 CAS publish | dump に `runtime_generation` |
| 194 | RPE10b1 — append-only GPU registration transaction | 済 | GPU registry 群を RAII transaction 化、5 段 fault point で部分登録を残さない | dump に arena generation / scope manifest |
| 195 | RPE10b2 — GPU owner-scope replacement / generation lease | 済 | 同 owner scope の再登録で同名 resource を新 handle へ置換、旧 Vulkan resource は lease 解放まで生存 | rendering config の hot reload が壊れなくなる |
| 196 | RPE10b3 — submission-fence lifetime + watcher publication | 済 | GPU submission lease を実 queue submit へ結び、root+preset+feature を一 transaction 化 | **rendering config / feature / preset の保存が実行中に atomic 反映** |
| 197 | Python / test-tool build isolation | 済 | `BUILD_TESTING` へ移行、`PELICAN_PYTHON_TESTS` と `PELICAN_WITH_SPIRV_LINK`(既定 OFF)を追加 | **Python なしでビルドできる** |
| 198 | host shaderc / target SPIR-V provider boundary | 済 | `PELICAN_RUNTIME_SHADER_COMPILER=ON` は Vulkan SDK shaderc を必須化、FetchContent fallback を廃止 | SDK 欠落時は案内付き configure error |
| 199 | Python opt-in development boundary | 済 | `PELICAN_PYTHON_TESTS` 既定を AUTO → OFF | 通常 configure が Python を探さない |
| 200 | RPE11a — fullscreen `PassImplementation` provider | 済 | 同じ logical pass contract を満たす **shader pair だけ**を差し替える公開 provider | pass の `implementation.provider`([第6章](06_rendering.md) §6.2)|
| 201 | RPE11b — tagged region / subgraph replacement | 済 | 1 パス → 最大 256 パスへの展開を、region 境界の logical type 一致を条件に許可 | pass の `regions` + パス列の `region_replacements` |
| 202a | RPE11c — global `GraphTransform` | 済 | 論理グラフ全体の構造変更(最大 32 段 chain)| トップレベル `graph_transforms` |
| 202b | RPE11d — renderer-wide `RenderStrategy` | 済 | preset / verbose authoring から renderer seed config **全体**を生成 | トップレベル `render_strategy` |

### WP203a〜214(XR multiview / physical plan / 描画機構 / 版の単一化、2026-07-25〜27)

| WP | タイトル | 状態 | 1行説明 | ユーザーから見える面 |
|---|---|---|---|---|
| 203a | XR2b-a — view execution target planning | 済 | `auto`/`sequential`/required `multiview`、resource の shared/sequential/layered layout、理由付き fallback | `xr.view_execution`([第6章](06_rendering.md) §6.12)|
| 203b | XR2b-b — array resource / `gl_ViewIndex` / one-execution | 済 | array image/view、per-view UBO、view-masked dynamic rendering、mixed-scope scheduler | 二回 `render()` を残さない契約が実装で満たされた |
| 203c | XR2b-c — OpenXR array swapchain / depth submit / GPU gate | **済(外部 gate 待ち)** | 2-layer 2D-array color swapchain + `XR_KHR_composition_layer_depth`、`xr.multiview_auto` の実測 profile gate | Meta XR Simulator / 物理 HMD / 対象 GPU の実測 gate が未消化 |
| 204 | physical plan eject / direct authoring | **済(外部 gate 待ち)** | plan pin、physical fragment v1〜v3、verified format/attachment、transient / tile-local / image alias、dependency-safe reorder | `vulkan_plan_pins` / `vulkan_physical_fragments`([第6章](06_rendering.md) §6.6)。一般 scope/queue と実機 GPU gate が未 |
| 205 | 公開 shadow contract + B 層 shadow 受光 | 済 | 公開 directional shadow resource / light relation + generated `pelican_shadow()` | feature の `surface_resources`。project へコピーした shadow feature が同値で動く |
| 206a | stable draw tag / filter | 済 | material 所有の安定 tag と pass 側 filter | pass の `material_filter`([第6章](06_rendering.md) §6.7)|
| 206b | pass-local material variant / multipass route | 済 | pass ローカルの named surface / render-state variant | inverted-hull outline が entity 複製なしに書ける |
| 207a | compute Frame/Light + sampled resource port | 済 | fullscreen/compute の named image port | pass / compute task の `resource_ports` |
| 207b | material/geometry typed frame-graph resource port | 済 | material vertex/fragment の typed readonly buffer / sampled image port | compute 結果を material から読める |
| 208 | lighting data contract v2 + clustered dogfood | 済 | scalable light inventory + cluster selection | 32 灯上限の解消。`engine://features/clustered_lighting.json` |
| 209a | static texture dimension + material sampler authoring | 済 | 2D/cube/2D-array/3D の KTX2 + sampler filter/address/compare/aniso | material から native cubemap を sampling できる |
| 209b | RT mip/layer/subresource view | 済 | 2D runtime RT の fixed/full mip、array layer、sampled/storage subresource | depth pyramid を layer 1 上で実 GPU dogfood |
| 210a〜g | indirect dispatch + GPU-written draw arguments | 済 | typed indirect dispatch → GPU-written indexed draw/count → depth-pyramid occlusion → segment table → hot reload → XR per-view → timing 判断入力 | GPU culling が project 側 compute shader だけで書ける |
| 211 | `dist-bake` + shaderc OFF feature delivery | 未 | precompiled variant manifest で runtime shader compiler なし配布 | 配布 / Quest 前に必須。`src/` に該当実装なし |
| 212 | VRS / foveation backend contract | 未 | Quest SA2 の device facts 待ち | 未着手 |
| 213 | 版の単一化 A — import manifest の version 必須化 | 済 | 4 形式の version 省略許容を廃止 | version 無し manifest は名前入り hard error |
| 214 | 版の単一化 B — physics service V1 の削除 | 済 | physics の service/API/provider 受理面を V2 だけに | V1 provider DLL は動かない |

### WP215〜238e(WSI epoch / material ABI / ViewFamily / 共通実行計画、2026-07-27〜31)

| WP | タイトル | 状態 | 1行説明 | ユーザーから見える面 |
|---|---|---|---|---|
| 215 | transactional window output root / frame token | 済 | `OutputCompileFacts` + candidate arena、`IFrameTarget` を move-only な frame token へ | resize / format 変更で部分状態が見えない |
| 216 | nonblocking `SwapchainEpoch` / XR mirror retirement | **済(外部 gate 待ち)** | swapchain 依存 object を immutable epoch へ、`device.waitIdle()` / `glfwWaitEvents()` を削除 | **最小化中も RPC/reload/ECS/audio が進む**。XR 実機 gate が未 |
| 217 | `SurfaceEpoch` recreation / present support revalidation | **済(外部 gate 待ち)** | fresh surface の再生成と present support 再検証 | **`VK_ERROR_SURFACE_LOST_KHR` から回復する**。live fault gate 済み、RDP / ディスプレイ切替の manual platform gate が未 |
| 218 | strategy-private arbitrary material outputs / typed MRT | 済 | 固定 5-MRT を廃し `pelican.material_outputs` v1(任意長 schema・engine 上限なし) | **G-buffer の枚数と型を作者が決められる**([第6章](06_rendering.md) §6.7)|
| 219 | material output attachment state | 済 | field 名キーの疎な `material_output_states`(blend / write mask) | output ごとに additive / R-only write |
| 220 | material same-pixel local-read ABI | 済 | `.surface` の screen input / image port を sampler **または** input attachment へ物理解決 | 公開アクセサ不変のまま tile GPU 上で subpass 相当の融合 |
| 221 | top-level render compiler program / open backend package | 済 | flat/preview/XR の compile 入口を一回の compiler program invocation へ抽出 | 描画挙動不変。**公開 game-DLL ABI は未凍結**(source-level のみ)|
| 222 | coordinated render graph / surface / material pipeline reload | 済 | 同じ watcher batch の render config + `.surface` + material values を一 transaction へ昇格 | **グラフとシェーダを同時に保存しても atomic に反映、失敗時は全て旧世代** |
| 223 | runtime ViewFamily foundation | 済 | stable family/view ID、Camera/OpenXR provider、family 別 temporal identity | `$main/$mono`、FrameUBO の stable family token |
| 224 | secondary ViewFamily relation/runtime | 済 | 論理〜Vulkan relation、family 別 FrameUBO、directional shadow provider | pass/compute task の `view_family` |
| 225 | cascaded secondary ViewFamily | 済 | CSM provider、array target、sequential schedule、cascade 別 culling | **3-cascade CSM が実 GPU で動く**(`cascade_count`)|
| 226 | planar reflection ViewFamily / 汎用 secondary culling | 済 | reflection provider、clip plane ABI、独立解像度 | `engine://features/planar_reflection.json` |
| 227 | planar reflection Forward opaque capture | 済 | Forward 再描画 + late binding 継承 + clustered compiler ABI | 反射に不透明 Forward 物が写る |
| 228 | secondary ViewFamily transparent capture / family-local sort | 済 | 同一 sort provider の view 別再評価、reflection-local snapshot | 反射に半透明物が正しい順で写る |
| 229 | ViewFamily-local clustered light selection | 済 | selection ABI v2、family token、XR 左右眼領域 | 反射内で多灯 clustered lighting が効く |
| 230 | planar reflection oblique near-plane projection | 済 | Vulkan ZO 一般式、透視/非対称/正射影/X 反転 | 水面下の物が反射に写り込まない |
| 231 / 231b | image extent dispatch / remaining-mip material view | 済 | typed output extent からの local-size 切り上げ除算、remaining mip descriptor | compute の dispatch 数を手書きしなくてよい |
| 232 | planar reflection mip filter / family-array material ABI | 済 | 7-level low-pass、pass-owned view lowering、scalar-family adapter | roughness に応じたぼけた反射 |
| 233 | replaceable render algorithm asset/package | 済 | typed shader asset parameter + 標準 package の project 差替え・build purge | **標準 prefilter シェーダを project 側で差し替えられる** |
| 234 | runtime ViewFamily provider package | 済 | 汎用 family registry、caller 優先解決、標準 planar policy の build purge | caller-authored family が常に優先される |
| 235 | raster attachment mip/layer view | 済 | typed attachment view + 論理〜Vulkan 物理計画 | 特定 mip / layer へ直接 raster 出力できる |
| 236 | runtime cube render target | 済 | resource/view 形状分離、cube-compatible allocation、face attachment、`samplerCube` | **runtime cubemap を描いて material から読める** |
| 237 | replaceable cube capture algorithm | 済 | stable 6-face provider、Deferred + Forward capture、package purge | `engine://features/cube_capture.json` |
| 238a | common `FrameExecutionPlan` vertical slice | 済 | backend 非依存の共通 IR。粗い endpoint class `{host, device, external}` と open な backend identity だけを持つ | `get_frame_plan` / `--dump-frame-plan` に `execution_plan` が併記される(観測面のみ。実行順の権威は FramePlan 側)|
| 238b | Generic Raster Pass ABI vertical slice | 済 | technique ごとに `PassInfo` variant を増やさず `type: "raster"` 一つ。contract は Vulkan 型 / shader module / binding 番号を持たない | pass 種別 `raster`(`draw` / `raster_state`)。sprite_demo で dogfood |
| 238c | complete physical plan / `NativeScope` data boundary | 済 | 完全 physical package の canonical round-trip + strict verifier。`NativeScope` 宣言は data-only | dump の `physical_target_plan.ejectable_complete_physical_plan`(sparse な physical fragment とは別 ABI)|
| 238d | `NativeScope` executor provider / runtime publication | **🚧 source slice のみ** | source-level executor registry、owner/generation lease、scope 単位 dispatch | `get_frame_plan.native_scope_executors`。**公開 game-DLL ABI は未追加**で、rendering config に書くキーも無い。同梱 builtin provider は `builtin.vulkan` 1 本で、その唯一の implementation が空マーカー `builtin.vulkan.noop_marker@1` |
| 238e | `NativeScope` command-producing Vulkan fixture | **🚧 source slice のみ** | default Vulkan package が保持する canonical logical graph / target topology / automatic plan / format capability / 実 extension closure から complete package を再検証・一括 install する helper。test provider が隔離 scope で実 Vulkan clear を記録 | 変化なし(呼び出し元は test のみ)。**公開 game-DLL raw command ABI と意図的な `VK_ERROR_DEVICE_LOST` 注入は未実装** |

> WP238a〜c のレポート日は 2026-07-30、WP238d/238e は 2026-07-31 ですが、**実際のコミット日は 5 本とも 2026-07-31** です。
> 上表の並びはコミット順に揃えています。

audit stop の系譜(正しい停止の運用実績): WP113(スカラー配送不能 → WP114 先行)/ WP120(morph 機構不在 → 3 分割)/ WP123(override 表現力不足 → WP122b 先行)。いずれも停止レポートが後続 WP の仕様の正になっています。

**要するに: WP1〜202b・WP205〜210g・WP213〜215・WP218〜238e は完了済みです
(WP113/120/123 は正しい停止後に分割・再派遣で着地)。active ledger に残るのは
外部 gate 待ちの 4 本 — WP203c(OpenXR array/depth を実装済み・Meta XR Simulator と
物理 HMD、対象 GPU の実測 gate 待ち)、WP204(physical plan eject。一般 scope/queue と
実機 GPU gate 待ち)、WP216 / WP217(実装済み・XR 実機と RDP / ディスプレイ切替の
manual platform gate 待ち)です。未着手は WP211 `dist-bake`(配布・Quest 前に必須)と
WP212 VRS/foveation(Quest SA2 の device 待ち)。予約 = WP22 pointcache /
WP32 IBL / WP36 GPU particle。**

WP 番号外の 2026-07-15 リファクタ群(design_reviews/ にレポートあり): module access hardening、PhysQuery 決定的クエリ基盤、purgeable physics provider boundary + provider DLL lifecycle。

今後の候補は [../roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md)が
正です。ただし **前回のマニュアル更新(2026-07-21)以降のコミットはほぼ全量が
WP180〜238e のレンダラ再構築に充てられた**ため、backlog の Tier 1(HR2-I = input/profile hot reload、U3 = UI hot
reload)と VRMA watcher 配線、唯一残った負債 CI2、anim_graph v2 / clip events は
いずれも着手されていません。XR2b は WP203a〜c のローカル実装まで済み、現実装の
Simulator/物理 HMD と対象 GPU 実測 gate が active です。

web 側(my_webpage)の WW 台帳は [第9章](09_web.md) §9.6 を参照してください。

## 11.2 設計文書マップ(docs/ ⇔ 本マニュアル)

矛盾時の優先順位: **凍結済み > ドラフト、設計文書 > 指示書**。
実装状態はレンダリング系が 2026-07-31、それ以外が 2026-07-19 の
コード・test・完了レポート基準です。

### 凍結・規範文書

| 文書 | 版 | 内容 | 対応する章 |
|---|---|---|---|
| [design_project_format.md](../design_project_format.md) [PF] | v6.3 | プロジェクト形式・PathResolver | [第3章](03_project_format.md) |
| [design_project_format_web_profile.md](../design_project_format_web_profile.md) [PFW] | v1.3 | web 解釈規則・サブセット原則 | [第9章](09_web.md) |
| [external_tools_requirements.md](../external_tools_requirements.md) | R1〜R10 | 外部ツール契約(glTF ハブ、transform_seq、JSON-RPC) | [第5章](05_assets.md)・[第10章](10_tools.md) |
| [design_shader_freedom_kit.md](../design_shader_freedom_kit.md) [SF] | — | シェーダ基盤(実装済み) | [第6章](06_rendering.md) |
| [design_headless_rendering.md](../design_headless_rendering.md) [HL] | — | ヘッドレス描画(実装済み) | [第2章](02_getting_started.md)・[第6章](06_rendering.md) |
| [animation_abi_v1.md](../animation_abi_v1.md) | **Frozen / normative** | アニメーション ABI v1(正本 header = `abi_v1.hpp`) | [第8章](08_gameplay.md) |

### ドラフト(実装済み or 実装中)

| 文書 | 実装状態 | 対応する章 |
|---|---|---|
| [design_scene_format.md](../design_scene_format.md) | ✅ WP25/63/79(`parent` は WP79 で追加) | [第4章](04_scene_ecs.md) |
| [design_ecs_lifecycle.md](../design_ecs_lifecycle.md) | ✅ WP62 | [第4章](04_scene_ecs.md) |
| [design_render_feature_modules.md](../design_render_feature_modules.md) | ✅ WP28〜31(feature = ユーザー空間ポリシーは §11.1 方針参照) | [第6章](06_rendering.md) |
| [design_compute_task_graph.md](../design_compute_task_graph.md) | ✅ WP33〜35 + WP64(実行一本化済み) | [第6章](06_rendering.md) |
| [design_material_shading.md](../design_material_shading.md) | ✅ M1〜M3.5(WP58/68/70/76/78/80/83) + stable material tag selection(WP206a) + pass-local named variant(WP206b) + material local-read(WP220)。spv-link は experimental | [第6章](06_rendering.md) |
| [design_color_pipeline.md](../design_color_pipeline.md) | ✅ v4 Accept・WP72〜74 で実装完了 | [第6章](06_rendering.md) |
| [design_postprocess_temporal.md](../design_postprocess_temporal.md) | ✅ T1/T2(WP88/95)。TAA は [design_taa_jitter.md](../design_taa_jitter.md) へ | [第6章](06_rendering.md) |
| [design_taa_jitter.md](../design_taa_jitter.md) | ✅ v2.1(条件付き受理)・J1/J1b/J1c/T-TAA すべて実装済み(WP112〜115) | [第6章](06_rendering.md) |
| [design_usd_openpbr.md](../design_usd_openpbr.md) | ✅ v2.1(条件付き受理)・M-PBR0a/0b + U-USD0a/0b/0c 実装済み(WP116〜119/124)。U-USD1 系は未 | [第5章](05_assets.md)・[第6章](06_rendering.md) |
| [design_openxr.md](../design_openxr.md) | ✅ v2.2・XR0〜XR4 + Meta XR Simulator gate実装済み(WP125〜138)。WP203a〜cでXR2b array/multiview/depth/profile gate、WP204でtile-local local readとのplanner/synthetic Vulkan接続をローカル実装済み。desktop mirror WSI lifecycleは**WP215〜217で実装済み**(XR実機・RDP/ディスプレイ切替のmanual platform gateは未消化)。現実装のSimulator/物理HMD・対象GPU実測、standaloneは未 | [第6章](06_rendering.md) §6.12・[第2章](02_getting_started.md) |
| [design_render_pipeline_extensibility.md](../design_render_pipeline_extensibility.md) [RPE] | ✅ RPE1〜RPE11d を実装済み(WP180〜202b)。draw sort / pass implementation / subgraph replacement / graph transform / render strategy の設計の正 | [第6章](06_rendering.md) §6.2 |
| [design_render_graph_compiler.md](../design_render_graph_compiler.md) [RGC] | ✅ 論理型カーネル・target planning・Vulkan physical plan・generic raster pass(WP185〜191/204/235〜238c)。**公開 ABI は未凍結** | [第6章](06_rendering.md) §6.2・§6.6 |
| [design_heterogeneous_execution_graph.md](../design_heterogeneous_execution_graph.md) [HEG] | 🚧 共通実行計画(WP238a)と generic raster dialect(WP238b)は実装済み。NativeScope(WP238c/d/e)は **source-level のみ**で公開 game-DLL ABI は未 | [第6章](06_rendering.md) §6.2・§6.6 |
| [design_wsi_epoch_recovery.md](../design_wsi_epoch_recovery.md) [WSI] | ✅ WP215/216/217 実装済み(ユーザー決定 2026-07-27)。RDP / ディスプレイ切替の manual platform gate は未消化 | [第6章](06_rendering.md)・[第2章](02_getting_started.md) |
| [render_mechanism_coverage.md](../render_mechanism_coverage.md) | ✅ 描画技法をコードへ再照合した棚卸し。残ギャップ(area/IES ライト、acceleration structure、bindless、VRS/foveation、instance 所有 tag、stencil 等)は同文書のギャップ表が正 | [第6章](06_rendering.md) |
| [render_authoring_ergonomics.md](../render_authoring_ergonomics.md) | 🚧 U2/U3/U5 の解消まで反映。**WP218 以降(material output ABI / ViewFamily / WP238 系)は未反映** | [第6章](06_rendering.md) |
| [design_debug_profiling.md](../design_debug_profiling.md) | ✅ D-P0〜D-P2 実装済み(WP139/140/143/145)。D-P3/D-P5/D-P4/D-P6/OPT は未 | [第10章](10_tools.md) |
| [shader_contract.md](../shader_contract.md) | ✅(FrameUBO/SSBO/resources manifest + material sampler/input-attachment ABIまで反映) | [第6章](06_rendering.md) |
| [design_input_actions.md](../design_input_actions.md) | ✅ I1〜I4 すべて(WP39/49/89/91) | [第7章](07_input_ui.md) |
| [design_ui_2d_foundation.md](../design_ui_2d_foundation.md) | ✅ v8 Accept・U0/U1/U2 実装済み(WP75/87/93) | [第7章](07_input_ui.md) |
| [design_2d_game_layer.md](../design_2d_game_layer.md) | ✅ v2.4・S2D-0a〜2 実装済み(WP103/104/106/107/109) | [第4章](04_scene_ecs.md)・[第6章](06_rendering.md)・[第8章](08_gameplay.md) |
| [design_event_payload_schema.md](../design_event_payload_schema.md) | ✅ v2 Accept・WP71 で実装 | [第8章](08_gameplay.md) |
| [design_animation_graph.md](../design_animation_graph.md) | ✅ v2.1・A0〜A2 + VRM application + VRMA-C0/R0/I0 実装済み(WP94〜102/111/121〜134/176〜178)。graph v2/live/SpringBone は未 | [第8章](08_gameplay.md) |
| [design_asset_hot_reload.md](../design_asset_hot_reload.md) | ✅ v2.1・HR0〜HR2-G、targeted animation generation(WP147)、VRMA reload entry(WP178)実装済み。HR2-I/U3/VRMA watcher配線は未 | [第10章](10_tools.md) |
| [design_text_hud.md](../design_text_hud.md) | ✅ WP54 | [第7章](07_input_ui.md) |
| [design_game_logic_native.md](../design_game_logic_native.md) | ✅ G1/G2 + behavior attachment/edit + two-generation reload(WP90/155/162/167) | [第8章](08_gameplay.md) |
| [design_editor_tooling.md](../design_editor_tooling.md) | ✅ v2.5(条件付き受理)。authoring/service/RPC/transaction/undo/save/snapshot/watch/preview、汎用 gizmo feature/RPC と Studio 操作を実装済み(WP149〜172/WP274/275)。WebSocket は未 | [第13章](13_editor.md)・[第10章](10_tools.md) |
| [design_object_behaviors.md](../design_object_behaviors.md) | ✅ v2.1・BEH0/BEH1/BEH2 実装済み(WP155/162/167)。`onFixedUpdate` や behavior 間の直接参照は未 | [第4章](04_scene_ecs.md)・[第8章](08_gameplay.md) |
| [design_event_layer.md](../design_event_layer.md) | ✅ v1.1 E1 + typed schema + E2 Enter/Exit(WP56/71/179)。Stay は未採用 | [第8章](08_gameplay.md) |
| [design_scene_flow.md](../design_scene_flow.md) | ✅ S1(S2 は未) | [第8章](08_gameplay.md) |
| [design_physics_queries.md](../design_physics_queries.md) | ✅ P1/P2 + shapeCast/Provider ABI V2 + trigger events(WP107/179)。rpc P3/mesh/BVH/rigid simulation は未 | [第8章](08_gameplay.md) |
| [design_camera_system.md](../design_camera_system.md) | ✅ C1/C2 + user-space 再編の追記 | [第8章](08_gameplay.md) |
| [design_determinism_services.md](../design_determinism_services.md) | ✅ WP53 | [第8章](08_gameplay.md) |
| [design_audio.md](../design_audio.md) | ✅ A1(A2/A3 は未) | [第8章](08_gameplay.md) |
| [design_persistence.md](../design_persistence.md) | ✅ WP55 + WP65(P1 実装済み) | [第3章](03_project_format.md)・[第8章](08_gameplay.md) |
| [design_asset_format_policy.md](../design_asset_format_policy.md) | ✅(KTX2 = WP92 済。状態列は古い) | [第5章](05_assets.md) |
| [design_asset_containers.md](../design_asset_containers.md) | ✅ K1〜K4(WP77/79/81/84) | [第5章](05_assets.md) |
| [design_project_dcc_houdini.md](../design_project_dcc_houdini.md) | ✅ import + import-tools(K3) | [第5章](05_assets.md) |
| [dcc_integration_qa_2026-06-12.md](../dcc_integration_qa_2026-06-12.md) | §6 = pelican.vat の正 ✅ | [第5章](05_assets.md) |
| [design_project_vcs.md](../design_project_vcs.md) | ✅ V1〜V3 + V2 manifest(WP66) | [第3章](03_project_format.md) |
| [design_project_interpretation_layer.md](../design_project_interpretation_layer.md) | ✅ WP44(現状記述は一部古い) | [第3章](03_project_format.md) |
| [design_build_tiers.md](../design_build_tiers.md) | ✅ B1/B2(B3/B4 未) | [第2章](02_getting_started.md)・[第10章](10_tools.md) |
| [design_devstudio_direction.md](../design_devstudio_direction.md) | 🚧 D1 shell / native viewport 済、project/outliner 統合と D2〜D3 は未 | [第10章](10_tools.md) |
| [design_roadmap_renderworld.md](../design_roadmap_renderworld.md) | 古い(一部失効) | — |
| [design_cloth_simulation.md](../design_cloth_simulation.md) | 古い(VAT レーンに実質置換) | [第5章](05_assets.md) |

### 指示書・運用・生成物

| 文書 | 内容 |
|---|---|
| [implementation_plan.md](../implementation_plan.md) | WP 詳細・受け入れ基準・共通規則(§0)。110KB 超 — 分割は backlog 登録済み |
| [roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md) | 今後の候補 47 件・6 ティア + D-P / SA トラック(セッション引き継ぎ用。ウェーブ表は 2026-07-16 夜時点) |
| [adding_features.md](../adding_features.md) | 機能追加レシピ集 |
| [agent_operations.md](../agent_operations.md) | マルチエージェント運用 |
| [rendering_phase1_review.md](../rendering_phase1_review.md) | Validation Run の流儀 |
| `design_reviews/` | codex の監査・敵対レビュー・WP レポート(一次資料) |
| `schemas/` | pelican.ui 系 JSON Schema 4 本(U0 成果物) |
| `color_migration_manifest.json` / `material_resources_manifest.json` | WP72 / WP76 系の生成物 |
| `source-code-guide/` | ソースコード読解ガイド(全 10 ファイル。[第12章](12_codemap.md) は入口・概観、詳細はこちら) |

### 方針決定(横断・重要)

> **設計決定(feature 層 = ユーザー空間・2026-07-12):** エンジンは**機構語彙**(anchor / history / snapshot / format_class / projection_jitter 枠など — 版付きで additive にのみ増える)だけを持ち、feature(JSON + シェーダ)は**ユーザー空間**。同梱 feature(hdr / shadow / bloom / debug 系)は**特権なしの標準ライブラリ**で、`engine://features/*.json` をプロジェクトへコピーして改造するのが公式ワークフロー。いじれない境界(canonical anchor の全順序・output_transform 等の常設ノード・色 invariant・決定性ゲート)は名前入りエラーで防衛される。カメラのコントローラ/ブレンド/シェイクも同様にユーザー空間へ再編済み。

> **設計決定(temporal のユーザー管理境界・2026-07-16):** [design_taa_jitter.md](../design_taa_jitter.md) §0-1 の境界表が規範。判定基準は**「5 年後に発展しているのはどちら側か」** — 品質・アルゴリズム(taa.json 構成・resolve/composite シェーダ・パラメータ・ジッタ系列)は全部ユーザー管理、データ供給と安全境界(ジッタ適用点・単一提供者排他・FrameUBO 供給 field)だけがエンジン語彙。語彙が足りなければ**追加の語彙 WP でエスカレーション**する(こっそり特権化しない)。`halton23` 等の名前付きジッタ系列も非特権の便宜にすぎない(同じ数表の直書きと全 byte 一致することが fixture で証明されている — WP115)。

> **設計決定(版の扱い・2026-07-26 ユーザー決定):** 全ての versioned 形式は版フィールドを持ち、**現行版ちょうど 1 つだけを受理する**。版が省略可能な形式は必須化する(WP213/214)。旧版を受理する分岐も、旧版を新版へ昇格する処理も足さない — 版を上げるときは旧版の受理を同時に削除する。ただし**版システム自体は将来のために維持する**(いま版フィールドを削ると、互換が実際に必要になった時点で全形式へ足し直しになる)。**例外**: ABI の版一致チェックは互換残しではなく**食い違い検出**(古いゲーム DLL を silent crash でなく明確なエラーにする)なので保持する。**runtime epoch は version ではない** — `SurfaceEpoch` / `SwapchainEpoch` / temporal reset epoch は process-local な寿命マーカーであり、保存形式の解釈には使わない。正本 = [implementation_plan.md](../implementation_plan.md) §0「版の扱い」。

> **設計決定(build 依存の境界・2026-07-24):** Python はエンジン/ゲームの通常 build 依存から外れた(`PELICAN_PYTHON_TESTS` 既定 OFF)。shaderc は Vulkan SDK の明示 provider であり、**cross target へ暗黙の source-build fallback を持ち込まない**。CI gate や fixture 生成ツールを無理に C++ へ移植しない、という開発境界も同時に固定された(WP197〜199)。

> **設計決定(描画 mechanism WP の共通方針・2026-07-26):** technique 名をエンジンの enum へ足さず typed な resource / view / execution / selection 語彙を足す / パーサと API だけで完了にせず project-owned な feature・material・shader の dogfood を 1 本通す / logical authoring は trust-first・optimize-by-default(矛盾は hard error、単なる曖昧 tie は deterministic + advisory)/ raw binding は C 層の escape hatch として残す / physical 指定は WP204 の fragment へ link し logical config へ Vulkan の field を漏らさない / feature 未参照時に追加 pass・resource・variant を持たない(WP206〜212)。

> **方向決定(Quest standalone = SA トラック・2026-07-17):** MVP は PCVR(Quest Link)のまま、standalone は段階計画として記録: SA0(NDK/clang spike)→ SA1(GLFW 非依存の platform 層)→ SA2(standalone session — XR1〜3 を再利用)→ SA3(性能: multiview + dist-bake 必須級)。今から守る door-keeper 4 件(clang CI smoke / Windows 専用コードのユニット境界 / B3/B4 前提化 / タイル GPU 設計規律)。

## 11.3 既知の食い違い(設計文書 vs コード)

2026-07-19 の再調査で更新(レンダリング系は 2026-07-31 に再確認)。**常にコードが正**。

### バグ探索の消化状況

- **2026-07-21 の読み取り専用バグ探索(正本: [`design_reviews/2026-07-21_readonly_bug_hunt.md`](../design_reviews/2026-07-21_readonly_bug_hunt.md))で confirmed とされた 25 件は、25 件すべて修正済み**です。先行既知の 2 件(sprite-1 / vk-1)も修正済みで、**未処理は判断保留だった bug-18(`gamelogicreload.cpp` の `pollAttempt`)1 件だけ**です。個票は同レポートが正本なので、本章には転記しません。
- **2026-07-26 のバグ探索第 2 波は別調査**です(id 体系が違う。正本: [`design_reviews/2026-07-26_bughunt_priority.md`](../design_reviews/2026-07-26_bughunt_priority.md)。54 主張 → CONFIRMED 39 / PARTIAL 11 / REFUTED 4)。この波の window 出力群は WP215〜217 の lifecycle として設計・実装され、ほかも 07-26〜27 の `fix(` コミット群で消化されています。
- ⚠ 第 2 波の **V1(`DEVICE_LOST` の検出分岐が死にコード)は部分解消にとどまります**。現在 typed な device-loss 分類が入っているのは WSI 経路(`swapchainrecovery` / `frametarget`)と NativeScope の capability bit だけで、**エンジン全体の device-loss 回復ポリシー(ドライバ TDR とエンジンのバグを区別する)はまだありません**。

### 挙動に関わるもの

1. **strict v1 化(WP63)が一部の設計文書に未反映**: scene のレガシー受理(「WARN+自動解釈」)、カメラ旧キーのエイリアス受理 — 文書にはあるが、実装はすべて名指しの hard error。
2. **hello-project(web サンプル)の camera が旧形式**: web で開けて pelican で開けない = サブセット原則違反の実例([第9章](09_web.md) §9.7)。
3. **`--play-seq` / `--seq-mesh` の相対パスが cwd 基準**([PF] はプロジェクトルート基準と規定)。`--play-vat` は設計どおり。
4. **project.json の `name` が必須になっていない**([PF] は必須)。欠落は黙って `user://` 無効化に落ちる。
5. **audio の `playMusic` が存在しない**(設計の API 表にはある)。全 voice は se バス固定。
6. **rpc `raycast` は未実装**(P3 スコープ)。※ただし WP107 で GameContext/PhysWorld には `shapeCast` all/closest が追加済み(rpc 面ではない)。
7. **`GameContext::debugText` は白・等倍固定**(設計の色・スケールは内部 API のみ)。
8. **OpenXR のセッション再生成はループ未配線**: [design_openxr.md](../design_openxr.md) §9 は swapchain/session 喪失時の再生成を規定するが、現行ループは teardown 要求で終了する(unwind・判定までは実装済み)。
9. **旧 sequential XR は Meta XR Simulator で検証済みだが、WP203c/WP204 の現実装と物理 HMD は未確認**: Simulator v201.0 で1000 frame/3分超/validation 0 は WP136/138 時点の旧 composition 経路。2-layer color/depth array、multiview、tile-local local read、Quest Link実機表示・入力、対象GPUの性能profile/帯域効果、standaloneは外部gate待ち。rpc は常に flat 駆動のため `get_status.xr.active=true` を rpc から観測する手段がない。

### キー名・形式の非対称(混同注意)

- **transform_seq は `rot`、scene / rpc は `rotation`**(意図的な別スキーマ判定)。
- **カメラの `yfov` はラジアン、ライトのコーン角は度、`--camera` の fov も度**。
- **schema/version エンベロープの有無が形式ごとに異なる**: scene / asset_data / input_actions / import / material / render_feature / frame_plan / ui / anim_graph / atlas / input_seq / settings にはあり、rendering config にはない。

### 文書の鮮度の問題

- `design_asset_format_policy.md` の状態列(VAT/EXR/音声/import/KTX2
  「未実装」)はすべて実装済み。

## 11.4 このマニュアルの更新ルール

本マニュアル(`docs/manual/`)は 2026-07-10 に全域調査から書き起こされ、
**2026-07-16 に WP64〜110**、**2026-07-17 に WP111〜137**、
**2026-07-19 に WP138〜179 の状態・event/physics/VRMA 差分**、
**2026-07-31 に WP180〜238e(レンダラ再構築。前回の宣言基準 `d13fc26` 以降 230 コミット
= merge を除く)**を反映しました。
維持のためのルール:

1. **コードが正。** 設計文書と食い違ったら、実装を確認してマニュアルを実装に合わせる(意図の説明として設計文書を引用するのは可)。
2. **実装状況バッジを守る。** ✅実装済み / 🚧実装中 / 📐設計のみ(未実装)。WP がマージされたら該当章のバッジと §11.1 の台帳を更新する。設計文書にしかない機能を「使える」と書かない。
3. **JSON 例は実物から。** `projects/example/`・`projects/sprite_demo/`・`projects/animgraph_demo/`・`test/fixtures/`・`src/core/resources/` の実ファイル、またはパーサ実装で検証した形だけを載せる。創作例には「(例)」と明記。
4. **章の追加**: `NN_slug.md` で採番し、[00_index.md](00_index.md) の目次と、関係する章の相互リンクを更新する。章の冒頭形式(タイトル → 対象行 → 「この章で学ぶこと」→ 末尾「関連文書」)を踏襲する。
5. **バッジ更新のタイミング**: 形式(スキーマ)に触れる WP・strict 化 WP・機能追加 WP のマージ時。直近では HR2-I・U3・VRMA watcher・CI2 の着地時、**XR2b(WP203c)/ WP204 / WP216 / WP217 は外部 gate を通過した時点**で関係章を書き換える。加えて WP211 `dist-bake` の着地時に第2章・第10章を、**WP238 系の公開 game-DLL ABI が凍結された時点**で第6章を書き換える(それまで NativeScope を「使える」と書かない)。
6. **一緒に直すもの(再発防止)**: golden ケースを足したら `test/golden/inventory.json` にも登録する(件数の自動発見はもう無い)。RPC メソッドを足したら [第10章](10_tools.md) の表と、編集系なら [第13章](13_editor.md) にも書く。CLI 引数を足したら第10章 §10.2 の表に足す。
7. **§11.3 の食い違い一覧は「解消したら消す」**。設計文書側が改訂されたか、実装が設計に追いついたら該当行を削除する。

## 関連文書

- [../implementation_plan.md](../implementation_plan.md) — WP 台帳の一次資料
- [../roadmap_backlog_2026-07.md](../roadmap_backlog_2026-07.md) — 今後の候補(47 件・6 ティア)
- [../README.md](../README.md) — 設計文書の索引
- [../agent_operations.md](../agent_operations.md) — 開発体制
- [第1章 エンジン全体像](01_overview.md) / [目次](00_index.md)
