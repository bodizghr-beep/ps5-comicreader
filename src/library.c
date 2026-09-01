/* library.c */
#include "library.h"
#include "common.h"
#include "doc.h"

#include <dirent.h>
#include <sys/stat.h>

#define MAX_DEPTH  5
#define STATE_FILE ".ps5cr_state"

/* PS5 montira USB uredjaje na /mnt/usbN. Nema garancije da je bas usb0,
 * pa se prolazi kroz sve moguce indekse. */
#define USB_SLOTS 8

static void lib_push(library_t *lib, int *cap, const char *path)
{
    if (lib->count == *cap) {
        int         ncap = *cap ? *cap * 2 : 64;
        lib_item_t *ni   = realloc(lib->items, (size_t)ncap * sizeof(lib_item_t));
        if (!ni)
            return;
        lib->items = ni;
        *cap       = ncap;
    }

    lib_item_t *it = &lib->items[lib->count];
    snprintf(it->path, sizeof(it->path), "%s", path);
    it->last_page = -1;

    /* Naslov = ime fajla bez ekstenzije. */
    snprintf(it->title, sizeof(it->title), "%s", path_base(path));
    char *dot = strrchr(it->title, '.');
    if (dot)
        *dot = '\0';

    lib->count++;
}

static void scan_dir(library_t *lib, int *cap, const char *dir, int depth)
{
    if (depth > MAX_DEPTH)
        return;

    DIR *dp = opendir(dir);
    if (!dp)
        return;

    struct dirent *de;
    char           child[LIB_PATH_MAX];

    while ((de = readdir(dp)) != NULL) {
        if (de->d_name[0] == '.')
            continue;

        int n = snprintf(child, sizeof(child), "%s/%s", dir, de->d_name);
        if (n < 0 || n >= (int)sizeof(child))
            continue;

        /* d_type nije pouzdan na svim FS-ovima, pa se pada na stat(). */
        int is_dir = 0, is_reg = 0;
        if (de->d_type == DT_DIR) {
            is_dir = 1;
        } else if (de->d_type == DT_REG) {
            is_reg = 1;
        } else {
            struct stat st;
            if (stat(child, &st) == 0) {
                is_dir = S_ISDIR(st.st_mode);
                is_reg = S_ISREG(st.st_mode);
            }
        }

        if (is_dir)
            scan_dir(lib, cap, child, depth + 1);
        else if (is_reg && doc_is_supported(child))
            lib_push(lib, cap, child);
    }

    closedir(dp);
}

static int item_sort_cb(const void *a, const void *b)
{
    return natural_cmp(((const lib_item_t *)a)->title, ((const lib_item_t *)b)->title);
}

int library_scan(library_t *lib)
{
    int cap = 0;

    memset(lib, 0, sizeof(*lib));

    /* Na PC build-u nema /mnt/usbN, pa se koren moze zadati promenljivom
     * okruzenja. Na konzoli se ova grana nikad ne aktivira. */
    const char *override = getenv("CR_ROOT");
    if (override && *override) {
        snprintf(lib->root, sizeof(lib->root), "%s", override);
        LOG("skeniram %s (CR_ROOT)", lib->root);
        scan_dir(lib, &cap, lib->root, 0);
        if (lib->count > 1)
            qsort(lib->items, (size_t)lib->count, sizeof(lib_item_t), item_sort_cb);
        LOG("ukupno %d dokumenata", lib->count);
        return lib->count;
    }

    for (int i = 0; i < USB_SLOTS; i++) {
        char        root[LIB_PATH_MAX];
        struct stat st;

        snprintf(root, sizeof(root), "/mnt/usb%d", i);
        if (stat(root, &st) != 0 || !S_ISDIR(st.st_mode))
            continue;

        if (!lib->root[0])
            snprintf(lib->root, sizeof(lib->root), "%s", root);

        LOG("skeniram %s", root);
        scan_dir(lib, &cap, root, 0);
    }

    if (lib->count > 1)
        qsort(lib->items, (size_t)lib->count, sizeof(lib_item_t), item_sort_cb);

    LOG("ukupno %d dokumenata", lib->count);
    return lib->count;
}

void library_free(library_t *lib)
{
    free(lib->items);
    lib->items = NULL;
    lib->count = 0;
}

/* ------------------------------------------------------------------ */
/* Stanje citanja: obican tekst, "putanja<TAB>stranica" po liniji.     */
/* Namerno primitivno - fajl je mali i mora prezivi nasilan prekid.    */

static void state_path(library_t *lib, char *buf, size_t len)
{
    const char *root = lib->root[0] ? lib->root : "/mnt/usb0";
    int n = snprintf(buf, len, "%s/%s", root, STATE_FILE);
    if (n < 0 || (size_t)n >= len)
        snprintf(buf, len, "/mnt/usb0/%s", STATE_FILE);
}

void state_load(library_t *lib)
{
    char sp[LIB_PATH_MAX + 16];
    state_path(lib, sp, sizeof(sp));

    FILE *f = fopen(sp, "r");
    if (!f)
        return;

    char line[LIB_PATH_MAX + 32];
    while (fgets(line, sizeof(line), f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';

        int page = atoi(tab + 1);
        for (int i = 0; i < lib->count; i++) {
            if (!strcmp(lib->items[i].path, line)) {
                lib->items[i].last_page = page;
                break;
            }
        }
    }
    fclose(f);
}

void state_save(library_t *lib, const char *path, int page)
{
    for (int i = 0; i < lib->count; i++) {
        if (!strcmp(lib->items[i].path, path)) {
            lib->items[i].last_page = page;
            break;
        }
    }

    char sp[LIB_PATH_MAX], tmp[LIB_PATH_MAX + 16];
    state_path(lib, sp, sizeof(sp));
    snprintf(tmp, sizeof(tmp), "%s.tmp", sp);

    /* Upis u privremeni fajl pa rename - da gasenje konzole usred upisa
     * ne ostavi polupraznu listu. */
    FILE *f = fopen(tmp, "w");
    if (!f) {
        ERR("ne mogu da upisem stanje u %s", tmp);
        return;
    }
    for (int i = 0; i < lib->count; i++) {
        if (lib->items[i].last_page >= 0)
            fprintf(f, "%s\t%d\n", lib->items[i].path, lib->items[i].last_page);
    }
    fclose(f);
    rename(tmp, sp);
}
