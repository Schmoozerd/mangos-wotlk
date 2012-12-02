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

#ifndef TRANSPORT_MGR_H
#define TRANSPORT_MGR_H

#include "Policies/Singleton.h"

namespace Movement
{
    template<typename length_type> class Spline;
};

class GOTransportBase;

typedef std::map < uint32 /*mapId*/, Movement::Spline<int32>* > TransportSplineMap;
typedef std::set < GameObject* /*MOTransporter*/ > TransportSet;

struct StaticTransportInfo
{
    GameObjectInfo const* goInfo;
    TransportSplineMap splines;
    uint32 period;

    StaticTransportInfo(GameObjectInfo const* _goInfo) :
        goInfo(_goInfo), period(0) {}
};

struct DynamicTransportInfo
{
    ObjectGuid transportGuid;
    uint32 currentMapId; // ToDo: There is probably an issue with instance transports

    // Standard constructor for std::map usage
    DynamicTransportInfo() :
        transportGuid(ObjectGuid()), currentMapId(0) {}

    DynamicTransportInfo(ObjectGuid _transportGuid, uint32 _currentMapId) :
        transportGuid(_transportGuid), currentMapId(_currentMapId) {}
};

typedef std::map < uint32 /*goEntry*/, StaticTransportInfo > StaticTransportInfoMap;
typedef std::map < uint32 /*goEntry*/, DynamicTransportInfo > DynamicTransportInfoMap;

class TransportMgr
{
    public:
        TransportMgr() {}
        ~TransportMgr();

        void InsertTransporter(GameObjectInfo const* goInfo);
        void InitializeTransporters();

        void LoadTransporterForInstanceMap(Map* map);
        void ReachedLastWaypoint(GOTransportBase const* transportBase);

        // HACK: REMOVE DAT!
        TransportSet const& GetTransports() const { return m_transports; }

        Movement::Spline<int32> const* GetTransportSpline(uint32 goEntry, uint32 mapId);
        TaxiPathNodeList const& GetTaxiPathNodeList(uint32 pathId);
        ObjectGuid GetTransportGuid(uint32 entry);

    private:
        GameObject* CreateTransporter(const GameObjectInfo* goInfo, Map* map, float x, float y, float z, uint32 period);

        TransportSet m_transports;
        StaticTransportInfoMap m_staticTransportInfos;
        DynamicTransportInfoMap m_dynamicTransportInfos;
};

#define sTransportMgr MaNGOS::Singleton<TransportMgr>::Instance()
#endif
