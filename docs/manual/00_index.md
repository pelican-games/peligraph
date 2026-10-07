# Pelican2 マニュアル

最終確認: 2026-10-07(develop d975ee9c0)

> 各章の冒頭「最終確認:」行に確認日とリビジョンを記載します。本文に残る過去の観測や未検証の範囲は、その節の注記を確認してください。
> 現在の開発状況は [v1 開発計画](../v1_development_plan.md)、v0 の公開仕様は [凍結一覧](../v0_freeze.md) を参照してください。2026-10-01 に `develop` の `c6df4822` と照合し、用語と一部の仕様説明を訂正しています。
> WP の仕様・完了記録と過去の転記は [第11章 §11.1 の索引](11_status.md#111-wp-の台帳はどこにあるか) から辿れます。現在の利用方法と有効な制限は各章で確認してください。
>
> **WP**(Work Package)= 開発の作業単位です。台帳 [../implementation_plan.md](../implementation_plan.md) に 1 行として登録され、**1 WP = 1 ブランチ = 1 マージ**で進みます。**番号は 2 系統あります** —— WP386 までの**通し番号**(`WP380a` のような枝番付き)と、v0 の段に使われた**記号 ID**(`AG-0` / `AG-1b` / `P-2a-1c` / `X-1` / `R1-2` / `N-2` …。段の系統 + 番号)です。本文のバッジも同じ規則で `✅WP382a` / `✅P-2a-3` のように書きます。台帳・履歴の参照先は [第11章 §11.1](11_status.md#111-wp-の台帳はどこにあるか)(v0 の段も索引から辿れます)、そのほかの記号と基本語は [第1章 §1.0](01_overview.md) と [第14章 用語集](14_glossary.md#wpwork-package) にまとめてあります。
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
| [第11章 実装状況と文書マップ](11_status.md) | WP 台帳・履歴への索引、設計文書⇔章の対応表、既知の食い違い一覧、本マニュアルの更新ルール |
| [第12章 コード読解ガイド](12_codemap.md) | データ → parser の早見表と、起動〜1フレームの追跡ルート。リポジトリ地図・型の詳細は所有節へのリンクから読む |
| [第13章 編集 RPC リファレンス](13_editor.md) | 実行中のシーンを編集する RPC の契約(オーサリング文書・チケット・undo/redo・プレビュー・保存)、**グラフの装着を編集する** RPC、ImGui のインスペクタ、Python クライアント |
| [第14章 用語集](14_glossary.md#この用語集の使い方) | 本マニュアルに出る用語と記号の索引(WP / D0 / codec / transaction / JSON-RPC / `name@1` / 共通グラフ / pose / instance / package など。基本語だけなら [第1章 §1.0](01_overview.md)) |
| [第15章 Studio の操作](15_studio.md) | Pelican Studio の panel ごとの操作(Inspector / Outliner / Materials / Assets / Graph / Frame Plan / Pass Editor / Render Features / Rendering settings / Ray layout / Layers / Problems / Play) |

> **共通グラフ(`pelican.graph`)に専用の章はありません。**値・描画・姿勢を 1 つの文書で繋ぐこの機構は 8 か所に分かれて書かれています —— **ノード語彙と入力の 4 mode は [第6章 §6.17「共通グラフのノード語彙」](06_rendering.md)**、**装着の編集操作(ノードキャンバスと Inspector)は [第13章 §13.5「グラフの装着を編集する」](13_editor.md)**、**語の定義は [第14章 §14.7「共通グラフと姿勢の語」](14_glossary.md#共通グラフpelicangraph-v1)**(package / `requires` / component は §14.2)です。姿勢グラフの書き方は [第8章 §8.13](08_gameplay.md)、`graph.instance` の owner は [第4章 §4.1](04_scene_ecs.md)、文書がどの root に入るかは [第3章 §3.2](03_project_format.md) と [第6章 §6.16](06_rendering.md) にあります。

## 読み方の目安

- **初めての人**: [第1章](01_overview.md) → [第2章](02_getting_started.md) → [第3章 §3.1〜§3.4](03_project_format.md#31-プロジェクトとは) → [第4章](04_scene_ecs.md)。`pelican_cli project init` で動くものを作り、プロジェクトの基本形・パス・アセットの置き場所を確認します。第3章の後半(宣言スキーマ・package・共有部品・移行など)は必要になったら読みます。
- **Studio を使う人**: [第1章](01_overview.md) → [第2章](02_getting_started.md) → [第15章](15_studio.md)(panel ごとの操作)。概念・操作・RPC の対応は[通し表](#studio-operation-map)、RPC の契約は[第13章](13_editor.md)で調べます。
- **ゲームロジックを書く人**: [第7章](07_input_ui.md) → [第8章](08_gameplay.md)。example の `playercontrol.cpp` が最小の完成形です。
- **絵作り・シェーダ**: 第6章。JSON だけでパイプラインが組めます。
- **ツール・自動化・CI**: 第10章。ヘッドレス + JSON-RPC が入口です。
- **「これって使えるの?」と思ったら**: 各機能の章の説明とバッジを確認してください。作業の仕様・完了記録は [第11章 §11.1 の索引](11_status.md#111-wp-の台帳はどこにあるか) から辿れます。
- **コードを読む人**: [読解ガイド README](../source-code-guide/README.md) の全体図と読む順番から始めます。データ → parser の早見表は[第12章](12_codemap.md)、クラス → 責務と内部の呼び出し連鎖は読解ガイドで調べます。
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

<a id="studio-operation-map"></a>

## 概念 → 操作 → Studio の場所 → RPC → 履歴の行き先 → 反映の状態

既存の所有節と操作説明を結ぶ通し表です。RPC の引数・戻り値・拒否条件はリンク先を参照してください。履歴の行き先は[第15章の7対象の地図](15_studio.md#undo-map)に合わせています。RT records は共有の描画履歴に入り、文字入力中は Qt の文字の Undo が優先されます。空欄は既存文書から確定できない欄です。

| 概念 | 操作 | Studio の場所 | RPC | 履歴の行き先 | 反映の状態 |
|---|---|---|---|---|---|
| [object・シーン](04_scene_ecs.md#41-シーンファイルの全体構造) | [object を置く](15_studio.md#153-outliner) | Outliner / Scene toolbar | [edit (`spawn`)](13_editor.md#編集操作operationsの-13-種) | Scene | [フレーム境界で適用](13_editor.md#133-編集の実行モデル--チケットとフレーム境界) |
| [component](04_scene_ecs.md#42-コンポーネント一覧シーンに書けるものすべて) | [component を足す](15_studio.md#152-inspector) | Inspector / Add component... | [edit (`add_component`)](13_editor.md#編集操作operationsの-13-種) | Scene | [フレーム境界で適用・Save で保存](13_editor.md#133-編集の実行モデル--チケットとフレーム境界) |
| [material](06_rendering.md#67-マテリアルm1m35--wp586870767883) | [material を作る](15_studio.md#154-materials) | Materials / New material… | [create_material](13_editor.md#material-文書を読み書きする) |  | [live / 再起動待ち / unusable](13_editor.md#material-文書を読み書きする) |
| [material の値・surface](06_rendering.md#67-マテリアルm1m35--wp586870767883) | [material を編集する](15_studio.md#154-materials) | Materials | [edit_material](13_editor.md#material-文書を読み書きする) | Materials | [live / rebuilt / 再起動待ち / unusable](13_editor.md#material-文書を読み書きする) |
| [object ごとの material](06_rendering.md#67-マテリアルm1m35--wp586870767883) | [選んだ object に割り当てる](15_studio.md#154-materials) | Inspector / Material / This object | [set_object_material](13_editor.md#material-文書を読み書きする) | Scene | [フレーム境界で適用](13_editor.md#material-文書を読み書きする) |
| [model 全体の material](06_rendering.md#67-マテリアルm1m35--wp586870767883) | [同じ model 全体に割り当てる](15_studio.md#154-materials) | Inspector / Material / Every model | [bind_material](13_editor.md#material-文書を読み書きする) | Inspector の Material 行 | [live / 再起動待ち / unusable](13_editor.md#material-文書を読み書きする) |
| [pass](06_rendering.md#62-rendering-config-のスキーマ) | [pass を足す](15_studio.md#158-frame-plan) | Frame Plan / Add pass…・Add material pass…（graph root は Graph palette） | [add_authored_pass / edit_graph_document](13_editor.md#レンダー設定を編集する) | 描画（graph root の node は Graph） | [フレーム境界で世代公開 / 契約変更は再起動待ち](13_editor.md#render-edit-sequence) |
| [render target](06_rendering.md#render_targets) | [render target を足す](15_studio.md#158-frame-plan) | Frame Plan / New render target…（Graph からも同じ form） | [edit_render_targets](13_editor.md#レンダー設定を編集する) | 描画 | [フレーム境界で世代公開 / 契約変更は再起動待ち](13_editor.md#render-edit-sequence) |
| [共通グラフの node](06_rendering.md#617-共通グラフのノード語彙) | [node を置く](15_studio.md#156-graph-タブ) | Graph / Add node…・palette | [edit_graph_document (`add_node`)](13_editor.md#グラフの装着を編集する) | Graph | [フレーム境界で世代公開 / 契約変更は再起動待ち](13_editor.md#render-edit-sequence) |
| [共通グラフの接続](06_rendering.md#617-共通グラフのノード語彙) | [node を繋ぐ](15_studio.md#156-graph-タブ) | Graph / sockets | [edit_graph_document (`set_input`)](13_editor.md#graph-edit-scope) | Graph | [フレーム境界で世代公開 / 契約変更は再起動待ち](13_editor.md#render-edit-sequence) |
| [fold group](13_editor.md#レンダー設定を編集する) | [fold / 検査する](15_studio.md#156-graph-タブ) | Graph / fold group | [fold_nodes / verify_fold](13_editor.md#レンダー設定を編集する) | 描画（Graph タブの送信順で選択） | [宣言直後は unverified・検査成功で verified を保存](13_editor.md#レンダー設定を編集する) |
| [frame_node_order](06_rendering.md#pass-の順を指定する--frame_node_order) | [Frame Plan の順序を変える](15_studio.md#158-frame-plan) | Frame Plan / Order… | [edit_frame_node_order](13_editor.md#レンダー設定を編集する) | 描画 | [フレーム境界で世代公開 / 契約変更は再起動待ち](13_editor.md#render-edit-sequence) |
| [ray layout・bindings](06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v3-と-pelican_ray_instancert3-1--wp374374a377379383) | [ray layout を編集する](15_studio.md#1512-ray-layout) | Rendering settings / Ray layout | [edit_ray_layout / edit_batch](13_editor.md#レンダー設定を編集する) | 描画 | [commit 時に組み直し / layout mode 変更は再起動待ち](13_editor.md#render-edit-sequence) |
| [procedural records](06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v3-と-pelican_ray_instancert3-1--wp374374a377379383) | [RT records を編集する](15_studio.md#1512-ray-layout) | Ray layout / records | [edit_ray_layout (`procedural_records`)](13_editor.md#レンダー設定を編集する) | RT records（共有の描画履歴） | [commit 時に runtime を組み直す](13_editor.md#レンダー設定を編集する) |
| [project の監視・store 宣言](03_project_format.md#34-asset-store--置き場所の間接化実装済みwp55) | [project 設定を変える](15_studio.md#155-assets) | Assets / Reload when saved outside・Track asset contents… | [edit_project_settings](13_editor.md#projectjson-の宣言を書き換える) | Assets | [その場で反映 / 反映失敗や module 不在は再起動待ち](13_editor.md#projectjson-の宣言を書き換える) |
| [Play セッション](13_editor.md#再生のセッション) | [Play / Stop](15_studio.md#1517-play) | Play toolbar | [set_play_state](13_editor.md#再生のセッション) |  | [edit / playing / paused・Stop で開始前の scene と履歴を復元](13_editor.md#再生のセッション) |
| [シーンの保存](03_project_format.md#39-シーンドキュメントと保存エディタ経路wp149158166168) | [保存する](15_studio.md#151-project-を作る開く保存する) | Save / 未保存変更の Save・Discard・Cancel | [save_scene](13_editor.md#save_scene) |  | [保存のみ・シーンは hot reload 対象外](13_editor.md#save_scene) |
| [可逆な編集・同時編集](13_editor.md#134-同時編集--actor-と-revision) | [Undo / Redo](15_studio.md#1514-履歴と-undo) | focus のある panel / Undo ボタン・Edit menu | [undo / redo](13_editor.md#セッションと編集)、[restore_render_documents](13_editor.md#render-文書を丸ごと読む戻す--get_render_documents--restore_render_documents)、[graph の逆操作](13_editor.md#グラフの装着を編集する)、[material の逆操作](13_editor.md#material-文書を読み書きする)、[project の逆操作](13_editor.md#projectjson-の宣言を書き換える) | Scene / Graph / 描画 / Materials / Inspector の Material 行 / Assets / RT records（focus・対象で分岐） | [元の編集の反映経路で戻す・再起動が要る描画編集は戻すと再起動](15_studio.md#1514-履歴と-undo) |

material の新規作成、Play、保存を単独で行ったときの履歴の行き先は空欄です。Play の Stop による履歴の復元や Keep の Undo 項目は[第15章 §15.17](15_studio.md#1517-play)、Undo の選択と外部再読込の区切りは[§15.14](15_studio.md#undo-map)を参照してください。
