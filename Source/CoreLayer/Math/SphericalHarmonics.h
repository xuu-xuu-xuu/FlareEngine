#pragma once
#include "Math.h"
#include <array>

namespace flare
{
// 正交归一实 SH，l<=2，世界 xyz：Y00,Y1-1(y),Y10(z),Y11(x),
// Y2-2(xy),Y2-1(yz),Y20(3z²-1),Y21(xz),Y22(x²-y²)。场景仍为右手 Y-up。
struct Sh9Rgb { std::array<Vec3,9> coefficients{}; };
// 与上述九个多项式对应的归一化常数，求值和离线解析积分只共用这一份定义。
inline constexpr std::array<double,9> RealSh9Scale{.28209479177387814,.4886025119029199,.4886025119029199,.4886025119029199,
    1.0925484305920792,1.0925484305920792,.31539156525252005,1.0925484305920792,.5462742152960396};
bool IsFinite(const Sh9Rgb& value) noexcept;
// 输入方向归一化；非法方向/系数失败保留 output。数学求值不裁掉负瓣。
bool RealSh9Basis(Vec3 direction,std::array<float,9>& output) noexcept;
bool EvaluateSh9(const Sh9Rgb& coefficients,Vec3 direction,Vec3& output) noexcept;
// Radiance -> E/pi，三个 band 的卷积因子为 1、2/3、1/4。
// 单位白 radiance 最终求值为 1；不再在着色时除 pi。
bool ConvolveDiffuseSh9(const Sh9Rgb& radiance,Sh9Rgb& output) noexcept;
}
