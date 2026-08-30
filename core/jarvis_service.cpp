#include "jarvis_service.h"

#include <optional>
#include <spdlog/spdlog.h>

// Constructor — takes the Engine (for STATUS), the AI client (for UNKNOWN commands), and the
// CapabilityRegistry (for dispatching every known command, including a classified UNKNOWN).
JarvisServiceImpl::JarvisServiceImpl(Engine& engine, JarvisAIClient& aiClient, CapabilityRegistry& registry)
    : engine_(engine), aiClient_(aiClient), registry_(registry) {
}

// Destructor
JarvisServiceImpl::~JarvisServiceImpl() {
  // Clean up any resources if needed.
}

// Core RPC method: ProcessCommand
// This is the entry point when a client (like Python) sends a command over gRPC.
::grpc::Status JarvisServiceImpl::ProcessCommand(
    ::grpc::ServerContext* context,
    const ::jarvis::v1::ExecuteCommandRequest* request,
    ::jarvis::v1::ExecuteCommandResponse* response) {

  // Step 1: Validate — reject unspecified command type before doing any work.
  if (request->command() == jarvis::v1::COMMAND_TYPE_UNSPECIFIED) {
    spdlog::warn("Rejected request: COMMAND_TYPE_UNSPECIFIED");
    response->set_success(false);
    response->set_message("Invalid request: command type must be specified.");
    response->set_command_type(jarvis::v1::COMMAND_TYPE_UNSPECIFIED);
    response->set_error_code(jarvis::v1::ERROR_CODE_INVALID_COMMAND);
    return ::grpc::Status::OK;
  }

  // Step 2: Translate proto enum → internal enum and extract payload.
  CommandType internalCmd = protoCommandToInternal(request->command());
  std::string payload = request->payload();

  spdlog::info("ProcessCommand: command={} payload='{}'",
      jarvis::v1::CommandType_Name(request->command()), payload);

  // Step 3: Dispatch through the registry. For UNKNOWN this always returns std::nullopt
  // (UNKNOWN is never registered) — output starts empty and gets replaced below regardless.
  ExecutionContext execContext{engine_, registry_};
  std::string output = registry_.dispatch(internalCmd, payload, execContext).value_or("");

  // Step 4: UNKNOWN commands are forwarded to the Python AI server. If the AI
  // classifies the text into a known intent with enough confidence, re-dispatch
  // as that command instead of just echoing the AI's reply back.
  if (internalCmd == CommandType::UNKNOWN) {
    spdlog::info("ProcessCommand: unrecognised command, forwarding to AI layer");
    AIResult aiResult = aiClient_.ProcessNaturalLanguage(payload);

    constexpr float kConfidenceThreshold = 0.5f;
    CommandType classifiedCmd = intentToCommandType(aiResult.intent);

    if (aiResult.success && classifiedCmd != CommandType::UNKNOWN &&
        aiResult.confidence >= kConfidenceThreshold) {
      spdlog::info("ProcessCommand: AI classified intent={} confidence={:.2f}, re-dispatching",
          aiResult.intent, aiResult.confidence);
      internalCmd = classifiedCmd;
      output = registry_.dispatch(classifiedCmd, payload, execContext).value_or("");
    } else {
      output = aiResult.reply;
    }
  }

  // Success if we got a non-empty reply (even AI errors return a descriptive string).
  const bool success = (internalCmd != CommandType::UNKNOWN) || !output.empty();

  spdlog::info("ProcessCommand: {}", success ? "OK" : "FAIL");

  // Step 5: Fill the response.
  response->set_success(success);
  response->set_message(output);
  response->set_command_type(internalCommandToProto(internalCmd));
  response->set_error_code(resultToProtoErrorCode(success));

  return ::grpc::Status::OK;
}

// Helper: Convert proto CommandType to internal CommandType.
CommandType JarvisServiceImpl::protoCommandToInternal(jarvis::v1::CommandType protoCmd) {
  switch (protoCmd) {
    case jarvis::v1::COMMAND_TYPE_ECHO:
      return CommandType::ECHO;
    case jarvis::v1::COMMAND_TYPE_UNKNOWN:
      return CommandType::UNKNOWN;
    case jarvis::v1::COMMAND_TYPE_EXIT:
      return CommandType::EXIT;
    case jarvis::v1::COMMAND_TYPE_HELP:
      return CommandType::HELP;
    case jarvis::v1::COMMAND_TYPE_ABOUT:
      return CommandType::ABOUT;
    case jarvis::v1::COMMAND_TYPE_STATUS:
      return CommandType::STATUS;
    case jarvis::v1::COMMAND_TYPE_UNSPECIFIED:
    default:
      return CommandType::UNKNOWN;
  }
}

// Helper: Convert internal CommandType to proto CommandType.
jarvis::v1::CommandType JarvisServiceImpl::internalCommandToProto(CommandType internalCmd) {
  switch (internalCmd) {
    case CommandType::ECHO:
      return jarvis::v1::COMMAND_TYPE_ECHO;
    case CommandType::UNKNOWN:
      return jarvis::v1::COMMAND_TYPE_UNKNOWN;
    case CommandType::EXIT:
      return jarvis::v1::COMMAND_TYPE_EXIT;
    case CommandType::HELP:
      return jarvis::v1::COMMAND_TYPE_HELP;
    case CommandType::ABOUT:
      return jarvis::v1::COMMAND_TYPE_ABOUT;
    case CommandType::STATUS:
      return jarvis::v1::COMMAND_TYPE_STATUS;
    default:
      return jarvis::v1::COMMAND_TYPE_UNSPECIFIED;
  }
}

// Helper: Convert an AI-classified intent string to internal CommandType.
CommandType JarvisServiceImpl::intentToCommandType(const std::string& intent) {
  if (intent == "STATUS") return CommandType::STATUS;
  if (intent == "ECHO") return CommandType::ECHO;
  if (intent == "ABOUT") return CommandType::ABOUT;
  return CommandType::UNKNOWN;
}

// Helper: Convert result bool to proto ErrorCode enum.
jarvis::v1::ErrorCode JarvisServiceImpl::resultToProtoErrorCode(bool success) {
  if (success) {
    return jarvis::v1::ERROR_CODE_NONE;
  } else {
    return jarvis::v1::ERROR_CODE_EXECUTION_FAILED;
  }
}
