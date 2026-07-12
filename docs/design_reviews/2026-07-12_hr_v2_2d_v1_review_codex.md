# アセットホットリロード v2 再レビュー + 2D ゲーム層 v1 初回敵対レビュー

日付: 2026-07-12  
対象: `docs/design_asset_hot_reload.md` v2、`docs/design_2d_game_layer.md` v1  
前回正本: `docs/design_reviews/2026-07-12_anim_v2_hotreload_review_codex.md` §6〜9

## 0. 結論

| 文書 | 判定 | 結論 |
|---|---|---|
| `design_asset_hot_reload.md` v2 | **条件付き** | 前回の再受理条件 5 点と HR-B1〜B8 は、名前だけでなく実装可能な分割・状態遷移・transaction としてほぼ全て入った。v1 の Reject は解除する。ただし、自己書き込みを consume した後の inventory/live hash、logical ID と content revision の区別、poll fallback の完全な状態機械、HR1-M と HR2-S の結合 gate が未契約である。**HR0/HR1 着手は本書 HR-C1〜C4 を各 WP の acceptance criteria に添付することを条件に可**。根拠: `docs/design_asset_hot_reload.md:32-78`, `docs/design_asset_hot_reload.md:80-129`, `docs/design_asset_hot_reload.md:131-204`, `docs/design_asset_hot_reload.md:225-266`。 |
| `design_2d_game_layer.md` v1 | **Reject** | 「単一 ECS/world identity、別 2D world は作らない」という中核方向は妥当。ただし現在の文書は、その決定を「2D 専用機構も増やさない」と過剰に一般化している。さらに SpriteView の JSON が scene v1 と不一致、U1 の framebuffer-px quad ABI は world quad に流用不能、camera snap だけでは pixel perfect にならず、現 physquery だけでは堅牢な platformer を書けない。S2D-0 は複数の未確定 ABI を一 WP に抱えている。**本書 2D-R1〜R6 を反映して v2 に再起草するまで S2D-0 着手不可**。根拠: `docs/design_2d_game_layer.md:22-55`, `docs/design_2d_game_layer.md:59-100`, `docs/design_2d_game_layer.md:101-128`, `docs/design_2d_game_layer.md:157-183`。 |

レビューは文書だけでなく、現行 scene loader、U1 CPU/GPU quad、UI resource ownership、WP48 camera、世代付き ECS ID、P1/P2 physquery と実装計画を突き合わせた。ビルド・テストは依頼どおり実施していない。

---

## 1. ホットリロード v2: 前回の再受理条件 5 点

| # | 前回条件 | v2 | 判定 |
|---:|---|---|:---:|
| 1 | Win32 state machine と overflow/rescan | handle flags、request lifetime、64 KiB 以下、即 re-arm、0-byte / `ERROR_NOTIFY_ENUM_DIR` から inventory rescan、rename、`CancelIoEx` 後の completion 回収まで規定した (`docs/design_asset_hot_reload.md:32-71`) | **解消** |
| 2 | resource ごとの identity/reverse dependency/candidate/commit/reset | canonical AssetKey、logical generation、reverse index を先行基盤にし、texture/material/shader/model/input/UI ごとの反映法を分けた (`docs/design_asset_hot_reload.md:131-164`) | **解消。ただし HR-C2** |
| 3 | 固定種別順を dependency transaction group に置換 | reverse dependency から group を作り、parse all → validate all → stage all → topological commit、失敗時 candidate のみ破棄とした (`docs/design_asset_hot_reload.md:175-204`) | **解消** |
| 4 | platform watcher と leak/rollback fixture | 実 FS、overflow injection、stop、resume race、fallback、dedupe、self-write と、GPU 側 rollback/leak/history を別 suite にした (`docs/design_asset_hot_reload.md:225-252`) | **解消。ただし HR-C1/C3** |
| 5 | HR0〜HR2 再分割と既存 shader poll 移行 | 前回表と同じ HR0、HR1、HR1-T/M、HR2-S/G/I へ分け、`ShaderLibrary` poll の移行を HR2-S にした (`docs/design_asset_hot_reload.md:254-266`) | **解消。ただし HR-C4** |

