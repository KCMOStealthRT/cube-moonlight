"""Convert raw frames from vdin-capture.py to PNG (runs on the PC; needs numpy + Pillow).

    python tools/vdin-to-png.py <frame.raw> [...]

Reads the .json next to each .raw for size, format and strides. NV12/NV12M/NV21/NV21M
(Y plane + interleaved chroma) and UYVY. BT.709 limited range, as the VPP output is.
"""
import json
import sys

import numpy as np
from PIL import Image


def yuv_to_rgb(y, u, v):
    y = (y.astype(np.float32) - 16) * (255 / 219)
    u = (u.astype(np.float32) - 128) * (255 / 224)
    v = (v.astype(np.float32) - 128) * (255 / 224)
    r = y + 1.5748 * v
    g = y - 0.1873 * u - 0.4681 * v
    b = y + 1.8556 * u
    return np.clip(np.dstack([r, g, b]), 0, 255).astype(np.uint8)


def convert(raw_path):
    meta = json.load(open(raw_path[:-4] + '.json'))
    w, h, fmt = meta['width'], meta['height'], meta['fourcc']
    data = np.fromfile(raw_path, dtype=np.uint8)
    planes = meta['planes']
    if fmt in ('NV12', 'NV21', 'NM12', 'NM21'):
        stride = planes[0]['bytesperline'] or w
        if len(planes) == 1 and stride == w * 3 // 2:
            stride = w  # vdin reports NV12's average bytes per pixel row (1.5 x width), not the Y stride
        if len(planes) > 1:  # NV12M: chroma is a second plane right after the first
            y_size = meta['bytesused'][0] or planes[0]['sizeimage']
        else:
            y_size = stride * h
        y = data[:stride * h].reshape(h, stride)[:, :w]
        c_stride = planes[1]['bytesperline'] if len(planes) > 1 and planes[1]['bytesperline'] else stride
        c = data[y_size:y_size + c_stride * (h // 2)].reshape(h // 2, c_stride)[:, :w]
        first, second = c[:, 0::2], c[:, 1::2]
        u, v = (first, second) if fmt in ('NV12', 'NM12') else (second, first)
        u = u.repeat(2, axis=0).repeat(2, axis=1)
        v = v.repeat(2, axis=0).repeat(2, axis=1)
        rgb = yuv_to_rgb(y, u, v)
    elif fmt == 'UYVY':
        stride = planes[0]['bytesperline'] or w * 2
        px = data[:stride * h].reshape(h, stride)[:, :w * 2]
        u, y0, v, y1 = px[:, 0::4], px[:, 1::4], px[:, 2::4], px[:, 3::4]
        y = np.empty((h, w), np.uint8)
        y[:, 0::2], y[:, 1::2] = y0, y1
        rgb = yuv_to_rgb(y, u.repeat(2, axis=1), v.repeat(2, axis=1))
    else:
        sys.exit('unsupported format %s' % fmt)
    out = raw_path[:-4] + '.png'
    Image.fromarray(rgb).save(out)
    print(out)


for path in sys.argv[1:]:
    convert(path)
