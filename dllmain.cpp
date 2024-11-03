#define WIN32_LEAN_AND_MEAN             // Exclude rarely-used stuff from Windows headers
// Windows Header Files

#include <windows.h>
#include <extdll.h>
#include <dllapi.h>
#include <meta_api.h>
#include <entity_state.h>
#include <cmath>

#include <cassert>
#include <format>
#include <numbers>
#include <format>
#include "navigation_map.h"

edict_t* host{};

enginefuncs_t g_engfuncs;
globalvars_t* gpGlobals;

// START of Metamod stuff

enginefuncs_t meta_engfuncs, meta_engfuncs_post;
gamedll_funcs_t* gpGamedllFuncs;
mutil_funcs_t* gpMetaUtilFuncs;
meta_globals_t* gpMetaGlobals;

DLL_FUNCTIONS func_table;
DLL_FUNCTIONS gFunctionTable_Post;

META_FUNCTIONS gMetaFunctionTable{
    nullptr, // pfnGetEntityAPI()
    nullptr, // pfnGetEntityAPI_Post()
    GetEntityAPI2, // pfnGetEntityAPI2()
    GetEntityAPI2_Post, // pfnGetEntityAPI2_Post()
    nullptr, // pfnGetNewDLLFunctions()
    nullptr, // pfnGetNewDLLFunctions_Post()
    GetEngineFunctions, // pfnGetEngineFunctions()
    nullptr,            // pfnGetEngineFunctions_Post()
};

bool is_loaded{};
bool draw_mesh{};
int beam_sprite{};

struct Color {
    int r, g, b;
};

void DrawBox(edict_t* ent, const Vector& start, const Vector& end, const Color& color, int life) {
	MESSAGE_BEGIN(MSG_ONE, SVC_TEMPENTITY, nullptr, ent);
	WRITE_BYTE(TE_BOX);
	WRITE_COORD(end.x);
	WRITE_COORD(end.y);
	WRITE_COORD(end.z);
	WRITE_COORD(start.x);
	WRITE_COORD(start.y);
	WRITE_COORD(start.z);
	WRITE_SHORT(life);
	WRITE_BYTE(color.r); // r, g, b
	WRITE_BYTE(color.g); // r, g, b
	WRITE_BYTE(color.b); // r, g, b
	MESSAGE_END();
}

plugin_info_t Plugin_info{
    const_cast<char*>(META_INTERFACE_VERSION),  // interface version
    const_cast<char*>("NAVMESH"),               // plugin name
    const_cast<char*>("Test Version"),          // plugin version
    const_cast<char*>(__DATE__),                // date of creation
    const_cast<char*>("plugin author"),                      // plugin author
    const_cast<char*>("plugin URL"),                      // plugin URL
    const_cast<char*>("plugintag"),               // plugin logtag
    PT_CHANGELEVEL,                             // when loadable
    PT_ANYTIME,                                 // when unloadable
};

navmesh::NavigationMap navigation_map{};

BOOL APIENTRY DllMain( HMODULE hModule,
                       DWORD  ul_reason_for_call,
                       LPVOID lpReserved
                     )
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH:
    case DLL_THREAD_ATTACH:
        break;
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        assert(is_loaded && "Unable to load the DLL. Check the .def setting.");
        break;
    }
    return TRUE;
}

