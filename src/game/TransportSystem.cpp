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

/**
 * @addtogroup TransportSystem
 * @{
 *
 * @file TransportSystem.cpp
 * This file contains the code needed for MaNGOS to provide abstract support for transported entities
 * Currently implemented
 * - Calculating between local and global coords
 * - Abstract storage of passengers (added by BoardPassenger, UnboardPassenger)
 */

#include "TransportSystem.h"
#include "Unit.h"
#include "Vehicle.h"
#include "MapManager.h"

/* **************************************** TransportBase ****************************************/

TransportBase::TransportBase(WorldObject* owner) :
    m_owner(owner),
    m_lastPosition(owner->GetPositionX(), owner->GetPositionY(), owner->GetPositionZ(), owner->GetOrientation()),
    m_sinO(sin(m_lastPosition.o)),
    m_cosO(cos(m_lastPosition.o)),
    m_updatePositionsTimer(500)
{
    MANGOS_ASSERT(m_owner);
}

TransportBase::~TransportBase()
{
    MANGOS_ASSERT(m_passengers.size() == 0);
}

// Update every now and then (after some change of transporter's position)
// This is used to calculate global positions (which don't have to be exact, they are only required for some server-side calculations
void TransportBase::Update(uint32 diff)
{
    if (m_updatePositionsTimer < diff)
    {
        if (fabs(m_owner->GetPositionX() - m_lastPosition.x) +
                fabs(m_owner->GetPositionY() - m_lastPosition.y) +
                fabs(m_owner->GetPositionZ() - m_lastPosition.z) > 1.0f ||
                MapManager::NormalizeOrientation(m_owner->GetOrientation() - m_lastPosition.o) > 0.01f)
            UpdateGlobalPositions();

        m_updatePositionsTimer = 500;
    }
    else
        m_updatePositionsTimer -= diff;
}

// Update the global positions of all passengers
void TransportBase::UpdateGlobalPositions()
{
    Position pos(m_owner->GetPositionX(), m_owner->GetPositionY(),
                 m_owner->GetPositionZ(), m_owner->GetOrientation());

    // Calculate new direction multipliers
    if (MapManager::NormalizeOrientation(pos.o - m_lastPosition.o) > 0.01f)
    {
        m_sinO = sin(pos.o);
        m_cosO = cos(pos.o);
    }

    // Update global positions
    for (PassengerMap::const_iterator itr = m_passengers.begin(); itr != m_passengers.end(); ++itr)
        UpdateGlobalPositionOf(itr->first, itr->second->GetLocalPositionX(), itr->second->GetLocalPositionY(),
                               itr->second->GetLocalPositionZ(), itr->second->GetLocalOrientation());

    m_lastPosition = pos;
}

// Update the global position of a passenger
void TransportBase::UpdateGlobalPositionOf(WorldObject* passenger, float lx, float ly, float lz, float lo) const
{
    float gx, gy, gz, go;
    CalculateGlobalPositionOf(lx, ly, lz, lo, gx, gy, gz, go);

    if (passenger->GetTypeId() == TYPEID_PLAYER || passenger->GetTypeId() == TYPEID_UNIT)
    {
        if (passenger->GetTypeId() == TYPEID_PLAYER)
        {
            m_owner->GetMap()->PlayerRelocation((Player*)passenger, gx, gy, gz, go);
        }
        else
            m_owner->GetMap()->CreatureRelocation((Creature*)passenger, gx, gy, gz, go);

        // If passenger is vehicle
        if (((Unit*)passenger)->IsVehicle())
            ((Unit*)passenger)->GetVehicleInfo()->UpdateGlobalPositions();
    }
    // ToDo: Add gameobject relocation
    // ToDo: Add passenger relocation for MO transports
}

// This rotates the vector (lx, ly) by transporter->orientation
void TransportBase::RotateLocalPosition(float lx, float ly, float& rx, float& ry) const
{
    rx = lx * m_cosO - ly * m_sinO;
    ry = lx * m_sinO + ly * m_cosO;
}

// This rotates the vector (rx, ry) by -transporter->orientation
void TransportBase::NormalizeRotatedPosition(float rx, float ry, float& lx, float& ly) const
{
    lx = rx * -m_cosO - ry * -m_sinO;
    ly = rx * -m_sinO + ry * -m_cosO;
}

