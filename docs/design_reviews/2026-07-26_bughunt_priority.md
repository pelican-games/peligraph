# バグハント確定リスト: 修正推奨順(検証済み)

日付: 2026-07-26
入力: 監査 6 体([findings_raw](2026-07-26_bughunt_findings_raw.md)、
[vulkan_sync](2026-07-26_bughunt_vulkan_sync.md))+ 敵対的検証
([verification](2026-07-26_bughunt_verification.md))
判定: **54 主張 → CONFIRMED 39 / PARTIAL 11 / REFUTED 4**

本書は**確定した実バグだけ**を「実害 ÷ 修正コスト」で並べたもの。無害な死にコードは
§4 に分離した。類型スイープ 2 本(ecs/abi、io/tools)は実行中で、結果は追補する。

## 0. 発見の構造(なぜ見つかったか)

すべて**同じ根に行き着く**:

> **C++ ラッパが下位 API の誤差契約を書き換えている場所に、欠陥が集中する。**

明示的な分類を強制する API は全部クリーンだった — GLFW(生 `VkResult`)・
VMA-Hpp(必ず throw)・SPIRV-Reflect・tinyexr・**自作 KTX2 パーサ**・
Win32 watcher・SPIRV-Tools・OpenXR(`XR_FAILED()` を正しく使用)。

壊れていたのは vulkan.hpp(例外化)・nlohmann(`value()`/`empty()` の非直感)・
tinygltf(`true` = 続行)・miniaudio(降格して success)・imgui(hook 未設定)。
**自前で書いた所は堅く、便利なラッパを使った所が壊れている**という逆説。

## 1. 最優先(実害あり・確定)

| # | ID | 内容 | 規模 | 根拠 |
|---|---|---|---|---|
| 1 | **C-1 / VKSYNC-03** | **DeletionQueue が GPU 完了前に解放**。update フェーズ(テクスチャ hot reload)で defer した資源が 1 epoch 早く破棄され、GPU use-after-free / device lost。**headless では構造的に再現しない**(offscreen が CPU/GPU 直列化)ので golden/CI で永久に検出できない | 中 | `deletionqueue.cpp:74-82` / `renderer.cpp:2478-2492` / `framephase.cpp:119-124` / `texturereloadhandler.cpp:213-230`。**独立 2 体が別角度で発見** |
| 2 | **VKSYNC-01** | **present 待ち binary semaphore を in-flight frame 単位で再利用**。submit fence は present がセマフォを消費した保証にならない。正しくは swapchain image 数だけ持ち `current_image_index` で選ぶ | 中 | `swapchainframetarget.cpp:211-215, 421-457`。Khronos 公式 guide が同型の誤りとして明示 |
| 3 | **B-1** | **DLL reload の teardown 失敗が握り潰され、成功として `FreeLibrary`**。生存 entity/callback が未マップ DLL を指す | 中〜大 | `teardown.cpp:17-30`(`catch(...)`)/ `gamelogicreload.cpp:246-290` |
| 4 | **VKSYNC-02** | acquire 後の例外で fence が未 signaled のまま残り、**次の描画が永久待機** | 中 | `swapchainframetarget.cpp:244-294` / `renderer.cpp:2903-3016` |
| 5 | **VKSYNC-04/05** | 読み取りを伴わない WAW に memory dependency が生成されない / depth barrier の stage scope が EARLY・LATE 片方ずつ欠落 | 中 | codex 同期監査 §VKSYNC-04, -05 |
| 6 | **V1** | **`DEVICE_LOST` が全リポジトリで 0 ヒット**。検出分岐 8 箇所は死にコード。TDR/ドライバリセットでエンジンのバグと区別できない終了 | 大(設計) | `waitForFences` 許可は `{eSuccess, eTimeout}`(3 SDK 共通)。**私が裏取り済み** |
| 7 | **D-2** | glTF の `normalized` を主頂点経路だけ読んでいない → 正規化 integer の TEXCOORD/COLOR/WEIGHTS が 65535 倍。**兄弟 2 経路は明示的に拒否している** | 中 | `gltf.cpp:594-771, 1614-1632` vs `gltf.cpp:773-784` / `vrmadecoder.cpp:319-329` |
| 8 | **D-12** | miniaudio の voice が**一度も解放されない**(`erase` が 0 件)。長時間・高頻度 SE で無制限増加 | 中 | `audio.cpp:294-320, 331-395, 435-467` |
| 9 | **D-3** | 16-bit PNG を含む glTF を RGBA8 として誤解釈(`bits`/`pixel_type` をどこも読まない)。サイズ照合が無く validation も出ない | 中 | `gltfimage.cpp:22-41` / `gltf.cpp:1366-1375` / `util.cpp:82-114` |

## 2. 小さくて即効(1 行〜小)

