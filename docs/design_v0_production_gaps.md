# v0 に足す制作の土台 —— 移行・パージの揃え方・制作の致命 5 件・BGM・基本形(v2、2026-09-24)

状態: **v2**。v1 を Claude の subagent が敵対検証(codex は利用者指示で使用不可のため代替。指摘 20 件: 致命 3 / 重い 13 / 軽い 4)し、全件を本文に反映した。
答えの対応は末尾 §12。§2 の web との順序は利用者が裁定済み(web も v0 に合わせて直す)。

## 0. 経緯と裁定

2026-09-24、利用者の問い「エンジン機能として無いことで制作が致命的にしにくいもの」への調査(実物の確認)から、次を v0 に入れると裁定した。

| 裁定 | 内容 |
|---|---|
| v0 の意味 | 形式は v0 の後も変わってよい。**v0 は「前の版の project を後の版のソフトへ移行する仕組み」がある最初の版**。[v0 凍結一覧](v0_freeze.md) の「凍結」(形を動かさない約束)はこの意図とずれるので書き直す(§1.4) |
| v0 より古い形式 | 移行の対象にしない。v0 までに出荷 project を現行形へ書き直し、旧版を読む分岐を消す(§2。web との順序は裁定待ち) |
| 足す項目 | (1) 移行 (2) パージの揃え方 (3) 制作の致命 5 件 (4) BGM とループ。加えて GUI session の依頼で (5) 基本形の同梱 |
| 作り方の条件 | 「既存の peligraph の仕様に合わせた仕組み」。専用 API を横に生やさず、登録契約・名前付きエラー・時間は port 値・CAS + RPC 先行に載せる |
| BGM | 「今のキーイングやタイミング制御と密接に関連づける」。scene を跨ぐ継続は**アプリ時刻に繋ぐ**(短い途切れは許容) |
| UI | `ctx.ui()` facade **と** `ui.*` sink(共通グラフの module)の両方 |
| 音声形式 | **WAV + Ogg Vorbis**。MP3 / FLAC は需要が出てから |

v1 に残すもの: 剛体、IBL / skybox、パーティクル、日本語フォント、加算 scene ロード、映像出力の拡張(EXR・α・motion blur・音の書き出し)、配布の一括手順、常駐 object、object の rename。

### 0.1 共通の作法(全 WP)

1. **登録で有無が決まる**: 機能は module / codec 行 / participant 行 / `requires` 語彙として登録され、無い機能は「登録されていない」ことで名前付きに落ちる。`#if` を呼び出し側に散らさない。
2. **名前付きエラー**: 新しい失敗は `<領域>.<名前>@1`。散文の例外を新設しない。既存の公開 SDK の契約(Status を返す API、凍結 snapshot)は壊さない。
3. **時間は値**: 時刻・再生位置・フェードは port を流れる値で表す。専用の時計や timeline 文書を作らない。
4. **エージェント先行**: 操作は engine の op / RPC に置き、Studio はそれを呼ぶ(D0)。
5. **証拠は本番経路**: 受け入れ条件ごとに「何を取り消したら落ちるか」を書く。fixture を変異させて代わりにしない。
6. **検査は経路で共有する**: scene load・editor の編集・実行時生成の 3 経路が同じ検査を通る(1 経路だけに置かない)。

## 1. 移行の枠(MG-1)

### 1.1 前提(2026-09-24、develop)

- 全文書が `schema` + `version` の envelope を持つ。複数版を受ける parser は「旧版を旧い意味のまま読む」分岐方式で、読み時の変換は無い。
- project.json は version 1 固定で、parse の最初に version を検査する(`src/project/projectformat.cpp:174`)。`engine_min_version` の検査は散文のエラー(:198-224)。`current_engine_version` は `"0.1.0"` の既定値のまま(`projectformat.hpp:26`)で、**形式を変えるたびに版を上げる規則は無い**。「どの版で保存したか」の記録も無い(`generator` は書くが読まれない)。
- 複数文書の原子的な書き換えは render config の transaction が公開 API(`renderconfigtransaction.hpp:51, 68`、起動時の recovery は `pelican_core.cpp:108`)。

### 1.2 決定

1. **移行の単位は文書の `(schema, version)`**。engine の版ではなく、各文書が既に持つ組で決める(手での編集や git の merge で記録がずれても二重に掛からない)。
2. **手順の登録表**: graph module と同じ manifest 形式。`DocumentMigrationStep{id, schema, from_version, to_version, description, plan(const Json&, ProjectContext) -> StepResult}`。`StepResult` は書き換え後の JSON と `manual_items[]`(自動で直せない箇所を名前付きで列挙)。同じ `(schema, from)` の重複、版の飛び(from+1 ≠ to)、循環は登録時に `project.migration_step_*@1` で拒否。**v0 時点の production の登録は 0 本**。
3. **検出は生の JSON だけで行う**: project を開くとき、project.json を含む各文書の envelope(`schema` / `version`)を parser より前に生で読み、登録手順の `from` に当たる文書が 1 つでもあれば `project.migration_required@1`(`error_fields: {documents:[{path, schema, from, to}…]}`)で止める。parser の version 検査より前に走るので、project.json 自身の形式変更も検出できる。
4. **版の記録は補助**: project.json に `saved_by_engine` を足すが、**移行の判定には使わない**(診断と表示だけ)。書くのは `project init` と移行の apply だけ(scene 保存のたびに project.json を書かない)。`engine_min_version` の検査は `project.engine_too_old@1` に名前を揃える。
5. **適用**: `plan`(dry-run、何も書かない)→ `apply`。apply は (a) 対象文書を `.pelican/migration/<timestamp>/` へ複製 (b) render config の transaction 部品を project 文書全般に一般化して一括置換 (c) `saved_by_engine` を更新。途中失敗は recovery で元に戻る。
6. **入口**: CLI `pelican_cli project migrate --project <dir> [--dry-run]`、RPC `plan_project_migration` / `apply_project_migration`(CAS は対象文書の digest の組)。Studio は開くときに `project.migration_required@1` を受けたら plan を見せて apply する(Studio 側は GUI session と調整)。
7. **回帰素材**: v0 の出荷 project を `test/fixtures/project_corpus/v0/` に固定する(JSON・shader・code のみ。gitignored な大容量資産は含めない)。
8. **台帳の規約に足す 1 行**: 「文書の `version` を上げる WP は、同じ WP で移行手順を 1 本登録し、corpus の移行 test を緑に保つ」。

