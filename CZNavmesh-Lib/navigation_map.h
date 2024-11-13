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
#pragma once
#include <extdll.h>
#include <dllapi.h>
#include <meta_api.h>
#include <entity_state.h>

#include <list>
#include <string>
#include <vector>
#include <functional>

namespace navmesh {	
	struct NavArea;
	struct NavNode;
	struct NavLadder;
	class HidingSpot;
	class NavigationMap;

	/**
	 * A place is a named group of navigation areas
	 */
	using Place = std::uint32_t;
	constexpr Place Undefined_Place = 0u;	// ie: "no place"
	constexpr Place Any_Place = 0xFFFF;

	struct Extent { Vector lo, hi; };
	struct Ray { Vector from, to; };

	//-------------------------------------------------------------------------------------------------------------------
	/**
	* The NavConnect union is used to refer to connections to areas
	*/
	union NavConnect {
		unsigned int id;
		NavArea* area;

		inline bool operator==(const NavConnect& other) const { return (area == other.area); }
	};



	//-------------------------------------------------------------------------------------------------------------------
	/**
	 * Used when building a path to determine the kind of path to build
	 */
	enum RouteType {
		FASTEST_ROUTE,
		SAFEST_ROUTE,
	};


	//--------------------------------------------------------------------------------------------------------------
	enum LadderDirectionType {
		LADDER_UP = 0,
		LADDER_DOWN,

		NUM_LADDER_DIRECTIONS
	};

	enum NavErrorType {
		NAV_OK,
		NAV_CANT_ACCESS_FILE,
		NAV_INVALID_FILE,
		NAV_BAD_FILE_VERSION,
		NAV_CORRUPT_DATA,
	};

	enum NavAttributeType {
		NAV_CROUCH = 0x01,											///< must crouch to use this node/area
		NAV_JUMP = 0x02,											///< must jump to traverse this area
		NAV_PRECISE = 0x04,											///< do not adjust for obstacles, just move along area
		NAV_NO_JUMP = 0x08,											///< inhibit discontinuity jumping
	};

	enum NavDirType {
		NORTH = 0,
		EAST = 1,
		SOUTH = 2,
		WEST = 3,

		NUM_DIRECTIONS
	};

	/**
	 * Defines possible ways to move from one area to another
	 */
	enum NavTraverseType {
		// NOTE: First 4 directions MUST match NavDirType
		GO_NORTH = 0,
		GO_EAST,
		GO_SOUTH,
		GO_WEST,
		GO_LADDER_UP,
		GO_LADDER_DOWN,
		GO_JUMP,

		NUM_TRAVERSE_TYPES
	};

	enum NavCornerType {
		NORTH_WEST = 0,
		NORTH_EAST = 1,
		SOUTH_EAST = 2,
		SOUTH_WEST = 3,

		NUM_CORNERS
	};



	//--------------------------------------------------------------------------------------------------------------
	/**
	 * A HidingSpot is a good place for a bot to crouch and wait for enemies
	 */
	class HidingSpot {
	public:
		HidingSpot(NavigationMap*);

		/**
		 * For use when generating - assigns unique ID
		 */
		HidingSpot(NavigationMap*, const Vector* pos, unsigned char flags);
		enum {
			IN_COVER = 0x01,							///< in a corner with good hard cover nearby
			GOOD_SNIPER_SPOT = 0x02,							///< had at least one decent sniping corridor
			IDEAL_SNIPER_SPOT = 0x04							///< can see either very far, or a large area, or both
		};
		Vector m_pos;											///< world coordinates of the spot
		unsigned int m_id;										///< this spot's unique ID
		unsigned int m_marker;									///< this spot's unique marker

		unsigned char m_flags;									///< bit flags

		inline static unsigned int m_nextID;							///< used when allocating spot ID's
		inline static unsigned int m_masterMarker;						///< used to mark spots
	};

	//--------------------------------------------------------------------------------------------------------------
	/**
	 * Stores a pointer to an interesting "spot", and a parametric distance along a path
	 */
	struct SpotOrder {
		float t;								///< parametric distance along ray where this spot first has LOS to our path
		union {
			HidingSpot* spot;					///< the spot to look at
			unsigned int id;					///< spot ID for save/load
		};
	};

	/**
	 * SpotEncounter stores possible path segments thru a NavArea, and the dangerous spots
	 * to look at as we traverse that path segment.
	 */
	struct SpotEncounter {
		NavConnect from{};
		NavDirType fromDir{};
		NavConnect to{};
		NavDirType toDir{};
		Ray path{};							///< the path segment
		std::list<SpotOrder> spotList{};	///< list of spots to look at, in order of occurrence
	};
		
