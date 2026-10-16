/*
 * gamestream — Moonlight host, HTTP part (spec: docs/moonlight-protocol.md §2); the streaming part
 * (RTSP, control, video) is in gs-stream.c.
 *
 *   :47989 HTTP   /serverinfo, /pair (phases 1-4)
 *   :47984 HTTPS  mutual TLS; the client cert must be in paired.pem: /serverinfo, /pair
 *                 (pairchallenge), /applist, /launch, /resume, /cancel
 *
 * Identity in the state dir (created on first start, never regenerate: clients pin the cert):
 *   key.pem + cert.pem (RSA-2048, self-signed, SHA-256), uniqueid, paired.pem (client certs).
 * Pairing: the client shows a 4-digit PIN; the user writes it to <state dir>/pin
 *   /storage/cube-moonlight/pin.sh 1234   (= echo 1234 > <state dir>/pin)
 * and the held phase-1 request continues (the client waits without a timeout).
 * One thread per connection; every response closes the connection (as Sunshine does).
 */
#define _GNU_SOURCE
#include "gamestream.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rand.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>

#define HTTP_PORT 47989
#define HTTPS_PORT 47984
#define RTSP_PORT 48010
#define APPVERSION "7.1.431.-1"
#define GFEVERSION "3.23.0.74"
#define HOSTNAME "FireCube"
#define PIN_WAIT_S 300
#define REQ_MAX 16384

void (*gs_on_cancel)(void);

static struct {
    char dir[200];
    char uniqueid[40];
    EVP_PKEY *key;
    X509 *cert;
    char *cert_pem;             /* PEM text of cert, sent as plaincert */
    size_t cert_pem_len;
    SSL_CTX *tls;
    pthread_mutex_t lock;       /* guards paired, launch, pair */
    STACK_OF(X509) *paired;
    struct gs_launch launch;
    struct {                    /* the one pairing in progress */
        int phase;              /* last completed phase, 0 = none */
        char client_id[128];
        unsigned char aes[16];
        X509 *client;
        unsigned char serversecret[16], serverchallenge[16], clienthash[32];
    } pair;
} gs = { .lock = PTHREAD_MUTEX_INITIALIZER };

struct conn {
    int fd;
    SSL *ssl;
    int authorized;             /* HTTPS with a paired client cert */
    char local_ip[INET_ADDRSTRLEN], peer_ip[INET_ADDRSTRLEN];
};

static void ssl_log(const char *what)
{
    unsigned long e = ERR_get_error();
    fprintf(stderr, "gs: %s: %s\n", what, e ? ERR_error_string(e, NULL) : "?");
}

/* ---------------------------------------------------------------- helpers */

static void hex_encode(char *out, const unsigned char *in, size_t n)
{
    static const char d[] = "0123456789ABCDEF";
    for (size_t i = 0; i < n; i++) {
        out[2 * i] = d[in[i] >> 4];
        out[2 * i + 1] = d[in[i] & 15];
    }
    out[2 * n] = 0;
}

static int hexval(int c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* returns decoded length, -1 on bad input or if it doesn't fit */
static int hex_decode(unsigned char *out, size_t max, const char *in)
{
    size_t n = strlen(in);
    if (n % 2 || n / 2 > max) return -1;
    for (size_t i = 0; i < n / 2; i++) {
        int h = hexval(in[2 * i]), l = hexval(in[2 * i + 1]);
        if (h < 0 || l < 0) return -1;
        out[i] = h << 4 | l;
    }
    return n / 2;
}

/* query parameter `name` (url-decoded) into out; returns 1 if present */
static int qget(const char *q, const char *name, char *out, size_t n)
{
    size_t nl = strlen(name);
    for (const char *p = q; p && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, name, nl) || p[nl] != '=') continue;
        const char *v = p + nl + 1;
        size_t o = 0;
        while (*v && *v != '&' && o + 1 < n) {
            if (*v == '%' && hexval(v[1]) >= 0 && hexval(v[2]) >= 0) {
                out[o++] = hexval(v[1]) << 4 | hexval(v[2]);
                v += 3;
            } else {
                out[o++] = *v == '+' ? ' ' : *v;
                v++;
            }
        }
        out[o] = 0;
        return 1;
    }
    return 0;
}

