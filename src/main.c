/* main.c - PS5 Comic Reader
 *
 * Payload ulazna tacka. Redosled: SDL -> prozor -> splash + skener USB-a -> petlja.
 */
#include <SDL2/SDL.h>
#include <curl/curl.h>

#include "common.h"
#include "ui.h"
#include "library.h"
#include "cache.h"
#include "splash.h"

#define ROW_H        44
#define LIST_TOP     120
#define PAD          48
#define HUD_MS       2500    /* koliko dugo HUD ostaje vidljiv */
#define STICK_DEAD   9000
#define PAN_SPEED    1400.0f /* piksela u sekundi pri punom otklonu */

typedef enum { SCREEN_BROWSER, SCREEN_READER, SCREEN_FETCH } screen_t;
typedef enum { FIT_SCREEN, FIT_WIDTH, FIT_HEIGHT, FIT_COUNT } fitmode_t;

/* Posao preuzimanja. Zivi u app_t, a nit ga puni preko atomika. */
typedef struct {
    source_t    *src;
    lib_entry_t  entry;        /* kopija - nivo steka moze nestati dok nit radi */
    char         local[LIB_PATH_MAX];
    SDL_Thread  *thread;
    int          want_page;

    /* Napredak u KB, jer SDL_atomic_t nosi int. 782 MB / 1024 = 763k,
     * daleko od granice. */
    SDL_atomic_t got_kb, total_kb;
    SDL_atomic_t done, status, cancel;
} fetch_job_t;

typedef struct {
    ui_t      ui;
    library_t lib;
    screen_t  screen;

    /* browser */
    int rows_visible;

    /* reader */
    cache_t  *cache;
    int       page;
    int       n_pages;
    fitmode_t fit;
    float     zoom;
    float     pan_x, pan_y;
    uint32_t  hud_until;
    char      cur_path[LIB_PATH_MAX];    /* kljuc za state: putanja ili URL */
    char      cur_local[LIB_PATH_MAX];   /* ono sto je dobio cache_open */

    fetch_job_t  fetch;
    source_t    *cur_src;                /* izvor tekuceg dokumenta */
    lib_entry_t  cur_entry;              /* za ponovno otvaranje poslije pada veze */

    int running;
} app_t;

/* ------------------------------------------------------------------ */

static void hud_bump(app_t *a)
{
    a->hud_until = SDL_GetTicks() + HUD_MS;
}

static void reader_close(app_t *a)
{
    /* Nit ne smije ostati da pise u a->fetch.local poslije gasenja. */
    if (a->fetch.thread) {
        SDL_AtomicSet(&a->fetch.cancel, 1);
        SDL_WaitThread(a->fetch.thread, NULL);
        a->fetch.thread = NULL;
    }

    if (a->cache) {
        state_save(&a->lib, a->cur_path, a->page);
        cache_close(a->cache);
        a->cache = NULL;
    }
}

static int fetch_progress(void *ud, int64_t got, int64_t total)
{
    fetch_job_t *j = ud;

    SDL_AtomicSet(&j->got_kb,   (int)(got   / 1024));
    SDL_AtomicSet(&j->total_kb, (int)(total / 1024));

    /* !=0 prekida curl. Krug postavlja cancel iz glavne niti. */
    return SDL_AtomicGet(&j->cancel);
}

static int fetch_thread(void *ud)
{
    fetch_job_t *j = ud;

    int r = j->src->be->fetch(j->src, j->entry.path, j->local, sizeof j->local,
                              fetch_progress, j);

    SDL_AtomicSet(&j->status, r);
    SDL_AtomicSet(&j->done, 1);
    return 0;
}

/* Oba izvora idu istim putem kroz nit. Za USB je fetch identitet, pa se
 * SCREEN_FETCH vidi jedan frejm ili nijedan - jedna grana umjesto dvije. */
static void reader_start(app_t *a, source_t *src, lib_entry_t *it, int want_page)
{
    fetch_job_t *j = &a->fetch;

    reader_close(a);

    memset(j, 0, sizeof *j);
    j->src       = src;
    j->entry     = *it;
    j->want_page = want_page;

    SDL_AtomicSet(&j->done, 0);
    SDL_AtomicSet(&j->cancel, 0);
    SDL_AtomicSet(&j->status, 0);
    SDL_AtomicSet(&j->got_kb, 0);
    SDL_AtomicSet(&j->total_kb, 0);

    j->thread = SDL_CreateThread(fetch_thread, "fetch", j);
    if (!j->thread) {
        ERR("SDL_CreateThread: %s", SDL_GetError());
        return;
    }

    a->screen = SCREEN_FETCH;
}

