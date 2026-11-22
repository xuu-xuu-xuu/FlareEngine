using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Numerics;
using System.Runtime.CompilerServices;

namespace FlareEngine;

/// <summary>常用键盘按键枚举（与标准虚拟键码对齐，供 <see cref="F_Input"/> 查询）。</summary>
public enum F_Key
{
    None = 0,
    Backspace = 8,
    Tab = 9,
    Enter = 13,
    Shift = 16,
    Control = 17,
    Alt = 18,
    Escape = 27,
    Space = 32,
    PageUp = 33,
    PageDown = 34,
    End = 35,
    Home = 36,
    Left = 37,
    Up = 38,
    Right = 39,
    Down = 40,
    Insert = 45,
    Delete = 46,
    D0 = 48, D1 = 49, D2 = 50, D3 = 51, D4 = 52,
    D5 = 53, D6 = 54, D7 = 55, D8 = 56, D9 = 57,
    A = 65, B = 66, C = 67, D = 68, E = 69, F = 70, G = 71,
    H = 72, I = 73, J = 74, K = 75, L = 76, M = 77, N = 78,
    O = 79, P = 80, Q = 81, R = 82, S = 83, T = 84, U = 85,
    V = 86, W = 87, X = 88, Y = 89, Z = 90,
    F1 = 112, F2 = 113, F3 = 114, F4 = 115, F5 = 116, F6 = 117,
    F7 = 118, F8 = 119, F9 = 120, F10 = 121, F11 = 122, F12 = 123,
}

/// <summary>全局帧时钟与时间缩放服务（对标 Unity Time）。</summary>
public static class F_Time
{
    private static float timeScale = 1f;
    /// <summary>上一帧到当前帧经过缩放后的间隔时间（秒）。</summary>
    public static float DeltaTime { get; internal set; }
    /// <summary>不受 <see cref="TimeScale"/> 影响的真实帧间隔时间（秒）。</summary>
    public static float UnscaledDeltaTime { get; internal set; }
    /// <summary>固定物理模拟步长（秒，默认 1/60）。</summary>
    public static float FixedDeltaTime { get; internal set; } = 1f / 60f;
    /// <summary>自进入运行态以来经过缩放的累计时间（秒）。</summary>
    public static float Time { get; internal set; }
    /// <summary>自进入运行态以来不受缩放影响的真实累计时间（秒）。</summary>
    public static float UnscaledTime { get; internal set; }
    /// <summary>自进入运行态以来已推进的总帧数。</summary>
    public static ulong FrameCount { get; internal set; }
    /// <summary>全局时间缩放倍率（1 为正常速度，0 为暂停时间推进，上限 100）。</summary>
    public static float TimeScale
    {
        get => timeScale;
        set => timeScale = float.IsFinite(value) && value >= 0f ? MathF.Min(value, 100f) : 1f;
    }

    internal static void Reset(float fixedStep = 1f / 60f)
    {
        timeScale = 1f;
        DeltaTime = 0f;
        UnscaledDeltaTime = 0f;
        FixedDeltaTime = fixedStep;
        Time = 0f;
        UnscaledTime = 0f;
        FrameCount = 0;
    }

    internal static void AdvanceFrame(float unscaledSeconds)
    {
        if (!float.IsFinite(unscaledSeconds) || unscaledSeconds < 0f) unscaledSeconds = 0f;
        UnscaledDeltaTime = unscaledSeconds;
        DeltaTime = unscaledSeconds * timeScale;
        UnscaledTime += unscaledSeconds;
        Time += DeltaTime;
        ++FrameCount;
    }
}

/// <summary>全局键鼠即时输入查询接口（在 <see cref="F_Behaviour.F_Update()"/> 中调用）。</summary>
public static class F_Input
{
    private static ScriptInput Active => ScriptSystem.Current?.Input ?? ScriptInput.Empty;

