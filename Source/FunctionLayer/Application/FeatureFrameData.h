#pragma once
#include "FrameData.h"
namespace se
{
class SceneParticles;
void ApplyFeatureFrameData(const World&,FrameData&,RenderDefaults defaults=RenderDefaults::Project);
void ApplyTimeOfDayFrameData(FrameData&);
void PopulateParticleFrame(const SceneParticles& particles,FrameData& output);
}
