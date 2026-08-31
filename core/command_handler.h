#pragma once

#include <string>
#include <unordered_map>

//Enum classes

//Command types supported by JARVIS
enum class CommandType {
    ECHO,
    UNKNOWN,
    EXIT,
    HELP,
    ABOUT,
    STATUS,
    SYSTEM_INFO
};

//Structs
struct ParsedCommand {
    CommandType type;
    std::string payload;
};

//User input handling functions
ParsedCommand parseCommand(const std::string& command);

//Parsing helper functions
std::string toLower(std::string text);
CommandType extractCommandType(std::istringstream& stream);
std::string extractPayload(std::istringstream& stream);

// Fallback text when no capability matched the parsed command. Kept here (not a capability —
// CommandType::UNKNOWN is never registered in the CapabilityRegistry) since both the CLI and
// the gRPC service need this exact string when a dispatch comes back empty.
std::string runUnknown();
