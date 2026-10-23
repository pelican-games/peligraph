# node の体と畳み方 —— 種類ごとの見た目、3 段の折りたたみ、layer stack の node

対象読者: Studio 担当。
ステータス: 設計(2026-10-23。利用者の要望から。WP は NB-1〜NB-3)。
前提: [design_canvas_2d.md](design_canvas_2d.md)(canvas の語彙と Layers dock、SG-1〜3)、[design_graph_layers.md](design_graph_layers.md)(Graph が入口、領域の filter)、
[design_studio_node_canvas_kernel.md](design_studio_node_canvas_kernel.md)(操作の層の一本化。未着手)。

## 0. 要望(2026-10-23 利用者)

- node の GUI はいろいろな形があってよい。邪魔なときはシンプルな形に畳めることが重要
- layer の node はペイントアプリのように、node に layer を足したり消したりしやすく(参考: Black Ink 2020 の Layer editor。node の中に layer の行が並び、行ごとに目・thumbnail・名前、入力の線は行に刺さる)
- canvas の node は「上の入力が上に合成される」ことが分かるように、layer の GUI を node の中にきちんと入れる

## 1. 今(develop_v1 `f06a04f53`)

| 項目 | 今 | 場所 |
|---|---|---|
| node の描き方 | 1 種類。見出しの帯 + output の行 + input の行。行の高さは全部 21、幅は 196〜340 | `view/graphcanvasdrawing.cpp` の `addNode` / `addNodeHeader` / `addNodeRows` |
| 畳む | socket の行を全部隠す 1 段(三角)。状態は session の `session_collapsed_` だけで、文書に残らない | `view/graphcanvaswidget.cpp:654-678` |
| canvas の見え方 | Graph は最初 Render 以外の領域を隠すので、canvas の node は `← canvas (29)` の印に縮む | `model/graphcanvasmodel.cpp` の `domainStubs()` |
| layer の編集 | 別の Layers dock。足す / 消す / ±1 の並べ替え / 目 / 不透明度 / blend は Qt 抜きの関数が op の列を作る | `model/graphcanvaslayerstack.*`、`view/graphcanvaslayerspanel.cpp` |
| stack の socket | すでに layer ごと(`layers[].input`、`<input>.opacity`、mask、`+ layer`)。ただし行は名前だけで、どの layer が上かが見えない | `model/graphcanvaslayerstack.cpp:166-211` |
| 位置の保存 | 文書の `layout.nodes.<id>.{x, y}`(`set_layout`、undo あり)。engine は node の object の他の鍵を通す | `graphcomposition.cpp:1825-1874` |

## 2. 決定

### 2.1 node の体(body)を種類ごとに

- node の中身の描き方を「体」として差し替えられるようにする。体は op で選ぶ(無ければ今の行の体)。体が決めるのは 行の並び・行の高さ・行の中の部品・socket の位置・畳んだときの形
- 体は Qt の描画の部品で、文書の形は変えない。編集は今までどおり model の関数が作る op の列(`submitEdit`)で、体は呼ぶだけ
- 線(edge)の端は今の仕組みのまま(socket item の data で探す)。体が置く socket にも同じ data を付ける
- 最初の体は 3 つ: 既定(今の行)/ layer stack(`canvas.stack`)/ カード(canvas の源と filter: `canvas.image`、`canvas.color`、`shape.*`、`filter.*`、`canvas.from_image`)

### 2.2 3 段の折りたたみ

| 段 | 既定の体 | layer stack | カード |
|---|---|---|---|
| 開く(open) | 全部の行 | 合成の thumbnail + layer の行 + 足す / 消すの帯 | thumbnail + socket の行 |
| 小さい(compact) | 線の繋がった socket の行だけ | 合成の thumbnail + 行の名前だけの細い列(socket は残す) | 見出し + thumbnail |
| 最小(minimal) | 見出しだけ(今の畳んだ形。線は左右の点に集まる) | 同じ | 同じ |

- 見出しの三角を押すと open → compact → minimal → open と回る。右クリックの menu に 3 つを名前で並べる。複数選んで一度に変えられる
- **段は文書の `layout.nodes.<id>.view`(`"compact"` / `"minimal"`、open は書かない)に保存**し、`set_layout` で送る(undo できる、保存・再読込で戻る)。今の session の表は使わない。engine は layout を読まないので形式の版は上げない(Studio の読み書きだけ足す)

### 2.3 layer stack の node(`canvas.stack`)

```
┌ hud ─────────────────── canvas.stack ▾ ┐
│ [合成の thumbnail 64px]           layer ●│  ← output
├──────────────────────────────────────┤
│● 👁 [▣] Title        Screen 80%  f  ›  │  ← 上 = 手前
│● 👁 [▣] Frame        mask             │
│● 👁 [▣] 3D                             │
│● 👁 [▣] Background                     │  ← 下 = 奥
├──────────────────────────────────────┤
│  ＋ ▾   🗑                    ↑ 手前に重なる │
└──────────────────────────────────────┘
```

