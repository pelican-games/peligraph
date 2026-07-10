# 2D/UI 基盤設計 v2 敵対的再レビュー

## 判定: **Reject**

対象: `docs/design_ui_2d_foundation.md` v2 ドラフト (2026-07-10)

v2 は v1 より大幅に前進した。世代付き `WidgetId`、capture/cancel、2 レーン、
Controller の所有権、hot reload transaction、pass 順、purge、ImGui 隔離、純 CPU の
U0 という大枠は採用可能である。しかし C1、C3、C7、C8、C11 はまだ実装契約まで
閉じておらず、v2 自身が新しく作った ABI 矛盾とフレーム位相矛盾もある。

したがって現版のまま U0/U1 の着手は認めない。以下の R1〜R6 を本文または
入力/交換形式側の「仕様の正」に規範として反映し、fixture 形式まで固定した再版を
再審査対象とする。

## 1. Blocker findings

### R1 [C1/C2/C11] `QuadVertex(24B)` は記載フィールドと一致せず、shader ABI も閉じていない

`float2 + float2 + u8x4` は 8 + 8 + 4 = **20B** である。v2 は `24B, align 4`
と書く一方、末尾 4B の pad、各 field offset、vertex stride を書いていない
(`docs/design_ui_2d_foundation.md:28-39`)。実装者が 20B stride と 24B stride に
分岐できるため、C1 の CPU/GPU ABI 固定条件を満たさない。

同じ箇所には次も残る。

- index type と index buffer ABI がない。既定 16384 quad は 65536 vertex、最大 index
  65535 なので `uint16` でも境界上は成立するが、`first_index/index_count` の型と
  overflow 規則を含めて明記しなければならない
  (`docs/design_ui_2d_foundation.md:40-45`)。
- `DrawRun` は `pipeline` を持つのに、併合条件は texture/sampler/clip しか要求しない。
  異なる pipeline/kind を同じ run に入れないことを規範化する必要がある
  (`docs/design_ui_2d_foundation.md:40-41`, `docs/design_ui_2d_foundation.md:53-57`)。
- straight alpha の RGB factor は書かれたが、alpha attachment の
  `srcAlpha/dstAlpha/op` は「固定する」と言うだけで値がない。頂点 `color.rgb` が
  linear 値か sRGB authored 値か、出力 attachment format/transfer もない
  (`docs/design_ui_2d_foundation.md:62-68`)。現行 debug_text は alpha に
  `one / one-minus-src-alpha`、旧 UI は `zero / one` を使っており、まさに統合対象の
  差である (`src/core/renderer/debugtext.cpp:343-353`,
  `src/core/renderer/uirenderer.cpp:25-38`)。

修正条件は、20B を採るなら offsets=`0/8/16`, stride=`20`、24B を採るなら
offset 20 の明示 pad を置き、`sizeof/alignof/offsetof` static assert と shader
reflection fixture をゲートにすること。併せて index type、全 blend factor/op、
vertex color の transfer function、attachment format を shader ABI に列挙すること。

既定 16384 quad / 256 clip 自体は v1 の初期容量として不自然ではない。問題は容量値
ではなく ABI と数え方である。clip 上限は「重複 intersection を canonicalize した後の
unique clip 数」か「push 回数」か、超過エラーに document key / stable widget id / 実数 /
上限を含めることも固定する必要がある (`docs/design_ui_2d_foundation.md:44-45`)。

### R2 [C3/C5] UI 消費、Actions、次フレーム semantic event を同時に守る frame phase がない

v2 は UI routing を `dispatchPendingEvents` の後へ置き、UI 消費マスクを Actions 評価前に
確定するとする (`docs/design_ui_2d_foundation.md:87-95`)。しかし現行
`dispatchPendingEvents` は game system の event handler を `GameContext` 付きで呼び、
handler は Actions を読める (`src/core/userpublic/details/event/registerer.cpp:103-107`,
`src/core/userpublic/gamecontext.cpp:22-46`)。Actions は query ごとに現在の snapshot から
評価されるため (`src/core/userpublic/userinput.cpp:48-53`)、UI routing 前の semantic
handler が未マスク入力を観測できる。

逆に UI routing を `dispatchPendingEvents` より前へ動かすだけでは、現行 event bus は
dispatch 時に live queue を swap/drain するため、その UI が emit した semantic event まで
同フレーム配送され、2 レーン規約を破る
(`src/core/userpublic/details/event/registerer.cpp:88-106`)。

必要なのは順序の文章だけでなく、frame 境界で queue を分離する次の phase 契約である。

