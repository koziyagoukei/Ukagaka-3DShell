#pragma once

#include "renderer/Camera.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string_view>
#include <vector>

namespace uka3d {

struct PetSettings {
    float distance{56.0F};
    float minSegment{4.0F};
    int windowMs{850};
    int turns{2};
    int cooldownMs{1500};
};

struct InteractionProfile {
    struct Sphere {
        std::string name;
        std::string bone;
        mmd::Float3 offset{};
        float radius{};
        int priority{};
    };
    PetSettings pet;
    bool mouseMove{true};
    std::vector<Sphere> spheres;
    static InteractionProfile load(const std::filesystem::path &path);
};

// Bound to the currently loaded PMX. Other hit shape implementations can return
// a name, priority and ray distance using the same selection rule.
class HitRegions {
  public:
    void bind(const InteractionProfile &profile, const MmdModel &model);
    [[nodiscard]] std::string_view pick(const CameraRay &ray, const MmdModel &model) const;

  private:
    struct BoundSphere {
        InteractionProfile::Sphere sphere;
        std::size_t bone;
    };
    std::vector<BoundSphere> spheres_;
};

enum class MouseEventKind { click, doubleClick, move, pet, dragStart, dragEnd };

struct PointerSample {
    long x{};
    long y{};
    float screenX{}; // Used for movement, so moving a window under a still pointer cannot pet it.
    float screenY{};
    std::string region;
    float dpiScale{1.0F};
    bool overModel{false};
};

class InteractionInput {
  public:
    using Emit = std::function<void(MouseEventKind, const PointerSample &)>;
    InteractionInput(PetSettings settings, bool mouseMove, std::uint64_t doubleClickMs, Emit emit);

    void press(PointerSample sample, std::uint64_t now, bool osDoubleClick);
    void release(const PointerSample &sample, std::uint64_t now, bool held);
    void move(PointerSample sample, std::uint64_t now, bool buttonsDown);
    void startDrag(std::uint64_t now);
    void endDrag(PointerSample sample);
    void cancelPress();
    void leave();
    void tick(std::uint64_t now);

  private:
    void flushClick();
    void resetStroke(PointerSample sample, std::uint64_t now);

    struct PendingClick {
        PointerSample sample;
        std::uint64_t deadline;
    };
    PetSettings settings_;
    bool mouseMove_;
    std::uint64_t doubleClickMs_;
    Emit emit_;
    std::optional<PointerSample> pressed_;
    std::optional<PendingClick> pending_;
    bool secondClick_{false};
    bool dragging_{false};
    std::uint64_t pressedAt_{};
    std::optional<PointerSample> lastMove_;
    std::optional<PointerSample> stroke_;
    std::uint64_t strokeStart_{};
    std::uint64_t nextPet_{};
    std::uint64_t nextMove_{};
    float lastDx_{};
    float lastDy_{};
    float distance_{};
    int turns_{};
};

} // namespace uka3d
