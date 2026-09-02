/* splash.h - ekran koji pokriva start dok teku skeniranje i citanje configa.
 *
 * Sam se crta i prezentuje, pa radi prije glavne petlje - u toj fazi jos
 * nema ni petlje ni obrade dogadjaja.
 */
#ifndef SPLASH_H
#define SPLASH_H

#include "ui.h"

typedef struct {
    ui_t        *ui;
    SDL_Texture *bg;          /* splash.png; NULL -> crta se ugradjeni */
    int          bg_w, bg_h;
    uint32_t     t0;          /* pocetak, za minimalno trajanje */
} splash_t;

/* Trazi splash.png; bez njega splash i dalje radi, samo nacrtan. */
void splash_init(splash_t *s, ui_t *ui);

/* Jedan frejm sa datom porukom o tome sta se trenutno radi. */
void splash_step(splash_t *s, const char *msg);

/* Ceka da se navrsi minimalno trajanje splash-a, ako je start bio brzi.
 * Bez ovoga se na brzom startu splash vidi kao treptaj ili nikako. */
void splash_hold(splash_t *s);

void splash_free(splash_t *s);

#endif /* SPLASH_H */
