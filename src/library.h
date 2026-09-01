/* library.h - navigacijski stek nad izvorima */
#ifndef LIBRARY_H
#define LIBRARY_H

#include "source.h"

#define LIB_DEPTH_MAX 16
#define LIB_SRC_MAX   16

typedef struct {
    source_t    *src;                  /* NULL samo na korijenu */
    char         path[LIB_PATH_MAX];
    char         title[LIB_TITLE_MAX];
    lib_entry_t *entries;
    int          count;
    int          sel, scroll;          /* prezivljavaju izlazak i povratak */
    char         err[128];             /* prazno ako je listanje uspjelo */
} lib_level_t;

typedef struct {
    char path[LIB_PATH_MAX];
    int  page;
} state_rec_t;

typedef struct {
    source_t    *sources[LIB_SRC_MAX];
    int          n_sources;

    lib_level_t  stack[LIB_DEPTH_MAX];
    int          depth;

    char         root[LIB_PATH_MAX];   /* prvi USB, za .ps5cr_state */

    state_rec_t *state;
    int          n_state, cap_state;
} library_t;

void library_reset(library_t *l);
int  library_add_source(library_t *l, source_t *s);

/* USB slotovi + config -> izvori -> korijen. Vraca broj izvora. */
int  library_init(library_t *l);
void library_free(library_t *l);

int  library_enter(library_t *l, int index);   /*  0 ok, -1 odbijeno */
int  library_back(library_t *l);               /*  1 izasao, 0 vec korijen */

lib_level_t *library_cur(library_t *l);
void library_breadcrumb(const library_t *l, char *buf, size_t len);

/* Trajno stanje: zapamcena stranica po putanji (ili URL-u). */
void state_load(library_t *l);
void state_save(library_t *l, const char *path, int page);
int  state_page_for(const library_t *l, const char *path);

#endif /* LIBRARY_H */
