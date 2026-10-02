# 第14章 用語集

対象: pelican2(HEAD=`82054c05`・2026-09-17、branch
`codex/render-target-runtime-slice`)/ このマニュアルはコードを正とする

用語・仕様説明の再照合: 2026-10-01（`develop` / `c6df4822`）。主な訂正内容は [確認記録](../documentation_review_2026-10-01.md) にあります。

## この用語集の使い方

- **複数の章で説明なしに出てくる語だけ**を集めた索引です。正確な仕様は各語の「初出」リンク先にあり、ここはその入口です。
- 設計文書に多い英語・略語は [§14.8](#148-設計コード解説で頻出する用語) にまとめています。
- 五十音順ではなく**分野別**に並んでいます。語を探すときはブラウザの検索(Ctrl+F)を使ってください。
- エンジン固有の語には実装への行リンクを添えました。**説明と実装が食い違ったら実装が正**です(食い違いの台帳は [第11章 §11.3](11_status.md#113-既知の食い違い設計文書-vs-コード))。

---

## 14.1 全体・体制・品質の語

**WP(Work Package)** — 実装作業の単位。**識別子は 2 系統**あります: `WP1`〜`WP386` の**通し番号**(`WP380a` のような枝番が付く)と、2026-09-16〜17 の v0 で使われた**記号 ID**(`AG-0` / `AG-1b` / `P-2a-1c` / `X-1` / `O-1` / `R1-2` / `N-2` …。「系統 + 段」で読み、`AG` = アニメーションと共通グラフ、`P` = パラメータと部品、`X` = 持ち出し、`O` = オフライン出力、`R` = 整理と凍結、`N` = ノードキャンバス)。どちらも **1 単位 = 1 ブランチ = 1 マージ**で、本マニュアルの `✅WP155/162/167` や `✅P-2a-3` のようなバッジは「その機能がどの単位で入ったか」を指します。状態の正は台帳 [../implementation_plan.md](../implementation_plan.md) で、本マニュアルの表は転記です。初出: [第11章 §11.1](11_status.md#111-wp-全台帳)。

**台帳** — 仕様・作業・検証結果を一覧で管理する文書の呼び方です。WP の仕様と履歴は [実装指示書](../implementation_plan.md) と [過去の完了記録](../implementation_archive.md)、v0 の公開仕様は [凍結一覧](../v0_freeze.md)、開発の現況は [v1 開発計画](../v1_development_plan.md) を参照します。履歴にある「未実装」「次に行う」を、そのまま現在の状態と解釈しないでください。 初出: [第11章 §11.1](11_status.md#111-wp-全台帳)。

**D0** — 「エディタに特権 API を作らない」という設計決定の記号。編集操作はまず JSON-RPC メソッドとして定義され、エンジン内蔵の ImGui インスペクタも外部の Python クライアントも Qt Studio も**同じ関数**を呼びます。Studio がエンジン内部へリンクしていないことはビルドレベルのテスト(`test/run_devstudio_boundary_negative.cmake`)で守られます。初出: [第13章 §13.1](13_editor.md#131-全体像--何ができるようになったか)。

**ABI(Application Binary Interface)** — 別々にコンパイルされたバイナリが呼び合うときの、関数名・引数の並び・構造体レイアウトの約束。pelican2 では「ゲーム DLL の ABI」(登録入口の名前と epoch anchor)、「physics provider ABI」、「material output ABI」など、**DLL や shader をまたぐ境界ごと**にこの語が出ます。初出: [第8章 ゲーム DLL の ABI](08_gameplay.md#ゲーム-dll-の-abiwp381381a382a)。([abi_v2.hpp:83](../../src/core/userpublic/physics/abi_v2.hpp#L91))

**決定的 / 決定性(deterministic)** — 同じ条件で実行したときに、処理順序や結果を再現できる性質です。このエンジンではシステムの実行順、入力の取得時点、固定シードの乱数、物理結果の整列などを明示します。時間に依存する処理には同じ時刻列も必要で、ウィンドウモードの実測 dt は毎回同じとは限りません。ヘッドレスの固定ステップは再現試験に使えますが、異なる GPU・ドライバ・ビルド間で浮動小数点の結果や画像が必ずビット単位で一致するという保証ではありません。 初出: [第8章 §8.10](08_gameplay.md#810-決定性--なぜ毎回同じ結果になるのか)。

**hard error** — 警告で済ませず**起動・ロードをその場で止める**エラー。スキーマ不一致・バージョン不一致・未知の名前・必須フィールド欠落がこれにあたります。「読めるふりをして一部だけ壊れる」事故を防ぐための方針で、メッセージには必ず手がかり(解決後の絶対パス・登録済み id 一覧・OFF になっているビルドフラグ名)が入ります。初出: [第1章 §1.5](01_overview.md#15-横断的な設計原則)。

**`<name>@<version>` 形式のエラーコード** — `component_unknown@1` / `game_logic_abi_stale@1` / `pelican.ray_tracing.bindings_without_layout@1` のように、**名前 + `@` + 契約の版**でエラーを識別する書き方。`@1` は「このエラーコードの意味の版」であり、エンジンの版でもスキーマの版でもありません。RPC の応答やログにそのまま出るので、コードで検索すれば発生箇所に到達できます。初出: [第4章 §4.5](04_scene_ecs.md#45-よくあるエラー)。([componentinfo.hpp:24](../../src/core/ecs/componentinfo.hpp#L26))

**名前入りの拒否(named reject)** — 拒否するときは必ず上の形式のコードを添える、という運用のこと。対義は「黙って素通しする」で、pelican2 は素通しを避ける側に倒しています。ただし web プロファイルだけは未知コンポーネントを `opaque` として素通しします(後述)。初出: [第1章 §1.5](01_overview.md#15-横断的な設計原則)。

**パージ可能(purgeable)** — 「参照を書かなければ機構ごと存在しない」性質。feature は rendering config に 1 つも書かなければ合成機構ごと素通りし、挙動は完全に不変です。UI も同じで、参照しなければモジュールごと生成されません。「使っていない機能が既定の挙動を変えていないこと」を golden テストで保証するための設計語です。初出: [第6章 §6.5](06_rendering.md#65-feature--パージ可能な-gpu-機能wp2831548788104226)。

**サブセット原則** — 「**web で開けるもの ⊆ pelican で開けるもの**」という不変条件。形式の拡張は常にエンジンが先行し、web が追随します。web が読めてエンジンが読めない、は違反です。初出: [第9章 §9.2](09_web.md#92-サブセット原則不変条件)、要約は [第3章 §3.7](03_project_format.md#37-サブセット原則web-との関係)。

**二層モデル** — アセットを「**ランタイム層**(エンジンが直接読む、閉じた小集合の形式 — glb / png / wav など)」と「**ソース層**(DCC の作業ファイル — FBX / PSD など)」に分け、エンジンはランタイム層しか読まない、という原則。ソース層 → ランタイム層の変換は `pelican_cli import` などの取り込み側の仕事です。初出: [第5章 §5.1](05_assets.md#51-二層モデル--この章で最も重要な原則)。

**golden** — 「正解の出力」を固定しておき、それとの一致で回帰を検出するテストのやり方。pelican2 では `test/golden/<ケース名>/` に `case.json` と `expected.png` があり、ヘッドレス描画の結果と突き合わせます(既定は許容差 0 の完全一致)。色は「encoded-sRGB + リニア straight alpha」契約です。初出: [第6章 §6.10](06_rendering.md#610-ヘッドレス描画とテストwp1816)。

**fixture** — テストが使う固定の入力データ(または、それを生成するコード)。`test/fixtures/` 以下に置かれます。うち `test/fixtures/project_format/` は web プロファイルと共有され、同じ入力に対する合否の一致を web 側の実行で検証します(§9.2 の「fixture 共有」)。初出: [第10章 §10.9](10_tools.md#109-テスト基盤)。

---

## 14.2 形式(JSON)とプロジェクトの語

**エンベロープ** — JSON の先頭に置く `{"schema": "pelican.scene", "version": 2, ...}` の 2 キーのこと。封筒の中身(本体)を読む前に、**形式名と版が一致するかだけ**を先に見ます。不一致は hard error です。初出: [第4章 エンベロープの規則](04_scene_ecs.md#エンベロープの規則)。([projectformat.hpp:14](../../src/project/projectformat.hpp#L28))

**closed schema / open schema** — closed schema は「宣言されたキー以外が来たら拒否する」受理仕様、open schema は「知らないキーは無視して通す」受理仕様です。エンジンの組み込みコンポーネントは closed schema(未知キーを拒否)で、ユーザー定義コンポーネントは codec を持たないため扱いが変わります。初出: [第12章 §12.5](12_codemap.md#125-データ--コード対応表json-はどこで読まれるか)。

**opaque(web)** — web ビューア側で、codec を持たない名前のコンポーネントを**検証せずそのまま通す**扱いのこと。web が closed schema を持つのは組み込み 7 コンポーネントだけなので、`pelican_ray_instance` や `List` フィールドを書いたシーンは「native では拒否されるのに web では黙って通る」状態です。web リポジトリは pelican2 の外にあるため、この点はコードで裏取りできていません(**未確認**)。初出: [第3章 §3.8](03_project_format.md#38-既知の食い違い設計文書-vs-実装)。

**宣言 schema(v2 / v3)** — behavior やユーザー定義コンポーネントの params を「型付きで宣言する」仕組みの版。v3 で `Flags` / `List` / `OptionalOverride` の**再帰的な**入れ子が書けるようになりました。再帰 schema を使う型があると、SDK は自動的に `_v3` 登録入口を選びます。初出: [第3章 §3.10](03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a)。([SchemaNode](../../src/core/userpublic/details/schema/structfieldschema.hpp#L155))

**`Flags`** — 宣言済みの名前の集合を、JSON では**文字列の配列**で書くフィールド型。合成則は OR ではなく**置換**です(上書き側の配列がそのまま新しい値になる)。初出: [第3章 §3.10](03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a)。([Flags](../../src/core/userpublic/details/schema/structfieldschema.hpp#L194))

**`List<Row>` / 行封筒** — 可変長の行の並びを持つフィールド型。JSON では素の配列ではなく `{"$list": ..., "rows": [...]}` という **object コンテナ**で書きます。素の配列にしないのは、行の版や由来を後から足せるようにするためです。初出: [第3章 §3.10](03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a)。([List](../../src/core/userpublic/details/schema/structfieldschema.hpp#L222))

**wire** — 「ディスク / RPC に出る形」のこと。同じ値でも、C++ の構造体(runtime)・正規化した中間形(canonical)・JSON に出る形(wire)は別物で、codec がこの 3 者を往復させます。「wire モデル」は「JSON に出る形の設計」の意味です。初出: [第3章 §3.10](03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a)。

**解決(resolve)** — 参照や宣言から、実際に使う対象を確定する操作です。パス解決では `project://` や相対パスをプロジェクト基準、`user://` をユーザーデータ領域基準で解釈し、asset store は設定されたマウント先を使います。`engine://` は絶対ファイルパスではなく埋め込みリソースの識別子になる場合があります。`#` 以降のフラグメントも保持します。許可されたルートからの脱出は拒否しますが、すべての参照先がプロジェクトディレクトリ内にあるという意味ではありません。描画計画などで使う resolve は別の対象を確定する処理です。 初出: [第3章 §3.3](03_project_format.md#33-パス参照と-pathresolver)。([PathResolver](../../src/core/loader/pathresolver.hpp#L9) / [ProjectPathResolver](../../src/project/projectpathresolver.hpp#L69))

**結合(binding)** — 解決で得た実体を、**実行時のオブジェクトへ繋ぐ**操作。「形式の解析(`src/project`、GPU 不要)」と「実行時の結合(`src/core`)」を分けるのが pelican2 の二段構えで、`src/project` は純粋なパーサに保たれます。入力の `binding` は別語で、こちらは「デバイスの操作 → アクション名」の対応表です([第7章 §7.2](07_input_ui.md#72-アクション定義とバインディングプロファイル))。初出: [第3章 §3.5](03_project_format.md#35-解釈レイヤ--pelican_project-とバインダの分離)。

**asset store** — アセットの**置き場所を間接化**する仕組み。`project.json` に論理的なマウント(`assets/**` → store)を宣言し、マシンごとの実パスは git に入れない `.pelican/local.json` で上書きします。バイナリを VCS に入れずに参照を共有するための層です。初出: [第3章 §3.4](03_project_format.md#34-asset-store--置き場所の間接化実装済みwp55)。

**フラグメント参照(`#`)** — `models/town.glb#Building_02` のように、**コンテナ内の 1 要素**を指す参照。解決の過程でフラグメントを落とさずに運ぶのが規約です。初出: [第5章 §5.6](05_assets.md#56-フラグメント参照とアセットコンテナ実ロード対応wp55777984)。

**authoring / runtime** — 同じシーンの 2 つの表現。**authoring** は編集の正本(`AuthoringSceneDocument`)で、オブジェクトの順序や `AuthoringObjectId` を保ち、rename / reorder しても参照が切れないようにします。**runtime** はエンジンが実際に動かす ECS 側の姿です。エディタは authoring を編集し、その結果が runtime へ投影されます。初出: [第3章 §3.9](03_project_format.md#39-シーンドキュメントと保存エディタ経路wp149158166168)。([AuthoringSceneDocument](../../src/core/loader/authoringscenedocument.hpp#L95))

**3 段合成** — `basic_config` の値を「エンジン既定 → `project.json` → `.pelican/local.json`」の 3 段で重ねて決めること。deep merge ではなく**パス単位で問い合わせる**方式なので、どの段が勝ったかを後から言えます。初出: [第3章 basic_config と「3 段合成」](03_project_format.md#basic_config-と3-段合成)。

**`requires`(project.json の)** — **このプロジェクトが動くために必要なビルド機能ユニットの名前**の配列。今日の語彙は `animation` / `audio` / `physics` / `xr` / `vat` / `seqplayer` の 6 語(それぞれ `PELICAN_WITH_ANIMATION` / `_AUDIO` / `_PHYSICS` / `_OPENXR` / `_VAT` / `_SEQPLAYER`。✅PG-1)で、未知の名前は `project.requires_unknown@1`、満たせないビルドで開くと `<語>.module_unavailable@1`(例: `animation.module_unavailable@1`)になります。**同じ綴りの `requires` が 3 か所にあり、それぞれ別物です**: (1) project.json = ビルド機能ユニット名の配列、(2) package manifest = `{engine_min_version, sdk_abi, capabilities}` の object(`capabilities` だけは (1) と同じ語彙で検査)、(3) feature の `integrations[].requires` = 他の feature の名前。初出: [第3章 §3.2](03_project_format.md#機能ユニットの要求requiresp-2b)。([projectformat.cpp:80](../../src/project/projectformat.cpp#L81))

**component(3 つの別の語)** — 綴りが同じで中身が違う語が 3 つあります。**(1) ECS のコンポーネント** = シーン JSON の `{"name": "transform", …}` 1 レコード([第4章](04_scene_ecs.md))。**(2) render component**(`pelican.render_component` v1)= materialize 済みの feature 1 本 + その宿主への要求を 1 ディレクトリにまとめた**共有部品**([第3章 §3.12](03_project_format.md#312-共有部品render-component-feature-1-本を持ち出して別-project-へ入れるp-2a-3))。**(3) package manifest の `provides.components`** = (2) のディレクトリを指すパスの配列([第3章 §3.11](03_project_format.md#311-package--本体ツリーの外から持ち込むx-1))。文脈で読み分けてください。([rendercomponent.hpp:16](../../src/project/rendercomponent.hpp#L16))

**package(`pelican.package` v1)** — `package.json` を持つ 1 ディレクトリ。feature / グラフ文書 / 共有部品 / C++ コードをまとめて配れます。エンジンは**プロジェクトの外にある package を解決しません**。初出: [第3章 §3.11](03_project_format.md#311-package--本体ツリーの外から持ち込むx-1)。([packagemanifest.hpp:19](../../src/project/packagemanifest.hpp#L19))

**vendoring(ベンダリング)** — 外から持ち込んだものを**参照ではなく複製として自分のプロジェクトに取り込む**こと。`install_package` は package ディレクトリを丸ごと `project://packages/<namespace>/` へ copy し、`project.json.packages` に `{namespace, package_version, sha256}` を記録します。複製元が消えてもプロジェクトは動きます。**名前空間は文書の中にも入ります** —— 複製時に feature 文書の `name` が `<namespace>.<名前>` に書き換えられるので、名前を比較するすべての経路が同じ名前を見ます。初出: [第3章 §3.11](03_project_format.md#namespace-は文書の中に入る)。

**section locator(`rendering` 節)** — rendering config は `pelican.graph` root の `rendering` の下に入ります。「rendering config のキーはどこにあるか」を 1 箇所で決める仕組みがこれで、共通グラフの 5 キーは最上位、それ以外は `rendering` の下と決め([renderingSectionOf()](../../src/project/renderconfigsection.hpp#L36))、**バイト正確な編集**はその節の中で行います([`isGraphRootKey`](../../src/project/renderconfigdocument.hpp#L17) / [`withSectionValue`](../../src/project/renderconfigdocument.hpp#L103))。読み取りと書き込みが同じ locator を通ります。最上位が `pelican.graph` でない文書(旧い legacy root)は MG-2b から `render_config.legacy_root@1` で断られ、`rendering` 節を見つけられない graph root は `render_config_section_unlocated`(RPC の拒否では `render_config.section_unlocated@1`)です。初出: [第3章 §3.2](03_project_format.md#rendering-config-の-rootp-2a-4a4b-mg-2b)・[第6章 §6.16](06_rendering.md)。

---

## 14.3 ロードと ECS の語

**scene load transaction(シーンロードのトランザクション)** — 新しいシーンを試験的に組み立ててから**一斉に切り替える**方式。段は `prepare → early_publish → publish → activate → commit_metadata → retire → flush → finish` の 8 つ固定で、commit 前に失敗すれば全員で巻き戻します。「トランザクション」という語は編集チケット・effect transaction にも使われるので、文脈に注意してください。初出: [第4章 §4.3](04_scene_ecs.md#43-シーンロードの流れ)。([SceneLoadPhase](../../src/core/loader/sceneload.hpp#L27))

**prepare / publish / commit** — 状態の更新を段階に分けるための名前です。**prepare** は確保・検証などの準備、**publish** は準備した状態の反映、**commit** は更新の確定を表します。ただし、具体的な段階と失敗時の扱いは機能ごとに異なります。シーンロードでは publish 後も activate などで失敗する可能性があり、commit 前なら反映済みの状態も巻き戻します。`noexcept` は例外を呼び出し元へ出さないという C++ の指定であり、それだけで一連の更新の原子性（全体を適用するか、全体を戻すか）を保証するものではありません。 初出: [第4章 §4.3](04_scene_ecs.md#43-シーンロードの流れ)。

**retire(退役)** — 旧シーンを片付ける段。ここで旧オブジェクトの `onDestroy` が走り、旧 entity は既に unpublish 済みなので**新シーンからは見えません**。retire 中に新たなシーンロードを要求しても拒否されます。初出: [第4章 §4.3](04_scene_ecs.md#43-シーンロードの流れ)。([scene.cpp の requestLoadDuringRetire](../../src/core/loader/scene.cpp#L1855))

**staging(ステージング)** — 「作用を実行せず、実行可能な形にして取っておく」こと。ロード中に `playSound` や `saveData` を呼ぶと、その場では鳴らず・書かず、**実行に必要な準備だけ済ませた entry** がトランザクションに積まれます。溜めた分が実際に外へ出るのは、ロードの 8 段のうち **flush** 段(`commit` に成功したときだけ)です。具体例: `onInit` で `playSound("bgm")` を呼ぶと、その場では voice slot の予約だけが起き、音が鳴るのは flush 段です(ロードが失敗すれば予約ごと消え、鳴りません)。初出: [第4章 ロード中の「作用」](04_scene_ecs.md#ロード中の作用はトランザクションに溜まるwp380be)。([PreparedEffectTransaction](../../src/core/loader/effecttransaction.hpp#L27) / [SceneLoadPhase](../../src/core/loader/sceneload.hpp#L27))

**outbox** — 溜めた作用を入れる固定長のバッファ。容量は 256 で、溢れた分は「溢れた」という 1 件のエラーに畳まれます(黙って捨てません)。初出: [第4章 ロード中の「作用」](04_scene_ecs.md#ロード中の作用はトランザクションに溜まるwp380be)。([effecttransaction.hpp:25](../../src/core/loader/effecttransaction.hpp#L25))

**flush** — 溜めた作用を**実際に実行する**段(8 段の 7 番目)。ここで初めて音が鳴り、セーブが書かれます。commit 済みの後なので、flush で失敗してもシーン切替は巻き戻らず、失敗は `effect_errors` として報告されます。初出: [第4章 commit したのに作用が失敗したとき](04_scene_ecs.md#commit-したのに作用が失敗したとき)。

**participant(参加者)** — 1 回のシーンロードで状態を遷移させる**当事者**のこと(ECS・レンダラ・物理・ゲームロジック・永続化など)。誰がどの段で何をしたかを行にした `sceneParticipantManifest` があり、テストと診断が「取りこぼした参加者」を名指しできます。初出: [第13章 §13.13](13_editor.md#1313-シーン遷移とゲームロジックの結果wp378380b380e381381a)。([SceneParticipantManifestEntry](../../src/core/loader/sceneparticipantmanifest.hpp#L36))

**codec(component codec)** — コンポーネントの**受理仕様**。JSON ↔ canonical ↔ runtime の 3 層を 1 つの表で往復させ、組み込みコンポーネントについては未知キーを拒否します(closed schema)。エディタが値を書き戻すと codec の正規形に整形されます。codec を持たない名前のコンポーネントは、保持はされますが編集面では扱えません。初出: [第12章 §12.5](12_codemap.md#125-データ--コード対応表json-はどこで読まれるか)。([ComponentCodecState](../../src/core/loader/componentcodec.hpp#L49))

**EntityId / generation(世代)** — ECS の entity ハンドルは `index + generation` の組です。index を再利用するとき generation を進めるので、古いハンドルは「同じ index の別 entity」に**誤接続しません**。generation が上限に達したスロットは再利用せず永久に退役させます(低確率の取り違えを許容しない)。初出: [第4章 §4.4](04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け)。([EntityId](../../src/core/userpublic/details/ecs/entity.hpp#L12))

**free list(空きリスト)** — 解放した index を貯めておき、次の生成で再利用するための列。pelican2 では後入れ先出しで取り出すので、**同じ操作列なら同じ再利用順**になります(決定性のため)。初出: [source-code-guide 第4章 free list](../source-code-guide/04_ecs_deep_dive.md#free-list)。([coretemplate.hpp:121](../../src/core/userpublic/details/ecs/coretemplate.hpp#L121))

**arena(アリーナ)** — 同種のものをまとめて置くエンジン側の置き場。behavior は ECS コンポーネントではなく、エンジン所有の `BehaviorAttachmentArena` に連番で並びます。ゲーム DLL の型をエンジンの ECS チャンクに載せずに、ホットリロード時の世代交代(インスタンス状態は失われ、params は authoring 値から再構築)とエディタからの付け外しを安全にするための置き場です。具体的には、エンジン側の 1 本の `std::vector<Attachment>` がすべての behavior 実体を持ち、ゲームコードは handle 経由でしか触りません([behaviorarena.hpp:193](../../src/core/gamelogic/behaviorarena.hpp#L233))。「arena」という呼び名の由来は設計文書に書かれておらず**未確認**です(この用語集では「置き場」の意味だけで使います)。初出: [第8章 §8.5.5](08_gameplay.md#855-実行順とライフサイクル決定性)。([BehaviorAttachmentArena](../../src/core/gamelogic/behaviorarena.hpp#L213))

**SoA / Chunk / Archetype** — ECS の格納形。**Archetype** は「同じコンポーネントの組み合わせを持つ entity の集合」、**Chunk** はそれを一定個数ずつ区切った箱、**SoA**(Structure of Arrays)は「コンポーネントごとに別の連続配列に置く」並べ方です。1 つのコンポーネントだけを走査するときに無駄なメモリを読まずに済みます。初出: [第4章 §4.4](04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け)、詳細は [source-code-guide 第4章](../source-code-guide/04_ecs_deep_dive.md#chunkはsoa)。([ECSComponentChunk](../../src/core/userpublic/details/ecs/chunk.hpp#L19))

**type erasure(型消去)** — テンプレートで知っていた型情報を、**サイズ・アライメント・関数ポインタの表**で表して、実行時に任意の型を同じコードで扱えるようにする手法。ECS の列は `void*` + 「構築 / 破棄 / 詰め替え」の関数ポインタで持たれています。初出: [source-code-guide 第4章](../source-code-guide/04_ecs_deep_dive.md#runtimeの型消去メタデータ)。([VariedArray](../../src/core/userpublic/details/ecs/chunk.hpp#L25))

**relocate(詰め替え)** — entity を削除したときに、末尾の要素を空いた位置へ移して列を詰める操作(swap-and-pop)。型消去された列では move 構築 + 破棄を 1 つの関数ポインタにまとめてこれを行います。**要素のアドレスが動く**ので、コンポーネントへの生ポインタをフレームをまたいで持つのは危険です。初出: [第4章 §4.4](04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け)。

**MutationScope** — ECS の構造変更(entity / コンポーネントの追加・削除)の**再入を禁止する**ガード。構造変更の途中でさらに構造変更を始めると、半端に適用された状態が観測されうるため、`std::logic_error("ECS structural mutation is not reentrant")` で弾きます。初出: [第4章 §4.4](04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け)。([MutationScope::MutationScope()](../../src/core/userpublic/details/ecs/coretemplate.cpp#L297))

**SlotMap** — 「index + generation のハンドルで引く配列 + 空きリスト」という一般的なデータ構造。pelican2 に `SlotMap` という名前の型はありませんが、`ModelInstanceId`(index + generation + scene_epoch)が同じ形です。初出: [source-code-guide 第10章 §10.3](../source-code-guide/10_background_knowledge.md#世代付きハンドルindex--generationと-slotmap)。([ModelInstanceId](../../src/core/renderer/modelinstance.hpp#L11))

**hazard(ハザード)** — 「同じものを読み書きする者どうしが同時に走ると壊れる」関係。2 か所で出ます: ECS ではシステムの read / write 宣言から実行順を自動で決めるための競合関係、フレームグラフではノード間のリソース依存から barrier を導くための関係です(`hazard_stress` は後者をわざと厳しくする計画プロファイル名)。初出: [第8章 §8.3](08_gameplay.md#83-ゲームシステムの登録と実行順序wp43)。([ECSHazardPolicy](../../src/core/userpublic/details/ecs/coretemplate.hpp#L35) / [targetplanning.hpp:257](../../src/project/targetplanning.hpp#L257))

**CRTP(奇妙に再帰したテンプレートパターン)** — 自分自身を基底クラスのテンプレート引数へ渡す書き方(`class Foo : Base<Foo>`)。仮想関数なしで「型ごとに違う実体」を作れるので、pelican2 ではモジュールの静的インスタンス生成に使われています。初出: [source-code-guide 第10章 §10.3](../source-code-guide/10_background_knowledge.md#crtp奇妙に再帰したテンプレートパターンによる強い型付け)。

---

## 14.4 ゲームロジックの語

**behavior(オブジェクト behavior)** — シーンの**オブジェクトに貼り付ける**ゲームコードの単位。`behavior` コンポーネントに型名と params を書くと、ゲーム DLL 側に登録された C++ クラスが実体化されます。もう一つの書き方であるゲームシステム(世界全体を回す)との使い分けは §8.5.7 にあります。初出: [第8章 §8.5](08_gameplay.md#85-オブジェクト-behavior--もう一つのゲームコードの書き方wp155162167)。

**GameContext** — ゲームコードがエンジンに触るための**公開ファサード**(唯一の窓口)。状態を持たず、毎フレーム作られて渡されます。入力・カメラ・物理クエリ・音・保存・乱数などがここに集まっています。初出: [第8章 §8.4](08_gameplay.md#84-gamecontext-api-一覧)。([GameContext](../../src/core/userpublic/gamecontext.hpp#L284))

**order(順序値)と総順序** — ゲームシステムの実行順を決める整数。同じ `order` のシステムは**名前の辞書順**で並ぶので、全システムに一意な順序が決まります(= 総順序)。これが決定性の土台の 1 つです。初出: [第8章 §8.3](08_gameplay.md#83-ゲームシステムの登録と実行順序wp43)。

**epoch anchor** — ゲーム DLL が必ず import する**目印のシンボル**(`pelican_game_logic_epoch_v2`)。ロードの前に DLL の import テーブルだけを読み、この目印が無ければ「古い DLL」として `game_logic_abi_stale@1` で拒否します。登録を持たない provider 専用 DLL にはリンカが強制する要素が無いので、この診断が唯一の門番です。世代番号は**シンボル名そのものへの文字列連結**で入ります —— マクロが `pelican_game_logic_epoch_v` に ABI 版を貼り付けるので、版が 3 になればシンボルは `pelican_game_logic_epoch_v3` に変わり、`_v2` を import する古い DLL は名前が一致しなくなります([gamelogic.hpp:10](../../src/core/userpublic/gamelogic.hpp#L10))。初出: [第8章 ゲーム DLL の ABI](08_gameplay.md#ゲーム-dll-の-abiwp381381a382a)。([gamelogic.hpp:23](../../src/core/userpublic/gamelogic.hpp#L23))

**shadow copy(シャドウコピー)** — 実行中の DLL は Windows では上書きできないため、**原本の複製を作ってそちらをロードする**手口。pelican2 は原本の隣の `.pelican-hot-reload/<プロセス ID>/` へ複製するので、同じプロジェクトを 2 つ起動しても取り合いになりません。初出: [第8章 ゲーム DLL の ABI](08_gameplay.md#ゲーム-dll-の-abiwp381381a382a)。([gamelogicreload.cpp:423](../../src/core/gamelogic/gamelogicreload.cpp#L501))

**PCG32(permuted congruential generator)** — 高速で周期が長い擬似乱数生成器の一種。pelican2 は `std::mt19937` + `std::uniform_*` を**使わず**(処理系によって出る列が違うため)、PCG32 を分布まで自前実装しています。既定シードは 0 固定で、毎回変えたいゲームは自分で種を入れます。初出: [第8章 §8.10](08_gameplay.md#810-決定性--なぜ毎回同じ結果になるのか)。([deterministicrng.cpp:14](../../src/core/userpublic/deterministicrng.cpp#L14))

**provider(プロバイダ)** — 機能の実装やデータを供給する、差し替え可能な実装です。物理では組み込み実装や Jolt をビルド設定で選べるほか、ゲーム DLL が実行時にプロバイダを登録できます。クエリごとに必要な機能（capability）を見て、対応するゲーム側プロバイダを優先し、なければビルドで選んだ実装を使います。結果の整列や同順位の処理はエンジン側の規則に従いますが、実装を替えても数値結果が完全一致するという意味ではありません。描画でもビューや描画項目を供給する実装にこの語を使います。 初出: [第8章 §8.8](08_gameplay.md#88-物理クエリとトリガーp1p2wp107179)。([ProviderV2](../../src/core/userpublic/physics/abi_v2.hpp#L91))

**GJK / EPA** — 凸形状どうしの当たり判定の定番アルゴリズム。GJK は 2 つの形状の差(ミンコフスキー差)の中で原点までの最近点を反復で探し、**離れているか / 距離はいくらか**を出します。貫通しているときは GJK だけでは深さが出ないので、EPA(expanding polytope algorithm)が GJK の残した単体を膨らませて押し出す深さと向きを求めます。pelican2 は両方を自前で実装しており、GJK 反復は 40 回上限です。初出: [source-code-guide 第5章 §5.7](../source-code-guide/05_gameplay_and_services.md#57-physics)。([physquerysweep.cpp:20](../../src/core/phys/physquerysweep.cpp#L20) / [penetration()](../../src/core/phys/physquerysweep.cpp#L657))

**MTD(minimum translation distance)** — 既に食い込んでいる 2 つの形状を、**最小の移動量で引き離す**ベクトル。`shapeCast`が初期重なりを返したとき、`normal * penetration_depth`がこれにあたります。ただしmeshの`approximate_depenetration`付き結果は、めり込みを解消する補正であり、最短とは限りません。同点になったときは「移動の逆向きを優先し、その後 world x/y/z」で固定して決定性を保ちます。初出: [第8章 §8.8](08_gameplay.md#88-物理クエリとトリガーp1p2wp107179)。

**matrix palette(マトリクスパレット)** — スキニングで使う**ボーン行列の配列**。頂点は「どのボーンをどれだけ参照するか」を持ち、シェーダはこの配列から行列を引いて頂点を変形します。pelican2 では glTF の skin ごとの範囲が結合パレットを過不足なく覆うことを検証します。初出: [source-code-guide 第3章](../source-code-guide/03_project_and_loading.md)。([buildSkinPalette()](../../src/core/animation/animationjobs.cpp#L403))

---

## 14.5 描画とレイトレーシングの語

**RT(render target)** — 描画結果を書き込む画像。rendering config の `render_targets` で名前・サイズ(`extent_scale` か固定 `width`/`height`)・format を宣言し、パスが `input` / `output` で名前参照します。文中の「display RT」は最終的に画面へ出す RT のことです。初出: [第6章 render_targets](06_rendering.md#render_targets)。

**swapchain(スワップチェーン)** — ウィンドウへ実際に表示される画像の列を、GPU とウィンドウシステムの間で回す仕組み。pelican2 では `"swapchain"` は**予約名**で、RT 名として書くとエラーになります。初出: [第6章 render_targets](06_rendering.md#render_targets)。([SwapchainFrameTarget](../../src/core/vkcore/swapchainframetarget.hpp#L9))

**format_class** — RT の実 format を直接書かず、**役割**で書く指定(`scene` / `display` / `data` / `explicit(...)`)。`scene` は HDR 時に float16・SDR 時に 8bit sRGB へ、`display` は 8bit sRGB へリゾルバが解決します。feature の有無で HDR 化を切り替えられるのはこの層があるからです。初出: [第6章 §6.3](06_rendering.md#63-カラーパイプラインwp7274)。

**SRGB / リニアワークフロー** — sRGB は「表示用に非線形な曲線をかけた」色の入れ物、リニアは「光の量に比例した」値。pelican2 は**シェーダの入出力をすべてリニア**にし、sRGB への変換はハードウェア(`*_SRGB` view)か終端の `output_transform` ノードの 1 か所だけで行います。シェーダに `pow(x, 2.2)` を書くと二重変換になります。初出: [第6章 §6.3](06_rendering.md#63-カラーパイプラインwp7274)。

**G-buffer** — 「色を最後に決めるために、位置・法線・材質などを**いったん複数の RT へ書き出す**」方式(deferred shading)の、その中間 RT 群のこと。pelican2 では G-buffer の構成はエンジンが固定せず、`.surface` の material output ABI で任意に宣言できます。初出: [第6章 任意 G-buffer](06_rendering.md#任意-g-buffer--material-output-abiattachment-statewp218219)。

**stem(ステム)** — シェーダを**拡張子なしの名前**で参照する規約(`"shaders/lava"`、`"engine://ssao"`)。解決順は `<stem>.vert`(GLSL ソース → 実行時コンパイル)→ `<stem>.vert.spv`(ビルド済み SPIR-V)で、拡張子を書くとエラーです。この規約は web プロファイルと共有される可搬形式です。初出: [第6章 stem 参照](06_rendering.md#stem-参照)、web 側は [第9章 §9.4](09_web.md#94-シェーダ-stem-規約--可搬シェーダの書き方)。

**SPIR-V / SPIR-V reflection** — SPIR-V は Vulkan が読むシェーダの中間バイナリ形式。**reflection**(リフレクション)は、そのバイナリを解析して「どの set / binding に何型のリソースが要るか」を取り出すことです。pelican2 は spirv-reflect で解析し、descriptor set layout・pipeline layout・push constant range を**全自動生成**します。初出: [第6章 自動リフレクションとキャッシュ](06_rendering.md#自動リフレクションとキャッシュ)。([ShaderCompiler](../../src/core/shader/shadercompiler.hpp#L75))

**descriptor set** — シェーダが使うリソース(テクスチャ・バッファ)をまとめて束ねた単位で、番号(set 0, 1, …)で区別します。pelican2 は set の役割を規約で固定しており(フレーム共通・パス・マテリアルなど)、シェーダ側は番号を直接書かずに規約の定数を使います。初出: [第6章 descriptor set 規約](06_rendering.md#descriptor-set-規約shader_contractmd-が正)。([pelican_sets.hpp:7](../../src/core/shader/pelican_sets.hpp#L15))

**feature** — GPU 機能を「JSON の断片 + シェーダ断片」で足す**ユーザー空間の**単位。rendering config の `features[]` に参照を書いたときだけ存在し、書かなければ機構ごと素通りします(パージ可能)。エンジン同梱の feature も特権は持たず、`engine://features/*.json` をコピーして改造するのが公式ワークフローです。初出: [第6章 §6.5](06_rendering.md#65-feature--パージ可能な-gpu-機能wp2831548788104226)。

**canonical anchor(標準アンカー)** — feature がパスを差し込む先として、常に実体化される 8 つの目印ノード(`sprite → post_main → tonemap → post_ldr → pelican_ui → debug_draw → debug_text → imgui`)。この**全順序はエンジンの契約**で並べ替えられませんが、あいだに挿すのは自由です。初出: [第6章 canonical anchor](06_rendering.md#canonical-anchorwp73)。

**frame graph(フレームグラフ)** — パスの `input` / `output` から依存を自動で導き、実行順・並び・barrier をエンジンが決める仕組み。手書きの barrier は禁止で、JSON の宣言順は依存が許す範囲での**安定タイブレーク**にすぎません。「グラフ」の中身は、**ノード** = パスや compute タスク、**辺** = 「ある RT を書く側 → それを読む側」の依存で、プランナはこれをトポロジカルに並べて `nodes`(実行順)・`levels`(依存が無く同じ段に置けるノードの束)・`barriers` を出します([FramePlan](../../src/core/renderingpass/frameplanner.hpp#L168))。初出: [第6章 §6.6](06_rendering.md#66-フレームグラフf0f2--wp3335--wp64)。

**FramePlan** — フレームグラフを解決した結果の**実行計画**。ノード種別は `render` / `compute` / `anchor` / `snapshot_copy` / `output_transform` で、全構成がこの順序で実行されます(旧来の「JSON 宣言順で実行」経路は撤去済み)。RPC の `get_frame_plan` や ImGui の plan viewer で中身を見られます。初出: [第6章 §6.6](06_rendering.md#66-フレームグラフf0f2--wp3335--wp64)。([FramePlan](../../src/core/renderingpass/frameplanner.hpp#L168) / [FramePlanNodeKind](../../src/core/renderingpass/frameplanner.hpp#L16))

**logical plan / physical plan(論理プラン / 物理プラン)** — **論理プラン**はリソースを「値と読み取り範囲」だけで表したデバイス非依存の表現、**物理プラン**はそこへ実フォーマット・サンプル数・scope・view 実行を載せたデバイス依存の表現です。「何を計算するか」と「Vulkan でどう置くか」を分ける層で、物理プランは丸ごと差し替え可能なパッケージとして eject できます。違いは型の欄で見るのが早く、論理側の 1 リソースは `name` / `type` / materialization の 3 項だけ([LogicalResourceDesc](../../src/project/logicalrendergraph.hpp#L60))、物理側の同じリソースにはそこへ `format` / `rasterization_samples` / `view_layout` / `mip_levels` / `array_layers` / alias 可否が付きます([VulkanPhysicalResourcePlan](../../src/project/targetrenderplanning.hpp#L208))。例えば `format_class: "scene"` は論理側では文字どおり「scene 用の色」でしかなく、実 format(HDR なら `R16G16B16A16_SFLOAT`、SDR なら `B8G8R8A8_SRGB`)を欄として持つのは物理側です([第6章 §6.3](06_rendering.md))。初出: [第6章 target planning と Vulkan plan pin](06_rendering.md#target-planning-と-vulkan-plan-pin)。([CompiledLogicalRenderGraph](../../src/project/logicalrendergraph.hpp#L258) / [VulkanTargetPlan](../../src/project/targetrenderplanning.hpp#L488))

**scope(スコープ)** — 物理プランの上で、**1 回の dynamic rendering を開いたまま連続して記録するノードの束**。コマンド記録のループは「scope → 実行(view) → その scope の全ノード」の順(scope-execution-major)で、「view ごとに全ノード」ではありません。`scope_edit_mode` はこの束を編集するときにどこまで崩してよいかの指定です。初出: [第6章 Vulkan physical fragment](06_rendering.md#vulkan-physical-fragment-v1--v2--v3)、詳細は [source-code-guide 第6章](../source-code-guide/06_rendering_vulkan_shader.md)。

**view family(ビューファミリ)** — 「同じ内容を複数の視点で描く」ときの、その**視点の束**。XR の左右眼、シャドウのカスケード、キューブキャプチャの 6 面などが該当します。`$main` の family は物理プランが view 数まで決め、secondary family は実行時に数が決まる、という非対称があります。初出: [第6章 §6.12](06_rendering.md#612-openxr-ステレオレンダリングwp125138--wp203ac実装済み)。([RenderViewFamily](../../src/core/renderer/viewfamily.hpp#L76) / [ViewFamilyV1](../../src/core/userpublic/render/render_strategy_abi_v1.hpp#L91))

**`.surface`** — マテリアルの**シェーダとパラメータ宣言**を 1 ファイルにまとめた自己記述コンテナ。値のほうは別の `*.material.json` に書きます(2 ファイル方式)。同梱の standard / toon / OpenPBR も特権なしの `.surface` として実装されています。初出: [第6章 .surface](06_rendering.md#surface--自己記述コンテナ)。([SurfaceShaderComposition](../../src/core/shader/surfacecompiler.hpp#L54))

**SSAO(screen-space ambient occlusion)** — 深度と法線から「くぼみの暗さ」を画面空間で近似する定番手法。pelican2 では feature ではなく**同梱シェーダ**(`engine://ssao` / `engine://ssao_blur`)で、プロジェクトの rendering config が自分でパスを組みます。初出: [第6章 §6.4](06_rendering.md#64-シェーダ基盤shader-freedom-kit-wp101519--m2bwp70)。([ssao.frag](../../src/core/resources/ssao.frag))

**bloom(ブルーム)** — 明るい部分をにじませて眩しさを表現する後処理。P-2a-1b(`6e04c7c8`)から同梱 feature `engine://features/bloom.json`(13 pass + 9 RT を `post_ldr` の後に挿入。shader は `bloom_highpass` / `bloom_blur_h` / `bloom_blur_v` / `bloom_upsample` / `bloom_composite`)。しきい値・膝・強度は feature の parameter(`threshold` / `knee` / `intensity`、P-2a-1c)ですが、**供給のされ方が 2 通りある**点に注意してください: `threshold` と `knee` は shader の define(`PELICAN_FEATURE_BLOOM_*`)として焼かれ、**`intensity` だけは `binding: "buffer"`**(P-2a-2、`bloom.json:26`)—— feature 自身が持つ `bloom.intensity` バッファ(`size: 4` / `host_source: runtime_f32`)を通る**実行時の値**で、`set_parameters` でも共通グラフの `render.scalar` sink でも毎フレーム書き換えられます。初出: [第6章 §6.4](06_rendering.md#64-シェーダ基盤shader-freedom-kit-wp101519--m2bwp70)。([bloom_highpass.frag](../../src/core/resources/bloom_highpass.frag))

**materialize(マテリアライズ)** — エンジン同梱の feature(`engine://features/*.json`)を**プロジェクト側の派生としてファイルに保存し、以後そちらを参照する**操作。以後は自分のものなので自由に編集できます([第1章](01_overview.md) 設計決定 11)。`parameters` の宣言は**残したまま**、確定した値が `bound_parameters` として書かれます —— **宣言は消えません**。`$name` の置換とスカラーの define は合成のたびに再生成されるので、古い値の define が残ることもありません([featurecompose.cpp:2850](../../src/project/featurecompose.cpp#L3334) のコメントが理由の正本)。値の優先順位は **instance の上書き > `bound_parameters` > 宣言の既定値**([featurecompose.cpp:1393](../../src/project/featurecompose.cpp#L1478))。同じ feature を 2 度 materialize することはできません(`is already materialized`)。初出: [第6章 §6.5](06_rendering.md#65-feature--パージ可能な-gpu-機能wp2831548788104226)。

**TAA(temporal anti-aliasing)** — 毎フレーム少しずらして描いた結果を、前フレームの履歴と混ぜてジャギーを消す手法。pelican2 では `taa.json` feature が resolve + composite の 2 パスと jitter provider を持ちます。履歴・速度・jitter はユーザー空間側の道具立てで、エンジンは機構だけを持ちます。初出: [第6章 標準 TAA](06_rendering.md#標準-taawp113)。

**projection jitter** — TAA のために、毎フレーム射影行列を**サブピクセル単位でずらす**こと。ずらす列は halton23 などの低食い違い数列で、`@history` の履歴 RT と組で使います。初出: [第6章 projection jitter](06_rendering.md#projection-jitterwp112115)。

**VAT(Vertex Animation Texture)** — 頂点の動きをテクスチャに焼き込んだアニメーション形式(布・破壊など、ボーンで表せない動きに使う)。pelican2 では glb の primitive の `extras["pelican.vat"]` にメタデータを置き、実体は glb 内の half-float bufferView です。再生は前後 2 フレームを `texelFetch` して**手動 lerp** します(ハードウェア線形フィルタは頂点方向へ滲むため禁止)。初出: [第5章 §5.4](05_assets.md#54-焼き込みアニメーション2--pelicanvat-v1wp20)。

**IBL(image-based lighting)** — 環境を写した画像から、拡散・鏡面の環境光を作るライティング手法。**pelican2 では未実装**です(WP32 は番号予約のみ)。同梱の `sky_ambient.json` は単色の環境光であって IBL ではありません。初出: [第11章 §11.1](11_status.md#111-wp-全台帳)。

**AS / BLAS / TLAS(加速構造)** — レイと形状の交差候補を高速に絞り込むためのデータ構造の総称が **AS** です。**BLAS**（bottom-level acceleration structure）は三角形群や手続き形状の境界ボックスをまとめた下位構造、**TLAS**（top-level acceleration structure）は BLAS の配置・変換行列などを持つ上位構造です。現在は**静的形状に加え、スキン・モーフ・VAT による変形形状にも対応**します。変形後の頂点を計算し、条件が合えば前回の BLAS を基に更新（refit）します。配置の transform だけが変わる場合は BLAS を再利用し、TLAS の行列へ反映します。詳しくは [加速構造の解説](../source-code-guide/11_ray_query_acceleration_structures.md)。 初出: [第6章 §6.15](06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v3-と-pelican_ray_instancert3-1--wp374374a377379383)。([accelerationstructure.hpp:69](../../src/core/vkcore/accelerationstructure.hpp#L125))

**SBT(shader binding table)** — レイトレーシングパイプラインで、「どのレイがどのシェーダ(raygen / miss / closest-hit)に当たるか」を引くための表。`ray_group_layout` の `ray_types` / `groups` / `misses` / `record` がこの表の形を決めます。`acceleration_structure` は SBT の入力ではないので、layout の digest からは取り除いて計算されます。初出: [第6章 §6.15](06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v3-と-pelican_ray_instancert3-1--wp374374a377379383)。([RayTracingSceneShaderBindingTables](../../src/core/shader/pipelinefactory.hpp#L187))

**`ray_group_layout`(ray layout)** — レイトレの宣言層の入口となる JSON。version は 3 だけを受理し(1 / 2 は MG-2a から `pelican.ray_group_layout.version_unsupported@1`)、**プロセスの実行中は変更できない 2 状態**(layout 無し / 有り)として起動時に確定します。layout があるときだけ `acceleration_structure` 宣言と `pelican_ray_instance` コンポーネントが使えます。プロセス途中でのモード変更は拒否されます。初出: [第6章 §6.15](06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v3-と-pelican_ray_instancert3-1--wp374374a377379383)。([RayLayoutRuntimeState](../../src/core/shader/raygrouplayout.hpp#L168))

---

## 14.6 テスト・運用・ツールの語

**JSON-RPC(stdio)** — 外部プロセスからエンジンを操作するためのプロトコル。1 行 1 メッセージの JSON を stdin / stdout でやり取りし(改行区切り JSON)、`--rpc` 中は **stdout がプロトコル専用**になります(ログは stderr へ)。エラーコードは JSON-RPC 2.0 の規約(`-32700` parse error など)に従います。初出: [第10章 §10.3](10_tools.md#103-json-rpcstdio-外部制御プロトコル実装済み)。([RpcServer](../../src/core/communication/rpcserver.hpp#L56))

**ホットリロード(hot reload)** — 実行を止めずに、保存されたファイルを反映すること。対象はシェーダ・`.surface`・テクスチャ・マテリアル値・モデルコンテナ・ゲームロジック DLL で、**シーンは対象外**です。1 つの変更に影響する全バリアントを準備してからフレーム境界で一括公開し、どれか 1 つでも失敗すれば全体を破棄して旧世代を維持します(fail-soft)。初出: [第10章 §10.5](10_tools.md#105-ホットリロードwp9096110147162178)。

**チケット(ticket)** — エディタの編集操作を**フレーム境界で適用する**ための受付票。受付時に前提条件を検査し、コミット時にもう一度検査します(受付から適用までの間に世界が動くため)。「トランザクション」という語がシーンロード・作用・編集の 3 か所で使われるので、編集の文脈ではチケットと呼び分けます。初出: [第13章 §13.3](13_editor.md#133-編集の実行モデル--チケットとフレーム境界)。([EditorCommandService](../../src/core/communication/editorcommandservice.hpp#L258))

**headless(ヘッドレス)** — ウィンドウを開かずに描画・実行するモード。時刻が固定ステップ(dt = 1/fps)になるためリプレイと golden テストの土台になり、CI から PNG を取り出せます。初出: [第2章 §2.5](02_getting_started.md#25-ヘッドレス実行ウィンドウなしで-png-を出す)。

**Job オブジェクト(Windows)** — 子プロセスを束ねて**まとめて終了させる**ための Windows のカーネルオブジェクト(POSIX のプロセスグループに相当)。pelican2 ではツールが起動した子プロセス(と孫)を取り残さないために使います。初出: [source-code-guide 第10章 §10.4](../source-code-guide/10_background_knowledge.md#windows-の-job-オブジェクトプロセスグループの対応物)。([processrunner.cpp:171](../../src/devcli/processrunner.cpp#L171))

**名前付き mutex(named mutex)** — 名前でプロセスをまたいで待ち合わせられる排他オブジェクト。`Global\` を付けるとマシン全体が範囲になります。pelican2 では GPU を使うテストを 1 本ずつ走らせる lease に使い、前の保持者が異常終了した場合(`WAIT_ABANDONED`)も取得成功として扱います(GPU が永久にロックされないため)。初出: [source-code-guide 第10章 §10.4](../source-code-guide/10_background_knowledge.md#windows-の名前付き-mutex-と-global-名前空間machine-wide-の相互排他)。([gpu_lease_run.cpp:157](../../test/gpu_lease_run.cpp#L229))

**doclink** — 文書中の `#L<行番号>` リンクが指す先を追跡し続けるためのツール(`tools/doclink.py`)。`update` で行番号を貼り直し、`check` で古くなったリンクを検出します。行番号はキャッシュで、本体は「その行に何が書いてあったか」というアンカーです。初出: [第10章 §10.10](10_tools.md#1010-リポジトリ保守スクリプトtools)。

---

## 14.7 共通グラフと姿勢の語

**共通グラフ(`pelican.graph` v1)** — **値(スカラー)・描画・姿勢を 1 つのノード語彙で繋ぐ**機構、およびその文書形式。文書は `definitions`(名前付きの部分グラフ)/ `nodes`(root のノード列)/ `outputs` を持ち、rendering config は同じ文書の `rendering` 節に同居します。「共通」は「1 本の曲線の値を GPU バッファにも姿勢のブレンド率にも送れる」という意味です。ノード語彙は [第6章 §6.17](06_rendering.md)、編集操作は [第13章 §13.5](13_editor.md)、凍結された op の一覧は [../v0_freeze.md](../v0_freeze.md) §1.3。初出: [第6章 §6.16](06_rendering.md)。([graphcomposition.cpp](../../src/project/graphcomposition.cpp))
※ 紛らわしい別物として **`pelican.value_graph`** があります。これは共通グラフを lowering した結果の**内部 IR + テスト面**であって、**外部に約束された契約ではありません**([../v0_freeze.md](../v0_freeze.md) §2 行 1)。プロジェクトが書くのは `pelican.graph` です。

**`graph.instance`(装着)** — 定義(`definitions` の 1 つ)を 1 回実体化するノード。`owner: {scene, object}` を書くと**シーンの個体に結び付き**、owner が解決できないときは**処理を停止するエラーにはせず、警告を出して非アクティブ（inactive）**になります。入力 port の値の決まり方は 4 通りで、これを **mode** と呼びます: `connected`(辺が繋がっている)/ `parameter`(`parameters` に書いた固定値)/ `default`(定義側の既定値)/ `override`(実行時に `setGraphParameter` で上書き)。優先は **辺 > `parameters` > 既定値**で、辺を外しても `parameters` は残るので**元の固定値に戻れます**。接続済みの入力に実行時上書きをかけると `graph.override_shadowed@1`(黙って消えません)。初出: [第4章 §4.1](04_scene_ecs.md#authoring-object-id-と-graphinstanceownerag-1ap-1p-2b)。

**姿勢グラフ / `pelican.pose`** — 共通グラフの姿勢用 module。op は `pose.clip_sample`(クリップを時刻でサンプルする)/ `pose.blend`(2 つの姿勢を weight で混ぜる)/ `pose.state_machine`(`pelican.anim_graph` 文書を包む)/ `pose.retarget`、sink は `pose.skeletal`。port を流れる `pelican.pose@1` は**バイト列ではなく**、staged pose のハンドル + レイアウト世代 + フレーム revision です。評価は描画ではなく**アニメーションのフェーズ**で走り、スカラー → 姿勢の**一方向**です(姿勢 → スカラーはありません)。`PELICAN_WITH_ANIMATION=OFF` のビルドには module ごと存在しません。初出: [第8章 §8.13](08_gameplay.md)・[第4章 §4.2](04_scene_ecs.md#姿勢の-2-つの装着様式ag-1aag-1b)。

**単一 writer / writer token** — 「1 つの個体の姿勢を書けるのは 1 本だけ」という規則と、その実行時の担保。文書レベルでは装着時に `pose.sink_conflict@1` で弾き、公開レベルでは `publishAnimationFrame` が **owner ごとの writer token**(フレーム revision 付き)を持ち、同じ revision に別の writer が来れば `authority_conflict`、同じ writer が 2 回来れば `duplicate_revision` を返します。文書の検査だけだと「検査をすり抜けた経路」が黙って二重書きになるため、公開処理でも書き込み元を検証します。初出: [第4章 §4.2](04_scene_ecs.md#姿勢の-2-つの装着様式ag-1aag-1b)。([PoseGraph::raiseSinkConflict()](../../src/core/animation/posegraph.cpp#L40))

**root motion** — クリップに焼かれた「根(root)の移動」を、姿勢としてではなく**個体の移動**として使うこと。`pose.clip_sample{root_motion: "extract", root_node}` で有効になり、移動量は owner の `localtransform` に加算され、staged pose 側の根はクリップ先頭の位置に固定(pin)されます。移動量は**巻き戻す前の時刻(unwrapped time)の差**で取るので、`speed < 0` でも 1 フレームに 1 周以上進んでも同じ 1 つの式です。`set_time`(シーク)は移動ではないと扱われ、その 1 フレームの移動量は 0 になります。**v0 では回転量を抽出しません**。末尾キーを先頭キーと同じにした「継ぎ目無し」クリップは正味の移動が 0 = 動きません。初出: [第4章 §4.2](04_scene_ecs.md#localtransform)・[第8章 §8.13](08_gameplay.md)。

**catalog(2 系統)** — 「今このビルドで何が使えるか」の一覧。**(1) グラフのカタログ** = 登録済み module / 型 / op / sink の語彙(`pelican_cli graph-catalog` と RPC `get_graph_catalog`。`PELICAN_WITH_ANIMATION` の ON / OFF で 5 module 36 op / 4 module 30 op と変わります。数は `PELICAN_WITH_AUDIO=ON` の build。GP-3 で常に登録される `pelican.ui` の 2 op、LP 系で `pelican.render` の 7 op が加わりました。構造の node の `graph.instance` / `graph.loop` は載りません)。**(2) feature のカタログ** = 使える render feature の一覧(RPC `list_render_features`。エンジン同梱と install 済み package が載り、`source` でどちらか分かります)。**(2) にプロジェクト内の素の `project://` feature は載りません**。初出: [第10章](10_tools.md)・[第3章 §3.11](03_project_format.md#311-package--本体ツリーの外から持ち込むx-1)。

**ループ(`graph.loop`)** — 共通グラフの**構造の node** で、body(definition)を集合の要素ごとに使う唯一のループ。`over`(集合)→ `where`(絞る)→ `order`(並べる)→ `limit`(先頭 N 個)→ 分岐(`branches`)の順に決まります。catalog に載らず、root の `nodes` にだけ置けます。保存されるのは parameters と body だけで、展開した pass(`<loop>/<element>/<node>`)は派生物です。初出: [第6章 §6.18](06_rendering.md)、設計は [../design_graph_loops.md](../design_graph_loops.md)。

**分岐(branch)** — ループの要素ごとに、どの body を使うかの条件と body の組。`branches` の上から順に `when` を見て最初に合ったものを使い、**最後の分岐は `when` 無しの else** でなければなりません。`"body": null` は「この要素では何もしない」。どの要素も選ばなかった分岐は `get_frame_plan` の `unmatched_branches` に出ます。初出: [第6章 §6.18](06_rendering.md)。

**carry** — ループの前の回の出力を次の回の入力へ渡す指定(`carry: [{from, to}]`)。最初の回はループの入力 edge の値を受けます。bloom の下り段のような「前の段の結果を縮めていく」連鎖に使い、expand の段だけで使えます。初出: [第6章 §6.18](06_rendering.md)。

**段(tier)—— expand / draw_loop** — ループが実行される場所。**expand** は組み直しのときに body を回の数だけ複写して、ふつうの pass・task・target として frame graph に渡す段(`over` が `range` か `view_axis`)。**draw_loop** は 1 つの material pass の中で、material ごとに描き方を選ぶ段(`over` が `draw_items`、body は `draw.*` だけ)。v0 では段が黙って変わらず、合わなければ `graph.loop_tier_unsupported@1` で断ります。初出: [第6章 §6.18](06_rendering.md)。

**draw step(描き方の 1 手)** — draw_loop の body で、1 つの material をどう描くかを 1 手ずつ並べる node(`draw.material` = そのまま / `draw.variant` = 名前付き variant で / `draw.override` = render state と define を替えた pipeline で)。`after` の edge で列になり、同じ range を 2 回以上描けます。override は新しい material を登録せず、世代が持つ別の表の pipeline で描きます。初出: [第6章 §6.18](06_rendering.md)。

**view の軸(view axis)/ `view_select`** — provider が作る view family の view が持つ、名前付きの整数の座標(`$shadow/directional` の `light` / `cascade`、`$capture/cube` の `face` など)。pass や task に `view_select: {<軸>: <値>}` を書くと、軸の値が一致する view だけで走ります。組み直し時に長さが決まる軸(`cascade`・`face`)はループの `over: {view_axis}` にも使えます。初出: [第6章 §6.18](06_rendering.md)。

**`render.target_ref`** — rendering 節・preset・feature が宣言した render target を、グラフから名前で指す node。target を作らず(所有しない)、つないだ port はその target の名前をそのまま使います。名前が合成後にあるかは組み直しのときに検査します(`graph.target_ref_unknown@1`)。初出: [第6章 §6.18](06_rendering.md)。

**`pass_replacements`** — feature の instance に書く `{"<feature の pass / task 名>": "<graph の node かループの id>"}`。書いた pass は feature から外れ、graph の node(ループなら展開した回の列)がその位置に入り、順序と `surface_resources` の名前も写ります。分解版で一体版の pass を置き換えるための仕組みです。初出: [第6章 §6.18](06_rendering.md)。

**分解版(decomposed graph)** — 標準 feature の処理を、グラフの node とループで書き直したもの(「標準機能には一体版と分解版の両方を持たせる」という原則の後者)。engine は `engine://graphs/bloom_loop.json` などを**雛形**として出荷し(文書の `decomposes` が、どの feature を外すか・置き換えるかを言う)、project がそれを自分の graph へ写して使います。rendering 節の material pass は `decompose_material_pass` で分解版に書き換えられます。初出: [第6章 §6.18](06_rendering.md)、[第13章](13_editor.md)。

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
| capability | あるビルド・実装・デバイスが利用できる機能。単にカタログに名前が載ることとは区別します。 |
| op（operation） | グラフのノードが行う操作の種類。JSON の `op` に `value.constant` などを指定します。 |
| port（ポート） / edge（辺） | ノードの値の出入口と、その接続。辺がデータや実行順序の依存を表します。 |
| sink（出力先） | グラフの評価結果を、姿勢や描画用バッファなどの外部の状態へ反映する出口。 |
| identity | 対象が同じものかを判別する識別情報。単なる表示名とは限りません。 |
| digest（ダイジェスト） | 内容から計算するハッシュ値。文書の変更検出などに使います。 |
| CAS（Compare And Swap） | 想定していた値と現在値が一致するときだけ更新する方式。編集 RPC では文書の digest を比較し、他の編集を上書きするのを防ぎます。CPU の原子命令を指す場合もあります。 |
| epoch / generation / revision | 状態の世代や変更回数を識別する値。何を更新したときに進むかは型ごとに異なり、相互に置き換えられません。 |
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
