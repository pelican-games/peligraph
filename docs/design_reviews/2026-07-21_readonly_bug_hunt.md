# 読み取り専用バグ探索 — 2026-07-21

日付: 2026-07-21

対象: branch `codex/rendering-phase1-refactor` / 基準コミット `5c3d38b`

## 手法

本調査は実装に一切手を触れない読み取り専用の探索です。手順は次の四段階でした。

1. 小型モデル 8 面(ローダ・パーサ / モデル読み込み / 純ロジック層 / UI / 描画パス / Vulkan 低層 / 入力 / ツール)で、機械的なパターン検査を並行実施しました。
2. 大型モデル 5 面(エディタのトランザクション / ECS とスケジューラ / ホットリロードと DLL / 物理とアニメの数値と決定性 / フレームグラフと XR)で、意味論に踏み込んだ検査を並行実施しました。
3. 上がってきた候補 45 件を選別し、重複を統合して明白な誤検出を除去し、32 件へ絞りました。
4. 残った 32 件を 1 件ずつ**反証**にかけました。担当には「これはバグではないと結論できる根拠を全力で探せ」と指示し、上流検証・早期 return・型やアサート・テストによる意図固定・設計文書の記述のいずれかで否定できたものは落としました。否定しきれなかったものだけを confirmed としています。

したがって以下の confirmed は「バグである」ことの証明ではなく、「バグでないと言い切れなかった」ものの集合です。ただし各件について何を試して否定できなかったかを §2 に残しているため、再検証の出発点としてはそのまま使えます。

## 0. 結論

候補 45 件 → 選別後 32 件 → **confirmed 25 件 / 判断保留 1 件 / 反証により否定 6 件**です。選別段階では 11 項目(複数候補をまとめたものを含む)を落としました。

※ 45 − 32 = 13 件が選別で落ちた計算になりますが、§4.2 の 11 項目をまとめられた候補ごとに展開すると 14 件になります。この差は報告元データに由来するもので(まとめ方の粒度の揺れ)、確定件数には影響しません。

confirmed 25 件の重大度別内訳は次のとおりです。

| 重大度 | 件数 | 内訳 |
|---|---|---|
| critical | 1 | bug-9 |
| high | 8 | bug-4, bug-5, bug-6, bug-10, bug-16, bug-20, bug-28, bug-29 |
| medium | 13 | bug-1, bug-3, bug-7, bug-11, bug-12, bug-14, bug-15, bug-17, bug-19, bug-21, bug-22, bug-30, bug-31 |
| low | 3 | bug-2, bug-23, bug-27 |

これとは別に、本探索の前から把握していた sprite-1(high)と vk-1(medium)があります。§1 の一覧には冒頭に含めています。

**調査そのものは実装に一切手を触れていません。** 探索・選別・反証のすべてが読み取り専用で行われ、この報告書も修正を含みません。修正は §5 に挙げた別タスクで扱います。

> **追記(2026-07-21 執筆時点):** 本報告書を書いている時点で、作業ツリーには **sprite-1 / vk-1 / bug-20 の修正が着手済み**(未コミット)です。`src/core/phys/physquerysweep.cpp` に `kMaxSimplexVertices` による頂点数ガード、`src/core/vkcore/core.cpp` に `drawIndirectFirstInstance` の有効化と能力検出(`bootstrap.*` / `indirectdrawlimits.*` 新設)、`src/core/renderer/spriterenderer.cpp` に描画データの分離(`spritedrawdata.hpp` 新設)が入っています。したがって「読み取り専用」は**調査手法の説明**であって、現在のツリーの状態ではありません。読者は該当 3 件について、本報告書の記述と実装が既にずれている可能性を前提にしてください。

## 1. 一覧

検証状況の列は 3 値です。`Fable 検証済み` は**統括(Fable)が実コードを直接読んで機構を確認した 3 件**、`反証通過` は反証担当が否定を試みて失敗した confirmed、`保留` は判断保留の 1 件です。

出典について 2 点補足します。**sprite-1 と vk-1 はこの探索の産物ではなく**、先行するドキュメント作業中に見つかり、その場で統括が実コードで確認したものです(したがって重大度も統括の付与で、本探索の JSON には現れません)。**bug-9 は他の 24 件と同じ反証工程を通っており**、そのうえで統括が `PreparedComponentSwap` の `noexcept` 宣言・`chunk.at()` が投げること・`EditorProjectionTransaction::commit` が prepare 全回し → publish 全回しであることを個別に確認したため、この列で区別しています。

なお、以下の表とその後の各節で併記しているシンボル名は、報告元のデータをそのまま踏襲しています。**シンボルの定義位置が「ファイル」列と異なる件があります**(bug-28 の `bufferReadAfterWriteBarrier` は `computetask.cpp`、bug-30 の `endFrameWithoutLayers` は `openxrcompositiontarget.cpp`、bug-31 の `consumeExtentChanged` も同ファイルに定義があります)。ファイル列は不具合が現れる場所、シンボルは関係する処理の名前と読んでください。

| id | 重大度 | ファイル | 一行要約 | 検証状況 |
|---|---|---|---|---|
| sprite-1 | high | [`src/core/renderer/spriterenderer.cpp`](../../src/core/renderer/spriterenderer.cpp) | DrawRun がチャンク境界で分割されず、前チャンクの `vertex_offset` を保持したまま延びる | Fable 検証済み |
| vk-1 | medium | [`src/core/renderer/polygoninstancecontainer.cpp`](../../src/core/renderer/polygoninstancecontainer.cpp) / [`src/core/vkcore/core.cpp`](../../src/core/vkcore/core.cpp) | 間接描画で `firstInstance` を非 0 にしながら `drawIndirectFirstInstance` フィーチャを有効化していない | Fable 検証済み |
| bug-9 | critical | [`src/core/userpublic/details/ecs/coretemplate.hpp`](../../src/core/userpublic/details/ecs/coretemplate.hpp) | noexcept な publish() が陳腐化した (chunk_index,row) で throwing な at() を呼び std::terminate する | Fable 検証済み |
| bug-4 | high | [`src/core/communication/editorjournal.cpp`](../../src/core/communication/editorjournal.cpp) | revert が元 transaction の write path に自分の id を刻むため 2 段目以降の undo が必ず衝突する | 反証通過 |
| bug-5 | high | [`src/core/communication/editorruntimefactory.cpp`](../../src/core/communication/editorruntimefactory.cpp) | 非 behavior の add_component の inverse に `component_index` が無いのに実行側が無条件に読む | 反証通過 |
| bug-6 | high | [`src/core/communication/editorruntimefactory.cpp`](../../src/core/communication/editorruntimefactory.cpp) | behavior attach 経路だけが `authored.at("params")` を無条件参照し、params 省略で例外になる | 反証通過 |
| bug-10 | high | [`src/core/userpublic/details/ecs/coretemplate.cpp`](../../src/core/userpublic/details/ecs/coretemplate.cpp) | MutationScope が core 単位の単一ラッチのため、構造変更 adapter を 2 個含むトランザクションが必ず失敗する | 反証通過 |
| bug-16 | high | [`src/core/watch/filewatcher.cpp`](../../src/core/watch/filewatcher.cpp) | ワーカースレッドに例外バリアが無く、走査中の filesystem_error が std::terminate になる | 反証通過 |
| bug-20 | high | [`src/core/phys/physquerysweep.cpp`](../../src/core/phys/physquerysweep.cpp) | GJK の simplex が 5 頂点に膨らみ、`std::array<int,4>` / `weights[4]` へ範囲外書き込みする | 反証通過 |
| bug-28 | high | [`src/core/vkcore/renderer.cpp`](../../src/core/vkcore/renderer.cpp) | フレームグラフの read_after_write バリアがレンダーターゲット(画像)に対して無言で捨てられる | 反証通過 |
| bug-29 | high | [`src/core/renderingpass/computetask.cpp`](../../src/core/renderingpass/computetask.cpp) | コンピュートタスクのディスクリプタセットが登録時に一度だけ解決され、リサイズ後も再バインドされない | 反証通過 |
| bug-1 | medium | [`src/core/model/gltf.cpp`](../../src/core/model/gltf.cpp) | `getDataFromAccessor` が accessor / bufferView の index を検証せず `[]` でアクセスする | 反証通過 |
| bug-3 | medium | [`src/core/loader/basicconfig.cpp`](../../src/core/loader/basicconfig.cpp) | `basic_config.camera.up` が 3 要素未満のとき検証なしに `operator[]` を 3 回引く | 反証通過 |
| bug-7 | medium | [`src/core/loader/editorpreviewprojection.cpp`](../../src/core/loader/editorpreviewprojection.cpp) | `descendant_world` の descendants 集合が scene ループの外にあり別 scene が混入する | 反証通過 |
| bug-11 | medium | [`src/core/ecs/archetypemigration.cpp`](../../src/core/ecs/archetypemigration.cpp) | archetype 移行・トークン経由の生成が毎回 CHUNK_CAPACITY 分の chunk を確保して 1 体だけ載せる | 反証通過 |
| bug-12 | medium | [`src/core/userpublic/details/ecs/coretemplate.cpp`](../../src/core/userpublic/details/ecs/coretemplate.cpp) | `remove()` だけが component version を更新せず、変更検知に依存するシステムが削除を取りこぼす | 反証通過 |
| bug-14 | medium | [`src/core/watch/filewatcher.cpp`](../../src/core/watch/filewatcher.cpp) | `armAll` が `status.error` をミューテックス外で書き換え、`status()` のコピーと競合する | 反証通過 |
| bug-15 | medium | [`src/core/watch/filewatcher.cpp`](../../src/core/watch/filewatcher.cpp) | `NativeWatch::stop()` の CancelIoEx が再アーム直前の窓で取りこぼされ停止が永久にハングする | 反証通過 |
| bug-17 | medium | [`src/core/watch/filewatcher.cpp`](../../src/core/watch/filewatcher.cpp) | digest がリトライ枯渇 / error で読めなかったファイルが「削除」として reconcile される | 反証通過 |
| bug-19 | medium | [`src/core/gamelogic/behaviorarena.cpp`](../../src/core/gamelogic/behaviorarena.cpp) | 恒久的に失敗するエディタ attach を毎フレーム無限リトライする | 反証通過 |
| bug-21 | medium | [`src/core/animation/animationprobe.cpp`](../../src/core/animation/animationprobe.cpp) | `advanceCursor` のループ境界が delta / duration 由来で無制限になりハングと変換 UB を起こす | 反証通過 |
| bug-22 | medium | [`src/core/animation/animationservice.cpp`](../../src/core/animation/animationservice.cpp) | `notify()` が `sink->object->asset` を null 検査なしで参照する | 反証通過 |
| bug-30 | medium | [`src/core/appflow/loop.cpp`](../../src/core/appflow/loop.cpp) | `xrBeginFrame` が非 ready かつ shouldRender=true のとき `endFrameWithoutLayers` が必ず throw する | 反証通過 |
| bug-31 | medium | [`src/core/vkcore/swapchainframetarget.cpp`](../../src/core/vkcore/swapchainframetarget.cpp) | XR セッション中はデスクトップスワップチェインが再生成されず、リサイズ後ミラーが停止する | 反証通過 |
| bug-2 | low | [`src/core/model/gltf.cpp`](../../src/core/model/gltf.cpp) | scenes が空の glTF で `model.scenes[0]` を範囲外アクセスする | 反証通過 |
| bug-23 | low | [`src/core/animation/vrmapplication.cpp`](../../src/core/animation/vrmapplication.cpp) | VRM 表情の override 適用が自身の書き換え結果を読み返し、結果が表情名の辞書順に依存する | 反証通過 |
| bug-27 | low | [`src/core/phys/physquery.cpp`](../../src/core/phys/physquery.cpp) | `raySphereRoots` が二乗次元の判別式に無次元の絶対イプシロンを使い、小半径の球で偽ヒットする | 反証通過 |
| bug-18 | low | [`src/core/gamelogic/gamelogicreload.cpp`](../../src/core/gamelogic/gamelogicreload.cpp) | `pollAttempt` が試行前に `observed_write_time` を進めるため失敗が握り潰される | 保留 |

