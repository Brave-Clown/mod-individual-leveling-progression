#include "IndividualLevelingProgression.h"

#include "Chat.h"
#include "ChatCommand.h"
#include "Config.h"
#include "Player.h"
#include "ScriptMgr.h"

#include <fmt/core.h>

using namespace Acore::ChatCommands;

namespace
{
    // Selects the target player for read-only commands: explicit name arg (if given)
    // resolved to an online Player*, else the handler's session player. Returns nullptr
    // (and reports an error to the handler) if neither is available.
    Player* ResolveTarget(ChatHandler* handler, Optional<PlayerIdentifier> who)
    {
        if (who)
        {
            if (Player* p = who->GetConnectedPlayer())
                return p;
            handler->SendSysMessage("That character is not online.");
            handler->SetSentErrorMessage(true);
            return nullptr;
        }
        if (Player* self = handler->GetSession() ? handler->GetSession()->GetPlayer() : nullptr)
            return self;
        handler->SendSysMessage("This command needs a target name when run from console.");
        handler->SetSentErrorMessage(true);
        return nullptr;
    }

    // Single requirement row in `.ilp status`. cur/req of 0/0 prints "—"
    // so unused gates stay quiet; `enabled=false` (category toggle off) overrides
    // with an explicit "disabled" so the user can tell it's intentionally skipped.
    void ReqLine(ChatHandler* h, char const* label, uint32 cur, uint32 req, bool enabled = true)
    {
        if (!enabled)
        {
            h->PSendSysMessage("  {:<22} |cffAAAAAAdisabled|r", label);
            return;
        }
        if (req == 0)
        {
            h->PSendSysMessage("  {:<22} —", label);
            return;
        }
        char const* color = cur >= req ? "|cff4CFF00" : "|cffFF4444";
        h->PSendSysMessage("  {:<22} {}{}/{}|r", label, color, cur, req);
    }

    // Companion to ReqLine for multi-bit pillars: shows which specific bits
    // (dungeons, SM wings, capitals) are checked vs unchecked. Label order must
    // match the BossIds / CapitalIds list order in the .dist, which the conf
    // comments already lock as append-only.
    void TickLine(ChatHandler* h, Player* p, ILP::SettingIndex idx,
                  std::vector<char const*> const& labels)
    {
        if (!p || labels.empty()) return;
        uint32 mask = p->GetPlayerSetting(ILP::SETTINGS_SOURCE, idx).value;
        std::string line = "    ";
        for (size_t i = 0; i < labels.size(); ++i)
        {
            if (i > 0) line += "  ";
            bool done = ((mask >> i) & 1u) != 0u;
            line += done ? "|cff4CFF00" : "|cffFF4444";
            line += labels[i];
            line += "|r";
        }
        h->SendSysMessage(line.c_str());
    }

