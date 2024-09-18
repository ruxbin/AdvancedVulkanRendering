#
#
#
#TraditionalCull
echo compiling TraditionalCull
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/CPP/ShaderConductor/Build/ninja-linux-gcc-x64-Debug/Bin/ShaderConductorCmd -E TraditionalCull -I lightculling.hlsl -S cs -T spirv -O TraditionalCull.cs.spv
echo compiling CoarseCull
#CoarseCull
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/CPP/ShaderConductor/Build/ninja-linux-gcc-x64-Debug/Bin/ShaderConductorCmd -E CoarseCull -I lightculling.hlsl -S cs -T spirv -O CoarseCull.cs.spv

echo compiling ClearIndices
#ClearLightIndices
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/CPP/ShaderConductor/Build/ninja-linux-gcc-x64-Debug/Bin/ShaderConductorCmd -E ClearLightIndices -I lightculling.hlsl -S cs -T spirv -O ClearIndices.cs.spv
echo compiling ClearDebugView
#ClearDebugView
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/CPP/ShaderConductor/Build/ninja-linux-gcc-x64-Debug/Bin/ShaderConductorCmd -E ClearDebugView -I lightculling.hlsl -S cs -T spirv -O ClearDebugView.cs.spv
echo compiling deferredPointLightingVS
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/CPP/ShaderConductor/Build/ninja-linux-gcc-x64-Debug/Bin/ShaderConductorCmd -E RenderSceneVS -I pointspotlight.hlsl -S vs -T spirv -O deferredPointLighting.vs.spv
echo compiling deferredPointLightingPS
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/CPP/ShaderConductor/Build/ninja-linux-gcc-x64-Debug/Bin/ShaderConductorCmd -E DeferredLighting -I pointspotlight.hlsl -S ps -T spirv -O deferredPointLighting.ps.spv


echo compiling deferredLightingPS
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/CPP/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/Bin/ShaderConductorCmd -E DeferredLighting -I deferredlighting.hlsl -S ps -T spirv -O deferredlighting.ps.spv

echo compiling deferredlightingVS
/run/media/ruxbin/8219f399-1a49-4a9e-af5c-69af4d51da2a/CPP/ShaderConductor/Build/ninja-linux-gcc-x64-RelWithDebInfo/Bin/ShaderConductorCmd -E AAPLSimpleTexVertexOutFSQuadVertexShader -I deferredlighting.hlsl -S vs -T spirv -O deferredlighting.vs.spv