五点は全て実質的に反映された。v1 の「方向はよいが見積単位として成立しない」という Reject 理由は残っていない。

## 2. ホットリロード v2: HR-B1〜B8 再照合

| finding | 判定 | 根拠 |
|---|:---:|---|
| HR-B1 Watcher/overflow/stop | **解消** | buffer/OVERLAPPED lifetime、overflow→rescan、rename 非 pairing、cancel→completion 回収を明記 (`docs/design_asset_hot_reload.md:39-71`)。 |
| HR-B2 WP82 hash の誤流用 | **解消** | 流用を picosha2 に限定し、ContentDigest、stable read、適用成功 baseline を新設 (`docs/design_asset_hot_reload.md:82-100`)。 |
| HR-B3 EntityId と model resource ID の混同 | **解消** | `ModelAssetId`、instance generation/free-list または in-place rebuild、source identity/reverse index を別基盤として要求 (`docs/design_asset_hot_reload.md:133-150`)。 |
| HR-B4 GPU commit が transaction でない | **解消** | 現 `GltfLoader::commit` の制約を認め、CPU candidate と GPU staged commit、allocator 回復を HR1 に要求 (`docs/design_asset_hot_reload.md:187-204`)。 |
| HR-B5 texture/material values の replace 面がない | **解消** | 全 descriptor rebind と SSBO update API を新設事項として明記し、HR1-T/M へ分離 (`docs/design_asset_hot_reload.md:152-157`, `docs/design_asset_hot_reload.md:259-261`)。 |
| HR-B6 shader reload を未実装扱い | **解消** | 現行 1 s poll の移行、`.surface`/include の穴、全 variant/pipeline 一括 swap を明記 (`docs/design_asset_hot_reload.md:158-158`, `src/core/shader/shaderlibrary.cpp:337-393`, `src/core/vkcore/renderer.cpp:705-715`)。 |
| HR-B7 固定種別順 | **解消** | dependency edge と transaction group に置換 (`docs/design_asset_hot_reload.md:175-196`)。 |
| HR-B8 resume の arm/scan race | **解消** | epoch、先 arm、scan、drain/re-hash、再 overflow、barrier、再 gate 時 candidate 破棄を順序付きで規定 (`docs/design_asset_hot_reload.md:108-129`)。 |

前回 §7 の非 blocking 項目も、status の watcher/resource error 分離 (`docs/design_asset_hot_reload.md:206-222`)、重複/escape (`docs/design_asset_hot_reload.md:73-78`)、delete policy (`docs/design_asset_hot_reload.md:166-173`)、stable read (`docs/design_asset_hot_reload.md:93-100`)、animation/history reset (`docs/design_asset_hot_reload.md:159-159`, `docs/design_asset_hot_reload.md:244-252`)として回収された。

## 3. ホットリロード v2 の受理条件

### HR-C1: self-write consume と inventory/live baseline を一つの hash にしない

文書は baseline を「最後に適用成功した hash」に固定する一方 (`docs/design_asset_hot_reload.md:93-95`)、editor self-write は一致通知を一回 consume して runtime reload を抑制する (`docs/design_asset_hot_reload.md:102-106`)。resume は保持 inventory と disk scan を比較する (`docs/design_asset_hot_reload.md:112-120`)。この三つを一個の hash で実装すると、consume 済みの editor write が次の reconcile で再び外部変更として現れるか、逆に runtime へ未適用の disk 内容を適用済みと誤認する。

**HR0 acceptance criteria に次を追加する。**

1. source ごとに最低限 `observed_digest`（安定 read 済み disk）、`live_digest`（runtime resource へ commit 済み）、`pending_digest`、self-write token を別状態として持つ。
2. self-write を抑制できるのは、同じ editor transaction が runtime への適用も成功させ `live_digest` を atomic に更新した場合だけとする。単なる file write の通知は抑制せず通常 reload する。
3. token consume は `observed_digest` を更新するが、runtime apply 成功なしに `live_digest` を進めない。disable/resume 後も disk/live の不一致を失わない。
4. fixture は (a) editor apply+write→resume で再 queue なし、(b) write 成功/runtime apply 失敗→通常 reload、(c) token 後に異なる外部 hash→reload、(d) epoch 変更で旧 token 不一致、を検査する。

