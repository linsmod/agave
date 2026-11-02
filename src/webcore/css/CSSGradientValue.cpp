/*
 * CSSGradientValue.cpp
 */

#include "config.h"
#include "CSSGradientValue.h"
#include "PlatformString.h"

namespace WebCore {

String CSSGradientValue::cssText() const
{
    String result("linear-gradient(");
    result += String::number(m_angle);
    result += "deg";

    for (unsigned i = 0; i < m_stops.size(); ++i) {
        result += ", ";
        result += m_stops[i].color.name();
        result += " ";
        result += String::number(m_stops[i].offset * 100.0);
        result += "%";
    }

    result += ")";
    return result;
}

} // namespace WebCore
