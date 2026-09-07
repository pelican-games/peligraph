# v1 の完成条件と開発順序

2026-09-07 更新。利用者が確定した製品方針と、開発再開時に必要な現況をまとめる。
WP の仕様・検証履歴の正本は [implementation_plan.md](implementation_plan.md)。
本書は機能を削減する裁定や、未実装 WP の仕様を新設するものではない。

## 再開時の入口

- 作業ブランチは `codex/render-target-runtime-slice`。RT・Studio基盤を `96d9cb7e`、embed再生成修正を `b0c000a9`、文書・運用整理を `1878a3e0` に統合。再開後、material passのray query設定を `73557204`、入出力編集を `d2f336a5`、raster接続編集を `2ecb5ae8`、rasterのshader・draw・state編集を `03ee3b32`、triangle worldモードのGUI・保存を `c20a2432` に統合。続いて異transform AABBのworld結合を実装した（7f2a2dc6）。AABBのgeometry/world切替もScene AS画面へ接続した（変換record宣言済みlayout v3が対象）。
- 本書の現況と残件を読み、着手する項目だけ台帳の該当WP・最終差分と `design_raytracing.md` §4-P / §11へ進む。台帳全体や完了履歴を毎回読み直さない。
- 利用者の最新指示が旧運用記録に優先する。2026-09-06に統合担当としてのcommitを許可済み。実装・影響範囲の確認・記録が揃った単位でcommitし、個別の実装workerは統合担当へ渡す。
- 同時agentは主担当を含め最大2、fastは使わない。`<作業ディレクトリ>/wp336`、`projects/showcase/`、無関係な `.claude/`・`out/` は触らない。モデル・thinking effort・プラグイン設定は利用者の選択を尊重する。
- 開始時にbranch・差分・対象jobの状態を確認する。完了したログや状態ファイルだけを根拠に、処理が今も動いていると判断しない。

### 統合済みと残件

| 区分 | 内容 |
|---|---|
| 実装済み | callableと動的record・公開API、skin/morph/VATとBLAS refit、3D/間接dispatch、名前付きTLASとASノード、複数geometry BLAS、world triangle/AABB結合（AABBは明示的な変換recordを使用）、AS前のcompute/material/raster/sprite描画 |
| 編集経路を実装済み | component追加・削除等、procedural asset作成・編集・未使用定義削除、task宣言/接続/dispatch、Callable初期値、AS順序、fullscreen preset編集、material/raster passの入出力・ray query/TLAS選択、material contract・対象/出力/resource設定・routing表編集、rasterのshader参照・draw/state JSON編集、root/preset feature instanceの公開パラメーター編集、project featureの明示pass編集、Frame Planドラッグ接続、triangle geometry/world切替とlayout同時保存 |
| 機能の残り | 計画・未了記録と現行コードを再照合して残件を確定する。6種の補助描画は出力先・shader・ray queryのGUI/RPC編集まで接続済み。UI/ImGuiは固定描画としてAS非依存。補助shaderのdescriptor契約不一致は生成/再読込時に拒否する。生成variantは元feature設定から再生成する。output_transformは固定色変換であり汎用編集は不要（design_color_pipeline.md §0）。機能全体の完了は未判定 |
| 操作・性能の残確認 | 新dialog全体の実エンジン接続下操作、保存・再読込を含む制作経路、複合undo等の既存レビュー残余、動的ASの確保コストと性能計測、配布物 |
| 後で行うもの | 学習サンプルと代表ゲーム。WP386cの追加数値oracle整備は保留であり、機能開発の必須条件に戻さない |

geometry occurrence候補選択とoverrideのcomponent/行・適用状態・実効値・拒否理由をStudioへ接続済み。RT stage/record/materialの観測、AS設定の実効値・適用元・優先順、Callableの実行時CPU設定も接続済み。省略Defaulted fieldのAdd default、Listの追加・削除・行内値編集・IDを保つ移動も接続済み。model reload後GPU反映、direct model参照のasset key、別Behaviorの独立Undo、spawn/add→List編集、unpack/destroyを跨ぐUndo/Redoも修正・確認済み。新画面の実GUI操作は残る。上表は実装率や残工数の積算ではない。個々の証拠と未確認範囲は下表・台帳に従う。

