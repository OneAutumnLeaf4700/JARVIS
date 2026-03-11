#include "engine.h"
#include "command_handler.h"

#include <iostream>
#include <string>

Engine::Engine() {
    running = true;
}

void Engine::run() {
    std::cout << "JARVIS Core Engine starting..." << std::endl;

    std::string input;

    while (running) {
        std::cout << "> ";
        std::getline(std::cin, input);

        if (handleCommand(input) == CommandResult::EXIT) {
            running = false;
        }
    }

    std::cout << "JARVIS Core Engine shutting down..." << std::endl;
}