## 2. 個別

### bug-9: PreparedComponentSwap が (chunk_index,row) を握るため、同一トランザクション内の archetype 移行後に noexcept な publish() から at() が投げて std::terminate する

**重大度**: critical

**場所**: [`src/core/userpublic/details/ecs/coretemplate.hpp`](../../src/core/userpublic/details/ecs/coretemplate.hpp):228-260 — `ECSCoreTemplatePublic::PreparedComponentSwap::publish` / `prepareComponentSwap`

**何が起きるか**

`PreparedComponentSwap` は EntityId ではなく (chunk_index, row) の組を保持し、publish 時に `chunks_storage[chunk_index]` と `chunk.at(component_index, row)` で書き込み先を再解決します。ところが `EditorProjectionTransaction::commit` は「全 adapter の prepare を回してから全 adapter の publish を回す」という 2 フェーズ構造で、archetype 移行 adapter の publish が先に走ると元 chunk は末尾行 backfill によって count が 1 減ります。この状態で後続の swap が古い row を触ると、`VariedArray::at` が `std::out_of_range` を投げ、noexcept 指定された publish() から例外が抜けて std::terminate になります。エディタで「親を持たず子も持たない root オブジェクトを別オブジェクトの下へドラッグする」という reparent 操作 1 個で成立し、プロセスが即死して未保存の編集が失われます。

**なぜバグと判断したか**

- `VariedArray::at` は `index >= count` で確実に throw し、migration publish は配列を消さず count だけ減らすので `has()` は通過して throw に到達します。`publish()`、`StandaloneTransformProjectionAdapter::publish()`、`EditorProjectionTransaction::commit` がいずれも noexcept で、外側に catch はありません。
- reparent が migration を組む条件(`local == nullptr`)と standalone が swap を prepare する条件(`world != nullptr && local == nullptr`)が完全に一致するため、移行対象 entity は必ず swap 側の prepared にも入ります。adapter の積む順序から publish 順は migration → swap で固定です。
- MutationScope は「構造変更の再入」だけを禁止するもので、`prepareComponentSwap` は MutationScope を取らないため守られません。
- `StandaloneTransformProjectionAdapter` / `prepareComponentSwap` を参照するテストはゼロで、この実経路を通すテストが 1 本も存在しません。
- 否定できたのは「`new_parent_id` が null(ルートへ戻す)なら migration が組まれない」1 点だけでした。

**修正の方向**

本命は `PreparedComponentSwap` に EntityId と component_id を持たせ、publish / rollback 内で noexcept な `resolve(id)` により再解決してから `atUnchecked` で書く形にすることです。解決できなければ何もせず published を立てず、publish 経路から throwing な `at()` を完全に排除します。次善策として構造変更世代カウンタを prepare 時に控え、publish フェーズ入口で世代が変わっていればトランザクションを Failed にする検出方式もあります。あわせて `TransformProjectionAdapter` が生ポインタで `TransformComponent*` を握っている点も同じ backfill で静かに壊れるため、同方針で EntityId 保持へ寄せる必要があります。

### bug-4: revert が元 transaction の write path の last_writer を自分の id で上書きするため、同一 target への 2 回連続 undo が必ず undo_conflict になる

**重大度**: high

**場所**: [`src/core/communication/editorjournal.cpp`](../../src/core/communication/editorjournal.cpp):2116-2140 — `requireRevertPreconditions` / `finalizeRevertJournal`

**何が起きるか**

undo を実行すると `finalizeRevertJournal` が revert レコード自身の transaction_id を、元 transaction が書いた path の `last_writers` に刻みます。一方 `requireRevertPreconditions` の第 2 検査は「その path の last_writer が undo 対象の transaction_id と一致しないなら undo_conflict」と判定するため、同じ path を 2 回編集してから undo を 2 回行うと、2 回目が必ず衝突します。失敗は受理時に throw されるので undo_stack が pop されず、そのスタックは以後永久に進めません。実質 1 段 undo しかできない状態になります。「同一 scene にオブジェクトを 2 つ spawn して Ctrl+Z を 2 回」という最初の 5 分でやる操作で踏みます。

**なぜバグと判断したか**

- `last_writers` への代入は通常 edit と revert の 2 箇所だけで、erase も巻き戻しもリポジトリ全体に存在しないことを grep で確認しました。
- 第 1 検査は同一 actor をスキップして自分の連続編集を undo 可能にする設計なので、第 2 検査がその意図を打ち消しています。undo_stack は actor 別なので source と revert の actor が食い違うケースは構造上存在しません。
- 2 段目 undo でも postcondition 自体は成立するため、失敗要因は第 2 検査だけです。
- 既存テストは 1 段 undo → redo のサイクルしか見ておらず、2 段以上の undo を行うテストは 1 件もありません。設計文書と実装マニュアルは多段 undo を前提に書かれています。
- ただし revert 側の stamp 自体は redo に必要(redo の source は undo record)なので、単純削除では redo が壊れます。一行タイポではなく設計上の穴です。

**修正の方向**

`last_writers` を「path ごとの単一 stamp」から「path ごとの writer スタック」に変え、通常 edit で push、undo 成功時に pop、redo で push し直す形にするのが最も整合的です。第 2 検査は top と source の transaction_id を比較します。これなら 2 段目 undo で top が元の transaction に戻り、redo の前提も同時に満たせます。回帰テストとして「同一 path 2 回編集 → undo 2 回」「同一 scene に spawn 2 回 → undo 2 回」「undo 2 段 → redo 2 段」を追加すべきです。

### bug-5: 非 behavior の add_component の inverse に component_index が無いのに、実行側が無条件で at("component_index") を読む

**重大度**: high

**場所**: [`src/core/communication/editorruntimefactory.cpp`](../../src/core/communication/editorruntimefactory.cpp):743-749 — `executeEditorProjection` / `EditorEditCoordinator::Impl::prepareAdd`

**何が起きるか**

`prepareAdd` は add_component の inverse を `remove_component` として組みますが、`component_index` を設定するのは behavior 分岐の中だけです。一方 `executeEditorProjection` は add/remove の分岐に入った直後、behavior 判定より前に `operation.at("component_index")` を無条件で評価します。したがって collider や light のような非 behavior コンポーネントを追加して undo すると、undo チケットは受理されるのにフレーム境界の実行で `key 'component_index' not found` が投げられ、ticket が failed になります。文書が変わらないため undo_stack は pop されず、その actor の undo / redo は以後恒久的に使用不能になります。

**なぜバグと判断したか**

- inverse を後から補完する経路は存在せず、`inverse["component_index"]` への代入は behavior ブロック内の 1 箇所だけであることを全ファイル grep で確認しました。
- 実行経路のすり替えもありません。undo は保存済み inverse をそのまま execute へ渡し、本番の execute は `executeEditorProjection` に束縛済みです。
- 問題の `at()` は behavior 判定より前、binding の null 判定より前、codec 解決より前に無条件評価されます。
- 受理時の dry-run も実行時の canonical 変換も component_index を読まないため早期 reject されず、「受理 → フレーム境界で失敗」という最悪の形になります。
- 既存テストのハーネスは behavior のときしか component_index を読まない自前 execute を持っており、この挙動を意図として固定しているテストは存在しません。
- 唯一の緩和要因は、エンジン内蔵 ImGui インスペクタが add_component を発行しないため RPC クライアント経由でしか踏めない点です。

**修正の方向**

生成側と消費側の両方を直すのが望ましいです。生成側は `inverse["component_index"]` の代入を behavior 分岐の外へ出し、`prepareRemove` が forward / inverse の双方で常に component_index を持つのと対称にします。消費側は無条件の `at()` をやめ、component_index を必要とする分岐の直前で読むようにし、非 behavior の remove_component では component_slot から実インデックスを解決します。`value("component_index", 0)` で埋めるのは誤ったインデックスを消す危険があるため不可です。

### bug-6: behavior attach 経路だけが authored.at("params") を無条件参照し、params 省略の authored JSON で例外になる

**重大度**: high

**場所**: [`src/core/communication/editorruntimefactory.cpp`](../../src/core/communication/editorruntimefactory.cpp):760 — `executeEditorProjection`

**何が起きるか**

同じ関数の set_component_value 経路は `contains("params")` で省略を正しく扱っているのに、behavior attach 経路だけが `authored.at("params").dump()` と無条件参照になっています。scene ファイルに `{"name":"behavior","type":"Foo"}` と params を省略して手書きされたオブジェクトの behavior を remove_component し、それを undo すると、フレーム境界で `key 'params' not found` が投げられて ticket が failed になります。document は未変更のまま undo_stack が pop されないため、その transaction より前の undo 履歴全体が到達不能になります。params 省略形はリポジトリ同梱の実シーンにも存在する正当な入力です。

**なぜバグと判断したか**

- 前進編集では `canonicalBehaviorComponent` が params 欠落を補うため常に "params" を持ちますが、undo は再 prepare せず保存済み inverse をそのまま実行します。inverse が積む component は authoring 文書の生 JSON です。
- authoring 文書はロード時に正規化されません。「raw tree を authoring authority として保持する」と明記されており、normalize 側に behavior 固有処理は 1 件もありません。
- params 省略は不正入力ではなく、シーンロード経路が明示的に空 params として扱っています。既存テストも undo 後に params が省略のまま保持されることを保証しています。
- ただしそのテストハーネスは execute をスタブ実装しており、実 `executeEditorProjection` を通す behavior attach のテストは 1 本もありません。
- 受理時の検証も write_set と postcondition しか見ないため、手前で弾かれることもありません。

