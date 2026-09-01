/* source.h - apstrakcija nad izvorom dokumenata
 *
 * Isti obrazac kao doc.h: UI i navigacija ne znaju da li citaju USB ili
 * WebDAV. Dodavanje treceg izvora znaci jos jednu implementaciju ove
 * strukture, bez diranja library.c ni main.c.
 */
#ifndef SOURCE_H
#define SOURCE_H

#include <stdint.h>
#include <stddef.h>

#define LIB_PATH_MAX  1024
#define LIB_TITLE_MAX 192
#define SRC_NAME_MAX  64

/* Jedan red u listi: folder ili dokument. */
typedef struct {
    char name[LIB_TITLE_MAX];   /* za prikaz: ime foldera ili naslov bez ekstenzije */
    char path[LIB_PATH_MAX];    /* USB: /mnt/usb0/x.cbz   HTTP: puni URL */
    int  is_dir;
    int  last_page;             /* -1 ako nije citano */
} lib_entry_t;

typedef struct source source_t;

/* Vraca !=0 da otkaze prenos. */
typedef int (*src_progress_fn)(void *ud, int64_t got, int64_t total);

typedef struct {
    const char *kind;                                  /* "usb" / "http" */

    /* Lista sadrzaj `path`. Alocira niz, pozivalac ga oslobadja sa free().
     * 0 = uspjeh, -1 = greska (opis u s->err). */
    int  (*list)(source_t *s, const char *path, lib_entry_t **out, int *n);

    /* Priprema putanju koju ce dobiti cache_open().
     * USB: kopira `path`. HTTP: URL (stream) ili lokalni fajl (download).
     * 0 = uspjeh, -1 = greska, 1 = korisnik otkazao. */
    int  (*fetch)(source_t *s, const char *path, char *local, size_t len,
                  src_progress_fn cb, void *ud);

    void (*close)(source_t *s);
} source_backend_t;

struct source {
    const source_backend_t *be;
    char  name[SRC_NAME_MAX];     /* prikaz u korijenu stabla */
    char  root[LIB_PATH_MAX];
    char  err[128];               /* posljednja greska, za prikaz u listi */
    void *priv;
};

source_t *source_usb_new(const char *root);

source_t *source_http_new(const char *name, const char *url, const char *type,
                          const char *user, const char *pass, int cache_mb);

void source_free(source_t *s);

/* Folderi prije fajlova, unutar grupe natural_cmp po imenu. */
int source_entry_cmp(const void *a, const void *b);

/* Izbacuje skrivene i nepodrzane unose, pa sortira. Folderi uvijek prolaze.
 * Zajednicko za USB i za mrezu - pravilo filtriranja postoji na jednom mjestu. */
void source_filter_sort(lib_entry_t *arr, int *n);

#endif /* SOURCE_H */
