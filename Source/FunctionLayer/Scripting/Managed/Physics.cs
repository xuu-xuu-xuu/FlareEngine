using System.Numerics;
using System.Runtime.InteropServices;

namespace FlareEngine;

/// <summary>盒体刚体创建参数描述。</summary>
[StructLayout(LayoutKind.Sequential)]
public struct F_BoxBodyDescription
{
    public Vector3 HalfExtents;
    public float Mass, Friction, Restitution;
    public uint Layer, Mask;
    public static F_BoxBodyDescription Dynamic => BoxBodyDescription.Dynamic;
    public static F_BoxBodyDescription Static => BoxBodyDescription.Static;
    public static implicit operator BoxBodyDescription(F_BoxBodyDescription v) => new()
    { HalfExtents = v.HalfExtents, Mass = v.Mass, Friction = v.Friction, Restitution = v.Restitution, Layer = v.Layer, Mask = v.Mask };
    public static implicit operator F_BoxBodyDescription(BoxBodyDescription v) => new()
    { HalfExtents = v.HalfExtents, Mass = v.Mass, Friction = v.Friction, Restitution = v.Restitution, Layer = v.Layer, Mask = v.Mask };
}

[StructLayout(LayoutKind.Sequential)]
public struct BoxBodyDescription
{
    public Vector3 HalfExtents;
    public float Mass, Friction, Restitution;
    public uint Layer, Mask;
    public static BoxBodyDescription Dynamic => new() { HalfExtents = new(0.5f), Mass = 1, Friction = 0.6f, Layer = 1, Mask = uint.MaxValue };
    public static BoxBodyDescription Static { get { var value = Dynamic; value.Mass = 0; return value; } }
}

/// <summary>球体刚体创建参数描述。</summary>
[StructLayout(LayoutKind.Sequential)]
public struct F_SphereBodyDescription
{
    public float Radius, Mass, Friction, Restitution;
    public uint Layer, Mask;
    private int isTrigger;
    private uint reserved;
    public bool IsTrigger { readonly get => isTrigger != 0; set => isTrigger = value ? 1 : 0; }
    public static F_SphereBodyDescription Dynamic => SphereBodyDescription.Dynamic;
    public static F_SphereBodyDescription Static => SphereBodyDescription.Static;
    public static F_SphereBodyDescription Trigger => SphereBodyDescription.Trigger;
    public static implicit operator SphereBodyDescription(F_SphereBodyDescription v) => new()
    { Radius = v.Radius, Mass = v.Mass, Friction = v.Friction, Restitution = v.Restitution, Layer = v.Layer, Mask = v.Mask, IsTrigger = v.IsTrigger };
    public static implicit operator F_SphereBodyDescription(SphereBodyDescription v) => new()
    { Radius = v.Radius, Mass = v.Mass, Friction = v.Friction, Restitution = v.Restitution, Layer = v.Layer, Mask = v.Mask, IsTrigger = v.IsTrigger };
}

[StructLayout(LayoutKind.Sequential)]
public struct SphereBodyDescription
{
    public float Radius, Mass, Friction, Restitution;
    public uint Layer, Mask;
    private int isTrigger;
    private uint reserved;
    public bool IsTrigger { readonly get => isTrigger != 0; set => isTrigger = value ? 1 : 0; }
    public static SphereBodyDescription Dynamic => new() { Radius = 0.5f, Mass = 1, Friction = 0.6f, Layer = 1, Mask = uint.MaxValue };
    public static SphereBodyDescription Static { get { var value = Dynamic; value.Mass = 0; return value; } }
    public static SphereBodyDescription Trigger { get { var value = Static; value.IsTrigger = true; return value; } }
}
[StructLayout(LayoutKind.Sequential)]
internal struct NativeRayHit
{
    internal ulong Body, EntityStableId;
    internal Vector3 Point, Normal;
    internal float Distance;
    internal uint Reserved;
}
[StructLayout(LayoutKind.Sequential)]
internal struct NativeContact
{
    internal ulong EntityA, EntityB;
    internal int Began, Ended;
    internal Vector3 Point, Normal;
    internal float Impulse;
    internal uint Reserved;
}
[StructLayout(LayoutKind.Sequential)]
internal struct NativeTrigger { internal ulong TriggerEntity, OtherEntity; internal int Entered; internal uint Reserved; }
/// <summary>物理射线检测命中结果（命中实体、刚体、世界交点、表面法线与命中距离）。</summary>
public readonly record struct F_RayHit(Entity Entity, F_RigidBody Body, Vector3 Point, Vector3 Normal, float Distance)
{
    public static implicit operator RayHit(F_RayHit h) => new(h.Entity, (RigidBody)h.Body, h.Point, h.Normal, h.Distance);
    public static implicit operator F_RayHit(RayHit h) => new(h.Entity, h.Body, h.Point, h.Normal, h.Distance);
}

