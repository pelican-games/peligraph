# WP239c: planar reflection material image-view ABI regression

## 判定

planar reflection の material resource 宣言と shader ABI は正しく、
`sequential_2d` に lower された反射 capture target の descriptor 解決が不足していた。

同じ transparent material pipeline は、main view family と反射用 secondary view
family の両方で使われる。resource port は `family_array`、shader は
`sampler2DArray` を要求する。一方、反射 capture は各 view を順次実行するため物理
layout が `sequential_2d` になるが、保存先 image 自体は全 view layer を持つ
array-backed target である。従来コードは scalar `shared_2d` だけを array descriptor
へ適応し、`sequential_2d` を `sampler2D` と判定して ABI mismatch を報告していた。

## 修正

- sampled material resource が `family_array` を宣言した場合に限り、
  `shared_2d` と `sequential_2d` の array-backed storage を descriptor 境界で
  `family_2d_array` に正規化する。
- descriptor は target 全 layer の canonical `e2DArray` image view を一つ束縛し、
  pass ごとの重複 descriptor variant を作らない。
- input attachment、通常の `shared_2d` / `per_view`、cube、layered multiview の
  検証規則は緩めない。
- image-view ABI mismatch には shader、解決後 descriptor、宣言 port の各 dimension
  を表示し、次の不一致を診断可能にする。

物理 scheduler の `sequential_2d` layout は変更していない。この適応は、複数の
compatible pass variant が一つの material shader ABI を共有する境界だけに限定した。

## 回帰確認

既存の GPU golden case
`planar reflection executes a clipped secondary view family on the GPU` が、描画と
readback を含む 81 assertions で通ることを確認した。
