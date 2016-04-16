//====== Copyright (c) 2014, Valve Corporation, All rights reserved. ========//
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF
// THE POSSIBILITY OF SUCH DAMAGE.
//===========================================================================//

#include <algorithm>
#include <stdarg.h>
#include <string>
#include <set>
#include <unordered_map>
#include <json_spirit.h>
#include "demofile.h"
#include "demofiledump.h"
#include "demofilepropdecode.h"
#include "win_stuff.h"
#include "geometry.h"
#include "google/protobuf/descriptor.h"
#include "google/protobuf/reflection_ops.h"
#include "google/protobuf/descriptor.pb.h"
#include "cstrike15_usermessages.pb.h"
#include "netmessages.pb.h"
#if defined(_WIN32) || defined(_WIN64)
#include <io.h>
#include <fcntl.h>
#endif
#if defined(_WIN32) || defined(_WIN64)
#include <locale>
#include <codecvt>
#else
#include "utf8.h"
#endif

// file globals
static int s_nNumStringTables;
static StringTableData_t s_StringTables[MAX_STRING_TABLES];

static int s_nServerClassBits = 0;
static std::vector<ServerClass_t> s_ServerClasses;
static std::vector<CSVCMsg_SendTable> s_DataTables;
static std::vector<ExcludeEntry> s_currentExcludes;
static std::vector<EntityEntry *> s_Entities;
static std::vector<player_info_t> s_PlayerInfos;
static std::map<int, player_info_t> userid_info;
// map xuid to player slot
static std::map<uint64, int> player_slot;
enum { DT_CSPlayer = 0, DT_CSGameRulesProxy = 1, DT_CSTeam = 2 };
int playerCoordIndex, playerCoordIndex2;
int serverClassesIds[3];

extern bool g_bDumpJson;
extern bool g_bPrettyJson;
extern bool g_bDumpGameEvents;
extern bool g_bOnlyHsBoxEvents;
extern bool g_bSupressFootstepEvents;
extern bool g_bShowExtraPlayerInfoInGameEvents;
extern bool g_bDumpDeaths;
extern bool g_bSupressWarmupDeaths;
extern bool g_bDumpStringTables;
extern bool g_bDumpDataTables;
extern bool g_bDumpPacketEntities;
extern bool g_bDumpNetMessages;

static bool s_bMatchStartOccured = false;
static int s_nCurrentTick;
json_spirit::wmObject player_names;

EntityEntry *FindEntity(int nEntity);
player_info_t *FindPlayerInfo(int userId);
int FindPlayerEntityIndex(int userId);

std::wstring toWide(const std::string &s) {
#if defined(_WIN32) || defined(_WIN64)
    return std::wstring_convert<std::codecvt_utf8<wchar_t>>().from_bytes(s);
#else
    std::wstring r;
    utf8::utf8to32(s.begin(), s.end(), std::back_inserter(r));
    return r;
#endif
}

void addUserId(const player_info_t &playerInfo) {
    userid_info[playerInfo.userID] = playerInfo;
    if (!playerInfo.fakeplayer && !playerInfo.ishltv)
        player_names[std::to_wstring(playerInfo.xuid)] = toWide(playerInfo.name);
    if (!playerInfo.fakeplayer) {
        for (int i = 0; i < s_PlayerInfos.size(); ++i)
            if (s_PlayerInfos[i].userID == playerInfo.userID) {
                player_slot[playerInfo.xuid] = i;
                break;
            }
    }
}

uint64 guid2xuid(std::string guid) {
    return 2 * std::stoll(guid.substr(10)) + 76561197960265728LL + (guid[8] == '1');
}

json_spirit::wmArray events;
json_spirit::wmObject match;
json_spirit::wmObject mm_rank_update;
std::pair<int, int> score_snapshot;

struct Team {
    int total_score;
};

std::unordered_map<int, int> id2teamno;
Team teams[4];
double tick_rate = -1;
std::map<uint64_t, int> jumped_last;
const double jump_duration = 0.75; // seconds
const double smoke_radius = 140;
const double player_height = 72;
const double player_crouch_height = 50;
const double smoke_height = 130;
std::map<uint64_t, int> bot_takeover;
// Map entityid to Point
std::map<int, Point> smokes;

json_spirit::wmArray point_to_json(const Point &p) {
    return json_spirit::wmArray({int(p.x), int(p.y), int(p.z)});
}

void addSmokes(Point p1, Point p2, json_spirit::wmObject &event) {
    json_spirit::wmArray tmp;
    for (auto &kv : smokes) {
        Point killer(p1.x, p1.y, p1.z + player_crouch_height);
        // Check if shooting to the legs AND head of the victim goes through smoke
        if (intersects(killer, p2, kv.second, smoke_radius, smoke_height) &&
            intersects(killer, Point(p2.x, p2.y, p2.z + player_height), kv.second, smoke_radius,
                       smoke_height)) {
            tmp.push_back(point_to_json(kv.second));
        }
    }
    if (!tmp.empty())
        event[L"smoke"] = tmp;
}

static std::set<std::wstring> hsbox_events = {L"player_death",
                                              L"round_start",
                                              L"round_end",
                                              L"player_spawn",
                                              L"game_restart",
                                              L"score_changed",
                                              L"player_hurt",
                                              L"bomb_defused",
                                              L"player_disconnected",
                                              L"round_officially_ended"};

void addEvent(const std::map<std::wstring, json_spirit::wmConfig::Value_type> &object_) {
    std::map<std::wstring, json_spirit::wmConfig::Value_type> object = object_;
    if (g_bOnlyHsBoxEvents) {
        std::wstring type = object.at(L"type").get_str();
        // Save score snapshot for later when we check if we're switching sides
        if (type == L"round_start") {
            score_snapshot = std::make_pair(teams[2].total_score, teams[3].total_score);
            bot_takeover.clear();
            smokes.clear();
        } else if (type == L"player_jump") {
            jumped_last[object.at(L"userid").get_int64()] = s_nCurrentTick;
        } else if (type == L"player_death") {
            uint64 attackerid = object.at(L"attacker").get_int64();
            if (tick_rate > 0 && jumped_last.count(attackerid) &&
                jumped_last[attackerid] >= s_nCurrentTick - jump_duration / tick_rate) {
                object[L"jump"] = s_nCurrentTick - jumped_last[attackerid];
            }
        } else if (type == L"bot_takeover") {
            uint64 human = object.at(L"userid").get_int64();
            int bot = object.at(L"botid").get_int();
            bot_takeover[human] = bot;
        } else if (type == L"smokegrenade_detonate") {
            smokes[object.at(L"entityid").get_int()] = Point(
                object.at(L"x").get_real(), object.at(L"y").get_real(), object.at(L"z").get_real());
        } else if (type == L"smokegrenade_expired") {
            smokes.erase(object.at(L"entityid").get_int());
        }
    }
    if (!g_bOnlyHsBoxEvents ||
        (g_bOnlyHsBoxEvents && hsbox_events.count(object.at(L"type").get_str())))
        events.push_back(object);
}

uint64 getXuid(int userid) {
    player_info_t *pPlayerInfo = FindPlayerInfo(userid);
    if (pPlayerInfo && !pPlayerInfo->fakeplayer)
        return pPlayerInfo->xuid;
    return userid;
}


void fatal_errorf(const char *fmt, ...) {
    va_list vlist;
    char buf[1024];

    va_start(vlist, fmt);
    vsnprintf(buf, sizeof(buf), fmt, vlist);
    buf[sizeof(buf) - 1] = 0;
    va_end(vlist);

    fprintf(stderr, "\nERROR: %s\n", buf);
    exit(-1);
}

bool CDemoFileDump::Open(const char *filename) {
    if (!m_demofile.Open(filename)) {
        fprintf(stderr, "Couldn't open '%s'\n", filename);
        return false;
    }

    return true;
}

void CDemoFileDump::MsgPrintf(const ::google::protobuf::Message &msg, int size) {
    if (g_bDumpNetMessages && !g_bDumpJson) {
        const std::string &TypeName = msg.GetTypeName();

        // Print the message type and size
        printf("---- %s (%d bytes) -----------------\n%s", TypeName.c_str(), size,
               msg.DebugString().c_str());
    }
}

template <class T, int msgType>
void PrintUserMessage(CDemoFileDump &Demo, const void *parseBuffer, int BufferSize) {
    T msg;

    if (msg.ParseFromArray(parseBuffer, BufferSize)) {
        Demo.MsgPrintf(msg, BufferSize);
    }
}

