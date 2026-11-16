#include "StaticModel.h"
#include "AssetIO.h"
#include "CoreLayer/Math/Math.h"
#include "CoreLayer/Math/TransformDecomposition.h"
#include <ufbx/ufbx.h>
#include <cgltf/cgltf.h>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <unordered_map>

namespace se
{
namespace
{
using asset_io::Diagnostic;
using F3 = std::array<float, 3>;
constexpr std::size_t MaxVertices = 4'000'000;
constexpr std::size_t MaxNodes = 100'000;
constexpr std::size_t MaxJoints = 65'536; // JOINTS_0 用 uint16 保存，最多 65536 根关节。
constexpr std::size_t MaxAnimationKeys = 1 << 20;
F3 Sub(F3 a, F3 b) { return { a[0] - b[0], a[1] - b[1], a[2] - b[2] }; }
F3 Add(F3 a, F3 b) { return { a[0] + b[0], a[1] + b[1], a[2] + b[2] }; }
F3 Mul(F3 a, float x) { return { a[0] * x, a[1] * x, a[2] * x }; }
float Dot(F3 a, F3 b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
F3 Cross(F3 a, F3 b) { return { a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0] }; }
F3 Normalized(F3 v, F3 fallback = { 0, 1, 0 })
{
    const float length = std::sqrt(Dot(v, v));
    return length > 1e-12f && std::isfinite(length) ? Mul(v, 1.0f / length) : fallback;
}

void GenerateBasis(MeshData& mesh, bool normals, bool tangents)
{
    std::vector<F3> normalSums(mesh.vertices.size()), tangentSums(mesh.vertices.size()), bitangentSums(mesh.vertices.size());
    for (std::size_t triangle = 0; triangle + 2 < mesh.indices.size(); triangle += 3)
    {
        const auto a = mesh.indices[triangle], b = mesh.indices[triangle + 1], c = mesh.indices[triangle + 2];
        const auto& va = mesh.vertices[a]; const auto& vb = mesh.vertices[b]; const auto& vc = mesh.vertices[c];
        const auto e1 = Sub(vb.position, va.position), e2 = Sub(vc.position, va.position);
        const auto n = Cross(e1, e2);
        for (const auto index : { a, b, c }) normalSums[index] = Add(normalSums[index], n);
        const float du1 = vb.uv[0] - va.uv[0], dv1 = vb.uv[1] - va.uv[1];
        const float du2 = vc.uv[0] - va.uv[0], dv2 = vc.uv[1] - va.uv[1];
        const float determinant = du1 * dv2 - dv1 * du2;
        if (std::abs(determinant) > 1e-12f)
        {
            const auto t = Mul(Sub(Mul(e1, dv2), Mul(e2, dv1)), 1.0f / determinant);
            const auto bt = Mul(Sub(Mul(e2, du1), Mul(e1, du2)), 1.0f / determinant);
            for (const auto index : { a, b, c })
            { tangentSums[index] = Add(tangentSums[index], t); bitangentSums[index] = Add(bitangentSums[index], bt); }
        }
    }
    for (std::size_t index = 0; index < mesh.vertices.size(); ++index)
    {
        auto& vertex = mesh.vertices[index];
        vertex.normal = Normalized(normals ? normalSums[index] : vertex.normal);
        if (tangents)
        {
            const auto n = vertex.normal;
            const auto fallback = Normalized(Cross(std::abs(n[1]) < 0.9f ? F3{ 0, 1, 0 } : F3{ 1, 0, 0 }, n), { 1, 0, 0 });
            const auto tangent = Normalized(Sub(tangentSums[index], Mul(n, Dot(n, tangentSums[index]))), fallback);
            vertex.tangent = { tangent[0], tangent[1], tangent[2], Dot(Cross(n, tangent), bitangentSums[index]) < 0 ? -1.0f : 1.0f };
        }
    }
}

bool ValidateModel(const StaticModel& model)
{
    if (model.meshes.empty() || model.nodes.empty() || model.nodes.size() > MaxNodes) return false;
    std::vector<unsigned char> marks(model.nodes.size());
    for (std::size_t index = 0; index < model.nodes.size(); ++index)
    {
        std::int32_t cursor = static_cast<std::int32_t>(index);
        while (cursor >= 0 && marks[static_cast<std::size_t>(cursor)] == 0)
        {
            marks[static_cast<std::size_t>(cursor)] = 1;
            const auto next = model.nodes[static_cast<std::size_t>(cursor)].parent;
            if (next < -1 || next >= static_cast<std::int32_t>(model.nodes.size())) return false;
            cursor = next;
        }
        if (cursor >= 0 && marks[static_cast<std::size_t>(cursor)] == 1) return false;
        cursor = static_cast<std::int32_t>(index);
        while (cursor >= 0 && marks[static_cast<std::size_t>(cursor)] == 1)
        { marks[static_cast<std::size_t>(cursor)] = 2; cursor = model.nodes[static_cast<std::size_t>(cursor)].parent; }
        for (const float v : model.nodes[index].localMatrix) if (!std::isfinite(v)) return false;
        for (const auto mesh : model.nodes[index].meshes) if (mesh >= model.meshes.size()) return false;
    }
    for (const auto& mesh : model.meshes)
    {
        if (mesh.vertices.empty() || mesh.vertices.size() > MaxVertices || mesh.indices.empty() || mesh.indices.size() % 3 != 0) return false;
        if (mesh.material < -1 || mesh.material >= static_cast<std::int32_t>(model.materials.size())) return false;
        if (mesh.meshNode >= model.nodes.size()) return false;
        if (mesh.skinning.size() != (mesh.skinned ? mesh.vertices.size() : 0)) return false;
        if (mesh.skinned && (model.joints.empty() || model.joints.size() > MaxJoints)) return false;
        for (const auto index : mesh.indices) if (index >= mesh.vertices.size()) return false;
        for (const auto& v : mesh.vertices)
        {
            for (const auto f : v.position) if (!std::isfinite(f)) return false;
            for (const auto f : v.normal) if (!std::isfinite(f)) return false;
            for (const auto f : v.uv) if (!std::isfinite(f)) return false;
            for (const auto f : v.tangent) if (!std::isfinite(f)) return false;
        }
        for (const auto& skin : mesh.skinning)
        {
            float sum = 0.0f;
            for (std::size_t influence = 0; influence < 4; ++influence)
            {
                if (!std::isfinite(skin.weights[influence]) || skin.weights[influence] < -1e-6f) return false;
                if (skin.joints[influence] >= model.joints.size()) return false;
                sum += skin.weights[influence];
            }
            // 导入器把权重和归一到 1（容差 1e-4）；这里放宽一点吸收缓存读写误差。
            if (std::abs(sum - 1.0f) > 2e-4f) return false;
        }
    }
    if (model.joints.size() > MaxJoints) return false;
    for (std::size_t index = 0; index < model.joints.size(); ++index)
    {
        const auto& joint = model.joints[index];
        if (joint.parent < -1 || joint.parent >= static_cast<std::int32_t>(index)) return false;
        for (const auto value : joint.inverseBind) if (!std::isfinite(value)) return false;
        for (const auto value : joint.restTranslation) if (!std::isfinite(value)) return false;
        for (const auto value : joint.restRotation) if (!std::isfinite(value)) return false;
        for (const auto value : joint.restScale) if (!std::isfinite(value)) return false;
        const float rotationLength = joint.restRotation[0] * joint.restRotation[0] + joint.restRotation[1] * joint.restRotation[1]
            + joint.restRotation[2] * joint.restRotation[2] + joint.restRotation[3] * joint.restRotation[3];
        if (!(rotationLength > 1e-12f)) return false;
    }
    for (const auto& clip : model.animations)
    {
        if (!std::isfinite(clip.duration) || clip.duration < 0.0f) return false;
        for (const auto& channel : clip.channels)
        {
            if (channel.joint >= model.joints.size()) return false;
            if (channel.times.empty() || channel.times.size() != channel.values.size() || channel.times.size() > MaxAnimationKeys) return false;
            if (static_cast<std::uint8_t>(channel.path) > static_cast<std::uint8_t>(ModelAnimationPath::Scale)) return false;
            if (static_cast<std::uint8_t>(channel.interpolation) > static_cast<std::uint8_t>(ModelAnimationInterpolation::Step)) return false;
            for (std::size_t key = 0; key < channel.times.size(); ++key)
            {
                if (!std::isfinite(channel.times[key])) return false;
                if (key > 0 && !(channel.times[key] > channel.times[key - 1])) return false;
                for (const auto value : channel.values[key]) if (!std::isfinite(value)) return false;
            }
        }
    }
    for (const auto& material : model.materials)
    {
        for (const auto value : material.baseColor) if (!std::isfinite(value)) return false;
        if (!std::isfinite(material.metallic) || !std::isfinite(material.roughness) || !std::isfinite(material.normalScale)) return false;
        if (material.baseColor[3] < 0.0f || material.baseColor[3] > 1.0f
            || !IsValidMaterialAlphaMode(material.alphaMode) || !std::isfinite(material.alphaCutoff)
            || material.alphaCutoff < 0.0f) return false;
        if (!std::isfinite(material.clearCoat.weight) || material.clearCoat.weight < 0 || material.clearCoat.weight > 1
            || !std::isfinite(material.clearCoat.roughness) || material.clearCoat.roughness < 0 || material.clearCoat.roughness > 1
            || !std::isfinite(material.clearCoat.normalScale)) return false;
        if(!IsValidMaterialLayer(material.sheen)||!IsValidMaterialLayer(material.anisotropy)||!IsValidMaterialLayer(material.subsurface))return false;
        for (const auto texture : { material.baseColorTexture, material.normalTexture, material.metallicRoughnessTexture,
            material.clearCoatWeightTexture, material.clearCoatRoughnessTexture, material.clearCoatNormalTexture,
            material.sheenColorTexture,material.sheenRoughnessTexture,material.anisotropyTexture })
            if (texture < -1 || texture >= static_cast<std::int32_t>(model.textures.size())) return false;
    }
    return true;
}

bool AddExternalTexture(StaticModel& output, TextureReference& texture, const std::string& uri,
    const std::filesystem::path& source, const ImportOptions& options, std::vector<AssetDiagnostic>& diagnostics, bool encodedUri = true)
{
    std::filesystem::path resolved;
    if (!(encodedUri ? asset_io::ResolveReference(source, options.allowedRoot, uri, resolved, texture.relativePath)
        : asset_io::ResolveRelative(source, options.allowedRoot, uri, resolved, texture.relativePath)))
    { Diagnostic(diagnostics, "Assets.ReferenceOutsideRoot", uri); return false; }
    texture.missing = !std::filesystem::is_regular_file(resolved);
    output.dependencies.push_back(texture.relativePath);
    if (texture.missing) Diagnostic(diagnostics, "Assets.MissingTexture", texture.relativePath, true);
    return true;
}

struct GltfFileContext { std::filesystem::path root; };
cgltf_result ReadGltfFile(const cgltf_memory_options*, const cgltf_file_options* files,
    const char* path, cgltf_size* size, void** data)
{
    try
    {
        const auto native = asset_io::FromUtf8(path);
        const auto& context = *static_cast<GltfFileContext*>(files->user_data);
        std::vector<std::uint8_t> bytes;
        if (!asset_io::Within(context.root, native) || !asset_io::Read(native, bytes)) return cgltf_result_io_error;
        if (*size != 0 && *size > bytes.size()) return cgltf_result_data_too_short;
        const auto length = *size == 0 ? bytes.size() : *size;
        *data = std::malloc((std::max)(length, std::size_t{ 1 }));
        if (!*data) return cgltf_result_out_of_memory;
        std::memcpy(*data, bytes.data(), length);
        *size = length;
        return cgltf_result_success;
    }
    catch (...) { return cgltf_result_io_error; }
}
void ReleaseGltfFile(const cgltf_memory_options*, const cgltf_file_options*, void* data) { std::free(data); }

// cgltf 对未知 alphaMode 会保留 Opaque 默认值；必须检查原 JSON，不能把坏遮罩静默变成实心。
// data.json 同时指向 glTF 文本或 GLB JSON chunk，避免两个入口使用不同的校验规则。
bool ValidateGltfAlpha(const cgltf_data& data, std::vector<AssetDiagnostic>& diagnostics)
{
    const auto document = nlohmann::json::parse(data.json, data.json + data.json_size, nullptr, false);
    if (!document.is_object()) return false;
    const auto materials = document.find("materials");
    if (materials == document.end()) return true;
    if (!materials->is_array()) return false;
    for (std::size_t index = 0; index < materials->size(); ++index)
    {
        const auto& material = (*materials)[index];
        const auto reject = [&](const char* field) {
            Diagnostic(diagnostics, "Assets.InvalidMaterial", "materials[" + std::to_string(index) + "]." + field);
            return false;
        };
        if (!material.is_object()) return reject("object");
        if (const auto mode = material.find("alphaMode"); mode != material.end())
        {
            MaterialAlphaMode parsed;
            if (!mode->is_string() || !ParseMaterialAlphaMode(mode->get_ref<const std::string&>(), parsed))
                return reject("alphaMode");
        }
        if (const auto cutoff = material.find("alphaCutoff"); cutoff != material.end())
        {
            if (!cutoff->is_number()) return reject("alphaCutoff");
            const auto value = cutoff->get<double>();
            if (!std::isfinite(value) || value < 0.0) return reject("alphaCutoff");
            if (value > std::numeric_limits<float>::max())
            { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "alphaCutoff exceeds finite float"); return false; }
        }
        const auto pbr = material.find("pbrMetallicRoughness");
        if (pbr == material.end()) continue;
        if (!pbr->is_object()) return reject("pbrMetallicRoughness");
        const auto factor = pbr->find("baseColorFactor");
        if (factor == pbr->end()) continue;
        if (!factor->is_array() || factor->size() != 4) return reject("baseColorFactor");
        for (std::size_t component = 0; component < 4; ++component)
        {
            if (!(*factor)[component].is_number()) return reject("baseColorFactor");
            const auto value = (*factor)[component].get<double>();
            if (!std::isfinite(value) || (component == 3 && (value < 0.0 || value > 1.0)))
                return reject("baseColorFactor");
        }
    }
    return true;
}

// cgltf 的数值转换不替代 glTF schema 校验。保留原 JSON 规则，glTF/GLB 同入口，坏输入不取默认蒙混过去。
bool ValidateGltfClearCoat(const cgltf_data& data, std::vector<AssetDiagnostic>& diagnostics)
{
    const auto document=nlohmann::json::parse(data.json,data.json+data.json_size,nullptr,false);
    if(!document.is_object())return false;
    const auto materials=document.find("materials");if(materials==document.end())return true;
    if(!materials->is_array())return false;
    for(std::size_t index=0;index<materials->size();++index)
    {
        const auto& material=(*materials)[index];
        const auto reject=[&](const std::string& field){Diagnostic(diagnostics,"Assets.InvalidMaterial","materials["+std::to_string(index)+"].extensions.KHR_materials_clearcoat."+field);return false;};
        if(!material.is_object())return reject("material");
        const auto extensions=material.find("extensions");if(extensions==material.end())continue;
        if(!extensions->is_object())return reject("extensions");
        const auto coat=extensions->find("KHR_materials_clearcoat");if(coat==extensions->end())continue;
        if(!coat->is_object())return reject("object");
        // 该扩展在规范中明确不能与这两种材质同时使用。
        if(extensions->contains("KHR_materials_unlit")||extensions->contains("KHR_materials_pbrSpecularGlossiness"))return reject("incompatible extension");
        for(const char* key:{"clearcoatFactor","clearcoatRoughnessFactor"})if(const auto value=coat->find(key);value!=coat->end())
        {
            if(!value->is_number())return reject(key);const auto number=value->get<double>();
            if(!std::isfinite(number)||number<0||number>1)return reject(key);
        }
        for(const char* key:{"clearcoatTexture","clearcoatRoughnessTexture","clearcoatNormalTexture"})if(const auto texture=coat->find(key);texture!=coat->end())
        {
            if(!texture->is_object()||!texture->contains("index")||!texture->at("index").is_number_unsigned()
                ||texture->at("index").get<std::uint64_t>()>=data.textures_count)return reject(key);
            if(const auto uv=texture->find("texCoord");uv!=texture->end())
                if(!uv->is_number_unsigned()||uv->get<std::uint64_t>()>std::numeric_limits<int>::max())return reject(std::string(key)+".texCoord");
            if(std::string_view(key)=="clearcoatNormalTexture")if(const auto scale=texture->find("scale");scale!=texture->end())
            {
                if(!scale->is_number())return reject("clearcoatNormalTexture.scale");const auto number=scale->get<double>();
                if(!std::isfinite(number))return reject("clearcoatNormalTexture.scale");
                if(std::abs(number)>std::numeric_limits<float>::max())
                {Diagnostic(diagnostics,"Assets.UnsupportedFeature","KHR_materials_clearcoat normal scale exceeds finite float");return false;}
            }
        }
    }
    return true;
}

bool ValidateGltfLayers(const cgltf_data& data,std::vector<SubsurfaceParameters>& subsurface,std::vector<AssetDiagnostic>& diagnostics)
{
    const auto json=nlohmann::json::parse(data.json,data.json+data.json_size,nullptr,false);
    if(!json.is_object())return false;
    subsurface.resize(data.materials_count);
    const auto materials=json.find("materials");if(materials==json.end())return data.materials_count==0;
    if(!materials->is_array()||materials->size()!=data.materials_count)return false;
    for(std::size_t index=0;index<materials->size();++index)
    {
        const auto& material=(*materials)[index];
        const auto reject=[&](const std::string& field){Diagnostic(diagnostics,"Assets.InvalidMaterial","materials["+std::to_string(index)+"]."+field);return false;};
        const auto number=[&](const nlohmann::json& object,const char* key,double minimum,double maximum,float* target=nullptr){
            const auto value=object.find(key);if(value==object.end())return true;
            if(!value->is_number())return reject(key);const auto n=value->get<double>();
            if(!std::isfinite(n)||n<minimum||n>maximum)return reject(key);
            if(std::abs(n)>std::numeric_limits<float>::max()){Diagnostic(diagnostics,"Assets.UnsupportedFeature",std::string(key)+" exceeds finite float");return false;}
            if(target)*target=static_cast<float>(n);return true;};
        const auto color=[&](const nlohmann::json& object,const char* key,Vec3* target=nullptr){
            const auto value=object.find(key);if(value==object.end())return true;
            if(!value->is_array()||value->size()!=3)return reject(key);Vec3 parsed;float* channels[]{&parsed.x,&parsed.y,&parsed.z};
            for(unsigned c=0;c<3;++c){if(!(*value)[c].is_number())return reject(key);const auto n=(*value)[c].get<double>();if(!std::isfinite(n)||n<0||n>1)return reject(key);*channels[c]=static_cast<float>(n);}
            if(target)*target=parsed;return true;};
        const auto texture=[&](const nlohmann::json& object,const char* key){
            const auto value=object.find(key);if(value==object.end())return true;
            if(!value->is_object()||!value->contains("index")||!value->at("index").is_number_unsigned()
                ||value->at("index").get<std::uint64_t>()>=data.textures_count)return reject(key);
            if(const auto uv=value->find("texCoord");uv!=value->end())if(!uv->is_number_unsigned()||uv->get<std::uint64_t>()>std::numeric_limits<int>::max())return reject(std::string(key)+".texCoord");
            return true;};
        if(const auto extensions=material.find("extensions");extensions!=material.end())
        {
            if(!extensions->is_object())return reject("extensions");
            for(const char* key:{"KHR_materials_sheen","KHR_materials_anisotropy"})if(const auto layer=extensions->find(key);layer!=extensions->end())
            {
                if(!layer->is_object())return reject(key);
                if(extensions->contains("KHR_materials_unlit")||extensions->contains("KHR_materials_pbrSpecularGlossiness"))return reject(std::string(key)+" incompatible extension");
                if(std::string_view(key)=="KHR_materials_sheen")
                {if(!color(*layer,"sheenColorFactor")||!number(*layer,"sheenRoughnessFactor",0,1)||!texture(*layer,"sheenColorTexture")||!texture(*layer,"sheenRoughnessTexture"))return false;}
                else if(!number(*layer,"anisotropyStrength",0,1)||!number(*layer,"anisotropyRotation",-std::numeric_limits<double>::max(),std::numeric_limits<double>::max())||!texture(*layer,"anisotropyTexture"))return false;
            }
        }
        // 应用专属 extras，只在显式声明时启用；不能把 KHR volume/transmission 自动当作皮肤。
        if(const auto extras=material.find("extras");extras!=material.end()&&extras->is_object())if(const auto engine=extras->find("SelfEngine");engine!=extras->end())
        {
            if(!engine->is_object())return reject("extras.SelfEngine");
            if(const auto skin=engine->find("subsurface");skin!=engine->end())
            {
                if(!skin->is_object())return reject("extras.SelfEngine.subsurface");auto& p=subsurface[index];
                if(!number(*skin,"weight",0,1,&p.weight)||!number(*skin,"radius",0,1,&p.radius)||!color(*skin,"color",&p.color)||!IsValidMaterialLayer(p))return reject("extras.SelfEngine.subsurface");
            }
        }
    }
    return true;
}

// ---- glTF 蒙皮/动画：节点变换、骨骼拓扑与动画采样 ----

// 节点局部矩阵与 ModelNode::localMatrix 使用同一表示（列主序、列向量，world = parentWorld * local），
// 导入缩放只施加到平移列，与静态节点处理保持一致。
std::array<float, 16> NodeLocalMatrix(const cgltf_node& node, float scale)
{
    std::array<float, 16> local{};
    cgltf_node_transform_local(&node, local.data());
    for (const auto offset : { 12, 13, 14 }) local[offset] *= scale;
    return local;
}

// 统一缩放 s 把世界坐标按 S 缩放，TRS 世界矩阵随之变为 S * M * S^-1：只有平移列被乘 s。
Mat4 ScaleConjugated(const Mat4& matrix, float scale)
{
    Mat4 result = matrix;
    result(0, 3) *= scale;
    result(1, 3) *= scale;
    result(2, 3) *= scale;
    return result;
}

void ComputeNodeWorlds(const cgltf_data& data, float scale, std::vector<Mat4>& worlds)
{
    worlds.assign(data.nodes_count, Mat4{});
    std::vector<unsigned char> done(data.nodes_count, 0);
    std::vector<std::size_t> path;
    for (std::size_t index = 0; index < data.nodes_count; ++index)
    {
        path.clear();
        const cgltf_node* cursor = &data.nodes[index];
        // cgltf_validate 保证节点图无环；这里保留步数上限，损坏数据也不进入无限循环。
        for (std::size_t steps = 0; cursor && !done[static_cast<std::size_t>(cursor - data.nodes)]; ++steps)
        {
            if (steps > data.nodes_count) break;
            path.push_back(static_cast<std::size_t>(cursor - data.nodes));
            cursor = cursor->parent;
        }
        for (auto it = path.rbegin(); it != path.rend(); ++it)
        {
            const auto& node = data.nodes[*it];
            const Mat4 local{ NodeLocalMatrix(node, scale) };
            worlds[*it] = node.parent ? worlds[static_cast<std::size_t>(node.parent - data.nodes)] * local : local;
            done[*it] = 1;
        }
    }
}

// 输出序的关节：父关节总在子关节之前。source 给出该关节在 skin->joints 中的原始下标，
// 逆绑定矩阵按原始下标读取。
struct JointTopology
{
    std::vector<const cgltf_node*> nodes;
    std::vector<std::int32_t> parents;
    std::vector<std::uint16_t> source;
};

std::string JointLabel(const cgltf_node& node, std::size_t index)
{
    return node.name ? std::string(node.name) : ("#" + std::to_string(index));
}

bool BuildJointTopology(const cgltf_skin& skin, const cgltf_data& data, std::vector<AssetDiagnostic>& diagnostics,
    JointTopology& output, std::string& detail)
{
    const auto jointCount = skin.joints_count;
    if (jointCount == 0) { detail = "skin has no joints"; return false; }
    if (jointCount > MaxJoints)
    { detail = "joint count " + std::to_string(jointCount) + " > " + std::to_string(MaxJoints); return false; }
    std::unordered_map<const cgltf_node*, std::int32_t> sourceOfNode;
    sourceOfNode.reserve(jointCount);
    for (std::size_t index = 0; index < jointCount; ++index)
    {
        const auto* node = skin.joints[index];
        if (!node) { detail = "joint " + std::to_string(index) + " has no node"; return false; }
        if (!sourceOfNode.emplace(node, static_cast<std::int32_t>(index)).second)
        { detail = "joint " + JointLabel(*node, index) + " is listed twice"; return false; }
    }
    // skin.skeleton 只声明骨架根；实体局部空间始终取场景根，因此这里只做“所有关节都在骨架根下”的一致性提示。
    if (skin.skeleton)
    {
        for (std::size_t index = 0; index < jointCount; ++index)
        {
            const cgltf_node* cursor = skin.joints[index];
            std::size_t steps = 0;
            while (cursor && cursor != skin.skeleton && steps++ <= data.nodes_count) cursor = cursor->parent;
            if (cursor != skin.skeleton)
            {
                Diagnostic(diagnostics, "Assets.UnsupportedFeature",
                    "skin skeleton root does not contain joint " + JointLabel(*skin.joints[index], index), true);
                break;
            }
        }
    }
    // 父关节 = 最近的关节祖先；走到非关节节点时继续上溯，从而把骨架根之上的节点变换折叠进 rest TRS。
    std::vector<std::int32_t> parentOfSource(jointCount, -1);
    for (std::size_t index = 0; index < jointCount; ++index)
    {
        const cgltf_node* cursor = skin.joints[index]->parent;
        for (std::size_t steps = 0; cursor; ++steps)
        {
            if (steps > data.nodes_count)
            {
                detail = "joint " + JointLabel(*skin.joints[index], index) + " parent cycle";
                return false;
            }
            if (const auto found = sourceOfNode.find(cursor); found != sourceOfNode.end())
            { parentOfSource[index] = found->second; break; }
            cursor = cursor->parent;
        }
    }
    std::vector<std::vector<std::int32_t>> children(jointCount);
    std::vector<std::int32_t> roots;
    for (std::size_t index = 0; index < jointCount; ++index)
    {
        if (parentOfSource[index] < 0) roots.push_back(static_cast<std::int32_t>(index));
        else children[static_cast<std::size_t>(parentOfSource[index])].push_back(static_cast<std::int32_t>(index));
    }
    // 从根做深度优先（显式栈，避免深链递归），父一定先于子入列。
    std::vector<std::int32_t> order;
    order.reserve(jointCount);
    std::vector<unsigned char> visited(jointCount, 0);
    std::vector<std::int32_t> stack;
    for (auto it = roots.rbegin(); it != roots.rend(); ++it) stack.push_back(*it);
    while (!stack.empty())
    {
        const auto current = stack.back();
        stack.pop_back();
        if (visited[static_cast<std::size_t>(current)]) continue;
        visited[static_cast<std::size_t>(current)] = 1;
        order.push_back(current);
        const auto& siblings = children[static_cast<std::size_t>(current)];
        for (auto it = siblings.rbegin(); it != siblings.rend(); ++it) stack.push_back(*it);
    }
    if (order.size() != jointCount) { detail = "joint hierarchy cycle"; return false; }
    std::vector<std::int32_t> outputOfSource(jointCount, -1);
    for (std::size_t out = 0; out < order.size(); ++out) outputOfSource[static_cast<std::size_t>(order[out])] = static_cast<std::int32_t>(out);
    output.nodes.clear();
    output.parents.assign(jointCount, -1);
    output.source.assign(jointCount, 0);
    for (std::size_t out = 0; out < order.size(); ++out)
    {
        const auto source = order[out];
        output.nodes.push_back(skin.joints[source]);
        output.source[out] = static_cast<std::uint16_t>(source);
        const auto parent = parentOfSource[static_cast<std::size_t>(source)];
        output.parents[out] = parent < 0 ? -1 : outputOfSource[static_cast<std::size_t>(parent)];
        if (output.parents[out] >= static_cast<std::int32_t>(out)) { detail = "joint hierarchy cycle"; return false; }
    }
    return true;
}

std::string ClipLabel(const cgltf_animation& animation, std::size_t index)
{
    return animation.name ? std::string(animation.name) : ("#" + std::to_string(index));
}

// 结构预检：只读通道/采样器结构，不读 accessor 数据，因此可以先于 cgltf_validate 给出具体诊断。
bool PrecheckAnimations(const cgltf_data& data, std::string& key, std::string& detail)
{
    for (cgltf_size animationIndex = 0; animationIndex < data.animations_count; ++animationIndex)
    {
        const auto& animation = data.animations[animationIndex];
        const auto label = ClipLabel(animation, animationIndex);
        if (animation.channels_count == 0 || animation.samplers_count == 0)
        { key = "Assets.UnsupportedAnimation"; detail = "clip " + label + " has no channels"; return false; }
        for (cgltf_size channelIndex = 0; channelIndex < animation.channels_count; ++channelIndex)
        {
            const auto& channel = animation.channels[channelIndex];
            const auto prefix = "clip " + label + " channel " + std::to_string(channelIndex);
            if (!channel.sampler) { key = "Assets.InvalidModel"; detail = prefix + " has no sampler"; return false; }
            if (channel.target_path == cgltf_animation_path_type_weights)
            { key = "Assets.UnsupportedFeature"; detail = prefix + " animates morph weights"; return false; }
            if (channel.target_path != cgltf_animation_path_type_translation
                && channel.target_path != cgltf_animation_path_type_rotation
                && channel.target_path != cgltf_animation_path_type_scale)
            { key = "Assets.InvalidModel"; detail = prefix + " has an unknown target path"; return false; }
            switch (channel.sampler->interpolation)
            {
            case cgltf_interpolation_type_linear:
            case cgltf_interpolation_type_step:
                break;
            case cgltf_interpolation_type_cubic_spline:
                key = "Assets.UnsupportedFeature";
                detail = prefix + " uses CUBICSPLINE interpolation";
                return false;
            default:
                key = "Assets.UnsupportedFeature";
                detail = prefix + " uses an unknown interpolation";
                return false;
            }
        }
    }
    return true;
}

bool BuildSkeletonData(const cgltf_data& data, const cgltf_skin& skin, const JointTopology& topology,
    const std::vector<Mat4>& worlds, const Mat4& skinSpaceInverse, float scale,
    std::vector<ModelJoint>& output, std::unordered_map<const cgltf_node*, std::uint16_t>& jointsByNode,
    std::string& detail)
{
    const auto jointCount = skin.joints_count;
    const auto* inverseBind = skin.inverse_bind_matrices;
    if (!inverseBind) { detail = "missing inverseBindMatrices"; return false; }
    if (inverseBind->type != cgltf_type_mat4 || inverseBind->component_type != cgltf_component_type_r_32f
        || inverseBind->count < jointCount || inverseBind->is_sparse)
    { detail = "inverseBindMatrices accessor"; return false; }
    // 绑定烘焙方向（不是直白的 inverseBind * meshNodeWorld，乘 W 会让 W 作用两次）：
    // glTF 的关节矩阵是 inverse(meshNodeWorld) * jointWorld * inverseBind，其中 inverse(meshNodeWorld)
    // 抵掉蒙皮网格节点自身的全局变换 W。本引擎把蒙皮表达在实体局部空间，顶点烘成 vertex' = W * vertex，
    // 运行时用 skin = jointWorld * inverseBind'。要求两者给出同一结果：
    //   jointWorld * inverseBind' * (W * vertex) = jointWorld * inverseBind * vertex
    // 因此 inverseBind' = inverseBind * inverse(W)。导入缩放 s 等价于世界共轭 S * M * S^-1，
    // 先对逆绑定做同样共轭，再乘已含缩放的 inverse(W_eff)。
    std::vector<Mat4> bakedBind(jointCount);
    for (std::size_t index = 0; index < jointCount; ++index)
    {
        std::array<float, 16> raw{};
        if (!cgltf_accessor_read_float(inverseBind, index, raw.data(), 16))
        { detail = "inverseBindMatrices entry " + std::to_string(index); return false; }
        for (const auto value : raw) if (!std::isfinite(value))
        { detail = "inverseBindMatrices entry " + std::to_string(index) + " not finite"; return false; }
        const Mat4 bind{ raw };
        bakedBind[index] = ScaleConjugated(bind, scale) * skinSpaceInverse;
        for (const auto value : bakedBind[index].values) if (!std::isfinite(value))
        { detail = "inverseBindMatrices entry " + std::to_string(index) + " not finite"; return false; }
    }
    output.resize(jointCount);
    jointsByNode.reserve(jointCount);
    for (std::size_t out = 0; out < jointCount; ++out)
    {
        const cgltf_node& node = *topology.nodes[out];
        ModelJoint& joint = output[out];
        joint.name = node.name ? node.name : "Joint";
        joint.parent = topology.parents[out];
        const Mat4& world = worlds[static_cast<std::size_t>(&node - data.nodes)];
        (void)world;
        // 运行时的 rest 必须是关节相对父关节的局部 TRS：动画通道输出的是局部 TRS，
        // 若这里烘焙世界变换，jointWorld 会二次累加位移（T-pose 直接拉成竖条）。
        const Mat4 local{ NodeLocalMatrix(node, scale) };
        Transform transform;
        if (!TryDecomposeTrs(local, transform))
        {
            detail = "joint " + joint.name + " rest transform is not TRS";
            return false;
        }
        joint.restTranslation = { transform.position.x, transform.position.y, transform.position.z };
        joint.restRotation = { transform.rotation.x, transform.rotation.y, transform.rotation.z, transform.rotation.w };
        joint.restScale = { transform.scale.x, transform.scale.y, transform.scale.z };
        joint.inverseBind = bakedBind[topology.source[out]].values;
        jointsByNode.emplace(&node, static_cast<std::uint16_t>(out));
    }
    return true;
}

// JOINTS_0/WEIGHTS_0：最多 4 个影响，权重有限且和为正，归一到 1（容差 1e-4）。
bool ReadSkinning(const cgltf_accessor& joints, const cgltf_accessor& weights, std::size_t vertexCount,
    std::size_t jointCount, std::span<const std::uint16_t> remap, std::vector<MeshSkinVertex>& output, std::string& detail)
{
    if (joints.type != cgltf_type_vec4 || joints.count != vertexCount || joints.is_sparse || joints.normalized)
    { detail = "JOINTS_0 accessor"; return false; }
    if (joints.component_type != cgltf_component_type_r_8u && joints.component_type != cgltf_component_type_r_16u
        && joints.component_type != cgltf_component_type_r_32u)
    { detail = "JOINTS_0 component type"; return false; }
    if (weights.type != cgltf_type_vec4 || weights.count != vertexCount || weights.is_sparse)
    { detail = "WEIGHTS_0 accessor"; return false; }
    if (weights.component_type != cgltf_component_type_r_32f && !weights.normalized)
    { detail = "WEIGHTS_0 component type"; return false; }
    output.resize(vertexCount);
    for (std::size_t vertex = 0; vertex < vertexCount; ++vertex)
    {
        cgltf_uint joint[4]{};
        float weight[4]{};
        if (!cgltf_accessor_read_uint(&joints, vertex, joint, 4) || !cgltf_accessor_read_float(&weights, vertex, weight, 4))
        { detail = "vertex " + std::to_string(vertex) + " skinning attributes"; return false; }
        float sum = 0.0f;
        for (std::size_t influence = 0; influence < 4; ++influence)
        {
            if (!std::isfinite(weight[influence]) || weight[influence] < 0.0f)
            { detail = "vertex " + std::to_string(vertex) + " weight not finite"; return false; }
            if (joint[influence] >= jointCount)
            {
                detail = "vertex " + std::to_string(vertex) + " joint index "
                    + std::to_string(joint[influence]) + " >= " + std::to_string(jointCount);
                return false;
            }
            sum += weight[influence];
        }
        if (!(sum > 0.0f)) { detail = "vertex " + std::to_string(vertex) + " weight sum 0"; return false; }
        if (std::abs(sum - 1.0f) > 1e-4f)
            for (auto& value : weight) value /= sum;
        MeshSkinVertex& target = output[vertex];
        for (std::size_t influence = 0; influence < 4; ++influence)
        {
            target.joints[influence] = remap.empty() ? static_cast<std::uint16_t>(joint[influence]) : remap[joint[influence]];
            target.weights[influence] = weight[influence];
        }
    }
    return true;
}

// 把网格节点的全局变换烘进绑定姿势顶点，使蒙皮表达在实体局部空间（StaticModelPreparation 随后
// 对蒙皮部件使用单位 modelFromMesh，不会再乘一次节点变换）。
void BakeMeshNodeTransform(MeshData& mesh, const Mat4& meshNodeWorld)
{
    bool identity = true;
    const Mat4 unit{};
    for (std::size_t index = 0; index < 16 && identity; ++index)
        if (std::abs(meshNodeWorld.values[index] - unit.values[index]) > 1e-6f) identity = false;
    if (identity) return;
    const auto inverse = Inverse(meshNodeWorld);
    if (!inverse) return; // 奇异变换在骨架构建阶段已被拒绝，这里只是防御。
    const auto inverseTranspose = Transpose(*inverse);
    for (auto& vertex : mesh.vertices)
    {
        const auto position = TransformPoint(meshNodeWorld, { vertex.position[0], vertex.position[1], vertex.position[2] });
        vertex.position = { position.x, position.y, position.z };
        if (const auto normal = TransformNormal(meshNodeWorld, { vertex.normal[0], vertex.normal[1], vertex.normal[2] }))
            vertex.normal = { normal->x, normal->y, normal->z };
        const auto tangent = Normalize(TransformVector(inverseTranspose, { vertex.tangent[0], vertex.tangent[1], vertex.tangent[2] }));
        if (LengthSquared(tangent) > 1e-12f) vertex.tangent = { tangent.x, tangent.y, tangent.z, vertex.tangent[3] };
    }
}

// 每个 clip 的名字来自 glTF 动画，duration 取所有采样输入时间的最大值；没有对应关节的通道不输出。
bool ImportGltfAnimations(const cgltf_data& data, const std::unordered_map<const cgltf_node*, std::uint16_t>& jointsByNode,
    std::vector<ModelAnimationClip>& output, std::vector<AssetDiagnostic>& diagnostics)
{
    for (cgltf_size animationIndex = 0; animationIndex < data.animations_count; ++animationIndex)
    {
        const auto& animation = data.animations[animationIndex];
        const auto label = ClipLabel(animation, animationIndex);
        ModelAnimationClip clip;
        clip.name = animation.name ? animation.name : "Animation";
        for (cgltf_size channelIndex = 0; channelIndex < animation.channels_count; ++channelIndex)
        {
            const auto& channel = animation.channels[channelIndex];
            const auto prefix = "clip " + label + " channel " + std::to_string(channelIndex);
            const cgltf_animation_sampler& sampler = *channel.sampler;
            ModelAnimationPath path = ModelAnimationPath::Rotation;
            if (channel.target_path == cgltf_animation_path_type_translation) path = ModelAnimationPath::Translation;
            else if (channel.target_path == cgltf_animation_path_type_rotation) path = ModelAnimationPath::Rotation;
            else if (channel.target_path == cgltf_animation_path_type_scale) path = ModelAnimationPath::Scale;
            else { Diagnostic(diagnostics, "Assets.UnsupportedFeature", prefix + " target path"); return false; }
            ModelAnimationInterpolation interpolation = ModelAnimationInterpolation::Linear;
            if (sampler.interpolation == cgltf_interpolation_type_step) interpolation = ModelAnimationInterpolation::Step;
            else if (sampler.interpolation != cgltf_interpolation_type_linear)
            { Diagnostic(diagnostics, "Assets.UnsupportedFeature", prefix + " interpolation"); return false; }
            const auto* input = sampler.input;
            const auto* values = sampler.output;
            if (!input || !values || input->type != cgltf_type_scalar || input->component_type != cgltf_component_type_r_32f
                || input->is_sparse || input->count == 0 || input->count > MaxAnimationKeys)
            { Diagnostic(diagnostics, "Assets.InvalidModel", prefix + " input accessor"); return false; }
            const auto components = path == ModelAnimationPath::Rotation ? 4u : 3u;
            if ((values->type != cgltf_type_vec4 && values->type != cgltf_type_vec3) || values->count != input->count
                || values->is_sparse || values->component_type != cgltf_component_type_r_32f)
            { Diagnostic(diagnostics, "Assets.InvalidModel", prefix + " output accessor"); return false; }
            if (static_cast<std::uint32_t>(cgltf_num_components(values->type)) != components)
            { Diagnostic(diagnostics, "Assets.InvalidModel", prefix + " output components"); return false; }
            ModelAnimationChannel target;
            target.path = path;
            target.interpolation = interpolation;
            target.times.resize(input->count);
            target.values.resize(input->count);
            for (cgltf_size key = 0; key < input->count; ++key)
            {
                float time = 0.0f;
                if (!cgltf_accessor_read_float(input, key, &time, 1) || !std::isfinite(time))
                { Diagnostic(diagnostics, "Assets.InvalidModel", prefix + " time not finite"); return false; }
                if (key > 0 && !(time > target.times[static_cast<std::size_t>(key) - 1]))
                { Diagnostic(diagnostics, "Assets.InvalidModel", prefix + " times not strictly increasing"); return false; }
                std::array<float, 4> value{};
                if (!cgltf_accessor_read_float(values, key, value.data(), components))
                { Diagnostic(diagnostics, "Assets.InvalidModel", prefix + " value"); return false; }
                for (std::size_t component = 0; component < 4; ++component)
                    if (!std::isfinite(value[component]))
                    { Diagnostic(diagnostics, "Assets.InvalidModel", prefix + " value not finite"); return false; }
                target.times[static_cast<std::size_t>(key)] = time;
                target.values[static_cast<std::size_t>(key)] = value;
            }
            // duration 取整个动画采样输入时间的最大值（含指向非关节节点的通道），与契约字面一致。
            clip.duration = (std::max)(clip.duration, target.times.back());
            const auto found = channel.target_node ? jointsByNode.find(channel.target_node) : jointsByNode.end();
            if (found == jointsByNode.end()) continue; // 非关节节点没有运行时通道，静默跳过。
            target.joint = found->second;
            // 同一关节同一路径只允许一条通道；仅当数据完全相同（重复导出）时收敛为一条。
            const auto duplicate = std::find_if(clip.channels.begin(), clip.channels.end(),
                [&](const ModelAnimationChannel& existing) { return existing.joint == target.joint && existing.path == target.path; });
            if (duplicate != clip.channels.end())
            {
                const bool identical = duplicate->interpolation == target.interpolation
                    && duplicate->times == target.times && duplicate->values == target.values;
                if (!identical)
                {
                    Diagnostic(diagnostics, "Assets.InvalidModel",
                        prefix + " duplicates joint " + std::to_string(target.joint) + " with different data");
                    return false;
                }
                continue;
            }
            clip.channels.push_back(std::move(target));
        }
        // 通道全部指向非关节节点时运行时没有可播放数据；保留 clip 名称与时长，但明确提示数据被忽略。
        if (clip.channels.empty())
            Diagnostic(diagnostics, "Assets.UnsupportedFeature", "clip " + label + " has no joint channels", true);
        output.push_back(std::move(clip));
    }
    return true;
}

bool ImportGltf(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
    const ImportOptions& options, StaticModel& output, std::vector<AssetDiagnostic>& diagnostics)
{
    GltfFileContext context{ options.allowedRoot };
    cgltf_options settings{};
    settings.file = { ReadGltfFile, ReleaseGltfFile, &context };
    cgltf_data* raw = nullptr;
    if (cgltf_parse(&settings, bytes.data(), bytes.size(), &raw) != cgltf_result_success)
    { Diagnostic(diagnostics, "Assets.InvalidModel", asset_io::Utf8(path)); return false; }
    std::unique_ptr<cgltf_data, decltype(&cgltf_free)> data(raw, cgltf_free);
    std::vector<SubsurfaceParameters> subsurface;
    if (!ValidateGltfAlpha(*data, diagnostics)||!ValidateGltfClearCoat(*data, diagnostics)||!ValidateGltfLayers(*data,subsurface,diagnostics)) return false;
    if (data->nodes_count > MaxNodes || data->extensions_required_count != 0)
    { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "required extension or node limit"); return false; }
    for (cgltf_size index = 0; index < data->buffer_views_count; ++index)
        if (data->buffer_views[index].has_meshopt_compression)
        { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "meshopt compression"); return false; }
    for (cgltf_size index = 0; index < data->textures_count; ++index)
        if (data->textures[index].has_basisu || data->textures[index].has_webp)
        { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "compressed/extended image"); return false; }
    for (cgltf_size index = 0; index < data->buffers_count; ++index)
    {
        const auto& buffer = data->buffers[index];
        if (buffer.size > asset_io::MaxFileBytes) return false;
        if (buffer.uri && std::strncmp(buffer.uri, "data:", 5) != 0)
        {
            std::filesystem::path resolved; std::string relative;
            if (!asset_io::ResolveReference(path, options.allowedRoot, buffer.uri, resolved, relative))
            { Diagnostic(diagnostics, "Assets.ReferenceOutsideRoot", buffer.uri); return false; }
            output.dependencies.push_back(relative);
        }
    }
    const auto filename = asset_io::Utf8(path);
    if (cgltf_load_buffers(&settings, data.get(), filename.c_str()) != cgltf_result_success)
    { Diagnostic(diagnostics, "Assets.InvalidModel", "buffer or accessor validation"); return false; }

    // 骨架与动画的结构预检先于 cgltf_validate：节点环、CUBICSPLINE 在这里得到具体诊断，而不是被
    // 笼统的 accessor 校验覆盖。此阶段只读节点/通道结构，不读 accessor 数据。
    const cgltf_skin* skin = data->skins_count ? &data->skins[0] : nullptr;
    if (data->skins_count > 1)
        Diagnostic(diagnostics, "Assets.UnsupportedFeature", "multiple skins; only the first is used", true);
    JointTopology topology;
    if (skin)
    {
        std::string detail;
        if (!BuildJointTopology(*skin, *data, diagnostics, topology, detail))
        { Diagnostic(diagnostics, "Assets.InvalidSkeleton", std::move(detail)); return false; }
    }
    if (data->animations_count)
    {
        std::string animationKey, animationDetail;
        if (!PrecheckAnimations(*data, animationKey, animationDetail))
        { Diagnostic(diagnostics, animationKey.c_str(), std::move(animationDetail)); return false; }
    }
    if (cgltf_validate(data.get()) != cgltf_result_success)
    { Diagnostic(diagnostics, "Assets.InvalidModel", "buffer or accessor validation"); return false; }

    // 节点世界矩阵与网格→节点绑定：蒙皮网格要把网格节点的全局变换烘进顶点与逆绑定矩阵。
    std::vector<Mat4> worlds;
    ComputeNodeWorlds(*data, options.scale, worlds);
    std::vector<std::int32_t> meshNodes(data->meshes_count, -1);
    std::vector<std::uint32_t> meshReferenceCounts(data->meshes_count, 0);
    std::vector<unsigned char> meshHasSkin(data->meshes_count, 0);
    for (cgltf_size index = 0; index < data->nodes_count; ++index)
    {
        const auto& node = data->nodes[index];
        if (!node.mesh) continue;
        const auto mesh = static_cast<std::size_t>(node.mesh - data->meshes);
        ++meshReferenceCounts[mesh];
        if (meshNodes[mesh] < 0) meshNodes[mesh] = static_cast<std::int32_t>(index);
        // 只要有一个引用节点带 skin，整个网格就按蒙皮导入（同一网格不能既是静态又是蒙皮）。
        if (node.skin) meshHasSkin[mesh] = 1;
    }
    const auto meshSkinned = [&](std::size_t mesh) { return meshHasSkin[mesh] != 0; };
    for (std::size_t mesh = 0; mesh < meshNodes.size(); ++mesh)
        if (meshSkinned(mesh) && meshReferenceCounts[mesh] > 1)
            Diagnostic(diagnostics, "Assets.UnsupportedFeature", "skinned mesh referenced by multiple nodes", true);
    // 一套骨架只能表达一个实体局部绑定空间；契约要求蒙皮网格节点的全局变换为恒等，
    // 这里容忍同一个非恒等变换，但拒绝互相冲突的绑定空间（逆绑定矩阵无法同时正确）。
    Mat4 skinSpace{};
    if (skin)
    {
        bool initialized = false;
        for (std::size_t mesh = 0; mesh < meshNodes.size(); ++mesh)
        {
            if (!meshSkinned(mesh)) continue;
            const Mat4& world = worlds[static_cast<std::size_t>(meshNodes[mesh])];
            if (!initialized) { skinSpace = world; initialized = true; continue; }
            for (std::size_t element = 0; element < 16; ++element)
                if (std::abs(world.values[element] - skinSpace.values[element]) > 1e-5f)
                { Diagnostic(diagnostics, "Assets.InvalidSkeleton", "skinned meshes use different node transforms"); return false; }
        }
    }
    Mat4 skinSpaceInverse{};
    std::unordered_map<const cgltf_node*, std::uint16_t> jointsByNode;
    // JOINTS_0 的下标是 glTF skin.joints 的原始顺序；资产里的 joints 已按父序重排，
    // 顶点权重必须经过同一置换，否则旋转会作用到错误的关节（静止姿态看不出来）。
    std::vector<std::uint16_t> skinToJoint;
    if (skin)
    {
        if (const auto inverse = Inverse(skinSpace)) skinSpaceInverse = *inverse;
        else { Diagnostic(diagnostics, "Assets.InvalidSkeleton", "mesh node transform is singular"); return false; }
        std::string detail;
        if (!BuildSkeletonData(*data, *skin, topology, worlds, skinSpaceInverse, options.scale,
            output.joints, jointsByNode, detail))
        { Diagnostic(diagnostics, "Assets.InvalidSkeleton", std::move(detail)); return false; }
        skinToJoint.resize(skin->joints_count);
        for (cgltf_size index = 0; index < skin->joints_count; ++index)
        {
            const auto found = jointsByNode.find(skin->joints[index]);
            if (found == jointsByNode.end())
            { Diagnostic(diagnostics, "Assets.InvalidSkeleton", "skin joint missing from topology"); return false; }
            skinToJoint[index] = found->second;
        }
    }

    std::unordered_map<const cgltf_image*, std::int32_t> imageIndices;
    for (cgltf_size index = 0; index < data->images_count; ++index)
    {
        const auto& image = data->images[index];
        TextureReference texture;
        if (image.mime_type) texture.mimeType = image.mime_type;
        if (image.buffer_view)
        {
            const auto* view = image.buffer_view;
            if (view->size > asset_io::MaxFileBytes || !view->buffer || !view->buffer->data) return false;
            const auto* begin = static_cast<const std::uint8_t*>(view->buffer->data) + view->offset;
            texture.embeddedBytes.assign(begin, begin + view->size);
        }
        else if (image.uri)
        {
            if (std::strncmp(image.uri, "data:", 5) == 0)
            { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "image data URI; use bufferView or external image"); return false; }
            if (!AddExternalTexture(output, texture, image.uri, path, options, diagnostics)) return false;
        }
        else { Diagnostic(diagnostics, "Assets.InvalidModel", "image source"); return false; }
        imageIndices[&image] = static_cast<std::int32_t>(output.textures.size());
        output.textures.push_back(std::move(texture));
    }
    const auto textureIndex = [&](const cgltf_texture_view& view) -> std::int32_t {
        if (!view.texture || !view.texture->image) return -1;
        return imageIndices.at(view.texture->image);
    };
    for (cgltf_size index = 0; index < data->materials_count; ++index)
    {
        const auto& source = data->materials[index];
        MaterialData material;
        material.name = source.name ? source.name : "Material";
        material.subsurface=subsurface[index];
        if (source.has_pbr_specular_glossiness
            || source.has_clearcoat || source.has_transmission || source.has_volume || source.has_sheen
            || source.has_iridescence || source.has_anisotropy || source.has_dispersion || material.subsurface.weight>0)
            Diagnostic(diagnostics, "Assets.MaterialApproximation", material.name, true);
        const auto& pbr = source.pbr_metallic_roughness;
        std::copy_n(pbr.base_color_factor, 4, material.baseColor.begin());
        material.metallic = pbr.metallic_factor;
        material.roughness = pbr.roughness_factor;
        for (const auto* view : { &pbr.base_color_texture, &pbr.metallic_roughness_texture, &source.normal_texture,
            &source.clearcoat.clearcoat_texture,&source.clearcoat.clearcoat_roughness_texture,&source.clearcoat.clearcoat_normal_texture,
            &source.sheen.sheen_color_texture,&source.sheen.sheen_roughness_texture,&source.anisotropy.anisotropy_texture })
            if (view->texture && (view->texcoord != 0 || view->has_transform))
            { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "texture coordinate set/transform"); return false; }
        material.baseColorTexture = textureIndex(pbr.base_color_texture);
        material.normalTexture = textureIndex(source.normal_texture);
        material.normalScale = source.normal_texture.scale;
        material.metallicRoughnessTexture = textureIndex(pbr.metallic_roughness_texture);
        material.doubleSided = source.double_sided != 0;
        material.alphaMode = source.alpha_mode == cgltf_alpha_mode_mask ? MaterialAlphaMode::Mask
            : source.alpha_mode == cgltf_alpha_mode_blend ? MaterialAlphaMode::Blend : MaterialAlphaMode::Opaque;
        material.alphaCutoff = source.alpha_cutoff;
        if(source.has_clearcoat)
        {
            material.clearCoat.weight=source.clearcoat.clearcoat_factor;
            material.clearCoat.roughness=source.clearcoat.clearcoat_roughness_factor;
            material.clearCoatWeightTexture=textureIndex(source.clearcoat.clearcoat_texture);
            material.clearCoatRoughnessTexture=textureIndex(source.clearcoat.clearcoat_roughness_texture);
            material.clearCoatNormalTexture=textureIndex(source.clearcoat.clearcoat_normal_texture);
            if(material.clearCoatNormalTexture>=0)material.clearCoat.normalScale=source.clearcoat.clearcoat_normal_texture.scale;
        }
        if(source.has_sheen)
        {
            material.sheen.color={source.sheen.sheen_color_factor[0],source.sheen.sheen_color_factor[1],source.sheen.sheen_color_factor[2]};
            material.sheen.roughness=source.sheen.sheen_roughness_factor;
            material.sheenColorTexture=textureIndex(source.sheen.sheen_color_texture);
            material.sheenRoughnessTexture=textureIndex(source.sheen.sheen_roughness_texture);
        }
        if(source.has_anisotropy)
        {
            material.anisotropy={source.anisotropy.anisotropy_strength,source.anisotropy.anisotropy_rotation};
            material.anisotropyTexture=textureIndex(source.anisotropy.anisotropy_texture);
        }
        output.materials.push_back(std::move(material));
    }

    std::vector<std::vector<std::uint32_t>> meshIndices(data->meshes_count);
    for (cgltf_size meshIndex = 0; meshIndex < data->meshes_count; ++meshIndex)
    {
        const auto& sourceMesh = data->meshes[meshIndex];
        const auto meshNode = meshNodes[static_cast<std::size_t>(meshIndex)];
        const bool skinned = meshSkinned(static_cast<std::size_t>(meshIndex));
        for (cgltf_size primitiveIndex = 0; primitiveIndex < sourceMesh.primitives_count; ++primitiveIndex)
        {
            const auto& primitive = sourceMesh.primitives[primitiveIndex];
            if (primitive.type != cgltf_primitive_type_triangles || primitive.targets_count || primitive.has_draco_mesh_compression)
            { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "non-triangle, morph or compressed primitive"); return false; }
            const cgltf_accessor *position = nullptr, *normal = nullptr, *uv = nullptr, *tangent = nullptr;
            const cgltf_accessor *joints0 = nullptr, *weights0 = nullptr;
            bool extraInfluences = false;
            for (cgltf_size attribute = 0; attribute < primitive.attributes_count; ++attribute)
            {
                const auto& value = primitive.attributes[attribute];
                if (value.type == cgltf_attribute_type_position) position = value.data;
                if (value.type == cgltf_attribute_type_normal) normal = value.data;
                if (value.type == cgltf_attribute_type_texcoord && value.index == 0) uv = value.data;
                if (value.type == cgltf_attribute_type_tangent) tangent = value.data;
                if (value.type == cgltf_attribute_type_joints) { if (value.index == 0) joints0 = value.data; else extraInfluences = true; }
                if (value.type == cgltf_attribute_type_weights) { if (value.index == 0) weights0 = value.data; else extraInfluences = true; }
                if (value.data->is_sparse)
                { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "sparse accessor"); return false; }
            }
            if (skinned)
            {
                if (extraInfluences)
                { Diagnostic(diagnostics, "Assets.InvalidSkeleton", "primitive uses more than 4 influences per vertex"); return false; }
                if (!joints0 || !weights0)
                { Diagnostic(diagnostics, "Assets.InvalidSkeleton", "skinned primitive without JOINTS_0/WEIGHTS_0"); return false; }
            }
            else if (joints0 || weights0)
            { Diagnostic(diagnostics, "Assets.InvalidSkeleton", "skinning attributes without a skin on the mesh node"); return false; }
            if (!position || position->type != cgltf_type_vec3 || position->count == 0 || position->count > MaxVertices)
            { Diagnostic(diagnostics, "Assets.InvalidModel", "positions"); return false; }
            MeshData mesh;
            mesh.name = sourceMesh.name ? sourceMesh.name : "Mesh";
            mesh.material = primitive.material ? static_cast<std::int32_t>(primitive.material - data->materials) : -1;
            mesh.skinned = skinned;
            mesh.meshNode = static_cast<std::uint32_t>(meshNode < 0 ? 0 : meshNode);
            mesh.vertices.resize(position->count);
            for (cgltf_size vertex = 0; vertex < position->count; ++vertex)
            {
                auto& target = mesh.vertices[vertex];
                if (!cgltf_accessor_read_float(position, vertex, target.position.data(), 3)) return false;
                if (normal && (normal->count != position->count || !cgltf_accessor_read_float(normal, vertex, target.normal.data(), 3))) return false;
                if (uv && (uv->count != position->count || !cgltf_accessor_read_float(uv, vertex, target.uv.data(), 2))) return false;
                if (tangent && (tangent->count != position->count || !cgltf_accessor_read_float(tangent, vertex, target.tangent.data(), 4))) return false;
                for (auto& component : target.position) component *= options.scale;
            }
            if (skinned)
            {
                std::string detail;
                if (!ReadSkinning(*joints0, *weights0, mesh.vertices.size(), output.joints.size(), skinToJoint, mesh.skinning, detail))
                { Diagnostic(diagnostics, "Assets.InvalidSkeleton", std::move(detail)); return false; }
                // 顶点与法线随网格节点变换进入实体局部空间；StaticModelPreparation 对蒙皮部件用单位变换。
                BakeMeshNodeTransform(mesh, skinSpace);
            }
            const auto count = primitive.indices ? primitive.indices->count : position->count;
            if (count == 0 || count > MaxVertices * 6 || count % 3 != 0 || (primitive.indices && primitive.indices->is_sparse)) return false;
            mesh.indices.reserve(count);
            for (cgltf_size element = 0; element < count; ++element)
            {
                const auto value = primitive.indices ? cgltf_accessor_read_index(primitive.indices, element) : element;
                if (value >= mesh.vertices.size()) return false;
                mesh.indices.push_back(static_cast<std::uint32_t>(value));
            }
            GenerateBasis(mesh, !normal, options.generateTangents && !tangent);
            meshIndices[meshIndex].push_back(static_cast<std::uint32_t>(output.meshes.size()));
            output.meshes.push_back(std::move(mesh));
        }
    }
    for (cgltf_size index = 0; index < data->nodes_count; ++index)
    {
        const auto& source = data->nodes[index];
        if (source.has_mesh_gpu_instancing)
        { Diagnostic(diagnostics, "Assets.UnsupportedFeature", "mesh GPU instancing"); return false; }
        ModelNode node;
        node.name = source.name ? source.name : "Node";
        node.parent = source.parent ? static_cast<std::int32_t>(source.parent - data->nodes) : -1;
        node.localMatrix = NodeLocalMatrix(source, options.scale);
        if (source.mesh) node.meshes = meshIndices[static_cast<std::size_t>(source.mesh - data->meshes)];
        output.nodes.push_back(std::move(node));
    }
    return ImportGltfAnimations(*data, jointsByNode, output.animations, diagnostics);
}

