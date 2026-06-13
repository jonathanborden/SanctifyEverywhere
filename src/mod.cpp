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

// FNamePool (GNames) FNameEntryAllocator base — found by .data scan in InitUE.
// Layout: +0x8 CurrentBlock (u32), +0xC CurrentByteCursor (u32), +0x10 Blocks[8192].
// Names are decoded straight out of the pool with safe reads; no game code is
// ever called, so a game update can never turn name lookup into a wild call
// (the old FNAME_TOSTRING_OFFSET approach crashed after the 2026-06-04 update).
static uintptr_t gNamePool = 0;

// Cached pointers needed by the auto path (found once at startup).
// K2_GetActorLocation is reused every frame to read fabricator positions.
static void* gGetLocFunc = nullptr;

// UClass "World" — native class, stable for the whole process lifetime. Lets
// FindCurrentWorld match by pointer compare instead of resolving every
// object's class name (the 1s-throttled idle scan was taking 8-20s).
static void* gWorldClass = nullptr;

// ============================================================
// Constants (Deadzone Rogue v1.4 - stable across ASLR)
// ============================================================

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
static bool ScanAbort();

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
    // Co-op: one ValPlayerSanctifyComponent exists per player (local + replicated
    // proxies). We must flip the unlock bool on ALL of them — setting only the
    // first-found one (arbitrary GUObjectArray order) randomly left some players,
    // including the host, unable to bless.
    void* playerSanctifyComps[16];
    int   playerSanctifyCompCount;
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
    void* containers[16];        // ALL live WBP_Game_Currencies_C instances
    int   containerCount;
    void* addCurrencyWidgetFunc; // AddCurrencyWidget on WBP_Game_Currencies_C
    uintptr_t processEventAddr;
    uint8_t currencyId[16];
    volatile bool done;
    volatile int  successCount;
} gAddReq = {};

static void GameThreadAddCurrency() {
    ProcessEvent_fn pe = (ProcessEvent_fn)gAddReq.processEventAddr;
    gAddReq.successCount = 0;
    // Add the vial to EVERY live currency container (visor HUD, tab/run menu,
    // forge, upgrade screens). Adding to a single "best guess" container made the
    // vial appear in only one of those views, flip-flopping per checkpoint load.
    for (int i = 0; i < gAddReq.containerCount; i++) {
        void* container = gAddReq.containers[i];
        __declspec(align(16)) uint8_t p[256] = {};
        memcpy(p, gAddReq.currencyId, 16); // FPrimaryAssetId at param offset 0
        __try {
            pe(container, gAddReq.addCurrencyWidgetFunc, p);
            Log("[GT-Add] AddCurrencyWidget OK on container 0x%llX\n", (uintptr_t)container);
            gAddReq.successCount++;
        } __except(EXCEPTION_EXECUTE_HANDLER) {
            Log("[GT-Add] AddCurrencyWidget CRASHED on container 0x%llX\n", (uintptr_t)container);
        }
    }
    gAddReq.done = true;
}

