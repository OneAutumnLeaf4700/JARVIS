#pragma once

#include <chrono>
#include <string>

class CapabilityRegistry;

//Status struct to return status information from engine to service layer
struct StatusInfo {
    bool running;
    long uptimeSeconds;
    std::string lastCommand;
};

class Engine{
    private:
        bool running; //Running flag
        std::chrono::steady_clock::time_point startTime; //track uptime
        std::string lastCommand;//track last command

        static std::string extractCommandName(const std::string& input);

    public:
        Engine();
        void run(CapabilityRegistry& registry);
        void terminate();
        StatusInfo getStatusInfo() const;
};