### HR-C2: logical identity、slot generation、content revision を分離する

「差し替えても参照側が生きる世代付き ID」と `ModelAssetId {index,generation}` が同じ箇所に書かれている (`docs/design_asset_hot_reload.md:140-145`)。現 ECS の generation は slot 再利用で stale handle を拒否する語彙であり (`src/core/userpublic/details/ecs/entity.hpp:12-17`, `src/core/userpublic/details/ecs/coretemplate.cpp:76-85`)、content hot reload ごとに増やす revision とは意味が違う。ここを混ぜると、reload 追従すべき参照が stale になるか、destroy/recreate を検知できない。

**HR1 acceptance criteria に次を追加する。**

1. `LogicalAssetId` は asset の宣言寿命中 stable、slot `generation` は unbind/destroy 後の再利用時のみ増加、`content_revision` は commit 成功ごとに増加、と定義する。
2. animation rig/layout のように互換性破断を検出する revision/generation は logical handle generation と別 field にする。
3. reverse dependency index の旧 edge 除去と新 edge 公開を、logical handle table swap と同じ commit barrier で可視化する。
4. fixture は同一 asset 1000 reload で logical ID 不変、content revision 単調増加、削除→再宣言で旧 handle stale、group rollback で ID/revision/edge 全不変を検査する。
5. `AssetKey` は local override の物理絶対 path ではなく、project logical reference + fragment を正本にし、物理 file identity は watcher dedupe のみに使う。複数 store と local override は場所だけを変え意味を変えない (`docs/design_project_vcs.md:29-37`, `docs/design_project_vcs.md:58-68`)。

### HR-C3: polling fallback に watcher と同じ reconcile 状態機械を与える

fallback は watch 開始失敗/network path で 2 s poll とだけ定義されている (`docs/design_asset_hot_reload.md:34-37`)。一方 resume protocol は「watcher を先に arm」「buffer を drain」「overflow」を前提とする (`docs/design_asset_hot_reload.md:108-124`)。polling 時の初期 inventory、scan の重複、gate 中断、watching への復帰点が未定義である。

**HR0 acceptance criteria に次を追加する。**

1. poll tick は前回完了後にだけ開始し、同一 store の scan/hash を重ねない。各 scan は開始 epoch を持ち、完了時に epoch/gate が違えば結果を破棄する。
2. polling resume は watcher の arm の代わりに「新 epoch の poll inventory 開始」を barrier とし、初回 full scan 完了までは `polling` ではなく `reconciling` とする。
3. watch 再試行の backoff、polling→watching の移行は **watch arm→inventory reconcile→poll 停止**の順とし、移行窓の変更を再 hash する。
4. stop/cancel、stable-read retry、canonical delta、self-write、status/error は watcher と同じ ContentDigest/ReloadQueue 経路を通す。
5. fake clock + watch-start failure fixture で modify/delete/rename、scan 中 gate、recovery 中変更、連続失敗/degraded を決定的に検査する。

### HR-C4: WP 境界を跨ぐ integration gate を明記する

HR1-M は surface layout 変更を transaction group に上げるが (`docs/design_asset_hot_reload.md:157-157`, `docs/design_asset_hot_reload.md:261-261`)、実 surface handler は HR2-S である (`docs/design_asset_hot_reload.md:262-262`)。また HR0 の status と centralized gate は HR0 の試験に必要なのに、WP 表では status schema が HR1 の内容である (`docs/design_asset_hot_reload.md:208-222`, `docs/design_asset_hot_reload.md:258-259`)。

**WP 表へ次を追記する。**

1. HR0 が `get_status.reload` の watcher state/epoch/error 部分を所有し、HR1 は resource counters/errors を additive に拡張する。
2. HR0 完了時は現 `EngineLaunchConfig::shader_hot_reload` を centralized gate の adapter にし、HR2-S 完了時に `reloadModifiedSources` の時刻 poll を削除する。二経路が同時に同じ shader を apply できる中間状態を作らない (`src/core/vkcore/renderer.cpp:705-715`)。
3. HR1-M 単体 gate は same-layout update と fake dependency actor まで。real `.surface + .material.json` cross-file atomic fixture は HR2-S の exit gate に置く。
4. HR1 framework は複数 logical table の commit を一つの frame-boundary barrier で公開し、observer が group の半端な revision を読めないことを fixture 化する。

