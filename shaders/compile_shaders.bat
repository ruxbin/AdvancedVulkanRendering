G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T vs_6_0 edward.hlsl -E RenderSceneVS -Fo edward.vs.spv


D:\VulkanSDK\1.3.275.0\Bin\glslc.exe shader.frag -o frag.spv


G:\dxc_cmake\RelWithDebInfo\bin\dxc.exe -spirv -T vs_6_0 drawcluster.hlsl -fspv-debug=vulkan-with-source -E RenderSceneVS -Fo drawcluster.vs.spv