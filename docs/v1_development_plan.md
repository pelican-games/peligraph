# v1 の完成条件と開発順序

2026-09-22 更新。利用者が確定した製品方針と、開発再開時に必要な現況をまとめる。
WP の仕様・検証履歴の正本は [implementation_plan.md](implementation_plan.md)。
本書は機能を削減する裁定や、未実装 WP の仕様を新設するものではない。

## 再開時の入口

- 2026-10-05 引継ぎ: EB-6／EB-6s の再起動なしの material 修復を統合。最新 develop と合わせた関連契約 21 件、material／Problems／RT の GUI scenario 10 本が成功。QD-1 の driver 強化と不変条件の棚卸しは別途検証中。公開 license・寄与者コード・履歴・文書の公開範囲は利用者判断待ち。配布は runtime shader compiler 入りで、compiler を外した配布の仕組みは v1 の別作業。

- v0 の出荷範囲・解消済み項目・再検証は [v0 凍結一覧](v0_freeze.md) §3・§4・§6を参照する。v1 の拡張を v0 の未実装と混同しない。
- 着手する項目の WP・最終差分だけ確認する。機能フラグの表は [専用台帳](build_features.md)、以前の着地時系列は [履歴](history/v0_delivery_2026-09.md) に分離した。
- 開始時に branch・差分・対象 job の現在状態を確認する。古いログの成功は現行 revision の release gate 通過を意味しない。
- 利用者の現在の指示を優先する。モデル・並列数・commit 権限・端末固有パスを過去のセッション記録から復元しない。

### RT・編集機能の棚卸し

| 区分 | 内容 |
|---|---|
| 実装済み | callableと動的record・公開API、skin/morph/VATとBLAS refit、3D/間接dispatch、名前付きTLASとASノード、複数geometry BLAS、world triangle/AABB結合（AABBは明示的な変換recordを使用）、AS前のcompute/material/raster/sprite描画 |
| 編集経路を実装済み | component追加・削除等、procedural asset作成・編集・未使用定義削除、task宣言/接続/dispatch、Callable初期値、AS順序、fullscreen preset編集、material/raster passの入出力・ray query/TLAS選択、material contract・対象/出力/resource設定・routing表編集、rasterのshader参照・draw/state JSON編集、root/preset feature instanceの公開パラメーター編集、project featureの明示pass編集、Frame Planドラッグ接続、triangle geometry/world切替とlayout同時保存 |
| 機能の残り | 計画・未了記録と現行コードを再照合して残件を確定する。6種の補助描画は出力先・shader・ray queryのGUI/RPC編集まで接続済み。UI/ImGuiは固定描画としてAS非依存。補助shaderのdescriptor契約不一致は生成/再読込時に拒否する。生成variantは元feature設定から再生成する。output_transformは固定色変換であり汎用編集は不要（design_color_pipeline.md §0）。機能全体の完了は未判定 |
| 操作・性能の残確認 | 新dialog全体の実エンジン接続下操作、保存・再読込を含む制作経路、複合undo等の既存レビュー残余、動的ASの確保コストと性能計測、配布物 |
| 後で行うもの | 追加の学習サンプルと、物理・音・save/loadまで使う代表ゲーム。出荷済み6 sampleの起動確認とは区別する。WP386cの追加数値oracle整備は保留であり、機能開発の必須条件に戻さない |

geometry occurrence候補選択とoverrideのcomponent/行・適用状態・実効値・拒否理由をStudioへ接続済み。RT stage/record/materialの観測、AS設定の実効値・適用元・優先順、Callableの実行時CPU設定も接続済み。省略Defaulted fieldのAdd default、Listの追加・削除・行内値編集・IDを保つ移動も接続済み。model reload後GPU反映、direct model参照のasset key、別Behaviorの独立Undo、spawn/add→List編集、unpack/destroyを跨ぐUndo/Redoも修正・確認済み。新画面の実GUI操作は残る。上表は実装率や残工数の積算ではない。個々の証拠と未確認範囲は下表・台帳に従う。

## 機能別の棚卸し（2026-09-07 の確認を基準にした v1 の候補）

