#include "platform/SstpEvents.h"

#include "platform/Log.h"
#include "platform/Utf.h"

#include <algorithm>
#include <charconv>
#include <cstring>
#include <format>

namespace uka3d {
namespace {

std::wstring normalizedDirectory(const std::filesystem::path &path) {
    auto result = path.lexically_normal().make_preferred().native();
    while (!result.empty() && result.back() == L'\\')
        result.pop_back();
    return result;
}

HWND readGhostWindow(const std::filesystem::path &ghostDir) {
    constexpr std::size_t maxMappingBytes = 4 * 1024 * 1024;
    const HANDLE mutex = OpenMutexW(SYNCHRONIZE | MUTEX_MODIFY_STATE, FALSE, L"SakuraUnicodeFMO");
    if (!mutex)
        return nullptr;
    HWND result = nullptr;
    const DWORD wait = WaitForSingleObject(mutex, 25);
    if (wait == WAIT_OBJECT_0 || wait == WAIT_ABANDONED) {
        if (const HANDLE map = OpenFileMappingW(FILE_MAP_READ, FALSE, L"SakuraUnicode")) {
            if (const auto *view = static_cast<const char *>(MapViewOfFile(map, FILE_MAP_READ, 0, 0, 0))) {
                MEMORY_BASIC_INFORMATION region{};
                std::uint32_t capacity{};
                if (VirtualQuery(view, &region, sizeof(region)) && region.RegionSize >= sizeof(capacity)) {
                    std::memcpy(&capacity, view, sizeof(capacity));
                    if (capacity > sizeof(capacity) && capacity <= std::min(region.RegionSize, maxMappingBytes)) {
                        const std::string_view payload(view + sizeof(capacity), capacity - sizeof(capacity));
                        result = sstpGhostWindow(payload.substr(0, payload.find('\0')), ghostDir);
                    }
                }
                UnmapViewOfFile(view);
            }
            CloseHandle(map);
        }
        ReleaseMutex(mutex);
    }
    CloseHandle(mutex);
    return result;
}

template <typename Visit>
void visitFmoRows(std::string_view rows, Visit visit) {
    while (!rows.empty()) {
        const auto end = rows.find('\n');
        auto row = rows.substr(0, end);
        if (row.ends_with('\r'))
            row.remove_suffix(1);
        const auto separator = row.find('\1');
        if (separator != std::string_view::npos)
            visit(row.substr(0, separator), row.substr(separator + 1));
        if (end == std::string_view::npos)
            break;
        rows.remove_prefix(end + 1);
    }
}

} // namespace

HWND sstpGhostWindow(std::string_view records, const std::filesystem::path &ghostDir) {
    const auto expected = normalizedDirectory(ghostDir);
    std::string_view ghostId;
    bool ambiguous = false;
    visitFmoRows(records, [&](std::string_view key, std::string_view value) {
        if (!key.ends_with(".ghostpath"))
            return;
        const auto actual = normalizedDirectory(utf8ToPath(value));
        if (CompareStringOrdinal(actual.c_str(), -1, expected.c_str(), -1, TRUE) == CSTR_EQUAL) {
            if (!ghostId.empty())
                ambiguous = true;
            ghostId = key.substr(0, key.size() - std::string_view(".ghostpath").size());
        }
    });
    if (ambiguous || ghostId.empty())
        return nullptr;
    std::uintptr_t handle{};
    visitFmoRows(records, [&](std::string_view key, std::string_view value) {
        if (!key.starts_with(ghostId) || key.substr(ghostId.size()) != ".hwnd")
            return;
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), handle);
        if (error != std::errc{} || end != value.data() + value.size())
            handle = 0;
    });
    return reinterpret_cast<HWND>(handle);
}

std::string_view sstpEventName(MouseEventKind kind) {
    switch (kind) {
    case MouseEventKind::click: return "OnMouseClick";
    case MouseEventKind::doubleClick: return "OnMouseDoubleClick";
    case MouseEventKind::move: return "OnMouseMove";
    case MouseEventKind::pet: return "OnUk3dPet";
    case MouseEventKind::dragStart: return "OnUk3dDragStart";
    case MouseEventKind::dragEnd: return "OnUk3dDragEnd";
    }
    return {};
}

std::string sstpMouseRequest(MouseEventKind kind, const PointerSample &sample) {
    return std::format("SEND SSTP/1.4\r\nCharset: UTF-8\r\nSender: ukagaka3d\r\nEvent: {}\r\n"
                       "Reference0: {}\r\nReference1: {}\r\nReference2: 0\r\nReference3: 0\r\n"
                       "Reference4: {}\r\nReference5: 0\r\nReference6: mouse\r\n\r\n",
                       sstpEventName(kind), sample.x, sample.y, sample.region);
}

bool sendDirectSstp(HWND sender, HWND target, std::string &request) {
    COPYDATASTRUCT data{9801, static_cast<DWORD>(request.size() + 1), request.data()};
    DWORD_PTR result{};
    return SendMessageTimeoutW(target, WM_COPYDATA, reinterpret_cast<WPARAM>(sender),
                               reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG | SMTO_BLOCK, 250, &result) != 0 &&
           result != 0;
}

SstpEvents::SstpEvents(HWND sender, DWORD parentPid, std::filesystem::path ghostDir)
    : sender_(sender), parentPid_(parentPid), ghostDir_(std::move(ghostDir)),
      worker_([this](std::stop_token stop) { run(stop); }) {}

SstpEvents::~SstpEvents() {
    worker_.request_stop();
}

void SstpEvents::enqueue(MouseEventKind kind, const PointerSample &sample) {
    const std::lock_guard lock(mutex_);
    if (kind == MouseEventKind::move && !queue_.empty() && queue_.back().kind == MouseEventKind::move) {
        queue_.back().sample = sample;
        return;
    }
    constexpr std::size_t maxQueued = 32;
    if (queue_.size() == maxQueued) {
        const auto move = std::find_if(queue_.begin(), queue_.end(), [](const Event &event) {
            return event.kind == MouseEventKind::move;
        });
        if (move == queue_.end())
            return;
        queue_.erase(move);
    }
    queue_.push_back({kind, sample});
    ready_.notify_one();
}

void SstpEvents::run(std::stop_token stop) {
    using namespace std::chrono_literals;
    HWND target = nullptr;
    auto nextLookup = std::chrono::steady_clock::time_point::min();
    while (!stop.stop_requested()) {
        Event event;
        {
            std::unique_lock lock(mutex_);
            ready_.wait(lock, stop, [this] { return !queue_.empty(); });
            if (stop.stop_requested())
                break;
            event = std::move(queue_.front());
            queue_.pop_front();
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= nextLookup) {
            target = readGhostWindow(ghostDir_);
            nextLookup = now + 500ms;
        }
        DWORD pid = 0;
        if (target)
            GetWindowThreadProcessId(target, &pid);
        if (pid == 0 || pid != parentPid_)
            continue;
        auto request = sstpMouseRequest(event.kind, event.sample);
        const bool sent = sendDirectSstp(sender_, target, request);
        if (event.kind != MouseEventKind::move)
            log::info("3D 入力 {}: {} ({}, {}) SSTP={}", sstpEventName(event.kind), event.sample.region,
                      event.sample.x, event.sample.y, sent ? "送信" : "失敗");
    }
}

} // namespace uka3d