static void reader_finish(app_t *a)
{
    fetch_job_t *j = &a->fetch;

    SDL_WaitThread(j->thread, NULL);
    j->thread = NULL;

    int status = SDL_AtomicGet(&j->status);
    if (status != 0) {
        if (status == 1)
            LOG("preuzimanje otkazano");
        else
            ERR("ne mogu da pripremim %s: %s", j->entry.path,
                j->src->err[0] ? j->src->err : "nepoznata greska");
        a->screen = SCREEN_BROWSER;
        return;
    }

    a->cache = cache_open(j->local, a->ui.r);
    if (!a->cache) {
        ERR("ne mogu da otvorim %s", j->local);
        a->screen = SCREEN_BROWSER;
        return;
    }

    snprintf(a->cur_path,  sizeof a->cur_path,  "%s", j->entry.path);
    snprintf(a->cur_local, sizeof a->cur_local, "%s", j->local);
    a->cur_src   = j->src;
    a->cur_entry = j->entry;

    a->n_pages = cache_page_count(a->cache);

    int p = (j->want_page >= 0) ? j->want_page : j->entry.last_page;
    a->page   = (p > 0 && p < a->n_pages) ? p : 0;
    a->fit    = FIT_SCREEN;
    a->zoom   = 1.0f;
    a->pan_x  = a->pan_y = 0.0f;
    a->screen = SCREEN_READER;

    cache_focus(a->cache, a->page);
    hud_bump(a);
}

static void reader_goto(app_t *a, int page)
{
    if (page < 0)
        page = 0;
    if (page >= a->n_pages)
        page = a->n_pages - 1;
    if (page == a->page)
        return;

    a->page  = page;
    a->pan_x = a->pan_y = 0.0f;
    cache_focus(a->cache, a->page);
    state_save(&a->lib, a->cur_path, a->page);
    hud_bump(a);
}

/* ------------------------------------------------------------------ */
/* Racuna gde i koliko veliko ide stranica na ekranu.                  */

static SDL_Rect page_rect(app_t *a, int tw, int th)
{
    float sw = (float)a->ui.screen_w;
    float sh = (float)a->ui.screen_h;
    float s;

    switch (a->fit) {
    case FIT_WIDTH:  s = sw / tw; break;
    case FIT_HEIGHT: s = sh / th; break;
    default:         s = MIN(sw / tw, sh / th); break;
    }
    s *= a->zoom;

    int w = (int)(tw * s);
    int h = (int)(th * s);

    /* Pan ima smisla samo po osi na kojoj slika prelazi ekran. */
    float max_x = MAX(0.0f, (w - sw) * 0.5f);
    float max_y = MAX(0.0f, (h - sh) * 0.5f);

    if (a->pan_x >  max_x) a->pan_x =  max_x;
    if (a->pan_x < -max_x) a->pan_x = -max_x;
    if (a->pan_y >  max_y) a->pan_y =  max_y;
    if (a->pan_y < -max_y) a->pan_y = -max_y;

    SDL_Rect rc;
    rc.w = w;
    rc.h = h;
    rc.x = (int)((sw - w) * 0.5f - a->pan_x);
    rc.y = (int)((sh - h) * 0.5f - a->pan_y);
    return rc;
}

/* Softverski renderer skalira svaki piksel koji mu se preda, pa se pri zumu
 * isplati predati mu samo vidljivi dio stranice. Rezultat je identican -
 * ovo je izbjegavanje posla, ne aproksimacija. Pri "uklopi u ekran" je
 * cijela stranica vidljiva pa se nista ne mijenja.
 *
 * Akcelerisani renderer bi ovo radio na GPU besplatno, ali ga na PS5 nema:
 * SDL2 iz pacbrew paketa sadrzi samo SW_RenderDriver, PS5 video draiver je
 * framebuffer bez GL konteksta, a libGL.so je simlink na OSMesu - dakle opet
 * CPU. Vidi docs/superpowers/plans/ za mjerenja.
 */
