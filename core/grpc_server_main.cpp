#include "ai_client.h"
#include "engine.h"
#include "jarvis_service.h"

#include <grpcpp/grpcpp.h>
#include <spdlog/spdlog.h>

#include <memory>
#include <string>

int main() {
    spdlog::set_level(spdlog::level::info);
    spdlog::set_pattern("[%H:%M:%S] [%^%l%$] %v");

    const std::string serverAddress = "0.0.0.0:50051";
    Engine engine;

    // Connect to the Python AI server. The channel is lazy — no error if Python isn't up yet.
    JarvisAIClient aiClient(grpc::CreateChannel("localhost:50052", grpc::InsecureChannelCredentials()));

    JarvisServiceImpl service(engine, aiClient);

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
