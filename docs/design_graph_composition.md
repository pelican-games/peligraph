# 全体グラフとコンポーネント表示の共通化

2026-09-07。利用者の「全体グラフと一緒に記述し、Studioの見せ方を分ける」
という方向に合わせた設計。**共通化の実装は未了**。コンポーネントごとに
独立したグラフ実行・保存系を追加する前に、本方針へ接続する。

## 現状と再利用する部分

- animation componentはgraph asset参照と個体別parameterを保存し、
  AnimationSystemがEvaluatorV1を装着する。数値グラフとは別の文法・所有。
- ValueGraph::Graphは数値ノードとbindingsを保存し、publishで音声・描画へ
  専用kind分岐を通して反映する。これは現在動作する経路だが、全体グラフの
  共通接続表現を完成させたものではない。
- [LogicalPortContract / LogicalResourceUse](../src/project/logicalrendergraph.hpp)は
  型付きポート・値のread/write・依存を既に表現する。
  [LogicalType](../src/project/logicalrendertype.hpp)にはvalue/stream/buffer等の
  型構築子と登録・照合の仕組みがある。共通化で別の型体系を作り直さない。
- CompiledLogicalRenderGraphのnode kindはrender/compute等であり、view_familyや
  render向けaccess intentを持つ。これは描画バックエンドの派生結果として残す。
  region_tagsとsubgraph replacementは最適化領域を表し、コンポーネントの
  インスタンス所有や汎用のサブグラフ定義を、そのまま表しているわけではない。

## 正本とインスタンス

### 最初の実装範囲（2026-09-07）

[graphcomposition](../src/project/graphcomposition.hpp)へ`pelican.graph` version 1の
サブグラフ展開を追加した。rootはdefinitions/nodes/outputsを持ち、definitions内の
定義もnodes/outputsを持つ。これはcommon authoringの入口であり、まだ既存の
render pipeline authoring全体やStudioへ統合したものではない。

- primitive nodeはid/op/inputs/parameters/outputsを持つ。inputsはport名から
  `{node, port}`への対応、outputsは公開port名の配列。実行操作の意味はbackendが扱う。
- graph.instanceはdefinition名とinputsを持ち、定義の公開出力を通常のportとして
  接続する。定義のgraph.input nodeのidが公開入力名で、そのvalue出力へ置換する。
- 展開後はinstance pathでnode idを区別し、originsにinstance/definition/nodeを
  保持する。原文の定義・instance・接続は変更しない。外部ファイル定義の解決は未実装。
- ValueGraph::Graphがこの形式を受け、value.* primitiveを既存の数値評価へ変換する。
  render.scalar/audio.volume/audio.pitchは通常のvalue入力portを持つsinkで、既存の
  サービス反映へ変換する。serializeは共通形式の原文構造を残し、生成bindingを保存しない。
  evaluateの公開出力にも生成binding用の内部出力を混ぜない。

共通port照合は既存LogicalTypeRegistryを使う。operation提供側が入力の型patternと
必須性、出力の具体型を定義し、展開後のinstance間edgeも同じ規則で照合する。
数値backendのvalue.*と描画/音声scalar sinkはpelican.value.scalar@1を使用する。
型不一致・不明port・必須入力欠落をコンパイル前に拒否し、暗黙変換は追加しない。
playbackの動的設定もinputsへ接続し、parametersは数値literalとして扱う。
型定義は保存文書へ複製せず、公開出力の型は接続元から求める。

全領域のoperation契約、汎用resource宣言、既存render featureの取込み、scene所有/状態、Studioの
scope表示は残件。render.scalarのtarget名は既存render bufferを参照する境界であり、
すべてのrender resourceを同じauthoring文書へ統合したという意味ではない。

編集対象は一つの論理的な接続モデルとする。一つのJSONファイルへ全内容を
埋め込むことは要求しない。再利用する定義は別assetに置けるが、参照を解決した
ノード・ポート・接続・インスタンスparameterの意味は全体を通して一意にする。

