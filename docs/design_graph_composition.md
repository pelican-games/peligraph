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
