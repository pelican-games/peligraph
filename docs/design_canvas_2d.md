# 2D module(canvas)—— node の中の layer stack を、核の語彙へ下げて描く

状態: v2(2026-10-02)。着地先は `develop_v1`(統合 branch `feature/v1-vector2d`)。v1 は「図形の node 群 → canvas」の案だった(同日午前)。利用者の指摘(§0)で中心を **layer stack** に替え、敵対レビュー(§13)と実証(§8)を反映した。§1 は実物と論文で確かめた事実、§2〜§7 は形、§11 は裁定の記録。

## 0. 経緯と裁定

2026-10-02、利用者の要求:

- peligraph のパージ可能機能の 1 つとして、2D 素材のアニメーション・レイアウト・描画を作る。長方形や円から、折れ線・ベジエ・フォントまで。2D の画面だけでなく 3D 空間に置く、アニメーションに使う
- DCC の面が強い機能は、node であっても build 向けにすべて焼き、リアルタイムの道具として遊べるように最適化する。複数の node を 1 本の shader に押し込むことが多いはずなので、その周りの設計をしっかり行う

同日午後、利用者が「行いたい node tool の特徴」を説明した:

- Black Ink の node 機能のように、**node の中に layer があり、その layer group の出力を演算する**
- layer group は PSD とある程度互換がある
- layer stack に、ゲーム特有の動的な 2D オブジェクトを足せる。動的な 2D オブジェクトの上に layer stack を積める
- ゲームにするときは、静的な所は焼き込んで高速化する
- layer に depth を付け、3D 空間に置かれた物として扱う、厚みを付ける、立体の layer stack をゲーム内オブジェクトとして扱う
- filter も node でやる
- 進むと最終的には UI 周りと融合していく
- 作りながら疑問点は聞く

| 日付 | 裁定(利用者) |
|---|---|
| 10-02 | 名前は `pelican.canvas` |
| 10-02 | brush で描く機能はかなり後回し(リアルタイム用途で付けたくなる時が来るかも、程度)。画素の layer は取り込み、図形と文字は Studio で作る |
| 10-02 | 動的な物は、まず「この 2D 機能で作る動く物」と「3D 描画の動く物」。render pass も layer に入れたい |
| 10-02 | depth は両方: 層ごとの奥行きと厚み、画素ごとの高さ |
| 10-02 | この機能は `develop_v1` 向け |

Black Ink の Layer Editor(公式文書で確認): 構成要素は Operator(Layer / Color / Blend / Mask / Transform / Blur / 色調整 / Symmetry / Tiling / Domain Wrap / Output Image)。**Layer Stack は「重ねる」Operator** で、入力に繋いだ Operator を好きな順に積み、入力ごとに blend mode・不透明度・表示を持つ。入力はどの Operator でもよく、Stack は入れ子にでき、Stack の出力は下流の Operator に繋がる。Stack に入った Operator は graph では隠れる。すべて非破壊。

関係する既存の裁定: [pelican_vision.md](pelican_vision.md) 1.1「2D はパージ可能な副機能群」と原則 1・3・4・6・10、[v1_development_plan.md](v1_development_plan.md) の「2D ノード集(instanced raster + compute + attribute buffer の副機能群)」、10-02 の「DCC 寄りの機能は完成時に完全に焼く」、[design_graph_layers.md](design_graph_layers.md)(上から下まで辿れる、領域の filter、編集できるのは L0 だけ)、[design_asset_containers.md](design_asset_containers.md) §3(PSD は engine の外の tool で展開。layout の意味論の正は 2D の設計が持つ)。

## 1. 今の作り(2026-10-02、develop `681b77bb`。実物で確認)

### 1.1 node と shader の対応

- `render.fullscreen` / `render.raster` / `render.compute` は、1 個の node = 1 個の pass(か compute task)= 1 組の shader = 1 本の pipeline。lowering(`src/project/rendergraphcomposition.cpp`)が node ごとに宣言を 1 件作り、名前は node の id。GL-2a の `origin` も宣言名をキーにした 1 対 1 の表。`render.material_pass` だけは material と variant の数だけ pipeline を持つ
- **複数の graph node の shader source を繋いで 1 本にする機構は無い。** 近いものは surface compiler(雛形に hook を virtual include で差し込む)、`ShaderLibrary::loadGeneratedSource`(RT の補助 shader だけが使う)、試験的な spvlink
- `graph.loop` は body の N node × M 回の pass に展開される(上限 64 回 / 合計 128)。`draw_items` の loop だけは 1 つの material pass の中で回る
- value 系の node は CPU で毎 frame 評価され(double)、`render.scalar` の sink が `runtime_f32` の storage buffer に書く。`render.scalar` は `target` と `offset` を取り vec4 まで書けるので(RS-15)、複数の sink で 1 本の buffer を埋められる。条件は 0 番地から隙間なく埋めること
- **作者の文書を読む消費者は compose だけではない。** core は同じ文書から ValueGraph・PoseGraph・attribute の解決を直接組み立てる(`src/core/renderingpass/renderingpassconfigregistration.cpp`)。editor は編集のたびに candidate を作る。`graph.instance` の展開は `partitionGraphDocument` の中
- **「値だけの編集」の道は無い。** `edit_graph_document` は layout だけの編集を除き、毎回 candidate を作って compile・publish する。catalog の `change` はどこからも読まれていない
- graph の入力は time / delta_time / scene_time / morph の重みだけ。owner の world transform を graph から読む口は無い

### 1.2 パージと登録

- フラグの正本は `cmake/pelican_feature_registry.cmake`。graph の module は `src/project/graphmodule.cpp` が明示的に登録し、`pelican.audio` と `pelican.pose` は `#if`
- 外した module の op は `graph.operation_unregistered@1`、型は `graph.type_unregistered@1`、`requires` にあれば `<name>.module_unavailable@1`
- 音声を足したときに触ったファイルは約 20 本で、`#if` は core の各所に散っている。**実行時に居る機能は、外す面が広い**

### 1.3 焼き込みと出荷

- **「すべてを焼いて出荷する」道具立ては今は無い。** bake 系の cli は `bake-camera` と、`develop_v1` に入った `intro-link bake`。`dist-bake`([design_build_tiers.md](design_build_tiers.md) §4)は未実装。Studio に Export は無い。音の解析の焼き込みは設計も実装も無い
- port を持つ graph の pass は、事前 compile した `.spv` を使えない(生成 include が GLSL の source を要求する)。shader compiler を外した配布に効く

