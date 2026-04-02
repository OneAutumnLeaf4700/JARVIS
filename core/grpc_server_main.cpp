#include "jarvis_service.h"

#include <grpcpp/grpcpp.h>

#include <iostream>
#include <memory>
#include <string>

int main() {
    const std::string serverAddress = "0.0.0.0:50051";
    JarvisServiceImpl service;

    grpc::ServerBuilder builder;
    builder.AddListeningPort(serverAddress, grpc::InsecureServerCredentials());
    builder.RegisterService(&service);

    std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
    if (!server) {
        std::cerr << "Failed to start gRPC server." << std::endl;
        return 1;
    }

    std::cout << "JARVIS gRPC server listening on " << serverAddress << std::endl;
    server->Wait();
    return 0;
}
