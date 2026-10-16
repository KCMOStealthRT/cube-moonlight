"""Grab frames from the Amlogic VDIN1 loopback (/dev/video71) on the Cube (debug tool, copy to /storage).

Runs on the box with the stock Python (ctypes + ioctl, no extra packages):

    python3 /storage/vdin-capture.py info              # read-only: caps, inputs, formats, current format
    python3 /storage/vdin-capture.py capture [N] [WxH] [FOURCC] [INPUT]
                                                       # N frames (default 3), default 1920x1080 NV12,
                                                       # input 6 = VIU1_WB0_VPP (video + GUI blended)
    python3 /storage/vdin-capture.py rate [SECONDS] [WxH] [FOURCC] [INPUT] [BUFFERS] [copy]
                                                       # frame-rate test: no files written; 'copy'
                                                       # also reads every frame out of the buffer

Inputs (from `info`, 2026-10-12): 0 VIU1_WB0_VD1 (video layer 1; the driver's default),
1 VIU1_WB0_VD2, 3 VIU1_WB0_OSD1 (GUI), 4 VIU1_WB0_OSD2, 6 VIU1_WB0_VPP (blended output).

Frames are saved raw to /storage/vdin-capture/ with a .json next to each (size, format,
plane strides); convert them on the PC with tools/vdin-to-png.py.

The device is V4L2 multi-planar capture only (device_caps 0x05201000). Every wait has a
timeout and the stream is always turned off and the device closed, even on errors.
"""
import ctypes as C
import fcntl
import json
import mmap
import os
import select
import sys
import time

DEV = '/dev/video71'
OUT = '/storage/vdin-capture'
TIMEOUT = 3  # seconds to wait for one frame

BUF_TYPE_MPLANE = 9  # V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE
MEMORY_MMAP = 1
FIELD_NONE = 1


def fourcc(s):
    return ord(s[0]) | ord(s[1]) << 8 | ord(s[2]) << 16 | ord(s[3]) << 24


def fourcc_str(v):
    return ''.join(chr((v >> (8 * i)) & 0xff) for i in range(4))


class Capability(C.Structure):
    _fields_ = [('driver', C.c_char * 16), ('card', C.c_char * 32), ('bus_info', C.c_char * 32),
                ('version', C.c_uint32), ('capabilities', C.c_uint32),
                ('device_caps', C.c_uint32), ('reserved', C.c_uint32 * 3)]


class FmtDesc(C.Structure):
    _fields_ = [('index', C.c_uint32), ('type', C.c_uint32), ('flags', C.c_uint32),
                ('description', C.c_char * 32), ('pixelformat', C.c_uint32),
                ('mbus_code', C.c_uint32), ('reserved', C.c_uint32 * 3)]


class Input(C.Structure):
    _fields_ = [('index', C.c_uint32), ('name', C.c_char * 32), ('type', C.c_uint32),
                ('audioset', C.c_uint32), ('tuner', C.c_uint32), ('std', C.c_uint64),
                ('status', C.c_uint32), ('capabilities', C.c_uint32), ('reserved', C.c_uint32 * 3)]


class PlanePixFormat(C.Structure):
    _fields_ = [('sizeimage', C.c_uint32), ('bytesperline', C.c_uint32),
                ('reserved', C.c_uint16 * 6)]


class PixFormatMplane(C.Structure):
    _fields_ = [('width', C.c_uint32), ('height', C.c_uint32), ('pixelformat', C.c_uint32),
                ('field', C.c_uint32), ('colorspace', C.c_uint32),
                ('plane_fmt', PlanePixFormat * 8), ('num_planes', C.c_uint8),
                ('flags', C.c_uint8), ('ycbcr_enc', C.c_uint8), ('quantization', C.c_uint8),
                ('xfer_func', C.c_uint8), ('reserved', C.c_uint8 * 7)]


class FormatUnion(C.Union):
    # raw_data[200] sets the size; the kernel union also holds pointers, hence 8-byte alignment
    _fields_ = [('pix_mp', PixFormatMplane), ('raw_data', C.c_uint8 * 200), ('_align', C.c_uint64)]


class Format(C.Structure):
    _fields_ = [('type', C.c_uint32), ('fmt', FormatUnion)]


class RequestBuffers(C.Structure):
    _fields_ = [('count', C.c_uint32), ('type', C.c_uint32), ('memory', C.c_uint32),
                ('capabilities', C.c_uint32), ('flags', C.c_uint8), ('reserved', C.c_uint8 * 3)]