static int clip_to_screen(const ui_t *ui, SDL_Rect full, int tw, int th,
                          SDL_Rect *src, SDL_Rect *dst)
{
    SDL_Rect screen = { 0, 0, ui->screen_w, ui->screen_h };
    SDL_Rect vis;

    if (!SDL_IntersectRect(&full, &screen, &vis))
        return 0;                       /* nista nije vidljivo */

    /* Nazad iz ekranskih u teksturne koordinate. */
    double sx = (double)tw / (double)full.w;
    double sy = (double)th / (double)full.h;

    src->x = (int)((vis.x - full.x) * sx);
    src->y = (int)((vis.y - full.y) * sy);
    src->w = (int)(vis.w * sx + 0.5);
    src->h = (int)(vis.h * sy + 0.5);

    if (src->x < 0) src->x = 0;
    if (src->y < 0) src->y = 0;
    if (src->x + src->w > tw) src->w = tw - src->x;
    if (src->y + src->h > th) src->h = th - src->y;
    if (src->w <= 0 || src->h <= 0)
        return 0;

    *dst = vis;
    return 1;
}

/* ------------------------------------------------------------------ */

static void draw_browser(app_t *a)
{
    ui_t        *ui = &a->ui;
    lib_level_t *lv = library_cur(&a->lib);
    char         buf[256];

    ui_fill_rect(ui, 0, 0, ui->screen_w, ui->screen_h, COL_BG);
    ui_text(ui, PAD, 44, 4, COL_ACCENT, "%s", APP_NAME);

    /* Breadcrumb se skracuje s LIJEVA - rep putanje je informativniji. */
    char bc[LIB_PATH_MAX];
    library_breadcrumb(&a->lib, bc, sizeof bc);

    int    max_bc = (ui->screen_w - 2 * PAD) / (8 * 2);
    size_t bclen  = strlen(bc);
    if (max_bc > 4 && bclen > (size_t)max_bc)
        ui_text(ui, PAD, 88, 2, COL_DIM, "...%s", bc + bclen - (size_t)max_bc + 3);
    else
        ui_text(ui, PAD, 88, 2, COL_DIM, "%s", bc);

    const char *footer = (a->lib.depth == 0)
        ? "Krst: otvori   Krug: izlaz   D-pad: kretanje"
        : "Krst: otvori   Krug: nazad   D-pad: kretanje";

    if (lv->err[0]) {
        ui_text(ui, PAD, LIST_TOP + 60, 3, COL_TEXT, "greska: %s", lv->err);
        ui_text(ui, PAD, ui->screen_h - 56, 2, COL_DIM, "%s", footer);
        return;
    }

    if (lv->count == 0) {
        ui_text(ui, PAD, LIST_TOP + 60, 3, COL_TEXT,
                a->lib.depth == 0 ? "Nema izvora. Ocekivano: /mnt/usb0/..." : "Prazno");
        ui_text(ui, PAD, LIST_TOP + 110, 2, COL_DIM,
                "Podrzano: cbz cbr cb7 cbt zip rar 7z pdf");
        ui_text(ui, PAD, ui->screen_h - 56, 2, COL_DIM, "%s", footer);
        return;
    }

    /* Skrol drzi selekciju unutar vidljivog opsega. */
    if (lv->sel < lv->scroll)
        lv->scroll = lv->sel;
    if (lv->sel >= lv->scroll + a->rows_visible)
        lv->scroll = lv->sel - a->rows_visible + 1;

    int max_chars = (ui->screen_w - 2 * PAD - 200) / (8 * 2);

    for (int i = 0; i < a->rows_visible; i++) {
        int idx = lv->scroll + i;
        if (idx >= lv->count)
            break;

        int          y  = LIST_TOP + i * ROW_H;
        lib_entry_t *it = &lv->entries[idx];
        int          on = (idx == lv->sel);

        ui_fill_rect(ui, PAD - 16, y - 6, ui->screen_w - 2 * PAD + 32, ROW_H - 4,
                     on ? COL_SEL : COL_PANEL);

        /* Folder se raspoznaje po kosoj crti i boji - font je ASCII, ikone otpadaju. */
        char label[LIB_TITLE_MAX + 2];
        snprintf(label, sizeof label, "%s%s", it->name, it->is_dir ? "/" : "");
        ui_ellipsize(buf, sizeof buf, label, max_chars);

        SDL_Color col = on ? COL_TEXT : (it->is_dir ? COL_ACCENT : COL_DIM);
        ui_text(ui, PAD, y, 2, col, "%s", buf);

        if (!it->is_dir && it->last_page > 0) {
            const char *badge = "nastavi";
            ui_text(ui, ui->screen_w - PAD - ui_text_width(2, badge), y, 2,
                    COL_ACCENT, "%s", badge);
        }
    }

    ui_text(ui, PAD, ui->screen_h - 56, 2, COL_DIM, "%s", footer);
}

