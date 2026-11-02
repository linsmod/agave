/*
 * CSSGradientValue.h - minimal CSS linear-gradient support.
 *
 * The 2010 engine has no gradient value at all, so this adds just enough to
 * paint `linear-gradient(<angle>|to <side>, <stop>, ...)` as a background.
 */

#ifndef CSSGradientValue_h
#define CSSGradientValue_h

#include "CSSValue.h"
#include "Color.h"
#include <wtf/Vector.h>

namespace WebCore {

class CSSGradientValue : public CSSValue {
public:
    struct Stop {
        float  offset;      /* 0..1, resolved by the parser */
        Color  color;
    };

    CSSGradientValue()
        : m_angle(180.0f)     /* CSS angles: 0 = to top, 90 = to right, 180 = to bottom */
        , m_repeating(false)
    {
    }

    void setAngle(float angle) { m_angle = angle; }
    float angle() const { return m_angle; }

    void setRepeating(bool repeating) { m_repeating = repeating; }
    bool isRepeating() const { return m_repeating; }

    void addStop(const Stop& stop) { m_stops.append(stop); }
    const Vector<Stop>& stops() const { return m_stops; }
    Vector<Stop>& mutableStops() { return m_stops; }

    virtual bool isGradientValue() const { return true; }
    virtual String cssText() const;

private:
    float m_angle;
    bool  m_repeating;
    Vector<Stop> m_stops;
};

} // namespace WebCore

#endif /* CSSGradientValue_h */
