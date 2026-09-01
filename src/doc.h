/* doc.h - apstrakcija nad izvorom stranica
 *
 * Ovde je cela poenta arhitekture: UI i kes ne znaju da li citaju CBZ, CBR
 * ili PDF. Dodavanje MuPDF-a kasnije znaci samo jos jednu implementaciju
 * ove strukture, bez diranja ostatka koda.
 */
#ifndef DOC_H
#define DOC_H

#include <stdint.h>

/* Dekodirana stranica u RGBA8888. Vlasnistvo nad `pixels` prelazi na pozivaoca. */
typedef struct {
    int      width;
    int      height;
    uint8_t *pixels;   /* width * height * 4 bajta */
} doc_page_t;

typedef struct doc doc_t;

typedef struct {
    const char *name;

    /* Vraca 1 ako backend prepoznaje ekstenziju. */
    int (*probe)(const char *path);

    /* Otvara dokument. NULL u slucaju greske. */
    doc_t *(*open)(const char *path);

    int (*page_count)(doc_t *d);

    /* Dekodira stranicu `index` u `out`. 0 = uspeh.
     * Poziva se iz radne niti, mora biti bezbedno po dokumentu
     * (jedan dokument = jedna nit, to garantuje kes). */
    int (*render)(doc_t *d, int index, doc_page_t *out);

    void (*close)(doc_t *d);
} doc_backend_t;

/* Oslobadja bafer stranice. */
void doc_page_free(doc_page_t *p);

/* Bira backend na osnovu putanje. NULL ako format nije podrzan. */
const doc_backend_t *doc_backend_for(const char *path);

/* Vraca 1 ako je fajl uopste podrzan (koristi library skener). */
int doc_is_supported(const char *path);

/* Registrovani backend-i. */
extern const doc_backend_t doc_backend_archive;
#ifdef HAVE_MUPDF
extern const doc_backend_t doc_backend_pdf;
#endif

#endif /* DOC_H */
