#!/usr/bin/env python3
"""Regenerate src/webcore/buildKJS/UserAgentStyleSheets.{h,cpp} from the CSS files.

The WebKit build runs src/webcore/css/make-css-file-arrays.pl for this; that path
needs perl plus a C preprocessor, and the generated files are checked in here, so
this script reproduces its output (same order, same 16-bytes-per-line layout) with
plain Python.

Usage:
    python tools/gen-ua-stylesheets.py        # rewrite the generated files

Run it after editing src/webcore/css/html4.css (or quirks/svg/view-source): the
engine parses those byte arrays, not the .css files, so a stale generated file
means the new rules silently never apply.
"""

import os
import re

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CSS_DIR = os.path.join(ROOT, 'src', 'webcore', 'css')
OUT_DIR = os.path.join(ROOT, 'src', 'webcore', 'buildKJS')

# (css file, symbol prefix). view-source.css becomes `sourceUserAgentStyleSheet`,
# which is the name CSSStyleSelector.cpp uses.
SHEETS = [
    ('html4.css', 'html4'),
    ('quirks.css', 'quirks'),
    ('view-source.css', 'source'),
    ('svg.css', 'svg'),
]


def crunch(text):
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    text = re.sub(r'\s+', ' ', text, flags=re.S)
    return text.strip()


def main():
    header = ['namespace WebCore {\n']
    body = ['namespace WebCore {\n']

    for name, prefix in SHEETS:
        path = os.path.join(CSS_DIR, name)

        with open(path, 'r', encoding='utf-8') as f:
            text = crunch(f.read())

        data = text.encode('utf-8')
        header.append('extern const char %sUserAgentStyleSheet[%d];\n' % (prefix, len(data)))
        body.append('extern const char %sUserAgentStyleSheet[%d] = {\n' % (prefix, len(data)))

        line = []
        for i, byte in enumerate(data):
            line.append(str(byte))
            if len(line) == 16 or i == len(data) - 1:
                body.append('    ' + ', '.join(line))
                body.append(',\n' if i != len(data) - 1 else '\n')
                line = []

        body.append('};\n')

    header.append('}\n')
    body.append('}\n')

    with open(os.path.join(OUT_DIR, 'UserAgentStyleSheets.h'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join(header))

    with open(os.path.join(OUT_DIR, 'UserAgentStyleSheetsData.cpp'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(''.join(body))

    print('wrote %s and UserAgentStyleSheetsData.cpp' % os.path.join(OUT_DIR, 'UserAgentStyleSheets.h'))


if __name__ == '__main__':
    main()
