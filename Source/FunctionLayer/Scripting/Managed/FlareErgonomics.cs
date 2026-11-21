using System.Collections.Concurrent;
using System.Diagnostics.CodeAnalysis;
using System.Numerics;
using System.Runtime.CompilerServices;

namespace FlareEngine;

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

public static class F_Time
{
    private static float timeScale = 1f;
    public static float DeltaTime { get; internal set; }
    public static float UnscaledDeltaTime { get; internal set; }
    public static float FixedDeltaTime { get; internal set; } = 1f / 60f;
    public static float Time { get; internal set; }
    public static float UnscaledTime { get; internal set; }
    public static ulong FrameCount { get; internal set; }
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

public static class F_Input
{
    private static ScriptInput Active => ScriptSystem.Current?.Input ?? ScriptInput.Empty;

    public static bool Key(F_Key key) => Active.Down((int)key);
    public static bool KeyDown(F_Key key) => Active.Pressed((int)key);
    public static bool KeyUp(F_Key key) => Active.Released((int)key);
    public static bool Mouse(MouseButton button = MouseButton.Left) => Active.Pointer.Down(button);
    public static bool MouseDown(MouseButton button = MouseButton.Left) => Active.Pointer.Pressed(button);
    public static bool MouseUp(MouseButton button = MouseButton.Left) => Active.Pointer.Released(button);
    public static Vector2 MousePosition => Active.Pointer.Position;
    public static Vector2 MouseDelta => ScriptSystem.Current?.Session.MouseDelta ?? Vector2.Zero;
    public static float MouseWheel => Active.Pointer.Wheel;

    public static float Axis(F_Key negative, F_Key positive)
        => (Key(positive) ? 1f : 0f) - (Key(negative) ? 1f : 0f);

    public static float Horizontal
    {
        get
        {
            float v = Axis(F_Key.A, F_Key.D);
            return v != 0f ? v : Axis(F_Key.Left, F_Key.Right);
        }
    }

    public static float Vertical
    {
        get
        {
            float v = Axis(F_Key.S, F_Key.W);
            return v != 0f ? v : Axis(F_Key.Down, F_Key.Up);
        }
    }

    public static Vector2 MoveVector
    {
        get
        {
            var v = new Vector2(Horizontal, Vertical);
            return v.LengthSquared() > 1f ? Vector2.Normalize(v) : v;
        }
    }
}

public static class F_Cursor
{
    public static bool Locked
    {
        get => ScriptSystem.Current?.Session.CursorMode.Locked ?? false;
        set
        {
            if (ScriptSystem.Current?.Session is { } s)
                s.SetCursorMode(value, value ? false : s.CursorMode.Visible);
        }
    }

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

public readonly record struct F_Ray(Vector3 Origin, Vector3 Direction)
{
    public Vector3 GetPoint(float distance) => Origin + Direction * distance;
}

public static class F_Camera
{
    public static F_Ray? ScreenPointToRay(Vector2 screenPixel, F_Entity? camera = null)
    {
        var session = camera?.Session ?? ScriptSystem.Current?.Session;
        if (session is null) return null;
        return session.ScreenPointToRay(camera?.StableId ?? 0, screenPixel) is { } r
            ? new F_Ray(r.Origin, r.Direction) : null;
    }
}

public static class F_Math
{
    public const float PI = MathF.PI;
    public const float Deg2Rad = MathF.PI / 180f;
    public const float Rad2Deg = 180f / MathF.PI;

    public static float Clamp(float value, float min, float max) => Math.Clamp(value, min, max);
    public static int Clamp(int value, int min, int max) => Math.Clamp(value, min, max);
    public static float Clamp01(float value) => Math.Clamp(value, 0f, 1f);
    public static float Lerp(float a, float b, float t) => a + (b - a) * Clamp01(t);
    public static float LerpUnclamped(float a, float b, float t) => a + (b - a) * t;
    public static Vector3 Lerp(Vector3 a, Vector3 b, float t) => Vector3.Lerp(a, b, Clamp01(t));

    public static float InverseLerp(float a, float b, float value)
        => MathF.Abs(b - a) > 1e-6f ? Clamp01((value - a) / (b - a)) : 0f;