### 1.3 受け入れ

1. **test 専用の手順**(`pelican.scene` の 1 key を改名する from=N → N+1 を test binary だけで登録)を使う。
   - その手順の `from` の scene を持つ project を開くと `project.migration_required@1`。対照: 検出を外す変異で parser まで進み、別の名前で落ちるか開いてしまい赤。
   - CLI の dry-run が何も書かず、apply が書き換え、退避先に元の bytes、`saved_by_engine` が更新。apply の途中の故障注入で全文書が元の bytes に戻る。対照: transaction を外す変異で部分書き換えが残り赤。
2. **corpus**: test 専用手順を登録した状態で corpus に「手順の `from` の版の文書」を 1 本含む複製を作り、migrate → 開く が通る。対照: apply の書き込みを no-op にする変異で開けず赤(手順 0 本の恒等では検出力が無いため)。
3. 手順の登録表の不正(重複・飛び・循環)がそれぞれ名前付き。
4. production binary に test 専用手順の symbol が無い(既存の dumpbin gate の型)。

### 1.4 v0 凍結一覧の書き直し

[v0_freeze.md](v0_freeze.md) の §1「凍結するもの」を「**v0 形式の一覧と移行の約束**」に改める。約束は「v0 形式の project は、後の版で `project migrate` により開ける状態にできる(自動で直せない箇所は名前付きで列挙される)」だけ。RPC 名・op 一覧・native ABI は「v0 時点の一覧(記録)」とし、不変を約束しない。§1.6「2 つの root」も §2 の結果に合わせて書き直す。MG-1 の着地時に行う。

## 2. v0 より古い形式の一掃(MG-2)

**裁定(2026-09-24、利用者)**: 「web も v0 に合わせて直す」。MG-2b は v0 で行い、web(`my_webpage`)の parser を v0 形式(scene 2 / asset_data 2 / graph root)へ合わせる作業を **WEB-1** として別 repo で起こす。順序は原則どおりエンジン先行(MG-2b の着地 → WEB-1)で、その間 web は新しい出荷 sample を開けない。

### 2.1 検証で分かった制約

- **サブセット原則**(web で開ける ⊆ pelican で開ける、形式拡張はエンジン先行): web(別 repo `my_webpage`)の parser は **scene v1 / asset_data v1 だけ**を受け、rendering は legacy root の形しか読まない(`packages/pelican-webgpu/src/pelican2/sceneFormat.ts:91-92`、`assetDataFormat.ts:30-31`、`configSchema.ts`)。pelican を v2・graph root だけにすると、web で開ける project を pelican が開けなくなる。
- 実態は v1 の見積もりより大きい: 出荷の scene は **7 本すべて v1**、asset_data は 5 本が v1、legacy root は **5 本**(animgraph / pathtrace / raytracing / sprite / vrm_xr)。`project init` の雛形も scene v1 / asset_data v1 を書く(`projecttemplate.cpp:121-124, 159-162`)。scene v1 を inline で書く test は 27 本。

### 2.2 分割

| 段 | 対象 | web への影響 |
|---|---|---|
| **MG-2a**(web に影響しない) | `ray_group_layout` 1〜2 と `ray_bindings` 1 の撤去、`status == "committed"` の読み取り互換の撤去(下の決定 3)、scene snapshot の永続化の版分岐(`basicconfig.cpp:819-826`)、layout 版による理由の分岐(`renderconfigeditor.cpp:213-216`) | 無し |
| **MG-2b**(web と順序がある) | scene v1、asset_data v1、rendering の legacy root | **有り**。裁定待ち(下) |

MG-2b の範囲: 出荷 7 project の scene を v2、asset_data v1 の 5 本を v2、legacy root の 5 本を graph root へ書き直す。`project init` の雛形を v2・graph root で直接書く。scene v1 を inline で書く test 27 本を現行形へ。その後に読み取り側の分岐を消す。

WEB-1(別 repo): web の parser が scene 2 / asset_data 2 / `pelican.graph` root の `rendering` 節を読む。未知の形は名前付きで拒否(web 側の既知の未了「名前付き拒否は未実装」もここで)。

### 2.3 決定(MG-2a と、MG-2b を行う場合の共通)

1. 各形式で旧版は `<schema>.version_unsupported@1` で拒否。
2. legacy root の撤去で消すのは**読み取り側の分岐**(`renderingSectionOf` の「graph root でなければ root 自身」、`RenderConfigDocument::graph_root_` の二態、editor の legacy 点検 `editorruntimefactory.cpp:1945`)。`wrapLegacyRenderingConfig` は読み時の包みではなく雛形と test の比較用なので、雛形を graph root で直接書く形に変えて消す。
3. **`committed` の読み取り互換**: R-1 の維持理由(古い binary が commit の途中で落ちた dir が開発者の手元に残り、以後 wedge する)は外部利用者の有無と無関係に成立する。そこで**黙った wedge を名前付きの停止に変える**: 旧形式の dir を見つけたら `render_config.legacy_transaction_dir@1`(`error_fields: {path}`)で止め、manual に手での片付け手順を書く。2 つの読み手(`renderconfigtransaction.cpp:497-502, 740-748`)を同時に消す。
4. 削った分岐ごとに「拒否の test」が 1 本あることを着地報告の表で示す(削った分岐を誰もテストしない問題への対策)。

