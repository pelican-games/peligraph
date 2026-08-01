# devstudio 方向性: Qt エディタ(埋め込みビューポート + プロトコル駆動)

対象読者: エンジン担当。
ステータス: v1.1(2026-07-08 D0 追加)。**Qt 採用・D0 はユーザー決定済み**、
詳細レビュー前。
前提: コマンド層 stage 2 実装済み・stage 3 GO、`design_game_logic_native.md`、
`design_project_interpretation_layer.md`、既存 devstudio Qt 骨組み(休眠中)。

## 0. 決定(2026-07-07 ユーザー)

**devstudio は Qt で作る。** 決め手 = ビューポートの同一プロセス埋め込み
(エディタ内に本物のエンジンのピクセルを遅延ゼロで出す)。

## 0.5 D0: エディタ特権の禁止(2026-07-08 ユーザー決定)

**標準 UI もツールの 1 つ**: devstudio は公開契約(rpc / pelican_project /
データ形式)の上に建つクライアントであり、それ以上の存在ではない。

1. **標準 UI ができることは、すべて公開面経由でできる** — devstudio だけが
   呼べる裏口 API を作らない。検査はビルドレベルで可能(devstudio は
   pelican_project + rpc クライアントとしてのみリンクし、エンジン内部
   シンボルに触れない)
2. **編集系 rpc が本当の API** — D2/D3 の編集操作(コンポーネント値変更・
   シーン保存・undo 単位)は、エディタ内部関数ではなく **rpc メソッドとして
   先に定義**し、devstudio はそれを呼ぶだけにする(§1-1 の「同じ意味論」を
   「同じ実体」へ格上げ)
3. **ツール自作の 3 つの入口**を公開面として維持する:
   データツール(形式 + pelican_project)/ ライブツール(JSON-RPC。複数
   クライアント同時接続は WebSocket 展開 — **本決定により優先度引き上げ**)/
   組み込みビュー(埋め込みビューポート + rpc)。devstudio はこの 3 つを
   全部使う**リファレンスツール**
4. 敷居下げの最初の一手 = `pelican_rpc.py`(薄い rpc クライアント 1 枚。
   小 WP 候補)。形式の JSON Schema 化は後回しで可
5. 副次効果: エージェントは人間のエディタと完全に同じ操作面を持つ /
   エディタのテストが rpc 結合テストに還元される(UI テストの沼を回避)

## 0.6 プロセス境界: 別プロセス + ウィンドウ再親付け(2026-08-01 ユーザー決定)

§0 の「同一プロセス埋め込み」と §0.5 D0 の「pelican_project + rpc クライアントとして
**のみ**リンクする」は、1 日違いで書かれたまま突き合わされていなかった。エンジンは
`pelican_core` にあるので、同一プロセスで本物のピクセルを出すには core をリンクするしかなく、
D0 の「ビルドレベルで検査可能」は成立しなくなる。**D0 を優先し、§0 の手段を差し替える。**

**決定**: devstudio はエンジンを**子プロセス**として起動し、そのウィンドウを Qt 側の
コンテナへ再親付けする。

**遅延は増えない。** 子プロセスは自分のスワップチェーンへ描き、OS のコンポジタが合成する。
読み戻しもコピーもプロセス間転送も挟まらず、エンジンは単体起動時と同じ経路で present する。
§0 が避けたかったのは「テクスチャへ描く → 読み戻す → 送る → 再アップロード」という
素朴な別プロセス実装であって、再親付けはそれを回避したまま別プロセスでいられる。
プロセス間を通るのは選択・編集・カメラといった**制御**だけで、これらは人間の操作速度の事象である。

**この決定が保つもの**:

- D0 のビルドレベル検査がそのまま生きる(「公開シンボル集合」を別途定義・維持する必要が無い)
- エンジンが落ちてもエディタと未保存の作業が生き残る
- Qt のイベントループと appflow ループの同居問題が発生しない

**代償として引き受けるもの**: Windows のプロセスをまたぐウィンドウ再親付けには、
入力フォーカス・キーボード経路・DPI・z 順に既知の癖がある。ここは D1 の実測対象
(§4-1)であり、詰まった場合に作り直しになるのは**ビューポート結線だけ**である —
アウトライナ・ドッキング・rpc 経由の編集はそのまま同一プロセス版へ移せる。

