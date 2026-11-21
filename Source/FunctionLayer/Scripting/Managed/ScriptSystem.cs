using System.Numerics;
using System.Reflection;

namespace FlareEngine;

public sealed class ScriptSystem : IDisposable
{
    private static bool Overrides(Type type, string name, Type[] parameters)
    {
        var method = type.GetMethod(name, BindingFlags.Instance | BindingFlags.Public, null, parameters, null);
        return method is not null && method.DeclaringType != typeof(F_Behaviour) && method.DeclaringType != typeof(global::SelfEngine.Behaviour);
    }

    private static bool OverridesByRef(Type type, string name, Type valueType)
        => Overrides(type, name, [valueType.MakeByRefType()]);

    private static bool OverridesLegacy(Type type, string name, Type[] parameters)
    {
        var method = type.GetMethod(name, BindingFlags.Instance | BindingFlags.Public, null, parameters, null);
        return method is not null && method.DeclaringType != typeof(global::SelfEngine.Behaviour);
    }

    private sealed class Entry(F_Behaviour value, ScriptComponentBinding native, SceneHandle scene)
    {
        internal readonly F_Behaviour Value = value;
        internal readonly ScriptComponentBinding Native = native;
        internal readonly SceneHandle Scene = scene;
        internal readonly bool IsLegacy = value is global::SelfEngine.Behaviour;
        internal readonly bool HasStart = Overrides(value.GetType(), nameof(F_Behaviour.F_Start), []);
        internal readonly bool HasFPreUpdate = Overrides(value.GetType(), nameof(F_Behaviour.F_PreUpdate), [typeof(float)]) || Overrides(value.GetType(), nameof(F_Behaviour.F_PreUpdate), []);
        internal readonly bool HasFFixedUpdate = Overrides(value.GetType(), nameof(F_Behaviour.F_FixedUpdate), [typeof(float)]) || Overrides(value.GetType(), nameof(F_Behaviour.F_FixedUpdate), []);
        internal readonly bool HasFUpdate = Overrides(value.GetType(), nameof(F_Behaviour.F_Update), [typeof(float)]) || Overrides(value.GetType(), nameof(F_Behaviour.F_Update), []);
        internal readonly bool HasFLateUpdate = Overrides(value.GetType(), nameof(F_Behaviour.F_LateUpdate), [typeof(float)]) || Overrides(value.GetType(), nameof(F_Behaviour.F_LateUpdate), []);
        internal readonly bool HasLegacyPreTick = value is global::SelfEngine.Behaviour && OverridesLegacy(value.GetType(), nameof(global::SelfEngine.Behaviour.OnPreTick), [typeof(double)]);
        internal readonly bool HasLegacyFixedUpdate = value is global::SelfEngine.Behaviour && OverridesLegacy(value.GetType(), nameof(global::SelfEngine.Behaviour.OnFixedUpdate), [typeof(double)]);
        internal readonly bool HasLegacyFixedPostUpdate = value is global::SelfEngine.Behaviour && OverridesLegacy(value.GetType(), nameof(global::SelfEngine.Behaviour.OnFixedPostUpdate), [typeof(double)]);
        internal readonly bool HasLegacyUpdate = value is global::SelfEngine.Behaviour && OverridesLegacy(value.GetType(), nameof(global::SelfEngine.Behaviour.OnUpdate), [typeof(double)]);
        internal readonly bool HasLegacyPostTick = value is global::SelfEngine.Behaviour && OverridesLegacy(value.GetType(), nameof(global::SelfEngine.Behaviour.OnPostTick), [typeof(double)]);
        internal readonly bool HasCollisionEnter = OverridesByRef(value.GetType(), nameof(F_Behaviour.F_CollisionEnter), typeof(F_Contact));
        internal readonly bool HasCollisionStay = OverridesByRef(value.GetType(), nameof(F_Behaviour.F_CollisionStay), typeof(F_Contact));
        internal readonly bool HasCollisionExit = OverridesByRef(value.GetType(), nameof(F_Behaviour.F_CollisionExit), typeof(F_Contact));
        internal readonly bool HasFTriggerEnter = Overrides(value.GetType(), nameof(F_Behaviour.F_TriggerEnter), [typeof(F_Entity)]);
        internal readonly bool HasFTriggerStay = Overrides(value.GetType(), nameof(F_Behaviour.F_TriggerStay), [typeof(F_Entity)]);
        internal readonly bool HasFTriggerExit = Overrides(value.GetType(), nameof(F_Behaviour.F_TriggerExit), [typeof(F_Entity)]);
        internal readonly bool HasLegacyTriggerEnter = value is global::SelfEngine.Behaviour && OverridesLegacy(value.GetType(), nameof(global::SelfEngine.Behaviour.OnTriggerEnter), [typeof(ulong)]);
        internal readonly bool HasLegacyTriggerExit = value is global::SelfEngine.Behaviour && OverridesLegacy(value.GetType(), nameof(global::SelfEngine.Behaviour.OnTriggerExit), [typeof(ulong)]);
        internal bool Created;
        internal bool Started;
        internal bool Active;
        internal bool Faulted;
        internal readonly Dictionary<TickPhase, ulong> Ticks = [];
    }

