# 第3章 プロジェクトデータとロード

[索引へ戻る](README.md) / [前章](02_runtime_lifecycle.md)

## 3.1 純粋パーサとruntime bindingを分ける

ここでいう **runtime binding** は、検証済みのデータをエンジンの実体（解決済みpath、ECS Component、GPU資源、module参照）へ結び付ける段のことです。前段の純粋パーサ（`src/project`）はVulkanもquillもmodule containerも知らず、構文・schema・version・形式上の意味検証と正規化だけを行います（§3.3）。「同じ検証規則をエンジン非依存のまま devstudio や CLI からも使う」ためにこの線が引かれています（[用語集](../manual/14_glossary.md) の「結合(binding)」）。データロードの基本形はこの二段です。

```text
JSON/テキスト
  ↓ src/project: 構文・schema・version・意味検証、正規化
Definition / Document / Result
  ↓ src/core: path・ECS・GPU・moduleへ結び付け
Runtime object
```

例:

| データ | 純粋層 | runtime層 |
|---|---|---|
| project.json封筒 | [`parseProjectEnvelopeText()`](../../src/project/projectformat.cpp#L122) | [`ProjectBasicConfig::ProjectBasicConfig()`](../../src/core/loader/basicconfig.cpp#L633) |
| path参照 | [`ProjectPathResolver`](../../src/project/projectpathresolver.hpp#L69) | [`PathResolver`](../../src/core/loader/pathresolver.hpp#L9)（module寿命、ログ、engine resource注入） |
| scene | [`normalizeSceneDataJson()`](../../src/project/sceneformat.cpp#L203) | [`SceneLoader::load()`](../../src/core/loader/scene.cpp#L522) |
| render feature | [`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L2695) | [`registerRenderGraphVariantFamilyFromJsonData()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1141) |
| JSON-RPC | [`parseJsonRpcRequest()`](../../src/project/jsonrpc.cpp#L148) | [`RpcServer`](../../src/core/communication/rpcserver.hpp#L53) |
| asset manifest | [`parse/generate/verify`](../../src/project/assetsmanifest.hpp#L60) | [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L11) |
| material/surface | [`parseMaterialFormatJson()`](../../src/project/materialformat.cpp#L744)、[`parseSurfaceFormat()`](../../src/project/surfaceformat.cpp#L1239) | runtime接続済み（WP116〜117, 122）。`.surface`は [`surfacecompiler`](../../src/core/shader/surfacecompiler.cpp) でGLSL/SPIR-V化されpipelineへ。`.material.json`は [`lowerMaterial()`](../../src/project/materiallowering.hpp#L140) → [`registerReloadableMaterialValuesFile()`](../../src/core/material/materialcontainer.hpp#L345) |

新形式を追加するときは、まず`src/project`へGPU非依存の受理/拒否規則を置き、次に`src/core`で実体化するのが既存パターンです。

## 3.2 ProjectSourceと設定の三段優先順位

[`ProjectSource`](../../src/core/loader/projectsrc.hpp#L7) は二種類の入力を保持します。

- `raw_data`: `PelicanCore(settings)`へ直接渡した上書きJSON
- `project_data`: playerが読んだ`project.json`全体

`project.json` のschema/version/`engine_min_version`ゲートは、まず [`parseProjectEnvelopeText()`](../../src/project/projectformat.cpp#L122) が検証します。`--ignore-engine-version`で継続する場合も純粋層はログを出さずwarning文字列を返し、engine adapterだけがそれをquillへ渡します。検証規則の正本はこの関数だけで、`basicconfig.cpp`やdevcliに写しはありません。

[`ProjectBasicConfig::ProjectBasicConfig()`](../../src/core/loader/basicconfig.cpp#L633) は検証済み封筒の`basic_config`を含む次の三sourceを [`JsonLoader`](../../src/core/loader/basicconfig.cpp#L61) へ渡します。

1. `raw_data` — API/起動時の上書き
2. `project.json` の `basic_config`
3. `engine://default_config.json`

各fieldはこの順で最初に見つかった値が採用されます（[`JsonLoader::getVal()`](../../src/core/loader/basicconfig.cpp#L69)）。JSON object全体をdeep mergeするのではなく、必要fieldをpath単位で問い合わせる方式です。

構築時にwindow、framerate、seed、camera、各データファイル参照を値として取り込みます。その後、assets/rendering/UI/input JSON本文はgetterの初回呼び出し時に読み、`mutable optional<string>`へcacheします（[basicconfig.hpp](../../src/core/loader/basicconfig.hpp#L123)）。

### sceneだけはcacheではなくdocument

sceneは単なる文字列cacheから、**[`SceneProjectionState`](../../src/core/loader/resolvedscene.hpp#L308) が所有する `AuthoringSceneDocument` と `ResolvedScene` の不変pair**へ格上げされました（WP149 / WP166 / WP360）。

格上げされたのがsceneだけなのは、編集の有無で必要な機能が違うからです。assets/rendering/UI/inputのJSONは読み取り専用で、cacheの役目は「二度目以降のファイル読みを省く」ことだけです。一方sceneはセッション中に書き換えられ、(a) ディスクへの書き戻し（本節の `saveSceneDocument()`）、(b) 改訂番号による競合検出（§3.12 のCAS）、(c) renameや並べ替えに耐えるobject単位の安定identity（§3.12 の `AuthoringObjectId`）を必要とします。`std::optional<std::string>` にはこの三つを置く場所がありません。

保持しているのは、scene に関する metadata を 1 つにまとめた [`SceneMetadataState`](../../src/core/loader/basicconfig.hpp#L104) です（WP380a）。

```cpp
struct SceneMetadataState {
    std::optional<SceneProjectionState> projection;
    std::optional<ScenePersistenceBaseline> persistence_baseline;
    BindableProviderSnapshot prefab_provider;
    std::uint64_t next_prefab_provider_generation = 1;
    std::uint64_t next_scene_revision = 1;
    std::uint64_t next_scene_resolver_generation = 1;
    std::uint64_t next_authoring_object_id = 1;
    SceneId current_scene_id;
    std::uint64_t epoch = 0;
    SceneAuthority active_authority{};

    void swap(SceneMetadataState &other) noexcept;
};

mutable SceneMetadataState scene_metadata;
```

1 struct にまとまっているのは、scene load の commit がこの struct の **1 回の `noexcept` swap** だからです（[`SceneMetadataState::swap()`](../../src/core/loader/basicconfig.cpp#L518) / [`MetadataCommitToken::commit()`](../../src/core/loader/basicconfig.cpp#L586)）。document だけ、projection だけ、current scene だけを個別に公開する経路は production には無く（[`MetadataCommitToken`](../../src/core/loader/sceneload.hpp#L196) の `Impl` は `ProjectBasicConfig` の private）、candidate は [`prepareBorrowedSceneMetadata()`](../../src/core/loader/basicconfig.cpp#L597)（committed projection をそのまま使う root）か [`prepareImportedSceneMetadata()`](../../src/core/loader/basicconfig.cpp#L610)（snapshot import）が token として作り、§3.5 の scene executor が commit します。失敗した token は捨てられるだけで、live の `scene_metadata` には触れません。「1 段の swap である」ことは台帳側の executor 仕様がそのまま定めていて、WP380a の完了記録は phase 列を `prepare → early_publish → noexcept candidate publish → activate → metadata commit(1 struct の noexcept swap)→ retire(次フレーム queue)→ outbox flush` と書いています（[`docs/implementation_plan.md`](../implementation_plan.md) の WP380a 完了記録）。コード側も同じことを言っており、分割公開は fault injector 専用の反実仮想として `Production can publish only SceneMetadataState as a whole through commit().` とコメントされています（[分割公開の反実仮想に付いたコメント](../../src/core/loader/basicconfig.cpp#L579)）。代入ではなく swap なので、commit 後は旧 state が token 側へ移り、旧 projection の破棄は commit 点の中ではなく token の寿命末で起きます（[`swap()` の本体](../../src/core/loader/basicconfig.cpp#L518) は member ごとの `std::swap` だけです）。

そのため [`sceneDataJson()`](../../src/core/loader/basicconfig.cpp#L1012) は**生ファイル文字列を返しません**。

```cpp
std::string ProjectBasicConfig::sceneDataJson() const {
    return AuthoringSceneAuthority::encodeSemantic(sceneDocument());
}
```

返るのはauthoring authorityによるsemantic encodeです。scene を **resolve する**とは、作者が書いた authoring document に prefab の展開と省略値の補完を施し、読み手が場合分けなしに読める不変の形（[`ResolvedScene`](../../src/core/loader/resolvedscene.hpp#L250)）へ畳むことです — 「scene の読み手に影響する authored な省略は、この不変 projection を prepare する時点で全て埋める」がヘッダのコメントの言い分です（[`ResolvedSceneDefaults` の上のコメント](../../src/core/loader/resolvedscene.hpp#L54)）。各componentが authored か生成物かは [`ResolvedComponentOrigin`](../../src/core/loader/resolvedscene.hpp#L178) に残ります。なお [用語集](../manual/14_glossary.md) の「解決(resolve)」は**path参照**をファイルの絶対パスへ確定させる別の用法で、§3.3 の話です。同じ動詞が2つの層で使われています。runtimeの読者はraw JSONではなく [`resolvedScene()`](../../src/core/loader/basicconfig.cpp#L774) を読み、両者とrevision / resolver generationは同じ `SceneProjectionState` から公開されます。読み書きのAPIは次の通りです。

| API | 宣言 | 役割 |
|---|---|---|
| `sceneProjectionState()` | [`sceneProjectionState()`](../../src/core/loader/basicconfig.cpp#L778) | authoring / resolved / revision / generation のpairを取得（初回は遅延load） |
| `sceneDocument()` / `resolvedScene()` | [`sceneDocument()`](../../src/core/loader/basicconfig.cpp#L770) / [`resolvedScene()`](../../src/core/loader/basicconfig.cpp#L774) | 同じpairのauthoring側 / resolved側を取得 |
| `updateSceneDocument()` | [`updateSceneDocument()`](../../src/core/loader/basicconfig.cpp#L789) | scene v1バイト列をprepareしてpairで差し替え |
| `invalidateSceneDocument()` | [`invalidateSceneDocument()`](../../src/core/loader/basicconfig.cpp#L793) | pairを破棄し、次回の再構築を強制 |
| `importSceneDocument()` | [`importSceneDocument()`](../../src/core/loader/basicconfig.cpp#L798) | 外部由来のバイト列を取り込み、新しい `SceneRevision` を返す |
| `saveSceneDocument()` | [`saveSceneDocument()`](../../src/core/loader/basicconfig.cpp#L912) | ディスクへ書き戻し、pairを公開し、`SceneSaveResult`（revision / digest / byte数）を返す |

保存の失敗は型付きです。[`SceneSaveErrorCode`](../../src/core/loader/basicconfig.hpp#L24) は `ExternalModification` / `Unavailable` / `IoFailure` の三つで、[`SceneSaveError`](../../src/core/loader/basicconfig.hpp#L30) が保持します。`scene_metadata.persistence_baseline` の `scene_digest_sha256` と保存直前のディスク内容を突き合わせるため、**エディタの外でファイルが書き換わっていれば `ExternalModification` で拒否**します。

テスト用のフォールト注入点も宣言的に列挙されています。[`SceneSaveFaultPoint`](../../src/core/loader/basicconfig.hpp#L39) は `AfterEncode` / `AfterDiskDigest` / `AfterTemporaryWrite` / `AfterTemporaryValidation` / `AfterCachePrepare` / `BeforeReplace`、[`SceneImportFaultPoint`](../../src/core/loader/basicconfig.hpp#L48) は `AfterCandidatePrepare` / `AfterPublication` です。

> **設計決定:** 保存経路の中断点を実装内部の分岐ではなくenumとして公開することで、「どの中断点でもディスク上のsceneが壊れない」という性質をテストから網羅的に叩けます。列挙名がそのまま保存手順の段階名になっています。

> 🧩 **難所 — 保存のTOCTOU窓**([`saveSceneDocument()`](../../src/core/loader/basicconfig.cpp#L912))
>
> **何をする所か**: 編集済み文書を一時ファイル経由でディスクへ書き戻し、原子的に置換してから、メモリ側の文書とbaseline digestを差し替えます。TOCTOU と「一時ファイル + rename による原子的置換」の一般的な背景は [第10章 §10.4](10_background_knowledge.md) にあります。
>
> **素朴に読むと**: digest比較が **2回** あるのが冗長に見えます。しかしdigest比較が言えるのは「**比較したその瞬間まで**外部変更が無かった」ことだけで、比較を過ぎてから外部が書いた分については何も保証しません。1回目([`disk_digest` の検査](../../src/core/loader/basicconfig.cpp#L943))は一時ファイルを書く**前**にあるので、これだけにすると一時ファイルの書き込み・読み戻し・意味検証にかかる時間がまるごと TOCTOU 窓(time-of-check to time-of-use の略 — 検査した時点と実際に使う時点がずれるせいで生まれる、その間に外部が書き換えられる隙間)になり、保存処理はその間に外部エディタが書いた内容を、気付かないまま上書きしてしまいます。2回目([置換直前の `stableDiskDigest` 検査](../../src/core/loader/basicconfig.cpp#L994))は置換の直前に置かれていて、窓を実務上無視できる幅まで縮めるためのものです(WP362 からは比較相手が `save_baseline.scene_digest_sha256` —— scene digest と prefab read-set を一体で凍結した不可分 baseline の一部 —— になりました)。もう一つ見落としやすいのが末尾の順序で、ファイル置換([`replaceSceneFileAtomically()` の呼び出し](../../src/core/loader/basicconfig.cpp#L1002))の **後** に [`publishPreparedSceneState(next_state)`](../../src/core/loader/basicconfig.cpp#L758) と [`*scene_metadata.persistence_baseline = std::move(next_baseline)`](../../src/core/loader/basicconfig.cpp#L1008) が来ます(置換の後にあるのは3回目の比較ではありません。baseline を保存したバイト列の digest と prefab read-set へ**更新**する操作です)。この区間を確保・decode・I/Oなしの無throwにしてあり、逆順にすると「置換に失敗したのにメモリ側だけ新しいrevision」が作れてしまいます。pair 公開が `noexcept` の [`publishPreparedSceneState()`](../../src/core/loader/basicconfig.cpp#L758)(中身は `SceneProjectionState` の `swap` と `next_*` カウンタの代入)で、baseline の更新が `ScenePersistenceBaseline`(`std::string` + `std::vector`)丸ごとの **move 代入**なのも同じ理由で、どちらもバッファを移すだけで新しい確保を伴いません(WP380a 前は `scene_baseline_digest->swap()` でした。§3.2 の `SceneMetadataState` に統合された今は optional の中身への move 代入です)。
>
> **骨子**:
> ```text
> semantic_bytes = AuthoringSceneAuthority::encodeSemantic(source) # SAVE0唯一の直列化
> disk_digest != baseline(1回目)   -> ExternalModification   # tmpを書く前
> tmpへwrite → 読み戻し一致 → load し直して authority raw view 一致
> next_document = AuthoringSceneAuthority::stage(...)
> next_state = ResolvedSceneResolver::prepare(next_document, generation, defaults) # 確保はここまで
> disk_digest != baseline(2回目)   -> ExternalModification   # 置換の直前
> replaceSceneFileAtomically(tmp, destination)
> publishPreparedSceneState(next_state); *persistence_baseline = move(next_baseline) # 無throw / 比較ではなくbaseline更新
> ```
>
> **手がかり**: 上の `SceneSaveFaultPoint` 6値がそのまま手順の段名で、2回目のdigest検査は `AfterCachePrepare` と `BeforeReplace` の**間**にあります。[`stableDiskDigest()`](../../src/core/loader/basicconfig.cpp#L354) が `watch::readStableContentDigest()` を使うのは、「書き込み途中のファイルを読んだ」状態(`retry`)を成功と混同しないためです。一時ファイルは `TemporarySceneFile` のデストラクタが必ず消すので、どの中断点でthrowしてもゴミが残りません。[`importSceneDocument()`](../../src/core/loader/basicconfig.cpp#L798) は逆に「失敗しうる間は live に触らない」側の作りで、candidate を `MetadataCommitToken` に包んで §3.5 の scene executor へ渡し、commit された場合だけ同じ `SceneMetadataState::swap()` で公開されます（reload が失敗すれば token が捨てられるだけで、戻す作業そのものがありません）。対にして読むと早いです。テストは [`sceneformat_test.cpp` 内](../../test/sceneformat_test.cpp#L428)「SAVE0 is failure-atomic at every prepare point」。
>
> **不変条件**: 直列化は1回だけ(以降の全段が同じバイト列を消費する)。ファイル置換より後にthrowしうる処理を置かない。baseline digestの更新はファイル置換と同一の無throw区間で行う。

## 3.3 PathResolver

解決規則の宣言は [`projectpathresolver.hpp`](../../src/project/projectpathresolver.hpp#L69)、中心実装は [`ProjectPathResolver::resolveRef()`](../../src/project/projectpathresolver.cpp#L551) です。ここは`pelican_project`に属し、Vulkan・quill・module containerへ依存しません。

engine側の [`PathResolver`](../../src/core/loader/pathresolver.hpp#L9) は薄いmodule adapterです。[`pathresolver.cpp`](../../src/core/loader/pathresolver.cpp#L1) に残るのは、module寿命、返されたwarningのログ、`engine://` IDを埋め込みbytesへ変えるloader注入だけです。この境界によりdevstudioやCLIは`pelican_core`をリンクせず同じescape防止・asset store規則を使えます。

### 返り値がpathだけではない理由

`ResolvedRef`は次のvariantです。

```cpp
std::variant<
    std::filesystem::path,
    EngineResourceId,
    ResolvedPathFragment,
    ResolvedEngineFragment
>
```

`engine://`はファイルシステム上のpathではなく、実行ファイルへ埋め込まれたresource IDです。また**fragment**を、コンテナ内要素のアドレスとして失わず伝えるためvariantになっています（[`parsePathReference()`](../../src/project/projectpathresolver.cpp#L278)）。fragmentは参照文字列の `#` 以降の `kind/path` で、「そのファイルの中のどれを指すか」を表します — 例が `assets/character.glb#mesh/Cube`（[`pathresolver_test.cpp` 内](../../test/pathresolver_test.cpp#L486)）で、`kind` が要素の種類、後半が名前です。`/` を含まない後半は名前アドレス、含む場合はコンテナ内のフルパスアドレスになります（[`AssetFragmentAddressKind`](../../src/project/projectpathresolver.hpp#L20)。[用語集](../manual/14_glossary.md) の「フラグメント参照(`#`)」）。`#` が2つある、`kind` だけで後半が無い、後半が全部数字、といった形は受理されません。

### scheme別の処理

| 参照 | 解決先 | 保護 |
|---|---|---|
| `engine://id` | `EngineResourceId` | 登録IDだけを後段で許可 |
| `project://x` または相対`x` | project rootまたはasset store | root外escape拒否 |
| `user://x` | project名由来のuser root | user root外escape拒否 |
| CLI由来の絶対path | そのpath | `--allow-absolute-paths`必須 |
| project JSON内の絶対path | 不許可 | 常に拒否 |

`std::filesystem::weakly_canonical`（`.` / `..` を畳み、途中までのsymlinkを解決して正規形へ落とす標準関数。末尾が実在しなくても失敗しない点が `canonical` との違いです）後にroot包含判定を行うため、単純な`../`文字列検査より強い境界です（[`ProjectPathResolver::resolveRef()`のproject処理](../../src/project/projectpathresolver.cpp#L656)）。

### asset store

[`ProjectPathResolver::setup()`](../../src/project/projectpathresolver.cpp#L343) は`project.json.asset_stores`を読みます。

- 各storeは論理`mount`と実rootを持つ。
- mount同士の重なりを拒否。
- `.pelican/local.json`で実rootだけをローカル上書き可能。
- 上書きはprojectで宣言済みのstore名だけ。
- 実root同士の包含/重なりも拒否。
- optional manifestのpathはproject root内に制限。

相対参照がmount prefixに一致すると、project rootではなくstore rootへ付け替えます（[`asset store解決`](../../src/project/projectpathresolver.cpp#L640)）。これにより、VCS上の論理pathは固定したまま、大容量assetの実配置を開発者ごとに変えられます。

### engine resource

shader、feature JSON、default config、debug fontなどは [`resources/CMakeLists.txt`](../../src/core/resources/CMakeLists.txt#L1) で`battery-embed`へ登録され、[`engineResource()`](../../src/core/loader/engineresources.cpp#L159) がIDを実データへ変換します。

resource追加には次の三箇所が必要です。

1. `b_embed(...)`または`embed_shader(...)`
2. `registered_ids`
3. `engineResource()`のdispatch

一つでも漏れるとビルドまたはruntime lookupが失敗します。これは[第9章](09_black_magic_and_gotchas.md)でも注意点として扱います。

## 3.4 asset manifestの起動検証

playerはcore起動前に [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L11) を呼びます。純粋な実作業は [`verifyAssetsManifest()`](../../src/project/assetsmanifest.cpp#L532) です。

- file size、SHA-256、missing/extra/case mismatch/unreadableを分類。
- `.pelican/assets-hash-cache.json`で再hashを減らす。
- `--strict-assets`なら通常warningの差異をerrorへ昇格。
- errorがあれば`PelicanCore::run()`へ入らず終了。

manifestの生成も同じ`src/project/assetsmanifest.cpp`にあり、`pelican_cli assets manifest`から利用されます。

## 3.5 Sceneのロード

sceneは実装を追う価値の高い、純粋層とruntime層の典型です。

### 1. envelopeと構文を正規化

[`normalizeSceneDataJson()`](../../src/project/sceneformat.cpp#L203) が次を検証します。

- `schema == "pelican.scene"`
- `version == 1`
- `scenes`がobject
- scene ID、object名、component名が`[a-zA-Z0-9_]`
- object名がscene内で一意
- 各objectに`components` arrayが存在
- `parent`（親子transform）が識別子であること。未知parent・曖昧parent・循環はエラー（[sceneformat.cpp](../../src/project/sceneformat.cpp#L105)、fixture: `scene_unknown_parent` / `scene_parent_cycle` / `scene_ambiguous_parent`）
- v1で禁止された旧`lights` fieldを拒否

この時点ではECS型やGPUには触れません。

> 🧩 **難所 — 3値のDFSが名前を出す**([`normalizeScene()`](../../src/project/sceneformat.cpp#L70) 内の visit ラムダ [本体](../../src/project/sceneformat.cpp#L168))
>
> **何をする所か**: `parent` エッジをたどって親子関係の循環を検出し、循環に参加しているobject名を並べたメッセージでthrowします。
>
> **素朴に読むと**: `auto &&self` を引数に取って `self(self, i)` で再帰する書き方が回りくどく見えます。generic lambda(引数型が `auto` のラムダ)は本体を書いている時点で自分の型が確定していないので、キャプチャ経由で自分を呼べません。`std::function<void(size_t)>` にすれば書けますが、型消去とヒープ確保が入り、純粋パーサ層にわざわざ間接呼び出しを増やすことになります。自分を第1引数で受け取ればキャプチャは参照のままで、インライン化も効きます(これを不要にする C++23 の deducing this は使えません — `pelican_project` は C++20 ビルドです、[src/project/CMakeLists.txt](../../src/project/CMakeLists.txt#L21))。もっと本質的なのは `state` が `0/1/2` の3値である点です。「訪問済み」を1値でしか持たないと、複数の子が同じ親を指す形(親リンクは森なので普通に起きます)で2度目の到達を循環と誤報するか、区別を諦めて毎回スタックを線形探索する羽目になります。1=いま辿っているスタック上、2=走査済みで安全、を分けて初めて `state[parent] == 1` だけが循環になります。さらに `stack` を別に持つのは「循環がある」ではなく `NodeA -> NodeB -> NodeC -> NodeA` と**実際の名前の並び**をエラーに載せるためで、scene作者に対する診断の質がこの構造の目的です。エッジの向きが子→親である点も、`std::find` で循環の始点を拾える前提になっています。
>
> **骨子**:
> ```text
> state[i] = 1; stack.push(i)
> parent あり かつ state[parent] == 0 -> self(self, parent)
> parent あり かつ state[parent] == 1 -> stack の parent 位置から末尾までを " -> " で連結して throw
> stack.pop(); state[i] = 2
> ```
>
> **手がかり**: 「未知parent」と「曖昧parent」はDFSより**前**の別ループで弾いてあるので([前段の検査](../../src/project/sceneformat.cpp#L134))、DFS中の `object_index_by_name.at(parent_name)` は必ず成功します。名前の無いobjectは `object_index_by_name` に載らないため親になれません。テストは [`sceneformat_test.cpp` 内](../../test/sceneformat_test.cpp#L114) で、`NodeA` / `NodeB` / `NodeC` の3名がメッセージに出ることまで検査します。
>
> **不変条件**: 3値stateの `1` と `2` を統合しない。エラーには循環に参加する名前の並びを載せる(「循環あり」だけにしない)。重複名・未知親・曖昧親の検査はDFSより前に済ませる。

### 2. runtime用にcomponentを分類

[`prepareSceneBindings()`](../../src/core/loader/scene.cpp#L279) はcomponentを四系統へ分けます。

- `light`: ECSへ入れず`LightLoadEntry`へ
- `collider`: `ColliderComponent`として検証し、後で`PhysWorld`へ。`PELICAN_WITH_PHYSICS` OFFのビルドでは、colliderを含むsceneは明示エラーです（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L127)）
- `behavior`: ECSへ入れず、[`BehaviorAttachmentArena`](../../src/core/gamelogic/behaviorarena.hpp#L174) へ（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L356) で特別扱い）
- その他: `ComponentInfoManager`で文字列名から`ComponentId`へ

このためsceneの見た目はcomponent配列でも、現在のruntime実装ではlight/collider/behaviorがECS Chunkに保存されるわけではありません。colliderをECSへ入れない理由は [`docs/design_ecs_lifecycle.md` §4](../design_ecs_lifecycle.md) の決定です — 「PhysWorld は『書き換え → 即 raycast で新位置』という既存契約を維持する」ため、PhysWorld 側が `Binding{name, collider, transform_source}` を持ち続け、query のたびに transform だけを ECS から resolve する形にしてあります（名前の重複検査とraycast結果の公開名がこの形に依存している、と同節が明記）。実装側の形は [§5.7 の World binding 層](05_gameplay_and_services.md) にあります。behavior が component でない理由も同種で、[`docs/design_object_behaviors.md` §0](../design_object_behaviors.md) が「behavior の正本 = engine-owned attachment arena。ECS component ではない。light/collider と同じ special dispatch の仲間」と決めています。

#### behaviorコンポーネント ✅実装済み

behaviorはobject単位に束ねてから渡します。これは §3.5 の executor の **prepare 段**で、resolved projection から読みます（[prepare 段の呼び出し](../../src/core/loader/scene.cpp#L633)）。

```cpp
const auto behavior_availability =
    game_logic_status.loaded
        ? BehaviorRegistryAvailability::active
        : BehaviorRegistryAvailability::dll_unavailable;
auto behavior_entries = prepareResolvedSceneBehaviorAttachments(
    scene->objects, behavior_availability);
ecs_objects = prepareSceneBindings(
    *scene, component_info_manager, light_entries,
    std::move(behavior_entries));
```

受理規則は [`prepareResolvedSceneBehaviorAttachments()`](../../src/core/gamelogic/behaviorarena.cpp#L159) にあります（raw JSON を受ける [`prepareSceneBehaviorAttachments()`](../../src/core/gamelogic/behaviorarena.cpp#L82) も同じ規則です）。

| 状況 | 結果 |
|---|---|
| `"type"` が無い / 文字列でない / 空文字列 | エラー `behavior on object '<name>' requires a non-empty string type` |
| DLLロード済みで、未登録の型名 | エラー `Unknown behavior type '<type>' on object '<name>'` |
| DLL不在で、未登録の型名 | warningを出して **pending** 扱いで続行 |

ここでの **pending** は「宣言だけ arena に載っていて、`Behavior` の実体がまだ作られていない attachment」です（[`PreparedSceneBehaviorAttachment::pending`](../../src/core/gamelogic/behaviorarena.hpp#L57)）。activate 段は pending を飛ばし（[`activatePublished()` の先頭の分岐](../../src/core/gamelogic/behaviorarena.cpp#L1017)）、毎フレームの update も `active` なものだけを回す（[`BehaviorAttachmentArena::update()`](../../src/core/gamelogic/behaviorarena.cpp#L1108)）ので、`onInit` / `onUpdate` はどちらも走りません。型名と生JSONは保持されるので、次に scene load か game-logic reload の reconciliation を通ったとき、その時点の registry で `pending` が付け直されます（[reconciliation の再判定](../../src/core/gamelogic/behaviorarena.cpp#L854)）。

behavior は executor の participant の 1 つです。entity を作った後に `BehaviorAttachmentArena::prepareSceneReconciliation()` で candidate を作り（[early_publish 段末尾の呼び出し](../../src/core/loader/scene.cpp#L981)）、publish 段で `noexcept` に arena へ swap し（[`behavior_candidate->publish()`](../../src/core/loader/scene.cpp#L1062)）、**activate 段**で `onInit` を走らせます（[`behavior_candidate->activate()`](../../src/core/loader/scene.cpp#L1076)）。activate は executor で**唯一の失敗しうる段**で、`onInit` が投げると executor の [`rollback` ラムダ](../../src/core/loader/scene.cpp#L731) が **全 participant** を旧世代へ戻し、結果は `PreCommitFailure{phase = activate}` になります（[activate 失敗の戻り値](../../src/core/loader/scene.cpp#L1081)）。document / projection / current scene / authority は commit 前なので旧世代のまま、arena も旧 attachment が戻ります。旧経路の [`clearRuntimeScene()`](../../src/core/loader/scene.cpp#L1278) は定義が残っていますが `load()` からは呼ばれません。

> **設計決定:** game logic DLLが無い状態でsceneを開くのは、ツールや検証では正常なケースです。そこで「DLLが有るのに型名が引けない」ときだけfailにし、DLL不在は保留にしています。ロード可否がビルド構成に依存してぶれない、という線引きです。

実物のscene JSONは次の形です（[`test/run_physics_trigger_behavior.ps1`](../../test/run_physics_trigger_behavior.ps1) が書き出すfixtureからの引用）。

```json
{"schema":"pelican.scene","version":1,"scenes":{"default_scene":{"objects":[
  {"name":"TriggerZone","components":[
    {"name":"transform","pos":[0,0,0]},
    {"name":"collider","shape":"sphere","radius":1.0,"trigger":true},
    {"name":"behavior","type":"wp179_trigger_behavior"}]},
  {"name":"Target","components":[
    {"name":"transform","pos":[1,0,0]},
    {"name":"collider","shape":"sphere","radius":1.0}]}]}}}
```

### 3. scene load は 1 つの transaction（WP380a）

[`SceneLoader::load()`](../../src/core/loader/scene.cpp#L522) は「旧 scene を消してから新 scene を作る」形ではありません。[`SceneLoadRequest`](../../src/core/loader/sceneload.hpp#L223)（committed projection を借りる `Borrowed`、または import の candidate と `MetadataCommitToken` を持つ `Owned`）を受け取り、[`SceneLoadResult`](../../src/core/loader/sceneload.hpp#L189)（`SceneLoadCommitted` / `PreCommitFailure` / `CommittedWithEffectFailure`）を返す **executor** で、段は次の順です。

```text
prepare（失敗可）
→ early_publish（失敗可。ECS は WP375 の inverse token で先に publish）
→ publish（noexcept。prepare 済み storage の swap だけ）
→ activate（失敗可。behavior の onInit。失敗しうる最後の段）
→ commit_metadata（noexcept。SceneMetadataState の 1 回の swap）
→ retire（noexcept。旧 state を次フレームの retirement queue へ積む）
→ flush（noexcept。prepare で確保した outbox slot から SceneLoaded を push）
→ finish（noexcept。early_publish で publish 済みの ECS create token を確定し、rollback 権を手放す）
```

段を記録するのは原則 executor ではなく participant 自身で、`SceneLoadPhaseTrace` へ書くのは metadata token（[`prepareRuntimeIdentity()`](../../src/core/loader/basicconfig.cpp#L561) が prepare、[`commit()`](../../src/core/loader/basicconfig.cpp#L592) が commit_metadata）、renderer（[`publishPreparedScene()`](../../src/core/renderer/polygoninstancecontainer.cpp#L531) が early_publish、[`enqueueSceneRetirement()`](../../src/core/renderer/polygoninstancecontainer.cpp#L690) が retire）、SeqPlayer / VatPlayer / Camera / Light / Physics の publish（いずれも trace を取る scene 専用 overload。例: [`Camera::publishPrepared()`](../../src/core/renderer/camera.cpp#L388)）、behavior arena（[`publish()`](../../src/core/gamelogic/behaviorarena.cpp#L633) / [`activate()`](../../src/core/gamelogic/behaviorarena.cpp#L685) / [`finish()`](../../src/core/gamelogic/behaviorarena.cpp#L730) が publish / activate / retire）、retirement queue（[`RetirementState::enqueue()`](../../src/core/loader/scene.cpp#L145)）、RNG（[`publishPreparedSequence()`](../../src/core/userpublic/deterministicrng.cpp#L126) が flush）、event outbox（[`PreparedEventOutbox::flush()`](../../src/core/userpublic/details/event/registerer.cpp#L279)）です。executor 自身が書くのは 2 か所だけで、pending swap 直前の [`record(SceneLoadPhase::flush)` の 1 行](../../src/core/loader/scene.cpp#L1136) と、created-entity batch を 1 participant と見なす [`record(SceneLoadPhase::finish)` の 1 行](../../src/core/loader/scene.cpp#L1148) です（WP380d。token 個別の `finish()` は記録しないので、`objects: []` の空 scene でも finish が列に出ます）。結果は [`SceneLoadCommitted::phase_trace`](../../src/core/loader/sceneload.hpp#L179) として返り、[`record()`](../../src/core/loader/sceneload.hpp#L47) は直前と同じ段なら足さないので、同じ段を複数の participant が通っても列は 1 つにまとまります。枠は 8 段ぶんの固定確保で、9 件目以降は列に入らない代わりに [`overflow`](../../src/core/loader/sceneload.hpp#L45) が立ちます（黙って落としません）。8 という数は [`SceneLoadPhase`](../../src/core/loader/sceneload.hpp#L27) の値数そのもの（[`sceneLoadPhaseCount = 8`](../../src/core/loader/sceneload.hpp#L38)）で、`record()` が直前と同じ段を足さない以上、段が上の順に1回ずつ現れる限り必ず収まります。つまり `overflow` が立つのは「一度離れた段へ戻った」ときだけで、確保を増やすべき状況ではなく**順序が壊れた**という信号です。`SceneLoadPhaseTrace` のコメントもそう書いています — `A trace is a fixed pre-commit allocation. Never silently discard an ordering observation: callers must reject an overflowing trace.`

**prepare**（[prepare 段の先頭](../../src/core/loader/scene.cpp#L578) から [`return fail(SceneLoadPhase::prepare, ...)`](../../src/core/loader/scene.cpp#L716) まで）では、candidate authority を 1 回だけ消費し（[`next_authority_epoch++`](../../src/core/loader/scene.cpp#L538)。失敗しても再利用しません）、metadata token に scene id と authority を書き（[`prepareRuntimeIdentity()`](../../src/core/loader/scene.cpp#L586)）、resolved scene を検証して（[`validateResolvedSceneFeatureBindings()`](../../src/core/loader/scene.cpp#L604)）、各 participant の candidate を live に触らずに作ります: renderer（[`prepareSceneReplacement()`](../../src/core/loader/scene.cpp#L641)。model の staging もここ）、camera（[`prepareSceneCameras()`](../../src/core/loader/scene.cpp#L686)）、light（[`LightContainer::prepareLoad()`](../../src/core/loader/scene.cpp#L688)）、SeqPlayer / VatPlayer（[`prepareSceneLoad()`](../../src/core/loader/scene.cpp#L692)）、retirement queue の容量予約（[`reserveOne()`](../../src/core/loader/scene.cpp#L143)）、RNG 列（[`prepareSequence()`](../../src/core/loader/scene.cpp#L704)）、event outbox（[`PreparedEventOutbox`](../../src/core/loader/scene.cpp#L708)。`SceneLoaded` の payload はここで確保します）。model は [`ModelAssetContainer::getModelTemplateByName()`](../../src/core/loader/scene.cpp#L653) で引きますが、`ModelAssetContainer` は **participant ではなく warm cache** です。candidate が失敗しても lazy load された model record / GPU resource は残り、戻しません（manifest の [`model_asset` / `warm_cache` 行](../../src/core/loader/sceneparticipantmanifest.cpp#L154)。どの scene にも束縛されない record なので世代混在にはなりません）。

**early_publish**（[`renderer.publishPreparedScene()`](../../src/core/loader/scene.cpp#L790) から [`return fail(SceneLoadPhase::early_publish, ...)`](../../src/core/loader/scene.cpp#L991) まで）では、ECS entity を [`ECSEntityMutation::prepareCreate()`](../../src/core/loader/scene.cpp#L799) の populate callback で作って `publish()` し（JSON は各 Component へここでロード）、名前 binding の candidate を集め、親子 transform を投影し（§4）、physics の candidate を作り（[`PhysWorld::prepareSceneBindings()` の呼び出し](../../src/core/loader/scene.cpp#L967)）、behavior の candidate を作り（[`prepareSceneReconciliation()`](../../src/core/loader/scene.cpp#L981)）、旧 entity を [`prepareDestroy()`](../../src/core/loader/scene.cpp#L985) で unpublish します。ここで投げると [`rollback` ラムダ](../../src/core/loader/scene.cpp#L731) が inverse token を逆順に消費して戻します。

**publish** は [`static_assert(noexcept(...))`](../../src/core/loader/scene.cpp#L995) で固定された swap の列です（[`object_bindings.swap()`](../../src/core/loader/scene.cpp#L1023) → SeqPlayer → VatPlayer → [`Camera::publishPrepared()`](../../src/core/loader/scene.cpp#L1031) → Light → [`PhysWorld::publishPrepared()`](../../src/core/loader/scene.cpp#L1039) → [`behavior_candidate->publish()`](../../src/core/loader/scene.cpp#L1062)）。WP380d 以降、executor が呼ぶのは `SceneLoadPhaseTrace &` を取る scene 専用 overload だけで、`publish` を記録するのは呼ばれた participant 自身です（[`Camera::publishPrepared()`](../../src/core/renderer/camera.cpp#L388) / [`PhysWorld::publishPrepared()`](../../src/core/phys/physworld.cpp#L364)）。rollback 側は同じ swap を trace 無しの overload で戻すので、戻した分が `publish` として trace に増えることはありません。この段は失敗しないので、`PreCommitFailure::phase` に `publish` はありません（[`SceneLoadPhase`](../../src/core/loader/sceneload.hpp#L27) 自体は 8 値ですが、`PreCommitFailure` に載りうるのは `prepare` / `early_publish` / `activate` の 3 つだけです）。

**activate** は [`behavior_candidate->activate()`](../../src/core/loader/scene.cpp#L1076) の 1 行で、`onInit` はこの時点で **全 participant が candidate に切り替わった状態**を観測します。ここで投げると同じ `rollback` ラムダが全 participant を旧世代へ戻し（[`PreCommitFailure{phase = activate}`](../../src/core/loader/scene.cpp#L1081)）、document / projection / current scene / active authority は commit 前なので旧世代のままです。activate 中の `emit` / RNG / `loadScene()` は live queue ではなく candidate（[`EventOutboxScope`](../../src/core/loader/scene.cpp#L726) / [`RngSequenceScope`](../../src/core/loader/scene.cpp#L728) / [`PendingSceneTransactionScope`](../../src/core/loader/scene.cpp#L181)）へ向くので、rollback で一緒に捨てられます。

**commit_metadata** は [`metadata.commit()`](../../src/core/loader/scene.cpp#L1110) の 1 回で、`SceneMetadataState` を丸ごと swap します（§3.2）。以降の段は [`static_assert(noexcept(...))`](../../src/core/loader/scene.cpp#L1084) で `noexcept` が固定されており、`PreCommitFailure` は出ません。

**retire** は旧 state を消すのではなく queue に積むだけです。旧 entity の destroy token と transient model は [`retirement_state->enqueue()`](../../src/core/loader/scene.cpp#L1118)（容量は prepare で予約済み）、renderer の旧 instance slot は [`enqueueSceneRetirement()`](../../src/core/loader/scene.cpp#L1124) へ入り、消費は次フレームです: [`drainRetirementQueue()`](../../src/core/loader/scene.cpp#L1251) が [`applyPendingLoad()` の先頭](../../src/core/loader/scene.cpp#L1208) と teardown の ecs 段（[`teardown.cpp` 内](../../src/core/appflow/teardown.cpp#L80)）で走り、renderer 側は [`drainSceneRetirements()`](../../src/core/renderer/polygoninstancecontainer.cpp#L703) が `triggerUpdate()` の先頭で消費します。そこでの失敗はフレームのエラーであって transaction のエラーではありません（state は既に一貫）。

**flush** は [`stageReserved()`](../../src/core/loader/scene.cpp#L1133) で prepare 済みの `SceneLoaded` を予約 slot へ移し、RNG 列と pending scene を publish してから [`event_outbox->flush()`](../../src/core/loader/scene.cpp#L1138) で live queue へ `noexcept` に push します。

**effect transaction**（audio / persistence / `loadScene` などの副作用）は、この段の列と同じ寿命で 1 つだけ開きます。[`EffectTransactionScope`](../../src/core/loader/scene.cpp#L727) は prepare の直後・`early_publish` に入る**前**に張られるので、component の JSON loader が早期 publish 中に呼ぶ副作用も、behavior の `onInit` と同じ transaction-local な sink を観測します（[executor 側のコメント](../../src/core/loader/scene.cpp#L719)）。実際の適用は flush で、[`effect_transaction->flush()`](../../src/core/loader/scene.cpp#L1143) が staging 時と同じ sequence 順（`0..next_sequence_`）に entry を消化します。したがって pre-commit で投げれば副作用は 1 つも出ず、scene と一緒に巻き戻ります。sink 側（どの公開 API がこの transaction を読み、何が shadow になり何が拒否されるか、coalesce の規則）は [§5.15](05_gameplay_and_services.md) にまとめてあります。

診断の記録は **allocation 禁止** です。[`record()`](../../src/core/loader/effecttransaction.cpp#L92) は [`errors_`](../../src/core/loader/effecttransaction.hpp#L125)（`sceneEffectCapacity + 1` 個の [`FixedEffectError`](../../src/core/loader/effecttransaction.hpp#L107)）の固定 slot を 1 つ取って、文字列も `FixedText<512>` / `FixedText<384>` へ `memcpy` するだけで、`std::string` も `json` も作りません。末尾の 1 slot は診断用ではなく **outbox 溢れ専用**で、[`reserveEntry()`](../../src/core/loader/effecttransaction.cpp#L114) が 256 件を超えて積めなかった**副作用の**件数を `effect_outbox_full@1` の `dropped` として数え続けます（診断そのものが 256 件を超えた場合は数えずに落ちます。非対称の理由は §5.15）。公開用の文字列と JSON は commit 後に初めて作られます — [`effectErrors()`](../../src/core/loader/scene.cpp#L1153) の呼び出しは `noexcept` の commit tail（`flush` / `finish`）を通過した**後**にあり、そこで固定 slot を sequence 順に並べて [`CommittedWithEffectFailure::EffectError`](../../src/core/loader/sceneload.hpp#L177) へ変換します。

副作用の失敗は例外ではなく [`CommittedWithEffectFailure`](../../src/core/loader/sceneload.hpp#L175) として `SceneLoadResult` に載り、**扱いは root ごと**です: startup は [`LOG_ERROR`](../../src/core/userpublic/pelican_core.cpp#L141)、pending も [`LOG_ERROR`](../../src/core/loader/scene.cpp#L1221)、rpc の `load_scene` は [`response["effect_errors"]`](../../src/core/communication/rpcserver.cpp#L1641) として JSON で返し（失敗が無ければ空配列）、import は [`SceneImportResult`](../../src/core/loader/sceneload.hpp#L276) にそのまま入れて呼び手へ渡します。いずれも `committed` であることは変わりません（scene は載っている）。

transient glTF 経路（`load_gltf`）は executor を通らず、従来どおり [`GameObjects::createWithComponents()`](../../src/core/loader/scene.cpp#L1456) の populate callback で作ります（§3.8 の「`load_gltf` はトランザクション」）。

### 4. authored local TRSのworld投影

ECS objectを作った後、同じ early_publish 段の中で親子transformのworld解決を済ませます（[親子 transform の投影](../../src/core/loader/scene.cpp#L859)）。behavior の `onInit`（activate 段）や `SceneLoaded` の配送（flush 段）より前です。

```cpp
// Project authored local TRS to world before any SceneLoaded observer can
// query the runtime. This is the same recurrence as LocalTransformSystem.
```

これは `LocalTransformSystem` と同じ漸化式です。したがって `SceneLoaded` を受け取ったコードは、まだ1フレームも回っていない時点でも world transform を問い合わせられます。

> 🧩 **難所 — preserve-worldの逆算**([`inverseLocal()`](../../src/core/loader/editorprojectiontransaction.cpp#L298) / [`makeReparentCommand()`](../../src/core/loader/editorprojectiontransaction.cpp#L598))
>
> **何をする所か**: 同じ漸化式の逆向きです。エディタが `preserve: "world"` で親を付け替えるとき、新しい親のworld TRSから「worldを保ったままの子のlocal TRS」を逆算します。
>
> **素朴に読むと**: `local = parent^-1 * world` を書くだけに見えます。ところがTRS(平行移動・回転・非一様スケール)は逆演算に対して**閉じていません**。親が非一様スケールと回転を同時に持つと、真の相対変換はせん断を含み、正準TRSでは表現できません。せん断が出る仕組みは合成側([`composeWorld()`](../../src/core/loader/editorprojectiontransaction.cpp#L157))を見ると分かります — `world.pos = parent.pos + parent.R * (parent.S * local.pos)` / `world.R = parent.R * local.R` / `world.S = parent.S * local.S` で、**親のスケールは親の軸に沿って**掛かります。子が回転していると伸縮の軸と子の軸が揃わないので、子の直交していた軸が斜交します。これがせん断で、「回転させた直交軸に沿った軸別スケール」しか書けない正準TRSの表現範囲の外です。素朴に計算しても数値そのものは出るので、壊れ方は「公開した瞬間に物体が歪む/ずれる」という無音の形になります。だからこの関数は計算して終わりではなく、**逆算 → もう一度合成 → 元のworldと一致しなければ `TransformUnrepresentable` で拒否** という表現可能性の検査になっています。検査の位置も一律ではありません。除算の**前**に並ぶゼロスケール判定と親TRSの有限性判定([この2つ](../../src/core/loader/editorprojectiontransaction.cpp#L310))は `inf` / `NaN` を作らないためのガードで、クォータニオンのノルム判定([`norm_squared` の算出](../../src/core/loader/editorprojectiontransaction.cpp#L338))は除算の**後**に来る表現可能性検査の一部です — 前者は `TransformZeroParentScale` / `TransformNonFinite`、後者は `TransformUnrepresentable` と、出るエラーコードも別です。ただし「非有限の検査は除算前だけ」ではありません。逆算した結果そのものの有限性も除算の後にもう一度見ていて([結果側の判定](../../src/core/loader/editorprojectiontransaction.cpp#L333))、こちらのエラーコードは `TransformNonFinite` です。
>
> **骨子**:
> ```text
> |parent.scale.{x,y,z}| <= eps -> TransformZeroParentScale     # 除算前ガード
> parent の pos/rot/scale が非有限 -> TransformNonFinite        # 除算前ガード
> pos      = inverse(parent.rot) * (world.pos - parent.pos) / parent.scale
> rotation = inverse(parent.rot) * world.rot
> scale    = world.scale / parent.scale
> 逆算結果が非有限 -> TransformNonFinite                          # 除算後
> |rotation|^2 が非有限または <= eps -> TransformUnrepresentable  # 除算後
> recomposed = composeWorld(parent, result)
> 全成分が相対許容差 32*FLT_EPSILON 内で一致しなければ TransformUnrepresentable
> ```
>
> **手がかり**: 許容差のラムダ `32.0F * epsilon * max(1, |left|, |right|)` は**相対**許容差で、絶対値の大きい座標でも桁落ちで誤検出しません(下限 `1.0F` があるので原点近傍では絶対許容差として働きます)。`makeReparentCommand()` は [`buildTransformNodes()`](../../src/core/loader/editorprojectiontransaction.cpp#L220) を **3回** 呼びます — 変更前worldの採取、親エッジ書き換え後(コメント通り「staged graphそのものがサイクルのpreflight」)、local差し替え後の事後条件証明です。[`projectNode()`](../../src/core/loader/editorprojectiontransaction.cpp#L195) の `node.state` は 0=未訪問 / 1=訪問中 / 2=完了 の白灰黒DFSで、1へ再入したらそれが親サイクルです。テストは [`Transform projection recomputes descendants and reparent policies`](../../test/editorprojectiontransaction_test.cpp#L628)。
>
> **不変条件**: 再合成の検証はadapterのprepareより**前**に済ませる(ランタイムへ触った後に「実は表現できなかった」を出さない)。許容差を緩めると、無音でずれたreparentが通るようになります。

### 5. 名前binding

名前付きかつ`transform`を持つobjectだけが `object_bindings`へ入ります。executor は live の map には書かず、early_publish 段で `candidate_object_bindings` に集め（[candidate への emplace](../../src/core/loader/scene.cpp#L850)）、publish 段で [`object_bindings.swap()`](../../src/core/loader/scene.cpp#L1023) します（rollback は逆向きの swap）。authoring id → entity の `runtime_object_bindings` も同じ形です。[`bindObjectTransform()`](../../src/core/loader/scene.cpp#L1307) は `load()` からは呼ばれません。RPC、camera controller、physicsはこの名前から世代付き`GameObjectId`を引き直します。

bindingは生ポインタを保持しません。アクセス時に`tryComponent<TransformComponent>()`でIDのgenerationとComponent存在を再検証します。

### 6. SceneLoaded event

[`SceneLoaded`](../../src/core/userpublic/events.hpp#L10) は「ロード完了後にその場で emit」ではありません。payload（`make_shared` と vector の容量）は prepare 段で [`event_outbox->prepare()`](../../src/core/loader/scene.cpp#L710) が確保し、commit 後の flush 段で [`stageReserved()`](../../src/core/loader/scene.cpp#L1133) → [`flush()`](../../src/core/loader/scene.cpp#L1138) が live queue へ `noexcept` に移します（[`PreparedEventOutbox`](../../src/core/userpublic/details/event/registerer.hpp#L324)。`stageReserved()` / `flush()` の実装は [`registerer.cpp` 内](../../src/core/userpublic/details/event/registerer.cpp#L261)）。activate 段で behavior が `emit` したものも同じ outbox に入り、`SceneLoaded` はその**後ろ**に並びます。rollback すると outbox ごと捨てられるので、失敗した load の event が旧世代の behavior に届くことはありません。フレーム末尾のpending loadから flush された場合、eventは次フレーム冒頭に届きます。

### 7. participant manifestとfault injector

上の段の列は「誰が何を持ち替えるか」を散らして持っているので、それを1枚の表として**runtime側に**持っているのが [`sceneParticipantManifest()`](../../src/core/loader/sceneparticipantmanifest.hpp#L53) です。テスト用の別ファイルではなく production の TU にあるので、テストと実装が別々の真実を持つことはありません。

1行が1つの**独立に観測できるフィールド**（またはビルドrootの1本）で、列は `owner` / `field` / `presence` / `role` / `prepare_owner` / `publication` / `rollback` / `retirement` / `snapshot_schema` / `mutation_id` です（[`SceneParticipantManifestEntry`](../../src/core/loader/sceneparticipantmanifest.hpp#L32)）。粒度を粗くしない理由はソースのコメントが書いています — `"renderer state"` のようにまとめた1行を作ると、後から足したカウンタがsnapshot比較にもmutationテストにも掛からずに逃げます（[そのコメント](../../src/core/loader/sceneparticipantmanifest.cpp#L52)）。

`presence` は4値で、**ビルド構成から導かれます**（[`SceneParticipantPresence`](../../src/core/loader/sceneparticipantmanifest.hpp#L9)）。physics / seqplayer / vatplayer は `real` か `stub`、XRとRPCは `real` か `absent`、audioは `real` か `uninstalled` です（[先頭の `#if` 群](../../src/core/loader/sceneparticipantmanifest.cpp#L9)）。「このビルドには無い」と「このビルドでは別物が入っている」を1つの `bool` へ潰さない、という区別です。

`role` は10値あり（[`SceneManifestRole`](../../src/core/loader/sceneparticipantmanifest.hpp#L16)、名前は [`sceneManifestRoleName()`](../../src/core/loader/sceneparticipantmanifest.cpp#L240)）、**executorの外に居るものも明示的に載る**のが要点です。

| role | 意味 | 例 |
|---|---|---|
| `participant` | 段の列に参加し、rollbackで戻る | `metadata.document`、`camera.*`、`ecs.entity_liveness` |
| `invariant` | 参加はするが値が変わらない | [`rng_sequence.increment`](../../src/core/loader/sceneparticipantmanifest.cpp#L149) |
| `cache` | participantではない。失敗しても戻さない | [`model_asset.warm_cache`](../../src/core/loader/sceneparticipantmanifest.cpp#L154) |
| `effect` | effect transactionのoutbox経由。flushで初めて外へ出る | [`audio.play`](../../src/core/loader/sceneparticipantmanifest.cpp#L156) / `persistence.save_data` |
| `non_rollback` | 一度消費したら戻さない | [`audio.handle_cursor`](../../src/core/loader/sceneparticipantmanifest.cpp#L159)（`u64-gap-named-v1` = 失敗すると番号に穴が空く、と明記） |
| `read_only` | outboxを重ねて読むだけ | [`persistence.load_data_overlay`](../../src/core/loader/sceneparticipantmanifest.cpp#L163) / `list_saves_overlay` |
| `reject` | transaction中は拒否する面 | [`gameobjects.direct_mutation`](../../src/core/loader/sceneparticipantmanifest.cpp#L165) / [`behavior_context.retired_self_setters`](../../src/core/loader/sceneparticipantmanifest.cpp#L166) |
| `frame_lifetime` | フレーム寿命でtransactionの外 | [`debug_text.frame_text`](../../src/core/loader/sceneparticipantmanifest.cpp#L167) |
| `production_root` | executorを呼ぶ**入口**の側 | [`root.startup` / `root.rpc` / `root.pending` / `root.import.unversioned` / `root.import.versioned`](../../src/core/loader/sceneparticipantmanifest.cpp#L169) |
| `excluded_wp380b` | 明示的な除外枠 | **現在この role の行は1つもありません**（enumと名前だけが存在） |

`mutation_id` は「この行が主張する挙動を、実際に壊せる注入点」の名前です（[`scene_load_mutation`](../../src/core/loader/sceneload.hpp#L91)）。埋まっているのは実質のparticipant行だけで、`invariant` / `cache` / `non_rollback` / `reject` / `frame_lifetime` / `read_only` と `presence != real` の行では**空でなければならない**、というのが契約テスト側の要求です（[`scene_transaction_contract_test.cpp` 内](../../test/scene_transaction_contract_test.cpp#L140)。`real` なparticipantでmutation付きの行はちょうど10という数まで固定されています）。ビルドで消える面のIDも条件式で空になります（例: `rpc_presence == real ? scene_load_mutation::root_rpc : std::string_view{}`）。

注入側は [`SceneLoadFaultInjector`](../../src/core/loader/sceneload.hpp#L74) の1メソッド `shouldMutate(phase, participant_name, mutation_id)` だけです。ここで使う [`SceneLoadFaultPhase`](../../src/core/loader/sceneload.hpp#L65) が `PreCommitFailure` の3値と**わざと別**である点が重要で、`prepare | publish | activate | commit | retire | flush` の6値、つまり `noexcept` のcommit末尾まで名前を持ちます（[そのコメント](../../src/core/loader/sceneload.hpp#L61)）。「外へ報告する段」と「壊せる辺」は同じ粒度ではない、という割り切りです。

判定のヘルパは [`sceneLoadMutationEnabled()`](../../src/core/loader/sceneloadfault.cpp#L33)（injectorが`nullptr`なら常に偽）と、rootごとの切断を引く [`sceneLoadRootDisconnected()`](../../src/core/loader/sceneloadfault.cpp#L41) で、後者は [`root_mutations`](../../src/core/loader/sceneloadfault.cpp#L16) の表で `SceneLoadRoot` からmutation IDを引きます。加えて `rollbackScene*()` の3本は、**わざと間違ったrollback**（outboxをflushしてしまう / RNG候補を公開してしまう / pending sceneを入れ替えてしまう）を実装したもので、ヘッダのコメントがそう宣言しています（[`sceneloadfault.hpp` 内](../../src/core/loader/sceneloadfault.hpp#L20)）。「正しい実装を消して赤を見る」のではなく、「間違った実装を名前付きで用意して、テストがそれを選ぶ」形です。

> 🧩 **難所 — 注入口はproductionバイナリに存在しない**([`SceneLoader::fault_injector`](../../src/core/loader/scene.hpp#L59) / [`test/sceneloadfault_testing.cpp`](../../test/sceneloadfault_testing.cpp#L5))
>
> **何をする所か**: fault injectorをloaderへ差す唯一の口です。差し口はメンバ変数 `fault_injector` と、staticな既定値 [`default_fault_injector_for_testing`](../../src/core/loader/scene.hpp#L61) の2つで、後者はconstructorが各インスタンスへコピーします（[constructorの初期化子](../../src/core/loader/scene.cpp#L157)）。
>
> **素朴に読むと**: 「テスト専用のsetterがproductionヘッダに宣言されている」ので、出荷バイナリにも入っていそうに見えます。実際には**宣言だけ**で、定義は `src/` の外の [`test/sceneloadfault_testing.cpp`](../../test/sceneloadfault_testing.cpp#L5) にしかありません。ヘッダ側は `namespace test_support` の2関数を前方宣言し、`SceneLoader` がそれを [`friend`](../../src/core/loader/scene.hpp#L65) にするだけです。したがって production の `pelican_core` をリンクしても、シンボルはどこにも現れません — 「無効化されている」ではなく「**存在しない**」が正確です。既定値がnullptrなので、production経路の `sceneLoadMutationEnabled()` は最初の `injector != nullptr` で必ず偽になります。
>
> **骨子**:
> ```text
> scene.hpp        : test_support::set...ForTesting(...) の宣言 + friend 宣言だけ
> scene.cpp        : default_fault_injector_for_testing = nullptr
> test/....cpp     : 定義本体(loader.fault_injector = injector)
> pelican_core     : 上の test TU をソースに含めない
> ctest wp380c_... : dumpbin /symbols で「入っていないこと」を検査
> ```
>
> **手がかり**: それを機械で確かめるのが [`wp380c_production_has_no_fault_setter_symbol`](../../test/CMakeLists.txt#L1813) で、検査は3種類あります。(1) `dumpbin /symbols` の出力に `SceneLoadFaultInjectorForTesting`（およびmodel release側の3つ）が現れないこと（[その `string(FIND)`](../../test/run_wp380c_no_production_fault_setter.cmake#L17)）。(2) test-onlyのTUが `pelican_core` の `SOURCES` に混ざっていないこと（[SOURCES 内の TU 照合](../../test/run_wp380c_no_production_fault_setter.cmake#L25)）。(3) productionヘッダ側に `set...ForTesting(...) {` という**定義**が書かれていないこと — 正規表現で本体つき定義を探します（[production header の列挙](../../test/run_wp380c_no_production_fault_setter.cmake#L31)）。3番目があるのは、ヘッダにinline定義を1行書くだけで(1)も(2)もすり抜けられるからです。この検査はWindows（`dumpbin` がある構成）限定です。
>
> **不変条件**: setterの定義をヘッダへ書かない（inlineでも）。test-onlyのTUを `pelican_core` のソースへ足さない。`default_fault_injector_for_testing` の既定値をnullptr以外にしない。

## 3.6 即時loadと要求load

- `SceneLoader::load(SceneLoadRequest)`はその場で全sceneを置換（commit まで到達した場合。§3.5 の 3）。production の root は startup（[`pelican_core.cpp` 内](../../src/core/userpublic/pelican_core.cpp#L133)）/ RPC `load_scene`（[`rpcserver.cpp` 内](../../src/core/communication/rpcserver.cpp#L1627)）/ pending apply / snapshot import（[`importSceneDocument()` からの `Owned` request](../../src/core/loader/basicconfig.cpp#L858)）で、いずれも [`requireSceneLoadCommitted()`](../../src/core/loader/scene.cpp#L461) で `PreCommitFailure` を従来どおりの例外へ戻します。
- [`requestLoad()`](../../src/core/loader/scene.cpp#L1176) は存在確認後、scene IDだけをpendingへ保存。activate 段の behavior から呼ばれた場合は live ではなく candidate projection に対して存在確認し、要求も candidate 側に積みます（rollback で一緒に捨てる）。
- [`applyPendingLoad()`](../../src/core/loader/scene.cpp#L1207) はフレームのゲーム更新末尾で、まず前回 commit 分の retirement queue を消費し、apply 時点の committed projection に late-bind した `Borrowed` request で実際にload。

`GameContext::loadScene()`はrequest型です。RPCの`load_scene`は即時loadです。この違いは、ゲームSystem update中にECS全削除が起きないようにするためです。

rootは列挙型として名前を持ちます（[`SceneLoadRoot`](../../src/core/loader/sceneload.hpp#L82)）。`unspecified` を除く5値がそのまま §3.5 の 7 の `production_root` 行に対応し、importだけは snapshot schema version で2つに割れます — [`importSceneDocument()`](../../src/core/loader/basicconfig.cpp#L798) が `snapshot_schema_version == 1` なら `import_unversioned`、それ以外なら `import_versioned` を選びます（[その三項演算子](../../src/core/loader/basicconfig.cpp#L860)）。

結果の扱いもrootごとに違います。startup / RPC / pending / import はいずれも [`requireSceneLoadCommitted()`](../../src/core/loader/sceneload.hpp#L296) を通して `PreCommitFailure` だけを例外へ戻しますが（ヘッダのコメントが規範 — 「pre-commit失敗は従来どおりthrow、commit済みのeffect失敗は成功なので呼び手へ返す」）、`CommittedWithEffectFailure` の運び方は分かれます。importは [`SceneImportResult`](../../src/core/loader/sceneload.hpp#L276) が `revision` と `load_result` を**組で**返すので、呼び手は「revisionは進んだが副作用が失敗した」を1つの値として受け取れます。`SceneRevision` への暗黙変換演算子があるため、revisionだけ見ていた既存の呼び手はそのまま動きます。startup と pending は `LOG_ERROR`、RPCは `effect_errors` 配列でJSONに載せます（§3.5 の 3）。

## 3.7 ComponentのJSONロード

sceneのcomponent名から型への変換は [`ComponentInfoManager`](../../src/core/ecs/componentinfo.hpp#L60) が担います。

1. runtime登録時に `name → ComponentId` を保存。
2. `ComponentId → ComponentInfo`からJSON loader callbackを得る。
3. [`ComponentInfoManager::loadByJson()`](../../src/core/ecs/componentinfo.cpp#L189) が`JsonArchiveLoader`を作る。
4. Componentの`ref(ar)`を型消去callback越しに呼ぶ。

`ref()`を持たないComponentはscene JSONからロードできず、明示エラーになります。JSON archiveの対応型は [`jsonarchive.hpp`](../../src/core/userpublic/serialize/jsonarchive.hpp#L12) に固定されています。

## 3.8 Asset modelから描画instanceまで

[`ModelAssetContainer`](../../src/core/asset/model.hpp#L22) は`asset_data.json.models`を読み、拡張子に応じて [`GltfLoader::loadGltf()`](../../src/core/model/gltf.cpp#L2690) / [`loadGltfBinary()`](../../src/core/model/gltf.cpp#L2679) を呼び、名前から`ModelTemplate`へmapします。受理拡張子は`.glb`/`.gltf`に加え **`.vrm`** です（[asset/model.cpp](../../src/core/asset/model.cpp#L282)）。`.vrm` がglTFと同列なのは別形式だからではなく、**VRMがバイナリglTF（GLB）コンテナそのもの**だからです — 拡張が増えているだけなので、同じbinary loader経路がそのまま読みます（受理判定の直前のコメントがそう書いています）。VRM固有の意味（humanoid bone / expression / lookAt / firstPerson）は、読み込んだglTF拡張から後段の [`vrmsemantic.hpp`](../../src/core/model/vrmsemantic.hpp) がデコードします。glTF scene fragmentの抽出には [`loadGltfBinarySceneNode()`](../../src/core/model/gltf.cpp#L2685) が使われます。

これらの `loadGltf*()` は、現在は三段APIのラッパです（[`gltf.hpp`](../../src/core/model/gltf.hpp#L54)）。

| 段 | 関数 | 性質 |
|---|---|---|
| prepare | [`prepareGltf()`](../../src/core/model/gltf.cpp#L2573) / [`prepareGltfBinary()`](../../src/core/model/gltf.cpp#L2566) / [`prepareGltfBinarySceneNode()`](../../src/core/model/gltf.cpp#L2580) | ファイル読み取りとparseまで |
| inspect | [`inspect()`](../../src/core/model/gltf.cpp#L2660) | **副作用なし**の候補パス。fragment解決と検証だけ |
| commit | [`commit()`](../../src/core/model/gltf.cpp#L2593) | GPU資源を確保して `ModelTemplate` を作る |

glTFロードの大まかな変換は次です。

```text
tinygltf Model
→ accessor/bufferからCommonPolygonVertDataを構築
→ VertBufContainerの大きなGPU vertex/index poolへ追記
→ textureをMaterialContainerへ登録
→ materialごとにPrimitiveRefInfoをまとめる
→ ModelTemplate
```

執筆基準時点から要素が増えています。VRM semanticデコード（[`vrmsemantic.hpp`](../../src/core/model/vrmsemantic.hpp) — humanoid bone / expression / lookAt / firstPerson、WP111）、morph target（[`morphtarget.hpp`](../../src/core/model/morphtarget.hpp)、WP121）、skeletal animation（[`skeletalanimation.hpp`](../../src/core/model/skeletalanimation.hpp)）、KTX2テクスチャ（[`loader/ktx2.hpp`](../../src/core/loader/ktx2.hpp)、BC5/BC7 fixtureあり）、atlas asset（[`asset/atlasasset.hpp`](../../src/core/asset/atlasasset.hpp) + [`renderer/atlasassetresource.hpp`](../../src/core/renderer/atlasassetresource.hpp)）です。モデルのhot reload（HR2-G、WP110）に伴い、世代管理は [`MaterialContainer::releaseModelResources()`](../../src/core/material/materialcontainer.hpp#L334) が担います。この API は `[[nodiscard]]` で、返り値の [`ReleaseReport`](../../src/core/model/modelresourcerelease.hpp#L36) は固定容量 8 の `ReleaseError`（`{stage, code, index}`。`index` は実 ID）と、9 件目以降を数える `overflow_count` だけを持ち、報告のために確保しません。[`releaseModelResources()` の本体](../../src/core/material/materialcontainer.cpp#L2455) は material も texture も resource 1 件ずつ try で囲むので、1 件の失敗で後続の返却が止まりません。同期経路は失敗を例外（`ModelResourceReleaseException`）に載せて運び、[`releaseErrorStatus()`](../../src/core/loader/scene.cpp#L206) が stage 名の列（溢れた分は `overflow:N`）へ畳んで [`PreCommitFailure::release_errors`](../../src/core/loader/sceneload.hpp#L143) に入れます。呼び元へ report を返せない destructor / deferred release / retirement queue / teardown の各経路は [`ReleaseErrorSink::record()`](../../src/core/model/modelresourcerelease.cpp#L11)（log + `release_errors` / `overflow_count` の累積カウンタ。[`status()`](../../src/core/model/modelresourcerelease.cpp#L28) で読めます）へ渡します（WP380d）。

失敗したときに**誰が解放するか**も、この API の一部です（WP380a-fix）。`ModelAssetContainer` は §3.5 の 3 のとおり participant ではなく warm cache なので、scene load が失敗しても cache は戻りません。ただし「戻さない」と「作りかけを残す」は別です。[constructor](../../src/core/asset/model.cpp#L467) は宣言を1件ずつ commit していき、途中で投げたときに二段で GPU 資源を返します — 内側の catch が**その1件**（[`releaseModelGpuResources(model, false)`](../../src/core/asset/model.cpp#L703)）、外側の catch が**それまでに登録済みの全件**（[`impl_->releaseAllModels()`](../../src/core/asset/model.cpp#L515)）です。どちらも report が空でなければ `ModelResourceReleaseException` に載せ替えて投げ直します。加えて `Impl` の [destructor](../../src/core/asset/model.cpp#L385) と、公開前の reload candidate を持つ [`ModelPayload` の destructor](../../src/core/asset/model.cpp#L327) が `ReleaseErrorSink` 経由で同じ解放を行います。**公開されなかった `ModelAssetContainer` は cache になりません**（moduleとして `__ready()` が立たない。§2.2）ので、部分状態を再利用するより資源を戻すほうが正しい、という選択です。

同じ WP の残り半分は**順序**です。起動経路では render graph の検証と公開が scene の prepare より**前**にあります（[`(void)GET_MODULE(Renderer)`](../../src/core/userpublic/pelican_core.cpp#L130) とその上のコメント。§2.1 の 10番）。この2つが逆だと、rendering config が壊れているプロジェクトでも先に model を upload してしまい、(a) 利用者が最初に見る fatal が material route の失敗ではなく morph 頂点 pool の容量超過にすり替わり、(b) 同一プロセスでの再試行ごとに pool の offset が積み上がります。順序と解放は独立した2つの性質で、片方だけ戻すともう片方のテストが落ちます。

> 🧩 **難所 — 128枚の結合palette**([`selectSkin()`](../../src/core/model/gltf.cpp#L1340) / [`loadMesh()`](../../src/core/model/gltf.cpp#L2056))
>
> **何をする所か**: 1つのglTFにskinが複数あっても、joint を **単一の128スロットpaletteへ順に連結** します(palette は matrix palette — 各ジョイントの変換行列を番号順に並べた配列で、頂点シェーダは頂点が持つ `JOINTS_0` の番号でここを引き、`WEIGHTS_0` の重みで合成します)。各skinは `palette_offset` を貰い、そのskinを使うprimitiveの `JOINTS_0` にoffsetを足して書き換えます。
>
> **素朴に読むと**: 1行なのに理由が重い箇所が3つあります。(1) `joints[component] + joint_offset` の再基準化 — glTFの `JOINTS_0` は**そのskinのjoints配列内のローカルindex**ですが、GPU側のpaletteはインスタンスあたり128枚固定ストライドの1本のバッファです。offsetを足さないと、2つ目のskinのメッシュが1つ目のskinの骨で動きます。範囲チェックがoffsetを足す**前**にあるのも必然で、足した後だと隣のskinの領域へ食い込む不正値を通してしまいます。(2) `if (!skinned) transformVertexData(dat, world_transform);` — skinnedなメッシュにだけnodeのworld変換を焼き込みません。glTF仕様が「skinned meshを参照するnodeの変換は無視する」と定めていて、inverse bind matrix(逆バインド行列 — bindポーズ(モデルを作ったときの基準姿勢)におけるそのジョイントのworld変換の逆行列で、頂点をジョイントのローカル空間へ引き戻すために掛けます)が既にその分を含むからです。焼けば二重に掛かってモデルが吹き飛び、逆にunskinnedへ焼かなければ階層の位置が全部原点へ寄ります。(3) paletteに収まらないskinを持つメッシュはthrowせず、警告つきで **skinning無しでロード**します(`selectSkin()` を明示的に呼ぶアニメーション側だけがthrow)。「読めるが動かない」を意図的に作っているので、この分岐を知らないと `maxSkinJoints` に辿り着けません。
>
> **骨子**:
> ```text
>   skin A (joints 0..29)     skin B (joints 0..17)
>         ▼ offset=0                ▼ offset=30
>   [ A0 A1 … A29 | B0 B1 … B17 | 未使用 … ]   ← 0..127
>   JOINTS_0(B の primitive): 生値 j → j + 30
>   buildSkinPalette(): palette[offset+i] = model_matrix[layout_node[i]] * inverse_bind[i]
> ```
>
> **手がかり**: `skin_joint_offsets` は「このskinは既にpaletteへ載せた」というメモ(skin index → offsetのmap)です。防ぎ方は単純で、`selectSkin()` は本体に入る前にこのmapを引き、載せ済みなら**記録済みのoffsetを返して即座に戻ります**([`gltf.cpp` 内](../../src/core/model/gltf.cpp#L1108))。joint配列への追記([その位置](../../src/core/model/gltf.cpp#L1378))はその後ろにあるので、同じskinを参照するメッシュが何個あってもjointが積まれるのは最初の1回だけです。分割側の [`addBinding`](../../src/core/animation/animationjobs.cpp#L178) は `expected_offset` を進めながら「binding群がpaletteを隙間なく覆っているか」を検証します — [`buildSkinPalette()`](../../src/core/animation/animationjobs.cpp#L403) 自体は `std::vector<Matrix4fV1> produced(required)` を値初期化してから `[0, required)` を丸ごとコピーする([`produced()`](../../src/core/animation/animationjobs.cpp#L411) / [`std::copy()`](../../src/core/animation/animationjobs.cpp#L428))ので、穴は未初期化ではなく**ゼロ行列**になります。壊れ方は不定値ではなく「その関節に属する頂点が原点へ潰れる」という決まった形で、覆い漏れを弾く責任は `addBinding` 側にあります。`maxSkinJoints = 128`([`skeletalanimation.hpp` 内](../../src/core/model/skeletalanimation.hpp#L11))とGLSL側の `PELICAN_MAX_SKIN_JOINTS`([pelican_skinning.glsl](../../src/core/resources/shaders/include/pelican_skinning.glsl))は同じ値の二重定義です。
>
> **不変条件**: binding群はoffset 0から結合paletteを隙間なく覆う。`JOINTS_0` は必ず `joint_offset` 加算済みでGPUへ届く。skinned primitiveの頂点はnode変換を含まない。

> 🧩 **難所 — 頭に触れたら三角ごと**([`splitVrmAutoTriangles()`](../../src/core/model/vrmfirstperson.cpp#L23) / 呼び出しは [`gltf.cpp` 内](../../src/core/model/gltf.cpp#L1842))
>
> **何をする所か**: VRM 1.0 の firstPerson アノテーションが `auto` のメッシュを、「一人称でも見える三角形」と「三人称専用の三角形」の2群へ分けます。firstPerson は VRM が node 単位に付ける「一人称視点でこのメッシュをどう扱うか」の注釈で（[`VrmFirstPersonMeshAnnotation`](../../src/core/model/vrmsemantic.hpp#L76) の `node` + `type`）、`type` が `auto` のときだけ「頭に属する三角形はどれか」の判定がエンジン側の仕事になります。`auto` の規則は「頂点が Head かその子孫に正の重みを持つなら、その三角形は頭グループ」です（[`vrmfirstperson.hpp` のコメント](../../src/core/model/vrmfirstperson.hpp#L27)）。
>
> **素朴に読むと**: まず索引が3段に変換されるのが読みづらい所です。`JOINTS_0` の値は**そのskinのjoints配列内のローカル番号**で、`model.skins[skin_index].joints` を引いて初めてglTFのnode indexになり、そこから頭関連ノード表([`headRelatedNodes()`](../../src/core/model/gltf.cpp#L2029))を引いて分類します。もう一つが呼び出し**位置**の制約です。この呼び出しは、直後にある `joints[component] + joint_offset` の書き換え([`gltf.cpp` 内](../../src/core/model/gltf.cpp#L1916))より**前**でなければなりません。offset加算後へ動かすと、渡した `skin_joint_nodes` の範囲外になるか、たまたま範囲内なら別ボーンとして解決されて静かに誤分類されます。この順序制約はコード上どこにも書かれておらず、隣接する2箇所の並びとしてしか表れていません。分類規則自体も非対称です — 重み `> 0` の成分だけを見る(重み0のjointスロットは無視する)一方、頂点が1つでも頭関連なら**三角形ごと** `third_person_only_indices` へ落とします。`first_person_only` の群を作らないのは、VRMの `auto` が「一人称で頭を消す」だけを意味し、逆方向は明示アノテーションでしか作れないからで、[`VrmAutoTriangleSplit`](../../src/core/model/vrmfirstperson.hpp#L22) がバケツを2本しか持たないのはその反映です。
>
> **骨子**:
> ```text
> joint(JOINTS_0 のローカル番号) -> skin_joint_nodes[joint] -> glTF node index
> weight > 0 かつ head_related_nodes[node] != 0 -> その頂点は頭関連
> 三角形の3頂点のどれかが頭関連 -> third_person_only、それ以外 -> both
> ```
>
> **手がかり**: `indices` が空なら 0,1,2,… を生成して三角形列として扱う([index 列の生成](../../src/core/model/vrmfirstperson.cpp#L10))ので、非indexedプリミティブも同じ経路です。TRIANGLES以外は呼び出し側([`primitive_mode` の検査](../../src/core/model/gltf.cpp#L2197))と本体([三角形数の検査](../../src/core/model/vrmfirstperson.cpp#L25))の両方で弾きます。分割後は both 側と third 側が別プリミティブとして `variants` へ積まれる([auto split の variant 登録](../../src/core/model/gltf.cpp#L2241))ので、1つのglTFプリミティブから2つの描画レンジが生まれます。ヘッダのコメントが規範で、各群の中では**元の三角形順が保たれます**([`vrmfirstperson.hpp` 内](../../src/core/model/vrmfirstperson.hpp#L27))。テストは [`vrmfirstperson_test.cpp` 内](../../test/vrmfirstperson_test.cpp#L7) と [`morph_gltf_test.cpp` 内](../../test/morph_gltf_test.cpp#L240)。
>
> **不変条件**: 呼び出しは `joint_offset` 加算より前。分類の単位は頂点ではなく三角形。`auto` が返すのは both / third の2群だけ。

> 🧩 **難所 — -1を空けて負へ伸ばす**([`skinnedMaterial()`](../../src/core/model/gltf.cpp#L1968) / [`registerVatMaterial()`](../../src/core/model/gltf.cpp#L723))
>
> **何をする所か**: 頂点シェーダだけを差し替えた派生マテリアルを登録し、そのIDをモデル内ローカルのmaterial ID空間へ割り当てます。
>
> **素朴に読むと**: `using ModelLocalMaterialId = int;`([宣言](../../src/core/model/gltf.cpp#L518))はglTFのmaterial indexをそのまま入れる型に見えます。ところが [同じ構造体](../../src/core/model/gltf.cpp#L542) に `next_generated_material = -2` があり、派生を作るたびに**減って**いきます。ID空間が符号で二分されていて、0以上は「glTFが書いたmaterial」、負は「エンジンが派生させたmaterial」です。`-1` から始めないのは、`-1` がglTF側の『material無し』を表す予約値だからで、採番から外してあります。派生を作る理由はvert shaderの差し替えだけで、skinnedは `skinnedVertShader()`、VATは `vatVertShader()` へ変えた同内容のコピーを `registerMaterial()` し直します — 同じ元materialから最大3種の実体が生まれます。ここで `generated_material_sources` に「派生ID → 元のsource index」を残すのが要です。実行時のマテリアル上書きは authored 側の index で来て、[`polygoninstancecontainer.cpp` 内](../../src/core/renderer/polygoninstancecontainer.cpp#L1892) がその index で初期値表を引き直すため、この表が無いと [`generated_source` の復元](../../src/core/model/gltf.cpp#L2526) で `source_material_index` が `noSourceMaterialIndex` に落ち、skinnedプリミティブにだけ上書きが効かないという片側だけの不具合になります。
>
> **骨子**:
> ```text
> 0,1,2,…    glTF の material index(authored)
> -1         material 無し(予約・採番しない)
> -2,-3,…    skinned / VAT 派生(next_generated_material--)
> generated_material_sources[派生ID] = 元の material index(無ければ noSourceMaterialIndex)
> ```
>
> **手がかり**: `skinned_material_variants` は「この元materialのskinned派生は作成済み」というキャッシュ([参照箇所](../../src/core/model/gltf.cpp#L1585))で、同じ元から二重に派生を作らない同一性保証も兼ねます(`registerVatMaterial()` に同種のキャッシュが無いのは、`base_vertex` がプリミティブごとに違って共有できないためです)。`source_material_index` は再グループ化のキーでもあり、[`modeltemplate.cpp` 内](../../src/core/model/modeltemplate.cpp#L176) は「解決後のmaterial」と「元のindex」の**組**でまとめ直します。テストは [`material absolute overrides are source-material scoped and temporal`](../../test/materialinstanceoverride_test.cpp#L256)「material absolute overrides are source-material scoped」。
>
> **不変条件**: `-1` は採番しない。派生を登録したら必ず `generated_material_sources` へ元のindexを残す。負のIDは `ModelTemplate` の外へ出さない(外向きは `source_material_index`)。

> 🧩 **難所 — morphはnode単位で切る**([`appendMorphDefaults()`](../../src/core/model/gltf.cpp#L1158) / [`loadMesh()`](../../src/core/model/gltf.cpp#L2056))
>
> **何をする所か**: モデル全体で1本の `default_weights` 配列へこのmesh分の既定weightを追記し、そのスライスの先頭位置を `morph_weight_offset` として返します。
>
> **素朴に読むと**: `appendMorphDefaults()` は「追記する前の `default_weights.size()`」を返すだけで([その return](../../src/core/model/gltf.cpp#L1195))、そのsizeがいつ増えるのかは呼ばれ方に依存します。鍵は `loadMesh()` が **nodeごとに**呼ばれる点で([`loadMesh()` 呼び出し](../../src/core/model/gltf.cpp#L2378))、同じmeshを2つのnodeが参照すれば独立した2スライスができ、nodeごとに別の表情を付けられます。つまりoffsetは「mesh単位」でも「target単位」でもなく「**このnodeのこのmesh**のスライスの先頭」です。GPU側は `instance_index * PELICAN_MAX_MORPH_WEIGHTS + weight_offset` でweightを引く([`pelican_morph.glsl` 内](../../src/core/resources/shaders/include/pelican_morph.glsl#L72)、CPU側の書き込みは [`vertbufcontainer.cpp` 内](../../src/core/model/vertbufcontainer.cpp#L226))ので、offsetをprimitiveから外してmeshに1つ持たせると、同じmeshを共有する2体の表情が連動します。容量検査([この検査](../../src/core/model/gltf.cpp#L925))を追記の**前**に置いてあるのも意図で、途中まで積んで失敗したlayoutを残さないためです。`node.weights` が `mesh.weights` より優先されるのはglTF仕様どおりですが、要素数が合わなければ黙ってmesh側へ落ちず、node名を添えてthrowします([その throw](../../src/core/model/gltf.cpp#L936))。
>
> **骨子**:
> ```text
> default_weights: [ nodeA/mesh0 の 4 個 | nodeB/mesh0 の 4 個 | nodeC/mesh1 の 2 個 | … ]
>                    ^offset=0             ^offset=4             ^offset=8
> appendMorphDefaults(): 容量検査 -> offset = size() -> target_count 個 push_back -> offset を返す
> GPU: weight_base = instance_index * PELICAN_MAX_MORPH_WEIGHTS + metadata.weight_offset
> ```
>
> **手がかり**: `target_count == 0` なら何も積まず 0 を返す([`target_count == 0` の早期 return](../../src/core/model/gltf.cpp#L1160))ので、morphを持たないプリミティブは全部offset 0を共有します(`target_count` が0なのでシェーダは即returnします、[`pelican_morph.glsl` 内](../../src/core/resources/shaders/include/pelican_morph.glsl#L70))。同じ容量検査は [`std::to_string()`](../../src/core/model/vertbufcontainer.cpp#L156) にもあり、GPU公開の直前でもう一度掛かります。`maxMorphWeightsPerInstance = 256`([`morphtarget.hpp` 内](../../src/core/model/morphtarget.hpp#L14))はインスタンスあたりのweight総数の上限で、プリミティブあたりのtarget数の上限 `maxMorphTargetsPerPrimitive = 64` とは別物です。テストは [`morph_gltf_test.cpp` 内](../../test/morph_gltf_test.cpp#L61) / [per-instance の weight frame](../../test/morph_gltf_test.cpp#L132)。
>
> **不変条件**: `morph_weight_offset` はprimitiveが持つ(mesh単位へ移さない)。容量検査は `default_weights` へ積む前に行う。`node.weights` の要素数不一致はthrowであってfallbackではない。

sceneの`SimpleModelViewComponent`は初期化時にdirtyになり、内部 [`SimpleModelViewUpdateSystem`](../../src/core/ecs/predefined/modelviewupdatesystem.cpp#L42) がmodel名からtemplateを引き、`PolygonInstanceContainer`へinstanceを置きます。

### `ModelInstanceId` はSlotMapハンドル（WP146）

`ModelInstanceId` は単なる整数の添字ではありません（[`ModelInstanceId`](../../src/core/renderer/modelinstance.hpp#L11)）。SlotMap は「配列のスロットを使い回しつつ、スロットごとに世代番号を持たせるハンドル方式」で、詳しい持ち物はこの節の最後（`PolygonInstanceContainer` 側）に書いてあります（[用語集](../manual/14_glossary.md)）。

```cpp
struct ModelInstanceId {
    std::uint32_t index = std::numeric_limits<std::uint32_t>::max();
    std::uint32_t generation = 0;
    std::uint64_t scene_epoch = 0;

    auto operator<=>(const ModelInstanceId &) const = default;
};

inline constexpr ModelInstanceId invalidModelInstanceId{};
```

文字列化は [`toString(id)`](../../src/core/renderer/modelinstance.hpp#L21) で `"index:generation@scene_epoch"` になります。ログやRPCの応答でこの形を見たらmodel instanceのハンドルです。

`PolygonInstanceContainer` 側はSlotMap(スロットマップ — 配列のスロットを使い回しつつ、スロットごとに世代番号を持たせるハンドル方式。空きスロットの番号はfree listへ積んで再利用し、再利用のたびに世代を進めるので、古いハンドルは世代不一致で弾けます)として `instance_generations` / `instance_alive` / `free_instance_indices` / `live_instance_count` / `scene_epoch` を持ちます（[`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L266)）。

> **設計決定:** slot identityは animation generation やmodel assetのcontent revisionから独立しています。`scene_epoch` が進むのは **model-instance slotが死んだとき**だけで、モデルのhot reloadでは進みません。これにより「アセットを差し替えても、生きているinstanceハンドルは有効なまま」という性質が保てます。

API面の主な変化は次の通りです。

| API | 位置 | 内容 |
|---|---|---|
| `removeModelInstance()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L334) | 戻り値が `void` → **`bool`** |
| `preflightModelInstance()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L327) | 確保前の容量チェック（const） |
| `stageModelInstance()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L328) | [`StagedModelInstance`](../../src/core/renderer/polygoninstancecontainer.hpp#L257) を返す |
| `publishModelInstance()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L331) | `noexcept` の単一公開点 |
| `prepareTrs()` / `publishPreparedTrs()` / `rollbackPreparedTrs()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L346) | TRS更新のprepare/publish |
| `isModelInstanceAlive()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L353) | 生存判定 |
| `instanceCountForTesting()` | [`advanceMorphHistoryAfterRender()`](../../src/core/renderer/polygoninstancecontainer.hpp#L439) | **live数**（`live_instance_count`）を返す |
| `slotCountForTesting()` | [`advanceMaterialOverrideHistoryAfterRender()`](../../src/core/renderer/polygoninstancecontainer.hpp#L440) | スロット総数 |

テストは [`test/modelinstance_slotmap_test.cpp`](../../test/modelinstance_slotmap_test.cpp)（GPUラベル）です。

### `load_gltf` はトランザクション（WP144）

RPCの `load_gltf` が使う [`SceneLoader::loadTransientGltf()`](../../src/core/loader/scene.cpp#L1398) は、「**割り当てを全部公開の前に済ませ、公開点を1箇所に絞る**」形で書かれています。

1. 名前bindingのhash nodeを先に `extract()`(C++17のnode handle — 連想コンテナから要素をノードごと切り離して持ち出すAPI。取り出したノードを戻す `insert()` は確保を伴いません)して確保（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L1412)）。コメント通り、エンティティ生成後にこのnodeを差し込む操作は割り当てを伴わないため、トランザクションを分割できません。
2. `prepareGltf*()` → [`inspect()`](../../src/core/model/gltf.cpp#L2660) の副作用なし候補パス → [`preflightModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L428) で、**model固有のVulkan資源を1つも確保する前に**容量超過を拒否します。
3. [`commit()`](../../src/core/model/gltf.cpp#L2593) でGPU資源を確保し、`stageModelInstance()` でstagingします。
4. エンティティ生成がthrowしたら `releaseModelGpuResources()` して `transient_models.pop_back()` します。
5. 単一公開点（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L1467)）。

```cpp
// No operation below allocates: this is the single publication point for
// the entity's slot, draw commands, resources, and optional name.
instances.publishModelInstance(std::move(staged_instance));
```

`SceneLoader` には [`transient_models`](../../src/core/loader/scene.hpp#L54) / [`runtime_only_changes`](../../src/core/loader/scene.hpp#L56) / [`releaseTransientModels()`](../../src/core/loader/scene.hpp#L74) / [`hasRuntimeOnlyChanges()`](../../src/core/loader/scene.hpp#L108) が追加されています。`runtime_only_changes` は編集RPC側で「これはruntime専用の変更なので保存できない」と判定するために使います。

## 3.9 その他の純粋形式

> 🧩 **難所 — 端数は先頭から配る**([`distribute()`](../../src/core/ui/layout.cpp#L93))
>
> **何をする所か**: stackレイアウトで、固定サイズを引いた残り幅を `Fill` の子へweight比で配り、整数除算で出た端数まで配り切ります。
>
> **素朴に読むと**: `while (!pool.empty())` の中でclampされた要素を pool から外し、`remaining` と `total_weight` を引き直してやり直す形が、目的の見えない不動点ループに見えます。これはflexboxのfreezeループと同じ形で、`min` / `max` に当たった要素をその値で凍結してから残り幅と総weightを引き直す処理です。1パスの比例配分で済ませると、clampされた分の余りが誰にも配られずレイアウトに隙間が残ります。凍結が1つも起きなかった回で初めて確定させ、`break` で抜けます(この `break` が無いと同じ配分を延々と繰り返します)。後半の `pool[(remaining - assigned - rem) % pool.size()]` は整数除算の切り捨てで生じた `R = remaining - assigned` ピクセルを配る処理で、`rem` を `R` から1へ減らすと `(remaining - assigned - rem)` は 0,1,2,… と増えるので、結果は「poolの先頭R要素に1pxずつ」です。丸め誤差を最後の要素へ押し付けたり四捨五入したりしないのは、レイアウトが `std::int64_t` の厳密ピクセルで決定性を要求するからで、同じdocumentと同じ幅なら常に同じ1pxの配り先になります。`remaining < 0` のときにfill要素を `min` に落として即returnする分岐([その分岐](../../src/core/ui/layout.cpp#L106))も、負の `remaining` をweightで割らないための前提です。
>
> **骨子**:
> ```text
> remaining = available - Σ(固定サイズ)
> remaining < 0 -> fill を全部 min にして return
> loop:
>   share   = remaining * weight / total_weight
>   bounded = clamp(share, min, max)
>   bounded != share -> 凍結: sizes[i]=bounded; remaining-=bounded; total_weight-=weight; pool から除去
>   1つも凍結しなかった -> 比例配分を確定し、余り R を pool[0..R) へ 1px ずつ足して break
> ```
>
> **手がかり**: `weight` はパース時に正であることが保証されている([`document.cpp` 内](../../src/core/ui/document.cpp#L73) の `weight must be positive`)ので、poolが空でない限り `total_weight` は0になりません — `total_weight == 0 ? 0 : …` は防御的な分岐です。`pool` の並びは子の宣言順のまま(`fill` を宣言順に積み、`erase` は順序を保つ)なので、「先頭R要素」は宣言順で先に来る `Fill` の子です。テストは [`ui_foundation_test.cpp` 内](../../test/ui_foundation_test.cpp#L190)「stack layout distributes integer remainder」。
>
> **不変条件**: 凍結が起きた回は確定せず必ずもう一周する。端数は宣言順の先頭から1pxずつ配る(最後の要素へまとめない)。負の `remaining` をweight除算へ渡さない。

> 🧩 **難所 — 丸めは足してから床**([`anchorEdge()`](../../src/core/ui/types.cpp#L62) / [`scaleEdge()`](../../src/core/ui/types.cpp#L70) / [`checkedI32()`](../../src/core/ui/types.cpp#L75))
>
> **何をする所か**: UI座標をbinary64(IEEE 754の倍精度浮動小数点)で計算してから整数ピクセルへ落とし、float32で厳密に表せる範囲を超えた座標を拒否します。
>
> **素朴に読むと**: `t2 + 0.5` を `floor` するだけの3行に見えますが、宣言側のコメントがこれを正の設計演算として明記しています — `floor((binary64(edge) + binary64(size)*anchor) + 0.5)`([`types.hpp` 内](../../src/core/ui/types.hpp#L63))。`std::round` / `std::lround` を使わないのは、それらが丸めモードに関係なくhalf-away-from-zero(0から遠い側へ丸める)で、`-0.5` と `+0.5` が非対称になるからです。`floor(x + 0.5)` なら規則が1つで、負側でも同じ向きに揃います。ただし `x + 0.5` の**加算自体**が丸められるので、FPUの丸めモードが `FE_TONEAREST`(最近接偶数丸め)でないと同じ入力から違うピクセルが出ます — 両関数の先頭にある `assert(std::fegetround() == FE_TONEAREST)` はその前提の宣言であって飾りではありません(ImGuiや外部DLLが丸めモードを触ると壊れます)。`checkedI32()` がint32の範囲だけでなく `|value| > 2^24` でもthrowするのは、UI矩形が最終的に [`buildDrawBatch()`](../../src/core/ui/drawcommands.cpp#L10) で `static_cast<float>` の頂点座標になるからです([頂点への変換](../../src/core/ui/drawcommands.cpp#L39))。float32が整数を厳密に表せるのは 2^24 までで、それを超えると「1pxずれる」ではなく「隣のピクセルと同じ座標になる」= クリップと当たり判定が食い違います。int32の範囲で通してしまうと、この破綻はGPU上でしか現れません。
>
> **骨子**:
> ```text
> anchorEdge : assert(FE_TONEAREST); t2 = edge + size*anchor; return checkedFloor(t2 + 0.5)
> checkedFloor: 非有限 / int64 範囲外            -> overflow_error("UI coordinate exceeds int64")
> checkedI32  : int32 範囲外 または |v| > 2^24   -> overflow_error("exact float32 integer domain")
> ```
>
> **手がかり**: `#pragma STDC FENV_ACCESS ON` がMSVC以外で立っている([`types.cpp` 内](../../src/core/ui/types.cpp#L10)、[`layout.cpp` 内](../../src/core/ui/layout.cpp#L10))のは、コンパイラに浮動小数点環境を勝手に仮定させないためです。`checkedFloor()` の上限判定が `value >= -static_cast<double>(int64_min)` という書き方なのは、`int64_max` をdoubleで厳密に表せないからです([その判定](../../src/core/ui/types.cpp#L55))。レイアウト側の `edge()`([`edge()`](../../src/core/ui/layout.cpp#L24))はint32範囲しか見ません — 2^24 の検査はpxへ変換する [`uiToPx()`](../../src/core/ui/types.cpp#L101) 側の責任です。テストは [`UI binary64 half-up vectors and viewport edge rounding are normative`](../../test/ui_foundation_test.cpp#L61)「UI binary64 half-up vectors and viewport edge rounding are normative」。
>
> **不変条件**: 丸めは `floor(x + 0.5)` 固定(`round` 系へ置き換えない)。丸めモードは `FE_TONEAREST`。GPUへ出る座標は `|value| <= 2^24`。

### render feature

[`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L2695) はfeature JSONを順番に読み、render target、buffer、pass、compute taskを追加し、限定的なoverrideを適用します。名前衝突、曖昧anchor、未知override fieldは即時エラーです。shader defineも重複排除して集約します。

runtime側の入口は [`registerRenderGraphVariantFamilyFromJsonData()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1141) です。[`loadRenderGraphVariantsFromConfigData()`](../../src/core/vkcore/renderer_config.cpp#L434) が `ProjectBasicConfig::renderingConfigJson()` の文字列と起動ターゲットの実extentを渡し、flat（OpenXR有効時は `#xr` も）とpreviewを **1回の登録トランザクション** として受け取ります。

起動主体が feature を足す公開面は `pelican_player --feature-overlay <uri>` です。文書の厳密な
v1 検証と加算は [`applyRenderFeatureOverlays()`](../../src/project/renderfeatureoverlay.cpp#L119) が担当し、
renderer は [`loadRenderGraphVariantsFromConfigWithStartupFeatureOverlays()`](../../src/core/vkcore/renderer_config.cpp#L402)
という**別名の startup 経路**からだけ呼びます。overlay が 0 件なら従来関数へそのまま委譲し、loader を
呼びません。

構造上の境界は次のとおりです。

```text
project.json -> ProjectEnvelope / ProjectBasicConfig
             -> renderingConfigJson()
             -> loadRenderGraphVariantsFromConfigData()       # 出荷グラフの基準

player CLI / tool -> EngineLaunchConfig.render_feature_overlays
                  -> ...WithStartupFeatureOverlays()
                  -> applyRenderFeatureOverlays()
                  -> loadRenderGraphVariantsFromConfigData()   # 明示した起動だけ
```

`ProjectEnvelope` と `ProjectBasicConfig` には overlay reference を格納する field がなく、project parser の
入力 schema にも overlay key はありません。一方、reference を持てる `EngineLaunchConfig` は player の
CLI が project 読み込みとは別に作る process-local module です。この型と名前付き入口の分離により、
project 読み込みから overlay を発見・保存・再読込するデータ経路そのものが存在しません。

sceneと違い、feature合成は「純粋層で済ませてからruntimeへ渡す」形ではありません。上の登録関数の内側でGPU非依存の [`runRenderCompilerProgram()`](../../src/core/renderingpass/rendercompilerprogram.cpp#L218) が走り、その中の [`resolveRenderPipeline()`](../../src/project/renderpipeline.cpp#L1027) が `composeRenderFeatureConfig()` を呼びます。二段であること自体は同じで、境界が呼び出し順ではなく「GPUに触る前／後」で引かれている、という違いです。詳細は[第6章](06_rendering_vulkan_shader.md)。

### material / surface

[`parseMaterialFormatJson()`](../../src/project/materialformat.cpp#L744) はmaterial v1とoptional surface catalogの整合性を検証します。[`parseSurfaceFormat()`](../../src/project/surfaceformat.cpp#L1239) はfront matter風の宣言部とshader codeを分け、parameter/texture/screen inputを構造化します。

これらはruntime接続済みです（WP116〜117, 122）。純粋層はparseと [`lowerMaterial()`](../../src/project/materiallowering.hpp#L140)（OpenPBR写像は [`openpbrmapping.hpp`](../../src/project/openpbrmapping.hpp)）まで、core層はsurface compile（[`surfacecompiler.cpp`](../../src/core/shader/surfacecompiler.cpp)）とvalues SSBO登録（[`registerReloadableMaterialValuesFile()`](../../src/core/material/materialcontainer.hpp#L345)）を担います。hot reloadは `MaterialValuesReloadHandler` / `TextureReloadHandler`（[src/core/material/](../../src/core/material)）です。

material bindingは **USD pathキー**を持ちます（[`materialformat.hpp`](../../src/project/materialformat.hpp#L45) の `usd_path`、[`modeltemplate.cpp`](../../src/core/model/modeltemplate.cpp#L99) で重複/未解決を検証）。これはU-USDレーン（WP119/124）の観測境界です。

> 🧩 **難所 — vec3の後ろ4 byte**([`std140SizeAlignment()`](../../src/project/materiallowering.cpp#L32) / [`makeSurfaceStd140Layout()`](../../src/project/materiallowering.cpp#L428))
>
> **何をする所か**: surfaceのparameter宣言順にstd140(GLSLのuniform block標準レイアウト規則)のオフセットを割り付け、values領域の総サイズを決めます。
>
> **素朴に読むと**: `vec3` に `{12, 16}` を返すのが「std140ではvec3は16バイト」という一般論と食い違って見えます。std140の規則は「開始位置は16の倍数、占有は12」で、直後に `float` が来ればその4バイトへ滑り込みます。だからパッカーは2つの値を別々の目的に使い分けます — `alignUp(offset, alignment)` で次の**開始位置**を決め、`offset += size` で**自分が食う量**だけ進めます([その2行](../../src/project/materiallowering.cpp#L369))。`size` を16にすると、規則どおりに計算するGLSLコンパイラ側とCPU側で4バイトずれ、vec3より後ろの**全パラメータ**が1つずつずれて読まれます。バリデーションレイヤもreflectionも検出できず、色が微妙に違うだけの症状になるのが最悪の性質です。だから12と16を分けて持つことが必須で、alignmentは次の要素の開始位置、sizeは自分が食う量、と役割が違います。最後に構造体全体を `layout.alignment`(既定16、[`materiallowering.hpp` 内](../../src/project/materiallowering.hpp#L29))へ切り上げるのも規則どおりです。
>
> **骨子**:
> ```text
> float/int -> {4, 4}   vec2 -> {8, 8}   vec3 -> {12, 16}   vec4/color -> {16, 16}
> offset = alignUp(offset, alignment)     # 開始位置(vec3 は 16 の倍数へ)
> members += {name, type, offset, size, alignment}
> offset += size                          # 占有量(vec3 は 12 なので直後の float が入る)
> layout.size = alignUp(offset, 16)
> ```
>
> **手がかり**: 割り付けは**宣言順**で、名前順でもサイズ順でもありません(テスト名がそのまま [`surface values use declaration-order std140 offsets`](../../test/materiallowering_test.cpp#L46)「surface values use declaration-order std140 offsets」)。上限 `materialCustomValueCapacity = 256`([`materiallowering.hpp` 内](../../src/project/materiallowering.hpp#L16))はGPU側の `MaterialGpuData::custom_values` と同じ値で、[`material.hpp` 内](../../src/core/material/material.hpp#L110) の `static_assert` が両者を結び付けています。超過時に `surface.params.back()` の名前を添えてthrowするのは、どのパラメータで溢れたかを作者に返すためですが、**溢れた位置ではなく末尾の名前**である点は割り切りです([その throw](../../src/project/materiallowering.cpp#L442))。
>
> **不変条件**: `size` と `alignment` を1つの値へ畳まない。割り付けは宣言順。構造体全体のサイズは16バイト境界へ切り上げる。

> 🧩 **難所 — 逸脱がforwardへ落とす**([`openPbrEligibility()`](../../src/project/materiallowering.cpp#L217) / [`automaticRoute()`](../../src/project/materiallowering.cpp#L252))
>
> **何をする所か**: `render_path: automatic` のmaterialをdeferred(G-buffer経由)へ流してよいかを判定し、駄目ならforward側のrouteを返します。
>
> **素朴に読むと**: `coat_weight == 0`、`specular_weight == 1`、`specular_ior == 1.5` …と、値が**規定の既定値と一致するか**を `1e-6` の許容で調べる条件が並びます。範囲検査でも能力検査でもないので、何を守っているのかコードからは読めません。理由はG-bufferが運べるのがOpenPBRのごく一部(base color / roughness / metalness / normal)だけで、それ以外のパラメータは「既定値のまま」でなければG-buffer経由で再現できないことです。だから条件は能力の判定ではなく「**既定から動いていないことの確認**」で、1つでも動いていればforwardへ回すしかありません。比較対象がmaterialの上書き値だけでなくsurface側の `default_value` にもフォールバックする([`effectiveValue()` 内](../../src/project/materiallowering.cpp#L190))のはそのためです。テクスチャ側も同じ理由で、値が既定でも `*_map` が付いていればピクセルごとに動くので不適格です([texture 側の条件](../../src/project/materiallowering.cpp#L225) の4つ)。`blend` がopaqueでない、`screen_input` を使う、の2つだけは深度・順序の理由で別枠になっています。
>
> **骨子**:
> ```text
> blend != opaque              -> blend_requires_forward
> screen_inputs あり           -> screen_input_requires_forward
> coat_weight != 0             -> coat_weight_nonzero
> base_diffuse_roughness != 0  -> diffuse_roughness_nonzero
> specular_weight != 1         -> custom_specular_weight
> specular_color != (1,1,1)    -> custom_specular_color
> specular_ior != 1.5          -> custom_specular_ior
> *_map のどれかが付く         -> custom_<map名>
> すべて既定 -> {compatible, openpbr_base_v1} -> automaticRoute が deferred_geometry
> ```
>
> **手がかり**: `reason` 文字列(`coat_weight_nonzero` など)がそのまま診断に出るので、G-bufferにチャネルを足したときは**この一覧から対応する条件を消す**、という対応関係になっています。逆に条件を足し忘れたままG-bufferを変えると、deferredに流れたmaterialが静かに違う見た目になります。不適格になったOpenPBR materialが実際にどこへ落ちるかは `automaticRoute()` の続きで、engine提供のOpenPBR surfaceはいずれも `pelican_lighting_v1` を実装しているため `forward_opaque` / `automatic_custom_lighting` になります([route の決定](../../src/project/materiallowering.cpp#L256))。判定の入口は [`evaluateDeferredEligibility()`](../../src/project/materiallowering.cpp#L406) で、OpenPBR surface以外は `standard_pbr_v1` の別条件を通ります。テストは [`materiallowering_test.cpp` 内](../../test/materiallowering_test.cpp#L240)「OpenPBR base subset routes deferred while extended lobes stay forward」。
>
> **不変条件**: 既定値の一覧とG-bufferのチャネル構成は対で動かす。`reason` 文字列は診断の一部なので条件と1対1に保つ。判定は既定からの逸脱の有無であって、値の妥当性検査ではない。

### import manifest / import rules

[`parseImportManifestJson()`](../../src/project/importmanifest.cpp#L244) はDCC納品物のtool/source/outputとSHA-256、対応schemaを検証します。runtime playerではなく、[`pelican_cli import`](../../src/devcli/importcommand.cpp#L351) が主利用者です。加えて [`importrules.hpp`](../../src/project/importrules.hpp) がmatch/recipe/defaultsの3層優先を持つルールベースimport（`pelican_cli import --rules`、[importcommand.cpp](../../src/devcli/importcommand.cpp#L355)）を提供します。

> 🧩 **難所 — `*` は `/` を越えない**([`segmentMatches()`](../../src/project/importrules.cpp#L155) / [`importGlobMatches()`](../../src/project/importrules.cpp#L258))
>
> **何をする所か**: importルールのglobパターンを、プロジェクト相対パスへ突き合わせます。
>
> **素朴に読むと**: 同じ形のDP(動的計画法 — 部分問題の可否を表へ埋めながら前進させる解き方)が2つ並び、片方は文字を、片方はパスセグメントを舐めます。`*` と `**` の遷移式は字面上まったく同じ2本(パターンを進める = 0個マッチ、値を1つ進めてパターンを留める = 1個以上)なので、両者の違いがどこから生まれるのかコードから読み取れません。違いは遷移式ではなく**何を1単位とするDPか**にあります。`*` のDPは `splitPath()` 済みの1セグメント内だけを走るので、区切り文字を消費する術が構造的にありません = `*` は `/` を越えられません。`**` のDPはセグメントの列を走るので、同じ2本の遷移が「0個以上のディレクトリ階層」になります。つまりglobの意味論が正規表現の特別扱いではなく、DPを2段に分けたこと自体で表現されています。どちらもpush型(`dp[i][j]` が真のときだけ前進し、偽なら `continue`)なので、バックトラック実装にありがちな `a*a*a*b` の指数爆発が起きません — 外部から与えられるルール文字列を舐めるので、これは性能ではなく安全性の選択です。片方だけ「便利だから」と `*` に `/` を跨がせると、ルールの優先順位(最初に一致したruleが勝つ、[rule 層の走査](../../src/project/importrules.cpp#L285))が変わり、import先が黙って入れ替わります。
>
> **骨子**:
> ```text
> importGlobMatches: 単位 = パスセグメント
>   patterns[i] == "**" -> dp[i+1][j] = true;  dp[i][j+1] = true   # 0 階層 / 1 階層以上
>   それ以外            -> segmentMatches(patterns[i], values[j]) なら dp[i+1][j+1] = true
> segmentMatches:    単位 = 1 文字(1 セグメント内)
>   pattern[i] == '*'   -> dp[i+1][j] = true;  dp[i][j+1] = true   # 0 文字 / 1 文字以上
>   pattern[i] == '?'   -> dp[i+1][j+1] = true
>   それ以外            -> pattern[i] == value[j] なら dp[i+1][j+1] = true
> ```
>
> **手がかり**: `splitPath()`([`splitPath()`](../../src/project/importrules.cpp#L141))は空セグメントも残す(`a//b` は3要素)ので、`*` が空文字列にも一致することと合わせて挙動が決まります。この2段のDPを使うのは3層のうちrule層だけで、defaults層は拡張子の完全一致です([defaults 層](../../src/project/importrules.cpp#L291))。テストは [`import globstar spans zero or more complete path segments`](../../test/importrules_test.cpp#L49)「import globstar spans zero or more complete path segments」と [`import rules evaluate explicit, defaults, builtin, and unmatched precedence`](../../test/importrules_test.cpp#L26)(優先順位)。
>
> **不変条件**: `*` は1セグメント内で閉じる。DPはpush型で、パターン長×値長の表を超えて探索しない。ルールは最初に一致したものが勝つ。

### JSON-RPC

[`parseJsonRpcRequest()`](../../src/project/jsonrpc.cpp#L148) はtransportに依存せずJSON-RPC 2.0 envelopeを検証します。batchとnotificationは現状不許可です。実メソッドdispatchだけがcore側にあります。

## 3.10 データロードを変更するときのチェック

1. schema/version gateを純粋層に置いたか。
2. pathを文字列連結せず`PathResolver`へ通したか。
3. project由来の絶対pathやroot escapeを許していないか。
4. parse成功前に旧runtime状態を壊していないか。
5. 型名/ID/名前bindingの寿命がscene遷移後も安全か。
6. valid/invalid fixtureとruntime binding testの両方を追加したか。
7. 形式パーサ実装とruntime接続済み範囲を混同していないか。
8. componentを増やすなら、§3.11 のcodec五点セットを揃えたか。

> 🧩 **難所 — 舐め直しで閉じる窓**([`Impl::reconcile()`](../../src/core/watch/filewatcher.cpp#L472) / [`Impl::scan()`](../../src/core/watch/filewatcher.cpp#L374))
>
> **何をする所か**: 監視対象のstoreを一巡して各ファイルの安定digestを取り、前回のinventoryとlive digestに照らしてreloadを積みます。
>
> **素朴に読むと**: `for (;;)` の先頭で `notification_generation` を控え、scan完了後に一致していなければ**もう一周まるごと**やり直す形が、終わらないループに見えます。ここまでするのは、native watchを張る `armAll()` と最初のscanの間、およびscan実行中に届いた変更が、ディスクinventoryの差分だけでは検出できないからです(スキャン済みの位置が後から書き変わります)。generationを前後で比べるのは「スキャン中に通知が来たか」を知る唯一の手段で、来ていればハッシュを全部取り直します — 部分再スキャンにしないのは、どのファイルが変わったかを通知が持っていない(overflowもある)からです。`scan()` が [`readStableContentDigest()`](../../src/core/watch/contentdigest.cpp#L47) へ渡すcancelクロージャ([その受け渡し](../../src/core/watch/filewatcher.cpp#L424))は別の理由です。プロジェクト全体のSHA-256は数秒かかりうるのに、その間にreload gateが閉じたりepochが進んだりすれば結果は捨てるしかありません。捕まえた `epoch` と現在を比べて途中で降りることで、gateを閉じる操作(replay開始など)がwatcherスレッドの完了待ちでブロックしなくなります。ループが止まるのは、`stopping` / gate無効 / epoch不一致で `false` を返す出口([早期return](../../src/core/watch/filewatcher.cpp#L364))と、generation一致で `true` を返す出口([正常終了](../../src/core/watch/filewatcher.cpp#L384))の2つがあるためです。
>
> **骨子**:
> ```text
> for (;;):
>     generation = notification_generation      # scan の前に控える
>     current = scan(epoch)                     # 各ファイルの安定 digest
>     stopping / gate 無効 / epoch 違い -> return false
>     observe() が queue_reload を返したものを queue へ、消えたものは removed
>     inventory = current
>     notification_generation == generation -> dirty/overflow を落として return true
>     # 違えば scan 中に通知が来ている: もう一周(部分ではなく全体)
> ```
>
> **手がかり**: 呼び出し側は「arm → reconcile」の順を守る必要があり、[その呼び出し箇所](../../src/core/watch/filewatcher.cpp#L447) にコメントで `Required transition order: arm, reconcile while still in polling state, then publish watching` と明記されています。`readStableContentDigest()` は書き込み途中のファイルを読むと `retry` を返し、`scan()` は25ms間隔で最大20回まで粘ります([リトライの設定](../../src/core/watch/filewatcher.cpp#L329))。テストは [`filewatcher_test.cpp` 内](../../test/filewatcher_test.cpp#L346)「disabled changes resume through arm-scan barrier exactly once」と [polling fallback 側](../../test/filewatcher_test.cpp#L217)(gate cancellation)。
>
> **不変条件**: generationはscanの**前**に控える。scan中に通知が来たら部分ではなく全体を取り直す。epochが動いたら結果を採用せず `false` で降りる。

> 🧩 **難所 — 自分の書き込みを一度だけ**([`ContentDigestState::observe()`](../../src/core/watch/contentdigest.cpp#L154) / [`registerSelfWrite()`](../../src/core/watch/contentdigest.cpp#L146))
>
> **何をする所か**: watcherが見つけたdigestを「外部の変更」と「エンジン自身の保存」へ振り分け、reloadを積むかどうかを返します。
>
> **素朴に読むと**: digestが一致していてepochも一致しているのにreloadを積む経路があるのが引っかかります。エンジン自身が保存したファイルは、watcherから見れば外部変更と区別できません。そこで保存側が「このdigestが来たら自分の書き込みだ」というトークン([`SelfWriteToken`](../../src/core/watch/contentdigest.hpp#L42))を置きます。条件は4つあり、digest一致 / epoch一致 / `runtime_applied` / `live_digest` 一致が揃ったときだけ `consumed_self_write` になります。`match` と `may_consume` を先に計算したうえで `state.self_write.reset()` を分岐の**前**に無条件で実行するのが要点で、トークンは1回の観測で必ず消えます(消費されても、外れても)。消えなければ、一致し続ける限り永久にリロードが抑止されます。`runtime_applied` がfalseのときに消費しないのは「ディスクには書けたが、実行中の状態への適用は失敗した」場合で、そのとき `registerSelfWrite()` は `live_digest` を更新しない([その代入](../../src/core/watch/contentdigest.cpp#L150))ので、下の `live_digest == digest` の近道にも掛からず `queue_reload` になります — ファイルとランタイムが食い違ったまま静かに固定されるのを防ぐ、あえての積み直しです。epoch違いを `live_digest` 一致の近道へ落とさない([そのコメント](../../src/core/watch/contentdigest.cpp#L168))のはコメントが規範で、gateが閉じて開き直った後(= 世界が入れ替わったかもしれない後)は古いtransactionの抑止を効かせない、という決めです。
>
> **骨子**:
> ```text
> observed_digest = digest; pending_digest = digest
> self_write あり:
>     match       = expected_digest == digest && epoch 一致
>     may_consume = match && runtime_applied && live_digest == digest
>     self_write.reset()                 # 分岐の前に無条件で消す
>     may_consume -> pending を落として consumed_self_write
>     !match      -> queue_reload        # live_digest 一致の近道へ落とさない
> live_digest == digest -> unchanged
> それ以外              -> queue_reload
> ```
>
> **手がかり**: `observe()` が比較するのは前回の**ディスクinventory**ではなく `live_digest`(= ランタイムへ実際に適用済みの内容)です。[`reconcile()` 内](../../src/core/watch/filewatcher.cpp#L491) のコメントが規範で、「一度失敗した候補は、バイトが同じでも次の保存/reconcileで再び対象になる」ためにこの比較になっています。トークンを置くのは [`FileWatcher::registerSelfWrite()`](../../src/core/watch/filewatcher.cpp#L688) 経由で、`runtime_apply_succeeded` をそのまま持ち回ります。テストは [`filewatcher_test.cpp` 内](../../test/filewatcher_test.cpp#L102)「ContentDigest streams bytes and separates observed live pending and self-write」と [batch commit の検査](../../test/filewatcher_test.cpp#L151)。
>
> **不変条件**: トークンは1回の観測で必ず消える。epochが違うトークンは抑止に使わない。`runtime_applied` がfalseの保存はreloadを積み直す。

## 3.11 Component codec（authored ↔ canonical ↔ runtime） ✅実装済み

componentのJSONをどう受理し、どう書き戻し、どうruntimeへ当てるかの**正は [`componentcodec.hpp`](../../src/core/loader/componentcodec.hpp#L226) に移りました**（WP151）。scene loaderやエディタは、それぞれ独自にJSONを解釈するのではなく、このテーブルを引きます。

```cpp
struct ComponentCodec {
    using DecodeAuthored = ResolutionResult<ComponentCodecValue> (*)(
        const nlohmann::json &);
    using EncodeCanonical = nlohmann::ordered_json (*)(const ComponentCodecValue &);
    using Schema = const SchemaNode &(*)();
    using RuntimeApply = void (*)(const ComponentCodecValue &, void *);
    using RuntimeProject = ComponentCodecValue (*)(const void *);
    using EditorPrepare = ComponentEditorSwapToken (*)(
        void *ecs, std::uint32_t entity_index,
        std::uint32_t entity_generation, const ComponentCodecValue &);
    using EditorSwap = void (*)(ComponentEditorSwapToken &,
                                EditorCodecSwapPhase) noexcept;

    std::string_view name;
    ComponentCodecRuntimeKind runtime_kind;
    EditorProjectionPolicy editor_policy;
    ...
};
```

この五つ（decode / encode / schema / apply / project）が**全codecに必須**です。値の運搬は `ComponentCodecValue = std::any` です。`decode_authored` は例外ではなく [`ResolutionResult<ComponentCodecValue>`](../../src/core/loader/resolutionresult.hpp#L55) を返し、登録テーブルでは `decodeResult<decodeXxx>` が生の decode 関数を包みます。`schema` が返すのは平坦な `StructFieldSchema` 列ではなく**再帰記述子** [`const SchemaNode &`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L155) で、平坦な列が要るときだけ [`fieldSchema()`](../../src/core/loader/componentcodec.hpp#L255) が作り直します（後述）。

`editor_prepare` / `editor_swap` の2本は必須ではありません（transform / simplemodelview / camera / light / collider は `nullptr`）。[`prepareEditorSwap()`](../../src/core/loader/componentcodec.cpp#L1540) は `editor_policy` が `EcsCodec` で、かつ両方が非nullのときだけ通し、そうでなければ `logic_error` です。[`EditorProjectionPolicy`](../../src/core/loader/componentcodec.hpp#L33) に既定値が無いのは意図で、ヘッダのコメントどおり「codecを増やすなら、live projectionを所有するruntime participantを選ぶ」ことを強制します。

`runtime_apply` と `runtime_project` がどの型を指すかは [`ComponentCodecRuntimeKind`](../../src/core/loader/componentcodec.hpp#L23) が決めます。ヘッダのコメントが規範です — transform は `TransformCodecTarget`、その他のECS codecはそのcomponent自身、camera/light は `CameraCodecData` / `LightCodecData`、collider は `ColliderComponent` です。

この中でtransformだけが、componentを直接指さずに [`TransformCodecTarget`](../../src/core/loader/componentcodec.hpp#L68)（`world` / `local` / `parent_world` の三つのポインタ）を経由します。authoredなのは **local TRS** ですが、ランタイムで全員が読むのは world の `TransformComponent` で、`LocalTransformComponent` は親を持つobjectにしか付かない、という食い違いがあるためです（[`editorruntimefactory.cpp` 内](../../src/core/communication/editorruntimefactory.cpp#L889) の `tryComponent<LocalTransformComponent>` はrootではnullになります）。そのため `runtime_apply` は local へ書いたうえで world も自分で合成し直し（[`applyTransform()`](../../src/core/loader/componentcodec.cpp#L399)）、`runtime_project` は local があればそれを返し、無ければ `parent_world` を使って world から逆算します（[`projectTransform()`](../../src/core/loader/componentcodec.cpp#L424)）。`parent_world` は両方向の変換に必要な係数で、これが無いと親の下のobjectについてlocalとworldを行き来できません。

登録テーブルは8行です（[`componentcodec.cpp` 内](../../src/core/loader/componentcodec.cpp#L1365)）。ただし**見えるのは常に8行とは限りません**（最後の行だけは後述のgate付きです）。

| codec名 | `runtime_kind` | `editor_policy` | editor swap |
|---|---|---|---|
| `transform` | `Ecs` | `TransformClosure` | なし |
| `simplemodelview` | `Ecs` | `RendererModel` | なし |
| `camera` | `Camera` | `Camera` | なし |
| `light` | `Light` | `Light` | なし |
| `collider` | `Collider` | `Collider` | なし |
| `animation` | `Ecs` | `EcsCodec` | あり |
| `sprite_view` | `Ecs` | `EcsCodec` | あり |
| `pelican_ray_instance` | `Ecs` | `EcsCodec` | あり（gate付き） |

transformだけは専用の投影関数 [`projectTransformRuntimeJson()`](../../src/core/loader/componentcodec.hpp#L311) を持ちます。

> **設計決定:** transformの「authored local TRS」と「表示専用のworld投影」を混同しない、という要件をコメントで明文化したうえで別関数に分けています。エディタが表示している値と、保存される値が同じものだと誤解しないための境界です。

未登録の名前を問い合わせても例外にはならず、[`componentCodecQueryMetadata()`](../../src/core/loader/componentcodec.hpp#L283) が [`ComponentCodecState::Missing`](../../src/core/loader/componentcodec.hpp#L48) を返します。エディタは「知らないcomponentは編集不可として表示する」ことができ、未知componentを含むsceneを開けなくなることがありません。

テストは [`test/componentcodec_test.cpp`](../../test/componentcodec_test.cpp)、fixtureは [`test/fixtures/component_codec/`](../../test/fixtures/component_codec) の `valid.json` / `invalid.json` です。

### `pelican_ray_instance` は二重にgateされる ✅実装済み

8番目のcodecだけは、**テーブルに載っていても引けるとは限りません**。[`componentCodecs()`](../../src/core/loader/componentcodec.cpp#L1565) は [`rayInstanceCodecEnabled()`](../../src/core/loader/componentcodec.cpp#L1558) が偽なら `codecs` の**先頭7要素だけ**を span として返します（[その `first()`](../../src/core/loader/componentcodec.cpp#L1570)）。判定は [`RayLayoutRuntimeState::builtinRayInstanceEnabled()`](../../src/core/shader/raygrouplayout.hpp#L180) で、`configure()` 済みかつモードが `LayoutV2` のときだけ真です。モードを固定するのは起動の1点だけで、組み込みComponent登録より前です（§2.1 の 7番）。同じ述語がECS側の登録も分けるので（[`predefined.cpp` 内](../../src/core/ecs/predefined.cpp#L38)）、「codecはあるがComponentが無い」状態は作られません。テストからは [`findComponentCodecIncludingDisabled()`](../../src/core/loader/componentcodec.hpp#L287) でgateを迂回できます。

WP382dから[`decodeRayInstance()`](../../src/core/loader/componentcodec.cpp#L1103)は`overrides`を可変長の行として受理します。省略した値は継承し、明示した0や空のflagsは上書きとして保持します。実行時のcatalogとAS planをscene/editorの候補に含め、公開・rollbackで一緒に切り替えます。

previewでは共通decode入口の[`componentDecodeSurfaceViolation()`](../../src/core/loader/componentcodec.cpp#L1475)が`ray_instance_overrides_unsupported@1`と`surface=preview`を返します。共通codecにはWeb用surface制約もありますが、別repoのWebパーサへの接続はWeb側の担当です。旧`overrides_not_enabled`分岐とそのlegacy branch IDは削除しました。

そのほかの受理規則も全て名前付きの legacy branch で拒否します: `instance_flags` は文字列配列のみ、重複flagは拒否、未知flag名は `ray_as_unknown_flag`、`force_opaque` と `force_no_opaque` の同時指定は `ray_as_conflicting_instance_flags`（[その検査](../../src/core/loader/componentcodec.cpp#L1074)）、`mask` は 0..255、`custom_index` は `rayCustomIndexMaximum` まで、そして**scalar 3項目もoverridesも省略された宣言**は `ray_instance_empty` です（[その検査](../../src/core/loader/componentcodec.cpp#L1187)）。

### 再帰schema `SchemaNode` と `$list` / `$row` ✅実装済み

codecの `schema` が返す [`SchemaNode`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L155) は、§5.14 の consteval なフィールド宣言DSLを**実行時の所有値**へ起こしたものです。v2の平坦な `StructFieldSchema` 列と違い、子を持てます。

| 要素 | 意味 |
|---|---|
| `tag` | [`SchemaNodeTag`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L79) = `Struct` / `Leaf` / `Flags` / `List` |
| `presence` | `Required` / `Defaulted` / `OptionalOverride` の3値（[`StructFieldPresence`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L67)） |
| `leaf_type` / `range` / `unit` | 平坦な記述子と同じ内容 |
| `default_json` | `Defaulted` のときだけ入る**正準JSONテキスト**（`nlohmann::json` ではなく文字列） |
| `vocabulary` | enum / Flags の名前と値（符号の有無を `signed_value` で持つ） |
| `children` | `Struct` なら各フィールド、`List` なら行の型のschemaが**1つだけ** |

平坦な列からの持ち上げは [`SchemaNode::fromFlat()`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L230)（v2の記録をそのまま包む。子は作りません）、宣言DSLからの構築は [`materializeSchemaNode()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L944) で、`List` のときだけ行型の schema へ**再帰**します（[その1行](../../src/core/userpublic/details/schema/structfieldjson.hpp)）。[`recursive()`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L160) は `Flags` か `List` が1つでもあれば真で、これが §2.11 のv3入口の選択に使われます。fingerprintは [`schemaNodeJson()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L983) の `dump()` そのものです（[`schemaNodeFingerprint()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L1012)）。

`List` のJSON表現は**オブジェクトのコンテナ**で、行のidを持つ封筒が付きます。

```json
{"waypoints": {
  "$list": {"v": 1, "next_id": 3},
  "rows": [
    {"$row": {"id": 1, "v": 1}, "pos": [0,0,0]},
    {"$row": {"id": 2, "v": 1, "hint": {"label": "goal"}}, "pos": [1,0,0]}]}}
```

decode（[list の枝](../../src/core/userpublic/details/schema/structfieldjson.hpp#L428)）が課す規則は次で、いずれも名前付きの `StructFieldErrorCode` です。

- コンテナが object でなければ `ListContainerMissing`。`$list` と `rows` 以外のキーは `UnknownField`。
- `$list` は `v` と `next_id` の2つだけ（他は `UnknownField`）。`v != 1` は `RowEnvelopeUnsupported`。
- 各行は `$row` を必須とし、その中身は `id` / `v` / `hint` の3つだけ。`id == 0` は `OutOfRange`、`v != 1` は `RowEnvelopeUnsupported`、`hint` は object のみで、**そのまま文字列として**保持されます（`hint_json`）。
- `id` の重複は `DuplicateRowId`。最後に `next_id` が**全行のidより大きい**ことを確認し、破れば `OutOfRange`（`$list.next_id must exceed every row id`）。
- 行の本体は `$row` を除いた残りを行型のschemaで [`decodeStructObject()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L711) へ渡します。エラーのpathは `field.rows[2].member` の形になります。

encode（[list の枝](../../src/core/userpublic/details/schema/structfieldjson.hpp#L643)）は逆向きに `$list` → `rows` の順で `ordered_json` を組み、`hint_json` は保存時のテキストを parse し直して戻します。`List` フィールドの既定値は常に**空**です（[`setStructDefault()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L209) の `Member{}`。宣言側も [`EmptyListDefault`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L589) の1引数形しか受け付けません）。行の型がさらに `List` を持つことは [`validRecursivePolicyField()`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L804) が拒否するので、入れ子は**1段だけ**です。

> **設計決定:** 行のidを配列添字ではなく封筒で持つのは、行を並べ替えても挿入しても外部参照が切れないようにするためです（§3.12 の `AuthoringObjectId` と同じ動機）。`next_id` を単調に保つ検査がその保証の要で、ここを緩めると同じidが再利用され、参照が黙って別の行を指します。

なお `ComponentCodec::fieldSchema()` が返す平坦な列は **rootの直下の子だけ**を写します（[その実装](../../src/core/loader/componentcodec.cpp#L1511)）。`List` の行schemaはそこに現れないので、平坦な列を消費する側（schema駆動のinspectorなど）は再帰schemaを完全には表現できません。

## 3.12 AuthoringSceneDocument ✅実装済み

§3.2 で触れたsceneのdocument化の中身です。宣言は [`authoringscenedocument.hpp`](../../src/core/loader/authoringscenedocument.hpp#L90) です。

### load とidentity

[`AuthoringSceneDocument::load(scene_v1_bytes, revision, first_authoring_object_id)`](../../src/core/loader/authoringscenedocument.hpp#L109) が、`SceneRevision` とセッション内で安定な [`AuthoringObjectId`](../../src/core/loader/authoringscenedocument.hpp#L26) を割り当てます。`AuthoringObjectId` は名前でも配列添字でもないため、objectをrenameしても並べ替えても、エディタ側の参照は切れません。

### authoring query と raw authority

三層のauthoring viewは [`AuthoringSceneAuthority::query()`](../../src/core/loader/authoringsceneauthority.hpp#L42) だけが取得します。`AuthoringSceneDocument` 自身からraw JSONやquery viewを取り出すpublic APIはなく、保存・journal・編集transactionのauthoring authorityと [`ResolvedSceneResolver`](../../src/core/loader/resolvedscene.hpp#L331) だけが型付き [`AuthoringSceneRawView`](../../src/core/loader/authoringsceneauthority.hpp#L16) を取得できます。Camera / DLL reload / editor runtime queryなどの読者はauthority headerへ依存せず、`ResolvedScene` の `source_json_exact` と `effective_json` を用途に応じて読みます。

| 型 | 内容 |
|---|---|
| [`AuthoringSceneView`](../../src/core/loader/authoringscenedocument.hpp#L55) | scene ID、authored JSON、object列 |
| [`AuthoringObjectView`](../../src/core/loader/authoringscenedocument.hpp#L43) | `AuthoringObjectId`、宣言順、name/parent、runtime entity ID、component列 |
| [`AuthoringComponentView`](../../src/core/loader/authoringscenedocument.hpp#L35) | 宣言順、authored JSON、`ComponentCodecQueryMetadata` |

componentごとに §3.11 のcodec metadataが付くので、「宣言としては存在するが、このビルドでは編集できない」状態をそのまま表現できます。

### stage と構造編集

改訂の作り方は二種類あり、どちらもauthoring authorityへ隔離されています。

- [`AuthoringSceneAuthority::stage(raw_document, revision)`](../../src/core/loader/authoringsceneauthority.hpp#L52) — **オブジェクト宣言のidentityを固定したまま**の未公開改訂。component配列やparentエッジは変えられますが、object宣言のidentityは動きません。
- [`AuthoringSceneAuthority::structuralStage()`](../../src/core/loader/authoringsceneauthority.hpp#L58) → [`AuthoringSceneDocumentStage`](../../src/core/loader/authoringscenedocument.hpp#L141) — object配列そのものを触る唯一の経路。`insertObject` / `removeObject` / `restoreObject` / `renameObject` / `reorderObject` を持ち、`finish(revision) &&` で確定します。変更種別は [`AuthoringStructuralChangeKind`](../../src/core/loader/authoringscenedocument.hpp#L72) の `Insert` / `Remove` / `Restore` / `Rename` / `Reorder` です。

実際にはどちらを使うかを呼び出し側が選ぶことはありません。[`EditorProjectionTransaction::commit()`](../../src/core/loader/editorprojectiontransaction.cpp#L893) がコマンド列を見てauthority経由で振り分けます（[その分岐](../../src/core/loader/editorprojectiontransaction.cpp#L925)）— `structural_apply` を持つコマンド（`makeInsertObjectCommand` / `makeRemoveObjectCommand` / `makeRestoreObjectCommand` / `makeRenameObjectCommand` / `makeReorderObjectCommand`）が1つでも混ざっていれば `structuralStage()` 経路へ、component値の書き換えやreparent（`makeSetComponentValueCommand` / `makeReparentCommand`）だけなら `stage()` 経路へ進みます。つまり「objectを増やす・消す・戻す・改名する・並べ替える」RPCが `structuralStage()`、それ以外の編集RPCが `stage()` に対応します。

> **設計決定:** `removeObject()` は破棄ではなく [`AuthoringObjectClosure`](../../src/core/loader/authoringscenedocument.hpp#L63) を**返します**。ここでのclosureは「取り除いた宣言を元の場所へそのまま戻すための記録一式」という意味で、関数のクロージャでもグラフの閉包でもありません。中身は `authoring_object_id` / `scene_id` / `declaration_index` / `previous_object_id` / `next_object_id` / `authored_json` の六つで（[`AuthoringObjectClosure`](../../src/core/loader/authoringscenedocument.hpp#L63)）、これだけあれば **同じ `AuthoringObjectId` を、消したときと同じ宣言区間（前後の兄弟の間）へ、JSONを一字も落とさずに戻せます** — 「無損失」はこの意味です。ヘッダのコメント通り、object配列の変更をこれらの操作だけに限ることで、metadataのidentity列とauthored JSONの並びがずれないことを型で保証しています。

> 🧩 **難所 — closureがidentityを運ぶ**([`restoreObject()`](../../src/core/loader/authoringscenedocument.cpp#L281) / journal側の [`finalizeJournal()`](../../src/core/communication/editorjournal.cpp#L5002))
>
> **何をする所か**: commit済みのjournal recordに、spawnしたobjectのclosure(id + scene + 宣言index + 前後の兄弟id + authored JSON)を後から書き足し、再生時は同じidを同じ宣言区間へ戻します。
>
> **素朴に読むと**: 「spawnのforwardをそのまま再実行すればundo/redoは戻る」と考えたくなります。ところが [`insertObject()`](../../src/core/loader/authoringscenedocument.cpp#L217) は呼ぶたびに `next_authoring_object_id_value_++` で**新しい** idを採番するので、再生後のidが元と違い、以後のjournal(全部idで対象を指す)が丸ごと外れます。だから `finalizeJournal()` はcommit直後にclosureを焼き込み、`commandFromCanonical()` はclosureがあれば `makeRestoreObjectCommand()` へ分岐します。もう一段難しいのが `restoreObject()` の挿入位置決定です。保存した `declaration_index` を盲信すると、間に他の編集が入っていたときにずれます。そこで前後の兄弟 **id** を現在の文書から引き直し、`next` があればその位置、無ければ `previous+1`、どちらも無ければ保存indexを配列長でクランプ、という三段の劣化戦略を取ります(`previous >= next` なら区間が反転しているので拒否)。
>
> **骨子**:
> ```text
> finalizeJournal: forward.op == "spawn" -> forward["closure"] = closureForObject(...)
> commandFromCanonical(spawn):
>     closure あり -> makeRestoreObjectCommand(closure)   # id 保存
>     closure なし -> makeInsertObjectCommand(...)        # 新規採番
> restoreObject: index = next ? *next : (previous ? *previous+1 : min(saved, size))
> ```
>
> **手がかり**: `previous_object_id` / `next_object_id` が「宣言区間」を index ではなく **id** で表しているのが要点です。`Insert` と `Restore` が別の [`AuthoringStructuralChangeKind`](../../src/core/loader/authoringscenedocument.hpp#L72) なのも同じ理由で、ランタイム側のadapterはこの種別で「新規entityを作るのか、同じauthoring idへ紐づけ直すのか」を決めます。テストは [`WP161 behavior attachment sequence survives spawn undo redo`](../../test/editorjournal_test.cpp#L1377) と [`editorprojectiontransaction_test.cpp` 内](../../test/editorprojectiontransaction_test.cpp#L495)(destroy interval)。
>
> **不変条件**: journalに載るspawnは必ずclosure付き(closureなしspawnは「新規採番」を意味する)。restoreは保存indexを盲信せず必ず前後idから引き直す。同一idの二重生存は禁止。

> 🧩 **難所 — 二段公開と巻き戻し順**([`EditorProjectionTransaction::commit()`](../../src/core/loader/editorprojectiontransaction.cpp#L893))
>
> **何をする所か**: 上のstage / structuralStageを実際に使う側です。JSONコマンド列を未公開の文書へ適用し、resolverで `{Authoring, Resolved, revision, generation}` のcandidate pairをprepareしてから、全adapterを prepare → publish → pair公開 → finish の順に流します。どこで失敗しても呼び出し前のpairと全読者へ戻します。
>
> **素朴に読むと**: 最初に引っかかるのは `prepared.push_back(adapter)` が `adapter->prepare(context)` の**前**にあることです。順序ミスに見えますが意図的で、prepareが例外を投げたadapter自身も巻き戻し対象に入れるための前倒しです。逆にすると、途中まで状態を掴んだadapterがrollbackされずに残ります。次が `prepared.empty() ? Rejected : Failed` の分岐 — adapterに一度も触れていなければ「受理していない(Rejected)」、一つでも触れたなら「実行して失敗した(Failed)」で、RPC応答の `status` 文字列がここで決まります。そして `publish()` / `rollback()` / `finish()` は全て `noexcept` です([`publish()`](../../src/core/loader/editorprojectiontransaction.hpp#L192))。「非確保」まで明文化されているのは `rollback()` だけで、[宣言のコメント](../../src/core/loader/editorprojectiontransaction.hpp#L193) の `Both paths must be allocation-free.` はrollbackのpublish前/後の2経路を指します — `publish()` / `finish()` の非確保はヘッダに書かれておらず、実装側の慣行です。確保・decode・検証は全部prepareへ前倒しされていて、これを崩すと「publish途中でthrowして半分だけ公開」が起こりえます。
>
> **骨子**:
> ```text
> base.revision != expected -> Rejected(StaleRevision)      # global CAS
> 構造編集があれば structural_stage 経由、無ければ stage(...)
> for a in adapters: prepared.push_back(a); a->prepare()    # ここで throw しても巻き戻せる
> for a in adapters: a->publish()                           # noexcept
> document_target_.publishProjectionState(candidate_state)         # pairを一操作でswap
> for a in reverse(prepared): a->finish()
> catch: for a in reverse(prepared): a->rollback()
> ```
>
> **手がかり**: [`EditorProjectionPublicationMode`](../../src/core/loader/editorprojectiontransaction.hpp#L39) の `StagedNoexcept` と `InverseToken` の差が効きます。publish後のfault注入が `InverseToken` にしか適用されないのは、publish後でも逆トークンで戻せるadapterだけがそこで失敗を許されるからです。`rollback()` は「publish前ならprepared状態の破棄、publish後なら逆トークンの適用」の**両義**で、[`TransformProjectionAdapter::rollback()`](../../src/core/loader/editorprojectiontransaction.cpp#L869) の `published_` 分岐がその実例です。テストは [`editorprojectiontransaction_test.cpp` 内](../../test/editorprojectiontransaction_test.cpp#L312) / [`Structural spawn destroy and mixed command faults restore document and runtime exactly`](../../test/editorprojectiontransaction_test.cpp#L554)。
>
> **不変条件**: adapter publish以降の通常経路はthrowも確保もしない。pair公開はadapter publishの**後**、finishの**前**。`AfterPublication` faultではpairを旧版へswapし直してからadapterを逆順rollbackするため、authoringとresolvedの片側だけが残る状態はありません。rollback / finish は必ず逆順。

fixtureは [`test/fixtures/authoring_scene/multi_scene_roundtrip.json`](../../test/fixtures/authoring_scene/multi_scene_roundtrip.json) です。

## 3.13 `.vrma`（VRM Animation）コンテナ ✅実装済み

WP176〜WP178で入った、アニメーションクリップの交換形式です。段は三つあります — decode（WP176） → retarget（WP177） → `AnimationSource` としての登録（WP178）。decodeとretargetが作る中間表現そのものは `AnimationSource` ではありませんが、retarget済みクリップはWP178でanim_graph/ABIへpublishされます（後述）。

### decode

入口は [`decodeVrmaAnimation()`](../../src/core/loader/vrmadecoder.hpp#L25) と [`loadVrmaAnimation()`](../../src/core/loader/vrmadecoder.hpp#L32) です。

- `container_name` は**エイリアスゲート**を兼ね、`.vrma` 拡張子が必須です。
- [`VrmaDecodeOptions`](../../src/core/loader/vrmadecoder.hpp#L13) の `import_profile` と `tool_version` は**必須のprovenance**です（`source_uri` は省略時にcontainer名で埋まります）。
- 正規に選ぶのは最初のアニメーションで、`#animation/0` として識別します。
- バイトは1度だけ読み、**同じバイト列をhashしてdecode**します。provenanceが実際にdecodeした内容と食い違わないための仕様です。

### データ型

[`vrmaanimation.hpp`](../../src/core/model/vrmaanimation.hpp) にあります。

| 型 | 位置 |
|---|---|
| [`VrmaClip`](../../src/core/model/vrmaanimation.hpp#L87) | クリップ本体 |
| [`VrmaBodyChannel`](../../src/core/model/vrmaanimation.hpp#L35) / [`VrmaExpressionChannel`](../../src/core/model/vrmaanimation.hpp#L42) / [`VrmaGazeChannel`](../../src/core/model/vrmaanimation.hpp#L49) | チャンネル |
| [`VrmaSourceRig`](../../src/core/model/vrmaanimation.hpp#L70) / [`VrmaClipMetadata`](../../src/core/model/vrmaanimation.hpp#L75) | 元リグとprovenance（`content_sha256` を含む） |

[`VrmaInterpolation`](../../src/core/model/vrmaanimation.hpp#L12) は `linear` / `step` / `cubic_spline` です。値はxyzw格納で、translationは `component_count == 3`、rotationは `4`。cubic splineの接線は `in_tangents` / `out_tangents` として別配列に保たれ、glTFのアニメーションデータを落としません。

> **設計決定:** ヘッダのコメントが規範です — VRMA-C0 は decode/storage のみを持ち、root motion delta も target-rig / application state も**意図的に存在しません**。hips translationは通常のbody channelのままです。

### retarget

[`retargetVrmaClip()`](../../src/core/animation/vrmaretarget.hpp#L124) が、target rigのローカル座標系へ写した中間クリップを作ります。

- プロファイルはバージョン付きです（[`vrmaRetargetProfileVersion = 1`](../../src/core/animation/vrmaretarget.hpp#L16)）。
- [`VrmaRootMotionPolicy`](../../src/core/animation/vrmaretarget.hpp#L21) は現在 `preserve_hips_translation` のみです。抽出モードを持たないのは、後のプロファイルがv1データを読み替えずに方針を足せるようにするための、意図的な空きスロットです。
- 出力 [`VrmaRetargetedClip`](../../src/core/animation/vrmaretarget.hpp#L108) は provenance（[`VrmaRetargetProvenance`](../../src/core/animation/vrmaretarget.hpp#L41) の `source_rig_sha256` / `target_rig_sha256` / `profile_version`）と、警告 [`VrmaRetargetWarningKind`](../../src/core/animation/vrmaretarget.hpp#L29)（`missing_optional_target_bone` / `missing_target_expression`）を持ちます。

> **設計決定:** これは**不変のtarget-rig-local中間クリップ**であり、この型自体は `AnimationSource` ではありません。「不変」は、`retargetVrmaClip()` の戻り値が `std::shared_ptr<const VrmaRetargetedClip>` で、作った後は誰も書き換えないことを指します（`sample()` はconstで、毎回新しい `VrmaRetargetSample` を返すだけです）。「中間」は、decode（source rigのlocal）と実行時の再生の**間**に置かれた段という意味で、座標系をtarget rigのlocalへ移すところまでしかやらず、再生位置・速度・blend状態のような実行時の状態は持ちません。`retargetVrmaClip()` は instance / graph / renderer / ABI のどこへもpublishせず、publishは次項のWP178 APIが担います。交換形式の取り込みと、実行時のアニメーション再生を別の層に保つための境界です。

> **注意:** [`vrmaretarget.hpp` 内](../../src/core/animation/vrmaretarget.hpp#L122) のコメント（`This is not an AnimationSource and does not publish to an instance, graph, renderer, or ABI.`）はWP177時点のままで、WP178で入った登録経路を反映していません。ソース側のコメントが古い箇所です。

> 🧩 **難所 — rest差分の回転移送**([`VrmaRetargetedClip::sample()`](../../src/core/animation/vrmaretarget.cpp#L418) / スケール決定は [`retargetVrmaClip()`](../../src/core/animation/vrmaretarget.cpp#L463) 内 [`translation_scale` の初期化](../../src/core/animation/vrmaretarget.cpp#L557))
>
> **何をする所か**: 上の箇条書きは方針とプロファイルまでで、**移送の式そのもの**が書かれていません。R0プロファイルの中身は「回転はrest差分で移送」「hipsのtranslationだけrest高さ比でスケール」の2規則です。
>
> **素朴に読むと**: 同じhuman boneなのだから、sourceのlocal回転をそのまま入れればよさそうに見えます。しかしsourceとtargetはbindポーズ(restの向き)が違うので、そのまま入れるとキーが0のフレームですらtargetがsourceのbind姿勢に化けます。実際の式は `target_rest.R * inverse(source_rest.R) * sample(t)` で、内側の `inverse(source_rest.R) * sample(t)` が「sourceのrestからの局所差分」、それをtargetのrestへ載せ直す形です。差分と呼べる理由は式を裏返すと見えます — `sample(t) = source_rest.R * (差分)` なので、括弧の中身は「restの姿勢から**さらに**どれだけ回したか」をrest基準で測った回転です。いちばん分かりやすいのは `sample(t)` がちょうど `source_rest.R` に等しいフレームで、そこでは差分が単位回転になり、結果は `target_rest.R` そのもの、つまりtargetは自分のrest姿勢のまま立ちます。**掛ける順序が意味そのもの**で、`inverse(source_rest) * target_rest * sample` と書くと別の回転になります。hipsのtranslationも非自明で、係数は「sourceのhips rest **world** Y の絶対値」と「targetの同じ値」の比 — 腰の高さの比で歩幅を合わせています。素朴にtranslationをそのまま流すと、背の低いモデルが宙に浮き、背の高いモデルが地面にめり込みます。`std::abs` はY軸が下向きのrigでも比を正に保つためです。
>
> **骨子**:
> ```text
> local_transforms := target_rest_pose のコピー   # チャンネルの無い骨は rest のまま
> rotation:    target_rest.R * inverse(source_rest.R) * sample(t)
> translation: target_rest.T + scale * (sample(t) - source_rest.T)
>              scale = |target_world_hips.y| / |source_world_hips.y|   (hips のみ許可)
> ```
>
> **手がかり**: hips以外のtranslationチャンネルは受理段階で**拒否**されます(`only hips translation is supported`)。R0はroot motion抽出を持たず、hipsのtranslationは普通のbody channelのまま、という上の設計決定の実装面です。またR0のrest参照は2種類に分かれます — 回転移送が使うのは各ボーンの *local* rest(`source_rest` / `target_rest`)だけで、親チェーンの向きの差は補正しません。hipsのtranslation係数だけが **world** rest 行列のY成分を見ます([高さ比の算出](../../src/core/animation/vrmaretarget.cpp#L561))。前者の割り切りのせいで、骨の比率が大きく違うrigでは肘や膝がずれますが、これは実装漏れではなくプロファイルv1の定義域です。`sample()` の出力は **target rigの元node index** で並ぶ点にも注意(layout順への写像は `animationservice.cpp` の `sampleVrma()` が `rig.layout_to_original` で行います)。
>
> **不変条件**: 回転の合成順序を `target_rest * inverse(source_rest) * sample` から動かさない。方針を足すなら [`VrmaRootMotionPolicy`](../../src/core/animation/vrmaretarget.hpp#L21) に値を増やして `version` を上げる。rig hashのcanonical文字列フォーマットは互換の一部で、フィールド追加は末尾のみ。

### `AnimationSource` としての登録（WP178） ✅実装済み

retarget済みクリップは、`anim_graph` と同じ名前付きClip/Cursor語彙へ登録できます。

| API | 位置 | 役割 |
|---|---|---|
| `AnimationServiceRuntime::registerVrmaSource()` | [`animationservice.hpp` 内](../../src/core/animation/animationservice.hpp#L34) | `VrmaRetargetedClip` をobject名 + source名で登録 |
| `AnimationServiceRuntime::reloadVrmaSource()` | [`registerVrmaSource()`](../../src/core/animation/animationservice.hpp#L37) | 論理identityを保ったまま、そのsource assetのgenerationだけを進める |

ABI側では [`AnimationClipKindV1`](../../src/core/userpublic/animation/abi_v1.hpp#L98) が `skeletal_clip` / `vrma_retargeted_clip` を区別します。clip descriptorの**加算的な末尾**（[`abi_v1.hpp` 内](../../src/core/userpublic/animation/abi_v1.hpp#L171)）に `profile_version` / `asset_identity` / `asset_generation` / `source_rig_sha256` / `target_rig_sha256` が載り、[`CursorStatusV1`](../../src/core/userpublic/animation/animgraph.hpp#L66) にも同じprovenanceが出ます。

> **設計決定:** skeletal clipと別語彙を作らず、`AnimationClipKindV1` とprovenanceフィールドで区別しています。ゲームコードから見たClip/Cursorの扱いは`.vrma`由来かどうかで変わらず、reloadは論理identityを保ったままgenerationだけを進めます。ABIの追加は既存フィールドの後ろへの加算なので、古い呼び出し側は `reserved2` までで読み止められます。

テストは [`test/vrmadecoder_test.cpp`](../../test/vrmadecoder_test.cpp) / [`test/vrmaretarget_test.cpp`](../../test/vrmaretarget_test.cpp) / [`test/vrmasource_test.cpp`](../../test/vrmasource_test.cpp)（GPU）、fixtureは [`test/vrma_fixture_writer.cpp`](../../test/vrma_fixture_writer.cpp) + [`test/vrma_fixture.hpp`](../../test/vrma_fixture.hpp) が生成します。

次章では、sceneの中心にあるECSの実体をメモリ配置から追います。
