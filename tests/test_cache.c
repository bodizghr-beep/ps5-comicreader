/* test_cache.c - provera niti u kesu i upload-a tekstura, bez pravog ekrana.
 * Simulira listanje napred-nazad i proverava da se sve stranice pojave.
 */
#include <SDL2/SDL.h>
#include "cache.h"
#include "ui.h"
#include "common.h"

static SDL_Texture *wait_for(cache_t *c, int page, int timeout_ms)
{
    uint32_t deadline = SDL_GetTicks() + timeout_ms;
    for (;;) {
        cache_pump(c);
        SDL_Texture *t = cache_texture(c, page, NULL, NULL);
        if (t)
            return t;
        if (cache_failed(c, page))
            return NULL;
        if (SDL_GetTicks() > deadline)
            return NULL;
        SDL_Delay(5);
    }
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "upotreba: %s <fajl>\n", argv[0]);
        return 1;
    }

    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        ERR("SDL_Init: %s", SDL_GetError());
        return 1;
    }
    SDL_Window   *w = SDL_CreateWindow("t", 0, 0, 640, 480, SDL_WINDOW_HIDDEN);
    SDL_Renderer *r = SDL_CreateRenderer(w, -1, SDL_RENDERER_SOFTWARE);
    if (!r) {
        ERR("renderer: %s", SDL_GetError());
        return 1;
    }

    ui_t ui;
    if (ui_init(&ui, r, 640, 480) != 0)
        return 1;

    cache_t *c = cache_open(argv[1], r);
    if (!c) {
        printf("FAIL: cache_open\n");
        return 1;
    }

    int n = cache_page_count(c);
    printf("stranica: %d\n", n);

    int fails = 0;

    /* Napred kroz ceo dokument. */
    for (int i = 0; i < n; i++) {
        cache_focus(c, i);
        if (!wait_for(c, i, 5000)) {
            printf("FAIL: stranica %d nije stigla\n", i);
            fails++;
        }
    }
    printf("prolaz unapred: %s\n", fails ? "GRESKE" : "ok");

    /* Nazad - tera reopen arhive. */
    int back_fails = 0;
    for (int i = n - 1; i >= 0; i--) {
        cache_focus(c, i);
        if (!wait_for(c, i, 5000)) {
            printf("FAIL: povratak na %d\n", i);
            back_fails++;
        }
    }
    printf("prolaz unazad: %s\n", back_fails ? "GRESKE" : "ok");
    fails += back_fails;

    /* Nasumicni skokovi. */
    unsigned seed = 12345;
    for (int k = 0; k < 30; k++) {
        seed = seed * 1103515245u + 12345u;
        int i = (int)((seed >> 16) % (unsigned)n);
        cache_focus(c, i);
        if (!wait_for(c, i, 5000)) {
            printf("FAIL: skok na %d\n", i);
            fails++;
        }
    }
    printf("nasumicni skokovi: %s\n", fails ? "GRESKE" : "ok");

    /* Jedan pravi frejm crtanja, da se proveri i UI put. */
    SDL_SetRenderDrawColor(r, 0, 0, 0, 255);
    SDL_RenderClear(r);
    SDL_Texture *t = cache_texture(c, 0, NULL, NULL);
    if (t) {
        SDL_Rect dst = { 0, 0, 320, 480 };
        SDL_RenderCopy(r, t, NULL, &dst);
    }
    ui_text(&ui, 10, 10, 2, (SDL_Color){ 255, 255, 255, 255 }, "1 / %d", n);
    SDL_RenderPresent(r);
    printf("crtanje frejma: ok\n");

    cache_close(c);
    ui_shutdown(&ui);
    SDL_DestroyRenderer(r);
    SDL_DestroyWindow(w);
    SDL_Quit();

    printf("%s\n", fails ? "== GRESKE ==" : "== sve ok ==");
    return fails ? 1 : 0;
}