### 1.4 既にある 2D

- `pelican.ui` v1(layout、widget 6 種、ASCII の bitmap font、quad ABI)、`sprite_view`、debug_text、ImGui。feature を参照したときだけ立ち上がり、renderer の固定の anchor で描く(graph の node ではない)
- font は bitmap だけ。freetype / msdfgen / harfbuzz は入っていない
- 既存の 2D の規約は straight alpha と `(SRC_ALPHA, ONE_MINUS_SRC_ALPHA)`([design_ui_2d_foundation.md](design_ui_2d_foundation.md) §1-3)
- **project の画像 file を graph の pass で読む口が無い**(§8)

### 1.5 論文から採る事実(16 本。メモは `<作業ディレクトリ>/v2d_papers/`)

| # | 事実 | 出所 |
|---|---|---|
| P1 | 距離から作る効果(縁取り・glow・影・太さ)は shader の定数なので、焼いた後でも毎 frame 動かせる。届く範囲の上限は焼くときの距離の幅で決まる | Green 2007、Chlumský 2017 |
| P2 | 描画の費用は焼いた形式でほぼ決まる。距離画像は fetch 1 回で「通常の texture と差がない」、MSDF は 1 channel より 12〜56 % 遅い、曲線の直描きは 2 MP で 1.1〜13.3 ms(同じ論文の画素 atlas は 26 µs。最良でも約 40 倍) | Green 2007、Chlumský 2017、Lengyel 2017 |
| P3 | 斜めの面に等方のぼかしを使うと文字が読めなくなる。距離の傾きを画面に写して AA 幅を決める(近似は `fwidth`) | Qin 2006、Chlumský 2017 |
| P4 | MSDF は角が鋭いまま。約 2 cell より細い線は表せず、太い縁取りで尖った角が切れる。焼くには自己交差の無い vector 入力が要る | Chlumský 2017 |
| P5 | 縮小(1 pixel に形が多数入る)は 6 本ともほぼ論じていない。設計側で決める必要がある | Ellis 2019 ほか |
| P6 | 形そのものが毎 frame 変わると、どの方式も前処理のやり直しになる | Loop & Blinn 2005、Ganacim 2014、Dokter 2019 |
| P7 | 例外: 2 次ベジエを帯として描いて GPU で距離場を作る方式は、256² 以下で 0.031〜0.035 ms、毎 frame 変形する 800² でも平均 0.10 ms。制約は 2 次だけ・重なりと自己交差は不可・距離は帯の幅まで | Chen ら 2026 |
| P8 | path を部品の列に焼いておくと、変換・線幅・join・cap・fill rule・色は**形を焼き直さずに**変えられる | Kilgard & Barran 2026 |
| P9 | stencil-then-cover は path を 1 本ずつ逐次に描くことが律速になる | Ganacim 2014、Dokter 2019、Kumar 2021 |
| P10 | 曲線を折れ線にすると、0.5 px の許容誤差でも輪やぎざぎざが出る | Manson & Schaefer 2011 / 2013 |
| P11 | 画面にまっすぐ置く文字だけなら画素 atlas が最良だが、透視や動的な拡縮には距離方式を使え、と著者自身が書いている | Rougier 2013 |

## 2. 中心の考え

1. **layer stack が node。** `canvas.stack` は入力に繋いだ layer を順に積む。入力ごとに blend・不透明度・表示・mask・depth を持つ。入れ子が group。stack の出力は layer で、filter の node や別の stack に繋がる(Black Ink と同じ)
2. **layer の線は「描き方の記述」。** 画素にするかどうかは compiler が決める。積むだけなら 1 回の draw の中の 1 item になり、近傍を読む filter や backdrop を読む blend が要る所だけ、image に描いてから使う
3. **canvas の node は実行しない。** 組み立てのたびに compiler(lowering)が核の語彙(`value.*` / `render.image` / `render.buffer` / `render.raster` + 生成した GLSL)へ下げる。実行時の engine は下げた結果だけを走らせる
4. **焼きは同じ compiler の別の方針。** 動かない layer の連なりを 1 枚に畳み、定数を shader に畳み、canvas の node を核の node に置き換えた project を出す。出荷物は canvas module の無い player で走る
5. **形は距離場。** 図形と文字は距離場として扱い、塗り・縁取り・glow・厚みを距離から作る(P1)

```
canvas.* / shape.* / filter.* の node(L0。作者が触る正本)
   │ ① 評価(CPU、決定的): 描く物の列(IR)と、各 parameter の段
   ▼
   │ ② 段の推論・image にする所の決定・まとめ・距離場の作り方の選択
   ▼
下げた形(派生。保存しない)
   value.* + render.buffer(表)+ render.image(canvas・atlas)+ render.raster × 少数 + 生成 GLSL
   │ 既存の lowering 以降は今のまま
   ▼
frame plan → Vulkan
```

fold(`fold_groups`)との違い: fold は同じ核の語彙の中の最適化で、開いた形もそのまま走る(だから印と失効が要る)。lowering は上の語彙から核の語彙への変換で、canvas の node は単独では走らない。決定的な関数で毎回やり直すので、印も失効も持たない。

## 3. 一般の契約 —— DCC 寄りの module を核の語彙へ下げる

canvas に限らない。作る段の道具を足すときの規則として置く(音の解析、intro-link も同じ形)。

| # | 規則 |
|---|---|
| L1 | **語彙の向き**: DCC module の op は、組み立て時に核の語彙へ下がる。core が持つのは「文書を読んだら登録済みの lowering を通す」汎用の hook 1 つだけで、module 固有の code は `src/project` と tool に置く |
| L2 | **一本道**: 編集中も出荷も同じ lowering を通る。違うのは方針だけ(`edit` / `ship`)。編集中にだけ動く経路を作らない。atlas や表も、**文書だけから決まる関数**にする(編集の履歴に依存させない) |
| L3 | **段**: 各 parameter がいつ評価されるか(§3.1)を lowering が推論する。作者は線を繋ぐだけ |
| L4 | **生成物は派生**: 生成した shader・表・atlas は内容の digest が名前。正本は node。生成物は読める |
| L5 | **焼き**: `ship` 方針の下げた形を project に書き出し、DCC の node を核の node に置き換え、`requires` からも外す。出荷用の project は build の産物 |
| L6 | **検査**: (a) `edit` と `ship` の絵が許容幅の中で一致、(b) module を外した build の player で焼いた project が同じ絵、(c) 焼いていない文書を外した build で開くと `graph.operation_unregistered@1` |
| L7 | **出自**: 下げた pass の名前は受け口の node の id(`origin` が 1 対 1 のまま戻れる)。item ごとの出自は lowering が表で返す |