以下は機能ごとの到達点と確認境界を残す表である。その後のAG/P/N群と2026-09-22の修正は
[v0の現況](v0_freeze.md)で上書きされる。ここに残る「未確認」「残件」だけを根拠に再実装しない。

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
| CARRIER-RT3 | 再帰 schema、可変長行、occurrence、個体ごとの geometry override | WP382a/b/383 着地済み。382b-fix は 6289434c に着地(関連37/37、レビュー残余あり)、382c は 345123b8 に着地(W1 98/98、レビュー残余あり)、382d は2958ac42に統合(W1 95/95) | 行の追加・削除・移動、対象 geometry の選択、override の出所表示。複合undo等の残余を閉じ、CARRIER-STUDIO との接続を確認 |
| RT3-2 | static AABB と intersection shader、三角形との混在 | WP386a/b統合済み。球のreport・reload・変更行列、procedural assetの新規作成・bounds/record編集・未使用定義削除RPC・保存・component装着・runtime反映を実GPU確認。専用Studio画面を接続。386cの数値基礎は6fec9a44まで保存、追加数値基盤は保留 | 新画面とmixed geometryの実編集操作を確認する。学習サンプルは後回し、数値oracleの完成を次機能の条件にしない |
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

### v0 の約束と「往復」(2026-09-16、Fable 提案。利用者と方向を確認済み、細部は未確定)

2026-09-15 の「アニメーショングラフの区切りと v1 洗練」を、利用者の製品思想([pelican_vision.md](pelican_vision.md))と外部リサーチ(Houdini HDA / TouchDesigner / Gaffer / Falcor / Hydra・MaterialX / Blender Simulation Zone / VS Code manifest)を踏まえて書き直したもの。
v0 は機能の総数ではなく、**約束する制作体験**で切る。

**pelican の中心**: シーン・資産・時間・機能の接続を制作物の正本として持ち、GUI でもコードでもエージェントでも同じ編集ができる。描画方式や高度な機能は、その正本を壊さずに交換・分解・除去できる。
利用者から見た一本の流れは **使う → 手続き化する → 開く → コードで変える → 再び部品にする** で、v0 で最も完成させる価値があるのはこの往復。

#### v0 の約束(4 シナリオ。各行の右が受け入れの形)

| シナリオ | 内容 | 受け入れ(§4 規約 10) |
|---|---|---|
| **作る** | 用意された scene から、異なる時計で動く対象と、グラフで制御される parameter を作る | GUI の操作列と RPC の呼び出し列が **同じ文書 bytes** になる。片方だけの操作が無い |
| **出力する** | 同じ scene をリアルタイムで動かし、対応範囲を明示したうえで指定時刻または replay からオフライン出力する | 指定時刻 t の連番出力が決定的(2 回で bytes 一致)。対応外のノードは名前付きで拒否され、近似や省略をしない |
| **拡張して持ち出す** | 標準部品を派生させ、グラフまたはコードを変更し、別の空プロジェクトで再利用する | 本体ツリーの外にあるテンプレート repo が公開 SDK と文書形式だけで build・実行できる。内部ヘッダや手作業を要求しない |
| **外す・失敗させる** | モジュールを外した構成が成立し、依存が欠ける作品は名前付きで拒否される。失敗した reload で旧世代が壊れない | `PELICAN_WITH_*` OFF の build と test。欠落依存の named reject。既存の rollback / 世代 test |

#### v0 で凍結するもの / 広く約束しないもの

| 領域 | 凍結する | v0 では約束しない |
|---|---|---|
| 文書・編集 | 型と port の意味、接続の正本(edge)、永続 ID、CAS と undo の基本契約、schema version の分離(文書 / op の意味 / package / native ABI は別の番号) | 保存の内部最適化、コンパイラの実装 |
| 共通グラフ | definition と instance の関係、owner、値・状態・イベントの区別、**ノードの評価分類**(直接評価 / 初期状態からの replay / ベイク済み / 記録が無ければ不可) | 全 domain の高度な最適化、全種類の完全な seek |
| パラメータ | 固定値と一方向の供給元の一貫した操作、現在値と供給元(instance・port・時計)の観測、**変更時の扱い**(値だけ / 再コンパイル / 再構築)の宣言 | 任意の式言語、双方向 bind |
| 描画 | 同じ scene を使う、対象を限定したラスタとオフライン RT の完成経路。共通に使える部分と renderer profile を要求する部分の明示 | 全 RT 機能の方式間互換、映画向け機能の網羅 |
| 拡張 | package manifest の最小形(名前空間と版、必要な engine / SDK、依存、提供する型・op・部品、必要 GPU 機能、説明)、登録契約(型 / op / sink)、purge ゲート | native ABI の永久互換、稼働中の任意 module の unload |

「一つのグラフモデル」は意味の共通化であり、一枚のキャンバスではない。Studio は制作対象 / 関係 / 時間 / 実行結果の view に分け、同じ正本を見る。authoring graph と展開・コンパイル済みの実行グラフは混同せず、Frame Plan から元の feature・node・コードへ戻れるようにする(生成結果を第二の正本にしない)。