template <>
void PrintUserMessage<CCSUsrMsg_ServerRankUpdate, CS_UM_ServerRankUpdate>(CDemoFileDump &Demo,
                                                                          const void *parseBuffer,
                                                                          int BufferSize) {
    CCSUsrMsg_ServerRankUpdate msg;
    if (msg.ParseFromArray(parseBuffer, BufferSize)) {
        if (g_bDumpJson) {
            if (g_bOnlyHsBoxEvents) {
                for (int i = 0; i < msg.rank_update_size(); ++i) {
                    const auto &ru = msg.rank_update(i);
                    uint64 xuid = 76561197960265728LL + ru.account_id();
                    json_spirit::wmObject tmp;
                    if (ru.has_num_wins())
                        tmp[L"num_wins"] = ru.num_wins();
                    if (ru.has_rank_old())
                        tmp[L"rank_old"] = ru.rank_old();
                    if (ru.has_rank_new())
                        tmp[L"rank_new"] = ru.rank_new();
                    if (ru.has_rank_change())
                        tmp[L"rank_change"] = ru.rank_change();
                    mm_rank_update[std::to_wstring(xuid)] = tmp;
                }
            }
        } else
            Demo.MsgPrintf(msg, BufferSize);
    } else {
    }
}

void CDemoFileDump::DumpUserMessage(const void *parseBuffer, int BufferSize) {
    CSVCMsg_UserMessage userMessage;

    if (userMessage.ParseFromArray(parseBuffer, BufferSize)) {
        int Cmd = userMessage.msg_type();
        int SizeUM = userMessage.msg_data().size();
        const void *parseBufferUM = &userMessage.msg_data()[0];
        switch (Cmd) {
#define HANDLE_UserMsg(_x)                                                                         \
    case CS_UM_##_x:                                                                               \
        PrintUserMessage<CCSUsrMsg_##_x, CS_UM_##_x>(*this, parseBufferUM, SizeUM);                \
        break

        default:
            // unknown user message
            break;
            HANDLE_UserMsg(VGUIMenu);
            HANDLE_UserMsg(Geiger);
            HANDLE_UserMsg(Train);
            HANDLE_UserMsg(HudText);
            HANDLE_UserMsg(SayText);
            HANDLE_UserMsg(SayText2);
            HANDLE_UserMsg(TextMsg);
            HANDLE_UserMsg(HudMsg);
            HANDLE_UserMsg(ResetHud);
            HANDLE_UserMsg(GameTitle);
            HANDLE_UserMsg(Shake);
            HANDLE_UserMsg(Fade);
            HANDLE_UserMsg(Rumble);
            HANDLE_UserMsg(CloseCaption);
            HANDLE_UserMsg(CloseCaptionDirect);
            HANDLE_UserMsg(SendAudio);
            HANDLE_UserMsg(RawAudio);
            HANDLE_UserMsg(VoiceMask);
            HANDLE_UserMsg(RequestState);
            HANDLE_UserMsg(Damage);
            HANDLE_UserMsg(RadioText);
            HANDLE_UserMsg(HintText);
            HANDLE_UserMsg(KeyHintText);
            HANDLE_UserMsg(ProcessSpottedEntityUpdate);
            HANDLE_UserMsg(ReloadEffect);
            HANDLE_UserMsg(AdjustMoney);
            HANDLE_UserMsg(StopSpectatorMode);
            HANDLE_UserMsg(KillCam);
            HANDLE_UserMsg(DesiredTimescale);
            HANDLE_UserMsg(CurrentTimescale);
            HANDLE_UserMsg(AchievementEvent);
            HANDLE_UserMsg(MatchEndConditions);
            HANDLE_UserMsg(DisconnectToLobby);
            HANDLE_UserMsg(DisplayInventory);
            HANDLE_UserMsg(WarmupHasEnded);
            HANDLE_UserMsg(ClientInfo);
            HANDLE_UserMsg(CallVoteFailed);
            HANDLE_UserMsg(VoteStart);
            HANDLE_UserMsg(VotePass);
            HANDLE_UserMsg(VoteFailed);
            HANDLE_UserMsg(VoteSetup);
            HANDLE_UserMsg(SendLastKillerDamageToClient);
            HANDLE_UserMsg(ItemPickup);
            HANDLE_UserMsg(ShowMenu);
            HANDLE_UserMsg(BarTime);
            HANDLE_UserMsg(AmmoDenied);
            HANDLE_UserMsg(MarkAchievement);
            HANDLE_UserMsg(ItemDrop);
            HANDLE_UserMsg(GlowPropTurnOff);
            HANDLE_UserMsg(ServerRankUpdate);

#undef HANDLE_UserMsg
        }
    }
}

template <class T, int msgType>
void PrintNetMessage(CDemoFileDump &Demo, const void *parseBuffer, int BufferSize) {
    T msg;

    if (msg.ParseFromArray(parseBuffer, BufferSize)) {
        if (msgType == svc_GameEventList) {
            Demo.m_GameEventList.CopyFrom(msg);
        }
        Demo.MsgPrintf(msg, BufferSize);
    }
}

template <>
void PrintNetMessage<CSVCMsg_ServerInfo, svc_ServerInfo>(CDemoFileDump &Demo,
                                                         const void *parseBuffer,
                                                         int BufferSize) {
    CSVCMsg_ServerInfo serverInfo;

    if (g_bDumpJson) {
        if (serverInfo.ParseFromArray(parseBuffer, BufferSize) && serverInfo.has_map_name()) {
            match[L"map"] = toWide(serverInfo.map_name());
            match[L"tickrate"] = serverInfo.tick_interval();
        }
    } else
        Demo.DumpUserMessage(parseBuffer, BufferSize);
    tick_rate = serverInfo.tick_interval();
}

template <>
void PrintNetMessage<CSVCMsg_UserMessage, svc_UserMessage>(CDemoFileDump &Demo,
                                                           const void *parseBuffer,
                                                           int BufferSize) {
    Demo.DumpUserMessage(parseBuffer, BufferSize);
}

player_info_t *FindPlayerInfo(int userId) {
    for (std::vector<player_info_t>::iterator i = s_PlayerInfos.begin(); i != s_PlayerInfos.end();
         i++) {
        if (i->userID == userId) {
            return &(*i);
        }
    }

    try {
        return &userid_info.at(userId);
    } catch (...) {
    }

    return NULL;
}

int FindPlayerEntityIndex(int userId) {
    int nIndex = 0;
    for (std::vector<player_info_t>::iterator i = s_PlayerInfos.begin(); i != s_PlayerInfos.end();
         i++) {
        if (i->userID == userId) {
            return nIndex;
        }
        nIndex++;
    }

    return -1;
}

const CSVCMsg_GameEventList::descriptor_t *GetGameEventDescriptor(const CSVCMsg_GameEvent &msg,
                                                                  CDemoFileDump &Demo) {
    int iDescriptor;

    for (iDescriptor = 0; iDescriptor < Demo.m_GameEventList.descriptors().size(); iDescriptor++) {
        const CSVCMsg_GameEventList::descriptor_t &Descriptor =
            Demo.m_GameEventList.descriptors(iDescriptor);

        if (Descriptor.eventid() == msg.eventid())
            break;
    }

    if (iDescriptor == Demo.m_GameEventList.descriptors().size()) {
        if (g_bDumpGameEvents) {
            if (!g_bDumpJson)
                printf("%s", msg.DebugString().c_str());
        }
        return NULL;
    }

    return &Demo.m_GameEventList.descriptors(iDescriptor);
}

