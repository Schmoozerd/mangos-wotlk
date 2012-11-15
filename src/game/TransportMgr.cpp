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
#include "./movement/spline.h"

using namespace Movement;

INSTANTIATE_SINGLETON_1(TransportMgr);

TransportMgr::~TransportMgr()
{
    // ToDo: They should get deleted after map unloading :?
    for (TransportSet::const_iterator itr = m_transports.begin(); itr != m_transports.end(); ++itr)
        delete *itr;
}

void TransportMgr::InsertTransporter(GameObjectInfo const* goInfo)
{
    MANGOS_ASSERT(goInfo && goInfo->type == GAMEOBJECT_TYPE_MO_TRANSPORT && goInfo->moTransport.taxiPathId < sTaxiPathNodesByPath.size());

    MOTransportInfo transportInfo(goInfo);
    TaxiPathNodeList const& path = GetTaxiPathNodeList(goInfo->moTransport.taxiPathId);
    MANGOS_ASSERT(path.size()); // Empty path :?

    // Default
    transportInfo.mapIds[0] = path[0].mapid;
    transportInfo.mapIds[1] = path[0].mapid;

    for (uint32 i = 0; i < path.size(); ++i)
    {
        if (transportInfo.mapIds[0] != path[i].mapid)
        {
            transportInfo.mapIds[1] = path[i].mapid;
            break;
        }
    }

    m_transportInfos.insert(TransportInfoMap::value_type(goInfo->id, transportInfo));
}

void TransportMgr::LoadTransporterForMap(Map* map)
{
    MANGOS_ASSERT(map);

    for (TransportInfoMap::const_iterator itr = m_transportInfos.begin(); itr != m_transportInfos.end(); ++itr)
    {
        // Transporter never visits this map
        if (itr->second.mapIds[0] != map->GetId() && itr->second.mapIds[1] != map->GetId())
            continue;

        // ToDo: Avoid double creating with loading different maps

        TransportRoute route = GetTransportRouteForMap(itr->second.goInfo->moTransport.taxiPathId, map->GetId());
        MANGOS_ASSERT(route.size());

        CreateTransporter(itr->second.goInfo, map, route.begin()->x, route.begin()->y, route.begin()->z);
    }
}

TransportRoute TransportMgr::GetTransportRouteForMap(uint32 pathId, uint32 mapId)
{
    TaxiPathNodeList const& path = GetTaxiPathNodeList(pathId);
    TransportRoute route;

    for (uint32 i = 0; i < path.size(); ++i)
        if (path[i].mapid == mapId)
            route.push_back(Vector3(path[i].x, path[i].y, path[i].z));

    return route;
}

TaxiPathNodeList const& TransportMgr::GetTaxiPathNodeList(uint32 pathId)
{
    MANGOS_ASSERT(pathId < sTaxiPathNodesByPath.size() && "Generating transport path failed. Check DBC files or transport GO data0 field.");
    return sTaxiPathNodesByPath[pathId];
}

uint32 TransportMgr::GetNextMapId(uint32 goEntry, uint32 currentMapId)
{
    TransportInfoMap::const_iterator itr = m_transportInfos.find(goEntry);
    MANGOS_ASSERT(itr != m_transportInfos.end());

    if (itr->second.mapIds[0] == currentMapId)
    {
        return itr->second.mapIds[1];
    }
    else if (itr->second.mapIds[1] == currentMapId)
        return itr->second.mapIds[0];

    MANGOS_ASSERT(false && "This MOTransporter should never be created in it's current map");
}

void TransportMgr::CreateTransporter(const GameObjectInfo* goInfo, Map* map, float x, float y, float z)
{
    MANGOS_ASSERT(goInfo && map);

    DEBUG_LOG("Create transporter %s, map %u", goInfo->name, map->GetId());

    GameObject* transporter = new GameObject;

    // Guid == Entry :? Orientation :?
    if (!transporter->Create(goInfo->id, goInfo->id, map, PHASEMASK_ANYWHERE, x, y, z, 0.0f))
    {
        delete transporter;
        return;
    }

    transporter->SetName(goInfo->name);
    // Most massive object transporter are always active objects
    transporter->SetActiveObjectState(true);
    // Add the transporter to the map
    map->Add<GameObject>(transporter);
    // Insert transporter into our "fast" set of transporters
    m_transports.insert(transporter);
}
