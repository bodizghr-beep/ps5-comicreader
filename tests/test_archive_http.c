/* test_archive_http.c - ista arhiva lokalno i preko HTTP-a mora dati isto */
#include "doc.h"
#include "common.h"
#include <assert.h>

/* Iz doc_archive.c, samo za testove. */
long vfs_http_requests_of_doc(doc_t *d);

/* Napredak otvaranja: traka u UI-ju racuna pos/total, pa pos ne smije da
 * opada - inace se traka vraca unazad. */
typedef struct {
    int     calls;
    int64_t last_pos, total;
    int     last_pages, max_pages;
    int     regress_pos, regress_pages;
    int     cancel_after;      /* 0 = ne otkazuj */
} prog_t;

static int rec_progress(void *ud, int64_t pos, int64_t total, int pages)
{
    prog_t *p = ud;

    if (p->calls && pos   < p->last_pos)   p->regress_pos++;
    if (p->calls && pages < p->last_pages) p->regress_pages++;

    p->calls++;
    p->last_pos   = pos;
    p->last_pages = pages;
    p->total      = total;
    if (pages > p->max_pages)
        p->max_pages = pages;

    return p->cancel_after && p->calls >= p->cancel_after;
}

int main(void)
{
    const char *url   = getenv("SRV_CBZ_URL");
    const char *local = getenv("SRV_CBZ_LOCAL");
    assert(url && local);

    const doc_backend_t *be = doc_backend_for(local);
    assert(be);

    doc_t *dl = be->open(local, NULL, NULL);
    assert(dl);
    int nl = be->page_count(dl);
    assert(nl > 0);

    prog_t pr;
    memset(&pr, 0, sizeof pr);

    doc_t *dh = be->open(url, rec_progress, &pr);
    assert(dh && "otvaranje preko URL-a mora raditi");
    int nh = be->page_count(dh);
    assert(nh == nl);

    /* Napredak: javljen bar jednom po stranici, monotono, sa poznatom
     * velicinom fajla i konacnim brojem stranica koji se poklapa. */
    assert(pr.calls >= nl);
    assert(pr.regress_pos   == 0 && "pos je nazadovao - traka bi isla unatrag");
    assert(pr.regress_pages == 0 && "broj stranica je nazadovao");
    assert(pr.total > 0 && "velicina fajla mora doci iz Content-Length");
    assert(pr.last_pos <= pr.total);
    assert(pr.max_pages == nl);

    /* Otkazivanje: callback vrati != 0 i otvaranje mora stati. */
    prog_t cx;
    memset(&cx, 0, sizeof cx);
    cx.cancel_after = 3;

    doc_t *dc = be->open(url, rec_progress, &cx);
    assert(!dc && "otkazano otvaranje mora vratiti NULL");
    assert(cx.calls == 3 && "poslije otkazivanja se ne smije citati dalje");

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
