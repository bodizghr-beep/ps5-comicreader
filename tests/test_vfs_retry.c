/* test_vfs_retry.c - prekid veze se prezivljava, trajna greska ne odlaze */
#include "vfs_http.h"
#include "common.h"
#include <assert.h>

int main(void)
{
    const char *url = getenv("SRV_FILE_URL");
    assert(url);

    /* Server obara svaki treci zahtjev (SRV_FAIL_EVERY=3 u run skripti).
     * Uz tri pokusaja citanje mora proci do kraja. */
    vfs_http_t *v = vfs_http_new(url);
    assert(v);
    assert(vh_open(NULL, v) == ARCHIVE_OK);

    long total = 0;
    for (;;) {
        const void *b = NULL;
        la_ssize_t  n = vh_read(NULL, v, &b);
        assert(n >= 0 && "prekid veze je smio biti ponovljen, ne prijavljen kao greska");
        if (n == 0)
            break;
        total += n;
    }
    assert(total == 2097152);

    vh_close(NULL, v);
    vfs_http_free(v);

    /* 404 se ne ponavlja - trajna greska mora pasti odmah. */
    char bad[512];
    snprintf(bad, sizeof bad, "%s.nema", url);
    vfs_http_t *v2 = vfs_http_new(bad);
    assert(v2);
    vh_open(NULL, v2);
    const void *b = NULL;
    assert(vh_read(NULL, v2, &b) < 0);
    long req = vfs_http_requests(v2);
    /* HEAD + jedan GET, bez tri ponavljanja. */
    assert(req <= 3);
    vfs_http_free(v2);

    printf("test_vfs_retry OK\n");
    return 0;
}
