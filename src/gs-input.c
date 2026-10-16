/*
 * gs-input — Moonlight input (control message 0x0206) -> Linux uinput devices. Kodi picks them
 * up like plugged-in hardware: keyboard + mice through libinput, gamepads through
 * peripheral.joystick. Spec: docs/moonlight-protocol.md §6.4; struct layouts checked against
 * moonlight-common-c Input.h (2026-10-14): header BE32 size + LE32 magic, keyboard/gamepad
 * fields LE, mouse fields BE.
 *
 * Devices are created on first use and kept while cube-stream runs (Kodi needs a moment to
 * notice a new device); a gamepad goes away when the client unplugs it. Everything held is
 * released when a stream ends (gs_input_reset), so no key stays stuck.
 * Called from the control thread only: no locking.
 */
#define _GNU_SOURCE
#include "gamestream.h"

#include <errno.h>
#include <fcntl.h>
#include <linux/uinput.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#define MAX_PADS 4
#define ABS_RANGE 65535                 /* absolute pointer: client coordinates scaled to this */

/* Windows virtual-key code -> Linux key code (what Moonlight sends: keyCode & 0xff) */
static const unsigned short vk_map[256] = {
    [0x08] = KEY_BACKSPACE, [0x09] = KEY_TAB, [0x0C] = KEY_KP5, [0x0D] = KEY_ENTER,
    [0x10] = KEY_LEFTSHIFT, [0x11] = KEY_LEFTCTRL, [0x12] = KEY_LEFTALT, [0x13] = KEY_PAUSE,
    [0x14] = KEY_CAPSLOCK, [0x1B] = KEY_ESC, [0x20] = KEY_SPACE,
    [0x21] = KEY_PAGEUP, [0x22] = KEY_PAGEDOWN, [0x23] = KEY_END, [0x24] = KEY_HOME,
    [0x25] = KEY_LEFT, [0x26] = KEY_UP, [0x27] = KEY_RIGHT, [0x28] = KEY_DOWN,
    [0x2C] = KEY_SYSRQ, [0x2D] = KEY_INSERT, [0x2E] = KEY_DELETE,
    [0x30] = KEY_0, [0x31] = KEY_1, [0x32] = KEY_2, [0x33] = KEY_3, [0x34] = KEY_4,
    [0x35] = KEY_5, [0x36] = KEY_6, [0x37] = KEY_7, [0x38] = KEY_8, [0x39] = KEY_9,
    [0x41] = KEY_A, [0x42] = KEY_B, [0x43] = KEY_C, [0x44] = KEY_D, [0x45] = KEY_E,
    [0x46] = KEY_F, [0x47] = KEY_G, [0x48] = KEY_H, [0x49] = KEY_I, [0x4A] = KEY_J,
    [0x4B] = KEY_K, [0x4C] = KEY_L, [0x4D] = KEY_M, [0x4E] = KEY_N, [0x4F] = KEY_O,
    [0x50] = KEY_P, [0x51] = KEY_Q, [0x52] = KEY_R, [0x53] = KEY_S, [0x54] = KEY_T,
    [0x55] = KEY_U, [0x56] = KEY_V, [0x57] = KEY_W, [0x58] = KEY_X, [0x59] = KEY_Y,
    [0x5A] = KEY_Z, [0x5B] = KEY_LEFTMETA, [0x5C] = KEY_RIGHTMETA, [0x5D] = KEY_COMPOSE,
    [0x5F] = KEY_SLEEP,
    [0x60] = KEY_KP0, [0x61] = KEY_KP1, [0x62] = KEY_KP2, [0x63] = KEY_KP3, [0x64] = KEY_KP4,
    [0x65] = KEY_KP5, [0x66] = KEY_KP6, [0x67] = KEY_KP7, [0x68] = KEY_KP8, [0x69] = KEY_KP9,
    [0x6A] = KEY_KPASTERISK, [0x6B] = KEY_KPPLUS, [0x6D] = KEY_KPMINUS, [0x6E] = KEY_KPDOT,
    [0x6F] = KEY_KPSLASH,
    [0x70] = KEY_F1, [0x71] = KEY_F2, [0x72] = KEY_F3, [0x73] = KEY_F4, [0x74] = KEY_F5,
    [0x75] = KEY_F6, [0x76] = KEY_F7, [0x77] = KEY_F8, [0x78] = KEY_F9, [0x79] = KEY_F10,
    [0x7A] = KEY_F11, [0x7B] = KEY_F12,
    [0x90] = KEY_NUMLOCK, [0x91] = KEY_SCROLLLOCK,
    [0xA0] = KEY_LEFTSHIFT, [0xA1] = KEY_RIGHTSHIFT, [0xA2] = KEY_LEFTCTRL,
    [0xA3] = KEY_RIGHTCTRL, [0xA4] = KEY_LEFTALT, [0xA5] = KEY_RIGHTALT,
    [0xA6] = KEY_BACK, [0xA7] = KEY_FORWARD, [0xA8] = KEY_REFRESH, [0xAC] = KEY_HOMEPAGE,
    [0xAD] = KEY_MUTE, [0xAE] = KEY_VOLUMEDOWN, [0xAF] = KEY_VOLUMEUP,
    [0xB0] = KEY_NEXTSONG, [0xB1] = KEY_PREVIOUSSONG, [0xB2] = KEY_STOPCD,
    [0xB3] = KEY_PLAYPAUSE,
    [0xBA] = KEY_SEMICOLON, [0xBB] = KEY_EQUAL, [0xBC] = KEY_COMMA, [0xBD] = KEY_MINUS,
    [0xBE] = KEY_DOT, [0xBF] = KEY_SLASH, [0xC0] = KEY_GRAVE,
    [0xDB] = KEY_LEFTBRACE, [0xDC] = KEY_BACKSLASH, [0xDD] = KEY_RIGHTBRACE,
    [0xDE] = KEY_APOSTROPHE, [0xE2] = KEY_102ND,
};

