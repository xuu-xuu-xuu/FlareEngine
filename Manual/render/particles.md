# 确定性粒子模拟与 GPU 广告牌绘制

这篇文档说明粒子效果如何在 CPU 上以固定随机种子做零堆分配确定性模拟，如何按混合模式排序打包为 48 字节 GPU 实例数据，并最终由 D3D12 在透明阶段之后复用场景只读深度完成面向相机的广告牌绘制。

## 粒子系统单向数据流

```text
ParticleEmitterComponent (World) ── / C# F_Particles 脚本指令 (EmitBurst / SetPlaying / Clear)
        │
        ▼
  SceneParticles::Synchronize(world, dt, paused)
        │  驱动每个实体的 ParticleSimulator (SoA 布局 + XorShift32 固定种子 + SwapRemove)
        ▼
  SceneParticles::BuildBatches(cameraPos)
        │  拆分为 Alpha 批（按相机距离由远到近排序）与 Additive 加色批
        │  填充 rhi::ParticleInstanceGpuData (48 B/粒子)
        ▼
  D3D12Particles::RecordD3D12Particles()（ParticleBillboard.hlsl，布局版本 41）
        └─ DepthReadOnly 测试场景深度 + 单次实例化 DrawInstanced(6, count) ──▶ 线性 HDR
```

## ParticleSimulator