    /// <summary>指定按键在当前帧处于持续按住状态时返回 true。</summary>
    public static bool Key(F_Key key) => Active.Down((int)key);
    /// <summary>指定按键在本帧刚刚按下时返回 true。</summary>
    public static bool KeyDown(F_Key key) => Active.Pressed((int)key);
    /// <summary>指定按键在本帧刚刚抬起时返回 true。</summary>
    public static bool KeyUp(F_Key key) => Active.Released((int)key);
    /// <summary>指定鼠标按键（默认左键）处于持续按住状态时返回 true。</summary>
    public static bool Mouse(MouseButton button = MouseButton.Left) => Active.Pointer.Down(button);
    /// <summary>指定鼠标按键处于持续按住状态时返回 true。</summary>
    public static bool Mouse(F_MouseButton button) => Active.Pointer.Down((MouseButton)button);
    /// <summary>指定鼠标按键（默认左键）在本帧刚刚按下时返回 true。</summary>
    public static bool MouseDown(MouseButton button = MouseButton.Left) => Active.Pointer.Pressed(button);
    /// <summary>指定鼠标按键在本帧刚刚按下时返回 true。</summary>
    public static bool MouseDown(F_MouseButton button) => Active.Pointer.Pressed((MouseButton)button);
    /// <summary>指定鼠标按键（默认左键）在本帧刚刚松开时返回 true。</summary>
    public static bool MouseUp(MouseButton button = MouseButton.Left) => Active.Pointer.Released(button);
    /// <summary>指定鼠标按键在本帧刚刚松开时返回 true。</summary>
    public static bool MouseUp(F_MouseButton button) => Active.Pointer.Released((MouseButton)button);
    /// <summary>鼠标在客户区内的像素坐标（左上角为原点）。</summary>
    public static Vector2 MousePosition => Active.Pointer.Position;
    /// <summary>当前帧鼠标原始相对位移（像素），适合第一/第三人称自由视角旋转。</summary>
    public static Vector2 MouseDelta => ScriptSystem.Current?.Session.MouseDelta ?? Vector2.Zero;
    /// <summary>当前帧鼠标滚轮增量。</summary>
    public static float MouseWheel => Active.Pointer.Wheel;

    /// <summary>根据负向键与正向键组合返回 -1、0 或 +1 的一维输入轴。</summary>
    public static float Axis(F_Key negative, F_Key positive)
        => (Key(positive) ? 1f : 0f) - (Key(negative) ? 1f : 0f);

    /// <summary>水平移动输入轴（A/D 或 Left/Right，范围 -1..+1）。</summary>
    public static float Horizontal
    {
        get
        {
            float v = Axis(F_Key.A, F_Key.D);
            return v != 0f ? v : Axis(F_Key.Left, F_Key.Right);
        }
    }

    /// <summary>前后/垂直移动输入轴（S/W 或 Down/Up，范围 -1..+1）。</summary>
    public static float Vertical
    {
        get
        {
            float v = Axis(F_Key.S, F_Key.W);
            return v != 0f ? v : Axis(F_Key.Down, F_Key.Up);
        }
    }

    /// <summary>经斜向长度归一化的二维平面移动输入向量（模长不超过 1）。</summary>
    public static Vector2 MoveVector
    {
        get
        {
            var v = new Vector2(Horizontal, Vertical);
            return v.LengthSquared() > 1f ? Vector2.Normalize(v) : v;
        }
    }
}

/// <summary>鼠标光标锁定与显隐控制（第一人称视角常用 <c>F_Cursor.Locked = true</c>）。</summary>
public static class F_Cursor
{
    /// <summary>获取或设置是否将鼠标锁定在窗口中央（设为 true 时自动隐藏光标）。</summary>
    public static bool Locked
    {
        get => ScriptSystem.Current?.Session.CursorMode.Locked ?? false;
        set
        {
            if (ScriptSystem.Current?.Session is { } s)
                s.SetCursorMode(value, value ? false : (s.CursorMode.Locked ? true : s.CursorMode.Visible));
        }
    }

    /// <summary>获取或设置鼠标系统光标是否可见。</summary>
    public static bool Visible
    {
        get => ScriptSystem.Current?.Session.CursorMode.Visible ?? true;
        set
        {
            if (ScriptSystem.Current?.Session is { } s)
                s.SetCursorMode(s.CursorMode.Locked, value);
        }
    }
}

/// <summary>三维空间射线（起点 <see cref="Origin"/> 与单位方向 <see cref="Direction"/>）。</summary>
public readonly record struct F_Ray(Vector3 Origin, Vector3 Direction)
{
    /// <summary>获取沿射线前进指定距离（米）处的世界坐标点。</summary>
    public Vector3 GetPoint(float distance) => Origin + Direction * distance;
}

/// <summary>摄像机空间投影工具类。</summary>
public static class F_Camera
{
    /// <summary>将屏幕像素坐标转换为从指定摄像机（缺省使用主摄像机）发出的世界空间射线。</summary>
    public static F_Ray? ScreenPointToRay(Vector2 screenPixel, F_Entity? camera = null)
    {
        var session = camera?.Session ?? ScriptSystem.Current?.Session;
        if (session is null) return null;
        return session.ScreenPointToRay(camera?.StableId ?? 0, screenPixel) is { } r
            ? new F_Ray(r.Origin, r.Direction) : null;
    }
}

/// <summary>游戏开发高频数学工具函数（插值、平滑阻尼、角度环绕与夹取）。</summary>
public static class F_Math
{
    /// <summary>圆周率常量 π。</summary>
    public const float PI = MathF.PI;
    /// <summary>角度转弧度乘子（π / 180）。</summary>
    public const float Deg2Rad = MathF.PI / 180f;
    /// <summary>弧度转角度乘子（180 / π）。</summary>
    public const float Rad2Deg = 180f / MathF.PI;

