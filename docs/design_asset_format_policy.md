# アセットフォーマット方針: ランタイム二層モデル

対象読者: エンジン担当 + DCC/ツール側担当。
ステータス: v1.1 ドラフト(2026-07-02。レビュー 1 巡反映: EXR をランタイム層へ
限定昇格(すぐ使うため)、音声は WAV のみで開始、USD は関心ありとして変換レーンを
早期候補に。FBX = Blender headless は承認済み)。
前提: `external_tools_requirements.md`(R1〜R10)、`design_project_dcc_houdini.md`
(import manifest)、`design_project_format.md`([PF] v6 凍結)、[PFW] v1.3 凍結。

## 0. 目的と原則

ゲーム制作で流通するファイルフォーマット(FBX / USD / Alembic / EXR / PSD /
BVH / WAV ...)をプロジェクトでどう取り回すかを 1 枚で決める。

**二層モデル**を採用する:

| 層 | 定義 | 例 |
|----|------|-----|
| **ランタイム層** | エンジンが直接読む**閉じた小集合**。§2 の表が全て | glb, PNG, transform_seq |
| **ソース層** | エンジンは**決して読まない**。import 時にランタイム層へ変換され、`imports/` + manifest に着地する | FBX, USD, PSD, .hip |

原則:

1. **ランタイム層への追加は本書の改訂を要する**(= エンジンへのリーダ・
   依存ライブラリ追加の門番。tinygltf/stb の小ささを守る)。
2. **ソース層のライブラリをエンジンにリンクしない**(FBX SDK・USD・Alembic・
   OpenEXR 等。ライセンス・ビルド時間・保守の全部で重い)。変換はツールレーン。
3. 変換は**明示的・決定的(R2)・ファイルとして残る**。Unity/Unreal の
   「インポートキャッシュに隠れる」方式は採らない — 変換結果は `imports/` の
   納品物であり、pelican.import manifest(DCC 文書 §3)が出所を記録する。
4. **サブセット原則([PFW])**: web が読むのはランタイム層のさらに部分集合
   (現状 glb と rendering config。VAT 等は需要が出てから追随)。

## 1. ランタイム層(エンジンが読むもの)

| 種別 | 形式 | 状態 | 備考 |
|------|------|------|------|
| シーン・メッシュ・スケルタルアニメ | glTF 2.0 (.glb) | **実装済み**(tinygltf) | VRM も glTF 拡張として受理済み |
| 頂点キャッシュ | pelican.vat(glb 内 bufferView + extras) | 仕様凍結・再生未実装(WP20 候補) | R5 |
| 剛体シーケンス | pelican.transform_seq (.jsonl) | **実装済み**(SeqPlayer/WP17) | R4 |
| パーティクル点群 | pelican.pointcache(glb 内、方向のみ決定) | 仕様未着手 | DCC 文書 §4-1 |
| 画像(開発レーン) | PNG / JPG | **実装済み**(stb_image) | 可搬・可読を優先 |
| 画像(GPU レディレーン) | **KTX2 + Basis Universal** | **将来追加を本書で予告**(WP 候補) | ミップ済み・BCn transcode・glTF 側は KHR_texture_basisu。リーダは libktx |
| 画像(HDR レーン) | **EXR(限定スコープ)** | **追加決定(2026-07-02 レビュー: すぐ使う)。WP 候補** | リーダは **tinyexr(ヘッダオンリー)**。single-part・half/float・非 deep のみ。用途は IBL/HDRI・LUT。大量テクスチャ運用になったら KTX2 HDR へ最適化移行 |
| 音声 | **WAV(PCM)のみで開始** | **未実装**(sound モジュールは空) | 実装時に miniaudio 系ヘッダオンリーを第一候補。圧縮レーン(Ogg 系)はサイズが問題になってから(§6 未決 1)。MP3 は採用しない |
| 設定・データ | JSON / JSONL(schema+version 必須、R10) | 実装済み | |
| シェーダ | GLSL / .spv / WGSL(stem 規約) | [PFW] §4 | |

- **EXR は限定スコープでランタイム層に入れる**(v1.1 改訂)。フル OpenEXR は
  リンクせず tinyexr(ヘッダオンリー)を使う。deep・multi-part・タイル等の
  高機能は非対応のまま(必要になったらソース層で KTX2 に変換して受ける)。
- **DDS は採用しない**(KTX2 が上位互換。二重にしない)。

## 2. ソース層 → 変換レーン対応表

