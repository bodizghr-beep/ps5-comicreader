/* config.c
 *
 * Format je namjerno primitivan: kljuc = vrijednost, sekcije [source].
 * Nepoznat kljuc se preskace uz upozorenje - stariji config ne smije
 * oboriti aplikaciju.
 */
#include "config.h"
#include "common.h"

#include <sys/stat.h>

#define CONF_NAME ".ps5cr.conf"
#define USB_SLOTS 8

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;

    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static void set_field(cfg_source_t *sr, const char *key, const char *val)
{
    if      (!strcmp(key, "name")) snprintf(sr->name, sizeof sr->name, "%s", val);
    else if (!strcmp(key, "url"))  snprintf(sr->url,  sizeof sr->url,  "%s", val);
    else if (!strcmp(key, "type")) snprintf(sr->type, sizeof sr->type, "%s", val);
    else if (!strcmp(key, "user")) snprintf(sr->user, sizeof sr->user, "%s", val);
    else if (!strcmp(key, "pass")) snprintf(sr->pass, sizeof sr->pass, "%s", val);
    else LOG("config: nepoznat kljuc '%s' u [source], preskacem", key);
}

int config_load(config_t *c, const char *path)
{
    memset(c, 0, sizeof *c);
    c->cache_mb = 4096;

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[LIB_PATH_MAX + 128];
    int  cur = -1;      /* indeks tekuceg [source] bloka */

    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);

        if (!*s || *s == '#' || *s == ';')
            continue;

        if (!strcmp(s, "[source]")) {
            if (c->n_srcs >= CFG_SRC_MAX) {
                ERR("config: vise od %d izvora, ostatak se ignorise", CFG_SRC_MAX);
                cur = -1;
                continue;
            }
            cur = c->n_srcs++;
            snprintf(c->srcs[cur].type, sizeof c->srcs[cur].type, "auto");
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            LOG("config: linija bez '=' preskocena: %s", s);
            continue;
        }
        *eq = '\0';

        char *key = trim(s);
        char *val = trim(eq + 1);

        if (cur < 0) {
            if (!strcmp(key, "cache_mb"))
                c->cache_mb = atoi(val);
            else
                LOG("config: nepoznat globalni kljuc '%s', preskacem", key);
        } else {
            set_field(&c->srcs[cur], key, val);
        }
    }

    fclose(f);

    /* Lozinka se namjerno ne ispisuje. */
    for (int i = 0; i < c->n_srcs; i++)
        LOG("config: izvor '%s' -> %s (type=%s, auth=%s)",
            c->srcs[i].name, c->srcs[i].url, c->srcs[i].type,
            c->srcs[i].user[0] ? "da" : "ne");

    return 0;
}

int config_find(char *out, size_t len)
{
    for (int i = 0; i < USB_SLOTS; i++) {
        char        p[LIB_PATH_MAX];
        struct stat st;

        snprintf(p, sizeof p, "/mnt/usb%d/%s", i, CONF_NAME);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, len, "%s", p);
            return 0;
        }
    }

    /* Host build: CR_ROOT zamjenjuje /mnt/usbN. */
    const char *ovr = getenv("CR_ROOT");
    if (ovr && *ovr) {
        char        p[LIB_PATH_MAX];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", ovr, CONF_NAME);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, len, "%s", p);
            return 0;
        }
    }
    return -1;
}