    bool HandleStatus(ChatHandler* handler, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;

        auto& cfg = ILP::Cfg();
        ILP::Gate g = ILP::CurrentGate(p);
        bool journey = ILP::IsJourney(p);
        bool complete = ILP::IsComplete(p);

        handler->PSendSysMessage("|cff4CFF00ILP|r — {} (lvl {})", p->GetName(), p->GetLevel());
        handler->PSendSysMessage("  Journey: {}{}|r   Complete: {}{}|r   Gate: |cffFFFF00{}|r",
                                 journey ? "|cff4CFF00on" : "|cffFF4444off", "",
                                 complete ? "|cff4CFF00yes" : "|cffAAAAAAno", "",
                                 ILP::GateName(g));

        switch (g)
        {
            case ILP::GATE_CAP_19:
                ReqLine(handler, "Warsong Gulch",       ILP::WSGCompleted(p),          cfg.cap19_wsg,        cfg.requirePvP);
                break;
            case ILP::GATE_CAP_29:
                ReqLine(handler, "Arathi Basin",        ILP::ABCompleted(p),           cfg.cap29_ab,         cfg.requirePvP);
                ReqLine(handler, "Distinct dungeons",   ILP::Cap29DungeonsDone(p),     cfg.cap29_dungeons,   cfg.requireDungeons);
                TickLine(handler, p, ILP::SETTING_CAP29_DUNGEON_MASK,
                         {"WC", "VC", "SFK", "Stocks", "BFD", "RFK"});
                ReqLine(handler, "First Aid",           ILP::FirstAidSkill(p),         cfg.cap29_firstAid,   cfg.requireFirstAid);
                break;
            case ILP::GATE_CAP_39:
                ReqLine(handler, "Battlegrounds (any)", ILP::Cap39AnyBGCompleted(p),   cfg.cap39_bg,         cfg.requirePvP);
                ReqLine(handler, "Scarlet Monastery",   ILP::Cap39SMWingsDone(p),      cfg.cap39_smWings,    cfg.requireDungeons);
                TickLine(handler, p, ILP::SETTING_CAP39_SM_MASK,
                         {"GY", "Lib", "Arm", "Cath"});
                ReqLine(handler, "First Aid",           ILP::FirstAidSkill(p),         cfg.cap39_firstAid,   cfg.requireFirstAid);
                break;
            case ILP::GATE_CAP_40_BUMP:
                ReqLine(handler, "Gadgetzan FP",        ILP::FlightPathsDiscovered(p), cfg.cap40_flightPaths, cfg.requireExploration);
                ReqLine(handler, "Full zones explored", ILP::FullZonesExplored(p),     cfg.cap40_fullZones,   cfg.requireExploration);
                break;
            case ILP::GATE_CAP_49:
                ReqLine(handler, "Battlegrounds (any)", ILP::Cap49AnyBGCompleted(p),   cfg.cap49_bg,         cfg.requirePvP);
                ReqLine(handler, "Dungeons",            ILP::Cap49DungeonsDone(p),     cfg.cap49_dungeons,   cfg.requireDungeons);
                TickLine(handler, p, ILP::SETTING_CAP49_DUNGEON_MASK,
                         {"Maraudon", "Uldaman", "ZF"});
                ReqLine(handler, "First Aid",           ILP::FirstAidSkill(p),         cfg.cap49_firstAid,   cfg.requireFirstAid);
                break;
            case ILP::GATE_FINALE:
                ReqLine(handler, "Alterac Valley",      ILP::AVCompleted(p),           cfg.finale_av,        cfg.requirePvP);
                ReqLine(handler, "Dungeons",            ILP::FinaleDungeonsDone(p),    cfg.finale_dungeons,  cfg.requireDungeons);
                TickLine(handler, p, ILP::SETTING_FINALE_DUNGEON_MASK,
                         {"BRD", "LBRS", "UBRS", "Strat:UD", "Strat:L",
                          "Scholo", "DM:N", "DM:E", "DM:W"});
                ReqLine(handler, "Capitals visited",    ILP::CapitalsVisited(p),       cfg.finale_capitals,  cfg.requireExploration);
                TickLine(handler, p, ILP::SETTING_CAPITALS_MASK,
                         {"SW", "IF", "Dar", "Org", "TB", "UC"});
                ReqLine(handler, "First Aid",           ILP::FirstAidSkill(p),         cfg.finale_firstAid,  cfg.requireFirstAid);
                break;
            case ILP::GATE_COMPLETE:
                handler->SendSysMessage("  Journey complete. Module no longer gates this character.");
                break;
        }
        return true;
    }

    bool HandleJourneyOn(ChatHandler* handler, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;
        ILP::SetJourney(p, true);
        handler->PSendSysMessage("Journey turned |cff4CFF00on|r for {}.", p->GetName());
        return true;
    }

    bool HandleJourneyOff(ChatHandler* handler, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;
        ILP::SetJourney(p, false);
        handler->PSendSysMessage("Journey turned |cffFF4444off|r for {}.", p->GetName());
        return true;
    }

    bool HandleSkip(ChatHandler* handler, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;

        if (!ILP::SkipPendingValid(p))
        {
            ILP::SkipPendingSet(p);
            handler->PSendSysMessage(
                "|cffFFFF00Confirm skip for {}?|r This grants progression-complete (raid-ready).",
                p->GetName());
            handler->SendSysMessage("Re-run the same command within 60 seconds to confirm.");
            return true;
        }

        ILP::SetComplete(p, true);
        ILP::SkipPendingClear(p);
        handler->PSendSysMessage("|cff4CFF00{}|r marked progression-complete.", p->GetName());
        return true;
    }

    bool HandleUnskip(ChatHandler* handler, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;
        ILP::SetComplete(p, false);
        ILP::SkipPendingClear(p);
        handler->PSendSysMessage("|cffFFFF00{}|r — progression-complete flag cleared.", p->GetName());
        return true;
    }

    bool HandleReset(ChatHandler* handler, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;
        ILP::ResetProgress(p);
        handler->PSendSysMessage(
            "|cffFFFF00{}|r — progression wiped. Journey: {}, gate: {}.",
            p->GetName(),
            ILP::IsJourney(p) ? "on" : "off",
            ILP::GateName(ILP::CurrentGate(p)));
        return true;
    }

    bool HandleReload(ChatHandler* handler)
    {
        sConfigMgr->Reload();
        ILP::LoadConfig();
        handler->SendSysMessage("|cff4CFF00ILP|r config reloaded.");
        return true;
    }

