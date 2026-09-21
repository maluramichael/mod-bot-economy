/*
 * mod-bot-economy - shared declarations.
 */

#ifndef MOD_BOT_ECONOMY_H
#define MOD_BOT_ECONOMY_H

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Weak, guarded telemetry hook. This module NEVER hard-links against the
// telemetry facility. When DAD_TELEMETRY_AVAILABLE is not defined at compile
// time the forward declaration below is absent and BotEconomy::Emit() compiles
// down to a no-op. This file MUST compile cleanly with the define ABSENT.
#if defined(DAD_TELEMETRY_AVAILABLE)
namespace DadTelemetry
{
    void Emit(std::string const&, std::vector<std::pair<std::string, std::string>> const&);
}
#endif

namespace BotEconomy
{
    // Cached config (populated in WorldScript::OnAfterConfigLoad).
    struct Config
    {
        bool     DadModeEnabled     = false;
        uint32   KillGoldBonusPct   = 0;
        uint32   TaxMinGoldCopper   = 5000;
        bool     SorterEnabled      = true;
        uint32   SorterIntervalSec  = 300;
        uint32   WarehouseLootThreshold = 0;   // 0 = record every loot; else min item Quality
        uint32   WarehouseSweepSec  = 600;      // periodic gold-surplus deposit sweep
    };

    Config& GetConfig();

    // Thin, guarded telemetry emitter (compiles to nothing when the telemetry
    // facility is not present at build time).
    inline void Emit(std::string const& event, std::vector<std::pair<std::string, std::string>> const& fields)
    {
#if defined(DAD_TELEMETRY_AVAILABLE)
        DadTelemetry::Emit(event, fields);
#else
        (void)event;
        (void)fields;
#endif
    }
}

#endif // MOD_BOT_ECONOMY_H