bool HandlePlayerConnectDisconnectEvents(const CSVCMsg_GameEvent &msg,
                                         const CSVCMsg_GameEventList::descriptor_t *pDescriptor) {
    // need to handle player_connect and player_disconnect because this is the only place bots get
    // added to our player info array
    // actual players come in via string tables
    bool bPlayerDisconnect = (pDescriptor->name().compare("player_disconnect") == 0);
    if (pDescriptor->name().compare("player_connect") == 0 || bPlayerDisconnect) {
        int numKeys = msg.keys().size();
        int userid = -1;
        unsigned int index = -1;
        const char *name = NULL;
        bool bBot = false;
        const char *reason = NULL;
        std::string guid;
        for (int i = 0; i < numKeys; i++) {
            const CSVCMsg_GameEventList::key_t &Key = pDescriptor->keys(i);
            const CSVCMsg_GameEvent::key_t &KeyValue = msg.keys(i);

            if (Key.name().compare("userid") == 0) {
                userid = KeyValue.val_short();
            } else if (Key.name().compare("index") == 0) {
                index = KeyValue.val_byte();
            } else if (Key.name().compare("name") == 0) {
                name = KeyValue.val_string().c_str();
            } else if (Key.name().compare("networkid") == 0) {
                guid = KeyValue.val_string();
                bBot = (KeyValue.val_string().compare("BOT") == 0);
            } else if (Key.name().compare("bot") == 0) {
                bBot = KeyValue.val_bool();
            } else if (Key.name().compare("reason") == 0) {
                reason = KeyValue.val_string().c_str();
            }
        }
        if (!g_bDumpJson)
            printf("userid %d index %d\n", userid, index);

        if (bPlayerDisconnect) {
            if (g_bDumpGameEvents) {
                if (g_bDumpJson)
                    addEvent({{L"type", L"player_disconnected"},
                              {L"name", toWide(name)},
                              {L"tick", s_nCurrentTick},
                              {L"reason", toWide(reason)},
                              {L"userid", getXuid(userid)}});
                else
                    printf("Player %s (id:%d) disconnected. reason:%s\n", name, userid, reason);
            }
            // mark the player info slot as disconnected
            player_info_t *pPlayerInfo = FindPlayerInfo(userid);
            if (pPlayerInfo) {
                if (!g_bDumpJson)
                    printf("Mark Player %s %s (id:%d) as disconnected\n", pPlayerInfo->name,
                           pPlayerInfo->guid, pPlayerInfo->userID);
                strcpy(pPlayerInfo->name, "disconnected");
                pPlayerInfo->userID = -1;
                pPlayerInfo->guid[0] = 0;
            }
        } else {
            player_info_t newPlayer;
            memset(&newPlayer, 0, sizeof(newPlayer));
            newPlayer.userID = userid;
            strcpy(newPlayer.name, name);
            newPlayer.fakeplayer = bBot;
            if (bBot) {
                strcpy(newPlayer.guid, "BOT");
            } else {
                strcpy(newPlayer.guid, guid.c_str());
                newPlayer.xuid = guid2xuid(guid);
            }

            addUserId(newPlayer);

            if (index < s_PlayerInfos.size()) {
                // only replace existing player slot if the userID is different (very unlikely)
                if (s_PlayerInfos[index].userID != userid) {
                    if (!g_bDumpJson) {
                        printf("userid %d index %d\n", userid, index);
                        printf("Player %s %s %ld (id:%d) replaced with Player %s %s %ld (id:%d).\n",
                               s_PlayerInfos[index].guid, s_PlayerInfos[index].name,
                               s_PlayerInfos[index].xuid, s_PlayerInfos[index].userID,
                               newPlayer.guid, newPlayer.name, newPlayer.xuid, newPlayer.userID);
                    }
                    if (std::string(s_PlayerInfos[index].name).compare(name) != 0)
                        s_PlayerInfos[index] = newPlayer;
                }
            } else {
                if (g_bDumpGameEvents) {
                    if (g_bDumpJson)
                        addEvent({{L"type", L"connect"},
                                  {L"name", toWide(name)},
                                  {L"steamid", toWide(newPlayer.guid)},
                                  {L"userid", userid}});
                    else
                        printf("Player %s %s (id:%d) connected.\n", newPlayer.guid, name, userid);
                }
                s_PlayerInfos.push_back(newPlayer);
            }
        }
        return true;
    }
    return false;
}

bool getPlayerPosition(int nIndex, Point &p) {
    int nEntityIndex = FindPlayerEntityIndex(nIndex) + 1;
    EntityEntry *pEntity = FindEntity(nEntityIndex);
    if (pEntity) {
        PropEntry *pXYProp = pEntity->FindProp("m_vecOrigin");
        PropEntry *pZProp = pEntity->FindProp("m_vecOrigin[2]");
        if (pXYProp && pZProp) {
            p = Point(pXYProp->m_pPropValue->m_value.m_vector.x,
                      pXYProp->m_pPropValue->m_value.m_vector.y,
                      pZProp->m_pPropValue->m_value.m_float);
            return true;
        }
    }
    return false;
}

bool ShowPlayerInfo(json_spirit::wmObject &event,
                    const char *pField,
                    int nIndex,
                    bool bShowDetails = true,
                    bool bCSV = false) {
    player_info_t *pPlayerInfo = FindPlayerInfo(nIndex);
    if (pPlayerInfo) {
        if (bCSV) {
            printf("%s, %s, %d", pField, pPlayerInfo->name, nIndex);
        } else {
            if (g_bDumpJson) {
                std::wstring field = toWide(pField);
                if (pPlayerInfo->fakeplayer)
                    event[field] = nIndex;
                else {
                    event[field] = pPlayerInfo->xuid;
                    std::wstring type = event[L"type"].get_str();
                    // Ignore player_spawn as this happens before round_start (where we clear the
                    // bot_takeover map)
                    // Also ignore player_death's assister field as csgo does the same
                    // (resulting in awarding assists to the controlling human instead of the bot)
                    if (bot_takeover.count(pPlayerInfo->xuid) && type != L"bot_takeover" &&
                        type != L"player_spawn" &&
                        (type != L"player_death" ||
                         (type == L"player_death" && field != L"assister")))
                        event[field] = bot_takeover[pPlayerInfo->xuid];
                }
            } else
                printf(" %s: %s %ld (id:%d)\n", pField, pPlayerInfo->name, pPlayerInfo->xuid,
                       nIndex);
        }

        if (bShowDetails) {
            int nEntityIndex = FindPlayerEntityIndex(nIndex) + 1;
            EntityEntry *pEntity = FindEntity(nEntityIndex);
            if (pEntity) {
                PropEntry *pXYProp = pEntity->FindProp("m_vecOrigin");
                PropEntry *pZProp = pEntity->FindProp("m_vecOrigin[2]");
                if (pXYProp && pZProp) {
                    if (bCSV) {
                        printf(", %f, %f, %f", pXYProp->m_pPropValue->m_value.m_vector.x,
                               pXYProp->m_pPropValue->m_value.m_vector.y,
                               pZProp->m_pPropValue->m_value.m_float);
                    } else {
                        printf("  position: %f, %f, %f\n",
                               pXYProp->m_pPropValue->m_value.m_vector.x,
                               pXYProp->m_pPropValue->m_value.m_vector.y,
                               pZProp->m_pPropValue->m_value.m_float);
                    }
                }
                PropEntry *pAngle0Prop = pEntity->FindProp("m_angEyeAngles[0]");
                PropEntry *pAngle1Prop = pEntity->FindProp("m_angEyeAngles[1]");
                if (pAngle0Prop && pAngle1Prop) {
                    if (bCSV) {
                        printf(", %f, %f", pAngle0Prop->m_pPropValue->m_value.m_float,
                               pAngle1Prop->m_pPropValue->m_value.m_float);
                    } else {
                        printf("  facing: pitch:%f, yaw:%f\n",
                               pAngle0Prop->m_pPropValue->m_value.m_float,
                               pAngle1Prop->m_pPropValue->m_value.m_float);
                    }
                }
                PropEntry *pTeamProp = pEntity->FindProp("m_iTeamNum");
                if (pTeamProp) {
                    if (bCSV) {
                        printf(", %s", (pTeamProp->m_pPropValue->m_value.m_int == 2) ? "T" : "CT");
                    } else {
                        printf("  team: %s\n",
                               (pTeamProp->m_pPropValue->m_value.m_int == 2) ? "T" : "CT");
                    }
                }
            }
        }
        return true;
    }
    if (!g_bDumpJson)
        printf("Cannot find player %d info.\n", nIndex);
    return false;
}

void HandlePlayerDeath(json_spirit::wmObject &event,
                       const CSVCMsg_GameEvent &msg,
                       const CSVCMsg_GameEventList::descriptor_t *pDescriptor) {
    int numKeys = msg.keys().size();

    int userid = -1;
    int attackerid = -1;
    int assisterid = 0;
    const char *pWeaponName = NULL;
    bool bHeadshot = false;
    for (int i = 0; i < numKeys; i++) {
        const CSVCMsg_GameEventList::key_t &Key = pDescriptor->keys(i);
        const CSVCMsg_GameEvent::key_t &KeyValue = msg.keys(i);

        if (Key.name().compare("userid") == 0) {
            userid = KeyValue.val_short();
        } else if (Key.name().compare("attacker") == 0) {
            attackerid = KeyValue.val_short();
        } else if (Key.name().compare("assister") == 0) {
            assisterid = KeyValue.val_short();
        } else if (Key.name().compare("weapon") == 0) {
            pWeaponName = KeyValue.val_string().c_str();
        } else if (Key.name().compare("headshot") == 0) {
            bHeadshot = KeyValue.val_bool();
        }
    }

    ShowPlayerInfo(event, "victim", userid, true, true);
    if (!g_bDumpJson)
        printf(", ");
    ShowPlayerInfo(event, "attacker", attackerid, true, true);
    if (!g_bDumpJson)
        printf(", %s, %s", pWeaponName, bHeadshot ? "true" : "false");
    if (assisterid != 0) {
        if (!g_bDumpJson)
            printf(", ");
        ShowPlayerInfo(event, "assister", assisterid, true, true);
    }
    if (!g_bDumpJson)
        printf("\n");
}

template <typename T>
void addProperty(json_spirit::wmObject &event, const std::string &key, const T &value) {
    if (g_bDumpJson)
        event[toWide(key)] = value;
    else
        std::wcout << value << " ";
}

