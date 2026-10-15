"""Fixtures the macOS feature matrix authors itself (no ROM data): a Delta
skin, the example mod as a zip, a local release server for the updater, and
small PNGs.
"""
import hashlib
import http.server
import json
import os
import struct
import threading
import zipfile
import zlib


def png(path, w, h, pixel):
    """Write an RGBA PNG; pixel(x, y) -> (r, g, b, a)."""
    rows = bytearray()
    for y in range(h):
        rows.append(0)
        for x in range(w):
            rows += bytes(pixel(x, y))

    def chunk(kind, data):
        c = struct.pack('>I', len(data)) + kind + data
        return c + struct.pack('>I', zlib.crc32(kind + data) & 0xFFFFFFFF)

    with open(path, 'wb') as f:
        f.write(b'\x89PNG\r\n\x1a\n')
        f.write(chunk(b'IHDR', struct.pack('>IIBBBBB', w, h, 8, 6, 0, 0, 0)))
        f.write(chunk(b'IDAT', zlib.compress(bytes(rows), 9)))
        f.write(chunk(b'IEND', b''))


# ---- Delta skin -------------------------------------------------------------

def _skin_rep(w, h, portrait, gba=False):
    """Items and screens for one orientation, in mapping points; a GBA skin
    has one 240x160 screen and no X, Y or touch screen."""
    if portrait:
        top = {'x': 0, 'y': 40, 'width': w, 'height': w * 0.75}
        bot = {'x': 0, 'y': 40 + w * 0.75, 'width': w, 'height': w * 0.75}
        cy = h - 150
        dpad = {'x': 20, 'y': cy - 60, 'width': 120, 'height': 120}
        btn = lambda dx, dy: {'x': w - 100 + dx, 'y': cy + dy - 25, 'width': 50, 'height': 50}
        start = {'x': w / 2 - 60, 'y': h - 50, 'width': 50, 'height': 30}
        select = {'x': w / 2 + 10, 'y': h - 50, 'width': 50, 'height': 30}
        menu = {'x': w / 2 - 25, 'y': 2, 'width': 50, 'height': 30}
        l = {'x': 0, 'y': 2, 'width': 80, 'height': 32}
        r = {'x': w - 80, 'y': 2, 'width': 80, 'height': 32}
    else:
        sw = h * 4 / 3 / 2  # each screen half the height, 4:3
        top = {'x': w / 2 - sw / 2, 'y': 0, 'width': sw, 'height': h / 2 * 0.98}
        bot = {'x': w / 2 - sw / 2, 'y': h / 2, 'width': sw, 'height': h / 2 * 0.98}
        cy = h / 2 + 40
        dpad = {'x': 30, 'y': cy - 60, 'width': 120, 'height': 120}
        btn = lambda dx, dy: {'x': w - 110 + dx, 'y': cy + dy - 25, 'width': 50, 'height': 50}
        start = {'x': w - 120, 'y': h - 40, 'width': 50, 'height': 30}
        select = {'x': 30, 'y': h - 40, 'width': 50, 'height': 30}
        menu = {'x': w - 60, 'y': h - 40, 'width': 50, 'height': 30}
        l = {'x': 0, 'y': 0, 'width': 90, 'height': 40}
        r = {'x': w - 90, 'y': 0, 'width': 90, 'height': 40}
    items = [
        {'inputs': {'up': 'up', 'down': 'down', 'left': 'left', 'right': 'right'}, 'frame': dpad,
         'extendedEdges': {'top': 10, 'bottom': 10, 'left': 10, 'right': 10}},
        {'inputs': ['a'], 'frame': btn(45, 0)},
        {'inputs': ['b'], 'frame': btn(0, 45)},
        {'inputs': ['x'], 'frame': btn(0, -45)},
        {'inputs': ['y'], 'frame': btn(-45, 0)},
        {'inputs': ['start'], 'frame': start},
        {'inputs': ['select'], 'frame': select},
        {'inputs': ['menu'], 'frame': menu},
        {'inputs': ['l'], 'frame': l},
        {'inputs': ['quickSave'], 'frame': {'x': l['x'], 'y': l['y'] + 50, 'width': 60, 'height': 30}},
        {'inputs': ['fastForward'], 'frame': {'x': r['x'] + r['width'] - 60, 'y': r['y'] + 50, 'width': 60, 'height': 30}},
        {'inputs': {'x': 'touchScreenX', 'y': 'touchScreenY'}, 'frame': bot},
        {'inputs': ['r'], 'frame': r},
    ]
    screens = [{'inputFrame': {'x': 0, 'y': 0, 'width': 256, 'height': 192}, 'outputFrame': top},
               {'inputFrame': {'x': 0, 'y': 192, 'width': 256, 'height': 192}, 'outputFrame': bot}]
    if gba:
        items = [it for it in items if it['inputs'] not in (['x'], ['y']) and 'touchScreenX' not in str(it['inputs'])]
        sh = min(h * 0.5 if not portrait else h, w * 2 / 3)  # 3:2
        out = {'x': w / 2 - sh * 0.75, 'y': 40 if portrait else 0, 'width': sh * 1.5, 'height': sh}
        screens = [{'inputFrame': {'x': 0, 'y': 0, 'width': 240, 'height': 160}, 'outputFrame': out}]
    return items, screens


