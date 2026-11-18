using System.Numerics;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;
using System.Text;

namespace SelfEngine;

public enum QualityLevel { Project, Performance, Balanced, High }

[StructLayout(LayoutKind.Sequential)]
public struct Transform
{
    public Vector3 Position;
    public Quaternion Rotation;
    public Vector3 Scale;
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
}

// W07：程序化天空覆盖。颜色是显示线性 0..1；太阳方向取场景方向光，不在这里重复定义。
public struct SkyState
{
    public Vector3 Zenith, Horizon, Ground, SunColor;
    public float SunIntensity, SunAngularRadius;
}

public sealed class Entity
{    internal Entity(EngineSession session, ulong handle) { Session = session; Handle = handle; }
    public EngineSession Session { get; }
    public ulong Handle { get; }
    public bool Enabled
    {
        get=>Session.Invoke(value=>{EngineSession.Check(NativeMethods.GetEntityEnabled(value,Handle,out var enabled));return enabled!=0;});
        set=>Session.Invoke(native=>{EngineSession.Check(NativeMethods.SetEntityEnabled(native,Handle,value?1:0));return 0;});
    }
    public bool IsAlive => !Session.IsDisposed && Session.Invoke(value => { EngineSession.Check(NativeMethods.IsAlive(value, Handle, out var alive)); return alive != 0; });
    internal bool IsPending=>!Session.IsDisposed&&Session.Invoke(value=>{EngineSession.Check(NativeMethods.EntityPending(value,Handle,out var pending));return pending!=0;});
    public ulong StableId => Session.Invoke(value => { EngineSession.Check(NativeMethods.GetStableId(value, Handle, out var id)); return id; });
    public unsafe string Name => Session.Invoke(value => EngineSession.ReadString((byte* buffer, uint capacity, out uint required) => NativeMethods.GetName(value, Handle, buffer, capacity, out required)));
    public Transform Transform
    {
        get => Session.Invoke(value => { EngineSession.Check(NativeMethods.GetTransform(value, Handle, out var transform)); return transform; });
        set => Session.Invoke(native => { EngineSession.Check(NativeMethods.SetTransform(native, Handle, in value)); return 0; });
    }
    public void Destroy() => Session.Invoke(value => { EngineSession.Check(NativeMethods.DestroyEntity(value, Handle)); return 0; });
    // W03：角色播放与关节查询。clip 名来自资产；关节不存在返回 null，不是异常。
    public void PlayAnimation(string clip,float speed=1f,bool loop=false)
        => Session.Invoke(value=>{EngineSession.Check(NativeMethods.ActorPlay(value,StableId,clip,checked((uint)Encoding.UTF8.GetByteCount(clip)),speed,loop?1:0));return 0;});
    public Vector3? JointPosition(string joint)
    {
        return Session.Invoke(value=>
        {
            EngineSession.Check(NativeMethods.JointPosition(value,StableId,joint,checked((uint)Encoding.UTF8.GetByteCount(joint)),out var position,out var found));
            return found!=0?(Vector3?)position:null;
        });
    }
    public void SetCameraFieldOfView(float radians)
        => Session.Invoke(value=>{EngineSession.Check(NativeMethods.SetCameraFov(value,StableId,radians));return 0;});
    // 挂接或替换静态网格；运行时可用来布景和生成植株。
    public void SetMesh(string assetId,bool castsShadow=true,bool receivesShadow=true)
        => Session.Invoke(value=>{EngineSession.Check(NativeMethods.SetMesh(value,Handle,assetId,checked((uint)Encoding.UTF8.GetByteCount(assetId)),castsShadow?1:0,receivesShadow?1:0));return 0;});
    // W07：世界方向投影到相机归一化屏幕坐标（左上原点，0..1）；不在前方返回 null。
    public Vector2? ProjectDirection(Vector3 direction)
    {
        return Session.Invoke(value=>
        {
            EngineSession.Check(NativeMethods.ProjectDirection(value,StableId,direction,out var uv,out var front));
            return front!=0?(Vector2?)uv:null;
        });
    }
}
