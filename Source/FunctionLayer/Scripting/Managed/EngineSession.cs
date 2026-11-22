using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace FlareEngine;

/// <summary>玩家运行时画质档位预设。</summary>
public enum F_QualityLevel { Project = 0, Performance = 1, Balanced = 2, High = 3 }
/// <summary>玩家运行时画质档位预设（兼容无前缀写法）。</summary>
public enum QualityLevel { Project = 0, Performance = 1, Balanced = 2, High = 3 }

/// <summary>三维空间变换（局部或世界坐标系的位置、四元数旋转与缩放）。</summary>
[StructLayout(LayoutKind.Sequential)]
public struct F_Transform
{
    /// <summary>三维位置坐标（米）。</summary>
    public Vector3 Position;
    /// <summary>单位四元数旋转。</summary>
    public Quaternion Rotation;
    /// <summary>三轴缩放倍率（默认 1,1,1）。</summary>
    public Vector3 Scale;
    /// <summary>原点无旋转且等比 1 倍缩放的单位变换。</summary>
    public static F_Transform Identity => new() { Rotation = Quaternion.Identity, Scale = Vector3.One };
    public static implicit operator Transform(F_Transform value) => new() { Position = value.Position, Rotation = value.Rotation, Scale = value.Scale };
    public static implicit operator F_Transform(Transform value) => new() { Position = value.Position, Rotation = value.Rotation, Scale = value.Scale };
}

/// <summary>三维空间变换（局部或世界坐标系的位置、四元数旋转与缩放）。</summary>
[StructLayout(LayoutKind.Sequential)]
public struct Transform
{
    /// <summary>三维位置坐标（米）。</summary>
    public Vector3 Position;
    /// <summary>单位四元数旋转。</summary>
    public Quaternion Rotation;
    /// <summary>三轴缩放倍率（默认 1,1,1）。</summary>
    public Vector3 Scale;
    /// <summary>原点无旋转且等比 1 倍缩放的单位变换。</summary>
    public static Transform Identity => new() { Rotation = Quaternion.Identity, Scale = Vector3.One };
}
[StructLayout(LayoutKind.Sequential)]
public struct ModelInfo { public uint Meshes,Vertices,Indices; private uint reserved; }

public sealed class EngineException(string key, int result, Exception? inner = null) : Exception(key, inner)
{
    public string Key { get; } = key;
    public int Result { get; } = result;
}

public sealed unsafe partial class EngineSession : IDisposable
{
    private readonly NativeSessionHandle handle;
    private readonly int ownerThread = Environment.CurrentManagedThreadId;
    private int callbackDepth;
    private bool inCallback => callbackDepth != 0;
    private bool disposeRequested;
    private bool disposed;
    public event Action<double>? Update;
    internal event Action<double>? PostStep;
    public event Action<string, Exception?>? Diagnostic;
    public event Action<bool>? EditorPlayChanged;
    internal Func<ulong,string,string>? EditorDataRequested;
    public Exception? LastCallbackException { get; private set; }
    public bool IsDisposed => disposed;
    public uint NativeScriptComponentCount=>IsDisposed?0:Invoke(value=>{Check(NativeMethods.ScriptComponentCount(value,out var count));return count;});
    public Physics Physics { get; }
    public GameUi Ui { get; }
    public SceneService Scenes{get;}
    public TimeOfDayService TimeOfDay{get;}
    public InputActions Actions { get; }
    public Audio Audio { get; }
    public Particles Particles { get; }
    public Navigation Navigation { get; }
    public bool IsWindowRunning { get; private set; }
    internal ScriptInput WindowInput { get; private set; } = ScriptInput.Empty;

