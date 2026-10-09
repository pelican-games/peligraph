# 資料を HTML で読む・公開する

Markdown を編集すると、MkDocs と Material テーマで目次・日本語検索・コードの色分け付きの HTML を生成できます。
エンジン本体や Qt、Vulkan SDK のビルドは不要です。

## 手元で読む

[uv](https://docs.astral.sh/uv/getting-started/installation/) を用意し、リポジトリのルートで実行します。
Python と資料用ライブラリはスクリプトの宣言に従って uv が用意します。

```sh
uv run tools/docs_site.py serve
```

表示された `http://127.0.0.1:8000/peligraph/` をブラウザで開きます。終了は Ctrl+C です。
各ページの h2 / h3 の節は閉じた状態で表示され、見出しをクリックすると開閉します。h3 の三角は親の h2 の内側に下げて置き、開いた節の左に引いた縦線で階層を示します。ページ上部の「すべて開く」「すべて閉じる」で全節をまとめて開閉でき、目次や検索から飛ぶと飛び先の節が開きます。
HTML ファイルだけ生成する場合は次を使います。

```sh
uv run tools/docs_site.py build
```

出力先は `build-docs/site/` です。HTML 自体は直接開けますが、検索は HTTP サーバー経由での閲覧を使ってください。
図の描画には Material テーマが CDN から Mermaid を読み込むため、ネット接続が必要です。

## 収録する資料

公開対象は [mkdocs.yml](../mkdocs.yml) の `nav` に明記したページ、表示用 CSS、節を折りたたむスクリプト(`docs/site/sections.js`)、テーマと検索に必要な資源だけです。
利用マニュアル、ソースコード読解ガイド、トップページ、本ページを収録します。
`docs/` に新しいファイルを置くだけでは公開対象になりません。追加する場合は内容を確認して `nav` に登録します。

ソースコード・設計書・開発履歴は HTML にコピーせず、生成時のコミットを指す GitHub リンクに変換します。
非公開リポジトリへのリンクは、公開サイトから開く場合もリポジトリの閲覧権限が必要です。
マニュアル本文に含まれるコード例と仕様説明は HTML に含まれます。

## GitHub Pages で公開する

GitHub Pages は静的 HTML の公開機能です。独自のサーバーを運用する必要はありません。
公開リポジトリでは Free、非公開リポジトリからの公開には対応する有料プランが必要です。
組織の非公開リポジトリでは Team または Enterprise の条件を確認してください。
[GitHub Pages の公式説明](https://docs.github.com/en/pages/getting-started-with-github-pages/what-is-github-pages)

**非公開リポジトリから作っても、Pages のサイトまで自動的に非公開になるわけではありません。**
関係者限定の Pages は、GitHub Enterprise Cloud の組織向けアクセス制御の対象条件を確認してください。
[Pages の公開範囲](https://docs.github.com/en/enterprise-cloud@latest/pages/getting-started-with-github-pages/changing-the-visibility-of-your-github-pages-site)

公開する場合の設定は次の順です。

1. 修正した資料と生成設定を `develop` に反映します。
2. リポジトリの **Settings → Pages → Build and deployment → Source** で **GitHub Actions** を選びます。
3. **Settings → Secrets and variables → Actions → Variables** に、リポジトリ変数 `PUBLISH_DOCS` を値 `true` で作成します。
4. **Actions → Documentation site → Run workflow** を `develop` で実行します。
5. `deploy` ジョブの URL を開きます。その後は `develop` の資料更新に合わせて再公開します。

`PUBLISH_DOCS` を設定するまでは HTML のビルドと成果物の保存だけを行います。
プルリクエストでもビルドを確認しますが、公開処理は実行しません。
この変数は新規デプロイの有効・無効を切り替えるもので、既に公開したサイトを取り下げる設定ではありません。
[GitHub Actions による Pages 公開](https://docs.github.com/en/pages/getting-started-with-github-pages/using-custom-workflows-with-github-pages)

組織の契約上、非公開リポジトリから Pages を使えない場合は、確認済みの HTML だけを別の公開リポジトリに置く方法もあります。
元のソースコードリポジトリの公開範囲を変える必要はありません。

## 保守時の確認

```sh
uv run tools/doclink.py check
uv run tools/doclink.py audit
uv run tools/docs_site.py build
```

`doclink` はソース参照を検査し、HTML ビルドは資料サイトの生成を確認します。
これらは本文と実装の意味が一致することまで証明するものではないため、仕様を変更したときは対応する説明も読み直してください。