    public static float MoveTowards(float current, float target, float maxDelta)
        => MathF.Abs(target - current) <= maxDelta ? target : current + MathF.Sign(target - current) * maxDelta;

    public static Vector3 MoveTowards(Vector3 current, Vector3 target, float maxDistanceDelta)
    {
        var delta = target - current;
        var sq = delta.LengthSquared();
        if (sq == 0f || (maxDistanceDelta >= 0f && sq <= maxDistanceDelta * maxDistanceDelta)) return target;
        return current + delta / MathF.Sqrt(sq) * maxDistanceDelta;
    }

    public static float Repeat(float t, float length)
        => length <= 1e-6f ? 0f : Clamp(t - MathF.Floor(t / length) * length, 0f, length);

    public static float PingPong(float t, float length)
    {
        t = Repeat(t, length * 2f);
        return length - MathF.Abs(t - length);
    }

    public static float DeltaAngle(float current, float target)
    {
        float delta = Repeat(target - current, 360f);
        return delta > 180f ? delta - 360f : delta;
    }

    public static float LerpAngle(float a, float b, float t) => a + DeltaAngle(a, b) * Clamp01(t);

    public static bool Approximately(float a, float b, float epsilon = 1e-5f) => MathF.Abs(a - b) <= epsilon;

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

public static class F_Quat
{
    public static Quaternion Identity => Quaternion.Identity;

    // 与编辑器 SceneInspector 一致的右手系 Z -> X -> Y 欧拉角（角度制）。
    public static Quaternion Euler(float xDegrees, float yDegrees, float zDegrees)
    {
        var qx = Quaternion.CreateFromAxisAngle(Vector3.UnitX, xDegrees * F_Math.Deg2Rad);
        var qy = Quaternion.CreateFromAxisAngle(Vector3.UnitY, yDegrees * F_Math.Deg2Rad);
        var qz = Quaternion.CreateFromAxisAngle(Vector3.UnitZ, zDegrees * F_Math.Deg2Rad);
        return Quaternion.Normalize(qy * qx * qz);
    }

    public static Quaternion Euler(Vector3 eulerDegrees) => Euler(eulerDegrees.X, eulerDegrees.Y, eulerDegrees.Z);

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

    // 右手系：将局部前方向 -Z 对齐到给定世界 forward。
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

    public static Quaternion Slerp(Quaternion a, Quaternion b, float t) => Quaternion.Slerp(a, b, F_Math.Clamp01(t));

    public static float Angle(Quaternion a, Quaternion b)
    {
        float dot = MathF.Min(MathF.Abs(Quaternion.Dot(Quaternion.Normalize(a), Quaternion.Normalize(b))), 1f);
        return dot > 0.999999f ? 0f : MathF.Acos(dot) * 2f * F_Math.Rad2Deg;
    }

    public static Quaternion RotateTowards(Quaternion from, Quaternion to, float maxDegreesDelta)
    {
        float angle = Angle(from, to);
        return angle <= 1e-5f || maxDegreesDelta >= angle ? to : Quaternion.Slerp(from, to, maxDegreesDelta / angle);
    }
}

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

    public Vector3 Position
    {
        get => Transform.Position;
        set => WriteLocalPose(value, null);
    }

    public Quaternion Rotation
    {
        get => Transform.Rotation;
        set => WriteLocalPose(null, value);
    }

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

    public Vector3 LocalScale
    {
        get => Transform.Scale;
        set { var t = Transform; t.Scale = value; Transform = t; }
    }

    public Vector3 EulerAngles
    {
        get => F_Quat.ToEuler(Transform.Rotation);
        set => WriteLocalPose(null, F_Quat.Euler(value));
    }

    public Vector3 Forward => Vector3.Normalize(Vector3.Transform(-Vector3.UnitZ, WorldTransform.Rotation));
    public Vector3 Right => Vector3.Normalize(Vector3.Transform(Vector3.UnitX, WorldTransform.Rotation));
    public Vector3 Up => Vector3.Normalize(Vector3.Transform(Vector3.UnitY, WorldTransform.Rotation));

