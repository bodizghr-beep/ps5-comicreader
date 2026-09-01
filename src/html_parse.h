/* html_parse.h - nginx/Apache autoindex -> lib_entry_t[] */
#ifndef HTML_PARSE_H
#define HTML_PARSE_H

#include "source.h"

/* Isti ugovor kao dav_parse: ne filtrira i ne sortira.
 * Vraca 0 uvijek osim na gresku alokacije. */
int html_parse(const char *html, size_t len, const char *base_url,
               const char *self_href, lib_entry_t **out, int *n);

#endif /* HTML_PARSE_H */