void ParseGameEvent(const CSVCMsg_GameEvent &msg,
                    const CSVCMsg_GameEventList::descriptor_t *pDescriptor) {
    if (pDescriptor) {
        if (!(pDescriptor->name().compare("player_footstep") == 0 && g_bSupressFootstepEvents)) {
            if (!HandlePlayerConnectDisconnectEvents(msg, pDescriptor)) {
                if (pDescriptor->name().compare("round_announce_match_start") == 0) {
                    s_bMatchStartOccured = true;
                }

                json_spirit::wmObject event;
                bool bAllowDeathReport = !g_bSupressWarmupDeaths || s_bMatchStartOccured;
                if (pDescriptor->name().compare("player_death") == 0 && g_bDumpDeaths &&
                    bAllowDeathReport) {
                    HandlePlayerDeath(event, msg, pDescriptor);
                }

                if (g_bDumpGameEvents) {
                    if (g_bDumpJson) {
                        event[L"type"] = toWide(pDescriptor->name());
                        event[L"tick"] = s_nCurrentTick;
                    } else
                        printf("%s\n{\n", pDescriptor->name().c_str());
                }
                int numKeys = msg.keys().size();
                int killer = -1, dead = -1;
                for (int i = 0; i < numKeys; i++) {
                    const CSVCMsg_GameEventList::key_t &Key = pDescriptor->keys(i);
                    const CSVCMsg_GameEvent::key_t &KeyValue = msg.keys(i);

                    if (g_bDumpGameEvents) {
                        bool bHandled = false;
                        if (Key.name().compare("userid") == 0 ||
                            Key.name().compare("attacker") == 0 ||
                            Key.name().compare("assister") == 0) {
                            if (pDescriptor->name().compare("player_death") == 0) {
                                if (Key.name().compare("userid") == 0)
                                    dead = KeyValue.val_short();
                                else if (Key.name().compare("attacker") == 0)
                                    killer = KeyValue.val_short();
                            }
                            bHandled =
                                ShowPlayerInfo(event, Key.name().c_str(), KeyValue.val_short(),
                                               g_bShowExtraPlayerInfoInGameEvents);
                        }
                        if (!bHandled) {
                            if (!g_bDumpJson)
                                printf(" %s: ", Key.name().c_str());

                            if (KeyValue.has_val_string()) {
                                addProperty(event, Key.name(), toWide(KeyValue.val_string()));
                            }
                            if (KeyValue.has_val_float()) {
                                addProperty(event, Key.name(), KeyValue.val_float());
                            }
                            if (KeyValue.has_val_long()) {
                                addProperty(event, Key.name(), KeyValue.val_long());
                            }
                            if (KeyValue.has_val_short()) {
                                addProperty(event, Key.name(), KeyValue.val_short());
                            }
                            if (KeyValue.has_val_byte()) {
                                addProperty(event, Key.name(), KeyValue.val_byte());
                            }
                            if (KeyValue.has_val_bool()) {
                                addProperty(event, Key.name(), KeyValue.val_bool());
                            }
                            if (KeyValue.has_val_uint64()) {
                                addProperty(event, Key.name(), KeyValue.val_uint64());
                            }
                            if (!g_bDumpJson)
                                printf("\n");
                        }
                    }
                }
                if (pDescriptor->name().compare("player_death") == 0) {
                    Point killerp, deadp;
                    if (killer && getPlayerPosition(dead, deadp) &&
                        getPlayerPosition(killer, killerp)) {
                        event[L"attacker_pos"] = point_to_json(killerp);
                        event[L"victim_pos"] = point_to_json(deadp);
                        addSmokes(killerp, deadp, event);
                    }
                }

                if (g_bDumpGameEvents) {
                    if (g_bDumpJson)
                        addEvent(event);
                    else
                        printf("}\n");
                }
            }
        }
    }
}

template <>
void PrintNetMessage<CSVCMsg_GameEvent, svc_GameEvent>(CDemoFileDump &Demo,
                                                       const void *parseBuffer,
                                                       int BufferSize) {
    CSVCMsg_GameEvent msg;

    if (msg.ParseFromArray(parseBuffer, BufferSize)) {
        const CSVCMsg_GameEventList::descriptor_t *pDescriptor = GetGameEventDescriptor(msg, Demo);
        if (pDescriptor) {
            ParseGameEvent(msg, pDescriptor);
        }
    }
}

template <typename T>
static void LowLevelByteSwap(T *output, const T *input) {
    T temp = *output;
    for (unsigned int i = 0; i < sizeof(T); i++) {
        ((unsigned char *)&temp)[i] = ((unsigned char *)input)[sizeof(T) - (i + 1)];
    }
    memcpy(output, &temp, sizeof(T));
}

void ParseStringTableUpdate(CBitRead &buf,
                            int entries,
                            int nMaxEntries,
                            int user_data_size,
                            int user_data_size_bits,
                            int user_data_fixed_size,
                            bool bIsUserInfo) {
    struct StringHistoryEntry {
        char string[(1 << SUBSTRING_BITS)];
    };

    int lastEntry = -1;

    // perform integer log2() to set nEntryBits
    int nTemp = nMaxEntries;
    int nEntryBits = 0;
    while (nTemp >>= 1)
        ++nEntryBits;

    bool bEncodeUsingDictionaries = buf.ReadOneBit() ? true : false;

    if (bEncodeUsingDictionaries) {
        printf("ParseStringTableUpdate: Encoded with dictionaries, unable to decode.\n");
        return;
    }

    std::vector<StringHistoryEntry> history;

    for (int i = 0; i < entries; i++) {
        int entryIndex = lastEntry + 1;

        if (!buf.ReadOneBit()) {
            entryIndex = buf.ReadUBitLong(nEntryBits);
        }

        lastEntry = entryIndex;

        if (entryIndex < 0 || entryIndex >= nMaxEntries) {
            printf("ParseStringTableUpdate: bogus string index %i\n", entryIndex);
            return;
        }

        const char *pEntry = NULL;
        char entry[1024];
        char substr[1024];
        entry[0] = 0;

        if (buf.ReadOneBit()) {
            bool substringcheck = buf.ReadOneBit() ? true : false;

            if (substringcheck) {
                int index = buf.ReadUBitLong(5);
                int bytestocopy = buf.ReadUBitLong(SUBSTRING_BITS);
                snprintf(entry, bytestocopy + 1, "%s", history[index].string);
                buf.ReadString(substr, sizeof(substr));
                snprintf(entry, sizeof(entry), "%s%s", entry, substr);
            } else {
                buf.ReadString(entry, sizeof(entry));
            }

            pEntry = entry;
        }

        // Read in the user data.
        unsigned char tempbuf[MAX_USERDATA_SIZE];
        memset(tempbuf, 0, sizeof(tempbuf));
        const void *pUserData = NULL;
        int nBytes = 0;

        if (buf.ReadOneBit()) {
            if (user_data_fixed_size) {
                // Don't need to read length, it's fixed length and the length was networked down
                // already.
                nBytes = user_data_size;
                assert(nBytes > 0);
                tempbuf[nBytes - 1] = 0; // be safe, clear last byte
                buf.ReadBits(tempbuf, user_data_size_bits);
            } else {
                nBytes = buf.ReadUBitLong(MAX_USERDATA_BITS);
                if (size_t(nBytes) > sizeof(tempbuf)) {
                    printf("ParseStringTableUpdate: user data too large (%d bytes).", nBytes);
                    return;
                }

                buf.ReadBytes(tempbuf, nBytes);
            }

            pUserData = tempbuf;
        }

        if (pEntry == NULL) {
            pEntry = ""; // avoid crash because of NULL strings
        }

        if (bIsUserInfo && pUserData != NULL) {
            const player_info_t *pUnswappedPlayerInfo = (const player_info_t *)pUserData;
            player_info_t playerInfo = *pUnswappedPlayerInfo;

            LowLevelByteSwap(&playerInfo.xuid, &pUnswappedPlayerInfo->xuid);
            LowLevelByteSwap(&playerInfo.userID, &pUnswappedPlayerInfo->userID);
            LowLevelByteSwap(&playerInfo.friendsID, &pUnswappedPlayerInfo->friendsID);

            bool bAdded = false;
            if ((unsigned int)entryIndex < s_PlayerInfos.size()) {
                if (!g_bDumpJson)
                    printf("Player %s %s (id:%d) replaced2 with Player %s %s (id:%d).\n",
                           s_PlayerInfos[entryIndex].guid, s_PlayerInfos[entryIndex].name,
                           s_PlayerInfos[entryIndex].userID, playerInfo.guid, playerInfo.name,
                           playerInfo.userID);
                s_PlayerInfos[entryIndex] = playerInfo;
            } else {
                bAdded = true;
                s_PlayerInfos.push_back(playerInfo);
            }
            addUserId(playerInfo);

            if (g_bDumpStringTables) {
                printf("player info\n{\n %s:true\n xuid:%" PRId64
                       "\n name:%s\n userID:%d\n guid:%s\n friendsID:%d\n friendsName:%s\n "
                       "fakeplayer:%d\n ishltv:%d\n filesDownloaded:%d\n}\n",
                       bAdded ? "adding" : "updating", playerInfo.xuid, playerInfo.name,
                       playerInfo.userID, playerInfo.guid, playerInfo.friendsID,
                       playerInfo.friendsName, playerInfo.fakeplayer, playerInfo.ishltv,
                       playerInfo.filesDownloaded);
            }
        } else {
            if (g_bDumpStringTables) {
                printf(" %d, %s, %d, %s \n", entryIndex, pEntry, nBytes, pUserData);
            }
        }

        if (history.size() > 31) {
            history.erase(history.begin());
        }

        StringHistoryEntry she;
        snprintf(she.string, sizeof(she.string), "%s", pEntry);
        history.push_back(she);
    }
}

