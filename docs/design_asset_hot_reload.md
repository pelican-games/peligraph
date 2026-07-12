# アセットホットリロード(v1)

対象読者: エンジン担当・開発体験を気にする人。
ステータス: v1 ドラフト(2026-07-12。codex 敵対レビュー前)。
前提: 確定規約 2 件(2026-07-08 ユーザー決定 — ①リプレイ/strict/rpc 駆動中は
ホットリロード無効 ②エディタの自己書き込みはハッシュ比較で無視)、
「ファイルが唯一の真実」(devstudio D3 の実行基盤を兼ねる)、
WP82(シェーダキャッシュ)、WP62(ECS ID 参照)、`design_project_vcs.md`
(mounted store)。

## 0. 原則

1. **置換は開発の日常** — ファイルを保存したら数百 ms 後に絵が変わるのが
   目標の体験。警告で止めない(構造の逸脱だけが WARNING、という
   検証方針と同じ精神)
2. **失敗で壊さない**: どの種別も「新リソースのロードに失敗したら
   旧リソース継続 + 名前入り WARN + `get_status.last_reload_error`」。
   コンパイルエラーの .surface を保存してもエンジンは落ちない
3. **決定性の聖域を侵さない**: リプレイ / strict / rpc 駆動中は
   **監視スレッド自体を止める**(イベントを溜めて後で流す、はしない —
   復帰時に一括再スキャン)
4. **適用はフレーム境界**: mid-frame 差し替えなし。E1/FrameInput と同じ
   位相規律(検知はいつでも、適用は次フレーム先頭で一括)

## 1. 監視基盤(1 個・共有)

- `FileWatcher` モジュール(新設): Win32 `ReadDirectoryChangesW` を一次、
  `std::filesystem` の mtime ポーリング(2s)を fallback
  (ネットワークドライブ・一部 mounted store 対策 — store ごとに自動判定)
- 監視対象 = project root 配下(assets / scenes / shaders / materials /
  ui / passes / imports)+ mounted store の実パス
- **デバウンス**: 最終書き込みから 200ms 静穏で 1 イベントに合流
  (エディタの分割書き込み・一時ファイル rename 対策)
- **内容ハッシュ比較**: mtime だけでは発火しない(touch・チェックアウトで
  嵐にならない)。WP82 のハッシュ機構を流用。**エディタ自己書き込みの
  抑制もここで自然に成立**(書いた内容のハッシュを事前登録 → 一致は無視)
- シェーダホットリロード(未実装の開発体験候補)は**この基盤に乗る**
  (別の監視を作らない)

## 2. リソース種別ごとの差し替え表(v1 の範囲)

| 種別 | 反映方法 | 難度 | v1 |
|------|---------|------|----|
| テクスチャ(独立画像 / KTX2) | GPU image を in-place 再アップロード(リソース ID 不変・参照無傷)。サイズ/format 変化時は image 再生成 + descriptor 再バインド | 低 | ✅ H1 |
| material values(.material.json) | SSBO 値の更新のみ(WP76 バインダ再実行) | 低 | ✅ H1 |
| .surface / シェーダ | variant 再コンパイル(WP82 キャッシュは source hash が変わるので自然に miss)+ pipeline 再生成。失敗 = 旧 pipeline 継続 | 中 | ✅ H2 |
| モデル(glb / フラグメント) | ModelTemplate 再ロード → 参照中の simplemodelview を ID で再バインド(WP62 の ID 参照が前提)。transform/物理は不変 | 中 | ✅ H2 |
| input_actions / バインディングプロファイル | 再パース + アクション再バインド | 低 | ✅ H2 |
| pelican.ui | **U3 の hot reload トランザクション**(side-build → atomic swap)に委譲 — 監視はここが発火源になるだけ | — | U3 と合流 |
| scene JSON | **自動リロード対象外**(ゲーム状態の全消しになるため)。rpc `load_scene` / ImGui ボタンの明示操作のみ | — | 対象外と明記 |
| rendering config / feature JSON | v1 対象外(frame graph 全再構築 = 実質再起動。将来 H4 候補) | 高 | 対象外と明記 |
| project.json / manifest | 対象外(再起動事項) | — | 対象外 |

## 3. 適用パイプライン

```
FileWatcher(検知・デバウンス・ハッシュ)
  → ReloadQueue(種別分類・同一リソースの合流)
  → フレーム先頭の適用フェーズ(FrameInput 位相の前)
     - 種別ハンドラが「新を構築 → 検証 → 差し替え」の順で実行
     - 構築/検証失敗 = 旧継続 + WARN + last_reload_error
  → 1 行 INFO ログ `reload: <種別> <パス> ok|failed (<時間>ms)`
```

- GPU in-flight 資源は既存の遅延破棄(DeletionQueue)に乗せる
- 同一フレームに複数種別が来た場合の適用順は固定
  (テクスチャ → values → シェーダ → モデル → 入力 — 依存の浅い順)

## 4. 観測点

- `get_status.reload`: `{watching: bool, applied: N, failed: N,
  last_error: {path, kind, message} | null}`
- ImGui にリロード履歴パネル(WP86 の流儀で後続 — v1 はログのみ)

## 5. テスト戦略

- FileWatcher はモック可能に(テストは実 FS イベントでなく直接
  ReloadQueue へ注入 — CI の FS イベントは不安定)
- 実 FS の end-to-end は 1 本だけ(テクスチャ書き換え → capture 差分)
- 失敗系: 壊れた .surface 保存 → 旧絵継続 + last_reload_error
- 決定性: rpc 駆動中に監視が止まっていること(ファイル書き換えても
  capture 不変)
- 自己書き込み抑制: ハッシュ事前登録 → 発火ゼロ

## 6. WP 分割

| 段階 | 内容 | 依存 |
|------|------|------|
| H1 | FileWatcher + ReloadQueue + テクスチャ + material values | なし |
| H2 | .surface/シェーダ + モデル + input_actions | H1 |
| (H3) | UI = U3 に合流 / (H4) rendering config = 将来 | — |

## 7. 未決事項

1. mounted store がネットワーク越しの場合のポーリング間隔(既定 2s で
   よいか — 大規模チーム運用の実測待ち)
2. エディタ自己書き込みハッシュの登録 API の置き場(rpc に置くか
   devstudio 直結か — D2 の編集系 rpc 設計と同時に決める)
3. KTX2(WP92)の in-place 再アップロードの mip 数変化の扱い
