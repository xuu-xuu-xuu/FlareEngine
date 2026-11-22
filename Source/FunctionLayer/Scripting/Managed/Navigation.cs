using System.Numerics;
using System.Text;

namespace FlareEngine;

/// <summary>导航网格寻路状态。</summary>
public enum F_NavPathStatus
{
    Complete = 0,
    Partial = 1,
    Unreachable = 2
}

/// <summary>导航网格寻路状态（兼容无前缀写法）。</summary>
public enum NavPathStatus
{
    Complete = 0,
    Partial = 1,
    Unreachable = 2
}

/// <summary>导航寻路结果（路径状态、路点数组与总路径长度米数）。</summary>
public readonly record struct F_NavPath(F_NavPathStatus Status, Vector3[] Waypoints, float TotalLengthMeters)
{
    public static implicit operator NavPath(F_NavPath value) => new((NavPathStatus)value.Status, value.Waypoints, value.TotalLengthMeters);
    public static implicit operator F_NavPath(NavPath value) => new((F_NavPathStatus)value.Status, value.Waypoints, value.TotalLengthMeters);
}

/// <summary>导航寻路结果（兼容无前缀写法）。</summary>
public readonly record struct NavPath(NavPathStatus Status, Vector3[] Waypoints, float TotalLengthMeters);

/// <summary>导航网格寻路与 NavigationAgent 代理控制服务。</summary>
public class F_Navigation
{
    private readonly EngineSession session;

    internal F_Navigation(EngineSession session) => this.session = session;

    /// <summary>在指定导航网格资产上计算从 <paramref name="start"/> 到 <paramref name="goal"/> 的折线路径。</summary>
    public unsafe NavPath FindPath(string navAssetId, Vector3 start, Vector3 goal)
    {
        ArgumentNullException.ThrowIfNull(navAssetId);
        uint byteCount = checked((uint)Encoding.UTF8.GetByteCount(navAssetId));
        return session.Invoke(handle =>
        {
            const uint initialCapacity = 64;
            var buffer = new Vector3[initialCapacity];
            NativeMethods.NavPathInfo info;
            fixed (Vector3* ptr = buffer)
            {
                EngineSession.Check(NativeMethods.NavFindPath(
                    handle, navAssetId, byteCount, start, goal, ptr, initialCapacity, out info));
            }

            if (info.WaypointCount == 0)
            {
                return new NavPath((NavPathStatus)info.Status, Array.Empty<Vector3>(), info.TotalLengthMeters);
            }

            if (info.WaypointCount > initialCapacity)
            {
                buffer = new Vector3[info.WaypointCount];
                fixed (Vector3* ptr = buffer)
                {
                    EngineSession.Check(NativeMethods.NavFindPath(
                        handle, navAssetId, byteCount, start, goal, ptr, info.WaypointCount, out info));
                }
            }

            int actualCount = checked((int)Math.Min(info.WaypointCount, (uint)buffer.Length));
            var waypoints = actualCount == buffer.Length ? buffer : buffer[..actualCount];
            return new NavPath((NavPathStatus)info.Status, waypoints, info.TotalLengthMeters);
        });
    }

    /// <summary>为挂有 NavigationAgent 组件的实体设置世界空间寻路目标点。</summary>
    public void SetDestination(F_Entity entity, Vector3 target)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.NavSetDestination(handle, entity.Handle, target));
            return 0;
        });
    }

    /// <summary>立即停止实体上导航代理的当前移动。</summary>
    public void Stop(F_Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.NavStopAgent(handle, entity.Handle));
            return 0;
        });
    }

    /// <summary>查询导航代理距离目标点的剩余路径长度（米）。</summary>
    public float RemainingDistanceMeters(F_Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.NavRemainingDistance(handle, entity.Handle, out var meters));
            return meters;
        });
    }

    /// <summary>查询导航代理当前的寻路可达状态。</summary>
    public NavPathStatus AgentStatus(F_Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.NavAgentStatus(handle, entity.Handle, out var status));
            return (NavPathStatus)status;
        });
    }
}

/// <summary>导航网格寻路与代理控制服务（兼容无前缀写法，继承自 <see cref="F_Navigation"/>）。</summary>
public sealed class Navigation : F_Navigation
{
    internal Navigation(EngineSession session) : base(session) { }
}
