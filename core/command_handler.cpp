#include <iostream>
#include <string>

bool handleCommand(const std::string& command) {
    if (command == "exit" || command == "quit") {
            std::cout << "Shutting down." << std::endl;
            return true;
        }
    
    std::cout << "You said: " << command << std::endl;

    return false;
}