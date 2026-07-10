# 2D/UI 基盤設計 v1 敵対的レビュー

## 判定: **Reject**

対象: `docs/design_ui_2d_foundation.md` v1 ドラフト (2026-07-10)

三層分離、ペインター順の決定性、ゲーム UI とツール UI の分離という方向は妥当である。しかし本文は、実装を始めるための契約ではなく、互いに両立しない希望の列挙に留まっている。特に次の四点は軽微な未決事項ではない。

1. 「1 頂点ストリーム」を「1 バッチ」と取り違えており、UV、テクスチャ選択、クリップ、サンプラ、描画種別、アルファ規約が未定義である (`docs/design_ui_2d_foundation.md:20-31`, `docs/design_ui_2d_foundation.md:90-94`)。
2. 「入力スナップショットから UI が最初に消費」は現行 L1 の情報量では実装不能である。L1 は同一フレーム内の順序と各イベント時点の座標を捨てる (`src/core/os/inputstate.hpp:16-23`, `src/core/os/inputstate.cpp:102-144`)。
3. `PELICAN_REGISTER_WIDGET` は live Controller の所有者、破棄順、hot reload、capture 消滅時処理を定めず、ECS v2.1 が閉じた生ポインタ穴を UI で開け直す (`docs/design_ui_2d_foundation.md:80-94`, `docs/design_ecs_lifecycle.md:206-214`)。
4. 「アンカーだけで大半の HUD は足りる」としながら、同じ v1 で stack、可変テキスト、ツールウィジェット、PSD 変換を約束している。これらを配置する最小サイズ・内在サイズ・余白・伸縮・overflow の規則がない (`docs/design_ui_2d_foundation.md:35-57`, `docs/design_ui_2d_foundation.md:69-86`, `docs/design_ui_2d_foundation.md:108-117`)。

したがって U1〜U3 の着手を認めない。以下の C1〜C12 を本文に規範として取り込み、純ロジック試験を先に成立させた再版なら「条件付き Accept」に上げられる。

## 1. クアッドバッチャ: per-vertex color 追加では設計にならない

### 1.1 必須データが欠けている

最低限、GPU が読む primitive 契約には次が必要である。

| データ | 推奨する置き場所 | 理由 |
|---|---|---|
| position | vertex | スクリーン/ワールド変換後の位置 |
| UV | vertex | アトラスの部分矩形、trim/rotate 済み sprite |
| color | vertex (`RGBA8_UNORM` 等、形式を固定) | tint と頂点グラデーション |
| texture/page index | quad ごとの値を vertex に複製、または primitive table | 複数アトラスを一 draw に含める場合に必要 |
| clip id | quad ごとの値、または draw-run の scissor | 親子 clip の交差。矩形そのものを全頂点へ複製する必要はない |
| material/sampler/flags | quad または draw-run | solid、RGBA sprite、bitmap glyph、将来 SDF、nearest/linear の分離 |

本文が確定したのは color だけであり (`docs/design_ui_2d_foundation.md:90-94`)、肝心の position/UV さえ形式として規定していない。「単色矩形をテクスチャ付きクアッドへ正規化」するなら、白 1 texel の予約ページを使うのか、`flags` で非サンプリング分岐するのかも必要である。

