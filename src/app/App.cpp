#include "app/App.h"
#include "app/Accessories.h"

#include "app/CameraTrack.h"
#include "app/Face.h"
#include "app/Playback.h"
#include "app/SeatAnchor.h"
#include "app/DesktopBehavior.h"
#include "app/Interaction.h"
#include "ipc/PathPolicy.h"
#include "ipc/PipeServer.h"
#include "mmd/MmdModel.h"
#include "platform/FramePacer.h"
#include "platform/Log.h"
#include "platform/SstpEvents.h"
#include "platform/TransparentWindow.h"
#include "platform/Utf.h"
#include "renderer/Camera.h"
#include "renderer/D3DContext.h"
#include "renderer/ModelRenderer.h"

#include <windows.h>

#include <psapi.h>

#include <algorithm>
#include <chrono>
#include <charconv>
#include <cmath>
#include <deque>
#include <future>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <random>
#include <tuple>

namespace uka3d {
namespace {

enum MenuId : UINT {
    menuPlayPause = 100,
    menuRewind,
    menuPhysics,
    menuTopmost,
    menuClickThrough,
    menuExit,
};

void showError(const RunEnvironment &env, const std::string &message) {
    if (env.showErrorDialogs)
        MessageBoxW(nullptr, utf8ToWide(message).c_str(), L"ukagaka3d エラー", MB_OK | MB_ICONERROR);
}

long long qpcNow() {
    LARGE_INTEGER counter;
    QueryPerformanceCounter(&counter);
    return counter.QuadPart;
}

double qpcSeconds(long long ticks) {
    static const double frequency = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return static_cast<double>(f.QuadPart);
    }();
    return static_cast<double>(ticks) / frequency;
}

class Clock {
  public:
    Clock() : last_(qpcNow()) {}
    double tick() {
        const long long now = qpcNow();
        const double dt = qpcSeconds(now - last_);
        last_ = now;
        return dt;
    }

