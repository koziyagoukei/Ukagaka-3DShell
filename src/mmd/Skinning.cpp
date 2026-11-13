#include "mmd/Skinning.h"

#include <algorithm>
#include <cmath>
#include <cstring>

// The SDEF / QDEF formulas and the quaternion conventions follow libmmd
// (MIT License, see THIRD_PARTY_NOTICES.md) so that both skinning paths
// produce the same deformation.

namespace uka3d {
namespace {

using mmd::Float3;
using mmd::Float4;

inline Float3 add(const Float3 &a, const Float3 &b) {
    return {a[0] + b[0], a[1] + b[1], a[2] + b[2]};
}
inline Float3 sub(const Float3 &a, const Float3 &b) {
    return {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
}
inline Float3 mul(const Float3 &a, float s) {
    return {a[0] * s, a[1] * s, a[2] * s};
}
inline float dot(const Float3 &a, const Float3 &b) {
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2];
}
inline Float3 cross(const Float3 &a, const Float3 &b) {
    return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
}
inline bool finite(const Float3 &v) {
    return std::isfinite(v[0]) && std::isfinite(v[1]) && std::isfinite(v[2]);
}
inline Float3 normalized(const Float3 &v) {
    const float l = std::sqrt(dot(v, v));
    return l > 1e-8F ? mul(v, 1.0F / l) : Float3{};
}
inline Float3 rotate(const Float4 &q, const Float3 &v) {
    const Float3 u{q[0], q[1], q[2]};
    return add(add(mul(u, 2.0F * dot(u, v)), mul(v, q[3] * q[3] - dot(u, u))), mul(cross(u, v), 2.0F * q[3]));
}
inline Float4 conjugate(const Float4 &q) {
    return {-q[0], -q[1], -q[2], q[3]};
}
inline Float4 multiply(const Float4 &a, const Float4 &b) {
    return {
        a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1],
        a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0],
        a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3],
        a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2],
    };
}
inline Float4 normalizeQuat(Float4 q) {
    const float l = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
    if (!(l > 1e-12F))
        return {0.0F, 0.0F, 0.0F, 1.0F};
    for (auto &c : q)
        c /= l;
    return q;
}
Float4 slerp(Float4 a, Float4 b, float t) {
    float d = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    if (d < 0.0F) {
        for (auto &value : b)
            value = -value;
        d = -d;
    }
    if (d > 0.9995F) {
        for (std::size_t i = 0; i < 4; ++i)
            a[i] += (b[i] - a[i]) * t;
        return normalizeQuat(a);
    }
    const float angle = std::acos(std::clamp(d, -1.0F, 1.0F));
    const float denominator = std::sin(angle);
    const float aWeight = std::sin((1.0F - t) * angle) / denominator;
    const float bWeight = std::sin(t * angle) / denominator;
    for (std::size_t i = 0; i < 4; ++i)
        a[i] = a[i] * aWeight + b[i] * bWeight;
    return normalizeQuat(a);
}

} // namespace

Float3 transformPoint(const BoneTransform &bone, const Float3 &bindPoint) {
    return add(rotate(bone.rotation, bindPoint), bone.translation);
}

Float3 jointPosition(const mmd::PmxModel &model, std::span<const BoneTransform> bones, std::size_t index) {
    const auto &bind = model.bones[index].position;
    return index < bones.size() ? transformPoint(bones[index], bind) : bind;
}

// ---------------------------------------------------------------------------
// Skinner
// ---------------------------------------------------------------------------

