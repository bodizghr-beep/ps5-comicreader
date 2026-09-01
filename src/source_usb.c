/* source_usb.c - lokalni USB izvor
 *
 * Za razliku od starog library_scan(), ovdje NEMA rekurzije: lista se
 * tacno jedan nivo, jer stablo gradi navigacijski stek u library.c.
 */
#include "source.h"
#include "common.h"

#include <dirent.h>
#include <sys/stat.h>

static int usb_list(source_t *s, const char *path, lib_entry_t **out, int *n)
{
    DIR *dp = opendir(path);
    if (!dp) {
        snprintf(s->err, sizeof s->err, "ne mogu da otvorim %s", path);
        return -1;
    }

    int          cap = 32, cnt = 0;
    lib_entry_t *arr = malloc((size_t)cap * sizeof *arr);
    if (!arr) {
        closedir(dp);
        return -1;
    }

    struct dirent *de;
    char           child[LIB_PATH_MAX];

    while ((de = readdir(dp)) != NULL) {
        if (!strcmp(de->d_name, ".") || !strcmp(de->d_name, ".."))
            continue;

        int len = snprintf(child, sizeof child, "%s/%s", path, de->d_name);
        if (len < 0 || len >= (int)sizeof child)
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

        if (!is_dir && !is_reg)
            continue;

        if (cnt == cap) {
            int          ncap = cap * 2;
            lib_entry_t *na   = realloc(arr, (size_t)ncap * sizeof *arr);
            if (!na)
                break;
            arr = na;
            cap = ncap;
        }

        lib_entry_t *e = &arr[cnt++];
        memset(e, 0, sizeof *e);
        snprintf(e->path, sizeof e->path, "%s", child);
        e->is_dir    = is_dir;
        e->last_page = -1;

        /* Ime je samo za prikaz, pa je odsijecanje predugackog namjerno. */
        snprintf(e->name, sizeof e->name, "%.*s",
                 (int)(sizeof e->name - 1), de->d_name);
        if (!is_dir) {
            char *dot = strrchr(e->name, '.');
            if (dot)
                *dot = '\0';
        }
    }

    closedir(dp);

    /* Skriveni, sistemski i nepodrzani otpadaju ovdje, jednom za sve izvore. */
    source_filter_sort(arr, &cnt);

    *out = arr;
    *n   = cnt;
    return 0;
}

/* USB fajl je vec na disku - nema sta da se preuzima. */
static int usb_fetch(source_t *s, const char *path, char *local, size_t len,
                     src_progress_fn cb, void *ud)
{
    (void)s; (void)cb; (void)ud;
    snprintf(local, len, "%s", path);
    return 0;
}

static void usb_close(source_t *s) { (void)s; }

static const source_backend_t usb_be = {
    "usb", usb_list, usb_fetch, usb_close
};

source_t *source_usb_new(const char *root)
{
    source_t *s = calloc(1, sizeof *s);
    if (!s)
        return NULL;

    s->be = &usb_be;
    snprintf(s->root, sizeof s->root, "%s", root);
    snprintf(s->name, sizeof s->name, "USB %s", path_base(root));
    return s;
}
