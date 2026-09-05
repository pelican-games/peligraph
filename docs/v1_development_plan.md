# v1 の完成条件と開発順序

2026-09-05。利用者がこの日の対話で確定した製品方針を記録する。
WP の仕様・着地状態の正本は [implementation_plan.md](implementation_plan.md)。
本書は機能を削減する裁定や、未実装 WP の仕様を新設するものではない。

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

## RT と編集面の初期棚卸し

RT3〜RT7 は v1 の対象候補として全て残す。AS の graph node 化は学習目的の中心候補であり、後続へ自動的に送らない。
段の名称・依存は [design_raytracing.md](design_raytracing.md) §8・§11 と台帳の最終差分に従う。
「記録上の完了」「今回確認したコード・ログ」「未確認」を区別する。既存の完了記録を全て未確認へ戻さない。
下表は着手順の判断材料であり、コードレビューや全数試験の合格証明ではない。

| 領域 | 学べる概念・v1 で扱う内容 | 現在地と根拠 | ノード操作・観測・サンプルの残確認 |
|---|---|---|---|
| RT1 / RT2 | raygen・miss・closest-hit・any-hit、再帰、SBT addressing、material ABI | 台帳 WP364〜368 に実装・検証記録あり。今回の統合ログで WP367 の本番 any-hit 試験が成功 | stage/record/material の結び付きの可視化、設定変更と GPU 結果を試す教材は完了未判定 |
| RT3-1 | instance mask/flags/custom index と geometry flags | WP374/374a 着地済み。WP384-fix 後の対象 GPU/RPC 4 件が統合ログで成功 | 個体編集→GPU 効果→保存→再読込を GUI から通す。既存 RPC の証拠を GUI 完了とは扱わない |
| CARRIER-RT3 | 再帰 schema、可変長行、occurrence、個体ごとの geometry override | WP382a/b/383 着地済み。382b-fix は 36ad3444 に着地(関連37/37、レビュー残余あり)、382c は 23cb5153 に着地(W1 98/98、レビュー残余あり)、382d は4ce4117dに統合(W1 95/95) | 行の追加・削除・移動、対象 geometry の選択、override の出所表示。複合undo等の残余を閉じ、CARRIER-STUDIO との接続を確認 |
| RT3-2 | static AABB と intersection shader、三角形との混在 | WP386a/b統合済み。球のreport・reload・変更行列を確認し、386cの数値基礎はae29fc98まで保存。数値基盤の追加実装は保留 | Playerで開ける学習サンプルを入口に、配置・サイズ・bounds・mixed geometryを実描画で確認する。数値oracleの完成を次機能の条件にしない |
| RT4 | callable、per-in-flight の動的 record | callable実行・reload、動的recordの宣言・更新・submission寿命管理、GameContext public APIを実装。ゲームDLLからの更新を実GPUで確認 | Studioのrecord編集UIは残る。次はRT5の変形geometry |
| RT5 | 変形 geometry の毎 frame rebuild/refit | skin/morph/VAT の compute 変形 → BLAS rebuild を接続。個体別変形とVAT補間によるヒット追随を実GPUで確認 | refit と更新負荷の削減が残る |
| RT6 | dispatch 一般化・indirect、AS graph node、複数 TLAS、複数 geometry BLAS | 画像由来サイズに加え、明示3DサイズとGameContextからのサイズ変更を実装・実GPU確認 | RT indirect、ASノード、複数TLAS・複数geometry BLASは残る |
| RT7 | ノードの一級化、編集 RPC、fork/CAS | 計画あり。Studio に frame plan model/graphics/read capability は存在するが、RT 全面編集の証拠ではない | 接続編集・診断・保存・再読込・失敗時の内容保持を通す。画面上のノード移動と実行 graph の編集を区別 |
| ゲーム制作 | component と scene、入力、物理、animation、音、UI、保存、配布 | 各機能の記録は manual/台帳に存在。代表ゲームでの一連の制作経路は今回未確認 | 実制作で詰まる操作を GUI 改善の優先順位に使う |

確認した実装入口: [pipelinefactory.cpp](../src/core/shader/pipelinefactory.cpp)、
[RT dispatch](../src/core/vkcore/raytracingpipelinedispatch.hpp)、
[AS](../src/core/renderer/accelerationstructure.cpp)、
[frame plan graphics](../src/devstudio/view/frameplangraphics.cpp)、
[read capability](../src/devstudio/view/frameplanreadcapability.hpp)。
次の詳細化では各行に既存テスト名・対象構成・故障経路・不足する操作を追記する。表を埋めるためだけに新テストを増やさない。

## 開発順序

1. 現在の修正・着地待ちを台帳の再開順で収束させる。WP384-fix の全数は完走し、既知 gate 失敗とレビュー残余を台帳に記録済み。
2. 上表を具体的な操作と既存の証拠へ対応付け、RT4〜RT7 の詳細仕様の所在と不足を確認する。
3. RT と carrier の実装を進めながら、既存の frame plan 表示を利用して観測と最小編集を早めに通す。
   最初の編集候補は instance 制御だが、mask 等も AS 更新・再利用判定に関係するため「軽い編集」と決めつけない。
   layout/record/接続変更は所有・世代・保存への影響を先に整理する。
4. RT4/RT5 → RT6 → RT7 の依存を満たす。先行する GUI は最終編集モデルへ接続し、別の実行系を作らない。
5. 学習サンプルと代表ゲームを通し、操作の一貫性、診断、性能、配布物を確認して公開条件を閉じる。

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
- 整理は独立した小変更で行う。第一候補は ray layout v1 の利用箇所、v2/v3 DLL 登録入口、RT oracle の呼び出し元、出荷対象の調査。

## Fable との相談

Codex が調査・実装・検証・統合・利用者への報告を担当し、Fable は重要な設計判断の独立相談役とする。
所有・寿命・保存契約・共同 transaction 等に重点を置き、全作業へ新たな多段レビューを追加しない。
原則は初回検討と論点を絞った再確認 1 回。既存 WP で確定した受け入れや打ち切り規約は勝手に変更しない。
採用・保留・不採用と根拠を残す。二者の合意はコードや実測の代わりにならない。
2026-09-05 の接続試験は Claude Code 2.1.260 / claude-fable-5-1、fast OFF、読み取り限定で成功。
原文はローカル `<作業ディレクトリ>/fable_trial_20260905/discussion.md`。共有可能な判断は本書と台帳に残す。

**2026-09-05 最新の優先順位:** 利用者の指示により、学習サンプルの作成・拡充は後回し。エンジン中核のRT4以降の機能完成を優先する。callableの宣言・コンパイル・SBT・実行・単独reloadを実装し、実GPUで確認。per-in-flight動的recordは次の実装とする。