### 2.4 受け入れ

1. 各形式で旧版が名前付きで拒否される。対照: 分岐を戻す変異で受理されて赤。
2. 旧形式の transaction dir で `render_config.legacy_transaction_dir@1`。
3. 出荷 project が現行 build で起動・capture(`project_catalog_headless_smoke`)。MG-2b を行う場合は legacy root の project が 0 本であることを catalog test が数える。

## 3. パージの揃え方(PG-1 / PG-2)

### 3.1 前提

- AG-0 / AG-1a が pose で通した型: OFF なら module を登録しない → 文書は `graph.operation_unregistered@1`、animation component を含む scene は prepare で `animation.module_unavailable@1` → rollback。codec は tool が読めるよう OFF でも残す。
- **scene の feature 検査は scene load の 1 箇所だけ**(`validateResolvedSceneFeatureBindings`、`src/core/loader/scene.cpp:826`)。editor の `add_component` と実行時生成は通らない。
- collider には OFF 時の拒否が既にあるが、名前ではなく `BuildFeatureDisabledError`(散文)を投げる(`scene.cpp:145-157`、`test/physics_disabled_probe.cpp`、`test/wp360_resolvedscene_test.cpp`)。
- `requires` の語彙は `projectrequirements.def` の animation 1 行。`.def` の行は option 1 個しか書けない(CMake の regex `([A-Z0-9_]+)`、C++ は `static_cast<bool>(option)`)。
- ずれている箇所: `pelican.audio` の無条件登録と準備時の散文例外(`graphmodule.cpp:310`、`renderingpassconfigregistration.cpp:1034`)/ facade の OFF 時の失敗が 4 形式 / provider 0 本構成で query が「当たり無し」(`physworld.cpp:564`)/ Studio の追加候補が codec 表から無条件 / participant の OFF 区分の分散 / package の `requires.capabilities` が未検査。
- animation facade の `not_found` は**公開ヘッダと凍結 snapshot v3 が明記する契約**(`gamecontext.hpp:72-76`、`test/fixtures/userpublic_frozen/v3/gamecontext.hpp:75`)。`AnimationContext` は Status を返す API で、Status の enum は ABI で凍結(`abi_v1.hpp:52-68`)。`BuildFeatureDisabledError` は既に `feature()` を持つ(`build_features.hpp:20-28`)。

### 3.2 決定(PG-1: 読み込み・編集・生成の時点での拒否)

1. **`requires` の語彙**を足す: `audio`、`physics`(= `PELICAN_WITH_PHYSICS` のみ。provider 0 本は query 時の名前付きエラーに留める。§3.3-2)、`xr`、`vat`、`seqplayer`。エラー名は `<語>.module_unavailable@1`。
2. **module の条件付き登録**: `pelican.audio` を `PELICAN_WITH_AUDIO` で囲む(pose と同形)。
3. **codec 行に必要な機能を持たせる**: `ComponentCodec::required_feature`(`requires` の語、無ければ空)。collider → physics、animation → animation。**検査は ResolvedScene の seam に置き、scene load・editor の編集(`add_component` / `spawn` / `duplicate`)・実行時生成(`ctx.spawn`)の 3 経路で共有する**。collider の既存の散文拒否はこれに置き換え、期待値を変える既存 test(`physics_disabled_probe`、`wp360_resolvedscene_test` など)を着地報告に表で列挙する。
4. **Studio の追加候補**: `available=false`、`unavailable_reason: "<語>.module_unavailable@1"`(既存の枠を使う)。`pelican_ray_instance` の「末尾を切り落とす」特例もこの `available` に寄せる。
5. **出荷 project の宣言**: sprite_demo に `physics`。vrm_xr_demo は XR 無しで起動できるかを実物で確かめ、必須なら `xr`。
6. **package**: install 時に `requires.capabilities` を同じ語彙で検査。未知語は `package.capability_unknown@1`、現 build で無効なら `<語>.module_unavailable@1`。

### 3.3 決定(PG-2: 呼んだときの失敗)

1. 例外を投げる facade(audio / physics / save など)の OFF 時は `BuildFeatureDisabledError` 1 種類に揃え、**派生型で `code() == "build_feature_disabled@1"` を足す**(公開 SDK の例外型は WP378 が ABI を pin しているので派生で足す。`feature()` と散文 message は維持)。散文の `runtime_error` を投げている箇所をこれに置き換える。
2. **animation facade は Status のまま**(`not_found` は凍結された契約)。「Status を返す API は OFF 時も Status で答える」を規則として公開ヘッダと manual に書く。区別できる Status を足すのは凍結契約の改訂として v1。
3. physics の provider 0 本構成: query は `physics.provider_unavailable@1`(例外。「当たり無し」と区別)。構成は registry の正当な対照なので消さない。
4. participant の OFF 区分の規則を文書化して揃える:「OFF でも scene に宣言が来うる機能は `stub`(宣言を拒否する役目)、来えない機能は `absent`」。audio の `uninstalled` と animation・UI の「行無し」を振り直し、`scene_transaction_contract_test` の表を更新。

### 3.4 受け入れ

1. OFF 構成ごとに、`requires` を宣言した project が読み込み時に `<語>.module_unavailable@1`。宣言しないが component を使う scene が load で同じ名前・rollback。**同じ component を editor の `add_component` で足しても同じ名前で拒否**。ON では全部通る。対照: seam の検査を外す変異で 3 経路とも赤、条件付き登録を外す変異で audio の文書が赤。
2. Studio の追加候補: OFF build で collider が `available=false` と理由付き。
3. facade: OFF build で audio / physics の各 1 本が `build_feature_disabled@1` と機能名。animation は `not_found` のまま(凍結 snapshot の test が不変)。
4. provider 0 本構成で raycast が `physics.provider_unavailable@1`。
5. package の capabilities 検査(未知語 / 無効語 / 有効語)。

