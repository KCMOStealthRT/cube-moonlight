/*
 * gs-mdns.c — announce the Moonlight host on the LAN (mDNS/DNS-SD "_nvstream._tcp", HTTP port),
 * so clients list it without being given the IP. Moonlight then asks /serverinfo at the
 * resolved address, as for a manually added host. No TXT records (Sunshine sends none).
 *
 * Done through the system's avahi-daemon (CoreELEC runs it for Kodi's zeroconf; its service
 * directory /etc/avahi/services is read-only): libavahi-client is loaded with dlopen at run
 * time, so cube-stream builds without Avahi headers and still runs where there is no Avahi
 * (then it only logs that clients must add the host by IP). AVAHI_CLIENT_NO_FAIL keeps the
 * client waiting for / reconnecting to the daemon (e.g. zeroconf switched off and on in Kodi).
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "gamestream.h"

/* the few avahi-client declarations used (avahi-client/client.h, publish.h; enums are int) */
typedef struct AvahiPoll AvahiPoll;
typedef struct AvahiThreadedPoll AvahiThreadedPoll;
typedef struct AvahiClient AvahiClient;
typedef struct AvahiEntryGroup AvahiEntryGroup;
typedef void (*AvahiClientCallback)(AvahiClient *c, int state, void *userdata);
typedef void (*AvahiEntryGroupCallback)(AvahiEntryGroup *g, int state, void *userdata);
#define AVAHI_CLIENT_S_REGISTERING 1
#define AVAHI_CLIENT_S_RUNNING 2
#define AVAHI_CLIENT_S_COLLISION 3
#define AVAHI_CLIENT_FAILURE 100
#define AVAHI_CLIENT_CONNECTING 101
#define AVAHI_CLIENT_NO_FAIL 2
#define AVAHI_ENTRY_GROUP_ESTABLISHED 2
#define AVAHI_ENTRY_GROUP_COLLISION 3
#define AVAHI_ENTRY_GROUP_FAILURE 4
#define AVAHI_IF_UNSPEC (-1)
#define AVAHI_PROTO_UNSPEC (-1)
#define SERVICE_TYPE "_nvstream._tcp"

static struct {
    AvahiThreadedPoll *(*threaded_poll_new)(void);
    const AvahiPoll *(*threaded_poll_get)(AvahiThreadedPoll *);
    int (*threaded_poll_start)(AvahiThreadedPoll *);
    AvahiClient *(*client_new)(const AvahiPoll *, int flags, AvahiClientCallback, void *, int *);
    int (*client_errno)(AvahiClient *);
    AvahiEntryGroup *(*group_new)(AvahiClient *, AvahiEntryGroupCallback, void *);
    int (*group_add_service)(AvahiEntryGroup *, int iface, int proto, int flags,
                             const char *name, const char *type, const char *domain,
                             const char *host, uint16_t port, ...);
    int (*group_commit)(AvahiEntryGroup *);
    int (*group_reset)(AvahiEntryGroup *);
    int (*group_free)(AvahiEntryGroup *);
    const char *(*strerror)(int);
    char *(*alternative_service_name)(const char *);
} av;

/* touched only from the Avahi poll thread (callbacks) after gs_mdns_start */
static AvahiEntryGroup *group;
static char *name;
static uint16_t port;

static void publish(AvahiClient *c);

static void group_cb(AvahiEntryGroup *g, int state, void *userdata)
{
    AvahiClient *c = userdata;
    if (state == AVAHI_ENTRY_GROUP_ESTABLISHED) {
        fprintf(stderr, "gs: mdns: announced \"%s\" (" SERVICE_TYPE ", port %u)\n", name, port);
    } else if (state == AVAHI_ENTRY_GROUP_COLLISION) {
        char *alt = av.alternative_service_name(name);     /* "FireCube #2" */
        fprintf(stderr, "gs: mdns: name \"%s\" taken on the LAN, using \"%s\"\n", name, alt);
        free(name);
        name = alt;
        av.group_reset(g);
        publish(c);
    } else if (state == AVAHI_ENTRY_GROUP_FAILURE) {
        fprintf(stderr, "gs: mdns: announcement failed: %s\n", av.strerror(av.client_errno(c)));
    }
}

