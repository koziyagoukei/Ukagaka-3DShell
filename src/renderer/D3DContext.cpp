#include "renderer/D3DContext.h"

#include "platform/Log.h"
#include "platform/Utf.h"

#include <wincodec.h>

#include <algorithm>
#include <format>
#include <vector>

namespace uka3d {

D3DError::D3DError(const std::string &what, HRESULT hr)
    : std::runtime_error(std::format("{}: {}", what, log::hresultToString(hr))), hr_(hr) {}

void throwIfFailed(HRESULT hr, const char *what) {
    if (FAILED(hr))
        throw D3DError(what, hr);
}

D3DContext::D3DContext(const Config &config)
    : transparent_(config.transparent), width_(std::max(1u, config.width)), height_(std::max(1u, config.height)),
      msaaSamples_(config.msaaSamples) {
    createDevice(config.debugLayer);
    createSwapChain(config.hwnd);
    createTargets();
}

D3DContext::~D3DContext() {
    if (context_) {
        context_->ClearState();
        context_->Flush();
    }
}

void D3DContext::createDevice(bool debugLayer) {
    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    if (debugLayer)
        flags |= D3D11_CREATE_DEVICE_DEBUG;
    const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL obtained{};
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                                   D3D11_SDK_VERSION, &device_, &obtained, &context_);
    if (FAILED(hr) && debugLayer) {
        log::warn("D3D11デバッグレイヤーを利用できないため無効化します: {}", log::hresultToString(hr));
        flags &= ~D3D11_CREATE_DEVICE_DEBUG;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device_, &obtained, &context_);
    }
    if (FAILED(hr)) {
        log::warn("ハードウェアデバイスの作成に失敗したためWARPにフォールバックします: {}", log::hresultToString(hr));
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, levels, ARRAYSIZE(levels),
                               D3D11_SDK_VERSION, &device_, &obtained, &context_);
    }
    throwIfFailed(hr, "D3D11CreateDevice");

    ComPtr<IDXGIDevice> dxgiDevice;
    throwIfFailed(device_.As(&dxgiDevice), "IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter;
    throwIfFailed(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    DXGI_ADAPTER_DESC desc{};
    adapter->GetDesc(&desc);
    char name[256]{};
    WideCharToMultiByte(CP_UTF8, 0, desc.Description, -1, name, sizeof(name) - 1, nullptr, nullptr);
    log::info("Direct3D 11 初期化成功: アダプタ=\"{}\" 機能レベル={}.{} VRAM={}MB", name, (obtained >> 12) & 0xF,
              (obtained >> 8) & 0xF, desc.DedicatedVideoMemory / (1024 * 1024));

    // Queue at most one frame ahead: lower latency, and Present() blocks
    // instead of letting the CPU run ahead of the GPU.
    ComPtr<IDXGIDevice1> dxgiDevice1;
    if (SUCCEEDED(device_.As(&dxgiDevice1)))
        dxgiDevice1->SetMaximumFrameLatency(1);

    // Clamp MSAA to what the device supports for the colour format.
    while (msaaSamples_ > 1) {
        UINT quality = 0;
        if (SUCCEEDED(device_->CheckMultisampleQualityLevels(colorFormat, msaaSamples_, &quality)) && quality > 0)
            break;
        msaaSamples_ /= 2;
    }
    if (msaaSamples_ < 1)
        msaaSamples_ = 1;
    log::info("MSAA: {}x", msaaSamples_);
}

void D3DContext::createSwapChain(HWND hwnd) {
    ComPtr<IDXGIDevice> dxgiDevice;
    throwIfFailed(device_.As(&dxgiDevice), "IDXGIDevice");
    ComPtr<IDXGIAdapter> adapter;
    throwIfFailed(dxgiDevice->GetAdapter(&adapter), "GetAdapter");
    ComPtr<IDXGIFactory2> factory;
    throwIfFailed(adapter->GetParent(IID_PPV_ARGS(&factory)), "IDXGIFactory2");

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width = width_;
    desc.Height = height_;
    desc.Format = colorFormat;
    desc.SampleDesc.Count = 1;
    desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount = 2;
    desc.Scaling = DXGI_SCALING_STRETCH;
    desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;

    if (transparent_) {
        desc.AlphaMode = DXGI_ALPHA_MODE_PREMULTIPLIED;
        throwIfFailed(factory->CreateSwapChainForComposition(device_.Get(), &desc, nullptr, &swapChain_),
                      "CreateSwapChainForComposition");
        throwIfFailed(DCompositionCreateDevice(dxgiDevice.Get(), IID_PPV_ARGS(&dcompDevice_)),
                      "DCompositionCreateDevice");
        throwIfFailed(dcompDevice_->CreateTargetForHwnd(hwnd, TRUE, &dcompTarget_), "CreateTargetForHwnd");
        throwIfFailed(dcompDevice_->CreateVisual(&dcompVisual_), "CreateVisual");
        throwIfFailed(dcompVisual_->SetContent(swapChain_.Get()), "IDCompositionVisual::SetContent");
        throwIfFailed(dcompTarget_->SetRoot(dcompVisual_.Get()), "IDCompositionTarget::SetRoot");
        throwIfFailed(dcompDevice_->Commit(), "IDCompositionDevice::Commit");
        log::info("スワップチェーン: DirectComposition (premultiplied alpha) {}x{}", width_, height_);
    } else {
        desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
        throwIfFailed(factory->CreateSwapChainForHwnd(device_.Get(), hwnd, &desc, nullptr, nullptr, &swapChain_),
                      "CreateSwapChainForHwnd");
        log::info("スワップチェーン: HWND (不透明) {}x{}", width_, height_);
    }
    factory->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
}

void D3DContext::createTargets() {
    throwIfFailed(swapChain_->GetBuffer(0, IID_PPV_ARGS(&backBuffer_)), "GetBuffer");

    D3D11_TEXTURE2D_DESC color{};
    color.Width = width_;
    color.Height = height_;
    color.MipLevels = 1;
    color.ArraySize = 1;
    color.Format = colorFormat;
    color.SampleDesc.Count = msaaSamples_;
    color.Usage = D3D11_USAGE_DEFAULT;
    color.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (msaaSamples_ > 1) {
        throwIfFailed(device_->CreateTexture2D(&color, nullptr, &msaaColor_), "CreateTexture2D(MSAA)");
        throwIfFailed(device_->CreateRenderTargetView(msaaColor_.Get(), nullptr, &rtv_), "CreateRenderTargetView");
    } else {
        throwIfFailed(device_->CreateRenderTargetView(backBuffer_.Get(), nullptr, &rtv_), "CreateRenderTargetView");
    }

    D3D11_TEXTURE2D_DESC depth = color;
    depth.Format = DXGI_FORMAT_D24_UNORM_S8_UINT;
    depth.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    throwIfFailed(device_->CreateTexture2D(&depth, nullptr, &depth_), "CreateTexture2D(depth)");
    throwIfFailed(device_->CreateDepthStencilView(depth_.Get(), nullptr, &dsv_), "CreateDepthStencilView");

    D3D11_TEXTURE2D_DESC staging{};
    staging.Width = 1;
    staging.Height = 1;
    staging.MipLevels = 1;
    staging.ArraySize = 1;
    staging.Format = colorFormat;
    staging.SampleDesc.Count = 1;
    staging.Usage = D3D11_USAGE_STAGING;
    staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto &texture : probeStaging_) {
        if (!texture)
            throwIfFailed(device_->CreateTexture2D(&staging, nullptr, &texture), "CreateTexture2D(staging)");
    }
}

