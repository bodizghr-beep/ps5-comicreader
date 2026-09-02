/* test_archive_http.c - ista arhiva lokalno i preko HTTP-a mora dati isto */
#include "doc.h"
#include "common.h"
#include <assert.h>

/* Iz doc_archive.c, samo za testove. */
long vfs_http_requests_of_doc(doc_t *d);

int main(void)
{
    const char *url   = getenv("SRV_CBZ_URL");
    const char *local = getenv("SRV_CBZ_LOCAL");
    assert(url && local);

    const doc_backend_t *be = doc_backend_for(local);
    assert(be);

    doc_t *dl = be->open(local);
    assert(dl);
    int nl = be->page_count(dl);
    assert(nl > 0);

    doc_t *dh = be->open(url);
    assert(dh && "otvaranje preko URL-a mora raditi");
    int nh = be->page_count(dh);
    assert(nh == nl);

    /* Ista stranica mora dati identicne piksele. */
    doc_page_t pl = { 0, 0, NULL }, ph = { 0, 0, NULL };
    assert(be->render(dl, nl / 2, &pl) == 0);
    assert(be->render(dh, nl / 2, &ph) == 0);
    assert(pl.width == ph.width && pl.height == ph.height);
    assert(memcmp(pl.pixels, ph.pixels, (size_t)pl.width * pl.height * 4) == 0);

    /* Kes zaglavlja mora prezivjeti skok unazad, inace svaki okret stranice
     * ponovo seta kroz cijelu arhivu (spec 7.4). */
    long before = vfs_http_requests_of_doc(dh);
    doc_page_t pb = { 0, 0, NULL };
    assert(be->render(dh, 1, &pb) == 0);        /* skok unazad -> reopen */
    doc_page_free(&pb);
    long after = vfs_http_requests_of_doc(dh);
    assert(after - before < 5 && "reopen je smio ici iz kesa, ne na mrezu");

    doc_page_free(&pl);
    doc_page_free(&ph);
    be->close(dl);
    be->close(dh);

    printf("test_archive_http OK (%d stranica, reopen = %ld zahtjeva)\n",
           nl, after - before);
    return 0;
}
