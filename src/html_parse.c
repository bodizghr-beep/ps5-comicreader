/* html_parse.c
 *
 * Namjerno NIJE HTML parser nego skener href atributa. Autoindex stranice
 * su generisane i predvidive, a pun parser bi bio treca zavisnost za
 * posao koji staje u sto linija.
 */
#include "html_parse.h"
#include "common.h"

/* Linkovi koje autoindex generise a nisu sadrzaj foldera. */
static int href_skip(const char *h)
{
    if (!*h)                      return 1;
    if (*h == '?' || *h == '#')   return 1;   /* sortiranje kolona */
    if (*h == '/')                return 1;   /* apsolutni "Parent Directory" */
    if (!strncmp(h, "..", 2))     return 1;
    if (strstr(h, "://"))         return 1;   /* link van ovog foldera */
    return 0;
}

int html_parse(const char *html, size_t len, const char *base_url,
               const char *self_href, lib_entry_t **out, int *n)
{
    *out = NULL;
    *n   = 0;

    int          cap = 32, cnt = 0;
    lib_entry_t *arr = calloc((size_t)cap, sizeof *arr);
    if (!arr)
        return -1;

    const char *p   = html;
    const char *end = html + len;

    while (p < end) {
        const char *h = strstr(p, "href=");
        if (!h || h >= end)
            break;

        h += 5;
        char quote = *h;
        if (quote != '"' && quote != '\'') {
            p = h;
            continue;
        }
        h++;

        const char *e2 = strchr(h, quote);
        if (!e2 || e2 >= end)
            break;

        size_t hl = (size_t)(e2 - h);
        if (hl >= LIB_PATH_MAX) {
            p = e2 + 1;
            continue;
        }

        char href[LIB_PATH_MAX];
        memcpy(href, h, hl);
        href[hl] = '\0';
        p = e2 + 1;

        if (href_skip(href))
            continue;

        int is_dir = (hl > 0 && href[hl - 1] == '/');

        if (cnt == cap) {
            int          ncap = cap * 2;
            lib_entry_t *na   = realloc(arr, (size_t)ncap * sizeof *na);
            if (!na)
                break;
            arr = na;
            memset(arr + cap, 0, (size_t)(ncap - cap) * sizeof *arr);
            cap = ncap;
        }

        lib_entry_t *ent = &arr[cnt];
        memset(ent, 0, sizeof *ent);
        ent->is_dir    = is_dir;
        ent->last_page = -1;

        /* URL ostaje enkodiran; self_href je vec enkodiran put foldera. */
        snprintf(ent->path, sizeof ent->path, "%s%s%s", base_url, self_href, href);

        char dec[LIB_PATH_MAX];
        url_decode(dec, sizeof dec, href);

        size_t dl = strlen(dec);
        if (dl && dec[dl - 1] == '/')
            dec[dl - 1] = '\0';

        snprintf(ent->name, sizeof ent->name, "%s", path_base(dec));

        if (!is_dir) {
            char *dot = strrchr(ent->name, '.');
            if (dot)
                *dot = '\0';
        }

        if (ent->name[0])
            cnt++;
    }

    *out = arr;
    *n   = cnt;
    return 0;
}
