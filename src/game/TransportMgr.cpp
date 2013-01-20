/*
 * Copyright (C) Continued-MaNGOS Project
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

#include "TransportMgr.h"
#include "Policies/SingletonImp.h"
#include "TransportSystem.h"
#include "ProgressBar.h"
#include "movement/spline.h"

using namespace Movement;

INSTANTIATE_SINGLETON_1(TransportMgr);

TransportMgr::~TransportMgr()
{
    // Delete splines
    for (StaticTransportInfoMap::const_iterator itr = m_staticTransportInfos.begin(); itr != m_staticTransportInfos.end(); ++itr)
        for (uint8 i = 0; i < MAX_MAPS_PER_MOT; ++i)
            delete itr->second.splineList[i];
}

void TransportMgr::InsertTransporter(GameObjectInfo const* goInfo)
{
    MANGOS_ASSERT(goInfo && goInfo->type == GAMEOBJECT_TYPE_MO_TRANSPORT && goInfo->moTransport.taxiPathId < sTaxiPathNodesByPath.size());

    StaticTransportInfo transportInfo(goInfo);
    TaxiPathNodeList const& path = GetTaxiPathNodeList(goInfo->moTransport.taxiPathId);
    MANGOS_ASSERT(path.size() > 0);                         // Empty path :?

    SplineBase::ControlArray transportPaths[MAX_MAPS_PER_MOT];

    // First split transport paths
    for (uint8 i = 0; i < MAX_MAPS_PER_MOT; ++i)
        transportInfo.mapIDs[i] = path[0].mapid;

    for (uint32 i = 0, j = 0; i < path.size(); ++i)
    {
        if (path[i].mapid != transportInfo.mapIDs[j])
        {
            ++j;
            transportInfo.mapIDs[j] = path[i].mapid;
        }

        transportPaths[j].push_back(G3D::Vector3(path[i].x, path[i].y, path[i].z));

        if (path[i].delay)
            transportInfo.period += path[i].delay * 1000; // Delay is in seconds
    }

    for (uint32 i = 0; i < MAX_MAPS_PER_MOT && (i == 0 || transportInfo.mapIDs[i - 1] != transportInfo.mapIDs[i]); ++i)
    {
        Spline<int32>* transportSpline = new Spline<int32>();
        // ToDo: Add support for cyclic transport paths
        transportSpline->init_spline(&transportPaths[i][0], transportPaths[i].size(), SplineBase::ModeCatmullrom);

        // The same struct is defined in MoveSpline.cpp
        struct InitCacher
        {
            float velocityInv;
            int32 time;

            InitCacher(float _velocity) :
                velocityInv(1000.f / _velocity), time(1) {}

            inline int operator()(Movement::Spline<int32>& s, int32 i)
            {
                time += (s.SegLength(i) * velocityInv);
                return time;
            }
        };

        InitCacher init(goInfo->moTransport.moveSpeed);
        transportSpline->initLengths(init);

        // All points are at same coords
        MANGOS_ASSERT(transportSpline->length() > 1);

        transportInfo.period += transportSpline->length();
        transportInfo.splineList[i] = transportSpline;
    }

    // Note, this uses the copy-constructor for transportInfo, must be evaluated if using new and pointer is better choide
    m_staticTransportInfos.insert(StaticTransportInfoMap::value_type(goInfo->id, transportInfo));
}

void TransportMgr::InitializeTransporters()
{
    for (StaticTransportInfoMap::const_iterator itr = m_staticTransportInfos.begin(); itr != m_staticTransportInfos.end(); ++itr)
    {
        MapEntry const* mapEntry = sMapStore.LookupEntry(itr->second.mapIDs[0]);
        if (!mapEntry)
        {
            sLog.outError("Transporter could not be created because of an invalid mapId. Check DBC files.");
            continue;
        }

        // Initialize only continental transporters
        if (!mapEntry->IsContinent())
            continue;

        // ToDo: Maybe it's better to load all continental maps on server startup (before transport initialization) directly and use sMapMgr.FindMap() here instead
        Map* continentalMap = sMapMgr.CreateMap(mapEntry->MapID, NULL);
        MANGOS_ASSERT(continentalMap);

        G3D::Vector3 const& startPos = itr->second.splineList[0]->getPoint(itr->second.splineList[0]->first());
        CreateTransporter(itr->second.goInfo, continentalMap, startPos.x, startPos.y, startPos.z, itr->second.period);
    }
}

void TransportMgr::LoadTransporterForInstanceMap(Map* map)
{
    MANGOS_ASSERT(map);

    // Continental MOTs must not be loaded here (They are already)
    if (!map->Instanceable())
        return;

    for (StaticTransportInfoMap::const_iterator itr = m_staticTransportInfos.begin(); itr != m_staticTransportInfos.end(); ++itr)
    {
        // Instance transporter must be always in one map. Note: This iterates over all transporters, hence we must check this way
        if (itr->second.mapIDs[0] == itr->second.mapIDs[1])
        {
            uint32 j = 0;
            for (; j < MAX_MAPS_PER_MOT; ++j)
                if (itr->second.mapIDs[j] == map->GetId())
                    break;

            if (j == MAX_MAPS_PER_MOT)                      // This is not the transporter we are looking for
                continue;

            G3D::Vector3 const& startPos = itr->second.splineList[j]->getPoint(itr->second.splineList[j]->first());
            CreateTransporter(itr->second.goInfo, map, startPos.x, startPos.y, startPos.z, itr->second.period);
        }
    }
}

void TransportMgr::ReachedLastWaypoint(GOTransportBase const* transportBase)
{
    DEBUG_LOG("TransportMgr Transporter %u reached last waypoint on map %u", transportBase->GetOwner()->GetEntry(), transportBase->GetOwner()->GetMapId());

    MANGOS_ASSERT(transportBase && transportBase->GetOwner()->GetObjectGuid().IsMOTransport());

    StaticTransportInfoMap::const_iterator staticInfo = m_staticTransportInfos.find(transportBase->GetOwner()->GetEntry());
    MANGOS_ASSERT(staticInfo != m_staticTransportInfos.end());

    uint32 mapIdx = 0;
    for (; mapIdx < MAX_MAPS_PER_MOT; ++mapIdx)
        if (staticInfo->second.mapIDs[mapIdx] == transportBase->GetOwner()->GetMapId())
            break;

    MANGOS_ASSERT(mapIdx < MAX_MAPS_PER_MOT && "This MOTransporter should never be created in it's current map.");

    ++mapIdx %= MAX_MAPS_PER_MOT;                           // Select next map

    // Transporter that only move on one map don't need multi-map teleportation
    if (staticInfo->second.mapIDs[mapIdx] == transportBase->GetOwner()->GetMapId())
        return;

    DEBUG_LOG("TransportMgr Transporter %u teleport to map %u", transportBase->GetOwner()->GetEntry(), staticInfo->second.mapIDs[mapIdx]);

    // ToDo: Maybe it's better to load all continental maps on server startup directly and use sMapMgr.FindMap() here instead
    Map* nextMap = sMapMgr.CreateMap(staticInfo->second.mapIDs[mapIdx], NULL);
    MANGOS_ASSERT(nextMap);

    // Create new transporter on the next map
    G3D::Vector3 const& startPos = staticInfo->second.splineList[mapIdx]->getPoint(staticInfo->second.splineList[mapIdx]->first());
    CreateTransporter(staticInfo->second.goInfo, nextMap, startPos.x, startPos.y, startPos.z, staticInfo->second.period);

    /* Teleport player passengers to the next map,
        and destroy the transporter and it's other passengers */
    for (PassengerMap::const_iterator passengerItr = transportBase->GetPassengers().begin(); passengerItr != transportBase->GetPassengers().end();)
    {
        MANGOS_ASSERT(passengerItr->first);

        if (passengerItr->first->GetTypeId() == TYPEID_PLAYER)
        {
            Player* plr = (Player*)passengerItr->first;

            // Is this correct?
            if (plr->isDead() && !plr->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
                plr->ResurrectPlayer(1.0f);

            TransportInfo* transportInfo = plr->GetTransportInfo();
            MANGOS_ASSERT(transportInfo);

            // TeleportTo unboards passenger
            if (!plr->TeleportTo(staticInfo->second.mapIDs[mapIdx] /*new map id*/, transportInfo->GetLocalPositionX(), transportInfo->GetLocalPositionY(),
                transportInfo->GetLocalPositionZ(), transportInfo->GetLocalOrientation(), 0, NULL, staticInfo->second.goInfo->id))
                plr->RepopAtGraveyard();                    // teleport to near graveyard if on transport, looks blizz like :)

            passengerItr = transportBase->GetPassengers().begin();
        }
        else
            ++passengerItr;
    }

#ifdef DEBUG_SHOW_MOT_WAYPOINTS
    transportBase->GetOwner()->GetMap()->MonsterYellToMap(GetCreatureTemplateStore(1), 42, 0, NULL);
#endif

    // Destroy old transporter
    transportBase->GetOwner()->Delete();
}

