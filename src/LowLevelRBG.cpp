/*
 * Copyright (C) 2016+ AzerothCore <www.azerothcore.org>, released under GNU AGPL v3 license: https://github.com/azerothcore/azerothcore-wotlk/blob/master/LICENSE-AGPL3
 */

#include "ArenaTeamMgr.h"
#include "BattlegroundMgr.h"
#include "Chat.h"
#include "Config.h"
#include "DisableMgr.h"
#include "Language.h"
#include "LFGMgr.h"
#include "Player.h"
#include "ScriptMgr.h"

using namespace Acore::ChatCommands;

class low_level_rbg_commandscript : public CommandScript
{
public:
    low_level_rbg_commandscript() : CommandScript("low_level_rbg_commandscript") { }

    ChatCommandTable GetCommands() const override
    {
        static ChatCommandTable lowLevelRbgCommandTable =
        {
            { "queue",  HandleLowLevelRBGCommand,   SEC_PLAYER, Console::No }
        };

        static ChatCommandTable commandTable =
        {
            { "rbg",    lowLevelRbgCommandTable }
        };

        return commandTable;
    }

    static bool HandleLowLevelRBGCommand(ChatHandler* handler)
    {
        Player* player = handler->GetSession()->GetPlayer();
        if (!player)
        {
            handler->SendSysMessage(LANG_NO_PLAYERS_FOUND);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // ignore if player is already in BG
        if (player->InBattleground())
            return false;

        if (sConfigMgr->GetOption<int32>("LowLevelRBG.MinLevelRBG", 10) > player->getLevel())
        {
            handler->PSendSysMessage("Too low level to get queued up for RBG");
            handler->SetSentErrorMessage(true);
            return false;
        }

        if (sConfigMgr->GetOption<int32>("LowLevelRBG.MaxLevelRBG", 79) < player->getLevel())
        {
            handler->PSendSysMessage("Too high level to get queued up for RBG");
            handler->SetSentErrorMessage(true);
            return false;
        }

        BattlegroundTypeId bgTypeId = BATTLEGROUND_RB;
        // entry not found
        if (!sBattlemasterListStore.LookupEntry(bgTypeId))
        {
            return false;
        }

        // chosen battleground type is disabled
        if (DisableMgr::IsDisabledFor(DISABLE_TYPE_BATTLEGROUND, bgTypeId, nullptr))
        {
            handler->SendSysMessage(LANG_BG_DISABLED);
            handler->SetSentErrorMessage(true);
            return false;
        }

        // get queue typeid and random typeid to check if already queued for them
        BattlegroundQueueTypeId bgQueueTypeId = BattlegroundMgr::BGQueueTypeId(bgTypeId, 0);
        BattlegroundQueueTypeId bgQueueTypeIdRandom = BattlegroundMgr::BGQueueTypeId(BATTLEGROUND_RB, 0);

        // safety check - bgQueueTypeId == BATTLEGROUND_QUEUE_NONE if tried to queue for arena using this function
        if (bgQueueTypeId == BATTLEGROUND_QUEUE_NONE)
        {
            return false;
        }

        // get bg template
        Battleground* bgt = sBattlegroundMgr->GetBattlegroundTemplate(bgTypeId);
        if (!bgt)
        {
            return false;
        }

        // expected bracket entry
        PvPDifficultyEntry const* bracketEntry = GetBattlegroundBracketByLevel(bgt->GetMapId(), player->getLevel());
        if (!bracketEntry)
        {
            return false;
        }

        // must have free queue slot
        if (!player->HasFreeBattlegroundQueueId())
        {
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, ERR_BATTLEGROUND_TOO_MANY_QUEUES);
            player->SendDirectMessage(&data);
            return false;
        }

        // queue result (default ok)
        GroupJoinBattlegroundResult err = GroupJoinBattlegroundResult(bgt->GetBgTypeID());
        if (!sScriptMgr->CanJoinInBattlegroundQueue(player, ObjectGuid::Empty, bgTypeId, false, err) && err <= 0)
        {
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, err);
            player->SendDirectMessage(&data);
            return false;
        }

        BattlegroundQueue& bgQueue = sBattlegroundMgr->GetBattlegroundQueue(bgQueueTypeId);

        lfg::LfgState lfgState = sLFGMgr->GetState(player->GetGUID());
        // check if player can queue:
        if (player->InBattleground()) // currently in battleground
        {
            err = ERR_BATTLEGROUND_NOT_IN_BATTLEGROUND;
        }
        else if (lfgState > lfg::LFG_STATE_NONE && (lfgState != lfg::LFG_STATE_QUEUED || !sWorld->getBoolConfig(CONFIG_ALLOW_JOIN_BG_AND_LFG))) // using lfg system
        {
            err = ERR_LFG_CANT_USE_BATTLEGROUND;
        }
        else if (!player->CanJoinToBattleground()) // has deserter debuff
        {
            err = ERR_GROUP_JOIN_BATTLEGROUND_DESERTERS;
        }
        else if (player->InBattlegroundQueueForBattlegroundQueueType(bgQueueTypeIdRandom)) // queued for random bg, so can't queue for anything else
        {
            err = ERR_IN_RANDOM_BG;
        }
        else if (player->InBattlegroundQueue()) // already in queue, so can't queue for random
        {
            err = ERR_IN_NON_RANDOM_BG;
        }
        else if (player->InBattlegroundQueueForBattlegroundQueueType(BATTLEGROUND_QUEUE_2v2) ||
            player->InBattlegroundQueueForBattlegroundQueueType(BATTLEGROUND_QUEUE_3v3) ||
            player->InBattlegroundQueueForBattlegroundQueueType(BATTLEGROUND_QUEUE_5v5)) // can't be already queued for arenas
        {
            err = ERR_BATTLEGROUND_QUEUED_FOR_RATED;
        }
        // don't let Death Knights join BG queues when they are not allowed to be teleported yet
        else if (player->getClass() == CLASS_DEATH_KNIGHT && player->GetMapId() == 609 && !player->IsGameMaster() && !player->HasSpell(50977))
        {
            err = ERR_BATTLEGROUND_NONE;
        }

        if (err <= 0)
        {
            WorldPacket data;
            sBattlegroundMgr->BuildGroupJoinedBattlegroundPacket(&data, err);
            player->SendDirectMessage(&data);
            return false;
        }

        GroupQueueInfo* ginfo = bgQueue.AddGroup(player, nullptr, bgTypeId, bracketEntry, 0, false, false, 0, 0);
        uint32 avgWaitTime = bgQueue.GetAverageQueueWaitTime(ginfo);
        uint32 queueSlot = player->AddBattlegroundQueueId(bgQueueTypeId);

        // send status packet
        WorldPacket data;
        sBattlegroundMgr->BuildBattlegroundStatusPacket(&data, bgt, queueSlot, STATUS_WAIT_QUEUE, avgWaitTime, 0, 0, TEAM_NEUTRAL);
        player->SendDirectMessage(&data);

        sScriptMgr->OnPlayerJoinBG(player);

        sBattlegroundMgr->ScheduleQueueUpdate(0, 0, bgQueueTypeId, bgTypeId, bracketEntry->GetBracketId());

        return true;
    }
};

void AddLowLevelRBGScripts()
{
    new low_level_rbg_commandscript();
}
