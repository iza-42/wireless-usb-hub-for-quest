/*
 * Minimal USB/IP server (protocol 1.1.1) for Android, no root.
 *
 * The Java side opens each USB device through UsbManager and hands us the
 * usbdevfs file descriptor. We serve OP_REQ_DEVLIST / OP_REQ_IMPORT on TCP
 * 3240 and forward CMD_SUBMIT / CMD_UNLINK as async usbdevfs URBs. Unlike the
 * Java UsbRequest API this supports isochronous endpoints (UVC cameras).
 */
#include <jni.h>
#include <android/log.h>
#include <pthread.h>
#include <poll.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <linux/usbdevice_fs.h>

#define TAG "QuestUsbIp"

#define USBIP_VERSION   0x0111
#define OP_REQ_DEVLIST  0x8005
#define OP_REP_DEVLIST  0x0005
#define OP_REQ_IMPORT   0x8003
#define OP_REP_IMPORT   0x0003
#define CMD_SUBMIT      1
#define CMD_UNLINK      2
#define RET_SUBMIT      3
#define RET_UNLINK      4

#define MAX_DEVS        8
#define MAX_IF          32
#define MAX_ISO_PKTS    1024
#define USBFS_ISO_MAX   128          /* kernel limit of packets per usbdevfs URB */
#define MAX_XFER        (32 << 20)
#define EP_UNKNOWN      0xff

/* ---------- logging (logcat + ring buffer shown in the app) ---------- */

#define LOG_LINES 24
static char g_log[LOG_LINES][160];
static int g_log_pos;
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;

static void logmsg(const char *fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    __android_log_print(ANDROID_LOG_INFO, TAG, "%s", line);
    pthread_mutex_lock(&g_log_lock);
    strcpy(g_log[g_log_pos], line);
    g_log_pos = (g_log_pos + 1) % LOG_LINES;
    pthread_mutex_unlock(&g_log_lock);
}

/* ---------- big-endian helpers / socket IO ---------- */

static void put32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = v; }
static void put16(uint8_t *p, uint16_t v) { p[0] = v >> 8; p[1] = v; }
static uint32_t get32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint16_t get16(const uint8_t *p) { return p[0] << 8 | p[1]; }