### 3.1 段(評価の頻度)

| 段 | いつ評価するか | 実体 |
|---|---|---|
| `bake` | 下げるとき 1 回 | 定数。shader の literal、表の初期値、atlas、畳んだ画像 |
| `frame` | 毎 frame、CPU | `value.*` → `render.scalar` → parameter 表 |
| `element` | 要素ごと(図形 1 個、文字 1 個、繰り返しの 1 回)、vertex shader | 生成 GLSL |
| `pixel` | 画素ごと、fragment shader | 生成 GLSL。距離・塗り・filter・合成 |

- node の出力の段 = 入力の段の最大。`value.input` と外から来る値は `frame`、`shape.element` は `element`
- 同じ node(`value.add`、`value.curve`)が、推論された段に応じて CPU にも GLSL にも下がる(Blender の field と同じ考え)。ただし **GLSL に下げてよい op は印で限る**: 状態を持つ・replay の op(`value.trigger_clock` / `value.event_clock` / `value.playback`)は `frame` の段に留まり、その出力を `element` の式の入力に使うことはできる。`shape.element` の下流に置いたら `canvas.element_op_unsupported@1`
- `value.curve` を GLSL で評価するときは、key の表をそのまま持ち、同じ補間式(step / linear / cubic)を使う(一定間隔の標本にしない)。残る差は double と float だけ
- `element` の段の値が value 側の出力(sink、pose への出力、instance の入力、graph の `outputs`)へ流れたら `canvas.element_rate_escapes@1`
- catalog の `change`(`value` / `rebuild`)が「`frame` 以下の段にできる parameter か」を宣言する

### 3.2 一本道の帰結(正直な制限)

実行時に動くのは「生成した GLSL」と「既存の `value.*`」だけなので、GLSL と value graph で書けない仕事は `bake` の段に限られる: 文字列の組版、MSDF の生成、自己交差の除去、layout の計算、PSD の展開。**実行中に変わる文字列は、この形のままでは描けない。** UI との融合(§10)に進む段で、組版の小さな runtime を別の unit として足す(そのとき item 表を game code が書ける口を作る)。

### 3.3 方針(`edit` / `ship`)

| | `edit` | `ship` |
|---|---|---|
| 線の無い `change: value` の parameter | 表の値(shader の text は変わらないので cache に当たる) | 定数。全 instance で同じなら shader の literal |
| 動かない layer の連なり | そのまま描く | 1 枚の image に畳む(§6) |
| 使われない kind・分岐 | 残してよい | 消す |
| 生成物 | 派生(cache) | 出荷用 project の file |

`edit` で値を動かしたときに組み直しを避けるには、engine に「値だけの編集」の道が要る(§9 の EN-v。今は無いので、当面は編集のたびに candidate が作られる。shader の再 compile は起きない)。

### 3.4 焼きの道具

- lowering を cli から呼ぶ `pelican_cli canvas-lower`(CV-1)が最初の形。**下げた project は今の player でそのまま走る**
- `dist-bake`(名前は build_tiers §4 が予約)を段の registry にする: 段 1 = DCC module の lowering(`ship`)、段 2 = shader の variant を `.spv` に(B4。port を持つ pass の `.spv` の道 = EN-spv が前提)
- 動かない layer を畳むときの描画は、**同じ生成 shader を headless の engine で走らせて**画像にする(FG-2 の「子 process で描く」仕組み)。CPU の別実装で畳まない(絵が変わる)

## 4. 語彙

module `pelican.canvas`(version 1)、フラグ `PELICAN_WITH_CANVAS`、`requires` の語 `canvas`、Studio の領域 `canvas`(op の接頭辞 `canvas.` / `shape.` / `filter.`)。

### 4.1 型

- `pelican.canvas.layer@1` —— 描き方の記述。外接矩形と、premultiplied alpha の色を持つ「まだ画素にしていない」layer
- `pelican.canvas.geometry@1` —— 形(輪郭の集まり。塗る前)

### 4.2 layer を作る op

| op | 入力 → 出力 | 中身 |
|---|---|---|
| `canvas.image` | → layer | project の画像(PNG など。PSD から展開した layer)。`asset`、位置 |
| `canvas.from_image` | `render.image` → layer | **他の pass の出力を layer にする。** 3D scene の描画、自作の pass、別の canvas。動的な物の入口 |
| `shape.fill` / `shape.stroke` / `shape.glow` | geometry → layer | 図形・文字の塗り(§4.5) |
| `canvas.color` | → layer | 単色・gradient |
| `canvas.stack` | layer × 可変 → layer | 重ねる(§4.3) |
| `canvas.backdrop` | → layer | 「この stack で、ここまでに積んだ結果」。調整 layer(下の全部に掛かる filter)を書くための源(§4.4) |

### 4.3 `canvas.stack`

```json
{"id": "hud", "op": "canvas.stack",
 "parameters": {
   "size": [1920, 1080],
   "blend_space": "srgb",
   "layers": [
     {"input": "bg",    "name": "Background"},
     {"input": "scene", "name": "3D", "blend": "normal"},
     {"input": "glow",  "name": "Glow", "blend": "screen", "opacity": 0.8},
     {"input": "frame", "name": "Frame", "mask": "frame_mask", "depth": 12, "thickness": 4},
     {"input": "title", "name": "Title", "clip": true, "visible": true}
   ]},
 "inputs": {"bg": {...}, "scene": {...}, "glow": {...}, "frame": {...}, "frame_mask": {...}, "title": {...}},
 "outputs": ["layer"]}
```