| ソース形式 | 主な出所 | 変換先 | 変換手段(標準レーン) |
|-----------|---------|--------|----------------------|
| FBX | Maya / 3ds Max / Marvelous / モーキャプ既製品 | glb | **Blender headless**(`blender --background --python fbx2gltf.py`)。FBX SDK はどこにもリンクしない |
| OBJ | 汎用・スカルプト | glb | 同上 |
| Alembic (.abc) | 布・流体・Houdini/Blender キャッシュ | pelican.vat / transform_seq | Houdini アダプタ or Blender レーン(R5) |
| USD / USDZ | 新しめの DCC 連携・Omniverse | glb | **ソース専用は維持**(ランタイム層には入れない)が、**関心あり(2026-07-02 レビュー)**: usd→glb 変換スクリプトを `pelican-import-tools` の早期候補に昇格(Blender 経由 or guc)。試作時期は §6 未決 4 |
| BVH / C3D | モーションキャプチャ | glb(スケルタルクリップ) | mocap lab(R3 既定) |
| PSD / TIFF / TGA | テクスチャ原本 | PNG →(将来)KTX2 | テクスチャベイク側で書き出し。`toktx` CLI を KTX2 エンコーダの標準にする |
| .sbsar(Substance) | マテリアル | テクスチャセット(PNG/KTX2) | ツール側ベイク(sbsrender) |
| EXR | レンダ出力・HDRI | (v1.1 で限定スコープのままランタイム層に昇格 — §1) | tinyexr が読めない高機能 EXR のみ、ソース層で変換して受ける |
| .hip / .blend / .ma 等 | DCC プロジェクト | (変換しない) | **ソースの中のソース**。manifest の `source.file` に記録されるだけ |
| PLY / LAS(スキャン・点群) | フォトグラメトリ | glb メッシュ / pointcache | 需要が出てから |
| WAV 32bit float マスター | DAW | WAV/Ogg(ランタイム仕様) | 音声実装後に規定 |

規則: **この表にない形式が来たら、まず既存の変換先に写像できないか検討し、
できない場合のみ本書を改訂**する(新形式の発明は最後の手段。DCC 文書 §1 と同じ)。

## 3. 変換の実行主体と置き場所

- `pelican_cli import`(WP21 候補)は**検証 + 登録**であり、変換そのものは
  行わない(エンジン repo に変換依存を持ち込まないため)。
- 変換スクリプト群は外部ツール群と同列の独立リポジトリ
  **`pelican-import-tools`**(Python)に置く。R1 構造(core/cli)・R2 決定性・
  R10 バージョン埋め込み・manifest 書き出しを守る。
  v1 の中身は `fbx2gltf.py`(Blender headless)1 本から始める。
- Blender headless を汎用コンバータに使う判断の理由: 無料・スクリプタブル・
  DCC bridge で既にエコシステム内・FBX/OBJ/ABC を一括カバー。専用 C++
  コンバータ(assimp 等)は品質問題が出た形式に限って個別導入を検討する。

## 4. 周辺領域(予約のみ・本書では決めない)

- **コリジョン形状**: glb 内の命名規約 or ノード extras(`pelican.collision`)で
  物理担当と別途設計。ソース形式は増やさない(glTF に乗せる)
- **ゲームデータテーブル**(CSV/スプレッドシート → JSON): 別トラック。
  ランタイム層は JSON のまま
- **フォント**(TTF → アトラスベイク): UI 拡張時。ランタイムはテクスチャ+
  メトリクス JSON になる想定(TTF パーサをエンジンに入れない方向)
- **動画**(カットシーン・動画テクスチャ): スコープ外

## 5. エンジン側 WP への反映(候補)

| 候補 | 内容 | 依存 | 規模 |
|------|------|------|------|
| WP23 | KTX2 リーダ(libktx + BasisU transcode、glTF KHR_texture_basisu 対応) | WP18 | 中 |
| WP24 | 音声再生基盤(WAV のみ、miniaudio 系) | なし(独立) | 中〜大 |
| WP26 | EXR リーダ(tinyexr、限定スコープ。IBL/HDRI テクスチャとして登録) | WP18 | 小〜中 |

EXR(WP26)は「すぐ使う」との判断(2026-07-02)により、
WP23(KTX2)より先に着手してよい。

番号は仮。WP20〜22(DCC 文書 §5)より優先度は低い —
**絵作りのクリティカルパス(VAT / import)を先に**、KTX2 はテクスチャ量が
問題になってから、音声はゲームループ側の要求が固まってから。

## 6. 未決事項

1. 圧縮音声レーン(Ogg Vorbis/Opus)の導入時期(WAV のファイルサイズが
   問題になってから。コーデック選択もその時点で)
2. EXR → KTX2 HDR への最適化移行時期(tinyexr 直読みで当面は足りる想定)
3. `pelican-import-tools` リポジトリの作成時期(houdini-adapter と同時が候補)
4. USD 変換レーン(usd→glb)の試作時期(関心あり。Blender 経由 vs guc の
   品質比較から)
5. web への KTX2 追随(KHR_texture_basisu は three.js 等で実績あり。
   サブセット原則どおり pelican 側導入後に判断)
