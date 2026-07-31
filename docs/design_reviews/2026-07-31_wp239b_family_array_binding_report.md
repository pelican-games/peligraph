# WP239b: family-array material input binding regression

## 判定

`hybrid_v1` の実装が束縛していた image view が正しく、GPU テストの期待値が
producer view-family array 導入前の scalar 契約のまま残っていた。

directional shadow input は `family_array` であり、生成 shader の descriptor ABI は
`sampler2DArray` である。そのため material descriptor は全 layer を含む
`e2DArray` view を束縛する。`RenderTargetContainer::getImageView()` は array target
でも layer 0 の `e2D` view を返す scalar compatibility accessor なので、この
期待値には使えない。

## 修正

- `getImageView()` と `getLayeredImageView()` の array target に対する契約を公開
  header に明記した。
- 1 layer の family array も、同じ範囲の subresource view を重複生成せず、常設の
  canonical layered view を使うよう統一した。descriptor shape は `e2DArray` のまま
  変わらない。
- `hybrid_v1` の GPU テストを、初回束縛と resize 後の再束縛の双方で
  `getLayeredImageView(shadow_map)` と比較するよう変更した。
- `family_array` ABI 検査は変更していない。

これにより、単なる実測 handle への期待値変更ではなく、shader ABI が要求する view
shape をテストしている。
