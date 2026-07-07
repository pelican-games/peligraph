# 決定性サービス: 乱数(v1)

対象読者: エンジン担当。
ステータス: v1 ドラフト(2026-07-08。小粒 — 設計というより規約の明文化)。
前提: `design_input_actions.md` §4(リプレイの三点セット)。

## 0. 位置づけ

リプレイ・シナリオテストの前提は **fixed step + input_seq + シード**。
前 2 つは実装済みだが、シードは空手形(ゲームが std::rand を呼んだら終わり)。
エンジン管理の決定的乱数を GameContext に置き、規約で締める。

## 1. 仕様

- 実装: **PCG32**(自前 ~40 行。プラットフォーム・コンパイラ非依存の
  決定性が保証される — std::mt19937 は distribution が処理系依存なので
  distribution も自前(整数レンジ・[0,1) float))
- シード: project.json `basic_config.seed`(省略時 0 — **既定は固定**。
  「毎回違う」が欲しいゲームは起動時に自分で `setSeed(エントロピー)`)
- API(GameContext):

```cpp
double random();                       // [0, 1)
int randomInt(int min, int max);       // [min, max] 両端含む
float randomFloat(float min, float max);
void setSeed(std::uint64_t seed);
std::uint64_t seed() const;            // 現在のシード(get_status にも出す)
```

- rpc: `get_status` に seed を追加。`set_seed {seed}` を追加(テスト用)
- ストリーム分離(gameplay 用と vfx 用を分けて、演出が消費しても
  ゲームプレイ列が乱れない)は v2 — 需要が出てから

## 2. 規約(明文化)

- エンジン内部で wall clock・std::rand・random_device を**判定に使わない**
  (現状守られている — adding_features.md の新サブシステム要件に追記)
- ゲームコードには「リプレイ互換にしたければ ctx の乱数だけを使う」と
  cookbook に記載(強制はしない — ゲームの自由)

## 3. テスト

同一シード → 同一列(固定期待値 fixture — プラットフォーム間で同一)。
setSeed 後の再現。get_status/set_seed の rpc 結合。