	// NavLadder encapsulates traversable ladders, and their connections to NavAreas
	struct NavLadder {
	public:
		Vector m_top{};										///< world coords of the top of the ladder
		Vector m_bottom{};									///< world coords of the top of the ladder
		float m_length{};									///< the length of the ladder
		NavDirType m_dir{};									///< which way the ladder faces (ie: surface normal of climbable side)
		Vector2D m_dirVector{};								///< unit vector representation of m_dir
		edict_t* m_entity{};								///< the ladder itself

		NavArea* m_topForwardArea{};						///< the area at the top of the ladder
		NavArea* m_topLeftArea{};
		NavArea* m_topRightArea{};
		NavArea* m_topBehindArea{};							///< area at top of ladder "behind" it - only useful for descending
		NavArea* m_bottomArea{};							///< the area at the bottom of the ladder

		bool m_isDangling{};								///< if true, the bottom of the ladder is hanging too high to climb up
	};	

	//-------------------------------------------------------------------------------------------------------------------
	/**
	 * A NavArea is a rectangular region defining a walkable area in the map
	 */
	struct NavArea {
		//--------------------------------------------------------------------------------------------------------------
		/**
		 * To keep constructors consistent
		 */
		void Initialize();

		//--------------------------------------------------------------------------------------------------------------
		/**
		 * Constructor used during normal runtime.
		 */
		NavArea();

		//- ladders -----------------------------------------------------------------------------------------
		void AddLadderUp(NavLadder* ladder) { m_ladder[LADDER_UP].push_back(ladder); }
		void AddLadderDown(NavLadder* ladder) { m_ladder[LADDER_DOWN].push_back(ladder); }

		//--------------------------------------------------------------------------------------------------------------
		/**
		 * Compute "portal" between to adjacent areas. 
		 * Return center of portal opening, and half-width defining sides of portal from center.
		 * NOTE: center->z is unset.
		 */
		void ComputePortal(const NavArea* to, NavDirType dir, Vector* center, float* halfWidth) const;

		//--------------------------------------------------------------------------------------------------------------
		/**
		 * Return true if 'pos' is within 2D extents of area.
		 */
		bool IsOverlapping(const Vector* pos) const noexcept;

		//--------------------------------------------------------------------------------------------------------------
		/**
		 * Return true if 'area' overlaps our 2D extents
		 */
		bool IsOverlapping(const NavArea* area) const noexcept;

		//--------------------------------------------------------------------------------------------------------------
		/**
		 * Return closest point to 'pos' on 'area'.
		 * Returned point is in 'close'.
		 */
		void GetClosestPointOnArea(const Vector* pos, Vector* close) const;
		float GetZ(const Vector* pos) const;
		float GetZ(float x, float y) const;

		static inline unsigned int m_nextID = 1;							///< used to allocate unique IDs
		//- approach areas ----------------------------------------------------------------------------------
		struct ApproachInfo {
			NavConnect here{};										///< the approach area
			NavConnect prev{};										///< the area just before the approach area on the path
			NavTraverseType prevToHereHow{};
			NavConnect next{};										///< the area just after the approach area on the path
			NavTraverseType hereToNextHow{};
		};

		std::uint32_t m_id{};									///< unique area ID
		Extent m_extent{};										///< extents of area in world coords (NOTE: lo.z is not necessarily the minimum Z, but corresponds to Z at point (lo.x, lo.y), etc
		Vector m_center{};										///< centroid of area
		std::uint8_t m_attributeFlags{};						///< set of attribute bit flags (see NavAttributeType)
		Place m_place{};										///< place descriptor

		/// height of the implicit corners
		float m_neZ{};
		float m_swZ{};

		static constexpr auto MAX_AREA_TEAMS = 2;

		//- for hunting -------------------------------------------------------------------------------------
		float m_clearedTimestamp[MAX_AREA_TEAMS];				///< time this area was last "cleared" of enemies

		//- "danger" ----------------------------------------------------------------------------------------
		float m_danger[MAX_AREA_TEAMS];						///< danger of this area, allowing bots to avoid areas where they died in the past - zero is no danger
		float m_dangerTimestamp[MAX_AREA_TEAMS];			///< time when danger value was set - used for decaying

		//- hiding spots ------------------------------------------------------------------------------------
		std::list<HidingSpot*> hiding_spots;

		//- encounter spots ---------------------------------------------------------------------------------
		std::list<SpotEncounter> encounter_spots;			///< list of possible ways to move thru this area, and the spots to look at as we do

		//- approach areas ----------------------------------------------------------------------------------
		enum { MAX_APPROACH_AREAS = 16 };
		ApproachInfo m_approach[MAX_APPROACH_AREAS];
		std::uint8_t m_approachCount;

