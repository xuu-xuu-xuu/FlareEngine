using System.Numerics;
using System.Text;

namespace FlareEngine;

/// <summary>可配置输入动作表（.inputactions.json）查询服务。</summary>
public class F_InputActions
{
    private readonly EngineSession session;

    internal F_InputActions(EngineSession session) => this.session = session;

    /// <summary>从 JSON 文本加载并激活输入动作映射配置。</summary>
    public void LoadJson(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        session.Invoke(value =>
        {
            EngineSession.Check(NativeMethods.InputLoadActions(value, json, checked((uint)Encoding.UTF8.GetByteCount(json))));
            return 0;
        });
    }

    /// <summary>查询指定动作在当前帧是否处于按住状态。</summary>
    public bool IsActionDown(string action)
    {
        ArgumentException.ThrowIfNullOrEmpty(action);
        return session.Invoke(value =>
        {
            EngineSession.Check(NativeMethods.InputActionButton(value, action, checked((uint)Encoding.UTF8.GetByteCount(action)), out var down, out _));
            return down != 0;
        });
    }

    /// <summary>查询指定动作在本帧是否刚刚触发按下。</summary>
    public bool IsActionPressed(string action)
    {
        ArgumentException.ThrowIfNullOrEmpty(action);
        return session.Invoke(value =>
        {
            EngineSession.Check(NativeMethods.InputActionButton(value, action, checked((uint)Encoding.UTF8.GetByteCount(action)), out _, out var pressed));
            return pressed != 0;
        });
    }

    /// <summary>读取指定一维连续输入动作的当前值。</summary>
    public float GetAxis1D(string action)
    {
        ArgumentException.ThrowIfNullOrEmpty(action);
        return session.Invoke(value =>
        {
            EngineSession.Check(NativeMethods.InputActionAxis1D(value, action, checked((uint)Encoding.UTF8.GetByteCount(action)), out var axis));
            return axis;
        });
    }

    /// <summary>读取指定二维复合输入动作（如移动摇杆/WASD）的当前向量。</summary>
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

/// <summary>可配置输入动作表查询服务（兼容无前缀写法，继承自 <see cref="F_InputActions"/>）。</summary>
public sealed class InputActions : F_InputActions
{
    internal InputActions(EngineSession session) : base(session) { }
}
