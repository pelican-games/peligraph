# シーン形式: pelican.scene v1

対象読者: エンジン担当 + web 担当 + DCC/ツール側担当。
ステータス: v1.1 ドラフト(2026-07-02。レビュー 1 巡反映:
複数シーン/1 ファイルを標準形に、lights を形式レベルでコンポーネント化、
§2-2(ファイル参照禁止)/§2-5(web の未知コンポーネント skip)は承認済み)。
前提: [PF] v6 凍結、[PFW] v1.3 凍結(サブセット原則)、
`design_project_interpretation_layer.md`(解釈レイヤ)、要求書 R7(命名)/R10(バージョニング)。
経緯: RenderWorld の ECS 合意形成を後回しにし、統合ブランチ系列で
シーン形式を独自定義してよいことになった(2026-07-02、implementation_plan §3)。
**現行の scene_data_json から離れすぎないこと**が制約。

## 0. 現行形式(大本)の観察

```json
{ "<scene_id>": {
    "lights":  [ {"name": "...", "type": "spot|point|directional", ...} ],
    "objects": [ {"components": [ {"name": "transform", ...}, {"name": "simplemodelview", "model": "sotai"} ]} ]
} }
```

- **objects = components 配列 + 名前ディスパッチ**(ComponentInfoManager が
  name → ComponentId 解決、`loadByJson` でロード)。つまり形式は既に
  「コンポーネント登録で拡張可能」であり、この骨格は変えない
- lights は components ではなく別セクション(LightContainer 管轄)
- model 参照はパスではなく **asset_data_json の id**(間接参照)
- 欠けているもの: schema/version ヘッダ(R10 違反状態)、オブジェクト名、
  シーン分割の規約([PF] §8 未決 1)

## 1. pelican.scene v1(提案)

```json
{
  "schema": "pelican.scene",
  "version": 1,
  "scenes": {
    "default_scene": {
      "objects": [
        {
          "name": "hero_character",
          "components": [
            {"name": "transform", "pos": [0,-1.2,1], "rotation": [0,0,0,1], "scale": [0.25,0.25,0.25]},
            {"name": "simplemodelview", "model": "character"}
          ]
        },
        {
          "name": "BlueSpot",
          "components": [
            {"name": "light", "type": "spot", "position": [2,0.5,2], "direction": [0,-0.5,-1],
             "intensity": 1.8, "color": [0.6,0.7,1.0], "innerConeAngle": 18.0, "outerConeAngle": 28.0}
          ]
        }
      ]
    }
  }
}
```

現行からの差分は **4 つ**:

1. **エンベロープ**: `schema` / `version` 必須 + シーン群を `scenes` キーに包む。
   ゲートは project.json と同じ意味論(schema 不一致・version 超過は hard error)
2. **`objects[].name`(任意)**: シーン内一意・R7 文字集合。用途は
   transform_seq のオブジェクト束縛・devstudio での選択・エラーメッセージ。
   省略時は無名(現行どおり)。重複は hard error
3. **シーン分割**([PF] §8 未決 1 に回答): **1 ファイル複数シーンが標準形**
   (2026-07-02 レビュー: 複数シーンあるのが普通、との判断)。1 シーン 1 ファイルも
   同じエンベロープで表せるので分割は自由。複数ファイル参照(project.json の
   scenes マップ化)は [PF] v2 の課題として**やらない**(凍結を破らない)
4. **lights のコンポーネント化(2026-07-02 レビューで採用)**: 専用セクションを
   廃止し、`{"name": "light", ...}` コンポーネントを持つオブジェクトとして表す。
   ライトの表示名はオブジェクトの `name` を使う(現行 lights[].name と同じ役割)。
   params(type/position/direction/intensity/color/コーン角)は現行と同一。
   **バインダが吸収する**: SceneLoader は light コンポーネントを ECS ではなく
   LightContainer へ振り分ける(エンジン内部は当面無変更)。ECS 移管
   (ロードマップの「ライトアニメーション更新の ECS 側移管」)が入っても
   **形式はこのまま変わらない** — これが解釈レイヤ分離の直接の配当。
   position/direction を transform コンポーネントに統合するのは
   RenderWorld 期の version 改訂に持ち越す(v1 ではライト内にインラインで持つ)

