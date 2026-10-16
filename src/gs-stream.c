/*
 * gs-stream — Moonlight host, streaming part (spec: docs/moonlight-protocol.md §3, §4, §6, §7).
 *
 *   :48010 TCP  RTSP handshake, one request per connection, closed after each response:
 *               OPTIONS, DESCRIBE, SETUP audio/video/control, ANNOUNCE, PLAY. Only while a
 *               launch is active; OPTIONS starts a new stream (ends the previous one).
 *   :47999 UDP  ENet control (cgutman/enet): every message AES-128-GCM wrapped (type 0x0001);
 *               handles IDR requests (0x0302) and liveness, sends termination (0x0109).
 *   :47998 UDP  video: the client's pings carry X-SS-Ping-Payload and give its address;
 *               RTP + NV video header, multi-FEC blocks with Reed-Solomon parity (nanors,
 *               the client's library), sends paced.
 *   input       control message 0x0206 -> gs-input.c (uinput keyboard, mice, gamepads).
 *   :48000 UDP  audio: pings give the address; RTP Opus, AES-128-CBC if negotiated, RS 4+2 FEC.
 *
 * Test hook: env GS_DROP=<percent> drops that share of video and audio datagrams (data and
 * parity) at random before sending, to watch the client recover them.
 *
 * A stream ends on ENet disconnect, 10 s without any sign of the client, /cancel or a new
 * /launch; the encoder side polls gs_stream_alive(). The launch ("app running") survives a
 * disconnect so the client offers Resume, as Sunshine does.
 */
#define _GNU_SOURCE
#include "gamestream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include <enet/enet.h>
#include <openssl/evp.h>
#include <rs.h>

#define RTSP_PORT 48010
#define VIDEO_PORT 47998
#define CONTROL_PORT 47999
#define AUDIO_PORT 48000
#define STREAM_TIMEOUT_S 10.0
#define RTSP_MAX 65536
#define SESSION_ID "DEADBEEFCAFE"
#define TERM_GRACEFUL 0x80030023u

enum { ST_NONE, ST_SETUP, ST_PLAYING };

static struct {
    pthread_mutex_t lock;
    int state;
    struct gs_stream s;
    unsigned next_id;
    char ping_payload[17];
    uint32_t connect_data;
    struct sockaddr_in video_peer, audio_peer;
    int video_peer_ok, audio_peer_ok;
    double last_seen;           /* last sign of life from the client */
    int idr;
    int vsock, asock;
} ss = { .lock = PTHREAD_MUTEX_INITIALIZER, .vsock = -1, .asock = -1 };

static int fec_percent = 20, pace_mbit = 300;
int gs_hevc_offered = 1;
static double drop_share;       /* GS_DROP test hook, 0..1 */

void gs_set_net(int fec, int pace)
{
    fec_percent = fec < 0 ? 0 : fec > 100 ? 100 : fec;
    pace_mbit = pace < 0 ? 0 : pace;
}

static int test_drop(void)
{
    return drop_share > 0 && rand() < drop_share * RAND_MAX;
}

static double now_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

