D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T vs_6_0 edward.hlsl -E RenderSceneVS -Fo edward.vs.spv



D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T vs_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo drawcluster.vs.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneDepthOnly -Fo drawcluster.depth.ps.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePS -Fo drawcluster.ps.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePass -Fo drawcluster.base.ps.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneForwardPS -Fo drawcluster.forward.ps.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T vs_6_0 deferredlighting.hlsl -fspv-debug=vulkan-with-source -E AAPLSimpleTexVertexOutFSQuadVertexShader -Fo deferredlighting.vs.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T ps_6_0 deferredlighting.hlsl -fspv-debug=vulkan-with-source -E DeferredLighting -Fo deferredlighting.ps.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T ps_6_0 pointspotlight.hlsl -fspv-debug=vulkan-with-source -E DeferredLighting -Fo deferredPointLighting.ps.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T vs_6_0 pointspotlight.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo deferredPointLighting.vs.spv



D:\dxc_2025_07_14\bin\x64\dxc.exe -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E CoarseCull -Fo CoarseCull.cs.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E TraditionalCull -Fo TraditionalCull.cs.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -enable-16bit-types -spirv -T cs_6_2 lightculling.hlsl -fspv-debug=vulkan-with-source -E ClearLightIndices -Fo ClearIndices.cs.spv

REM GPU-Driven Optimization Shaders
REM Stage 1: GPU Culling (opaque/alpha-mask/transparent split + Hi-Z)
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T cs_6_2 gpucull.hlsl -fspv-debug=vulkan-with-source -E EncodeDrawBuffer -Fo gpucull.cs.spv

REM Stage 1: Base pass alpha-mask variant (reads material from SSBO)
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePassAlphaMask -Fo drawcluster.base.alphamask.ps.spv

REM Stage 2: Shadow cascade culling
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T cs_6_2 shadowcull.hlsl -fspv-debug=vulkan-with-source -E ShadowCull -Fo shadowcull.cs.spv

REM Stage 2: Shadow depth indirect (reads material from SSBO for alpha)
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneShadowDepthIndirect -Fo drawcluster.shadow.indirect.ps.spv

REM Stage 3: Hi-Z pyramid generation
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T cs_6_2 hiz.hlsl -fspv-debug=vulkan-with-source -E CopyDepthToHiZ -Fo hiz_copy.cs.spv
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T cs_6_2 hiz.hlsl -fspv-debug=vulkan-with-source -E DownsampleHiZ -Fo hiz_downsample.cs.spv

REM Stage 4: Forward pass indirect (reads material from SSBO)
D:\dxc_2025_07_14\bin\x64\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneForwardPSIndirect -Fo drawcluster.forward.indirect.ps.spv