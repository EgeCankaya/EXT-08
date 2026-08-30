#include "Signals.h"

#include <core/logging/GlobalLogger.h>

#include <chrono>
#include <cstdlib>
#include <string>
#include <thread>

namespace n8ro::bridge {
namespace {

constexpr const char* kCategory = "n8ro-bridge";

// Lock-free by static assertion rather than by assumption. Only a lock-free atomic (or a
// volatile sig_atomic_t) may be touched from a signal handler; if this ever failed to hold on
// some target, the build should stop rather than the program deadlock in the field.
std::atomic<int> gInterrupts{0};
static_assert(std::atomic<int>::is_always_lock_free,
              "the interrupt counter is written from a signal handler and must be lock-free");

std::atomic<bool> gWatchdogRunning{false};

// Everything this does is one atomic increment. No logging, no allocation, no lock, no IO -
// see the header for why that is a correctness requirement and not a preference.
extern "C" void onInterrupt(int) {
    gInterrupts.fetch_add(1, std::memory_order_relaxed);
    // Re-arm. Some platforms reset a handler to SIG_DFL on delivery, which would make the
    // second Ctrl-C a hard kill and lose the tail the first one was about to save.
    std::signal(SIGINT, onInterrupt);
#ifdef SIGBREAK
    std::signal(SIGBREAK, onInterrupt);
#endif
}

}  // namespace

bool installInterruptHandlers() {
    bool ok = std::signal(SIGINT, onInterrupt) != SIG_ERR;
    if (std::signal(SIGTERM, onInterrupt) == SIG_ERR) {
        ok = false;
    }
#ifdef SIGBREAK
    // Ctrl-Break is a separate console event on Windows and an operator pressing it means
    // exactly what Ctrl-C means.
    if (std::signal(SIGBREAK, onInterrupt) == SIG_ERR) {
        ok = false;
    }
#endif
    return ok;
}

int interruptCount() { return gInterrupts.load(std::memory_order_relaxed); }

void startDrainWatchdog(int forcedExitCode) {
    if (gWatchdogRunning.exchange(true)) {
        return;
    }
    std::thread([forcedExitCode] {
        for (;;) {
            if (gInterrupts.load(std::memory_order_relaxed) >= 2) {
                // The warning the handler could not write. Then out, without unwinding: the
                // point of a forced exit is that whatever is stuck stays stuck rather than
                // being waited on.
                N8RO_LOG_WARNING(
                    std::string("second interrupt during drain - forcing exit with code ") +
                        std::to_string(forcedExitCode) +
                        ". The capture may be missing its trailer and the records still in the "
                        "queue; a first interrupt alone would have saved both",
                    kCategory);
                n8ro::core::GlobalLogger::flush();
                std::_Exit(forcedExitCode);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }).detach();
}

bool drainWatchdogRunning() { return gWatchdogRunning.load(); }

}  // namespace n8ro::bridge
