// SEA wet-process equipment simulator: acts as the "PLC" side of the demo.
// Runs a Modbus TCP server and advances an internal fill -> heat -> drain
// recipe state machine on a ~200ms timer, independent of client connections.

#include <modbus.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <thread>

#ifdef _WIN32
// modbus-tcp.h (pulled in via modbus.h) already includes <winsock2.h> on
// Windows, which provides closesocket(). <unistd.h> is POSIX-only and
// doesn't exist under MSVC.
#else
#include <unistd.h>
#endif

namespace {

constexpr int MODBUS_PORT = 502;

// Holding register map (function codes 03 / 06 / 16).
constexpr int REG_FILL_LEVEL = 0;   // 0-100 (%)
constexpr int REG_TEMPERATURE = 1;  // 0-100 (deg C)
constexpr int REG_PHASE = 2;        // 0=idle 1=filling 2=heating 3=draining
constexpr int NUM_REGISTERS = 3;

// Coil map (function codes 01 / 05 / 15).
constexpr int COIL_ALARM = 0;    // set by server when temperature > 65C
constexpr int COIL_CONTROL = 1;  // written by client: 1=start, 0=stop/reset
constexpr int NUM_COILS = 2;

enum Phase : uint16_t {
    PHASE_IDLE = 0,
    PHASE_FILLING = 1,
    PHASE_HEATING = 2,
    PHASE_DRAINING = 3,
};

constexpr int TICK_MS = 200;
constexpr double FILL_DURATION_S = 5.0;
constexpr double HEAT_DURATION_S = 8.0;
constexpr double DRAIN_DURATION_S = 5.0;
constexpr double HEAT_TARGET_C = 60.0;
constexpr double ALARM_THRESHOLD_C = 65.0;

// Per-tick step magnitudes, derived from the full travel distance of each
// ramp so both filling/draining (100-unit range) and heating (60-degree
// range) complete in their documented duration.
constexpr double FILL_STEP = 100.0 / FILL_DURATION_S * (TICK_MS / 1000.0);
constexpr double HEAT_STEP = HEAT_TARGET_C / HEAT_DURATION_S * (TICK_MS / 1000.0);
constexpr double DRAIN_STEP = 100.0 / DRAIN_DURATION_S * (TICK_MS / 1000.0);

std::atomic<bool> g_running{true};

void handle_signal(int) { g_running = false; }

// Moves `current` toward `target` by `step_magnitude`, clamping so it never
// overshoots. `step_magnitude` is always a positive distance-per-tick; the
// direction is inferred from where `target` sits relative to `current`.
double step_toward(double current, double target, double step_magnitude) {
    if (target >= current) {
        double next = current + step_magnitude;
        return next >= target ? target : next;
    }
    double next = current - step_magnitude;
    return next <= target ? target : next;
}

// Runs the recipe state machine, updating the shared Modbus mapping every
// tick. Guarded by mapping_mutex since the network thread reads/writes the
// same mapping while serving requests.
//
// Fill level and temperature are tracked as full-precision doubles across
// ticks (not re-derived from the rounded uint16 registers each time) --
// ramp steps like the 1.5-per-tick heating rate have a fractional part,
// and re-reading the truncated register every tick would silently discard
// that fraction, compounding into the wrong overall ramp duration. To
// still let a Modbus test tool force a value directly (e.g. writing the
// temperature register above 65 during Heating to exercise the alarm
// path), each tick compares the register against what the simulation
// itself last wrote there: a mismatch means an external write happened,
// so that value is adopted as the new baseline.
void run_simulation(modbus_mapping_t *mapping, std::mutex &mapping_mutex) {
    Phase phase = PHASE_IDLE;
    bool alarm = false;
    bool prev_control = false;
    double fill = 0.0;
    double temp = 0.0;
    uint16_t last_written_fill = 0;
    uint16_t last_written_temp = 0;

    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(TICK_MS));

        std::lock_guard<std::mutex> lock(mapping_mutex);
        bool control = mapping->tab_bits[COIL_CONTROL] != 0;

        if (mapping->tab_registers[REG_FILL_LEVEL] != last_written_fill) {
            fill = mapping->tab_registers[REG_FILL_LEVEL];
        }
        if (mapping->tab_registers[REG_TEMPERATURE] != last_written_temp) {
            temp = mapping->tab_registers[REG_TEMPERATURE];
        }

