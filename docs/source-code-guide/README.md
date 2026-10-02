# Pelican2 ソースコード読解ガイド

調査時点: 2026-09-04

対象ブランチ: `codex/render-target-runtime-slice`

基準コミット: `8864050`

上記は初回調査の基準です。その後の追記は各章で扱っています。2026-10-01 に `develop` の `c6df4822` と照合し、用語、参照先、加速構造などの説明を訂正しました。全機能をこの日付で再検証したという意味ではありません。現在の開発状況は [v1 開発計画](../v1_development_plan.md)、v0 の公開仕様は [凍結一覧](../v0_freeze.md) を参照してください。

この文書群は、Pelican2を「利用する方法」ではなく、**ソースコードがどう分割され、起動後に何がどの順で動き、複雑な実装がなぜその形になっているか**を理解するための読解ガイドです。

既存の利用マニュアルや設計文書とは目的を分けるため、[`docs/manual/`](../manual/00_index.md) には追加せず、独立した `docs/source-code-guide/` にまとめています。既存文書を正として言い換えたものではなく、調査時点の実装をソースから逆引きした資料です。

## この解説書の読み方

この文書群が説明せずに使う前提知識は 3 つです — **C++20**(concept、`if constexpr`、fold 式、`consteval`、`<=>`)、**Vulkan の基礎**(instance / device / queue / swapchain / descriptor / pipeline がそれぞれ何であるか)、**ECS の一般概念**(entity と component を分けて持ち、system がその組み合わせを走査する、という考え方)。この 3 つ以外の一般知識は、本文の側に説明を添える方針です。

Pelican が「こう呼んでいる」基本語(scene、asset、feature、render target、frame graph、behavior など)は [`docs/manual/14_glossary.md`](../manual/14_glossary.md) が正で、この解説書はそこを再定義しません。一方、**仕様の細部や定番イディオムを知らなくて読めない**とき(Vulkan の特別値、glTF の `byteStride`、CRTP、SFINAE、トポロジカルソートなど)は [第10章 前提知識の補足](10_background_knowledge.md) を引いてください。

コードベース固有の難しさは各章の `🧩 難所` ブロックにまとめてあります(下の難所インデックス)。

