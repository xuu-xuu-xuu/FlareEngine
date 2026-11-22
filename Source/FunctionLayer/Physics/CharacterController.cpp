#include "FunctionLayer/Physics/PhysicsWorld.h"

#include <algorithm>
#include <array>
#include <cmath>

namespace flare
{
bool CharacterController::Move(PhysicsWorld& world,Vec3 horizontalDirection,bool jumpPressed,float seconds)
{
    if(!IsFinite(position)||!IsFinite(horizontalDirection)||!IsFinite(radius)||radius<=0.001f
        ||!IsFinite(segmentHalfHeight)||segmentHalfHeight<0||!IsFinite(verticalVelocity)
        ||!IsFinite(walkSpeed)||walkSpeed<0||!IsFinite(jumpSpeed)||jumpSpeed<0
        ||!IsFinite(maximumPushImpulse)||maximumPushImpulse<0||!IsFinite(seconds)||seconds<=0||seconds>0.1f
        ||!IsFinite(world.gravity))return false;
    horizontalDirection.y=0;const float inputLength=Length(horizontalDirection);
    if(!IsFinite(inputLength))return false;
    if(inputLength>1)horizontalDirection=horizontalDirection/inputLength;
    const float nextVertical=(jumpPressed&&grounded?jumpSpeed:verticalVelocity)+world.gravity.y*seconds;
    const Vec3 displacement=(horizontalDirection*walkSpeed+Vec3{0,nextVertical,0})*seconds;
    const float count=std::ceil(Length(displacement)/(radius*0.25f));
    if(!IsFinite(nextVertical)||!IsFinite(count)||count>128)return false;
    const unsigned steps=static_cast<unsigned>(std::max(1.0f,count));
    const Vec3 increment=displacement/static_cast<float>(steps);
    // 超出分段预算时不留下半次跳跃。
    verticalVelocity=nextVertical;
    float pushBudget=maximumPushImpulse;grounded=false;
    const auto bodies=world.Bodies();
    for(unsigned step=0;step<steps;++step)
    {
        position+=increment;
        for(unsigned iteration=0;iteration<8;++iteration)
        {
            bool corrected=false;
            for(const auto handle:bodies)
            {
                const auto* body=world.GetBody(handle);if(!body||body->description.isTrigger)continue;
                const auto contact=body->description.shapeKind==ColliderShapeKind::Sphere
                    ?CollideCapsuleSphere(position,segmentHalfHeight,radius,{body->description.shape.center,body->description.radius})
                    :CollideCapsuleBox(position,segmentHalfHeight,radius,body->description.shape);
                if(!contact)continue;
                if(contact->normal.y>0.65f&&verticalVelocity<=0){grounded=true;verticalVelocity=0;}
                if(contact->normal.y< -0.65f&&verticalVelocity>0)verticalVelocity=0;
                if(contact->depth>0)
                {
                    position+=contact->normal*(contact->depth+0.0001f);corrected=true;
                    const float into=-Dot(horizontalDirection*walkSpeed,contact->normal);
                    if(body->description.mass>0&&into>0&&pushBudget>0)
                    {
                        const Vec3 pushDirection=-contact->normal;
                        const Vec3 pointVelocity=body->linearVelocity+Cross(body->angularVelocity,
                            contact->point-body->description.shape.center);
                        // 只补足沿接触法线的目标速度，且整次移动共享有限冲量预算。
                        const float needed=std::max(0.0f,into-Dot(pointVelocity,pushDirection))*body->description.mass;
                        const float impulse=std::min(pushBudget,needed);
                        if(impulse>0){world.ApplyImpulse(handle,pushDirection*impulse,contact->point);pushBudget-=impulse;}
                    }
                }
            }
            if(!corrected)break;
        }
    }
    return IsFinite(position);
}

std::vector<DebugLine> CharacterController::DebugLines() const
{
    std::vector<DebugLine> lines;
    if(!IsFinite(position)||!IsFinite(radius)||radius<=0||!IsFinite(segmentHalfHeight)||segmentHalfHeight<0)return lines;
    constexpr unsigned segments=16;const Vec3 color{1,0.8f,0.1f};
    for(unsigned i=0;i<segments;++i)
    {
        const float a=2*Pi*static_cast<float>(i)/segments,b=2*Pi*static_cast<float>(i+1)/segments;
        for(float sign:{-1.0f,1.0f})
        {
            const Vec3 center=position+Vec3{0,sign*segmentHalfHeight,0};
            lines.push_back({center+Vec3{radius*std::cos(a),0,radius*std::sin(a)},center+Vec3{radius*std::cos(b),0,radius*std::sin(b)},color});
            const float arcA=Pi*static_cast<float>(i)/segments,arcB=Pi*static_cast<float>(i+1)/segments;
            for(unsigned axis=0;axis<2;++axis)
            {
                const Vec3 p=axis==0?Vec3{radius*std::cos(arcA),sign*radius*std::sin(arcA),0}:Vec3{0,sign*radius*std::sin(arcA),radius*std::cos(arcA)};
                const Vec3 q=axis==0?Vec3{radius*std::cos(arcB),sign*radius*std::sin(arcB),0}:Vec3{0,sign*radius*std::sin(arcB),radius*std::cos(arcB)};
                lines.push_back({center+p,center+q,color});
            }
        }
    }
    for(const Vec3 axis:{Vec3{1,0,0},Vec3{-1,0,0},Vec3{0,0,1},Vec3{0,0,-1}})
        lines.push_back({position+axis*radius-Vec3{0,segmentHalfHeight,0},position+axis*radius+Vec3{0,segmentHalfHeight,0},color});
    return lines;
}
}
