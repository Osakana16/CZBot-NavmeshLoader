/***
*
*	Copyright (c) 1996-2002, Valve LLC. All rights reserved.
*	
*	This product contains software technology licensed from Id 
*	Software, Inc. ("Id Technology").  Id Technology (c) 1996 Id Software, Inc. 
*	All Rights Reserved.
*
*   Use, distribution, and modification of this source code and/or resulting
*   object code is restricted to non-commercial enhancements to products from
*   Valve LLC.  All other use, distribution, or modification is prohibited
*   without written permission from Valve LLC.
*
****/
#include "navigation_map.h"
#include <vector>
#include <string>

#include <optional>

#include <format>
#include <cassert>
#include <unordered_map>

void UTIL_TraceLine(const Vector& vecStart, const Vector& vecEnd, IGNORE_MONSTERS igmon, edict_t* pentIgnore, TraceResult* ptr) {
	TRACE_LINE(vecStart, vecEnd, igmon, pentIgnore, ptr);
}


void UTIL_TraceLine(const Vector& vecStart, const Vector& vecEnd, IGNORE_MONSTERS igmon, IGNORE_GLASS iglass, edict_t* pentIgnore, TraceResult* ptr) {
	TRACE_LINE(vecStart, vecEnd, igmon | iglass, pentIgnore, ptr);
}

namespace {
	struct Color {
		int r, g, b;
	};

	fpos_t GET_FILE_SIZE(const std::string& Map_Name) {
		fpos_t filesize = -1;
		if (FILE* fp = fopen(("cstrike\\" + Map_Name).c_str(), "rb"); fp != nullptr) {
			fseek(fp, 0, SEEK_END);
			fgetpos(fp, &filesize);
			fclose(fp);
		} else if ((fp = fopen(("czero\\" + Map_Name).c_str(), "rb")) != nullptr) {
			fseek(fp, 0, SEEK_END);
			fgetpos(fp, &filesize);
			fclose(fp);
		}
		return filesize;
	}

	bool InViewCone(edict_t* const self, const Vector& Origin) noexcept {
		MAKE_VECTORS(self->v.angles);
		const auto Vector_2D_Los = (Origin - self->v.origin).Make2D().Normalize();
		const auto Dot = DotProduct(Vector_2D_Los, gpGlobals->v_forward.Make2D());
		return (Dot > 0.50);
	}

	bool IsVisible(edict_t* const self, const Vector& Origin) noexcept {
		// look through caller's eyes
		TraceResult tr;
		UTIL_TraceLine(self->v.origin + self->v.view_ofs, Origin, dont_ignore_monsters, ignore_glass, self, &tr);
		return (tr.flFraction >= 1.0);	// line of sight is not established or valid
	}


	edict_t* FindEntityByString(edict_t* pentStart, const char* szKeyword, const char* szValue) {
		edict_t* pentEntity = FIND_ENTITY_BY_STRING(pentStart, szKeyword, szValue);
		if (!FNullEnt(pentEntity))
			return pentEntity;
		return nullptr;
	}

	edict_t* FindEntityByClassname(edict_t* pentStart, const char* szName) {
		return FindEntityByString(pentStart, "classname", szName);
	}
}

namespace navmesh {
	constexpr float GenerationStepSize = 25.0f;		// (30) was 20, but bots can't fit always fit
	constexpr float StepHeight = 18.0f;						///< if delta Z is greater than this, we have to jump to get up
	constexpr float JumpHeight = 41.8f;						///< if delta Z is less than this, we can jump up on it
	constexpr float JumpCrouchHeight = 58.0f;			///< (48) if delta Z is less than or equal to this, we can jumpcrouch up on it

	// Strictly speaking, you CAN get up a slope of 1.643 (about 59 degrees), but you move very, very slowly
	// This slope will represent the slope you can navigate without much slowdown
	constexpr float MaxSlope = 1.4f;							///< rise/run - if greater than this, we can't move up it (de_survivor canyon ramps)

	// instead of MaxSlope, we are using the following max Z component of a unit normal
	constexpr float MaxUnitZSlope = 0.7f;

	constexpr float BotRadius = 10.0f;						///< circular extent that contains bot
	constexpr float DeathDrop = 200.0f;						///< (300) distance at which we will die if we fall - should be about 600, and pay attention to fall damage during pathfind

	constexpr float HalfHumanWidth = 16.0f;
	constexpr float HalfHumanHeight = 36.0f;
	constexpr float HumanHeight = 72.0f;

	//--------------------------------------------------------------------------------------------------------------
	/**
	 * Return the ground height below this point in "height".
	 * Return false if position is invalid (outside of map, in a solid area, etc).
	 */
	bool GetGroundHeight(const Vector* pos, float* height, Vector* normal = nullptr) {
		Vector to{ pos->x, pos->y, pos->z - 9999.9f };

		float offset;
		Vector from;
		TraceResult result;
		edict_t* ignore = nullptr;
		float ground = 0.0f;

		constexpr float maxOffset = 100.0f;
		constexpr float inc = 10.0f;

		constexpr auto MAX_GROUND_LAYERS = 16;
		struct GroundLayerInfo {
			float ground{};
			Vector normal{};
		} layer[MAX_GROUND_LAYERS]{};
		int layerCount = 0;

		for (offset = 1.0f; offset < maxOffset; offset += inc) {
			from = *pos + Vector(0, 0, offset);
			UTIL_TraceLine(from, to, ignore_monsters, dont_ignore_glass, ignore, &result);

			// if the trace came down thru a door, ignore the door and try again
			// also ignore breakable floors
			if (result.pHit) {
				if (FClassnameIs(VARS(result.pHit), "func_door") ||
					FClassnameIs(VARS(result.pHit), "func_door_rotating") ||
					(FClassnameIs(VARS(result.pHit), "func_breakable") && VARS(result.pHit)->takedamage == DAMAGE_YES)) {
					ignore = result.pHit;
					// keep incrementing to avoid infinite loop if more than one entity is along the traceline...
					/// @todo Deal with multiple ignore entities in a single TraceLine()
					//offset -= inc;
					continue;
				}
			}

			if (result.fStartSolid == false) {
				// if we didnt start inside a solid area, the trace hit a ground layer

				// if this is a new ground layer, add it to the set
				if (layerCount == 0 || result.vecEndPos.z > layer[layerCount - 1].ground) {
					layer[layerCount].ground = result.vecEndPos.z;
					layer[layerCount].normal = result.vecPlaneNormal;
					++layerCount;

					if (layerCount == MAX_GROUND_LAYERS)
						break;
				}
			}
		}

		if (layerCount == 0)
			return false;

		// find the lowest layer that allows a player to stand or crouch upon it
		int i{};
		for (i = 0; i < layerCount - 1; ++i) {
			if (layer[i + 1].ground - layer[i].ground >= HalfHumanHeight)
				break;
		}

		*height = layer[i].ground;

		if (normal)
			*normal = layer[i].normal;

		return true;
	}