static int sha256(unsigned char out[32], const void *a, size_t an, const void *b, size_t bn,
                  const void *c, size_t cn)
{
    EVP_MD_CTX *m = EVP_MD_CTX_new();
    unsigned int len = 32;
    int ok = m && EVP_DigestInit_ex(m, EVP_sha256(), NULL) && EVP_DigestUpdate(m, a, an) &&
             EVP_DigestUpdate(m, b, bn) && EVP_DigestUpdate(m, c, cn) &&
             EVP_DigestFinal_ex(m, out, &len);
    EVP_MD_CTX_free(m);
    return ok ? 0 : -1;
}

/* AES-128-ECB without padding; n must be a multiple of 16 */
static int aes_ecb(const unsigned char key[16], int enc, unsigned char *out,
                   const unsigned char *in, int n)
{
    EVP_CIPHER_CTX *c = EVP_CIPHER_CTX_new();
    int l1 = 0, l2 = 0;
    int ok = c && EVP_CipherInit_ex(c, EVP_aes_128_ecb(), NULL, key, NULL, enc) &&
             EVP_CIPHER_CTX_set_padding(c, 0) && EVP_CipherUpdate(c, out, &l1, in, n) &&
             EVP_CipherFinal_ex(c, out + l1, &l2);
    EVP_CIPHER_CTX_free(c);
    return ok && l1 + l2 == n ? 0 : -1;
}

/* raw signature bytes of a certificate (what the pairing hashes cover) */
static const unsigned char *cert_sig(X509 *x, int *len)
{
    const ASN1_BIT_STRING *sig;
    X509_get0_signature(&sig, NULL, x);
    *len = ASN1_STRING_length(sig);
    return ASN1_STRING_get0_data(sig);
}

/* ---------------------------------------------------------------- identity */

static void path(char *out, size_t n, const char *file)
{
    snprintf(out, n, "%s/%s", gs.dir, file);
}

static int create_creds(const char *kp, const char *cp)
{
    EVP_PKEY *key = EVP_RSA_gen(2048);
    X509 *x = X509_new();
    BIGNUM *bn = BN_new();
    if (!key || !x || !bn) return -1;
    X509_set_version(x, 2);
    BN_rand(bn, 159, BN_RAND_TOP_ANY, BN_RAND_BOTTOM_ANY);
    BN_to_ASN1_INTEGER(bn, X509_get_serialNumber(x));
    BN_free(bn);
    X509_gmtime_adj(X509_getm_notBefore(x), 0);
    X509_gmtime_adj(X509_getm_notAfter(x), 20L * 365 * 86400);
    X509_set_pubkey(x, key);
    X509_NAME *name = X509_get_subject_name(x);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
                               (const unsigned char *)"FireCube GameStream Host", -1, -1, 0);
    X509_set_issuer_name(x, name);
    if (!X509_sign(x, key, EVP_sha256())) return -1;

    mode_t old = umask(077);
    FILE *f = fopen(kp, "w");
    if (!f || !PEM_write_PrivateKey(f, key, NULL, NULL, 0, NULL, NULL)) return -1;
    fclose(f);
    f = fopen(cp, "w");
    if (!f || !PEM_write_X509(f, x)) return -1;
    fclose(f);
    umask(old);
    fprintf(stderr, "gs: created new host key + certificate in %s\n", gs.dir);
    gs.key = key;
    gs.cert = x;
    return 0;
}

