#pragma once

#include "mmd/MmdModel.h"

#include <DirectXMath.h>

namespace uka3d {

struct CameraRay {
    DirectX::XMFLOAT3 origin;
    DirectX::XMFLOAT3 direction;
    float length;
};

// Orbit camera in MMD's left-handed space (+Y up, model faces -Z toward the viewer).
class Camera {
  public:
    // Places the camera in front of the model so that the whole bounding box fits.
    void fitToBounds(const Bounds &bounds, float aspect, float margin = 1.08F, bool heightOnly = false);
    void zoom(float factor);

    [[nodiscard]] DirectX::XMFLOAT3 eye() const;
    [[nodiscard]] DirectX::XMMATRIX view() const;
    [[nodiscard]] DirectX::XMMATRIX projection(float aspect) const;
    [[nodiscard]] DirectX::XMFLOAT2 project(const mmd::Float3 &position, float width, float height) const;
    [[nodiscard]] CameraRay ray(float x, float y, float width, float height) const;

    DirectX::XMFLOAT3 target{0.0F, 10.0F, 0.0F};
    float distance{45.0F};
    float fovY{DirectX::XMConvertToRadians(30.0F)};
    float yaw{0.0F};
    float pitch{0.0F};
    float roll{0.0F}; // around the view direction (+ = the image turns counter-clockwise)
    // Shifts the rendered image without moving the camera, in normalised device
    // units (+x right, +y up; 2 = full width/height). Applied by projection(),
    // so project() and ray() stay consistent with what is drawn.
    float ndcOffsetX{0.0F};
    float ndcOffsetY{0.0F};
    float nearZ{0.5F};
    float farZ{1000.0F};
};

} // namespace uka3d
