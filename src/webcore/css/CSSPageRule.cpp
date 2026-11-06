/**
 * This file is part of the DOM implementation for KDE.
 *
 * (C) 1999-2003 Lars Knoll (knoll@kde.org)
 * (C) 2002-2003 Dirk Mueller (mueller@kde.org)
 * Copyright (C) 2002, 2005, 2006 Apple Computer, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public License
 * along with this library; see the file COPYING.LIB.  If not, write to
 * the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301, USA.
 */

#include "config.h"
#include "CSSPageRule.h"

#include "CSSMutableStyleDeclaration.h"

namespace WebCore {

CSSPageRule::CSSPageRule(StyleBase* parent)
    : CSSRule(parent)
{
}

CSSPageRule::~CSSPageRule()
{
}

void CSSPageRule::setDeclaration(PassRefPtr<CSSMutableStyleDeclaration> style)
{
    m_style = style;
}

void CSSPageRule::setSelectorText(const String& selectorText, ExceptionCode&)
{
    /* No reparse: the selector only picks which page box the rule applies to,
     * and nothing here implements paged media yet. Keeping the text makes
     * `rule.selectorText = x; rule.selectorText` consistent. */
    m_selectorText = selectorText;
}

String CSSPageRule::cssText() const
{
    String result("@page");

    if (!m_selectorText.isEmpty()) {
        result += " ";
        result += m_selectorText;
    }

    result += " { ";
    if (m_style)
        result += m_style->cssText();
    result += " }";
    return result;
}

} // namespace WebCore
