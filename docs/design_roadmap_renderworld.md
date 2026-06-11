# レンダラー開発ロードマップ & RenderWorld 境界設計

対象読者: ECS 担当者(境界 API の合意用)+ レンダラー担当(全体順序の基準)

ステータス: ドラフト(レビュー待ち)

## 1. 目的

- レンダラー側の今後の開発順序を共有し、ECS 側との接点を明確にする
- ECS とレンダラーの境界を「RenderWorld」として形式化し、互いの内部変更が相手に波及しない構造を合意する

## 2. 開発順序(全体)

レンダラー本線(上から順に実施):

| # | 項目 | 概要 | ECS への影響 |
|---|------|------|--------------|
| 1 | ヘッドレス描画 + PNG 出力 | スワップチェーン非依存の描画経路と読み戻し。swapchain 再生成(リサイズ/OutOfDate 対応)も同梱 | なし |
| 2 | 遅延破棄機構 | GPU 使用中リソースの安全な破棄(deletion queue)。固定容量バッファの growable 化の前提 | なし |
| 3 | シェーダ自由化キット | ランタイム GLSL→SPIR-V、SPIR-V リフレクション、汎用パイプラインファクトリ。詳細は `design_shader_freedom_kit.md` | なし |
| 4 | ゴールデンイメージテスト + ホットリロード | 3 の副産物。ctest にシェーダ描画テストを追加 | なし |
| 5 | compute パス | パス JSON に `"type": "compute"` 追加、storage image/buffer、バリア自動化 | なし |
| 6 | GPU 計測 + debug_utils | パス単位の GPU タイムスタンプ、`--profile` で JSON 出力、RenderDoc 用の名前付け | なし |
| 7 | bindless / BDA | descriptor indexing と buffer device address。RT の前提 | なし |
| 8 | RT パイプライン | BLAS/TLAS → SBT → RT シャドウ/AO | メッシュデータ参照(§4.4 で吸収) |

並行トラック(本線と独立に進行):

| # | 項目 | 概要 | ECS への影響 |
|---|------|------|--------------|
| A | RenderWorld 形式化 | 本文書 §3〜§5。**合意が必要なのはここだけ** | 境界 API の合意 |
| B | コマンド層 (JSON-RPC) | エンジンをテキストコマンドで操作可能に(エージェント/外部ツール対応) | なし |
| C | GUI 再配線 → DCC 連携 | devstudio をコマンド層クライアント化、Blender 等からのプレビュー | なし |

意図的に後回しにするもの: LOD、フラスタム/オクルージョンカリング、ソート/バッチング、アニメーション、ストリーミング、レンダースレッド分離。いずれも RenderWorld の draw command 生成 seam(§5.2)に後挿しできる設計にしておく。

### 2.1 外部ツール群(別プロジェクト)との連携要件

Blender 向け動画作成ツール群(物理シミュレーション、ポーズ推定等)が別プロジェクトとして進行する。エンジン側で受けるための追加要件(詳細はツール側要求書 `docs/external_tools_requirements.md`):

- **仮想時刻の注入**: 現在 `Renderer::render()` が壁時計(`FrameClock::now()`)依存。時刻源を差し替え可能にし、コマンド層に `set_time` / `step_frame` を設ける。シミュ再生・動画化・テスト決定性の前提
- **連番画像出力**: ヘッドレスの `--render-out` を `out/%04d.png` 形式の連番に対応(`design_headless_rendering.md` §6 参照)
- **コマンド層(B)に追加する命令**: `load_gltf` / `set_time` / `step_frame` / `render_frame` / `capture`。これが揃うとツール→エンジンのプレビューループが成立する
- **剛体シミュ結果の再生**: ベイク済み transform 列は `RenderWorld::updateTransforms(span)` でそのまま受けられる(追加設計不要)
- **スケルタルアニメ/スキニング**: ポーズ推定の活用に将来必要。新規領域のため別途設計(ロードマップ未組込)

## 3. 現状の境界

事実上の境界はすでに存在している:

- ECS 側は `SimpleModelViewComponent` が `ModelInstanceId` を保持し、`PolygonInstanceContainer` の API を呼ぶ
  - `placeModelInstance(ModelTemplate&) -> ModelInstanceId`
  - `setTrs(ModelInstanceId, pos, rotation, scale)`
  - `removeModelInstance(ModelInstanceId)`