    public EngineSession()
    {
        Physics = new(this);
        Ui = new(this);
        Scenes=new(this);
        TimeOfDay=new(this);
        Actions = new(this);
        Audio = new(this);
        Particles = new(this);
        Navigation = new(this);
        Check(NativeMethods.Abi(out var abi));
        if (abi.Version != 1 || abi.TransformSize != Marshal.SizeOf<Transform>() || abi.PointerSize != 8)
            throw new EngineException("Bridge.AbiMismatch", 1);
        Check(NativeMethods.Create(out var value));
        handle = new NativeSessionHandle(value);
        try
        {
            // 弱句柄不会把整个 session 永久根住；Step 自身保持 owner 强引用。
            handle.CallbackContext = GCHandle.Alloc(this, GCHandleType.Weak);
            Check(NativeMethods.SetUpdate(value, &NativeUpdate, GCHandle.ToIntPtr(handle.CallbackContext)));
            Check(NativeMethods.SetPostStep(value, &NativePostStep, GCHandle.ToIntPtr(handle.CallbackContext)));
            Check(NativeMethods.SetEnvironmentCallback(value,&NativeEnvironmentTick,GCHandle.ToIntPtr(handle.CallbackContext)));
            if(Marshal.SizeOf<EventHeader>()!=88||Marshal.SizeOf<EventStatistics>()!=104||Marshal.SizeOf<AudioPlayDescription>()!=40)throw new EngineException("Bridge.AbiMismatch",1);
            Check(NativeMethods.SetEventCallback(value,&NativeEvent,GCHandle.ToIntPtr(handle.CallbackContext)));
            Check(NativeMethods.SetSceneCallback(value,&NativeSceneRetiring,GCHandle.ToIntPtr(handle.CallbackContext)));
            Check(NativeMethods.SetEditorCallback(value, &NativeEditorPhase, GCHandle.ToIntPtr(handle.CallbackContext)));
            Check(NativeMethods.SetEditorData(value, &NativeEditorData, GCHandle.ToIntPtr(handle.CallbackContext)));
        }
        catch { handle.Dispose(); throw; }
    }

    public double Value
    {
        get => Invoke(value => { Check(NativeMethods.GetValue(value, out var result)); return result; });
        set => Invoke(native => { Check(NativeMethods.SetValue(native, value)); return 0; });
    }
    public ulong StepCount => Invoke(value => { Check(NativeMethods.GetStepCount(value, out var count)); return count; });

