#!/usr/bin/env python3
"""Build gauge video assets.

OBV1 (MJPEG container, main/media/obv1_format.h; OBV2 is accepted by inspect/verify):

    python3 tools/make_video.py boot  INPUT -o boot.obv            # 360x360 4:2:2 Q75 24 fps
    python3 tools/make_video.py theme INPUT -o rpm.obv --size 240x240 [--poster rpm.png]
    python3 tools/make_video.py inspect FILE.obv
    python3 tools/make_video.py verify  FILE.obv                   # CRCs + every JPEG frame

  boot  -> copy to /sdcard/OBD/BOOT/boot.obv
  theme -> copy to /sdcard/OBD/THEMES/<theme>/<key>.obv or /sdcard/OBD/VIDEO/<key>.obv

Legacy boot animation (delta_varint_rgb565_black, flash bootmedia partition):

    python3 tools/make_video.py legacy INPUT -o bootmedia/slot_a [--raw bootmedia.raw]

  writes boot_block.txt + boot_block.bin; --raw also writes the 4 KB-manifest +
  bin image (tools/make_bootmedia_raw.py layout) for /sdcard/OBD/BOOT/bootmedia.raw.

Firmware self-test pictures (main/media/media_jpeg_selftest.inc):

    python3 tools/make_video.py selftest-data -o main/media/media_jpeg_selftest.inc

INPUT is a directory of images (one per frame, sorted by name), an animated
GIF/PNG/WebP, a still image (needs --duration), or any video ffmpeg can read.

Requirements: Pillow; ffmpeg in PATH only for video-file inputs.
"""

import argparse
import io
import os
import struct
import subprocess
import sys
import tempfile
import zlib

try:
    from PIL import Image
except ImportError:
    print('ERROR: Pillow not installed. Run: pip install Pillow', file=sys.stderr)
    sys.exit(1)

# ---------------------------------------------------------------- OBV1 format

OBV1_MAGIC = b'OBV1'
OBV1_VERSION = 1
OBV2_VERSION = 2
OBV1_HEADER_SIZE = 64
OBV1_INDEX_ENTRY = 8
OBV2_INDEX_ENTRY = 16
OBV1_FRAME_MAX = 48 * 1024
OBV1_MAX_FRAMES = 36000
OBV1_MAX_DIM = 360
OBV1_FLAG_BLOCK_SAFE = 0x01
OBV1_FLAG_LOOP = 0x02
SUB_CODES = {'gray': 0, '444': 1, '422': 2, '420': 3}
SUB_NAMES = {v: k for k, v in SUB_CODES.items()}
PIL_SUBSAMPLING = {'444': 0, '422': 1, '420': 2}
# Header without the trailing header_crc32 (bytes 0x00..0x3B).
HDR_FMT = '<4sHHHHHBBIIIIIIII12s'
assert struct.calcsize(HDR_FMT) == 0x3C

IMAGE_EXTS = ('.png', '.jpg', '.jpeg', '.bmp', '.gif', '.webp', '.tif', '.tiff')


def crc32(data, crc=0):
    return zlib.crc32(data, crc) & 0xFFFFFFFF


def block_safe(w, h, sub):
    """Matches the firmware rule: block decode needs w, h % 8 == 0 and, for
    4:2:0 (16-line MCU), h % 16 == 0."""
    if sub in ('gray', '444'):
        return False   # firmware has no block self-test for these, frame mode
    if w % 8 or h % 8:
        return False
    return not (sub == '420' and h % 16)