static int load_identity(void)
{
    char kp[256], cp[256], up[256];
    path(kp, sizeof(kp), "key.pem");
    path(cp, sizeof(cp), "cert.pem");
    path(up, sizeof(up), "uniqueid");
    mkdir(gs.dir, 0700);

    FILE *f = fopen(kp, "r");
    if (f) { gs.key = PEM_read_PrivateKey(f, NULL, NULL, NULL); fclose(f); }
    f = fopen(cp, "r");
    if (f) { gs.cert = PEM_read_X509(f, NULL, NULL, NULL); fclose(f); }
    if (!gs.key || !gs.cert) {
        if (gs.key || gs.cert) {
            fprintf(stderr, "gs: only one of key.pem/cert.pem readable, refusing to replace\n");
            return -1;
        }
        if (create_creds(kp, cp) < 0) { ssl_log("create key/cert"); return -1; }
    }

    BIO *b = BIO_new(BIO_s_mem());
    PEM_write_bio_X509(b, gs.cert);
    char *p;
    long n = BIO_get_mem_data(b, &p);
    gs.cert_pem = malloc(n);
    memcpy(gs.cert_pem, p, n);
    gs.cert_pem_len = n;
    BIO_free(b);

    f = fopen(up, "r");
    if (!f || !fgets(gs.uniqueid, sizeof(gs.uniqueid), f) || strlen(gs.uniqueid) < 36) {
        if (f) fclose(f);
        unsigned char r[16];
        RAND_bytes(r, 16);
        r[6] = (r[6] & 0x0f) | 0x40;
        r[8] = (r[8] & 0x3f) | 0x80;
        snprintf(gs.uniqueid, sizeof(gs.uniqueid),
                 "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11],
                 r[12], r[13], r[14], r[15]);
        f = fopen(up, "w");
        if (f) fprintf(f, "%s\n", gs.uniqueid);
    }
    if (f) fclose(f);
    gs.uniqueid[strcspn(gs.uniqueid, "\r\n")] = 0;

    gs.paired = sk_X509_new_null();
    char pp[256];
    path(pp, sizeof(pp), "paired.pem");
    f = fopen(pp, "r");
    if (f) {
        X509 *x;
        while ((x = PEM_read_X509(f, NULL, NULL, NULL))) sk_X509_push(gs.paired, x);
        fclose(f);
        ERR_clear_error();
    }
    fprintf(stderr, "gs: host %s, %d paired client(s)\n", gs.uniqueid, sk_X509_num(gs.paired));
    return 0;
}

static int is_paired(X509 *x)      /* caller holds lock */
{
    for (int i = 0; i < sk_X509_num(gs.paired); i++)
        if (!X509_cmp(sk_X509_value(gs.paired, i), x)) return 1;
    return 0;
}

static void add_paired(X509 *x)    /* caller holds lock */
{
    if (is_paired(x)) return;
    X509_up_ref(x);
    sk_X509_push(gs.paired, x);
    char pp[256];
    path(pp, sizeof(pp), "paired.pem");
    mode_t old = umask(077);
    FILE *f = fopen(pp, "a");
    umask(old);
    if (f) { PEM_write_X509(f, x); fclose(f); }
}

/* ---------------------------------------------------------------- HTTP I/O */

static int conn_write(struct conn *c, const char *p, size_t n)
{
    while (n) {
        int w = c->ssl ? SSL_write(c->ssl, p, n) : (int)write(c->fd, p, n);
        if (w <= 0) {
            if (!c->ssl && w < 0 && errno == EINTR) continue;
            return -1;
        }
        p += w;
        n -= w;
    }
    return 0;
}

static void reply(struct conn *c, int http_status, const char *type, const char *body, size_t n)
{
    char h[256];
    int hl = snprintf(h, sizeof(h),
                      "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                      "Connection: close\r\n\r\n",
                      http_status, http_status == 200 ? "OK" : "Not Found", type, n);
    if (conn_write(c, h, hl) == 0) conn_write(c, body, n);
}

/* body = the inside of <root>; status = the XML status_code */
static void reply_xml(struct conn *c, int status, const char *msg, const char *body)
{
    size_t n = strlen(body) + 256;
    char *x = malloc(n);
    int l = snprintf(x, n, "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<root status_code=\"%d\"%s%s%s>%s</root>\n",
                     status, msg ? " status_message=\"" : "", msg ? msg : "", msg ? "\"" : "", body);
    reply(c, 200, "application/xml", x, l);
    free(x);
}