static int read_full(int s, void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        ssize_t r = recv(s, p, n, 0);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

static int write_full(int s, const void *buf, size_t n)
{
    const uint8_t *p = buf;
    while (n) {
        ssize_t r = send(s, p, n, MSG_NOSIGNAL);
        if (r < 0 && errno == EINTR) continue;
        if (r <= 0) return -1;
        p += r; n -= r;
    }
    return 0;
}

/* ---------- devices ---------- */

typedef struct {
    int used, gone, attached;
    int fd, sock;
    int busnum, devnum, speed;
    char busid[32], path[256];
    uint16_t vid, pid, bcd;
    uint8_t cls, sub, proto, cfgval, numcfg;
    int numif;
    uint8_t ifnum[MAX_IF], if_cls[MAX_IF], if_sub[MAX_IF], if_proto[MAX_IF];
    uint8_t eptype[32];            /* usbdevfs URB type per endpoint, index = num | (in ? 16 : 0) */
    uint8_t *desc;
    int desclen;
} usbdev;

static usbdev g_devs[MAX_DEVS];
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;

static int ep_index(int addr) { return (addr & 0x0f) | ((addr & 0x80) ? 16 : 0); }

static void parse_desc(usbdev *d)
{
    const uint8_t *b = d->desc;
    int pos = 0, active = 0, first_cfg = 1;
    static const uint8_t xfer_to_urb[4] = {
        USBDEVFS_URB_TYPE_CONTROL, USBDEVFS_URB_TYPE_ISO,
        USBDEVFS_URB_TYPE_BULK, USBDEVFS_URB_TYPE_INTERRUPT };

    d->numif = 0;
    memset(d->eptype, EP_UNKNOWN, sizeof(d->eptype));
    while (pos + 2 <= d->desclen) {
        int len = b[pos], type = b[pos + 1];
        if (len < 2 || pos + len > d->desclen) break;
        if (type == 1 && len >= 18) {
            d->bcd = b[pos + 12] | b[pos + 13] << 8;
            d->vid = b[pos + 8] | b[pos + 9] << 8;
            d->pid = b[pos + 10] | b[pos + 11] << 8;
            d->cls = b[pos + 4]; d->sub = b[pos + 5]; d->proto = b[pos + 6];
            d->numcfg = b[pos + 17];
        } else if (type == 2 && len >= 9) {
            /* unconfigured device: describe the first configuration */
            active = (b[pos + 5] == d->cfgval) || (d->cfgval == 0 && first_cfg);
            first_cfg = 0;
        } else if (type == 4 && len >= 9 && active) {
            if (b[pos + 3] == 0 && d->numif < MAX_IF) {
                d->ifnum[d->numif] = b[pos + 2];
                d->if_cls[d->numif] = b[pos + 5];
                d->if_sub[d->numif] = b[pos + 6];
                d->if_proto[d->numif] = b[pos + 7];
                d->numif++;
            }
        } else if (type == 5 && len >= 7 && active) {
            d->eptype[ep_index(b[pos + 2])] = xfer_to_urb[b[pos + 3] & 3];
        }
        pos += len;
    }
}

static int get_active_config(int fd)
{
    uint8_t v = 0;
    struct usbdevfs_ctrltransfer ct = {
        .bRequestType = 0x80, .bRequest = 8, .wValue = 0, .wIndex = 0,
        .wLength = 1, .timeout = 1000, .data = &v };
    return ioctl(fd, USBDEVFS_CONTROL, &ct) == 1 ? v : -1;
}

static void claim_all(usbdev *d)
{
    for (int i = 0; i < d->numif; i++) {
        unsigned int n = d->ifnum[i];
        struct usbdevfs_ioctl dc = { .ifno = n, .ioctl_code = USBDEVFS_DISCONNECT, .data = NULL };
        ioctl(d->fd, USBDEVFS_IOCTL, &dc);          /* detach kernel driver, ok to fail */
        if (ioctl(d->fd, USBDEVFS_CLAIMINTERFACE, &n) < 0)
            logmsg("claim interface %u failed: %s", n, strerror(errno));
    }
}

static void release_all(usbdev *d)
{
    for (int i = 0; i < d->numif; i++) {
        unsigned int n = d->ifnum[i];
        ioctl(d->fd, USBDEVFS_RELEASEINTERFACE, &n);
    }
}

/* 312-byte usbip_usb_device */
static void pack_device(uint8_t *p, const usbdev *d)
{
    memset(p, 0, 312);
    strncpy((char *)p, d->path, 255);
    strncpy((char *)p + 256, d->busid, 31);
    put32(p + 288, d->busnum);
    put32(p + 292, d->devnum);
    put32(p + 296, d->speed);
    put16(p + 300, d->vid);
    put16(p + 302, d->pid);
    put16(p + 304, d->bcd);
    p[306] = d->cls; p[307] = d->sub; p[308] = d->proto;
    p[309] = d->cfgval; p[310] = d->numcfg; p[311] = d->numif;
}

/* ---------- URB forwarding ---------- */

typedef struct uctx {
    struct uctx *next;
    uint32_t seq, client_npk, unlink_seq;
    int dir_in, type, unlinked;
    int npk, nurbs, remaining;
    uint32_t *iso_off;              /* offsets as sent by the client */
    struct usbdevfs_urb **urbs;
    uint8_t *buf;
} uctx;

typedef struct {
    int sock, fd;
    usbdev *d;
    pthread_mutex_t send_lock, list_lock;
    uctx *pending;
    int npending;
    volatile int alive;
    /* stats, logged periodically by the reaper */
    long n_sub, n_done, n_err, bytes_in, bytes_out;
    int last_err, last_err_ep, log_errs;
    long logged_sub;
} conn_t;

static void free_ctx(uctx *x)
{
    for (int i = 0; i < x->nurbs; i++) free(x->urbs[i]);
    free(x->urbs);
    free(x->iso_off);
    free(x->buf);
    free(x);
}

static void send_locked(conn_t *c, const uint8_t *m, size_t n)
{
    pthread_mutex_lock(&c->send_lock);
    if (c->alive && write_full(c->sock, m, n) < 0) {
        c->alive = 0;
        shutdown(c->sock, SHUT_RDWR);
    }
    pthread_mutex_unlock(&c->send_lock);
}

static void send_simple_ret(conn_t *c, uint32_t seq, int status, uint32_t npk)
{
    uint8_t m[48] = {0};
    put32(m, RET_SUBMIT);
    put32(m + 4, seq);
    put32(m + 20, (uint32_t)status);
    put32(m + 32, npk);
    send_locked(c, m, sizeof(m));
}

static void send_ret_unlink(conn_t *c, uint32_t seq, int status)
{
    uint8_t m[48] = {0};
    put32(m, RET_UNLINK);
    put32(m + 4, seq);
    put32(m + 20, (uint32_t)status);
    send_locked(c, m, sizeof(m));
}

static void send_ret_submit(conn_t *c, uctx *x)
{
    int status = 0, actual = 0, errors = 0, start_frame = x->urbs[0]->start_frame;
    size_t datalen = 0;

    for (int i = 0; i < x->nurbs; i++) {
        struct usbdevfs_urb *u = x->urbs[i];
        if (!status && u->status) status = u->status;
        errors += u->error_count;
        if (x->type == USBDEVFS_URB_TYPE_ISO)
            for (int k = 0; k < u->number_of_packets; k++) actual += u->iso_frame_desc[k].actual_length;
        else
            actual += u->actual_length > 0 ? u->actual_length : 0;
    }
    if (x->dir_in) datalen = actual;

    size_t total = 48 + datalen + (size_t)x->npk * 16;
    uint8_t *m = malloc(total);
    if (!m) return;
    memset(m, 0, 48);
    put32(m, RET_SUBMIT);
    put32(m + 4, x->seq);
    put32(m + 20, (uint32_t)status);
    put32(m + 24, (uint32_t)actual);
    put32(m + 28, (uint32_t)start_frame);
    put32(m + 32, x->type == USBDEVFS_URB_TYPE_ISO ? (uint32_t)x->npk : x->client_npk);
    put32(m + 36, (uint32_t)errors);

    uint8_t *p = m + 48;
    if (x->type == USBDEVFS_URB_TYPE_ISO) {
        /* IN data is sent packed: each packet's actual bytes back to back */
        uint8_t *descs = m + 48 + datalen;
        int pk = 0;
        for (int i = 0; i < x->nurbs; i++) {
            struct usbdevfs_urb *u = x->urbs[i];
            uint8_t *src = u->buffer;
            for (int k = 0; k < u->number_of_packets; k++, pk++) {
                struct usbdevfs_iso_packet_desc *f = &u->iso_frame_desc[k];
                if (x->dir_in) { memcpy(p, src, f->actual_length); p += f->actual_length; }
                put32(descs + pk * 16, x->iso_off[pk]);
                put32(descs + pk * 16 + 4, f->length);
                put32(descs + pk * 16 + 8, f->actual_length);
                put32(descs + pk * 16 + 12, (uint32_t)f->status);
                src += f->length;
            }
        }
    } else if (datalen) {
        memcpy(p, x->buf + (x->type == USBDEVFS_URB_TYPE_CONTROL ? 8 : 0), datalen);
    }
    send_locked(c, m, total);
    free(m);
}

/* Control requests the host side must perform itself instead of forwarding. */
static int intercept_control(conn_t *c, const uint8_t *s, int *status)
{
    usbdev *d = c->d;
    uint16_t wValue = s[2] | s[3] << 8, wIndex = s[4] | s[5] << 8;

    if (s[0] == 0x00 && s[1] == 9) {                       /* SET_CONFIGURATION */
        release_all(d);
        unsigned int cfg = wValue;
        *status = ioctl(d->fd, USBDEVFS_SETCONFIGURATION, &cfg) < 0 ? -errno : 0;
        d->cfgval = wValue;
        parse_desc(d);
        claim_all(d);
        logmsg("SET_CONFIGURATION %u -> %d", wValue, *status);
        return 1;
    }
    if (s[0] == 0x01 && s[1] == 11) {                      /* SET_INTERFACE */
        struct usbdevfs_setinterface si = { .interface = wIndex, .altsetting = wValue };
        *status = ioctl(d->fd, USBDEVFS_SETINTERFACE, &si) < 0 ? -errno : 0;
        logmsg("SET_INTERFACE if=%u alt=%u -> %d", wIndex, wValue, *status);
        return 1;
    }
    if (s[0] == 0x02 && s[1] == 1 && wValue == 0) {        /* CLEAR_FEATURE(ENDPOINT_HALT) */
        unsigned int ep = wIndex;
        *status = ioctl(d->fd, USBDEVFS_CLEAR_HALT, &ep) < 0 ? -errno : 0;
        return 1;
    }
    if (s[0] == 0x23 && s[1] == 3 && wValue == 4) {        /* hub SET_FEATURE(PORT_RESET) */
        *status = 0;                                       /* resetting would re-enumerate; ignore */
        return 1;
    }
    return 0;
}

static int handle_submit(conn_t *c, const uint8_t *h)
{
    usbdev *d = c->d;
    uint32_t seq = get32(h + 4), dir_in = get32(h + 12) == 1, ep = get32(h + 16);
    uint32_t flags = get32(h + 20), npk_raw = get32(h + 32);
    int32_t len = (int32_t)get32(h + 24);
    const uint8_t *setup = h + 40;

    if (len < 0 || len > MAX_XFER) { logmsg("bad transfer length %d", len); return -1; }

    int type = ep == 0 ? USBDEVFS_URB_TYPE_CONTROL : d->eptype[ep_index(ep | (dir_in ? 0x80 : 0))];
    if (type == EP_UNKNOWN) type = USBDEVFS_URB_TYPE_BULK;
    int iso = type == USBDEVFS_URB_TYPE_ISO;
    int npk = iso ? (int)npk_raw : 0;
    if (iso && (npk <= 0 || npk > MAX_ISO_PKTS)) { logmsg("bad iso packet count %d", npk); return -1; }

    uctx *x = calloc(1, sizeof(*x));
    int hdr = type == USBDEVFS_URB_TYPE_CONTROL ? 8 : 0;
    x->buf = malloc(hdr + len + 1);
    x->seq = seq; x->client_npk = npk_raw; x->dir_in = dir_in; x->type = type; x->npk = npk;
    if (hdr) memcpy(x->buf, setup, 8);
    if (!dir_in && len > 0 && read_full(c->sock, x->buf + hdr, len) < 0) { free_ctx(x); return -1; }

    uint32_t *lens = NULL;
    if (iso) {
        uint8_t *raw = malloc(npk * 16);
        x->iso_off = malloc(npk * sizeof(uint32_t));
        lens = malloc(npk * sizeof(uint32_t));
        if (read_full(c->sock, raw, npk * 16) < 0) { free(raw); free(lens); free_ctx(x); return -1; }
        for (int i = 0; i < npk; i++) {
            x->iso_off[i] = get32(raw + i * 16);
            lens[i] = get32(raw + i * 16 + 4);
        }
        free(raw);
        if (!dir_in) {
            /* OUT data arrives as the full buffer; repack contiguously for usbdevfs */
            uint8_t *packed = malloc(len + 1);
            size_t o = 0;
            for (int i = 0; i < npk; i++) {
                if (x->iso_off[i] + lens[i] <= (uint32_t)len) memcpy(packed + o, x->buf + x->iso_off[i], lens[i]);
                o += lens[i];
            }
            free(x->buf);
            x->buf = packed;
        }
    }

    if (type == USBDEVFS_URB_TYPE_CONTROL) {
        int st;
        if (intercept_control(c, setup, &st)) {
            send_simple_ret(c, seq, st, npk_raw);
            free_ctx(x);
            return 0;
        }
    }

    unsigned int uflags = 0;
    if ((flags & 0x1) && dir_in) uflags |= USBDEVFS_URB_SHORT_NOT_OK;
    if (flags & 0x40) uflags |= USBDEVFS_URB_ZERO_PACKET;
    if (iso) uflags |= USBDEVFS_URB_ISO_ASAP;

    x->nurbs = iso ? (npk + USBFS_ISO_MAX - 1) / USBFS_ISO_MAX : 1;
    x->urbs = calloc(x->nurbs, sizeof(*x->urbs));
    size_t boff = 0;
    for (int i = 0; i < x->nurbs; i++) {
        int n = iso ? (npk - i * USBFS_ISO_MAX < USBFS_ISO_MAX ? npk - i * USBFS_ISO_MAX : USBFS_ISO_MAX) : 0;
        struct usbdevfs_urb *u = calloc(1, sizeof(*u) + n * sizeof(struct usbdevfs_iso_packet_desc));
        u->type = type;
        u->endpoint = type == USBDEVFS_URB_TYPE_CONTROL ? 0 : (ep | (dir_in ? 0x80 : 0));
        u->flags = uflags;
        u->usercontext = x;
        if (iso) {
            int bl = 0;
            for (int k = 0; k < n; k++) {
                u->iso_frame_desc[k].length = lens[i * USBFS_ISO_MAX + k];
                bl += u->iso_frame_desc[k].length;
            }
            u->number_of_packets = n;
            u->buffer = x->buf + boff;
            u->buffer_length = bl;
            boff += bl;
        } else {
            u->buffer = x->buf;
            u->buffer_length = hdr + len;
        }
        x->urbs[i] = u;
    }
    free(lens);

    pthread_mutex_lock(&c->list_lock);
    x->next = c->pending; c->pending = x; c->npending++;
    int submitted = 0, err = 0;
    for (int i = 0; i < x->nurbs; i++) {
        if (ioctl(c->fd, USBDEVFS_SUBMITURB, x->urbs[i]) < 0) { err = -errno; break; }
        submitted++;
    }
    x->remaining = submitted;
    c->n_sub++;
    if (c->n_sub <= 10 && ep != 0)
        logmsg("submit #%ld ep%02x type %d len %d npk %d", c->n_sub, ep | (dir_in ? 0x80 : 0), type, len, npk);
    if (submitted == 0) {
        c->pending = x->next; c->npending--;
        pthread_mutex_unlock(&c->list_lock);
        logmsg("submit ep%02x type %d failed: %s", ep | (dir_in ? 0x80 : 0), type, strerror(-err));
        if (err == -ENODEV) { c->d->gone = 1; free_ctx(x); return -1; }
        send_simple_ret(c, seq, err, npk_raw);
        free_ctx(x);
        return 0;
    }
    if (submitted < x->nurbs) {
        /* partial iso submit: cancel what went out, the reaped status reports the failure */
        for (int i = 0; i < submitted; i++) ioctl(c->fd, USBDEVFS_DISCARDURB, x->urbs[i]);
    }
    pthread_mutex_unlock(&c->list_lock);
    return 0;
}

static void handle_unlink(conn_t *c, const uint8_t *h)
{
    uint32_t seq = get32(h + 4), target = get32(h + 20);
    int found = 0;
    pthread_mutex_lock(&c->list_lock);
    for (uctx *x = c->pending; x; x = x->next) {
        if (x->seq == target && !x->unlinked) {
            x->unlinked = 1;
            x->unlink_seq = seq;
            for (int i = 0; i < x->nurbs; i++) ioctl(c->fd, USBDEVFS_DISCARDURB, x->urbs[i]);
            found = 1;
            break;
        }
    }
    pthread_mutex_unlock(&c->list_lock);
    if (!found) send_ret_unlink(c, seq, 0);
}

static void on_reaped(conn_t *c, struct usbdevfs_urb *u)
{
    uctx *x = u->usercontext;
    pthread_mutex_lock(&c->list_lock);
    if (--x->remaining > 0) { pthread_mutex_unlock(&c->list_lock); return; }
    for (uctx **pp = &c->pending; *pp; pp = &(*pp)->next)
        if (*pp == x) { *pp = x->next; c->npending--; break; }
    pthread_mutex_unlock(&c->list_lock);

    struct usbdevfs_urb *u0 = x->urbs[0];
    c->n_done++;
    if (u0->status && !x->unlinked) {
        c->n_err++; c->last_err = u0->status; c->last_err_ep = u0->endpoint;
        if (c->log_errs++ < 10) logmsg("ep%02x type %d len %d -> status %d", u0->endpoint, x->type, u0->buffer_length, u0->status);
    }
    if (x->dir_in) c->bytes_in += u0->actual_length > 0 ? u0->actual_length : 0;
#ifdef VERBOSE_CONTROL
    if (x->type == USBDEVFS_URB_TYPE_CONTROL) {
        const uint8_t *st = x->buf;
        char hex[3 * 16 + 1] = "";
        int n = u0->actual_length > 16 ? 16 : u0->actual_length;
        for (int i = 0; i < n; i++) sprintf(hex + 3 * i, "%02x ", st[8 + i]);
        __android_log_print(ANDROID_LOG_INFO, TAG, "ctrl %02x %02x v=%04x i=%04x len=%u -> st=%d act=%d [%s]",
                            st[0], st[1], st[2] | st[3] << 8, st[4] | st[5] << 8, st[6] | st[7] << 8,
                            u0->status, u0->actual_length, hex);
    }
#endif
    if (x->unlinked) send_ret_unlink(c, x->unlink_seq, -ECONNRESET);
    else send_ret_submit(c, x);
    free_ctx(x);
}

static void *reaper_thread(void *arg)
{
    conn_t *c = arg;
    int discarded = 0;
    for (;;) {
        pthread_mutex_lock(&c->list_lock);
        int pending = c->npending;
        if (!c->alive && !discarded) {
            for (uctx *x = c->pending; x; x = x->next)
                for (int i = 0; i < x->nurbs; i++) ioctl(c->fd, USBDEVFS_DISCARDURB, x->urbs[i]);
            discarded = 1;
        }
        pthread_mutex_unlock(&c->list_lock);
        if (!c->alive && pending == 0) break;

        static __thread struct timespec last;
        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec - last.tv_sec >= 3 && c->n_sub != c->logged_sub) {
            last = now;
            c->logged_sub = c->n_sub;
            logmsg("stats: %ld sent, %ld done, %d pending, %ld errors, %.1f MB in", c->n_sub, c->n_done,
                   pending, c->n_err, c->bytes_in / 1048576.0);
        }

        struct pollfd p = { .fd = c->fd, .events = POLLOUT };
        int r = poll(&p, 1, 100);
        if (r < 0 && errno != EINTR) break;
        for (;;) {
            struct usbdevfs_urb *u = NULL;
            if (ioctl(c->fd, USBDEVFS_REAPURBNDELAY, &u) < 0) {
                if (errno == ENODEV) goto gone;
                break;
            }
            on_reaped(c, u);
        }
        if (p.revents & (POLLERR | POLLHUP)) goto gone;
    }
    return NULL;
gone:
    logmsg("%s: device disconnected", c->d->busid);
    c->d->gone = 1;
    c->alive = 0;
    shutdown(c->sock, SHUT_RDWR);
    /* kernel dropped the URBs; free our bookkeeping */
    pthread_mutex_lock(&c->list_lock);
    while (c->pending) { uctx *x = c->pending; c->pending = x->next; free_ctx(x); }
    c->npending = 0;
    pthread_mutex_unlock(&c->list_lock);
    return NULL;
}

