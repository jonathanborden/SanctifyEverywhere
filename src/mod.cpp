#include "mod.h"
#include "tick_hook.h"
#include <cstdio>
#include <atomic>
#include <cstring>
#include <cmath>

// ============================================================
// Globals
// ============================================================

static std::atomic<bool> gRunning{false};
static HANDLE gModThread = nullptr;
FILE* gLogFile = nullptr;

// Types needed early
struct FName { int32_t ComparisonIndex; int32_t Number; };
struct FString { wchar_t* Data; int32_t Count; int32_t Max; };
typedef void (__fastcall *FNameToString_fn)(const FName* name, FString& outStr);
typedef void (__fastcall *ProcessEvent_fn)(void*, void*, void*);

// UE state - found once, reused
void* gObjArrayBase = nullptr;
int32_t gNumElements = 0, gNumChunks = 0;
int gItemSize = 24;
uintptr_t gTextStart = 0, gTextSize = 0;
uintptr_t gDataStart = 0, gDataSize = 0;
uintptr_t gExeBase = 0, gExeEnd = 0;
static uintptr_t gProcessEventAddr = 0;
static FNameToString_fn gFNameToString = nullptr;

// Cached UFunction/object pointers (found once per session)
static void* gBeginSpawnFunc = nullptr;
static void* gFinishSpawnFunc = nullptr;
static void* gGetLocFunc = nullptr;
static void* gSetLocFunc = nullptr;
static void* gConstructionFunc = nullptr;
static void* gBeginPlayFunc = nullptr;
static void* gGameplayStaticsCDO = nullptr;

// ============================================================
// Constants (Deadzone Rogue v1.4 - stable across ASLR)
// ============================================================

static const int64_t FNAME_TOSTRING_OFFSET = -0x0A27FB50; // FNameToString = GUObjectArray_struct + this
static const int PROCESS_EVENT_VTABLE_SLOT = 0x4C;
static const int BEGIN_SPAWN_RETVAL_OFFSET = 0x88;
static const int FINISH_SPAWN_RETVAL_OFFSET = 120;

// ============================================================
// Forward declarations & types
// ============================================================

void Log(const char* fmt, ...);
bool IsSafeToRead(void* addr, size_t size);
bool SafeRead64(uintptr_t addr, uintptr_t* out);
bool SafeRead32(uintptr_t addr, int32_t* out);
void* GetUObject(int32_t index);
bool GetObjectName(void* obj, wchar_t* buf, int bufSize);
void* GetObjClass(void* obj);
void* GetObjOuter(void* obj);
bool IsCDO(void* obj);

// ============================================================
// Spawn request (background thread -> game thread)
// ALL pointers found FRESH per F9 press — not cached across level loads
// ============================================================

static struct {
    void* bpAnvilClass;
    void* gameWorld;
    void* spawnFunc;
    void* finishSpawnFunc;
    void* gameplayStaticsCDO;
    void* setLocFunc;
    void* constructionFunc;
    void* beginPlayFunc;
    void* rerunConstructionFunc;
    void* setSanctifiedAnvilFunc;
    void* gameModeInstance;
    void* enableSAFunc;
    void* safeRoomMgr;
    uintptr_t processEventAddr;
    double spawnX[32], spawnY[32], spawnZ[32];
    int spawnCount;
    volatile bool resultReady;
    volatile bool success;
} gSpawnReq = {};

static void SpawnOneAnvil(ProcessEvent_fn pe, double sx, double sy, double sz) {

    // BeginDeferredActorSpawnFromClass
    __declspec(align(16)) uint8_t p[256] = {};
    *(void**)(p + 0) = gSpawnReq.gameWorld;
    *(void**)(p + 8) = gSpawnReq.bpAnvilClass;
    *(double*)(p + 16) = 0; *(double*)(p + 24) = 0; *(double*)(p + 32) = 0; *(double*)(p + 40) = 1.0; // Quat identity
    *(double*)(p + 48) = sx;
    *(double*)(p + 56) = sy;
    *(double*)(p + 64) = sz;
    *(double*)(p + 72) = 0;
    *(double*)(p + 80) = 1.0; *(double*)(p + 88) = 1.0; *(double*)(p + 96) = 1.0; *(double*)(p + 104) = 0; // Scale
    *(uint8_t*)(p + 112) = 1; // AlwaysSpawn

    __try { pe(gSpawnReq.gameplayStaticsCDO, gSpawnReq.spawnFunc, p); }
    __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] BeginSpawn CRASHED\n"); gSpawnReq.resultReady = true; return; }

    void* actor = *(void**)(p + BEGIN_SPAWN_RETVAL_OFFSET);
    if (!actor) { Log("[GT] BeginSpawn returned null\n"); gSpawnReq.resultReady = true; return; }
    Log("[GT] Actor: 0x%llX\n", (uintptr_t)actor);

    // FinishSpawningActor
    Log("[GT] Calling FinishSpawn...\n");
    __declspec(align(16)) uint8_t fp[256] = {};
    *(void**)(fp + 0) = actor;
    *(double*)(fp + 0x10) = 0; *(double*)(fp + 0x18) = 0; *(double*)(fp + 0x20) = 0; *(double*)(fp + 0x28) = 1.0;
    *(double*)(fp + 0x30) = sx; *(double*)(fp + 0x38) = sy; *(double*)(fp + 0x40) = sz;
    *(double*)(fp + 0x48) = 0;
    *(double*)(fp + 0x50) = 1.0; *(double*)(fp + 0x58) = 1.0; *(double*)(fp + 0x60) = 1.0; *(double*)(fp + 0x68) = 0;

    __try {
        pe(gSpawnReq.gameplayStaticsCDO, gSpawnReq.finishSpawnFunc, fp);
        void* result = *(void**)(fp + FINISH_SPAWN_RETVAL_OFFSET);
        Log("[GT] FinishSpawn returned: 0x%llX\n", (uintptr_t)result);
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[GT] FinishSpawn CRASHED!\n");
    }

    // Move actor to correct position (FTransform is ignored during spawn)
    if (gSpawnReq.setLocFunc) {
        __declspec(align(16)) uint8_t slp[512] = {};
        *(double*)(slp + 0) = sx;
        *(double*)(slp + 8) = sy;
        *(double*)(slp + 16) = sz;
        // bSweep=false at 24, bTeleport=true near end
        int32_t setRv = 0;
        SafeRead32((uintptr_t)gSpawnReq.setLocFunc + 0xB8, &setRv);
        if (setRv > 4) *(uint8_t*)(slp + setRv - 1) = 1; // bTeleport=true
        __try { pe(actor, gSpawnReq.setLocFunc, slp); Log("[GT] SetActorLocation OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] SetActorLocation CRASHED\n"); }
    }

    // Trigger Blueprint init
    if (gSpawnReq.constructionFunc) {
        __declspec(align(16)) uint8_t ip[64] = {};
        __try { pe(actor, gSpawnReq.constructionFunc, ip); Log("[GT] UserConstructionScript OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] UserConstructionScript CRASHED\n"); }
    }
    if (gSpawnReq.beginPlayFunc) {
        __declspec(align(16)) uint8_t ip[64] = {};
        __try { pe(actor, gSpawnReq.beginPlayFunc, ip); Log("[GT] ReceiveBeginPlay OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] ReceiveBeginPlay CRASHED\n"); }
    }

    // RerunConstructionScripts — forces component creation and registration
    if (gSpawnReq.rerunConstructionFunc) {
        __declspec(align(16)) uint8_t ip[64] = {};
        __try { pe(actor, gSpawnReq.rerunConstructionFunc, ip); Log("[GT] RerunConstructionScripts OK\n"); }
        __except(EXCEPTION_EXECUTE_HANDLER) { Log("[GT] RerunConstructionScripts CRASHED\n"); }
    }

    // Register anvil with game's sanctify system
    if (gSpawnReq.setSanctifiedAnvilFunc && gSpawnReq.gameModeInstance && actor) {
        __declspec(align(16)) uint8_t ssa[64] = {};
        *(void**)(ssa + 0) = actor;
        __try {
            pe(gSpawnReq.gameModeInstance, gSpawnReq.setSanctifiedAnvilFunc, ssa);
            Log("[GT] SetSanctifiedAnvil OK\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GT] SetSanctifiedAnvil CRASHED\n");
        }
    }

    // Enable sanctification via the safe room encounter manager
    if (gSpawnReq.enableSAFunc && gSpawnReq.safeRoomMgr && actor) {
        __declspec(align(16)) uint8_t esp[256] = {};
        // EnableSanctifiedAnvil PS=144 — likely takes an actor reference
        *(void**)(esp + 0) = actor;
        __try {
            pe(gSpawnReq.safeRoomMgr, gSpawnReq.enableSAFunc, esp);
            Log("[GT] EnableSanctifiedAnvil OK!\n");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GT] EnableSanctifiedAnvil CRASHED\n");
        }
    }

    Log("[GT] Anvil at (%.0f, %.0f, %.0f) done\n", sx, sy, sz);
}

