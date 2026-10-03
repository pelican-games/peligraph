# アセットホットリロード(v2.3)

対象読者: エンジン担当・開発体験を気にする人。
ステータス: **v2.3 — 条件付き受理 + coordinated render pipeline 接続済み
(2026-07-29)**。v1 は敵対レビュー
`docs/design_reviews/2026-07-12_anim_v2_hotreload_review_codex.md`(以下
「レビュー」)§6-9 で **Reject** — Watcher の overflow/復帰プロトコル欠落、
WP82 hash「流用」の不成立、WP62 EntityId の取り違え、GPU resource
container に replace/rollback 面がない、の 4 点が主因。v2 = レビュー §9 の
再受理条件 5 点(Win32 state machine / 種別ごとの identity・transaction /
dependency transaction group / platform fixture / HR0-HR2 再分割)を反映した
全面改稿。v2 は再レビュー
`docs/design_reviews/2026-07-12_hr_v2_2d_v1_review_codex.md`(以下
「再レビュー」)で条件付き受理 — **HR0/HR1 着手は HR-C1〜C4 を各 WP の
受入条件に添付することが条件**。v2.1 = その 4 条件の正本反映(§2-1a・
§3-1a・§2-4・§7 の gate 追記)。v2.2 は WP196 で rendering config /
render feature / pipeline preset の transaction と submission-fence lifetime を
同じ runtime publication 経路へ接続した。v2.3はWP222で、同じwatcher batchの
render graph、surface shader、graphics pipeline、material metadata/valueを
選択的なcompanion requestとして同じpublicationへ接続した。
実装状況: **HR2-S は WP108(2026-07-15)で完了**。以下の規範どおり、
shader の独自 mtime poll は撤去され、FileWatcher/ContentDigest の単一路、
source/include/.surface reverse dependency、全 variant/pipeline/material layout の
group-wide transaction が実装された。**FW-1(2026-10-02、`92681afd`)** で走査は
size / mtime の比較になり、hash は変わったファイルだけになった(§1・§2-1・§2-1a・
§2-3)。**HW-1(2026-10-02、`c947cef4`)** で、監視する素材とフォルダを project.json の
`hot_reload` で宣言できるようになった(§0 の 3・§2-3・§3-2a)。**PJ-1(2026-10-03、
`89643cd7`)** で、`hot_reload` と `asset_stores` を動いている engine に RPC
`edit_project_settings` で書かせ、その場で watcher を作り直せるようになった(§2-3。
HW-2 の engine 側)。**HW-2s(2026-10-03、`78737fdf`)** で、Studio の Assets の
ドックの行ごとの「Reload when saved outside」がこの RPC で `hot_reload` を書き換える
ようになった(manual 第13章 §13.1 の「Studio の Assets のドック」)。監視が開いて
いるとき、Studio は `reload_shaders` の `committed: false` を watcher の読み直しとして
扱う(§2-3 の gate が開いていれば watcher が先に compile しうるため)。
前提: 確定規約 2 件(2026-07-08 ユーザー決定 — ①リプレイ/strict/rpc 駆動中は
ホットリロード無効 ②エディタの自己書き込みは無視)、2026-10-02 の利用者決定
(素材やフォルダごとに hot reload の有効化を設定できる形にする。既定 = 何も宣言して
いない project を Studio で開いたときは監視なし。これにより ① の「rpc 駆動中」は
「rpc 駆動中で、`hot_reload` を宣言していないとき」に狭まった)、「ファイルが唯一の
真実」(devstudio D3 の実行基盤を兼ねる)、WP82(シェーダキャッシュ)、
`design_project_vcs.md`(mounted store)。

## 0. 原則(維持)

1. **置換は開発の日常** — ファイルを保存したら数百 ms 後に絵が変わるのが
   目標の体験。警告で止めない
2. **失敗で壊さない**: どの種別も「candidate 構築に失敗したら旧リソース
   継続 + 名前入り WARN + status」。壊れた .surface を保存しても落ちない
3. **決定性の聖域を侵さない**: リプレイ / strict / rpc 駆動中は監視を
   無効化。復帰は §2 の reconcile プロトコル(単純な「一括再スキャン」
   ではない — arm/scan race があるため)。
   (2026-10-02 利用者決定、HW-1)rpc 駆動中でも、project.json が
   `hot_reload`(project root からの論理 path の配列)を宣言していれば、
   列挙したものだけを監視する。空の配列は監視しない。宣言しなければ従来
   どおり(単体の player は全体を監視、rpc 駆動中は監視しない)。リプレイ /
   strict と headless(`shader_hot_reload = false`)は宣言があっても無効。
   宣言は作者が「ここは外のツールで書き換える」と明示した場所だけに監視を
   開くもので、宣言しない project の rpc 駆動の決定性は変わらない