def build_obv(frames, w, h, fps, sub, loop):
    n = len(frames)
    if not 1 <= n <= OBV1_MAX_FRAMES:
        raise ValueError('frame count %d outside 1..%d' % (n, OBV1_MAX_FRAMES))
    index = bytearray()
    off = 0
    for f in frames:
        index += struct.pack('<II', off, len(f))
        off += len(f)
    data = b''.join(frames)
    flags = (OBV1_FLAG_BLOCK_SAFE if block_safe(w, h, sub) else 0) | (OBV1_FLAG_LOOP if loop else 0)
    duration_ms = int(round(n * 1000.0 / fps))
    hdr = struct.pack(HDR_FMT, OBV1_MAGIC, OBV1_VERSION, OBV1_HEADER_SIZE, w, h, fps,
                      SUB_CODES[sub], flags, n, OBV1_HEADER_SIZE,
                      OBV1_HEADER_SIZE + OBV1_INDEX_ENTRY * n, len(data),
                      max(len(f) for f in frames), duration_ms, crc32(bytes(index)),
                      crc32(data), b'\0' * 12)
    hdr += struct.pack('<I', crc32(hdr))
    return hdr + bytes(index) + data


def parse_obv(blob):
    """Read OBV1/OBV2 headers and entries; V2 entries append time/duration_us."""
    if len(blob) < OBV1_HEADER_SIZE:
        raise ValueError('truncated header')
    f = struct.unpack(HDR_FMT, blob[:0x3C])
    (magic, ver, hsize, w, h, fps, sub, flags, n, ioff, doff, dsize, maxf, dur,
     icrc, dcrc, _rsv) = f
    if magic != OBV1_MAGIC:
        raise ValueError('bad magic %r' % magic)
    if ver not in (OBV1_VERSION, OBV2_VERSION) or hsize != OBV1_HEADER_SIZE:
        raise ValueError('unsupported version %d / header_size %d' % (ver, hsize))
    if crc32(blob[:0x3C]) != struct.unpack_from('<I', blob, 0x3C)[0]:
        raise ValueError('header CRC mismatch')
    stride = OBV2_INDEX_ENTRY if ver == OBV2_VERSION else OBV1_INDEX_ENTRY
    if ver == OBV2_VERSION and (fps != 0 or struct.unpack_from('<I', blob, 0x30)[0] != 1000000 or blob[0x34:0x3c] != b'\0' * 8):
        raise ValueError('invalid OBV2 timing header')
    info = dict(version=ver, width=w, height=h, fps=fps, subsampling=SUB_NAMES.get(sub, '?%d' % sub),
                flags=flags, frame_count=n, index_offset=ioff, data_offset=doff,
                data_size=dsize, max_frame_size=maxf, duration_ms=dur,
                index_crc32=icrc, data_crc32=dcrc)
    if ioff != OBV1_HEADER_SIZE or doff != ioff + stride * n:
        raise ValueError('bad index/data offsets')
    if not 1 <= n <= OBV1_MAX_FRAMES:
        raise ValueError('invalid frame count')
    if len(blob) < doff + dsize or (ver == OBV2_VERSION and len(blob) != doff + dsize):
        raise ValueError('truncated: file %d B, header says %d B' % (len(blob), doff + dsize))
    index = blob[ioff:doff]
    entries = [struct.unpack_from('<II', index, i * stride) +
               (struct.unpack_from('<II', index, i * stride + 8) if ver == OBV2_VERSION else ())
               for i in range(n)]
    return info, entries, index


# ---------------------------------------------------------------- JPEG checks

def jpeg_sof(frame):
    """Return (sof_marker, width, height, sub_name) from the first SOF segment."""
    if frame[:2] != b'\xff\xd8':
        raise ValueError('missing SOI')
    pos = 2
    while pos + 4 <= len(frame):
        if frame[pos] != 0xFF:
            raise ValueError('bad marker at %d' % pos)
        marker = frame[pos + 1]
        if marker == 0xFF:
            pos += 1
            continue
        seg_len = struct.unpack_from('>H', frame, pos + 2)[0]
        if 0xC0 <= marker <= 0xCF and marker not in (0xC4, 0xC8, 0xCC):
            prec, hh, ww, nc = struct.unpack_from('>BHHB', frame, pos + 4)
            if prec != 8:
                raise ValueError('%d-bit precision' % prec)
            if nc == 1:
                sub = 'gray'
            elif nc == 3:
                hv = frame[pos + 4 + 6 + 1]
                sub = {0x11: '444', 0x21: '422', 0x22: '420'}.get(hv, 'hv%02x' % hv)
                for c in range(1, 3):
                    if frame[pos + 4 + 6 + 3 * c + 1] != 0x11:
                        raise ValueError('unsupported chroma sampling')
            else:
                raise ValueError('%d components' % nc)
            return marker, ww, hh, sub
        if marker == 0xDA:
            break
        pos += 2 + seg_len
    raise ValueError('no SOF before SOS')


