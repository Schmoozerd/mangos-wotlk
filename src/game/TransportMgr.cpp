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
    // HACK ALERT: REMOVE THIS !!!
    for (TransportSet::const_iterator itr = m_transports.begin(); itr != m_transports.end(); ++itr)
        delete *itr;

    // Delete splines
    for (StaticTransportInfoMap::const_iterator itr = m_staticTransportInfos.begin(); itr != m_staticTransportInfos.end(); ++itr)
        for (TransportSplineMap::const_iterator itr2 = itr->second.splines.begin(); itr2 != itr->second.splines.end(); ++itr)
            delete itr2->second;
}

void TransportMgr::InsertTransporter(GameObjectInfo const* goInfo)
{
    MANGOS_ASSERT(goInfo && goInfo->type == GAMEOBJECT_TYPE_MO_TRANSPORT && goInfo->moTransport.taxiPathId < sTaxiPathNodesByPath.size());

    StaticTransportInfo transportInfo(goInfo);
    TaxiPathNodeList const& path = GetTaxiPathNodeList(goInfo->moTransport.taxiPathId);
    MANGOS_ASSERT(path.size() > 0); // Empty path :?

    std::map < uint32 /*mapId*/, SplineBase::ControlArray > transportPaths;

    // First split transport paths
    for (uint32 i = 0; i < path.size(); ++i)
    {
        transportPaths[path[i].mapid].push_back(G3D::Vector3(path[i].x, path[i].y, path[i].z));

        if (path[i].delay)
            transportInfo.period += path[i].delay * 1000; // Delay is in seconds
    }

    for (std::map<uint32, SplineBase::ControlArray>::const_iterator itr = transportPaths.begin(); itr != transportPaths.end(); ++itr)
    {
        Spline<int32>* transportSpline = new Spline<int32>();
        // ToDo: Add support for cyclic transport paths
        transportSpline->init_spline(&itr->second[0], itr->second.size(), SplineBase::ModeCatmullrom);

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
        transportInfo.splines[itr->first] = transportSpline;
    }

    m_staticTransportInfos.insert(StaticTransportInfoMap::value_type(goInfo->id, transportInfo));
}

void TransportMgr::InitializeTransporters()
{
    for (StaticTransportInfoMap::const_iterator itr = m_staticTransportInfos.begin(); itr != m_staticTransportInfos.end(); ++itr)
    {
        // Start with first map
        TransportSplineMap::const_iterator itr2 = itr->second.splines.begin();
        MANGOS_ASSERT(itr2 != itr->second.splines.end());

        MapEntry const* mapEntry = sMapStore.LookupEntry(itr2->first);

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

        G3D::Vector3 const& startPos = itr2->second->getPoint(itr2->second->first());
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
        // Instance transporter must be always in one map
        if (itr->second.splines.size() == 1)
        {
            TransportSplineMap::const_iterator itr2 = itr->second.splines.find(map->GetId());

            // This is not the transporter we are looking for
            if (itr2 == itr->second.splines.end())
                continue;

            G3D::Vector3 const& startPos = itr2->second->getPoint(itr2->second->first());
            CreateTransporter(itr->second.goInfo, map, startPos.x, startPos.y, startPos.z, itr->second.period);
        }
    }
}

void TransportMgr::ReachedLastWaypoint(GOTransportBase const* transportBase)
{
    MANGOS_ASSERT(transportBase && transportBase->GetOwner()->GetObjectGuid().IsMOTransport());

    StaticTransportInfoMap::const_iterator itr = m_staticTransportInfos.find(transportBase->GetOwner()->GetEntry());
    MANGOS_ASSERT(itr != m_staticTransportInfos.end());

    TransportSplineMap::const_iterator itr2 = itr->second.splines.find(transportBase->GetOwner()->GetMapId());
    MANGOS_ASSERT(itr2 != itr->second.splines.end() && "This MOTransporter should never be created in it's current map.");

    ++itr2;
    if (itr2 == itr->second.splines.end())
        itr2 = itr->second.splines.begin();

    MANGOS_ASSERT(itr2->first != transportBase->GetOwner()->GetMapId() && "The next mapId for the MOTransporter would be the same as the current.");

    // ToDo: Maybe it's better to load all continental maps on server startup directly and use sMapMgr.FindMap() here instead
    Map* nextMap = sMapMgr.CreateMap(itr2->first, NULL);
    MANGOS_ASSERT(nextMap);

    // Create new transporter on the next map
    G3D::Vector3 const& startPos = itr2->second->getPoint(itr2->second->first());
    CreateTransporter(itr->second.goInfo, nextMap, startPos.x, startPos.y, startPos.z, itr->second.period);

    /* Teleport player passengers to the next map,
        and destroy the transporter and it's other passengers */
    for (PassengerMap::const_iterator itr3 = transportBase->GetPassengers().begin(); itr3 != transportBase->GetPassengers().end(); itr3 = transportBase->GetPassengers().begin())
    {
        MANGOS_ASSERT(itr3->first);

        if (itr3->first->GetTypeId() == TYPEID_PLAYER)
        {
            Player* plr = (Player*)itr3->first;

            // Is this correct?
            if (plr->isDead() && !plr->HasFlag(PLAYER_FLAGS, PLAYER_FLAGS_GHOST))
                plr->ResurrectPlayer(1.0f);

            TransportInfo* transportInfo = plr->GetTransportInfo();
            MANGOS_ASSERT(transportInfo);

            // TeleportTo unboards passenger
            if (!plr->TeleportTo(itr2->first, transportInfo->GetLocalPositionX(), transportInfo->GetLocalPositionY(),
                transportInfo->GetLocalPositionZ(), transportInfo->GetLocalOrientation(), 0, NULL, itr->second.goInfo->id))
                plr->RepopAtGraveyard(); // teleport to near graveyard if on transport, looks blizz like :)
        }
        //else
        // remove WorldObject
    }

    //HACK
    m_transports.erase(transportBase->GetOwner());

    // Destroy old transporter
    transportBase->GetOwner()->Delete();
}

Movement::Spline<int32> const* TransportMgr::GetTransportSpline(uint32 goEntry, uint32 mapId)
{
    StaticTransportInfoMap::const_iterator itr = m_staticTransportInfos.find(goEntry);

    if (itr == m_staticTransportInfos.end())
        return NULL;

    TransportSplineMap::const_iterator itr2 = itr->second.splines.find(mapId);

    if (itr2 == itr->second.splines.end())
        return NULL;

    return itr2->second;
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
    // HACK: Remove IT!!
    m_transports.insert(transporter);
    // Insert / Overwrite dynamic transport data
    m_dynamicTransportInfos[goInfo->id] = DynamicTransportInfo(transporter->GetObjectGuid(), map->GetId());

    return transporter;
}
