#include "Core/Application.h"

#include <cstdlib>
#include <exception>
#include <iostream>

int main() {
    try {
        engine::Application app;
        app.run();
    } catch (const std::exception& error) {
        std::cerr << "Fatal error: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}