### 互換規則

- **レガシー形式(schema キーなし・トップレベルがシーン id)は読み込み時に
  検出して WARN + 自動解釈**(`scenes` に包んだとみなす)。旧 `lights`
  セクションも同様に WARN + light コンポーネント列へ自動変換して解釈する。
  移行期間の措置であり、projects/example(WP18c)は v1 形式で書く。
  レガシー受理の削除時期は未決 4
- 現行ローダの意味論は不変: 未知コンポーネント名は fail-fast(黙って読み飛ばさない)、
  コンポーネント params の解釈は各コンポーネントの `loadByJson` が正

## 2. 規約(v1 で確定させるもの)

1. **コンポーネント名 = ComponentInfoManager の登録名**。新コンポーネントの追加は
   `userpublic/components` + 登録で完結し、シーン形式仕様の改訂を要しない
   (形式が定義するのは「name + 任意 params のレコード列」まで)
2. **ファイル参照をコンポーネント params に直接書くことを禁止**。
   モデル・テクスチャ等は必ず asset_data_json の id を経由する(間接参照)。
   理由: パス解決規則([PF] §3)の適用面を asset_data_json に限定し、
   シーンの検証をエンジン・web・ツールで安価にする
3. **light はコンポーネント**(§1-4)。ただし v1 の light コンポーネントは
   ComponentInfoManager を経由せず、バインダ(SceneLoader)が LightContainer へ
   直接振り分ける特別扱いとする(ECS に light コンポーネント型を今は作らない。
   作るのは ECS 移管のとき — そのときも JSON は不変)
4. 命名は R7(`[a-zA-Z0-9_]`)。scene id・object name・component name すべて
5. web(WW4 以降): サブセット原則どおり、web は理解するコンポーネント
   (transform / simplemodelview 相当)だけ束縛し、**未知コンポーネントは
   エラーではなく「未対応」として表示上スキップしてよい**(エンジンの fail-fast と
   非対称だが、これは「web は開けるものが少ない」方向でありサブセット原則に適合。
   ただし schema/version ゲートは同一意味論)

## 3. 実装 WP(候補: WP25。小)

1. 解釈レイヤ方針(`design_project_interpretation_layer.md` §2)に沿い、
   エンベロープ検証 + レガシー検出(scenes 包み・lights → light コンポーネント変換)を
   **純ロジック**として実装(GPU 不要テスト)
2. SceneLoader(バインダ): `scenes` 解決後のオブジェクト列を処理。
   light コンポーネントを含むオブジェクトは LightContainer へ、
   それ以外は従来どおり ComponentInfoManager/ECS へ振り分ける。
   `LightContainer::load` の入力を「light コンポーネント列」に合わせて調整
3. `objects[].name` の一意性検証 + ComponentInfoManager 未知名エラーに
   object name を含める(デバッグ性)
4. projects/example の scene JSON を v1 形式へ更新(WP18c と調整。
   lights もコンポーネント表現へ書き換え)
5. fixture: valid(v1 / レガシー両形式)+ invalid(schema 不一致・version 超過・
   name 重複)を `test/fixtures/project_format/` に追加(共有 fixture 運用)
6. golden テスト(WP16)で移行前後の描画一致を確認(ライト経路を触るため必須)

## 4. 未決事項

1. transform_seq と `objects[].name` の束縛規約(SeqPlayer の objects 配列との
   対応付け。WP17 の「全オブジェクト共有メッシュ」を超える v2 で確定)
2. prefab / ネスト(objects の階層化)— RenderWorld 期まで持ち込まない
3. シーン間参照・シーン遷移のランタイム API(形式ではなくエンジン API の話)
4. レガシー形式受理の削除時期(devstudio がシーンを書き出すようになった時点が候補)
