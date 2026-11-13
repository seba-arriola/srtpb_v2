/***********************************************************************************
 *  srtpbc - C replay engine for SRTPB
 *
 *  Reads MiniSEED files, builds continuous traces, slices them into
 *  PacketTimeInterval windows and sends the resulting 512-byte STEIM2
 *  records to a SeedLink/DataLink server (or to a file).
 *
 *  This is the high-performance engine used by srtpb.py.  It replaces the
 *  per-trace ObsPy slicing/encoding loop which could not keep up with
 *  thousands of channels in real time.
 ***********************************************************************************/

#define _GNU_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <inttypes.h>
#include <string.h>
#include <math.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/time.h>
#include <sys/socket.h>

#include "libmseed.h"
#include "libdali.h"

#define VERSION "1.0"
#define DEFAULT_HOST "127.0.0.1"
#define DEFAULT_PORT "16000"
#define DEFAULT_DT 1.0

/* ------------------------------------------------------------------------- */
/* Small utilities                                                           */
/* ------------------------------------------------------------------------- */

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + (int64_t)tv.tv_usec;
}

static hptime_t now_hptime(void)
{
    return (hptime_t)now_us();
}

static void sleep_until_us(int64_t target)
{
    int64_t now;
    while ((now = now_us()) < target) {
        int64_t d = target - now;
        struct timespec ts;
        ts.tv_sec = (time_t)(d / 1000000LL);
        ts.tv_nsec = (long)((d % 1000000LL) * 1000LL);
        if (nanosleep(&ts, NULL) != 0 && errno == EINTR)
            continue;
        if (errno != EINTR)
            break;
    }
}

static void iso_time(hptime_t t, char *buf, size_t buflen)
{
    char tmp[64];
    if (!ms_hptime2isotimestr(t, tmp, 1))
        snprintf(tmp, sizeof(tmp), "?");
    snprintf(buf, buflen, "%s", tmp);
}

/* Build an hptime for the date of ref plus a second-of-day value. */
static hptime_t hptime_with_sod(hptime_t ref, double sod)
{
    time_t secs = (time_t)(ref / (hptime_t)HPTMODULUS);
    struct tm tm;
    double whole = floor(sod);
    int h = (int)(whole / 3600.0);
    int m = (int)((whole - h * 3600.0) / 60.0);
    int s = (int)(whole - h * 3600.0 - m * 60.0);

    if (!gmtime_r(&secs, &tm))
        return HPTERROR;

    tm.tm_hour = h;
    tm.tm_min = m;
    tm.tm_sec = s;
    tm.tm_isdst = 0;

    time_t out = timegm(&tm);
    if (out == (time_t)-1)
        return HPTERROR;

    return (hptime_t)out * (hptime_t)HPTMODULUS +
           (hptime_t)llround((sod - whole) * 1000000.0);
}

/* ------------------------------------------------------------------------- */
/* Configuration                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    char *key;
    double value;
} LatencyEntry;

typedef struct {
    char **files;
    int nfiles;
    int files_cap;

    char *host;
    char *port;
    char *output;

    double dt;
    double latency_default;
    int modify;
    int fast;
    int test;
    int verbose;
    int direct;
    double direct_delay;

    hptime_t start_us;
    hptime_t end_us;
    int has_start;
    int has_end;

    double ti_sec;
    double tf_sec;
    int has_ti;
    int has_tf;

    LatencyEntry *latencies;
    int nlatencies;
    int latencies_cap;
} Config;

static void config_init(Config *c)
{
    memset(c, 0, sizeof(*c));
    c->host = strdup(DEFAULT_HOST);
    c->port = strdup(DEFAULT_PORT);
    c->dt = DEFAULT_DT;
    c->latency_default = 0.0;
    c->direct = 0;
    c->direct_delay = -1.0;
}

static void config_free(Config *c)
{
    int i;
    for (i = 0; i < c->nfiles; i++)
        free(c->files[i]);
    free(c->files);
    free(c->host);
    free(c->port);
    free(c->output);
    for (i = 0; i < c->nlatencies; i++)
        free(c->latencies[i].key);
    free(c->latencies);
    memset(c, 0, sizeof(*c));
}

static int config_add_file(Config *c, const char *file)
{
    if (c->nfiles == c->files_cap) {
        int cap = c->files_cap ? c->files_cap * 2 : 16;
        char **p = realloc(c->files, (size_t)cap * sizeof(char *));
        if (!p)
            return 0;
        c->files = p;
        c->files_cap = cap;
    }
    c->files[c->nfiles] = strdup(file);
    if (!c->files[c->nfiles])
        return 0;
    c->nfiles++;
    return 1;
}

static int config_add_latency(Config *c, const char *spec)
{
    const char *eq = strchr(spec, '=');
    char key[128];
    double value;
    char *end = NULL;

    if (!eq) {
        fprintf(stderr, "Invalid latency specification '%s' (expected NET.STA=seconds)\n", spec);
        return 0;
    }
    if ((size_t)(eq - spec) >= sizeof(key)) {
        fprintf(stderr, "Latency station id too long: '%s'\n", spec);
        return 0;
    }
    memcpy(key, spec, (size_t)(eq - spec));
    key[eq - spec] = '\0';

    errno = 0;
    value = strtod(eq + 1, &end);
    if (errno || end == eq + 1 || *end != '\0') {
        fprintf(stderr, "Invalid latency value in '%s'\n", spec);
        return 0;
    }

    if (c->nlatencies == c->latencies_cap) {
        int cap = c->latencies_cap ? c->latencies_cap * 2 : 16;
        LatencyEntry *p = realloc(c->latencies, (size_t)cap * sizeof(LatencyEntry));
        if (!p)
            return 0;
        c->latencies = p;
        c->latencies_cap = cap;
    }
    c->latencies[c->nlatencies].key = strdup(key);
    if (!c->latencies[c->nlatencies].key)
        return 0;
    c->latencies[c->nlatencies].value = value;
    c->nlatencies++;
    return 1;
}

static double latency_for(const Config *c, const char *net, const char *sta)
{
    char key[64];
    int i;
    snprintf(key, sizeof(key), "%s.%s", net, sta);
    for (i = 0; i < c->nlatencies; i++)
        if (!strcmp(c->latencies[i].key, key))
            return c->latencies[i].value;
    return c->latency_default;
}

/* ------------------------------------------------------------------------- */
/* Trace storage                                                             */
/* ------------------------------------------------------------------------- */

