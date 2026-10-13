#pragma once

#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

// Non-udev "sensor" alarm channel (fan stall / overtemp), polled from sysfs by
// the fan thread. It shares the State present/missing + ack machinery with the
// camera channels: present == healthy, missing == fault. `name` is the channel
// key ("fan" / "overtemp").
struct SensorChannel {
    std::string name;
    std::string alarm_wav;
    std::string recover_wav;
};

// fan.conf tunables. When the file is missing (or a value is absent/illegal)
// the defaults below stand, so the watchdog never fails to start over config.
struct FanConfig {
    int overtemp_threshold_mc = 80000;      // 80°C (sysfs temp is millidegrees C)
    int stall_seconds = 10;                 // pwm>0 && fan==0 sustained window
    int poll_seconds = 5;                   // sysfs poll interval
    std::string fan_hwmon_name = "pwmfan";
    std::string thermal_zone_type = "little-core-thermal";
    std::string fan_alarm_wav = "风扇故障.wav";
    std::string fan_recover_wav = "风扇恢复.wav";
    std::string overtemp_alarm_wav = "过温.wav";
    std::string overtemp_recover_wav = "温度恢复.wav";
};

// Parse fan.conf (`key = value`, '#' comments). Missing file / unknown keys /
// malformed values leave the corresponding default in place and never error.
FanConfig parse_fan_conf(const std::string& path);

class State;

// Poll fan/temperature sysfs every poll_seconds and drive the fan/overtemp
// sensor channels in `state` until `running` is cleared. A channel whose sysfs
// path cannot be resolved degrades to present=true (journald-only, no alarm).
void run_fan_loop(State& state, const FanConfig& cfg,
                  const std::atomic<bool>& running);