static void put_le16(unsigned char *p, unsigned v) { p[0] = v; p[1] = v >> 8; }
static void put_le32(unsigned char *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }
static void put_be16(unsigned char *p, unsigned v) { p[0] = v >> 8; p[1] = v; }
static void put_be32(unsigned char *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static unsigned get_le16(const unsigned char *p) { return p[0] | p[1] << 8; }
static uint32_t get_le32(const unsigned char *p) { return p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

/* ---------------------------------------------------------------- session state */

static void stream_end_locked(const char *why)
{
    if (ss.state == ST_NONE) return;
    fprintf(stderr, "gs: stream %u ended (%s)\n", ss.s.id, why);
    ss.state = ST_NONE;
    ss.video_peer_ok = ss.audio_peer_ok = 0;
    ss.idr = 0;
}

void gss_stop(const char *why)
{
    pthread_mutex_lock(&ss.lock);
    stream_end_locked(why);
    pthread_mutex_unlock(&ss.lock);
}

static void stream_end_id(unsigned id, const char *why)
{
    pthread_mutex_lock(&ss.lock);
    if (ss.s.id == id) stream_end_locked(why);
    pthread_mutex_unlock(&ss.lock);
}

/* new stream for the current launch (caller holds lock) */
static void stream_begin_locked(const struct gs_launch *L)
{
    stream_end_locked("new RTSP handshake");
    memset(&ss.s, 0, sizeof(ss.s));
    ss.s.id = ++ss.next_id;
    memcpy(ss.s.rikey, L->rikey, 16);
    ss.s.rikeyid = L->rikeyid;
    memcpy(ss.ping_payload, L->ping_payload, sizeof(ss.ping_payload));
    ss.connect_data = L->connect_data;
    ss.video_peer_ok = ss.audio_peer_ok = 0;
    ss.idr = 1;
    ss.last_seen = now_s();
    ss.state = ST_SETUP;
}

int gs_stream_ready(struct gs_stream *out)
{
    pthread_mutex_lock(&ss.lock);
    int ok = ss.state == ST_PLAYING && ss.video_peer_ok;
    if (ok) *out = ss.s;
    pthread_mutex_unlock(&ss.lock);
    return ok;
}

int gs_stream_alive(unsigned id)
{
    pthread_mutex_lock(&ss.lock);
    int ok = ss.state != ST_NONE && ss.s.id == id;
    pthread_mutex_unlock(&ss.lock);
    return ok;
}

int gs_idr_requested(void)
{
    pthread_mutex_lock(&ss.lock);
    int r = ss.idr;
    ss.idr = 0;
    pthread_mutex_unlock(&ss.lock);
    return r;
}

/* ---------------------------------------------------------------- RTSP */

/* integer value of SDP attribute "a=<name>:<value>", def if missing */
static long sdp_int(const char *sdp, const char *name, long def)
{
    size_t nl = strlen(name);
    for (const char *p = strstr(sdp, name); p; p = strstr(p + 1, name)) {
        if (p[nl] == ':' && (p == sdp || p[-1] == '=')) return strtol(p + nl + 1, NULL, 0);
    }
    return def;
}

static void rtsp_reply(int fd, const char *status, int cseq, const char *headers,
                       const char *payload)
{
    char b[2048];
    int n = snprintf(b, sizeof(b), "RTSP/1.0 %s\r\nCSeq: %d\r\n%s\r\n%s", status, cseq,
                     headers ? headers : "", payload ? payload : "");
    if (n > (int)sizeof(b)) n = sizeof(b);
    for (int o = 0; o < n;) {
        ssize_t w = send(fd, b + o, n - o, MSG_NOSIGNAL);
        if (w <= 0) break;
        o += w;
    }
}

static void rtsp_announce(const char *sdp)
{
    struct gs_stream *s = &ss.s;
    s->width = sdp_int(sdp, "x-nv-video[0].clientViewportWd", 1920);
    s->height = sdp_int(sdp, "x-nv-video[0].clientViewportHt", 1080);
    s->fps = sdp_int(sdp, "x-nv-video[0].maxFPS", 60);
    s->packet_size = sdp_int(sdp, "x-nv-video[0].packetSize", 1392);
    s->max_ref_frames = sdp_int(sdp, "x-nv-video[0].maxNumReferenceFrames", 0);
    s->hevc = sdp_int(sdp, "x-nv-vqos[0].bitStreamFormat", 0) == 1;
    s->audio_channels = sdp_int(sdp, "x-nv-audio.surround.numChannels", 2);
    s->audio_ms = sdp_int(sdp, "x-nv-aqos.packetDuration", 5);
    s->enc_flags = sdp_int(sdp, "x-ss-general.encryptionEnabled", 0);
    if (sdp_int(sdp, "x-nv-general.featureFlags", 0) & 0x20) s->enc_flags |= GS_ENC_AUDIO;
    s->fec_min = sdp_int(sdp, "x-nv-vqos[0].fec.minRequiredFecPackets", 0);
    /* as Sunshine: the configured bitrate has to carry the FEC parity, audio (256 kbit/s,
     * at most 20 %) and ~500 kbit/s of packet overhead (at most 10 %) */
    long kbps = sdp_int(sdp, "x-ml-video.configuredBitrateKbps", 0);
    if (kbps > 0) {
        if (fec_percent <= 80) kbps = kbps * (100 - fec_percent) / 100;
        kbps -= 256 < kbps / 5 ? 256 : kbps / 5;
        kbps -= 500 < kbps / 10 ? 500 : kbps / 10;
    } else {
        kbps = sdp_int(sdp, "x-nv-vqos[0].bw.maximumBitrateKbps", 10000);
    }
    s->kbps = kbps < 500 ? 500 : kbps > 80000 ? 80000 : kbps;
    if (s->packet_size < 256 || s->packet_size > 1500) s->packet_size = 1392;
    if (s->fps <= 0 || s->fps > 240) s->fps = 60;
}

static void rtsp_handle(int fd, const char *peer_ip)
{
    static char req[RTSP_MAX + 1];
    size_t n = 0, need = 0;
    char *body = NULL;
    while (n < RTSP_MAX) {
        ssize_t r = recv(fd, req + n, RTSP_MAX - n, 0);
        if (r <= 0) return;
        n += r;
        req[n] = 0;
        if (!body && (body = strstr(req, "\r\n\r\n"))) {
            body += 4;
            char *cl = strcasestr(req, "\r\nContent-length:");
            need = (body - req) + (cl && cl < body ? strtoul(cl + 17, NULL, 10) : 0);
        }
        if (body && n >= need) break;
    }
    if (!body) return;

    char method[16], target[256];
    if (sscanf(req, "%15s %255s", method, target) != 2) return;
    char *c = strcasestr(req, "\r\nCSeq:");
    int cseq = c ? atoi(c + 7) : 0;

    struct gs_launch L;
    gs_get_launch(&L);
    if (!L.active) {
        fprintf(stderr, "gs: rtsp %s from %s without a launch\n", method, peer_ip);
        rtsp_reply(fd, "503 Service Unavailable", cseq, NULL, NULL);
        return;
    }
    fprintf(stderr, "gs: rtsp %s %s from %s\n", method, target, peer_ip);

    pthread_mutex_lock(&ss.lock);
    if (!strcmp(method, "OPTIONS") || ss.state == ST_NONE) stream_begin_locked(&L);
    ss.last_seen = now_s();

    if (!strcmp(method, "OPTIONS")) {
        pthread_mutex_unlock(&ss.lock);
        rtsp_reply(fd, "200 OK", cseq, "Public: OPTIONS, DESCRIBE, SETUP, ANNOUNCE, PLAY\r\n", NULL);
    } else if (!strcmp(method, "DESCRIBE")) {
        pthread_mutex_unlock(&ss.lock);
        /* H.264 (+ HEVC Main if offered: the client looks for this exact sprop line), no
         * RFI; control V2 + audio encryption, no video encryption.
         * Opus layouts <channels><streams><coupled><mapping>, one per channel count (= normal
         * quality only). For 6/8 channels the client rotates the mapping (final[3] =
         * adv[ch-1], final[4..] = adv[3..ch-2]); these strings give the identity mapping the
         * encoder uses (docs/moonlight-protocol.md §5.6). Without an 8-channel line a 7.1 client
         * fails to connect. */
        char sdp[512];
        snprintf(sdp, sizeof(sdp),
                 "a=x-ss-general.featureFlags:0\n"
                 "a=x-ss-general.encryptionSupported:5\n"
                 "a=x-ss-general.encryptionRequested:5\n"
                 "%s"
                 "a=fmtp:97 surround-params=21101\n"
                 "a=fmtp:97 surround-params=642012453\n"
                 "a=fmtp:97 surround-params=85301245673\n",
                 gs_hevc_offered ? "sprop-parameter-sets=AAAAAU\n" : "");
        rtsp_reply(fd, "200 OK", cseq, "Content-Type: application/sdp\r\n", sdp);
    } else if (!strcmp(method, "SETUP")) {
        char *t = strstr(target, "streamid=");
        char h[256];
        int len = 0;
        if (t && !strncmp(t + 9, "audio", 5))
            len = snprintf(h, sizeof(h), "Transport: server_port=%d\r\nX-SS-Ping-Payload: %s\r\n",
                           AUDIO_PORT, ss.ping_payload);
        else if (t && !strncmp(t + 9, "video", 5))
            len = snprintf(h, sizeof(h), "Transport: server_port=%d\r\nX-SS-Ping-Payload: %s\r\n",
                           VIDEO_PORT, ss.ping_payload);
        else if (t && !strncmp(t + 9, "control", 7))
            len = snprintf(h, sizeof(h), "Transport: server_port=%d\r\nX-SS-Connect-Data: %u\r\n",
                           CONTROL_PORT, ss.connect_data);
        pthread_mutex_unlock(&ss.lock);
        if (!len) {
            rtsp_reply(fd, "404 Not Found", cseq, NULL, NULL);
            return;
        }
        snprintf(h + len, sizeof(h) - len, "Session: " SESSION_ID ";timeout = 90\r\n");
        rtsp_reply(fd, "200 OK", cseq, h, NULL);
    } else if (!strcmp(method, "ANNOUNCE")) {
        int fmt = sdp_int(body, "x-nv-vqos[0].bitStreamFormat", 0);
        if ((fmt != 0 && !(fmt == 1 && gs_hevc_offered)) ||
            sdp_int(body, "x-nv-video[0].dynamicRangeMode", 0) != 0) {
            pthread_mutex_unlock(&ss.lock);
            fprintf(stderr, "gs: client wants format %d / HDR, not offered\n", fmt);
            rtsp_reply(fd, "400 BAD REQUEST", cseq, NULL, NULL);
            return;
        }
        rtsp_announce(body);
        struct gs_stream s = ss.s;
        pthread_mutex_unlock(&ss.lock);
        fprintf(stderr, "gs: stream %u: %s %dx%d@%d, %d kbit/s, packet %d, refs %d, audio %d ch "
                "%d ms, encryption 0x%x, fec %d %% (min %d), pacing %d Mbit/s\n", s.id,
                s.hevc ? "HEVC" : "H.264", s.width, s.height, s.fps, s.kbps, s.packet_size, s.max_ref_frames, s.audio_channels,
                s.audio_ms, s.enc_flags, fec_percent, s.fec_min, pace_mbit);
        rtsp_reply(fd, "200 OK", cseq, NULL, NULL);
    } else if (!strcmp(method, "PLAY")) {
        ss.state = ST_PLAYING;
        pthread_mutex_unlock(&ss.lock);
        rtsp_reply(fd, "200 OK", cseq, NULL, NULL);
    } else {
        pthread_mutex_unlock(&ss.lock);
        rtsp_reply(fd, "501 Not Implemented", cseq, NULL, NULL);
    }
}

static void *rtsp_thread(void *arg)
{
    (void)arg;
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(RTSP_PORT),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 8) < 0) {
        fprintf(stderr, "gs: tcp port %d: %s\n", RTSP_PORT, strerror(errno));
        return NULL;
    }
    for (;;) {      /* one short request per connection: handled in turn */
        struct sockaddr_in peer;
        socklen_t pl = sizeof(peer);
        int fd = accept(ls, (struct sockaddr *)&peer, &pl);
        if (fd < 0) continue;
        struct timeval tv = { .tv_sec = 5 };
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &peer.sin_addr, ip, sizeof(ip));
        rtsp_handle(fd, ip);
        shutdown(fd, SHUT_RDWR);    /* the client reads until close */
        close(fd);
    }
    return NULL;
}