4. **適用はフレーム境界**: mid-frame 差し替えなし。検知はいつでも、適用は
   次フレーム先頭で一括
5. **(v2 追加)差し替えは transaction**: 「新を構築 → 検証 → 差し替え」を
   種別ごとの逐次処理でなく、CPU candidate → GPU staged commit →
   atomic swap の二段 + 依存グループ単位で行う(§4)

## 1. FileWatcher — Win32 state machine(レビュー HR-B1)

`FileWatcher` モジュール(新設)。一次 = Win32 `ReadDirectoryChangesW`
(overlapped)、fallback = mtime ポーリング(2s 既定。store ごとに自動
判定 — watch 開始失敗・network path で降格)。監視対象 = project root
配下 + mounted store の実パス。

- project root に重なる store(mount が空)の最上位の `.pelican/`(shader
  cache・`assets-hash-cache.json`・transaction・移行・log。engine が常に
  書く)は**走査も通知も対象外**(FW-1)。reload participant が claim する
  source はそこに無い
- `hot_reload` の宣言があれば(HW-1、§2-3)、宣言に届かない mount の store は
  走査も native watch もせず、走査は宣言に届かないフォルダに入らず、宣言の
  外のファイルは inventory に入れない(hash も queue もしない)
- 1 回の通知の record が全部 `.pelican` の下か宣言の外を名指すなら、その
  通知では走査しない

### 1-1. handle と I/O の規範

- directory handle は `FILE_LIST_DIRECTORY` +
  `FILE_SHARE_READ|WRITE|DELETE` +
  `FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED` で開く
- 通知 buffer と `OVERLAPPED` は request completion まで生存を保証し、
  DWORD alignment を守る。buffer size は **local/network 共通で 64 KiB
  以下を既定**(network directory は 64 KiB 超で
  `ERROR_INVALID_PARAMETER`)
- 最初の read で kernel buffer size が handle lifetime 中固定される —
  「後から拡張」は設計に含めない
- completion を処理したら**即 re-arm**。hash/デバウンスは watcher thread
  外(worker)へ渡し、重い処理中に read 未発行の窓を作らない

### 1-2. overflow は rescan で回復する(拡張ではなく)

- overflow = 「成功 + 0 bytes」または `ERROR_NOTIFY_ENUM_DIR`。このとき
  個別 event は**全破棄**されているため、**必ず該当 subtree の inventory
  rescan**(全走査 = 各ファイルの size / mtime を前回の inventory と比較し、
  hash は size か mtime が違うファイルにだけ。FW-1)へ落とす。Microsoft
  仕様もこの場合の enumeration を要求する
- rescan 中に来た event は drain して該当 path を再 hash。rescan 中に
  再 overflow したらもう一周

### 1-3. rename・停止の規範

- rename は OLD/NEW が**別 record**で、completion 境界を跨ぎ得る —
  pairing を前提にせず、old/new path の両方を dirty とし content
  inventory で確定する(atomic-save の temp→rename もこれで拾う)
- 停止手順(順序固定): `CancelIoEx`(完了を待たない)→ completion
  回収(`ERROR_OPERATION_ABORTED` 含む)→ buffer/OVERLAPPED 解放 →
  handle close → thread join。**「thread を止める」だけでは不成立** —
  kernel は handle が開いている限り buffer へ変更を蓄積するため、
  disable 時は directory handle を閉じる

### 1-4. 重複と escape

- project root と mounted store が重なる場合は canonical store/file
  identity で dedupe(同一実ファイルの二重通知を 1 本化)
- reparse point / symlink が store 外へ出る path は既存 PathResolver と
  同じ escape policy で拒否

## 2. ContentDigest・自己書き込み・gate/reconcile

### 2-1. ContentDigest(レビュー HR-B2 — WP82「流用」の訂正)

WP82 から流用できるのは SHA-256 ライブラリ(picosha2)だけ。
`sourceGraphSha256` / `shaderCacheKey` は shadercompiler.cpp の内部関数、
assets manifest の `fileSha256` も private であり、file content digest
API は存在しない。よって:

- **`ContentDigest` 共通 utility を新設**: streaming SHA-256、canonical
  `AssetKey`(§3-1)、read/share violation・cancel の扱い、byte count を
  API 化。WP82/assetsmanifest の既存 private 実装は後続でこれへ寄せる
  (挙動不変の移行)。→ FW-1 で streaming SHA-256 は `Sha256Stream`
  (`src/project/sha256stream.{hpp,cpp}`。Windows は BCrypt、それ以外は
  picosha2。どちらも小文字 16 進で同じ digest)の 1 つになり、assets
  manifest の `assetFileSha256()` と watcher の `readStableContentDigest()`
  が共有する。読む単位はどちらも 1 MiB(`Sha256Stream::read_chunk_bytes`)。
  shader cache の鍵(`shadercompiler.cpp`)は今も picosha2 を直接使う
