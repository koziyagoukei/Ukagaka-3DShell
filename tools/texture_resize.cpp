// uka3d_texresize - downsizes colour textures for distribution.
//
//   uka3d_texresize <input.png> <output.png> <max-size>
//
// Colour is filtered in linear light (sRGB in, sRGB out) and weighted by
// alpha, so transparent texels do not bleed dark fringes into the edges of
// cut-out parts (hair tips, lace). Alpha itself is filtered linearly. The
// number of channels of the source is kept (RGB stays RGB). Images that are
// already small enough are copied unchanged. Only use this for colour
// textures: normal maps or other data textures need a different filter.
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include <stb_image.h>
#include <stb_image_resize2.h>
#include <stb_image_write.h>

#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace {

std::string utf8(const std::wstring &text) {
    const int n = WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<std::size_t>(std::max(0, n - 1)), '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.c_str(), -1, out.data(), n, nullptr, nullptr);
    return out;
}

FILE *openFile(const std::wstring &path, const wchar_t *mode) {
    FILE *file = nullptr;
    return _wfopen_s(&file, path.c_str(), mode) == 0 ? file : nullptr;
}

void writeToFile(void *context, void *data, int size) {
    std::fwrite(data, 1, static_cast<std::size_t>(size), static_cast<FILE *>(context));
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    if (argc != 4) {
        std::fputs("usage: uka3d_texresize <input.png> <output.png> <max-size>\n", stderr);
        return 1;
    }
    const std::wstring input = argv[1], output = argv[2];
    const int maxSize = std::max(1, _wtoi(argv[3]));

    FILE *in = openFile(input, L"rb");
    if (in == nullptr) {
        std::fprintf(stderr, "cannot open %s\n", utf8(input).c_str());
        return 1;
    }
    std::vector<unsigned char> bytes;
    unsigned char chunk[65536];
    for (std::size_t n; (n = std::fread(chunk, 1, sizeof(chunk), in)) > 0;)
        bytes.insert(bytes.end(), chunk, chunk + n);
    std::fclose(in);
    int width = 0, height = 0, channels = 0;
    std::unique_ptr<stbi_uc, decltype(&stbi_image_free)> pixels(
        stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height, &channels, 0),
        &stbi_image_free);
    if (!pixels) {
        std::fprintf(stderr, "cannot decode %s: %s\n", utf8(input).c_str(), stbi_failure_reason());
        return 1;
    }

    int newWidth = width, newHeight = height;
    while (std::max(newWidth, newHeight) > maxSize) {
        newWidth = std::max(1, newWidth / 2);
        newHeight = std::max(1, newHeight / 2);
    }
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(output).parent_path(), ec);
    if (newWidth == width && newHeight == height) {
        std::filesystem::copy_file(input, output, std::filesystem::copy_options::overwrite_existing, ec);
        std::printf("%s: %dx%d kept\n", utf8(std::filesystem::path(input).filename().wstring()).c_str(), width, height);
        return ec ? 1 : 0;
    }

    stbir_pixel_layout layout = STBIR_RGBA; // straight alpha: the resizer weights colour by alpha
    switch (channels) {
    case 1: layout = STBIR_1CHANNEL; break;
    case 2: layout = STBIR_RA; break;
    case 3: layout = STBIR_RGB; break;
    default: layout = STBIR_RGBA; break;
    }
    std::unique_ptr<unsigned char[]> resized(new unsigned char[static_cast<std::size_t>(newWidth) * newHeight * channels]);
    if (stbir_resize_uint8_srgb(pixels.get(), width, height, 0, resized.get(), newWidth, newHeight, 0, layout) == nullptr) {
        std::fprintf(stderr, "resize failed: %s\n", utf8(input).c_str());
        return 1;
    }
    FILE *out = openFile(output, L"wb");
    if (out == nullptr) {
        std::fprintf(stderr, "cannot write %s\n", utf8(output).c_str());
        return 1;
    }
    stbi_write_png_compression_level = 9;
    const int ok = stbi_write_png_to_func(writeToFile, out, newWidth, newHeight, channels, resized.get(), newWidth * channels);
    std::fclose(out);
    if (!ok)
        return 1;
    std::printf("%s: %dx%d -> %dx%d (%d channels)\n", utf8(std::filesystem::path(input).filename().wstring()).c_str(), width,
                height, newWidth, newHeight, channels);
    return 0;
}