§0 の「決め手 = 同一プロセス埋め込み」は「決め手 = **ネイティブウィンドウ合成による
ゼロコピーのビューポート**」と読み替える。Qt 採用の理由は変わらない。

## 1. アーキテクチャ原則(Qt でも守るもの)

1. **編集操作は公開プロトコル(rpc メソッド)そのものを通す**。§0.6 で別プロセスに
   決まったため、「同じ意味論の内部 API」ではなく **同じ実体**になった
   (§0.5 の 2 がそのまま満たされる)。`set_time` / `update_transforms` /
   `get_frame_plan` 等を rpc として呼ぶ。理由: (a) エージェント・DCC・web と
   エディタが同じ語彙で動く (b) undo/redo = 操作の逆適用として自然に設計できる
   (c) エディタで出来ることは自動化でも出来る、が保たれる
2. **解釈は pelican_project(解釈レイヤ)を直接リンク** — シーン/アセット/
   manifest の表示・検証はエンジン起動なしで可能に(ライブラリ分離の最初の
   本格的な受益者。未実施のターゲット分離を先行条件にする)
3. ビューポート = エンジンの Vulkan スワップチェーンをネイティブウィンドウ
   ハンドルで Qt 内に埋め込み(WSI 統合)。headless/rpc 経路には影響を与えない
4. UI の見た目は web デザインシステムと**語彙を揃える**(tokens の対応表を作る。
   実装は共有しない — 二重投資はビューポートの価値で回収する判断済み)

### 1.1 UI シェルの所有境界(WP249)

devstudio のシェルは **Qt Widgets** で作る。`QMainWindow` がメニュー、ステータス、中央
workspace、`QDockWidget`、タブ化、レイアウト保存・復元を所有する。これは
`QMainWindow::saveState()` / `restoreState()` が dock の object name と配置を一体で扱うためで、
標準のドッキング機構を持たない QML に同じ責務を二重実装しない。

QML を使う場合は `QQuickWidget` 等に載せた **葉のパネル内部だけ**に限る。QML の root が
window shell、dock、レイアウトメニュー、プリセット永続化を所有してはならない。現在の D1
シェル自体は Widgets のみで、将来 QML パネルが追加されてもこの所有境界は変えない。

## 2. エディタが要求するエンジン機能(設計はエディタ都合で駆動)

| 機能 | 実装形 | 備考 |
|------|--------|------|
| ピッキング(視覚) | **ID バッファパス = render feature**(`engine://features/picking.json`)+ 読み出し | パージ可能・エディタ以外(デバッグ)にも使える |
| ピッキング(論理) | 物理クエリ(raycast)— 別途必須決定済み | 視覚と論理の両方を持つ |
| ギズモ | debug_draw(実装済み)の上にハンドル描画 + ドラッグ → update_transforms | |
| アセットホットリロード拡張 | シェーダ以外(テクスチャ・モデル・シーン)の監視再読込 | 編集ループの核 |
| シーン保存 round-trip | rpc 変更の蓄積 → pelican.scene v1 へ書き戻し | レガシー形式受理の削除条件(scene 文書 未決 4)がここで満ちる |

## 3. 段階(D 系)

| 段階 | 内容 | 依存 |
|------|------|------|
| D1 | プロジェクトを開く + 埋め込みビューポートで実エンジン表示 + アウトライナ(読み取り専用、pelican_project 直リンク) | 解釈レイヤのターゲット分離 |
| D2 | 選択(ID バッファ + raycast)・ギズモ・プロパティ編集(stage 3 の update_transforms 意味論) | D1, stage 3, 物理クエリ |
| D3 | 保存 round-trip + undo/redo + アセットホットリロード拡張 | D2 |

## 4. 未決事項

1. **解決済み(WP251、2026-08-01)**: Qt 6.10.3 と、§0.6 の別プロセス
   `pelican_player` のネイティブ HWND 再親付けを採用する。QVulkanWindow による
   同一プロセス化は D0 のリンク境界を壊すため採らない。実測結果と制約は §4.1。
2. エディタ内ビルド連携(G1b の PELICAN_PROJECT ビルドをエディタから叩くか)
3. web デザインシステムとのトークン対応表の管理場所
4. 複数プロジェクト同時編集(v1 はしない)

### 4.1 WP251 ネイティブ HWND 埋め込み実測(2026-08-01)

