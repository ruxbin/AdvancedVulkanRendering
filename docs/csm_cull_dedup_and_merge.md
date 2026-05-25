# CSM 剔除优化:cascade 去重 + threadgroup 合并

日期:2026-05-25
影响 shader:`shaders/commonstruct.hlsl`, `shaders/shadowcull.hlsl`(→ `shadowcull.cs.spv`)
影响 C++:无

## 背景

参考 `D:\ModernRenderingWithMetal` (Apple 的 Modern Rendering with Metal 示例) 在 `AAPLCulling.metal` 里对 CSM 剔除做了两项 F: 项目原本没有的优化:

1. **Cascade 间去重**:Apple 的 `encodeChunksWithCullingFiltered` 让一个 chunk 不会被多个 cascade 重复绘制。
2. **Threadgroup 内 draw 合并**:Apple 的 `indexCountFollowingPrevious[]` 把同 material、indexBegin 连续的 chunk 合并成 1 条 `DrawIndexedIndirectCommand`。

F: 项目的 `ShadowCull` (`shaders/shadowcull.hlsl`) 之前是:每个 thread 一个 chunk,内层循环遍历所有 cascade,每个 cascade 单独 frustum 测试,通过则 `InterlockedAdd` 写 1 条 indirect command。结果:

- 一个跨多 cascade 的 chunk 会被画 N 次(没有去重)。
- 每个 chunk 一条 draw(没有合并)。

## 改动

### 1. `commonstruct.hlsl`:新增 `FrustumContains`

```hlsl
bool FrustumContains(Frustum frustum, AAPLBoundingBox3 aabb)
{
    [unroll]
    for (int i = 0; i < 6; i++)
    {
        Plane p = frustum.borders[i];
        float3 nVertex;
        nVertex.x = (p.normal.x >= 0.0f) ? aabb.min.x : aabb.max.x;
        nVertex.y = (p.normal.y >= 0.0f) ? aabb.min.y : aabb.max.y;
        nVertex.z = (p.normal.z >= 0.0f) ? aabb.min.z : aabb.max.z;
        if (dot(nVertex, p.normal) - p.w < 0.0f)
            return false;
    }
    return true;
}
```

N-vertex (negative-vertex) 优化:对每个 plane 直接挑出"对 plane 距离最小的那个 AABB 角",一次点积判定整个 AABB 是否完全在 plane 内侧。6 个 plane 全过 = fully contained。

零额外 buffer,与现有 `IsInside`/`FrustumCull` 共用 `Plane` 约定(normal 指向 frustum 内,d > 0 = inside)。

### 2. `shadowcull.hlsl`:三阶段重写

#### Phase 1 — 可见性 + cascade 去重

内层 cascade 循环按 cascade 从细到粗(0 → N-1)走。一旦某个 cascade 完全包含 chunk → `break`,后续粗 cascade 不再写。

```hlsl
[unroll]
for (uint c = 0; c < SHADOW_CASCADE_COUNT; c++)
{
    if (c >= cascadeCount) break;
    if (FrustumCull(cascadeFrustum[c], chunkAABB))    continue;
    if (chunkRadius < cascadeCullThreshold[c].x)      continue;

    visibleBits |= (1u << (c * 2u + myBucket));

    // Cascade-de-dup: stop at the first cascade that fully covers us.
    if (FrustumContains(cascadeFrustum[c], chunkAABB))
        break;
}
```

**正确性依据**:`deferredlighting.hlsl:38-71` 的 `evaluateCascadeShadows` 是 first-hit-wins —— 像素按 cascade 0 → N-1 顺序找第一个 UV 落在 [0,1] 的 cascade。当 chunk 完全在 cascade k 的 frustum 内,该 chunk 在更粗 cascade (k+1, k+2, ...) 的覆盖区像素必定先命中 cascade k,粗 cascade 上的 caster 永远不会被采样到,所以可以安全省略。

**注意**:这是 **partial-overlap 时仍写多 cascade、only fully-contained 才 break** 的"保守去重"。比 Apple 的 filtered 路径(any-overlap-then-skip)更安全,代价是少一些去重效率。如果场景里大物体多,可以改成 Apple 风格(把 `if (FrustumContains)` 改为无条件 `break`),但要接受 caster 阴影投射跨 cascade 边界时可能漏阴影的 edge case。

#### Phase 2 — 邻居吸收测试(Apple 风格的 indexCountFollowingPrevious)

threadgroup memory:

```hlsl
groupshared uint g_visibleBits   [128];           // (cascade,bucket) → bit
groupshared uint g_indexBegin    [128];
groupshared uint g_indexCount    [128];
groupshared uint g_materialIndex [128];
groupshared uint g_bucket        [128];           // 0 = opaque, 1 = alphaMask
groupshared uint g_absorbed      [128 * 3 * 2];   // per (thread, cascade, bucket)
```

总 threadgroup memory 约 5.5 KB,远低于 Vulkan 实现的下限。

每个 thread 把自己 phase 1 算出的 visibility/字段 publish 到 TG memory,然后在屏障后对每个 (cascade, bucket) 检查"前一个 thread 能不能吃掉我":

```hlsl
bool sameBucket  = (prevBucket == myBucket);
bool contiguous  = (myIndexBegin == prevIndexBegin + prevIndexCount);
bool materialOk  = (myBucket == 0u) || (myMaterialIndex == prevMaterial);
bool baseMerge   = sameBucket && contiguous && materialOk;

if (baseMerge) {
    [unroll]
    for (uint c = 0; c < SHADOW_CASCADE_COUNT; c++) {
        if (c >= cascadeCount) break;
        uint bit = 1u << (c * 2u + myBucket);
        if ((prevVisible & bit) && (visibleBits & bit))
            g_absorbed[tg * 6 + c * 2 + myBucket] = myIndexCount;
    }
}
```

