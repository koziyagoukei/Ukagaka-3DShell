#include "app/Accessories.h"

#include "ipc/PathPolicy.h"
#include "platform/Log.h"
#include "platform/Utf.h"

#include <algorithm>
#include <cmath>
#include <fstream>

using namespace DirectX;

namespace uka3d {
namespace {

struct AccessoryError : std::runtime_error {
    const char *code;
    AccessoryError(const char *c, const char *message) : std::runtime_error(message), code(c) {}
};

float number(const ipc::Json &value, float low, float high) {
    if (!value.is_number())
        throw AccessoryError(ipc::error::invalidParams, "変換値は数値で指定してください");
    const auto n = value.get<double>();
    if (!std::isfinite(n) || n < low || n > high)
        throw AccessoryError(ipc::error::invalidParams, "変換値が指定範囲外です");
    return static_cast<float>(n);
}

void updateTransform(AccessoryTransform &transform, const ipc::Json &json) {
    const auto readVector = [&](const char *key, mmd::Float3 &out, float limit) {
        const auto it = json.find(key);
        if (it == json.end())
            return;
        if (!it->is_array() || it->size() != 3)
            throw AccessoryError(ipc::error::invalidParams, "position / rotation は数値3個の配列です");
        for (std::size_t i = 0; i < out.size(); ++i)
            out[i] = number((*it)[i], -limit, limit);
    };
    readVector("position", transform.position, 10000.0F);
    readVector("rotation", transform.rotation, 36000.0F);
    if (const auto it = json.find("scale"); it != json.end())
        transform.scale = number(*it, 0.001F, 1000.0F);
}

ipc::Json readJson(const std::filesystem::path &path) {
    if (std::filesystem::file_size(path) > ipc::kMaxMessageBytes)
        throw std::runtime_error("アクセサリー設定が大きすぎます");
    std::ifstream file(path, std::ios::binary);
    return ipc::Json::parse(file, nullptr, true, true);
}

} // namespace

XMMATRIX AccessoryTransform::matrix() const {
    return XMMatrixScaling(scale, scale, scale) *
           XMMatrixRotationRollPitchYaw(XMConvertToRadians(rotation[0]), XMConvertToRadians(rotation[1]),
                                        XMConvertToRadians(rotation[2])) *
           XMMatrixTranslation(position[0], position[1], position[2]);
}

XMMATRIX attachmentMatrix(const mmd::PmxModel &model, std::size_t bone,
                          std::span<const BoneTransform> previous, std::span<const BoneTransform> current,
                          float alpha) {
    const auto toVector = [](const mmd::Float3 &v) { return XMVectorSet(v[0], v[1], v[2], 0.0F); };
    const auto toQuat = [](const BoneTransform &t) {
        return XMVectorSet(t.rotation[0], t.rotation[1], t.rotation[2], t.rotation[3]);
    };
    const auto position = toVector(jointPosition(model, current, bone));
    if (bone >= current.size())
        return XMMatrixTranslationFromVector(position);
    auto rotation = toQuat(current[bone]);
    if (bone >= previous.size() || alpha >= 1.0F)
        return XMMatrixRotationQuaternion(rotation) * XMMatrixTranslationFromVector(position);
    rotation = XMQuaternionSlerp(toQuat(previous[bone]), rotation, alpha);
    const auto joint = XMVectorLerp(toVector(jointPosition(model, previous, bone)), position, alpha);
    return XMMatrixRotationQuaternion(rotation) * XMMatrixTranslationFromVector(joint);
}

Accessories::Accessories(D3DContext &d3d, ModelRenderer::SharedAssets &assets, const MmdModel &main,
                         std::filesystem::path base, std::vector<std::filesystem::path> roots)
    : d3d_(d3d), assets_(assets), main_(&main), mainBounds_(main.bindPoseBounds()),
      base_(std::move(base)), roots_(std::move(roots)) {}

std::filesystem::path Accessories::assetPath(const ipc::Json &value) const {
    const auto path = ipc::resolveWithinRoots(utf8ToPath(value.get<std::string>()), base_, roots_);
    if (!path)
        throw AccessoryError(ipc::error::forbiddenPath, "許可されたフォルダの外は指定できません");
    std::error_code ec;
    if (!std::filesystem::is_regular_file(*path, ec))
        throw AccessoryError(ipc::error::notFound, "アクセサリーのファイルが見つかりません");
    return *path;
}

void Accessories::bind(Entry &entry, const std::string &parent, const std::string &bone) {
    const MmdModel *parentModel = nullptr;
    if (parent == "main") {
        parentModel = main_;
    } else if (!parent.empty()) {
        auto next = parent;
        while (!next.empty() && next != "main") {
            if (next == entry.id)
                throw AccessoryError(ipc::error::invalidParams, "親子関係を循環させることはできません");
            const auto it = entries_.find(next);
            if (it == entries_.end())
                throw AccessoryError(ipc::error::notFound, "親モデルが見つかりません");
            if (next == parent)
                parentModel = it->second.model.get();
            next = it->second.parent;
        }
    }
    if (!bone.empty() && !parentModel)
        throw AccessoryError(ipc::error::invalidParams, "parentBone には親モデルが必要です");
    const auto index = bone.empty() ? std::nullopt : parentModel->findBone(bone);
    if (!bone.empty() && !index)
        throw AccessoryError(ipc::error::notFound, "装着先のボーンが見つかりません");
    entry.parent = parent;
    entry.bone = bone;
    entry.boneIndex = index;
    entry.attached = true;
}

void Accessories::load(const ipc::Json &params) {
    const auto id = params.at("id").get<std::string>();
    if (id == "main" || !ipc::isValidInstanceId(id))
        throw AccessoryError(ipc::error::invalidParams, "アクセサリー ID が不正です");
    if (entries_.contains(id))
        throw AccessoryError(ipc::error::invalidParams, "アクセサリー ID は登録済みです");
    if (entries_.size() >= 32)
        throw AccessoryError(ipc::error::invalidParams, "アクセサリーの上限は32個です");
    Entry entry;
    entry.id = id;
    entry.path = assetPath(params.at("model"));
    entry.visible = params.value("visible", false);
    entry.physics = params.value("physics", false);
    entry.loop = params.value("loop", true);
    updateTransform(entry.transform, params);
    bind(entry, params.value("parentModel", std::string("main")), params.value("parentBone", std::string()));
    if (const auto motion = params.find("motion"); motion != params.end())
        entry.motion = assetPath(*motion);
    entry.model = std::make_unique<MmdModel>();
    entry.model->loadModel(entry.path);
    if (!entry.motion.empty())
        entry.model->loadMotion(entry.motion);
    entry.model->setupPhysics({entry.physics});
    entry.bounds = entry.model->bindPoseBounds();
    Playback::Config playback;
    playback.hasMotion = entry.model->hasMotion();
    playback.lastFrame = entry.model->motionLastFrame();
    playback.fixedStep = entry.model->physicsEnabled();
    playback.loop = entry.loop;
    entry.playback = Playback(playback);
    entry.renderer = std::make_unique<ModelRenderer>(d3d_, *entry.model, assets_);
    entries_.emplace(id, std::move(entry));
}

void Accessories::loadConfig(const std::filesystem::path &path) {
    const auto config = readJson(path);
    if (const auto list = config.find("accessories"); list != config.end()) {
        if (!list->is_array())
            throw std::runtime_error("accessories は配列で指定してください");
        for (const auto &params : *list) {
            try {
                load(params);
            } catch (const std::exception &error) {
                log::warn("アクセサリー設定を読み込めません: {}", error.what());
            }
        }
    }
    if (!config.value("saveAccessories", false))
        return;
    statePath_ = path.parent_path() / L"ukagaka3d_accessories_state.json";
    if (!std::filesystem::exists(statePath_))
        return;
    const auto state = readJson(statePath_);
    for (auto &[id, entry] : entries_) {
        const auto saved = state.find(id);
        if (saved == state.end())
            continue;
        try {
            auto transform = entry.transform;
            updateTransform(transform, *saved);
            const bool visible = saved->value("visible", entry.visible);
            const bool attached = saved->value("attached", entry.attached);
            entry.transform = transform;
            entry.visible = visible;
            entry.attached = attached;
        } catch (const std::exception &error) {
            log::warn("アクセサリー {} の保存状態を読み込めません: {}", id, error.what());
        }
    }
}

void Accessories::saveState() const {
    if (statePath_.empty())
        return;
    try {
        auto state = ipc::Json::object();
        for (const auto &[id, entry] : entries_)
            state[id] = {{"attached", entry.attached}, {"visible", entry.visible}, {"position", entry.transform.position},
                         {"rotation", entry.transform.rotation}, {"scale", entry.transform.scale}};
        auto temp = statePath_;
        temp += L".tmp";
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        file << state.dump(2);
        file.close();
        if (!file || !MoveFileExW(temp.c_str(), statePath_.c_str(), MOVEFILE_REPLACE_EXISTING))
            log::warn("アクセサリーの状態を保存できません");
    } catch (const std::exception &error) {
        log::warn("アクセサリーの状態を保存できません: {}", error.what());
    }
}

void Accessories::replaceMain(const MmdModel &main) {
    main_ = &main;
    mainBounds_ = main.bindPoseBounds();
    for (auto &[id, entry] : entries_) {
        if (entry.parent != "main")
            continue;
        entry.attached = false;
        entry.boneIndex = entry.bone.empty() ? std::nullopt : main.findBone(entry.bone);
    }
    // Replacing the parent only affects this session.
}

bool Accessories::active(const Entry &entry) const {
    if (!entry.visible || !entry.attached)
        return false;
    if (entry.parent.empty() || entry.parent == "main")
        return true;
    return active(entries_.at(entry.parent));
}

ipc::Json Accessories::status(const Entry &entry) const {
    return {{"id", entry.id}, {"model", pathToUtf8(entry.path)}, {"parentModel", entry.parent},
            {"parentBone", entry.bone}, {"attached", entry.attached}, {"visible", entry.visible},
            {"effectiveVisible", active(entry)}, {"physics", entry.model->physicsEnabled()},
            {"position", entry.transform.position}, {"rotation", entry.transform.rotation}, {"scale", entry.transform.scale},
            {"motion", pathToUtf8(entry.motion)}, {"loop", entry.loop}};
}

ipc::Json Accessories::execute(const ipc::Request &request) {
    const auto &params = request.params;
    const auto &command = request.command;
    const auto &rid = request.requestId;
    try {
        if (command == "list_accessories") {
            auto list = ipc::Json::array();
            for (const auto &[id, entry] : entries_)
                list.push_back(status(entry));
            return ipc::makeSuccess(rid, {{"accessories", std::move(list)}});
        }
        const auto id = params.at("id").get<std::string>();
        if (command == "load_accessory") {
            load(params);
            saveState();
            return ipc::makeSuccess(rid, status(entries_.at(id)));
        }
        const auto found = entries_.find(id);
        if (found == entries_.end())
            throw AccessoryError(ipc::error::notFound, "アクセサリーが見つかりません");
        auto &entry = found->second;
        if (command == "get_accessory_status")
            return ipc::makeSuccess(rid, status(entry));
        if (command == "unload_accessory") {
            for (auto &[childId, child] : entries_) {
                if (child.parent == id) {
                    child.parent.clear();
                    child.bone.clear();
                    child.boneIndex.reset();
                    child.attached = false;
                    child.visible = false;
                }
            }
            entries_.erase(found);
            saveState();
            return ipc::makeSuccess(rid);
        }
        if (command == "attach_accessory") {
            const auto parent = params.value("parentModel", entry.parent);
            const auto bone = params.value("parentBone", entry.bone);
            bind(entry, parent, bone);
            entry.visible = true;
        } else if (command == "detach_accessory") {
            entry.attached = false;
            entry.visible = false;
        } else if (command == "set_accessory_visible") {
            entry.visible = params.at("visible").get<bool>();
        } else if (command == "set_accessory_transform") {
            auto transform = entry.transform;
            updateTransform(transform, params);
            entry.transform = transform;
        } else {
            return ipc::makeError(rid, ipc::error::unknownCommand, "未対応のアクセサリーコマンドです");
        }
        saveState();
        return ipc::makeSuccess(rid, status(entry));
    } catch (const AccessoryError &error) {
        return ipc::makeError(rid, error.code, error.what());
    } catch (const ipc::Json::exception &error) {
        return ipc::makeError(rid, ipc::error::invalidParams, error.what());
    } catch (const std::exception &error) {
        return ipc::makeError(rid, ipc::error::loadFailed, error.what());
    }
}

bool Accessories::visible() const {
    return std::any_of(entries_.begin(), entries_.end(), [this](const auto &item) { return active(item.second); });
}

bool Accessories::animating() const {
    return std::any_of(entries_.begin(), entries_.end(), [this](const auto &item) {
        const auto &entry = item.second;
        return active(entry) && (entry.model->physicsEnabled() || (entry.model->hasMotion() && !entry.playback.finished()));
    });
}

void Accessories::update(double dt) {
    for (auto &[id, entry] : entries_) {
        if (!active(entry) || (!entry.dirty && !entry.model->physicsEnabled() && entry.playback.finished()))
            continue;
        const auto step = entry.playback.update(dt);
        if (!entry.dirty && step.kind == Playback::Kind::none)
            continue;
        if (step.kind == Playback::Kind::seek)
            entry.model->resetPhysics();
        const auto &frame = step.kind == Playback::Kind::wrap ? entry.model->evaluatePoseAcrossLoop(step.frame, step.seconds)
                                                              : entry.model->evaluatePose(step.frame, step.seconds);
        if (entry.model->poseOnlySupported()) {
            entry.model->skinner().skin(frame.bones, frame.morphWeights, entry.renderer->beginFrame());
            entry.renderer->commitFrame(frame, entry.dirty || step.kind == Playback::Kind::seek);
        } else {
            entry.renderer->setFrame(frame, entry.dirty || step.kind == Playback::Kind::seek);
        }
        entry.dirty = false;
        entry.renderer->upload(1.0F);
    }
}

void Accessories::draw(ModelRenderer &mainRenderer, const Camera &camera, std::span<const BoneTransform> previous,
                       std::span<const BoneTransform> current, float alpha) {
    struct Item {
        ModelRenderer *renderer;
        XMMATRIX world;
        float depth;
    };
    const auto view = camera.view();
    const auto depth = [&](const Bounds &bounds, FXMMATRIX world) {
        XMVECTOR midpoint = XMVectorZero();
        if (bounds.valid)
            midpoint = XMVectorSet((bounds.minimum[0] + bounds.maximum[0]) * 0.5F,
                                   (bounds.minimum[1] + bounds.maximum[1]) * 0.5F,
                                   (bounds.minimum[2] + bounds.maximum[2]) * 0.5F, 1.0F);
        return XMVectorGetZ(XMVector3TransformCoord(midpoint, world * view));
    };
    const auto identity = XMMatrixIdentity();
    std::array<Item, 33> items{};
    std::size_t count = 0;
    if (mainRenderer.visible())
        items[count++] = {&mainRenderer, identity, depth(mainBounds_, identity)};
    const auto worldOf = [&](const auto &recurse, const Entry &entry) -> XMMATRIX {
        const auto local = entry.transform.matrix();
        if (entry.parent.empty())
            return local;
        if (entry.parent == "main")
            return entry.boneIndex ? local * attachmentMatrix(main_->model(), *entry.boneIndex, previous, current, alpha)
                                   : local;
        const auto &parent = entries_.at(entry.parent);
        const auto parentWorld = recurse(recurse, parent);
        if (!entry.boneIndex)
            return local * parentWorld;
        return local * attachmentMatrix(parent.model->model(), *entry.boneIndex, {}, parent.model->bones(), 1.0F) *
               parentWorld;
    };
    for (auto &[id, entry] : entries_) {
        if (!active(entry) || !entry.renderer->visible())
            continue;
        const auto world = worldOf(worldOf, entry);
        items[count++] = {entry.renderer.get(), world, depth(entry.bounds, world)};
    }
    const std::span visibleItems(items.data(), count);
    for (const auto &item : visibleItems)
        item.renderer->draw(camera, item.world, ModelRenderer::Pass::opaque);
    std::stable_sort(visibleItems.begin(), visibleItems.end(), [](const Item &a, const Item &b) { return a.depth > b.depth; });
    for (const auto &item : visibleItems)
        item.renderer->draw(camera, item.world, ModelRenderer::Pass::translucent);
}

} // namespace uka3d
