using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;

namespace FlareEngine;

/// <summary>音频播放参数描述（位置、音量、音高、3D 衰减与循环开关）。</summary>
[StructLayout(LayoutKind.Sequential)]
public struct F_AudioPlayDescription
{
    public Vector3 Position;
    public float Volume;
    public float Pitch;
    public float MinDistanceMeters;
    public float MaxDistanceMeters;
    public float SpatialBlend;
    private int loop;
    public int Priority;

    public bool Loop
    {
        readonly get => loop != 0;
        set => loop = value ? 1 : 0;
    }

    public static F_AudioPlayDescription Default => AudioPlayDescription.Default;
    public static F_AudioPlayDescription Spatial(Vector3 position, float volume = 1.0f, float minDistance = 1.0f, float maxDistance = 30.0f, bool loop = false)
        => AudioPlayDescription.Spatial(position, volume, minDistance, maxDistance, loop);

    public static implicit operator AudioPlayDescription(F_AudioPlayDescription v) => new()
    {
        Position = v.Position, Volume = v.Volume, Pitch = v.Pitch,
        MinDistanceMeters = v.MinDistanceMeters, MaxDistanceMeters = v.MaxDistanceMeters,
        SpatialBlend = v.SpatialBlend, Loop = v.Loop, Priority = v.Priority
    };
    public static implicit operator F_AudioPlayDescription(AudioPlayDescription v) => new()
    {
        Position = v.Position, Volume = v.Volume, Pitch = v.Pitch,
        MinDistanceMeters = v.MinDistanceMeters, MaxDistanceMeters = v.MaxDistanceMeters,
        SpatialBlend = v.SpatialBlend, Loop = v.Loop, Priority = v.Priority
    };
}

[StructLayout(LayoutKind.Sequential)]
public struct AudioPlayDescription
{
    public Vector3 Position;
    public float Volume;
    public float Pitch;
    public float MinDistanceMeters;
    public float MaxDistanceMeters;
    public float SpatialBlend;
    private int loop;
    public int Priority;

    public bool Loop
    {
        readonly get => loop != 0;
        set => loop = value ? 1 : 0;
    }

    public static AudioPlayDescription Default => new()
    {
        Position = Vector3.Zero,
        Volume = 1.0f,
        Pitch = 1.0f,
        MinDistanceMeters = 1.0f,
        MaxDistanceMeters = 30.0f,
        SpatialBlend = 0.0f,
        Loop = false,
        Priority = 0
    };

    public static AudioPlayDescription Spatial(
        Vector3 position,
        float volume = 1.0f,
        float minDistance = 1.0f,
        float maxDistance = 30.0f,
        bool loop = false) => new()
    {
        Position = position,
        Volume = volume,
        Pitch = 1.0f,
        MinDistanceMeters = minDistance,
        MaxDistanceMeters = maxDistance,
        SpatialBlend = 1.0f,
        Loop = loop,
        Priority = 0
    };
}

/// <summary>活跃音频声部句柄。</summary>
public readonly record struct F_AudioVoiceHandle(ulong Handle)
{
    public bool IsValid => Handle != 0;
    public static implicit operator AudioVoiceHandle(F_AudioVoiceHandle v) => new(v.Handle);
    public static implicit operator F_AudioVoiceHandle(AudioVoiceHandle v) => new(v.Handle);
}

public readonly record struct AudioVoiceHandle(ulong Handle)
{
    public bool IsValid => Handle != 0;
}

/// <summary>音频播放与 3D 空间音源控制服务。</summary>
public unsafe class F_Audio
{
    private readonly EngineSession session;

    internal F_Audio(EngineSession session) => this.session = session;

    /// <summary>获取或设置全局主音量（0..1）。</summary>
    public float MasterVolume
    {
        get => session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.AudioGetMasterVolume(handle, out var volume));
            return volume;
        });
        set => session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.AudioSetMasterVolume(handle, value));
            return 0;
        });
    }

    /// <summary>当前正在发声的活跃音频声部数量。</summary>
    public uint ActiveVoiceCount => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioActiveVoiceCount(handle, out var count));
        return count;
    });

    /// <summary>按完整参数描述播放指定音频资产并返回声部句柄。</summary>
    public AudioVoiceHandle Play(string assetId, AudioPlayDescription description)
    {
        ArgumentException.ThrowIfNullOrEmpty(assetId);
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.AudioPlay(
                handle,
                assetId,
                checked((uint)Encoding.UTF8.GetByteCount(assetId)),
                in description,
                out var voice));
            return new AudioVoiceHandle(voice);
        });
    }

    /// <summary>播放单次 2D 音效（不循环）。</summary>
    public AudioVoiceHandle PlayOneShot(string assetId, float volume = 1.0f)
    {
        var description = AudioPlayDescription.Default;
        description.Volume = volume;
        return Play(assetId, description);
    }

    /// <summary>在指定三维世界坐标播放单次 3D 空间衰减音效。</summary>
    public AudioVoiceHandle PlayOneShotAt(
        string assetId,
        Vector3 position,
        float volume = 1.0f,
        float minDistance = 1.0f,
        float maxDistance = 30.0f) =>
        Play(assetId, AudioPlayDescription.Spatial(position, volume, minDistance, maxDistance, loop: false));

    /// <summary>立即停止指定音频声部。</summary>
    public void Stop(AudioVoiceHandle voice) => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioStopVoice(handle, voice.Handle));
        return 0;
    });

    /// <summary>检查指定音频声部是否仍在播放中。</summary>
    public bool IsPlaying(AudioVoiceHandle voice) => voice.IsValid && session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioIsVoicePlaying(handle, voice.Handle, out var playing));
        return playing != 0;
    });

    /// <summary>动态更新活动声部的位置、音量、音高或衰减参数。</summary>
    public void UpdateVoice(AudioVoiceHandle voice, AudioPlayDescription description) => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioSetVoiceParameters(handle, voice.Handle, in description));
        return 0;
    });

    /// <summary>触发实体上挂载的 AudioSource 组件开始播放。</summary>
    public void PlaySource(F_Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.AudioPlaySource(handle, entity.Handle));
            return 0;
        });
    }

    /// <summary>停止实体上挂载的 AudioSource 组件播放。</summary>
    public void StopSource(F_Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.AudioStopSource(handle, entity.Handle));
            return 0;
        });
    }

    /// <summary>查询实体上挂载的 AudioSource 组件当前是否正在播放。</summary>
    public bool IsSourcePlaying(F_Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        return session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.AudioIsSourcePlaying(handle, entity.Handle, out var playing));
            return playing != 0;
        });
    }

    public void SetPaused(bool paused) => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioSetPaused(handle, paused ? 1 : 0));
        return 0;
    });

    public void StopAll() => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioStopAll(handle));
        return 0;
    });

    public void UseNullBackend(bool forceNull = true) => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioUseNullBackend(handle, forceNull ? 1 : 0));
        return 0;
    });
}

/// <summary>音频播放与 3D 空间音源控制服务（兼容无前缀写法，继承自 <see cref="F_Audio"/>）。</summary>
public sealed unsafe class Audio : F_Audio
{
    internal Audio(EngineSession session) : base(session) { }
}
