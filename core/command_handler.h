#pragma once

#include "engine.h"

#include <string>


enum class CommandType {
    ECHO,
    UNKNOWN,
    EXIT
};

CommandType handleCommand(const std::string& command);
CommandType parseCommand(const std::string& command);
CommandType runCMD(CommandType cmdType);