/* ---------------------------------------------------------------- UDP pings */

static void *ping_thread(void *arg)
{
    (void)arg;
    struct pollfd p[2] = { { .fd = ss.vsock, .events = POLLIN }, { .fd = ss.asock, .events = POLLIN } };
    unsigned char b[1500];
    for (;;) {
        if (poll(p, 2, -1) <= 0) continue;
        for (int i = 0; i < 2; i++) {
            if (!(p[i].revents & POLLIN)) continue;
            struct sockaddr_in from;
            socklen_t fl = sizeof(from);
            ssize_t n = recvfrom(p[i].fd, b, sizeof(b), 0, (struct sockaddr *)&from, &fl);
            if (n <= 0) continue;
            pthread_mutex_lock(&ss.lock);
            int match = ss.state != ST_NONE &&
                        ((n >= 16 && memmem(b, n, ss.ping_payload, 16)) ||
                         (n == 4 && !memcmp(b, "PING", 4)));
            if (match) {
                struct sockaddr_in *peer = i ? &ss.audio_peer : &ss.video_peer;
                int *ok = i ? &ss.audio_peer_ok : &ss.video_peer_ok;
                if (!*ok || peer->sin_port != from.sin_port || peer->sin_addr.s_addr != from.sin_addr.s_addr) {
                    char ip[INET_ADDRSTRLEN];
                    inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                    fprintf(stderr, "gs: %s ping from %s:%d\n", i ? "audio" : "video", ip,
                            ntohs(from.sin_port));
                }
                *peer = from;
                *ok = 1;
                ss.last_seen = now_s();
            }
            pthread_mutex_unlock(&ss.lock);
        }
    }
    return NULL;
}

