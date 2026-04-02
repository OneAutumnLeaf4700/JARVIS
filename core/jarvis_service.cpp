#include "jarvis_service.h"
#include "command_handler.h"

// Constructor
JarvisServiceImpl::JarvisServiceImpl() {
  // Initialize the service. For now, nothing special to do.
  // Later, you might initialize state or connect to the engine here.
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
  
  // Step 1: Extract command type from the proto request.
  // The proto request contains a CommandType enum value.
  // We need to convert it to our internal CommandType enum.
  CommandType internalCmd = protoCommandToInternal(request->command());
  
  // Step 2: Build the command payload (what comes after the command name).
  // In your case, this is the optional text argument to the command.
  std::string payload = request->payload();
  
  // Step 3: Parse and execute the command using existing engine logic.
  // We reuse the existing command handling flow instead of duplicating logic.
  // This is why the adapter pattern is good: command logic stays centralized.
  ParsedCommand parsedCmd;
  parsedCmd.type = internalCmd;
  parsedCmd.payload = payload;
  
  // Step 4: Run the command through the dispatch system.
  // This calls the appropriate handler (echo, help, status, etc).
  // The output is returned as a string instead of being printed to stdout.
  std::string output = runCMD(parsedCmd);

  // Step 5: Fill the response object with results.
  // The response must be filled before we return.
  // These are the fields defined in your proto ExecuteCommandResponse message.

  // success field: for now, always true (assumes command executed).
  // Later track failures and set this to false on errors.
  response->set_success(true);

  // message field: the actual output of the command.
  response->set_message(output);
  
  // command_type field: echo back the normalized command type.
  // We convert the internal enum back to proto enum for the response.
  response->set_command_type(internalCommandToProto(internalCmd));
  
  // error_code field: indicate success or failure.
  // For now, always NO_ERROR (success).
  // Later, different error codes for different failures.
  response->set_error_code(resultToProtoErrorCode(true));
  
  // Step 6: Return gRPC status.
  // OK means the RPC itself succeeded (transport level).
  // Even if the command failed, we return OK here because the RPC transport worked.
  // The actual command success/failure is encoded in the response fields above.
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
