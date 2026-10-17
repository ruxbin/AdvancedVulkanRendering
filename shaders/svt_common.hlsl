// svt_common.hlsl - Streaming Virtual Texture 共享采样函数（设计 §8.2）。
//
// 本文件只包含数据结构与采样逻辑，不声明 binding、不决定磁盘布局。
// 宿主 shader 通过参数传入资源（同 VlmIndirect 的传参方式）。
// 绑定布局见 drawcluster.hlsl 的 SVT_ENABLED 段（set2）。
//
// 语义约定（设计文档 2026-10-16 §8.2）：
//  - 导数必须在任何非一致分支之前取得，由调用方传入 du/dv。
//  - 每个候选 mip 的查找都使用 Repeat 后的 UV；导数基于原始 UV。
//  - 页表只存精确页；缺页时从 UV 重算父 mip 页坐标继续，直到 tail。
//  - texel center 对应 i+0.5（归一化坐标约定），physicalUV 不加半 texel。
//  - 首版双线性 + 手动三线性，不做 anisotropy。

#ifndef SVT_COMMON_HLSL
#define SVT_COMMON_HLSL

// ---------------------------------------------------------------------------
// 常量（与 C++ 侧 SvtGpuConstants 保持一致）
// ---------------------------------------------------------------------------

#ifndef SVT_MAX_POOLS
#define SVT_MAX_POOLS 8
#endif

#ifndef SVT_MAX_TAILS
#define SVT_MAX_TAILS 1024
#endif

// 每 asset 页表偏移数组的固定容量。
// T >= 16 且 dim <= 16384 时 tailFirstMip <= 10，16 留有余量。
#ifndef SVT_MAX_TILED_MIPS
#define SVT_MAX_TILED_MIPS 16
#endif

#define SVT_INVALID_INDEX 0xffffffffu

// 页表项编码（设计 §7.1）：bit31 valid，bits[19:0] slot index。
#define SVT_PAGE_VALID_BIT 0x80000000u
#define SVT_PAGE_SLOT_MASK 0x000fffffu

// ---------------------------------------------------------------------------
// GPU 数据结构（C++ 侧逐字段镜像，见 SvtVulkanRuntime.h）
// ---------------------------------------------------------------------------

// 每格式池的几何（pool key = {format, T, B}，设计 §7.2）。
struct SvtPoolGpuDesc
{
    uint atlasWidth;   // = slotsPerRow * P
    uint atlasHeight;  // = rows * P
    uint slotsPerRow;
    uint slotCount;
    uint paddedTile;   // P = T + 2B
    uint tileSize;     // T（内部 texel）
    uint border;       // B（每侧 texel）
    uint _pad;
};

// 每资产虚拟地址描述（设计 §7.1、§8.1）。
struct SvtAssetGpuDesc
{
    uint width;           // texels
    uint height;
    uint mipCount;        // 完整链层数（含 tail）
    uint tailFirstMip;    // 第一个宽高均 <= T 的 mip；页表只覆盖 [0, tailFirstMip)
    uint pageBase;        // 全局扁平页表中的起始项
    uint poolIndex;       // -> svtPools / svtAtlases
    uint tailTextureIndex;// -> svtTails；每个注册资产必有 tail
    uint flags;           // bit0: sRGB
    uint mipPageOffsets[SVT_MAX_TILED_MIPS]; // 资产页表内每 mip 的起始偏移
};

// 材质 side table（设计 §8.1）：四个槽各自独立选择 SVT 或旧路径。
// 槽序与 AAPLShaderMaterial 的 *_texture_index 一致：0=albedo 1=roughness 2=normal 3=emissive。
struct SvtMaterialBinding
{
    uint4 assets; // 每槽一个 assetIndex；SVT_INVALID_INDEX 表示走旧 _Textures 路径
};

// ---------------------------------------------------------------------------
// 地址数学（与 CPU 侧 svt::svtMipDim/svtTilesPerAxis 语义一致）
// ---------------------------------------------------------------------------

