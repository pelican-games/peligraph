# Ray query / RT pipeline 用加速構造と変形ジオメトリ

## 先に 1 文だけ定義しておく語

先に、この章で使う 3 組の用語を説明します。詳しい処理は後の各節で扱います。

- **静的ジオメトリ / 動的ジオメトリ** — 静的ジオメトリは元の頂点をそのまま使います。スキン付き / morph / VAT は compute で変形後の頂点を生成して加速構造へ渡します。
- **BLAS / TLAS** — BLAS(bottom-level acceleration structure)はジオメトリ群の空間分割を GPU が辿れる形に焼いた下位構造、TLAS(top-level acceleration structure)はその BLAS を変換行列・mask・custom index 付きの**インスタンス行**として並べた上位構造です。ray は TLAS から引き、TLAS の行が BLAS を指します([用語集](../manual/14_glossary.md))。
- **ray query / ray tracing pipeline** — ray query は通常の fragment / compute シェーダの中から TLAS を辿る使い方(`VK_KHR_ray_query`)、ray tracing pipeline は raygen / miss / closest-hit という専用ステージと **SBT**(shader binding table — どの stage のどの shader を呼ぶかの handle をバイト列へ並べた表)を持ち `vkCmdTraceRaysKHR` で起動する使い方(`VK_KHR_ray_tracing_pipeline`)です。この章は両方を別々の opt-in feature として扱い、同じ影を 2 通りに計算して突き合わせます。

## 変形ジオメトリの契約

`PolygonInstanceContainer::recordRayDeformation` は、AS 構築前に現在の時刻・VAT texture・morph weight・skin palette を読み、VAT → morph → skin の順で変形します。VAT の補間、loop、終端clamp、法線textureもラスタと共有します。計算式はラスタ描画の GLSL helper と共有します。出力は位置・法線・接線・UV・色を持つ `CommonVertStruct` で、BLAS と hit shader の属性参照は同じ頂点バッファを使います。

BLAS の入力条件を満たさない形状は除外し、その理由を診断に記録します。スキン・モーフ・VAT であること自体は除外理由ではありません。`Renderer::currentFramePlanJson()` の `ray_query_acceleration_structures` 診断には、除外したプリミティブ数・インスタンス数、スキン付き / morph / VAT / `blas_ineligible` 別の件数、および asset / geometry allocation / mesh / primitive / node を含む名前が出ます。`blas_ineligible` は、頂点または索引が空、索引数が三角形を構成できない、あるいは頂点オフセットが負で、ラスタライズはできても BLAS の三角形入力条件を満たさないプリミティブです。同じ分類は初回構築時と分類変更時にログにも記録されます。対象外が存在すること自体はエラーではありません。

変形出力はインスタンスごと・論理フレームごとに作成します。同じ個体・元geometry・topologyなら前回BLASを読み取り元に、新規BLASへrefitします。初回とモデル・geometry poolの変更時はrebuildします。同じメッシュの別インスタンスの姿勢は独立します。出力・compute descriptor・pipeline・refit元はsubmission完了まで保持し、使用中のASを書き換えません。静的BLASの再利用は維持します。変形出力が無いまま元姿勢をASに渡すことはエラーにします。