/* ---------------------------------------------------------------- endpoints */

static void do_serverinfo(struct conn *c)
{
    struct gs_launch L;
    gs_get_launch(&L);
    char b[1024];
    snprintf(b, sizeof(b),
             "<hostname>" HOSTNAME "</hostname><appversion>" APPVERSION "</appversion>"
             "<GfeVersion>" GFEVERSION "</GfeVersion><uniqueid>%s</uniqueid>"
             "<HttpsPort>%d</HttpsPort><ExternalPort>%d</ExternalPort>"
             "<MaxLumaPixelsHEVC>%s</MaxLumaPixelsHEVC><mac>00:00:00:00:00:00</mac>"
             "<LocalIP>%s</LocalIP><ServerCodecModeSupport>%d</ServerCodecModeSupport>"
             "<PairStatus>%d</PairStatus><currentgame>%d</currentgame>"
             "<state>%s</state>",
             /* HEVC: Sunshine's MaxLumaPixelsHEVC; SCM_H264 0x1 | SCM_HEVC 0x100 (no Main10) */
             gs.uniqueid, HTTPS_PORT, HTTP_PORT, gs_hevc_offered ? "1869449984" : "0",
             c->local_ip, gs_hevc_offered ? 0x101 : 0x1, c->authorized,
             L.active ? L.appid : 0, L.active ? "SUNSHINE_SERVER_BUSY" : "SUNSHINE_SERVER_FREE");
    reply_xml(c, 200, NULL, b);
}

/* phase 1: hold the request until the PIN file appears, then send our cert */
static void pair_getservercert(struct conn *c, const char *q, const char *client_id)
{
    char salt_hex[64], dev[64] = "?", cert_hex[8192];
    unsigned char salt[16], pem[4096];
    static pthread_mutex_t one = PTHREAD_MUTEX_INITIALIZER;
    int pl;
    if (!qget(q, "salt", salt_hex, sizeof(salt_hex)) || hex_decode(salt, 16, salt_hex) != 16 ||
        !qget(q, "clientcert", cert_hex, sizeof(cert_hex)) ||
        (pl = hex_decode(pem, sizeof(pem), cert_hex)) <= 0) {
        reply_xml(c, 400, "Bad pairing request", "<paired>0</paired>");
        return;
    }
    BIO *b = BIO_new_mem_buf(pem, pl);
    X509 *client = PEM_read_bio_X509(b, NULL, NULL, NULL);
    BIO_free(b);
    if (!client) {
        reply_xml(c, 400, "Bad client certificate", "<paired>0</paired>");
        return;
    }
    if (pthread_mutex_trylock(&one)) {          /* another pairing is waiting for its PIN */
        X509_free(client);
        reply_xml(c, 400, "Another pairing is in progress", "<paired>0</paired>");
        return;
    }
    qget(q, "devicename", dev, sizeof(dev));
    char pinpath[256], pin[16] = "";
    path(pinpath, sizeof(pinpath), "pin");
    unlink(pinpath);
    fprintf(stderr, "gs: pairing request from \"%s\" (%s) — enter the PIN shown by Moonlight:\n"
            "gs:   echo NNNN > %s\n", dev, c->peer_ip, pinpath);

    for (int i = 0; i < PIN_WAIT_S * 2 && !*pin; i++) {
        FILE *f = fopen(pinpath, "r");
        if (f) {
            if (!fgets(pin, sizeof(pin), f)) *pin = 0;
            fclose(f);
            unlink(pinpath);
            pin[strcspn(pin, "\r\n ")] = 0;
            if (strlen(pin) != 4 || strspn(pin, "0123456789") != 4) {
                fprintf(stderr, "gs: PIN must be 4 digits\n");
                *pin = 0;
            }
        }
        if (!*pin) usleep(500000);
    }
    pthread_mutex_unlock(&one);
    if (!*pin) {
        fprintf(stderr, "gs: no PIN within %d s, pairing aborted\n", PIN_WAIT_S);
        X509_free(client);
        reply_xml(c, 200, NULL, "<paired>0</paired>");
        return;
    }

    unsigned char h[32];
    sha256(h, salt, 16, pin, 4, "", 0);
    pthread_mutex_lock(&gs.lock);
    X509_free(gs.pair.client);
    memset(&gs.pair, 0, sizeof(gs.pair));
    gs.pair.client = client;
    memcpy(gs.pair.aes, h, 16);
    snprintf(gs.pair.client_id, sizeof(gs.pair.client_id), "%s", client_id);
    gs.pair.phase = 1;
    pthread_mutex_unlock(&gs.lock);

    char *body = malloc(gs.cert_pem_len * 2 + 64);
    strcpy(body, "<paired>1</paired><plaincert>");
    hex_encode(body + strlen(body), (unsigned char *)gs.cert_pem, gs.cert_pem_len);
    strcat(body, "</plaincert>");
    reply_xml(c, 200, NULL, body);
    free(body);
}

