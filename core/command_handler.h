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
    STATUS
};

//Structs
struct ParsedCommand {
    CommandType type;
    std::string payload;
};

//User input handling functions
CommandType handleCommand(const std::string& command);
ParsedCommand parseCommand(const std::string& command);

//Parsing helper functions
std::string toLower(std::string text);
CommandType extractCommandType(std::istringstream& stream);
std::string extractPayload(std::istringstream& stream);

//COMMAND TYPE IMPLEMENTATIONS
void runCMD(ParsedCommand command);
void runEcho(const std::string& payload);
void runUnknown();
void runHelp(const std::string& payload);
void runAbout();