    /// <summary>将浮点数限制在 [min, max] 区间内。</summary>
    public static float Clamp(float value, float min, float max) => Math.Clamp(value, min, max);
    /// <summary>将整数限制在 [min, max] 区间内。</summary>
    public static int Clamp(int value, int min, int max) => Math.Clamp(value, min, max);
    /// <summary>将浮点数限制在 [0, 1] 区间内。</summary>
    public static float Clamp01(float value) => Math.Clamp(value, 0f, 1f);
    /// <summary>在 <paramref name="a"/> 与 <paramref name="b"/> 之间按夹取到 [0,1] 的权重 <paramref name="t"/> 线性插值。</summary>
    public static float Lerp(float a, float b, float t) => a + (b - a) * Clamp01(t);
    /// <summary>不夹取权重 <paramref name="t"/> 的无界线性插值。</summary>
    public static float LerpUnclamped(float a, float b, float t) => a + (b - a) * t;
    /// <summary>在两个三维向量之间按夹取到 [0,1] 的权重线性插值。</summary>
    public static Vector3 Lerp(Vector3 a, Vector3 b, float t) => Vector3.Lerp(a, b, Clamp01(t));

    /// <summary>计算 <paramref name="value"/> 在区间 [a, b] 中所处的归一化比例（0..1）。</summary>
    public static float InverseLerp(float a, float b, float value)
        => MathF.Abs(b - a) > 1e-6f ? Clamp01((value - a) / (b - a)) : 0f;

    /// <summary>以不超过 <paramref name="maxDelta"/> 的步幅将当前值推向目标值。</summary>
    public static float MoveTowards(float current, float target, float maxDelta)
        => MathF.Abs(target - current) <= maxDelta ? target : current + MathF.Sign(target - current) * maxDelta;

    /// <summary>以不超过 <paramref name="maxDistanceDelta"/> 的距离将当前点移向目标点。</summary>
    public static Vector3 MoveTowards(Vector3 current, Vector3 target, float maxDistanceDelta)
    {
        var delta = target - current;
        var sq = delta.LengthSquared();
        if (sq == 0f || (maxDistanceDelta >= 0f && sq <= maxDistanceDelta * maxDistanceDelta)) return target;
        return current + delta / MathF.Sqrt(sq) * maxDistanceDelta;
    }

    /// <summary>将数值 <paramref name="t"/> 循环折叠在 [0, length) 周期区间内。</summary>
    public static float Repeat(float t, float length)
        => length <= 1e-6f ? 0f : Clamp(t - MathF.Floor(t / length) * length, 0f, length);

    /// <summary>使数值 <paramref name="t"/> 在 0 与 <paramref name="length"/> 之间往返折返。</summary>
    public static float PingPong(float t, float length)
    {
        t = Repeat(t, length * 2f);
        return length - MathF.Abs(t - length);
    }

    /// <summary>计算两个角度（度）之间的最短有符号夹角差（范围 -180..180）。</summary>
    public static float DeltaAngle(float current, float target)
    {
        float delta = Repeat(target - current, 360f);
        return delta > 180f ? delta - 360f : delta;
    }

    /// <summary>按最短圆弧方向在两个角度（度）之间线性插值（自动跨越 360 度边界）。</summary>
    public static float LerpAngle(float a, float b, float t) => a + DeltaAngle(a, b) * Clamp01(t);

    /// <summary>判断两个浮点数之差的绝对值是否小于给定容差 <paramref name="epsilon"/>。</summary>
    public static bool Approximately(float a, float b, float epsilon = 1e-5f) => MathF.Abs(a - b) <= epsilon;

    /// <summary>临界阻尼弹簧平滑过渡标量（帧率无关，适合数值缓动）。</summary>
    public static float SmoothDamp(float current, float target, ref float currentVelocity, float smoothTime,
        float maxSpeed = float.PositiveInfinity, float? deltaTime = null)
    {
        float dt = deltaTime ?? (F_Time.DeltaTime > 0f ? F_Time.DeltaTime : (1f / 60f));
        smoothTime = MathF.Max(0.0001f, smoothTime);
        float omega = 2f / smoothTime;
        float x = omega * dt;
        float exp = 1f / (1f + x + 0.48f * x * x + 0.235f * x * x * x);
        float change = current - target;
        float originalTo = target;
        float maxChange = maxSpeed * smoothTime;
        change = Clamp(change, -maxChange, maxChange);
        target = current - change;
        float temp = (currentVelocity + omega * change) * dt;
        currentVelocity = (currentVelocity - omega * temp) * exp;
        float output = target + (change + temp) * exp;
        if ((originalTo - current > 0f) == (output > originalTo))
        {
            output = originalTo;
            currentVelocity = dt > 1e-6f ? (output - originalTo) / dt : 0f;
        }
        return output;
    }