// Calculate a global position of local positions based on this transporter
void TransportBase::CalculateGlobalPositionOf(float lx, float ly, float lz, float lo, float& gx, float& gy, float& gz, float& go) const
{
    RotateLocalPosition(lx, ly, gx, gy);
    gx += m_owner->GetPositionX();
    gy += m_owner->GetPositionY();

    gz = lz + m_owner->GetPositionZ();
    go = MapManager::NormalizeOrientation(lo + m_owner->GetOrientation());
}

//  Helper function to check if a unit is boarded onto this transporter (or a transporter boarded onto this) recursively
bool TransportBase::HasOnBoard(WorldObject const* passenger) const
{
    MANGOS_ASSERT(passenger);

    // For efficiency we go down from the (possible) passenger until we reached our owner, or until we reached no passenger
    // Note, this will not catch, if self and passenger are boarded onto the same transporter (as it should not)
    while (passenger->IsBoarded())
    {
        // pasenger is boarded onto this
        if (passenger->GetTransportInfo()->GetTransport() == m_owner)
            return true;
        else
            passenger = passenger->GetTransportInfo()->GetTransport();
    }

    return false;
}

void TransportBase::BoardPassenger(WorldObject* passenger, float lx, float ly, float lz, float lo, uint8 seat)
{
    TransportInfo* transportInfo = new TransportInfo(passenger, this, lx, ly, lz, lo, seat);

    // Insert our new passenger
    m_passengers.insert(PassengerMap::value_type(passenger, transportInfo));

    // The passenger needs fast access to transportInfo
    passenger->SetTransportInfo(transportInfo);
}

void TransportBase::UnBoardPassenger(WorldObject* passenger)
{
    PassengerMap::iterator itr = m_passengers.find(passenger);

    if (itr == m_passengers.end())
        return;

    // Set passengers transportInfo to NULL
    passenger->SetTransportInfo(NULL);

    // Delete transportInfo
    delete itr->second;

    // Unboard finally
    m_passengers.erase(itr);
}

/* *************************************** GOTransportBase ***************************************/

GOTransportBase::GOTransportBase(GameObject* owner, uint32 pathId) : TransportBase(owner)
{
    GenerateWaypoints(pathId);
}

GOTransportBase::~GOTransportBase()
{
}

bool GOTransportBase::Board(WorldObject* passenger, float lx, float ly, float lz, float lo)
{
    MANGOS_ASSERT(passenger);

    // Passenger still boarded
    if (passenger->IsBoarded())
        return false;

    // Transport size is limited
    if (fabs(lx) > 50.0f || fabs(ly) > 50.0f || fabs(lz) > 50.0f)
        return false;

    BoardPassenger(passenger, lx, ly, lz, lo, 255);

    // Set ONTRANSPORT flag for client
    if (passenger->GetObjectGuid().IsUnit())
        ((Unit*)passenger)->m_movementInfo.AddMovementFlag(MOVEFLAG_ONTRANSPORT);

    DETAIL_LOG("%s boarded transport %s.", passenger->GetName(), m_owner->GetName());
    return true;
}

bool GOTransportBase::UnBoard(WorldObject* passenger)
{
    MANGOS_ASSERT(passenger);

    // Passenger already unboarded
    if (!passenger->IsBoarded())
        return false;

    // Remove ONTRANSPORT flag
    if (passenger->GetObjectGuid().IsUnit())
        ((Unit*)passenger)->m_movementInfo.RemoveMovementFlag(MOVEFLAG_ONTRANSPORT);

    UnBoardPassenger(passenger);

    DETAIL_LOG("%s removed from transport %s.", passenger->GetName(), m_owner->GetName());
    return true;
}

void GOTransportBase::Update(uint32 diff)
{
    if (m_WayPoints.size() <= 1)
        return;

    m_timer = WorldTimer::getMSTime() % m_owner->GetUInt32Value(GAMEOBJECT_LEVEL);
    while (((m_timer - m_curr->first) % m_pathTime) > ((m_next->first - m_curr->first) % m_pathTime))
    {

        DoEventIfAny(*m_curr, true);

        MoveToNextWayPoint();

        DoEventIfAny(*m_curr, false);

        // first check help in case client-server transport coordinates de-synchronization
        if (m_curr->second.mapid != m_owner->GetMapId() || m_curr->second.teleport)
        {
            TeleportTransport(m_curr->second.mapid, m_curr->second.x, m_curr->second.y, m_curr->second.z);
        }
        else
        {
            m_owner->GetMap()->GameObjectRelocation((GameObject*)m_owner, m_curr->second.x, m_curr->second.y, m_curr->second.z, m_owner->GetOrientation());

            // Update passenger positions
            UpdateGlobalPositions();
        }

        m_nextNodeTime = m_curr->first;

        if (m_curr == m_WayPoints.begin())
            DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, " ************ BEGIN ************** %s", m_owner->GetName());

        DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "%s moved to %f %f %f %d", m_owner->GetName(), m_curr->second.x, m_curr->second.y, m_curr->second.z, m_curr->second.mapid);
    }
}

