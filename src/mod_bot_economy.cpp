/*
 * mod-bot-economy
 *
 * A self-contained "Dad Mode" economy layer for random playerbots on the
 * mod-playerbots AzerothCore fork (WotLK 3.3.5a).
 *
 * Behaviors (ALL gated behind DadMode.Enabled):
 *   1. Kill-gold bonus  - random bots get a small gold bonus per creature kill.
 *   2. Guild tax        - a slice of a bot's on-hand gold trickles into its
 *                         guild bank (via the real Guild deposit path).
 *   3. Direct deposit   - ledger rows are written to bot_warehouse: looted
 *                         ITEMS as they drop (on the loot hook), plus a bot's
 *                         surplus GOLD at login AND on a periodic sweep of
 *                         already-online bots. Never moves real inventory/gold.
 *   4. Bank sorter      - a periodic, PLAYER-SAFE pass that consolidates
 *                         duplicate warehouse stacks, skipping any guild whose
 *                         real (non-bot) members are currently online.
 *
 * See conf/mod_bot_economy.conf.dist for keys and mod-bot-economy's HANDOFF
 * doc for the full rationale, the exact core/Guild/Player APIs used, and the
 * explicit list of what is and is NOT actually done here.
 */

#include "Config.h"
#include "Creature.h"
#include "DatabaseEnv.h"
#include "Guild.h"
#include "GuildMgr.h"
#include "Item.h"
#include "ItemTemplate.h"
#include "ObjectAccessor.h"
#include "ObjectMgr.h"
#include "Player.h"
#include "ScriptMgr.h"
#include "World.h"

// Playerbots fork headers. "Playerbots.h" pulls in PlayerbotAI.h and
// RandomPlayerbotMgr.h (which define GET_PLAYERBOT_AI and sRandomPlayerbotMgr).
#include "Playerbots.h"
#include "PlayerbotAI.h"
#include "RandomPlayerbotMgr.h"

#include "mod_bot_economy.h"

#include <ctime>

namespace BotEconomy
{
    Config& GetConfig()
    {
        static Config cfg;
        return cfg;
    }
}

using BotEconomy::GetConfig;

namespace
{
    // ---- bot detection --------------------------------------------------
    //
    // ASSUMPTION (documented in HANDOFF): "random bot" == the player has a
    // playerbot AI AND the playerbots RandomPlayerbotMgr recognizes it as a
    // random bot. GET_PLAYERBOT_AI() is the reliably-linkable "has bot AI"
    // test; sRandomPlayerbotMgr.IsRandomBot() narrows it to the random-bot
    // population (RNDBOT* accounts) so we never touch a real player's gold.
    bool IsRandomBotPlayer(Player* player)
    {
        if (!player)
            return false;

        if (GET_PLAYERBOT_AI(player) == nullptr)
            return false;

        return sRandomPlayerbotMgr.IsRandomBot(player);
    }

    uint32 NowUnix()
    {
        return static_cast<uint32>(::time(nullptr));
    }

    // Create our two tables. Runs at OnStartup via CharacterDatabase.
    // Deliberately NOT an SQL update file: on this fork a failing module SQL
    // aborts the whole worldserver boot, so we create tables programmatically
    // and tolerate failure at runtime instead.
    void EnsureSchema()
    {
        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `bot_tax_log` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
            "`bot_guid` INT UNSIGNED NOT NULL, "
            "`guild_id` INT UNSIGNED NOT NULL, "
            "`gold_copper` INT UNSIGNED NOT NULL, "
            "`ts` INT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`id`), "
            "KEY `idx_bot` (`bot_guid`), "
            "KEY `idx_guild` (`guild_id`), "
            "KEY `idx_ts` (`ts`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");

