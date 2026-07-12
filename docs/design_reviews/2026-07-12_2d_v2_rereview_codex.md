# 2D ゲーム層 v2 再レビュー

対象: `docs/design_2d_game_layer.md` v2  
照合元: `docs/design_reviews/2026-07-12_hr_v2_2d_v1_review_codex.md` §4-8  
レビュー日: 2026-07-12

## 0. 結論

**判定: 条件付き**。

前回の Reject の原因だった 2D-R1〜R6 は、いずれも方向としては反映された。単一 world の意味は ownership/identity に限定され、scene v1 の `name` dispatch と asset ID 間接参照、U1 から分離した world sprite ABI、strict pixel-perfect の二方式、有限値を前提とする sort key、S2D-P と WP 再分割が本文に入っている (`docs/design_2d_game_layer.md:23`, `docs/design_2d_game_layer.md:63`, `docs/design_2d_game_layer.md:98`, `docs/design_2d_game_layer.md:131`, `docs/design_2d_game_layer.md:192`, `docs/design_2d_game_layer.md:221`)。従って中核方針を再び Reject する理由はない。

ただし、そのまま実装へ渡すと契約が二通り以上に分かれる箇所が五つ残る。いずれもアーキテクチャの撤回を要さず、S2D-0a / S2D-1 / S2D-P の exit gate に追加できるため、下記 C1〜C5 を受理条件とする。

## 1. 前回 6 条件の再照合

| 条件 | 判定 | 再照合 |
|---|---|---|
| 2D-R1 | 充足 | 禁止対象を別 world・別 ID・同期 bridge に限定し、render extraction、culling、tile chunk、compositor、query helper を同一 world の派生 consumer として明示的に許可した (`docs/design_2d_game_layer.md:25`, `docs/design_2d_game_layer.md:28`, `docs/design_2d_game_layer.md:30`)。前回指摘の偽の二択は解消している。 |
| 2D-R2 | 条件付き充足 | component 例は `name` dispatch となり、texture は asset declaration ID + fragment になった (`docs/design_2d_game_layer.md:65`, `docs/design_2d_game_layer.md:67`, `docs/design_2d_game_layer.md:70`)。実 loader も `component.at("name")` で二段階に dispatch する (`src/core/loader/scene.cpp:107`, `src/core/loader/scene.cpp:117`)。scene 正本の「params に file path を直接書かず asset_data_json の id を経由する」とも一致する (`docs/design_scene_format.md:100`, `docs/design_scene_format.md:103`)。ただし sampler の所有元が schema/command のどちらにも閉じていないため C1 が必要。 |
| 2D-R3 | 充足 | 共有を AtlasAsset、page identity、sampler/alpha/color 規約、CPU helper に絞り、UI と sprite の command/vertex ABI、pipeline、pass、depth、sort、座標変換を分離した (`docs/design_2d_game_layer.md:42`, `docs/design_2d_game_layer.md:52`)。現実装で UI page upload が `UiModule::pages()` に直結する事実 (`src/core/renderer/uicontainer.cpp:104`, `src/core/renderer/uicontainer.cpp:119`)も認識し、consumer-neutral resource への抽出と片側 consumer の非初期化を要求している (`docs/design_2d_game_layer.md:44`, `docs/design_2d_game_layer.md:46`)。U1 の 16384 を sprite の frame 上限へ転用しない点も明記された (`docs/design_2d_game_layer.md:57`, `src/core/ui/drawcommands.hpp:15`)。 |
| 2D-R4 | 条件付き充足 | `ppu`、projection/viewport 由来の world-units-per-pixel、`zoom` を分離し、logical target と render-only quantization の二方式、target pixel に基づく camera snap を置いた (`docs/design_2d_game_layer.md:105`, `docs/design_2d_game_layer.md:112`, `docs/design_2d_game_layer.md:120`)。camera 実装が `[-xmag,+xmag]×[-ymag,+ymag]` を使う事実とも整合する (`src/core/renderer/camera.cpp:424`)。ただし三量を結ぶ式と strict 対象集合が未確定なので C4 が必要。 |
| 2D-R5 | 条件付き充足 | monotonic `declaration_seq:uint64`、full `(index,generation)` tie-break、finite 必須、canonical float total key、depth 合成の限界を追加した (`docs/design_2d_game_layer.md:133`, `docs/design_2d_game_layer.md:137`, `docs/design_2d_game_layer.md:141`, `docs/design_2d_game_layer.md:143`, `docs/design_2d_game_layer.md:155`)。通常の一 entity 一 sprite について前回条件は満たす。ただし §5 の複数 command render source まで含めると EntityId だけでは全順序にならないため C2 が必要。 |
| 2D-R6 | 条件付き充足 | sweep/shapeCast、filter/all-hit、MTD、安定 identity を S2D-P として新設し、side-scroller の前提依存にした (`docs/design_2d_game_layer.md:197`, `docs/design_2d_game_layer.md:199`, `docs/design_2d_game_layer.md:202`, `docs/design_2d_game_layer.md:205`, `docs/design_2d_game_layer.md:228`)。S2D-0a/0b/1/P/2 の再分割も前回案どおりである (`docs/design_2d_game_layer.md:223`)。public query 面と決定的な同時 hit 順序を C5 で閉じれば受理できる。 |

