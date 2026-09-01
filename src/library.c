/* library.c - navigacijski stek
 *
 * Dubina 0 je sinteticki nivo sa spiskom izvora. Ulazak u izvor je isto
 * sto i ulazak u folder, pa UI ne poznaje pojam "izvora".
 */
#include "library.h"
#include "config.h"
#include "common.h"

#include <sys/stat.h>

#define STATE_FILE ".ps5cr_state"
#define USB_SLOTS  8

static void level_clear(lib_level_t *lv)
{
    free(lv->entries);
    lv->entries = NULL;
    lv->count   = 0;
    lv->err[0]  = '\0';
}

/* Korijen: po jedan red za svaki izvor. */
static void build_root(library_t *l)
{
    lib_level_t *lv = &l->stack[0];

    level_clear(lv);
    lv->src     = NULL;
    lv->path[0] = '\0';
    snprintf(lv->title, sizeof lv->title, "/");

    if (l->n_sources == 0)
        return;

    lv->entries = calloc((size_t)l->n_sources, sizeof *lv->entries);
    if (!lv->entries)
        return;

    for (int i = 0; i < l->n_sources; i++) {
        lib_entry_t *e = &lv->entries[i];
        snprintf(e->name, sizeof e->name, "%s", l->sources[i]->name);
        snprintf(e->path, sizeof e->path, "%s", l->sources[i]->root);
        e->is_dir    = 1;
        e->last_page = -1;
    }
    lv->count = l->n_sources;
}

void library_reset(library_t *l)
{
    memset(l, 0, sizeof *l);
    build_root(l);
}

int library_add_source(library_t *l, source_t *s)
{
    if (!s)
        return -1;
    if (l->n_sources >= LIB_SRC_MAX) {
        ERR("previse izvora, %s ignorisan", s->name);
        source_free(s);
        return -1;
    }
    l->sources[l->n_sources++] = s;
    build_root(l);
    return 0;
}

lib_level_t *library_cur(library_t *l)
{
    return &l->stack[l->depth];
}

/* Popunjava last_page iz ucitanog stanja. */
static void stamp_state(library_t *l, lib_level_t *lv)
{
    for (int i = 0; i < lv->count; i++)
        if (!lv->entries[i].is_dir)
            lv->entries[i].last_page = state_page_for(l, lv->entries[i].path);
}

int library_enter(library_t *l, int index)
{
    lib_level_t *cur = library_cur(l);

    if (index < 0 || index >= cur->count)
        return -1;
    if (!cur->entries[index].is_dir)
        return -1;

    if (l->depth + 1 >= LIB_DEPTH_MAX) {
        LOG("stek dubine %d je pun, ulazak odbijen", LIB_DEPTH_MAX);
        return -1;
    }

    source_t   *src = (l->depth == 0) ? l->sources[index] : cur->src;
    const char *p   = cur->entries[index].path;
    const char *nm  = cur->entries[index].name;

    lib_level_t *nx = &l->stack[l->depth + 1];
    level_clear(nx);
    nx->src    = src;
    nx->sel    = 0;
    nx->scroll = 0;
    snprintf(nx->path,  sizeof nx->path,  "%s", p);
    snprintf(nx->title, sizeof nx->title, "%s", nm);

    if (src->be->list(src, p, &nx->entries, &nx->count) != 0) {
        nx->entries = NULL;
        nx->count   = 0;
        snprintf(nx->err, sizeof nx->err, "%s",
                 src->err[0] ? src->err : "listanje nije uspjelo");
    } else {
        stamp_state(l, nx);
    }

    l->depth++;
    return 0;
}

int library_back(library_t *l)
{
    if (l->depth == 0)
        return 0;

    level_clear(&l->stack[l->depth]);
    l->depth--;
    return 1;
}

void library_breadcrumb(const library_t *l, char *buf, size_t len)
{
    if (l->depth == 0) {
        snprintf(buf, len, "/");
        return;
    }

    size_t o = 0;
    buf[0] = '\0';
    for (int i = 1; i <= l->depth; i++) {
        int w = snprintf(buf + o, len - o, "%s/", l->stack[i].title);
        if (w < 0 || (size_t)w >= len - o)
            break;
        o += (size_t)w;
    }
}