static void pair_fail(struct conn *c, const char *why)
{
    fprintf(stderr, "gs: pairing failed: %s\n", why);
    X509_free(gs.pair.client);
    memset(&gs.pair, 0, sizeof(gs.pair));
    pthread_mutex_unlock(&gs.lock);
    reply_xml(c, 200, NULL, "<paired>0</paired>");
}

static void do_pair(struct conn *c, const char *q)
{
    char id[128] = "", v[1200], phrase[32] = "";
    qget(q, "uniqueid", id, sizeof(id));
    qget(q, "phrase", phrase, sizeof(phrase));

    if (!strcmp(phrase, "getservercert")) {
        if (c->ssl) { reply_xml(c, 400, "Use HTTP", "<paired>0</paired>"); return; }
        pair_getservercert(c, q, id);
        return;
    }
    if (!strcmp(phrase, "pairchallenge")) {     /* phase 5, HTTPS: TLS already proved the cert */
        reply_xml(c, 200, NULL, c->authorized ? "<paired>1</paired>" : "<paired>0</paired>");
        if (c->authorized) fprintf(stderr, "gs: client %s paired\n", c->peer_ip);
        return;
    }

    pthread_mutex_lock(&gs.lock);
    if (!gs.pair.phase || strcmp(id, gs.pair.client_id)) {
        pthread_mutex_unlock(&gs.lock);
        reply_xml(c, 400, "No pairing in progress", "<paired>0</paired>");
        return;
    }
    unsigned char in[512], dec[48], out[48];
    int n;
    if (qget(q, "clientchallenge", v, sizeof(v))) {             /* phase 2 */
        if (gs.pair.phase != 1 || hex_decode(in, sizeof(in), v) != 16 ||
            aes_ecb(gs.pair.aes, 0, dec, in, 16))
            return pair_fail(c, "bad clientchallenge");
        int sl;
        const unsigned char *sig = cert_sig(gs.cert, &sl);
        RAND_bytes(gs.pair.serversecret, 16);
        RAND_bytes(gs.pair.serverchallenge, 16);
        unsigned char plain[48];
        sha256(plain, dec, 16, sig, sl, gs.pair.serversecret, 16);
        memcpy(plain + 32, gs.pair.serverchallenge, 16);
        aes_ecb(gs.pair.aes, 1, out, plain, 48);
        gs.pair.phase = 2;
        pthread_mutex_unlock(&gs.lock);
        char b[200];
        strcpy(b, "<paired>1</paired><challengeresponse>");
        hex_encode(b + strlen(b), out, 48);
        strcat(b, "</challengeresponse>");
        reply_xml(c, 200, NULL, b);
    } else if (qget(q, "serverchallengeresp", v, sizeof(v))) {  /* phase 3 */
        if (gs.pair.phase != 2 || hex_decode(in, sizeof(in), v) != 32 ||
            aes_ecb(gs.pair.aes, 0, gs.pair.clienthash, in, 32))
            return pair_fail(c, "bad serverchallengeresp");
        unsigned char sig[512];
        size_t sl = sizeof(sig);
        EVP_MD_CTX *m = EVP_MD_CTX_new();
        int ok = EVP_DigestSignInit(m, NULL, EVP_sha256(), NULL, gs.key) &&
                 EVP_DigestSign(m, sig, &sl, gs.pair.serversecret, 16);
        EVP_MD_CTX_free(m);
        if (!ok) return pair_fail(c, "sign failed");
        gs.pair.phase = 3;
        char b[1200];
        strcpy(b, "<paired>1</paired><pairingsecret>");
        hex_encode(b + strlen(b), gs.pair.serversecret, 16);
        hex_encode(b + strlen(b), sig, sl);
        strcat(b, "</pairingsecret>");
        pthread_mutex_unlock(&gs.lock);
        reply_xml(c, 200, NULL, b);
    } else if (qget(q, "clientpairingsecret", v, sizeof(v))) {  /* phase 4 */
        if (gs.pair.phase != 3 || (n = hex_decode(in, sizeof(in), v)) <= 16)
            return pair_fail(c, "bad clientpairingsecret");
        int sl;
        const unsigned char *csig = cert_sig(gs.pair.client, &sl);
        unsigned char h[32];
        sha256(h, gs.pair.serverchallenge, 16, csig, sl, in, 16);
        if (memcmp(h, gs.pair.clienthash, 32)) return pair_fail(c, "hash mismatch (wrong PIN?)");
        EVP_MD_CTX *m = EVP_MD_CTX_new();
        int ok = EVP_DigestVerifyInit(m, NULL, EVP_sha256(), NULL,
                                      X509_get0_pubkey(gs.pair.client)) == 1 &&
                 EVP_DigestVerify(m, in + 16, n - 16, in, 16) == 1;
        EVP_MD_CTX_free(m);
        if (!ok) return pair_fail(c, "client signature invalid");
        add_paired(gs.pair.client);
        X509_free(gs.pair.client);
        memset(&gs.pair, 0, sizeof(gs.pair));
        pthread_mutex_unlock(&gs.lock);
        fprintf(stderr, "gs: client certificate stored (%s)\n", c->peer_ip);
        reply_xml(c, 200, NULL, "<paired>1</paired>");
    } else {
        pthread_mutex_unlock(&gs.lock);
        reply_xml(c, 400, "Unknown pairing phase", "<paired>0</paired>");
    }
}