- baseline は「最後に**適用成功した** hash」。検知しただけ・失敗した
  candidate の hash を baseline に進めない(失敗後の再保存が無視される
  事故を防ぐ)
- **走査は size / mtime の比較、hash は変わったファイルだけ(FW-1)**:
  inventory の entry は directory の一覧から取った size / mtime を持ち、
  次の走査で両方が同じで、その値が信用できる(下の settle 窓を過ぎていて、
  hash がその版のファイルを読んだ)entry は読まずに digest を持ち越す。
  hash するのは size か mtime が違うファイル、新しいファイル、信用できない
  entry だけ
- **起動時は hash しない(FW-1)**: 最初の走査はファイルを読まずに inventory
  を作り、live は「未知」(§2-1a の `live_unknown`)にする。未知はどの
  digest とも等しくないので、各ファイルの最初の観測は、中身が同じ再保存でも
  1 回 reload を積む(起動時の中身を知らないため)。2 回目からは基準がある
- **settle 窓(FW-1)**: mtime が走査の開始から 100 ms 以内のファイルは
  size / mtime を信用せず、次の走査で読み直す(起動時も窓の中のファイル
  だけは hash する)。NTFS の mtime は連続の書き込みで同じ値になり、
  走査の直後に同じ size で書き直した保存を取りこぼすため(git の
  racy-clean と同じ考え方)。100 ms は debounce の 200 ms より短く、
  native watch が拾った保存は 1 回だけ hash する。mtime の刻みが窓より
  粗い store(FAT の 2 s、時刻のずれた network share)で同じ刻みの中に
  同じ size で 2 回保存すると、2 回目は取りこぼす(受け入れ済み)
- hash 前後で size/mtime/file identity を比較し、書き込み中ならデバウンス
  再開。安定 read が得られない間は旧リソース継続 + retry(永久失敗に
  しない)
- **デバウンス 200ms は既定値であって correctness 境界ではない** — 正本は
  「静穏後の安定 read + retry」

### 2-1a. digest の三状態分離(再レビュー HR-C1 — HR0 受入条件)

baseline・self-write consume・reconcile 比較を**一個の hash で実装しては
ならない**(consume 済み editor write が reconcile で外部変更として再出現
するか、runtime 未適用の disk 内容を適用済みと誤認する)。source ごとに:

- `observed_digest`(安定 read 済み disk)/ `live_digest`(runtime へ
  commit 成功済み)/ `pending_digest` / self-write token を**別状態**で持つ
- self-write を抑制できるのは、同じ editor transaction が **runtime への
  適用も成功させ `live_digest` を atomic に更新した場合だけ**。単なる
  file write の通知は抑制せず通常 reload する
- token consume は `observed_digest` を更新するが、runtime apply 成功
  なしに `live_digest` を進めない。disable/resume 後も disk/live の
  不一致を失わない
- (FW-1)起動時に hash しなかった source は `live_unknown`(live の中身が
  未知)。未知の live はどの digest とも等しくない(self-write の consume
  にも `unchanged` にも使わない)ので、最初の観測は `queue_reload`。消去は
  未知でも報告する。commit 成功と、runtime 適用済みの self-write の登録で
  未知は解ける
- fixture: (a) editor apply+write→resume で再 queue なし
  (b) write 成功/runtime apply 失敗→通常 reload
  (c) token 後に異なる外部 hash→reload
  (d) epoch 変更で旧 token 不一致

### 2-2. エディタ自己書き込みトークン

path TTL ではなく **`(AssetKey, expected hash, watcher epoch)`** で登録し、
一致した通知を一回だけ consume する。異なる hash(外部変更が割り込んだ)は
抑制しない。§2-1a の三状態と接続する(consume ≠ live 前進)。
登録 API の置き場は D2 の編集系 rpc 設計と同時に決める(§8)。

### 2-3. gate と reconcile(レビュー HR-B8 — arm/scan race の解消)

「先に scan して後から arm」はその間の変更を失う。プロトコル:

1. **disable**: gate epoch を増やし新規 queue を拒否 → pending apply を
   破棄 → §1-3 の停止手順で watcher を完全停止。「最後に適用成功した
   content inventory」は保持
2. **resume/reconcile**: 新 epoch の watcher を**先に arm**(適用は
   まだ禁止)→ 全 store を走査して保持 inventory と比較(size / mtime の
   比較で、hash は size か mtime が違うファイルにだけ。FW-1。`hot_reload`
   の宣言があれば、宣言に届く store・フォルダ・ファイルだけ。HW-1)