static void GameThreadSpawn() {
    Log("[GT] Spawning %d anvils on thread %lu\n", gSpawnReq.spawnCount, GetCurrentThreadId());
    ProcessEvent_fn pe = (ProcessEvent_fn)gSpawnReq.processEventAddr;

    for (int i = 0; i < gSpawnReq.spawnCount; i++) {
        SpawnOneAnvil(pe, gSpawnReq.spawnX[i], gSpawnReq.spawnY[i], gSpawnReq.spawnZ[i]);
    }

    // Check if GetSanctifyingAvailable is now TRUE
    void* gsAvailFunc = nullptr;
    void* gameStateInst = nullptr;
    {
        wchar_t nb2[256], cb2[256];
        for (int i = 0; i < gNumElements; i++) {
            void* o = GetUObject(i); if (!o) continue;
            if (!GetObjectName(o, nb2, 256)) continue;
            if (wcscmp(nb2, L"GetSanctifyingAvailable") == 0) {
                int32_t ps2 = 0; SafeRead32((uintptr_t)o + 0x58, &ps2);
                if (ps2 <= 8) { gsAvailFunc = o; }
            }
        }
        for (int i = 0; i < gNumElements && !gameStateInst; i++) {
            void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
            void* c = GetObjClass(o); if (!c) continue;
            if (!GetObjectName(c, cb2, 256)) continue;
            if (wcsstr(cb2, L"GameState") && wcsstr(cb2, L"RogueLite")) {
                gameStateInst = o;
            }
        }
    }
    if (gsAvailFunc && gameStateInst) {
        __declspec(align(16)) uint8_t gp[64] = {};
        __try {
            pe(gameStateInst, gsAvailFunc, gp);
            Log("[GT] GetSanctifyingAvailable = %s\n", gp[0] ? "TRUE" : "FALSE");
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GT] GetSanctifyingAvailable crashed\n");
        }

        // If still FALSE, brute-force find the bool property and set it TRUE
        if (!gp[0] && IsSafeToRead(gameStateInst, 0x800)) {
            Log("[GT] Brute-force searching GameState for sanctify bool...\n");
            uint8_t* gs = (uint8_t*)gameStateInst;
            bool found = false;

            // Skip UObject header (first 0x28 bytes) and scan up to 2KB
            for (int off = 0x28; off < 0x800 && !found; off++) {
                if (gs[off] != 0) continue; // Only try bytes that are currently 0

                // Flip to 1
                gs[off] = 1;

                // Check if GetSanctifyingAvailable now returns TRUE
                __declspec(align(16)) uint8_t check[64] = {};
                __try {
                    pe(gameStateInst, gsAvailFunc, check);
                    if (check[0]) {
                        Log("[GT] *** FOUND! GameState+0x%X = sanctify bool! Set to TRUE ***\n", off);
                        found = true;
                        // Leave it as TRUE!
                    } else {
                        // Not the right byte, restore
                        gs[off] = 0;
                    }
                } __except(EXCEPTION_EXECUTE_HANDLER) {
                    gs[off] = 0; // Restore on crash
                }
            }

            if (!found) {
                Log("[GT] Bool not found in first 0x800 bytes of GameState\n");
            }
        }
    }

    // Also brute-force the player's ValPlayerSanctifyComponent bool
    void* isSanctAvailFunc = nullptr;
    {
        wchar_t fnb[256];
        for (int i = 0; i < gNumElements && !isSanctAvailFunc; i++) {
            void* o = GetUObject(i); if (!o) continue;
            if (!GetObjectName(o, fnb, 256) || wcscmp(fnb, L"IsSanctifyingAvailable") != 0) continue;
            int32_t ps2 = 0; SafeRead32((uintptr_t)o + 0x58, &ps2);
            if (ps2 <= 8) isSanctAvailFunc = o;
        }
    }
    if (isSanctAvailFunc) {
        wchar_t cnb[256], onb[256];
        for (int i = 0; i < gNumElements; i++) {
            void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
            void* c = GetObjClass(o); if (!c) continue;
            if (!GetObjectName(c, cnb, 256) || !wcsstr(cnb, L"ValPlayerSanctifyComponent")) continue;
            GetObjectName(o, onb, 256);
            // Skip the GEN_VARIABLE (CDO template)
            if (wcsstr(onb, L"GEN_VARIABLE")) continue;

            Log("[GT] Checking player sanctify component '%ls'...\n", onb);

            // Check current state
            __declspec(align(16)) uint8_t chk[64] = {};
            __try { pe(o, isSanctAvailFunc, chk); }
            __except(EXCEPTION_EXECUTE_HANDLER) { continue; }

            if (chk[0]) {
                Log("[GT] Already TRUE, skipping\n");
                continue;
            }

            // Brute-force find the bool
            Log("[GT] IsSanctifyingAvailable=FALSE, brute-forcing...\n");
            if (IsSafeToRead(o, 0x400)) {
                uint8_t* mem = (uint8_t*)o;
                bool found2 = false;
                for (int off = 0x28; off < 0x400 && !found2; off++) {
                    if (mem[off] != 0) continue;
                    mem[off] = 1;
                    __declspec(align(16)) uint8_t chk2[64] = {};
                    __try {
                        pe(o, isSanctAvailFunc, chk2);
                        if (chk2[0]) {
                            Log("[GT] *** FOUND! PlayerSanctifyComponent+0x%X = sanctify bool! ***\n", off);
                            found2 = true;
                        } else {
                            mem[off] = 0;
                        }
                    } __except(EXCEPTION_EXECUTE_HANDLER) { mem[off] = 0; }
                }
                if (!found2) Log("[GT] Player sanctify bool not found in first 0x400 bytes\n");
            }
        }
    }

    Log("[GT] *** %d ANVILS SPAWNED! ***\n", gSpawnReq.spawnCount);
    gSpawnReq.success = true;
    gSpawnReq.resultReady = true;
}

