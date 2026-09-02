/* test_http_fetch.c - izbor stream/download rezima i kes na disku */
#include "source.h"
#include "common.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

int main(void)
{
    const char *base = getenv("SRV_URL");
    assert(base);

    source_t *s = source_http_new("test", base, "auto", NULL, NULL, 512);
    assert(s);

    char url[LIB_PATH_MAX], local[LIB_PATH_MAX];

    /* 1. Arhiva + Range -> stream, local je sam URL, nista se ne preuzima. */
    snprintf(url, sizeof url, "%sstrip.cbz", base);
    assert(s->be->fetch(s, url, local, sizeof local, NULL, NULL) == 0);
    assert(!strcmp(local, url) && "arhiva uz Range mora ostati URL");

    /* 2. Nije arhiva -> download, bez obzira na Range. */
    snprintf(url, sizeof url, "%sknjiga.pdf", base);
    assert(s->be->fetch(s, url, local, sizeof local, NULL, NULL) == 0);
    assert(is_url(local) == 0 && "PDF mora zavrsiti kao lokalni fajl");

    struct stat st;
    assert(stat(local, &st) == 0);
    assert(st.st_size == 150000);

    /* 3. Drugi poziv koristi vec preuzeti fajl - ista putanja, bez prenosa. */
    char first[LIB_PATH_MAX];
    snprintf(first, sizeof first, "%s", local);
    time_t mtime_before = st.st_mtime;
    sleep(1);

    assert(s->be->fetch(s, url, local, sizeof local, NULL, NULL) == 0);
    assert(!strcmp(local, first));
    assert(stat(local, &st) == 0);
    assert(st.st_mtime == mtime_before && "postojeci fajl se ne preuzima ponovo");

    source_free(s);
    printf("test_http_fetch OK\n");
    return 0;
}
