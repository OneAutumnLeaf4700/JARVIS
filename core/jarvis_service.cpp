#include "jarvis_service.h"
#include "command_handler.h"

#include <iomanip>
#include <sstream>
#include <spdlog/spdlog.h>

// Constructor — takes a reference to the Engine so STATUS can query live state.
JarvisServiceImpl::JarvisServiceImpl(Engine& engine) : engine_(engine) {
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

  // Step 3: Build and run the command.
  // STATUS returns "" from runCMD — handled as a special case below.
  ParsedCommand parsedCmd;
  parsedCmd.type = internalCmd;
  parsedCmd.payload = payload;

  std::string output = runCMD(parsedCmd);

  // STATUS is a special case: its data lives in the Engine, not the command handler.
  if (internalCmd == CommandType::STATUS) {
    StatusInfo info = engine_.getStatusInfo();

    const long hours   = info.uptimeSeconds / 3600;
    const long minutes = (info.uptimeSeconds % 3600) / 60;
    const long seconds = info.uptimeSeconds % 60;

    std::ostringstream out;
    out << "Engine: " << (info.running ? "running" : "stopped") << "\n";
    out << "Uptime: "
        << std::setfill('0') << std::setw(2) << hours   << ":"
        << std::setw(2)      << minutes << ":"
        << std::setw(2)      << seconds << "\n";
    out << "Last command: " << info.lastCommand;
    output = out.str();
  }

  // Step 4: Determine success.
  // UNKNOWN means the command wasn't recognised — that's a client error.
  const bool success = (internalCmd != CommandType::UNKNOWN);

  if (!success) {
    spdlog::warn("ProcessCommand: unrecognised command, returning error");
  } else {
    spdlog::info("ProcessCommand: OK");
  }

  // Step 5: Fill the response.
  response->set_success(success);
  response->set_message(output);
  response->set_command_type(internalCommandToProto(internalCmd));
  response->set_error_code(resultToProtoErrorCode(success));

  return ::grpc::Status::OK;
}

// Helper: Convert proto CommandType to internal CommandType.
CommandType JarvisServiceImpl::protoCommandToInternal(jarvis::v1::CommandType protoCmd) {
  // Proto enums and internal enums use different names/values.
  // This method translates between the two worlds.
  
  // We switch on the proto enum and return the matching internal enum.
  // Proto uses COMMAND_TYPE_* naming, internal uses bare names.
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
      // If unknown or unspecified, return UNKNOWN.
      return CommandType::UNKNOWN;
  }
}

// Helper: Convert internal CommandType to proto CommandType.
jarvis::v1::CommandType JarvisServiceImpl::internalCommandToProto(CommandType internalCmd) {
  // Reverse mapping: internal enum -> proto enum.
  // This is used when filling the response to send back to the client.
  
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

// Helper: Convert result bool to proto ErrorCode enum.
jarvis::v1::ErrorCode JarvisServiceImpl::resultToProtoErrorCode(bool success) {
  // For now, a very simple mapping:
  // success = true  -> ERROR_CODE_NONE
  // success = false -> ERROR_CODE_EXECUTION_FAILED
  
  // Later, you can make this more sophisticated with different error codes.
  if (success) {
    return jarvis::v1::ERROR_CODE_NONE;
  } else {
    return jarvis::v1::ERROR_CODE_EXECUTION_FAILED;
  }
}
