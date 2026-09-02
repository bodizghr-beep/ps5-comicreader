/* cache.c */
#include "cache.h"
#include "doc.h"
#include "common.h"

#define N_SLOTS      6
#define PREFETCH_FWD 2   /* koliko stranica unapred */
#define PREFETCH_BWD 1   /* i unazad */

enum {
    SLOT_EMPTY = 0,
    SLOT_LOADING,     /* radna nit dekodira */
    SLOT_PIXELS,      /* RGBA spreman, ceka upload */
    SLOT_TEXTURE,     /* gotovo, na GPU */
    SLOT_FAILED
};

typedef struct {
    int          index;
    int          state;
    doc_page_t   page;   /* validno u SLOT_PIXELS */
    SDL_Texture *tex;    /* validno u SLOT_TEXTURE */
    int          w, h;
} slot_t;

struct cache {
    const doc_backend_t *be;
    doc_t               *doc;
    SDL_Renderer        *renderer;
    int                  n_pages;

    slot_t slots[N_SLOTS];

    SDL_mutex  *lock;
    SDL_cond   *wake;
    SDL_Thread *worker;
    int         focus;
    int         quit;
};

/* ------------------------------------------------------------------ */

/* Da li je `index` u prozoru koji zelimo da drzimo u kesu. */
static int wanted(cache_t *c, int index)
{
    return index >= c->focus - PREFETCH_BWD &&
           index <= c->focus + PREFETCH_FWD &&
           index >= 0 && index < c->n_pages;
}

/* Pod lockom: vraca slot za dati indeks ili NULL. */
static slot_t *find_slot(cache_t *c, int index)
{
    for (int i = 0; i < N_SLOTS; i++)
        if (c->slots[i].state != SLOT_EMPTY && c->slots[i].index == index)
            return &c->slots[i];
    return NULL;
}

/* Pod lockom: bira sledeci indeks koji treba dekodirati.
 * Prioritet: trenutna stranica, pa unapred, pa unazad. */
static int next_to_load(cache_t *c)
{
    int order[1 + PREFETCH_FWD + PREFETCH_BWD];
    int n = 0;

    order[n++] = c->focus;
    for (int k = 1; k <= PREFETCH_FWD; k++)
        order[n++] = c->focus + k;
    for (int k = 1; k <= PREFETCH_BWD; k++)
        order[n++] = c->focus - k;

    for (int i = 0; i < n; i++) {
        int idx = order[i];
        if (idx < 0 || idx >= c->n_pages)
            continue;
        if (find_slot(c, idx))
            continue;
        return idx;
    }
    return -1;
}

/* ------------------------------------------------------------------ */

static int worker_fn(void *arg)
{
    cache_t *c = arg;

    SDL_LockMutex(c->lock);
    for (;;) {
        if (c->quit)
            break;

        int idx = next_to_load(c);
        if (idx < 0) {
            SDL_CondWait(c->wake, c->lock);
            continue;
        }

        /* Nadji prazan slot. Ako ga nema, glavna nit jos nije stigla da
         * oslobodi stare - sacekaj sledecu priliku. */
        slot_t *s = NULL;
        for (int i = 0; i < N_SLOTS; i++) {
            if (c->slots[i].state == SLOT_EMPTY) {
                s = &c->slots[i];
                break;
            }
        }
        if (!s) {
            SDL_CondWait(c->wake, c->lock);
            continue;
        }

        s->index = idx;
        s->state = SLOT_LOADING;
        SDL_UnlockMutex(c->lock);

        doc_page_t page = { 0, 0, NULL };
        int        rc   = c->be->render(c->doc, idx, &page);

        SDL_LockMutex(c->lock);
        /* Slot je i dalje nas - niko ga ne dira dok je u LOADING stanju. */
        if (rc == 0) {
            s->page  = page;
            s->w     = page.width;
            s->h     = page.height;
            s->state = SLOT_PIXELS;
        } else {
            s->state = SLOT_FAILED;
        }
    }
    SDL_UnlockMutex(c->lock);
    return 0;
}

/* ------------------------------------------------------------------ */