// ============================================================
// Core functions
// ============================================================

void Log(const char* fmt, ...) {
    if (!gLogFile) return;
    va_list a; va_start(a, fmt); vfprintf(gLogFile, fmt, a); va_end(a);
    fflush(gLogFile);
}

bool IsSafeToRead(void* addr, size_t size) {
    if (!addr) return false;
    MEMORY_BASIC_INFORMATION m;
    if (!VirtualQuery(addr, &m, sizeof(m))) return false;
    if (m.State != MEM_COMMIT || (m.Protect & (PAGE_NOACCESS|PAGE_GUARD)) || !m.Protect) return false;
    return true;
}

bool SafeRead64(uintptr_t a, uintptr_t* o) {
    if (!IsSafeToRead((void*)a, 8)) return false;
    __try { *o = *(uintptr_t*)a; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SafeRead32(uintptr_t a, int32_t* o) {
    if (!IsSafeToRead((void*)a, 4)) return false;
    __try { *o = *(int32_t*)a; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool GetFNameStr(const FName* n, wchar_t* buf, int sz) {
    buf[0] = 0;
    if (!gFNameToString || !n) return false;
    FString r = {0,0,0};
    __try { gFNameToString(n, r); } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
    if (r.Data && r.Count > 0 && IsSafeToRead(r.Data, 2)) {
        __try { wcsncpy_s(buf, sz, r.Data, min(r.Count, sz-1)); return true; }
        __except(EXCEPTION_EXECUTE_HANDLER) {}
    }
    return false;
}

void* GetUObject(int32_t i) {
    if (!gObjArrayBase || i < 0 || i >= gNumElements) return nullptr;
    int32_t ci = i / 65536, wi = i % 65536;
    if (ci >= gNumChunks) return nullptr;
    uintptr_t cb = 0, cp = 0, op = 0;
    __try {
        if (!SafeRead64((uintptr_t)gObjArrayBase, &cb) || !cb) return nullptr;
        if (!SafeRead64(cb + ci * 8, &cp) || !cp) return nullptr;
        if (!SafeRead64(cp + (uintptr_t)wi * gItemSize, &op)) return nullptr;
        return (void*)op;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}

bool GetObjectName(void* obj, wchar_t* buf, int sz) {
    if (!obj || !IsSafeToRead(obj, 0x20)) { buf[0]=0; return false; }
    FName n; SafeRead32((uintptr_t)obj+0x18, &n.ComparisonIndex); SafeRead32((uintptr_t)obj+0x1C, &n.Number);
    return GetFNameStr(&n, buf, sz);
}

void* GetObjClass(void* o) {
    if (!o || !IsSafeToRead(o, 0x18)) return nullptr;
    uintptr_t c = 0; SafeRead64((uintptr_t)o + 0x10, &c);
    return (c && IsSafeToRead((void*)c, 0x20)) ? (void*)c : nullptr;
}

void* GetObjOuter(void* o) {
    if (!o || !IsSafeToRead(o, 0x28)) return nullptr;
    uintptr_t u = 0; SafeRead64((uintptr_t)o + 0x20, &u);
    return (u && IsSafeToRead((void*)u, 0x20)) ? (void*)u : nullptr;
}

bool IsCDO(void* o) {
    wchar_t n[256]; return GetObjectName(o, n, 256) && wcsncmp(n, L"Default__", 9) == 0;
}

// ============================================================
// Pattern scan (only needed once per session for GUObjectArray)
// ============================================================

static uintptr_t ScanPattern(uintptr_t start, size_t size, const uint8_t* pat, const char* mask) {
    size_t pl = strlen(mask); if (size < pl) return 0;
    const uint8_t* m = (const uint8_t*)start;
    for (size_t i = 0; i <= size - pl; i++) {
        bool f = true;
        for (size_t j = 0; j < pl; j++) if (mask[j]=='x' && m[i+j]!=pat[j]) { f=false; break; }
        if (f) return start + i;
    }
    return 0;
}

static bool ValidateObjArray(uintptr_t a, int32_t ne, int32_t nc, int is) {
    uintptr_t cp = 0; if (!SafeRead64(a, &cp) || !cp) return false;
    if (!IsSafeToRead((void*)cp, nc*8)) return false;
    uintptr_t c0 = 0; if (!SafeRead64(cp, &c0) || !c0) return false;
    if (!IsSafeToRead((void*)c0, (size_t)is*16)) return false;
    int v = 0;
    for (int i = 1; i < min(30, ne); i++) {
        uintptr_t op = 0; if (!SafeRead64(c0 + (uintptr_t)i*is, &op)) continue;
        if (!op || !IsSafeToRead((void*)op, 0x28)) continue;
        uintptr_t vt = 0; if (!SafeRead64(op, &vt) || vt < gExeBase || vt >= gExeEnd) continue;
        uintptr_t cl = 0; if (!SafeRead64(op+0x10, &cl) || !cl || !IsSafeToRead((void*)cl, 0x20)) continue;
        uintptr_t cv = 0; if (!SafeRead64(cl, &cv) || cv < gExeBase || cv >= gExeEnd) continue;
        v++;
    }
    return v >= 5;
}

static bool InitUE() {
    gExeBase = (uintptr_t)GetModuleHandleA(nullptr);
    PIMAGE_DOS_HEADER d = (PIMAGE_DOS_HEADER)gExeBase;
    PIMAGE_NT_HEADERS n = (PIMAGE_NT_HEADERS)(gExeBase + d->e_lfanew);
    gExeEnd = gExeBase + n->OptionalHeader.SizeOfImage;
    PIMAGE_SECTION_HEADER s = IMAGE_FIRST_SECTION(n);
    for (int i = 0; i < n->FileHeader.NumberOfSections; i++) {
        if (!strncmp((char*)s[i].Name, ".text", 5)) { gTextStart = gExeBase + s[i].VirtualAddress; gTextSize = s[i].Misc.VirtualSize; }
        if (!strncmp((char*)s[i].Name, ".data", 5)) { gDataStart = gExeBase + s[i].VirtualAddress; gDataSize = s[i].Misc.VirtualSize; }
    }

    // Find GUObjectArray
    const uint8_t lp[] = {0x48, 0x8D, 0x0D};
    uintptr_t sf = gTextStart; int sc = 0;
    while (sc < 10000) {
        size_t rem = gTextSize - (sf - gTextStart); if (rem < 16) break;
        uintptr_t r = ScanPattern(sf, rem, lp, "xxx"); if (!r) break; sf = r + 1; sc++;
        int32_t disp = *(int32_t*)(r + 3); uintptr_t tgt = r + 7 + disp;
        if (tgt < gDataStart || tgt >= gDataStart + gDataSize) continue;
        for (int off = 0; off <= 0x48; off += 8) {
            int32_t me=0, ne=0, mc=0, nc=0;
            if (!SafeRead32(tgt+off+0x10,&me)||!SafeRead32(tgt+off+0x14,&ne)||!SafeRead32(tgt+off+0x18,&mc)||!SafeRead32(tgt+off+0x1C,&nc)) continue;
            if (ne<10000||ne>2000000||nc<1||nc>500||me<ne||mc<nc||me>10000000||ne==32759) continue;
            if (ValidateObjArray(tgt+off, ne, nc, 24)) {
                gObjArrayBase = (void*)(tgt+off); gNumElements = ne; gNumChunks = nc;
                Log("GUObjectArray: 0x%llX (%d objects)\n", tgt+off, ne);
                goto found_guoa;
            }
        }
    }
    Log("GUObjectArray NOT FOUND\n"); return false;
found_guoa:

    // Compute FNameToString
    uintptr_t guoaStruct = (uintptr_t)gObjArrayBase - 0x10;
    uintptr_t fna = guoaStruct + FNAME_TOSTRING_OFFSET;
    if (fna < gTextStart || fna >= gTextStart + gTextSize) { Log("FNameToString out of range\n"); return false; }
    FName tn = {0,0}; FString tr = {0,0,0};
    FNameToString_fn fc = (FNameToString_fn)fna;
    __try { fc(&tn, tr); } __except(EXCEPTION_EXECUTE_HANDLER) { Log("FNameToString crashed\n"); return false; }
    if (!tr.Data || tr.Count < 4) { Log("FNameToString didn't return 'None'\n"); return false; }
    gFNameToString = fc;
    Log("FNameToString: 0x%llX OK\n", fna);

    return true;
}

// Find UFunction by name (returns first match with PropsSize in range)
static void* FindUFunc(const wchar_t* name, int minPS, int maxPS) {
    wchar_t nb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;
        if (wcscmp(nb, name) != 0) continue;
        int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
        if (ps >= minPS && ps <= maxPS) return o;
    }
    return nullptr;
}

// Find class by name
static void* FindClass(const wchar_t* name) {
    wchar_t nb[256], cb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256) || wcscmp(nb, name) != 0) continue;
        void* c = GetObjClass(o); if (!c) continue;
        if (GetObjectName(c, cb, 256) && (wcscmp(cb, L"Class")==0 || wcscmp(cb, L"BlueprintGeneratedClass")==0))
            return o;
    }
    return nullptr;
}

// Cache all needed UFunctions/objects (heavy, run on background thread)
static bool CacheObjects() {
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
    Log("Caching objects (%d total)...\n", gNumElements);

    gBeginSpawnFunc = FindUFunc(L"BeginDeferredActorSpawnFromClass", 100, 200);
    gFinishSpawnFunc = FindUFunc(L"FinishSpawningActor", 100, 200);
    gGetLocFunc = FindUFunc(L"K2_GetActorLocation", 20, 50);
    gSetLocFunc = FindUFunc(L"K2_SetActorLocation", 200, 400);
    gConstructionFunc = FindUFunc(L"UserConstructionScript", 0, 16);
    gBeginPlayFunc = FindUFunc(L"ReceiveBeginPlay", 0, 16);

    wchar_t nb[256];
    for (int i = 0; i < gNumElements && !gGameplayStaticsCDO; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (GetObjectName(o, nb, 256) && wcscmp(nb, L"Default__GameplayStatics") == 0)
            gGameplayStaticsCDO = o;
    }

    // Find ProcessEvent
    if (gGameplayStaticsCDO) {
        uintptr_t vt = 0; SafeRead64((uintptr_t)gGameplayStaticsCDO, &vt);
        if (vt) {
            uintptr_t fa = 0; SafeRead64(vt + PROCESS_EVENT_VTABLE_SLOT * 8, &fa);
            if (fa >= gTextStart && fa < gTextStart + gTextSize) {
                uint8_t* b = (uint8_t*)fa;
                if (IsSafeToRead((void*)fa, 4) && b[0]==0x40 && b[1]==0x55 && b[2]==0x56 && b[3]==0x57)
                    gProcessEventAddr = fa;
            }
        }
    }

    // Don't install tick hook here - do it in F9 when we know the level window is ready

    Log("Cached: BeginSpawn=%s FinishSpawn=%s GetLoc=%s SetLoc=%s Constr=%s BeginPlay=%s CDO=%s PE=%s Tick=%s\n",
        gBeginSpawnFunc?"OK":"NO", gFinishSpawnFunc?"OK":"NO", gGetLocFunc?"OK":"NO",
        gSetLocFunc?"OK":"NO", gConstructionFunc?"OK":"NO", gBeginPlayFunc?"OK":"NO",
        gGameplayStaticsCDO?"OK":"NO", gProcessEventAddr?"OK":"NO", "OK");

    return gBeginSpawnFunc && gFinishSpawnFunc && gGameplayStaticsCDO && gProcessEventAddr;
}

// Get actor location via ProcessEvent on any object
static bool GetActorLoc(void* actor, double* x, double* y, double* z) {
    if (!gGetLocFunc || !gProcessEventAddr || !actor) return false;
    __declspec(align(16)) uint8_t lp[128] = {};
    __try {
        ((ProcessEvent_fn)gProcessEventAddr)(actor, gGetLocFunc, lp);
        *x = *(double*)(lp); *y = *(double*)(lp+8); *z = *(double*)(lp+16);
        return (*x != 0 || *y != 0 || *z != 0);
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Get player PAWN location (not controller!)
static bool GetPlayerLoc(double* x, double* y, double* z) {
    if (!gGetLocFunc || !gProcessEventAddr) return false;
    wchar_t cb[256];
    // Try pawn classes first (the actual character in the world)
    const wchar_t* pawnClasses[] = {L"CharPlayer_Dungeon", L"CharPlayer", L"ValChar", nullptr};
    for (int pc = 0; pawnClasses[pc]; pc++) {
        for (int i = 0; i < gNumElements; i++) {
            void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
            void* c = GetObjClass(o); if (!c) continue;
            if (!GetObjectName(c, cb, 256) || !wcsstr(cb, pawnClasses[pc])) continue;
            if (GetActorLoc(o, x, y, z)) return true;
        }
    }
    return false;
}

// ============================================================
// Key handlers
// ============================================================

static bool IsKeyJustPressed(int vk) {
    static bool p[256] = {};
    bool c = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool w = p[vk]; p[vk] = c;
    return c && !w;
}

// ============================================================
// Main thread
// ============================================================

static DWORD WINAPI ModThreadProc(LPVOID) {
    Sleep(5000);
    char lp[MAX_PATH];
    if (GetEnvironmentVariableA("USERPROFILE", lp, MAX_PATH)) strcat_s(lp, "\\Desktop\\SanctifyMod.log");
    gLogFile = fopen(lp, "w");
    if (!gLogFile) return 0;

    Log("=== SanctifyEverywhere v0.9 ===\n");
    Log("Background thread ID: %lu\n", GetCurrentThreadId());

    HWND gw = nullptr;
    for (int i = 0; i < 120 && gRunning; i++) { Sleep(1000); gw = FindWindowA("UnrealWindow", nullptr); if (gw) break; }
    if (!gw) { Log("No window\n"); return 0; }

    Log("Window found, waiting for level load...\n");
    Sleep(10000);

    if (!InitUE()) { Log("InitUE FAILED\n"); goto idle; }
    if (!CacheObjects()) { Log("CacheObjects FAILED\n"); goto idle; }

    Log("\n*** READY! Press F9 to spawn anvil, F10 to capture position ***\n\n");

idle:
    while (gRunning) {
        Sleep(100);
        HWND fg = GetForegroundWindow();
        if (!fg) continue;
        char wc[64] = {}; GetClassNameA(fg, wc, 64);
        if (strcmp(wc, "UnrealWindow") != 0) continue;

        if (IsKeyJustPressed(VK_F9) && gProcessEventAddr && gBeginSpawnFunc) {
            Log("=== F9: SPAWN ===\n");

            // Refresh object count
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);

            // Re-cache any UFunctions that weren't found at startup
            if (!gSetLocFunc) gSetLocFunc = FindUFunc(L"K2_SetActorLocation", 200, 400);
            if (!gConstructionFunc) gConstructionFunc = FindUFunc(L"UserConstructionScript", 0, 16);
            if (!gBeginPlayFunc) gBeginPlayFunc = FindUFunc(L"ReceiveBeginPlay", 0, 16);
            Log("SetLoc=%s Constr=%s BeginPlay=%s\n",
                gSetLocFunc?"OK":"NO", gConstructionFunc?"OK":"NO", gBeginPlayFunc?"OK":"NO");

            // Find BP_SanctifiedAnvil_C
            void* anvilClass = FindClass(L"BP_SanctifiedAnvil_C");
            Log("AnvilClass: %s\n", anvilClass ? "OK" : "NOT LOADED");
            if (!anvilClass) { Log("BP_SanctifiedAnvil_C not loaded, cannot spawn\n"); continue; }

            // Find game world
            void* world = nullptr;
            wchar_t nb[256], cb[256];
            for (int i = 0; i < gNumElements && !world; i++) {
                void* o = GetUObject(i); if (!o) continue;
                void* c = GetObjClass(o); if (!c) continue;
                if (!GetObjectName(c, cb, 256) || wcscmp(cb, L"World") != 0) continue;
                if (!GetObjectName(o, nb, 256) || wcsncmp(nb, L"Default__", 9) == 0) continue;
                if (wcsstr(nb, L"RogueLite") || wcsstr(nb, L"Mission")) world = o;
            }
            Log("World: %s\n", world ? "OK" : "NO");
            if (!world) continue;

            // Get player location
            double px, py, pz;
            if (!GetPlayerLoc(&px, &py, &pz)) { Log("Player location failed\n"); continue; }
            Log("Player: (%.0f, %.0f, %.0f)\n", px, py, pz);

            // Spawn 300 units in front of player
            // Find ALL UFunctions fresh — log every detail for debugging
            Log("--- Finding UFunctions (detailed) ---\n");

            // Find ALL FinishSpawningActor matches
            void* freshBeginSpawn = nullptr;
            void* freshFinishSpawn = nullptr;
            void* freshSetLoc = nullptr;
            void* freshConstruction = nullptr;
            void* freshBeginPlay = nullptr;
            void* freshCDO = nullptr;
            {
                wchar_t nb[256], ob[256];
                for (int i = 0; i < gNumElements; i++) {
                    void* o = GetUObject(i); if (!o) continue;
                    if (!GetObjectName(o, nb, 256)) continue;

                    int32_t ps = 0;
                    SafeRead32((uintptr_t)o + 0x58, &ps);
                    void* outer = GetObjOuter(o);
                    if (outer) GetObjectName(outer, ob, 256); else ob[0] = 0;

                    if (wcscmp(nb, L"BeginDeferredActorSpawnFromClass") == 0) {
                        Log("  BeginDeferred: PS=%d outer='%ls' at 0x%llX\n", ps, ob, (uintptr_t)o);
                        if (ps >= 100 && ps <= 200 && !freshBeginSpawn) freshBeginSpawn = o;
                    }
                    if (wcscmp(nb, L"FinishSpawningActor") == 0) {
                        Log("  FinishSpawn: PS=%d outer='%ls' at 0x%llX\n", ps, ob, (uintptr_t)o);
                        if (ps >= 100 && ps <= 200 && !freshFinishSpawn) freshFinishSpawn = o;
                    }
                    if (wcscmp(nb, L"K2_SetActorLocation") == 0 && ps >= 200) {
                        Log("  SetLoc: PS=%d outer='%ls' at 0x%llX\n", ps, ob, (uintptr_t)o);
                        if (!freshSetLoc) freshSetLoc = o;
                    }
                    if (wcscmp(nb, L"UserConstructionScript") == 0) {
                        Log("  Construction: PS=%d outer='%ls' at 0x%llX\n", ps, ob, (uintptr_t)o);
                        // Prefer BP_SanctifiedAnvil_C's version over generic
                        if (wcsstr(ob, L"SanctifiedAnvil")) freshConstruction = o;
                        else if (ps <= 16 && !freshConstruction) freshConstruction = o;
                    }
                    if (wcscmp(nb, L"ReceiveBeginPlay") == 0) {
                        Log("  BeginPlay: PS=%d outer='%ls' at 0x%llX\n", ps, ob, (uintptr_t)o);
                        // Prefer BP_SanctifiedAnvil_C's version over generic
                        if (wcsstr(ob, L"SanctifiedAnvil")) freshBeginPlay = o;
                        else if (ps <= 16 && !freshBeginPlay) freshBeginPlay = o;
                    }
                    if (wcscmp(nb, L"Default__GameplayStatics") == 0) {
                        Log("  CDO: at 0x%llX\n", (uintptr_t)o);
                        if (!freshCDO) freshCDO = o;
                    }
                    if (wcscmp(nb, L"RerunConstructionScripts") == 0 || wcscmp(nb, L"RegisterAllComponents") == 0) {
                        Log("  %ls: PS=%d outer='%ls' at 0x%llX\n", nb, ps, ob, (uintptr_t)o);
                    }
                }
            }

            // Find ProcessEvent
            uintptr_t freshPE = 0;
            if (freshCDO) {
                uintptr_t vt = 0; SafeRead64((uintptr_t)freshCDO, &vt);
                if (vt) {
                    uintptr_t fa = 0; SafeRead64(vt + PROCESS_EVENT_VTABLE_SLOT * 8, &fa);
                    if (fa >= gTextStart && fa < gTextStart + gTextSize) freshPE = fa;
                }
                Log("  PE: 0x%llX\n", freshPE);
            }

            Log("Selected: BS=0x%llX FS=0x%llX SL=0x%llX CS=0x%llX BP=0x%llX CDO=0x%llX PE=0x%llX\n",
                (uintptr_t)freshBeginSpawn, (uintptr_t)freshFinishSpawn, (uintptr_t)freshSetLoc,
                (uintptr_t)freshConstruction, (uintptr_t)freshBeginPlay, (uintptr_t)freshCDO, freshPE);

            if (!freshBeginSpawn || !freshFinishSpawn || !freshCDO || !freshPE) {
                Log("Missing critical functions, cannot spawn\n");
                continue;
            }

            gSpawnReq.bpAnvilClass = anvilClass;
            gSpawnReq.gameWorld = world;
            gSpawnReq.spawnFunc = freshBeginSpawn;
            gSpawnReq.finishSpawnFunc = freshFinishSpawn;
            gSpawnReq.gameplayStaticsCDO = freshCDO;
            gSpawnReq.processEventAddr = freshPE;
            gSpawnReq.setLocFunc = freshSetLoc;
            gSpawnReq.constructionFunc = freshConstruction;
            gSpawnReq.beginPlayFunc = freshBeginPlay;
            gSpawnReq.rerunConstructionFunc = FindUFunc(L"RerunConstructionScripts", 0, 16);
            // Find ALL fabricators and create spawn positions for each
            int sc = 0;
            {
                wchar_t cb2[256];
                for (int i = 0; i < gNumElements && sc < 32; i++) {
                    void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                    void* c = GetObjClass(o); if (!c) continue;
                    if (!GetObjectName(c, cb2, 256) || wcscmp(cb2, L"BP_Fabricator_C") != 0) continue;
                    double fx, fy, fz;
                    if (GetActorLoc(o, &fx, &fy, &fz)) {
                        gSpawnReq.spawnX[sc] = fx + (-576.8);
                        gSpawnReq.spawnY[sc] = fy + 35.5;
                        gSpawnReq.spawnZ[sc] = fz + (-123.1);
                        Log("  Fab %d at (%.0f, %.0f, %.0f) -> Anvil at (%.0f, %.0f, %.0f)\n",
                            sc, fx, fy, fz, gSpawnReq.spawnX[sc], gSpawnReq.spawnY[sc], gSpawnReq.spawnZ[sc]);
                        sc++;
                    }
                }
            }
            if (sc == 0) {
                // Fallback: spawn near player
                gSpawnReq.spawnX[0] = px + 300;
                gSpawnReq.spawnY[0] = py;
                gSpawnReq.spawnZ[0] = pz;
                sc = 1;
                Log("No fabricators found, spawning near player\n");
            }
            gSpawnReq.spawnCount = sc;

            // Find SetSanctifiedAnvil and GameMode instance (background thread — safe)
            void* ssaFunc = FindUFunc(L"SetSanctifiedAnvil", 4, 16);
            void* gmInst = nullptr;
            if (ssaFunc) {
                wchar_t cb3[256];
                for (int i = 0; i < gNumElements && !gmInst; i++) {
                    void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                    void* c = GetObjClass(o); if (!c) continue;
                    if (!GetObjectName(c, cb3, 256)) continue;
                    if (wcsstr(cb3, L"GameMode") && wcsstr(cb3, L"Dungeon")) {
                        wchar_t on[256]; GetObjectName(o, on, 256);
                        Log("GameMode: '%ls' (class '%ls')\n", on, cb3);
                        gmInst = o;
                    }
                }
                if (!gmInst) {
                    // Broader search
                    for (int i = 0; i < gNumElements && !gmInst; i++) {
                        void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                        void* c = GetObjClass(o); if (!c) continue;
                        if (!GetObjectName(c, cb3, 256)) continue;
                        if (wcsstr(cb3, L"GameMode") && wcsstr(cb3, L"RogueLite")) {
                            wchar_t on[256]; GetObjectName(o, on, 256);
                            Log("GameMode (broad): '%ls' (class '%ls')\n", on, cb3);
                            gmInst = o;
                        }
                    }
                }
            }
            gSpawnReq.setSanctifiedAnvilFunc = ssaFunc;
            gSpawnReq.gameModeInstance = gmInst;
            Log("SetSanctifiedAnvil=%s GameMode=%s\n", ssaFunc?"OK":"NO", gmInst?"OK":"NO");

            // Find EnableSanctifiedAnvil on the safe room encounter manager
            void* enableSAFunc = FindUFunc(L"EnableSanctifiedAnvil", 100, 200);
            void* safeRoomMgr = nullptr;
            if (enableSAFunc) {
                wchar_t cb4[256];
                for (int i = 0; i < gNumElements && !safeRoomMgr; i++) {
                    void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                    void* c = GetObjClass(o); if (!c) continue;
                    if (!GetObjectName(c, cb4, 256)) continue;
                    if (wcsstr(cb4, L"EncounterManager") && wcsstr(cb4, L"SafeRoom")) {
                        wchar_t on[256]; GetObjectName(o, on, 256);
                        Log("SafeRoom EncounterMgr: '%ls' (class '%ls')\n", on, cb4);
                        safeRoomMgr = o;
                    }
                }
            }
            gSpawnReq.enableSAFunc = enableSAFunc;
            gSpawnReq.safeRoomMgr = safeRoomMgr;
            Log("EnableSanctifiedAnvil=%s SafeRoomMgr=%s\n", enableSAFunc?"OK":"NO", safeRoomMgr?"OK":"NO");

            // Find GameState and try to enable sanctification
            // GetSanctifyingAvailable returns FALSE in non-Zone4 — we need to set it TRUE
            void* getSanctAvailFunc = FindUFunc(L"GetSanctifyingAvailable", 0, 8);
            if (getSanctAvailFunc) {
                // Find its Outer to determine which class it belongs to
                void* gsOuter = GetObjOuter(getSanctAvailFunc);
                wchar_t gsOuterName[256] = {};
                if (gsOuter) GetObjectName(gsOuter, gsOuterName, 256);
                Log("GetSanctifyingAvailable owner: '%ls'\n", gsOuterName);

                // The bool property is likely right at the start of the return params (PropsSize=1)
                // If we can find the GameState instance and the property offset,
                // we can write TRUE directly.

                // For now, search for ANY function named "Set*Sanctif*" or "*Sanctif*Available"
                wchar_t fnb[256];
                Log("Searching for sanctify setter functions...\n");
                for (int i = 0; i < gNumElements; i++) {
                    void* o = GetUObject(i); if (!o) continue;
                    if (!GetObjectName(o, fnb, 256)) continue;
                    void* oc = GetObjClass(o); if (!oc) continue;
                    wchar_t ocn[256]; GetObjectName(oc, ocn, 256);
                    if (wcscmp(ocn, L"Function") != 0) continue;
                    if (wcsstr(fnb, L"Sanctif") && (wcsstr(fnb, L"Set") || wcsstr(fnb, L"Enable") || wcsstr(fnb, L"Available"))) {
                        int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
                        void* fo = GetObjOuter(o);
                        wchar_t fon[256] = {}; if (fo) GetObjectName(fo, fon, 256);
                        Log("  '%ls' PS=%d owner='%ls'\n", fnb, ps, fon);
                    }
                }
            }

            Log("Spawning %d anvils...\n", sc);
            gSpawnReq.resultReady = false;
            gSpawnReq.success = false;

            Log("Spawning at (%.0f, %.0f, %.0f)...\n", gSpawnReq.spawnX, gSpawnReq.spawnY, gSpawnReq.spawnZ);

            // Install window timer (the ONLY method proven to produce visible actors)
            TickHook::Uninstall(); // Clean up any previous timer
            if (!TickHook::Install(0)) {
                Log("FAILED to install window timer!\n");
                continue;
            }

            TickHook::QueueOnGameThread(GameThreadSpawn);

            for (int w = 0; w < 100 && !gSpawnReq.resultReady; w++) Sleep(50);
            if (gSpawnReq.success) Log("*** ANVIL VISIBLE! ***\n");
            else Log("Spawn may have failed\n");
        }

        if (IsKeyJustPressed(VK_F10) && gProcessEventAddr && gGetLocFunc) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            double x, y, z;
            if (GetPlayerLoc(&x, &y, &z))
                Log("F10 PAWN POSITION: (%.2f, %.2f, %.2f)\n", x, y, z);
            else
                Log("F10: Could not get pawn position\n");
        }

        if (IsKeyJustPressed(VK_F8) && gObjArrayBase && gFNameToString && gProcessEventAddr) {
            SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
            SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
            Log("\n=== F8: THOROUGH SANCTIFY DIAGNOSTICS (%d objects) ===\n", gNumElements);
            wchar_t nb[256], cb[256];
            ProcessEvent_fn pe = (ProcessEvent_fn)gProcessEventAddr;

            // 1. All sanctify-related boolean checks
            Log("--- Boolean State Checks ---\n");

            // GetSanctifyingAvailable on GameState
            void* gsAvail = FindUFunc(L"GetSanctifyingAvailable", 0, 8);
            if (gsAvail) {
                for (int i = 0; i < gNumElements; i++) {
                    void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                    void* c = GetObjClass(o); if (!c) continue;
                    if (!GetObjectName(c, cb, 256) || !wcsstr(cb, L"GameState") || !wcsstr(cb, L"RogueLite")) continue;
                    __declspec(align(16)) uint8_t p[64] = {};
                    __try { pe(o, gsAvail, p); Log("  GameState.GetSanctifyingAvailable() = %s\n", p[0]?"TRUE":"FALSE"); }
                    __except(EXCEPTION_EXECUTE_HANDLER) { Log("  GameState.GetSanctifyingAvailable() crashed\n"); }
                    break;
                }
            }

            // IsSanctifyingAvailable on ValPlayerSanctifyComponent
            void* isSanctAvail = FindUFunc(L"IsSanctifyingAvailable", 0, 8);
            if (isSanctAvail) {
                for (int i = 0; i < gNumElements; i++) {
                    void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                    void* c = GetObjClass(o); if (!c) continue;
                    if (!GetObjectName(c, cb, 256) || !wcsstr(cb, L"SanctifyComponent")) continue;
                    GetObjectName(o, nb, 256);
                    __declspec(align(16)) uint8_t p[64] = {};
                    __try { pe(o, isSanctAvail, p); Log("  '%ls'.IsSanctifyingAvailable() = %s\n", nb, p[0]?"TRUE":"FALSE"); }
                    __except(EXCEPTION_EXECUTE_HANDLER) { Log("  '%ls'.IsSanctifyingAvailable() crashed\n", nb); }
                }
            }

            // 2. Call ALL sanctify getter functions we know about
            Log("\n--- All Sanctify Getters ---\n");
            const wchar_t* getterNames[] = {
                L"CanSanctifyItem", L"CanSanctifyItemAt", L"IsItemSanctifiable",
                L"GetSanctificationsLeft", L"GetSanctifyItemCost",
                L"GetSanctifiableItemAssets", L"GetSanctifyingAvailable",
                L"GetCheckpointSanctifiedAnvil", L"GetSanctifyComponent",
                L"GetCheckpointSanctifiedAnvilData", nullptr
            };
            for (int g = 0; getterNames[g]; g++) {
                wchar_t fnb[256];
                for (int i = 0; i < gNumElements; i++) {
                    void* o = GetUObject(i); if (!o) continue;
                    if (!GetObjectName(o, fnb, 256) || wcscmp(fnb, getterNames[g]) != 0) continue;
                    void* oc = GetObjClass(o); if (!oc) continue;
                    wchar_t ocn[256]; GetObjectName(oc, ocn, 256);
                    if (wcscmp(ocn, L"Function") != 0) continue;
                    int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
                    void* fo = GetObjOuter(o);
                    wchar_t fon[256] = {}; if (fo) GetObjectName(fo, fon, 256);
                    Log("  %ls: PS=%d owner='%ls'\n", getterNames[g], ps, fon);
                    break;
                }
            }

            // 3. Anvil instances — dump first 0x100 bytes to compare native vs spawned
            Log("\n--- Anvil Instance Memory Comparison ---\n");
            int anvilIdx = 0;
            for (int i = 0; i < gNumElements && anvilIdx < 3; i++) {
                void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                void* c = GetObjClass(o); if (!c) continue;
                if (!GetObjectName(c, cb, 256) || wcscmp(cb, L"BP_SanctifiedAnvil_C") != 0) continue;
                GetObjectName(o, nb, 256);
                double ax, ay, az;
                GetActorLoc(o, &ax, &ay, &az);
                Log("  Anvil %d: '%ls' at (%.0f, %.0f, %.0f) addr=0x%llX\n", anvilIdx, nb, ax, ay, az, (uintptr_t)o);

                // Dump key property range (skip UObject header, focus on actor-specific data)
                if (IsSafeToRead(o, 0x400)) {
                    uint8_t* mem = (uint8_t*)o;
                    // Dump offsets 0x200-0x400 where game-specific properties usually live
                    for (int row = 0x200; row < 0x400; row += 32) {
                        // Only log rows that have non-zero data
                        bool hasData = false;
                        for (int b = 0; b < 32; b++) if (mem[row+b]) { hasData = true; break; }
                        if (!hasData) continue;
                        Log("    +0x%03X:", row);
                        for (int b = 0; b < 32; b++) Log(" %02X", mem[row+b]);
                        Log("\n");
                    }
                }
                anvilIdx++;
            }

            // 4. Player Sanctify Component — call getters on it
            Log("\n--- Player Sanctify Component State ---\n");
            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                void* c = GetObjClass(o); if (!c) continue;
                if (!GetObjectName(c, cb, 256) || !wcsstr(cb, L"ValPlayerSanctifyComponent")) continue;
                GetObjectName(o, nb, 256);
                void* outer = GetObjOuter(o);
                wchar_t on[256] = {}; if (outer) GetObjectName(outer, on, 256);
                Log("  '%ls' outer='%ls' at 0x%llX\n", nb, on, (uintptr_t)o);

                // Try GetSanctifyComponent on the player controller
                void* getSanctComp = FindUFunc(L"GetSanctifyComponent", 4, 16);
                if (getSanctComp && outer) {
                    __declspec(align(16)) uint8_t gsc[64] = {};
                    __try {
                        pe(outer, getSanctComp, gsc);
                        void* comp = *(void**)(gsc + 0);
                        Log("    GetSanctifyComponent() = 0x%llX %s\n",
                            (uintptr_t)comp, (comp == o) ? "(MATCHES)" : "(DIFFERENT!)");
                    } __except(EXCEPTION_EXECUTE_HANDLER) {}
                }

                // Dump component memory too
                if (IsSafeToRead(o, 0x200)) {
                    uint8_t* mem = (uint8_t*)o;
                    for (int row = 0x80; row < 0x200; row += 32) {
                        bool hasData = false;
                        for (int b = 0; b < 32; b++) if (mem[row+b]) { hasData = true; break; }
                        if (!hasData) continue;
                        Log("    +0x%03X:", row);
                        for (int b = 0; b < 32; b++) Log(" %02X", mem[row+b]);
                        Log("\n");
                    }
                }
            }

            // 5. Safe room encounter manager state
            Log("\n--- SafeRoom EncounterManager ---\n");
            for (int i = 0; i < gNumElements; i++) {
                void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
                void* c = GetObjClass(o); if (!c) continue;
                if (!GetObjectName(c, cb, 256) || !wcsstr(cb, L"EncounterManager") || !wcsstr(cb, L"SafeRoom")) continue;
                GetObjectName(o, nb, 256);
                Log("  '%ls' (class '%ls')\n", nb, cb);

                // Try CheckCanEnableSanctifiedAnvil
                void* checkFunc = FindUFunc(L"CheckCanEnableSanctifiedAnvil", 30, 60);
                if (checkFunc) {
                    __declspec(align(16)) uint8_t cfp[64] = {};
                    __try {
                        pe(o, checkFunc, cfp);
                        Log("    CheckCanEnableSanctifiedAnvil() returned (first byte = %d)\n", cfp[0]);
                    } __except(EXCEPTION_EXECUTE_HANDLER) {
                        Log("    CheckCanEnableSanctifiedAnvil() crashed\n");
                    }
                }
                break;
            }

            Log("\n=== END DIAGNOSTICS ===\n");
        }
    }
    return 0;
}

void Mod::Start() { gRunning = true; gModThread = CreateThread(nullptr, 0, ModThreadProc, nullptr, 0, nullptr); }
void Mod::Stop() {
    gRunning = false;
    TickHook::Uninstall();
    if (gModThread) { WaitForSingleObject(gModThread, 5000); CloseHandle(gModThread); }
    if (gLogFile) { fclose(gLogFile); gLogFile = nullptr; }
}