## 2. 受理条件

### C1 — S2D-0a: sampler の所有元と command key を一意にする

本文は共有規約に `sampler key(nearest/linear)` を含める一方 (`docs/design_2d_game_layer.md:48`)、`sprite_view` 交換形式には sampler がなく (`docs/design_2d_game_layer.md:70`)、列挙された `SpriteCommand` の field にも sampler がない (`docs/design_2d_game_layer.md:53`)。現 U1 では sampler は draw key の一部であり、nearest と linear は別 descriptor である (`src/core/ui/drawcommands.hpp:31`, `src/core/ui/drawcommands.hpp:38`, `src/core/renderer/uicontainer.cpp:149`, `src/core/renderer/uicontainer.cpp:164`)。暗黙に atlas、strict mode、renderer default のいずれかから選ぶ実装では、同じ scene の結果と batch 分割が実装者依存になる。

S2D-0a に次を追加すること。

1. sampler の正本を `sprite_view`、asset declaration、または project/camera policy のどこに置くか一つに決め、override の有無と既定値を固定する。
2. `SpriteCommand` またはその参照する immutable material/page key に sampler を保持し、run/batch key の一部にする。
3. `nearest | linear` 以外、未知 field、asset default と override の組合せを schema fixture に入れる。strict pixel-perfect が nearest を強制するのか、非 nearest を error にするのかも S2D-1 から同じ key を使って判定する。

### C2 — S2D-0a: render source の複数 command を含む全順序を閉じる

最終 tie-break は full EntityId までである (`docs/design_2d_game_layer.md:141`)。一方、escape hatch は一つの entity が TileMap/particle/foliage の command/instance 列を供給でき、ABI を「1 command = 1 entity」に固定しない (`docs/design_2d_game_layer.md:169`, `docs/design_2d_game_layer.md:178`)。同じ source entity から同 layer・同 z/y の command が複数出れば、現在の key は完全同値となり、source 内コンテナの走査順が最終 bytes を決める。これは §3 の「全順序」と §8 の command byte 一致に反する (`docs/design_2d_game_layer.md:143`, `docs/design_2d_game_layer.md:149`, `docs/design_2d_game_layer.md:216`)。

S2D-0a で EntityId の後ろに **source-local stable ordinal**（または同等の stable primitive/command identity）を置き、発行・cache 再利用・削除再追加・replay 時の規則を定義すること。50k fixture は「一 entity から複数 command」「同 layer・同 depth」「source の内部格納順を変えても規範 ordinal が同じなら同一 bytes」を含める。

### C3 — S2D-0a: 16384 境界を実際に跨ぐ fixture を追加する

設計は visibility 後の列を 16384 以下の draw chunk に分割するとする (`docs/design_2d_game_layer.md:172`)。しかし指定された 50,000 論理 sprite fixture は可視約 2,000 であり (`docs/design_2d_game_layer.md:175`)、一 chunk に収まるので分割ロジックも「一 frame 最大 16384 ではない」ことも検証できない。S2D-0a の exit gate がこのケースを `chunk fixture` と呼ぶだけでは不足する (`docs/design_2d_game_layer.md:225`)。

既存の 50k/2k culling ケースは維持し、別ケースとして可視数 `16384`、`16385`、および二 chunk を超える値を入れること。各 chunk の vertex/index 型上限、連結した command 順/hash、境界前後の欠落・重複なしを検査する。これにより U1 の `maxQuads = 16384` (`src/core/ui/drawcommands.hpp:15`) を誤って frame hard limit として再利用する回帰を検出する。

### C4 — S2D-1: strict pixel-perfect を式と対象集合で閉じる

三概念の分離は正しいが、`zoom = framebuffer_pixels_per_source_pixel` と定義しただけで (`docs/design_2d_game_layer.md:105`)、`ppu`、projection、content viewport/letterbox を結ぶ成立式がない。例えば既定 size・単位 scale の texel については、少なくとも各軸で

`zoom_axis = (1 / ppu) / world_units_per_framebuffer_pixel_axis`

という関係（logical target 方式では target→framebuffer の整数倍率を含む同値な式）が必要である。現 orthographic 実装では x 軸の world 幅が `2*xmag`、y 軸が `2*ymag` なので (`src/core/renderer/camera.cpp:424`)、どの extent を content viewport として式へ入れるかを決めなければ、方式 (a) の「1 source texel = 1 target pixel」も方式 (b) の量子化も判定できない。

