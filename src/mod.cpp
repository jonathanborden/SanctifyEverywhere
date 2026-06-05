#include "mod.h"
#include "tick_hook.h"
#include <cstdio>
#include <atomic>
#include <cstring>

// ============================================================
// Globals
// ============================================================

static std::atomic<bool> gRunning{false};
static HANDLE gModThread = nullptr;
FILE* gLogFile = nullptr;
static DWORD gLogLastTick = 0; // ms tick of previous Log() call — drives the "+Δms" prefix

extern HMODULE gThisModule; // set in dllmain.cpp DLL_PROCESS_ATTACH

// Build an absolute path next to the deployed DLL (game's Win64 folder).
// Falls back to current dir if module lookup fails.
static void BuildLogPath(const char* fname, char* out, size_t outsz) {
    out[0] = 0;
    char mp[MAX_PATH] = {};
    if (gThisModule && GetModuleFileNameA(gThisModule, mp, MAX_PATH)) {
        char* slash = strrchr(mp, '\\');
        if (slash) *(slash + 1) = 0;
        strcpy_s(out, outsz, mp);
        strcat_s(out, outsz, fname);
    } else {
        strcpy_s(out, outsz, ".\\");
        strcat_s(out, outsz, fname);
    }
}

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

// Cached pointers needed by the auto path (found once at startup).
// K2_GetActorLocation is reused every frame to read fabricator positions.
static void* gGetLocFunc = nullptr;

// ============================================================
// Constants (Deadzone Rogue v1.4 - stable across ASLR)
// ============================================================

static const int64_t FNAME_TOSTRING_OFFSET = -0x0A27FB50;
static const int PROCESS_EVENT_VTABLE_SLOT = 0x4C;
static const int BEGIN_SPAWN_RETVAL_OFFSET = 0x88;
static const int FINISH_SPAWN_RETVAL_OFFSET = 120;

// Brute-forced property offsets (constant per game version)
static const int GAMESTATE_SANCTIFY_BOOL_OFFSET = 0x592;
static const int PLAYER_SANCTIFY_BOOL_OFFSET = 0x13A;

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
static bool GetFNameStr(const FName* n, wchar_t* buf, int sz);

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
    void* gameStateInst;
    void* playerSanctifyComp;
    uintptr_t processEventAddr;
    double spawnX[32], spawnY[32], spawnZ[32];
    int spawnCount;
    volatile bool resultReady;
    volatile bool success;
} gSpawnReq = {};

// ============================================================
// Vial HUD widget — AddCurrencyWidget on the live HUD container
// ============================================================

static struct {
    void* container;             // live WBP_Game_Currencies_C
    void* addCurrencyWidgetFunc; // AddCurrencyWidget on WBP_Game_Currencies_C
    uintptr_t processEventAddr;
    uint8_t currencyId[16];
    volatile bool done;
    volatile bool success;
} gAddReq = {};

static void GameThreadAddCurrency() {
    ProcessEvent_fn pe = (ProcessEvent_fn)gAddReq.processEventAddr;
    __declspec(align(16)) uint8_t p[256] = {};
    memcpy(p, gAddReq.currencyId, 16); // FPrimaryAssetId at param offset 0
    __try {
        pe(gAddReq.container, gAddReq.addCurrencyWidgetFunc, p);
        Log("[GT-Add] AddCurrencyWidget OK (retptr@+16=0x%llX, @+24=0x%llX)\n",
            *(uintptr_t*)(p + 16), *(uintptr_t*)(p + 24));
        gAddReq.success = true;
    } __except(EXCEPTION_EXECUTE_HANDLER) {
        Log("[GT-Add] AddCurrencyWidget CRASHED\n");
    }
    gAddReq.done = true;
}

// Find an FName by locating any loaded UObject whose name resolves to `target`.
// Any FName resolving to a given string shares the same ComparisonIndex, so this
// yields the exact FName needed to rebuild an FPrimaryAssetId — no harvest required.
static bool FindFNameByObjectName(const wchar_t* target, FName* out) {
    wchar_t nb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o || !IsSafeToRead(o, 0x20)) continue;
        FName n;
        if (!SafeRead32((uintptr_t)o + 0x18, &n.ComparisonIndex)) continue;
        if (!SafeRead32((uintptr_t)o + 0x1C, &n.Number)) continue;
        if (!GetFNameStr(&n, nb, 256)) continue;
        if (wcscmp(nb, target) == 0) { *out = n; return true; }
    }
    return false;
}