3. scan 中に buffer へ来た event を drain して該当 path を再 hash。
   hash 前後で変化したファイルは静穏まで retry。overflow は scan を
   もう一周
4. delete/rename を含む最終 delta を canonical AssetKey 順に queue し、
   epoch と gate が不変なら次フレーム先頭から apply。途中で gate が
   再度立ったら全 candidate を破棄
5. `watching=true` の意味 = arm 完了ではなく **reconcile barrier 完了**。
   status は `disabled | reconciling | watching | polling | degraded`

gate の発火源(リプレイ開始/終了・strict・rpc 駆動)は**中央 1 箇所**が
所有し、FileWatcher はそれを購読する。WP89 が replay 開始時に既存 shader
hot reload flag を折る処理は、この centralized gate へ統合する(各 handler
の独自判定で race を作らない)。

**宣言(HW-1、2026-10-02)**: gate の入力に project.json の `hot_reload` の
有無(`WatchDeclaration`: undeclared / empty / listed)が加わった。設定するのは
`ReloadService::setup()` で、起動時に 1 回(launch の再設定は宣言に触らない)。
PJ-1(2026-10-03)から、RPC `edit_project_settings` が project.json を書いた後に
`setup()` をもう一度呼ぶ。前の watcher を止め、resolver の今の store と宣言で
新しい watcher を作る(inventory はそのときのディスクから取り直す)。FW-1 の
累計(`scans` / `hashed_files` / `hashed_bytes`)は前の watcher の分を足して
引き継ぐ。
決めるのは gate の 1 か所(`ReloadGate::updateLocked()`)だけ:

- rpc 駆動の理由は undeclared のときだけ効く。listed なら rpc 駆動中でも監視する
- empty は監視しない(理由 `"hot_reload empty"`。rpc 駆動中でも `"rpc driver"`
  ではなくこちら)
- 理由の優先は replay → strict assets → rpc driver → hot_reload empty →
  shader_hot_reload disabled。headless は `shader_hot_reload = false` なので、
  宣言があっても止まる
- ✅AV-1(2026-10-02)から、`PELICAN_WITH_HOT_RELOAD=OFF` の build(配布 build)
  では理由 `"hot reload build disabled"` がこれらより上の最上位に入る
  (`updateLocked()` の `constexpr bool build_disabled`。`#if` ではなく定数)。
  OFF では他の理由に関係なく監視が無い: `ReloadService::setup()` は FileWatcher
  を作らず、宣言は読むが効かない(INFO 1 行)。game logic の DLL の poll もしない
  (明示の reload の経路は変えていない。`explicitShaderReloadRefusal()` が断るのは
  replay と strict だけ)

宣言の当たり判定は、`AssetKey` の path が要素と等しいか、要素をフォルダとして
その下か(成分ごと。`assets/tex` は `assets/textures/a.png` に当たらない。
大文字小文字は区別する)。宣言は起動時に読む。project.json を手で書き換えたら
engine を起こし直す。PJ-1(2026-10-03)から、その場で切り替える口は RPC
`edit_project_settings` で、engine が project.json に書き(project の文書の
transaction、digest の CAS)、`PathResolver::replaceDeclarations()` で resolver の
store の一覧と宣言を差し替え、`ReloadService::setup()` で watcher を作り直す。
`asset_stores` で受けるのは store の追加と manifest の宣言の追加だけで、mount の
移動と起動時からある store / manifest の削除・差し替えは
`project.settings_change_unsupported@1` で断る(manual 第3章 §3.4・第10章 §10.3)。
HW-2s(2026-10-03、`78737fdf`)から、Studio の Assets のドックがこの口を呼ぶ。
行の印は `get_status.reload.watch` を同じ当たり判定(成分ごと、大文字小文字を
区別する)で読み、親のフォルダで効いている行はその行だけでは外せない(`hot_reload`
に除外の書き方が無い)。
着地時に残った穴: 新しい watcher は、見張っていなかった間の外の変更を読み直さない。
resolver の差し替えは lock を取らず、main thread の frame の境目で呼ぶ前提。

### 2-4. polling fallback にも同じ reconcile 状態機械(再レビュー HR-C3 — HR0 受入条件)

fallback は「2s poll」ではなく watcher と同等の状態契約を持つ:

1. poll tick は前回完了後にだけ開始し、同一 store の走査(size / mtime の
   比較と、変わったファイルの hash。FW-1 から変更の無い tick は hash 0)を
   重ねない。各 scan は開始 epoch を持ち、完了時に epoch/gate が
   違えば結果を破棄する