        CharacterDatabase.Execute(
            "CREATE TABLE IF NOT EXISTS `bot_warehouse` ("
            "`id` BIGINT UNSIGNED NOT NULL AUTO_INCREMENT, "
            "`owner_guid` INT UNSIGNED NOT NULL, "
            "`item_entry` INT UNSIGNED NOT NULL, "
            "`item_count` INT UNSIGNED NOT NULL DEFAULT 1, "
            "`ts` INT UNSIGNED NOT NULL, "
            "PRIMARY KEY (`id`), "
            "KEY `idx_owner` (`owner_guid`), "
            "KEY `idx_item` (`item_entry`)"
            ") ENGINE=InnoDB DEFAULT CHARSET=utf8mb4;");
    }

    // ---- guild tax ------------------------------------------------------
    //
    // Tax GuildTaxPct% of the gold the bot JUST EARNED this event (the taxable
    // increment - e.g. a kill's gold value - NOT the bot's whole wallet), with a
    // floor of 1 copper so tiny incomes still contribute. The tax is moved into
    // the guild bank via the REAL deposit path,
    // Guild::HandleMemberDepositMoney(WorldSession*, uint32) (Guild.h:725 /
    // Guild.cpp:1703): that function atomically debits the depositing player
    // (ModifyMoney(-amount) + SaveGoldToDB) and credits the bank
    // (_ModifyBankMoney) inside one DB transaction, so we never touch bank money
    // directly and the two sides can't desync. Then log + emit telemetry.
    //
    // taxableCopper is the amount the bot gained this event; we never tax more
    // than that, and never more than the bot actually holds (no underflow).
    void MaybePayGuildTax(Player* bot, uint32 taxableCopper)
    {
        BotEconomy::Config const& cfg = GetConfig();

        // Nothing was earned this event -> nothing to tax.
        if (taxableCopper == 0)
            return;

        uint32 onHand = bot->GetMoney();
        if (onHand == 0)
            return;

        uint32 guildId = bot->GetGuildId();
        if (guildId == 0)
            return;

        Guild* guild = bot->GetGuild();
        if (!guild)
            return;

        WorldSession* session = bot->GetSession();
        if (!session)
            return;

        // 1% (config) of the taxable increment, with a 1-copper minimum floor.
        uint32 tax = static_cast<uint32>((static_cast<uint64>(taxableCopper) * cfg.GuildTaxPct) / 100);
        if (tax == 0)
            tax = 1; // minimum 1 copper per taxed event

        // Never tax more than the bot earned this event...
        if (tax > taxableCopper)
            tax = taxableCopper;

        // ...and never more than the bot actually holds (no underflow).
        if (tax > onHand)
            tax = onHand;

        // Real Guild deposit path: debits the bot, credits the guild bank.
        guild->HandleMemberDepositMoney(session, tax);

        uint32 botGuidLow = bot->GetGUID().GetCounter();
        CharacterDatabase.Execute(
            "INSERT INTO `bot_tax_log` (`bot_guid`, `guild_id`, `gold_copper`, `ts`) VALUES ({}, {}, {}, {})",
            botGuidLow, guildId, tax, NowUnix());

        BotEconomy::Emit("tax_paid", {
            {"bot",   std::to_string(botGuidLow)},
            {"name",  bot->GetName()},
            {"guild", std::to_string(guildId)},
            {"gold",  std::to_string(tax)},
        });
    }

    // ---- gold-surplus warehouse deposit --------------------------------
    //
    // Write a ledger row to bot_warehouse recording the bot's surplus gold
    // (on-hand above the tax floor). Kept intentionally minimal and SAFE - we
    // do NOT move real gold/items around; this is a record table. Shared by the
    // login hook (Task 3) and the periodic sweep (so already-online bots get
    // recorded too). item_entry = 0 is our sentinel for a gold deposit;
    // item_count carries the copper amount.
    void MaybeDepositGoldSurplus(Player* bot)
    {
        BotEconomy::Config const& cfg = GetConfig();

        uint32 onHand = bot->GetMoney();
        // Only record a "surplus" once the bot is clearly above the tax floor.
        if (onHand <= cfg.TaxMinGoldCopper)
            return;

        uint32 surplus = onHand - cfg.TaxMinGoldCopper;
        uint32 ownerGuidLow = bot->GetGUID().GetCounter();

        CharacterDatabase.Execute(
            "INSERT INTO `bot_warehouse` (`owner_guid`, `item_entry`, `item_count`, `ts`) VALUES ({}, 0, {}, {})",
            ownerGuidLow, surplus, NowUnix());

        BotEconomy::Emit("deposit", {
            {"bot",  std::to_string(ownerGuidLow)},
            {"name", bot->GetName()},
            {"dest", "warehouse"},
            {"item", "0"},           // 0 == gold sentinel
            {"qty",  "0"},
            {"gold", std::to_string(surplus)},
        });
    }
}

