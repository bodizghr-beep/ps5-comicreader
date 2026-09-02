/* splash.c */
#include "splash.h"
#include "common.h"

#include "stb_image.h"

/* Korisnikova slika stoji uz eboot.elf. CR_ROOT je isti izlaz kao u
 * config.c i library.c: host build nema /data/homebrew. */
#define SPLASH_DIR  "/data/homebrew/ComicReader"
#define SPLASH_FILE "splash.png"

/* Skeniranje USB-a i citanje configa umiju da prodju za koji stotinak ms,
 * a splash tada nema svrhu - ne stigne da se procita. */
#define SPLASH_MIN_MS 3500

#define SPLASH_PAD  48
#define TITLE_SCALE 5
#define GLYPH_H     8

static void splash_path(char *buf, size_t len)
{
    const char *root = getenv("CR_ROOT");

    if (root && *root)
        snprintf(buf, len, "%s/%s", root, SPLASH_FILE);
    else
        snprintf(buf, len, "%s/%s", SPLASH_DIR, SPLASH_FILE);
}

/* stb je preveden sa STBI_NO_STDIO (vidi stb_impl.c), pa se fajl cita ovdje. */
static uint8_t *read_file(const char *path, size_t *out_len)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;

    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return NULL;
    }

    long n = ftell(f);
    if (n <= 0) {
        fclose(f);
        return NULL;
    }
    rewind(f);

    uint8_t *buf = malloc((size_t)n);
    if (!buf) {
        fclose(f);
        return NULL;
    }

    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);

    if (got != (size_t)n) {
        free(buf);
        return NULL;
    }

    *out_len = got;
    return buf;
}

void splash_init(splash_t *s, ui_t *ui)
{
    memset(s, 0, sizeof *s);
    s->ui = ui;
    s->t0 = SDL_GetTicks();

    char path[512];
    splash_path(path, sizeof path);

    size_t   len = 0;
    uint8_t *raw = read_file(path, &len);
    if (!raw) {
        LOG("splash: nema %s, crtam ugradjeni", path);
        return;
    }

    int      w = 0, h = 0, ch = 0;
    uint8_t *px = stbi_load_from_memory(raw, (int)len, &w, &h, &ch, 4);
    free(raw);

    if (!px) {
        ERR("splash: %s se ne moze dekodirati (%s), crtam ugradjeni",
            path, stbi_failure_reason());
        return;
    }

    SDL_Texture *t = SDL_CreateTexture(ui->r, SDL_PIXELFORMAT_ABGR8888,
                                       SDL_TEXTUREACCESS_STATIC, w, h);
    if (!t) {
        ERR("splash: SDL_CreateTexture: %s", SDL_GetError());
        stbi_image_free(px);
        return;
    }

    SDL_UpdateTexture(t, NULL, px, w * 4);
    stbi_image_free(px);

    s->bg   = t;
    s->bg_w = w;
    s->bg_h = h;
    LOG("splash: %s (%dx%d)", path, w, h);
}

void splash_step(splash_t *s, const char *msg)
{
    ui_t *ui = s->ui;

    ui_fill_rect(ui, 0, 0, ui->screen_w, ui->screen_h, COL_BG);

    if (s->bg) {
        /* Uklapanje u ekran uz cuvanje odnosa stranica; okvir ostaje COL_BG.
         * Slika zamjenjuje ime i verziju - poruka o fazi ide preko nje. */
        float sc = MIN((float)ui->screen_w / s->bg_w,
                       (float)ui->screen_h / s->bg_h);

        SDL_Rect dst;
        dst.w = (int)(s->bg_w * sc);
        dst.h = (int)(s->bg_h * sc);
        dst.x = (ui->screen_w - dst.w) / 2;
        dst.y = (ui->screen_h - dst.h) / 2;

        SDL_RenderCopy(ui->r, s->bg, NULL, &dst);
    } else {
        int y = ui->screen_h / 2 - 60;

        ui_text(ui, (ui->screen_w - ui_text_width(TITLE_SCALE, APP_NAME)) / 2,
                y, TITLE_SCALE, COL_ACCENT, "%s", APP_NAME);

        char ver[32];
        snprintf(ver, sizeof ver, "verzija %s", APP_VERSION);
        ui_text(ui, (ui->screen_w - ui_text_width(2, ver)) / 2,
                y + TITLE_SCALE * GLYPH_H + 24, 2, COL_DIM, "%s", ver);
    }

    if (msg && *msg) {
        /* Preko korisnikove slike poruka bi se izgubila na svijetloj podlozi,
         * pa ide na istu poluprovidnu traku kakvu citac koristi za HUD. */
        if (s->bg)
            ui_fill_rect(ui, 0, ui->screen_h - 64, ui->screen_w, 64, COL_HUD);

        ui_text(ui, SPLASH_PAD, ui->screen_h - 56, 2,
                s->bg ? COL_TEXT : COL_DIM, "%s", msg);
    }

    SDL_RenderPresent(ui->r);
}

void splash_hold(splash_t *s)
{
    uint32_t elapsed = SDL_GetTicks() - s->t0;

    if (elapsed >= SPLASH_MIN_MS)
        return;

    LOG("splash: start je trajao %u ms, drzim jos %u", elapsed,
        SPLASH_MIN_MS - elapsed);

    /* Ekran vec drzi posljednji frejm, pa se ovdje samo ceka. Dogadjaji se
     * pumpaju da sistem ne vidi aplikaciju kao zaglavljenu.
     * Oduzimanje bez znaka prezivljava prelazak SDL_GetTicks preko granice. */
    while (SDL_GetTicks() - s->t0 < SPLASH_MIN_MS) {
        SDL_PumpEvents();
        SDL_Delay(16);
    }
}

void splash_free(splash_t *s)
{
    if (s->bg)
        SDL_DestroyTexture(s->bg);
    s->bg = NULL;
}
