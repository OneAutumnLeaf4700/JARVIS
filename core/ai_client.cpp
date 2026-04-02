#include "ai_client.h"
#include <spdlog/spdlog.h>

JarvisAIClient::JarvisAIClient(std::shared_ptr<grpc::Channel> channel)
    : stub_(jarvis::ai::v1::JarvisAIService::NewStub(channel)) {}

std::string JarvisAIClient::ProcessNaturalLanguage(const std::string& text) {
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
    return "[AI unavailable: " + status.error_message() + "]";
  }

  if (!response.success()) {
    spdlog::warn("AIClient: AI server returned error: {}", response.error());
    return "[AI error: " + response.error() + "]";
  }

  spdlog::info("AIClient: reply='{}'", response.reply());
  return response.reply();
}