実測環境は Windows NT 10.0.26200.0、Qt 6.10.3、2560x1440 の 96 DPI モニターである。
`dist_debug/pelican_studio.exe` から `build/src/player/Debug/pelican_player.exe` を起動し、
`projects/sprite_demo` を表示した。モックウィンドウではなく実際の Studio / GLFW HWND に
クリック・キーの Windows メッセージを通し、focus HWND/PID、player の入力収録、
`GetDpiForWindow`、`WindowFromPoint`、client rect を記録した。クリックは物理 HID ではなく
実 HWND の WndProc と GLFW の mouse capture を通す自動操作である。強制終了は別の実 player
プロセスに `TerminateProcess` を行った。

| 項目 | 実測値と観察 | 判定 |
|------|--------------|------|
| 入力フォーカス | Studio PID 46484 から player PID 55364 の描画面をクリックすると focus PID は 55364、Qt の Output をクリックすると 46484、player を再クリックすると 55364 へ移った。GLFW の外部子 HWND は Qt の mouse event を覆うため、埋め込み中だけ 5 ms 間隔で child thread の mouse capture を検出して focus を渡す。 | 実用可。player と Qt の双方向移動を確認。 |
| キーボード経路 | player focus 中に player HWND へ送った入力の収録は `F1` down/up、`D` down/文字 `d`/up、`LeftControl` down・`S` down/文字 `s`・up・Control up を各 1 系列記録した。Qt focus へ戻して Studio HWND へ送った `Ctrl+S` では player event は 0 件増加だった。現時点の Studio shell には `Ctrl+S` QAction 自体がなく、保存 shortcut との機能競合はまだ発生しない。 | **条件付き**。実 HWND 間の target 分離は確認したが、物理 HID と将来の global shortcut の arbitration は未実測。global shortcut 追加時に engine focus 中の扱いを明示する。 |
| DPI | 通常時は host/player とも 96 DPI、per-monitor aware、775x482 px で矩形一致。`QT_SCALE_FACTOR=1.5` の Qt 150% 経路では host/player とも 1193x722 px で矩形一致した。通常時比は幅 1.539、高さ 1.498(文字・dock の再レイアウトを含む)。ただし OS が返す DPI は両 process とも 96 のままである。 | **条件付き**。DPR による再親付け寸法は通ったが、実 144-DPI モニターおよび monitor 間移動はこの機材では未実測。player 内容の実 DPI scale の合否にはしない。 |
| z 順 | player は host の直接の `WS_CHILD` で top child かつ host と矩形一致し、隣接する Project / Inspector / Output dock へはみ出さなかった。Layout の実サブメニューを player と画面座標 `280,143-383,157` で重ねた点の `WindowFromPoint` は Studio PID 46484 を返し、画面上もメニューが前面だった。 | 実用可。dock の overdraw と menu が native child の後ろへ沈む症状なし。 |
| リサイズ追従 | Studio を 1296x839 から 1016x659 へ変更し、viewport は 775x482 から 770x302 px へ追従した。最終 build の 2 ms polling では host 変化開始 19 ms、最後の不一致 22 ms、20 ms 連続一致を 45 ms で確認。不一致は 2 sample、最大差 5x180 px。同手順の直前 run は 20/29/52 ms、4 sample だった。さらに 300 ms 後には host/player の矩形が完全一致した。scene 背景も host 背景も黒いため、色だけによる黒帯の識別はできない。 | **条件付きで実用可**。反復実測で最大 29 ms の過渡的な寸法差は存在し、過渡的な黒帯なしとは断定しない。独自 swapchain 再生成はなく、`SetWindowPos` が既存 GLFW framebuffer callback と WP215〜217 の epoch 経路へ入力される。 |
| 子プロセス強制終了 | player を exit code 251 で強制終了し、終了検知後 2 秒でも Studio の process と window は生存。viewport は黒背景と `Restart Engine` を表示した。 | 実用可。engine crash は editor crash に波及しない。 |

以上から Windows ネイティブ HWND 方式は D1 のビューポート結線として採用を維持する。
実 144-DPI monitor の未実測は方式決定を覆す failure とは扱わないが、対応機材での
per-monitor DPI 移動試験を将来の platform matrix に残す。再親付け後も Studio と player の
DPI awareness が揃わない構成は許容しない。
