#include "engine.h"
#include "capability.h"
#include "capability_registry.h"
#include "command_handler.h"

#include <cctype>
#include <chrono>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>

//Engine Constructor
Engine::Engine() {
    running = true;
    startTime = std::chrono::steady_clock::now();
    lastCommand = "none";
}

//Engine entry point
void Engine::run(CapabilityRegistry& registry) {
    std::cout << "JARVIS Core Engine starting..." << std::endl;

    std::string input;

    while (running) {
        std::cout << ">";
        std::getline(std::cin, input);

        ParsedCommand parsed = parseCommand(input);
        std::string commandName = extractCommandName(input);

        //Update last command tracker to current command
        if (parsed.type != CommandType::STATUS) {
            if (!commandName.empty()) {
                lastCommand = commandName;
            }
        }

        //Engine layer handles termination — exit stays outside the registry
        if (parsed.type == CommandType::EXIT) {
            terminate();
            continue;
        }

        ExecutionContext context{*this, registry};
        std::optional<std::string> output = registry.dispatch(parsed.type, parsed.payload, context);

        // A word the CommandType enum doesn't know about might still be a plugin-registered
        // string intent (e.g. "system-info") — try that before declaring it unrecognised.
        bool stringIntentKnown = false;
        if (!output && parsed.type == CommandType::UNKNOWN && !commandName.empty()) {
            stringIntentKnown = registry.resolve(commandName) != nullptr;
            output = registry.dispatch(commandName, parsed.payload, context);
        }

        if (output) {
            if (!output->empty()) {
                std::cout << *output << std::endl;
            }
        } else if (parsed.type == CommandType::UNKNOWN && !stringIntentKnown) {
            std::cout << runUnknown() << std::endl;
        } else {
            // Resolved command, but disabled — a distinct outcome from "unrecognised" (INV-7).
            std::cout << "Command is currently unavailable." << std::endl;
        }
    }
}

//Program termination
void Engine::terminate() {
    std::cout << "Terminating JARVIS Core Engine..." << std::endl;
    running = false;
}

//Determine command type to perform required action
std::string Engine::extractCommandName(const std::string& input) {
    std::istringstream stream(input);
    std::string commandName;
    stream >> commandName;
    for (char& ch : commandName) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return commandName;
}

//Get status information to send to jarvis service layer
StatusInfo Engine::getStatusInfo() const {
    const auto now = std::chrono::steady_clock::now();
    const auto secondsElapsed = std::chrono::duration_cast<std::chrono::seconds>(now - startTime).count();

    StatusInfo info;
    info.running = running;
    info.uptimeSeconds = secondsElapsed;
    info.lastCommand = lastCommand;
    return info;
}