[`flare::ParticleSimulator`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Particles/ParticleSimulator.h#L84) 位于 `EngineParticles`（仅依赖 `EngineCore`，可独立用于无头仿真测试）。内部采用 **SoA（Structure of Arrays）** 预分配固定容量数组、确定性 32 位 `XorShift32` 随机数发生器以及 `O(1)` `SwapRemove` 回收机制：运行期步进和粒子消亡全程**零堆内存分配**，同一种子下多轮回放轨迹严格一致。

支持点（`Point`）、球（`Sphere`）与锥形（`Cone`）三种发射形状（`EmitterShape`），提供重力加速度、起止尺寸与颜色插值、`softFactor` 边缘柔化系数等参数（[`ParticleEmitterDesc`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Particles/ParticleSimulator.h#L49)）。

### Step

[`void Step(double deltaSeconds, const Mat4& emitterWorld = Mat4::Identity()) noexcept`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Particles/ParticleSimulator.h#L97) 推进活跃粒子速度与位置积分、回收过期粒子并按 `emissionRatePerSecond` 发射新粒子；`dt <= 0` 时仅同步属性不老化。初始化时调用 `Prewarm()` 可提前步进到稳态粒子群。

### EmitBurst

[`std::size_t EmitBurst(std::size_t count, const Mat4& emitterWorld = Mat4::Identity()) noexcept`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Particles/ParticleSimulator.h#L100) 在发射器当前世界变换处立即瞬发指定数量粒子，受 `maxParticles` 容量上限保护并返回实际生成数量。

## SceneParticles

[`flare::SceneParticles`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Particles/SceneParticles.h#L13) 位于 `EngineSceneParticles`，负责把 `World` 中挂载 `ParticleEmitterComponent` 的实体同步到对应模拟器，并生成只读渲染批供 RHI 消费。

### Synchronize

[`void Synchronize(const World& world, double deltaSeconds, bool paused)`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Particles/SceneParticles.h#L19) 将每个启用实体的发射器属性与世界矩阵同步至模拟器并步进；编辑器暂停时保留当前存活粒子帧以便视口检查。

### BuildBatches

[`void BuildBatches(Vec3 cameraPosition, std::vector<rhi::ParticleBatch>& output) const`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Particles/SceneParticles.h#L31) 将全部活跃粒子按混合模式拆分为常规 Alpha 批与加色（`additive`）批，对 Alpha 批按到相机距离由远及近排序，并填充符合 [`MaterialLayout.h#L35`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/MaterialLayout.h#L35) 契约的 48 字节 `ParticleInstanceGpuData`：

```cpp
// Source/PlatformLayer/Graphics/MaterialLayout.h#L35（ParticleLayoutVersion = 41）
struct alignas(16) ParticleInstanceGpuData   // 48 字节，绑定至 t0 StructuredBuffer
{
    float position[3]{};
    float size = 0;
    float color[4]{};
    float params[4]{}; // x=rotationRadians, y=additive, z=softFactor, w=reserved
};

struct alignas(16) ParticleFrameConstants    // 112 字节，绑定至 b0
{
    Mat4  clipFromWorld{};
    float cameraRight[4]{};
    float cameraUp[4]{};
    float viewport[4]{};
};
```

## D3D12Particles

[`D3D12Particles`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Particles.h#L9) 维护独立的根签名以及 `alphaPipeline` / `additivePipeline` 两个混合 PSO。[`RecordD3D12Particles()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/PlatformLayer/Graphics/D3D12/D3D12Particles.h#L18) 在 HDR 场景颜色与深度之上以 **`DepthReadOnly`** 模式绑定只读深度视图，每批通过一次无顶点缓冲的实例化绘制扩展出面向相机的旋转广告牌四边形，并在像素着色器完成径向柔和衰减与预乘 Alpha 输出：

```hlsl
// Resources/Shaders/ParticleBillboard.hlsl#L22（广告牌扩展与径向柔化）
const ParticleInstanceGpuData instanceData = particleInstances[instanceId];
const float2 corner = corners[vertexId % 6u];
float sinA = sin(instanceData.params.x), cosA = cos(instanceData.params.x);
float2 rotated = float2(corner.x * cosA - corner.y * sinA, corner.x * sinA + corner.y * cosA);
float  halfSize = max(instanceData.positionAndSize.w, 0.0f) * 0.5f;
float3 worldPosition = instanceData.positionAndSize.xyz
    + particleCameraRight.xyz * (rotated.x * halfSize)
    + particleCameraUp.xyz    * (rotated.y * halfSize);

// PSMain：圆形裁剪 + softFactor 径向衰减 + 预乘 Alpha 输出
const float radiusSq = dot(input.uv, input.uv);
clip(1.0f - radiusSq);
const float radial = pow(saturate(1.0f - radiusSq), input.softFactor);
const float alpha  = saturate(input.color.a * radial);
clip(alpha - (1.0f / 1024.0f));
return float4(max(input.color.rgb, 0.0f) * alpha, alpha);
```

## Particles

C# 脚本通过 `Session.Particles`（[`FlareEngine.F_Particles`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Particles.cs#L4)，同时保留兼容别名 `Particles`）实时控制发射器，不直接改写只读世界组件，因此在物理回调或带挂起命令的帧内也可安全调用：

| 方法 / 属性 | 签名 | 说明 |
| --- | --- | --- |
| `EmitterCount` | `uint EmitterCount { get; }` | 返回当前场景中跟踪的粒子发射器数量 |
| `EmitBurst` | `uint EmitBurst(F_Entity entity, uint count)` | 在指定实体发射器处瞬发 `count` 颗粒子，返回实际生成数 |
| `SetPlaying` | `void SetPlaying(F_Entity entity, bool playing)` | 启动或暂停指定发射器的持续发射 |
| `Clear` | `void Clear(F_Entity? entity = null)` | 清空指定实体（或 `null` 清空全场）存活粒子 |
| `ActiveCount` | `uint ActiveCount(F_Entity? entity = null)` | 查询指定实体（或全场）当前存活粒子总数 |

```csharp
// 在脚本行为中触发一次火花爆发
public void OnImpact(F_Entity sparkEntity)
{
    uint spawned = Session.Particles.EmitBurst(sparkEntity, 48);
    if (spawned == 0)
        Session.Particles.SetPlaying(sparkEntity, true);
}
```

::: tip 调试与回放一致性提示
- 若需要跨机器或跨帧完全复现粒子位置，务必在 `ParticleEmitterDesc` 中固定 `seed` 并使用固定仿真步长驱动 `Step()`。
- 粒子通道要求场景深度以只读模式（`D3D12_RESOURCE_STATE_DEPTH_READ`）绑定，使透明粒子能被不透明物体正确遮挡而又不会错误污染后续深度读取。
:::
