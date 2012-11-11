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

INSTANTIATE_SINGLETON_1(TransportMgr);

TransportMgr::~TransportMgr()
{
    for (TransportSet::const_iterator itr = m_transports.begin(); itr != m_transports.end(); ++itr)
        delete *itr;
}

void TransportMgr::LoadTransports()
{
    QueryResult* result = WorldDatabase.Query("SELECT entry, name, period FROM transports");

    uint32 count = 0;

    if (!result)
    {
        BarGoLink bar(1);
        bar.step();

        sLog.outString();
        sLog.outString(">> Loaded %u transports", count);
        return;
    }

    BarGoLink bar(result->GetRowCount());

    do
    {
        bar.step();

        Field* fields = result->Fetch();

        uint32 entry = fields[0].GetUInt32();
        std::string name = fields[1].GetCppString();
        uint32 period = fields[2].GetUInt32();

        const GameObjectInfo* pGOInfo = ObjectMgr::GetGameObjectInfo(entry);

        if (!pGOInfo)
        {
            sLog.outErrorDb("Transport ID: %u, Name: %s, will not be loaded, gameobject_template missing", entry, name.c_str());
            continue;
        }

        if (pGOInfo->type != GAMEOBJECT_TYPE_MO_TRANSPORT)
        {
            sLog.outErrorDb("Transport ID: %u, Name: %s, will not be loaded, gameobject_template type wrong", entry, name.c_str());
            continue;
        }

        GameObject* transporter = new GameObject;

        /* First of all we have to create our transportbase and calculate it's path,
           to get the startposition and set it correctly later */
        transporter->SetTransportBase(pGOInfo->moTransport.taxiPathId);
        GOTransportBase* transportBase = transporter->GetTransportBase();
        MANGOS_ASSERT(transportBase); // Can occur with invalid taxiPathId

        /* As most MOT's are active objects we have to create a valid map object as well
           ToDo: MOT's in instances are not possible with the current handling */
        Map* transporterMap = sMapMgr.CreateMap(transportBase->GetCurrentWayPoint().mapid, transporter);
        MANGOS_ASSERT(transporterMap); // Instancemaps will crash for sure

        // Guid == Entry :? Orientation :? QuaternionData :?
        if (!transporter->Create(pGOInfo->id, pGOInfo->id, transporterMap, PHASEMASK_ANYWHERE, transportBase->GetCurrentWayPoint().x,
                    transportBase->GetCurrentWayPoint().y, transportBase->GetCurrentWayPoint().z, 0.0f, QuaternionData(), GO_ANIMPROGRESS_DEFAULT))
        {
            sLog.outError("Transport ID: %u, Name: %s, could not be created. Skipped", entry, name.c_str());
            delete transporter;
            continue;
        }

        // Set period
        transporter->SetUInt32Value(GAMEOBJECT_LEVEL, period);

        // low part always 0, dynamicHighValue is some kind of progression (not implemented)
        transporter->SetUInt16Value(GAMEOBJECT_DYNAMIC, 0, 0);
        transporter->SetUInt16Value(GAMEOBJECT_DYNAMIC, 1, 0 /*dynamicHighValue*/);

        // Most massive object transporter are always active objects
        transporter->SetActiveObjectState(true);

        // Add the transporter to the map
        transporterMap->Add<GameObject>(transporter);

        // Insert our new mot
        m_transports.insert(transporter);

        ++count;
    }
    while (result->NextRow());

    delete result;

    sLog.outString();
    sLog.outString(">> Loaded %u transports", count);

    // check transport data DB integrity
    result = WorldDatabase.Query("SELECT gameobject.guid,gameobject.id,transports.name FROM gameobject,transports WHERE gameobject.id = transports.entry");
    if (result)                                             // wrong data found
    {
        do
        {
            Field* fields = result->Fetch();

            uint32 guid  = fields[0].GetUInt32();
            uint32 entry = fields[1].GetUInt32();
            std::string name = fields[2].GetCppString();
            sLog.outErrorDb("Transport %u '%s' have record (GUID: %u) in `gameobject`. Transports DON'T must have any records in `gameobject` or its behavior will be unpredictable/bugged.", entry, name.c_str(), guid);
        }
        while (result->NextRow());

        delete result;
    }
}
