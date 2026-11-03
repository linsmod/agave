# tools/check_prelude_lines.py
#
# Sanity check for a hand-formatted prelude: report lines that end while still
# inside a string (or a block comment), because a newline inside a string
# literal changes its bytes.

import io
import sys

path = sys.argv[1]
src = io.open(path, encoding='utf-8').read()

state = 'code'
quote = ''
line = 1
bad = []

i = 0
while i < len(src):
    c = src[i]

    if c == '\n':
        if state == 'string':
            bad.append((line, state))
        if state == 'line_comment':
            state = 'code'
        line += 1
        i += 1
        continue

    if state == 'code':
        if c == '/' and i + 1 < len(src) and src[i + 1] == '/':
            state = 'line_comment'; i += 2; continue
        if c == '/' and i + 1 < len(src) and src[i + 1] == '*':
            state = 'block_comment'; i += 2; continue
        if c in '"\'':
            state = 'string'; quote = c; i += 1; continue
    elif state == 'string':
        if c == '\\':
            i += 2; continue
        if c == quote:
            state = 'code'
    elif state == 'block_comment':
        if c == '*' and i + 1 < len(src) and src[i + 1] == '/':
            state = 'code'; i += 2; continue

    i += 1

print('%s: lines=%d bad_string_breaks=%d' % (path, src.count('\n'), len(bad)))
for b in bad[:20]:
    print('  line %d ends inside a string' % b[0])