static int udp_bind(int port)
{
    int s = socket(AF_INET, SOCK_DGRAM, 0), one = 1, buf = 1 << 20;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_SNDBUF, &buf, sizeof(buf));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0) {
        fprintf(stderr, "gs: udp port %d: %s\n", port, strerror(errno));
        close(s);
        return -1;
    }
    return s;
}

/* ---------------------------------------------------------------- control (ENet) */

/* AES-128-GCM, no AAD. V2: 12-byte IV = seq LE, 0..., dir, 'C'; V1: 16-byte IV, iv[0] = seq */
static int gcm(int enc, const unsigned char key[16], int v2, uint32_t seq, char dir,
               const unsigned char *in, int n, unsigned char *out, unsigned char tag[16])
{
    unsigned char iv[16] = { 0 };
    int ivlen = 16;
    if (v2) {
        put_le32(iv, seq);
        iv[10] = dir;
        iv[11] = 'C';
        ivlen = 12;
    } else {
        iv[0] = seq;
    }
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int l = 0, l2 = 0;
    int ok = c && EVP_CipherInit_ex(c, EVP_aes_128_gcm(), NULL, NULL, NULL, enc) &&
             EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_IVLEN, ivlen, NULL) &&
             EVP_CipherInit_ex(c, NULL, NULL, key, iv, enc) &&
             (enc || EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_SET_TAG, 16, tag)) &&
             EVP_CipherUpdate(c, out, &l, in, n) && EVP_CipherFinal_ex(c, out + l, &l2) &&
             (!enc || EVP_CIPHER_CTX_ctrl(c, EVP_CTRL_GCM_GET_TAG, 16, tag));
    EVP_CIPHER_CTX_free(c);
    return ok ? 0 : -1;
}

struct ctl {
    ENetHost *host;
    ENetPeer *peer;
    unsigned id;                /* stream the peer belongs to */
    unsigned char key[16];
    int v2;
    uint32_t tx_seq;
};

static void ctl_send(struct ctl *k, unsigned type, const unsigned char *payload, unsigned plen)
{
    unsigned char pt[64], wire[64 + 24];
    if (plen > sizeof(pt) - 4) return;
    put_le16(pt, type);
    put_le16(pt + 2, plen);
    memcpy(pt + 4, payload, plen);
    put_le16(wire, 0x0001);
    put_le16(wire + 2, 4 + 16 + 4 + plen);
    put_le32(wire + 4, k->tx_seq);
    if (gcm(1, k->key, k->v2, k->tx_seq, 'H', pt, 4 + plen, wire + 24, wire + 8) < 0) return;
    k->tx_seq++;
    ENetPacket *p = enet_packet_create(wire, 24 + 4 + plen, ENET_PACKET_FLAG_RELIABLE);
    if (p && enet_peer_send(k->peer, 0, p) < 0) enet_packet_destroy(p);
}