複数テクスチャを同一 draw で読むなら descriptor array と non-uniform indexing の feature/limit を明示して有効化しなければならない。Vulkan の runtime descriptor array と non-uniform sampled-image indexing は無条件の基礎機能ではなく、feature bit と `nonuniformEXT` の指定を要する ([Khronos: Descriptor indexing](https://docs.vulkan.org/samples/latest/samples/extensions/descriptor_indexing/README.html), [VK_EXT_descriptor_indexing](https://docs.vulkan.org/guide/latest/extensions/VK_EXT_descriptor_indexing.html))。web サブセットまで考えるなら、v1 の安全な規範は次のいずれかである。

- 固定上限 N の texture slot を 1 draw に束ね、超過時に安定分割する。
- 1 texture/page ごとに draw-run を分ける。後で対応 GPU のみ bindless path を追加する。

どちらでも、**描画順を変えず連続 run だけを併合**しなければならない。宣言順 A(texture 0), B(texture 1), C(texture 0) を A+C に並べ替えると半透明のペインター順を破壊する。つまり「クアッドバッチャ 1 本」は共有する command builder という意味にはできるが、「常に 1 draw call」にはできない。

### 1.2 既存二実装の統合には互換性がない

現行 UI は頂点バッファを持たず、`gl_VertexIndex` から全面 UV を生成し (`src/core/resources/ui.vert:16-37`)、texture ごとに descriptor set を bind して 6 頂点を draw する (`src/core/renderer/uirenderer.cpp:95-121`)。さらに入力コレクションは `std::unordered_map` であり (`src/core/renderer/uicontainer.hpp:29-47`)、その反復順がそのまま描画順になる。現行実装は既に golden の根に置けない。

一方 debug_text は `position + color + uv` の 48-byte SSBO vertex を持ち (`src/core/renderer/debugtext.hpp:17-22`)、単一フォントアトラスを単一 draw で描く (`src/core/renderer/debugtext.cpp:417-430`)。これをそのまま汎用 vertex ABI にすると、texture/clip/flags がなく、`vec4 position` と pad による帯域浪費も固定化する。「グリフ描画を移設」ではなく、CPU の glyph layout と immutable font atlas だけを再利用し、共通 `QuadCommand` へ変換すべきである。

さらに現行 UI は linear sampler (`src/core/renderer/uicontainer.cpp:28-39`)、debug_text は nearest sampler (`src/core/renderer/debugtext.cpp:41-51`) である。blend も UI は destination alpha を保持する設定 (`src/core/renderer/uirenderer.cpp:31-37`)、debug_text は source alpha を通常合成する設定 (`src/core/renderer/debugtext.cpp:343-353`) で一致しない。sampler と premultiplied/straight alpha、sRGB/linear、出力 alpha の規約を draw key に入れない統合は、テキストをぼかすか既存 golden を壊す。

### 1.3 clip は後付けできない

本文の hit test は矩形なのに、描画 clip は一度も定義されていない (`docs/design_ui_2d_foundation.md:96-106`)。panel 内の gauge、stack、長い label、将来 scroll は親矩形を越えて描画される。v1 は少なくとも次を規定すべきである。

- clip stack は traversal 中に親子矩形の整数 pixel intersection を取る。
- 同じ clip の連続 primitive は Vulkan scissor run に束ねる。
- clip 変更は painter order を越えて並べ替えない。
- 空 clip は primitive を落とす。
- shader clip を採る場合は `clip_id` と clip table の上限・精度を固定する。

## 2. 入力 capture: 現行 snapshot には必要情報がない

### 2.1 「UI が最初に消費」は現在の API では不可能

`InputSnapshot` は down/pushed/released bit、最終 mouse 座標、累積 delta しか持たない (`src/core/os/inputstate.hpp:16-30`)。`beginFrame()` は ordered event queue を走査した後に queue を消し、最終 snapshot だけを公開する (`src/core/os/inputstate.cpp:93-157`)。同一フレームに down→move→up があっても、UI が分かるのは両 edge、最終座標、合計 delta だけである。実テストも同一フレーム down/up を二つの bit に畳むことを仕様化している (`test/inputstate_test.cpp:40-50`)。

したがって次を区別できない。

- widget A 上で press、B 上へ move、B 上で release
- A 上で click 後、同一フレームに B を click
- drag 中に複数の折返点を通った経路
- press 時点と release 時点で異なる tree/layout snapshot を見るべきケース

action set の「消費」は UI の代用にもならない。現行は action 評価内部で上位 set が使った key/mouse axis を下位 set から隠すだけで (`src/core/os/actionmap.cpp:588-628`)、hit したときだけ raw MouseLeft を gameplay から隠す外部 mask API はない。

### 2.2 正しいフレーム経路

`InputState` を次の形へ改訂すべきである。

```cpp
struct FrameInput {
    std::span<const InputEvent> ordered_events; // event_seq 付き、frame 中 immutable
    InputSnapshot snapshot;                    // Actions/held state 用
};
```

通常ループでは現在 `beginFrame → dispatchPendingEvents → ECS → game systems` の順である (`src/core/appflow/loop.cpp:198-214`)。UI routing は **`dispatchPendingEvents` の後、ECS update の前**へ置く。そうすれば前フレームの UI semantic event は従来どおり frame 頭に配送され、現在フレームの UI emit は次フレームまで queue に残る。同時に UI の consumption mask は gameplay が Actions を評価する前に確定する。rpc loop も同じ順序へ揃える必要がある (`src/core/communication/rpcserver.cpp:417-425`)。

L0 の GLFW と rpc は既に同じ `InputEvent` を queue へ入れる (`src/core/os/window.cpp:174-205`, `src/core/communication/rpcserver.cpp:373-406`, `src/core/communication/rpcserver.cpp:556-562`)。ordered frame view を消さずに残せば、この長所を維持できる。

一方、`design_input_actions.md` は記録単位を L1 snapshot 列とする (`docs/design_input_actions.md:89-95`)。これは UI replay には情報不足なので、`pelican.input_seq` v1 を実装前に次のどちらかへ改訂する必要がある。

- ordered pointer events を snapshot と共に記録する。
- UI を「1 pointer edge/1 frame」へ明示的に制限する。これは drag/連打要件と両立しないため非推奨。

同じ snapshot から同じ hit 結果が出る、という主張は成立しない。**同じ ordered events + 同じ UI document revision + 同じ viewport transform** が再現単位である。

### 2.3 DPI が未決のままでは描画矩形と hit 矩形がずれる

GLFW cursor callback の座標をそのまま snapshot に入れる一方 (`src/core/os/window.cpp:201-205`)、描画 extent は framebuffer pixel で得ている (`src/core/os/window.cpp:216-223`)。GLFW は cursor を screen coordinates、framebuffer を pixels と定義し、High-DPI では 1:1 でないと明記している ([GLFW Input guide](https://www.glfw.org/docs/latest/input), [GLFW Window guide](https://www.glfw.org/docs/latest/window_guide.html))。仮想解像度/DPI を「未決」に残したまま U2 の hit test を実装してはならない (`docs/design_ui_2d_foundation.md:24-25`, `docs/design_ui_2d_foundation.md:142`)。

`ViewportTransform {window_points ↔ framebuffer_pixels ↔ virtual_ui_units}` を frame ごとに一つ作り、layout、render、hit test、rpc inject、golden が全て同じ変換を使うこと。letterbox 外の入力規則も必要である。

### 2.4 capture state machine

capture は `Controller*` ではなく世代付き `WidgetId` で持つ。最低限、次を規範化する。

1. press の hit は最前面の enabled/visible/hit-testable widget 一つへ決定し、pointer id/button ごとに capture を得る。
2. capture 中の move/up は矩形外でも owner へ送る。別 widget が capture を奪うなら旧 owner に `pointer_cancel` を先に送る。Unity も capture 中は hover 外を含む全 pointer event を owner に送り、奪取時に capture-out を通知する ([Unity: Capture the pointer](https://docs.unity3d.com/Manual/UIE-capture-the-pointer.html))。
3. owner が hide/disable/remove、document hot reload、scene unload、focus lossしたら `pointer_cancel`、pressed/hover 解除、capture 解放。release 時に別 widget へ click を誤送しない。
4. widget tree の変更要求は current event dispatch 後まで command queue に置く。event callback 中に vector/tree を直接変異させない。
5. click は「同じ owner で press、drag threshold 未超過、release 規則を満たす」と定義する。drag の delta は event 間の virtual UI unit で計算し、snapshot の frame 合計を使わない。

## 3. 1 フレーム遅延: 見た目と gameplay event を分ける

E1 の次フレーム配送自体は維持すべきである。現行実装も queue を frame 境界で swap し (`src/core/userpublic/details/event/registerer.cpp:88-107`)、reentrant emit を次回へ送ることをテストしている (`test/eventlayer_test.cpp:48-62`)。ここへ `emitImmediate` を足すと、E1 が避けた system 順依存を復活させる。

ただし本文の「UI はコールバックを持たない」と「Controller が pointer event を受ける」は矛盾している (`docs/design_ui_2d_foundation.md:85`, `docs/design_ui_2d_foundation.md:98-105`)。二つの lane を明示すべきである。

- **UI-local lane (同フレーム)**: hover/pressed/capture、slider thumb、drag preview、text caret、Controller の subtree/value command。UI module 内だけを変更し、GameContext や ECS を直接変更しない。
- **semantic lane (次フレーム)**: `MenuOpened`、設定確定、ゲームへの通知。既存 event bus に emit する。

これなら button の pressed は入力を読んだ同じ frame の render に出せ、ゲームロジックは従来の決定的境界を守れる。連打は ordered input から同順に複数 event を queue すればよい。連続 drag を gameplay が同 tick に必要とする場合は event bus ではなく、Actions と同型の frame-scoped `UiActionFrame` を gameplay update が query する。汎用即時 callback は設けない。

JSON の `"emit": {"on_click":"MenuOpened"}` も payload 契約がない。現行 by-name emit は登録型を探して JSON payload を構築する (`src/core/userpublic/details/event/registerer.cpp:57-71`)。UI load 時に event 名と payload schema を検証し、未知名を click 時まで遅延させないこと。drag delta、widget id、静的 payload の写像を schema に定義する必要がある。

## 4. `PELICAN_REGISTER_WIDGET`: live object を static 登録物にしてはならない

ECS v2.1 は `EntityId {index,generation}` を実装済みで (`src/core/userpublic/details/ecs/entity.hpp:12-25`)、フレームを跨ぐ component 生ポインタを禁止して使用時 resolve を要求する (`docs/design_ecs_lifecycle.md:166-175`, `docs/design_ecs_lifecycle.md:206-212`)。Widget Controller も同じ失効モデルにするが、widget を ECS entity にする必要はない。

推奨所有モデルは次である。

```text
WidgetTypeRegistry (process/static lifetime)
  └─ type name → metadata + factory のみ。live Controller を持たない

UiModule
  └─ UiDocumentInstance (document revision を持つ)
      ├─ WidgetArena: WidgetId {index,generation} → node
      ├─ ControllerArena: node と同じ lifetime
      ├─ Capture/Focus/Hover: WidgetId だけを保持
      └─ Render/Layout snapshot
```

- `PELICAN_REGISTER_WIDGET(T, "drag_number", schema_version)` は factory と型情報だけを登録する。重複名は hard error。
- `UiModule` が parse/validate 成功後に controller を親→子で構築し、破棄は子→親。Controller 自身や module-global static が tree を所有しない。
- Controller API は `onPointer(const UiPointerEvent&, UiControllerContext&)` のような制限面とし、`UiControllerContext` は `WidgetId` resolve、`UiCommandBuffer`、semantic event sink を提供する。node/component の参照を保存させない。
- ECS を参照する Controller は `GameObjectId` を値で保存し、使用時に `tryComponent`/公開 API で resolve する。`T*`/`T&` を member に保持しない。
- subtree mutation は handle 指定の command として dispatch 後に commit する。消滅した handle は stale no-op + status を返す。
- hot reload は新 document を side-build/validate し、frame 境界で atomic swap する。失敗時は旧 document を保持する。swap 前に旧 capture へ cancel、旧 Controller を逆順 deinit する。
- state 継承が必要なら `(document key, stable id, type, schema_version)` による明示 `saveState/restoreState` だけを許す。古い Controller object の延命は禁止する。

「ツリーの所有者は module か component か」の答えは、**live tree は UiModule、game component は必要なら `UiDocumentHandle` を値で持つだけ**である。component が tree を直接所有すると relocate/remove/hot reload と capture の寿命が三重化する。

## 5. アンカー + stack: 現在の v1 目標には不足

サンプル JSON では root 以外の gauge/label/button に size がない (`docs/design_ui_2d_foundation.md:35-50`)。stack が「等間隔配置」しても、子の希望幅、高さ、button の label padding、gauge の basis が未定義なので rect を計算できない。これは機能不足以前の未定義動作である。

比較対象から得るべき教訓は「巨大な CSS をコピーする」ではない。

- Godot 自身、anchors は基本的な multiple-resolution には有効だが複雑 UI では扱いにくく Container が必要と説明する。Container は child の手動配置を無効化して layout owner を一つにする ([Godot: Using Containers](https://docs.godotengine.org/en/stable/tutorials/ui/gui_containers.html))。
- Godot Control は minimum size、clip、focus neighbor、layout direction、mouse filter を別々の契約として持つ。anchor だけでは UI の入力・localization・accessibility は閉じない ([Godot: Control](https://docs.godotengine.org/en/stable/classes/class_control.html))。
- Unity UI Toolkit は Yoga/Flexbox を採用して responsive parent/child layout を成立させている ([Unity: UI Toolkit layouts](https://docs.unity3d.com/Manual/best-practice-guides/ui-toolkit-for-advanced-unity-developers/layouts.html))。
- RmlUi は box model と flex を持つ一方、content-based sizing は複数回 format となり得るため性能上 definite size を推奨する。完全 CSS は不要だが、padding/margin/min-max/intrinsic size を省略する根拠にはならない ([RmlUi box model](https://mikke89.github.io/RmlUiDoc/pages/rcss/box_model.html), [RmlUi flexbox](https://mikke89.github.io/RmlUiDoc/pages/rcss/flexboxes.html))。

pelican.ui v1 の最小閉包は次である。

1. 各 axis の size mode: `fixed | content | fill`。必要なら `weight` 一つだけを stack child に許す。
2. `min_size/max_size`、image/label/9patch/button の intrinsic size 算出。
3. stack の `direction/gap/padding/align/justify`。同じ axis を anchor と stack の両方が支配しない。
4. 9 方位の点 anchor ではなく、少なくとも edge anchor/inset による stretch、または `anchor_min/anchor_max + offsets`。
5. `overflow: visible | clip`、nested clip intersection。
6. `visible` が layout space を残すかを分ける (`hidden` と `collapsed`)。
7. safe area、aspect/letterbox、layout direction の規則。RTL 自動 mirror を v2 に送るなら v1 は明示的に LTR-only error/warn とする。
8. pixel snap と余り pixel の配分。例: stack の余りは宣言順へ 1 pixel ずつ、毎 frame root から再計算。

grid、wrap、scroll、full CSS cascade は v2 でよい。上記まで拒むなら v1 を「固定 ASCII HUD 専用」に改名し、tool widget pack、PSD タイトル画面、responsive config の約束を削るべきである。

PSD 接続の表現も誇張されている。asset 設計の `pelican.layout` は位置、サイズ、不透明度、表示、group だけを最小出力とし (`docs/design_asset_containers.md:63-76`)、blend 忠実度は normal のみ (`docs/design_asset_containers.md:126-134`)。mask、layer effects、PSD text、font、非 normal blend を扱わず「そのまま出る」とは言えない。converter は supported subset を列挙し、未対応表現を hard error か明示 flatten にすること。

## 6. ImGui / pelican.ui 分割: 方向は Accept、導入規約は不足

「ツール overlay は ImGui、出荷 game UI は pelican.ui」という分離は実績がある。O3DE は ImGui Gem を runtime debug/profiling overlay として提供し、別の LyShine Gem を project runtime UI として提供する ([O3DE Gem Reference](https://www.docs.o3de.org/docs/user-guide/gems/reference/), [O3DE LyShine](https://www.docs.o3de.org/docs/user-guide/gems/reference/ui/lyshine/))。したがって二系統の存在自体は問題ではない。

ただし `PELICAN_WITH_IMGUI` と「公開 API のみ」「golden/replay では無効」だけでは次が閉じない。

### 6.1 build/runtime isolation

- OFF build は ImGui source、backend、header、link symbol、font asset を一切含めない専用 CMake target 分離にする。
- ON build でも runtime disabled なら context/backend/pass/input hook を生成しない。単に draw を捨てるだけでは、tool code が public mutation API を呼ぶ副作用が残る。
- golden/replay/headless では tool callback 自体を実行しない。frame plan に ImGui pass がないこと、入力 consumption が変わらないこと、公開 API 呼出し回数が 0 であることを試験する。

### 6.2 input arbitration

Dear ImGui 公式は入力を常に ImGui へ渡したうえで、`WantCaptureMouse/Keyboard` のとき underlying app から隠すよう求めている ([Dear ImGui FAQ](https://github.com/ocornut/imgui/blob/master/docs/FAQ.md), [Getting Started](https://github.com/ocornut/imgui/wiki/Getting-Started))。従って優先順は、`raw input → ImGui → (未 capture なら pelican.ui) → (未 consume なら gameplay)` と明文化する。

現行 Window は GLFW callbacks を自前登録している (`src/core/os/window.cpp:174-205`)。ImGui backend に callback install を任せる方式と自前 forward を混ぜて二重配信してはならない。公式も renderer backend より platform backend の方が難しく、既存 backend の利用を推奨している ([Dear ImGui BACKENDS.md](https://github.com/ocornut/imgui/blob/master/docs/BACKENDS.md))。Window を callback の唯一 owner とし、ordered event を ImGui backend adapter と InputState の両方へ一度ずつ渡す形が安全である。

### 6.3 render/API boundary

- pass 順を `game scene → pelican.ui → debug_text → ImGui` 等に固定し、feature 配列順へ暗黙委任しない。
- standard tool から `GET_MODULE` や private renderer state を参照しない。公開 snapshot/query と通常の command/RPC だけを使う。
- 公開 API でも mutating API は存在する。「特権なし」は「副作用なし」ではないため、replay/golden で tool code を非実行にすることが本当の隔離である。
- docking/multi-viewport、clipboard、IME、OS cursor、device loss は v1 の対応/非対応を明示する。game viewport への単一 overlay だけなら multi-viewport は OFF に固定する。

## 7. golden: 現状の主張は試験件数が少なすぎる

現行 debug_text golden は tolerance average/max とも 0 である (`test/golden/debug_text_feature/tolerance.json:1`)。この水準を共通 batcher で守るには、少なくとも次の非決定性源を閉じる必要がある。

| 非決定性源 | 必須対策 |
|---|---|
| `unordered_map` traversal | render command は document traversal の `vector` 順。map は lookup 専用 |
| layer sort の同値 | key を `(layer, declaration_seq)` に固定し stable sort、または初めから順序付き生成 |
| float 累積 | 前 frame の rect を更新せず root から再計算。layout は integer/fixed-point、丸め箇所を規範化 |
| virtual resolution/DPI | golden の viewport/content scale/letterbox を fixture に固定。render と hit test は同じ transform |
| text metrics | v1 bitmap は同梱 glyph table の integer advance を正とする。OS font/runtime hinting を使わない |
| dynamic atlas | v1 は import-time atlas を正とする。将来 runtime glyph は sorted key、決定的 pack、時間依存 eviction 禁止 |
| texture sampling | sampler を draw key に含め、pixel-art/text は pixel snap + nearest。atlas bleed padding を import 規約化 |
| alpha/color space | straight/premultiplied、blend factors、sRGB decode/output format を shader ABI と fixture に固定 |
| hot reload/async load | frame 境界 atomic swap。golden 中 reload/clock-based completion を禁止 |
| ImGui/animation/clock | pass と callback を無効化。UI animation を入れるなら EngineTime fixed step のみ |

pixel golden だけでは layout/hit/capture の誤りを局所化できない。次の semantic fixture も保存すべきである。

- widget ごとの final rect、clip rect、layer、declaration sequence
- ordered draw-run (`pipeline/sampler/texture/clip/first/count`)
- ordered routed input (`event_seq → target WidgetId → consumed/cancel/click/drag`)
- hot reload 前後の capture cancellation と Controller init/deinit 列

## 8. 実装依存と frame graph の記述が事実と合わない

U1 の依存が「なし」は誤りである (`docs/design_ui_2d_foundation.md:132-138`)。設計自身が `atlas.png#sprite/name` を必須語彙にする一方 (`docs/design_ui_2d_foundation.md:20-23`)、atlas pack は K3 である (`docs/design_asset_containers.md:117-124`)。現リポジトリにも `#sprite`/`atlas_pack` 実装はなく、計画上も K1→K3 の将来作業である (`docs/implementation_plan.md:1357`)。U3 の PSD→UI recipe も K3 と `pelican.layout` subset 定義に依存する。

frame graph の「post_ldr 以後の ui anchor」も存在しない。現行 example は `FinalBloomComposite` の次に名前付き `ui_pass` を直書きする (`projects/example/passes/main_rendering_config.json:298-316`)。feature 挿入語彙は `before:<pass名>/after:<pass名>/end` であり (`docs/design_render_feature_modules.md:59-70`)、semantic な `post_ldr`/`ui` anchor ではない。debug_text は単に `end` へ入る (`src/core/resources/features/debug_text.json:5-18`)。UI、debug_text、ImGui が全て末尾を要求した場合の順は現状の feature 配列順に漏れる。

さらに現 UI pass は shader field を受けず、embedded `UiRenderer` に dispatch する特殊型である (`src/core/renderingpass/renderingpassvalidation.cpp:161-185`, `src/core/vkcore/render_pass_dispatch.cpp:78-95`)。U1 で「既存 UI パス置換」をするなら、次を本文に決める必要がある。

- `engine://features/ui.json` の purgeable feature にするのか、project config の必須 pass のままか。
- `ui` pass の shader/resource ABI を config へ開くのか、専用 renderer に閉じるのか。
- `pelican.ui → debug_text → ImGui → present` の semantic anchors を新設するのか、予約 pass 名を正とするのか。
- feature 無参照時に UI module/GPU resource/parser が本当に立ち上がらないか。

## 9. 再審査の必須条件

- **C1**: `QuadVertex/QuadCommand/DrawRun` の CPU/GPU ABI、型幅、alignment、上限、overflow、alpha/color-space を本文に固定する。
- **C2**: texture slot 戦略、sampler/material key、nested clip、stable run 分割を固定し、atlas 0/1/0 の重なり試験を置く。
- **C3**: `FrameInput` に ordered events を残し、UI consumption mask を Actions 前に適用する。rpc/replay も同一 event stream を使う。
- **C4**: 世代付き `WidgetId` と capture/cancel state machine、DPI/viewport transform を規範化する。
- **C5**: UI-local immediate lane と次フレーム semantic event lane を分離し、`emitImmediate` を禁止する。
- **C6**: Widget registry/factory、UiModule/Document/Controller ownership、hot reload transaction、ECS ID resolve 規約を定義する。
- **C7**: size mode、intrinsic/min-max、stack gap/padding/align、stretch、overflow、pixel rounding を v1 に入れる。入れないならスコープを固定 HUD に縮小する。
- **C8**: U1/U3 の K1/K3 依存を正し、PSD converter の supported subset と unsupported error を列挙する。
- **C9**: UI/debug_text/ImGui/present の pass 順と purge 条件を機械検証できる形にする。
- **C10**: ImGui の build/runtime/input/API isolation を定義し、OFF/headless/golden/replay で context・pass・callback がゼロの試験を置く。
- **C11**: deterministic layout/render profile と semantic fixture を定義し、既存 debug_text exact golden を統合前後で維持する。
- **C12**: 旧 `ui_overlay.json` の atomic migration、parse 失敗時挙動、hot reload 失敗時旧版維持、GPU in-flight resource の遅延破棄を定義する。

## 10. 推奨する実装順

| 段階 | 内容 | ゲート |
|---|---|---|
| U0 | pure CPU: schema、layout、WidgetId arena、ordered input routing、capture、draw-command generation | semantic fixture 全通過。GPU なし |
| U1 | atlas asset/K3 接続、quad buffer、stable draw-run、clip、ui pass、panel/image | 2 atlas 交互重なり、nested clip、旧 UI migration、golden |
| U2 | bitmap label/button、UI-local state、E1 emit、rpc ordered click/drag/replay | debug_text exact golden 維持、同 frame 複数 click |
| U3 | Controller factory/lifecycle、hot reload transaction、gauge/stack/tokens | remove/hide/reload 中 capture cancel、init/deinit 列 |
| U4 | ImGui optional unit と tool widget pack | OFF/headless/golden/replay 完全不在、入力優先順位試験 |
| U5 | PSD subset converter | K3 後。unsupported PSD 表現の明示 error |

現案の U1→U2→U3 は、GPU を先に作ってから入力・所有・layout 契約を発明する順序である。UI の難所は quad を出すことではなく、rect、順序、寿命、capture を同じ決定的モデルに閉じることにある。U0 を通さずに GPU 実装へ入るべきではない。