これらは設計方向の差し戻しではなく、実装時に二通りへ解釈される状態契約を閉じる条件である。従って判定は Reject ではなく条件付きとする。

---

## 4. 2D v1: 中核決定「world は一つ」の評価

### 4.1 受理できる部分

単一 ECS identity、既存 Transform/parent、camera、event/replay/editor を再利用し、2D→3D を transform、3D→2D を projection として扱う方向は合理的である (`docs/design_2d_game_layer.md:22-34`)。現 scene v1 も parent を同じ `EntityId`/Transform hierarchy で表し、scene 専用階層を作らない (`docs/design_scene_format.md:80-86`)。2D/3D 混在を第一要件とする本プロジェクトでは、別 world・別 ID・同期ブリッジを最初から作る利益は小さい。

### 4.2 公正でない部分

しかし比較対象を「別 world = 別 ID と同期」とだけ置くのは偽の二択である (`docs/design_2d_game_layer.md:24-30`)。**単一 ECS/world ownership のまま、2D 専用 render extraction、spatial index、tile chunk、logical canvas transform を持つ**構成は可能である。「エンジン機構は増やさない」まで禁止すると (`docs/design_2d_game_layer.md:36-38`)、大量 sprite の culling/chunking、tilemap、logical-resolution render target を実装する場所まで失う。

Godot 分離型の実利は別 ID そのものではなく、2D transform/canvas ordering/visibility/physics/tilemap を 3D の depth/material path から独立最適化できることにある。本設計は ID/world の分離を不採用にしてよいが、その実利まで不要と結論してはならない。文書自身も tilemap を将来枠に持つ (`docs/design_2d_game_layer.md:82-88`, `docs/design_2d_game_layer.md:176-183`)。

**2D-R1:** 中核決定を「単一 ECS world・単一 EntityId・Transform hierarchy 共用。第二の scene ownership は作らない」と限定すること。2D render extraction/culling、tile chunk、pixel-perfect compositor、query helper は同じ world の派生 index/consumer として許可すること。これなら中核決定を維持したまま、数万 sprite と tilemap の逃げ道を残せる。

## 5. 2D v1 の blocking findings

### 2D-R2: SpriteView の交換形式が scene v1 と一致しない

文書例は `{ "type": "sprite_view", "texture": "assets/atlas.json#sprite/..." }` である (`docs/design_2d_game_layer.md:59-70`)。現 scene v1 は component の `name` を `ComponentInfoManager` で dispatch し (`docs/design_scene_format.md:15-24`, `src/core/loader/scene.cpp:92-117`)、component params の直接 file reference も禁止している (`docs/design_scene_format.md:98-106`)。したがって例は現行 loader では `component.at("name")` で失敗し、直接 atlas/image path も scene 正本に反する。

**v2/S2D-0 条件:** 

1. 形式を `{"name":"sprite_view", ...}` に直し、component 登録名、public struct、`ref`/validation、未知 field、既定値を閉じる。
2. `texture` が project asset ID なのか project logical ref なのかを scene/asset policy と一度だけ整合させる。現 scene v1 を維持するなら asset declaration ID + `#sprite/...` とし、単独画像も暗黙 atlas resource の asset ID にする。
3. atlas fragment の rename/missing、size/pivot/layer の範囲、finite transform/color、sampler の validation fixture を S2D-0a に置く。

### 2D-R3: U1 と共有できるのは資産・規約・一部 helper であり、quad ABI ではない

2D 文書は texture page、sampler、alpha/color と「QuadCommand 相当の POD と index 展開」を共有するとする (`docs/design_2d_game_layer.md:40-55`)。しかし現 U1 の `QuadCommand` は整数 framebuffer rect と UI `(layer,decl_seq)` を持ち、`QuadVertex` は `float2 position` 20 B である (`src/core/ui/drawcommands.hpp:18-28`, `src/core/ui/drawcommands.hpp:31-52`)。CPU 展開も rect の四隅を framebuffer px vertex に焼く (`src/core/ui/drawcommands.cpp:31-46`)。UI vertex shader はその px を直接 clip position に変換し、depth は固定 0 である (`src/core/resources/ui.vert:6-17`)。world transform、camera VP、rotation、pivot、billboard、per-vertex depth を表せない。