static void ctl_terminate(struct ctl *k)
{
    unsigned char code[4];
    put_be32(code, TERM_GRACEFUL);
    ctl_send(k, 0x0109, code, 4);
    enet_host_flush(k->host);
    enet_peer_disconnect_later(k->peer, 0);
    fprintf(stderr, "gs: control: termination sent to stream %u\n", k->id);
    k->peer = NULL;
    gs_input_reset();
}

static void ctl_receive(struct ctl *k, const unsigned char *p, size_t n)
{
    unsigned char pt[2048];
    if (n < 24 || get_le16(p) != 0x0001) return;
    unsigned len = get_le16(p + 2);
    if (len < 20 || len + 4 > n || len - 20 > sizeof(pt)) return;
    unsigned char tag[16];
    memcpy(tag, p + 8, 16);
    if (gcm(0, k->key, k->v2, get_le32(p + 4), 'C', p + 24, len - 20, pt, tag) < 0) {
        fprintf(stderr, "gs: control: message failed to decrypt (seq %u)\n", get_le32(p + 4));
        return;
    }
    if (len - 20 < 4) return;
    unsigned type = get_le16(pt);
    pthread_mutex_lock(&ss.lock);
    if (ss.s.id == k->id) {
        ss.last_seen = now_s();
        if (type == 0x0302) ss.idr = 1;
    }
    pthread_mutex_unlock(&ss.lock);
    switch (type) {
    case 0x0302: fprintf(stderr, "gs: control: IDR requested\n"); break;
    case 0x0200: case 0x0307: case 0x5502: case 0x0201: case 0x0204: break;
    case 0x0206: {                                /* LE16 len, then the input packet */
        unsigned plen = get_le16(pt + 2);
        if (plen <= len - 24) gs_input(pt + 4, plen);
        break;
    }
    default: fprintf(stderr, "gs: control: message type 0x%04x (%u bytes)\n", type, len - 24);
    }
}

static void *control_thread(void *arg)
{
    struct ctl *k = arg;
    for (;;) {
        ENetEvent ev;
        int r = enet_host_service(k->host, &ev, 50);
        if (r > 0 && ev.type == ENET_EVENT_TYPE_CONNECT) {
            char ip[64] = "?";
            struct sockaddr_in *sa = (struct sockaddr_in *)&ev.peer->address.address;
            inet_ntop(AF_INET, &sa->sin_addr, ip, sizeof(ip));
            pthread_mutex_lock(&ss.lock);
            int ok = ss.state != ST_NONE && (ev.data == ss.connect_data || ev.data == 0);
            unsigned id = ss.s.id;
            if (ok) {
                memcpy(k->key, ss.s.rikey, 16);
                k->v2 = ss.s.enc_flags & GS_ENC_CONTROL_V2;
                ss.last_seen = now_s();
            }
            pthread_mutex_unlock(&ss.lock);
            if (!ok) {
                fprintf(stderr, "gs: control: refused connect from %s (data %u)\n", ip, ev.data);
                enet_peer_disconnect_now(ev.peer, 0);
                continue;
            }
            if (k->peer && k->peer != ev.peer) enet_peer_disconnect_now(k->peer, 0);
            k->peer = ev.peer;
            k->id = id;
            k->tx_seq = 0;
            fprintf(stderr, "gs: control: client %s connected to stream %u (%s IV)\n", ip, id,
                    k->v2 ? "V2" : "V1");
        } else if (r > 0 && ev.type == ENET_EVENT_TYPE_RECEIVE) {
            if (ev.peer == k->peer) ctl_receive(k, ev.packet->data, ev.packet->dataLength);
            enet_packet_destroy(ev.packet);
        } else if (r > 0 && ev.type == ENET_EVENT_TYPE_DISCONNECT) {
            if (ev.peer == k->peer) {
                k->peer = NULL;
                gs_input_reset();
                stream_end_id(k->id, "client disconnected");
            }
        }

        pthread_mutex_lock(&ss.lock);
        if (ss.state != ST_NONE && now_s() - ss.last_seen > STREAM_TIMEOUT_S)
            stream_end_locked("client timed out");
        pthread_mutex_unlock(&ss.lock);
        if (k->peer && !gs_stream_alive(k->id)) ctl_terminate(k);
    }
    return NULL;
}

/* ---------------------------------------------------------------- video */

#define VHDR 32                 /* RTP 12 + reserved 4 + NV_VIDEO_PACKET 16 */
#define BATCH 64
#define PACE_BATCH 16           /* datagrams per paced burst */

/* RTP header, frameIndex, multiFecBlocks and fecInfo of shard d. The client rebuilds exactly
 * these bytes for recovered data shards, so they are the only ones parity shards may get
 * after encoding (docs/moonlight-protocol.md §4.4). */