static void draw_fetch(app_t *a)
{
    ui_t        *ui = &a->ui;
    fetch_job_t *j  = &a->fetch;

    int got   = SDL_AtomicGet(&j->got_kb);
    int total = SDL_AtomicGet(&j->total_kb);

    ui_fill_rect(ui, 0, 0, ui->screen_w, ui->screen_h, COL_BG);

    char title[LIB_TITLE_MAX + 8];
    ui_ellipsize(title, sizeof title, j->entry.name,
                 (ui->screen_w - 2 * PAD) / (8 * 3));
    ui_text(ui, PAD, ui->screen_h / 2 - 120, 3, COL_TEXT, "%s", title);

    int bar_w = ui->screen_w - 2 * PAD;
    int bar_y = ui->screen_h / 2 - 40;

    ui_fill_rect(ui, PAD, bar_y, bar_w, 28, COL_PANEL);
    if (total > 0) {
        int w = (int)((int64_t)bar_w * got / total);
        ui_fill_rect(ui, PAD, bar_y, w, 28, COL_SEL);
    }

    if (total > 0)
        ui_text(ui, PAD, bar_y + 48, 2, COL_DIM, "%d / %d MB   %d%%",
                got / 1024, total / 1024, (int)((int64_t)100 * got / total));
    else
        ui_text(ui, PAD, bar_y + 48, 2, COL_DIM, "%d MB", got / 1024);

    ui_text(ui, PAD, ui->screen_h - 56, 2, COL_DIM, "Krug: otkazi");
}

static void draw_reader(app_t *a)
{
    ui_t *ui = &a->ui;
    int   tw = 0, th = 0;

    ui_fill_rect(ui, 0, 0, ui->screen_w, ui->screen_h, COL_BG);

    SDL_Texture *t = cache_texture(a->cache, a->page, &tw, &th);

    if (t) {
        SDL_Rect full = page_rect(a, tw, th);
        SDL_Rect src, dst;

        if (clip_to_screen(ui, full, tw, th, &src, &dst))
            SDL_RenderCopy(ui->r, t, &src, &dst);
    } else if (cache_failed(a->cache, a->page)) {
        /* Kod mreznog izvora je pala veza daleko vjerovatnija od ostecene
         * slike, a za razliku od nje je i rjesiva. */
        if (a->cur_src && !strcmp(a->cur_src->be->kind, "http")) {
            ui_text(ui, PAD, ui->screen_h / 2 - 30, 3, COL_TEXT,
                    "Veza prekinuta");
            ui_text(ui, PAD, ui->screen_h / 2 + 20, 2, COL_DIM,
                    "Krst: pokusaj ponovo   Krug: nazad na listu");
        } else {
            ui_text(ui, PAD, ui->screen_h / 2, 3, COL_DIM,
                    "Stranica %d se ne moze prikazati", a->page + 1);
        }
    } else {
        /* Namerno bez animacije - prefetch obicno stigne pre nego sto se
         * ovo uopste primeti. */
        ui_text(ui, PAD, ui->screen_h / 2, 3, COL_DIM, "Ucitavanje...");
    }

    if (SDL_GetTicks() < a->hud_until) {
        static const char *fitname[FIT_COUNT] = { "ekran", "sirina", "visina" };
        int                bar_h = 64;
        int                y     = ui->screen_h - bar_h;

        ui_fill_rect(ui, 0, y, ui->screen_w, bar_h, COL_HUD);
        ui_text(ui, PAD, y + 22, 2, COL_TEXT, "%d / %d", a->page + 1, a->n_pages);

        char info[128];
        snprintf(info, sizeof(info), "%s  x%.1f", fitname[a->fit], a->zoom);
        ui_text(ui, ui->screen_w - PAD - ui_text_width(2, info), y + 22, 2,
                COL_DIM, "%s", info);
    }
}

/* ------------------------------------------------------------------ */

