# Pelican2 マニュアル

対象: pelican2(HEAD=`82054c05`・2026-09-17、branch
`codex/render-target-runtime-slice`)/ 初版 2026-07-10、WP64〜111 反映
2026-07-16、WP112〜137 反映 2026-07-17、WP138〜179 の状態差分を
2026-07-19 に同期、WP180〜238e(レンダラ再構築)を 2026-07-31、
WP375c〜385(scene load transaction / game DLL ABI / 宣言 schema v3 /
GPU lease)を 2026-09-04、**v0 の段(AG-0 〜 R-3 と Studio 側の N-1 〜 N-3 / N-5 / Q-1 / Q-1b。共通グラフ / 姿勢グラフ /
package / 共有部品 / オフライン出力 / ノードキャンバスと自動操作面)を 2026-09-17** に反映

> ⚠ **章ごとに鮮度が違います。**各章の冒頭「対象:」行がその章の基準です。
> 現在の開発状況は [v1 開発計画](../v1_development_plan.md)、v0 の公開仕様は [凍結一覧](../v0_freeze.md) を参照してください。2026-10-01 に `develop` の `c6df4822` と照合し、用語と一部の仕様説明を訂正しています。
> **WP239〜374(2026-08)は [第11章 §11.1](11_status.md) の WP 表に行として転記されていません** ——
> WP 単位の仕様・履歴の参照先は [../implementation_plan.md](../implementation_plan.md) です。
>
> **WP**(Work Package)= 開発の作業単位です。台帳 [../implementation_plan.md](../implementation_plan.md) に 1 行として登録され、**1 WP = 1 ブランチ = 1 マージ**で進みます。**番号は 2 系統あります** —— WP386 までの**通し番号**(`WP380a` のような枝番付き)と、v0 の段に使われた**記号 ID**(`AG-0` / `AG-1b` / `P-2a-1c` / `X-1` / `R1-2` / `N-2` …。段の系統 + 番号)です。本文のバッジも同じ規則で `✅WP382a` / `✅P-2a-3` のように書きます。状態の一覧は [第11章 §11.1](11_status.md)(v0 の段は末尾の表)、そのほかの記号と基本語は [第1章 §1.0](01_overview.md) と [第14章 用語集](14_glossary.md) にまとめてあります。
>
> **v0 で凍結した面(文書 schema・RPC 名・共通グラフの op / 型 / sink・ABI・package manifest)の正本は [../v0_freeze.md](../v0_freeze.md)** です。本マニュアルはその面の「使い方」を書きます。

より深いクラス単位のコード解説は姉妹文書 [../source-code-guide/](../source-code-guide/README.md) にあります(本マニュアル = 使い方と現状、source-code-guide = 内部構造の深掘り)。

Pelican2 の**使い方と設計を人間向けに解説する公式マニュアル**です。docs/ 直下の設計文書(design_*.md)が「これから作るもの・作ったものの意図」を書くのに対し、本マニュアルは「**いま何がどう動くか**」を書きます。原則は 2 つ:

1. **コードが正** — 設計文書とコードが食い違う場合、実装の挙動を記載し、食い違いは [第11章 §11.3](11_status.md) の一覧に台帳化する。
2. **実装状況を偽らない** — 機能ごとに ✅実装済み / 🚧実装中 / 📐設計のみ(未実装) のバッジを付ける。設計文書にしかない機能を「使える」とは書かない。

## 目次