/* Moonlight gamepad button flags -> Xbox 360 pad as the Linux xpad driver reports it */
static const struct { unsigned flag; unsigned short code; } pad_buttons[] = {
    { 0x1000, BTN_A }, { 0x2000, BTN_B }, { 0x4000, BTN_X }, { 0x8000, BTN_Y },
    { 0x0100, BTN_TL }, { 0x0200, BTN_TR }, { 0x0020, BTN_SELECT }, { 0x0010, BTN_START },
    { 0x0400, BTN_MODE }, { 0x0040, BTN_THUMBL }, { 0x0080, BTN_THUMBR },
};

static int kbd = -1, mouse = -1, tablet = -1, pad[MAX_PADS] = { -1, -1, -1, -1 };
static unsigned char held[KEY_CNT / 8];         /* keys + mouse buttons currently down */
static int last_abs;                            /* last pointer motion was absolute */
static int wheel_rest, hwheel_rest;             /* sub-notch scroll remainder (120 = notch) */
static unsigned long count, scrolls, pad_updates;

static unsigned le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static int be16s(const unsigned char *p) { return (short)(p[0] << 8 | p[1]); }
static int le16s(const unsigned char *p) { return (short)(p[0] | p[1] << 8); }
static uint32_t le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void emit(int fd, int type, int code, int value)
{
    struct input_event e;
    memset(&e, 0, sizeof(e));
    e.type = type;
    e.code = code;
    e.value = value;
    if (write(fd, &e, sizeof(e)) < 0 && errno != EAGAIN) perror("gs: uinput write");
}

static void syn(int fd) { emit(fd, EV_SYN, SYN_REPORT, 0); }

static void abs_axis(int fd, int code, int min, int max, int flat)
{
    struct uinput_abs_setup a;
    memset(&a, 0, sizeof(a));
    a.code = code;
    a.absinfo.minimum = min;
    a.absinfo.maximum = max;
    a.absinfo.flat = flat;
    ioctl(fd, UI_ABS_SETUP, &a);
}

enum { DEV_KBD, DEV_MOUSE, DEV_TABLET, DEV_PAD };

