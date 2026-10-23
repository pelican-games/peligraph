# Pelican2 ドキュメント

Pelican2 は、描画処理をノードで組み立てながらゲームを制作するためのエンジンです。
このサイトでは、利用方法とソースコードの構造を日本語で説明します。

## 目的から読む

| やりたいこと | 読み始める場所 |
|---|---|
| ビルドして動かす | [ビルドと起動](manual/02_getting_started.md) |
| 初めて使う | [第1章](manual/01_overview.md) → [第2章](manual/02_getting_started.md) → [第3章 §3.1〜§3.4](manual/03_project_format.md#31-プロジェクトとは) → [第4章](manual/04_scene_ecs.md)。第3章の後半は必要になったら([読み方](manual/00_index.md#読み方の目安)) |
| 描画やシェーダを組む | [レンダリング](manual/06_rendering.md) |
| Studio を使う | [第1章](manual/01_overview.md) → [第2章](manual/02_getting_started.md) → [第15章 Studio の操作](manual/15_studio.md)。[通し表](manual/00_index.md#studio-operation-map)で RPC・履歴・反映を調べる |
| ゲームロジックを書く | [第7章 入力と UI](manual/07_input_ui.md) → [第8章 ゲームロジック](manual/08_gameplay.md) |
| コードを読む | [読解ガイド README](source-code-guide/README.md)の全体図と読む順番。データ → parser の早見表は[第12章](manual/12_codemap.md) |
| 用語・略語を調べる | [用語集](manual/14_glossary.md) |

画面上部から日本語でも検索できます。各ページの目次から節へ移動でき、コード例はコピーできます。

## 資料の対象と参照先

各章の冒頭「最終確認:」行に確認日を記載しています。
本文に残る過去の観測や未検証の範囲は、その節の注記を確認してください。
実装済み・未実装の区別と各節の対象条件を確認してください。

このサイトに収録しているのは利用マニュアルとソースコード読解ガイドです。
ソースコード、設計書、作業履歴へのリンクは GitHub の該当リビジョンを開きます。
リポジトリが非公開の場合、これらのリンク先の閲覧にはリポジトリへのアクセス権が必要です。

生成・公開の手順は [HTML の生成と GitHub Pages](documentation_site.md) にあります。