さらに atlas GPU page は `UiModule::pages()` から `UIContainer` が所有する (`src/core/renderer/uicontainer.cpp:104-138`)。UI feature が無ければ UiModule/GPU resource/parser を生成しないのが U1 の gate である (`docs/design_reviews/2026-07-12_wp87_report.md:15-19`)。2D game が UI を使わない場合にこの container を共有すると purge 規約を破る。

**v2/S2D-0 条件:** 

1. 共通層を `AtlasDocument/AtlasAsset`、texture page identity、sampler key、straight-alpha/color contract、quad index topology/chunk helper に限定する。
2. UI は `UiQuadCommand/QuadVertexPx`、sprite は `SpriteCommand`（world transform/pivot/UV/color/page/layer/sort/billboard）と world vertex/instance ABI を別に持つ。pipeline/pass/depth/sort/coordinate transform は共有しない。
3. atlas parser/upload/page lifetime を UiModule/UIContainer から consumer-neutral resource へ抽出し、UI と sprite の一方だけを有効にしても他方を初期化しない。両方有効時は同じ image/page allocation を参照する。
4. U1 の 16384 quad は一 document の hard limitであり (`src/core/ui/drawcommands.hpp:15-16`, `src/core/ui/drawcommands.cpp:14-18`)、数万 sprite へ転用しない。sprite は visibility cull 後に複数 `uint16` chunk または `uint32`/instancing へ分ける。

### 2D-R4: camera-side `1/ppu` snap だけでは pixel perfect にならない

文書は sprite size を texture px / ppu とし、camera position を ppu 格子へ丸めれば nearest と合わせて dot game が成立するとする (`docs/design_2d_game_layer.md:90-100`)。しかし現 orthographic projection は `[-xmag,+xmag] × [-ymag,+ymag]` を framebuffer 全体へ写し (`src/core/renderer/camera.cpp:424-433`)、resize 時にも xmag/ymag は変わらない (`src/core/renderer/camera.cpp:513-515`)。従って一 framebuffer pixel の world 幅は `2*xmag/width`、高さは `2*ymag/height` であり、一般には `1/ppu` ではない。任意 xmag/ymag、非整数 zoom、奇数 viewport、subpixel sprite transform では camera を `1/ppu` に丸めても shimmer/uneven texel size が残る。

**v2/S2D-1 条件:** 

1. `ppu`（asset size）と `world_units_per_framebuffer_pixel`（projection/viewport）を分離し、`zoom = framebuffer_pixels_per_source_pixel` の許可値を定義する。
2. strict pixel-perfect mode は (a) logical-resolution target へ 1 source texel = 1 target pixel で描いて整数倍 nearest upscale、または (b) world→view 後の render-only vertex/pivot quantization、のどちらかを採用する。物理 Transform は変更しない。
3. camera snap は `1/ppu` 固定でなく、選んだ projection の world-units-per-target-pixel と pixel-center/half-texel 規則で行う。x/y pixel density 不一致、非整数 upscale、viewport letterbox 時の policy を error/letterbox/非 strict のいずれかに固定する。
4. 個別 sprite snap を component に持たせなくてもよいが、fractional moving sprite を strict mode でどう量子化し、smooth camera interpolation とどう両立するかを renderer policy として持つ。
5. golden は odd/even texture、odd/even viewport、zoom 1/2/3、fractional camera、fractional sprite、rotated/non-uniform scaled sprite（strict 対象外なら明示）、atlas edge を含める。

### 2D-R5: sort key と depth 合成の意味論が閉じていない

`layer → z/y_down/declaration → EntityId index` は方向として妥当だが (`docs/design_2d_game_layer.md:101-118`)、次が未定義である。

