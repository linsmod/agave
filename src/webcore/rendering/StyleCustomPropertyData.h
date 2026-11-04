/*
 * StyleCustomPropertyData.h - the custom properties ('--x') of one element.
 *
 * A custom property is resolved during style resolution, not while the
 * stylesheet is parsed: the value depends on the cascade at the element
 * (theme classes, per-component overrides) and on the parent's values. Each
 * RenderStyle therefore carries one of these maps; a child style starts out
 * sharing its parent's map (the data is immutable once resolution finished) and
 * copies it only when the child declares a property of its own.
 */

#ifndef StyleCustomPropertyData_h
#define StyleCustomPropertyData_h

#include "AtomicString.h"
#include "PlatformString.h"
#include <wtf/HashMap.h>
#include <wtf/PassRefPtr.h>
#include "Shared.h"
#include "StringHash.h"

namespace WebCore {

class StyleCustomPropertyData : public Shared<StyleCustomPropertyData> {
public:
    static PassRefPtr<StyleCustomPropertyData> create();
    PassRefPtr<StyleCustomPropertyData> copy() const;

    bool hasProperty(const String& name) const { return m_values.contains(name); }

    String getProperty(const String& name) const
    {
        return m_values.get(name);
    }

    void setProperty(const String& name, const String& value)
    {
        m_values.set(name, value);
    }

    size_t size() const { return m_values.size(); }

private:
    HashMap<String, String> m_values;
};

/* Defined after the class: adoptRef() instantiates Shared<T>::deref(), which
 * needs the complete type. */
inline PassRefPtr<StyleCustomPropertyData> StyleCustomPropertyData::create()
{
    /* Shared<> starts at refcount 0, so PassRefPtr(T*) (which refs) is the
     * right way to hand out the first reference; adoptRef() is for objects
     * that already start at 1. */
    return PassRefPtr<StyleCustomPropertyData>(new StyleCustomPropertyData);
}

inline PassRefPtr<StyleCustomPropertyData> StyleCustomPropertyData::copy() const
{
    StyleCustomPropertyData* data = new StyleCustomPropertyData;

    data->m_values = m_values;
    return PassRefPtr<StyleCustomPropertyData>(data);
}

} // namespace WebCore

#endif // StyleCustomPropertyData_h
