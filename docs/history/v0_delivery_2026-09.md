# v0 の着地・検証履歴（2026-09-06〜18）

2026-09-22 に [v1 計画](../v1_development_plan.md)から移した記録。
内容は当時の観測・進行中の判断であり、現在の作業指示や未完了一覧ではない。
同じ段落内の「実装中」「着地済み」は記録時点の違いを残している。
現行の状態・再検証は [v0 凍結一覧](../v0_freeze.md)、WP の仕様・受け入れ記録は
[実装台帳](../implementation_plan.md)を参照する。

以下の旧ブランチ・端末パス・commit 権限・agent 数は当時のセッション情報であり、
新しい作業の設定や権限として引き継がない。

## 再開時の入口

- 作業ブランチは `codex/render-target-runtime-slice`。RT・Studio基盤を `cb5c4c26`、embed再生成修正を `d0956f95`、文書・運用整理を `19026575` に統合。再開後、material passのray query設定を `3b76a31a`、入出力編集を `e2a6acb0`、raster接続編集を `db49f4b6`、rasterのshader・draw・state編集を `c5daf0e1`、triangle worldモードのGUI・保存を `1cc74798` に統合。続いて異transform AABBのworld結合を実装した（73865d07）。AABBのgeometry/world切替もScene AS画面へ接続した（変換record宣言済みlayout v3が対象）。
- 本書の現況と残件を読み、着手する項目だけ台帳の該当WP・最終差分と `design_raytracing.md` §4-P / §11へ進む。台帳全体や完了履歴を毎回読み直さない。
- 利用者の最新指示が旧運用記録に優先する。2026-09-06に統合担当としてのcommitを許可済み。実装・影響範囲の確認・記録が揃った単位でcommitし、個別の実装workerは統合担当へ渡す。
- 同時agentは主担当を含め最大2、fastは使わない。`<作業ディレクトリ>/wp336`、`projects/showcase/`、無関係な `.claude/`・`out/` は触らない。モデル・thinking effort・プラグイン設定は利用者の選択を尊重する。
- 開始時にbranch・差分・対象jobの状態を確認する。完了したログや状態ファイルだけを根拠に、処理が今も動いていると判断しない。

## 当時の進捗と残件