template <>
void PrintNetMessage<CSVCMsg_CreateStringTable, svc_CreateStringTable>(CDemoFileDump &Demo,
                                                                       const void *parseBuffer,
                                                                       int BufferSize) {
    CSVCMsg_CreateStringTable msg;

    if (msg.ParseFromArray(parseBuffer, BufferSize)) {
        bool bIsUserInfo = !strcmp(msg.name().c_str(), "userinfo");
        if (g_bDumpStringTables) {
            printf("CreateStringTable:%s:%d:%d:%d:%d:\n", msg.name().c_str(), msg.max_entries(),
                   msg.num_entries(), msg.user_data_size(), msg.user_data_size_bits());
        }
        CBitRead data(&msg.string_data()[0], msg.string_data().size());
        ParseStringTableUpdate(data, msg.num_entries(), msg.max_entries(), msg.user_data_size(),
                               msg.user_data_size_bits(), msg.user_data_fixed_size(), bIsUserInfo);

        snprintf(s_StringTables[s_nNumStringTables].szName,
                 sizeof(s_StringTables[s_nNumStringTables].szName), "%s", msg.name().c_str());
        s_StringTables[s_nNumStringTables].nMaxEntries = msg.max_entries();
        s_nNumStringTables++;
    }
}

template <>
void PrintNetMessage<CSVCMsg_UpdateStringTable, svc_UpdateStringTable>(CDemoFileDump &Demo,
                                                                       const void *parseBuffer,
                                                                       int BufferSize) {
    CSVCMsg_UpdateStringTable msg;

    if (msg.ParseFromArray(parseBuffer, BufferSize)) {
        CBitRead data(&msg.string_data()[0], msg.string_data().size());

        if (msg.table_id() < s_nNumStringTables &&
            s_StringTables[msg.table_id()].nMaxEntries > msg.num_changed_entries()) {
            bool bIsUserInfo = !strcmp(s_StringTables[msg.table_id()].szName, "userinfo");
            if (g_bDumpStringTables) {
                printf("UpdateStringTable:%d(%s):%d:\n", msg.table_id(),
                       s_StringTables[msg.table_id()].szName, msg.num_changed_entries());
            }
            ParseStringTableUpdate(data, msg.num_changed_entries(),
                                   s_StringTables[msg.table_id()].nMaxEntries, 0, 0, 0,
                                   bIsUserInfo);
        } else {
            printf("Bad UpdateStringTable:%d:%d!\n", msg.table_id(), msg.num_changed_entries());
        }
    }
}

void RecvTable_ReadInfos(const CSVCMsg_SendTable &msg) {
    if (g_bDumpDataTables) {
        printf("%s:%d\n", msg.net_table_name().c_str(), msg.props_size());

        for (int iProp = 0; iProp < msg.props_size(); iProp++) {
            const CSVCMsg_SendTable::sendprop_t &sendProp = msg.props(iProp);

            if ((sendProp.type() == DPT_DataTable) || (sendProp.flags() & SPROP_EXCLUDE)) {
                printf("%d:%06X:%s:%s%s\n", sendProp.type(), sendProp.flags(),
                       sendProp.var_name().c_str(), sendProp.dt_name().c_str(),
                       (sendProp.flags() & SPROP_EXCLUDE) ? " exclude" : "");
            } else if (sendProp.type() == DPT_Array) {
                printf("%d:%06X:%s[%d]\n", sendProp.type(), sendProp.flags(),
                       sendProp.var_name().c_str(), sendProp.num_elements());
            } else {
                printf("%d:%06X:%s:%f,%f,%08X%s\n", sendProp.type(), sendProp.flags(),
                       sendProp.var_name().c_str(), sendProp.low_value(), sendProp.high_value(),
                       sendProp.num_bits(),
                       (sendProp.flags() & SPROP_INSIDEARRAY) ? " inside array" : "");
            }
        }
    }
}

template <>
void PrintNetMessage<CSVCMsg_SendTable, svc_SendTable>(CDemoFileDump &Demo,
                                                       const void *parseBuffer,
                                                       int BufferSize) {
    CSVCMsg_SendTable msg;

    if (msg.ParseFromArray(parseBuffer, BufferSize)) {
        RecvTable_ReadInfos(msg);
    }
}

CSVCMsg_SendTable *GetTableByClassID(uint32 nClassID) {
    for (uint32 i = 0; i < s_ServerClasses.size(); i++) {
        if (s_ServerClasses[i].nClassID == nClassID) {
            return &(s_DataTables[s_ServerClasses[i].nDataTable]);
        }
    }
    return NULL;
}

CSVCMsg_SendTable *GetTableByName(const char *pName) {
    for (unsigned int i = 0; i < s_DataTables.size(); i++) {
        if (s_DataTables[i].net_table_name().compare(pName) == 0) {
            return &(s_DataTables[i]);
        }
    }
    return NULL;
}

FlattenedPropEntry *GetSendPropByIndex(uint32 uClass, uint32 uIndex) {
    if (uIndex < s_ServerClasses[uClass].flattenedProps.size()) {
        return &s_ServerClasses[uClass].flattenedProps[uIndex];
    }
    return NULL;
}

bool IsPropExcluded(CSVCMsg_SendTable *pTable, const CSVCMsg_SendTable::sendprop_t &checkSendProp) {
    for (unsigned int i = 0; i < s_currentExcludes.size(); i++) {
        if (pTable->net_table_name().compare(s_currentExcludes[i].m_pDTName) == 0 &&
            checkSendProp.var_name().compare(s_currentExcludes[i].m_pVarName) == 0) {
            return true;
        }
    }
    return false;
}

void GatherExcludes(CSVCMsg_SendTable *pTable) {
    for (int iProp = 0; iProp < pTable->props_size(); iProp++) {
        const CSVCMsg_SendTable::sendprop_t &sendProp = pTable->props(iProp);
        if (sendProp.flags() & SPROP_EXCLUDE) {
            s_currentExcludes.push_back(ExcludeEntry(sendProp.var_name().c_str(),
                                                     sendProp.dt_name().c_str(),
                                                     pTable->net_table_name().c_str()));
        }

        if (sendProp.type() == DPT_DataTable) {
            CSVCMsg_SendTable *pSubTable = GetTableByName(sendProp.dt_name().c_str());
            if (pSubTable != NULL) {
                GatherExcludes(pSubTable);
            }
        }
    }
}

void GatherProps(CSVCMsg_SendTable *pTable, int nServerClass);

void GatherProps_IterateProps(CSVCMsg_SendTable *pTable,
                              int nServerClass,
                              std::vector<FlattenedPropEntry> &flattenedProps) {
    for (int iProp = 0; iProp < pTable->props_size(); iProp++) {
        const CSVCMsg_SendTable::sendprop_t &sendProp = pTable->props(iProp);

        if ((sendProp.flags() & SPROP_INSIDEARRAY) || (sendProp.flags() & SPROP_EXCLUDE) ||
            IsPropExcluded(pTable, sendProp)) {
            continue;
        }

        if (sendProp.type() == DPT_DataTable) {
            CSVCMsg_SendTable *pSubTable = GetTableByName(sendProp.dt_name().c_str());
            if (pSubTable != NULL) {
                if (sendProp.flags() & SPROP_COLLAPSIBLE) {
                    GatherProps_IterateProps(pSubTable, nServerClass, flattenedProps);
                } else {
                    GatherProps(pSubTable, nServerClass);
                }
            }
        } else {
            if (sendProp.type() == DPT_Array) {
                flattenedProps.push_back(
                    FlattenedPropEntry(&sendProp, &(pTable->props(iProp - 1))));
            } else {
                flattenedProps.push_back(FlattenedPropEntry(&sendProp, NULL));
            }
        }
    }
}

void GatherProps(CSVCMsg_SendTable *pTable, int nServerClass) {
    std::vector<FlattenedPropEntry> tempFlattenedProps;
    GatherProps_IterateProps(pTable, nServerClass, tempFlattenedProps);

    std::vector<FlattenedPropEntry> &flattenedProps = s_ServerClasses[nServerClass].flattenedProps;
    for (uint32 i = 0; i < tempFlattenedProps.size(); i++) {
        flattenedProps.push_back(tempFlattenedProps[i]);
    }
}