	int NavAreaGrid::ComputeHashKey(unsigned int id) const {
		return id & 0xFF;
	}

	int NavAreaGrid::WorldToGridX(float wx) const {
		int x = (wx - m_minX) / m_cellSize;
		if (x < 0)
			x = 0;
		else if (x >= m_gridSizeX)
			x = m_gridSizeX - 1;

		return x;
	}

	int NavAreaGrid::WorldToGridY(float wy) const {
		int y = (wy - m_minY) / m_cellSize;
		if (y < 0)
			y = 0;
		else if (y >= m_gridSizeY)
			y = m_gridSizeY - 1;

		return y;
	}

	void NavArea::ComputePortal(const NavArea* to, NavDirType dir, Vector* center, float* halfWidth) const {
		if (dir == NORTH || dir == SOUTH) {
			if (dir == NORTH)
				center->y = m_extent.lo.y;
			else
				center->y = m_extent.hi.y;

			float left = max(m_extent.lo.x, to->m_extent.lo.x);
			float right = min(m_extent.hi.x, to->m_extent.hi.x);

			// clamp to our extent in case areas are disjoint
			if (left < m_extent.lo.x)
				left = m_extent.lo.x;
			else if (left > m_extent.hi.x)
				left = m_extent.hi.x;

			if (right < m_extent.lo.x)
				right = m_extent.lo.x;
			else if (right > m_extent.hi.x)
				right = m_extent.hi.x;

			center->x = (left + right) / 2.0f;
			*halfWidth = (right - left) / 2.0f;
		} else	// EAST or WEST
		{
			if (dir == WEST)
				center->x = m_extent.lo.x;
			else
				center->x = m_extent.hi.x;

			float top = max(m_extent.lo.y, to->m_extent.lo.y);
			float bottom = min(m_extent.hi.y, to->m_extent.hi.y);

			// clamp to our extent in case areas are disjoint
			if (top < m_extent.lo.y)
				top = m_extent.lo.y;
			else if (top > m_extent.hi.y)
				top = m_extent.hi.y;

			if (bottom < m_extent.lo.y)
				bottom = m_extent.lo.y;
			else if (bottom > m_extent.hi.y)
				bottom = m_extent.hi.y;

			center->y = (top + bottom) / 2.0f;
			*halfWidth = (bottom - top) / 2.0f;
		}
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
	* Return true if 'pos' is within 2D extents of area.
	*/
	bool NavArea::IsOverlapping(const Vector* pos) const noexcept {
		return (pos->x >= m_extent.lo.x && pos->x <= m_extent.hi.x &&
				pos->y >= m_extent.lo.y && pos->y <= m_extent.hi.y);
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
	* Return true if 'area' overlaps our 2D extents
	*/
	bool NavArea::IsOverlapping(const NavArea* area) const noexcept {
		return (area->m_extent.lo.x < m_extent.hi.x && area->m_extent.hi.x > m_extent.lo.x &&
				area->m_extent.lo.y < m_extent.hi.y && area->m_extent.hi.y > m_extent.lo.y);
	}

	/**
	 * Given a HidingSpot ID, return the associated HidingSpot
	 */
	HidingSpot* NavigationMap::GetHidingSpotByID(std::uint32_t id) {
		for (auto& spot : m_hidingSpots) {
			if (spot->m_id == id)
				return spot;
		}
		return nullptr;
	}

	HidingSpot::HidingSpot(NavigationMap* mesh) {
		m_pos = Vector(0, 0, 0);
		m_id = 0;
		m_flags = 0;

		mesh->AddHidingSpots(this);
	}

	/**
		* For use when generating - assigns unique ID
		*/
	HidingSpot::HidingSpot(NavigationMap* mesh, const Vector* pos, unsigned char flags) {
		m_pos = *pos;
		m_id = m_nextID++;
		m_flags = flags;
		
		mesh->AddHidingSpots(this);
	}

	void PlaceDirectory::Reset(void) {
		m_directory.clear();
	}

	/// return true if this place is already in the directory
	bool PlaceDirectory::IsKnown(Place place) const {
		std::vector<Place>::const_iterator it = std::find(m_directory.begin(), m_directory.end(), place);

		return (it != m_directory.end());
	}

	/// return the directory entry corresponding to this Place (0 = no entry)
	PlaceDirectory::EntryType PlaceDirectory::GetEntry(Place place) const {
		if (place == Undefined_Place)
			return 0;

		std::vector<Place>::const_iterator it = std::find(m_directory.begin(), m_directory.end(), place);
		if (it == m_directory.end()) {
			assert(false && "PlaceDirectory::GetEntry failure");
			return 0;
		}

		return 1 + (it - m_directory.begin());
	}

	/// add the place to the directory if not already known
	void PlaceDirectory::AddPlace(Place place) {
		if (place == Undefined_Place)
			return;

		assert(place < 1000);

		if (IsKnown(place))
			return;

		m_directory.push_back(place);
	}

	/// given an entry, return the Place
	Place PlaceDirectory::EntryToPlace(EntryType entry) const {
		if (entry == 0)
			return Undefined_Place;

		int i = entry - 1;

		if (i > m_directory.size()) {
			assert(false && "PlaceDirectory::EntryToPlace: Invalid entry");
			return Undefined_Place;
		}

		return m_directory[i];
	}

	void NavigationMap::DestroyLadders() {
		while (!m_navLadders.empty()) {
			NavLadder* ladder = m_navLadders.front();
			m_navLadders.pop_front();
			delete ladder;
		}
	}


	//--------------------------------------------------------------------------------------------------------------

	/**
	 * The singleton for accessing the grid
	 */
	NavAreaGrid::NavAreaGrid(void) : m_cellSize(300.0f) {
		m_grid = nullptr;
		Reset();
	}

	NavAreaGrid::~NavAreaGrid() {
		delete[] m_grid;
		m_grid = nullptr;
	}


	void AddDirectionVector(Vector* v, NavDirType dir, float amount) {
		switch (dir) {
			case NORTH: v->y -= amount; return;
			case SOUTH: v->y += amount; return;
			case EAST:  v->x += amount; return;
			case WEST:  v->x -= amount; return;
			default: break;
		}
	}

	//--------------------------------------------------------------------------------------------------------------
	inline NavDirType OppositeDirection(NavDirType dir) {
		switch (dir) {
			case NORTH: return SOUTH;
			case SOUTH: return NORTH;
			case EAST:	return WEST;
			case WEST:	return EAST;
		}
		return NORTH;
	}

	//--------------------------------------------------------------------------------------------------------------
	inline NavDirType DirectionLeft(NavDirType dir) {
		switch (dir) {
			case NORTH: return WEST;
			case SOUTH: return EAST;
			case EAST:	return NORTH;
			case WEST:	return SOUTH;
		}

		return NORTH;
	}

	//--------------------------------------------------------------------------------------------------------------
	NavDirType DirectionRight(NavDirType dir) {
		switch (dir) {
			case NORTH: return EAST;
			case SOUTH: return WEST;
			case EAST:	return SOUTH;
			case WEST:	return NORTH;
		}

		return NORTH;
	}

	//--------------------------------------------------------------------------------------------------------------
	void DirectionToVector2D(NavDirType dir, Vector2D* v) {
		switch (dir) {
			case NORTH: v->x = 0.0f; v->y = -1.0f; break;
			case SOUTH: v->x = 0.0f; v->y = 1.0f; break;
			case EAST:  v->x = 1.0f; v->y = 0.0f; break;
			case WEST:  v->x = -1.0f; v->y = 0.0f; break;
		}
	}

	//--------------------------------------------------------------------------------------------------------------
	float DirectionToAngle(NavDirType dir) {
		switch (dir) {
			case NORTH:	return 270.0f;
			case SOUTH:	return 90.0f;
			case EAST:	return 0.0f;
			case WEST:	return 180.0f;
		}

		return 0.0f;
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
	 * Start at given position and find first area in given direction
	 */
	NavArea* NavigationMap::FindFirstAreaInDirection(const Vector* start, NavDirType dir, float range, float beneathLimit, edict_t* traceIgnore, Vector* closePos) {
		Vector pos = *start;

		int end = (int)((range / GenerationStepSize) + 0.5f);
		NavArea* area{};
		for (int i = 1; i <= end; i++) {
			AddDirectionVector(&pos, dir, GenerationStepSize);

			// make sure we dont look thru the wall
			TraceResult result;

			if (traceIgnore)
				UTIL_TraceLine(*start, pos, ignore_monsters, traceIgnore, &result);
			else
				UTIL_TraceLine(*start, pos, ignore_monsters, nullptr, &result);

			if (result.flFraction != 1.0f)
				break;

			if (area = m_navAreaGrid.GetNavArea(&pos, beneathLimit); area != nullptr) {
				if (closePos) {
					closePos->x = pos.x;
					closePos->y = pos.y;
					closePos->z = area->GetZ(pos.x, pos.y);
				}
				break;
			}
		}
		return area;
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
	* Constructor used during normal runtime.
	*/
	NavArea::NavArea(void) {
		Initialize();
	}

	void NavArea::Initialize(void) {
		m_marker = 0;
		m_parent = nullptr;
		m_parentHow = GO_NORTH;
		m_attributeFlags = 0;
		m_place = 0;

		for (int i = 0; i < MAX_AREA_TEAMS; ++i) {
			m_danger[i] = 0.0f;
			m_dangerTimestamp[i] = 0.0f;

			m_clearedTimestamp[i] = 0.0f;
		}

		m_approachCount = 0;

		// set an ID for splitting and other interactive editing - loads will overwrite this
		m_id = m_nextID++;

		m_prevHash = nullptr;
		m_nextHash = nullptr;
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
		* Return closest point to 'pos' on 'area'.
		* Returned point is in 'close'.
		*/
	void NavArea::GetClosestPointOnArea(const Vector* pos, Vector* close) const {
		const Extent* extent = &m_extent;

		if (pos->x < extent->lo.x) {
			if (pos->y < extent->lo.y) {
				// position is north-west of area
				*close = extent->lo;
			} else if (pos->y > extent->hi.y) {
				// position is south-west of area
				close->x = extent->lo.x;
				close->y = extent->hi.y;
			} else {
				// position is west of area
				close->x = extent->lo.x;
				close->y = pos->y;
			}
		} else if (pos->x > extent->hi.x) {
			if (pos->y < extent->lo.y) {
				// position is north-east of area
				close->x = extent->hi.x;
				close->y = extent->lo.y;
			} else if (pos->y > extent->hi.y) {
				// position is south-east of area
				*close = extent->hi;
			} else {
				// position is east of area
				close->x = extent->hi.x;
				close->y = pos->y;
			}
		} else if (pos->y < extent->lo.y) {
			// position is north of area
			close->x = pos->x;
			close->y = extent->lo.y;
		} else if (pos->y > extent->hi.y) {
			// position is south of area
			close->x = pos->x;
			close->y = extent->hi.y;
		} else {
			// position is inside of area - it is the 'closest point' to itself
			*close = *pos;
		}
		close->z = GetZ(close);
	}

	float NavArea::GetZ(const Vector* pos) const {
		float dx = m_extent.hi.x - m_extent.lo.x;
		float dy = m_extent.hi.y - m_extent.lo.y;

		// guard against division by zero due to degenerate areas
		if (dx == 0.0f || dy == 0.0f)
			return m_neZ;

		float u = (pos->x - m_extent.lo.x) / dx;
		float v = (pos->y - m_extent.lo.y) / dy;

		// clamp Z values to (x,y) volume
		if (u < 0.0f)
			u = 0.0f;
		else if (u > 1.0f)
			u = 1.0f;

		if (v < 0.0f)
			v = 0.0f;
		else if (v > 1.0f)
			v = 1.0f;

		float northZ = m_extent.lo.z + u * (m_neZ - m_extent.lo.z);
		float southZ = m_swZ + u * (m_extent.hi.z - m_swZ);

		return northZ + v * (southZ - northZ);
	}

	float NavArea::GetZ(float x, float y) const {
		Vector pos(x, y, 0.0f);
		return GetZ(&pos);
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
	 * For each ladder in the map, create a navigation representation of it.
	 */
	void NavigationMap::BuildLadders() {
		// remove any left-over ladders
		DestroyLadders();

		TraceResult result;
		edict_t* entity = FindEntityByClassname(nullptr, "func_ladder");
		while (entity) {
			NavLadder* ladder = new NavLadder;

			// compute top & bottom of ladder
			ladder->m_top.x = (entity->v.absmin.x + entity->v.absmax.x) / 2.0f;
			ladder->m_top.y = (entity->v.absmin.y + entity->v.absmax.y) / 2.0f;
			ladder->m_top.z = entity->v.absmax.z;

			ladder->m_bottom.x = ladder->m_top.x;
			ladder->m_bottom.y = ladder->m_top.y;
			ladder->m_bottom.z = entity->v.absmin.z;

			// determine facing - assumes "normal" runged ladder
			float xSize = entity->v.absmax.x - entity->v.absmin.x;
			float ySize = entity->v.absmax.y - entity->v.absmin.y;
			if (xSize > ySize) {
				// ladder is facing north or south - determine which way
				// "pull in" traceline from bottom and top in case ladder abuts floor and/or ceiling
				const Vector from = ladder->m_bottom + Vector(0.0f, GenerationStepSize, GenerationStepSize);
				const Vector to = ladder->m_top + Vector(0.0f, GenerationStepSize, -GenerationStepSize);

				UTIL_TraceLine(from, to, ignore_monsters, ENT(entity), &result);

				if (result.flFraction != 1.0f || result.fStartSolid)
					ladder->m_dir = NORTH;
				else
					ladder->m_dir = SOUTH;
			} else {
				// ladder is facing east or west - determine which way
				const Vector from = ladder->m_bottom + Vector(GenerationStepSize, 0.0f, GenerationStepSize);
				const Vector to = ladder->m_top + Vector(GenerationStepSize, 0.0f, -GenerationStepSize);

				UTIL_TraceLine(from, to, ignore_monsters, ENT(entity), &result);

				if (result.flFraction != 1.0f || result.fStartSolid)
					ladder->m_dir = WEST;
				else
					ladder->m_dir = EAST;
			}

			// adjust top and bottom of ladder to make sure they are reachable
			// (cs_office has a crate right in front of the base of a ladder)
			const Vector along = (ladder->m_top - ladder->m_bottom).Normalize();
			const float length = along.Length();
			Vector on, out;
			constexpr float minLadderClearance = 32.0f;

			// adjust bottom to bypass blockages
			constexpr float inc = 10.0f;
			for (float t = 0.0f; t <= length; t += inc) {
				on = ladder->m_bottom + t * along;

				out = on;
				AddDirectionVector(&out, ladder->m_dir, minLadderClearance);

				UTIL_TraceLine(on, out, ignore_monsters, ENT(entity), &result);

				if (result.flFraction == 1.0f && !result.fStartSolid) {
					// found viable ladder bottom
					ladder->m_bottom = on;
					break;
				}
			}

			// adjust top to bypass blockages
			for (float t = 0.0f; t <= length; t += inc) {
				on = ladder->m_top - t * along;

				out = on;
				AddDirectionVector(&out, ladder->m_dir, minLadderClearance);

				UTIL_TraceLine(on, out, ignore_monsters, ENT(entity), &result);

				if (result.flFraction == 1.0f && !result.fStartSolid) {
					// found viable ladder top
					ladder->m_top = on;
					break;
				}
			}

			ladder->m_length = (ladder->m_top - ladder->m_bottom).Length();

			DirectionToVector2D(ladder->m_dir, &ladder->m_dirVector);

			ladder->m_entity = entity;
			const float nearLadderRange = 75.0f;		// 50

			//
			// Find naviagtion area at bottom of ladder
			//

			// get approximate postion of player on ladder
			Vector center = ladder->m_bottom + Vector(0, 0, GenerationStepSize);
			AddDirectionVector(&center, ladder->m_dir, HalfHumanWidth);

			ladder->m_bottomArea = m_navAreaGrid.GetNearestNavArea(this, &center, true);
			if (!ladder->m_bottomArea) {
				ALERT(at_console, "ERROR: Unconnected ladder bottom at ( %g, %g, %g )\n", ladder->m_bottom.x, ladder->m_bottom.y, ladder->m_bottom.z);
			} else {
				// store reference to ladder in the area
				ladder->m_bottomArea->AddLadderUp(ladder);
			}

			//
			// Find adjacent navigation areas at the top of the ladder
			//

			// get approximate postion of player on ladder
			center = ladder->m_top + Vector(0, 0, GenerationStepSize);
			AddDirectionVector(&center, ladder->m_dir, HalfHumanWidth);

			// find "ahead" area
			ladder->m_topForwardArea = FindFirstAreaInDirection(&center, OppositeDirection(ladder->m_dir), nearLadderRange, 120.0f, entity);
			if (ladder->m_topForwardArea == ladder->m_bottomArea)
				ladder->m_topForwardArea = nullptr;

			// find "left" area
			ladder->m_topLeftArea = FindFirstAreaInDirection(&center, DirectionLeft(ladder->m_dir), nearLadderRange, 120.0f, entity);
			if (ladder->m_topLeftArea == ladder->m_bottomArea)
				ladder->m_topLeftArea = nullptr;

			// find "right" area
			ladder->m_topRightArea = FindFirstAreaInDirection(&center, DirectionRight(ladder->m_dir), nearLadderRange, 120.0f, entity);
			if (ladder->m_topRightArea == ladder->m_bottomArea)
				ladder->m_topRightArea = nullptr;

			// find "behind" area - must look farther, since ladder is against the wall away from this area
			ladder->m_topBehindArea = FindFirstAreaInDirection(&center, ladder->m_dir, 2.0f * nearLadderRange, 120.0f, entity);
			if (ladder->m_topBehindArea == ladder->m_bottomArea)
				ladder->m_topBehindArea = nullptr;

			// can't include behind area, since it is not used when going up a ladder
			if (!ladder->m_topForwardArea && !ladder->m_topLeftArea && !ladder->m_topRightArea)
				ALERT(at_console, "ERROR: Unconnected ladder top at ( %g, %g, %g )\n", ladder->m_top.x, ladder->m_top.y, ladder->m_top.z);

			// store reference to ladder in the area(s)
			if (ladder->m_topForwardArea)
				ladder->m_topForwardArea->AddLadderDown(ladder);

			if (ladder->m_topLeftArea)
				ladder->m_topLeftArea->AddLadderDown(ladder);

			if (ladder->m_topRightArea)
				ladder->m_topRightArea->AddLadderDown(ladder);

			if (ladder->m_topBehindArea)
				ladder->m_topBehindArea->AddLadderDown(ladder);

			// adjust top of ladder to highest connected area
			float topZ = -99999.9f;
			bool topAdjusted = false;
			NavArea* topAreaList[4]{ ladder->m_topForwardArea,ladder->m_topLeftArea, ladder->m_topRightArea, ladder->m_topBehindArea };

			for (int a = 0; a < 4; ++a) {
				NavArea* topArea = topAreaList[a];
				if (topArea == nullptr)
					continue;

				Vector close;
				topArea->GetClosestPointOnArea(&ladder->m_top, &close);
				if (topZ < close.z) {
					topZ = close.z;
					topAdjusted = true;
				}
			}

			if (topAdjusted)
				ladder->m_top.z = topZ;

			//
			// Determine whether this ladder is "dangling" or not
			// "Dangling" ladders are too high to go up
			//
			ladder->m_isDangling = false;
			if (ladder->m_bottomArea) {
				Vector bottomSpot;
				ladder->m_bottomArea->GetClosestPointOnArea(&ladder->m_bottom, &bottomSpot);
				if (ladder->m_bottom.z - bottomSpot.z > HumanHeight)
					ladder->m_isDangling = true;
			}

			// add ladder to global list
			m_navLadders.push_back(ladder);

			entity = FindEntityByClassname(entity, "func_ladder");
		}
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
	 * Load AI navigation data from a file
	 */
	bool NavigationMap::Load(const std::string& Path_To_Nav) {
		// free previous navigation map data
		// TODO: Destroy current navigation meshes.

		if (FILE* fp = fopen(Path_To_Nav.c_str(), "rb"); fp != nullptr) {
			Destroy();
			NavArea::m_nextID = 1;

			// check magic number
			struct { std::uint32_t magic, version; } header;
			fread(&header, sizeof header, 1, fp);

			if (header.version >= 4) {
				// get size of source bsp file and verify that the bsp hasn't changed
				std::uint32_t saveBspSize{};
				fread(&saveBspSize, sizeof(std::uint32_t), 1, fp);

				// verify size
				std::string bspFilename = std::format("maps\\{}.bsp", STRING(gpGlobals->mapname));
				std::uint32_t bspSize = (std::uint32_t)GET_FILE_SIZE(bspFilename);

				if (bspSize != saveBspSize) {
					// this nav file is out of date for this bsp file
					const char* msg = "*** WARNING ***\nThe AI navigation data is from a different version of this map.\nThe CPU players will likely not perform well.\n";
					SERVER_PRINT("\n-----------------\n");
					SERVER_PRINT(msg);
					SERVER_PRINT("-----------------\n\n");
				}
			}

			// load Place directory
			if (header.version >= 5) {
				std::unordered_map<std::string, Place> place_id{
					{"BombsiteA",1},
					{"BombsiteB",2},
					{"BombsiteC",3},
					{"Hostages",4},
					{"HostageRescueZone",5},
					{"VipRescueZone",6},
					{"CTSpawn",7},
					{"TSpawn",8},
					{"Bridge",9},
					{"Middle",10},
					{"House",11},
					{"Apartment",12},
					{"Apartments",13},
					{"Market",14},
					{"Sewers",15},
					{"Tunnel",16},
					{"Ducts",17},
					{"Village",18},
					{"Roof",19},
					{"Upstairs",20},
					{"Downstairs",21},
					{"Basement",22},
					{"Crawlspace",23},
					{"Kitchen",24},
					{"Inside",25},
					{"Outside",26},
					{"Tower",27},
					{"WineCellar",28},
					{"Garage",29},
					{"Courtyard",30},
					{"Water",31},
					{"FrontDoor",32},
					{"BackDoor",33},
					{"SideDoor",34},
					{"BackWay",35},
					{"FrontYard",36},
					{"BackYard",37},
					{"SideYard",38},
					{"Lobby",39},
					{"Vault",40},
					{"Elevator",41},
					{"DoubleDoors",42},
					{"SecurityDoors",43},
					{"LongHall",44},
					{"SideHall",45},
					{"FrontHall",46},
					{"BackHall",47},
					{"MainHall",48},
					{"FarSide",49},
					{"Windows",50},
					{"Window",51},
					{"Attic",52},
					{"StorageRoom",53},
					{"ProjectorRoom",54},
					{"MeetingRoom",55},
					{"ConferenceRoom",56},
					{"ComputerRoom",57},
					{"BigOffice",58},
					{"LittleOffice",59},
					{"Dumpster",60},
					{"Airplane",61},
					{"Underground",62},
					{"Bunker",63},
					{"Mines",64},
					{"Front",65},
					{"Back",66},
					{"Rear",67},
					{"Side",68},
					{"Ramp",69},
					{"Underpass",70},
					{"Overpass",71},
					{"Stairs",72},
					{"Ladder",73},
					{"Gate",74},
					{"GateHouse",75},
					{"LoadingDock",76},
					{"GuardHouse",77},
					{"Entrance",78},
					{"VendingMachines",79},
					{"Loft",80},
					{"Balcony",81},
					{"Alley",82},
					{"BackAlley",83},
					{"SideAlley",84},
					{"FrontRoom",85},
					{"BackRoom",86},
					{"SideRoom",87},
					{"Crates",88},
					{"Truck",89},
					{"Bedroom",90},
					{"FamilyRoom",91},
					{"Bathroom",92},
					{"LivingRoom",93},
					{"Den",94},
					{"Office",95},
					{"Atrium",96},
					{"Entryway",97},
					{"Foyer",98},
					{"Stairwell",99},
					{"Fence",100},
					{"Deck",101},
					{"Porch",102},
					{"Patio",103},
					{"Wall",104}
				};

				// read number of entries
				PlaceDirectory::EntryType count;
				fread(&count, sizeof(PlaceDirectory::EntryType), 1, fp);
				m_placeDirectory.Reserve(count);

				// read each entry
				for (uint32_t i = 0; i < count; ++i) {
					std::uint16_t len{};
					fread(&len, sizeof(std::uint16_t), 1, fp);

					char placeName[256];
					fread(placeName, sizeof(char), len, fp);

					if (auto it = place_id.find(placeName); it != place_id.end()) {
						m_placeDirectory.AddPlace(it->second);
					}
				}
			}

			// get number of areas
			std::uint32_t count{};
			fread(&count, sizeof(std::uint32_t), 1, fp);

			Extent extent;
			extent.lo.x = 9999999999.9f;
			extent.lo.y = 9999999999.9f;
			extent.hi.x = -9999999999.9f;
			extent.hi.y = -9999999999.9f;

			// load the areas and compute total extent
			for (std::uint32_t i = 0; i < count; ++i) {
				NavArea* area = new NavArea();
				// load ID
				fread(&area->m_id, sizeof(std::uint32_t), 1, fp);

				// update nextID to avoid collisions
				if (area->m_id >= NavArea::m_nextID)
					NavArea::m_nextID = area->m_id + 1;

				// load attribute flags
				fread(&area->m_attributeFlags, sizeof(std::uint8_t), 1, fp);

				// load extent of area
				fread(&area->m_extent, sizeof(Extent), 1, fp);

				area->m_center.x = (area->m_extent.lo.x + area->m_extent.hi.x) / 2.0f;
				area->m_center.y = (area->m_extent.lo.y + area->m_extent.hi.y) / 2.0f;
				area->m_center.z = (area->m_extent.lo.z + area->m_extent.hi.z) / 2.0f;

				// load heights of implicit corners
				fread(&area->m_neZ, sizeof(float), 1, fp);
				fread(&area->m_swZ, sizeof(float), 1, fp);

				// load connections (IDs) to adjacent areas
				// in the enum order NORTH, EAST, SOUTH, WEST
				for (int d = 0; d < NUM_DIRECTIONS; d++) {
					// load number of connections for this direction
					std::uint32_t count{};
					fread(&count, sizeof(std::uint32_t), 1, fp);
					for (std::uint32_t j = 0; j < count; ++j) {
						NavConnect connect{};
						fread(&connect.id, sizeof(std::uint32_t), 1, fp);
						area->m_connect[d].push_back(connect);
					}
				}

				//
				// Load hiding spots
				//

				// load number of hiding spots
				std::uint8_t hidingSpotCount{};
				fread(&hidingSpotCount, sizeof(std::uint8_t), 1, fp);

				if (header.version == 1) {
					// load simple vector array
					Vector pos;
					for (int h = 0; h < hidingSpotCount; ++h) {
						fread(&pos, sizeof(float), 3, fp);

						// create new hiding spot and put on master list
						HidingSpot* spot = new HidingSpot(this, &pos, HidingSpot::IN_COVER);
						area->hiding_spots.push_back(spot);
					}
				} else {
					// load HidingSpot objects for this area
					for (int h = 0; h < hidingSpotCount; ++h) {
						// create new hiding spot and put on master list
						HidingSpot* spot = new HidingSpot(this);
						fread(&spot->m_id, sizeof(unsigned int), 1, fp);
						fread(&spot->m_pos, sizeof(Vector), 1, fp);
						fread(&spot->m_flags, sizeof(unsigned char), 1, fp);

						// update next ID to avoid ID collisions by later spots
						if (spot->m_id >= spot->m_nextID)
							spot->m_nextID = spot->m_id + 1;
						area->hiding_spots.push_back(spot);
					}
				}

				//
				// Load number of approach areas
				//
				fread(&area->m_approachCount, sizeof(std::uint8_t), 1, fp);

				// load approach area info (IDs)
				std::uint8_t type;
				for (int a = 0; a < area->m_approachCount; ++a) {
					fread(&area->m_approach[a].here.id, sizeof(std::uint32_t), 1, fp);
					fread(&area->m_approach[a].prev.id, sizeof(std::uint32_t), 1, fp);
					fread(&type, sizeof(std::uint8_t), 1, fp);
					area->m_approach[a].prevToHereHow = (NavTraverseType)type;

					fread(&area->m_approach[a].next.id, sizeof(std::uint32_t), 1, fp);
					fread(&type, sizeof(std::uint8_t), 1, fp);
					area->m_approach[a].hereToNextHow = (NavTraverseType)type;
				}
				//
				// Load encounter paths for this area
				//
				std::uint32_t count;
				fread(&count, sizeof(std::uint32_t), 1, fp);

				for (std::uint32_t e = 0; e < count; ++e) {
					SpotEncounter encounter;
					fread(&encounter.from.id, sizeof(std::uint32_t), 1, fp);
					std::uint8_t dir;
					fread(&dir, sizeof(std::uint8_t), 1, fp);
					encounter.fromDir = static_cast<NavDirType>(dir);

					fread(&encounter.to.id, sizeof(std::uint32_t), 1, fp);
					fread(&dir, sizeof(std::uint8_t), 1, fp);
					encounter.toDir = static_cast<NavDirType>(dir);

					// read list of spots along this path
					std::uint8_t spotCount;
					fread(&spotCount, sizeof(std::uint8_t), 1, fp);

					SpotOrder order;
					for (int s = 0; s < spotCount; ++s) {
						fread(&order.id, sizeof(std::uint32_t), 1, fp);

						std::uint8_t t;
						fread(&t, sizeof(std::uint8_t), 1, fp);
						order.t = (float)t / 255.0f;
						encounter.spotList.push_back(order);
					}
					area->encounter_spots.push_back(encounter);
				}

				//
				// Load Place data
				//
				std::uint16_t entry{};
				fread(&entry, sizeof(entry), 1, fp);

				// convert entry to actual Place
				area->m_place = m_placeDirectory.EntryToPlace(entry);
				m_areas.push_back(area);

				const Extent* areaExtent = &area->m_extent;

				if (areaExtent->lo.x < extent.lo.x)
					extent.lo.x = areaExtent->lo.x;
				if (areaExtent->lo.y < extent.lo.y)
					extent.lo.y = areaExtent->lo.y;
				if (areaExtent->hi.x > extent.hi.x)
					extent.hi.x = areaExtent->hi.x;
				if (areaExtent->hi.y > extent.hi.y)
					extent.hi.y = areaExtent->hi.y;
			}

			// add the areas to the grid
			m_navAreaGrid.Initialize(extent.lo.x, extent.hi.x, extent.lo.y, extent.hi.y);

			for (auto& area : m_areas) {
				m_navAreaGrid.AddNavArea(area);
			}
			// allow areas to connect to each other, etc
			for (auto& area : m_areas) {
				Validate(area);
			}

			//
			// Set up all the ladders
			//
			BuildLadders();

			fclose(fp);
			return true;
		} else {
			return false;
		}

	}

	void NavigationMap::Destroy() {
		// remove each element of the list and delete them
		while (!m_areas.empty()) {
			auto area = m_areas.front();
			m_areas.pop_front();
			delete area;
		}

		// destroy ladder representations
		DestroyLadders();

		// destroy all hiding spots
		DestroyHidingSpots();
		// reset the grid
		m_navAreaGrid.Reset();
	}

	void NavigationMap::Validate(NavArea* area) {
		// connect areas together
		for (int d = 0; d < NUM_DIRECTIONS; d++) {
			for (auto connection = area->m_connect[d].begin(); connection != area->m_connect[d].end(); ++connection) {
				NavConnect* connect = &(*connection);

				unsigned int id = connect->id;
				connect->area = m_navAreaGrid.GetNavAreaByID(id);
				if (id && connect->area == nullptr) {
					SERVER_PRINT("ERROR: Corrupt navigation data. Cannot connect Navigation Areas.\n");
				}
			}
		}

		// resolve approach area IDs
		for (int a = 0; a < area->m_approachCount; ++a) {
			area->m_approach[a].here.area = m_navAreaGrid.GetNavAreaByID(area->m_approach[a].here.id);
			if (area->m_approach[a].here.id && area->m_approach[a].here.area == nullptr) {
				SERVER_PRINT("ERROR: Corrupt navigation data. Missing Approach Area (here).\n");
			}

			area->m_approach[a].prev.area = m_navAreaGrid.GetNavAreaByID(area->m_approach[a].prev.id);
			if (area->m_approach[a].prev.id && area->m_approach[a].prev.area == nullptr) {
				SERVER_PRINT("ERROR: Corrupt navigation data. Missing Approach Area (prev).\n");
			}

			area->m_approach[a].next.area = m_navAreaGrid.GetNavAreaByID(area->m_approach[a].next.id);
			if (area->m_approach[a].next.id && area->m_approach[a].next.area == nullptr) {
				SERVER_PRINT("ERROR: Corrupt navigation data. Missing Approach Area (next).\n");
			}
		}

		// resolve spot encounter IDs
		for (auto spotIter = area->encounter_spots.begin(); spotIter != area->encounter_spots.end(); ++spotIter) {
			SpotEncounter* e = &(*spotIter);
			e->from.area = m_navAreaGrid.GetNavAreaByID(e->from.id);
			if (e->from.area  == nullptr) {
				SERVER_PRINT("ERROR: Corrupt navigation data. Missing \"from\" Navigation Area for Encounter Spot.\n");
			}

			e->to.area = m_navAreaGrid.GetNavAreaByID(e->to.id);
			if (e->to.area == nullptr) {
				SERVER_PRINT("ERROR: Corrupt navigation data. Missing \"to\" Navigation Area for Encounter Spot.\n");
			}

			if (e->from.area && e->to.area) {
				// compute path
				float halfWidth;
				area->ComputePortal(e->to.area, e->toDir, &e->path.to, &halfWidth);
				area->ComputePortal(e->from.area, e->fromDir, &e->path.from, &halfWidth);

				const float eyeHeight = HalfHumanHeight;
				e->path.from.z = e->from.area->GetZ(&e->path.from) + eyeHeight;
				e->path.to.z = e->to.area->GetZ(&e->path.to) + eyeHeight;
			}

			// resolve HidingSpot IDs
			for (auto oiter = e->spotList.begin(); oiter != e->spotList.end(); ++oiter) {
				SpotOrder* order = &(*oiter);

				order->spot = GetHidingSpotByID(order->id);
				if (order->spot == nullptr) {
					SERVER_PRINT("ERROR: Corrupt navigation data. Missing Hiding Spot\n");
				}
			}
		}

		// build overlap list
		/// @todo Optimize this
		for (auto oiter = m_areas.begin(); oiter != m_areas.end(); ++oiter) {
			NavArea* area2 = *oiter;

			if (area2 == area)
				continue;

			if (area->IsOverlapping(area2))
				area->m_overlapList.push_back(area2);
		}
	}

	/**
	 * Clear the grid
	 */
	void NavAreaGrid::Reset(void) {
		if (m_grid)
			delete[] m_grid;

		m_grid = nullptr;
		m_gridSizeX = 0;
		m_gridSizeY = 0;

		// clear the hash table
		for (int i = 0; i < HASH_TABLE_SIZE; ++i)
			m_hashTable[i] = nullptr;

		m_areaCount = 0;
	}

	/**
	 * Allocate the grid and define its extents
	 */
	void NavAreaGrid::Initialize( float minX, float maxX, float minY, float maxY )
	{
		if (m_grid)
			Reset();

		m_minX = minX;
		m_minY = minY;

		m_gridSizeX = ((maxX - minX) / m_cellSize) + 1;
		m_gridSizeY = ((maxY - minY) / m_cellSize) + 1;

		m_grid = new std::list<NavArea*>[ m_gridSizeX * m_gridSizeY ];
	}

	/**
	 * Add an area to the grid
	 */
	void NavAreaGrid::AddNavArea(NavArea* area) {
		// add to grid
		const Extent* extent = &area->m_extent;

		int loX = WorldToGridX(extent->lo.x);
		int loY = WorldToGridY(extent->lo.y);
		int hiX = WorldToGridX(extent->hi.x);
		int hiY = WorldToGridY(extent->hi.y);

		for (int y = loY; y <= hiY; ++y)
			for (int x = loX; x <= hiX; ++x)
				m_grid[x + y * m_gridSizeX].push_back(const_cast<NavArea*>(area));

		// add to hash table
		int key = ComputeHashKey(area->m_id);

		if (m_hashTable[key]) {
			// add to head of list in this slot
			area->m_prevHash = nullptr;
			area->m_nextHash = m_hashTable[key];
			m_hashTable[key]->m_prevHash = area;
			m_hashTable[key] = area;
		} else {
			// first entry in this slot
			m_hashTable[key] = area;
			area->m_nextHash = nullptr;
			area->m_prevHash = nullptr;
		}
		++m_areaCount;
	}

	/**
	 * Remove an area from the grid
	 */
	void NavAreaGrid::RemoveNavArea(NavArea* area) {
		// add to grid
		const Extent* extent = &area->m_extent;

		int loX = WorldToGridX(extent->lo.x);
		int loY = WorldToGridY(extent->lo.y);
		int hiX = WorldToGridX(extent->hi.x);
		int hiY = WorldToGridY(extent->hi.y);

		for (int y = loY; y <= hiY; ++y)
			for (int x = loX; x <= hiX; ++x)
				m_grid[x + y * m_gridSizeX].remove(area);

		// remove from hash table
		int key = ComputeHashKey(area->m_id);

		if (area->m_prevHash) {
			area->m_prevHash->m_nextHash = area->m_nextHash;
		} else {
			// area was at start of list
			m_hashTable[key] = area->m_nextHash;

			if (m_hashTable[key])
				m_hashTable[key]->m_prevHash = nullptr;
		}

		if (area->m_nextHash) {
			area->m_nextHash->m_prevHash = area->m_prevHash;
		}
		--m_areaCount;
	}

	/**
	 * Given a position, return the nav area that IsOverlapping and is *immediately* beneath it
	 */
	NavArea* NavAreaGrid::GetNavArea(const Vector* pos, float beneathLimit) const {
		if (m_grid == nullptr)
			return nullptr;

		// get list in cell that contains position
		int x = WorldToGridX(pos->x);
		int y = WorldToGridY(pos->y);
		auto list = &m_grid[x + y * m_gridSizeX];


		// search cell list to find correct area
		NavArea* use = nullptr;
		float useZ = -99999999.9f;
		Vector testPos = *pos + Vector(0, 0, 5);

		for (auto iter = list->begin(); iter != list->end(); ++iter) {
			NavArea* area = *iter;

			// check if position is within 2D boundaries of this area
			if (area->IsOverlapping(&testPos)) {
				// project position onto area to get Z
				float z = area->GetZ(&testPos);

				// if area is above us, skip it
				if (z > testPos.z)
					continue;

				// if area is too far below us, skip it
				if (z < pos->z - beneathLimit)
					continue;

				// if area is higher than the one we have, use this instead
				if (z > useZ) {
					use = area;
					useZ = z;
				}
			}
		}
		return use;
	}

	void NavigationMap::ForEachArea(std::function<void(const NavArea*)> func) {
		for (auto& area : m_areas) {
			func(area);
		}
	}

	NavArea* NavigationMap::GetNavArea(const Vector* pos) const {
		return m_navAreaGrid.GetNavArea(pos);
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
	 * Given a position in the world, return the nav area that is closest
	 * and at the same height, or beneath it.
	 * Used to find initial area if we start off of the mesh.
	 */
	NavArea* NavAreaGrid::GetNearestNavArea(NavigationMap* mesh, const Vector* pos, bool anyZ) const {
		if (m_grid == nullptr)
			return nullptr;


		// quick check
		auto close = GetNavArea(pos);
		if (close)
			return close;

		// ensure source position is well behaved
		Vector source;
		source.x = pos->x;
		source.y = pos->y;
		if (!GetGroundHeight(pos, &source.z))
			return nullptr;

		source.z += HalfHumanHeight;

		/// @todo Step incrementally using grid for speed
		
		float closeDistSq = 99999999.9f;
		// find closest nav area
		mesh->ForEachArea(
			[&](const NavArea* area) -> void {
				Vector areaPos;
				area->GetClosestPointOnArea(&source, &areaPos);

				float distSq = std::pow((areaPos - source).Length(), 2);

				// keep the closest area
				if (distSq < closeDistSq) {
					// check LOS to area
					if (!anyZ) {
						TraceResult result;
						UTIL_TraceLine(source, areaPos + Vector(0, 0, HalfHumanHeight), ignore_monsters, ignore_glass, nullptr, &result);
						if (result.flFraction != 1.0f)
							return;
					}

					closeDistSq = distSq;
					close = const_cast<NavArea*>(area);
				}
			}
		);
		return close;
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
	 * Given an ID, return the associated area
	 */
	NavArea* NavAreaGrid::GetNavAreaByID(unsigned int id) const {
		if (id == 0)
			return nullptr;

		int key = ComputeHashKey(id);

		for (NavArea* area = m_hashTable[key]; area; area = area->m_nextHash)
			if (area->m_id == id)
				return area;

		return nullptr;
	}

	//--------------------------------------------------------------------------------------------------------------
	/**
	 * Return radio chatter place for given coordinate
	 */
	unsigned int NavAreaGrid::GetPlace(NavigationMap* mesh, const Vector* pos) const {
		NavArea* area = GetNearestNavArea(mesh, pos, true);

		if (area)
			return area->m_place;

		return Undefined_Place;
	}


	void NavigationMap::DestroyHidingSpots(void) {
		// remove all hiding spot references from the nav areas
		for (auto& area : m_areas) {
			area->hiding_spots.clear();
		}

		HidingSpot::m_nextID = 0;

		// free all the HidingSpots
		for (auto iter = m_hidingSpots.begin(); iter != m_hidingSpots.end(); ++iter)
			delete* iter;

		m_hidingSpots.clear();
	}

	void NavigationMap::AddHidingSpots(HidingSpot* spot) { m_hidingSpots.push_back(spot); }
}