2. polling の resume は「新 epoch の poll inventory 開始」を barrier と
   し、初回 full scan 完了までは `polling` でなく `reconciling`
3. watch 再試行 backoff。polling→watching の移行は **watch arm →
   inventory reconcile → poll 停止**の順とし、移行窓の変更を再 hash する
4. stop/cancel・stable-read retry・canonical delta・self-write・status は
   watcher と同じ ContentDigest/ReloadQueue 経路を通す(別実装禁止)
5. fixture: fake clock + watch 開始失敗で modify/delete/rename、scan 中
   gate、recovery 中変更、連続失敗→degraded を決定的に検査

## 3. リソース identity と種別表(レビュー HR-B3/B5)

### 3-1. 前提となる identity 基盤(HR1)

v1 の表が「低難度」とした行の多くは、現行コードに差し替え面がないため
成立しない(MaterialContainer は register のみ / ResourceContainer に
generation なし / removeModelInstance は slot を回収せず place は末尾
append — リロード反復で 1024 上限に到達)。よって種別ハンドラの前に:

- **canonical `AssetKey`**: **project logical reference + fragment を
  正本**とする(local override の物理絶対パスではない — 物理 file
  identity は watcher dedupe のみに使う。複数 store と local override は
  場所だけを変え意味を変えない — 再レビュー HR-C2-5)
- **logical resource generation**: `ModelAssetId {index, generation}` 等、
  差し替えても参照側が生きる世代付き ID。`ModelInstanceId` は
  generation/free-list 化するか、同じ instance record/render-command
  span を in-place rebuild する API を設ける

### 3-1a. identity の三概念分離(再レビュー HR-C2 — HR1 受入条件)

「世代」を一語で使わない。ECS の generation(slot 再利用の stale 検出)と
リロードの revision は意味が違う:

1. **`LogicalAssetId`** = asset の宣言寿命中 stable /
   **slot `generation`** = unbind/destroy 後の slot 再利用時のみ増加 /
   **`content_revision`** = commit 成功ごとに単調増加 — の三つを別定義
2. animation rig/layout のような**互換性破断を検出する revision** は
   logical handle の generation とは別 field
3. reverse dependency index の旧 edge 除去・新 edge 公開は logical
   handle table swap と**同じ commit barrier** で可視化
4. fixture: 同一 asset 1000 reload で logical ID 不変 + content_revision
   単調増加 / 削除→再宣言で旧 handle stale / group rollback で
   ID・revision・edge 全不変
- **reverse dependency index**: `AssetKey → 参照中の live resources`
  (model_name/fragment → template → instances、texture → material
  descriptor sets、root/include/virtual source → ShaderBundle/Pipeline/
  Material)。model container は現在 constructor-local 宣言を捨てて
  名前→template map しか残さないため、source identity の保持を追加する

### 3-2. 種別表(v2)

| 種別 | 反映方法 | 前提 | WP |
|------|---------|------|----|
| テクスチャ(独立画像 / KTX2) | 同 shape は in-place 再アップロード(logical `GlobalTextureId` 不変)。shape/format/mip 変化は image 再生成 + **参照中の全 material descriptor set を reverse index で再バインド** | HR1(descriptor は material 登録時に書かれるため逆引き必須) | HR1-T |
| material values(.material.json) | 再 parse/lower → 同 layout なら SSBO update API(新設 — 現行は登録時一回書きのみ)。surface layout が変わる場合は transaction group(§4)へ昇格 | HR1 | HR1-M |
| .surface / シェーダ | `FileWatcher`/`ContentDigest` が source/include/.surface の `AssetKey` を一括供給し、`ShaderLibrary` の reverse dependency から全 variant を prepare。`PipelineFactory` が依存 pipeline を全て構築し、必要なら material values/layout も同じ candidate に加え、全成功後だけ一括 publish。失敗時は旧 bundle/pipeline/material を継続し、旧 GPU pipeline/layout は `DeletionQueue` へ送る。独自 mtime poll は持たない | HR1 | HR2-S(WP108 済) |
| モデル(glb / フラグメント) | CPU candidate(PreparedGltf)→ GPU staged commit(§4)→ 参照中 instance の in-place rebuild。ECS EntityId・Transform・physics は不変のまま render commands / skinning binding を差し替え。**rig layout 変化時は animation cursor/snapshot を generation error → reset、WP88 temporal history も reset**(previous=current で zero velocity — anim 設計書 §1-5 と同じ規則)。container 1 個の変更でそれを参照する全 fragment template を再評価 | HR1 | HR2-G |
| input_actions / プロファイル | 再パース candidate → フレーム境界 swap。held input / consume の扱いを fixture 化 | HR0 | HR2-I |
| pelican.ui | U3 の hot reload トランザクションに委譲 — ただし **HR0 の AssetKey/epoch/reconcile 契約は共有**(UI 独自 watcher を作らない) | HR0 | U3 合流 |
| scene JSON | 自動リロード対象外(ゲーム状態の全消し)。rpc `load_scene` / ImGui ボタンの明示操作のみ | — | 対象外 |
| rendering config / feature JSON / pipeline preset | project-backed root とその feature / preset 依存を一つの watcher batch にまとめ、preview + flat (+ 起動中なら XR)を全 prepareする。同じbatchのshaderとmaterial valuesを選択的に候補へ加え、graph / shader / pipeline / materialを一回のruntime generation publicationで公開する。失敗時は旧root / registry / shader / material / watch dependencyを維持する | RPE10b2 | RPE10b3(WP196) + RPE8f(WP222 済) |
| project.json / manifest | 対象外(再起動事項) | — | 対象外 |