static void run_attached(int sock, usbdev *d)
{
    conn_t c = { .sock = sock, .fd = d->fd, .d = d, .alive = 1 };
    pthread_mutex_init(&c.send_lock, NULL);
    pthread_mutex_init(&c.list_lock, NULL);
    claim_all(d);

    pthread_t reaper;
    pthread_create(&reaper, NULL, reaper_thread, &c);

    uint8_t h[48];
    long n_submit = 0;
    while (c.alive && !d->gone) {
        if (read_full(sock, h, sizeof(h)) < 0) break;
        uint32_t cmd = get32(h);
        if (cmd == CMD_SUBMIT) {
            if (handle_submit(&c, h) < 0) break;
            n_submit++;
        } else if (cmd == CMD_UNLINK) {
            handle_unlink(&c, h);
        } else {
            logmsg("unknown command %u", cmd);
            break;
        }
    }
    c.alive = 0;
    shutdown(sock, SHUT_RDWR);
    pthread_join(reaper, NULL);
    release_all(d);
    logmsg("%s: detached (%ld transfers)", d->busid, n_submit);
}

/* ---------- TCP server ---------- */

static void *conn_thread(void *arg)
{
    int s = (int)(intptr_t)arg;
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));

    uint8_t h[8];
    while (read_full(s, h, 8) == 0) {
        uint16_t code = get16(h + 2);
        if (code == OP_REQ_DEVLIST) {
            pthread_mutex_lock(&g_lock);
            int n = 0;
            size_t sz = 12;
            for (int i = 0; i < MAX_DEVS; i++)
                if (g_devs[i].used && !g_devs[i].gone) { n++; sz += 312 + 4 * g_devs[i].numif; }
            uint8_t *m = calloc(1, sz), *p = m + 12;
            put16(m, USBIP_VERSION); put16(m + 2, OP_REP_DEVLIST); put32(m + 8, n);
            for (int i = 0; i < MAX_DEVS; i++) {
                usbdev *d = &g_devs[i];
                if (!d->used || d->gone) continue;
                pack_device(p, d); p += 312;
                for (int k = 0; k < d->numif; k++, p += 4) {
                    p[0] = d->if_cls[k]; p[1] = d->if_sub[k]; p[2] = d->if_proto[k];
                }
            }
            pthread_mutex_unlock(&g_lock);
            write_full(s, m, sz);
            free(m);
        } else if (code == OP_REQ_IMPORT) {
            char busid[33] = {0};
            if (read_full(s, busid, 32) < 0) break;
            usbdev *d = NULL;
            pthread_mutex_lock(&g_lock);
            for (int i = 0; i < MAX_DEVS; i++)
                if (g_devs[i].used && !g_devs[i].gone && !strcmp(g_devs[i].busid, busid)) d = &g_devs[i];
            int ok = d && !d->attached;
            if (ok) { d->attached = 1; d->sock = s; }
            pthread_mutex_unlock(&g_lock);

            uint8_t m[8 + 312] = {0};
            put16(m, USBIP_VERSION); put16(m + 2, OP_REP_IMPORT); put32(m + 4, ok ? 0 : 1);
            if (!ok) {
                logmsg("import %s refused (%s)", busid, d ? "already in use" : "not found");
                write_full(s, m, 8);
                break;
            }
            pack_device(m + 8, d);
            if (write_full(s, m, sizeof(m)) == 0) {
                logmsg("%s %04x:%04x attached by PC", d->busid, d->vid, d->pid);
                run_attached(s, d);
            }
            pthread_mutex_lock(&g_lock);
            d->attached = 0; d->sock = -1;
            pthread_mutex_unlock(&g_lock);
            break;
        } else {
            logmsg("unknown op 0x%04x", code);
            break;
        }
    }
    close(s);
    return NULL;
}

