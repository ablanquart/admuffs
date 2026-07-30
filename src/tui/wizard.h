// SPDX-License-Identifier: MIT
// wizard.h - first-run ncurses setup wizard.
#pragma once

#include "config.h"
#include "ir/ir_database.h"

#include <string>

namespace admuffs {

// Runs the interactive setup, mutating cfg. On completion, writes cfg to
// config_path. Returns true if the user finished and the config was saved.
bool run_setup_wizard(Config& cfg, const IrDatabase& db, const std::string& config_path);

}  // namespace admuffs