    bool HandleDevXP(ChatHandler* handler, uint32 amount, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;
        // Player::GiveXP itself does NOT fire OnPlayerGiveXP — only the upstream
        // XP sources (kill, quest, explore, BG) do. To exercise the cap clamp,
        // fire the hook ourselves with the same shape KillRewarder uses.
        uint32 modified = amount;
        sScriptMgr->OnPlayerGiveXP(p, modified, nullptr, PlayerXPSource::XPSOURCE_KILL);
        p->GiveXP(modified, nullptr, 0.f);
        handler->PSendSysMessage("{}: requested {} XP, hook clamped to {}. Bar now {}/{}.",
                                 p->GetName(), amount, modified,
                                 p->GetUInt32Value(PLAYER_XP),
                                 p->GetUInt32Value(PLAYER_NEXT_LEVEL_XP));
        return true;
    }

    bool HandleDevBG(ChatHandler* handler, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;
        // Credits whichever counter the current gate uses (WSG / AB / Cap-39 BG /
        // Cap-49 BG / AV). No-op for Cap 40 bump and Complete.
        ILP::CreditCurrentGateBG(p);
        return true;
    }

    bool HandleDevDungeon(ChatHandler* handler, uint32 bossEntry, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;
        // Routes through the same path OnPlayerCreatureKill uses, so the gate /
        // already-credited / current-gate filter all behave identically. If
        // nothing prints, the entry isn't tracked for this player's current gate.
        ILP::CreditDungeonBoss(p, bossEntry);
        return true;
    }

    bool HandleDevCapital(ChatHandler* handler, uint32 zoneId, Optional<PlayerIdentifier> who)
    {
        Player* p = ResolveTarget(handler, who);
        if (!p) return false;
        // Same path as the OnPlayerUpdateZone hook. Silent no-op if the zone
        // isn't in CapitalIds, the bit's already set, or journey is off.
        ILP::CreditCapitalVisit(p, zoneId);
        return true;
    }

    bool HandleDebugDump(ChatHandler* handler)
    {
        auto& c = ILP::Cfg();
        handler->PSendSysMessage("Enable={} Announce={} Debug={} JourneyDefault={}",
                                 c.enable, c.announce, c.debug, c.journeyDefault);
        handler->PSendSysMessage("Require PvP={} Dungeons={} FirstAid={} Exploration={}",
                                 c.requirePvP, c.requireDungeons, c.requireFirstAid, c.requireExploration);
        handler->PSendSysMessage("Cap19 WSG={}", c.cap19_wsg);
        handler->PSendSysMessage("Cap29 AB={} Dungeons={} FirstAid={}",
                                 c.cap29_ab, c.cap29_dungeons, c.cap29_firstAid);
        handler->PSendSysMessage("Cap39 BG={} SMWings={} FirstAid={}",
                                 c.cap39_bg, c.cap39_smWings, c.cap39_firstAid);
        handler->PSendSysMessage("Cap40 FlightPaths={} FullZones={}",
                                 c.cap40_flightPaths, c.cap40_fullZones);
        handler->PSendSysMessage("Cap49 BG={} Dungeons={} FirstAid={}",
                                 c.cap49_bg, c.cap49_dungeons, c.cap49_firstAid);
        handler->PSendSysMessage("Finale AV={} Dungeons={} Capitals={} FirstAid={}",
                                 c.finale_av, c.finale_dungeons, c.finale_capitals, c.finale_firstAid);
        return true;
    }
}

class IndividualLevelingProgressionCommandScript : public CommandScript
{
public:
    IndividualLevelingProgressionCommandScript() : CommandScript("IndividualLevelingProgressionCommandScript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable journeyTable =
        {
            { "on",  HandleJourneyOn,  SEC_GAMEMASTER, Console::Yes },
            { "off", HandleJourneyOff, SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable debugTable =
        {
            { "dump", HandleDebugDump, SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable devTable =
        {
            { "xp",      HandleDevXP,      SEC_GAMEMASTER, Console::Yes },
            { "bg",      HandleDevBG,      SEC_GAMEMASTER, Console::Yes },
            { "dungeon", HandleDevDungeon, SEC_GAMEMASTER, Console::Yes },
            { "capital", HandleDevCapital, SEC_GAMEMASTER, Console::Yes },
        };

        static ChatCommandTable progressionTable =
        {
            { "status",  HandleStatus,  SEC_PLAYER,     Console::Yes },
            { "skip",    HandleSkip,    SEC_GAMEMASTER, Console::Yes },
            { "unskip",  HandleUnskip,  SEC_GAMEMASTER, Console::Yes },
            { "reset",   HandleReset,   SEC_GAMEMASTER, Console::Yes },
            { "reload",  HandleReload,  SEC_GAMEMASTER, Console::Yes },
            { "journey", journeyTable },
            { "debug",   debugTable },
            { "dev",     devTable },
        };

        static ChatCommandTable root =
        {
            { "ilp", progressionTable },
        };
        return root;
    }
};

void RegisterIndividualLevelingProgressionCommandScript()
{
    new IndividualLevelingProgressionCommandScript();
}
