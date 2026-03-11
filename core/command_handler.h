#pragma once

#include <string>

//Enum classes
enum class CommandType {
    ECHO,
    UNKNOWN,
    EXIT
};

//Structs
struct ParsedCommand {
    CommandType type;
    std::string payload;
};

//Function declarations
CommandType handleCommand(const std::string& command);
ParsedCommand parseCommand(const std::string& command);
CommandType runCMD(CommandType cmdType, const std::string& payload);
std::string toLower(std::string text);