typedef struct {
    char net[12];
    char sta[12];
    char loc[12];
    char chan[12];
    char quality;
    char id[64];
    double samprate;
    hptime_t starttime;
    int64_t npts;
    int64_t cap;
    int32_t *data;
    double latency;
    int64_t load_order;
    MSRecord *msr;
} Trace;

#define HASH_SIZE 32768

typedef struct {
    char key[64];
    int idx;
    int used;
} HashEntry;

typedef struct {
    Trace *traces;
    int ntraces;
    int cap;
    HashEntry hash[HASH_SIZE];
    int64_t load_order;
} Engine;

static uint32_t hash_string(const char *s)
{
    uint32_t h = 2166136261u;
    while (*s) {
        h ^= (uint8_t)*s++;
        h *= 16777619u;
    }
    return h;
}

static HashEntry *hash_slot(Engine *e, const char *key, int create)
{
    uint32_t h = hash_string(key) & (HASH_SIZE - 1);
    uint32_t start = h;

    do {
        HashEntry *entry = &e->hash[h];
        if (!entry->used) {
            if (!create)
                return NULL;
            snprintf(entry->key, sizeof(entry->key), "%s", key);
            entry->used = 1;
            entry->idx = -1;
            return entry;
        }
        if (!strcmp(entry->key, key))
            return entry;
        h = (h + 1) & (HASH_SIZE - 1);
    } while (h != start);

    return NULL;
}

static void make_id(char *id, size_t idlen, MSRecord *msr)
{
    snprintf(id, idlen, "%s.%s.%s.%s",
             msr->network, msr->station, msr->location, msr->channel);
}

static int trace_reserve(Trace *t, int64_t need)
{
    int64_t cap;
    int32_t *p;

    if (need <= t->cap)
        return 1;

    cap = t->cap > 0 ? t->cap : 4096;
    while (cap < need) {
        if (cap > need / 2)
            cap = need;
        else
            cap *= 2;
    }
    if ((uint64_t)cap > SIZE_MAX / sizeof(int32_t))
        return 0;
    p = realloc(t->data, (size_t)cap * sizeof(int32_t));
    if (!p)
        return 0;
    t->data = p;
    t->cap = cap;
    return 1;
}

static int trace_append(Trace *t, MSRecord *msr, int64_t off, int64_t n)
{
    int64_t i;

    if (n <= 0)
        return 1;
    if (!msr->datasamples || off < 0 || off + n > msr->numsamples)
        return 0;
    if (!trace_reserve(t, t->npts + n))
        return 0;

    if (msr->sampletype == 'i') {
        memcpy(t->data + t->npts,
               (int32_t *)msr->datasamples + off,
               (size_t)n * sizeof(int32_t));
    } else if (msr->sampletype == 'f') {
        float *src = (float *)msr->datasamples;
        for (i = 0; i < n; i++)
            t->data[t->npts + i] = (int32_t)src[off + i];
    } else if (msr->sampletype == 'd') {
        double *src = (double *)msr->datasamples;
        for (i = 0; i < n; i++)
            t->data[t->npts + i] = (int32_t)src[off + i];
    } else {
        static int warned = 0;
        if (!warned) {
            fprintf(stderr, "Warning: unsupported sample type '%c', records skipped\n",
                    msr->sampletype ? msr->sampletype : '?');
            warned = 1;
        }
        return 0;
    }

    t->npts += n;
    return 1;
}

static hptime_t trace_exclusive_end(Trace *t)
{
    if (t->npts <= 0 || t->samprate <= 0.0)
        return t->starttime;
    return t->starttime +
           (hptime_t)llround((double)t->npts * 1000000.0 / t->samprate);
}

static hptime_t trace_endtime(Trace *t)
{
    if (t->npts <= 0 || t->samprate <= 0.0)
        return t->starttime;
    return t->starttime +
           (hptime_t)llround((double)(t->npts - 1) * 1000000.0 / t->samprate);
}

static Trace *engine_new_trace(Engine *e, const Config *cfg, MSRecord *msr,
                               int64_t off, int64_t n, hptime_t seg_start)
{
    Trace *t;
    HashEntry *slot;

    if (e->ntraces == e->cap) {
        int cap = e->cap ? e->cap * 2 : 256;
        Trace *p = realloc(e->traces, (size_t)cap * sizeof(Trace));
        if (!p)
            return NULL;
        e->traces = p;
        e->cap = cap;
    }

    t = &e->traces[e->ntraces];
    memset(t, 0, sizeof(*t));
    snprintf(t->net, sizeof(t->net), "%s", msr->network);
    snprintf(t->sta, sizeof(t->sta), "%s", msr->station);
    snprintf(t->loc, sizeof(t->loc), "%s", msr->location);
    snprintf(t->chan, sizeof(t->chan), "%s", msr->channel);
    t->quality = msr->dataquality ? msr->dataquality : 'D';
    make_id(t->id, sizeof(t->id), msr);
    t->samprate = msr->samprate;
    t->starttime = seg_start;
    t->latency = latency_for(cfg, t->net, t->sta);
    t->load_order = e->load_order++;

    if (!trace_append(t, msr, off, n)) {
        free(t->data);
        memset(t, 0, sizeof(*t));
        return NULL;
    }

    slot = hash_slot(e, t->id, 1);
    if (!slot) {
        free(t->data);
        memset(t, 0, sizeof(*t));
        return NULL;
    }
    slot->idx = e->ntraces;
    e->ntraces++;
    return t;
}

/*
 * Parse the requested time window out of a record.  The trimming follows
 * ObsPy's read(starttime=..., endtime=...) semantics: nearest sample,
 * inclusive end.  Returns 0 if the record has no samples in the window.
 */
