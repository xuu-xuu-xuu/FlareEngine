#pragma once
// 根据已固定 selfshadow/ltc_code fitLTC.cpp 改编；原许可证及未修改原件在 ThirdParty/LtcReference。
// 引用：Heitz, Dupuy, Hill, Neubelt, ACM TOG 35(4), 2016。
// 衍生改动：移除绘图/输出，参数化生产域；GGX 分支采用 Heitz 2018 VNDF，
// view 坐标在 Nv 两端加密；保留上游 MIS 三次误差目标和 Nelder-Mead。
#include <LtcReference/fit/LTC.h>
#include <LtcReference/fit/brdf.h>
#include <LtcReference/fit/brdf_ggx.h>
#include <LtcReference/fit/nelder_mead.h>
namespace flare::ltc_reference
{
inline thread_local int sampleSide=32;
inline constexpr float minimumRoughness=.045f,minimumNv=.0001f;
// 原始 NDF 在 grazing 下产生约 6.6% 幅度偏差；VNDF 的 eval/pdf=G2/G1 有界。
// Heitz, Sampling the GGX Distribution of Visible Normals, JCGT 7(4), 2018.
class BrdfGGXVisible final : public Brdf
{
public:
    float eval(const vec3& V,const vec3& L,const float alpha,float& pdf) const override
    {
        const double nv=V.z;
        if(nv<=0){pdf=0;return 0;}
        const auto sum=V+L;
        if(glm::dot(sum,sum)<1e-20f){pdf=0;return 0;}
        const auto H=glm::normalize(sum);const double vh=glm::dot(V,H),nh=H.z,a2=double(alpha)*alpha;
        if(vh<=0||nh<=0){pdf=0;return 0;}
        const double denominator=nh*nh*(a2-1)+1,distribution=a2/(3.14159265358979323846*denominator*denominator);
        const auto rootV=std::sqrt(nv*nv+a2*(1-nv*nv));
        pdf=float(distribution/(2*(nv+rootV)));
        if(L.z<=0)return 0;
        const double nl=L.z,rootL=std::sqrt(nl*nl+a2*(1-nl*nl));
        return float(distribution*nl/(2*(nl*rootV+nv*rootL)));
    }
    vec3 sample(const vec3& V,const float alpha,const float U1,const float U2) const override
    {
        const auto vh=glm::normalize(vec3(alpha*V.x,alpha*V.y,V.z));
        const auto lensq=vh.x*vh.x+vh.y*vh.y;
        const auto t1=lensq>0?vec3(-vh.y,vh.x,0)/std::sqrt(lensq):vec3(1,0,0),t2=glm::cross(vh,t1);
        const auto radius=std::sqrt(U1),phi=6.2831853071795864769f*U2,x=radius*std::cos(phi),s=.5f*(1+vh.z);
        const auto y=(1-s)*std::sqrt(std::max(0.0f,1-x*x))+s*radius*std::sin(phi);
        const auto nh=t1*x+t2*y+vh*std::sqrt(std::max(0.0f,1-x*x-y*y));
        const auto h=glm::normalize(vec3(alpha*nh.x,alpha*nh.y,std::max(0.0f,nh.z)));
        return -V+2.0f*glm::dot(V,h)*h;
    }
};
void computeAvgTerms(const Brdf& brdf, const vec3& V, const float alpha,
    float& norm, float& fresnel, vec3& averageDir)
{
    norm = 0.0f;
    fresnel = 0.0f;
    averageDir = vec3(0, 0, 0);

    for (int j = 0; j < sampleSide; ++j)
    for (int i = 0; i < sampleSide; ++i)
    {
        const float U1 = (i + 0.5f)/sampleSide;
        const float U2 = (j + 0.5f)/sampleSide;

        // sample
        const vec3 L = brdf.sample(V, alpha, U1, U2);

        // eval
        float pdf;
        float eval = brdf.eval(V, L, alpha, pdf);

        if (pdf > 0)
        {
            float weight = eval / pdf;

            vec3 H = normalize(V+L);

            // accumulate
            norm       += weight;
            fresnel    += weight * pow(1.0f - glm::max(dot(V, H), 0.0f), 5.0f);
            averageDir += weight * L;
        }
    }

    norm    /= (float)(sampleSide*sampleSide);
    fresnel /= (float)(sampleSide*sampleSide);

    // clear y component, which should be zero with isotropic BRDFs
    averageDir.y = 0.0f;

    averageDir = normalize(averageDir);
}

// compute the error between the BRDF and the LTC
// using Multiple Importance Sampling
float computeError(const LTC& ltc, const Brdf& brdf, const vec3& V, const float alpha)
{
    double error = 0.0;

    for (int j = 0; j < sampleSide; ++j)
    for (int i = 0; i < sampleSide; ++i)
    {
        const float U1 = (i + 0.5f)/sampleSide;
        const float U2 = (j + 0.5f)/sampleSide;

        // importance sample LTC
        {
            // sample
            const vec3 L = ltc.sample(U1, U2);

            float pdf_brdf;
            float eval_brdf = brdf.eval(V, L, alpha, pdf_brdf);
            float eval_ltc = ltc.eval(L);
            float pdf_ltc = eval_ltc/ltc.magnitude;

            // error with MIS weight
            double error_ = fabsf(eval_brdf - eval_ltc);
            error_ = error_*error_*error_;
            error += error_/(pdf_ltc + pdf_brdf);
        }

        // importance sample BRDF
        {
            // sample
            const vec3 L = brdf.sample(V, alpha, U1, U2);

            float pdf_brdf;
            float eval_brdf = brdf.eval(V, L, alpha, pdf_brdf);
            float eval_ltc = ltc.eval(L);
            float pdf_ltc = eval_ltc/ltc.magnitude;

            // error with MIS weight
            double error_ = fabsf(eval_brdf - eval_ltc);
            error_ = error_*error_*error_;
            error += error_/(pdf_ltc + pdf_brdf);
        }
    }

    return (float)error / (float)(sampleSide*sampleSide);
}

struct FitLTC
{
    FitLTC(LTC& ltc_, const Brdf& brdf, bool isotropic_, const vec3& V_, float alpha_) :
        ltc(ltc_), brdf(brdf), V(V_), alpha(alpha_), isotropic(isotropic_)
    {
    }

