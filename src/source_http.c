/* source_http.c - mrezni izvor preko WebDAV-a ili autoindex stranice
 *
 * Listanje je sinhrono i zove se iz glavne petlje. Na LAN-u je PROPFIND
 * jednog foldera desetine milisekundi, a gornja granica je CONNECTTIMEOUT.
 */
#include "source.h"
#include "dav_parse.h"
#include "html_parse.h"
#include "doc.h"
#include "common.h"

#include <curl/curl.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <unistd.h>

#define CACHE_DIR_DEFAULT "/data/tmp/ps5cr"

typedef struct {
    char url[LIB_PATH_MAX];      /* korijen izvora, uvijek sa zavrsnom / */
    char base[LIB_PATH_MAX];     /* shema+host+port, bez zavrsne / */
    char user[64];
    char pass[64];
    int  use_dav;                /* 1 = PROPFIND, 0 = autoindex */
    int  cache_mb;               /* gornja granica kesa, iz configa */
} http_priv_t;

typedef struct {
    char  *buf;
    size_t len;
} membuf_t;

static size_t sink(void *data, size_t sz, size_t nm, void *ud)
{
    membuf_t *m = ud;
    size_t    n = sz * nm;

    char *nb = realloc(m->buf, m->len + n + 1);
    if (!nb)
        return 0;

    m->buf = nb;
    memcpy(m->buf + m->len, data, n);
    m->len += n;
    m->buf[m->len] = '\0';
    return n;
}

/* method NULL = obican GET. Vraca HTTP status ili -1 na gresku transporta. */
static long http_body(http_priv_t *p, const char *url, const char *method,
                      const char *depth, membuf_t *out)
{
    CURL *c = curl_easy_init();
    if (!c)
        return -1;

    struct curl_slist *hdr = NULL;
    if (depth)
        hdr = curl_slist_append(hdr, "Depth: 1");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    if (method)
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (hdr)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);

    if (p->user[0]) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, p->user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, p->pass);
    }

    CURLcode rc   = curl_easy_perform(c);
    long     code = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else
        LOG("http: %s -> %s", method ? method : "GET", curl_easy_strerror(rc));

    if (hdr)
        curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return code;
}

/* Iz punog URL-a vadi putanju (dio od trece kose crte). */
static void url_path_of(const char *url, char *out, size_t len)
{
    const char *p = strstr(url, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    snprintf(out, len, "%s", p ? p : "/");
}

static void url_base_of(const char *url, char *out, size_t len)
{
    const char *p     = strstr(url, "://");
    const char *slash = p ? strchr(p + 3, '/') : NULL;

    if (!slash) {
        snprintf(out, len, "%s", url);
        return;
    }
    size_t n = (size_t)(slash - url);
    if (n >= len)
        n = len - 1;
    memcpy(out, url, n);
    out[n] = '\0';
}

static int http_list(source_t *s, const char *path, lib_entry_t **out, int *n)
{
    http_priv_t *p  = s->priv;
    membuf_t     mb = { NULL, 0 };

    char url[LIB_PATH_MAX];
    snprintf(url, sizeof url, "%s", path);

    /* Folder mora imati zavrsnu kosu crtu, inace Apache salje redirect. */
    size_t ul = strlen(url);
    if (ul && url[ul - 1] != '/' && ul + 1 < sizeof url) {
        url[ul]     = '/';
        url[ul + 1] = '\0';
    }

    char self[LIB_PATH_MAX];
    url_path_of(url, self, sizeof self);

    s->err[0] = '\0';
    *out = NULL;
    *n   = 0;

    /* auto: prvo PROPFIND, pa fallback na autoindex. Odluka se pamti. */
    if (p->use_dav) {
        long code = http_body(p, url, "PROPFIND", "1", &mb);

        if (code == 207) {
            int r = dav_parse(mb.buf ? mb.buf : "", mb.len, p->base, self, out, n);
            free(mb.buf);
            if (r != 0) {
                snprintf(s->err, sizeof s->err, "neispravan PROPFIND odgovor");
                return -1;
            }
            source_filter_sort(*out, n);
            return 0;
        }

        free(mb.buf);
        mb.buf = NULL;
        mb.len = 0;

        if (code == 401 || code == 403) {
            snprintf(s->err, sizeof s->err,
                     "%ld - provjeri user/pass u .ps5cr.conf", code);
            return -1;
        }
        if (code == 404) {
            snprintf(s->err, sizeof s->err, "404 - putanja ne postoji na serveru");
            return -1;
        }
        if (code < 0) {
            snprintf(s->err, sizeof s->err, "server nije dostupan");
            return -1;
        }
        if (code == 405 || code == 501)
            LOG("http: server ne zna PROPFIND (%ld), prelazim na autoindex", code);
        else
            LOG("http: neocekivan PROPFIND status %ld, probam autoindex", code);
        p->use_dav = 0;
    }

    long code = http_body(p, url, NULL, NULL, &mb);
    if (code != 200) {
        free(mb.buf);
        if (code == 401 || code == 403)
            snprintf(s->err, sizeof s->err,
                     "%ld - provjeri user/pass u .ps5cr.conf", code);
        else if (code < 0)
            snprintf(s->err, sizeof s->err, "server nije dostupan");
        else
            snprintf(s->err, sizeof s->err, "HTTP %ld", code);
        return -1;
    }

    int r = html_parse(mb.buf ? mb.buf : "", mb.len, p->base, self, out, n);
    free(mb.buf);
    if (r != 0) {
        snprintf(s->err, sizeof s->err, "ne mogu da procitam listing");
        return -1;
    }

    source_filter_sort(*out, n);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Priprema fajla za citanje: stream ili pun download.                  */

/* Streaming postoji samo kroz libarchive callback-ove, koje ima jedino
 * doc_archive.c. Sve ostalo (PDF) mora biti pravi fajl na disku. */
static int is_archive_path(const char *path)
{
    return doc_backend_for(path) == &doc_backend_archive;
}

static void cache_dir(char *out, size_t len)
{
    const char *base = getenv("CR_TMP");
    if (!base || !*base)
        base = CACHE_DIR_DEFAULT;

    snprintf(out, len, "%s", base);

    /* mkdir -p, po komponentama. */
    for (char *p = out + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(out, 0755);
        *p = '/';
    }
    mkdir(out, 0755);
}

/* Ime u kesu: hash URL-a + citljivo ime, da se dva istoimena stripa iz
 * razlicitih foldera ne pobrkaju. */
static void cache_name(const char *url, char *out, size_t len)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }

    char dec[LIB_PATH_MAX];
    url_decode(dec, sizeof dec, url);

    char safe[96];
    snprintf(safe, sizeof safe, "%s", path_base(dec));
    for (char *p = safe; *p; p++)
        if (*p == '/' || *p == ' ' || *p == '\'' || *p == '"')
            *p = '_';

    char dir[LIB_PATH_MAX];
    cache_dir(dir, sizeof dir);
    snprintf(out, len, "%s/%08x_%s", dir, h, safe);
}