static void on_button(app_t *a, SDL_GameControllerButton b)
{
    if (a->screen == SCREEN_BROWSER) {
        lib_level_t *lv = library_cur(&a->lib);

        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (lv->sel > 0) lv->sel--;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (lv->sel < lv->count - 1) lv->sel++;
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            lv->sel = MAX(0, lv->sel - a->rows_visible);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            lv->sel = MIN(lv->count - 1, lv->sel + a->rows_visible);
            break;
        case SDL_CONTROLLER_BUTTON_A:
            if (lv->count == 0)
                break;
            if (lv->entries[lv->sel].is_dir)
                library_enter(&a->lib, lv->sel);
            else
                reader_start(a, lv->src, &lv->entries[lv->sel], -1);
            break;
        case SDL_CONTROLLER_BUTTON_B:
            /* Krug izlazi iz nivoa; u korijenu gasi aplikaciju. */
            if (!library_back(&a->lib))
                a->running = 0;
            break;
        default:
            break;
        }
        return;
    }

    if (a->screen == SCREEN_FETCH) {
        if (b == SDL_CONTROLLER_BUTTON_B)
            SDL_AtomicSet(&a->fetch.cancel, 1);
        return;
    }

    /* SCREEN_READER */
    switch (b) {
    case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
    case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
        reader_goto(a, a->page + 1);
        break;
    case SDL_CONTROLLER_BUTTON_A:
        if (cache_failed(a->cache, a->page) && a->cur_src &&
            !strcmp(a->cur_src->be->kind, "http")) {
            /* Tekuca stranica, ne zapamcena - korisnik ne smije nazad na
             * pocetak stripa od 500 stranica zbog jedne smetnje. */
            reader_start(a, a->cur_src, &a->cur_entry, a->page);
        } else {
            reader_goto(a, a->page + 1);
        }
        break;
    case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
    case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
        reader_goto(a, a->page - 1);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_UP:
        reader_goto(a, a->page - 10);
        break;
    case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
        reader_goto(a, a->page + 10);
        break;
    case SDL_CONTROLLER_BUTTON_Y:
        a->fit   = (a->fit + 1) % FIT_COUNT;
        a->zoom  = 1.0f;
        a->pan_x = a->pan_y = 0.0f;
        hud_bump(a);
        break;
    case SDL_CONTROLLER_BUTTON_X:
        hud_bump(a);
        break;
    case SDL_CONTROLLER_BUTTON_B:
        reader_close(a);
        a->screen = SCREEN_BROWSER;
        break;
    case SDL_CONTROLLER_BUTTON_START:
        a->running = 0;
        break;
    default:
        break;
    }
}

/* Analogni unos se cita svaki frejm, ne kroz dogadjaje. */
static void poll_analog(app_t *a, SDL_GameController *gc, float dt)
{
    if (!gc || a->screen != SCREEN_READER)
        return;

    int lx = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTX);
    int ly = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_LEFTY);

    if (abs(lx) > STICK_DEAD)
        a->pan_x += (lx / 32767.0f) * PAN_SPEED * dt;
    if (abs(ly) > STICK_DEAD)
        a->pan_y += (ly / 32767.0f) * PAN_SPEED * dt;

    /* Okidaci zumiraju: L2 odzumira, R2 zumira. */
    int lt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERLEFT);
    int rt = SDL_GameControllerGetAxis(gc, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);

    float dz = ((rt - lt) / 32767.0f) * 1.2f * dt;
    if (dz != 0.0f) {
        a->zoom += dz;
        if (a->zoom < 1.0f)  a->zoom = 1.0f;
        if (a->zoom > 5.0f)  a->zoom = 5.0f;
        hud_bump(a);
    }
}

/* ------------------------------------------------------------------ */

/* library_init javlja fazu prije nego sto je zapocne; svaka poruka je jedan
 * frejm splash ekrana. Faze su sinhrone, pa se crta odavde a ne iz petlje. */
static void splash_progress(void *ud, const char *msg)
{
    splash_step((splash_t *)ud, msg);
}

