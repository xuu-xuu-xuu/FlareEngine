# 刚体、碰撞与角色控制

这篇文档讲清楚 Flare 物理系统的完整链路：`PhysicsWorld` 如何完成广相层掩码过滤、Box/Sphere 窄相求解与冲量加权接触事件；`ScenePhysics` 如何桥接世界实体与第 0 帧射线同步；以及运动学胶囊 `CharacterController` 的步进防穿墙与有限推箱机制。

## 一句话定位

物理系统分为纯 CPU 求解库 `PhysicsWorld`（不持有 `World` 实体也不持有 GPU 对象）和场景同步层 `ScenePhysics`：前者负责刚体积分、序贯冲量约束、触发器追踪、射线查询与胶囊角色解算，后者负责把实体上的 `PhysicsBodyComponent` / `ColliderSettingsComponent` / `CharacterSettingsComponent` 以及 `EntityRecord::layer` 双向同步给物理世界。

## 固定步与射线查询数据流

```text
  Environment::FixedStep() ──▶ PhysicsStep() (EngineNative.cpp)
       │
       ├─ 1. ScenePhysics::Synchronize(world, physics)
       │       ├─ 实体 layer(0..31) ──▶ BodyDesc.layer = (1u << layer)
       │       ├─ 检测属性/缩放/层变更 ──▶ 重建刚体；位姿变化 ──▶ Teleport
       │       └─ 同步 CharacterSettingsComponent 参数到 CharacterController
       │
       ├─ 2. PhysicsWorld::Step(dt)
       │       ├─ 外力/重力积分 + 阻尼
       │       ├─ 广相 AABB 配对 + 双向掩码过滤 (ad.mask & bd.layer) && (bd.mask & ad.layer)
       │       ├─ 触发器对 ──▶ TriggerTracker::Update() (确定性 Enter/Exit)
       │       ├─ 实心对   ──▶ Box/Sphere 窄相 + Warm Start 特征缓存 + 16 次序贯冲量迭代
       │       ├─ 回填冲量加权 ContactEvent (point / normal / normalImpulse)
       │       └─ 位置/四元数积分 + 休眠检测 (0.7s 低速进 Sleep)
       │
       ├─ 3. ScenePhysics::ApplyTransforms()  (刚体位姿写回 World)
       ├─ 4. SceneNavigation::StepAgents()    (导航代理复用 MoveCharacter)
       └─ 5. ScenePhysics::AdvanceCharacters() (未手动 Move 的角色仍受重力与着地解算)

  脚本第 0 帧调用 Raycast() ──▶ flare_physics_raycast()
       ├─ 先执行 ScenePhysics::Synchronize() (确保未跑过 PhysicsStep 也能命中场景刚体)
       └─ PhysicsWorld::Raycast(origin, dir, dist, mask, includeTriggers)
```

## 核心源码拆解

### 1. 刚体描述、冲量加权接触事件与 `Layer` 掩码过滤（`PhysicsWorld`）

