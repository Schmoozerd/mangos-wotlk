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
#include "TransportMgr.h"
#include "movement/MoveSpline.h"

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

GOTransportBase::GOTransportBase(GameObject* owner, uint32 pathId) :
    TransportBase(owner),
    m_transportStopTimer(0),
    m_currentNode(0),
    m_pointIdx(0),
    m_timePassed(0),
    m_pathProgress(0),
    m_bArrived(false)
{
    LoadTransportSpline();
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
    if (m_bArrived)
        return;

    m_pathProgress += diff;

    if (m_transportStopTimer)
    {
        if (m_transportStopTimer < diff)
        {
            m_transportStopTimer = 0;

            // Handle departure event
            //TaxiPathNodeEntry const& node = GetCurrentNode();

            //if (node.departureEventID)
                //DoEventIfAny(player, m_currentNode, departureEvent);
        }
        else
        {
            m_transportStopTimer -= diff;
            return; // MOT is chilling at the beach
        }
    }

    UpdateTransportSpline(diff);

    enum
    {
        POSITION_UPDATE_DELAY = 400,
    };

    if (m_updatePositionsTimer < diff || m_bArrived)
    {
        m_updatePositionsTimer = POSITION_UPDATE_DELAY;
        Movement::Location loc = ComputePosition();

        m_owner->GetMap()->GameObjectRelocation((GameObject*)m_owner, loc.x, loc.y, loc.z, loc.orientation);
        DETAIL_FILTER_LOG(LOG_FILTER_TRANSPORT_MOVES, "%s moved to %f %f %f %f", m_owner->GetName(), loc.x, loc.y, loc.z, loc.orientation);

        // Update passenger positions
        UpdateGlobalPositions();
    }
    else
        m_updatePositionsTimer -= diff;

    uint32 pointId = (uint32)m_pointIdx - m_transportSpline->first() + (int)m_bArrived;
    if (pointId > m_currentNode)
    {
        do
        {
            ++m_currentNode;

            // Handle arrival event
            TaxiPathNodeEntry const& node = GetCurrentNode();

            //if (node.arrivalEventID)
                //DoEventIfAny(player, m_currentNode, arrivalEvent);

            if (node.delay)
            {
                m_transportStopTimer = node.delay * 1000;
                break;
            }

            if (pointId == m_currentNode)
                break;
        }
        while (true);
    }

    // Last waypoint is reached
    if (m_bArrived)
        sTransportMgr.ReachedLastWaypoint(this);
}

void GOTransportBase::LoadTransportSpline()
{
    // ToDo: Handle elevators and similar
    MANGOS_ASSERT(m_owner->GetObjectGuid().IsMOTransport());

    m_transportSpline = sTransportMgr.GetTransportSpline(m_owner->GetEntry(), m_owner->GetMap()->GetId());
    MANGOS_ASSERT(m_transportSpline);

    m_pointIdx = m_transportSpline->first();
}

void GOTransportBase::UpdateTransportSpline(uint32 diff)
{
    m_timePassed += diff;

    if (m_timePassed >= m_transportSpline->length(m_pointIdx + 1))
    {
        ++m_pointIdx;
        if (m_pointIdx >= m_transportSpline->last())
        {
            if (m_transportSpline->isCyclic())
            {
                m_currentNode = 0;
                m_pointIdx = m_transportSpline->first();
                m_timePassed = m_timePassed % m_transportSpline->length();
            }
            else // Arrived
            {
                m_bArrived = true;
                m_pointIdx = m_transportSpline->last() - 1;
                m_timePassed = m_transportSpline->length();
            }
        }
    }
}

TaxiPathNodeEntry const& GOTransportBase::GetCurrentNode()
{
    TaxiPathNodeList const& path = sTransportMgr.GetTaxiPathNodeList(((GameObject*)m_owner)->GetGOInfo()->moTransport.taxiPathId);
    MANGOS_ASSERT(m_currentNode < path.size() && "Current node doesn't exist in transport path.");

    return path[m_currentNode];
}

Movement::Location GOTransportBase::ComputePosition()
{
    float u = 1.f;
    int32 seg_time = m_transportSpline->length(m_pointIdx, m_pointIdx + 1);
    if (seg_time > 0)
        u = (m_timePassed - m_transportSpline->length(m_pointIdx)) / (float)seg_time;
    Movement::Location c;
    m_transportSpline->evaluate_percent(m_pointIdx, u, c);

    G3D::Vector3 hermite;
    m_transportSpline->evaluate_derivative(m_pointIdx, u, hermite);
    c.orientation = atan2(hermite.y, hermite.x);

    return c;
}

/*void GOTransportBase::LoadTransportPath(uint32 pathId)
{
    Movement::MoveSplineInitArgs args;
    args.flags = Movement::MoveSplineFlag(Movement::MoveSplineFlag::Catmullrom);
    args.velocity = ((GameObject*)m_owner)->GetGOInfo()->moTransport.moveSpeed;

    TaxiPathNodeList const& path = sTransportMgr.GetTaxiPathNodeList(pathId);
    uint32 stopDuration = 0;

    Movement::MoveSplineInitArgs args2;
    args2.flags = Movement::MoveSplineFlag(Movement::MoveSplineFlag::Catmullrom);
    args2.velocity = ((GameObject*)m_owner)->GetGOInfo()->moTransport.moveSpeed;
    Movement::MoveSpline* spline2TEST = new Movement::MoveSpline();
    uint32 stopDuration2 = 0;

    for (uint32 i = 0; i < path.size(); ++i)
    {
        if (path[i].mapid == m_owner->GetMap()->GetId())
        {
            args.path.push_back(G3D::Vector3(path[i].x, path[i].y, path[i].z));

            if (path[i].arrivalEventID || path[i].delay || path[i].departureEventID)
            {
                stopDuration += path[i].delay * 1000; // MS Time
                m_transportStops.insert(TransportStopMap::value_type(args.path.size() - 1,
                    TransportStop(path[i].arrivalEventID, path[i].delay * 1000, path[i].departureEventID)));
            }
        }
        else
        {
            args2.path.push_back(G3D::Vector3(path[i].x, path[i].y, path[i].z));

            if (path[i].arrivalEventID || path[i].delay || path[i].departureEventID)
                stopDuration2 += path[i].delay * 1000; // MS Time
        }
    }

    m_moveSpline->Initialize(args);
    spline2TEST->Initialize(args2);

    DEBUG_LOG("DURATION DER STRECKE1: %i", m_moveSpline->Duration() + stopDuration);
    DEBUG_LOG("STOPDURATION1: %u", stopDuration);
    DEBUG_LOG("DURATION DER STRECKE2: %i", spline2TEST->Duration() + stopDuration2);
    DEBUG_LOG("STOPDURATION2: %u", stopDuration2);

    // ToDo: Set period !correctly!
    m_owner->SetUInt32Value(GAMEOBJECT_LEVEL, m_moveSpline->Duration() + stopDuration + spline2TEST->Duration() + stopDuration2);

    delete spline2TEST;
}*/

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
