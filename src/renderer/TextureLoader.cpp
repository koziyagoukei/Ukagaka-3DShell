#include "renderer/TextureLoader.h"

#include "platform/Log.h"
#include "platform/Utf.h"

#include <stb_image.h>
#include <wincodec.h>

#include <algorithm>
#include <cwctype>
#include <format>
#include <fstream>

namespace uka3d {
namespace {

std::vector<std::uint8_t> readFileBytes(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input)
        throw std::runtime_error(std::format("ファイルを開けません: {}", pathToUtf8(path)));
    input.seekg(0, std::ios::end);
    const auto size = input.tellg();
    if (size <= 0)
        throw std::runtime_error(std::format("ファイルが空です: {}", pathToUtf8(path)));
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    input.seekg(0, std::ios::beg);
    input.read(reinterpret_cast<char *>(bytes.data()), size);
    if (!input)
        throw std::runtime_error(std::format("ファイルを読み込めません: {}", pathToUtf8(path)));
    return bytes;
}

bool decodeWithStb(const std::vector<std::uint8_t> &bytes, Image &image) {
    int width = 0;
    int height = 0;
    int channels = 0;
    stbi_uc *pixels = stbi_load_from_memory(bytes.data(), static_cast<int>(bytes.size()), &width, &height,
                                            &channels, 4);
    if (pixels == nullptr)
        return false;
    image.width = static_cast<std::uint32_t>(width);
    image.height = static_cast<std::uint32_t>(height);
    image.rgba.assign(pixels, pixels + static_cast<std::size_t>(width) * height * 4);
    stbi_image_free(pixels);
    return true;
}

bool decodeWithWic(const std::filesystem::path &path, Image &image, HRESULT &error) {
    ComPtr<IWICImagingFactory> factory;
    error = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory));
    if (FAILED(error))
        return false;
    ComPtr<IWICBitmapDecoder> decoder;
    error = factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand,
                                               &decoder);
    if (FAILED(error))
        return false;
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(error = decoder->GetFrame(0, &frame)))
        return false;
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(error = factory->CreateFormatConverter(&converter)))
        return false;
    if (FAILED(error = converter->Initialize(frame.Get(), GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone,
                                             nullptr, 0.0, WICBitmapPaletteTypeCustom)))
        return false;
    UINT width = 0;
    UINT height = 0;
    if (FAILED(error = converter->GetSize(&width, &height)) || width == 0 || height == 0)
        return false;
    image.width = width;
    image.height = height;
    image.rgba.resize(static_cast<std::size_t>(width) * height * 4);
    error = converter->CopyPixels(nullptr, width * 4, static_cast<UINT>(image.rgba.size()), image.rgba.data());
    return SUCCEEDED(error);
}

} // namespace

bool Image::hasTranslucency() const noexcept {
    for (std::size_t i = 3; i < rgba.size(); i += 4) {
        if (rgba[i] < 250)
            return true;
    }
    return false;
}

Image decodeImageFile(const std::filesystem::path &path) {
    const auto bytes = readFileBytes(path);
    Image image;
    if (decodeWithStb(bytes, image))
        return image;
    const std::string stbReason = stbi_failure_reason() ? stbi_failure_reason() : "unknown";
    HRESULT wicError = S_OK;
    if (decodeWithWic(path, image, wicError))
        return image;
    throw std::runtime_error(std::format("画像をデコードできません: {} (stb: {}, WIC: {})", pathToUtf8(path),
                                         stbReason, log::hresultToString(wicError)));
}

Texture TextureCache::createFromImage(const Image &image, bool generateMips) {
    auto *device = d3d_.device();
    D3D11_TEXTURE2D_DESC desc{};
    desc.Width = image.width;
    desc.Height = image.height;
    desc.MipLevels = generateMips ? 0 : 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (generateMips ? D3D11_BIND_RENDER_TARGET : 0);
    desc.MiscFlags = generateMips ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;

    ComPtr<ID3D11Texture2D> texture;
    if (generateMips) {
        throwIfFailed(device->CreateTexture2D(&desc, nullptr, &texture), "CreateTexture2D(texture)");
        d3d_.context()->UpdateSubresource(texture.Get(), 0, nullptr, image.rgba.data(), image.width * 4, 0);
    } else {
        const D3D11_SUBRESOURCE_DATA data{image.rgba.data(), image.width * 4, 0};
        throwIfFailed(device->CreateTexture2D(&desc, &data, &texture), "CreateTexture2D(texture)");
    }
    Texture result;
    throwIfFailed(device->CreateShaderResourceView(texture.Get(), nullptr, &result.srv), "CreateShaderResourceView");
    if (generateMips)
        d3d_.context()->GenerateMips(result.srv.Get());
    result.translucent = image.hasTranslucency();
    return result;
}

const Texture &TextureCache::load(const std::filesystem::path &path) {
    std::wstring key = path.lexically_normal().native();
    std::transform(key.begin(), key.end(), key.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    if (const auto found = cache_.find(key); found != cache_.end())
        return found->second;

    Texture texture;
    try {
        const Image image = decodeImageFile(path);
        texture = createFromImage(image, true);
        ++loaded_;
        log::debug("テクスチャ読み込み: {} ({}x{}{})", pathToUtf8(path), image.width, image.height,
                   texture.translucent ? ", 半透明あり" : "");
    } catch (const std::exception &e) {
        ++failed_;
        log::warn("テクスチャを読み込めません（白で代替）: {}", e.what());
    }
    return cache_.emplace(std::move(key), std::move(texture)).first->second;
}

} // namespace uka3d
