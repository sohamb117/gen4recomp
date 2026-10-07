"""dp_dchain_pass.py NAME=FRAMES ...: mark chained D/P passes proven (status line gone, estimate = measured frames,\n[run] frames = 1.5x, or the old bound when larger)."""
import re, sys, math
for arg in sys.argv[1:]:
    name, est = arg.split('=')
    p = 'tests/e2e/diamond/%s/milestone.toml' % name
    t = open(p).read()
    t = re.sub(r'^status = "planned"\n', '', t, flags=re.M)
    t = re.sub(r'^estimate = \d+', 'estimate = %s' % est, t, count=1, flags=re.M)
    budget = int(math.ceil(int(est) * 1.5 / 100.0) * 100)
    old = re.search(r'\[run\]\n(?:[^\[]*?\n)?frames = (\d+)', t)
    if old and int(old.group(1)) > budget:
        budget = int(old.group(1))  # a lab start meets the trainers a chain beat earlier: keep its larger bound
    t = re.sub(r'(\[run\]\n(?:[^\[]*?\n)?)frames = \d+', lambda m: m.group(1) + 'frames = %d' % budget, t, count=1)
    open(p, 'w').write(t)
    print(name, est, budget)
