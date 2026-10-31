/*
* This file is part of Project SkyFire https://www.projectskyfire.org.
* See LICENSE.md file for Copyright information
*/

#include "ItemLevelShift.h"

#include "ByteBuffer.h"
#include "DBCStores.h"
#include "ItemPrototype.h"

#include <cmath>
#include <map>
#include <memory>
#include <mutex>

// ObjectMgr.cpp: the item damage tables by item level (ItemDamage*.dbc).
void FillItemDamageFields(float* minDamage, float* maxDamage, float* dps, uint32 itemLevel, uint32 itemClass,
    uint32 itemSubClass, uint32 quality, uint32 delay, float statScalingFactor, uint32 inventoryType, uint32 flags2);

namespace
{
    // RandPropPoints column for a slot, as the 5.4.8 client picks it (bows and guns are
    // two-handers here, wands one-handers); -1 = no budget.
    int32 BudgetColumn(ItemTemplate const* proto)
    {
        switch (proto->InventoryType)
        {
            case INVTYPE_HEAD: case INVTYPE_BODY: case INVTYPE_CHEST: case INVTYPE_LEGS:
            case INVTYPE_RANGED: case INVTYPE_2HWEAPON: case INVTYPE_ROBE: case INVTYPE_THROWN:
                return 0;
            case INVTYPE_RANGEDRIGHT:
                return proto->SubClass == ITEM_SUBCLASS_WEAPON_WAND ? 3 : 0;
            case INVTYPE_WEAPON: case INVTYPE_WEAPONMAINHAND: case INVTYPE_WEAPONOFFHAND:
                return 3;
            case INVTYPE_SHOULDERS: case INVTYPE_WAIST: case INVTYPE_FEET: case INVTYPE_HANDS:
            case INVTYPE_TRINKET:
                return 1;
            case INVTYPE_NECK: case INVTYPE_WRISTS: case INVTYPE_FINGER: case INVTYPE_SHIELD:
            case INVTYPE_CLOAK: case INVTYPE_HOLDABLE:
                return 2;
            case INVTYPE_RELIC:
                return 4;
            default:
                return -1;
        }
    }

    // Stat budget of an item of this quality and slot at this item level; 0 for poor and
    // common items, which the client does not scale either.
    float Budget(ItemTemplate const* proto, uint32 itemLevel)
    {
        int32 const column = BudgetColumn(proto);
        RandomPropertiesPointsEntry const* points = sRandomPropertiesPointsStore.LookupEntry(itemLevel);
        if (column < 0 || !points)
            return 0.0f;
        switch (proto->Quality)
        {
            case ITEM_QUALITY_UNCOMMON:
                return float(points->UncommonPropertiesPoints[column]);
            case ITEM_QUALITY_RARE:
            case ITEM_QUALITY_HEIRLOOM:
                return float(points->RarePropertiesPoints[column]);
            case ITEM_QUALITY_EPIC:
            case ITEM_QUALITY_LEGENDARY:
            case ITEM_QUALITY_ARTIFACT:
                return float(points->EpicPropertiesPoints[column]);
            default:
                return 0.0f;
        }
    }

    float SocketCost(uint32 itemLevel)
    {
        GtItemSocketCostPerLevelEntry const* entry = itemLevel ? sGtItemSocketCostPerLevelStore.LookupEntry(itemLevel - 1) : nullptr;
        return entry ? entry->cost : 0.0f;
    }