void D3DContext::releaseTargets() {
    context_->OMSetRenderTargets(0, nullptr, nullptr);
    rtv_.Reset();
    msaaColor_.Reset();
    dsv_.Reset();
    depth_.Reset();
    backBuffer_.Reset();
    probePending_.fill(std::nullopt);
    probedAlpha_.reset();
    context_->Flush();
}

void D3DContext::resize(UINT width, UINT height) {
    width = std::max(1u, width);
    height = std::max(1u, height);
    if (width == width_ && height == height_)
        return;
    releaseTargets();
    throwIfFailed(swapChain_->ResizeBuffers(0, width, height, DXGI_FORMAT_UNKNOWN, 0), "ResizeBuffers");
    width_ = width;
    height_ = height;
    createTargets();
    log::debug("スワップチェーンをリサイズ: {}x{}", width_, height_);
}

void D3DContext::beginFrame(const float clearColor[4]) {
    context_->ClearRenderTargetView(rtv_.Get(), clearColor);
    context_->ClearDepthStencilView(dsv_.Get(), D3D11_CLEAR_DEPTH | D3D11_CLEAR_STENCIL, 1.0F, 0);
    ID3D11RenderTargetView *rtv = rtv_.Get();
    context_->OMSetRenderTargets(1, &rtv, dsv_.Get());
    D3D11_VIEWPORT viewport{0.0F, 0.0F, static_cast<float>(width_), static_cast<float>(height_), 0.0F, 1.0F};
    context_->RSSetViewports(1, &viewport);
}