// Rebuild the SanctityToken FPrimaryAssetId = {type FName, name FName} from names.
static bool BuildSanctityCurrencyId(uint8_t out[16]) {
    FName typeName, assetName;
    if (!FindFNameByObjectName(L"ValItemAsset", &typeName)) return false;
    if (!FindFNameByObjectName(L"Item_Currency_SanctityToken", &assetName)) return false;
    memset(out, 0, 16);
    memcpy(out + 0, &typeName, 8);
    memcpy(out + 8, &assetName, 8);
    return true;
}

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
    Log("[GT] Spawning %d anvils\n", gSpawnReq.spawnCount);
    ProcessEvent_fn pe = (ProcessEvent_fn)gSpawnReq.processEventAddr;

    for (int i = 0; i < gSpawnReq.spawnCount; i++) {
        SpawnOneAnvil(pe, gSpawnReq.spawnX[i], gSpawnReq.spawnY[i], gSpawnReq.spawnZ[i]);
    }

    // Set GameState sanctify bool using cached offset (instant — no scanning)
    if (gSpawnReq.gameStateInst && IsSafeToRead(gSpawnReq.gameStateInst, GAMESTATE_SANCTIFY_BOOL_OFFSET + 1)) {
        ((uint8_t*)gSpawnReq.gameStateInst)[GAMESTATE_SANCTIFY_BOOL_OFFSET] = 1;
        Log("[GT] GameState+0x%X set TRUE\n", GAMESTATE_SANCTIFY_BOOL_OFFSET);
    }

    // Set player sanctify component bool using cached offset (instant)
    if (gSpawnReq.playerSanctifyComp && IsSafeToRead(gSpawnReq.playerSanctifyComp, PLAYER_SANCTIFY_BOOL_OFFSET + 1)) {
        ((uint8_t*)gSpawnReq.playerSanctifyComp)[PLAYER_SANCTIFY_BOOL_OFFSET] = 1;
        Log("[GT] PlayerSanctifyComp+0x%X set TRUE\n", PLAYER_SANCTIFY_BOOL_OFFSET);
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
    SYSTEMTIME st; GetLocalTime(&st);
    DWORD now = GetTickCount();
    DWORD delta = gLogLastTick ? (now - gLogLastTick) : 0;
    gLogLastTick = now;
    fprintf(gLogFile, "[%02u:%02u:%02u.%03u +%lums] ",
        st.wHour, st.wMinute, st.wSecond, st.wMilliseconds, (unsigned long)delta);
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

// Minimal startup cache: resolve K2_GetActorLocation (used to read fabricator
// positions) and ProcessEvent (via the GameplayStatics CDO vtable). DoSpawnAnvils
// re-finds the spawn functions fresh per level load, so nothing else is cached here.
static bool CacheObjects() {
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    Log("Startup scan (%d objects)...\n", gNumElements);

    void* gameplayStaticsCDO = nullptr;
    wchar_t nb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;
        if (!gGetLocFunc && wcscmp(nb, L"K2_GetActorLocation") == 0) {
            int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 20 && ps <= 50) gGetLocFunc = o;
        }
        if (!gameplayStaticsCDO && wcscmp(nb, L"Default__GameplayStatics") == 0)
            gameplayStaticsCDO = o;
    }

    if (gameplayStaticsCDO) {
        uintptr_t vt = 0; SafeRead64((uintptr_t)gameplayStaticsCDO, &vt);
        if (vt) {
            uintptr_t fa = 0; SafeRead64(vt + PROCESS_EVENT_VTABLE_SLOT * 8, &fa);
            if (fa >= gTextStart && fa < gTextStart + gTextSize) {
                uint8_t* b = (uint8_t*)fa;
                if (IsSafeToRead((void*)fa, 4) && b[0]==0x40 && b[1]==0x55 && b[2]==0x56 && b[3]==0x57)
                    gProcessEventAddr = fa;
            }
        }
    }

    Log("Cached: GetLoc=%s PE=%s\n", gGetLocFunc?"OK":"NO", gProcessEventAddr?"OK":"NO");
    return gGetLocFunc && gProcessEventAddr;
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

