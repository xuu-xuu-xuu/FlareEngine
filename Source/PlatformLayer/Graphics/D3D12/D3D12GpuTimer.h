#pragma once
#include <cstdint>
inline constexpr std::uint32_t D3D12FrameTimestampCount=34; // normal23/24，AO25..28，heterogeneous29/30，HZB31..33。
struct D3D12Presentation;
class Localization;
bool SetD3D12GpuTiming(D3D12Presentation& presentation,bool enabled,const Localization& text);
bool ReadD3D12GpuTimings(D3D12Presentation& presentation,const Localization& text);
