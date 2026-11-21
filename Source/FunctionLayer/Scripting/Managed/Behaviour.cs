using System.Reflection;
using System.Numerics;

namespace FlareEngine;

[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public class F_ScriptAttribute(string id) : Attribute { public string Id { get; } = id; }

[AttributeUsage(AttributeTargets.Field)]
public class F_ExposeAttribute : Attribute;

[AttributeUsage(AttributeTargets.Field)]
public sealed class F_HeaderAttribute(string title) : Attribute { public string Title { get; } = title; }

[AttributeUsage(AttributeTargets.Field)]
public sealed class F_RangeAttribute(float min, float max) : Attribute
{
    public float Min { get; } = min;
    public float Max { get; } = max;
}

[AttributeUsage(AttributeTargets.Field)]
public sealed class F_TooltipAttribute(string text) : Attribute { public string Text { get; } = text; }

public readonly record struct EntityReference(ulong StableId)
{
    public Entity? Resolve(EngineSession session) => StableId == 0 ? null : session.FindEntity(StableId);
    public static EntityReference From(F_Entity? entity) => new(entity?.StableId ?? 0);
}

public readonly record struct F_Contact(F_Entity Other, Vector3 Point, Vector3 Normal, float Impulse);

public abstract class F_Behaviour
{
    internal ScriptSystem? Owner;
    internal bool EnabledValue = true;
    public Entity Entity { get; internal set; } = null!;
    public Transform Transform
    {
        get => Entity.Transform;
        set => Entity.Transform = value;
    }
    public void Translate(Vector3 delta, bool localSpace = true) => Entity.Translate(delta, localSpace);
    public void Rotate(Vector3 eulerDegrees, bool localSpace = true) => Entity.Rotate(eulerDegrees, localSpace);
    public void LookAt(Vector3 worldTarget, Vector3? worldUp = null) => Entity.LookAt(worldTarget, worldUp);
    public T? GetComponent<T>() where T : class => Entity.GetComponent<T>();
    public bool TryGetComponent<T>([System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out T? component) where T : class
        => Entity.TryGetComponent(out component);
    public IReadOnlyList<T> GetComponents<T>() where T : class => Entity.GetComponents<T>();
    public Entity Instantiate(F_Entity source, Vector3? position = null, Quaternion? rotation = null)
        => F_Scene.Instantiate(Scripts, source, position, rotation);
    public void Destroy(F_Entity target, float delaySeconds = 0f)
    {
        ArgumentNullException.ThrowIfNull(target);
        if (delaySeconds <= 0f)
        {
            if (target is Entity ent && ent.Session == Scripts.Session) Scripts.Destroy(ent);
            else if (Scripts.Session.FindEntity(target.StableId) is { } found) Scripts.Destroy(found);
        }
        else Scripts.ScheduleDestroy(target.StableId, delaySeconds);
    }
    public ScriptSystem Scripts => Owner ?? throw new InvalidOperationException("Script.NotAttached");
    public ScriptInput Input => Scripts.Input;
    public InputActions Actions => Scripts.Session.Actions;
    public Audio Audio => Scripts.Session.Audio;
    public Particles Particles => Scripts.Session.Particles;
    public Navigation Navigation => Scripts.Session.Navigation;
    public Physics Physics => Scripts.Session.Physics;
    public virtual bool UpdateWhenPaused => false;
    public bool Enabled
    {
        get => EnabledValue;
        set { if (Owner is null) EnabledValue = value; else { if (!value) EnabledValue = false; Owner.SetEnabled(this, value); } }
    }

    public virtual void F_Awake() { }
    public virtual void F_Enable() { }
    public virtual void F_Start() { }
    public virtual void F_FixedUpdate() { }
    public virtual void F_FixedUpdate(float fixedDelta) => F_FixedUpdate();
    public virtual void F_PreUpdate() { }
    public virtual void F_PreUpdate(float delta) => F_PreUpdate();
    public virtual void F_Update() { }
    public virtual void F_Update(float delta) => F_Update();
    public virtual void F_LateUpdate() { }
    public virtual void F_LateUpdate(float delta) => F_LateUpdate();
    public virtual void F_CollisionEnter(in F_Contact contact) { }
    public virtual void F_CollisionStay(in F_Contact contact) { }
    public virtual void F_CollisionExit(in F_Contact contact) { }
    public virtual void F_TriggerEnter(F_Entity other) { }
    public virtual void F_TriggerStay(F_Entity other) { }
    public virtual void F_TriggerExit(F_Entity other) { }
    public virtual void F_Disable() { }
    public virtual void F_Destroy() { }

    protected void DestroySelf() => Scripts.Destroy(Entity);
    protected IDisposable Subscribe<T>(Action<T> handler) => Scripts.Events.Subscribe(this, handler);
}

public sealed class ScriptRegistry
{
    private readonly Dictionary<string, Type> types = new(StringComparer.Ordinal);
    public IReadOnlyDictionary<string, Type> Types => types;
    internal void Clear() => types.Clear();
    public void Register<T>() where T : F_Behaviour, new() => Register(typeof(T));
    public void Register(Type type)
    {
        if (type.IsAbstract || !typeof(F_Behaviour).IsAssignableFrom(type) || type.GetConstructor(Type.EmptyTypes) is null)
            throw new ArgumentException("Script.InvalidType", nameof(type));
        string id = IdOf(type);
        if (string.IsNullOrWhiteSpace(id) || id.Length > 256 || id.Any(char.IsControl))
            throw new ArgumentException("Script.InvalidType", nameof(type));
        if (!types.TryAdd(id, type) && types[id] != type) throw new InvalidOperationException("Script.DuplicateType");
    }
    public void Discover(Assembly assembly)
    {
        foreach (var type in assembly.GetTypes().Where(t => t.IsPublic && !t.IsAbstract && typeof(F_Behaviour).IsAssignableFrom(t)))
            Register(type);
    }
    public static string IdOf(Type type) => type.GetCustomAttribute<F_ScriptAttribute>(inherit: false)?.Id ?? type.FullName ?? type.Name;
    internal F_Behaviour? Create(string id) => types.TryGetValue(id, out var type) ? (F_Behaviour?)Activator.CreateInstance(type) : null;
}

public enum MouseButton { Left, Right, Middle }
public readonly record struct PointerInput(Vector2 Position, uint ButtonsDown, uint ButtonsPressed, uint ButtonsReleased, float Wheel, bool Consumed)
{
    public bool Down(MouseButton button) => !Consumed && (ButtonsDown & (1u << (int)button)) != 0;
    public bool Pressed(MouseButton button) => !Consumed && (ButtonsPressed & (1u << (int)button)) != 0;
    public bool Released(MouseButton button) => !Consumed && (ButtonsReleased & (1u << (int)button)) != 0;
    internal PointerInput WithoutEdges() => this with { ButtonsPressed = 0, ButtonsReleased = 0, Wheel = 0 };
    internal PointerInput MergeEdges(PointerInput newer) => newer.Consumed ? newer.WithoutEdges()
        : newer with { ButtonsPressed = ButtonsPressed | newer.ButtonsPressed, ButtonsReleased = ButtonsReleased | newer.ButtonsReleased, Wheel = Wheel + newer.Wheel };
}
public sealed class ScriptInput
{
    private readonly HashSet<int> down;
    private readonly HashSet<int> pressed;
    private readonly HashSet<int> released;
    public static ScriptInput Empty { get; } = new([], [], []);
    public bool KeyboardConsumed { get; }
    public PointerInput Pointer { get; }
    public ScriptInput(IEnumerable<int> down, IEnumerable<int> pressed, IEnumerable<int> released, bool keyboardConsumed = false, PointerInput pointer = default)
    {
        this.down = new(down); this.pressed = new(pressed); this.released = new(released);
        if (this.down.Concat(this.pressed).Concat(this.released).Any(key => key is < 0 or > 255)) throw new ArgumentException("Script.InvalidKey");
        KeyboardConsumed = keyboardConsumed;
        Pointer = pointer.Consumed ? pointer with { ButtonsDown = 0, ButtonsPressed = 0, ButtonsReleased = 0, Wheel = 0 } : pointer;
    }
    public bool Down(int key) => !KeyboardConsumed && down.Contains(key);
    public bool Pressed(int key) => !KeyboardConsumed && pressed.Contains(key);
    public bool Released(int key) => !KeyboardConsumed && released.Contains(key);
    public bool Down(F_Key key) => Down((int)key);
    public bool Pressed(F_Key key) => Pressed((int)key);
    public bool Released(F_Key key) => Released((int)key);
    internal ScriptInput WithoutEdges() => new(down, [], [], KeyboardConsumed, Pointer.WithoutEdges());
    internal ScriptInput MergeEdges(ScriptInput newer) => newer.KeyboardConsumed
        ? new(newer.down, [], [], true, Pointer.MergeEdges(newer.Pointer))
        : new(newer.down, pressed.Concat(newer.pressed), released.Concat(newer.released), false, Pointer.MergeEdges(newer.Pointer));
}

public readonly record struct ScriptDiagnostic(string Key, ulong EntityHandle, string ScriptType, string Detail, string Phase)
{
    // 保留原四参构造和四字段解构，已编译的项目也能继续调用原签名。
    public ScriptDiagnostic(string Key, ulong EntityHandle, string ScriptType, string Detail) : this(Key, EntityHandle, ScriptType, Detail, "") { }
    public void Deconstruct(out string Key, out ulong EntityHandle, out string ScriptType, out string Detail)
    { Key = this.Key; EntityHandle = this.EntityHandle; ScriptType = this.ScriptType; Detail = this.Detail; }
}
