// EXT-08 Bus Telemetry Bridge - M2: the smallest possible client.
//
// create(), start the message pump, and print the engine state, frame number,
// simulation time and scenario name once a second from the client's local
// getters. No subscription, no capture, no bus round trip on the print path.
//
// Scope: BTB-CX-1 only. Everything else in docs/prd.md belongs to M3 and later.

#include <core/logging/GlobalLogger.h>
#include <core/logging/LogReportingFactory.h>
#include <infrastructure/SimulationEngineClient.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <exception>
#include <optional>
#include <string>
#include <thread>

namespace {

constexpr const char* kCategory = "n8ro-bridge";
constexpr const char* kUsage =
    "usage: n8ro-bridge --config <SimEngineClient_*> --model-path <dir> --schema-file <name>\n"
    "\n"
    "  --config       client-side sim engine config entry, e.g. SimEngineClient_SharedMemory.\n"
    "                 A SimEngineHost_* entry names the wrong side and will not connect.\n"
    "  --model-path   directory holding the schema and instance database, e.g. C:\\N8RO\\data\\db\n"
    "  --schema-file  schema name inside that database, e.g. N8roSimSchema\n";

struct Options {
    std::string config;
    std::string modelPath;
    std::string schemaFile;
};

// Exit codes. Distinct values so a script can tell a bad invocation from a bad
// configuration without scraping the log.
constexpr int kExitOk = 0;
constexpr int kExitUsage = 2;
constexpr int kExitCreateFailed = 3;
constexpr int kExitUnexpected = 4;

// Returns false and names the offending argument. Never throws: argv parsing is the
// first place an exception would escape main, and the platform contract forbids that
// (CLAUDE.md, hard rule 1).
bool parseOptions(int argc, char** argv, Options& out, std::string& error) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            error = "help";
            return false;
        }

        std::string* target = nullptr;
        if (arg == "--config") {
            target = &out.config;
        } else if (arg == "--model-path") {
            target = &out.modelPath;
        } else if (arg == "--schema-file") {
            target = &out.schemaFile;
        } else {
            error = "unrecognised option " + arg;
            return false;
        }

        if (i + 1 >= argc) {
            error = arg + " requires a value";
            return false;
        }
        *target = argv[++i];
    }

    if (out.config.empty()) {
        error = "--config is required";
        return false;
    }
    if (out.modelPath.empty()) {
        error = "--model-path is required";
        return false;
    }
    if (out.schemaFile.empty()) {
        error = "--schema-file is required";
        return false;
    }
    return true;
}

// create() reports failure as an empty optional and nothing else, so the diagnostic has
// to come from us. Echo all three values back exactly as resolved: [S1] warns the
// difficulty here is configuration, and the common fault - naming a SimEngineHost_*
// entry where a client is required - is only visible when the value is printed.
void reportCreateFailure(const Options& options) {
    N8RO_LOG_ERROR(std::string("SimulationEngineClient::create returned nullopt; no client was "
                               "constructed and nothing was subscribed"),
                   kCategory);
    N8RO_LOG_ERROR(std::string("  --config      = ") + options.config, kCategory);
    N8RO_LOG_ERROR(std::string("  --model-path  = ") + options.modelPath, kCategory);
    N8RO_LOG_ERROR(std::string("  --schema-file = ") + options.schemaFile, kCategory);
    N8RO_LOG_ERROR(std::string("check, in this order: the config entry exists in the model "
                               "database and is a SimEngineClient_* entry, not a SimEngineHost_* "
                               "one; the model path is the directory holding the schema database; "
                               "the schema file name matches the one the engine was started with"),
                   kCategory);
}

int run(const Options& options) {
    n8ro::sim::SimulationEngineClientConfig clientConfig;
    clientConfig.simEngineConfigName = options.config;
    clientConfig.modelPath = options.modelPath;
    clientConfig.schemaFileName = options.schemaFile;

    N8RO_LOG_INFO(std::string("creating client: config=") + options.config + " modelPath=" +
                      options.modelPath + " schemaFile=" + options.schemaFile,
                  kCategory);

    std::optional<n8ro::sim::SimulationEngineClient> client =
        n8ro::sim::SimulationEngineClient::create(clientConfig);
    if (!client) {
        reportCreateFailure(options);
        return kExitCreateFailed;
    }

    // The local getters are fed by the client's own sim/engine/state subscription, which
    // delivers nothing until the pump runs. Until the engine publishes its first frame
    // they read back as empty / zero, which is why the first lines print "(none yet)".
    client->startMessagePump();
    N8RO_LOG_INFO(std::string("message pump started; reporting engine state once a second from "
                              "the local getters (Ctrl-C to stop)"),
                  kCategory);

    for (;;) {
        // Every value below is a local read on this client. Nothing here touches the bus.
        const std::string engineState = client->getEngineState();
        const std::uint64_t frame = client->getFrameNumber();
        const double simTime = client->getSimulationTimeS();
        const std::optional<std::string> scenario = client->getLoadedScenarioName();

        std::printf("engine=%-12s frame=%-10llu simTime=%10.3f scenario=%s\n",
                    engineState.empty() ? "(none yet)" : engineState.c_str(),
                    static_cast<unsigned long long>(frame), simTime,
                    scenario && !scenario->empty() ? scenario->c_str() : "(none)");
        std::fflush(stdout);

        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
}

}  // namespace

int main(int argc, char** argv) {
    // Console-only logger. The rotating-file initializer writes under <N8RO_RELEASE>\logs,
    // and C:\N8RO is read-only for this project (CLAUDE.md, Guardrail).
    n8ro::core::GlobalLogger::setLogger(
        n8ro::core::LogReportingFactory::createLogger<n8ro::core::ConsoleLoggerConfig>());

    // Hard rule 1: no exception escapes main. The try block starts after the logger exists
    // so that a failure still produces a named diagnostic rather than a silent abort.
    try {
        Options options;
        std::string error;
        if (!parseOptions(argc, argv, options, error)) {
            if (error == "help") {
                std::fputs(kUsage, stdout);
                n8ro::core::GlobalLogger::flush();
                return kExitOk;
            }
            N8RO_LOG_ERROR(std::string("bad invocation: ") + error, kCategory);
            std::fputs(kUsage, stderr);
            n8ro::core::GlobalLogger::flush();
            return kExitUsage;
        }

        const int code = run(options);
        n8ro::core::GlobalLogger::flush();
        return code;
    } catch (const std::exception& e) {
        N8RO_LOG_CRITICAL(std::string("unhandled std::exception escaped to main: ") + e.what(),
                          kCategory);
        n8ro::core::GlobalLogger::flush();
        return kExitUnexpected;
    } catch (...) {
        N8RO_LOG_CRITICAL(std::string("unhandled non-std exception escaped to main"), kCategory);
        n8ro::core::GlobalLogger::flush();
        return kExitUnexpected;
    }
}
