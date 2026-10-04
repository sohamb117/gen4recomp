import { execFileSync } from "node:child_process";
import { mkdirSync } from "node:fs";
import { resolve } from "node:path";
const root = resolve(import.meta.dirname, "../.."),
  out = resolve(root, "build/web-tests"),
  wabt = resolve(root, ".cache/toolchains/wabt/bin");
mkdirSync(out, { recursive: true });
const run = (exe, args) => execFileSync(exe, args, { stdio: "inherit" });
run(resolve(root, ".cache/toolchains/wasi-sdk/bin/clang"), [
  "--target=wasm32-wasip1",
  "-std=c11",
  "-O2",
  "-g",
  `-I${root}/core/include`,
  resolve(root, "core/tests/mock_guest.c"),
  "-Wl,--global-base=184549376",
  "-Wl,--no-stack-first",
  "-Wl,--initial-memory=268435456",
  "-Wl,--max-memory=268435456",
  "-Wl,-z,stack-size=1048576",
  "-Wl,--export=__stack_pointer",
  "-Wl,--export=malloc",
  "-Wl,--export=free",
  "-lm",
  "-o",
  `${out}/mock.wasm`,
]);
run(`${wabt}/wasm2wat`, [`${out}/mock.wasm`, "-o", `${out}/mock.wat`]);
run("python3", [
  resolve(root, "web/scripts/prepare-wat.py"),
  `${out}/mock.wat`,
]);
run(`${wabt}/wat2wasm`, [
  `${out}/mock.wat`,
  "--debug-names",
  "-o",
  `${out}/patched.wasm`,
]);
run(process.execPath, [
  resolve(root, "web/node_modules/binaryen/bin/wasm-opt"),
  `${out}/patched.wasm`,
  "--asyncify",
  "--pass-arg=asyncify-imports@np_host.vblank,np_host.fiber_switch",
  "-o",
  `${out}/browser.wasm`,
]);
