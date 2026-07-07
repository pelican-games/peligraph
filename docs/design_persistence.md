# 永続化: ユーザー設定とセーブデータ(v1)

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-08。レビュー前)。
前提: [PF](PathResolver — **user:// 追加は [PF] v6.3 改訂が必要 = 凍結文書の
版数改訂としてユーザー承認を得てから実装**)、`design_input_actions.md`
(リバインド保存の受け皿)。

## 0. 三種類のデータの区別(設計の核)

| 種類 | 置き場所 | 書き込み |
|------|---------|---------|
| プロジェクトデータ | project:// | **読み取り専用**(現行どおり) |
| ユーザー設定 | user://settings.json | エンジン既知キー + ゲーム自由キー |
| セーブデータ | user://saves/<slot>.json | ゲーム主導の JSON |

`user://` = OS のユーザーディレクトリ(`%APPDATA%/pelican/<project_id>/`。
project_id は project.json の name — 未決 1)。プロジェクトディレクトリには
**何も書かない**(読み取り専有の原則を守る — 複数インスタンス並行起動の
安全性の根拠でもある)。

## 1. ユーザー設定(pelican.settings v1)

- エンベロープ schema/version + 2 区画:
  `engine`(既知キー: バス音量・画質プリセット・ウィンドウ)と
  `game`(自由 JSON — ゲームが定義)
- 入力リバインドは actions.json への**差分**として保存
  (`design_input_actions.md` の分離設計を実体化)
- 読み: 起動時に自動。書き: 明示 API(`GameContext::saveSettings()`)。
  自動保存はしない(壊れた設定を書き続ける事故防止)

## 2. セーブデータ(v1 = ゲーム主導)

```cpp
void saveData(std::string_view slot, const Json &data);
std::optional<Json> loadData(std::string_view slot) const;
std::vector<SlotInfo> listSaves() const; // slot 名 + タイムスタンプ
```

- v1 は **ゲームが自分で JSON を組む**(何を保存するかはゲームが知っている)。
  コンポーネント自動シリアライズ(ワールドスナップショット)は v2 —
  serialize `ref` の逆方向で、シーン形式との整合を要設計
- 書き込みは atomic(temp + rename)。破損時は読み失敗を返す(クラッシュしない)

## 3. web プロファイル

user:// は web では localStorage / OPFS に写像可能(サブセット原則: 形式は
同じ JSON)。実装は web 側に需要が出てから。

## 4. 実装順(WP 候補)

| 段階 | 内容 | 前提 |
|------|------|------|
| P1 | [PF] v6.3(user://)+ settings 読み書き + saveData/loadData + atomic 書き込み + テスト(temp dir 差し替え) | **[PF] 改訂承認** |
| P2 | リバインド差分保存(入力 I 系と同時)+ 音量連携(オーディオ A2 と同時) | P1 |
| v2 | ワールドスナップショット(コンポーネント自動シリアライズ) | 需要 |

## 5. 未決事項

1. project_id の一意性(name 衝突)。推奨: name + 明示 id キー(任意)の併用
2. テストでの user:// の扱い: 環境変数 or 起動引数で root を差し替え可能に
   (rpc 複数インスタンスのテスト分離にも使う)
3. セーブの互換性(ゲーム更新後の旧セーブ)はゲーム側の責務と明記するか