    /// <summary>临界阻尼弹簧平滑过渡三维向量（适合相机平滑跟随）。</summary>
    public static Vector3 SmoothDamp(Vector3 current, Vector3 target, ref Vector3 currentVelocity, float smoothTime,
        float maxSpeed = float.PositiveInfinity, float? deltaTime = null)
    {
        float vx = currentVelocity.X, vy = currentVelocity.Y, vz = currentVelocity.Z;
        var res = new Vector3(
            SmoothDamp(current.X, target.X, ref vx, smoothTime, maxSpeed, deltaTime),
            SmoothDamp(current.Y, target.Y, ref vy, smoothTime, maxSpeed, deltaTime),
            SmoothDamp(current.Z, target.Z, ref vz, smoothTime, maxSpeed, deltaTime));
        currentVelocity = new Vector3(vx, vy, vz);
        return res;
    }
}

/// <summary>右手系四元数旋转构造与欧拉角转换工具类。</summary>
public static class F_Quat
{
    /// <summary>无旋转单位四元数。</summary>
    public static Quaternion Identity => Quaternion.Identity;

    /// <summary>按与编辑器属性检视面板一致的右手系欧拉角（角度制）构造单位四元数。</summary>
    public static Quaternion Euler(float xDegrees, float yDegrees, float zDegrees)
    {
        var qx = Quaternion.CreateFromAxisAngle(Vector3.UnitX, xDegrees * F_Math.Deg2Rad);
        var qy = Quaternion.CreateFromAxisAngle(Vector3.UnitY, yDegrees * F_Math.Deg2Rad);
        var qz = Quaternion.CreateFromAxisAngle(Vector3.UnitZ, zDegrees * F_Math.Deg2Rad);
        return Quaternion.Normalize(qy * qx * qz);
    }

    /// <summary>按欧拉角向量（角度制）构造单位四元数。</summary>
    public static Quaternion Euler(Vector3 eulerDegrees) => Euler(eulerDegrees.X, eulerDegrees.Y, eulerDegrees.Z);

    /// <summary>将四元数还原为欧拉角向量（角度制）。</summary>
    public static Vector3 ToEuler(Quaternion rotation)
    {
        var q = rotation.LengthSquared() > 1e-8f ? Quaternion.Normalize(rotation) : Quaternion.Identity;
        var m = Matrix4x4.CreateFromQuaternion(q);
        // Matrix4x4 为行主序：C++ 列主序 m(r,c) 对应 m[c,r]。
        float cosine = MathF.Sqrt(m.M12 * m.M12 + m.M22 * m.M22);
        float x = MathF.Atan2(-m.M32, cosine);
        bool singular = cosine < 0.00001f;
        float y = singular ? MathF.Atan2(-m.M13, m.M11) : MathF.Atan2(m.M31, m.M33);
        float z = singular ? 0f : MathF.Atan2(m.M12, m.M22);
        return new Vector3(x, y, z) * F_Math.Rad2Deg;
    }

    /// <summary>构造将局部前方向（-Z）对准给定世界 <paramref name="forward"/> 方向的旋转四元数。</summary>
    public static Quaternion LookRotation(Vector3 forward, Vector3? worldUp = null)
    {
        if (forward.LengthSquared() < 1e-8f) return Quaternion.Identity;
        var fwd = Vector3.Normalize(forward);
        var up = worldUp ?? Vector3.UnitY;
        if (MathF.Abs(Vector3.Dot(fwd, Vector3.Normalize(up))) > 0.9995f)
            up = MathF.Abs(fwd.Y) < 0.9f ? Vector3.UnitY : Vector3.UnitZ;
        var world = Matrix4x4.CreateWorld(Vector3.Zero, fwd, up);
        return Quaternion.Normalize(Quaternion.CreateFromRotationMatrix(world));
    }

    /// <summary>在两个四元数之间按夹取到 [0,1] 的比例球面线性插值。</summary>
    public static Quaternion Slerp(Quaternion a, Quaternion b, float t) => Quaternion.Slerp(a, b, F_Math.Clamp01(t));

    /// <summary>计算两个旋转四元数之间的最小夹角（角度制，0..180）。</summary>
    public static float Angle(Quaternion a, Quaternion b)
    {
        float dot = MathF.Min(MathF.Abs(Quaternion.Dot(Quaternion.Normalize(a), Quaternion.Normalize(b))), 1f);
        return dot > 0.999999f ? 0f : MathF.Acos(dot) * 2f * F_Math.Rad2Deg;
    }