S2D-1 の方式選定時に次を exit gate とすること。

1. x/y ごとの成立式、整数 zoom の判定許容差、pixel center の原点、odd viewport/letterbox の content rect を規範化する。
2. strict 対象を、size 省略・単位 scale の sprite だけにするのか、明示 size、整数 scale、fractional/non-uniform scale、rotation、billboard まで含めるのか表で固定する。対象外は silent degradation ではなく status/error で観測可能にする。
3. camera snap と render-only vertex/pivot quantization の適用順を一つにし、二重丸めを禁止する。物理 Transform を変更しないという現契約 (`docs/design_2d_game_layer.md:112`) は維持する。
4. §2 の golden (`docs/design_2d_game_layer.md:127`) に、式を一項だけ破る xmag/ymag・viewport・ppu の negative fixture を加える。

### C5 — S2D-P: sweep/filter/MTD の public 結果と同順位規則を固定する

現 pure API の `Shape` は geometry/pose を内包するが、query world が返せるのは closest ray と overlap ID 列だけである (`src/core/phys/physquery.hpp:20`, `src/core/phys/physquery.hpp:38`, `src/core/phys/physquery.hpp:74`)。`PhysWorld` 側も同じ二 API しか公開していない (`src/core/phys/physworld.hpp:48`)。v2 の `shapeCast/sweep(start, delta, filter)` は必要能力を正しく列挙しているが (`docs/design_2d_game_layer.md:199`)、moving shape の寸法/姿勢、all-hit の順序、同 TOI、initial overlap、filter 継続後の identity がまだ public contract になっていない。fixture に「同時 hit tie」を置くだけでは期待値を一意に書けない (`docs/design_2d_game_layer.md:210`)。

S2D-P で次を header/fixture の同じ WP に入れること。

1. 入力を少なくとも moving `Shape`（寸法・初期 pose）、`delta`、filter とし、zero delta/non-finite/initial overlap の扱いを定義する。
2. hit に TOI、position、normal、stable collider identity と、ECS entity が存在する場合の full identity を保持する。scene object 名だけ、再利用可能な index だけ、のどちらにも退化させない。
3. all-hit の規範順を `(canonical TOI, stable collider identity, shape/subshape ordinal)` 等の total key で固定し、同 TOI と MTD の非一意軸の tie-break を定義する。closest と filter 継続はこの同じ ordered result から導く。
4. layer/mask、self/ignore、trigger、one-way metadata の格納先と既定値を collider/public query schema に追加し、未知 bit・stale identity・ignore 後の次 hit を negative/positive fixture にする。

## 3. v2 で追加された記述の確認

### 3.1 escape hatch と 50k fixture

一 entity が複数 sprite を供給し、cull 後に sort/chunk する方針は、単一 world と大量 sprite を両立させる正しい逃げ道である (`docs/design_2d_game_layer.md:163`, `docs/design_2d_game_layer.md:169`, `docs/design_2d_game_layer.md:172`)。実装不能な要求ではない。新たに生じた穴は、複数 command の final tie と、可視 2k fixture が chunk 境界を踏まないことだけであり、C2/C3 で局所的に閉じられる。

### 3.2 `snapshot:` 予約

`snapshot:` は v1 対象外かつ実装順でも「将来」に隔離されているため、今回の S2D-0a〜2 の blocker ではない (`docs/design_2d_game_layer.md:189`, `docs/design_2d_game_layer.md:230`)。ただし現 PathResolver が scheme として認識する構文は `://` を含むものだけで (`src/core/loader/pathresolver.cpp:37`)、未知 scheme は reject する (`src/core/loader/pathresolver.cpp:527`)。従ってこの一行を、現在の PathResolver が `snapshot:` を解決できるという仕様として読んではならない。

将来 WP では `snapshot:` を asset/path reference とは別の typed runtime resource ref にするか、共通 resolver の variant と正規構文を正式に拡張すること。S2D-0a〜2 では parser や `SpriteView.texture` に受理させず、予約語のままにする限り矛盾はない。

## 4. 最終判断

v2 は前回 Reject の中核を骨抜きにせず修正している。特に、単一 world を「専用機構禁止」と同一視しないこと、U1 quad ABI を world へ流用しないこと、camera snap 単独案と既存 query 十分論を撤回したことは明確である (`docs/design_2d_game_layer.md:28`, `docs/design_2d_game_layer.md:37`, `docs/design_2d_game_layer.md:100`, `docs/design_2d_game_layer.md:194`)。

C1〜C3 を **S2D-0a**、C4 を **S2D-1**、C5 を **S2D-P** の exit gate に追加する条件で受理する。これらを付けずに着手すると sampler/batch、command order/chunk、pixel contract、query hit order がそれぞれ実装者依存になるため、無条件 Accept にはしない。一方、いずれも現在の world ownership、render pass 分離、WP 依存順を変更しない局所条件なので Reject には戻さない。
