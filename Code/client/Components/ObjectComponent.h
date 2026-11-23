#pragma once

#ifndef TP_INTERNAL_COMPONENTS_GUARD
#error Include Components.h instead
#endif

struct ObjectComponent
{
    ObjectComponent(const uint32_t acId)
        : Id(acId)
    {
    }

    // Set through emplace, replace or patch only: World keeps an index of server ids.
    uint32_t Id;
};
