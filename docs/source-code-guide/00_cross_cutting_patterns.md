# 横断パターン

[索引へ戻る](README.md) / [領域別の難所索引](09_black_magic_and_gotchas.md#923-仕様の難所索引領域別)

## 9.22 横断パターン(初読で 5 つ以上の領域に同じ形で現れるもの)

この節と次の §9.23 は、章の本文ではなく**逆引きの索引**です。第9章のここまでは「マクロ展開、型消去、静的初期化、寿命、現在の未実装点」を順に説明してきましたが、実際の読解で詰まるのは順番に読んでいるときではなく、**知らない場所を先に開いてしまったとき**です。そのときに「この形は何か」「どこに答えが書いてあるか」を引くための表がこの 2 節です。

説明の**本体は各章の `🧩 難所` ブロック**にあります。ここは答えを二重に書かず、既に難所ブロックがある機構は 1 文とリンクだけで送り出します。索引にしか答えが無いものだけ、その場で 2〜4 文書いています。

以下の 6 つは、初読で 5 つ以上の領域に**同じ形**で現れます。1 つ知ると複数の領域が同時に読めるので、領域別索引(§9.23)より先に読んでください。

### 三段トランザクション(prepare → publish → finish)

**形**: 失敗しうる作業(確保・検証・容量チェック・id 採番)を prepare に全部集め、publish を `noexcept` の swap 列にして「公開後は失敗しない」を型で固定し、finish は「もう巻き戻さない」と決まってから初めて破棄・`deinit` を行う。

**どこに出るか**: loader は [`SceneLoader::load()`](../../src/core/loader/scene.cpp#L1650) が prepare → early_publish → publish → activate → commit_metadata → retire → flush → finish の 8 段。ECS は [`ECSArchetypeMigrationAdapter`](../../src/core/ecs/archetypemigration.hpp#L45) のヘッダコメントが「Publication must not fail」を宣言し、**公開済みの実体**に `cb_deinit` を呼ぶのは [`finishPublished()`](../../src/core/ecs/archetypemigration.cpp#L352) とその逆手順の [`rollbackPublished()`](../../src/core/ecs/archetypemigration.cpp#L229) だけ(未公開の候補側は `discardPrepared()` / `deinitializeStaged()` / `rollbackCreate()` が担当する)。描画は [`stagePreparedSceneModel()`](../../src/core/renderer/polygoninstancecontainer.cpp#L747) が容量検査と確保、物理は [`PhysWorld::prepareBindings()`](../../src/core/phys/physworld.cpp#L287)、behavior は [`PreparedBehaviorAttachmentEdits::publish()`](../../src/core/gamelogic/behaviorarena.cpp#L472)、アセットは [`model.cpp` の validate/stage](../../src/core/asset/model.cpp#L685)。

**読むときの手順**: まず `publish` の宣言に `noexcept` が付いているかを見て、付いていれば「この関数より前に全ての失敗が済んでいる」と読み替えます。次に、その関数が触るメンバが *candidate* 側か *live* 側かを 1 つずつ確かめてください。

### `noexcept` なコミットテール

**形**: 「戻せないのに失敗しうる」状態を作らないため、コミット点以降に呼ぶ全関数が throw しないことを **20 個の `static_assert(noexcept(...))`** でコンパイル時に固定する。「確保もしない」の側は static_assert では固定できないので、報告構造を固定長にすることで担保する。

**どこに出るか**: [`scene.cpp` の static_assert 群](../../src/core/loader/scene.cpp#L1082) が publish/finish/retire/flush を固定し、全体は retire → flush → finish を含む 8 段です。不可逆点は [`metadata.commit()`](../../src/core/loader/scene.cpp#L635) の単一 metadata swap 1 点。公開用の文字列と JSON は [tail を抜けてから materialize](../../src/core/loader/scene.cpp#L1813) します。ECS 側の同じ宣言は [archetypemigration.hpp のヘッダコメント](../../src/core/ecs/archetypemigration.hpp#L44)、`noexcept` の中で確保しないための固定長報告は [`ReleaseReport`](../../src/core/model/modelresourcerelease.hpp#L36)、同じ理由の固定長診断が [`PreparedEffectTransaction::errors_`](../../src/core/loader/effecttransaction.hpp#L142)。

**読むときの手順**: `PreCommitFailure` が取りうる phase 値の集合を先に見てください。prepare / early_publish / activate の 3 つしかないこと自体が、「それ以降は失敗しない」の裏返しです(→ §9.21 の「`noexcept` の commit tail の中で文字列や JSON を作らない」)。

### rollback は公開の逆順(LIFO)、cleanup と discard は別物

**形**: 後に触ったものから先に戻す。既に live へ公開したものを逆トークン/逆 swap で旧世代へ戻すのが *cleanup*、一度も公開していない候補をスコープ終了で捨てるだけなのが *discard* で、後者は「何もしない」のが正しい rollback。

**どこに出るか**: [`scene.cpp` の rollback ラムダ](../../src/core/loader/scene.cpp#L1734) は [`rollback_order` の表](../../src/core/loader/scene.cpp#L1724) をそのまま回すだけで、順は behavior → physics → light → camera → VatPlayer → SeqPlayer → loader state → renderer → procedural → ECS 退役(destroyed の inverse token を `rbegin` から) → ECS(created を `rbegin` から) → event outbox → RNG → pending scene。ECS は [`rollbackPublished()`](../../src/core/ecs/archetypemigration.cpp#L229)、behavior arena は [`activate()`](../../src/core/gamelogic/behaviorarena.cpp#L620) の失敗時に `activated` を逆順に破棄、シェーダは [`discard_new_layouts` ラムダ](../../src/core/shader/pipelinefactory.cpp#L1948) が「今回追加した分だけ」を消し、journal は [子孫を declaration_index 降順に並べてから削除](../../src/core/communication/editorjournalops.cpp#L1134)。

**読むときの手順**: rollback の本体を読む前に「この参加者は公開済みか」を判定してください。event outbox / rng / pending scene のように**何もしないのが正しい**参加者を「巻き戻し漏れ」と誤読するのが最も多い間違いです。

### generation による stale ハンドル検出

**形**: index(= スロット)は再利用するが generation は再利用しない。index と generation を**対で持てば**、古いハンドルが新しい実体を指す ABA を必ず検出できます。持ち方は 1 語の 64bit に詰める形([`LogicalAssetId`](../../src/core/watch/reloadtransaction.hpp#L21))と、フィールドを分けて scene epoch まで足す形([`ModelInstanceId`](../../src/core/renderer/modelinstance.hpp#L28))の両方があります。index を伴わない単調カウンタ(`ray_generation` / `geometry_address_generation`)は形が違い、ハンドルの ABA ではなく「手元のスナップショットが古いか」の判定に使います。

**どこに出るか**: 論理アセットは [`makeLogicalAssetId()`](../../src/core/watch/reloadtransaction.cpp#L19) が `(generation << 32) | index`、entity は [`prepareCreate()` の `reuses_id`](../../src/core/ecs/archetypemigration.cpp#L1029)、描画インスタンスは [`stagePreparedSceneModel()` の `nextId`](../../src/core/renderer/polygoninstancecontainer.cpp#L747)、マテリアルは [`ray_generation` の採番](../../src/core/material/materialcontainer.cpp#L808)、ジオメトリのデバイスアドレスは [`geometry_address_generation`](../../src/core/model/vertbufcontainer.cpp#L326)。

**読むときの手順**: [第4章「generation 0 を跨がない台帳」](04_ecs_deep_dive.md)を先に読み、そのうえで「この世代は何が変わったら進むのか」を軸ごとに分けてください。scene epoch・slot generation・content revision・compatibility revision は**互いに独立**で、1 本だと思って読むと必ず外します(→ §9.2)。

### identity / canonical key / lease の三層

**形**: (a) 内容の同一性を決める canonical key、(b) それを固定長にした digest、(c) 実行時の生存を証明する handle / lease。片方だけでは「内容は同じだが実体が死んでいる」と「実体は生きているが内容が変わった」を区別できない。

**どこに出るか**: RT の行は [3 つの identity を同時に組み立て](../../src/core/renderer/accelerationstructure.cpp#L674)、マテリアル表は [`source_by_handle` と `source_by_key` の 2 名前空間](../../src/core/shader/raygrouplayout.cpp#L1845)を両方照合し、マテリアル側は [`ray_generation` と `ray_lease` の生成](../../src/core/material/materialcontainer.cpp#L808) の対で答えます。resolver は [`resolverInputsFingerprint()`](../../src/core/loader/resolvedscene.cpp#L757)、editor journal は [`attachment_handle` / `attachment_seq` / `attachment_identity`](../../src/core/communication/editorjournalops.cpp#L351) の 3 本立て。

**読むときの手順**: 「どちらが欠けたときにどのエラーコードになるか」を先に引いてください。三層のどれを検査しているかは、エラー名(`*_stale` / `*_ambiguous` / `*_mismatch`)がほぼそのまま表しています。

### fault injection seam と mutation id

**形**: 本番コードの分岐上に注入点を置き、「この分岐を壊すとどのテストが赤くなるか」を名指しできるようにする。manifest の `mutation_id` 列がその索引。

**どこに出るか**: scene load の入口は [`sceneLoadMutationEnabled()`](../../src/core/loader/sceneloadfault.cpp#L30) と [manifest の `root.import.versioned` 行](../../src/core/loader/sceneparticipantmanifest.cpp#L199)、保存の TOCTOU 窓は [`BeforeReplace` の注入位置](../../src/core/loader/basicconfig.cpp#L1017)、SBT は [production branch observer](../../src/core/shader/pipelinefactory.cpp#L1731)、TLAS は [`overrideRayTracingTlasRecordOffsetForTesting`](../../src/core/renderer/accelerationstructure.cpp#L818)、present は [本物の `presentKHR` を呼んでから戻り値だけ差し替える](../../src/core/vkcore/swapchainframetarget.cpp#L2606)。

**読むときの手順**: [第3章「注入口はproductionバイナリに存在しない」](03_project_and_loading.md)を先に読んでください。注入点は「テスト用の別経路」ではなく**本番経路そのもの**で、経路を差し替える方向に直すと A/B が破綻します。

