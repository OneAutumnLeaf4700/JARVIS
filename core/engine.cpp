#include <iostream>
#include <string>
#include "engine.h"
#include "command_handler.h"

void Engine::run() {
    std::cout << "JARVIS Core Engine starting..." << std::endl;

    std::string input;

    while (true) {
        std::cout << "> ";
        std::getline(std::cin, input);

        if (handleCommand(input)) {break;}
    }
}