### 3-2a. rendering pipeline reload の境界(WP196)

pipeline participant が監視するのは、active flat program から確定した次の
project logical source である。

- `basic_config.rendering_config_json` の root
- project-backed render-pipeline preset
- project-backed render-feature instance

`engine://` / `user://` / 絶対・foreign source は project watcher identity に
入れない。同梱 feature を編集したい場合は project へコピーし、その参照へ
差し替える。shader source/includeは従来の shader participant が所有し、
pipeline participantとの二重claimを作らない。

**rpc 駆動中は watcher の要求がこの participant に届かない(HW-1)**:
`hot_reload` の宣言で rpc 駆動中に watcher が動くとき、`ReloadService::applyFrame()`
は watcher が届けた要求を `pelican.render_pipeline` の participant に claim させない
(`ReloadGate::rpcDriven()` のとき)。rpc 駆動中の render 文書(root / preset /
feature)は client の `reload_render_documents`(RL-1)が持ち、watcher からも
publish すると同じ文書で generation が 2 回出るため。誰も claim しない要求は
従来どおり捨てられる。除外を renderer の登録ではなく `applyFrame()` に置いたので、
`applyRequestsForTesting()` で participant を直接呼ぶ経路は変わらない。

同じ watcher frame に root、preset、feature の変更が複数届いても、pipeline
participantは一度だけ次を行う。

1. rootをdiskから読み直し、preview、flat、必要ならXRをCPU側で全解決する
2. 全variantを一つのGPU registration checkpointとowner scopeでprepareする
3. default pass、terminal、凍結済みruntime moduleとの整合をcandidate上で検証する
4. 一回のgeneration CAS後だけGPU arenaをcommitし、config cacheとwatch source集合を更新する
5. parse / shader compile / target planning / GPU registration / validationのどこで
   失敗してもcandidateだけをrollbackし、active generationと旧watch sourceを維持する

WP222ではparticipantの通常claim所有権を変えず、batch ownerが明示した
`companion_participants`からrequest単位で必要なものだけを選べる。render-pipeline ownerは
shader requestとmaterial-values requestを同じcandidateへ取り込み、texture requestは
取り込まない。選ばれたrequestはowner成功後にだけまとめて成功となり、失敗時は
companion participantを別経路で再実行しない。

ownerはprivate runtime generationに対してlive material ABIを再解決し、必要な
surface shader / graphics pipelineを候補化する。全検証後のmaterial commitは
publication mutex内でbase-generation stale検査の後、root CASの直前に一度だけ実行する。
したがってstale candidateやshader compile failureでgraphとmaterialの世代が分離しない。

起動後に初めて`ui` / `sprite` / `gpu_timing`等を有効化し、必要moduleが
startup profileで初期化されていない場合は、module graph凍結後の遅延生成をせず
publish前に名前付きエラーで拒否する。既に初期化済みのfeatureの編集・再有効化は
同じreload経路を使える。

GPU resourceの退役条件は論理frame終了ではない。各swapchain in-flight slot、
offscreen submit、OpenXRの各eye submit、独立desktop mirror submitが、使用した
runtime generationをtype-erased leaseとして保持する。対応fence完了後だけleaseを
解放するため、active rootが差し替わってCPU側参照が消えても、GPUが旧pipeline /
descriptor / targetを実行中なら旧scopeは退役しない。

### 3-3. delete / rename の policy

file delete を「load failure」と同一視しない:

- logical asset が manifest/宣言に残ったままの delete = 旧リソース継続 +
  missing error(status に出す)
- 宣言自体の削除 = dependency transaction として unbind(参照側は
  fallback 資源 or 名前入りエラー描画)

## 4. 適用パイプライン — dependency transaction group(レビュー HR-B4/B7)

