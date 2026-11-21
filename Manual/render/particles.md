# 确定性粒子模拟与 GPU 广告牌绘制

粒子系统由纯 CPU 确定性模拟器、场景发射器同步器与 D3D12 广告牌渲染通道组成：仿真层仅依赖 `EngineCore`，通过 `rhi::ParticleFrame` 值快照单向传入渲染后端，在透明阶段之后复用场景深度完成软粒子衰减。

## ParticleSimulator

`ParticleSimulator` 位于 `EngineParticles`（仅依赖 `EngineCore`），内部以固定种子 `XorShift32` 驱动点/球/锥形发射形状（`EmitterShape`）、重力加速度、尺寸与颜色插值，采用 `SwapRemove` 保持紧密内存布局，保证同种子下多轮回放确定一致。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Particles/ParticleSimulator.h#L84)

### Step

`void Step(double deltaSeconds, const Mat4& emitterWorld = Mat4::Identity())` 推进活跃粒子积分、回收过期粒子并按 `emissionRatePerSecond` 生成新粒子；初始化时调用 `Prewarm()` 可提前预热稳态粒子群。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Particles/ParticleSimulator.h#L97)

### EmitBurst

`std::size_t EmitBurst(std::size_t count, const Mat4& emitterWorld = Mat4::Identity())` 立即瞬发指定数量粒子，受发射器容量上限约束并返回实际生成数量。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Particles/ParticleSimulator.h#L100)

## SceneParticles

`SceneParticles` 位于 `EngineSceneParticles`，跟踪世界中挂载 `ParticleEmitterComponent` 的实体，同步属性修改并构建面向渲染器的只读粒子批次。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Particles/SceneParticles.h#L13)

### Synchronize

`void Synchronize(const World& world, double deltaSeconds, bool paused)` 将组件属性与实体世界矩阵同步到对应模拟器并按步长推进；暂停时保留当前粒子帧供视口检查。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Particles/SceneParticles.h#L19)

### BuildBatches

`void BuildBatches(Vec3 cameraPosition, std::vector<rhi::ParticleBatch>& output)` 将全部活跃粒子拆分为常规 Alpha 混合与加色（`additive`）两类批次，对 Alpha 粒子按相机距离由远及近排序，填充 48 字节的 `ParticleInstanceGpuData`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Particles/SceneParticles.h#L31)

## D3D12Particles

`D3D12Particles` 位于 `PlatformLayer/Graphics/D3D12`，在 HDR 场景颜色与深度之上以 `DepthReadOnly` 模式实例化绘制面向相机的广告牌四边形（遵循 [MaterialLayout.h](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Graphics/MaterialLayout.h#L284) 的 `ParticleLayoutVersion` 常量布局），并在像素着色器采样场景深度执行软粒子过渡。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/PlatformLayer/Graphics/D3D12/D3D12Particles.h#L9)

## Particles

C# 脚本通过 `Session.Particles`（`FlareEngine.Particles`）控制粒子爆发、启停与清空：`EmitBurst(Entity, uint)`、`SetPlaying(Entity, bool)`、`Clear(Entity?)` 与 `ActiveCount(Entity?)`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Scripting/Managed/Particles.cs#L3)