## 4. 名前解決・ワールド座標・localtransform(GP-1)

### 4.1 前提

- 名前 → id の束縛は `SceneLoader::object_bindings`(`scene.hpp:60`)で非公開。同名は emplace の先勝ち(`scene.cpp:694-698`)。ワールド変換は内部の glm 型で、公開面は local だけ。
- **identity 設計**([design_object_identity.md](design_object_identity.md)):公開の入口は `ObjectRef` に限る。authority で古い参照を拒否する。解決の失敗は `stale_authority → wrong_domain → not_materialized → missing_capability` の順の名前(:228-244)。`name_ambiguous` は修正 C で削除済み。互換 adapter を「第三の境界」にしない(修正 4)。`ObjectRef` は未実装。
- localtransform は scene に parent があるときだけ、hierarchy に参加する object にだけ付く(`scene.cpp:325-341`)。`LocalTransformSystem` は local から world を毎回計算し直す(`localtransformsystem.cpp:40-43`)。editor の projection は `hierarchy ? &local : nullptr` で、hierarchy 外の object には world だけを書く(`editorprojectionadapters.cpp:329, 381`)。

### 4.2 決定

1. **`ObjectRef` を identity 設計どおり最小で実装して公開する**: `ctx.objects().resolve(name) -> Resolved<ObjectRef>`。authority は「現在の scene の run」。失敗は設計の名前(`object.stale_authority@1` / `object.not_materialized@1` など、該当するもの)で返す。同名は設計どおり先勝ちの宣言順で、曖昧エラーは作らない。`GameObjectId` への変換は `ctx.objects().materialize(ref)` を通し、authority の古い ref はここで拒否される。
2. **ワールド座標の読み取り**: `ctx.objects().worldTransform(ref)`(公開の値型)。`setLocalTransform` 直後の読みは次の transform system までずれることを公開ヘッダに書く。
3. **localtransform を全 scene object に付ける**: 付与の条件を撤去し、`hierarchy` で分岐している箇所(projection adapter、codec `componentcodec.cpp:640`、scene の組み立て)を**全部列挙して直す**。hierarchy 外の object に world を直接書く経路は、local を同時に書く形に変える。自動で付けた localtransform は scene 保存に書き出さない。
4. ID-A(identity 設計の本実装)が来たときは、この `ObjectRef` がそのまま入口として残り、中身だけ広がる。置き換えの手順を台帳の WP 節に書く。

### 4.3 受け入れ

1. 解決の成功 / 無し / 古い authority(scene を reload した後の ref)がそれぞれの名前。world の読み取り(親子 2 段、回転と scale を含む)。
2. parent を持たない scene で `setLocalTransform` が true になり、次 frame に world が動く。対照: 付与の条件を戻す変異で false が返り赤。
3. editor の gizmo で hierarchy 外の object を動かし、次 frame に戻らない。対照: projection の分岐を直さない変異で位置が戻り赤。
4. 保存した scene に自動付与の localtransform が現れない。posegraph_demo の JSON から回避の `localtransform` を消しても smoke が緑。

## 5. 実行時生成(GP-2)

### 5.1 前提

- 設計の約束: prefab の U4 `ctx.spawn`(単一 object、1 spawn = 1 entity、replay trace 同一: [design_prefabs.md](design_prefabs.md):148)。behavior の callback 中の生成は次の boundary へ遅らせる([design_object_behaviors.md](design_object_behaviors.md):135-146)。**behaviors 設計自体が「3 つ組から engine-owned の monotonic な `attachment_seq` を確定」と書いている**(:141-146)。runtime で注入した object は `TransientObjectRef`(identity :214-228。authority は run id を含む)。
- 実装: `createObject` / `createSpriteObject` は transform(と sprite)だけ。collider の実行時登録は scene load と editor 経路のみ。前例は `loadTransientGltf`(`runtime_only` の physics binding)。`reserveRuntimeAttachmentIdentity` は引数を無視する単調カウンタ(`behaviorarena.cpp:1057-1071`)。**runtime には editor journal の commit_seq が無い**。

### 5.2 決定

1. **`ctx.spawn(prefab, SpawnParams{transform, parameters, name?}) -> TransientObjectRef`**: prefab の展開は load 時と同じ ResolvedScene seam を通す(§3.2-3 の feature 検査もここで効く)。得た component 束を `SceneObjectProjectionAdapter` と participant(renderer・physics・behavior・animation)で投影。collider は `runtime_only` の binding、behavior は attachment arena。文書に書き戻さず、scene 保存に含めない。
2. **時点**: 常に次の boundary で実体化(callback の中か外かで挙動を変えない)。boundary 前の ref の `materialize` は `object.not_materialized@1`。
3. **runtime 側の attachment 識別子の組を定義する**: `(boundary 番号, その boundary での spawn の index, attachment の index)` から `attachment_seq` を確定する(behaviors 設計の「3 つ組から確定」の runtime 版。editor 側は既存の commit_seq の組のまま)。
4. **破棄**: `ctx.destroy(ref)` も boundary で行い、participant が逆順に退役(collider の binding、behavior の onDestroy)。
5. **play 中の一時物の寿命**: Studio の preview(play)の中で spawn した object は preview の終了で消える(preview の外の状態を汚さない)。preview の外(通常の player 実行)では scene の寿命に従う。
6. **範囲**: 単一 object の prefab のみ(多 object は prefab §7 の別設計で v1)。`PhysWorld::bindCollider` の即時登録は spawn から使わない。

### 5.3 受け入れ