C_DLLEXPORT int Meta_Query(char* ifvers, plugin_info_t** pPlugInfo, mutil_funcs_t* pMetaUtilFuncs) {
    // this function is the first function ever called by metamod in the plugin DLL. Its purpose
    // is for metamod to retrieve basic information about the plugin, such as its meta-interface
    // version, for ensuring compatibility with the current version of the running metamod.

    // keep track of the pointers to metamod function tables metamod gives us
    gpMetaUtilFuncs = pMetaUtilFuncs;
    *pPlugInfo = &Plugin_info;

    // check for interface version compatibility
    if (strcmp(ifvers, Plugin_info.ifvers) != 0) {
        int mmajor = 0, mminor = 0, pmajor = 0, pminor = 0;

        LOG_CONSOLE(PLID, "%s: meta-interface version mismatch (metamod: %s, %s: %s)", Plugin_info.name, ifvers, Plugin_info.name, Plugin_info.ifvers);
        LOG_MESSAGE(PLID, "%s: meta-interface version mismatch (metamod: %s, %s: %s)", Plugin_info.name, ifvers, Plugin_info.name, Plugin_info.ifvers);

        // if plugin has later interface version, it's incompatible (update metamod)
        sscanf(ifvers, "%d:%d", &mmajor, &mminor);
        sscanf(META_INTERFACE_VERSION, "%d:%d", &pmajor, &pminor);

        if ((pmajor > mmajor) || ((pmajor == mmajor) && (pminor > mminor))) {
            LOG_CONSOLE(PLID, "metamod version is too old for this plugin; update metamod");
            LOG_ERROR(PLID, "metamod version is too old for this plugin; update metamod");
            return (FALSE);
        }

        // if plugin has older major interface version, it's incompatible (update plugin)
        else if (pmajor < mmajor) {
            LOG_CONSOLE(PLID, "metamod version is incompatible with this plugin; please find a newer version of this plugin");
            LOG_ERROR(PLID, "metamod version is incompatible with this plugin; please find a newer version of this plugin");
            return (FALSE);
        }
    }
    return (TRUE); // tell metamod this plugin looks safe
}

void
UTIL_ClientPrintAll(int msg_dest, const char *msg_name, const char *param1,
                    const char *param2, const char *param3,
                    const char *param4) {
    static int gmsgTextMsg = REG_USER_MSG("TextMsg", -1);

    MESSAGE_BEGIN(MSG_ALL, gmsgTextMsg);
    WRITE_BYTE(msg_dest);
    WRITE_STRING(msg_name);

    if (param1)
        WRITE_STRING(param1);
    if (param2)
        WRITE_STRING(param2);
    if (param3)
        WRITE_STRING(param3);
    if (param4)
        WRITE_STRING(param4);

    MESSAGE_END();
}

bool show_centermessage{};
void CenterMessage(const char *buffer) {
    UTIL_ClientPrintAll(HUD_PRINTCENTER, buffer);
}


C_DLLEXPORT int Meta_Attach(PLUG_LOADTIME now, META_FUNCTIONS* pFunctionTable, meta_globals_t* pMGlobals, gamedll_funcs_t* pGamedllFuncs) {
    // this function is called when metamod attempts to load the plugin. Since it's the place
    // where we can tell if the plugin will be allowed to run or not, we wait until here to make
    // our initialization stuff, like registering CVARs and dedicated server commands.

    // are we allowed to load this plugin now ?
    if (now > Plugin_info.loadable) {
        LOG_CONSOLE(PLID, "%s: plugin NOT attaching (can't load plugin right now)", Plugin_info.name);
        LOG_ERROR(PLID, "%s: plugin NOT attaching (can't load plugin right now)", Plugin_info.name);
        return (FALSE); // returning FALSE prevents metamod from attaching this plugin
    }

    // keep track of the pointers to engine function tables metamod gives us
    gpMetaGlobals = pMGlobals;
    memcpy(pFunctionTable, &gMetaFunctionTable, sizeof(META_FUNCTIONS));
    gpGamedllFuncs = pGamedllFuncs;

    // print a message to notify about plugin attaching
    LOG_CONSOLE(PLID, "%s: plugin attaching", Plugin_info.name);
    LOG_MESSAGE(PLID, "%s: plugin attaching", Plugin_info.name);

    REG_SVR_COMMAND("loadnav", [] {
        if (!navigation_map.Load(std::format("cstrike/maps/{}.nav", STRING(gpGlobals->mapname)))) {
            if (!navigation_map.Load(std::format("czero/maps/{}.nav", STRING(gpGlobals->mapname)))) {
                SERVER_PRINT("Navmesh: Failed to load the nav file.");
                return;
            } else {
                SERVER_PRINT("Navmesh: Loaded the nav file from czero.");
            }
        } else {        
            SERVER_PRINT("Navmesh: Loaded the nav file from cstrike.");
        }
    });

    REG_SVR_COMMAND("getnav", [] {
        auto mesh = navigation_map.GetNavArea(&host->v.origin);
        if (mesh != nullptr) {
            SERVER_PRINT(std::format("NavID: {}\n", mesh->m_id).c_str());
        } else {
            SERVER_PRINT("Could not get the navigation mesh.\n");
        }
    });
    // ask the engine to register the server commands this plugin uses
    return (TRUE); // returning TRUE enables metamod to attach this plugin
}