    /// <summary>以不超过 <paramref name="maxDegreesDelta"/>（度）的步幅将当前旋转朝目标旋转推进。</summary>
    public static Quaternion RotateTowards(Quaternion from, Quaternion to, float maxDegreesDelta)
    {
        float angle = Angle(from, to);
        return angle <= 1e-5f || maxDegreesDelta >= angle ? to : Quaternion.Slerp(from, to, maxDegreesDelta / angle);
    }
}

/// <summary>场景实体基类，封装空间变换、父子层级、标签图层与组件查询操作。</summary>
public partial class F_Entity
{
    private Entity AsEntity() => this as Entity ?? new Entity(Session, Handle);

    private void WriteLocalPose(Vector3? position, Quaternion? rotation)
    {
        var current = Transform;
        var nextPos = position ?? current.Position;
        var nextRot = rotation.HasValue ? Quaternion.Normalize(rotation.Value) : current.Rotation;
        if (Session.Physics.GetBody(AsEntity()) is { } body)
        {
            current.Position = nextPos;
            current.Rotation = nextRot;
            body.Teleport(current, clearVelocity: false);
            return;
        }
        if (position.HasValue && TryGetComponent<CharacterController>(out var cc))
        {
            cc.Teleport(nextPos);
            if (rotation.HasValue)
            {
                current = Transform;
                current.Rotation = nextRot;
                Transform = current;
            }
            return;
        }
        current.Position = nextPos;
        current.Rotation = nextRot;
        Transform = current;
    }

    /// <summary>判断当前实体句柄是否有效（已激活或本帧刚创建尚未退役）。</summary>
    public bool IsValid => !Session.IsDisposed && (IsAlive || IsPending);

    /// <summary>获取或设置实体的语义标签（如 Untagged、Player、MainCamera）。</summary>
    public string Tag
    {
        get => Session.GetEntityTag(Handle);
        set => Session.SetEntityTag(Handle, value);
    }

    /// <summary>获取或设置实体所在的物理/系统路由图层索引（0..31）。</summary>
    public int Layer
    {
        get => Session.GetEntityLayer(Handle);
        set => Session.SetEntityLayer(Handle, value);
    }

    /// <summary>精确比对当前实体是否带有指定标签。</summary>
    public bool CompareTag(string tag) => string.Equals(Tag, tag ?? "Untagged", StringComparison.Ordinal);

    /// <summary>获取或设置相对父节点的局部坐标（米）；若挂有刚体或角色控制器会自动安全瞬移。</summary>
    public Vector3 Position
    {
        get => Transform.Position;
        set => WriteLocalPose(value, null);
    }

    /// <summary>获取或设置相对父节点的局部旋转四元数。</summary>
    public Quaternion Rotation
    {
        get => Transform.Rotation;
        set => WriteLocalPose(null, value);
    }

    /// <summary>获取或设置实体在世界坐标系中的位置（米）。</summary>
    public Vector3 WorldPosition
    {
        get => WorldTransform.Position;
        set
        {
            if (Parent is { } parent)
            {
                var pt = parent.WorldTransform;
                var parentMatrix = Matrix4x4.CreateScale(pt.Scale)
                    * Matrix4x4.CreateFromQuaternion(Quaternion.Normalize(pt.Rotation))
                    * Matrix4x4.CreateTranslation(pt.Position);
                if (Matrix4x4.Invert(parentMatrix, out var invParent))
                {
                    Position = Vector3.Transform(value, invParent);
                    return;
                }
            }
            Position = value;
        }
    }

    /// <summary>获取或设置实体在世界坐标系中的旋转四元数。</summary>
    public Quaternion WorldRotation
    {
        get => WorldTransform.Rotation;
        set
        {
            var normalized = Quaternion.Normalize(value);
            if (Parent is { } parent)
                Rotation = Quaternion.Normalize(Quaternion.Inverse(Quaternion.Normalize(parent.WorldTransform.Rotation)) * normalized);
            else
                Rotation = normalized;
        }
    }

    /// <summary>获取或设置相对父节点的局部三轴缩放倍率。</summary>
    public Vector3 LocalScale
    {
        get => Transform.Scale;
        set { var t = Transform; t.Scale = value; Transform = t; }
    }

    /// <summary>获取或设置局部欧拉角旋转（角度制）。</summary>
    public Vector3 EulerAngles
    {
        get => F_Quat.ToEuler(Transform.Rotation);
        set => WriteLocalPose(null, F_Quat.Euler(value));
    }