| ID | 内容 | 根拠 |
|---|---|---|
| **D-11** | quill の `FileSink` 例外が `main` の保護外 → 書込み不能 CWD で**起動時 terminate**、ログも出ない | `log.cpp:16-28` / `pelican_core.cpp:32-41` / `main.cpp:434-444` |
| **D-1** | `jsonrpc.cpp:173` の `value()` が型不一致で throw。`{"jsonrpc": 2.0}` の 1 行で終了。**queue overflow 時は reader thread 直呼びで `std::terminate`** | `jsonrpc.cpp:148-175` / `windowedrpchost.cpp:39-68` |
| **D-4** | imgui の `CheckVkResultFn` 未設定 → backend 内 46 箇所の VkResult が no-op、未初期化 descriptor を update に渡しうる | `imguisystem.cpp:313-329` |
| **V2** | 再生成後も `current_image_index`/`has_rendered_frame` が古く、capture rpc が誤画像・範囲外読み | `swapchainframetarget.cpp:159-165, 198-209, 497-515` |
| **B-3** | `found->empty()` が string で常に false(唯一の書き間違い箇所)。`get_ref<const std::string&>().empty()` へ | `editorpreviewprojection.cpp:83-91` |
| **B-7** | swapchain 禁止 guard が、それが守る lookup の後ろ。並べ替え | `renderingpassvalidation.cpp:209-216` |
| **B-ORDER / D-13** | `platformLoadError()` を `releaseGameLogicRegistrations` の後に読む。1 行移動 | `gamelogicreload.cpp:132-139` |
| **D-10** | `Texture::source` の -1 を無検査で `images.at()`。**他 7 箇所の -1 ガードはある** | `gltf.cpp:1366-1374` |
| **E-1** | BC5 atlas の正確な診断が先行 guard に隠れ「size 不一致」と誤表示 | `atlasassetresource.cpp:67-80, 127-149` |
| **E-2** | `noexcept` 内の lock/push_back 例外で terminate(graceful path が到達不能) | `renderpolicyregistry.cpp:387-408, 532-542` |
| **B-6** | int64 範囲検査を `UINT64_MAX` がすり抜ける | `materialformat.cpp:365-373` / `surfaceformat.cpp:319-327` |
| **C-2** | detached reader が caller 所有 stream を参照(テストで UAF) | `windowedrpchost.cpp:15-28, 88-103` |
| **D-7/D-8/D-9** | tinygltf の `true` = 「続行した」を成功として扱い、欠落画像・壊れた animation channel・`asset.version` 欠落の診断が失われる | `gltf.cpp:211-223` / `vrmadecoder.cpp:571-577` |
| **D-6** | `ma_sound_start` の失敗分岐が実質死亡。再生の成否を検出できていない | `audio.cpp:338-369` |
| **B-9** | version bit と tile 属性が矛盾する EXR を scanline として復号 | `imageloader.cpp:173-194` |

## 3. 実害あり・修正が大きい(要設計判断)

| ID | 内容 |
|---|---|
| **A-F5** | `SURFACE_LOST` 回復不能(サーフェス再生成 API が存在しない) |
| **A-F11** | 最小化がレンダリングでなく**ループ全体**を止める(rpc・reload・ECS・音声も停止) |
| **A-F12** | XR mirror が「device idle を待たない」という自分の規約に反し、次 HMD フレームを stutter させる |
| **A-F6** | swapchain format 変更に RT/pipeline が追従しない |
| **A-F2/F3/F4/F9** | ゼロ extent TOCTOU・部分失敗のロールバック欠如。**現状の実害は「その操作で落ちる」**(混在状態で描画継続ではない)。局所回復を入れるなら 4 件とも先に transaction 化が必要 |
| **A-F7** | 恒常 SUBOPTIMAL で毎フレーム `waitIdle` + 再構築(停止条件なし) |

## 4. 無害な死にコード(掃除対象・実害なし)

`B-4`(素な集合の交差検査)/ `B-T2-PIPE`(pipeline result 比較)/
`B-T2-FENCE`(fence 比較 5 箇所)/ `B-T3-SCENE`(**警告レーン全体が死亡** —
アクセサ・3 代入・swap がドキュメント公開のたびにコピー)/
`B-T3-SAVE`(**保存のたびに JSON 全体を再パース**)/ `B-T3-SHADER` /
`B-T3-CANON`(同じ非正準検査が 4 箇所)/ `E-3`〜`E-7` / Tier 3 の約 45 件。

**削除の安全網**: golden byte 一致 + trace 一致 + 全テスト。§0 の
「挙動変更とリファクタの混在禁止」に従い独立コミットで。

## 5. 棄却された主張(4 件 — エージェントの誤り)

| ID | なぜ誤りか |
|---|---|
| **B-2** | SymbolId は**型未確定の遅延制約**として全 kind に通す設計。dead case は重複だが「型検査 bypass」は逆(テストが遅延 bind を要求) |
| **B-5** | prefer/automatic の共通 sample count 降格は**理由付きの仕様**。exact は正しく失敗する。死んでいるのは防御 throw だけ |
| **C-3** | 現行の構築順(`pelican_core.cpp:64` → `:83`)+ LIFO 破棄で watcher join が先。**設計上の脆さ**であって UAF 経路ではない |
| **D-16** | GLFW pointer の lifetime 違反は形式上のみ。現行 2 caller は GLFW 生存中に即消費/コピー |

## 6. 恒久的な検出手段(再発防止)

今回のバグ類型は**静的レビューでは原理的に見つからない**(コンパイルが通り、読んで
正しく、レビューも通る)。恒久策の優先順:

1. **未実行 recovery 経路レポート** — catch/エラー分岐/fallback のうち全テストを
   通して一度も実行されないものの一覧。今回の死にコード群は全部ここに出る
2. **ラッパ契約の突き合わせ監査**(今回の手法)を CI 化 — ライブラリの許可結果と
   コード側の比較を機械照合
3. **カオススモーク**(実ウィンドウ) — リサイズ/最大化/最小化/ディスプレイ切替。
   種バグはここで即死する
4. **敵対的な外部 API レイヤ** — Vulkan が OUT_OF_DATE / DEVICE_LOST / OOM を返す
   状況を強制する fault injection(既存の fault point 規律を外向きに拡張)

**重要**: C-1(DeletionQueue)は **headless では構造的に再現しない**。GPU gate
(CI2)無しでは永久に検出できない類のバグである。
