# 导航网格烘焙、A* 漏斗寻路与角色代理

这篇文档讲清楚 Flare 导航系统的完整管线：从场景静态几何体采集、体素化烘焙生成 `.senav` 凸多边形网格、基于 BVH + A* + Funnel 的拐点寻路，到运行期 `SceneNavigation` 如何复用 `ScenePhysics::MoveCharacter` 驱动 AI 代理移动。

## 一句话定位

导航系统拆成**纯几何寻路库 `EngineNavigation`**（仅依赖 `EngineCore`，零物理/渲染依赖）、**场景代理协调器 `EngineSceneNavigation`**（复用物理胶囊移动，不写第二套防穿墙逻辑）以及**异步/离线烘焙前端**（编辑器 `NavigationBakeService` 与命令行 `NavigationCooker`）。

## 烘焙与寻路数据流

```text
  [离线 / 编辑器烘焙]
  World 静态实体 ──▶ CollectNavigationSceneGeometry()
       │             (遵守 EntityStaticFlags::Explicit & Navigation，支持无 Mesh 的静态盒/球碰撞体)
       ▼
  BakeNavMesh(vertices, indices, NavBakeSettings)
    (体素化 → 坡度/净高过滤 → 代理半径腐蚀 → 凸多边形与邻接表 → 平坦 BVH)
       │
       ▼
  .senav 资产 (魔数 SENAV001, v1) ──(NavigationBakeService 异步写出 / NavigationCooker CLI)

  [运行时寻路与代理推进]
  SceneNavigation::SetDestination() ──▶ ResolveNavMesh() (按内容哈希缓存 .senav)
       │                                      │
       ▼                                      ▼
  FindNavPath() (BVH 定位起终点 → 多边形通道 A* → Simple Stupid Funnel 漏斗拉直)
       │
       ▼
  SceneNavigation::StepAgents() (固定步由 PhysicsStep 调用)
       ├─ 若实体挂有 CharacterSettingsComponent ──▶ ScenePhysics::MoveCharacter() (共享防穿墙与推箱)
       └─ 否则回退直接按 speed 插值更新实体世界坐标
```

## 核心源码拆解

### 1. 导航网格数据结构与 BVH 查询（`NavMeshData`）