## 現行コードに基づく次の作業（2026-09-07、物理実装の区切り後）

利用者から「機能実装をどこまで行うか」「物理を優先した理由」の確認を受け、物理の連続拡張を止めて開発順を再整理した。これは未実装機能の削除や、v1完成の宣言ではない。台帳の過去の「次は」「未実装」をそのまま現在の作業一覧として扱わない。

| 区分 | 現在の判断と次の作業 |
|---|---|
| RTの実行機能 | RT4のcallable/動的record、RT5の変形/refit、RT6のdispatch/名前付きAS/geometry結合、RT7のtask/接続編集には実装・統合記録がある。旧記録だけを根拠に再実装しない。残る組合せは具体的な利用操作と現行コードから判断する |
| 完了した描画機能 | WP242bのpoint/spot shadow。標準featureの深度pass・汎用family行列buffer・照明resource portを接続。実GPUでforward/deferredの影ON/OFF画素差とpoint全6方向の影・対応深度面を確認。標準algorithm OFFの独立buildとprovider不在・汎用matrix機能の実行確認も成功。未使用の専用packerと専用テストは削除済み |
| 完了した影品質設定 | WP242c。directional/point/spotにpcf_radius（0/1/2＝1/9/25点、既定1）、bias（既定0.0015）、strength（0＝影なし、1＝完全遮蔽、既定0.65）を公開。既定値は各feature宣言に置き、forward/deferredは同じPCF実装を利用。設定変更の実GPU効果と既存影経路を確認済み |
| RTの観測 | get_ray_occurrencesは同じscene-plan snapshotのrecordフィールド名・型・offset・値、group名、layout/bindings参照と選択行、group/recordの適用元をInspectorへ返す。ASのmask/flags/custom indexは実効値とfieldごとの適用元、geometry rule index・override row IDを表示する。Frame Planのshader slotにgroup/ray名を併記する。geometryのhit groupから読み込み済みFrame Planのtask候補を選び、既存Task Editorへ進む操作を追加。動的callable recordはruntime_callable_recordsでCPU側実行時設定を取得できる（task詳細とRaw JSONに表示）。GPU完了値のreadbackと実GUI操作確認は残る。世代の異なるGPU情報を混ぜない |
| コンポーネント | Inspectorに追加・削除・Add default・List追加/削除/移動・override候補選択がある。unpack後のList追加をUndoすると採番値の差でunpack Undoが失敗する不具合を修正し、全順序のUndo/Redoと実値変更時の拒否を確認した。destroy/復元を跨ぐList編集のjournal Undo/Redoも確認済み。全componentのruntime再装着や実GUI操作とは区別する |
| 機能完成後の確認 | 実エンジン接続下の新画面操作、保存・再読み込みを含む制作経路、性能・配布。確認だけのために新しい機能開発を止めたり、広域テストを毎回繰り返したりしない |
| 物理の現在地 | mesh raycastのscene/import/reload・BVH保持、Sphere・Box・Capsuleのmesh overlapをprovider/serviceへ接続。回転とメッシュの非一様scale・反転・shearを扱い、三角形表面との接触を判定する。Sphere・Box・Capsuleのsweepを公開Shape Castへ接続。Boxは連続SAT、Capsuleは軸線を三角形側へ移してSphereと解析式を共有し、回転固定の最初の接触時刻とmesh表面上の位置を求める。mesh Triggerを通常形状との組み合わせ・mesh同士の表面接触へ接続。Enter/Exit・相互filter・設定の保持に対応。Sphere/Box/Capsuleの初期めり込み解消も接続し、mesh全体に対して補正量を検証する。最短性を保証しない補正は近似フラグで区別する。任意の凹meshの厳密最短MTD探索は未実装 |
| 後の段階 | 学習サンプル・代表ゲーム・GUI操作感の仕上げ。動的剛体シミュレーションは既存仕様の将来トラック。これらを今の残件探索から自動的に着手対象へ広げない |

