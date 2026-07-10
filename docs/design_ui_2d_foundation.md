# 2D 描画基盤と UI システム(v2)

対象読者: エンジン担当・UI/2D を作る人。
ステータス: v2 ドラフト(2026-07-10。**v1 は codex レビューで Reject —
`docs/design_reviews/2026-07-10_ui_2d_review_codex.md` の C1〜C12 を全面受理して
改稿。再レビュー待ち**)。
前提: 2026-07-07 決定「UI と 2D ゲームは描画基盤共有・上物分離」、
2026-07-10 決定「エンジン標準スキン = ツール調 / エンジン UI = ImGui /
ゲーム UI = pelican.ui」、ECS v2.1(世代付き ID・生ポインタ禁止)、
イベント層 E1、asset containers(K 系)。

## 0. 三層と依存の訂正(C8)

| 層 | 内容 | 本書 |
|----|------|------|
| 2D パス(基盤) | クアッド ABI・run 分割・クリップ・座標変換 | §1〜2 |
| UI 層 | pelican.ui・レイアウト・入力・Controller・イベント | §3〜6 |
| 2D ゲーム層 | 別文書(予約) | — |

**依存の訂正**: v1 の「U1 依存なし」は誤り。アトラス(`#sprite/`)は **K3
(atlas_pack)に依存**する。PSD→UI は K3 + `pelican.layout` の supported
subset 定義に依存。ImGui はエンジン UI として別ユニット(§8)。

## 1. クアッド描画 ABI(C1・C2)

### 1-1. データ形式(CPU/GPU 契約)

```
QuadVertex(24B, align 4):
  float2 position   // framebuffer px(ViewportTransform 適用後)
  float2 uv         // 正規化。solid は白 1texel ページの UV 固定値
  u8x4   color      // RGBA8_UNORM straight alpha
QuadCommand(CPU 中間・POD):
  layer:u16, decl_seq:u32       // ソートキー(この 2 つで全順序が決まる)
  texture_page:u16              // アトラスページ。0 = 白 1texel 予約ページ
  sampler_key:u8                // nearest | linear
  kind:u8                       // solid | sprite | glyph(sdf は将来値を予約)
  clip_id:u16                   // クリップ表への添字
  rect, uv_rect, color
DrawRun:
  pipeline, texture_page, sampler_key, scissor, first_index, index_count
```

- 上限: 1 フレームのクアッド数・クリップ数に固定上限(config 宣言、既定
  16384/256)。**超過は名前入きエラー**(黙って欠けない)
- 「単色 = 白 texel ページ」方式を採用(kind 分岐シェーダより run 併合が単純)

### 1-2. run 分割(順序の絶対規則)

- ソートは `(layer, decl_seq)` の **stable な全順序**。生成自体を document
  走査順で行い、**map 走査順に依存しない**(現行 UI の unordered_map 描画順は
  移行時に廃止)
- **併合は「隣接し、かつ texture_page・sampler_key・clip が同じ run」のみ**。
  A(tex0) B(tex1) C(tex0) を A+C にまとめることは決してしない
  (ペインターズ順の保存が正しさ — draw call 数はその次)
- v1 のテクスチャ戦略 = **1 run 1 ページ**。descriptor array / bindless は
  将来の native 高速路(§3-5 の classic/bindless 併用と同じ構図)として予約
- クリップ: 親子矩形の**整数 px 交差**をスタックで計算し `clip_id` に割当。
  同一 clip の連続 run を scissor に束ねる。クリップ変更は順序を越えない。
  空クリップのプリミティブはドロップ

### 1-3. alpha・色空間・sampler(既存 2 実装の不一致の解消)

- **straight alpha / blend = (src_alpha, 1-src_alpha)** に統一。sRGB テクスチャは
  sRGB view でサンプリングし linear で合成。出力 alpha の扱いを shader ABI に固定
- **sampler は draw key の一部**(text/pixel-art = nearest + pixel snap、
  画像 = linear)。現行 UI(linear・dst alpha 保持)と debug_text(nearest・
  src alpha)の不一致は、この key 化で共存させてから統合する
