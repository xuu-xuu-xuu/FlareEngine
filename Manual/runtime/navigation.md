# 导航网格烘焙、A* 漏斗寻路与角色代理

导航系统拆分为纯几何寻路库 `EngineNavigation`（仅依赖 `EngineCore`）、场景代理协调器 `EngineSceneNavigation`（复用 `ScenePhysics::MoveCharacter` 推进胶囊体，不重写第二套碰撞防穿墙逻辑），以及离线烘焙工具 `NavigationCooker` 与编辑器后台烘焙服务 `NavigationBakeService`。

## NavMeshData

`NavMeshData` 保存 `.senav` 导航网格的烘焙参数 `NavBakeSettings`、顶点表、凸多边形及邻接边表，并内置基于 `Aabb` 的平坦 BVH 用于快速多边形定位。`Serialize()` 与 `Deserialize()` 负责 `.senav` 资产二进制序列化与完整性校验。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Navigation/NavMeshData.h#L39)

### FindNearestPolygon

`std::optional<std::uint32_t> FindNearestPolygon(Vec3 point, Vec3 searchHalfExtents, Vec3* projectedPoint = nullptr) const noexcept` 借助 BVH 在限定半尺寸搜索盒内定位最近可行走多边形，并可选输出表面投影点供寻路起终点使用。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Navigation/NavMeshData.h#L56)

## NavMeshBaker

自由函数 [BakeNavMesh()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Navigation/NavMeshBaker.h#L10) 输入世界空间三角形汤与 `NavBakeSettings`（体素步长、代理半径/身高、最大坡度与最大台阶高度），执行体素化、可行走面坡度过滤、障碍半径腐蚀与邻接凸多边形构建，输出可直接序列化的 `NavMeshData`。

| 参数 | 默认值 | 含义 |
| :--- | :--- | :--- |
| `cellSizeMeters` | `0.25` | 体素网格水平精度（米） |
| `agentRadiusMeters` | `0.35` | 角色碰撞半径（用于障碍腐蚀） |
| `agentHeightMeters` | `1.8` | 角色净空高度（米） |
| `maxSlopeDegrees` | `45` | 允许行走的最大坡度（度） |
| `maxStepMeters` | `0.4` | 相邻可行走网格允许跨越的台阶高度差（米） |

## NavPathfinder

自由函数 [FindNavPath()](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Navigation/NavPathfinder.h#L23) 在 `NavMeshData` 上先以多边形通道执行 A* 搜索，再运行 Simple Stupid Funnel（漏斗拉直）生成紧凑拐点数组 `NavPathResult`，状态明确区分 `Complete`、`Partial` 与 `Unreachable`。

## SceneNavigation

`SceneNavigation` 位于 `EngineSceneNavigation`，为携带 `NavigationAgentComponent` 的实体维护路径目标与当前拐点索引。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Navigation/SceneNavigation.h#L25)

### SetDestination

`bool SetDestination(const World&, AssetStore*, StableEntityId entityId, Vec3 target, std::string& errorKey)` 读取实体引用的 `.senav` 资产并计算新路径；`StepAgents()` 在固定步中计算朝向当前拐点的期望速度，直接交由 [ScenePhysics](physics.md) 推进胶囊体移动。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Navigation/SceneNavigation.h#L44)

## NavigationBakeService

`NavigationBakeService` 位于 `ToolLayer/Editor`，在编辑器中收集场景静态网格世界三角形后投递至 `CoreLayer/TaskQueue` 后台线程异步调用 `BakeNavMesh`，再在主线程 `Poll()` 阶段原子写出 `.senav` 并刷新项目资产清单；命令行离线批量烘焙使用同源可执行程序 [NavigationCooker.cpp](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ToolLayer/Tools/NavigationCooker.cpp#L33)。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/ToolLayer/Editor/NavigationBakeService.h#L27)

## Navigation

C# 脚本通过 `Session.Navigation`（`SelfEngine.Navigation`）执行同步路径查询或驱动导航代理：`FindPath(string, Vector3, Vector3)`、`SetDestination(Entity, Vector3)`、`Stop(Entity)` 与 `RemainingDistanceMeters(Entity)`。[查看源码](https://github.com/xuu-xuu-xuu/FlareEngine/blob/6f1fea505082d4a90a24b4d1779c45fbf8c6152d/Source/FunctionLayer/Scripting/Managed/Navigation.cs#L15)