- `declaration` を生成順と呼ぶ一方、現 ECS index は free-list から再利用される (`src/core/userpublic/details/ecs/coretemplate.cpp:76-85`, `src/core/userpublic/details/ecs/coretemplate.cpp:191-205`)。index は monotonic declaration sequence ではない。
- z/y は float であり、NaN/inf、`-0/+0`、epsilon 同値、z を pivot/center/bounds のどこから取るかがない。EntityId tie-break を置くだけでは comparator の strict weak order を保証しない。
- depth test ON/write OFF は opaque 3D に sprite を遮蔽させ、sprite 同士を painter order にするには正しい (`docs/design_2d_game_layer.md:120-128`)。ただし sprite は後続 draw の occluder にならず、layer は幾何 depth を上書きする。これは仕様としてよいが、opaque cutout、3D transparent、交差/大きな回転 quad の限界を明記すべきである。

**v2/S2D-0 条件:** 

1. sprite 登録時に monotonic `declaration_seq:uint64` を発行し、scene declaration/replay/recreate 時の規則を固定する。EntityId は最終 tie-break に full `(index,generation)` を使い、生成順の代用にしない。
2. transform/sort input は finite を必須にし、z は view-space pivot depth、y_down は world-space pivot Y など sampling point を固定する。比較は exact total key（必要なら canonical float key/quantization）を作ってから行う。
3. policy/layer/declaration sequence を camera/project のどちらが所有するか、複数 camera で command list を再生成するかを決める。
4. fixture は ID 再利用、同 z/y、`-0/+0`、invalid non-finite、複数 layer、camera 移動、billboard、opaque 3D の前後を含み、二回の command bytes と最終絵を比較する。
5. depth test ON/write OFF は v1 の標準として受理する。3D transparent 相互ソートは既知制限のままでよいが、sprite depth prepass/alpha-cutout を将来拡張点として予約する。

### 2D-R6: raycast + overlap だけでは platformer の最小閉包にならない

文書は平面拘束と既存 raycast/overlap で 2D 用途に足りるとする (`docs/design_2d_game_layer.md:157-166`)。現 API は ray×sphere/box/capsule、boolean overlap、closest ray、ID 列だけである (`src/core/phys/physquery.hpp:45-75`)。元設計も sweep、all-hit、layer/mask を v2 予約にしている (`docs/design_physics_queries.md:21-25`)。この面では次を安全に書けない。

- capsule/box を移動量に沿って sweep する continuous collision（薄い床/壁の tunneling 防止）
- slope normal と time-of-impact に基づく slide/step/ground 判定
- initial overlap の penetration vector/depenetration
- one-way floor を無視した後の「次の hit」（closest 一件しかない）
- player 自身/trigger/別 plane を除く layer/mask/filter

少数 ray を足元から飛ばす hand-authored game は書けるが、それを platformer 一般の成立根拠にしてはならない。3D physics simulation や Box2D を今導入する必要はない。**最小 engine 面は query の拡張**で足りる。

**S2D-P（S2D-2 より前）の条件:** 

1. capsule/box の deterministic `shapeCast/sweep(start, delta, filter)` → TOI、position、normal、stable collider/entity ID を追加する。
2. layer/mask、self/ignore set、trigger、one-way metadataを query filter で扱う。one-way は previous position/approach direction を使う user-space policyでもよいが、無視後の次 hit を取得できる all-hit/filter 継続が必要。
3. initial overlap に対する signed distance/MTD または deterministic depenetration query を追加する。
4. `moveAndSlide`、slope limit、step-up、coyote time、moving-platform attachment は標準 user-space library に置いてよい。engine は sweep/contact data と stable identity だけを保証する。
5. fixture は落下接地、斜面上り/下り、壁 slide、corner、薄床高速移動、下から one-way 通過、上から接地、initial overlap、同時 hit tie を fixed-step で検査する。

## 6. 大量 sprite・tilemap と単一 world の破綻条件

単一 world 自体は数万 sprite で破綻しない。破綻するのは「1 sprite = 必ず一 ECS entity + 毎 frame 全件 CPU sort + U1 の一 document buffer」の実装に固定した場合である。現 U1 は 16384 quad を超えると error にするため (`src/core/ui/drawcommands.cpp:14-18`)、そのまま共有する S2D-0 は要求を満たさない。

