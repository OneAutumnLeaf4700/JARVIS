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
        std::cout << ">";
        std::getline(std::cin, input);

        CommandType cmdType = handleCommand(input);

        if (cmdType == CommandType::EXIT) {
            terminate();
        }
    }
}

void Engine::terminate() {
    std::cout << "Terminating JARVIS Core Engine..." << std::endl;
    running = false;
}
