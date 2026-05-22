#ifdef NATIVE_BUILD

#include <cstdio>
#include <cstdlib>
#include <vector>
#include <thread>
#include <chrono>
#include <atomic>
#include <csignal>
#include <iostream>
#include <string>
#include <poll.h>
#include <unistd.h>
#include <sys/types.h>

// Platform abstraction
#include "utils/simple-timer.hpp"
#include "id-generator.hpp"

// CLI modules
#include "cli/cli-terminal.hpp"
#include "cli/cli-device.hpp"
#include "cli/cli-renderer.hpp"
#include "cli/cli-commands.hpp"
#include "cli/cli-event-tap.hpp"

// Native drivers for global instances
#include "device/drivers/native/native-logger-driver.hpp"
#include "device/drivers/native/native-clock-driver.hpp"
#include "device/drivers/native/native-peer-broker.hpp"

// Constants
static constexpr int MIN_DEVICES = 1;
static constexpr int MAX_DEVICES = 8;

// Global running flag for signal handling
std::atomic<bool> g_running{true};
static bool g_headless = false;

// Command input state
std::string g_commandBuffer;
std::string g_commandResult;
int g_selectedDevice = 0;  // Currently selected device index

void signalHandler(int signal) {
    (void)signal;  // Suppress unused parameter warning
    g_running = false;
}

/**
 * Parse command line arguments for device count.
 * Returns -1 if no valid count specified (prompt needed).
 */
int parseArgs(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        std::string arg = argv[i];

        if (arg == "--headless") {
            g_headless = true;
            continue;
        }

        // Check for -n or --count flag
        if ((arg == "-n" || arg == "--count") && i + 1 < argc) {
            int count = std::atoi(argv[i + 1]);
            if (count >= MIN_DEVICES && count <= MAX_DEVICES) {
                return count;
            }
        }
        
        // Check for bare number argument
        int count = std::atoi(arg.c_str());
        if (count >= MIN_DEVICES && count <= MAX_DEVICES) {
            return count;
        }
        
        // Help flag
        if (arg == "-h" || arg == "--help") {
            printf("PDN CLI Simulator\n");
            printf("Usage: %s [options] [device_count]\n\n", argv[0]);
            printf("Options:\n");
            printf("  -n, --count N   Create N devices (1-%d)\n", MAX_DEVICES);
            printf("  --headless      Line-buffered REPL mode: read commands from stdin, print ok:/err: to stdout\n");
            printf("  -h, --help      Show this help message\n");
            printf("\nExamples:\n");
            printf("  %s           Interactive prompt for device count\n", argv[0]);
            printf("  %s 3         Create 3 devices\n", argv[0]);
            printf("  %s -n 4      Create 4 devices\n", argv[0]);
            exit(0);
        }
    }
    return -1;  // No valid count found, need to prompt
}

/**
 * Prompt user for device count with validation.
 */
int promptDeviceCount() {
    printf("\n");
    printf("\033[1;33m");  // Bold yellow
    printf("How many PDN devices would you like to simulate? (1-%d)\n", MAX_DEVICES);
    printf("\033[0m");
    printf("  - Device 1 will be a Hunter\n");
    printf("  - Device 2 will be a Bounty\n");
    printf("  - Roles alternate: Hunter, Bounty, Hunter, Bounty...\n");
    printf("\n");
    
    while (true) {
        printf("\033[1;36m");  // Bold cyan
        printf("Enter device count [1]: ");
        printf("\033[0m");
        fflush(stdout);
        
        std::string input;
        std::getline(std::cin, input);
        
        // Default to 1 if empty
        if (input.empty()) {
            return 1;
        }
        
        // Parse input
        int count = std::atoi(input.c_str());
        if (count >= MIN_DEVICES && count <= MAX_DEVICES) {
            return count;
        }
        
        printf("\033[1;31m");  // Bold red
        printf("Invalid input. Please enter a number between %d and %d.\n", MIN_DEVICES, MAX_DEVICES);
        printf("\033[0m");
    }
}

/**
 * Create devices with alternating hunter/bounty roles.
 */