static void publish(AvahiClient *c)
{
    int r;
    if (!group && !(group = av.group_new(c, group_cb, c))) {
        fprintf(stderr, "gs: mdns: entry group: %s\n", av.strerror(av.client_errno(c)));
        return;
    }
    if ((r = av.group_add_service(group, AVAHI_IF_UNSPEC, AVAHI_PROTO_UNSPEC, 0, name,
                                  SERVICE_TYPE, NULL, NULL, port, NULL)) < 0 ||
        (r = av.group_commit(group)) < 0)
        fprintf(stderr, "gs: mdns: publish: %s\n", av.strerror(r));
}

static void client_cb(AvahiClient *c, int state, void *userdata)
{
    (void)userdata;
    switch (state) {
    case AVAHI_CLIENT_S_RUNNING:            /* daemon up (again): (re)announce */
        publish(c);
        break;
    case AVAHI_CLIENT_S_REGISTERING:        /* daemon renaming the host: withdraw until RUNNING */
    case AVAHI_CLIENT_S_COLLISION:
        if (group) av.group_reset(group);
        break;
    case AVAHI_CLIENT_CONNECTING:           /* daemon gone; NO_FAIL waits for it to come back */
        if (group) { av.group_free(group); group = NULL; }
        fprintf(stderr, "gs: mdns: avahi-daemon not running, waiting for it\n");
        break;
    case AVAHI_CLIENT_FAILURE:
        fprintf(stderr, "gs: mdns: avahi client failed: %s\n", av.strerror(av.client_errno(c)));
        break;
    }
}

int gs_mdns_start(const char *service_name, int http_port)
{
    void *h = dlopen("libavahi-client.so.3", RTLD_NOW);
    void *hc = dlopen("libavahi-common.so.3", RTLD_NOW);
    if (!h || !hc) {
        fprintf(stderr, "gs: mdns: no libavahi-client (%s): add the host by IP in Moonlight\n",
                dlerror());
        return -1;
    }
#define SYM(lib, field, sym) if (!(*(void **)&av.field = dlsym(lib, sym))) goto missing
    SYM(hc, threaded_poll_new, "avahi_threaded_poll_new");
    SYM(hc, threaded_poll_get, "avahi_threaded_poll_get");
    SYM(hc, threaded_poll_start, "avahi_threaded_poll_start");
    SYM(hc, strerror, "avahi_strerror");
    SYM(hc, alternative_service_name, "avahi_alternative_service_name");
    SYM(h, client_new, "avahi_client_new");
    SYM(h, client_errno, "avahi_client_errno");
    SYM(h, group_new, "avahi_entry_group_new");
    SYM(h, group_add_service, "avahi_entry_group_add_service");
    SYM(h, group_commit, "avahi_entry_group_commit");
    SYM(h, group_reset, "avahi_entry_group_reset");
    SYM(h, group_free, "avahi_entry_group_free");
#undef SYM

    name = strdup(service_name);
    port = http_port;
    int err = 0;
    AvahiThreadedPoll *tp = av.threaded_poll_new();
    /* client_cb runs inside client_new already (before the poll thread starts) */
    if (!tp || !av.client_new(av.threaded_poll_get(tp), AVAHI_CLIENT_NO_FAIL, client_cb, NULL,
                              &err) ||
        av.threaded_poll_start(tp) < 0) {
        fprintf(stderr, "gs: mdns: avahi client: %s\n", err ? av.strerror(err) : "start failed");
        return -1;
    }
    return 0;
missing:
    fprintf(stderr, "gs: mdns: libavahi-client lacks a symbol (%s)\n", dlerror());
    return -1;
}
