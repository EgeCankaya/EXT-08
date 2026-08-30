// EXT-08 Bus Telemetry Bridge - M7: clean interruption (BTB-SD-1).
//
// The requirement is precise about the division of labour, and it is the right division:
//
//   "The signal handler itself does no work beyond setting a flag - no IO, no allocation,
//    no locking."
//
// That is not style. A signal handler runs asynchronously with respect to everything else in
// the process; allocating, locking or writing from one can deadlock against the very thread
// it interrupted. So the handler here increments one lock-free counter and returns, and every
// consequence of the interrupt happens on the main loop, which is already awake four times a
// second.
//
// The second interrupt is the other half of the requirement: "a second Ctrl-C during drain
// forces exit with a logged warning rather than hanging". A handler cannot log, so the
// warning comes from a watchdog on an ordinary thread - see `startDrainWatchdog`.
//
// Windows note: the CRT's console handler raises SIGINT on a thread of its own, so `signal`
// is the portable spelling and it works here. SIGBREAK is registered too, because Ctrl-Break
// is a distinct event on this platform and an operator who presses it means the same thing.

#pragma once

#include <atomic>
#include <csignal>

namespace n8ro::bridge {

// Registers handlers for SIGINT, SIGTERM and (on Windows) SIGBREAK. Returns false if the
// platform refused one, which is worth logging but is not fatal: a bridge that cannot be
// interrupted cleanly still records correctly, it just has to be ended by the host.
[[nodiscard]] bool installInterruptHandlers();

// How many interrupts have arrived. 0 means none; 1 means "shut down cleanly"; 2 or more
// means the operator asked twice and wants out now.
[[nodiscard]] int interruptCount();

// Starts a detached watchdog for the teardown window. If a second interrupt arrives while the
// queue is draining and the trailer is being written, it logs a named warning and terminates
// the process with `forcedExitCode` rather than letting the operator watch a hang.
//
// Started immediately before teardown and never stopped: the process is on its way out either
// way, and a watchdog that has to be joined is one more thing that can fail to be joined.
void startDrainWatchdog(int forcedExitCode);

// True once the watchdog has been started, so the main path can report whether the tail it
// wrote was written under one.
[[nodiscard]] bool drainWatchdogRunning();

}  // namespace n8ro::bridge