- debug_text の統合は「グリフレイアウト(CPU)とフォントアトラスの再利用 +
  QuadCommand への変換」— 48B SSBO 頂点形式は**引き継がない**。
  **統合前後で debug_text の exact golden(tolerance 0)を維持**すること

## 2. 入力・座標・capture(C3・C4)

### 2-1. FrameInput(入力層の前提工事)

現行 L1 スナップショットはフレーム内のイベント順序を捨てるため
「UI が最初に消費」は実装不能(レビュー実証)。**InputState を改訂**する:

```cpp
struct FrameInput {
    std::span<const InputEvent> ordered_events; // event_seq 付き・フレーム中 immutable
    InputSnapshot snapshot;                     // Actions / held 用(従来)
};
```

- L0(GLFW / rpc)は既に順序付きキューを持つ — **キューを消さず公開**する形
- UI ルーティングの位置 = `dispatchPendingEvents の後・ECS update の前`
  (前フレームの semantic イベントは従来どおり頭で配送され、UI の消費マスクが
  Actions 評価前に確定する)。rpc ループも同順に揃える
- **UI 消費マスク**: UI がヒット/capture 中の pointer 入力を Actions 評価から
  隠す外部マスク API を Actions に追加
- 再現単位 = **ordered events + UI document revision + ViewportTransform**。
  `pelican.input_seq` v1 は ordered pointer events を記録する形に改訂
  (I 系設計への波及として記録)

### 2-2. ViewportTransform と DPI(v1 の未決 1 を確定)

- `ViewportTransform { window_points ↔ framebuffer_px ↔ virtual_ui_units }` を
  フレームごとに 1 個作り、**レイアウト・描画・hit・rpc inject・golden が
  全て同じ変換を使う**(GLFW の cursor=screen coords / framebuffer=px の
  不一致を一点で吸収)
- 仮想解像度を config で宣言(既定 = framebuffer 1:1)。スケールは
  integer / free を選択、letterbox の余白は入力対象外(規則明記)

### 2-3. capture 状態機械

- capture の保持は **世代付き `WidgetId{index,generation}`**(Controller
  ポインタ禁止 — ECS と同じ失効モデル)
- press: 最前面の enabled/visible/hit-testable 1 widget が pointer id/button
  ごとに capture を取得
- capture 中の move/up は**矩形外でも owner へ**。奪取時は旧 owner に
  `pointer_cancel` を先に送る
- owner の hide/disable/remove・document hot reload・scene unload・focus loss で
  `pointer_cancel` + pressed/hover 解除 + capture 解放。release 時に別 widget へ
  click を誤送しない
- **click の定義** = 同一 owner で press、drag 閾値未満、release 規則充足。
  drag delta は**イベント間の virtual units**(フレーム合計を使わない)
- tree の変更要求はイベント dispatch 後に command queue で commit
  (コールバック中の直接変異禁止 — ECS の mutation guard と同思想)

## 3. 2 レーンのイベント意味論(C5 — v1 の矛盾の解消)

| レーン | タイミング | 変更してよいもの |
|--------|-----------|-----------------|
| **UI-local** | 同フレーム | hover/pressed/capture、スライダーのつまみ、ドラッグプレビュー、Controller の自 subtree/値(UiCommandBuffer 経由) |
| **semantic** | 次フレーム(イベントバス E1) | `MenuOpened` 等のゲームへの通知 |

- **`emitImmediate` は設けない**(E1 が排除したシステム順依存を復活させない)
- ボタンの押し込み表示は同フレームに出る(UI-local)、ゲームの反応は
  次フレーム(semantic)— 見た目の即応と決定性が両立
- 連続 drag をゲームが同 tick で要る場合は、イベントでなく **frame-scoped
  `UiActionFrame` query**(Actions と同型)を将来オプションとして予約
- `"emit"` の **payload スキーマは UI ロード時に検証**(未知イベント名・
  型不一致はロード時の名前入きエラー — click 時まで遅延させない)。
  drag delta / widget id / 静的値の写像をスキーマで定義

