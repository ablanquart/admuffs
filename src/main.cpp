// SPDX-License-Identifier: MIT
// main.cpp - CLI entry point for admuffs.
#include "app.h"
#include "config.h"
#include "common.h"
#include "net/http.h"
#include "ir/ir_database.h"
#include "ir/ir_diag.h"
#include "net/discovery.h"
#include "sys/sysinfo.h"
#include "tui/wizard.h"
#include "version.h"

#include <sys/stat.h>

#include <atomic>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>

using namespace admuffs;

namespace {
std::atomic<bool> g_stop{false};
void on_signal(int) { g_stop = true; }

const char* kVersion = "admuffs " ADMUFFS_VERSION;

void usage(const char* argv0) {
    printf(
        "%s - automatically mute TV commercials (ANAVI Infrared pHAT + Raspberry Pi)\n\n"
        "Usage: %s [command] [options]\n\n"
        "Commands:\n"
        "  --setup            Run the interactive TUI setup wizard (default if no config)\n"
        "  --run              Run the detection/mute loop (default if config exists)\n"
        "  --test-mute        Send one mute+unmute to verify TV control, then exit\n"
        "  --list-tvs         Print the TV brands/models in the IR database\n"
        "  --discover         Scan the LAN for TVs via SSDP\n"
        "  --ir-check         Diagnose the IR hardware + transmit path, then exit\n"
        "  --install-service  Install + enable the systemd unit (needs sudo);\n"
        "                     admuffs then starts on boot and survives restarts\n"
        "  --uninstall-service  Disable and remove the systemd unit (needs sudo)\n"
        "  --version          Print version and exit\n"
        "  --help             Show this help\n\n"
        "Options:\n"
        "  --config PATH      Config file path (default: ~/.config/admuffs/admuffs.conf)\n"
        "  --ir-db DIR        IR code database directory (default: data/ir_codes)\n"
        "  --dry-run          Force IR dry-run backend (no hardware needed)\n"
        "  --verbose          Debug-level logging\n",
        kVersion, argv0);
}
}  // namespace

int main(int argc, char** argv) {
    std::string config_path = default_config_path();
    std::string ir_db_override;
    std::string command;
    bool dry_run = false, verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i];
        auto next = [&](const char* name) -> std::string {
            if (i + 1 >= argc) { fprintf(stderr, "%s needs an argument\n", name); exit(2); }
            return argv[++i];
        };
        if (a == "--setup" || a == "--run" || a == "--test-mute" ||
            a == "--list-tvs" || a == "--discover" || a == "--ir-check" ||
            a == "--install-service" || a == "--uninstall-service") command = a;
        else if (a == "--config") config_path = next("--config");
        else if (a == "--ir-db") ir_db_override = next("--ir-db");
        else if (a == "--dry-run") dry_run = true;
        else if (a == "--verbose") verbose = true;
        else if (a == "--version") { printf("%s\n", kVersion); return 0; }
        else if (a == "--help" || a == "-h") { usage(argv[0]); return 0; }
        else { fprintf(stderr, "unknown argument: %s\n", a.c_str()); usage(argv[0]); return 2; }
    }

    Http::global_init();

    Config cfg;
    bool have_config = cfg.load(config_path);
    if (!ir_db_override.empty()) cfg.ir_db_dir = ir_db_override;
    if (dry_run) cfg.ir_backend = "dryrun";

    // Default the log to a file next to the config, and rotate it if large,
    // so the live log survives across restarts and is inspectable off-box.
    std::string log_path = cfg.log_file;
    if (log_path.empty()) {
        std::string cp = config_path;               // .../admuffs/admuffs.conf
        size_t slash = cp.find_last_of('/');
        if (slash != std::string::npos) log_path = cp.substr(0, slash + 1) + "admuffs.log";
    }
    if (!log_path.empty()) {
        struct stat st;
        if (stat(log_path.c_str(), &st) == 0 && st.st_size > 2 * 1024 * 1024)
            rename(log_path.c_str(), (log_path + ".1").c_str());   // keep one old file
    }
    log_init(verbose ? LogLevel::Debug : static_cast<LogLevel>(cfg.log_level), log_path);
    if (!log_path.empty()) { chmod(log_path.c_str(), S_IRUSR | S_IWUSR); LOG_INFO("logging to %s", log_path.c_str()); }

    // Default command: setup if unconfigured, otherwise run.
    if (command.empty()) command = have_config ? "--run" : "--setup";

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    int rc = 0;
    if (command == "--list-tvs") {
        IrDatabase db; db.load(cfg.ir_db_dir);
        for (const auto& b : db.brands()) {
            printf("%s:\n", b.c_str());
            for (auto* m : db.models_for(b))
                printf("    %-16s %s\n", m->model.c_str(), m->display_name.c_str());
        }
    } else if (command == "--discover") {
        printf("Scanning the LAN via SSDP (Roku + DIAL)...\n");
        auto devs = ssdp_search("roku:ecp", 3000);
        auto dial = ssdp_search("urn:dial-multiscreen-org:service:dial:1", 3000);
        devs.insert(devs.end(), dial.begin(), dial.end());
        if (devs.empty()) printf("  (no devices found)\n");
        for (const auto& d : devs)
            printf("  %-16s %s\n", d.ip.c_str(),
                   (d.server.empty() ? d.st : d.server).c_str());
    } else if (command == "--ir-check") {
        rc = run_ir_check(cfg);
    } else if (command == "--install-service") {
        std::string err;
        if (install_service(err)) {
            printf("admuffs installed and started as a systemd service.\n"
                   "  status:  systemctl status admuffs\n"
                   "  logs:    journalctl -u admuffs -f\n"
                   "It now starts on boot, restarts on failure, and the web\n"
                   "remote's RESTART button performs a clean service restart.\n");
        } else { fprintf(stderr, "install failed: %s\n", err.c_str()); rc = 1; }
    } else if (command == "--uninstall-service") {
        std::string err;
        if (uninstall_service(err)) printf("admuffs service removed.\n");
        else { fprintf(stderr, "uninstall failed: %s\n", err.c_str()); rc = 1; }
    } else if (command == "--setup") {
        IrDatabase db; db.load(cfg.ir_db_dir);
        bool ok = run_setup_wizard(cfg, db, config_path);
        rc = ok ? 0 : 1;
    } else if (command == "--test-mute") {
        App app(cfg, config_path);
        app.init();
        rc = app.test_mute() ? 0 : 1;
    } else {  // --run
        if (!have_config)
            LOG_WARN("no config at %s; using defaults. Run --setup first.", config_path.c_str());
        App app(cfg, config_path);
        if (!app.init()) { rc = 1; }
        else rc = app.run(g_stop);
    }

    Http::global_cleanup();
    return rc;
}
