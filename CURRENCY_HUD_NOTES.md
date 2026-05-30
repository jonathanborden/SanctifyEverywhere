# Nanite-Vial HUD Investigation — Working Notes

Handoff doc for the "display nanite-vial count in the HUD in all zones" effort.
All code lives in `src/mod.cpp` (~1430 lines). Game: Deadzone Rogue (UE 5.5.4, internal "Valhalla").

## Goal
Show the player's **nanite-vial** count in the HUD in every zone, the way the game
does natively in zone 4. (Originally only zone 4 shows it.)

## CRITICAL CONTEXT  *** SOLVED via unpacked content (see below) ***
- **NANITE VIAL = `Item_Currency_SanctityToken`** (base-game currency item). PROVEN: string
  table `ST_Currency.uasset` maps key `Currency_SanctityToken` -> display name "Nanite Vial",
  desc "Used at Nanite Forge to grant weapons a Blessing and to reroll Blessed Affixes". It is
  the currency the BLESSING ANVIL consumes — ties directly to the original anvil-spawn feature.
- **EARLIER HYPOTHESES WERE WRONG** (do not re-chase): vials are NOT an Apophis-only system and
  NOT a gameplay-attribute float. The count is an **int item-count** stored in the player's
  **Local Inventory Component**, keyed by the currency's PrimaryAssetId. (That is why the CE
  4-byte INT scan worked, the FLOAT scan found nothing, and the displayed value at
  WBP_Game_Currency_C +0x570 is an int.) `GetMatchCurrency`/`ECurrencyType` were simply the
  wrong subsystem.
- Base currencies Scrap (`Item_Currency_Scrap`) and Tech (`Item_Currency_Tech`) work the same
  way (inventory item-count, same `WBP_Game_Currency_C` widgets).

## WHY VIALS ONLY SHOW IN ZONE 4  (from unpacked blueprints)
- Unpacked game content lives at `C:\Users\jonathan\Desktop\Projects\Deadzone\unpacked\`.
  Key blueprints: `Valhalla/Content/UI/Components/Game/WBP_Game_Currencies.uasset` (container,
  runtime class `WBP_Game_Currencies_C`) and `WBP_Game_Currency.uasset` (per-currency widget).
- Container name-table logic: `DefaultCurrencies` = {Scrap, Tech, SanctityToken}; it adds/removes
  per-currency widgets via `OnAvailableRunProgressionMechanicsUpdated` ->
  `GetAvailableRunProgressionMechanics` (from `Val_Game_State`) and a `bSanctifyingAvailable`
  flag; `AddCurrencyWidget(CurrencyId)` / `RemoveCurrencyWidget`; map `CurrencyWidgets` keyed by
  CurrencyId. => The vial (SanctityToken) widget is shown only when sanctifying / the relevant
  progression mechanic is AVAILABLE = zone 4 (where the Nanite Forge exists).
- Per-currency widget (`WBP_Game_Currency`) name-table logic: `GetLocalInventoryComponent` +
  `BindAssetCount`/`RequestItemAsset`/`ItemAsset`/`CurrencyId`/`SetCurrencyId`; `OnCurrencyChanged`
  -> `Conv_IntToText` -> `CountText`. i.e. it binds to the inventory count for its CurrencyId.

## THE SETTER / GATING MECHANISM (from BP_Onyx_EncounterManager_Child_SafeRoom)
- Only 3 assets reference progression mechanics: `WBP_Game_Currencies` + `UI_Game_TabMenu`
  (consumers) and **`BP_Onyx_EncounterManager_Child_SafeRoom`** (the SETTER). The SafeRoom is the
  zone-4 room that holds the Nanite Forge/anvil.
- Chain: SafeRoom `CheckCanEnableSanctifiedAnvil` -> `EnableSanctifiedAnvil` -> sets
  **`bSanctifyingAvailable`** (bool on `Val_Game_State_Space_Dungeon`) -> broadcasts
  `OnAvailableRunProgressionMechanicsUpdated` -> HUD container handler re-runs
  `GetAvailableRunProgressionMechanics` (returns a Map) and `AddCurrencyWidget` for each entry.
- KEY: the SafeRoom encounter only exists in zone 4, BUT `bSanctifyingAvailable` lives on the
  GAME STATE (`Val_Game_State_Space_Dungeon`), which exists in ALL zones => zone-independent lever.
- Confirms the F9 observation: spawning the anvil actor flips LOOT DROPS (anvil registers w/ loot
  subsystem) but NOT the HUD, because the HUD needs the game-state flag / mechanic-map, set by the
  SafeRoom encounter — not by the anvil actor. Two separate gates.
- Other relevant fns/vars seen: `EnableSanctifiedAnvil`, `TestForceAnvilSpawn`, `RegisterFeature`,
  `CachedSanctifiedAnvilEnabled`; casts to `Val_Sanctified_Anvil`, `SD_Vendor_Fabricator`.

## PATH TO GOAL (show vials in zones 1-3)
- **Native (preferred):** on the live `WBP_Game_Currencies_C` HUD instance in zones 1-3, call
  `AddCurrencyWidget(SanctityToken CurrencyId)` via ProcessEvent (game thread). Widget self-binds
  to inventory and displays, identical to zone 4. NEED: exact SanctityToken PrimaryAssetId
  (PrimaryAssetType + name; "ValItemAsset" string seen) and AddCurrencyWidget param layout.
- **Alt:** force `bSanctifyingAvailable` / inject SanctityToken into available progression
  mechanics so the game's own code adds the widget. Dovetails with anvil-spawn feature.
- To inspect uasset blueprints here: `cat file | tr -c '[:print:]' '\n' | grep -aoE '[A-Za-z_][A-Za-z0-9_]{4,}'`
  dumps the FName table (no `strings` binary available). Proper graph view needs FModel/UAssetGUI.

## Build / Deploy / Log
- Build (from bash, in DLLMod dir): `cmd //c "echo. | .\\build.bat"` (build.bat ends in `pause`).
- Output: `build\dwmapi.dll`. Deploy to:
  `C:\Program Files (x86)\Steam\steamapps\common\Deadzone Rogue\Valhalla\Binaries\Win64\dwmapi.dll`