1. frame 開始時に E1 の `pending` を immutable な `deliver_now` へ swap する。
2. ordered input を freeze し、ImGui → pelican.ui の順で routing する。
3. UI consumption を適用した Actions frame を一度だけ確定する。
4. `deliver_now` を game handlers へ配送する。以後の emit は常に `pending_next` へ入る。
5. ECS / game systems を更新する。

さらに「外部マスク API」だけでは同一フレームの `UI click → world click` のような混在時に
raw MouseLeft 全体を隠すのか、未消費 event から Actions snapshot を再構成するのかが不明で
ある (`docs/design_ui_2d_foundation.md:91-92`)。frame 単位の coarse mask を採るならその制限を
規範化し、event 単位を採るなら masked snapshot の構成規則を定義すること。

前提 WP の受け入れ範囲には最低限、(a) `event_seq` の型・採番点・寿命、(b) immutable
span の寿命、(c) UI consumption の event/control 単位、(d) Actions の一回評価と外部 mask、
(e) GLFW/rpc/replay の同一 `InputEvent` 化、(f) 通常 loop と rpc loop の phase 同一性、
(g) `pelican.input_seq` schema/version 改訂を含める必要がある。現在の入力設計の正はなお
「L1 snapshot 列」を記録単位としており (`docs/design_input_actions.md:89-94`)、v2 の
「I 系 WP に切り出す」だけでは C3 の受け入れ条件になっていない
(`docs/design_ui_2d_foundation.md:253-254`)。

### R3 [C4/C7/C8] レイアウト語彙は増えたが、単位・解法・PSD 写像が未定義

`position` は ViewportTransform 適用後の framebuffer px、drag delta は virtual units、
レイアウトは整数 px とそれぞれ書かれている
(`docs/design_ui_2d_foundation.md:28-31`, `docs/design_ui_2d_foundation.md:97-104`,
`docs/design_ui_2d_foundation.md:117-120`, `docs/design_ui_2d_foundation.md:150-154`)。
これは `position=float2 framebuffer px` という選択自体の矛盾ではない。virtual layout を
一度だけ framebuffer edge へ変換するなら成立する。しかし v2 は、どの段階で整数化するか、
原点/Y 向き、rect が edge か pixel center か、free scale 時の min/max edge の丸め、
hit が丸め前/後のどちらを見るかを決めていない。現行 debug_text も pixel→clip 変換を
独自に持つため (`src/core/renderer/debugtext.cpp:141-155`)、exact golden にはこの規約が必要である。

また、`fixed|content|fill`、intrinsic、min/max、stack、anchor の名前は揃ったが、次の
制約解法がない (`docs/design_ui_2d_foundation.md:138-155`)。

- `content` 親の中の `fill` 子、または content 親に対する stretch anchor という循環を
  error にするか、どちらを先に解くか。
- fixed/content/min-max を差し引いた残りを fill/weight へ配る順序、min/max clamp 後の
  再配分、残りが負のときの overflow。
- panel intrinsic の「content」が non-stack の複数 absolute child に対して union bounds
  なのか、anchor child を intrinsic 計算から除外するのか。
- `anchor_min/max` の値域・座標原点と offsets の `[left,top,right,bottom]` か
  `[x,y,w,h]` か。

「親が stack なら子 anchor は error」は layout owner を一つにする規則として妥当であり、
PSD と必然的には衝突しない。PSD group を non-stack panel に写し、各 child を absolute
anchor/offset にすればよい。しかしその写像が v2 にない。資産設計が PSD から保証するのは
名前・位置・サイズ・不透明度・表示・group までで
(`docs/design_asset_containers.md:63-76`)、normal blend 以外も v1 対象外である
(`docs/design_asset_containers.md:126-134`)。v2 は「supported subset に依存」と
「未対応はエラー」と述べるだけで、subset と変換表を列挙していない
(`docs/design_ui_2d_foundation.md:20-22`, `docs/design_ui_2d_foundation.md:251-251`)。
したがって C8 は依存訂正だけ解消、PSD subset は未解消である。

修正条件は、layout の canonical space と丸め段を一つに決め、上記 constraint algorithm と
cycle error を擬似コードまたは規範表で固定すること。PSD は少なくとも
`group→non-stack panel`, `position/size→absolute offsets`, `opacity`, `visibility`,
`normal blend` の写像と、mask/effect/PSD text/font/non-normal blend の
error または明示 flatten 方針を表にすること。

### R4 [C5/C6] UI-local command の commit 境界と権限、semantic payload の identity が閉じていない

UI-local lane に `UiCommandBuffer 経由` と書き、callback 中の直接 tree mutation を禁じる
点は整合している (`docs/design_ui_2d_foundation.md:119-127`)。ただし「event dispatch 後」が
1 event 後か frame の ordered event 列全体の後か不明である。同一フレームで最初の click が
subtree を変えた場合、二番目の click が旧 tree と新 tree のどちらを hit するかが決まらない。
各 event 後に commit + dirty layout を再計算するか、frame 中は snapshot を凍結して render 前に
一括 commit するかを選び、semantic trace に固定する必要がある。