- `layers` は下から上の順。`input` と `mask` は自分の `inputs` の key(port は `parameter_dependent`)
- 入力ごとの性質: `visible`、`opacity`(value)、`blend`、`clip`(1 つ下の layer の alpha で切る。PSD の clipping)、`mask`(別の layer の alpha か輝度)、`depth` / `thickness` / `height`(§7)、`pass_through`(入れ子の stack を、親の列にそのまま並べるか、1 枚にしてから積むか。PSD の group の既定は pass through)
- `blend_space`: 合成を行う色の空間。`linear`(既定)か `srgb`。Photoshop は既定で sRGB の値のまま合成するので、PSD から来た stack は `srgb` にする(見た目を合わせる)。stack の出力で linear に戻す
- 座標は stack の `size` の画素。原点は左上、+y は下(PSD と UI に合わせる)
- canvas の image は **premultiplied alpha**(透明な下地に積んだ結果は必ずそうなる)。既存の 2D の規約(straight)と違うので、canvas の image を他の pass や material に渡す所で premultiplied として扱う
- blend mode の語彙は PSD の名前に合わせる(`normal` / `multiply` / `screen` / `overlay` / `darken` / `lighten` / `color_dodge` / `color_burn` / `hard_light` / `soft_light` / `difference` / `exclusion` / `linear_dodge` / `linear_burn` / `hue` / `saturation` / `color` / `luminosity` …)。未対応は `canvas.blend_unsupported@1`

### 4.4 filter(layer → layer)

filter は 3 種類に分かれ、種類で下げ方が決まる。

| 種類 | 何を変えるか | 例 | 下げ方 |
|---|---|---|---|
| 座標 | 読む位置 `p` | `canvas.transform`(移動・回転・拡縮)、`filter.tile`、`filter.symmetry`、`filter.warp` | item の shader に融合(距離場や画像を読む前に `p` を変える) |
| 色 | 読んだ色 `c` | `filter.levels`、`filter.brightness_contrast`、`filter.hue_saturation`、`filter.gradient_map`、`filter.color_overlay`、`filter.invert` | item の shader に融合(pass を増やさない) |
| 近傍 | 周りの画素 | `filter.blur`、`filter.drop_shadow`、`filter.outer_glow` | 入力を image に描き、別の pass(既存の blur の loop)で処理 |

- 調整 layer(PSD の adjustment layer)= `canvas.backdrop` に色の filter を掛けた layer。**blend mode も同じ仕組み**(backdrop を読んで色を決める)。backdrop を読む layer があると、lowering はそこで draw を区切り、canvas の写し(既存の `render.snapshot_copy`)を読ませる
- 距離を持つ layer(図形・文字)の縁取り・glow・影は、近傍の filter ではなく距離から作る(pass を増やさない。P1)

### 4.5 図形(`shape.*`)

| 種類 | op | `value` にできる主な parameter |
|---|---|---|
| 形の源 | `shape.rect` / `shape.ellipse` / `shape.polygon` / `shape.polyline` / `shape.path` / `shape.text` | 大きさ、角の丸み、点の座標、文字の大きさ(点の数、font、文字列は `rebuild`) |
| 形の加工 | `shape.transform` / `shape.boolean` / `shape.offset` / `shape.trim` / `shape.repeat` / `shape.morph` | 変換、なめらかさ、量、start / end、重み |
| 塗り | `shape.fill` / `shape.stroke` / `shape.glow` | 色、幅、半径(cap・join・align は shader を選び直す `rebuild`。形の焼き直しは要らない) |
| 要素 | `shape.element` | 番号 / 個数 / 0..1 の位置(§3.1 の `element` の段) |
| 値へ | `shape.path_sample` | path 上の位置と角度を value に(motion path) |

### 4.6 受け口

| op | 入力 → 出力 | 用途 |
|---|---|---|
| `canvas.output` | layer → `render.image`(resource port `image`) | 画面に出す、他の pass に渡す、material の texture にする |
| `canvas.place` | layer → 3D の pass への描き込み | 3D 空間に置く(§7) |
| `canvas.field_image` | geometry → `render.image`(距離場) | 自作 shader の素材 |

### 4.7 名前付き error(現時点)

`canvas.in_loop_body@1`、`canvas.reserved_id@1`、`canvas.blend_unsupported@1`、`canvas.layer_input_missing@1`、`canvas.stack_cycle@1`、`canvas.output_unconnected@1`、`canvas.element_rate_escapes@1`、`canvas.element_op_unsupported@1`、`canvas.method_unavailable@1`、`canvas.range_unbounded@1`。黙って近似したときは diagnostics(`canvas.group_opacity_flattened`、`canvas.layout_uses_rest_value` など)。

## 5. 複数の node を 1 本の shader へ

### 5.1 IR

①の評価で、1 つの受け口に流れ込む node 群を「描く物の列」にする。

```
item {
  order                   // 描き順
  transform               // 2D の affine と座標の filter の合成。因子ごとに段を持つ
  source                  // 何を読むか
    := field(距離場の式の木: prim | atlas | image | op(union …))+ styles[](fill | stroke | glow)
     | image(render.image か asset、uv 矩形)
     | backdrop
  color_filters[]         // 色の filter の列
  blend, opacity, mask?, clip?
  depth?, thickness?
  element_scope?
}
```

### 5.2 kind と 1 回の draw

- **kind** = source・styles・filter の形が同じ item の類。違うのは parameter だけ(文字 500 個は 1 kind)
- 受け口 1 つにつき、生成 shader は 1 組。vertex は item 表から外接矩形の quad を作り、`element` の式を評価する。fragment は `switch (kind)` で色を出す
- **backdrop を読まない item が続く間は、1 回の draw。** 同じ draw の中の primitive は順に blend されるので、描き順は instance の順で保たれる
- draw が区切れる所: backdrop を読む item(blend mode、調整 layer)、image にしてから使う layer(近傍の filter、pass through でない group、group の不透明度、mask にする stack)。区切りの数と理由は diagnostics に出す
- AA: 距離を持つ item は、距離の画面空間の傾きで画素単位に直して coverage にする(P3)。画像は sampler の補間
- 実測(§8): 図形 9 個、1080p、1 draw で GPU 約 0.016 ms

### 5.3 表

- item 表(instance ごと): kind、表の offset、外接矩形、変換、不透明度
- parameter 表は 2 本: `bake`(定数。初期値だけ)と `frame`(`render.scalar` の sink が隙間なく埋める)。分けるのは「sink は buffer 全体を覆う」規則のため(§1.1)

### 5.4 名前

`canvas.output` の id を `<id>` として、下げた node は `<id>`(`render.raster`。write の port は `image`)、`<id>$canvas_image`、`<id>$canvas_items`、`<id>$canvas_bake`、`<id>$canvas_frame`、`<id>$canvas_set_<n>`。作者の id と重なれば `canvas.reserved_id@1`。

