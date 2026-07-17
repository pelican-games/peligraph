# オブジェクトへの振る舞い紐付け(behavior)(v1)

対象読者: エンジン担当・ゲームロジックを書く人。
ステータス: v1 ドラフト(2026-07-17。敵対レビュー前)。
前提: `design_game_logic_native.md`(G1a =
PELICAN_REGISTER_SYSTEM + GameContext・G1b = PELICAN_PROJECT・
G2 = DLL hot reload + owner generation)、scene v1(component の
`name` dispatch)、WP62(typed component 登録・optional serializer)、
決定性規範、`design_editor_tooling.md`(インスペクタの反射)。

## 0. 問題 — 「このオブジェクトにこの振る舞い」の語彙がない

現状のゲームロジックは **グローバルなシステム**(登録順に毎フレーム
実行され、コンポーネントを走査する)のみ。Unity の
「GameObject に MonoBehaviour を付ける」に当たる、**シーンデータから
オブジェクト単位に振る舞いを宣言する**方法がない。example の
playercontrol も「名前でオブジェクトを探すシステム」で書かれている。

## 1. 設計 — behavior = 登録クラス + component 参照

### 1-1. C++ 側(ゲーム DLL・ユーザー空間)

```cpp
class PlayerControl final : public Pelican::Behavior {
  public:
    // params は scene JSON から型付きで注入される(§1-3)
    struct Params {
        float speed = 4.0f;
        float jump_height = 1.2f;
    };

    void onInit(Pelican::BehaviorContext &ctx) override;
    void onUpdate(Pelican::BehaviorContext &ctx) override;   // 毎フレーム
    void onEvent(Pelican::BehaviorContext &ctx,
                 const Pelican::EventView &event) override;   // E1 購読
    void onDestroy(Pelican::BehaviorContext &ctx) override;
};
PELICAN_REGISTER_BEHAVIOR(PlayerControl);  // 型名 = 登録名
```

- `Behavior` は**特権なしの公開基底**。`BehaviorContext` は
  GameContext と同じ公開面 + **自分の entity**(transform・
  components・イベント emit)への短縮アクセス
- 既存の PELICAN_REGISTER_SYSTEM は**そのまま残る**(横断ロジック用)。
  behavior は「1 オブジェクトに閉じるロジック」の糖衣 — 実体は
  エンジンが持つ **1 個の BehaviorSystem** が全 behavior instance を
  決定的順序で回す(ユーザーから見えるのは基底クラスだけ)

### 1-2. シーン側(scene v1 の additive component)

```jsonc
{ "name": "behavior",
  "type": "PlayerControl",
  "params": { "speed": 6.0 } }
```

- `type` = 登録名。未登録は**名前入りエラー**(DLL 未ロード時は
  ロード後に解決 — 起動順の規範を §2 で)
- 1 オブジェクトに複数 behavior 可(配列順 = 実行順の tie-break)
- params は省略可(C++ 側 default)

### 1-3. params の反射(インスペクタと同じ土台)

- `Params` struct は **JSON serialize/deserialize を必須**
  (WP62 の typed 登録と同じ流儀 — マクロで固定 field を宣言)。
  これによりインスペクタ(design_editor_tooling.md §1-3)で
  behavior の params が**そのまま編集可能**になる
- 型は scene v1 が許す値のみ(数値・bool・文字列・vecN)。
  参照(他オブジェクト)は **名前文字列**で持ち、解決は
  onInit(WP62 の use-time resolve の流儀 — 生ポインタ保持禁止)

### 1-4. 実行順と決定性

- 実行順 = **(system order 上の BehaviorSystem 位置)→ entity の
  安定順(scene 宣言順 = EntityId 発行順)→ 同一 entity 内は
  components 配列順**。全 tie が決定的
- onUpdate 内の乱数は既存 PCG32(get_status.seed)・時間は
  EngineTime — 既存の決定性規範がそのまま適用
- リプレイ(WP89)は入力再生で自然に一致(behavior は入力の消費者)

## 2. ライフサイクルと G2 reload

- **生成**: シーンロードで component 構築 → 対応する DLL の登録が
  揃った時点で instance 化 + onInit(DLL より先にシーンが来た場合は
  pending — 名前入り WARN で観測可能)
- **破棄**: destroy_object / シーン遷移で onDestroy → instance 破棄
- **DLL hot reload(G2)**: owner generation で旧 instance を
  onDestroy → 新 DLL の登録で **params から再 init**(WP90 の
  全リセット方式 v1 と同じ割り切り — behavior の内部状態は消える。
  状態を残したい値は params/component 側へ置くのが作法。
  状態 serialize による引き継ぎは v2 予約)
- エンジン(BehaviorSystem)は instance を owner 付きで保持し、
  unload 時に必ず解放(WP90/102 の owner 契約と同型)

## 3. インスペクタ統合

- behavior component は「type 選択(登録済み一覧の combo)+
  params 編集」の専用 UI(反射は §1-3 で自動)
- **add_component("behavior", type=...) で実行時アタッチ**も
  E-RPC 経由で可能(次フレーム onInit)

## 4. WP 分割

| WP | 内容 | gate | 依存 |
|----|------|------|------|
| **BEH0** | Behavior 基底 + PELICAN_REGISTER_BEHAVIOR + BehaviorSystem + scene component + params 反射 | 決定的実行順 fixture・未登録エラー・複数 behavior・リプレイ 2 回一致 | G1a/WP62(済) |
| **BEH1** | G2 reload 統合(owner・pending 解決・params 再 init) | reload 前後の fixture(WP90 流儀)・pending WARN | BEH0・WP90 |
| **BEH2** | E-RPC/インスペクタ統合(type combo・params 編集・実行時アタッチ) | E-RPC fixture・インスペクタで speed を変えて挙動が変わる実測 | BEH0・E-RPC1 |
| 後続 | example の playercontrol を behavior に移行(dogfooding)・状態引き継ぎ reload(v2) | 挙動 byte 一致 | BEH0-2 |

## 5. 未決事項

1. onFixedUpdate(物理レート)の要否 — 現行は単一レートなので保留
   (fixed timestep 導入時に additive)
2. behavior 間の直接参照(getBehavior<T>)— v1 は不許可
   (イベント E1 経由が作法)。需要を見て v2
3. 優先度/実行順の明示指定 — v1 は宣言順のみ。要求が出たら
   system order と同じ整数 order を additive