        if (alarm) {
            // Held here until the client writes 0 (stop/reset) to the control coil.
            if (!control) {
                alarm = false;
                phase = PHASE_IDLE;
                fill = 0.0;
                temp = 0.0;
            }
        } else if (!control) {
            // Stop/reset requested: abort whatever phase we're in back to idle.
            if (phase != PHASE_IDLE) {
                phase = PHASE_IDLE;
                fill = 0.0;
                temp = 0.0;
            }
        } else if (!prev_control && phase == PHASE_IDLE) {
            // Rising edge on the control coil starts a new cycle.
            phase = PHASE_FILLING;
        } else {
            switch (phase) {
                case PHASE_FILLING:
                    fill = step_toward(fill, 100.0, FILL_STEP);
                    if (fill >= 100.0) {
                        phase = PHASE_HEATING;
                    }
                    break;
                case PHASE_HEATING:
                    temp = step_toward(temp, HEAT_TARGET_C, HEAT_STEP);
                    if (temp > ALARM_THRESHOLD_C) {
                        alarm = true;
                    } else if (temp >= HEAT_TARGET_C) {
                        phase = PHASE_DRAINING;
                    }
                    break;
                case PHASE_DRAINING:
                    fill = step_toward(fill, 0.0, DRAIN_STEP);
                    if (fill <= 0.0) {
                        phase = PHASE_IDLE;
                        fill = 0.0;
                        temp = 0.0;
                    }
                    break;
                default:
                    break;
            }
        }

        prev_control = control;

        last_written_fill = static_cast<uint16_t>(fill + 0.5);
        last_written_temp = static_cast<uint16_t>(temp + 0.5);
        mapping->tab_registers[REG_FILL_LEVEL] = last_written_fill;
        mapping->tab_registers[REG_TEMPERATURE] = last_written_temp;
        mapping->tab_registers[REG_PHASE] = static_cast<uint16_t>(phase);
        mapping->tab_bits[COIL_ALARM] = alarm ? 1 : 0;
    }
}

} // namespace

int main() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);

    modbus_t *ctx = modbus_new_tcp("127.0.0.1", MODBUS_PORT);
    if (!ctx) {
        std::cerr << "Failed to create Modbus TCP context\n";
        return 1;
    }

    modbus_mapping_t *mapping = modbus_mapping_new(NUM_COILS, 0, NUM_REGISTERS, 0);
    if (!mapping) {
        std::cerr << "Failed to allocate Modbus mapping: " << modbus_strerror(errno) << "\n";
        modbus_free(ctx);
        return 1;
    }

    int server_socket = modbus_tcp_listen(ctx, 1);
    if (server_socket == -1) {
        std::cerr << "Failed to listen on port " << MODBUS_PORT << ": " << modbus_strerror(errno) << "\n";
        modbus_mapping_free(mapping);
        modbus_free(ctx);
        return 1;
    }

    std::cout << "SEA PLC simulator listening on 127.0.0.1:" << MODBUS_PORT << "\n";
    std::cout << "Holding registers: fill_level=" << REG_FILL_LEVEL
              << " temperature=" << REG_TEMPERATURE << " phase=" << REG_PHASE << "\n";
    std::cout << "Coils: alarm=" << COIL_ALARM << " control=" << COIL_CONTROL << "\n";

    std::mutex mapping_mutex;
    std::thread sim_thread(run_simulation, mapping, std::ref(mapping_mutex));

    uint8_t query[MODBUS_TCP_MAX_ADU_LENGTH];

    while (g_running) {
        int client_socket = modbus_tcp_accept(ctx, &server_socket);
        if (client_socket == -1) {
            continue;
        }
        std::cout << "HMI client connected\n";

        while (g_running) {
            int rc = modbus_receive(ctx, query);
            if (rc > 0) {
                std::lock_guard<std::mutex> lock(mapping_mutex);
                modbus_reply(ctx, query, rc, mapping);
            } else if (rc == -1) {
                std::cout << "HMI client disconnected\n";
                break;
            }
        }
        modbus_close(ctx);
    }

    g_running = false;
    sim_thread.join();

#ifdef _WIN32
    closesocket(server_socket);
#else
    close(server_socket);
#endif
    modbus_mapping_free(mapping);
    modbus_free(ctx);
    return 0;
}