1. collider と behavior を持つ prefab を spawn → 次 frame に raycast が当たり、behavior の onInit が 1 回。destroy → onDestroy、当たらなくなる。
2. callback 中の spawn と外の spawn で結果の順序が同じ。
3. **同じ入力の preview を 2 回回して、attachment 識別子の id 部分(authority の run id を除く)が一致**。対照: 単調カウンタに戻す変異で 2 回目がずれて赤(カウンタが preview を跨いで引き継がれるため)。
4. spawn した object が scene 保存に含まれない。preview の終了で消える。
5. OFF 構成で collider 付き prefab の spawn が `physics.module_unavailable@1`(§3 の seam の共有)。

## 6. ゲームコードからの UI 操作(GP-3)

### 6.1 前提

- `UiModule` は `ui_config_json` の 1 文書だけを持ち、描画は `DocumentNode.text` を読む(`src/core/ui/module.cpp:117, 136`)。公開の書き換え API は無い。gauge は描画も未実装。
- UI node の `id` は**兄弟の間でしか一意でない**(一意なのは path: `document.cpp:233-235`)。UI 設計の同一性は `(document_key, stable_id)`。
- **UI には build OFF 構成が無い**。有無は render config が UI feature を参照するかで決まる(`src/core/appflow/loop.cpp:166`)。
- UI 設計 §5 の Controller / widget 登録 / `UiDocumentHandle`(消えた handle は no-op + status)は U3 の予定で未実装。

### 6.2 決定

1. **node の指定は `(document_key, path)`**。v0 は文書が 1 つなので `document_key` は既定値を持つ。
2. **`ctx.ui()` facade**: `setText(path, text)`、`setVisible(path, bool)`、`setValue(path, float)`。書き込みは boundary で適用。未知の path は `ui.node_unknown@1`、型に合わない操作は `ui.property_mismatch@1`、UI feature が参照されていない project では `ui.feature_unreferenced@1`。
3. **`pelican.ui` graph module**(sink 2 本): `ui.value`(入力 `value`、parameter `node: path`)と `ui.visible`。**単一 writer の単位は `(文書, path, 性質)`**(UI node は owner に属さないので、owner 単位ではなく書き先の単位で数える)。同じ単位を 2 つの sink が書く文書は render config の準備時に `ui.sink_conflict@1`。facade がその単位に書くと facade が `ui.sink_conflict@1` を投げる(sink が勝つ。graph が値の正本)。UI feature が参照されていない project で `ui.*` を使う文書は準備時に `ui.feature_unreferenced@1`。
4. **gauge の描画**を実装する。
5. U3 の Controller・widget 登録・`UiDocumentHandle`・scene ごとの UI 文書は v1。今回の facade と sink は U3 の handle の外側の面として残る(U3 が来たら path の解決を handle 経由に替える)。

### 6.3 受け入れ

1. facade で label の文字、表示、gauge の値を変え、headless capture の画素が変わる。
2. `value.curve` → `ui.value` で gauge が 2 時刻で違う画素になる。対照: sink の適用を no-op にする変異で画素が変わらず赤。
3. 二重 writer(sink 同士)が準備時に、facade と sink の衝突が facade で `ui.sink_conflict@1`。UI feature の無い project で `ui.feature_unreferenced@1`。
4. 兄弟でない同じ `id` を持つ 2 node を path で書き分けられる。

## 7. マウスの位置と画面 → ray(GP-4)

### 7.1 前提

- action map の axis2 は **[-1,1] に clamp され、複数 binding は加算される**(`src/core/os/actionmap.cpp:492-501`)。mouse の binding は axis1 のみ(:451-467)。px の絶対座標は axis として表せない。
- `InputSnapshot.mouse_x/y` は GLFW の窓座標。input sequence の header は fps だけで content scale を持たない(`inputsequence.cpp:183-186`)。UI 設計は再現単位に ViewportTransform を含める(`design_ui_2d_foundation.md:142`)。RPC `inject_input` の mouseMove(`rpcserver.cpp:839-840`)の単位は未定義。ImGui は窓座標前提(`imguisystem.cpp:113`)。
- 画面 → ray の変換は src に無い。camera 行列は内部 API のみ。`PhysicsContext::raycastClosest(phys::Ray)` は公開。

### 7.2 決定

1. **pointer は action ではなく入力の状態として公開する**: `ctx.input().pointer() -> PointerState{position_px, inside}`。値域の制限も合成もしない。`InputSnapshot` は窓座標のまま残し、**換算は公開の段(pointer と UI router)でだけ**行う(ImGui は窓座標のまま)。UI router の DPI 1.0 固定は同じ換算に揃える。
2. **決定性**: input sequence の記録に content scale と framebuffer の大きさを足す(header の版を上げる。MG-1 の規約により移行手順を 1 本登録)。再生は記録された換算を使い、再生する機械の scale に依らない。
3. `inject_input` の mouseMove は**窓座標**と manual に明記する(既存の意味を確定するだけ)。
4. **`ctx.render().screenRay(position_px) -> Resolved<phys::Ray>`**: 現在の主 camera の view / projection の逆。単一 view でないとき(XR、複数 view)は `render.screen_ray_unavailable@1`。
5. カーソルのロック / 非表示: `ctx.input().setCursorMode(normal|hidden|locked)`。埋め込み viewport(Studio)ではロックを無視し、`get_status` に実効値を出す。

### 7.3 受け入れ

1. 記録した sequence を別の content scale の環境で再生して pointer の px が記録どおり。対照: 記録の scale を使わない変異で倍になり赤。
2. `screenRay(画面中央)` → `raycastClosest` が正面の collider に当たり、端では当たらない。**camera は原点から外し、回転させる**(view の逆を外す変異が検出されるように)。対照: view / projection の逆をそれぞれ外す変異で赤。
3. XR 構成で `render.screen_ray_unavailable@1`。

## 8. Studio の object 操作(OL-1、engine 側)

**Outliner の GUI は GUI session が F 系列の後に持つ(2026-09-24 合意)。ここは engine 側だけ。**

### 8.1 前提