設計上の「契約」「正本」「面」や lowering、IR、capability、digest などは [用語集 §14.8](../manual/14_glossary.md#148-設計コード解説で頻出する用語) で説明しています。

## このガイドの特徴

- 主要な説明には、実装へ直接ジャンプできる `ファイル#L行番号` リンクがあります。
- 公開API、エンジン内部モジュール、純粋なデータ変換層、Vulkan層を区別します。
- ECSは利用法だけでなく、世代付きID、アーキタイプ（archetype — 「同じComponentの組み合わせを持つentityの集まり」を1つの単位として扱うECSの分類）、SoAチャンク（SoA = structure of arrays。1 entity分をまとめた構造体を並べるのではなく、Componentごとに別々の連続配列へ分けて置く格納方式で、それを一定個数ずつ区切った箱がchunkです）、型消去（type erasure — 具体的な型ごとの情報をsize/alignmentと関数ポインタの表で表し、どんな型でも同じ経路で扱えるようにする手法）、ライフサイクル、変更検知、並列スケジューリングまで追います。
- マクロ、自動登録、`__COUNTER__`（展開されるたびに0, 1, 2…と増える整数へ置き換わるプリプロセッサ組み込みマクロ。展開ごとに重複しない番号を作るのに使います）、型消去コールバックなどの実装技法は、展開後に何が起きるかを段階的に説明します。
- 完成済みの機能と、骨組みだけの箇所・現在の制約を分けて記載します。

## 読む順番

| 章 | 内容 | 最初に読むべき人 |
|---|---|---|
| [第1章 全体構造とビルド](01_architecture_and_build.md) | ターゲット、ディレクトリ、依存方向、外部ライブラリ、ビルド機能フラグ | 全員 |
| [第2章 起動・モジュール・1フレーム](02_runtime_lifecycle.md) | `main()` から初期化、5フェーズ更新、通常/headless/RPC、終了処理 | 実行経路を掴みたい人 |
| [第3章 プロジェクトデータとロード](03_project_and_loading.md) | `project.json`、パス解決、scene、asset、純粋パーサと実行時バインド | データ形式を追加する人 |
| [第4章 ECS徹底解剖](04_ecs_deep_dive.md) | Entity、Component、Chunk、System、登録、更新、削除、ロールバック | ECSを理解・変更する人 |
| [第5章 ゲームAPIとサービス](05_gameplay_and_services.md) | GameContext、ゲームSystem、Event、入力、Camera、Physics、Audio、保存 | ゲームコードを書く人 |
| [第6章 描画・Vulkan・Shader](06_rendering_vulkan_shader.md) | Rendering config、Frame graph、Dynamic Rendering、Shader反射、hot reload | レンダラを変更する人 |
| [第7章 ツール・RPC・テスト](07_tools_rpc_tests.md) | devcli、Studio、JSON-RPC、テストの種類と読み方 | ツール/CIを触る人 |
| [第8章 クラス・インターフェース索引](08_class_interface_index.md) | 主要型を責務別に引ける宣言/実装/テスト索引 | 名前から探したい人 |
| [第9章 マクロ・型消去・変更時の注意](09_black_magic_and_gotchas.md) | マクロ展開、型消去、静的初期化、寿命、現在の未実装点 | 深い改修をする人 |
| [第10章 前提知識の補足](10_background_knowledge.md) | このコードが当然としている一般知識(Vulkan / glTF / C++ / OS / アルゴリズム / 座標と数値 / 宣言的グラフと拡張の作法) | コードは素直なのに読めないとき |
| [第11章 Ray query / RT pipeline 用加速構造](11_ray_query_acceleration_structures.md) | 静的・変形形状の BLAS / TLAS、AS 行の identity(`MaterialRowIdentity`)、2 種の shadow mask、SBT(shader binding table — RT pipeline が raygen / miss / hit の各 shader group handle を引くための GPU バッファ)、set 0 契約、raster shadow との差 | ray query / RT pipeline の土台や shadow feature を変更する人 |

## 難所インデックス(詰まったときの逆引き)

各章の `🧩 難所` ブロックでは、実装の目的、読み違えやすい点、処理の手順、変更時に守る条件を説明します。知りたい内容から章を選び、章内で `難所` を検索してください。

| 章 | 解説する主な実装上の注意点 |
|---|---|
| [第2章](02_runtime_lifecycle.md) | モジュールの生成・破棄順、実行モードごとの更新順、参照の取得に伴う状態変更 |
| [第3章](03_project_and_loading.md) | 保存中のファイル変更、親子付け替え時の座標変換、シーン切替の巻き戻し、アニメーションの骨格間変換 |
| [第4章](04_ecs_deep_dive.md) | Entity の生成と削除、読み書きの競合、世代番号の再利用制限、コンポーネント移行時の要素移動 |
| [第5章](05_gameplay_and_services.md) | 物理クエリの順序と接触判定、姿勢のブレンドと反映時点、パラメータの検証と一括交換 |
| [第6章](06_rendering_vulkan_shader.md) | 描画パスの挿入順、リソースへの読み書きの依存、GPU 同期、シェーダの結合、ホットリロード、履歴のリセット |
| [第7章](07_tools_rpc_tests.md) | 編集前の検証、undo の前提条件、プレビュー資源の寿命と元の状態の保持 |
| [第9章](09_black_magic_and_gotchas.md) | マクロと型消去、ABI の互換性、JSON を編集する位置の特定、姿勢の移動量抽出、参照先を解決できない場合の扱い |

一般的でない専門用語(Floyd–Warshall・SFINAE・ADL・TOI・MTD・GJK/EPA・CAS・TOCTOU・ABA・Halton 列など)は、**その語が実際に出てくる章の初出箇所で 1〜2 文の説明を添えてあります**(この索引には用語表を置きません)。どの章に出るかは語ごとに違う — たとえば Floyd–Warshall は frame graph の推移閉包を扱う[第6章](06_rendering_vulkan_shader.md)、SFINAE と ADL は登録マクロを扱う[第9章](09_black_magic_and_gotchas.md)です — ので、横断して引くときは `rg '<語>' docs/source-code-guide` が最短です。Pelican 固有の基本語は [`docs/manual/14_glossary.md`](../manual/14_glossary.md)、一般知識の側は[第10章](10_background_knowledge.md)にまとまっています。

難所ブロックが「このコードベース固有の難しさ」を扱うのに対して、**コード自体は素直なのに仕様や定番イディオムを知らなくて読めない**ときは [第10章 前提知識の補足](10_background_knowledge.md) を引いてください(Vulkan・glTF・C++・OS・アルゴリズム・座標と数値・OpenXR・宣言的グラフと拡張の作法の 149 項目。章の冒頭に分類ごとの索引があります)。

## 最短の読解ルート

エンジン全体を最短で追うなら、次のリンクを順に開いてください。

1. [`main()`](../../src/player/main.cpp#L703) — CLIで起動条件を確定する。
2. [`PelicanCore::run()`](../../src/core/userpublic/pelican_core.cpp#L99) — 設定、ECS、scene、loopを組み立てる。
3. [`Loop::run()`](../../src/core/appflow/loop.cpp#L772) — 通常/XR/headless/RPCの実行方式を分ける（windowed + RPCを含む5経路）。
4. [`updateFrameState()`](../../src/core/appflow/framephase.cpp#L152) — 1フレームのゲーム状態更新を5フェーズで実行する。
5. [`ECSCoreTemplatePublic::update()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L663) — 内部ECS Systemを依存順に実行する（実行計画は [`buildECSExecutionPlan()`](../../src/core/userpublic/details/ecs/coretemplate.cpp#L206) が作る）。
6. [描画処理（複数のビュー群を受け取る実装）](../../src/core/vkcore/renderer.cpp#L6861) — 描画計画を GPU コマンドへ変換します。[単一ビュー群の呼び出し口](../../src/core/vkcore/renderer.cpp#L5258) はこの実装へ転送します。[通常画面の描画](../../src/core/vkcore/renderer.cpp#L6893) は計画の選択と再変換の再試行を担当し、[引数なしの描画関数](../../src/core/vkcore/renderer.cpp#L6936) はカメラから 1 つのビューを組み立てます。
7. [`RuntimeTeardownGuard::run()`](../../src/core/appflow/teardown.cpp#L149) — 例外時もGPU/ECS/queue資源を規範順で解放する（実体は [`teardownRuntimeNoThrow()`](../../src/core/appflow/teardown.cpp#L122)）。

## リンクの見方

- `型名 → file.hpp#L10` の形式のリンクは宣言へ飛びます。
- `型名::関数 → file.cpp#L100` の形式のリンクは主要実装へ飛びます。
- 「仕様として読む」リンクは対応する [`test/`](../../test) のテストへ飛びます。

行番号はリファクタで必ずずれます。ずれたリンクは「無いリンク」より悪い(黙って無関係な
コードを指すため)ので、[`tools/doclink.py`](../../tools/doclink.py) が追従させます。

```bash
uv run tools/doclink.py check
```

インタプリタは uv がスクリプト冒頭の宣言から自前で用意するので、システムに Python を
入れる必要はありません。依存も無いので素の `python tools/doclink.py` でも動きます。

`check` は現状を報告するだけ、`update` が行番号を書き直します。追従の鍵は行番号ではなく
**アンカー**で、二通りあります。

- **シンボル** — リンク文字列が `` `Renderer::render()` `` のような識別子なら、対象ファイル中の
  **定義**を探し直します。`DECLARE_MODULE(Foo)` や `class PELICAN_API Foo` といった
  このリポジトリ固有の宣言の形も知っています。
- **台帳** — リンク文字列が識別子でないとき(ファイル名、日本語、`#L123` 自体)は、
  [`docs/link_anchors.json`](../link_anchors.json) が**その行の本文を憶えて**おり、
  次回はその本文を探し直します。

どちらでも決められないときは書き換えず、名指しで報告して終わります。候補が等距離に
2 つあるようなときに勘で選ばないための設計です。

### 追従では拾えない 2 種

`check` が見るのは「リンクがアンカーの上に居るか」だけです。アンカーを離れないまま
参照が嘘になる道が 2 つあり、そちらは `audit` が見ます。

```bash
uv run tools/doclink.py audit
```

- **最初から違う行にアンカーを取ってしまったもの** — 追従は健全に働き続けるのに、
  着地点がずっと間違っています。空行や閉じ括弧を指しているもの、ラベルが名指しした
  識別子が対象の前後 3 行に無いものを報告します。
- **消えた関数を説明している本文** — リンクが正しいファイルの正しい行を指していても、
  説明している関数が改名・削除されていれば無意味です。`` `foo()` `` 形式の記述を
  ソース全体の識別子と突き合わせます。

**第10章は 2 段構えです。** 先発の 73 項目は markdown リンクではなく
`` `symbol()`(175 行) `` の形で 227 箇所を引用しており、9 割はファイルを文脈に依存します。
全部リンクにすると解説が読めなくなるため、追従の代わりに章の冒頭で基準コミットを宣言する
方を採っています。2026-09-04 に足した 70 項目は「使用場所」の行にリンクをまとめる書式で、
こちらは `doclink.py` の追従対象です。`audit` の識別子検査は書式に依存しないので、**消えた関数の説明は
第10章でも検出されます**。

どちらも**人が判断して直すもの**なので、`update` のような自動修正はありません。
外部 API や「かつて存在した」という意図的な言及は
[`docs/doc_audit_allowlist.txt`](../doc_audit_allowlist.txt) に理由付きで登録します。
**登録した名前がソースへ復活すると audit が落ちます** — 免除が腐って溜まらないようにするためです。

commit 時に自動で走らせるには:

```bash
git config core.hooksPath tools/hooks
```

フックは**ステージ済みで、かつ未ステージの変更を持たない**文書だけを書き換えます
(`git add` が無関係な編集を巻き込まないため)。`--no-verify` で迂回できます。

## 一枚で見た全体像

```mermaid
flowchart LR
    Project["project.json / scene / assets / passes"]
    Player["pelican_player\nCLI・起動"]
    Core["pelican_core\n実行時モジュール"]
    Pure["pelican_project\n純粋パース・検証"]
    Game["project/code\nゲームSystem\npelican_game_logic (DLL)"]
    ECS["ECS\nEntity・Chunk・System"]
    Render["Rendering\nFrame graph・Shader"]
    XR["OpenXR\nsession・composition"]
    VK["Vulkan\nDevice・FrameTarget・GPU資源"]
    Tools["pelican_cli / Studio / tests"]

    Project --> Player
    Project --> Pure
    Player --> Core
    Game -->|DLLとしてロード| Player
    Core --> Pure
    Core --> ECS
    Core --> Render
    Core --> XR
    XR --> VK
    Render --> VK
    Tools --> Pure
    Tools --> Core
```

大切なのは、`src/project` がGPUやモジュールコンテナに依存しない**形式ロジック**、`src/core` がそれを実行時オブジェクトへ結び付ける**エンジン本体**、`src/player` がプロセスの入口、という分担です。
