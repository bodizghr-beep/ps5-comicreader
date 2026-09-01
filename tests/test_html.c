/* test_html.c - autoindex parser */
#include "html_parse.h"
#include "source.h"
#include "common.h"
#include <assert.h>

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    assert(b);
    assert(fread(b, 1, (size_t)n, f) == (size_t)n);
    b[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return b;
}

static lib_entry_t *by_name(lib_entry_t *e, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].name, name))
            return &e[i];
    return NULL;
}

int main(void)
{
    size_t len;
    char  *html = slurp("tests/fixtures/autoindex_nginx.html", &len);

    lib_entry_t *e = NULL;
    int          n = 0;

    assert(html_parse(html, len, "http://<ip-nas>:8080", "/STRIPOVI/", &e, &n) == 0);

    /* Prolaze samo relativni linkovi: Serija/, CBR, index.php.
     * Otpadaju: ../ (roditelj), ?C=N;O=D (sortiranje), /STRIPOVI/ (apsolutni). */
    assert(n == 3);

    lib_entry_t *d = by_name(e, n, "Serija");
    assert(d && d->is_dir == 1);
    assert(!strcmp(d->path, "http://<ip-nas>:8080/STRIPOVI/Serija/"));

    lib_entry_t *f = by_name(e, n, "Stripoteka 41-50");
    assert(f && f->is_dir == 0);
    assert(!strcmp(f->path, "http://<ip-nas>:8080/STRIPOVI/Stripoteka%2041-50.cbr"));

    assert(by_name(e, n, "index") != NULL);   /* filtriranje je posao source_filter_sort */

    source_filter_sort(e, &n);
    assert(by_name(e, n, "index") == NULL);
    assert(by_name(e, n, "Serija") != NULL);
    assert(e[0].is_dir == 1);

    free(e);
    free(html);

    /* Prazan ulaz nije greska, samo nula unosa. */
    lib_entry_t *e2 = NULL;
    int          n2 = 0;
    assert(html_parse("", 0, "http://x", "/", &e2, &n2) == 0);
    assert(n2 == 0);
    free(e2);

    printf("test_html OK\n");
    return 0;
}