コンポーネントは、そのモデル上のサブグラフインスタンスとして表す。
インスタンスは定義参照、公開ポート、parameter、対象objectとの対応を持つ。
同じ定義を使う二つの個体は別のinstance identityを持ち、時計・状態・音声等の
実行時所有を共有しない。生のEntityId/SoundHandle/GPU handleはassetに保存しない。

外との接続は公開ポート同士の通常のedgeとして記述する。コンポーネント側の
接続表と全体グラフ側の接続表を二重に保存して同期する方式にはしない。
Studioのコンポーネント表示は、同じモデルのscopeによる抽出・折り畳み・
公開parameterの編集であり、実行や接続の意味を変えない。

保存するauthoring identityとコンパイル結果のnode identityは分け、変換結果から
元のinstance/node/portへ追跡できる対応を保つ。共有定義の編集と単一instanceの
展開編集は区別し、後者で他の個体を変更しない。

## コンパイルと実行

### インスタンスの入力パラメーター（2026-09-07）

graph.inputはdefaultに有限のscalarを持てる。graph.instanceのparametersは公開入力名から
scalarへの対応で、定義を複製せずに個別の値を保存する。接続を与えた入力はedgeを使い、
未接続入力はinstance.parameters、次にgraph.input.defaultを使う。どれもなければ未接続
エラー。同じ入力へのedgeとparameterの二重指定、不明parameter名は拒否する。
rootにもparametersを指定できる。buffer/image等へのliteral変換を暗黙に追加しない。

literalで束縛した入力は、展開時にinstance pathを名前とするvalue.parameterへ変換する。
ValueGraph::Graph::parameterDefaults()で有効な名前と初期値を取得でき、evaluate/publishへ
ParameterValuesを渡して、その評価だけを上書きできる（例: left/gain）。不明名や非有限値は
拒否し、Graph本体とserialize結果は変更しない。通常Rendererは保存値を使用する。

2instanceの異なる保存値、片側だけの上書き、定義defaultへのfallbackを確認。実GPUでも
上書きによる出力変更と、上書きなしの次回評価で保存値へ戻ることを確認した。
このparameter表現を使い、以下のobject所有と通常Rendererへのoverride供給を接続した。

### objectに所有されるparameter（2026-09-07）

graph.instance.ownerに`{"scene":"scene名","object":正のauthoring object ID}`を保存する。
runtimeのEntityIdを保存しない。ネストしたparameterは最も近いowner宣言に属する。
Graph::parameterOwnerは、存在するparameterに対応する保存上の所有者を返す。

GameContext::setGraphParameter(object, "left/gain", value)で実行時overrideを設定し、
value=nulloptで解除する。公開中のグラフにparameterがあり、現在sceneのauthoring IDが
その生存GameObjectへ対応する場合だけ受け付ける。別個体・削除済みID・不明parameter・
曖昧な複数グラフへの参照はfalse。sceneのeffect transaction内でもfalseとする。

SceneLoaderはグラフごとに弱参照でoverrideを管理する。別グラフの同名parameterは独立し、
GPU世代退役後も古いGraphを保持し続けない。削除済み個体のoverrideは次の取得/評価時に
除去し、scene commitでは全件破棄する。新しい描画グラフは保存値から開始し、失敗した
描画更新では旧グラフのoverrideを維持する。通常Rendererがこのsnapshotをpublishへ渡す。

owner不在時の処理停止は、以下のscope依存と通常Rendererのフレーム評価へ接続する。
objectごとの動的instance生成/削除、状態付きノード、イベント/音声handleの所有は残件。

実行停止への準備として、展開結果にnode_scopes/output_scopesを追加した。各nodeと公開出力が
必要とするowner scopeを、親scopeと入力依存からコンパイル時に求める。子に別ownerがあっても
親の生存条件を外さず、依存先へ伝播する。入力をそのまま公開出力へ渡すだけのinstanceも、
alias展開で生存条件を消さない。共有入力の生成元や独立した兄弟へ逆向きには伝播しない。
数値→bufferの生成upload sinkとGPU側partitionにも同じ条件を保持する。これは派生metadataで、
保存JSONに展開結果を追加しない。

