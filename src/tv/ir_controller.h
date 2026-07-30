// SPDX-License-Identifier: MIT
// ir_controller.h - TvController implemented over infrared (universal fallback).
#pragma once

#include "tv/tv_controller.h"
#include "ir/ir_transmitter.h"
#include "ir/ir_database.h"

namespace admuffs {

class IrController : public TvController {
public:
    IrController(IrTransmitter transmitter, IrTvProfile profile)
        : tx_(std::move(transmitter)), profile_(std::move(profile)) {}

    // IR is one-way; we cannot sense the TV, so it is always "available".
    bool available() override { return true; }
    bool connect() override { return true; }
    bool send(TvCommand cmd) override;
    std::string name() const override {
        return "IR (" + profile_.display_name + ", " + tx_.backend_name() + ")";
    }
    bool is_network() const override { return false; }

private:
    IrTransmitter tx_;
    IrTvProfile profile_;
    static const char* command_key(TvCommand cmd);
};

}  // namespace admuffs