## 4. pelican.ui v1 — レイアウトの最小閉包(C7)

v1 の「アンカーのみ」を撤回し、以下を v1 に含める(これ未満では stack に
置いた button の矩形すら定まらない):

1. **size mode(軸ごと)**: `fixed | content | fill`。stack 子には `weight`
   (1 個だけの追加自由度)
2. **intrinsic size** の算出規則: label = グリフメトリクス、image = sprite
   実寸、button = label + padding、panel = 9patch マージン + content
3. `min_size` / `max_size`
4. stack: `direction / gap / padding / align / justify`。**同じ軸を anchor と
   stack の両方が支配しない**(親が stack なら子の anchor は無効 — エラー)
5. アンカーは 9 方位の点でなく **`anchor_min/anchor_max + offsets`**
   (edge inset による stretch 表現)
6. `overflow: visible | clip`、`visible / hidden(空間残す)/ collapsed(残さない)`
7. **pixel snap**: レイアウトは整数 px、余り px は宣言順に 1px ずつ配分。
   毎フレーム root から再計算(float 累積を持ち越さない)
8. v1 は **LTR 固定(明示エラー)**。RTL/safe area は v2 予約

スタイル(トークン・状態オーバーレイ・ツール調標準スキン)と語彙 6 種
(panel/image/label/button/gauge/stack)、拡張 3 段
(合成 / PELICAN_REGISTER_WIDGET / ツールパック — ただしツールパックは
ImGui 採用により縮小し「出荷するゲーム内ツール」需要時に再評価)は v1 から維持。

## 5. Widget/Controller の所有権(C6 — 生ポインタ穴を開け直さない)

```
WidgetTypeRegistry(static): type 名 → metadata + factory のみ。live オブジェクトなし
UiModule
  └─ UiDocumentInstance(document revision 保持)
      ├─ WidgetArena: WidgetId{index,gen} → node
      ├─ ControllerArena: node と同 lifetime(親→子で構築、子→親で破棄)
      ├─ capture/focus/hover: WidgetId のみ保持
      └─ layout/render snapshot
```

- `PELICAN_REGISTER_WIDGET(T, "drag_number", schema_version)` は factory 登録
  だけ。重複名 hard error
- Controller API は制限面: `onPointer(const UiPointerEvent&, UiControllerContext&)`。
  ctx は WidgetId resolve / UiCommandBuffer / semantic sink を提供。
  **node・component への参照保存は禁止**。ECS 参照は `GameObjectId` 値 +
  使用時 resolve
- subtree 変更は handle 指定 command として dispatch 後 commit。消滅 handle は
  stale no-op + status(ECS の stale 表と同じ流儀)
- **hot reload はトランザクション**: 新 document を side-build/validate →
  フレーム境界で atomic swap。失敗時は旧 document 維持。swap 前に旧 capture へ
  cancel、旧 Controller を逆順 deinit。状態継承は
  `(document key, stable id, type, schema_version)` の明示 saveState/restoreState のみ
- ツリーの所有者は **UiModule**。ゲーム component は `UiDocumentHandle` を
  値で持つだけ(直接所有しない)

## 6. 描画パスと purge(C9 — 事実誤認の訂正)

- v1 の「post_ldr 以後の ui アンカー」は存在しない(feature 挿入語彙は
  before/after/end のみ、現行 example は ui_pass 直書き)。v2 では
  **予約パス名の列を規範化**: `… → pelican_ui → debug_text → imgui → present`。
  feature 配列順への暗黙依存を排し、この順をプランの fixture で固定
- ui は `engine://features/ui.json` の **purgeable feature** に移行(現行の
  「shader 無しの特殊 pass 型」を置換)。**不参照時に UiModule・GPU 資源・
  パーサが立ち上がらないこと**をテストで保証
- 旧 `ui_overlay.json` の移行は atomic(strict v1 の流儀 — 変換は外部、
  ランタイムは新形式のみ)。パース失敗時は名前入りエラー、hot reload 失敗時は
  旧版維持、GPU in-flight 資源は遅延破棄(C12)