int library_init(library_t *l)
{
    library_reset(l);

    /* Host build: CR_ROOT zamjenjuje /mnt/usbN. */
    const char *ovr = getenv("CR_ROOT");
    if (ovr && *ovr) {
        snprintf(l->root, sizeof l->root, "%s", ovr);
        library_add_source(l, source_usb_new(ovr));
    } else {
        for (int i = 0; i < USB_SLOTS; i++) {
            char        p[LIB_PATH_MAX];
            struct stat st;

            snprintf(p, sizeof p, "/mnt/usb%d", i);
            if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            if (!l->root[0])
                snprintf(l->root, sizeof l->root, "%s", p);
            library_add_source(l, source_usb_new(p));
        }
    }

    char     cpath[LIB_PATH_MAX];
    config_t cfg;
    if (config_find(cpath, sizeof cpath) == 0 && config_load(&cfg, cpath) == 0) {
        for (int i = 0; i < cfg.n_srcs; i++) {
            source_t *s = source_http_new(cfg.srcs[i].name, cfg.srcs[i].url,
                                          cfg.srcs[i].type, cfg.srcs[i].user,
                                          cfg.srcs[i].pass, cfg.cache_mb);
            if (s)
                library_add_source(l, s);
        }
    }

    LOG("izvora: %d", l->n_sources);
    return l->n_sources;
}

void library_free(library_t *l)
{
    for (int i = 0; i < LIB_DEPTH_MAX; i++)
        level_clear(&l->stack[i]);

    for (int i = 0; i < l->n_sources; i++)
        source_free(l->sources[i]);
    l->n_sources = 0;

    free(l->state);
    l->state   = NULL;
    l->n_state = l->cap_state = 0;
}

/* ------------------------------------------------------------------ */
/* Stanje citanja. Format nepromijenjen: "putanja<TAB>stranica".       */
/* Kljuc je entry->path, dakle za mrezu puni URL.                      */

static void state_path(library_t *l, char *buf, size_t len)
{
    const char *root = l->root[0] ? l->root : "/mnt/usb0";
    int n = snprintf(buf, len, "%s/%s", root, STATE_FILE);
    if (n < 0 || (size_t)n >= len)
        snprintf(buf, len, "/mnt/usb0/%s", STATE_FILE);
}

static state_rec_t *state_find(library_t *l, const char *path)
{
    for (int i = 0; i < l->n_state; i++)
        if (!strcmp(l->state[i].path, path))
            return &l->state[i];
    return NULL;
}

int state_page_for(const library_t *l, const char *path)
{
    for (int i = 0; i < l->n_state; i++)
        if (!strcmp(l->state[i].path, path))
            return l->state[i].page;
    return -1;
}

static int state_grow(library_t *l)
{
    if (l->n_state < l->cap_state)
        return 0;

    int          ncap = l->cap_state ? l->cap_state * 2 : 64;
    state_rec_t *ns   = realloc(l->state, (size_t)ncap * sizeof *ns);
    if (!ns)
        return -1;

    l->state     = ns;
    l->cap_state = ncap;
    return 0;
}

void state_load(library_t *l)
{
    char sp[LIB_PATH_MAX + 16];
    state_path(l, sp, sizeof sp);

    FILE *f = fopen(sp, "r");
    if (!f)
        return;

    char line[LIB_PATH_MAX + 32];
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';

        /* Odsjecena putanja bi postala pogresan kljuc i tiho izgubila
         * zapamcenu stranicu, pa se predugacka linija radije preskace. */
        if (strlen(line) >= LIB_PATH_MAX) {
            LOG("stanje: preskacem predugacku putanju");
            continue;
        }

        if (state_grow(l) != 0)
            break;

        memcpy(l->state[l->n_state].path, line, strlen(line) + 1);
        l->state[l->n_state].page = atoi(tab + 1);
        l->n_state++;
    }
    fclose(f);
    LOG("stanje: %d zapisa", l->n_state);
}

void state_save(library_t *l, const char *path, int page)
{
    state_rec_t *r = state_find(l, path);

    if (r) {
        r->page = page;
    } else {
        if (state_grow(l) != 0)
            return;
        snprintf(l->state[l->n_state].path, LIB_PATH_MAX, "%s", path);
        l->state[l->n_state].page = page;
        l->n_state++;
    }

    /* Osvjezi i vidljivi red, da badge "nastavi" odmah bude tacan. */
    lib_level_t *cur = library_cur(l);
    for (int i = 0; i < cur->count; i++)
        if (!strcmp(cur->entries[i].path, path))
            cur->entries[i].last_page = page;

    char sp[LIB_PATH_MAX], tmp[LIB_PATH_MAX + 16];
    state_path(l, sp, sizeof sp);
    snprintf(tmp, sizeof tmp, "%s.tmp", sp);

    /* Upis u privremeni fajl pa rename - gasenje konzole usred upisa
     * ne smije ostaviti polupraznu listu. */
    FILE *f = fopen(tmp, "w");
    if (!f) {
        ERR("ne mogu da upisem stanje u %s", tmp);
        return;
    }
    for (int i = 0; i < l->n_state; i++)
        if (l->state[i].page >= 0)
            fprintf(f, "%s\t%d\n", l->state[i].path, l->state[i].page);
    fclose(f);
    rename(tmp, sp);
}