**修正の方向**

760 行を set_component_value 経路と同じ形に揃え、`contains("params")` で取り出したうえで `.dump()` ではなく `canonicalizeBehaviorParams` を通します。これで params 省略の例外と、非正規形 params が arena の canonical_params に流入する問題の両方が同時に閉じます。あわせて、スタブではなく実プロジェクション経路で「params 省略 behavior の remove → undo」を回す回帰テストを追加すべきです。

### bug-10: MutationScope が core 単位の単一ラッチなので、prepare を全部先に回す projection トランザクションに ECS 構造変更 adapter を 2 個以上含められない

**重大度**: high

**場所**: [`src/core/userpublic/details/ecs/coretemplate.cpp`](../../src/core/userpublic/details/ecs/coretemplate.cpp):297-302 — `ECSCoreTemplatePublic::MutationScope`

**何が起きるか**

MutationScope の構築子は `mutation_active` が立っていれば `logic_error("ECS structural mutation is not reentrant")` を投げます。各 prepare はこのスコープを token に持たせて publish / rollback まで解放しないため、「全 adapter prepare → 全 adapter publish」という 2 フェーズのトランザクションに構造変更 adapter が 2 個入ると、2 個目の prepare で必ず例外になります。しかも destroy 操作は子孫を含む閉包に展開されるので、子を 1 個持つ親を削除するだけで adapter が 2 個になります。つまり**子を持つオブジェクトは一度も削除できません**。葉オブジェクトの削除は成功するため、葉だけ試していると気づけません。

**なぜバグと判断したか**

- `mutation_active` はカウンタでもスレッドローカルでもない core 単位の単純な bool で、再入で確実に logic_error になります。
- commit が厳密な 2 フェーズであること、lifecycle adapter が object_id ごとに 1 個ずつ生成されることを実コードで確認しました。フィルタも早期 return もありません。
- 上流にクランプも分割もありません。バッチ全体が 1 リクエストにまとめられ execute は 1 回だけ呼ばれます。
- binding は現シーンの名前付き全オブジェクトに張られるため、「binding が無いから別の例外で先に落ちる」という逃げ道もありません。
- 既存テストは必ず前の token を解放してから次を prepare する逐次パターンのみで、同時保持のテストは 1 件もありません。
- 決定的なのは文書同士の正面衝突です。ECS 側の解説は「prepared token を持っている間は構造変更が全て logic_error」と述べ、エディタ設計は「閉包削除を 1 トランザクションで」と要求しています。コードは両方を実装しており両立していません。
- 壊れ方は Failed で終わるだけで ECS 状態は無傷です。データ破壊がないため critical ではなく high としています。

**修正の方向**

`mutation_active` を参照カウントに変えるのは誤りです。rollback の assert が「prepared token が chunks_storage の末尾を占有し続ける」ことに依存しており、単純に多重許可すると失敗原子性が壊れます。推奨は ECS 側に複数エンティティ対応の prepared token(`prepareDestroyMany` / `prepareCreateMany`)を新設し、1 個の MutationScope が N 体分の状態を保持する形へ一般化することです。あわせて lifecycle adapter を object_ids 全体で 1 個にまとめます。再発防止として、1 トランザクションに lifecycle adapter を 2 個入れるテストと、親 + 子の destroy を実 ECS で通す統合テストが必要です。

### bug-16: FileWatcher のワーカースレッドに例外バリアが無く、走査中の filesystem_error が std::terminate になる

**重大度**: high

**場所**: [`src/core/watch/filewatcher.cpp`](../../src/core/watch/filewatcher.cpp):313-327, 468-480 — `FileWatcher::Impl::workerMain` / `Impl::scan`

**何が起きるか**

`scan()` は iterator 構築も属性判定も relative もすべて error_code 版を使って続行する設計なのに、`++it` だけが throwing overload です。MSVC の実装は次の階層を開けなかったときに `filesystem_error` を投げ、`skip_permission_denied` は権限エラーしか吸収しません。ワーカースレッドの起動側には try/catch が一切ないため、走査中にサブディレクトリが消えると例外がスレッド関数から抜けて std::terminate になります。ログも teardown も Vulkan の後始末も走らないままプロセスが即死し、競合依存なので「たまに落ちる」形で現れます。

**なぜバグと判断したか**

- 該当マシンの MSVC ヘッダを実際に読み、`operator++` が失敗時に `_Throw_fs_error` へ到達すること、remap されるのは権限拒否と空ボリューム特例だけで、消えたディレクトリはそのまま throw になることを確認しました。
- ワーカースレッドは本番で回ります。manual_clock の既定は false で、ReloadService はオプションを渡さず start します。ゲートはプレイヤーの非 headless 実行で既定有効です。
- 外側の try/catch はメインスレッドのもので別スレッドからの逸出は捕捉できず、プロジェクトコードに `std::set_terminate` は存在しません。
- 決定的テストはすべて manual_clock でテストスレッド上から回すため terminate 経路が再現されず、start() を使う統合テストもディレクトリ消失を一度も再現していません。
- 報告が過小評価していた点として、最も広い窓はネットワークストアです。ネットワークストアは native watch から外れて 2 秒ごとの再帰走査に落ちるため、一過性のネットワークエラーも同じ throw 地点に到達します。

**修正の方向**

直接原因として `++it` を error_code 版に置き換え、失敗時はクリアして続行する既存方針に揃えます(失敗時は iterator が end になるため、実質「消えた階層以降を諦めて次回 poll で拾い直す」縮退になります)。多層防御として workerMain 全体と NativeWatch のスレッド関数を catch で包み、ログを出して degraded に落とします。これで bad_alloc など他の逸出経路もまとめて塞げます。あわせて `extendedPath` / `isNetworkStore` の error_code なし `absolute` も置き換えるべきです。

### bug-20: GJK の simplex が 5 頂点に膨らみ得るため closestForSubset が std::array<int,4> / weights[4] を範囲外書き込みする

**重大度**: high

**場所**: [`src/core/phys/physquerysweep.cpp`](../../src/core/phys/physquerysweep.cpp):229-243, 304-315, 377-379 — `closestForSubset` / `reduceSimplex` / `convexDistance`

**何が起きるか**

simplex を 4 頂点以下に保つ不変条件を守っているのは `convexDistance` の早期 return ただ一つで、その閾値 `kDistanceTolerance^2`(4e-10)は**絶対値**です。ところが残差の数値誤差は Minkowski 差頂点の大きさ R に比例するため、half_extents 1000 の地面ボックスのような大きな形状(R ≈ 2828)では残差が閾値を 3〜4 桁上回り、早期 return が発火しません。すると `reduceSimplex` が 4 頂点を全部残し、push_back で 5 頂点になり、次の反復で `indices[4]` / `local_weights[4]` / `solveLinear` 内の `matrix` へスタック上の範囲外書き込みが起きます。大きな地面にキャラクタカプセルがめり込んだ状態での shapeCast で踏みます。

**なぜバグと判断したか**

- `shapeCast` は overlaps チェックの前に無条件で `convexDistance` を呼ぶため、「重なった形状での GJK」は必ず走ります。
- 形状寸法に上限クランプはリポジトリ内に存在せず、`validShape` は有限性と非負しか見ません。ワールドスケールが乗るのでむしろ拡大しえます。
- 誤差の見積もりを Gram 成分の作り方(float の dot)まで遡って行い、R ≳ 100 で残差が閾値を破ることを確認しました。progress チェックも duplicateVertex も、Capsule/Sphere が絡むと救済になりません。
- 関連配列はすべて「頂点数 4 以下」前提のサイズで、`operator[]` 素通し、assert も `at()` もありません。`reserve(4)` は容量ヒントにすぎません。
- 既存テストは半径 0.5・half_extents {1,1,1}・中心距離 5 までしか使っておらず、「小さい形状では守られる」ことしか固定していません。閾値が絶対値であることを意図として明示した記述もコメントも docs にもありません。
- Jolt はオプトインで既定はこの builtin プロバイダです。

**修正の方向**

不変条件「simplex は高々 4 頂点」を制御フローで強制します。`reduceSimplex` 後に `simplex.size() >= 4` なら「原点を包含した」と判定して return し、現在の早期 return が担っている役目を脆い浮動小数点閾値ではなく組合せ的条件で担保します。`closestForSubset` にも 4 頂点超の早期 nullopt を置きます。あわせて早期 return の閾値を simplex 頂点の最大ノルムに対する相対項付きにし、Gram 成分を double で内積を取ることで、そもそも発火頻度を下げられます。回帰テストは half_extents {1000,1,1000} のボックスにカプセルをめり込ませた shapeCast です。

### bug-28: コンパイル済みフレームグラフの read_after_write バリアが、レンダーターゲット(画像)に対して無言で捨てられる

**重大度**: high

**場所**: [`src/core/vkcore/renderer.cpp`](../../src/core/vkcore/renderer.cpp):617-624 — `executePlannedFrameGraph` / `ComputeTaskContainer::bufferReadAfterWriteBarrier`

**何が起きるか**

ノードごとに呼ばれるのは `bufferReadAfterWriteBarrier` だけで、その実装は先頭で「フレームグラフのバッファでない資源」をすべて落とします。barrier のリソース名はレンダーターゲット名なので、プランナが載せ実行系がコンパイルしたバリアが実行時に 1 本も積まれません。多くのケースはレイアウト遷移が偶然カバーしますが、レイアウトトラッカーは old と new が同じなら早期 return するため、両パスとも同じレイアウトのまま重ね書きするパターンでは何も出ません。出荷デフォルトの hybrid_v1 パイプラインがまさにこの形で、3 つのレンダーパスインスタンスが同じアタッチメントへ barrier なしで連続書き込みします。

**なぜバグと判断したか**