// =====================================================================
//  PlayerScript: kill-gold bonus, guild tax, direct deposit at login.
// =====================================================================
class BotEconPlayerScript : public PlayerScript
{
public:
    BotEconPlayerScript() : PlayerScript("BotEcon_PlayerScript") { }

    // Task 1 (kill-gold bonus) + Task 2 (guild tax), both on the kill hook.
    void OnPlayerCreatureKill(Player* killer, Creature* killed) override
    {
        BotEconomy::Config const& cfg = GetConfig();
        if (!cfg.DadModeEnabled)
            return;

        if (!killed)
            return;

        if (!IsRandomBotPlayer(killer))
            return;

        // Base kill value derived from the victim's level (in copper).
        uint32 level = killed->GetLevel();
        uint32 baseValue = level * 100u; // 1 silver per level as a gentle base

        uint32 bonus = 0;
        if (cfg.KillGoldBonusPct > 0)
        {
            if (baseValue == 0)
                bonus = 50; // flat small fallback when base works out to 0
            else
                bonus = static_cast<uint32>((static_cast<uint64>(baseValue) * cfg.KillGoldBonusPct) / 100);
        }

        if (bonus > 0)
        {
            killer->ModifyMoney(static_cast<int32>(bonus));

            BotEconomy::Emit("bot_kill", {
                {"bot",          std::to_string(killer->GetGUID().GetCounter())},
                {"name",         killer->GetName()},
                {"victim_entry", std::to_string(killed->GetEntry())},
                {"gold",         std::to_string(bonus)},
                {"map",          std::to_string(killer->GetMapId())},
                {"zone",         std::to_string(killer->GetZoneId())},
            });
        }

        // Task 2: tax a slice of the gold EARNED from this kill into the guild
        // bank. The taxable base is the kill's gold value (a proxy for the loot
        // gold the bot gains from the kill) plus any bonus this module granted -
        // NOT the bot's whole wallet. So a level-1 bot yields ~1 copper, not a
        // cut of its entire (manager-seeded) balance.
        MaybePayGuildTax(killer, baseValue + bonus);
    }

    // Task 3: direct deposit. Kept intentionally minimal and SAFE - we write a
    // ledger row to bot_warehouse recording the bot's surplus gold; we do NOT
    // move real inventory items around (that path is risky and out of scope).
    // See HANDOFF for the exact limits.
    void OnPlayerLogin(Player* player) override
    {
        BotEconomy::Config const& cfg = GetConfig();
        if (!cfg.DadModeEnabled)
            return;

        if (!IsRandomBotPlayer(player))
            return;

        // No real gold/items are removed - this is a ledger.
        MaybeDepositGoldSurplus(player);
    }

    // Task 1 (items): record looted items into the warehouse ledger. Fires on
    // every successful loot pickup, so it is kept deliberately cheap. We record
    // the REAL item_entry + count; no inventory is moved (ledger only). Gated by
    // WarehouseLootThreshold: 0 records every loot, else only items whose
    // template Quality >= the threshold.
    void OnPlayerLootItem(Player* player, Item* item, uint32 count, ObjectGuid /*lootguid*/) override
    {
        BotEconomy::Config const& cfg = GetConfig();
        if (!cfg.DadModeEnabled)
            return;

        if (!item)
            return;

        if (!IsRandomBotPlayer(player))
            return;

        uint32 entry = item->GetEntry();

        if (cfg.WarehouseLootThreshold > 0)
        {
            ItemTemplate const* proto = sObjectMgr->GetItemTemplate(entry);
            if (!proto || proto->Quality < cfg.WarehouseLootThreshold)
                return;
        }

        uint32 ownerGuidLow = player->GetGUID().GetCounter();

        CharacterDatabase.Execute(
            "INSERT INTO `bot_warehouse` (`owner_guid`, `item_entry`, `item_count`, `ts`) VALUES ({}, {}, {}, {})",
            ownerGuidLow, entry, count, NowUnix());

        BotEconomy::Emit("deposit", {
            {"bot",  std::to_string(ownerGuidLow)},
            {"name", player->GetName()},
            {"dest", "warehouse"},
            {"item", std::to_string(entry)},
            {"qty",  std::to_string(count)},
            {"gold", "0"},
        });
    }
};