static int g_listen = -1;

static void *accept_thread(void *arg)
{
    (void)arg;
    for (;;) {
        struct sockaddr_in a;
        socklen_t al = sizeof(a);
        int s = accept(g_listen, (struct sockaddr *)&a, &al);
        if (s < 0) { if (errno == EINTR) continue; logmsg("accept: %s", strerror(errno)); break; }
        logmsg("connection from %s", inet_ntoa(a.sin_addr));
        pthread_t t;
        pthread_create(&t, NULL, conn_thread, (void *)(intptr_t)s);
        pthread_detach(t);
    }
    return NULL;
}

/* ---------- JNI ---------- */

JNIEXPORT jint JNICALL Java_com_questusbip_Native_startServer(JNIEnv *env, jclass cls, jint port)
{
    if (g_listen >= 0) return 0;
    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port), .sin_addr.s_addr = INADDR_ANY };
    if (bind(s, (struct sockaddr *)&a, sizeof(a)) < 0 || listen(s, 4) < 0) {
        int e = errno;
        logmsg("cannot listen on port %d: %s", port, strerror(e));
        close(s);
        return -e;
    }
    g_listen = s;
    pthread_t t;
    pthread_create(&t, NULL, accept_thread, NULL);
    pthread_detach(t);
    logmsg("USB/IP server listening on port %d", port);
    return 0;
}

