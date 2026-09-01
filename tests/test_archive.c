/* test_archive.c - offline provera backend-a bez SDL-a.
 *   cc -Isrc -o t tests/test_archive.c src/common.c src/doc.c \
 *      src/doc_archive.c src/stb_impl.c -larchive -lm
 */
#include "doc.h"
#include "common.h"

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "upotreba: %s <fajl>\n", argv[0]);
        return 1;
    }

    /* 1. provera prirodnog sortiranja */
    struct { const char *a, *b; int want; } cases[] = {
        { "page2.jpg",  "page10.jpg", -1 },
        { "page10.jpg", "page2.jpg",   1 },
        { "p01.jpg",    "p1.jpg",      0 },
        { "a.jpg",      "b.jpg",      -1 },
        { "img9.png",   "img10.png",  -1 },
    };
    int fails = 0;
    for (size_t i = 0; i < sizeof(cases)/sizeof(cases[0]); i++) {
        int got = natural_cmp(cases[i].a, cases[i].b);
        got = (got > 0) - (got < 0);
        if (got != cases[i].want) {
            printf("FAIL natural_cmp(%s,%s) = %d, ocekivano %d\n",
                   cases[i].a, cases[i].b, got, cases[i].want);
            fails++;
        }
    }
    printf("natural_cmp: %s\n", fails ? "GRESKE" : "ok");

    /* 2. otvaranje i dekodiranje */
    const doc_backend_t *be = doc_backend_for(argv[1]);
    if (!be) {
        printf("FAIL: nema backend-a za %s\n", argv[1]);
        return 1;
    }
    printf("backend: %s\n", be->name);

    doc_t *d = be->open(argv[1]);
    if (!d) {
        printf("FAIL: open\n");
        return 1;
    }

    int n = be->page_count(d);
    printf("stranica: %d\n", n);

    /* Sekvencijalno, pa skok unazad - testira reopen putanju. */
    int order[64], k = 0;
    for (int i = 0; i < n && k < 60; i++)
        order[k++] = i;
    if (n > 2) {
        order[k++] = 1;
        order[k++] = 0;
        order[k++] = n - 1;
    }

    for (int i = 0; i < k; i++) {
        doc_page_t p = { 0, 0, NULL };
        if (be->render(d, order[i], &p) != 0) {
            printf("FAIL: render(%d)\n", order[i]);
            fails++;
            continue;
        }
        printf("  [%d] %dx%d  prvi piksel rgba=%02x%02x%02x%02x\n",
               order[i], p.width, p.height,
               p.pixels[0], p.pixels[1], p.pixels[2], p.pixels[3]);
        doc_page_free(&p);
    }

    be->close(d);
    printf("%s\n", fails ? "== GRESKE ==" : "== sve ok ==");
    return fails ? 1 : 0;
}
