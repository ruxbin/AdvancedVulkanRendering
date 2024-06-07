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
	[unroll(2)]
		for (
int z = 0;z < 2; z++)
		{
		[unroll(2)]
        for (
int y = 0;y < 2; y++)
			{
			[unroll(2)]
            for (
int x = 0;x < 2; x++)
				{
float3 cornor_i(x== 0 ? aabb.min.x : aabb.max.x, y== 0 ? aabb.min.y : aabb.max.y, z== 0 ? aabb.min.z: aabb.max.z);
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
    Plane broders[6];
    
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
[[vk::binding(0,1)]] cbuffer Frustum frustum;

[[vk::binding(0,2)]] StructuredBuffer<AAPLMeshChunk> meshChunks;


//TODO: 合批

[numthreads(128, 1, 1)]
void EncodeDrawBuffer(uint3 DTid : SV_DispatchThreadID)
{
    uint chunkIndex = DTid.x;
    
    if (!FrustumCull(frustum, meshChunks[chunkIndex].boundingBox))
    {
        drawParams[chunkIndex] = {
            .indexCount = meshChunks[chunkIndex].indexCount,.instanceCount = 1, .firstIndex = meshChunks[chunkIndex].indexBegin, vertexOffset = 0, firstInstance = 0

        }

    }

}