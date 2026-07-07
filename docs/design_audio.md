# オーディオ(v1)

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-08。レビュー前)。
前提: `design_asset_format_policy.md`(WAV+Ogg 決定済み)、
`design_build_tiers.md`(新サブシステムは誕生時からユニット化)。

## 0. 方針

- **`PELICAN_WITH_AUDIO` ユニットとして誕生**(build tiers 要件の初適用例)。
  OFF = 明確エラー、dist-config が音声アセット有無から導出
- バックエンドは **miniaudio** を推奨(単一ヘッダ・MIT-0/public domain・
  WASAPI 等の OS 差を吸収・WAV デコーダ内蔵・Ogg は stb_vorbis 連携)。
  FetchContent 追加 1 件で済む
- **headless / rpc 駆動時は null バックエンド**(デバイス不要・無音で全 API が
  成功する)。CI と決定性のため必須 — 音は golden 化しない、テストは
  「再生状態の観測」(playing / stopped / 再生位置)で行う

## 1. 三層構造

| 層 | 内容 |
|----|------|
| バックエンド | miniaudio デバイス + null。ミキシングは miniaudio のエンジン機能を使う |
| バス | master / bgm / se の 3 バス固定(v1)。バス毎音量。設定永続化(`design_persistence.md`)と連携予約 |
| API | GameContext 経由(下記) |

## 2. API(v1)

```cpp
SoundHandle playSound(std::string_view path);        // SE: 発火して忘れる(se バス)
SoundHandle playMusic(std::string_view path, bool loop); // BGM: ストリーム再生(bgm バス)
void stopSound(SoundHandle);
void setBusVolume(std::string_view bus, float volume); // "master"/"bgm"/"se"
bool isPlaying(SoundHandle) const;
```

- path は project:// / engine://(PathResolver 経由 — 絶対パス拒否も同じ)
- 形式: WAV = SE(全読み)、Ogg = BGM(ストリーミング)。
  他形式はソース層(変換して imports/ へ)— asset policy 準拠
- 3D 音響 v1 = **距離減衰 + パンのみ**(listener = アクティブカメラ)。
  HRTF・オクルージョンはスコープ外

## 3. データ駆動(v2 予約)

- scene v1 に `audio_source` コンポーネント(ループ環境音の配置)
- pelican.audio_banks(SE 名 → ファイルの間接層。ゲームコードにパスを
  直書きしないための辞書)— 需要が出たら

## 4. 実装順(WP 候補)

| 段階 | 内容 |
|------|------|
| A1 | ユニット + miniaudio + null バックエンド + WAV SE 再生 + バス音量 + GameContext API + OFF スモーク |
| A2 | Ogg ストリーミング(BGM)+ フェードイン/アウト + 設定永続化連携 |
| A3 | 距離減衰/パン + audio_source コンポーネント |

## 5. 未決事項

1. Ogg デコーダ: miniaudio 同梱の stb_vorbis で足りるか(A2 で判断)
2. web プロファイル: WebAudio 写像は形式(WAV/Ogg)がそのまま通るので
   サブセット原則は保てる — WW 化は web 側に音の需要が出てから
3. SoundHandle の世代管理(再利用事故防止)— handle.hpp の既存流儀に従う