static int trim_record(const Config *cfg, MSRecord *msr,
                       int64_t *off, int64_t *n, hptime_t *seg_start)
{
    int64_t total = msr->numsamples;
    int64_t i0 = 0;
    int64_t i1;
    double fs = msr->samprate;

    if (total <= 0 || fs <= 0.0)
        return 0;

    /*
     * libmseed (and therefore ObsPy's read(starttime=..., endtime=...))
     * selects records by their start/end coverage.  A record that starts
     * after the requested end is not included at all, even if its first
     * sample would be the nearest one to the end time.
     */
    if (cfg->has_end && msr->starttime > cfg->end_us)
        return 0;
    if (cfg->has_start) {
        hptime_t rec_end = msr_endtime(msr);
        if (rec_end != HPTERROR && rec_end < cfg->start_us)
            return 0;
    }

    i1 = total - 1;

    if (cfg->has_start) {
        i0 = (int64_t)llround((double)(cfg->start_us - msr->starttime) * fs / 1000000.0);
        if (i0 < 0)
            i0 = 0;
        if (i0 > total)
            i0 = total;
    }
    if (cfg->has_end) {
        i1 = (int64_t)llround((double)(cfg->end_us - msr->starttime) * fs / 1000000.0);
        if (i1 > total - 1)
            i1 = total - 1;
    }
    if (i0 > i1)
        return 0;

    *off = i0;
    *n = i1 - i0 + 1;
    *seg_start = msr->starttime +
                 (hptime_t)llround((double)i0 * 1000000.0 / fs);
    return 1;
}

static int engine_add_record(Engine *e, const Config *cfg, MSRecord *msr)
{
    int64_t off, n;
    hptime_t seg_start;
    char id[64];
    HashEntry *slot;
    Trace *t;

    if (!trim_record(cfg, msr, &off, &n, &seg_start))
        return 1;

    make_id(id, sizeof(id), msr);
    slot = hash_slot(e, id, 0);
    if (slot && slot->used && slot->idx >= 0) {
        t = &e->traces[slot->idx];
        if (fabs(t->samprate - msr->samprate) <= 1e-9) {
            hptime_t expected = trace_exclusive_end(t);
            double tol = 0.5 * 1000000.0 / msr->samprate;
            int64_t diff = seg_start - expected;

            if (llabs(diff) <= (int64_t)tol) {
                return trace_append(t, msr, off, n);
            } else if (diff < 0) {
                int64_t skip = (int64_t)llround(
                    (double)(expected - seg_start) * msr->samprate / 1000000.0);
                if (skip < n)
                    return trace_append(t, msr, off + skip, n - skip);
                return 1;
            }
        }
    }

    return engine_new_trace(e, cfg, msr, off, n, seg_start) != NULL;
}

static int cmp_trace(const void *pa, const void *pb)
{
    const Trace *a = (const Trace *)pa;
    const Trace *b = (const Trace *)pb;

    if (a->starttime < b->starttime)
        return -1;
    if (a->starttime > b->starttime)
        return 1;
    if (a->load_order < b->load_order)
        return -1;
    if (a->load_order > b->load_order)
        return 1;
    return 0;
}

static void engine_free(Engine *e)
{
    int i;
    for (i = 0; i < e->ntraces; i++) {
        if (e->traces[i].msr) {
            e->traces[i].msr->datasamples = NULL;
            msr_free(&e->traces[i].msr);
        }
        free(e->traces[i].data);
    }
    free(e->traces);
    memset(e, 0, sizeof(*e));
}

/* ------------------------------------------------------------------------- */
/* File scanning / loading                                                   */
/* ------------------------------------------------------------------------- */

static int scan_headers(const Config *cfg, hptime_t *min_start,
                        hptime_t *max_end, int64_t *record_count)
{
    int fi;
    int ok = 1;

    *min_start = HPTERROR;
    *max_end = HPTERROR;
    *record_count = 0;

    for (fi = 0; fi < cfg->nfiles; fi++) {
        const char *file = cfg->files[fi];
        MSFileParam *msfp = NULL;
        MSRecord *msr = NULL;
        off_t fpos = 0;
        int last = 0;
        int ret;

        while ((ret = ms_readmsr_r(&msfp, &msr, file, 0, &fpos, &last,
                                   1, 0, 0)) == MS_NOERROR) {
            hptime_t s, e;

            if (!MS_ISDATAINDICATOR(msr->dataquality))
                continue;
            if (msr->samprate <= 0.0 || msr->samplecnt <= 0)
                continue;

            s = msr->starttime;
            e = msr_endtime(msr);
            if (e == HPTERROR)
                continue;
            if (*min_start == HPTERROR || s < *min_start)
                *min_start = s;
            if (*max_end == HPTERROR || e > *max_end)
                *max_end = e;
            (*record_count)++;
        }
        if (ret != MS_ENDOFFILE) {
            fprintf(stderr, "Error reading %s: %s\n", file, ms_errorstr(ret));
            ok = 0;
        }
        ms_readmsr_r(&msfp, &msr, NULL, 0, NULL, NULL, 0, 0, 0);
    }

    return ok && *min_start != HPTERROR;
}

static int load_traces(const Config *cfg, Engine *e)
{
    int fi;
    int ok = 1;

    e->load_order = 0;

    for (fi = 0; fi < cfg->nfiles; fi++) {
        const char *file = cfg->files[fi];
        MSFileParam *msfp = NULL;
        MSRecord *msr = NULL;
        off_t fpos = 0;
        int last = 0;
        int ret;

        while ((ret = ms_readmsr_r(&msfp, &msr, file, 0, &fpos, &last,
                                   1, 1, 0)) == MS_NOERROR) {
            if (!MS_ISDATAINDICATOR(msr->dataquality))
                continue;
            if (msr->samprate <= 0.0 || msr->numsamples <= 0)
                continue;
            if (!engine_add_record(e, cfg, msr)) {
                fprintf(stderr, "Error storing data from %s\n", file);
                ok = 0;
                break;
            }
        }
        if (ret != MS_ENDOFFILE) {
            fprintf(stderr, "Error reading %s: %s\n", file, ms_errorstr(ret));
            ok = 0;
        }
        ms_readmsr_r(&msfp, &msr, NULL, 0, NULL, NULL, 0, 0, 0);
        if (!ok)
            break;
    }

    return ok;
}

