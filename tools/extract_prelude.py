# tools/extract_prelude.py
#
# One-off helper: turn the `kPrelude` C string array in AntRuntime.cpp into a
# standalone prelude.js. Each C fragment is emitted as one JS line when the
# fragment ends in code state; fragments that stop inside a string/comment are
# joined without a newline (a newline there would change the literal).

import io
import re
import sys

SRC = sys.argv[1] if len(sys.argv) > 1 else 'src/javascript/ant_backend/AntRuntime.cpp'
OUT = sys.argv[2] if len(sys.argv) > 2 else 'src/javascript/ant_backend/prelude.js'

lines = io.open(SRC, encoding='utf-8', errors='surrogateescape').read().split('\n')

start = next(i for i, l in enumerate(lines) if 'static const char kPrelude[]' in l)
end = next(i for i, l in enumerate(lines) if i > start and '})(window);' in l)

print('start=%d end=%d span=%d' % (start, end, end - start))
print('first fragment line: %r' % lines[start + 1][:100])
print('last fragment line : %r' % lines[end][:100])

frag_re = re.compile(r'^\s*"((?:[^"\\]|\\.)*)"\s*[,;]?\s*$')


def unescape(s):
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == '\\' and i + 1 < len(s):
            n = s[i + 1]
            if n == 'n':
                out.append('\n'); i += 2
            elif n == 't':
                out.append('\t'); i += 2
            elif n == 'r':
                out.append('\r'); i += 2
            elif n == '"':
                out.append('"'); i += 2
            elif n == '\\':
                out.append('\\'); i += 2
            elif n == "'":
                out.append("'"); i += 2
            elif n == 'x' and i + 3 < len(s):
                out.append(chr(int(s[i + 2:i + 4], 16))); i += 4
            else:
                out.append(n); i += 2
        else:
            out.append(c); i += 1
    return ''.join(out)


frags = []
for line in lines[start + 1:end + 1]:
    m = frag_re.match(line)
    if m:
        frags.append(unescape(m.group(1)))
    elif line.strip().startswith('"})(window);";'):
        frags.append('})(window);')

# Emit a newline after a fragment only when it ends in code state, so a literal
# that spans two fragments keeps its exact bytes.
state = 'code'
quote = ''
nl_safe = True
body = []

for frag in frags:
    i = 0
    while i < len(frag):
        c = frag[i]
        if state == 'code':
            if c == '/' and i + 1 < len(frag) and frag[i + 1] == '/':
                state = 'line_comment'; i += 2; continue
            if c == '/' and i + 1 < len(frag) and frag[i + 1] == '*':
                state = 'block_comment'; i += 2; continue
            if c in '\'"`':
                state = 'string'; quote = c; i += 1; continue
        elif state == 'string':
            if c == '\\':
                i += 2; continue
            if c == quote:
                state = 'code'
        elif state == 'line_comment':
            if c == '\n':
                state = 'code'
        elif state == 'block_comment':
            if c == '*' and i + 1 < len(frag) and frag[i + 1] == '/':
                state = 'code'; i += 2; continue
        i += 1
    body.append(frag)
    body.append('\n' if state == 'code' else '')

text = ''.join(body)
text = text.rstrip('\n') + '\n'

io.open(OUT, 'w', encoding='utf-8', newline='\n').write(text)
print('fragments=%d lines=%d bytes=%d -> %s' % (len(frags), text.count('\n'), len(text), OUT))
