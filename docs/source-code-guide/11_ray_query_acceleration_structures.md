# Ray query 用加速構造の静的限定契約

`RayQueryAccelerationStructureScope` が作る BLAS / TLAS は、静的ジオメトリ専用です。`VertBufContainer` に存在する頂点は元姿勢だけであり、頂点シェーダで変形されるスキン付き、morph、VAT のプリミティブを BLAS に入れてはいけません。入れると、描画と異なるバインドポーズの形状が ray query から見えてしまいます。

この除外は無言のフォールバックではありません。`Renderer::currentFramePlanJson()` の `ray_query_acceleration_structures` 診断には、除外したプリミティブ数・インスタンス数、スキン付き / morph / VAT / `blas_ineligible` 別の件数、および asset / geometry allocation / mesh / primitive / node を含む名前が出ます。`blas_ineligible` は、頂点または索引が空、索引数が三角形を構成できない、あるいは頂点オフセットが負で、ラスタライズはできても BLAS の三角形入力にはできない静的プリミティブです。同じ分類は初回構築時と分類変更時にログにも記録されます。対象外が存在すること自体はエラーではありません。

変形後ジオメトリを生成する compute deform pass と、その出力からの BLAS 構築は後続 WP の範囲です。現時点でスキン付き、morph、VAT を静的 BLAS に代入する経路はありません。

加速構造 scope は、物理計画が `pelican.vulkan.ray_query@1` を要求するか、登録したシェーダの reflection が frame set の TLAS binding を検出した場合に `RenderPipelineGpuResourceScope` へ登録されます。後者により、独自パスが feature manifest の能力宣言を経由しなくても TLAS layout だけを作って実行時に欠けることはありません。同じ reflection 判定が拡張 frame-set layout、実デバイスの能力確認、owner scope 登録を決めます。どちらも要求しないプロジェクトでは scope 自体が無く、BLAS / TLAS の構築回数は 0 のままです。scope の差し替えは既存 GPU owner scope の purge に従います。

適格な静的インスタンスが 0 件でもエラーにはしません。Vulkan が 0 primitive の build にも要求する有効な instance-data address を inactive record で与え、0-instance の miss-only TLAS を構築します。したがって descriptor は常に有効で ray はすべて miss し、マスクは全面可視になります。`tlas_instance_count == 0` と変形 / BLAS-ineligible の除外件数は通常どおり診断へ出ます。

BLAS が保持するのは大域 index / vertex pool のデバイスアドレスです。そのため `VertBufContainer::ensureIndexCapacity` または `VertBufContainer::ensureVertexCapacity` がプールを再確保すると、全 BLAS を破棄して現在のアドレスから作り直します。`PolygonInstanceContainer::rebuildModelInstances` も同じく BLAS 無効化世代を進めます。一方、transform は BLAS を無効化せず、フレームごとに再構築する TLAS のインスタンス行列へ反映します。

構築中の storage、scratch、instance buffer は、新しいフレーム数ベースの退役規則を持ちません。描画 submission が取得済みの `DeletionQueue::leaseForNextSubmission` と同じバッチへ defer され、`confirmSubmission` 後も実 fence の lease が解放されるまで保持されます。

## `rt_shadow_mask` feature

`engine://features/rt_shadow_mask.json` は、ray query で最初に画素を変える opt-in feature です。`deferred_geometry` の直後に通常の `fullscreen` pass を挿入し、`gbuffer_worldpos` の位置と `gbuffer_normal` の法線から先頭の directional light へ shadow ray を飛ばします。出力は feature 専有の `R8_UNORM` render target `rt_shadow_mask` です。この WP では他の pass はマスクを消費せず、feature を外すと pass と target の両方が消えます。

シェーダの TLAS は set 0 の binding 6 (`PELICAN_RAY_QUERY_TLAS_BINDING`) にあります。set 1 は従来どおり fullscreen の画像入力だけで、`gbuffer_worldpos` が binding 0、`gbuffer_normal` が binding 1 を使います。`PipelineFactory` は TLAS binding を reflection した pipeline だけに 7-binding の frame-set layout を使い、`FrameResources` もその場合だけ拡張 descriptor set と acceleration-structure pool entry を確保します。layout handle は reflection 結果から作る hash set に登録されるため、描画ごとの判定は登録済み pipeline 数を走査しません。ray query を要求しない graph は従来の 6-binding layout、descriptor set 数、uniform/storage descriptor 数のままです。

feature は早期の target planning のため `pelican.vulkan.ray_query@1` を graph の required capabilities に伝播します。加えて TLAS binding の reflection 自体が同じ能力を要求するため、manifest を介さない独自シェーダも能力の無い環境では `pelican.plan.ray_query_required_unavailable@1` で失敗します。TLAS binding の識別規則を layout と能力で別々に持つことはありません。GLSL source と SPIR-V 1.5 の埋め込み成果物を両方登録しているため、`PELICAN_RUNTIME_SHADER_COMPILER=ON` は source compile、`OFF` は `.spv` を選びます。

マスクは可視なら 1、最初の交差があれば 0 の hard shadow です。`gbuffer_worldpos.a` は被覆信号で、標準 `deferred_geometry` は背景を 0 で clear し、材質シェーダは描いた画素へ 1 を書きます。背景は TLAS へ ray を出しません。

ray origin は射線方向ではなく、光の側へ向けた G-buffer 法線方向にずらします。bias は最小 0.01 に加えて `max(abs(world_position)) / 512` を使います。これは `R16G16B16A16_SFLOAT` の world position を原点から 100 以上離したときにも約 2 ULP を確保し、量子化された受け面への自己ヒットを避けるためです。最大距離は 10000 で、現時点では先頭の directional light だけを扱います。static receiver と static blocker の画素テストでは、影になる固定領域と可視の固定領域を別々に assertion します。一方、同じ位置に置いた skinned / morph / VAT blocker は G-buffer には描かれても TLAS に無いため、マスクに影が生じないことを個別の画素 assertion と除外診断の両方で固定しています。これは一時的な静的限定を見える形にした契約であり、golden による黙示的な正当化ではありません。

同じ static blocker を directional raster shadow と比較すると、影領域は一部重なりますが完全一致しません。主な差は次のとおりです。

- ray mask は二値の 0 / 1 ですが、現在の raster lighting は shadow visibility 0.35 / 1 を BRDF に掛けます。
- raster は cascade の投影範囲、shadow-map 解像度、depth bias の影響を受けます。ray query は world-space origin bias と最大距離を使うため、輪郭と接触部がずれます。
- ray query は静的 TLAS と opaque ray flag を使います。raster 側で扱える変形 geometry や material/shadow-caster の条件とは対象集合が異なります。

このためテストは同一性ではなく、同じ scene で raster の暗化画素が存在すること、ray mask と重なる画素が存在すること、かつ異なる画素も存在することを検証します。