static int prepare_traces(Engine *e, const Config *cfg)
{
    int i;

    if (e->ntraces == 0)
        return 0;

    qsort(e->traces, (size_t)e->ntraces, sizeof(Trace), cmp_trace);

    for (i = 0; i < e->ntraces; i++) {
        Trace *t = &e->traces[i];
        MSRecord *m;

        if (t->npts <= 0)
            continue;

        if ((uint64_t)t->npts <= SIZE_MAX / sizeof(int32_t)) {
            int32_t *p = realloc(t->data, (size_t)t->npts * sizeof(int32_t));
            if (p) {
                t->data = p;
                t->cap = t->npts;
            }
        }

        m = msr_init(NULL);
        if (!m) {
            fprintf(stderr, "Cannot allocate MSRecord\n");
            return 0;
        }
        memcpy(m->network, t->net, sizeof(m->network) - 1);
        m->network[sizeof(m->network) - 1] = '\0';
        memcpy(m->station, t->sta, sizeof(m->station) - 1);
        m->station[sizeof(m->station) - 1] = '\0';
        memcpy(m->location, t->loc, sizeof(m->location) - 1);
        m->location[sizeof(m->location) - 1] = '\0';
        memcpy(m->channel, t->chan, sizeof(m->channel) - 1);
        m->channel[sizeof(m->channel) - 1] = '\0';
        m->dataquality = t->quality;
        m->samprate = t->samprate;
        m->sampletype = 'i';
        m->reclen = 512;
        m->encoding = 11;   /* STEIM2 */
        m->byteorder = 1;   /* big endian */
        {
            struct blkt_1001_s b1001;
            memset(&b1001, 0, sizeof(b1001));
            if (!msr_addblockette(m, (char *)&b1001, sizeof(b1001), 1001, 0)) {
                fprintf(stderr, "Cannot add Blockette 1001\n");
                msr_free(&m);
                return 0;
            }
        }
        t->msr = m;
    }

    (void)cfg;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* DataLink helpers                                                          */
/* ------------------------------------------------------------------------- */

static DLCP *dali_open(const Config *cfg)
{
    char addr[160];
    DLCP *dl;

    snprintf(addr, sizeof(addr), "%s:%s", cfg->host, cfg->port);
    dl = dl_newdlcp(addr, "SRTPB");
    if (!dl)
        return NULL;
    dl_loginit(cfg->verbose ? 4 : 0, NULL, NULL, NULL, NULL);
    if (dl_connect(dl) < 0) {
        fprintf(stderr, "Cannot connect to DataLink server %s\n", addr);
        dl_freedlcp(dl);
        return NULL;
    }
    return dl;
}

static int send_all(int fd, const void *buf, size_t len)
{
    const uint8_t *p = (const uint8_t *)buf;

    while (len > 0) {
        ssize_t n = send(fd, p, len, MSG_NOSIGNAL);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                struct pollfd pfd;
                int pr;
                pfd.fd = fd;
                pfd.events = POLLOUT;
                pfd.revents = 0;
                pr = poll(&pfd, 1, 1000);
                if (pr < 0 && errno == EINTR)
                    continue;
                if (pr <= 0)
                    return 0;
                continue;
            }
            return 0;
        }
        if (n == 0)
            return 0;
        p += n;
        len -= (size_t)n;
    }
    return 1;
}

static int send_buffer(DLCP *dl, const uint8_t *buf, size_t len)
{
    size_t off = 0;

    while (off < len) {
        size_t chunk = len - off;
        if (chunk > 262144)
            chunk = 262144;
        if (!send_all(dl->link, buf + off, chunk))
            return 0;
        off += chunk;
    }
    return 1;
}

