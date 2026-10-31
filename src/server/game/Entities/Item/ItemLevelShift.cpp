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
    // RandPropPoints column for a slot, as GenerateEnchSuffixFactor picks it; -1 = no budget.
    int32 BudgetColumn(uint32 inventoryType)
    {
        switch (inventoryType)
        {
            case INVTYPE_HEAD: case INVTYPE_BODY: case INVTYPE_CHEST: case INVTYPE_LEGS:
            case INVTYPE_2HWEAPON: case INVTYPE_ROBE:
                return 0;
            case INVTYPE_SHOULDERS: case INVTYPE_WAIST: case INVTYPE_FEET: case INVTYPE_HANDS:
            case INVTYPE_TRINKET:
                return 1;
            case INVTYPE_NECK: case INVTYPE_WRISTS: case INVTYPE_FINGER: case INVTYPE_SHIELD:
            case INVTYPE_CLOAK: case INVTYPE_HOLDABLE:
                return 2;
            case INVTYPE_WEAPON: case INVTYPE_WEAPONMAINHAND: case INVTYPE_WEAPONOFFHAND:
                return 3;
            case INVTYPE_RANGED: case INVTYPE_THROWN: case INVTYPE_RANGEDRIGHT:
                return 4;
            default:
                return -1;
        }
    }

    // Stat budget of an item of this quality and slot at this item level.
    uint32 Budget(ItemTemplate const* proto, uint32 itemLevel)
    {
        int32 const column = BudgetColumn(proto->InventoryType);
        RandomPropertiesPointsEntry const* points = sRandomPropertiesPointsStore.LookupEntry(itemLevel);
        if (column < 0 || !points)
            return 0;
        switch (proto->Quality)
        {
            case ITEM_QUALITY_UNCOMMON:
                return points->UncommonPropertiesPoints[column];
            case ITEM_QUALITY_RARE:
            case ITEM_QUALITY_HEIRLOOM:
                return points->RarePropertiesPoints[column];
            case ITEM_QUALITY_EPIC:
            case ITEM_QUALITY_LEGENDARY:
            case ITEM_QUALITY_ARTIFACT:
                return points->EpicPropertiesPoints[column];
            default:
                // Poor and common items have no column; uncommon follows the same curve.
                return points->UncommonPropertiesPoints[column];
        }
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

    // Stats follow the item level budget, keeping the item's own stat mix.
    uint32 const oldBudget = Budget(proto, proto->ItemLevel);
    uint32 const newBudget = Budget(proto, shifted->ItemLevel);
    double const factor = oldBudget && newBudget ? double(newBudget) / double(oldBudget)
        : double(shifted->ItemLevel) / double(std::max<uint32>(proto->ItemLevel, 1));
    for (uint8 i = 0; i < MAX_ITEM_PROTO_STATS; ++i)
        if (shifted->ItemStat[i].ItemStatValue)
            shifted->ItemStat[i].ItemStatValue = int32(std::lround(double(proto->ItemStat[i].ItemStatValue) * factor));

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