void FlattenDataTable(int nServerClass) {
    CSVCMsg_SendTable *pTable = &s_DataTables[s_ServerClasses[nServerClass].nDataTable];

    s_currentExcludes.clear();
    GatherExcludes(pTable);

    GatherProps(pTable, nServerClass);

    std::vector<FlattenedPropEntry> &flattenedProps = s_ServerClasses[nServerClass].flattenedProps;

    // get priorities
    std::vector<uint32> priorities;
    priorities.push_back(64);
    for (unsigned int i = 0; i < flattenedProps.size(); i++) {
        uint32 priority = flattenedProps[i].m_prop->priority();

        bool bFound = false;
        for (uint32 j = 0; j < priorities.size(); j++) {
            if (priorities[j] == priority) {
                bFound = true;
                break;
            }
        }

        if (!bFound) {
            priorities.push_back(priority);
        }
    }

    std::sort(priorities.begin(), priorities.end());

    // sort flattenedProps by priority
    uint32 start = 0;
    for (uint32 priority_index = 0; priority_index < priorities.size(); ++priority_index) {
        uint32 priority = priorities[priority_index];

        while (true) {
            uint32 currentProp = start;
            while (currentProp < flattenedProps.size()) {
                const CSVCMsg_SendTable::sendprop_t *prop = flattenedProps[currentProp].m_prop;

                if (prop->priority() == priority ||
                    (priority == 64 && (SPROP_CHANGES_OFTEN & prop->flags()))) {
                    if (start != currentProp) {
                        FlattenedPropEntry temp = flattenedProps[start];
                        flattenedProps[start] = flattenedProps[currentProp];
                        flattenedProps[currentProp] = temp;
                    }
                    start++;
                    break;
                }
                currentProp++;
            }

            if (currentProp == flattenedProps.size())
                break;
        }
    }

    if (nServerClass == serverClassesIds[DT_CSPlayer]) {
        for (size_t i = 0; i < flattenedProps.size(); ++i)
            if (flattenedProps[i].m_prop->var_name() == "m_vecOrigin")
                playerCoordIndex = i;
            else if (flattenedProps[i].m_prop->var_name() == "m_vecOrigin[2]")
                playerCoordIndex2 = i;
    }
}

int ReadFieldIndex(CBitRead &entityBitBuffer, int lastIndex, bool bNewWay) {
    if (bNewWay) {
        if (entityBitBuffer.ReadOneBit()) {
            return lastIndex + 1;
        }
    }

    int ret = 0;
    if (bNewWay && entityBitBuffer.ReadOneBit()) {
        ret = entityBitBuffer.ReadUBitLong(3); // read 3 bits
    } else {
        ret = entityBitBuffer.ReadUBitLong(7); // read 7 bits
        switch (ret & (32 | 64)) {
        case 32:
            ret = (ret & ~96) | (entityBitBuffer.ReadUBitLong(2) << 5);
            assert(ret >= 32);
            break;
        case 64:
            ret = (ret & ~96) | (entityBitBuffer.ReadUBitLong(4) << 5);
            assert(ret >= 128);
            break;
        case 96:
            ret = (ret & ~96) | (entityBitBuffer.ReadUBitLong(7) << 5);
            assert(ret >= 512);
            break;
        }
    }

    if (ret == 0xFFF) // end marker is 4095 for cs:go
    {
        return -1;
    }

    return lastIndex + 1 + ret;
}

bool updateTeamScore(uint32 entity_id, int val) {
    int teamno = id2teamno[entity_id];
    Team &team = teams[teamno];
    // Check for weird score update
    if (val < score_snapshot.first && val < score_snapshot.second)
        return false;
    // No change really
    if (team.total_score == val)
        return false;
    team.total_score = val;
    return true;
}

void handleTeamProp(uint32 entity_id, const std::string &key, const Prop_t &value) {
    if (key == "m_iTeamNum") {
        if (value.m_value.m_int == 2 || value.m_value.m_int == 3)
            id2teamno[entity_id] = value.m_value.m_int;
        return;
    }
    if (!id2teamno.count(entity_id))
        return;

    if (key != "m_scoreTotal")
        return;
    bool changed = updateTeamScore(entity_id, value.m_value.m_int);
    if (changed) {
        if (g_bOnlyHsBoxEvents)
            events.push_back(json_spirit::wmObject(
                {{L"type", L"score_changed"},
                 {L"tick", s_nCurrentTick},
                 {L"score", json_spirit::wmArray({teams[2].total_score, teams[3].total_score})}}));
    }
}

bool ReadNewEntity(CBitRead &entityBitBuffer, EntityEntry *pEntity) {
    bool bNewWay = (entityBitBuffer.ReadOneBit() == 1); // 0 = old way, 1 = new way

    static std::vector<int> fieldIndices(5000);
    fieldIndices.clear();

    int steps = 0;
    int index = -1;
    do {
        index = ReadFieldIndex(entityBitBuffer, index, bNewWay);
        if (index != -1) {
            fieldIndices.push_back(index);
        }
        // Sometimes this loop never ends: demo is probably corrupted
        // Hoping valid packets never get to 20000 indices
        if (++steps > 20000) {
            fprintf(stderr, "Corrupted demo\n");
            exit(1);
        }
    } while (index != -1);

    CSVCMsg_SendTable *pTable = GetTableByClassID(pEntity->m_uClass);
    if (g_bDumpPacketEntities) {
        printf("Table: %s\n", pTable->net_table_name().c_str());
    }

    for (unsigned int i = 0; i < fieldIndices.size(); i++) {
        FlattenedPropEntry *pSendProp = GetSendPropByIndex(pEntity->m_uClass, fieldIndices[i]);
        if (pSendProp) {
            // for -hsbox update only the entities and properties we need
            if (g_bOnlyHsBoxEvents) {
                bool team = pEntity->m_uClass == serverClassesIds[DT_CSTeam];
                bool gamerules = pEntity->m_uClass == serverClassesIds[DT_CSGameRulesProxy];
                bool player = pEntity->m_uClass == serverClassesIds[DT_CSPlayer];
                if ((team || gamerules || (player && (playerCoordIndex == fieldIndices[i] ||
                                                      playerCoordIndex2 == fieldIndices[i])))) {
                    Prop_t *pProp = DecodeProp(entityBitBuffer, pSendProp, pEntity->m_uClass,
                                               fieldIndices[i], !g_bDumpPacketEntities);
                    pEntity->AddOrUpdateProp(pSendProp, pProp);
                    if (team) {
                        handleTeamProp(pEntity->m_uSerialNum, pSendProp->m_prop->var_name(),
                                       *pProp);
                    } else if (gamerules && pSendProp->m_prop->var_name() == "m_bGameRestart" &&
                               pProp->m_value.m_int) {
                        addEvent({{L"type", L"game_restart"}, {L"tick", s_nCurrentTick}});
                    }
                } else
                    DecodePropFake(entityBitBuffer, pSendProp, pEntity->m_uClass, fieldIndices[i],
                                   !g_bDumpPacketEntities);
            } else {
                Prop_t *pProp = DecodeProp(entityBitBuffer, pSendProp, pEntity->m_uClass,
                                           fieldIndices[i], !g_bDumpPacketEntities);
                pEntity->AddOrUpdateProp(pSendProp, pProp);
            }
        } else {
            return false;
        }
    }

    return true;
}

EntityEntry *FindEntity(int nEntity) {
    for (std::vector<EntityEntry *>::iterator i = s_Entities.begin(); i != s_Entities.end(); i++) {
        if ((*i)->m_nEntity == nEntity) {
            return *i;
        }
    }

    return NULL;
}

EntityEntry *AddEntity(int nEntity, uint32 uClass, uint32 uSerialNum) {
    // if entity already exists, then replace it, else add it
    EntityEntry *pEntity = FindEntity(nEntity);
    if (pEntity) {
        pEntity->m_uClass = uClass;
        pEntity->m_uSerialNum = uSerialNum;
    } else {
        pEntity = new EntityEntry(nEntity, uClass, uSerialNum);
        s_Entities.push_back(pEntity);
    }

    return pEntity;
}

void RemoveEntity(int nEntity) {
    for (std::vector<EntityEntry *>::iterator i = s_Entities.begin(); i != s_Entities.end(); i++) {
        EntityEntry *pEntity = *i;
        if (pEntity->m_nEntity == nEntity) {
            s_Entities.erase(i);
            delete pEntity;
            break;
        }
    }
}

