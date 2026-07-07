# ビルド階層: プロジェクト導出の配布ビルド(バイナリからのパージ)

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-07。レビュー前)。
前提: `design_render_feature_modules.md`(参照パージ)、[PF]/[PFW]、
`design_asset_format_policy.md`(二層モデル)、WP21(import/devcli)。

## 0. 要求(2026-07-07 ユーザー方針)

参照パージ(実行時コスト・リソースを消す)に加えて、
**配布サイズのためにバイナリからコードを消す経路も「ある程度使いやすく」**したい。

使いやすさの定義: CMake フラグを手で並べさせない。
**プロジェクトが宣言している使用機能から、配布ビルド構成を導出する。**

## 1. バイナリサイズの内訳(何を消せるか)

| 種類 | 中身 | 消し方 |
|------|------|--------|
| 機能コード | VAT / EXR / compute 実行系 / rpc / SeqPlayer / import CLI / アクション層 … | 機能ユニット単位のコンパイルアウト(§2) |
| 埋め込みリソース | b::embed のシェーダ・feature fragment・default 画像等(engine_resources 30 id) | 配布時の embed セット絞り込み(§3) |
| 大物依存 | **shaderc(MB 級・最大)**、tinygltf、GLFW、quill 等 | shaderc は既に OFF 可。他は共通基盤として残す(効果小) |

## 2. 機能ユニット(PELICAN_WITH_*)

参照パージ可能な各サブシステムを CMake オプションの単位にする。
アーキテクチャ上すでにファイル・モジュール分離されているため、
コンパイルアウト = ソース除外 + 登録スタブ化で済む。

| オプション | 対象 | 備考 |
|-----------|------|------|
| `PELICAN_WITH_VAT` | vatformat / vatplayer / vat.vert | |
| `PELICAN_WITH_EXR` | imageloader の EXR 分岐 + tinyexr | |
| `PELICAN_WITH_COMPUTE` | computetask / framegraphruntime の compute 側 | プランナ(純ロジック)は常在 |
| `PELICAN_WITH_RPC` | jsonrpc / rpcserver | |
| `PELICAN_WITH_SEQPLAYER` | seqplayer | DCC プレビュー用。ゲーム配布では通常不要 |
| `PELICAN_WITH_INPUT_ACTIONS` | actionmap | |
| `PELICAN_WITH_HOT_RELOAD` | シェーダ監視 | 配布では常に OFF 相当 |
| (既存)`PELICAN_RUNTIME_SHADER_COMPILER` | shaderc | §4 の前提に注意 |

規約(全ユニット共通・レビューで守らせる):

1. **既定はすべて ON**(開発ビルド = フル。日常の体験は何も変わらない)
2. OFF 時に該当機能を参照する入力が来たら、**silent skip ではなく明確なエラー**:
   「この バイナリは PELICAN_WITH_VAT=OFF でビルドされています」
   (参照パージの「reject は必ず理由を言う」文化の延長)
3. ユニットの粒度は粗く保つ(10 個前後まで。ファイル単位の細分化はしない)
4. テスト: フル構成が正。各ユニット OFF は「ビルドが通る + OFF エラーが出る」の
   スモークのみ CI 化(組合せ爆発はテストしない — 単一 OFF × N 通りだけ)

## 3. 導出: プロジェクト → 配布プリセット

手動フラグ指定の代わりに、devcli に導出コマンドを足す:

```
pelican_cli dist-config <project> --out dist-preset.cmake
```

やること:

1. プロジェクトを解釈(既存の解釈系を流用)し、使用機能を列挙:
   - rendering config の `features` / `compute_tasks` の有無
   - asset_data_json / manifest 内の VAT・EXR アセットの有無
   - `input_actions_json` の有無
   - rpc / seqplayer は既定 OFF(配布ゲームに不要。`--with rpc` で明示追加)
2. 対応する `PELICAN_WITH_*` の ON/OFF 一覧を CMake キャッシュファイル
   (または CMakePresets.json の configurePreset)として出力
3. **embed セットの絞り込み一覧**も同時に出力: プロジェクトが参照する
   engine:// id だけを埋め込む(engine_resources の部分集合。
   レジストリ一致テストは「フルセット」前提なので、配布ビルドでは
   `PELICAN_EMBED_SUBSET` としてビルド時にリスト注入する形)

使い方(全体で 3 コマンド):

```
pelican_cli dist-config projects/mygame --out build-dist/preset.cmake
cmake . -B build-dist -C build-dist/preset.cmake -DCMAKE_BUILD_TYPE=Release
cmake --build build-dist --config Release
```

## 4. shaderc と stem バリアントの関係(配布の前提条件)

配布ビルドで shaderc を OFF にするには、stem 参照 + feature defines の
**バリアント事前焼き出し**が必要(既知の未決)。方針だけ確定する:

- `pelican_cli dist-config` が使用 define 集合を列挙できる(合成結果から確定的)
  → `dist-bake` サブコマンド(将来)が「stem × 実際に使う define 集合」の
  .spv を生成して配布プロジェクトに同梱
- それまでの間、**配布ビルド v1 は shaderc ON のまま**でよい(サイズ削減の
  残りを先に回収する。shaderc OFF は dist-bake とセットで v2)

## 5. 検証

- 配布ビルドの正しさ = **その project での headless golden 1 周**
  (`--project <対象> --render-out` の比較。フル構成との画一致)
- CI はフル構成のみ常設。dist はリリース手順の一部として手動/スクリプト実行

## 6. 実装順(WP 候補)

| 段階 | 内容 | 依存 |
|------|------|------|
| B1 | ユニット化 4 つ(VAT / EXR / RPC / SEQPLAYER)+ OFF スモーク CI | なし |
| B2 | `pelican_cli dist-config`(機能列挙 + プリセット出力) | B1, WP21 |
| B3 | embed サブセット注入 | B2 |
| B4 | `dist-bake`(バリアント焼き出し)+ shaderc OFF 配布 | B2, 未決解消 |

## 7. 未決事項

1. GLFW / ウィンドウ系を headless 配布(サーバレンダリング用途)で外すか
2. 音声・ロジック VM・OpenXR(未実装)は最初からユニットとして生まれる
   (パージ要件 — 各設計文書に PELICAN_WITH_* を明記させる)
3. embed サブセットとレジストリ一致テストの配布ビルドでの扱い(§3-3 の詳細)
4. CMakePresets.json 化 vs -C キャッシュファイルの選択(実装時に)
