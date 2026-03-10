#include <iostream>
#include <string>

int main() {
    std::cout << "JARVIS Core Engine starting..." << std::endl;

    std::string input;

    while (true) {
        std::cout << "> ";
        std::getline(std::cin, input);

        if (input == "exit" || input == "quit") {
            std::cout << "Shutting down." << std::endl;
            break;
        }

        std::cout << "You said: " << input << std::endl;
    }

    return 0;
}
