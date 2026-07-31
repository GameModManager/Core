#pragma once

#include <string>

namespace engine {

class PluginLoader;

// Initialize the embedded Python interpreter and the gmm module.
// Safe to call multiple times - only initializes once.
bool python_init();

// Load a .py plugin file and call its register() function.
// Returns true on success.
bool python_load_plugin(PluginLoader* loader, const std::string& path);

// Shut down the embedded Python interpreter.
void python_shutdown();

}  // namespace engine