Movement::Spline<int32> const* TransportMgr::GetTransportSpline(uint32 goEntry, uint32 mapId)
{
    StaticTransportInfoMap::const_iterator itr = m_staticTransportInfos.find(goEntry);

    if (itr == m_staticTransportInfos.end())
        return NULL;

    uint32 mapIdx = 0;
    for (; mapIdx < MAX_MAPS_PER_MOT; ++mapIdx)
        if (itr->second.mapIDs[mapIdx] == mapId)
            break;

    if (mapIdx >= MAX_MAPS_PER_MOT)
        return NULL;

    return itr->second.splineList[mapIdx];
}

TaxiPathNodeList const& TransportMgr::GetTaxiPathNodeList(uint32 pathId)
{
    MANGOS_ASSERT(pathId < sTaxiPathNodesByPath.size() && "Generating transport path failed. Check DBC files or transport GO data0 field.");
    return sTaxiPathNodesByPath[pathId];
}

ObjectGuid TransportMgr::GetTransportGuid(uint32 entry)
{
    DynamicTransportInfoMap::const_iterator itr = m_dynamicTransportInfos.find(entry);

    if (itr == m_dynamicTransportInfos.end())
        return ObjectGuid();

    return itr->second.transportGuid;
}

uint32 TransportMgr::GetCurrentMapId(uint32 entry)
{
    DynamicTransportInfoMap::const_iterator itr = m_dynamicTransportInfos.find(entry);

    if (itr == m_dynamicTransportInfos.end())
        return 0;

    return itr->second.currentMapId;
}