// ============================================================
// Reusable actions — driven by the auto-on-load path
// ============================================================

static const int CURRENCY_ID_OFFSET = 0x55C; // FPrimaryAssetId in WBP_Game_Currency_C body

// True when the current level already provides sanctify natively (zone 4):
// the SafeRoom EncounterManager exists, or GameState bSanctifyingAvailable is set.
static bool IsZone4Native() {
    DWORD t0 = GetTickCount();
    wchar_t cb[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
        if (wcsstr(cb, L"EncounterManager") && wcsstr(cb, L"SafeRoom")) {
            Log("[Zone4?] YES via SafeRoom EncounterManager class=%ls (scan %lums)\n",
                cb, (unsigned long)(GetTickCount() - t0));
            return true;
        }
        if (wcsstr(cb, L"GameState") && wcsstr(cb, L"RogueLite")) {
            int32_t v = 0;
            if (SafeRead32((uintptr_t)o + GAMESTATE_SANCTIFY_BOOL_OFFSET, &v) && (v & 0xFF)) {
                Log("[Zone4?] YES via GameState+0x%X=true class=%ls (scan %lums)\n",
                    GAMESTATE_SANCTIFY_BOOL_OFFSET, cb, (unsigned long)(GetTickCount() - t0));
                return true;
            }
        }
    }
    Log("[Zone4?] NO (scan %lums) — proceeding with anvils+vials\n",
        (unsigned long)(GetTickCount() - t0));
    return false;
}

// Find the active gameplay World instance (a fresh one is created each level load).
// `verbose` dumps every World candidate and the gameplay filter result — used by the
// auto loop when no matching world is found, to identify safe-room / unknown maps.
static void* FindCurrentWorld(bool verbose = false) {
    DWORD t0 = GetTickCount();
    wchar_t cb[256], nb[256], ob[256];
    int worldCount = 0;
    void* result = nullptr;
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
        if (wcscmp(cb, L"World") != 0) continue;
        worldCount++;
        if (!GetObjectName(o, nb, 256)) continue;
        bool match = wcsstr(nb, L"RogueLite") || wcsstr(nb, L"Mission");
        if (verbose && worldCount <= 16) {
            void* ou = GetObjOuter(o);
            ob[0] = 0; if (ou) GetObjectName(ou, ob, 256);
            Log("[FindWorld] cand #%d name=%ls outer=%ls match=%s\n",
                worldCount, nb, ob, match ? "Y" : "N");
        }
        if (match && !result) result = o;
    }
    if (verbose) {
        Log("[FindWorld] %d World candidates, %s (scan %lums)\n",
            worldCount, result ? "MATCHED" : "no gameplay match",
            (unsigned long)(GetTickCount() - t0));
    }
    return result;
}

// True if a SanctityToken currency widget already exists (native zone 4, or already added).
static bool SanctityWidgetPresent() {
    wchar_t cb[256], ns[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
        if (wcscmp(cb, L"WBP_Game_Currency_C") != 0) continue;
        uintptr_t a = (uintptr_t)o + CURRENCY_ID_OFFSET + 8; // name FName
        if (!IsSafeToRead((void*)a, 8)) continue;
        FName n; SafeRead32(a + 0, &n.ComparisonIndex); SafeRead32(a + 4, &n.Number);
        if (GetFNameStr(&n, ns, 256) && wcsstr(ns, L"SanctityToken")) return true;
    }
    return false;
}