- 行は `layers[]` の逆順(上の行ほど手前)。帯の右下に「↑ 手前に重なる」の小さな注記
- 行の左端に その layer の入力の socket(port は今の `layers[].input`)。線が行に刺さるので、どの入力がどの順で重なるかが線の位置で分かる。`<input>.opacity` と mask の socket は、線が繋がっているときだけ行の中に小さく出す
- 行の部品: 目(`visible`)/ thumbnail(32px)/ 名前(double-click で名前を変える)/ blend の略称(normal 以外)/ 不透明度(100 % 以外。drag で変えられる)/ `f`(filter あり)/ `›`(group や definition の中に入る = 今の scope の切り替え)
- 足す: 帯の **＋** の menu(Rect / Ellipse / Text… / Image… / Group / Adjustment ▸)。選んだ行の上に入る。線を待つ「空の行」は作らない(canvas.stack は繋がっていない入力を `canvas.layer_input_missing@1` で断る。既にある出力は行の間へ線を落として入れる)
- 消す: 帯の 🗑 か Delete(選んだ行)。行だけを消す(源の node は今の Layers dock と同じ規則)
- 並べ替え: 行を drag して任意の位置へ(新しい関数 `graphCanvasMoveLayerToOps(from, to)`。今の ±1 の関数はそれの特別な場合)
- 外の node の output の線を行と行の間に落とすと、その位置に layer が入る(新しい関数 `graphCanvasInsertLayerOps(at, source)`。今の `+ layer` は一番上に入る特別な場合)
- 行を選ぶと Layers dock の選んだ行も同じになる(dock は位置・filter の数値などの細かい欄の場所。node は構造と順番の場所)
- PSD の取り込みの読み取り専用の stack では、足す / 消す / 並べ替えを出さない(dock と同じ規則)

### 2.4 カード(canvas の源と filter)

- 見出しの下に thumbnail(その node の出力の layer を描いたもの)。socket の行はその下
- filter のカードは filter の名前と主な値 1 つ(例 `Blur 4 px`)を見出しの横に

### 2.5 thumbnail

- CPU の参照描画(`src/project/canvas/canvasreference.*`)で Studio の中で作る。engine の描画を待たない
- layer ごと: その layer だけを読む一時の stack と output を足した文書を作って描く(関数を足す)。合成: stack の output をそのまま
- 線で決まる値は、Live の値があればそれ、無ければ rest の値(canvas の lowering が使うもの)
- 画像は Studio が decode して渡す。文字は lowering が font を読む
- 64×64 以下。UI の thread の外で作り、文書の該当部分の digest で cache。作れないとき(font が無い等)は灰色の枠と理由の tooltip

### 2.6 canvas の領域の見せ方

- 文書に canvas の node があるときは、Graph の `Show:` の既定に Canvas を入れる(`← canvas (29)` に縮めない)。利用者が外せば今までどおり印に縮む

## 3. WP

| WP | 中身 | 受け入れ |
|---|---|---|
| **NB-1** | 体を差し替える口(既定の体を今の描き方のまま移す)、3 段の折りたたみ(三角で回す、menu、複数選択)、段を `layout.nodes.<id>.view` に保存(undo・保存・再読込)、canvas がある文書の `Show:` の既定 | 既定の体の node の絵が今と同じ(既存の scene test が緑)。段が undo で戻り、保存して開き直しても残る。compact で繋がった socket だけ残り、線がそこに付いたまま |
| **NB-2** | layer stack の体: 行(逆順、行ごとの socket、目・名前・blend・不透明度・f・›)、＋ / 🗑 / drag の並べ替え / 行の間への drop、dock の選択と同期 | 行の順が手前から奥。node の上の操作が dock の操作と同じ op を送る(model の test)。drag で任意の位置へ 1 回の編集で動く。scenario 1 本(Blank から canvas を作り、node の上で足す・消す・並べ替える) |
| **NB-3** | thumbnail(CPU の参照描画、thread の外、cache)と カードの体 | 1 枚の stack の各行の thumbnail が参照描画と一致。重い文書でも UI が止まらない(描く間は灰色) |

- NB-2 と NB-3 は NB-1 の口の上に載る。NB-3 の thumbnail が無い間、NB-2 の行の thumbnail の枠は空のまま
- 操作の層の一本化([design_studio_node_canvas_kernel.md](design_studio_node_canvas_kernel.md))は未着手。NB-2 の行の上の mouse の処理は今の `graphcanvaswidget.cpp` の振り分けに足す。一本化の WP が始まるときは、行の操作もその adapter に移す

## 4. 未決

- 細かい欄(位置、filter の数値)を node の中に開くか(今は dock)。NB-2 を触ってもらってから決める
- compact の既定の体を「繋がった socket だけ」にするか「行を 1 行の要約にする」か
- Frame Plan の canvas にも同じ体を使うか(Frame Plan は pass の node なので、まず Graph だけ)
