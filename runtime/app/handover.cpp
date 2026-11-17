// A title may relaunch itself between the front end and a level (a quick reboot on the Xbox, a new process here).
// The old instance keeps its window up in kernel::Relaunch until this one has presented its first frames, so the
// kept picture stays on screen through the switch. Here the window is created hidden and opens at that point.
#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>

#include <cstdlib>
#endif

#include <atomic>

#include "app/app.h"
#include "core/log.h"
#include "kernel/kernel.h"

namespace app {

namespace {
std::atomic<bool> g_handing{false};
#ifdef _WIN32
HANDLE g_ready = nullptr;
#else
int g_ready = -1;  // kernel::kHandoverFd's pipe
#endif
}  // namespace

#ifdef _WIN32
bool StartHandover() {
    g_ready = OpenEventW(EVENT_MODIFY_STATE, FALSE, kernel::HandoverEvent(GetCurrentProcessId()).c_str());
    g_handing = g_ready != nullptr;
    if (g_handing) LOGI("handover: the previous instance waits for the first frames");
    return g_handing;
}

#else
bool StartHandover() {
    if (const char* fd = std::getenv(kernel::kHandoverFd)) {
        g_ready = std::atoi(fd);
        unsetenv(kernel::kHandoverFd);
    }
    g_handing = g_ready >= 0;
    if (g_handing) LOGI("handover: the previous instance waits for the first frames");
    return g_handing;
}
#endif

bool HandingOver() { return g_handing; }

void FinishHandover() {
    if (!g_handing.exchange(false)) return;
    OpenGameWindow(false);
#ifdef _WIN32
    SetEvent(g_ready);
    CloseHandle(g_ready);
#else
    char c = 1;
    (void)!write(g_ready, &c, 1);
    close(g_ready);
    g_ready = -1;
#endif
}

}  // namespace app
