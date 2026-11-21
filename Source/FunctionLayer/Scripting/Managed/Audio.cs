using System.Numerics;
using System.Runtime.InteropServices;
using System.Text;

namespace FlareEngine;

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

public readonly record struct AudioVoiceHandle(ulong Handle)
{
    public bool IsValid => Handle != 0;
}

public sealed unsafe class Audio
{
    private readonly EngineSession session;

    internal Audio(EngineSession session) => this.session = session;

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

    public uint ActiveVoiceCount => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioActiveVoiceCount(handle, out var count));
        return count;
    });

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

    public AudioVoiceHandle PlayOneShot(string assetId, float volume = 1.0f)
    {
        var description = AudioPlayDescription.Default;
        description.Volume = volume;
        return Play(assetId, description);
    }

    public AudioVoiceHandle PlayOneShotAt(
        string assetId,
        Vector3 position,
        float volume = 1.0f,
        float minDistance = 1.0f,
        float maxDistance = 30.0f) =>
        Play(assetId, AudioPlayDescription.Spatial(position, volume, minDistance, maxDistance, loop: false));

    public void Stop(AudioVoiceHandle voice) => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioStopVoice(handle, voice.Handle));
        return 0;
    });

    public bool IsPlaying(AudioVoiceHandle voice) => voice.IsValid && session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioIsVoicePlaying(handle, voice.Handle, out var playing));
        return playing != 0;
    });

    public void UpdateVoice(AudioVoiceHandle voice, AudioPlayDescription description) => session.Invoke(handle =>
    {
        EngineSession.Check(NativeMethods.AudioSetVoiceParameters(handle, voice.Handle, in description));
        return 0;
    });

    public void PlaySource(Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.AudioPlaySource(handle, entity.Handle));
            return 0;
        });
    }

    public void StopSource(Entity entity)
    {
        if (entity.Session != session) throw new ArgumentException("Bridge.InvalidEntity");
        session.Invoke(handle =>
        {
            EngineSession.Check(NativeMethods.AudioStopSource(handle, entity.Handle));
            return 0;
        });
    }

    public bool IsSourcePlaying(Entity entity)
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