def _skin_art(path, w, h, items, screens):
    rects = [(it['frame'], (240, 80, 200, 255)) for it in items if isinstance(it['inputs'], list)]
    rects.append((items[0]['frame'], (250, 220, 90, 255)))  # the d-pad
    holes = [s['outputFrame'] for s in screens]

    def inside(r, x, y):
        return r['x'] <= x < r['x'] + r['width'] and r['y'] <= y < r['y'] + r['height']

    def pixel(x, y):
        for r in holes:
            if inside(r, x, y):
                return (0, 0, 0, 0)
        for r, c in rects:
            if inside(r, x, y):
                edge = min(x - r['x'], y - r['y'], r['x'] + r['width'] - 1 - x, r['y'] + r['height'] - 1 - y)
                return c if edge < 4 else (60, 30, 70, 255)
        stripe = ((x + y) // 24) % 2
        return (20, 110 + 20 * stripe, 120, 255)

    png(path, int(w), int(h), pixel)


def deltaskin(path, name='nativeplat Test Skin', gba=False):
    """A hand-made DS (or GBA) skin: iphone/edgeToEdge portrait (390x844)
    and landscape (844x390), PNG art drawn here."""
    reps = {}
    art = {}
    for orient, (w, h) in (('portrait', (390, 844)), ('landscape', (844, 390))):
        items, screens = _skin_rep(w, h, orient == 'portrait', gba)
        fn = 'np_%s.png' % orient
        art[fn] = (w, h, items, screens)
        reps[orient] = {'assets': {'medium': fn}, 'items': items, 'screens': screens,
                        'mappingSize': {'width': w, 'height': h}, 'translucent': False}
    info = {'name': name, 'identifier': 'org.nativeplat.testskin' + ('.gba' if gba else ''),
            'gameTypeIdentifier': 'com.rileytestut.delta.game.' + ('gba' if gba else 'ds'), 'debug': False,
            'representations': {'iphone': {'edgeToEdge': reps}}}
    tmp = path + '.d'
    os.makedirs(tmp, exist_ok=True)
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        z.writestr('info.json', json.dumps(info, indent=1))
        for fn, (w, h, items, screens) in art.items():
            p = os.path.join(tmp, fn)
            _skin_art(p, w, h, items, screens)
            z.write(p, fn)
    return path


# ---- mod package ------------------------------------------------------------

def mod_zip(src_dir, path):
    """The example package as a zip holding the one package folder."""
    base = os.path.basename(src_dir.rstrip('/'))
    with zipfile.ZipFile(path, 'w', zipfile.ZIP_DEFLATED) as z:
        for dirpath, _, files in os.walk(src_dir):
            for f in files:
                full = os.path.join(dirpath, f)
                z.write(full, os.path.join(base, os.path.relpath(full, src_dir)))
    return path


# ---- local release server ---------------------------------------------------

class ReleaseServer:
    """GitHub's /repos/<repo>/releases/latest for a tag, a macOS zip and
    sha256sums.txt, on 127.0.0.1."""

    def __init__(self, repo, tag, payload, bad_digest=False):
        self.repo, self.tag, self.payload = repo, tag, payload
        self.bad = bad_digest
        handler = self._handler()
        self.httpd = http.server.ThreadingHTTPServer(('127.0.0.1', 0), handler)
        self.port = self.httpd.server_address[1]
        self.api = 'http://127.0.0.1:%d' % self.port
        self.thread = threading.Thread(target=self.httpd.serve_forever, daemon=True)

    def _handler(self):
        srv = self
        zipname = 'nativeplat-%s-macos-arm64.zip' % srv.tag

        class H(http.server.BaseHTTPRequestHandler):
            def log_message(self, *a):
                pass

            def send(self, body, ctype):
                self.send_response(200)
                self.send_header('Content-Type', ctype)
                self.send_header('Content-Length', str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def do_GET(self):
                digest = hashlib.sha256(srv.payload).hexdigest()
                if srv.bad:
                    digest = '0' * 64
                if self.path == '/repos/%s/releases/latest' % srv.repo:
                    rel = {'tag_name': srv.tag, 'name': 'nativeplat ' + srv.tag,
                           'body': 'Test release served by tests/mac/fixtures.py',
                           'assets': [
                               {'name': zipname, 'size': len(srv.payload),
                                'browser_download_url': srv.api + '/dl/' + zipname},
                               {'name': 'sha256sums.txt', 'size': 0,
                                'browser_download_url': srv.api + '/dl/sha256sums.txt'}]}
                    self.send(json.dumps(rel).encode(), 'application/json')
                elif self.path == '/dl/' + zipname:
                    self.send(srv.payload, 'application/zip')
                elif self.path == '/dl/sha256sums.txt':
                    self.send(('%s  %s\n' % (digest, zipname)).encode(), 'text/plain')
                else:
                    self.send_error(404)

        return H

    def __enter__(self):
        self.thread.start()
        return self

    def __exit__(self, *a):
        self.httpd.shutdown()
