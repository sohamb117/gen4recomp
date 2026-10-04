"""Patch a COPY of WABT's linear WAT: ARM division and browser host exports."""
import re, sys
from pathlib import Path
p = Path(sys.argv[1]); text = p.read_text()
ops = sorted(set(re.findall(r'\b(i(?:32|64)\.(?:div|rem)_[su])\b', text)))
# Replace only instruction tokens. Helpers are appended after rewriting.
for op in ops:
 text = re.sub(r'(?m)^(\s*)' + re.escape(op) + r'(\s*)$', r'\1call $np_' + op.replace('.', '_') + r'\2', text)
helpers = []
for op in ops:
 ty, inst = op.split('.'); bits = int(ty[1:]); rem = inst.startswith('rem'); signed = inst.endswith('_s')
 zero = f'({ty}.const 0)' if rem and bits == 32 else '(local.get 0)'
 normal = f'({op} (local.get 0) (local.get 1))'
 if signed and not rem:
  normal = f'(if (result {ty}) (i32.and ({ty}.eq (local.get 0) ({ty}.const {-2**(bits-1)})) ({ty}.eq (local.get 1) ({ty}.const -1))) (then (local.get 0)) (else {normal}))'
 helpers.append(f'(func $np_{op.replace(".", "_")} (param {ty} {ty}) (result {ty}) (if (result {ty}) ({ty}.eqz (local.get 1)) (then {zero}) (else {normal})))')
for name in ['__stack_pointer', 'malloc', 'free']:
 if f'(export "{name}"' in text: continue
 kind = 'global' if name == '__stack_pointer' else 'func'
 if not re.search(r'\(' + kind + r' \$' + name + r'[\s)]', text):
  raise SystemExit(f'Missing {name}; keep names in the guest build')
 helpers.append(f'(export "{name}" ({kind} ${name}))')
end = text.rfind(')'); p.write_text(text[:end] + '\n' + '\n'.join(helpers) + '\n)\n')
print(f'ARM division: {len(ops)} instruction kinds; browser exports added')