Skinner::Skinner(const mmd::PmxModel &model) : model_(model) {
    const std::size_t count = model.vertices.size();
    const std::size_t boneCount = model.bones.size();
    position_.resize(count);
    normal_.resize(count);
    influenceCount_.assign(count, 0);
    influenceBone_.assign(count * 4, 0);
    influenceWeight_.assign(count * 4, 0.0F);
    dirtyFlag_.assign(count, 0);
    appliedWeights_.assign(model.morphs.size(), 0.0F);
    matrices_.resize(boneCount);

    for (std::size_t v = 0; v < count; ++v) {
        const auto &vertex = model.vertices[v];
        position_[v] = vertex.position;
        normal_[v] = vertex.normal;

        // Same influence rules as libmmd's linear path.
        const std::size_t influences =
            vertex.weightType == mmd::PmxWeightType::bdef1
                ? 1
                : (vertex.weightType == mmd::PmxWeightType::bdef2 || vertex.weightType == mmd::PmxWeightType::sdef ? 2
                                                                                                                 : 4);
        float total = 0.0F;
        std::uint8_t used = 0;
        for (std::size_t i = 0; i < influences; ++i) {
            const auto bone = vertex.bones[i];
            const float weight = vertex.weights[i];
            if (bone < 0 || static_cast<std::size_t>(bone) >= boneCount || !std::isfinite(weight) || weight <= 0.0F)
                continue;
            influenceBone_[v * 4 + used] = static_cast<std::uint32_t>(bone);
            influenceWeight_[v * 4 + used] = weight;
            total += weight;
            ++used;
        }
        if (std::isfinite(total) && total > 1e-6F) {
            for (std::uint8_t i = 0; i < used; ++i)
                influenceWeight_[v * 4 + i] /= total;
            influenceCount_[v] = used;
        }

        if (vertex.weightType == mmd::PmxWeightType::sdef) {
            const auto first = vertex.bones[0];
            const auto second = vertex.bones[1];
            if (first >= 0 && second >= 0 && static_cast<std::size_t>(first) < boneCount &&
                static_cast<std::size_t>(second) < boneCount && std::isfinite(vertex.weights[0]) &&
                finite(vertex.sdefC) && finite(vertex.sdefR0) && finite(vertex.sdefR1)) {
                Sdef sdef;
                sdef.vertex = static_cast<std::uint32_t>(v);
                sdef.bone0 = static_cast<std::uint32_t>(first);
                sdef.bone1 = static_cast<std::uint32_t>(second);
                sdef.weight = std::clamp(vertex.weights[0], 0.0F, 1.0F);
                const auto halfDelta = mul(sub(vertex.sdefR0, vertex.sdefR1), 0.5F);
                sdef.c = vertex.sdefC;
                sdef.cr0 = add(vertex.sdefC, mul(halfDelta, 1.0F - sdef.weight));
                sdef.cr1 = sub(vertex.sdefC, mul(halfDelta, sdef.weight));
                sdef_.push_back(sdef);
                influenceCount_[v] |= 0x80; // handled by the SDEF pass
            }
        } else if (vertex.weightType == mmd::PmxWeightType::qdef) {
            qdef_.push_back({static_cast<std::uint32_t>(v)});
            influenceCount_[v] |= 0x80; // handled by the QDEF pass
        }
    }

    for (std::size_t m = 0; m < model.morphs.size(); ++m) {
        const auto &morph = model.morphs[m];
        if (morph.type != 1 || morph.offsets.empty())
            continue;
        // libmmd ignores a morph entirely when any of its offsets is not finite.
        if (std::ranges::all_of(morph.offsets, [](const auto &offset) { return finite(offset.vector3); }))
            vertexMorphs_.push_back(static_cast<std::uint32_t>(m));
    }
}

void Skinner::applyMorphs(std::span<const float> morphWeights) {
    const auto weightOf = [&](std::uint32_t morph) {
        const float w = morph < morphWeights.size() ? morphWeights[morph] : 0.0F;
        return std::isfinite(w) && std::abs(w) >= 1e-8F ? w : 0.0F;
    };
    bool changed = false;
    for (const auto morph : vertexMorphs_) {
        if (weightOf(morph) != appliedWeights_[morph]) {
            changed = true;
            break;
        }
    }
    if (!changed)
        return;

    // Rebuild from the bind pose (no incremental deltas, so no drift over time).
    for (const auto v : dirtyVertices_) {
        position_[v] = model_.vertices[v].position;
        dirtyFlag_[v] = 0;
    }
    dirtyVertices_.clear();
    for (const auto morph : vertexMorphs_) {
        const float weight = weightOf(morph);
        appliedWeights_[morph] = weight;
        if (weight == 0.0F)
            continue;
        for (const auto &offset : model_.morphs[morph].offsets) {
            if (offset.index < 0 || static_cast<std::size_t>(offset.index) >= position_.size())
                continue;
            const auto v = static_cast<std::uint32_t>(offset.index);
            position_[v] = add(position_[v], mul(offset.vector3, weight));
            if (!dirtyFlag_[v]) {
                dirtyFlag_[v] = 1;
                dirtyVertices_.push_back(v);
            }
        }
    }
}