通常RendererはSceneLoaderから不在scopeの集合をフレームごとに取得し、数値publishと
各viewのcompute実行で共有する。sceneがない場合もowner付きscopeは不在として扱う。
ValueGraphは不在scopeに依存するnodeを評価せず、その公開出力を返さず、音声/描画sinkへの
反映も行わない。停止を出力先の故障としてunavailableへ報告しない。手動evaluate/publishは
inactive引数で同じ条件を指定でき、省略時は従来どおり全scopeを有効とする。

共通graphのrender.computeは同じ依存条件でdispatchを停止する。資源やbarrierの計画は維持し、
停止した出力bufferは既存の内容を保持する。後続の共通graph consumerも停止条件を受けるため、
停止値を新しい結果として消費しない。scene再読込みでownerが再び存在すれば保存値から再開する。
root.rendering内に手書きしたnative passの暗黙の資源参照は共通edgeではなく、このscope伝播の
対象外。画像passの共通化、状態/eventの停止・解放、GPU資源の個体別回収は引き続き残件。

### 共通authoringから計算シェーダーへ（2026-09-07）

[lowerRenderGraphDocument](../src/project/rendergraphcomposition.hpp)を既存の
composeRenderFeatureConfigへ接続した。pelican.graphを渡すと、共通instanceを展開して
以下のnodeを既存buffers/compute_tasksへ変換し、通常のpreset/feature構成へ渡す。

- render.buffer: parametersに既存buffer設定（size/lifetime/host_source等）とelementを
  保存し、resource出力を公開する。nameはinstance pathとnode idから生成する。
- render.compute: parametersに既存shader参照/dispatch設定とportsを保存する。
  portsは名前ごとにelementとaccess（read/write/read_write）を持ち、同名のinputsで
  bufferのresource出力またはcomputeの書込み結果へ接続する。write/read_writeのportは
  同名の出力を公開し、そこへの接続から完了依存も生成する。buffer自体の参照と書込み後の
  参照を区別し、宣言順で意味を変えない。公開したsubgraphの出力も接続可能。
- storage_bufferの型引数elementを既存LogicalTypeRegistryで照合する。reads/writesと
  shader resource_portsはedgeとaccessから生成し、別途の手書きを拒否する。
  同じbufferへの複数portは読み書き一覧では一つにまとめる。
- rootのrenderingは既存の全体設定・preset/feature参照・画像passを保持できる移行用の
  入口。ここにbuffers/compute_tasksを重複宣言しない。生成configは保存の正本に戻さない。

CPU上の既存解決・compute/buffer契約検査と、instance間の書込み→読込みの実行順を確認。
続いて、混在文書の数値→buffer→GPU computeの経路も以下の範囲で接続した。

### 数値と描画の実行分割（2026-09-07）

画像もrender.imageのresource出力で共通graphへ宣言する。parametersのtypeは
`{"semantic":"pelican.render.color_signal@1","arguments":{"reference":"scene","transfer":"linear","range":"extended"}}`
のように既存LogicalTypeRegistryのimage型を指定し、残りのformat/extent_scale/usage等は
既存render target設定へ渡す。保存上の名前はnode idから生成する。物理formatと論理的な
色/深度の意味は別であり、接続時には後者を既存の型規則で照合する。

render.computeの画像portはkind=image、type、access=read/write/read_writeを持つ。
binding=storage（既定）またはsampledと、既存のview/sampling/subresource設定を
shader resource_portsへ渡す。buffer portの省略kindはbufferのまま。画像とbufferの
取り違え、色のreference/transfer/rangeやtopologyの不一致は暗黙変換しない。
書込み結果portからの接続は画像でもafterを生成する。派生render targetはpreset展開後に
追加し、通常の資源登録・shader interface・barrier計画を使用する。
画像を扱うcomputeのowner停止も共通edgeを通じて伝播する。raster/fullscreen passの
共通node化とattribute scopeはこの画像resource/compute接続とは別の残件。

[partitionGraphDocument](../src/project/graphpartition.hpp)が全体のport型を照合してから、
既存の数値評価と描画コンパイルへ渡すnode/outputを分ける。両backendが同じ分割処理を
使い、未登録operationや領域間の型不一致を、片側で無視する形にしない。

