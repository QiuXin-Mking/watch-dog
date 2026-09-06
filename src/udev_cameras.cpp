#include "udev_cameras.h"

#include <fcntl.h>
#include <libudev.h>
#include <unistd.h>

#include <cstring>

#include "log.h"

namespace {

// udev property first, raw sysfs attribute as a fallback (both are the same on
// a fully-processed device; sysfs fallback keeps enumeration working even when
// the udev database has not tagged a freshly-hotplugged device yet).
const char* prop_or_attr(struct udev_device* dev, const char* prop, const char* attr) {
    const char* v = udev_device_get_property_value(dev, prop);
    if (v && *v) return v;
    const char* a = udev_device_get_sysattr_value(dev, attr);
    if (a) return a;
    return "";
}

UsbDeviceProps extract(struct udev_device* dev) {
    UsbDeviceProps p;
    if (!dev) return p;
    const char* syspath = udev_device_get_syspath(dev);
    if (syspath) p.syspath = syspath;

    p.vid = 0;
    p.pid = 0;
    parse_hex16(prop_or_attr(dev, "ID_VENDOR_ID", "idVendor"), p.vid);
    parse_hex16(prop_or_attr(dev, "ID_MODEL_ID", "idProduct"), p.pid);
    p.product = prop_or_attr(dev, "ID_MODEL", "product");
    p.serial = prop_or_attr(dev, "ID_SERIAL_SHORT", "serial");
    return p;
}

}  // namespace

std::vector<UsbDeviceProps> enumerate_usb_devices() {
    std::vector<UsbDeviceProps> out;
    struct udev* ctx = udev_new();
    if (!ctx) return out;

    struct udev_enumerate* en = udev_enumerate_new(ctx);
    udev_enumerate_add_match_subsystem(en, "usb");
    udev_enumerate_scan_devices(en);

    struct udev_list_entry* devices = udev_enumerate_get_list_entry(en);
    struct udev_list_entry* entry = nullptr;
    udev_list_entry_foreach(entry, devices) {
        const char* syspath = udev_list_entry_get_name(entry);
        if (!syspath) continue;
        struct udev_device* dev = udev_device_new_from_syspath(ctx, syspath);
        if (!dev) continue;
        const char* devtype = udev_device_get_devtype(dev);
        if (!devtype || std::strcmp(devtype, "usb_device") != 0) {
            udev_device_unref(dev);
            continue;
        }
        UsbDeviceProps p = extract(dev);
        if (p.vid != 0) out.push_back(std::move(p));
        udev_device_unref(dev);
    }

    udev_enumerate_unref(en);
    udev_unref(ctx);
    return out;
}

bool spec_matches(const CameraSpec& spec, const UsbDeviceProps& dev) {
    if (spec.vid != dev.vid || spec.pid != dev.pid) return false;
    // New-firmware SN prefix takes priority (serial starts with e.g. "SL").
    if (!spec.sn_prefix.empty() && dev.serial.size() >= spec.sn_prefix.size() &&
        dev.serial.compare(0, spec.sn_prefix.size(), spec.sn_prefix) == 0) {
        return true;
    }
    // Old-firmware fallback.
    if (spec.fallback.type == FallbackRule::Type::kPid) {
        return true;  // vid+pid already matched
    }
    if (spec.fallback.type == FallbackRule::Type::kProduct) {
        return dev.product == spec.fallback.product;
    }
    return false;
}

std::string match_first_spec(const std::vector<CameraSpec>& specs, const UsbDeviceProps& dev) {
    for (const auto& s : specs) {
        if (spec_matches(s, dev)) return s.name;
    }
    return {};
}

UdevMonitor::~UdevMonitor() {
    shutdown();
}

bool UdevMonitor::init() {
    if (mon_) return true;
    ctx_ = udev_new();
    if (!ctx_) {
        log_error("udev: udev_new failed");
        return false;
    }
    // "udev" netlink = events already processed by the udev daemon, so the
    // ID_* properties are populated. ("kernel" netlink would give raw events.)
    mon_ = udev_monitor_new_from_netlink(ctx_, "udev");
    if (!mon_) {
        log_error("udev: udev_monitor_new_from_netlink failed");
        shutdown();
        return false;
    }
    udev_monitor_filter_add_match_subsystem_devtype(mon_, "usb", "usb_device");
    if (udev_monitor_enable_receiving(mon_) < 0) {
        log_error("udev: udev_monitor_enable_receiving failed");
        shutdown();
        return false;
    }
    fd_ = udev_monitor_get_fd(mon_);
    // Drain all pending events without blocking.
    int flags = fcntl(fd_, F_GETFL, 0);
    fcntl(fd_, F_SETFL, flags | O_NONBLOCK);
    return true;
}

void UdevMonitor::shutdown() {
    if (mon_) {
        udev_monitor_unref(mon_);
        mon_ = nullptr;
    }
    if (ctx_) {
        udev_unref(ctx_);
        ctx_ = nullptr;
    }
    fd_ = -1;
}

void UdevMonitor::process_events(State& state) {
    while (true) {
        struct udev_device* dev = udev_monitor_receive_device(mon_);
        if (!dev) break;
        const char* action = udev_device_get_action(dev);
        if (action) {
            UsbDeviceProps p = extract(dev);
            if (std::strcmp(action, "remove") == 0) {
                std::string removed_name;
                bool handled = state.remove_device_by_syspath(p.syspath,
                                                              &removed_name);
                if (handled) {
                    log_info("udev remove syspath=%s name=%s",
                             p.syspath.c_str(), removed_name.c_str());
                } else {
                    std::string name = match_first_spec(state.specs(), p);
                    if (!name.empty()) {
                        log_info("udev remove(by props) syspath=%s name=%s",
                                 p.syspath.c_str(), name.c_str());
                        state.remove_named(name);
                    }
                    // else: non-target USB device removal -> ignore (R7).
                }
            } else if (std::strcmp(action, "add") == 0) {
                std::string name = match_first_spec(state.specs(), p);
                if (!name.empty()) {
                    log_info("udev add syspath=%s name=%s", p.syspath.c_str(),
                             name.c_str());
                    state.add_present_device(name, p.syspath);
                }
            }
            // "change"/"bind"/"unbind"/"move" are irrelevant for us.
        }
        udev_device_unref(dev);
    }
}