static void do_applist(struct conn *c)
{
    char b[200];
    snprintf(b, sizeof(b), "<App><IsHdrSupported>0</IsHdrSupported><AppTitle>Kodi</AppTitle>"
             "<ID>%d</ID></App>", GS_APP_ID);
    reply_xml(c, 200, NULL, b);
}

static void do_launch(struct conn *c, const char *q, int resume)
{
    char v[128], mode[64] = "";
    struct gs_launch L = { 0 };
    if (!qget(q, "rikey", v, sizeof(v)) || hex_decode(L.rikey, 16, v) != 16 ||
        !qget(q, "rikeyid", v, sizeof(v))) {
        reply_xml(c, 400, "Missing rikey/rikeyid", "<gamesession>0</gamesession>");
        return;
    }
    L.rikeyid = (int32_t)strtol(v, NULL, 10);
    L.appid = qget(q, "appid", v, sizeof(v)) ? atoi(v) : GS_APP_ID;
    if (L.appid != GS_APP_ID && !resume) {
        reply_xml(c, 404, "No such app", "<gamesession>0</gamesession>");
        return;
    }
    L.appid = GS_APP_ID;
    L.width = 1920; L.height = 1080; L.fps = 60;
    if (qget(q, "mode", mode, sizeof(mode)))
        sscanf(mode, "%dx%dx%d", &L.width, &L.height, &L.fps);
    L.surround_info = qget(q, "surroundAudioInfo", v, sizeof(v)) ? atoi(v) : 0x30002;
    L.local_audio = qget(q, "localAudioPlayMode", v, sizeof(v)) ? atoi(v) : 0;
    unsigned char r[8];
    RAND_bytes(r, 8);
    hex_encode(L.ping_payload, r, 8);
    RAND_bytes((unsigned char *)&L.connect_data, 4);
    L.active = 1;

    pthread_mutex_lock(&gs.lock);
    if (resume && !gs.launch.active) {
        pthread_mutex_unlock(&gs.lock);
        reply_xml(c, 503, "Nothing is running", "<resume>0</resume>");
        return;
    }
    L.generation = gs.launch.generation + 1;
    gs.launch = L;
    pthread_mutex_unlock(&gs.lock);
    gss_stop(resume ? "resume" : "new launch");      /* new keys: a running stream is stale */
    fprintf(stderr, "gs: %s from %s: %dx%d@%d, audio 0x%x\n", resume ? "resume" : "launch",
            c->peer_ip, L.width, L.height, L.fps, L.surround_info);

    char b[256];
    snprintf(b, sizeof(b), "<sessionUrl0>rtsp://%s:%d</sessionUrl0><%s>1</%s>", c->local_ip,
             RTSP_PORT, resume ? "resume" : "gamesession", resume ? "resume" : "gamesession");
    reply_xml(c, 200, NULL, b);
}