inline void Skinner::skinLinear(std::size_t v, SkinnedVertex &out) const {
    const std::uint8_t count = influenceCount_[v] & 0x7F;
    const float *p = position_[v].data();
    const float *n = normal_[v].data();
    if (count == 0) {
        std::memcpy(out.position, p, sizeof(out.position));
        std::memcpy(out.normal, n, sizeof(out.normal));
        return;
    }
    const std::uint32_t *bone = &influenceBone_[v * 4];
    const float *m;
    float blended[12];
    if (count == 1) {
        m = matrices_[bone[0]].m;
    } else {
        const float *weight = &influenceWeight_[v * 4];
        const float *a = matrices_[bone[0]].m;
        const float *b = matrices_[bone[1]].m;
        for (int i = 0; i < 12; ++i)
            blended[i] = a[i] * weight[0] + b[i] * weight[1];
        for (std::uint8_t k = 2; k < count; ++k) {
            const float *c = matrices_[bone[k]].m;
            for (int i = 0; i < 12; ++i)
                blended[i] += c[i] * weight[k];
        }
        m = blended;
    }
    out.position[0] = m[0] * p[0] + m[1] * p[1] + m[2] * p[2] + m[3];
    out.position[1] = m[4] * p[0] + m[5] * p[1] + m[6] * p[2] + m[7];
    out.position[2] = m[8] * p[0] + m[9] * p[1] + m[10] * p[2] + m[11];
    const float nx = m[0] * n[0] + m[1] * n[1] + m[2] * n[2];
    const float ny = m[4] * n[0] + m[5] * n[1] + m[6] * n[2];
    const float nz = m[8] * n[0] + m[9] * n[1] + m[10] * n[2];
    const float length = std::sqrt(nx * nx + ny * ny + nz * nz);
    const float scale = length > 1e-8F ? 1.0F / length : 0.0F;
    out.normal[0] = nx * scale;
    out.normal[1] = ny * scale;
    out.normal[2] = nz * scale;
}

