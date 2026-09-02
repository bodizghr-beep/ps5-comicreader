/* vfs_http.c
 *
 * Adaptivni chunk je mjeren, ne pogodjen (spec 5.3):
 *   fiksnih 4 KB  -> 386 zahtjeva za jednu stranicu od 1.53 MB
 *   fiksnih 1 MB  -> 461 zahtjev za setnju kroz zaglavlja, i 61% fajla preneseno
 * Zato: kreni malo, rasti x4 dok se cita uzastopno, resetuj na svaki skok.
 */
#include "vfs_http.h"
#include "common.h"

#include <curl/curl.h>

#define VH_CHUNK_MIN (4 * 1024)
#define VH_CHUNK_MAX (1024 * 1024)
#define VH_URL_MAX   1024

struct vfs_http {
    char     url[VH_URL_MAX];
    CURL    *curl;

    int64_t  off;        /* logicka pozicija citaca */
    int64_t  size;       /* -1 ako server nije rekao */

    uint8_t *buf;        /* bafer posljednjeg chunka */
    size_t   buf_cap;
    size_t   buf_len;

    size_t   chunk;      /* trenutna velicina zahtjeva */
    long     requests;
};

typedef struct {
    vfs_http_t *v;
    size_t      len;
} sink_t;

static size_t sink_write(void *data, size_t sz, size_t nm, void *ud)
{
    sink_t *s = ud;
    size_t  n = sz * nm;

    if (s->len + n > s->v->buf_cap) {
        size_t   ncap = s->len + n;
        uint8_t *nb   = realloc(s->v->buf, ncap);
        if (!nb)
            return 0;
        s->v->buf     = nb;
        s->v->buf_cap = ncap;
    }
    memcpy(s->v->buf + s->len, data, n);
    s->len += n;
    return n;
}

/* Jedan Range GET. 0 = uspjeh, -1 = greska. */
static int fetch_range(vfs_http_t *v, int64_t off, size_t len)
{
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld",
             (long long)off, (long long)(off + (int64_t)len - 1));

    sink_t s = { v, 0 };

    curl_easy_setopt(v->curl, CURLOPT_URL, v->url);
    curl_easy_setopt(v->curl, CURLOPT_RANGE, range);
    curl_easy_setopt(v->curl, CURLOPT_WRITEFUNCTION, sink_write);
    curl_easy_setopt(v->curl, CURLOPT_WRITEDATA, &s);

    CURLcode rc = curl_easy_perform(v->curl);
    v->requests++;

    if (rc != CURLE_OK) {
        ERR("vfs_http: %s (offset %lld)", curl_easy_strerror(rc), (long long)off);
        return -1;
    }

    long code = 0;
    curl_easy_getinfo(v->curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 206 && code != 200) {
        ERR("vfs_http: HTTP %ld na offsetu %lld", code, (long long)off);
        return -1;
    }

    v->buf_len = s.len;
    return 0;
}

/* HEAD, samo da se sazna velicina. Ako ne uspije, radi se i bez nje. */
static void probe_size(vfs_http_t *v)
{
    /* vh_open() se zove pri svakom reopenu (ar_seek_to -> ar_open). Velicina
     * se ne mijenja, pa bi ponovni HEAD bio ~115 ms cistog gubitka po skoku. */
    if (v->size >= 0)
        return;

    curl_easy_setopt(v->curl, CURLOPT_URL, v->url);
    curl_easy_setopt(v->curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(v->curl, CURLOPT_RANGE, NULL);

    if (curl_easy_perform(v->curl) == CURLE_OK) {
        curl_off_t cl = -1;
        curl_easy_getinfo(v->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 0)
            v->size = (int64_t)cl;
    }
    v->requests++;

    curl_easy_setopt(v->curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(v->curl, CURLOPT_HTTPGET, 1L);
}

vfs_http_t *vfs_http_new(const char *url)
{
    if (!url || !is_url(url))
        return NULL;

    vfs_http_t *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;

    snprintf(v->url, sizeof v->url, "%s", url);
    v->chunk = VH_CHUNK_MIN;
    v->size  = -1;

    v->curl = curl_easy_init();
    if (!v->curl) {
        free(v);
        return NULL;
    }

    curl_easy_setopt(v->curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(v->curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(v->curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(v->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(v->curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* Namjerno bez CURLOPT_TIMEOUT - ubijao bi legitimno spore prenose. */

    return v;
}

void vfs_http_free(vfs_http_t *v)
{
    if (!v)
        return;
    if (v->curl)
        curl_easy_cleanup(v->curl);
    free(v->buf);
    free(v);
}

int64_t vfs_http_size(const vfs_http_t *v)     { return v ? v->size : -1; }
long    vfs_http_requests(const vfs_http_t *v) { return v ? v->requests : 0; }

int vh_open(struct archive *a, void *cd)
{
    (void)a;
    vfs_http_t *v = cd;

    probe_size(v);
    v->off   = 0;
    v->chunk = VH_CHUNK_MIN;
    return ARCHIVE_OK;
}

la_ssize_t vh_read(struct archive *a, void *cd, const void **buf)
{
    vfs_http_t *v = cd;

    if (v->size >= 0 && v->off >= v->size)
        return 0;                         /* EOF */

    size_t want = v->chunk;
    if (v->size >= 0 && v->off + (int64_t)want > v->size)
        want = (size_t)(v->size - v->off);

    if (fetch_range(v, v->off, want) != 0) {
        if (a)
            archive_set_error(a, -1, "vfs_http: Range zahtjev nije uspio");
        return -1;
    }

    if (v->buf_len == 0)
        return 0;

    *buf = v->buf;
    v->off += (int64_t)v->buf_len;

    /* Uzastopno citanje: rasti x4 do gornje granice. */
    v->chunk = (v->chunk * 4 > VH_CHUNK_MAX) ? VH_CHUNK_MAX : v->chunk * 4;

    return (la_ssize_t)v->buf_len;
}

la_int64_t vh_skip(struct archive *a, void *cd, la_int64_t req)
{
    (void)a;
    vfs_http_t *v = cd;

    if (req <= 0)
        return 0;

    if (v->size >= 0 && v->off + req > v->size)
        req = v->size - v->off;

    v->off  += req;
    v->chunk = VH_CHUNK_MIN;      /* skok prekida niz uzastopnih citanja */
    return req;
}

la_int64_t vh_seek(struct archive *a, void *cd, la_int64_t off, int whence)
{
    (void)a;
    vfs_http_t *v = cd;
    int64_t     n;

    switch (whence) {
    case SEEK_SET: n = off; break;
    case SEEK_CUR: n = v->off + off; break;
    case SEEK_END:
        if (v->size < 0)
            return ARCHIVE_FATAL;
        n = v->size + off;
        break;
    default:
        return ARCHIVE_FATAL;
    }

    if (n < 0)
        n = 0;

    v->off   = n;
    v->chunk = VH_CHUNK_MIN;
    return n;
}

int vh_close(struct archive *a, void *cd)
{
    (void)a; (void)cd;
    return ARCHIVE_OK;
}
