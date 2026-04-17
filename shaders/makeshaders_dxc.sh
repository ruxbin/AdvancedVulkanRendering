



/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo drawcluster.vs.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneDepthOnly -Fo drawcluster.depth.ps.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePS -Fo drawcluster.ps.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePass -Fo drawcluster.base.ps.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneForwardPS -Fo drawcluster.forward.ps.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 deferredlighting.hlsl -fspv-debug=vulkan-with-source -E AAPLSimpleTexVertexOutFSQuadVertexShader -Fo deferredlighting.vs.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 deferredlighting.hlsl -E DeferredLighting -Fo deferredlighting.ps.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 pointspotlight.hlsl -fspv-debug=vulkan-with-source -E DeferredLighting -Fo deferredPointLighting.ps.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 pointspotlight.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo deferredPointLighting.vs.spv



/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -E CoarseCull -Fo CoarseCull.cs.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E TraditionalCull -Fo TraditionalCull.cs.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E ClearLightIndices -Fo ClearIndices.cs.spv

echo GPU-Driven Optimization Shaders
echo Stage 1: GPU Culling \(opaque/alpha-mask/transparent split + Hi-Z\)
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 gpucull.hlsl -E EncodeDrawBuffer -Fo gpucull.cs.spv

echo Stage 1: Base pass alpha-mask variant \(reads material from SSBO\)
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePassAlphaMask -Fo drawcluster.base.alphamask.ps.spv

echo Stage 2: Shadow cascade culling
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 shadowcull.hlsl -E ShadowCull -Fo shadowcull.cs.spv

echo Stage 2: Shadow depth indirect \(reads material from SSBO for alpha\)
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneShadowDepthIndirect -Fo drawcluster.shadow.indirect.ps.spv

echo Stage 3: Hi-Z pyramid generation
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 hiz.hlsl -fspv-debug=vulkan-with-source -E CopyDepthToHiZ -Fo hiz_copy.cs.spv
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 hiz.hlsl -fspv-debug=vulkan-with-source -E DownsampleHiZ -Fo hiz_downsample.cs.spv

echo Stage 4: Forward pass indirect \(reads material from SSBO\)
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneForwardPSIndirect -Fo drawcluster.forward.indirect.ps.spv


echo compiling drawOccluderVS
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 drawoccluders.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo occluders.vs.spv

echo compiling drawclustershadowVS
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T vs_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVSShadow -Fo drawclusterShadow.vs.spv

echo compiling drawoccludersvs
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -E RenderSceneVS drawoccluders.hlsl -T vs_6_0 -Fo occluders.wireframe.vs.spv

echo compiling drawoccludersps
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -E WireframePS drawoccluders.hlsl -T ps_6_0 -Fo occluders.wireframe.ps.spv


echo compiling sao
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/UnrealEngine/Engine/Source/ThirdParty/ShaderConductor/Build-RelWithDebInfo.x86_64-unknown-linux-gnu/External/DirectXShaderCompiler/bin/dxc -spirv -T cs_6_2 sao.hlsl -fspv-debug=vulkan-with-source -E ScalableAmbientObscurance -Fo sao.cs.spv