## 6. 静的な所を焼く

`ship` 方針で、stack の中の layer を「動くか」で分ける。layer が動かない = 源が動かず(画像 asset、`bake` の段の図形)、その layer に掛かる parameter がすべて `bake` の段。

| 場合 | 焼き方 |
|---|---|
| 動かない layer が下から続く | 1 枚の image に畳む。**CV-5 の実装**: backdrop を読む layer(normal 以外の blend、調整 layer)では連なりを切る。stack が丸ごと静的なときだけ blend 込みで 1 枚(`static_unit`) |
| 動く layer の上に、blend が normal の動かない layer が続く | 1 枚の premultiplied の image に畳む(over は結合的) |
| 動く layer の上の、backdrop を読む動かない layer | 中身(filter を掛けた結果)だけを 1 枚に焼き、合成は実行時に残す(**未実装**。CV-5 は切るだけ) |
| 図形・文字だけの連なり | 畳まない(式のまま描くほうが安く、3D で寄っても鋭い)。作者が `bake: "texture"` で固定できる |

- 畳む解像度は `ceil(stack.size × bake_scale)`(既定 1)。畳むときの描画は CPU の参照ではなく、**連なりだけの文書を `edit` で下げて player(`--headless --frames 2 --render-out`)で描く**。得た PNG は `<out>/canvas_baked/<digest>.png`(premultiplied の linear を sRGB で encode した 8 bit RGBA。読み側は `canvas.image` の `alpha: "premultiplied"`)。digest は連なりの文書 + 画素数 + 読む file の sha256。同じ project から同じ byte
- `ship` は定数も畳む: 1 つの draw の中で全 item が線なしで同じ値を持つ parameter は shader の literal にし、bake 表には残りだけを書く(`canvas.place` の draw は対象外)。出力のどの JSON にも名前が残らない画像と font の file は出力から削る
- 畳まない所: 図形・文字だけの連なり(`bake: "texture"` があれば畳む)、既に 1 枚の `canvas.image` の連なり、`canvas.place` された stack(`canvas.flatten_skipped {reason: "placed"}`)、pass through の下の stack の中(group の不透明度の近似が一致しない)。`--player` が無ければ `canvas.flatten_skipped {reason: "no_player"}`。player が描けなければ `canvas.flatten_failed@1`、16384 画素幅を超えると `canvas.flatten_size_exceeded@1`
- 畳むと変わる絵: stack の `size` の外に出る item は切れる、1 を超える値は 8 bit で clamp される
- 畳んだ結果と理由は diagnostics と L1 の詳細欄に出す(「この 7 枚は 1 枚になった」)
- 検査は L6(a): `edit` の絵と `ship` の絵を比べる

## 7. depth と 3D

| 性質 | 意味 | 段階 |
|---|---|---|
| `depth` | layer の奥行き(stack の面からの距離。stack の単位) | CV-6 |
| `thickness` | layer を押し出した厚み(側面あり) | CV-6 |
| `height` | 画素ごとの高さを与える layer(浮き彫り) | 後。形だけ予約 |

- `canvas.place` は stack を 3D の pass に描く。**同じ depth の layer どうしは 2D と同じ合成、違う depth の間は奥から順の normal の合成**(3D の背景を backdrop として読む blend は扱わない)。depth test あり。depth write は選べる(切り抜きとして 3D の物を隠すか)
- 座標は engine の規約のまま([design_coordinate_convention.md](design_coordinate_convention.md): 右手系、+Y 上、camera は −Z 前方、clip の y は射影の中で反転)。stack の +x は面の +x、stack の +y(画像の下向き)は面の −y。裏表は view 空間の法線で判定し(巻き方に依らない)、cull はしない
- 同じ fragment の code を、vertex だけ「camera の VP × model 行列」に替えて使う。距離の傾きで AA を取るので透視と斜めに耐える(P3、P11)。実証済み(§8)
- 厚み: 距離を持つ layer(図形・文字、alpha から距離場を焼いた画像)を、箱の中で距離場を押し出して描く(側面が出る)。最初は unlit で側面の色を指定
- **ゲーム内オブジェクトにする**: stack を definition に入れ、object に `graph.instance` として装着する。model 行列は owner の world transform(engine の新しい口 EN-4 が要る。§1.1)
- 逆向き(3D の物を layer にする)は `canvas.from_image`: `render.material_pass` で対象の object だけを透明な image に描き、それを layer として積む

## 8. 実証(SH-0)

「下げた先の形」を手で書き、engine を変えずに今ある exe で走らせた(`<作業ディレクトリ>/v2d_work/sh0/`)。

| 点 | 結果 |
|---|---|
| canvas の image、item 表(`render.attribute`、instance、`initial_f32`) | そのまま動いた |
| 1 本の raster pass(9 instance、1 draw、`switch (kind)`、premultiplied の blend) | そのまま動いた。AA、重なり順、時刻で変わる半径と回転を目で確認 |
| 複数の value → 1 本の buffer | RS-15 入りの build でそのまま動いた(`render.scalar` を並べる)。sink が buffer 全体を覆わないと `incompatible frame graph output buffer` |
| present | `present.input` に canvas を書くだけ |
| 3D 配置 | そのまま動いた(`render.material_pass` の color / depth に同じ fragment で板を描く。camera 行列は `pelicanFrame`。箱の奥に隠れる) |
| 画像 asset → sampled 入力 | **口が無い**。`render.image` の未知の key は黙って無視される → EN-2 |
| 事前 compile した `.spv` | port を持つ pass では断られる → EN-spv |
| frame time | 1080p、図形 9 個で shapes の pass の GPU 約 0.016 ms(warm-up 前の目安) |

### 8.1 パージの検査(L6。2026-10-02、`b1c9e591`、手で 1 回)

`PELICAN_WITH_CANVAS=OFF` で `pelican_player` と `pelican_cli` を build して確かめた(script は `<作業ディレクトリ>/v2d_work/off_check.sh`。test にはまだなっていない)。

| 検査 | 結果 |
|---|---|
| (b) ON の `canvas-lower` が下げた project を、OFF の player で描く | frame 1 と frame 61 が ON の player の PNG と byte 一致 |
| (c) 下げていない project(`requires: [canvas]`)を OFF の player で開く | `canvas.module_unavailable@1` で終了(exit 1) |
| (c) OFF の cli の `canvas-lower` | `build_feature_disabled@1` |
| OFF の `graph-catalog` | `pelican.canvas` が無い |