進捗(置換更新): **基準は 2026-09-17 `6f5ecffc` で緑**。**AG-0 着地 `c9085d6f`**、**AG-1a 着地 `ee751752`**。AG-1b は設計レビュー 2 巡で「一本化」を v0 では撤回し、v3 = pose graph 経路の parity(loop / speed / start_time、位相ずれ、状態移送)+ owner 単位の排他 + OFF build の fail-fast に確定(event は v0 の後)。**P-1 着地 `3611e390`**。**AG-1b 着地 `129eb218`**(全数 1886 緑、RT6 の間欠 1 件は直列緑)。**P-2a-1a 着地 `60e95f53`**(W1 の赤は RT6 間欠のみ、記録して squash)。**P-2a-1b 着地 `6e04c7c8`**(gate は build + 非 GPU のみ。利用者指示で「進めて後でテスト」に切替、全数は P-2 を閉じるとき)、**P-2a-1c 着地 `e3a2d3d0`**(materialize 後の parameter 編集を含む)。P-2a-2(runtime 値)実装中。**O-1 着地 `fcbe1a98`**(`--start-time` の replay、frame 等価を bytes で確認)。**AG-2 着地 `37cc55ba`**(root motion、10 mode GPU test)。**P-2a-4a + P-2a-3 着地 `7e983ed9`**(graph root の `rendering` 節を authoring 全面が読み書き、component の export / import)。**P-2a-2 着地 `340f8537`**(`binding: buffer`、共通グラフから毎 frame)。P-2 の (a) は閉じた。**AG-3a 着地 `32d9a20b`**、**X-1 着地 `64eae566`**、**P-2b 着地 `400820f0`**(posegraph_demo、`requires`)。**P-2a-4b 着地 `e1c326c1`**(example は graph root)。**R-3 着地 `be4c60a4`**(`docs/v0_freeze.md`)。**R-1 着地 `96d5e1d8`**。**engine 側の v0 段は全部着地し、全数(configure + build + ctest -j4、GPU 込み、source `450f2866`)が 1947 / 1947 緑**(RT6 の間欠は R1-2 で消えた)。利用者の裁定(2026-09-17): **Studio の改良(N 系 / AG-3b、別 session)も v0** で、tag は Studio 側が入って利用者が閉じるまで打たない。以後はゆったり磨く(残りの 1 枚は下)。P-2a-4b と R 群は起票済み(`aac1295c`)、**R1-2 着地 `cc95a732`**(committed marker。704 round で新経路 0 件)。全数は P-2 を閉じるときに 1 回(利用者指示)。**GUI 系(N 群、別セッション)**: N-1 共通グラフの読み取りキャンバス着地 `edafc41d`(Frame Plan dock の Graph tab、`pelican_studio --project --tab`)。利用者レビュー 1 巡目から **N-1b 着地 `6c47a8a4`**(見出し + ソケット欄、型ごとのソケット形状は JSON 既定表 → module 宣言優先)。**N-2 着地 `f2eccbfd`**(折り畳み / 辺の接続・切断 / 値のその場編集 / palette 追加・削除 / ラベル / 定義 scope の編集 op と `set_node_label`)。次は利用者レビュー 3 巡目と AG-3b(定義内の値の重ね = `get_graph_node_values`、時間軸 view)。AG-3 は AG-3a(engine、別セッション)/ AG-3b(Studio、この列)に分割。P-2 は設計レビュー 2 巡で打ち切り、v3 = 1a(anchor 明示と撤去)/ 1b(Bloom の feature 化と oracle の付け替え)/ 1c(define 化と materialize 後の編集)/ P-2a-2(runtime 値)+ 送り(共有部品の公開、出荷 project の `pelican.graph` root、posegraph_demo、外部定義参照、hdr on の Bloom)。まず 1a。 **→ 着地 `ce5e96c0`**(装着 → 編集 → 観測 → 保存 → 再読込 のシナリオ `graph_observe_roundtrip` が実 engine で緑)**N-7 着地 `434c4534`**(パネルの文字)。**N-6 着地 `5bd02967`**(reroute / tie / 配置の永続化 `layout` + `set_layout`)。Studio 側の v0 一覧は backlog を残して閉じた。**N-8 着地 `8e48473f`**(Frame Plan の canvas 編集: 接続 / 切断 / 帯 / 追加 / 削除 / 配置の永続化。legacy root 4 project は配置を保存できない = 磨きの一覧)。**Q-2 着地 `bf2776fc`**(応答性: idle RPC 56 → 0.5/秒、frame plan 1 回 parse、canvas の cache。Debug build が最大要因 = Release 版 launcher `<作業ディレクトリ>/studio_rel.cmd`。embed の固まりは診断のみ → Q-4)。**Q-3 着地 `c90fde90`**(帯の editor が窓の瞬きで閉じて古い値を保存する欠陥。N-2 由来、シナリオはこの機械で一度も緑でなかった)。
**説明書とコード解説の更新着地 `45278126`**(2026-09-17 夜、利用者依頼「今ある機能の説明書の更新とコード解説の更新」。manual 00〜04 / 06 / 08 / 10〜14、guide 01〜11 + README、`docs/v0_freeze.md`。敵対 fact-check 2 本(manual 18 件 + 矛盾 7、guide 32 件 + link 21)を全件適用、doclink 4760 / 0 unresolved。Q-1b `cd9e243f`・N-5 `6a26eef3` まで反映)。

#### v0 の残り(2026-09-17 夜、engine 側 + Studio 側の 1 枚。置換更新)