uint svtMipDim(uint dim, uint mip)
{
    return max(dim >> mip, 1u);
}

uint svtTilesPerAxis(uint dim, uint tileSize)
{
    return (dim + tileSize - 1) / tileSize;
}

// ---------------------------------------------------------------------------
// 单 mip 采样：页表查找 + 父 mip 回退 + tail 回退（设计 §8.2）
// ---------------------------------------------------------------------------

// 采样虚拟 mip（精确驻留则查 atlas；缺页则逐级走向父 mip；越过 tailFirstMip 则查 tail）。
// mip 必须先 clamp 到 [0, mipCount-1]。回退到 tail 时 LOD 自然 clamp 到 0
// （循环落在 tailFirstMip 时 lod==0，与 §8.2 “从更细 mip 缺页回退到 tail 时 LOD clamp 至 0”一致）。
half4 SvtSampleMip(
    float2 uv,                                  // 原始 Repeat UV（内部做 frac）
    uint mip,                                   // 请求的虚拟 mip
    SvtAssetGpuDesc asset,
    SvtPoolGpuDesc pool,
    StructuredBuffer<uint> pageTable,
    Texture2D<half4> atlases[SVT_MAX_POOLS],
    Texture2D<half4> tails[SVT_MAX_TAILS],
    SamplerState atlasSampler,
    SamplerState tailSampler)
{
    // HLSL 无递归：有界循环沿父 mip 链回退，最坏 mipCount 级。
    uint m = mip;
    [loop]
    for (int guard = 0; guard < 32; ++guard)
    {
        if (m >= asset.tailFirstMip)
        {
            if (asset.tailTextureIndex == SVT_INVALID_INDEX)
                return half4(0, 0, 0, 1);
            float lod = float(m) - float(asset.tailFirstMip);
            return tails[NonUniformResourceIndex(asset.tailTextureIndex)]
                .SampleLevel(tailSampler, uv, lod);
        }

        // 每个候选 mip 都使用 Repeat 后的 UV（§8.2）。
        float2 wuv = frac(uv);
        uint dimW = svtMipDim(asset.width, m);
        uint dimH = svtMipDim(asset.height, m);
        float2 texel = wuv * float2(dimW, dimH);

        uint tilesX = svtTilesPerAxis(dimW, pool.tileSize);
        uint tilesY = svtTilesPerAxis(dimH, pool.tileSize);
        // POT 尺寸下 frac(uv) < 1 保证 texel < dim；min 仅作浮点防御。
        uint pageX = min((uint)(texel.x / pool.tileSize), tilesX - 1);
        uint pageY = min((uint)(texel.y / pool.tileSize), tilesY - 1);

        uint entry = pageTable[asset.pageBase + asset.mipPageOffsets[m]
                               + pageY * tilesX + pageX];
        if (entry & SVT_PAGE_VALID_BIT)
        {
            uint slot = entry & SVT_PAGE_SLOT_MASK;
            float2 slotXY = float2(slot % pool.slotsPerRow,
                                   slot / pool.slotsPerRow);
            // local 为连续坐标（不加半 texel，texel center 约定见文件头）。
            float2 local = texel - float2(pageX, pageY) * pool.tileSize;
            float2 physical = slotXY * pool.paddedTile + pool.border + local;
            float2 atlasUV = physical / float2(pool.atlasWidth, pool.atlasHeight);
            return atlases[NonUniformResourceIndex(asset.poolIndex)]
                .SampleLevel(atlasSampler, atlasUV, 0);
        }
        ++m; // 缺页：重算父 mip 页坐标（不能沿用当前 mip 的 frac 坐标）
    }
    return half4(0, 0, 0, 1);
}

// ---------------------------------------------------------------------------
// 完整采样：导数 LOD + 手动三线性（设计 §8.2）
// ---------------------------------------------------------------------------

