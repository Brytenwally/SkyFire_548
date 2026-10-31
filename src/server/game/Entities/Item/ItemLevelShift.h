/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*
* Per-item item level shift, carried by the item's upgrade modifier
* (ITEM_DYNAMIC_MODIFIERS index 2) like 5.4's item upgrades. The upgrade ids
* are synthetic: UPGRADE_ID_BASE + MAX_SHIFT + shift, served to the client as
* ItemUpgrade.db2 hotfix rows whose item level bonus is the shift, so the
* client recomputes the tooltip for the shifted item level.
*
* The client shows it only when ITEM_FIELD_MODIFIERS_MASK covers indices 0-2 (mask 7):
* confirmed in game with Blizzard's row 492 (item level 553 -> 557, "Upgrade Level: 1/4",
* stats recomputed by the client).
*
* Test feature (`.debug itemlevel`): the shift is not saved with the item yet.
*/

#ifndef SF_ITEM_LEVEL_SHIFT_H
#define SF_ITEM_LEVEL_SHIFT_H

#include "Define.h"

#include <vector>

class ByteBuffer;
struct ItemTemplate;

namespace ItemLevelShift
{
    constexpr uint32 MODIFIER_INDEX = 2;            // ITEM_DYNAMIC_MODIFIERS: 0 reforge, 1 transmog, 2 upgrade
    constexpr uint32 UPGRADE_ID_BASE = 900000;
    constexpr int32 MAX_SHIFT = 1000;
    // Rows announced to clients at login (hotfix list).
    constexpr int32 HOTFIX_MIN_SHIFT = -200;
    constexpr int32 HOTFIX_MAX_SHIFT = 600;
    constexpr uint32 HOTFIX_TIMESTAMP = 1761980400; // fixed, so clients keep their cached rows; bump when the rows change

    inline bool IsShiftUpgradeId(uint32 id)
    {
        return id >= UPGRADE_ID_BASE && id <= UPGRADE_ID_BASE + 2 * uint32(MAX_SHIFT);
    }

    inline int32 ShiftOf(uint32 upgradeId)
    {
        return IsShiftUpgradeId(upgradeId) ? int32(upgradeId - UPGRADE_ID_BASE) - MAX_SHIFT : 0;
    }

    inline uint32 UpgradeIdFor(int32 shift)
    {
        return shift ? UPGRADE_ID_BASE + uint32(shift + MAX_SHIFT) : 0;
    }

    // The template with ItemLevel + shift: stats scaled by the item level budget
    // (RandPropPoints), armor and weapon damage recomputed for the new item level.
    // Cached; the pointer stays valid for the server's lifetime.
    ItemTemplate const* ShiftedTemplate(ItemTemplate const* proto, int32 shift);

    // RandPropPoints budget of this item's quality and slot at an item level (the client's
    // column choice); 0 for poor/common items or slots without a budget. A random suffix's
    // stats are allocation x suffix factor / 10000, so this is also a full suffix factor.
    float StatBudget(ItemTemplate const* proto, uint32 itemLevel);

    // ItemUpgrade.db2 (5.4.8 build 18273, 46 rows, read from the owner's client data):
    // ID, ItemUpgradePathID, ItemLevelBonus (total, not per step: 446 = +4, 447 = +8),
    // PrevUpgradeID, CurrencyID (396 Valor), CurrencyCost. Paths live in ItemUpgradePath.dbc
    // (1, 89-91, 108, 110-113, 116-119, 124, 125), a DBC the server cannot hotfix, so the
    // synthetic rows reuse a real one.
    constexpr uint32 UPGRADE_PATH = 124;            // the 5.4 path (Siege of Orgrimmar items start at 491)
    void WriteUpgradeRecord(uint32 upgradeId, ByteBuffer& record);
}

#endif