- フレームグラフ実行経路で image barrier を出すのはレイアウトトラッカーの `transition` ただ一つで、それが早期 return することを確認しました。
- 上流バリデーションはむしろ救済を不可能にしていました。非 fullscreen パスの input_targets を例外で拒否し、fullscreen でも同じ color target を read と write に置くことを拒否するため、`color_load_op: "load"` で読む対象を input_targets に宣言する方法が構造的に存在しません。
- プランナ側は確かにバリアを積みます。ゴールデンプランに `read_after_write` の記録が実在します。
- テストは固定していません。renderer の実行トレースゴールデンに "barrier" の出現は 0 件で、プランのゴールデンは JSON しか検証していません。実デバイスを使うテストも debug messenger 未登録のため validation メッセージが失敗になりません。
- Vulkan 的にハザードであることは、このリポジトリ自身の過去の WP 報告が同クラスの `SYNC-HAZARD-READ-AFTER-WRITE` を実検出・修正した記録で裏付けられます。その修正はレイアウト遷移が起きることに依存しており、レイアウト不変の連鎖では同じハザードが残ります。
- ただし報告のもう一方のトリガ「同一 storage image の compute → compute」は既知の文書化済み制約であり、新規バグとしては除外しました。未認識なのは render → render の同レイアウト連鎖のほうです。

**修正の方向**

バリア発行をリソース種別で分岐させ、バッファでないときに黙って return せず、concrete なレンダーターゲットなら `vk::ImageMemoryBarrier` を出します。レイアウトは現在値を old / new 両方に使い、純粋な memory dependency として発行します。責務の置き場所としては、フレームグラフのバリア発行を専用モジュールへ切り出すか、現在レイアウトを握っているトラッカーに `memoryDependency` を足すのが素直です。回帰は、発行したバリアを実行トレース JSON に記録してゴールデン化し、debug messenger を登録して `SYNC-HAZARD-` をテスト失敗にすることで固定します。

### bug-29: コンピュートタスクのディスクリプタセットが登録時に一度だけ解決され、リサイズ後もヒストリ面反転後も再バインドされない

**重大度**: high

**場所**: [`src/core/renderingpass/computetask.cpp`](../../src/core/renderingpass/computetask.cpp):340-432 — `ComputeTaskContainer::createDescriptorSet` / `registerComputeTask`

**何が起きるか**

ディスクリプタセットは登録時点の image view を焼き込み、登録関数は名前で早期 return するため作り直されません。再バインド API も存在しません。リサイズ経路はフルスクリーンパスだけを面倒見ており、コンピュート側には触れません。したがって (a) ウィンドウをリサイズすると、旧 view が削除キュー経由で確実に破棄されたあとも古いハンドルを指したまま dispatch され、use-after-free やデバイスロストになります。(b) history: true のターゲットを参照している場合はディスクリプタが登録時の面に固定される一方、レイアウト遷移は毎フレーム反転する側に対して行われるため、レイアウト不一致の UB になります。

**なぜバグと判断したか**

- 登録関数の呼び出し元は起動時の 1 箇所だけで、リサイズハンドラは 3 つの処理しか行わずコンピュートコンテナに触れません。public API に再バインド系は存在しません。
- 「コンピュートがレンダーターゲットを bind する構成は実在しない」という反証は失敗しました。テスト成果物のパス定義が実際に `STORAGE` usage のターゲットを compute の reads / writes に書いており、マニュアルもコンピュートタスクをユーザー向け機能として記載しています。
- 旧 view は削除キューに defer され、in-flight フレーム後に確実に破棄されます。生き残りません。
- リバインドを検証するテストはフルスクリーン側の 1 箇所だけで、リサイズを行うゴールデンケースはコンピュートタスクを持たないプロジェクトです。
- (b) については `reads: ["x@history"]` と書くと起動時に hard error になることが判明しましたが、プレーン名で history: true のターゲットを参照する経路は完全に通るため、欠陥自体は残ります(プランナが `@history` を受理するのに実行系が throw する、という食い違いも別途あります)。
- 対照的にフルスクリーン側はパリティごとに 2 セット持ちリサイズ時に貼り直されます。コンピュート側だけが両方の仕組みを欠いています。

**修正の方向**

`ComputeTaskContainer` に再バインド API を追加し、リサイズ経路からフルスクリーン側の隣で呼びます。history 対応はフルスクリーン側と同じくパリティごとに 2 セット確保し、dispatch 時に選択します。当面 history 対応を入れないのであれば、history: true の RT を compute の reads / writes に見つけた時点で hard error にし、静かに壊れる代わりに起動時に落とすべきです。あわせて `@history` サフィックスの扱いをプランナと実行系のどちらかに寄せる必要があります。

### bug-1: getDataFromAccessor が accessor index / bufferView index を検証せず [] でアクセスする

**重大度**: medium

**場所**: [`src/core/model/gltf.cpp`](../../src/core/model/gltf.cpp):600-604 — `getDataFromAccessor` / `loadAnimationClip` / `selectSkin`

**何が起きるか**

`getDataFromAccessor` は accessor と bufferView を範囲検査なしの `operator[]` で引きます。呼び出し元のうち animation sampler の入出力と skin の inverseBindMatrices は index の妥当性を一切確認していません。壊れた、あるいは悪意ある glTF を読むと範囲外読み出しになり、ガベージな byteOffset / stride でバッファを読むためクラッシュに至ります。仕様準拠のファイルでも危険で、`accessor.bufferView == -1` は glTF 2.0 で合法なため、sparse 変換を通したアニメーション付きモデルで `bufferViews[-1]` に到達しえます。

**なぜバグと判断したか**

- tinygltf は animation / skin の accessor index を検証しません。post-parse の検証パスも存在せず、負値もそのまま Model に載ります。
- pelican 側の上流にも検証はなく、プロジェクトの .glb / .gltf が直接ローダへ渡ります。
- type 不一致チェックは 3 つの `operator[]` より後ろにあるため、早期 return による回避もありません。
- 同ファイルの morph 経路と VRMA デコーダは、同じ関数を呼ぶ前に accessor index / bufferView / buffer / stride / byte range をすべて検査して throw します。つまり「未検証 index は安全でない」は本ファイルの設計前提であり、animation / skin 側の欠落は意図ではありません。
- `inspect()` の直前コメントが「accessor / rig のエラーは Vulkan オブジェクトに触れる前に副作用なしで fail する」と明文の契約を述べており、無検査アクセスはその走査自体をクラッシュ地点にしてしまいます。
- 副次的に、base bufferView を持つ sparse accessor では crash せず「sparse 上書きを無視した誤ったアニメーション値」を黙って読む第 2 の壊れ方があります。

**修正の方向**

morph 経路 / VRMA デコーダと同じ検査を `getDataFromAccessor` の先頭へ移設して共通化します。accessor index の範囲、sparse の明示的な拒否、bufferView index の範囲、buffer index の範囲、そして byte range が buffer に収まることをすべて throw で弾きます。これで animation と skin だけでなく、tinygltf が黙って skip する primitive attribute 経路も同時に塞げます。エラーメッセージには animation 名や skin 名などの文脈を含め、既存の morph テストと同形式で固定します。

### bug-3: basic_config.camera.up が配列でない / 要素数 3 未満のとき、検証なしに operator[] を 3 回引く

**重大度**: medium

**場所**: [`src/core/loader/basicconfig.cpp`](../../src/core/loader/basicconfig.cpp):531-532 — `ProjectBasicConfig::ProjectBasicConfig`

**何が起きるか**

`camera.up` を `is_array()` も `size() >= 3` も検査せずに 3 回添字します。同ファイルの他の basic_config フィールドは型と値域を明示検証して throw しており、up だけがその規約から外れています。プロジェクト JSON の up を `[0, -1]` のように 2 要素で書くと、Release ビルドでは確保末尾を 16 バイト踏み越えて読み、隣接ヒープの内容次第で意味不明な例外・ゴミ値での誤姿勢描画・野良ポインタ逆参照のいずれかになります。手書きが前提のフォーマットなので、要素を 1 つ落とすタイポは現実的です。

**なぜバグと判断したか**

- 報告の半分は反証できました。本リポジトリの nlohmann/json 3.12.0 の const `operator[]` は非配列に対して明示的に throw するため、オブジェクトや文字列を書いたケースは正しく例外になります。UB にはなりません。
- 反証できなかったのは要素数 3 未満の配列です。上流検証は存在せず、未知フィールド拒否も camera 本体には掛かっていません。JSON Schema バリデータもリンクされていません。
- カメラパラメータはフィールド単位で 3 ソースを走査するため、up だけ壊れていても他フィールドは既定値で充足され、必ず該当行へ到達します。
- テストによる意図固定もありません。フィクスチャは camera 自体を省略しており、up の異常系テストは存在しません。
- むしろ報告より悪い点として、`getVal` が値返しのため vector のコピーは capacity == size ちょうどで確保され、余裕スラックがゼロです。さらに glm のベクトル構築子が値渡しなので、範囲外の JSON 値が「コピー構築」されます。
- 4 要素以上を無警告で捨てる挙動も、同ファイルの他フィールドの厳格さと矛盾しています。

**修正の方向**

同ファイル内の既存規約に揃え、配列であること、要素数がちょうど 3 であること、各要素が数値であることを明示検査して専用メッセージで throw します。さらに有限性と非ゼロベクトル(長さ 0 の up は後段の正規化で NaN になります)まで見るのが望ましいです。`at(i)` に替えるだけでも UB は消えますが、エラーメッセージが JSON 例外になるため専用 throw を推奨します。あわせて未知フィールド拒否を camera 本体にも掛けると再発を構造的に防げます。

### bug-7: descendant_world の descendants 集合が scene ループの外にあり、別 scene の同名親を持つオブジェクトが結果に混入する

**重大度**: medium

**場所**: [`src/core/loader/editorpreviewprojection.cpp`](../../src/core/loader/editorpreviewprojection.cpp):746-774 — `EditorPreviewEvaluationContext::evaluate`(kind == "descendant_world")

**何が起きるか**

descendants 集合が scene ループの外側で宣言され、全 scene の閉包計算と収集が同じ集合を共有し続けます。包含判定は親の名前一致だけで scene_id を比較しません。名前は scene 内でのみ一意なので、複数 scene を持つプロジェクトで descendant_world を投げると別 scene のオブジェクトが結果に混入します。しかも集合には root 自身の名前が seed されるため、親子関係すら不要で、別 scene に同名オブジェクトがあるだけで混入し、そこから子孫へ閉包が伸びます。ギズモの子孫プレビューや複数選択の world 変換表示が別 scene を対象に含めます。

**なぜバグと判断したか**

- 名前の一意性は scene ごとです。正規化は scene ごとに名前カウントを作り直し、名前解決も scene_id 一致を条件にしており、ソースコードガイドも「scene が違えば同名でも別物」と明記しています。
- 文書が単一 scene とは限りません。authoring 文書は全 scene を走査し、複数 scene のフィクスチャで実際にテストが通っています。scene 数の上限もありません。
- preview 経路が現 scene に絞られることもありません。文書全体が配線されており、`evalPreview` は scene_id を受け取らずフィルタもありません。
- テストは単一 scene のフィクスチャ 2 件のみで、multi-scene のカバレッジがありません。
- 決定的なのは同一リポジトリ内の双子実装です。journal 側の subtree 列挙は同じ名前ベースの閉包を実装しつつ、scene_id が異なる候補を明示的に除外しています。projection の transform 意味論は scene ローカルなのに、descendant クエリだけが文書横断になっています。
- read-only クエリなので状態破壊はなく、返る対象集合と数値が誤るだけです。

