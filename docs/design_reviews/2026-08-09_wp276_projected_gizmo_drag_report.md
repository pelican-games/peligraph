# WP276: projected gizmo drag and persistence verification

## 結果

- `query_gizmo_handle` contract 2 は、公開 `handle` に `drag_direction` と
  `value_per_logical_pixel` を返す。移動と拡縮は engine が射影した軸、回転は掴んだ
  ring segment の正回転接線を使う。
- Studio は論理 pixel 差分に対する
  `dot(delta, drag_direction) * value_per_logical_pixel` の一式だけを使う。camera / projection
  行列と mode 固有の pixel 係数は持たない。
- 視線方向へ潰れた移動・拡縮軸は表示用 marker を残すが drag 契約を持たず、RPC は
  `handle: null` を返す。NaN、無限大、巨大な逆数を client へ出さない。
- 問い合わせの成功と失敗は同じ generation と request ID の predicate を通る。二度目の
  押下で置き換えられた一度目の失敗は通知を変更しない。
- 保存回帰は GizmoModel が作った値を実際の editor preview lease で commit し、
  `ProjectBasicConfig::saveSceneDocument()` でファイルを置換した後、別の module generation
  から scene を decode し直して authoring `pos` を比較する。RPC の送信や旧 in-memory
  document は合否に使わない。

## 欠陥 3 の修正前 red gate

修正前の WP275 commit `ee1be90070242a9187a8cef03589a6eca8d5a10e` を detached clone にし、
既存の `Devstudio save scene persists committed authoring edits only while idle` へだけ次の
観測を追加して実行した。

1. disk 上の scene fixture を authoring `pos: [1, 2, 3]` で作る。
2. 既存 harness で gizmo-style preview を `[4, 2, 3]` へ commit し、従来どおり mock の
   `save_scene` 成功応答を返す。
3. disk の scene fixture を読み直し、authoring `pos == [4, 2, 3]` を要求する。

実行コマンドは次である。

```powershell
ctest --test-dir build -C Debug --output-on-failure -R "Devstudio save scene persists"
```

結果は 49 assertions 成功後、reload 比較の `[1,2,3] == [4.0,2.0,3.0]` で失敗した。
従来テストが観測していた mock RPC と busy 遷移では disk が変わらないことを、WP276 の
実装を含めずに確認した。検証後は clone と生成物を削除した。

## 受け入れ検証

- configure cache は `PELICAN_WITH_SPIRV_LINK=ON`、`SKIP_DEVSTUDIO=OFF`。
- `cmake --build build --config Debug --parallel`: 成功。
- `ctest --test-dir build -C Debug --output-on-failure`: **1146 / 1146 成功**、失敗 0。
  `gpu` label は 131 件。既存の symlink capability test 1 件は skip。
- `pelican_player.exe --project projects/animgraph_demo --xr off`: hidden windowed で 8 秒生存。
  控えた PID 33772 だけを停止し、stderr 0 byte、fatal / Validation Error / VUID marker 0、
  検証 log は削除済み。
- `uv run tools/doclink.py check`: 2310 ok、stale / unresolved 0。
- `uv run tools/doclink.py audit`: misaimed / absent identifier 0。
- `git diff --check`: 成功。
- `src/devstudio/` の `lookAt|perspective|VPMatrix|glm::mat4`: 0 件。
- 削除対象 3 identifier の word-boundary grep: 0 件。
