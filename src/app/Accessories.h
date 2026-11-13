#pragma once

#include "app/Playback.h"
#include "ipc/Protocol.h"
#include "renderer/ModelRenderer.h"

#include <map>

namespace uka3d {

struct AccessoryTransform {
    mmd::Float3 position{};
    mmd::Float3 rotation{};
    float scale{1.0F};
    [[nodiscard]] DirectX::XMMATRIX matrix() const;
};

// libmmd supplies a skinning transform, which maps the bind joint to the
// current joint. Restore the bind joint origin before applying it.
DirectX::XMMATRIX attachmentMatrix(const mmd::PmxModel &model, std::size_t bone,
                                   std::span<const BoneTransform> previous, std::span<const BoneTransform> current,
                                   float alpha);

class Accessories {
  public:
    Accessories(D3DContext &d3d, ModelRenderer::SharedAssets &assets, const MmdModel &main,
                std::filesystem::path base, std::vector<std::filesystem::path> roots);
    void loadConfig(const std::filesystem::path &path);
    void replaceMain(const MmdModel &main);
    [[nodiscard]] ipc::Json execute(const ipc::Request &request);
    [[nodiscard]] bool visible() const;
    [[nodiscard]] bool animating() const;
    void update(double dt);
    void draw(ModelRenderer &mainRenderer, const Camera &camera, std::span<const BoneTransform> previous,
              std::span<const BoneTransform> current, float alpha);

  private:
    struct Entry {
        std::string id;
        std::filesystem::path path;
        std::filesystem::path motion;
        std::string parent;
        std::string bone;
        std::optional<std::size_t> boneIndex;
        AccessoryTransform transform;
        bool attached{true};
        bool visible{false};
        bool physics{false};
        bool loop{true};
        bool dirty{true};
        Playback playback{Playback::Config{}};
        std::unique_ptr<MmdModel> model;
        std::unique_ptr<ModelRenderer> renderer;
        Bounds bounds;
    };
    [[nodiscard]] std::filesystem::path assetPath(const ipc::Json &value) const;
    [[nodiscard]] ipc::Json status(const Entry &entry) const;
    void load(const ipc::Json &params);
    void bind(Entry &entry, const std::string &parent, const std::string &bone);
    [[nodiscard]] bool active(const Entry &entry) const;
    void saveState() const;

    D3DContext &d3d_;
    ModelRenderer::SharedAssets &assets_;
    const MmdModel *main_;
    Bounds mainBounds_;
    std::filesystem::path base_;
    std::vector<std::filesystem::path> roots_;
    std::filesystem::path statePath_;
    std::map<std::string, Entry> entries_;
};

} // namespace uka3d