    private readonly List<Entry> entries = [];
    private readonly Queue<Action> mutations = [];
    private readonly HashSet<ulong> destroying = [];
    private readonly HashSet<(ulong A, ulong B)> activeContacts = [];
    private readonly HashSet<(ulong Trigger, ulong Other)> activeTriggers = [];
    private int dispatchDepth;
    private bool draining;
    private bool running;
    private bool stopping;
    private bool disposed;
    private bool disposeRequested;
    private sealed class PendingAwait(int mode, float remaining, Action continuation)
    {
        internal readonly int Mode = mode;
        internal float Remaining = remaining;
        internal readonly Action Continuation = continuation;
        internal bool SkipCurrentFrame = mode == 1;
    }
    private sealed class ScriptSynchronizationContext(ScriptSystem owner) : SynchronizationContext
    {
        public override void Post(SendOrPostCallback d, object? state)
        {
            if (owner.disposed || owner.stopping) return;
            try { d(state); }
            catch (Exception ex) { owner.Report(null, "Script.CallbackFailed", ex.ToString(), "async"); }
        }
        public override void Send(SendOrPostCallback d, object? state) => Post(d, state);
    }

    internal static ScriptSystem? Current { get; private set; }
    private readonly ScriptSynchronizationContext syncContext;
    internal bool SceneReloadRequested { get; set; }
    private readonly List<(Entity Entity, Action<Entity>? Configure)> pendingSpawns = [];
    private readonly List<(ulong StableId, float Remaining)> delayedDestroys = [];
    private readonly List<PendingAwait> pendingAwaits = [];
    private bool simulationPaused;
    private ScriptInput frameInput = ScriptInput.Empty;
    private ScriptInput fixedInput = ScriptInput.Empty;
    public EngineSession Session { get; }
    public ScriptRegistry Registry { get; }
    public ScriptEvents Events { get; }
    public UiActionBindings UiActions { get; }
    public ScriptCommands Commands { get; }
    public ScriptInput Input { get; private set; } = ScriptInput.Empty;
    public double FixedDeltaSeconds { get; }
    public bool AutomaticFixedUpdates { get; }
    public bool SimulatePhysics { get; }
    public int MaxFixedSteps { get; }
    public ulong DroppedFixedSteps { get; private set; }
    public int Count => entries.Count;
    public bool IsRunning => running;
    public bool SimulationPaused
    {
        get => simulationPaused;
        set { if (simulationPaused == value) return; simulationPaused = value; frameInput = fixedInput = Input = ScriptInput.Empty; if (Session.EnvironmentOwner == this && !Session.IsDisposed) Session.Invoke(handle => { EngineSession.Check(NativeMethods.EnvironmentPaused(handle, value ? 1 : 0)); return 0; }); }
    }
    public event Action<ScriptDiagnostic>? Diagnostic;

