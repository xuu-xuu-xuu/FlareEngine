using System.Numerics;
using System.Runtime.InteropServices;

namespace FlareEngine;

/// <summary>角色胶囊体控制器参数描述。</summary>
[StructLayout(LayoutKind.Sequential)]
public struct F_CharacterDescription
{
    public float Radius, SegmentHalfHeight, WalkSpeed, JumpSpeed, MaximumPushImpulse;
    public static F_CharacterDescription Default => new() { Radius = 0.3f, SegmentHalfHeight = 0.6f, WalkSpeed = 4, JumpSpeed = 5, MaximumPushImpulse = 0.5f };
    public static implicit operator CharacterDescription(F_CharacterDescription v) => new()
    { Radius = v.Radius, SegmentHalfHeight = v.SegmentHalfHeight, WalkSpeed = v.WalkSpeed, JumpSpeed = v.JumpSpeed, MaximumPushImpulse = v.MaximumPushImpulse };
    public static implicit operator F_CharacterDescription(CharacterDescription v) => new()
    { Radius = v.Radius, SegmentHalfHeight = v.SegmentHalfHeight, WalkSpeed = v.WalkSpeed, JumpSpeed = v.JumpSpeed, MaximumPushImpulse = v.MaximumPushImpulse };
}

[StructLayout(LayoutKind.Sequential)]
public struct CharacterDescription
{
    public float Radius, SegmentHalfHeight, WalkSpeed, JumpSpeed, MaximumPushImpulse;
    public static CharacterDescription Default => new() { Radius = 0.3f, SegmentHalfHeight = 0.6f, WalkSpeed = 4, JumpSpeed = 5, MaximumPushImpulse = 0.5f };
}
[StructLayout(LayoutKind.Sequential)]
internal struct NativeCharacterState { internal Vector3 Position; internal float VerticalVelocity; internal int Grounded; }
/// <summary>胶囊体角色控制器运动状态（当前位置、竖直速度与着地标志 <see cref="Grounded"/>）。</summary>
public readonly record struct F_CharacterState(Vector3 Position, float VerticalVelocity, bool Grounded)
{
    public static implicit operator CharacterState(F_CharacterState v) => new(v.Position, v.VerticalVelocity, v.Grounded);
    public static implicit operator F_CharacterState(CharacterState v) => new(v.Position, v.VerticalVelocity, v.Grounded);
}
/// <summary>胶囊体角色控制器运动状态（兼容无前缀写法）。</summary>
public readonly record struct CharacterState(Vector3 Position, float VerticalVelocity, bool Grounded);

/// <summary>运动学胶囊角色控制器组件代理（支持斜坡滑移、跳跃与对动态刚体施加有限推力）。</summary>
public class F_CharacterController
{
    private bool destroyed;
    private void RequireAlive()
    {
        // 保持原 C ABI 的 FLARE_INVALID_HANDLE=2 与稳定错误键，旧调用者可沿用异常处理。
        if(destroyed)throw new EngineException("Bridge.InvalidCharacter",2);
    }
    /// <summary>所属场景实体。</summary>
    public Entity Entity { get; }
    private CharacterState currentState;
    private readonly bool authored;
    /// <summary>读取角色控制器最新位置、竖直速度与是否着地（Grounded）。</summary>
    public CharacterState State
    {
        get
        {
            if(!authored)return currentState;
            RequireAlive();
            return Entity.Session.Invoke(handle=>{EngineSession.Check(NativeMethods.CharacterComponentState(handle,Entity.Handle,out var state));return new CharacterState(state.Position,state.VerticalVelocity,state.Grounded!=0);});
        }
        private set=>currentState=value;
    }
    internal F_CharacterController(Entity entity,NativeCharacterState state)
    {Entity=entity;authored=true;State=new(state.Position,state.VerticalVelocity,state.Grounded!=0);}
    /// <summary>绑定实体上已挂载的编辑器角色控制器组件（也可直接使用 <c>GetComponent&lt;F_CharacterController&gt;()</c>）。</summary>
    public static CharacterController FromComponent(F_Entity entity)
    {
        var concrete = entity as Entity ?? new Entity(entity.Session, entity.Handle);
        return entity.Session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.CharacterComponentState(handle, concrete.Handle, out var state));
            return new CharacterController(concrete, state);
        });
    }
    /// <summary>在实体上动态创建并绑定角色控制器。</summary>
    public F_CharacterController(F_Entity entity, CharacterDescription description)
    {
        var concrete = entity as Entity ?? new Entity(entity.Session, entity.Handle);
        Entity = concrete;
        concrete.Session.Invoke(session => { EngineSession.Check(NativeMethods.CharacterCreate(session, concrete.Handle, in description)); return 0; });
        State = new(concrete.Transform.Position, 0, false);
    }
    /// <summary>在 <see cref="F_Behaviour.F_FixedUpdate()"/> 中按水平方向输入与跳跃请求移动角色。</summary>
    public CharacterState Move(Vector3 horizontalDirection, bool jumpPressed, double seconds)
    {
        RequireAlive();
        State = Entity.Session.Invoke(session =>
        {
            EngineSession.Check(NativeMethods.CharacterMove(session, Entity.Handle, horizontalDirection, jumpPressed ? 1 : 0, (float)seconds, out var state));
            return new CharacterState(state.Position, state.VerticalVelocity, state.Grounded != 0);
        });
        return State;
    }
    /// <summary>将角色控制器瞬移到指定世界坐标并清零竖直速度。</summary>
    public void Teleport(Vector3 position)
    {
        RequireAlive();
        Entity.Session.Invoke(session => { EngineSession.Check(NativeMethods.CharacterTeleport(session, Entity.Handle, position)); return 0; });
        State = new(position, 0, false);
    }
    /// <summary>销毁角色控制器组件代理。</summary>
    public void Destroy()
    {
        if(destroyed)return;
        try{Entity.Session.Invoke(session=>{EngineSession.Check(NativeMethods.CharacterDestroy(session,Entity.Handle));return 0;});}
        catch(EngineException exception)when(exception.Key=="Bridge.InvalidCharacter"&&(Entity.Session.IsSceneRetiring(Entity.Handle)||!Entity.IsAlive)){/* 场景/GO 已释放控制器代理。 */}
        destroyed=true;
    }
}

/// <summary>运动学胶囊角色控制器组件代理（兼容无前缀写法，继承自 <see cref="F_CharacterController"/>）。</summary>
public sealed class CharacterController : F_CharacterController
{
    internal CharacterController(Entity entity, NativeCharacterState state) : base(entity, state) { }
    /// <summary>在实体上动态创建并绑定角色控制器。</summary>
    public CharacterController(F_Entity entity, CharacterDescription description) : base(entity, description) { }
}
internal static partial class NativeMethods
{
    [LibraryImport(Library, EntryPoint="flare_character_component_state")]
    internal static partial int CharacterComponentState(ulong session,ulong entity,out NativeCharacterState state);
    [LibraryImport(Library, EntryPoint="flare_character_create")]
    internal static partial int CharacterCreate(ulong session,ulong entity,in CharacterDescription description);
    [LibraryImport(Library, EntryPoint="flare_character_move")]
    internal static partial int CharacterMove(ulong session,ulong entity,Vector3 direction,int jump,float seconds,out NativeCharacterState state);
    [LibraryImport(Library, EntryPoint="flare_character_teleport")]
    internal static partial int CharacterTeleport(ulong session,ulong entity,Vector3 position);
    [LibraryImport(Library, EntryPoint="flare_character_destroy")]
    internal static partial int CharacterDestroy(ulong session,ulong entity);
}
