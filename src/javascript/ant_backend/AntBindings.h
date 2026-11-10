/*
 * Copyright (c) 2026, agave project contributors
 * All rights reserved.
 *
 * The ant backend's interface description ABI.
 *
 * WebCore/*.idl is the single source of an interface's shape: which attributes
 * exist, which of them are writable, which methods it owns, and its constants.
 * CodeGeneratorAnt.pm (bindings/scripts) turns one IDL file into one
 * buildANT/Ant<Interface>.cpp that fills the structs below, and the runtime
 * consumes them instead of keeping the same facts in hand-written C++ tables
 * (the facade's kDomAttributes/kDomMethods and the per-interface registrations
 * in AntRuntime.cpp).
 *
 * Deliberately plain data: the generated translation unit needs no function
 * pointers and no knowledge of AntRuntime, so a generator change cannot break
 * the engine and the engine's behaviour is not coupled to codegen output.
 * Behaviour (what an attribute actually returns, which C++ method a JS call
 * lands on) stays hand-written in the custom layer - the IDL only decides
 * *which* names and *what shape* they have.
 */

#ifndef ANTBINDINGS_H
#define ANTBINDINGS_H

/* --- attribute flags ---------------------------------------------------- */
#define ANTB_ATTR_WRITABLE 1u   /* `attribute` (a setter exists), not `readonly attribute` */
#define ANTB_ATTR_CUSTOM   2u   /* [Custom] / [CustomGetter] / [CustomSetter] */
#define ANTB_ATTR_REPLACEABLE 4u
#define ANTB_ATTR_PUT_FORWARDS 8u

/* --- method flags ------------------------------------------------------- */
#define ANTB_METHOD_CUSTOM 1u   /* [Custom] */

struct AntbAttributeDef {
    const char* name;
    unsigned    flags;      /* ANTB_ATTR_* */
    const char* valueType;  /* IDL type, for the JS->native conversion layer */
};

struct AntbMethodDef {
    const char* name;
    unsigned    argCount;   /* -1 when the IDL declares variadic/optional */
    unsigned    flags;      /* ANTB_METHOD_* */
    const char* returnType;
};

struct AntbConstantDef {
    const char* name;
    double      value;
};

/* One interface. `parent` is the IDL's first parent (`interface X : Y, Z` ->
 * "Y"); the rest are implemented interfaces whose members the IDL already
 * flattened into this interface's own lists. Members are the interface's own
 * declarations only: inherited ones are reached through the prototype chain. */
struct AntbInterfaceDef {
    const char*             name;
    const char*             parent;
    const AntbAttributeDef* attributes;
    unsigned                attributeCount;
    const AntbMethodDef*    methods;
    unsigned                methodCount;
    const AntbConstantDef*  constants;
    unsigned                constantCount;
};

namespace AntBindings {

/* Generated files register themselves from a file-scope initializer, so adding
 * an interface is "generate the file and let the build compile it" - there is
 * no hand-kept interface list to fall out of date. */
int registerInterface(const AntbInterfaceDef* def);

unsigned interfaceCount();
const AntbInterfaceDef* interfaceAt(unsigned index);
const AntbInterfaceDef* findInterface(const char* name);

} /* namespace AntBindings */

#endif /* ANTBINDINGS_H */
