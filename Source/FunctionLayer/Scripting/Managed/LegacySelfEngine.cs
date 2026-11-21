namespace SelfEngine;

[AttributeUsage(AttributeTargets.Class, Inherited = false)]
public sealed class ScriptIdAttribute(string id) : global::FlareEngine.F_ScriptAttribute(id);

[AttributeUsage(AttributeTargets.Field)]
public sealed class ExposeAttribute : global::FlareEngine.F_ExposeAttribute;

public abstract class Behaviour : global::FlareEngine.F_Behaviour
{
    public override void F_Awake() => OnCreate();
    public override void F_Enable() => OnEnable();
    public override void F_Disable() => OnDisable();
    public override void F_Destroy() => OnDestroy();

    public virtual void OnCreate() { }
    public virtual void OnEnable() { }
    public virtual void OnUpdate(double deltaSeconds) { }
    public virtual void OnPreTick(double deltaSeconds) { }
    public virtual void OnPostTick(double deltaSeconds) { }
    public virtual void OnFixedUpdate(double deltaSeconds) { }
    public virtual void OnFixedPostUpdate(double deltaSeconds) { }
    public virtual void OnTriggerEnter(ulong otherEntityId) { }
    public virtual void OnTriggerExit(ulong otherEntityId) { }
    public virtual void OnDisable() { }
    public virtual void OnDestroy() { }
}
