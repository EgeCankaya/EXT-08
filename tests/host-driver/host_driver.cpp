// EXT-08 - a minimal driver for the headless host, for the R8 spike (M7).
//
// **This is a test tool, not part of the bridge.** The bridge is a passive observer: it
// subscribes and never publishes, and PRD ADR-4 and §14 of the format spec both lean on that.
// Driving a run needs the control direction, which is out of scope for v1 - so it lives here,
// in `tests/`, where it cannot be mistaken for a capability of `n8ro-bridge`.
//
// Why it exists: R8 asks whether the host EXT-17 will actually use publishes the same thing
// twice. `n8ro-sim-local` does not - it paces against the wall clock and skips about 1% of
// frames, a different 1% each run (PRD rev 5/6, format spec §14). The headless
// `n8ro-sim-app.exe` has never been measured, and [S2] states the simulation is deterministic
// by contract. That question decides EXT-17's whole determinism self-test design, so it is
// worth half an hour.
//
// OQ-2 asked what the headless invocation is. Answered by observation:
//
//     n8ro-sim-app.exe --sim-config SimEngineHost_SharedMemory ^
//                      --model-path C:\N8RO\data\db --schema-file N8roSimSchema
//
// It hosts an engine and subscribes to its command topic; it takes **no scenario argument**,
// because loading a scenario is a separate step published on `sim/scenario/command`. That is
// what this driver does.
//
// **The run is bounded by frame number, not by wall-clock time.** That is the entire point:
// a wall-clock budget makes two runs end at different frames, which would guarantee the two
// captures differ for a reason that has nothing to do with determinism.
//
// Build:
//   cl /std:c++17 /EHsc /W4 /O2 ^
//      /I %N8RO_RELEASE%\include\n8ro-core /I %N8RO_RELEASE%\include\n8ro-sim ^
//      /I %N8RO_RELEASE%\include\n8ro-schema /I %N8RO_RELEASE%\include\n8ro-data ^
//      /Fe:host_driver.exe tests\host-driver\host_driver.cpp ^
//      /link /LIBPATH:%N8RO_RELEASE%\lib n8ro-core.lib n8ro-sim.lib n8ro-schema.lib n8ro-data.lib

#include <core/logging/GlobalLogger.h>
#include <core/logging/LogReportingFactory.h>
#include <infrastructure/SimulationEngineClient.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>

namespace {

// Waits up to `timeout` for `predicate`, polling the client's local getters - none of which
// touch the bus. Returns whether it came true.
template <typename Predicate>
bool waitFor(Predicate predicate, std::chrono::milliseconds timeout, const char* what) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (predicate()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    std::printf("timed out waiting for %s\n", what);
    return false;
}

}  // namespace

int main(int argc, char** argv) {
    n8ro::core::GlobalLogger::setLogger(
        n8ro::core::LogReportingFactory::createLogger<n8ro::core::ConsoleLoggerConfig>());

    std::string config = "SimEngineClient_SharedMemory";
    std::string modelPath = "C:\\N8RO\\data\\db";
    std::string schemaFile = "N8roSimSchema";
    std::string scenario;
    unsigned long long frames = 2000;   // 100 s at the platform's 20 Hz frame rate

    for (int i = 1; i + 1 < argc; i += 2) {
        const std::string arg = argv[i];
        if (arg == "--config") config = argv[i + 1];
        else if (arg == "--model-path") modelPath = argv[i + 1];
        else if (arg == "--schema-file") schemaFile = argv[i + 1];
        else if (arg == "--scenario") scenario = argv[i + 1];
        else if (arg == "--frames") frames = std::strtoull(argv[i + 1], nullptr, 10);
    }
    if (scenario.empty()) {
        std::fputs("usage: host_driver --scenario <name> [--config X] [--model-path Y]\n"
                   "                   [--schema-file Z] [--frames N]\n"
                   "\n"
                   "Drives an already-running n8ro-sim-app.exe: loads the scenario, starts the\n"
                   "engine, waits for N frames, then stops it. The run is bounded by FRAME\n"
                   "COUNT, not by wall-clock time, so two runs cover the same simulation.\n",
                   stderr);
        return 2;
    }

    n8ro::sim::SimulationEngineClientConfig clientConfig;
    clientConfig.simEngineConfigName = config;
    clientConfig.modelPath = modelPath;
    clientConfig.schemaFileName = schemaFile;

    std::optional<n8ro::sim::SimulationEngineClient> client =
        n8ro::sim::SimulationEngineClient::create(clientConfig);
    if (!client) {
        std::fputs("SimulationEngineClient::create returned nullopt\n", stderr);
        return 3;
    }
    client->startMessagePump();

    // The host must be up and publishing before a command means anything.
    if (!waitFor([&client] { return !client->getEngineState().empty(); },
                 std::chrono::seconds(30), "the host to publish engine state")) {
        client->stopMessagePump();
        return 4;
    }
    std::printf("host is up: engine=%s\n", client->getEngineState().c_str());

    if (!client->sendScenarioCommand("load_scenario", scenario)) {
        std::fputs("sendScenarioCommand(load_scenario) failed\n", stderr);
        client->stopMessagePump();
        return 5;
    }
    if (!waitFor([&client] { return client->isScenarioLoaded(); }, std::chrono::seconds(60),
                 "the scenario to load")) {
        client->stopMessagePump();
        return 6;
    }
    std::printf("scenario loaded: %s\n",
                client->getLoadedScenarioName().value_or(std::string("(unnamed)")).c_str());

    if (!client->sendEngineCommand("start")) {
        std::fputs("sendEngineCommand(start) failed\n", stderr);
        client->stopMessagePump();
        return 7;
    }
    std::printf("engine started; running to frame %llu\n", frames);

    // Frame count, never a clock. Two runs stopped at the same frame have covered the same
    // simulation; two runs stopped after the same number of seconds have not.
    if (!waitFor([&client, frames] { return client->getFrameNumber() >= frames; },
                 std::chrono::seconds(600), "the frame budget")) {
        static_cast<void>(client->sendEngineCommand("stop"));
        client->stopMessagePump();
        return 8;
    }

    std::printf("reached frame %llu at simTime %.6f; stopping\n",
                static_cast<unsigned long long>(client->getFrameNumber()),
                client->getSimulationTimeS());
    if (!client->sendEngineCommand("stop")) {
        std::fputs("sendEngineCommand(stop) failed\n", stderr);
    }

    // Long enough for the teardown burst to be published and observed by anything watching.
    std::this_thread::sleep_for(std::chrono::seconds(3));
    client->stopMessagePump();
    n8ro::core::GlobalLogger::flush();
    return 0;
}
