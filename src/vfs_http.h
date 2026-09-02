/* vfs_http.h - libarchive izvor koji cita HTTP Range zahtjevima
 *
 * Postoji zbog jedne mjerene cinjenice: puna setnja kroz zaglavlja arhive
 * od 782 MB trazi manje od 1% bajtova. Preuzimanje cijelog fajla prije prve
 * stranice je zato cista steta.
 */
#ifndef VFS_HTTP_H
#define VFS_HTTP_H

#include <archive.h>
#include <stdint.h>

typedef struct vfs_http vfs_http_t;

vfs_http_t *vfs_http_new(const char *url);
void        vfs_http_free(vfs_http_t *v);

/* libarchive callback-ovi. `a` smije biti NULL u testovima. */
int        vh_open (struct archive *a, void *cd);
la_ssize_t vh_read (struct archive *a, void *cd, const void **buf);
la_int64_t vh_skip (struct archive *a, void *cd, la_int64_t req);
la_int64_t vh_seek (struct archive *a, void *cd, la_int64_t off, int whence);
int        vh_close(struct archive *a, void *cd);

int64_t vfs_http_size(const vfs_http_t *v);

/* Broj poslatih HTTP zahtjeva - testovi na ovome tvrde da skip i seek
 * ne salju nista i da kes zaglavlja radi. */
long vfs_http_requests(const vfs_http_t *v);

/* Statistika kesa malih citanja (spec 7.4). */
void vfs_http_cache_stats(const vfs_http_t *v, long *hits, long *misses);

/* Kredencijali se drze ovdje, a ne u URL-u: URL je kljuc u .ps5cr_state
 * (spec 14) i lozinka bi zavrsila u citljivom tekstu na USB-u.
 * Tabela se puni pri startu i poslije toga je samo za citanje, pa je
 * dijeljenje medju nitima bezbjedno. */
void vfs_http_register(const char *url_prefix, const char *user, const char *pass);
void vfs_http_clear_creds(void);

#endif /* VFS_HTTP_H */
