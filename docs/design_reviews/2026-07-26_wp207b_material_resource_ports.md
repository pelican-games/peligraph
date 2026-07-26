# WP207b 設計・実装報告: material/geometry typed resource ports

Status: implemented and verified (2026-07-26)

## 解いた問題

WP207a で fullscreen / compute は frame-graph image を logical name と generated
accessor で扱えるようになった。一方 material shader が通常経路で読めた pass resource は、
屈折向けの builtin `screen_inputs` に限られていた。このため compute simulation の buffer を
vertex displacement へ渡すには、surface と pass の双方へ raw set/binding を埋め込む必要が
あり、producer edge、hot reload 世代、shader stage の対応も利用者が手で維持する状態だった。

WP207b は `.surface` が宣言する shader-facing interface と、material pass が行う
frame-graph resource mapping を分離する。既存の `screen_inputs` ABI は変更せず、
current `pelican.surface v1` へ additive に追加した。旧版を読む互換分岐や新しい版は
導入していない。

## 公開 authoring 契約

surface は semantic port、descriptor kind、buffer element、利用 stage だけを宣言する。

```glsl
//! pelican.surface v1
//! language: glsl
//! resource_ports:
//!   - { name: displacement, kind: buffer, element: vec4, stage: vertex }
//!   - { name: simulation_color, kind: image, stage: fragment }

void pelican_vertex_displace_v1(inout PelicanVertexV1 vertex) {
    vertex.position += pelican_load_displacement(0u).xyz;
}

void pelican_surface_v1(
    in PelicanSurfaceInputV1 input_data,
    inout PelicanSurfaceV1 surface) {
    surface.base_color =
        pelican_sample_simulation_color(input_data.uv);
}
```

buffer element は `float` / `vec2` / `vec3` / `vec4`、対応する
`int` / `ivec*`、`uint` / `uvec*`、`mat4` を受理する。buffer は readonly std430
array として生成され、`pelican_load_<port>(index)` と
`pelican_count_<port>()` を公開する。image は sampled 2D image として
`pelican_sample_<port>(uv)` と `pelican_size_<port>()` を公開する。

`stage` は `vertex` / `fragment` / `vertex_fragment`。省略時は fragment である。
生成 descriptor は宣言 stage だけへ露出し、reflection は実 SPIR-V の stage visibility
まで照合する。binding 番号、descriptor block、実変数名は generated interface の内部詳細で
あり、surface は記述しない。

material pass は port 名を、既に宣言された graph image/buffer へ割り当てる。

```json
{
  "name": "material_geometry",
  "type": "material",
  "material_contract": "forward_opaque_v1",
  "material_resources": {
    "displacement": {
      "resource": "simulation_positions",
      "access": "storage",
      "footprint": "arbitrary"
    },
    "simulation_color": {
      "resource": "simulation_color",
      "access": "sampled",
      "view": "shared_2d",
      "sampling": {
        "filter": "nearest",
        "address": "clamp_to_edge"
      },
      "footprint": "arbitrary"
    }
  },
  "output": {
    "color": "lit_color",
    "depth": "scene_depth"
  }
}
```

pass が所有するのは logical resource、current/history、read footprint、view、
sampling である。surface は再利用可能な shader interface だけを所有する。
`material_resources` は graph resource を新設せず、通常の read edge と access intent に
lowering される。未知 producer、buffer の `@history`、同一 image の current/history
混在、buffer の sampled access、image の storage access は名前付きで拒否する。

## 実行・世代契約

- planner は material resource read を既存の fullscreen/compute と同じ graph へ入れ、
  compute write → material read の edge と read-after-write barrier を導出する。
- material buffer は logical name を描画時に再検索せず、runtime compile 時に
  generation-owned `FrameGraphBufferId` へ固定する。旧 frame、candidate、rollback の
  同名 buffer が交差しない。
- descriptor は surface reflection と pass mapping の両方を満たす material/pass の組だけに
  作る。route/filter で消費しない material は bind しない。
- render-target recreate では image view を再解決する。pipeline reload では新しい
  target/buffer ID を含む別 cache key へ移り、失敗した candidate は active generation と
  descriptor revision を変更しない。
- typed port を使わない material は従来の screen-input descriptor pool を使う。
  storage-capable pool と追加 sampler は最初の typed binding まで生成しないため、
  feature-off 構成に常設 GPU object を増やさない。

buffer consumer の同期先は vertex/fragment の双方を安全側に含める。material pass は複数の
surface を受け入れられ、同じ semantic port が material ごとに異なる stage で使われ得るため、
pass 単位で一方へ狭めない。pipeline reflection 自体は各 surface の正確な stage を維持する。

## 既存契約との関係

- `screen_inputs` と `pelican_screen_<name>()` は変更していない。snapshot/refraction の
  binding 順と byte golden も従来どおりである。
- material custom texture は set 2、material resource は set 1 であり、用途を混ぜない。
- raw set 1 ABI は低レベル実験用 escape hatch として残る。
- `material_resources` を持つ pass でも、その port を宣言しない material shaderへ
  descriptor を押し付けない。反対に、surface の必須 port を pass が供給しない場合は
  material/pass 名付きで失敗する。

## 意図的な制限

- material buffer は readonly storage buffer。material shader からの write は扱わない。
- image accessor は現在 `sampler2D` であり、shared 2D と sequential per-view 2D を扱う。
  layered multiview の `sampler2DArray` accessor は未実装なので明示 reject する。
- image は sampled access のみ。storage image material access は実 workload が現れた時に、
  output/hazard 契約と一緒に追加する。
- static texture dimension、RT mip/layer/subresource、compare/anisotropy sampler は
  WP209a/b の範囲である。
- indirect dispatch/draw argument は WP210、scalable light inventory は WP208 の範囲である。

## 検証

- surface parser/compiler test は kind、element、stage、generated accessor、
  descriptor kind/name/stage reflection を検証する。
- material pass/parser/planner test は image/buffer mapping、same-frame producer edge、
  storage intent、footprint、history、missing producer、current/history 混在を検証する。
- headless Vulkan dogfood は compute が書いた `vec4` buffer を vertex hook が読み、
  quad を変位させる。同時に別 fullscreen pass の画像を fragment hook が sampled port で読み、
  最終画像の色と重心移動を確認する。
- 同 dogfood は render-target recreate、新しい buffer ID を持つ正常 pipeline reload、
  壊れた resource mapping の rollback、各段階の descriptor revision と画像一致を検証する。
- 既存 material/screen-input GPU tests と全 CTest を併走し、回帰がないことを確認する。