static int create(int kind, int n)
{
    int fd = open("/dev/uinput", O_WRONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        perror("gs: /dev/uinput");
        return -1;
    }
    struct uinput_setup us;
    memset(&us, 0, sizeof(us));
    us.id.bustype = BUS_VIRTUAL;
    us.id.vendor = 0xbeef;
    us.id.version = 1;
    switch (kind) {
    case DEV_KBD:
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        for (int k = KEY_ESC; k < KEY_MAX && k < BTN_MISC; k++) ioctl(fd, UI_SET_KEYBIT, k);
        for (int k = KEY_OK; k <= KEY_MAX && k < BTN_TRIGGER_HAPPY; k++) ioctl(fd, UI_SET_KEYBIT, k);
        us.id.product = 1;
        snprintf(us.name, sizeof(us.name), "Moonlight Keyboard");
        break;
    case DEV_MOUSE:
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        for (int b = BTN_LEFT; b <= BTN_EXTRA; b++) ioctl(fd, UI_SET_KEYBIT, b);
        ioctl(fd, UI_SET_EVBIT, EV_REL);
        ioctl(fd, UI_SET_RELBIT, REL_X);
        ioctl(fd, UI_SET_RELBIT, REL_Y);
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
        ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
        ioctl(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);
        us.id.product = 2;
        snprintf(us.name, sizeof(us.name), "Moonlight Mouse");
        break;
    case DEV_TABLET:    /* absolute pointer like a VM "USB tablet": ABS_X/Y + buttons */
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        for (int b = BTN_LEFT; b <= BTN_EXTRA; b++) ioctl(fd, UI_SET_KEYBIT, b);
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        ioctl(fd, UI_SET_ABSBIT, ABS_X);
        ioctl(fd, UI_SET_ABSBIT, ABS_Y);
        abs_axis(fd, ABS_X, 0, ABS_RANGE, 0);
        abs_axis(fd, ABS_Y, 0, ABS_RANGE, 0);
        ioctl(fd, UI_SET_EVBIT, EV_REL);       /* scroll goes where the pointer is */
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL);
        ioctl(fd, UI_SET_RELBIT, REL_HWHEEL);
        ioctl(fd, UI_SET_RELBIT, REL_WHEEL_HI_RES);
        ioctl(fd, UI_SET_RELBIT, REL_HWHEEL_HI_RES);
        ioctl(fd, UI_SET_PROPBIT, INPUT_PROP_POINTER);
        us.id.product = 3;
        snprintf(us.name, sizeof(us.name), "Moonlight Absolute Mouse");
        break;
    case DEV_PAD:       /* Steam's virtual Xbox 360 pad "Microsoft X-Box 360 pad <n>" 28DE:11FF:
                         * Kodi's peripheral.joystick ships udev button maps for exactly these
                         * (same xpad layout); for 045E:028E it has only "linux"-driver maps, and
                         * a pad without a map is registered but ignored (tested 2026-10-15) */
        ioctl(fd, UI_SET_EVBIT, EV_KEY);
        for (size_t i = 0; i < sizeof(pad_buttons) / sizeof(pad_buttons[0]); i++)
            ioctl(fd, UI_SET_KEYBIT, pad_buttons[i].code);
        ioctl(fd, UI_SET_EVBIT, EV_ABS);
        for (int a = ABS_X; a <= ABS_RZ; a++) ioctl(fd, UI_SET_ABSBIT, a);
        ioctl(fd, UI_SET_ABSBIT, ABS_HAT0X);
        ioctl(fd, UI_SET_ABSBIT, ABS_HAT0Y);
        abs_axis(fd, ABS_X, -32768, 32767, 128);
        abs_axis(fd, ABS_Y, -32768, 32767, 128);
        abs_axis(fd, ABS_RX, -32768, 32767, 128);
        abs_axis(fd, ABS_RY, -32768, 32767, 128);
        abs_axis(fd, ABS_Z, 0, 255, 0);
        abs_axis(fd, ABS_RZ, 0, 255, 0);
        abs_axis(fd, ABS_HAT0X, -1, 1, 0);
        abs_axis(fd, ABS_HAT0Y, -1, 1, 0);
        us.id.bustype = BUS_USB;
        us.id.vendor = 0x28de;
        us.id.product = 0x11ff;
        us.id.version = 0x110;
        snprintf(us.name, sizeof(us.name), "Microsoft X-Box 360 pad %d", n);
        break;
    }
    if (ioctl(fd, UI_DEV_SETUP, &us) < 0 || ioctl(fd, UI_DEV_CREATE) < 0) {
        fprintf(stderr, "gs: input: creating \"%s\": %s\n", us.name, strerror(errno));
        close(fd);
        return -1;
    }
    if (kind == DEV_PAD) fprintf(stderr, "gs: input: gamepad %d connected (\"%s\")\n", n, us.name);
    else fprintf(stderr, "gs: input: created \"%s\"\n", us.name);
    return fd;
}