v2 は次の escape hatch を明記すること。

1. ordinary SpriteView は ECS entity でよいが、TileMap/particle/foliage は一 entity が chunk/instance 列を供給できる render source とする。tile 一枚ごとの EntityId を要求しない。
2. camera frustum/canvas bounds で visibility cull してから sortし、結果を 16384 以下の draw chunk に分割する。static chunk は transform/atlas/sort revision が変わるまで command cache を再利用する。
3. 50,000 logical sprite（うち可視 2,000 程度）の CPU fixture を S2D-0a に置き、出力順/hash、chunk 上限、非可視除外、同一入力再実行一致を検査する。性能値は WP29 の CPU/GPU 計測形式で記録する。
4. tilemap format は将来でもよいが、S2D ABI が「一 command は一 entity」や「一 frame 最大 16384」に凍結されないことを S2D-0 で保証する。

この形なら Godot 型の第二 world を作らずに、Godot 型が持つ 2D 専用最適化の利益を取り込める。

## 7. 実装順の再分割案

現 S2D-0 は component schema、atlas resource refactor、world command ABI、sort、GPU pass、depth、golden を一度に含み (`docs/design_2d_game_layer.md:176-182`)、U1 を「そのまま使える基盤」と誤認している。WP48 が実装したのは glTF orthographic projection であって pixel snap ではなく (`docs/implementation_plan.md:921-935`)、P1/P2 は ray/overlap query までである (`docs/implementation_plan.md:875-919`)。

| WP | 内容 | exit gate | 依存 |
|---|---|---|---|
| **S2D-0a Contract/CPU** | scene v1 に合う SpriteView schema/public component、consumer-neutral AtlasAsset 抽出、SpriteCommand/world ABI、sort total key、visibility/caching/chunking | schema fixture、ID reuse/float sort fixture、50k logical/2k visible chunk fixture、GPU 不要 | K3、U1（コード流用でなく atlas/U1 contract の抽出元） |
| **S2D-0b GPU world quad** | world-space vertex/instance buffer、camera VP、pivot/flip、scene pass、straight alpha、depth test ON/write OFF、atlas page bind | 2D/3D 遮蔽、layer/z、複数 atlas、rotation/parent、UI 無し/有り resource ownership golden | S2D-0a、WP48 |
| **S2D-1 Pixel/Policy** | strict pixel-perfect compositor または render quantization、ppu/zoom、y_down/declaration、billboard、flipbook dogfood | 2D-R4/R5 fixture と pixel golden 全通過 | S2D-0b |
| **S2D-P Query minimum** | sweep/shape cast、filter/all-hit、MTD、stable collider identity | 2D-R6 の純 CPU fixture | P1/P2 |
| **S2D-2 Vertical slice** | side-scroller 1 面を固定選択し、入力/event/fixed-step/ground/slope/one-way を実証。top-down に逃げて platformer 条件を未検証にしない | replay 二回一致、接地/斜面/one-way/tunneling scenario | S2D-1、S2D-P |
| **将来** | TileMap format/chunk、9-slice、snapshot texture、3D transparent 統合 | 各別設計 | — |

## 8. 再受理条件

`design_2d_game_layer.md` v2 の再レビュー条件は次の六点である。

1. 単一 world 決定を「単一 ownership/identity」に限定し、2D consumer/acceleration/compositor を禁止しない（2D-R1）。
2. SpriteView を scene v1 の `name` dispatch と asset reference policy に合わせる（2D-R2）。
3. U1 との共有境界を asset/resource/helper に狭め、world ABI/pass/sort を分離する（2D-R3）。
4. ppu、ortho magnification、viewport、zoom、pixel center を結んだ strict pixel-perfect 契約を追加する（2D-R4）。
5. declaration sequence、finite float total key、EntityId reuse、depth 制限を閉じる（2D-R5）。
6. platformer の前に sweep/filter/MTD の query 最小面を置き、S2D-0〜2 を §7 の粒度へ分割する（2D-R6）。

中核の「2D と 3D を同じ world で扱う」方針そのものを撤回する必要はない。Reject の対象は、その方針から現在の ABI・pixel perfect・physics・WP 粒度が自動的に導けるとしている部分である。
