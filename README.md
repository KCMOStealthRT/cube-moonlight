# cube-moonlight

Stream the **whole output of a Fire TV Cube (3rd gen) running CoreELEC 22 / Kodi 22** to any
[Moonlight](https://moonlight-stream.org) client. You get the Kodi GUI, hardware-decoded video
and audio, and you can control Kodi with a keyboard, mouse, touch or gamepad from a PC, phone,
tablet or another TV.

It is a small GameStream host written in C that runs on the Cube itself. Nothing else is needed:
no PC in the loop, no capture card.

```
HDMI output ──► VDIN1 loopback ──► Wave521 HW H.264/HEVC enc ┐
(Kodi GUI + video)  (/dev/video71)       (zero-copy dma-buf) ├─► Moonlight host ──► Moonlight
Kodi PCM ─────► audio loopback ──► Opus stereo / 5.1 / 7.1 ──┘   (RTSP, ENet, RTP + FEC)
                  (ALSA hw:0,4)                      ◄── keyboard / mouse / touch / gamepad (uinput)
```

> **Status: working proof of concept, built and tested on one box** (CoreELEC 22.0-Piers_rc1,
> Moonlight-qt 6.2 on Windows, Moonlight on Android). Expect rough edges. Read
> [Risks](#risks-read-before-installing) before installing: it edits a boot file.

## Features

- 1080p60 H.264 or HEVC (the client picks) from the Cube's own hardware encoder (~4–5 ms per
  frame, ~1 % CPU)
- Stereo, 5.1 and 7.1 Opus audio, as the client asks (follows Kodi's output, resampled to 48 kHz)
- Video FEC (Reed-Solomon, 20 %) + send pacing, audio FEC: robust on Wi-Fi
- Input: keyboard, relative and absolute mouse (touch), scroll, up to 4 gamepads. Gamepads show
  up as Steam virtual pads, so Kodi's built-in button map works.
- Moonlight pairing (any number of clients), launch / resume / quit
- Dolby/DTS passthrough is switched off in Kodi during a stream (bitstreams can't be captured)
  and back on afterwards. Kodi restarts a playing film when that setting flips, so cube-stream
  seeks back to where you were.
- Follows output-mode changes (e.g. 1080p60 → 2160p24 for a film)
- Starts at boot (systemd service)
- Announces itself on the LAN (mDNS `_nvstream._tcp` through CoreELEC's Avahi), so Moonlight
  lists it without typing the IP

## Limitations

- **Stream resolution ≤ 1080p**: VDIN on this SoC scales to at most 1920 wide. 4K output is
  downscaled. HEVC is 8-bit Main only (no HDR stream).
- **Dolby Vision content can't be captured** (the driver doesn't support DV loopback; colours
  come out wrong). SDR works. HDR10 looked right on an SDR client; HDR output itself is untested.
- Protected (Widevine L1) video would be black. Normal Kodi playback isn't protected.
- One stream at a time. A new launch ends the running one.
- mDNS discovery only works on the box's own subnet (it doesn't cross routers/VLANs unless
  your router repeats mDNS). Otherwise add the box **by its IP address**.
- The pairing PIN is entered over SSH (`pin.sh`), not on the TV.
- Moonlight's UTF-8 text messages aren't handled. On-screen keyboards that send key presses
  work (tested with Artemis on Android).
- Untested: what happens when the TV / HDMI sink is off or unplugged.
- Kodi's volume applies to the stream (the audio is tapped after Kodi's mixer).

## Requirements

- **Fire TV Cube 3rd gen** (`t7_gazelle_pvt`), unlocked, running **CoreELEC 22** with SSH
  enabled. Other Amlogic boxes are not supported: the installer refuses them.
- Kodi > Settings > Services > Control > **"Allow remote control from applications on this
  system"** on (local JSON-RPC, for the passthrough switch and seek-back).
- A **Linux x86_64 machine or WSL2** (Ubuntu 24.04 tested) to build on: ~5 GB disk, ~30 min the
  first time (it rebuilds the CoreELEC kernel to get matching module CRCs).
- A Moonlight client: [moonlight-qt](https://github.com/moonlight-stream/moonlight-qt) (PC/Mac/
  Linux), Moonlight for Android / iOS / Apple TV / webOS, ...

## Licensing: why you build it yourself

The encoder needs Amlogic's userspace library (`libencoder`), which is under an Amlogic SLA
that prohibits redistribution. **This repository contains no Amlogic code and there are no
binary releases.** `build/build.sh` fetches everything from its original source and builds a
package for **your** box. Don't share that package. Details and the exact notice:
[THIRD-PARTY.md](THIRD-PARTY.md).

The kernel modules are built from CoreELEC's own GPL `media_modules` source. Because of
modversions they only load on the exact CoreELEC build they were compiled for, so **every
CoreELEC update needs a rebuild** (see [Updating CoreELEC](#updating-coreelec)).

## Build

On Ubuntu / WSL2:

```bash
sudo apt install gcc-aarch64-linux-gnu make curl git python3 patch bc flex bison file perl libasound2-dev libssl-dev libelf-dev
git clone https://github.com/<you>/cube-moonlight.git && cd cube-moonlight
```

1. Copy a few files from the box: its CoreELEC release, three libraries to link against, and
   its media modules to verify the build against. This is read-only on the box. Run it from any
   shell that can `ssh root@<box-ip>`. Under WSL2's default NAT networking the box may not be
   reachable; run this one step from Git Bash on Windows into a folder WSL can see:

   ```bash
   build/fetch-from-box.sh 192.168.1.50 sysroot
   # with a key: SSH_OPTS="-i ~/.ssh/mykey" build/fetch-from-box.sh 192.168.1.50 sysroot
   ```

2. Build:

   ```bash
   build/build.sh --sysroot sysroot
   ```

   It reads the box's CoreELEC build ID and looks up the exact kernel, `common_drivers`,
   `media_modules` and clang that release uses (from CoreELEC's `package.mk` files). It then
   downloads and builds everything in `~/cube-moonlight-build` (`--work` to change it). Before
   packaging, **every rebuilt media module is compared with the box's own copy** (vermagic and
   all symbol CRCs). If anything differs, the build stops: nothing untrusted goes near your
   kernel. Logs: `~/cube-moonlight-build/build-<release>/*.log`.

   Result: `~/cube-moonlight-build/build-<release>/cube-moonlight-<release>.tar.gz`.

## Install

```bash
scp ~/cube-moonlight-build/build-*/cube-moonlight-*.tar.gz root@192.168.1.50:/storage/
ssh root@192.168.1.50
cd /storage && tar -xzf cube-moonlight-*.tar.gz && sh cube-moonlight/install.sh
```

The installer checks the box, the CoreELEC release and the package. It asks before each step:

1. **Edits `/flash/dtb.img`** (the device tree, a boot file) to enable two hardware loopbacks
   that Amlogic ships disabled. One is VDIN1 video capture (`/dev/video71`); the other is the
   audio loopback (ALSA `hw:0,4`). The original is backed up to
   `/storage/cube-moonlight/backup/dtb.img.orig` first. `python3 dtb-patch.py check | dryrun`
   shows exactly what changes. Only that file is touched, never a partition or block device.
2. Installs and enables `cube-moonlight.service`.

Then **reboot**. About 30 s after Kodi is up, `cat /storage/cube-moonlight/cube-stream.log`
should show cube-stream waiting for clients.

## Pair and play

1. In Moonlight the box shows up as **FireCube** (mDNS). If it doesn't (different subnet, or
   Kodi's zeroconf is off), use **Add PC/host manually** → the box's IP. Click it: Moonlight
   shows a PIN.
2. On the box (SSH), within 5 minutes:

   ```bash
   /storage/cube-moonlight/pin.sh 1234
   ```

3. Start **Kodi** in Moonlight. Quitting the stream leaves Kodi "running" (Resume). "Quit app"
   ends it.

In Moonlight's settings, choose 1080p or 720p at 60 fps. A bitrate of 10–20 Mbit/s is plenty for
Kodi. For surround, set Moonlight's audio to 5.1 and Kodi's audio output to 5.1 or more.

Ports (LAN): TCP 47984, 47989, 48010; UDP 47998, 47999, 48000.

## Running it

```bash
systemctl status cube-moonlight            # state
systemctl restart cube-moonlight           # stop = SIGTERM, clean
cat /storage/cube-moonlight/cube-stream.log
touch /storage/cube-moonlight/no-autostart # disable at boot (rm to enable again)
```

Extra options for cube-stream (`LD_LIBRARY_PATH=/storage/cube-moonlight/lib
/storage/cube-moonlight/bin/cube-stream -h`) go on one line in `/storage/cube-moonlight/options`,
e.g. `-F 30` for more video FEC on a poor network.

The encoder modules are loaded into RAM at each boot by the service and are never installed
into the system. A boot-loop guard means that if loading them ever crashed the kernel, the next
boot skips them and says so in the log.

## Updating CoreELEC

A CoreELEC update **replaces `dtb.img`** (undoing the loopbacks) and changes the kernel (the
modules no longer fit). cube-moonlight notices and stays off, with a message in its log. Then:

```bash
build/fetch-from-box.sh 192.168.1.50 sysroot && build/build.sh --sysroot sysroot
# on the box:
systemctl stop cube-moonlight
cd /storage && tar -xzf cube-moonlight-<new release>.tar.gz && sh cube-moonlight/install.sh
reboot
```

Your pairings are kept (`/storage/cube-moonlight/moonlight/`).

## Risks (read before installing)

- **Boot file.** A wrong `dtb.img` can stop CoreELEC from booting. The patcher refuses any
  device tree that isn't the Cube 3's known layout and verifies its result, and the change was
  tested on a real box. Still, **have a way back**: know how to reach `/flash` (the CoreELEC
  data partition) from Fire OS / recovery on your setup, and copy
  `/storage/cube-moonlight/backup/dtb.img.orig` to your PC after installing.
- **Out-of-tree kernel module.** CoreELEC reboots on a kernel oops. The modules come from
  CoreELEC's own source and are CRC-checked against your kernel, but the encoder driver isn't
  something CoreELEC ships or tests.
- **The encoder shares a clock with the video decoders.** No problems were seen, but it's
  something to suspect if playback ever misbehaves while streaming.
- **No access control beyond Moonlight pairing.** RTSP is open on your LAN. Don't expose
  these ports to the internet.
- **Never write to the Cube's `logo` partition or any raw block device** while experimenting
  around this. On the Cube 3 that can hard-brick it. Nothing here does.

## Undo

```bash
sh /storage/cube-moonlight/uninstall.sh            # stop + remove service, restore dtb.img
sh /storage/cube-moonlight/uninstall.sh --purge    # ... and delete everything incl. pairings
reboot
```

If CoreELEC doesn't boot after the dtb change, restore the backup onto `/flash/dtb.img` from
whatever gives you access to the data partition on your setup (Fire OS with root, recovery).
CoreELEC's stock Cube 3 device tree is also on the box as
`/usr/share/bootloader/device_trees/t7_pop1-g_amazon_3rd_gen_cube.dtb`.

## Known issues

- **Don't `kill -9` cube-stream while a client is streaming.** A VDIN driver bug then leaves
  `/dev/video71` busy until the next reboot. `systemctl stop` / `kill` (SIGTERM) is fine, and
  crashes are handled.
- After a stream, the encoder hardware stays powered until reboot (upstream driver behaviour,
  harmless).
- When the client asks for 7.1, the stream negotiates 7.1, but Kodi on this box outputs 5.1
  (the side channels are silent).

## Tools (for development)

- `tools/gs-test-client.py --host IP pair|check|launch|stream`: a Moonlight protocol test client
  (Python, needs `cryptography`). It pairs, then launches and receives video **and** audio. It
  checks every packet, recomputes the FEC parity, decrypts the audio, and writes
  `gs-stream.h264` / `.opus`. Its identity in `./gs-client/` is a paired credential: keep it
  private.
- `tools/vdin-capture.py` (on the box) + `tools/vdin-to-png.py`: grab raw VDIN frames.
- `build/ko-versions.py A.ko [B.ko]`: dump / compare module CRCs.

## How it works

- **Video:** VDIN1 is Amlogic's video input block. In loopback mode with input 6
  (`VIU1_WB0_VPP`) it captures the blended output (GUI + video) as NV12. The V4L2 buffers are
  exported as dma-bufs and fed straight to the Chips&Media Wave521 encoder (`amvenc_multi`), so
  there's no copy. The encoder runs IPP with a single reference frame (Moonlight needs that),
  H.264 High or HEVC Main as the client asks (`-H 0` offers H.264 only).
- **Audio:** the Amlogic sound card's loopback DAI taps TDM-C (Kodi's PCM going to HDMI: 8 ch,
  HDMI order). cube-stream follows Kodi's output rate, maps the channels to Moonlight's layout,
  resamples to 48 kHz and encodes Opus multistream.
- **Host:** `src/gamestream.c` (HTTP/HTTPS, pairing, app list), `src/gs-stream.c` (RTSP, ENet
  control, RTP video with Reed-Solomon FEC, encrypted RTP audio with FEC), `src/gs-input.c`
  (uinput devices), `src/gs-mdns.c` (mDNS announcement via the system's avahi-daemon,
  libavahi-client loaded at run time). The protocol as implemented is in
  [docs/moonlight-protocol.md](docs/moonlight-protocol.md).

## Credits

[Moonlight](https://github.com/moonlight-stream) and [Sunshine](https://github.com/LizardByte/Sunshine)
(protocol reference), [Wolf](https://github.com/games-on-whales/wolf),
[cgutman/enet](https://github.com/cgutman/enet), [nanors](https://github.com/sleepybishop/nanors),
[CoreELEC](https://coreelec.org), [Opus](https://opus-codec.org), [Speex](https://speex.org).
Not affiliated with any of them, Amazon, Amlogic, NVIDIA or Team Kodi.

## License

MIT for this repository ([LICENSE](LICENSE)). Third-party components keep their own licences:
[THIRD-PARTY.md](THIRD-PARTY.md).