  private:
    long long last_;
};

// CPU time used by this process (user + kernel), in seconds.
double processCpuSeconds() {
    FILETIME create, exit, kernel, user;
    if (!GetProcessTimes(GetCurrentProcess(), &create, &exit, &kernel, &user))
        return 0.0;
    const auto toSeconds = [](const FILETIME &ft) {
        return static_cast<double>((static_cast<unsigned long long>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime) /
               1e7;
    };
    return toSeconds(kernel) + toSeconds(user);
}

// Rolling statistics, logged every kStatInterval seconds.
struct FrameStats {
    static constexpr double kStatInterval = 10.0;
    double windowStart{};
    double cpuStart{};
    int presents{};
    int evaluations{};
    std::uint64_t ticksStart{};
    double evaluateSeconds{};
    double renderSeconds{};
    double maxInterval{};
    long long lastPresent{};

    void reset(double now) {
        windowStart = now;
        cpuStart = processCpuSeconds();
        presents = evaluations = 0;
        evaluateSeconds = renderSeconds = maxInterval = 0.0;
    }
};

struct BehaviorOptions {
    std::filesystem::path walkMotion;
    std::filesystem::path sitMotion;
    std::filesystem::path idleMotion;
    double speed{180.0};
    double waitSeconds{15.0};
    double sitChance{0.2};
    bool useRootMotion{false};
};

class App {
  public:
    App(const Options &options, const RunEnvironment &env)
        : options_(options), env_(env), pacer_(options.fps),
          playback_(Playback::Config{}), blinkOn_(options.blink), gazeOn_(options.gaze),
          blinkMorphName_(options.blinkMorph) {
        blink_ = BlinkScheduler(BlinkSettings{}, std::random_device{}()); // different timing every run
        GazeSettings gaze;
        gaze.enabled = options.gaze;
        gaze.radius = options.gazeRadius;
        gaze_.setSettings(gaze);
    }

    int run();

  private:
    void mainLoop();
    void frameTick(double dt);
    bool needsRedraw() const;
    std::optional<POINT> cursorProbe();
    void updateClickThrough(const std::optional<POINT> &probe);
    void onPointer(ShellWindow::PointerAction action, POINT client, bool flag);
    void checkMonitor(bool force);
    void logStats(bool final);
    void showContextMenu(POINT screen);
    void togglePlay();
    void togglePhysics();
    void rewind();
    void requestExit();

    // ---- IPC (commands from the SAORI bridge / other clients) ---------------
    struct PendingCommand {
        ipc::Request request;
        std::promise<ipc::Json> response;
    };
    // Which camera track goes with a motion: the <name>.camera.json next to it
    // (if any), none, or an explicit file.
    struct CameraChoice {
        enum class Mode : std::uint8_t { sidecar, none, file } mode{Mode::sidecar};
        std::filesystem::path path;
    };
    struct QueuedMotion {
        std::filesystem::path path;
        bool loop{true};
        CameraChoice camera;
    };
    bool startIpc();
    ipc::Json handleIpcOnServerThread(const ipc::Request &request);
    void processIpcQueue();
    ipc::Json executeCommand(const ipc::Request &request);
    ipc::Json statusJson() const;
    std::optional<std::filesystem::path> resolveIpcPath(const ipc::Json &params, const char *key,
                                                        ipc::Json &errorResponse, const std::string &requestId,
                                                        bool allowMissing = false) const;
    void loadNewModel(const std::filesystem::path &path);
    void playMotion(const std::filesystem::path &path, bool loop, const CameraChoice &camera = {});
    std::optional<CameraChoice> readCameraChoice(const ipc::Json &params, ipc::Json &errorResponse,
                                                 const std::string &requestId) const;
    void setCameraTrack(std::optional<CameraTrack> track, std::filesystem::path path);
    void bindFace();
    bool updateFace(double dt, float frame);
    [[nodiscard]] ipc::Json faceJson() const;
    void updateCameraView();
    [[nodiscard]] mmd::Float3 defaultCameraFocus() const;
    void stopMotion();
    void failQueuedCommands();
    void fitCamera();
    void followWindow();
    HWND findFollowTarget() const;
    ipc::Json executeBehavior(const ipc::Request &request);
    ipc::Json behaviorStatus() const;
    bool readBehaviorOptions(const ipc::Request &request, BehaviorOptions &options, ipc::Json &error);
    void tickBehavior(double now);
    void startWalk(POINT destination, const BehaviorOptions &options, bool automatic);
    void startSeat(const DesktopWindow &target, SeatPosition position, std::optional<std::size_t> hip,
                   float offset, const BehaviorOptions &options, bool automatic);
    void finishBehavior(bool returnFromSeat);
    void playBehaviorMotion(const std::filesystem::path &path);
    void placeSeat();
    [[nodiscard]] RECT shellRect() const;
    [[nodiscard]] Camera behaviorCamera(bool withSeatOffset = true, bool withTrack = true) const;
    void presentPose(const mmd::AnimatedModelFrame &frame, bool discontinuous);
    void beginPoseFade(double seconds);
    void bindSeatAnchor();
    [[nodiscard]] DirectX::XMFLOAT2 seatAnchorPoint() const;
    [[nodiscard]] ipc::Json seatConfigJson() const;
    ipc::Json configureSeat(const ipc::Request &request);
    void tickGlide(double now);

    const Options &options_;
    const RunEnvironment &env_;
    std::unique_ptr<MmdModel> model_;
    std::unique_ptr<ShellWindow> window_;
    std::optional<InteractionProfile> interactionProfile_;
    HitRegions hitRegions_;
    std::unique_ptr<SstpEvents> sstpEvents_;
    std::optional<InteractionInput> interactionInput_;
    std::optional<double> transparentSince_; // cursor over a transparent pixel since (qpc seconds)
    static constexpr double kClickThroughDelaySeconds = 0.08;
    std::unique_ptr<D3DContext> d3d_;
    std::unique_ptr<ModelRenderer::SharedAssets> sharedAssets_;
    std::unique_ptr<ModelRenderer> renderer_;
    std::unique_ptr<Accessories> accessories_;
    std::vector<BoneTransform> previousDisplayBones_;
    Camera camera_;
    Clock clock_;
    FramePacer pacer_;
    Playback playback_;

    bool exitRequested_{false};
    bool clickThroughEnabled_{true};
    bool viewDirty_{true}; // resize/zoom/etc. needs a redraw
    bool occluded_{false};
    HMONITOR monitor_{nullptr};
    double refreshHz_{0.0};
    long long loopStart_{};
    double lastMonitorCheck_{-1.0};
    FrameStats stats_;
    std::uint64_t totalPresents_{};
    double measuredFps_{0.0};

    ipc::PipeServer ipcServer_;
    std::mutex queueMutex_;
    std::deque<std::shared_ptr<PendingCommand>> queue_;
    bool acceptingCommands_{true};
    HANDLE parentProcess_{nullptr};
    bool needEvaluate_{false};
    HWND followTarget_{nullptr};
    double lastFollowCheck_{-1.0};
    std::optional<QueuedMotion> nextMotion_;

    // Camera track of the current motion. cameraView_ is what is applied this
    // frame; on a motion switch it fades from the view shown at that moment.
    static constexpr double kCameraFadeSeconds = 0.25;
    std::optional<CameraTrack> cameraTrack_;
    std::filesystem::path cameraTrackPath_;
    std::optional<CameraView> cameraView_; // nullopt = the default camera
    CameraView cameraFadeFrom_{};
    double cameraFadeStart_{0.0};
    bool cameraFading_{false};

    // Automatic blink and gaze (Face.h). Bones/morphs are looked up per model.
    BlinkScheduler blink_;
    GazeFollower gaze_;
    bool blinkOn_{true};
    bool gazeOn_{true};
    std::string blinkMorphName_;
    std::optional<std::size_t> blinkMorph_;
    std::vector<std::size_t> eyeClosingMorphs_;
    std::optional<std::size_t> neckBone_, headBone_, eyesBone_, leftEyeBone_, rightEyeBone_;
    float lastBlinkWeight_{0.0F};

    struct Seat {
        DesktopWindow window;
        SeatPosition position;
        std::optional<std::size_t> hip;
        float offset;
        POINT returnPosition;
        POINT edge{};
        RECT work{};
        // Resting window origin; only changes when the target moves or the pose drifts far.
        POINT origin{};
        bool edgeDirty{true};
        // Hop from the previous position onto the seat.
        POINT hopFrom{};
        double hopStart{};
        bool landed{false};
    };
    // Window glide back to the pre-seat position (stand up).
    struct Glide {
        POINT from{};
        POINT to{};
        double start{};
        double seconds{};
    };
    std::unique_ptr<DesktopWindows> desktopWindows_;
    BehaviorState behavior_{BehaviorState::idle};
    std::optional<DesktopWalk> walk_;
    std::optional<Seat> seat_;
    std::optional<std::size_t> rootBone_;
    BehaviorOptions activeBehavior_;
    BehaviorOptions autoOptions_;
    std::mt19937 behaviorRandom_{std::random_device{}()};
    bool desktopManaged_{false};
    bool autoBehavior_{false};
    bool crossMonitors_{false};
    bool autoAction_{false};
    bool grabbed_{false};
    std::filesystem::path grabMotion_;
    std::filesystem::path grabIdleMotion_;
    int walkDirection_{-1};
    double behaviorUpdated_{};
    double seatChecked_{};
    double nextAutoAction_{};

    // Pose shown on screen (may be a cross-fade between two motions).
    std::vector<BoneTransform> displayBones_;
    std::vector<BoneTransform> fadeFrom_;
    std::vector<BoneTransform> blendScratch_;
    std::unique_ptr<PoseBlender> poseBlender_;
    bool fading_{false};
    double fadeStart_{};
    double fadeSeconds_{};

    SeatSettings seatSettings_;
    SeatAnchor seatAnchor_;
    // Seat anchor in client pixels (projected without the seat offset), per tick.
    DirectX::XMFLOAT2 anchorPrevious_{};
    DirectX::XMFLOAT2 anchorCurrent_{};
    float seatNdcX_{0.0F};
    float seatNdcY_{0.0F};
    std::optional<Glide> glide_;
};

double smoothStep(double t) {
    t = std::clamp(t, 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

constexpr double kBehaviorFadeSeconds = 0.25;

int App::run() {
    // ---- Load assets ------------------------------------------------------
    model_ = std::make_unique<MmdModel>();
    if (options_.parentPid != 0) {
        parentProcess_ = OpenProcess(SYNCHRONIZE, FALSE, options_.parentPid);
        if (parentProcess_ == nullptr) {
            log::fatal("親プロセス (pid {}) が見つからないため終了します", options_.parentPid);
            return exitUsage;
        }
    }
    try {
        model_->loadModel(options_.modelPath);
        if (!options_.motionPath.empty())
            model_->loadMotion(options_.motionPath);
        else
            log::info("VMD未指定: 静止表示します");
    } catch (const LoadError &e) {
        log::fatal("{}", e.what());
        showError(env_, e.what());
        return exitLoadError;
    }
    PhysicsOptions physicsOptions;
    physicsOptions.enabled = options_.physics;
    model_->setupPhysics(physicsOptions);
    log::info("物理演算の状態: {}{}", toString(model_->physicsStatus()),
              model_->physicsEnabled() ? "（固定ステップ 1/60 秒）" : "");

    bindSeatAnchor();

    Playback::Config playbackConfig;
    playbackConfig.hasMotion = model_->hasMotion();
    playbackConfig.lastFrame = model_->motionLastFrame();
    playbackConfig.loop = options_.loop;
    playbackConfig.fixedStep = model_->physicsEnabled();
    playback_ = Playback(playbackConfig);

    if (!options_.hitRegionsPath.empty() && options_.parentPid != 0 && !options_.allowedRoots.empty()) {
        try {
            interactionProfile_ = InteractionProfile::load(options_.hitRegionsPath);
            hitRegions_.bind(*interactionProfile_, *model_);
            log::info("3D 入力設定: {} 領域を読み込み", interactionProfile_->spheres.size());
        } catch (const std::exception &error) {
            interactionProfile_.reset();
            log::warn("3D 入力設定を読み込めません: {}", error.what());
        }
    }

    // ---- Window + Direct3D -------------------------------------------------
    const bool transparent = options_.windowMode == WindowMode::transparent;
    try {
        ShellWindow::Callbacks callbacks;
        callbacks.onResize = [this](UINT w, UINT h) {
            if (d3d_) {
                d3d_->resize(w, h);
                viewDirty_ = true;
            }
        };
        callbacks.onRenderRequest = [this] { frameTick(clock_.tick()); };
        callbacks.onKeyDown = [this](WPARAM key) {
            if (key == VK_ESCAPE)
                requestExit();
            else if (key == VK_SPACE)
                togglePlay();
            else if (key == VK_HOME)
                rewind();
            else if (key == 'P')
                togglePhysics();
        };
        callbacks.onContextMenu = [this](POINT pt) { showContextMenu(pt); };
        callbacks.onWheel = [this](float notches, bool ctrl) {
            if (ctrl && window_->transparent())
                window_->scaleBy(std::pow(1.1F, notches));
            else
                camera_.zoom(std::pow(0.9F, notches));
            viewDirty_ = true;
        };
        callbacks.onClose = [this] { requestExit(); };
        callbacks.onWake = [this] { processIpcQueue(); };
        callbacks.onDragStart = [this] {
            glide_.reset();
            if (walk_ || seat_)
                finishBehavior(false);
            if (desktopManaged_)
                nextAutoAction_ = qpcSeconds(qpcNow()) + autoOptions_.waitSeconds;
        };
        callbacks.onGrabStart = [this] {
            grabbed_ = true;
            log::info("つまみ開始");
            playBehaviorMotion(grabMotion_);
        };
        callbacks.onGrabEnd = [this] {
            grabbed_ = false;
            nextAutoAction_ = qpcSeconds(qpcNow()) + autoOptions_.waitSeconds;
            log::info("つまみ終了");
            playBehaviorMotion(grabIdleMotion_.empty() ? activeBehavior_.idleMotion : grabIdleMotion_);
        };
        if (interactionProfile_)
            callbacks.onPointer = [this](auto action, POINT client, bool flag) { onPointer(action, client, flag); };

        std::wstring title = L"ukagaka3d - " + utf8ToWide(model_->model().metadata.modelName);
        window_ = std::make_unique<ShellWindow>(title, options_.width, options_.height, transparent,
                                                options_.topmost, std::move(callbacks), !options_.ipcInstance.empty());
        const SIZE client = window_->clientSize();
        D3DContext::Config config;
        config.hwnd = window_->hwnd();
        config.transparent = transparent;
        config.width = static_cast<UINT>(client.cx);
        config.height = static_cast<UINT>(client.cy);
        config.debugLayer = options_.debug;
        d3d_ = std::make_unique<D3DContext>(config);
        sharedAssets_ = std::make_unique<ModelRenderer::SharedAssets>(*d3d_);
        renderer_ = std::make_unique<ModelRenderer>(*d3d_, *model_, *sharedAssets_);
        auto accessoryBase = options_.accessoryConfig.empty() ?
            (options_.allowedRoots.empty() ? model_->modelPath().parent_path() : options_.allowedRoots.front()) :
            std::filesystem::absolute(options_.accessoryConfig).parent_path();
        auto roots = options_.allowedRoots;
        if (roots.empty())
            roots.push_back(accessoryBase);
        accessories_ = std::make_unique<Accessories>(*d3d_, *sharedAssets_, *model_, accessoryBase, std::move(roots));
        if (!options_.accessoryConfig.empty()) {
            try {
                accessories_->loadConfig(options_.accessoryConfig);
            } catch (const std::exception &error) {
                log::warn("アクセサリー設定を読み込めません: {}", error.what());
            }
        }
        if (interactionProfile_) {
            // SAORI supplies its ghost/master directory as the first allowed root.
            auto master = options_.allowedRoots.front().lexically_normal();
            if (master.filename().empty())
                master = master.parent_path();
            sstpEvents_ = std::make_unique<SstpEvents>(window_->hwnd(), options_.parentPid,
                                                      master.parent_path().parent_path());
            interactionInput_.emplace(interactionProfile_->pet, interactionProfile_->mouseMove, GetDoubleClickTime(),
                                      [this](MouseEventKind kind, const PointerSample &sample) {
                                          if (kind != MouseEventKind::move)
                                              log::debug("入力イベント: {} {}", static_cast<int>(kind), sample.region);
                                          sstpEvents_->enqueue(kind, sample);
                                      });
        }
    } catch (const std::exception &e) {
        log::fatal("描画の初期化に失敗しました: {}", e.what());
        showError(env_, std::string("描画の初期化に失敗しました:\n") + e.what());
        return exitGraphicsError;
    }

    fitCamera();
    log::info("カメラ初期化: 注視点=({:.2f}, {:.2f}, {:.2f}) 距離={:.2f}", camera_.target.x, camera_.target.y,
              camera_.target.z, camera_.distance);
    if (!options_.ipcInstance.empty() && !startIpc()) {
        sstpEvents_.reset();
        accessories_.reset();
        renderer_.reset();
        sharedAssets_.reset();
        d3d_.reset();
        window_.reset();
        return exitIpcError;
    }
    window_->show();
    if (transparent)
        desktopWindows_ = std::make_unique<DesktopWindows>(options_.parentPid);
    checkMonitor(true);
    log::info("描画ループ開始{}", model_->hasMotion() ? (options_.loop ? "（ループ再生）" : "（1回再生）") : "");

    // ---- Main loop ---------------------------------------------------------
    int exitCode = exitOk;
    try {
        mainLoop();
    } catch (const std::exception &e) {
        log::fatal("描画中にエラーが発生しました: {}", e.what());
        showError(env_, std::string("描画中にエラーが発生しました:\n") + e.what());
        exitCode = exitGraphicsError;
    }
    logStats(true);

    // Answer what is still queued, then stop the server (it waits for an
    // in-flight response such as the reply to "shutdown").
    {
        std::lock_guard lock(queueMutex_);
        acceptingCommands_ = false;
    }
    processIpcQueue();
    ipcServer_.stop();
    failQueuedCommands();
    if (parentProcess_ != nullptr)
        CloseHandle(parentProcess_);

    sstpEvents_.reset();
    accessories_.reset();
    renderer_.reset();
    sharedAssets_.reset();
    d3d_.reset();
    desktopWindows_.reset();
    window_.reset();
    return exitCode;
}

void App::mainLoop() {
    loopStart_ = qpcNow();
    clock_.tick();
    stats_.reset(0.0);
    stats_.lastPresent = qpcNow();
    int quitCode = 0;
    while (!exitRequested_) {
        if (!ShellWindow::pumpMessages(quitCode) || exitRequested_)
            break;
        if (interactionInput_)
            interactionInput_->tick(GetTickCount64());
        if (parentProcess_ != nullptr && WaitForSingleObject(parentProcess_, 0) == WAIT_OBJECT_0) {
            log::info("起動元プロセスが終了したため終了します");
            requestExit();
            break;
        }
        const double sinceStart = qpcSeconds(qpcNow() - loopStart_);

        if (options_.exitAfterSeconds > 0.0 && sinceStart >= options_.exitAfterSeconds) {
            log::info("--exit-after {} 秒に達したため終了します", options_.exitAfterSeconds);
            if (!options_.capturePath.empty())
                d3d_->requestCapture(options_.capturePath);
            viewDirty_ = true;
            frameTick(clock_.tick());
            requestExit();
            break;
        }
        if (sinceStart - lastMonitorCheck_ >= 1.0)
            checkMonitor(false);
        if (!options_.followWindowClass.empty() && sinceStart - lastFollowCheck_ >= 0.05) {
            lastFollowCheck_ = sinceStart;
            followWindow();
        }
        if (desktopManaged_ || autoBehavior_)
            tickBehavior(qpcSeconds(qpcNow()));

        // Minimised / hidden / occluded: no rendering, sleep until input or 100 ms.
        if (IsIconic(window_->hwnd()) || !IsWindowVisible(window_->hwnd()) || occluded_) {
            MsgWaitForMultipleObjectsEx(0, nullptr, 100, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            clock_.tick(); // the simulation does not run while hidden
            if (occluded_) {
                occluded_ = false; // re-test with the next Present
                viewDirty_ = true;
            }
            continue;
        }

        // Frame pacing: timer mode waits here; VSync mode waits inside Present.
        if (!pacer_.frameDue()) {
            pacer_.waitForFrameOrMessage();
            continue;
        }

        const auto probe = cursorProbe();
        if (!needsRedraw() && !(accessories_->visible() && probe && !d3d_->alphaAt(*probe))) {
            // Idle (paused or static without physics): keep click-through
            // alive but do not render. Wake on input or every ~33 ms.
            const double dt = clock_.tick();
            playback_.update(dt);
            if (updateFace(dt, static_cast<float>(playback_.frame())))
                needEvaluate_ = true; // a blink or the gaze: render the next frame
            updateClickThrough(probe);
            if (!accessories_->visible())
                d3d_->probeOnly(probe);
            MsgWaitForMultipleObjectsEx(0, nullptr, 33, QS_ALLINPUT, MWMO_INPUTAVAILABLE);
            continue;
        }
        frameTick(clock_.tick());
    }
}

bool App::needsRedraw() const {
    if (accessories_->animating())
        return true;
    if (viewDirty_ || needEvaluate_ || fading_ || cameraFading_ || glide_ || (seat_ && !seat_->landed))
        return true;
    if (playback_.paused())
        return false;
    // Physics keeps simulating even without a motion (hair settling, etc.).
    return playback_.fixedStep() || model_->hasMotion();
}

void App::frameTick(double dt) {
    const long long t0 = qpcNow();
    const Playback::Step step = playback_.update(dt);
    if (updateFace(dt, step.kind != Playback::Kind::none ? step.frame : static_cast<float>(playback_.frame())))
        needEvaluate_ = true;
    if (step.kind != Playback::Kind::none) {
        const mmd::AnimatedModelFrame *frame = nullptr;
        switch (step.kind) {
        case Playback::Kind::seek:
            model_->resetPhysics();
            frame = &model_->evaluatePose(step.frame, 0.0F);
            break;
        case Playback::Kind::advance:
            frame = &model_->evaluatePose(step.frame, step.seconds);
            break;
        case Playback::Kind::wrap:
            frame = &model_->evaluatePoseAcrossLoop(step.frame, step.seconds);
            break;
        case Playback::Kind::none:
            break;
        }
        presentPose(*frame, step.kind == Playback::Kind::seek);
        ++stats_.evaluations;
        needEvaluate_ = false;
    } else if (needEvaluate_ || fading_) {
        // Morph change or pose cross-fade while paused / idle: re-evaluate the
        // same frame without a simulation step.
        presentPose(model_->evaluatePose(static_cast<float>(playback_.frame()), 0.0F), false);
        ++stats_.evaluations;
        needEvaluate_ = false;
    }
    if (playback_.finished() && nextMotion_) {
        const QueuedMotion next = *nextMotion_;
        nextMotion_.reset();
        try {
            playMotion(next.path, next.loop, next.camera);
        } catch (const std::exception &e) {
            log::error("続けて再生するモーションを読み込めません: {}", e.what());
        }
    }
    accessories_->update(dt);
    const long long t1 = qpcNow();
    stats_.evaluateSeconds += qpcSeconds(t1 - t0);

    // Fixed-step physics: draw the state between the last two ticks so that
    // render rates above 60 fps stay smooth (and 30 fps does not judder).
    renderer_->upload(playback_.interpolationAlpha());

    if (glide_)
        tickGlide(qpcSeconds(qpcNow()));
    if (seat_)
        placeSeat();
    updateCameraView();
    const Camera renderCamera = behaviorCamera();
    if (behavior_ == BehaviorState::transition) {
        if (seat_ && seat_->landed)
            behavior_ = BehaviorState::sitting;
        else if (!seat_ && !glide_)
            behavior_ = BehaviorState::idle;
    }
    const float transparentClear[4] = {0.0F, 0.0F, 0.0F, 0.0F};
    const float windowedClear[4] = {0.18F, 0.20F, 0.24F, 1.0F};
    const auto *clear = d3d_->transparent() ? transparentClear : windowedClear;
    const auto probe = cursorProbe();
    const bool multiModel = accessories_->visible();
    d3d_->beginFrame(clear);
    if (multiModel) {
        if (probe) {
            renderer_->draw(renderCamera);
            d3d_->sampleInputFrame(*probe);
            d3d_->beginFrame(clear);
        }
        accessories_->draw(*renderer_, renderCamera, previousDisplayBones_, displayBones_,
                           playback_.interpolationAlpha());
    } else {
        renderer_->draw(renderCamera);
    }
    updateClickThrough(probe);
    const long long t2 = qpcNow();
    stats_.renderSeconds += qpcSeconds(t2 - t1);

    occluded_ = !d3d_->endFrame(probe, pacer_.syncInterval(), !multiModel);
    pacer_.framePresented();
    viewDirty_ = false;

    const long long now = qpcNow();
    stats_.maxInterval = std::max(stats_.maxInterval, qpcSeconds(now - stats_.lastPresent));
    stats_.lastPresent = now;
    ++stats_.presents;
    ++totalPresents_;
    const double sinceStart = qpcSeconds(now - loopStart_);
    if (sinceStart - stats_.windowStart >= FrameStats::kStatInterval)
        logStats(false);
}

std::optional<POINT> App::cursorProbe() {
    if (!window_->transparent())
        return std::nullopt;
    POINT cursor{};
    GetCursorPos(&cursor);
    ScreenToClient(window_->hwnd(), &cursor);
    const SIZE size = window_->clientSize();
    if (cursor.x >= 0 && cursor.y >= 0 && cursor.x < size.cx && cursor.y < size.cy)
        return cursor;
    return std::nullopt;
}

void App::updateClickThrough(const std::optional<POINT> &probe) {
    if (!window_->transparent())
        return;
    if (window_->dragging() || !clickThroughEnabled_ || !probe) {
        transparentSince_.reset();
        window_->setClickThrough(false);
        return;
    }
    const auto alpha = d3d_->probedAlpha();
    if (!alpha)
        return;
    if (*alpha >= 8) {
        transparentSince_.reset();
        window_->setClickThrough(false);
        return;
    }
    // Let clicks through only after the cursor has stayed over a transparent
    // pixel for a moment. Passing over gaps in hair or between fingers would
    // otherwise switch click-through on for a frame, and Windows reports that
    // as the mouse leaving the window, which restarts pet detection.
    const double now = qpcSeconds(qpcNow());
    if (!transparentSince_)
        transparentSince_ = now;
    if (now - *transparentSince_ >= kClickThroughDelaySeconds)
        window_->setClickThrough(true);
}

void App::onPointer(ShellWindow::PointerAction action, POINT client, bool flag) {
    if (!interactionInput_)
        return;
    using Action = ShellWindow::PointerAction;
    auto &input = *interactionInput_;
    const auto now = GetTickCount64();
    if (action != Action::move)
        log::debug("ポインタ: 操作={} ({}, {}) flag={}", static_cast<int>(action), client.x, client.y, flag);
    switch (action) {
    case Action::leave: input.leave(); return;
    case Action::cancel: input.cancelPress(); return;
    case Action::dragStart: input.startDrag(now); return;
    default: break;
    }

    POINT screen = client;
    ClientToScreen(window_->hwnd(), &screen);
    PointerSample sample{client.x, client.y, static_cast<float>(screen.x), static_cast<float>(screen.y), {},
                         static_cast<float>(GetDpiForWindow(window_->hwnd())) / 96.0F};
    if (action == Action::dragEnd) {
        input.endDrag(std::move(sample));
        return;
    }
    const SIZE size = window_->clientSize();
    if (client.x >= 0 && client.y >= 0 && client.x < size.cx && client.y < size.cy) {
        const auto alpha = window_->transparent() ? d3d_->alphaAt(client) : std::nullopt;
        if (!alpha || *alpha >= 8) {
            const auto ray = behaviorCamera().ray(static_cast<float>(client.x) + 0.5F,
                                                  static_cast<float>(client.y) + 0.5F,
                                                  static_cast<float>(size.cx), static_cast<float>(size.cy));
            sample.region = hitRegions_.pick(ray, *model_);
            sample.overModel = !sample.region.empty() || alpha.has_value();
        }
    }
    if (action == Action::move) {
        log::debug("ポインタ移動: ({}, {}) 部位={} モデル上={} ボタン={}", client.x, client.y, sample.region,
                   sample.overModel, flag);
        input.move(std::move(sample), now, flag);
    }
    else if (action == Action::release)
        input.release(sample, now, flag);
    else
        input.press(std::move(sample), now, action == Action::doublePress);
}

void App::checkMonitor(bool force) {
    lastMonitorCheck_ = qpcSeconds(qpcNow() - loopStart_);
    HMONITOR monitor = MonitorFromWindow(window_->hwnd(), MONITOR_DEFAULTTONEAREST);
    const double refresh = FramePacer::monitorRefreshRate(window_->hwnd());
    if (!force && monitor == monitor_ && refresh == refreshHz_)
        return;
    monitor_ = monitor;
    refreshHz_ = refresh;
    pacer_.configure(refresh);
    log::info("フレームレート制御: {}", pacer_.describe());
}

void App::logStats(bool final) {
    const double now = qpcSeconds(qpcNow() - loopStart_);
    const double window = std::max(1e-6, now - stats_.windowStart);
    const double cpu = processCpuSeconds() - stats_.cpuStart;
    SYSTEM_INFO info{};
    GetSystemInfo(&info);
    const std::uint64_t ticks = playback_.totalTicks() - stats_.ticksStart;
    if (stats_.presents > 0)
        measuredFps_ = stats_.presents / window;
    if (stats_.presents > 0 || final) {
        log::info("実測: 描画 {:.1f}fps (目標 {}) | 最大フレーム間隔 {:.1f}ms | 評価 {}回 平均 {:.2f}ms | 描画CPU 平均 "
                  "{:.2f}ms | 物理 {} tick ({:.1f}/s) | CPU {:.1f}% (1コア比, 全体 {:.1f}%) | 物理={} 一時停止={}",
                  stats_.presents / window, options_.fps == 0 ? std::string("無制限") : std::to_string(options_.fps),
                  stats_.maxInterval * 1000.0, stats_.evaluations,
                  stats_.evaluations ? stats_.evaluateSeconds * 1000.0 / stats_.evaluations : 0.0,
                  stats_.presents ? stats_.renderSeconds * 1000.0 / stats_.presents : 0.0, ticks, ticks / window,
                  cpu / window * 100.0, cpu / window * 100.0 / std::max<DWORD>(1, info.dwNumberOfProcessors),
                  model_->physicsEnabled() ? "ON" : "OFF", playback_.paused() ? "はい" : "いいえ");
    }
    stats_.reset(now);
    stats_.ticksStart = playback_.totalTicks();
    if (final) {
        PROCESS_MEMORY_COUNTERS_EX memory{};
        memory.cb = sizeof(memory);
        GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS *>(&memory),
                             sizeof(memory));
        log::info("統計: 総描画 {} フレーム / {:.1f} 秒, ループ {} 回, 破棄した遅延 {:.2f} 秒 | メモリ: private {:.1f}MB, "
                  "peak working set {:.1f}MB",
                  totalPresents_, now, playback_.loopCount(), playback_.droppedSeconds(),
                  memory.PrivateUsage / 1048576.0, memory.PeakWorkingSetSize / 1048576.0);
    }
}

// ---------------------------------------------------------------------------
// IPC
// ---------------------------------------------------------------------------
bool App::startIpc() {
    ipc::PipeServer::Config config;
    config.instanceId = options_.ipcInstance;
    config.allowedClientPid = options_.parentPid;
    std::string error;
    const bool ok = ipcServer_.start(
        config, [this](const ipc::Request &request) { return handleIpcOnServerThread(request); },
        [](const std::string &message) { log::info("IPC: {}", message); }, error);
    if (!ok) {
        log::fatal("IPC サーバーを開始できません: {}", error);
        return false;
    }
    log::info("IPC サーバー開始: \\\\.\\pipe\\ukagaka3d.{} (接続許可: {}, 許可フォルダ {} 件)", options_.ipcInstance,
              options_.parentPid != 0 ? std::format("pid {}", options_.parentPid) : std::string("同一ユーザー"),
              options_.allowedRoots.size());
    for (const auto &rootPath : options_.allowedRoots)
        log::info("IPC 許可フォルダ: {}", pathToUtf8(rootPath));
    return true;
}

// Runs on the pipe thread: hands the command to the UI thread and waits.
ipc::Json App::handleIpcOnServerThread(const ipc::Request &request) {
    auto pending = std::make_shared<PendingCommand>();
    pending->request = request;
    auto future = pending->response.get_future();
    {
        std::lock_guard lock(queueMutex_);
        if (!acceptingCommands_)
            return ipc::makeError(request.requestId, ipc::error::notRunning, "ランタイムは終了処理中です");
        queue_.push_back(pending);
    }
    window_->postWake();
    if (future.wait_for(std::chrono::seconds(20)) != std::future_status::ready)
        return ipc::makeError(request.requestId, ipc::error::timeout, "ランタイムの UI スレッドが応答しません");
    return future.get();
}

void App::processIpcQueue() {
    for (;;) {
        std::shared_ptr<PendingCommand> pending;
        {
            std::lock_guard lock(queueMutex_);
            if (queue_.empty())
                return;
            pending = queue_.front();
            queue_.pop_front();
        }
        ipc::Json response;
        try {
            response = executeCommand(pending->request);
        } catch (const std::exception &e) {
            response = ipc::makeError(pending->request.requestId, ipc::error::internal, e.what());
        }
        pending->response.set_value(std::move(response));
    }
}

void App::failQueuedCommands() {
    std::lock_guard lock(queueMutex_);
    for (auto &pending : queue_)
        pending->response.set_value(
            ipc::makeError(pending->request.requestId, ipc::error::notRunning, "ランタイムは終了しました"));
    queue_.clear();
}

std::optional<std::filesystem::path> App::resolveIpcPath(const ipc::Json &params, const char *key,
                                                         ipc::Json &errorResponse,
                                                         const std::string &requestId, bool allowMissing) const {
    const auto it = params.find(key);
    if (it == params.end() || !it->is_string() || it->get<std::string>().empty()) {
        errorResponse = ipc::makeError(requestId, ipc::error::invalidParams, std::format("{} （文字列）が必要です", key));
        return std::nullopt;
    }
    const std::filesystem::path requested = utf8ToPath(it->get<std::string>());
    std::filesystem::path resolved;
    if (!options_.allowedRoots.empty()) {
        // Relative paths are interpreted against the first allowed root.
        auto within = ipc::resolveWithinRoots(requested, options_.allowedRoots.front(), options_.allowedRoots);
        if (!within) {
            errorResponse = ipc::makeError(requestId, ipc::error::forbiddenPath,
                                           "許可されたフォルダの外のファイルは指定できません: " + it->get<std::string>());
            return std::nullopt;
        }
        resolved = *within;
    } else {
        if (!requested.is_absolute()) {
            errorResponse = ipc::makeError(requestId, ipc::error::invalidParams, "絶対パスで指定してください");
            return std::nullopt;
        }
        resolved = requested.lexically_normal();
    }
    std::error_code ec;
    if (!allowMissing && !std::filesystem::is_regular_file(resolved, ec)) {
        errorResponse =
            ipc::makeError(requestId, ipc::error::notFound, "ファイルが見つかりません: " + pathToUtf8(resolved));
        return std::nullopt;
    }
    return resolved;
}

void App::loadNewModel(const std::filesystem::path &path) {
    auto model = std::make_unique<MmdModel>();
    model->loadModel(path); // throws LoadError; the current model stays on failure
    PhysicsOptions physicsOptions;
    physicsOptions.enabled = options_.physics;
    model->setupPhysics(physicsOptions);
    // A reload of the same path may have changed the PMX contents.
    sharedAssets_->meshes.erase(model->modelPath());
    sharedAssets_->textures.clearLookup();
    auto renderer = std::make_unique<ModelRenderer>(*d3d_, *model, *sharedAssets_);
    if (walk_ || seat_)
        finishBehavior(true);

    renderer_.reset(); // refers to the old model
    model_ = std::move(model);
    accessories_->replaceMain(*model_);
    previousDisplayBones_.clear();
    if (interactionProfile_) {
        hitRegions_.bind(*interactionProfile_, *model_);
        interactionInput_->leave();
    }
    renderer_ = std::move(renderer);
    nextMotion_.reset();
    cameraTrack_.reset(); // the new model starts with the default camera
    cameraTrackPath_.clear();
    cameraView_.reset();
    cameraFading_ = false;
    needEvaluate_ = false;
    bindSeatAnchor();

    Playback::Config config;
    config.fixedStep = model_->physicsEnabled();
    playback_ = Playback(config);
    fitCamera();
    SetWindowTextW(window_->hwnd(), (L"ukagaka3d - " + utf8ToWide(model_->model().metadata.modelName)).c_str());
    viewDirty_ = true;
}

void App::playMotion(const std::filesystem::path &path, bool loop, const CameraChoice &camera) {
    // An explicit camera file must load (the request fails otherwise); a broken
    // sidecar only costs the camera move, not the motion.
    std::optional<CameraTrack> track;
    std::filesystem::path trackPath;
    if (camera.mode == CameraChoice::Mode::file) {
        track = CameraTrack::load(camera.path);
        trackPath = camera.path;
    }
    model_->loadMotion(path); // throws LoadError; the current motion stays on failure
    if (camera.mode == CameraChoice::Mode::sidecar) {
        for (const auto &sidecar : CameraTrack::sidecarPaths(path)) {
            std::error_code ec;
            if (!std::filesystem::is_regular_file(sidecar, ec))
                continue;
            try {
                track = CameraTrack::load(sidecar);
                trackPath = sidecar;
            } catch (const std::exception &e) {
                log::warn("カメラトラックを読み込めないため、カメラは動かしません: {}", e.what());
            }
            break;
        }
    }
    setCameraTrack(std::move(track), std::move(trackPath));
    Playback::Config config = playback_.config();
    config.hasMotion = true;
    config.lastFrame = model_->motionLastFrame();
    config.loop = loop;
    playback_.restart(config); // frame 0, physics state kept
    viewDirty_ = true;
}

// ---------------------------------------------------------------------------
// Camera tracks
// ---------------------------------------------------------------------------

mmd::Float3 App::defaultCameraFocus() const {
    return {camera_.target.x, camera_.target.y, camera_.target.z};
}

void App::setCameraTrack(std::optional<CameraTrack> track, std::filesystem::path path) {
    if (!track && !cameraTrack_ && !cameraView_)
        return; // the default camera stays as it is
    cameraFadeFrom_ = cameraView_ ? *cameraView_ : neutralCameraView(defaultCameraFocus());
    cameraFadeStart_ = qpcSeconds(qpcNow());
    cameraFading_ = true;
    cameraTrack_ = std::move(track);
    cameraTrackPath_ = std::move(path);
    if (cameraTrack_)
        log::info("カメラトラック: {}（{}、キー {} 個）", pathToUtf8(cameraTrackPath_),
                  cameraTrack_->isVmd() ? "MMD カメラ" : "JSON", cameraTrack_->keyCount());
    viewDirty_ = true;
}

// Samples the track at the current motion frame (focus bones in their current
// pose) and blends in from the previous view after a motion switch.
void App::updateCameraView() {
    if (!cameraTrack_ && !cameraFading_) {
        cameraView_.reset();
        return;
    }
    const mmd::Float3 fallback = defaultCameraFocus();
    CameraView view = neutralCameraView(fallback);
    if (cameraTrack_) {
        view = cameraTrack_->sample(playback_.frame(), fallback,
                                    [this](const CameraKey &key) -> std::optional<mmd::Float3> {
                                        if (key.focus)
                                            return key.focus;
                                        if (key.focusBone.empty())
                                            return std::nullopt;
                                        const auto bone = model_->findBone(key.focusBone);
                                        if (!bone)
                                            return std::nullopt; // models differ: use the default look-at point
                                        const auto p = model_->bonePosition(*bone, true);
                                        return mmd::Float3{p[0] + key.focusOffset[0], p[1] + key.focusOffset[1],
                                                           p[2] + key.focusOffset[2]};
                                    });
    }
    if (cameraFading_) {
        const double t = (qpcSeconds(qpcNow()) - cameraFadeStart_) / kCameraFadeSeconds;
        if (t >= 1.0)
            cameraFading_ = false;
        else
            view = blendCameraViews(cameraFadeFrom_, view, applyCameraEase(CameraEase::smooth, static_cast<float>(t)));
    }
    if (!cameraTrack_ && !cameraFading_)
        cameraView_.reset();
    else
        cameraView_ = view;
}

// ---------------------------------------------------------------------------
// Automatic face: blink and gaze
// ---------------------------------------------------------------------------

void App::bindFace() {
    blinkMorph_.reset();
    eyeClosingMorphs_.clear();
    if (!blinkMorphName_.empty())
        blinkMorph_ = model_->findMorph(blinkMorphName_);
    for (const char *name : {"まばたき", "瞬き", "blink", "Blink"})
        if (!blinkMorph_)
            blinkMorph_ = model_->findMorph(name);
    // Morphs that already close the eyes: no blink on top of them.
    const auto &morphs = model_->model().morphs;
    for (std::size_t i = 0; i < morphs.size(); ++i) {
        const std::string &n = morphs[i].name;
        for (const char *part : {"まばたき", "瞬き", "ウィンク", "ｳｨﾝｸ", "笑い", "はぅ", "なごみ", "目閉じ", "blink", "wink"})
            if (n.find(part) != std::string::npos) {
                eyeClosingMorphs_.push_back(i);
                break;
            }
    }
    const auto bone = [&](std::initializer_list<const char *> names) -> std::optional<std::size_t> {
        for (const char *name : names)
            if (const auto found = model_->findBone(name))
                return found;
        return std::nullopt;
    };
    neckBone_ = bone({"首", "neck"});
    headBone_ = bone({"頭", "head"});
    eyesBone_ = bone({"両目", "eyes"});
    leftEyeBone_ = bone({"左目", "eye_L"});
    rightEyeBone_ = bone({"右目", "eye_R"});
    gaze_.reset();
    model_->setAutoMorph(std::nullopt, 0.0F);
    model_->setGazeRotations({});
    lastBlinkWeight_ = 0.0F;
    log::info("自動の表情: まばたき={} 視線=頭{} 首{} 目{}",
              blinkMorph_ ? model_->model().morphs[*blinkMorph_].name : std::string("（モーフなし）"),
              headBone_ ? "あり" : "なし", neckBone_ ? "あり" : "なし",
              (eyesBone_ || leftEyeBone_ || rightEyeBone_) ? "あり" : "なし");
}

// Advances the blink and the gaze. Returns true if the pose needs re-evaluating.
bool App::updateFace(double dt, float frame) {
    bool changed = false;

    // Blink: unless the motion or an expression already closes the eyes.
    float blinkWeight = 0.0F;
    if (blinkOn_ && blinkMorph_) {
        bool eyesClosed = false;
        for (const auto morph : eyeClosingMorphs_) {
            const float w = std::max(model_->overrideWeight(morph).value_or(0.0F), model_->motionMorphWeight(morph, frame));
            if (w > 0.3F) {
                eyesClosed = true;
                break;
            }
        }
        if (eyesClosed)
            blink_.hold();
        else
            blinkWeight = blink_.update(dt);
    }
    if (blinkWeight != lastBlinkWeight_) {
        model_->setAutoMorph(blinkMorph_, blinkWeight);
        lastBlinkWeight_ = blinkWeight;
        changed = true;
    }

    // Gaze: follow the cursor while it is near the head.
    if (!headBone_ || !window_ || !d3d_)
        return changed;
    bool active = false;
    facemath::Quat wanted = facemath::kIdentity;
    if (gazeOn_ && !grabbed_ && !walk_ && window_->hwnd() != nullptr && IsWindowVisible(window_->hwnd())) {
        POINT cursor{};
        if (GetCursorPos(&cursor) && ScreenToClient(window_->hwnd(), &cursor)) {
            const float width = static_cast<float>(d3d_->width());
            const float height = static_cast<float>(d3d_->height());
            const Camera camera = behaviorCamera();
            const std::size_t eyeBone = eyesBone_ ? *eyesBone_ : *headBone_;
            const mmd::Float3 eyes = model_->bonePosition(eyeBone, true);
            const auto head = camera.project(eyes, width, height);
            const float dpiScale = static_cast<float>(GetDpiForWindow(window_->hwnd())) / 96.0F;
            const float radius = gaze_.settings().radius > 0.0F ? gaze_.settings().radius * dpiScale : height * 0.6F;
            const float dx = static_cast<float>(cursor.x) - head.x;
            const float dy = static_cast<float>(cursor.y) - head.y;
            active = std::isfinite(head.x) && std::isfinite(head.y) && dx * dx + dy * dy <= radius * radius;
            if (active) {
                const auto ray = camera.ray(static_cast<float>(cursor.x) + 0.5F, static_cast<float>(cursor.y) + 0.5F,
                                            width, height);
                const auto eye = camera.eye();
                const mmd::Float3 target =
                    gazeTarget({eye.x, eye.y, eye.z}, {ray.direction.x, ray.direction.y, ray.direction.z}, eyes);
                const mmd::Float3 toward{target[0] - eyes[0], target[1] - eyes[1], target[2] - eyes[2]};
                // The head as the motion turned it: take out the gaze we added last frame.
                const auto &bones = model_->bones();
                const facemath::Quat headNow =
                    *headBone_ < bones.size() ? bones[*headBone_].rotation : facemath::kIdentity;
                const facemath::Quat base = facemath::multiply(headNow, facemath::conjugate(gaze_.headPart()));
                wanted = gazeRotation(base, toward, gaze_.settings().maxYaw, gaze_.settings().maxPitch);
            }
        }
    }
    if (gaze_.update(dt, active, wanted)) {
        std::vector<std::pair<std::size_t, mmd::Float4>> rotations;
        if (gaze_.weight() > 0.0F) {
            const auto &s = gaze_.settings();
            if (neckBone_)
                rotations.emplace_back(*neckBone_, gaze_.part(s.neckShare));
            rotations.emplace_back(*headBone_, gaze_.part(neckBone_ ? s.headShare : s.headShare + s.neckShare));
            if (eyesBone_) {
                rotations.emplace_back(*eyesBone_, gaze_.part(s.eyeShare));
            } else {
                if (leftEyeBone_)
                    rotations.emplace_back(*leftEyeBone_, gaze_.part(s.eyeShare));
                if (rightEyeBone_)
                    rotations.emplace_back(*rightEyeBone_, gaze_.part(s.eyeShare));
            }
        }
        model_->setGazeRotations(rotations);
        changed = true;
    }
    return changed;
}

ipc::Json App::faceJson() const {
    return {{"blink", blinkOn_},
            {"blinkMorph", blinkMorph_ ? ipc::Json(model_->model().morphs[*blinkMorph_].name) : ipc::Json(nullptr)},
            {"gaze", gazeOn_},
            {"gazeRadius", gaze_.settings().radius},
            {"gazeWeight", gaze_.weight()}};
}

std::optional<App::CameraChoice> App::readCameraChoice(const ipc::Json &params, ipc::Json &errorResponse,
                                                       const std::string &requestId) const {
    CameraChoice choice;
    const auto it = params.find("camera");
    if (it == params.end() || it->is_null())
        return choice;
    if (it->is_boolean()) {
        if (!it->get<bool>())
            choice.mode = CameraChoice::Mode::none;
        return choice;
    }
    if (!it->is_string()) {
        errorResponse = ipc::makeError(requestId, ipc::error::invalidParams,
                                       "camera はカメラトラックのパスか false で指定してください");
        return std::nullopt;
    }
    const auto path = resolveIpcPath(params, "camera", errorResponse, requestId);
    if (!path)
        return std::nullopt;
    choice.mode = CameraChoice::Mode::file;
    choice.path = *path;
    return choice;
}

void App::stopMotion() {
    model_->clearMotion();
    Playback::Config config = playback_.config();
    config.hasMotion = false;
    config.lastFrame = 0.0F;
    playback_.restart(config); // back to the rest pose, physics state kept
    nextMotion_.reset();
    setCameraTrack(std::nullopt, {});
    viewDirty_ = true;
}

ipc::Json App::statusJson() const {
    const auto &pmx = model_->model();
    ipc::Json motion = nullptr;
    if (model_->hasMotion()) {
        motion = {{"path", pathToUtf8(model_->motionPath())},
                  {"frame", playback_.frame()},
                  {"lastFrame", model_->motionLastFrame()},
                  {"loop", playback_.config().loop},
                  {"finished", playback_.finished()}};
        if (nextMotion_)
            motion["next"] = pathToUtf8(nextMotion_->path);
        motion["camera"] = cameraTrack_ ? ipc::Json(pathToUtf8(cameraTrackPath_)) : ipc::Json(nullptr);
    }
    ipc::Json morphs = ipc::Json::object();
    for (const auto &[name, weight] : model_->morphOverrides())
        morphs[name] = weight;
    return {{"pid", GetCurrentProcessId()},
            {"instanceId", options_.ipcInstance},
            {"model",
             {{"name", pmx.metadata.modelName},
              {"path", pathToUtf8(model_->modelPath())},
              {"vertices", pmx.vertices.size()},
              {"morphs", pmx.morphs.size()},
              {"rigidBodies", pmx.rigidBodies.size()}}},
            {"motion", motion},
            {"paused", playback_.paused()},
            {"physics", {{"enabled", model_->physicsEnabled()}, {"status", toString(model_->physicsStatus())}}},
            {"fps", {{"target", options_.fps}, {"measured", measuredFps_}}},
            {"morphOverrides", morphs},
            {"face", faceJson()},
            {"transparent", window_->transparent()}};
}

ipc::Json App::executeCommand(const ipc::Request &request) {
    const std::string &id = request.requestId;
    const std::string &command = request.command;
    const ipc::Json &params = request.params;
    log::info("IPC コマンド: {} {}", command, ipc::dump(params));
    ipc::Json errorResponse;

    if (command == "ping")
        return ipc::makeSuccess(id, {{"pong", true}, {"pid", GetCurrentProcessId()}});
    if (command == "status")
        return ipc::makeSuccess(id, statusJson());
    if (command.ends_with("_accessory") || command.starts_with("set_accessory_") || command == "list_accessories" ||
        command == "get_accessory_status") {
        auto response = accessories_->execute(request);
        if (response.value("success", false) && command != "list_accessories" && command != "get_accessory_status")
            viewDirty_ = true;
        return response;
    }

    if (command == "load_model") {
        const auto path = resolveIpcPath(params, "path", errorResponse, id);
        if (!path)
            return errorResponse;
        try {
            loadNewModel(*path);
        } catch (const std::exception &e) {
            log::error("IPC load_model 失敗: {}", e.what());
            return ipc::makeError(id, ipc::error::loadFailed, e.what());
        }
        return ipc::makeSuccess(id, statusJson());
    }

    if (command == "play_motion") {
        const auto path = resolveIpcPath(params, "path", errorResponse, id);
        if (!path)
            return errorResponse;
        bool loop = true;
        if (const auto it = params.find("loop"); it != params.end()) {
            if (!it->is_boolean())
                return ipc::makeError(id, ipc::error::invalidParams, "loop は true / false で指定してください");
            loop = it->get<bool>();
        }
        const auto camera = readCameraChoice(params, errorResponse, id);
        if (!camera)
            return errorResponse;
        std::optional<QueuedMotion> next;
        if (const auto it = params.find("next"); it != params.end() && !it->is_null()) {
            if (!it->is_object())
                return ipc::makeError(id, ipc::error::invalidParams, "next は {\"path\":..,\"loop\":..} で指定してください");
            const auto nextPath = resolveIpcPath(*it, "path", errorResponse, id);
            if (!nextPath)
                return errorResponse;
            const auto nextCamera = readCameraChoice(*it, errorResponse, id);
            if (!nextCamera)
                return errorResponse;
            QueuedMotion queued{*nextPath, true, *nextCamera};
            if (const auto l = it->find("loop"); l != it->end() && l->is_boolean())
                queued.loop = l->get<bool>();
            next = queued;
        }
        try {
            playMotion(*path, loop, *camera);
        } catch (const std::exception &e) {
            log::error("IPC play_motion 失敗: {}", e.what());
            return ipc::makeError(id, ipc::error::loadFailed, e.what());
        }
        nextMotion_ = loop ? std::nullopt : next;
        return ipc::makeSuccess(id, statusJson());
    }

    if (command == "stop_motion") {
        stopMotion();
        return ipc::makeSuccess(id, statusJson());
    }

    if (command == "set_morph") {
        const auto name = params.find("name");
        if (name == params.end() || !name->is_string() || name->get<std::string>().empty())
            return ipc::makeError(id, ipc::error::invalidParams, "name（モーフ名）が必要です");
        float weight = 1.0F;
        if (const auto it = params.find("weight"); it != params.end()) {
            if (!it->is_number())
                return ipc::makeError(id, ipc::error::invalidParams, "weight は数値（0〜1）で指定してください");
            weight = it->get<float>();
        }
        if (!model_->setMorphWeight(name->get<std::string>(), weight))
            return ipc::makeError(id, ipc::error::notFound, "モデルにモーフがありません: " + name->get<std::string>());
        needEvaluate_ = true;
        return ipc::makeSuccess(id, statusJson());
    }

    if (command == "configure_face") {
        // {"blink": bool, "gaze": bool, "gazeRadius": px, "blinkMorph": name}; omitted = unchanged
        const auto boolean = [&](const char *key, bool &value) -> bool {
            const auto it = params.find(key);
            if (it == params.end() || it->is_null())
                return true;
            if (!it->is_boolean())
                return false;
            value = it->get<bool>();
            return true;
        };
        bool blinkOn = blinkOn_;
        bool gazeOn = gazeOn_;
        if (!boolean("blink", blinkOn) || !boolean("gaze", gazeOn))
            return ipc::makeError(id, ipc::error::invalidParams, "blink / gaze は true か false で指定してください");
        GazeSettings gaze = gaze_.settings();
        if (const auto it = params.find("gazeRadius"); it != params.end() && !it->is_null()) {
            if (!it->is_number() || it->get<double>() < 0.0 || it->get<double>() > 10000.0)
                return ipc::makeError(id, ipc::error::invalidParams, "gazeRadius は 0〜10000（0 = 自動）です");
            gaze.radius = it->get<float>();
        }
        std::string morph = blinkMorphName_;
        if (const auto it = params.find("blinkMorph"); it != params.end() && !it->is_null()) {
            if (!it->is_string())
                return ipc::makeError(id, ipc::error::invalidParams, "blinkMorph はモーフ名で指定してください");
            morph = it->get<std::string>();
        }
        blinkOn_ = blinkOn;
        gazeOn_ = gazeOn;
        gaze.enabled = gazeOn;
        gaze_.setSettings(gaze);
        if (morph != blinkMorphName_) {
            blinkMorphName_ = morph;
            bindFace();
        }
        needEvaluate_ = true;
        return ipc::makeSuccess(id, faceJson());
    }

    if (command == "set_morphs") {
        // {"weights": {"name": w, ...}, "reset": true}: one atomic expression change.
        const auto weights = params.find("weights");
        if (weights == params.end() || !weights->is_object())
            return ipc::makeError(id, ipc::error::invalidParams, "weights（{モーフ名: 重み}）が必要です");
        for (const auto &[name, value] : weights->items())
            if (!value.is_number())
                return ipc::makeError(id, ipc::error::invalidParams, "重みは数値で指定してください: " + name);
        std::string missing;
        for (const auto &[name, value] : weights->items())
            if (!model_->hasMorph(name))
                missing += (missing.empty() ? "" : ", ") + name;
        if (params.value("reset", true))
            model_->resetMorph("");
        for (const auto &[name, value] : weights->items())
            model_->setMorphWeight(name, value.get<float>());
        needEvaluate_ = true;
        ipc::Json result = statusJson();
        if (!missing.empty())
            result["missingMorphs"] = missing; // tolerated: models differ
        return ipc::makeSuccess(id, result);
    }

    if (command == "reset_morph") {
        std::string name;
        if (const auto it = params.find("name"); it != params.end() && !it->is_null()) {
            if (!it->is_string())
                return ipc::makeError(id, ipc::error::invalidParams, "name は文字列で指定してください");
            name = it->get<std::string>();
        }
        model_->resetMorph(name);
        needEvaluate_ = true;
        return ipc::makeSuccess(id, statusJson());
    }

    if (command == "shutdown") {
        requestExit();
        return ipc::makeSuccess(id, {{"shuttingDown", true}});
    }

    return executeBehavior(request);
}

RECT App::shellRect() const {
    RECT rect{};
    GetWindowRect(window_->hwnd(), &rect);
    return rect;
}

ipc::Json App::behaviorStatus() const {
    const RECT rect = shellRect();
    ipc::Json result{{"state", toString(grabbed_ ? BehaviorState::transition : behavior_)}, {"autoEnabled", autoBehavior_},
                     {"crossMonitors", crossMonitors_}, {"topmost", window_->topmost()},
                     {"grabbed", grabbed_},
                     {"x", rect.left}, {"y", rect.top}};
    const RECT work = workArea({rect.left + (rect.right - rect.left) / 2,
                                rect.top + (rect.bottom - rect.top) / 2});
    result["monitorWork"] = {work.left, work.top, work.right, work.bottom};
    result["dpi"] = GetDpiForWindow(window_->hwnd());
    if (walk_)
        result["destination"] = {{"x", walk_->destination.x}, {"y", walk_->destination.y}, {"speed", walk_->speed}};
    if (seat_) {
        result["seat"] = {{"target", std::to_string(reinterpret_cast<std::uintptr_t>(seat_->window.hwnd))},
                          {"position", toString(seat_->position)},
                          {"edgeX", seat_->edge.x}, {"edgeY", seat_->edge.y},
                          {"anchor", seat_->hip ? "bone" : seatAnchor_.mode()},
                          {"landed", seat_->landed}};
    }
    return result;
}

bool App::readBehaviorOptions(const ipc::Request &request, BehaviorOptions &options, ipc::Json &error) {
    for (const auto &[key, value, minimum, maximum] :
         {std::tuple{"speed", &options.speed, 1.0, 3000.0},
          std::tuple{"waitSeconds", &options.waitSeconds, 2.0, 3600.0},
          std::tuple{"sitChance", &options.sitChance, 0.0, 1.0}}) {
        const auto it = request.params.find(key);
        if (it == request.params.end())
            continue;
        if (!it->is_number() || !std::isfinite(it->get<double>()) ||
            it->get<double>() < minimum || it->get<double>() > maximum) {
            error = ipc::makeError(request.requestId, ipc::error::invalidParams,
                                   std::format("{} は {}〜{} の数値で指定してください", key, minimum, maximum));
            return false;
        }
        *value = it->get<double>();
    }
    if (const auto it = request.params.find("useRootMotion"); it != request.params.end()) {
        if (!it->is_boolean()) {
            error = ipc::makeError(request.requestId, ipc::error::invalidParams, "useRootMotion は真偽値です");
            return false;
        }
        options.useRootMotion = it->get<bool>();
    }
    for (const auto &[key, path] : {std::pair{"walkMotion", &options.walkMotion},
                                    std::pair{"sitMotion", &options.sitMotion},
                                    std::pair{"idleMotion", &options.idleMotion}}) {
        const auto it = request.params.find(key);
        if (it == request.params.end())
            continue;
        if (it->is_null()) {
            path->clear();
            continue;
        }
        auto resolved = resolveIpcPath(request.params, key, error, request.requestId, true);
        if (!resolved)
            return false;
        *path = std::move(*resolved);
    }
    return true;
}

ipc::Json App::executeBehavior(const ipc::Request &request) {
    const auto &id = request.requestId;
    const auto &command = request.command;
    const auto &params = request.params;
    const auto invalid = [&](const std::string &message) {
        return ipc::makeError(id, ipc::error::invalidParams, message);
    };
    if (command == "get_behavior_status")
        return ipc::makeSuccess(id, behaviorStatus());
    if (command == "configure_seat")
        return configureSeat(request);
    if (command != "move_to" && command != "stop_move" && command != "sit_on_window" &&
        command != "stand_up" && command != "set_auto_behavior" && command != "set_cross_monitor" &&
        command != "set_topmost" && command != "configure_grab" && command != "list_windows")
        return ipc::makeError(id, ipc::error::unknownCommand, "不明なコマンドです: " + command);
    if (!desktopWindows_)
        return invalid("デスクトップ行動には背景透過モードが必要です");

    if (command == "configure_grab") {
        int holdMs = 180;
        if (const auto it = params.find("holdMs"); it != params.end()) {
            if (!it->is_number_integer() || it->get<double>() < 80.0 || it->get<double>() > 1000.0)
                return invalid("holdMs は 80〜1000 の整数です");
            holdMs = it->get<int>();
        }
        std::filesystem::path grab;
        std::filesystem::path idle;
        ipc::Json error;
        for (const auto &[key, path] : {std::pair{"motion", &grab}, std::pair{"idleMotion", &idle}}) {
            if (!params.contains(key) || params[key].is_null())
                continue;
            auto resolved = resolveIpcPath(params, key, error, id, true);
            if (!resolved)
                return error;
            *path = std::move(*resolved);
        }
        grabMotion_ = std::move(grab);
        grabIdleMotion_ = std::move(idle);
        window_->setGrabDelay(grabMotion_.empty() ? 0 : static_cast<UINT>(holdMs));
        return ipc::makeSuccess(id, behaviorStatus());
    }

    if (command == "list_windows") {
        auto result = ipc::Json::array();
        for (const auto &window : desktopWindows_->list()) {
            wchar_t title[256]{};
            GetWindowTextW(window.hwnd, title, static_cast<int>(std::size(title)));
            result.push_back({{"target", std::to_string(reinterpret_cast<std::uintptr_t>(window.hwnd))},
                              {"title", wideToUtf8(title)}, {"pid", window.pid}});
        }
        return ipc::makeSuccess(id, {{"windows", std::move(result)}});
    }
    if (command == "stop_move" || command == "stand_up") {
        if ((command == "stop_move" && walk_) || (command == "stand_up" && seat_))
            finishBehavior(command == "stand_up");
        return ipc::makeSuccess(id, behaviorStatus());
    }
    if (command == "set_cross_monitor" || command == "set_topmost") {
        const auto enabled = params.find("enabled");
        if (enabled == params.end() || !enabled->is_boolean())
            return invalid("enabled は真偽値で指定してください");
        if (command == "set_topmost") {
            window_->setTopmost(enabled->get<bool>());
            return ipc::makeSuccess(id, behaviorStatus());
        }
        crossMonitors_ = enabled->get<bool>();
        if (!crossMonitors_ && walk_) {
            const RECT rect = shellRect();
            const SIZE size{rect.right - rect.left, rect.bottom - rect.top};
            const RECT work = workArea({rect.left + size.cx / 2, rect.top + size.cy / 2});
            const POINT destination{std::lround(walk_->destination.x), std::lround(walk_->destination.y)};
            const POINT clamped = clampToWorkArea(destination, size, work);
            walk_->destination = {static_cast<double>(clamped.x), static_cast<double>(clamped.y)};
        }
        return ipc::makeSuccess(id, behaviorStatus());
    }

    BehaviorOptions options = autoOptions_;
    ipc::Json error;
    ipc::Request optionsRequest = request;
    if (command == "sit_on_window" && !params.contains("sitMotion") && params.contains("motion"))
        optionsRequest.params["sitMotion"] = params["motion"];
    if (!readBehaviorOptions(optionsRequest, options, error))
        return error;

    if (command == "set_auto_behavior") {
        const auto enabled = params.find("enabled");
        if (enabled == params.end() || !enabled->is_boolean())
            return invalid("enabled は真偽値で指定してください");
        autoOptions_ = std::move(options);
        autoBehavior_ = enabled->get<bool>();
        if (!autoBehavior_ && autoAction_)
            finishBehavior(true);
        nextAutoAction_ = qpcSeconds(qpcNow()) + autoOptions_.waitSeconds;
    } else if (command == "move_to") {
        const RECT rect = shellRect();
        const SIZE size{rect.right - rect.left, rect.bottom - rect.top};
        const RECT currentWork = workArea({rect.left + size.cx / 2, rect.top + size.cy / 2});
        POINT destination{rect.left, rect.top};
        if (params.contains("nextMonitor") && !params["nextMonitor"].is_boolean())
            return invalid("nextMonitor は真偽値で指定してください");
        if (params.value("nextMonitor", false)) {
            if (!crossMonitors_ || params.contains("x") || params.contains("y"))
                return invalid("別の画面へ歩くにはディスプレイまたぎを ON にしてください");
            const auto areas = desktopWorkAreas();
            const RECT *nearest = nullptr;
            double distance = 0.0;
            for (const RECT &area : areas) {
                if (EqualRect(&area, &currentWork))
                    continue;
                const double d = std::hypot((area.left + area.right - currentWork.left - currentWork.right) / 2.0,
                                            (area.top + area.bottom - currentWork.top - currentWork.bottom) / 2.0);
                if (!nearest || d < distance) {
                    nearest = &area;
                    distance = d;
                }
            }
            if (!nearest)
                return ipc::makeError(id, ipc::error::notFound, "別のディスプレイが見つかりません");
            destination = clampToWorkArea({(nearest->left + nearest->right - size.cx) / 2,
                                           (nearest->top + nearest->bottom - size.cy) / 2}, size, *nearest);
        } else if (params.contains("x") || params.contains("y")) {
            for (const char *key : {"x", "y"}) {
                const auto it = params.find(key);
                if (it == params.end() || !it->is_number() || !std::isfinite(it->get<double>()) ||
                    std::abs(it->get<double>()) > 1000000.0)
                    return invalid("x と y は画面座標の数値で指定してください");
            }
            destination = {std::lround(params["x"].get<double>()), std::lround(params["y"].get<double>())};
            destination = clampToWorkArea(destination, size, crossMonitors_ ? workArea(destination) : currentWork);
        } else {
            destination.x += walkDirection_ * 240;
            auto clamped = clampToWorkArea(destination, size, currentWork);
            if (clamped.x == rect.left) {
                destination.x = rect.left - walkDirection_ * 240;
                clamped = clampToWorkArea(destination, size, currentWork);
            }
            destination = clamped;
        }
        startWalk(destination, options, false);
    } else {
        const auto targetValue = params.find("target");
        if (targetValue != params.end() && !targetValue->is_string())
            return invalid("target は foreground または list_windows の対象 ID です");
        const std::string target = params.value("target", std::string("foreground"));
        std::optional<DesktopWindow> window;
        if (target == "foreground") {
            window = desktopWindows_->foreground();
        } else {
            std::uintptr_t handle{};
            const auto [end, ec] = std::from_chars(target.data(), target.data() + target.size(), handle);
            if (ec != std::errc{} || end != target.data() + target.size() || handle == 0)
                return invalid("target は foreground または list_windows の対象 ID です");
            window = desktopWindows_->inspect(reinterpret_cast<HWND>(handle));
        }
        if (!window)
            return ipc::makeError(id, ipc::error::notFound, "着席できるウィンドウが見つかりません");
        if ((params.contains("position") && !params["position"].is_string()) ||
            (params.contains("anchorBone") && !params["anchorBone"].is_string()))
            return invalid("position と anchorBone は文字列で指定してください");
        const std::string alignment = params.value("position", std::string("top-center"));
        SeatPosition position;
        if (alignment == "top-left")
            position = SeatPosition::left;
        else if (alignment == "top-center")
            position = SeatPosition::center;
        else if (alignment == "top-right")
            position = SeatPosition::right;
        else
            return invalid("position は top-left / top-center / top-right で指定してください");
        const auto anchorName = params.value("anchorBone", std::string{});
        const auto hip = model_->findBone(anchorName);
        if (!anchorName.empty() && !hip)
            return ipc::makeError(id, ipc::error::notFound, "着席基準のボーンが見つかりません: " + anchorName);
        float offset = 0.0F;
        if (const auto it = params.find("anchorOffsetY"); it != params.end()) {
            if (!it->is_number() || !std::isfinite(it->get<float>()) || std::abs(it->get<float>()) > 10000.0F)
                return invalid("anchorOffsetY はモデル座標の数値で指定してください");
            offset = it->get<float>();
        }
        startSeat(*window, position, hip, offset, options, false);
    }
    return ipc::makeSuccess(id, behaviorStatus());
}

void App::playBehaviorMotion(const std::filesystem::path &path) {
    nextMotion_.reset();
    playback_.setPaused(false);
    if (!fading_)
        beginPoseFade(kBehaviorFadeSeconds);
    if (!path.empty()) {
        try {
            if (options_.allowedRoots.empty()) {
                playMotion(path, true);
                return;
            }
            if (const auto allowed = ipc::resolveWithinRoots(path, options_.allowedRoots.front(), options_.allowedRoots)) {
                playMotion(*allowed, true);
                return;
            }
            log::warn("許可フォルダ外の行動用 VMD は読み込みません");
        } catch (const LoadError &e) {
            log::warn("行動用 VMD を読み込めないため基本姿勢に切り替えます: {}", e.what());
        }
    }
    stopMotion();
}

void App::startWalk(POINT destination, const BehaviorOptions &options, bool automatic) {
    glide_.reset();
    if (seat_) {
        window_->setSeatForeground(false);
        desktopWindows_->watch(std::nullopt);
        seat_.reset();
    }
    const RECT rect = shellRect();
    desktopManaged_ = true;
    window_->setDragTarget(nullptr);
    activeBehavior_ = options;
    autoAction_ = automatic;
    behaviorUpdated_ = qpcSeconds(qpcNow());
    walk_ = DesktopWalk{{static_cast<double>(rect.left), static_cast<double>(rect.top)},
                        {static_cast<double>(destination.x), static_cast<double>(destination.y)}, options.speed};
    if (destination.x != rect.left)
        walkDirection_ = destination.x < rect.left ? -1 : 1;
    rootBone_ = model_->findBone("センター");
    if (!rootBone_)
        rootBone_ = model_->findBone("center");
    if (!rootBone_)
        rootBone_ = model_->findBone("全ての親");
    if (!rootBone_)
        rootBone_ = model_->findBone("root");
    if (destination.x == rect.left && destination.y == rect.top) {
        finishBehavior(false);
        return;
    }
    behavior_ = BehaviorState::walking;
    log::info("歩行開始: ({}, {}) → ({}, {}) 速度={:.1f}px/s",
              rect.left, rect.top, destination.x, destination.y, options.speed);
    playBehaviorMotion(options.walkMotion);
}

void App::startSeat(const DesktopWindow &target, SeatPosition position, std::optional<std::size_t> hip,
                    float offset, const BehaviorOptions &options, bool automatic) {
    const RECT rect = shellRect();
    POINT returnPosition = seat_ ? seat_->returnPosition : POINT{rect.left, rect.top};
    if (glide_) {
        returnPosition = glide_->to; // sat down again while still returning
        glide_.reset();
    }
    // An explicit anchor bone keeps the Phase 2 behaviour (joint height); without
    // one the configured seat anchor (body surface) is used.
    walk_.reset();
    desktopManaged_ = true;
    window_->setDragTarget(nullptr);
    activeBehavior_ = options;
    autoAction_ = automatic;
    const UINT dpi = GetDpiForWindow(target.hwnd);
    const POINT edge = seatPoint(target.frame, position, MulDiv(seatSettings_.insetPixels, dpi ? dpi : 96, 96));
    const double transition = seatSettings_.transitionMs / 1000.0;
    seat_ = Seat{target, position, hip, offset, returnPosition, edge, workArea(edge)};
    seat_->origin = seat_->hopFrom = {rect.left, rect.top};
    seat_->hopStart = qpcSeconds(qpcNow());
    seat_->landed = transition <= 0.0;
    seatNdcX_ = seatNdcY_ = 0.0F;
    beginPoseFade(transition);
    desktopWindows_->watch(target);
    seatChecked_ = qpcSeconds(qpcNow());
    nextAutoAction_ = seatChecked_ + autoOptions_.waitSeconds;
    behavior_ = BehaviorState::transition;
    window_->setSeatForeground(true);
    log::info("着席開始: {} 上端=({}, {}) 基準={}", toString(position), edge.x, edge.y,
              hip ? model_->model().bones[*hip].name : std::string(seatAnchor_.mode()));
    playBehaviorMotion(options.sitMotion);
    anchorPrevious_ = anchorCurrent_ = seatAnchorPoint();
}

void App::finishBehavior(bool returnFromSeat) {
    if (seat_) {
        log::info("着席解除");
        window_->setSeatForeground(false);
        if (returnFromSeat) {
            const RECT rect = shellRect();
            const POINT origin = seat_->returnPosition;
            const POINT destination =
                clampToWorkArea(origin, {rect.right - rect.left, rect.bottom - rect.top}, workArea(origin));
            const double seconds = seatSettings_.transitionMs / 1000.0;
            if (seconds > 0.0)
                glide_ = Glide{{rect.left, rect.top}, destination, qpcSeconds(qpcNow()), seconds};
            else
                window_->moveTo(destination);
            beginPoseFade(seconds);
        }
        desktopWindows_->watch(std::nullopt);
        seat_.reset();
        seatNdcX_ = seatNdcY_ = 0.0F;
    }
    walk_.reset();
    autoAction_ = false;
    behavior_ = BehaviorState::transition;
    nextAutoAction_ = qpcSeconds(qpcNow()) + autoOptions_.waitSeconds;
    playBehaviorMotion(activeBehavior_.idleMotion);
}

Camera App::behaviorCamera(bool withSeatOffset, bool withTrack) const {
    Camera camera = camera_;
    if (withTrack && cameraView_) {
        // The track moves the camera relative to the default one: the focus
        // takes the place of the default look-at point.
        const CameraView &v = *cameraView_;
        camera.target.x += v.focus[0] - camera_.target.x;
        camera.target.y += v.focus[1] - camera_.target.y;
        camera.target.z += v.focus[2] - camera_.target.z;
        camera.distance /= std::max(0.05F, v.zoom);
        camera.yaw += DirectX::XMConvertToRadians(v.yawDegrees);
        camera.pitch = std::clamp(camera.pitch + DirectX::XMConvertToRadians(v.pitchDegrees), -1.45F, 1.45F);
        camera.roll += DirectX::XMConvertToRadians(v.rollDegrees);
        if (v.fovDegrees > 0.0F)
            camera.fovY = DirectX::XMConvertToRadians(v.fovDegrees);
    }
    if (seat_) {
        camera.yaw += DirectX::XMConvertToRadians(seatSettings_.yawDegrees);
        if (withSeatOffset) {
            camera.ndcOffsetX = seatNdcX_;
            camera.ndcOffsetY = seatNdcY_;
        }
    }
    if (!walk_)
        return camera;
    camera.yaw += walkDirection_ * DirectX::XMConvertToRadians(65.0F);
    if (!activeBehavior_.useRootMotion && rootBone_) {
        const auto bind = model_->bonePosition(*rootBone_, false);
        const auto posed = model_->bonePosition(*rootBone_, true);
        camera.target.x += posed[0] - bind[0];
        camera.target.z += posed[2] - bind[2];
    }
    return camera;
}

// ---------------------------------------------------------------------------
// Displayed pose
// ---------------------------------------------------------------------------

void App::beginPoseFade(double seconds) {
    if (seconds <= 0.0 || !poseBlender_ || displayBones_.size() != model_->model().bones.size()) {
        fading_ = false;
        return;
    }
    fadeFrom_ = displayBones_;
    fadeStart_ = qpcSeconds(qpcNow());
    fadeSeconds_ = seconds;
    fading_ = true;
}

// Skins and hands one evaluated tick to the renderer. During a motion switch
// the shown pose is a cross-fade from the pose at the moment of the switch.
void App::presentPose(const mmd::AnimatedModelFrame &frame, bool discontinuous) {
    previousDisplayBones_ = displayBones_;
    if (!model_->poseOnlySupported()) {
        renderer_->setFrame(frame, discontinuous); // libmmd skinned the vertices (soft bodies)
        displayBones_ = frame.bones;
    } else {
        const std::vector<BoneTransform> *bones = &frame.bones;
        if (fading_) {
            const double t = (qpcSeconds(qpcNow()) - fadeStart_) / fadeSeconds_;
            if (discontinuous || t >= 1.0 || fadeFrom_.size() != frame.bones.size()) {
                fading_ = false;
            } else {
                poseBlender_->blend(fadeFrom_, frame.bones, static_cast<float>(smoothStep(t)), blendScratch_);
                bones = &blendScratch_;
            }
        }
        displayBones_ = *bones;
        model_->skinner().skin(displayBones_, frame.morphWeights, renderer_->beginFrame());
        renderer_->commitFrame(frame, discontinuous);
    }
    if (discontinuous)
        previousDisplayBones_.clear();
    if (seat_) {
        anchorPrevious_ = discontinuous ? seatAnchorPoint() : anchorCurrent_;
        anchorCurrent_ = seatAnchorPoint();
    }
}

void App::bindSeatAnchor() {
    bindFace();
    seatAnchor_.bind(model_->model(), seatSettings_);
    poseBlender_ = std::make_unique<PoseBlender>(model_->model());
    displayBones_.clear();
    fading_ = false;
    std::string bones;
    for (const auto &name : seatAnchor_.contactBoneNames())
        bones += (bones.empty() ? "" : ", ") + name;
    log::info("着席基準: 方式={} 基準={} 接地ボーン=[{}] 代表点={}", seatAnchor_.mode(), seatAnchor_.anchorName(), bones,
              seatAnchor_.contactPointCount());
}

// Where the seat reference appears in the window (client pixels, before the
// seat offset): x from the anchor, y from whichever contact point looks lowest.
DirectX::XMFLOAT2 App::seatAnchorPoint() const {
    const float width = static_cast<float>(d3d_->width());
    const float height = static_cast<float>(d3d_->height());
    // The seat stays put while a camera track plays: anchor with the default camera.
    const Camera camera = behaviorCamera(false, false);
    const auto &bones = displayBones_.empty() ? model_->bones() : displayBones_;
    mmd::Float3 anchor{camera_.target.x, camera_.target.y, camera_.target.z};
    const bool boneOnly = seat_ && seat_->hip;
    if (boneOnly)
        anchor = jointPosition(model_->model(), bones, *seat_->hip);
    else if (std::string_view(seatAnchor_.mode()) != "none")
        anchor = seatAnchor_.evaluate(model_->model(), bones);
    if (seat_)
        anchor[1] += seat_->offset;
    DirectX::XMFLOAT2 pixel = camera.project(anchor, width, height);
    if (!boneOnly && seatAnchor_.contactPointCount() > 0) {
        float lowest = -std::numeric_limits<float>::infinity();
        seatAnchor_.forEachContact(bones, [&](mmd::Float3 point) {
            if (seat_)
                point[1] += seat_->offset;
            const float y = camera.project(point, width, height).y;
            if (std::isfinite(y))
                lowest = std::max(lowest, y);
        });
        if (std::isfinite(lowest))
            pixel.y = lowest;
    }
    return pixel;
}

// Keeps the seat anchor on the target's top edge.
//
// The window itself only moves when the target moves (or the pose drifts by
// by a pixel). The remaining fraction is absorbed by shifting the
// projection, which is sub-pixel accurate and always in step with the frame
// being drawn, so the character neither jitters nor lags behind its window.
void App::placeSeat() {
    const RECT rect = shellRect();
    const SIZE size{rect.right - rect.left, rect.bottom - rect.top};
    const float width = static_cast<float>(d3d_->width());
    const float height = static_cast<float>(d3d_->height());
    const float alpha = playback_.interpolationAlpha();
    const DirectX::XMFLOAT2 pixel{anchorPrevious_.x + (anchorCurrent_.x - anchorPrevious_.x) * alpha,
                                  anchorPrevious_.y + (anchorCurrent_.y - anchorPrevious_.y) * alpha};
    if (!std::isfinite(pixel.x) || !std::isfinite(pixel.y))
        return;
    const float px = std::clamp(pixel.x, 0.0F, static_cast<float>(size.cx));
    const float py = std::clamp(pixel.y, 0.0F, static_cast<float>(size.cy));

    // Exact (fractional) origin that would put the anchor on the edge.
    const float idealX = static_cast<float>(seat_->edge.x) - px;
    const float idealY = static_cast<float>(seat_->edge.y) - py;
    const POINT ideal{std::lround(idealX), std::lround(idealY)};
    const POINT clamped = clampToWorkArea(ideal, size, seat_->work);
    // Follow in whole pixels with a little hysteresis (no flipping between two
    // origins when the pose hovers around x.5); the camera covers the fraction.
    if (seat_->edgeDirty || std::abs(idealX - static_cast<float>(seat_->origin.x)) > 0.75F ||
        std::abs(idealY - static_cast<float>(seat_->origin.y)) > 0.75F) {
        seat_->origin = clamped;
        seat_->edgeDirty = false;
    }

    // Hop from the previous position onto the seat.
    double progress = 1.0;
    POINT origin = seat_->origin;
    if (!seat_->landed) {
        const double seconds = std::max(0.001, seatSettings_.transitionMs / 1000.0);
        const double t = (qpcSeconds(qpcNow()) - seat_->hopStart) / seconds;
        if (t >= 1.0) {
            seat_->landed = true;
        } else {
            progress = smoothStep(t);
            const double dx = static_cast<double>(seat_->origin.x - seat_->hopFrom.x);
            const double dy = static_cast<double>(seat_->origin.y - seat_->hopFrom.y);
            const double arc = std::min(0.25 * std::hypot(dx, dy), 0.35 * static_cast<double>(size.cy));
            origin.x = std::lround(seat_->hopFrom.x + dx * progress);
            origin.y = std::lround(seat_->hopFrom.y + dy * progress - arc * 4.0 * progress * (1.0 - progress));
        }
    }
    if (origin.x != rect.left || origin.y != rect.top)
        window_->moveTo(origin);

    // Residual between the resting origin and the exact position. Zero on an
    // axis where the window is held inside the screen (e.g. a maximised target).
    float dx = clamped.x == ideal.x ? static_cast<float>(seat_->edge.x - seat_->origin.x) - px : 0.0F;
    float dy = clamped.y == ideal.y ? static_cast<float>(seat_->edge.y - seat_->origin.y) - py : 0.0F;
    const float limit = 2.0F;
    dx = std::clamp(dx, -limit, limit) * static_cast<float>(progress);
    dy = std::clamp(dy, -limit, limit) * static_cast<float>(progress);
    seatNdcX_ = 2.0F * dx / std::max(1.0F, width);
    seatNdcY_ = -2.0F * dy / std::max(1.0F, height);
}

void App::tickGlide(double now) {
    const double t = (now - glide_->start) / std::max(0.001, glide_->seconds);
    const double s = smoothStep(t);
    const RECT rect = shellRect();
    const double dx = static_cast<double>(glide_->to.x - glide_->from.x);
    const double dy = static_cast<double>(glide_->to.y - glide_->from.y);
    const double arc = std::min(0.25 * std::hypot(dx, dy), 0.35 * static_cast<double>(rect.bottom - rect.top));
    POINT origin{std::lround(glide_->from.x + dx * s), std::lround(glide_->from.y + dy * s - arc * 4.0 * s * (1.0 - s))};
    if (t >= 1.0) {
        origin = glide_->to;
        glide_.reset();
    }
    if (origin.x != rect.left || origin.y != rect.top)
        window_->moveTo(origin);
    viewDirty_ = true;
}

// ---------------------------------------------------------------------------
// configure_seat
// ---------------------------------------------------------------------------

ipc::Json App::seatConfigJson() const {
    return {{"mode", seatAnchor_.mode()},
            {"anchor", seatAnchor_.anchorName()},
            {"anchorBone", seatSettings_.anchorBone},
            {"contactBones", seatAnchor_.contactBoneNames()},
            {"contactPoints", seatAnchor_.contactPointCount()},
            {"useContact", seatSettings_.useContact},
            {"offsetX", seatSettings_.offsetX},
            {"offsetY", seatSettings_.offsetY},
            {"yaw", seatSettings_.yawDegrees},
            {"inset", seatSettings_.insetPixels},
            {"transitionMs", seatSettings_.transitionMs}};
}

ipc::Json App::configureSeat(const ipc::Request &request) {
    const auto &id = request.requestId;
    const auto &params = request.params;
    const auto invalid = [&](const std::string &message) {
        return ipc::makeError(id, ipc::error::invalidParams, message);
    };
    if (const auto it = params.find("reset"); it != params.end() && !it->is_boolean())
        return invalid("reset は真偽値で指定してください");
    SeatSettings settings = params.value("reset", false) ? SeatSettings{} : seatSettings_;
    if (const auto it = params.find("anchorBone"); it != params.end()) {
        if (!it->is_string())
            return invalid("anchorBone は文字列で指定してください");
        settings.anchorBone = it->get<std::string>();
        if (!settings.anchorBone.empty() && !model_->findBone(settings.anchorBone))
            return ipc::makeError(id, ipc::error::notFound, "着席基準のボーンが見つかりません: " + settings.anchorBone);
    }
    if (const auto it = params.find("contactBones"); it != params.end()) {
        if (!it->is_array() || it->size() > 32)
            return invalid("contactBones は最大 32 個のボーン名の配列です");
        settings.contactBones.clear();
        for (const auto &entry : *it) {
            if (!entry.is_string() || entry.get<std::string>().empty())
                return invalid("contactBones はボーン名の配列です");
            if (!model_->findBone(entry.get<std::string>()))
                return ipc::makeError(id, ipc::error::notFound,
                                      "接地ボーンが見つかりません: " + entry.get<std::string>());
            settings.contactBones.push_back(entry.get<std::string>());
        }
    }
    if (const auto it = params.find("useContact"); it != params.end()) {
        if (!it->is_boolean())
            return invalid("useContact は真偽値で指定してください");
        settings.useContact = it->get<bool>();
    }
    const auto number = [&](const char *key, double minimum, double maximum, double &value) {
        const auto it = params.find(key);
        if (it == params.end())
            return true;
        if (!it->is_number() || !std::isfinite(it->get<double>()) || it->get<double>() < minimum ||
            it->get<double>() > maximum)
            return false;
        value = it->get<double>();
        return true;
    };
    double offsetX = settings.offsetX, offsetY = settings.offsetY, yaw = settings.yawDegrees;
    double inset = settings.insetPixels, transition = settings.transitionMs;
    if (!number("offsetX", -1000.0, 1000.0, offsetX) || !number("offsetY", -1000.0, 1000.0, offsetY))
        return invalid("offsetX / offsetY はモデル座標の数値（±1000）で指定してください");
    if (!number("yaw", -180.0, 180.0, yaw))
        return invalid("yaw は -180〜180 度で指定してください");
    if (!number("inset", 0.0, 2000.0, inset))
        return invalid("inset は 0〜2000 ピクセルで指定してください");
    if (!number("transitionMs", 0.0, 5000.0, transition))
        return invalid("transitionMs は 0〜5000 で指定してください");
    settings.offsetX = static_cast<float>(offsetX);
    settings.offsetY = static_cast<float>(offsetY);
    settings.yawDegrees = static_cast<float>(yaw);
    settings.insetPixels = static_cast<int>(std::lround(inset));
    settings.transitionMs = static_cast<int>(std::lround(transition));

    seatSettings_ = std::move(settings);
    seatAnchor_.bind(model_->model(), seatSettings_);
    if (seat_) {
        // Apply to the current seat at once so the result can be tuned live.
        const UINT dpi = GetDpiForWindow(seat_->window.hwnd);
        seat_->edge = seatPoint(seat_->window.frame, seat_->position,
                                MulDiv(seatSettings_.insetPixels, dpi ? dpi : 96, 96));
        seat_->edgeDirty = true;
        anchorPrevious_ = anchorCurrent_ = seatAnchorPoint();
    }
    viewDirty_ = true;
    log::info("着席設定: 方式={} 基準={} 代表点={} 補正=({:.2f}, {:.2f}) 向き={:.0f}° 端から={}px 遷移={}ms",
              seatAnchor_.mode(), seatAnchor_.anchorName(), seatAnchor_.contactPointCount(), seatSettings_.offsetX,
              seatSettings_.offsetY, seatSettings_.yawDegrees, seatSettings_.insetPixels, seatSettings_.transitionMs);
    return ipc::makeSuccess(id, seatConfigJson());
}

void App::tickBehavior(double now) {
    if (walk_ && now - behaviorUpdated_ >= 1.0 / 120.0) {
        const bool arrived = walk_->advance(now - behaviorUpdated_);
        behaviorUpdated_ = now;
        POINT origin{std::lround(walk_->position.x), std::lround(walk_->position.y)};
        const RECT rect = shellRect();
        if (arrived)
            origin = clampToWorkArea(origin, {rect.right - rect.left, rect.bottom - rect.top}, workArea(origin));
        if (origin.x != rect.left || origin.y != rect.top)
            window_->moveTo(origin);
        if (arrived) {
            log::info("歩行完了: ({}, {})", origin.x, origin.y);
            finishBehavior(false);
        }
    }
    if (seat_ && now - seatChecked_ >= 1.0 / 60.0 &&
        (desktopWindows_->changed() || now - seatChecked_ >= 0.25)) {
        seatChecked_ = now;
        const auto target = desktopWindows_->inspect(seat_->window.hwnd);
        if (desktopWindows_->destroyed() || !target || target->pid != seat_->window.pid ||
            target->thread != seat_->window.thread) {
            finishBehavior(true);
            return;
        }
        const UINT dpi = GetDpiForWindow(target->hwnd);
        const POINT edge =
            seatPoint(target->frame, seat_->position, MulDiv(seatSettings_.insetPixels, dpi ? dpi : 96, 96));
        seat_->window.frame = target->frame;
        if (edge.x != seat_->edge.x || edge.y != seat_->edge.y) {
            seat_->edge = edge;
            seat_->edgeDirty = true;
            log::debug("着席追従: 上端=({}, {})", edge.x, edge.y);
        }
        const RECT work = workArea(edge);
        if (!EqualRect(&work, &seat_->work)) {
            log::info("着席追従: 別の画面へ移動 上端=({}, {})", edge.x, edge.y);
            seat_->edgeDirty = true;
        }
        seat_->work = work;
        if (seat_->edgeDirty) {
            placeSeat(); // move at once; the next frame refines the sub-pixel offset
            viewDirty_ = true;
        }
        const HWND foreground = GetForegroundWindow();
        DWORD foregroundPid = 0;
        GetWindowThreadProcessId(foreground, &foregroundPid);
        window_->setSeatForeground(foreground == target->hwnd || foreground == window_->hwnd() ||
                                   (foregroundPid && foregroundPid == options_.parentPid));
    }
    if (!autoBehavior_ || window_->dragging() || now < nextAutoAction_)
        return;
    if (seat_ && autoAction_) {
        finishBehavior(true);
        return;
    }
    if (behavior_ != BehaviorState::idle)
        return;
    if (std::bernoulli_distribution(autoOptions_.sitChance)(behaviorRandom_)) {
        if (const auto target = desktopWindows_->foreground()) {
            startSeat(*target, SeatPosition::center, std::nullopt, 0.0F, autoOptions_, true);
            return;
        }
    }
    const RECT rect = shellRect();
    const SIZE size{rect.right - rect.left, rect.bottom - rect.top};
    RECT work = workArea({rect.left + size.cx / 2, rect.top + size.cy / 2});
    if (crossMonitors_) {
        const auto areas = desktopWorkAreas();
        if (!areas.empty())
            work = areas[std::uniform_int_distribution<std::size_t>(0, areas.size() - 1)(behaviorRandom_)];
    }
    const LONG x = std::uniform_int_distribution<LONG>(work.left, std::max(work.left, work.right - size.cx))(behaviorRandom_);
    const LONG y = std::uniform_int_distribution<LONG>(work.top, std::max(work.top, work.bottom - size.cy))(behaviorRandom_);
    startWalk({x, y}, autoOptions_, true);
}

void App::fitCamera() {
    const float aspect = static_cast<float>(d3d_->width()) / static_cast<float>(std::max(1u, d3d_->height()));
    camera_.fitToBounds(model_->bindPoseBounds(), aspect, options_.fitHeight ? 1.02F : 1.08F, options_.fitHeight);
    camera_.zoom(1.0F / std::max(0.2F, options_.zoom));
    viewDirty_ = true;
}

HWND App::findFollowTarget() const {
    struct Search {
        DWORD pid;
        std::wstring className;
        HWND found;
    } search{options_.parentPid, utf8ToWide(options_.followWindowClass), nullptr};
    EnumWindows(
        [](HWND hwnd, LPARAM param) -> BOOL {
            auto *s = reinterpret_cast<Search *>(param);
            DWORD pid = 0;
            GetWindowThreadProcessId(hwnd, &pid);
            if (pid != s->pid || !IsWindowVisible(hwnd))
                return TRUE;
            wchar_t name[256]{};
            GetClassNameW(hwnd, name, 256);
            if (s->className == name) {
                s->found = hwnd;
                return FALSE;
            }
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&search));
    return search.found;
}

// Keeps this window exactly on top of the followed window (SSP's transparent
// shell), so SSP's balloon placement matches the 3D character.
void App::followWindow() {
    if (followTarget_ == nullptr || !IsWindow(followTarget_) || !IsWindowVisible(followTarget_)) {
        const HWND found = findFollowTarget();
        if (found != followTarget_) {
            followTarget_ = found;
            window_->setDragTarget(desktopManaged_ ? nullptr : found);
            window_->setForwardTarget(found);
            if (found != nullptr)
                log::info("ウィンドウ追従: クラス {} のウィンドウを検出", options_.followWindowClass);
        }
        if (followTarget_ == nullptr)
            return;
    }
    RECT target{};
    if (!GetWindowRect(followTarget_, &target) || target.right - target.left < 16 || target.bottom - target.top < 16)
        return;
    RECT current{};
    GetWindowRect(window_->hwnd(), &current);
    if (desktopManaged_) {
        // Leave the controls reachable at the departure point while walking.
        if (walk_)
            return;
        if (current.left != target.left || current.top != target.top)
            SetWindowPos(followTarget_, nullptr, current.left, current.top, 0, 0,
                         SWP_ASYNCWINDOWPOS | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
        return;
    }
    if (EqualRect(&current, &target))
        return;
    const bool resized = (current.right - current.left) != (target.right - target.left) ||
                         (current.bottom - current.top) != (target.bottom - target.top);
    window_->setBounds(target);
    if (resized) {
        log::info("ウィンドウ追従: {}x{} ({},{})", target.right - target.left, target.bottom - target.top, target.left,
                  target.top);
        fitCamera();
    }
}

void App::togglePlay() {
    playback_.setPaused(!playback_.paused());
    viewDirty_ = true;
    log::info("{}", playback_.paused() ? "一時停止" : "再生");
}

void App::togglePhysics() {
    if (!model_->physicsAvailable()) {
        log::info("物理演算は利用できません（{}）", toString(model_->physicsStatus()));
        return;
    }
    const bool enabled = model_->setPhysicsEnabled(!model_->physicsEnabled());
    playback_.setFixedStep(enabled); // re-synchronises at the current frame
    viewDirty_ = true;
}

void App::rewind() {
    playback_.rewind(); // next update: seek to frame 0, physics re-initialised
    viewDirty_ = true;
    log::info("先頭フレームに戻しました");
}

void App::requestExit() {
    if (!exitRequested_)
        log::info("終了要求を受け付けました");
    exitRequested_ = true;
}

void App::showContextMenu(POINT screen) {
    HMENU menu = CreatePopupMenu();
    const bool canPlay = model_->hasMotion() || model_->physicsEnabled();
    AppendMenuW(menu, MF_STRING | (canPlay ? 0 : MF_GRAYED), menuPlayPause,
                playback_.paused() ? L"再生\tSpace" : L"一時停止\tSpace");
    AppendMenuW(menu, MF_STRING | (canPlay ? 0 : MF_GRAYED), menuRewind, L"先頭に戻す\tHome");
    AppendMenuW(menu, MF_STRING | (model_->physicsAvailable() ? 0 : MF_GRAYED) |
                          (model_->physicsEnabled() ? MF_CHECKED : 0),
                menuPhysics, L"物理演算\tP");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (window_->topmost() ? MF_CHECKED : 0), menuTopmost, L"常に最前面");
    if (window_->transparent())
        AppendMenuW(menu, MF_STRING | (clickThroughEnabled_ ? MF_CHECKED : 0), menuClickThrough,
                    L"透明部分をクリック透過");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, menuExit, L"終了\tEsc");

    SetForegroundWindow(window_->hwnd());
    const UINT command = static_cast<UINT>(TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON | TPM_NONOTIFY,
                                                          screen.x, screen.y, 0, window_->hwnd(), nullptr));
    DestroyMenu(menu);
    // The menu loop blocked us; do not feed that time to the simulation.
    clock_.tick();
    switch (command) {
    case menuPlayPause:
        togglePlay();
        break;
    case menuRewind:
        rewind();
        break;
    case menuPhysics:
        togglePhysics();
        break;
    case menuTopmost:
        window_->setTopmost(!window_->topmost());
        break;
    case menuClickThrough:
        clickThroughEnabled_ = !clickThroughEnabled_;
        break;
    case menuExit:
        requestExit();
        break;
    default:
        break;
    }
}

} // namespace

int runApp(const Options &options, const RunEnvironment &environment) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    int result = exitUnexpected;
    {
        App app(options, environment);
        result = app.run();
    }
    if (SUCCEEDED(com))
        CoUninitialize();
    return result;
}

} // namespace uka3d
