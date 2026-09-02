/* test_vfs_cache.c - drugi prolaz po istim offsetima ne smije na mrezu */
#include "vfs_http.h"
#include "common.h"
#include <assert.h>

/* Oponasa setnju po zaglavljima: mali read pa veliki skip, u krug. */
static void walk(vfs_http_t *v, int steps)
{
    assert(vh_seek(NULL, v, 0, SEEK_SET) == 0);
    for (int i = 0; i < steps; i++) {
        const void *b = NULL;
        la_ssize_t  n = vh_read(NULL, v, &b);
        if (n <= 0)
            break;
        if (vh_skip(NULL, v, 100000) <= 0)
            break;
    }
}

int main(void)
{
    const char *url = getenv("SRV_FILE_URL");
    assert(url);

    vfs_http_t *v = vfs_http_new(url);
    assert(v);
    assert(vh_open(NULL, v) == ARCHIVE_OK);

    walk(v, 15);
    long after_first = vfs_http_requests(v);
    assert(after_first > 10);          /* prvi prolaz stvarno ide na mrezu */

    /* Drugi prolaz po identicnim offsetima: nijedan novi zahtjev. */
    walk(v, 15);
    assert(vfs_http_requests(v) == after_first);

    long hits = 0, misses = 0;
    vfs_http_cache_stats(v, &hits, &misses);
    assert(hits >= 15);

    /* Veliki chunk se NE kesira - inace bi 700 MB arhiva pojela memoriju.
     * Citanje u nizu naraste preko granice i tada mora ici na mrezu. */
    assert(vh_seek(NULL, v, 0, SEEK_SET) == 0);
    for (int i = 0; i < 6; i++) {
        const void *b = NULL;
        vh_read(NULL, v, &b);          /* 4K, 16K, 64K, 256K, 1M, 1M */
    }
    long before = vfs_http_requests(v);
    assert(vh_seek(NULL, v, 0, SEEK_SET) == 0);
    for (int i = 0; i < 6; i++) {
        const void *b = NULL;
        vh_read(NULL, v, &b);
    }
    /* Prva tri (4K, 16K, 64K) su iz kesa, veci nisu. */
    assert(vfs_http_requests(v) > before);

    vh_close(NULL, v);
    vfs_http_free(v);
    printf("test_vfs_cache OK (hits %ld, misses %ld)\n", hits, misses);
    return 0;
}