GameObject* TransportMgr::CreateTransporter(const GameObjectInfo* goInfo, Map* map, float x, float y, float z, uint32 period)
{
    MANGOS_ASSERT(goInfo && map);

    DEBUG_LOG("Create transporter %s, map %u", goInfo->name, map->GetId());

    GameObject* transporter = new GameObject;

    // Guid == Entry :? Orientation :?
    if (!transporter->Create(goInfo->id, goInfo->id, map, PHASEMASK_ANYWHERE, x, y, z, 0.0f))
    {
        delete transporter;
        return NULL;
    }

    transporter->SetName(goInfo->name);
    // Most massive object transporter are always active objects
    transporter->SetActiveObjectState(true);
    // Set period
    transporter->SetUInt32Value(GAMEOBJECT_LEVEL, period);
    // Add the transporter to the map
    map->Add<GameObject>(transporter);
    // Insert / Overwrite dynamic transport data
    m_dynamicTransportInfos[goInfo->id] = DynamicTransportInfo(transporter->GetObjectGuid(), map->GetId());

#ifdef DEBUG_SHOW_MOT_WAYPOINTS
    // Debug helper, view waypoints
    TaxiPathNodeList const& path = GetTaxiPathNodeList(goInfo->moTransport.taxiPathId);
    for (uint32 i = 0; i < path.size(); ++i)
    {
        if (path[i].mapid != map->GetId())
            continue;

        if (Creature* pSummoned = transporter->SummonCreature(1, path[i].x, path[i].y, path[i].z + 30.0f, 0.0f, TEMPSUMMON_TIMED_DESPAWN, period, true))
            pSummoned->SetObjectScale(1.0f + (path.size() - i) * 1.0f);
    }
    // Debug helper, yell a bit
    map->MonsterYellToMap(GetCreatureTemplateStore(1), 41, 0, NULL);
#endif

    return transporter;
}
