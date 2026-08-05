// SPDX-License-Identifier: MIT
#include "tv/tv_controller.h"
#include "tv/api_controllers.h"
#include "tv/ir_controller.h"
#include "ir/ir_database.h"
#include "ir/ir_record.h"
#include "config.h"
#include "common.h"

#include "json.hpp"

#include <fstream>
#include <sstream>

namespace admuffs {

namespace {

std::unique_ptr<TvController> make_api_controller(const Config& cfg,
                                                  const CredentialSaveFn& on_credential) {
    if (cfg.tv_ip.empty()) {
        LOG_WARN("API control requested but no tv_ip configured");
        return nullptr;
    }
    std::string b = to_lower(cfg.tv_brand);
    if (b == "samsung") {
        SamsungController::TokenCallback cb;
        if (on_credential)
            cb = [on_credential](const std::string& tok) { on_credential("samsung_token", tok); };
        return std::make_unique<SamsungController>(cfg.tv_ip, cfg.samsung_token, cb);
    }
    if (b == "lg")      return std::make_unique<LgController>(cfg.tv_ip, cfg.lg_client_key);
    if (b == "roku")    return std::make_unique<RokuController>(cfg.tv_ip);
    if (b == "sony")    return std::make_unique<SonyController>(cfg.tv_ip, cfg.sony_psk);
    if (b == "vizio")   return std::make_unique<VizioController>(cfg.tv_ip, cfg.vizio_auth_token);
    LOG_WARN("no network API implemented for brand '%s'", cfg.tv_brand.c_str());
    return nullptr;
}

std::unique_ptr<TvController> make_ir_controller(const Config& cfg, const IrDatabase& db) {
    const IrTvProfile* base = db.find(cfg.tv_brand, cfg.tv_model);
    if (!base) {
        LOG_WARN("no IR profile for %s/%s", cfg.tv_brand.c_str(), cfg.tv_model.c_str());
        return nullptr;
    }
    IrTvProfile profile = *base;  // copy so we can apply overrides
    if (!cfg.ir_remote.empty()) {
        profile.default_lirc_remote = cfg.ir_remote;
        for (auto& kv : profile.commands)
            if (kv.second.lirc_remote.empty() || kv.second.lirc_remote == base->default_lirc_remote)
                kv.second.lirc_remote = cfg.ir_remote;
    }
    // User-recorded codes (web remote "Record Remote Codes" / the record
    // script) override the bundled database for whatever TV is configured:
    // codes captured from the actual remote in the room are ground truth.
    {
        std::ifstream in(recorded_keys_path());
        if (in) {
            try {
                std::stringstream ss; ss << in.rdbuf();
                nlohmann::json j = nlohmann::json::parse(ss.str());
                int applied = 0;
                for (auto it = j.begin(); it != j.end(); ++it) {
                    IrCommand& c = profile.commands[it.key()];
                    c.name = it.key();
                    c.carrier_hz = it.value().value("carrier_hz", 38000);
                    c.pulses = it.value()["pulses"].get<std::vector<int>>();
                    applied++;
                }
                if (applied)
                    LOG_INFO("IR: %d user-recorded key(s) override the database "
                             "(%s)", applied, recorded_keys_path().c_str());
            } catch (const std::exception& e) {
                LOG_WARN("IR: bad recorded_keys.json ignored: %s", e.what());
            }
        }
    }

    IrTransmitter tx;
    tx.init(cfg.ir_backend, cfg.ir_device);
    return std::make_unique<IrController>(std::move(tx), std::move(profile));
}

// Prefers the network API, falls back to IR transparently.
class CompositeController : public TvController {
public:
    CompositeController(std::unique_ptr<TvController> primary,
                        std::unique_ptr<TvController> fallback)
        : primary_(std::move(primary)), fallback_(std::move(fallback)) {}

    bool available() override {
        return (primary_ && primary_->available()) || (fallback_ && fallback_->available());
    }

    bool connect() override {
        if (primary_ && primary_->available() && primary_->connect()) {
            use_primary_ = true;
            LOG_INFO("TV control: using %s", primary_->name().c_str());
            return true;
        }
        use_primary_ = false;
        if (fallback_) {
            LOG_INFO("TV control: using fallback %s", fallback_->name().c_str());
            return fallback_->connect();
        }
        return false;
    }

    bool send(TvCommand cmd) override {
        if (use_primary_ && primary_ && primary_->send(cmd)) return true;
        if (use_primary_ && primary_)
            LOG_WARN("primary controller failed for %s; trying IR", tv_command_name(cmd));
        return fallback_ ? fallback_->send(cmd) : false;
    }

    std::string name() const override {
        std::string n = primary_ ? primary_->name() : "none";
        if (fallback_) n += " -> " + fallback_->name();
        return n;
    }
    bool is_network() const override { return use_primary_; }

private:
    std::unique_ptr<TvController> primary_, fallback_;
    bool use_primary_ = false;
};

}  // namespace

std::unique_ptr<TvController> make_controller(const Config& cfg, const IrDatabase& db,
                                              CredentialSaveFn on_credential) {
    switch (cfg.method) {
        case ControlMethod::Ir:
            return make_ir_controller(cfg, db);
        case ControlMethod::Api: {
            auto api = make_api_controller(cfg, on_credential);
            if (api) return api;
            LOG_WARN("falling back to IR (no usable API)");
            return make_ir_controller(cfg, db);
        }
        case ControlMethod::Auto:
        default: {
            auto api = make_api_controller(cfg, on_credential);
            auto ir = make_ir_controller(cfg, db);
            if (api && ir) return std::make_unique<CompositeController>(std::move(api), std::move(ir));
            if (api) return api;
            return ir;
        }
    }
}

}  // namespace admuffs