/* Budzet: manje od cache_mb iz configa i cetvrtine slobodnog prostora.
 * cache_mb je gornja granica, ne obecanje. */
static int64_t cache_budget(const char *dir, int cache_mb)
{
    struct statvfs vfs;
    int64_t        cfg = (int64_t)cache_mb * 1024 * 1024;

    if (statvfs(dir, &vfs) != 0)
        return cfg;

    int64_t freeb   = (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize;
    int64_t quarter = freeb / 4;
    return (cfg < quarter) ? cfg : quarter;
}

/* Izbacuje najstarije po mtime dok se ne oslobodi mjesto za `need`. */
static void cache_evict(const char *dir, int64_t budget, int64_t need)
{
    for (;;) {
        DIR *dp = opendir(dir);
        if (!dp)
            return;

        int64_t total    = 0;
        time_t  oldest_t = 0;
        char    oldest[LIB_PATH_MAX] = { 0 };

        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;

            char        p[LIB_PATH_MAX];
            struct stat st;
            if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p)
                continue;
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
                continue;

            total += st.st_size;
            if (!oldest[0] || st.st_mtime < oldest_t) {
                oldest_t = st.st_mtime;
                snprintf(oldest, sizeof oldest, "%s", p);
            }
        }
        closedir(dp);

        if (total + need <= budget || !oldest[0])
            return;

        LOG("kes: izbacujem %s", path_base(oldest));
        if (unlink(oldest) != 0)
            return;
    }
}

/* Velicina fajla na serveru, ili -1. Treba je cache_evict prije nego sto
 * preuzimanje pocne, da se ne krene pa stane bez prostora.
 * HEAD ide kroz CURLOPT_NOBODY, ne kroz CUSTOMREQUEST - inace curl i dalje
 * ceka tijelo odgovora. */
static int64_t remote_size(http_priv_t *p, const char *url, long *code_out)
{
    CURL *c = curl_easy_init();
    if (!c)
        return -1;

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    if (p->user[0]) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, p->user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, p->pass);
    }

    int64_t sz   = -1;
    long    code = -1;
    if (curl_easy_perform(c) == CURLE_OK) {
        curl_off_t cl = -1;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 0)
            sz = (int64_t)cl;
    }
    curl_easy_cleanup(c);

    if (code_out)
        *code_out = code;
    return sz;
}

typedef struct {
    src_progress_fn cb;
    void           *ud;
    int64_t         base;      /* vec preuzeto prije nastavka */
} prog_t;

static int xferinfo(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                    curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;
    prog_t *p = ud;
    if (!p->cb)
        return 0;
    return p->cb(p->ud, p->base + (int64_t)dlnow, p->base + (int64_t)dltotal);
}

