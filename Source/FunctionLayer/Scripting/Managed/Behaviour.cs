using System.Reflection;
using System.Numerics;

namespace FlareEngine;

/// <summary>显式为脚本组件指定稳定的持久化类型标识符（缺省时自动使用完整类名）。</summary>
[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public class F_ScriptAttribute(string id) : Attribute { public string Id { get; } = id; }

/// <summary>将字段暴露到编辑器属性检视面板（Inspector）并随场景绑定自动序列化保存。</summary>
[AttributeUsage(AttributeTargets.Field)]
public class F_ExposeAttribute : Attribute;

/// <summary>在属性检视面板中当前暴露字段上方绘制分组小标题。</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class F_HeaderAttribute(string title) : Attribute { public string Title { get; } = title; }

/// <summary>将数值暴露字段限制在指定闭区间内，并在属性检视面板渲染为可拖拽滑条。</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class F_RangeAttribute(float min, float max) : Attribute
{
    public float Min { get; } = min;
    public float Max { get; } = max;
}

/// <summary>为暴露字段提供鼠标悬停提示说明文本。</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class F_TooltipAttribute(string text) : Attribute { public string Text { get; } = text; }

/// <summary>在属性检视面板中当前暴露字段上方留出指定像素的垂直间距。</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class F_SpaceAttribute(float pixels = 8f) : Attribute { public float Pixels { get; } = pixels; }

/// <summary>将暴露字段在属性检视面板中显示为只读状态，禁止手动编辑。</summary>
[AttributeUsage(AttributeTargets.Field)]
public sealed class F_ReadOnlyAttribute : Attribute;

/// <summary>线性空间 RGBA 浮点颜色（常规分量范围 0..1，HDR 发光可大于 1）。</summary>
public record struct F_Color(float R, float G, float B, float A = 1f)
{
    /// <summary>提取 RGB 三维向量。</summary>
    public Vector3 ToVector3() => new(R, G, B);
    /// <summary>提取 RGBA 四维向量。</summary>
    public Vector4 ToVector4() => new(R, G, B, A);
    public static F_Color White => new(1f, 1f, 1f, 1f);
    public static F_Color Black => new(0f, 0f, 0f, 1f);
    public static F_Color Red => new(1f, 0f, 0f, 1f);
    public static F_Color Green => new(0f, 1f, 0f, 1f);
    public static F_Color Blue => new(0f, 0f, 1f, 1f);
    public static F_Color Yellow => new(1f, 0.92f, 0.016f, 1f);
    public static F_Color Cyan => new(0f, 1f, 1f, 1f);
}

/// <summary>可序列化的场景实体稳定 ID 引用槽位，支持在属性检视面板拖拽实体或通过 <c>⊙</c> 选择器赋值。</summary>
public readonly record struct F_EntityReference(ulong StableId)
{
    /// <summary>当前槽位是否已绑定非零实体 ID。</summary>
    public bool IsValid => StableId != 0;
    /// <summary>在当前活动会话中解析引用的运行时实体；未赋值或目标已销毁时返回 null。</summary>
    public Entity? Resolve() => StableId == 0 ? null : ScriptSystem.Current?.Session.FindEntity(StableId);
    /// <summary>在给定会话中解析引用的运行时实体；未赋值或目标已销毁时返回 null。</summary>
    public Entity? Resolve(EngineSession session) => StableId == 0 ? null : session.FindEntity(StableId);
    /// <summary>从实体实例构造稳定引用。</summary>
    public static F_EntityReference From(F_Entity? entity) => new(entity?.StableId ?? 0);
    public static implicit operator EntityReference(F_EntityReference value) => new(value.StableId);
    public static implicit operator F_EntityReference(EntityReference value) => new(value.StableId);
}

/// <summary>可序列化的场景实体稳定 ID 引用槽位（兼容无前缀写法，等价于 <see cref="F_EntityReference"/>）。</summary>
public readonly record struct EntityReference(ulong StableId)
{
    /// <summary>当前槽位是否已绑定非零实体 ID。</summary>
    public bool IsValid => StableId != 0;
    /// <summary>在当前活动会话中解析引用的运行时实体；未赋值或目标已销毁时返回 null。</summary>
    public Entity? Resolve() => StableId == 0 ? null : ScriptSystem.Current?.Session.FindEntity(StableId);
    /// <summary>在给定会话中解析引用的运行时实体；未赋值或目标已销毁时返回 null。</summary>
    public Entity? Resolve(EngineSession session) => StableId == 0 ? null : session.FindEntity(StableId);
    /// <summary>从实体实例构造稳定引用。</summary>
    public static EntityReference From(F_Entity? entity) => new(entity?.StableId ?? 0);
}

/// <summary>物理刚体碰撞接触信息：对方实体、世界接触点、接触法线与冲量大小。</summary>
public readonly record struct F_Contact(F_Entity Other, Vector3 Point, Vector3 Normal, float Impulse);

/// <summary>
/// FlareEngine 游戏脚本组件基类。继承此类并挂载到场景实体即可响应生命周期、物理碰撞与输入事件。
/// </summary>
public abstract class F_Behaviour
{
    internal ScriptSystem? Owner;
    internal bool EnabledValue = true;
    /// <summary>当前脚本所挂载的场景实体。</summary>
    public Entity Entity { get; internal set; } = null!;
    /// <summary>当前实体的局部空间变换（位置、旋转四元数、缩放）。</summary>
    public Transform Transform
    {
        get => Entity.Transform;
        set => Entity.Transform = value;
    }
    /// <summary>平移当前实体（默认沿局部坐标系，传入 localSpace: false 沿世界坐标系）。</summary>
    public void Translate(Vector3 delta, bool localSpace = true) => Entity.Translate(delta, localSpace);
    /// <summary>按欧拉角（角度制）旋转当前实体。</summary>
    public void Rotate(Vector3 eulerDegrees, bool localSpace = true) => Entity.Rotate(eulerDegrees, localSpace);
    /// <summary>旋转当前实体使其局部前方向（-Z）朝向给定世界坐标目标点。</summary>
    public void LookAt(Vector3 worldTarget, Vector3? worldUp = null) => Entity.LookAt(worldTarget, worldUp);
    /// <summary>获取当前实体上挂载的指定类型组件（支持其他脚本、<see cref="RigidBody"/> 或 <see cref="CharacterController"/>）。</summary>
    public T? GetComponent<T>() where T : class => Entity.GetComponent<T>();
    /// <summary>尝试获取当前实体上挂载的指定类型组件，找到时返回 true。</summary>
    public bool TryGetComponent<T>([System.Diagnostics.CodeAnalysis.NotNullWhen(true)] out T? component) where T : class
        => Entity.TryGetComponent(out component);
    /// <summary>获取当前实体上挂载的全部指定类型组件列表。</summary>
    public IReadOnlyList<T> GetComponents<T>() where T : class => Entity.GetComponents<T>();
    /// <summary>深克隆给定实体及其完整子树（含网格、灯光、物理与脚本绑定），可选覆盖初始位置与旋转。</summary>
    public Entity Instantiate(F_Entity source, Vector3? position = null, Quaternion? rotation = null)
        => F_Scene.Instantiate(Scripts, source, position, rotation);
    /// <summary>销毁目标实体；可指定延迟秒数（0 表示立即销毁）。</summary>
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
    /// <summary>所属脚本调度系统。</summary>
    public ScriptSystem Scripts => Owner ?? throw new InvalidOperationException("Script.NotAttached");
    /// <summary>当前帧底层键鼠输入快照（常规玩法推荐使用静态类 <see cref="F_Input"/>）。</summary>
    public ScriptInput Input => Scripts.Input;
    /// <summary>动作映射输入服务（Input Actions）。</summary>
    public InputActions Actions => Scripts.Session.Actions;
    /// <summary>音频播放与 3D 音源控制服务。</summary>
    public Audio Audio => Scripts.Session.Audio;
    /// <summary>GPU/CPU 粒子发射器控制服务。</summary>
    public Particles Particles => Scripts.Session.Particles;
    /// <summary>导航网格寻路与代理控制服务。</summary>
    public Navigation Navigation => Scripts.Session.Navigation;
    /// <summary>物理世界查询（射线检测、刚体创建与接触轮询）服务。</summary>
    public Physics Physics => Scripts.Session.Physics;
    /// <summary>游戏界面文档树（GameUI）节点查询与交互服务。</summary>
    public GameUi Ui => Scripts.Session.Ui;
    /// <summary>重写并返回 true 时，即使编辑器处于暂停状态仍继续执行更新。</summary>
    public virtual bool UpdateWhenPaused => false;
    /// <summary>当前脚本组件的启用状态；禁用时会触发 <see cref="F_Disable"/> 并暂停帧更新。</summary>
    public bool Enabled
    {
        get => EnabledValue;
        set { if (Owner is null) EnabledValue = value; else { if (!value) EnabledValue = false; Owner.SetEnabled(this, value); } }
    }

    /// <summary>脚本实例挂载到实体后最早调用一次（早于 <see cref="F_Enable"/> 与 <see cref="F_Start"/>），适合初始化自身引用。</summary>
    public virtual void F_Awake() { }
    /// <summary>脚本每次由禁用变为启用时调用。</summary>
    public virtual void F_Enable() { }
    /// <summary>脚本首次参与帧更新前调用一次（在所有组件完成 <see cref="F_Awake"/> 之后）。</summary>
    public virtual void F_Start() { }
    /// <summary>按固定物理步长（默认 60Hz）调用，适合刚体施力与角色控制器移动。</summary>
    public virtual void F_FixedUpdate() { }
    /// <summary>按固定物理步长调用并传入固定步长秒数 <paramref name="fixedDelta"/>。</summary>
    public virtual void F_FixedUpdate(float fixedDelta) => F_FixedUpdate();
    /// <summary>每帧在常规 <see cref="F_Update()"/> 之前调用。</summary>
    public virtual void F_PreUpdate() { }
    /// <summary>每帧在常规更新前调用并传入帧间隔秒数 <paramref name="delta"/>。</summary>
    public virtual void F_PreUpdate(float delta) => F_PreUpdate();
    /// <summary>每渲染帧调用一次（可通过 <see cref="F_Time.DeltaTime"/> 读取帧间隔秒数）。</summary>
    public virtual void F_Update() { }
    /// <summary>每渲染帧调用一次并传入经过时间缩放的帧间隔秒数 <paramref name="delta"/>。</summary>
    public virtual void F_Update(float delta) => F_Update();
    /// <summary>每帧在所有脚本完成 <see cref="F_Update()"/> 后调用，适合相机跟随。</summary>
    public virtual void F_LateUpdate() { }
    /// <summary>每帧在常规更新后调用并传入帧间隔秒数 <paramref name="delta"/>。</summary>
    public virtual void F_LateUpdate(float delta) => F_LateUpdate();
    /// <summary>当前实体的非触发器刚体与另一刚体开始碰撞时调用。</summary>
    public virtual void F_CollisionEnter(in F_Contact contact) { }
    /// <summary>当前实体的非触发器刚体与另一刚体持续接触期间每步调用。</summary>
    public virtual void F_CollisionStay(in F_Contact contact) { }
    /// <summary>当前实体的非触发器刚体与另一刚体分离时调用。</summary>
    public virtual void F_CollisionExit(in F_Contact contact) { }
    /// <summary>另一实体进入当前触发器（或当前实体进入另一触发器）时调用。</summary>
    public virtual void F_TriggerEnter(F_Entity other) { }
    /// <summary>触发器重叠持续期间每步调用。</summary>
    public virtual void F_TriggerStay(F_Entity other) { }
    /// <summary>触发器重叠结束分离时调用。</summary>
    public virtual void F_TriggerExit(F_Entity other) { }
    /// <summary>脚本被禁用或销毁前调用。</summary>
    public virtual void F_Disable() { }
    /// <summary>脚本或所属实体被销毁时调用一次，适合释放外部资源。</summary>
    public virtual void F_Destroy() { }

    /// <summary>立即销毁当前脚本所属的实体。</summary>
    protected void DestroySelf() => Scripts.Destroy(Entity);
    /// <summary>订阅强类型脚本事件总线消息，生命周期随当前脚本自动解绑。</summary>
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

/// <summary>鼠标按键枚举（供 <see cref="F_Input"/> 查询）。</summary>
public enum F_MouseButton { Left = 0, Right = 1, Middle = 2 }
public enum MouseButton { Left = 0, Right = 1, Middle = 2 }
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
