/*
 * This file is part of the Continued-MaNGOS Project
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

#include "Common.h"

#include "Transports.h"
#include "MapManager.h"
#include "ObjectMgr.h"
#include "ObjectGuid.h"
#include "Path.h"

#include "WorldPacket.h"
#include "DBCStores.h"
#include "ScriptMgr.h"

Transport::Transport() : GameObject()
{
    m_updateFlag = (UPDATEFLAG_TRANSPORT | UPDATEFLAG_HIGHGUID | UPDATEFLAG_HAS_POSITION | UPDATEFLAG_ROTATION);
    m_transportBase = new TransportBase(this);
}

Transport::~Transport()
{
    delete m_transportBase;
}

bool Transport::Create(uint32 guidlow, uint32 mapid, float x, float y, float z, float ang, uint8 animprogress, uint16 dynamicHighValue)
{
    Relocate(x, y, z, ang);
    // instance id and phaseMask isn't set to values different from std.

    if (!IsPositionValid())
    {
        sLog.outError("Transport (GUID: %u) not created. Suggested coordinates isn't valid (X: %f Y: %f)",
                      guidlow, x, y);
        return false;
    }

    Object::_Create(guidlow, 0, HIGHGUID_MO_TRANSPORT);

    GameObjectInfo const* goinfo = ObjectMgr::GetGameObjectInfo(guidlow);

    if (!goinfo)
    {
        sLog.outErrorDb("Transport not created: entry in `gameobject_template` not found, guidlow: %u map: %u  (X: %f Y: %f Z: %f) ang: %f", guidlow, mapid, x, y, z, ang);
        return false;
    }

    m_goInfo = goinfo;

    SetObjectScale(goinfo->size);

    SetUInt32Value(GAMEOBJECT_FACTION, goinfo->faction);
    // SetUInt32Value(GAMEOBJECT_FLAGS, goinfo->flags);
    SetUInt32Value(GAMEOBJECT_FLAGS, (GO_FLAG_TRANSPORT | GO_FLAG_NODESPAWN));
    SetUInt32Value(GAMEOBJECT_LEVEL, m_period);
    SetEntry(goinfo->id);

    //SetDisplayId(goinfo->displayId);
    // Use SetDisplayId only if we have the GO assigned to a proper map!
    SetUInt32Value(GAMEOBJECT_DISPLAYID, goinfo->displayId);
    m_displayInfo = sGameObjectDisplayInfoStore.LookupEntry(goinfo->displayId);

    SetGoState(GO_STATE_READY);
    SetGoType(GameobjectTypes(goinfo->type));
    SetGoArtKit(0);
    SetGoAnimProgress(animprogress);

    // low part always 0, dynamicHighValue is some kind of progression (not implemented)
    SetUInt16Value(GAMEOBJECT_DYNAMIC, 0, 0);
    SetUInt16Value(GAMEOBJECT_DYNAMIC, 1, dynamicHighValue);

    SetName(goinfo->name);

    return true;
}

struct keyFrame
{
    explicit keyFrame(TaxiPathNodeEntry const& _node) : node(&_node),
        distSinceStop(-1.0f), distUntilStop(-1.0f), distFromPrev(-1.0f), tFrom(0.0f), tTo(0.0f)
    {
    }

    TaxiPathNodeEntry const* node;

    float distSinceStop;
    float distUntilStop;
    float distFromPrev;
    float tFrom, tTo;
};

bool Transport::GenerateWaypoints(uint32 pathid, std::set<uint32>& mapids)
{
    if (pathid >= sTaxiPathNodesByPath.size())
        return false;

    TaxiPathNodeList const& path = sTaxiPathNodesByPath[pathid];

    std::vector<keyFrame> keyFrames;
    int mapChange = 0;
    mapids.clear();
    for (size_t i = 1; i < path.size() - 1; ++i)
    {
        if (mapChange == 0)
        {
            TaxiPathNodeEntry const& node_i = path[i];
            if (node_i.mapid == path[i + 1].mapid)
            {
                keyFrame k(node_i);
                keyFrames.push_back(k);
                mapids.insert(k.node->mapid);
            }
            else
            {
                mapChange = 1;
            }
        }
        else
        {
            --mapChange;
        }
    }

    int lastStop = -1;
    int firstStop = -1;

    // first cell is arrived at by teleportation :S
    keyFrames[0].distFromPrev = 0;
    if (keyFrames[0].node->actionFlag == 2)
    {
        lastStop = 0;
    }

    // find the rest of the distances between key points
    for (size_t i = 1; i < keyFrames.size(); ++i)
    {
        if ((keyFrames[i].node->actionFlag == 1) || (keyFrames[i].node->mapid != keyFrames[i - 1].node->mapid))
        {
            keyFrames[i].distFromPrev = 0;
        }
        else
        {
            keyFrames[i].distFromPrev =
                sqrt(pow(keyFrames[i].node->x - keyFrames[i - 1].node->x, 2) +
                     pow(keyFrames[i].node->y - keyFrames[i - 1].node->y, 2) +
                     pow(keyFrames[i].node->z - keyFrames[i - 1].node->z, 2));
        }
        if (keyFrames[i].node->actionFlag == 2)
        {
            // remember first stop frame
            if (firstStop == -1)
                firstStop = i;
            lastStop = i;
        }
    }

    float tmpDist = 0;
    for (size_t i = 0; i < keyFrames.size(); ++i)
    {
        int j = (i + lastStop) % keyFrames.size();
        if (keyFrames[j].node->actionFlag == 2)
            tmpDist = 0;
        else
            tmpDist += keyFrames[j].distFromPrev;
        keyFrames[j].distSinceStop = tmpDist;
    }

    for (int i = int(keyFrames.size()) - 1; i >= 0; --i)
    {
        int j = (i + (firstStop + 1)) % keyFrames.size();
        tmpDist += keyFrames[(j + 1) % keyFrames.size()].distFromPrev;
        keyFrames[j].distUntilStop = tmpDist;
        if (keyFrames[j].node->actionFlag == 2)
            tmpDist = 0;
    }

    for (size_t i = 0; i < keyFrames.size(); ++i)
    {
        if (keyFrames[i].distSinceStop < (30 * 30 * 0.5f))
            keyFrames[i].tFrom = sqrt(2 * keyFrames[i].distSinceStop);
        else
            keyFrames[i].tFrom = ((keyFrames[i].distSinceStop - (30 * 30 * 0.5f)) / 30) + 30;

        if (keyFrames[i].distUntilStop < (30 * 30 * 0.5f))
            keyFrames[i].tTo = sqrt(2 * keyFrames[i].distUntilStop);
        else
            keyFrames[i].tTo = ((keyFrames[i].distUntilStop - (30 * 30 * 0.5f)) / 30) + 30;

        keyFrames[i].tFrom *= 1000;
        keyFrames[i].tTo *= 1000;
    }

    //    for (int i = 0; i < keyFrames.size(); ++i) {
    //        sLog.outString("%f, %f, %f, %f, %f, %f, %f", keyFrames[i].x, keyFrames[i].y, keyFrames[i].distUntilStop, keyFrames[i].distSinceStop, keyFrames[i].distFromPrev, keyFrames[i].tFrom, keyFrames[i].tTo);
    //    }

    // Now we're completely set up; we can move along the length of each waypoint at 100 ms intervals
    // speed = max(30, t) (remember x = 0.5s^2, and when accelerating, a = 1 unit/s^2
    int t = 0;
    bool teleport = false;
    if (keyFrames[keyFrames.size() - 1].node->mapid != keyFrames[0].node->mapid)
        teleport = true;

    WayPoint pos(keyFrames[0].node->mapid, keyFrames[0].node->x, keyFrames[0].node->y, keyFrames[0].node->z, teleport,
                 keyFrames[0].node->arrivalEventID, keyFrames[0].node->departureEventID);
    m_WayPoints[0] = pos;
    t += keyFrames[0].node->delay * 1000;

    uint32 cM = keyFrames[0].node->mapid;
    for (size_t i = 0; i < keyFrames.size() - 1; ++i)
    {
        float d = 0;
        float tFrom = keyFrames[i].tFrom;
        float tTo = keyFrames[i].tTo;

        // keep the generation of all these points; we use only a few now, but may need the others later
        if (((d < keyFrames[i + 1].distFromPrev) && (tTo > 0)))
        {
            while ((d < keyFrames[i + 1].distFromPrev) && (tTo > 0))
            {
                tFrom += 100;
                tTo -= 100;

                if (d > 0)
                {
                    float newX, newY, newZ;
                    newX = keyFrames[i].node->x + (keyFrames[i + 1].node->x - keyFrames[i].node->x) * d / keyFrames[i + 1].distFromPrev;
                    newY = keyFrames[i].node->y + (keyFrames[i + 1].node->y - keyFrames[i].node->y) * d / keyFrames[i + 1].distFromPrev;
                    newZ = keyFrames[i].node->z + (keyFrames[i + 1].node->z - keyFrames[i].node->z) * d / keyFrames[i + 1].distFromPrev;

                    bool teleport = false;
                    if (keyFrames[i].node->mapid != cM)
                    {
                        teleport = true;
                        cM = keyFrames[i].node->mapid;
                    }

                    //                    sLog.outString("T: %d, D: %f, x: %f, y: %f, z: %f", t, d, newX, newY, newZ);
                    WayPoint pos(keyFrames[i].node->mapid, newX, newY, newZ, teleport);
                    if (teleport)
                        m_WayPoints[t] = pos;
                }

                if (tFrom < tTo)                            // caught in tFrom dock's "gravitational pull"
                {
                    if (tFrom <= 30000)
                    {
                        d = 0.5f * (tFrom / 1000) * (tFrom / 1000);
                    }
                    else
                    {
                        d = 0.5f * 30 * 30 + 30 * ((tFrom - 30000) / 1000);
                    }
                    d = d - keyFrames[i].distSinceStop;
                }
                else
                {
                    if (tTo <= 30000)
                    {
                        d = 0.5f * (tTo / 1000) * (tTo / 1000);
                    }
                    else
                    {
                        d = 0.5f * 30 * 30 + 30 * ((tTo - 30000) / 1000);
                    }
                    d = keyFrames[i].distUntilStop - d;
                }
                t += 100;
            }
            t -= 100;
        }

        if (keyFrames[i + 1].tFrom > keyFrames[i + 1].tTo)
            t += 100 - ((long)keyFrames[i + 1].tTo % 100);
        else
            t += (long)keyFrames[i + 1].tTo % 100;

        bool teleport = false;
        if ((keyFrames[i + 1].node->actionFlag == 1) || (keyFrames[i + 1].node->mapid != keyFrames[i].node->mapid))
        {
            teleport = true;
            cM = keyFrames[i + 1].node->mapid;
        }

        WayPoint pos(keyFrames[i + 1].node->mapid, keyFrames[i + 1].node->x, keyFrames[i + 1].node->y, keyFrames[i + 1].node->z, teleport,
                     keyFrames[i + 1].node->arrivalEventID, keyFrames[i + 1].node->departureEventID);

        //        sLog.outString("T: %d, x: %f, y: %f, z: %f, t:%d", t, pos.x, pos.y, pos.z, teleport);

        // if (teleport)
        m_WayPoints[t] = pos;

        t += keyFrames[i + 1].node->delay * 1000;
        //        sLog.outString("------");
    }

    uint32 timer = t;

    //    sLog.outDetail("    Generated %lu waypoints, total time %u.", (unsigned long)m_WayPoints.size(), timer);

    m_next = m_WayPoints.begin();                           // will used in MoveToNextWayPoint for init m_curr
    MoveToNextWayPoint();                                   // m_curr -> first point
    MoveToNextWayPoint();                                   // skip first point

    m_pathTime = timer;

    m_nextNodeTime = m_curr->first;

    return true;
}

void Transport::MoveToNextWayPoint()
{
    m_curr = m_next;

    ++m_next;
    if (m_next == m_WayPoints.end())
        m_next = m_WayPoints.begin();
}

void Transport::TeleportTransport(uint32 newMapid, float x, float y, float z)
{
    Map const* oldMap = GetMap();
    Relocate(x, y, z);

    PassengerMap const passengers = m_transportBase->GetPassengers();
    for (PassengerMap::const_iterator itr = passengers.begin(); itr != passengers.end(); ++itr)
    {
        MANGOS_ASSERT(itr->first);

        if (itr->first->GetTypeId() == TYPEID_PLAYER)
        {
            Player* plr = (Player*)itr->first;

            // Is this correct?
            if (plr->isDead() && !plr->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
                plr->ResurrectPlayer(1.0f);

            TransportInfo* transportInfo = plr->GetTransportInfo();

            MANGOS_ASSERT(transportInfo);

            float rx, ry;
            m_transportBase->RotateLocalPosition(transportInfo->GetLocalPositionX(),
                    transportInfo->GetLocalPositionY(), rx, ry);

            // Teleport passenger failed
            if (!plr->TeleportTo(newMapid, x + rx, y + ry, z + transportInfo->GetLocalPositionZ(),
                    MapManager::NormalizeOrientation(GetOrientation() + transportInfo->GetLocalOrientation()), TELE_TO_NOT_LEAVE_TRANSPORT))
            {
                plr->RepopAtGraveyard(); // teleport to near graveyard if on transport, looks blizz like :)
                UnBoardPassenger(plr);
            }

            // WorldPacket data(SMSG_811, 4);
            // data << uint32(0);
            // plr->GetSession()->SendPacket(&data);
        }
        //else
        // ToDo: Add creature/gameobject relocation map1->map2
    }

    // we need to create and save new Map object with 'newMapid' because if not done -> lead to invalid Map object reference...
    // player far teleport would try to create same instance, but we need it NOW for transport...
    // correct me if I'm wrong O.o
    Map* newMap = sMapMgr.CreateMap(newMapid, this);
    SetMap(newMap);

    if (oldMap != newMap)
    {
        UpdateForMap(oldMap);
        UpdateForMap(newMap);
    }
}

void Transport::Update(uint32 update_diff, uint32 /*p_time*/)
{
    if (m_WayPoints.size() <= 1)
        return;

    m_timer = WorldTimer::getMSTime() % m_period;
    while (((m_timer - m_curr->first) % m_pathTime) > ((m_next->first - m_curr->first) % m_pathTime))
    {

        DoEventIfAny(*m_curr, true);

        MoveToNextWayPoint();

        DoEventIfAny(*m_curr, false);

        // first check help in case client-server transport coordinates de-synchronization
        if (m_curr->second.mapid != GetMapId() || m_curr->second.teleport)
        {
            TeleportTransport(m_curr->second.mapid, m_curr->second.x, m_curr->second.y, m_curr->second.z);
        }
        else
        {
            Relocate(m_curr->second.x, m_curr->second.y, m_curr->second.z);

            // Update passenger positions
            m_transportBase->UpdateGlobalPositions();
        }

        m_nextNodeTime = m_curr->first;

        if (m_curr == m_WayPoints.begin())
            DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, " ************ BEGIN ************** %s", GetName());

        DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "%s moved to %f %f %f %d", GetName(), m_curr->second.x, m_curr->second.y, m_curr->second.z, m_curr->second.mapid);
    }
}

