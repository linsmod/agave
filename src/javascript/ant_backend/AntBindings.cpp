/*
 * Copyright (c) 2026, agave project contributors
 * All rights reserved.
 *
 * The generated-interface registry (see AntBindings.h). One vector, filled by
 * the file-scope initializers in buildANT/Ant<Interface>.cpp; nothing here knows
 * which interfaces exist.
 */

#include "AntBindings.h"

#include <vector>
#include <cstring>

namespace {

std::vector<const AntbInterfaceDef*>& interfaceRegistry()
{
    /* Function-local so the vector is constructed on first use: the generated
     * files' initializers run before main() in an order nobody controls. */
    static std::vector<const AntbInterfaceDef*>* registry = new std::vector<const AntbInterfaceDef*>();
    return *registry;
}

} /* namespace */

namespace AntBindings {

int registerInterface(const AntbInterfaceDef* def)
{
    if (!def || !def->name)
        return 0;

    interfaceRegistry().push_back(def);
    return 1;
}

unsigned interfaceCount()
{
    return (unsigned)interfaceRegistry().size();
}

const AntbInterfaceDef* interfaceAt(unsigned index)
{
    if (index >= interfaceRegistry().size())
        return 0;
    return interfaceRegistry()[index];
}

const AntbInterfaceDef* findInterface(const char* name)
{
    if (!name)
        return 0;

    const std::vector<const AntbInterfaceDef*>& all = interfaceRegistry();

    for (size_t i = 0; i < all.size(); ++i) {
        if (all[i] && all[i]->name && strcmp(all[i]->name, name) == 0)
            return all[i];
    }
    return 0;
}

} /* namespace AntBindings */