今回の再照合ではRT設計書に残っていたport接続・順序編集の未実装表記を現行実装へ訂正した。RT4〜RT7の主要機能を再実装する根拠はなく、GPU readbackや追加数値oracleを次の必須機能にはしない。表は全機能の完了監査や工数見積りではない。

コンポーネントからの[アニメーショングラフ](design_animation_graph.md)装着を実装。sceneの`clip XOR graph`宣言・graph参照の保存、既存EvaluatorV1のowner threadでの登録/解除・tick、GameContextのパラメーター変更/状態切替を接続した。clipとの切替、名前付き/名前なしobject、削除・scene reloadを実機で確認。初期値はgraph文書のparametersを使い、componentのparameters Listに個体別overrideを保存・適用する。同一instanceのmodel rebind・graphファイル更新・setTime通知は実装したが個別試験は未実施。graph assetのscene準備時検証と編集失敗時の状態復元、GUI・汎用graph間接続は残件。

VRMAの自動ファイル監視も過去の未了記録にあるが、既存register/reload APIはdecoded clipを受けるため、source pathの所有と監視の接続が必要。graph装着より先に別の監視基盤を追加しない。物理の厳密最短MTD、サンプル、GUI仕上げへこの作業を広げない。

graph装着の前提として、AnimationServiceのscene登録をepoch/entity/model instanceへ照合し対象だけ退役する処理をフレームへ接続した。共有モデルの別個体と同名再登録の独立性をCPU試験で確認。実scene/rendererの再spawn確認はcomponent装着と合わせて残す。

## 製品の完成条件

目的は、最新のグラフィックスパイプラインをノードベースで分かりやすく学習でき、
3D ゲームの制作にも使えるエンジンを公開すること。
RT 機能を十分に完成させ、GUI の操作感を磨いた段階を v1 とする。
小さなゲームが動くという理由だけで RT の残計画を削らない。

- エンジン: RT の主要な shader stage、AS、record、動的更新、dispatch を扱える。
- ノード操作: パイプラインの構成と依存を編集し、保存・再読込できる。
- 観測: 入力から実行順・資源・shader の選択まで追え、エラーを原因のノードへ辿れる。
- 学習: 小さなサンプルで設定変更前後の違いを試せる。内部情報は必要に応じて詳しく辿れる。
- 制作: 取り込み・配置・component 編集・入力・移動・衝突・animation・音・UI・scene 遷移・保存・配布を代表ゲームで通す。

代表ゲームは制作経路の確認、学習サンプルは graphics 概念の説明と操作確認に使う。
全サンプルを巨大な新テスト基盤へ作り替えない。対象 OS/GPU、配布形態、性能目標は未確定で、公開条件を具体化する段階で固定する。

## RT と編集面の棚卸し

RT3〜RT7 は v1 の対象候補として全て残す。AS の graph node 化は学習目的の中心候補であり、後続へ自動的に送らない。
段の名称・依存は [design_raytracing.md](design_raytracing.md) §8・§11 と台帳の最終差分に従う。
「記録上の完了」「今回確認したコード・ログ」「未確認」を区別する。既存の完了記録を全て未確認へ戻さない。
下表は着手順の判断材料であり、コードレビューや全数試験の合格証明ではない。

