/*
 * gamestream — Moonlight (GameStream / Sunshine protocol) host front end for cube-stream.
 * Spec: docs/moonlight-protocol.md. HTTP :47989 + HTTPS :47984 (serverinfo, pairing, applist,
 * launch/resume/cancel), RTSP :48010, ENet control :47999 (AES-GCM), RTP video :47998
 * (Reed-Solomon FEC, paced), RTP Opus audio :48000 (encrypted, RS 4+2 FEC).
 */
#ifndef GAMESTREAM_H
#define GAMESTREAM_H
#include <stddef.h>
#include <stdint.h>

#define GS_APP_ID 1             /* the only app: "Kodi" (= the whole screen) */

struct gs_launch {
    int active;                 /* an app "runs" (between /launch and /cancel) */
    int appid;
    int width, height, fps;     /* requested mode */
    unsigned char rikey[16];    /* session AES key (control, audio) */
    int32_t rikeyid;            /* audio IV base */
    int surround_info;          /* channelMask << 16 | channelCount */
    int local_audio;            /* 1 = keep playing on the host */
    char ping_payload[17];      /* X-SS-Ping-Payload, 16 ASCII chars */
    uint32_t connect_data;      /* X-SS-Connect-Data */
    unsigned generation;        /* +1 on every /launch or /resume (new keys) */
};

/* Stream parameters negotiated by RTSP ANNOUNCE (one streaming session). */
struct gs_stream {
    unsigned id;                /* +1 per ANNOUNCE; gs_stream_alive(id) tells when it ends */
    int width, height, fps;
    int kbps;                   /* video bitrate for the encoder */
    int packet_size;            /* video payload unit (x-nv-video[0].packetSize) */
    int max_ref_frames;         /* 0 = any */
    int hevc;                   /* 1 = HEVC Main (x-nv-vqos[0].bitStreamFormat 1), 0 = H.264 */
    int fec_min;                /* minimum parity shards per FEC block (0 = none) */
    int audio_channels, audio_ms;
    int enc_flags;              /* GS_ENC_* in use */
    unsigned char rikey[16];
    int32_t rikeyid;
};
#define GS_ENC_CONTROL_V2 0x01
#define GS_ENC_VIDEO 0x02
#define GS_ENC_AUDIO 0x04

/* Network options, before gs_start: video FEC percentage (0..100, default 20 as Sunshine)
 * and the send pacing rate in Mbit/s (0 = unpaced; default 300; a frame is still always sent
 * within half a frame interval). */
void gs_set_net(int fec_percent, int pace_mbit);

/* Offer HEVC Main besides H.264 (serverinfo, DESCRIBE); the client picks. Set before gs_start;
 * default 1. */
extern int gs_hevc_offered;

/* Loads or creates the host identity in state_dir (key.pem, cert.pem, uniqueid, paired.pem)
 * and starts the HTTP, HTTPS, RTSP, control and UDP threads. Returns 0 on success. */
int gs_start(const char *state_dir);

/* Copy of the current launch state (thread-safe). */
void gs_get_launch(struct gs_launch *out);

/* 1 + *out filled when a session is ready for video (PLAY done, client's video ping seen). */
int gs_stream_ready(struct gs_stream *out);

/* 0 once stream `id` has ended (client gone, quit, timeout, new launch). */
int gs_stream_alive(unsigned id);

/* 1 if the client asked for an IDR frame since the last call (clears the request). */
int gs_idr_requested(void);

/* Packetizes one H.264 access unit (Annex B, SPS+PPS in front of IDRs) and sends it to the
 * client. ts90k = capture time (90 kHz), latency = capture -> sent in 0.1 ms units. */
int gs_video_send(const unsigned char *au, size_t n, int idr, uint32_t ts90k, unsigned latency);

/* Sends one Opus packet (s.audio_ms long, 48 kHz) to the client: RTP, encrypted if negotiated.
 * Returns -1 while no stream / no audio ping yet. */
int gs_audio_send(const unsigned char *opus, int n);

/* gs-mdns.c: announce "_nvstream._tcp" on the LAN via the system's avahi-daemon (dlopen'ed
 * libavahi-client); called by gs_start. 0 = announcing (or waiting for the daemon), -1 = no
 * Avahi (logged; clients then add the host by IP). */
int gs_mdns_start(const char *service_name, int http_port);

/* Called by the HTTP side on /cancel; set before gs_start. */
extern void (*gs_on_cancel)(void);

/* internal: gs-stream.c (control thread) -> gs-input.c (uinput). p = the input packet
 * (NV_INPUT_HEADER + body) from a 0x0206 control message; reset releases everything held. */
void gs_input(const unsigned char *p, size_t n);
void gs_input_reset(void);

/* internal: gamestream.c (HTTP) -> gs-stream.c (RTSP, control, UDP) */
int gss_start(void);
void gss_stop(const char *why);

#endif
