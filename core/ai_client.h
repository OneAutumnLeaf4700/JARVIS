#pragma once

#include "../generated/cpp/ai.grpc.pb.h"
#include <grpcpp/grpcpp.h>
#include <string>

// Result of a call to the Python AI server: the human-readable reply plus the
// structured classification the caller can act on.
struct AIResult {
  bool success;
  std::string reply;
  std::string intent;
  float confidence;
};

// JarvisAIClient is the C++ side gRPC client that calls the Python AI server.
// When the command layer receives input it can't handle (UNKNOWN), it forwards
// the raw text here and returns whatever the AI server replies.
class JarvisAIClient {
 public:
  explicit JarvisAIClient(std::shared_ptr<grpc::Channel> channel);

  // Send raw natural language text to the Python AI server.
  // Returns the AI's reply plus classified intent/confidence, or an
  // error-describing reply with success=false if the call fails.
  AIResult ProcessNaturalLanguage(const std::string& text);

 private:
  std::unique_ptr<jarvis::ai::v1::JarvisAIService::Stub> stub_;
};