| 領域 | 学べる概念・v1 で扱う内容 | 現在地と根拠 | ノード操作・観測・サンプルの残確認 |
|---|---|---|---|
| RT1 / RT2 | raygen・miss・closest-hit・any-hit、再帰、SBT addressing、material ABI | 台帳 WP364〜368 に実装・検証記録あり。今回の統合ログで WP367 の本番 any-hit 試験が成功 | stage/record/material の結び付きの可視化、設定変更と GPU 結果を試す教材は完了未判定 |
| RT3-1 | instance mask/flags/custom index と geometry flags | WP374/374a 着地済み。WP384-fix 後の対象 GPU/RPC 4 件が統合ログで成功 | 個体編集→GPU 効果→保存→再読込を GUI から通す。既存 RPC の証拠を GUI 完了とは扱わない |
| CARRIER-RT3 | 再帰 schema、可変長行、occurrence、個体ごとの geometry override | WP382a/b/383 着地済み。382b-fix は 36ad3444 に着地(関連37/37、レビュー残余あり)、382c は 23cb5153 に着地(W1 98/98、レビュー残余あり)、382d は4ce4117dに統合(W1 95/95) | 行の追加・削除・移動、対象 geometry の選択、override の出所表示。複合undo等の残余を閉じ、CARRIER-STUDIO との接続を確認 |
| RT3-2 | static AABB と intersection shader、三角形との混在 | WP386a/b統合済み。球のreport・reload・変更行列、procedural assetの新規作成・bounds/record編集・未使用定義削除RPC・保存・component装着・runtime反映を実GPU確認。専用Studio画面を接続。386cの数値基礎はae29fc98まで保存、追加数値基盤は保留 | 新画面とmixed geometryの実編集操作を確認する。学習サンプルは後回し、数値oracleの完成を次機能の条件にしない |
| RT4 | callable、per-in-flight の動的 record | callable実行・reload、動的recordの宣言・更新・submission寿命管理、GameContext public APIを実装。Studioから初期値の永続編集も接続 | 新しい初期値dialogの実操作確認は残る |
| RT5 | 変形 geometry の毎 frame rebuild/refit | skin/morph/VAT のcompute変形、初回BLAS buildと以後のrefitを接続。個体別変形・VAT補間のヒット追随、モデル変更時rebuildを実GPU確認 | 出力allocation削減と性能計測は残る。使用中のASを変更しない方式 |
| RT6 | dispatch 一般化・indirect、AS graph node、複数 TLAS、複数 geometry BLAS | 3D・間接dispatch、名前付きTLAS構築とconsumerの交互配置・選択（compute/RT/material/raster）、共有AS前の独立compute・material・fullscreen/raster・sprite実行、複数triangle geometry BLAS/refit、明示worldモードで異transform個体の結合・移動rebuild・morph refit、単一componentの複数AABB asset/BLAS結合を実装・実GPU確認 | 補助描画6種のray query宣言・AS選択とUI/ImGuiのAS非依存化は実装済み。AABBは明示的なworldモードと変換recordで異transform結合に対応。triangle worldモードはScene AS画面から保存・GPU反映へ接続済み |
| RT7 | ノードの一級化、編集 RPC、fork/CAS | 名前付きAS・task接続・Callable初期値、preset fork、fullscreen/material/rasterと6種の補助描画の編集、root/preset featureパラメーター編集と生成variant再生成を実装。compute/RT taskの宣言編集・追加・削除、dispatchフォーム、候補付き接続図とドラッグ接続、ASとconsumerの交互順序編集をRPC/Studioへ接続 | 新dialog全体の実エンジン接続下での手動操作は未確認。補助描画は仕様どおり通常inputなし、shadowはvertexのみ、velocity/pickingはskinned vertexも編集可能 |
| ゲーム制作 | component と scene、入力、物理、animation、音、UI、保存、配布 | 各機能の記録は manual/台帳に存在。Inspectorの宣言JSON入力、登録behaviorとparams付き追加、model・texture・procedural asset選択を接続。削除・Undo/Redo・保存は既存journalを使用 | 新フォームの実操作、asset定義編集、代表ゲームでの一連の制作経路は残る |

確認した実装入口: [pipelinefactory.cpp](../src/core/shader/pipelinefactory.cpp)、
[RT dispatch](../src/core/vkcore/raytracingpipelinedispatch.hpp)、
[AS](../src/core/renderer/accelerationstructure.cpp)、
[frame plan graphics](../src/devstudio/view/frameplangraphics.cpp)、
[read capability](../src/devstudio/view/frameplanreadcapability.hpp)。
検証の詳細は台帳の該当記録を参照する。着手時に対象の不足だけを具体化し、全項目への証拠の再転記や、表を埋めるためのテスト追加は行わない。

## 開発順序

### 全体グラフを正本にする（2026-09-07、直近の方針）

