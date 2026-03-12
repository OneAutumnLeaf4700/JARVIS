#include "command_handler.h"

#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <unordered_map>
#include <functional>

//COMMAND MAPPING

//User -> CommandType mapping
//Maps command strings to their corresponding CommandType
static const std::unordered_map<std::string, CommandType> COMMAND_MAP = {
    {"echo", CommandType::ECHO},
    {"unknown", CommandType::UNKNOWN},
    {"exit", CommandType::EXIT}, 
    {"help", CommandType::HELP}, 
    {"about", CommandType::ABOUT},
    {"status", CommandType::STATUS}
};

//CommandType -> Execution function mapping
//Maps CommandType to their corresponding execution functions
static const std::unordered_map<CommandType, std::function<void(const std::string&)>> COMMAND_DISPATCH = {
    {CommandType::ECHO, runEcho},
    {CommandType::UNKNOWN, [](const std::string&) { runUnknown(); }},
    {CommandType::EXIT, [](const std::string&) {  }}, // THIS IS INTENTIONAL. EXIT IS HANDLED IN ENGINE LAYER. NOTHING SHOULD BE DONE IN THIS ABSTRACTION
    {CommandType::HELP, runHelp},
    {CommandType::ABOUT, [](const std::string&) { runAbout(); }},
    {CommandType::STATUS, [](const std::string&) { }} // THIS IS INTENTIONAL. STATUS IS HANDLED IN ENGINE LAYER.

};

//CommandType -> Description mapping for help command
//Maps CommandType to their corresponding descriptions for help command
static const std::unordered_map<CommandType, std::string> COMMAND_DESCRIPTIONS = {
    {CommandType::ECHO, "Echoes the input back to the user. Usage: echo [text]"},
    {CommandType::UNKNOWN, "Default response for unrecognized commands."},
    {CommandType::EXIT, "Terminates the JARVIS Core Engine. Usage: exit"},
    {CommandType::HELP, "Provides information about available commands. Usage: help [command]"},
    {CommandType::ABOUT, "Provides information about JARVIS. Usage: about"},
    {CommandType::STATUS, "Shows engine state, uptime, and last command. Usage: status"}
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

//Handle the user input command and return 
CommandType handleCommand(const std::string& input) {
    //Command must be parsed to determine the appropriate action

    //Parse the command and run
    ParsedCommand parsed = parseCommand(input);
    runCMD(parsed);

    return parsed.type; //Return the command type for any additional handling in the engine loop
}

//Run the command based on its type and return the result
void runCMD(ParsedCommand command) {
    //Split object into type and payload
    CommandType cmdType = command.type;
    std::string payload = command.payload;

    //Execute the command based on its type
    auto it = COMMAND_DISPATCH.find(cmdType);
    if (it != COMMAND_DISPATCH.end()) {
        //Command type found in dispatch map, execute the corresponding function
        it->second(payload);
    } else {
        //Command type not found, run unknown command handler
        runUnknown();
    }
}

//COMMAND TYPE IMPLEMENTATIONS

//Echo command implementation
void runEcho(const std::string& payload) {
    std::cout << payload << std::endl;
}

//Unknown command implementation
void runUnknown() {
    std::cout << "Command not recognised. Please try again." << std::endl;
}

//Help command implementation
void runHelp(const std::string& payload) {
    //Help must list all commands available to jarvis. 
    //Will do this by iterating through command map and printing out keys in string format, alongside description of each.

    //Payload must be parsed to determine whether user wants to see all commands or get help on a specific command
    if (payload.empty()) {
        //No payload, list all commands
        std::cout << "Available commands:" << std::endl;

        //Iterate through COMMAND_MAP to get each command name (key) and its CommandType (value).
        //For each entry, look up the description in COMMAND_DESCRIPTIONS using the CommandType.
        //Each map entry is a std::pair — .first is the key, .second is the value.
        for (const auto& [name, type] : COMMAND_MAP) {
            auto it = COMMAND_DESCRIPTIONS.find(type);
            if (it != COMMAND_DESCRIPTIONS.end()) {
                std::cout << "  - " << name << ": " << it->second << std::endl;
            }
            else {
                std::cout << "  - " << name << ": No description available." << std::endl;
            }
        }
    }
    
    //If payload isnt empty we need to parse data and determine the command the user needs help with
    
}

//About command implementation
void runAbout() {
    std::cout << "JARVIS Core Engine v1.0" << std::endl;
    std::cout << "Developed by Rayyan." << std::endl;
}
