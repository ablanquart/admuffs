// SPDX-License-Identifier: MIT
#include "tui/wizard.h"
#include "common.h"
#include "net/discovery.h"
#include "net/http.h"
#include "tv/tv_controller.h"
#include "tv/api_controllers.h"

#include "json.hpp"

#include <ncurses.h>
#include <string>
#include <vector>

using json = nlohmann::json;

namespace admuffs {
namespace {

void draw_header(const char* subtitle) {
    erase();
    attron(A_BOLD);
    mvprintw(0, 2, "admuffs setup  -  auto-mute commercials");
    attroff(A_BOLD);
    mvhline(1, 0, ACS_HLINE, COLS);
    mvprintw(2, 2, "%s", subtitle);
}

void draw_footer(const char* hint) {
    mvhline(LINES - 2, 0, ACS_HLINE, COLS);
    mvprintw(LINES - 1, 2, "%s", hint);
    refresh();
}

// Vertical single-select menu. Returns index, or -1 if cancelled (q/ESC).
int menu(const char* title, const std::vector<std::string>& items, int initial = 0) {
    int sel = initial;
    int top = 4;
    for (;;) {
        draw_header(title);
        int max_rows = LINES - top - 3;
        int first = 0;
        if (sel >= max_rows) first = sel - max_rows + 1;
        for (int i = 0; i < (int)items.size() && i < max_rows; ++i) {
            int idx = first + i;
            if (idx >= (int)items.size()) break;
            if (idx == sel) attron(A_REVERSE);
            mvprintw(top + i, 4, "%-*s", COLS - 8, items[idx].c_str());
            if (idx == sel) attroff(A_REVERSE);
        }
        draw_footer("Up/Down move   Enter select   q back/cancel");

        int ch = getch();
        if (ch == KEY_UP || ch == 'k') { if (sel > 0) sel--; }
        else if (ch == KEY_DOWN || ch == 'j') { if (sel < (int)items.size() - 1) sel++; }
        else if (ch == '\n' || ch == KEY_ENTER) return sel;
        else if (ch == 'q' || ch == 27) return -1;
    }
}

std::string prompt_input(const char* title, const std::string& initial) {
    std::string buf = initial;
    for (;;) {
        draw_header(title);
        mvprintw(5, 4, "> %s", buf.c_str());
        draw_footer("Type value   Enter confirm   Esc cancel");
        int ch = getch();
        if (ch == '\n' || ch == KEY_ENTER) return buf;
        if (ch == 27) return initial;
        if (ch == KEY_BACKSPACE || ch == 127 || ch == 8) { if (!buf.empty()) buf.pop_back(); }
        else if (ch >= 32 && ch < 127) buf.push_back((char)ch);
    }
}

void message(const char* title, const std::string& body) {
    draw_header(title);
    int row = 5;
    for (auto& line : split(body, '\n')) mvprintw(row++, 4, "%s", line.c_str());
    draw_footer("Press any key to continue");
    getch();
}

// Suspend curses around noisy/network operations so logs don't corrupt the UI.
struct CursesPause {
    CursesPause() { def_prog_mode(); endwin(); }
    ~CursesPause() { reset_prog_mode(); refresh(); }
};

// --- Vizio PIN pairing (inline, needs the TV present) ---------------------
bool vizio_pair(const std::string& ip, std::string& token_out, std::string& err) {
    HttpRequest s; s.method = "PUT"; s.insecure = true;
    s.url = "https://" + ip + ":7345/pairing/start";
    s.headers = {"Content-Type: application/json"};
    s.body = R"({"DEVICE_ID":"admuffs","DEVICE_NAME":"admuffs"})";
    auto r1 = Http::request(s);
    if (!r1.ok || !r1.is2xx()) { err = "pairing/start failed: " + r1.error; return false; }

    long ptoken = 0;
    try { ptoken = json::parse(r1.body)["ITEM"]["PAIRING_REQ_TOKEN"].get<long>(); }
    catch (...) { err = "could not parse pairing token"; return false; }

    // The PIN is now shown on the TV; collect it (needs the UI back briefly).
    std::string pin;
    { reset_prog_mode(); pin = prompt_input("Enter the PIN shown on your Vizio TV", ""); def_prog_mode(); endwin(); }
    if (pin.empty()) { err = "no PIN entered"; return false; }

    HttpRequest p; p.method = "PUT"; p.insecure = true;
    p.url = "https://" + ip + ":7345/pairing/pair";
    p.headers = {"Content-Type: application/json"};
    json body = {{"DEVICE_ID", "admuffs"}, {"CHALLENGE_TYPE", 1},
                 {"RESPONSE_VALUE", pin}, {"PAIRING_REQ_TOKEN", ptoken}};
    p.body = body.dump();
    auto r2 = Http::request(p);
    if (!r2.ok || !r2.is2xx()) { err = "pairing/pair failed: " + r2.error; return false; }
    try { token_out = json::parse(r2.body)["ITEM"]["AUTH_TOKEN"].get<std::string>(); }
    catch (...) { err = "could not parse auth token"; return false; }
    return true;
}

bool brand_has_api(const std::string& brand) {
    std::string b = to_lower(brand);
    return b == "samsung" || b == "lg" || b == "roku" || b == "sony" || b == "vizio";
}

// Actively verify the network path: reach the TV and complete any session
// handshake. Mutates cfg when the TV hands out credentials (Samsung token,
// LG client-key). Caller must have curses paused (network + TV prompts).
bool verify_api_connection(Config& cfg, std::string& detail) {
    std::string b = to_lower(cfg.tv_brand);
    if (b == "roku") {
        RokuController c(cfg.tv_ip);
        if (c.available()) return true;
        detail = "no ECP response on :8060";
        return false;
    }
    if (b == "samsung") {
        SamsungController c(cfg.tv_ip, cfg.samsung_token,
                            [&cfg](const std::string& t) { cfg.samsung_token = t; });
        if (!c.available()) { detail = "no REST response on :8001"; return false; }
        if (c.connect()) return true;
        detail = "websocket remote refused (prompt declined or wss issue)";
        return false;
    }
    if (b == "lg") {
        LgController c(cfg.tv_ip, cfg.lg_client_key);
        if (!c.available()) { detail = "port 3000 unreachable"; return false; }
        if (c.connect()) { cfg.lg_client_key = c.client_key(); return true; }
        detail = "SSAP registration failed (pairing prompt not accepted?)";
        return false;
    }
    if (b == "sony") {
        SonyController c(cfg.tv_ip, cfg.sony_psk);
        if (c.available()) return true;
        detail = "REST refused (wrong PSK, or IP control disabled on the TV)";
        return false;
    }
    if (b == "vizio") {
        VizioController c(cfg.tv_ip, cfg.vizio_auth_token);
        if (!c.available()) { detail = "port 7345 unreachable"; return false; }
        if (cfg.vizio_auth_token.empty()) { detail = "reachable but not paired"; return false; }
        return true;
    }
    detail = "no network API implemented for brand '" + cfg.tv_brand + "'";
    return false;
}

std::string discover_ip(const std::string& brand) {
    std::vector<DiscoveredDevice> devs;
    {
        CursesPause pause;
        std::string st = (to_lower(brand) == "roku")
            ? "roku:ecp" : "urn:dial-multiscreen-org:service:dial:1";
        devs = ssdp_search(st, 3000);
        if (devs.empty()) devs = ssdp_search("ssdp:all", 3000);
    }
    if (devs.empty()) {
        message("Discovery", "No devices found via SSDP.\nYou can enter the IP manually.");
        return "";
    }
    std::vector<std::string> items;
    for (auto& d : devs) {
        std::string label = d.ip;
        if (!d.server.empty()) label += "   " + d.server;
        items.push_back(label);
    }
    items.push_back("<enter IP manually>");
    int sel = menu("Select your TV", items, 0);
    if (sel < 0 || sel >= (int)devs.size()) return "";
    return devs[sel].ip;
}

}  // namespace

bool run_setup_wizard(Config& cfg, const IrDatabase& db, const std::string& config_path) {
    // Keep logs off the terminal while the TUI owns the screen.
    log_init(LogLevel::Error, "/tmp/admuffs-setup.log");

    initscr();
    cbreak();
    noecho();
    keypad(stdscr, TRUE);
    curs_set(0);

    bool saved = false;
    do {
        // 1) Brand. Common brands up front; everything else (older and
        // budget sets, mostly IR-only) lives under "Other...".
        auto brands = db.brands();
        if (brands.empty()) { message("Error", "IR database is empty. Check ir_db_dir."); break; }
        auto is_primary = [](const std::string& b) {
            std::string l = to_lower(b);
            return l == "samsung" || l == "lg" || l == "sony" || l == "vizio" ||
                   l == "roku" || l == "tcl" || l == "generic";
        };
        std::vector<std::string> primary, other;
        for (const auto& b : brands) (is_primary(b) ? primary : other).push_back(b);

        cfg.tv_brand.clear();
        while (cfg.tv_brand.empty()) {
            std::vector<std::string> items = primary;
            if (!other.empty()) items.push_back("Other  (Panasonic, Toshiba, Magnavox, JVC, ...)");
            int bi = menu("Step 1/7  -  Select your TV brand", items, 0);
            if (bi < 0) break;                       // cancel wizard
            if (!other.empty() && bi == (int)primary.size()) {
                int oi = menu("Step 1/7  -  Other brands (q = back)", other, 0);
                if (oi < 0) continue;                // back to the main list
                cfg.tv_brand = other[oi];
            } else {
                cfg.tv_brand = items[bi];
            }
        }
        if (cfg.tv_brand.empty()) break;

        // 2) Model
        auto models = db.models_for(cfg.tv_brand);
        std::vector<std::string> mnames;
        for (auto* m : models) mnames.push_back(m->display_name + "  [" + m->model + "]");
        int mi = menu("Step 2/7  -  Select your model (pick 'generic' if unsure)", mnames, 0);
        if (mi < 0) break;
        cfg.tv_model = models[mi]->model;

        // 3) Control method
        int meth = menu("Step 3/7  -  Control method",
                        {"Auto  (network API if reachable, else IR)",
                         "Network API only",
                         "Infrared only"}, 0);
        if (meth < 0) break;
        cfg.method = (meth == 0) ? ControlMethod::Auto
                   : (meth == 1) ? ControlMethod::Api : ControlMethod::Ir;

        // 4) Network API setup (IP + pairing + verified connection).
        // Page A locates the TV; page B pairs and verifies, with a
        // retry / go-back / continue loop mirroring the IR mute test.
        if (cfg.method != ControlMethod::Ir) {
            if (!brand_has_api(cfg.tv_brand)) {
                if (cfg.method == ControlMethod::Api)
                    message("No network API",
                            "No network API is implemented for '" + cfg.tv_brand + "'.\n"
                            "Control will fall back to infrared.");
            } else {
                bool api_done = false;
                while (!api_done) {
                    // ---- Page A: locate the TV ----
                    int how = menu("Step 4/7  -  Find your TV on the network",
                                   {"Scan the network (SSDP)",
                                    "Enter IP address manually",
                                    "Skip network setup (IR only)"}, 0);
                    if (how < 0 || how == 2) break;
                    if (how == 0) cfg.tv_ip = discover_ip(cfg.tv_brand);
                    if (how == 1 || cfg.tv_ip.empty())
                        cfg.tv_ip = prompt_input("Enter your TV's IP address", cfg.tv_ip);
                    if (cfg.tv_ip.empty()) {
                        if (menu("No IP address set.",
                                 {"Go back and try again", "Skip network setup"}, 0) != 0)
                            break;
                        continue;
                    }

                    // ---- Page B: pair + verify, retry loop ----
                    std::string b = to_lower(cfg.tv_brand);
                    for (;;) {
                        // Brand-specific pairing inputs before the check.
                        if (b == "sony")
                            cfg.sony_psk = prompt_input(
                                "Sony pre-shared key (Settings > Network > IP control)",
                                cfg.sony_psk);
                        if (b == "vizio" && cfg.vizio_auth_token.empty()) {
                            std::string tok, err;
                            bool paired;
                            { CursesPause pause; paired = vizio_pair(cfg.tv_ip, tok, err); }
                            if (paired) cfg.vizio_auth_token = tok;
                            else message("Vizio pairing failed", err);
                        }
                        if (b == "lg" && cfg.lg_client_key.empty())
                            message("LG pairing",
                                    "Watch your TV: a pairing prompt will appear.\n"
                                    "Accept it on the TV when it does.");
                        if (b == "samsung" && cfg.samsung_token.empty())
                            message("Samsung",
                                    "The TV may show an 'allow this remote' prompt.\n"
                                    "Accept it on the TV when it appears.");

                        bool ok;
                        std::string detail;
                        { CursesPause pause; ok = verify_api_connection(cfg, detail); }

                        if (ok) {
                            message("Connected",
                                    "Network connection to " + cfg.tv_brand + " at " +
                                    cfg.tv_ip + " verified successfully.");
                            api_done = true;
                            break;
                        }

                        int r = menu(("Connection/pairing FAILED: " + detail).c_str(),
                                     {"Try again",
                                      "Go back (re-enter IP / rescan)",
                                      "Continue anyway (IR fallback will be used)",
                                      "Hint: what usually causes this?"}, 0);
                        if (r == 0) continue;              // retry page B
                        if (r == 1) break;                 // back to page A
                        if (r == 3) {
                            message("Common causes",
                                    "- Wrong IP (TV got a new DHCP lease; rescan)\n"
                                    "- TV asleep or in deep standby (wake it first)\n"
                                    "- Pairing prompt on the TV declined or timed out\n"
                                    "- Sony: PSK mismatch, or IP control disabled\n"
                                    "- Vizio: PIN mistyped -- redo pairing\n"
                                    "- TV and Pi on different networks/VLANs\n"
                                    "- 'Mobile TV On' / network-standby off (Samsung/LG)");
                            continue;
                        }
                        api_done = true;                   // continue anyway
                        break;
                    }
                }
            }
        }

        // 5) IR setup when applicable
        if (cfg.method != ControlMethod::Api) {
            int be_initial = (cfg.ir_backend == "ir-ctl") ? 1
                           : (cfg.ir_backend == "dryrun") ? 2 : 0;
            int be = menu("Step 5/7  -  IR backend",
                          {"LIRC (irsend)", "kernel rc-core (ir-ctl)", "Dry run (log only)"},
                          be_initial);
            if (be == 0) cfg.ir_backend = "irsend";
            else if (be == 1) cfg.ir_backend = "ir-ctl";
            else cfg.ir_backend = "dryrun";
            if (cfg.ir_backend == "ir-ctl")
                cfg.ir_device = prompt_input("IR device (ir-ctl)", cfg.ir_device);
            cfg.ir_remote = prompt_input(
                "LIRC remote name (blank = use database default)", cfg.ir_remote);
        }

        // 6) Audio input for loudness + ACR detection
        cfg.audio_device = prompt_input(
            "Step 6/7  -  ALSA capture device ('auto' = find USB mic, plughw:N,0 = specific, blank = disable)",
            cfg.audio_device.empty() ? "auto" : cfg.audio_device);

        if (!cfg.audio_device.empty()) {
            // Where does that input listen? This decides how unmuting works
            // (see the mute paradox in README/HARDWARE.md).
            int tap = menu("Where does the audio input listen?",
                           {"Room microphone (hears the TV's speakers)",
                            "Upstream tap (source line-out / HDMI extractor / TV optical-out) - recommended"},
                           cfg.audio_tap == "upstream" ? 1 : 0);
            if (tap >= 0) cfg.audio_tap = (tap == 1) ? "upstream" : "room";

            // Offer only the modes that actually work with the chosen tap.
            // A room mic goes deaf when the TV is muted, so "Mute" is not
            // offered there -- it could only unmute on the failsafe timer.
            // Duck and Normalize keep hearing (quieter) audio, so a mic can
            // time them correctly.
            bool room = (cfg.audio_tap != "upstream");
            std::vector<std::string> modes;
            std::vector<std::string> ids;
            if (!room) {
                modes.push_back("Mute the TV (silent ads)   (recommended)");
                ids.push_back("mute");
            }
            modes.push_back(std::string("Duck: lower volume during ads; "
                                        "detection keeps hearing") +
                            (room ? "   (recommended)" : ""));
            ids.push_back("duck");
            modes.push_back("Normalize: continuously level volume to a target "
                            "you set (web remote: SET VOLUME TARGET)");
            ids.push_back("normalize");

            std::string cur = to_lower(cfg.mute_mode);
            if (room && cur == "mute") cur = "duck";  // not offered for room mic
            int init = 0;
            for (size_t i = 0; i < ids.size(); ++i)
                if (ids[i] == cur) init = (int)i;

            const char* title = room
                ? "What should Admuffs do about loud commercials? "
                  "(Mute isn't offered: a room mic can't hear when to unmute)"
                : "What should Admuffs do about loud commercials?";
            int mm = menu(title, modes, init);
            if (mm >= 0) cfg.mute_mode = ids[mm];
            else if (room && to_lower(cfg.mute_mode) == "mute")
                cfg.mute_mode = "duck";  // cancelled, but never save room+mute
        }

        // 7) Optional: ACRCloud ad recognition
        if (!cfg.audio_device.empty() &&
            menu("Step 7/7  -  Configure ACRCloud ad recognition? (optional subscription)",
                 {"Skip for now (loudness-only detection)",
                  "Yes, enter my ACRCloud project credentials"}, 0) == 1) {
            cfg.acr_provider = "acrcloud";
            cfg.acr_host = prompt_input(
                "ACRCloud host (e.g. identify-us-west-2.acrcloud.com)", cfg.acr_host);
            cfg.acr_key = prompt_input("ACRCloud access key", cfg.acr_key);
            cfg.acr_secret = prompt_input("ACRCloud access secret", cfg.acr_secret);
        }

        // Optional live test, with confirm + resend loop.
        if (menu("Test TV control now?", {"Yes, send a mute/unmute", "No, finish"}, 0) == 0) {
            IrDatabase local; local.load(cfg.ir_db_dir);
            auto tv = make_controller(cfg, local);
            bool connected = false;
            for (;;) {
                bool sent = false;
                {
                    CursesPause pause;
                    if (tv) {
                        if (!connected) connected = tv->connect();
                        sent = tv->send(TvCommand::Mute);
                        sleep_ms(1200);
                        tv->send(TvCommand::Unmute);
                    }
                }
                if (!sent) {
                    int r = menu("Test FAILED to send (see any errors above).",
                                 {"Resend the test",
                                  "Continue and save anyway",
                                  "Hint: what usually causes this?"}, 0);
                    if (r == 0) continue;
                    if (r == 2) {
                        message("Common causes",
                                "- ir-ctl backend but the TV profile has no raw pulses\n"
                                "  (run 'admuffs --ir-check', see section [4])\n"
                                "- irsend backend but lircd uses the receive-only devinput\n"
                                "  driver: set 'driver = default' and 'device = /dev/lircX'\n"
                                "  (the TRANSMIT one) in /etc/lirc/lirc_options.conf,\n"
                                "  then: sudo systemctl restart lircd\n"
                                "- wrong ir_device, or no line of sight to the TV\n"
                                "- network method: wrong IP or pairing not accepted");
                        continue;
                    }
                    break;
                }
                int r = menu("Command sent. Did the TV actually mute (and unmute)?",
                             {"Yes, it worked", "No -- resend the test",
                              "No -- continue and save anyway"}, 0);
                if (r == 1) continue;
                break;
            }
        }

        saved = cfg.save(config_path);
        message(saved ? "Saved" : "Save failed",
                saved ? ("Configuration written to:\n" + config_path +
                         "\n\nRun 'admuffs --run' to start auto-muting.")
                      : ("Could not write:\n" + config_path));
    } while (false);

    endwin();
    return saved;
}

}  // namespace admuffs