また v1 条件は UI-local callback が GameContext/ECS を直接変更しないことだったが、v2 は
Controller に `GameObjectId` を保持して使用時 resolve する道を認める
(`docs/design_ui_2d_foundation.md:174-181`)。pointer callback から resolve した ECS を直接
変更できるなら semantic lane を迂回できる。`onPointer` は UI command と semantic sink のみ、
ECS は read-only snapshot まで、gameplay mutation は次フレーム semantic handler のみ、と
権限を明記すべきである。

payload も「widget id を schema で写像」としかなく、具体形式がない
(`docs/design_ui_2d_foundation.md:134-136`)。semantic event は次フレーム配送なので、runtime
`WidgetId{index,generation}` は reload/remove 後に stale になり得る。payload には
`(document key, stable id)` を値としてコピーし、runtime handle は UI 内部だけに留めること。
event 名、payload field path、source (`static|stable_id|drag_delta`)、型、required/default、
単位、範囲を UI document schema として固定し、ロード時 valid/invalid fixture を置く必要がある。

hot reload の cancel callback が積んだ旧 document 向け command を commit するか破棄するか、
その status を誰が受け取るかも swap 規約に追加すること
(`docs/design_ui_2d_foundation.md:180-185`)。

### R5 [C11] semantic fixture は項目一覧であり、交換形式ではない

v2 は widgets、draw runs、input routing、reload lifecycle の四列を挙げた
(`docs/design_ui_2d_foundation.md:202-223`)。これは観測点として正しいが、U0 の複数実装が
同じ期待値を生成できる JSON 契約ではない。少なくとも次を固定する必要がある。

- `schema/version`、document key と revision の表現。
- rect/clip/scissor の単位、原点、edge 順、integer/float と canonical rounding。
- widget の比較 identity。cross-reload trace は stable id を正とし、生 `WidgetId` は arena
  単体 fixture に分離する。
- `event_seq` の JSON 表現、pointer id/button/position、consumed control と副作用の enum。
- array の規範順、optional field の省略/null 規則、error trace の形式。
- pipeline/texture を実行時数値 ID でなく安定名で保存する規則。

最小形は例えば次である。

```json
{
  "schema": "pelican.ui_semantic_fixture",
  "version": 1,
  "viewport": {
    "framebuffer_px": [1280, 720],
    "virtual_ui_units": [320, 180],
    "content_rect_px": [0, 0, 1280, 720]
  },
  "document": {"key": "hud", "revision": "sha256:..."},
  "widgets": [
    {"id": "root/play", "type": "button", "rect_px": [40, 24, 120, 32],
     "clip_px": [0, 0, 1280, 720], "layer": 0, "decl_seq": 3}
  ],
  "draw_runs": [
    {"pipeline": "rgba_straight", "sampler": "nearest",
     "texture": "atlas:ui/page:0", "scissor_px": [0, 0, 1280, 720],
     "first_index": 0, "index_count": 6}
  ],
  "input_trace": [
    {"event_seq": "17", "kind": "pointer_down", "pointer_id": 0,
     "button": "left", "position_ui": [20, 10], "target": "root/play",
     "consumed": ["mouse:left"], "effects": ["capture"]}
  ],
  "lifecycle": []
}
```

これは例であり、本文の canonical space 選択に合わせて `rect_px` 等は調整してよい。
重要なのは JSON key の存在より、安定 identity・単位・順序・丸めを schema v1 として固定する
ことである。

### R6 [C1/C11] debug_text exact golden は達成可能だが、現記述だけでは保証できない

現在の debug_text は整数 pixel から頂点を作り、nearest/clamp sampler を使い、alpha は
`one / one-minus-src-alpha` で合成している
(`src/core/renderer/debugtext.cpp:41-51`, `src/core/renderer/debugtext.cpp:292-333`,
`src/core/renderer/debugtext.cpp:343-353`)。CPU glyph layout と同じ atlas を再利用し、同じ
pixel edge、UV、triangle diagonal、sampler、blend、attachment format を再現すれば、
共通 QuadCommand 化後も tolerance 0 は達成可能である。従って
`test/golden/debug_text_feature/tolerance.json:1` を安易に緩めるべきではない。

必要なのは、R1/R3 の ABI と座標規約を確定し、(a) 旧 path、(b) 共通 path を同じ fixture に
描き、統合 commit で byte-exact に比較する compatibility gate である。意図的に alpha/color
space の意味を変える場合だけ、変更理由を記録した一回限りの versioned baseline 更新を行い、
更新後は再び tolerance 0 とする。非ゼロ tolerance で ABI 差を吸収してはならない
(`docs/design_ui_2d_foundation.md:69-71`, `docs/design_ui_2d_foundation.md:248-248`)。

