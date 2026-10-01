# 日本語資料と実装の照合記録（2026-10-01）

確認対象は `develop` の `c6df4822`。利用マニュアルとソースコード読解ガイドを中心に、用語、案内、仕様の断定、ソース参照を確認しました。エンジンの実装は変更していません。

## 実装・説明の食い違い

| 以前の説明 | 訂正 | 確認先 |
|---|---|---|
| AS は静的ジオメトリ専用 | スキン・モーフ・VAT の変形に対応し、条件に応じて BLAS を更新する。用語集とガイド索引の古い制約を削除 | [形状分類と BLAS 構築](../src/core/renderer/accelerationstructure.cpp)、[変形ジオメトリの解説](source-code-guide/11_ray_query_acceleration_structures.md) |
| 変形形状の説明直後に「この除外」と書かれ、変形形状自体を除外するように読める | 除外対象は BLAS の入力条件を満たさない形状。変形方式別の診断件数と除外理由を区別 | 同ファイルの `classifyRayQueryGeometry` |
| prepare / publish / commit の一般論として、publish が no-throw なら部分反映が原理的に起こらない | シーンロードでは反映後の activate も失敗し得る。commit 前の巻き戻しと、各段階の保証を区別 | [SceneLoadParticipant](../src/core/loader/sceneloadparticipant.hpp)、[シーンロード](../src/core/loader/scene.cpp) |
| 物理 provider の選択はビルド時 | 組み込み実装のビルド設定と、ゲーム DLL の実行時登録を区別。必要機能ごとに供給元を選ぶ | [providerForCapabilityLocked](../src/core/phys/physicsruntime.cpp)、[既存テスト](../test/physicsservice_test.cpp) |
| provider を替えても結果が変わらない | 整列規則は共有するが、異なる実装の数値結果の完全一致までは保証しない | 同上 |
| resolve は必ず絶対ファイルパスを返し、すべてプロジェクト内に収まる | engine リソース ID、user 領域、asset store、フラグメントを区別 | [ProjectPathResolver](../src/project/projectpathresolver.cpp) |
| 同じ入力なら常に同じ結果 | 時刻列・実行環境の条件を明記し、GPU 画像のビット一致を暗黙に保証しない | [EngineTime](../src/core/appflow/enginetime.cpp)、[描画比較の許容差](../test/golden/vrm_xr_demo/tolerance.json) |
| 通常機能は全部既定 ON、例外は SPIR-V linker だけ | Jolt などの既定 OFF も明記し、生成される機能台帳を参照 | [機能設定](../cmake/pelican_feature_registry.cmake)、[ビルド機能台帳](build_features.md) |
| Renderer の別々の overload への案内が同じ行を指す | 単一ビュー群、複数ビュー群、通常画面、引数なしの関数を別々の行へ案内 | [renderer.cpp](../src/core/vkcore/renderer.cpp) |
| shader compiler が結果オブジェクトでエラーを返す唯一の例外 | 他の Status / Result 型を使う API と矛盾するため「唯一」を削除 | [ShaderCompileResult](../src/core/shader/shadercompiler.hpp)、物理・アニメーション API |
| 文書入口に7月の残件が現在の次の作業のように並ぶ | 古い一覧を入口から外し、現況・公開仕様・過去の WP 履歴の参照先を分ける | [v1 開発計画](v1_development_plan.md)、[v0 凍結一覧](v0_freeze.md) |

## 日本語・用語

- 「relocate 舞踏」「3 回舐めてから swap」「publish しない publish」「舐め直しで閉じる窓」を、要素移動・検証・反映の保留・ファイルの再走査という具体的な処理名へ変更しました。
- 「無音で inactive（警告あり）」を「警告を出して非アクティブになる」へ訂正しました。
- 「顔」「縫い目」「追加交渉面」「宣言レーン」など、見出しだけでは内容が伝わらない表現を言い換えました。
- ガイドの難所索引を、略語を連ねた目印から「その章で何が分かるか」の案内に整理しました。
- [用語集 §14.8](manual/14_glossary.md#148-設計コード解説で頻出する用語) に、契約・正本・面・IR・lowering・capability・digest・CAS など22項目を追加しました。

## 確認範囲

`doclink check` はソース参照5,894件で問題なし、`audit` は参照先の不一致・存在しない識別子とも0件でした。HTML の厳格モードのビルドに成功し、生成物30 HTML（資料29ページと404ページ）のリンク・表示資源の参照12,974件に欠落はありませんでした。公開対象の限定とリンク変換の回帰テスト8件も成功しています。ブラウザではレイアウト、日本語検索（「加速構造」で検索結果を表示）、ガイドの全体構造図の描画を確認しました。

これは全設計書・全コード行の意味を網羅した再検証ではありません。過去の設計案・検証記録の内容を現在の仕様で上書きせず、履歴として残しています。C++ 本体は変更していないため、エンジンや GPU の全テストは再実行していません。
