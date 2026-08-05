// SPDX-License-Identifier: MIT
// tv_controller.h - abstract TV control + factory.
#pragma once

#include <functional>
#include <memory>
#include <string>

namespace admuffs {

struct Config;
class IrDatabase;

// The full command vocabulary. Digit0..Digit9 are contiguous so digit N can
// be formed as Digit0 + N. Not every backend supports every command; a
// controller returns false for commands it cannot express, which lets the
// composite controller fall back to IR for just that keypress.
enum class TvCommand {
    Mute, Unmute, ToggleMute, VolumeUp, VolumeDown, PowerToggle,
    Up, Down, Left, Right, Ok,
    Back, Settings, Home,
    ChannelUp, ChannelDown, Input,
    Digit0, Digit1, Digit2, Digit3, Digit4,
    Digit5, Digit6, Digit7, Digit8, Digit9,
};

const char* tv_command_name(TvCommand c);

// Digit helpers.
inline bool tv_command_is_digit(TvCommand c) {
    return c >= TvCommand::Digit0 && c <= TvCommand::Digit9;
}
inline int tv_command_digit(TvCommand c) {
    return static_cast<int>(c) - static_cast<int>(TvCommand::Digit0);
}

class TvController {
public:
    virtual ~TvController() = default;

    // Cheap reachability probe (no pairing side effects where possible).
    virtual bool available() = 0;

    // Establish a session / complete pairing handshake. Safe to call again.
    virtual bool connect() = 0;

    // Execute a command. Returns false on failure so a fallback can be tried.
    virtual bool send(TvCommand cmd) = 0;

    virtual std::string name() const = 0;
    virtual bool is_network() const = 0;
};

// Called when a controller obtains a new credential worth persisting
// (key is a Config field name, e.g. "samsung_token").
using CredentialSaveFn =
    std::function<void(const std::string& key, const std::string& value)>;

// Build the controller described by cfg. When method is Auto, returns a
// composite that prefers the reachable network API and falls back to IR.
std::unique_ptr<TvController> make_controller(const Config& cfg, const IrDatabase& db,
                                              CredentialSaveFn on_credential = nullptr);

}  // namespace admuffs
