#include "TextureCompression.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

namespace se
{
namespace
{
// 格式依据Microsoft BC7 Format/Mode Reference；权重、fix-up和端点排列按规范。
// 无第三方编码器代码。质量策略为PCA初值+确定性最小二乘，明确只搜索mode5/6。
using Color=std::array<int,4>;
using Block=std::array<Color,16>;
constexpr int Weights4[]{0,4,9,13,17,21,26,30,34,38,43,47,51,55,60,64};
constexpr int Weights2[]{0,21,43,64};
struct Bits
{
    std::array<std::uint8_t,16> bytes{};unsigned cursor=0;
    void Put(unsigned value,unsigned count){for(unsigned b=0;b<count;++b,++cursor)bytes[cursor/8]|=std::uint8_t(((value>>b)&1u)<<(cursor%8));}
};
struct ReadBits
{
    const std::uint8_t* bytes;unsigned cursor=0;
    unsigned Get(unsigned count){unsigned v=0;for(unsigned b=0;b<count;++b,++cursor)v|=((bytes[cursor/8]>>(cursor%8))&1u)<<b;return v;}
};
Color Interpolate(const Color& a,const Color& b,int weight)
{Color value{};for(unsigned c=0;c<4;++c)value[c]=((64-weight)*a[c]+weight*b[c]+32)>>6;return value;}
int Error(const Color& a,const Color& b,unsigned channels)
{int error=0;for(unsigned c=0;c<channels;++c){const auto d=a[c]-b[c];error+=d*d;}return error;}
void Initial(const Block& block,unsigned channels,std::array<double,4>& a,std::array<double,4>& b)
{
    std::array<double,4> mean{},axis{};double covariance[4][4]{};int maximumRange=-1;
    for(unsigned c=0;c<channels;++c)
    {
        int minimum=255,maximum=0;for(const auto& pixel:block){mean[c]+=pixel[c]/16.;minimum=std::min(minimum,pixel[c]);maximum=std::max(maximum,pixel[c]);}
        if(maximum-minimum>maximumRange){maximumRange=maximum-minimum;axis={};axis[c]=1;}
    }
    for(const auto& pixel:block)for(unsigned c=0;c<channels;++c)for(unsigned d=0;d<channels;++d)
        covariance[c][d]+=(pixel[c]-mean[c])*(pixel[d]-mean[d]);
    for(unsigned iteration=0;iteration<10;++iteration)
    {
        std::array<double,4> next{};double norm=0;
        for(unsigned c=0;c<channels;++c){for(unsigned d=0;d<channels;++d)next[c]+=covariance[c][d]*axis[d];norm+=next[c]*next[c];}
        if(norm<1e-15)break;norm=std::sqrt(norm);for(unsigned c=0;c<channels;++c)axis[c]=next[c]/norm;
    }
    double low=std::numeric_limits<double>::max(),high=-low;
    for(const auto& pixel:block){double t=0;for(unsigned c=0;c<channels;++c)t+=(pixel[c]-mean[c])*axis[c];low=std::min(low,t);high=std::max(high,t);}
    for(unsigned c=0;c<channels;++c){a[c]=std::clamp(mean[c]+low*axis[c],0.,255.);b[c]=std::clamp(mean[c]+high*axis[c],0.,255.);}
}
Color Quantize(const std::array<double,4>& source,unsigned channels,int parity,bool mode6)
{
    Color result{};
    for(unsigned c=0;c<channels;++c)
    {
        if(mode6){const auto q=std::clamp(int(std::lround((source[c]-parity)*.5)),0,127);result[c]=q*2+parity;}
        else {const auto q=std::clamp(int(std::lround(source[c]*127./255)),0,127);result[c]=(q<<1)|(q>>6);}
    }
    return result;
}
int Assign(const Block& block,const Color& a,const Color& b,unsigned channels,bool mode6,std::array<unsigned,16>& indices)
{
    const auto count=mode6?16u:4u;const auto* weights=mode6?Weights4:Weights2;int total=0;
    for(unsigned p=0;p<16;++p)
    {
        int best=std::numeric_limits<int>::max();
        for(unsigned i=0;i<count;++i){const auto error=Error(block[p],Interpolate(a,b,weights[i]),channels);if(error<best){best=error;indices[p]=i;}}
        total+=best;
    }
    return total;
}
void Refine(const Block& block,const std::array<unsigned,16>& indices,unsigned channels,bool mode6,
    std::array<double,4>& a,std::array<double,4>& b)
{
    double aa=0,ab=0,bb=0;std::array<double,4> ay{},by{};const auto* weights=mode6?Weights4:Weights2;
    for(unsigned p=0;p<16;++p)
    {
        const double v=weights[indices[p]]/64.,u=1-v;aa+=u*u;ab+=u*v;bb+=v*v;
        for(unsigned c=0;c<channels;++c){ay[c]+=u*block[p][c];by[c]+=v*block[p][c];}
    }
    const double determinant=aa*bb-ab*ab;if(determinant<1e-12)return;
    for(unsigned c=0;c<channels;++c){a[c]=std::clamp((ay[c]*bb-by[c]*ab)/determinant,0.,255.);b[c]=std::clamp((by[c]*aa-ay[c]*ab)/determinant,0.,255.);}
}
struct Fit {Color a{},b{};std::array<unsigned,16> indices{};int error=std::numeric_limits<int>::max();};
Fit FitColors(const Block& block,bool mode6)
{
    const unsigned channels=mode6?4u:3u;std::array<double,4> initialA{},initialB{};Initial(block,channels,initialA,initialB);Fit best;
    for(int parityA=0;parityA<(mode6?2:1);++parityA)for(int parityB=0;parityB<(mode6?2:1);++parityB)
    {
        // 完全不透明/全透明是材质语义边界，不用1LSB alpha误差交换RGB的微小收益。
        const bool opaque=std::all_of(block.begin(),block.end(),[](const auto& p){return p[3]==255;});
        const bool transparent=std::all_of(block.begin(),block.end(),[](const auto& p){return p[3]==0;});
        if(mode6&&((opaque&&(parityA!=1||parityB!=1))||(transparent&&(parityA!=0||parityB!=0))))continue;
        auto a=initialA,b=initialB;
        for(unsigned iteration=0;iteration<5;++iteration)
        {
            Fit candidate;candidate.a=Quantize(a,channels,parityA,mode6);candidate.b=Quantize(b,channels,parityB,mode6);
            candidate.error=Assign(block,candidate.a,candidate.b,channels,mode6,candidate.indices);
            if(candidate.error<best.error)best=candidate;
            Refine(block,candidate.indices,channels,mode6,a,b);
        }
    }
    return best;
}
std::array<std::uint8_t,16> Encode(const Block& pixels)
{
    auto rgba=FitColors(pixels,true),rgb=FitColors(pixels,false);int alpha0=255,alpha1=0;
    for(const auto& p:pixels){alpha0=std::min(alpha0,p[3]);alpha1=std::max(alpha1,p[3]);}
    std::array<unsigned,16> alphaIndices{};int alphaError=0;
    for(unsigned p=0;p<16;++p)
    {
        int best=std::numeric_limits<int>::max();for(unsigned i=0;i<4;++i)
        {const int value=((64-Weights2[i])*alpha0+Weights2[i]*alpha1+32)>>6,d=pixels[p][3]-value;
         if(d*d<best){best=d*d;alphaIndices[p]=i;}}
        alphaError+=best;
    }
    Bits bits;
    if(rgba.error<=rgb.error+alphaError)
    {
        if(rgba.indices[0]&8u){std::swap(rgba.a,rgba.b);for(auto& index:rgba.indices)index=15-index;}
        bits.Put(1u<<6,7);
        for(unsigned c=0;c<4;++c){bits.Put(unsigned(rgba.a[c])>>1,7);bits.Put(unsigned(rgba.b[c])>>1,7);}
        bits.Put(unsigned(rgba.a[0])&1u,1);bits.Put(unsigned(rgba.b[0])&1u,1);
        for(unsigned p=0;p<16;++p)bits.Put(rgba.indices[p],p?4u:3u);
    }
    else
    {
        if(rgb.indices[0]&2u){std::swap(rgb.a,rgb.b);for(auto& index:rgb.indices)index=3-index;}
        if(alphaIndices[0]&2u){std::swap(alpha0,alpha1);for(auto& index:alphaIndices)index=3-index;}
        bits.Put(1u<<5,6);bits.Put(0,2);
        for(unsigned c=0;c<3;++c){bits.Put(unsigned(rgb.a[c])>>1,7);bits.Put(unsigned(rgb.b[c])>>1,7);}
        bits.Put(unsigned(alpha0),8);bits.Put(unsigned(alpha1),8);
        for(unsigned p=0;p<16;++p)bits.Put(rgb.indices[p],p?2u:1u);
        for(unsigned p=0;p<16;++p)bits.Put(alphaIndices[p],p?2u:1u);
    }
    return bits.bytes;
}
bool Dimensions(std::uint32_t width,std::uint32_t height)
{return width&&height&&width<=8192&&height<=8192;}
}
bool CompressBc7(std::span<const std::uint8_t> rgba8,std::uint32_t width,std::uint32_t height,Bc7Image& output,std::string& error)
{
    if(!Dimensions(width,height)||rgba8.size()!=std::size_t(width)*height*4){error="Rendering.TextureEncodeFailed";return false;}
    Bc7Image candidate{width,height,{}};const auto nx=(width+3)/4,ny=(height+3)/4;candidate.blocks.resize(std::size_t(nx)*ny*16);
    for(std::uint32_t by=0;by<ny;++by)for(std::uint32_t bx=0;bx<nx;++bx)
    {
        Block pixels{};for(unsigned y=0;y<4;++y)for(unsigned x=0;x<4;++x)
        {const auto offset=(std::size_t(std::min(by*4+y,height-1))*width+std::min(bx*4+x,width-1))*4;
         for(unsigned c=0;c<4;++c)pixels[y*4+x][c]=rgba8[offset+c];}
        const auto block=Encode(pixels);std::copy(block.begin(),block.end(),candidate.blocks.begin()+(std::size_t(by)*nx+bx)*16);
    }
    output=std::move(candidate);error.clear();return true;
}
bool DecodeBc7Mode56(const Bc7Image& image,std::vector<std::uint8_t>& rgba8,std::string& error)
{
    const auto nx=(image.width+3)/4,ny=(image.height+3)/4;
    if(!Dimensions(image.width,image.height)||image.blocks.size()!=std::size_t(nx)*ny*16){error="Rendering.TextureDecodeFailed";return false;}
    std::vector<std::uint8_t> candidate(std::size_t(image.width)*image.height*4);
    for(unsigned by=0;by<ny;++by)for(unsigned bx=0;bx<nx;++bx)
    {
        ReadBits bits{image.blocks.data()+(std::size_t(by)*nx+bx)*16};unsigned mode=0;while(mode<8&&!bits.Get(1))++mode;
        if(mode!=5&&mode!=6){error="Rendering.TextureDecodeFailed";return false;}
        unsigned rotation=mode==5?bits.Get(2):0;Color a{},b{};
        for(unsigned c=0;c<(mode==6?4u:3u);++c){a[c]=int(bits.Get(7));b[c]=int(bits.Get(7));}
        if(mode==6){const auto pa=bits.Get(1),pb=bits.Get(1);for(unsigned c=0;c<4;++c){a[c]=a[c]*2+int(pa);b[c]=b[c]*2+int(pb);}}
        else {for(unsigned c=0;c<3;++c){a[c]=(a[c]<<1)|(a[c]>>6);b[c]=(b[c]<<1)|(b[c]>>6);}a[3]=int(bits.Get(8));b[3]=int(bits.Get(8));}
        std::array<unsigned,16> colors{},alphas{};
        for(unsigned p=0;p<16;++p)colors[p]=bits.Get(p?(mode==6?4u:2u):(mode==6?3u:1u));
        if(mode==5)for(unsigned p=0;p<16;++p)alphas[p]=bits.Get(p?2u:1u);
        for(unsigned p=0;p<16;++p)
        {
            auto color=Interpolate(a,b,mode==6?Weights4[colors[p]]:Weights2[colors[p]]);
            if(mode==5)color[3]=((64-Weights2[alphas[p]])*a[3]+Weights2[alphas[p]]*b[3]+32)>>6;
            if(rotation)std::swap(color[3],color[rotation-1]);
            const auto x=bx*4+p%4,y=by*4+p/4;if(x>=image.width||y>=image.height)continue;
            for(unsigned c=0;c<4;++c)candidate[(std::size_t(y)*image.width+x)*4+c]=std::uint8_t(color[c]);
        }
    }
    rgba8=std::move(candidate);error.clear();return true;
}
}