JNIEXPORT jint JNICALL Java_com_questusbip_Native_addDevice(JNIEnv *env, jclass cls, jint fd, jbyteArray desc, jstring name)
{
    const char *nm = (*env)->GetStringUTFChars(env, name, NULL);
    int bus = 0, dev = 0;
    sscanf(nm, "/dev/bus/usb/%d/%d", &bus, &dev);

    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < MAX_DEVS; i++) if (!g_devs[i].used && !g_devs[i].attached) { slot = i; break; }
    if (slot < 0) { pthread_mutex_unlock(&g_lock); (*env)->ReleaseStringUTFChars(env, name, nm); return -1; }

    usbdev *d = &g_devs[slot];
    memset(d, 0, sizeof(*d));
    d->fd = fd; d->sock = -1;
    d->busnum = bus; d->devnum = dev;
    snprintf(d->busid, sizeof(d->busid), "%d-%d", bus, dev);
    snprintf(d->path, sizeof(d->path), "%s", nm);
    d->desclen = (*env)->GetArrayLength(env, desc);
    d->desc = malloc(d->desclen);
    (*env)->GetByteArrayRegion(env, desc, 0, d->desclen, (jbyte *)d->desc);
    int cfg = get_active_config(fd);
    d->cfgval = cfg > 0 ? cfg : 0;
    int sp = ioctl(fd, USBDEVFS_GET_SPEED, 0);
    d->speed = sp > 0 ? sp : 3;
    parse_desc(d);
    d->used = 1;
    pthread_mutex_unlock(&g_lock);

    logmsg("shared %s %04x:%04x speed %d, %d interfaces", d->busid, d->vid, d->pid, d->speed, d->numif);
    (*env)->ReleaseStringUTFChars(env, name, nm);
    return slot;
}

