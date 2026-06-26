@echo off
REM DX12 Shader Compilation Script - HLSL to DXIL (.cso)
REM Uses DXC from Vulkan SDK (also supports DXIL output)

set DXC=D:\VulkanSDK\1.3.296.0\Bin\dxc.exe
set DXFLAGS=-D DX12_BACKEND

REM === DrawCluster (vertex + pixel variants) ===
%DXC% %DXFLAGS% -T vs_6_0 drawcluster.hlsl -E RenderSceneVS -Fo drawcluster.vs.cso
%DXC% %DXFLAGS% -T ps_6_0 drawcluster.hlsl -E RenderSceneBasePS -Fo drawcluster.ps.cso
%DXC% %DXFLAGS% -T ps_6_0 drawcluster.hlsl -E RenderSceneDepthOnly -Fo drawcluster.depth.ps.cso
%DXC% %DXFLAGS% -T ps_6_0 drawcluster.hlsl -E RenderSceneBasePass -Fo drawcluster.base.ps.cso
%DXC% %DXFLAGS% -T ps_6_0 drawcluster.hlsl -E RenderSceneForwardPS -Fo drawcluster.forward.ps.cso
%DXC% %DXFLAGS% -T ps_6_0 drawcluster.hlsl -E RenderSceneBasePassAlphaMask -Fo drawcluster.base.alphamask.ps.cso
%DXC% %DXFLAGS% -T ps_6_0 drawcluster.hlsl -E RenderSceneShadowDepthIndirect -Fo drawcluster.shadow.indirect.ps.cso
%DXC% %DXFLAGS% -T ps_6_0 drawcluster.hlsl -E RenderSceneForwardPSIndirect -Fo drawcluster.forward.indirect.ps.cso
%DXC% %DXFLAGS% -T vs_6_0 drawcluster.hlsl -E RenderSceneVSShadow -Fo drawclusterShadow.vs.cso

REM === Deferred Lighting ===
%DXC% %DXFLAGS% -T vs_6_0 deferredlighting.hlsl -E AAPLSimpleTexVertexOutFSQuadVertexShader -Fo deferredlighting.vs.cso
%DXC% %DXFLAGS% -T ps_6_0 deferredlighting.hlsl -E DeferredLighting -Fo deferredlighting.ps.cso

REM === Point/Spot Lights ===
%DXC% %DXFLAGS% -T vs_6_0 pointspotlight.hlsl -E RenderSceneVS -Fo deferredPointLighting.vs.cso
%DXC% %DXFLAGS% -T ps_6_0 pointspotlight.hlsl -E DeferredLighting -Fo deferredPointLighting.ps.cso

REM === Light Culling ===
%DXC% %DXFLAGS% -enable-16bit-types -T cs_6_2 lightculling.hlsl -E CoarseCull -Fo CoarseCull.cs.cso
%DXC% %DXFLAGS% -enable-16bit-types -T cs_6_2 lightculling.hlsl -E TraditionalCull -Fo TraditionalCull.cs.cso
%DXC% %DXFLAGS% -enable-16bit-types -T cs_6_2 lightculling.hlsl -E ClearLightIndices -Fo ClearIndices.cs.cso

REM === GPU Culling ===
%DXC% %DXFLAGS% -T cs_6_2 gpucull.hlsl -E EncodeDrawBuffer -Fo gpucull.cs.cso

REM === Shadow Culling ===
%DXC% %DXFLAGS% -T cs_6_2 shadowcull.hlsl -E ShadowCull -Fo shadowcull.cs.cso

REM === Hi-Z Pyramid ===
%DXC% %DXFLAGS% -T cs_6_2 hiz.hlsl -E CopyDepthToHiZ -Fo hiz_copy.cs.cso
%DXC% %DXFLAGS% -T cs_6_2 hiz.hlsl -E DownsampleHiZ -Fo hiz_downsample.cs.cso

REM === SAO ===
%DXC% %DXFLAGS% -T cs_6_2 sao.hlsl -E ScalableAmbientObscurance -Fo sao.cs.cso

REM === Occluders ===
%DXC% %DXFLAGS% -T vs_6_0 drawoccluders.hlsl -E RenderSceneVS -Fo occluders.vs.cso
%DXC% %DXFLAGS% -T ps_6_0 drawoccluders.hlsl -E WireframePS -Fo occluders.wireframe.ps.cso
%DXC% %DXFLAGS% -T vs_6_0 drawoccluders.hlsl -E RenderSceneVS -Fo occluders.wireframe.vs.cso

echo All DX12 shaders compiled.