// du/dv 必须由调用方在非一致分支之外计算后传入：
//   float2 du = ddx(uv); float2 dv = ddy(uv);
half4 SvtSampleTexture(
    float2 uv,
    float2 du,                                  // ddx(uv)
    float2 dv,                                  // ddy(uv)
    uint assetIndex,
    float mipBias,
    StructuredBuffer<SvtAssetGpuDesc> svtAssets,
    StructuredBuffer<SvtPoolGpuDesc> svtPools,
    StructuredBuffer<uint> svtPageTable,
    Texture2D<half4> atlases[SVT_MAX_POOLS],
    Texture2D<half4> tails[SVT_MAX_TAILS],
    SamplerState atlasSampler,
    SamplerState tailSampler)
{
    SvtAssetGpuDesc asset = svtAssets[NonUniformResourceIndex(assetIndex)];
    SvtPoolGpuDesc pool = svtPools[NonUniformResourceIndex(asset.poolIndex)];

    // 导数在 frac 之前、基于全尺寸计算（§8.2）。
    float2 duTex = du * float2(asset.width, asset.height);
    float2 dvTex = dv * float2(asset.width, asset.height);
    float maxLen = max(max(length(duTex), length(dvTex)), 1e-6);
    float lambda = clamp(log2(maxLen) + mipBias, 0.0, float(asset.mipCount - 1));

    uint m0 = (uint)floor(lambda);
    float fracL = lambda - float(m0);
    uint m1 = min(m0 + 1, asset.mipCount - 1);

    half4 c0 = SvtSampleMip(uv, m0, asset, pool, svtPageTable, atlases, tails,
                            atlasSampler, tailSampler);
    if (m1 == m0)
        return c0;
    half4 c1 = SvtSampleMip(uv, m1, asset, pool, svtPageTable, atlases, tails,
                            atlasSampler, tailSampler);
    return lerp(c0, c1, (half)fracL);
}

// Optional base-color diagnostics. Uses the same exact-page/parent lookup.
float3 SvtDebugColor(float2 uv, float2 du, float2 dv, uint assetIndex,
    float bias, uint mode, StructuredBuffer<SvtAssetGpuDesc> assets,
    StructuredBuffer<SvtPoolGpuDesc> pools, StructuredBuffer<uint> pages)
{
    SvtAssetGpuDesc a = assets[assetIndex];
    SvtPoolGpuDesc p = pools[a.poolIndex];
    float rho = max(length(du * float2(a.width,a.height)), length(dv * float2(a.width,a.height)));
    uint requested = (uint)floor(clamp(log2(max(rho,1e-6))+bias,0.0,float(a.mipCount-1)));
    uint actual = requested;
    uint slot = SVT_INVALID_INDEX;
    [loop] for (; actual < a.tailFirstMip; ++actual) {
        uint2 dims = uint2(svtMipDim(a.width,actual),svtMipDim(a.height,actual));
        uint2 tile = min(uint2(frac(uv)*dims)/p.tileSize, (dims+p.tileSize-1)/p.tileSize-1);
        uint entry = pages[a.pageBase+a.mipPageOffsets[actual]+tile.y*svtTilesPerAxis(dims.x,p.tileSize)+tile.x];
        if (entry & SVT_PAGE_VALID_BIT) { slot=entry & SVT_PAGE_SLOT_MASK; break; }
    }
    float value = mode == 1 ? requested : (mode == 5 ? actual-requested : actual);
    float3 color = 0.3 + 0.7 * frac((value+1)*float3(0.618,0.381,0.173));
    if (mode == 4) color = slot == SVT_INVALID_INDEX ? float3(1,0,1)
        : 0.15+0.85*frac((slot+1)*float3(0.618,0.381,0.173));
    if (mode == 3) {
        float2 local = frac(frac(uv)*float2(svtMipDim(a.width,requested),svtMipDim(a.height,requested))/p.tileSize);
        float2 edge = min(local,1-local)*p.tileSize;
        color = min(edge.x,edge.y) < 1.5 ? float3(0,0,0) : color;
    }
    return color;
}

// ---------------------------------------------------------------------------
// 反馈编码与写入（设计 §8.3）
// ---------------------------------------------------------------------------

