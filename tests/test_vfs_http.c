/* test_vfs_http.c - Range callback-ovi bez libarchive, direktno */
#include "vfs_http.h"
#include "common.h"
#include <assert.h>

int main(void)
{
    const char *url = getenv("SRV_FILE_URL");
    const char *loc = getenv("SRV_FILE_LOCAL");
    assert(url && loc);

    /* Lokalni original, za poredjenje bajt po bajt. */
    FILE *f = fopen(loc, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *ref = malloc((size_t)fsz);
    assert(ref);
    assert(fread(ref, 1, (size_t)fsz, f) == (size_t)fsz);
    fclose(f);

    vfs_http_t *v = vfs_http_new(url);
    assert(v);
    assert(vh_open(NULL, v) == ARCHIVE_OK);
    assert(vfs_http_size(v) == (int64_t)fsz);

    /* 1. Sekvencijalno citanje mora dati identican sadrzaj. */
    long off = 0;
    for (;;) {
        const void *b = NULL;
        la_ssize_t  n = vh_read(NULL, v, &b);
        assert(n >= 0);
        if (n == 0)
            break;
        assert(off + n <= fsz);
        assert(memcmp(ref + off, b, (size_t)n) == 0);
        off += n;
    }
    assert(off == fsz);

    /* 2. Chunk raste x4 od 4 KB do 1 MB: 4K, 16K, 64K, 256K, 1M, 1M, ...
     *    Za fajl od 2 MB to je sest zahtjeva, ne 512. */
    long req_seq = vfs_http_requests(v);
    assert(req_seq <= 8);

    /* 3. seek vraca chunk na pocetnih 4 KB i ne salje zahtjev. */
    long before = vfs_http_requests(v);
    assert(vh_seek(NULL, v, 0, SEEK_SET) == 0);
    assert(vfs_http_requests(v) == before);      /* seek ne salje nista */

    const void *b = NULL;
    la_ssize_t  n = vh_read(NULL, v, &b);
    assert(n == 4096);                            /* resetovan na minimum */
    assert(memcmp(ref, b, 4096) == 0);

    /* 4. skip pomjera poziciju bez zahtjeva. */
    before = vfs_http_requests(v);
    assert(vh_skip(NULL, v, 1000) == 1000);
    assert(vfs_http_requests(v) == before);

    n = vh_read(NULL, v, &b);
    assert(n == 4096);                            /* skip takodje resetuje chunk */
    assert(memcmp(ref + 4096 + 1000, b, 4096) == 0);

    /* 5. Citanje iza kraja fajla je EOF, ne greska. */
    assert(vh_seek(NULL, v, 0, SEEK_END) == (la_int64_t)fsz);
    n = vh_read(NULL, v, &b);
    assert(n == 0);

    assert(vh_close(NULL, v) == ARCHIVE_OK);
    vfs_http_free(v);
    free(ref);

    printf("test_vfs_http OK (%ld zahtjeva za %ld bajtova)\n", req_seq, fsz);
    return 0;
}