    public void Step(double deltaSeconds)
    {
        LastCallbackException = null;
        try { Invoke(value => { Check(NativeMethods.Step(value, deltaSeconds), LastCallbackException); return 0; }); }
        finally
        {
            if (disposeRequested) Dispose();
            GC.KeepAlive(this);
        }
    }
    public void SetWindowCapture(string? path,ulong frame=1)
    {
        var target=string.IsNullOrEmpty(path)?null:Path.GetFullPath(path);
        Invoke(value=>{Check(NativeMethods.SetWindowCapture(value,target,target is null?0:checked((uint)Encoding.UTF8.GetByteCount(target)),target is null?0:frame));return 0;});
    }
    // 固定回放尺寸；0x0 清除。只在窗口运行前配置。
    public void SetWindowResolution(uint width,uint height)
        => Invoke(value=>{Check(NativeMethods.SetWindowResolution(value,width,height));return 0;});
    public void ConfigurePlayerQuality(string gameId,bool showMenu=true,string? profilesJson=null)
    {
        ArgumentException.ThrowIfNullOrWhiteSpace(gameId);
        var key=Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(Encoding.UTF8.GetBytes(gameId)));
        var file=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"FlareEngine","Players",key,"quality.txt");
        var profiles=profilesJson??"{}";
        Invoke(value=>{Check(NativeMethods.ConfigureQualityProfiles(value,profiles,checked((uint)Encoding.UTF8.GetByteCount(profiles))));Check(NativeMethods.ConfigureQuality(value,file,checked((uint)Encoding.UTF8.GetByteCount(file)),showMenu?1:0));return 0;});
    }
    public QualityLevel PlayerQuality
    {
        get=>Invoke(value=>{Check(NativeMethods.PlayerQuality(value,-1,out int current));return (QualityLevel)current;});
        set=>Invoke(native=>{if(!Enum.IsDefined(value))throw new ArgumentOutOfRangeException(nameof(value));Check(NativeMethods.PlayerQuality(native,(int)value,out _));return 0;});
    }
    // 固定步长（秒）；0 清除。序列抓帧缺省 1/30 秒。
    public void SetWindowFixedStep(double seconds)
        => Invoke(value=>{Check(NativeMethods.SetWindowFixedStep(value,seconds));return 0;});
    // 连续固定步抓帧目录与帧数；count 0 清除。与单帧抓帧互斥。
    public void SetCaptureSequence(string? directory,ulong count)
    {
        var target=string.IsNullOrEmpty(directory)||count==0?null:Path.GetFullPath(directory);
        if(target is not null)Directory.CreateDirectory(target);
        Invoke(value=>{Check(NativeMethods.SetCaptureSequence(value,target,target is null?0:checked((uint)Encoding.UTF8.GetByteCount(target)),target is null?0:count));return 0;});
    }
    // W06：运行时外观覆盖。mode 0 基础色 / 1 白模 / 2 Toon；传 -1 清除并回到场景设置。
    public void SetLook(int mode,float blend=1f)
        => Invoke(value=>{Check(NativeMethods.SetLook(value,mode,blend));return 0;});
    // W07：程序化天空覆盖；null 清除。颜色为显示线性 0..1，太阳方向仍来自场景方向光。
    public void SetSky(SkyState? state)
    {
        if(state is null){Invoke(value=>{Check(NativeMethods.SetSky(value,null,0));return 0;});return;}
        var s=state.Value;
        var culture=System.Globalization.CultureInfo.InvariantCulture;
        var text=string.Format(culture,
            "{{\"zenith\":[{0:R},{1:R},{2:R}],\"horizon\":[{3:R},{4:R},{5:R}],\"ground\":[{6:R},{7:R},{8:R}],\"sunColor\":[{9:R},{10:R},{11:R}],\"sunIntensity\":{12:R},\"sunAngularRadius\":{13:R}}}",
            s.Zenith.X,s.Zenith.Y,s.Zenith.Z,s.Horizon.X,s.Horizon.Y,s.Horizon.Z,s.Ground.X,s.Ground.Y,s.Ground.Z,
            s.SunColor.X,s.SunColor.Y,s.SunColor.Z,s.SunIntensity,s.SunAngularRadius);
        Invoke(value=>{Check(NativeMethods.SetSky(value,text,checked((uint)Encoding.UTF8.GetByteCount(text))));return 0;});
    }
    public void RunWindow(string locale = "zh-CN", bool visible = true, ulong maximumFrames = 0)
    {
        if (IsWindowRunning) throw new EngineException("Bridge.SessionBusy", 4);
        LastCallbackException = null;
        IsWindowRunning = true;
        try { Invoke(value => { Check(NativeMethods.RunWindow(value, locale, (uint)Encoding.UTF8.GetByteCount(locale), visible ? 1 : 0, maximumFrames), LastCallbackException); return 0; }); }
        finally
        {
            IsWindowRunning = false; WindowInput = ScriptInput.Empty;
            if (disposeRequested) Dispose();
            GC.KeepAlive(this);
        }
    }
    public void RequestWindowClose() => Invoke(value => { Check(NativeMethods.RequestStop(value)); return 0; });
    public void RequestEditorPlay(bool playing) => Invoke(value => { Check(NativeMethods.EditorSetPlaying(value,playing?1:0)); return 0; });
    public string InspectScriptBindings(ulong stableEntityId) => Invoke(value => ReadString((byte* buffer,uint capacity,out uint required)=>NativeMethods.InspectorQuery(value,stableEntityId,buffer,capacity,out required)));
    public void EditScriptBindings(ulong stableEntityId,string request) => Invoke(value=>{Check(NativeMethods.InspectorApply(value,stableEntityId,request,checked((uint)Encoding.UTF8.GetByteCount(request))));return 0;});
    public void RunEditor(string projectRoot, string locale = "zh-CN", bool visible = true, ulong maximumFrames = 0, bool startPlaying = false)
    {
        if(IsWindowRunning)throw new EngineException("Bridge.SessionBusy",4);
        LastCallbackException=null;IsWindowRunning=true;
        try { Invoke(value => { Check(NativeMethods.RunEditor(value,projectRoot,checked((uint)Encoding.UTF8.GetByteCount(projectRoot)),locale,checked((uint)Encoding.UTF8.GetByteCount(locale)),visible?1:0,maximumFrames,startPlaying?1:0),LastCallbackException);return 0;}); }
        finally { IsWindowRunning=false;WindowInput=ScriptInput.Empty;Ui.InvalidateProxies();if(disposeRequested)Dispose();GC.KeepAlive(this); }
    }
    public void LoadLocale(string locale) => Invoke(value => { Check(NativeMethods.SetLocale(value, locale, checked((uint)Encoding.UTF8.GetByteCount(locale)))); return 0; });
    public string Text(string key) => Invoke(value => ReadString((byte* buffer, uint capacity, out uint required) => NativeMethods.Text(value, key, checked((uint)Encoding.UTF8.GetByteCount(key)), buffer, capacity, out required)));
    public void SetAssetRoot(string root,string? cache=null)
    {
        root=Path.GetFullPath(root);cache=Path.GetFullPath(cache??Path.Combine(root,".cache"));
        Invoke(value=>{Check(NativeMethods.SetAssetRoot(value,root,checked((uint)Encoding.UTF8.GetByteCount(root)),cache,checked((uint)Encoding.UTF8.GetByteCount(cache))));return 0;});
    }
    public IReadOnlyList<string> AssetIds => Invoke(value=>System.Text.Json.JsonSerializer.Deserialize<List<string>>(ReadString((byte* output,uint capacity,out uint required)=>NativeMethods.AssetList(value,output,capacity,out required)))??[]);
    public ModelInfo LoadModelInfo(string id) => Invoke(value=>{Check(NativeMethods.LoadModelInfo(value,id,checked((uint)Encoding.UTF8.GetByteCount(id)),out var info));return info;});
    private ScriptInput ReadInput() => Invoke(value =>
    {
        Check(NativeMethods.GetInput(value, out var input));
        Check(NativeMethods.GetPointer(value, out var pointer));
        List<int> down = [], pressed = [], released = [];
        for (int key = 0; key < 256; ++key)
        {
            if (input.Down[key] != 0) down.Add(key);
            if (input.Pressed[key] != 0) pressed.Add(key);
            if (input.Released[key] != 0) released.Add(key);
        }
        return new ScriptInput(down, pressed, released, input.KeyboardConsumed != 0,
            new(new(pointer.X,pointer.Y),pointer.Down,pointer.Pressed,pointer.Released,pointer.Wheel,pointer.Consumed!=0));
    });

    public Entity CreateEntity(string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        return Invoke(value => { Check(NativeMethods.CreateEntity(value, name, checked((uint)Encoding.UTF8.GetByteCount(name)), out var entity)); return new Entity(this, entity); });
    }
    public Entity? FindEntity(ulong stableId) => Invoke<Entity?>(value =>
    {
        var result = NativeMethods.FindEntity(value, stableId, out var entity);
        if (result == 2) return null;
        Check(result);
        return new Entity(this, entity);
    });
    public Entity? FindEntityByName(string name)
    {
        ArgumentNullException.ThrowIfNull(name);
        return Invoke<Entity?>(value =>
        {
            Check(NativeMethods.FindEntityByName(value, name, checked((uint)Encoding.UTF8.GetByteCount(name)), out var entity, out var found));
            return found != 0 ? new Entity(this, entity) : null;
        });
    }
    public Entity? FindEntityByTag(string tag)
    {
        ArgumentNullException.ThrowIfNull(tag);
        return Invoke<Entity?>(value =>
        {
            Check(NativeMethods.FindEntityByTag(value, tag, checked((uint)Encoding.UTF8.GetByteCount(tag)), out var entity, out var found));
            return found != 0 ? new Entity(this, entity) : null;
        });
    }
    internal string GetEntityTag(ulong entity)
        => Invoke(value => ReadString((byte* buffer, uint capacity, out uint required) => NativeMethods.GetTag(value, entity, buffer, capacity, out required)));
    internal void SetEntityTag(ulong entity, string tag)
    {
        var text = string.IsNullOrEmpty(tag) ? "Untagged" : tag;
        Invoke(value => { Check(NativeMethods.SetTag(value, entity, text, checked((uint)Encoding.UTF8.GetByteCount(text)))); return 0; });
    }
    internal int GetEntityLayer(ulong entity)
        => Invoke(value => { Check(NativeMethods.GetLayer(value, entity, out var layer)); return (int)layer; });
    internal void SetEntityLayer(ulong entity, int layer)
        => Invoke(value => { Check(NativeMethods.SetLayer(value, entity, checked((uint)layer))); return 0; });
    public Vector2 MouseDelta => Invoke(value => { Check(NativeMethods.GetMouseDelta(value, out var delta)); return delta; });
    public (bool Locked, bool Visible) CursorMode => Invoke(value =>
    {
        Check(NativeMethods.GetCursorMode(value, out var locked, out var visible));
        return (locked != 0, visible != 0);
    });
    public void SetCursorMode(bool locked, bool visible)
        => Invoke(value => { Check(NativeMethods.SetCursorMode(value, locked ? 1 : 0, visible ? 1 : 0)); return 0; });
    public (Vector3 Origin, Vector3 Direction)? ScreenPointToRay(ulong cameraStableId, Vector2 screenPixel) => Invoke<(Vector3, Vector3)?>(value =>
    {
        var result = NativeMethods.ScreenPointToRay(value, cameraStableId, screenPixel, out var origin, out var direction);
        if (result == 1 || result == 2) return null;
        Check(result);
        return (origin, direction);
    });
    public IReadOnlyList<(ulong SourceStableId, Entity Clone)> DuplicateEntity(F_Entity source, Vector3? position = null, Quaternion? rotation = null)
    {
        ArgumentNullException.ThrowIfNull(source);
        if (source.Session != this) throw new ArgumentException("Bridge.InvalidEntity");
        return Invoke<IReadOnlyList<(ulong, Entity)>>(value =>
        {
            Vector3 posVal = position ?? default;
            Quaternion rotVal = rotation.HasValue ? Quaternion.Normalize(rotation.Value) : Quaternion.Identity;
            float* rotPtr = stackalloc float[4] { rotVal.X, rotVal.Y, rotVal.Z, rotVal.W };
            Vector3* posPtr = position.HasValue ? &posVal : null;
            float* optRotPtr = rotation.HasValue ? rotPtr : null;
            var probe = NativeMethods.DuplicateEntity(value, source.Handle, posPtr, optRotPtr, null, null, 0, out var required);
            if (probe != 3) Check(probe);
            if (required == 0) return [];
            var srcIds = new ulong[required];
            var dstHandles = new ulong[required];
            fixed (ulong* pSrc = srcIds)
            fixed (ulong* pDst = dstHandles)
            {
                Check(NativeMethods.DuplicateEntity(value, source.Handle, posPtr, optRotPtr, pSrc, pDst, required, out var actual));
                var result = new (ulong, Entity)[actual];
                for (uint i = 0; i < actual; ++i)
                    result[i] = (srcIds[i], new Entity(this, dstHandles[i]));
                return result;
            }
        });
    }
    public string ExportScene() => Invoke(value => ReadString((byte* buffer, uint capacity, out uint required) => NativeMethods.ExportScene(value, buffer, capacity, out required)));
    public void ImportScene(string json)
    {
        ArgumentNullException.ThrowIfNull(json);
        Invoke(value => { Check(NativeMethods.ImportScene(value, json, checked((uint)Encoding.UTF8.GetByteCount(json)))); return 0; });
    }
    public void Dispose()
    {
        if (disposed) return;
        if (Environment.CurrentManagedThreadId != ownerThread) throw new EngineException("Bridge.WrongThread", 6);
        if (inCallback) { disposeRequested = true; return; }
        disposed = true;
        Update = null;
        PostStep = null;
        EditorPlayChanged = null;
        EventHandlers.Clear();
        SceneRetiring=null;retiringEntities.Clear();
        handle.Dispose();
    }

    internal T Invoke<T>(Func<ulong,T> action)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        if (Environment.CurrentManagedThreadId != ownerThread) throw new EngineException("Bridge.WrongThread", 6);
        bool pinned = false;
        handle.DangerousAddRef(ref pinned);
        try { return action(unchecked((ulong)handle.DangerousGetHandle())); }
        finally { if (pinned) handle.DangerousRelease(); }
    }
    internal static void Check(int result, Exception? inner = null)
    {
        if (result == 0) return;
        byte* buffer = stackalloc byte[256];
        var read = NativeMethods.LastError(buffer, 256, out _);
        var key = read == 0 ? Marshal.PtrToStringUTF8((nint)buffer)! : "Bridge.NativeException";
        throw new EngineException(key, result, inner);
    }
    internal unsafe delegate int StringReader(byte* buffer, uint capacity, out uint required);
    internal static string ReadString(StringReader read)
    {
        var result = read(null, 0, out var required);
        if (result != 3) Check(result);
        if (required == 0 || required > 64u*1024u*1024u) throw new EngineException("Bridge.InvalidArgument", 1);
        var bytes = new byte[required];
        fixed (byte* buffer = bytes) Check(read(buffer, required, out _));
        return Encoding.UTF8.GetString(bytes, 0, checked((int)required - 1));
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeEditorData(ulong _,ulong entity,byte* request,uint length,byte* output,uint capacity,uint* required,nint context)
    {
        EngineSession? owner=null;
        try
        {
            owner=GCHandle.FromIntPtr(context).Target as EngineSession;if(owner is null||required==null)return 1;
            ++owner.callbackDepth;
            var command=length==0?"":Encoding.UTF8.GetString(new ReadOnlySpan<byte>(request,checked((int)length)));
            var text=owner.EditorDataRequested?.Invoke(entity,command)??"{}";
            var bytes=Encoding.UTF8.GetBytes(text);*required=checked((uint)bytes.Length+1);
            if(capacity<*required)return 3;
            bytes.CopyTo(new Span<byte>(output,bytes.Length));output[bytes.Length]=0;return 0;
        }
        catch(Exception exception)
        {
            if(owner is not null){owner.LastCallbackException=exception;try{owner.Diagnostic?.Invoke("Script.MetadataFailed",exception);}catch{}}
            return 1;
        }
        finally{if(owner is not null){if(owner.disposeRequested&&owner.IsWindowRunning)try{owner.RequestWindowClose();}catch{}--owner.callbackDepth;}}
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeEditorPhase(ulong _,int phase,nint context)
    {
        EngineSession? owner=null;
        try
        {
            owner=GCHandle.FromIntPtr(context).Target as EngineSession;if(owner is null)return 1;
            ++owner.callbackDepth;
            if(phase==0)owner.Ui.InvalidateProxies();
            Exception? failure=null;
            if(owner.EditorPlayChanged is not null)
                foreach(Action<bool> callback in owner.EditorPlayChanged.GetInvocationList())
                    try{callback(phase==0);}catch(Exception exception){failure??=exception;}
            if(failure is not null)throw failure;
            if(owner.disposeRequested)owner.RequestWindowClose();
            return 0;
        }
        catch(Exception exception)
        {
            if(owner is not null){owner.LastCallbackException=exception;try{owner.Diagnostic?.Invoke("Bridge.ManagedCallbackFailed",exception);}catch{}}
            return 1;
        }
        finally {if(owner is not null){if(phase!=0)owner.Ui.InvalidateProxies();--owner.callbackDepth;}}
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativeUpdate(ulong _, double deltaSeconds, nint context)
    {
        EngineSession? owner = null;
        try
        {
            owner = GCHandle.FromIntPtr(context).Target as EngineSession;
            if (owner is null) return 1;
            ++owner.callbackDepth;
            if (owner.IsWindowRunning) owner.WindowInput = owner.ReadInput();
            owner.Update?.Invoke(deltaSeconds);
            if (owner.disposeRequested && owner.IsWindowRunning) owner.RequestWindowClose();
            return 0;
        }
        catch (Exception exception)
        {
            if (owner is not null)
            {
                owner.LastCallbackException = exception;
                try { owner.Diagnostic?.Invoke("Bridge.ManagedCallbackFailed", exception); } catch { }
            }
            return 1;
        }
        finally { if (owner is not null) --owner.callbackDepth; }
    }
    [UnmanagedCallersOnly(CallConvs = [typeof(CallConvCdecl)])]
    private static int NativePostStep(ulong _, double deltaSeconds, nint context)
    {
        EngineSession? owner = null;
        try
        {
            owner = GCHandle.FromIntPtr(context).Target as EngineSession;
            if (owner is null) return 1;
            ++owner.callbackDepth;
            owner.PostStep?.Invoke(deltaSeconds);
            if (owner.disposeRequested && owner.IsWindowRunning) owner.RequestWindowClose();
            return 0;
        }
        catch (Exception exception)
        {
            if (owner is not null)
            {
                owner.LastCallbackException = exception;
                try { owner.Diagnostic?.Invoke("Bridge.ManagedCallbackFailed", exception); } catch { }
            }
            return 1;
        }
        finally { if (owner is not null) --owner.callbackDepth; }
    }
}

/// <summary>程序化天空覆盖状态（颜色为显示线性空间 0..1，太阳方向取自场景方向光）。</summary>
public struct SkyState
{
    public Vector3 Zenith, Horizon, Ground, SunColor;
    public float SunIntensity, SunAngularRadius;
}

public partial class F_Entity
{
    internal F_Entity(EngineSession session, ulong handle) { Session = session; Handle = handle; }
    /// <summary>所属原生引擎会话。</summary>
    public EngineSession Session { get; }
    /// <summary>底层运行时实体句柄（持久化保存请使用 <see cref="StableId"/>）。</summary>
    public ulong Handle { get; }
    /// <summary>获取或设置实体的全局激活状态（Active）；禁用后暂停渲染与脚本更新但不会销毁实体。</summary>
    public bool Enabled
    {
        get=>Session.Invoke(value=>{EngineSession.Check(NativeMethods.GetEntityEnabled(value,Handle,out var enabled));return enabled!=0;});
        set=>Session.Invoke(native=>{EngineSession.Check(NativeMethods.SetEntityEnabled(native,Handle,value?1:0));return 0;});
    }
    /// <summary>检查实体在当前世界中是否仍然存活有效。</summary>
    public bool IsAlive => !Session.IsDisposed && Session.Invoke(value => { EngineSession.Check(NativeMethods.IsAlive(value, Handle, out var alive)); return alive != 0; });
    internal bool IsPending=>!Session.IsDisposed&&Session.Invoke(value=>{EngineSession.Check(NativeMethods.EntityPending(value,Handle,out var pending));return pending!=0;});
    /// <summary>跨保存与重载保持不变的 64 位实体唯一标识符（StableEntityId）。</summary>
    public ulong StableId => Session.Invoke(value => { EngineSession.Check(NativeMethods.GetStableId(value, Handle, out var id)); return id; });
    /// <summary>实体在场景层级面板中的名称。</summary>
    public unsafe string Name => Session.Invoke(value => EngineSession.ReadString((byte* buffer, uint capacity, out uint required) => NativeMethods.GetName(value, Handle, buffer, capacity, out required)));
    /// <summary>获取或设置相对父节点的局部空间完整变换。</summary>
    public Transform Transform
    {
        get => Session.Invoke(value => { EngineSession.Check(NativeMethods.GetTransform(value, Handle, out var transform)); return transform; });
        set => Session.Invoke(native => { EngineSession.Check(NativeMethods.SetTransform(native, Handle, in value)); return 0; });
    }
    /// <summary>读取合成全部父级层级后的世界空间完整变换。</summary>
    public Transform WorldTransform
        => Session.Invoke(value => { EngineSession.Check(NativeMethods.GetWorldTransform(value, Handle, out var transform)); return transform; });
    /// <summary>读取当前父实体；位于场景根层级时返回 null。</summary>
    public Entity? Parent
        => Session.Invoke<Entity?>(value => { EngineSession.Check(NativeMethods.GetParent(value, Handle, out var parent)); return parent == 0 ? null : new Entity(Session, parent); });
    /// <summary>设置父实体（传入 null 解绑回场景根层级）；<paramref name="keepWorld"/> 为 true 时保持世界位姿不变。</summary>
    public void SetParent(F_Entity? parent, bool keepWorld = true)
    {
        if (parent is not null && parent.Session != Session) throw new ArgumentException("Bridge.InvalidEntity");
        Session.Invoke(value => { EngineSession.Check(NativeMethods.SetParent(value, Handle, parent?.Handle ?? 0, keepWorld ? 1 : 0)); return 0; });
    }
    /// <summary>立即销毁该实体及其子层级。</summary>
    public void Destroy() => Session.Invoke(value => { EngineSession.Check(NativeMethods.DestroyEntity(value, Handle)); return 0; });
    /// <summary>在角色模型上播放指定骨骼动画片段（<paramref name="clip"/> 为导入资产内的片段名）。</summary>
    public void PlayAnimation(string clip,float speed=1f,bool loop=false)
        => Session.Invoke(value=>{EngineSession.Check(NativeMethods.ActorPlay(value,StableId,clip,checked((uint)Encoding.UTF8.GetByteCount(clip)),speed,loop?1:0));return 0;});
    /// <summary>查询骨骼关节在世界空间中的当前位置；未找到指定关节时返回 null。</summary>
    public Vector3? JointPosition(string joint)
    {
        return Session.Invoke(value=>
        {
            EngineSession.Check(NativeMethods.JointPosition(value,StableId,joint,checked((uint)Encoding.UTF8.GetByteCount(joint)),out var position,out var found));
            return found!=0?(Vector3?)position:null;
        });
    }
    /// <summary>设置摄像机组件的垂直视场角（弧度制）。</summary>
    public void SetCameraFieldOfView(float radians)
        => Session.Invoke(value=>{EngineSession.Check(NativeMethods.SetCameraFov(value,StableId,radians));return 0;});
    /// <summary>为实体挂载或替换静态网格资产，并配置投射与接收阴影开关。</summary>
    public void SetMesh(string assetId,bool castsShadow=true,bool receivesShadow=true)
        => Session.Invoke(value=>{EngineSession.Check(NativeMethods.SetMesh(value,Handle,assetId,checked((uint)Encoding.UTF8.GetByteCount(assetId)),castsShadow?1:0,receivesShadow?1:0));return 0;});
    /// <summary>将世界空间方向投影到当前摄像机的归一化屏幕 UV（左上原点 0..1）；位于相机背面时返回 null。</summary>
    public Vector2? ProjectDirection(Vector3 direction)
    {
        return Session.Invoke(value=>
        {
            EngineSession.Check(NativeMethods.ProjectDirection(value,StableId,direction,out var uv,out var front));
            return front!=0?(Vector2?)uv:null;
        });
    }
}

/// <summary>运行时场景实体实例。</summary>
public sealed class Entity : F_Entity
{
    internal Entity(EngineSession session, ulong handle) : base(session, handle) { }
}
