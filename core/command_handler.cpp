#include "command_handler.h"

#include <iostream>

//Handle the user input command and return 
CommandType handleCommand(const std::string& command) {
    
    //Parse the command to determine its type
    CommandType cmdType = parseCommand(command);

    //Check for terminal commands and return early if necessary
    if (cmdType == CommandType::EXIT) {
        return cmdType;
    }

    //Run the command if no terminal commands are triggered
    runCMD(cmdType);

    return cmdType;
}

//Parse the user input to determine the type of command
CommandType parseCommand(const std::string& command) {
    
    //Simple command parsing logic based on the input string
    if (command == "echo") {
        return CommandType::ECHO;
    }
    else if (command == "exit" || command == "quit") {
        return CommandType::EXIT;
    }
    else {
        return CommandType::UNKNOWN;
    }
}

//Run the command based on its type and return the result
CommandType runCMD(CommandType cmdType) {
    switch (cmdType) {
        case CommandType::ECHO:
            std::cout << "Echo command executed!" << std::endl;
            return CommandType::ECHO;
        case CommandType::EXIT:
            std::cout << "Exit command executed!" << std::endl;
            return CommandType::EXIT;
        default:
            std::cout << "Unknown command!" << std::endl;
            return CommandType::UNKNOWN;
    }
}