class PlaneM(C.Union):
    _fields_ = [('mem_offset', C.c_uint32), ('userptr', C.c_ulong), ('fd', C.c_int32)]


class Plane(C.Structure):
    _fields_ = [('bytesused', C.c_uint32), ('length', C.c_uint32), ('m', PlaneM),
                ('data_offset', C.c_uint32), ('reserved', C.c_uint32 * 11)]


class Timeval(C.Structure):
    _fields_ = [('sec', C.c_long), ('usec', C.c_long)]


class Timecode(C.Structure):
    _fields_ = [('type', C.c_uint32), ('flags', C.c_uint32), ('frames', C.c_uint8),
                ('seconds', C.c_uint8), ('minutes', C.c_uint8), ('hours', C.c_uint8),
                ('userbits', C.c_uint8 * 4)]


class BufferM(C.Union):
    _fields_ = [('offset', C.c_uint32), ('userptr', C.c_ulong),
                ('planes', C.POINTER(Plane)), ('fd', C.c_int32)]


class Buffer(C.Structure):
    _fields_ = [('index', C.c_uint32), ('type', C.c_uint32), ('bytesused', C.c_uint32),
                ('flags', C.c_uint32), ('field', C.c_uint32), ('timestamp', Timeval),
                ('timecode', Timecode), ('sequence', C.c_uint32), ('memory', C.c_uint32),
                ('m', BufferM), ('length', C.c_uint32), ('reserved2', C.c_uint32),
                ('request_fd', C.c_int32)]


# struct sizes the kernel expects on arm64 (they're encoded in the ioctl numbers)
EXPECTED = {Capability: 104, FmtDesc: 64, Input: 80, Format: 208, RequestBuffers: 20,
            Plane: 64, Buffer: 88}
for _cls, _size in EXPECTED.items():
    assert C.sizeof(_cls) == _size, '%s is %d bytes, expected %d' % (_cls.__name__, C.sizeof(_cls), _size)


def _ioc(direction, nr, size):
    return direction << 30 | size << 16 | ord('V') << 8 | nr


R, W, RW = 2, 1, 3
QUERYCAP = _ioc(R, 0, C.sizeof(Capability))
ENUM_FMT = _ioc(RW, 2, C.sizeof(FmtDesc))
G_FMT = _ioc(RW, 4, C.sizeof(Format))
S_FMT = _ioc(RW, 5, C.sizeof(Format))
REQBUFS = _ioc(RW, 8, C.sizeof(RequestBuffers))
QUERYBUF = _ioc(RW, 9, C.sizeof(Buffer))
QBUF = _ioc(RW, 15, C.sizeof(Buffer))
DQBUF = _ioc(RW, 17, C.sizeof(Buffer))
STREAMON = _ioc(W, 18, C.sizeof(C.c_int))
STREAMOFF = _ioc(W, 19, C.sizeof(C.c_int))
ENUMINPUT = _ioc(RW, 26, C.sizeof(Input))
G_INPUT = _ioc(R, 38, C.sizeof(C.c_int))
S_INPUT = _ioc(RW, 39, C.sizeof(C.c_int))


def ioctl(fd, req, arg):
    fcntl.ioctl(fd, req, arg, True)
    return arg


def describe_format(f):
    p = f.fmt.pix_mp
    planes = [{'bytesperline': p.plane_fmt[i].bytesperline, 'sizeimage': p.plane_fmt[i].sizeimage}
              for i in range(p.num_planes)]
    return {'width': p.width, 'height': p.height, 'fourcc': fourcc_str(p.pixelformat),
            'field': p.field, 'colorspace': p.colorspace, 'quantization': p.quantization,
            'xfer_func': p.xfer_func, 'ycbcr_enc': p.ycbcr_enc, 'num_planes': p.num_planes,
            'planes': planes}


def info(fd):
    cap = ioctl(fd, QUERYCAP, Capability())
    print('driver=%s card=%s bus=%s caps=0x%08x device_caps=0x%08x' % (
        cap.driver.decode(), cap.card.decode(), cap.bus_info.decode(),
        cap.capabilities, cap.device_caps))

    print('inputs:')
    i = 0
    while i < 32:
        inp = Input(index=i)
        try:
            ioctl(fd, ENUMINPUT, inp)
        except OSError:
            break
        print('  %d: %s (type %d, status 0x%x)' % (i, inp.name.decode(errors='replace'), inp.type, inp.status))
        i += 1
    try:
        cur = C.c_int()
        ioctl(fd, G_INPUT, cur)
        print('current input: %d' % cur.value)
    except OSError as e:
        print('G_INPUT failed: %s' % e)

    print('formats:')
    i = 0
    while i < 32:
        d = FmtDesc(index=i, type=BUF_TYPE_MPLANE)
        try:
            ioctl(fd, ENUM_FMT, d)
        except OSError:
            break
        print('  %s %s' % (fourcc_str(d.pixelformat), d.description.decode(errors='replace')))
        i += 1

    f = Format(type=BUF_TYPE_MPLANE)
    try:
        ioctl(fd, G_FMT, f)
        print('current format: %s' % json.dumps(describe_format(f)))
    except OSError as e:
        print('G_FMT failed: %s' % e)


