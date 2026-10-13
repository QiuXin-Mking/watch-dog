#include "sensor.h"

#include <glob.h>
#include <unistd.h>

#include <chrono>
#include <fstream>
#include <optional>

#include "log.h"
#include "state.h"

namespace {

// First whitespace-delimited token of a sysfs file (hwmom `name`, thermal
// `type`, or a bare integer value). Empty on open/read failure.
std::string read_token(const std::string& path) {
    std::ifstream in(path);
    std::string s;
    if (in) in >> s;
    return s;
}

std::optional<long> read_long(const std::string& path) {
    std::ifstream in(path);
    long v = 0;
    if (!(in >> v)) return std::nullopt;
    return v;
}

// Directory of the hwmon whose `name` equals `name`, or "" when absent. Uses
// glob + name matching (not a hard-coded index) because hwmonN order drifts
// across boots.
std::string find_hwmon(const std::string& name) {
    glob_t g;
    if (glob("/sys/class/hwmon/hwmon*/name", 0, nullptr, &g) != 0) return "";
    std::string dir;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        if (read_token(g.gl_pathv[i]) == name) {
            dir = g.gl_pathv[i];
            dir.resize(dir.size() - 5);  // strip "/name"
            break;
        }
    }
    globfree(&g);
    return dir;
}

// Directory of the thermal_zone whose `type` equals `type`, or "" when absent.
std::string find_thermal_zone(const std::string& type) {
    glob_t g;
    if (glob("/sys/class/thermal/thermal_zone*/type", 0, nullptr, &g) != 0)
        return "";
    std::string dir;
    for (size_t i = 0; i < g.gl_pathc; ++i) {
        if (read_token(g.gl_pathv[i]) == type) {
            dir = g.gl_pathv[i];
            dir.resize(dir.size() - 5);  // strip "/type"
            break;
        }
    }
    globfree(&g);
    return dir;
}

}  // namespace

void run_fan_loop(State& state, const FanConfig& cfg,
                  const std::atomic<bool>& running) {
    using clock = std::chrono::steady_clock;

    const std::string hwmon = find_hwmon(cfg.fan_hwmon_name);
    if (hwmon.empty()) {
        log_warn("fan: hwmon name '%s' not found -> fan channel stays healthy",
                 cfg.fan_hwmon_name.c_str());
    }
    const std::string zone = find_thermal_zone(cfg.thermal_zone_type);
    if (zone.empty()) {
        log_warn("fan: thermal_zone type '%s' not found -> overtemp channel "
                 "stays healthy",
                 cfg.thermal_zone_type.c_str());
    }

    clock::time_point stall_since{};
    while (running.load()) {
        bool fan_healthy = true;
        bool overtemp = false;

        // Fan stall: PWM commands the fan (pwm1 > 0) but the tach reads 0 for
        // >= stall_seconds. Only judged when BOTH files read cleanly, so a
        // missing tach path degrades to healthy rather than a false stall.
        if (!hwmon.empty()) {
            const auto pwm = read_long(hwmon + "/pwm1");
            const auto rpm = read_long(hwmon + "/fan1_input");
            if (pwm && rpm) {
                const bool commanded = *pwm > 0;
                const bool stalled = commanded && *rpm <= 0;
                if (stalled) {
                    if (stall_since == clock::time_point{})
                        stall_since = clock::now();
                } else {
                    stall_since = clock::time_point{};
                }
                const bool stall_fault =
                    stall_since != clock::time_point{} &&
                    clock::now() - stall_since >=
                        std::chrono::seconds(cfg.stall_seconds);
                if (stall_fault) {
                    log_warn("fan: STALL pwm=%ld fan1_input=%ld (>=%ds)",
                             *pwm, *rpm, cfg.stall_seconds);
                }
                fan_healthy = !stall_fault;
            }
        }

        // Overtemp: temperature above the threshold (millidegrees C).
        if (!zone.empty()) {
            const auto temp = read_long(zone + "/temp");
            if (temp) {
                overtemp = *temp > cfg.overtemp_threshold_mc;
                if (overtemp) {
                    log_warn("overtemp: temp=%ld mC > %d mC", *temp,
                             cfg.overtemp_threshold_mc);
                }
            }
        }

        state.set_sensor_present("fan", fan_healthy);
        state.set_sensor_present("overtemp", !overtemp);

        // Interruptible sleep: wake within ~1s of `running` being cleared.
        for (int i = 0; i < cfg.poll_seconds && running.load(); ++i) {
            usleep(1000000);
        }
    }
    log_info("fan loop exit");
}