[`NavMeshData`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/NavMeshData.h#L39) 保存烘焙参数 `NavBakeSettings`、世界空间顶点表 `vertices_`、凸多边形与邻接边表 `polygons_`（邻接索引 `-1` 表示实心边界），以及用于加速定位的平坦 `Aabb` BVH（叶节点阈值 `4`）。

- **二进制持久化**：`Serialize()` / `Deserialize()` 读写 `.senav` 资产（魔数 `SENAV001`、格式版本 `1`，校验顶点数 `<= 2M`、多边形数 `<= 512K`、单多边形边数 `<= 64`）。
- **最近可行走面定位**：[`FindNearestPolygon()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/NavMeshData.h#L56) 在以 `point` 为中心、`searchHalfExtents` 为半尺寸的搜索盒内遍历 BVH，返回距离最近的多边形索引，并可选通过 `ProjectPointToPolygon()` 输出带高度插值的表面投影点。

### 2. 几何采集与体素烘焙（`CollectNavigationSceneGeometry` & `NavMeshBaker`）

在进入烘焙前，[`CollectNavigationSceneGeometry()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/SceneNavigation.cpp#L70) 遍历世界实体收集世界空间三角形汤，过滤规则与全引擎动静契约一致：

```cpp
// Source/FunctionLayer/Navigation/SceneNavigation.cpp
const EntityRecord* rec = world.Get(entity);
if (rec && (rec->staticFlags & EntityStaticFlags::Explicit) != 0)
{
    // 用户显式声明动静契约时，严格以 Navigation 位为准
    if ((rec->staticFlags & EntityStaticFlags::Navigation) == 0) continue;
}
else
{
    // 自动推导模式：排除导航代理、胶囊角色和动态刚体 (body.mass > 0)
    if (world.FindComponent(entity, NavigationAgentComponent::TypeId)
        || world.FindComponent(entity, CharacterSettingsComponent::TypeId)) continue;
    // ... 若 mass > 0 则跳过
}
```

如果某个静态障碍/地面实体没有可见 `MeshComponent`，但挂有非触发器的静态 `PhysicsBodyComponent` 或 `ColliderSettingsComponent`，采集器也会用单位盒/球网格将其物理体积纳入烘焙，避免“隐形空气墙未参与烘焙”。

随后调用自由函数 [`BakeNavMesh()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/NavMeshBaker.h#L10) 执行体素化、可行走坡度/净高过滤、障碍半径腐蚀与邻接凸多边形构建：

| `NavBakeSettings` 参数 | 默认值 | 含义 | CLI 选项 |
| :--- | :--- | :--- | :--- |
| `cellSizeMeters` | `0.25` | 体素网格水平精度（米） | `--cell` |
| `agentRadiusMeters` | `0.35` | 角色碰撞半径，用于障碍向内腐蚀（米） | `--radius` |
| `agentHeightMeters` | `1.8` | 角色可行走净空高度（米） | `--height` |
| `maxSlopeDegrees` | `45.0` | 允许行走的最大地面坡度（度） | `--slope` |
| `maxStepMeters` | `0.4` | 相邻可行走体素允许跨越的台阶高度差（米） | `--step` |

### 3. A* 通道与漏斗拉直（`NavPathfinder`）

[`FindNavPath()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/NavPathfinder.h#L23) 接收 `NavMeshData`、起终点与搜索半尺寸（默认 `{2, 4, 2}` 米）：
1. 用 `FindNearestPolygon()` 将起点和终点投影到最近可行走多边形；
2. 在多边形邻接图上运行 A* 搜索可行通道；若目标不可达但能找到离目标最近的连通多边形，返回 `NavPathStatus::Partial`；
3. 对通道共享门户边（portals）运行 **Simple Stupid Funnel** 漏斗拉直算法，输出紧凑拐点数组 `NavPathResult{ status, waypoints, totalLengthMeters }`。

### 4. 场景代理协调与复用物理胶囊（`SceneNavigation`）

[`SceneNavigation`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/SceneNavigation.h#L25) 维护 `.senav` 资产缓存（按文件内容哈希自动检测重烘焙热更新）与每个活跃 `NavigationAgentComponent` 的运行时路径状态：

- [`SetDestination()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/SceneNavigation.cpp#L378) 与 [`StopAgent()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/SceneNavigation.cpp#L408) 只改写 `SceneNavigation` 内部运行时覆盖表，**不直接写只读 `World` 组件**，因此可以在脚本回调或有挂起命令期间安全调用。
- [`StepAgents()`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/SceneNavigation.cpp#L433) 在物理固定步内推进所有代理：按 `0.1m` 容差推进当前拐点、在末点 `stoppingDistance` 内判定到达；移动时**优先检测实体是否挂有胶囊角色控制器**，若有则直接将期望水平方向交给 [`ScenePhysics::MoveCharacter()`](physics.md)，否则回退为按 `navagent.speed` 直接插值实体世界位置：

```cpp
// Source/FunctionLayer/Navigation/SceneNavigation.cpp
if (const auto* character = scenePhysics.Character(record->stableId))
{
    const float moveDt = std::min(deltaSeconds, 0.05f);
    const float walkSpeed = character->walkSpeed;
    float scale = walkSpeed > 1e-4f ? std::min(1.0f, runtime->authored.speed / walkSpeed) : 1.0f;
    if (isFinal && walkSpeed * moveDt > 1e-4f)
        scale = std::min(scale, len / (walkSpeed * moveDt));
    scenePhysics.MoveCharacter(world, physics, record->stableId, dirXZ * scale, false, moveDt);
}
```

### 5. 异步编辑器烘焙与命令行离线烘焙

- **编辑器后台服务**：[`NavigationBakeService`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Editor/NavigationBakeService.h#L27) 在主线程收集场景三角形后提交到 `CoreLayer/TaskQueue` 工作线程调用 `BakeNavMesh()`，不阻塞编辑器 UI；待 `Poll()` 收到结果后在主线程原子写出 `.senav`、注册资产并可通过 `MountBakedNavMeshToWorld()` 自动挂载到未分配导航网格的代理上。
- **命令行工具**：[`NavigationCooker.cpp`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/ToolLayer/Tools/NavigationCooker.cpp#L33) 使用完全相同的 `CollectNavigationSceneGeometry` + `BakeNavMesh` 路径，支持在构建流水线里批量离线烘焙：
  ```text
  NavigationCooker.exe bake <scene.scene.json> <contentRoot> <output.senav> [--cell 0.25] [--radius 0.35] [--height 1.8] [--slope 45] [--step 0.4]
  ```

### 6. C# 托管脚本接口（`F_Navigation`）

C# 脚本在 `F_Behaviour` 中直接通过 `Navigation` 属性（底层即 `Scripts.Session.Navigation`，类型为 [`FlareEngine.F_Navigation`](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Navigation.cs#L33)，兼容子类 `Navigation`）发起一次性路径查询或控制场景内的导航代理：

```csharp
// 1. 驱动挂载了 NavigationAgent 的实体自动寻路
Navigation.SetDestination(Entity, targetPos);
float left = Navigation.RemainingDistanceMeters(Entity);

// 2. 纯查询路径拐点（不移动任何实体）
F_NavPath path = Navigation.FindPath(navAssetId, startPos, goalPos);
if (path.Status == F_NavPathStatus.Complete) { /* 遍历 path.Waypoints */ }
```

## 常用接口速查

| 类 / 模块 | 核心接口 | 源码位置 |
| --- | --- | --- |
| `NavMeshData` | `FindNearestPolygon` / `ProjectPointToPolygon` / `Serialize` / `Deserialize` | [NavMeshData.h#L39](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/NavMeshData.h#L39) |
| `NavMeshBaker` | `BakeNavMesh(vertices, indices, settings, output, errorKey)` | [NavMeshBaker.h#L10](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/NavMeshBaker.h#L10) |
| `NavPathfinder` | `FindNavPath(navMesh, start, goal, queryHalfExtents)` | [NavPathfinder.h#L23](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/NavPathfinder.h#L23) |
| `SceneNavigation` | `FindPath` / `SetDestination` / `StopAgent` / `StepAgents` / `RemainingDistanceMeters` | [SceneNavigation.h#L35](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Navigation/SceneNavigation.h#L35) |
| `F_Navigation` (C#) | `FindPath` / `SetDestination` / `Stop` / `RemainingDistanceMeters` / `AgentStatus` | [Navigation.cs#L40](https://github.com/xuu-xuu-xuu/FlareEngine/blob/4030e29/Source/FunctionLayer/Scripting/Managed/Navigation.cs#L40) |

::: tip 如何让某些活动门或装饰物不参与导航烘焙？
在编辑器选中该实体，将实体头部的 `Static` 下拉切为显式模式并取消勾选 `Navigation`（即设置 `EntityStaticFlags::Explicit` 且清空 `EntityStaticFlags::Navigation`），重新烘焙即可将其从 `.senav` 几何采集源中排除。
:::

::: warning 给导航代理同时配置 `CharacterController` 时的速度上限
查看 `SceneNavigation::StepAgents()` 中 `scale = min(1.0f, navagent.speed / character.walkSpeed)`：当代理复用胶囊体移动时，最终水平速度不会超过胶囊体自身的 `character.walkSpeed`。如果发现 AI 移速提不上去，请同步调高 `CharacterSettingsComponent` 的 `walkSpeed`。
:::