std::string String(ufbx_string value) { return { value.data, value.length }; }
bool ImportFbxAlpha(const ufbx_material& source, MaterialData& material, std::vector<AssetDiagnostic>& diagnostics)
{
    const auto& opacity = source.pbr.opacity;
    const auto& transparency = source.fbx.transparency_color;
    const auto& factor = source.fbx.transparency_factor;
    // 单独的 opacity 图和有色透射不能冒充 baseColor 图的 alpha；禁止静默丢掉洞口。
    for (const auto* map : { &opacity, &transparency, &factor })
        if (map->texture && map->texture_enabled && !map->feature_disabled)
        { Diagnostic(diagnostics, "Assets.UnsupportedFeature", material.name + ": separate opacity texture"); return false; }
    const auto scalar = [&](const ufbx_material_map& map, double& value) {
        value = map.value_real;
        if (!std::isfinite(value) || value < 0.0 || value > 1.0)
        { Diagnostic(diagnostics, "Assets.InvalidMaterial", material.name + ": opacity"); return false; }
        if (map.value_components >= 3 && (!std::isfinite(map.value_vec3.y) || !std::isfinite(map.value_vec3.z)
            || std::abs(value - map.value_vec3.y) > 1e-6 || std::abs(value - map.value_vec3.z) > 1e-6))
        { Diagnostic(diagnostics, "Assets.UnsupportedFeature", material.name + ": colored transmission/opacity"); return false; }
        return true;
    };
    double alpha = 1.0;
    if (opacity.has_value && !opacity.feature_disabled)
    {
        if (!scalar(opacity, alpha)) return false;
    }
    else if (factor.has_value || transparency.has_value)
    {
        double amount = factor.value_real, color = 1.0;
        if (!std::isfinite(amount) || amount < 0.0 || amount > 1.0)
        { Diagnostic(diagnostics, "Assets.InvalidMaterial", material.name + ": transparency factor"); return false; }
        if (transparency.has_value && !scalar(transparency, color)) return false;
        alpha = 1.0 - amount * color;
        if (alpha < 1.0) Diagnostic(diagnostics, "Assets.MaterialApproximation", material.name + ": scalar transmission to alpha", true);
    }
    material.baseColor[3] = static_cast<float>(alpha);
    if (alpha < 1.0) material.alphaMode = MaterialAlphaMode::Blend;
    material.doubleSided = source.features.double_sided.enabled;
    return true;
}