    void update(const float* params)
    {
        float m11 = std::max<float>(params[0], 1e-7f);
        float m22 = std::max<float>(params[1], 1e-7f);
        float m13 = params[2];

        if (isotropic)
        {
            ltc.m11 = m11;
            ltc.m22 = m11;
            ltc.m13 = 0.0f;
        }
        else
        {
            ltc.m11 = m11;
            ltc.m22 = m22;
            ltc.m13 = m13;
        }
        ltc.update();
    }

    float operator()(const float* params)
    {
        update(params);
        return computeError(ltc, brdf, V, alpha);
    }

    const Brdf& brdf;
    LTC& ltc;
    bool isotropic;

    const vec3& V;
    float alpha;
};

// fit brute force
// refine first guess by exploring parameter space
void fit(LTC& ltc, const Brdf& brdf, const vec3& V, const float alpha, const float epsilon = 0.05f, const bool isotropic = false)
{
    float startFit[3] = { ltc.m11, ltc.m22, ltc.m13 };
    float resultFit[3];

    FitLTC fitter(ltc, brdf, isotropic, V, alpha);

    // Find best-fit LTC lobe (scale, alphax, alphay)
    [[maybe_unused]] float error = NelderMead<3>(resultFit, startFit, epsilon, 1e-5f, 100, fitter);

    // Update LTC with best fitting values
    fitter.update(resultFit);
}

// fit data
void fitTab(mat3* tab, vec2* tabMagFresnel, const int N, const Brdf& brdf)
{
    LTC ltc;

    // loop over theta and alpha
    for (int a = N - 1; a >=     0; --a)
    for (int t =     0; t <= N - 1; ++t)
    {
        // 两端加密：Nv=minNv+(1-minNv)*cos(pi*y/2)^2。
        float x = t/float(N - 1);
        float cosine = cosf(1.57079632679489661923f*x);
        float ct = std::clamp(minimumNv+(1.0f-minimumNv)*cosine*cosine,minimumNv,1.0f);
        const vec3 V = vec3(sqrtf(std::max(0.0f,1-ct*ct)), 0, ct);

        // alpha = roughness^2
        float roughness = minimumRoughness+(1.0f-minimumRoughness)*a/float(N - 1);
        float alpha = std::max<float>(roughness*roughness, minimumRoughness*minimumRoughness);




        vec3 averageDir;
        computeAvgTerms(brdf, V, alpha, ltc.magnitude, ltc.fresnel, averageDir);

        bool isotropic;

        // 1. first guess for the fit
        // init the hemisphere in which the distribution is fitted
        // if theta == 0 the lobe is rotationally symmetric and aligned with Z = (0 0 1)
        if (t == 0)
        {
            ltc.X = vec3(1, 0, 0);
            ltc.Y = vec3(0, 1, 0);
            ltc.Z = vec3(0, 0, 1);

            if (a == N - 1) // roughness = 1
            {
                ltc.m11 = 1.0f;
                ltc.m22 = 1.0f;
            }
            else // init with roughness of previous fit
            {
                ltc.m11 = tab[a + 1 + t*N][0][0];
                ltc.m22 = tab[a + 1 + t*N][1][1];
            }

            ltc.m13 = 0;
            ltc.update();

            isotropic = true;
        }
        // otherwise use previous configuration as first guess
        else
        {
            vec3 L = averageDir;
            vec3 T1(L.z, 0, -L.x);
            vec3 T2(0, 1, 0);
            ltc.X = T1;
            ltc.Y = T2;
            ltc.Z = L;

            ltc.update();

            isotropic = false;
        }

        // 2. fit (explore parameter space and refine first guess)
        float epsilon = 0.05f;
        fit(ltc, brdf, V, alpha, epsilon, isotropic);

        // copy data
        tab[a + t*N] = ltc.M;
        tabMagFresnel[a + t*N][0] = ltc.magnitude;
        tabMagFresnel[a + t*N][1] = ltc.fresnel;

        // kill useless coefs in matrix
        tab[a+t*N][0][1] = 0;
        tab[a+t*N][1][0] = 0;
        tab[a+t*N][2][1] = 0;
        tab[a+t*N][1][2] = 0;




    }
}


}
