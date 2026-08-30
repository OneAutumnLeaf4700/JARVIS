#include "ai_client.h"
#include <spdlog/spdlog.h>

JarvisAIClient::JarvisAIClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(jarvis::ai::v1::JarvisAIService::NewStub(channel)) {}

AIResult JarvisAIClient::ProcessNaturalLanguage(const std::string& text) {
  jarvis::ai::v1::NaturalLanguageRequest request;
  request.set_text(text);

  jarvis::ai::v1::NaturalLanguageResponse response;
  grpc::ClientContext context;

  // 5-second deadline — don't block forever if the Python AI server is down.
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));

  spdlog::info("AIClient: sending to Python AI server: '{}'", text);

  grpc::Status status = stub_->ProcessNaturalLanguage(&context, request, &response);

  if (!status.ok()) {
    spdlog::warn("AIClient: call failed — {} (code {})", status.error_message(), static_cast<int>(status.error_code()));
    return AIResult{false, "[AI unavailable: " + status.error_message() + "]", "UNKNOWN", 0.0f};
  }

  if (!response.success()) {
    spdlog::warn("AIClient: AI server returned error: {}", response.error());
    return AIResult{false, "[AI error: " + response.error() + "]", "UNKNOWN", 0.0f};
  }

  spdlog::info("AIClient: reply='{}' intent={} confidence={:.2f}", response.reply(), response.intent(), response.confidence());
  return AIResult{true, response.reply(), response.intent(), response.confidence()};
}