**修正の方向**

descendants 集合の構築を scene ループの内側へ移し、root が属する scene 以外はスキップします。journal 側が既に採用している scene_id ガードと同じ形にするのが自然です。root の scene を特定するには、評価済みオブジェクト検索が scene_id を返すようにするか、scene ループ内で object_id 一致により判定します。あわせて同名オブジェクトを持つ multi-scene フィクスチャで回帰テストを追加します。

### bug-11: archetype 移行・トークン経由の entity 生成が毎回 CHUNK_CAPACITY 分の chunk を新規確保して 1 体だけ載せる

**重大度**: medium

**場所**: [`src/core/ecs/archetypemigration.cpp`](../../src/core/ecs/archetypemigration.cpp):96-101, 482-487 — `ECSArchetypeMigration::prepare` / `State::publish` / `ECSEntityMutation::prepareCreate`

**何が起きるか**

prepare は必ず新しい chunk を作って 1 体だけ載せ、publish は無条件に末尾へ足します。空き容量のある既存 chunk を一切参照しません。一方 `createEntities` は残容量のある既存 chunk を探して再利用します。chunk のストレージは常に満杯ぶん確保されるため、1 回の移行や生成ごとに「容量 4096・実体 1」の chunk が積まれ、統合も解放もされません。数百体 spawn で数十〜数百 MB になり、同時に全システムの走査対象 chunk 数が編集回数に線形比例して伸びるため、編集を続けるほどエディタのフレーム時間が劣化します。

**なぜバグと判断したか**

- 末尾追加自体は rollback の撤収規約を成立させるための実装都合で、その旨はソースコードガイドにも記述があります。しかし「1 体につき 4096 枠を確保し回収しない」ことをコスト込みで受け入れた記述はどこにもありません。
- 逆に設計文書と過去のレビューは「fresh chunk を空のまま残す弱保証を採るならメモリ上限をテストで固定せよ」と要求しており、その上限テストは存在しません。
- テストによる意図固定もありません。移行側の chunk 数やメモリを固定する assertion は皆無で、既存のチャンク分割テストは再利用する側だけを見ています。
- 実運用の呼び出し元は多数あります。add_component / remove_component が 1 件ごとに、spawn / destroy / restore_objects が 1 オブジェクトごとに token を作ります。
- リセットもされません。`clearEntities` に到達するのは明示的なシーンロードと DLL リロードと teardown だけで、編集の commit も save もリロードしません(save は hot reload しないと明示しています)。
- 副次的に、毎回の `reserve(size + 1)` により操作ごとに chunks_storage 全体が再確保され、全 chunk が move されます。

**修正の方向**

prepare / prepareCreate を `createEntities` と同じ規則に揃え、同一 archetype で残容量のある chunk があればそこへ追記し、無い場合のみ末尾へ新規追加します。その際 token に「既存へ追記したか / 新規を末尾に足したか」を持たせて rollback を分岐させ、末尾占有を前提とした assert を置き換えます。空になった chunk を erase してはいけません(全 chunk index がずれます)。回収は「空 chunk を再利用可能スロットとして次の生成で埋める」方向、つまり上記の再利用で自然に達成する形にします。

### bug-12: ECSCoreTemplatePublic::remove() だけが component version を更新しないため、変更検知に依存するシステムが削除を取りこぼす

**重大度**: medium

**場所**: [`src/core/userpublic/details/ecs/coretemplate.cpp`](../../src/core/userpublic/details/ecs/coretemplate.cpp):550-567 — `ECSCoreTemplatePublic::remove`

**何が起きるか**

`remove()` は chunk から行を抜いたあと version を一切更新しません。同じ「entity 1 体を chunk から抜く」操作でも、mutation token 経由の destroy も archetype 移行も `createEntities` も必ず version を更新します。そのため force 指定なしで登録され、宣言コンポーネントがすべて const なシステムは、chunk の実体数と行の中身が変わっているのに skip され続けます。しかも skip は 1 フレームではなく恒久的で、他の何かが変更を起こすまでその chunk は永久に無視されます。現行の predefined システムはすべて force 登録なので出荷状態では顕在化していません。

**なぜバグと判断したか**

- `removeAt` は行の破棄と再配置しか行わず、version に一切触れないことを確認しました。
- 死んだ経路ではなくユーザー向け破棄の本流です。オブジェクト削除 API からこの関数に到達します。
- skip 述語を実装から再導出し、「宣言がすべて const」という前提条件が必要十分であることを確認しました。書き込みを宣言するシステムは自分で version を更新するため skip されません。
- 非 force 登録は可能どころか既定値です。force のほうが opt-in で、既存テストも既定で登録しています。
- 意図として固定しているテストやコメントはなく、むしろ逆です。既存テストは非 force システムをわざわざ登録して archetype の追加削除が確実に観測されることを固定していますし、過去の WP 報告は「構造変更を変更検知から見落とさない」ことを明文化しています。
- `git log -S` で追うと、この `remove()` は version 更新規律が導入される前の実装であり、意図的判断ではなく未移行の旧経路に見えます。

**修正の方向**

`removeAt` の直後に、`createEntities` や destroy publish と同じく全 index の version を現在 tick へ更新する処理を入れます。`updateVersion` は noexcept なので noexcept 性は崩れません。あわせて、非 force かつ const のみのシステムで削除後に process が呼ばれることを固定する回帰テストを、既存の移行テストと同じ形で追加します。

### bug-14: FileWatcher::Impl::armAll が status.error をミューテックス外で書き換え、FileWatcher::status() のコピーとデータ競合する

**重大度**: medium

**場所**: [`src/core/watch/filewatcher.cpp`](../../src/core/watch/filewatcher.cpp):290-292 — `FileWatcher::Impl::armAll` / `FileWatcher::status`

**何が起きるか**

`armAll` は arm 失敗時に `status.error` をロックを取らずに move 代入します。同ファイルの他の status 書き込みはすべてロック下にあり、この 1 行だけが例外です。読み手である `status()` は同じミューテックスを取って `std::string` をコピーするため、旧バッファの解放とポインタ差し替えがコピー構築と同時進行します。arm 失敗はネットワークストアでのポーリング fallback など設計された常態なので、恒常的に踏みえます。症状は多くの場合ステータス JSON の文字化けですが、最悪は解放済みヒープからのコピーです。

**なぜバグと判断したか**

- 呼び出し側ロック説は成立しません。`armAll` はロックブロックを閉じてから呼ばれます。しかも冒頭の停止処理が watch スレッドを join し、そのスレッドが同じ非再帰ミューテックスを取りえるため、ロック保持で呼ぶと必ずデッドロックします。ロック外呼び出しは構造的に必然で、だからこそ再ロックを忘れています。
- 失敗経路は到達不能どころか設計された経路です。ネットワークストア判定のコメント自身が polling fallback と明記しており、非 Windows のスタブでは常にこの経路になります。
- 単一スレッド説も否定しました。既定オプションでワーカースレッドが起動し、読み手は RPC の get_status などメインスレッド側から到達します。
- テストは意図として固定していません。arm 失敗を強制するテストはすべて manual_clock でワーカースレッドが無く、実スレッドの統合テストでは arm が成功するためこの行が一度も実行されません。組み合わせが未テストなだけです。
- 良性スカラでもありません。書かれる文字列は SSO を超える長さでヒープ確保されます。なお同時に更新される失敗カウンタと次回リトライ時刻はワーカースレッド専有なのでレースではなく、`status.error` だけがスレッド境界を越えます。

**修正の方向**

該当行、最低でも `status.error` への代入を末尾のロックブロック内へ移します。停止処理と arm ループはロック外に残し、watch スレッドの join と通知のロック順序を壊さないようにします。失敗カウンタと次回リトライ時刻も同じブロックに入れておくと、他の読み出し箇所との一貫性が取れます。

### bug-15: NativeWatch::stop() の CancelIoEx が再アーム直前の窓で取りこぼされ、停止が永久にハングする

**重大度**: medium

**場所**: [`src/core/watch/filewatcher.cpp`](../../src/core/watch/filewatcher.cpp):106-171 — `NativeWatch::stop` / `NativeWatch::threadMain`

**何が起きるか**

watch スレッドは完了収集後に停止フラグを一度だけ見て、再アームまで再チェックしません。その区間には保留 I/O が存在しないため、別スレッドが `stop()` を呼んで `CancelIoEx` を撃っても何も取り消されず、戻り値も無視されます。watch スレッドは新しい監視要求を発行して次の変更まで無限にブロックし、`stop()` は完了通知を述語版・無タイムアウトで待ち続けて永久に戻りません。終了シーケンス中は誰も監視ツリーを触らないため、実質的な恒久ハングになります。

**なぜバグと判断したか**

- 停止呼び出し元は watch スレッドとは別のスレッドであり、同時実行を禁じるものはありません。
- 停止側のロックは完了通知の受け渡しにしか使われず、再アーム側はそのロックを一切取りません。停止フラグは atomic ですが、可視性が保証されるだけで時間窓そのものは消えません。
- 危険な並びを整理すると「フラグ読み取り → 停止側が CancelIoEx(保留 I/O なし) → 再アーム発行 → 無限待ち」に限定され、窓は数マイクロ秒ですが、プリエンプトされればミリ秒級に伸びます。閉じている根拠はありません。
- テストは静穏時の停止しか通しておらず、停止時点で watch スレッドは待機に駐留しているため `CancelIoEx` が必ず保留 I/O を掴みます。設計文書も停止手順の順序だけを規定し、「何も取り消さなかった場合」を規定していません。
- 本番で native watcher は動きます。ホットリロードの既定値と gate 条件から、通常のデスクトップ実行では実際に arm されます。
- 頻度が最も高いのは再アームレースです。ストアのどれかが arm に失敗する構成では、バックオフごとに停止と再 arm を繰り返すため、窓に当たる機会が桁違いに増えます。

**修正の方向**

「1 回撃って無限に待つ」構造から外します。第一案は再アームと `CancelIoEx` を同じミューテックスで相互排他にし、再アーム側がロック内で停止フラグを再チェックして、停止中なら発行せずに完了通知を立てて終了する形です。これで取りこぼしが構造的に発生しなくなります。難しければ、待機をタイムアウト付きに変えてリトライごとに全 I/O キャンセルを再発行し、`CancelIoEx` の戻り値も判定してログに出します。あわせて「書き込みバースト中に停止する」統合テストを追加します。