- `edit` の op は 12 本(`editorjournal.cpp:1603-1624`)で、spawn / destroy / reparent(`preserve: local|world`)がある。**複製も rename も無い**(identity 設計は rename を後回しにしている)。
- graph.instance の owner は authoring_object_id で結ばれ、この id は宣言順で採番される(永続化は任意: `sceneformat.cpp:40-56`)。**複製の挿入や reparent の後に保存して開き直すと、owner が別の object へ黙って移りうる**(`scenegraphruntime.cpp:35-41`、posegraph_demo README :79-87)。
- Outliner の key は `(scene_id, declaration_index)`。identity の修正 E(wire 上の authority)は未実装。instance_id を journal に記録する規約がある(台帳 :14705-14708)。

### 8.2 決定

1. **構造を変える op(spawn / destroy / reparent / duplicate)は、影響する object の永続 authoring id を文書に書き出す**(任意から、構造編集の後は必須へ)。これで宣言順の採番に依存しなくなり、owner の黙った付け替えが起きない。
2. **`duplicate` op**(journal の 13 本目): 対象 object と子孫を複製し、新しい authoring id を採番して書き出す。名前は `<元>_copy<N>` で衝突回避。prefab instance は instance ごと複製し、新しい `instance_id` を journal に記録(既存の規約どおり)。graph.instance の owner が複製元を指すものは複製しない(owner は複製元のまま)。undo は 1 手。
3. `scene_tree` と `edit` の応答に authoring id を載せ、Outliner の key を authoring id に変えられるようにする(修正 E の authority は ID-A の範囲で、ここでは入れない)。
4. **rename は範囲外**(identity 設計の後回しに従う)。GUI session に伝える。

### 8.3 受け入れ

1. duplicate → undo → redo の往復、prefab instance の複製、名前の衝突回避。
2. **graph.instance の owner を持つ object の前に object を複製・挿入 → 保存 → 開き直す → owner が同じ object を指す**。対照: authoring id の書き出しを外す変異で owner がずれて赤。
3. `scene_tree` の応答に authoring id。

## 8a. 基本形の同梱(GP-5)

GUI session の第二段階(forward / deferred の material ループを Studio で組む)からの依頼(2026-09-24)。blank project にメッシュが無く、Studio から形を置く手段が無い。

### 8a.1 決定

1. `engine://primitives/{cube,plane,sphere}` を、asset_data に登録しなくても model の参照として解決する。形は**コードで生成**(binary を同梱しない)。生成は決定的で、法線・UV・tangent を持つ。
2. 既定の material(灰色の OpenPBR base)。差し替えは既存の material binding の経路。
3. 生成の op は新設しない。既存の `edit` の `spawn`(transform + model component)で置く。Studio の「Add object → Cube」は GUI session が OL-1 の GUI と一緒に持つ。
4. model component の名前と参照の書式は、実装で codec(今は `simplemodelview.model`)を確かめて確定し、GUI session へ送る。
5. web は `engine://primitives/*` を知らない。サブセット原則により、**web が開けない参照を pelican が持つのは許される**(逆は不可)。web 側の対応は web の WP。

### 8a.2 受け入れ

1. blank project に RPC の spawn で cube を置き、headless capture で形が写る。保存 → 開き直しで残る。
2. asset_data に何も無い project で参照が解決する。未知の primitive 名は `model.primitive_unknown@1`。
3. 生成した頂点列の digest が実行ごとに一致する。

## 9. BGM とループ(AU-1 / AU-2)

### 9.1 前提

- 時間系 op(`src/core/userpublic/valuegraph.cpp`、`animation/curve.cpp`):
  - `event.playback` は payload 無しの stream で命令は `start / pause / resume / reset`。
  - `value.playback`: delay → `sample_interval` の量子化 → speed → **wrap(`once / repeat / ping_pong`、文字列 parameter)** → `source_begin..end`。音源の進む速さは `(end-begin)/duration*|speed|`。数値の 6 項目は literal か edge(lowering で区別できる: `valuegraph.cpp:252-257`)。
  - graph 入力の時計は `time` / `delta_time` / `scene_time`。
- `audio.playback`(`{asset}`、入力 `events` 必須・`time` → `voice`): **time はイベントの到着判定だけ**。start は常に `playSound` で新しい voice(`scenegraphruntime.cpp:153-172`)。lowering は未知の parameter を拒否(`keys(parameters, {"asset"})`)。
- **scene の commit は `graph_runtime.clear()` だけで voice を止めない**(`scene.cpp:722-725`。stopAll の類は src に無い)。前の scene の voice は管理外のまま鳴り続ける = **既存の不具合**。scene 内の owner の交代・消失では stop。
- イベントの時刻は scene 基準(`now - scene_start_time`)。
- backend: voice は常に Se bus、WAV を全体 decode、start ごとに再 decode、looping / seek / fade 未使用。**headless と rpc は null backend**(cursor が無く「時計を捏造しない」方針: `audio.cpp:269-310, 450-453`)。Studio の player は rpc なので **Studio では音が鳴らない**。config 登録時の検査用 decode は全体 decode(`renderingpassconfigregistration.cpp:1028-1036`)。
- [design_audio.md](design_audio.md) の A2 は `playMusic(path, loop)` と Ogg ストリーミングを予定していた(未実装)。本節がそれを置き換える。
- miniaudio 0.11.25。取得済みソースに `extras/stb_vorbis.c` が同梱。

### 9.2 決定(AU-1)

1. **scene の commit で graph の voice を全部止める**(既存の不具合の修正。以下の前提)。

