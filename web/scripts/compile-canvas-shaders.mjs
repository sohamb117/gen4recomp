// Build-time translation only: generated browser code never evals GLSL or JS.
import GLSL from "../integrations/gen123-canvas/node_modules/glsl-transpiler/index.js";
import { readFileSync, writeFileSync } from "node:fs";
import { resolve } from "node:path";
const dir = resolve(import.meta.dirname, "../../build/gen123-canvas");
const sources = JSON.parse(
  readFileSync(resolve(dir, "shader-sources.json"), "utf8"),
);
const normalize = (s) =>
  s
    .replace(/\/\*[\s\S]*?\*\//g, "")
    .replace(/\/\/[^\n]*/g, "")
    .replace(/\s+/g, "");
const done = new Map(),
  failed = [];
for (const { source, path } of sources) {
  const key = normalize(source);
  if (done.has(key)) continue;
  try {
    const compile = GLSL({
      optimize: true,
      uniform: (n) => `uniforms[${JSON.stringify(n)}]`,
      includes: { texture: false, textureSize: false },
    });
    const glsl = source
      .replace(/\bextern\b/g, "uniform")
      .replace(/\bImage\b/g, "sampler2D")
      .replace(/\bnumber\b/g, "float")
      .replace(/\bTexel\b/g, "texture2D")
      .replace(/\bdiscard\s*;/g, "return vec4(0.0);");
    const js = compile(glsl);
    // This is a build-time syntax check, never shipped runtime evaluation.
    new Function("uniforms", "texture", "textureSize", js + "\nreturn effect;");
    done.set(key, { js, path });
  } catch (e) {
    failed.push({ path, error: e.message });
  }
}
writeFileSync(
  resolve(dir, "shaders.js"),
  "/* Generated from pinned GPL game shader sources using MIT glsl-transpiler. */\nwindow.canvasShaderFactories = new Map([\n" +
    [...done]
      .map(
        ([key, { js }]) =>
          `[${JSON.stringify(key)}, function(uniforms, texture, textureSize){\n${js}\nreturn effect;}]`,
      )
      .join(",\n") +
    "\n]);\n",
);
writeFileSync(
  resolve(dir, "canvas_shaders.lua"),
  "return {\n" +
    [...done.keys()].map((key) => `[${JSON.stringify(key)}]=true,`).join("\n") +
    "\n}\n",
);
writeFileSync(
  resolve(dir, "shader-report.json"),
  JSON.stringify(
    { compiled: [...done].map(([key, { path }]) => ({ key, path })), failed },
    null,
    2,
  ),
);
console.log(
  `Canvas shaders: ${done.size} translated, ${failed.length} unsupported`,
  failed,
);