#### v0 の着地と v1 へ残した範囲

| 段 | 内容 | 閉じ方 |
|---|---|---|
| **AG-0 登録契約と purge** ✅ `c9085d6f` | module が型(opaque handle + identity + 世代)・op(`GraphOperationContracts`)・sink を登録する契約を明文化。animation を外した build で共通グラフが compile・評価・描画できる | `PELICAN_WITH_ANIMATION` OFF の build と test。未登録の型・op を使う文書は `graph.type_unregistered@1` 等で名前付き拒否 |
| **AG-1 pose モジュール** ✅ 1a `ee751752` / 1b `129eb218` | `pelican.pose@1` の型・op・sink、pose graph 経路のloop/speed/start_timeとowner単位の排他を実装。v0はanimation componentのclip/graph経路も維持する | 共通graph経路のparityとOFF buildのfail-fastを確認。経路の一本化とtyped eventの拡張はv1 |
| **P-1 パラメータ mode と由来** ✅ `3611e390` | Inspector の欄に mode(固定値 / 接続)と導線(曲線で制御 / 別の出力に接続 / 供給元を開く)。「接続」は共通グラフの edge そのもの(Inspector と graph は同じ edge の別 view)。parameter 宣言に「変更時の扱い」を持たせる。`get_status` の実効値に instance・port・時計を載せる | GUI で「曲線で制御」→ 文書に `value.curve` / `value.playback` と edge が生える。graph 側で edge を消すと欄が固定値に戻る。分類「値だけ」以外の欄は接続不可と表示され、RPC でも拒否 |
| **P-2 往復のパイロット 2 本**(a ✅ 1a `60e95f53` / 1b `6e04c7c8` / 1c `e3a2d3d0` / 2 `340f8537` / 4a+3 `7e983ed9`。b ✅ P-2b `400820f0`)| (a) Bloom: 使う → 供給元を接続 → 内部を見る(参照を保つ)→ このプロジェクト用に派生 → 共有部品として保存 → 別プロジェクトで使う。(b) 曲線駆動の blend: 姿勢部品で同じ往復(AG-1 の受け入れを兼ねる) | 「内部を見る」で materialize されない(標準部品の更新を受け続ける)。派生後は元を変えない。保存した部品が別プロジェクトで同じ出力 |
| **AG-2 root motion** ✅ `37cc55ba` | v0はclip_sampleから根の移動量を抽出し、個体のmovementへ渡す | clipによる移動と出力を外した対照を確認。state_machine / blend経由と回転量はv1 |
| **O-1 オフライン出力** ✅ `fcbe1a98` | `--start-time`による初期状態からのreplayと連番出力 | 同じ時刻のframe等価をbytesで確認。評価分類の強制と状態を進めないread-only renderはv1 |
| **X-1 持ち出し** ✅ `64eae566`(package manifest / vendoring / namespace 付き catalog / 公開 SDK だけの include を compile で判定)| 本体ツリー外のテンプレート repo(graph / feature パックが主力、native 拡張は SDK とビルド条件を固定)。package manifest とカタログ(Studio 検索・CLI 一覧・エージェント向け) | シナリオ「拡張して持ち出す」の test |
| **AG-3 観測と view** ✅ 3a `32d9a20b` / 3b `ce5e96c0` | `get_status.animation` / `get_graph_node_values`と、Studioの値の帯・Live・Timelineを接続 | `graph_observe_roundtrip`で装着→編集→観測→保存→再読込。pose/event固有の未確認範囲は凍結一覧§4 |
| **R 群(整理・操作感・サンプル・凍結)** ✅ R-1 `96d5e1d8` / R-3 `be4c60a4` | 二つの正本の撤去/維持を裁定し、サンプル索引・凍結一覧・requiresを整備。2026-09-22の追加修正は凍結一覧§5 | 最終release構成の確認後にtagを判断する。複合Undoと代表ゲームの拡張はv1 |

#### v0 の現状と残り（2026-09-22）