static int dev(int *fd, int kind)
{
    if (*fd < 0) *fd = create(kind, 0);
    return *fd;
}

static void key(int fd, int code, int down)
{
    emit(fd, EV_KEY, code, down);
    syn(fd);
    if (down) held[code / 8] |= 1 << (code % 8);
    else held[code / 8] &= ~(1 << (code % 8));
}

static void pad_state(int n, unsigned buttons, int lt, int rt, int lx, int ly, int rx, int ry)
{
    int fd = pad[n];
    for (size_t i = 0; i < sizeof(pad_buttons) / sizeof(pad_buttons[0]); i++)
        emit(fd, EV_KEY, pad_buttons[i].code, !!(buttons & pad_buttons[i].flag));
    emit(fd, EV_ABS, ABS_HAT0X, (buttons & 0x8 ? 1 : 0) - (buttons & 0x4 ? 1 : 0));
    emit(fd, EV_ABS, ABS_HAT0Y, (buttons & 0x2 ? 1 : 0) - (buttons & 0x1 ? 1 : 0));
    emit(fd, EV_ABS, ABS_Z, lt);
    emit(fd, EV_ABS, ABS_RZ, rt);
    emit(fd, EV_ABS, ABS_X, lx);
    emit(fd, EV_ABS, ABS_Y, ly < -32767 ? 32767 : -ly);     /* XInput: up = +, evdev: up = - */
    emit(fd, EV_ABS, ABS_RX, rx);
    emit(fd, EV_ABS, ABS_RY, ry < -32767 ? 32767 : -ry);
    syn(fd);
}

static void pad_remove(int n)
{
    if (pad[n] < 0) return;
    pad_state(n, 0, 0, 0, 0, 0, 0, 0);
    ioctl(pad[n], UI_DEV_DESTROY);
    close(pad[n]);
    pad[n] = -1;
    fprintf(stderr, "gs: input: gamepad %d disconnected\n", n);
}

