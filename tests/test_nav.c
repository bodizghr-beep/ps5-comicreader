/* test_nav.c - navigacijski stek nad laznim izvorom, bez USB-a i mreze */
#include "library.h"
#include "common.h"
#include <assert.h>

/* Lazni izvor:
 *   /fake        -> [dir A, dir deepdir, file doc1]
 *   /fake/A      -> [file doc2]
 *   ...deep...   -> uvijek jedan folder, za provjeru granice steka
 */
static int fake_list(source_t *s, const char *path, lib_entry_t **out, int *n)
{
    (void)s;
    lib_entry_t *e;

    if (strstr(path, "deep")) {
        e = calloc(1, sizeof *e);
        snprintf(e->name, sizeof e->name, "deep");
        snprintf(e->path, sizeof e->path, "%s/deep", path);
        e->is_dir = 1;
        e->last_page = -1;
        *out = e; *n = 1;
        return 0;
    }

    if (strstr(path, "/A")) {
        e = calloc(1, sizeof *e);
        snprintf(e->name, sizeof e->name, "doc2");
        snprintf(e->path, sizeof e->path, "%s/doc2.cbz", path);
        e->is_dir = 0;
        e->last_page = -1;
        *out = e; *n = 1;
        return 0;
    }

    e = calloc(3, sizeof *e);
    snprintf(e[0].name, sizeof e[0].name, "A");
    snprintf(e[0].path, sizeof e[0].path, "%s/A", path);
    e[0].is_dir = 1; e[0].last_page = -1;
    snprintf(e[1].name, sizeof e[1].name, "deepdir");
    snprintf(e[1].path, sizeof e[1].path, "%s/deep", path);
    e[1].is_dir = 1; e[1].last_page = -1;
    snprintf(e[2].name, sizeof e[2].name, "doc1");
    snprintf(e[2].path, sizeof e[2].path, "%s/doc1.cbz", path);
    e[2].is_dir = 0; e[2].last_page = -1;
    *out = e; *n = 3;
    return 0;
}

static int fake_fetch(source_t *s, const char *p, char *local, size_t len,
                      src_progress_fn cb, void *ud)
{
    (void)s; (void)cb; (void)ud;
    snprintf(local, len, "%s", p);
    return 0;
}

static void fake_close(source_t *s) { (void)s; }

static const source_backend_t fake_be = { "fake", fake_list, fake_fetch, fake_close };

static source_t *fake_new(const char *name)
{
    source_t *s = calloc(1, sizeof *s);
    s->be = &fake_be;
    snprintf(s->name, sizeof s->name, "%s", name);
    snprintf(s->root, sizeof s->root, "/fake");
    return s;
}

/* Splash trazi da library_init najavi fazu prije nego sto je zapocne. */
static const char *prog_msgs[8];
static int         n_prog;

static void rec_progress(void *ud, const char *msg)
{
    assert(ud == &n_prog);
    if (n_prog < 8)
        prog_msgs[n_prog++] = msg;
}

int main(void)
{
    library_t l;
    library_reset(&l);
    assert(library_add_source(&l, fake_new("izvor1")) == 0);
    assert(library_add_source(&l, fake_new("izvor2")) == 0);

    /* Korijen: po jedan red za svaki izvor, sve folderi. */
    lib_level_t *cur = library_cur(&l);
    assert(l.depth == 0);
    assert(cur->count == 2);
    assert(cur->entries[0].is_dir == 1);
    assert(!strcmp(cur->entries[0].name, "izvor1"));

    char bc[256];
    library_breadcrumb(&l, bc, sizeof bc);
    assert(!strcmp(bc, "/"));

    /* Ulazak u prvi izvor. */
    assert(library_enter(&l, 0) == 0);
    assert(l.depth == 1);
    cur = library_cur(&l);
    assert(cur->count == 3);
    library_breadcrumb(&l, bc, sizeof bc);
    assert(!strcmp(bc, "izvor1/"));

    /* sel se pamti pri izlasku i povratku. */
    cur->sel = 2;
    assert(library_enter(&l, 0) == 0);      /* u folder A */
    assert(l.depth == 2);
    cur = library_cur(&l);
    assert(cur->count == 1);
    assert(!strcmp(cur->entries[0].name, "doc2"));
    library_breadcrumb(&l, bc, sizeof bc);
    assert(!strcmp(bc, "izvor1/A/"));

    assert(library_back(&l) == 1);
    assert(l.depth == 1);
    assert(library_cur(&l)->sel == 2);      /* zapamceno */

    /* Ulazak u fajl se odbija. */
    assert(library_enter(&l, 2) == -1);
    assert(l.depth == 1);

    /* Granica steka: ulazak u "deep" folder u nedogled mora stati. */
    for (int i = 0; i < LIB_DEPTH_MAX + 5; i++)
        library_enter(&l, 1);
    assert(l.depth <= LIB_DEPTH_MAX - 1);

    while (library_back(&l) == 1)
        ;
    assert(l.depth == 0);
    assert(library_back(&l) == 0);          /* korijen: nema kuda dalje */

    library_free(&l);

    /* library_init javlja faze splash ekranu: prvo USB, pa podesavanja. */
    setenv("CR_ROOT", "/nema/ovakvog/foldera", 1);

    library_t l2;
    library_init(&l2, rec_progress, &n_prog);
    assert(n_prog == 2);
    assert(strstr(prog_msgs[0], "USB"));
    assert(strstr(prog_msgs[1], "podesavanja"));
    library_free(&l2);

    /* Bez callbacka mora raditi isto - splash je opcion. */
    library_init(&l2, NULL, NULL);
    library_free(&l2);

    printf("test_nav OK\n");
    return 0;
}
