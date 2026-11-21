using FlareEngine;
using System.Numerics;
namespace FlareGame;

[F_Script("renderingstudio.spin")]
public sealed class Spin : F_Behaviour
{
    [F_Expose] public float RadiansPerSecond=1;
    public override void F_Update(float delta)
    {
        var transform=Transform;
        transform.Rotation=Quaternion.Normalize(Quaternion.CreateFromAxisAngle(Vector3.UnitY,RadiansPerSecond*delta)*transform.Rotation);
        Transform=transform;
    }
}
