#include "command_handler.h"

#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>

//COMMAND MAPPING

//User -> CommandType mapping
//Maps command strings to their corresponding CommandType
static const std::unordered_map<std::string, CommandType> COMMAND_MAP = {
    {"echo", CommandType::ECHO},
    {"exit", CommandType::EXIT},
    {"help", CommandType::HELP},
    {"about", CommandType::ABOUT},
    {"status", CommandType::STATUS}
};

//PARSING HELPER FUNCTIONS

//Trim function to remove leading and trailing whitespace from a string
namespace {
std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return "";
    }

    const std::size_t last = text.find_last_not_of(" \t\n\r");
    return text.substr(first, last - first + 1);
    }
}

//Convert a string to lowercase for case-insensitive command parsing
std::string toLower(std::string text) {
    std::transform(
        text.begin(),
        text.end(),
        text.begin(),
        [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        }
    );
    return text;
}

//Parse the user input to determine the type of command
ParsedCommand parseCommand(const std::string& input) {
    //Commands are broken down into a command type (first word) and an optional payload
    //The parsed command object is then returned for execution

    //Return a default object of type unknown if the input cannot be parsed
    ParsedCommand result{CommandType::UNKNOWN, ""};

    //Remove leading and trailing whitespace
    std::string cleaned = trim(input);

    if (cleaned.empty()) { // If the cleaned input is empty, return the default unknown command
        return result;
    }

    std::istringstream stream(cleaned); // Turn the cleaned input into a string stream for word by word parsing

    //Part 1: Command extraction
    //Extract the command type (first word)
    CommandType commandType = extractCommandType(stream);

    //Part 2: Payload extraction
    //Extract rest of the input as payload
    std::string payload = extractPayload(stream);

    //Build result object
    result.type = commandType;
    result.payload = payload;

    return result;
}

//Extract command type
CommandType extractCommandType(std::istringstream& stream) {
    std::string command;
    stream >> command;
    command = toLower(command);

    //Look up the command in the command map
    auto it = COMMAND_MAP.find(command);

    if (it !=COMMAND_MAP.end()) {
        //Command found in map, return the corresponding CommandType
        return it->second;
    }

    //Command not found, return UNKNOWN
    return CommandType::UNKNOWN;
}

//Extract payload content
std::string extractPayload(std::istringstream& stream) {
    std::string payload;
    std::getline(stream, payload);
    return trim(payload);
}

//Fallback text when no capability matched
std::string runUnknown() {
    return "Command not recognised. Please try again.";
}
