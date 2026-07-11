# 色パイプライン(v1)

対象読者: エンジン担当・シェーダ/UI を書く人。
ステータス: v1 ドラフト(2026-07-11。**ユーザー決定: SRGB swapchain +
linear 統一の「しっかり版」— golden の一斉再基準化 1 回を許容**。
codex 敵対レビュー前)。
前提: HDR/tonemap feature(WP30)、`design_ui_2d_foundation.md` v4 B1、
`docs/shader_contract.md`、UI v2 レビュー R6(baseline 更新の例外手続き)。

## 0. 原則(現代エンジンの標準形に合わせる)

1. **シェーダから出る色は常に linear**。sRGB エンコード(OETF)は表示直前の
   1 箇所だけ — 可能ならハードウェア(`*_SRGB` attachment)に任せる
2. 作業空間 = linear。シーン内部は HDR(既存の R16G16B16A16_SFLOAT)
3. エンコードの適用回数は**常にちょうど 1 回**(二重適用・欠落を検出する
   テストを常設する — §3)

## 1. 現状(実測 — UI v3 レビュー B1 の指摘)

- swapchain 選択は `R8G8B8A8_UNORM` / `B8G8R8A8_UNORM` を優先、
  headless も `R8G8B8A8_UNORM` 固定 — **UNORM view はエンコードしない**
- 既存シェーダは事実上 display-referred(sRGB 空間の値)をそのまま書いて
  つじつまが合っている状態(= linear ワークフローになっていない)
- テクスチャの sRGB view 適用状況は未監査(→ §5 C0)

## 2. 目標状態

### 2-1. attachment / swapchain(列挙表)

| 対象 | フォーマット | 備考 |
|------|-------------|------|
| swapchain(SDR) | **`B8G8R8A8_SRGB` / `R8G8B8A8_SRGB` を優先選択** + `VK_COLOR_SPACE_SRGB_NONLINEAR_KHR` 明示 | シェーダは linear を書き、ハードウェアがエンコード |
| swapchain フォールバック | `*_UNORM`(SRGB 非対応デバイスのみ)| **present 直前に encode 専用 fullscreen パスを 1 個挿す**(§2-3)。選択結果はログ + get_status に出す |
| headless/offscreen | `R8G8B8A8_SRGB` | readback バイト = エンコード済み = **PNG/golden はそのまま sRGB**(意味論が明確になる) |
| scene color(HDR 中間) | `R16G16B16A16_SFLOAT`(linear)— 現行どおり | |
| LDR 中間(tonemap 後) | `R8G8B8A8_SRGB` view | |

### 2-2. テクスチャと authored 色

- **color テクスチャ**(baseColor/emissive)= sRGB view でサンプル(自動で
  linear に復号)。**data テクスチャ**(normal/metallicRoughness/AO)= UNORM。
  glTF の規約どおり。loader の format 選択規則として明文化
- **authored 色**(scene のライト色・UI スキン・pelican.material の factor):
  JSON 上は sRGB 表記 → **パース時に 1 回だけ linear へ変換**。
  変換式は IEC 61966-2-1 の区分関数(c ≤ 0.04045 → c/12.92、
  それ以外 → ((c+0.055)/1.055)^2.4)、clamp [0,1]、u8 への量子化は
  round-half-even。ライトの強度(intensity)は元々 linear のスカラー —
  変換しない

### 2-3. シェーダ側の規約

- 全シェーダの出力 = **linear のみ**。OETF をシェーダに書くことは禁止
  (フォールバック時も encode は専用最終パスが担う — 個々のシェーダは
  swapchain 形式を知らない)
- blend は linear 空間で行われることになる(SRGB attachment 上の blend は
  ハードウェアが decode→blend→encode)— **半透明の見えは現行(sRGB 空間
  blend)から変わる**。これは修正であってバグではない(§3 の再基準化に含む)
- tonemap(既存 hdr feature)の出力段は「linear LDR を書く」に改める
  (現行がカーブ内でエンコードを済ませている場合はそこを外す — C0 監査対象)

## 3. 移行(golden 一斉再基準化 — 1 回きり)

R6 レビューが認めた唯一の例外手続きをここで使う:

1. **C0 監査**: 全シェーダ・全 RT の色空間実態の棚卸し(何が display-referred
   か・テクスチャ view の現状)→ 変更対象リスト
2. **C1 実装(単独ゲート)**: §2 への移行を一括で行い、
   **golden 全ケースを理由記録付きで再生成**(この設計書と PR が理由の記録。
   更新後は再び tolerance 0 / exact)
3. **encode 正当性テストの常設**:
   - known-value: linear 0.5 を書いた readback が 188(±0)であること
   - グラデーション ramp の golden(二重エンコード/欠落は ramp の形で即発覚)
   - SRGB / UNORM フォールバック両経路で同一 readback(フォールバックの
     encode パスの等価性)
4. 以後の golden は「**エンコード済み sRGB バイト**」という明確な意味論を持つ

## 4. HDR ディスプレイ出力(将来トラック — 席だけ予約)

- HDR10: `A2B10G10R10_UNORM` + `VK_COLOR_SPACE_HDR10_ST2084`(PQ)
- scRGB(Windows): `R16G16B16A16_SFLOAT` + extended sRGB linear
- tonemap 出力段に「output transform の差し込み点」を予約(SDR sRGB /
  HDR10 PQ / scRGB の切替点)。**本設計はエンコードの正しさのみを扱い、
  トーンカーブ(ACES/AgX 等)の選択は別トラック**(現行カーブ維持)

## 5. 実装順(WP 候補)

| 段階 | 内容 | 依存 |
|------|------|------|
| C0 | 色空間監査(棚卸しレポート — コード変更なし) | なし |
| C1 | §2 一括移行 + golden 再基準化 + encode テスト常設(単独ゲート) | C0, **WP70(set 0 再編と衝突するため後)** |

## 6. 未決事項

1. exposure 制御(カメラ/シーン単位)— tonemap トラックと同時
2. トーンカーブの選択肢(ACES / AgX)— 別トラック
3. OCIO 統合 — 遠い将来(グレーディング需要が出たら)
