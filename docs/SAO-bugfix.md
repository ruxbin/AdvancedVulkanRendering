# SAO 效果不对:Metal port 时矩阵索引未转置 + 多余的 Y 翻转

## 现象

Scalable Ambient Obscurance(SAO)出来的 AO 几乎看不到效果——边缘和角落本该暗下去但没暗,或者只在贴边一小条出来一丁点。

涉及文件:
- `shaders/sao.hlsl`(SAO compute,从 `AAPLAmbientObscurance.metal` port 过来)

参考:
- `shaders/commonstruct.hlsl:107-124` `worldPositionForTexcoord`(deferred 路径里反向重建 world pos,工作正常,作为对照基准)
- `Src/Include/Matrix.h` `reverseZperspective`

---

## 根本原因

### Bug 1:`invProj` 索引位置没从 Metal 风格转换到 HLSL 风格

`shaders/sao.hlsl:47, 79`:

```hlsl
// BEFORE
depth * invProj[2][3] + invProj[3][3]
```

按 HLSL/SPIR-V 列向量约定,`invProj[i][j]` 是 row i, col j。

reverse-Z perspective 的逆矩阵(列向量形式):

```
invP = [[1/a,   0,    0,    0  ];
        [ 0,  1/b,    0,    0  ];
        [ 0,    0,    0,    1  ];     ← row 2: invP[2][3] = 1
        [ 0,    0,  1/d,  -c/d]]      ← row 3: invP[3][2] = 1/d, [3][3] = -c/d
```

depth 系数应该是 `invP[3][2] = 1/d`,代码用了 `invP[2][3] = 1`,**差一个 `d` 因子**。

对 `n = 0.1, f = 100` 这种典型设置,`d = fn/(f-n) ≈ 0.1`。

代码算出来的 `view.w = depth - c/d`,正确应该是 `(depth - c)/d`。后续 `view.z = 1/view.w` 错了 `~10×`,因此:

```hlsl
const float radius = 0.5 * H * projectionMatrix[1][1];     // focal length in pixels
float discSize = radius / cameraPosition.z;                 // ← .z 大 10× → discSize 小 10×
```

→ AO 采样圈缩到原来的 ~10 分之一,半径几个像素,效果几乎看不见。

来源是 Metal 的 `M[i][j]` = `column[i].component[j]`(等价 HLSL 的 `[j][i]`),port 时 4 个用到 invProj 的索引位置应该一起转置,但漏了。

### Bug 2:多余的 `ndc.y *= -1.0`

`shaders/sao.hlsl:40, 60` 和 `GetOffsetCameraSpacePosition` 里隐含的 `float2(2.0, -2.0)`(line 73)。

参照 `commonstruct.hlsl:117` 的 `worldPositionForTexcoord`:

```hlsl
ndc.xy = texCoord.xy * 2 - 1;
//ndc.y *= -1;       // 注释明确说明:不翻
```

deferredlighting 用这个反向重建 world pos,shadow 计算正常 → 该项目的深度纹理采样约定就是 `ndc = uv*2 - 1`,**不需要翻 Y**。SAO 多翻一次,跟整个项目其他路径不一致。

后果:
- `GetCameraSpacePositionFromDepth` 算出的 view.y 方向反了
- 4 邻域 normal 重建的 `cross(dx, dy)` 整体翻一面
- AO sampling 的 `vn = dot(v, normal)` 全部反号
- `max(vn - bias, 0)` 几乎全吃成 0 → AO factor 趋近 1.0(等于没 AO)

注意 SAO 内部所有用 ndc/view 的代码都 self-consistent 地翻一次,所以**点位本身**是 round-trip 一致的,但 **normal 方向不是**——cross product 的 sign 直接受 Y direction 影响,而 AO 强烈依赖 normal sign。

---

## 修法

`shaders/sao.hlsl` 三个函数,共 6 处改动:

1. **`GetCameraSpacePositionFromDepth` (line 35-52)**
   - 删掉 `ndc.y *= -1.0;`
   - `invProj[3][0]` → `invProj[0][3]`(都是 0,清理一致性)
   - `invProj[3][1]` → `invProj[1][3]`(都是 0,清理一致性)
   - `invProj[2][3]` → `invProj[3][2]` ★ 关键修复

2. **`GetCameraSpaceBasePosition` (line 55-67)**
   - 删掉 `ndc.y *= -1.0;`
   - `invProj[3][0]` / `invProj[3][1]` → `invProj[0][3]` / `invProj[1][3]`

3. **`GetOffsetCameraSpacePosition` (line 70-84)**
   - `float2(2.0, -2.0)` → `float2(2.0, 2.0)`
   - `invProj[2][3]` → `invProj[3][2]` ★ 关键修复

重编译:

```bash
dxc -spirv -T cs_6_2 sao.hlsl -E ScalableAmbientObscurance -Fo sao.cs.spv
```

---

## 调试线索/对照基准

`worldPositionForTexcoord`(deferred 路径用)用的是**完整 4×4 矩阵 mul**:

```hlsl
mul(cameraParams.invViewProjectionMatrix, ndc)
```

完整 mul 不依赖单个元素的 layout 假设——16 个元素都参与运算,自然就对。SAO 为了省指令把矩阵展开成几个对角元素 + 第 3 列/行,展开时索引位置要对应 HLSL 的 `[row][col]` 约定,这就是 port 时容易出错的地方。

下次再有这种"从 Metal/D3D 行向量 port 到 HLSL/SPIR-V 列向量"的优化代码,**优先用完整 mul 验证一遍数值,再做手动展开**。

---

## 文件改动总览

```
shaders/sao.hlsl
  - 3 处 ndc.y *= -1.0 / (2.0, -2.0) 全删
  - 2 处 invProj[2][3] → invProj[3][2](depth 系数,关键)
  - 4 处 invProj[3][0]/[3][1] → invProj[0][3]/[1][3](都是 0,顺手对齐)
```
