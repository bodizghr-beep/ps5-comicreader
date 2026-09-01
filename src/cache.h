/* cache.h - kes dekodiranih stranica sa prefetch-om u pozadini
 *
 * Podela posla izmedju niti:
 *   radna nit  - dekodira stranicu iz dokumenta u RGBA bafer
 *   glavna nit - pravi SDL teksturu iz tog bafera i oslobadja stare slotove
 *
 * SDL teksture se smeju dirati samo iz niti koja je napravila renderer, pa
 * radna nit namerno ne zna nista o SDL-u.
 */
#ifndef CACHE_H
#define CACHE_H

#include <SDL2/SDL.h>

typedef struct cache cache_t;

cache_t *cache_open(const char *path, SDL_Renderer *renderer);
void     cache_close(cache_t *c);

int cache_page_count(cache_t *c);

/* Govori kesu koja je stranica trenutno na ekranu; oko nje se radi prefetch. */
void cache_focus(cache_t *c, int index);

/* Glavna nit, jednom po frejmu: upload gotovih bafera i izbacivanje starih. */
void cache_pump(cache_t *c);

/* Tekstura stranice ili NULL ako jos nije spremna. */
SDL_Texture *cache_texture(cache_t *c, int index, int *w, int *h);

/* 1 ako je stranica trajno neuspesna (ostecena slika i sl.). */
int cache_failed(cache_t *c, int index);

#endif /* CACHE_H */