JNIEXPORT void JNICALL Java_com_questusbip_Native_removeDevice(JNIEnv *env, jclass cls, jint slot)
{
    if (slot < 0 || slot >= MAX_DEVS) return;
    usbdev *d = &g_devs[slot];
    pthread_mutex_lock(&g_lock);
    d->gone = 1;
    if (d->attached && d->sock >= 0) shutdown(d->sock, SHUT_RDWR);
    pthread_mutex_unlock(&g_lock);
    for (int i = 0; i < 50 && d->attached; i++) usleep(50000);   /* let the connection wind down */
    pthread_mutex_lock(&g_lock);
    logmsg("%s removed", d->busid);
    free(d->desc);
    d->desc = NULL;
    d->used = 0;
    pthread_mutex_unlock(&g_lock);
}

JNIEXPORT jstring JNICALL Java_com_questusbip_Native_status(JNIEnv *env, jclass cls)
{
    char out[8192];
    int o = 0;
    pthread_mutex_lock(&g_lock);
    o += snprintf(out + o, sizeof(out) - o, "%s\n\nDispositivos compartidos:\n",
                  g_listen >= 0 ? "Servidor activo (puerto 3240)" : "Servidor detenido");
    int any = 0;
    for (int i = 0; i < MAX_DEVS; i++) {
        usbdev *d = &g_devs[i];
        if (!d->used) continue;
        any = 1;
        o += snprintf(out + o, sizeof(out) - o, "  %s  %04x:%04x  %d interfaces%s\n", d->busid, d->vid, d->pid,
                      d->numif, d->attached ? "  [EN USO POR LA PC]" : "");
    }
    if (!any) o += snprintf(out + o, sizeof(out) - o, "  (ninguno)\n");
    pthread_mutex_unlock(&g_lock);

    o += snprintf(out + o, sizeof(out) - o, "\nRegistro:\n");
    pthread_mutex_lock(&g_log_lock);
    for (int i = 0; i < LOG_LINES; i++) {
        const char *l = g_log[(g_log_pos + i) % LOG_LINES];
        if (*l && o < (int)sizeof(out) - 200) o += snprintf(out + o, sizeof(out) - o, "  %s\n", l);
    }
    pthread_mutex_unlock(&g_log_lock);
    return (*env)->NewStringUTF(env, out);
}
