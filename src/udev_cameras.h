#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "spec.h"
#include "state.h"

// Raw USB-device properties used for matching.
struct UsbDeviceProps {
    uint16_t vid = 0;
    uint16_t pid = 0;
    std::string product;  // ID_MODEL  (fallback: sysfs "product")
    std::string serial;   // ID_SERIAL_SHORT (fallback: sysfs "serial")
    std::string syspath;  // /sys/devices/... unique per physical port
};

// Enumerate currently present USB devices (subsystem usb, DEVTYPE usb_device)
// that expose a vendor id. Includes non-camera devices (caller filters).
std::vector<UsbDeviceProps> enumerate_usb_devices();

// Does this device match this spec? VID/PID and the configured match rule must agree.
bool spec_matches(const CameraSpec& spec, const UsbDeviceProps& dev);

// Index of the first spec matched by dev, or -1 when it matches nothing.
int match_first_spec(const std::vector<CameraSpec>& specs, const UsbDeviceProps& dev);

// Wraps a udev_monitor (subsystem "usb", devtype "usb_device") fed by the udev
// daemon so events carry udev-computed properties. The fd is set non-blocking.
class UdevMonitor {
public:
    UdevMonitor() = default;
    ~UdevMonitor();

    UdevMonitor(const UdevMonitor&) = delete;
    UdevMonitor& operator=(const UdevMonitor&) = delete;

    bool init();
    int fd() const { return fd_; }
    void shutdown();

    // Drain every queued event and update the shared State (add/remove).
    void process_events(State& state);

private:
    struct udev* ctx_ = nullptr;
    struct udev_monitor* mon_ = nullptr;
    int fd_ = -1;
};