- レンダラー側は `PolygonInstanceContainer` から indirect draw バッファとモデル行列バッファを読んで描画する

問題は境界が**暗黙**であること:

1. 境界がどの API か明文化されておらず、リンク/include 制約もないため、どちらかが相手の内部に依存するコードを書ける状態
2. レンダラーのテストに ECS 一式が必要になりうる
3. カメラ・ライトは別経路(`Camera`、`LightContainer`)で境界の形がバラバラ

## 4. 提案: RenderWorld 境界

### 4.1 原則

1. **一方通行**: データは ECS → RenderWorld → レンダラー の方向にのみ流れる。レンダラーから ECS のヘッダを include しない(CMake のリンク依存でも強制する)
2. **plain data**: 境界を流れるのは ID と数値データのみ。ECS の型(Entity、Component)は境界を越えない
3. **唯一の入口**: シーン内容の描画への反映は RenderWorld の API 経由のみ

### 4.2 API スケッチ

実装は既存 `PolygonInstanceContainer` / `LightContainer` / `Camera` への委譲から始め、入口を 1 つの facade に集約する。ただし API の形は以下の点で現行から変更する(理由は各コメント):

```cpp
// core/renderworld/renderworld.hpp
namespace Pelican {

// 世代付きハンドル: 削除済みインスタンスの ID 使い回し(stale handle)を検出可能にする。
// ID が ECS 側コードに増殖する前の今が変更の最終機会
struct RenderInstanceId {
    uint32_t index;
    uint32_t generation;
};

struct ModelInstanceTransform {
    RenderInstanceId id;
    glm::vec3 pos;
    glm::quat rotation;
    glm::vec3 scale;
};

// カメラは「モジュールを突く」のではなくフレームデータとして渡す。
// 複数ビュー(シャドウ・エディタ・DCC プレビュー)を最初から前提にする
struct ViewDesc {
    glm::mat4 view;
    glm::mat4 proj;
};

DECLARE_MODULE(RenderWorld) {
  public:
    // MeshId は opaque なアセットハンドル(ModelTemplate& の参照渡しをやめる。
    // ストリーミング/非同期ロード/BLAS 管理の前提)。当面はモデル登録時に発行
    RenderInstanceId placeModelInstance(MeshId mesh);
    void removeModelInstance(RenderInstanceId id);          // stale id は throw

    // 個別 setTrs ではなく一括更新。ECS 側は「変更分を集めて 1 回渡す」。
    // 内部実装を SSBO ステージング直書き(GPU-driven)に差し替えても境界が変わらない
    void updateTransforms(std::span<const ModelInstanceTransform> transforms);

    // 毎フレーム設定。views[0] がメインビュー
    void setViews(std::span<const ViewDesc> views);

    // ライト(現行 LightContainer の登録 API をここに寄せる)
    LightId placeLight(const LightDesc &desc);
    void removeLight(LightId id);
    void setLight(LightId id, const LightDesc &desc);

    // フレーム境界: ここまでの書き込みでシーン状態が一貫していることを宣言する。
    // 当面は中身なし。将来のレンダースレッド分離のスナップショット点、
    // TLAS 再構築点、ホットリロード安全点がすべてここに揃う
    void commit();
};

} // namespace Pelican
```

移行について:

- ECS 側の呼び出しコードの変更は「呼び先の差し替え + setTrs の収集化」程度
- レンダラーのテスト/シェーダサンドボックスは ECS を介さず RenderWorld に直接書き込む

### 4.3 責務の移動(現状の層違反の解消)

1. **ライトアニメーション**: 現在 `Renderer::render()`(vkcore/renderer.cpp)が `light_container.updateAnimation(time)` を呼んでおり、シーンロジックが描画側にある。ECS/ゲームロジック側の update に移し、レンダラーは当該フレームのライトデータを受け取るだけにする
2. **エンティティ削除時のインスタンス解放**: entity 破棄 → `removeModelInstance` を誰がいつ呼ぶかを明文化する。理想は ECS 側のコンポーネント破棄処理で保証
3. **`predefined` コンポーネントの所有権**: `modelview.hpp` 等はレンダラー境界に触るため、変更 PR はレンダラー担当が出し、ECS 担当がレビューする分担とする

