G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T vs_6_0 edward.hlsl -E RenderSceneVS -Fo edward.vs.spv


D:\VulkanSDK\1.3.275.0\Bin\glslc.exe shader.frag -o frag.spv


G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T vs_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo drawcluster.vs.spv
G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneDepthOnly -Fo drawcluster.depth.ps.spv
G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePS -Fo drawcluster.ps.spv
G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneBasePass -Fo drawcluster.base.ps.spv
G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T ps_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneForwardPS -Fo drawcluster.forward.ps.spv
G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T vs_6_0 deferredlighting.hlsl -fspv-debug=vulkan-with-source -E AAPLSimpleTexVertexOutFSQuadVertexShader -Fo deferredlighting.vs.spv
G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T ps_6_0 deferredlighting.hlsl -fspv-debug=vulkan-with-source -E DeferredLighting -Fo deferredlighting.ps.spv
G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T ps_6_0 pointspotlight.hlsl -fspv-debug=vulkan-with-source -E DeferredLighting -Fo deferredPointLighting.ps.spv
G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T vs_6_0 pointspotlight.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo deferredPointLighting.vs.spv