# ---------------------------------------------------------------- frame input

def fit_image(im, w, h, mode='contain', shift_up=0):
    if im.mode in ('RGBA', 'LA', 'P'):
        im = im.convert('RGBA')
        bg = Image.new('RGBA', im.size, (0, 0, 0, 255))
        bg.alpha_composite(im)
        im = bg
    im = im.convert('RGB')
    sw, sh = im.size
    s = min(w / sw, h / sh) if mode == 'contain' else max(w / sw, h / sh)
    nw, nh = max(1, int(round(sw * s))), max(1, int(round(sh * s)))
    if (nw, nh) != (sw, sh):
        im = im.resize((nw, nh), Image.LANCZOS)
    if (nw, nh) == (w, h) and not shift_up:
        return im
    canvas = Image.new('RGB', (w, h), (0, 0, 0))
    canvas.paste(im, ((w - nw) // 2, (h - nh) // 2 - shift_up))
    return canvas


def _ffmpeg_frames(path, w, h, fps, mode, duration, tmpdir):
    ar = 'decrease' if mode == 'contain' else 'increase'
    cmd = ['ffmpeg', '-v', 'error', '-y', '-i', path]
    if duration:
        cmd += ['-t', str(duration)]
    cmd += ['-vf', 'fps=%d,scale=%d:%d:force_original_aspect_ratio=%s:flags=lanczos'
            % (fps, w, h, ar), os.path.join(tmpdir, 'f_%05d.png')]
    try:
        r = subprocess.run(cmd, capture_output=True, text=True)
    except FileNotFoundError:
        raise SystemExit('ERROR: %s is not an image; ffmpeg is needed for video input' % path)
    if r.returncode != 0:
        raise SystemExit('ERROR: ffmpeg failed:\n%s' % r.stderr[-800:])
    return sorted(os.path.join(tmpdir, n) for n in os.listdir(tmpdir) if n.endswith('.png'))


def load_frames(src, w, h, fps, mode='contain', shift_up=0, duration=0.0):
    """Yield w x h RGB PIL images at `fps`."""
    if os.path.isdir(src):
        names = sorted(n for n in os.listdir(src) if n.lower().endswith(IMAGE_EXTS))
        if duration:
            names = names[:max(1, int(round(duration * fps)))]
        for n in names:
            with Image.open(os.path.join(src, n)) as im:
                yield fit_image(im, w, h, mode, shift_up)
        return
    try:
        im = Image.open(src)
    except Exception:
        im = None
    if im is not None:
        with im:
            nsrc = getattr(im, 'n_frames', 1)
            if nsrc <= 1:
                if not duration:
                    raise SystemExit('ERROR: still image input needs --duration')
                still = fit_image(im, w, h, mode, shift_up)
                for _ in range(max(1, int(round(duration * fps)))):
                    yield still
                return
            # Animated image: resample the frame timeline to `fps`.
            starts, t = [], 0
            for i in range(nsrc):
                im.seek(i)
                starts.append(t)
                t += im.info.get('duration', 100) or 100
            total = duration * 1000.0 if duration else t
            k, cur, cur_img = 0, -1, None
            while k * 1000.0 / fps < total:
                ts = (k * 1000.0 / fps) % t
                idx = max(i for i in range(nsrc) if starts[i] <= ts)
                if idx != cur:
                    im.seek(idx)
                    cur, cur_img = idx, fit_image(im.copy(), w, h, mode, shift_up)
                yield cur_img
                k += 1
        return
    with tempfile.TemporaryDirectory() as tmp:
        for p in _ffmpeg_frames(src, w, h, fps, mode, duration, tmp):
            with Image.open(p) as fim:
                yield fit_image(fim, w, h, mode, shift_up)


# ---------------------------------------------------------------- OBV encode

def encode_jpeg(im, quality, sub):
    buf = io.BytesIO()
    if sub == 'gray':
        im.convert('L').save(buf, 'JPEG', quality=quality, optimize=True)
    else:
        im.save(buf, 'JPEG', quality=quality, subsampling=PIL_SUBSAMPLING[sub],
                optimize=True, progressive=False)
    return buf.getvalue()


def encode_capped(im, quality, sub, cap, min_quality=30):
    q = quality
    while True:
        data = encode_jpeg(im, q, sub)
        if len(data) <= cap or q <= min_quality:
            return data, q
        q -= 5


def parse_size(text):
    try:
        w, h = (int(v) for v in text.lower().split('x'))
    except ValueError:
        raise argparse.ArgumentTypeError('size must be WxH, e.g. 240x240')
    return w, h


def round8(v):
    return max(8, min(OBV1_MAX_DIM, (v + 7) // 8 * 8))


def make_obv(args, w, h, loop):
    if not (1 <= args.fps <= 60):
        raise SystemExit('ERROR: --fps must be 1..60')
    cap = min(args.max_frame, OBV1_FRAME_MAX)
    frames, first, worst_q, too_big = [], None, args.quality, 0
    for i, im in enumerate(load_frames(args.input, w, h, args.fps, args.fit,
                                       args.shift_up, args.duration)):
        if first is None:
            first = im
        data, q = encode_capped(im, args.quality, args.sub, cap)
        worst_q = min(worst_q, q)
        if len(data) > cap:
            too_big += 1
        frames.append(data)
        if (i + 1) % 50 == 0:
            print('  frame %d: %d B (Q%d)' % (i + 1, len(data), q))
    if not frames:
        raise SystemExit('ERROR: no frames')
    if too_big:
        raise SystemExit('ERROR: %d frame(s) exceed %d B even at Q30; lower --size or --fps'
                         % (too_big, cap))
    blob = build_obv(frames, w, h, args.fps, args.sub, loop)
    with open(args.output, 'wb') as f:
        f.write(blob)
    sizes = [len(f) for f in frames]
    print('%s: %dx%d %s %d fps, %d frames (%.2f s), %d B' % (
        args.output, w, h, args.sub, args.fps, len(frames), len(frames) / args.fps, len(blob)))
    print('  frame avg %d B, max %d B, min Q%d, %.0f KB/s, block_safe=%s' % (
        sum(sizes) // len(sizes), max(sizes), worst_q,
        sum(sizes) * args.fps / len(sizes) / 1024.0, block_safe(w, h, args.sub)))
    return first


def cmd_boot(args):
    w = h = args.size
    if w % 8 or not 8 <= w <= OBV1_MAX_DIM:
        raise SystemExit('ERROR: --size must be a multiple of 8, 8..360')
    make_obv(args, w, h, args.loop)
    return 0


def cmd_theme(args):
    w, h = round8(args.size[0]), round8(args.size[1])
    if (w, h) != tuple(args.size):
        print('  element %dx%d rounded up to %dx%d (centred, edges cropped)'
              % (args.size[0], args.size[1], w, h))
    first = make_obv(args, w, h, not args.no_loop)
    if args.poster:
        # Poster at the element's own size: what the video covers on screen.
        poster = fit_image(first, args.size[0], args.size[1], 'cover')
        poster.save(args.poster)
        print('  poster %s (%dx%d)' % (args.poster, args.size[0], args.size[1]))
    return 0


def timed_ring_bytes(entries, max_frame):
    """Match the firmware's sliding one-second window plus wrap headroom."""
    total = largest = left = 0
    record = lambda e: 8 + ((e[1] + 3) & ~3)
    for n, entry in enumerate(entries):
        total += record(entry)
        while left < n and entry[2] - entries[left][2] >= 1000000:
            total -= record(entries[left])
            left += 1
        largest = max(largest, total)
    return largest + 2 * (8 + ((max_frame + 3) & ~3))


def cmd_inspect(args):
    with open(args.file, 'rb') as f:
        blob = f.read()
    info, entries, _ = parse_obv(blob)
    for k, v in info.items():
        if k == 'flags':
            v = '0x%02x%s%s' % (v, ' BLOCK_SAFE' if v & 1 else '', ' LOOP' if v & 2 else '')
        elif k.endswith('crc32'):
            v = '0x%08x' % v
        print('%-15s %s' % (k, v))
    sizes = [e[1] for e in entries]
    total_us = sum(e[3] for e in entries) if info['version'] == OBV2_VERSION else 0
    fps = info['fps'] if info['version'] == OBV1_VERSION else info['frame_count'] * 1000000 / max(1, total_us)
    if info['version'] == OBV2_VERSION:
        print('%-15s %d B' % ('prebuffer', timed_ring_bytes(entries, info['max_frame_size'])))
    print('%-15s avg %d B, min %d B, max %d B, %.0f KB/s' % (
        'frames', sum(sizes) // len(sizes), min(sizes), max(sizes),
        sum(sizes) * fps / len(sizes) / 1024.0))
    return 0


def cmd_verify(args):
    with open(args.file, 'rb') as f:
        blob = f.read()
    try:
        info, entries, index = parse_obv(blob)
    except ValueError as e:
        print('%s: FAIL %s' % (args.file, e))
        return 1
    errs = []
    w, h, sub = info['width'], info['height'], info['subsampling']
    if w % 8 or h % 8 or not (8 <= w <= OBV1_MAX_DIM and 8 <= h <= OBV1_MAX_DIM):
        errs.append('geometry %dx%d' % (w, h))
    if info['version'] == OBV1_VERSION and not 1 <= info['fps'] <= 60:
        errs.append('fps %d' % info['fps'])
    if info['version'] == OBV2_VERSION and (sub not in ('420', '422') or info['flags'] != (OBV1_FLAG_BLOCK_SAFE if block_safe(w, h, sub) else 0)):
        errs.append('unsupported OBV2 sampling or block-safe flag')
    if not 0 < info['max_frame_size'] <= OBV1_FRAME_MAX:
        errs.append('max_frame_size %d' % info['max_frame_size'])
    if crc32(index) != info['index_crc32']:
        errs.append('index CRC mismatch')
    data = blob[info['data_offset']:info['data_offset'] + info['data_size']]
    if crc32(data) != info['data_crc32']:
        errs.append('data CRC mismatch')
    if bool(info['flags'] & OBV1_FLAG_BLOCK_SAFE) and not block_safe(w, h, sub):
        errs.append('BLOCK_SAFE set but geometry/subsampling is not block safe')
    end = 0
    time_end = 0
    for i, entry in enumerate(entries):
        off, size = entry[:2]
        if info['version'] == OBV2_VERSION:
            timestamp, duration = entry[2:]
            if timestamp != time_end or duration < 16666:
                errs.append('frame %d: invalid timestamp/duration' % i)
            time_end = timestamp + duration
        if len(errs) > 20:
            break
        if (off != end if info['version'] == OBV2_VERSION else off < end) or off + size > info['data_size'] or not 4 <= size <= info['max_frame_size']:
            errs.append('frame %d: bad index entry off=%d size=%d' % (i, off, size))
            continue
        end = off + size
        fr = data[off:off + size]
        try:
            marker, fw, fh, fsub = jpeg_sof(fr)
            if marker != 0xC0:
                errs.append('frame %d: SOF%d, not baseline' % (i, marker - 0xC0))
            if (fw, fh) != (w, h):
                errs.append('frame %d: %dx%d, header says %dx%d' % (i, fw, fh, w, h))
            if fsub != sub:
                errs.append('frame %d: subsampling %s, header says %s' % (i, fsub, sub))
            if fr[-2:] != b'\xff\xd9':
                errs.append('frame %d: missing EOI' % i)
        except (ValueError, struct.error) as e:
            errs.append('frame %d: %s' % (i, e))
    if info['version'] == OBV2_VERSION:
        if time_end > 0xffffffff or (time_end + 999) // 1000 != info['duration_ms'] or end != info['data_size']:
            errs.append('OBV2 duration or trailing frame data mismatch')
        if timed_ring_bytes(entries, info['max_frame_size']) > 1280 * 1024:
            errs.append('OBV2 one-second prebuffer exceeds 1.25 MiB')
    if errs:
        for e in errs:
            print('%s: FAIL %s' % (args.file, e))
        return 1
    if info['version'] == OBV2_VERSION:
        rate = info['frame_count'] * 1000000 / time_end
        print('%s: OK (OBV2, %d frames, %dx%d %s, %.3f s, avg %.2f fps)' %
              (args.file, info['frame_count'], w, h, sub, time_end / 1000000, rate))
    else:
        print('%s: OK (%d frames, %dx%d %s %d fps)' % (args.file, info['frame_count'], w, h, sub,
                                                      info['fps']))
    return 0


# ---------------------------------------------------------------- legacy (delta_varint)

def rgb888_to_rgb565(r, g, b):
    r5 = (r * 31 + 127) // 255
    g6 = (g * 63 + 127) // 255
    b5 = (b * 31 + 127) // 255
    return ((r5 & 0x1F) << 11) | ((g6 & 0x3F) << 5) | (b5 & 0x1F)


def encode_varint(value):
    out = bytearray()
    while True:
        chunk = value & 0x7F
        value >>= 7
        if value > 0:
            chunk |= 0x80
        out.append(chunk)
        if value == 0:
            return bytes(out)


def grid_edges(canvas, grid):
    return [int(i * canvas / grid) for i in range(grid + 1)]


def frame_to_cells(img, grid, x_edges, y_edges, threshold):
    """Per cell: black when its brightest pixel is below threshold, else the
    average colour of the pixels at/above threshold (keeps thin fading lines)."""
    w, h = img.size
    px = img.load()
    cells = []
    for gy in range(grid):
        y0, y1 = y_edges[gy], y_edges[gy + 1]
        if y1 <= y0:
            continue
        for gx in range(grid):
            x0, x1 = x_edges[gx], x_edges[gx + 1]
            if x1 <= x0:
                continue
            max_lum = 0.0
            sr = sg = sb = 0
            bright = count = 0
            for y in range(y0, min(y1, h)):
                for x in range(x0, min(x1, w)):
                    r, g, b = px[x, y][:3]
                    lum = 0.299 * r + 0.587 * g + 0.114 * b
                    if lum > max_lum:
                        max_lum = lum
                    count += 1
                    if lum >= threshold:
                        sr += r
                        sg += g
                        sb += b
                        bright += 1
            if count == 0 or max_lum < threshold or bright == 0:
                packed = 0
            else:
                packed = ((sr // bright) << 16) | ((sg // bright) << 8) | (sb // bright)
            cells.append((gy * grid + gx, packed))
    return cells


def encode_frame_delta(prev, cells, count_u32):
    changes = []
    for idx, packed in cells:
        if prev.get(idx, -1) != packed:
            prev[idx] = packed
            changes.append((idx, packed))
    buf = bytearray(struct.pack('<I' if count_u32 else '<H', len(changes)))
    prev_idx = -1
    for idx, packed in changes:
        delta = idx if prev_idx < 0 else idx - prev_idx
        black = 1 if packed == 0 else 0
        buf += encode_varint((delta << 1) | black)
        if not black:
            buf += struct.pack('<H', rgb888_to_rgb565((packed >> 16) & 0xFF,
                                                      (packed >> 8) & 0xFF, packed & 0xFF))
        prev_idx = idx
    return bytes(buf), len(changes)


def cmd_legacy(args):
    canvas, grid, fps = args.canvas, args.grid, args.fps
    if not (1 <= canvas <= 512 and 1 <= grid <= canvas and 1 <= fps <= 120):
        raise SystemExit('ERROR: need canvas 1..512, grid 1..canvas, fps 1..120')
    use_v2 = grid * grid > 0xFFFF   # u16 change count would overflow
    fmt = 'delta_varint_rgb565_black_v2' if use_v2 else 'delta_varint_rgb565_black_v1'
    os.makedirs(args.output, exist_ok=True)
    xe = ye = grid_edges(canvas, grid)
    prev, out, peak, total, n = {}, bytearray(), 0, 0, 0
    for im in load_frames(args.input, canvas, canvas, fps, 'contain', args.shift_up,
                          args.duration):
        data, changes = encode_frame_delta(prev, frame_to_cells(im, grid, xe, ye, args.threshold),
                                           use_v2)
        out += data
        n += 1
        peak = max(peak, changes)
        total += changes
        if n % 20 == 0:
            print('  frame %d: %d changes' % (n, changes))
    if not n:
        raise SystemExit('ERROR: no frames')
    manifest = '\n'.join([
        'canvas_width=%d' % canvas, 'canvas_height=%d' % canvas,
        'grid_width=%d' % grid, 'grid_height=%d' % grid,
        'fps=%d' % fps, 'frame_count=%d' % n,
        'duration_ms=%d' % int(round(n * 1000.0 / fps)),
        'binary_size=%d' % len(out),
        'stream_format=%s' % fmt, 'data_file=boot_block.bin']) + '\n'
    with open(os.path.join(args.output, 'boot_block.bin'), 'wb') as f:
        f.write(out)
    with open(os.path.join(args.output, 'boot_block.txt'), 'w') as f:
        f.write(manifest)
    print('%s: %d frames %dx%d grid %d, %d B, avg %.0f / peak %d changes' % (
        args.output, n, canvas, canvas, grid, len(out), total / n, peak))
    if args.raw:
        raw = manifest.encode() + b'\xff' * (0x1000 - len(manifest)) + bytes(out)
        with open(args.raw, 'wb') as f:
            f.write(raw)
        print('  raw image %s (%d B)' % (args.raw, len(raw)))
    return 0


# ---------------------------------------------------------------- self-test data

ST_SIZE = 360
ST_CELL = 40
ST_BAND_Y = 352     # last 8 lines: half MCU row for 4:2:0
ST_BAND = (255, 128, 0)
ST_PALETTE = [(0, 0, 0), (255, 255, 255), (220, 30, 30), (30, 200, 60), (40, 60, 230),
              (240, 220, 40), (40, 210, 220), (210, 50, 200), (128, 128, 128)]


def selftest_image():
    n = ST_SIZE // ST_CELL
    cells = [[(cx + 3 * cy) % len(ST_PALETTE) for cx in range(n)] for cy in range(n)]
    im = Image.new('RGB', (ST_SIZE, ST_SIZE))
    for cy in range(n):
        for cx in range(n):
            im.paste(ST_PALETTE[cells[cy][cx]],
                     (cx * ST_CELL, cy * ST_CELL, (cx + 1) * ST_CELL, (cy + 1) * ST_CELL))
    im.paste(ST_BAND, (0, ST_BAND_Y, ST_SIZE, ST_SIZE))
    return im, cells


def c_bytes(name, data):
    lines = ['static const uint8_t %s[%d] = {' % (name, len(data))]
    for i in range(0, len(data), 16):
        lines.append('    ' + ', '.join('0x%02x' % b for b in data[i:i + 16]) + ',')
    lines.append('};')
    return '\n'.join(lines)


def cmd_selftest_data(args):
    im, cells = selftest_image()
    j422 = encode_jpeg(im, 75, '422')
    j420 = encode_jpeg(im, 75, '420')
    n = len(cells)
    out = [
        '/* Generated by tools/make_video.py selftest-data - do not edit. */',
        '/* 360x360 test picture: %dx%d cells of %d px, palette index per cell,' % (n, n, ST_CELL),
        ' * lines %d..%d an orange band (half MCU row for 4:2:0). Q75 baseline. */'
        % (ST_BAND_Y, ST_SIZE - 1),
        '#define MJ_ST_SIZE   %d' % ST_SIZE,
        '#define MJ_ST_CELL   %d' % ST_CELL,
        '#define MJ_ST_CELLS  %d' % n,
        '#define MJ_ST_BAND_Y %d' % ST_BAND_Y,
        'static const uint8_t k_st_band[3] = {%d, %d, %d};' % ST_BAND,
        'static const uint8_t k_st_palette[%d][3] = {' % len(ST_PALETTE),
    ]
    out += ['    {%d, %d, %d},' % c for c in ST_PALETTE]
    out += ['};', 'static const uint8_t k_st_cells[%d][%d] = {' % (n, n)]
    out += ['    {' + ', '.join(str(v) for v in row) + '},' for row in cells]
    out += ['};', c_bytes('k_st_jpeg_422', j422), c_bytes('k_st_jpeg_420', j420), '']
    with open(args.output, 'w') as f:
        f.write('\n'.join(out))
    print('%s: 4:2:2 %d B, 4:2:0 %d B' % (args.output, len(j422), len(j420)))
    return 0


# ---------------------------------------------------------------- CLI

def add_obv_opts(p, fps, quality, sub):
    p.add_argument('input')
    p.add_argument('-o', '--output', required=True)
    p.add_argument('--fps', type=int, default=fps)
    p.add_argument('--quality', type=int, default=quality, help='JPEG quality (lowered per '
                   'frame, down to 30, when a frame exceeds --max-frame)')
    p.add_argument('--sub', choices=('422', '420', '444', 'gray'), default=sub,
                   help='chroma subsampling (4:2:2 / 4:2:0 allow block decode)')
    p.add_argument('--max-frame', type=int, default=OBV1_FRAME_MAX,
                   help='per-frame byte cap (<= 49152)')
    p.add_argument('--fit', choices=('contain', 'cover'), default='contain')
    p.add_argument('--shift-up', type=int, default=0, help='move content up N px')
    p.add_argument('--duration', type=float, default=0.0, help='trim/loop to N seconds')


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    sp = ap.add_subparsers(dest='cmd')
    sp.required = True

    p = sp.add_parser('boot', help='SD boot animation /sdcard/OBD/BOOT/boot.obv')
    add_obv_opts(p, 24, 75, '422')
    p.add_argument('--size', type=int, default=360)
    p.add_argument('--loop', action='store_true', help='set the LOOP hint flag')
    p.set_defaults(fn=cmd_boot)

    p = sp.add_parser('theme', help='theme element video')
    add_obv_opts(p, 30, 75, '422')
    p.add_argument('--size', type=parse_size, required=True,
                   help='element WxH; rounded up to multiples of 8')
    p.add_argument('--poster', help='also write the first frame as a PNG poster')
    p.add_argument('--no-loop', action='store_true')
    p.set_defaults(fn=cmd_theme)

    p = sp.add_parser('inspect', help='print an OBV1/OBV2 header')
    p.add_argument('file')
    p.set_defaults(fn=cmd_inspect)

    p = sp.add_parser('verify', help='check CRCs and every frame of an OBV1/OBV2 file')
    p.add_argument('file')
    p.set_defaults(fn=cmd_verify)

    p = sp.add_parser('legacy', help='delta_varint boot animation (flash bootmedia partition)')
    p.add_argument('input')
    p.add_argument('-o', '--output', default='bootmedia/slot_a')
    p.add_argument('--canvas', type=int, default=360)
    p.add_argument('--grid', type=int, default=240)
    p.add_argument('--fps', type=int, default=15)
    p.add_argument('--threshold', type=float, default=40.0,
                   help='luminance below which a cell is black')
    p.add_argument('--shift-up', type=int, default=0)
    p.add_argument('--duration', type=float, default=0.0)
    p.add_argument('--raw', help='also write the bootmedia.raw image (4 KB manifest + bin)')
    p.set_defaults(fn=cmd_legacy)

    p = sp.add_parser('selftest-data', help='regenerate main/media/media_jpeg_selftest.inc')
    p.add_argument('-o', '--output', default='main/media/media_jpeg_selftest.inc')
    p.set_defaults(fn=cmd_selftest_data)

    args = ap.parse_args()
    return args.fn(args)


if __name__ == '__main__':
    sys.exit(main())