render.bufferのvalue入力にscalarを接続すると、size=4・host_source=runtime_f32の
float bufferと既存render.scalarによるpublish sinkを派生させる。バッファ名を接続側へ
手書きせず、instance展開後のidで対応づける。この境界は現在1個のfloat用であり、
別element/size/host_sourceとの組合せは拒否する。vector/structの転送は今後の拡張。
入力のないbufferは既存の宣言方法を維持する。

ValueGraph::Graphは共通原文を保存し、evaluateには数値の公開出力だけを返す。
生成したsink/転送設定やGPU resource出力を数値の公開出力へ混ぜない。同じserialize結果を
描画composeへ渡せる。publish→既存upload→ComputeTaskContainerのdispatchを実GPUで
実行し、2つの入力でshaderの計算結果が2.5/5.5に変化することを読み戻して確認した。
この直接dispatchの確認に続き、下記の通常Renderer経路も接続した。

### 描画世代に保持する数値処理とscene時計（2026-09-07）

共通文書から作ったValueGraphをCompiledFrameGraphExecutionへ保持し、既存の描画世代の
準備/公開と一緒に切り替える。公開前に入力・出力値とbufferの存在/host_source/サイズを
検査する。失敗した候補は公開中の数値処理と描画資源を置き換えない。

通常Rendererはruntime_f32のupload直前に評価する。入力はtime（EngineTime.now）、
delta_time（EngineTime.dt）、scene_time（nowから最後に成功したscene読込み時刻を引いた値）。
scene開始時刻はSceneLoaderのmetadata commit後に更新する。setTimeによる時刻変更は
これらの計算にも反映する。scene_timeはアニメーション専用の独立時計ではない。

Graph::publishへ渡すrender writerが、そのframeのbuffer_bindingsで保持するIDへ転送する。
更新後のグローバル名検索へ戻らず、古い世代を保持するframeの資源対応を維持する。
音声handleの自動解決や任意外部inputの供給はまだないため、未束縛の入力/出力を含む
自動実行候補は公開前に拒否する。手動Graph APIのsound/inputs供給は維持する。

通常Rendererでの実GPU出力、scene再読込みによるscene_timeリセット、数値変更を含む
描画世代更新、未束縛inputを持つ候補失敗後の旧出力維持を確認した。presetと併用する
共通buffer/compute宣言はpreset展開後に追加する。

現在のグラフ文書の所有単位はプロジェクトと描画世代。上記owner指定による個体parameterの
所有は実装済み。sceneごとに別のgraphを選ぶ保存形式、objectへの動的なsubgraph instance装着、
個体stateの寿命と動的な資源回収は未実装。数値/computeの生存条件による停止は上記経路で扱う。

raster/fullscreen画像passの共通node化、attributeの適用範囲、feature自体の共通instance化、状態/イベントの実行境界、
scene/Studio統合は残件。

### アトリビュート・シェーダー・反復の扱い

アトリビュートの生成/付与と固有シェーダーも、同じ編集グラフのノード・資源・
ポートとして表す方向とする。コード本体を外部shader assetへ置くことと、
依存・入出力が全体グラフから見えることは両立する。シェーダー参照、設定、
読み書きするアトリビュートの型・適用範囲は正本に残す。

アトリビュートは頂点・instance・material等のどの要素に属する値かを区別する。
同名のfloatという理由だけで異なる要素数/適用範囲を接続しない。既存の型照合に
必要な意味を追加する位置を確認し、コンポーネントごとの特例にしない。

マテリアル反復の保存粒度は利用者が検討中であり、以下は実装前の暫定案とする。

- authoringで指定した対象集合/選択条件・反復本体・公開入出力を保存する。
- 実行時に列挙されたmaterial一覧、生成draw/dispatch、物理buffer割当は派生結果。
  Studioでは観察可能にしても、通常保存の正本へ全件を複製しない。
- 反復ごとに作る資源、反復間で共有する資源、前回反復から受け取る値は意味が
  異なる。独立な集合処理と順序依存の反復を区別する。
- 明示的に展開して編集した場合は、展開後の構成を正本へ保存する。
  折り畳み表示の解除だけでは構成を変えない。対象集合が後で変化したときに
  展開済み構成へ自動で個体を足すかは、保存方針と合わせて決める。