2. **一体版と分解版**(原則 3: 同じ設定を表す最適化した一体版と、編集できる分解版):
   - **一体版**: `audio.playback` に `value.playback` と同じ時間写像の parameter を持たせる: `wrap`、`source_begin`、`source_end`、`speed`、`delay`(literal または edge)。`wrap: repeat` かつ速さの式が一定のとき、backend は **sample 精度のネイティブループ**(data source の loop point)で鳴らす。
   - **分解版**: `audio.playback` の任意入力 `position`(音源内の秒)に、任意の graph(`value.playback` を含む)を繋ぐ。voice は `position` に frame 粒度で追従する。
   - **意味の契約**: 「一体版 `audio.playback{wrap, source_*, speed, delay}` と、分解版 `value.playback{同じ値} → audio.playback.position` は、各時刻で同じ音源位置を指す」。違いはループ点の途切れ(一体版は無し、分解版は最大 1 frame)だけで、manual に書く。同じ文書に両方は書けない(`position` 接続と写像 parameter の同時指定は `audio.playback_mapping_conflict@1`)。
3. **position 接続時(分解版)の状態機械**:

   | 状態 | 入り | 振る舞い |
   |---|---|---|
   | 待機 | 初期 / reset | 鳴らない |
   | 再生 | start イベント、または `events` 未接続なら owner の最初の frame | 毎 frame、voice の cursor と `position` の差を**音源の長さを法として**比べ、許容(既定 1 frame + 10 ms)を超えたら seek。`position` が進まない frame は voice を止める |
   | 一時停止 | pause イベント | `position` が進んでも鳴らさない(**イベントが優先**)。resume で再生へ |
   - position 接続時は `events` を任意にする(未接続なら「owner がある間は鳴る」)。
4. **一体版の開始・停止**は今日どおりイベント(`events` 必須)。
5. **バス**: parameter `bus: "se" | "bgm"`(既定 `se`)。`parameter_changes` に rebuild を宣言。`GameContext` の `playSound` にも同じ指定(既定 Se)。
6. **フェード**: `value.curve`(キーで音量の包絡)→ `audio.volume`。backend は frame ごとの目標値へ 1 frame かけて線形に寄せる。専用の fade API は作らない。
7. **decode の 1 回化**: asset 単位で decode 済みの buffer を共有(WAV・短い Ogg)。config 登録時の検査は、streaming 対象(§9.3)では header だけ読む。
8. **scene を跨ぐ継続**: BGM の owner を**各 scene に同じ名前で置き**、時計に `time`(アプリ時刻)を繋ぐ。scene の commit で前の voice は止まり(決定 1)、新しい scene の owner の最初の frame に同じ位置から鳴り直す(短い途切れを許容。常駐 owner の無途切れは v1)。
   - **イベントの時刻の基準**: イベントは scene 基準の時刻で積まれる。BGM の時計を `time` に繋ぐときは `events` を使わず(position 接続・`events` 未接続)、開始は owner の存在で表す。`time` と scene 基準のイベントを同じ node で混ぜる文書は `audio.clock_mismatch@1`(lowering で、`events` の供給元の時計と `position` の時計の入力名が違えば拒否)。
9. **Studio で音を出す**: 窓を持つ rpc の player(Studio の埋め込み)は実 device を使う。headless は §9.4 の device 無し engine。null backend は device を開けないときの退避として残す。

### 9.3 決定(AU-2: Ogg とストリーミング)

1. 対応形式は **WAV + Ogg Vorbis**。拡張子と先頭 bytes の両方で判定し、それ以外は `audio.format_unsupported@1`。[資産形式の方針](design_asset_format_policy.md) の「WAV のみ」を改める。
2. Ogg は `stb_vorbis.c` を miniaudio と同じ translation unit で有効化(`PELICAN_WITH_AUDIO` の中だけ)。
3. 一定長(既定 10 秒)を超える音源は voice ごとの streaming decoder(`ma_decoder`)で読む。一体版のネイティブループと分解版の seek は streaming でも同じ意味で動く。
4. test 用に**自作の短い Ogg**(数 KB、正弦波を 1 回だけ encode)を `test/fixtures/audio/` に置き、出所(生成手順・ツールの版・ライセンス = project 自身)を記録する(`example_assets.md` の禁止は example の大容量資産の規則。test fixture には既存の binary がある)。

### 9.4 受け入れ(音の検査は device 無しの miniaudio engine で行う)

**測り方**: miniaudio の device 無し engine(`noDevice`、`ma_engine_read_pcm_frames` を test が回す)で、実際に mix された PCM と voice の cursor を決定的に読む。null backend の捏造した cursor は使わない。

1. 分解版: `set_time` で時刻を飛ばすと voice の cursor が追従する。時計の pause で voice が止まる。pause イベントが position より優先。対照: 追従の seek を外す変異で cursor がずれたまま赤。
2. 一体版と分解版の等価: 同じ値の `audio.playback{wrap: repeat…}` と `value.playback → position` で、ループ点を跨ぐ区間の cursor 列が許容内で一致。一体版はループ点の PCM が連続(seek 0 回)。対照: ネイティブループを外す変異で一体版の seek が 1 回以上になり赤。
3. scene を切り替えると前の scene の voice が止まる。対照: 決定 1 を外す変異で前の voice が鳴り続け赤。`time` に繋いだ BGM が次の scene で同じ位置(許容内)から鳴る。
4. `bus: bgm` の voice が Bgm bus の音量に従い、Se の音量変更に従わない。
5. `value.curve → audio.volume` で、frame 間の gain の段差が線形補間される(mix された PCM の包絡で確認)。
6. 同じ asset の start を 2 回して decode が 1 回。
7. Ogg の再生・seek・streaming(閾値を test 用に下げる)。未対応形式が `audio.format_unsupported@1`。`audio.playback_mapping_conflict@1`、`audio.clock_mismatch@1`。

### 9.5 やらないこと(v1)

3D 定位、音の書き出し(O-1 の音声出力)、ducking / sidechain、常駐 owner による無途切れの scene 跨ぎ、MP3 / FLAC、`event.playback` の語彙拡張。

## 10. 順序と依存

