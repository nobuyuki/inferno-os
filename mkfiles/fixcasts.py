#!/usr/bin/env python3
# porting aid: read gcc diagnostics on stdin and insert (uintptr) after each
# cast reported by -Wpointer-to-int-cast or -Wint-to-pointer-cast.
# usage: fixcasts.py dir < log      (the result must be reviewed by hand)
import sys, re, os
d = sys.argv[1]
sites = set()
for l in sys.stdin:
    m = re.match(r'([^:\s]+):(\d+):(\d+): warning: cast (from pointer to integer|to pointer from integer) of different size', l)
    if m and not m.group(1).startswith('/'):
        sites.add((os.path.normpath(os.path.join(d, m.group(1))), int(m.group(2)), int(m.group(3))))
byfile = {}
for f, ln, col in sites:
    byfile.setdefault(f, []).append((ln, col))
for f, ss in byfile.items():
    lines = open(f, encoding='latin-1').read().split('\n')
    for ln, col in sorted(ss, reverse=True):
        s = lines[ln-1]
        # gcc columns count a tab as advancing to the next multiple of 8
        i, c = 0, 1
        while i < len(s) and c < col:
            c = c + 1 if s[i] != '\t' else ((c-1)//8+1)*8+1
            i += 1
        if i >= len(s) or s[i] != '(':
            print('skip', f, ln, col, s.strip()); continue
        j = s.index(')', i)
        lines[ln-1] = s[:j+1] + '(uintptr)' + s[j+1:]
        print('%s:%d: %s' % (f, ln, lines[ln-1].strip()))
    open(f, 'w', encoding='latin-1').write('\n'.join(lines))