// Spawn sanctified anvils at every fabricator. Returns 0 = not ready (retry later),
// 1 = attempted (do not retry — avoids duplicate anvils). Waits until fabricators
// have loaded before spawning.
static int DoSpawnAnvils() {
    DWORD spawnT0 = GetTickCount();
    Log("=== SPAWN ANVILS === (gNumElements before refresh)\n");
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);
    Log("[Spawn] scanning %d objects, %d chunks\n", gNumElements, gNumChunks);

    void* anvilClass = nullptr;
    void* world = nullptr;
    void* freshBS = nullptr, *freshFS = nullptr, *freshSL = nullptr;
    void* freshCS = nullptr, *freshBP = nullptr, *freshCDO = nullptr;
    void* ssaFunc = nullptr, *esaFunc = nullptr;
    void* gmInst = nullptr, *srmInst = nullptr, *gsInst = nullptr, *pscInst = nullptr;
    void* fabInstances[32] = {}; int fabCount = 0;
    wchar_t nb[256], cb[256], ob[256];

    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;

        int32_t ps = 0;
        if (wcscmp(nb, L"BeginDeferredActorSpawnFromClass") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 100 && ps <= 200 && !freshBS) freshBS = o;
        }
        else if (wcscmp(nb, L"FinishSpawningActor") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 100 && ps <= 200 && !freshFS) freshFS = o;
        }
        else if (wcscmp(nb, L"K2_SetActorLocation") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 200 && !freshSL) freshSL = o;
        }
        else if (wcscmp(nb, L"UserConstructionScript") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            void* ou = GetObjOuter(o);
            if (ou) { GetObjectName(ou, ob, 256); if (wcsstr(ob, L"SanctifiedAnvil")) freshCS = o; }
            if (ps <= 16 && !freshCS) freshCS = o;
        }
        else if (wcscmp(nb, L"ReceiveBeginPlay") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            void* ou = GetObjOuter(o);
            if (ou) { GetObjectName(ou, ob, 256); if (wcsstr(ob, L"SanctifiedAnvil")) freshBP = o; }
            if (ps <= 16 && !freshBP) freshBP = o;
        }
        else if (wcscmp(nb, L"Default__GameplayStatics") == 0) { if (!freshCDO) freshCDO = o; }
        else if (wcscmp(nb, L"SetSanctifiedAnvil") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 4 && ps <= 16 && !ssaFunc) ssaFunc = o;
        }
        else if (wcscmp(nb, L"EnableSanctifiedAnvil") == 0) {
            SafeRead32((uintptr_t)o + 0x58, &ps);
            if (ps >= 100 && !esaFunc) esaFunc = o;
        }

        if (wcsncmp(nb, L"Default__", 9) == 0) continue;

        void* c = GetObjClass(o); if (!c) continue;
        if (!GetObjectName(c, cb, 256)) continue;

        if (!anvilClass && (wcscmp(cb, L"BlueprintGeneratedClass") == 0 || wcscmp(cb, L"Class") == 0)
            && wcscmp(nb, L"BP_SanctifiedAnvil_C") == 0) anvilClass = o;

        if (!world && wcscmp(cb, L"World") == 0 && (wcsstr(nb, L"RogueLite") || wcsstr(nb, L"Mission")))
            world = o;

        if (wcscmp(cb, L"BP_Fabricator_C") == 0 && fabCount < 32)
            fabInstances[fabCount++] = o;

        if (!gmInst && wcsstr(cb, L"GameMode") && (wcsstr(cb, L"Dungeon") || wcsstr(cb, L"RogueLite")))
            gmInst = o;
        if (!srmInst && wcsstr(cb, L"EncounterManager") && wcsstr(cb, L"SafeRoom"))
            srmInst = o;
        if (!gsInst && wcsstr(cb, L"GameState") && wcsstr(cb, L"RogueLite"))
            gsInst = o;
        if (!pscInst && wcsstr(cb, L"ValPlayerSanctifyComponent") && !wcsstr(nb, L"GEN_VARIABLE"))
            pscInst = o;
    }

    uintptr_t freshPE = 0;
    if (freshCDO) {
        uintptr_t vt = 0; SafeRead64((uintptr_t)freshCDO, &vt);
        if (vt) { uintptr_t fa = 0; SafeRead64(vt + PROCESS_EVENT_VTABLE_SLOT * 8, &fa);
            if (fa >= gTextStart && fa < gTextStart + gTextSize) freshPE = fa; }
    }

    Log("[Spawn] scan done in %lums: Anvil=%s World=%s CDO=%s BS=%s FS=%s SL=%s CS=%s BP=%s PE=%s "
        "SetSA=%s EnableSA=%s GM=%s SafeRoom=%s GS=%s PSC=%s Fabs=%d\n",
        (unsigned long)(GetTickCount() - spawnT0),
        anvilClass?"OK":"NO", world?"OK":"NO", freshCDO?"OK":"NO", freshBS?"OK":"NO", freshFS?"OK":"NO",
        freshSL?"OK":"NO", freshCS?"OK":"NO", freshBP?"OK":"NO", freshPE?"OK":"NO",
        ssaFunc?"OK":"NO", esaFunc?"OK":"NO", gmInst?"OK":"NO", srmInst?"OK":"NO",
        gsInst?"OK":"NO", pscInst?"OK":"NO", fabCount);

    if (!anvilClass || !world || !freshBS || !freshFS || !freshCDO || !freshPE) {
        Log("[Spawn] missing critical objects — NOT READY, will retry\n");
        return 0;
    }

    int sc = 0;
    for (int fi = 0; fi < fabCount && sc < 32; fi++) {
        double fx, fy, fz;
        if (GetActorLoc(fabInstances[fi], &fx, &fy, &fz)) {
            gSpawnReq.spawnX[sc] = fx - 576.8;
            gSpawnReq.spawnY[sc] = fy + 35.5;
            gSpawnReq.spawnZ[sc] = fz - 123.1;
            sc++;
        }
    }
    if (sc == 0) {
        Log("[Spawn] %d fabricators found but 0 reachable locations — NOT READY, will retry\n", fabCount);
        return 0;
    }
    gSpawnReq.spawnCount = sc;
    Log("[Spawn] resolved %d/%d fabricator locations\n", sc, fabCount);

    gSpawnReq.bpAnvilClass = anvilClass;
    gSpawnReq.gameWorld = world;
    gSpawnReq.spawnFunc = freshBS;
    gSpawnReq.finishSpawnFunc = freshFS;
    gSpawnReq.gameplayStaticsCDO = freshCDO;
    gSpawnReq.processEventAddr = freshPE;
    gSpawnReq.setLocFunc = freshSL;
    gSpawnReq.constructionFunc = freshCS;
    gSpawnReq.beginPlayFunc = freshBP;
    gSpawnReq.rerunConstructionFunc = nullptr;
    gSpawnReq.setSanctifiedAnvilFunc = ssaFunc;
    gSpawnReq.gameModeInstance = gmInst;
    gSpawnReq.enableSAFunc = esaFunc;
    gSpawnReq.safeRoomMgr = srmInst;
    gSpawnReq.gameStateInst = gsInst;
    gSpawnReq.playerSanctifyComp = pscInst;

    Log("Spawning %d anvils...\n", sc);
    gSpawnReq.resultReady = false;
    gSpawnReq.success = false;

    TickHook::Uninstall();
    if (!TickHook::Install(0)) { Log("FAILED to install window timer!\n"); return 1; }

    DWORD gtT0 = GetTickCount();
    TickHook::QueueOnGameThread(GameThreadSpawn);
    int waited = 0;
    for (int w = 0; w < 100 && !gSpawnReq.resultReady; w++) { Sleep(50); waited++; }
    Log("[Spawn] game-thread wait: %dms (result=%s)\n",
        waited * 50, gSpawnReq.resultReady ? (gSpawnReq.success ? "OK" : "FAIL") : "TIMEOUT");
    if (gSpawnReq.success) Log("[Spawn] *** ANVIL VISIBLE! *** (total %lums)\n",
        (unsigned long)(GetTickCount() - spawnT0));
    else Log("[Spawn] may have failed (total %lums, game-thread %lums)\n",
        (unsigned long)(GetTickCount() - spawnT0), (unsigned long)(GetTickCount() - gtT0));
    return 1;
}