### bug-17: digest がリトライ枯渇 / error で読めなかったファイルが「削除」として reconcile される

**重大度**: medium

**場所**: [`src/core/watch/filewatcher.cpp`](../../src/core/watch/filewatcher.cpp):328-382 — `FileWatcher::Impl::scan` / `FileWatcher::Impl::reconcile`

**何が起きるか**

走査は最大 500 ms のリトライののち、安定した digest が取れたエントリしか結果に入れません。リトライ枯渇と読み取りエラーは「存在しない」と区別されないまま結果から欠落し、reconcile はそれを一律で削除イベントとしてキューに積みます。実在するファイルに対して削除が飛ぶため、モデルやテクスチャの再読み込みが「file is missing」という虚偽のメッセージで失敗し、失敗カウンタと last_reload_error に残ります。特にネットワークストアでは読み取りエラーが 1 回で即座に偽削除になります。

**なぜバグと判断したか**

- reconcile は削除を積む前に存在確認も再試行もしません。欠落判定は live digest の有無だけで、猶予カウンタもありません。
- 下流も無害化していません。各リロードハンドラが parse で例外を投げ、シェーダ経路も種別を無視してディスクを読み直すためビジー中は同様に失敗します。
- テストは意図として固定していません。ビジーや共有違反のケースは存在せず、削除の検証は実削除だけです。むしろ設計文書と過去の WP 報告が「安定 read が得られない間は旧リソース継続 + retry で永久失敗にしない」と明記しており、現実装は文書と矛盾します。同じ digest API を使う別モジュールは retry / error / cancelled を missing と明確に区別しています。
- 読み取りエラーは retry より重い扱いです。1 回も再試行されずに即 break するため、ネットワーク断のような一過性エラーがそのまま偽削除になります。
- 一方で報告の impact は一部過大でした。走査結果は毎回入れ替わるので同じエントリが繰り返し削除扱いされることはなく、旧リソースは保持されるため状態破壊も起きません。ビジー解消後の走査で自己修復します。破壊はしませんが、診断が能動的に間違っており、存在しない削除の調査へ開発者を誘導します。

**修正の方向**

走査の戻り値を「安定して読めた群」と「読めなかった群」に分け、reconcile の削除判定から後者を除外します。読めなかったエントリは前回の情報ごと持ち越し、次回読めたときに差分で正しい変更イベントが出るようにします。読めなかったものが残っている間は dirty を落とさず、確実にもう一周させます。あわせて、恒久的に読めないファイルを黙って隠さないよう、一定回数継続したら削除とは別の診断としてログに出すべきです。

### bug-19: activateReadyEditorAttachments が恒久的に失敗するアタッチメントを毎フレーム無限リトライする

**重大度**: medium

**場所**: [`src/core/gamelogic/behaviorarena.cpp`](../../src/core/gamelogic/behaviorarena.cpp):482-519 — `BehaviorAttachmentArena::activateReadyEditorAttachments`

**何が起きるか**

エディタから attach した behavior の起動に失敗すると、catch はインスタンスを破棄してログを出すだけで、アタッチメント自体を除去しません。破棄処理は初期化済みフラグと active フラグを落とすため、次フレームの skip 条件をすべて外してしまい、脱出条件が存在しません。結果として「生成 → 例外 → 破棄 → ERROR ログ」が毎フレーム、1 フレームあたり「配送イベント数 + 1」回繰り返されます。バックオフも回数上限もありません。もう一方の起動経路は失敗を致命として扱い、巻き戻して rethrow するので、「起動できないアタッチメントを残さない」が本来の不変条件です。

**なぜバグと判断したか**

- 上流の prepare は登録の存在とインスタンス生成までしか行わず、初期化コールバックは一度も呼ばれません。これは設計として文書化されており、初期化失敗を prepare で捕まえることは原理的にできません。
- 初期化コールバックは noexcept ではなく、throw する実装は既存テストでサポート対象として扱われています。
- 脱出条件がありません。登録テンプレートの破棄ラムダが末尾でインスタンスを空にするため、判定は毎回真になり丸ごと再生成されます。
- アタッチメントを消す 3 箇所はいずれも起動失敗では発火せず、リトライ上限やバックオフや隔離フラグは grep しても存在しません。
- シーンロード経路は別扱いで救いになりません。到達母集団はちょうどエディタ attach 分です。
- テストも固定していません。エディタ用フィクスチャは throw しない behavior のみを使い、throw する behavior はもう一方の経路にしか当てられていません。
- 報告が拾えていなかった増幅があります。もう一方の経路は保留中の変更を巻き戻しますが、エディタ側の catch は巻き戻しません。初期化中にオブジェクト生成を呼んでから throw する実装では、毎フレーム 1 体ずつエンティティが増え続けます。
- なお報告のトリガのうち「パラメータのデコードで throw」は初回では到達不能でした。prepare が同じ文字列で成功しているためです。実際に踏めるのは「初期化が決定的に throw する」場合に絞られます。

**修正の方向**

catch を「恒久失敗の終端化」に変えます。失敗フラグを持たせて skip 条件に加えるか、publish 側と同様にアタッチメントを除去します(除去するなら「起動できないアタッチメントを残さない」という不変条件と揃います)。あわせて初期化前に保留変更のサイズを控え、catch で巻き戻します。失敗状態と例外メッセージをインスペクタへ出せるようにし、active=false だけでは pending と区別できない現状を改善すべきです。回帰テストとして、throw する初期化をエディタ attach したときに初期化呼び出しが 1 回で止まることを固定します。

### bug-21: advanceCursor のループ境界が delta_seconds / duration 由来で無制限になり、ハングと float→int64 変換の UB を起こす

**重大度**: medium

**場所**: [`src/core/animation/animationprobe.cpp`](../../src/core/animation/animationprobe.cpp):269-297 — `ProbeRuntime::advanceCursor`

**何が起きるか**

ループ回数が delta_seconds と duration の比にそのまま比例し、上限がありません。入力検査は有限性しか見ないため、単位取り違えなどで巨大な finite 値が渡ると素通りします。repeat カーソルに対しておよそ `|delta| / duration + 3` 回、ミューテックスを保持したまま空回りするため、ゲームスレッドが固まると同時に同じランタイムを触る他の処理もすべてブロックされます。さらに商が int64 の表現範囲を超えると変換が UB になり、x86-64 と ARM64 で結果が変わります。

**なぜバグと判断したか**

- 有限性は弾くのに大きさは見ない、という非対称が実在します。上限のクランプはグラフ評価側にも ABI 構造体にも存在せず、ABI 文書にも上限の契約がありません。
- clamp モードは早期に潰れることを確認できましたが、公開 ABI 経由で作られるカーソルは repeat 固定なので 100% 脆弱側です。
- テストも固定していません。既存の delta 値は小さいか、seek 分岐や clamp モードに入るためこのループを通りません。
- 型や assert による不可能化もありません。
- 一方で報告の impact のうち「注釈がある場合の bad_alloc が C ABI 境界を越える」は公開経路からは到達しないため反証しました。ABI 経由のカーソルは注釈が空です。「1e19 でハング」も不正確で、実際は約 1e10〜9.2e18 の帯でハングし、それ以上は UB による誤結果になります。
- エンジン自身はこの関数を駆動していません。踏むのはユーザー空間の評価器が自分で dt を渡した場合に限られます。ただしこの関数は凍結 ABI として公開され、他のフィールドは厳密に検査している防御的境界なので、「バグでない」根拠にはなりません。
- なお「長時間バックグラウンド後の catch-up」は数万反復にしかならず、モバイル固有のトリガとしては実質否定されます。

**修正の方向**

まず注釈が空なら外側ループを丸ごとスキップします。公開経路の無限ループはこれだけで消えます。次に int64 への変換前に商をクランプし、安全域外なら invalid_argument を返して変換 UB と移植性の穴を閉じます。さらに走査ループ数に明示的な上限を設け、報告可能な件数の上限と揃えた形で弾き、その旨を ABI 文書に契約として明記します。あわせて、同ファイルの他の ABI 関数と揃えて当該エントリポイントを noexcept + try/catch にすべきです。

### bug-22: notify() が sink->object->asset を null 検査なしで参照し、skeletal→非 skeletal のホットリロード後にクラッシュする

**重大度**: medium

**場所**: [`src/core/animation/animationservice.cpp`](../../src/core/animation/animationservice.cpp):1502-1504 — `AnimationServiceRuntime::Impl::notify`

**何が起きるか**

skeletal だったモデルをスキン無しのモデルへホットリロードすると、asset が nullptr になる一方で sink は一切除去されません。この状態でクライアントが「レイアウト世代不一致」の通知を送ると、null 参照でクラッシュします。しかもこの破損状態は恒久的で、あとで skeletal に戻しても早期 return と重複名の throw により復旧しません。同ファイルの他の参照箇所はすべて null を検査しており、ここだけが無防備です。

**なぜバグと判断したか**

- 置換が nullptr のとき asset がそのまま nullptr になる経路を実装で確認しました。リロードは skeletal と非 skeletal の差を受理するだけで拒否しません。
- ただし報告のトリガは反証しました。グラフ評価経由の自動通知では、先に rig 解決が not_found で弾かれて通知に到達しません。
- 残る生存経路は否定できませんでした。評価器の通知メソッドは公開 API で、rig 解決を一切せずサービスへ直行します。生の通知関数ポインタも ABI でゲーム DLL に露出しており、既存テストがクライアント発の不一致通知を直接呼んで ok を要求しています。つまり「意図された・テスト済みの使い方」です。
- 検証関数は構造体サイズと版しか見ず、観測レイアウトを検査しません。短絡評価により当該種別のときだけ参照されます。
- severity を下げた理由はゲート条件です。モデルのホットリロードは開発構成でのみ有効で、出荷ビルドではまず踏みません。DLL アンロード時に無効化されるため同一セッション内に限られ、リロード直後にクライアントが送る自然な種別は別のもので、そちらは安全です。
- 下げきれない理由は、この ABI が「敵対 DLL フィクスチャで守る境界」と明記されており、任意入力に対してクラッシュせず Status を返すことが設計契約だからです。

**修正の方向**

同ファイルの他の箇所と同じ形で asset の生存を先に検査します。ただし単に invalid_argument を返すのは意味的に不適切で、asset が消えている場合は「観測されたレイアウトは確かに現行と一致しない」が真なので、比較をスキップして通知を受理するのが正しい振る舞いです。あわせて、sink を生かしたまま asset を null にする設計自体の見直しにも価値があります。sink の世代を進めて以後の検索を stale にするか、オブジェクトごと除去して not_found を返す形が考えられます。