    /// <summary>实体在世界空间中的单位前方向向量（右手系局部 -Z）。</summary>
    public Vector3 Forward => Vector3.Normalize(Vector3.Transform(-Vector3.UnitZ, WorldTransform.Rotation));
    /// <summary>实体在世界空间中的单位右方向向量（局部 +X）。</summary>
    public Vector3 Right => Vector3.Normalize(Vector3.Transform(Vector3.UnitX, WorldTransform.Rotation));
    /// <summary>实体在世界空间中的单位上方向向量（局部 +Y）。</summary>
    public Vector3 Up => Vector3.Normalize(Vector3.Transform(Vector3.UnitY, WorldTransform.Rotation));

    /// <summary>按位移量平移实体（默认沿局部坐标系，传入 localSpace: false 沿世界坐标系）。</summary>
    public void Translate(Vector3 delta, bool localSpace = true)
    {
        if (localSpace)
            Position += Vector3.Transform(delta, Transform.Rotation);
        else
            WorldPosition += delta;
    }

    /// <summary>按增量欧拉角（角度制）旋转实体。</summary>
    public void Rotate(Vector3 eulerDegrees, bool localSpace = true)
    {
        var dq = F_Quat.Euler(eulerDegrees);
        if (localSpace)
            Rotation = Quaternion.Normalize(Transform.Rotation * dq);
        else
            WorldRotation = Quaternion.Normalize(dq * WorldRotation);
    }

    /// <summary>旋转实体使其局部前方向（-Z）对准给定世界空间目标点。</summary>
    public void LookAt(Vector3 worldTarget, Vector3? worldUp = null)
    {
        var dir = worldTarget - WorldPosition;
        if (dir.LengthSquared() > 1e-8f)
            WorldRotation = F_Quat.LookRotation(dir, worldUp);
    }

    /// <summary>以当前摄像机实体为投影源，将屏幕像素坐标转换为世界空间射线。</summary>
    public F_Ray? ScreenPointToRay(Vector2 screenPixel) => F_Camera.ScreenPointToRay(screenPixel, this);

    /// <summary>获取当前实体上挂载的指定类型组件（支持脚本子类、<see cref="F_RigidBody"/> / <see cref="F_Rigidbody"/> 或 <see cref="F_CharacterController"/>）。</summary>
    public T? GetComponent<T>() where T : class
    {
        if (typeof(F_RigidBody).IsAssignableFrom(typeof(T)))
            return Session.Physics.GetBody(AsEntity()) as T;
        if (typeof(F_CharacterController).IsAssignableFrom(typeof(T)))
        {
            try { return CharacterController.FromComponent(AsEntity()) as T; }
            catch (EngineException) { return null; }
        }
        if (Session.EnvironmentOwner is ScriptSystem sys)
        {
            var id = StableId;
            foreach (var b in sys.AllAttachedBehaviours)
                if (b.Entity.StableId == id && b is T typed) return typed;
        }
        return null;
    }

    /// <summary>尝试获取当前实体上挂载的指定类型组件，存在时返回 true。</summary>
    public bool TryGetComponent<T>([NotNullWhen(true)] out T? component) where T : class
    {
        component = GetComponent<T>();
        return component is not null;
    }

    /// <summary>获取当前实体上挂载的全部指定类型组件列表。</summary>
    public IReadOnlyList<T> GetComponents<T>() where T : class
    {
        var single = typeof(F_RigidBody).IsAssignableFrom(typeof(T)) || typeof(F_CharacterController).IsAssignableFrom(typeof(T))
            ? GetComponent<T>() : null;
        if (single is not null) return [single];
        if (Session.EnvironmentOwner is not ScriptSystem sys) return [];
        var id = StableId;
        var list = new List<T>();
        foreach (var b in sys.AllAttachedBehaviours)
            if (b.Entity.StableId == id && b is T typed) list.Add(typed);
        return list;
    }
}

/// <summary>全局物理世界静态快捷入口（对标 <see cref="F_Input"/> / <see cref="F_Scene"/>，委托当前活动会话的 <see cref="Physics"/> 服务）。</summary>
public static class F_Physics
{
    /// <summary>发射物理射线并返回最近命中信息（未命中返回 null）。</summary>
    public static F_RayHit? Raycast(Vector3 origin, Vector3 direction, float distance, uint mask = uint.MaxValue)
        => ScriptSystem.Current?.Session.Physics.Raycast(origin, direction, distance, mask) is { } found
            ? (F_RayHit)found : null;

    /// <summary>发射物理射线并通过 out 参数输出 <see cref="F_RaycastHit"/>，命中时返回 true。</summary>
    public static bool Raycast(Vector3 origin, Vector3 direction, float distance, out F_RaycastHit hit, uint mask = uint.MaxValue)
    {
        if (ScriptSystem.Current?.Session.Physics.Raycast(origin, direction, distance, mask) is { } found)
        {
            hit = found;
            return true;
        }
        hit = default;
        return false;
    }