残り: `requires` を書いていない文書での `graph.operation_unregistered@1`、Studio(`src/devstudio` の Layers パネルは `#if` を持たない)の OFF。(a) `edit` と `ship` の一致は CV-5 の `canvas_ship_headless_player` が見る(OFF の player で `ship` の出力を描く確認は `off_check.sh` で手動)。

既知の不具合(CV-5 で見つけた。未修正): `canvas.output` に `size` を書くと `render.image` に width / height だけが出て `extent_scale` が無く、engine が拒否する。`canvas.place` の group の image も同じ経路。engine 側の穴(SG-3 で見えた): filter の数値は value の線を受けない(`canvas.filter_edge_unsupported@1`)、`ranges` / `opacity_stops` / `dithered` / `legacy` は未対応。

## 9. 段(WP の順)

| WP | 中身 | 状態 |
|---|---|---|
| SH-0 | 下げ先の実証 | ✅ |
| **CV-1** | module、`shape.rect` / `shape.ellipse` / `shape.fill` / `shape.stroke` / `canvas.stack`(normal)/ `canvas.output`、lowering(純関数)、CPU の参照描画、`pelican_cli canvas-lower`。下げた project を今の player で走らせる | ✅ `4e133e0c`。参照描画との一致 99.96 %(2/255 以内。外れは inner stroke の角の 23 画素) |
| **EN-2** | 画像 asset を graph の pass で読む口(`render.image` の `source {asset, color_space, alpha, mips}`)、未知の key の拒否(`graph.image_parameter_unknown@1`) | ✅ `a3fd7e99` |
| **CV-1b** | engine が文書を読む点で登録済みの lowering を通す汎用の hook(`src/project/authoringlowering`。登録点と editor の compose の道で呼ぶ。core は canvas の名前を知らない)。生成 shader は `.pelican/generated/canvas/<stem>/`。rendering 節の pass の入力に `canvas.output` の id を書ける。`get_authoring_lowering` | ✅ `b5f399fc` |
| **CV-P** | PSD の取り込み(`pelican-import-tools` の `psd-canvas`。psd-tools)→ 画像 + mask + canvas の definition + `reference.png`(§10) | ✅ import tool の `feature/canvas-psd`(`13310e1`、`d1577a8`) |
| CV-2a | `canvas.image` / `canvas.from_image` / `canvas.color`、`canvas.transform`(定数)、mask(画像だけ)・clip(土台 + clip 群を 1 枚にして ATOP)、pass through でない group、`blend_space`、definition の展開。blend は normal まで | ✅ `c8185793`。PSD の往復は全画素が `reference.png` と 1/255 以内 |
| CV-2b | blend mode 26 種(W3C の 16 + Photoshop だけの 10)、調整 layer(`canvas.backdrop`)、色の filter 6 種。backdrop を読む item 1 個につき写し 1 回(`<prefix>back<k>`。共有は未実装) | ✅ `20132b0f`。blend 8 種と調整 layer 3 種の PSD が `reference.png` と全画素 1/255 以内。soft_light と hard_mix は psd-tools と定義が違い、CPU の参照で確認 |
| CV-6a | 3D に置く: `canvas.place`(position / rotation / scale は value)と layer の `depth`。四隅を計算する関数 `canvasplace`。箱に隠れる・視差(計算との差 1.2 px 以内)・曲線で動く、を GPU で確認 | ✅ `70e1d184`。最初は正面の文字が左右反転して出たが、原因は engine の 3D の画面が world の鏡像だったこと(canvas の bug ではない)。engine 側を v0 で直し(develop `c8367b87` CC-1、`1501685a` CC-2。[design_coordinate_convention.md](design_coordinate_convention.md))、CVM-1 で取り込んだ後は「Placed」が正しく読める |
| CVM-1 | develop(座標の規約 CC-1 / CC-2、IA-1、PS-1、CT-5 ほか)の取り込み。`canvas.place` の test を新しい規約(camera は −Z 前方、回転なしで z = 3 から見る)に書き直し。EN-2 の `source` と IA-1 の `initial` / PS-1 の `lifetime` を同じ `render.image` に書くと `graph.image_source_conflict@1`(file の image は中身を自分で持つ) | ✅ merge `2fa65483` + `635225ce`。非 GPU 193、GPU 12(canvas 7 + 鏡像の検査 + CC-2 + IA-1 + EN-2 ×2)、Studio の unit test 8(SG-1 / SG-2)と scenario 4(canvas の 2 本は仮想モニタで。hidden desktop では viewport の画素が黒で赤になる)が緑。Studio では IA-1s と SG-2 の struct 名が衝突し、canvas 側を `GraphCanvasPictureImport` に改名 |
| CV-5 | `ship` 方針: `pelican_cli canvas-lower --policy ship --player <exe>`。静的な連なりを player で 1 枚に焼いて `canvas.image` に置き換え、定数を shader の literal に、参照の無くなった画像と font を出力から除く。report に `flattened` / `folded_constants` / `removed_assets` | ✅ `a5399a71`。PSD の fixture + 動く円で、`edit` と `ship` の絵が 2 つの時刻とも全画素 1/255 以内。instance 10 → 4、raster pass 5 → 2、定数 15 個 |
| CV-3 | filter: 色(融合)、座標(融合)、近傍(blur・影) | |
| DF-1 の結果 | 画像 → SDF: 縁の位置を 2D の点で持ち 3×3 で測り直す版が平均誤差 0.028 画素(素朴な種 0.137、2 値化 0.217)。MTSDF: Arial 32² の被覆率の MSE 4.80e-5(1 channel は 8.30e-4)。日本語は 48²、画数の多い字は 64² | ✅(`<作業ディレクトリ>/v2d_work/df1/run_full.log`) |
| CV-4 | 文字: project の ttf を lowering が直接読む(中間の font 形式は作らない)、MSDF の atlas、`shape.text`(kerning・改行・揃え・折り返し)、lowering の context(file を読む口)、生成物に binary(atlas の PNG) | ✅ `b1c9e591`。GPU と参照描画の一致 99.987 %(8/255 以内)。「鬱」は `atlas_cell` 64 が要る |
| SG-1 | Studio: 領域 `canvas`、palette の Canvas、stack のソケット、Canvas layers パネル(行・+ Rect / + Ellipse / + Group / + Stroke・目・不透明度・並べ替え・欄)。scenario `canvas_layers_from_blank` | ✅ `cbd95519` |
| SG-2 | Studio: Layers を独立の dock に、+ Image、clip / mask / pass through の欄、変換、PSD の取り込み結果の読み込み | ✅ `c57ed285` |
| SG-3 | Studio: blend の combo(26 種、Photoshop の group 順)、+ Text(project の ttf、Import font…)、+ Adjustment / + Filter(transform → filter → stack の順)、Place in 3D scene(`render.material_pass` の color に `canvas.place`)、拒否 5 種の文、行の印。scenario `canvas_text_blend_from_blank`(OS の Arial を prepare で写す。repo に font は無い) | ✅ `4cc6dc26` |
| CV-6 | 3D: `canvas.place`、depth、厚み、owner の transform(EN-4) | |
| CV-7 | path(折れ線・ベジエ)、`shape.trim`、`shape.path_sample`、`element` の段(`shape.repeat`、1 文字ずつ動く文字) | |
| CV-8 | 点が動く複雑な path(GPU で距離場を描く。§12) | |
| EN-v | 「値だけの編集」の RPC(canvas に限らない) | |
| EN-spv | port を持つ pass を事前 compile した SPIR-V で動かす(shader compiler を外した配布) | |
| SG-n(Studio) | viewport の上の gizmo(「値だけの編集」の後)、`ship` の Export、stops の色の Pick、Place の scenario | |