この段階で汎用foreach文法や、全マテリアルを列挙して保存する経路を追加しない。

### 既存authoringとの接点（ソース確認済み）

[composeRenderFeatureConfig](../src/project/featurecompose.hpp)はpreset参照を解決し、
featureを構成したconfigとfeature_instances、pass/resource provenanceを返す。
materialized_feature observerは実際のparameter binding位置の具体化結果を通知する。
これらを共通の出自追跡へ接続する候補とし、別の展開処理を先に複製しない。

materializeRenderFeatureDocumentはbindFeatureParametersからboundFeatureDocumentを
生成する処理であり、汎用のサブグラフinstance展開やmaterial反復展開ではない。
現機能をそのまま「全体グラフをコンポーネント別に表示できる」とは扱わない。

[materiallowering](../src/project/materiallowering.hpp)はmaterial/surfaceの定義を
shader契約・値/texture binding等へ変換し、core/materialでruntimeへ適用する。
この変換結果の資源identityや列挙を、authoring上の反復の定義と混同しない。

共通化の入口はproject側のauthoring参照解決/構成に置き、既存render向けのconfigと
数値等の実行計画を派生させる方向で具体化する。どの既存構造へscope/instanceを
追加するかは、まだ実装で確定していない。CompiledLogicalRenderGraphのnode kindを
増やすだけの変更では、編集・保存の正本を統一したことにはならない。

### 実行領域への変換

共通の編集表現から参照解決・型照合・境界の確認を行い、各実行領域へ変換する。
既存の描画コンパイラ、骨格Evaluator、数値評価、音声サービスを実行先として
再利用する。各領域のコンパイル結果をStudioの正本として保存しない。

全体の接続を共有しても実行周期は共有しない。frame更新、物理固定step、
音声sample、GPU submissionの境界を保つ。異なる周期をまたぐ接続には、
値の保持/補間、イベント消費、CPU/GPU転送などの意味を明示する。
GPU uploadや音声への反映は既存サービスへ渡す境界処理として残す。

数値とイベントを同じdouble列に押し込まない。状態を持つノードでは、
前回値・遅延・イベント時刻等を明示し、同一評価内の循環と時間をまたぐ依存を
区別する。現在の純粋な数値グラフの循環拒否は、この状態表現の代用にはしない。

標準サブグラフは境界契約を保つ最適化実装へ置換できる。通常の折り畳み表示と
「展開して編集可能にする操作」は別。表示を開閉しただけで最適化を解除しない。
定義を個体へ展開して編集する場合は、その一体化に依存する置換を外し、
公開port・parameter・依存と編集内容を引き継ぐ。再保存で元の定義へ戻さない。

## 移行順と確認する結果

1. 既存authoringの参照解決・feature展開の入口を確認し、共通のscope/instanceと
   port/edgeを置く位置を決める。数値グラフの専用scene componentを先に追加しない。
   共通のID・型・接続表を既存機構と重複させないことを、変更対象の型で示す。
2. 数値サブグラフ一つとrender buffer一つの接続を、その編集モデルに記述する。
   現在のValueGraphの計算処理とruntime_f32転送を再利用する。全体表示とscope表示で
   同じedgeを編集でき、保存・再読込後も同一の接続で評価されるところまで通す。
3. 同一定義の二つのinstanceでparameterと状態を分離し、一方の削除・展開・変更が
   他方へ波及しないことを確認する。その所有経路にscene装着/解除を接続する。
4. 音声・骨格・物理へ接続を広げる。ValueGraph::publishの専用kind表へ種類を
   積み増す前に、共通portと実行境界へ置換する。音声イベントはこの境界で扱う。

既存文書は移行入力として扱えるが、同じ接続に二つの正本を残さない。
未リリースであることを踏まえ、必要な変換を行った後も旧・新runtimeを並走させる
互換機構は既定にしない。現時点で機械的に既存文法を削除する指示でもない。

確認はこの縦の経路を対象にする。全機能の再検証表や学習サンプルを先に増やさず、
型不一致、instance独立性、保存後の接続、実際の出力反映を変更に応じて確認する。
