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
#include <unistd.h>

#define VH_CHUNK_MIN (4 * 1024)
#define VH_CHUNK_MAX (1024 * 1024)
#define VH_URL_MAX   1024

/* Kesiraju se samo mala citanja - to su zaglavlja. Podaci stranice su
 * veliki i pri ponovnom otvaranju se ionako ne citaju ponovo.
 * Mjereno: 495 offseta i 1.93 MB pokriva arhivu od 504 stranice, pa je
 * 8 MB cetvorostruka rezerva. */
#define VH_CACHE_MAX_CHUNK (64 * 1024)
#define VH_CACHE_BUDGET    (8 * 1024 * 1024)

#define VH_RETRIES  3
#define VH_CRED_MAX 8

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

    /* Kes zaglavlja: bez njega svaki skok unazad ponovo seta kroz arhivu,
     * jer ar_seek_to() radi reopen a cache.c trazi focus-1 pri svakom
     * okretu stranice (spec 7.4). */
    struct vh_ent {
        int64_t  off;
        size_t   len;
        uint8_t *data;
    }       *cache;
    int      n_cache, cap_cache;
    size_t   cache_bytes;
    long     hits, misses;
};

/* ------------------------------------------------------------------ */
/* Kredencijali. Namjerno van URL-a - vidi vfs_http.h.                  */

static struct {
    char prefix[VH_URL_MAX];
    char user[64];
    char pass[64];
} g_creds[VH_CRED_MAX];
static int g_n_creds;

void vfs_http_register(const char *url_prefix, const char *user, const char *pass)
{
    if (!url_prefix || !user || !*user)
        return;
    if (g_n_creds >= VH_CRED_MAX) {
        ERR("vfs_http: tabela kredencijala je puna");
        return;
    }
    snprintf(g_creds[g_n_creds].prefix, sizeof g_creds[0].prefix, "%s", url_prefix);
    snprintf(g_creds[g_n_creds].user,   sizeof g_creds[0].user,   "%s", user);
    snprintf(g_creds[g_n_creds].pass,   sizeof g_creds[0].pass,   "%s", pass ? pass : "");
    g_n_creds++;
    /* Lozinka se namjerno ne loguje. */
    LOG("vfs_http: kredencijali za %s", url_prefix);
}

void vfs_http_clear_creds(void)
{
    memset(g_creds, 0, sizeof g_creds);
    g_n_creds = 0;
}

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

static struct vh_ent *cache_find(vfs_http_t *v, int64_t off, size_t len)
{
    for (int i = 0; i < v->n_cache; i++)
        if (v->cache[i].off == off && v->cache[i].len >= len)
            return &v->cache[i];
    return NULL;
}

static void cache_put(vfs_http_t *v, int64_t off, const uint8_t *data, size_t len)
{
    if (len > VH_CACHE_MAX_CHUNK)
        return;
    if (cache_find(v, off, len))
        return;

    /* FIFO: izbacuju se najstariji upisi, s pocetka niza. */
    int drop = 0;
    while (v->cache_bytes + len > VH_CACHE_BUDGET && drop < v->n_cache) {
        v->cache_bytes -= v->cache[drop].len;
        free(v->cache[drop].data);
        drop++;
    }
    if (drop > 0) {
        memmove(v->cache, v->cache + drop,
                (size_t)(v->n_cache - drop) * sizeof *v->cache);
        v->n_cache -= drop;
    }

    if (v->n_cache == v->cap_cache) {
        int            ncap = v->cap_cache ? v->cap_cache * 2 : 128;
        struct vh_ent *nc   = realloc(v->cache, (size_t)ncap * sizeof *nc);
        if (!nc)
            return;
        v->cache     = nc;
        v->cap_cache = ncap;
    }

    uint8_t *copy = malloc(len);
    if (!copy)
        return;
    memcpy(copy, data, len);

    v->cache[v->n_cache].off  = off;
    v->cache[v->n_cache].len  = len;
    v->cache[v->n_cache].data = copy;
    v->n_cache++;
    v->cache_bytes += len;
}

static void cache_free(vfs_http_t *v)
{
    for (int i = 0; i < v->n_cache; i++)
        free(v->cache[i].data);
    free(v->cache);
    v->cache       = NULL;
    v->n_cache     = v->cap_cache = 0;
    v->cache_bytes = 0;
}

