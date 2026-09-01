/* source_http.c - mrezni izvor preko WebDAV-a ili autoindex stranice
 *
 * Listanje je sinhrono i zove se iz glavne petlje. Na LAN-u je PROPFIND
 * jednog foldera desetine milisekundi, a gornja granica je CONNECTTIMEOUT.
 */
#include "source.h"
#include "dav_parse.h"
#include "html_parse.h"
#include "common.h"

#include <curl/curl.h>

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

/* Pravi fetch dolazi u Fazi 3 (Task 13). */
static int http_fetch(source_t *s, const char *path, char *local, size_t len,
                      src_progress_fn cb, void *ud)
{
    (void)path; (void)local; (void)len; (void)cb; (void)ud;
    snprintf(s->err, sizeof s->err, "citanje preko mreze jos nije implementirano");
    return -1;
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
