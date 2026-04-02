#pragma once

#include "../generated/cpp/jarvis.grpc.pb.h"
#include "command_handler.h"
#include <grpcpp/grpcpp.h>

// JarvisServiceImpl is the server-side implementation of the gRPC JarvisService.
// It inherits from the generated JarvisService::Service base class.
// This class acts as the adapter between gRPC transport layer and internal engine logic.
class JarvisServiceImpl final : public jarvis::v1::JarvisService::Service {
 public:
  JarvisServiceImpl();
  virtual ~JarvisServiceImpl();

  // Override the ProcessCommand RPC method from the generated service interface.
  // This method is invoked by gRPC when a client sends a ProcessCommand request.
  // Parameters:
  //   - context: gRPC server context (contains metadata about the RPC call)
  //   - request: the incoming request message from the proto definition
  //   - response: the response message we must fill before returning
  // Return:
  //   - grpc::Status: indicates success/failure of the RPC transport level
  ::grpc::Status ProcessCommand(
      ::grpc::ServerContext* context,
      const ::jarvis::v1::ExecuteCommandRequest* request,
      ::jarvis::v1::ExecuteCommandResponse* response) override;

 private:
  // Helper method to convert proto CommandType enum to internal CommandType enum.
  // Why: proto enums and internal enums are separate.
  // We must translate between them at the adapter boundary.
  CommandType protoCommandToInternal(jarvis::v1::CommandType protoCmd);

  // Helper method to convert internal CommandType enum to proto CommandType enum.
  // Why: we need to send back a normalized command type to the client.
  jarvis::v1::CommandType internalCommandToProto(CommandType internalCmd);

  // Helper method to convert internal error/result into a proto ErrorCode enum.
  // Why: error classification should be transport-agnostic on the proto side.
  jarvis::v1::ErrorCode resultToProtoErrorCode(bool success);
};