void Transport::UpdateForMap(Map const* targetMap)
{
    Map::PlayerList const& pl = targetMap->GetPlayers();
    if (pl.isEmpty())
        return;

    if (GetMapId() == targetMap->GetId())
    {
        for (Map::PlayerList::const_iterator itr = pl.begin(); itr != pl.end(); ++itr)
        {
            if (!itr->getSource()->IsBoarded() || this != itr->getSource()->GetTransportInfo()->GetTransport())
            {
                UpdateData transData;
                BuildCreateUpdateBlockForPlayer(&transData, itr->getSource());
                WorldPacket packet;
                transData.BuildPacket(&packet);
                itr->getSource()->SendDirectMessage(&packet);
            }
        }
    }
    else
    {
        UpdateData transData;
        BuildOutOfRangeUpdateBlock(&transData);
        WorldPacket out_packet;
        transData.BuildPacket(&out_packet);

        for (Map::PlayerList::const_iterator itr = pl.begin(); itr != pl.end(); ++itr)
            if (!itr->getSource()->IsBoarded() || this != itr->getSource()->GetTransportInfo()->GetTransport())
                itr->getSource()->SendDirectMessage(&out_packet);
    }
}

void Transport::DoEventIfAny(WayPointMap::value_type const& node, bool departure)
{
    if (uint32 eventid = departure ? node.second.departureEventID : node.second.arrivalEventID)
    {
        DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "Taxi %s event %u of node %u of %s \"%s\") path", departure ? "departure" : "arrival", eventid, node.first, GetGuidStr().c_str(), GetName());

        if (!sScriptMgr.OnProcessEvent(eventid, this, this, departure))
            GetMap()->ScriptsStart(sEventScripts, eventid, this, this);
    }
}