**material 处理**:
- opaque bucket:shadow pass 是 depth-only(`RenderSceneShadowDepthIndirect:380-389` 的 `specAlphaMask=false` 分支是空操作),material 与渲染结果无关 → 放宽 merge 条件。
- alpha-mask bucket:`RenderSceneShadowDepthIndirect` 要用 material 的 `albedo_texture_index` 做 alpha 测试 → 必须 same material 才能 merge。

#### Phase 3 — owner 发射合并 draw

被前一个吸收的 thread 自己跳过;没被吸收的 thread 沿 TG 向后扫,累加被吸收邻居的 `indexCount`,一次 atomic + 一条 indirect command 替代原本 N 条:

```hlsl
uint totalIndexCount = myIndexCount;
for (uint j = tg + 1u; j < endTG; j++) {
    uint extra = g_absorbed[j * 6 + slot];
    if (extra == 0u) break;
    totalIndexCount += extra;
}

InterlockedAdd(shadowWriteIndex[slot], 1, insertIndex);
uint globalIdx = cascadeBase + insertIndex;
DrawIndexedIndirectCommand cmd = { totalIndexCount, 1, myIndexBegin, 0, globalIdx };
shadowDrawParams [globalIdx] = cmd;
shadowChunkIndices[globalIdx] = chunkIndex;
```

`firstInstance=globalIdx` 与原版语义一致,VS 端 `chunkIndices[globalIdx]` 取到合并组的**起始 chunk**。

## 兼容性

| 层 | 改动 |
|----|------|
| C++ (`Shadow.cpp`) | 0 |
| RenderPass / framebuffer | 0 |
| `_shadowDrawParamsBuffers` 布局 | 0 |
| `_shadowWriteIndexBuffers` 布局 | 0 |
| `vkCmdDispatch` 参数 | 0(仍是 `ceil(totalChunks/128)`) |
| `vkCmdDrawIndexedIndirectCount` 参数 | 0 |
| 阴影 VS (`drawclusterShadow.vs.spv`) | 0 |
| 阴影 PS (`drawcluster.shadow.indirect.ps.spv`) | 0 |
| 其他 include `commonstruct.hlsl` 的 shader | 0(只新增函数) |

**优雅退化**:场景里完全没有可合并的连续 chunk 时,phase 2 不触发,phase 3 退化为每 chunk 一条 draw,与原 shader 行为完全等价。

## 验证清单

1. **shader 编译**:`dxc.exe -spirv -T cs_6_2 shadowcull.hlsl -E ShadowCull -Fo shadowcull.cs.spv` 已通过(`shadowcull.cs.spv` 10736 字节)。
2. **视觉回归**:
   - 阴影边缘是否仍正确,特别注意 cascade 0 / cascade 1 交界处的大型物体
   - 跨 cascade 的细长物体(柱子、栏杆)是否漏阴影 → 若漏,去重过于激进(理论上 `FrustumContains` 不应该误判,需 debug)
3. **指标**:RenderDoc 抓帧,看
   - `shadowWriteIndex[i*2+0/1]` 计数:去重生效后远 cascade 计数应该下降
   - shadow render pass 的 `drawIndexedIndirect` 命令数:合并生效后应该减少
   - shadow pass GPU time 下降
4. **如果只想验证去重**:把 phase 2/3 的 merge 逻辑短路(让 absorbed 永远是 0,phase 3 totalIndexCount=myIndexCount),保留 phase 1 的 `break`。
5. **如果只想验证合并**:把 phase 1 的 `if (FrustumContains)` 那行删掉(关闭去重)。

## 与 Apple 版本的对照

| 维度 | Apple `encodeChunksWithCulling[Filtered]` | 本次 F: 实现 |
|------|------------------------------------------|--------------|
| Dispatch 结构 | per-cascade 独立 dispatch | 单 dispatch,内层循环 cascade |
| Cascade 去重 | filtered kernel 用第二个相机做 diff,加 vertex amplification | 单 dispatch 内 fully-contained 时 break |
| Threadgroup 合并 | `indexCountFollowingPrevious[]` per-cascade | per-(cascade,bucket) `g_absorbed[]` |
| HiZ 遮挡剔除 | ✅ 可选,每 cascade 自己的 depth pyramid | ❌ 仍未做(见 [`后续`](#后续可选优化)) |
| ICB combineDraws 后处理 | ✅ | ❌(Vulkan 走 `DrawIndexedIndirectCount`,无对应 API) |
| 几何体测试单位 | bounding sphere | bounding AABB(沿用原项目约定) |
| `cascadeCullThreshold` 小物体剔除 | ❌ | ✅(原项目独有) |

## 后续可选优化

- **Shadow 路径加 HiZ**:把 `gpucull.hlsl` 的 HiZ 测试搬到 shadow cull 里,每个 cascade 用自己 shadow map 上一帧的 depth pyramid。前提是 shadow pass 写得起 depth pyramid 的代价(近 cascade 通常划算,远 cascade 可能不)。
- **激进去重**:如果观察发现保守策略带来的"重复绘制"仍是热点,把 `if (FrustumContains)` 改成无条件 `break`,接受 caster 阴影跨 cascade 边界的 edge case。
- **跨 TG 合并**:目前合并只在 128 个 chunk 的 TG 内做。如果原始 chunk 排序保证大批连续(比如同一 mesh 内的所有 chunk 物理相邻),可以加一个二次 pass 跨 TG 合并。需要先 profile,看是不是热点。
