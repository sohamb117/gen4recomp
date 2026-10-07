import re, sys, math
for arg in sys.argv[1:]:
    name, est = arg.split('=')
    p = 'tests/e2e/diamond/%s/milestone.toml' % name
    t = open(p).read()
    t = re.sub(r'^status = "planned"\n', '', t, flags=re.M)
    t = re.sub(r'^estimate = \d+', 'estimate = %s' % est, t, count=1, flags=re.M)
    budget = int(math.ceil(int(est) * 1.5 / 100.0) * 100)
    t = re.sub(r'(\[run\]\n(?:[^\[]*?\n)?)frames = \d+', lambda m: m.group(1) + 'frames = %d' % budget, t, count=1)
    open(p, 'w').write(t)
    print(name, est, budget)
