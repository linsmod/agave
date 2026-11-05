/*
    This file is part of the KDE libraries

    Copyright (C) 1999 Lars Knoll (knoll@kde.org)
    Copyright (C) 2006 Apple Computer, Inc.

    This library is free software; you can redistribute it and/or
    modify it under the terms of the GNU Library General Public
    License as published by the Free Software Foundation; either
    version 2 of the License, or (at your option) any later version.

    This library is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
    Library General Public License for more details.

    You should have received a copy of the GNU Library General Public License
    along with this library; see the file COPYING.LIB.  If not, write to
    the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
    Boston, MA 02110-1301, USA.
*/

#ifndef Length_h
#define Length_h

#include <wtf/Assertions.h>

namespace WebCore {

    const int undefinedLength = -1;
    const int percentScaleFactor = 128;

    /* Calculated holds `percent + px` (CSS calc() with a percentage term).
     * The percentage lives in m_value exactly like Percent; the pixel part is
     * an integer offset in m_calcPx. calcValue() adds the two, which is what
     * most layout code needs; the few sites that treat Percent specially must
     * also accept Calculated (see isCalculated() call sites in RenderBox /
     * RenderBlock / RenderObject). */
    enum LengthType { Auto, Relative, Percent, Fixed, Static, Intrinsic, MinIntrinsic, Calculated };

    struct Length {
        Length()
            : m_value(0), m_calcPx(0)
        {
        }

        Length(LengthType t)
            : m_value(t), m_calcPx(0)
        {
        }

        Length(int v, LengthType t, bool q = false)
            : m_value((v * 16) | (q << 3) | t), m_calcPx(0) // FIXME: Doesn't work if the passed-in value is very large!
        {
            ASSERT(t != Percent);
            ASSERT(t != Calculated);
        }

        Length(double v, LengthType t, bool q = false)
            : m_value(static_cast<int>(v * percentScaleFactor) * 16 | (q << 3) | t), m_calcPx(0)
        {
            ASSERT(t == Percent);
        }

        /* calc(): percentage plus a pixel offset. */
        Length(double percent, int px)
            : m_value(static_cast<int>(percent * percentScaleFactor) * 16 | Calculated)
            , m_calcPx(px)
        {
        }

        bool operator==(const Length& o) const { return m_value == o.m_value && m_calcPx == o.m_calcPx; }
        bool operator!=(const Length& o) const { return !(*this == o); }

        int value() const {
            ASSERT(type() != Percent);
            ASSERT(type() != Calculated);
            return rawValue();
        }

        int calcPx() const { return m_calcPx; }

        double calcPercent() const
        {
            ASSERT(type() == Calculated);
            return static_cast<double>(rawValue()) / percentScaleFactor;
        }

        int rawValue() const { return (m_value & ~0xF) / 16; }

        double percent() const
        {
            ASSERT(type() == Percent);
            return static_cast<double>(rawValue()) / percentScaleFactor;
        }

        LengthType type() const { return static_cast<LengthType>(m_value & 7); }
        bool quirk() const { return (m_value >> 3) & 1; }

        void setValue(LengthType t, int value)
        {
            ASSERT(t != Percent);
            setRawValue(t, value);
        }

        void setRawValue(LengthType t, int value) { m_value = value * 16 | (m_value & 0x8) | t; }

        void setValue(int value)
        {
            ASSERT(!value || (type() != Percent && type() != Calculated));
            setRawValue(value);
        }

        void setRawValue(int value) { m_value = value * 16 | (m_value & 0xF); }

        void setValue(LengthType t, double value)
        {
            ASSERT(t == Percent);
            m_value = static_cast<int>(value * percentScaleFactor) * 16 | (m_value & 0x8) | t;
        }

        void setValue(double value)
        {
            ASSERT(type() == Percent);
            m_value = static_cast<int>(value * percentScaleFactor) * 16 | (m_value & 0xF);
        }

        // note: works only for certain types, returns undefinedLength otherwise
        int calcValue(int maxValue) const
        {
            switch (type()) {
                case Fixed:
                    return value();
                case Percent:
                    return maxValue * rawValue() / (100 * percentScaleFactor);
                case Calculated: {
                    /* Chrome rounds a percentage resize to the nearest pixel;
                     * 33.333% of 600 must land on 200, not 199. */
                    double v = maxValue * (double)rawValue() / (100.0 * percentScaleFactor) + m_calcPx;
                    return v < 0 ? (int)(v - 0.5) : (int)(v + 0.5);
                }
                case Auto:
                    return maxValue;
                default:
                    return undefinedLength;
            }
        }

        int calcMinValue(int maxValue) const
        {
            switch (type()) {
                case Fixed:
                    return value();
                case Percent:
                    return maxValue * rawValue() / (100 * percentScaleFactor);
                case Calculated:
                    return maxValue * rawValue() / (100 * percentScaleFactor) + m_calcPx;
                case Auto:
                default:
                    return 0;
            }
        }

        bool isUndefined() const { return rawValue() == undefinedLength; }
        bool isZero() const { return !(m_value & ~0xF) && !m_calcPx; }
        bool isPositive() const { return type() == Calculated ? (rawValue() > 0 || m_calcPx > 0) : rawValue() > 0; }
        bool isNegative() const { return type() == Calculated ? (rawValue() < 0 && m_calcPx < 0) : rawValue() < 0; }

        bool isAuto() const { return type() == Auto; }
        bool isRelative() const { return type() == Relative; }
        bool isPercent() const { return type() == Percent; }
        bool isCalculated() const { return type() == Calculated; }
        bool isFixed() const { return type() == Fixed; }
        bool isStatic() const { return type() == Static; }
        bool isIntrinsicOrAuto() const { return type() == Auto || type() == MinIntrinsic || type() == Intrinsic; }

    private:
        int m_value;
        int m_calcPx;
    };

} // namespace WebCore

#endif // Length_h