cache_t *cache_open(const char *path, SDL_Renderer *renderer,
                    doc_progress_fn cb, void *ud)
{
    const doc_backend_t *be = doc_backend_for(path);
    if (!be) {
        ERR("nepodrzan format: %s", path);
        return NULL;
    }

    cache_t *c = calloc(1, sizeof(*c));
    if (!c)
        return NULL;

    c->be       = be;
    c->renderer = renderer;
    c->doc      = be->open(path, cb, ud);
    if (!c->doc) {
        free(c);
        return NULL;
    }

    c->n_pages = be->page_count(c->doc);
    c->lock    = SDL_CreateMutex();
    c->wake    = SDL_CreateCond();
    c->focus   = 0;

    c->worker = SDL_CreateThread(worker_fn, "page-decoder", c);
    if (!c->worker) {
        ERR("SDL_CreateThread: %s", SDL_GetError());
        be->close(c->doc);
        SDL_DestroyCond(c->wake);
        SDL_DestroyMutex(c->lock);
        free(c);
        return NULL;
    }

    return c;
}

void cache_close(cache_t *c)
{
    if (!c)
        return;

    SDL_LockMutex(c->lock);
    c->quit = 1;
    SDL_CondBroadcast(c->wake);
    SDL_UnlockMutex(c->lock);
    SDL_WaitThread(c->worker, NULL);

    for (int i = 0; i < N_SLOTS; i++) {
        if (c->slots[i].state == SLOT_PIXELS)
            doc_page_free(&c->slots[i].page);
        if (c->slots[i].tex)
            SDL_DestroyTexture(c->slots[i].tex);
    }

    c->be->close(c->doc);
    SDL_DestroyCond(c->wake);
    SDL_DestroyMutex(c->lock);
    free(c);
}

int cache_page_count(cache_t *c)
{
    return c ? c->n_pages : 0;
}

void cache_focus(cache_t *c, int index)
{
    if (!c)
        return;
    SDL_LockMutex(c->lock);
    if (c->focus != index) {
        c->focus = index;
        SDL_CondSignal(c->wake);
    }
    SDL_UnlockMutex(c->lock);
}

void cache_pump(cache_t *c)
{
    if (!c)
        return;

    SDL_LockMutex(c->lock);

    int freed = 0;

    for (int i = 0; i < N_SLOTS; i++) {
        slot_t *s = &c->slots[i];

        /* Izbacivanje: samo glavna nit dira teksture, pa nema trke. */
        if ((s->state == SLOT_PIXELS || s->state == SLOT_TEXTURE ||
             s->state == SLOT_FAILED) && !wanted(c, s->index)) {
            if (s->state == SLOT_PIXELS)
                doc_page_free(&s->page);
            if (s->tex) {
                SDL_DestroyTexture(s->tex);
                s->tex = NULL;
            }
            s->state = SLOT_EMPTY;
            freed    = 1;
            continue;
        }

        if (s->state != SLOT_PIXELS)
            continue;

        /* Upload na GPU. Radi se van locka jer je SDL_CreateTexture spor,
         * ali bafer je nas - radna nit ne dira slotove u PIXELS stanju. */
        doc_page_t page = s->page;
        s->page.pixels  = NULL;
        SDL_UnlockMutex(c->lock);

        SDL_Texture *t = SDL_CreateTexture(c->renderer, SDL_PIXELFORMAT_ABGR8888,
                                           SDL_TEXTUREACCESS_STATIC,
                                           page.width, page.height);
        if (t) {
            SDL_UpdateTexture(t, NULL, page.pixels, page.width * 4);
            SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND);
        } else {
            ERR("SDL_CreateTexture: %s", SDL_GetError());
        }
        doc_page_free(&page);

        SDL_LockMutex(c->lock);
        s->tex   = t;
        s->state = t ? SLOT_TEXTURE : SLOT_FAILED;
        freed    = 1;
    }

    if (freed)
        SDL_CondSignal(c->wake);

    SDL_UnlockMutex(c->lock);
}

SDL_Texture *cache_texture(cache_t *c, int index, int *w, int *h)
{
    if (!c)
        return NULL;

    SDL_LockMutex(c->lock);
    slot_t      *s = find_slot(c, index);
    SDL_Texture *t = NULL;

    if (s && s->state == SLOT_TEXTURE) {
        t = s->tex;
        if (w) *w = s->w;
        if (h) *h = s->h;
    }
    SDL_UnlockMutex(c->lock);
    return t;
}

int cache_failed(cache_t *c, int index)
{
    if (!c)
        return 0;
    SDL_LockMutex(c->lock);
    slot_t *s  = find_slot(c, index);
    int     rc = (s && s->state == SLOT_FAILED);
    SDL_UnlockMutex(c->lock);
    return rc;
}
