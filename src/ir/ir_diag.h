// SPDX-License-Identifier: MIT
// ir_diag.h - "is my IR hardware actually there?" diagnostic report.
#pragma once

namespace admuffs {
struct Config;

// Prints a human-readable IR hardware/software report to stdout and returns
// 0 when a transmit-capable device AND a working backend were found.
int run_ir_check(const Config& cfg);

}  // namespace admuffs
