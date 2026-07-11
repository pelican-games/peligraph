# 2D 描画基盤と UI システム(v7)

対象読者: エンジン担当・UI/2D を作る人。
ステータス: v7 ドラフト(2026-07-11。v6 は round 3 レビューで **Reject**
(`docs/design_reviews/2026-07-11_color_ui_v6_rereview_codex.md` §2)。
v7 の主変更: ①decimal parse と FP environment の実装契約(正本 parser・
/fp:strict 相当・FE_TONEAREST 検査)+ px 変換後 overflow 規則
②semantic invariant の再定義(clip の計算可能化 = widget record に
overflow を追加・run 0 と総量の拘束・scissor containment・入力/capture
状態機械・参照整合)+ **schema-valid/semantic-invalid fixture** の追加
③coverage manifest の主張を実データと一致させる(小 enum = 全値 /
key・pad = 語彙 branch 代表 + enum 同期テスト)。
色正本は `design_color_pipeline.md` v4(パス列も v4 §2-2 の一意全順序)。
payload 正本 = `design_event_payload_schema.md` v2(**条件付き Accept 済み**
— E-C1〜E-C6 は WP71 に添付)。
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
QuadVertex(stride = 20B, align 4 — R1 で 24B 誤記を訂正):
  offset 0  : float2 position   // framebuffer px(§2-2 の変換・丸めの後)
  offset 8  : float2 uv         // 正規化。solid は白 1texel ページの UV 固定値
  offset 16 : u8x4   color      // RGBA8_UNORM・linear・straight alpha
  // sizeof==20 / alignof==4 / 各 offsetof を static_assert し、
  // shader reflection fixture(頂点属性の location/format/offset)をゲートにする

index: uint16(quad あたり 6 index、頂点は 4/quad)。
  既定上限 16384 quad = 65536 頂点で uint16 の境界に収まることを static_assert。
  DrawRun の first_index:u32 / index_count:u32

QuadCommand(CPU 中間・POD):
  layer:u16, decl_seq:u32       // ソートキー(この 2 つで全順序)
  texture_page:u16              // 0 = 白 1texel 予約ページ
  sampler_key:u8                // nearest | linear
  kind:u8                       // solid | sprite | glyph(sdf は値予約)
  clip_id:u16                   // 正規化済みクリップ表への添字
  rect, uv_rect, color

DrawRun:
  pipeline_key(= kind 系), texture_page, sampler_key, scissor,
  first_index:u32, index_count:u32
  // 併合キーは (pipeline_key, texture_page, sampler_key, clip_id) の完全一致
  //(R1: pipeline を併合条件に明記 — 異 kind を同 run に入れない)
```

- **blend(値まで固定)**: color = `(SRC_ALPHA, ONE_MINUS_SRC_ALPHA, ADD)`、
  alpha = `(ONE, ONE_MINUS_SRC_ALPHA, ADD)`
- **色の ABI は `design_color_pipeline.md` が正(B1 — v4 で分離)**:
  UI シェーダは **linear のみを出力**(OETF を書かない)。attachment
  フォーマット・swapchain 選択(`*_SRGB` 優先 + UNORM フォールバックの
  encode 専用最終パス)・authored sRGB → linear の変換式と丸め・blend が
  linear 空間で行われることは全て色パイプライン設計に従う。
  **U1 は色パイプライン C1(移行 + golden 再基準化)の後**(§9)。
  R6 の byte-exact 互換ゲートは **C1b** 後の新 baseline 上で運用する
  (色 v4 は C1 を C1a 構造 / C1b 色に分割 — 色依存はすべて C1b 完了が基準)
- 上限: 1 フレームのクアッド数(既定 16384)と**正規化後の一意クリップ数**
  (既定 256 — push 回数ではない)。超過エラーには document key・
  widget stable id・実数・上限を含める(R1)
- 「単色 = 白 texel ページ」方式を採用(kind 分岐シェーダより run 併合が単純)

### 1-2. run 分割(順序の絶対規則)

- ソートは `(layer, decl_seq)` の **stable な全順序**。生成自体を document
  走査順で行い、**map 走査順に依存しない**(現行 UI の unordered_map 描画順は
  移行時に廃止)
- **併合は「隣接し、かつ pipeline_key・texture_page・sampler_key・clip が
  全て同じ run」のみ**(§1-1 の完全キーと同一 — 再掲)。
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
- **フレーム位相契約(R2 — 順序の文章でなく queue 分離まで規範化)**:

  ```
  1. フレーム開始: E1 の pending を immutable な deliver_now へ swap
  2. ordered input を freeze し、ImGui → pelican.ui の順でルーティング
  3. UI 消費を適用した Actions フレームを一度だけ確定
  4. deliver_now を game handler へ配送(以後の emit は常に pending_next 行き
     — UI が emit した semantic は必ず次フレーム)
  5. ECS / game systems 更新
  ```

  この順により「semantic handler が未マスク入力を観測する」穴
  (現行は dispatchPendingEvents 中の handler が Actions を読める)と
  「UI emit の同フレーム配送」の両方が閉じる。**rpc ループも同一位相**
- **消費マスクの意味論(粗粒度で確定)**: UI がポインタをヒット/capture した
  フレームは、そのポインタのボタン・移動を **frame 単位で** Actions から隠す
  (同一フレームの「UI クリック + ワールドクリック」の混在は v1 では
  表現しない — 制限として明記。イベント単位の細粒度マスクは将来拡張)
- 再現単位 = **ordered events + UI document revision + ViewportTransform**
- **FrameInput 前提 WP の受け入れ範囲(R2 — I 系に切り出す際の必須項目)**:
  (a) event_seq の型(u64)・採番点(キュー投入時)・寿命(フレーム内)
  (b) immutable span の寿命規約 (c) UI 消費の単位(上記 frame 粗粒度)
  (d) Actions の一回評価化 + 外部マスク API (e) GLFW/rpc/replay の同一
  InputEvent 化 (f) 通常 loop と rpc loop の位相同一性テスト
  (g) `pelican.input_seq` の記録単位改訂(下記)
- **`design_input_actions.md` §4 の正本改訂(本設計と同時)**: 収録の記録単位を
  「L1 スナップショット列」から「**ordered InputEvent 列(event_seq +
  フレーム境界マーカー)**」へ変更。スナップショットは再生時に再構成する。
  I3 未実装のため互換負債なし

### 2-2. ViewportTransform と DPI(v1 の未決 1 を確定)

- `ViewportTransform { window_points ↔ framebuffer_px ↔ ui_units }` を
  フレームごとに 1 個作り、**レイアウト・描画・hit・rpc inject・golden が
  全て同じ変換を使う**
- **canonical space と丸め段(R3 で確定)**:
  - **レイアウトは ui_units の整数で計算**(ui_units = framebuffer_px /
    ui_scale。ui_scale は config 宣言、integer / free、既定 1.0 = px 1:1)
  - 座標系: 左上原点・Y 下向き。rect は **edge 表現**(`[left, top, right,
    bottom]`、右下は排他)
  - px への変換は **draw command 発行時に一度だけ**: 各 edge を
    `round(edge * ui_scale)`(half-up)— edge 単位の丸めなので隣接矩形に
    隙間/重なりが出ない
  - **hit test は丸め後の px rect**(描画と同じ実体)に対して行う
  - drag delta は ui_units(イベント間の座標差を逆変換)
- letterbox の余白は入力対象外(ヒットなし・capture 中の座標は clamp せず
  そのまま渡す)

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
- **UI-local command の commit 境界 = フレーム一括(R4 で確定)**: フレーム中の
  全イベントは**凍結されたツリースナップショット**に対してルーティングし、
  UiCommandBuffer はレイアウト/描画の前に一括 commit する。帰結として
  「同一フレームの 2 回目のクリックは 1 回目の変更前のツリーに当たる」—
  これは仕様(semantic trace に commit バッチとして記録)
- **Controller の権限(R4)**: `onPointer` ができるのは ①凍結スナップショットと
  自状態の読取 ②UiCommandBuffer への積み込み ③semantic sink への emit のみ。
  **ECS は read-only resolve まで(set 系呼出は禁止)** — ゲーム状態の変更は
  次フレームの semantic handler だけ。semantic lane の迂回路を作らない
- ボタンの押し込み表示は同フレームに出る(UI-local)、ゲームの反応は
  次フレーム(semantic)— 見た目の即応と決定性が両立
- 連続 drag をゲームが同 tick で要る場合は、イベントでなく **frame-scoped
  `UiActionFrame` query**(Actions と同型)を将来オプションとして予約
- **payload スキーマ(U-B3 v2 — 照合先の訂正)**: v4 の「by-name emit が既に
  payload 構築スキーマを持つ」は**事実誤認**だった(現行
  `EventTypeRegistration` は name / type_index / load 関数ポインタのみで、
  field の事前列挙はできない —
  `src/core/userpublic/details/event/registerer.hpp:34-38`)。照合先は新設の
  **EventPayloadSchema**(`docs/design_event_payload_schema.md` 正本)であり、
  その導入 WP が **U0 の前提**になる(§9)
- field 定義(JSON 形まで確定。型・required・range は UI 側では宣言しない —
  正は EventPayloadSchema 側):

  ```json
  "emit": { "on_click": { "event": "MenuOpened",
    "fields": [
      { "name": "source", "from": "stable_id" },
      { "name": "amount", "from": "static", "value": 1 },
      { "name": "delta",  "from": "drag_delta_ui" },
      { "name": "volume", "from": "widget_value(value)" } ] } }
  ```

  - `from` の enum: `static | stable_id | drag_delta_ui | widget_value(<path>)`。
    `<path>` は widget プロパティ名(v1 は 1 段のみ: `value` / `checked` /
    `text`。型は widget 型表が定義。ネストパスは予約)
  - UI ロード時の照合: `name` → schema field を引き、`from` ソースの型
    (static = JSON 値の型、stable_id = string、drag_delta_ui = vec2、
    widget_value = widget 型表)と schema field の型を突き合わせる。
    schema の required field が emit 定義に無い / emit 定義に schema に無い
    field がある(unknown-field 拒否)/ schema を持たないイベントへの
    fields 指定 — いずれもロードエラー
  - ロード時エラー(名前入り)を invalid fixture で固定(§7 の error code
    enum と対応): ①未知イベント名 ②未知 `from`・未知 widget_value path
    ③型不一致 ④range 逸脱(schema が range を宣言する field)
    ⑤required 欠落 ⑥schema 外 field ⑦payload 無しイベントへの fields 指定
  - **widget の同一性は `(document_key, stable_id)` を値でコピー**して載せる
    (semantic は次フレーム配送なので runtime WidgetId は reload/remove で
    stale になり得る — runtime handle は UI 内部限定)
- hot reload の swap 時、旧 document 向けの未 commit command は**破棄**し、
  破棄件数を reload status として返す(cancel コールバックが積んだ command も
  同様 — R4)

## 4. pelican.ui v1 — レイアウトの最小閉包(C7)

v1 の「アンカーのみ」を撤回し、以下を v1 に含める(これ未満では stack に
置いた button の矩形すら定まらない):

1. **size mode(軸ごと)**: `fixed | content | fill`。stack 子には `weight`
   (1 個だけの追加自由度)
2. **intrinsic size** の算出規則: label = グリフメトリクス、image = sprite
   実寸、button = label + padding、panel = 9patch マージン + content。
   panel(非 stack)の content = **anchor 子を除外した absolute 子の
   union bounds**(anchor 子は親サイズに依存するため intrinsic に参加しない)
3. `min_size` / `max_size`
4. stack: `direction / gap / padding / align / justify`。**同じ軸を anchor と
   stack の両方が支配しない**(親が stack なら子の anchor は無効 — エラー)
5. アンカー形式(R3 で確定): `anchor_min` / `anchor_max` は親矩形に対する
   `[0,1]²` の割合、`offsets = [left, top, right, bottom]`(ui_units 整数、
   アンカー点からの符号付き距離)
   - **端数の整数化(U-B2 で数値規範まで確定)**: 各 edge を
     `edge = round_half_up(parent_edge + parent_size × anchor) + offset` で
     計算する — anchor 由来の乗算だけ実数で行い、**round を 1 回だけ**通して
     整数 ui_units の世界に入る(以降の制約解法・fill 配分・min/max は全て
     整数演算)
     - **正本 = binary64 演算意味論(U-B2 round 2 で確定)**: 「実数としての
       half-up」ではなく、**各演算を IEEE 754 binary64・RN-even で 1 回ずつ
       丸めた結果**が規範である。手順(この順・この演算のみ):
       ①anchor = JSON 小数の correctly-rounded binary64。**正本 conversion は
       エンジン同梱の 1 実装**(MSVC/libc++ とも correctly-rounded な
       `std::from_chars` ベースの wrapper — stdlib の `strtod`・JSON
       ライブラリ既定・locale に依存する経路を UI ロードでは使わない)。
       gate: decimal midpoint・隣接値を含む「token → binary64 bit pattern
       (uint64 16 進)」fixture を Windows/MSVC と CI の他 compiler で一致
       ②`t1 = parent_size × anchor`(binary64 乗算)
       ③`t2 = parent_edge + t1`(binary64 加算)
       ④`t3 = t2 + 0.5`(binary64 加算 — **この丸め上がりも規範に含む**)
       ⑤`edge = floor(t3)` + offset(整数加算)
       **FP environment の実装契約**: UI レイアウトを実行する翻訳単位は
       MSVC `/fp:strict`(他 compiler は同等設定)でビルドし、FMA
       contraction 無効。レイアウト実行 thread で `FE_TONEAREST` を
       debug assert(rounding mode を変える外部ライブラリの混入検知)。
       gate は結果 vector だけでなくビルド設定と assert の存在を検査し、
       rounding mode / locale を意図的に変えた負テストを置く
     - test vector(規範。⑤の結果):
       `-1.5 → -1`、`-0.5 → 0`、`0.5 → 1`、`1.5 → 2`、
       `parent_size 101 × anchor 0.5 = 50.5 → 51`、
       **`parent_size 1 × anchor 0.49999999999999994`(= nextafter(0.5,0))
       → ④で 1.0 に丸み上がり → `1`**(数学的 half-up なら 0 だが、
       binary64 意味論が正)、
       `parent_size 1 × anchor -0.49999999999999994 → ④で +2⁻⁵⁴ → 0`、
       `parent_edge 2²⁶ + 0.5 境界`(大きな edge での加算丸め)
     - **整数 overflow 規則**: ⑤以降の制約解法・fill 配分・offset 加算は
       **int64** で行い、最終 rect の各 edge が int32 に収まらない場合は
       レイアウトエラー(error code `limit_exceeded`、path = 当該 widget)。
       int64 中間での overflow は入力上限(座標・offset は int32 宣言)から
       到達不能
     - **px 変換後の overflow も同規則**: `edge × ui_scale`(ui_scale ≤ 16)
       は int32 の ui_units から int32 超の px を作れる。px 変換も binary64 →
       floor → **int64** で受け、px edge が int32(かつ float32 の整数
       正確表現域)に収まらなければ `limit_exceeded`。記録規則: `rect_ui` は
       クリップ前の値、`scissor_px` は framebuffer へクリップ後の値
       (§7 invariant の containment はこの規則が前提)
     - §2-2 の px 変換丸め(`round(edge × ui_scale)` — 同じ binary64 意味論)
       は「canonical 整数 ui_units → framebuffer px」の**別段の量子化**であり
       二重丸めではない。direct float→px と同値になることは仕様として
       期待しない(例: 幅 101・anchor 0.5 は常にまず 51 ui_units になる)
6. `overflow: visible | clip`、`visible / hidden(空間残す)/ collapsed(残さない)`
7. **制約解法(R3 — 規範)**:

   ```
   pass 1(bottom-up): 全ノードの intrinsic を計算(content 用)
   pass 2(top-down):
     stack 軸: fixed → content(=intrinsic)を確定し、
       残り = 親サイズ − 確定分 − gap/padding(全て整数 ui_units)
       残りを fill/weight に比例配分(floor、余りは ui_units の整数として
       宣言順に 1 ずつ)
       min/max clamp → clamp されたノードを池から除いて再配分
       (反復は子数回で必ず停止)
       残りが負(fixed/content/min の合計 > 親サイズ)の一本化規則:
         fill 子 = 各自の min_size(min 未宣言は 0)。**min が fill=0 に勝つ**
         全子を min を保ったまま宣言順に詰めて配置し、親の終端 edge を
         超えた分はそのまま親外にはみ出す(rect は縮めない)。
         可視性は親の overflow(visible = 見える / clip = 親 rect で scissor)
     非 stack: anchor_min/max + offsets で直接確定
   循環はロード時エラー: content 親の中の fill 子 /
     content 親に対する stretch anchor(anchor_min ≠ anchor_max)子
   ```

8. **pixel snap**: レイアウトは ui_units の整数、px への丸めは §2-2 の
   1 箇所のみ。毎フレーム root から再計算(float 累積を持ち越さない)
9. v1 は **LTR 固定(明示エラー)**。RTL/safe area は v2 予約
10. **PSD → pelican.ui 変換表(R3/C8 — supported subset の確定)**:

    | pelican.layout の要素 | 写像 |
    |----|----|
    | group | 非 stack panel(skin なし) |
    | layer の位置/サイズ | `anchor_min = anchor_max = [0,0]` + absolute offsets |
    | opacity | color.a への乗算 |
    | visibility | `hidden` |
    | normal blend | そのまま |
    | **mask / layer effects / PSD テキスト / フォント / 非 normal blend** | **変換エラー**(名前入り。flatten は import ツールの将来オプション — 黙って劣化させない) |

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
  before/after/end のみ、現行 example は ui_pass 直書き)。**パス列の正本は
  `design_color_pipeline.md` v3 §2-2 の canonical frame-plan anchor**
  (`… → post_ldr → pelican_ui → debug_draw → debug_text → imgui →
  output_transform → present`)。UI はその `pelican_ui` anchor に入る —
  本書独自の列は持たない
  (v5 までの独自列規定は撤回)。feature 配列順への暗黙依存を排し、
  この順をプランの fixture で固定
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

**semantic fixture = 交換形式 `pelican.ui_semantic_fixture` v1(R5 — JSON 契約)**:

```json
{ "schema": "pelican.ui_semantic_fixture", "version": 1,
  "viewport": { "framebuffer_px": [1280, 720], "ui_scale": 1.0,
                "content_rect_px": [0, 0, 1280, 720] },
  "document": { "key": "hud", "revision": "sha256:..." },
  "widgets": [
    { "id": "root/play", "type": "button",
      "rect_ui": [40, 24, 160, 56], "clip_ui": [0, 0, 1280, 720],
      "layer": 0, "decl_seq": 3 } ],
  "draw_runs": [
    { "pipeline": "rgba_straight", "sampler": "nearest",
      "texture": "atlas:ui/page:0", "scissor_px": [0, 0, 1280, 720],
      "first_index": 0, "index_count": 6 } ],
  "input_trace": [
    { "event_seq": 17, "kind": "pointer_down", "pointer_id": 0,
      "button": "left", "position_ui": [20, 10], "target": "root/play",
      "consumed": ["mouse:left"], "effects": ["capture"] } ],
  "lifecycle": [] }
```

規約: rect は ui_units 整数の edge 表現(`[l,t,r,b]`、右下排他)/ widget の
同一性は `document_key + stable id パス`(runtime WidgetId は arena
単体テストに分離)/ pipeline・texture は**安定名**(実行時数値 ID 禁止)/
配列は決定順(widgets = 走査順、runs = 発行順、trace = event_seq 順)/
optional は省略(null 不使用)。

**正本 = 機械可読スキーマ(U-B4 v2)**: 本節の散文・例は説明であり、正は
**`docs/schemas/pelican.ui_semantic_fixture.schema.json`**(JSON Schema
draft 2020-12。本版と同時にコミット済み)。閉じ方の要点:

- 全 object が `additionalProperties: false`。全 property の型・
  required/optional・数値範囲(座標系 int32、`event_seq` 0〜2^53−1、
  count 系 ≥0)を宣言
- `input_trace` は `kind` を discriminator とする **`oneOf`**:
  - `pointer_down` / `pointer_up`: `pointer_id`・`button`・`position_ui`
    必須。`target` は optional(ヒットなし = 省略)
  - `pointer_move`: `pointer_id`・`position_ui` 必須。**`button` 禁止**
  - `pointer_cancel`: `pointer_id` のみ必須。**`position_ui`・`target`・
    `button` 禁止**(発生源に座標が無いケースがあるため)
- `lifecycle` も kind ごとの variant:
  `controller_init` / `controller_deinit` = `controller`(型名)+
  `widget` 必須 / `capture_cancel` = `widget` + `reason` 必須 /
  `document_swap` = `from_revision` + `to_revision` + `dropped_commands`
  (int ≥0)必須 / `command_dropped` = `count` + `reason` 必須
- **`consumed` の語彙を閉じる**: `mouse:left|right|middle|move|wheel`、
  `key:<key名>`、`touch:<slot番号>`、`pad:<control名>`。key/pad の名前表は
  schema の `$defs.ui_key` / `$defs.ui_pad_control` が正(v1 の実装対象は
  mouse のみ — 他は語彙として予約し、fixture に現れたら schema 違反ではなく
  「未実装機能の使用」としてロードエラー)
- **errors**(fixture 直下の配列、省略可): `{ phase, code, path, message }`
  - `phase` = `parse | layout | route | commit`
  - `code` は closed enum(`$defs.error_code`): `unknown_event` /
    `unknown_source` / `unknown_widget_value_path` / `type_mismatch` /
    `range_violation` / `required_missing` / `unknown_field` /
    `no_payload_event` / `unknown_widget_type` / `axis_conflict` /
    `layout_cycle` / `duplicate_stable_id` / `limit_exceeded` /
    `unsupported_control`
  - `path` は **RFC 6901 JSON Pointer**(入力 document JSON 内の位置)
  - 複数 error は検出順(parse = 文書順、layout = 解法順)。invalid case の
    期待値はこの errors 配列で、**照合は phase/code/path のみ**
    (message は人間向け — 比較対象外)
- **数値・比較の canonical 規則**: 比較は「**parse 後の semantic
  equality**」が正(JSON テキストの byte 比較はしない — RFC 8785 等の
  canonicalization には依存しない)。writer 側規則として integer field は
  integer 表記(`-0`・指数表記・`1.0` 禁止)、float が許されるのは
  `ui_scale` のみ(shortest round-trip 表記)
- **event_seq** = number、2^53−1 以下を writer が保証(超過は fixture 生成
  エラー)。event_seq は FrameInput 正本(WP69)の u64 単調列で、リセットは
  **プロセス起動時のみ**。replay/golden は 1 プロセス内で完結させ、
  境界をまたぐ event_seq 比較はしない
- **JSON Schema は「構造」の正本であり「意味」の正本ではない**(round 2
  U-B4 の受理): 逆転 rect・重複 id・非単調 event_seq 等は schema-valid に
  なり得る。意味の正本は次の **normative semantic invariant 表**とし、
  これを検査する **semantic validator が U0 ゲートの第 3 段**になる
  (実装と expected が同じ不正値を出しても equality だけでは通らない):

  validator の入力は **fixture 単体で完結**する(document や layout
  snapshot を要求しない)。そのために widget record へ `overflow`
  (`visible | clip`、省略 = visible)を追加した(schema v1 に含む)。
  親は stable-id パスから導出する(`root/menu/play` の親 = `root/menu`)。

  | # | invariant | 規則 |
  |---|-----------|------|
  | I1 | rect 順序 | 全 rect_ui/clip_ui/scissor_px/content_rect_px で `left ≤ right` かつ `top ≤ bottom` |
  | I2 | 祖先存在 | 非 root widget のパス接頭辞(親・祖先)は全て widgets[] に存在する |
  | I3 | clip 一致 | widget の `clip_ui` = 「`overflow: clip` な祖先の rect_ui と viewport content rect の交差」に等しい(I2 により計算可能) |
  | I4 | id 一意性 | widgets[].id は fixture 内で一意 |
  | I5 | decl_seq / event_seq | widgets の decl_seq・input_trace の event_seq は狭義単調増加 |
  | I6 | draw run index | run 0 の `first_index` = 0。以降は前 run の `first_index + index_count` に一致(連続)。最終 end = 総 index 数。総 quad 数(Σ index_count / 6)≤ 16384 |
  | I7 | effects / consumed | 配列内重複なし(schema の uniqueItems と二重化) |
  | I8 | scissor containment | `0 ≤ left ≤ right ≤ framebuffer_px.x` かつ `0 ≤ top ≤ bottom ≤ framebuffer_px.y`(§4 の「scissor_px はクリップ後」規則の帰結) |
  | I9 | texture 名 | パス走査不可(`..` 禁止 — schema pattern と二重化) |
  | I10 | 参照整合 | input_trace の `target`・lifecycle の `widget` は widgets[] に存在する。例外: `capture_cancel` の reason が `remove | reload | scene_unload` の場合のみ不在を許す(消滅が原因のキャンセル) |
  | I11 | capture 状態機械 | pointer_id(+button)ごとに状態を追跡: `capture` effect は `target` 必須かつ非 capture 状態でのみ / `release_capture`・`click`・`drag*` は同 pointer の先行 `pointer_down`(capture 成立)が必要 / `cancel` は capture 中のみ / `click` と `cancel` は同一イベントに同居しない / `hover_enter`・`hover_exit` は交互 |
  | I12 | viewport | content_rect_px ⊆ [0,0,framebuffer_px] |

  **effects は網羅的な semantic 出力である**(I11 が検査する — 「実装が
  出したものを写すだけ」ではない)。invariant を足すときは
  schema-valid/semantic-invalid fixture を同時に足す(下記 coverage)。

- **fixture coverage manifest**:
  `test/fixtures/ui_semantic/normative/coverage.json` に
  「schema の enum 値 / oneOf branch / invalid class / **semantic
  invariant(I1〜I12)** → それを行使する fixture ファイル」の対応表を置く。
  被覆の主張は正確に(round 3 指摘の受理):
  - **小 enum(kind / button / effects / reason / phase / code / sampler)=
    全値を列挙**
  - **`ui_key` / `ui_pad_control` = 語彙 branch 代表 1 件ずつ**(全値
    fixture は置かない)。代わりに **enum 同期テスト**を置く: schema の
    `$defs` から enum を機械抽出し、`consumed_control` の pattern
    alternation と一致することを CI で検査(enum 追加の検出は fixture
    でなくこのテストが担う)
  - semantic invariant は **I ごとに「その 1 項だけ違反する
    schema-valid/semantic-invalid fixture」**を `semantic_invalid/` に置く
  - CI は manifest 自体の schema 検証 + 全行の実在検査 + 「schema から
    機械抽出した enum/branch/invariant 集合 ⊆ manifest key 集合」を検査
    (未被覆 = fail)
- **normative fixture 一式(本版と同時にコミット済み)**:
  `test/fixtures/ui_semantic/normative/`
  - `valid/` — schema に適合すべき実物: `minimal.json`、
    `input_trace_variants.json`(kind 4 種 + effects 全種 + button 3 種 +
    consumed 4 語彙クラス)、`lifecycle_variants.json`(kind 5 種 +
    capture_cancel reason 全 7 種 + command_dropped reason 全 3 種)、
    `errors_variants.json`(error code 全種)
  - `invalid/` — schema 検証で**落ちるべき**実物(1 違反 1 ファイル):
    未知 property / 未知 enum 値 / variant 必須欠落 / variant 禁止 field
    (pointer_move の button、pointer_cancel の position)/ 範囲外
    event_seq / 型違い / 未知 error code / root 必須欠落 / 不正 revision
    pattern / null 値 / 非整数座標 / 不正 consumed / traversal texture 名 /
    effects 重複(uniqueItems)
  - U0 ゲートは **3 段**: ①fixture の schema 検証結果が期待どおり +
    coverage manifest 全行被覆 ②semantic validator(上の invariant 表)
    ③実装出力 ↔ expected fixture の semantic equality。
    UI ロードエラー系の入力 document fixture は document JSON 文法の確定
    (U0 実装の最初の成果物)と同時に追加し、期待値側は
    `errors_variants.json` の形式に従う

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
- multi-viewport / docking は v1 OFF 固定。clipboard/IME/OS cursor・
  **device loss 時のバックエンド資源再生成**の対応可否を導入 WP で明示(R6 補)

## 9. 実装順(v7 — U0 起点)

| 段階 | 内容 | ゲート |
|------|------|--------|
| **U0** | **純 CPU**: スキーマ・レイアウト・WidgetId arena・ordered 入力ルーティング・capture・draw command 生成 + **semantic validator の実装**(**依存: FrameInput WP69(済)+ EventPayloadSchema WP(設計 Accept 後)**) | §7 の 3 段ゲート(schema 検証 + coverage manifest / semantic validator / expected との semantic equality)全通過(GPU なし) |
| U1 | K3 アトラス接続・quad buffer・stable run・clip・ui feature 化・panel/image(**依存: 色パイプライン C1b 完了後**) | 2 アトラス交互重なり・nested clip・旧 UI 移行・golden |
| U2 | bitmap label/button・UI-local 状態・E1 emit・rpc の ordered click/drag/replay | **debug_text の旧経路/共通経路を同一 fixture に描いて byte-exact 比較する互換ゲート**(R6 — tolerance 0 を緩めない。意図的な色意味論変更時のみ理由記録付きの versioned baseline 更新 → 以後再び 0) |
| U3 | Controller factory/lifecycle・hot reload トランザクション・gauge/stack/トークン | remove/hide/reload 中の capture cancel・init/deinit 列 |
| U4 | ImGui ユニット(§8) | OFF/headless/golden/replay の完全不在・入力優先順位 |
| U5 | PSD subset converter | K3 後。未対応表現の明示エラー |

前提工事(U0 より前 or 同時): **FrameInput(InputState 改訂)**と
input_seq v1 の改訂 — 入力設計(I 系)側の WP として切り出す。

## 10. 未決事項(v4 で残る最小)

1. SDF/日本語テキスト(text_hud v2 と同時 — 本書 v1 は ASCII bitmap 固定)
2. scroll / grid / wrap / RTL / safe area(v2)
3. bindless テクスチャページ(native 高速路 — マテリアルの classic/bindless と同時期)
4. 2D ゲーム層とのバッチャ共有形態(別文書で)