bool ImportFbx(const std::filesystem::path& path, const std::vector<std::uint8_t>& bytes,
    const ImportOptions& options, StaticModel& output, std::vector<AssetDiagnostic>& diagnostics)
{
    ufbx_load_opts settings{};
    settings.target_axes = ufbx_axes_right_handed_y_up;
    settings.target_unit_meters = 1.0;
    settings.space_conversion = UFBX_SPACE_CONVERSION_MODIFY_GEOMETRY;
    settings.geometry_transform_handling = UFBX_GEOMETRY_TRANSFORM_HANDLING_HELPER_NODES;
    settings.inherit_mode_handling = UFBX_INHERIT_MODE_HANDLING_HELPER_NODES;
    settings.generate_missing_normals = true;
    const auto filename = asset_io::Utf8(path);
    settings.filename = { filename.data(), filename.size() };
    ufbx_error error{};
    std::unique_ptr<ufbx_scene, decltype(&ufbx_free_scene)> scene(
        ufbx_load_memory(bytes.data(), bytes.size(), &settings, &error), ufbx_free_scene);
    if (!scene)
    { Diagnostic(diagnostics, "Assets.InvalidModel", String(error.description)); return false; }
    if (scene->anim_stacks.count || scene->bones.count || scene->skin_deformers.count || scene->blend_deformers.count || scene->cache_deformers.count)
    { Diagnostic(diagnostics, "Assets.UnsupportedAnimation"); return false; }
    if (scene->nodes.count > MaxNodes) return false;

    std::unordered_map<const ufbx_texture*, std::int32_t> textureIndices;
    bool textureOk = true;
    const auto textureIndex = [&](const ufbx_material_map& map) -> std::int32_t {
        const auto* texture = map.texture;
        if (!texture || !map.texture_enabled) return -1;
        if (texture->type != UFBX_TEXTURE_FILE)
        { Diagnostic(diagnostics, "Assets.MaterialApproximation", "layered/procedural texture", true); return -1; }
        if (const auto found = textureIndices.find(texture); found != textureIndices.end()) return found->second;
        TextureReference target;
        if (texture->content.size)
        {
            const auto* begin = static_cast<const std::uint8_t*>(texture->content.data);
            target.embeddedBytes.assign(begin, begin + texture->content.size);
        }
        else
        {
            const auto relative = String(texture->relative_filename);
            textureOk = AddExternalTexture(output, target, relative.empty() ? String(texture->filename) : relative,
                path, options, diagnostics, false) && textureOk;
        }
        const auto result = static_cast<std::int32_t>(output.textures.size());
        output.textures.push_back(std::move(target));
        textureIndices[texture] = result;
        return result;
    };
    for (const auto* source : scene->materials)
    {
        MaterialData material;
        material.name = String(source->name);
        const auto& color = source->pbr.base_color.has_value ? source->pbr.base_color : source->fbx.diffuse_color;
        if (color.has_value)
            material.baseColor = { static_cast<float>(color.value_vec3.x), static_cast<float>(color.value_vec3.y), static_cast<float>(color.value_vec3.z), 1.0f };
        if (!ImportFbxAlpha(*source, material, diagnostics)) return false;
        if (source->pbr.metalness.has_value) material.metallic = static_cast<float>(source->pbr.metalness.value_real);
        if (source->pbr.roughness.has_value) material.roughness = static_cast<float>(source->pbr.roughness.value_real);
        material.baseColorTexture = textureIndex(source->pbr.base_color.texture ? source->pbr.base_color : source->fbx.diffuse_color);
        material.normalTexture = textureIndex(source->pbr.normal_map.texture ? source->pbr.normal_map : source->fbx.normal_map);
        output.materials.push_back(std::move(material));
    }
    if (!textureOk) return false;
    for (const auto* sourceNode : scene->nodes)
    {
        ModelNode node;
        node.name = String(sourceNode->name);
        node.parent = sourceNode->parent ? static_cast<std::int32_t>(sourceNode->parent->typed_id) : -1;
        for (std::size_t column = 0; column < 4; ++column)
            for (std::size_t row = 0; row < 3; ++row)
                node.localMatrix[column * 4 + row] = static_cast<float>(sourceNode->node_to_parent.v[column * 3 + row]);
        for (const auto offset : { 12, 13, 14 }) node.localMatrix[offset] *= options.scale;
        if (const auto* source = sourceNode->mesh)
        {
            if (source->num_indices > MaxVertices || source->max_face_triangles > MaxVertices / 3) return false;
            std::vector<std::uint32_t> triangleIndices(source->max_face_triangles * 3);
            for (const auto& part : source->material_parts)
            {
                if (part.num_triangles == 0) continue;
                MeshData mesh;
                mesh.name = String(source->name);
                mesh.meshNode = static_cast<std::uint32_t>(sourceNode->typed_id);
                if (part.index < sourceNode->materials.count)
                    mesh.material = static_cast<std::int32_t>(sourceNode->materials.data[part.index]->typed_id);
                for (const auto faceIndex : part.face_indices)
                {
                    const auto face = source->faces.data[faceIndex];
                    const auto triangleCount = ufbx_triangulate_face(triangleIndices.data(), triangleIndices.size(), source, face);
                    for (std::size_t corner = 0; corner < static_cast<std::size_t>(triangleCount) * 3; ++corner)
                    {
                        const auto index = triangleIndices[corner];
                        MeshVertex vertex;
                        const auto position = ufbx_get_vertex_vec3(&source->vertex_position, index);
                        const auto normal = ufbx_get_vertex_vec3(&source->vertex_normal, index);
                        vertex.position = { static_cast<float>(position.x) * options.scale, static_cast<float>(position.y) * options.scale, static_cast<float>(position.z) * options.scale };
                        vertex.normal = { static_cast<float>(normal.x), static_cast<float>(normal.y), static_cast<float>(normal.z) };
                        if (source->vertex_uv.exists)
                        {
                            const auto uv = ufbx_get_vertex_vec2(&source->vertex_uv, index);
                            vertex.uv = { static_cast<float>(uv.x), 1.0f - static_cast<float>(uv.y) };
                        }
                        mesh.indices.push_back(static_cast<std::uint32_t>(mesh.vertices.size()));
                        mesh.vertices.push_back(vertex);
                    }
                }
                GenerateBasis(mesh, false, options.generateTangents);
                node.meshes.push_back(static_cast<std::uint32_t>(output.meshes.size()));
                output.meshes.push_back(std::move(mesh));
            }
        }
        output.nodes.push_back(std::move(node));
    }
    return true;
}
}