### bug-30: xrBeginFrame が ready 以外を返しつつ shouldRender が true のとき、endFrameWithoutLayers が必ず logic_error を投げる

**重大度**: medium

**場所**: [`src/core/appflow/loop.cpp`](../../src/core/appflow/loop.cpp):489-527 — `XrCompositionTarget::Impl::endFrameWithoutLayers` / XR フレームループ

**何が起きるか**

ループは「begin が ready」かつ「shouldRender が true」の AND で分岐し、false のとき zero-layer で閉じる専用パスを呼びます。ところがその専用パスは「shouldRender が false であること」を前提に検査しており、begin が非 ready かつ shouldRender が true の組み合わせでは必ず throw します。セッション喪失予告やフレーム破棄はどちらも想定内の戻り値なので、HMD 描画中にランタイムを終了したりケーブルを抜いたりすると発生します。結果として `xrEndFrame` を一度も呼ばないままフレームを開いたまま残し、誤ったメッセージで fatal 終了します。

**なぜバグと判断したか**

- 先に terminal path で抜けてくれないかを調べましたが、begin はカウンタを増やして enum を返すだけで terminal path を触りません。イベントポーリングは同一反復の先頭で終わっているため、戻り値で知った時点では break が効きません。
- 待機側は shouldRender をそのまま載せるだけで begin の結果とは無関係です。セッション喪失予告は失敗コードではないため throw もされません。組み合わせは構成可能です。
- 分岐は完全な二分岐で、else は無条件にこの専用パスを呼びます。回避路はありません。
- 「意図的な設計」を否定しようとして、逆に報告を補強する証拠が出ました。セッション層のテストは shouldRender を既定の true にしたままフレーム破棄を流し、zero-layer で閉じることを CHECK しています。テストケース名自体が「すべてのフレームが zero-layer で終わる」です。セッション層の `endFrame` には shouldRender の検査が一切ありません。つまり composition 側の追加条件はそれと矛盾する過剰な前提です。
- 一点だけ報告を反証しました。impact の「未処理例外でプロセスが落ちる」は誤りで、上位の catch が受けて fatal ログと exit code 1 になり、teardown も走ります。ただし「xrEndFrame を呼ばずに終わる」部分は事実です。

**修正の方向**

zero-layer で閉じる専用パスの拒否条件から shouldRender の項を外し、composition の状態不変条件だけを検査するようにします。これでセッション層の `endFrame` と契約が一致します。あるいはループ側で、begin が非 ready のときは composition target を経由せずセッションの `endFrame` を直接呼ぶ形でも構いません。いずれにせよフレームは必ず一度閉じられ、状態が idle に戻る必要があります。あわせて loop の XR 分岐を対象にした回帰テストを追加します。

### bug-31: XR セッション中はデスクトップスワップチェインが再生成されず、ウィンドウリサイズ後ミラーが恒久的に停止する

**重大度**: medium

**場所**: [`src/core/vkcore/swapchainframetarget.cpp`](../../src/core/vkcore/swapchainframetarget.cpp):259-264, 409-419 — `SwapchainFrameTarget::beginFrame`(nonblocking)/ `XrCompositionTarget::consumeExtentChanged`

**何が起きるか**

nonblocking 経路はスワップチェインが古くなっても再生成せず nullopt を返すだけで、「後続のフラットフレームが通常のブロッキング再生成を行う」というコメントに委ねています。しかし XR ループはセッション実行中フラットフレームを完全にスキップするため、そのフラットフレームが来ません。結果として XR セッション中にデスクトップウィンドウをリサイズすると回復パスが構造的に到達不能になり、ミラー表示がセッションの間ずっと止まります。HMD 側の描画には影響しません。

**なぜバグと判断したか**

- 再生成の呼び出し箇所は 2 つだけで、どちらも nonblocking では到達しません。nonblocking に入る唯一の経路は XR ミラーであり、そこからは絶対に再生成されません。
- ブロッキング経路の呼び出し元を全列挙し、セッション実行中は XR 分岐の continue によりフラット経路が完全にスキップされることを確認しました。shouldRender が false の分岐も同様に continue へ落ちるため抜け道になりません。
- ウィンドウ側に別のリサイズフックはありません。フレームバッファサイズのコールバックは登録されておらず、参照はポーリング専用です。ウィンドウのリサイズ自体は無効化されていません。
- テストと設計文書も固定していません。「nonblocking 経路はスワップチェインを作り直さない」は意図として尊重すべき不変条件ですが、「代わりに誰かが再生成する」が欠落しています。XR 中にフラットフレームが来ないことは docs のどこにも書かれていません。
- 古くなった状態は一過性ではなく、不一致が解消されるまで永続します。ただし nullopt 復帰はフェンスのリセット前なので同期の整合は壊れず、壊れ方は「ミラーが止まる」だけです。
- 報告の訂正点が 2 つあります。extent 変更フラグを消費する主体がない点は有害さの本体ではなく、セッション停止後の最初のフラットフレームで正しく回収されます。また「恒久的」はプロセス寿命ではなくセッション寿命で、セッションが停止すれば自己修復します。

**修正の方向**

docs の不変条件は維持したまま、XR ループ側に再生成の主体を作ります。スワップチェイン側に stale フラグを追加して nonblocking の失敗時に立て、公開メソッドとして「stale なら再生成する」を用意し、XR 分岐でミラーの present 直後(XR コンポジションのクリティカルパス外)に呼びます。再生成は待機を含むため、stale のときだけ呼ぶこと、必要なら間引くことが重要です。ミラーが自分の acquire ループ内で再生成する形にはしないでください。最低限の緩和として、連続ドロップが続いたら一度警告を出すべきです。

### bug-2: scenes が空の glTF で model.scenes[0] を範囲外アクセスする

**重大度**: low

**場所**: [`src/core/model/gltf.cpp`](../../src/core/model/gltf.cpp):1187 — `selectLoad`

**何が起きるか**

fragment 指定なしのモデル全体ロード経路で、既定シーン index を三項演算子で選んで無条件に添字します。glTF 仕様上 scenes は必須ではなく、tinygltf は scenes を空・既定シーンを -1 のまま返すため、三項演算子は 0 を選んで空 vector に添字します。Release では実質クラッシュで、しかもファイル名を示すエラーメッセージすら出ません。同じ行は既定シーン index が要素数以上の場合も範囲外になります。

**なぜバグと判断したか**

- tinygltf は scenes を必須にしていません。ヘッダに「scene is not mandatory」というコメントがあり、既定シーン index のクランプも検証もしません。
- 呼び出し元が状態を作らない保証もありません。宣言検証は拡張子しか見ず、fragment を省略する経路が複数あります。
- 早期 return による回避もなく、副作用のない検証を意図した経路より前に踏みます。
- テストも固定していません。フィクスチャは常に scenes を書き出しています。ただし devcli 側には空 scenes を明示的に検査して例外を投げる箇所があり、「空 scenes はありうる」という認識自体はコードベース内に存在します。ランタイムローダ側だけが同等のガードを欠いています。
- fragment 経路は scenes 不在や未到達ノードを許容するフォールバックを持っており、全体ロード経路だけがこの配慮を欠く非対称があります。
- low とした理由は、主要エクスポータが必ず scenes を書き出すため実配布アセットで踏む確率が低く、被害がロード時のプロセス落ちに限られるためです。

**修正の方向**

添字の直前にガードを入れます。scenes が空なら fragment 経路と同じく「どのノードの子でもないノード」を root として列挙するフォールバックに落とします(scenes 不在は仕様上 valid なので throw より妥当です)。既定シーン index は範囲内のときだけ採用し、外れていたら 0 へフォールバックするかソースパスを含む例外にします。いずれにせよ同ファイルの他の索引箇所と同じ throw ベースに統一します。

### bug-23: VRM 表情の override 適用が自身の書き換え結果を読み返し、結果が表情名のアルファベット順に依存する

**重大度**: low

**場所**: [`src/core/animation/vrmapplication.cpp`](../../src/core/animation/vrmapplication.cpp):251-281 — `resolveExpressionFrame`

**何が起きるか**

外側ループは重みマップを参照で回しながら末尾で書き換え、内側の寄与元読み取りは同じマップから読みます。マップは辞書順に走査されるため、先に処理された寄与元は既に override 適用後(多くは 0)の値になり、寄与が丸ごと失われます。結果として、意味的に同一な設定でも表情名の辞書順によって抑制対象が反転します。直前のコメントは「寄与元は override 適用前の値を使う」と明言しており、実装がその不変条件を満たしていません。

**なぜバグと判断したか**

- エイリアシングの実在をコードで確認しました。同一グループの除外は救いになりません。書き換えられうる要素同士は互いに異グループなので寄与元として読まれます。
- 上流での検証やクランプもありません。パーサは preset と custom を区別せず override を無条件に受理し、「手続き的 preset は override を持てない」という制約はどこにもありません。
- テストも固定していません。override を使うテストは 1 件だけで、その寄与元は書き換わらないグループのため修正前後どちらでも通ります。順序依存経路を固定するフィクスチャは存在しません。
- 型による不可能化もなく、辞書順走査は確定しています。
- 報告の「リグの表情名を変えただけで結果が変わる」は不正確です。custom 表情は preset 名と衝突できず、手続き的な名前は固定なので改名できません。ただし改名なしでも同じ非対称は起き、リップシンクが特定の母音を出したフレームで抑制対象が反転します。
- low とした理由は、一般的なエクスポータが override を感情 preset にしか付けないため、実際に踏むのは手編集の VRM に限られるためです。

**修正の方向**

override ループに入る前に寄与元の値を不変スナップショットとして退避し、内側は必ずそちらから読むようにします(あるいは block / blend を先に全部集計してから第 2 パスで一括適用します)。これでコメントが宣言する不変条件が満たされ、結果がマップの走査順に依存しなくなります。あわせて、手続き的 preset 同士が互いに override するケースをユニットテストで固定します。

### bug-27: raySphereRoots が長さの二乗次元を持つ判別式に無次元の絶対イプシロンを使っている

**重大度**: low

**場所**: [`src/core/phys/physquery.cpp`](../../src/core/phys/physquery.cpp):144-161 — `raySphereRoots`

**何が起きるか**

判別式は長さの二乗の次元を持つのに、比較に使われるのは無次元の絶対イプシロン(1e-5)です。そのため実効半径が膨張し、絶対誤差の上限は半径によらず約 3.16 mm(ワールド単位)になります。半径 0.5 なら 0.002% ですが、半径 0.001 では 232% です。overlap の太らせと違い、raycast では明確なミスがヒットに化けます。細いカプセルの端キャップも同じ関数を経由します。

**なぜバグと判断したか**