static void do_cancel(struct conn *c)
{
    pthread_mutex_lock(&gs.lock);
    gs.launch.active = 0;
    pthread_mutex_unlock(&gs.lock);
    gss_stop("quit");
    if (gs_on_cancel) gs_on_cancel();
    fprintf(stderr, "gs: cancel from %s\n", c->peer_ip);
    reply_xml(c, 200, NULL, "<cancel>1</cancel>");
}

/* ---------------------------------------------------------------- server */

static void handle(struct conn *c)
{
    static __thread char req[REQ_MAX + 1];
    size_t n = 0;
    while (n < REQ_MAX) {
        int r = c->ssl ? SSL_read(c->ssl, req + n, REQ_MAX - n)
                       : (int)read(c->fd, req + n, REQ_MAX - n);
        if (r <= 0) return;
        n += r;
        req[n] = 0;
        if (strstr(req, "\r\n\r\n")) break;
    }
    char method[8], target[REQ_MAX];
    if (sscanf(req, "%7s %16383s", method, target) != 2 || strcmp(method, "GET")) {
        reply(c, 404, "text/plain", "", 0);
        return;
    }
    char *q = strchr(target, '?');
    if (q) *q++ = 0; else q = "";

    if (c->ssl) {
        X509 *peer = SSL_get1_peer_certificate(c->ssl);
        if (peer) {
            pthread_mutex_lock(&gs.lock);
            c->authorized = is_paired(peer);
            pthread_mutex_unlock(&gs.lock);
            X509_free(peer);
        }
        if (!c->authorized) {
            fprintf(stderr, "gs: https %s from %s: client not paired\n", target, c->peer_ip);
            reply_xml(c, 401, "The client is not authorized. Certificate verification failed.", "");
            return;
        }
    }
    if (strcmp(target, "/serverinfo"))
        fprintf(stderr, "gs: %s %s from %s\n", c->ssl ? "https" : "http", target, c->peer_ip);

    if (!strcmp(target, "/serverinfo")) do_serverinfo(c);
    else if (!strcmp(target, "/pair")) do_pair(c, q);
    else if (!strcmp(target, "/unpair")) {
        pthread_mutex_lock(&gs.lock);
        X509_free(gs.pair.client);
        memset(&gs.pair, 0, sizeof(gs.pair));
        pthread_mutex_unlock(&gs.lock);
        reply_xml(c, 200, NULL, "");
    } else if (c->ssl && !strcmp(target, "/applist")) do_applist(c);
    else if (c->ssl && !strcmp(target, "/launch")) do_launch(c, q, 0);
    else if (c->ssl && !strcmp(target, "/resume")) do_launch(c, q, 1);
    else if (c->ssl && !strcmp(target, "/cancel")) do_cancel(c);
    else reply(c, 404, "text/plain", "", 0);
}

