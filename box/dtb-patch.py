#!/usr/bin/env python3
"""Fire TV Cube 3 device-tree changes for cube-moonlight (runs on the box).

  dtb-patch.py check        show what is applied, in /flash/dtb.img and in the running kernel
  dtb-patch.py dryrun       apply to a copy in /tmp and show the dts diff (dtb.img untouched)
  dtb-patch.py apply        back up /flash/dtb.img (first time only), then apply what is missing
  dtb-patch.py rollback     put the backup back (add --force if dtb.img changed since apply)
  dtb-patch.py apply-to IN OUT   apply to a copy (testing)

The changes (both take effect after a reboot):
  1. VDIN1 loopback capture -> /dev/video71 (Kodi GUI + video as the TV gets it):
     /vdin1 status "okay", v4l_support_en 1; /reserved-memory/linux,vdin1_cma status "okay"
  2. Audio loopback -> ALSA hw:0,4 (Kodi's PCM on its way to HDMI):
     /soc/audiobus@0xFE330000/loopback@0: datalb_src 2 (TDMOUT_C), datalb_chnum 8,
     datalb_chmask 0xff, datalb-lane-mask-in <1 1 1 1>, a phandle; and a new LAST sound-card
     link /auge_sound/aml-audio-card,dai-link@4 (loopback -> Amlogic dummy codec). It must be
     the last link: ALSA numbers PCM devices in link order, so Kodi's devices keep their numbers.

Only the file /flash/dtb.img is edited (written as a new file, then renamed over it); no block
device is ever touched. CoreELEC's dtb.xml service only rewrites its own nodes, so these edits
survive it, but a CoreELEC update replaces dtb.img: run install.sh again afterwards.
"""
import hashlib
import os
import re
import shutil
import subprocess
import sys

DTB = '/flash/dtb.img'
BAKDIR = '/storage/cube-moonlight/backup'
BAK = BAKDIR + '/dtb.img.orig'
PATCHED_MD5 = BAKDIR + '/dtb.img.patched.md5'
DT_ID = 't7_gazelle_pvt'                       # Fire TV Cube 3rd gen
VDIN = '/vdin1'
CMA = '/reserved-memory/linux,vdin1_cma'
LB = '/soc/audiobus@0xFE330000/loopback@0'
CARD = '/auge_sound'
LINK = CARD + '/aml-audio-card,dai-link@4'
LB_PROPS = [('datalb_src', 'u', '2'), ('datalb_chnum', 'u', '8'), ('datalb_chmask', 'x', 'ff'),
            ('datalb-lane-mask-in', 'u', '1 1 1 1')]


class Refuse(Exception):
    pass


def run(*cmd, inp=None):
    r = subprocess.run(cmd, input=inp, capture_output=True, text=True)
    if r.returncode:
        raise Refuse('%s failed: %s' % (' '.join(cmd), r.stderr.strip()))
    return r.stdout


def get(f, node, prop, t='s'):
    r = subprocess.run(['fdtget', '-t', t, f, node, prop], capture_output=True, text=True)
    return r.stdout.strip() if r.returncode == 0 else None


def has_node(f, node):
    return subprocess.run(['fdtget', '-l', f, node], capture_output=True).returncode == 0


def md5(f):
    return hashlib.md5(open(f, 'rb').read()).hexdigest()


def dts(f):
    return run('dtc', '-q', '-I', 'dtb', '-O', 'dts', f)


def links(f):
    return run('fdtget', '-l', f, CARD).split()


def vdin_applied(f):
    return (get(f, VDIN, 'status') == 'okay' and get(f, VDIN, 'v4l_support_en', 'u') == '1'
            and get(f, CMA, 'status') == 'okay')