/// <summary>物理射线检测命中结果（Unity 风格命名别名，等价于 <see cref="F_RayHit"/>）。</summary>
public readonly record struct F_RaycastHit(Entity Entity, F_RigidBody Body, Vector3 Point, Vector3 Normal, float Distance)
{
    public static implicit operator RayHit(F_RaycastHit h) => new(h.Entity, (RigidBody)h.Body, h.Point, h.Normal, h.Distance);
    public static implicit operator F_RaycastHit(RayHit h) => new(h.Entity, h.Body, h.Point, h.Normal, h.Distance);
    public static implicit operator F_RayHit(F_RaycastHit h) => new(h.Entity, h.Body, h.Point, h.Normal, h.Distance);
    public static implicit operator F_RaycastHit(F_RayHit h) => new(h.Entity, h.Body, h.Point, h.Normal, h.Distance);
}

/// <summary>物理射线检测命中结果（兼容无前缀写法）。</summary>
public readonly record struct RayHit(Entity Entity, RigidBody Body, Vector3 Point, Vector3 Normal, float Distance);
/// <summary>物理接触事件快照（含真实流形接触点、由 EntityA 指向 EntityB 的表面法线与法向冲量）。</summary>
public readonly record struct PhysicsContact(EntityReference EntityA, EntityReference EntityB, bool Began, bool Ended, Vector3 Point, Vector3 Normal, float Impulse)
{
    public PhysicsContact(EntityReference entityA, EntityReference entityB, bool began, bool ended)
        : this(entityA, entityB, began, ended, Vector3.Zero, Vector3.UnitY, 0f) { }
}
/// <summary>物理触发器进出事件快照。</summary>
public readonly record struct PhysicsTrigger(EntityReference TriggerEntity, EntityReference OtherEntity, bool Entered);

/// <summary>刚体动力学瞬时状态（位姿、线速度、角速度、质量与休眠标志）。</summary>
[StructLayout(LayoutKind.Sequential)]
public struct F_RigidBodyState
{
    public Transform Transform;
    public Vector3 LinearVelocity, AngularVelocity;
    public float Mass;
    private int sleeping;
    /// <summary>刚体当前是否处于物理休眠状态。</summary>
    public readonly bool Sleeping => sleeping != 0;
    public static implicit operator RigidBodyState(F_RigidBodyState s)
    {
        RigidBodyState r = default;
        r.Transform = s.Transform;
        r.LinearVelocity = s.LinearVelocity;
        r.AngularVelocity = s.AngularVelocity;
        r.Mass = s.Mass;
        if (s.Sleeping) r.SetSleepingInternal(true);
        return r;
    }
    public static implicit operator F_RigidBodyState(RigidBodyState s)
    {
        F_RigidBodyState r = default;
        r.Transform = s.Transform;
        r.LinearVelocity = s.LinearVelocity;
        r.AngularVelocity = s.AngularVelocity;
        r.Mass = s.Mass;
        r.sleeping = s.Sleeping ? 1 : 0;
        return r;
    }
}

/// <summary>刚体动力学瞬时状态（位姿、线速度、角速度、质量与休眠标志）。</summary>
[StructLayout(LayoutKind.Sequential)]
public struct RigidBodyState
{
    public Transform Transform;
    public Vector3 LinearVelocity,AngularVelocity;
    public float Mass;
    private int sleeping;
    /// <summary>刚体当前是否处于物理休眠状态。</summary>
    public readonly bool Sleeping=>sleeping!=0;
    internal void SetSleepingInternal(bool v) => sleeping = v ? 1 : 0;
}

