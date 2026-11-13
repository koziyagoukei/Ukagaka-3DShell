#include "renderer/ModelRenderer.h"

#include "platform/Log.h"
#include "platform/Utf.h"

#include <windows.h>

#include "shaders/model_PSEdge.h"
#include "shaders/model_PSMain.h"
#include "shaders/model_VSEdge.h"
#include "shaders/model_VSMain.h"

#include <algorithm>
#include <cmath>
#include <cstring>

using namespace DirectX;

namespace uka3d {
namespace {

struct StaticVertex {
    XMFLOAT2 uv;
    float edgeScale;
};

struct SceneConstants {
    XMFLOAT4X4 viewProj;
    XMFLOAT4X4 view;
    XMFLOAT4X4 world;
    XMFLOAT3 lightDir;
    float edgePixel;
    XMFLOAT3 lightColor;
    float pad0;
    XMFLOAT3 eyePos;
    float pad1;
};
static_assert(sizeof(SceneConstants) % 16 == 0);

struct MaterialConstants {
    XMFLOAT4 diffuse;
    XMFLOAT3 specular;
    float shininess;
    XMFLOAT3 ambient;
    UINT flags;
    XMFLOAT4 edgeColor;
    float edgeSize;
    XMFLOAT3 pad;
    XMFLOAT4 texMul;
    XMFLOAT4 texAdd;
    XMFLOAT4 sphereMul;
    XMFLOAT4 sphereAdd;
    XMFLOAT4 toonMul;
    XMFLOAT4 toonAdd;
};
static_assert(sizeof(MaterialConstants) % 16 == 0);

constexpr UINT flagTexture = 1;
constexpr UINT flagSphere = 2;
constexpr UINT flagSphereAdd = 4;
constexpr UINT flagToon = 8;

XMFLOAT4 toF4(const mmd::Float4 &v) {
    return {v[0], v[1], v[2], v[3]};
}
XMFLOAT3 toF3(const mmd::Float3 &v) {
    return {v[0], v[1], v[2]};
}

// MMD ships toon01.bmp - toon10.bmp with the application; they are not
// redistributable, so approximate them with procedural gradients:
// upper half lit (white), lower half shadow tint.
Image makeSharedToonImage(int index) {
    static constexpr std::array<std::array<float, 3>, 10> shadows{{
        {0.80F, 0.80F, 0.80F}, {0.94F, 0.85F, 0.80F}, {0.75F, 0.75F, 0.75F}, {0.92F, 0.82F, 0.82F},
        {0.85F, 0.85F, 0.92F}, {0.70F, 0.70F, 0.70F}, {0.88F, 0.78F, 0.72F}, {0.86F, 0.86F, 0.86F},
        {0.96F, 0.90F, 0.88F}, {0.82F, 0.86F, 0.80F},
    }};
    const auto &shadow = shadows[static_cast<std::size_t>(std::clamp(index, 0, 9))];
    Image image;
    image.width = 1;
    image.height = 32;
    image.rgba.resize(image.height * 4);
    for (std::uint32_t y = 0; y < image.height; ++y) {
        const float v = (static_cast<float>(y) + 0.5F) / static_cast<float>(image.height);
        // Soft step around the terminator (v = 0.5).
        const float t = std::clamp((v - 0.45F) / 0.1F, 0.0F, 1.0F);
        const float s = t * t * (3.0F - 2.0F * t);
        for (int c = 0; c < 3; ++c)
            image.rgba[y * 4 + c] = static_cast<std::uint8_t>(std::lround(255.0F * (1.0F + (shadow[c] - 1.0F) * s)));
        image.rgba[y * 4 + 3] = 255;
    }
    return image;
}

} // namespace

ModelRenderer::ModelRenderer(D3DContext &d3d, const MmdModel &model, SharedAssets &assets)
    : d3d_(d3d), model_(model), assets_(assets) {
    createPipeline();
    createBuffers();
    loadMaterials();
}

void ModelRenderer::createPipeline() {
    auto *device = d3d_.device();
    throwIfFailed(device->CreateVertexShader(g_VSMain, sizeof(g_VSMain), nullptr, &vs_), "CreateVertexShader");
    throwIfFailed(device->CreatePixelShader(g_PSMain, sizeof(g_PSMain), nullptr, &ps_), "CreatePixelShader");
    throwIfFailed(device->CreateVertexShader(g_VSEdge, sizeof(g_VSEdge), nullptr, &edgeVs_), "CreateVertexShader(edge)");
    throwIfFailed(device->CreatePixelShader(g_PSEdge, sizeof(g_PSEdge), nullptr, &edgePs_), "CreatePixelShader(edge)");

    const D3D11_INPUT_ELEMENT_DESC elements[] = {
        {"POSITION", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"NORMAL", 0, DXGI_FORMAT_R32G32B32_FLOAT, 0, 12, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 1, 0, D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 1, DXGI_FORMAT_R32_FLOAT, 1, 8, D3D11_INPUT_PER_VERTEX_DATA, 0},
    };
    throwIfFailed(device->CreateInputLayout(elements, ARRAYSIZE(elements), g_VSMain, sizeof(g_VSMain), &layout_),
                  "CreateInputLayout");

    D3D11_BUFFER_DESC cb{};
    cb.Usage = D3D11_USAGE_DYNAMIC;
    cb.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cb.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    cb.ByteWidth = sizeof(SceneConstants);
    throwIfFailed(device->CreateBuffer(&cb, nullptr, &sceneCb_), "CreateBuffer(scene)");
    cb.ByteWidth = sizeof(MaterialConstants);
    throwIfFailed(device->CreateBuffer(&cb, nullptr, &materialCb_), "CreateBuffer(material)");

    D3D11_SAMPLER_DESC sampler{};
    sampler.Filter = D3D11_FILTER_ANISOTROPIC;
    sampler.MaxAnisotropy = 8;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    sampler.MaxLOD = D3D11_FLOAT32_MAX;
    sampler.ComparisonFunc = D3D11_COMPARISON_NEVER;
    throwIfFailed(device->CreateSamplerState(&sampler, &wrapSampler_), "CreateSamplerState(wrap)");
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    throwIfFailed(device->CreateSamplerState(&sampler, &clampSampler_), "CreateSamplerState(clamp)");

    // Premultiplied-alpha "over" blending for both colour and alpha, so the
    // render target stays premultiplied for DirectComposition.
    D3D11_BLEND_DESC blend{};
    auto &rt = blend.RenderTarget[0];
    rt.BlendEnable = TRUE;
    rt.SrcBlend = D3D11_BLEND_ONE;
    rt.DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOp = D3D11_BLEND_OP_ADD;
    rt.SrcBlendAlpha = D3D11_BLEND_ONE;
    rt.DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    rt.BlendOpAlpha = D3D11_BLEND_OP_ADD;
    rt.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    throwIfFailed(device->CreateBlendState(&blend, &blend_), "CreateBlendState");

    D3D11_DEPTH_STENCIL_DESC depth{};
    depth.DepthEnable = TRUE;
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ALL;
    depth.DepthFunc = D3D11_COMPARISON_LESS_EQUAL;
    throwIfFailed(device->CreateDepthStencilState(&depth, &depth_), "CreateDepthStencilState");
    depth.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    throwIfFailed(device->CreateDepthStencilState(&depth, &depthReadOnly_), "CreateDepthStencilState(read-only)");

    // MMD/PMX uses D3D's default clockwise front faces in a left-handed space.
    D3D11_RASTERIZER_DESC raster{};
    raster.FillMode = D3D11_FILL_SOLID;
    raster.CullMode = D3D11_CULL_BACK;
    raster.DepthClipEnable = TRUE;
    raster.MultisampleEnable = TRUE;
    raster.AntialiasedLineEnable = FALSE;
    throwIfFailed(device->CreateRasterizerState(&raster, &cullBack_), "CreateRasterizerState(back)");
    raster.CullMode = D3D11_CULL_NONE;
    throwIfFailed(device->CreateRasterizerState(&raster, &cullNone_), "CreateRasterizerState(none)");
    raster.CullMode = D3D11_CULL_FRONT;
    throwIfFailed(device->CreateRasterizerState(&raster, &cullFront_), "CreateRasterizerState(front)");
}

void ModelRenderer::createBuffers() {
    auto *device = d3d_.device();
    const auto &pmx = model_.model();

    std::vector<ModelRenderer::DynamicVertex> dynamics(pmx.vertices.size());
    for (std::size_t i = 0; i < pmx.vertices.size(); ++i) {
        const auto &v = pmx.vertices[i];
        std::memcpy(dynamics[i].position, v.position.data(), sizeof(dynamics[i].position));
        std::memcpy(dynamics[i].normal, v.normal.data(), sizeof(dynamics[i].normal));
    }

    D3D11_BUFFER_DESC desc{};
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.ByteWidth = static_cast<UINT>(dynamics.size() * sizeof(ModelRenderer::DynamicVertex));
    D3D11_SUBRESOURCE_DATA data{dynamics.data(), 0, 0};
    throwIfFailed(device->CreateBuffer(&desc, &data, &dynamicVb_), "CreateBuffer(dynamic VB)");

    auto &cached = assets_.meshes[model_.modelPath()];
    mesh_ = cached.lock();
    if (mesh_)
        return;
    mesh_ = std::make_shared<SharedAssets::Mesh>();
    std::vector<StaticVertex> statics;
    statics.reserve(pmx.vertices.size());
    for (const auto &v : pmx.vertices)
        statics.push_back({{v.uv[0], v.uv[1]}, v.edgeScale});
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.CPUAccessFlags = 0;
    desc.ByteWidth = static_cast<UINT>(statics.size() * sizeof(StaticVertex));
    data.pSysMem = statics.data();
    throwIfFailed(device->CreateBuffer(&desc, &data, &mesh_->vertices), "CreateBuffer(static VB)");
    desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    if (pmx.vertices.size() <= 0xFFFF) {
        std::vector<std::uint16_t> indices(pmx.indices.begin(), pmx.indices.end());
        mesh_->indexFormat = DXGI_FORMAT_R16_UINT;
        desc.ByteWidth = static_cast<UINT>(indices.size() * sizeof(std::uint16_t));
        data.pSysMem = indices.data();
        throwIfFailed(device->CreateBuffer(&desc, &data, &mesh_->indices), "CreateBuffer(IB16)");
    } else {
        desc.ByteWidth = static_cast<UINT>(pmx.indices.size() * sizeof(std::uint32_t));
        data.pSysMem = pmx.indices.data();
        throwIfFailed(device->CreateBuffer(&desc, &data, &mesh_->indices), "CreateBuffer(IB32)");
    }
    cached = mesh_;
}

ID3D11ShaderResourceView *ModelRenderer::sharedToon(int index) {
    if (index < 0 || index >= static_cast<int>(sharedToons_.size()))
        return nullptr;
    auto &toon = sharedToons_[static_cast<std::size_t>(index)];
    if (!toon.srv)
        toon = assets_.textures.createFromImage(makeSharedToonImage(index), false);
    return toon.srv.Get();
}

void ModelRenderer::loadMaterials() {
    const auto &pmx = model_.model();
    const auto textureAt = [&](std::int32_t index) -> const Texture & {
        static const Texture missing;
        if (index < 0 || static_cast<std::size_t>(index) >= pmx.textures.size())
            return missing;
        materialTextures_.push_back(assets_.textures.load(model_.resolveTexturePath(static_cast<std::size_t>(index))));
        return *materialTextures_.back();
    };

    UINT offset = 0;
    materials_.reserve(pmx.materials.size());
    for (const auto &material : pmx.materials) {
        GpuMaterial gpu;
        gpu.indexOffset = offset;
        gpu.indexCount = material.indexCount;
        offset += material.indexCount;
        gpu.doubleSided = (material.drawFlags & 0x01) != 0;
        gpu.edge = (material.drawFlags & 0x10) != 0 && material.edgeSize > 0.0F;
        const auto &base = textureAt(material.textureIndex);
        gpu.texture = base.srv.Get();
        gpu.textureTranslucent = base.translucent;
        gpu.sphereMode = material.sphereMode;
        if (material.sphereMode == 1 || material.sphereMode == 2)
            gpu.sphere = textureAt(material.sphereTextureIndex).srv.Get();
        if (material.toonMode == 1)
            gpu.toon = sharedToon(material.toonTextureIndex);
        else
            gpu.toon = textureAt(material.toonTextureIndex).srv.Get();
        materials_.push_back(gpu);
        log::debug("マテリアル \"{}\": 面={} テクスチャ={} スフィア={}({}) トゥーン={}{} 両面={} エッジ={}",
                   material.name, material.indexCount / 3, material.textureIndex, material.sphereTextureIndex,
                   material.sphereMode, material.toonMode == 1 ? "共有" : "", material.toonTextureIndex,
                   gpu.doubleSided, gpu.edge);
    }
    log::info("テクスチャキャッシュ: 成功={} 失敗={}", assets_.textures.loadedCount(), assets_.textures.failedCount());

    animatedMaterials_.resize(pmx.materials.size());
    for (std::size_t i = 0; i < pmx.materials.size(); ++i) {
        const auto &m = pmx.materials[i];
        auto &a = animatedMaterials_[i];
        a.diffuse = m.diffuse;
        a.specular = m.specular;
        a.shininess = m.shininess;
        a.ambient = m.ambient;
        a.edgeColor = m.edgeColor;
        a.edgeSize = m.edgeSize;
    }
}

void ModelRenderer::update(const mmd::AnimatedModelFrame &frame) {
    setFrame(frame, true);
    upload(1.0F);
}

void ModelRenderer::setFrame(const mmd::AnimatedModelFrame &frame, bool discontinuous) {
    const auto &vertices = frame.vertices;
    if (vertices.size() != model_.model().vertices.size()) {
        visible_ = frame.visible;
        if (frame.materials.size() == animatedMaterials_.size())
            animatedMaterials_ = frame.materials;
        return;
    }
    const auto out = beginFrame();
    for (std::size_t i = 0; i < vertices.size(); ++i) {
        std::memcpy(out[i].position, vertices[i].position.data(), sizeof(out[i].position));
        std::memcpy(out[i].normal, vertices[i].normal.data(), sizeof(out[i].normal));
    }
    commitFrame(frame, discontinuous);
}

std::span<ModelRenderer::DynamicVertex> ModelRenderer::beginFrame() {
    // Keep the previous simulated state for render-rate interpolation.
    std::swap(previous_, current_);
    current_.resize(model_.model().vertices.size());
    return current_;
}

void ModelRenderer::commitFrame(const mmd::AnimatedModelFrame &frame, bool discontinuous) {
    visible_ = frame.visible;
    if (frame.materials.size() == animatedMaterials_.size())
        animatedMaterials_ = frame.materials;
    if (discontinuous || previous_.size() != current_.size())
        previous_ = current_;
}

void ModelRenderer::upload(float alpha) {
    if (current_.empty())
        return;
    alpha = std::clamp(alpha, 0.0F, 1.0F);
    auto *context = d3d_.context();
    D3D11_MAPPED_SUBRESOURCE mapped{};
    throwIfFailed(context->Map(dynamicVb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map(dynamic VB)");
    if (alpha >= 1.0F) {
        std::memcpy(mapped.pData, current_.data(), current_.size() * sizeof(DynamicVertex));
    } else {
        // Flat float arrays: this loop auto-vectorises.
        const float beta = 1.0F - alpha;
        const float *a = previous_.data()->position;
        const float *b = current_.data()->position;
        float *out = static_cast<float *>(mapped.pData);
        const std::size_t count = current_.size() * (sizeof(DynamicVertex) / sizeof(float));
        for (std::size_t i = 0; i < count; ++i)
            out[i] = a[i] * beta + b[i] * alpha; // the pixel shader renormalises normals
    }
    context->Unmap(dynamicVb_.Get(), 0);
}

void ModelRenderer::draw(const Camera &camera, FXMMATRIX world, Pass pass) {
    if (!visible_)
        return;
    auto *context = d3d_.context();
    const float aspect = static_cast<float>(d3d_.width()) / static_cast<float>(std::max(1u, d3d_.height()));

    SceneConstants scene{};
    const XMMATRIX view = camera.view();
    XMStoreFloat4x4(&scene.viewProj, view * camera.projection(aspect));
    XMStoreFloat4x4(&scene.view, view);
    XMStoreFloat4x4(&scene.world, world);
    // MMD default light: direction (-0.5, -1.0, 0.5), colour 154/255.
    XMStoreFloat3(&scene.lightDir, XMVector3Normalize(XMVectorSet(-0.5F, -1.0F, 0.5F, 0.0F)));
    scene.lightColor = {154.0F / 255.0F, 154.0F / 255.0F, 154.0F / 255.0F};
    scene.eyePos = camera.eye();
    scene.edgePixel = 2.0F * std::tan(camera.fovY * 0.5F) / static_cast<float>(std::max(1u, d3d_.height()));

    D3D11_MAPPED_SUBRESOURCE mapped{};
    throwIfFailed(context->Map(sceneCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped), "Map(scene CB)");
    std::memcpy(mapped.pData, &scene, sizeof(scene));
    context->Unmap(sceneCb_.Get(), 0);

    ID3D11Buffer *vbs[] = {dynamicVb_.Get(), mesh_->vertices.Get()};
    const UINT strides[] = {sizeof(ModelRenderer::DynamicVertex), sizeof(StaticVertex)};
    const UINT offsets[] = {0, 0};
    context->IASetVertexBuffers(0, 2, vbs, strides, offsets);
    context->IASetIndexBuffer(mesh_->indices.Get(), mesh_->indexFormat, 0);
    context->IASetInputLayout(layout_.Get());
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    ID3D11Buffer *cbs[] = {sceneCb_.Get(), materialCb_.Get()};
    context->VSSetConstantBuffers(0, 2, cbs);
    context->PSSetConstantBuffers(0, 2, cbs);
    ID3D11SamplerState *samplers[] = {wrapSampler_.Get(), clampSampler_.Get()};
    context->PSSetSamplers(0, 2, samplers);
    const float blendFactor[4] = {0, 0, 0, 0};
    context->OMSetBlendState(blend_.Get(), blendFactor, 0xFFFFFFFF);
    context->OMSetDepthStencilState(pass == Pass::translucent ? depthReadOnly_.Get() : depth_.Get(), 0);

    const auto drawInPass = [&](std::size_t i, bool edge) {
        const auto &m = animatedMaterials_[i];
        const bool translucent = m.diffuse[3] < 1.0F ||
            (edge ? m.edgeColor[3] < 1.0F : materials_[i].textureTranslucent ||
                     m.textureMultiply[3] + m.textureAdd[3] < 1.0F);
        return pass == Pass::all || translucent == (pass == Pass::translucent);
    };

    const auto writeMaterial = [&](std::size_t index, UINT flags) {
        const auto &m = animatedMaterials_[index];
        MaterialConstants constants{};
        constants.diffuse = toF4(m.diffuse);
        constants.specular = toF3(m.specular);
        constants.shininess = m.shininess;
        constants.ambient = toF3(m.ambient);
        constants.flags = flags;
        constants.edgeColor = toF4(m.edgeColor);
        constants.edgeSize = m.edgeSize;
        constants.texMul = toF4(m.textureMultiply);
        constants.texAdd = toF4(m.textureAdd);
        constants.sphereMul = toF4(m.sphereMultiply);
        constants.sphereAdd = toF4(m.sphereAdd);
        constants.toonMul = toF4(m.toonMultiply);
        constants.toonAdd = toF4(m.toonAdd);
        D3D11_MAPPED_SUBRESOURCE map{};
        throwIfFailed(context->Map(materialCb_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &map), "Map(material CB)");
        std::memcpy(map.pData, &constants, sizeof(constants));
        context->Unmap(materialCb_.Get(), 0);
    };

    // Main pass, in PMX material order (MMD's draw order for translucency).
    context->VSSetShader(vs_.Get(), nullptr, 0);
    context->PSSetShader(ps_.Get(), nullptr, 0);
    for (std::size_t i = 0; i < materials_.size(); ++i) {
        const auto &gpu = materials_[i];
        if (gpu.indexCount == 0 || animatedMaterials_[i].diffuse[3] <= 0.0F || !drawInPass(i, false))
            continue;
        UINT flags = 0;
        if (gpu.texture)
            flags |= flagTexture;
        if (gpu.sphere)
            flags |= flagSphere | (gpu.sphereMode == 2 ? flagSphereAdd : 0);
        if (gpu.toon)
            flags |= flagToon;
        writeMaterial(i, flags);
        ID3D11ShaderResourceView *srvs[] = {gpu.texture, gpu.sphere, gpu.toon};
        context->PSSetShaderResources(0, 3, srvs);
        context->RSSetState(gpu.doubleSided ? cullNone_.Get() : cullBack_.Get());
        context->DrawIndexed(gpu.indexCount, gpu.indexOffset, 0);
    }

    // Edge pass (inverted hull).
    context->VSSetShader(edgeVs_.Get(), nullptr, 0);
    context->PSSetShader(edgePs_.Get(), nullptr, 0);
    context->RSSetState(cullFront_.Get());
    for (std::size_t i = 0; i < materials_.size(); ++i) {
        const auto &gpu = materials_[i];
        if (!gpu.edge || gpu.indexCount == 0 || animatedMaterials_[i].diffuse[3] <= 0.0F || !drawInPass(i, true))
            continue;
        writeMaterial(i, 0);
        context->DrawIndexed(gpu.indexCount, gpu.indexOffset, 0);
    }

    ID3D11ShaderResourceView *nullSrvs[3] = {};
    context->PSSetShaderResources(0, 3, nullSrvs);
}

} // namespace uka3d