static int send_with_reconnect(DLCP **dlp, const uint8_t *buf, size_t len,
                               const Config *cfg)
{
    int attempt;

    if (!*dlp) {
        *dlp = dali_open(cfg);
        if (!*dlp)
            return 0;
    }

    if (send_buffer(*dlp, buf, len))
        return 1;

    fprintf(stderr, "DataLink send failed, reconnecting...\n");
    dl_disconnect(*dlp);

    for (attempt = 0; attempt < 10; attempt++) {
        if (dl_connect(*dlp) >= 0) {
            fprintf(stderr, "Reconnected to %s:%s\n", cfg->host, cfg->port);
            if (send_buffer(*dlp, buf, len))
                return 1;
            dl_disconnect(*dlp);
        }
        sleep(1);
    }

    fprintf(stderr, "Could not reconnect to DataLink server\n");
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Growable byte buffer                                                      */
/* ------------------------------------------------------------------------- */

typedef struct {
    uint8_t *data;
    size_t len;
    size_t cap;
} Buffer;

static int buffer_reserve(Buffer *b, size_t extra)
{
    size_t need = b->len + extra;
    size_t cap;
    uint8_t *p;

    if (need <= b->cap)
        return 1;
    cap = b->cap ? b->cap : 65536;
    while (cap < need)
        cap *= 2;
    p = realloc(b->data, cap);
    if (!p)
        return 0;
    b->data = p;
    b->cap = cap;
    return 1;
}

static int buffer_add(Buffer *b, const void *data, size_t len)
{
    if (!buffer_reserve(b, len))
        return 0;
    memcpy(b->data + b->len, data, len);
    b->len += len;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Packing / sending a window                                                */
/* ------------------------------------------------------------------------- */

typedef struct {
    const Config *cfg;
    Buffer wire;
    FILE *out;
    double cur_sr;
    int64_t records;
    int64_t bytes;
    int error;
} PackContext;

/* Return the record start time.  MiniSEED headers are big-endian. */
static hptime_t record_start_time(const char *record)
{
    BTime bt;
    bt.year = (uint16_t)(((uint8_t)record[20] << 8) | (uint8_t)record[21]);
    bt.day = (uint16_t)(((uint8_t)record[22] << 8) | (uint8_t)record[23]);
    bt.hour = (uint8_t)record[24];
    bt.min = (uint8_t)record[25];
    bt.sec = (uint8_t)record[26];
    bt.unused = 0;
    bt.fract = (uint16_t)(((uint8_t)record[28] << 8) | (uint8_t)record[29]);
    return ms_btime2hptime(&bt);
}

/* Return the microsecond correction from Blockette 1001, if present. */
static int record_usec(const char *record, int reclen)
{
    uint16_t offset = (uint16_t)(((uint8_t)record[46] << 8) |
                                 (uint8_t)record[47]);

    while (offset != 0 && (int)offset + 6 <= reclen) {
        uint16_t type = (uint16_t)(((uint8_t)record[offset] << 8) |
                                   (uint8_t)record[offset + 1]);
        uint16_t next = (uint16_t)(((uint8_t)record[offset + 2] << 8) |
                                   (uint8_t)record[offset + 3]);
        if (type == 1001)
            return (int)(int8_t)record[offset + 5];
        if (next == 0)
            break;
        offset = next;
    }
    return 0;
}

static void record_handler(char *record, int reclen, void *handlerdata)
{
    PackContext *ctx = (PackContext *)handlerdata;
    char streamid[64];
    char header[192];
    uint8_t pre[3];
    int hlen;
    hptime_t rec_start;
    hptime_t rec_end;
    uint16_t nsamp;

    if (ctx->error)
        return;

    ctx->records++;
    ctx->bytes += reclen;

    if (ctx->cfg->test)
        return;

    if (ctx->out) {
        if (fwrite(record, (size_t)reclen, 1, ctx->out) != 1) {
            ctx->error = 1;
            fprintf(stderr, "Error writing output file: %s\n", strerror(errno));
        }
        return;
    }

    if (!ms_recsrcname(record, streamid, 0)) {
        ctx->error = 1;
        fprintf(stderr, "Cannot determine stream id from packed record\n");
        return;
    }

    rec_start = record_start_time(record);
    rec_start += (hptime_t)record_usec(record, reclen);
    nsamp = (uint16_t)(((uint8_t)record[30] << 8) | (uint8_t)record[31]);
    rec_end = rec_start;
    if (ctx->cur_sr > 0.0 && nsamp > 0)
        rec_end = rec_start +
                  (hptime_t)llround((double)(nsamp - 1) * 1000000.0 / ctx->cur_sr);

    hlen = snprintf(header, sizeof(header),
                    "WRITE %s %" PRId64 " %" PRId64 " N %d",
                    streamid, (int64_t)rec_start, (int64_t)rec_end, reclen);
    if (hlen <= 0 || hlen > 255) {
        ctx->error = 1;
        fprintf(stderr, "Invalid DataLink header length\n");
        return;
    }

    pre[0] = 'D';
    pre[1] = 'L';
    pre[2] = (uint8_t)hlen;

    if (!buffer_add(&ctx->wire, pre, 3) ||
        !buffer_add(&ctx->wire, header, (size_t)hlen) ||
        !buffer_add(&ctx->wire, record, (size_t)reclen)) {
        ctx->error = 1;
        fprintf(stderr, "Out of memory building DataLink packet\n");
    }
}

/*
 * The old engine sorted the stream by endtime before the replay loop.
 * Stream.slice() aligns every window to the first trace in the stream,
 * so the reference trace was the one with the earliest end time (ties
 * resolved by the previous starttime sort).  Keep that behavior.
 */
static Trace *find_reference(Engine *e)
{
    Trace *ref = &e->traces[0];
    int i;
    for (i = 1; i < e->ntraces; i++) {
        Trace *t = &e->traces[i];
        hptime_t te = trace_endtime(t);
        hptime_t re = trace_endtime(ref);
        if (te < re ||
            (te == re && t->starttime < ref->starttime) ||
            (te == re && t->starttime == ref->starttime &&
             t->load_order < ref->load_order))
            ref = t;
    }
    return ref;
}

static void align_window(const Trace *ref, hptime_t win_start,
                         hptime_t win_end, hptime_t *aligned_start,
                         hptime_t *aligned_end)
{
    hptime_t ref_start = ref->starttime;
    hptime_t ref_end = trace_endtime((Trace *)ref);
    double fs = ref->samprate;
    int64_t d0, d1;

    d0 = (int64_t)llround((double)(win_start - ref_start) * fs / 1000000.0);
    *aligned_start = ref_start +
                     (hptime_t)llround((double)d0 * 1000000.0 / fs);

    d1 = (int64_t)llround((double)(win_end - ref_end) * fs / 1000000.0);
    *aligned_end = ref_end +
                   (hptime_t)llround((double)d1 * 1000000.0 / fs);
}

static int pack_trace(PackContext *ctx, Trace *t, hptime_t win_start,
                      hptime_t win_end, hptime_t shift, int modify)
{
    double fs = t->samprate;
    int64_t i0, i1, n;
    hptime_t rec_start;
    int64_t packed = 0;
    int rc;

    if (t->npts <= 0 || fs <= 0.0)
        return 1;

    i0 = (int64_t)llround((double)(win_start - t->starttime) * fs / 1000000.0);
    i1 = (int64_t)llround((double)(win_end - t->starttime) * fs / 1000000.0);

    if (i0 < 0)
        i0 = 0;
    if (i1 > t->npts - 1)
        i1 = t->npts - 1;
    if (i0 >= t->npts || i1 < 0)
        return 1;

    /*
     * ObsPy's Trace.slice() returns both endpoints; the old engine then
     * discarded the last sample to avoid an overlap with the next window:
     *   [i0, i1]  ->  [i0, i1)
     * A single-sample slice is kept as-is.
     */
    if (i1 > i0)
        n = i1 - i0;
    else
        n = 1;

    if (n <= 0)
        return 1;

    rec_start = t->starttime +
                (hptime_t)llround((double)i0 * 1000000.0 / fs);
    rec_start -= (hptime_t)llround(t->latency * 1000000.0);
    if (modify)
        rec_start += shift;

    t->msr->starttime = rec_start;
    t->msr->numsamples = n;
    t->msr->samplecnt = n;
    t->msr->datasamples = t->data + i0;
    t->msr->samprate = fs;
    t->msr->sampletype = 'i';
    t->msr->reclen = 512;
    t->msr->encoding = 11;
    t->msr->byteorder = 1;
    t->msr->sequence_number = 1;
    if (t->msr->ststate) {
        t->msr->ststate->packedsamples = 0;
        t->msr->ststate->lastintsample = 0;
        t->msr->ststate->comphistory = 0;
    }

    ctx->cur_sr = fs;
    rc = msr_pack(t->msr, record_handler, ctx, &packed, 1, 0);
    if (rc < 0) {
        fprintf(stderr, "Error packing trace %s at %" PRId64 "\n",
                t->id, (int64_t)rec_start);
        return 0;
    }
    return 1;
}

static FILE *open_output(const char *path)
{
    FILE *f;

    if (!strcmp(path, "-"))
        return stdout;

    f = fopen(path, "ab");
    if (!f)
        fprintf(stderr, "Cannot open output file '%s': %s\n", path, strerror(errno));
    return f;
}

/* ------------------------------------------------------------------------- */
/* Real-time replay                                                          */
/* ------------------------------------------------------------------------- */

static int run_replay(const Config *cfg, Engine *e)
{
    int i, ti;
    hptime_t data_start;
    hptime_t data_end;
    hptime_t tpb;
    hptime_t shift;
    int64_t dt_us;
    int64_t nti;
    int64_t max_lag = 0;
    int64_t total_proc = 0;
    int64_t windows_done = 0;
    DLCP *dl = NULL;
    FILE *out = NULL;
    PackContext ctx;
    Trace *ref;
    int rc = 1;

    if (e->ntraces <= 0) {
        fprintf(stderr, "No data available for replay\n");
        return 1;
    }

    data_start = e->traces[0].starttime;
    data_end = trace_endtime(&e->traces[0]);
    for (i = 1; i < e->ntraces; i++) {
        hptime_t end = trace_endtime(&e->traces[i]);
        if (end > data_end)
            data_end = end;
    }

    if (data_end <= data_start) {
        fprintf(stderr, "Data span is empty\n");
        return 1;
    }

    dt_us = (int64_t)llround(cfg->dt * 1000000.0);
    if (dt_us <= 0) {
        fprintf(stderr, "Packet interval must be greater than zero\n");
        return 1;
    }
    nti = (int64_t)ceil((double)(data_end - data_start) / (double)dt_us);
    if (nti < 1)
        nti = 1;

    if (cfg->verbose) {
        char s1[64], s2[64];
        iso_time(data_start, s1, sizeof(s1));
        iso_time(data_end, s2, sizeof(s2));
        fprintf(stderr, "Traces: %d, samples: ", e->ntraces);
        {
            int64_t total_samples = 0;
            for (i = 0; i < e->ntraces; i++)
                total_samples += e->traces[i].npts;
            fprintf(stderr, "%" PRId64 "\n", total_samples);
        }
        fprintf(stderr, "Span: %s - %s (%d windows of %.3f s)\n",
                s1, s2, (int)nti, cfg->dt);
    }

    if (!cfg->test) {
        if (cfg->output) {
            out = open_output(cfg->output);
            if (!out)
                return 1;
        } else {
            dl = dali_open(cfg);
            if (!dl)
                return 1;
        }
    }

    memset(&ctx, 0, sizeof(ctx));
    ctx.cfg = cfg;
    ctx.out = out;

    tpb = now_hptime();
    shift = cfg->modify ? (tpb - data_start) : 0;
    ref = find_reference(e);

    for (i = 0; i < nti; i++) {
        hptime_t win_start = data_start + (hptime_t)i * (hptime_t)dt_us;
        hptime_t win_end = data_start + (hptime_t)(i + 1) * (hptime_t)dt_us;
        hptime_t aligned_start, aligned_end;
        hptime_t target = tpb + (hptime_t)(i + 1) * (hptime_t)dt_us;
        int64_t before, after;
        int64_t lag;
        int64_t proc;

        align_window(ref, win_start, win_end, &aligned_start, &aligned_end);

        if (!cfg->fast && !cfg->test)
            sleep_until_us((int64_t)target);

        before = now_us();
        ctx.wire.len = 0;
        ctx.error = 0;

        for (ti = 0; ti < e->ntraces; ti++) {
            if (!pack_trace(&ctx, &e->traces[ti], aligned_start,
                            aligned_end, shift, cfg->modify)) {
                ctx.error = 1;
                break;
            }
        }

        if (ctx.error) {
            fprintf(stderr, "Error while packing window %d\n", i);
            goto cleanup;
        }

        if (!cfg->test && !out && ctx.wire.len > 0) {
            if (!send_with_reconnect(&dl, ctx.wire.data, ctx.wire.len, cfg)) {
                fprintf(stderr, "Error sending window %d\n", i);
                goto cleanup;
            }
        }

        after = now_us();
        proc = after - before;
        lag = after - (int64_t)target;
        if (lag > max_lag)
            max_lag = lag;
        total_proc += proc;
        windows_done++;

        if (cfg->verbose && (!cfg->fast && !cfg->test) &&
            (i % 60 == 0 || i == nti - 1)) {
            char ts[64];
            iso_time(target, ts, sizeof(ts));
            fprintf(stderr,
                    "[%d/%d] %s lag=%.3fs proc=%.3fs records=%" PRId64 "\n",
                    i + 1, (int)nti, ts, (double)lag / 1000000.0,
                    (double)proc / 1000000.0, ctx.records);
        }
    }

    if (cfg->verbose) {
        fprintf(stderr,
                "Replay done: %" PRId64 " records, %" PRId64 " bytes, "
                "avg proc %.3fs, max lag %.3fs\n",
                ctx.records, ctx.bytes,
                windows_done ? (double)total_proc / windows_done / 1000000.0 : 0.0,
                (double)max_lag / 1000000.0);
    }

    rc = 0;

cleanup:
    if (out && out != stdout)
        fclose(out);
    if (dl) {
        dl_disconnect(dl);
        dl_freedlcp(dl);
    }
    free(ctx.wire.data);

    if (rc == 0) {
        char s1[64], s2[64], s3[64];
        FILE *summary = (!cfg->output || strcmp(cfg->output, "-"))
                            ? stdout : stderr;
        iso_time(tpb, s1, sizeof(s1));
        iso_time(data_start, s2, sizeof(s2));
        iso_time(data_end, s3, sizeof(s3));
        fprintf(summary,
                "Playback started at: %s\n"
                "Data span: %s - %s\n"
                "Delta (sec): %.6f\n",
                s1, s2, s3, (double)(tpb - data_start) / 1000000.0);
    }

    return rc;
}

/* ------------------------------------------------------------------------- */
/* Direct mode (-d)                                                          */
/* ------------------------------------------------------------------------- */

static int run_direct(const Config *cfg)
{
    int fi;
    int64_t count = 0;
    int64_t bytes = 0;
    DLCP *dl = NULL;
    FILE *out = NULL;
    int rc = 1;

    if (!cfg->test) {
        if (cfg->output) {
            out = open_output(cfg->output);
            if (!out)
                return 1;
        } else {
            dl = dali_open(cfg);
            if (!dl)
                return 1;
        }
    }

    for (fi = 0; fi < cfg->nfiles; fi++) {
        const char *file = cfg->files[fi];
        MSFileParam *msfp = NULL;
        MSRecord *msr = NULL;
        off_t fpos = 0;
        int last = 0;
        int ret;

        while ((ret = ms_readmsr_r(&msfp, &msr, file, 0, &fpos, &last,
                                   1, 1, 0)) == MS_NOERROR) {
            if (!MS_ISDATAINDICATOR(msr->dataquality))
                continue;
            if (!msr->record || msr->reclen <= 0)
                continue;

            if (count > 0 && count % 250 == 0 && cfg->direct_delay > 0.0) {
                struct timespec ts;
                double d = cfg->direct_delay;
                ts.tv_sec = (time_t)d;
                ts.tv_nsec = (long)((d - floor(d)) * 1e9);
                nanosleep(&ts, NULL);
            }

            if (!cfg->test) {
                if (out) {
                    if (fwrite(msr->record, (size_t)msr->reclen, 1, out) != 1) {
                        fprintf(stderr, "Error writing output file: %s\n", strerror(errno));
                        goto cleanup_direct;
                    }
                } else {
                    char streamid[64];
                    if (!ms_recsrcname(msr->record, streamid, 0)) {
                        fprintf(stderr, "Cannot determine stream id for record\n");
                        goto cleanup_direct;
                    }
                    hptime_t rec_end = msr_endtime(msr);
                    if (rec_end == HPTERROR)
                        rec_end = msr->starttime;
                    if (dl_write(dl, msr->record, msr->reclen, streamid,
                                 (dltime_t)msr->starttime,
                                 (dltime_t)rec_end, 0) < 0) {
                        fprintf(stderr, "DataLink write failed\n");
                        goto cleanup_direct;
                    }
                }
            }
            count++;
            bytes += msr->reclen;
        }
        if (ret != MS_ENDOFFILE) {
            fprintf(stderr, "Error reading %s: %s\n", file, ms_errorstr(ret));
            goto cleanup_direct;
        }
        ms_readmsr_r(&msfp, &msr, NULL, 0, NULL, NULL, 0, 0, 0);
    }

    if (cfg->verbose)
        fprintf(stderr, "Direct mode: %" PRId64 " records, %" PRId64 " bytes\n",
                count, bytes);
    rc = 0;

cleanup_direct:
    if (out && out != stdout)
        fclose(out);
    if (dl) {
        dl_disconnect(dl);
        dl_freedlcp(dl);
    }
    return rc;
}

/* ------------------------------------------------------------------------- */
/* Command line                                                              */
/* ------------------------------------------------------------------------- */

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options] -- file1 [file2 ...]\n"
        "\n"
        "Options:\n"
        "  --host HOST            DataLink host (default %s)\n"
        "  --port PORT            DataLink port (default %s)\n"
        "  --dt SECONDS           Packet time interval (default %.1f)\n"
        "  --latency-default SEC  Latency for stations not listed\n"
        "  --latency NET.STA=SEC  Per-station latency (repeatable)\n"
        "  --modify               Shift data times to playback start\n"
        "  --fast                 Replay as fast as possible\n"
        "  --test                 Pack but do not send or write\n"
        "  --output FILE          Write MiniSEED records to FILE ('-' = stdout)\n"
        "  --direct DELAY         Direct mode: send records, sleeping DELAY\n"
        "                         seconds every 250 records\n"
        "  --start-us US          Absolute start time (microseconds since epoch)\n"
        "  --end-us US            Absolute end time (microseconds since epoch)\n"
        "  --ti SECONDS           Start time of day (seconds since 00:00:00)\n"
        "  --tf SECONDS           End time of day (seconds since 00:00:00)\n"
        "  --verbose              Verbose output\n"
        "  --help                 Show this help\n",
        prog, DEFAULT_HOST, DEFAULT_PORT, DEFAULT_DT);
}

