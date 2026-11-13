#pragma once

#include "app/Interaction.h"

#include <windows.h>

#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

namespace uka3d {

[[nodiscard]] std::string_view sstpEventName(MouseEventKind kind);
[[nodiscard]] std::string sstpMouseRequest(MouseEventKind kind, const PointerSample &sample);
// FMO data starts after its four-byte capacity field. Only a matching ghost
// directory may supply the destination HWND.
[[nodiscard]] HWND sstpGhostWindow(std::string_view records, const std::filesystem::path &ghostDir);
bool sendDirectSstp(HWND sender, HWND target, std::string &request);

class SstpEvents {
  public:
    SstpEvents(HWND sender, DWORD parentPid, std::filesystem::path ghostDir);
    ~SstpEvents();
    void enqueue(MouseEventKind kind, const PointerSample &sample);

  private:
    void run(std::stop_token stop);

    struct Event {
        MouseEventKind kind;
        PointerSample sample;
    };
    HWND sender_;
    DWORD parentPid_;
    std::filesystem::path ghostDir_;
    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::deque<Event> queue_;
    std::jthread worker_;
};

} // namespace uka3d