    /// <summary>发射物理射线并通过 out 参数输出 <see cref="F_RayHit"/>，命中时返回 true。</summary>
    public static bool Raycast(Vector3 origin, Vector3 direction, float distance, out F_RayHit hit, uint mask = uint.MaxValue)
    {
        if (ScriptSystem.Current?.Session.Physics.Raycast(origin, direction, distance, mask) is { } found)
        {
            hit = found;
            return true;
        }
        hit = default;
        return false;
    }

    /// <summary>在指定实体上创建盒体碰撞刚体。</summary>
    public static F_RigidBody? CreateBox(F_Entity entity, F_BoxBodyDescription description)
        => ScriptSystem.Current?.Session.Physics.CreateBox(entity, description);

    /// <summary>在指定实体上创建球体碰撞刚体。</summary>
    public static F_RigidBody? CreateSphere(F_Entity entity, F_SphereBodyDescription description)
        => ScriptSystem.Current?.Session.Physics.CreateSphere(entity, description);

    /// <summary>获取指定实体上的刚体代理（不存在时返回 null）。</summary>
    public static F_RigidBody? GetBody(F_Entity entity)
        => ScriptSystem.Current?.Session.Physics.GetBody(entity);

    /// <summary>开关视口物理碰撞线框调试绘制。</summary>
    public static bool SetDebugDrawing(bool enabled)
        => ScriptSystem.Current?.Session.Physics.SetDebugDrawing(enabled) ?? false;
}

/// <summary>物理刚体运行时人体工学扩展属性与方法。</summary>
public partial class F_RigidBody
{
    /// <summary>获取或设置刚体的世界空间线速度（米/秒）。</summary>
    public Vector3 Velocity
    {
        get => State.LinearVelocity;
        set => SetVelocity(value, State.AngularVelocity);
    }

    /// <summary>获取或设置刚体的世界空间角速度（弧度/秒）。</summary>
    public Vector3 AngularVelocity
    {
        get => State.AngularVelocity;
        set => SetVelocity(State.LinearVelocity, value);
    }

    /// <summary>在刚体质心处施加持续世界空间力（牛顿，建议在 <see cref="F_Behaviour.F_FixedUpdate()"/> 调用）。</summary>
    public void AddForce(Vector3 force) => ApplyForce(force, Entity.Position);
    /// <summary>在刚体质心处施加瞬时冲量（千克·米/秒，适合跳跃或击退）。</summary>
    public void AddImpulse(Vector3 impulse) => ApplyImpulse(impulse, Entity.Position);
}

/// <summary>当前活动场景实体查找、克隆实例化与场景重载工具类。</summary>
public static class F_Scene
{
    /// <summary>按稳定实体 ID 查找当前场景中的存活实体。</summary>
    public static Entity? Find(ulong stableId) => ScriptSystem.Current?.Session.FindEntity(stableId);

    /// <summary>按实体名称查找当前场景中首个匹配的存活实体。</summary>
    public static Entity? Find(string name)
        => string.IsNullOrEmpty(name) ? null : ScriptSystem.Current?.Session.FindEntityByName(name);

    /// <summary>按实体标签（Tag）查找当前场景中首个匹配的存活实体。</summary>
    public static Entity? FindWithTag(string tag)
        => string.IsNullOrEmpty(tag) ? null : ScriptSystem.Current?.Session.FindEntityByTag(tag);

    /// <summary>查找场景中首个处于启用状态的指定类型脚本实例。</summary>
    public static T? FindObjectOfType<T>() where T : class
    {
        var sys = ScriptSystem.Current;
        if (sys is null) return null;
        foreach (var b in sys.Attachments)
            if (b.Enabled && b.Entity.IsAlive && b.Entity.Enabled && b is T typed)
                return typed;
        return null;
    }

    /// <summary>查找场景中全部处于启用状态的指定类型脚本实例列表。</summary>
    public static IReadOnlyList<T> FindObjectsOfType<T>() where T : class
    {
        var sys = ScriptSystem.Current;
        if (sys is null) return [];
        var list = new List<T>();
        foreach (var b in sys.Attachments)
            if (b.Enabled && b.Entity.IsAlive && b.Entity.Enabled && b is T typed)
                list.Add(typed);
        return list;
    }

    /// <summary>请求在当前帧结束后重新加载当前活动场景。</summary>
    public static void Reload()
    {
        if (ScriptSystem.Current is { } sys)
            sys.SceneReloadRequested = true;
    }

    /// <summary>深克隆给定实体及其完整子树并同步复制暴露字段，返回克隆出的根实体。</summary>
    public static Entity Instantiate(F_Entity source, Vector3? position = null, Quaternion? rotation = null)
    {
        var sys = ScriptSystem.Current ?? (source.Session.EnvironmentOwner as ScriptSystem)
            ?? throw new InvalidOperationException("Script.NotAttached");
        return Instantiate(sys, source, position, rotation);
    }