| WP | 内容 | 前提 | 規模 |
|---|---|---|---|
| GP-5 | 基本形の同梱 | — | 小〜中 |
| MG-2a | 旧形式の一掃(web に影響しないもの) | — | 小〜中 |
| MG-2b | scene v1 / asset_data v1 / legacy root | MG-2a | 大(出荷 7 project、雛形、test 27 本) |
| WEB-1 | web の parser を v0 形式へ(別 repo) | MG-2b | 中 |
| PG-1 | 読み込み・編集・生成の時点の拒否 | — | 中 |
| PG-2 | 呼んだときの失敗 | PG-1 | 小 |
| MG-1 | 移行の枠 | MG-2a(MG-2b を v0 でやるならその後) | 中 |
| GP-1 | ObjectRef・world・localtransform | — | 中 |
| GP-4 | pointer・screenRay・カーソル | MG-1(input sequence の版上げで移行手順を 1 本) | 小〜中 |
| GP-3 | `ctx.ui()`・`ui.*` sink・gauge | — | 中 |
| AU-1 | voice の停止修正・一体版 / 分解版・bus・フェード・decode 1 回・Studio で音 | PG-1 | 大 |
| AU-2 | Ogg・streaming | AU-1 | 中 |
| GP-2 | `ctx.spawn` | GP-1、PG-1 | 大 |
| OL-1 | 構造編集での authoring id 書き出し・duplicate op | — | 中 |

着地の順(案): **GP-5(GUI session の第二段階を塞いでいる)→ MG-2a → PG-1 → GP-1 → (PG-2 / MG-1 / GP-3 を並行)→ GP-4 → OL-1 → AU-1 → AU-2 → GP-2**。GP-2 は識別子の変更を含むので最後に単独で着地する。MG-2b は MG-2a の直後(MG-1 の corpus を一版にしてから固定するため MG-1 より前)、WEB-1 はその後。

実装者: codex が使えない間は Claude(Opus)の subagent。1 worktree 1 agent、着地は直列(GUI session の着地状況を見てから)。仕様の敵対レビューも Claude の subagent で代替し、報告にそう書く。

## 11. 用語

- **一体版 / 分解版**: pelican_vision §1.3 原則 3。同じ設定を表す、最適化した 1 つの実装と、ノードに分解して編集できる実装。
- **boundary**: behavior の副作用・生成・破棄を適用する frame の区切り(behaviors 設計)。

## 12. v1 検証の指摘と答え(2026-09-24、Claude subagent による代替レビュー)

| # | 重さ | 指摘 | 答え |
|---|---|---|---|
| 1 | 致命 | MG-2 がサブセット原則を破る(web は v1・legacy root のみ)。規模も過小 | §2 を MG-2a / 2b に分割。2b は利用者の裁定待ち。規模を数え直した |
| 2 | 致命 | `mouse/position` を axis2 にすると clamp と加算で px が壊れる。record の決定性も不成立 | §7.2: action ではなく pointer の状態。sequence に scale を記録 |
| 3 | 致命 | scene 跨ぎで前の voice が止まらず BGM が二重。イベントと position の優先が未定 | §9.2-1 で既存の不具合を直す。状態機械を表で決めた。時計の基準の混在は拒否 |
| 4 | 重い | ネイティブループの判定条件不足、剰余の比較、原則 3 の誤用 | §9.2-2: 一体版を明示の parameter にし、分解版との意味の契約を持たせた。比較は剰余 |
| 5 | 重い | null backend では測れない。Studio で音が鳴らない。design_audio.md を無視 | §9.4 を device 無し engine に。§9.2-9 で Studio は実 device。A2 の置き換えを明記 |
| 6 | 重い | `.def` で「provider 1 つ以上」を表せない。PG-2 と矛盾 | `physics` = WITH_PHYSICS のみ。provider 0 本は query 時 |
| 7 | 重い | prepare の検査は scene load だけ。collider の既存拒否が未記載 | 検査を ResolvedScene seam に置き 3 経路で共有。既存 test の期待値変更を表にする |
| 8 | 重い | animation の `not_found` を例外にすると凍結 SDK を破る | Status のまま。規則として文書化 |
| 9 | 重い | `wrapLegacyRenderingConfig` は読み時の包みではない | 消す対象を読み取り側の分岐に直した |
| 10 | 重い | R-1 の維持理由の読み違い | 名前付きの停止 + 手での片付け手順に変えた |
| 11 | 重い | 移行を engine の版で決めるのは脆い。project.json の version 検査が先に走る | 文書の `(schema, version)` で決める。検出は生の JSON。`saved_by_engine` は記録だけ |
| 12 | 重い | resolver が identity 設計と食い違う | `ObjectRef` を設計どおり最小で実装。エラー名を設計に揃えた |
| 13 | 重い | localtransform を全付与すると world 直書きが毎 frame 戻される | `hierarchy` 分岐を全部列挙して直す。gizmo の受け入れを足した |
| 14 | 重い | runtime に commit_seq が無い。対照が検出しない | runtime の組を定義。対照は preview 2 回 |
| 15 | 重い | UI の node 指定・OFF 構成・writer の単位 | `(document_key, path)`、`ui.feature_unreferenced@1`、単位は `(文書, path, 性質)`。facade との衝突の扱いを決めた |
| 16 | 重い | rename が無い。複製・挿入で graph の owner が黙って移る | 構造編集で authoring id を書き出す。rename は範囲外。受け入れに開き直しを足した |
| 17 | 軽い | `inject_input` の単位、ImGui、camera が原点の test | §7.2-1, 3、§7.3-2 |
| 18 | 軽い | lowering の parameter 許可、`parameter_changes`、streaming の検査 decode、処理順の記述 | §9.1、§9.2-5, 7 |
| 19 | 軽い | 一掃の表の漏れ | §2.2 の MG-2a に足した |
| 20 | 軽い | MG-1 の corpus 対照が fixture 変異で恒等、GP-3 の対照が登録の有無だけ | §1.3-2、§6.3-2 を production の変異に直した |