    // One stat at another item level, the way the 5.4.8 client computes it for upgraded
    // items: budget * allocation / 10000, minus int(socket cost multiplier * socket cost),
    // rounded. The database keeps the multiplier rounded to a whole number (1.5 is stored as
    // 2), so it is recovered from the item's own value at its base item level, to a quarter.
    // Verified against the owner's tooltips (Robes of the Tendered Heart at 503/553/603,
    // Corsair's Overshirt at 20/70). Returns false when the item has no allocation data.
    bool ClientStatValue(ItemTemplate const* proto, uint8 index, uint32 itemLevel, int32& value)
    {
        _ItemStat const& stat = proto->ItemStat[index];
        float const baseBudget = Budget(proto, proto->ItemLevel);
        float const newBudget = Budget(proto, itemLevel);
        if (stat.ItemStatUnk1 <= 0 || baseBudget <= 0.0f || newBudget <= 0.0f)
            return false;

        float multiplier = 0.0f;
        if (stat.ItemStatUnk2)
            if (float const baseCost = SocketCost(proto->ItemLevel); baseCost > 0.0f)
            {
                float const deducted = baseBudget * float(stat.ItemStatUnk1) * 0.0001f - float(stat.ItemStatValue);
                multiplier = std::max(0.0f, std::round(deducted / baseCost * 4.0f) / 4.0f);
            }

        float result = newBudget * float(stat.ItemStatUnk1) * 0.0001f;
        result -= float(int32(multiplier * SocketCost(itemLevel)));
        value = int32(std::floor(result + 0.5f));
        return true;
    }

    std::mutex g_lock;
    std::map<std::pair<uint32, int32>, std::unique_ptr<ItemTemplate>> g_cache;
}

ItemTemplate const* ItemLevelShift::ShiftedTemplate(ItemTemplate const* proto, int32 shift)
{
    if (!proto || !shift)
        return proto;

    std::lock_guard<std::mutex> guard(g_lock);
    auto key = std::make_pair(proto->ItemId, shift);
    auto it = g_cache.find(key);
    if (it != g_cache.end())
        return it->second.get();

    auto shifted = std::make_unique<ItemTemplate>(*proto);
    int32 const level = std::max<int32>(1, int32(proto->ItemLevel) + shift);
    shifted->ItemLevel = uint32(level);

    // Stats as the client computes them for the shifted item (budget share and socket cost);
    // items without allocation data scale by the budget ratio instead.
    float const oldBudget = Budget(proto, proto->ItemLevel);
    float const newBudget = Budget(proto, shifted->ItemLevel);
    double const factor = oldBudget > 0.0f && newBudget > 0.0f ? double(newBudget) / double(oldBudget)
        : double(shifted->ItemLevel) / double(std::max<uint32>(proto->ItemLevel, 1));
    for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
    {
        if (!proto->ItemStat[i].ItemStatValue)
            continue;
        int32 value = 0;
        if (!ClientStatValue(proto, i, shifted->ItemLevel, value))
            value = int32(std::lround(double(proto->ItemStat[i].ItemStatValue) * factor));
        shifted->ItemStat[i].ItemStatValue = value;
    }

    // Armor comes from ItemTemplate::GetArmor() at the new item level; weapon damage
    // from the damage tables, as at load.
    if (proto->Class == ITEM_CLASS_WEAPON)
    {
        float minDamage = 0.0f, maxDamage = 0.0f, dps = 0.0f;
        FillItemDamageFields(&minDamage, &maxDamage, &dps, shifted->ItemLevel, proto->Class, proto->SubClass,
            proto->Quality, proto->Delay, proto->StatScalingFactor, proto->InventoryType, proto->Flags2);
        if (dps > 0.0f)
        {
            shifted->DamageMin = minDamage;
            shifted->DamageMax = maxDamage;
            shifted->DPS = dps;
        }
    }

    ItemTemplate const* result = shifted.get();
    g_cache.emplace(key, std::move(shifted));
    return result;
}

void ItemLevelShift::WriteUpgradeRecord(uint32 upgradeId, ByteBuffer& record)
{
    record << uint32(upgradeId);
    record << uint32(UPGRADE_PATH);                 // an existing ItemUpgradePath
    record << int32(ShiftOf(upgradeId));            // item level bonus
    record << uint32(0);                            // previous upgrade
    record << uint32(0);                            // currency
    record << uint32(0);                            // cost
}
