/*
 * cube-stream — Moonlight host streaming the Fire TV Cube 3's full output (Kodi GUI + video).
 *
 *   VDIN1 loopback (/dev/video71, input 6 VIU1_WB0_VPP, NV12, V4L2 mmap)
 *     -> T7 Wave521 hardware encoder (libvpcodec, /dev/amvenc_multi) -> H.264 or HEVC Main
 *   + audio: LOOPBACK-A hw:0,4 (Kodi's PCM to HDMI, dtb change in box/dtb-patch.py)
 *     at Kodi's output rate -> stereo/5.1/7.1 -> 48 kHz (speexdsp) -> Opus (static libopus)
 *
 * Needs encoder_common.ko + amvenc_multi.ko loaded (box/autostart.sh does it). Each stream gets a
 * fresh encoder instance, so it starts with SPS/PPS + IDR.
 * Moonlight host (gamestream.c + gs-stream.c + gs-input.c): pairing, launch, RTSP, control,
 * input, RTP video + Opus audio — serve_moonlight().
 * Build: build/build.sh. The Amlogic libs are SLA code: never redistribute builds.
 */
#define _GNU_SOURCE
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <alsa/asoundlib.h>
#include <opus/opus.h>
#include <opus/opus_multistream.h>
#include "speex_resampler.h"   /* speexdsp resample.c, built with RANDOM_PREFIX=cube */

#include "vp_multi_codec_1_0.h"
#include "gamestream.h"

#define NBUFS 4
#define OUTBUF_SIZE (4 * 1024 * 1024)

static struct {
    const char *dev, *gsdir;
    int input, gop, zerocopy, audio, fec, pace;
} opt = { "/dev/video71", "/storage/cube-moonlight/moonlight", 6, 60, 1, 1, 20, 300 };

/* capture buffer: mmap for the copy path, exported dma-buf fd (-1 if none) for zero-copy */
struct vbuf { unsigned char *ptr; size_t len; int dmafd; };

static volatile sig_atomic_t stop;
static void on_signal(int sig) { (void)sig; stop = 1; }

/* VDIN's release refuses to clean up while capture runs (VDIN_FLAG_DEC_STARTED), leaving the
 * queue owned by a dead file: every later REQBUFS fails with EBUSY until a reboot. So on a
 * crash stop the capture before dying (kill -9 can't be caught: never use it while capturing). */
static volatile int capture_fd = -1;
static void on_fatal(int sig)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (capture_fd >= 0) ioctl(capture_fd, VIDIOC_STREAMOFF, &type);
    signal(sig, SIG_DFL);
    raise(sig);
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do r = ioctl(fd, req, arg); while (r < 0 && errno == EINTR);
    return r;
}

static void die(const char *what)
{
    perror(what);
    exit(1);
}

/* VDIN1 capture: same sequence as tools/vdin-capture.py (the driver needs num_planes and the
 * plane size filled in, otherwise REQBUFS fails). Returns the fd, or -1 (logged, cleaned up):
 * a capture failure ends that client/stream, never the process (the Moonlight host lives in
 * it). bufs must come in zeroed with dmafd = -1. */
static void capture_close(int fd, struct vbuf *bufs);