def audio_state(f):
    """'applied', 'missing', or raises Refuse for a layout this script doesn't know."""
    ls = links(f)
    base = ['aml-audio-card,dai-link@%d' % i for i in range(4)]
    if ls == base:
        return 'missing'
    if ls == base + ['aml-audio-card,dai-link@4']:
        ph = get(f, LB, 'phandle', 'x')
        if (ph and get(f, LINK + '/cpu', 'sound-dai', 'x') == ph
                and all(get(f, LB, p, t) == v for p, t, v in LB_PROPS)):
            return 'applied'
        raise Refuse('%s has a dai-link@4 that is not cube-moonlight\'s' % f)
    raise Refuse('unexpected sound-card links in %s: %s' % (f, ' '.join(ls)))


def preflight(f):
    dtid = get(f, '/', 'amlogic-dt-id')
    if dtid != DT_ID:
        raise Refuse('%s is for "%s", not the Fire TV Cube 3 (%s)' % (f, dtid, DT_ID))
    if 'vdin-t7' not in (get(f, VDIN, 'compatible') or '') or not has_node(f, CMA):
        raise Refuse('no VDIN1 nodes in %s' % f)
    if not has_node(f, LB) or not has_node(f, CARD + '/aml-audio-card,dai-link@3'):
        raise Refuse('no audio loopback / sound card nodes in %s' % f)


def dummy_codec_phandle(text):
    m = re.search(r'compatible = "amlogic, aml_dummy_codec";[^{}]*?phandle = <(0x[0-9a-f]+)>;', text)
    if not m:
        raise Refuse('no phandle on the aml_dummy_codec node')
    return m.group(1)


def apply_to(src, dst):
    """Copy src to dst with every missing change applied. Returns the list of changes made."""
    preflight(src)
    shutil.copyfile(src, dst)
    done = []
    if not vdin_applied(dst):
        run('fdtput', '-t', 's', dst, CMA, 'status', 'okay')
        run('fdtput', '-t', 's', dst, VDIN, 'status', 'okay')
        run('fdtput', '-t', 'i', dst, VDIN, 'v4l_support_en', '1')
        done.append('VDIN1 loopback capture')
    if audio_state(dst) == 'missing':
        text = dts(dst)
        codec = dummy_codec_phandle(text)
        ph = get(dst, LB, 'phandle', 'x')
        if not ph:
            used = [int(x, 16) for x in re.findall(r'phandle = <0x([0-9a-f]+)>;', text)]
            ph = '%x' % (max(used) + 1)
            run('fdtput', '-t', 'x', dst, LB, 'phandle', ph)
        for p, t, v in LB_PROPS:
            run('fdtput', '-t', t, dst, LB, p, *v.split())
        # add dai-link@4 after dai-link@3 as text (fdtput -c would put it first)
        text = dts(dst)
        start = text.find('\t\taml-audio-card,dai-link@3 {\n')
        if start < 0:
            raise Refuse('dai-link@3 not found in the dts')
        depth, i = 0, start
        while True:
            c = text[i]
            if c == '{':
                depth += 1
            elif c == '}':
                depth -= 1
                if depth == 0:
                    break
            i += 1
        end = text.index('\n', i) + 1
        node = ('\n\t\taml-audio-card,dai-link@4 {\n'
                '\t\t\tmclk-fs = <0x100>;\n'
                '\t\t\tcontinuous-clock;\n'
                '\t\t\tsuffix-name = "alsaPORT-loopback";\n\n'
                '\t\t\tcpu {\n\t\t\t\tsound-dai = <0x%s>;\n'
                '\t\t\t\tsystem-clock-frequency = <0xbb8000>;\n\t\t\t};\n\n'
                '\t\t\tcodec {\n\t\t\t\tsound-dai = <%s>;\n\t\t\t};\n\t\t};\n') % (ph, codec)
        text = text[:end] + node + text[end:]
        run('dtc', '-q', '-I', 'dts', '-O', 'dtb', '-o', dst, '-', inp=text)
        done.append('audio loopback (hw:0,4)')
    if not vdin_applied(dst) or audio_state(dst) != 'applied':
        raise Refuse('verification of the edited copy failed')
    dts(dst)                                   # must still parse
    return done