コンポーネント固有のグラフを別系統として増やす進め方を見直す。
[全体グラフとコンポーネント表示の共通化](design_graph_composition.md)を参照し、
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
次は音声等のevent sinkとraster/fullscreen passの共通node化を進める。
音声sinkの前提となるsetSoundPausedとpaused状態を追加し、同じvoiceの一時停止/再開を
null backendで確認した。eventから音声への自動接続とownerに連動したvoice解放は残件。
停止時もGPU資源は描画世代が保持する。動的instance装着や個体単位の資源回収は未了。
sceneごとのgraph選択と音声handle/任意外部inputの自動供給はまだない。
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

### ノードへの分解を優先する（2026-09-06 利用者方針）

- 新機能・既存機能の変更時は、既存ノード、resource port、buffer、view family、project shaderの組み合わせで表現できる範囲を先に確認する。専用のruntime処理を追加する前に、具体的な構成と不足する基盤を区別する。
- アルゴリズムは可能な限り、利用者が分解・接続変更できる標準feature／グラフ構成として提供する。標準の一括操作を用意する場合も、内部構成を編集できる経路を保つ。
- 使用頻度の高い標準機能は、通常時には最適化した一体の機能として追加・提供してよい。ノードへの分解を選ぶと、編集可能な構成へ展開し、その一体化に依存する専用最適化を解除する。独立に正当性を保てる一般的なコンパイラ最適化まで一律に無効化する必要はない。
- 最適化版と分解版は同じ機能・設定を表現する。分解時に入力・出力・パラメーター・依存を引き継ぎ、保存・再読込後も展開済み構成と編集内容を保つ。再最適化で利用者の編集を上書きしない。低層のコンパイル結果編集・既存fork経路を実装根拠として確認し、未確認の展開操作を実装済みとは扱わない。
- 既存実装に分解可能な処理が一体化されている場合も、対象機能を扱う際に既存ノードへの分解を検討する。一括の全面改修を開始する指示ではない。分解により処理の流れを理解・変更しやすくなるかを判断する。
- GPU資源の寿命、同期、描画・dispatchの実行などの基盤は適切な責務に残す。既存ノードで不足する場合は、影など個別アルゴリズムに固定した仕組みより再利用できる最小の基盤を追加する。
- 現在のpoint/spot shadowでもこの方針を適用する。clustered lightingの既存graph buffer/resource port経路を参照し、未統合の影専用frame descriptor追加は取り下げた。指定した解決済みview familyの行列をgraph bufferへ公開するhost_sourceを追加。receiver接続と影の実GPU確認はWP242bで完了。
- 既存forkRenderPipelineはpreset原文のprojectへのコピーと参照切替であり、feature展開とは別の操作。feature設定の具体化・保存は下記のmaterialize操作で接続した。XR/preview向けの実行variant生成はfeatureの一体化とは別であり、variantごとの独立文書化を分解の必須条件として追加しない。
- タグ付き領域の既存差し替えは`edit_region_replacement`でproviderを選択・解除できる。解除は元ノードを保持して差し替え要求だけを削除し、保存・再読込後も選択を保持する。StudioのRegion replacements...からproject presetの領域と現在のproviderを取得し、選択・解除・保存へ接続した。実エンジン接続下の画面操作は未確認。標準featureの具体化は下記のMake editable...を使う。
- パラメーターを具体値へ展開したfeature文書は`materializeRenderFeatureDocument`で生成できる。runtime設定はbound_parametersとして保持する。`edit_render_features`のmaterialize操作でroot/project presetの選択instanceの参照切替・新規文書保存まで接続済み。StudioのMake editable...から同じApplyへ接続した。対象featureが追加・直接上書きするpass、有効integrationが上書きするpass、surface resourceの接続先のタグ付き領域は同じ保存で差し替えを解除する。全アルゴリズムの内部処理を既存ノードへ分解し終えたとはしない。

1. 入口の残件から具体的な操作を1つ選ぶ。実装済みのRT4/RT5を未着手へ戻さない。
2. RT6の残る組合せとRT7の編集範囲を進める。GUIは既存runtime・RPC・保存経路へ接続し、別の実行系を作らない。
3. 変更する機能と一緒に、関連する責務の重複や不要な旧分岐を整理する。GPU寿命・保存・公開境界の処理は維持する。
4. 中核機能が揃った段階でGUIの操作感を磨き、学習サンプルと代表ゲームで制作経路・性能・配布を確認して公開条件を閉じる。