## Studio 側(別 session pelican2-3e が所有。**2026-09-17 夜に全部着地**)
- N-3 `d8d7387d` / Q-1 `c026002d` / Q-1b `cd9e243f` / N-5 `6a26eef3` / N-4 `142a34fc` / AG-3b `ce5e96c0`(各 spec は `<作業ディレクトリ>/*_spec.md`、完了記録は台帳)
- backlog(未予定、利用者が決める): parameter_dependent な render.* op の palette form / seed constant の自動掃除 / 必須入力の disconnect UX / graph 編集の undo / definition の create・delete・copy(P-2b-2)/ Features dock の export・import UI(P-2a-3 の RPC)と package feature / node の text elision / Studio の exit crash(139)/ animation sink の e2e / scrub の GPU coverage
- 既知の穴: Studio が raytracing_demo を開けない

## engine 側(v0 の段は全部着地。残りは磨き = 記録済みの穴。`docs/v0_freeze.md` §4 と同じ)
- `test/fixtures/devstudio/example_frame_plan.json` の再凍結(Studio 入り build で数値 assert を追随。P-2a-4b の穴)
- `requires` を読まない configure 時の corpus game logic 目標(OFF build + `PELICAN_TEST_PROJECTS_DIR`)
- GPU lease log の掃除(`gpu_lease_run.cpp:94`、`%TEMP%` に数千個)
- pose 側 snapshot(`get_graph_node_values` は rendering 側の値。event / trigger 駆動で pose 側とずれ得る)
- owner id の永続化(位置依存 id、不在 id は無音 inactive → R-1 で 1 行警告)
- package: 自前 shader(X-2)、pass / RT 名の namespace、`ProjectEnvelope` の `packages`、`get_graph_catalog` の package
- hdr on の Bloom / Bloom shader の define prefix と namespace 付き派生
- component 経路(component_graph / component_clip)の `get_status.animation` test
- SceneLoader の二重 load hang / 放棄 scene transaction の teardown hang / `looks_mutating` gate / signed-zero の AS identity
- 複合 undo(v1)/ 代表ゲームの物理・音・save(v1)/ 外部定義参照(P-2b-2)/ read-only render(O-1 の送り)/ 評価分類の強制(recorded の宣言者と同時)
- codex の敵対コードレビュー(limit 解除後、CLI 更新が要る): AG-0 / AG-1a / AG-1b / P-1 / R0 + 今日の 14 本


## 共通グラフへの接続経緯

### 全体グラフを正本にする（2026-09-07、直近の方針）