std::vector<cli::DeviceInstance> createDevices(int count) {
    std::vector<cli::DeviceInstance> devices;
    devices.reserve(count);

    if (!g_headless) {
        printf("\n\033[1;32m");  // Bold green
        printf("Creating %d device%s...\n", count, count == 1 ? "" : "s");
        printf("\033[0m");
    }

    for (int i = 0; i < count; i++) {
        // All hunters so SerialCableBroker uses PRIMARY→AUXILIARY cabling
        // uniformly; alternating roles would double-book PRIMARY jacks.
        bool isHunter = true;
        devices.push_back(cli::DeviceFactory::createDevice(i, isHunter));

        if (!g_headless) {
            printf("  Device %s: %s\n",
                   devices.back().deviceId.c_str(),
                   isHunter ? "Hunter" : "Bounty");
        }
    }

    if (!g_headless) {
        printf("\n");
        printf("Press any key to start simulation...\n");
        fflush(stdout);

        // Wait for keypress (blocking read)
        #ifndef _WIN32
        getchar();
        #else
        _getch();
        #endif
    }

    return devices;
}

static void runHeadless(std::vector<cli::DeviceInstance>& devices, cli::Renderer& renderer) {
    cli::CommandProcessor commandProcessor;
    int selectedDevice = 0;

    printf("ready devices=%zu pid=%d\n", devices.size(), (int)getpid());
    fflush(stdout);

    bool eventsEnabled = true;
    cli::EventTap::subscribe([&eventsEnabled](const cli::SimEvent& e) {
        if (!eventsEnabled) return;
        std::string line = "event ts=" + std::to_string(e.timestampMs);
        if (e.deviceIndex >= 0) {
            line += " device=" + std::to_string(e.deviceIndex);
        }
        line += " kind=" + e.kind;
        for (const auto& [k, v] : e.kv) {
            line += " " + k + "=";
            bool needsQuote = v.find(' ') != std::string::npos || v.find('"') != std::string::npos;
            if (needsQuote) {
                std::string esc = v;
                for (size_t i = 0; i < esc.size(); ++i) {
                    if (esc[i] == '"') { esc.insert(i, "\\"); ++i; }
                }
                line += "\"" + esc + "\"";
            } else {
                line += v;
            }
        }
        line += "\n";
        fputs(line.c_str(), stdout);
        fflush(stdout);
    });

    std::string line;
    bool stdinOpen = true;

    while (g_running) {
        struct pollfd pfd{};
        pfd.fd = STDIN_FILENO;
        pfd.events = POLLIN;
        int pr = poll(&pfd, 1, 0);
        if (pr > 0 && (pfd.revents & POLLIN)) {
            char buf[1024];
            ssize_t n = read(STDIN_FILENO, buf, sizeof(buf));
            if (n <= 0) {
                stdinOpen = false;
            } else {
                line.append(buf, buf + n);
            }
        }

        size_t nl;
        while ((nl = line.find('\n')) != std::string::npos) {
            std::string cmd = line.substr(0, nl);
            line.erase(0, nl + 1);
            if (!cmd.empty() && cmd.back() == '\r') cmd.pop_back();

            if (cmd.empty()) continue;

            auto result = commandProcessor.execute(cmd, devices, selectedDevice, renderer);
            std::string escaped;
            escaped.reserve(result.message.size());
            for (char c : result.message) {
                if (c == '\n') { escaped += "\\n"; }
                else if (c == '\r') { /* drop */ }
                else { escaped += c; }
            }
            printf("%s: %s\n", result.success ? "ok" : "err", escaped.c_str());
            fflush(stdout);

            if (result.shouldQuit) {
                g_running = false;
                break;
            }
        }

        if (!g_running) break;

        if (!stdinOpen && line.empty()) {
            g_running = false;
            break;
        }

        NativePeerBroker::getInstance().deliverPackets();
        cli::SerialCableBroker::getInstance().transferData();
        for (auto& device : devices) {
            device.pdn->loop();
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }
}

int main(int argc, char** argv) {
    // Set up signal handler for graceful shutdown
    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);
    
    // Create global clock and logger
    NativeClockDriver* globalClock = new NativeClockDriver("global_clock");
    NativeLoggerDriver* globalLogger = new NativeLoggerDriver("global_logger");
    
    // Suppress logger output unless file-logging is requested
    globalLogger->setSuppressOutput(getenv("PDN_CLI_LOG_FILE") == nullptr);
    
    // Set up global platform abstractions
    g_logger = globalLogger;
    SimpleTimer::setPlatformClock(globalClock);
    
    // Initialize the ID generator singleton
    IdGenerator::initialize(globalClock->milliseconds());
    
    // Determine device count from args or prompt
    int deviceCount = parseArgs(argc, argv);

    // Renderer is constructed once and shared by both branches.
    // In headless mode it is never repainted but commands that need it
    // (state, display, mirror, captions) receive a valid reference.
    cli::Renderer renderer;

    if (g_headless) {
        if (deviceCount < 0) {
            fprintf(stderr, "headless mode requires a device count argument\n");
            return 1;
        }
        std::vector<cli::DeviceInstance> devices = createDevices(deviceCount);
        runHeadless(devices, renderer);
        for (auto& device : devices) {
            cli::DeviceFactory::destroyDevice(device);
        }
    } else {
        // Show header (before raw mode)
        cli::Terminal::clearScreen();
        cli::printHeader();

        if (deviceCount < 0) {
            deviceCount = promptDeviceCount();
        }

        // Create devices
        std::vector<cli::DeviceInstance> devices = createDevices(deviceCount);

        // Now configure terminal for non-blocking input (Unix only)
        #ifndef _WIN32
        struct termios oldTermios = cli::Terminal::enableRawMode();
        #endif

        // Set up display for simulation
        cli::Terminal::clearScreen();
        cli::Terminal::hideCursor();
        cli::printHeader();

        cli::CommandProcessor commandProcessor;

        // Show initial help hint
        g_commandResult = "Ready! Use LEFT/RIGHT to select device, UP/DOWN for buttons. Type 'help' for commands.";

        // Main loop
        while (g_running) {
            // Handle input (non-blocking)
            int key = cli::Terminal::readKey();
            while (key != static_cast<int>(cli::Key::NONE)) {
                if (key == static_cast<int>(cli::Key::ARROW_UP)) {
                    // Up arrow = primary button click on selected device
                    if (g_selectedDevice >= 0 && g_selectedDevice < static_cast<int>(devices.size())) {
                        devices[g_selectedDevice].primaryButtonDriver->execCallback(ButtonInteraction::CLICK);
                        g_commandResult = "Button1 click on " + devices[g_selectedDevice].deviceId;
                    }
                } else if (key == static_cast<int>(cli::Key::ARROW_DOWN)) {
                    // Down arrow = secondary button click on selected device
                    if (g_selectedDevice >= 0 && g_selectedDevice < static_cast<int>(devices.size())) {
                        devices[g_selectedDevice].secondaryButtonDriver->execCallback(ButtonInteraction::CLICK);
                        g_commandResult = "Button2 click on " + devices[g_selectedDevice].deviceId;
                    }
                } else if (key == static_cast<int>(cli::Key::ARROW_LEFT)) {
                    // Left arrow = select previous device
                    if (g_selectedDevice > 0) {
                        g_selectedDevice--;
                        g_commandResult = "Selected device " + devices[g_selectedDevice].deviceId;
                    }
                } else if (key == static_cast<int>(cli::Key::ARROW_RIGHT)) {
                    // Right arrow = select next device
                    if (g_selectedDevice < static_cast<int>(devices.size()) - 1) {
                        g_selectedDevice++;
                        g_commandResult = "Selected device " + devices[g_selectedDevice].deviceId;
                    }
                } else if (key == '\n' || key == '\r') {
                    // Execute command on Enter
                    auto result = commandProcessor.execute(g_commandBuffer, devices, g_selectedDevice, renderer);
                    g_commandResult = result.message;
                    if (result.shouldQuit) {
                        g_running = false;
                    }
                    g_commandBuffer.clear();
                } else if (key == 127 || key == '\b') {
                    // Backspace - remove last character
                    if (!g_commandBuffer.empty()) {
                        g_commandBuffer.pop_back();
                    }
                } else if (key == 27) {
                    // Escape - clear command buffer
                    g_commandBuffer.clear();
                    g_commandResult.clear();
                } else if (key == 3) {
                    // Ctrl+C - quit
                    g_running = false;
                } else if (key >= 32 && key < 127) {
                    // Printable character - add to buffer
                    g_commandBuffer += static_cast<char>(key);
                }
                key = cli::Terminal::readKey();
            }

            // Deliver pending peer-to-peer messages
            NativePeerBroker::getInstance().deliverPackets();

            // Transfer serial data between connected devices
            cli::SerialCableBroker::getInstance().transferData();

            // Update all devices
            for (auto& device : devices) {
                device.pdn->loop();
            }

            // Render UI
            renderer.renderUI(devices, g_commandResult, g_commandBuffer, g_selectedDevice);

            // Sleep to maintain ~30 FPS update rate
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }

        // Restore terminal settings and cursor
        cli::Terminal::showCursor();

        // Move cursor below the UI for clean shutdown message
        printf("\n\nShutting down...\n");

        for (auto& device : devices) {
            cli::DeviceFactory::destroyDevice(device);
        }
    }

    delete globalLogger;
    delete globalClock;

    return 0;
}

#endif // NATIVE_BUILD
