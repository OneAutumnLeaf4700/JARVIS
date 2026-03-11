#pragma once
#include <string>

enum class CommandResult {
	RUNNING,
	EXIT
};

CommandResult handleCommand(const std::string& command);