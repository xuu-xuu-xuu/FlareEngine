namespace FlareEngine;

public sealed class Particles
{
    private readonly EngineSession session;

    internal Particles(EngineSession session) => this.session = session;

    public uint EmitterCount => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.ParticlesEmitterCount(handle, out var count));
        return count;
    });

    public uint EmitBurst(Entity entity, uint count)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.ParticlesEmitBurst(handle, entity.Handle, count, out var spawned));
            return spawned;
        });
    }

    public void SetPlaying(Entity entity, bool playing)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.ParticlesSetPlaying(handle, entity.Handle, playing ? 1 : 0));
            return 0;
        });
    }

    public void Clear(Entity? entity = null)
    {
        if (entity is not null && entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.ParticlesClear(handle, entity?.Handle ?? 0));
            return 0;
        });
    }

    public uint ActiveCount(Entity? entity = null)
    {
        if (entity is not null && entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.ParticlesActiveCount(handle, entity?.Handle ?? 0, out var count));
            return count;
        });
    }
}
