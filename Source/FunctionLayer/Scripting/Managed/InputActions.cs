using System.Numerics;
using System.Text;

namespace FlareEngine;

public sealed class InputActions
{
    private readonly EngineSession session;

    internal InputActions(EngineSession session) => this.session = session;

    public void LoadJson(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        session.Invoke(value =>
        {
            EngineSession.Check(NativeMethods.InputLoadActions(value, json, checked((uint)Encoding.UTF8.GetByteCount(json))));
            return 0;
        });
    }

    public bool IsActionDown(string action)
    {
        ArgumentException.ThrowIfNullOrEmpty(action);
        return session.Invoke(value =>
        {
            EngineSession.Check(NativeMethods.InputActionButton(value, action, checked((uint)Encoding.UTF8.GetByteCount(action)), out var down, out _));
            return down != 0;
        });
    }

    public bool IsActionPressed(string action)
    {
        ArgumentException.ThrowIfNullOrEmpty(action);
        return session.Invoke(value =>
        {
            EngineSession.Check(NativeMethods.InputActionButton(value, action, checked((uint)Encoding.UTF8.GetByteCount(action)), out _, out var pressed));
            return pressed != 0;
        });
    }

    public float GetAxis1D(string action)
    {
        ArgumentException.ThrowIfNullOrEmpty(action);
        return session.Invoke(value =>
        {
            EngineSession.Check(NativeMethods.InputActionAxis1D(value, action, checked((uint)Encoding.UTF8.GetByteCount(action)), out var axis));
            return axis;
        });
    }

    public Vector2 GetAxis2D(string action)
    {
        ArgumentException.ThrowIfNullOrEmpty(action);
        return session.Invoke(value =>
        {
            EngineSession.Check(NativeMethods.InputActionAxis2D(value, action, checked((uint)Encoding.UTF8.GetByteCount(action)), out var axis));
            return axis;
        });
    }
}
