/* Experimental Canvas 2D backend for the isolated LÖVE adapter. No WebGL calls. */
class CanvasLoveRenderer {
  constructor(canvas, fs) {
    this.canvas = canvas;
    this.fs = fs;
    this.resources = new Map([[0, this.surface(canvas)]]);
    this.shaders = new Map();
    this.state = {
      target: 0,
      color: [1, 1, 1, 1],
      shader: 0,
      blend: "alpha",
      clip: null,
      width: 1,
    };
    this.frames = 0;
    this.frameMs = [];
    this.started = performance.now();
  }
  surface(canvas) {
    const ctx = canvas.getContext("2d", { willReadFrequently: true });
    if (!ctx) throw Error("Canvas 2D unavailable");
    return { canvas, ctx, version: 0, variants: new Map() };
  }
  make(w, h) {
    const c = document.createElement("canvas");
    c.width = w;
    c.height = h;
    return this.surface(c);
  }
  batch(commands) {
    const start = performance.now();
    this.lastCommands = commands;
    for (const [op, ...a] of commands) {
      switch (op) {
        case "canvas":
          this.resources.set(a[0], this.make(a[1], a[2]));
          break;
        case "image": {
          const [id, w, h, path] = a,
            r = this.make(w, h),
            data = this.fs.readFile(path);
          r.ctx.putImageData(
            new ImageData(new Uint8ClampedArray(data), w, h),
            0,
            0,
          );
          this.fs.unlink(path);
          this.resources.set(id, r);
          break;
        }
        case "release":
          this.resources.delete(a[0]);
          this.shaders.delete(a[0]);
          break;
        case "filter": {
          const r = this.resources.get(a[0]);
          if (r) r.linear = a[1] === "linear";
          break;
        }
        case "wrap": {
          const r = this.resources.get(a[0]);
          if (r) r.wrap = a.slice(1);
          break;
        }
        case "batch":
          this.resources.set(a[0], { texture: a[1], sprites: [] });
          break;
        case "batchSet":
          this.resources.get(a[0]).sprites[a[1] - 1] = {
            quad: a[2],
            matrix: a[3],
            color: a[4],
          };
          break;
        case "batchClear":
          this.resources.get(a[0]).sprites = [];
          break;
        case "batchTexture":
          this.resources.get(a[0]).texture = a[1];
          break;
        case "batchRange":
          this.resources.get(a[0]).range = [a[1] - 1, a[2]];
          break;
        case "newShader":
          this.shaders.set(a[0], {
            kind: a[1],
            keyed: a[2],
            source: a[3],
            uniforms: {},
          });
          break;
        case "uniform":
          this.shaders.get(a[0]).uniforms[a[1]] =
            a[2].length === 1 ? a[2][0] : a[2];
          break;
        case "shader":
          this.state.shader = a[0];
          break;
        case "color":
          this.state.color = a[0];
          break;
        case "target":
          this.state.target = a[0];
          break;
        case "blend":
          this.state.blend = a[0];
          break;
        case "scissor":
          this.state.clip = a[0] || null;
          break;
        case "lineWidth":
          this.state.width = a[0];
          break;
        case "resize":
          this.canvas.width = a[0];
          this.canvas.height = a[1];
          this.dirty(this.resources.get(0));
          break;
        case "draw":
          this.draw(...a);
          break;
        case "shape":
          this.shape(...a);
          break;
        case "clear":
          this.clear(a[0]);
          break;
        case "readback": {
          const [id, x, y, w, h, path] = a;
          const r = this.resources.get(id);
          this.upload(r);
          this.fs.writeFile(path, r.ctx.getImageData(x, y, w, h).data);
          break;
        }
        case "present":
          this.frames++;
          break;
        default:
          throw Error(`Canvas renderer: unknown command ${op}`);
      }
    }
    this.frameMs.push(performance.now() - start);
    if (this.frameMs.length > 12000) this.frameMs.shift();
  }
  pixels(r) {
    r.pixels ||= r.ctx.getImageData(0, 0, r.canvas.width, r.canvas.height).data;
    return r.pixels;
  }
  upload(r) {
    if (r.needsUpload) {
      r.ctx.putImageData(
        new ImageData(
          new Uint8ClampedArray(r.pixels),
          r.canvas.width,
          r.canvas.height,
        ),
        0,
        0,
      );
      r.needsUpload = false;
    }
  }
  dirty(r) {
    r.version++;
    r.variants.clear();
    r.pixels = null;
  }
  withTarget(matrix, fn, blend = true) {
    const r = this.resources.get(this.state.target),
      c = r.ctx;
    this.upload(r);
    c.save();
    c.resetTransform();
    if (this.state.clip) {
      c.beginPath();
      c.rect(...this.state.clip);
      c.clip();
    }
    if (matrix) c.setTransform(...matrix);
    c.imageSmoothingEnabled = false;
    c.lineWidth = this.state.width;
    if (blend) {
      const modes = {
        alpha: "source-over",
        replace: "copy",
        add: "lighter",
        multiply: "multiply",
        screen: "screen",
        darken: "darken",
        lighten: "lighten",
      };
      if (!modes[this.state.blend])
        throw Error(`Canvas renderer: unsupported blend ${this.state.blend}`);
      c.globalCompositeOperation = modes[this.state.blend];
    }
    fn(c);
    c.restore();
    this.dirty(r);
  }
  clear(color) {
    this.withTarget(
      null,
      (c) => {
        c.clearRect(0, 0, c.canvas.width, c.canvas.height);
        if (color[3]) {
          c.fillStyle = this.css(color);
          c.fillRect(0, 0, c.canvas.width, c.canvas.height);
        }
      },
      false,
    );
  }
  css(c) {
    return `rgba(${c[0] * 255},${c[1] * 255},${c[2] * 255},${c[3] ?? 1})`;
  }
  genericTexture(r, color, matrix) {
    const shader = this.shaders.get(this.state.shader);
    const normalize = (s) =>
      s
        .replace(/\/\*[\s\S]*?\*\//g, "")
        .replace(/\/\/[^\n]*/g, "")
        .replace(/\s+/g, "");
    shader.factory ||= window.canvasShaderFactories.get(
      normalize(shader.source),
    );
    if (!shader.factory)
      throw Error("Canvas shader was not compiled at build time");
    const dependencies = [];
    const uniform = {};
    for (const [name, value] of Object.entries(shader.uniforms)) {
      if (value?.texture) {
        const image = this.resources.get(value.texture);
        uniform[name] = image;
        dependencies.push([value.texture, image.version]);
      } else uniform[name] = value;
    }
    const key = JSON.stringify([
      shader.source,
      shader.uniforms,
      color,
      matrix,
      dependencies,
    ]);
    let result = r.variants.get(key);
    if (!result) {
      const sample = (image, uv) => {
        const w = image.canvas.width,
          h = image.canvas.height;
        let x = Math.floor(uv[0] * w),
          y = Math.floor(uv[1] * h);
        x =
          image.wrap?.[0] === "repeat"
            ? ((x % w) + w) % w
            : Math.max(0, Math.min(w - 1, x));
        y =
          image.wrap?.[1] === "repeat"
            ? ((y % h) + h) % h
            : Math.max(0, Math.min(h - 1, y));
        const p = this.pixels(image),
          i = (y * w + x) * 4;
        return [p[i] / 255, p[i + 1] / 255, p[i + 2] / 255, p[i + 3] / 255];
      };
      const effect = shader.factory(uniform, sample, (image) => [
        image.canvas.width,
        image.canvas.height,
      ]);
      const w = r.canvas.width,
        h = r.canvas.height,
        out = new Uint8ClampedArray(w * h * 4);
      for (let y = 0; y < h; y++)
        for (let x = 0; x < w; x++) {
          const sc = [
            matrix[0] * (x + 0.5) + matrix[2] * (y + 0.5) + matrix[4],
            matrix[1] * (x + 0.5) + matrix[3] * (y + 0.5) + matrix[5],
          ];
          const rgba = effect(color, r, [(x + 0.5) / w, (y + 0.5) / h], sc),
            i = (y * w + x) * 4;
          for (let k = 0; k < 4; k++) out[i + k] = rgba[k] * 255;
        }
      result = this.make(w, h).canvas;
      result.getContext("2d").putImageData(new ImageData(out, w, h), 0, 0);
      if (r.variants.size >= 32)
        r.variants.delete(r.variants.keys().next().value);
      r.variants.set(key, result);
    }
    return { image: result, alpha: 1 };
  }
  texture(r, color, matrix) {
    this.upload(r);
    const shader = this.shaders.get(this.state.shader);
    if (shader?.kind === "generic")
      return this.genericTexture(r, color, matrix);
    const tint = shader?.kind === "palette" ? [1, 1, 1, 1] : color;
    if (!shader && tint[0] === 1 && tint[1] === 1 && tint[2] === 1)
      return { image: r.canvas, alpha: tint[3] };
    const key = JSON.stringify([shader, tint.slice(0, 3)]);
    let image = r.variants.get(key);
    if (!image) {
      const { width: w, height: h } = r.canvas;
      r.pixels ||= r.ctx.getImageData(0, 0, w, h).data;
      const data = new Uint8ClampedArray(r.pixels),
        u = shader?.uniforms;
      for (let i = 0; i < data.length; i += 4) {
        let red = data[i] / 255,
          green = data[i + 1] / 255,
          blue = data[i + 2] / 255;
        if (shader?.kind === "palette" || shader?.kind === "gbcPalette") {
          const shade =
            shader.kind === "palette"
              ? red > 0.83
                ? 0
                : red > 0.5
                  ? 1
                  : red > 0.17
                    ? 2
                    : 3
              : Math.floor((1 - red) * 3 + 0.5);
          const p = u[(shader.kind === "palette" ? "c" : "pal") + shade] || [
            0, 0, 0,
          ];
          if (
            shader.keyed &&
            (shader.kind === "gbcPalette"
              ? shade === 0
              : red > 0.83 && green > 0.83 && blue > 0.83)
          )
            data[i + 3] = 0;
          [red, green, blue] = p;
        } else if (
          shader?.kind === "colorKey" &&
          red * tint[0] > 0.83 &&
          green * tint[1] > 0.83 &&
          blue * tint[2] > 0.83
        )
          data[i + 3] = 0;
        data[i] = red * tint[0] * 255;
        data[i + 1] = green * tint[1] * 255;
        data[i + 2] = blue * tint[2] * 255;
      }
      const target = this.make(w, h);
      target.ctx.putImageData(new ImageData(data, w, h), 0, 0);
      image = target.canvas;
      if (r.variants.size >= 32)
        r.variants.delete(r.variants.keys().next().value);
      r.variants.set(key, image);
    }
    return { image, alpha: tint[3] };
  }
  draw(id, quad, matrix, color = this.state.color) {
    const shader = this.shaders.get(this.state.shader);
    if (shader?.kind.startsWith("gba"))
      return gbaDraw(this, shader, quad, matrix);
    const r = this.resources.get(id);
    if (!r) throw Error(`Missing Canvas resource ${id}`);
    if (r.sprites) {
      const begin = r.range?.[0] || 0,
        end = r.range?.[1] ? begin + r.range[1] : r.sprites.length;
      const parent = new DOMMatrix(matrix);
      for (let i = begin; i < end; i++) {
        const s = r.sprites[i];
        if (!s) continue;
        const m = parent.multiply(new DOMMatrix(s.matrix));
        this.draw(
          r.texture,
          s.quad,
          [m.a, m.b, m.c, m.d, m.e, m.f],
          color.map((v, j) => v * s.color[j]),
        );
      }
      return;
    }
    const tex = this.texture(r, color, matrix),
      img = tex.image;
    this.withTarget(matrix, (c) => {
      c.globalAlpha = tex.alpha;
      c.imageSmoothingEnabled = !!r.linear;
      if (quad) {
        const [x, y, w, h] = quad;
        if (
          r.wrap?.includes("repeat") &&
          (x < 0 || y < 0 || x + w > img.width || y + h > img.height)
        ) {
          c.save();
          c.beginPath();
          c.rect(0, 0, w, h);
          c.clip();
          c.translate(-x, -y);
          c.fillStyle = c.createPattern(img, "repeat");
          c.fillRect(x, y, w, h);
          c.restore();
        } else c.drawImage(img, x, y, w, h, 0, 0, w, h);
      } else c.drawImage(img, 0, 0);
    });
  }
  shape(name, args, matrix) {
    this.withTarget(matrix, (c) => {
      c.fillStyle = c.strokeStyle = this.css(this.state.color);
      c.beginPath();
      let mode = args[0];
      if (name === "rectangle") c.rect(args[1], args[2], args[3], args[4]);
      else if (name === "circle")
        c.arc(args[1], args[2], args[3], 0, Math.PI * 2);
      else if (name === "ellipse")
        c.ellipse(args[1], args[2], args[3], args[4], 0, 0, Math.PI * 2);
      else if (name === "arc") {
        let a = args.slice(1);
        if (typeof a[0] === "string") a.shift();
        c.arc(a[0], a[1], a[2], a[3], a[4]);
      } else if (name === "line" || name === "polygon") {
        if (name === "line") {
          mode = "line";
        } else args = args.slice(1);
        if (Array.isArray(args[0])) args = args[0];
        for (let i = 0; i < args.length; i += 2)
          i ? c.lineTo(args[i], args[i + 1]) : c.moveTo(args[i], args[i + 1]);
        if (name === "polygon") c.closePath();
      } else throw Error(`Canvas renderer: unsupported shape ${name}`);
      if (mode === "fill") c.fill();
      else c.stroke();
    });
  }
}
window.CanvasLoveRenderer = CanvasLoveRenderer;