def write_dtb(src):
    """Replace /flash/dtb.img with src: new file + fsync + rename, never in place."""
    tmp = DTB + '.cube-new'
    shutil.copyfile(src, tmp)
    with open(tmp, 'rb') as fh:
        os.fsync(fh.fileno())
    if md5(tmp) != md5(src):
        os.unlink(tmp)
        raise Refuse('copy to /flash failed verification')
    os.rename(tmp, DTB)
    os.sync()


def running_state():
    dt = '/proc/device-tree'
    try:
        vd = open(dt + '/vdin1/status').read().strip('\0\n') == 'okay'
    except OSError:
        vd = False
    au = os.path.isdir(dt + '/auge_sound/aml-audio-card,dai-link@4')
    return vd, au


def check():
    print('%s md5 %s' % (DTB, md5(DTB)))
    preflight(DTB)
    print('  VDIN1 loopback capture: %s' % ('applied' if vdin_applied(DTB) else 'missing'))
    print('  audio loopback:         %s' % audio_state(DTB))
    vd, au = running_state()
    print('running kernel: VDIN1 %s, audio loopback %s, /dev/video71 %s' % (
        'on' if vd else 'off', 'on' if au else 'off',
        'present' if os.path.exists('/dev/video71') else 'absent'))
    if os.path.exists(BAK):
        print('backup: %s (md5 %s)' % (BAK, md5(BAK)))


def main(argv):
    cmd = argv[1] if len(argv) > 1 else ''
    tmp = '/tmp/cube-moonlight-dtb'
    if cmd == 'check':
        check()
    elif cmd == 'apply-to' and len(argv) == 4:
        print('applied: %s' % (', '.join(apply_to(argv[2], argv[3])) or 'nothing (all present)'))
    elif cmd == 'dryrun':
        os.makedirs(tmp, exist_ok=True)
        done = apply_to(DTB, tmp + '/new.dtb')
        open(tmp + '/old.dts', 'w').write(dts(DTB))
        open(tmp + '/new.dts', 'w').write(dts(tmp + '/new.dtb'))
        subprocess.run(['diff', tmp + '/old.dts', tmp + '/new.dts'])
        print('dryrun: would apply %s (%s untouched)' % (', '.join(done) or 'nothing', DTB))
    elif cmd == 'apply':
        os.makedirs(tmp, exist_ok=True)
        done = apply_to(DTB, tmp + '/new.dtb')
        if not done:
            print('nothing to do: both changes are already in %s' % DTB)
            return 0
        os.makedirs(BAKDIR, exist_ok=True)
        if not os.path.exists(BAK):
            shutil.copyfile(DTB, BAK)
            with open(BAK, 'rb') as fh:
                os.fsync(fh.fileno())
            if md5(BAK) != md5(DTB):
                raise Refuse('backup verification failed')
            print('backup: %s' % BAK)
        write_dtb(tmp + '/new.dtb')
        open(PATCHED_MD5, 'w').write(md5(DTB) + '\n')
        print('applied: %s. Reboot to activate.' % ', '.join(done))
    elif cmd == 'rollback':
        if not os.path.exists(BAK):
            raise Refuse('no backup %s' % BAK)
        want = open(PATCHED_MD5).read().strip() if os.path.exists(PATCHED_MD5) else None
        if want and md5(DTB) != want and '--force' not in argv:
            raise Refuse('%s changed since cube-moonlight patched it (CoreELEC update?); '
                         'restoring the old backup could undo that. Use --force to restore anyway, '
                         'or leave it.' % DTB)
        write_dtb(BAK)
        print('restored %s from %s - reboot to activate' % (DTB, BAK))
    else:
        print(__doc__)
        return 1
    return 0


if __name__ == '__main__':
    try:
        sys.exit(main(sys.argv))
    except Refuse as e:
        print('dtb-patch: %s' % e, file=sys.stderr)
        sys.exit(2)
