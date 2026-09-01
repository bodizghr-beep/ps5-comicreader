/* dav_parse.c
 *
 * Koristi se libxml2, a ne rucni skener, zbog jedne stvari: namespace
 * prefiksi u odgovoru nisu ni stabilni ni jednaki (u istom odgovoru QNAP-a
 * vide se D:, lp1: i lp2:). libxml2 u node->name drzi LOKALNO ime, pa
 * poredjenje po prefiksu uopste ne treba.
 */
#include "dav_parse.h"
#include "common.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

static int is_elem(xmlNode *n, const char *local)
{
    return n->type == XML_ELEMENT_NODE && !xmlStrcmp(n->name, (const xmlChar *)local);
}

static xmlNode *child_elem(xmlNode *p, const char *local)
{
    for (xmlNode *c = p->children; c; c = c->next)
        if (is_elem(c, local))
            return c;
    return NULL;
}

/* collection je ugnijezdjen: propstat > prop > resourcetype > collection */
static int has_elem_deep(xmlNode *p, const char *local)
{
    for (xmlNode *c = p->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE)
            continue;
        if (!xmlStrcmp(c->name, (const xmlChar *)local))
            return 1;
        if (has_elem_deep(c, local))
            return 1;
    }
    return 0;
}

/* Uklanja zavrsnu kosu crtu osim ako je putanja samo "/". */
static void strip_slash(char *s)
{
    size_t n = strlen(s);
    if (n > 1 && s[n - 1] == '/')
        s[n - 1] = '\0';
}

int dav_parse(const char *xml, size_t len, const char *base_url,
              const char *self_href, lib_entry_t **out, int *n)
{
    *out = NULL;
    *n   = 0;

    xmlDoc *doc = xmlReadMemory(xml, (int)len, NULL, NULL,
                                XML_PARSE_NOBLANKS | XML_PARSE_NONET |
                                XML_PARSE_NOERROR  | XML_PARSE_NOWARNING);
    if (!doc)
        return -1;

    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || !is_elem(root, "multistatus")) {
        xmlFreeDoc(doc);
        return -1;
    }

    int          cap = 32, cnt = 0;
    lib_entry_t *arr = calloc((size_t)cap, sizeof *arr);
    if (!arr) {
        xmlFreeDoc(doc);
        return -1;
    }

    char self[LIB_PATH_MAX];
    snprintf(self, sizeof self, "%s", self_href);
    strip_slash(self);

    for (xmlNode *r = root->children; r; r = r->next) {
        if (!is_elem(r, "response"))
            continue;

        xmlNode *h = child_elem(r, "href");
        if (!h)
            continue;

        xmlChar *raw = xmlNodeGetContent(h);
        if (!raw)
            continue;

        char href[LIB_PATH_MAX];
        snprintf(href, sizeof href, "%s", (const char *)raw);
        xmlFree(raw);

        int is_dir = has_elem_deep(r, "collection");

        /* Self-unos: ista putanja kao trazena. */
        char cmp[LIB_PATH_MAX];
        snprintf(cmp, sizeof cmp, "%s", href);
        strip_slash(cmp);
        if (!strcmp(cmp, self))
            continue;

        if (cnt == cap) {
            int          ncap = cap * 2;
            lib_entry_t *na   = realloc(arr, (size_t)ncap * sizeof *na);
            if (!na)
                break;
            arr = na;
            memset(arr + cap, 0, (size_t)(ncap - cap) * sizeof *arr);
            cap = ncap;
        }

        lib_entry_t *e = &arr[cnt];
        memset(e, 0, sizeof *e);
        e->is_dir    = is_dir;
        e->last_page = -1;

        /* URL zadrzava enkodiranje - to je ono sto ide u HTTP zahtjev. */
        if (is_url(href))
            snprintf(e->path, sizeof e->path, "%s", href);
        else
            snprintf(e->path, sizeof e->path, "%s%s", base_url, href);

        /* Ime se dekodira, samo za prikaz. */
        char dec[LIB_PATH_MAX];
        url_decode(dec, sizeof dec, cmp);
        snprintf(e->name, sizeof e->name, "%s", path_base(dec));

        if (!is_dir) {
            char *dot = strrchr(e->name, '.');
            if (dot)
                *dot = '\0';
        }

        if (e->name[0])
            cnt++;
    }

    xmlFreeDoc(doc);

    *out = arr;
    *n   = cnt;
    return 0;
}
