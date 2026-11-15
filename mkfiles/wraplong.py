#!/usr/bin/env python3
# one-off porting aid: bracket system #includes that follow lib9.h
# with push_macro/pop_macro of `long' (see Linux/amd64/include/lib9.h)
import sys, os, re
root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
inc = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')
def local(name, q, d):
    for p in (d, root+'/include', root+'/Linux/amd64/include', root+'/libinterp', root+'/emu/port', root+'/emu/Linux',
              root+'/utils/include', os.path.join(d, '..', 'cc'), os.path.join(d, '..', 'ld'), os.path.join(d, '..', 'include')):
        if os.path.exists(os.path.join(p, name)):
            return True
    return False
for f in sys.argv[1:]:
    lines = open(f, encoding='latin-1').read().split('\n')
    if any('push_macro("long")' in l for l in lines):
        continue
    out, seen, inrun, changed = [], False, False, False
    for l in lines:
        m = inc.match(l)
        issys = bool(m) and not local(m.group(2), m.group(1), os.path.dirname(f) or '.')
        if m and not issys:
            seen = True
        if seen and issys and not inrun:
            out += ['#pragma push_macro("long")', '#undef long']
            inrun = changed = True
        elif inrun and not issys and l.strip() != '' and not l.lstrip().startswith(('#if', '#endif', '#else', '#elif', '/*', '*', '//', '#define', '#undef')):
            out.append('#pragma pop_macro("long")')
            inrun = False
        out.append(l)
    if inrun:
        out.append('#pragma pop_macro("long")')
    if changed:
        open(f, 'w', encoding='latin-1').write('\n'.join(out))
        print('wrapped', f)