## 7. 決定性と検証(C11)

非決定性源への対策(規範):

| 源 | 対策 |
|----|------|
| コレクション走査順 | render command は document 走査の vector 順。map は lookup 専用 |
| 同値ソート | `(layer, decl_seq)` stable sort |
| float 累積 | 毎フレーム root から整数レイアウト再計算 |
| DPI/解像度 | golden は viewport/scale/letterbox を fixture に固定・hit と描画は同一変換 |
| テキスト | v1 は同梱グリフ表の整数 advance が正(OS フォント不使用) |
| アトラス | import-time が正(K3)。bleed padding を import 規約化 |
| sampler/alpha | draw key + shader ABI + fixture に固定 |
| reload/クロック | golden 中は禁止。UI アニメは EngineTime fixed step のみ |

**semantic fixture**(pixel golden の手前の防衛線):

- widget ごとの最終 rect / clip / layer / decl_seq
- 順序付き draw-run 列(pipeline/sampler/texture/clip/first/count)
- 順序付き入力ルーティング列(event_seq → 対象 WidgetId → consumed/cancel/click/drag)
- hot reload 前後の capture cancel と Controller init/deinit 列

## 8. ImGui(エンジン UI)の隔離規約(C10)

- **build**: 専用 CMake target。`PELICAN_WITH_IMGUI=OFF` でソース・シンボル・
  フォント資産を一切含めない
- **runtime**: 無効時は context/backend/pass/入力 hook を生成しない
  (draw を捨てるだけの「見えないが動く」状態を作らない)
- **input**: 優先順 `raw → ImGui(WantCapture)→ pelican.ui → gameplay` を
  明文化。**Window が GLFW callback の唯一の owner** で、ordered event を
  ImGui adapter と InputState へ一度ずつ渡す(backend の callback install と
  自前 forward の二重配信禁止)
- **golden/replay/headless**: ツールコールバック自体を実行しない。
  「frame plan に imgui pass が無い・入力消費が不変・公開 API 呼出 0 回」を
  テストで固定
- ツールコードは公開 API(GameContext / rpc 意味論)のみ使用(D0)。
  ただし「特権なし ≠ 副作用なし」— 本当の隔離は非実行で担保
- multi-viewport / docking は v1 OFF 固定。clipboard/IME/OS cursor の
  対応可否を導入 WP で明示

## 9. 実装順(v2 — U0 起点。レビュー §10 を採用)

| 段階 | 内容 | ゲート |
|------|------|--------|
| **U0** | **純 CPU**: スキーマ・レイアウト・WidgetId arena・ordered 入力ルーティング・capture・draw command 生成 | semantic fixture 全通過(GPU なし) |
| U1 | K3 アトラス接続・quad buffer・stable run・clip・ui feature 化・panel/image | 2 アトラス交互重なり・nested clip・旧 UI 移行・golden |
| U2 | bitmap label/button・UI-local 状態・E1 emit・rpc の ordered click/drag/replay | **debug_text exact golden 維持**・同フレーム複数 click |
| U3 | Controller factory/lifecycle・hot reload トランザクション・gauge/stack/トークン | remove/hide/reload 中の capture cancel・init/deinit 列 |
| U4 | ImGui ユニット(§8) | OFF/headless/golden/replay の完全不在・入力優先順位 |
| U5 | PSD subset converter | K3 後。未対応表現の明示エラー |

前提工事(U0 より前 or 同時): **FrameInput(InputState 改訂)**と
input_seq v1 の改訂 — 入力設計(I 系)側の WP として切り出す。

## 10. 未決事項(v2 で残る最小)

1. SDF/日本語テキスト(text_hud v2 と同時 — 本書 v1 は ASCII bitmap 固定)
2. scroll / grid / wrap / RTL / safe area(v2)
3. bindless テクスチャページ(native 高速路 — マテリアルの classic/bindless と同時期)
4. 2D ゲーム層とのバッチャ共有形態(別文書で)