// =====================================================================
//  WorldScript: config load, schema, periodic warehouse sorter.
// =====================================================================
class BotEconWorldScript : public WorldScript
{
public:
    BotEconWorldScript() : WorldScript("BotEcon_WorldScript"), _sorterTimerMs(0), _sweepTimerMs(0) { }

    void OnAfterConfigLoad(bool /*reload*/) override
    {
        BotEconomy::Config& cfg = GetConfig();
        cfg.DadModeEnabled    = sConfigMgr->GetOption<bool>("DadMode.Enabled", false);
        cfg.KillGoldBonusPct  = sConfigMgr->GetOption<uint32>("DadMode.Economy.KillGoldBonusPct", 0);
        cfg.GuildTaxPct       = sConfigMgr->GetOption<uint32>("DadMode.Economy.GuildTaxPct", 1);
        cfg.TaxMinGoldCopper  = sConfigMgr->GetOption<uint32>("DadMode.Economy.TaxMinGoldCopper", 5000);
        cfg.SorterEnabled     = sConfigMgr->GetOption<bool>("DadMode.Economy.SorterEnabled", true);
        cfg.SorterIntervalSec = sConfigMgr->GetOption<uint32>("DadMode.Economy.SorterIntervalSec", 300);
        cfg.WarehouseLootThreshold = sConfigMgr->GetOption<uint32>("DadMode.Economy.WarehouseLootThreshold", 0);
        cfg.WarehouseSweepSec = sConfigMgr->GetOption<uint32>("DadMode.Economy.WarehouseSweepSec", 600);

        // Clamp GuildTaxPct to a sane range so a fat-fingered config can't
        // try to deposit >100% of a bot's gold.
        if (cfg.GuildTaxPct > 100)
            cfg.GuildTaxPct = 100;
    }

    void OnStartup() override
    {
        // Always create the tables so the rest of the module can rely on them,
        // even if DadMode is currently off (harmless empty tables).
        EnsureSchema();
    }

    // Task 4: player-safe warehouse sorter + periodic gold-surplus sweep.
    void OnUpdate(uint32 diff) override
    {
        BotEconomy::Config const& cfg = GetConfig();
        if (!cfg.DadModeEnabled)
            return;

        // Periodic gold-surplus sweep: record already-online random bots that
        // are above the tax floor, so bots that were logged in before DadMode
        // turned on (or that earned gold since login) still get a warehouse row.
        {
            _sweepTimerMs += diff;
            uint32 sweepMs = cfg.WarehouseSweepSec * 1000u;
            if (sweepMs == 0)
                sweepMs = 600000u;

            if (_sweepTimerMs >= sweepMs)
            {
                _sweepTimerMs = 0;
                RunGoldSurplusSweep();
            }
        }

        if (!cfg.SorterEnabled)
            return;

        _sorterTimerMs += diff;
        uint32 intervalMs = cfg.SorterIntervalSec * 1000u;
        if (intervalMs == 0)
            intervalMs = 300000u;

        if (_sorterTimerMs < intervalMs)
            return;

        _sorterTimerMs = 0;
        RunWarehouseSorter();
    }

private:
    uint32 _sorterTimerMs;
    uint32 _sweepTimerMs;

