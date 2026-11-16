#pragma once
#include "HdrImage.h"

namespace se
{
// 与LTC的rough/Nv坐标一致，x=Charlie方向反照率（归一后），y=该rough列的保守归一因子。
struct SheenEnergyLut {std::uint32_t size=64,samples=4096;std::vector<Vec2> values;};
bool BakeSheenEnergy(std::uint32_t size,std::uint32_t samples,SheenEnergyLut& output);
bool SaveSheenEnergy(const std::filesystem::path& path,const SheenEnergyLut& value);
bool LoadSheenEnergy(const std::filesystem::path& path,SheenEnergyLut& output);
}