## 10. PSD、動的な物、UI

### 10.1 PSD

- 取り込みは engine の外の tool([design_asset_containers.md](design_asset_containers.md) §3 の方針のまま。psd-tools)。出力は (a) layer ごとの切り詰めた PNG と位置、(b) **canvas の definition**(`canvas.stack` と `canvas.image` の node 群)。§3 が予約した「layout JSON」の実体をこれにする
- 対応: group → 入れ子の stack(pass through の印つき)、blend mode、不透明度、表示、layer mask → `mask`、clipping → `clip`、調整 layer → `canvas.backdrop` + 色の filter(対応する種類だけ。他は diagnostics に出して飛ばす)
- 最初は画素にするもの: 文字、layer style、smart object、vector mask(後で文字と図形は `shape.*` に対応させる)
- 生成物は再取り込みで上書きされる読み取り専用(§5 の規約のまま)。project 側は definition を参照し、layer を名前で指して動かす

### 10.2 動的な物

| 物 | 入れ方 |
|---|---|
| この 2D 機能で作る動く物 | parameter に `value.*` を繋ぐ。layer の変換は `canvas.transform` |
| 3D 描画の動く物、render pass | その pass の `render.image` を `canvas.from_image` で layer にする。上下に静的な layer を積める(§6 の規則で焼かれる) |
| stack を 3D の物にする | `canvas.place`(§7) |
| game code が毎 frame 描く物、実行中に変わる文字列 | 後(§3.2)。item 表を外から書く口として設計する |

### 10.3 UI との融合(方向)

- `pelican.ui` の描画(quad の列)は canvas の item の特別な場合なので、後で canvas の lowering に載せ替えられる。そのとき要るのは: 実行時の layout と組版(§3.2 の unit)、当たり判定、入力
- 当たり判定は IR を CPU で評価して行う(参照描画と同じ code)。`element` の段の式も CPU で評価できるようにしておく(test の正解と共用)。動いている値が要るので、Studio からは engine の RPC で引く
- それまでは font の資産と atlas の規約だけを共有する

## 11. 進め方の裁定の記録

| 点 | 状態 |
|---|---|
| 名前 `pelican.canvas` | 利用者が決定 |
| brush は後回し、画素は取り込み | 利用者が決定 |
| 動的な物の最初の範囲(§10.2 の上 2 行) | 利用者が決定 |
| depth は層ごと + 画素ごと | 利用者が決定 |
| Studio 側(Layers パネルなど)はこの track の session が `develop_v1` 向けに進める | 利用者が決定 |
| 次の優先は文字(font と MSDF) | 利用者が決定 |
| 距離場(MSDF、画像からの SDF)は内製。外部の font / MSDF の library は足さない(font は既に依存にある `stb_truetype` を tool で使う)。画像 → SDF は EDTAA4(Gustavson。縁の距離を subpixel で見積もり、後処理で微調整)とマーチングパラボラ(Felzenszwalb の下側包絡)の併用。EDTAA4 の配布 code は GPL v3+ なので写さず、手法から書き直す | 利用者が決定 |
| build は target を絞る(初回の全体 build の 59 % は誰も回さない test だった) | 利用者が指示(「死活問題」) |
| 3D の画面が鏡像だった件は canvas 側で x を反転せず、engine を v0 で標準の規約に直す(「すべて適切な形にして」。版は上げない) | 利用者が決定(2026-10-02)。develop に着地済み |
| 実行時に canvas の code を持たない(§2-3)。実行中に変わる文字列は UI との融合の段で | Claude の案で進行中。異議があれば戻す |
| 編集中と出荷で同じ道(L2) | 同上 |
| 距離場を中心にする(§12) | 同上 |

## 12. 距離場の作り方

| method | いつ作るか | 向く形 | 段階 |
|---|---|---|---|
| `analytic` | 画素ごとに閉じた式 | 長方形・楕円・線分・少数の 2 次ベジエ。形そのものも動かせる | CV-1 |
| `atlas` | 焼くとき(MSDF + 真の距離) | 文字、動かない複雑な path。変換・塗り・縁取り・glow は動かせる(P1) | CV-4 |
| `field_pass` | 毎 frame、GPU で距離場の image を描く | 点が動く複雑な path | CV-8 |
| `curves` / `mesh` | — | 予約(極端に寄る文字、巨大な vector の絵) | 後 |