/// <summary>物理刚体运行时代理（可通过 <c>GetComponent&lt;F_RigidBody&gt;()</c> 或 <c>GetComponent&lt;F_Rigidbody&gt;()</c> 获取）。</summary>
public partial class F_RigidBody
{
    internal F_RigidBody(EngineSession session, ulong handle, Entity entity) { Session = session; Handle = handle; Entity = entity; }
    internal EngineSession Session { get; }
    /// <summary>底层物理刚体句柄。</summary>
    public ulong Handle { get; }
    /// <summary>所属场景实体。</summary>
    public Entity Entity { get; }
    /// <summary>读取刚体当前的位姿、速度与休眠状态。</summary>
    public RigidBodyState State=>Session.Invoke(session=>{EngineSession.Check(NativeMethods.GetBodyState(session,Handle,out var state));return state;});
    /// <summary>获取或设置刚体是否为仅检测重叠而不产生物理排斥力的触发器（Trigger）。</summary>
    public bool IsTrigger
    {
        get => Session.Invoke(session => { EngineSession.Check(NativeMethods.GetBodyTrigger(session, Handle, out var value)); return value != 0; });
        set => Session.Invoke(session => { EngineSession.Check(NativeMethods.SetBodyTrigger(session, Handle, value ? 1 : 0)); return 0; });
    }
    /// <summary>直接设置刚体的线速度（米/秒）与角速度（弧度/秒）。</summary>
    public void SetVelocity(Vector3 linear, Vector3 angular = default) => Session.Invoke(session => { EngineSession.Check(NativeMethods.SetBodyVelocity(session, Handle, linear, angular)); return 0; });
    /// <summary>在给定世界空间作用点对刚体施加持续力（牛顿）。</summary>
    public void ApplyForce(Vector3 force, Vector3 worldPoint) => Apply(force, worldPoint, false);
    /// <summary>在给定世界空间作用点对刚体施加瞬时冲量。</summary>
    public void ApplyImpulse(Vector3 impulse, Vector3 worldPoint) => Apply(impulse, worldPoint, true);
    private void Apply(Vector3 force, Vector3 point, bool impulse) => Session.Invoke(session => { EngineSession.Check(NativeMethods.ApplyBodyForce(session, Handle, force, point, impulse ? 1 : 0)); return 0; });
    /// <summary>将刚体瞬移到指定变换位置（默认清空已有速度）。</summary>
    public void Teleport(Transform transform, bool clearVelocity = true) => Session.Invoke(session => { EngineSession.Check(NativeMethods.TeleportBody(session, Handle, in transform, clearVelocity ? 1 : 0)); return 0; });
    /// <summary>从物理世界中销毁移除该刚体。</summary>
    public void Destroy()
    {
        try{Session.Invoke(session=>{EngineSession.Check(NativeMethods.DestroyBody(session,Handle));return 0;});}
        catch(EngineException exception)when(exception.Key=="Bridge.InvalidBody"&&(Session.IsSceneRetiring(Entity.Handle)||!Entity.IsAlive)){/* 场景/GO 已代为释放原生代理，所有者清理仍可安全完成。 */}
    }
}

/// <summary>物理刚体运行时代理（Unity 大小写习惯别名，继承自 <see cref="F_RigidBody"/>）。</summary>
public class F_Rigidbody : F_RigidBody
{
    internal F_Rigidbody(EngineSession session, ulong handle, Entity entity) : base(session, handle, entity) { }
}

/// <summary>物理刚体运行时代理（兼容无前缀写法，继承自 <see cref="F_Rigidbody"/>）。</summary>
public sealed partial class RigidBody : F_Rigidbody
{
    internal RigidBody(EngineSession session, ulong handle, Entity entity) : base(session, handle, entity) { }
}

