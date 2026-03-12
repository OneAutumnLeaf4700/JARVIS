#pragma once

#include <chrono>
#include <string>

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
};