// Add the SanctityToken vial widget to the live HUD. Returns 0 = not ready (retry),
// 1 = added, 2 = already present (skip).
static int DoAddVials() {
    DWORD vialT0 = GetTickCount();
    Log("=== ADD VIALS ===\n");
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);

    DWORD t0 = GetTickCount();
    if (SanctityWidgetPresent()) {
        Log("[Vials] SanctityToken widget already present — skip (check %lums)\n",
            (unsigned long)(GetTickCount() - t0));
        return 2;
    }
    Log("[Vials] widget-present check: %lums (not present)\n",
        (unsigned long)(GetTickCount() - t0));

    t0 = GetTickCount();
    uint8_t currencyId[16] = {};
    if (!BuildSanctityCurrencyId(currencyId)) {
        Log("[Vials] could not resolve SanctityToken CurrencyId — NOT READY, will retry "
            "(scan %lums)\n", (unsigned long)(GetTickCount() - t0));
        return 0;
    }
    Log("[Vials] CurrencyId built (%lums, type/name FNames resolved)\n",
        (unsigned long)(GetTickCount() - t0));

    if (!gProcessEventAddr) { Log("[Vials] ProcessEvent not available\n"); return 0; }

    t0 = GetTickCount();
    void* addCurrFunc = nullptr;
    void* container = nullptr;
    wchar_t nb[256], cb[256], ob[256];
    for (int i = 0; i < gNumElements; i++) {
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;
        if (!addCurrFunc && wcscmp(nb, L"AddCurrencyWidget") == 0) {
            void* ou = GetObjOuter(o);
            if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currencies_C"))
                addCurrFunc = o;
        }
        if (IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c) continue;
        if (!GetObjectName(c, cb, 256)) continue;
        if (!container && wcscmp(cb, L"WBP_Game_Currencies_C") == 0 && wcscmp(nb, L"Currencies") == 0) {
            void* ou = GetObjOuter(o);
            void* ouou = ou ? GetObjOuter(ou) : nullptr;
            if (ouou && GetObjectName(ouou, ob, 256) && wcsstr(ob, L"UI_Game_Visor_C")) container = o;
        }
    }
    Log("[Vials] HUD scan %lums: addFn=%s container=%s\n",
        (unsigned long)(GetTickCount() - t0),
        addCurrFunc?"OK":"NO", container?"OK":"NO");

    if (!addCurrFunc || !container) {
        Log("[Vials] HUD container/func not ready — NOT READY, will retry\n");
        return 0;
    }

    gAddReq.container = container;
    gAddReq.addCurrencyWidgetFunc = addCurrFunc;
    gAddReq.processEventAddr = gProcessEventAddr;
    memcpy(gAddReq.currencyId, currencyId, 16);
    gAddReq.done = false; gAddReq.success = false;

    TickHook::Uninstall();
    if (!TickHook::Install(0)) { Log("[Vials] tick hook install failed\n"); return 0; }
    DWORD gtT0 = GetTickCount();
    TickHook::QueueOnGameThread(GameThreadAddCurrency);
    int waited = 0;
    for (int w = 0; w < 100 && !gAddReq.done; w++) { Sleep(50); waited++; }
    Log("[Vials] game-thread wait: %dms (done=%s success=%s)\n",
        waited * 50, gAddReq.done?"Y":"N", gAddReq.success?"Y":"N");
    Log("[Vials] %s (total %lums)\n",
        gAddReq.success ? "*** ADDED OK ***" : "ADD FAILED",
        (unsigned long)(GetTickCount() - vialT0));
    return gAddReq.success ? 1 : 0;
}