| 章 | 内容 |
|---|---|
| [第1章 エンジン全体像](01_overview.md) | Pelican2 とは何か。リポジトリ地図・レイヤ規則・横断的な設計原則・開発体制 |
| [第2章 ビルドと起動](02_getting_started.md) | 必要環境、ビルド、example の起動、最初のプロジェクト作成、ヘッドレス実行、テスト |
| [第3章 プロジェクト形式](03_project_format.md) | project.json のスキーマ(`requires` = 機能ユニットの要求を含む)、PathResolver(project:// / engine:// / user:// / #フラグメント)、asset store、**package**(外から持ち込んで project に複製する)、**共有部品(render component)**の export / import |
| [第4章 シーンと ECS](04_scene_ecs.md) | pelican.scene v2、コンポーネント全種、シーンロードの流れ、ECS の要点、シーン遷移 |
| [第5章 アセットパイプライン](05_assets.md) | 二層モデル、asset_data.json、DCC 連携(import / transform_seq / VAT)、対応形式一覧 |
| [第6章 レンダリング](06_rendering.md) | rendering config(`pelican.graph` root)、パス種別、feature、フレームグラフ、シェーダ基盤(stem / ホットリロード / set 規約)、**共通グラフのノード語彙**(§6.17) |
| [第7章 入力と UI](07_input_ui.md) | 入力四層、actions.json、アクションセットスタック、UI の現状、debug_text HUD |
| [第8章 ゲームロジック](08_gameplay.md) | ゲームコードの書き方(システムと **オブジェクト behavior**)、GameContext、イベント、カメラ、物理クエリ、決定性、オーディオ、永続化 |
| [第9章 Web プロファイル](09_web.md) | サブセット原則、web ビューアでの開き方、シェーダ stem と WGSL、web 互換チェックリスト |
| [第10章 ツールリファレンス](10_tools.md) | pelican_player 全 CLI(オフライン出力の `--start-time` を含む)、JSON-RPC 全メソッド(graph 系 6 本・package 2 本・共有部品 2 本を含む)、pelican_cli(`package` / `graph-catalog` を含む)、配布ビルド、Studio、テスト基盤、`tools/` の保守スクリプト |
| [第11章 実装状況と文書マップ](11_status.md) | WP 全台帳、設計文書⇔章の対応表、既知の食い違い一覧、本マニュアルの更新ルール |
| [第12章 コード読解ガイド](12_codemap.md) | **技術解説書**。リポジトリ地図、起動〜1フレームの追跡ルート、JSON→パーサ対応表、実装パターン(ソースへのジャンプリンク付き) |
| [第13章 編集 RPC リファレンス](13_editor.md) | 実行中のシーンを編集する RPC の契約(オーサリング文書・チケット・undo/redo・プレビュー・保存)、**グラフの装着を編集する** RPC、ImGui のインスペクタ、Python クライアント |
| [第14章 用語集](14_glossary.md) | 本マニュアルに出る用語と記号の索引(WP / D0 / codec / transaction / JSON-RPC / `name@1` / 共通グラフ / pose / instance / package など。基本語だけなら [第1章 §1.0](01_overview.md)) |
| [第15章 Studio の操作](15_studio.md) | Pelican Studio の panel ごとの操作(Inspector / Outliner / Materials / Assets / Graph / Frame Plan / Pass Editor / Render Features / Rendering settings / Ray layout / Layers / Problems / Play) |

> **共通グラフ(`pelican.graph`)に専用の章はありません。**値・描画・姿勢を 1 つの文書で繋ぐこの機構は 3 か所に分かれて書かれています —— **ノード語彙と入力の 4 mode は [第6章 §6.17「共通グラフのノード語彙」](06_rendering.md)**、**装着の編集操作(ノードキャンバスと Inspector)は [第13章 §13.5「グラフの装着を編集する」](13_editor.md)**、**語の定義は [第14章 §14.7「共通グラフと姿勢の語」](14_glossary.md)**(package / `requires` / component は §14.2)です。姿勢グラフの書き方は [第8章 §8.13](08_gameplay.md)、`graph.instance` の owner は [第4章 §4.1](04_scene_ecs.md)、文書がどの root に入るかは [第3章 §3.2](03_project_format.md) と [第6章 §6.16](06_rendering.md) にあります。

## 読み方の目安

- **初めての人**: 第1章 → 第2章 → 第3章 → 第4章 の順で、`pelican_cli project init` から動くものを作るのが最短です。
- **Studio を使う人**: 第1章 → 第2章 → 第15章(panel ごとの操作)。操作が送る RPC の細部が要るときは第13章の編集 RPC リファレンス。
- **ゲームを書く人**: 第7章・第8章。example の `playercontrol.cpp` が最小の完成形です。
- **絵作り・シェーダ**: 第6章。JSON だけでパイプラインが組めます。
- **ツール・自動化・CI**: 第10章。ヘッドレス + JSON-RPC が入口です。
- **「これって使えるの?」と思ったら**: 第11章の WP 台帳とバッジを確認してください。
- **エンジンのソースを読む人**: 第12章。どのファイルから読み始めるか・JSON がどこで読まれるかの地図です。
- **エディタ・自動化ツールを書く人**: 第13章。実行中のエンジンを外から編集する API の全体像です。
- **グラフでパラメータや姿勢を動かす人**: 第6章 §6.17(ノード語彙)→ 第13章(装着の編集)→ 第8章 §8.13(姿勢)。実物は [projects/posegraph_demo](../../projects/posegraph_demo)。
- **部品を配る・もらう人**: 第3章 §3.11(package)と §3.12(共有部品)。
- **語や記号が分からないとき**: 第14章の用語集(頻出の基本語だけなら [第1章 §1.0](01_overview.md))。

## 表記の約束

- ✅実装済み / 🚧実装中 / 📐設計のみ(未実装)
- `> **設計決定:** …` の引用ブロックは「なぜそうなっているか」の確定事項(出典は各設計文書)
- JSON 例はすべて実物(`projects/example/` 等)またはパーサ実装で検証済みの形
- ソース・設計文書へはリポジトリ内相対リンク(`../design_*.md`、`../../src/...`)

## 更新について

更新ルール（実装状況の表記、章の追加、食い違いの記録）は [第11章 §11.4](11_status.md) にあります。機能の変更時には該当する章を更新し、旧版の未実装一覧を現在の残件として再利用しないでください。

v0 の統合・再検証・リリース手順は [凍結一覧](../v0_freeze.md) §5・§6 を参照してください。ここに掲載した過去のテスト件数は、現在のビルドの成功を示すものではありません。