def capture(fd, count, width, height, pixfmt, input_index, seconds=None, nbufs=3, copy=False):
    ioctl(fd, S_INPUT, C.c_int(input_index))
    cur = ioctl(fd, G_INPUT, C.c_int())
    print('input: %d' % cur.value)
    f = Format(type=BUF_TYPE_MPLANE)
    p = f.fmt.pix_mp
    p.width, p.height, p.pixelformat, p.field = width, height, fourcc(pixfmt), FIELD_NONE
    # the driver doesn't fill in the planes itself ("not support num_planes=0", and REQBUFS
    # then fails), so describe them: NV12/NV21 one plane, NM12/NM21 Y + CbCr, UYVY one plane
    if pixfmt in ('NM12', 'NM21'):
        p.num_planes = 2
        p.plane_fmt[0].bytesperline = p.plane_fmt[1].bytesperline = width
        p.plane_fmt[0].sizeimage = width * height
        p.plane_fmt[1].sizeimage = width * height // 2
    elif pixfmt == 'UYVY':
        p.num_planes = 1
        p.plane_fmt[0].bytesperline = width * 2
        p.plane_fmt[0].sizeimage = width * height * 2
    else:
        p.num_planes = 1
        p.plane_fmt[0].bytesperline = width
        p.plane_fmt[0].sizeimage = width * height * 3 // 2
    ioctl(fd, S_FMT, f)
    meta = dict(describe_format(f), input=input_index)
    print('format set: %s' % json.dumps(meta))
    if not p.num_planes:
        raise SystemExit('driver rejected the format (num_planes 0); not requesting buffers')

    req = ioctl(fd, REQBUFS, RequestBuffers(count=nbufs, type=BUF_TYPE_MPLANE, memory=MEMORY_MMAP))
    print('buffers: %d' % req.count)
    maps = []  # per buffer: list of (mmap, length) per plane
    planes_keep = []  # keep plane arrays alive while the kernel may write to them
    streaming = False
    try:
        for i in range(req.count):
            planes = (Plane * 8)()
            planes_keep.append(planes)
            b = Buffer(index=i, type=BUF_TYPE_MPLANE, memory=MEMORY_MMAP, length=8)
            b.m.planes = C.cast(planes, C.POINTER(Plane))
            ioctl(fd, QUERYBUF, b)
            maps.append([(mmap.mmap(fd, planes[j].length, mmap.MAP_SHARED, mmap.PROT_READ,
                                    offset=planes[j].m.mem_offset), planes[j].length)
                         for j in range(b.length)])
            ioctl(fd, QBUF, b)

        ioctl(fd, STREAMON, C.c_int(BUF_TYPE_MPLANE))
        streaming = True
        if seconds:
            measure(fd, maps, seconds, copy)
            return
        os.makedirs(OUT, exist_ok=True)
        stamp = time.strftime('%Y%m%d-%H%M%S')
        got = 0
        while got < count:
            ready, _, _ = select.select([fd], [], [], TIMEOUT)
            if not ready:
                print('no frame within %d s, giving up' % TIMEOUT)
                break
            planes = (Plane * 8)()
            b = Buffer(type=BUF_TYPE_MPLANE, memory=MEMORY_MMAP, length=8)
            b.m.planes = C.cast(planes, C.POINTER(Plane))
            ioctl(fd, DQBUF, b)
            name = '%s/%s-%d' % (OUT, stamp, got)
            with open(name + '.raw', 'wb') as out:
                for j in range(b.length):
                    m, _ = maps[b.index][j]
                    used = planes[j].bytesused or planes[j].length
                    m.seek(planes[j].data_offset)
                    out.write(m.read(used - planes[j].data_offset))
            frame = dict(meta, sequence=b.sequence, flags=b.flags,
                         bytesused=[planes[j].bytesused for j in range(b.length)])
            with open(name + '.json', 'w') as out:
                json.dump(frame, out)
            print('saved %s.raw (seq %d, %s bytes)' % (name, b.sequence, frame['bytesused']))
            got += 1
            ioctl(fd, QBUF, b)
    finally:
        if streaming:
            try:
                ioctl(fd, STREAMOFF, C.c_int(BUF_TYPE_MPLANE))
            except OSError as e:
                print('STREAMOFF failed: %s' % e)
        for planes in maps:
            for m, _ in planes:
                m.close()
        try:  # release the driver's buffers
            ioctl(fd, REQBUFS, RequestBuffers(count=0, type=BUF_TYPE_MPLANE, memory=MEMORY_MMAP))
        except OSError:
            pass