コンポーネント固有のグラフを別系統として増やす進め方を見直す。
[全体グラフとコンポーネント表示の共通化](../design_graph_composition.md)を参照し、
同じ編集モデルのサブグラフinstanceとして保存し、Studioだけ表示を分ける。
既存CompiledLogicalRenderGraphは描画用の派生結果であり、編集の正本に転用しない。
現在のValueGraph・bindingsとanimation componentは実装済みだが共通化は未了。
project/graphcompositionへ共通形式の定義/instance/port展開を追加し、ValueGraphの
数値backendへ接続した。保存は元の共通文書を保ち、生成bindingは派生結果とする。
共通portは既存LogicalTypeRegistryへ接続し、数値operationと描画/音声scalar sinkの
入力契約を照合する。既存render authoring全体/scene/Studioとの統合は未了。
render.buffer/render.computeの資源・型付き入出力を既存compose入口へ接続した。
計算結果portへの接続から完了依存を生成する。混在文書の実行分割と、bufferのvalue入力を
通じた数値→runtime_f32→GPU compute消費も接続し、実結果を読み戻して確認した。
現在の転送境界は1個のfloat用。数値処理を描画世代に保持し、通常Rendererで自動評価する
経路とscene読込み時刻の供給を接続した。更新失敗時の旧世代維持も実GPUで確認済み。
instanceの保存parameter、定義default、evaluate/publishへの一時overrideは実装した。
instance.ownerによるauthoring objectとの対応と、GameContextからのparameter変更を接続した。
削除/scene再読込み時のoverride破棄、別グラフの独立性も通常Rendererで確認済み。
ownerの生存状態に沿った数値評価・publish・依存computeの停止と、scene再読込みでの再開を
通常Rendererへ接続した。render.imageとcomputeのstorage/sampled画像portも接続済み。
個体別EvaluationStateとvalue.trigger_clockの開始/一時停止/再開/resetも通常Rendererへ接続した。
型付き再生イベントのsource/時計consumerと時刻付きキューも通常Rendererへ接続した。
audio.playbackへ開始/pause/resume/resetを接続し、個体・scene・Graphの寿命に沿ったvoice解放を
通常Renderer経由で確認した（音声はnull backend）。typed voice portからvolume/pitchへの接続と、
新規再生/再開始したframeの値反映も実装した。raster/fullscreen passの共通nodeから既存パスへの
変換とCPUコンパイルに加え、GPU画素出力とowner不在時の描画scope停止を確認した。
render.attributeのvertex/instance/material/index domainを型identityとして照合し、既存bufferとshader
accessorへ渡す入口も追加した。countからstd430サイズとraster描画数を導出する接続も実装した。
GltfLoaderの属性抽出を共通graphのsource参照・count解決・候補buffer uploadへ接続した。
GPUで複数頂点の属性値とcountを読み、元のsource参照の保存と失敗候補による旧世代維持を確認した。
属性の実ファイル依存を変更通知へ接続し、同じGLBや`.gltf`の外部bufferをモデルとgraph属性が共有する場合の
共同更新を実装した。候補失敗時は両方を維持し、buffer参照先の切替に合わせて依存も更新する。
glTFインデックスも共通sourceから読出し、draw.index_sourceからnative indexed drawへ接続した。
候補世代のbufferを保持し、GPUで後半三角形の画素・owner停止/再開を確認した。
rasterの頂点shaderではvertex/instance属性の引数なしloadを追加し、glTFの頂点と
first_instance=1のcompute生成属性を使ったGPU画素出力を確認した。
遅延RT公開時の共有モデル検証、変形後属性は残件。
モーフ差分はsource.morph_targetから属性portへ読み出せる。通常computeでのウェイト合成と
不正target時の旧世代維持をGPU確認済み。所有者付きvalue.morph_weightから個体の現在ウェイトを
読み、通常computeへの合成・個体削除時の停止・再生成後の再接続を確認した。
この接続で区切る。スキニング済みGPU属性の参照とアニメーショングラフ全体の共通化は別工程。
動的描画nodeは現状scopeを分離して実行し、
同じowner条件のnode間融合と共通history画像のpreview対応は残件。
停止時もGPU資源は描画世代が保持する。動的instance装着や個体単位の資源回収は未了。
sceneごとのgraph選択と、既存scalar音声bindingへのhandle/任意外部inputの自動供給はまだない。
画像pass、feature自体の共通instance化、時計以外の状態/イベント境界、Studio統合は残件。
専用ValueGraphComponentや新しいkind分岐の追加は先行しない。
アトリビュート生成/付与・固有shaderもこの共通表現へ含める。material反復の
保存粒度は未確定。対象条件/反復本体を定義として保存し、列挙結果を派生にする案を
設計へ記録したが、汎用foreach文法や全件展開保存を先行実装しない。

### グラフ間のデータ接続（2026-09-07 利用者要望、未実装）

アニメーションと物理をノードで組み合わせ、その出力をレンダリンググラフの入力へ接続できるようにする。専用エフェクトごとの隠れた転送処理ではなく、既存resource port/bufferの仕組みへ型付きデータとして公開する。描画・compute・RTの各consumerが同じ出力を参照でき、保存・再読込後も接続を保持することを完成条件とする。