static int parse_int64(const char *s, int64_t *out)
{
    char *end = NULL;
    errno = 0;
    *out = strtoll(s, &end, 10);
    return !errno && end != s && *end == '\0';
}

static int parse_double(const char *s, double *out)
{
    char *end = NULL;
    errno = 0;
    *out = strtod(s, &end);
    return !errno && end != s && *end == '\0';
}

static int parse_args(int argc, char **argv, Config *cfg)
{
    int i = 1;
    int files_started = 0;

    while (i < argc) {
        const char *arg = argv[i];

        if (!strcmp(arg, "--")) {
            files_started = 1;
            i++;
            continue;
        }

        if (files_started || arg[0] != '-') {
            if (!config_add_file(cfg, arg)) {
                fprintf(stderr, "Out of memory\n");
                return 0;
            }
            i++;
            continue;
        }

        if (!strcmp(arg, "--host")) {
            if (++i >= argc) goto missing;
            free(cfg->host);
            cfg->host = strdup(argv[i]);
        } else if (!strcmp(arg, "--port")) {
            if (++i >= argc) goto missing;
            free(cfg->port);
            cfg->port = strdup(argv[i]);
        } else if (!strcmp(arg, "--dt")) {
            if (++i >= argc || !parse_double(argv[i], &cfg->dt)) goto bad;
        } else if (!strcmp(arg, "--latency-default")) {
            if (++i >= argc || !parse_double(argv[i], &cfg->latency_default)) goto bad;
        } else if (!strcmp(arg, "--latency")) {
            if (++i >= argc || !config_add_latency(cfg, argv[i])) goto bad;
        } else if (!strcmp(arg, "--modify")) {
            cfg->modify = 1;
        } else if (!strcmp(arg, "--fast")) {
            cfg->fast = 1;
        } else if (!strcmp(arg, "--test")) {
            cfg->test = 1;
        } else if (!strcmp(arg, "--output")) {
            if (++i >= argc) goto missing;
            free(cfg->output);
            cfg->output = strdup(argv[i]);
        } else if (!strcmp(arg, "--direct")) {
            if (++i >= argc || !parse_double(argv[i], &cfg->direct_delay)) goto bad;
            cfg->direct = 1;
        } else if (!strcmp(arg, "--start-us")) {
            if (++i >= argc || !parse_int64(argv[i], &cfg->start_us)) goto bad;
            cfg->has_start = 1;
        } else if (!strcmp(arg, "--end-us")) {
            if (++i >= argc || !parse_int64(argv[i], &cfg->end_us)) goto bad;
            cfg->has_end = 1;
        } else if (!strcmp(arg, "--ti")) {
            if (++i >= argc || !parse_double(argv[i], &cfg->ti_sec)) goto bad;
            cfg->has_ti = 1;
        } else if (!strcmp(arg, "--tf")) {
            if (++i >= argc || !parse_double(argv[i], &cfg->tf_sec)) goto bad;
            cfg->has_tf = 1;
        } else if (!strcmp(arg, "--verbose")) {
            cfg->verbose++;
        } else if (!strcmp(arg, "--help") || !strcmp(arg, "-h")) {
            usage(argv[0]);
            exit(0);
        } else {
            fprintf(stderr, "Unknown option: %s\n", arg);
            usage(argv[0]);
            return 0;
        }
        i++;
        continue;

missing:
        fprintf(stderr, "Missing value for option %s\n", arg);
        return 0;
bad:
        fprintf(stderr, "Invalid value for option %s\n", arg);
        return 0;
    }

    if (cfg->nfiles == 0) {
        fprintf(stderr, "No input files specified\n");
        return 0;
    }
    if (cfg->dt <= 0.0) {
        fprintf(stderr, "Packet interval must be greater than zero\n");
        return 0;
    }
    return 1;
}