## 調査と確認を反復しない進め方

- 最初に変更対象・期待する動作・既存の確認手段を短く決める。曖昧な「全体を改善」を、毎回の広域調査や検証基盤の新設へ広げない。
- 検索はファイル・symbol・WPを絞り、必要な段落と差分だけ読む。build/testの全ログはファイルへ保存し、終了コードと失敗箇所・短い末尾を確認する。
- build/testは既存sessionの待機を通常30〜60秒にまとめる。数秒間隔の確認や同じログの再読込みを繰り返さない。完了通知があればそれを使い、無ければ同じhandleを確認する。待機中は独立した調査・文書作業を行い、build/test対象のsourceを変更しない。
- 実装中は変更したcase/sectionを先に確認し、修正が揃った区切りで関連範囲を通す。失敗・追加変更・未解決の懸念がなければ同じ全数・全構成を反復しない。GPUは `-j1`、現在のWindows buildは `-j2` を基準とし、CPU並列度は対象と既存lockに合わせる。
- 新しいテストは実際の故障を検出するために追加する。AS順序・GPU寿命・rollback・保存・ABIの必要な確認と、実装の写経や出力の無意味な固定を区別する。文書だけの変更ではPlayer/StudioのbuildやGPU試験は不要。
- モデルを使い分ける場合は、限定的な実装は通常モデル、機械的な整理は小さいモデル、GPU寿命やtransaction等の複雑な判断はAstraを候補とする。これを理由に勝手に設定を変えない。委任時は対象ファイル・境界・成果物に絞り、同じ広域調査を主担当とworkerで重複させない。
- レビューは未解決の論点と危険な変更に絞る。過去に確定した設計を理由なく再審議しない。完了記録は最終動作・有効な検証・残件・commitを中心に1回まとめ、途中の試行錯誤は必要な理由以外をログへ残す。
- 本書の現況は追記で古い「最新」を積み上げず置換更新する。台帳には確定した仕様差分と検証履歴を残す。機能の区切りで短い引き継ぎを作り、次の作業に無関係な会話履歴を持ち越さない。

## 保守上の整理候補

| 対象 | 確認できた負担 | 整理するタイミング |
|---|---|---|
| 現況と履歴 | 台帳の旧再開順・未commit表記と最新の状態が混在していた | 今回、入口と履歴を明示して統合状態を更新。以後は入口を置換する |
| renderer/material container | 約5,900/5,100行に処理が集中。初回準備と公開済み世代の使い分けでmaterial/spriteの不具合が発生し修正済み | 関連機能を触る際に責務と共通経路を整理。行数だけを理由に一括分割しない |
| RT production test | 大きなfixtureに機能別条件が集まり、取り込み設定・初回起動・カメラ等の準備ミスが修正往復を生んだ | 次に関連fixtureを変更するとき、共通の本番起動手順と機能別設定を整理。独立の検証frameworkは増やさない |
| 動的ASの確保 | refit後も出力バッファ・ASの新規確保が残り、性能影響は未測定 | 性能を計測してから必要な再利用を導入。使用中資源の上書きはしない |

未実装機能・未確認操作・技術的負債を同じ「未完」にまとめない。旧互換分岐の不要性や配布物へのtest tool混入は、呼出元・build target・最終成果物を確認して判断する。

## 機能を保って開発負担を減らす規則