frame planの`blas_build_count`と`blas_update_count`で、作り直しとrefitの累積回数を区別できます。refitはVulkanの[out-of-place UPDATE](https://docs.vulkan.org/spec/latest/chapters/accelstructures.html)を使い、出力bufferの再確保は残ります。

同じ個体のtriangle行は、連続q・同一transform・同一instance制御の条件で1つのBLASへ結合します。materialとgeometry flagsは個別に保ち、SBTは`q_base + gl_GeometryIndexEXT`の行を参照します。静的共有と動的refitのkeyはgeometry列全体で比較します。診断の`geometry_count`で結合数が分かり、`tlas_instance_count`は結合後の件数です。姿勢やmaskの違いで分割された場合は、新しいgeometry列のBLASを用意します。

構築処理はframe graphの`__scene_acceleration_structure` nodeと、名前付きTLASごとの`__scene_acceleration_structure:<name>` nodeで実行します。前者が変形compute・共有BLAS・default TLAS・SBTを用意し、後者が対象TLASを構築します。最後のAS操作でdescriptor bundleをまとめて公開し、全consumerはその完成を待ちます。XRでは論理フレーム内で共有します。ray queryだけをshaderから使う場合も、graphの`required_capabilities`へ`pelican.vulkan.ray_query@1`を宣言して構築依存を明示します。

`scene_acceleration_structures`を宣言すると、同じBLAS群からinstance mask別の名前付きTLASも構築します。RT taskの`ray_tracing.acceleration_structure`で選択し、省略時は全sceneです。qとSBTは共通のまま、TLAS別のdescriptor bundleをbindします。全TLAS・descriptorを同じpublicationで所有し、task間で使用中descriptorを変更しません。`named_tlas_instance_counts`に各集合の件数が出ます。

加速構造 scope は、物理計画が `pelican.vulkan.ray_query@1` を要求するか、登録したシェーダの reflection が frame set の TLAS binding を検出した場合に `RenderPipelineGpuResourceScope` へ登録されます。後者により、独自パスが feature manifest の能力宣言を経由しなくても TLAS layout だけを作って実行時に欠けることはありません。同じ reflection 判定が拡張 frame-set layout、実デバイスの能力確認、owner scope 登録を決めます。どちらも要求しないプロジェクトでは scope 自体が無く、BLAS / TLAS の構築回数は 0 のままです。scope の差し替えは既存 GPU owner scope の purge に従います。

適格なインスタンスが 0 件でもエラーにはしません。Vulkan が 0 primitive の build にも要求する有効な instance-data address を inactive record で与え、0-instance の miss-only TLAS を構築します。したがって descriptor は常に有効で ray はすべて miss し、マスクは全面可視になります。`tlas_instance_count == 0` と BLAS-ineligible の除外件数は通常どおり診断へ出ます。

BLAS が保持するのは大域 index / vertex pool のデバイスアドレスです。そのため `VertBufContainer::ensureIndexCapacity` または `VertBufContainer::ensureVertexCapacity` がプールを再確保すると、全 BLAS を破棄して現在のアドレスから作り直します。`PolygonInstanceContainer::rebuildModelInstances` も同じく BLAS 無効化世代を進めます。一方、transform は BLAS を無効化せず、フレームごとに再構築する TLAS のインスタンス行列へ反映します。

構築中の storage、scratch、instance buffer は、新しいフレーム数ベースの退役規則を持ちません。描画 submission が取得済みの `DeletionQueue::leaseForNextSubmission` と同じバッチへ defer され、`confirmSubmission` 後も実 fence の lease が解放されるまで保持されます。

## AS 行の入力と identity

TLAS の行がどこから来るかは、3つの型を順に読むと追えます。宣言面(mask / flags / custom index / material 鍵規則)そのものは[第6章 §6.20](06_rendering_vulkan_shader.md)にあります。

identity が層ごとに姿を変えるのがこの経路の読みどころです。同じ 1 行を指す identity が、snapshot では**構造化された field 群**、`RaySceneRowInput` では**3 本の文字列**、material 側では**1 つの typed union** になります。名前が似ていて役割が違うので、まず全体をこの形で掴んでから各節へ入ってください。3 本に分かれているのは、`scene_identity` と `geometry_identity` が **canonical sort の第1/第2キー**であるのに対し、3 本目の `geometry_match_identity` は sort キーではなく**曖昧性 guard の等価域**だからで、ヘッダのコメントがその理由を書いています — 「authored 側の asset / mesh / node の別名が、同じ位置にある同じ geometry を別物に見せてはいけない」([`RaySceneRowInput` の `geometry_match_identity` の上](../../src/core/shader/raygrouplayout.hpp#L248))。詳細は §「`RaySceneRowInput` の identity 3 種」。

```text
draw inventory
  ↓ renderer が記録開始前に凍結
RayQueryGeometryInstanceSnapshot        生の identity と range(model 型・draw inventory 型に依存しない)
    identity : asset_identity, authoring_object_id, instance_index + instance_generation,
               scene_epoch
    material : material_asset_reference, material_document_reference, material_name,
               source_material_index(std::optional)
    geometry : geometry_allocation_id, index_offset / index_count,
               vertex_offset / vertex_count
  ↓ identity 文字列 3 本 + material 4 field へ整形
RaySceneRowInput
    scene_identity          = asset@<canonical transform>
                              … 永続的な同一性。canonical sort の第1キー(publish 順を含まない)
    geometry_identity       = asset#allocation:index_offset:vertex_offset:mesh:primitive:node
                              … 同一 object 内で primitive を区別する第2キー
    geometry_match_identity = allocation:index_offset:index_count:vertex_offset:vertex_count
                              @<transform>
                              … sort キーではなく曖昧性 guard の等価域(物理 GPU 範囲 + 変換のみ)
  ↓ material 4 field を materialRowIdentity() で 1 つへ正準化
MaterialRowIdentity   = MaterialRowDocumentIdentity{document, material_name?}
                      | MaterialRowAssetIdentity{asset, source_material_index}
                      | MaterialRowIdentityUnavailable
                      … AS rule の gate も material 表の lookup も、この 1 個だけを見る
  ↓ canonical sort → 添字 q を付与
RaySceneRow                             q / group / record / 解決済み AS 値
```

### `RayQueryGeometryInstanceSnapshot`

[`RayQueryGeometryInstanceSnapshot`](../../src/core/vkcore/accelerationstructure.hpp#L31) は、記録が始まる前に renderer 側で凍結される値です。ヘッダのコメントが境界を規範として書いています — **vkcore は安定した identity と range だけを見て、model 型や draw inventory 型には依存しない**。

| 区分 | field |
|---|---|
| identity | `asset_identity`、`authoring_object_id`、`instance_index` / `instance_generation`、`scene_epoch` |
| 宣言 | [`std::optional<RayInstanceComponent> ray_instance`](../../src/core/vkcore/accelerationstructure.hpp#L38) |
| material selector | `material_asset_reference`、`material_document_reference`、`material_name`、[`std::optional<std::uint32_t> source_material_index`](../../src/core/vkcore/accelerationstructure.hpp#L50)、`resolved_material_handle` / `_generation` / `_lease` |
| geometry | `geometry_allocation_id`、`index_count` / `index_offset`、`vertex_offset` / `vertex_count`、`attribute_presence` |
| 変形方式 | `skinned` / `morph_deformed` / `vat_deformed` |
| 名前付けと配置 | `mesh_index`、`primitive_index`、`node_index`、`world_transform` |

`authoring_object_id` が入っているのは、`pelican_ray_instance` が **object 単位**の宣言だからです。宣言を持つ authoring object が1つも BLAS 適格な geometry を持たないとき、[`pelican.ray_instance_not_ray_traced@1`](../../src/core/renderer/accelerationstructure.cpp#L604) で止めます。skin/morph は適格として宣言を解決し、GPU記録時に変形出力の存在を確認します。

### `RaySceneRowInput` の identity 3 種

[`RaySceneRowInput`](../../src/core/shader/raygrouplayout.hpp#L242) は identity 文字列を3本持ちます。名前が似ていますが役割は別で、混ぜると壊れます([resolveRayTracingSceneRows()](../../src/core/renderer/accelerationstructure.cpp#L568))。

| field | 形 | 役割 |
|---|---|---|
| [`scene_identity`](../../src/core/shader/raygrouplayout.hpp#L246) | `asset@<canonical transform>` | 永続的な scene 上の同一性。`q` の canonical sort の第1キー。**draw inventory の publish 順を意図的に含みません**(instance slot / generation / 公開順序を入れると `q` が公開順に依存します) |
| `geometry_identity` | `asset#allocation:index_offset:vertex_offset:mesh:primitive:node` | 同一 object 内で primitive を区別する第2キー |
| [`geometry_match_identity`](../../src/core/shader/raygrouplayout.hpp#L252) | `allocation:index_offset:index_count:vertex_offset:vertex_count@<transform>` | **曖昧性 guard の等価域**。canonical sort キーと違い、物理的な GPU geometry 範囲と canonical transform だけを含みます |

3本目だけ設計意図が違います。コメントが理由を書いています — authored 側の asset / mesh / node の別名が、**同じ位置にある同じ geometry を別物に見せてはいけない**からです。sort 後に隣接行を突き合わせ、`geometry_match_identity` が一致するのに group か record data が違えば [`pelican.ray_row_binding_ambiguous@1`](../../src/core/shader/raygrouplayout.cpp#L1738) で止めます。

`q` は sort 後の添字で、行数が 2^24 を超えると [scene row q の24-bit上限診断](../../src/core/shader/raygrouplayout.cpp#L1750) です(instance SBT offset が 24 bit のため)。

### `MaterialRowIdentity`: gate と lookup が同じ値を見る

material 側は元々 `{document, material_name, asset, source_material_index}` の4 field がバラバラに使われていて、AS rule の gate と material lookup が別の解釈をする組み合わせがありました。WP383 で正準化し、[`MaterialRowIdentity`](../../src/core/shader/raygrouplayout.hpp#L298) という **1つの typed union** に寄せてあります。

```cpp
using MaterialRowIdentity =
    std::variant<MaterialRowDocumentIdentity, MaterialRowAssetIdentity,
                 MaterialRowIdentityUnavailable>;
```

正準化は [`materialRowIdentity()`](../../src/core/shader/raygrouplayout.cpp#L61) の1関数だけが行い、規則は次です。

| 入力 | 結果 |
|---|---|
| `material_document` が非空 | [`MaterialRowDocumentIdentity{document, material_name}`](../../src/core/shader/raygrouplayout.hpp#L282)。**document の存在が勝ちます** |
| document が空、`material_asset` が非空、`source_material_index` が有る | [`MaterialRowAssetIdentity{asset, source_material_index}`](../../src/core/shader/raygrouplayout.hpp#L287) |
| 同上で index が `UINT32_MAX` | [`pelican.ray_material_index_out_of_range@1`](../../src/core/shader/raygrouplayout.cpp#L72) |
| それ以外 | [`MaterialRowIdentityUnavailable`](../../src/core/shader/raygrouplayout.hpp#L292) |

**`material_name` は optional**です。単一 material の `.mat.json` のように名前を持たない document material があるので、「document かつ名前あり」を Document の条件にすると、それが Asset か Unavailable へ落ちて document selector が解決できなくなります。document selector 側も、名前が空なら名前を問わず一致し、名前付きなら一致を要求する非対称になっています([`rayAsSelectorMatches()`](../../src/core/shader/raygrouplayout.cpp#L718))。

もう1つの要点は `source_material_index` が `RaySceneRowInput` / snapshot の両方で `std::optional` であることです。コメントが規範を書いています — **欠損した index が黙って 0 になってはいけません**。glTF の直読みなどで material が無い primitive は index を持たず、identity は `Unavailable` になります。0 に丸めると、たまたま 0 番 material を指す asset selector が誤って一致します。

この単一 identity は、AS rule の gate([`rayAsSelectorMatches()`](../../src/core/shader/raygrouplayout.cpp#L718))、ray bindings の selector 照合([`selectorMatches()`](../../src/core/shader/raygrouplayout.cpp#L470))、`material_reference` の生成、material 表の lookup がすべて同じ値を見ます。

## `rt_shadow_mask` feature

`engine://features/rt_shadow_mask.json` は、ray query で最初に画素を変える opt-in feature です。`deferred_geometry` の直後に通常の `fullscreen` pass を挿入し、`gbuffer_worldpos` の位置と `gbuffer_normal` の法線から先頭の directional light へ shadow ray を飛ばします。出力は feature 専有の `R8_UNORM` render target `rt_shadow_mask` です。この WP では他の pass はマスクを消費せず、feature を外すと pass と target の両方が消えます。

シェーダの TLAS は set 0 の binding 6 (`PELICAN_RAY_QUERY_TLAS_BINDING`) にあります。set 1 は従来どおり fullscreen の画像入力だけで、`gbuffer_worldpos` が binding 0、`gbuffer_normal` が binding 1 を使います。`PipelineFactory` は TLAS binding を reflection した pipeline だけに 7-binding の frame-set layout を使い、`FrameResources` もその場合だけ拡張 descriptor set と acceleration-structure pool entry を確保します。layout handle は reflection 結果から作る hash set に登録されるため、描画ごとの判定は登録済み pipeline 数を走査しません。ray query を要求しない graph は従来の 6-binding layout、descriptor set 数、uniform/storage descriptor 数のままです。

feature は早期の target planning のため `pelican.vulkan.ray_query@1` を graph の required capabilities に伝播します。加えて TLAS binding の reflection 自体が同じ能力を要求するため、manifest を介さない独自シェーダも能力の無い環境では `pelican.plan.ray_query_required_unavailable@1` で失敗します。TLAS binding の識別規則を layout と能力で別々に持つことはありません。GLSL source と SPIR-V 1.5 の埋め込み成果物を両方登録しているため、`PELICAN_RUNTIME_SHADER_COMPILER=ON` は source compile、`OFF` は `.spv` を選びます。

マスクは可視なら 1、最初の交差があれば 0 の hard shadow です。`gbuffer_worldpos.a` は被覆信号で、標準 `deferred_geometry` は背景を 0 で clear し、材質シェーダは描いた画素へ 1 を書きます。背景は TLAS へ ray を出しません。

ray origin は光の側へ向けた G-buffer 法線方向にずらします。bias は最小 0.01 に加えて `max(abs(world_position)) / 512` を使います。これは半精度の world position の量子化による自己ヒットを避けるためです。最大距離は 10000 で、現時点では先頭の directional light だけを扱います。static / skinned / morph / VAT blocker は影を作ります。不適格な三角形範囲は除外診断に出ます。

## `rt_shadow_mask_pipeline` feature

`engine://features/rt_shadow_mask_pipeline.json` は、同じ hard shadow を `VK_KHR_ray_tracing_pipeline` の raygen / miss / closest-hit と SBT でも計算する、別の opt-in feature です。既存の ray query feature を置き換えません。両方を同時に有効化でき、RT pipeline 版は専有の `R8_UNORM` storage image `rt_shadow_mask_pipeline` へ `imageStore` します。set 0 binding 6 の TLAS、set 1 binding 0/1 の world position/normal、同 binding 2 の storage image が固定 ABI です。

この処理を fullscreen pass にしてはいけません。`vkCmdTraceRaysKHR` は dynamic rendering scope 内へ記録できないため、feature は既存の最上位 `compute_tasks` に ray-tracing 宣言を載せます。frame plan 上の kind は `compute` のままであり、通常の compute dispatch と同じく render pass の外で実行されます。`dispatch.rays_from.port` が選んだ image mip の幅・高さをそのまま trace dimensions にし、1 画素につき 1 raygen invocation を起動します。workgroup の切り上げ除算は行いません。

raygen は ray query shader と同じ被覆判定、先頭 directional light、法線側 bias、最大距離 10000、opaque/first-hit flag を使います。miss は payload へ 1、closest-hit は 0 を書き、raygen が storage image へ保存します。既存テストは背景・影・可視域・遠方のscene・空のTLAS・skin/morph・不適格な三角形範囲について、両経路のR8 targetを比較します。埋め込みSPIR-Vの経路もあります。

設定で使える RT stage は `raygen` / `miss` / `closesthit` だけです。`any_hit` / `intersection` / `callable` を `ray_tracing` 内または task 直下に宣言すると `pelican.ray_tracing.unsupported_shader_stage@1` で停止します。未知の stage も無視しません。

デバイス選択では、WP280 の acceleration structure / ray query / deferred host operations / buffer device address が揃った上で、RT pipeline feature、extension、`shaderGroupHandleSize` / `shaderGroupHandleAlignment` / `shaderGroupBaseAlignment` の全てが有効な場合だけ `pelican.vulkan.ray_tracing_pipeline@1` を公開します。途中までしか揃わない場合、RT pipeline extension を部分的に有効化しませんが、成立している ray query 経路は維持します。要求 graph を非対応 endpoint へ割り当てると `pelican.plan.ray_tracing_pipeline_required_unavailable@1`、有効化済み extension の PFN が取得できない場合は `pelican.vulkan.ray_tracing_pipeline_entry_points_unavailable@1` です。

SBT の byte layout は [`shaderbindingtable.cpp`](../../src/core/shader/shaderbindingtable.cpp) の純粋関数で決めます。record stride は handle size を handle alignment へ切り上げ、raygen / miss / hit の各 region offset はそれぞれ base alignment へ切り上げます。実バッファでは device address 自体を base alignment へ進め、handle の実データだけを各 stride の先頭へコピーします。この計算は Vulkan device を作らない単体テストで、handle size と handle alignment が異なる場合、および base alignment の方が大きい場合を別々に固定しています。`PipelineFactory` は RT pipeline、shader group handle、SBT buffer を同じ pipeline record として所有し、hot reload の世代退役にもまとめて載せます。

同じ static blocker を directional raster shadow と比較すると、影領域は一部重なりますが完全一致しません。主な差は次のとおりです。

- ray mask は二値の 0 / 1 ですが、現在の raster lighting は shadow visibility 0.35 / 1 を BRDF に掛けます。
- raster は cascade の投影範囲、shadow-map 解像度、depth bias の影響を受けます。ray query は world-space origin bias と最大距離を使うため、輪郭と接触部がずれます。
- ray query は opaque ray flag を使います。material/shadow-caster の条件は raster 側と対象集合が異なります。

このため raster shadow との比較テストは同一性ではなく、同じ scene で raster の暗化画素が存在すること、ray mask と重なる画素が存在すること、かつ異なる画素も存在することを検証します。一方、ray query 版と RT pipeline 版の比較は前述のとおり完全一致です。

## AS 宣言を編集する RPC は「節 locator」を通る

`scene_acceleration_structures` を読み書きする 2 本の RPC — [`getSceneAccelerationStructures()`](../../src/core/communication/renderconfigeditor.cpp#L1860) と [`editSceneAccelerationStructures()`](../../src/core/communication/renderconfigeditor.cpp#L1884) — は、rendering config の root を直接触りません。読みは [`renderingSectionOrEmpty()`](../../src/project/renderconfigsection.hpp#L49)、書きは [`withSectionValue()`](../../src/project/renderconfigdocument.cpp#L691) を通します。したがって扱うのは **`pelican.graph` root の `rendering.scene_acceleration_structures`** です。P-2a-4b から MG-2b までは legacy root の最上位の `scene_acceleration_structures` も同じ RPC で扱えましたが、MG-2b で legacy root 自体が `render_config.legacy_root@1` で拒否されるようになりました([第6章 §6.1](06_rendering_vulkan_shader.md) の「rendering config の root は `pelican.graph` だけ」)。

read が返すのは 6 キーです。

| キー | 中身 |
|---|---|
| `source_reference` / `source_digest` | 編集の CAS に使う文書参照とダイジェスト |
| `definitions` | 節の `scene_acceleration_structures` 配列そのまま |
| `task_connections` | ray を使う compute task ごとの `{name, authored_acceleration_structure, acceleration_structure}` |
| `triangle_geometry` / `procedural_geometry` | ray group layout 側の座標空間情報(`editable` と `source_digest` を含む) |

`task_connections` の 2 本立てが読みどころです。task が文書に書いた名前(省略時は `"scene"`)が `authored_acceleration_structure`、それを節の [`ray_task_acceleration_structures`](../../src/core/communication/renderconfigeditor.cpp#L1867) が上書きした結果が `acceleration_structure` で、**上書きは task の宣言を書き換えずに別のキーへ積まれます**。編集側も同じ形で、[接続の検査](../../src/core/communication/renderconfigeditor.cpp#L1938)が「知らない ray task」と「知らない AS 名」を別々の拒否にしてから `ray_task_acceleration_structures` を書きます([書き込み](../../src/core/communication/renderconfigeditor.cpp#L1954))。ただしこの 1 本目は `task_connections` を渡したときだけで、省くと [`scene_acceleration_structures` の書き込み](../../src/core/communication/renderconfigeditor.cpp#L1956)だけが走ります。

もう一段の読みどころは、この 1 回の編集が**2 種類の文書**を跨ぐことです。`triangle_space` / `procedural_space` を渡すと ray group layout 文書側の `acceleration_structure.triangle_space` / `procedural_space` を書き換えることになるため、rendering config の `base_source_digest` とは別に [`base_layout_digest`](../../src/core/communication/renderconfigeditor.cpp#L1892) が必須になり(片方だけ渡すと拒否)、layout 文書は候補文書として同じ transaction に積まれます([候補の構築](../../src/core/communication/renderconfigeditor.cpp#L1921))。空間を書き換えた bytes は保存前に [`parseRayGroupLayout()` で往復検証](../../src/core/communication/renderconfigeditor.cpp#L1920)されます。`definitions` 自体は「その値だけを持つ config を compile してみる」という形で検証され([検証](../../src/core/communication/renderconfigeditor.cpp#L1931))、通らなければ `invalid_scene_acceleration_structures` です。

## 出荷サンプル `projects/raytracing_demo`

この章の内容を最小構成で読める出荷 project が [`projects/raytracing_demo`](../../projects/raytracing_demo) です。配置・半径 / AABB・shader record・交点・陰影・表示までが 1 プロジェクトに収まっており、rendering config は MG-2b から **`pelican.graph` root** で、[`rendering.json`](../../projects/raytracing_demo/rendering.json) の `rendering` 節の先頭 3 キーが `ray_group_layout` / `ray_bindings` / `features`(`project://features/spheres.json`。出荷 7 project のうち project 側 feature を持つのはこれと `pathtrace_demo` の 2 本)です。文書の版は [`ray_layout.json`](../../projects/raytracing_demo/ray_layout.json) が `pelican.ray_group_layout` v3、[`ray_bindings.json`](../../projects/raytracing_demo/ray_bindings.json) が `pelican.ray_bindings` v2 で、MG-2a からエンジンが読むのはこの版だけです。同じ 2 文書を持つ出荷 project はもう 1 本あり、[`projects/pathtrace_demo`](../../projects/pathtrace_demo)([`ray_layout.json`](../../projects/pathtrace_demo/ray_layout.json) v3 / [`ray_bindings.json`](../../projects/pathtrace_demo/ray_bindings.json) v2)です(版の一覧は [`docs/v0_freeze.md`](../v0_freeze.md) §1.1)。

**以前の既知の穴(解消済み)**: Studio で開くと、埋め込み engine が起動直後に [`Renderer logical frame requires a compiled render pipeline`](../../src/core/vkcore/renderer.cpp#L5201) で落ちることがありました。shader cache が効いて起動が速いと、窓の出力より先に最初の frame が来て、出力待ちで飛ばした frame が staged な起動 generation を巻き戻していたのが原因です。`7cb39996` / `e5995d88` で直り、[v0 凍結一覧 §4.1](../v0_freeze.md) が Studio での起動・capture・開き直し(10 / 10)を確認しています。
