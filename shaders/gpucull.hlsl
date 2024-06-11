#include "commonstruct.hlsl"

/*
hlsl 不支持成员函数，不支持reference parameter，语法层面比metal差
*/
struct Plane
{
    //float nx, ny, nz, w;
    float3 normal;
	float w;
};


bool IsInside(Plane p, AAPLBoundingBox3 aabb)
	{
		
float maxD = -1e9; // , minD = std::numeric_limits<float>::max;
	//unroll掉，不需要loop带来的branch--不太确定编译器是否会自动处理，x,y,z每个值的循环次数是固定的
		[unroll]
		for (int z_z = 0;z_z < 2; z_z++)
		{
			[unroll]
        		for (int y_y = 0;y_y < 2; y_y++)
			{
				[unroll]
            			for (int x_x = 0;x_x < 2; x_x++)
				{
					float3 cornor_i=float3(x_x== 0 ? aabb.min.x : aabb.max.x, y_y== 0 ? aabb.min.y : aabb.max.y, z_z== 0 ? aabb.min.z: aabb.max.z);
                			float d1 = dot(cornor_i,p.normal) - p.w;
					if (d1 > maxD)
						maxD = d1;
					//if (d1 < minD)
					//	minD = d1;
				}
			}
		}
		return maxD > 0;
	}

struct Frustum
{
    Plane borders[6];
    
};

bool FrustumCull(Frustum frustum,  AAPLBoundingBox3 aabb)
{
	[unroll]
    for (int i = 0; i < 6; i++)
    {
        if (!IsInside(frustum.borders[i],aabb))
            return true;
    }
    return false;
}


[[vk::binding(0,0)]] RWStructuredBuffer<DrawIndexedIndirectCommand> drawParams;
[[vk::binding(1,0)]] cbuffer cullParams {
uint totalChunks;
Frustum frustum;
}

[[vk::binding(2,0)]] StructuredBuffer<AAPLMeshChunk> meshChunks;
[[vk::binding(3,0)]] RWStructuredBuffer<uint> writeIndex;
[[vk::binding(4,0)]] RWStructuredBuffer<uint> chunkIndices;

groupshared uint visible[128];
//TODO: 合批

[numthreads(128, 1, 1)]
void EncodeDrawBuffer(uint3 DTid : SV_DispatchThreadID,uint3 GTid:SV_GroupThreadID)
{
    uint chunkIndex = DTid.x;
    uint groupThreadIndex = GTid.x;
   visible[groupThreadIndex] = 0;
   if(chunkIndex<totalChunks)
   {
    if (!FrustumCull(frustum, meshChunks[chunkIndex].boundingBox))
    {
    visible[groupThreadIndex]=1;
    uint insertIndex = 0;
	InterlockedAdd(writeIndex[0],1,insertIndex);
	DrawIndexedIndirectCommand drawParam = {meshChunks[chunkIndex].indexCount,1,meshChunks[chunkIndex].indexBegin,0,0};
        drawParams[insertIndex] = drawParam;
	chunkIndices[insertIndex] = chunkIndex;
	//{
            //.indexCount = meshChunks[chunkIndex].indexCount,.instanceCount = 1, .firstIndex = meshChunks[chunkIndex].indexBegin, vertexOffset = 0, firstInstance = 0

        //}

    }
	}
}