- アニメーション: 評価済みの数値カーブ・姿勢/行列を公開する。キーフレーム自体をGPUで補間したい用途には、時間・値・補間方式を持つカーブ資産の入力も区別して扱う。
- 数値接続の保存: ValueGraph::Graphでpelican.value_graph文書のinput/constant/curve/add/multiply/playbackと名前付き配列出力を保存・評価する経路を追加。playbackの使用区間・duration・speed・delay・sample_intervalは定数/他ノードを接続でき、wrapも保存する。bindingsでrender_buffer/sound_volume/sound_pitchへの出力先を保存し、publishで反映できる。音声は論理名から実行中handleを渡す。時刻は外部入力で、内部時計を持たない。sceneへの自動装着、triggerノード、pose等の型、GUIは未実装。
- 描画への数値入力: bufferの`host_source: "runtime_f32"`とGameContext::setRenderValuesでfloat配列を公開する経路を追加。既存storage buffer portをconsumerへ接続できる。CPU側でコピーし、rendererの既存host更新位置で転送する。未設定はゼロ、公開後は次の更新まで保持し、サイズは宣言全体との一致を要求する。数値グラフのbindingsからこの経路への接続保存も追加。sceneからの自動実行とGUI配線は残件。
- カーブの設定は形状と再生を分離する。使用区間・再生時間・速度/逆再生・ループ/往復、値の倍率/オフセット、開始トリガー/遅延/再トリガー、連続/固定間隔/LUT解像度を組み合わせる。公開C++のScalarCurve・sampleCurve・CurveTriggerClockで基本評価を実装。数値グラフで形状とadd/multiply・playbackによる変換の接続保存まで追加。triggerノード・LUT公開・GUIと複数トリガーの重ね合わせは未実装であり、一括機能を提供する場合も各処理へ分解できる構成を保つ。
- 物理: transform・速度等の状態と、接触等のイベント列を区別する。イベントを描画フレームごとに重複消費せず、固定stepと描画補間の関係を明示する。現在未実装の剛体シミュレーションを実装済みとはしない。
- 音声（追加要望）: グラフのイベントから再生/停止、連続値から音量/ピッチ等を操作する。GameContextの既存playSound/stopSound/setBusVolumeに、個別voiceのsetSoundVolume/setSoundPitchとsoundStateを追加。音量は個別値×バス×masterを維持する。再生位置はminiaudioのsource cursor秒であり、デバイスから聞こえる時刻ではない。null backendは時計なしを返す。新しい変更APIはupdate thread向けでscene effect staging中はfalseを返す。数値グラフのbindingsからvolume/pitchへ接続でき、音声出力は1要素を要求する。stage内の設定予約、再生/停止イベントとその重複消費防止は未実装。
- 境界: 型・要素数・座標系・時刻/世代・CPU/GPUの所在を接続契約に含める。CPU結果は必要な出力だけアップロードし、GPU上の結果は可能ならbufferを直接共有する。同時使用中の内容を書き換えず、必要な依存・同期・寿命を既存実行基盤へ渡す。

まず既存評価器の装着/解除とscene上の所有を接続し、評価済みカーブからrender入力へ至る一経路を通す。その後に配列/姿勢・物理出力へ広げる。カーブ、物理、描画の全処理を同じ時間刻みの一枚のグラフへ押し込む方針ではない。標準機能はノードへの分解可能性を保持する。

既存pelican.anim_graphのstate/clip/blend評価器は標準の姿勢生成処理として再利用する。汎用の数値/イベント接続、音声制御、render resourceへの出力を、そのstate machine文法へ個別の特例として積み込まない。合成側で型付きポートと実行phaseを扱い、各評価器/サービスへ渡す。scene componentはその構成と公開パラメーターを装着する入口とする。


## 凍結一覧の旧混在表（2026-09-22 の分類前）

## 4. v0 の既知の穴

約束の内側にあるが今日は塞がっていないもの。**行番号ではなく台帳の節名**で引く。