bool ValidateStaticModel(const StaticModel& model) { return ValidateModel(model); }

bool ImportStaticModel(const std::filesystem::path& source, const ImportOptions& requested,
    StaticModel& output, std::vector<AssetDiagnostic>& diagnostics)
{
    try
    {
        if (!std::isfinite(requested.scale) || requested.scale <= 0)
        { Diagnostic(diagnostics, "Assets.InvalidOptions"); return false; }
        const auto path = std::filesystem::weakly_canonical(source);
        ImportOptions options = requested;
        if (options.allowedRoot.empty()) options.allowedRoot = path.parent_path();
        if (!asset_io::Within(options.allowedRoot, path))
        { Diagnostic(diagnostics, "Assets.ReferenceOutsideRoot", asset_io::Utf8(source)); return false; }
        std::vector<std::uint8_t> bytes;
        if (!asset_io::Read(path, bytes))
        { Diagnostic(diagnostics, "Assets.ReadFailed", asset_io::Utf8(path)); return false; }
        std::string extension = path.extension().string();
        std::transform(extension.begin(), extension.end(), extension.begin(), [](unsigned char value) { return static_cast<char>(std::tolower(value)); });
        StaticModel model;
        const auto previousDiagnostics = diagnostics.size();
        bool imported = false;
        if (extension == ".fbx") imported = ImportFbx(path, bytes, options, model, diagnostics);
        else if (extension == ".gltf" || extension == ".glb") imported = ImportGltf(path, bytes, options, model, diagnostics);
        else { Diagnostic(diagnostics, "Assets.UnsupportedFormat", extension); return false; }
        if (!imported || !ValidateModel(model))
        {
            if (diagnostics.size() == previousDiagnostics) Diagnostic(diagnostics, "Assets.InvalidModel", asset_io::Utf8(path));
            return false;
        }
        std::sort(model.dependencies.begin(), model.dependencies.end());
        model.dependencies.erase(std::unique(model.dependencies.begin(), model.dependencies.end()), model.dependencies.end());
        for (auto& material : model.materials)
            for (const auto index : { material.baseColorTexture, material.normalTexture, material.metallicRoughnessTexture,
                material.clearCoatWeightTexture, material.clearCoatRoughnessTexture, material.clearCoatNormalTexture,
                material.sheenColorTexture,material.sheenRoughnessTexture,material.anisotropyTexture })
                if (index >= 0 && model.textures[static_cast<std::size_t>(index)].missing) material.needsFallbackTexture = true;
        output = std::move(model);
        return true;
    }
    catch (const std::exception& error)
    { Diagnostic(diagnostics, "Assets.InvalidModel", error.what()); return false; }
}
}