v1 の「種別の浅い順に固定順で適用」は不可(同じ保存で .surface の params と
.material.json values が変わると、旧 layout に values を先 bind する)。

```
FileWatcher(検知)
  → ContentDigest(安定 read + hash 照合 + 自己書き込み consume)
  → ReloadQueue: AssetKey と reverse dependency index から
    **reload transaction group** を構成
    (edge: surface→material values / container→fragments /
     texture→descriptors。無関係 group は独立)
  → フレーム先頭の適用フェーズ:
     group 内は parse all → validate all → stage all →
     commit(topological order)
     - CPU candidate: parse/decode/lower/reflect/全 capability 検証。
       **global ID を発行しない**
     - GPU staged commit: candidate resource set を構築し、全成功後に
       logical handle table を atomic swap。旧 set は DeletionQueue へ
     - 途中失敗 = candidate set のみ破棄(global allocator/offset を
       回復)。旧リソース継続 + WARN + status
  → 1 行 INFO `reload: <group> <パス...> ok|failed (<時間>ms)`
```

一つのrequestを複数participantが通常claimしてはならない。複数asset種別を同じ
publicationへ合流させる場合は、ownerとcompanionの組を登録時に固定し、ownerの
`companion_claims`がrequest単位で選択する。選択されなかったrequestは元participantの
通常transactionへ残す。この境界により「material participantだからtextureまで
render graph reloadへ巻き込む」といった過剰な直列化を避ける。

- 現行 `GltfLoader::commit` は即座に global Material/VertBuf container へ
  登録し、途中失敗の rollback がない — HR1 で candidate/staged commit に
  分離する。append-only mega-buffer を維持する場合は suballocation/
  free-list/compaction か、reload candidate 専用 buffer ownership の
  いずれかが必要
- GPU in-flight 資源は既存 DeletionQueue(遅延破棄)に乗せる

## 5. 観測点

- `get_status.reload`(**単一 schema — v1 の二重定義を統一**):

  ```json
  {
    "state": "disabled|reconciling|watching|polling|degraded",
    "epoch": 3,
    "applied": 12,
    "failed": 1,
    "last_watcher_error": {"store": "...", "message": "..."} | null,
    "last_reload_error": {"path": "...", "kind": "...", "message": "..."} | null
  }
  ```

  watcher error(監視自体の異常)と resource reload error(candidate
  失敗)を分ける

  今の実物(`ReloadService::statusJson()`、FW-1 / HW-1 の後)は上の例と
  ずれている。`last_watcher_error` は無く、監視の異常と gate が閉じている
  理由は `error`(文字列か `null`)に出る。ほかに `participants`・`runtime`・
  `model_catalog` があり、HW-1 で次の 4 つが足された:

  ```json
  {
    "state": "watching", "epoch": 3, "error": null,
    "applied": 1, "failed": 0, "participants": [...], "runtime": {...},
    "model_catalog": null, "last_reload_error": null,
    "watch": ["assets/textures", "passes"],
    "scans": 4, "hashed_files": 2, "hashed_bytes": 20
  }
  ```

  `watch` は正規化した `hot_reload` の宣言(無ければ `null`、空の配列なら
  `[]`)。`scans`(完了した走査の回数)・`hashed_files`・`hashed_bytes` は
  FW-1 の `WatcherStatus` の起動からの累計で、watcher が無いときは 0
  (PJ-1 から、`edit_project_settings` が watcher を作り直しても前の分を足して
  数え続ける)
- log(FW-1): reconcile が終わるたびに、hash したファイルがあれば(起動後の
  最初は 0 でも)INFO を 1 行 `file watcher: scanned {件数} files, hashed
  {件数} ({x.x} MB) in {ms} ms`(件数は走査後の inventory の数、MB は 10^6 byte)
- ImGui にリロード履歴パネル(WP86 の流儀で後続 — 初期はログのみ)

## 6. テスト戦略(レビュー §8)

mock queue 注入は handler の unit test に使うが、**Watcher correctness は
mock では検証できない** — HR0 に Windows integration suite(render/GPU
不要・実 FS)を必須とする:

- normal modify / create/delete / atomic-save temp→rename /
  rename OLD/NEW / Unicode・長い相対パス
- **synthetic overflow 注入**(0 bytes / `ERROR_NOTIFY_ENUM_DIR`)→
  full rescan → 変更の全回収。決定的 injection と stress の両方
- pending overlapped read 中の stop → `CancelIoEx` → join。
  buffer/OVERLAPPED の UAF なし(ASan/検証ビルドで)
- disabled 中の変更 → resume の arm→scan 中の再変更 → resume 中の
  再 disable。最終 hash が**一度だけ** queue される
