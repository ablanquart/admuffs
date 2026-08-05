// SPDX-License-Identifier: MIT
#include "tv/ir_controller.h"
#include "common.h"

namespace admuffs {

const char* IrController::command_key(TvCommand cmd) {
    // Logical names follow the Linux rc-core keymap conventions, which is
    // also what irrecord suggests and what the JSON database uses.
    switch (cmd) {
        case TvCommand::Mute:
        case TvCommand::Unmute:
        case TvCommand::ToggleMute:  return "KEY_MUTE";   // IR mute is a toggle
        case TvCommand::VolumeUp:    return "KEY_VOLUMEUP";
        case TvCommand::VolumeDown:  return "KEY_VOLUMEDOWN";
        case TvCommand::PowerToggle: return "KEY_POWER";
        case TvCommand::Up:          return "KEY_UP";
        case TvCommand::Down:        return "KEY_DOWN";
        case TvCommand::Left:        return "KEY_LEFT";
        case TvCommand::Right:       return "KEY_RIGHT";
        case TvCommand::Ok:          return "KEY_OK";
        case TvCommand::Back:        return "KEY_BACK";
        case TvCommand::Settings:    return "KEY_MENU";
        case TvCommand::Home:        return "KEY_HOME";
        case TvCommand::ChannelUp:   return "KEY_CHANNELUP";
        case TvCommand::ChannelDown: return "KEY_CHANNELDOWN";
        case TvCommand::Input:       return "KEY_SOURCE";
        case TvCommand::Digit0:      return "KEY_0";
        case TvCommand::Digit1:      return "KEY_1";
        case TvCommand::Digit2:      return "KEY_2";
        case TvCommand::Digit3:      return "KEY_3";
        case TvCommand::Digit4:      return "KEY_4";
        case TvCommand::Digit5:      return "KEY_5";
        case TvCommand::Digit6:      return "KEY_6";
        case TvCommand::Digit7:      return "KEY_7";
        case TvCommand::Digit8:      return "KEY_8";
        case TvCommand::Digit9:      return "KEY_9";
    }
    return "KEY_MUTE";
}

bool IrController::send(TvCommand cmd) {
    const IrCommand* c = profile_.command(command_key(cmd));
    if (!c) {
        LOG_WARN("IR: no code '%s' for %s", command_key(cmd), profile_.display_name.c_str());
        return false;
    }
    return tx_.send(*c);
}

}  // namespace admuffs