static int verify_any(int ok, X509_STORE_CTX *x) { (void)ok; (void)x; return 1; }

static void *conn_thread(void *arg)
{
    struct conn *c = arg;
    struct timeval tv = { .tv_sec = 10 };
    setsockopt(c->fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(c->fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (c->ssl) {
        SSL_set_fd(c->ssl, c->fd);
        if (SSL_accept(c->ssl) != 1) {
            ssl_log("TLS handshake");
            goto out;
        }
    }
    handle(c);
    if (c->ssl) SSL_shutdown(c->ssl);
out:
    if (c->ssl) SSL_free(c->ssl);
    ERR_clear_error();
    close(c->fd);
    free(c);
    return NULL;
}

static void *listen_thread(void *arg)
{
    int https = (intptr_t)arg;
    int port = https ? HTTPS_PORT : HTTP_PORT;
    int ls = socket(AF_INET, SOCK_STREAM, 0), one = 1;
    setsockopt(ls, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port),
                             .sin_addr.s_addr = htonl(INADDR_ANY) };
    if (bind(ls, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(ls, 8) < 0) {
        fprintf(stderr, "gs: tcp port %d: %s\n", port, strerror(errno));
        return NULL;
    }
    for (;;) {
        struct sockaddr_in peer, local;
        socklen_t pl = sizeof(peer), ll = sizeof(local);
        int fd = accept(ls, (struct sockaddr *)&peer, &pl);
        if (fd < 0) continue;
        struct conn *c = calloc(1, sizeof(*c));
        c->fd = fd;
        getsockname(fd, (struct sockaddr *)&local, &ll);
        inet_ntop(AF_INET, &local.sin_addr, c->local_ip, sizeof(c->local_ip));
        inet_ntop(AF_INET, &peer.sin_addr, c->peer_ip, sizeof(c->peer_ip));
        if (https) c->ssl = SSL_new(gs.tls);
        pthread_t th;
        pthread_attr_t at;
        pthread_attr_init(&at);
        pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
        if (pthread_create(&th, &at, conn_thread, c)) {
            if (c->ssl) SSL_free(c->ssl);
            close(fd);
            free(c);
        }
        pthread_attr_destroy(&at);
    }
    return NULL;
}

void gs_get_launch(struct gs_launch *out)
{
    pthread_mutex_lock(&gs.lock);
    *out = gs.launch;
    pthread_mutex_unlock(&gs.lock);
}

int gs_start(const char *state_dir)
{
    snprintf(gs.dir, sizeof(gs.dir), "%s", state_dir);
    if (load_identity() < 0) return -1;
    gs.tls = SSL_CTX_new(TLS_server_method());
    if (!gs.tls || SSL_CTX_use_certificate(gs.tls, gs.cert) != 1 ||
        SSL_CTX_use_PrivateKey(gs.tls, gs.key) != 1) {
        ssl_log("TLS context");
        return -1;
    }
    /* request a client cert, accept any at TLS level; handle() checks it against paired.pem */
    SSL_CTX_set_verify(gs.tls, SSL_VERIFY_PEER, verify_any);
    pthread_t th;
    if (pthread_create(&th, NULL, listen_thread, (void *)(intptr_t)0) ||
        pthread_create(&th, NULL, listen_thread, (void *)(intptr_t)1))
        return -1;
    fprintf(stderr, "gs: Moonlight host listening on tcp %d (http) + %d (https)\n",
            HTTP_PORT, HTTPS_PORT);
    if (gss_start() < 0) return -1;
    gs_mdns_start(HOSTNAME, HTTP_PORT);     /* best effort: without it, clients add the IP */
    return 0;
}