void D3DContext::sampleProbe(std::optional<POINT> probe) {
    // Read back the oldest probe (written probeRing-1 samples ago) to avoid stalls.
    const std::size_t readIndex = (probeIndex_ + 1) % probeRing;
    if (probePending_[readIndex]) {
        D3D11_MAPPED_SUBRESOURCE mapped{};
        if (SUCCEEDED(context_->Map(probeStaging_[readIndex].Get(), 0, D3D11_MAP_READ, 0, &mapped))) {
            probedAlpha_ = static_cast<const std::uint8_t *>(mapped.pData)[3]; // BGRA
            probedPoint_ = *probePending_[readIndex];
            context_->Unmap(probeStaging_[readIndex].Get(), 0);
        }
        probePending_[readIndex].reset();
    }
    if (probe && probe->x >= 0 && probe->y >= 0 && static_cast<UINT>(probe->x) < width_ &&
        static_cast<UINT>(probe->y) < height_) {
        const D3D11_BOX box{static_cast<UINT>(probe->x), static_cast<UINT>(probe->y), 0,
                            static_cast<UINT>(probe->x) + 1, static_cast<UINT>(probe->y) + 1, 1};
        context_->CopySubresourceRegion(probeStaging_[probeIndex_].Get(), 0, 0, 0, 0, backBuffer_.Get(), 0, &box);
        probePending_[probeIndex_] = probe;
    }
    probeIndex_ = (probeIndex_ + 1) % probeRing;
}

void D3DContext::probeOnly(std::optional<POINT> probe) {
    // With a flip-model swap chain the current back buffer holds an earlier
    // presented image; while idle every buffer contains the same picture.
    sampleProbe(probe);
}

bool D3DContext::endFrame(std::optional<POINT> probe, UINT syncInterval) {
    if (msaaSamples_ > 1)
        context_->ResolveSubresource(backBuffer_.Get(), 0, msaaColor_.Get(), 0, colorFormat);
    sampleProbe(probe);

    if (!capturePath_.empty()) {
        try {
            captureBackBuffer(capturePath_);
            log::info("フレームを保存しました: {}", pathToUtf8(capturePath_));
        } catch (const std::exception &e) {
            log::error("フレーム保存に失敗しました: {}", e.what());
        }
        capturePath_.clear();
    }

    const HRESULT hr = swapChain_->Present(syncInterval, 0);
    if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET) {
        throw D3DError("GPUデバイスが失われました", device_->GetDeviceRemovedReason());
    }
    throwIfFailed(hr, "Present");
    return hr != DXGI_STATUS_OCCLUDED;
}

void D3DContext::captureBackBuffer(const std::filesystem::path &path) {
    D3D11_TEXTURE2D_DESC desc{};
    backBuffer_->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ComPtr<ID3D11Texture2D> staging;
    throwIfFailed(device_->CreateTexture2D(&desc, nullptr, &staging), "CreateTexture2D(capture)");
    context_->CopyResource(staging.Get(), backBuffer_.Get());

    std::vector<std::uint8_t> pixels(static_cast<std::size_t>(desc.Width) * desc.Height * 4);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    throwIfFailed(context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), "Map(capture)");
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto *src = static_cast<const std::uint8_t *>(mapped.pData) + static_cast<std::size_t>(y) * mapped.RowPitch;
        auto *dst = pixels.data() + static_cast<std::size_t>(y) * desc.Width * 4;
        for (UINT x = 0; x < desc.Width; ++x) {
            const std::uint8_t a = transparent_ ? src[x * 4 + 3] : 255;
            for (int c = 0; c < 3; ++c) {
                const unsigned value = src[x * 4 + c];
                // Un-premultiply (transparent mode renders premultiplied alpha).
                dst[x * 4 + c] = static_cast<std::uint8_t>(
                    !transparent_ ? value : (a == 0 ? 0 : std::min(255u, (value * 255u + a / 2) / a)));
            }
            dst[x * 4 + 3] = a;
        }
    }
    context_->Unmap(staging.Get(), 0);

    ComPtr<IWICImagingFactory> factory;
    throwIfFailed(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
                  "WICImagingFactory");
    ComPtr<IWICStream> stream;
    throwIfFailed(factory->CreateStream(&stream), "CreateStream");
    throwIfFailed(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "InitializeFromFilename");
    ComPtr<IWICBitmapEncoder> encoder;
    throwIfFailed(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "CreateEncoder");
    throwIfFailed(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Encoder::Initialize");
    ComPtr<IWICBitmapFrameEncode> frame;
    throwIfFailed(encoder->CreateNewFrame(&frame, nullptr), "CreateNewFrame");
    throwIfFailed(frame->Initialize(nullptr), "Frame::Initialize");
    throwIfFailed(frame->SetSize(desc.Width, desc.Height), "SetSize");
    WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
    throwIfFailed(frame->SetPixelFormat(&format), "SetPixelFormat");
    if (format != GUID_WICPixelFormat32bppBGRA)
        throw std::runtime_error("PNG encoder does not accept 32bpp BGRA");
    throwIfFailed(frame->WritePixels(desc.Height, desc.Width * 4, static_cast<UINT>(pixels.size()), pixels.data()),
                  "WritePixels");
    throwIfFailed(frame->Commit(), "Frame::Commit");
    throwIfFailed(encoder->Commit(), "Encoder::Commit");
}

} // namespace uka3d