void Skinner::skin(std::span<const BoneTransform> bones, std::span<const float> morphWeights,
                   std::span<SkinnedVertex> out) {
    const std::size_t count = std::min(out.size(), position_.size());
    applyMorphs(morphWeights);

    // Bone matrices: R = (w^2 - u.u) I + 2 u u^T + 2 w [u]x  (same as libmmd's rotate()).
    for (std::size_t b = 0; b < matrices_.size(); ++b) {
        float *m = matrices_[b].m;
        if (b >= bones.size()) {
            const float identity[12] = {1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0};
            std::memcpy(m, identity, sizeof(identity));
            continue;
        }
        const auto &q = bones[b].rotation;
        const auto &t = bones[b].translation;
        const float x = q[0], y = q[1], z = q[2], w = q[3];
        const float s = w * w - (x * x + y * y + z * z);
        m[0] = s + 2.0F * x * x;
        m[1] = 2.0F * (x * y - w * z);
        m[2] = 2.0F * (x * z + w * y);
        m[3] = t[0];
        m[4] = 2.0F * (x * y + w * z);
        m[5] = s + 2.0F * y * y;
        m[6] = 2.0F * (y * z - w * x);
        m[7] = t[1];
        m[8] = 2.0F * (x * z - w * y);
        m[9] = 2.0F * (y * z + w * x);
        m[10] = s + 2.0F * z * z;
        m[11] = t[2];
    }

    for (std::size_t v = 0; v < count; ++v) {
        if (influenceCount_[v] & 0x80)
            continue; // SDEF / QDEF below
        skinLinear(v, out[v]);
    }

    for (const auto &sdef : sdef_) {
        if (sdef.vertex >= count)
            continue;
        bool done = false;
        if (sdef.bone0 < bones.size() && sdef.bone1 < bones.size()) {
            const auto &first = bones[sdef.bone0];
            const auto &second = bones[sdef.bone1];
            const auto rotation = slerp(second.rotation, first.rotation, sdef.weight);
            const auto translated0 = transformPoint(first, sdef.cr0);
            const auto translated1 = transformPoint(second, sdef.cr1);
            const auto position = add(rotate(rotation, sub(position_[sdef.vertex], sdef.c)),
                                      add(mul(translated0, sdef.weight), mul(translated1, 1.0F - sdef.weight)));
            const auto normal = normalized(rotate(rotation, normal_[sdef.vertex]));
            if (finite(position) && finite(normal)) {
                std::memcpy(out[sdef.vertex].position, position.data(), sizeof(float) * 3);
                std::memcpy(out[sdef.vertex].normal, normal.data(), sizeof(float) * 3);
                done = true;
            }
        }
        if (!done)
            skinLinear(sdef.vertex, out[sdef.vertex]);
    }

    for (const auto &entry : qdef_) {
        if (entry.vertex >= count)
            continue;
        const auto &vertex = model_.vertices[entry.vertex];
        Float4 real{};
        Float4 dual{};
        Float4 pivot{};
        bool initialized = false;
        for (std::size_t influence = 0; influence < 4; ++influence) {
            const auto bone = vertex.bones[influence];
            float weight = vertex.weights[influence];
            if (bone < 0 || static_cast<std::size_t>(bone) >= bones.size() || !std::isfinite(weight) || weight <= 0.0F)
                continue;
            const auto &rotation = bones[static_cast<std::size_t>(bone)].rotation;
            const auto &translation = bones[static_cast<std::size_t>(bone)].translation;
            auto dualPart = multiply({translation[0], translation[1], translation[2], 0.0F}, rotation);
            for (auto &component : dualPart)
                component *= 0.5F;
            if (!initialized) {
                pivot = rotation;
                initialized = true;
            } else if (pivot[0] * rotation[0] + pivot[1] * rotation[1] + pivot[2] * rotation[2] +
                           pivot[3] * rotation[3] <
                       0.0F) {
                weight = -weight;
            }
            for (std::size_t component = 0; component < 4; ++component) {
                real[component] += rotation[component] * weight;
                dual[component] += dualPart[component] * weight;
            }
        }
        const float magnitude =
            std::sqrt(real[0] * real[0] + real[1] * real[1] + real[2] * real[2] + real[3] * real[3]);
        bool done = false;
        if (initialized && std::isfinite(magnitude) && magnitude > 1e-8F) {
            for (std::size_t component = 0; component < 4; ++component) {
                real[component] /= magnitude;
                dual[component] /= magnitude;
            }
            const float projection = real[0] * dual[0] + real[1] * dual[1] + real[2] * dual[2] + real[3] * dual[3];
            for (std::size_t component = 0; component < 4; ++component)
                dual[component] -= real[component] * projection;
            const auto translation = multiply(dual, conjugate(real));
            const auto position = add(rotate(real, position_[entry.vertex]),
                                      {2.0F * translation[0], 2.0F * translation[1], 2.0F * translation[2]});
            const auto normal = normalized(rotate(real, normal_[entry.vertex]));
            if (finite(position) && finite(normal)) {
                std::memcpy(out[entry.vertex].position, position.data(), sizeof(float) * 3);
                std::memcpy(out[entry.vertex].normal, normal.data(), sizeof(float) * 3);
                done = true;
            }
        }
        if (!done)
            skinLinear(entry.vertex, out[entry.vertex]);
    }
}

// ---------------------------------------------------------------------------
// Pose blending
// ---------------------------------------------------------------------------

