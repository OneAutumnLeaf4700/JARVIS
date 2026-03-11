#include "command_handler.h"

#include <iostream>

CommandResult handleCommand(const std::string& command) {
    if (command == "exit" || command == "quit") {
        std::cout << "Shutting down." << std::endl;
        return CommandResult::EXIT;
    }

    std::cout << "You said: " << command << std::endl;

    return CommandResult::RUNNING;
}