/* 0 = spremno, -1 = greska, 1 = korisnik otkazao. */
static int http_download(source_t *s, const char *url, char *local, size_t len,
                         src_progress_fn cb, void *ud)
{
    http_priv_t *p = s->priv;

    cache_name(url, local, len);

    long    code = 0;
    int64_t want = remote_size(p, url, &code);

    if (code == 401 || code == 403) {
        snprintf(s->err, sizeof s->err,
                 "%ld - provjeri user/pass u .ps5cr.conf", code);
        return -1;
    }
    if (code == 404) {
        snprintf(s->err, sizeof s->err, "404 - fajl ne postoji");
        return -1;
    }

    /* Vec preuzet u cijelosti? Onda nema sta da se radi. */
    struct stat st;
    if (stat(local, &st) == 0 && S_ISREG(st.st_mode) &&
        want > 0 && st.st_size == want) {
        LOG("kes: %s vec preuzet", path_base(local));
        return 0;
    }

    int64_t have = 0;
    if (stat(local, &st) == 0 && S_ISREG(st.st_mode))
        have = st.st_size;

    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(s->err, sizeof s->err, "curl init");
        return -1;
    }

    FILE *f = NULL;
    if (have > 0 && want > 0 && have < want) {
        /* Nastavak prekinutog preuzimanja - za 780 MB preko WiFi-ja ovo je
         * razlika izmedju smetnje i pocinjanja iz pocetka. */
        f = fopen(local, "ab");
        curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)have);
        LOG("nastavljam preuzimanje od %lld B", (long long)have);
    } else {
        char dir[LIB_PATH_MAX];
        cache_dir(dir, sizeof dir);
        cache_evict(dir, cache_budget(dir, p->cache_mb), want > 0 ? want : 0);
        f    = fopen(local, "wb");
        have = 0;
    }

    if (!f) {
        curl_easy_cleanup(c);
        snprintf(s->err, sizeof s->err, "ne mogu da pisem u kes");
        return -1;
    }

    prog_t pr = { cb, ud, have };

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferinfo);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pr);

    if (p->user[0]) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, p->user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, p->pass);
    }

    CURLcode rc = curl_easy_perform(c);
    fclose(f);
    curl_easy_cleanup(c);

    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        LOG("preuzimanje otkazano, djelimican fajl ostaje za nastavak");
        return 1;
    }
    if (rc != CURLE_OK) {
        snprintf(s->err, sizeof s->err, "%s", curl_easy_strerror(rc));
        return -1;
    }
    return 0;
}

static int http_fetch(source_t *s, const char *path, char *local, size_t len,
                      src_progress_fn cb, void *ud)
{
    http_priv_t *p = s->priv;

    s->err[0] = '\0';

    /* PDF i sve sto nije arhiva - uvijek download, jer streaming ide kroz
     * libarchive callback-ove koje ima samo doc_archive.c. */
    if (!is_archive_path(path))
        return http_download(s, path, local, len, cb, ud);

    /* Podrzava li server Range? Jedan zahtjev od jednog bajta je dovoljan. */
    membuf_t mb   = { NULL, 0 };
    CURL    *c    = curl_easy_init();
    long     code = 0;

    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, path);
        curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &mb);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        if (p->user[0]) {
            curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
            curl_easy_setopt(c, CURLOPT_USERNAME, p->user);
            curl_easy_setopt(c, CURLOPT_PASSWORD, p->pass);
        }
        if (curl_easy_perform(c) == CURLE_OK)
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
    }
    free(mb.buf);

    if (code == 206) {
        /* Stream: cache_open dobija sam URL, disk se ne dira. */
        snprintf(local, len, "%s", path);
        return 0;
    }

    LOG("server ne podrzava Range (status %ld), prelazim na pun download", code);
    return http_download(s, path, local, len, cb, ud);
}

static void http_close(source_t *s)
{
    free(s->priv);
    s->priv = NULL;
}

static const source_backend_t http_be = {
    "http", http_list, http_fetch, http_close
};

source_t *source_http_new(const char *name, const char *url, const char *type,
                          const char *user, const char *pass, int cache_mb)
{
    if (!url || !is_url(url)) {
        ERR("izvor '%s': url mora pocinjati sa http://", name ? name : "?");
        return NULL;
    }

    source_t    *s = calloc(1, sizeof *s);
    http_priv_t *p = calloc(1, sizeof *p);
    if (!s || !p) {
        free(s);
        free(p);
        return NULL;
    }

    snprintf(p->url, sizeof p->url, "%s", url);
    size_t ul = strlen(p->url);
    if (ul && p->url[ul - 1] != '/' && ul + 1 < sizeof p->url) {
        p->url[ul]     = '/';
        p->url[ul + 1] = '\0';
    }
    url_base_of(p->url, p->base, sizeof p->base);

    if (user) snprintf(p->user, sizeof p->user, "%s", user);
    if (pass) snprintf(p->pass, sizeof p->pass, "%s", pass);
    p->cache_mb = cache_mb > 0 ? cache_mb : 4096;

    /* type: auto i webdav krecu od PROPFIND-a, autoindex ga preskace. */
    p->use_dav = !(type && !strcmp(type, "autoindex"));

    s->be   = &http_be;
    s->priv = p;
    snprintf(s->name, sizeof s->name, "%s", name && *name ? name : "mreza");
    snprintf(s->root, sizeof s->root, "%s", p->url);

    return s;
}
