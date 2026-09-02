#include "plugin_internal.h"

#include <cctype>
#include <sstream>

std::optional<int> parseVolumeArgument(const std::string& payload) {
    std::istringstream stream(payload);
    std::string verb;
    stream >> verb;

    if (verb == "get") {
        return std::nullopt;
    }
    if (verb != "set") {
        return std::nullopt;
    }

    std::string levelText;
    if (!(stream >> levelText) || levelText.empty()) {
        return std::nullopt;
    }
    for (char c : levelText) {
        if (!std::isdigit(static_cast<unsigned char>(c))) {
            return std::nullopt;
        }
    }

    const int level = std::stoi(levelText);
    if (level < 0 || level > 100) {
        return std::nullopt;
    }
    return level;
}

std::vector<std::string> buildVolumeArgv(int level) {
    return {"pactl", "set-sink-volume", "@DEFAULT_SINK@", std::to_string(level) + "%"};
}

std::vector<std::string> buildShutdownArgv() {
    return {"systemctl", "poweroff"};
}