template <>
void PrintNetMessage<CSVCMsg_PacketEntities, svc_PacketEntities>(CDemoFileDump &Demo,
                                                                 const void *parseBuffer,
                                                                 int BufferSize) {
    CSVCMsg_PacketEntities msg;

    if (msg.ParseFromArray(parseBuffer, BufferSize)) {
        CBitRead entityBitBuffer(&msg.entity_data()[0], msg.entity_data().size());
        bool bAsDelta = msg.is_delta();
        int nHeaderCount = msg.updated_entries();
        int nHeaderBase = -1;
        int nNewEntity = -1;
        int UpdateFlags = 0;

        UpdateType updateType = PreserveEnt;

        while (updateType < Finished) {
            nHeaderCount--;

            bool bIsEntity = (nHeaderCount >= 0) ? true : false;

            if (bIsEntity) {
                UpdateFlags = FHDR_ZERO;

                nNewEntity = nHeaderBase + 1 + entityBitBuffer.ReadUBitVar();
                nHeaderBase = nNewEntity;

                // leave pvs flag
                if (entityBitBuffer.ReadOneBit() == 0) {
                    // enter pvs flag
                    if (entityBitBuffer.ReadOneBit() != 0) {
                        UpdateFlags |= FHDR_ENTERPVS;
                    }
                } else {
                    UpdateFlags |= FHDR_LEAVEPVS;

                    // Force delete flag
                    if (entityBitBuffer.ReadOneBit() != 0) {
                        UpdateFlags |= FHDR_DELETE;
                    }
                }
            }

            for (updateType = PreserveEnt; updateType == PreserveEnt;) {
                // Figure out what kind of an update this is.
                if (!bIsEntity || nNewEntity > ENTITY_SENTINEL) {
                    updateType = Finished;
                } else {
                    if (UpdateFlags & FHDR_ENTERPVS) {
                        updateType = EnterPVS;
                    } else if (UpdateFlags & FHDR_LEAVEPVS) {
                        updateType = LeavePVS;
                    } else {
                        updateType = DeltaEnt;
                    }
                }

                switch (updateType) {
                case EnterPVS: {
                    uint32 uClass = entityBitBuffer.ReadUBitLong(s_nServerClassBits);
                    uint32 uSerialNum =
                        entityBitBuffer.ReadUBitLong(NUM_NETWORKED_EHANDLE_SERIAL_NUMBER_BITS);
                    if (g_bDumpPacketEntities) {
                        printf("Entity Enters PVS: id:%d, class:%d, serial:%d\n", nNewEntity,
                               uClass, uSerialNum);
                    }
                    EntityEntry *pEntity = AddEntity(nNewEntity, uClass, uSerialNum);
                    if (!ReadNewEntity(entityBitBuffer, pEntity)) {
                        fprintf(stderr, "*****Error reading entity! Bailing on this PacketEntities!\n");
                        return;
                    }
                } break;

                case LeavePVS: {
                    if (!bAsDelta) // Should never happen on a full update.
                    {
                        printf("WARNING: LeavePVS on full update");
                        updateType = Failed; // break out
                        assert(0);
                    } else {
                        if (g_bDumpPacketEntities) {
                            if (UpdateFlags & FHDR_DELETE) {
                                printf("Entity leaves PVS and is deleted: id:%d\n", nNewEntity);
                            } else {
                                printf("Entity leaves PVS: id:%d\n", nNewEntity);
                            }
                        }
                        RemoveEntity(nNewEntity);
                    }
                } break;

                case DeltaEnt: {
                    EntityEntry *pEntity = FindEntity(nNewEntity);
                    if (pEntity) {
                        if (g_bDumpPacketEntities) {
                            printf("Entity Delta update: id:%d, class:%d, serial:%d\n",
                                   pEntity->m_nEntity, pEntity->m_uClass, pEntity->m_uSerialNum);
                        }
                        if (!ReadNewEntity(entityBitBuffer, pEntity)) {
                            fprintf(stderr, "*****Error reading entity! Bailing on this PacketEntities!\n");
                            return;
                        }
                    } else {
                        assert(0);
                    }
                } break;

                case PreserveEnt: {
                    if (!bAsDelta) // Should never happen on a full update.
                    {
                        printf("WARNING: PreserveEnt on full update");
                        updateType = Failed; // break out
                        assert(0);
                    } else {
                        if (nNewEntity >= MAX_EDICTS) {
                            printf("PreserveEnt: nNewEntity == MAX_EDICTS");
                            assert(0);
                        } else {
                            if (g_bDumpPacketEntities) {
                                printf("PreserveEnt: id:%d\n", nNewEntity);
                            }
                        }
                    }
                } break;

                default:
                    break;
                }
            }
        }
    }
}

static std::string GetNetMsgName(int Cmd) {
    if (NET_Messages_IsValid(Cmd)) {
        return NET_Messages_Name((NET_Messages)Cmd);
    } else if (SVC_Messages_IsValid(Cmd)) {
        return SVC_Messages_Name((SVC_Messages)Cmd);
    }

    return "NETMSG_???";
}

void CDemoFileDump::DumpDemoPacket(CBitRead &buf, int length) {
    while (buf.GetNumBytesRead() < length) {
        int Cmd = buf.ReadVarInt32();
        int Size = buf.ReadVarInt32();

        if (buf.GetNumBytesRead() + Size > length) {
            const std::string &strName = GetNetMsgName(Cmd);

            fatal_errorf("DumpDemoPacket()::failed parsing packet. Cmd:%d '%s' \n", Cmd,
                         strName.c_str());
        }

        switch (Cmd) {
#define HANDLE_NetMsg(_x)                                                                          \
    case net_##_x:                                                                                 \
        PrintNetMessage<CNETMsg_##_x, net_##_x>(                                                   \
            *this, buf.GetBasePointer() + buf.GetNumBytesRead(), Size);                            \
        break
#define HANDLE_SvcMsg(_x)                                                                          \
    case svc_##_x:                                                                                 \
        PrintNetMessage<CSVCMsg_##_x, svc_##_x>(                                                   \
            *this, buf.GetBasePointer() + buf.GetNumBytesRead(), Size);                            \
        break

        default:
            // unknown net message
            break;

            HANDLE_NetMsg(NOP);               // 0
            HANDLE_NetMsg(Disconnect);        // 1
            HANDLE_NetMsg(File);              // 2
            HANDLE_NetMsg(Tick);              // 4
            HANDLE_NetMsg(StringCmd);         // 5
            HANDLE_NetMsg(SetConVar);         // 6
            HANDLE_NetMsg(SignonState);       // 7
            HANDLE_SvcMsg(ServerInfo);        // 8
            HANDLE_SvcMsg(SendTable);         // 9
            HANDLE_SvcMsg(ClassInfo);         // 10
            HANDLE_SvcMsg(SetPause);          // 11
            HANDLE_SvcMsg(CreateStringTable); // 12
            HANDLE_SvcMsg(UpdateStringTable); // 13
            HANDLE_SvcMsg(VoiceInit);         // 14
            HANDLE_SvcMsg(VoiceData);         // 15
            HANDLE_SvcMsg(Print);             // 16
            HANDLE_SvcMsg(Sounds);            // 17
            HANDLE_SvcMsg(SetView);           // 18
            HANDLE_SvcMsg(FixAngle);          // 19
            HANDLE_SvcMsg(CrosshairAngle);    // 20
            HANDLE_SvcMsg(BSPDecal);          // 21
            HANDLE_SvcMsg(UserMessage);       // 23
            HANDLE_SvcMsg(GameEvent);         // 25
            HANDLE_SvcMsg(PacketEntities);    // 26
            HANDLE_SvcMsg(TempEntities);      // 27
            HANDLE_SvcMsg(Prefetch);          // 28
            HANDLE_SvcMsg(Menu);              // 29
            HANDLE_SvcMsg(GameEventList);     // 30
            HANDLE_SvcMsg(GetCvarValue);      // 31

#undef HANDLE_SvcMsg
#undef HANDLE_NetMsg
        }

        buf.SeekRelative(Size * 8);
    }
}

void CDemoFileDump::HandleDemoPacket() {
    democmdinfo_t info;
    int dummy;
    char data[NET_MAX_PAYLOAD];

    m_demofile.ReadCmdInfo(info);
    m_demofile.ReadSequenceInfo(dummy, dummy);

    CBitRead buf(data, NET_MAX_PAYLOAD);
    int length = m_demofile.ReadRawData((char *)buf.GetBasePointer(), buf.GetNumBytesLeft());
    buf.Seek(0);
    DumpDemoPacket(buf, length);
}

bool ReadFromBuffer(CBitRead &buffer, void **pBuffer, int &size) {
    size = buffer.ReadVarInt32();
    if (size < 0 || size > NET_MAX_PAYLOAD) {
        return false;
    }

    // Check its valid
    if (size > buffer.GetNumBytesLeft()) {
        return false;
    }

    *pBuffer = malloc(size);

    // If the read buffer is byte aligned, we can parse right out of it
    if ((buffer.GetNumBitsRead() % 8) == 0) {
        memcpy(*pBuffer, buffer.GetBasePointer() + buffer.GetNumBytesRead(), size);
        buffer.SeekRelative(size * 8);
        return true;
    }

    // otherwise we have to ReadBytes() it out
    if (!buffer.ReadBytes(*pBuffer, size)) {
        return false;
    }

    return true;
}

