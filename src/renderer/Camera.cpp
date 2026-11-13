#include "renderer/Camera.h"

#include <algorithm>
#include <cmath>

using namespace DirectX;

namespace uka3d {

void Camera::fitToBounds(const Bounds &bounds, float aspect, float margin, bool heightOnly) {
    if (!bounds.valid) {
        target = {0.0F, 10.0F, 0.0F};
        distance = 45.0F;
        return;
    }
    const float hx = 0.5F * (bounds.maximum[0] - bounds.minimum[0]);
    const float hy = 0.5F * (bounds.maximum[1] - bounds.minimum[1]);
    const float hz = 0.5F * (bounds.maximum[2] - bounds.minimum[2]);
    target = {bounds.minimum[0] + hx, bounds.minimum[1] + hy, bounds.minimum[2] + hz};

    const float tanHalf = std::tan(fovY * 0.5F);
    const float fitVertical = hy / tanHalf;
    const float fitHorizontal = hx / (tanHalf * std::max(aspect, 0.01F));
    // Distance measured from the front face of the bounding box.
    // heightOnly: frame the character's height (wide T-pose arms / cloaks may be cut).
    distance = std::max({fitVertical, heightOnly ? 0.0F : fitHorizontal, 0.1F}) * margin + hz;

    const float radius = std::sqrt(hx * hx + hy * hy + hz * hz);
    nearZ = std::max(0.01F, (distance - radius) * 0.1F);
    farZ = distance + radius * 4.0F + 100.0F;
}

void Camera::zoom(float factor) {
    distance = std::clamp(distance * factor, 0.5F, 5000.0F);
    farZ = std::max(farZ, distance * 4.0F);
}

XMFLOAT3 Camera::eye() const {
    // yaw=0, pitch=0 -> camera on the -Z side looking toward +Z.
    const float cp = std::cos(pitch);
    const XMFLOAT3 offset{-std::sin(yaw) * cp * distance, std::sin(pitch) * distance, -std::cos(yaw) * cp * distance};
    return {target.x + offset.x, target.y + offset.y, target.z + offset.z};
}

XMMATRIX Camera::view() const {
    const XMFLOAT3 e = eye();
    return XMMatrixLookAtLH(XMLoadFloat3(&e), XMLoadFloat3(&target), XMVectorSet(0.0F, 1.0F, 0.0F, 0.0F));
}

XMMATRIX Camera::projection(float aspect) const {
    XMMATRIX matrix = XMMatrixPerspectiveFovLH(fovY, std::max(aspect, 0.01F), nearZ, farZ);
    if (ndcOffsetX != 0.0F || ndcOffsetY != 0.0F) {
        // x_clip += offset * w_clip (row-vector convention: the w row feeds x and y).
        XMMATRIX shift = XMMatrixIdentity();
        shift.r[3] = XMVectorSet(ndcOffsetX, ndcOffsetY, 0.0F, 1.0F);
        matrix = XMMatrixMultiply(matrix, shift);
    }
    return matrix;
}

XMFLOAT2 Camera::project(const mmd::Float3 &position, float width, float height) const {
    const auto point = XMVector3Project(XMVectorSet(position[0], position[1], position[2], 1.0F),
                                       0.0F, 0.0F, width, height, 0.0F, 1.0F,
                                       projection(width / height), view(), XMMatrixIdentity());
    return {XMVectorGetX(point), XMVectorGetY(point)};
}

CameraRay Camera::ray(float x, float y, float width, float height) const {
    const auto proj = projection(width / height);
    const auto viewMatrix = view();
    const auto unproject = [&](float z) {
        return XMVector3Unproject(XMVectorSet(x, y, z, 1.0F), 0.0F, 0.0F, width, height, 0.0F, 1.0F,
                                  proj, viewMatrix, XMMatrixIdentity());
    };
    const auto nearPoint = unproject(0.0F);
    const auto segment = XMVectorSubtract(unproject(1.0F), nearPoint);
    CameraRay result{};
    XMStoreFloat3(&result.origin, nearPoint);
    XMStoreFloat3(&result.direction, XMVector3Normalize(segment));
    result.length = XMVectorGetX(XMVector3Length(segment));
    return result;
}

} // namespace uka3d