- Log (OVERWRITTEN each game launch): `C:\Users\jonathan\Desktop\SanctifyMod.log`
- DLL loads at game launch — code changes require a game restart.

## Current F-key handlers (mod.cpp ModThreadProc)
- F9  = spawn anvils (the original working blessing feature — don't break it)
- F10 = player position
- F8  = currency data-model dump (DumpStructProperties for AddCurrencyWidget/SetCurrencyId/
        BindCurrency/GetMatchCurrency params; DumpEnum for Currency-named enums)
- F7  = big currency HUD scan
- F6  = currency experiment on game thread (SetVisibility, OnAvailableRunProgressionMechanics
        Updated, GetMatchCurrency) — logs the GetMatchCurrency value
- F5  = vial/nanite/scrap/wallet/resource NAME discovery + WBP_Game_Currency_C instance int dump
- F4  = Cheat-Engine-style changed-value scan (press=snapshot, spend, press=diff).
        NOW float-aware; scans gameplay-singleton classes; offset window 0x10..0x1200;
        one VirtualQuery per object then direct reads; cap 2.5M entries.
- F3  = currency widget -> model trace (RESULT: widgets hold NO model pointer)
- F2  = ADDRESS RESOLVER. Parses every <Address>HEX</Address> from the saved CE table
        (C:\Users\jonathan\Desktop\Projects\Deadzone\DeadzoneSteam-Win64-Shipping.CT) at press
        time, walks GUObjectArray, reports INSIDE object "name" class +0xNN, or "not in any body
        (heap)" + nearest object below. Reads file at runtime so CE addrs (ASLR-fresh each launch)
        need no rebuild — workflow: scan in CE -> Save (Ctrl+S) -> press F2.

## CONFIRMED FINDINGS
1. **`WBP_Game_Currency_C + 0x570` = the live on-screen currency value (int32).**
   `+0x574` = animate-from / previous value (settles to 0). Confirmed: vial widgets went
   41->40 exactly matching a 1-vial spend ("had 40 here" note).
2. Multiple `WBP_Game_Currency_C` instances exist (one per currency per HUD/visor instance).
   In zone 4: a vial group (~40) and another group at ~3621 (scrap or similar).
3. **`GetMatchCurrency` is NOT vials.** On ValPlayerState, PropertiesSize=4, return-only int,
   NO input param. Returns 0 even with 44 vials on screen. Dead end.
4. **`ECurrencyType` enum** (no vials): 0 Credits, 1 Tokens, 2 Paid, 3 Common, 4 Uncommon,
   5 Rare, 6 Epic, 7 Legendary, 8 Exotic, 9 _MAX.
5. `AddCurrencyWidget` (PS=73, on WBP_Game_Currencies_C), `SetCurrencyId` (PS=50, on
   WBP_Game_Currency_C), `BindCurrency` (PS=33) all exist. **Their FProperty param-layout dump
   produced GARBAGE numeric offsets** => my FField/FProperty numeric offsets are WRONG for this
   build. Only the property TYPE names + the 0x78 inner-type ptr + enum dump were valid.
6. Widgets are fed by delegate broadcast and just cache the value at 0x570 — F3 found no model
   pointer (only +0x600 -> own WidgetBlueprintGeneratedClass). Can't reach source via widgets.
7. **Full-object body scans (F4) found NO int32 equal to the vial display count.** => source is
   either heap-stored (TMap/TArray) or a FLOAT on a class/offset not yet covered. (Latest
   float-aware F4 build was deployed but its results not yet analyzed when context was cleared.)
8. **F2 ADDRESS-RESOLVER CONFIRMS +0x570 (independent tool).** Cheat Engine 4-byte scan for the
   vial count narrowed to 9 addresses; F2 resolved 7 of them to `WBP_Game_Currency_C` instances
   at exactly **+0x570** (the other 2 were unrelated heap allocs near BodySetup / MovieSceneFloat
   Section). The "0xBE0 even spacing" between the 7 was just the WBP_Game_Currency_C object size —
   they are 7 consecutive widget instances, NOT an array of data structs. So CE re-found the
   display MIRRORS, not the source.
9. **The int scan caught ONLY int widget-mirrors, never an upstream source => source is very
   likely a FLOAT.** A 4-byte int scan for e.g. 39 cannot match a float 39.0f (bit pattern
   0x421C0000). NEXT: CE **Float** exact-value scan -> spend -> rescan -> save -> F2.
10. **`ValAttributeSet` is the prime SOURCE suspect.** F4 diff repeatedly flagged a `ValAttributeSet`
    object (addr 0x2B4041E63F0 this run) changing at **+0xF20 / +0xF24 / +0xF28** across spends.
    Attribute sets store float {BaseValue,CurrentValue} pairs and persist across zone transitions
    (unlike the per-HUD widgets, which are destroyed/recreated per zone). If the float scan + F2
    land here, that offset is our persistent cross-zone read source.

## UE5 memory offsets (this build, 5.5.4)
WORKING:
- UObject: Class +0x10, FName +0x18, Outer +0x20
- UStruct: SuperStruct 0x40, Children(UField*) 0x48, ChildProperties(FField*) 0x50, PropertiesSize 0x58
- FField: ClassPrivate +0x08 (gives correct property TYPE name)
- FProperty inner typed ptr +0x78 (EnumProperty -> ECurrencyType worked)
- FFieldClass: FName Name at +0x00
- UEnum: Names TArray data +0x40, count +0x48; entry = FName(8)+int64(8)=16B
UNVERIFIED / LIKELY WRONG for this build (re-derive via raw hex dump if param decode needed):
- FField NamePrivate (tried 0x28), FProperty ArrayDim 0x38 / ElementSize 0x3C / Flags 0x40 /
  Offset_Internal 0x4C — all produced garbage. Layout is shifted; dump raw bytes of a known
  FProperty (e.g. GetMatchCurrency's ReturnValue) to locate real offsets.

## Constants / gotchas (see also memory: feedback-approach, project-deadzone-mod)
- FNameToString offset from GUObjectArray_struct: -0x0A27FB50
- ProcessEvent vtable slot: 0x4C (prologue 40 55 56 57)
- UE functions MUST run on the game thread (TickHook window-timer). ProcessEvent off-thread for
  simple getters has worked but is risky.
- ~270k objects in-match (includes all loaded assets). Gameplay objects have high instance-number
  suffixes (created late) — beware snapshot caps truncating before reaching them.
- IsSafeToRead uses VirtualQuery (expensive per call).

## PLAN / NEXT STEPS
1. **Cheat Engine (user-driven):** Value Type = Float, exact scan = current vials, spend,
   next scan = new value, narrow to 1-3 addresses. Attributes store {BaseValue, CurrentValue}
   so expect TWO adjacent addresses 4 bytes apart. CE covers the whole address space incl. heap
   — this is the gap the in-mod body-scan can't reach.
2. **Address-resolver key (REQUESTED, build next):** given an absolute address, walk
   GUObjectArray and find which UObject's memory range contains it + the offset; report
   object/class/offset. If the address is NOT inside any object body => it's heap/TMap; then use
   CE "Find out what accesses this address" + the register holding the struct ptr, or a CE
   pointer scan to a static base, and trace from there.
3. **Compare scrap/tech (base) vs vials (DLC):** find scrap's read path; if scrap uses a clean
   currency getter but vials don't, that confirms vials are a separate (attribute) system.
4. If pursuing native-widget population (AddCurrencyWidget path): first re-derive the correct
   FProperty offsets via raw hex dump, then decode the currency-id param + the vial currency's
   true identity (not in ECurrencyType).

## Approaches already RULED OUT
- GetMatchCurrency as the vial getter (returns 0).
- Following widget pointers to a model (widgets hold none).
- int32 body-scan for the display value (no match in scanned objects).
- (User earlier declined a GDI overlay — keep populating/reading the NATIVE widget/data.)
