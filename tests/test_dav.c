/* test_dav.c - PROPFIND parser nad stvarnim odgovorom QNAP-a */
#include "dav_parse.h"
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
    char  *xml = slurp("tests/fixtures/propfind_stripovi.xml", &len);

    lib_entry_t *e = NULL;
    int          n = 0;

    assert(dav_parse(xml, len, "http://<ip-nas>:5000", "/STRIPOVI/", &e, &n) == 0);

    /* Fixture ima 7 <response>; self otpada, ostaje 6. Parser jos ne filtrira. */
    assert(n == 6);

    /* Folder */
    lib_entry_t *d = by_name(e, n, "STRIPOVI");
    assert(d && d->is_dir == 1);
    assert(!strcmp(d->path, "http://<ip-nas>:5000/STRIPOVI/STRIPOVI/"));

    /* Fajl: ime dekodirano, URL ostaje enkodiran */
    lib_entry_t *f = by_name(e, n, "Stripoteka 41-50");
    assert(f && f->is_dir == 0);
    assert(!strcmp(f->path, "http://<ip-nas>:5000/STRIPOVI/Stripoteka%2041-50.cbr"));
    assert(f->last_page == -1);

    /* Zarez i zagrada u imenu ne smiju razbiti parser */
    assert(by_name(e, n, "Stripoteka 0106 bd-3,74MB)") != NULL);

    /* Sistemski folder je jos tu - filtriranje je posao source_filter_sort */
    assert(by_name(e, n, "@Recycle") != NULL);

    /* Poslije filtriranja: @Recycle i index.php otpadaju, oba CBR-a ostaju,
     * folder je prvi. PDF zavisi od HAVE_MUPDF pa se broj namjerno ne tvrdi. */
    source_filter_sort(e, &n);
    assert(e[0].is_dir == 1);
    assert(!strcmp(e[0].name, "STRIPOVI"));
    assert(by_name(e, n, "@Recycle") == NULL);
    assert(by_name(e, n, "index") == NULL);
    assert(by_name(e, n, "Stripoteka 41-50") != NULL);
    assert(by_name(e, n, "Stripoteka 51-60") != NULL);

    free(e);
    free(xml);

    /* Smece na ulazu ne smije rusiti. */
    lib_entry_t *e2 = NULL;
    int          n2 = 0;
    assert(dav_parse("<nije-xml", 9, "http://x", "/", &e2, &n2) == -1);

    printf("test_dav OK\n");
    return 0;
}