def measure(fd, maps, seconds, copy):
    """Dequeue/requeue for SECONDS and print frame-rate statistics."""
    arrivals, seqs, stamps = [], [], []
    copied = 0
    cpu0, start = time.process_time(), time.monotonic()
    while time.monotonic() - start < seconds:
        ready, _, _ = select.select([fd], [], [], TIMEOUT)
        if not ready:
            print('no frame within %d s, stopping' % TIMEOUT)
            break
        planes = (Plane * 8)()
        b = Buffer(type=BUF_TYPE_MPLANE, memory=MEMORY_MMAP, length=8)
        b.m.planes = C.cast(planes, C.POINTER(Plane))
        ioctl(fd, DQBUF, b)
        arrivals.append(time.monotonic())
        seqs.append(b.sequence)
        stamps.append(b.timestamp.sec + b.timestamp.usec / 1e6)
        if copy:
            for j in range(b.length):
                m, _ = maps[b.index][j]
                copied += len(m[:planes[j].bytesused or planes[j].length])
        ioctl(fd, QBUF, b)
    elapsed = time.monotonic() - start
    cpu = time.process_time() - cpu0
    n = len(arrivals)
    print('frames: %d in %.2f s = %.2f fps' % (n, elapsed, n / elapsed if elapsed else 0))
    if n > 1:
        gaps = sorted((b - a) * 1000 for a, b in zip(arrivals, arrivals[1:]))
        med = gaps[len(gaps) // 2]
        print('interval ms: min %.1f  median %.1f  avg %.1f  max %.1f  (> 1.5x median: %d)' % (
            gaps[0], med, sum(gaps) / len(gaps), gaps[-1], sum(1 for g in gaps if g > 1.5 * med)))
        span = stamps[-1] - stamps[0]
        if span > 0:
            print('driver timestamps: %.2f fps over %.2f s' % ((n - 1) / span, span))
        print('sequence: first %d last %d (frames the driver numbered: %d)' % (
            seqs[0], seqs[-1], seqs[-1] - seqs[0] + 1))
    print('script cpu: %.2f s = %.0f%% of one core%s' % (
        cpu, 100 * cpu / elapsed if elapsed else 0,
        ', copied %.0f MB/s' % (copied / elapsed / 1e6) if copy else ''))


def main():
    mode = sys.argv[1] if len(sys.argv) > 1 else 'info'
    fd = os.open(DEV, os.O_RDWR | os.O_NONBLOCK)
    try:
        if mode == 'info':
            info(fd)
        elif mode == 'capture':
            count = int(sys.argv[2]) if len(sys.argv) > 2 else 3
            size = sys.argv[3] if len(sys.argv) > 3 else '1920x1080'
            pixfmt = sys.argv[4] if len(sys.argv) > 4 else 'NV12'
            input_index = int(sys.argv[5]) if len(sys.argv) > 5 else 6  # VIU1_WB0_VPP
            width, height = (int(v) for v in size.split('x'))
            capture(fd, count, width, height, pixfmt, input_index)
        elif mode == 'rate':
            seconds = float(sys.argv[2]) if len(sys.argv) > 2 else 10
            size = sys.argv[3] if len(sys.argv) > 3 else '1920x1080'
            pixfmt = sys.argv[4] if len(sys.argv) > 4 else 'NV12'
            input_index = int(sys.argv[5]) if len(sys.argv) > 5 else 6
            nbufs = int(sys.argv[6]) if len(sys.argv) > 6 else 3
            copy = len(sys.argv) > 7 and sys.argv[7] == 'copy'
            width, height = (int(v) for v in size.split('x'))
            capture(fd, 0, width, height, pixfmt, input_index, seconds, nbufs, copy)
        else:
            sys.exit(__doc__)
    finally:
        os.close(fd)


if __name__ == '__main__':
    main()