- 選び方: 式で書けて葉が K 個以下 → `analytic`。形が `bake` の段 → `atlas`。それ以外 → `field_pass`(入るまでは `canvas.method_unavailable@1`)
- 下流の要求を逆に辿って atlas の距離の幅を決める(縁取り・glow・影の最大の届く範囲。`frame` の段の半径には parameter の `max` を使い、無ければ `canvas.range_unbounded@1`)
- font の資産(`pelican.font`): metrics と kerning、輪郭(2 次ベジエ)、焼いた atlas。ttf → 輪郭は取り込みの tool(`pelican_cli font-import`。既に依存にある `stb_truetype.h` を使う。engine と player は ttf を読まない)。atlas は「文書が使う glyph」から決まる(L2)。日本語は使う文字だけ焼く
- 距離場の生成は内製(`src/project/canvas/`): 輪郭 → MSDF(RGB)+ 真の距離(A)。符号は winding(交差数)で決め、輪郭が重なる漢字に耐える。shader は `clamp(median(r,g,b), a - t, a + t)` で復元し、MSDF の暴れを真の距離で抑える
- 画像の layer の距離場(縁取り・glow・厚みに使う)は、alpha から作る: EDTAA4 の縁の見積もりを種にして、マーチングパラボラで広げる
- 縮小(P5)は実測で決める(第一案: 真の距離の channel に mip)
- `field_pass` の制限(レビューの指摘): 重なる輪郭では内側の辺で距離が 0 になり継ぎ目が出る / 符号と距離を別に持つと補間で縁が跳ぶ / 外側の角の楔を覆う必要がある / 1 枚の image では和しか作れない。Chen らと同じく「重ならない閉じた輪郭」に限り、重ねは `shape.boolean`(距離場どうしの演算)で書く。CV-8 の最初に実験で確かめる

## 13. 検証の指摘と答え

### 13.1 第 1 回(v1 の案に対して、2026-10-02。code に当てた敵対レビュー)

| 指摘 | 答え |
|---|---|
| compose の前に挟むだけでは足りない。core は ValueGraph・PoseGraph・attribute を作者の文書から直接組み立て、editor も candidate を作る。「`#if` は 2 か所」は成り立たない | §1.1 に事実を足し、L1 を「core は汎用の hook 1 つ」に直した。hook は CV-1b。CV-1 は cli から下げて今の player で走らせる(core を変えない) |
| `value.constant` にしても値だけの編集にならない(その道が無い) | §1.1・§3.3 に書いた。EN-v として別に立てる |
| `element` の段: replay の op は GLSL に下げられない、`value.curve` の step は標本にすると合わない、漏れの検査が sink だけ | §3.1 を直した(op の印、key の表と同じ補間式、value 側の出力すべてで検査) |
| `render.scalar` は `target` / `offset` で 1 本の buffer を埋められる(RS-15) | §1.1 を直し、表を `bake` と `frame` に分けた(§5.3) |
| `$` は作者の id にも使える。canvas の image・port の名前が未定義 | §5.4 に名前と `canvas.reserved_id@1` を書いた |
| premultiplied は既存の straight の規約と食い違う | §4.3 に「canvas の image は premultiplied」と、渡す所の扱いを書いた |
| atlas を `edit` で随時足すと L2 が破れる | L2 に「文書だけから決まる関数」を足した |
| `field_pass` が破綻する場合 4 つ | §12 に制限として書いた |
| owner の transform を読む口は無い | §1.1・§7 に書き、EN-4 にした |
| module を外しても残る物(requires の語、Studio の表、型表、catalog の test) | L5 に「`requires` からも外す」。残る物の一覧は CV-5 の受け入れで扱う |
| Studio は ValueGraph を持たないので動く要素の当たり判定ができない | §10.3(engine の RPC で引く。CPU の評価は参照描画と共用) |
| 論文の取り違え(P10 の出所、P2 の「2 桁」、P8 と `rebuild`) | §1.5 と §4.5 を直した |
| `render.material_pass` は複数の pipeline を持つ | §1.1 を直した |
| 編集ごとの lowering と MSDF の費用、pass の上限 128 | lowering は digest で memo する。区切りの数は diagnostics に出す(§5.2) |
| `dist-bake` の段 1 だけでは shader compiler を外せない | §3.4 と EN-spv |

## 付録 A. 例

作者が書く node(角丸の長方形の丸みが曲線で動き、円の線と重なる):

```json
{"id": "clock", "op": "value.input", "parameters": {"name": "scene_time"}, "outputs": ["value"]},
{"id": "pulse", "op": "value.curve", "inputs": {"time": {"node": "clock", "port": "value"}},
 "parameters": {"keys": [{"time": 0, "value": 8}, {"time": 1, "value": 40}]}, "outputs": ["value"]},
{"id": "card", "op": "shape.rect", "parameters": {"size": [320, 180], "center": [640, 360], "corner_radius": 8},
 "inputs": {"corner_radius": {"node": "pulse", "port": "value"}}, "outputs": ["geometry"]},
{"id": "dot", "op": "shape.ellipse", "parameters": {"size": [90, 90], "center": [760, 320]}, "outputs": ["geometry"]},
{"id": "card_fill", "op": "shape.fill", "inputs": {"geometry": {"node": "card", "port": "geometry"}},
 "parameters": {"color": [0.1, 0.4, 0.9, 1]}, "outputs": ["layer"]},
{"id": "dot_line", "op": "shape.stroke", "inputs": {"geometry": {"node": "dot", "port": "geometry"}},
 "parameters": {"color": [1, 1, 1, 1], "width": 4}, "outputs": ["layer"]},
{"id": "hud", "op": "canvas.stack",
 "parameters": {"size": [1280, 720], "layers": [{"input": "card"}, {"input": "dot"}]},
 "inputs": {"card": {"node": "card_fill", "port": "layer"}, "dot": {"node": "dot_line", "port": "layer"}},
 "outputs": ["layer"]},
{"id": "screen", "op": "canvas.output", "inputs": {"layer": {"node": "hud", "port": "layer"}}, "outputs": ["image"]}
```

下げた形: `screen`(`render.raster`、instance 2 個、1 draw)、`screen$canvas_image`、`screen$canvas_items`、`screen$canvas_bake`、`screen$canvas_frame` と `render.scalar` の sink(`corner_radius` だけ `pulse` が毎 frame 書く)。`clock` と `pulse` はそのまま残る。

生成した fragment の骨子:

```glsl
float d;
switch (kind) {
case 0: d = sd_round_rect(p, B(off + 0), F(foff + 0)); break;   // size(bake), corner_radius(frame)
case 1: d = sd_ellipse(p, B(off + 0)); break;
}
float s = 1.0 / length(vec2(dFdx(d), dFdy(d)));                 // 1 単位が何 pixel か
switch (kind) {
case 0: out_color = C(off + 2) * coverage(d * s); break;                              // fill
case 1: out_color = C(off + 2) * coverage((abs(d) - 0.5 * B(off + 6)) * s); break;    // stroke
}
```
