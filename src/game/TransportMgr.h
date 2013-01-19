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

// Use this while developing for MOTs
#define DEBUG_SHOW_MOT_WAYPOINTS

namespace Movement
{
    template<typename length_type> class Spline;
};

class GOTransportBase;

typedef std::map < uint32 /*mapId*/, Movement::Spline<int32>* > TransportSplineMap; // Waypoint path in mapId

struct StaticTransportInfo                                                          // Static information for each transporter
{
    GameObjectInfo const* goInfo;                                                   // goInfo
    TransportSplineMap splines;                                                     // Waypoints
    uint32 period;                                                                  // period for one circle

    StaticTransportInfo(GameObjectInfo const* _goInfo) :
        goInfo(_goInfo), period(0) {}
};

// Track on which map a multi-map transport currently is
struct DynamicTransportInfo                                                         // Class for what exactly, actually we only need to track the muli-map transports, rest is easy
{
    ObjectGuid transportGuid;
    uint32 currentMapId; // ToDo: There is probably an issue with instance transports

    // Standard constructor for std::map usage
    DynamicTransportInfo() :
        transportGuid(ObjectGuid()), currentMapId(0) {}

    DynamicTransportInfo(ObjectGuid _transportGuid, uint32 _currentMapId) :
        transportGuid(_transportGuid), currentMapId(_currentMapId) {}
};

typedef std::map < uint32 /*goEntry*/, StaticTransportInfo > StaticTransportInfoMap; // Static data by go-entry
typedef std::map < uint32 /*goEntry*/, DynamicTransportInfo > DynamicTransportInfoMap; // dynamic data by go-entry

class TransportMgr                                          // Mgr to hold static data and manage multi-map transports
{
    public:
        TransportMgr() {}
        ~TransportMgr();

        void InsertTransporter(GameObjectInfo const* goInfo); // Called on GO-Loading, creates static data for this go
        void InitializeTransporters();                      // Called from World. Starts Transporters on Continents

        void LoadTransporterForInstanceMap(Map* map);       // Called in Map constructor
        void ReachedLastWaypoint(GOTransportBase const* transportBase); // Called by GOTransportBase::Update when last waypoint is reached. Will trigger action for passengers

        Movement::Spline<int32> const* GetTransportSpline(uint32 goEntry, uint32 mapId);    // Get Static Waypoint Data for a transporter and map
        TaxiPathNodeList const& GetTaxiPathNodeList(uint32 pathId);
        ObjectGuid GetTransportGuid(uint32 entry);                                          // Get guid of current transporter
        uint32 GetCurrentMapId(uint32 entry);                                               // Get current mapId of transporter

    private:
        GameObject* CreateTransporter(const GameObjectInfo* goInfo, Map* map, float x, float y, float z, uint32 period);

        StaticTransportInfoMap m_staticTransportInfos;
        DynamicTransportInfoMap m_dynamicTransportInfos;
};

#define sTransportMgr MaNGOS::Singleton<TransportMgr>::Instance()

#endif
