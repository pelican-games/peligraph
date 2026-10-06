# 第14章 用語集

最終確認: 2026-10-07(develop d975ee9c0)

用語・仕様説明の再照合: 2026-10-01（`develop` / `c6df4822`）。主な訂正内容は [確認記録](../documentation_review_2026-10-01.md) にあります。

## この用語集の使い方

- **複数の章で説明なしに出てくる語だけ**を集めた索引です。正確な仕様は各語の「定義」リンク先にあり、ここはその入口です。
- 設計文書に多い英語・略語は [§14.8](#148-設計コード解説で頻出する用語) にまとめています。
- 五十音順ではなく**分野別**に並んでいます。語を探すときはブラウザの検索(Ctrl+F)を使ってください。
- 実装への行リンクは定義している節にあります。**説明と実装が食い違ったら実装が正**です(食い違いの台帳は [第11章 §11.3](11_status.md#113-既知の食い違い設計文書-vs-コード))。

---

## 14.1 全体・体制・品質の語

### WP(Work Package)

実装作業の単位。番号は通し番号と記号 ID の 2 系統です。 定義: [第11章 §11.1][def-0]。

### 台帳

仕様・作業・検証結果を一覧で管理する文書。現在の状態と過去の履歴を区別して参照します。 定義: [第11章 §11.1][def-1]。

### D0

エディタに特権 API を作らず、Studio も公開 JSON-RPC を呼ぶという設計決定。 定義: [第13章 §13.1][def-2]。

### ABI(Application Binary Interface)

ゲーム DLL・physics provider・material output など、バイナリをまたぐ境界ごとの呼び出し規約。 定義: [第8章 ゲーム DLL の ABI][def-3]。

### 決定的 / 決定性(deterministic)

同じ条件で処理順序や結果を再現できる性質。GPU・ドライバをまたぐビット一致の保証ではありません。 定義: [第8章 §8.10][def-4]。

### hard error

警告で済ませず**起動・ロードをその場で止める**エラー。スキーマ不一致・バージョン不一致・未知の名前・必須フィールド欠落がこれにあたります。「読めるふりをして一部だけ壊れる」事故を防ぐための方針で、メッセージには必ず手がかり(解決後の絶対パス・登録済み id 一覧・OFF になっているビルドフラグ名)が入ります。 定義: [第1章 §1.5][def-5]。

### `<name>@<version>` 形式のエラーコード

名前と `@` と契約の版でエラーを識別する書き方。版はエンジンやスキーマの版とは別です。 定義: [第4章 §4.5][def-6]。

### 名前入りの拒否(named reject)

拒否するときは必ず上の形式のコードを添える、という運用のこと。対義は「黙って素通しする」で、pelican2 は素通しを避ける側に倒しています。ただし web プロファイルだけは未知コンポーネントを `opaque` として素通しします(後述)。 定義: [第1章 §1.5][def-7]。

### パージ可能(purgeable)

参照を書いたときだけ機構が存在する性質。feature や UI の参照を省くと機構ごと素通りします。 定義: [第6章 §6.5][def-8]。

### サブセット原則

「**web で開けるもの ⊆ pelican で開けるもの**」という不変条件。形式の拡張は常にエンジンが先行し、web が追随します。web が読めてエンジンが読めない、は違反です。 定義: [第9章 §9.2][def-9]・[第3章 §3.7][def-9-1]。

### 二層モデル

アセットをランタイム層とソース層に分け、エンジンはランタイム層だけを読むという原則。 定義: [第5章 §5.1][def-10]。

### golden

ヘッドレス描画の出力を固定した正解画像と比較する回帰試験。既定は許容差 0 です。 定義: [第6章 §6.10][def-11]。

### fixture

テストが使う固定の入力データ(または、それを生成するコード)。`test/fixtures/` 以下に置かれます。うち `test/fixtures/project_format/` は web プロファイルと共有され、同じ入力に対する合否の一致を web 側の実行で検証します(§9.2 の「fixture 共有」)。 定義: [第10章 §10.9][def-12]。

---

## 14.2 形式(JSON)とプロジェクトの語

### エンベロープ

JSON の `schema` と `version` の 2 キー。形式名と版を本体より先に検査します。 定義: [第4章 エンベロープの規則][def-13]。

### closed schema / open schema

組み込みコンポーネントは未知キーを拒否する closed schema。codec の無いユーザー定義型は扱いが異なります。 定義: [第12章 §12.5][def-14]。

### opaque(web)

web で codec の無いコンポーネントを検証せず通す扱い。web 側の実装はこのツリーでは未確認です。 定義: [第3章 §3.8][def-15]。

### 宣言 schema(v2 / v3)

behavior やユーザー定義コンポーネントの params を型付きで宣言する仕組みの版。v3 は再帰的な入れ子に対応します。 定義: [第3章 §3.10][def-16]。

### `Flags`

宣言済みの名前の集合を文字列配列で書くフィールド型。上書きは OR ではなく配列の置換です。 定義: [第3章 §3.10][def-17]。

### `List<Row>` / 行封筒

行の版や由来を持てる可変長フィールド型。JSON は素の配列でなく `$list` と `rows` の object です。 定義: [第3章 §3.10][def-18]。

### wire

「ディスク / RPC に出る形」のこと。同じ値でも、C++ の構造体(runtime)・正規化した中間形(canonical)・JSON に出る形(wire)は別物で、codec がこの 3 者を往復させます。「wire モデル」は「JSON に出る形の設計」の意味です。 定義: [第3章 §3.10][def-19]。

### 解決(resolve)

パスの resolve は参照から使う対象を確定する操作。定義: [第3章 §3.3][def-20]。

描画計画の resolve は役割から実 format などを確定する操作。定義: [第6章 §6.3][def-resolve-render]。

### 結合(binding)

runtime binding は解析した実体を実行時オブジェクトへ繋ぐ操作。定義: [第3章 §3.5][def-21]。

入力の binding はデバイス操作とアクション名の対応。定義: [第7章 §7.2][def-binding-input]。

### asset store

論理マウントとマシンごとの実パスを分け、アセットの置き場所を間接化する仕組み。 定義: [第3章 §3.4][def-22]。

### フラグメント参照(`#`)

`models/town.glb#Building_02` のように、**コンテナ内の 1 要素**を指す参照。解決の過程でフラグメントを落とさずに運ぶのが規約です。 定義: [第5章 §5.6][def-23]。

### authoring / runtime

authoring は編集の正本、runtime は実行する ECS 側の姿。編集結果を runtime へ投影します。 定義: [第3章 §3.9][def-24]。

### 3 段合成

`basic_config` の値を「エンジン既定 → `project.json` → `.pelican/local.json`」の 3 段で重ねて決めること。deep merge ではなく**パス単位で問い合わせる**方式なので、どの段が勝ったかを後から言えます。 定義: [第3章 basic_config と「3 段合成」][def-25]。

### `requires`(3 つの要求)

project.json は必要なビルド機能名の配列です。定義: [第3章 §3.2][def-26a]。

package は engine・SDK・capability の要求です。定義: [第3章 §3.11][def-26b]。

feature の integrations[].requires は他の feature 名です。定義: [第6章 §6.5][def-26c]。

### component(3 つの別の語)

ECS の component はシーンのコンポーネント 1 レコード。定義: [第4章 §4.2][def-27a]。

render component は feature 1 本を持ち出す共有部品。定義: [第3章 §3.12][def-27b]。

package の provides.components は共有部品ディレクトリのパス配列。定義: [第3章 §3.11][def-27c]。

### package(`pelican.package` v1)

`package.json` を持ち、feature・グラフ・共有部品・コードをまとめて配るディレクトリ。 定義: [第3章 §3.11][def-28]。

### vendoring(ベンダリング)

外の部品を参照でなく複製としてプロジェクトへ取り込む操作。文書の名前にも namespace を付けます。 定義: [第3章 §3.11][def-29]。

### section locator(`rendering` 節)

共通グラフの root と `rendering` 節のキーの置き場所を決める仕組み。読み取りと編集が同じ規則に従います。 定義: [第6章 §6.16][def-30]。

---

## 14.3 ロードと ECS の語

### scene load transaction(シーンロードのトランザクション)

新シーンを準備して切り替える 8 段の適用単位。publish 後の activate でも失敗でき、commit 前なら巻き戻します。 定義: [第4章 §4.3][def-31]。

### prepare / publish / commit

prepare は準備、publish は反映、commit は確定。scene load では publish は途中の段です。 定義: [第4章 §4.3][def-32]。

### retire(退役)

旧シーンを片付ける段。旧 entity は既に unpublish 済みで、旧オブジェクトの `onDestroy` が走ります。 定義: [第4章 §4.3][def-33]。

### staging(ステージング)

ロード中の作用を実行せず、実行可能な entry として溜める操作。commit 成功後の flush で実行します。 定義: [第4章 ロード中の「作用」][def-34]。

### outbox

溜めた作用を入れる固定長のバッファ。容量は 256 で、溢れた分は「溢れた」という 1 件のエラーに畳まれます(黙って捨てません)。 定義: [第4章 ロード中の「作用」][def-35]。

### flush

溜めた作用を**実際に実行する**段(8 段の 7 番目)。ここで初めて音が鳴り、セーブが書かれます。commit 済みの後なので、flush で失敗してもシーン切替は巻き戻らず、失敗は `effect_errors` として報告されます。 定義: [第4章 commit したのに作用が失敗したとき][def-36]。

### participant(参加者)

シーンロードで状態を遷移させる当事者。participant manifest で各段の担当を確認できます。 定義: [第13章 §13.13][def-37]。

### codec(component codec)

JSON・canonical・runtime を往復させるコンポーネントの受理仕様。組み込み型では未知キーを拒否します。 定義: [第12章 §12.5][def-38]。

### EntityId / generation(世代)

ECS の entity を index と generation で識別するハンドル。古い ID が再利用したスロットに誤接続するのを防ぎます。 定義: [第4章 §4.4][def-39]。



### arena(アリーナ)

behavior 実体をエンジン側でまとめて所有する置き場。ゲームコードは handle を通して触ります。 定義: [第8章 §8.5.5][def-41]。

### SoA / Chunk / Archetype

ECS の格納形。Archetype は同じ構成の集合、Chunk はその箱、SoA はコンポーネント別の連続配列です。 定義: [第4章 §4.4][def-42]・[source-code-guide 第4章][def-421]。



### relocate(詰め替え)

entity を削除したときに、末尾の要素を空いた位置へ移して列を詰める操作(swap-and-pop)。型消去された列では move 構築 + 破棄を 1 つの関数ポインタにまとめてこれを行います。**要素のアドレスが動く**ので、コンポーネントへの生ポインタをフレームをまたいで持つのは危険です。 定義: [第4章 §4.4][def-44]。

### MutationScope

ECS の構造変更中に構造変更が再入するのを拒否するガード。半端な状態の観測を防ぎます。 定義: [第4章 §4.4][def-45]。



### hazard(ハザード)

ECS の hazard はシステムの read / write 宣言に基づく競合関係。定義: [第8章 §8.3][def-47]。

フレームグラフの hazard は資源依存から barrier を導く競合関係。定義: [第6章 §6.6][def-hazard-render]。

---

## 14.4 ゲームロジックの語

### behavior(オブジェクト behavior)

シーンのオブジェクトに貼るゲームコードの単位。型名と params からゲーム DLL のクラスを実体化します。 定義: [第8章 §8.5][def-49]。

### GameContext

ゲームコードがエンジンに触るための**公開ファサード**(唯一の窓口)。状態を持たず、毎フレーム作られて渡されます。入力・カメラ・物理クエリ・音・保存・乱数などがここに集まっています。 定義: [第8章 §8.4][def-50]。

### order(順序値)と総順序

ゲームシステムの実行順を決める整数。同じ `order` のシステムは**名前の辞書順**で並ぶので、全システムに一意な順序が決まります(= 総順序)。これが決定性の土台の 1 つです。 定義: [第8章 §8.3][def-51]。

### epoch anchor

ゲーム DLL が必ず import する、ABI の版を名前に含むシンボル。ロード前に古い DLL を拒否する目印です。 定義: [第8章 ゲーム DLL の ABI][def-52]。

### shadow copy(シャドウコピー)

ゲーム DLL の原本を `.pelican-hot-reload/<プロセス ID>/` へ複製してロードする方式。 定義: [第8章 ゲーム DLL の ABI][def-53]。

### PCG32(permuted congruential generator)

決定的乱数サービスが使う擬似乱数生成器。分布まで自前実装し、既定シードは 0 です。 定義: [第8章 §8.10][def-54]。

### provider(プロバイダ)

物理の provider は query の capability に合う実装の供給者。定義: [第8章 §8.8][def-55]。

描画の provider は view や描画項目を供給する実装。定義: [第6章 §6.12][def-provider-render]。

### MTD(minimum translation distance)

初期重なりを引き離す移動量。mesh の `approximate_depenetration` は最短の保証がありません。 定義: [第8章 §8.8][def-57]。

### matrix palette(マトリクスパレット)

スキニングで使うボーン行列の配列。glTF の skin ごとの範囲が結合パレットを過不足なく覆うか検証します。 定義: [source-code-guide 第3章][def-58]。

---

## 14.5 描画とレイトレーシングの語

### RT(render target)

描画結果を書き込む画像。`render_targets` で宣言し、パスから名前で参照します。 定義: [第6章 render_targets][def-59]。

### swapchain(スワップチェーン)

表示画像を GPU とウィンドウ間で回す仕組み。`swapchain` は RT 名として使えない予約名です。 定義: [第6章 render_targets][def-60]。

### format_class

RT の format を役割で指定するキー。`scene`・`display`・`data`・`explicit(...)` を解決します。 定義: [第6章 §6.3][def-61]。

### SRGB / リニアワークフロー

シェーダではリニア値を使い、表示時に sRGB へ変換する色の規約。変換は終端で 1 回だけ行います。 定義: [第6章 §6.3][def-62]。

### G-buffer

後で色を決めるための中間 RT 群。構成はエンジン固定でなく material output ABI で宣言できます。 定義: [第6章 任意 G-buffer][def-63]。

### stem(ステム)

シェーダを拡張子なしの名前で参照する規約。GLSL ソースを先に、ビルド済み SPIR-V を次に探します。 定義: [第6章 stem 参照][def-64]・[第9章 §9.4][def-641]。

### SPIR-V / SPIR-V reflection

SPIR-V はシェーダの中間バイナリ。reflection はそこから資源の型と binding を読み、layout を自動生成する処理です。 定義: [第6章 自動リフレクションとキャッシュ][def-65]。

### descriptor set

シェーダに渡す資源の束。Pelican は set 0〜3 の役割を規約で固定しています。 定義: [第6章 descriptor set 規約][def-66]。

### feature

GPU 機能を JSON とシェーダの断片で足すユーザー空間の単位。参照したときだけ存在します。 定義: [第6章 §6.5][def-67]。

### canonical anchor(標準アンカー)

feature の挿入先になる 8 つの標準ノード。ノード間への挿入はできますが、標準ノードの全順序は固定です。 定義: [第6章 canonical anchor][def-68]。

### frame graph(フレームグラフ)

パスの入出力から依存・実行順・barrier を導く仕組み。JSON 宣言順は依存が許す範囲のタイブレークです。 定義: [第6章 §6.6][def-69]。

### FramePlan

フレームグラフを解決した実行計画。ノード・level・barrier を RPC や plan viewer で調べられます。 定義: [第6章 §6.6][def-70]。

### logical plan / physical plan(論理プラン / 物理プラン)

論理プランはデバイス非依存の値と読み取り範囲、物理プランは format・sample 数・scope・view 実行を載せた表現。 定義: [第6章 target planning と Vulkan plan pin][def-71]。

### scope(スコープ)

物理プランの scope は同じ実行条件でまとめる単位です。定義: [第6章](06_rendering.md#physical-plan-scope)。

### グラフ編集の scope

グラフ編集の scope は root または definition の編集対象です。定義: [第13章](13_editor.md#graph-edit-scope)。

### view family(ビューファミリ)

同じ内容を複数視点で描くときの視点の束。`$main` と secondary family では view 数の決まり方が異なります。 定義: [第6章 §6.12][def-73]。

### `.surface`

シェーダとパラメータ宣言をまとめたマテリアルの自己記述ファイル。値は別の material JSON に書きます。 定義: [第6章 .surface][def-74]。

### SSAO(screen-space ambient occlusion)

深度と法線から暗さを近似する画面空間の手法。同梱シェーダを使い、プロジェクト自身がパスを組みます。 定義: [第6章 §6.4][def-75]。

### bloom(ブルーム)

明るい部分をにじませる後処理。bloom feature の threshold・knee は define、intensity は実行時バッファ値です。 定義: [第6章 §6.4][def-76]。

### materialize(マテリアライズ)

同梱 feature をプロジェクトの派生ファイルにする操作。parameters の宣言は残り、値は bound_parameters に保存します。 定義: [第6章 §6.5][def-77]。

### TAA(temporal anti-aliasing)

履歴と現在の画像を混ぜるアンチエイリアス。同梱 `taa.json` は resolve・composite と jitter provider を持ちます。 定義: [第6章 標準 TAA][def-78]。

### projection jitter

TAA のために、毎フレーム射影行列を**サブピクセル単位でずらす**こと。ずらす列は halton23 などの低食い違い数列で、`@history` の履歴 RT と組で使います。 定義: [第6章 projection jitter][def-79]。

### VAT(Vertex Animation Texture)

頂点の動きを焼き込む形式。Pelican は glb の half-float bufferView に保存し、再生時に手動 lerp します。 定義: [第5章 §5.4][def-80]。

### IBL(image-based lighting)

環境を写した画像から、拡散・鏡面の環境光を作るライティング手法。**pelican2 では未実装**です(WP32 は番号予約のみ)。同梱の `sky_ambient.json` は単色の環境光であって IBL ではありません。 定義: [第11章 §11.1][def-81]。

### AS / BLAS / TLAS(加速構造)

AS は交差候補を絞る構造、BLAS は形状、TLAS はその配置の構造。静的形状とスキン・モーフ・VAT の変形に対応します。 定義: [第6章 §6.15][def-82]。

### SBT(shader binding table)

レイとシェーダを対応付ける表。`ray_group_layout` が形を決め、AS は layout の digest に含めません。 定義: [第6章 §6.15][def-83]。

### `ray_group_layout`(ray layout)

レイトレ宣言の入口。version 3 のみ受理し、layout の有無は起動時に確定します。 定義: [第6章 §6.15][def-84]。

---

## 14.6 テスト・運用・ツールの語

### JSON-RPC(stdio)

外部プロセスから player を制御するプロトコル。stdio は 1 行 1 JSON、stdout はプロトコル専用です。 定義: [第10章 §10.3][def-85]。

### ホットリロード(hot reload)

保存されたファイルを実行中に反映する仕組み。変更に影響する候補をまとめて準備し、失敗時は旧世代を保ちます。 定義: [第10章 §10.5][def-86]。

### チケット(ticket)

編集をフレーム境界で適用する受付票。受付時と commit 時に前提条件を検査します。 定義: [第13章 §13.3][def-87]。

### headless(ヘッドレス)

ウィンドウを開かずに描画・実行するモード。時刻が固定ステップ(dt = 1/fps)になるためリプレイと golden テストの土台になり、CI から PNG を取り出せます。 定義: [第2章 §2.5][def-88]。





### doclink

文書中の `#L<行番号>` リンクが指す先を追跡し続けるためのツール(`tools/doclink.py`)。`update` で行番号を貼り直し、`check` で古くなったリンクを検出します。行番号はキャッシュで、本体は「その行に何が書いてあったか」というアンカーです。 定義: [第10章 §10.10][def-91]。

---

## 14.7 共通グラフと姿勢の語

### 共通グラフ(`pelican.graph` v1)

値・描画・姿勢を 1 つのノード語彙で繋ぐ機構と文書形式。`pelican.value_graph` は内部 IR です。 定義: [第6章 §6.16][def-92]。

### `graph.instance`(装着)

definition を 1 回実体化するノード。入力は辺・parameters・既定値、実行時 override は `render().setValueGraphParameter` で設定します。 定義: [第4章 §4.1][def-93]。

### 姿勢グラフ / `pelican.pose`

共通グラフの姿勢 module。アニメーションのフェーズで評価し、スカラーから姿勢へ一方向に繋ぎます。 定義: [第8章 §8.13][def-94]・[第4章 §4.2][def-941]。

### 単一 writer / writer token

1 個体の姿勢を書けるのは 1 本だけという規則。装着時と公開時の writer token の両方で検査します。 定義: [第4章 §4.2][def-95]。

### root motion

クリップの根の移動を owner の localtransform に渡す機能。v0 では回転を抽出せず、シーク時の移動量は 0 です。 定義: [第4章 §4.2][def-96]・[第8章 §8.13][def-961]。

### catalog(2 系統)

グラフの catalog は登録済み module・型・op・sink の語彙。定義: [第10章 §10.4][def-97a]。

feature の catalog は同梱 feature と install 済み package の一覧。定義: [第13章 §13.5][def-97b]。

### ループ(`graph.loop`)

definition を集合の要素ごとに使う構造ノード。絞り込み・並べ替え・limit・分岐を解決して body を使います。 定義: [第6章 §6.18][def-98]。

### 分岐(branch)

ループの要素ごとの条件と body の組。最初に合った分岐を使い、最後は when 無しの else が必須です。 定義: [第6章 §6.18][def-99]。

### carry

ループの前回の出力を次回の入力へ渡す指定。expand 段だけで使えます。 定義: [第6章 §6.18][def-100]。

### 段(tier)—— expand / draw_loop

expand は組み直し時に body を複写する段、draw_loop は material pass 内で描き方を選ぶ段。 定義: [第6章 §6.18][def-101]。

### draw step(描き方の 1 手)

draw_loop 内で material の描き方を 1 手ずつ指定するノード。after の辺で実行列を作ります。 定義: [第6章 §6.18][def-102]。

### view の軸(view axis)/ `view_select`

view が持つ名前付き整数の座標。view_select は一致する view を選び、固定長の軸はループにも使えます。 定義: [第6章 §6.18][def-103]。

### `render.target_ref`

既存の render target を名前で指すノード。target は作らず、合成後の名前を検査します。 定義: [第6章 §6.18][def-104]。

### `pass_replacements`

feature の pass・task を graph のノードやループで置き換える指定。順序と資源名も引き継ぎます。 定義: [第6章 §6.18][def-105]。

### 分解版(decomposed graph)

標準 feature の処理をノードとループで書き直したグラフ。出荷雛形を自分の graph へ写して使います。 定義: [第6章 §6.18][def-106]・[第13章][def-1061]。

---

### `name@1`(版付き識別子)

名前と契約の版を組にした機械可読識別子。エラーコードにも operation・implementation ID にも使います。 定義: [第4章 §4.5][def-6]。

### transaction(トランザクション)

全体を適用するか、失敗時に巻き戻す更新の単位。scene load は publish 後も activate で失敗でき、commit が確定点です。 定義: [第4章 §4.3][def-extra0]。

### `rendering` 節

共通グラフ文書の中に rendering config を置く object。グラフ root のキーとは所有を分けます。 定義: [第6章 §6.16][def-extra1]。

### 共有部品(render component)

materialize 済み feature 1 本と宿主への要求をまとめ、別 project へ持ち出す部品。 定義: [第3章 §3.12][def-extra2]。

---

## 14.8 設計・コード解説で頻出する用語

文書では次の英語や略語を使います。識別子や JSON のキーは原文のまま記載しますが、説明文では以下の意味です。

| 用語 | この資料での意味 |
|---|---|
| 契約（contract） | 入出力、制約、失敗時の応答など、呼び出し側と実装側が守る仕様。法律上の契約を指すものではありません。 |
| 正本 | 食い違いを判断する際の基準文書。現在の実装状況、公開仕様、過去の設計意図は区別して参照します。 |
| 面 / surface | API・編集操作・データ形式などを外部に公開する範囲。文脈が曖昧なときは「公開 API」「編集対象」などと読み替えます。`.surface` はマテリアルのシェーダ定義ファイルという別の意味です。 |
| authoring | 人やツールが編集・保存するデータ。実行時（runtime）に生成されるデータと区別します。 |
| schema（スキーマ） | データに使えるキー、型、必須項目などの形式規則。 |
| canonical（正規形） | 比較や処理を一貫させるために表現を統一した形。「正しい値」の同義語ではありません。 |
| IR（Intermediate Representation、中間表現） | 入力の宣言を解析してから実行用データを作るまでの間に使う表現。 |
| lowering | 宣言や中間表現を、実行方式に近い具体的な表現へ変換する処理。たとえば描画グラフから Vulkan 向けの計画を作ります。 |
| backend（バックエンド） | 共通の仕様を、Vulkan などの具体的な実行方式で実現する部分。 |
| dialect | 共通グラフ内で、ある処理分野が使う型・操作・規則のまとまり。 |
| capability | 利用可能な機能を指す用法と、対象への操作権限を持つ token を指す用法があります（[ガイド第5章](../source-code-guide/05_gameplay_and_services.md)）。 |
| op（operation） | グラフのノードが行う操作の種類。JSON の `op` に `value.constant` などを指定します。 |
| port（ポート） / edge（辺） | ノードの値の出入口と、その接続。辺がデータや実行順序の依存を表します。 |
| sink（出力先） | グラフの評価結果を、姿勢や描画用バッファなどの外部の状態へ反映する出口。 |
| identity | 対象が同じものかを判別する識別情報。単なる表示名とは限りません。 |
| digest（ダイジェスト） | 内容の照合値で、文書の `source_digest` と要求の base の関係は[第13章](13_editor.md#digest-cas)を参照してください。 |
| CAS（Compare And Swap） | 想定値と現在値が一致したときだけ更新する方式で、文書編集の規則は[第13章](13_editor.md#digest-cas)を参照してください（CPU の原子命令を指す用法もあります）。 |
| epoch / generation / revision | 型ごとに対象が異なる世代・版の識別値で、描画の generation は[第6章](06_rendering.md#render-generation)を参照してください。 |
| 描画の世代 / publish / commit | 描画で同時に使う snapshot とその公開・確定で、定義は[第6章](06_rendering.md#render-generation)にあります。 |
| preset | rendering config が選ぶ基本構成で、engine / project と fork の関係は[第6章](06_rendering.md#render-preset)を参照してください。 |
| stale_source_digest / external_modification | commit 時の保持版・ディスクとの不一致を区別する code で、受付時との違いは[第13章](13_editor.md#digest-cas)を参照してください。 |
| lease（リース） | 資源の利用権や寿命を保持する仕組み。GPU テストの排他実行と、GPU 送信完了までの資源保持では対象が異なります。 |
| submission | GPU キューへコマンドを送ること、またはその送信単位。CPU 側での送信と GPU 側での完了は別です。 |
| refit / rebuild | 加速構造の既存の構成を利用した更新と、作り直し。使い分けの条件は [加速構造の解説](../source-code-guide/11_ray_query_acceleration_structures.md) を参照してください。 |
| gate / fixture / golden | 合否を判定する検証、検証用の固定データや環境、比較の基準にする結果。どの範囲を検証したかは個々のテストによります。 |

---

## 関連文書

- [第11章 実装状況と文書マップ](11_status.md) — WP 台帳と設計文書マップ。語の出所をたどるときの起点
- [第12章 コード読解ガイド](12_codemap.md) — JSON がどのパーサで読まれるかの対応表
- [../source-code-guide/10_background_knowledge.md](../source-code-guide/10_background_knowledge.md) — Vulkan / glTF / C++ / OS の**一般知識**の詳説(本章の一般用語はここへ繋がります)
- [../source-code-guide/README.md](../source-code-guide/README.md) — クラス単位の内部構造解説

[def-0]: 11_status.md#111-wp-全台帳
[def-1]: 11_status.md#111-wp-全台帳
[def-2]: 13_editor.md#131-全体像--何ができるようになったか
[def-3]: 08_gameplay.md#ゲーム-dll-の-abiwp381381a382a
[def-4]: 08_gameplay.md#810-決定性--なぜ毎回同じ結果になるのか
[def-5]: 01_overview.md#15-横断的な設計原則
[def-6]: 04_scene_ecs.md#45-よくあるエラー
[def-7]: 01_overview.md#15-横断的な設計原則
[def-8]: 06_rendering.md#65-feature--パージ可能な-gpu-機能wp2831548788104226
[def-9]: 09_web.md#92-サブセット原則不変条件
[def-9-1]: 03_project_format.md#37-サブセット原則web-との関係
[def-10]: 05_assets.md#51-二層モデル--この章で最も重要な原則
[def-11]: 06_rendering.md#610-ヘッドレス描画とテストwp1816
[def-12]: 10_tools.md#109-テスト基盤
[def-13]: 04_scene_ecs.md#エンベロープの規則
[def-14]: 12_codemap.md#125-データ--コード対応表json-はどこで読まれるか
[def-15]: 03_project_format.md#38-既知の食い違い設計文書-vs-実装
[def-16]: 03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a
[def-17]: 03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a
[def-18]: 03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a
[def-19]: 03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a
[def-20]: 03_project_format.md#33-パス参照と-pathresolver
[def-21]: 03_project_format.md#35-解釈レイヤ--pelican_project-とバインダの分離
[def-22]: 03_project_format.md#34-asset-store--置き場所の間接化実装済みwp55
[def-23]: 05_assets.md#56-フラグメント参照とアセットコンテナ実ロード対応wp55777984
[def-24]: 03_project_format.md#39-シーンドキュメントと保存エディタ経路wp149158166168
[def-25]: 03_project_format.md#basic_config-と3-段合成
[def-26a]: 03_project_format.md#機能ユニットの要求requiresp-2b
[def-26b]: 03_project_format.md#311-package--本体ツリーの外から持ち込むx-1
[def-26c]: 06_rendering.md#65-feature--パージ可能な-gpu-機能wp2831548788104226
[def-27a]: 04_scene_ecs.md#42-コンポーネント一覧シーンに書けるものすべて
[def-27b]: 03_project_format.md#312-共有部品render-component-feature-1-本を持ち出して別-project-へ入れるp-2a-3
[def-27c]: 03_project_format.md#311-package--本体ツリーの外から持ち込むx-1
[def-28]: 03_project_format.md#311-package--本体ツリーの外から持ち込むx-1
[def-29]: 03_project_format.md#namespace-は文書の中に入る
[def-30]: 06_rendering.md#616-rendering-config-の-root--pelicangraph-rootp-2a-4a4bmg-2b
[def-31]: 04_scene_ecs.md#43-シーンロードの流れ
[def-32]: 04_scene_ecs.md#43-シーンロードの流れ
[def-33]: 04_scene_ecs.md#43-シーンロードの流れ
[def-34]: 04_scene_ecs.md#ロード中の作用はトランザクションに溜まるwp380be
[def-35]: 04_scene_ecs.md#ロード中の作用はトランザクションに溜まるwp380be
[def-36]: 04_scene_ecs.md#commit-したのに作用が失敗したとき
[def-37]: 13_editor.md#1313-シーン遷移とゲームロジックの結果wp378380b380e381381a
[def-38]: 12_codemap.md#125-データ--コード対応表json-はどこで読まれるか
[def-39]: 04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け
[def-41]: 08_gameplay.md#855-実行順とライフサイクル決定性
[def-42]: 04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け
[def-421]: ../source-code-guide/04_ecs_deep_dive.md#chunkはsoa
[def-44]: 04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け
[def-45]: 04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け
[def-47]: 08_gameplay.md#83-ゲームシステムの登録と実行順序wp43
[def-49]: 08_gameplay.md#85-オブジェクト-behavior--もう一つのゲームコードの書き方wp155162167
[def-50]: 08_gameplay.md#84-gamecontext-api-一覧
[def-51]: 08_gameplay.md#83-ゲームシステムの登録と実行順序wp43
[def-52]: 08_gameplay.md#ゲーム-dll-の-abiwp381381a382a
[def-53]: 08_gameplay.md#ゲーム-dll-の-abiwp381381a382a
[def-54]: 08_gameplay.md#810-決定性--なぜ毎回同じ結果になるのか
[def-55]: 08_gameplay.md#88-物理クエリとトリガーp1p2wp107179
[def-57]: 08_gameplay.md#88-物理クエリとトリガーp1p2wp107179
[def-58]: ../source-code-guide/03_project_and_loading.md
[def-59]: 06_rendering.md#render_targets
[def-60]: 06_rendering.md#render_targets
[def-61]: 06_rendering.md#63-カラーパイプラインwp7274
[def-62]: 06_rendering.md#63-カラーパイプラインwp7274
[def-63]: 06_rendering.md#任意-g-buffer--material-output-abiattachment-statewp218219
[def-64]: 06_rendering.md#stem-参照
[def-641]: 09_web.md#94-シェーダ-stem-規約--可搬シェーダの書き方
[def-65]: 06_rendering.md#自動リフレクションとキャッシュ
[def-66]: 06_rendering.md#descriptor-set-規約shader_contractmd-が正
[def-67]: 06_rendering.md#65-feature--パージ可能な-gpu-機能wp2831548788104226
[def-68]: 06_rendering.md#canonical-anchorwp73
[def-69]: 06_rendering.md#66-フレームグラフf0f2--wp3335--wp64
[def-70]: 06_rendering.md#66-フレームグラフf0f2--wp3335--wp64
[def-71]: 06_rendering.md#target-planning-と-vulkan-plan-pin
[def-72]: 06_rendering.md#vulkan-physical-fragment-v1--v2--v3
[def-721]: ../source-code-guide/06_rendering_vulkan_shader.md
[def-73]: 06_rendering.md#612-openxr-ステレオレンダリングwp125138--wp203ac実装済み
[def-74]: 06_rendering.md#surface--自己記述コンテナ
[def-75]: 06_rendering.md#64-シェーダ基盤shader-freedom-kit-wp101519--m2bwp70
[def-76]: 06_rendering.md#64-シェーダ基盤shader-freedom-kit-wp101519--m2bwp70
[def-77]: 06_rendering.md#65-feature--パージ可能な-gpu-機能wp2831548788104226
[def-78]: 06_rendering.md#標準-taawp113
[def-79]: 06_rendering.md#projection-jitterwp112115
[def-80]: 05_assets.md#54-焼き込みアニメーション2--pelicanvat-v1wp20
[def-81]: 11_status.md#111-wp-全台帳
[def-82]: 06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v3-と-pelican_ray_instancert3-1--wp374374a377379383
[def-83]: 06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v3-と-pelican_ray_instancert3-1--wp374374a377379383
[def-84]: 06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v3-と-pelican_ray_instancert3-1--wp374374a377379383
[def-85]: 10_tools.md#103-json-rpcstdio-外部制御プロトコル実装済み
[def-86]: 10_tools.md#105-ホットリロードwp9096110147162178
[def-87]: 13_editor.md#133-編集の実行モデル--チケットとフレーム境界
[def-88]: 02_getting_started.md#25-ヘッドレス実行ウィンドウなしで-png-を出す
[def-91]: 10_tools.md#1010-リポジトリ保守スクリプトtools
[def-92]: 06_rendering.md#616-rendering-config-の-root--pelicangraph-rootp-2a-4a4bmg-2b
[def-93]: 04_scene_ecs.md#authoring-object-id-と-graphinstanceownerag-1ap-1p-2b
[def-94]: 08_gameplay.md#813-アニメーション再生wp389497101102176178ag-1a1b2
[def-941]: 04_scene_ecs.md#姿勢の-2-つの装着様式ag-1aag-1b
[def-95]: 04_scene_ecs.md#姿勢の-2-つの装着様式ag-1aag-1b
[def-96]: 04_scene_ecs.md#localtransform
[def-961]: 08_gameplay.md#813-アニメーション再生wp389497101102176178ag-1a1b2
[def-97a]: 10_tools.md#104-pelican_cli-リファレンス
[def-97b]: 13_editor.md#135-編集-rpc-リファレンス
[def-98]: 06_rendering.md#618-共通グラフのループとグラフで組む-passlp-0a0e--lp-1a1e--lp-va--lp-va2--lp-vb--lp-2--lp-3a--lp-3b--lp-6
[def-99]: 06_rendering.md#618-共通グラフのループとグラフで組む-passlp-0a0e--lp-1a1e--lp-va--lp-va2--lp-vb--lp-2--lp-3a--lp-3b--lp-6
[def-100]: 06_rendering.md#618-共通グラフのループとグラフで組む-passlp-0a0e--lp-1a1e--lp-va--lp-va2--lp-vb--lp-2--lp-3a--lp-3b--lp-6
[def-101]: 06_rendering.md#618-共通グラフのループとグラフで組む-passlp-0a0e--lp-1a1e--lp-va--lp-va2--lp-vb--lp-2--lp-3a--lp-3b--lp-6
[def-102]: 06_rendering.md#618-共通グラフのループとグラフで組む-passlp-0a0e--lp-1a1e--lp-va--lp-va2--lp-vb--lp-2--lp-3a--lp-3b--lp-6
[def-103]: 06_rendering.md#618-共通グラフのループとグラフで組む-passlp-0a0e--lp-1a1e--lp-va--lp-va2--lp-vb--lp-2--lp-3a--lp-3b--lp-6
[def-104]: 06_rendering.md#618-共通グラフのループとグラフで組む-passlp-0a0e--lp-1a1e--lp-va--lp-va2--lp-vb--lp-2--lp-3a--lp-3b--lp-6
[def-105]: 06_rendering.md#618-共通グラフのループとグラフで組む-passlp-0a0e--lp-1a1e--lp-va--lp-va2--lp-vb--lp-2--lp-3a--lp-3b--lp-6
[def-106]: 06_rendering.md#618-共通グラフのループとグラフで組む-passlp-0a0e--lp-1a1e--lp-va--lp-va2--lp-vb--lp-2--lp-3a--lp-3b--lp-6
[def-1061]: 13_editor.md
[def-extra0]: 04_scene_ecs.md#43-シーンロードの流れ
[def-extra1]: 06_rendering.md#616-rendering-config-の-root--pelicangraph-rootp-2a-4a4bmg-2b
[def-extra2]: 03_project_format.md#312-共有部品render-component-feature-1-本を持ち出して別-project-へ入れるp-2a-3
[def-resolve-render]: 06_rendering.md#63-カラーパイプラインwp7274
[def-binding-input]: 07_input_ui.md#72-アクション定義とバインディングプロファイル
[def-hazard-render]: 06_rendering.md#66-フレームグラフf0f2--wp3335--wp64
[def-provider-render]: 06_rendering.md#612-openxr-ステレオレンダリングwp125138--wp203ac実装済み