static int capture_open(struct vbuf *bufs, int width, int height)
{
    const char *what = opt.dev;
    int fd = open(opt.dev, O_RDWR | O_NONBLOCK);
    if (fd < 0) goto fail;
    what = "VIDIOC_S_INPUT";
    if (xioctl(fd, VIDIOC_S_INPUT, &opt.input) < 0) goto fail;

    struct v4l2_format f = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE };
    f.fmt.pix_mp.width = width;
    f.fmt.pix_mp.height = height;
    f.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_NV12;
    f.fmt.pix_mp.field = V4L2_FIELD_NONE;
    f.fmt.pix_mp.num_planes = 1;
    f.fmt.pix_mp.plane_fmt[0].bytesperline = width;
    f.fmt.pix_mp.plane_fmt[0].sizeimage = width * height * 3 / 2;
    what = "VIDIOC_S_FMT";
    if (xioctl(fd, VIDIOC_S_FMT, &f) < 0) goto fail;
    if (f.fmt.pix_mp.num_planes != 1) {
        errno = EINVAL;
        what = "VIDIOC_S_FMT (num_planes)";
        goto fail;
    }

    struct v4l2_requestbuffers req = { .count = NBUFS, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                       .memory = V4L2_MEMORY_MMAP };
    what = "VIDIOC_REQBUFS";    /* EBUSY: VDIN was left streaming by a killed process, see
                                   README "Known issues" */
    if (xioctl(fd, VIDIOC_REQBUFS, &req) < 0) goto fail;
    if (req.count < 2) { errno = ENOMEM; goto fail; }

    for (unsigned i = 0; i < req.count; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
        struct v4l2_buffer b = { .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                 .memory = V4L2_MEMORY_MMAP, .length = VIDEO_MAX_PLANES,
                                 .m.planes = planes };
        what = "VIDIOC_QUERYBUF";
        if (xioctl(fd, VIDIOC_QUERYBUF, &b) < 0) goto fail;
        bufs[i].len = planes[0].length;
        bufs[i].ptr = mmap(NULL, planes[0].length, PROT_READ, MAP_SHARED, fd,
                           planes[0].m.mem_offset);
        what = "mmap";
        if (bufs[i].ptr == MAP_FAILED) { bufs[i].ptr = NULL; goto fail; }
        bufs[i].dmafd = -1;
        if (opt.zerocopy) {
            struct v4l2_exportbuffer e = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                           .index = i, .plane = 0, .flags = O_RDONLY | O_CLOEXEC };
            if (xioctl(fd, VIDIOC_EXPBUF, &e) == 0)
                bufs[i].dmafd = e.fd;
            else if (i == 0)
                fprintf(stderr, "VIDIOC_EXPBUF: %s -> copy path\n", strerror(errno));
        }
        what = "VIDIOC_QBUF";
        if (xioctl(fd, VIDIOC_QBUF, &b) < 0) goto fail;
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    what = "VIDIOC_STREAMON";
    if (xioctl(fd, VIDIOC_STREAMON, &type) < 0) goto fail;
    fprintf(stderr, "capture: %s input %d, %dx%d NV12, %u buffers, %s\n",
            opt.dev, opt.input, width, height, req.count,
            bufs[0].dmafd >= 0 ? "zero-copy (dma-buf)" : "copy (vmalloc)");
    capture_fd = fd;
    return fd;
fail:
    fprintf(stderr, "capture: %s: %s\n", what, strerror(errno));
    if (fd >= 0) capture_close(fd, bufs);
    return -1;
}

static void capture_close(int fd, struct vbuf *bufs)
{
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(fd, VIDIOC_STREAMOFF, &type);
    capture_fd = -1;
    for (int i = 0; i < NBUFS; i++) {
        if (bufs[i].ptr && bufs[i].ptr != MAP_FAILED) munmap(bufs[i].ptr, bufs[i].len);
        if (bufs[i].dmafd >= 0) close(bufs[i].dmafd);
    }
    close(fd);
}

/* GOP preset IPP_SINGLE (each P frame references only the previous one). The default preset
 * IPP references two frames, which Moonlight's decoder setup can't hold (it patches the SPS
 * for low delay: "reference picture missing during reorder"). */
static vl_codec_handle_t encoder_open(int width, int height, int fps, int kbps, int hevc)
{
    vl_encode_info_t info;
    qp_param_t qp;
    memset(&info, 0, sizeof(info));
    memset(&qp, 0, sizeof(qp));
    info.width = width;
    info.height = height;
    info.frame_rate = fps;
    info.bit_rate = kbps * 1000;
    info.gop = opt.gop;
    info.img_format = IMG_FMT_NV12;
    info.prepend_spspps_to_idr_frames = true;   /* (VPS/)SPS/PPS before every IDR */
    info.profile = 0;                           /* auto: H.264 High / HEVC Main */
    info.gop_pattern = 8;                       /* PRESET_IDX_IPP_SINGLE */
    qp.qp_min = 0;  qp.qp_max = 51;
    qp.qp_I_base = 30; qp.qp_I_min = 0; qp.qp_I_max = 51;
    qp.qp_P_base = 30; qp.qp_P_min = 0; qp.qp_P_max = 51;
    qp.qp_B_base = 30; qp.qp_B_min = 0; qp.qp_B_max = 51;
    vl_codec_handle_t h = vl_multi_encoder_init(hevc ? CODEC_ID_H265 : CODEC_ID_H264, info, &qp);
    if (!h) fprintf(stderr, "vl_multi_encoder_init failed\n");
    return h;
}

/* encode capture buffer vb (frame at data_offset) into out */
static encoding_metadata_t encode_buf(vl_codec_handle_t enc, const struct vbuf *vb,
                                      unsigned data_offset, int width, int height,
                                      vl_frame_type_t type, unsigned char *out)
{
    unsigned char *y = vb->ptr + data_offset;
    vl_buffer_info_t in, ret;
    memset(&in, 0, sizeof(in));
    memset(&ret, 0, sizeof(ret));
    in.buf_fmt = IMG_FMT_NV12;
    in.buf_stride = 0;                       /* stride == width */
    if (vb->dmafd >= 0) {                    /* encoder reads the VDIN buffer directly */
        in.buf_type = DMA_TYPE;
        in.buf_info.dma_info.shared_fd[0] = vb->dmafd;
        in.buf_info.dma_info.shared_fd[1] = in.buf_info.dma_info.shared_fd[2] = -1;
        in.buf_info.dma_info.num_planes = 1; /* NV12 in one buffer, CbCr after Y */
    } else {                                 /* library copies from the mmap */
        in.buf_type = VMALLOC_TYPE;
        in.buf_info.in_ptr[0] = (unsigned long)y;
        in.buf_info.in_ptr[1] = (unsigned long)(y + width * height);
        in.buf_info.in_ptr[2] = 0;
    }
    return vl_multi_encoder_encode(enc, type, out, &in, &ret);
}