### 4.4 運用規約

1. **リンク方向の強制**: `src/core/renderer` / `src/core/renderworld` から `src/core/ecs` への include を禁止。レビュー頼みにせず CMake ターゲット分割で機械的に強制する
2. **テストの相互独立**: レンダラーのテストは ECS 非依存(RenderWorld 直書き)、ECS のテストはレンダラー非リンク
3. **更新順序の明文化**: 現行の `ecs.update() → renderer.render()`(単一スレッド、appflow/loop.cpp)を前提として文書化し、変更時は両者合意とする

### 4.5 将来の拡張(合意不要だが予告)

- **draw command 生成 seam**: RenderWorld のインスタンスリスト → indirect draw 列の生成を独立ステップにする。LOD・カリング・ソートはここに後挿しされる(ECS 側は無関係)
- **BLAS/TLAS(RT 用)**: BLAS はメッシュ登録時(MeshId 単位)、TLAS は RenderWorld のインスタンス情報から構築する。**ECS への追加要求は発生しない**
- **レンダースレッド分離**: 行う場合は `commit()` がスナップショット点になる。push 型 API は維持される

## 5. ECS 担当への確認事項

API の形(§4.2):

1. `RenderInstanceId` の世代付き化に伴い `SimpleModelViewComponent` の保持型が変わることに異議がないか
2. Transform 更新を一括 API に変えること、および**変更検出(dirty 集約)を ECS 側で行うか全件渡すか**の方針
3. カメラを `setViews`(行列のフレームデータ)にすることに異議がないか。ECS 側カメラコンポーネントが view/proj を計算して渡す形になる
4. ライト登録を RenderWorld 経由に寄せることに問題がないか
5. `placeModelInstance` の引数を `ModelTemplate&` から `MeshId` に変えることに異議がないか
6. `commit()` を ECS update の最後(または Loop 内の固定位置)で呼ぶ規約に合意できるか

責務(§4.3):

7. ライトアニメーション更新を ECS/ゲームロジック側へ移管できるか(受け入れ先の想定)
8. エンティティ削除時の `removeModelInstance` 呼び出し保証を ECS 側で持てるか
9. `predefined` コンポーネントの PR 分担(レンダラー側が出す)でよいか

運用(§4.4):

10. include/リンク方向の制約と CMake ターゲット分割に合意できるか
11. 更新順序(`ecs.update() → renderer.render()`)の固定化に合意できるか

## 6. 肥大化対策の方針(機構ではなく規律)

専用のパージ機構やプラグインシステムは作らない。以下の 3 点で代替する:

1. **core / feature の 2 層ルール**: feature は core に依存してよいが、core は feature を知らない(core 側から feature モジュールを `GET_MODULE` したら違反)。CMake ターゲット分割でリンク方向を固定し、レビュー基準とする。`FastModuleContainer` は遅延初期化のため、参照されないモジュールの実行時コストは既にゼロ
2. **機能はなるべくアセットにする**: パスシステムは JSON + シェーダのデータ駆動であり、シェーダ自由化キット導入後は bloom/ssao 等のエフェクト類は完全にアセット化される(パージ = ファイル削除)。新機能追加時は「これは C++ の機能か、アセット(パス定義 + シェーダ)か」を必ず問い、アセットにできるものを C++ に書かない
3. **重い外部依存のみ CMake オプション化**: RT、DXC/Slang バックエンド、サウンド、物理など「外部依存がある・ビルド時間に効く」ものだけ `PELICAN_ENABLE_*` フラグで丸ごと無効化可能にする(既存の `SKIP_DEVSTUDIO` と同じ流儀)。軽い機能のフラグ化は組み合わせ爆発を招くため行わない

動的プラグイン(DLL ロード等)は、第三者拡張の需要かビルド時間の破綻が実際に起きるまで作らない。

## 7. 参考

- Phase 1 リファクタの経緯と既知の follow-up: `docs/rendering_phase1_review.md`
- シェーダ自由化キットの詳細: `docs/design_shader_freedom_kit.md`
