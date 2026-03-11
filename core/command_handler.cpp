#include "command_handler.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace {
std::string trim(const std::string& text) {
    const std::size_t first = text.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) {
        return "";
    }

    const std::size_t last = text.find_last_not_of(" \t\n\r");
    return text.substr(first, last - first + 1);
}
} // namespace

//Handle the user input command and return 
CommandType handleCommand(const std::string& input) {

    //Parse the command and run 
    ParsedCommand parsed = parseCommand(input);
    runCMD(parsed.type, parsed.payload);

    return parsed.type; //return the command type to the engine to determine if it should continue running or not
}

//Parse the user input to determine the type of command
ParsedCommand parseCommand(const std::string& input) {
    //Commands are broken down into a command type (first word) and an optional payload
    //Return a default object of type unknown if the input cannot be parsed
    ParsedCommand result{CommandType::UNKNOWN, ""};

    //Remove leading and trailing whitespace
    std::string cleaned = trim(input); 

    if (cleaned.empty()) {
        return result;
    }

    std::istringstream stream(cleaned); // Turn the cleaned input into a string stream for easy parsing

    //Extract the command type (first word)
    std::string command;
    stream >> command;
    command = toLower(command);

    //Extract rest of the input as payload
    std::string payload;
    std::getline(stream, payload);
    payload = trim(payload); //Remove leading and trailing whitespace from payload
    
    //Determine command type based on the first word
    if (command == "echo") {
        result.type = CommandType::ECHO;
        result.payload = payload; //Store the payload for the echo command
    } else if (command == "exit") {
        result.type = CommandType::EXIT;
    } else {
        result.type = CommandType::UNKNOWN;
    }
    
    return result;
}

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

//Run the command based on its type and return the result
CommandType runCMD(CommandType cmdType, const std::string& payload) {
    
    switch (cmdType) {
        case CommandType::ECHO:
            if (payload.empty()) {
                std::cout << "(empty)" << std::endl;
            } else {
                std::cout << payload << std::endl;
            }
            return CommandType::ECHO;
        case CommandType::EXIT:
            std::cout << "Shutting down." << std::endl;
            return CommandType::EXIT;
        default:
            std::cout << "Unknown command!" << std::endl;
            return CommandType::UNKNOWN;
    }
}