    internal static Entity Instantiate(ScriptSystem scripts, F_Entity source, Vector3? position, Quaternion? rotation)
    {
        ArgumentNullException.ThrowIfNull(source);
        var clonedSubtree = scripts.Session.DuplicateEntity(source, position, rotation);
        if (clonedSubtree.Count == 0)
            throw new EngineException("Bridge.InvalidEntity", 2);
        Dictionary<ulong, ulong>? remap = clonedSubtree.Count > 0 ? new(clonedSubtree.Count) : null;
        foreach (var (srcStableId, clonedEntity) in clonedSubtree)
            remap![srcStableId] = clonedEntity.StableId;

        var existingAttachments = scripts.AllAttachedBehaviours;
        foreach (var (srcStableId, clonedEntity) in clonedSubtree)
        {
            foreach (var existing in existingAttachments)
            {
                if (existing.Entity.StableId != srcStableId) continue;
                var id = ScriptRegistry.IdOf(existing.GetType());
                var copy = scripts.Registry.Create(id) ?? (F_Behaviour?)Activator.CreateInstance(existing.GetType());
                if (copy is null) continue;
                copy.EnabledValue = existing.Enabled;
                ScriptFields.CopyExposedFields(existing, copy, remap);
                scripts.Attach(clonedEntity, copy);
            }
        }
        return clonedSubtree[0].Clone;
    }
}

/// <summary>编辑器控制台日志输出工具（自动附带源文件名与行号，双击控制台日志可直接跳回代码行）。</summary>
public static class F_Log
{
    private static readonly ConcurrentQueue<string> queue = new();
    private static int count;

    /// <summary>每次输出日志时触发的订阅事件。</summary>
    public static event Action<string>? Logged;
    internal static bool HasPending => !queue.IsEmpty;

    /// <summary>输出普通信息日志到编辑器控制台。</summary>
    public static void Info(object? message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
        => Emit("[Info]", message, file, line, false);

    /// <summary>输出警告日志到编辑器控制台。</summary>
    public static void Warn(object? message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
        => Emit("[Warning]", message, file, line, false);

    /// <summary>输出错误日志到编辑器控制台。</summary>
    public static void Error(object? message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
        => Emit("[Error]", message, file, line, true);

    internal static void PushRaw(string formatted)
    {
        Enqueue(formatted);
        try { Logged?.Invoke(formatted); } catch { }
    }

    private static void Emit(string prefix, object? message, string file, int line, bool stderr)
    {
        var src = string.IsNullOrEmpty(file) || line <= 0 ? "" : $"{Path.GetFileName(file)}({line},1): ";
        var formatted = $"{prefix} {src}{message}";
        if (stderr) Console.Error.WriteLine(formatted);
        else Console.WriteLine(formatted);
        PushRaw(formatted);
    }

    private static void Enqueue(string item)
    {
        queue.Enqueue(item);
        if (Interlocked.Increment(ref count) > 512 && queue.TryDequeue(out _))
            Interlocked.Decrement(ref count);
    }

    /// <summary>取出并清空当前缓存的日志行队列。</summary>
    public static IReadOnlyList<string> Drain()
    {
        if (queue.IsEmpty) return [];
        var list = new List<string>();
        while (queue.TryDequeue(out var item))
        {
            Interlocked.Decrement(ref count);
            list.Add(item);
        }
        return list;
    }
}

/// <summary>主线程确定性异步等待辅助类（在脚本 async 方法中配合 await 使用，替代后台线程 Task.Delay）。</summary>
public static class F_Await
{
    public readonly struct StepAwaiter(int mode, float seconds) : INotifyCompletion
    {
        public bool IsCompleted => mode == 0 && seconds <= 0f;
        public void GetResult() { }
        public void OnCompleted(Action continuation)
        {
            var sys = ScriptSystem.Current;
            if (sys is not { IsRunning: true }) return;
            sys.EnqueueAwait(mode, seconds, continuation);
        }
    }

    public readonly struct StepAwaitable(int mode, float seconds)
    {
        public StepAwaiter GetAwaiter() => new(mode, seconds);
    }

    /// <summary>在游戏主循环等待指定缩放时间（秒）后继续执行。</summary>
    public static StepAwaitable Seconds(float seconds) => new(0, seconds);
    /// <summary>等待到下一渲染帧主循环继续执行。</summary>
    public static StepAwaitable NextFrame() => new(1, 0f);
    /// <summary>等待到下一次固定物理步（FixedUpdate）继续执行。</summary>
    public static StepAwaitable FixedUpdate() => new(2, 0f);
}