int main(int argc, char **argv)
{
    (void)argc;
    (void)argv;

    app_t a;
    memset(&a, 0, sizeof(a));
    a.running = 1;
    a.screen  = SCREEN_BROWSER;

    LOG("SDL_Init...");
    /* Pokusaj prvo samo VIDEO, bez GAMECONTROLLER - na nekim PS5 SDL build-ovima
     * GAMECONTROLLER init pada ako ScePad nije spreman. Dodajemo ga posle. */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        ERR("SDL_Init VIDEO: %s", SDL_GetError());
        return 1;
    }
    LOG("SDL_Init VIDEO ok");

    /* Prije bilo koje niti - curl_global_init nije thread-safe. */
    curl_global_init(CURL_GLOBAL_DEFAULT);

    /* Gamepad inicijalizuj odvojeno - pad se i dalje cita ako ovo propadne. */
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0)
        ERR("SDL_Init GAMECONTROLLER: %s (nastavlja bez kontrolera)", SDL_GetError());
    else
        LOG("SDL_Init GAMECONTROLLER ok");

    LOG("SDL_CreateWindow...");
    SDL_Window *win = SDL_CreateWindow(APP_NAME,
                                       SDL_WINDOWPOS_UNDEFINED, SDL_WINDOWPOS_UNDEFINED,
                                       1920, 1080,
                                       SDL_WINDOW_FULLSCREEN_DESKTOP);
    if (!win) {
        ERR("SDL_CreateWindow: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }
    LOG("SDL_CreateWindow ok");

    LOG("SDL_CreateRenderer...");
    SDL_Renderer *ren = SDL_CreateRenderer(win, -1,
                                           SDL_RENDERER_ACCELERATED | SDL_RENDERER_PRESENTVSYNC);
    if (!ren) {
        /* Ocekivano na PS5, nije greska: SDL2 iz pacbrew paketa sadrzi samo
         * SW_RenderDriver, PS5 video draiver je framebuffer bez GL konteksta,
         * a libGL.so je simlink na OSMesu - dakle i "GL" bi bio CPU.
         * Zato se zum oslanja na src pravougaonik u draw_reader. */
        LOG("nema akcelerisanog renderera (%s), koristim software",
            SDL_GetError());
        ren = SDL_CreateRenderer(win, -1, SDL_RENDERER_SOFTWARE);
    }
    if (!ren) {
        ERR("SDL_CreateRenderer: %s", SDL_GetError());
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    LOG("SDL_CreateRenderer ok");

    int sw = 1920, sh = 1080;
    SDL_GetRendererOutputSize(ren, &sw, &sh);
    LOG("izlaz %dx%d", sw, sh);

    if (ui_init(&a.ui, ren, sw, sh) != 0) {
        SDL_DestroyRenderer(ren);
        SDL_DestroyWindow(win);
        SDL_Quit();
        return 1;
    }
    a.rows_visible = (sh - LIST_TOP - 100) / ROW_H;

    /* Od ovog trenutka ekran nije crn. Sve prije njega - SDL_Init, prozor,
     * renderer - se ne moze pokriti, jer nema cime da se crta. */
    splash_t sp;
    splash_init(&sp, &a.ui);

    library_init(&a.lib, splash_progress, &sp);

    splash_step(&sp, "Ucitavam zapamcene stranice...");
    state_load(&a.lib);

    splash_hold(&sp);
    splash_free(&sp);

    SDL_GameController *gc = NULL;
    for (int i = 0; i < SDL_NumJoysticks(); i++) {
        if (SDL_IsGameController(i)) {
            gc = SDL_GameControllerOpen(i);
            if (gc) {
                LOG("kontroler: %s", SDL_GameControllerName(gc));
                break;
            }
        }
    }
    if (!gc)
        ERR("kontroler nije nadjen - unos nece raditi");

    uint32_t prev = SDL_GetTicks();

    while (a.running) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
            case SDL_QUIT:
                a.running = 0;
                break;
            case SDL_CONTROLLERBUTTONDOWN:
                on_button(&a, (SDL_GameControllerButton)ev.cbutton.button);
                break;
            case SDL_CONTROLLERDEVICEADDED:
                if (!gc)
                    gc = SDL_GameControllerOpen(ev.cdevice.which);
                break;
            case SDL_KEYDOWN:
                /* Korisno pri testiranju na PC build-u. */
                if (ev.key.keysym.sym == SDLK_ESCAPE)
                    a.running = 0;
                break;
            default:
                break;
            }
        }

        uint32_t now = SDL_GetTicks();
        float    dt  = (now - prev) / 1000.0f;
        prev = now;
        if (dt > 0.1f)
            dt = 0.1f;

        poll_analog(&a, gc, dt);

        if (a.cache)
            cache_pump(a.cache);

        if (a.screen == SCREEN_FETCH && SDL_AtomicGet(&a.fetch.done))
            reader_finish(&a);

        if (a.screen == SCREEN_BROWSER)
            draw_browser(&a);
        else if (a.screen == SCREEN_FETCH)
            draw_fetch(&a);
        else
            draw_reader(&a);

        SDL_RenderPresent(ren);
    }

    reader_close(&a);
    library_free(&a.lib);
    ui_shutdown(&a.ui);
    if (gc)
        SDL_GameControllerClose(gc);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    curl_global_cleanup();

    LOG("kraj");
    return 0;
}