PoseBlender::PoseBlender(const mmd::PmxModel &model) : model_(model) {
    const std::size_t count = model.bones.size();
    order_.reserve(count);
    std::vector<std::uint8_t> state(count, 0); // 0 = new, 1 = on stack, 2 = done
    std::vector<std::uint32_t> stack;
    for (std::size_t start = 0; start < count; ++start) {
        if (state[start] == 2)
            continue;
        stack.push_back(static_cast<std::uint32_t>(start));
        while (!stack.empty()) {
            const auto bone = stack.back();
            const auto parent = model.bones[bone].parent;
            const bool parentPending = parent >= 0 && static_cast<std::size_t>(parent) < count &&
                                       state[static_cast<std::size_t>(parent)] == 0;
            if (state[bone] == 0 && parentPending) {
                state[bone] = 1;
                stack.push_back(static_cast<std::uint32_t>(parent));
                continue;
            }
            // Parent done, missing, or part of a cycle (state 1): emit this bone.
            if (state[bone] != 2) {
                state[bone] = 2;
                order_.push_back(bone);
            }
            stack.pop_back();
        }
    }
}

void PoseBlender::blend(std::span<const BoneTransform> from, std::span<const BoneTransform> to, float weight,
                        std::vector<BoneTransform> &out) const {
    const std::size_t count = model_.bones.size();
    out.resize(count);
    if (from.size() < count || to.size() < count) {
        if (to.size() >= count)
            std::copy_n(to.begin(), count, out.begin());
        return;
    }
    weight = std::clamp(weight, 0.0F, 1.0F);
    // Joint positions of the three poses. `emitted` marks parents already blended.
    std::vector<Float3> fromPos(count), toPos(count), outPos(count);
    std::vector<std::uint8_t> emitted(count, 0);
    for (std::size_t i = 0; i < count; ++i) {
        fromPos[i] = transformPoint(from[i], model_.bones[i].position);
        toPos[i] = transformPoint(to[i], model_.bones[i].position);
    }
    for (const auto bone : order_) {
        const auto parent = model_.bones[bone].parent;
        Float4 rotation;
        Float3 position;
        if (parent >= 0 && static_cast<std::size_t>(parent) < count && emitted[static_cast<std::size_t>(parent)]) {
            const auto p = static_cast<std::size_t>(parent);
            const auto fromInverse = conjugate(from[p].rotation);
            const auto toInverse = conjugate(to[p].rotation);
            const auto localFrom = multiply(fromInverse, from[bone].rotation);
            const auto localTo = multiply(toInverse, to[bone].rotation);
            const auto offsetFrom = rotate(fromInverse, sub(fromPos[bone], fromPos[p]));
            const auto offsetTo = rotate(toInverse, sub(toPos[bone], toPos[p]));
            const auto local = slerp(localFrom, localTo, weight);
            const auto offset = add(mul(offsetFrom, 1.0F - weight), mul(offsetTo, weight));
            rotation = normalizeQuat(multiply(out[p].rotation, local));
            position = add(outPos[p], rotate(out[p].rotation, offset));
        } else {
            rotation = slerp(from[bone].rotation, to[bone].rotation, weight);
            position = add(mul(fromPos[bone], 1.0F - weight), mul(toPos[bone], weight));
        }
        out[bone].rotation = rotation;
        out[bone].translation = sub(position, rotate(rotation, model_.bones[bone].position));
        outPos[bone] = position;
        emitted[bone] = 1;
    }
}

void lerpBones(std::span<const BoneTransform> from, std::span<const BoneTransform> to, float weight,
               std::vector<BoneTransform> &out) {
    out.resize(to.size());
    if (from.size() != to.size()) {
        std::copy(to.begin(), to.end(), out.begin());
        return;
    }
    weight = std::clamp(weight, 0.0F, 1.0F);
    for (std::size_t i = 0; i < to.size(); ++i) {
        Float4 b = to[i].rotation;
        const auto &a = from[i].rotation;
        if (a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3] < 0.0F)
            for (auto &c : b)
                c = -c;
        Float4 q;
        for (std::size_t c = 0; c < 4; ++c)
            q[c] = a[c] + (b[c] - a[c]) * weight;
        out[i].rotation = normalizeQuat(q);
        for (std::size_t c = 0; c < 3; ++c)
            out[i].translation[c] = from[i].translation[c] + (to[i].translation[c] - from[i].translation[c]) * weight;
    }
}

} // namespace uka3d