bool ParseDataTable(CBitRead &buf) {
    CSVCMsg_SendTable msg;
    while (1) {
        buf.ReadVarInt32();

        void *pBuffer = NULL;
        int size = 0;
        if (!ReadFromBuffer(buf, &pBuffer, size)) {
            printf("ParseDataTable: ReadFromBuffer failed.\n");
            return false;
        }
        msg.ParseFromArray(pBuffer, size);
        free(pBuffer);

        if (msg.is_end())
            break;

        RecvTable_ReadInfos(msg);

        s_DataTables.push_back(msg);
    }

    short nServerClasses = buf.ReadShort();
    assert(nServerClasses);
    for (int i = 0; i < nServerClasses; i++) {
        ServerClass_t entry;
        entry.nClassID = buf.ReadShort();
        if (entry.nClassID >= nServerClasses) {
            printf("ParseDataTable: invalid class index (%d).\n", entry.nClassID);
            return false;
        }

        int nChars;
        buf.ReadString(entry.strName, sizeof(entry.strName), false, &nChars);
        buf.ReadString(entry.strDTName, sizeof(entry.strDTName), false, &nChars);

        // find the data table by name
        entry.nDataTable = -1;
        for (unsigned int j = 0; j < s_DataTables.size(); j++) {
            if (strcmp(entry.strDTName, s_DataTables[j].net_table_name().c_str()) == 0) {
                entry.nDataTable = j;
                break;
            }
        }

        if (g_bDumpDataTables) {
            printf("class:%d:%s:%s(%d)\n", entry.nClassID, entry.strName, entry.strDTName,
                   entry.nDataTable);
        }
        s_ServerClasses.push_back(entry);
        if (g_bOnlyHsBoxEvents) {
            if (!strcmp(entry.strDTName, "DT_CSPlayer"))
                serverClassesIds[DT_CSPlayer] = entry.nClassID;
            else if (!strcmp(entry.strDTName, "DT_CSTeam"))
                serverClassesIds[DT_CSTeam] = entry.nClassID;
            else if (!strcmp(entry.strDTName, "DT_CSGameRulesProxy"))
                serverClassesIds[DT_CSGameRulesProxy] = entry.nClassID;
        }
    }

    if (g_bDumpDataTables) {
        printf("Flattening data tables...");
    }
    for (int i = 0; i < nServerClasses; i++) {
        FlattenDataTable(i);
    }
    if (g_bDumpDataTables) {
        printf("Done.\n");
    }

    // perform integer log2() to set s_nServerClassBits
    int nTemp = nServerClasses;
    s_nServerClassBits = 0;
    while (nTemp >>= 1)
        ++s_nServerClassBits;

    s_nServerClassBits++;

    return true;
}

bool DumpStringTable(CBitRead &buf, bool bIsUserInfo) {
    int numstrings = buf.ReadWord();
    if (g_bDumpStringTables) {
        printf("%d\n", numstrings);
    }

    if (bIsUserInfo) {
        if (g_bDumpStringTables) {
            printf("Clearing player info array.\n");
        }
        s_PlayerInfos.clear();
    }

    for (int i = 0; i < numstrings; i++) {
        char stringname[4096];

        buf.ReadString(stringname, sizeof(stringname));

        assert(strlen(stringname) < 100);

        if (buf.ReadOneBit() == 1) {
            int userDataSize = (int)buf.ReadWord();
            assert(userDataSize > 0);
            unsigned char *data = new unsigned char[userDataSize + 4];
            assert(data);

            buf.ReadBytes(data, userDataSize);

            if (bIsUserInfo && data != NULL) {
                const player_info_t *pUnswappedPlayerInfo = (const player_info_t *)data;
                player_info_t playerInfo = *pUnswappedPlayerInfo;

                LowLevelByteSwap(&playerInfo.xuid, &pUnswappedPlayerInfo->xuid);
                LowLevelByteSwap(&playerInfo.userID, &pUnswappedPlayerInfo->userID);
                LowLevelByteSwap(&playerInfo.friendsID, &pUnswappedPlayerInfo->friendsID);

                if (g_bDumpStringTables) {
                    printf("adding:player info:\n xuid:%" PRId64
                           "\n name:%s\n userID:%d\n guid:%s\n friendsID:%d\n friendsName:%s\n "
                           "fakeplayer:%d\n ishltv:%d\n filesDownloaded:%d\n",
                           playerInfo.xuid, playerInfo.name, playerInfo.userID, playerInfo.guid,
                           playerInfo.friendsID, playerInfo.friendsName, playerInfo.fakeplayer,
                           playerInfo.ishltv, playerInfo.filesDownloaded);
                }

                s_PlayerInfos.push_back(playerInfo);
                addUserId(playerInfo);
            } else {
                if (g_bDumpStringTables) {
                    printf(" %d, %s, userdata[%d] \n", i, stringname, userDataSize);
                }
            }

            delete[] data;

            assert(buf.GetNumBytesLeft() > 10);
        } else {
            if (g_bDumpStringTables) {
                printf(" %d, %s \n", i, stringname);
            }
        }
    }

    // Client side stuff
    if (buf.ReadOneBit() == 1) {
        int numstrings = buf.ReadWord();
        for (int i = 0; i < numstrings; i++) {
            char stringname[4096];

            buf.ReadString(stringname, sizeof(stringname));

            if (buf.ReadOneBit() == 1) {
                int userDataSize = (int)buf.ReadWord();
                assert(userDataSize > 0);
                unsigned char *data = new unsigned char[userDataSize + 4];
                assert(data);

                buf.ReadBytes(data, userDataSize);

                if (i >= 2) {
                    if (g_bDumpStringTables) {
                        printf(" %d, %s, userdata[%d] \n", i, stringname, userDataSize);
                    }
                }

                delete[] data;

            } else {
                if (i >= 2) {
                    if (g_bDumpStringTables) {
                        printf(" %d, %s \n", i, stringname);
                    }
                }
            }
        }
    }

    return true;
}

bool DumpStringTables(CBitRead &buf) {
    int numTables = buf.ReadByte();
    for (int i = 0; i < numTables; i++) {
        char tablename[256];
        buf.ReadString(tablename, sizeof(tablename));

        if (g_bDumpStringTables) {
            printf("ReadStringTable:%s:", tablename);
        }

        bool bIsUserInfo = !strcmp(tablename, "userinfo");
        if (!DumpStringTable(buf, bIsUserInfo)) {
            printf("Error reading string table %s\n", tablename);
        }
    }

    return true;
}

void CDemoFileDump::DoDump() {
    s_bMatchStartOccured = false;

    bool demofinished = false;
    while (!demofinished) {
        int tick = 0;
        unsigned char cmd;
        unsigned char playerSlot;
        m_demofile.ReadCmdHeader(cmd, tick, playerSlot);
        s_nCurrentTick = tick;
        // COMMAND HANDLERS
        switch (cmd) {
        case dem_synctick:
            break;

        case dem_stop: {
            demofinished = true;
        } break;

        case dem_consolecmd: {
            m_demofile.ReadRawData(NULL, 0);
        } break;

        case dem_datatables: {
            char *data = (char *)malloc(DEMO_RECORD_BUFFER_SIZE);
            CBitRead buf(data, DEMO_RECORD_BUFFER_SIZE);
            m_demofile.ReadRawData((char *)buf.GetBasePointer(), buf.GetNumBytesLeft());
            buf.Seek(0);
            if (!ParseDataTable(buf)) {
                printf("Error parsing data tables. \n");
            }
            free(data);
        } break;

        case dem_stringtables: {
            char *data = (char *)malloc(DEMO_RECORD_BUFFER_SIZE);
            CBitRead buf(data, DEMO_RECORD_BUFFER_SIZE);
            m_demofile.ReadRawData((char *)buf.GetBasePointer(), buf.GetNumBytesLeft());
            buf.Seek(0);
            if (!DumpStringTables(buf)) {
                printf("Error parsing string tables. \n");
            }
            free(data);
        } break;

        case dem_usercmd: {
            int dummy;
            m_demofile.ReadUserCmd(NULL, dummy);
        } break;

        case dem_signon:
        case dem_packet: {
            HandleDemoPacket();
        } break;

        default:
            break;
        }
    }
    if (g_bDumpJson) {
        match[L"events"] = events;
        match[L"servername"] = toWide(m_demofile.m_DemoHeader.servername);
        match[L"player_names"] = player_names;
        json_spirit::wmArray gotv_bots;
        for (const auto &kv : userid_info)
            if (kv.second.ishltv)
                gotv_bots.push_back(toWide(kv.second.name));
        match[L"gotv_bots"] = gotv_bots;
        if (!mm_rank_update.empty())
            match[L"mm_rank_update"] = mm_rank_update;

        json_spirit::wmObject uids;
        for (const auto &kv: player_slot)
            uids[std::to_wstring(kv.first)] = kv.second;
        match[L"player_slots"] = uids;

        int options = 0;
        if (g_bPrettyJson)
            options = json_spirit::pretty_print | json_spirit::remove_trailing_zeros;
#if defined(_WIN32) || defined(_WIN64)
        _setmode(_fileno(stdout), _O_U8TEXT);
#endif
        json_spirit::write(match, std::wcout, options);
    }
}
