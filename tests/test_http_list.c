/* test_http_list.c - listanje preko WebDAV-a i preko autoindex fallbacka.
 * Server se pokrece iz test skripte, adresa dolazi kroz SRV_URL. */
#include "source.h"
#include "common.h"
#include <assert.h>

int main(void)
{
    const char *url = getenv("SRV_URL");
    assert(url && "SRV_URL mora biti postavljen");

    source_t *s = source_http_new("test", url, "auto", NULL, NULL, 512);
    assert(s);

    lib_entry_t *e = NULL;
    int          n = 0;
    assert(s->be->list(s, s->root, &e, &n) == 0);

    /* Server servira folder sa: Serija/ (folder) i a.cbz (fajl).
     * readme.txt otpada na filtriranju. */
    assert(n == 2);
    assert(e[0].is_dir == 1);
    assert(!strcmp(e[0].name, "Serija"));
    assert(!strcmp(e[1].name, "a"));
    assert(e[1].is_dir == 0);

    /* Ulazak u podfolder mora raditi istim pozivom. */
    lib_entry_t *sub = NULL;
    int          ns  = 0;
    assert(s->be->list(s, e[0].path, &sub, &ns) == 0);
    assert(ns == 1);
    assert(!strcmp(sub[0].name, "b"));

    free(sub);
    free(e);
    source_free(s);

    printf("test_http_list OK (%s)\n", getenv("SRV_NO_DAV") ? "autoindex" : "webdav");
    return 0;
}
