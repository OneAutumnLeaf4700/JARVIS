#include <iostream>
#include <string>
#include "capability_registry.h"
#include "engine.h"

int main() {
    Engine engine;
    CapabilityRegistry registry;
    registerBuiltinCapabilities(registry);

    engine.run(registry);

    return 0;
}