static void video_header(unsigned char *d, uint32_t pkt, uint32_t ts90k, uint32_t frame,
                         int block, int blocks, int shard, int data, int pct)
{
    d[0] = 0x90;                            /* RTP v2 + extension bit */
    d[1] = 0;
    put_be16(d + 2, (uint16_t)pkt);
    put_be32(d + 4, ts90k);
    put_be32(d + 8, 0);
    put_le32(d + 20, frame);
    d[27] = block << 4 | (blocks - 1) << 6;
    put_le32(d + 28, (uint32_t)shard << 12 | (uint32_t)data << 22 | (uint32_t)pct << 4);
}

/* parity shards for `data` data shards: Sunshine's rule, then the percentage the client
 * turns back into the same count with (data * pct + 99) / 100 */
static int video_parity(int data, int pct, int fec_min, int *pct_out)
{
    *pct_out = 0;
    if (!pct) return 0;
    int parity = (data * pct + 99) / 100;
    if (parity < fec_min) parity = fec_min;
    for (; parity > 0; parity--) {
        int p = 100 * parity / data;
        if (data + parity <= DATA_SHARDS_MAX && p <= 255 && (data * p + 99) / 100 == parity) {
            *pct_out = p;
            return parity;
        }
    }
    return 0;
}

static void sleep_until(double t)
{
    struct timespec ts = { .tv_sec = (time_t)t, .tv_nsec = (long)((t - (time_t)t) * 1e9) };
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &ts, NULL) == EINTR) {}
}

int gs_video_send(const unsigned char *au, size_t n, int idr, uint32_t ts90k, unsigned latency)
{
    static unsigned cur_id;
    /* packet counter: RTP seq = its low 16 bits, streamPacketIndex = 24 bits (as Sunshine's int
     * lowseq). Deriving the index from the 16-bit seq made the client report every frame as
     * corrupt after the first wrap (65536 packets, ~2 min of film at 20 Mbit/s). */
    static uint32_t pkt;
    static uint32_t frame;
    static unsigned char *buf;
    static size_t cap;
    static struct mmsghdr msgs[BATCH];
    static struct iovec iov[BATCH];
    static _Alignas(16) unsigned char rsbuf[sizeof(reed_solomon) + DATA_SHARDS_MAX * DATA_SHARDS_MAX];

    pthread_mutex_lock(&ss.lock);
    int ok = ss.state == ST_PLAYING && ss.video_peer_ok;
    struct sockaddr_in to = ss.video_peer;
    int P = ss.s.packet_size - 16;          /* payload bytes per datagram */
    int fps = ss.s.fps, fec_min = ss.s.fec_min;
    unsigned id = ss.s.id;
    pthread_mutex_unlock(&ss.lock);
    if (!ok) return -1;
    if (id != cur_id) {
        cur_id = id;
        /* test hook, e.g. 65300 = 16-bit wrap within seconds; moonlight-qt drops everything
         * before the wrap then (its RTP queue expects seq to start near 0, as Sunshine's does) */
        const char *e = getenv("GS_PKT_START");
        pkt = e ? strtoul(e, NULL, 0) : 0;
        frame = 1;
    }

    /* FEC blocks: nanors allows 255 shards (data + parity) per block, the header 4 blocks.
     * A frame too large for that goes without parity (Sunshine does the same). */
    size_t total = 8 + n;                   /* frame header + access unit */
    int shards = (total + P - 1) / P;
    int pct = fec_percent;
    int max_data = pct ? DATA_SHARDS_MAX * 100 / (100 + pct) : DATA_SHARDS_MAX;
    int blocks = (shards + max_data - 1) / max_data;
    if (blocks > 4) {
        pct = 0;
        blocks = (shards + DATA_SHARDS_MAX - 1) / DATA_SHARDS_MAX;
        if (blocks > 4) blocks = 4;
    }
    int per = (shards + blocks - 1) / blocks;
    if (per > 1023) {
        fprintf(stderr, "gs: video frame too large (%zu bytes), dropped\n", n);
        frame++;                            /* the gap makes the client ask for an IDR */
        return -1;
    }
    int bdata[4], bpar[4], bpct[4], count = 0;
    for (int b = 0; b < blocks; b++) {
        bdata[b] = b == blocks - 1 ? shards - b * per : per;
        bpar[b] = video_parity(bdata[b], pct, fec_min, &bpct[b]);
        count += bdata[b] + bpar[b];
    }
    size_t dg = VHDR + P;
    if (cap < count * dg) {
        free(buf);
        cap = count * dg;
        buf = malloc(cap);
        if (!buf) { cap = 0; return -1; }
    }

    unsigned lpl = total % P ? total % P : P;
    unsigned char fh[8] = { 0x01, (unsigned char)latency, (unsigned char)(latency >> 8),
                            idr ? 2 : 1, (unsigned char)lpl, (unsigned char)(lpl >> 8), 0, 0 };
    unsigned char *d = buf;
    for (int b = 0, i = 0; b < blocks; b++) {   /* i = data shard index in the frame */
        unsigned char *first = d, *sh[DATA_SHARDS_MAX];
        for (int x = 0; x < bdata[b]; x++, i++, d += dg) {
            memset(d, 0, VHDR);
            video_header(d, pkt, ts90k, frame, b, blocks, x, bdata[b], bpct[b]);
            put_le32(d + 16, pkt << 8);
            d[24] = 0x01 | (x == 0 ? 0x04 : 0) | (x == bdata[b] - 1 ? 0x02 : 0);   /* PIC, SOF, EOF */
            d[26] = 0x10;

            unsigned char *o = d + VHDR;    /* payload: bytes [i*P, i*P+P) of fh + au */
            size_t off = (size_t)i * P, left = P;
            if (off < 8) {
                size_t k = 8 - off < left ? 8 - off : left;
                memcpy(o, fh + off, k);
                o += k; left -= k; off += k;
            }
            size_t a = off - 8, k = a < n ? n - a : 0;
            if (k > left) k = left;
            memcpy(o, au + a, k);
            memset(o + k, 0, left - k);     /* zero padding: harmless after Annex B data */
            pkt++;
        }
        if (!bpar[b]) continue;
        /* parity over whole datagrams; then the parity shards get their own headers */
        int ts_ = bdata[b] + bpar[b];
        for (int x = 0; x < ts_; x++) sh[x] = first + x * dg;
        reed_solomon *rs = reed_solomon_new_static(rsbuf, sizeof(rsbuf), bdata[b], bpar[b]);
        if (!rs || reed_solomon_encode(rs, sh, ts_, dg) < 0) {
            fprintf(stderr, "gs: video FEC encode failed (%d+%d)\n", bdata[b], bpar[b]);
            return -1;
        }
        for (int x = bdata[b]; x < ts_; x++, d += dg) {
            video_header(d, pkt, ts90k, frame, b, blocks, x, bdata[b], bpct[b]);
            pkt++;
        }
    }

    /* pacing: at most pace_mbit, but the whole frame within half a frame interval */
    double rate = pace_mbit * 1e6 / 8, t0 = now_s(), sent_bytes = 0;
    if (rate > 0 && fps > 0 && rate < count * dg * 2.0 * fps) rate = count * dg * 2.0 * fps;
    int batch = rate > 0 ? PACE_BATCH : BATCH;
    for (int i = 0; i < count; i += batch) {
        int m = count - i < batch ? count - i : batch, k = 0;
        if (rate > 0 && i) sleep_until(t0 + sent_bytes / rate);
        for (int j = 0; j < m; j++) {
            if (test_drop()) continue;
            iov[k].iov_base = buf + (i + j) * dg;
            iov[k].iov_len = dg;
            memset(&msgs[k].msg_hdr, 0, sizeof(msgs[k].msg_hdr));
            msgs[k].msg_hdr.msg_name = &to;
            msgs[k].msg_hdr.msg_namelen = sizeof(to);
            msgs[k].msg_hdr.msg_iov = &iov[k];
            msgs[k].msg_hdr.msg_iovlen = 1;
            k++;
        }
        for (int sent = 0; sent < k;) {
            int r = sendmmsg(ss.vsock, msgs + sent, k - sent, 0);
            if (r < 0) {
                if (errno == EINTR) continue;
                fprintf(stderr, "gs: video send: %s\n", strerror(errno));
                frame++;
                return -1;
            }
            sent += r;
        }
        sent_bytes += m * dg;
    }
    frame++;
    return 0;
}

