#pragma once

#include "renderer/D3DContext.h"

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace uka3d {

struct Image {
    std::uint32_t width{};
    std::uint32_t height{};
    std::vector<std::uint8_t> rgba; // straight (non-premultiplied) RGBA8
    [[nodiscard]] bool hasTranslucency() const noexcept;
};

// Decodes an image file. PNG/JPEG/BMP/TGA/GIF/PSD via stb_image, anything else
// (DDS, TIFF, ...) via WIC. Throws std::runtime_error (UTF-8 message).
Image decodeImageFile(const std::filesystem::path &path);

struct Texture {
    ComPtr<ID3D11ShaderResourceView> srv;
    bool translucent{};
};

// Creates GPU textures with mip maps and caches them by normalized path.
class TextureCache {
  public:
    explicit TextureCache(D3DContext &d3d) : d3d_(d3d) {}

    // Returns nullptr srv when the file cannot be loaded (a warning is logged once).
    const Texture &load(const std::filesystem::path &path);
    Texture createFromImage(const Image &image, bool generateMips);

    [[nodiscard]] std::size_t loadedCount() const noexcept {
        return loaded_;
    }
    [[nodiscard]] std::size_t failedCount() const noexcept {
        return failed_;
    }

  private:
    D3DContext &d3d_;
    std::map<std::wstring, Texture> cache_;
    std::size_t loaded_{};
    std::size_t failed_{};
};

} // namespace uka3d
