#include "engine.h"
#include "command_handler.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>

//Engine Constructor
Engine::Engine() {
    running = true;
    startTime = std::chrono::steady_clock::now();
    lastCommand = "none";
}

//Engine entry point
void Engine::run() {
    std::cout << "JARVIS Core Engine starting..." << std::endl;

    std::string input;

    while (running) {
        std::cout << ">";
        std::getline(std::cin, input);

        CommandType cmdType = handleCommand(input);

        //Update last command tracker to current command
        if (cmdType != CommandType::STATUS) {
            std::string commandName = extractCommandName(input);
            if (!commandName.empty()) {
                lastCommand = commandName;
            }
        }

        //Engine layer handles status
        if (cmdType == CommandType::STATUS) {
            printStatus();
        }

        //Engine layer handles termination
        if (cmdType == CommandType::EXIT) {
            terminate();
        }
    }
}

//Program termination
void Engine::terminate() {
    std::cout << "Terminating JARVIS Core Engine..." << std::endl;
    running = false;
}

//Status command implementation
void Engine::printStatus() const {
    const auto now = std::chrono::steady_clock::now();
    const auto secondsElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();

    const long hours = secondsElapsed / 3600;
    const long minutes = (secondsElapsed % 3600) / 60;
    const long seconds = secondsElapsed % 60;

    std::cout << "Engine: " << (running ? "running" : "stopped") << std::endl;
    std::cout << "Uptime: "
              << std::setfill('0') << std::setw(2) << hours << ":"
              << std::setw(2) << minutes << ":"
              << std::setw(2) << seconds << std::endl;
    std::cout << "Last command: " << lastCommand << std::endl;
}

//Determine command type
std::string Engine::extractCommandName(const std::string& input) {
    std::istringstream stream(input);
    std::string commandName;
    stream >> commandName;
    for (char& ch : commandName) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return commandName;
}