void gs_input(const unsigned char *p, size_t n)
{
    if (n < 8) return;
    uint32_t magic = le32(p + 4);
    count++;
    switch (magic) {
    case 0x03: case 0x04: {                     /* key down / up */
        if (n < 14) return;
        int vk = le16(p + 9) & 0xff, code = vk_map[vk];
        if (!code) {
            fprintf(stderr, "gs: input: unmapped key VK 0x%02x\n", vk);
            return;
        }
        if (dev(&kbd, DEV_KBD) >= 0) key(kbd, code, magic == 0x03);
        break;
    }
    case 0x07:                                  /* relative mouse move */
        if (n < 12 || dev(&mouse, DEV_MOUSE) < 0) return;
        emit(mouse, EV_REL, REL_X, be16s(p + 8));
        emit(mouse, EV_REL, REL_Y, be16s(p + 10));
        syn(mouse);
        last_abs = 0;
        break;
    case 0x05: {                                /* absolute mouse position */
        if (n < 18 || dev(&tablet, DEV_TABLET) < 0) return;
        int x = be16s(p + 8), y = be16s(p + 10), w = be16s(p + 14), h = be16s(p + 16);
        if (w <= 0 || h <= 0) return;
        x = x < 0 ? 0 : x > w ? w : x;
        y = y < 0 ? 0 : y > h ? h : y;
        emit(tablet, EV_ABS, ABS_X, (long)x * ABS_RANGE / w);
        emit(tablet, EV_ABS, ABS_Y, (long)y * ABS_RANGE / h);
        syn(tablet);
        last_abs = 1;
        break;
    }
    case 0x08: case 0x09: {                     /* mouse button down / up */
        static const unsigned short btn[] = { 0, BTN_LEFT, BTN_MIDDLE, BTN_RIGHT, BTN_SIDE, BTN_EXTRA };
        if (n < 9 || p[8] < 1 || p[8] > 5) return;
        int *fd = last_abs ? &tablet : &mouse;  /* click where the pointer last moved */
        if (dev(fd, last_abs ? DEV_TABLET : DEV_MOUSE) >= 0) key(*fd, btn[p[8]], magic == 0x08);
        break;
    }
    case 0x0A: {                                /* vertical scroll, 120 = one notch, + = up */
        int *fd = last_abs ? &tablet : &mouse;  /* scroll where the pointer last moved */
        if (n < 10 || dev(fd, last_abs ? DEV_TABLET : DEV_MOUSE) < 0) return;
        int amt = be16s(p + 8);
        wheel_rest += amt;
        emit(*fd, EV_REL, REL_WHEEL_HI_RES, amt);
        if (wheel_rest / 120) emit(*fd, EV_REL, REL_WHEEL, wheel_rest / 120);
        wheel_rest %= 120;
        syn(*fd);
        scrolls++;
        break;
    }
    case 0x55000001: {                          /* horizontal scroll (Sunshine extension) */
        int *fd = last_abs ? &tablet : &mouse;  /* scroll where the pointer last moved */
        if (n < 10 || dev(fd, last_abs ? DEV_TABLET : DEV_MOUSE) < 0) return;
        int amt = be16s(p + 8);
        hwheel_rest += amt;
        emit(*fd, EV_REL, REL_HWHEEL_HI_RES, amt);
        if (hwheel_rest / 120) emit(*fd, EV_REL, REL_HWHEEL, hwheel_rest / 120);
        hwheel_rest %= 120;
        syn(*fd);
        scrolls++;
        break;
    }
    case 0x0C: {                                /* multi-controller state (all fields LE) */
        if (n < 34) return;
        int c = le16(p + 10), mask = le16(p + 12);
        if (c < 0 || c >= MAX_PADS) return;
        if (!(mask & (1 << c))) {               /* the client unplugged this pad */
            pad_remove(c);
            return;
        }
        if (pad[c] < 0 && (pad[c] = create(DEV_PAD, c)) < 0) return;
        pad_updates++;
        pad_state(c, le16(p + 16) | le16(p + 30) << 16, p[18], p[19], le16s(p + 20),
                  le16s(p + 22), le16s(p + 24), le16s(p + 26));
        break;
    }
    case 0x55000004:                            /* controller arrival: create it right away */
        if (n >= 9 && p[8] < MAX_PADS && pad[p[8]] < 0) pad[p[8]] = create(DEV_PAD, p[8]);
        break;
    case 0x0D:                                  /* enable haptics: no rumble here */
        break;
    case 0x17:                                  /* UTF-8 text (IME): not supported yet */
        fprintf(stderr, "gs: input: text input ignored\n");
        break;
    default:
        fprintf(stderr, "gs: input: magic 0x%08x (%zu bytes) ignored\n", magic, n);
    }
}

void gs_input_reset(void)
{
    int fds[] = { kbd, mouse, tablet };
    for (int k = 0; k < KEY_CNT; k++) {
        if (!(held[k / 8] & 1 << (k % 8))) continue;
        for (int i = 0; i < 3; i++)
            if (fds[i] >= 0) emit(fds[i], EV_KEY, k, 0);
    }
    for (int i = 0; i < 3; i++)
        if (fds[i] >= 0) syn(fds[i]);
    memset(held, 0, sizeof(held));
    for (int c = 0; c < MAX_PADS; c++) pad_remove(c);
    if (count) fprintf(stderr, "gs: input: %lu events this stream (%lu scroll, %lu gamepad), "
                       "all released\n", count, scrolls, pad_updates);
    count = scrolls = pad_updates = 0;
    wheel_rest = hwheel_rest = 0;
}
