# 第14章 用語集

対象: pelican2(HEAD=`d146b8bd`・2026-09-04、branch
`codex/render-target-runtime-slice`)/ このマニュアルはコードを正とする

## この用語集の使い方

- **複数の章で説明なしに出てくる語だけ**を集めた索引です。正確な仕様は各語の「初出」リンク先にあり、ここはその入口です。
- 五十音順ではなく**分野別**に並んでいます。語を探すときはブラウザの検索(Ctrl+F)を使ってください。
- エンジン固有の語には実装への行リンクを添えました。**説明と実装が食い違ったら実装が正**です(食い違いの台帳は [第11章 §11.3](11_status.md#113-既知の食い違い設計文書-vs-コード))。

---

## 14.1 全体・体制・品質の語

**WP(Work Package)** — 実装作業の単位で、`WP1`〜`WP386` の通し番号が振られます。本マニュアルの `✅WP155/162/167` のようなバッジは「その機能がどの WP で入ったか」を指します。番号ごとの状態の正は台帳 [../implementation_plan.md](../implementation_plan.md) で、本マニュアルの WP 表は転記です。初出: [第11章 §11.1](11_status.md#111-wp-全台帳)。

**台帳** — `docs/implementation_plan.md` のこと。未完了 WP の指示と、共通・運用規則が載ります(完了済み WP の逐語記録は `docs/implementation_archive.md` へ移されます)。章と食い違ったら台帳が正です。初出: [第11章 §11.1](11_status.md#111-wp-全台帳)。

**D0** — 「エディタに特権 API を作らない」という設計決定の記号。編集操作はまず JSON-RPC メソッドとして定義され、エンジン内蔵の ImGui インスペクタも外部の Python クライアントも Qt Studio も**同じ関数**を呼びます。Studio がエンジン内部へリンクしていないことはビルドレベルのテスト(`test/run_devstudio_boundary_negative.cmake`)で守られます。初出: [第13章 §13.1](13_editor.md#131-全体像--何ができるようになったか)。

**ABI(Application Binary Interface)** — 別々にコンパイルされたバイナリが呼び合うときの、関数名・引数の並び・構造体レイアウトの約束。pelican2 では「ゲーム DLL の ABI」(登録入口の名前と epoch anchor)、「physics provider ABI」、「material output ABI」など、**DLL や shader をまたぐ境界ごと**にこの語が出ます。初出: [第8章 ゲーム DLL の ABI](08_gameplay.md#ゲーム-dll-の-abiwp381381a382a)。([abi_v2.hpp:83](../../src/core/userpublic/physics/abi_v2.hpp#L83))

**決定的 / 決定性(deterministic)** — 同じビルド・同じプロジェクト・同じ入力なら、何度実行しても**同じ結果**になること。pelican2 ではフレームループの固定順序・入力のフレーム頭スナップショット・システム順(order → 名前辞書順)・イベントの次フレーム配送・物理のタイブレーク・固定シードの PCG32・ヘッドレスの固定ステップ(dt = 1/fps)の 8 要素で作ります。ウィンドウモードは実測 dt なので厳密なリプレイはヘッドレスで行います。初出: [第8章 §8.10](08_gameplay.md#810-決定性--なぜ毎回同じ結果になるのか)。

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

**エンベロープ** — JSON の先頭に置く `{"schema": "pelican.scene", "version": 1, ...}` の 2 キーのこと。封筒の中身(本体)を読む前に、**形式名と版が一致するかだけ**を先に見ます。不一致は hard error です。初出: [第4章 エンベロープの規則](04_scene_ecs.md#エンベロープの規則)。([projectformat.hpp:14](../../src/project/projectformat.hpp#L14))

**closed schema / open schema** — closed schema は「宣言されたキー以外が来たら拒否する」受理仕様、open schema は「知らないキーは無視して通す」受理仕様です。エンジンの組み込みコンポーネントは closed schema(未知キーを拒否)で、ユーザー定義コンポーネントは codec を持たないため扱いが変わります。初出: [第12章 §12.5](12_codemap.md#125-データ--コード対応表json-はどこで読まれるか)。

**opaque(web)** — web ビューア側で、codec を持たない名前のコンポーネントを**検証せずそのまま通す**扱いのこと。web が closed schema を持つのは組み込み 7 コンポーネントだけなので、`pelican_ray_instance` や `List` フィールドを書いたシーンは「native では拒否されるのに web では黙って通る」状態です。web リポジトリは pelican2 の外にあるため、この点はコードで裏取りできていません(**未確認**)。初出: [第3章 §3.8](03_project_format.md#38-既知の食い違い設計文書-vs-実装)。

**宣言 schema(v2 / v3)** — behavior やユーザー定義コンポーネントの params を「型付きで宣言する」仕組みの版。v3 で `Flags` / `List` / `OptionalOverride` の**再帰的な**入れ子が書けるようになりました。再帰 schema を使う型があると、SDK は自動的に `_v3` 登録入口を選びます。初出: [第3章 §3.10](03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a)。([SchemaNode](../../src/core/userpublic/details/schema/structfieldschema.hpp#L155))

**`Flags`** — 宣言済みの名前の集合を、JSON では**文字列の配列**で書くフィールド型。合成則は OR ではなく**置換**です(上書き側の配列がそのまま新しい値になる)。初出: [第3章 §3.10](03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a)。([Flags](../../src/core/userpublic/details/schema/structfieldschema.hpp#L194))

**`List<Row>` / 行封筒** — 可変長の行の並びを持つフィールド型。JSON では素の配列ではなく `{"$list": ..., "rows": [...]}` という **object コンテナ**で書きます。素の配列にしないのは、行の版や由来を後から足せるようにするためです。初出: [第3章 §3.10](03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a)。([List](../../src/core/userpublic/details/schema/structfieldschema.hpp#L222))

**wire** — 「ディスク / RPC に出る形」のこと。同じ値でも、C++ の構造体(runtime)・正規化した中間形(canonical)・JSON に出る形(wire)は別物で、codec がこの 3 者を往復させます。「wire モデル」は「JSON に出る形の設計」の意味です。初出: [第3章 §3.10](03_project_format.md#310-コンポーネント宣言スキーマ-v3--flags-と-list-の-wirewp382a)。

**解決(resolve)** — 書かれた参照文字列(`project://`、`engine://`、`user://`、素の相対パス、`#フラグメント`)を**実ファイルの絶対パスへ確定させる**操作。cwd は使いません。担当は `PathResolver` で、プロジェクトルートの外へ出る参照は脱出防止で弾かれます。初出: [第3章 §3.3](03_project_format.md#33-パス参照と-pathresolver)。([PathResolver](../../src/core/loader/pathresolver.hpp#L9) / [ProjectPathResolver](../../src/project/projectpathresolver.hpp#L69))

**結合(binding)** — 解決で得た実体を、**実行時のオブジェクトへ繋ぐ**操作。「形式の解析(`src/project`、GPU 不要)」と「実行時の結合(`src/core`)」を分けるのが pelican2 の二段構えで、`src/project` は純粋なパーサに保たれます。入力の `binding` は別語で、こちらは「デバイスの操作 → アクション名」の対応表です([第7章 §7.2](07_input_ui.md#72-アクション定義とバインディングプロファイル))。初出: [第3章 §3.5](03_project_format.md#35-解釈レイヤ--pelican_project-とバインダの分離)。

**asset store** — アセットの**置き場所を間接化**する仕組み。`project.json` に論理的なマウント(`assets/**` → store)を宣言し、マシンごとの実パスは git に入れない `.pelican/local.json` で上書きします。バイナリを VCS に入れずに参照を共有するための層です。初出: [第3章 §3.4](03_project_format.md#34-asset-store--置き場所の間接化実装済みwp55)。

**フラグメント参照(`#`)** — `models/town.glb#Building_02` のように、**コンテナ内の 1 要素**を指す参照。解決の過程でフラグメントを落とさずに運ぶのが規約です。初出: [第5章 §5.6](05_assets.md#56-フラグメント参照とアセットコンテナ実ロード対応wp55777984)。

**authoring / runtime** — 同じシーンの 2 つの表現。**authoring** は編集の正本(`AuthoringSceneDocument`)で、オブジェクトの順序や `AuthoringObjectId` を保ち、rename / reorder しても参照が切れないようにします。**runtime** はエンジンが実際に動かす ECS 側の姿です。エディタは authoring を編集し、その結果が runtime へ投影されます。初出: [第3章 §3.9](03_project_format.md#39-シーンドキュメントと保存エディタ経路wp149158166168)。([AuthoringSceneDocument](../../src/core/loader/authoringscenedocument.hpp#L90))

**3 段合成** — `basic_config` の値を「エンジン既定 → `project.json` → `.pelican/local.json`」の 3 段で重ねて決めること。deep merge ではなく**パス単位で問い合わせる**方式なので、どの段が勝ったかを後から言えます。初出: [第3章 basic_config と「3 段合成」](03_project_format.md#basic_config-と3-段合成)。

---

## 14.3 ロードと ECS の語

**scene load transaction(シーンロードのトランザクション)** — 新しいシーンを試験的に組み立ててから**一斉に切り替える**方式。段は `prepare → early_publish → publish → activate → commit_metadata → retire → flush → finish` の 8 つ固定で、commit 前に失敗すれば全員で巻き戻します。「トランザクション」という語は編集チケット・effect transaction にも使われるので、文脈に注意してください。初出: [第4章 §4.3](04_scene_ecs.md#43-シーンロードの流れ)。([SceneLoadPhase](../../src/core/loader/sceneload.hpp#L27))

**prepare / publish / commit** — pelican2 全体で繰り返される 3 点セット。**prepare** で「投げうる準備(確保・コピー・検証)」を全部済ませ、**publish** は失敗しようのない差し替えだけにし、**commit** で外から見える状態を確定させます。publish が no-throw なので「半分だけ適用された状態」が原理的に作れません。初出: [第4章 §4.3](04_scene_ecs.md#43-シーンロードの流れ)。

**retire(退役)** — 旧シーンを片付ける段。ここで旧オブジェクトの `onDestroy` が走り、旧 entity は既に unpublish 済みなので**新シーンからは見えません**。retire 中に新たなシーンロードを要求しても拒否されます。初出: [第4章 §4.3](04_scene_ecs.md#43-シーンロードの流れ)。([scene.cpp の requestLoadDuringRetire](../../src/core/loader/scene.cpp#L1193))

**staging(ステージング)** — 「作用を実行せず、実行可能な形にして取っておく」こと。ロード中に `playSound` や `saveData` を呼ぶと、その場では鳴らず・書かず、**実行に必要な準備だけ済ませた entry** がトランザクションに積まれます。溜めた分が実際に外へ出るのは、ロードの 8 段のうち **flush** 段(`commit` に成功したときだけ)です。具体例: `onInit` で `playSound("bgm")` を呼ぶと、その場では voice slot の予約だけが起き、音が鳴るのは flush 段です(ロードが失敗すれば予約ごと消え、鳴りません)。初出: [第4章 ロード中の「作用」](04_scene_ecs.md#ロード中の作用はトランザクションに溜まるwp380be)。([PreparedEffectTransaction](../../src/core/loader/effecttransaction.hpp#L27) / [SceneLoadPhase](../../src/core/loader/sceneload.hpp#L27))

**outbox** — 溜めた作用を入れる固定長のバッファ。容量は 256 で、溢れた分は「溢れた」という 1 件のエラーに畳まれます(黙って捨てません)。初出: [第4章 ロード中の「作用」](04_scene_ecs.md#ロード中の作用はトランザクションに溜まるwp380be)。([effecttransaction.hpp:25](../../src/core/loader/effecttransaction.hpp#L25))

**flush** — 溜めた作用を**実際に実行する**段(8 段の 7 番目)。ここで初めて音が鳴り、セーブが書かれます。commit 済みの後なので、flush で失敗してもシーン切替は巻き戻らず、失敗は `effect_errors` として報告されます。初出: [第4章 commit したのに作用が失敗したとき](04_scene_ecs.md#commit-したのに作用が失敗したとき)。

**participant(参加者)** — 1 回のシーンロードで状態を遷移させる**当事者**のこと(ECS・レンダラ・物理・ゲームロジック・永続化など)。誰がどの段で何をしたかを行にした `sceneParticipantManifest` があり、テストと診断が「取りこぼした参加者」を名指しできます。初出: [第13章 §13.13](13_editor.md#1313-シーン遷移とゲームロジックの結果wp378380b380e381381a)。([SceneParticipantManifestEntry](../../src/core/loader/sceneparticipantmanifest.hpp#L32))

**codec(component codec)** — コンポーネントの**受理仕様**。JSON ↔ canonical ↔ runtime の 3 層を 1 つの表で往復させ、組み込みコンポーネントについては未知キーを拒否します(closed schema)。エディタが値を書き戻すと codec の正規形に整形されます。codec を持たない名前のコンポーネントは、保持はされますが編集面では扱えません。初出: [第12章 §12.5](12_codemap.md#125-データ--コード対応表json-はどこで読まれるか)。([ComponentCodecState](../../src/core/loader/componentcodec.hpp#L48))

**EntityId / generation(世代)** — ECS の entity ハンドルは `index + generation` の組です。index を再利用するとき generation を進めるので、古いハンドルは「同じ index の別 entity」に**誤接続しません**。generation が上限に達したスロットは再利用せず永久に退役させます(低確率の取り違えを許容しない)。初出: [第4章 §4.4](04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け)。([EntityId](../../src/core/userpublic/details/ecs/entity.hpp#L12))

**free list(空きリスト)** — 解放した index を貯めておき、次の生成で再利用するための列。pelican2 では後入れ先出しで取り出すので、**同じ操作列なら同じ再利用順**になります(決定性のため)。初出: [source-code-guide 第4章 free list](../source-code-guide/04_ecs_deep_dive.md#free-list)。([coretemplate.hpp:121](../../src/core/userpublic/details/ecs/coretemplate.hpp#L121))

**arena(アリーナ)** — 同種のものをまとめて置くエンジン側の置き場。behavior は ECS コンポーネントではなく、エンジン所有の `BehaviorAttachmentArena` に連番で並びます。ゲーム DLL の型をエンジンの ECS チャンクに載せずに、ホットリロード時の世代交代(インスタンス状態は失われ、params は authoring 値から再構築)とエディタからの付け外しを安全にするための置き場です。具体的には、エンジン側の 1 本の `std::vector<Attachment>` がすべての behavior 実体を持ち、ゲームコードは handle 経由でしか触りません([behaviorarena.hpp:193](../../src/core/gamelogic/behaviorarena.hpp#L193))。「arena」という呼び名の由来は設計文書に書かれておらず**未確認**です(この用語集では「置き場」の意味だけで使います)。初出: [第8章 §8.5.5](08_gameplay.md#855-実行順とライフサイクル決定性)。([BehaviorAttachmentArena](../../src/core/gamelogic/behaviorarena.hpp#L174))

**SoA / Chunk / Archetype** — ECS の格納形。**Archetype** は「同じコンポーネントの組み合わせを持つ entity の集合」、**Chunk** はそれを一定個数ずつ区切った箱、**SoA**(Structure of Arrays)は「コンポーネントごとに別の連続配列に置く」並べ方です。1 つのコンポーネントだけを走査するときに無駄なメモリを読まずに済みます。初出: [第4章 §4.4](04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け)、詳細は [source-code-guide 第4章](../source-code-guide/04_ecs_deep_dive.md#chunkはsoa)。([ECSComponentChunk](../../src/core/userpublic/details/ecs/chunk.hpp#L19))

**type erasure(型消去)** — テンプレートで知っていた型情報を、**サイズ・アライメント・関数ポインタの表**へ潰して、実行時に任意の型を同じコードで扱えるようにする手法。ECS の列は `void*` + 「構築 / 破棄 / 詰め替え」の関数ポインタで持たれています。初出: [source-code-guide 第4章](../source-code-guide/04_ecs_deep_dive.md#runtimeの型消去メタデータ)。([VariedArray](../../src/core/userpublic/details/ecs/chunk.hpp#L25))

**relocate(詰め替え)** — entity を削除したときに、末尾の要素を空いた位置へ移して列を詰める操作(swap-and-pop)。型消去された列では move 構築 + 破棄を 1 つの関数ポインタにまとめてこれを行います。**要素のアドレスが動く**ので、コンポーネントへの生ポインタをフレームをまたいで持つのは危険です。初出: [第4章 §4.4](04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け)。

**MutationScope** — ECS の構造変更(entity / コンポーネントの追加・削除)の**再入を禁止する**ガード。構造変更の途中でさらに構造変更を始めると、半端に適用された状態が観測されうるため、`std::logic_error("ECS structural mutation is not reentrant")` で弾きます。初出: [第4章 §4.4](04_scene_ecs.md#44-ランタイム-ecs-の要点ゲーム開発者向け)。([MutationScope::MutationScope()](../../src/core/userpublic/details/ecs/coretemplate.cpp#L297))

**SlotMap** — 「index + generation のハンドルで引く配列 + 空きリスト」という一般的なデータ構造。pelican2 に `SlotMap` という名前の型はありませんが、`ModelInstanceId`(index + generation + scene_epoch)が同じ形です。初出: [source-code-guide 第10章 §10.3](../source-code-guide/10_background_knowledge.md#世代付きハンドルindex--generationと-slotmap)。([ModelInstanceId](../../src/core/renderer/modelinstance.hpp#L11))

**hazard(ハザード)** — 「同じものを読み書きする者どうしが同時に走ると壊れる」関係。2 か所で出ます: ECS ではシステムの read / write 宣言から実行順を自動で決めるための競合関係、フレームグラフではノード間のリソース依存から barrier を導くための関係です(`hazard_stress` は後者をわざと厳しくする計画プロファイル名)。初出: [第8章 §8.3](08_gameplay.md#83-ゲームシステムの登録と実行順序wp43)。([ECSHazardPolicy](../../src/core/userpublic/details/ecs/coretemplate.hpp#L35) / [targetplanning.hpp:257](../../src/project/targetplanning.hpp#L257))

**CRTP(奇妙に再帰したテンプレートパターン)** — 自分自身を基底クラスのテンプレート引数へ渡す書き方(`class Foo : Base<Foo>`)。仮想関数なしで「型ごとに違う実体」を作れるので、pelican2 ではモジュールの静的インスタンス生成に使われています。初出: [source-code-guide 第10章 §10.3](../source-code-guide/10_background_knowledge.md#crtp奇妙に再帰したテンプレートパターンによる強い型付け)。

---

## 14.4 ゲームロジックの語

**behavior(オブジェクト behavior)** — シーンの**オブジェクトに貼り付ける**ゲームコードの単位。`behavior` コンポーネントに型名と params を書くと、ゲーム DLL 側に登録された C++ クラスが実体化されます。もう一つの書き方であるゲームシステム(世界全体を回す)との使い分けは §8.5.7 にあります。初出: [第8章 §8.5](08_gameplay.md#85-オブジェクト-behavior--もう一つのゲームコードの書き方wp155162167)。

**GameContext** — ゲームコードがエンジンに触るための**公開ファサード**(唯一の窓口)。状態を持たず、毎フレーム作られて渡されます。入力・カメラ・物理クエリ・音・保存・乱数などがここに集まっています。初出: [第8章 §8.4](08_gameplay.md#84-gamecontext-api-一覧)。([GameContext](../../src/core/userpublic/gamecontext.hpp#L22))

**order(順序値)と総順序** — ゲームシステムの実行順を決める整数。同じ `order` のシステムは**名前の辞書順**で並ぶので、全システムに一意な順序が決まります(= 総順序)。これが決定性の土台の 1 つです。初出: [第8章 §8.3](08_gameplay.md#83-ゲームシステムの登録と実行順序wp43)。

**epoch anchor** — ゲーム DLL が必ず import する**目印のシンボル**(`pelican_game_logic_epoch_v2`)。ロードの前に DLL の import テーブルだけを読み、この目印が無ければ「古い DLL」として `game_logic_abi_stale@1` で拒否します。登録を持たない provider 専用 DLL にはリンカが強制する要素が無いので、この診断が唯一の門番です。世代番号は**シンボル名そのものへの文字列連結**で入ります —— マクロが `pelican_game_logic_epoch_v` に ABI 版を貼り付けるので、版が 3 になればシンボルは `pelican_game_logic_epoch_v3` に変わり、`_v2` を import する古い DLL は名前が一致しなくなります([gamelogic.hpp:10](../../src/core/userpublic/gamelogic.hpp#L10))。初出: [第8章 ゲーム DLL の ABI](08_gameplay.md#ゲーム-dll-の-abiwp381381a382a)。([gamelogic.hpp:23](../../src/core/userpublic/gamelogic.hpp#L23))

**shadow copy(シャドウコピー)** — 実行中の DLL は Windows では上書きできないため、**原本の複製を作ってそちらをロードする**手口。pelican2 は原本の隣の `.pelican-hot-reload/<プロセス ID>/` へ複製するので、同じプロジェクトを 2 つ起動しても取り合いになりません。初出: [第8章 ゲーム DLL の ABI](08_gameplay.md#ゲーム-dll-の-abiwp381381a382a)。([gamelogicreload.cpp:423](../../src/core/gamelogic/gamelogicreload.cpp#L423))

**PCG32(permuted congruential generator)** — 高速で周期が長い擬似乱数生成器の一種。pelican2 は `std::mt19937` + `std::uniform_*` を**使わず**(処理系によって出る列が違うため)、PCG32 を分布まで自前実装しています。既定シードは 0 固定で、毎回変えたいゲームは自分で種を入れます。初出: [第8章 §8.10](08_gameplay.md#810-決定性--なぜ毎回同じ結果になるのか)。([deterministicrng.cpp:14](../../src/core/userpublic/deterministicrng.cpp#L14))

**provider(プロバイダ)** — ある機能の**実装を差し替えられる口**。物理クエリでは Builtin / Jolt / ゲーム DLL の provider を capability 単位で差し替えられ(選択はビルド時)、描画では view family や draw の供給者を差し替えられます。クエリの順序やタイブレークの契約は provider ではなく**エンジンが所有する**ので、差し替えても結果の並びは変わりません。初出: [第8章 §8.8](08_gameplay.md#88-物理クエリとトリガーp1p2wp107179)。([ProviderV2](../../src/core/userpublic/physics/abi_v2.hpp#L83))

**GJK / EPA** — 凸形状どうしの当たり判定の定番アルゴリズム。GJK は 2 つの形状の差(ミンコフスキー差)の中で原点までの最近点を反復で探し、**離れているか / 距離はいくらか**を出します。貫通しているときは GJK だけでは深さが出ないので、EPA(expanding polytope algorithm)が GJK の残した単体を膨らませて押し出す深さと向きを求めます。pelican2 は両方を自前で実装しており、GJK 反復は 40 回上限です。初出: [source-code-guide 第5章 §5.7](../source-code-guide/05_gameplay_and_services.md#57-physics)。([physquerysweep.cpp:20](../../src/core/phys/physquerysweep.cpp#L20) / [penetration()](../../src/core/phys/physquerysweep.cpp#L657))

**MTD(minimum translation distance)** — 既に食い込んでいる 2 つの形状を、**最小の移動量で引き離す**ベクトル。`shapeCast` が初期重なりを返したとき、`normal * penetration_depth` がこれにあたります。同点になったときは「移動の逆向きを優先し、その後 world x/y/z」で固定して決定性を保ちます。初出: [第8章 §8.8](08_gameplay.md#88-物理クエリとトリガーp1p2wp107179)。

**matrix palette(マトリクスパレット)** — スキニングで使う**ボーン行列の配列**。頂点は「どのボーンをどれだけ参照するか」を持ち、シェーダはこの配列から行列を引いて頂点を変形します。pelican2 では glTF の skin ごとの範囲が結合パレットを過不足なく覆うことを検証します。初出: [source-code-guide 第3章](../source-code-guide/03_project_and_loading.md)。([buildSkinPalette()](../../src/core/animation/animationjobs.cpp#L403))

---

## 14.5 描画とレイトレーシングの語

**RT(render target)** — 描画結果を書き込む画像。rendering config の `render_targets` で名前・サイズ(`extent_scale` か固定 `width`/`height`)・format を宣言し、パスが `input` / `output` で名前参照します。文中の「display RT」は最終的に画面へ出す RT のことです。初出: [第6章 render_targets](06_rendering.md#render_targets)。

**swapchain(スワップチェーン)** — ウィンドウへ実際に表示される画像の列を、GPU とウィンドウシステムの間で回す仕組み。pelican2 では `"swapchain"` は**予約名**で、RT 名として書くとエラーになります。初出: [第6章 render_targets](06_rendering.md#render_targets)。([SwapchainFrameTarget](../../src/core/vkcore/swapchainframetarget.hpp#L9))

**format_class** — RT の実 format を直接書かず、**役割**で書く指定(`scene` / `display` / `data` / `explicit(...)`)。`scene` は HDR 時に float16・SDR 時に 8bit sRGB へ、`display` は 8bit sRGB へリゾルバが解決します。feature の有無で HDR 化を切り替えられるのはこの層があるからです。初出: [第6章 §6.3](06_rendering.md#63-カラーパイプラインwp7274)。

**SRGB / リニアワークフロー** — sRGB は「表示用に非線形な曲線をかけた」色の入れ物、リニアは「光の量に比例した」値。pelican2 は**シェーダの入出力をすべてリニア**にし、sRGB への変換はハードウェア(`*_SRGB` view)か終端の `output_transform` ノードの 1 か所だけで行います。シェーダに `pow(x, 2.2)` を書くと二重変換になります。初出: [第6章 §6.3](06_rendering.md#63-カラーパイプラインwp7274)。

**G-buffer** — 「色を最後に決めるために、位置・法線・材質などを**いったん複数の RT へ書き出す**」方式(deferred shading)の、その中間 RT 群のこと。pelican2 では G-buffer の構成はエンジンが固定せず、`.surface` の material output ABI で任意に宣言できます。初出: [第6章 任意 G-buffer](06_rendering.md#任意-g-buffer--material-output-abiattachment-statewp218219)。

**stem(ステム)** — シェーダを**拡張子なしの名前**で参照する規約(`"shaders/lava"`、`"engine://ssao"`)。解決順は `<stem>.vert`(GLSL ソース → 実行時コンパイル)→ `<stem>.vert.spv`(ビルド済み SPIR-V)で、拡張子を書くとエラーです。この規約は web プロファイルと共有される可搬形式です。初出: [第6章 stem 参照](06_rendering.md#stem-参照)、web 側は [第9章 §9.4](09_web.md#94-シェーダ-stem-規約--可搬シェーダの書き方)。

**SPIR-V / SPIR-V reflection** — SPIR-V は Vulkan が読むシェーダの中間バイナリ形式。**reflection**(リフレクション)は、そのバイナリを解析して「どの set / binding に何型のリソースが要るか」を取り出すことです。pelican2 は spirv-reflect で解析し、descriptor set layout・pipeline layout・push constant range を**全自動生成**します。初出: [第6章 自動リフレクションとキャッシュ](06_rendering.md#自動リフレクションとキャッシュ)。([ShaderCompiler](../../src/core/shader/shadercompiler.hpp#L39))

**descriptor set** — シェーダが使うリソース(テクスチャ・バッファ)をまとめて束ねた単位で、番号(set 0, 1, …)で区別します。pelican2 は set の役割を規約で固定しており(フレーム共通・パス・マテリアルなど)、シェーダ側は番号を直接書かずに規約の定数を使います。初出: [第6章 descriptor set 規約](06_rendering.md#descriptor-set-規約shader_contractmd-が正)。([pelican_sets.hpp:7](../../src/core/shader/pelican_sets.hpp#L7))

**feature** — GPU 機能を「JSON の断片 + シェーダ断片」で足す**ユーザー空間の**単位。rendering config の `features[]` に参照を書いたときだけ存在し、書かなければ機構ごと素通りします(パージ可能)。エンジン同梱の feature も特権は持たず、`engine://features/*.json` をコピーして改造するのが公式ワークフローです。初出: [第6章 §6.5](06_rendering.md#65-feature--パージ可能な-gpu-機能wp2831548788104226)。

**canonical anchor(標準アンカー)** — feature がパスを差し込む先として、常に実体化される 8 つの目印ノード(`sprite → post_main → tonemap → post_ldr → pelican_ui → debug_draw → debug_text → imgui`)。この**全順序はエンジンの契約**で並べ替えられませんが、あいだに挿すのは自由です。初出: [第6章 canonical anchor](06_rendering.md#canonical-anchorwp73)。

**frame graph(フレームグラフ)** — パスの `input` / `output` から依存を自動で導き、実行順・並び・barrier をエンジンが決める仕組み。手書きの barrier は禁止で、JSON の宣言順は依存が許す範囲での**安定タイブレーク**にすぎません。「グラフ」の中身は、**ノード** = パスや compute タスク、**辺** = 「ある RT を書く側 → それを読む側」の依存で、プランナはこれをトポロジカルに並べて `nodes`(実行順)・`levels`(依存が無く同じ段に置けるノードの束)・`barriers` を出します([FramePlan](../../src/core/renderingpass/frameplanner.hpp#L154))。初出: [第6章 §6.6](06_rendering.md#66-フレームグラフf0f2--wp3335--wp64)。

**FramePlan** — フレームグラフを解決した結果の**実行計画**。ノード種別は `render` / `compute` / `anchor` / `snapshot_copy` / `output_transform` で、全構成がこの順序で実行されます(旧来の「JSON 宣言順で実行」経路は撤去済み)。RPC の `get_frame_plan` や ImGui の plan viewer で中身を見られます。初出: [第6章 §6.6](06_rendering.md#66-フレームグラフf0f2--wp3335--wp64)。([FramePlan](../../src/core/renderingpass/frameplanner.hpp#L154) / [FramePlanNodeKind](../../src/core/renderingpass/frameplanner.hpp#L15))

**logical plan / physical plan(論理プラン / 物理プラン)** — **論理プラン**はリソースを「値と読み取り範囲」だけで表したデバイス非依存の表現、**物理プラン**はそこへ実フォーマット・サンプル数・scope・view 実行を載せたデバイス依存の表現です。「何を計算するか」と「Vulkan でどう置くか」を分ける層で、物理プランは丸ごと差し替え可能なパッケージとして eject できます。違いは型の欄で見るのが早く、論理側の 1 リソースは `name` / `type` / materialization の 3 項だけ([LogicalResourceDesc](../../src/project/logicalrendergraph.hpp#L60))、物理側の同じリソースにはそこへ `format` / `rasterization_samples` / `view_layout` / `mip_levels` / `array_layers` / alias 可否が付きます([VulkanPhysicalResourcePlan](../../src/project/targetrenderplanning.hpp#L202))。例えば `format_class: "scene"` は論理側では文字どおり「scene 用の色」でしかなく、実 format(HDR なら `R16G16B16A16_SFLOAT`、SDR なら `B8G8R8A8_SRGB`)を欄として持つのは物理側です([第6章 §6.3](06_rendering.md))。初出: [第6章 target planning と Vulkan plan pin](06_rendering.md#target-planning-と-vulkan-plan-pin)。([CompiledLogicalRenderGraph](../../src/project/logicalrendergraph.hpp#L254) / [VulkanTargetPlan](../../src/project/targetrenderplanning.hpp#L478))

**scope(スコープ)** — 物理プランの上で、**1 回の dynamic rendering を開いたまま連続して記録するノードの束**。コマンド記録のループは「scope → 実行(view) → その scope の全ノード」の順(scope-execution-major)で、「view ごとに全ノード」ではありません。`scope_edit_mode` はこの束を編集するときにどこまで崩してよいかの指定です。初出: [第6章 Vulkan physical fragment](06_rendering.md#vulkan-physical-fragment-v1--v2--v3)、詳細は [source-code-guide 第6章](../source-code-guide/06_rendering_vulkan_shader.md)。

**view family(ビューファミリ)** — 「同じ内容を複数の視点で描く」ときの、その**視点の束**。XR の左右眼、シャドウのカスケード、キューブキャプチャの 6 面などが該当します。`$main` の family は物理プランが view 数まで決め、secondary family は実行時に数が決まる、という非対称があります。初出: [第6章 §6.12](06_rendering.md#612-openxr-ステレオレンダリングwp125138--wp203ac実装済み)。([RenderViewFamily](../../src/core/renderer/viewfamily.hpp#L72) / [ViewFamilyV1](../../src/core/userpublic/render/render_strategy_abi_v1.hpp#L88))

**`.surface`** — マテリアルの**シェーダとパラメータ宣言**を 1 ファイルにまとめた自己記述コンテナ。値のほうは別の `*.material.json` に書きます(2 ファイル方式)。同梱の standard / toon / OpenPBR も特権なしの `.surface` として実装されています。初出: [第6章 .surface](06_rendering.md#surface--自己記述コンテナ)。([SurfaceShaderComposition](../../src/core/shader/surfacecompiler.hpp#L54))

**SSAO(screen-space ambient occlusion)** — 深度と法線から「くぼみの暗さ」を画面空間で近似する定番手法。pelican2 では feature ではなく**同梱シェーダ**(`engine://ssao` / `engine://ssao_blur`)で、プロジェクトの rendering config が自分でパスを組みます。初出: [第6章 §6.4](06_rendering.md#64-シェーダ基盤shader-freedom-kit-wp101519--m2bwp70)。([ssao.frag](../../src/core/resources/ssao.frag))

**bloom(ブルーム)** — 明るい部分をにじませて眩しさを表現する後処理。SSAO と同じく feature ではなく同梱シェーダ群(`bloom_highpass` / `bloom_blur_h` / `bloom_blur_v` / `bloom_upsample` / `bloom_composite`)で、パスの組み方はプロジェクト側です。初出: [第6章 §6.4](06_rendering.md#64-シェーダ基盤shader-freedom-kit-wp101519--m2bwp70)。([bloom_highpass.frag](../../src/core/resources/bloom_highpass.frag))

**TAA(temporal anti-aliasing)** — 毎フレーム少しずらして描いた結果を、前フレームの履歴と混ぜてジャギーを消す手法。pelican2 では `taa.json` feature が resolve + composite の 2 パスと jitter provider を持ちます。履歴・速度・jitter はユーザー空間側の道具立てで、エンジンは機構だけを持ちます。初出: [第6章 標準 TAA](06_rendering.md#標準-taawp113)。

**projection jitter** — TAA のために、毎フレーム射影行列を**サブピクセル単位でずらす**こと。ずらす列は halton23 などの低食い違い数列で、`@history` の履歴 RT と組で使います。初出: [第6章 projection jitter](06_rendering.md#projection-jitterwp112115)。

**VAT(Vertex Animation Texture)** — 頂点の動きをテクスチャに焼き込んだアニメーション形式(布・破壊など、ボーンで表せない動きに使う)。pelican2 では glb の primitive の `extras["pelican.vat"]` にメタデータを置き、実体は glb 内の half-float bufferView です。再生は前後 2 フレームを `texelFetch` して**手動 lerp** します(ハードウェア線形フィルタは頂点方向へ滲むため禁止)。初出: [第5章 §5.4](05_assets.md#54-焼き込みアニメーション2--pelicanvat-v1wp20)。

**IBL(image-based lighting)** — 環境を写した画像から、拡散・鏡面の環境光を作るライティング手法。**pelican2 では未実装**です(WP32 は番号予約のみ)。同梱の `sky_ambient.json` は単色の環境光であって IBL ではありません。初出: [第11章 §11.1](11_status.md#111-wp-全台帳)。

**AS / BLAS / TLAS(加速構造)** — レイトレーシングで「レイがどの三角形に当たるか」を高速に絞るための木構造。**BLAS**(bottom-level)は 1 つのジオメトリの中身、**TLAS**(top-level)は BLAS のインスタンスを世界に並べた表で、**AS** はその総称です。pelican2 の AS は**静的ジオメトリ専用**で、transform を動かしても BLAS は無効化されません(TLAS の行が変わるだけ)。初出: [第6章 §6.15](06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v1--v2-と-pelican_ray_instancert3-1--wp374374a377379383)。([accelerationstructure.hpp:69](../../src/core/vkcore/accelerationstructure.hpp#L119))

**SBT(shader binding table)** — レイトレーシングパイプラインで、「どのレイがどのシェーダ(raygen / miss / closest-hit)に当たるか」を引くための表。`ray_group_layout` の `ray_types` / `groups` / `misses` / `record` がこの表の形を決めます。`acceleration_structure` は SBT の入力ではないので、layout の digest からは取り除いて計算されます。初出: [第6章 §6.15](06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v1--v2-と-pelican_ray_instancert3-1--wp374374a377379383)。([RayTracingSceneShaderBindingTables](../../src/core/shader/pipelinefactory.hpp#L173))

**`ray_group_layout`(ray layout v1 / v2)** — レイトレの宣言層の入口となる JSON。version は 1 か 2 だけを受理し、**プロセス一生ものの 3 値**(layout 無し / v1 / v2)として起動時に確定します。v2 のときだけ `acceleration_structure` 宣言と `pelican_ray_instance` コンポーネントが使えます。プロセス途中でのモード変更は拒否されます。初出: [第6章 §6.15](06_rendering.md#615-レイトレーシングの宣言層--ray-layout-v1--v2-と-pelican_ray_instancert3-1--wp374374a377379383)。([RayLayoutRuntimeState](../../src/core/shader/raygrouplayout.hpp#L164))

---

## 14.6 テスト・運用・ツールの語

**JSON-RPC(stdio)** — 外部プロセスからエンジンを操作するためのプロトコル。1 行 1 メッセージの JSON を stdin / stdout でやり取りし(改行区切り JSON)、`--rpc` 中は **stdout がプロトコル専用**になります(ログは stderr へ)。エラーコードは JSON-RPC 2.0 の規約(`-32700` parse error など)に従います。初出: [第10章 §10.3](10_tools.md#103-json-rpcstdio-外部制御プロトコル実装済み)。([RpcServer](../../src/core/communication/rpcserver.hpp#L53))

**ホットリロード(hot reload)** — 実行を止めずに、保存されたファイルを反映すること。対象はシェーダ・`.surface`・テクスチャ・マテリアル値・モデルコンテナ・ゲームロジック DLL で、**シーンは対象外**です。1 つの変更に影響する全バリアントを準備してからフレーム境界で一括公開し、どれか 1 つでも失敗すれば全体を破棄して旧世代を維持します(fail-soft)。初出: [第10章 §10.5](10_tools.md#105-ホットリロードwp9096110147162178)。

**チケット(ticket)** — エディタの編集操作を**フレーム境界で適用する**ための受付票。受付時に前提条件を検査し、コミット時にもう一度検査します(受付から適用までの間に世界が動くため)。「トランザクション」という語がシーンロード・作用・編集の 3 か所で使われるので、編集の文脈ではチケットと呼び分けます。初出: [第13章 §13.3](13_editor.md#133-編集の実行モデル--チケットとフレーム境界)。([EditorCommandService](../../src/core/communication/editorcommandservice.hpp#L280))

**headless(ヘッドレス)** — ウィンドウを開かずに描画・実行するモード。時刻が固定ステップ(dt = 1/fps)になるためリプレイと golden テストの土台になり、CI から PNG を取り出せます。初出: [第2章 §2.5](02_getting_started.md#25-ヘッドレス実行ウィンドウなしで-png-を出す)。

**Job オブジェクト(Windows)** — 子プロセスを束ねて**まとめて終了させる**ための Windows のカーネルオブジェクト(POSIX のプロセスグループに相当)。pelican2 ではツールが起動した子プロセス(と孫)を取り残さないために使います。初出: [source-code-guide 第10章 §10.4](../source-code-guide/10_background_knowledge.md#windows-の-job-オブジェクトプロセスグループの対応物)。([processrunner.cpp:171](../../src/devcli/processrunner.cpp#L171))

**名前付き mutex(named mutex)** — 名前でプロセスをまたいで待ち合わせられる排他オブジェクト。`Global\` を付けるとマシン全体が範囲になります。pelican2 では GPU を使うテストを 1 本ずつ走らせる lease に使い、前の保持者が異常終了した場合(`WAIT_ABANDONED`)も取得成功として扱います(GPU が永久にロックされないため)。初出: [source-code-guide 第10章 §10.4](../source-code-guide/10_background_knowledge.md#windows-の名前付き-mutex-と-global-名前空間machine-wide-の相互排他)。([gpu_lease_run.cpp:157](../../test/gpu_lease_run.cpp#L157))

**doclink** — 文書中の `#L<行番号>` リンクが指す先を追跡し続けるためのツール(`tools/doclink.py`)。`update` で行番号を貼り直し、`check` で古くなったリンクを検出します。行番号はキャッシュで、本体は「その行に何が書いてあったか」というアンカーです。初出: [第10章 §10.10](10_tools.md#1010-リポジトリ保守スクリプトtools)。

---

## 関連文書

- [第11章 実装状況と文書マップ](11_status.md) — WP 台帳と設計文書マップ。語の出所をたどるときの起点
- [第12章 コード読解ガイド](12_codemap.md) — JSON がどのパーサで読まれるかの対応表
- [../source-code-guide/10_background_knowledge.md](../source-code-guide/10_background_knowledge.md) — Vulkan / glTF / C++ / OS の**一般知識**の詳説(本章の一般用語はここへ繋がります)
- [../source-code-guide/README.md](../source-code-guide/README.md) — クラス単位の内部構造解説
