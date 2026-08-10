# Ray query 用加速構造の静的限定契約

`RayQueryAccelerationStructureScope` が作る BLAS / TLAS は、静的ジオメトリ専用です。`VertBufContainer` に存在する頂点は元姿勢だけであり、頂点シェーダで変形されるスキン付き、morph、VAT のプリミティブを BLAS に入れてはいけません。入れると、描画と異なるバインドポーズの形状が ray query から見えてしまいます。

この除外は無言のフォールバックではありません。`Renderer::currentFramePlanJson()` の `ray_query_acceleration_structures` 診断には、除外したプリミティブ数・インスタンス数、スキン付き / morph / VAT 別の件数、および asset / geometry allocation / mesh / primitive / node を含む名前が出ます。同じ分類は初回構築時と分類変更時にログにも記録されます。対象外が存在すること自体はエラーではありません。

変形後ジオメトリを生成する compute deform pass と、その出力からの BLAS 構築は後続 WP の範囲です。現時点でスキン付き、morph、VAT を静的 BLAS に代入する経路はありません。

加速構造 scope は、物理計画が `pelican.vulkan.ray_query@1` を要求した場合だけ `RenderPipelineGpuResourceScope` に登録されます。要求しないプロジェクトでは scope 自体が無く、BLAS / TLAS の構築回数は 0 のままです。scope の差し替えは既存 GPU owner scope の purge に従います。

BLAS が保持するのは大域 index / vertex pool のデバイスアドレスです。そのため `VertBufContainer::ensureIndexCapacity` または `VertBufContainer::ensureVertexCapacity` がプールを再確保すると、全 BLAS を破棄して現在のアドレスから作り直します。`PolygonInstanceContainer::rebuildModelInstances` も同じく BLAS 無効化世代を進めます。一方、transform は BLAS を無効化せず、フレームごとに再構築する TLAS のインスタンス行列へ反映します。

構築中の storage、scratch、instance buffer は、新しいフレーム数ベースの退役規則を持ちません。描画 submission が取得済みの `DeletionQueue::leaseForNextSubmission` と同じバッチへ defer され、`confirmSubmission` 後も実 fence の lease が解放されるまで保持されます。