    public void Translate(Vector3 delta, bool localSpace = true)
    {
        if (localSpace)
            Position += Vector3.Transform(delta, Transform.Rotation);
        else
            WorldPosition += delta;
    }

    public void Rotate(Vector3 eulerDegrees, bool localSpace = true)
    {
        var dq = F_Quat.Euler(eulerDegrees);
        if (localSpace)
            Rotation = Quaternion.Normalize(Transform.Rotation * dq);
        else
            WorldRotation = Quaternion.Normalize(dq * WorldRotation);
    }

    public void LookAt(Vector3 worldTarget, Vector3? worldUp = null)
    {
        var dir = worldTarget - WorldPosition;
        if (dir.LengthSquared() > 1e-8f)
            WorldRotation = F_Quat.LookRotation(dir, worldUp);
    }

    public F_Ray? ScreenPointToRay(Vector2 screenPixel) => F_Camera.ScreenPointToRay(screenPixel, this);

    public T? GetComponent<T>() where T : class
    {
        if (typeof(T) == typeof(RigidBody))
            return Session.Physics.GetBody(AsEntity()) as T;
        if (typeof(T) == typeof(CharacterController))
        {
            try { return CharacterController.FromComponent(AsEntity()) as T; }
            catch (EngineException) { return null; }
        }
        if (Session.EnvironmentOwner is ScriptSystem sys)
        {
            var id = StableId;
            foreach (var b in sys.Attachments)
                if (b.Entity.StableId == id && b is T typed) return typed;
        }
        return null;
    }

    public bool TryGetComponent<T>([NotNullWhen(true)] out T? component) where T : class
    {
        component = GetComponent<T>();
        return component is not null;
    }

    public IReadOnlyList<T> GetComponents<T>() where T : class
    {
        var single = typeof(T) == typeof(RigidBody) || typeof(T) == typeof(CharacterController)
            ? GetComponent<T>() : null;
        if (single is not null) return [single];
        if (Session.EnvironmentOwner is not ScriptSystem sys) return [];
        var id = StableId;
        var list = new List<T>();
        foreach (var b in sys.Attachments)
            if (b.Entity.StableId == id && b is T typed) list.Add(typed);
        return list;
    }
}

public sealed partial class RigidBody
{
    public Vector3 Velocity
    {
        get => State.LinearVelocity;
        set => SetVelocity(value, State.AngularVelocity);
    }

    public Vector3 AngularVelocity
    {
        get => State.AngularVelocity;
        set => SetVelocity(State.LinearVelocity, value);
    }

    public void AddForce(Vector3 force) => ApplyForce(force, Entity.Position);
    public void AddImpulse(Vector3 impulse) => ApplyImpulse(impulse, Entity.Position);
}

public static class F_Scene
{
    public static Entity? Find(ulong stableId) => ScriptSystem.Current?.Session.FindEntity(stableId);

    public static Entity? Find(string name)
        => string.IsNullOrEmpty(name) ? null : ScriptSystem.Current?.Session.FindEntityByName(name);

    public static T? FindObjectOfType<T>() where T : class
    {
        var sys = ScriptSystem.Current;
        if (sys is null) return null;
        foreach (var b in sys.Attachments)
            if (b.Enabled && b.Entity.IsAlive && b.Entity.Enabled && b is T typed)
                return typed;
        return null;
    }

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

    public static void Reload()
    {
        if (ScriptSystem.Current is { } sys)
            sys.SceneReloadRequested = true;
    }

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

        var existingAttachments = scripts.Attachments.ToArray();
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

public static class F_Log
{
    private static readonly ConcurrentQueue<string> queue = new();
    private static int count;

    public static event Action<string>? Logged;
    internal static bool HasPending => !queue.IsEmpty;

    public static void Info(object? message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
        => Emit("[Info]", message, file, line, false);

    public static void Warn(object? message, [CallerFilePath] string file = "", [CallerLineNumber] int line = 0)
        => Emit("[Warning]", message, file, line, false);

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

    public static StepAwaitable Seconds(float seconds) => new(0, seconds);
    public static StepAwaitable NextFrame() => new(1, 0f);
    public static StepAwaitable FixedUpdate() => new(2, 0f);
}
