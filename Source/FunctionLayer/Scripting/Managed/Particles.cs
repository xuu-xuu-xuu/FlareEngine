namespace FlareEngine;

/// <summary>粒子发射器运行时控制服务。</summary>
public class F_Particles
{
    private readonly EngineSession session;

    internal F_Particles(EngineSession session) => this.session = session;

    /// <summary>当前场景中的粒子发射器总数。</summary>
    public uint EmitterCount => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.ParticlesEmitterCount(handle, out var count));
        return count;
    });

    /// <summary>在指定实体的粒子发射器上瞬时迸发生成 <paramref name="count"/> 颗粒子，返回实际生成数量。</summary>
    public uint EmitBurst(F_Entity entity, uint count)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.ParticlesEmitBurst(handle, entity.Handle, count, out var spawned));
            return spawned;
        });
    }

    /// <summary>启动或暂停指定实体粒子发射器的持续发射。</summary>
    public void SetPlaying(F_Entity entity, bool playing)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.ParticlesSetPlaying(handle, entity.Handle, playing ? 1 : 0));
            return 0;
        });
    }

    /// <summary>清空指定实体（或传入 null 清空全场）已生成的存活粒子。</summary>
    public void Clear(F_Entity? entity = null)
    {
        if (entity is not null && entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.ParticlesClear(handle, entity?.Handle ?? 0));
            return 0;
        });
    }

    /// <summary>统计指定实体（或全场）当前存活的粒子数量。</summary>
    public uint ActiveCount(F_Entity? entity = null)
    {
        if (entity is not null && entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.ParticlesActiveCount(handle, entity?.Handle ?? 0, out var count));
            return count;
        });
    }
}

/// <summary>粒子发射器运行时控制服务（兼容无前缀写法，继承自 <see cref="F_Particles"/>）。</summary>
public sealed class Particles : F_Particles
{
    internal Particles(EngineSession session) : base(session) { }
}
