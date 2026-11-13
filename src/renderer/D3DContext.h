#pragma once

#include <windows.h>

#include <d3d11_1.h>
#include <dcomp.h>
#include <dxgi1_2.h>
#include <wrl/client.h>

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <stdexcept>
#include <string>

namespace uka3d {

using Microsoft::WRL::ComPtr;

class D3DError : public std::runtime_error {
  public:
    D3DError(const std::string &what, HRESULT hr);
    [[nodiscard]] HRESULT hr() const noexcept {
        return hr_;
    }

  private:
    HRESULT hr_;
};

void throwIfFailed(HRESULT hr, const char *what);

// Owns the D3D11 device, the swap chain and the render targets.
//
// Transparent mode uses a composition swap chain (premultiplied alpha) bound to
// the window through DirectComposition, so the desktop shows through wherever
// the rendered alpha is zero. Windowed mode uses an ordinary HWND swap chain.
class D3DContext {
  public:
    struct Config {
        HWND hwnd{};
        bool transparent{true};
        UINT width{};
        UINT height{};
        UINT msaaSamples{4};
        bool debugLayer{false};
    };

    explicit D3DContext(const Config &config);
    ~D3DContext();
    D3DContext(const D3DContext &) = delete;
    D3DContext &operator=(const D3DContext &) = delete;

    void resize(UINT width, UINT height);
    void beginFrame(const float clearColor[4]);
    // Resolves MSAA, samples the alpha under `probe` (client coords) for
    // click-through hit testing, and presents with the given sync interval
    // (0 = no VSync wait). Returns false when the output is occluded.
    bool endFrame(std::optional<POINT> probe, UINT syncInterval);
    // Samples the alpha under `probe` from the last frame without rendering
    // (used while idle so click-through keeps working).
    void probeOnly(std::optional<POINT> probe);

    // Saves the next presented frame as a PNG (straight alpha) inside endFrame().
    void requestCapture(std::filesystem::path path) {
        capturePath_ = std::move(path);
    }

    // Alpha (0-255) of the most recently completed probe, if any.
    [[nodiscard]] std::optional<std::uint8_t> probedAlpha() const noexcept {
        return probedAlpha_;
    }
    [[nodiscard]] std::optional<std::uint8_t> alphaAt(POINT point) const noexcept {
        if (point.x != probedPoint_.x || point.y != probedPoint_.y)
            return std::nullopt;
        return probedAlpha_;
    }

    [[nodiscard]] ID3D11Device *device() const noexcept {
        return device_.Get();
    }
    [[nodiscard]] ID3D11DeviceContext *context() const noexcept {
        return context_.Get();
    }
    [[nodiscard]] UINT width() const noexcept {
        return width_;
    }
    [[nodiscard]] UINT height() const noexcept {
        return height_;
    }
    [[nodiscard]] bool transparent() const noexcept {
        return transparent_;
    }

  private:
    void createDevice(bool debugLayer);
    void createSwapChain(HWND hwnd);
    void createTargets();
    void releaseTargets();
    void captureBackBuffer(const std::filesystem::path &path);
    void sampleProbe(std::optional<POINT> probe);

    bool transparent_;
    UINT width_;
    UINT height_;
    UINT msaaSamples_;

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<IDXGISwapChain1> swapChain_;

    ComPtr<IDCompositionDevice> dcompDevice_;
    ComPtr<IDCompositionTarget> dcompTarget_;
    ComPtr<IDCompositionVisual> dcompVisual_;

    ComPtr<ID3D11Texture2D> backBuffer_;
    ComPtr<ID3D11Texture2D> msaaColor_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    ComPtr<ID3D11Texture2D> depth_;
    ComPtr<ID3D11DepthStencilView> dsv_;

    static constexpr std::size_t probeRing = 3;
    std::array<ComPtr<ID3D11Texture2D>, probeRing> probeStaging_;
    std::array<std::optional<POINT>, probeRing> probePending_{};
    std::size_t probeIndex_{0};
    std::optional<std::uint8_t> probedAlpha_;
    POINT probedPoint_{};
    std::filesystem::path capturePath_;

    static constexpr DXGI_FORMAT colorFormat = DXGI_FORMAT_B8G8R8A8_UNORM;
};

} // namespace uka3d
