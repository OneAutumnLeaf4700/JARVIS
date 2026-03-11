#pragma once

class Engine{
    private:
        bool running;

    public:
        Engine();     
        void run();
};

enum class EngineState {
	RUNNING,
	EXIT
};