C_DLLEXPORT int Meta_Detach(PLUG_LOADTIME now, PL_UNLOAD_REASON reason) {
    // this function is called when metamod unloads the plugin. A basic check is made in order
    // to prevent unloading the plugin if its processing should not be interrupted.

    // is metamod allowed to unload the plugin ?
    if ((now > Plugin_info.unloadable) && (reason != PNL_CMD_FORCED)) {
        LOG_CONSOLE(PLID, "%s: plugin NOT detaching (can't unload plugin right now)", Plugin_info.name);
        LOG_ERROR(PLID, "%s: plugin NOT detaching (can't unload plugin right now)", Plugin_info.name);
        return (FALSE); // returning FALSE prevents metamod from unloading this plugin
    }
    return (TRUE); // returning TRUE enables metamod to unload this plugin
}

C_DLLEXPORT void WINAPI
GiveFnptrsToDll(enginefuncs_t* pengfuncsFromEngine, globalvars_t* pGlobals) {
    // get the engine functions from the engine...
    memcpy(&g_engfuncs, pengfuncsFromEngine, sizeof(enginefuncs_t));
    gpGlobals = pGlobals;
    is_loaded = true;
}

C_DLLEXPORT int GetEntityAPI2(DLL_FUNCTIONS* pFunctionTable, int* interfaceVersion) {
    func_table.pfnStartFrame = []() -> void {
        RETURN_META(MRES_IGNORED); 
    };
    func_table.pfnGameInit = []() -> void { RETURN_META(MRES_IGNORED); };
    func_table.pfnSpawn = [](edict_t* entity) -> int { RETURN_META_VALUE(MRES_IGNORED, 0); };
    func_table.pfnClientConnect = [](edict_t* entity, const char* Name, const char* Address, char Reject_Reason[]) -> qboolean {
        if (gpGlobals->deathmatch) {
            // check if this client is the listen server client
            if (strcmp(Address, "loopback") == 0) {
                host = entity;            
                beam_sprite = PRECACHE_MODEL("sprites/laserbeam.spr");
            }
        }
        RETURN_META_VALUE(MRES_IGNORED, 0);
    };
    func_table.pfnClientDisconnect = [](edict_t* entity) -> void { RETURN_META(MRES_IGNORED); };
    func_table.pfnClientPutInServer = [](edict_t* entity) -> void { RETURN_META(MRES_IGNORED); };
    func_table.pfnServerActivate = [](edict_t* edictList, int edictCount, int) -> void { RETURN_META(MRES_IGNORED); };
    func_table.pfnClientCommand = [](edict_t*) -> void { RETURN_META(MRES_IGNORED); };

    memcpy(pFunctionTable, &func_table, sizeof(DLL_FUNCTIONS));
    return (TRUE);
}

C_DLLEXPORT int GetEntityAPI2_Post(DLL_FUNCTIONS* pFunctionTable, int* interfaceVersion) {
    memcpy(pFunctionTable, &gFunctionTable_Post, sizeof(DLL_FUNCTIONS));
    return (TRUE);
}

C_DLLEXPORT int
GetEngineFunctions(enginefuncs_t* pengfuncsFromEngine, int* interfaceVersion) {
    memcpy(pengfuncsFromEngine, &meta_engfuncs, sizeof(enginefuncs_t));
    return TRUE;
}
