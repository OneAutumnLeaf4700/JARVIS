#include "ai_client.h"
#include "capability_registry.h"
#include "engine.h"
#include "jarvis_service.h"
#include "plugin_config.h"
#include "plugin_loader.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    const std::string serverAddress = "0.0.0.0:50051";
    Engine engine;

    // declared before registry so it outlives it. NOTE: CapabilityRegistry's maps have no
    // internal synchronization. This server dispatches concurrent RPCs on separate threads, so
    // pluginLoader.disablePlugin()/unloadPlugin() (and the unregisterCapability() they call)
    // must never be invoked while requests may be in flight, unless a maintenance path first
    // stops request dispatch — no RPC here does that today.
    PluginLoader pluginLoader;
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    for (const std::string& dir : loadPluginDirs("config/plugin_dirs.cfg")) {
        for (const PluginLoadResult& result : pluginLoader.loadFromDirectory(dir, registry)) {
            if (!result.loaded) {
                spdlog::warn("Plugin '{}' failed to load: {}", result.pluginId, result.reason);
            }
        }
    }

    PluginConfig pluginConfig = PluginConfig::load("config/capabilities.cfg", "config/consent_grants.cfg");
    registry.setPluginConfig(&pluginConfig);

    // Connect to the Python AI server. The channel is lazy — no error if Python isn't up yet.
    JarvisAIClient aiClient(grpc::CreateChannel("localhost:50052", grpc::InsecureChannelCredentials()));

    JarvisServiceImpl service(engine, aiClient, registry);

    grpc::ServerBuilder builder;
    builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        spdlog::error("Failed to start gRPC server on {}", serverAddress);
        return 1;
    }

    spdlog::info("JARVIS gRPC server listening on {}", serverAddress);
    server->Wait();
    return 0;
}