/* ---------------------------------------------------------------- audio */

/* RTP PT 97: BE16 seq from 0, BE32 timestamp in ms (+= packet duration), ssrc 0. Payload =
 * Opus, or AES-128-CBC/PKCS#7 of it (IV = BE32(rikeyid + seq), rest 0) when audio encryption
 * is on (docs/moonlight-protocol.md §5.2, §5.4).
 * FEC (§5.5): after every 4 data packets whose first seq is a multiple of 4, 2 RS parity packets
 * (PT 127, seq = last data seq + 1 + shard) over the 4 payloads, with GFE's fixed parity matrix.
 * Needs equal payload sizes (CBR); a block with different sizes goes without parity. */
#define AFEC_DATA 4
#define AFEC_PARITY 2
#define AFEC_MAX (1400 + 16)

int gs_audio_send(const unsigned char *opus, int n)
{
    static const unsigned char matrix[AFEC_DATA * AFEC_PARITY] =
        { 0x77, 0x40, 0x38, 0x0e, 0xc7, 0xa7, 0x0d, 0x6c };
    static unsigned cur_id;
    static uint16_t seq;
    static uint32_t ts;
    static reed_solomon *rs;
    static _Alignas(16) unsigned char shard[AFEC_DATA + AFEC_PARITY][AFEC_MAX];
    static int shard_len[AFEC_DATA];
    static unsigned have;               /* bit x: data shard x of the current block stored */

    pthread_mutex_lock(&ss.lock);
    int ok = ss.state == ST_PLAYING && ss.audio_peer_ok;
    struct sockaddr_in to = ss.audio_peer;
    unsigned id = ss.s.id;
    int enc = ss.s.enc_flags & GS_ENC_AUDIO, ms = ss.s.audio_ms;
    unsigned char key[16];
    memcpy(key, ss.s.rikey, 16);
    uint32_t keyid = (uint32_t)ss.s.rikeyid;
    pthread_mutex_unlock(&ss.lock);
    if (!ok || n <= 0 || n > 1400) return -1;
    if (id != cur_id) {
        cur_id = id;
        seq = 0;
        ts = 0;
        have = 0;
    }
    if (!rs && (rs = reed_solomon_new(AFEC_DATA, AFEC_PARITY)))
        memcpy(rs->p, matrix, sizeof(matrix));

    unsigned char p[12 + AFEC_MAX];
    p[0] = 0x80;
    p[1] = 97;
    put_be16(p + 2, seq);
    put_be32(p + 4, ts);
    put_be32(p + 8, 0);
    int len = n;
    if (enc) {
        unsigned char iv[16] = { 0 };
        put_be32(iv, keyid + seq);
        EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
        int l1 = 0, l2 = 0;
        int good = c && EVP_EncryptInit_ex(c, EVP_aes_128_cbc(), NULL, key, iv) &&
                   EVP_EncryptUpdate(c, p + 12, &l1, opus, n) &&
                   EVP_EncryptFinal_ex(c, p + 12 + l1, &l2);    /* PKCS#7 padding (default) */
        EVP_CIPHER_CTX_free(c);
        if (!good) return -1;
        len = l1 + l2;
    } else {
        memcpy(p + 12, opus, n);
    }
    int r = test_drop() ? 0 : sendto(ss.asock, p, 12 + len, 0, (struct sockaddr *)&to, sizeof(to));

    int x = seq % AFEC_DATA;
    if (x == 0) have = 0;
    memcpy(shard[x], p + 12, len);
    shard_len[x] = len;
    have |= 1u << x;
    if (x == AFEC_DATA - 1 && rs && have == (1u << AFEC_DATA) - 1) {
        int same = 1;
        for (int i = 1; i < AFEC_DATA; i++) same &= shard_len[i] == len;
        uint8_t *sh[AFEC_DATA + AFEC_PARITY];
        for (int i = 0; i < AFEC_DATA + AFEC_PARITY; i++) sh[i] = shard[i];
        if (same && reed_solomon_encode(rs, sh, AFEC_DATA + AFEC_PARITY, len) == 0) {
            for (int i = 0; i < AFEC_PARITY; i++) {
                unsigned char f[12 + 12 + AFEC_MAX];
                f[0] = 0x80;
                f[1] = 127;
                put_be16(f + 2, (uint16_t)(seq + 1 + i));
                put_be32(f + 4, 0);
                put_be32(f + 8, 0);
                f[12] = i;                                  /* AUDIO_FEC_HEADER */
                f[13] = 97;
                put_be16(f + 14, (uint16_t)(seq - (AFEC_DATA - 1)));
                put_be32(f + 16, ts - (AFEC_DATA - 1) * ms);
                put_be32(f + 20, 0);
                memcpy(f + 24, shard[AFEC_DATA + i], len);
                if (!test_drop()) sendto(ss.asock, f, 24 + len, 0, (struct sockaddr *)&to, sizeof(to));
            }
        }
    }
    seq++;
    ts += ms;
    return r < 0 ? -1 : 0;
}

