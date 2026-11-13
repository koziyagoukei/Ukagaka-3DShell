#pragma once

#include "mmd/MmdModel.h"
#include "renderer/Camera.h"
#include "renderer/D3DContext.h"
#include "mmd/Skinning.h"
#include "renderer/TextureLoader.h"

#include <DirectXMath.h>

#include <array>
#include <memory>
#include <span>
#include <vector>

namespace uka3d {

// Uploads a PMX model to the GPU and draws it with an MMD-like shading model.
// Skinning is done on the CPU by libmmd; only positions/normals are streamed.
class ModelRenderer {
  public:
    struct SharedAssets {
        explicit SharedAssets(D3DContext &d3d) : textures(d3d) {}
        struct Mesh {
            ComPtr<ID3D11Buffer> vertices;
            ComPtr<ID3D11Buffer> indices;
            DXGI_FORMAT indexFormat{DXGI_FORMAT_R32_UINT};
        };
        TextureCache textures;
        std::map<std::filesystem::path, std::weak_ptr<Mesh>> meshes;
    };
    enum class Pass { all, opaque, translucent };
    ModelRenderer(D3DContext &d3d, const MmdModel &model, SharedAssets &assets);

    using DynamicVertex = SkinnedVertex;

    // Uploads the animated vertices and material values of one frame.
    void update(const mmd::AnimatedModelFrame &frame);
    // Stores a newly simulated frame on the CPU. The previous one is kept for
    // interpolation unless `discontinuous` (seek, first frame).
    void setFrame(const mmd::AnimatedModelFrame &frame, bool discontinuous);
    // Zero-copy variant: returns the buffer for the new simulated frame (the
    // previous one is kept for interpolation); fill it, then call commitFrame().
    [[nodiscard]] std::span<DynamicVertex> beginFrame();
    void commitFrame(const mmd::AnimatedModelFrame &frame, bool discontinuous);
    // Writes lerp(previous, current, alpha) to the GPU vertex buffer.
    void upload(float alpha);
    void draw(const Camera &camera, DirectX::FXMMATRIX world = DirectX::XMMatrixIdentity(), Pass pass = Pass::all);
    [[nodiscard]] bool visible() const noexcept { return visible_; }

  private:
    struct GpuMaterial {
        UINT indexOffset{};
        UINT indexCount{};
        bool doubleSided{};
        bool edge{};
        bool textureTranslucent{};
        ID3D11ShaderResourceView *texture{};
        ID3D11ShaderResourceView *sphere{};
        ID3D11ShaderResourceView *toon{};
        std::uint8_t sphereMode{}; // 0 none, 1 multiply, 2 add, 3 sub-texture (treated as none)
    };

    void createPipeline();
    void createBuffers();
    void loadMaterials();
    ID3D11ShaderResourceView *sharedToon(int index);

    D3DContext &d3d_;
    const MmdModel &model_;
    SharedAssets &assets_;
    std::shared_ptr<SharedAssets::Mesh> mesh_;

    ComPtr<ID3D11VertexShader> vs_;
    ComPtr<ID3D11PixelShader> ps_;
    ComPtr<ID3D11VertexShader> edgeVs_;
    ComPtr<ID3D11PixelShader> edgePs_;
    ComPtr<ID3D11InputLayout> layout_;
    ComPtr<ID3D11Buffer> dynamicVb_; // float3 position, float3 normal
    ComPtr<ID3D11Buffer> sceneCb_;
    ComPtr<ID3D11Buffer> materialCb_;
    ComPtr<ID3D11SamplerState> wrapSampler_;
    ComPtr<ID3D11SamplerState> clampSampler_;
    ComPtr<ID3D11BlendState> blend_;
    ComPtr<ID3D11DepthStencilState> depth_;
    ComPtr<ID3D11DepthStencilState> depthReadOnly_;
    ComPtr<ID3D11RasterizerState> cullBack_;
    ComPtr<ID3D11RasterizerState> cullNone_;
    ComPtr<ID3D11RasterizerState> cullFront_;

    std::vector<DynamicVertex> previous_;
    std::vector<DynamicVertex> current_;
    std::vector<GpuMaterial> materials_;
    std::vector<std::shared_ptr<const Texture>> materialTextures_;
    std::vector<mmd::AnimatedModelFrame::Material> animatedMaterials_;
    std::array<Texture, 10> sharedToons_{};
    bool visible_{true};
};

} // namespace uka3d