// Find an FName by locating any loaded UObject whose name resolves to `target`.
// Any FName resolving to a given string shares the same ComparisonIndex, so this
// yields the exact FName needed to rebuild an FPrimaryAssetId — no harvest required.
static bool FindFNameByObjectName(const wchar_t* target, FName* out) {
    wchar_t nb[256];
    for (int i = 0; i < gNumElements; i++) {
        if ((i & 0x3FFF) == 0 && ScanAbort()) return false;
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

    // Set the unlock bool on EVERY player sanctify component (co-op: one per
    // player). Setting only the first one left blessing working for a random
    // subset of players and often not the host.
    int pscSet = 0;
    for (int p = 0; p < gSpawnReq.playerSanctifyCompCount; p++) {
        void* comp = gSpawnReq.playerSanctifyComps[p];
        if (comp && IsSafeToRead(comp, PLAYER_SANCTIFY_BOOL_OFFSET + 1)) {
            ((uint8_t*)comp)[PLAYER_SANCTIFY_BOOL_OFFSET] = 1;
            pscSet++;
        }
    }
    if (pscSet) Log("[GT] PlayerSanctifyComp+0x%X set TRUE on %d component(s)\n",
                    PLAYER_SANCTIFY_BOOL_OFFSET, pscSet);

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

// Fast variants: SEH only, no VirtualQuery syscall. Use ONLY on pointers that
// chain from already-validated structures (obj array chunks, UObjects, name
// pool blocks) — all UE heap, never guard pages. The VirtualQuery in SafeRead
// cost ~1µs per read, which multiplied into 10-20s full-object scans.
static bool FastRead64(uintptr_t a, uintptr_t* o) {
    __try { *o = *(uintptr_t*)a; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static bool FastRead32(uintptr_t a, int32_t* o) {
    __try { *o = *(int32_t*)a; return true; } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Decode one FNameEntry from a candidate pool. Entry: u16 header (bIsWide:1,
// LowercaseProbeHash:5, Len:10) followed by Len ANSI bytes or UTF-16 chars.
// Block = index >> 16, byte offset = (index & 0xFFFF) * 2 (stride 2).
static bool PoolReadName(uintptr_t pool, uint32_t idx, wchar_t* buf, int sz) {
    buf[0] = 0;
    uint32_t block = idx >> 16;
    uint32_t off = (idx & 0xFFFF) * 2;
    if (block >= 8192) return false;
    uintptr_t bp = 0;
    if (!FastRead64(pool + 0x10 + (uintptr_t)block * 8, &bp) || !bp) return false;
    int32_t h32 = 0;
    if (!FastRead32(bp + off, &h32)) return false;
    uint16_t h = (uint16_t)h32;
    int wide = h & 1, len = h >> 6;
    if (len <= 0 || len >= sz || len > 1023) return false;
    __try {
        if (wide) { memcpy(buf, (const void*)(bp + off + 2), (size_t)len * 2); buf[len] = 0; }
        else {
            const uint8_t* s = (const uint8_t*)(bp + off + 2);
            for (int i = 0; i < len; i++) buf[i] = (wchar_t)s[i];
            buf[len] = 0;
        }
        return true;
    } __except(EXCEPTION_EXECUTE_HANDLER) { buf[0] = 0; return false; }
}

static bool GetFNameStr(const FName* n, wchar_t* buf, int sz) {
    buf[0] = 0;
    if (!gNamePool || !n) return false;
    if (!PoolReadName(gNamePool, (uint32_t)n->ComparisonIndex, buf, sz)) return false;
    if (n->Number) { // match FNameToString: "_<Number-1>" suffix on numbered names
        size_t l = wcslen(buf);
        if ((int)l < sz - 1) swprintf_s(buf + l, sz - l, L"_%d", n->Number - 1);
    }
    return true;
}

void* GetUObject(int32_t i) {
    if (!gObjArrayBase || i < 0 || i >= gNumElements) return nullptr;
    int32_t ci = i / 65536, wi = i % 65536;
    if (ci >= gNumChunks) return nullptr;
    uintptr_t cb = 0, cp = 0, op = 0;
    if (!FastRead64((uintptr_t)gObjArrayBase, &cb) || !cb) return nullptr;
    if (!FastRead64(cb + ci * 8, &cp) || !cp) return nullptr;
    if (!FastRead64(cp + (uintptr_t)wi * gItemSize, &op)) return nullptr;
    return (void*)op;
}

bool GetObjectName(void* obj, wchar_t* buf, int sz) {
    buf[0] = 0;
    if (!obj) return false;
    FName n;
    if (!FastRead32((uintptr_t)obj+0x18, &n.ComparisonIndex)) return false;
    if (!FastRead32((uintptr_t)obj+0x1C, &n.Number)) return false;
    return GetFNameStr(&n, buf, sz);
}

void* GetObjClass(void* o) {
    if (!o) return nullptr;
    uintptr_t c = 0;
    if (!FastRead64((uintptr_t)o + 0x10, &c)) return nullptr;
    return (void*)c;
}

void* GetObjOuter(void* o) {
    if (!o) return nullptr;
    uintptr_t u = 0;
    if (!FastRead64((uintptr_t)o + 0x20, &u)) return nullptr;
    return (void*)u;
}

bool IsCDO(void* o) {
    wchar_t n[256]; return GetObjectName(o, n, 256) && wcsncmp(n, L"Default__", 9) == 0;
}

// True when a long object scan must bail out: mod stopping, or the game is
// shutting down (objects are being destroyed under us). Checked periodically
// inside every full-array scan so an in-flight scan can't outlive teardown.
static bool ScanAbort() {
    return !gRunning || TickHook::IsShuttingDown();
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

// True if p points at the FNameEntry for "None" (ANSI, len 4) — the entry that
// always sits at offset 0 of FNamePool block 0. SEH guards cross-page reads.
static bool LooksLikeNoneEntry(uintptr_t p) {
    if (!IsSafeToRead((void*)p, 2)) return false;
    __try {
        uint16_t h = *(uint16_t*)p;
        if ((h & 1) || (h >> 6) != 4) return false;
        return memcmp((const void*)(p + 2), "None", 4) == 0;
    } __except(EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// Scan [lo,hi) of committed .data for a slot whose value points at the "None"
// entry — that slot is &Blocks[0], i.e. pool base + 0x10. Returns the SLOT
// address (so the caller can resume past a rejected candidate). SEH-wrapped so
// a region disappearing mid-scan can't take the thread down.
static uintptr_t ScanForNamePoolSlot(uintptr_t lo, uintptr_t hi) {
    __try {
        for (uintptr_t a = (lo + 7) & ~(uintptr_t)7; a + 8 <= hi; a += 8) {
            uintptr_t p = *(uintptr_t*)a;
            if (p < 0x10000 || p > 0x7FFFFFFFFFFFULL || (p & 1)) continue;
            if (LooksLikeNoneEntry(p)) return a;
        }
    } __except(EXCEPTION_EXECUTE_HANDLER) {}
    return 0;
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

    // Find FNamePool (GNames): scan .data for &Blocks[0] — the slot whose value
    // points at the "None" entry. Validate each candidate by resolving real
    // UObject names from the already-found GUObjectArray through it. Read-only;
    // replaces the hardcoded FNAME_TOSTRING_OFFSET call that crashed after the
    // 2026-06-04 game update.
    {
        DWORD t0 = GetTickCount();
        uintptr_t dEnd = gDataStart + gDataSize;
        for (uintptr_t page = gDataStart & ~(uintptr_t)0xFFF; page < dEnd && !gNamePool; page += 0x1000) {
            if (!IsSafeToRead((void*)page, 0x1000)) continue;
            uintptr_t lo = page > gDataStart ? page : gDataStart;
            uintptr_t hi = (page + 0x1000) < dEnd ? (page + 0x1000) : dEnd;
            while (!gNamePool) {
                uintptr_t slot = ScanForNamePoolSlot(lo, hi);
                if (!slot) break;
                lo = slot + 8; // resume past this slot if rejected
                uintptr_t cand = slot - 0x10;
                // Validate: >=5 of the first 200 live objects must resolve to a name
                int ok = 0;
                wchar_t vb[256];
                for (int i = 0; i < 200 && i < gNumElements && ok < 5; i++) {
                    void* o = GetUObject(i); if (!o || !IsSafeToRead(o, 0x20)) continue;
                    int32_t ci = 0; if (!SafeRead32((uintptr_t)o + 0x18, &ci)) continue;
                    if (PoolReadName(cand, (uint32_t)ci, vb, 256) && vb[0]) ok++;
                }
                if (ok >= 5) {
                    gNamePool = cand;
                    Log("FNamePool: 0x%llX (scan %lums, validated %d names)\n",
                        cand, (unsigned long)(GetTickCount() - t0), ok);
                } else {
                    Log("FNamePool candidate 0x%llX rejected (%d/5 names) — continuing scan\n", cand, ok);
                }
            }
        }
        if (!gNamePool) {
            Log("FNamePool NOT FOUND (scan %lums)\n", (unsigned long)(GetTickCount() - t0));
            return false;
        }
    }

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
        if (!gWorldClass && wcscmp(nb, L"World") == 0) {
            wchar_t cn[64];
            void* c = GetObjClass(o);
            if (c && GetObjectName(c, cn, 64) && wcscmp(cn, L"Class") == 0)
                gWorldClass = o; // the native UClass named "World"
        }
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

    Log("Cached: GetLoc=%s PE=%s WorldClass=%s\n",
        gGetLocFunc?"OK":"NO", gProcessEventAddr?"OK":"NO", gWorldClass?"OK":"NO");
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

// True when the current level already provides sanctify natively (zone 4).
// Decided from the world's map name: Zone4 -> native, Zone1/2/3 -> not. Only
// for unknown map names fall back to the GameState bSanctifyingAvailable bool.
// (The old "any SafeRoom EncounterManager instance" heuristic false-positived
// on RogueLite_Zone1_Mission19_P, which contains one, and skipped the forge.)
static bool IsZone4Native(void* world) {
    wchar_t wn[256] = {};
    if (world) GetObjectName(world, wn, 256);
    if (wcsstr(wn, L"Zone4")) {
        Log("[Zone4?] YES via world name %ls\n", wn);
        return true;
    }
    if (wcsstr(wn, L"Zone1") || wcsstr(wn, L"Zone2") || wcsstr(wn, L"Zone3")) {
        Log("[Zone4?] NO via world name %ls — proceeding with anvils+vials\n", wn);
        return false;
    }

    // Unknown map name — check GameState's sanctify bool as a fallback.
    DWORD t0 = GetTickCount();
    wchar_t cb[256];
    for (int i = 0; i < gNumElements; i++) {
        if ((i & 0x3FFF) == 0 && ScanAbort()) return false;
        void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
        if (wcsstr(cb, L"GameState") && wcsstr(cb, L"RogueLite")) {
            int32_t v = 0;
            if (SafeRead32((uintptr_t)o + GAMESTATE_SANCTIFY_BOOL_OFFSET, &v) && (v & 0xFF)) {
                Log("[Zone4?] YES via GameState+0x%X=true class=%ls (world %ls, scan %lums)\n",
                    GAMESTATE_SANCTIFY_BOOL_OFFSET, cb, wn, (unsigned long)(GetTickCount() - t0));
                return true;
            }
        }
    }
    Log("[Zone4?] NO (world %ls, fallback scan %lums) — proceeding with anvils+vials\n",
        wn, (unsigned long)(GetTickCount() - t0));
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
        if ((i & 0x3FFF) == 0 && ScanAbort()) return nullptr;
        void* o = GetUObject(i); if (!o) continue;
        void* c = GetObjClass(o); if (!c) continue;
        // Cheap pointer compare against cached UClass; fall back to name match
        // only if the class cache failed at startup.
        if (gWorldClass) { if (c != gWorldClass) continue; }
        else { if (!GetObjectName(c, cb, 256) || wcscmp(cb, L"World") != 0) continue; }
        if (IsCDO(o)) continue;
        worldCount++;
        if (!GetObjectName(o, nb, 256)) continue;
        // Real gameplay maps are RogueLite_ZoneN_..._P (always carry "Zone" and/or
        // "Mission"). Exclude:
        //   - the between-runs hub/colony ship, which is the bare RogueLite_P (no
        //     Zone/Mission) and the RogueLite_SubLevel_StartingShip_* sublevels
        //     (one per zone, incl. _Apophis for the Zone-4 DLC),
        //   - the home-base Lounge and other RogueLite_SubLevel_* hub/social areas,
        //   - non-RogueLite worlds like DungeonMapZonesAudio (has "Zones" but isn't
        //     gameplay) and MainMenu_P.
        // The game's native forge already serves the hub/ship, so spawning there put
        // an out-of-bounds anvil on the starter ship.
        bool match = wcsstr(nb, L"RogueLite")
            && (wcsstr(nb, L"Zone") || wcsstr(nb, L"Mission"))
            && !wcsstr(nb, L"SubLevel");
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

// FPrimaryAssetId offset inside WBP_Game_Currency_C, derived at runtime from a
// live widget (game updates move it; 0x55C was the pre-2026-06-04 value).
static int gCurrencyIdOffset = 0;

// One pass over live WBP_Game_Currency_C instances. Derives gCurrencyIdOffset,
// reports whether a SanctityToken widget already exists, and collects ALL LIVE
// currencies containers. There are several WBP_Game_Currencies_C instances at
// once (visor HUD, tab/run menu, forge, upgrade screens); the native Scrap/Tech
// currencies show in each, so the vial must be added to each. Adding to only one
// made the vial flip between the HUD and the tab menu per checkpoint load.
struct CurrencyScanResult {
    int   widgetCount;        // live currency widget instances found
    bool  sanctityPresent;    // a widget already shows SanctityToken
    void* liveContainers[16]; // every live currency container
    int   liveCount;
};

// A WBP_Game_Currencies_C is LIVE (actually mounted on a player-facing UI) when
// its outer chain reaches the running game instance/engine. Template/CDO
// containers instead chain up to a /Game/...(Package); AddCurrencyWidget on those
// shows nothing.
static bool IsLiveContainer(void* container) {
    wchar_t nb[128];
    void* ou = container;
    for (int d = 0; d < 10 && ou; d++) {
        ou = GetObjOuter(ou); if (!ou) break;
        if (GetObjectName(ou, nb, 128)
            && (wcsstr(nb, L"ValGameEngine") || wcsstr(nb, L"BP_ValGameInstance")))
            return true;
    }
    return false;
}

// Format an object's outer chain as "Name(Class) <- Name(Class) <- ..." for logs.
static void FormatOuterChain(void* o, wchar_t* out, int outsz, int depth) {
    out[0] = 0;
    wchar_t nb[128], cb[128];
    void* ou = o;
    for (int d = 0; d < depth; d++) {
        ou = GetObjOuter(ou); if (!ou) break;
        nb[0] = cb[0] = 0;
        GetObjectName(ou, nb, 128);
        void* c = GetObjClass(ou); if (c) GetObjectName(c, cb, 128);
        size_t l = wcslen(out);
        swprintf_s(out + l, outsz - l, L"%s%ls(%ls)", d ? L" <- " : L"", nb, cb);
    }
}

static void ScanCurrencyWidgets(CurrencyScanResult* r, bool verbose) {
    r->widgetCount = 0; r->sanctityPresent = false; r->liveCount = 0;
    wchar_t cb[256], ns[256], ts[256], oc[512];

    // Pass 1: collect widget + container instances.
    void* widgets[64]; int nw = 0;
    void* containers[16]; int ncont = 0;
    for (int i = 0; i < gNumElements; i++) {
        if ((i & 0x3FFF) == 0 && ScanAbort()) return;
        void* o = GetUObject(i); if (!o || IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;
        if (wcscmp(cb, L"WBP_Game_Currency_C") == 0) { if (nw < 64) widgets[nw++] = o; }
        else if (wcscmp(cb, L"WBP_Game_Currencies_C") == 0) { if (ncont < 16) containers[ncont++] = o; }
    }
    r->widgetCount = nw;

    // Derive the FPrimaryAssetId offset from the first widget that has a
    // valid {type="ValItemAsset", name="Item_Currency_*"} pair in its body.
    for (int w = 0; w < nw && !gCurrencyIdOffset; w++) {
        void* o = widgets[w];
        for (int off = 0x400; off <= 0x900; off += 4) {
            FName t, n;
            if (!FastRead32((uintptr_t)o + off + 0, &t.ComparisonIndex)) break;
            FastRead32((uintptr_t)o + off + 4, &t.Number);
            FastRead32((uintptr_t)o + off + 8, &n.ComparisonIndex);
            FastRead32((uintptr_t)o + off + 12, &n.Number);
            if (!GetFNameStr(&t, ts, 256) || wcscmp(ts, L"ValItemAsset") != 0) continue;
            if (!GetFNameStr(&n, ns, 256) || wcsncmp(ns, L"Item_Currency_", 14) != 0) continue;
            gCurrencyIdOffset = off;
            Log("[Vials] derived CurrencyId offset: +0x%X (was 0x%X pre-update) via %ls\n",
                off, CURRENCY_ID_OFFSET, ns);
            break;
        }
    }

    // Pass 2: per widget — only need to know whether a SanctityToken vial widget
    // already exists anywhere (so we don't add it twice).
    for (int w = 0; w < nw; w++) {
        void* o = widgets[w];
        ns[0] = 0;
        if (gCurrencyIdOffset) {
            FName n;
            if (FastRead32((uintptr_t)o + gCurrencyIdOffset + 8, &n.ComparisonIndex)
                && FastRead32((uintptr_t)o + gCurrencyIdOffset + 12, &n.Number))
                GetFNameStr(&n, ns, 256);
            if (wcsstr(ns, L"SanctityToken")) r->sanctityPresent = true;
        }
        if (verbose) {
            FormatOuterChain(o, oc, 512, 4);
            Log("[Vials] widget 0x%llX id=%ls outers: %ls\n",
                (uintptr_t)o, ns[0] ? ns : L"?", oc);
        }
    }

    // Collect EVERY live container (outer chain reaches the game instance/engine).
    // The vial gets added to all of them so it shows on the HUD AND the tab menu
    // AND the forge/upgrade screens — wherever Scrap/Tech already appear.
    for (int ci = 0; ci < ncont; ci++) {
        bool live = IsLiveContainer(containers[ci]);
        if (live && r->liveCount < 16) r->liveContainers[r->liveCount++] = containers[ci];
        if (verbose) {
            wchar_t nb[128] = {};
            GetObjectName(containers[ci], nb, 128);
            FormatOuterChain(containers[ci], oc, 512, 4);
            Log("[Vials] container 0x%llX name=%ls%s outers: %ls\n",
                (uintptr_t)containers[ci], nb, live ? L" [LIVE]" : L"", oc);
        }
    }
    if (verbose)
        Log("[Vials] %d container instances, %d live\n", ncont, r->liveCount);
    // Fallback: nothing matched the liveness test — try the first container so we
    // at least attempt an add rather than silently doing nothing.
    if (!r->liveCount && ncont) r->liveContainers[r->liveCount++] = containers[0];
}

// Spawn a sanctified anvil at every fabricator that doesn't already have one.
// Idempotent: safe to call repeatedly within a world (skips fabricators already
// covered), so the caller can keep calling it across a settle window to catch
// fabricators that stream in late on a fast checkpoint reload. Returns:
//   0 = not done — missing critical objects, or a fabricator is still loading
//       its transform; the caller should retry.
//   1 = nothing left to do this call (all fabricators covered / none pending).
//  >1 = the count of new anvils placed this call.
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
    void* gmInst = nullptr, *srmInst = nullptr, *gsInst = nullptr;
    void* pscInstances[16] = {}; int pscCount = 0; // all player sanctify comps (co-op = one per player)
    void* fabInstances[32] = {}; int fabCount = 0;
    void* anvilInstances[64] = {}; int anvilInstCount = 0; // existing anvils, for idempotent re-spawn
    wchar_t nb[256], cb[256], ob[256];

    for (int i = 0; i < gNumElements; i++) {
        if ((i & 0x3FFF) == 0 && ScanAbort()) { Log("[Spawn] scan aborted (shutdown)\n"); return 0; }
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

        if (!world && wcscmp(cb, L"World") == 0 && wcsstr(nb, L"RogueLite")
            && (wcsstr(nb, L"Zone") || wcsstr(nb, L"Mission")) && !wcsstr(nb, L"SubLevel"))
            world = o;

        if (wcscmp(cb, L"BP_Fabricator_C") == 0 && fabCount < 32)
            fabInstances[fabCount++] = o;

        // Already-spawned anvils — used to skip fabricators we've already covered
        // so repeated spawn passes within a world never stack duplicate anvils.
        if (wcscmp(cb, L"BP_SanctifiedAnvil_C") == 0 && anvilInstCount < 64)
            anvilInstances[anvilInstCount++] = o;

        if (!gmInst && wcsstr(cb, L"GameMode") && (wcsstr(cb, L"Dungeon") || wcsstr(cb, L"RogueLite")))
            gmInst = o;
        if (!srmInst && wcsstr(cb, L"EncounterManager") && wcsstr(cb, L"SafeRoom"))
            srmInst = o;
        if (!gsInst && wcsstr(cb, L"GameState") && wcsstr(cb, L"RogueLite"))
            gsInst = o;
        if (wcsstr(cb, L"ValPlayerSanctifyComponent") && !wcsstr(nb, L"GEN_VARIABLE")
            && pscCount < 16)
            pscInstances[pscCount++] = o;
    }

    uintptr_t freshPE = 0;
    if (freshCDO) {
        uintptr_t vt = 0; SafeRead64((uintptr_t)freshCDO, &vt);
        if (vt) { uintptr_t fa = 0; SafeRead64(vt + PROCESS_EVENT_VTABLE_SLOT * 8, &fa);
            if (fa >= gTextStart && fa < gTextStart + gTextSize) freshPE = fa; }
    }

    Log("[Spawn] scan done in %lums: Anvil=%s World=%s CDO=%s BS=%s FS=%s SL=%s CS=%s BP=%s PE=%s "
        "SetSA=%s EnableSA=%s GM=%s SafeRoom=%s GS=%s PSC=%d Fabs=%d\n",
        (unsigned long)(GetTickCount() - spawnT0),
        anvilClass?"OK":"NO", world?"OK":"NO", freshCDO?"OK":"NO", freshBS?"OK":"NO", freshFS?"OK":"NO",
        freshSL?"OK":"NO", freshCS?"OK":"NO", freshBP?"OK":"NO", freshPE?"OK":"NO",
        ssaFunc?"OK":"NO", esaFunc?"OK":"NO", gmInst?"OK":"NO", srmInst?"OK":"NO",
        gsInst?"OK":"NO", pscCount, fabCount);

    if (!anvilClass || !world || !freshBS || !freshFS || !freshCDO || !freshPE) {
        Log("[Spawn] missing critical objects — NOT READY, will retry\n");
        return 0;
    }

    // Resolve locations of anvils that already exist, so we can skip fabricators
    // we've already placed one at (idempotent re-spawn).
    double anvX[64], anvY[64], anvZ[64]; int anvLocCount = 0;
    for (int ai = 0; ai < anvilInstCount && anvLocCount < 64; ai++) {
        double ax, ay, az;
        if (GetActorLoc(anvilInstances[ai], &ax, &ay, &az)) {
            anvX[anvLocCount] = ax; anvY[anvLocCount] = ay; anvZ[anvLocCount] = az; anvLocCount++;
        }
    }

    // Build spawn list from fabricators that (a) report a real transform and
    // (b) don't already have an anvil at their target spot. A fabricator that
    // streamed in but hasn't initialized its transform reads (0,0,0); GetActorLoc
    // rejects that, so we count it as "not ready" and report retry — this is the
    // safe-room fabricator that loads late on a fast checkpoint reload.
    int sc = 0, covered = 0, notReady = 0;
    for (int fi = 0; fi < fabCount && sc < 32; fi++) {
        double fx, fy, fz;
        if (!GetActorLoc(fabInstances[fi], &fx, &fy, &fz)) { notReady++; continue; }
        double tx = fx - 576.8, ty = fy + 35.5, tz = fz - 123.1;
        bool already = false;
        for (int k = 0; k < anvLocCount; k++) {
            double dx = anvX[k] - tx, dy = anvY[k] - ty, dz = anvZ[k] - tz;
            if (dx*dx + dy*dy + dz*dz < 150.0*150.0) { already = true; break; }
        }
        if (already) { covered++; continue; }
        gSpawnReq.spawnX[sc] = tx; gSpawnReq.spawnY[sc] = ty; gSpawnReq.spawnZ[sc] = tz; sc++;
    }
    if (sc == 0) {
        // Nothing new to place. If a fabricator is still loading, the caller should
        // keep retrying within its window; otherwise everything's covered.
        Log("[Spawn] no new anvils: %d covered, %d not-ready, %d total fabs — %s\n",
            covered, notReady, fabCount, notReady ? "will retry" : "all covered");
        return notReady ? 0 : 1;
    }
    gSpawnReq.spawnCount = sc;
    Log("[Spawn] placing %d new anvil(s) (%d already covered, %d not-ready, %d total fabs)\n",
        sc, covered, notReady, fabCount);

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
    for (int p = 0; p < pscCount; p++) gSpawnReq.playerSanctifyComps[p] = pscInstances[p];
    gSpawnReq.playerSanctifyCompCount = pscCount;

    Log("Spawning %d anvils...\n", sc);
    gSpawnReq.resultReady = false;
    gSpawnReq.success = false;

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
    return 1 + sc; // >1 = placed sc new anvils this call
}

// Add the SanctityToken vial widget to the live HUD. Returns 0 = not ready (retry),
// 1 = added, 2 = already present (skip).
static int DoAddVials() {
    DWORD vialT0 = GetTickCount();
    Log("=== ADD VIALS ===\n");
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);

    DWORD t0 = GetTickCount();
    CurrencyScanResult cs;
    ScanCurrencyWidgets(&cs, true);
    Log("[Vials] widget scan %lums: %d currency widgets, sanctity=%s, liveContainers=%d\n",
        (unsigned long)(GetTickCount() - t0), cs.widgetCount,
        cs.sanctityPresent ? "PRESENT" : "absent", cs.liveCount);
    if (cs.sanctityPresent) return 2;

    // No live currency widgets yet = the HUD hasn't been built. Adding now
    // would target a template/preload container and silently show nothing.
    if (!cs.widgetCount || !cs.liveCount) {
        Log("[Vials] HUD not live yet (no existing currency widgets) — NOT READY, will retry\n");
        return 0;
    }

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
    wchar_t nb[256], ob[256], cb2[64];
    static bool funcsDumped = false; // one-time class API dump for diagnostics
    for (int i = 0; i < gNumElements; i++) {
        if ((i & 0x3FFF) == 0 && ScanAbort()) { Log("[Vials] scan aborted (shutdown)\n"); return 0; }
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;
        if (!addCurrFunc && wcscmp(nb, L"AddCurrencyWidget") == 0) {
            void* ou = GetObjOuter(o);
            if (ou && GetObjectName(ou, ob, 256) && wcsstr(ob, L"WBP_Game_Currencies_C")) {
                addCurrFunc = o;
                if (funcsDumped) break;
            }
        }
        if (!funcsDumped) {
            void* c = GetObjClass(o);
            if (c && GetObjectName(c, cb2, 64) && wcscmp(cb2, L"Function") == 0) {
                void* ou = GetObjOuter(o);
                if (ou && GetObjectName(ou, ob, 256)
                    && (wcscmp(ob, L"WBP_Game_Currencies_C") == 0 || wcscmp(ob, L"WBP_Game_Currency_C") == 0)) {
                    int32_t ps = 0; SafeRead32((uintptr_t)o + 0x58, &ps);
                    Log("[Vials] fn %ls::%ls PS=%d\n", ob, nb, ps);
                }
            }
        }
    }
    funcsDumped = true;
    int32_t addPS = 0;
    if (addCurrFunc) SafeRead32((uintptr_t)addCurrFunc + 0x58, &addPS);
    Log("[Vials] HUD scan %lums: addFn=%s (PropertiesSize=%d, was 73 pre-update)\n",
        (unsigned long)(GetTickCount() - t0), addCurrFunc?"OK":"NO", addPS);

    if (!addCurrFunc) {
        Log("[Vials] AddCurrencyWidget not found — NOT READY, will retry\n");
        return 0;
    }

    for (int i = 0; i < cs.liveCount; i++) gAddReq.containers[i] = cs.liveContainers[i];
    gAddReq.containerCount = cs.liveCount;
    gAddReq.addCurrencyWidgetFunc = addCurrFunc;
    gAddReq.processEventAddr = gProcessEventAddr;
    memcpy(gAddReq.currencyId, currencyId, 16);
    gAddReq.done = false; gAddReq.successCount = 0;

    if (!TickHook::Install(0)) { Log("[Vials] tick hook install failed\n"); return 0; }
    DWORD gtT0 = GetTickCount();
    TickHook::QueueOnGameThread(GameThreadAddCurrency);
    int waited = 0;
    for (int w = 0; w < 100 && !gAddReq.done; w++) { Sleep(50); waited++; }
    Log("[Vials] game-thread wait: %dms (done=%s added=%d/%d)\n",
        waited * 50, gAddReq.done?"Y":"N", gAddReq.successCount, gAddReq.containerCount);
    if (gAddReq.successCount == 0) {
        Log("[Vials] ADD FAILED (total %lums)\n", (unsigned long)(GetTickCount() - vialT0));
        return 0;
    }

    // The call not crashing is NOT proof a widget was created (the BP can
    // early-out and return null). Verify a SanctityToken widget now exists;
    // if not, report not-ready so the auto loop retries.
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    ScanCurrencyWidgets(&cs, false);
    if (cs.sanctityPresent) {
        Log("[Vials] *** ADDED & VERIFIED *** (total %lums)\n",
            (unsigned long)(GetTickCount() - vialT0));
        return 1;
    }
    Log("[Vials] call returned OK but NO SanctityToken widget exists — will retry "
        "(total %lums)\n", (unsigned long)(GetTickCount() - vialT0));
    return 0;
}

// ============================================================
// DIAGNOSTIC: synergy-system state dump (READ-ONLY)
// ------------------------------------------------------------
// Synergies are a player-UPGRADE mechanic (no actor to spawn). The game gates
// them by zone: PlayerUpgradeConfig maps Mission.Zone.N -> PlayerUpgrades_ZoneN,
// and only PlayerUpgrades_Zone3 lists any Synergy_* assets. So the live
// ValPlayerUpgradeComponent in zones 1-2 has an empty synergy set.
//
// This dump runs once per world in EVERY zone. Capture one Zone 3 log (synergies
// work) and one Zone 1 log (absent); the diff reveals the injection target:
//   - whether Synergy_* / PlayerUpgrades_Zone3 assets are even resident in memory
//     (if not, injection must force-load them first)
//   - the live ValPlayerUpgradeComponent and which of its fields point at synergy
//     / config assets (UpgradeConfig, SynergyAssetMap entries)
//   - UFunctions on the upgrade component / player state / loot widget — candidate
//     entry points for adding a synergy and refreshing the UI
// Nothing here writes game memory.
// ============================================================

static void HexDumpToLog(const char* tag, void* base, int len) {
    if (!IsSafeToRead(base, len)) {
        Log("[Dump] %s @0x%llX UNREADABLE\n", tag, (uintptr_t)base);
        return;
    }
    char line[200];
    for (int o = 0; o < len; o += 16) {
        int n = sprintf_s(line, sizeof(line), "[Dump] %s +0x%03X:", tag, o);
        for (int b = 0; b < 16 && o + b < len; b++)
            n += sprintf_s(line + n, sizeof(line) - n, " %02X", ((uint8_t*)base)[o + b]);
        Log("%s\n", line);
    }
}

// FField/FProperty layout for THIS build (decoded from raw node bytes): the
// Owner field is 8 bytes (not 16), so everything is shifted -8 from UE5 stock.
//   UStruct::SuperStruct      @ 0x40
//   UStruct::ChildProperties  @ 0x50  (FField* linked list)
//   FField::Next              @ 0x18
//   FField::NamePrivate       @ 0x20
//   FProperty::Offset_Internal@ 0x44
// These are the defaults; CalibrateLayout() re-derives all four at runtime and
// only overrides them if the defaults stop resolving (survives a game update).
static const int USTRUCT_SUPER = 0x40;
static int gChildProps = 0x50;
static int gFNext      = 0x18;
static int gFName      = 0x20;
static int gFOffset    = 0x44;
static bool gLayoutChecked = false;

// Find the FField* for propName in classObj's chain (+ supers), or 0.
static uintptr_t FindField(void* classObj, int childOff, int nameOff, int nextOff, const wchar_t* propName) {
    wchar_t nb[256];
    uintptr_t st = (uintptr_t)classObj;
    for (int depth = 0; st && depth < 24; depth++) {
        uintptr_t field = 0;
        if (!FastRead64(st + childOff, &field)) break;
        for (int guard = 0; field && guard < 8192; guard++) {
            FName n{};
            if (FastRead32(field + nameOff, &n.ComparisonIndex)
                && FastRead32(field + nameOff + 4, &n.Number)
                && GetFNameStr(&n, nb, 256) && wcscmp(nb, propName) == 0) return field;
            if (!FastRead64(field + nextOff, &field)) break;
        }
        if (!FastRead64(st + USTRUCT_SUPER, &st)) break;
    }
    return 0;
}

// Resolve a property's byte offset within an instance by name. -1 if not found.
static int GetPropOffset(void* classObj, const wchar_t* propName) {
    uintptr_t f = FindField(classObj, gChildProps, gFName, gFNext, propName);
    if (!f) return -1;
    int32_t off = -1; FastRead32(f + gFOffset, &off);
    return (off >= 0 && off < 0x10000) ? off : -1;
}

// Validate the layout against two known props (different sane offsets); if the
// defaults fail (e.g. after a game update) brute-force all four offsets. Runs once.
static bool CalibrateLayout(void* sentinelClass, const wchar_t* s1, const wchar_t* s2) {
    if (gLayoutChecked) return gChildProps >= 0;
    gLayoutChecked = true;

    auto resolves = [&](int child, int name, int next, int offI, int& o1, int& o2) -> bool {
        uintptr_t f1 = FindField(sentinelClass, child, name, next, s1);
        uintptr_t f2 = FindField(sentinelClass, child, name, next, s2);
        if (!f1 || !f2) return false;
        int32_t a = -1, b = -1;
        FastRead32(f1 + offI, &a); FastRead32(f2 + offI, &b);
        o1 = a; o2 = b;
        return a >= 0 && a < 0x4000 && b >= 0 && b < 0x4000 && a != b;
    };

    int o1, o2;
    if (resolves(gChildProps, gFName, gFNext, gFOffset, o1, o2)) {
        Log("[Dump] reflection layout OK (defaults): %ls@0x%X %ls@0x%X "
            "[child0x%X name0x%X next0x%X off0x%X]\n", s1, o1, s2, o2,
            gChildProps, gFName, gFNext, gFOffset);
        return true;
    }

    const int childC[] = { 0x50, 0x48, 0x58, 0x40, 0x60, 0x68, 0x70 };
    const int nameC[]  = { 0x20, 0x28, 0x18, 0x30, 0x10 };
    const int nextC[]  = { 0x18, 0x20, 0x28, 0x10 };
    const int offC[]   = { 0x44, 0x4C, 0x48, 0x54, 0x40 };
    for (int a = 0; a < (int)(sizeof(childC)/sizeof(int)); a++)
     for (int b = 0; b < (int)(sizeof(nameC)/sizeof(int)); b++)
      for (int c = 0; c < (int)(sizeof(nextC)/sizeof(int)); c++) {
        if (nameC[b] == nextC[c]) continue;
        for (int d = 0; d < (int)(sizeof(offC)/sizeof(int)); d++)
          if (resolves(childC[a], nameC[b], nextC[c], offC[d], o1, o2)) {
            gChildProps = childC[a]; gFName = nameC[b]; gFNext = nextC[c]; gFOffset = offC[d];
            Log("[Dump] reflection RE-CALIBRATED: child0x%X name0x%X next0x%X off0x%X "
                "(%ls@0x%X %ls@0x%X)\n", gChildProps, gFName, gFNext, gFOffset, s1, o1, s2, o2);
            return true;
          }
      }
    gChildProps = -1;
    Log("[Dump] reflection calibration FAILED for both sentinels\n");
    return false;
}

// Read a TArray/TMap (FScriptArray/FScriptMap both start with {void* Data; int32
// Num; int32 Max}) header at instance+off. Returns Num (element count), -1 if bad.
static int ReadContainerNum(void* inst, int off) {
    if (off < 0) return -1;
    int32_t num = 0;
    if (!FastRead32((uintptr_t)inst + off + 8, &num)) return -1;
    return num;
}

static int DoDumpSynergies() {
    Log("===== SYNERGY DIAGNOSTIC DUMP =====\n");
    SafeRead32((uintptr_t)gObjArrayBase + 0x14, &gNumElements);
    SafeRead32((uintptr_t)gObjArrayBase + 0x1C, &gNumChunks);

    void* upgComps[16]; int nUpg = 0;
    void* playerStates[16]; int nPS = 0;
    void* lootSel[8]; int nLoot = 0;
    int nSynListView = 0, nActiveSynList = 0; // synergy-UI widget instances
    void* synView = nullptr; // first WBP_SynergyListView_C instance
    int synTotal = 0;
    wchar_t zoneWorlds[8][96]; int nZoneWorlds = 0; // World instances naming a zone

    wchar_t nb[256], cb[256];

    // Pass 1: count assets, collect the instances we read values from, and note
    // which zone-named World objects are loaded (so the log self-labels the zone).
    for (int i = 0; i < gNumElements; i++) {
        if ((i & 0x3FFF) == 0 && ScanAbort()) { Log("[Dump] aborted (shutdown)\n"); return 0; }
        void* o = GetUObject(i); if (!o) continue;
        if (!GetObjectName(o, nb, 256)) continue;
        if (wcsncmp(nb, L"Synergy_", 8) == 0) synTotal++;

        if (IsCDO(o)) continue;
        void* c = GetObjClass(o); if (!c || !GetObjectName(c, cb, 256)) continue;

        if (wcscmp(cb, L"World") == 0 && wcsstr(nb, L"Zone") && nZoneWorlds < 8)
            wcscpy_s(zoneWorlds[nZoneWorlds++], 96, nb);

        if (wcscmp(cb, L"ValPlayerUpgradeComponent") == 0 && !wcsstr(nb, L"GEN_VARIABLE")
            && nUpg < 16) upgComps[nUpg++] = o;
        // PlayerState instance is a BP/native subclass — match by suffix; the
        // property walk climbs SuperStruct so r_SynergyIds still resolves.
        else if (wcsstr(cb, L"PlayerState") && !wcsstr(cb, L"Base") && nPS < 16) playerStates[nPS++] = o;
        else if (wcscmp(cb, L"WBP_LootSelection_C") == 0 && nLoot < 8) lootSel[nLoot++] = o;
        else if (wcscmp(cb, L"WBP_SynergyListView_C") == 0) { nSynListView++; if (!synView) synView = o; }
        else if (wcscmp(cb, L"WBP_ActiveSynergyList_C") == 0) nActiveSynList++;
    }

    Log("[Dump] zone-worlds loaded: %d%s\n", nZoneWorlds, nZoneWorlds ? "" : " (none — likely a hub/ship)");
    for (int i = 0; i < nZoneWorlds; i++) Log("[Dump]   world: %ls\n", zoneWorlds[i]);
    Log("[Dump] assets: Synergy_*=%d resident\n", synTotal);
    Log("[Dump] instances: UpgradeComponent=%d PlayerState=%d LootSelection=%d "
        "SynergyListView=%d ActiveSynergyList=%d\n",
        nUpg, nPS, nLoot, nSynListView, nActiveSynList);

    // Calibrate the reflection layout against the upgrade component's known
    // bComponentReady bool before reading anything by name.
    if (nUpg > 0) {
        void* cls = GetObjClass(upgComps[0]);
        wchar_t clsName[128] = {}; if (cls) GetObjectName(cls, clsName, 128);
        Log("[Dump] UpgComp class=%ls @0x%llX\n", clsName[0] ? clsName : L"?", (uintptr_t)cls);
        // Two sentinels that must resolve to different offsets (bool vs int).
        if (!CalibrateLayout(cls, L"bComponentReady", L"DynamicBundleCount"))
            HexDumpToLog("UpgClass", cls, 0x90);
    }

    // --- DECISIVE: live synergy state read by NAME via reflection offsets ---
    // SynergyAssetMap.Num == 0 in zones 1-2 and > 0 in zones 3-4 would prove the
    // run-pool gate. bComponentReady validates the reflection walk (must be 0/1).
    for (int u = 0; u < nUpg; u++) {
        void* comp = upgComps[u];
        void* cls = GetObjClass(comp);
        int offReady = GetPropOffset(cls, L"bComponentReady");
        int offMap   = GetPropOffset(cls, L"SynergyAssetMap");
        int offGrpsByType = GetPropOffset(cls, L"UpgradeGroupsByType");
        int offCfg   = GetPropOffset(cls, L"UpgradeConfig");
        int32_t ready = -1; if (offReady >= 0) FastRead32((uintptr_t)comp + offReady, &ready);
        Log("[Dump] UpgComp #%d @0x%llX offsets: bComponentReady@0x%X=%d  "
            "SynergyAssetMap@0x%X  UpgradeGroupsByType@0x%X  UpgradeConfig@0x%X\n",
            u, (uintptr_t)comp, offReady, ready & 0xFF, offMap, offGrpsByType, offCfg);
        Log("[Dump]   *** SynergyAssetMap.Num=%d   UpgradeGroupsByType.Num=%d ***\n",
            ReadContainerNum(comp, offMap), ReadContainerNum(comp, offGrpsByType));
        if (offCfg >= 0) {
            uintptr_t p = 0; FastRead64((uintptr_t)comp + offCfg, &p);
            wchar_t tn[128] = {}; if (p) GetObjectName((void*)p, tn, 128);
            Log("[Dump]   UpgradeConfig -> 0x%llX %ls\n", (uintptr_t)p, tn[0] ? tn : L"(null)");
        }
        HexDumpToLog("UpgComp", comp, 0x240); // window around the map for offline check
    }

    // PlayerState: r_SynergyIds / CachedSynergyIds counts (non-empty once active).
    for (int s = 0; s < nPS && s < 2; s++) {
        void* ps = playerStates[s];
        void* cls = GetObjClass(ps);
        GetObjectName(cls, cb, 256);
        int offR = GetPropOffset(cls, L"r_SynergyIds");
        int offC = GetPropOffset(cls, L"CachedSynergyIds");
        Log("[Dump] PlayerState #%d class=%ls r_SynergyIds@0x%X Num=%d  CachedSynergyIds@0x%X Num=%d\n",
            s, cb, offR, ReadContainerNum(ps, offR), offC, ReadContainerNum(ps, offC));
    }

    // LootSelection (present only while the loot/upgrade screen is open):
    // bUsesSynergies bool — the UI gate.
    for (int l = 0; l < nLoot; l++) {
        void* w = lootSel[l];
        int off = GetPropOffset(GetObjClass(w), L"bUsesSynergies");
        int32_t v = -1; if (off >= 0) FastRead32((uintptr_t)w + off, &v);
        Log("[Dump] LootSelection #%d @0x%llX bUsesSynergies@0x%X = %d\n",
            l, (uintptr_t)w, off, v & 0xFF);
    }

    // SynergyListView: ActiveSynergyIds is the array the UI shows (ArrayProperty);
    // these widgets exist in all zones — the question is whether they're fed.
    if (synView) {
        int off = GetPropOffset(GetObjClass(synView), L"ActiveSynergyIds");
        Log("[Dump] SynergyListView @0x%llX ActiveSynergyIds@0x%X Num=%d\n",
            (uintptr_t)synView, off, ReadContainerNum(synView, off));
    }

    Log("===== END SYNERGY DUMP =====\n");
    return 1;
}

// ============================================================
// Main thread
// ============================================================

static DWORD WINAPI ModThreadProc(LPVOID) {
    Sleep(5000);
#if SANCTIFY_LOGGING
    char lp[MAX_PATH];
    BuildLogPath("SanctifyMod.log", lp, MAX_PATH);
    gLogFile = fopen(lp, "w");
    if (!gLogFile) return 0;
#endif

    Log("=== SanctifyEverywhere v1.2 ===\n");
    Log("Background thread ID: %lu\n", GetCurrentThreadId());

    HWND gw = nullptr;
    for (int i = 0; i < 120 && gRunning; i++) { Sleep(1000); gw = FindWindowA("UnrealWindow", nullptr); if (gw) break; }
    if (!gw) { Log("No window\n"); return 0; }

    Log("Window found, waiting for level load...\n");
    Sleep(10000);

    if (!InitUE()) { Log("InitUE FAILED\n"); goto idle; }
    if (!CacheObjects()) { Log("CacheObjects FAILED\n"); goto idle; }

    // Install the window subclass + timer NOW (not lazily at first spawn) so the
    // heartbeat below works from the start. Spawn/vial paths reuse it (Install
    // is idempotent).
    if (!TickHook::Install(0))
        Log("Heartbeat hook install failed — shutdown detection degraded\n");

    Log("\n*** READY! Auto-spawning anvils + vials on each zone 1-3 load ***\n\n");

idle:
    while (gRunning) {
        Sleep(100);

        // ---- SHUTDOWN DETECTION ----
        // The 2-3 minute exit hang was this thread scanning (and worse, calling
        // ProcessEvent on stale worlds) while the engine destroyed every UObject.
        // Three independent signals end/pause all activity:
        //  1. WM_DESTROY/WM_ENDSESSION on the game window -> exit thread.
        //  2. Game window handle no longer valid -> exit thread (after a grace
        //     period in case the window is being recreated).
        //  3. Heartbeat stale: the game thread hasn't pumped our WM_TIMER for
        //     >2s (teardown, or a heavy hitch) -> skip scans until it resumes.
        if (TickHook::IsShuttingDown()) {
            Log("[Exit] window destroy seen — mod thread exiting\n");
            return 0;
        }
        if (gw && !IsWindow(gw)) {
            static DWORD windowGoneSince = 0;
            HWND ngw = FindWindowA("UnrealWindow", nullptr);
            if (ngw) { // window was recreated — rebind hook to the new one
                gw = ngw;
                windowGoneSince = 0;
                TickHook::Uninstall();
                TickHook::Install(0);
                Log("[Exit] game window recreated — rebound heartbeat hook\n");
            } else {
                if (!windowGoneSince) windowGoneSince = GetTickCount();
                if (GetTickCount() - windowGoneSince >= 3000) {
                    Log("[Exit] game window gone — mod thread exiting\n");
                    return 0;
                }
                continue;
            }
        }
        if (TickHook::HeartbeatAgeMs() > 2000) continue;

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
            static bool  autoDumpDone = false; // synergy diagnostic, once per world

            DWORD now = GetTickCount();
            if ((int)(now - autoNextScan) >= 0) {

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
                    autoDumpDone = false;
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

                // SYNERGY DIAGNOSTIC: dump once per world in EVERY zone (incl. zone
                // 3/4 reference). Read-only; compiled out unless logging is enabled.
#if SANCTIFY_LOGGING
                if (autoWorld && !autoDumpDone && (int)(now - autoFirstSeen) >= 3000) {
                    autoDumpDone = true;
                    DoDumpSynergies();
                }
#endif

                if (autoWorld && !autoSkipZone4 && !(autoAnvilsDone && autoVialsDone)
                    && (int)(now - autoFirstSeen) >= 3000) {

                    if (IsZone4Native(autoWorld)) {
                        autoSkipZone4 = true;
                        Log("[Auto] Zone 4 (native sanctify) — skipping auto spawn/vials\n");
                    } else {
                        autoAttempts++;
                        Log("[Auto] --- attempt %d/20 (anvils=%d vials=%d) ---\n",
                            autoAttempts, autoAnvilsDone?1:0, autoVialsDone?1:0);
                        if (!autoAnvilsDone) {
                            int r = DoSpawnAnvils(); // idempotent; places at any newly-ready fabricator
                            if (r > 1) Log("[Auto] placed %d new anvil(s)\n", r - 1);
                            // Hold the spawn window open ~18s from first sight of the
                            // world so a safe-room fabricator that streams in late
                            // (reads (0,0,0) right after a fast checkpoint reload, or
                            // whose actor appears a beat later) still gets an anvil.
                            // DoSpawnAnvils skips fabricators already covered, so the
                            // extra passes never stack duplicates and are scan-only
                            // once everything's placed. Close the window after 18s.
                            if ((int)(now - autoFirstSeen) >= 18000) {
                                autoAnvilsDone = true;
                                Log("[Auto] anvil spawn window closed — idling\n");
                            }
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

                // Throttle from scan END so a slow scan can't restart back-to-back
                // and peg a core indefinitely.
                autoNextScan = GetTickCount() + 1000;
            }
        }
    }
    return 0;
}

void Mod::Start() {
    // Idempotent: Proxy::Init() starts the mod at DLL attach AND the first
    // proxied dwmapi call triggers EnsureModStarted() — without this guard two
    // scanner threads were created (the first one's handle leaked).
    if (gRunning.exchange(true)) return;
    gModThread = CreateThread(nullptr, 0, ModThreadProc, nullptr, 0, nullptr);
}

void Mod::Stop(bool processTerminating) {
    gRunning = false;
    if (processTerminating) {
        // ExitProcess path: every other thread is already terminated, and we
        // hold the loader lock. Waiting on the mod thread here always burned
        // the full timeout (it can't deliver thread-detach while we hold the
        // lock) — adding 5s to every game exit. Just release resources.
        if (gModThread) { CloseHandle(gModThread); gModThread = nullptr; }
        if (gLogFile) { fclose(gLogFile); gLogFile = nullptr; }
        return;
    }
    // FreeLibrary path (doesn't happen for a statically-imported proxy, but be
    // correct): restore the wndproc, give the thread a moment to notice gRunning.
    TickHook::Uninstall();
    if (gModThread) { WaitForSingleObject(gModThread, 1000); CloseHandle(gModThread); gModThread = nullptr; }
    if (gLogFile) { fclose(gLogFile); gLogFile = nullptr; }
}
