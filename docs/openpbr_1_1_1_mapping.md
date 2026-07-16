# OpenPBR 1.1.1 real-time subset 規範

本書は WP117 / M-PBR0b の数値・texture・import 写像の規範である。実装対象は
base / specular / IOR / coat 1 層 / emission / normal / alpha に限定し、
USD/MaterialX 自体の読込みは U-USD0c に残す。

## 1. exact pin

- upstream: `AcademySoftwareFoundation/OpenPBR`
- tag: `v1.1.1`
- commit: `f8d6d947dfae4c9b599965a86c22826ea7a8dbfb`
- 列挙元: `reference/open_pbr_surface.mtlx`

同じ pin は `engine://surfaces/openpbr/manifest.json` と example material の
`OPENPBR_PIN_*` define に記録する。patch 更新も自動互換とは扱わず、表・fixture・
golden の明示更新を要する。

## 2. wrapper-B と公開 lighting 境界

artifact は `{opaque,mask,blend} × {single,double}` の 6 `.surface` wrapper。
wrapper の param/texture 宣言は名前・順序・型・default・colorspace まで同一で、
差は alpha mode 定数、double-sided 定数、blend/cull/depth state だけである。
BRDF 本体は登録済みの単一
`engine://shaders/material/openpbr_lighting.glsl` に置く。この include が使うのは
`PelicanSurfaceV1`、生成済み param/texture accessor、`pelican_light`、
`pelican_shadow`、`pelican_env_ambient` の公開 API だけであり、standard/toon と同じ
非特権境界である。OPAQUE/MASK は `forward_opaque`、BLEND は
`forward_transparent` へ送る WP116 routing をそのまま使う。

## 3. v1 subset 数表

`value CS` は定数値の解釈、`texture CS/channel` は texture override の view と
利用 channel。color texture は sRGB view で decode 後 scene-linear、data texture は
UNORM/linear とする。法線は tangent-space RGB (`[0,1]→[-1,1]`)。

| input | type | unit / range | default | value CS | texture CS / channel |
|---|---|---|---|---|---|
| `base_weight` | float | unitless `[0,1]` | `1` | linear | linear / R |
| `base_color` | color | reflectance `[0,1]` | `(0.8,0.8,0.8)` | scene-linear | sRGB / RGB |
| `base_diffuse_roughness` | float | unitless `[0,1]` | `0` | linear | linear / R |
| `base_metalness` | float | unitless `[0,1]` | `0` | linear | linear / R |
| `specular_weight` | float | `[0,+∞)` (UI soft max 1) | `1` | linear | linear / R |
| `specular_color` | color | tint `[0,1]` | `(1,1,1)` | scene-linear | sRGB / RGB |
| `specular_roughness` | float | unitless `[0,1]` | `0.3` | linear | linear / R |
| `specular_ior` | float | ratio `[0,+∞)` (UI soft 1–3) | `1.5` | linear | linear / R |
| `coat_weight` | float | unitless `[0,1]` | `0` | linear | linear / R |
| `coat_color` | color | tint `[0,1]` | `(1,1,1)` | scene-linear | sRGB / RGB |
| `coat_roughness` | float | unitless `[0,1]` | `0` | linear | linear / R |
| `coat_ior` | float | ratio `[0,+∞)` (UI soft 1–3) | `1.6` | linear | linear / R |
| `coat_darkening` | float | unitless `[0,1]` | `1` | linear | linear / R |
| `emission_luminance` | float | cd/m² `[0,+∞)` (UI soft max 1000) | `0` | linear | linear / R |
| `emission_color` | color | chromaticity scale `[0,1]` | `(1,1,1)` | scene-linear | sRGB / RGB |
| `geometry_opacity` | float | coverage `[0,1]` | `1` | linear | linear / R |
| `geometry_normal` | normal | unit vector | geometry normal | linear | linear / tangent RGB |
| `geometry_coat_normal` | normal | unit vector | geometry normal | linear | linear / tangent RGB |
| `alpha_cutoff` | float | coverage `[0,1]` | `0.5` | linear | なし (import metadata) |

定数と texture は乗算する。MASK の境界は `alpha >= alpha_cutoff` を keep、未満を
discard。BLEND だけ `geometry_opacity` を出力 alpha に使い、OPAQUE は 1 を出す。
法線 scale は Pelican 側の補助 param で、OpenPBR input の replacement ではない。

## 4. import 写像

完全な machine fixture は `test/fixtures/openpbr/mapping_v1_1_1.json`。以下は要約。
「copy」は factor と採用 texture を同じ target の param/map へ分けて保持する。

| source | source input | target / rule |
|---|---|---|
| UsdPreviewSurface | `diffuseColor` | `base_color`; `base_weight=1` |
| UsdPreviewSurface | `metallic`, `roughness`, `ior` | `base_metalness`, `specular_roughness`, `specular_ior` |
| UsdPreviewSurface | `emissiveColor` | RGB を最大成分で正規化して `emission_color`; 最大成分を `emission_luminance` |
| UsdPreviewSurface | `opacity`, `normal` | `geometry_opacity`, `geometry_normal_map` |
| MaterialX `open_pbr_surface` | subset の同名 input | constant 又は image の R/RGB/normal を同名 param/map へ直接写像 |
| glTF core | base color RGB/A | `base_color` / `geometry_opacity` |
| glTF core | metallic/roughness/normal | `base_metalness` / `specular_roughness` / `geometry_normal_map` |
| `KHR_materials_specular` | factor/color | `specular_weight` / `specular_color` |
| `KHR_materials_ior` | `ior` | `specular_ior` |
| `KHR_materials_clearcoat` | factor/roughness/normal | `coat_weight` / `coat_roughness` / `geometry_coat_normal_map` |
| glTF emission | `emissiveFactor * emissiveStrength` | PreviewSurface と同じ color/luminance 分解。1 glTF unit = 1 cd/m² の preview 規約 |
| glTF state | `alphaMode`, `alphaCutoff`, `doubleSided` | WP116 `routing` と `alpha_cutoff` |

MaterialX allowlist は `open_pbr_surface` の上表 subset inputについて、constant、image、
R/RGB channel extract、declared colorspace、既定 texcoord/2D transform、normalmap まで。
任意 nodegraph 実行や unsupported lobe は許可しない。U-USD0c はこの表を消費するが、
本 WP は importer を実装しない。

## 5. unsupported WARN

対象例は anisotropy、transmission、subsurface、fuzz、thin film、thin-walled。
node/input が存在するだけでは WARN しない。次のいずれかだけを
`OPENPBR_UNSUPPORTED_INPUT` として出す。

1. 非 default 値が authored されている。
2. connection があり、その connection が結果へ寄与する。

出力 field は `code`, `prim_path`, `input`, `fallback` の固定 4 項目。
default authored、未接続、又は非寄与 connection は無音とする。runtime 面は
`makeOpenPbrUnsupportedInputWarning` がこの判定と compact JSON を提供し、U-USD0c は
同じ関数を使う。
