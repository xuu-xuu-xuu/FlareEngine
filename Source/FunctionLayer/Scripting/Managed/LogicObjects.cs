using System.Runtime.InteropServices;

namespace SelfEngine;

/// <summary>逻辑状态的弱引用。跨收集点保存时可显式 Retain，释放令牌后允许回收。</summary>
public sealed class LogicState
{
    private readonly EngineSession session;
    private readonly ulong token;
    internal LogicState(EngineSession owner, ulong value) { session = owner;token = value; }
    public double Value
    {
        get => session.LogicOperation(1, token).value;
        set => session.LogicOperation(2, token, value: value);
    }
    public void Link(LogicState? next)
    {
        if (next is not null && !ReferenceEquals(session, next.session))
            throw new ArgumentException("Bridge.InvalidHandle", nameof(next));
        session.LogicOperation(3, token, next?.token ?? 0);
    }
    public IDisposable Retain() => new LogicRoot(session, session.LogicOperation(4, token).token);
    private sealed class LogicRoot(EngineSession session, ulong token) : IDisposable
    {
        private bool released;
        public void Dispose()
        {
            if (released) return;
            if (!session.IsDisposed) session.LogicOperation(5, token);
            released = true;
        }
    }
}

public sealed unsafe partial class EngineSession
{
    public LogicState CreateLogicState(double value = 0) => new(this, LogicOperation(0, value: value).token);
    /// <summary>仅在世界更新完成的安全点调用，返回收集后的存活对象数。</summary>
    public int CollectLogicObjects() => checked((int)LogicOperation(6).value);
    internal (ulong token, double value) LogicOperation(uint operation, ulong token = 0, ulong target = 0, double value = 0)
        => Invoke(handle => {
            Check(NativeMethods.LogicOperation(handle, operation, token, target, value, out var result, out var scalar));
            return (result, scalar);
        });
}

internal static unsafe partial class NativeMethods
{
    [LibraryImport(Library, EntryPoint = "se_logic_operation")]
    internal static partial int LogicOperation(ulong session, uint operation, ulong token, ulong target,
        double value, out ulong result, out double scalar);
}
