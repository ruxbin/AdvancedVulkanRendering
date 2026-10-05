



/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo drawcluster.vs.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneDepthOnly -Fo drawcluster.depth.ps.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePS -Fo drawcluster.ps.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePass -Fo drawcluster.base.ps.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneForwardPS -Fo drawcluster.forward.ps.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 deferredlighting.hlsl -fspv-debug=vulkan-with-source -E AAPLSimpleTexVertexOutFSQuadVertexShader -Fo deferredlighting.vs.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 deferredlighting.hlsl -E DeferredLighting -Fo deferredlighting.ps.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 pointspotlight.hlsl -fspv-debug=vulkan-with-source -E DeferredLighting -Fo deferredPointLighting.ps.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 pointspotlight.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo deferredPointLighting.vs.spv



/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -E CoarseCull -Fo CoarseCull.cs.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E TraditionalCull -Fo TraditionalCull.cs.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E ClearDebugView -Fo ClearDebugView.cs.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E ClearLightIndices -Fo ClearIndices.cs.spv

echo Spot light culling variants
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -E CoarseCullSpot -Fo CoarseCullSpot.cs.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E TraditionalCullSpot -Fo TraditionalCullSpot.cs.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E ClearLightIndicesSpot -Fo ClearLightIndicesSpot.cs.spv

echo GPU-Driven Optimization Shaders
echo Stage 1: GPU Culling \(opaque/alpha-mask/transparent split + Hi-Z\)
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 gpucull.hlsl -E EncodeDrawBuffer -Fo gpucull.cs.spv

echo Stage 1: Base pass alpha-mask variant \(reads material from SSBO\)
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePassAlphaMask -Fo drawcluster.base.alphamask.ps.spv

echo Stage 2: Shadow cascade culling
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 shadowcull.hlsl -E ShadowCull -Fo shadowcull.cs.spv

echo Stage 2: Shadow depth indirect \(reads material from SSBO for alpha\)
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneShadowDepthIndirect -Fo drawcluster.shadow.indirect.ps.spv

echo Stage 3: Hi-Z pyramid generation
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 hiz.hlsl -fspv-debug=vulkan-with-source -E CopyDepthToHiZ -Fo hiz_copy.cs.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 hiz.hlsl -fspv-debug=vulkan-with-source -E DownsampleHiZ -Fo hiz_downsample.cs.spv

echo Stage 4: Forward pass indirect \(reads material from SSBO\)
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneForwardPSIndirect -Fo drawcluster.forward.indirect.ps.spv


echo compiling drawOccluderVS
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 drawoccluders.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo occluders.vs.spv

echo compiling drawclustershadowVS
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVSShadow -Fo drawclusterShadow.vs.spv

echo compiling spot shadow VS \(reads viewProj from push constant, no camera UBO\)
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc  -spirv -T vs_6_0 drawclusterShadowSpot.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVSShadowSpot -Fo drawclusterShadowSpot.vs.spv


echo compiling scatter volume kernels \(two entry points from one source file\)
# ACCUM_PASS gates which binding block + kernel body scattervolume.hlsl exposes,
# so the accumulate entry point must be compiled with it defined.
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 scattervolume.hlsl -E ScatterVolume       -Fo scattervolume.cs.spv
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -D ACCUM_PASS -T cs_6_2 scattervolume.hlsl -E AccumulateScattering -Fo accumscatter.cs.spv

echo compiling drawoccludersvs
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -E RenderSceneVS drawoccluders.hlsl -T vs_6_0 -Fo occluders.wireframe.vs.spv

echo compiling drawoccludersps
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -E WireframePS drawoccluders.hlsl -T ps_6_0 -Fo occluders.wireframe.ps.spv


echo compiling sao
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 sao.hlsl -fspv-debug=vulkan-with-source -E ScalableAmbientObscurance -Fo sao.cs.spv

echo raytracing
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T lib_6_3 rt_lighting.hlsl -fspv-target-env=vulkan1.2 -fspv-extension=SPV_KHR_ray_tracing -fspv-extension=SPV_KHR_physical_storage_buffer -fspv-extension=SPV_KHR_non_semantic_info -fspv-extension=SPV_EXT_descriptor_indexing -Fo rt_lighting.lib.spv

echo compiling decalvs
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -E DecalVS decal.hlsl -T vs_6_0 -Fo decal.vs.spv

echo compiling decalps
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -E DecalPS decal.hlsl -T ps_6_0 -fspv-debug=vulkan-with-source -Fo decal.ps.spv

echo compiling resolve
/home/songjiang/SOURCE/ShaderConductor/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 resolve.hlsl -fspv-debug=vulkan-with-source -E ResolvePS -Fo resolve.ps.spv