void GOTransportBase::GenerateWaypoints(uint32 pathId)
{
    MANGOS_ASSERT(pathId < sTaxiPathNodesByPath.size() && "Generating transport path failed. Check DBC files or transport GO data0 field.");

    TaxiPathNodeList const& path = sTaxiPathNodesByPath[pathId];

    std::vector<keyFrame> keyFrames;
    int mapChange = 0;
    //mapids.clear();
    for (size_t i = 1; i < path.size() - 1; ++i)
    {
        if (mapChange == 0)
        {
            TaxiPathNodeEntry const& node_i = path[i];
            if (node_i.mapid == path[i + 1].mapid)
            {
                keyFrame k(node_i);
                keyFrames.push_back(k);
                //mapids.insert(k.node->mapid);
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

    m_nextNodeTime = m_curr->first;*/
}

void GOTransportBase::MoveToNextWayPoint()
{
    m_curr = m_next;

    ++m_next;
    if (m_next == m_WayPoints.end())
        m_next = m_WayPoints.begin();
}

void GOTransportBase::DoEventIfAny(WayPointMap::value_type const& node, bool departure)
{
    if (uint32 eventid = departure ? node.second.departureEventID : node.second.arrivalEventID)
    {
        DEBUG_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "Taxi %s event %u of node %u of %s \"%s\") path", departure ? "departure" : "arrival", eventid, node.first, m_owner->GetGuidStr().c_str(), m_owner->GetName());

        if (!sScriptMgr.OnProcessEvent(eventid, m_owner, m_owner, departure))
            m_owner->GetMap()->ScriptsStart(sEventScripts, eventid, m_owner, m_owner);
    }
}

void GOTransportBase::TeleportTransport(uint32 newMapid, float x, float y, float z)
{
    // Remove transporter from old map
    m_owner->GetMap()->Remove<GameObject>((GameObject*)m_owner, false);

    // we need to create and save new Map object with 'newMapid' because if not done -> lead to invalid Map object reference...
    // player far teleport would try to create same instance, but we need it NOW for transport...
    // correct me if I'm wrong O.o
    Map* newMap = sMapMgr.CreateMap(newMapid, m_owner);
    // Instancemaps are not supported currently
    MANGOS_ASSERT(newMap);

    // Relocate to new position
    m_owner->Relocate(x, y, z);
    // Add transporter to new map
    newMap->Add<GameObject>((GameObject*)m_owner);

    for (PassengerMap::const_iterator itr = m_passengers.begin(); itr != m_passengers.end(); ++itr)
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
            RotateLocalPosition(transportInfo->GetLocalPositionX(),
                    transportInfo->GetLocalPositionY(), rx, ry);

            // Teleport passenger failed
            if (!plr->TeleportTo(newMapid, x + rx, y + ry, z + transportInfo->GetLocalPositionZ(),
                    MapManager::NormalizeOrientation(m_owner->GetOrientation() + transportInfo->GetLocalOrientation()), TELE_TO_NOT_LEAVE_TRANSPORT))
            {
                plr->RepopAtGraveyard(); // teleport to near graveyard if on transport, looks blizz like :)
                UnBoard(plr);
            }

            // WorldPacket data(SMSG_811, 4);
            // data << uint32(0);
            // plr->GetSession()->SendPacket(&data);
        }
        //else
        // ToDo: Add creature/gameobject relocation map1->map2
    }
}

/* **************************************** TransportInfo ****************************************/

TransportInfo::TransportInfo(WorldObject* owner, TransportBase* transport, float lx, float ly, float lz, float lo, uint8 seat) :
    m_owner(owner),
    m_transport(transport),
    m_localPosition(lx, ly, lz, lo),
    m_seat(seat)
{
    MANGOS_ASSERT(owner && m_transport);
}

void TransportInfo::SetLocalPosition(float lx, float ly, float lz, float lo)
{
    m_localPosition.x = lx;
    m_localPosition.y = ly;
    m_localPosition.z = lz;
    m_localPosition.o = lo;

    // Update global position
    m_transport->UpdateGlobalPositionOf(m_owner, lx, ly, lz, lo);
}

/*! @} */
