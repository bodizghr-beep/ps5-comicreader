/* dav_parse.h - WebDAV PROPFIND multistatus -> lib_entry_t[] */
#ifndef DAV_PARSE_H
#define DAV_PARSE_H

#include "source.h"

/* base_url: shema i host bez zavrsne kose crte, npr. "http://<ip-nas>:5000"
 * self_href: trazena putanja, npr. "/STRIPOVI/" - taj unos se preskace
 *
 * Vraca 0 i alocira niz (pozivalac ga oslobadja sa free()), -1 na neispravan XML.
 * NE filtrira i NE sortira - to radi source_filter_sort(). */
int dav_parse(const char *xml, size_t len, const char *base_url,
              const char *self_href, lib_entry_t **out, int *n);

#endif /* DAV_PARSE_H */