bool Transport::BoardPassenger(WorldObject* passenger, float lx, float ly, float lz, float lo)
{
    MANGOS_ASSERT(passenger);

    // Passenger still boarded
    if (passenger->IsBoarded())
        return false;

    // Transport size is limited
    if (fabs(lx) > 50.0f || fabs(ly) > 50.0f || fabs(lz) > 50.0f)
        return false;

    m_transportBase->BoardPassenger(passenger, lx, ly, lz, lo, 255);

    // Set ONTRANSPORT flag for client
    if (passenger->GetObjectGuid().IsUnit())
        ((Unit*)passenger)->m_movementInfo.AddMovementFlag(MOVEFLAG_ONTRANSPORT);

    DETAIL_LOG("%s boarded transport %s.", passenger->GetName(), GetName());
    return true;
}

bool Transport::UnBoardPassenger(WorldObject* passenger)
{
    MANGOS_ASSERT(passenger);

    // Passenger already unboarded
    if (!passenger->IsBoarded())
        return false;

    // Remove ONTRANSPORT flag
    if (passenger->GetObjectGuid().IsUnit())
        ((Unit*)passenger)->m_movementInfo.RemoveMovementFlag(MOVEFLAG_ONTRANSPORT);

    m_transportBase->UnBoardPassenger(passenger);
    DETAIL_LOG("%s removed from transport %s.", passenger->GetName(), GetName());
    return true;
}