void vfs_http_cache_stats(const vfs_http_t *v, long *hits, long *misses)
{
    if (hits)   *hits   = v ? v->hits : 0;
    if (misses) *misses = v ? v->misses : 0;
}

/* Greske koje vrijedi ponoviti: kratak pad veze, zastoj, polovican
 * odgovor. Trajne (401/403/404) se NE ponavljaju - ponavljanje ih samo
 * odlaze i trosi vrijeme. */
static int curl_err_transient(CURLcode rc)
{
    switch (rc) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_GOT_NOTHING:
        return 1;
    default:
        return 0;
    }
}

/* Jedan Range GET, uz do VH_RETRIES pokusaja. 0 = uspjeh, -1 = greska. */
static int fetch_range(vfs_http_t *v, int64_t off, size_t len)
{
    static const int backoff_ms[VH_RETRIES] = { 500, 2000, 5000 };

    char range[64];
    snprintf(range, sizeof range, "%lld-%lld",
             (long long)off, (long long)(off + (int64_t)len - 1));

    for (int try = 0; try < VH_RETRIES; try++) {
        sink_t s = { v, 0 };

        curl_easy_setopt(v->curl, CURLOPT_URL, v->url);
        curl_easy_setopt(v->curl, CURLOPT_RANGE, range);
        curl_easy_setopt(v->curl, CURLOPT_WRITEFUNCTION, sink_write);
        curl_easy_setopt(v->curl, CURLOPT_WRITEDATA, &s);

        CURLcode rc = curl_easy_perform(v->curl);
        v->requests++;

        long code = 0;
        if (rc == CURLE_OK)
            curl_easy_getinfo(v->curl, CURLINFO_RESPONSE_CODE, &code);

        if (rc == CURLE_OK && (code == 206 || code == 200)) {
            v->buf_len = s.len;
            return 0;
        }

        if (rc == CURLE_OK && (code == 401 || code == 403 || code == 404)) {
            ERR("vfs_http: HTTP %ld, ne ponavljam", code);
            return -1;
        }

        int retryable = (rc != CURLE_OK && curl_err_transient(rc)) ||
                        (rc == CURLE_OK && code >= 500);
        if (!retryable) {
            ERR("vfs_http: %s (HTTP %ld) na offsetu %lld",
                curl_easy_strerror(rc), code, (long long)off);
            return -1;
        }

        if (try + 1 < VH_RETRIES) {
            LOG("vfs_http: smetnja na offsetu %lld, pokusaj %d/%d za %d ms",
                (long long)off, try + 2, VH_RETRIES, backoff_ms[try]);
            usleep((useconds_t)backoff_ms[try] * 1000);
        }
    }

    ERR("vfs_http: %d pokusaja nije uspjelo na offsetu %lld",
        VH_RETRIES, (long long)off);
    return -1;
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

    /* Najduzi prefiks koji odgovara URL-u daje kredencijale. */
    int    best = -1;
    size_t bl   = 0;
    for (int i = 0; i < g_n_creds; i++) {
        size_t pl = strlen(g_creds[i].prefix);
        if (!strncmp(v->url, g_creds[i].prefix, pl) && pl > bl) {
            best = i;
            bl   = pl;
        }
    }
    if (best >= 0) {
        curl_easy_setopt(v->curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(v->curl, CURLOPT_USERNAME, g_creds[best].user);
        curl_easy_setopt(v->curl, CURLOPT_PASSWORD, g_creds[best].pass);
    }

    return v;
}

void vfs_http_free(vfs_http_t *v)
{
    if (!v)
        return;
    if (v->curl)
        curl_easy_cleanup(v->curl);
    cache_free(v);
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

    struct vh_ent *ce = cache_find(v, v->off, want);
    if (ce) {
        v->hits++;
        *buf = ce->data;
        la_ssize_t n = (la_ssize_t)ce->len;
        v->off  += n;
        v->chunk = (v->chunk * 4 > VH_CHUNK_MAX) ? VH_CHUNK_MAX : v->chunk * 4;
        return n;
    }
    v->misses++;

    if (fetch_range(v, v->off, want) != 0) {
        if (a)
            archive_set_error(a, -1, "vfs_http: Range zahtjev nije uspio");
        return -1;
    }

    if (v->buf_len == 0)
        return 0;

    cache_put(v, v->off, v->buf, v->buf_len);

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