    // Periodic gold-surplus deposit: iterate all online random bots and record a
    // gold-sentinel warehouse row for any above the tax floor. Ledger only - no
    // real gold is moved. Reuses the same helper as the login hook.
    void RunGoldSurplusSweep()
    {
        HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
        for (auto const& pair : players)
        {
            Player* p = pair.second;
            if (!p)
                continue;

            if (!IsRandomBotPlayer(p))
                continue;

            MaybeDepositGoldSurplus(p);
        }
    }

    // Consolidate duplicate bot_warehouse stacks (same owner_guid + item_entry)
    // into a single summed row, EXCEPT for owners belonging to any guild that
    // currently has a real (non-bot) member online - a conservative stand-in
    // for "a player has the guild bank open" (this fork exposes no reliable
    // public guild-bank-open flag; see HANDOFF). This pass only ever touches
    // OUR bot_warehouse table - it never mutates a real guild bank - so the
    // lock is belt-and-suspenders safety.
    //
    // SAFETY: every statement carries an explicit WHERE. We NEVER issue an
    // unconditional DELETE.
    void RunWarehouseSorter()
    {
        // 1) Build the set of "locked" guild ids: guilds with an online, real
        //    (non-bot) member. Skip those owners' rows this pass.
        std::string lockedGuildCsv;
        {
            HashMapHolder<Player>::MapType const& players = ObjectAccessor::GetPlayers();
            for (auto const& pair : players)
            {
                Player* p = pair.second;
                if (!p)
                    continue;

                // Only real players lock a guild; bots don't.
                if (IsRandomBotPlayer(p))
                    continue;

                uint32 gid = p->GetGuildId();
                if (gid == 0)
                    continue;

                if (!lockedGuildCsv.empty())
                    lockedGuildCsv += ",";
                lockedGuildCsv += std::to_string(gid);
            }
        }

        // Optional exclusion clause: owners whose current guild is locked.
        // guild_member (guildid, guid) lives in the characters DB, same DB as
        // bot_warehouse, so this subquery is valid here.
        std::string ownerExclusion;
        if (!lockedGuildCsv.empty())
        {
            ownerExclusion =
                " AND `owner_guid` NOT IN (SELECT `guid` FROM `guild_member` WHERE `guildid` IN (" +
                lockedGuildCsv + "))";
        }

        // 2) Sum duplicates into the surviving (MIN id) row of each group.
        //    HAVING COUNT(*) > 1 => only groups that actually have duplicates.
        CharacterDatabase.Execute(
            "UPDATE `bot_warehouse` w "
            "JOIN (SELECT `owner_guid`, `item_entry`, MIN(`id`) AS keep_id, SUM(`item_count`) AS total "
            "      FROM `bot_warehouse` "
            "      WHERE 1=1{} "
            "      GROUP BY `owner_guid`, `item_entry` HAVING COUNT(*) > 1) g "
            "  ON w.`id` = g.keep_id "
            "SET w.`item_count` = g.total, w.`ts` = {}",
            ownerExclusion, NowUnix());

        // 3) Delete the now-redundant duplicate rows (all but the kept id).
        //    WHERE w.`id` <> g.keep_id is ALWAYS present - never an unqualified
        //    DELETE.
        CharacterDatabase.Execute(
            "DELETE w FROM `bot_warehouse` w "
            "JOIN (SELECT `owner_guid`, `item_entry`, MIN(`id`) AS keep_id "
            "      FROM `bot_warehouse` "
            "      WHERE 1=1{} "
            "      GROUP BY `owner_guid`, `item_entry` HAVING COUNT(*) > 1) g "
            "  ON w.`owner_guid` = g.`owner_guid` AND w.`item_entry` = g.`item_entry` "
            "WHERE w.`id` <> g.keep_id",
            ownerExclusion);

        BotEconomy::Emit("sorter_pass", {
            {"locked_guilds", lockedGuildCsv.empty() ? "none" : lockedGuildCsv},
        });
    }
};

// Self-registration entry point called by loader.cpp.
void AddBotEconomyScripts()
{
    new BotEconPlayerScript();
    new BotEconWorldScript();
}