static int resolve_time_window(Config *cfg, hptime_t min_start, hptime_t max_end)
{
    hptime_t ref;

    if (cfg->has_ti || cfg->has_tf) {
        ref = cfg->has_start ? cfg->start_us : min_start;
        if (cfg->has_ti) {
            cfg->start_us = hptime_with_sod(ref, cfg->ti_sec);
            if (cfg->start_us == HPTERROR) {
                fprintf(stderr, "Cannot resolve -ti time\n");
                return 0;
            }
            cfg->has_start = 1;
        }
        if (cfg->has_tf) {
            cfg->end_us = hptime_with_sod(ref, cfg->tf_sec);
            if (cfg->end_us == HPTERROR) {
                fprintf(stderr, "Cannot resolve -tf time\n");
                return 0;
            }
            cfg->has_end = 1;
        }
    }

    if (!cfg->has_start)
        cfg->start_us = min_start;
    if (!cfg->has_end)
        cfg->end_us = max_end;

    if (cfg->end_us <= cfg->start_us) {
        fprintf(stderr, "End time must be after start time\n");
        return 0;
    }
    return 1;
}

int main(int argc, char **argv)
{
    Config cfg;
    Engine engine;
    hptime_t min_start, max_end;
    int64_t record_count;
    int ret = 1;

    config_init(&cfg);
    memset(&engine, 0, sizeof(engine));

    if (!parse_args(argc, argv, &cfg)) {
        config_free(&cfg);
        return 1;
    }

    if (!scan_headers(&cfg, &min_start, &max_end, &record_count)) {
        fprintf(stderr, "No MiniSEED data found\n");
        config_free(&cfg);
        return 1;
    }

    if (!resolve_time_window(&cfg, min_start, max_end)) {
        config_free(&cfg);
        return 1;
    }

    if (cfg.verbose) {
        char s1[64], s2[64];
        iso_time(min_start, s1, sizeof(s1));
        iso_time(max_end, s2, sizeof(s2));
        fprintf(stderr, "Scanned %" PRId64 " records, data span %s - %s\n",
                record_count, s1, s2);
        if (cfg.has_ti || cfg.has_tf) {
            iso_time(cfg.start_us, s1, sizeof(s1));
            iso_time(cfg.end_us, s2, sizeof(s2));
            fprintf(stderr, "Requested window: %s - %s\n", s1, s2);
        }
    }

    if (cfg.direct) {
        ret = run_direct(&cfg);
    } else {
        if (!load_traces(&cfg, &engine)) {
            fprintf(stderr, "Error loading traces\n");
            engine_free(&engine);
            config_free(&cfg);
            return 1;
        }
        if (!prepare_traces(&engine, &cfg)) {
            fprintf(stderr, "No data in the requested window\n");
            engine_free(&engine);
            config_free(&cfg);
            return 1;
        }
        ret = run_replay(&cfg, &engine);
        engine_free(&engine);
    }

    config_free(&cfg);
    return ret;
}