/// <summary>物理世界服务（射线检测、刚体查找与动态刚体创建）。</summary>
public sealed unsafe class Physics
{
    private readonly EngineSession session;
    internal Physics(EngineSession session) => this.session = session;
    internal object? ClockOwner;
    /// <summary>开关视口物理碰撞体线框调试绘制，返回修改前的旧状态。</summary>
    public bool SetDebugDrawing(bool enabled)=>session.Invoke(handle=>{EngineSession.Check(NativeMethods.SetPhysicsDebugDraw(handle,enabled?1:0,out var previous));return previous!=0;});
    public double DroppedSeconds { get; private set; }
    /// <summary>在指定实体上创建盒体碰撞刚体。</summary>
    public RigidBody CreateBox(F_Entity entity, BoxBodyDescription description)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        var concrete = entity as Entity ?? new Entity(session, entity.Handle);
        return session.Invoke(handle => { EngineSession.Check(NativeMethods.CreateBody(handle, concrete.Handle, in description, out var body)); return new RigidBody(session, body, concrete); });
    }
    /// <summary>在指定实体上创建盒体刚体或触发器。</summary>
    public RigidBody CreateBox(F_Entity entity, BoxBodyDescription description, bool isTrigger)
    {
        var body = CreateBox(entity, description);
        if (!isTrigger) return body;
        try { body.IsTrigger = true; return body; }
        catch { body.Destroy(); throw; }
    }
    /// <summary>在指定实体上创建球体碰撞刚体。</summary>
    public RigidBody CreateSphere(F_Entity entity, SphereBodyDescription description)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        var concrete = entity as Entity ?? new Entity(session, entity.Handle);
        return session.Invoke(handle => { EngineSession.Check(NativeMethods.CreateSphereBody(handle, concrete.Handle, in description, out var body)); return new RigidBody(session, body, concrete); });
    }
    /// <summary>获取实体上已有的刚体实例（含编辑器挂载的 PhysicsBody 组件），不存在时返回 null。</summary>
    public RigidBody? GetBody(F_Entity entity)
    {
        if(entity.Session!=session)throw new ArgumentException("Bridge.InvalidEntity");
        var concrete = entity as Entity ?? new Entity(session, entity.Handle);
        return session.Invoke(handle=>{EngineSession.Check(NativeMethods.FindBody(handle,concrete.Handle,out var body));return body==0?null:new RigidBody(session,body,concrete);});
    }
    /// <summary>手动推进单步物理模拟（仅在非托管时钟接管模式下可用）。</summary>
    public void Step(double seconds)
    {
        if (ClockOwner is not null) throw new InvalidOperationException("Script.PhysicsClockOwned");
        StepInternal(seconds);
    }
    internal void StepInternal(double seconds) => session.Invoke(handle => { EngineSession.Check(NativeMethods.PhysicsStep(handle, seconds)); return 0; });
    /// <summary>按帧时长累加推进固定步物理模拟。</summary>
    public uint Advance(double frameSeconds)
    {
        if (ClockOwner is not null) throw new InvalidOperationException("Script.PhysicsClockOwned");
        return session.Invoke(handle => { EngineSession.Check(NativeMethods.PhysicsAdvance(handle, frameSeconds, out var steps, out var dropped)); DroppedSeconds = dropped; return steps; });
    }
    /// <summary>向物理世界发射射线并返回最近命中点（<paramref name="mask"/> 为 32 位碰撞层掩码，未命中返回 null）。</summary>
    public RayHit? Raycast(Vector3 origin, Vector3 direction, float distance, uint mask = uint.MaxValue) => session.Invoke<RayHit?>(handle =>
    {
        EngineSession.Check(NativeMethods.Raycast(handle, origin, direction, distance, mask, out var hit, out var found));
        if (found == 0) return null;
        var entity = session.FindEntity(hit.EntityStableId);
        return entity is null ? null : new RayHit(entity, new(session, hit.Body, entity), hit.Point, hit.Normal, hit.Distance);
    });
    public IReadOnlyList<PhysicsContact> DrainContacts() => session.Invoke<IReadOnlyList<PhysicsContact>>(handle =>
    {
        var result = NativeMethods.DrainContacts(handle, null, 0, out var required);
        if (result != 3) EngineSession.Check(result);
        if (required == 0) return [];
        var events = new NativeContact[required];
        fixed (NativeContact* buffer = events) EngineSession.Check(NativeMethods.DrainContacts(handle, buffer, required, out _));
        return events.Select(e => new PhysicsContact(new(e.EntityA), new(e.EntityB), e.Began != 0, e.Ended != 0, e.Point, e.Normal, e.Impulse)).ToArray();
    });
    public IReadOnlyList<PhysicsTrigger> DrainTriggers() => session.Invoke<IReadOnlyList<PhysicsTrigger>>(handle =>
    {
        var result = NativeMethods.DrainTriggers(handle, null, 0, out var required);
        if (result != 3) EngineSession.Check(result);
        if (required == 0) return [];
        var events = new NativeTrigger[required];
        fixed (NativeTrigger* buffer = events) EngineSession.Check(NativeMethods.DrainTriggers(handle, buffer, required, out _));
        return events.Select(e => new PhysicsTrigger(new(e.TriggerEntity), new(e.OtherEntity), e.Entered != 0)).ToArray();
    });
}

