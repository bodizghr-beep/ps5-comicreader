/* source_http.c - mrezni izvor
 *
 * Privremeni stub. Faza 2 plana (Task 8) donosi listanje preko WebDAV-a i
 * autoindex stranice, Faza 3 (Task 13) citanje preko Range zahtjeva.
 * Do tada library_init preskace mrezne izvore iz configa.
 */
#include "source.h"
#include "common.h"

source_t *source_http_new(const char *name, const char *url, const char *type,
                          const char *user, const char *pass, int cache_mb)
{
    (void)url; (void)type; (void)user; (void)pass; (void)cache_mb;
    LOG("http izvor '%s' jos nije implementiran", name);
    return NULL;
}