static int send_all(int sock, const unsigned char *p, size_t n)
{
    while (n) {
        ssize_t w = send(sock, p, n, MSG_NOSIGNAL);
        if (w < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= w;
    }
    return 0;
}

/* --- audio: LOOPBACK-A (hw:0,4, taps TDM-C = Kodi's PCM to HDMI) -> Opus -> Moonlight
 * The loopback must run at Kodi's current output rate (44.1 or 48 kHz, read from Kodi's TDM-C
 * PCM, pcm3p): reopen it when that changes, idle while Kodi outputs no PCM (idle/passthrough).
 * Capture 8 ch S32 in HDMI order FL FR LFE FC S1L S1R S2L S2R (Kodi is set to 7.1; a 5.1 source
 * fills slots 4-5), then per output layout (Moonlight channel order FL FR FC LFE BL BR SL SR):
 *   stereo: L = FL + .707 (FC + S1L + S2L), R likewise (LFE dropped)
 *   5.1:    FL FR FC LFE, BL = S1L + .707 S2L, BR likewise
 *   7.1:    FL FR FC LFE, BL BR = slots 4-5, SL SR = slots 6-7
 * resample to 48 kHz (speexdsp), Opus (multistream, Sunshine's layouts: 1/1, 4/2, 5/3 streams/
 * coupled, identity mapping), CBR frames of the client's packet duration (5 ms) via
 * gs_audio_send, paced silence while Kodi outputs no PCM so the client's audio clock keeps
 * running. */
#define AUDIO_DEV "hw:0,4"
#define KODI_HW_PARAMS "/proc/asound/card0/pcm3p/sub0/hw_params"
#define A_CH 8
#define O_RATE 48000
#define O_FRAME_MAX 960                 /* 20 ms at 48 kHz */
#define FIFO_MAX (O_FRAME_MAX * 8)
#define O_CH_MAX 8
#define ML_AUDIO_KBPS 256               /* Moonlight stereo (Sunshine: 96, or 512 "HQ") */
#define ML_AUDIO_KBPS_51 512            /* Sunshine: 256, HQ 1536 */
#define ML_AUDIO_KBPS_71 640            /* Sunshine: 450, HQ 2048 */

struct audio_ctx {
    int channels;                       /* output: 2, 6 (5.1) or 8 (7.1) */
    int frame;                          /* Opus frame in 48 kHz samples */
    int kbps;
    volatile int run;
    unsigned long packets, bytes, overruns, silence;
    int rate;                           /* current capture rate, 0 = idle */
    pthread_t th;
};

/* Kodi's current TDM-C output rate, 0 if that PCM is closed */
static int kodi_rate(void)
{
    char buf[512];
    FILE *f = fopen(KODI_HW_PARAMS, "r");
    if (!f) return 0;
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    char *r = strstr(buf, "rate: ");
    return r ? atoi(r + 6) : 0;
}

static snd_pcm_t *loopback_open(int rate, unsigned latency_us)
{
    snd_pcm_t *pcm;
    int err;
    if ((err = snd_pcm_open(&pcm, AUDIO_DEV, SND_PCM_STREAM_CAPTURE, 0)) < 0) {
        fprintf(stderr, "audio: %s: %s\n", AUDIO_DEV, snd_strerror(err));
        return NULL;
    }
    if ((err = snd_pcm_set_params(pcm, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                                  A_CH, rate, 0, latency_us)) < 0 ||
        (err = snd_pcm_start(pcm)) < 0) {   /* a capture stream only runs once started */
        fprintf(stderr, "audio: %s @ %d Hz: %s\n", AUDIO_DEV, rate, snd_strerror(err));
        snd_pcm_close(pcm);
        return NULL;
    }
    return pcm;
}

static void *audio_thread(void *arg)
{
    struct audio_ctx *a = arg;
    snd_pcm_t *pcm = NULL;
    SpeexResamplerState *rs = NULL;
    int err, rate = 0, frame = a->frame, ch = a->channels;
    static const unsigned char mapping[O_CH_MAX] = { 0, 1, 2, 3, 4, 5, 6, 7 };
    int streams = ch == 8 ? 5 : ch == 6 ? 4 : 1, coupled = ch == 8 ? 3 : ch == 6 ? 2 : 1;
    OpusMSEncoder *enc = opus_multistream_encoder_create(O_RATE, ch, streams, coupled, mapping,
                                                         OPUS_APPLICATION_RESTRICTED_LOWDELAY, &err);
    int32_t *cap = malloc(4800 * A_CH * sizeof(int32_t));   /* up to 100 ms per read */
    float *st = malloc(4800 * ch * sizeof(float));            /* mixed to the output layout */
    float *fifo = malloc((FIFO_MAX + 6000) * ch * sizeof(float));   /* 48 kHz */
    static const float zero[O_FRAME_MAX * O_CH_MAX];
    int fifo_n = 0;
    double t_check = 0, t_silence = 0;
    unsigned char pkt[1500];

    if (!enc || !cap || !st || !fifo) {
        fprintf(stderr, "audio: init failed (%s)\n", enc ? "malloc" : opus_strerror(err));
        goto out;
    }
    opus_multistream_encoder_ctl(enc, OPUS_SET_BITRATE(a->kbps * 1000));
    opus_multistream_encoder_ctl(enc, OPUS_SET_VBR(0));   /* CBR: audio FEC */

    while (a->run && !stop) {
        double now = now_s();
        if (now >= t_check) {           /* follow Kodi's output rate */
            t_check = now + 0.5;
            int want = kodi_rate();
            if (want != rate) {
                if (pcm) { snd_pcm_close(pcm); pcm = NULL; }
                if (rs) { speex_resampler_destroy(rs); rs = NULL; }
                fifo_n = 0;
                rate = 0;
                if (want) {     /* short periods (≈5 ms) for low latency */
                    pcm = loopback_open(want, 20000);
                    rs = pcm ? speex_resampler_init(ch, want, O_RATE, 5, &err) : NULL;
                    if (pcm && rs) rate = want;
                }
                a->rate = rate;
                fprintf(stderr, "audio: Kodi PCM %s -> capture %s\n",
                        want ? "on" : "off", rate ? "running" : "idle");
                if (want) fprintf(stderr, "audio: %d Hz -> Opus %s 48 kHz %d kbit/s, %d ms\n",
                                  want, ch == 8 ? "7.1" : ch == 6 ? "5.1" : "stereo", a->kbps,
                                  frame * 1000 / O_RATE);
            }
        }
        if (!pcm) {
            /* silence at the real-time rate; restart the clock after a stall */
            if (now - t_silence > 0.1) t_silence = now;
            while (t_silence <= now) {
                int n = opus_multistream_encode_float(enc, zero, frame, pkt, sizeof(pkt));
                if (n > 0 && gs_audio_send(pkt, n) == 0) { a->packets++; a->bytes += n; }
                a->silence++;
                t_silence += (double)frame / O_RATE;
            }
            usleep(2000);
            continue;
        }
        t_silence = 0;

        if (snd_pcm_wait(pcm, 200) == 0) continue;
        int chunk = rate * frame / O_RATE;
        snd_pcm_sframes_t got = snd_pcm_readi(pcm, cap, chunk);
        if (got == -EAGAIN) continue;
        if (got < 0) {
            a->overruns++;
            if (snd_pcm_recover(pcm, got, 1) < 0 || snd_pcm_start(pcm) < 0) {
                snd_pcm_close(pcm); pcm = NULL; rate = 0; t_check = 0;   /* reopen */
            }
            continue;
        }
        const float k = 1.0f / 2147483648.0f, h = 0.7071f / 2147483648.0f;
        for (int i = 0; i < got; i++) {     /* capture FL FR LFE FC S1L S1R S2L S2R */
            const int32_t *s = cap + i * A_CH;
            float *o = st + i * ch;
            if (ch == 2) {
                float c = h * s[3];
                o[0] = k * s[0] + c + h * s[4] + h * s[6];
                o[1] = k * s[1] + c + h * s[5] + h * s[7];
                continue;
            }
            o[0] = k * s[0]; o[1] = k * s[1]; o[2] = k * s[3]; o[3] = k * s[2];    /* FL FR FC LFE */
            if (ch == 6) {
                o[4] = k * s[4] + h * s[6];
                o[5] = k * s[5] + h * s[7];
            } else {
                o[4] = k * s[4]; o[5] = k * s[5]; o[6] = k * s[6]; o[7] = k * s[7];
            }
        }
        spx_uint32_t in_len = got, out_len = FIFO_MAX + 6000 - fifo_n;
        speex_resampler_process_interleaved_float(rs, st, &in_len, fifo + ch * fifo_n, &out_len);
        fifo_n += out_len;

        while (fifo_n >= frame) {
            int n = opus_multistream_encode_float(enc, fifo, frame, pkt, sizeof(pkt));
            fifo_n -= frame;
            memmove(fifo, fifo + ch * frame, fifo_n * ch * sizeof(float));
            if (n < 0) { fprintf(stderr, "audio: opus: %s\n", opus_strerror(n)); continue; }

            /* no peer yet / stream ending: gs_audio_send just drops it */
            if (gs_audio_send(pkt, n) == 0) { a->packets++; a->bytes += n; }
        }
        if (fifo_n > FIFO_MAX) fifo_n = 0;                       /* can't happen; be safe */
    }
out:
    if (rs) speex_resampler_destroy(rs);
    if (pcm) snd_pcm_close(pcm);
    if (enc) opus_multistream_encoder_destroy(enc);
    free(cap); free(st); free(fifo);
    return NULL;
}

/* --- Kodi JSON-RPC (raw TCP 127.0.0.1:9090): passthrough off while a Moonlight client watches.
 * Passthrough bitstreams (Dolby/DTS) bypass TDM-C, so the loopback captures nothing; with
 * passthrough off Kodi decodes them to PCM. The previous value comes back when the stream ends;
 * a marker file in the state dir covers a crash (restored at the next start). audiooutput.config
 * (fixed/best match) is never touched. */
#define KODI_RPC_PORT 9090
#define PT_MARKER "passthrough-restore"

/* sends one request (id 4242), returns 0 with the matching response object in resp;
 * Kodi may interleave notifications on the socket, those are skipped */
static int kodi_rpc(const char *req, char *resp, size_t n)
{
    int s = socket(AF_INET, SOCK_STREAM, 0);
    struct timeval tv = { .tv_sec = 2 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(KODI_RPC_PORT),
                             .sin_addr.s_addr = htonl(INADDR_LOOPBACK) };
    if (connect(s, (struct sockaddr *)&a, sizeof(a)) < 0 ||
        send_all(s, (const unsigned char *)req, strlen(req)) < 0) {
        close(s);
        return -1;
    }
    size_t len = 0, start = 0;
    int depth = 0, instr = 0, esc = 0;
    while (len < n - 1) {
        ssize_t r = recv(s, resp + len, n - 1 - len, 0);
        if (r <= 0) break;
        for (size_t i = len; i < len + r; i++) {    /* split the stream into JSON objects */
            char c = resp[i];
            if (instr) {
                if (esc) esc = 0;
                else if (c == '\\') esc = 1;
                else if (c == '"') instr = 0;
            } else if (c == '"') {
                instr = 1;
            } else if (c == '{') {
                if (!depth++) start = i;
            } else if (c == '}' && depth && !--depth &&
                       memmem(resp + start, i - start + 1, "\"id\":4242", 9)) {
                memmove(resp, resp + start, i - start + 1);
                resp[i - start + 1] = 0;
                close(s);
                return 0;
            }
        }
        len += r;
    }
    close(s);
    return -1;
}

static int kodi_passthrough_get(void)          /* 1 / 0, -1 = Kodi not reachable */
{
    char r[4096];
    if (kodi_rpc("{\"jsonrpc\":\"2.0\",\"id\":4242,\"method\":\"Settings.GetSettingValue\","
                 "\"params\":{\"setting\":\"audiooutput.passthrough\"}}", r, sizeof(r)) < 0)
        return -1;
    return strstr(r, "\"value\":true") ? 1 : strstr(r, "\"value\":false") ? 0 : -1;
}

static int kodi_passthrough_set(int on)
{
    char q[256], r[4096];
    snprintf(q, sizeof(q), "{\"jsonrpc\":\"2.0\",\"id\":4242,\"method\":\"Settings.SetSettingValue\","
             "\"params\":{\"setting\":\"audiooutput.passthrough\",\"value\":%s}}",
             on ? "true" : "false");
    return kodi_rpc(q, r, sizeof(r)) == 0 && strstr(r, "\"result\":true") ? 0 : -1;
}

static void pt_marker(char *p, size_t n) { snprintf(p, n, "%s/" PT_MARKER, opt.gsdir); }

/* --- seek back after the passthrough flip: Kodi restarts a playing film from the start when
 * audiooutput.passthrough changes (seen with Kodi 22). So read the position before the flip,
 * then (in a thread, the stream mustn't wait) wait until the restarted film plays again and seek
 * back to it. */
static int kodi_player(void)                    /* id of the active player, -1 if none */
{
    char r[4096];
    if (kodi_rpc("{\"jsonrpc\":\"2.0\",\"id\":4242,\"method\":\"Player.GetActivePlayers\"}",
                 r, sizeof(r)) < 0)
        return -1;
    char *p = strstr(r, "\"playerid\":");
    return p ? atoi(p + 11) : -1;
}

static long kodi_time_ms(int player)            /* playback position, -1 if unknown */
{
    char q[256], r[4096];
    snprintf(q, sizeof(q), "{\"jsonrpc\":\"2.0\",\"id\":4242,\"method\":\"Player.GetProperties\","
             "\"params\":{\"playerid\":%d,\"properties\":[\"time\"]}}", player);
    if (kodi_rpc(q, r, sizeof(r)) < 0) return -1;
    char *t = strstr(r, "\"time\":{");          /* "totaltime" has no quote before "time" */
    if (!t) return -1;
    char *end = strchr(t, '}');
    long v[4] = { 0 };
    const char *k[4] = { "\"hours\":", "\"minutes\":", "\"seconds\":", "\"milliseconds\":" };
    for (int i = 0; i < 4; i++) {
        char *f = strstr(t, k[i]);
        if (!f || (end && f > end)) return -1;
        v[i] = atol(f + strlen(k[i]));
    }
    return ((v[0] * 60 + v[1]) * 60 + v[2]) * 1000 + v[3];
}

static void kodi_seek_ms(int player, long ms)
{
    char q[320], r[4096];
    snprintf(q, sizeof(q), "{\"jsonrpc\":\"2.0\",\"id\":4242,\"method\":\"Player.Seek\",\"params\":"
             "{\"playerid\":%d,\"value\":{\"time\":{\"hours\":%ld,\"minutes\":%ld,\"seconds\":%ld,"
             "\"milliseconds\":%ld}}}}", player, ms / 3600000, ms / 60000 % 60, ms / 1000 % 60,
             ms % 1000);
    if (kodi_rpc(q, r, sizeof(r)) < 0 || strstr(r, "\"error\""))
        fprintf(stderr, "kodi: seek back failed\n");
    else
        fprintf(stderr, "kodi: seeked back to %ld:%02ld:%02ld\n", ms / 3600000, ms / 60000 % 60,
                ms / 1000 % 60);
}

static void *seek_back_thread(void *arg)
{
    long pos = (long)(intptr_t)arg, last = -1;
    for (int i = 0; i < 80; i++) {              /* up to 20 s for Kodi to reopen the film */
        usleep(250000);
        int p = kodi_player();
        long t = p < 0 ? -1 : kodi_time_ms(p);
        if (t < 0) continue;
        if (t + 3000 >= pos) {                  /* not (yet) restarted */
            last = -1;
            continue;
        }
        if (last >= 0 && t > last) {            /* restarted and playing again */
            kodi_seek_ms(p, pos);
            return NULL;
        }
        last = t;
    }
    fprintf(stderr, "kodi: no restart seen, position kept as it was\n");
    return NULL;
}

/* flip audiooutput.passthrough; a playing film gets put back where it was */
static int passthrough_flip(int on)
{
    int p = kodi_player();
    long pos = p < 0 ? -1 : kodi_time_ms(p);
    if (kodi_passthrough_set(on) < 0) return -1;
    if (pos > 5000) {                           /* nothing to restore in the first seconds */
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        pthread_create(&th, &at, seek_back_thread, (void *)(intptr_t)pos);
        pthread_attr_destroy(&at);
    }
    return 0;
}

static void passthrough_off(void)
{
    int v = kodi_passthrough_get();
    if (v < 0) fprintf(stderr, "kodi: JSON-RPC not reachable, passthrough unchanged\n");
    if (v != 1) return;
    char p[256];
    pt_marker(p, sizeof(p));
    FILE *f = fopen(p, "w");                    /* before the change: a crash still restores */
    if (f) { fputs("audiooutput.passthrough was on\n", f); fclose(f); }
    if (passthrough_flip(0) < 0) {
        unlink(p);
        fprintf(stderr, "kodi: could not turn passthrough off\n");
        return;
    }
    fprintf(stderr, "kodi: passthrough off while the Moonlight client watches\n");
}

static void passthrough_restore(const char *why)
{
    char p[256];
    pt_marker(p, sizeof(p));
    if (access(p, F_OK)) return;                /* we didn't change it */
    if (passthrough_flip(1) == 0) {
        unlink(p);
        fprintf(stderr, "kodi: passthrough back on (%s)\n", why);
    } else {
        fprintf(stderr, "kodi: passthrough restore failed (%s), marker kept for a retry\n", why);
    }
}

/* current HDMI output mode, e.g. "1080p60hz", "2160p24hz" ("" if unreadable) */
static void display_mode(char *buf, size_t n)
{
    FILE *f = fopen("/sys/class/display/mode", "r");
    buf[0] = 0;
    if (f) {
        if (!fgets(buf, n, f)) buf[0] = 0;
        fclose(f);
    }
    buf[strcspn(buf, "\r\n")] = 0;
}

/* Moonlight session: capture at the client's resolution (VDIN scales), encode with its bitrate,
 * hand each access unit to gs_video_send (RTP over UDP) until the stream ends. Frames beyond
 * the client's fps are skipped; an IDR goes out first and whenever the client asks. */
static void serve_moonlight(const struct gs_stream *s, unsigned char *out)
{
    int w = s->width & ~7, h = s->height & ~7;
    if (w > 1920 || h > 1080 || w < 320 || h < 180) {
        fprintf(stderr, "moonlight: %dx%d not supported, sending 1920x1080\n", s->width, s->height);
        w = 1920;
        h = 1080;
    }
    struct vbuf bufs[NBUFS] = { { 0 } };
    for (int i = 0; i < NBUFS; i++) bufs[i].dmafd = -1;
    int vfd = capture_open(bufs, w, h);
    if (vfd < 0) {
        gss_stop("capture failed");     /* the client gets a termination, no retry loop */
        return;
    }
    vl_codec_handle_t enc = encoder_open(w, h, s->fps, s->kbps, s->hevc);
    if (!enc) { capture_close(vfd, bufs); return; }
    fprintf(stderr, "moonlight: stream %u: %s %dx%d, %d fps max, %d kbit/s\n",
            s->id, s->hevc ? "HEVC" : "H.264", w, h, s->fps, s->kbps);

    if (opt.audio) passthrough_off();
    int ch = s->audio_channels == 6 || s->audio_channels == 8 ? s->audio_channels : 2;
    if (ch != s->audio_channels)
        fprintf(stderr, "moonlight: client wants %d audio channels, sending stereo\n",
                s->audio_channels);
    struct audio_ctx actx = { .channels = ch, .run = 1,
                              .kbps = ch == 8 ? ML_AUDIO_KBPS_71 : ch == 6 ? ML_AUDIO_KBPS_51
                                                                           : ML_AUDIO_KBPS };
    int ms = s->audio_ms == 10 ? 10 : 5;            /* Opus frame sizes Moonlight uses */
    actx.frame = O_RATE / 1000 * ms;
    int audio_started = opt.audio && pthread_create(&actx.th, NULL, audio_thread, &actx) == 0;

    double t_start = now_s(), frame_dt = 1.0 / s->fps, next = 0, t_stat = t_start, enc_time = 0;
    double send_time = 0, send_max = 0;     /* gs_video_send: FEC + paced sending */
    double t_mode = t_start;
    unsigned long frames = 0, bytes = 0, skipped = 0, idrs = 0;
    char mode[32];
    display_mode(mode, sizeof(mode));
    while (!stop && gs_stream_alive(s->id)) {
        if (now_s() - t_mode >= 0.5) {  /* output mode change (e.g. 2160p24 for a 4K film)? */
            char m[32];
            t_mode = now_s();
            display_mode(m, sizeof(m));
            if (strcmp(m, mode)) {
                /* VDIN sets its scaler only at STREAMON: without a restart it keeps the old
                 * geometry and crops (a quarter of a 4K picture). Reopen at the same size. */
                fprintf(stderr, "moonlight: output %s -> %s, restarting capture\n", mode, m);
                snprintf(mode, sizeof(mode), "%s", m);
                capture_close(vfd, bufs);
                memset(bufs, 0, sizeof(bufs));
                for (int i = 0; i < NBUFS; i++) bufs[i].dmafd = -1;
                if ((vfd = capture_open(bufs, w, h)) < 0) {
                    gss_stop("capture failed after an output mode change");
                    break;
                }
                next = 0;
            }
        }
        struct pollfd pfd = { .fd = vfd, .events = POLLIN };
        int pr = poll(&pfd, 1, 200);
        if (pr < 0 && errno == EINTR) continue;
        if (pr <= 0) continue;
        struct v4l2_plane planes[VIDEO_MAX_PLANES] = { 0 };
        struct v4l2_buffer b = { .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
                                 .memory = V4L2_MEMORY_MMAP, .length = VIDEO_MAX_PLANES,
                                 .m.planes = planes };
        if (xioctl(vfd, VIDIOC_DQBUF, &b) < 0) {
            if (errno == EAGAIN) continue;
            perror("VIDIOC_DQBUF");
            break;
        }
        double t_cap = now_s();
        if (next && t_cap < next - frame_dt / 2) {     /* over the client's frame rate */
            skipped++;
            if (xioctl(vfd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); break; }
            continue;
        }
        next = next && t_cap - next < frame_dt ? next + frame_dt : t_cap + frame_dt;

        int idr = gs_idr_requested();
        double t0 = now_s();
        encoding_metadata_t md = encode_buf(enc, &bufs[b.index], planes[0].data_offset, w, h,
                                            idr ? FRAME_TYPE_IDR : FRAME_TYPE_AUTO, out);
        double t1 = now_s();
        enc_time += t1 - t0;
        if (xioctl(vfd, VIDIOC_QBUF, &b) < 0) { perror("VIDIOC_QBUF"); break; }
        if (!md.is_valid) {
            fprintf(stderr, "encode error %d\n", md.err_cod);
            continue;
        }
        if (md.encoded_data_length_in_bytes > 0) {
            uint32_t ts = (uint32_t)((t_cap - t_start) * 90000.0);
            unsigned lat = (unsigned)((t1 - t_cap) * 10000.0);    /* 0.1 ms units */
            gs_video_send(out, md.encoded_data_length_in_bytes, md.is_key_frame, ts, lat);
            double ds = now_s() - t1;
            send_time += ds;
            if (ds > send_max) send_max = ds;
            bytes += md.encoded_data_length_in_bytes;
            idrs += md.is_key_frame;
        }
        frames++;

        if (t1 - t_stat >= 2.0) {
            double dt = t1 - t_stat;
            fprintf(stderr, "moonlight: %.1f fps (%lu skipped), %.0f kbit/s, encode %.1f ms/frame, "
                    "send %.1f ms/frame (max %.1f), %lu IDR | audio %d Hz, %lu pkt (%lu silent), "
                    "overruns %lu\n",
                    frames / dt, skipped, bytes * 8 / dt / 1000,
                    frames ? enc_time / frames * 1000 : 0, frames ? send_time / frames * 1000 : 0,
                    send_max * 1000, idrs, actx.rate, actx.packets, actx.silence, actx.overruns);
            frames = bytes = skipped = idrs = 0;
            actx.packets = actx.bytes = actx.silence = 0;
            enc_time = send_time = send_max = 0;
            t_stat = t1;
        }
    }
    if (audio_started) {
        actx.run = 0;
        pthread_join(actx.th, NULL);
    }
    passthrough_restore("stream ended");
    vl_multi_encoder_destroy(enc);
    if (vfd >= 0) capture_close(vfd, bufs);
    fprintf(stderr, "moonlight: stream %u stopped\n", s->id);
}

static void usage(const char *argv0)
{
    fprintf(stderr,
            "usage: %s [-g GOP] [-i INPUT] [-z 0|1] [-a 0|1] [-H 0|1] [-G DIR] [-F PCT] [-P MBIT]\n"
            "  -g GOP length (IDR interval in frames), default 60\n"
            "  -i VDIN input, default 6 (VPP)\n"
            "  -z 1 = zero-copy dma-buf input if VDIN can export it (default), 0 = copy\n"
            "  -a 1 = Opus audio from " AUDIO_DEV " (default), 0 = video only\n"
            "  -H 1 = offer HEVC besides H.264 (default; the client picks), 0 = H.264 only\n"
            "  -G Moonlight host state dir (key, cert, paired clients), default\n"
            "     /storage/cube-moonlight/moonlight\n"
            "  -F video FEC parity in %% of the data packets, default 20; 0 = off\n"
            "  -P video send pacing in Mbit/s, default 300; 0 = unpaced\n",
            argv0);
    exit(1);
}

int main(int argc, char **argv)
{
    int c;
    while ((c = getopt(argc, argv, "g:i:z:a:H:G:F:P:h")) != -1) {
        switch (c) {
        case 'g': opt.gop = atoi(optarg); break;
        case 'i': opt.input = atoi(optarg); break;
        case 'z': opt.zerocopy = atoi(optarg); break;
        case 'a': opt.audio = atoi(optarg); break;
        case 'H': gs_hevc_offered = atoi(optarg) != 0; break;
        case 'G': opt.gsdir = optarg; break;
        case 'F': opt.fec = atoi(optarg); break;
        case 'P': opt.pace = atoi(optarg); break;
        default: usage(argv[0]);
        }
    }
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    signal(SIGPIPE, SIG_IGN);
    signal(SIGSEGV, on_fatal);
    signal(SIGBUS, on_fatal);
    signal(SIGABRT, on_fatal);
    signal(SIGFPE, on_fatal);
    signal(SIGILL, on_fatal);

    gs_set_net(opt.fec, opt.pace);
    if (gs_start(opt.gsdir) < 0) {
        fprintf(stderr, "cube-stream: Moonlight host failed to start\n");
        return 1;
    }
    passthrough_restore("left over from an earlier run");

    unsigned char *out = malloc(OUTBUF_SIZE);
    if (!out) die("malloc");
    fprintf(stderr, "cube-stream: Moonlight host ready (H.264%s, gop %d, FEC %d %%, pacing %d Mbit/s)\n",
            gs_hevc_offered ? " + HEVC" : "", opt.gop, opt.fec, opt.pace);

    /* VDIN1 and the encoder stay idle until a Moonlight stream starts; one stream at a time */
    while (!stop) {
        struct gs_stream gss;
        if (gs_stream_ready(&gss))
            serve_moonlight(&gss, out);
        else
            usleep(50000);
    }
    free(out);
    return 0;
}