/* ---------------------------------------------------------------- start */

int gss_start(void)
{
    static struct ctl k;
    reed_solomon_init();
    const char *e = getenv("GS_DROP");
    if (e && atof(e) > 0) {
        drop_share = atof(e) / 100;
        srand(time(NULL));
        fprintf(stderr, "gs: test hook: dropping %.2f %% of video/audio datagrams\n", drop_share * 100);
    }
    if ((ss.vsock = udp_bind(VIDEO_PORT)) < 0 || (ss.asock = udp_bind(AUDIO_PORT)) < 0)
        return -1;
    if (enet_initialize()) {
        fprintf(stderr, "gs: enet_initialize failed\n");
        return -1;
    }
    ENetAddress a;
    memset(&a, 0, sizeof(a));
    enet_address_set_host(&a, "0.0.0.0");
    enet_address_set_port(&a, CONTROL_PORT);
    k.host = enet_host_create(AF_INET, &a, 4, 0, 0, 0);
    if (!k.host) {
        fprintf(stderr, "gs: udp port %d: enet_host_create failed\n", CONTROL_PORT);
        return -1;
    }
    pthread_t th;
    if (pthread_create(&th, NULL, rtsp_thread, NULL) ||
        pthread_create(&th, NULL, ping_thread, NULL) ||
        pthread_create(&th, NULL, control_thread, &k))
        return -1;
    fprintf(stderr, "gs: streaming ports: rtsp tcp %d, video udp %d, control udp %d, audio udp %d\n",
            RTSP_PORT, VIDEO_PORT, CONTROL_PORT, AUDIO_PORT);
    return 0;
}
