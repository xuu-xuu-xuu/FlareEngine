using System.Numerics;
using System.Text;

namespace SelfEngine;

public enum NavPathStatus
{
    Complete = 0,
    Partial = 1,
    Unreachable = 2
}

public readonly record struct NavPath(NavPathStatus Status, Vector3[] Waypoints, float TotalLengthMeters);

public sealed class Navigation
{
    private readonly EngineSession session;

    internal Navigation(EngineSession session) => this.session = session;

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

    public void SetDestination(Entity entity, Vector3 target)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.NavSetDestination(handle, entity.Handle, target));
            return 0;
        });
    }

    public void Stop(Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.NavStopAgent(handle, entity.Handle));
            return 0;
        });
    }

    public float RemainingDistanceMeters(Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.NavRemainingDistance(handle, entity.Handle, out var meters));
            return meters;
        });
    }

    public NavPathStatus AgentStatus(Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.NavAgentStatus(handle, entity.Handle, out var status));
            return (NavPathStatus)status;
        });
    }
}