// ============================================================
// Main thread
// ============================================================

static DWORD WINAPI ModThreadProc(LPVOID) {
    Sleep(5000);
    char lp[MAX_PATH];
    BuildLogPath("SanctifyMod.log", lp, MAX_PATH);
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

    Log("\n*** READY! Auto-spawning anvils + vials on each zone 1-3 load ***\n\n");

idle:
    while (gRunning) {
        Sleep(100);

        // ---- AUTO: spawn anvils + add vials on each new level load (zones 1-3) ----
        // Runs regardless of window focus, throttled to ~1s. Detects a new gameplay
        // World, waits ~3s for it to settle, skips zone 4 (native), and otherwise
        // applies anvils + vials once per world. Manual F9/F12 remain as fallback.
        if (gObjArrayBase) {
            static void* autoWorld = nullptr;
            static bool  autoAnvilsDone = false;
            static bool  autoVialsDone = false;
            static bool  autoSkipZone4 = false;
            static int   autoAttempts = 0;
            static DWORD autoFirstSeen = 0;
            static DWORD autoNextScan = 0;
            static DWORD autoNoWorldSince = 0;
            static DWORD autoLastNoWorldDump = 0;
            static bool  autoIdleLogged = false;

            DWORD now = GetTickCount();
            if ((int)(now - autoNextScan) >= 0) {
                autoNextScan = now + 1000; // throttle full scans to ~1s

                SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
                SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);

                // If we've had no world for a while, switch to verbose mode so we can
                // see what World objects ARE loaded (the gameplay filter may be missing
                // a safe-room / unknown map name).
                bool verbose = false;
                if (!autoWorld && autoNoWorldSince
                    && (int)(now - autoNoWorldSince) >= 5000
                    && (int)(now - autoLastNoWorldDump) >= 10000) {
                    verbose = true;
                    autoLastNoWorldDump = now;
                    Log("[Auto] no world for %lums — dumping all World candidates\n",
                        (unsigned long)(now - autoNoWorldSince));
                }

                DWORD scanT0 = GetTickCount();
                void* w = FindCurrentWorld(verbose);
                DWORD scanMs = GetTickCount() - scanT0;
                if (w != autoWorld) {
                    Log("[Auto] World CHANGED: 0x%llX -> 0x%llX (FindWorld scan %lums)\n",
                        (uintptr_t)autoWorld, (uintptr_t)w, (unsigned long)scanMs);
                    autoWorld = w;
                    autoAnvilsDone = autoVialsDone = autoSkipZone4 = false;
                    autoAttempts = 0;
                    autoFirstSeen = now;
                    autoIdleLogged = false;
                    if (w) {
                        autoNoWorldSince = 0;
                        wchar_t wn[256] = {}, on[256] = {};
                        GetObjectName(w, wn, 256);
                        void* ou = GetObjOuter(w); if (ou) GetObjectName(ou, on, 256);
                        Log("[Auto] New world name=%ls outer=%ls — will apply after 3s settle\n", wn, on);
                    } else {
                        autoNoWorldSince = now;
                        Log("[Auto] No gameplay world (menu/loading/unknown-map)\n");
                    }
                }

                if (autoWorld && !autoSkipZone4 && !(autoAnvilsDone && autoVialsDone)
                    && (int)(now - autoFirstSeen) >= 3000) {

                    if (IsZone4Native()) {
                        autoSkipZone4 = true;
                        Log("[Auto] Zone 4 (native sanctify) — skipping auto spawn/vials\n");
                    } else {
                        autoAttempts++;
                        Log("[Auto] --- attempt %d/20 (anvils=%d vials=%d) ---\n",
                            autoAttempts, autoAnvilsDone?1:0, autoVialsDone?1:0);
                        if (!autoAnvilsDone) {
                            int r = DoSpawnAnvils(); // waits until fabricators have loaded
                            if (r == 1) { autoAnvilsDone = true; Log("[Auto] anvils marked done\n"); }
                            else Log("[Auto] anvils returned %d — will retry\n", r);
                        }
                        if (!autoVialsDone) {
                            int r = DoAddVials();
                            if (r != 0) { autoVialsDone = true; Log("[Auto] vials marked done (r=%d)\n", r); }
                            else Log("[Auto] vials returned %d — will retry\n", r);
                        }
                        if (autoAttempts >= 20 && !(autoAnvilsDone && autoVialsDone)) {
                            Log("[Auto] *** GAVE UP after %d attempts (anvils=%d vials=%d) — "
                                "see scan logs above for missing objects ***\n",
                                autoAttempts, autoAnvilsDone?1:0, autoVialsDone?1:0);
                            autoAnvilsDone = autoVialsDone = true;
                        }
                    }
                }

                // Heartbeat: log once when we transition into the idle state so the user
                // can correlate "fan got quieter" with mod being done for this world.
                if (autoWorld && !autoIdleLogged
                    && (autoSkipZone4 || (autoAnvilsDone && autoVialsDone))) {
                    autoIdleLogged = true;
                    Log("[Auto] *** IDLING for this world — only world-change scan runs now "
                        "(fan should be quieter) ***\n");
                }
            }
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
