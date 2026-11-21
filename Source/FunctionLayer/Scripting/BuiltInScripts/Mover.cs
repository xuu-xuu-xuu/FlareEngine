using FlareEngine;

namespace ProjectScripts;

[F_Script("sample.project_mover")]
public sealed class Mover : F_Behaviour
{
    [F_Expose] public float Speed = 2;
    public override void F_Awake() => Subscribe<MenuSettingsChanged>(settings => Speed = settings.MoveSpeed);
    public override void F_Update(float delta)
    {
        var transform = Entity.Transform;
        transform.Position.X += Speed * delta;
        Entity.Transform = transform;
    }
}

[F_Script("sample.project_rotator")]
public sealed class Rotator : F_Behaviour
{
    [F_Expose] public float Speed = 1;
    public override void F_Update(float delta)
    {
        var transform = Entity.Transform;
        transform.Rotation = System.Numerics.Quaternion.Normalize(System.Numerics.Quaternion.CreateFromAxisAngle(System.Numerics.Vector3.UnitY, Speed * delta) * transform.Rotation);
        Entity.Transform = transform;
    }
}