AG / P / O / X / R / N / Q の着地 SHA は [v0 凍結一覧 §5](v0_freeze.md#5-v0-で入ったもの)、
途中の判断・検証件数は [日付付き履歴](history/v0_delivery_2026-09.md) にまとめた。
2026-09-17 の engine 全数 1947 / 1947 は当時の構成の結果であり、現行 Studio を含む release の完了宣言ではない。

| 区分 | 現在の扱い |
|---|---|
| 実装・対象回帰を確認済み | scene authoring ID の永続化、Graph Canvas の基本 Undo/Redo と配置の同時復元、未保存 scene の close/project 切替ガード、終了通知による Studio crash の修正、build ごとの runtime 配置と Runtime install、`requires` に沿った configure target 選別 |
| v0 の再検証 | 新規projectの編集・保存・完全再起動と出荷6 sampleの別project経由再読込はWindows Runtime stageで確認済み。最終revisionのGPU / Studio gateは別に完了を判定する。旧 hang・OFF build・shader / AS の指摘は [凍結一覧 §4](v0_freeze.md#4-v0-の状態と再検証) の範囲で再現を判定する |
| v1 の機能・操作拡張 | scene と graph/render config を束ねる複合 Undo、外部 definition 参照と create/delete/copy、package の shader・名前空間・catalog 拡張、read-only render と評価分類の強制、代表ゲームの物理・音・save/load・UI。個々の仕様は既存 WP / 設計に従う |
| 未予定の操作改善候補 | parameter_dependent な render op の palette form、seed constant 自動掃除、必須入力 disconnect UX、Features dock の export/import UI、node text elision、animation sink / scrub の GPU coverage。現状の不足を利用操作で確認してから選ぶ |

Graph Canvas の基本 Undo は `0d41aeba` で実装済み。render config 全体や scene journal と跨る複合 Undo は別の未完了項目である。
scene owner ID の永続化も `efb30dad` で実装済みのため、新規開発候補には戻さない。

#### v0 を閉じるときに v1 へ送ったもの(2026-10-10)

公開前のバグ探し(発端は SK-1。WP の行は [台帳](implementation_plan.md) の V0-ADD の WP 索引の GA-1〜CB-1)の中で、v0 では直さないと決めたもの。

| 項目 | v0 での扱い | v1 で行うこと |
|---|---|---|
| VAT を部品単位へ | VAT の texture と再生の情報は glTF の読み込みで material に焼かれる(`registerVatMaterial`、`src/core/model/gltf.cpp`)。SK-1(`e265e5658`)で、VAT の部品に別の material を当てる 3 経路(`material_bindings`、scene の `simplemodelview.materials`、RPC `bind_material`)を拒否した | VAT の texture と bounds を部品の側へ移し、VAT の部品にも project material を当てられるようにする(統合側の記録では SK-2 の名) |
| scene journal を起こし直しの先へ | 起こし直し(`restart_required`)で engine の中の scene の editor journal が消え、scene の Undo が効かなくなる(scene の中身は残る)。2026-10-08 の利用者の決定「v0 では直さない」。QD-1 の Undo の検査に印が残る(`test/studio_scenarios/QD1_FINDINGS.md` の Bundle B) | journal の持ち主は engine のまま、起こし直しの前に古い engine から取り出し、Save の後の scene の digest が一致するときだけ新しい engine に渡す(統合側の案。起こし直しで意味が変わる値の洗い出しが先)。上の表の「scene と graph/render config を束ねる複合 Undo」とは別の項目 |
| 依存の同梱(DS-1c) | 依存は configure のときに FetchContent が git で取る | `third_party/` に同梱し(`cmake/pelican_third_party.cmake`)、git もネットワークも無しで configure する(opt-in の Jolt は FetchContent のまま。`agent/ds1c` の `cmake/pelican_third_party.cmake` の注記)。`PELICAN_FETCH_DEPENDENCIES`(既定 OFF)で git から取る道も残す。branch `agent/ds1c` にあり、公開の後に着地させる |
| VD-1(補助 pass の頂点の hook) | surface の `vertex_displace_v1` hook は material の頂点 shader にだけ効き、shadow / velocity / picking の engine 固定の頂点 shader(`src/core/resources/shadow_depth.vert`・`velocity.vert`・`picking.vert`)には無い。hook で頂点を動かすと、影・速度・picking は元の形のまま。CB-1(着地待ち、`agent/cb1`)の組 08 で確認した(known_red。`test/CB1_FINDINGS.md`) | hook のある material は、補助 pass でも material ごとの頂点 shader を使う |
| USD の import | engine は USD を読まない。USD は外部の import の道具のレシピで運ぶ([manual 5 章](manual/05_assets.md) の USD レーン)。外の model を Studio に持ち込む道(MI-1 / MI-1s、`.glb` / `.vrm`)は v0 に入れ、USD の import は v1 とした(2026-10-04 の利用者の決定。統合側の記録) | 範囲は v1 で決める(既存の U-USD1 系との関係は未確認) |
| WEB-1 | web の parser を v0 形式(scene 2 / asset_data 2 / `pelican.graph` root)へ合わせる WP(別 repo。[設計](design_v0_production_gaps.md) §2、台帳の V0-ADD の WP 索引)。v0 では行わない(2026-10-04 の利用者の決定。統合側の記録) | v0 の後に別 repo で行う。web が読まない形は名前付きで断る |

#### 部品の 3 操作(Houdini HDA を参照)

- **内部を見る**: 参照関係を保ったまま、構成・値・コードを確認する。開いただけで materialize しない。
- **このプロジェクト用に派生させる**: 独立した定義を作り、元の標準部品を変更しない(既存の fork / materialize / 区域差し替えをこの語彙に揃える)。
- **共有部品として公開する**: 公開する入出力と依存関係を整理し、再利用可能な単位にする。
- prefab / graph definition / render feature を同じ文書形式に潰さない。「公開インターフェースを持つ再利用部品」としての操作感だけを揃える。
- 最適化版と分解版は、公開される設定と意味の契約を一つにし、実行実装を複数にする。利用者が内部を書き換えたら元の高速実装を無条件に使わない。コードノードはコードノードのまま残り、入出力・副作用・ソースが見える。

#### module と package

- **module** = 型・op・sink を登録する実行上の単位。**package** = 文書・shader・コード・説明・サンプル・依存関係をまとめて配る単位。package はコードを持たなくてよい(「プラグイン = DLL」にしない)。
- 公開の種類: graph・機能パック(最初の主力)/ native 拡張 module(SDK とビルド条件を固定、再ビルド許容)/ 外部ツール連携と編集支援(文書形式と公開 RPC を境界に、実行コアから分離)。
- 標準部品は本体 repo で共同開発するが、登録契約・依存・purge test は外部拡張と同じ境界を通す。実験的なものは Labs 相当の公開単位。外部拡張テンプレートだけは別 repo で成立させる。導入は固定 revision からの展開で足り、作品が使った部品の版を固定できることを優先する。
- パージ可能と稼働中 unload は別。v0 はビルド時に外せることと起動時の依存検証を保証し、hot reload は寿命管理を検証した種類に限る。

#### やらない(v0 の後、またはパージ可能モジュールとして)

双方向 bind / 任意の式言語 / layer・additive・IK・手付け / 2D ノード集(instanced raster + compute + attribute buffer の副機能群)/ 映像向け合成ノード / timeline 文書(時間軸は view)/ 物理 → graph 入力 / native ABI の永久互換 / 任意 module の unload / マーケットプレイスと細分化された repo 群。

#### 参照先(照合用。断定ではない)

部品化 = Houdini Digital Asset(内部展開、parameter promotion、編集許可と定義への復帰)。パラメータ操作 = TouchDesigner の Parameter Mode と Component Time。シーンと評価 = Gaffer の Box / Reference / Context。描画拡張 = NVIDIA Falcor(`reflect()` と `execute()` の分離、Mogwai)。描画方式の境界 = Hydra の scene index / filter / render delegate、MaterialX の NodeDef と実装の分離。状態と時間 = Blender Simulation Zone(前状態・キャッシュ・ベイクの境界)。登録と発見 = VS Code の extension manifest と contribution point。コード側の説明をノードへ反映 = vvvv。

### 共通グラフの現在地と v1 の接続拡張

共通形式 `pelican.graph` の definition / instance / edge、数値・描画・pose の module、
Scene owner と Studio Canvas は接続済み。外部 authoring の正本は共通文書であり、
旧 `pelican.value_graph` は内部 IR / test 面に限る。animation component の
`clip` / `graph` 経路は v0 で維持すると決めた経路で、未完了の撤去作業ではない
（[凍結一覧 §2](v0_freeze.md#2-撤去--維持の表)）。

数値カーブから resource buffer、評価済み pose から描画への接続は実装済み。
物理結果を共通 graph の入力へ供給すること、スキニング済み GPU 属性の共有、
scene ごとの graph 選択、動的 instance の資源回収、任意の外部入力・汎用反復は
v1 の個別設計対象とする。既存の typed resource port と評価・寿命契約を再利用し、
専用 component や暗黙の転送経路を増やさない。

当時の個々の接続・GPU 検証・残件の記録は [共通グラフへの接続経緯](history/v0_delivery_2026-09.md#共通グラフへの接続経緯) に保存した。
履歴にある「Studio 統合未了」「共通化未了」はその後の AG / P / N の着地で部分的に更新されているため、未実装の一覧として読まない。

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