    public ScriptSystem(EngineSession session, ScriptRegistry registry, bool automaticFixedUpdates = true,
        double fixedDeltaSeconds = 1.0 / 60.0, int maxFixedSteps = 8, bool simulatePhysics = true)
    {
        if (!double.IsFinite(fixedDeltaSeconds) || fixedDeltaSeconds <= 0 || fixedDeltaSeconds > 1 || maxFixedSteps < 1)
            throw new ArgumentException("Script.InvalidClock");
        Session = session; Registry = registry; AutomaticFixedUpdates = automaticFixedUpdates;
        FixedDeltaSeconds = fixedDeltaSeconds; MaxFixedSteps = maxFixedSteps;
        syncContext = new(this);
        Events = new(this);
        UiActions = new(this);
        Commands = new(this);
        SimulatePhysics = simulatePhysics;
        if (simulatePhysics)
        {
            if (session.Physics.ClockOwner is not null) throw new InvalidOperationException("Script.PhysicsClockOwned");
            session.Physics.ClockOwner = this;
        }
        session.EnvironmentPhaseChanged += OnPhase; session.ComponentTick += OnComponentTick;
        session.SceneRetiring += OnSceneRetiring;
    }
    public T Attach<T>(Entity entity, Action<T>? configure = null) where T : F_Behaviour, new()
    {
        Registry.Register<T>();
        var value = new T(); configure?.Invoke(value);
        Attach(entity, value);
        return value;
    }
    public void Attach(Entity entity, F_Behaviour value)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        if (stopping || entity.Session != Session || (!entity.IsAlive && !entity.IsPending) || value.Owner is not null)
            throw new ArgumentException("Script.InvalidAttachment");
        value.Entity = entity;
        value.Owner = this;
        Mutate(() =>
        {
            if ((!entity.IsAlive && !entity.IsPending) || destroying.Contains(entity.Handle)) { value.Owner = null; return; }
            ScriptComponentBinding binding;
            try { binding = new(entity, value.EnabledValue); } catch { value.Owner = null; throw; }
            var entry = new Entry(value, binding, Session.Scenes.Of(entity)?.Handle ?? default);
            entries.Add(entry);
            if (running) Create(entry);
        });
    }
    public void Start()
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        if (running) return;
        if (Session.EnvironmentOwner is not null && Session.EnvironmentOwner != this) throw new InvalidOperationException("Script.EnvironmentClockOwned");
        Session.Invoke(handle => { EngineSession.Check(NativeMethods.EnvironmentConfigure(handle, FixedDeltaSeconds, checked((uint)MaxFixedSteps), AutomaticFixedUpdates ? 1 : 0, SimulatePhysics ? 1 : 0)); return 0; });
        Session.EnvironmentOwner = this; Current = this; DroppedFixedSteps = 0; running = true; Events.Open();
        F_Time.Reset((float)FixedDeltaSeconds);
        try
        {
            foreach (var entry in entries.ToArray()) Create(entry);
            FlushMutations();
            RunPendingStarts();
            FlushMutations();
            if (simulationPaused) Session.Invoke(handle => { EngineSession.Check(NativeMethods.EnvironmentPaused(handle, 1)); return 0; });
        }
        catch { Stop(); throw; }
    }
    public void SetInput(ScriptInput input)
    {
        frameInput = input;
        fixedInput = fixedInput.MergeEdges(input);
    }
    public void Spawn(string name, Action<Entity>? configure = null) => Mutate(() =>
    {
        if (!stopping) { var entity = Session.CreateEntity(name); if (entity.IsPending) pendingSpawns.Add((entity, configure)); else configure?.Invoke(entity); }
    });
    public void Destroy(Entity entity)
    {
        if (entity.Session != Session) throw new ArgumentException("Script.InvalidAttachment");
        destroying.Add(entity.Handle);
        if (entity.IsAlive || entity.IsPending) entity.Destroy();
        Mutate(() =>
        {
            foreach (var entry in entries.Where(e => e.Value.Entity.Handle == entity.Handle).ToArray()) Remove(entry);
            if (entity.IsAlive || entity.IsPending) entity.Destroy();
            destroying.Remove(entity.Handle);
            SweepDead();
        });
    }
    public void Remove(F_Behaviour value) => Mutate(() =>
    {
        var entry = entries.Find(e => e.Value == value);
        if (entry is not null) Remove(entry);
    });
    public void SetEnabled(F_Behaviour value, bool enabled) => Mutate(() =>
    {
        var entry = entries.Find(e => e.Value == value);
        if (entry is null) return;
        if (entry.Faulted) { if (enabled) Report(value, "Script.FaultedCannotEnable", "", entry.IsLegacy ? "OnEnable" : nameof(F_Behaviour.F_Enable)); return; }
        value.EnabledValue = enabled;
        entry.Native.SetEnabled(enabled);
        RefreshActivation(entry);
    });
    public void FixedUpdate(double deltaSeconds)
    {
        if (AutomaticFixedUpdates) throw new InvalidOperationException("Script.ExternalFixedUpdateDisabled");
        if (!double.IsFinite(deltaSeconds) || deltaSeconds <= 0 || deltaSeconds > 1) throw new ArgumentException("Script.InvalidClock");
        if (!running || SimulationPaused) return;
        Session.Invoke(handle => { EngineSession.Check(NativeMethods.EnvironmentFixedStep(handle, deltaSeconds)); return 0; });
    }
    private void OnPhase(EnvironmentPhase phase, double seconds)
    {
        if (!running || disposed || Session.EnvironmentOwner != this) return;
        if (phase == EnvironmentPhase.Prepare)
        {
            var pending = pendingSpawns.ToArray(); pendingSpawns.Clear();
            foreach (var (entity, configure) in pending)
            {
                if (!running || disposed) break;
                if (entity.IsPending) { pendingSpawns.Add((entity, configure)); continue; }
                if (!entity.IsAlive) continue;
                try { using var scope = Session.EnterSceneCallbackScope(Session.Scenes.Of(entity)?.Handle ?? default); configure?.Invoke(entity); } catch (Exception exception) { Report(null, "Script.CallbackFailed", exception.GetType().Name + ": " + exception.Message, "Spawn"); if (entity.IsAlive) entity.Destroy(); }
            }
        }
        if (phase == EnvironmentPhase.PrepareFinished)
        {
            SweepDead();
            foreach (var entry in entries.ToArray()) Create(entry);
            RefreshActivation();
            RunPendingStarts();
        }
        if (phase == EnvironmentPhase.FrameBegin)
        {
            Current = this;
            if (!simulationPaused) F_Time.AdvanceFrame((float)seconds);
            if (Session.IsWindowRunning) SetInput(Session.WindowInput);
            SweepDead(); RefreshActivation();
            foreach (var uiEvent in Session.Ui.DrainEvents())
            { if (!running || disposed) break; UiActions.Dispatch(uiEvent); if (!running || disposed) break; Events.Publish(uiEvent); }
            RefreshActivation();
            RunPendingStarts();
            Input = frameInput;
            if (!simulationPaused) TickTimersAndAwaits(isFixed: false, F_Time.DeltaTime);
        }
        if (phase is EnvironmentPhase.PreTick or EnvironmentPhase.Tick or EnvironmentPhase.PostTick)
        {
            Current = this;
            RefreshActivation();
            RunPendingStarts();
            Input = frameInput;
        }
        if (phase is EnvironmentPhase.FixedPreTick or EnvironmentPhase.FixedPostTick)
        {
            Current = this;
            F_Time.FixedDeltaTime = (float)FixedDeltaSeconds;
            RefreshActivation();
            RunPendingStarts();
            Input = fixedInput;
            if (phase == EnvironmentPhase.FixedPreTick && !simulationPaused)
                TickTimersAndAwaits(isFixed: true, (float)FixedDeltaSeconds);
        }
        if (phase == EnvironmentPhase.AfterPhysics && SimulatePhysics)
        {
            DispatchContacts(Session.Physics.DrainContacts());
            DispatchTriggers(Session.Physics.DrainTriggers());
        }
        if (phase == EnvironmentPhase.FixedEnd) { fixedInput = fixedInput.WithoutEdges(); Input = frameInput; }
        if (phase == EnvironmentPhase.FrameEnd)
        {
            frameInput = frameInput.WithoutEdges(); Input = frameInput; UpdateClockDiagnostics();
        }
        SweepDead(); FlushMutations();
    }

    private F_Contact BuildContact(Entity self, Entity other)
    {
        var pSelf = self.Transform.Position;
        var pOther = other.Transform.Position;
        var delta = pSelf - pOther;
        var lenSq = delta.LengthSquared();
        var normal = lenSq > 1e-8f ? delta / MathF.Sqrt(lenSq) : Vector3.UnitY;
        var point = (pSelf + pOther) * 0.5f;
        float impulse = 0f;
        try
        {
            var bSelf = Session.Physics.GetBody(self);
            var bOther = Session.Physics.GetBody(other);
            var vSelf = bSelf?.State.LinearVelocity ?? Vector3.Zero;
            var vOther = bOther?.State.LinearVelocity ?? Vector3.Zero;
            impulse = (vSelf - vOther).Length();
        }
        catch { }
        return new F_Contact(other, point, normal, impulse);
    }

    private void DispatchContactPair(Entry[] snapshot, ulong idA, ulong idB, int mode)
    {
        var entityA = Session.FindEntity(idA);
        var entityB = Session.FindEntity(idB);
        if (entityA is null || entityB is null) return;
        foreach (var entry in snapshot)
        {
            if (!running || disposed) break;
            bool wants = mode == 0 ? entry.HasCollisionEnter : mode == 1 ? entry.HasCollisionStay : entry.HasCollisionExit;
            if (!wants || !CanRun(entry.Value)) continue;
            var selfId = entry.Value.Entity.StableId;
            Entity? selfEntity = selfId == idA ? entityA : selfId == idB ? entityB : null;
            Entity? otherEntity = selfId == idA ? entityB : selfId == idB ? entityA : null;
            if (selfEntity is null || otherEntity is null) continue;
            var contact = BuildContact(selfEntity, otherEntity);
            string phaseName = mode == 0 ? nameof(F_Behaviour.F_CollisionEnter)
                : mode == 1 ? nameof(F_Behaviour.F_CollisionStay) : nameof(F_Behaviour.F_CollisionExit);
            Action callback = mode == 0
                ? () => entry.Value.F_CollisionEnter(in contact)
                : mode == 1
                    ? () => entry.Value.F_CollisionStay(in contact)
                    : () => entry.Value.F_CollisionExit(in contact);
            InvokeEvent(entry.Value, callback, phaseName);
            FlushMutations();
        }
    }

    private void DispatchContacts(IReadOnlyList<PhysicsContact> contacts)
    {
        if (contacts.Count == 0 && activeContacts.Count == 0) return;
        var snapshot = entries.ToArray();
        HashSet<(ulong A, ulong B)> touched = [];
        foreach (var contact in contacts)
        {
            if (!running || disposed) break;
            Events.Publish(contact, EventPhase.AfterPhysics);
            if (!running || disposed) break;
            ulong a = contact.EntityA.StableId, b = contact.EntityB.StableId;
            var pair = a <= b ? (a, b) : (b, a);
            if (contact.Began)
            {
                activeContacts.Add(pair);
                touched.Add(pair);
                DispatchContactPair(snapshot, pair.Item1, pair.Item2, 0);
            }
            if (contact.Ended)
            {
                activeContacts.Remove(pair);
                touched.Add(pair);
                DispatchContactPair(snapshot, pair.Item1, pair.Item2, 2);
            }
        }
        if (activeContacts.Count == 0 || !snapshot.Any(e => e.HasCollisionStay)) return;
        foreach (var pair in activeContacts.ToArray())
        {
            if (!running || disposed) break;
            if (touched.Contains(pair)) continue;
            if (Session.FindEntity(pair.A) is null || Session.FindEntity(pair.B) is null)
            {
                activeContacts.Remove(pair);
                continue;
            }
            DispatchContactPair(snapshot, pair.A, pair.B, 1);
        }
    }

    private void DispatchTriggers(IReadOnlyList<PhysicsTrigger> triggers)
    {
        if (triggers.Count == 0 && activeTriggers.Count == 0) return;
        var snapshot = entries.ToArray();
        HashSet<(ulong Trigger, ulong Other)> touched = [];
        foreach (var trigger in triggers)
        {
            if (!running || disposed) break;
            Events.Publish(trigger, EventPhase.AfterPhysics);
            if (!running || disposed) break;
            var triggerId = trigger.TriggerEntity.StableId;
            var otherId = trigger.OtherEntity.StableId;
            var pair = (triggerId, otherId);
            touched.Add(pair);
            if (trigger.Entered) activeTriggers.Add(pair);
            else activeTriggers.Remove(pair);

            foreach (var entry in snapshot)
            {
                if (!running || disposed) break;
                bool hasModern = trigger.Entered ? entry.HasFTriggerEnter : entry.HasFTriggerExit;
                bool hasLegacy = trigger.Entered ? entry.HasLegacyTriggerEnter : entry.HasLegacyTriggerExit;
                if (!hasModern && !hasLegacy) continue;
                if (!CanRun(entry.Value)) continue;
                var entityId = entry.Value.Entity.StableId;
                ulong argument;
                if (entityId == triggerId) argument = otherId;
                else if (entityId == otherId) argument = triggerId;
                else continue;

                if (hasModern && Session.FindEntity(argument) is { } otherEntity)
                {
                    Action modernCallback = trigger.Entered
                        ? () => entry.Value.F_TriggerEnter(otherEntity)
                        : () => entry.Value.F_TriggerExit(otherEntity);
                    InvokeEvent(entry.Value, modernCallback, trigger.Entered ? nameof(F_Behaviour.F_TriggerEnter) : nameof(F_Behaviour.F_TriggerExit));
                    FlushMutations();
                }
                if (hasLegacy && entry.Value is global::SelfEngine.Behaviour legacy && CanRun(entry.Value))
                {
                    Action legacyCallback = trigger.Entered
                        ? () => legacy.OnTriggerEnter(argument)
                        : () => legacy.OnTriggerExit(argument);
                    InvokeEvent(entry.Value, legacyCallback, trigger.Entered ? "OnTriggerEnter" : "OnTriggerExit");
                    FlushMutations();
                }
            }
        }
        if (activeTriggers.Count == 0 || !snapshot.Any(e => e.HasFTriggerStay)) return;
        foreach (var (triggerId, otherId) in activeTriggers.ToArray())
        {
            if (!running || disposed) break;
            if (touched.Contains((triggerId, otherId))) continue;
            var trigEnt = Session.FindEntity(triggerId);
            var otherEnt = Session.FindEntity(otherId);
            if (trigEnt is null || otherEnt is null)
            {
                activeTriggers.Remove((triggerId, otherId));
                continue;
            }
            foreach (var entry in snapshot)
            {
                if (!running || disposed) break;
                if (!entry.HasFTriggerStay || !CanRun(entry.Value)) continue;
                var entityId = entry.Value.Entity.StableId;
                F_Entity? counterpart = entityId == triggerId ? otherEnt : entityId == otherId ? trigEnt : null;
                if (counterpart is null) continue;
                InvokeEvent(entry.Value, () => entry.Value.F_TriggerStay(counterpart), nameof(F_Behaviour.F_TriggerStay));
                FlushMutations();
            }
        }
    }

    private void UpdateClockDiagnostics()
    {
        if (Session.IsDisposed || Session.EnvironmentOwner != this) return;
        double count = Math.Round(Session.SimulationClock.DroppedSeconds / FixedDeltaSeconds);
        ulong dropped = count >= ulong.MaxValue ? ulong.MaxValue : (ulong)Math.Max(0, count);
        if (dropped > DroppedFixedSteps) Report(null, "Script.FixedStepBudget", (dropped - DroppedFixedSteps).ToString(System.Globalization.CultureInfo.InvariantCulture));
        DroppedFixedSteps = dropped;
    }

    private void RegisterTicks(Entry entry)
    {
        if (!running || entry.Faulted || !entries.Contains(entry) || !entry.Native.IsAlive || entry.Ticks.Count != 0) return;
        void Register(bool active, TickPhase phase, bool paused = false)
        {
            if (!active) return;
            var token = Session.Invoke(handle => { EngineSession.Check(NativeMethods.ComponentTickRegister(handle, entry.Native.Handle, (uint)phase, phase is TickPhase.FixedPreTick or TickPhase.FixedPostTick ? 1 : 0, paused ? 1 : 0, out var task)); return task; });
            entry.Ticks.Add(phase, token);
        }
        Register(entry.HasFPreUpdate || entry.HasLegacyPreTick, TickPhase.PreTick, entry.Value.UpdateWhenPaused);
        Register(entry.HasFFixedUpdate || entry.HasLegacyFixedUpdate, TickPhase.FixedPreTick);
        Register(entry.HasLegacyFixedPostUpdate, TickPhase.FixedPostTick);
        Register(entry.HasFUpdate || entry.HasLegacyUpdate, entry.Value.UpdateWhenPaused ? TickPhase.FrameBegin : TickPhase.Tick, entry.Value.UpdateWhenPaused);
        Register(entry.HasFLateUpdate || entry.HasLegacyPostTick, TickPhase.PostTick, entry.Value.UpdateWhenPaused);
    }

    private void UnregisterTicks(Entry entry)
    {
        foreach (var token in entry.Ticks.Values)
        {
            if (Session.IsDisposed) break;
            try { Session.Invoke(handle => { EngineSession.Check(NativeMethods.ComponentTickRemove(handle, token)); return 0; }); }
            catch (EngineException exception) when (exception.Key == "Tick.InvalidTask") { }
        }
        entry.Ticks.Clear();
    }

    public unsafe void SetTickPrerequisites(F_Behaviour value, TickPhase phase, params TickDependency[] prerequisites)
    {
        var entry = entries.Find(e => e.Value == value);
        if (entry is null || !entry.Ticks.TryGetValue(phase, out var task)) throw new InvalidOperationException("Tick.InvalidTask");
        var native = new NativeMethods.TickPrerequisite[prerequisites.Length];
        for (int i = 0; i < native.Length; ++i)
        {
            var prior = entries.Find(e => e.Value == prerequisites[i].Behaviour);
            if (prior is null || !prior.Ticks.TryGetValue(prerequisites[i].Phase, out var token)) throw new InvalidOperationException("Tick.InvalidPrerequisite");
            native[i] = new() { Task = token, Mode = prerequisites[i].RequireFresh ? 1u : 0u, Reserved = 0 };
        }
        Session.Invoke(handle => { fixed (NativeMethods.TickPrerequisite* buffer = native) EngineSession.Check(NativeMethods.ComponentTickPrerequisites(handle, task, buffer, checked((uint)native.Length))); return 0; });
    }

    private void OnComponentTick(ulong component, TickPhase phase, double seconds)
    {
        if (!running || disposed) return;
        // 原生调度器已决定暂停任务及编辑器单步；这里仍校验生命周期，不能再次拦截强制单步。
        var entry = entries.Find(e => e.Native.Handle == component);
        if (entry is null || !CanRun(entry.Value, true)) return;
        if (!entry.Started)
        {
            entry.Started = true;
            if (entry.HasStart)
            {
                Invoke(entry, entry.Value.F_Start, phase: nameof(F_Behaviour.F_Start));
                if (!CanRun(entry.Value, true)) { SweepDead(); FlushMutations(); return; }
            }
        }
        float delta = (float)seconds;
        switch (phase)
        {
            case TickPhase.PreTick:
                if (entry.HasFPreUpdate) Invoke(entry, () => entry.Value.F_PreUpdate(delta), phase: nameof(F_Behaviour.F_PreUpdate));
                if (entry.HasLegacyPreTick && !entry.Faulted && entry.Value is global::SelfEngine.Behaviour lp) Invoke(entry, () => lp.OnPreTick(seconds), phase: "OnPreTick");
                break;
            case TickPhase.FixedPreTick:
                if (entry.HasFFixedUpdate) Invoke(entry, () => entry.Value.F_FixedUpdate(delta), phase: nameof(F_Behaviour.F_FixedUpdate));
                if (entry.HasLegacyFixedUpdate && !entry.Faulted && entry.Value is global::SelfEngine.Behaviour lf) Invoke(entry, () => lf.OnFixedUpdate(seconds), phase: "OnFixedUpdate");
                break;
            case TickPhase.FixedPostTick:
                if (entry.HasLegacyFixedPostUpdate && entry.Value is global::SelfEngine.Behaviour lfp) Invoke(entry, () => lfp.OnFixedPostUpdate(seconds), phase: "OnFixedPostUpdate");
                break;
            case TickPhase.PostTick:
                if (entry.HasFLateUpdate) Invoke(entry, () => entry.Value.F_LateUpdate(delta), phase: nameof(F_Behaviour.F_LateUpdate));
                if (entry.HasLegacyPostTick && !entry.Faulted && entry.Value is global::SelfEngine.Behaviour lpt) Invoke(entry, () => lpt.OnPostTick(seconds), phase: "OnPostTick");
                break;
            default:
                if (entry.HasFUpdate) Invoke(entry, () => entry.Value.F_Update(delta), phase: nameof(F_Behaviour.F_Update));
                if (entry.HasLegacyUpdate && !entry.Faulted && entry.Value is global::SelfEngine.Behaviour lu) Invoke(entry, () => lu.OnUpdate(seconds), phase: "OnUpdate");
                break;
        }
        SweepDead(); FlushMutations();
    }

    private void Create(Entry entry)
    {
        if (entry.Created || !entry.Value.Entity.IsAlive || !entry.Native.IsAlive || destroying.Contains(entry.Value.Entity.Handle)) return;
        entry.Created = true;
        Invoke(entry, entry.Value.F_Awake, phase: entry.IsLegacy ? "OnCreate" : nameof(F_Behaviour.F_Awake));
        RefreshActivation(entry);
        RegisterTicks(entry);
    }

    private void RunPendingStarts()
    {
        if (!running || disposed) return;
        foreach (var entry in entries.ToArray())
        {
            if (!running || disposed) break;
            if (!entry.Created || entry.Started || !entry.Active || entry.Faulted || !entries.Contains(entry)) continue;
            entry.Started = true;
            if (entry.HasStart)
                Invoke(entry, entry.Value.F_Start, phase: nameof(F_Behaviour.F_Start));
        }
    }

    private void Remove(Entry entry)
    {
        if (!entries.Remove(entry)) return;
        UnregisterTicks(entry);
        entry.Native.SetEnabled(false);
        if (entry.Active) { entry.Active = false; Invoke(entry, entry.Value.F_Disable, false, entry.IsLegacy ? "OnDisable" : nameof(F_Behaviour.F_Disable)); }
        if (entry.Created) { entry.Created = false; Invoke(entry, entry.Value.F_Destroy, false, entry.IsLegacy ? "OnDestroy" : nameof(F_Behaviour.F_Destroy)); }
        Events.Remove(entry.Value);
        UiActions.Remove(entry.Value);
        Commands.Remove(entry.Value);
        entry.Native.Remove();
        entry.Value.Owner = null;
        if (!entries.Any(other => other.Value.Entity.Handle == entry.Value.Entity.Handle)) destroying.Remove(entry.Value.Entity.Handle);
    }

    private void SweepDead()
    {
        if (Session.IsDisposed) return;
        foreach (var entry in entries.ToArray()) if ((!entry.Value.Entity.IsAlive && !entry.Value.Entity.IsPending) || (!entry.Native.IsAlive && !entry.Native.Pending)) Remove(entry);
    }

    private void OnSceneRetiring(SceneRetirement retirement)
    {
        if (disposed) return;
        foreach (var entry in entries.ToArray())
        {
            if (retirement.Members.Contains(entry.Value.Entity.Handle))
            { destroying.Add(entry.Value.Entity.Handle); entry.Value.EnabledValue = false; entry.Native.SetEnabled(false); Mutate(() => Remove(entry)); }
            else ScriptFields.ClearSceneReferences(entry.Value, retirement.Invalidated);
        }
    }

    private void RefreshActivation()
    { if (!running || disposed) return; foreach (var entry in entries.ToArray()) RefreshActivation(entry); }

    private void RefreshActivation(Entry entry)
    {
        if (!entry.Created || !entries.Contains(entry)) return;
        bool active = running && !entry.Faulted && entry.Value.EnabledValue && !destroying.Contains(entry.Value.Entity.Handle) && entry.Value.Entity.IsAlive && entry.Native.Active;
        if (active == entry.Active) return;
        entry.Active = active;
        string phaseName = active
            ? (entry.IsLegacy ? "OnEnable" : nameof(F_Behaviour.F_Enable))
            : (entry.IsLegacy ? "OnDisable" : nameof(F_Behaviour.F_Disable));
        Invoke(entry, active ? entry.Value.F_Enable : entry.Value.F_Disable, phase: phaseName);
    }

    internal bool CanRun(F_Behaviour value) => CanRun(value, false);
    private bool CanRun(F_Behaviour value, bool scheduledTick)
    {
        var entry = entries.Find(e => e.Value == value);
        return running && value.EnabledValue && (scheduledTick || !SimulationPaused || value.UpdateWhenPaused) && entry is { Created: true, Active: true, Faulted: false }
            && !destroying.Contains(value.Entity.Handle) && value.Entity.IsAlive && entry.Native.Active;
    }

    internal ulong ComponentHandle(F_Behaviour value) => entries.Find(entry => entry.Value == value)?.Native.Handle ?? throw new ArgumentException("Script.NotAttached");
    internal bool InvokeEvent(F_Behaviour value, Action action, string phase = "Event")
    {
        var entry = entries.Find(e => e.Value == value);
        if (entry is null) return false; Invoke(entry, action, phase: phase); return !entry.Faulted;
    }

    private void Invoke(Entry entry, Action action, bool disableOnError = true, string phase = "")
    {
        var prevCurrent = Current;
        var prevSync = SynchronizationContext.Current;
        Current = this;
        SynchronizationContext.SetSynchronizationContext(syncContext);
        ++dispatchDepth;
        try { using var scope = Session.EnterSceneCallbackScope(entry.Scene, entry.Native.Handle); action(); }
        catch (Exception exception)
        {
            Report(entry.Value, "Script.CallbackFailed", exception.ToString(), phase);
            if (disableOnError)
            {
                entry.Faulted = true;
                entry.Value.EnabledValue = false;
                entry.Native.SetEnabled(false);
                if (entry.Active)
                {
                    entry.Active = false;
                    try { entry.Value.F_Disable(); } catch (Exception disabled) { Report(entry.Value, "Script.CallbackFailed", disabled.GetType().Name + ": " + disabled.Message, entry.IsLegacy ? "OnDisable" : nameof(F_Behaviour.F_Disable)); }
                }
            }
        }
        finally
        {
            --dispatchDepth;
            SynchronizationContext.SetSynchronizationContext(prevSync);
            Current = prevCurrent ?? (running ? this : null);
        }
    }

    internal void ScheduleDestroy(ulong stableId, float delaySeconds)
    {
        if (!running || stopping || disposed) return;
        delayedDestroys.Add((stableId, MathF.Max(0f, delaySeconds)));
    }

    internal void EnqueueAwait(int mode, float seconds, Action continuation)
    {
        if (!running || stopping || disposed) return;
        pendingAwaits.Add(new PendingAwait(mode, MathF.Max(0f, seconds), continuation));
    }

    private void TickTimersAndAwaits(bool isFixed, float dt)
    {
        if (!isFixed && delayedDestroys.Count != 0)
        {
            for (int i = delayedDestroys.Count - 1; i >= 0; --i)
            {
                var (stableId, rem) = delayedDestroys[i];
                rem -= dt;
                if (rem <= 0f)
                {
                    delayedDestroys.RemoveAt(i);
                    if (Session.FindEntity(stableId) is { IsAlive: true } target) Destroy(target);
                }
                else delayedDestroys[i] = (stableId, rem);
            }
        }
        if (pendingAwaits.Count == 0) return;
        var ready = new List<Action>();
        for (int i = 0; i < pendingAwaits.Count; ++i)
        {
            var item = pendingAwaits[i];
            if (isFixed)
            {
                if (item.Mode != 2) continue;
                ready.Add(item.Continuation);
                pendingAwaits.RemoveAt(i--);
            }
            else if (item.Mode == 1)
            {
                if (item.SkipCurrentFrame) { item.SkipCurrentFrame = false; continue; }
                ready.Add(item.Continuation);
                pendingAwaits.RemoveAt(i--);
            }
            else if (item.Mode == 0)
            {
                item.Remaining -= dt;
                if (item.Remaining <= 0f)
                {
                    ready.Add(item.Continuation);
                    pendingAwaits.RemoveAt(i--);
                }
            }
        }
        var prevSync = SynchronizationContext.Current;
        SynchronizationContext.SetSynchronizationContext(syncContext);
        try
        {
            foreach (var cont in ready)
            {
                if (!running || stopping || disposed) break;
                ++dispatchDepth;
                try { cont(); }
                catch (Exception ex) { Report(null, "Script.CallbackFailed", ex.ToString(), "F_Await"); }
                finally { --dispatchDepth; }
                FlushMutations();
            }
        }
        finally
        {
            SynchronizationContext.SetSynchronizationContext(prevSync);
        }
    }

    private void Mutate(Action action)
    {
        ObjectDisposedException.ThrowIf(disposed, this);
        mutations.Enqueue(action);
        FlushMutations();
    }

    internal void FlushMutations()
    {
        if (dispatchDepth != 0 || draining) return;
        draining = true;
        try
        {
            int count = 0;
            while (mutations.Count != 0)
            {
                if (++count > 10000) { mutations.Clear(); throw new InvalidOperationException("Script.MutationBudget"); }
                mutations.Dequeue()();
            }
        }
        finally { draining = false; }
    }

    internal void Report(F_Behaviour? value, string key, string detail, string phase = "")
    {
        try { Diagnostic?.Invoke(new(key, value?.Entity.Handle ?? 0, value is null ? "" : ScriptRegistry.IdOf(value.GetType()), detail, phase)); }
        catch { }
    }

    public void Stop()
    {
        if (stopping || disposed) return;
        if (dispatchDepth != 0) { mutations.Enqueue(Stop); return; }
        stopping = true;
        SceneReloadRequested = false;
        UpdateClockDiagnostics();
        running = false;
        foreach (var entry in entries.ToArray()) Remove(entry);
        mutations.Clear(); destroying.Clear(); activeContacts.Clear(); activeTriggers.Clear();
        Events.Clear(); UiActions.Clear(); Commands.Clear();
        delayedDestroys.Clear(); pendingAwaits.Clear();
        if (Current == this) Current = null;
        foreach (var (entity, _) in pendingSpawns) if (entity.IsPending) entity.Destroy(); pendingSpawns.Clear();
        if (Session.EnvironmentOwner == this) { if (!Session.IsDisposed) Session.Invoke(handle => { EngineSession.Check(NativeMethods.EnvironmentRelease(handle)); return 0; }); Session.EnvironmentOwner = null; }
        frameInput = fixedInput = Input = ScriptInput.Empty;
        simulationPaused = false;
        stopping = false;
        if (disposeRequested) CompleteDispose();
    }

    public void Dispose()
    {
        if (disposed) return;
        // Stop 会取消普通结构修改；终止请求必须独立保存，不能随 mutations 被清空。
        if (dispatchDepth != 0 || stopping) { disposeRequested = true; if (!stopping) mutations.Enqueue(Dispose); return; }
        Stop();
        CompleteDispose();
    }

    private void CompleteDispose()
    {
        if (disposed) return;
        disposed = true; disposeRequested = false;
        Session.EnvironmentPhaseChanged -= OnPhase; Session.ComponentTick -= OnComponentTick;
        Session.SceneRetiring -= OnSceneRetiring;
        if (Session.Physics.ClockOwner == this) Session.Physics.ClockOwner = null;
    }

    internal IReadOnlyList<F_Behaviour> Attachments => entries.Select(e => e.Value).ToArray();
}