internal static unsafe partial class NativeMethods
{
    [LibraryImport(Library, EntryPoint = "flare_physics_set_debug_draw")]
    internal static partial int SetPhysicsDebugDraw(ulong session,int enabled,out int previous);
    [LibraryImport(Library, EntryPoint = "flare_body_create")]
    internal static partial int CreateBody(ulong session, ulong entity, in BoxBodyDescription description, out ulong body);
    [LibraryImport(Library, EntryPoint = "flare_body_create_sphere")]
    internal static partial int CreateSphereBody(ulong session, ulong entity, in SphereBodyDescription description, out ulong body);
    [LibraryImport(Library, EntryPoint = "flare_body_set_trigger")]
    internal static partial int SetBodyTrigger(ulong session, ulong body, int isTrigger);
    [LibraryImport(Library, EntryPoint = "flare_body_is_trigger")]
    internal static partial int GetBodyTrigger(ulong session, ulong body, out int isTrigger);
    [LibraryImport(Library, EntryPoint = "flare_body_destroy")]
    internal static partial int DestroyBody(ulong session, ulong body);
    [LibraryImport(Library, EntryPoint = "flare_body_find")]
    internal static partial int FindBody(ulong session,ulong entity,out ulong body);
    [LibraryImport(Library, EntryPoint = "flare_body_get_state")]
    internal static partial int GetBodyState(ulong session, ulong body, out RigidBodyState state);
    [LibraryImport(Library, EntryPoint = "flare_body_set_velocity")]
    internal static partial int SetBodyVelocity(ulong session, ulong body, Vector3 linear, Vector3 angular);
    [LibraryImport(Library, EntryPoint = "flare_body_apply_force")]
    internal static partial int ApplyBodyForce(ulong session, ulong body, Vector3 force, Vector3 point, int impulse);
    [LibraryImport(Library, EntryPoint = "flare_body_teleport")]
    internal static partial int TeleportBody(ulong session, ulong body, in Transform transform, int clearVelocity);
    [LibraryImport(Library, EntryPoint = "flare_physics_step")]
    internal static partial int PhysicsStep(ulong session, double seconds);
    [LibraryImport(Library, EntryPoint = "flare_physics_advance")]
    internal static partial int PhysicsAdvance(ulong session, double seconds, out uint steps, out double dropped);
    [LibraryImport(Library, EntryPoint = "flare_physics_raycast")]
    internal static partial int Raycast(ulong session, Vector3 origin, Vector3 direction, float distance, uint mask, out NativeRayHit hit, out int found);
    [LibraryImport(Library, EntryPoint = "flare_physics_drain_contacts")]
    internal static partial int DrainContacts(ulong session, NativeContact* events, uint capacity, out uint required);
    [LibraryImport(Library, EntryPoint = "flare_physics_drain_triggers")]
    internal static partial int DrainTriggers(ulong session, NativeTrigger* events, uint capacity, out uint required);
}
