#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

#include <Structs/Inventory.h>

struct InventoryComponent
{
    Inventory Content{};
    // The creation snapshot may precede native default-outfit equip. Once a
    // real inventory/equipment event arrives, an empty inventory is explicit.
    bool HasAuthoritativeMutation{};
};
