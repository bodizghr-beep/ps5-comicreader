/* library.h - popis dokumenata na USB uredjajima */
#ifndef LIBRARY_H
#define LIBRARY_H

#define LIB_PATH_MAX  1024
#define LIB_TITLE_MAX 192

typedef struct {
    char path[LIB_PATH_MAX];
    char title[LIB_TITLE_MAX];   /* ime fajla bez ekstenzije, za prikaz */
    int  last_page;              /* iz sacuvanog stanja; -1 ako nije citano */
} lib_item_t;

typedef struct {
    lib_item_t *items;
    int         count;
    char        root[LIB_PATH_MAX];  /* prvi pronadjeni USB, za upis stanja */
} library_t;

/* Skenira /mnt/usb0../mnt/usb7 i puni listu. Vraca broj nadjenih fajlova. */
int  library_scan(library_t *lib);
void library_free(library_t *lib);

/* Trajno stanje: zapamcena stranica po putanji. */
void state_load(library_t *lib);
void state_save(library_t *lib, const char *path, int page);

#endif /* LIBRARY_H */