- 数式の次元と数値を検算し、ヒット条件と実効半径の膨張率が報告どおりであることを確認しました。偽ヒットを止める後続の分岐もありません。
- 呼び出し元が小半径を作らないという反証は失敗しました。検証は正値と有限性しか要求せず下限がなく、さらにワールドスケールが乗るためいくらでも小さくなります。スケール 0 を弾かないため、半径ちょうど 0 の球が正規パスで生成可能です。
- 既定ビルドはこの builtin プロバイダで、しかも RPC のプレビュー評価はプロバイダを経由せず直接この関数群を呼ぶため、Jolt ビルドでも通ります。
- テストも固定していません。接線ケースは判別式が厳密に 0 なので、どのイプシロンでも同じく通ります。半径 0.25 未満のコライダを検証するテストは存在しません。
- 設計文書に許容量の記述もありません。文書化されている 1e-5 は shapeCast の線形許容量で、raycast の許容量はどこにも定義されていません。ソースコードガイドは「二乗量には二乗のイプシロンを使う」という定型を挙げており、この関数はそこから外れています。
- 緩和要因として、この式の形は原点が遠いと桁落ちするため、現在の定数は実質的にその補償として働いている面があります。ただしそれは小半径を救わず、むしろ「この式のままではミリ級の球のヒット判定ができない」ことを意味します。

**修正の方向**

まず式を数値的に安定な形へ書き換えます。垂直成分から距離の二乗を直接求めて判別式を作り、桁落ちを消します。そのうえで判定を二乗次元の許容量、あるいは線形換算で `h <= r + ε` 相当に置き換えます。後者なら文書化済みの接触イプシロンと次元が一致します。イプシロンだけ縮めて式を直さないと、遠方原点での桁落ちが接線付近のちらつきとして表面化するため、必ず同時に行ってください。同じ不整合はカプセルの側面分岐と overlap 群にもあるので、規約を決めてファイル全体で揃えるのが望ましいです。

## 3. 判断保留

### bug-18: GameLogicReloader::pollAttempt が試行前に observed_write_time を進めるため、一時的な共有違反で DLL 変更が恒久的に握り潰される

**場所**: [`src/core/gamelogic/gamelogicreload.cpp`](../../src/core/gamelogic/gamelogicreload.cpp):353-370 — `GameLogicReloader::pollAttempt`

**機構は報告どおりで、反証できませんでした。** 観測済みタイムスタンプの書き込み点は 3 箇所だけで、失敗時に巻き戻す経路は存在しません。poll 経路は毎フレーム走り、この参加者はファイル監視からは絶対に駆動されないため、上流の再キューによる救済もありません。復帰路は F5 と手動 RPC、あるいはファイルのタイムスタンプ再変化だけです。poll 失敗後の挙動を守るテストも壊すテストもありません。

**しかし「バグ」と呼ぶ根拠は弱いと判断しました。**

- 設計意図が文書化されています。過去の設計レビューが「成功**または失敗**した強制リロードは観測済みタイムスタンプを承認し、次の poll が同じファイルを二度適用しないようにする」と明示的な仕様として記述しており、「失敗時も idle poll が last_error を保持する」という定常状態も想定されています。
- 「進めない」修正は明確な劣化です。主要な失敗クラス(不正な PE、ABI 不一致、スキーマ非互換など)は同じバイト列に対して決定的に失敗するため、進めなければ毎フレーム数 MB のシャドウコピーとロードを永久に繰り返すことになります。つまり必要なのは「一時的な失敗と恒久的な失敗の分類」であり、バグ修正ではなく機能追加です。
- 主張されたトリガの多くは自己修復します。比較は等値判定なので、書き手がクローズ時にタイムスタンプを更新すれば次の poll で再試行されます。統計情報の取得自体が失敗する場合は既に安全側の分岐になっています。
- 失敗は握り潰されていません。エラーログが出て、失敗カウンタと last_error に記録され、ステータス RPC からも見えます。

**反証しきれなかった残り**として、タイムスタンプが最終値になった後でコピーだけが失敗する窓は理屈上存在します。より現実的なのは状態依存の検証失敗で、DLL は変えずシーン側を直せば救えるのに、DLL のタイムスタンプが動かないため poll が二度と再試行しない、というギャップです。ここは「再試行すれば成功しうる失敗」を取りこぼす実在の穴です。

**総合**: コードは報告どおりに動きますが、これはエッジトリガ型ポーリングの標準的な実装であり、同じ方針が姉妹関数のコメントと設計レビュー文書で明示されています。堅牢性の余地はありますが、バグと断定できないため保留とします。仮に直すなら、失敗を再試行可能なものと恒久的なものに分類し、前者のときだけ観測済みタイムスタンプを復元する形が妥当です。

## 4. 否定・却下されたもの

再報告を防ぐため、理由込みで残します。

### 4.1 反証により否定(6 件)

- **open_preview が受理済み未実行の edit チケットとの重なりを検査せず、フレーム境界で preview が先に処理されるため先着の edit が飢餓する** — 機構の記述自体はコードと一致します(preview のリース検査、保留処理の順序、edit 側の重なり検査)が、反証が成立しました。
- **`~VariedArray` が rollbackTail 経由で破棄するため、生きた entity を抱えたまま chunk / core を破棄すると deinit が一度も呼ばれない** — 報告された事実関係(デストラクタが destroy のみ呼び deinit を呼ばない、core のデストラクタが clearEntities を呼ばない)は読解どおり正しいものの、これは意図的な設計判断であり、ドキュメントとテストで固定されていました。
- **blocked_commits がフェーズ失敗のたびに単調増加し、フレーム境界で刈り取られない** — push と削除の箇所、リビジョンの単調増加という「機構としての事実」は正しいものの、反証が成立しました。
- **cubic-spline の回転トラックが半球補正も正規化もされず、隣接キーが逆符号だとサンプル全体が失敗する** — 4 点で反証されました。特に、この実装の設計正本である WP177 報告が cubic_spline の非補正を明示しており、報告の中心的推論を直接否定します。
- **VRM の FrameState がフェーズ連鎖の中断時に孤児化し、frames マップが毎フレーム増え続ける** — 機構は事実ですが、「毎フレーム増え続ける」という前提条件がエンジン側で成立しません。
- **renderLogicalFrame がビューループ内で投げると XR の abortFrame を迂回し、取得済みスワップチェイン画像と開いたままのフレームが残る** — renderer / XR composition target / loop / セッションの各実装を読み合わせた結果、反証が成立しました。

### 4.2 選別段階で落としたもの(11 項目)

- **bitmapfont.cpp:109-111「グリフレイアウトの int64 オーバーフロー」** — 誤検出。カーソル座標は非空白グリフごとに INT32 範囲で検査され、範囲外なら throw します。空白のみで成長させても int64 を溢れさせるには約 5.7e17 文字が必要で到達不能です。
- **drawcommands.cpp:36「vector size の uint16 キャスト」** — 誤検出。最大 quad 数が先に検査されるため、基準値は最大 65532 で uint16 に収まります。候補自身も「現状は範囲内」と認めており、将来の変更に対する脆さの指摘です。
- **cmdbuf.cpp:21「Release で待機セマフォとステージのサイズ不一致を検査しない」** — 誤検出。実呼び出しはすべてコンパイル時固定の初期化子リストで、外部入力で破れる経路がありません。
- **rendertarget.cpp:76「PNG stride の int オーバーフロー」** — 誤検出。オーバーフローには幅 5.4 億以上が必要で、Vulkan の実機上限を 4 桁超えます。
- **render_pass_frame_setup.cpp:34 および renderer.cpp:336「input_target_history の `at()` にサイズ検証なし」(2 件)** — 誤検出。パス定義の検証がサイズ一致を明示的に検査して throw し、唯一のプログラム的構築箇所も両者を 1 要素で設定しています。サイズ不一致は構築不能です。
- **memorydiagnostics.cpp:18「要素数 × 要素サイズの uint64 オーバーフロー」** — 誤検出。合計 2^64 バイト超の論理要素数が必要で、実在するアドレス空間を超えます。
- **core.cpp:514「logical_frame の uint64→uint32 縮小キャスト」** — 誤検出に近く報告不能。2^32 フレームは 60fps で約 2.2 年の連続実行に相当し、具体的な破壊シナリオを再現条件付きで示せません。
- **inputstate.cpp:253-260「move constructor に acquire() が無い」** — 誤検出。move は所有権ごと奪って移動元を無効化するため、借用の総数は不変です。acquire を足すと逆に二重カウントになります。
- **inputstate.cpp:276-290「move assignment に acquire() が無い」** — 上と同じ理由で誤検出。
- **tools/pelican_rpc.py:137「error.get('code') が None を返しうる」** — 却下。JSON-RPC 仕様上 code は必須で、サーバは自リポジトリの実装です。影響も例外メッセージの表示だけで、数値演算は存在しません。防御的コーディングの提案であってバグではありません。
- **gltf.cpp:901 / 904 / 971 の accessor index 未検証(3 件)** — 誤検出ではありませんが bug-1 に統合しました。根本原因はいずれも `getDataFromAccessor` が index を検証しないことで、3 件は同じ欠陥の 3 つの呼び出し元です。

## 5. 別途起票済み

本報告書は調査結果の記録のみで、修正は次の 4 タスクとして別途起票済みです。

1. **スプライトの DrawRun `vertex_offset` 修正**(sprite-1 / high)— チャンク境界で DrawRun を分割し、前チャンクの `vertex_offset` を引き継がないようにする。🚧 作業ツリーで着手済み。
2. **`drawIndirectFirstInstance` の有効化または `firstInstance` の 0 化**(vk-1 / medium)— 仕様違反(VUID-vkCmdDrawIndexedIndirect-firstInstance-00530)の解消。モバイル / タイル GPU での実害の可能性を含む。🚧 作業ツリーで着手済み(`core.cpp` で有効化 + 能力検出を新設)。
3. **bug-9(critical)の修正** — `PreparedComponentSwap` を EntityId 保持へ寄せ、noexcept な publish 経路から throwing な `at()` を排除する。
4. **残り 24 件の仕分けと修正** — 本報告書 §2 の confirmed のうち bug-9 を除く 24 件について、優先度付けと修正方針の確定を行う。§3 の bug-18 は保留のまま、この仕分けの中で扱いを決める。**bug-20 はこのタスクに含まれますが、作業ツリーでは既に `kMaxSimplexVertices` による頂点数ガードが入っています**(🚧 着手済み)。

着手済みの 3 件については、修正が入った時点で §2 の該当節と、`docs/source-code-guide/` の関連する難所ブロック(GJK の頂点数、スプライトの 16384 quad 天井、間接描画の `firstInstance`)の整合を確認してください。