		//- A* pathfinding algorithm ------------------------------------------------------------------------
		inline static std::uint32_t m_masterMarker{};
		std::uint32_t m_marker{};									///< used to flag the area as visited
		NavArea* m_parent{};										///< the area just prior to this on in the search path
		NavTraverseType m_parentHow{};							///< how we get from parent to us
		float m_totalCost{};										///< the distance so far plus an estimate of the distance left
		float m_costSoFar{};										///< distance travelled so far

		inline static NavArea* m_openList{};
		NavArea* m_nextOpen, * m_prevOpen;						///< only valid if m_openMarker == m_masterMarker
		std::uint32_t m_openMarker;								///< if this equals the current marker value, we are on the open list

		//- connections to adjacent areas -------------------------------------------------------------------
		std::list<NavConnect> m_connect[NUM_DIRECTIONS];				///< a list of adjacent areas for each direction
		std::list<NavLadder*> m_ladder[NUM_LADDER_DIRECTIONS];		///< list of ladders leading up and down from this area

		//---------------------------------------------------------------------------------------------------
		NavNode* m_node[NUM_CORNERS];						///< nav nodes at each corner of the area

		std::list<NavArea*> m_overlapList;					///< list of areas that overlap this area

		void OnDestroyNotify(NavArea* dead);					///< invoked when given area is going away

		NavArea* m_prevHash, * m_nextHash;						///< for hash table in NavAreaGrid
	};

	/**
	 * The NavAreaGrid is used to efficiently access navigation areas by world position.
	 * Each cell of the grid contains a list of areas that overlap it.
	 * Given a world position, the corresponding grid cell is ( x/cellsize, y/cellsize ).
	 */
	class NavAreaGrid {
	public:
		NavAreaGrid(void);
		~NavAreaGrid();

		void Reset(void);										///< clear the grid to empty
		void Initialize(float minX, float maxX, float minY, float maxY);	///< clear and reset the grid to the given extents
		void AddNavArea(NavArea* area);						///< add an area to the grid
		void RemoveNavArea(NavArea* area);					///< remove an area from the grid
		unsigned int GetNavAreaCount(void) const { return m_areaCount; }	///< return total number of nav areas

		NavArea* GetNavArea(const Vector* pos, float beneathLimt = 120.0f) const;	///< given a position, return the nav area that IsOverlapping and is *immediately* beneath it
		NavArea* GetNavAreaByID(unsigned int id) const;
		NavArea* GetNearestNavArea(NavigationMap*, const Vector* pos, bool anyZ = false) const;

		Place GetPlace(NavigationMap* mesh, const Vector* pos) const;				///< return radio chatter place for given coordinate
	private:
		const float m_cellSize;
		std::list<NavArea*>* m_grid;
		int m_gridSizeX;
		int m_gridSizeY;
		float m_minX;
		float m_minY;
		unsigned int m_areaCount;								///< total number of nav areas

		static constexpr auto HASH_TABLE_SIZE = 256;
		NavArea* m_hashTable[HASH_TABLE_SIZE]{};				///< hash table to optimize lookup by ID

		/// returns a hash key for the given nav area ID
		int ComputeHashKey(unsigned int id) const;
		int WorldToGridX(float wx) const;
		int WorldToGridY(float wy) const;
	};

	class PlaceDirectory {
		std::vector<Place> m_directory;
	public:
		using EntryType = std::uint16_t;

		void Reset();
		bool IsKnown(Place place) const;
		EntryType GetEntry(Place place) const;
		void AddPlace(Place place);
		Place EntryToPlace(EntryType entry) const;

		inline void Reserve(size_t count) { m_directory.reserve(count); }
	};

	class NavigationMap {
		std::list<NavArea*> m_areas{};
		PlaceDirectory m_placeDirectory{};
		NavAreaGrid m_navAreaGrid{};
		std::list<NavLadder*> m_navLadders{};
		std::list<HidingSpot*> m_hidingSpots{};

		HidingSpot* GetHidingSpotByID(std::uint32_t id);
		void DestroyHidingSpots();

		void Validate(NavArea* area);
		void BuildLadders();
		void DestroyLadders();
	public:
		void Destroy();
		void ForEachArea(std::function<void(const NavArea*)>);
		NavArea* GetNavArea(const Vector* pos) const;

		NavArea* FindFirstAreaInDirection(const Vector* start, NavDirType dir, float range, float beneathLimit, edict_t* traceIgnore = nullptr, Vector* closePos = nullptr);
		bool Load(const std::string& Path_To_Nav);
		void AddHidingSpots(HidingSpot* spot);
	};
}