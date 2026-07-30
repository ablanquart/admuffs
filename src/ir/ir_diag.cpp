// SPDX-License-Identifier: MIT
#include "ir/ir_diag.h"
#include "config.h"
#include "common.h"
#include "ir/ir_database.h"

#include <cerrno>
#include <fcntl.h>
#include <linux/lirc.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace admuffs {

namespace {
bool cmd_exists(const char* cmd) {
    std::string c = std::string("command -v ") + cmd + " >/dev/null 2>&1";
    return system(c.c_str()) == 0;
}
}  // namespace

int run_ir_check(const Config& cfg) {
    printf("admuffs IR hardware check\n");
    printf("========================\n\n");

    // 1) Kernel devices created by the gpio-ir / gpio-ir-tx overlays.
    printf("[1] /dev/lirc* devices (created by the dtoverlay lines)\n");
    int found = 0, tx_capable = 0, perm_denied = 0;
    std::string first_tx;
    for (int i = 0; i < 4; ++i) {
        std::string dev = "/dev/lirc" + std::to_string(i);
        struct stat st;
        if (stat(dev.c_str(), &st) != 0) continue;  // truly absent
        int fd = open(dev.c_str(), O_RDONLY | O_NONBLOCK);
        if (fd < 0) {
            found++;  // the node exists; we just couldn't probe its features
            if (errno == EACCES || errno == EPERM) {
                perm_denied++;
                printf("    %s : exists but PERMISSION DENIED for this user\n", dev.c_str());
            } else if (errno == EBUSY) {
                printf("    %s : exists (busy -- in use by another process, likely lircd)\n",
                       dev.c_str());
            } else {
                printf("    %s : exists but open failed (%s)\n", dev.c_str(), strerror(errno));
            }
            continue;
        }
        found++;
        uint32_t feats = 0;
        if (ioctl(fd, LIRC_GET_FEATURES, &feats) == 0) {
            bool can_send = feats & LIRC_CAN_SEND_PULSE;
            bool can_rec  = feats & LIRC_CAN_REC_MODE2;
            printf("    %s : %s%s%s\n", dev.c_str(),
                   can_send ? "TRANSMIT" : "",
                   (can_send && can_rec) ? " + " : "",
                   can_rec ? "RECEIVE" : "");
            if (can_send && !tx_capable) { tx_capable = 1; first_tx = dev; }
        } else {
            printf("    %s : present (features query failed)\n", dev.c_str());
        }
        close(fd);
    }
    if (!found) {
        printf("    NONE FOUND.\n");
        printf("    -> The IR overlays are not active. The ANAVI pHAT's IR parts are\n");
        printf("       plain GPIO (LED=GPIO17, receiver=GPIO18) -- NOT I2C. I2C is only\n");
        printf("       for the pHAT's optional sensors. Add to /boot/firmware/config.txt:\n");
        printf("           dtoverlay=gpio-ir,gpio_pin=18\n");
        printf("           dtoverlay=gpio-ir-tx,gpio_pin=17\n");
        printf("       then REBOOT -- overlays only load at boot.\n");
        printf("       (scripts/setup-pi.sh adds these lines for you.)\n");
        printf("       If the lines are already there and you rebooted, check:\n");
        printf("           dmesg | grep -iE 'gpio-ir|lirc'\n");
    } else if (perm_denied && !tx_capable) {
        printf("    -> Devices exist but this user cannot open them. Fix with:\n");
        printf("           sudo usermod -aG video %s\n", getenv("USER") ? getenv("USER") : "<user>");
        printf("       then log out and back in (or re-run this check with sudo).\n");
    } else if (!tx_capable) {
        printf("    -> Devices exist but none can TRANSMIT. Check the gpio-ir-tx overlay.\n");
    } else {
        printf("    -> Transmitter present at %s. The HAT is detected.\n", first_tx.c_str());
    }

    // 2) Userspace transmit tools.
    printf("\n[2] transmit backends\n");
    bool has_irsend = cmd_exists("irsend");
    bool has_irctl  = cmd_exists("ir-ctl");
    printf("    irsend (lirc)   : %s\n", has_irsend ? "installed" : "NOT installed (apt install lirc)");
    printf("    ir-ctl (rc-core): %s\n", has_irctl ? "installed" : "NOT installed (apt install v4l-utils)");

    // lircd's own driver/device config: the Debian default 'devinput' driver
    // is receive-only, so irsend fails with "hardware does not support
    // sending" no matter which device exists.
    if (has_irsend) {
        std::string drv, dev;
        FILE* f = fopen("/etc/lirc/lirc_options.conf", "r");
        if (f) {
            char line[256];
            while (fgets(line, sizeof(line), f)) {
                std::string s = trim(line);
                if (s.rfind("driver", 0) == 0) {
                    size_t eq = s.find('=');
                    if (eq != std::string::npos) drv = trim(s.substr(eq + 1));
                } else if (s.rfind("device", 0) == 0) {
                    size_t eq = s.find('=');
                    if (eq != std::string::npos) dev = trim(s.substr(eq + 1));
                }
            }
            fclose(f);
            printf("    lircd config    : driver=%s device=%s\n",
                   drv.empty() ? "?" : drv.c_str(), dev.empty() ? "?" : dev.c_str());
            if (drv == "devinput") {
                printf("    ! lircd uses the 'devinput' driver, which CANNOT transmit\n");
                printf("      (this is the Debian default). irsend will fail with\n");
                printf("      \"hardware does not support sending\". Fix in\n");
                printf("      /etc/lirc/lirc_options.conf:\n");
                printf("          driver = default\n");
                printf("          device = %s   (your TRANSMIT device from [1])\n",
                       first_tx.empty() ? "/dev/lircX" : first_tx.c_str());
                printf("      then: sudo systemctl restart lircd\n");
            } else if (!first_tx.empty() && !dev.empty() && dev != "auto" && dev != first_tx) {
                printf("    ! lircd is bound to %s, but the TRANSMIT device is %s --\n",
                       dev.c_str(), first_tx.c_str());
                printf("      irsend will fail. Point 'device' at %s in\n", first_tx.c_str());
                printf("      /etc/lirc/lirc_options.conf and restart lircd.\n");
            }
        }
    }

    // 3) What admuffs is actually configured to do.
    printf("\n[3] admuffs configuration\n");
    printf("    ir_backend = %s%s\n", cfg.ir_backend.c_str(),
           cfg.ir_backend == "dryrun"
               ? "   <-- DRYRUN NEVER TRANSMITS; set irsend or ir-ctl" : "");
    printf("    ir_device  = %s\n", cfg.ir_device.c_str());
    if (cfg.ir_backend == "irsend" && !has_irsend)
        printf("    ! backend is irsend but the tool is missing -> admuffs silently dry-runs\n");
    if (cfg.ir_backend == "ir-ctl" && !has_irctl)
        printf("    ! backend is ir-ctl but the tool is missing -> admuffs silently dry-runs\n");

    // 4) Does the selected TV profile have usable codes for that backend?
    printf("\n[4] IR codes for %s/%s\n", cfg.tv_brand.c_str(), cfg.tv_model.c_str());
    bool codes_ok = false;  // must be proven usable for the chosen backend
    IrDatabase db;
    db.load(cfg.ir_db_dir);
    const IrTvProfile* p = db.find(cfg.tv_brand, cfg.tv_model);
    if (!p) {
        printf("    no profile found in %s\n",
               resolve_ir_db_dir(cfg.ir_db_dir).c_str());
    } else {
        const IrCommand* mute = p->command("KEY_MUTE");
        if (!mute) {
            printf("    profile '%s' has no KEY_MUTE\n", p->display_name.c_str());
        } else {
            codes_ok = (cfg.ir_backend == "ir-ctl") ? mute->has_raw()
                     : (cfg.ir_backend == "irsend") ? mute->has_lirc()
                     : true;  // dryrun "sends" anything
            printf("    KEY_MUTE: lirc=[%s / %s]  raw=%zu pulses\n",
                   mute->lirc_remote.c_str(), mute->lirc_button.c_str(),
                   mute->pulses.size());
            if (cfg.ir_backend == "irsend" && mute->has_lirc()) {
                std::string rem = !cfg.ir_remote.empty() ? cfg.ir_remote : mute->lirc_remote;
                std::string chk = "irsend LIST " + rem + " \"\" >/dev/null 2>&1";
                if (has_irsend && system(chk.c_str()) != 0) {
                    codes_ok = false;
                    printf("    ! LIRC has no remote named '%s' configured.\n", rem.c_str());
                    printf("      Record one (irrecord) or install a lircd.conf for your TV,\n");
                    printf("      or switch to the ir-ctl backend with raw codes.\n");
                    printf("      See docs/HARDWARE.md section 3.\n");
                }
            }
            if (cfg.ir_backend == "ir-ctl" && !mute->has_raw())
                printf("    ! ir-ctl backend needs raw pulses; this profile has none.\n"
                       "      Record with 'ir-ctl -r' and add them to the JSON (HARDWARE.md).\n");
        }
    }

    printf("\n[5] live checks you can do\n");
    printf("    - Watch the IR LED through a PHONE CAMERA while running\n");
    printf("      'admuffs --test-mute': the LED flashes visibly on camera.\n");
    printf("    - Prove the receiver works: 'ir-ctl -r -d <rx device>' (or 'mode2'),\n");
    printf("      press buttons on your real remote, pulses should print.\n");

    bool hw_ok = found && tx_capable && cfg.ir_backend != "dryrun" &&
                 ((cfg.ir_backend == "irsend" && has_irsend) ||
                  (cfg.ir_backend == "ir-ctl" && has_irctl));
    bool ok = hw_ok && codes_ok;
    if (ok)
        printf("\nVERDICT: IR transmit path is ready.\n");
    else if (hw_ok && !codes_ok)
        printf("\nVERDICT: hardware + backend are ready, but the selected TV profile\n"
               "has no usable KEY_MUTE codes for the '%s' backend (see [4]).\n",
               cfg.ir_backend.c_str());
    else
        printf("\nVERDICT: IR transmit path NOT ready (see notes above).\n");
    return ok ? 0 : 1;
}

}  // namespace admuffs