// word0: assetIndex[15:0] | generation16[31:16]
// word1: pageX[11:0] | pageY[23:12] | mip[27:24] | reserved=0[31:28]
uint2 svtEncodeFeedback(uint32_t assetIndex, uint32_t generation16,
                        uint32_t pageX, uint32_t pageY, uint32_t mip)
{
    uint2 w;
    w.x = (assetIndex & 0xFFFFu) | ((generation16 & 0xFFFFu) << 16);
    w.y = (pageX & 0xFFFu) | ((pageY & 0xFFFu) << 12) | ((mip & 0xFu) << 24);
    return w;
}

// 反馈 buffer 头部布局（16 字节，设计 §8.3）：
//   [0] attemptCount（uint，原子递增；溢出检测 = attemptCount - capacity）
//   [1] overflowCount（uint；CPU 消费时由 min(attemptCount,capacity) 推算，
//       GPU 不写，保留为 0）
//   [2][3] 保留
#define SVT_FB_HEADER_U32 4

// 单槽反馈写入（命中也反馈，设计 §8.3）。请求 mip 为 shader 的期望 mip
//（m0/m1 两级都各发一条）。tail 范围不发（asset 侧判断）。
// feedbackBuffer：feedback buffer 的 UAV 视图（header + uint2 数组连续）。
void svtAppendFeedback(
    RWStructuredBuffer<uint> feedbackBuffer,
    uint32_t feedbackCapacity,       // uint2 条目容量（不含 header）
    uint32_t assetIndex,
    uint32_t generation16,
    SvtAssetGpuDesc asset,
    SvtPoolGpuDesc pool,
    uint32_t mip,                    // 期望 mip（已 clamp 到 mipCount-1）
    float2 uv)                       // Repeat UV（页坐标用 frac 后计算）
{
    if (assetIndex == SVT_INVALID_INDEX)
        return;
    if (mip >= asset.tailFirstMip)
        return; // tail 常驻，无需请求（设计 §8.3）

    float2 wuv = frac(uv);
    uint32_t dimW = svtMipDim(asset.width, mip);
    uint32_t dimH = svtMipDim(asset.height, mip);
    uint32_t tilesX = svtTilesPerAxis(dimW, pool.tileSize);
    uint32_t tilesY = svtTilesPerAxis(dimH, pool.tileSize);
    float2 texel = wuv * float2(dimW, dimH);
    uint32_t pageX = min((uint)(texel.x / pool.tileSize), tilesX - 1);
    uint32_t pageY = min((uint)(texel.y / pool.tileSize), tilesY - 1);

    uint2 encoded = svtEncodeFeedback(assetIndex, generation16, pageX, pageY, mip);

    uint slot = 0;
    InterlockedAdd(feedbackBuffer[0], 1u, slot);
    if (slot < feedbackCapacity)
    {
        feedbackBuffer[SVT_FB_HEADER_U32 + slot * 2 + 0] = encoded.x;
        feedbackBuffer[SVT_FB_HEADER_U32 + slot * 2 + 1] = encoded.y;
    }
    // 溢出：仅丢弃（attemptCount 继续 ML 计数；CPU 侧推算 overflow，
    // 设计 §8.3 "满时截断，帧统计显示 overflow"）。
}

// 采样点选择：每 (factor x factor) 屏幕块选 1 个位置，随帧变化（设计 §8.3）。
// pixelPos：SV_Position；返回本像素是否被选中。
bool svtFeedbackSamplePoint(uint2 pixelPos, uint32_t factor, uint32_t frameIndex)
{
    if (factor <= 1)
        return true;
    // 块内偏移随帧轮转（确定性 hash(frame) -> [0, factor)）。
    uint32_t h = frameIndex * 2654435761u;
    uint32_t ox = h % factor;
    uint32_t oy = (h / factor) % factor;
    return (pixelPos.x % factor == ox) && (pixelPos.y % factor == oy);
}

#endif // SVT_COMMON_HLSL