- watch 開始失敗 → polling fallback 降格と status、復帰
- overlapping mounted store の dedupe、store 外 reparse escape 拒否
- 自己書き込み `(path, hash, epoch)` は一致一回だけ抑制、異なる外部
  hash は抑制しない

resource handler suite(HR1 以降・GPU あり)は別に:

- candidate 失敗後に live ID/count/絵が不変
- **1000 回リロードで instance/texture/material/vertex allocation が
  単調 leak しない**
- GPU in-flight の遅延破棄(フレーム跨ぎで crash しない)
- model の rig layout 変化 → stale animation reset + temporal history
  reset(骨 palette の ghost なし)
- 決定性: rpc 駆動中に監視が止まっている(書き換えても capture 不変)。
  HW-1 からは「`hot_reload` を宣言しない project で」。宣言した project では
  宣言したフォルダの texture が rpc 駆動中にも反映され、外は反映されないことを
  見る(`texturereload_test` の HW-1 の GPU test)

## 7. WP 分割(レビュー §9 — v1 の H1/H2 を廃止)

| WP | 内容 | exit gate | 依存 |
|----|------|-----------|------|
| **HR0 Watch/Reconcile** | Win32 overlapped watcher(§1)・poll fallback・ContentDigest(§2-1)・inventory・デバウンス・overflow rescan・gate epoch・resume barrier(§2-3)・ReloadQueue。resource handler は fake のみ | §1・§2 の規範 + §6 の非 GPU integration suite 全 green。既存 shader poll との二重適用なし(gate 統合) | なし |
| **HR1 Identity/Transaction** | canonical AssetKey・logical resource generation・reverse dependency index(§3-1)・CPU candidate/GPU staged commit(§4)・DeletionQueue 接続・status schema(§5) | candidate 失敗で global counts/IDs/live bytes 不変。old/new generation と dependency group の fixture | HR0 |
| **HR1-T Texture** | texture replace(同 shape upload / shape・format・mip 変化の再生成)+ 全 descriptor rebind | 同 GlobalTextureId 維持・1000 reload leak なし・KTX2 mip/format fixture | HR1 |
| **HR1-M Values** | .material.json 再 parse/lower・同 layout SSBO update・surface layout 変化は transaction group | WP76 layout fixture 再利用・旧 values 継続・二体/material 単位更新 | HR1 |
| **HR2-S Surface/Shader** | FileWatcher への移行・include/source reverse dependency(WP82 source graph と同じ入力集合)・全 variant/pipeline/material-layout transaction | root/include/.surface edit・compile 失敗で旧絵・cache hit/miss・in-flight 旧 pipeline・cross-file rollback | HR1・**WP108 完了** |
| **HR2-G Model/Fragment** | ModelAsset/Instance generation・fragment reverse index・staged GLTF GPU commit・live instance in-place rebuild・animation/temporal reset | WP77 fragment 全種・複数 instance・rig 変化・1000 reload・failure rollback | HR1・**WP110 完了** |
| **HR2-I Input** | input_actions/profile の candidate + フレーム境界 swap | held input/consume policy・invalid candidate rollback・gate epoch | HR0 |

UI(U3)は HR0 の AssetKey/epoch/reconcile 契約だけを共有して独立に進む。

**WP 境界を跨ぐ gate(再レビュー HR-C4)**:

1. `get_status.reload` の **watcher state/epoch/error 部分は HR0 が所有**。
   HR1 は resource counters/errors を additive に拡張する
2. HR0 完了時に現 `EngineLaunchConfig::shader_hot_reload` を centralized
   gate の adapter 化。HR2-S(WP108)で ShaderLibrary の時刻 poll を削除済み。
   **FileWatcher 以外の経路から同じ shader を自動 apply しない**
3. HR1-M 単体 gate は same-layout update + fake dependency actor まで。
   実 `.surface + .material.json` の cross-file atomic fixture は
   **HR2-S の exit gate** に置く
4. HR1 framework は複数 logical table の commit を一つの frame-boundary
   barrier で公開し、observer が group の半端な revision を読めないことを
   fixture 化する

## 8. 未決事項

1. mounted store がネットワーク越しの場合のポーリング間隔(既定 2s —
   実測待ち)
2. エディタ自己書き込みトークンの登録 API の置き場(rpc か devstudio
   直結か — D2 の編集系 rpc 設計と同時に)
3. mega-buffer の回収方式(suballocation/free-list vs reload 専用
   buffer ownership)— HR1 設計時に現行 VertBufContainer の実測で決める
4. pipeline feature追加時に必要なruntime module profile自体をhot-expandするか
   (現状は未初期化moduleをpublish前rejectし、再起動を要求)
