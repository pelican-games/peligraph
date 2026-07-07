# ゲームロジック実行方式: ネイティブ C++(複数ファイル)

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-07。方針はユーザー決定済み、詳細レビュー前)。
前提: `design_input_actions.md`(Actions API = ロジックの入力面)、
`design_build_tiers.md`、[PF]、`adding_features.md` レシピ 3。

## 0. 方針決定(2026-07-07 ユーザー)

**ゲームロジックは C++ の複数ファイルで書く。スクリプト言語は採用しない。**

既存資産(通称・黒魔術)を土台にする — `userpublic/details/` には既に:

- `registerComponent<T>(name)`: 型消去テンプレートによるコンポーネント静的登録。
  serialize(jsonarchive)連携で **scene JSON からの loadByJson が自動化**される
- `coretemplate` / `coredist`: ECS コアを封じたままユーザー型を注入する機構
- `userpublic` = ゲームから見えてよい API の境界(レイヤ規則の実体)

欠けているのは 2 つ: **システム(毎フレームロジック)の登録面**と、
**プロジェクト側 C++ のビルド取り込み**。

## 1. G1 — プロジェクト内 C++ の静的リンク(エンジン = SDK)

```
myproject/
  project.json
  code/
    CMakeLists.txt        # pelican_game_sources() に .cpp を列挙するだけ
    playercontrol.cpp     # コンポーネント/システムを普通の C++ で
    scoring.cpp
  scenes/ assets/ ...
```

1. **ビルド形態**: プロジェクトの `code/` をエンジンの player ターゲットに
   取り込んでビルドする(`cmake -DPELICAN_PROJECT=path/to/myproject` で
   プロジェクト専用 player が出る)。エンジンはソース同居の SDK であり、
   ABI 安定化問題を回避する(DLL 境界を作らない)
2. **コンポーネント**: 既存の registerComponent 黒魔術をそのまま使う。
   プロジェクト側 .cpp で登録すれば scene JSON から即座に使える
   (`adding_features.md` レシピ 3 がプロジェクト側でも成立する)
3. **システム登録 API(新設・userpublic)**: 同じ静的登録パターンで

   ```cpp
   PELICAN_REGISTER_SYSTEM(PlayerControl, /*order=*/100);
   struct PlayerControl {
       void update(GameContext &ctx);   // ctx = Actions / dt / シーン操作の facade
   };
   ```

   - 実行点は Loop の固定位置(`ecs.update()` の後・renderer の前)で、
     **order 昇順 → 同順は登録名の辞書順**(決定性 — 静的初期化順に依存しない)
   - `GameContext` が API 面: Actions(WP39)、EngineTime の読み取り、
     transform/シーン操作、(将来)音・イベント。**GET_MODULE 直呼びは
     ゲームコードに許さない**(userpublic facade のみ — レイヤ規則)
4. **project.json**: 追加キー不要(code/ の有無は CMake が見る。
   [PF] 改訂なしで始められる)

## 2. G2 — DLL ホットリロード(プレビュー即時性・後続)

- G1 と**同じソース**を game DLL としてビルドし、エンジンが監視・再ロード
  (シェーダホットリロード WP14 と同じ操作感を C++ に)
- 設計課題(G2 着手時に確定): 状態の生存(コンポーネントデータは ECS 側に
  あるので生きる。システム内部状態は POD 制約 or 再構築規約)、
  リロードのトランザクション(失敗時は旧 DLL 続行 — [SF] §7.5 と同じ規約)
- G1 の API を変えないことが制約(ビルド形態だけの差にする)

## 3. 決定性・配布・web

- システムは固定点・固定順で走る。入力はスナップショット(WP37)経由 —
  既存の決定性資産(リプレイ・シナリオテスト)がゲームロジックにそのまま効く
- 配布: プロジェクト = code + data なので dist-config(WP41)と自然に統合
  (プロジェクト専用 player がそのまま配布物)
- **web はロジック対象外**(サブセット原則 — pelican 専用領域と明言。
  web デモにロジックが要る場合はその時に別途判断)

## 4. 実装順(WP 候補)

| 段階 | 内容 | 依存 |
|------|------|------|
| G1a | システム登録 API + GameContext facade + Loop 固定点(エンジン内デモシステム 1 つで検証) | WP39 |
| G1b | `PELICAN_PROJECT` ビルド取り込み + example への code/ 追加(操作できる最小デモ) | G1a, WP40 |
| G2 | DLL ホットリロード | G1b |

## 5. 未決事項

1. GameContext の API 面の初期範囲(v1: Actions / time / transform 操作 /
   ログ。シーン生成・破棄をどこまで最初から入れるか)
2. システムとフレームグラフ CPU 拡張(design_compute_task_graph §6)の将来統合
   (システム = 粗粒度 CPU タスクの最初の住人になり得る)
3. G2 の状態移行規約の詳細
4. エンジン側ビルトインシステム(SeqPlayer 等)を同じ登録面に寄せるか