| 穴 | 記録している節 |
|---|---|
| `graph.instance.owner` の authoring object id — **解消**。scene 保存時に ID と次の採番値を永続化し、削除・再読込後も別個体へ付け替わらない | `sceneformat` / `AuthoringSceneDocument` の `[persistent-identity]` 回帰テスト |
| 加速構造 row transform の identity 比較が ±0 を別物とする(`accelerationstructure.cpp`) | 「未了項目と再開順」 |
| `frameIndex() == 0` の間は animation frame を公開できない(`setTime` だけで駆動する headless 呼び手) | 「未了項目と再開順」現況 |
| `SceneLoader::load` を 2 回呼ぶと hang | `### AG-1b:` |
| 放棄した scene transaction の teardown で hang | `### WP380e:` / 「未了項目と再開順」 |
| `looks_mutating` gate | `### WP386: RT3-2` |
| `PELICAN_WITH_RPC=OFF` build で落ちる test(`raytracing_rt3_1_production_test.cpp` の `#if`) | `### AG-1a:` / `### R-1:`(安い穴) |
| Studio が raytracing_demo を開けない(`renderer.cpp`) | `### WP386: RT3-2` |
| temp dir を共有する `renderconfigeditor` 6 件が並列で赤 — R-1 で per-process temp root(flake は再現せず。真の候補は `gpu_lease_run.cpp:94` が lease log を消さないこと: `%TEMP%` に 3358 個)| `### O-1:` / `### R-1:`(安い穴) |
| export の `.tmp-<pid>` が残る — **R-1 で閉じた**(次の export が掃除)| `### P-2a-3:` / `### R-1:`(安い穴) |
| `structfieldjson.hpp` の呼び手無し複製 — **R-1 で閉じた** | `### R-1:`(安い穴) |
| shader source の改行と cache key | `### WP386: RT3-2` |
| preset + graph root の `region_replacements` | `### P-2a-4a:` / `### P-2a-3:` |
| `--start-time` の `llround` overflow(≈1e17 s。今日は起きない) | `### O-1:` |
| `requires` を configure が読まず OFF build で DLL の link に失敗する — **解消**。configure と test sweep が同じ宣言表を使い、要求を満たさない sample target を作らない | `projectrequirements.def` / `pelican_project_requirements.cmake` |
| source tree の `dist_debug/` を別 build が上書きする — **解消**。実行ファイルは `<build>/dist/<config>/`、配布物は `cmake --install --component Runtime` でまとめる | `pelican_runtime_layout.cmake` / README |
| `pelican_studio` の終了時クラッシュ — **終了時に届く RPC 通知の経路を修正**。Windows で保存・project 切替後のアクセス違反を再現し、viewport 破棄前の通知切断で正常終了を確認。scenario runner も異常終了を失敗として扱う | `EmbeddedViewport::~EmbeddedViewport` / `[shutdown]` 回帰テスト |
| `test/gpu_lease_run.cpp:94` が GPU lease の log を消さず `%TEMP%` に `pelican_gpu_lease_v1.<pid>.log` が数千個積もる(R-1 の temp dir flake の真の候補)| `### R-1:` の完了記録 |
| N-3 のパンは scroll bar 経由なので、**描画が view に収まっている間は中ボタンドラッグが何も動かさない**(使い勝手の穴)| `### Q-1b:` の着地前の実測(2026-09-17)。**N-4 `142a34fc` で解消**(view が余白を持ち `centerOn` で動く)|
| `studio_scenario_graph_edit_roundtrip` が赤(帯の editor が ~500 ms で閉じる)| 根本は N-2 `f2eccbfd` の `FocusOut` 規則。この機械では導入 commit `c026002d` から一度も緑でなかった(Q-1 の記録は別 tree)。`450f2866` の全数 1947 はこのシナリオを含まない。**Q-3 `c90fde90` で修正**(環境依存の timing ではなく実欠陥) |

