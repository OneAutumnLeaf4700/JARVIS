#pragma once

#include <string>
#include <unordered_map>


//Enum classes
enum class CommandType {
    ECHO,
    UNKNOWN,
    EXIT,
    HELP
};

//Structs
struct ParsedCommand {
    CommandType type;
    std::string payload;
};

//Function declarations
CommandType handleCommand(const std::string& command);
ParsedCommand parseCommand(const std::string& command);
void runCMD(ParsedCommand command);
std::string toLower(std::string text);
void runEcho(const std::string& payload);
void runUnknown();
CommandType extractCommandType(std::istringstream& stream);
std::string extractPayload(std::istringstream& stream);
void runHelp(const std::string& payload);


