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
| project.json封筒 | [`parseProjectEnvelopeText()`](../../src/project/projectformat.cpp#L311) | [`ProjectBasicConfig::ProjectBasicConfig()`](../../src/core/loader/basicconfig.cpp#L625) |
| path参照 | [`ProjectPathResolver`](../../src/project/projectpathresolver.hpp#L114) | [`PathResolver`](../../src/core/loader/pathresolver.hpp#L9)（module寿命、ログ、engine resource注入） |
| scene | [`normalizeSceneDataJson()`](../../src/project/sceneformat.cpp#L268) | [`SceneLoader::load()`](../../src/core/loader/scene.cpp#L1650) |
| render feature | [`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L3422) | [`registerRenderGraphVariantFamilyFromJsonData()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1660) |
| JSON-RPC | [`parseJsonRpcRequest()`](../../src/project/jsonrpc.cpp#L148) | [`RpcServer`](../../src/core/communication/rpcserver.hpp#L56) |
| asset manifest | [`parse/generate/verify`](../../src/project/assetsmanifest.hpp#L73) | [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L148)、結果を持つ module [`AssetsVerification`](../../src/core/loader/assetsverification.hpp#L83)(✅AV-1) |
| material/surface | [`parseMaterialFormatJson()`](../../src/project/materialformat.cpp#L744)、[`parseSurfaceFormat()`](../../src/project/surfaceformat.cpp#L1239) | runtime接続済み（WP116〜117, 122）。`.surface`は [`surfacecompiler`](../../src/core/shader/surfacecompiler.cpp) でGLSL/SPIR-V化されpipelineへ。`.material.json`は [`lowerMaterial()`](../../src/project/materiallowering.hpp#L160) → [`registerReloadableMaterialValuesFile()`](../../src/core/material/materialcontainer.hpp#L347) |

新形式を追加するときは、まず`src/project`へGPU非依存の受理/拒否規則を置き、次に`src/core`で実体化するのが既存パターンです。

## 3.2 ProjectSourceと設定の三段優先順位

[`ProjectSource`](../../src/core/loader/projectsrc.hpp#L7) は二種類の入力を保持します。

- `raw_data`: `PelicanCore(settings)`へ直接渡した上書きJSON
- `project_data`: playerが読んだ`project.json`全体

`project.json` のschema/version/`engine_min_version`ゲートは、まず [`parseProjectEnvelopeText()`](../../src/project/projectformat.cpp#L311) が検証します。`--ignore-engine-version`で継続する場合も純粋層はログを出さずwarning文字列を返し、engine adapterだけがそれをquillへ渡します。検証規則の正本はこの関数だけで、`basicconfig.cpp`やdevcliに写しはありません。`engine_min_version` が新しすぎるときの拒否は MG-1 から `project.engine_too_old@1` という名前で始まります（[`projectEngineTooOldCode`](../../src/project/projectformat.hpp#L19)）。

player ではこの parse の**前**に、MG-1 の入口 [`requireProjectMigrationNotRequired()`](../../src/project/projectmigration.cpp#L563) が 1 回だけ走ります（[呼ぶ場所](../../src/player/main.cpp#L728)）。落ちたプロセスが残した project 文書の transaction を復旧してから、project root の下の `*.json` の最上位の `schema` / `version` を parser を通さずに読み、登録された移行手順の `from` に当たる文書があれば `project.migration_required@1` で止めます。parser の version 検査より前に置くのは、project.json 自身の版が上がったときも「知らない版」ではなく「移行が要る」で止めるためです。v0 の登録表は空で、そのときは root を歩きもしません。流れ全体は [manual 第3章 §3.13](../manual/03_project_format.md) にあります。

[`ProjectEnvelope`](../../src/project/projectformat.hpp#L39) が持つのは `name` / `engine_min_version` / `saved_by_engine` / `requirements` / `basic_config` / `asset_stores` / `hot_reload` / `prefabs` の8つです(`hot_reload` は HW-1 で増えた。下の箇条)。`saved_by_engine`（MG-1）は最後に project.json を書いたエンジンの版の**記録**で、何の判定にも使いません（文字列でなければ無視します）。このうち `requirements` は P-2b で入った **`requires`** キーで（`requires` はC++のキーワードなのでフィールド名だけずらしてあります）、「このプロジェクトが無いと動かないビルド機能」を宣言します。

- 語彙は [`knownProjectRequirements()`](../../src/project/projectformat.hpp#L88) が返す表だけが持ち、現在は **`animation` / `audio` / `physics` / `xr` / `vat` / `seqplayer` の6語**です（PG-1。各語が `PELICAN_WITH_ANIMATION` / `_AUDIO` / `_PHYSICS` / `_OPENXR` / `_VAT` / `_SEQPLAYER` の1つを指す）。表の行は `src/project/projectrequirements.def` で、同じ行をCMake側のコーパス選別（`cmake/pelican_project_requirements.cmake`）、component codecの `required_feature`、package の `requires.capabilities` の検査も読みます。知らない名前は黙って無視せず `project.requires_unknown@1` で拒否します（[`parseProjectRequirements()`](../../src/project/projectformat.cpp#L119)）。
- このビルドが満たせない要求を書いたプロジェクトは、封筒のパースの時点で `<語>.module_unavailable@1`（例: `animation.module_unavailable@1`）を投げて止まります（[検査](../../src/project/projectformat.cpp#L264)）。ノードを1つずつ辿って「そんなopは知らない」と言われるより前に、名前で答えるためです。
- 失敗させずに「このビルドでは開けない」だけ知りたい呼び手（`projects/` を順に調べるテスト等）のために、[`unsatisfiedProjectRequirement()`](../../src/project/projectformat.hpp#L109) が最初の未充足要求を返す形も用意されています。

**`hot_reload`(✅HW-1、`c947cef4`)。**hot reload が監視する素材とフォルダの宣言で、[`parseProjectHotReload()`](../../src/project/projectformat.cpp#L168) が `parseProjectEnvelopeJson()` の中で `asset_stores` の隣に読み、`ProjectEnvelope::hot_reload`(`std::optional<std::vector<std::string>>`。キーが無ければ `nullopt`)に入れます。

- 正規化: `/` と `\` で成分に分け、空の成分と `.` を落として `/` でつなぎ直します。存在するかは見ません(純粋層なので filesystem を触らない)。
- 拒否は 1 つの code [`projectHotReloadInvalidCode`](../../src/project/projectformat.hpp#L36)(`project.hot_reload_invalid@1`)で、`requires` の `project.requires_unknown@1` と同じく文言が種類を言います: 配列でない / 文字列でない / 正規化して空 / 絶対(先頭が `/` か `\`、`C:` の形、`://` を含む)/ `..` の成分 / 正規化して重複。`"."` は空、`project://x` は絶対の扱いです。
- runtime への道: [`ProjectPathResolver::setup()`](../../src/project/projectpathresolver.cpp#L343) が envelope の値を保持し、`hotReloadPaths()` で返します(reset で消える)。engine の [`PathResolver::hotReloadPaths()`](../../src/core/loader/pathresolver.hpp#L34) はそれに委譲し、[`ReloadService::setup()`](../../src/core/watch/reloadservice.cpp#L882) が起動時に 1 回読んで、`FileWatcherOptions::watch_paths` と gate の `WatchDeclaration` に渡します(下の「難所 — ファイルの再走査」の後の段落)。✅PJ-1 から、RPC `edit_project_settings` が値を `replaceDeclarations()` で差し替え、`ReloadService::setup()` をもう一度呼びます(§3.3 の「宣言のその場の差し替え」)。
- test: [`projectformat_test.cpp` 内](../../test/projectformat_test.cpp#L127)「HW-1 project.json hot_reload is read as normalized logical paths and refused by name」。

もう一つ、X-1で `project.json` に増えた **`packages`** キーは `ProjectEnvelope` には入りません。読むのは [`readInstalledPackagesJson()`](../../src/project/packagemanifest.hpp#L103) で、1エントリは `{namespace, package_version, sha256}` の [`InstalledPackage`](../../src/project/packagemanifest.hpp#L70) です（§3.16）。封筒のパーサとvendoringの台帳を別の関数に分けてあるので、package を知らない読み手は `packages` を素通りできます。

[`ProjectBasicConfig::ProjectBasicConfig()`](../../src/core/loader/basicconfig.cpp#L625) は検証済み封筒の`basic_config`を含む次の三sourceを [`JsonLoader`](../../src/core/loader/basicconfig.cpp#L64) へ渡します。

1. `raw_data` — API/起動時の上書き
2. `project.json` の `basic_config`
3. `engine://default_config.json`

各fieldはこの順で最初に見つかった値が採用されます（[`JsonLoader::getVal()`](../../src/core/loader/basicconfig.cpp#L72)）。JSON object全体をdeep mergeするのではなく、必要fieldをpath単位で問い合わせる方式です。

構築時にwindow、framerate、seed、camera、各データファイル参照を値として取り込みます。その後、assets/rendering/UI/input JSON本文はgetterの初回呼び出し時に読み、`mutable optional<string>`へcacheします（[basicconfig.hpp](../../src/core/loader/basicconfig.hpp#L125)）。

### sceneだけはcacheではなくdocument

sceneは単なる文字列cacheから、**[`SceneProjectionState`](../../src/core/loader/resolvedscene.hpp#L326) が所有する `AuthoringSceneDocument` と `ResolvedScene` の不変pair**へ格上げされました（WP149 / WP166 / WP360）。

格上げされたのがsceneだけなのは、編集の有無で必要な機能が違うからです。assets/rendering/UI/inputのJSONは読み取り専用で、cacheの役目は「二度目以降のファイル読みを省く」ことだけです。一方sceneはセッション中に書き換えられ、(a) ディスクへの書き戻し（本節の `saveSceneDocument()`）、(b) 改訂番号による競合検出（§3.12 のCAS）、(c) renameや並べ替えに耐えるobject単位の安定identity（§3.12 の `AuthoringObjectId`）を必要とします。`std::optional<std::string>` にはこの三つを置く場所がありません。

保持しているのは、scene に関する metadata を 1 つにまとめた [`SceneMetadataState`](../../src/core/loader/basicconfig.hpp#L106) です（WP380a）。

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

1 struct にまとまっているのは、scene load の commit がこの struct の **1 回の `noexcept` swap** だからです（[`SceneMetadataState::swap()`](../../src/core/loader/basicconfig.cpp#L506) / [`MetadataCommitToken::commit()`](../../src/core/loader/basicconfig.cpp#L584)）。document だけ、projection だけ、current scene だけを個別に公開する経路は production には無く（[`MetadataCommitToken`](../../src/core/loader/sceneload.hpp#L195) の `Impl` は `ProjectBasicConfig` の private）、candidate は [`prepareBorrowedSceneMetadata()`](../../src/core/loader/basicconfig.cpp#L589)（committed projection をそのまま使う root）か [`prepareImportedSceneMetadata()`](../../src/core/loader/basicconfig.cpp#L602)（snapshot import）か [`prepareRestoredSceneMetadata()`](../../src/core/loader/basicconfig.cpp#L896)（再生のセッションの Stop。✅PM-2。下の注）が token として作り、§3.5 の scene executor が commit します。失敗した token は捨てられるだけで、live の `scene_metadata` には触れません。「1 段の swap である」ことは台帳側の executor 仕様がそのまま定めていて、WP380a の完了記録は phase 列を `prepare → early_publish → noexcept candidate publish → activate → metadata commit(1 struct の noexcept swap)→ retire(次フレーム queue)→ outbox flush` と書いています（[`docs/implementation_plan.md`](../implementation_plan.md) の WP380a 完了記録）。コード側も同じことを言っており、分割公開は fault injector 専用の反実仮想として `Production can publish only SceneMetadataState as a whole through commit().` とコメントされています（[分割公開の反実仮想に付いたコメント](../../src/core/loader/basicconfig.cpp#L567)）。代入ではなく swap なので、commit 後は旧 state が token 側へ移り、旧 projection の破棄は commit 点の中ではなく token の寿命末で起きます（[`swap()` の本体](../../src/core/loader/basicconfig.cpp#L506) は member ごとの `std::swap` だけです）。

そのため [`sceneDataJson()`](../../src/core/loader/basicconfig.cpp#L1036) は**生ファイル文字列を返しません**。

```cpp
std::string ProjectBasicConfig::sceneDataJson() const {
    return AuthoringSceneAuthority::encodeSemantic(sceneDocument());
}
```

返るのはauthoring authorityによるsemantic encodeです。scene を **resolve する**とは、作者が書いた authoring document に prefab の展開と省略値の補完を施し、読み手が場合分けなしに読める不変の形（[`ResolvedScene`](../../src/core/loader/resolvedscene.hpp#L268)）へ畳むことです — 「scene の読み手に影響する authored な省略は、この不変 projection を prepare する時点で全て埋める」がヘッダのコメントの言い分です（[`ResolvedSceneDefaults` の上のコメント](../../src/core/loader/componentcodec.hpp#L209)）。各componentが authored か生成物かは [`ResolvedComponentOrigin`](../../src/core/loader/resolvedscene.hpp#L173) に残ります。なお [用語集](../manual/14_glossary.md) の「解決(resolve)」は**path参照**をファイルの絶対パスへ確定させる別の用法で、§3.3 の話です。同じ動詞が2つの層で使われています。runtimeの読者はraw JSONではなく [`resolvedScene()`](../../src/core/loader/basicconfig.cpp#L769) を読み、両者とrevision / resolver generationは同じ `SceneProjectionState` から公開されます。読み書きのAPIは次の通りです。

| API | 宣言 | 役割 |
|---|---|---|
| `sceneProjectionState()` | [`sceneProjectionState()`](../../src/core/loader/basicconfig.cpp#L773) | authoring / resolved / revision / generation のpairを取得（初回は遅延load） |
| `sceneDocument()` / `resolvedScene()` | [`sceneDocument()`](../../src/core/loader/basicconfig.cpp#L765) / [`resolvedScene()`](../../src/core/loader/basicconfig.cpp#L769) | 同じpairのauthoring側 / resolved側を取得 |
| `updateSceneDocument()` | [`updateSceneDocument()`](../../src/core/loader/basicconfig.cpp#L784) | scene（v2）バイト列をprepareしてpairで差し替え |
| `invalidateSceneDocument()` | [`invalidateSceneDocument()`](../../src/core/loader/basicconfig.cpp#L788) | pairを破棄し、次回の再構築を強制 |
| `importSceneDocument()` | [`importSceneDocument()`](../../src/core/loader/basicconfig.cpp#L793) | 外部由来のバイト列を取り込み、新しい `SceneRevision` を返す |
| `saveSceneDocument()` | [`saveSceneDocument()`](../../src/core/loader/basicconfig.cpp#L936) | ディスクへ書き戻し、pairを公開し、`SceneSaveResult`（revision / digest / byte数）を返す |

**再生のセッションの Stop の token(✅PM-2)。**[`prepareRestoredSceneMetadata(saved)`](../../src/core/loader/basicconfig.cpp#L896) は、Play のときに覚えた `AuthoringSceneDocument` を §3.12 の [`restagedAuthoring()`](../../src/core/loader/authoringscenedocument.cpp#L435) で出し直し(中身と `AuthoringObjectId` は保存時のまま、`SceneRevision` は `next_scene_revision`、次の `AuthoringObjectId` は保存時と今の大きい方)、今の defaults・prefab registry・bindable provider で resolver を prepare して candidate にします。`prepareImportedSceneMetadata()` の candidate との違いは、ディスクの基準(persistence baseline)の digest と provider を今のまま残すことです(セッションの間は `save_scene` を断るので、ファイルはセッションの始めのまま)。RPC の `set_play_state` はこの token を `SceneLoadRequest::owned` に載せ、保存した scene の id で読み込みます。commit までは何も公開されず、読み込みが新しい authority を与えるので、その epoch が editor の gate を閉じます(authority は保存も復元もしません)。

保存の失敗は型付きです。[`SceneSaveErrorCode`](../../src/core/loader/basicconfig.hpp#L26) は `ExternalModification` / `Unavailable` / `IoFailure` の三つで、[`SceneSaveError`](../../src/core/loader/basicconfig.hpp#L32) が保持します。`scene_metadata.persistence_baseline` の `scene_digest_sha256` と保存直前のディスク内容を突き合わせるため、**エディタの外でファイルが書き換わっていれば `ExternalModification` で拒否**します。

テスト用のフォールト注入点も宣言的に列挙されています。[`SceneSaveFaultPoint`](../../src/core/loader/basicconfig.hpp#L41) は `AfterEncode` / `AfterDiskDigest` / `AfterTemporaryWrite` / `AfterTemporaryValidation` / `AfterCachePrepare` / `BeforeReplace`、[`SceneImportFaultPoint`](../../src/core/loader/basicconfig.hpp#L50) は `AfterCandidatePrepare` / `AfterPublication` です。

> **設計決定:** 保存経路の中断点を実装内部の分岐ではなくenumとして公開することで、「どの中断点でもディスク上のsceneが壊れない」という性質をテストから網羅的に叩けます。列挙名がそのまま保存手順の段階名になっています。

> 🧩 **難所 — 保存のTOCTOU窓**([`saveSceneDocument()`](../../src/core/loader/basicconfig.cpp#L936))
>
> **何をする所か**: 編集済み文書を一時ファイル経由でディスクへ書き戻し、原子的に置換してから、メモリ側の文書とbaseline digestを差し替えます。TOCTOU と「一時ファイル + rename による原子的置換」の一般的な背景は [第10章 §10.4](10_background_knowledge.md) にあります。
>
> **素朴に読むと**: digest比較が **2回** あるのが冗長に見えます。しかしdigest比較が言えるのは「**比較したその瞬間まで**外部変更が無かった」ことだけで、比較を過ぎてから外部が書いた分については何も保証しません。1回目([`disk_digest` の検査](../../src/core/loader/basicconfig.cpp#L967))は一時ファイルを書く**前**にあるので、これだけにすると一時ファイルの書き込み・読み戻し・意味検証にかかる時間がまるごと TOCTOU 窓(time-of-check to time-of-use の略 — 検査した時点と実際に使う時点がずれるせいで生まれる、その間に外部が書き換えられる隙間)になり、保存処理はその間に外部エディタが書いた内容を、気付かないまま上書きしてしまいます。2回目([置換直前の `stableDiskDigest` 検査](../../src/core/loader/basicconfig.cpp#L1018))は置換の直前に置かれていて、窓を実務上無視できる幅まで縮めるためのものです(WP362 からは比較相手が `save_baseline.scene_digest_sha256` —— scene digest と prefab read-set を一体で凍結した不可分 baseline の一部 —— になりました)。もう一つ見落としやすいのが末尾の順序で、ファイル置換([`replaceSceneFileAtomically()` の呼び出し](../../src/core/loader/basicconfig.cpp#L1026))の **後** に [`publishPreparedSceneState(next_state)`](../../src/core/loader/basicconfig.cpp#L753) と [`*scene_metadata.persistence_baseline = std::move(next_baseline)`](../../src/core/loader/basicconfig.cpp#L1032) が来ます(置換の後にあるのは3回目の比較ではありません。baseline を保存したバイト列の digest と prefab read-set へ**更新**する操作です)。この区間を確保・decode・I/Oなしの無throwにしてあり、逆順にすると「置換に失敗したのにメモリ側だけ新しいrevision」が作れてしまいます。pair 公開が `noexcept` の [`publishPreparedSceneState()`](../../src/core/loader/basicconfig.cpp#L753)(中身は `SceneProjectionState` の `swap` と `next_*` カウンタの代入)で、baseline の更新が `ScenePersistenceBaseline`(`std::string` + `std::vector`)丸ごとの **move 代入**なのも同じ理由で、どちらもバッファを移すだけで新しい確保を伴いません(WP380a 前は `scene_baseline_digest->swap()` でした。§3.2 の `SceneMetadataState` に統合された今は optional の中身への move 代入です)。
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
> **手がかり**: 上の `SceneSaveFaultPoint` 6値がそのまま手順の段名で、2回目のdigest検査は `AfterCachePrepare` と `BeforeReplace` の**間**にあります。[`stableDiskDigest()`](../../src/core/loader/basicconfig.cpp#L342) が `watch::readStableContentDigest()` を使うのは、「書き込み途中のファイルを読んだ」状態(`retry`)を成功と混同しないためです。一時ファイルは `TemporarySceneFile` のデストラクタが必ず消すので、どの中断点でthrowしてもゴミが残りません。[`importSceneDocument()`](../../src/core/loader/basicconfig.cpp#L793) は逆に「失敗しうる間は live に触らない」側の作りで、candidate を `MetadataCommitToken` に包んで §3.5 の scene executor へ渡し、commit された場合だけ同じ `SceneMetadataState::swap()` で公開されます（reload が失敗すれば token が捨てられるだけで、戻す作業そのものがありません）。対にして読むと早いです。テストは [`sceneformat_test.cpp` 内](../../test/sceneformat_test.cpp#L698)「SAVE0 is failure-atomic at every prepare point」。
>
> **不変条件**: 直列化は1回だけ(以降の全段が同じバイト列を消費する)。ファイル置換より後にthrowしうる処理を置かない。baseline digestの更新はファイル置換と同一の無throw区間で行う。

## 3.3 PathResolver

解決規則の宣言は [`projectpathresolver.hpp`](../../src/project/projectpathresolver.hpp#L114)、中心実装は [`ProjectPathResolver::resolveRef()`](../../src/project/projectpathresolver.cpp#L723) です。ここは`pelican_project`に属し、Vulkan・quill・module containerへ依存しません。

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

`engine://`はファイルシステム上のpathではなく、実行ファイルへ埋め込まれたresource IDです。また**fragment**を、コンテナ内要素のアドレスとして失わず伝えるためvariantになっています（[`parsePathReference()`](../../src/project/projectpathresolver.cpp#L278)）。fragmentは参照文字列の `#` 以降の `kind/path` で、「そのファイルの中のどれを指すか」を表します — 例が `assets/character.glb#mesh/Cube`（[`pathresolver_test.cpp` 内](../../test/pathresolver_test.cpp#L486)）で、`kind` が要素の種類、後半が名前です。`/` を含まない後半は名前アドレス、含む場合はコンテナ内のフルパスアドレスになります（[`AssetFragmentAddressKind`](../../src/project/projectpathresolver.hpp#L21)。[用語集](../manual/14_glossary.md) の「フラグメント参照(`#`)」）。`#` が2つある、`kind` だけで後半が無い、後半が全部数字、といった形は受理されません。

### scheme別の処理

| 参照 | 解決先 | 保護 |
|---|---|---|
| `engine://id` | `EngineResourceId` | 登録IDだけを後段で許可 |
| `project://x` または相対`x` | project rootまたはasset store | root外escape拒否 |
| `user://x` | project名由来のuser root | user root外escape拒否 |
| CLI由来の絶対path | そのpath | `--allow-absolute-paths`必須 |
| project JSON内の絶対path | 不許可 | 常に拒否 |

`std::filesystem::weakly_canonical`（`.` / `..` を畳み、途中までのsymlinkを解決して正規形へ落とす標準関数。末尾が実在しなくても失敗しない点が `canonical` との違いです）後にroot包含判定を行うため、単純な`../`文字列検査より強い境界です（[`ProjectPathResolver::resolveRef()`のproject処理](../../src/project/projectpathresolver.cpp#L828)）。

### asset store

[`ProjectPathResolver::setup()`](../../src/project/projectpathresolver.cpp#L343) は`project.json.asset_stores`を読みます。

- 各storeは論理`mount`と実rootを持つ。
- mount同士の重なりを拒否。
- `.pelican/local.json`で実rootだけをローカル上書き可能。
- 上書きはprojectで宣言済みのstore名だけ。
- 実root同士の包含/重なりも拒否。
- optional manifestのpathはproject root内に制限。

相対参照がmount prefixに一致すると、project rootではなくstore rootへ付け替えます（[`asset store解決`](../../src/project/projectpathresolver.cpp#L812)）。これにより、VCS上の論理pathは固定したまま、大容量assetの実配置を開発者ごとに変えられます。

#### 宣言のその場の差し替え(✅PJ-1、`89643cd7`)

RPC `edit_project_settings`(第7章の RPC の表、manual 第10章 §10.3)は、setup をやり直さずに `asset_stores` と `hot_reload` の部分だけを差し替えます。

- `asset_stores` の読み方は `setupImpl()` から切り出した static の [`parseAssetStores()`(PJ-1 で切り出し)](../../src/project/projectpathresolver.cpp#L357) 1 か所です(`.pelican/local.json` の適用込み)。setup も差し替えも同じ検査を通ります。
- [`planDeclarations()`](../../src/project/projectpathresolver.cpp#L648) は新しい宣言を読み(検査に落ちれば `projectSettingsInvalidCode`)、[`planChange()`](../../src/project/projectpathresolver.cpp#L544) で今の store と比べて [`AssetStoresChange`](../../src/project/projectpathresolver.hpp#L95) `{added_stores, removed_stores, declared_manifests, removed_manifests}` を返します。何も変えません。RPC は project.json を書く前にこれを呼んで断ります。
- `planChange()` の規則: 今ある store について、消えたら `stores_added_in_place` に無い限り断る / 論理 mount が変わったら断る / root(`.pelican/local.json`)が変わったら断る / manifest が無→有なら宣言の追加(解決できなければ `projectSettingsInvalidCode`)/ 有→無は `manifests_declared_in_place` か `stores_added_in_place` にあるときだけ / 有→別のものは断る。新しい store は、root が `weakly_canonical(project root / 論理 mount)` と同じときだけ受けます(mount の下の参照が前と同じファイルに解けるため)。断るのは [`AssetStoresChangeRefused`](../../src/project/projectpathresolver.hpp#L78) `{code, store, reason}` で、code は `project.settings_change_unsupported@1` か `project.settings_invalid@1` です。
- [`ProjectPathResolver::replaceDeclarations()`](../../src/project/projectpathresolver.cpp#L662) は同じ plan を作り(断るときは何も変えない)、`stores_added_in_place` / `manifests_declared_in_place` を更新してから `asset_stores` と `hot_reload_paths` を置き換えます。store と一緒に足した manifest は store の側にだけ記録するので、外すときは store ごと外れます。2 つの記録は `reset()` で消えます。
- engine の [`PathResolver::replaceDeclarations()`(module の側)](../../src/core/loader/pathresolver.cpp#L62) は委譲して、`asset store added in place: <name>=<root>` / `asset store removed in place: <name>` を INFO で出します。
- lock はありません。差し替えの間ほかの thread が resolver を読まないことは呼び手が守る前提で、engine は RPC の handler(frame の境目、main thread)から呼びます(header の注記)。
- test: [`projectsettingsrpc_test.cpp` の PJ-1 の 1 本目](../../test/projectsettingsrpc_test.cpp#L207)(headless の `pelican_player --rpc` を子 process で起こし、Blank の project に store・監視・manifest を足して戻す)と [2 本目](../../test/projectsettingsrpc_test.cpp#L321)(起動時からある宣言は変えられない)。`test/CMakeLists.txt` では `pelican_define_test(projectsettingsrpc_test GPU ...)` で定義しています。

### engine resource

shader、feature JSON、default config、debug fontなどは [`resources/CMakeLists.txt`](../../src/core/resources/CMakeLists.txt#L1) で`battery-embed`へ登録され、[`engineResource()`](../../src/core/loader/engineresources.cpp#L169) がIDを実データへ変換します。

resource追加には次の三箇所が必要です。

1. `b_embed(...)`または`embed_shader(...)`
2. `registered_ids`
3. `engineResource()`のdispatch

一つでも漏れるとビルドまたはruntime lookupが失敗します。これは[第9章](09_black_magic_and_gotchas.md)でも注意点として扱います。

## 3.4 asset manifestの起動検証

playerはcore起動前に module [`AssetsVerification`](../../src/core/loader/assetsverification.hpp#L83) の [`verifyAtStartup()`](../../src/core/loader/assetsverification.cpp#L175) を呼びます([呼び出し](../../src/player/main.cpp#L779)。✅AV-1 から。それまでは自由関数 `verifyAssetsAtStartup()` を直接呼んでいました)。`verifyAtStartup()` は store の一覧・hash cache の path・strict を覚えてから、今も残る [`verifyAssetsAtStartup()`](../../src/core/loader/assetsverification.cpp#L148)(中身は無名名前空間の `verifyAssetStores(..., log = true)`)を呼び、store ごとの結果 [`AssetStoreVerification`](../../src/core/loader/assetsverification.hpp#L27)(`StartupAssetsVerificationSummary::results`)を `Record {verified_at, stores}` として持ちます。log の行と strict の判定は AV-1 の前と同じです。純粋な実作業は [`verifyAssetsManifest()`](../../src/project/assetsmanifest.cpp#L574) です。

- file size、SHA-256、missing/extra/case mismatch/unreadableを分類。
- `.pelican/assets-hash-cache.json`で再hashを減らす。
- `--strict-assets`なら通常warningの差異をerrorへ昇格。
- errorがあれば`PelicanCore::run()`へ入らず終了。
- `PELICAN_WITH_HOT_RELOAD=OFF`(配布 build)では、strict でなければ照合せず、理由 `"PELICAN_WITH_HOT_RELOAD=OFF"` だけを持って返る([`#if !PELICAN_WITH_HOT_RELOAD`](../../src/core/loader/assetsverification.cpp#L188))。

**持った結果の読み方と走らせ直し(✅AV-1)。**module は RPC の 3 method と `get_status.assets_verification` に結果を渡します(形は manual 第10章 §10.3 の「起動時の asset 検証の結果」)。

| 入口 | RPC | 中身 |
|---|---|---|
| [`statusJson()`](../../src/core/loader/assetsverification.cpp#L204) | `get_status.assets_verification` | `running` → `running`、module が設定されていない → `not_run` `"this host does not verify assets"`、飛ばした理由があればそれ、結果が無いか store が 0 → `not_run` `"no asset store declares a manifest"`、それ以外は severity ごとの件数 |
| [`detailJson()`](../../src/core/loader/assetsverification.cpp#L238) | `get_assets_verification` | `statusJson()` の `state` / `reason` に、`verified_at` と store ごとの issue の全件。走っている間は直前の `Record` を返す |
| [`verify()`](../../src/core/loader/assetsverification.cpp#L302) | `verify_assets` | `startLocked()` が worker thread を 1 本起こす(走っていれば何もしない = 合流)。worker の [`run()`](../../src/core/loader/assetsverification.cpp#L253) は `verifyAssetStores(..., log = false)` で同じ照合をし、成功したら `Record` を置き換えて飛ばした理由を消す |
| [`updateManifest()`](../../src/core/loader/assetsverification.cpp#L317) | `update_assets_manifest` | store を名前で引き、`generateAssetsManifest(root, manifest, cache, name, false)` で書いてから `verify()` |
| [`redeclareStores()`(PJ-1)](../../src/core/loader/assetsverification.cpp#L376) | `edit_project_settings` の反映 | ✅PJ-1。新しく宣言した manifest のうちファイルが無いものを `disk` の下で `generateAssetsManifest()` で書き、store の一覧を置き換えてから `verify()`。manifest を宣言した store が 1 つも無くなれば `Record` を捨てる。走っている worker があれば `stale` を立て、worker は結果を出す前に新しい一覧でもう一度照合する([`stale` を見る所](../../src/core/loader/assetsverification.cpp#L284))。返り値は `verify()` の答えに `generated` / `failed` を足したもの |

mutex は 2 つです。`mutex` は状態(store の一覧・結果・`running`)を守り、`disk` は store・manifest・hash cache を読み書きする間を守ります。worker の照合と `updateManifest()` の書き込みは `disk` で直列になるので、再検証の最中に `update_assets_manifest` が来ると handler(= frame)はその照合が終わるまで待ちます(報告に残された穴)。PJ-1 の `redeclareStores()` が manifest を書くときも同じで、書いている間と照合の終わりを待つ間は frame が止まります。module の破棄は `stop()` で worker を join するので、長い照合の途中で engine を閉じると照合の終わりまで待ちます。拒否は [`AssetsVerificationError`](../../src/core/loader/assetsverification.hpp#L64)(`code()` と `fields()`)で、RPC の側が `data: {error_code, error_fields}` にします。`fields_` は丸括弧で初期化します。波括弧だと nlohmann の json 1 つが要素 1 つの配列になり、RPC の側が object でない `fields()` を `{}` に置き換えてしまうためです(コンストラクタの注記)。

RPC の側は [`internal::configureAssetsVerificationRpcHandlers()`](../../src/core/communication/rpcserver.cpp#L1584) で、module は `EngineRpcModules::assets_verification` に `FastModuleContainer::tryGet<AssetsVerification>()` で入ります。module の無い host(in-process の test など)は `get_status` で `not_run`、`verify_assets` / `update_assets_manifest` で `assets_verification_unavailable@1` になります。test は [`assetsverification_test.cpp`](../../test/assetsverification_test.cpp#L161) の AV-1 の 4 本(`RpcServer::processLine` で往復。worker は `holdForTesting()` / `waitForTesting()` で止めて `running` を見る)です。

**hash の速さ(✅AH-1、`0f93b5b8`)。**cache が外れた file の hash は [`assetFileSha256()`](../../src/project/assetsmanifest.cpp#L162) で、file を 1 MiB の buffer で読みます。AH-1 はこの関数の中で、Windows では CNG(`BCryptCreateHash(BCRYPT_SHA256_ALG_HANDLE, ...)` / `BCryptHashData` / `BCryptFinishHash`)、それ以外では picosha2 の `hash256_one_by_one` に chunk ごとに渡していました。今は FW-1(`92681afd`)がその計算を [`Sha256Stream`](../../src/project/sha256stream.hpp#L15)(`src/project/sha256stream.cpp`。Windows では BCrypt、それ以外では picosha2)に移し、file watcher と共有しています。1 MiB は `Sha256Stream::read_chunk_bytes` です。`pelican_project` は Windows で `bcrypt` を link します(`src/project/CMakeLists.txt`)。どちらも小文字の 16 進の SHA-256 で、digest は前と同じです。前は picosha2 に `istreambuf_iterator` で 1 byte ずつ渡しており、commit の記述では hash cache の無い Debug の `projects/example`(約 70 MB)で 23〜31 s、変更後は 0.5〜0.75 s です。前の時間は Studio が engine の窓を見つけるまでの待ち(15 s)より長く、cache の無い worktree で Studio の test を走らせると engine が kill されていました。

hash を始める前に、[`verifyAssetsManifest()`](../../src/project/assetsmanifest.cpp#L574) は `AssetManifestVerifyOptions::before_hashing`(`include_content` のときだけ)を 1 回呼び、manifest の file のうち実在するものを cache が当たるかどうかで数えた [`AssetManifestHashPlan`](../../src/project/assetsmanifest.hpp#L56) `{to_hash, bytes_to_hash, cached}` を渡します([数える所](../../src/project/assetsmanifest.cpp#L605))。cache の当たりの判定は、hash の時に使う `cachedHash()`(size と mtime が合い、値が 16 進の SHA-256)と同じです。起動時の照合(`verifyAssetStores(..., log = true)`)はそれで `assets manifest [<store>] verifying <N> assets (<X> MB), <M> cached` を INFO で出します(`to_hash` が 0 なら出しません。[出す所](../../src/core/loader/assetsverification.cpp#L54))。✅AV-1 から同じ callback が `to_hash` / `cached` を store の結果の `hashed` / `cached` に残し、`verify_assets` の再検証(`log = false`)は残すだけで出しません。test: `assetsmanifest_test` の [「asset file hashing matches SHA-256 for empty, small, and multi-chunk files」](../../test/assetsmanifest_test.cpp#L69)。`Sha256Stream` そのものは FW-1 の 2 本が見ます: `assetsmanifest_test` の [「FW-1 Sha256Stream streams across the 1 MiB read unit like one-shot SHA-256」](../../test/assetsmanifest_test.cpp#L91)(hashlib で求めた値と比べ、2 回目の `finishHex()` は throw)と、`filewatcher_test` の [「FW-1 Sha256Stream, assetFileSha256 and the watcher digest equal picosha2 around 1 MiB」](../../test/filewatcher_test.cpp#L668)(長さ 0 / 3 / 1 MiB の前後 / 2 MiB+7 を、一括・不揃いの分割・`assetFileSha256()`・`readStableContentDigest()` の 4 経路で picosha2 と比べる。assetsmanifest_test の実行ファイルは picosha2 の include を受け取っていないので、こちらに置いた)。

manifestの生成も同じ`src/project/assetsmanifest.cpp`にあり、`pelican_cli assets manifest`から利用されます。

## 3.5 Sceneのロード

sceneは実装を追う価値の高い、純粋層とruntime層の典型です。

### 1. envelopeと構文を正規化

[`normalizeSceneDataJson()`](../../src/project/sceneformat.cpp#L268) が次を検証します。

- `schema == "pelican.scene"`
- `version == 1`
- `scenes`がobject
- scene ID、object名、component名が`[a-zA-Z0-9_]`
- object名がscene内で一意
- 各objectに`components` arrayが存在
- `parent`（親子transform）が識別子であること。未知parent・曖昧parent・循環はエラー（[sceneformat.cpp](../../src/project/sceneformat.cpp#L170)、fixture: `scene_unknown_parent` / `scene_parent_cycle` / `scene_ambiguous_parent`）
- v1で禁止された旧`lights` fieldを拒否

この時点ではECS型やGPUには触れません。

> 🧩 **難所 — 3値のDFSが名前を出す**([`normalizeScene()`](../../src/project/sceneformat.cpp#L135) 内の visit ラムダ [本体](../../src/project/sceneformat.cpp#L233))
>
> **何をする所か**: `parent` エッジをたどって親子関係の循環を検出し、循環に参加しているobject名を並べたメッセージでthrowします。
>
> **素朴に読むと**: `auto &&self` を引数に取って `self(self, i)` で再帰する書き方が回りくどく見えます。generic lambda(引数型が `auto` のラムダ)は本体を書いている時点で自分の型が確定していないので、キャプチャ経由で自分を呼べません。`std::function<void(size_t)>` にすれば書けますが、型消去とヒープ確保が入り、純粋パーサ層にわざわざ間接呼び出しを増やすことになります。自分を第1引数で受け取ればキャプチャは参照のままで、インライン化も効きます(これを不要にする C++23 の deducing this は使えません — `pelican_project` は C++20 ビルドです、[src/project/CMakeLists.txt](../../src/project/CMakeLists.txt#L33))。もっと本質的なのは `state` が `0/1/2` の3値である点です。「訪問済み」を1値でしか持たないと、複数の子が同じ親を指す形(親リンクは森なので普通に起きます)で2度目の到達を循環と誤報するか、区別を諦めて毎回スタックを線形探索する羽目になります。1=いま辿っているスタック上、2=走査済みで安全、を分けて初めて `state[parent] == 1` だけが循環になります。さらに `stack` を別に持つのは「循環がある」ではなく `NodeA -> NodeB -> NodeC -> NodeA` と**実際の名前の並び**をエラーに載せるためで、scene作者に対する診断の質がこの構造の目的です。エッジの向きが子→親である点も、`std::find` で循環の始点を拾える前提になっています。
>
> **骨子**:
> ```text
> state[i] = 1; stack.push(i)
> parent あり かつ state[parent] == 0 -> self(self, parent)
> parent あり かつ state[parent] == 1 -> stack の parent 位置から末尾までを " -> " で連結して throw
> stack.pop(); state[i] = 2
> ```
>
> **手がかり**: 「未知parent」と「曖昧parent」はDFSより**前**の別ループで弾いてあるので([前段の検査](../../src/project/sceneformat.cpp#L199))、DFS中の `object_index_by_name.at(parent_name)` は必ず成功します。名前の無いobjectは `object_index_by_name` に載らないため親になれません。テストは [`sceneformat_test.cpp` 内](../../test/sceneformat_test.cpp#L126) で、`NodeA` / `NodeB` / `NodeC` の3名がメッセージに出ることまで検査します。
>
> **不変条件**: 3値stateの `1` と `2` を統合しない。エラーには循環に参加する名前の並びを載せる(「循環あり」だけにしない)。重複名・未知親・曖昧親の検査はDFSより前に済ませる。

### 2. runtime用にcomponentを分類

[`prepareSceneBindings()`](../../src/core/loader/scene.cpp#L305) はcomponentを四系統へ分けます。

- `light`: ECSへ入れず`LightLoadEntry`へ
- `collider`: `ColliderComponent`として検証し、後で`PhysWorld`へ。`PELICAN_WITH_PHYSICS` OFFのビルドでは、colliderを含むsceneはprepareで `physics.module_unavailable@1` です（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L74)）。判定は codec の `required_feature`（collider は `physics`）を見る [`resolvedObjectFeatureRefusal()`](../../src/core/loader/resolvedscene.cpp#L78) で、animation と同じ検査を editor の `add_component` / `spawn` も通ります（PG-1。以前の散文の `BuildFeatureDisabledError` は scene 側から消えました）
- `behavior`: ECSへ入れず、[`BehaviorAttachmentArena`](../../src/core/gamelogic/behaviorarena.hpp#L213) へ（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L382) で特別扱い）
- その他: `ComponentInfoManager`で文字列名から`ComponentId`へ

このためsceneの見た目はcomponent配列でも、現在のruntime実装ではlight/collider/behaviorがECS Chunkに保存されるわけではありません。colliderをECSへ入れない理由は [`docs/design_ecs_lifecycle.md` §4](../design_ecs_lifecycle.md) の決定です — 「PhysWorld は『書き換え → 即 raycast で新位置』という既存契約を維持する」ため、PhysWorld 側が `Binding{name, collider, transform_source}` を持ち続け、query のたびに transform だけを ECS から resolve する形にしてあります（名前の重複検査とraycast結果の公開名がこの形に依存している、と同節が明記）。実装側の形は [§5.7 の World binding 層](05_gameplay_and_services.md) にあります。behavior が component でない理由も同種で、[`docs/design_object_behaviors.md` §0](../design_object_behaviors.md) が「behavior の正本 = engine-owned attachment arena。ECS component ではない。light/collider と同じ special dispatch の仲間」と決めています。

#### behaviorコンポーネント ✅実装済み

behaviorはobject単位に束ねてから渡します。これは §3.5 の executor の **prepare 段**で、resolved projection から読みます（[prepare 段の呼び出し](../../src/core/loader/scene.cpp#L997)）。

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

受理規則は [`prepareResolvedSceneBehaviorAttachments()`](../../src/core/gamelogic/behaviorarena.cpp#L172) にあります（raw JSON を受ける [`prepareSceneBehaviorAttachments()`](../../src/core/gamelogic/behaviorarena.cpp#L95) も同じ規則です）。

| 状況 | 結果 |
|---|---|
| `"type"` が無い / 文字列でない / 空文字列 | エラー `behavior on object '<name>' requires a non-empty string type` |
| DLLロード済みで、未登録の型名 | エラー `Unknown behavior type '<type>' on object '<name>'` |
| DLL不在で、未登録の型名 | warningを出して **pending** 扱いで続行 |

ここでの **pending** は「宣言だけ arena に載っていて、`Behavior` の実体がまだ作られていない attachment」です（[`PreparedSceneBehaviorAttachment::pending`](../../src/core/gamelogic/behaviorarena.hpp#L57)）。activate 段は pending を飛ばし（[`activatePublished()` の先頭の分岐](../../src/core/gamelogic/behaviorarena.cpp#L948)）、毎フレームの update も `active` なものだけを回す（[`BehaviorAttachmentArena::update()`](../../src/core/gamelogic/behaviorarena.cpp#L1173)）ので、`onInit` / `onUpdate` はどちらも走りません。型名と生JSONは保持されるので、次に scene load か game-logic reload の reconciliation を通ったとき、その時点の registry で `pending` が付け直されます（[reconciliation の再判定](../../src/core/gamelogic/behaviorarena.cpp#L776)）。

**眠った attachment は pending とは別物です（✅PM-1b）。**`edit` 状態（`--start-state edit` か `set_play_state {state: "edit"}`）では、arena 全体に眠りの flag が立ちます（[`BehaviorAttachmentArena::setDormant()`](../../src/core/gamelogic/behaviorarena.cpp#L818)）。眠っている間の scene load・reconciliation・spawn・editor の attach は、attachment を handle・attachment_seq・type・owner・正規化した params 付きで作りますが、instance を作りません（上のリンクの行の `!dormant_` と、[`activatePublished()` の冒頭](../../src/core/gamelogic/behaviorarena.cpp#L940)・[reconciliation の activate の先頭](../../src/core/gamelogic/behaviorarena.cpp#L581) で何もせず戻る）。`update()` と `dispatchEvent()` も入口で戻るので、眠った attachment を起こす道はありません。editor の `set_params` は保存した params を替えるだけです。`pending` は付かないので、editor はこれを編集の正しい相手として扱います。眠りを解いただけでは instance はできず、次の scene load（Play のときの作り直し）の activate 段で constructor と `onInit` が走ります。眠りを切り替えるときは、game のコードが積んだ遅延ミューテーションを捨てます。`edit` で読み込んだ runtime の `scene_start_time` は、今の時刻ではなく 0 です（`LoaderStateParticipant::prepare`）。

behavior は executor の participant の 1 つです。entity を作った後に `BehaviorAttachmentArena::prepareSceneReconciliation()` で candidate を作り（[early_publish 段末尾の呼び出し](../../src/core/loader/scene.cpp#L1539)）、publish 段で `noexcept` に arena へ swap し（[`behavior_candidate->publish()`](../../src/core/loader/scene.cpp#L1546)）、**activate 段**で `onInit` を走らせます（[`behavior_candidate->activate()`](../../src/core/loader/scene.cpp#L1548)）。activate は executor で**唯一の失敗しうる段**で、`onInit` が投げると executor の [`rollback` ラムダ](../../src/core/loader/scene.cpp#L1734) が **全 participant** を旧世代へ戻し、結果は `PreCommitFailure{phase = activate}` になります（[activate 失敗の戻り値](../../src/core/loader/scene.cpp#L1789)）。document / projection / current scene / authority は commit 前なので旧世代のまま、arena も旧 attachment が戻ります。旧経路の [`clearRuntimeScene()`](../../src/core/loader/scene.cpp#L1939) は定義が残っていますが `load()` からは呼ばれません。

> **設計決定:** game logic DLLが無い状態でsceneを開くのは、ツールや検証では正常なケースです。そこで「DLLが有るのに型名が引けない」ときだけfailにし、DLL不在は保留にしています。ロード可否がビルド構成に依存してぶれない、という線引きです。

実物のscene JSONは次の形です（[`test/run_physics_trigger_behavior.ps1`](../../test/run_physics_trigger_behavior.ps1) が書き出すfixtureからの引用）。

```json
{"schema":"pelican.scene","version":2,"scenes":{"default_scene":{"objects":[
  {"name":"TriggerZone","components":[
    {"name":"transform","pos":[0,0,0]},
    {"name":"collider","shape":"sphere","radius":1.0,"trigger":true},
    {"name":"behavior","type":"wp179_trigger_behavior"}]},
  {"name":"Target","components":[
    {"name":"transform","pos":[1,0,0]},
    {"name":"collider","shape":"sphere","radius":1.0}]}]}}}
```

### 3. scene load は 1 つの transaction（WP380a）

[`SceneLoader::load()`](../../src/core/loader/scene.cpp#L1650) は「旧 scene を消してから新 scene を作る」形ではありません。[`SceneLoadRequest`](../../src/core/loader/sceneload.hpp#L222)（committed projection を借りる `Borrowed`、または import の candidate と `MetadataCommitToken` を持つ `Owned`）を受け取り、[`SceneLoadResult`](../../src/core/loader/sceneload.hpp#L188)（`SceneLoadCommitted` / `PreCommitFailure` / `CommittedWithEffectFailure`）を返す **executor** で、段は次の順です。executor 本体は 18 個の [`SceneLoadParticipant`](../../src/core/loader/sceneloadparticipant.hpp#L55) を段ごとの順序表（`prepare_order` / `early_publish_order` / `publish_order` / `rollback_order` ほか）に沿って回す driver で、以前 `load()` のローカル変数だった候補群は [`SceneLoadContext`](../../src/core/loader/scene.cpp#L643) が持ち、fault 注入は [`SceneLoadMutationGate`](../../src/core/loader/sceneloadparticipant.hpp#L96) が一手に引き受けます。

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

段を記録するのは原則 executor ではなく participant 自身で、`SceneLoadPhaseTrace` へ書くのは metadata token（[`prepareRuntimeIdentity()`](../../src/core/loader/basicconfig.cpp#L549) が prepare、[`commit()`](../../src/core/loader/basicconfig.cpp#L584) が commit_metadata）、renderer（[`publishPreparedScene()`](../../src/core/renderer/polygoninstancecontainer.cpp#L601) が early_publish、[`enqueueSceneRetirement()`](../../src/core/renderer/polygoninstancecontainer.cpp#L747) が retire）、SeqPlayer / VatPlayer / Camera / Light / Physics の publish（いずれも trace を取る scene 専用 overload。例: [`Camera::publishPrepared()`](../../src/core/renderer/camera.cpp#L368)）、behavior arena（[`publish()`](../../src/core/gamelogic/behaviorarena.cpp#L558) / [`activate()`](../../src/core/gamelogic/behaviorarena.cpp#L620) / [`finish()`](../../src/core/gamelogic/behaviorarena.cpp#L637) が publish / activate / retire）、retirement queue（[`RetirementState::enqueue()`](../../src/core/loader/scene.cpp#L153)）、RNG（[`publishPreparedSequence()`](../../src/core/userpublic/deterministicrng.cpp#L126) が flush）、event outbox（[`PreparedEventOutbox::flush()`](../../src/core/userpublic/details/event/registerer.cpp#L279)）です。executor 自身が書くのは 2 か所だけで、pending swap 直前の [`record(SceneLoadPhase::flush)` の 1 行](../../src/core/loader/scene.cpp#L1800) と、created-entity batch を 1 participant と見なす [`record(SceneLoadPhase::finish)` の 1 行](../../src/core/loader/scene.cpp#L1808) です（WP380d。token 個別の `finish()` は記録しないので、`objects: []` の空 scene でも finish が列に出ます）。結果は [`SceneLoadCommitted::phase_trace`](../../src/core/loader/sceneload.hpp#L178) として返り、[`record()`](../../src/core/loader/sceneload.hpp#L47) は直前と同じ段なら足さないので、同じ段を複数の participant が通っても列は 1 つにまとまります。枠は 8 段ぶんの固定確保で、9 件目以降は列に入らない代わりに [`overflow`](../../src/core/loader/sceneload.hpp#L45) が立ちます（黙って落としません）。8 という数は [`SceneLoadPhase`](../../src/core/loader/sceneload.hpp#L27) の値数そのもの（[`sceneLoadPhaseCount = 8`](../../src/core/loader/sceneload.hpp#L38)）で、`record()` が直前と同じ段を足さない以上、段が上の順に1回ずつ現れる限り必ず収まります。つまり `overflow` が立つのは「一度離れた段へ戻った」ときだけで、確保を増やすべき状況ではなく**順序が壊れた**という信号です。`SceneLoadPhaseTrace` のコメントもそう書いています — `A trace is a fixed pre-commit allocation. Never silently discard an ordering observation: callers must reject an overflowing trace.`

**prepare**（[prepare 段の先頭](../../src/core/loader/scene.cpp#L1741) から [`return fail(SceneLoadPhase::prepare, ...)`](../../src/core/loader/scene.cpp#L1745) まで）では、candidate authority を 1 回だけ消費し（[`next_authority_epoch++`](../../src/core/loader/scene.cpp#L1666)。失敗しても再利用しません）、metadata token に scene id と authority を書き（[`prepareRuntimeIdentity()`](../../src/core/loader/scene.cpp#L621)）、resolved scene を検証して（[`validateResolvedSceneFeatureBindings()`](../../src/core/loader/scene.cpp#L840)）、各 participant の candidate を live に触らずに作ります: renderer（[`prepareSceneReplacement()`](../../src/core/loader/scene.cpp#L1044)。model の staging もここ）、camera（[`prepareSceneCameras()`](../../src/core/loader/scene.cpp#L1111)）、light（[`LightContainer::prepareLoad()`](../../src/core/loader/scene.cpp#L1147)）、SeqPlayer / VatPlayer（[`prepareSceneLoad()`](../../src/core/loader/scene.cpp#L1174)）、retirement queue の容量予約（[`reserveOne()`](../../src/core/loader/scene.cpp#L151)）、RNG 列（[`prepareSequence()`](../../src/core/loader/scene.cpp#L1566)）、event outbox（[`PreparedEventOutbox`](../../src/core/loader/scene.cpp#L1603)。`SceneLoaded` の payload はここで確保します）。model は [`ModelAssetContainer::getModelTemplateByName()`](../../src/core/loader/scene.cpp#L1056) で引きますが、`ModelAssetContainer` は **participant ではなく warm cache** です。candidate が失敗しても lazy load された model record / GPU resource は残り、戻しません（manifest の [`model_asset` / `warm_cache` 行](../../src/core/loader/sceneparticipantmanifest.cpp#L181)。どの scene にも束縛されない record なので世代混在にはなりません）。

**early_publish**（[`renderer.publishPreparedScene()`](../../src/core/loader/scene.cpp#L1085) から [`return fail(SceneLoadPhase::early_publish, ...)`](../../src/core/loader/scene.cpp#L1770) まで）では、ECS entity を [`ECSEntityMutation::prepareCreate()`](../../src/core/loader/scene.cpp#L1258) の populate callback で作って `publish()` し（JSON は各 Component へここでロード）、名前 binding の candidate を集め、親子 transform を投影し（§4）、physics の candidate を作り（[`PhysWorld::prepareSceneBindings()` の呼び出し](../../src/core/loader/scene.cpp#L1485)）、behavior の candidate を作り（[`prepareSceneReconciliation()`](../../src/core/loader/scene.cpp#L1539)）、旧 entity を [`prepareDestroy()`](../../src/core/loader/scene.cpp#L1423) で unpublish します。ここで投げると [`rollback` ラムダ](../../src/core/loader/scene.cpp#L1734) が inverse token を逆順に消費して戻します。

**publish** は [`static_assert(noexcept(...))`](../../src/core/loader/scene.cpp#L1774) で固定された swap の列です（[`object_bindings.swap()`](../../src/core/loader/scene.cpp#L719) → SeqPlayer → VatPlayer → [`Camera::publishPrepared()`](../../src/core/loader/scene.cpp#L1117) → Light → [`PhysWorld::publishPrepared()`](../../src/core/loader/scene.cpp#L1491) → [`behavior_candidate->publish()`](../../src/core/loader/scene.cpp#L1546)）。WP380d 以降、executor が呼ぶのは `SceneLoadPhaseTrace &` を取る scene 専用 overload だけで、`publish` を記録するのは呼ばれた participant 自身です（[`Camera::publishPrepared()`](../../src/core/renderer/camera.cpp#L368) / [`PhysWorld::publishPrepared()`](../../src/core/phys/physworld.cpp#L387)）。rollback 側は同じ swap を trace 無しの overload で戻すので、戻した分が `publish` として trace に増えることはありません。この段は失敗しないので、`PreCommitFailure::phase` に `publish` はありません（[`SceneLoadPhase`](../../src/core/loader/sceneload.hpp#L27) 自体は 8 値ですが、`PreCommitFailure` に載りうるのは `prepare` / `early_publish` / `activate` の 3 つだけです）。

**activate** は [`behavior_candidate->activate()`](../../src/core/loader/scene.cpp#L1548) の 1 行で、`onInit` はこの時点で **全 participant が candidate に切り替わった状態**を観測します。ここで投げると同じ `rollback` ラムダが全 participant を旧世代へ戻し（[`PreCommitFailure{phase = activate}`](../../src/core/loader/scene.cpp#L1789)）、document / projection / current scene / active authority は commit 前なので旧世代のままです。activate 中の `emit` / RNG / `loadScene()` は live queue ではなく candidate（[`EventOutboxScope`](../../src/core/loader/scene.cpp#L1758) / [`RngSequenceScope`](../../src/core/loader/scene.cpp#L1761) / [`PendingSceneTransactionScope`](../../src/core/loader/scene.cpp#L225)）へ向くので、rollback で一緒に捨てられます。

**commit_metadata** は [`metadata.commit()`](../../src/core/loader/scene.cpp#L635) の 1 回で、`SceneMetadataState` を丸ごと swap します（§3.2）。以降の段は [`static_assert(noexcept(...))`](../../src/core/loader/scene.cpp#L1792) で `noexcept` が固定されており、`PreCommitFailure` は出ません。

**retire** は旧 state を消すのではなく queue に積むだけです。旧 entity の destroy token と transient model は [`retirement_state->enqueue()`](../../src/core/loader/scene.cpp#L754)（容量は prepare で予約済み）、renderer の旧 instance slot は [`enqueueSceneRetirement()`](../../src/core/loader/scene.cpp#L1093) へ入り、消費は次フレームです: [`drainRetirementQueue()`](../../src/core/loader/scene.cpp#L1912) が [`applyPendingLoad()` の先頭](../../src/core/loader/scene.cpp#L1870) と teardown の ecs 段（[`teardown.cpp` 内](../../src/core/appflow/teardown.cpp#L80)）で走り、renderer 側は [`drainSceneRetirements()`](../../src/core/renderer/polygoninstancecontainer.cpp#L760) が `triggerUpdate()` の先頭で消費します。そこでの失敗はフレームのエラーであって transaction のエラーではありません（state は既に一貫）。

renderer の slot には、frame を挟まない連続の load の道もあります(`8bde3c2f`。利用者の codex の変更、handoff 05)。RPC の `load_scene` を続けると、前の load が retire した世代が `triggerUpdate()` を通らずに queue に残ります。[`prepareSceneReplacement()` の先頭](../../src/core/renderer/polygoninstancecontainer.cpp#L576) は、queue が空でなければ `VulkanManageCore::waitIdle()` で GPU の読み手の完了を待ってから回収し、それから次の候補の slot を取ります。公開中の世代は commit まで回収しません。あわせて物理的な slot の数 `maxModelInstances` を `2 * maxActiveModelInstances`(2,048)にし、同時に生きている数の上限 `maxActiveModelInstances`(1,024)は [staging の容量の検査](../../src/core/renderer/polygoninstancecontainer.cpp#L637) が `liveCount()` で見ます。候補を作る間も公開中の世代の slot が rollback のために残るので、前は旧と新の合計が 1,024 を超える置き換え(約 1,000 model のシーンの最初の再読込)が `Model instance capacity exceeded` で失敗していました(codex の報告)。device の buffer は 18.2 MiB 増えます(skin palette の 2 本で 16 MiB。commit の説明の値)。test: `modelinstance_slotmap_test` の [「Full-capacity scene replacement keeps rollback slots and reclaims older generations」](../../test/modelinstance_slotmap_test.cpp#L390)。

**flush** は [`stageReserved()`](../../src/core/loader/scene.cpp#L1612) で prepare 済みの `SceneLoaded` を予約 slot へ移し、RNG 列と pending scene を publish してから [`event_outbox->flush()`](../../src/core/loader/scene.cpp#L1618) で live queue へ `noexcept` に push します。

**effect transaction**（audio / persistence / `loadScene` などの副作用）は、この段の列と同じ寿命で 1 つだけ開きます。[`EffectTransactionScope`](../../src/core/loader/scene.cpp#L1759) は prepare の直後・`early_publish` に入る**前**に張られるので、component の JSON loader が早期 publish 中に呼ぶ副作用も、behavior の `onInit` と同じ transaction-local な sink を観測します（[executor 側のコメント](../../src/core/loader/scene.cpp#L1748)）。実際の適用は flush で、[`effect_transaction->flush()`](../../src/core/loader/scene.cpp#L1644) が staging 時と同じ sequence 順（`0..next_sequence_`）に entry を消化します。したがって pre-commit で投げれば副作用は 1 つも出ず、scene と一緒に巻き戻ります。sink 側（どの公開 API がこの transaction を読み、何が shadow になり何が拒否されるか、coalesce の規則）は [§5.15](05_gameplay_and_services.md) にまとめてあります。

診断の記録は **allocation 禁止** です。[`record()`](../../src/core/loader/effecttransaction.cpp#L97) は [`errors_`](../../src/core/loader/effecttransaction.hpp#L142)（`sceneEffectCapacity + 1` 個の [`FixedEffectError`](../../src/core/loader/effecttransaction.hpp#L124)）の固定 slot を 1 つ取って、文字列も `FixedText<512>` / `FixedText<384>` へ `memcpy` するだけで、`std::string` も `json` も作りません。末尾の 1 slot は診断用ではなく **outbox 溢れ専用**で、[`reserveEntry()`](../../src/core/loader/effecttransaction.cpp#L119) が 256 件を超えて積めなかった**副作用の**件数を `effect_outbox_full@1` の `dropped` として数え続けます（診断そのものが 256 件を超えた場合は数えずに落ちます。非対称の理由は §5.15）。公開用の文字列と JSON は commit 後に初めて作られます — [`effectErrors()`](../../src/core/loader/scene.cpp#L1815) の呼び出しは `noexcept` の commit tail（`flush` / `finish`）を通過した**後**にあり、そこで固定 slot を sequence 順に並べて [`CommittedWithEffectFailure::EffectError`](../../src/core/loader/sceneload.hpp#L176) へ変換します。

副作用の失敗は例外ではなく [`CommittedWithEffectFailure`](../../src/core/loader/sceneload.hpp#L174) として `SceneLoadResult` に載り、**扱いは root ごと**です: startup は [`LOG_ERROR`](../../src/core/userpublic/pelican_core.cpp#L138)、pending も [`LOG_ERROR`](../../src/core/loader/scene.cpp#L1883)、rpc の `load_scene` は [`response["effect_errors"]`](../../src/core/communication/rpcserver.cpp#L2043) として JSON で返し（失敗が無ければ空配列）、import は [`SceneImportResult`](../../src/core/loader/sceneload.hpp#L275) にそのまま入れて呼び手へ渡します。いずれも `committed` であることは変わりません（scene は載っている）。

transient glTF 経路（`load_gltf`）は executor を通らず、従来どおり [`GameObjects::createWithComponents()`](../../src/core/loader/scene.cpp#L2248) の populate callback で作ります（§3.8 の「`load_gltf` はトランザクション」）。

### 4. authored local TRSのworld投影

ECS objectを作った後、同じ early_publish 段の中で transform を持つ全objectのworld解決を済ませます（[transform node の投影](../../src/core/loader/scene.cpp#L1313)）。behavior の `onInit`（activate 段）や `SceneLoaded` の配送（flush 段）より前です。

GP-1 から、投影の対象は「親子に参加するobject」ではなく **`transform` を持つ全object（`transform_node`）** です。scene に `parent` が1つも無くても同じ経路を通ります。各objectの `LocalTransformComponent` へ親リンクを書き、[`resolveParentOrder()`](../../src/core/loader/scene.cpp#L1358) で親が先に来る順に並べ、transform codec の `runtime_apply` で local を書いてから world を合成します。rootは親を持たないので、その local は authored の TRS そのものになります。

合成は [`composeWorldTransform()`](../../src/core/loader/componentcodec.cpp#L2342) で、`LocalTransformSystem` も同じ関数を呼びます（[`composeFromLocal()`](../../src/core/ecs/predefined/localtransformsystem.cpp#L14)）。したがって `SceneLoaded` を受け取ったコードは、まだ1フレームも回っていない時点でも world transform を問い合わせられ、最初のフレームの再計算でも同じ world に着地します。この関数は回転を正規化します（`normalize(parent.R * normalize(local.R))`）。GP-1 以前の `LocalTransformSystem` は正規化せずに掛けていたので、親子のある scene の毎フレームの world 回転は、以前と下位 bit が変わることがあります。

> 🧩 **難所 — preserve-worldの逆算**([`inverseLocal()`](../../src/core/loader/editorprojectiontransaction.cpp#L288) / [`makeReparentCommand()`](../../src/core/loader/editorprojectiontransaction.cpp#L588))
>
> **何をする所か**: 同じ漸化式の逆向きです。エディタが `preserve: "world"` で親を付け替えるとき、新しい親のworld TRSから「worldを保ったままの子のlocal TRS」を逆算します。
>
> **素朴に読むと**: `local = parent^-1 * world` を書くだけに見えます。ところがTRS(平行移動・回転・非一様スケール)は逆演算に対して**閉じていません**。親が非一様スケールと回転を同時に持つと、真の相対変換はせん断を含み、正準TRSでは表現できません。せん断が出る仕組みは合成側([`composeWorld()`](../../src/core/loader/editorprojectiontransaction.cpp#L157))を見ると分かります — `world.pos = parent.pos + parent.R * (parent.S * local.pos)` / `world.R = parent.R * local.R` / `world.S = parent.S * local.S` で、**親のスケールは親の軸に沿って**掛かります。子が回転していると伸縮の軸と子の軸が揃わないので、子の直交していた軸が斜交します。これがせん断で、「回転させた直交軸に沿った軸別スケール」しか書けない正準TRSの表現範囲の外です。素朴に計算しても数値そのものは出るので、壊れ方は「公開した瞬間に物体が歪む/ずれる」という無音の形になります。だからこの関数は計算して終わりではなく、**逆算 → もう一度合成 → 元のworldと一致しなければ `TransformUnrepresentable` で拒否** という表現可能性の検査になっています。検査の位置も一律ではありません。除算の**前**に並ぶゼロスケール判定と親TRSの有限性判定([この2つ](../../src/core/loader/editorprojectiontransaction.cpp#L300))は `inf` / `NaN` を作らないためのガードで、クォータニオンのノルム判定([`norm_squared` の算出](../../src/core/loader/editorprojectiontransaction.cpp#L328))は除算の**後**に来る表現可能性検査の一部です — 前者は `TransformZeroParentScale` / `TransformNonFinite`、後者は `TransformUnrepresentable` と、出るエラーコードも別です。ただし「非有限の検査は除算前だけ」ではありません。逆算した結果そのものの有限性も除算の後にもう一度見ていて([結果側の判定](../../src/core/loader/editorprojectiontransaction.cpp#L323))、こちらのエラーコードは `TransformNonFinite` です。
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
> **手がかり**: 許容差のラムダ `32.0F * epsilon * max(1, |left|, |right|)` は**相対**許容差で、絶対値の大きい座標でも桁落ちで誤検出しません(下限 `1.0F` があるので原点近傍では絶対許容差として働きます)。`makeReparentCommand()` は [`buildTransformNodes()`](../../src/core/loader/editorprojectiontransaction.cpp#L214) を **3回** 呼びます — 変更前worldの採取、親エッジ書き換え後(コメント通り「staged graphそのものがサイクルのpreflight」)、local差し替え後の事後条件証明です。[`projectNodes()`](../../src/core/loader/editorprojectiontransaction.cpp#L183) は共有の `resolveParentOrder()`(fresh / active / done の白灰黒DFS)で歩き、active へ再入したらそれが親サイクルです。テストは [`Transform projection recomputes descendants and reparent policies`](../../test/editorprojectiontransaction_test.cpp#L628)。
>
> **不変条件**: 再合成の検証はadapterのprepareより**前**に済ませる(ランタイムへ触った後に「実は表現できなかった」を出さない)。許容差を緩めると、無音でずれたreparentが通るようになります。

### 5. 名前binding

名前付きかつ`transform`を持つobjectだけが `object_bindings`へ入ります。executor は live の map には書かず、early_publish 段で `candidate_object_bindings` に集め（[candidate への emplace](../../src/core/loader/scene.cpp#L706)）、publish 段で [`object_bindings.swap()`](../../src/core/loader/scene.cpp#L719) します（rollback は逆向きの swap）。authoring id → entity の `runtime_object_bindings` も同じ形です。[`bindObjectTransform()`](../../src/core/loader/scene.cpp#L1971) は `load()` からは呼ばれません。RPC、camera controller、physicsはこの名前から世代付き`GameObjectId`を引き直します。

bindingは生ポインタを保持しません。アクセス時に`tryComponent<TransformComponent>()`でIDのgenerationとComponent存在を再検証します。

### 6. SceneLoaded event

[`SceneLoaded`](../../src/core/userpublic/events.hpp#L10) は「ロード完了後にその場で emit」ではありません。payload（`make_shared` と vector の容量）は prepare 段で [`event_outbox->prepare()`](../../src/core/loader/scene.cpp#L1605) が確保し、commit 後の flush 段で [`stageReserved()`](../../src/core/loader/scene.cpp#L1612) → [`flush()`](../../src/core/loader/scene.cpp#L1568) が live queue へ `noexcept` に移します（[`PreparedEventOutbox`](../../src/core/userpublic/details/event/registerer.hpp#L324)。`stageReserved()` / `flush()` の実装は [`registerer.cpp` 内](../../src/core/userpublic/details/event/registerer.cpp#L261)）。activate 段で behavior が `emit` したものも同じ outbox に入り、`SceneLoaded` はその**後ろ**に並びます。rollback すると outbox ごと捨てられるので、失敗した load の event が旧世代の behavior に届くことはありません。フレーム末尾のpending loadから flush された場合、eventは次フレーム冒頭に届きます。

### 7. participant manifestとfault injector

上の段の列は「誰が何を持ち替えるか」を散らして持っているので、それを1枚の表として**runtime側に**持っているのが [`sceneParticipantManifest()`](../../src/core/loader/sceneparticipantmanifest.hpp#L76) です。テスト用の別ファイルではなく production の TU にあるので、テストと実装が別々の真実を持つことはありません。

1行が1つの**独立に観測できるフィールド**（またはビルドrootの1本）で、列は `owner` / `field` / `presence` / `role` / `prepare_owner` / `publication` / `rollback` / `retirement` / `snapshot_schema` / `mutation_id` です（[`SceneParticipantManifestEntry`](../../src/core/loader/sceneparticipantmanifest.hpp#L36)）。粒度を粗くしない理由はソースのコメントが書いています — `"renderer state"` のようにまとめた1行を作ると、後から足したカウンタがsnapshot比較にもmutationテストにも掛からずに逃げます（[そのコメント](../../src/core/loader/sceneparticipantmanifest.cpp#L79)）。

`presence` は `real` / `stub` / `absent` の3値で、**ビルド構成から導かれます**（[`SceneParticipantPresence`](../../src/core/loader/sceneparticipantmanifest.hpp#L14)）。機能を持つビルドでは `real` で、機能を外したビルドで `stub` と `absent` のどちらになるかは、[`feature_divisions` の表](../../src/core/loader/sceneparticipantmanifest.cpp#L23) の1箇所で決まります（PG-2）。表の1行が1つのビルドモジュールで（[`SceneFeatureDivision`](../../src/core/loader/sceneparticipantmanifest.hpp#L58)）、規則は1つです。

- 機能を外しても、その機能の**宣言が scene に来うる**なら `stub`。OFF ビルドは、その宣言を名前付きで拒否する部分だけを残します。何が scene に来て何が拒否するかは、行の `off_declaration` に書いてあります。
- 来えないなら `absent`。`off_declaration` は空です。

現在の割り当ては、`stub` が audio / animation / physics / vat / seqplayer、`absent` が xr / rpc / imgui です。participant 行は presence を行ごとに選ばず、[`presenceOf()`](../../src/core/loader/sceneparticipantmanifest.cpp#L58) でこの表を引きます。表に無い名前を引くと、`consteval` 関数の中の `throw` に評価が届くのでコンパイルが止まります。animation と imgui は自分の participant 行を持ちませんが、分類されないモジュールを残さないために表には行があります。表は [`sceneFeatureDivisions()`](../../src/core/loader/sceneparticipantmanifest.hpp#L77) で外から読めて、[PG-2 の契約テスト](../../test/scene_transaction_contract_test.cpp#L212) が「`stub` になるのは `off_declaration` が空でないときだけ」「codec に `required_feature` がある component のモジュールは `stub`」「participant 行の presence は表の値」を検査します。

以前は audio にだけ4値目の `uninstalled` がありましたが、PG-2 で消えました。audio を外したビルドでも scene の callback が音の effect（`playSound` / `stopSound` / `setBusVolume`）を積もうとすることはあり、effect transaction がそれを `build_feature_disabled@1` で拒否するので、audio は `stub` です。seqplayer の宣言は scene 文書ではなく起動オプション `--play-seq` から来ます。それでも `stub` に入れてあり、「scene に来うる」を厳密に読めば `absent` になりうる行です（PG-2 が残した論点）。

`role` は10値あり（[`SceneManifestRole`](../../src/core/loader/sceneparticipantmanifest.hpp#L20)、名前は [`sceneManifestRoleName()`](../../src/core/loader/sceneparticipantmanifest.cpp#L290)）、**executorの外に居るものも明示的に載る**のが要点です。

| role | 意味 | 例 |
|---|---|---|
| `participant` | 段の列に参加し、rollbackで戻る | `metadata.document`、`camera.*`、`ecs.entity_liveness` |
| `invariant` | 参加はするが値が変わらない | [`rng_sequence.increment`](../../src/core/loader/sceneparticipantmanifest.cpp#L176) |
| `cache` | participantではない。失敗しても戻さない | [`model_asset.warm_cache`](../../src/core/loader/sceneparticipantmanifest.cpp#L181) |
| `effect` | effect transactionのoutbox経由。flushで初めて外へ出る | [`audio.play`](../../src/core/loader/sceneparticipantmanifest.cpp#L183) / `persistence.save_data` |
| `non_rollback` | 一度消費したら戻さない | [`audio.handle_cursor`](../../src/core/loader/sceneparticipantmanifest.cpp#L186)（`u64-gap-named-v1` = 失敗すると番号に穴が空く、と明記） |
| `read_only` | outboxを重ねて読むだけ | [`persistence.load_data_overlay`](../../src/core/loader/sceneparticipantmanifest.cpp#L190) / `list_saves_overlay` |
| `reject` | transaction中は拒否する面 | [`gameobjects.direct_mutation`](../../src/core/loader/sceneparticipantmanifest.cpp#L192) / [`behavior_context.retired_self_setters`](../../src/core/loader/sceneparticipantmanifest.cpp#L193) |
| `frame_lifetime` | フレーム寿命でtransactionの外 | [`debug_text.frame_text`](../../src/core/loader/sceneparticipantmanifest.cpp#L194) |
| `production_root` | executorを呼ぶ**入口**の側 | [`root.startup` / `root.rpc` / `root.pending` / `root.import.versioned`](../../src/core/loader/sceneparticipantmanifest.cpp#L196)（`root.import.unversioned` は MG-2b で SnapshotV1 と一緒に消えました） |
| `excluded_wp380b` | 明示的な除外枠 | **現在この role の行は1つもありません**（enumと名前だけが存在） |

`mutation_id` は「この行が主張する挙動を、実際に壊せる注入点」の名前です（[`scene_load_mutation`](../../src/core/loader/sceneload.hpp#L92)）。埋まっているのは実質のparticipant行だけで、`invariant` / `cache` / `non_rollback` / `reject` / `frame_lifetime` / `read_only` と `presence != real` の行では**空でなければならない**、というのが契約テスト側の要求です（[`scene_transaction_contract_test.cpp` 内](../../test/scene_transaction_contract_test.cpp#L141)。`real` なparticipantでmutation付きの行はちょうど10という数まで固定されています）。ビルドで消える面のIDも条件式で空になります（例: `rpc_presence == real ? scene_load_mutation::root_rpc : std::string_view{}`）。

注入側は [`SceneLoadFaultInjector`](../../src/core/loader/sceneload.hpp#L74) の1メソッド `shouldMutate(phase, participant_name, mutation_id)` だけです。ここで使う [`SceneLoadFaultPhase`](../../src/core/loader/sceneload.hpp#L65) が `PreCommitFailure` の3値と**わざと別**である点が重要で、`prepare | publish | activate | commit | retire | flush` の6値、つまり `noexcept` のcommit末尾まで名前を持ちます（[そのコメント](../../src/core/loader/sceneload.hpp#L61)）。「外へ報告する段」と「壊せる辺」は同じ粒度ではない、という割り切りです。

判定のヘルパは [`sceneLoadMutationEnabled()`](../../src/core/loader/sceneloadfault.cpp#L30)（injectorが`nullptr`なら常に偽）と、rootごとの切断を引く [`sceneLoadRootDisconnected()`](../../src/core/loader/sceneloadfault.cpp#L38) で、後者は [`root_mutations`](../../src/core/loader/sceneloadfault.cpp#L16) の表で `SceneLoadRoot` からmutation IDを引きます。加えて `rollbackScene*()` の3本は、**わざと間違ったrollback**（outboxをflushしてしまう / RNG候補を公開してしまう / pending sceneを入れ替えてしまう）を実装したもので、ヘッダのコメントがそう宣言しています（[`sceneloadfault.hpp` 内](../../src/core/loader/sceneloadfault.hpp#L20)）。「正しい実装を消して赤を見る」のではなく、「間違った実装を名前付きで用意して、テストがそれを選ぶ」形です。

> 🧩 **難所 — 注入口はproductionバイナリに存在しない**([`SceneLoader::fault_injector`](../../src/core/loader/scene.hpp#L131) / [`test/sceneloadfault_testing.cpp`](../../test/sceneloadfault_testing.cpp#L5))
>
> **何をする所か**: fault injectorをloaderへ差す唯一の口です。差し口はメンバ変数 `fault_injector` と、staticな既定値 [`default_fault_injector_for_testing`](../../src/core/loader/scene.hpp#L133) の2つで、後者はconstructorが各インスタンスへコピーします（[constructorの初期化子](../../src/core/loader/scene.cpp#L165)）。
>
> **素朴に読むと**: 「テスト専用のsetterがproductionヘッダに宣言されている」ので、出荷バイナリにも入っていそうに見えます。実際には**宣言だけ**で、定義は `src/` の外の [`test/sceneloadfault_testing.cpp`](../../test/sceneloadfault_testing.cpp#L5) にしかありません。ヘッダ側は `namespace test_support` の2関数を前方宣言し、`SceneLoader` がそれを [`friend`](../../src/core/loader/scene.hpp#L138) にするだけです。したがって production の `pelican_core` をリンクしても、シンボルはどこにも現れません — 「無効化されている」ではなく「**存在しない**」が正確です。既定値がnullptrなので、production経路の `sceneLoadMutationEnabled()` は最初の `injector != nullptr` で必ず偽になります。
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
> **手がかり**: それを機械で確かめるのが [`wp380c_production_has_no_fault_setter_symbol`](../../test/CMakeLists.txt#L2497) で、検査は3種類あります。(1) `dumpbin /symbols` の出力に `SceneLoadFaultInjectorForTesting`（およびmodel release側の3つ）が現れないこと（[その `string(FIND)`](../../test/run_wp380c_no_production_fault_setter.cmake#L29)）。(2) test-onlyのTUが `pelican_core` の `SOURCES` に混ざっていないこと（[SOURCES 内の TU 照合](../../test/run_wp380c_no_production_fault_setter.cmake#L37)）。(3) productionヘッダ側に `set...ForTesting(...) {` という**定義**が書かれていないこと — 正規表現で本体つき定義を探します（[production header の列挙](../../test/run_wp380c_no_production_fault_setter.cmake#L87)）。3番目があるのは、ヘッダにinline定義を1行書くだけで(1)も(2)もすり抜けられるからです。この検査はWindows（`dumpbin` がある構成）限定です。
>
> **不変条件**: setterの定義をヘッダへ書かない（inlineでも）。test-onlyのTUを `pelican_core` のソースへ足さない。`default_fault_injector_for_testing` の既定値をnullptr以外にしない。

### load 後: sceneが抱えるグラフ runtime ✅実装済み（P-1 / P-2b）

`pelican.graph` 文書の `graph.instance` ノードは `{scene, object}` という **authoring 上の宛先**（[`GraphInstanceOwner`](../../src/project/graphinstanceowner.hpp#L9)）を持ちます。これをこのフレームの生きたオブジェクトへ結び付けるのが [`src/core/loader/scenegraphruntime.cpp`](../../src/core/loader/scenegraphruntime.cpp#L40) で、無名名前空間の小さな補助 [`parameterValues()`](../../src/core/loader/scenegraphruntime.cpp#L21) を除けば `SceneLoader` のメンバ関数だけが並ぶファイルです。scene側が持つのはグラフ本体ではなく、**グラフ1本あたりの runtime**（評価state 2つ、パラメータ上書き、鳴っている音）です。

**owner の解決**。[`graphOwnerObject()`](../../src/core/loader/scenegraphruntime.cpp#L40) は、今のsceneに属し、かつ `authoring_object_id` が一致して生きているオブジェクトを探します。見つからない scope は [`inactiveGraphScopes()`](../../src/core/loader/scenegraphruntime.cpp#L445) に入り、その scope に依存するノードは**そのフレームだけ評価されません**（エラーにはなりません。オブジェクトが再publishされる最中は正常な状態だからです）。

> ⚠️ ただし「この文書が指すオブジェクトはこのsceneに存在しない」は別の話で、こちらは静かに沈む種類の失敗です。[`reportUnresolvedGraphOwners()`](../../src/core/loader/scenegraphruntime.cpp#L59) が、一度も解決できていない scope を**グラフ世代の同期時に1回だけ**警告します（描画パス側で言うと、republish 中の正常な状態までログに流れるため）。記憶は `graph_owner_reported_scopes` に残り、scene load でだけクリアされます。v0 の既知の穴「authoring object id は保存 → 再読込をまたぐと別個体を指しうる」と組み合わさると、**警告1行のほかは何も起きずアニメーションだけが動かない**という形になるので、`graph.instance` が効かないときはまずこの警告を探してください。

**評価stateが2つある理由**は [第2章 §2.5](02_runtime_lifecycle.md) に書いたとおりです。[`graphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L149)（描画側）と [`poseGraphEvaluationState()`](../../src/core/loader/scenegraphruntime.cpp#L153)（姿勢側）は同じ `GraphRuntime` の別メンバで、[`prepareGraphRuntimeTransfer()`](../../src/core/loader/scenegraphruntime.cpp#L358) はグラフ世代が変わるとき両方をまとめて移送します（片方だけ置いていくと、描画側が走り続けたまま姿勢側のクロックだけが初期状態へ戻ります）。owner が変わった scope は移送後に `resetState()` で落とされます。

**publish snapshot**。描画側の評価が実際に各instance入力ポートへ何を流したかは、[`publishGraphValues()`](../../src/core/loader/scenegraphruntime.cpp#L144) が `graph_values`（[`PublishedInstanceInputs`](../../src/core/userpublic/valuegraph.hpp#L116)）として置いていきます。読み手がグラフを評価し直すとクロックが進みイベントが消費されるので、**描いた世代の値を読む道はこの1本だけ**です。snapshot は値の並び順を決めた `Graph` を `shared_ptr` で**保持**します（借用ではありません）—— 世代が入れ替わった後に読んだ人でも、その値に対応する名前が手元にあるようにするためです。

## 3.6 即時loadと要求load

- `SceneLoader::load(SceneLoadRequest)`はその場で全sceneを置換（commit まで到達した場合。§3.5 の 3）。production の root は startup（[`pelican_core.cpp` 内](../../src/core/userpublic/pelican_core.cpp#L151)）/ RPC `load_scene`（[`rpcserver.cpp` 内](../../src/core/communication/rpcserver.cpp#L2029)）/ pending apply / snapshot import（[`importSceneDocument()` からの `Owned` request](../../src/core/loader/basicconfig.cpp#L845)）で、いずれも [`requireSceneLoadCommitted()`](../../src/core/loader/scene.cpp#L500) で `PreCommitFailure` を従来どおりの例外へ戻します。
- [`requestLoad()`](../../src/core/loader/scene.cpp#L1838) は存在確認後、scene IDだけをpendingへ保存。activate 段の behavior から呼ばれた場合は live ではなく candidate projection に対して存在確認し、要求も candidate 側に積みます（rollback で一緒に捨てる）。
- [`applyPendingLoad()`](../../src/core/loader/scene.cpp#L1869) はフレームのゲーム更新末尾で、まず前回 commit 分の retirement queue を消費し、apply 時点の committed projection に late-bind した `Borrowed` request で実際にload。

`GameContext::loadScene()`はrequest型です。RPCの`load_scene`は即時loadです。この違いは、ゲームSystem update中にECS全削除が起きないようにするためです。

rootは列挙型として名前を持ちます（[`SceneLoadRoot`](../../src/core/loader/sceneload.hpp#L82)）。`unspecified` を除く4値がそのまま §3.5 の 7 の `production_root` 行に対応します。以前は import が snapshot schema version で `import_unversioned`（SnapshotV1）と `import_versioned` に割れていましたが、MG-2b で SnapshotV1 が撤去され、[`importSceneDocument()`](../../src/core/loader/basicconfig.cpp#L793) は常に [`import_versioned`](../../src/core/loader/basicconfig.cpp#L846) で request を作ります。

結果の扱いもrootごとに違います。startup / RPC / pending / import はいずれも [`requireSceneLoadCommitted()`](../../src/core/loader/sceneload.hpp#L295) を通して `PreCommitFailure` だけを例外へ戻しますが（ヘッダのコメントが規範 — 「pre-commit失敗は従来どおりthrow、commit済みのeffect失敗は成功なので呼び手へ返す」）、`CommittedWithEffectFailure` の運び方は分かれます。importは [`SceneImportResult`](../../src/core/loader/sceneload.hpp#L275) が `revision` と `load_result` を**組で**返すので、呼び手は「revisionは進んだが副作用が失敗した」を1つの値として受け取れます。`SceneRevision` への暗黙変換演算子があるため、revisionだけ見ていた既存の呼び手はそのまま動きます。startup と pending は `LOG_ERROR`、RPCは `effect_errors` 配列でJSONに載せます（§3.5 の 3）。

## 3.7 ComponentのJSONロード

sceneのcomponent名から型への変換は [`ComponentInfoManager`](../../src/core/ecs/componentinfo.hpp#L60) が担います。

1. runtime登録時に `name → ComponentId` を保存。
2. `ComponentId → ComponentInfo`からJSON loader callbackを得る。
3. [`ComponentInfoManager::loadByJson()`](../../src/core/ecs/componentinfo.cpp#L189) が`JsonArchiveLoader`を作る。
4. Componentの`ref(ar)`を型消去callback越しに呼ぶ。

`ref()`を持たないComponentはscene JSONからロードできず、明示エラーになります。JSON archiveの対応型は [`jsonarchive.hpp`](../../src/core/userpublic/serialize/jsonarchive.hpp#L12) に固定されています。

ただし `collider` と `sprite_view` の `ref()` は、**自前のパーサを持ちません**。どちらの `loadFromJsonArchive()` も [`requireComponentCodec()`](../../src/core/loader/componentcodec.hpp#L396) で codec を引き、`decode_authored` → `runtime_apply` に委譲します（[collider 側](../../src/core/userpublic/components/collider.cpp#L39) / [sprite_view 側](../../src/core/userpublic/components/spriteview.cpp#L30)）。受理仕様の実装が §3.11 の codec の1本だけになるので、この ECS 経路と scene loader 経路が別々に drift することはありません（失敗は `ResolutionFailureException` として投げ直されます）。

## 3.8 Asset modelから描画instanceまで

[`ModelAssetContainer`](../../src/core/asset/model.hpp#L39) は`asset_data.json.models`を読み、拡張子に応じて [`GltfLoader::loadGltf()`](../../src/core/model/gltf.cpp#L2764) / [`loadGltfBinary()`](../../src/core/model/gltf.cpp#L2753) を呼び、名前から`ModelTemplate`へmapします。受理拡張子は`.glb`/`.gltf`に加え **`.vrm`** です（[asset/model.cpp](../../src/core/asset/model.cpp#L304)）。`.vrm` がglTFと同列なのは別形式だからではなく、**VRMがバイナリglTF（GLB）コンテナそのもの**だからです — 拡張が増えているだけなので、同じbinary loader経路がそのまま読みます（受理判定の直前のコメントがそう書いています）。VRM固有の意味（humanoid bone / expression / lookAt / firstPerson）は、読み込んだglTF拡張から後段の [`vrmsemantic.hpp`](../../src/core/model/vrmsemantic.hpp) がデコードします。glTF scene fragmentの抽出には [`loadGltfBinarySceneNode()`](../../src/core/model/gltf.cpp#L2759) が使われます。

これらの `loadGltf*()` は、現在は三段APIのラッパです（[`gltf.hpp`](../../src/core/model/gltf.hpp#L68)）。

| 段 | 関数 | 性質 |
|---|---|---|
| prepare | [`prepareGltf()`](../../src/core/model/gltf.cpp#L2547) / [`prepareGltfBinary()`](../../src/core/model/gltf.cpp#L2540) / [`prepareGltfBinarySceneNode()`](../../src/core/model/gltf.cpp#L2559) | ファイル読み取りとparseまで |
| inspect | [`inspect()`](../../src/core/model/gltf.cpp#L2734) | **副作用なし**の候補パス。fragment解決と検証だけ |
| commit | [`commit()`](../../src/core/model/gltf.cpp#L2572) | GPU資源を確保して `ModelTemplate` を作る |

glTFロードの大まかな変換は次です。

```text
tinygltf Model
→ accessor/bufferからCommonPolygonVertDataを構築
→ VertBufContainerの大きなGPU vertex/index poolへ追記
→ textureをMaterialContainerへ登録
→ materialごとにPrimitiveRefInfoをまとめる
→ ModelTemplate
```

執筆基準時点から要素が増えています。VRM semanticデコード（[`vrmsemantic.hpp`](../../src/core/model/vrmsemantic.hpp) — humanoid bone / expression / lookAt / firstPerson、WP111）、morph target（[`morphtarget.hpp`](../../src/core/model/morphtarget.hpp)、WP121）、skeletal animation（[`skeletalanimation.hpp`](../../src/core/model/skeletalanimation.hpp)）、KTX2テクスチャ（[`loader/ktx2.hpp`](../../src/core/loader/ktx2.hpp)、BC5/BC7 fixtureあり）、atlas asset（[`asset/atlasasset.hpp`](../../src/core/asset/atlasasset.hpp) + [`renderer/atlasassetresource.hpp`](../../src/core/renderer/atlasassetresource.hpp)）です。モデルのhot reload（HR2-G、WP110）に伴い、世代管理は [`MaterialContainer::releaseModelResources()`](../../src/core/material/materialcontainer.hpp#L336) が担います。この API は `[[nodiscard]]` で、返り値の [`ReleaseReport`](../../src/core/model/modelresourcerelease.hpp#L36) は固定容量 8 の `ReleaseError`（`{stage, code, index}`。`index` は実 ID）と、9 件目以降を数える `overflow_count` だけを持ち、報告のために確保しません。[`releaseModelResources()` の本体](../../src/core/material/materialcontainer.cpp#L1322) は material も texture も resource 1 件ずつ try で囲むので、1 件の失敗で後続の返却が止まりません。同期経路は失敗を例外（`ModelResourceReleaseException`）に載せて運び、[`releaseErrorStatus()`](../../src/core/loader/scene.cpp#L250) が stage 名の列（溢れた分は `overflow:N`）へ畳んで [`PreCommitFailure::release_errors`](../../src/core/loader/sceneload.hpp#L142) に入れます。呼び元へ report を返せない destructor / deferred release / retirement queue / teardown の各経路は [`ReleaseErrorSink::record()`](../../src/core/model/modelresourcerelease.cpp#L11)（log + `release_errors` / `overflow_count` の累積カウンタ。[`status()`](../../src/core/model/modelresourcerelease.cpp#L28) で読めます）へ渡します（WP380d）。

失敗したときに**誰が解放するか**も、この API の一部です（WP380a-fix）。`ModelAssetContainer` は §3.5 の 3 のとおり participant ではなく warm cache なので、scene load が失敗しても cache は戻りません。ただし「戻さない」と「作りかけを残す」は別です。[constructor](../../src/core/asset/model.cpp#L513) は宣言を1件ずつ commit していき、途中で投げたときに二段で GPU 資源を返します — 内側の catch が**その1件**（[`releaseModelGpuResources(model, false)`](../../src/core/asset/model.cpp#L837)）、外側の catch が**それまでに登録済みの全件**（[`impl_->releaseAllModels()`](../../src/core/asset/model.cpp#L562)）です。どちらも report が空でなければ `ModelResourceReleaseException` に載せ替えて投げ直します。加えて `Impl` の [destructor](../../src/core/asset/model.cpp#L420) と、公開前の reload candidate を持つ [`ModelPayload` の destructor](../../src/core/asset/model.cpp#L359) が `ReleaseErrorSink` 経由で同じ解放を行います。**公開されなかった `ModelAssetContainer` は cache になりません**（moduleとして `__ready()` が立たない。§2.2）ので、部分状態を再利用するより資源を戻すほうが正しい、という選択です。

同じ WP の残り半分は**順序**です。起動経路では render graph の検証と公開が scene の prepare より**前**にあります（[`(void)GET_MODULE(Renderer)`](../../src/core/userpublic/pelican_core.cpp#L146) とその上のコメント。§2.1 の 10番）。この2つが逆だと、rendering config が壊れているプロジェクトでも先に model を upload してしまい、(a) 利用者が最初に見る fatal が material route の失敗ではなく morph 頂点 pool の容量超過にすり替わり、(b) 同一プロセスでの再試行ごとに pool の offset が積み上がります。順序と解放は独立した2つの性質で、片方だけ戻すともう片方のテストが落ちます。

> 🧩 **難所 — 128枚の結合palette**([`selectSkin()`](../../src/core/model/gltf.cpp#L1632) / [`loadMesh()`](../../src/core/model/gltf.cpp#L2258))
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
> **手がかり**: `skin_joint_offsets` は「このskinは既にpaletteへ載せた」というメモ(skin index → offsetのmap)です。防ぎ方は単純で、`selectSkin()` は本体に入る前にこのmapを引き、載せ済みなら**記録済みのoffsetを返して即座に戻ります**([`gltf.cpp` 内](../../src/core/model/gltf.cpp#L758))。joint配列への追記([その位置](../../src/core/model/gltf.cpp#L1670))はその後ろにあるので、同じskinを参照するメッシュが何個あってもjointが積まれるのは最初の1回だけです。分割側の [`addBinding`](../../src/core/animation/animationjobs.cpp#L178) は `expected_offset` を進めながら「binding群がpaletteを隙間なく覆っているか」を検証します — [`buildSkinPalette()`](../../src/core/animation/animationjobs.cpp#L403) 自体は `std::vector<Matrix4fV1> produced(required)` を値初期化してから `[0, required)` を丸ごとコピーする([`produced()`](../../src/core/animation/animationjobs.cpp#L411) / [`std::copy()`](../../src/core/animation/animationjobs.cpp#L428))ので、穴は未初期化ではなく**ゼロ行列**になります。壊れ方は不定値ではなく「その関節に属する頂点が原点へ潰れる」という決まった形で、覆い漏れを弾く責任は `addBinding` 側にあります。`maxSkinJoints = 128`([`skeletalanimation.hpp` 内](../../src/core/model/skeletalanimation.hpp#L11))とGLSL側の `PELICAN_MAX_SKIN_JOINTS`([pelican_skinning.glsl](../../src/core/resources/shaders/include/pelican_skinning.glsl))は同じ値の二重定義です。
>
> **不変条件**: binding群はoffset 0から結合paletteを隙間なく覆う。`JOINTS_0` は必ず `joint_offset` 加算済みでGPUへ届く。skinned primitiveの頂点はnode変換を含まない。

> 🧩 **難所 — 頭に触れたら三角ごと**([`splitVrmAutoTriangles()`](../../src/core/model/vrmfirstperson.cpp#L23) / 呼び出しは [`gltf.cpp` 内](../../src/core/model/gltf.cpp#L2101))
>
> **何をする所か**: VRM 1.0 の firstPerson アノテーションが `auto` のメッシュを、「一人称でも見える三角形」と「三人称専用の三角形」の2群へ分けます。firstPerson は VRM が node 単位に付ける「一人称視点でこのメッシュをどう扱うか」の注釈で（[`VrmFirstPersonMeshAnnotation`](../../src/core/model/vrmsemantic.hpp#L76) の `node` + `type`）、`type` が `auto` のときだけ「頭に属する三角形はどれか」の判定がエンジン側の仕事になります。`auto` の規則は「頂点が Head かその子孫に正の重みを持つなら、その三角形は頭グループ」です（[`vrmfirstperson.hpp` のコメント](../../src/core/model/vrmfirstperson.hpp#L27)）。
>
> **素朴に読むと**: まず索引が3段に変換されるのが読みづらい所です。`JOINTS_0` の値は**そのskinのjoints配列内のローカル番号**で、`model.skins[skin_index].joints` を引いて初めてglTFのnode indexになり、そこから頭関連ノード表([`headRelatedNodes()`](../../src/core/model/gltf.cpp#L681))を引いて分類します。もう一つが呼び出し**位置**の制約です。この呼び出しは、直後にある `joints[component] + joint_offset` の書き換え([`gltf.cpp` 内](../../src/core/model/gltf.cpp#L2175))より**前**でなければなりません。offset加算後へ動かすと、渡した `skin_joint_nodes` の範囲外になるか、たまたま範囲内なら別ボーンとして解決されて静かに誤分類されます。この順序制約はコード上どこにも書かれておらず、隣接する2箇所の並びとしてしか表れていません。分類規則自体も非対称です — 重み `> 0` の成分だけを見る(重み0のjointスロットは無視する)一方、頂点が1つでも頭関連なら**三角形ごと** `third_person_only_indices` へ落とします。`first_person_only` の群を作らないのは、VRMの `auto` が「一人称で頭を消す」だけを意味し、逆方向は明示アノテーションでしか作れないからで、[`VrmAutoTriangleSplit`](../../src/core/model/vrmfirstperson.hpp#L22) がバケツを2本しか持たないのはその反映です。
>
> **骨子**:
> ```text
> joint(JOINTS_0 のローカル番号) -> skin_joint_nodes[joint] -> glTF node index
> weight > 0 かつ head_related_nodes[node] != 0 -> その頂点は頭関連
> 三角形の3頂点のどれかが頭関連 -> third_person_only、それ以外 -> both
> ```
>
> **手がかり**: `indices` が空なら 0,1,2,… を生成して三角形列として扱う([index 列の生成](../../src/core/model/vrmfirstperson.cpp#L10))ので、非indexedプリミティブも同じ経路です。TRIANGLES以外は呼び出し側([`primitive_mode` の検査](../../src/core/model/gltf.cpp#L1095))と本体([三角形数の検査](../../src/core/model/vrmfirstperson.cpp#L25))の両方で弾きます。分割後は both 側と third 側が別プリミティブとして `variants` へ積まれる([auto split の variant 登録](../../src/core/model/gltf.cpp#L1161))ので、1つのglTFプリミティブから2つの描画レンジが生まれます。ヘッダのコメントが規範で、各群の中では**元の三角形順が保たれます**([`vrmfirstperson.hpp` 内](../../src/core/model/vrmfirstperson.hpp#L27))。テストは [`vrmfirstperson_test.cpp` 内](../../test/vrmfirstperson_test.cpp#L7) と [`morph_gltf_test.cpp` 内](../../test/morph_gltf_test.cpp#L241)。
>
> **不変条件**: 呼び出しは `joint_offset` 加算より前。分類の単位は頂点ではなく三角形。`auto` が返すのは both / third の2群だけ。

> 🧩 **難所 — -1を空けて負へ伸ばす**([`skinnedMaterial()`](../../src/core/model/gltf.cpp#L902) / [`registerVatMaterial()`](../../src/core/model/gltf.cpp#L870))
>
> **何をする所か**: 頂点シェーダだけを差し替えた派生マテリアルを登録し、そのIDをモデル内ローカルのmaterial ID空間へ割り当てます。
>
> **素朴に読むと**: `using ModelLocalMaterialId = int;`([宣言](../../src/core/model/gltf.cpp#L641))はglTFのmaterial indexをそのまま入れる型に見えます。ところが [同じ構造体](../../src/core/model/gltf.cpp#L1293) に `next_generated_material = -2` があり、派生を作るたびに**減って**いきます。ID空間が符号で二分されていて、0以上は「glTFが書いたmaterial」、負は「エンジンが派生させたmaterial」です。`-1` から始めないのは、`-1` がglTF側の『material無し』を表す予約値だからで、採番から外してあります。派生を作る理由はvert shaderの差し替えだけで、skinnedは `skinnedVertShader()`、VATは `vatVertShader()` へ変えた同内容のコピーを `registerMaterial()` し直します — 同じ元materialから最大3種の実体が生まれます。ここで `generated_material_sources` に「派生ID → 元のsource index」を残すのが要です。実行時のマテリアル上書きは authored 側の index で来て、[`polygoninstancecontainer.cpp` 内](../../src/core/renderer/polygoninstancecontainer.cpp#L1889) がその index で初期値表を引き直すため、この表が無いと [`generated_source` の復元](../../src/core/model/gltf.cpp#L2500) で `source_material_index` が `noSourceMaterialIndex` に落ち、skinnedプリミティブにだけ上書きが効かないという片側だけの不具合になります。
>
> **骨子**:
> ```text
> 0,1,2,…    glTF の material index(authored)
> -1         material 無し(予約・採番しない)
> -2,-3,…    skinned / VAT 派生(next_generated_material--)
> generated_material_sources[派生ID] = 元の material index(無ければ noSourceMaterialIndex)
> ```
>
> **手がかり**: `skinned_material_variants` は「この元materialのskinned派生は作成済み」というキャッシュ([参照箇所](../../src/core/model/gltf.cpp#L1877))で、同じ元から二重に派生を作らない同一性保証も兼ねます(`registerVatMaterial()` に同種のキャッシュが無いのは、`base_vertex` がプリミティブごとに違って共有できないためです)。`source_material_index` は再グループ化のキーでもあり、[`modeltemplate.cpp` 内](../../src/core/model/modeltemplate.cpp#L176) は「解決後のmaterial」と「元のindex」の**組**でまとめ直します。テストは [`material absolute overrides are source-material scoped and temporal`](../../test/materialinstanceoverride_test.cpp#L258)「material absolute overrides are source-material scoped」。
>
> **不変条件**: `-1` は採番しない。派生を登録したら必ず `generated_material_sources` へ元のindexを残す。負のIDは `ModelTemplate` の外へ出さない(外向きは `source_material_index`)。

> 🧩 **難所 — morphはnode単位で切る**([`appendMorphDefaults()`](../../src/core/model/gltf.cpp#L1450) / [`loadMesh()`](../../src/core/model/gltf.cpp#L2258))
>
> **何をする所か**: モデル全体で1本の `default_weights` 配列へこのmesh分の既定weightを追記し、そのスライスの先頭位置を `morph_weight_offset` として返します。
>
> **素朴に読むと**: `appendMorphDefaults()` は「追記する前の `default_weights.size()`」を返すだけで([その return](../../src/core/model/gltf.cpp#L1487))、そのsizeがいつ増えるのかは呼ばれ方に依存します。鍵は `loadMesh()` が **nodeごとに**呼ばれる点で([`loadMesh()` 呼び出し](../../src/core/model/gltf.cpp#L2350))、同じmeshを2つのnodeが参照すれば独立した2スライスができ、nodeごとに別の表情を付けられます。つまりoffsetは「mesh単位」でも「target単位」でもなく「**このnodeのこのmesh**のスライスの先頭」です。GPU側は `instance_index * PELICAN_MAX_MORPH_WEIGHTS + weight_offset` でweightを引く([`pelican_morph.glsl` 内](../../src/core/resources/shaders/include/pelican_morph.glsl#L72)、CPU側の書き込みは [`vertbufcontainer.cpp` 内](../../src/core/model/vertbufcontainer.cpp#L226))ので、offsetをprimitiveから外してmeshに1つ持たせると、同じmeshを共有する2体の表情が連動します。容量検査([この検査](../../src/core/model/gltf.cpp#L1453))を追記の**前**に置いてあるのも意図で、途中まで積んで失敗したlayoutを残さないためです。`node.weights` が `mesh.weights` より優先されるのはglTF仕様どおりですが、要素数が合わなければ黙ってmesh側へ落ちず、node名を添えてthrowします([その throw](../../src/core/model/gltf.cpp#L1466))。
>
> **骨子**:
> ```text
> default_weights: [ nodeA/mesh0 の 4 個 | nodeB/mesh0 の 4 個 | nodeC/mesh1 の 2 個 | … ]
>                    ^offset=0             ^offset=4             ^offset=8
> appendMorphDefaults(): 容量検査 -> offset = size() -> target_count 個 push_back -> offset を返す
> GPU: weight_base = instance_index * PELICAN_MAX_MORPH_WEIGHTS + metadata.weight_offset
> ```
>
> **手がかり**: `target_count == 0` なら何も積まず 0 を返す([`target_count == 0` の早期 return](../../src/core/model/gltf.cpp#L1452))ので、morphを持たないプリミティブは全部offset 0を共有します(`target_count` が0なのでシェーダは即returnします、[`pelican_morph.glsl` 内](../../src/core/resources/shaders/include/pelican_morph.glsl#L70))。同じ容量検査は [`std::to_string()`](../../src/core/model/vertbufcontainer.cpp#L156) にもあり、GPU公開の直前でもう一度掛かります。`maxMorphWeightsPerInstance = 256`([`morphtarget.hpp` 内](../../src/core/model/morphtarget.hpp#L14))はインスタンスあたりのweight総数の上限で、プリミティブあたりのtarget数の上限 `maxMorphTargetsPerPrimitive = 64` とは別物です。テストは [`morph_gltf_test.cpp` 内](../../test/morph_gltf_test.cpp#L62) / [per-instance の weight frame](../../test/morph_gltf_test.cpp#L133)。
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

`PolygonInstanceContainer` 側の同一性はSlotMap(スロットマップ — 配列のスロットを使い回しつつ、スロットごとに世代番号を持たせるハンドル方式。空きスロットの番号はfree listへ積んで再利用し、再利用のたびに世代を進めるので、古いハンドルは世代不一致で弾けます)で、`generations_` / `alive_` / `free_indices_` / `live_count_` / `scene_epoch_` を持つのは [`RendererInternal::ModelInstanceSlots`](../../src/core/renderer/modelinstanceslots.hpp#L22) です。スロット index が同じ instance を指す per-slot 配列(model 行列、skin palette、morph weight など17本)は [`ModelInstanceSlotTable`](../../src/core/renderer/polygoninstancecontainer.hpp#L312) に1つにまとまっていて、`instance_slots` もその一員です — 候補は表ごと複製し、公開は表ごと [`swap()`](../../src/core/renderer/polygoninstancecontainer.hpp#L353) するので、per-slot 配列を1本足しても運ぶ側の呼び出し点は増えません。

> **設計決定:** slot identityは animation generation やmodel assetのcontent revisionから独立しています。`scene_epoch` が進むのは **model-instance slotが死んだとき**だけで、モデルのhot reloadでは進みません。これにより「アセットを差し替えても、生きているinstanceハンドルは有効なまま」という性質が保てます。

API面の主な変化は次の通りです。

| API | 位置 | 内容 |
|---|---|---|
| `removeModelInstance()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L430) | 戻り値が `void` → **`bool`** |
| `preflightModelInstance()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L331) | 確保前の容量チェック（const） |
| `stageModelInstance()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L332) | [`StagedModelInstance`](../../src/core/renderer/polygoninstancecontainer.hpp#L359) を返す |
| `publishModelInstance()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L427) | `noexcept` の単一公開点 |
| `prepareTrs()` / `publishPreparedTrs()` / `rollbackPreparedTrs()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L442) | TRS更新のprepare/publish |
| `isModelInstanceAlive()` | [`polygoninstancecontainer.hpp` 内](../../src/core/renderer/polygoninstancecontainer.hpp#L449) | 生存判定 |
| `instanceCountForTesting()` | [`advanceMorphHistoryAfterRender()`](../../src/core/renderer/polygoninstancecontainer.hpp#L522) | **live数**（`live_instance_count`）を返す |
| `slotCountForTesting()` | [`advanceMaterialOverrideHistoryAfterRender()`](../../src/core/renderer/polygoninstancecontainer.hpp#L523) | スロット総数 |

テストは [`test/modelinstance_slotmap_test.cpp`](../../test/modelinstance_slotmap_test.cpp)（GPUラベル）です。

### `load_gltf` はトランザクション（WP144）

RPCの `load_gltf` が使う [`SceneLoader::loadTransientGltf()`](../../src/core/loader/scene.cpp#L2179) は、「**割り当てを全部公開の前に済ませ、公開点を1箇所に絞る**」形で書かれています。

1. 名前bindingのhash nodeを先に `extract()`(C++17のnode handle — 連想コンテナから要素をノードごと切り離して持ち出すAPI。取り出したノードを戻す `insert()` は確保を伴いません)して確保（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L2193)）。コメント通り、エンティティ生成後にこのnodeを差し込む操作は割り当てを伴わないため、トランザクションを分割できません。
2. `prepareGltf*()` → [`inspect()`](../../src/core/model/gltf.cpp#L2734) の副作用なし候補パス → [`preflightModelInstance()`](../../src/core/renderer/polygoninstancecontainer.hpp#L511) で、**model固有のVulkan資源を1つも確保する前に**容量超過を拒否します。
3. [`commit()`](../../src/core/model/gltf.cpp#L2572) でGPU資源を確保し、`stageModelInstance()` でstagingします。
4. エンティティ生成がthrowしたら `releaseModelGpuResources()` して `transient_models.pop_back()` します。
5. 単一公開点（[`scene.cpp` 内](../../src/core/loader/scene.cpp#L2259)）。

```cpp
// No operation below allocates: this is the single publication point for
// the entity's slot, draw commands, resources, and optional name.
instances.publishModelInstance(std::move(staged_instance));
```

`SceneLoader` には [`transient_models`](../../src/core/loader/scene.hpp#L73) / [`runtime_only_changes`](../../src/core/loader/scene.hpp#L75) / [`releaseTransientModels()`](../../src/core/loader/scene.hpp#L147) / [`hasRuntimeOnlyChanges()`](../../src/core/loader/scene.hpp#L269) が追加されています。`runtime_only_changes` は編集RPC側で「これはruntime専用の変更なので保存できない」と判定するために使います。

## 3.9 その他の純粋形式

プロジェクトが持てる文書は「`schema` と `version` の**厳密一致**で受け、知らない組は名前付きで拒否する」という1つの規則で揃っています。全量（31行）は [`docs/v0_freeze.md` §1.1](../v0_freeze.md) が正本なので、ここでは本章で扱う代表と、v0で増えた分を並べます。

| schema | version | パーサ | 位置づけ |
|---|---|---|---|
| `pelican.project` | 1 | [`projectformat.cpp`](../../src/project/projectformat.cpp#L311) | `project.json`。`engine_min_version` と `requires`（§3.2） |
| `pelican.scene` | 1〜2 | [`sceneformat.cpp`](../../src/project/sceneformat.cpp#L268) | 保存は元の版を維持。prefabはv2が前提。永続IDは両版で受理 |
| `pelican.graph` | 1 | [`graphcomposition.cpp`](../../src/project/graphcomposition.cpp#L1138) | 共通グラフ。`definitions` / `nodes` / `outputs`（§3.15） |
| `pelican.render_feature` | 1 | [`featurecompose.cpp`](../../src/project/featurecompose.cpp#L3422) | 本節「render feature」 |
| `pelican.render_pipeline` | 1 | [`renderpipeline.cpp`](../../src/project/renderpipeline.cpp#L834) | preset（`hybrid_v1` など） |
| `pelican.render_component` | 1 | [`rendercomponent.hpp`](../../src/project/rendercomponent.hpp#L16) | export / import される描画部品（§3.16） |
| `pelican.package` | 1 | [`packagemanifest.hpp`](../../src/project/packagemanifest.hpp#L19) | vendoringされるディレクトリの封筒（§3.16） |
| `pelican.graph_catalog` | 1 | [`graphmodule.cpp`](../../src/project/graphmodule.cpp#L714) | `pelican_cli graph-catalog` の出力（module / op / 型の一覧） |
| `pelican.material` / `pelican.material_bindings` | 1 | [`parseMaterialFormatJson()`](../../src/project/materialformat.cpp#L744) / [`parsePrimitiveMaterialBindingJson()`](../../src/project/materialformat.cpp#L767) | 本節「material / surface」 |
| `pelican.import` / `pelican.import_rules` | 1 | [`importmanifest.cpp`](../../src/project/importmanifest.cpp#L252) / [`parseImportRulesJson()`](../../src/project/importrules.cpp#L196) | 本節「import manifest / import rules」 |

1つだけ性格の違うものがあります。**`pelican.value_graph` はプロジェクトが書く形式ではありません。** `pelican.graph` の values 分割を評価器向けに落とした**内部IR**で、[`ValueGraph::Graph`](../../src/core/userpublic/valuegraph.hpp#L97) のコンストラクタが受ける文字列がこの形です。直書きのIRを読む経路が残っているのはテストのためで（IR側はlowering結果の真上位集合なので、bindings decodeの名前付き拒否はこの形でしか到達できません）、production で `Graph` を作るのは [`renderingpassconfigregistration.cpp` の構築点](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1028) の1箇所、入力は `pelican.graph` だけです。プロジェクト側の文書としては扱わないでください（§3.15）。

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

[`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L3422) はfeature JSONを順番に読み、render target、buffer、pass、compute taskを追加し、限定的なoverrideを適用します。名前衝突、曖昧anchor、未知override fieldは即時エラーです。shader defineも重複排除して集約します。

runtime側の入口は [`registerRenderGraphVariantFamilyFromJsonData()`](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1660) です。[`loadRenderGraphVariantsFromConfigData()`](../../src/core/vkcore/renderer_config.cpp#L453) が `ProjectBasicConfig::renderingConfigJson()` の文字列と起動ターゲットの実extentを渡し、flat（OpenXR有効時は `#xr` も）とpreviewを **1回の登録トランザクション** として受け取ります。

✅PV-1(`a173e70c`)から、この登録の中で preview の変種だけが組めないときは、登録を失敗にせず、preview を「利用できない」として記録します。compiler program は preview の要求(`data_only` の artifact)の失敗を `RenderCompilerProgramVariantOutput::unavailable`(`std::exception_ptr`)に入れて返し(✅RG-3 から入れるのは preview の拒否と、runtime の変種が組めた後の失敗だけ。manual 第6章 §6.13 の PV-1 の段落)、`RegistrationStages::prepare()` の [PV-1 の preview の分岐](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1200) がそれを `unavailablePreviewGraphProgram(previewUnsupportedAuthoringReason(...))` にします。runtime の変種(flat / `#xr`)は先に組むので、そちらが組めなければ今までどおりそこで投げます(コードの注記)。`loadRenderGraphVariantsFromConfigData()` は preview が利用できなければ warning を 1 行出します。reason の形と出る所は manual 第6章 §6.13 の PV-1 の段落です。

起動主体が feature を足す公開面は `pelican_player --feature-overlay <uri>` です。文書の厳密な
v1 検証と加算は [`applyRenderFeatureOverlays()`](../../src/project/renderfeatureoverlay.cpp#L122) が担当し、
renderer は [`loadRenderGraphVariantsFromConfigWithStartupFeatureOverlays()`](../../src/core/vkcore/renderer_config.cpp#L421)
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

sceneと違い、feature合成は「純粋層で済ませてからruntimeへ渡す」形ではありません。上の登録関数の内側でGPU非依存の [`runRenderCompilerProgram()`](../../src/core/renderingpass/rendercompilerprogram.cpp#L236) が走り、その中の [`resolveRenderPipeline()`](../../src/project/renderpipeline.cpp#L1212) が `composeRenderFeatureConfig()` を呼びます。二段であること自体は同じで、境界が呼び出し順ではなく「GPUに触る前／後」で引かれている、という違いです。詳細は[第6章](06_rendering_vulkan_shader.md)。

#### feature の要る target・挿入先・snapshot の `after`(FR-1 / FR-2)✅実装済み

Graph で組んだ pipeline に feature や snapshot を足したときの拒否を、Studio が 1 回の編集で直せる形にした変更です(FR-1 `45ca769b`、FR-2 `0b15fb62`)。engine は target も pass も黙って足さず、要るものを名前で返します。利用者向けの説明は [manual 第6章](../manual/06_rendering.md) §6.5「feature が pipeline に求める target と挿入先」と §6.8 です。

**読む側の 3 つの純関数**(どれも壊れた文書で投げず、断るのは合成器の仕事。ヘッダの注記)

| 関数 | 返すもの | 使う所 |
|---|---|---|
| [`renderFeatureRequiredTargets()`](../../src/project/featurecompose.cpp#L4284) | `[{name, type, usage?}]`。pass の `output.color` / `output.depth` / `input`、`render_target_overrides` の鍵、文書の `required_targets` を、名前が最初に出た順に | `list_render_features` の `required_targets`、合成の検査 |
| [`renderFeatureInsertions()`](../../src/project/featurecompose.cpp#L4411) | `[{pass, anchor, position}]`。`insert` が `after:` / `before:` の entry だけ、文書の順 | `list_render_features` の `insertion` |
| [`renderSnapshotAfterCandidates()`](../../src/project/featurecompose.cpp#L4435) | 各 family の `post_ldr` の anchor から `pelican_ui` の anchor の手前までの frame node の名前(anchor は anchor 名)の、全 family の共通部分を最初の family の順で | `get_render_authoring_context` の `snapshot_after_candidates`、`snapshot.after_unknown@1` の `candidates` |

`renderFeatureRequiredTargets()` の型の決め方は 3 段です。文書の `required_targets` に同じ名前があればその型、無ければ depth attachment に使う名前は `pelican.render.depth@1`(device / projection)、残りは `pelican.render.legacy_opaque_image@1` です。`$<parameter>` はその parameter の `default` に読み替え(`default` が無ければ数えない。✅RG-3 から、3 つ目の引数 `bindings`(`{parameter: target}`)に値があればそちらに読み替え、parameter の宣言の `usage` を型の決め方に足します。[RG-3 で結んだ先を読む所](../../src/project/featurecompose.cpp#L4333))、`@history` は外し、feature 自身の `render_targets` / `buffers` と `swapchain` / `display` は外します。integration の fragment は見ません。

✅FR-4(`2eb08d21`)はこの 3 段を変えず、同梱の feature の文書に宣言を足しただけです。`sky_ambient.json` と `bloom.json` の `lit_color`、`rt_shadow_mask.json` の `gbuffer_worldpos` / `gbuffer_normal` を `pelican.render.color_signal@1`(`reference: scene`、`transfer: linear`、`range: extended`)と書き、Graph の color port と同じ型にしました([FR-4 で bloom が lit_color を宣言する所](../../src/core/resources/features/bloom.json#L413))。宣言の無い色の target(`taa` の `velocity` など)は今も最後の段の `legacy_opaque_image` です(`featurecompose_test` の FR-1 の test が見ます)。

**合成の中の 3 か所**

- **要る target の検査**: `composeRenderFeatureConfig()` は feature を当てる前に、pipeline の target 名に、snapshot の名前と全 feature(と integration の fragment)の `render_targets` を足した集合 `available_targets` を作ります([集合を作る所 FR-1](../../src/project/featurecompose.cpp#L3668))。各 feature の [`requireFeatureTargets()`](../../src/project/featurecompose.cpp#L1985) は、まず文書の `required_targets` の形を [`validateDeclaredRequiredTargets()`](../../src/project/featurecompose.cpp#L1959) で見て(code の無い `std::runtime_error`)、次に要る target の最初の欠けを `feature.required_target_missing@1` `{feature, target, type, document}` の `GraphRegistryError` で投げます。呼ぶのは `applyRenderTargetOverrides()` と `addFeaturePasses()` の前です([呼ぶ所 FR-1](../../src/project/featurecompose.cpp#L3710))。だから同じ feature で挿入先も欠けていれば、こちらが先に出ます。後の feature が宣言する target も `available_targets` に入っているので、feature の順には依りません。
- **挿入先の拒否**: `insertPassByAnchor()` は feature 名と文書の参照を受けるようになり、`findRenderPassAnchorMatches()` が 0 件のとき `feature.insertion_anchor_missing@1` `{feature, anchor, candidates, document}` を投げます([投げる所 FR-2](../../src/project/featurecompose.cpp#L2336))。`candidates` は [`insertionCandidatesOf()`](../../src/project/featurecompose.cpp#L2293) で、その時点の config の全 family の pass(anchor は anchor 名)を順に集め、`findRenderPassAnchorMatches()` がちょうど 1 件になる名前だけを残します。2 件以上は今までどおり code の無い `ambiguous` です。
- **snapshot の `after`**: `materializeSnapshots()` は、copy を挿す前に候補を `snapshotAfterCandidatesOf()`([候補の本体](../../src/project/featurecompose.cpp#L516))で読み、family に `after` の node が無いと `snapshot.after_unknown@1` `{snapshot, after, resolved?, candidates}` を投げます([投げる所 FR-1 snapshot](../../src/project/featurecompose.cpp#L611))。あわせて copy の `snapshot_after` を、anchor のときは `__anchor_<anchor>` にしました([書く所 FR-1 snapshot](../../src/project/featurecompose.cpp#L634))。前は anchor の素の名前を書いていて、その名前の frame node が無いので実行計画(`pelican.dependency.snapshot_after@1` の依存)が断っていました。`materializeSnapshots()` は合成で 1 回だけ、pipeline の作り方を見ずに呼ばれるので、この不具合は anchor 名を書いたどの pipeline にもありました。

3 つの code の定義と fields の注記は [`featurecompose.hpp` の FR-1 / FR-2 の code](../../src/project/featurecompose.hpp#L182) の周りです。同じ FR-1 で、`render.target_ref` の型の食い違い `graph.target_ref_type_mismatch@1` に `expected: {type, layers?}` が付きました([`expectedTargetRefType()`](../../src/project/rendergraphcomposition.cpp#L1384))。kind が合えば書いた型の topology を target のものに替え、合わなければ depth か legacy の型にし、`two_d_array` なら `layers` を付けます。

RPC 側は 2 か所だけです。`listRenderFeatures()` は catalog の各 entry の文書を読み(`engine://` は engine resource、それ以外は `resolve_document_path`)、2 つの関数の結果を `required_targets` / `insertion` に入れます。読めなければどちらも `[]` です([足す所 FR-1 / FR-2](../../src/core/communication/renderconfigeditor.cpp#L1257))。`getRenderAuthoringContext()` は解決した config から `snapshot_after_candidates` を入れます([足す所 FR-1 context](../../src/core/communication/renderconfigeditor.cpp#L2576))。

**書き手の無い target(✅FR-3、`55390f4b`)**

宣言はあるが誰も書いていない target を feature が読むと、FR-3 の前は合成が通り、frame planner が `Pass input target is not produced as an earlier output` の文で断っていました。FR-3 は合成の終わりにこれを名前付きで断ります。利用者向けは manual 第6章 §6.5 の「書き手」です。

- **読む側**: `renderFeatureRequiredTargets()` に引数 `with_needs_writer`(既定 `false`)が付き、`true` なら各要素に `needs_writer` を入れます。名前を `SAMPLED` として(`@history` でなく)使う所があれば「読む」、`COLOR_ATTACHMENT` / `DEPTH_STENCIL_ATTACHMENT` として使う所があれば「書く」と数え、`needs_writer` は「読む かつ 書かない」です([FR-3 の needs_writer の式](../../src/project/featurecompose.cpp#L4404))。`listRenderFeatures()` は `true` で呼びます([FR-3 で catalog に needs_writer を出す所](../../src/core/communication/renderconfigeditor.cpp#L1271))。
- **合成の中**: `composeRenderFeatureConfig()` は各 feature の FR-1 の検査の直後に `{名前, 参照, renderFeatureRequiredTargets(loaded.feature, true, &resolved_parameters)}` を `written_checks` に積み([積む所 FR-3](../../src/project/featurecompose.cpp#L3714)。✅RG-3 `7a12fc2a` から、束ねる前の feature の文書と instance の binding で読み、depth の parameter に束ねた target は parameter が宣言する depth の型のまま。コードの注記)、fold group を当てた後、material の routing を解く前に、1 件ずつ [`requireFeatureTargetsWritten()`](../../src/project/featurecompose.cpp#L2072) を呼びます([呼ぶ所 FR-3](../../src/project/featurecompose.cpp#L4244))。全 feature を当てた後なので、後の feature の pass が書く target も「書いている」に数えます。
- **`requireFeatureTargetsWritten()` の手順**:
  1. `needs_writer` の target で buffer でないものについて、出所(provenance)がその feature の pass の中から、その名前を `input` に持つ pass を探し、family とその中の位置を覚えます。無ければ(この合成に読む pass が無ければ)飛ばします。
  2. 合成後の `render_targets` に宣言があり、`initial` を持たないときだけ続けます。宣言の無いものは FR-1 が先に断っています。
  3. 全 family の pass と `compute_tasks` を [`mayWriteTarget()`](../../src/project/featurecompose.cpp#L2003) で見ます。`input` / `reads` / `input_sampling` / `name` / `after` / `before` の下は数えず、それ以外の所にその名前の文字列があれば書いているとみなします(注記: 迷えば「書くかもしれない」に倒し、残りは frame planner に任せる)。
  4. 誰も書いていなければ候補を作り、`feature.required_target_unwritten@1` `{feature, target, type, writer_candidates, document}` の `GraphRegistryError` を投げます。
- **候補の作り方**: 読む pass と同じ family で、その pass より前の pass のうち、`view_family` が空で、出所が feature でないもの([FR-3 で候補から外す所](../../src/project/featurecompose.cpp#L2139))。種類は `type` の semantic が `pelican.render.depth@1` なら depth で `output.depth` を、それ以外は color で `output.color` の各要素(2 つ目から `color.<番号>`)を見て、書いている target の種類(`composedTargetKindOf()`: 宣言の format の先頭が `D` なら depth)が同じものを残します。`swapchain` / `display` と要る target 自身は外します。pass の出所の `graph_origin` が root の node(instance path もループも空)なら、`graphWritePort()` がその node の入力 port のうち `render.image`(id が resource)か `render.target_ref`(`name` が resource)に繋がるものを探し、見つかれば `node` とその port 名を入れます。
- code の定義とヘッダの注記は [`featureRequiredTargetUnwrittenCode`](../../src/project/featurecompose.hpp#L175) です。

test: `renderconfigeditor_test` の [「FR-3 a feature's target that no pass writes is refused with the passes that could write it, and connecting one composes」](../../test/renderconfigeditor_test.cpp#L11819)(Blank から Graph で組んだ deferred の pipeline の fixture `test/fixtures/fr3_blank_graph/`、編集の ticket と実際の Vulkan の program を device 無しで組む preflight)、`engine_primitives_test` の [「FR-3 sky_ambient on the Blank-built deferred pipeline names the material pass's depth as its writer, and connecting it publishes, through the running renderer」](../../test/engine_primitives_test.cpp#L3160)(GPU)。FR-1 / FR-2 の `featurecompose_test` は、宣言だけでなく前の pass の `output.depth` を `scene_depth` にして通す形に書き換えました(FR-1 の test は、宣言と target_ref だけでは候補が空の `feature.required_target_unwritten@1` になることも見ます)。RPC の `edit_batch` は第7章の編集系の表です。

test: `featurecompose_test` の [「FR-1 a feature names the targets it leaves to the pipeline and is refused by name without one」](../../test/featurecompose_test.cpp#L9921)・[「FR-1 a snapshot after a name the pipeline lacks is refused with the names it takes」](../../test/featurecompose_test.cpp#L10033)・[「FR-1 a target_ref whose type contradicts its target is told the type to write」](../../test/featurecompose_test.cpp#L10077)・[「FR-2 a feature inserted after a pass the pipeline lacks is refused with the names it takes」](../../test/featurecompose_test.cpp#L10119)。RPC の道は `renderconfigeditor_test` の FR-1 / FR-2 の 2 本です。

### material / surface

[`parseMaterialFormatJson()`](../../src/project/materialformat.cpp#L744) はmaterial v1とoptional surface catalogの整合性を検証します。[`parseSurfaceFormat()`](../../src/project/surfaceformat.cpp#L1239) はfront matter風の宣言部とshader codeを分け、parameter/texture/screen inputを構造化します。

これらはruntime接続済みです（WP116〜117, 122）。純粋層はparseと [`lowerMaterial()`](../../src/project/materiallowering.hpp#L160)（OpenPBR写像は [`openpbrmapping.hpp`](../../src/project/openpbrmapping.hpp)）まで、core層はsurface compile（[`surfacecompiler.cpp`](../../src/core/shader/surfacecompiler.cpp)）とvalues SSBO登録（[`registerReloadableMaterialValuesFile()`](../../src/core/material/materialcontainer.hpp#L347)）を担います。hot reloadは `MaterialValuesReloadHandler` / `TextureReloadHandler`（[src/core/material/](../../src/core/material)）です。

material bindingは **USD pathキー**を持ちます（[`materialformat.hpp`](../../src/project/materialformat.hpp#L45) の `usd_path`、[`modeltemplate.cpp`](../../src/core/model/modeltemplate.cpp#L99) で重複/未解決を検証）。これはU-USDレーン（WP119/124）の観測境界です。

#### RPC で material 文書を読み書きする(✅MT-1、`976c410d`)

新しい [`materialdocumentrpc.hpp` の MT-1 の契約の注記](../../src/core/communication/materialdocumentrpc.hpp#L14) / `materialdocumentrpc.cpp`(`pelican_core`)が、7 本の RPC(`list_materials` / `get_material` / `get_surface_schema` / `get_material_bindings` / `edit_material` / `create_material` / `bind_material`)を持ちます。入口は [`configureMaterialDocumentRpcHandlers()`](../../src/core/communication/materialdocumentrpc.cpp#L1443) の 1 つで、[`configureEngineRpcHandlers()` が PJ-1 の直後に呼びます](../../src/core/communication/rpcserver.cpp#L1590)。RPC の契約は manual 第10章 §10.3「material 文書を読み書きする」、live と restart_required の規則は manual 第6章 §6.7 です。

- **書き方**: 書く 3 本は、文書を `nlohmann::ordered_json` で読んで直し、鍵の順・2 空白の indent・元の改行コードで出し直します(新しいファイルは LF。[MT-1 の文書の書き出し `renderJsonDocument`](../../src/core/communication/materialdocumentrpc.cpp#L109))。書き込みは `commitProjectDocuments()`(project の文書の transaction。digest の CAS)で、`ProjectDocumentExternalModification` は `external_modification`、`ProjectDocumentTransactionBusy` は `material.transaction_busy@1` に写します。asset_data を書いたときは `ProjectBasicConfig::publishAssetDataJson()` で engine の写しも替えます([MT-1 の asset_data の写しの更新](../../src/core/communication/materialdocumentrpc.cpp#L515))。
- **書く前の検査**: 値は surface の宣言で型と `min` / `max` を見ます([MT-1 の値の検査 `valueProblem`](../../src/core/communication/materialdocumentrpc.cpp#L362))。書いた後の文書は、起動時と同じく `parseMaterialFormatJson()` と `lowerMaterial()` / `lowerMaterialVariants()` に通します([MT-1 の候補の文書の検査](../../src/core/communication/materialdocumentrpc.cpp#L458))。
- **`edit_material` の当て方**: 変わった鍵が `values` だけなら `MaterialContainer::prepareSurfaceMaterialReload({}, {文書の key})` を作って commit します(WP105 の values reload の経路)。ほかの鍵が変わったら `restart_required` です([MT-1 の live の判定](../../src/core/communication/materialdocumentrpc.cpp#L890))。
- **`create_material` の登録**: `VulkanManageCore::waitIdle()` の後、[`ProjectMaterialAssetContainer::registerDocument()`](../../src/core/material/projectmaterialasset.cpp#L521) で新しい文書の material を登録します。module がまだ無ければ `GET_MODULE` で作り、constructor が asset_data の全部の文書を登録します(コードの注記)。
  - `registerDocument()` は constructor と同じ `Impl::registerDocuments()` を 1 件で呼びます。constructor が持っていた「文書を読み、surface を parse し、lowering して登録する」処理を、MT-1 でこの関数に切り出しました。parse できない文書・無い surface・登録済みの名前は、1 つも登録せずに throw します(header の注記)。
  - [`ProjectMaterialAssetContainer::documentOf()`](../../src/core/material/projectmaterialasset.cpp#L528) は、登録済みの material を読んだ文書の参照(asset_data の `materials[]` の参照)を返します(`Impl::material_documents`)。`list_materials` / `get_material` の `registered` と、`create_material` の名前の衝突の検査が使います。
- **`bind_material` の当て方**: [`ModelAssetContainer`](../../src/core/asset/model.hpp#L39) に MT-1 で足した関数を使います(§3.8 の model の container)。
  - [`ModelAssetContainer::hasModel()`](../../src/core/asset/model.cpp#L850): その名前の template を今持っているか。
  - [`ModelAssetContainer::materialSlots()`](../../src/core/asset/model.cpp#L854): model のファイル(基本形なら生成した形)を `GltfLoader::inspect()` で読み直し、primitive ごとに `ModelMaterialSlot {mesh, primitive, slot, routing}`(`slot` は primitive が持つ glTF の material の名前)を mesh・primitive の順に返します。bindings の内容は見ず、何も登録しません。
  - [`ModelAssetContainer::declaredMaterialBindings()`](../../src/core/asset/model.cpp#L883): 今の宣言が持つ material_bindings の文書。
  - [`ModelAssetContainer::exchangeMaterialBindings()`](../../src/core/asset/model.cpp#L888): 参照を material_bindings の文書として読み(`loadMaterialBindings()`)、宣言の中のものと入れ替えて前のものを返します。template は、`reloadKey()` の reload が作り直すまで前の bindings のままです。
  - [`ModelAssetContainer::restoreMaterialBindings()`](../../src/core/asset/model.cpp#L896): 入れ替えの後の reload が失敗したときに前のものを戻します。
  - [`ModelAssetContainer::reloadKey()`](../../src/core/asset/model.cpp#L901): その model の reload を要求する `AssetKey`(宣言の `container_key`)。
  - [`ModelAssetContainer::modelDeclaringMaterial()`](../../src/core/asset/model.cpp#L905): その名前の glTF の material を持つ model の名前(`create_material` の名前の衝突。同じ名前の project material は、その model の bindings を曖昧にして読み込みを止めるため)。
  - 流れ: 文書を commit → `exchangeMaterialBindings()` → `waitIdle()` → [`ReloadService::reloadModelsNow()`](../../src/core/watch/reloadservice.cpp#L409)(`ReloadKind::modified` の要求で model の reload group を作り、transaction を prepare・publication の準備・commit まで、返る前に済ませる。ファイルの変更が積むのと同じ group)。reload が throw したら `restoreMaterialBindings()` で戻し、`restart_required` を返します([MT-1 の model の読み直しを呼ぶ所](../../src/core/communication/materialdocumentrpc.cpp#L1432))。
- test: [`materialdocumentrpc_test.cpp` の MT-1 の 1 本](../../test/materialdocumentrpc_test.cpp#L202)(GPU。`pelican_define_test(materialdocumentrpc_test GPU RUNTIME_SHADER ...)`)。着地の gate(触った家族の 259 本)は緑でした(統合側の記録)。

> 🧩 **難所 — vec3の後ろ4 byte**([`std140SizeAlignment()`](../../src/project/materiallowering.cpp#L32) / [`makeSurfaceStd140Layout()`](../../src/project/materiallowering.cpp#L435))
>
> **何をする所か**: surfaceのparameter宣言順にstd140(GLSLのuniform block標準レイアウト規則)のオフセットを割り付け、values領域の総サイズを決めます。
>
> **素朴に読むと**: `vec3` に `{12, 16}` を返すのが「std140ではvec3は16バイト」という一般論と食い違って見えます。std140の規則は「開始位置は16の倍数、占有は12」で、直後に `float` が来ればその4バイトへ滑り込みます。だからパッカーは2つの値を別々の目的に使い分けます — `alignUp(offset, alignment)` で次の**開始位置**を決め、`offset += size` で**自分が食う量**だけ進めます([その2行](../../src/project/materiallowering.cpp#L376))。`size` を16にすると、規則どおりに計算するGLSLコンパイラ側とCPU側で4バイトずれ、vec3より後ろの**全パラメータ**が1つずつずれて読まれます。バリデーションレイヤもreflectionも検出できず、色が微妙に違うだけの症状になるのが最悪の性質です。だから12と16を分けて持つことが必須で、alignmentは次の要素の開始位置、sizeは自分が食う量、と役割が違います。最後に構造体全体を `layout.alignment`(既定16、[`materiallowering.hpp` 内](../../src/project/materiallowering.hpp#L29))へ切り上げるのも規則どおりです。
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
> **手がかり**: 割り付けは**宣言順**で、名前順でもサイズ順でもありません(テスト名がそのまま [`surface values use declaration-order std140 offsets`](../../test/materiallowering_test.cpp#L49)「surface values use declaration-order std140 offsets」)。上限 `materialCustomValueCapacity = 256`([`materiallowering.hpp` 内](../../src/project/materiallowering.hpp#L16))はGPU側の `MaterialGpuData::custom_values` と同じ値で、[`material.hpp` 内](../../src/core/material/material.hpp#L113) の `static_assert` が両者を結び付けています。超過時に `surface.params.back()` の名前を添えてthrowするのは、どのパラメータで溢れたかを作者に返すためですが、**溢れた位置ではなく末尾の名前**である点は割り切りです([その throw](../../src/project/materiallowering.cpp#L449))。
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
> **手がかり**: `reason` 文字列(`coat_weight_nonzero` など)がそのまま診断に出るので、G-bufferにチャネルを足したときは**この一覧から対応する条件を消す**、という対応関係になっています。逆に条件を足し忘れたままG-bufferを変えると、deferredに流れたmaterialが静かに違う見た目になります。不適格になったOpenPBR materialが実際にどこへ落ちるかは `automaticRoute()` の続きで、engine提供のOpenPBR surfaceはいずれも `pelican_lighting_v1` を実装しているため `forward_opaque` / `automatic_custom_lighting` になります([route の決定](../../src/project/materiallowering.cpp#L256))。判定の入口は [`evaluateDeferredEligibility()`](../../src/project/materiallowering.cpp#L413) で、OpenPBR surface以外は `standard_pbr_v1` の別条件を通ります。テストは [`materiallowering_test.cpp` 内](../../test/materiallowering_test.cpp#L243)「OpenPBR base subset routes deferred while extended lobes stay forward」。
>
> **不変条件**: 既定値の一覧とG-bufferのチャネル構成は対で動かす。`reason` 文字列は診断の一部なので条件と1対1に保つ。判定は既定からの逸脱の有無であって、値の妥当性検査ではない。

### import manifest / import rules

[`parseImportManifestJson()`](../../src/project/importmanifest.cpp#L252) はDCC納品物のtool/source/outputとSHA-256、対応schemaを検証します。runtime playerではなく、[`pelican_cli import`](../../src/devcli/importcommand.cpp#L351) が主利用者です。✅AB-1 から output の schema に `pelican.audio_analysis_table`(`version: 1` 必須。[AB-1 の schema の行](../../src/project/importmanifest.cpp#L159))が加わり、音の解析の bake が同じ形の manifest を書きます。manifest から「何から作った file か」と古さを読む [`readDerivedAssetStatus()`](../../src/project/audioanalysisformat.cpp#L250) は `parseImportManifestJson()` を通るので、どの tool の manifest も読みます(第5章 §5.9「音の解析を焼く」)。加えて [`importrules.hpp`](../../src/project/importrules.hpp) がmatch/recipe/defaultsの3層優先を持つルールベースimport（`pelican_cli import --rules`、[importcommand.cpp](../../src/devcli/importcommand.cpp#L355)）を提供します。

**読む側が開いている file の置き換え —— `filereplace`(✅AB-2、`b3003b2a`)。**焼いた表と manifest、`pelican_cli image ktx2` の出力は、engine の file の見張り・hash・表や texture の読み込みが開いている最中に置き換えられることがあります。[`filereplace.hpp`](../../src/project/filereplace.hpp#L19)(`pelican_project`)がその置き換えを 1 か所に持ちます。

- [`writeFileReplacing()`](../../src/project/filereplace.cpp#L169) は、`<path>.tmp` に書いて閉じ、`replaceFile()` で `<path>` へ置き換えます。どこで失敗しても一時 file を消し、`<path>` は元のままです(親の dir は作ります)。
- [`replaceFile()`](../../src/project/filereplace.cpp#L113) は、Windows では POSIX の意味の rename(`SetFileInformationByHandle` の `FileRenameInfoEx` に `FILE_RENAME_FLAG_REPLACE_IF_EXISTS | FILE_RENAME_FLAG_POSIX_SEMANTICS`)を試し、その情報 class が無い Windows・file system では `MoveFileExW(MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)` に落とします。`ERROR_ACCESS_DENIED` / `ERROR_SHARING_VIOLATION` / `ERROR_LOCK_VIOLATION` はやり直し、ほかの失敗は `std::runtime_error` です。Windows 以外は `std::filesystem::rename` を 1 回です。
- やり直しは [`FileReplaceRetry`](../../src/project/filereplace.hpp#L31)(`first_wait` 50 ms、`total` 2000 ms)。待ちは倍々で、合計が `total` を越えないように切り、最後にもう 1 回試します。それでも駄目なら [`FileReplaceLocked`](../../src/project/filereplace.hpp#L19)(`std::runtime_error` の派生で `path()` を持つ)を投げます。
- POSIX の rename は、delete も共有して開いている handle(コードの注記の例は real-time scanner)があっても置き換えられ、開いている側は古い file を持ち続けます。delete を共有しない handle には今も `ERROR_SHARING_VIOLATION` が返るので、やり直しの loop は残してあります(コードの注記)。
- 1 回分の試み(`posixReplace()`・`isTransientReplaceError()`・`isUnsupportedInformationClass()`・`systemErrorMessage()`)は、もとは project 文書の transaction(`projectdocumenttransaction.cpp`)の中にあり、AB-2 で namespace `FileReplaceWin32` に移して共有しました。transaction は自分のやり直しの loop(試みの間に digest を確かめ直す)を持ったままです(ヘッダの注記)。
- 使う所: 音の解析の bake([`bakeAudioAnalysis()` で `FileReplaceLocked` を受ける所](../../src/core/audio/audioanalysis.cpp#L214)。`audio.analysis_failed@1` に `reason: "output_locked"` と `path` を付ける。第5章「音の解析を焼く(AB-1)」)と `pelican_cli image ktx2`([`runKtx2Command()` の置き換え](../../src/devcli/imagecommand.cpp#L110))。
- 読む側は共有のモードを変えず、開いている時間を短くしました: [`loadKtx2File()`](../../src/core/loader/imageloader.cpp#L102) は読み終えて閉じてから parse し、[AB-2 の `readDerivedAssetStatus()`](../../src/project/audioanalysisformat.cpp#L250) は manifest を閉じてから元の音と出力を hash します。
- test: `ab1_audio_analysis_test` の「AB-2 bake_audio_analysis replaces a table a reader held open once it closes, and refuses with output_locked when it never does」(`std::ifstream` で開いたまま bake を始め、300 ms 後に閉じれば committed。Windows では閉じなければ `output_locked`、`FILE_SHARE_DELETE` 付きの handle なら待たずに committed)。

> 🧩 **難所 — `*` は `/` を越えない**([`segmentMatches()`](../../src/project/importrules.cpp#L155) / [`importGlobMatches()`](../../src/project/importrules.cpp#L258))
>
> **何をする所か**: importルールのglobパターンを、プロジェクト相対パスへ突き合わせます。
>
> **素朴に読むと**: 同じ形のDP(動的計画法 — 部分問題の可否を表へ埋めながら前進させる解き方)が2つ並び、片方は文字を、片方はパスセグメントを順に調べます。`*` と `**` の遷移式は字面上まったく同じ2本(パターンを進める = 0個マッチ、値を1つ進めてパターンを留める = 1個以上)なので、両者の違いがどこから生まれるのかコードから読み取れません。違いは遷移式ではなく**何を1単位とするDPか**にあります。`*` のDPは `splitPath()` 済みの1セグメント内だけを走るので、区切り文字を消費する術が構造的にありません = `*` は `/` を越えられません。`**` のDPはセグメントの列を走るので、同じ2本の遷移が「0個以上のディレクトリ階層」になります。つまりglobの意味論が正規表現の特別扱いではなく、DPを2段に分けたこと自体で表現されています。どちらもpush型(`dp[i][j]` が真のときだけ前進し、偽なら `continue`)なので、バックトラック実装にありがちな `a*a*a*b` の指数爆発が起きません — 外部から与えられるルール文字列を順に調べるので、これは性能ではなく安全性の選択です。片方だけ「便利だから」と `*` に `/` を跨がせると、ルールの優先順位(最初に一致したruleが勝つ、[rule 層の走査](../../src/project/importrules.cpp#L285))が変わり、import先が黙って入れ替わります。
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

> 🧩 **難所 — ファイルの再走査で監視通知の取りこぼしを防ぐ**([`Impl::reconcile()`](../../src/core/watch/filewatcher.cpp#L685) / [`Impl::scan()`](../../src/core/watch/filewatcher.cpp#L508))
>
> **何をする所か**: 監視対象のstoreを一巡し、各ファイルの size / mtime を前回のinventoryと比べて、違うファイルだけ安定digestを取り(✅FW-1。同じものは digest を持ち越す)、live digestに照らしてreloadを積みます。
>
> **素朴に読むと**: `for (;;)` の先頭で `notification_generation` を控え、scan完了後に一致していなければ**もう一周まるごと**やり直す形が、終わらないループに見えます。ここまでするのは、native watchを張る `armAll()` と最初のscanの間、およびscan実行中に届いた変更が、ディスクinventoryの差分だけでは検出できないからです(スキャン済みの位置が後から書き変わります)。generationを前後で比べるのは「スキャン中に通知が来たか」を知る唯一の手段で、来ていれば全体をもう一度走査します(FW-1 からは size / mtime の比較なので、取り直す hash は変わったファイルの分だけです) — 部分再スキャンにしないのは、どのファイルが変わったかを通知が持っていない(overflowもある)からです。`scan()` が [`readStableContentDigest()`](../../src/core/watch/contentdigest.cpp#L129) へ渡すcancelクロージャ([その受け渡し](../../src/core/watch/filewatcher.cpp#L643))は別の理由です。大きいファイルの SHA-256 は時間がかかりうる(FW-1 前は reconcile のたびに全ファイルを hash し、`projects/example` の複写で 1 回約 17〜19 s)のに、その間にreload gateが閉じたりepochが進んだりすれば結果は捨てるしかありません。捕まえた `epoch` と現在を比べて途中で降りることで、gateを閉じる操作(replay開始など)がwatcherスレッドの完了待ちでブロックしなくなります。ループが止まるのは、`stopping` / gate無効 / epoch不一致で `false` を返す出口([早期return](../../src/core/watch/filewatcher.cpp#L495))と、generation一致で `true` を返す出口([正常終了](../../src/core/watch/filewatcher.cpp#L467))の2つがあるためです。
>
> **骨子**:
> ```text
> for (;;):
>     generation = notification_generation      # scan の前に控える
>     current = scan(epoch, seed)               # size/mtime が同じ entry は持ち越し、違うものだけ安定 digest
>                                               # (FW-1。起動時 = seed は settle 済みのファイルを hash しない)
>     stopping / gate 無効 / epoch 違い -> return false
>     seed なら seedLive / seedLiveUnknown(digest の無い entry)で終わり
>     それ以外: digest の無い entry は飛ばし、observe() が queue_reload を返したものを queue へ、消えたものは removed
>     inventory = current
>     notification_generation == generation -> dirty/overflow を落として return true
>     # 違えば scan 中に通知が来ている: もう一周(部分ではなく全体)
> ```
>
> **手がかり**: 呼び出し側は「arm → reconcile」の順を守る必要があり、[その呼び出し箇所](../../src/core/watch/filewatcher.cpp#L673) にコメントで `Required transition order: arm, reconcile while still in polling state, then publish watching` と明記されています。`readStableContentDigest()` は書き込み途中のファイルを読むと `retry` を返し、`scan()` は25ms間隔で最大20回まで粘ります([リトライの設定](../../src/core/watch/filewatcher.cpp#L459))。テストは [`filewatcher_test.cpp` 内](../../test/filewatcher_test.cpp#L461)「disabled changes resume through arm-scan barrier exactly once」と [polling fallback 側](../../test/filewatcher_test.cpp#L258)(gate cancellation)。
>
> **不変条件**: generationはscanの**前**に控える。scan中に通知が来たら部分ではなく全体を取り直す。epochが動いたら結果を採用せず `false` で降りる。

**走査を安くした所(✅FW-1、`92681afd`)と、宣言で絞る所(✅HW-1、`c947cef4`)。**どちらも `src/core/watch/filewatcher.cpp` の中です。

- **持ち越しの条件**: inventory の entry は一覧から取った size / mtime と `stat_trusted` を持ち、[`recordFile()`](../../src/core/watch/filewatcher.cpp#L608) は size / mtime / key が同じで `stat_trusted` の entry を読まずに持ち越します。`stat_trusted` が立つのは、mtime が走査の開始から [`settled_after`](../../src/core/watch/filewatcher.cpp#L104)(100 ms)より前で、hash の読んだ handle の size / mtime が一覧と同じときだけです。窓の中のファイルは次の走査でもう一度読みます。NTFS の mtime は連続の書き込みで同じ値になり、窓が無いと「走査の直後に同じ size で書き直した保存」を取りこぼすためです(FW-1 の報告: 連続 200 回の書き込みで異なる mtime は 85 個。仕様書は対策を足さない指示で、ここは設計から外れた判断)。
- **起動時**: 最初の reconcile は `seed` で走り、settle 済みのファイルは hash せずに digest の空の entry にし、`ContentDigestState::seedLiveUnknown()` で live を未知にします([`SourceDigestState::live_unknown`](../../src/core/watch/contentdigest.hpp#L56))。未知はどの digest とも等しくない([`liveEquals()`](../../src/core/watch/contentdigest.cpp#L141))ので、そのファイルの最初の観測は中身が同じでも `queue_reload` です。消去は `observeMissing()` が未知でも報告します。
- **`.pelican`**: mount が空の store の深さ 0 の `.pelican` には入りません([`isProjectMetadataName()`](../../src/core/watch/filewatcher.cpp#L63))。native watch の側は、1 回の通知の record が全部無視してよい名前なら走査の理由にしません([`onlyIgnoredRecords()`](../../src/core/watch/filewatcher.cpp#L232))。
- **path の key**: plain な entry は root の canonical に一覧の相対名を足して key にします(1 ファイルごとに `weakly_canonical` を 5 回呼び、Debug の 3000 ファイルで 1.3 s かかっていた)。symlink / junction とその下だけは従来どおり 1 件ずつ解決します。
- **数える**: [`WatcherStatus`](../../src/core/watch/filewatcher.hpp#L25) の `scans` / `hashed_files` / `hashed_bytes` は起動からの累計で、`Impl` の `total_*` に足して `status()` が写します。reconcile の終わりに、hash したファイルがあれば(起動後の最初は 0 でも)INFO の `file watcher: scanned … files, hashed … (… MB) in … ms` を出します([出す所](../../src/core/watch/filewatcher.cpp#L756))。
- **宣言(HW-1)**: `FileWatcherOptions::watch_paths` が `nullopt` なら全部を見ます。配列なら、constructor が届かない mount の store を外し(空の配列なら全部。[`Impl` の constructor](../../src/core/watch/filewatcher.cpp#L429))、`scan()` は `watchPathReaches()` で届かないフォルダに入らず、[`watchPathCovers()`](../../src/core/watch/filewatcher.cpp#L383) で外のファイルを inventory に入れません。`onlyIgnoredRecords()` は宣言の外の名前も無視してよい名前に数えます。gate の側は [`ReloadGate::updateLocked()`](../../src/core/watch/reloadgate.cpp#L37) で、`WatchDeclaration` が undeclared のときだけ rpc 駆動を閉じる理由にし、empty は `"hot_reload empty"` で閉じます。`--rpc` の下では [`ReloadService::applyFrame()`](../../src/core/watch/reloadservice.cpp#L920) が watcher の要求を `pelican.render_pipeline` の参加者に claim させません(`applyRequests()` の `excluded_participant`。`applyRequestsForTesting()` は誰も外さない)。
- test: `filewatcher_test` の FW-1 の 7 本(起動時の hash 0 と 1 ファイルの書き換え、同じ中身の再保存、未知の live の状態機械、polling、overflow、`.pelican`、`Sha256Stream`)と HW-1 の 3 本(当たり判定と hash、届かない store、gate)、[`reloadtransaction_test.cpp` 内](../../test/reloadtransaction_test.cpp#L472)「HW-1 a declared watch runs under an RPC driver and never reaches the render pipeline」(本物の watcher thread で `passes/main.json` を書き換え、rpc では render pipeline の名前の fake が 0 回、rpc なしでは 1 回呼ばれる)、GPU の [`texturereload_test.cpp` 内](../../test/texturereload_test.cpp#L240)「HW-1 under an RPC driver the watch reloads a texture in the declared folder and not one outside」。

> 🧩 **難所 — 自分の書き込みを一度だけ**([`ContentDigestState::observe()`](../../src/core/watch/contentdigest.cpp#L182) / [`registerSelfWrite()`](../../src/core/watch/contentdigest.cpp#L171))
>
> **何をする所か**: watcherが見つけたdigestを「外部の変更」と「エンジン自身の保存」へ振り分け、reloadを積むかどうかを返します。
>
> **素朴に読むと**: digestが一致していてepochも一致しているのにreloadを積む経路があるのが引っかかります。エンジン自身が保存したファイルは、watcherから見れば外部変更と区別できません。そこで保存側が「このdigestが来たら自分の書き込みだ」というトークン([`SelfWriteToken`](../../src/core/watch/contentdigest.hpp#L42))を置きます。条件は4つあり、digest一致 / epoch一致 / `runtime_applied` / `live_digest` 一致が揃ったときだけ `consumed_self_write` になります。`match` と `may_consume` を先に計算したうえで `state.self_write.reset()` を分岐の**前**に無条件で実行するのが要点で、トークンは1回の観測で必ず消えます(消費されても、外れても)。消えなければ、一致し続ける限り永久にリロードが抑止されます。`runtime_applied` がfalseのときに消費しないのは「ディスクには書けたが、実行中の状態への適用は失敗した」場合で、そのとき `registerSelfWrite()` は `live_digest` を更新しない([その代入](../../src/core/watch/contentdigest.cpp#L176))ので、下の `live_digest == digest` の近道にも掛からず `queue_reload` になります — ファイルとランタイムが食い違ったまま静かに固定されるのを防ぐ、あえての積み直しです。epoch違いを `live_digest` 一致の近道へ落とさない([そのコメント](../../src/core/watch/contentdigest.cpp#L196))のはコメントが規範で、gateが閉じて開き直った後(= 世界が入れ替わったかもしれない後)は古いtransactionの抑止を効かせない、という決めです。
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
> **手がかり**: `observe()` が比較するのは前回の**ディスクinventory**ではなく `live_digest`(= ランタイムへ実際に適用済みの内容)です。[`reconcile()` 内](../../src/core/watch/filewatcher.cpp#L721) のコメントが規範で、「一度失敗した候補は、バイトが同じでも次の保存/reconcileで再び対象になる」ためにこの比較になっています。トークンを置くのは [`FileWatcher::registerSelfWrite()`](../../src/core/watch/filewatcher.cpp#L930) 経由で、`runtime_apply_succeeded` をそのまま持ち回ります。テストは [`filewatcher_test.cpp` 内](../../test/filewatcher_test.cpp#L143)「ContentDigest streams bytes and separates observed live pending and self-write」と [batch commit の検査](../../test/filewatcher_test.cpp#L192)。
>
> **不変条件**: トークンは1回の観測で必ず消える。epochが違うトークンは抑止に使わない。`runtime_applied` がfalseの保存はreloadを積み直す。

## 3.11 Component codec（authored ↔ canonical ↔ runtime） ✅実装済み

componentのJSONをどう受理し、どう書き戻し、どうruntimeへ当てるかの**正は [`componentcodec.hpp`](../../src/core/loader/componentcodec.hpp#L309) に移りました**（WP151）。scene loaderやエディタは、それぞれ独自にJSONを解釈するのではなく、このテーブルを引きます。

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

この五つ（decode / encode / schema / apply / project）が**全codecに必須**です。値の運搬は `ComponentCodecValue = std::any` です。`decode_authored` は例外ではなく [`ResolutionResult<ComponentCodecValue>`](../../src/core/loader/resolutionresult.hpp#L55) を返し、登録テーブルでは `decodeResult<decodeXxx>` が生の decode 関数を包みます。`schema` が返すのは平坦な `StructFieldSchema` 列ではなく**再帰記述子** [`const SchemaNode &`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L155) で、平坦な列が要るときだけ [`fieldSchema()`](../../src/core/loader/componentcodec.hpp#L344) が作り直します（後述）。

`editor_prepare` / `editor_swap` の2本は必須ではありません（transform / simplemodelview / camera / light / collider は `nullptr`）。[`prepareEditorSwap()`](../../src/core/loader/componentcodec.cpp#L2184) は `editor_policy` が `EcsCodec` で、かつ両方が非nullのときだけ通し、そうでなければ `logic_error` です。[`EditorProjectionPolicy`](../../src/core/loader/componentcodec.hpp#L34) に既定値が無いのは意図で、ヘッダのコメントどおり「codecを増やすなら、live projectionを所有するruntime participantを選ぶ」ことを強制します。

`runtime_apply` と `runtime_project` がどの型を指すかは [`ComponentCodecRuntimeKind`](../../src/core/loader/componentcodec.hpp#L24) が決めます。ヘッダのコメントが規範です — transform は `TransformCodecTarget`、その他のECS codecはそのcomponent自身、camera/light は `CameraCodecData` / `LightCodecData`、collider は `ColliderComponent` です。

この中でtransformだけが、componentを直接指さずに [`TransformCodecTarget`](../../src/core/loader/componentcodec.hpp#L69)（`world` / `local` / `parent_world` の三つのポインタ）を経由します。authoredなのは **local TRS** ですが、ランタイムで全員が読むのは world の `TransformComponent` で、`LocalTransformComponent` を持たないobjectもある、という食い違いがあるためです。GP-1 以降、scene load と editor の spawn では transform を持つ全objectに local が付きます（§4）。付かないのは editor の `add_component` で後から `transform` を足したobject（次の load まで）と、`load_gltf` の transient object です。そのため `runtime_apply` は local へ書いたうえで world も自分で合成し直し（[`applyTransform()`](../../src/core/loader/componentcodec.cpp#L660)）、`runtime_project` は local があればそれを返し、無ければ `parent_world` を使って world から逆算します（[`projectTransform()`](../../src/core/loader/componentcodec.cpp#L675)）。`parent_world` は両方向の変換に必要な係数で、これが無いと親の下のobjectについてlocalとworldを行き来できません。editor の読み取り（[`queryEditorRuntime()`](../../src/core/communication/editorruntimefactory.cpp#L896) の `tryComponent<LocalTransformComponent>`）もこの `runtime_project` を通るので、`get_components` の `local_trs` は root でも注入された local（= authored の値）を返します。GP-1 以前の root では local が無く、world をそのまま返していました。

登録テーブルは8行です（[`componentcodec.cpp` 内](../../src/core/loader/componentcodec.cpp#L2008)）。ただし**見えるのは常に8行とは限りません**（最後の行だけは後述のgate付きです）。

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

transformだけは専用の投影関数 [`projectTransformRuntimeJson()`](../../src/core/loader/componentcodec.hpp#L444) を持ちます。

> **設計決定:** transformの「authored local TRS」と「表示専用のworld投影」を混同しない、という要件をコメントで明文化したうえで別関数に分けています。エディタが表示している値と、保存される値が同じものだと誤解しないための境界です。

未登録の名前を問い合わせても例外にはならず、[`componentCodecQueryMetadata()`](../../src/core/loader/componentcodec.hpp#L397) が [`ComponentCodecState::Missing`](../../src/core/loader/componentcodec.hpp#L49) を返します。エディタは「知らないcomponentは編集不可として表示する」ことができ、未知componentを含むsceneを開けなくなることがありません。

テストは [`test/componentcodec_test.cpp`](../../test/componentcodec_test.cpp)、fixtureは [`test/fixtures/component_codec/`](../../test/fixtures/component_codec) の `valid.json` / `invalid.json` です。

### `pelican_ray_instance` は二重にgateされる ✅実装済み

9番目(末尾)のcodecだけは、**テーブルに載っていても引けるとは限りません**。[`componentCodecs()`](../../src/core/loader/componentcodec.cpp#L2224) は [`rayInstanceCodecEnabled()`](../../src/core/loader/componentcodec.cpp#L2217) が偽なら `codecs` の**先頭8要素だけ**を span として返します（[その `first()`](../../src/core/loader/componentcodec.cpp#L2232)）。判定は [`RayLayoutRuntimeState::builtinRayInstanceEnabled()`](../../src/core/shader/raygrouplayout.hpp#L177) で、`configure()` 済みかつモードが `LayoutV3`(layout がある。MG-2a で layout の版は 3 だけになりました)のときだけ真です。モードを固定するのは起動の1点だけで、組み込みComponent登録より前です（§2.1 の 7番）。同じ述語がECS側の登録も分けるので（[`predefined.cpp` 内](../../src/core/ecs/predefined.cpp#L38)）、「codecはあるがComponentが無い」状態は作られません。テストからは [`findComponentCodecIncludingDisabled()`](../../src/core/loader/componentcodec.hpp#L420) でgateを迂回できます。

WP382dから[`decodeRayInstance()`](../../src/core/loader/componentcodec.cpp#L1743)は`overrides`を可変長の行として受理します。省略した値は継承し、明示した0や空のflagsは上書きとして保持します。実行時のcatalogとAS planをscene/editorの候補に含め、公開・rollbackで一緒に切り替えます。

previewでは共通decode入口の[`componentDecodeSurfaceViolation()`](../../src/core/loader/componentcodec.cpp#L2119)が`ray_instance_overrides_unsupported@1`と`surface=preview`を返します。共通codecにはWeb用surface制約もありますが、別repoのWebパーサへの接続はWeb側の担当です。旧`overrides_not_enabled`分岐とそのlegacy branch IDは削除しました。

そのほかの受理規則も全て名前付きの legacy branch で拒否します: `instance_flags` は文字列配列のみ、重複flagは拒否、未知flag名は `ray_as_unknown_flag`、`force_opaque` と `force_no_opaque` の同時指定は `ray_as_conflicting_instance_flags`（[その検査](../../src/core/loader/componentcodec.cpp#L1714)）、`mask` は 0..255、`custom_index` は `rayCustomIndexMaximum` まで、そして**scalar 3項目もoverridesも省略された宣言**は `ray_instance_empty` です（[その検査](../../src/core/loader/componentcodec.cpp#L1827)）。

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

平坦な列からの持ち上げは [`SchemaNode::fromFlat()`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L230)（v2の記録をそのまま包む。子は作りません）、宣言DSLからの構築は [`materializeSchemaNode()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L972) で、`List` のときだけ行型の schema へ**再帰**します（[その1行](../../src/core/userpublic/details/schema/structfieldjson.hpp)）。[`recursive()`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L160) は `Flags` か `List` が1つでもあれば真で、これが §2.11 のv3入口の選択に使われます。fingerprintは [`schemaNodeJson()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L1011) の `dump()` そのものです（[`schemaNodeFingerprint()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L1040)）。

`List` のJSON表現は**オブジェクトのコンテナ**で、行のidを持つ封筒が付きます。

```json
{"waypoints": {
  "$list": {"v": 1, "next_id": 3},
  "rows": [
    {"$row": {"id": 1, "v": 1}, "pos": [0,0,0]},
    {"$row": {"id": 2, "v": 1, "hint": {"label": "goal"}}, "pos": [1,0,0]}]}}
```

decode（[list の枝](../../src/core/userpublic/details/schema/structfieldjson.hpp#L589)）が課す規則は次で、いずれも名前付きの `StructFieldErrorCode` です。

- コンテナが object でなければ `ListContainerMissing`。`$list` と `rows` 以外のキーは `UnknownField`。
- `$list` は `v` と `next_id` の2つだけ（他は `UnknownField`）。`v != 1` は `RowEnvelopeUnsupported`。
- 各行は `$row` を必須とし、その中身は `id` / `v` / `hint` の3つだけ。`id == 0` は `OutOfRange`、`v != 1` は `RowEnvelopeUnsupported`、`hint` は object のみで、**そのまま文字列として**保持されます（`hint_json`）。
- `id` の重複は `DuplicateRowId`。最後に `next_id` が**全行のidより大きい**ことを確認し、破れば `OutOfRange`（`$list.next_id must exceed every row id`）。
- 行の本体は `$row` を除いた残りを行型のschemaで [`decodeStructObject()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L754) へ渡します。エラーのpathは `field.rows[2].member` の形になります。

encode（[list の枝](../../src/core/userpublic/details/schema/structfieldjson.hpp#L589)）は逆向きに `$list` → `rows` の順で `ordered_json` を組み、`hint_json` は保存時のテキストを parse し直して戻します。`List` フィールドの既定値は常に**空**です（[`setStructDefault()`](../../src/core/userpublic/details/schema/structfieldjson.hpp#L317) の `Member{}`。宣言側も [`EmptyListDefault`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L589) の1引数形しか受け付けません）。行の型がさらに `List` を持つことは [`validRecursivePolicyField()`](../../src/core/userpublic/details/schema/structfieldschema.hpp#L804) が拒否するので、入れ子は**1段だけ**です。

> **設計決定:** 行のidを配列添字ではなく封筒で持つのは、行を並べ替えても挿入しても外部参照が切れないようにするためです（§3.12 の `AuthoringObjectId` と同じ動機）。`next_id` を単調に保つ検査がその保証の要で、ここを緩めると同じidが再利用され、参照が黙って別の行を指します。

なお `ComponentCodec::fieldSchema()` が返す平坦な列は **rootの直下の子だけ**を写します（[その実装](../../src/core/loader/componentcodec.cpp#L2155)）。`List` の行schemaはそこに現れないので、平坦な列を消費する側（schema駆動のinspectorなど）は再帰schemaを完全には表現できません。

### 別の field の値で読まれる field —— `ComponentCodecFieldCondition`（SC-1）✅実装済み

`StructFieldSchema` には「この field は別の field がこの値のときだけ読まれる」という条件の欄がありません。✅SC-1（`14e10b19`）は camera の controller をその形で schema に載せるため、条件を schema の外の表に置き、editor の wire で field の隣に付けます。

- **宣言**: [`camera_schema` の controller の部分](../../src/core/loader/componentcodec.cpp#L156) は、`controller.type` を `enumValues("orbit", "follow", "fly")` の `required` にし、`target` / `distance` / `yaw` / `pitch`（unit `rad`）/ `sensitivity` / `offset` / `speed` / `damping` を足しました。どれかの type で省ける field は `defaulted`（値は省いたときに読まれる値）、読む type の全部で必須の field は `required` になっています（宣言の並びの読み）。並べた値は [`decodeController()`](../../src/core/loader/componentcodec.cpp#L841) が読む鍵から取っています。`yaw_degrees` / `pitch_degrees`（`yaw` / `pitch` の別名）と `up`（受けるが読まない）は入れていません。
- **条件の表**: [`componentCodecFieldConditions()`](../../src/core/loader/componentcodec.cpp#L2304) が codec 名から [`ComponentCodecFieldCondition`](../../src/core/loader/componentcodec.hpp#L406) `{field, discriminant, applies_to, required_for, default_value}` の列を返します。camera 以外は空です。`default_value` は宣言の `Defaulted` の node から `schemaDefaultJson()` で取ります。`decodeController()` の既定値（`sensitivity` の 1、`yaw` / `pitch` の 0 など）は別に書かれていて、両者が一致することは下の codec の test が確かめます。`damping` は codec がどの type でも読みますが、動かすときに読むのは orbit と follow だけ（`cameracontrollersystem.cpp`）なので、`applies_to` は orbit / follow です。
- **wire**: [`codecSchemaFieldJson()`](../../src/core/communication/editorcommandservice.cpp#L100) が `schemaFieldJson()` の結果に `default`・`discriminant`・`applies_to`・`required_for`（空でなければ）を足します。呼ぶのは component の query（`get_components` / `scene_tree` の `schema.fields`。[query の側](../../src/core/communication/editorcommandservice.cpp#L1822)）と [`getComponentAddCandidates()` の `schema_fields`](../../src/core/communication/editorcommandservice.cpp#L933) の 2 か所です。wire の形は [manual 第13章](../manual/13_editor.md) §13.5。
- **prefab**: prefab の bindable descriptor は [`componentCodecs()` の `fieldSchema()` を回る所](../../src/core/loader/prefabprovider.cpp#L292) で、json pointer が一致する descriptor の型・範囲・enum を codec の schema から受けます。camera の `/controller/*` の descriptor は SC-1 の前から [`builtinPrefabBindableDescriptors()`](../../src/project/prefab.cpp#L720) の表にあり（type ごとの条件は `BindableDiscriminant` でその表が自分で持つ）、SC-1 で codec の schema に controller の field が入ったので、`/controller/type` などの型・範囲・enum がこの上書きで codec の値になります（コードの読み。prefab の側の test は確かめていません）。`componentCodecFieldConditions()` の表は descriptor には使われません。
- test: `componentcodec_test` の [「SC-1 camera controller schema states each type and the fields it reads」](../../test/componentcodec_test.cpp#L593)（条件の表を `decodeAuthored` / 正準化で突き合わせる。表が codec の読みとずれれば落ちる）、`editorcommandservice_test` の [「SC-1 get_components describes the camera controller by type」](../../test/editorcommandservice_test.cpp#L262)。wire の fixture `test/fixtures/wp360/get_components.parent.ndjson` も新しい field に合わせて書き換わっています。

## 3.12 AuthoringSceneDocument ✅実装済み

§3.2 で触れたsceneのdocument化の中身です。宣言は [`authoringscenedocument.hpp`](../../src/core/loader/authoringscenedocument.hpp#L95) です。

### load とidentity

[`AuthoringSceneDocument::load(scene_v1_bytes, revision, first_authoring_object_id)`](../../src/core/loader/authoringscenedocument.hpp#L114)（引数名に `v1` が残っていますが、渡すのは scene v2 の文書です）が、`SceneRevision` とセッション内で安定な [`AuthoringObjectId`](../../src/core/loader/authoringscenedocument.hpp#L26) を割り当てます。`AuthoringObjectId` は名前でも配列添字でもないため、objectをrenameしても並べ替えても、エディタ側の参照は切れません。

### authoring query と raw authority

三層のauthoring viewは [`AuthoringSceneAuthority::query()`](../../src/core/loader/authoringsceneauthority.hpp#L42) だけが取得します。`AuthoringSceneDocument` 自身からraw JSONやquery viewを取り出すpublic APIはなく、保存・journal・編集transactionのauthoring authorityと [`ResolvedSceneResolver`](../../src/core/loader/resolvedscene.hpp#L355) だけが型付き [`AuthoringSceneRawView`](../../src/core/loader/authoringsceneauthority.hpp#L16) を取得できます。Camera / DLL reload / editor runtime queryなどの読者はauthority headerへ依存せず、`ResolvedScene` の `source_json_exact` と `effective_json` を用途に応じて読みます。

| 型 | 内容 |
|---|---|
| [`AuthoringSceneView`](../../src/core/loader/authoringscenedocument.hpp#L55) | scene ID、authored JSON、object列 |
| [`AuthoringObjectView`](../../src/core/loader/authoringscenedocument.hpp#L43) | `AuthoringObjectId`、宣言順、name/parent、runtime entity ID、component列 |
| [`AuthoringComponentView`](../../src/core/loader/authoringscenedocument.hpp#L35) | 宣言順、authored JSON、`ComponentCodecQueryMetadata` |

componentごとに §3.11 のcodec metadataが付くので、「宣言としては存在するが、このビルドでは編集できない」状態をそのまま表現できます。

### stage と構造編集

改訂の作り方は二種類あり、どちらもauthoring authorityへ隔離されています。

- [`AuthoringSceneAuthority::stage(raw_document, revision)`](../../src/core/loader/authoringsceneauthority.hpp#L59) — **オブジェクト宣言のidentityを固定したまま**の未公開改訂。component配列やparentエッジは変えられますが、object宣言のidentityは動きません。
- [`AuthoringSceneAuthority::structuralStage()`](../../src/core/loader/authoringsceneauthority.hpp#L65) → [`AuthoringSceneDocumentStage`](../../src/core/loader/authoringscenedocument.hpp#L153) — object配列そのものを触る唯一の経路。`insertObject` / `removeObject` / `restoreObject` / `renameObject` / `reorderObject` を持ち、`finish(revision) &&` で確定します。変更種別は [`AuthoringStructuralChangeKind`](../../src/core/loader/authoringscenedocument.hpp#L72) の `Insert` / `Remove` / `Restore` / `Rename` / `Reorder` です。

実際にはどちらを使うかを呼び出し側が選ぶことはありません。[`EditorProjectionTransaction::commit()`](../../src/core/loader/editorprojectiontransaction.cpp#L883) がコマンド列を見てauthority経由で振り分けます（[その分岐](../../src/core/loader/editorprojectiontransaction.cpp#L915)）— `structural_apply` を持つコマンド（`makeInsertObjectCommand` / `makeRemoveObjectCommand` / `makeRestoreObjectCommand` / `makeRenameObjectCommand` / `makeReorderObjectCommand`）が1つでも混ざっていれば `structuralStage()` 経路へ、component値の書き換えやreparent（`makeSetComponentValueCommand` / `makeReparentCommand`）だけなら `stage()` 経路へ進みます。つまり「objectを増やす・消す・戻す・改名する・並べ替える」RPCが `structuralStage()`、それ以外の編集RPCが `stage()` に対応します。

**3 つ目の出し直し — `restagedAuthoring()`(✅PM-2)。**再生のセッションの Stop だけが使う private の [`restagedAuthoring(revision, minimum_next_object_id)`](../../src/core/loader/authoringscenedocument.cpp#L435) は、文書の中身と `AuthoringObjectId` をそのまま、新しい `SceneRevision` の文書として複写します。revision は 0 でなく今より大きくなければ投げ、次の `AuthoringObjectId` は自分の値と `minimum_next_object_id` の大きい方にします。つまり「中身は戻すが、番号は巻き戻さない」出し直しです(設計 [design_play_mode.md](../design_play_mode.md) §6.3 は、番号を巻き戻す私有の `publishPreparedSceneState` を使わないためにこの入口を足すとしています)。呼ぶのは friend の `ProjectBasicConfig::prepareRestoredSceneMetadata()`(§3.2)だけです。

> **設計決定:** `removeObject()` は破棄ではなく [`AuthoringObjectClosure`](../../src/core/loader/authoringscenedocument.hpp#L63) を**返します**。ここでのclosureは「取り除いた宣言を元の場所へそのまま戻すための記録一式」という意味で、関数のクロージャでもグラフの閉包でもありません。中身は `authoring_object_id` / `scene_id` / `declaration_index` / `previous_object_id` / `next_object_id` / `authored_json` の六つで（[`AuthoringObjectClosure`](../../src/core/loader/authoringscenedocument.hpp#L63)）、これだけあれば **同じ `AuthoringObjectId` を、消したときと同じ宣言区間（前後の兄弟の間）へ、JSONを一字も落とさずに戻せます** — 「無損失」はこの意味です。ヘッダのコメント通り、object配列の変更をこれらの操作だけに限ることで、metadataのidentity列とauthored JSONの並びがずれないことを型で保証しています。

> 🧩 **難所 — closureがidentityを運ぶ**([`restoreObject()`](../../src/core/loader/authoringscenedocument.cpp#L307) / journal側の [`finalizeJournal()`](../../src/core/communication/editorjournal.cpp#L3614))
>
> **何をする所か**: commit済みのjournal recordに、spawnしたobjectのclosure(id + scene + 宣言index + 前後の兄弟id + authored JSON)を後から書き足し、再生時は同じidを同じ宣言区間へ戻します。
>
> **素朴に読むと**: 「spawnのforwardをそのまま再実行すればundo/redoは戻る」と考えたくなります。ところが [`insertObject()`](../../src/core/loader/authoringscenedocument.cpp#L241) は呼ぶたびに `next_authoring_object_id_value_++` で**新しい** idを採番するので、再生後のidが元と違い、以後のjournal(全部idで対象を指す)が丸ごと外れます。だから `finalizeJournal()` はcommit直後にclosureを焼き込み、`commandFromCanonical()` はclosureがあれば `makeRestoreObjectCommand()` へ分岐します。もう一段難しいのが `restoreObject()` の挿入位置決定です。保存した `declaration_index` を盲信すると、間に他の編集が入っていたときにずれます。そこで前後の兄弟 **id** を現在の文書から引き直し、`next` があればその位置、無ければ `previous+1`、どちらも無ければ保存indexを配列長でクランプ、という三段の劣化戦略を取ります(`previous >= next` なら区間が反転しているので拒否)。
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
> **手がかり**: `previous_object_id` / `next_object_id` が「宣言区間」を index ではなく **id** で表しているのが要点です。`Insert` と `Restore` が別の [`AuthoringStructuralChangeKind`](../../src/core/loader/authoringscenedocument.hpp#L72) なのも同じ理由で、ランタイム側のadapterはこの種別で「新規entityを作るのか、同じauthoring idへ紐づけ直すのか」を決めます。テストは [`WP161 behavior attachment sequence survives spawn undo redo`](../../test/editorjournal_test.cpp#L1680) と [`editorprojectiontransaction_test.cpp` 内](../../test/editorprojectiontransaction_test.cpp#L495)(destroy interval)。
>
> **不変条件**: journalに載るspawnは必ずclosure付き(closureなしspawnは「新規採番」を意味する)。restoreは保存indexを盲信せず必ず前後idから引き直す。同一idの二重生存は禁止。

> 🧩 **難所 — 二段公開と巻き戻し順**([`EditorProjectionTransaction::commit()`](../../src/core/loader/editorprojectiontransaction.cpp#L883))
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
> **手がかり**: [`EditorProjectionPublicationMode`](../../src/core/loader/editorprojectiontransaction.hpp#L39) の `StagedNoexcept` と `InverseToken` の差が効きます。publish後のfault注入が `InverseToken` にしか適用されないのは、publish後でも逆トークンで戻せるadapterだけがそこで失敗を許されるからです。`rollback()` は「publish前ならprepared状態の破棄、publish後なら逆トークンの適用」の**両義**で、[`TransformProjectionAdapter::rollback()`](../../src/core/loader/editorprojectiontransaction.cpp#L859) の `published_` 分岐がその実例です。テストは [`editorprojectiontransaction_test.cpp` 内](../../test/editorprojectiontransaction_test.cpp#L312) / [`Structural spawn destroy and mixed command faults restore document and runtime exactly`](../../test/editorprojectiontransaction_test.cpp#L554)。
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
>
> ここで「無い」のは**この容器の話**である点に注意してください。root motion 自体はv0で出荷済みで、置き場が違います —— 共通グラフの `pose.clip_sample` ノードに `root_motion: "extract"` と `root_node` を書くと、clipの根の移動が個体のtransformへ渡ります（AG-2。パラメータの受理は [`poseClipSampleExtractsRootMotion()`](../../src/project/posegraphmodule.hpp#L45)、抽出の実装は [`extractRootMotion()`](../../src/core/animation/posegraphruntime.cpp#L322)、読み方は[第5章](05_gameplay_and_services.md)）。`.vrma` のretargetプロファイルとは別の層です。

### retarget

[`retargetVrmaClip()`](../../src/core/animation/vrmaretarget.hpp#L124) が、target rigのローカル座標系へ写した中間クリップを作ります。

- プロファイルはバージョン付きです（[`vrmaRetargetProfileVersion = 1`](../../src/core/animation/vrmaretarget.hpp#L16)）。
- [`VrmaRootMotionPolicy`](../../src/core/animation/vrmaretarget.hpp#L21) は現在 `preserve_hips_translation` のみです。抽出モードを持たないのは、後のプロファイルがv1データを読み替えずに方針を足せるようにするための、意図的な空きスロットです。**「Pelicanにroot motionが無い」わけではありません** —— 抽出は上の設計決定で触れたとおり `pose.clip_sample{root_motion: "extract"}` 側に実装されており、ここで空いているのは *retargetプロファイルが抽出方針を宣言する枠*だけです。
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
| `AnimationServiceRuntime::registerVrmaSource()` | [`animationservice.hpp` 内](../../src/core/animation/animationservice.hpp#L41) | `VrmaRetargetedClip` をobject名 + source名で登録 |
| `AnimationServiceRuntime::reloadVrmaSource()` | [`registerVrmaSource()`](../../src/core/animation/animationservice.hpp#L44) | 論理identityを保ったまま、そのsource assetのgenerationだけを進める |

ABI側では [`AnimationClipKindV1`](../../src/core/userpublic/animation/abi_v1.hpp#L98) が `skeletal_clip` / `vrma_retargeted_clip` を区別します。clip descriptorの**加算的な末尾**（[`abi_v1.hpp` 内](../../src/core/userpublic/animation/abi_v1.hpp#L171)）に `profile_version` / `asset_identity` / `asset_generation` / `source_rig_sha256` / `target_rig_sha256` が載り、[`CursorStatusV1`](../../src/core/userpublic/animation/animgraph.hpp#L66) にも同じprovenanceが出ます。

> **設計決定:** skeletal clipと別語彙を作らず、`AnimationClipKindV1` とprovenanceフィールドで区別しています。ゲームコードから見たClip/Cursorの扱いは`.vrma`由来かどうかで変わらず、reloadは論理identityを保ったままgenerationだけを進めます。ABIの追加は既存フィールドの後ろへの加算なので、古い呼び出し側は `reserved2` までで読み止められます。

テストは [`test/vrmadecoder_test.cpp`](../../test/vrmadecoder_test.cpp) / [`test/vrmaretarget_test.cpp`](../../test/vrmaretarget_test.cpp) / [`test/vrmasource_test.cpp`](../../test/vrmasource_test.cpp)（GPU）、fixtureは [`test/vrma_fixture_writer.cpp`](../../test/vrma_fixture_writer.cpp) + [`test/vrma_fixture.hpp`](../../test/vrma_fixture.hpp) が生成します。

## 3.14 rendering configの根は `pelican.graph` だけ ✅実装済み（P-2a-4a / P-2a-4b / MG-2b）

`rendering_config_json` が指すファイルの**根**は `pelican.graph` root だけです。共通グラフ文書の5キー（`schema` / `version` / `definitions` / `nodes` / `outputs`）の横に `rendering` キーがあり、rendering config の中身はその下に入ります。出荷7本と `devcli project init` の雛形がすべてこの形です。

P-2a-4b までは、`rendering` 相当の中身を最上位に直接書く **legacy root** も契約でした。MG-2b でその読み手を撤去し、legacy root は `render_config.legacy_root@1`（[`RenderConfigLegacyRoot`](../../src/project/renderconfigdocument.hpp#L37)）で断ります。断る場所は、起動時にファイルを最初に読む [`loadRenderingPassConfigJsonFromString()`](../../src/core/renderingpass/renderingpassconfigloader.cpp#L16)、feature overlay の [`applyRenderFeatureOverlays()`](../../src/project/renderfeatureoverlay.cpp#L122)、ray tracing の [`loadRayTracingProjectConfig()`](../../src/core/shader/raygrouplayout.cpp#L1452)、編集文書の [`AuthoredRenderConfigDocument`](../../src/project/renderconfigdocument.cpp#L546) です。出荷の legacy root 5本は `{"schema":"pelican.graph","version":1,"definitions":{},"nodes":[],"outputs":{},"rendering":<旧い根>}` に書き換えられました。

**「authored な `rendering` キーがどこにあるか」を決める場所は1つ**で、[`src/project/renderconfigsection.hpp`](../../src/project/renderconfigsection.hpp#L12) の小さなヘッダに全部入っています。読み手も書き手もここを呼ぶ規約で、各所が自前の `if (graph)` を育てないためです。

- [`isRenderGraphRoot()`](../../src/project/renderconfigsection.hpp#L22) — `schema == "pelican.graph"` かどうかだけ。`nlohmann::json` 用と [`ordered_json` 用](../../src/project/renderconfigsection.hpp#L27)の 2 定義があるので、このヘッダは**3 つの名前 / 4 定義**です。
- [`renderingSectionOf()`](../../src/project/renderconfigsection.hpp#L36) — `rendering` メンバを返します。根が `pelican.graph` でなければ `RenderConfigLegacyRoot` を、`rendering` オブジェクトが無ければ [`RenderConfigSectionUnlocated`](../../src/project/renderconfigdocument.hpp#L22)（メッセージは `graph root render config has no rendering section object`）を投げます。MG-2b より前は、graph root でない根に対して**根そのもの**を返していました。
- [`renderingSectionOrEmpty()`](../../src/project/renderconfigsection.hpp#L49) — 同じ判定の**読み取り用**。節が無ければ空オブジェクトを返します。legacy root はこちらでも断ります（`RenderConfigLegacyRoot` は捕まえません）。

この2本の使い分けが不変条件です。**読みは節が無くても失敗せず（「ここには何も書かれていない」と答える）、編集だけが名前付きで断る。** graph root に `rendering` を持たない文書は正当（グラフだけを書いた文書）なので、読むたびに落ちては困る一方、そこへ authored な値を書こうとするのは行き先が無い操作だからです。

**包む関数はもうありません。** P-2a-4b では `wrapLegacyRenderingConfig()` が5キーを付けて legacy 設定を `rendering` へ入れる唯一の場所でしたが、読み手が graph root しか受けなくなったので MG-2b で撤去されました。雛形の [`rendering_config_json`](../../src/project/projecttemplate.cpp#L199) と [`blank_rendering_config_json`](../../src/project/projecttemplate.cpp#L228) は graph root の全文を直接書いています(blank の `present` の fragment は VI-2 から `engine://passthrough` で、雛形は shader を書きません)。

**`rendering` 節には `buffers` / `compute_tasks` を書けません。** graph root の lowering（[`lowerRenderGraphDocument()`](../../src/project/rendergraphcomposition.cpp#L1354)）が、この2つを graph node（`render.buffer` / `render.compute`）の接続から作るため、節にあれば `render graph derives field from connections: <key>` で拒否します。この規則は MG-2b より前からあります。legacy root には lowering が無かったので根に書けましたが、legacy root が無くなったことでその書き方も無くなりました。テストで根に宣言していたものは task feature へ移されています。エディタの「root scope で compute_tasks / buffers を編集する」経路はこれで到達不能になり、後の掃除の WP（台帳の MG-3）に回っています。

編集側の唯一の書き手は [`AuthoredRenderConfigDocument::withSectionValue()`](../../src/project/renderconfigdocument.cpp#L691) で、graph root 自身の5キー（`schema` / `version` / `definitions` / `nodes` / `outputs`）と editor の `layout` は根へ、それ以外は `rendering.<key>` へ書きます。この関数はJSONを組み直さず、元のバイト列の中で該当メンバの範囲を差し替えます（[`locateSectionMembers()`](../../src/project/renderconfigdocument.cpp#L305)）。理由は digest がファイル全体のバイト列に対して取られるためで、詳細は[第6章](06_rendering_vulkan_shader.md)にあります。

## 3.15 共通グラフ文書 `pelican.graph` ✅実装済み（AG-0 / P-1）

`pelican.graph` は「スカラ・描画・姿勢をまたぐ1本の宣言グラフ」の文書形式です。純粋層だけで**展開 → 検証 → 実行領域への分割**まで進み、GPUにもECSにも触れません。

### 文書の形

根が持つのは `definitions` / `nodes` / `outputs`（と `schema` / `version`、rendering config を兼ねる場合は `rendering`）です。

- `nodes` — 根のノード列。各要素は `id` と `op`、任意で `inputs` / `parameters` / `outputs` / `label`。
- `definitions` — 名前付きの部分グラフ。中に `graph.input`（境界ポート）を置けます。
- `graph.instance` — 定義を根へ1個体として置くノード。`definition` と `owner`（`{scene, object}`）を持ちます。

この2つの構造用opは置ける側が決まっていて、`graph.instance` を定義の中へ入れると `graph.nested_instance@1`、`graph.input` を根へ置くと `graph.input_scope@1` で拒否されます（[`editGraphDocument()` 内](../../src/project/graphcomposition.cpp#L2304)）。

### 展開 — `expandGraphDocument()`

[`expandGraphDocument()`](../../src/project/graphcomposition.hpp#L174) が instance を実体化し、[`ExpandedGraphDocument`](../../src/project/graphcomposition.hpp#L158) を返します。ここで大事なのは**元の文書が正本のまま**という点で、展開後のIDや由来（`origins`）は導出物であって書き戻されません。

instance の入力ポートがどこから値を取るかは `edge > parameters > default` の順で決まり、結果は [`GraphInstanceInput`](../../src/project/graphcomposition.hpp#L140) に記録されます。edge で繋がれたポートは**展開後にノードを残しません**（定義の中の消費側が外の生産者を直接指す）ので、この由来は「展開中の authoring 文書」からしか読めません。

### 編集 — `editGraphDocument()`

[`editGraphDocument()`](../../src/project/graphcomposition.hpp#L245) は op の並びを1本の候補文書へ順に適用し、**最後に1回だけ展開**します。途中で1つでも落ちれば候補ごと捨てられ、呼び手の文書は無傷です。受け付ける op は5種で、**宣言の上のコメントがその5種と8つの名前付き拒否を列挙しています**（`40099e3f` で `set_node_label` と残りの拒否名が追記され、コメントと実装が揃いました）。

| op | 実装 | 断り方 |
|---|---|---|
| `add_node` | [.cpp](../../src/project/graphcomposition.cpp#L2292) | `graph.node_duplicate@1` / `graph.nested_instance@1` / `graph.input_scope@1` |
| `remove_node` | [.cpp](../../src/project/graphcomposition.cpp#L2333) | `graph.output_referenced@1`（出力や他ノードがまだ指している） |
| `set_input` | [.cpp](../../src/project/graphcomposition.cpp#L2364) | `source` は `{node, port}` ちょうど2キー、または `null`（切断） |
| `set_node_parameters` | [.cpp](../../src/project/graphcomposition.cpp#L2402) | 空オブジェクトは `parameters` キーごと削除 |
| `set_node_label` | [.cpp](../../src/project/graphcomposition.cpp#L2415) | 人が付ける表示名。識別子ではないので、消すのは空文字列で表す |
| `set_layout` | [.cpp](../../src/project/graphcomposition.cpp#L1083) | scope の editor 用 `layout`(node 位置 / reroute / tie)を丸ごと置換。`null` で消す。名指した node / 辺が無い、座標が非有限、未知 key は `graph.layout_invalid@1`。layout だけの op 列は再 compose しない(N-6) |
| `add_definition` / `remove_definition` / `rename_definition` | [`applyDefinitionOp()`](../../src/project/graphcomposition.cpp#L2220) | ✅LP-0c。`graph.definition_duplicate@1` / `graph.definition_missing@1`。改名は名指す `graph.instance` と `graph.loop` の分岐を同じ op で書き換える。「消した definition がまだ名指されていないか」「instance / ループの供給する port が definition にあるか」「消した `graph.input` をまだ誰かが供給していないか」は op ごとではなく列の最後に 1 回([`runDeferredCheck()`](../../src/project/graphcomposition.cpp#L2126)) |

各opは任意で **`scope`** を取ります。省略（または根の名前）なら根の `nodes`、定義名なら `definitions.<名前>.nodes` を編集対象にします（[`resolveEditScope()`](../../src/project/graphcomposition.cpp#L1524)）。scope は op ごとに解決し直します —— 同じ並びの前のopが定義の中身を変えている可能性があるからです。定義の中に居る id を根から触ろうとすると `graph.definition_immutable@1` です。

[`editGraphInstances()`](../../src/project/graphcomposition.hpp#L251)（instance の着脱）は、自分で文書を触らず**上のopへ翻訳してから** `editGraphDocument()` を呼びます。2つの入口が「何が妥当な編集か」で食い違えないようにするためです。

### ループと合成の後の解決の段 ✅実装済み(LP-0a〜LP-Vb)

`graph.loop`(書き方の正は manual 第6章 §6.18)は `graph.instance` と同じ構造の node で、処理が 2 つの段に分かれます。**形の検査は展開器**([`expandGraphDocument()`](../../src/project/graphcomposition.cpp#L1137) の中のループの展開)が編集と読み込みのたびに行い、各分岐の body を `<loop>/$<branch>/` の名前で 1 回だけ**雛形**として展開します(port の型の検査のため)。**回ごとの複写は合成の後**で、preset と feature の合成が済んでから [`resolveGraphDocumentAfterComposition()`](../../src/project/rendergraphcomposition.cpp#L1511) が [`expandRenderGraphLoops()`](../../src/project/rendergraphcomposition.cpp#L2192) を呼び、雛形の名前 `<loop>/$<branch>/` を `<loop>/<element>/` へ付け替えます。lowering はそのために、合成の後で使うもの(target の参照、history の読み、置き場所待ちの run、ループの body、ループの出力を読む側、draw_loop)を [`LoweredRenderGraphDocument`](../../src/project/rendergraphcomposition.hpp#L200) に残します。

| ファイル | 持ち物 |
|---|---|
| [`graphloop.hpp`](../../src/project/graphloop.hpp#L122) | ループの形 `GraphLoopShape`、段の分類 `classifyGraphLoopTier()`、要素 `graphLoopElements()`、上限(1 ループ 64 回、graph 全体 128 本) |
| [`graphloopexpression.hpp`](../../src/project/graphloopexpression.hpp#L20) / `.cpp` | `$expr` / `$format` の文法(冒頭の BNF が正本)、検査と評価。実数は 64 bit 整数の分数で計算し、最後に 1 回だけ double にする |
| [`renderloopreport.hpp`](../../src/project/renderloopreport.hpp#L15) | `get_frame_plan` の `loops[]` と `loop_origin`、変種で非活性にした graph の node(`RenderGraphDeactivatedNode`) |
| [`rendergraphcomposition.hpp`](../../src/project/rendergraphcomposition.hpp#L26) / `.cpp` | `render.target_ref` の検査、置き場所の run(`placeRenderGraphPassRun()` / `placeDeclaredRenderGraphPassRuns()`)、expand の展開、`pass_replacements` の適用、変種で外れる feature への依存 |
| [`materialdrawloop.hpp`](../../src/project/materialdrawloop.hpp#L97) / `.cpp` | draw_loop の純粋層: `MaterialDrawStep`、material の照合の鍵、分岐の選び方、`get_frame_plan` の要素と paging、LP-2 の拒否 code |
| [`materialpassdecompose.hpp`](../../src/project/materialpassdecompose.hpp#L74) / `.cpp` | `decompose_material_pass` の文書の書き換え(editor 側は [`RenderConfigEditorService::decomposeMaterialPass()`](../../src/core/communication/renderconfigeditor.cpp#L3253) が 1 つの transaction に載せる) |
| [`viewselect.hpp`](../../src/project/viewselect.hpp#L21) | `view_select` の 4 つの拒否 code と、座標と選択の照合 |
| `src/core/resources/graphs/*.json` | 出荷する分解版の雛形 4 本(engine は読まない。`decomposes` を持つ) |

合成の中の順は [`composeRenderFeatureConfig()`](../../src/project/featurecompose.cpp#L3422) の中で、(1) graph の lowering(`pass_set` の既定は [`defaultGraphPassFamily()`](../../src/project/featurecompose.cpp#L3396))→ (2) `insert.declared` の run を宣言の列へ置く → (3) feature・integration の合成 → (4) 変種で外れる feature に依存する node を外す([`withoutExcludedGraphDependents()`](../../src/project/rendergraphcomposition.cpp#L2622))→ (5) 残りの run を置く → (6) surface resources の consumer の選択と snapshot → (7) 解決の段(target の参照の検査 → ループの展開 → draw_loop の報告 → [`applyRenderFeaturePassReplacements()`](../../src/project/rendergraphcomposition.cpp#L2804))→ (8) ループの回の中で consumer を選び直す、です。run を consumer の選択より前に置くのは graph で書いた material pass が影の map を受けるため(LP-3a)、(8) は置き換えた consumer の代わりの回が受けるため(LP-Vb)です。

**graph の node の出所(✅GL-2a)。**lowering は node が作った pass・compute task の名前ごとに、展開の [`GraphNodeOrigin`](../../src/project/graphcomposition.hpp#L82)(instance path・definition・node・loop・loop branch)を `LoweredRenderGraphDocument::origins` に残します([残す所](../../src/project/rendergraphcomposition.cpp#L1014))。ループの body の node は雛形の id `<loop>/$<branch>/<node>` で残ります。`applyRenderFeaturePassReplacements()` は、置き換え先ごとに置き換えた `{feature, pass}` を `RenderGraphLoopResolution::replacements` に返します(ループでは置き換えた pass と同じ family の回だけ。[返す所](../../src/project/rendergraphcomposition.cpp#L2895))。合成は provenance を同期した直後に、この 2 つを [`RenderPassProvenance`](../../src/project/renderpipeline.hpp#L277) の `graph_origin` と `replaces` へ載せます([載せる所](../../src/project/featurecompose.cpp#L4200)):root と instance の宣言は `graph_declarations` にある名前で引き、ループの回は `loop_origin` の `(loop, element)` が選んだ分岐から雛形の id を作って引き、`instance_path` を空にします。feature の provenance は飛ばします。`get_frame_plan` の `origin` / `replaces`(`framePlanToJson()`)と `get_compute_task_authoring` の `graph_node` は、どちらもこの 1 つを読みます(GL-2a の前は、editor が root を展開し直して task 名と突き合わせていました)。

**fold group の決定を展開名へ写す(✅FG-1)。**[`graphfold.hpp`](../../src/project/graphfold.hpp#L80) が root の `fold_groups` を読み([`parseGraphFoldGroups()`](../../src/project/graphfold.cpp#L128))、group の開いた形の digest を作り([`graphFoldGroupDigest()`](../../src/project/graphfold.cpp#L185))、印と比べて `verified` / `stale` / `unverified` を決めます([`reportGraphFoldGroups()`](../../src/project/graphfold.cpp#L240))。合成は上の `graph_origin` を provenance に載せた直後に [`applyGraphFoldGroups()`](../../src/project/graphfold.cpp#L269) を呼び([FG-1 の段を呼ぶ所](../../src/project/featurecompose.cpp#L4240))、`verified` の group の主語を展開名へ写して、合成した config の `target_planning` に `true` で書き込みます。pass と task は provenance の `loop_origin`(ループの回。`<loop>` の先頭の区切りが root の node)か `graph_origin`(instance の中なら `instance_path` の先頭)から root の node を引き、target と buffer は `loop_origin` か展開名の先頭の区切りで引きます。feature の provenance は飛ばします。合成した config が同じ entry を `false` にしていれば `graph.fold_decision_conflict@1` を投げます([投げる所](../../src/project/graphfold.cpp#L416))。結果は [`RenderFeatureComposeResult::fold_groups` / `fold_planning`](../../src/project/featurecompose.hpp#L73) に残り、`resolveRenderPipeline()` が印の無い・古い group を `fold_unverified` / `fold_stale` の診断にし([診断にする所](../../src/project/renderpipeline.cpp#L1400))、`fold_planning` の graph 名に変種の接尾辞を付けて([付ける所](../../src/project/renderpipeline.cpp#L1358))compiled pipeline へ渡します。`framePlanToJson()` はそれを `fold_decisions` として出し、`appendFramePlanPhysicalNodeReferences()` が `pelican.plan.fold_decision_applied@1` の要素にします(第7章)。editor は同じ `fold_planning` を authoring context に受け([渡す所](../../src/core/communication/editorruntimefactory.cpp#L2184))、EL-2 の `edit_target_planning` の `false` と [`graphFoldDecisionConflict()`](../../src/project/graphfold.hpp#L147) で突き合わせます。文書には展開名を書かないので、写しは組み立てのたびに作り直され、文書には残りません。

**graph の compute task を読む rendering 節の pass(✅RS-6)。**compute task は全 pass の後に宣言されるので、合成は canonical の順と terminal の待ちを明示した後に [`waitOnGraphTasksInScenePasses()`](../../src/project/rendergraphcomposition.cpp#L3075) を呼び([RS-6 を呼ぶ所](../../src/project/featurecompose.cpp#L4163))、graph の task(`graph_declarations` とループの展開の名前で選ぶ。✅RS-6b から `graph_features.feature_passes` に載る feature と integration の task も。[task を選ぶ所](../../src/project/featurecompose.cpp#L4157))が書く render target を、family ごとに最初の canonical anchor より前で読む最初の rendering 節・preset の pass に `after: <task>` を足します。足すかどうかは、frame planner と同じ順(`after` / `before` と、前に宣言された最後の書き手から読みへ)を family ごとに組んだ [`FamilyOrder`](../../src/project/rendergraphcomposition.cpp#L3022) の到達で決め、書き手がすでに読み手に届くときと、読み手が task に届く(足すと循環になる)ときは足しません。読み手が `input` に挙げる graph の `render.image` には `SAMPLED` を足します(RS-6b から feature の render target も。合成が `resource_provenance` の source が feature の名前を `graph_images` に加えて渡します。[加える所](../../src/project/featurecompose.cpp#L4162))。読み手の候補から feature の pass を外す判定は RS-6 のままなので、feature が `before` / `after` で決めた順は変わりません。足した組([`GraphTaskReaderWait`](../../src/project/rendergraphcomposition.hpp#L428))は、provenance を同期した後に `RenderPassProvenance::added_after`([宣言](../../src/project/renderpipeline.hpp#L301))へ載り([載せる所](../../src/project/featurecompose.cpp#L4180))、`framePlanToJson()` が node の `added_after` として出します。RS-6b: `SAMPLED` を足した image は [`GraphTaskReaderWait::sampled`](../../src/project/rendergraphcomposition.hpp#L434) に残り([残す所](../../src/project/rendergraphcomposition.cpp#L3148))、合成がそれを `RenderResourceProvenance::added_usage`([`added_usage` の宣言](../../src/project/renderpipeline.hpp#L326))へ載せ([`added_usage` に載せる所](../../src/project/featurecompose.cpp#L4188))、`framePlanToJson()` が `resources[]` の `added_usage` として出します。関数名 `waitOnGraphTasksInScenePasses` は RS-6 のままで、中身は graph と feature の task です。task が待つ置き場所の無い graph の pass の塊を読み手の前に動かすのは、RS-3 の [`graphPassRunReaderPosition()`](../../src/project/rendergraphcomposition.cpp#L1687) の後半です([RS-6 の分](../../src/project/rendergraphcomposition.cpp#L1741)。塊を待つ task を `after` / `before` で閉包に集め、そういう task だけが書く image の最初の読み手を、塊が直接書く image の読み手と比べて早い方を取ります)。

**graph の番号付きの入力の読み方(✅RS-14)。**graph module の [`renderPorts()`](../../src/project/graphmodule.cpp#L328) は `binding: "input"` の port について `view` だけを断り([断る所](../../src/project/graphmodule.cpp#L442))、`sampling` / `subresource` は値を見ずに通します。lowering([`lowerRenderGraphDocumentForComposition()`](../../src/project/rendergraphcomposition.cpp#L151))は番号付きの port を pass の `input` に並べるときに、その 2 つを `input_index` ごとに集め、pass の `input_settings` に番号の順の配列(設定の無い番号は `null`、設定を持つ最後の番号まで)として書きます([書く所](../../src/project/rendergraphcomposition.cpp#L983))。設定が 1 つも無ければ field を出さないので、書かない graph の合成結果は変わりません。node の `parameters` に `input_settings` があれば、`type` / `input` / `output` と同じく `render graph derives field from connections` で断ります([断る所](../../src/project/rendergraphcomposition.cpp#L821))。値の検査は engine が pass の JSON を読むときで、名前付きの port と同じ parser です(第6章 §6.2「番号付きの入力の設定」。書き方の正は manual 第6章 §6.18「番号付きの入力」)。

**body の image の大きさと `log2`(✅RS-15)。**式を許す場所の表 [`bodyExpressionPlace()`](../../src/project/graphcomposition.cpp#L586) に、body の `render.image` の `width` / `height` / `layers` / `mip_levels`(整数の式)が加わりました([該当箇所](../../src/project/graphcomposition.cpp#L593))。評価した整数は、書いた数と同じく render target を読む所で検査されます。式の評価器には `log2(n)`(正の整数の切り捨て。1 未満と小数は `not_integer`。[評価する所](../../src/project/graphloopexpression.cpp#L252))が、組み直し時の定数 [`composedConstants()`](../../src/project/rendergraphcomposition.cpp#L1920) には「`mip_levels: "full"` で `width` / `height` が正の整数、`format_class` が `display` でない target の `mip_levels_of` は `bit_width(max(width, height))`」が加わりました([該当箇所](../../src/project/rendergraphcomposition.cpp#L1931))。文法の正本の BNF([graphloopexpression.hpp](../../src/project/graphloopexpression.hpp#L20))も `log2` を含みます。

**必須の parameter と欠けた鍵(✅RS-15 / CT-1)。**op の必須の表 [`renderNodeRequiredParameters()`](../../src/project/graphmodule.cpp#L262) の検査は、✅RS-15 から [`GraphModuleRegistry::resolveOperation()`](../../src/project/graphmodule.cpp#L676) が契約を作る前に 1 回呼びます([呼ぶ所](../../src/project/graphmodule.cpp#L685))。✅CT-1 から、展開器が文書から鍵を読む所(`nodes` / `outputs` / `id` / `op` / `definition` / edge の `node` / `port`)は [`member()`](../../src/project/graphcomposition.cpp#L32) を通り、`<場所> requires <鍵>` の `std::invalid_argument` になります。render の pass の port は [`checkRenderPortDeclaration()`](../../src/project/graphmodule.cpp#L313) が先に見て、欠けを `graph.port_field_missing@1` にします。どちらも manual 第6章 §6.18 の「名前付きの拒否の一覧」の下に書き方があります。

**op の parameter の表(✅CT-3)。**op ごとの parameter の形は [`graphparameterschema.hpp`](../../src/project/graphparameterschema.hpp#L77) の 1 つの表で、catalog・RS-9 の必須・型の検査の 3 つがこれを読みます。

- **形**: 1 件は [`GraphParameterSchema`](../../src/project/graphparameterschema.hpp#L77) `{name, kind, required, alternatives, default_value, enum_values, min, max, items, fields, expression, description, checked_by_op}`、op 1 つ分は [`GraphOperationParameters`](../../src/project/graphparameterschema.hpp#L111) `{parameters, complete}` です。`items` は配列の要素か map の member の形で、`shared_ptr<const GraphParameterSchema>` で持ちます。
- **表**: [`makeTable()`](../../src/project/graphparameterschema.cpp#L386) が、小さな builder の `P`(`required()` / `fallback()` / `values()` / `range()` / `item()` / `field()` / `expression()` / `checkedByOp()`)で op ごとに書き、関数ローカルの static に 1 回だけ作ります。複数の pass が同じ鍵を読むもの(`view_select`・`insert`・load / store・buffer の `host_source` など)は、表の前の共有の関数にまとめてあります。`render.material_pass` の `material_contract` の `enum` は、EL-4 と同じ `materialPassContracts` から作ります([`render.material_pass` の CT-3 の行](../../src/project/graphparameterschema.cpp#L542))。
- **RS-9 と catalog**: `renderNodeRequiredParameters()` は [`graphOperationRequiredParameters()`](../../src/project/graphparameterschema.cpp#L847) を返すだけになりました(表の `required` の entry を表の順に、名前の後に `alternatives`)。`operationJson()` は同じ表から `required_parameters` と、各 entry を [`graphParameterSchemaJson()`](../../src/project/graphparameterschema.cpp#L794) で JSON にした `parameters` を作ります。表に無い op は `parameters: []`・`parameters_complete: false` です。
- **型の検査**: [`GraphModuleRegistry::resolveOperation()`](../../src/project/graphmodule.cpp#L676) が、RS-9 の検査の直後、factory を呼ぶ前に [`checkGraphNodeParameterTypes()`](../../src/project/graphparameterschema.cpp#L860) を呼びます。node の `parameters` にある鍵だけを表の順に見て、[`checkValue()`](../../src/project/graphparameterschema.cpp#L644) が kind ごとに再帰します。道筋は配列で `[<番号>]`、object で `.<鍵>` を足して作り、最初に合わない所で `graph.render_node_parameter_invalid@1` を投げます。`checked_by_op` の entry(今は `pose.clip_sample` の `root_node`)、JSON の `null`、[`isGraphLoopExpression()`](../../src/project/graphloopexpression.hpp#L102) に当たる式の object は、どの深さでも見ません。`fields` の中の `required` は見ません(op の parser が断るので、表示のための値です)。

manual 第6章 §6.17「各 op の parameter の形」と §6.18「名前付きの拒否の一覧」の CT-3 の項に、利用者から見た形があります。

**parameter の表の CT-4 の鍵と、op の置き場所(✅CT-4、`4d682338`)。**Studio が欄を作るための鍵を同じ表に足し、op ごとの置ける場所を表から検査するようにした変更です。CT-3 の検査は変わりません。

- **形**: `GraphParameterSchema` に `soft_min` / `soft_max`(`optional<double>`。slider の範囲で検査しない)、`asset_kind` / `extensions`、`choices_from`(`optional<GraphParameterChoices>`)、`variants`(名前無しの `GraphParameterSchema` の配列)が加わり、kind に `union_`(catalog の綴りは `union`)が加わりました。[CT-4 の `GraphParameterChoices` の宣言](../../src/project/graphparameterschema.hpp#L55) は `{rpc, key, field, graph_ops}` で、`graph_ops` が空でなければ `{graph_nodes: [...]}`、空なら `{rpc, key, field?}` として出ます。`key` の道と `field` の読み方はこの宣言の注記が正です。`GraphOperationParameters` には `placement`([`GraphOperationPlacement`](../../src/project/graphparameterschema.hpp#L69) の `anywhere` / `owned_instance`)が加わりました。
- **表**: builder の `P` に `softRange()` / `assetOf()` / `choicesFrom()` / `choicesFromGraph()` / `variant()` と、kind の関数 `union_()` が加わりました。表の前の共有の関数に `sceneModel()`(`.glb` / `.gltf` / `.vrm` の model)と `inheritBindingsFrom()`、定数に `authoring_context`(`"get_render_authoring_context"`)・`image_extent_soft_max`(8192)・`image_mip_levels_soft_max`(14)があります([CT-4 の定数と 8192 の出どころ](../../src/project/graphparameterschema.cpp#L146))。`add()` の最後の引数が placement(既定 `anywhere`)で、`pose.*` の 6 つと `value.morph_weight` が `owned_instance` です。
- **JSON と検査**: `graphParameterSchemaJson()` が新しい鍵を出し、`variants` の各要素は `items` と同じく名前無しで出します([CT-4 の鍵を出す所](../../src/project/graphparameterschema.cpp#L803))。`operationJson()` が各 op に `placement` を出します([CT-4 で op に placement を出す所](../../src/project/graphmodule.cpp#L588))。`checkValue()` は `union_` を `expression` / `json` と同じく見ません([CT-4 で union を素通しする所](../../src/project/graphparameterschema.cpp#L728))。
- **置き場所の検査**: [`checkGraphOperationPlacement()`](../../src/project/graphparameterschema.cpp#L789) は、owner の中でなく、表の placement が `owned_instance` の op なら `graph.op_requires_owned_instance@1` `{node, op}` を `GraphRegistryError` で投げます。表に無い op は `anywhere` です。呼ぶのは `partitionGraphDocument()` の node の走査の先頭で、展開した node id が owner 付きの instance の中かを `graphInstanceOwnerOf(graph.instance_owners, id)` で判定して渡します([CT-4 の placement の検査を partition で呼ぶ所](../../src/project/graphpartition.cpp#L374))。partition は編集でも読み込みでも通るので、どちらでも同じ code です(コードの注記)。`posegraph.cpp` の `pose operation requires an owned graph instance` の文は残っています(今この文に届く経路があるかは未確認)。
- **context**: `getRenderAuthoringContext()` が `view_families`(`mainRenderViewFamilyId` と、`renderViewFamilyProviderRegistry().providers()` の各 `family_id`)と `canonical_anchors`(新しい [`canonicalRenderPassAnchors()`](../../src/project/featurecompose.cpp#L48))を足します([CT-4 で context に足す所](../../src/core/communication/renderconfigeditor.cpp#L2651))。

test: `curve_test` の [「CT-4 the catalog says where a value is chosen from, an asset's kind, a union's shapes, a slider's range and where an op may stand」](../../test/curve_test.cpp#L1427)(catalog の値と、partition の拒否・owner を付ければ通ること)、`renderconfigeditor_test` の [「CT-4 an edit placing an owned-instance op at the root is refused by name」](../../test/renderconfigeditor_test.cpp#L1497)。

**`where` と `asset_manifest`、render.scalar の書き込み先の検査(✅CT-5、`65f3a8a3`)。**

- **形**: [`GraphParameterChoices`](../../src/project/graphparameterschema.hpp#L55) に `where`(`vector<pair<名前, 値の列>>`。RPC の一覧なら要素の member、`graph_ops` なら node の parameters の値が、列のどれかに合うものだけを残す)と `asset_parameter`(兄弟の parameter が名指す asset の `<asset>.import.json` の `key` を読む形)が加わりました。builder の `P` に [`choicesFromAssetManifest()`](../../src/project/graphparameterschema.cpp#L94) と [`where()`](../../src/project/graphparameterschema.cpp#L102) が加わり、`where()` は直前の `choicesFrom*()` が作った `choices_from` に条件を足します。
- **JSON**: `graphParameterSchemaJson()` は `graph_ops` → `{graph_nodes}`、`asset_parameter` → `{asset_manifest: {parameter, manifest: "<asset>.import.json", key}}`、それ以外 → `{rpc, key, field?}` の順に形を選び、`where` があれば値が 1 つなら文字列、複数なら配列で足します([CT-5 で where を出す所](../../src/project/graphparameterschema.cpp#L827))。
- **表**: `render.scalar` の `target` に `where("host_source", {"runtime_f32"})` と `where("element", render_scalar_target_elements)`、`imageType()` の `semantic` に catalog の `modules.types`(`where("constructor", {"image"})`)、新しい `regionTag()` / `accelerationStructure()` の共有の関数、`value.sample_table` の `row` の文字列の形に `choicesFromAssetManifest("asset", "source.metadata.layout.features")`、`render.fullscreen` の `push_constants` に enum が入りました。
- **render.scalar の書き込み先の検査**: [`checkRenderScalarTargets()`](../../src/project/graphpartition.cpp#L276) は、partition の値の domain の `render.scalar` を、書き込み先の graph の `render.buffer` / `render.attribute` ごとにまとめ、sink の合計の byte 数(`value` が無ければ 4、2〜4 個の数の配列なら × 4、それ以外は「数えられない」)と比べます。`element` が float 系でない buffer は飛ばし(`graph.render_scalar_type_mismatch@1` に任せる)、`host_source` は書いた値か、`source.pose_node` を持つ attribute なら `runtime_f32` と見ます。合わなければ `graph.render_scalar_target_incompatible@1` を投げます。呼ぶのは `expandLoopValues()` の直後です([partition から CT-5 の検査を呼ぶ所](../../src/project/graphpartition.cpp#L448))。graph の外で宣言された buffer は、登録の段の [`attachProgramGraphState()` の CT-5 の拒否](../../src/core/renderingpass/renderingpassconfigregistration.cpp#L1357) が同じ code で断ります(`node` は無し)。
- **context と instance**: `getRenderAuthoringContext()` が `acceleration_structures` と `render_target_vocabulary.format_class_patterns` を、`getGraphInstances()` が [`graphDefinitionInstances()`](../../src/core/communication/renderconfigeditor.cpp#L126) の `definition_instances` を足します。`editGraphInstances()` は `GraphRegistryError` を `graphRefusalInDocument()` に通し、code と field を保って返します([CT-5 の edit_graph_instances の catch](../../src/core/communication/renderconfigeditor.cpp#L2910))。

利用者から見た形は manual 第6章 §6.5・§6.17・§6.18 の CT-5 の項と第13章 §13.5 です。test: `curve_test` の [「CT-5 the catalog's remaining choices and a render.scalar writing an incompatible buffer」](../../test/curve_test.cpp#L1586) と、`renderconfigeditor_test` の CT-5 の 4 本([「CT-5 an edit writing a render.scalar into an incompatible buffer is refused by name」](../../test/renderconfigeditor_test.cpp#L1535) から)。

**拒否に node を書いた文書を足す(✅CT-2)。**`graph.*` の拒否を投げる所は今までどおりで、受け取る側が detail に `document` と `definition` を足します。規則は [`graphcomposition.hpp` の CT-2 の宣言](../../src/project/graphcomposition.hpp#L38) の注記が正です: engine が組む node・definition・ループの body は全部 rendering の root の `pelican.graph` 文書にあり(preset と authored pass は node を持たない feature、graph の template は root に写される)、`document` はいつも root 文書です。

- [`graphDocumentReference()`](../../src/project/graphcomposition.cpp#L1206) が参照を綴り直します。`project://` / `engine://` はそのまま、project からの相対 path は前に `project://` を付け、空・絶対・ほかの scheme は `nullopt`(field を出さない)。
- [`graphNodeDefinition()`](../../src/project/graphcomposition.cpp#L1219) が node の id から definition を引きます。区切りの無い id は root の `nodes` にあれば `nullopt`、無ければそれを持つ definition が 1 つだけのときその名前。`/` を含む展開名は先頭から辿り、`graph.instance` はその `definition` へ、`graph.loop` は `$<n>` なら `branches[n].body`、要素名ならその node を body に持つ分岐の `body` へ進みます。辿れなければ `nullopt`。
- [`graphRefusalSource()`](../../src/project/graphcomposition.cpp#L1315) は、detail の `scope`(編集の op の。`root` 以外で definition の名前なら)を先に採り、無ければ `graphNodeDefinition()` を使います。
- [`addGraphRefusalSource()`](../../src/project/graphcomposition.hpp#L67) は template で、`nlohmann::json` と editor の ordered JSON の両方に同じ規則を当てます。detail が object で code が `graph.` で始まるときだけ、まだ無い鍵だけを足します。
- [`graphRefusalInDocument()`](../../src/project/graphcomposition.cpp#L1329) は、足したものがあれば detail を足した `GraphRegistryError` を作り直します(`what()` の JSON にも入る)。足すものが無ければ元のまま返します。

呼ぶ所は 3 つの経路です。

- **編集の ticket**: [`RenderConfigEditorService::rejection()`](../../src/core/communication/renderconfigeditor.cpp#L1077) が、`graph.` の code の rejected の全部に editor の持つ root で足します([CT-2 の `withGraphRefusalDocument()`](../../src/core/communication/renderconfigeditor.cpp#L110))。その前に、変種の preflight は候補の root で、commit の失敗は ticket が運んだ root で、`edit_graph_document` は op を当てた後の文書で足します(op が足した node もそこで見つかるため。[`edit_graph_document` で足す所](../../src/core/communication/renderconfigeditor.cpp#L2975))。先に足した鍵は残るので、後の `rejection()` は上書きしません。
- **renderer が構成を読む所**: [`loadRenderGraphVariantsFromConfigData()`](../../src/core/vkcore/renderer_config.cpp#L453) が `GraphRegistryError` を受けたら足して投げ直し、`<code>: <JSON>` の文で運ばれる `graph.` の code(`view_select` と draw_loop)は、型がちょうど `std::runtime_error` のときだけ文を組み直します(派生型を失わないため。[文を組み直す所](../../src/core/vkcore/renderer_config.cpp#L625))。この関数は `reloadRenderPipelineFromDisk()` と `applyRenderPipelineAuthoringCandidate()` からも呼ばれます。
- **起動時の致命の行**: `PelicanCore::run()` の [CT-2 の `GraphRegistryError` の catch](../../src/core/userpublic/pelican_core.cpp#L180) が、[`startupGraphRefusal()`](../../src/core/userpublic/pelican_core.cpp#L62) で `ProjectBasicConfig` の root を読んで足してから `Pelican fatal error : ...` を書きます。

test は `renderconfigeditor_test` の `CT-2 …` 2 本(編集の ticket の道と、id から definition を辿る規則)です。起動時と reload の道を通す test はありません。

> 🧩 **難所 — 回の順は名前に頼れない**: 展開器は node を id の辞書順に並べるので、`<loop>/10/x` は `<loop>/9/x` より前に来ます。そこで body の外の資源に触る回は、要素の順に明示の `after` でつながれます。回ごとの `render.image` だけに触る回はつながれず、並列にできます。

> 🧩 **難所 — image node は pass を作らない(✅LP-6)**([書き手の表](../../src/project/rendergraphcomposition.cpp#L299) / [`waitOnImageWriters` の定義](../../src/project/rendergraphcomposition.cpp#L587))
>
> **何をする所か**: `render.image` node から読む pass の `after` に、その image を書く graph の pass を足します。
>
> **素朴に読むと**: 「edge の producer を `after` に足せば順は付く」と考えたくなります。ところが image node は target を宣言するだけで pass が無く、lowering は資源 node を producer にしたとき `after` を足しません。そのため LP-6 の前は、image node から読む pass は資源の名前だけを持ち、順は id の順任せでした。
>
> **骨子**:
> ```text
> image_writers[image] = graph の pass のうち、attachment / write / read_write の port、material_pass の color と depth、
>                        shadow_depth の depth、snapshot_copy の destination がその render.image を指すもの
> downstream = edge・after / before・snapshot_after・body の node → そのループ
> waitOnImageWriters(reader, endpoint, after):
>   history の読み・ループの出力は対象外(呼ぶ側と冒頭で弾く)
>   for writer in image_writers[endpoint.node]:
>     自分自身 / 別の body の書き手 / 待てない family / reaches(reader, writer) なら飛ばす
>     after += writer; downstream[writer] += reader   # 足した待ちも到達に入れる
> ```
>
> **手がかり**: 呼ぶのは fullscreen / raster / compute の `access: "read"` の port と、snapshot_copy の `source` の 2 か所です。表は `render.image` だけを載せ、`render.target_ref` と buffer は対象外です。root の graph の pass は、ループの有無によらず依存の順に並べてから宣言します([その所](../../src/project/rendergraphcomposition.cpp#L1055))。ループが無いときに id 順のままだと、`enforceCanonicalOrder` が宣言順で鎖をつなぐので、足した `after` と逆向きの辺ができて循環します。
>
> **不変条件**: 読み手の下流にいる書き手は待たない(到達を見てから足し、足した辺も到達に加える)ので、足した待ちが循環を作りません。規則の一覧は [design_graph_loops.md](../design_graph_loops.md) §5.2 です。

### 分割 — `partitionGraphDocument()`

[`partitionGraphDocument()`](../../src/project/graphpartition.hpp#L28) は文書全体を検証してから、[`GraphExecutionPartitions`](../../src/project/graphpartition.hpp#L7) の3つへ分けます。どのopがどの領域かは、opを登録したモジュールが宣言します（[`GraphExecutionDomain`](../../src/project/graphcomposition.hpp#L263)）。CPUのスカラopを1つ足しても、分割側に新しいop名を教える必要はありません。

```text
values     … CPUのスカラ評価（value.* / audio.* / render.scalar）
rendering  … 描画資源とパス（render.*）
animation  … 姿勢（pose.*）。同じフレームの values より 1 段 前 に走る
```

**`animation` が values の1段前に走るのに、animationのノードは values のスカラを読みます。** 矛盾しないのは、animation フェーズが values 分割を**自分の `EvaluationState` で自分で評価し直す**からです（[`evaluateLowered()` の呼び出し](../../src/core/ecs/predefined/animationsystem.cpp#L368)）。runtime で値を手渡しているのではなく、分割のときに「animationノードが必要とするスカラ」へ専用の出力名が付いており、animation 側はその名前で自分の評価結果から拾います。名前は [`graphAnimationScalarOutput()`](../../src/project/graphpartition.cpp#L461) が決める **`$pose/<node>/<port>`** で、両側がこの1関数を呼びます。2つの状態は同じ入力で1フレームに1回ずつ進むので、どちらも相手のクロックを進めません（[コメントの規範](../../src/core/ecs/predefined/animationsystem.cpp#L355)）。流れは values → animation の一方向で、逆はありません。同じ仕組みで instance 入力ポートの値も **`$value/<instance path>/<port>`**（[`graphInstanceInputOutput()`](../../src/project/graphpartition.cpp#L465)）として出力に現れます —— edge で繋がれたポートは自分のノードを持たないので、展開が出力として名付けて初めて「描いたフレームが何を使ったか」を観測できるためです。

### lowered IR と評価器（`ValueGraph`）

values 分割を評価器向けに落とした形が `pelican.value_graph`（§3.9 のとおり**内部IR**）で、[`ValueGraph::Graph`](../../src/core/userpublic/valuegraph.hpp#L97) がそれを読んで評価します。

- 文書は不変で、状態を持つopは外の [`EvaluationState`](../../src/core/userpublic/valuegraph.hpp#L129) を更新します。`evaluate()` はサービスへ何も publish しません。
- 状態を渡さずに状態依存opを評価しようとした場合は「引数が足りない」ではなく、**このバックエンドに唯一できないこと**として名前で断ります —— `graph.evaluation_requires_state@1` に、聞いたノードと**authoring文書の綴りの**op名を添えます（[`statelessEvaluationRejected()`](../../src/core/userpublic/valuegraph.cpp#L60)。lowered schema はモジュール名前空間を落とすので、拒否を読む側にその事情を知らせない配慮です）。
- [`publish()`](../../src/core/userpublic/valuegraph.hpp#L232) は評価に加えてサービスへの書き込みまで行い、[`PublishResult`](../../src/core/userpublic/valuegraph.hpp#L98) を返します。`instance_inputs` は各instance入力ポートが実際に何を食べたか、`node_values` は**全ノードの値**（グラフ自身のノード順、そのフレームで非活性だったノードは値なし＝0を捏造しない）です。全ノードの計算は宣言出力を射影する前に済んでいるので、これを持ち帰る費用はvector 1本ぶんです。

runtime 側の持ち方（誰がどの `EvaluationState` を使うか、snapshot をどこへ置くか）は §3.5 の「load 後: sceneが抱えるグラフ runtime」です。

## 3.16 package と render component ✅実装済み（X-1 / P-2a-3）

この2つは「プロジェクトの外へ部品を持ち出す／持ち込む」ための形式で、対になっています。

### `pelican.package` v1

package は **`package.json` を持つ1ディレクトリ**です（[`packagemanifest.hpp`](../../src/project/packagemanifest.hpp#L13)）。著者はツリーの外のどこで書いてもよく、install は `<project>/packages/<namespace>/` への**コピー**です。エンジンはプロジェクトの外の package を解決しないので、vendorされた瞬間に「どこから来たか」は効かなくなります。

manifest が持つのは名前空間と版、必要なengine / SDK、依存、提供物です（[`PackageManifest`](../../src/project/packagemanifest.hpp#L60)）。

```json
{
  "schema": "pelican.package", "version": 1,
  "namespace": "acme.glowkit", "package_version": "1.0.0",
  "requires": { "engine_min_version": "0.1.0", "sdk_abi": 2, "capabilities": [] },
  "dependencies": [],
  "provides": { "features": [], "graphs": [], "components": [], "code": false }
}
```

- `provides` のパスは package 内の相対のみで、絶対・`..`・バックスラッシュ・scheme付きは `package_path_invalid` です。壊れた封筒は `package_manifest_invalid`、名前空間がドット区切り識別子でなければ `package_namespace_invalid`（[`parsePackageManifestJson()`](../../src/project/packagemanifest.hpp#L84)）。
- [`installPackage()`](../../src/project/packagemanifest.hpp#L126) は**1バイトもコピーする前に**全検査を済ませ、コピー自体はstagingディレクトリのrenameで公開します。`project.json` の `packages` に `{namespace, package_version, sha256}` を記録します。
- `sha256` は [`packageTreeDigest()`](../../src/project/packagemanifest.hpp#L100)、つまり**インストール済みの全ファイルを相対パス順に並べたバイト列**のdigestです。`pelican_cli package list` がこれを再計算するので、記録した値は書きっぱなしではなく本番経路で検算されます。
- feature 文書は vendor されると `<namespace>.<元の名前>` という名前を持ちます（[`qualifiedPackageFeatureName()`](../../src/project/packagemanifest.hpp#L93)）。名前を比較する箇所は全部「文書自身の `name`」を読むので、名前空間はカタログではなく文書の中に居る必要があります。
- [`uninstallPackage()`](../../src/project/packagemanifest.hpp#L133) は、まだ参照が残っていれば `package_in_use` で何も消しません。誰が掴んでいるかは [`PackageError::names()`](../../src/project/packagemanifest.hpp#L37) が列挙します。
- `requires.sdk_abi` は install 時に `PELICAN_GAME_LOGIC_ABI_VERSION` と突き合わせます（[`packageSdkAbiVersion()`](../../src/project/packagemanifest.hpp#L79)）。ただし**本当のgateはこれではなく**、DLLをロードするときにloaderが解決するepoch anchorです（[第1章 §1.2](01_architecture_and_build.md)）。manifestの数値は「早く気付くための印」で、強制ではありません。
- `requires.capabilities` は project の `requires` と同じ6語の語彙で検査します（[capabilities の検査](../../src/project/packagemanifest.cpp#L635)、PG-1）。まず全要素が既知語かを見て、知らない語は `package.capability_unknown@1`（既知の一覧付き）、次にこのビルドがOFFにした語を `<語>.module_unavailable@1` で拒否します。どちらもコピーの前です。

コードを持つ package のビルド（`pelican_package_sources()` / `pelican_packages()` と転送ヘッダSDK）は[第1章 §1.2](01_architecture_and_build.md)です。

### `pelican.render_component` v1

render component は「materialize済みの feature 文書1本 + それをホストするプロジェクトに要求する契約」の組です（[`rendercomponent.hpp`](../../src/project/rendercomponent.hpp#L12)）。ディレクトリに `feature.json` と `component.json` の2枚が入り、**必ず一緒に**書かれ、一緒に読まれます。「バイト列をコピーする」だけでは操作が終わらない、というのがこの形式の要点です。

- [`buildRenderComponent()`](../../src/project/rendercomponent.hpp#L53) が2枚を作ります。可搬性の検査は文書の**全文字列値の走査**で、`project://` が1つでも残っていれば `render_component_project_reference`、共有済みのengine featureを書き出そうとすれば `render_component_engine_reference` です。shader / include / `shader_assets` / `ray_tracing` / `integrations` / `pass_overrides` を列挙する代わりに全走査するのは、参照の置き場が増えても取りこぼさないためです。
- `component.json` 側の契約は [`renderComponentRequiredTargets()`](../../src/project/rendercomponent.hpp#L46) が求める **`requires.render_targets`** です。feature のパスが読み書きするRTのうち、feature 自身が宣言していないものを並べます（`swapchain` はエンジン所有ですが、他と同じように要求として報告されます）。
- [`writeRenderComponent()`](../../src/project/rendercomponent.hpp#L59) は **ディレクトリのrename**で公開します。中断すると `<行き先>.tmp-<pid>` が残るだけで、半端に書けたcomponentは公開されません。
- [`readRenderComponent()`](../../src/project/rendercomponent.hpp#L65) は封筒（`render_component_invalid`）と `feature.json` のdigest（`render_component_digest_mismatch`）の両方を確かめます。

RPCからの使い方（`export_render_feature` / `import_render_feature`、書き先が `user://components/<name>/` に固定されること）は[第7章](07_tools_rpc_tests.md)です。

次章では、sceneの中心にあるECSの実体をメモリ配置から追います。
