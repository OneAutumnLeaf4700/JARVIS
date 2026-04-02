#pragma once

#include <chrono>
#include <string>

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

        void printStatus() const;
        static std::string extractCommandName(const std::string& input);

    public:
        Engine();     
        void run();
        void terminate();
        StatusInfo getStatusInfo() const;
};



