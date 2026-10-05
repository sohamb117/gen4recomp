/* CPU equivalents of Gen1Recomp's three GBA PPU shaders. No GPU API. */
function gbaDraw(renderer, shader, quad, matrix) {
  const r = renderer.resources.get(renderer.state.target),
    u = shader.uniforms;
  const w = r.canvas.width,
    h = r.canvas.height;
  const output = renderer.pixels(r).slice();
  const texture = (name) => renderer.resources.get(u[name]?.texture);
  const data = (name) => {
    const t = texture(name);
    return t ? renderer.pixels(t) : null;
  };
  const pal = data("palTex");
  const clamp = (v, max) => Math.max(0, Math.min(max - 1, v));
  const mod = (v, n) => ((v % n) + n) % n;
  const sample = (bytes, tw, th, x, y) =>
    4 * (clamp(Math.floor(y), th) * tw + clamp(Math.floor(x), tw));
  const clip = renderer.state.clip || [0, 0, w, h];
  let x0 = Math.max(0, Math.ceil(clip[0])),
    y0 = Math.max(0, Math.ceil(clip[1]));
  let x1 = Math.min(w, Math.ceil(clip[0] + clip[2])),
    y1 = Math.min(h, Math.ceil(clip[1] + clip[3]));
  // PPU uses a 1x1 white texture expanded to integer rectangles.
  if (matrix[1] || matrix[2])
    throw Error("GBA PPU surface must use axis-aligned transforms");
  x0 = Math.max(x0, Math.floor(matrix[4]));
  y0 = Math.max(y0, Math.floor(matrix[5]));
  x1 = Math.min(x1, Math.ceil(matrix[4] + matrix[0]));
  y1 = Math.min(y1, Math.ceil(matrix[5] + matrix[3]));
  if (shader.kind === "gbaBG") {
    const idx = data("idxTex"),
      line = data("lineTex"),
      [tw, th] = u.texSize,
      m = u.mat,
      ref = u.ref,
      ofs = u.ofs;
    for (let y = y0; y < y1; y++)
      for (let x = x0; x < x1; x++) {
        let tx, ty;
        if (u.affine > 0.5) {
          tx = Math.floor((ref[0] + m[0] * x + m[1] * y) / 256);
          ty = Math.floor((ref[1] + m[2] * x + m[3] * y) / 256);
          if (u.wrap > 0.5) {
            tx = mod(tx, tw);
            ty = mod(ty, th);
          } else if (tx < 0 || ty < 0 || tx >= tw || ty >= th) continue;
        } else {
          let dx = ofs[0],
            dy = ofs[1];
          if (u.lineMode > 0.5) {
            const val = line[y * 4] + line[y * 4 + 1] * 256;
            if (u.lineMode < 1.5) dx = val;
            else dy = val;
          }
          tx = mod(x + dx, tw);
          ty = mod(y + dy, th);
        }
        const index = idx[sample(idx, tw, th, tx, ty)],
          o = (y * w + x) * 4;
        if (u.bpp8 > 0.5 ? index === 0 : index % 16 === 0) {
          output.fill(0, o, o + 4);
          continue;
        }
        output[o] = pal[index * 4];
        output[o + 1] = pal[index * 4 + 1];
        output[o + 2] = pal[index * 4 + 2];
        output[o + 3] = 255;
      }
  } else if (shader.kind === "gbaOBJ") {
    const sheet = data("sheet"),
      [tw, th] = u.sheetSize,
      [sw, sh] = u.sprSize,
      box = u.boxPos,
      size = u.boxSize,
      m = u.mat,
      flip = u.flip,
      origin = u.frameOrigin;
    for (let y = y0; y < y1; y++)
      for (let x = x0; x < x1; x++) {
        let sx = x - box[0],
          sy = y - box[1];
        if (u.affine > 0.5) {
          const dx = sx - Math.floor(size[0] / 2),
            dy = sy - Math.floor(size[1] / 2);
          sx = Math.floor((m[0] * dx + m[1] * dy) / 256) + Math.floor(sw / 2);
          sy = Math.floor((m[2] * dx + m[3] * dy) / 256) + Math.floor(sh / 2);
          if (sx < 0 || sy < 0 || sx >= sw || sy >= sh) continue;
        } else {
          if (flip[0] > 0.5) sx = sw - 1 - sx;
          if (flip[1] > 0.5) sy = sh - 1 - sy;
        }
        const idx =
          sheet[sample(sheet, tw, th, origin[0] + sx, origin[1] + sy)];
        if (!idx) continue;
        const pi = (256 + (u.bpp8 > 0.5 ? idx : u.palBase + idx)) * 4,
          o = (y * w + x) * 4;
        output[o] = pal[pi];
        output[o + 1] = pal[pi + 1];
        output[o + 2] = pal[pi + 2];
        output[o + 3] = Math.round(u.tag * 255);
      }
  } else {
    const bg = [data("bg0"), data("bg1"), data("bg2"), data("bg3")],
      obj = data("obj"),
      objWin = data("objWin");
    const inside = (x, y, v) => x >= v[0] && x < v[1] && y >= v[2] && y < v[3];
    const layers = [0, 1, 2, 3]
      .filter((i) => u.bgOn[i] > 0.5)
      .sort((a, b) => u.bgPrio[a] - u.bgPrio[b] || a - b);
    for (let y = y0; y < y1; y++)
      for (let x = x0; x < x1; x++) {
        const o = (y * w + x) * 4;
        let mask = 63;
        if (u.winFlags.some((v) => v > 0.5)) {
          mask = u.winMasks[2];
          if (u.winFlags[0] > 0.5 && inside(x, y, u.win0)) mask = u.winMasks[0];
          else if (u.winFlags[1] > 0.5 && inside(x, y, u.win1))
            mask = u.winMasks[1];
          else if (u.winFlags[2] > 0.5 && objWin[o + 3] > 127)
            mask = u.winMasks[3];
        }
        const ov = obj[o + 3],
          op = Math.floor((ov - 16) / 4),
          semi = Math.floor((ov - 16) / 2) % 2;
        const present = u.objOn > 0.5 && ov >= 16 && mask & 16;
        let first = 5,
          second = 5;
        for (const i of layers) {
          if (mask & (1 << i) && bg[i][o + 3] > 127) {
            if (first === 5) first = i;
            else {
              second = i;
              break;
            }
          }
        }
        if (present) {
          if (first === 5 || op <= u.bgPrio[first]) {
            second = first;
            first = 4;
          } else if (second === 5 || op <= u.bgPrio[second]) second = 4;
        }
        const a = first === 5 ? pal : first === 4 ? obj : bg[first],
          b = second === 5 ? pal : second === 4 ? obj : bg[second];
        const ai = first === 5 ? 0 : o,
          bi = second === 5 ? 0 : o;
        const top = u.t1 & (1 << first),
          bottom = u.t2 & (1 << second);
        for (let c = 0; c < 3; c++) {
          let v = Math.round((a[ai + c] * 31) / 255),
            v2 = Math.round((b[bi + c] * 31) / 255);
          if (mask & 32) {
            if (
              ((first === 4 && semi > 0) || (u.bldMode === 1 && top)) &&
              bottom
            )
              v = Math.min(31, Math.floor((v * u.ev[0] + v2 * u.ev[1]) / 16));
            else if (u.bldMode === 2 && top)
              v += Math.floor(((31 - v) * u.ev[2]) / 16);
            else if (u.bldMode === 3 && top)
              v -= Math.floor((v * u.ev[2]) / 16);
          }
          output[o + c] = v * 8 + Math.floor(v / 4);
        }
        output[o + 3] = 255;
      }
  }
  r.pixels = output;
  r.needsUpload = true;
  r.version++;
  r.variants.clear();
}
window.gbaDraw = gbaDraw;