[`PhysicsWorld`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.h#L40) 支持有向包围盒（`ColliderShapeKind::Box`）与球体（`ColliderShapeKind::Sphere`），区分动态刚体（`mass > 0`）、静态刚体（`mass == 0`）与纯触发器（`isTrigger == true`）。

来源：[PhysicsWorld.h#L15](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.h#L15) · [PhysicsWorld.cpp#L215](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.cpp#L215) · [PhysicsWorld.cpp#L299](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.cpp#L299)

```cpp
// Source/FunctionLayer/Physics/PhysicsWorld.h
struct BodyDesc
{
    BoxShape shape;
    float mass = 1; // 0 表示静态
    float friction = 0.6f, restitution = 0.0f;
    float linearDamping = 0.02f, angularDamping = 0.05f;
    std::uint32_t layer = 1, mask = UINT32_MAX; // 32 位碰撞层与接收掩码
    std::uint64_t entityStableId = 0;           // 物理不拥有实体，仅回传稳定 ID
    ColliderShapeKind shapeKind = ColliderShapeKind::Box;
    float radius = 0.5f;
    bool isTrigger = false;                     // true 时仅产出 TriggerPairEvent，不产生冲量
};

struct ContactEvent
{
    BodyHandle a, b;
    std::uint64_t entityA = 0, entityB = 0;
    bool began = false;
    bool ended = false;
    Vec3 point{}, normal{};  // 真实流形接触中心与法线
    float normalImpulse = 0.0f;
};
```

#### Layer 双向掩码过滤与冲量加权真实流形

1. **实体 Layer 映射**：[`ScenePhysics::Synchronize()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/ScenePhysics.cpp#L51) 把 `EntityRecord::layer`（`0..31`）转为 `description.layer = 1u << entityLayer`；若实体层发生变化，`ScenePhysics` 自动标记该代理为 `stale` 并重建刚体。
2. **广相配对与射线过滤**：[`PhysicsWorld::Step()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.cpp#L215) 要求 `(ad.mask & bd.layer) && (bd.mask & ad.layer)` 才进入窄相碰撞；[`PhysicsWorld::Raycast()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.cpp#L368) 用 `mask & description.layer` 筛选目标层（默认跳过触发器，除非 `includeTriggers == true`）。
3. **序贯冲量求解后的真实接触点回填**：求解器使用带特征 ID 匹配的 Warm Start 缓存与 `velocityIterations = 16` 轮速度约束迭代（引入 `PenetrationSlop = 0.003m` 与 `RestitutionVelocityThreshold = 1.0 m/s` 避免静止堆叠微抖与低速弹跳）。迭代完毕后，引擎不会随便拿几何中点敷衍，而是按每个接触点的**真实法向冲量 `normalImpulse` 加权求接触中心**：

```cpp
// Source/FunctionLayer/Physics/PhysicsWorld.cpp (迭代求解后回填 ContactEvent)
for (auto& ev : events_)
{
    if (!ev.began) continue;
    const PairKey key{ ev.a.Pack(), ev.b.Pack() };
    Vec3 weightedPt{}, avgPt{};
    float impulseSum = 0.0f;
    std::size_t count = 0;
    for (const auto& c : constraints)
    {
        if (!(c.key == key)) continue;
        avgPt = avgPt + c.point;
        weightedPt = weightedPt + c.point * c.normalImpulse;
        impulseSum += c.normalImpulse;
        ev.normal = c.normal;
        ++count;
    }
    if (count > 0)
    {
        ev.point = impulseSum > 1e-6f ? (weightedPt * (1.0f / impulseSum))
                                      : (avgPt * (1.0f / static_cast<float>(count)));
        ev.normalImpulse = impulseSum;
    }
}
```

当接触断开（上一帧在 `cache_` 中而本帧消失）时，引擎用缓存的 `localA`/`localB` 还原脱离世界坐标和法线，压入 `ended = true` 的 `ContactEvent`。触发器重叠则交由 [`TriggerTracker`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/TriggerTracker.h#L38) 产出确定性排序的 `TriggerPairEvent`。

### 2. 第 0 帧射线查询前的即时同步（`ScenePhysics::Synchronize`）

在 Play 启动的第 0 帧（例如脚本 `F_Start()` 中），或脚本刚在 `F_Update()` 修改了实体的位置/缩放/Layer 之后，固定步的 `PhysicsStep()` 可能还没来得及执行。为防止此时打射线穿过场景里明明存在的地面或墙壁，原生桥接 [`flare_physics_raycast`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/NativeBridge/NativePhysicsExports.inl#L203)（以及 `flare_body_find`、`flare_character_component_state`）在查询前会主动触发 `ScenePhysics::Synchronize()`：

```cpp
// Source/FunctionLayer/Scripting/NativeBridge/NativePhysicsExports.inl
int32_t FLARE_CALL flare_physics_raycast(
    FLARE_Session handle, FLARE_Vector3 origin, FLARE_Vector3 direction,
    float distance, uint32_t mask, FLARE_RayHit* output, int32_t* found) noexcept
{
    return Protect([&]() -> int32_t {
        auto* s = Find(handle); if (!s) return MissingSession();
        if (!output || !found || !std::isfinite(distance) || distance <= 0)
            return Error(FLARE_INVALID_ARGUMENT, "Bridge.InvalidArgument");
        // 关键：查询前即时同步场景刚体与碰撞层，消除第 0 帧漏击
        if (!s->scenePhysics.Synchronize(s->WorldRef(), s->physics))
            return Error(FLARE_INTERNAL_ERROR, "Bridge.PhysicsFailed");
        *output = {}; *found = 0;
        const auto hit = s->physics.Raycast(Vector(origin), Vector(direction), distance, mask);
        if (hit)
        {
            *found = 1;
            *output = { hit->body.Pack(), hit->entityStableId,
                { hit->point.x, hit->point.y, hit->point.z },
                { hit->normal.x, hit->normal.y, hit->normal.z }, hit->distance, 0 };
        }
        return FLARE_OK;
    });
}
```

### 3. 胶囊角色步进与有限推箱冲量（`CharacterController`）

[`CharacterController`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.h#L105) 是一根直立胶囊体（中心 `position`、中轴半长 `segmentHalfHeight`、半径 `radius`），不参与常规刚体旋转求解，专门用于稳定可控的 3D 角色移动。

[`CharacterController::Move()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/CharacterController.cpp#L9) 采用**受限子步进 + 迭代推离 + 冲量预算封顶**三步法：

```cpp
// Source/FunctionLayer/Physics/CharacterController.cpp
const float nextVertical = (jumpPressed && grounded ? jumpSpeed : verticalVelocity) + world.gravity.y * seconds;
const Vec3 displacement = (horizontalDirection * walkSpeed + Vec3{0, nextVertical, 0}) * seconds;
// 1. 按胶囊半径的 1/4 细分步数（上限 128 步），避免高速移动穿透薄墙
const float count = std::ceil(Length(displacement) / (radius * 0.25f));
if (!IsFinite(nextVertical) || !IsFinite(count) || count > 128) return false;
const unsigned steps = static_cast<unsigned>(std::max(1.0f, count));
const Vec3 increment = displacement / static_cast<float>(steps);

verticalVelocity = nextVertical;
float pushBudget = maximumPushImpulse; // 整次 Move 共享推箱冲量上限
grounded = false;

for (unsigned step = 0; step < steps; ++step)
{
    position += increment;
    for (unsigned iteration = 0; iteration < 8; ++iteration)
    {
        bool corrected = false;
        for (const auto handle : bodies)
        {
            const auto* body = world.GetBody(handle);
            if (!body || body->description.isTrigger) continue;
            const auto contact = body->description.shapeKind == ColliderShapeKind::Sphere
                ? CollideCapsuleSphere(position, segmentHalfHeight, radius, {body->description.shape.center, body->description.radius})
                : CollideCapsuleBox(position, segmentHalfHeight, radius, body->description.shape);
            if (!contact) continue;
            // 2. 法线 Y > 0.65 视为站稳地面；Y < -0.65 视为撞到天花板
            if (contact->normal.y > 0.65f && verticalVelocity <= 0) { grounded = true; verticalVelocity = 0; }
            if (contact->normal.y < -0.65f && verticalVelocity > 0) verticalVelocity = 0;
            if (contact->depth > 0)
            {
                position += contact->normal * (contact->depth + 0.0001f);
                corrected = true;
                // 3. 推动动态刚体时，仅补足沿接触法向的目标速度且不超过剩余 pushBudget
                const float into = -Dot(horizontalDirection * walkSpeed, contact->normal);
                if (body->description.mass > 0 && into > 0 && pushBudget > 0)
                {
                    const Vec3 pushDirection = -contact->normal;
                    const Vec3 pointVelocity = body->linearVelocity
                        + Cross(body->angularVelocity, contact->point - body->description.shape.center);
                    const float needed = std::max(0.0f, into - Dot(pointVelocity, pushDirection)) * body->description.mass;
                    const float impulse = std::min(pushBudget, needed);
                    if (impulse > 0) { world.ApplyImpulse(handle, pushDirection * impulse, contact->point); pushBudget -= impulse; }
                }
            }
        }
        if (!corrected) break;
    }
}
```

C# 脚本通过托管代理 [`F_CharacterController`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/CharacterController.cs#L36)（及兼容子类 `CharacterController`）调用 `Move`、`Teleport` 与读取 `State`（`Position` / `VerticalVelocity` / `Grounded`），对应 C ABI 入口为 `flare_character_create` / `flare_character_move` / `flare_character_teleport` / `flare_character_component_state`。

### 4. 解耦的调试线渲染（`PhysicsDebugDraw`）

`PhysicsWorld::DebugLines()` 与 `CharacterController::DebugLines()` 只生成纯 CPU 的 `{ from, to, color }` 线段列表（触发器青色、静态灰、休眠蓝、活跃动态绿、角色胶囊金黄、接触法线红）；渲染层再由 [`AppendPhysicsDebugDraws()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Rendering/PhysicsDebugDraw.cpp#L18) 将每条线段转换成拉伸后的单位立方体 `rhi::DrawIndexed` 绘制命令，彻底隔绝物理层对 D3D12/RHI 的依赖。

## 常用常量与接口速查

| 常量 / 接口 | 默认值 / 说明 | 源码位置 |
| --- | --- | --- |
| `MaximumLinearSpeed` / `MaximumAngularSpeed` | `100.0f` m/s / `100.0f` rad/s（超限返回 `Physics.SpeedLimit`） | [PhysicsWorld.h#L48](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.h#L48) |
| `SleepLinearSpeed` / `SleepAngularSpeed` / `SleepDelaySeconds` | `0.04` / `0.05` / `0.7s`（接触中持续低于阈值进入休眠） | [PhysicsWorld.h#L54](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.h#L54) |
| `PhysicsWorld::Step` / `Raycast` / `Events` / `TriggerEvents` | 步进（`0 < dt <= 0.1s`）、射线检测与事件读取 | [PhysicsWorld.h#L68](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/PhysicsWorld.h#L68) |
| `ScenePhysics::Synchronize` / `ApplyTransforms` / `MoveCharacter` | 世界组件与 `PhysicsWorld` 双向同步及角色控制 | [ScenePhysics.h#L15](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Physics/ScenePhysics.h#L15) |

::: tip 刚体与角色控制器互斥约束
在反射组件创建阶段（[`FeatureComponents.cpp#L230`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Runtime/FeatureComponents.cpp#L230)）和原生桥接层（`HasConflictingPhysicsOwner`），同一个实体不允许同时挂载 `CharacterSettingsComponent` 与 `PhysicsBodyComponent`/`ColliderSettingsComponent`，违反时返回 `"Feature.Physics.Exclusive"` 或 `"Bridge.InvalidBodyAttachment"`。
:::

::: warning 物理世界由 `Environment` 驱动时禁止手工再调 `Step()`
当宿主启用了 `PhysicsWorld::SetEnvironmentDriven(true)` 时，模拟时钟归 `Environment` 独占，外部直接调用 `PhysicsWorld::Step()` 或 `Advance()` 会立刻返回 `"Physics.ClockOwned"`，必须统一通过 `Environment::RunFrame()` 内的 `StepFromEnvironment()` 推进，避免双时钟重放。
:::