## 2. C1〜C12 照合

| 条件 | 判定 | 照合結果 |
|---|---|---|
| C1 | **未解消** | command 語彙は追加されたが、24B/20B 矛盾、index ABI、alpha factor、color transfer が未定義(R1)。 |
| C2 | **部分解消** | 1 page/run、隣接 run、nested clip、0/1/0 gate は妥当。ただし pipeline を併合 key に明記し index ABI を閉じる必要がある(R1)。 |
| C3 | **未解消** | ordered `FrameInput` の方向は正しいが、E1/Actions の frame phase、mask 意味論、rpc/replay、input_seq の正本改訂と WP 受け入れ範囲が不足(R2)。 |
| C4 | **部分解消** | 世代付き capture/cancel state machine は解消。ViewportTransform の canonical space と丸め段が未解消(R3)。 |
| C5 | **部分解消** | 2 レーンと `emitImmediate` 禁止は解消。commit 境界、ECS mutation 禁止、payload schema/stable identity が不足(R2/R4)。 |
| C6 | **概ね解消** | registry/factory、UiModule 所有、逆順破棄、side-build/swap、state key は妥当。cancel 中 command の扱いだけ追記が必要(R4)。 |
| C7 | **未解消** | 必要語彙は入ったが、constraint algorithm、循環、anchor 形式、単位/丸めが実装可能な規範になっていない(R3)。 |
| C8 | **未解消** | K3 依存訂正は解消。PSD supported subset と `pelican.layout→pelican.ui` 写像がない(R3)。 |
| C9 | **解消** | 予約 pass 順、plan fixture、purgeable feature、非起動 test が規定された (`docs/design_ui_2d_foundation.md:189-200`)。 |
| C10 | **条件付き解消** | build/runtime/input/headless 隔離は十分。v1 で要求した device loss の扱いだけ導入 WP から落ちている (`docs/design_ui_2d_foundation.md:224-240`)。 |
| C11 | **未解消** | 非決定性対策表はよいが、semantic fixture schema と exact golden を成立させる ABI/座標契約が不足(R1/R3/R5/R6)。 |
| C12 | **解消** | 外部 atomic migration、strict runtime、失敗時旧版維持、in-flight 遅延破棄が規定された (`docs/design_ui_2d_foundation.md:198-200`)。 |

## 3. 依頼で挙げられた六点への回答

1. **24B/上限/position**: 既定上限は初期値として妥当。24B は field 合計と矛盾して
   不可。framebuffer-px position は transform を一度だけ適用するなら妥当だが、layout の
   canonical space と snap 段を固定するまで ViewportTransform 一元化とは言えない。
2. **stack と PSD**: stack child の anchor error は妥当。PSD child を non-stack panel の
   absolute offsets へ写すなら衝突しないが、その変換表と supported subset がないため現版では
   未解消。
3. **FrameInput WP**: 不十分。rpc inject、event_seq、span lifetime、Actions mask、E1 queue
   swap、input_seq schema、通常/rpc/replay 同値 test まで受け入れ条件に入れる必要がある。
4. **UI-local と command queue**: `UiCommandBuffer 経由`なので表面上は整合する。ただし
   commit が event ごとか frame ごとか、pointer callback の ECS mutation 禁止が未定義。
5. **debug_text exact golden**: 達成可能であり tolerance は緩めない。意図的な色意味論変更時
   のみ baseline を versioned 更新し、その後は 0 に戻す。
6. **semantic fixture**: 観測項目は正しいが JSON 形式として不足。R5 の header、安定 ID、
   単位、順序、丸め、trace enum まで固定して初めて U0 の gate になる。

## 4. 再審査の必須条件

1. R1 の vertex/index/blend/color ABI を offset と型まで固定し、static assert + reflection
   fixture を定義する。
2. R2 の frame phase と E1 queue swap、consumption semantics、I 系 WP の受け入れ test を
   本文と `design_input_actions.md` の正本へ反映する。
3. R3 の layout algorithm/cycle error/canonical space/pixel rounding と PSD 変換表を固定する。
4. R4 の command commit 境界、Controller 権限、semantic payload schema/stable identity を
   固定する。
5. R5 の `pelican.ui_semantic_fixture` schema v1 と valid/invalid/expected fixture を定義する。
6. R6 の旧/new debug_text byte-exact compatibility gate を U2 の受け入れ条件に追加する。

この六点が入れば、C1〜C12 の残件は機械検証可能になり、次版は少なくとも
**条件付き Accept** の審査対象になる。