- **2026-09-05 利用者裁定(数値検証の拡張を停止):** v1の目的はグラフィックスを学び、編集して使えることであり、Vulkan数値適合性の認証ではない。WP386cのrigid区間、slab区間、predicate後の数学的一意性、constructive generator、任意SPIR-Vの意味認定を新たに作る作業は保留し、RT4以降へ進む必須条件から外す。既存の有用な回帰テストは維持する。配置・AABB・混在シーンは既存描画経路と実際の編集・保存で確認し、発見した実害を修正する。機能の正しさ、GPU寿命、不正入力への必要な診断は引き続き確認する。本裁定が旧WPの数値golden完遂要件に優先する。
- **2026-09-05 利用者方針:** 保守的な処理の積み重ねで可読性が落ちている箇所は、実装・修正時に現行契約と呼び出し元を確認して整理する。不要な旧版分岐、重複検査、到達不能な救済、方針転換で不要になったコードは維持を目的に残さない。保存内容の保護と GPU 寿命等の必要な処理は責務・条件をまとめて読みやすくする。対象外の大規模整理は混ぜない。
- 未公開版の内部 SBT 行番号 `q` や内部 bytes の旧値を、互換契約として固定しない。現行方式での決定性、形状 / shader / record の対応、保存設定の意味、誤った addressing の検出を維持する。永続 row ID、undo による raw 復元、実際に公開した ABI 等の契約とは区別する。
- 同じ変更・構成・故障経路について有効な証拠が既にあれば、理由なく同じ検証を反復しない。
  GPU 寿命・rollback・保存・ABI 等の故障経路を区別し、単純な「1 主張 1 テスト」は強制しない。
- §4 規約 10 の否定対照は維持する。本番を写経するテスト、同じ実装から期待値を作るテストは受け入れの代用にしない。
  GPU 全数を内周で毎回回さず、変更対象の既存テストを使う。広域確認は統合の節目にまとめる。
- **2026-09-05 再開時の運用更新:** 各WPで全構成・全数を繰り返す旧要件は、影響範囲に基づく確認へ改める。変更した条件分岐・依存・設定を通る構成を選び、構成間で共通のコードだけの変更は同じ証拠を再利用する。機能フラグ自体や公開・保存境界を変えたときは対応する対照構成を確認する。実行した対象・省略理由・既知失敗を記録し、未実行を成功とはしない。前回未完だった全数は台帳の完走・既知失敗記録で閉じ、恒久的な再実行義務にしない。
- 厳密一致は保存契約・ID・ABI・決定性等、厳密である必要がある対象に使う。
  描画は目的に応じて数値許容や可視効果を使う。旧 source/SBT/TLAS の不変要求は、移行用の一時条件か現行製品契約かを判定する。
- 旧版受理の廃止と ABI 不一致の検出を区別する。単一現行版という共通方針と、RT/layout や DLL 入口の個別仕様の不整合は、参照と移行先を調べてから明示裁定する。
  進行中 WP に無断で互換撤去を混ぜない。
- 保存失敗や不正入力から編集内容を守る処理、GPU 資源寿命、必要な rollback は維持する。
  根拠のない救済、到達不能分岐、重複検査を対象にする。
- 配布監査は source の名前ではなく、build target・リンク・最終ファイルを辿る。
  `BUILD_TESTING=OFF` だけで混入ゼロとも、core のコンパイル対象というだけで混入ありとも断定しない。
  runtime の診断・学習用途を持つ機能と、fixture/fault setter/開発専用ツールを区別する。
- 整理は独立した小変更で行う。RT oracleのtest専用targetへの分離は台帳に実施記録があるため、理由なく最初からやり直さない。ray layoutやDLL入口の旧版分岐は利用実態を確認したものだけ撤去し、出荷対象の未確認部分は配布確認で扱う。

## Fable との相談

Codex が調査・実装・検証・統合・利用者への報告を担当し、Fable は重要な設計判断の独立相談役とする。
所有・寿命・保存契約・共同 transaction 等に重点を置き、全作業へ新たな多段レビューを追加しない。
原則は初回検討と論点を絞った再確認 1 回。既存 WP で確定した受け入れや打ち切り規約は勝手に変更しない。
採用・保留・不採用と根拠を残す。二者の合意はコードや実測の代わりにならない。
2026-09-05 の接続試験は Claude Code 2.1.260 / claude-fable-5-1、fast OFF、読み取り限定で成功。
原文はローカル `<作業ディレクトリ>/fable_trial_20260905/discussion.md`。共有可能な判断は本書と台帳に残す。

学習サンプルを後回しにする利用者方針は継続。callableと動的recordの実装・確認は済んでいる。次の作業は冒頭の現況と残件から選ぶ。
