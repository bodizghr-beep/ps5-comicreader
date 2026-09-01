/* doc_archive.c - CBZ / CBR / CB7 / CBT / ZIP / RAR
 *
 * libarchive daje jedinstven API za sve ove formate, pa je "CBR podrska"
 * zapravo besplatna nuspojava - rar4 i rar5 idu kroz isti kod kao zip.
 *
 * Kljucni problem: libarchive je *streaming* biblioteka i ne ume nasumicno
 * da skoci na N-ti unos. Resenje je citac koji pamti gde je stao:
 *   - skok unapred  -> preskace zaglavlja do cilja (jeftino)
 *   - skok unazad   -> zatvara i ponovo otvara arhivu, pa unapred
 * Posto se strip cita sekvencijalno i kes radi prefetch unapred, ponovno
 * otvaranje se u praksi desava samo kad korisnik skoci nazad.
 */
#include "doc.h"
#include "common.h"

#include <archive.h>
#include <archive_entry.h>

#include "stb_image.h"

#ifdef HAVE_WEBP
#include <webp/decode.h>
#endif

#define BLOCK_SIZE (256 * 1024)
#define MAX_PATH_LEN 1024

typedef struct {
    char *name;
    int   ord;      /* redni broj zaglavlja u arhivi, za sinhronizaciju */
} entry_t;

struct doc {
    char     path[MAX_PATH_LEN];
    entry_t *entries;
    int      n_entries;

    struct archive *ar;     /* NULL kad je stream zatvoren */
    int             pos;    /* redni broj sledeceg zaglavlja koje ce se procitati */
};

/* ------------------------------------------------------------------ */

static int is_archive_ext(const char *e)
{
    static const char *ok[] = { "cbz", "cbr", "cb7", "cbt", "zip", "rar", "7z", "tar", NULL };
    for (int i = 0; ok[i]; i++)
        if (!strcasecmp(e, ok[i]))
            return 1;
    return 0;
}

static int is_image_ext(const char *e)
{
    static const char *ok[] = { "jpg", "jpeg", "png", "bmp", "gif", "tga",
#ifdef HAVE_WEBP
                                "webp",
#endif
                                NULL };
    for (int i = 0; ok[i]; i++)
        if (!strcasecmp(e, ok[i]))
            return 1;
    return 0;
}

/* Dekodira sirove bajtove slike u RGBA8888.
 * WebP ide kroz libwebp jer ga stb_image ne podrzava; sve ostalo kroz stb. */
static uint8_t *decode_image(const uint8_t *raw, size_t len, int *w, int *h,
                             const char **err)
{
#ifdef HAVE_WEBP
    /* RIFF....WEBP - provera zaglavlja je pouzdanija od ekstenzije, jer se
     * u divljini nalaze .jpg fajlovi koji su zapravo webp. */
    if (len >= 12 && !memcmp(raw, "RIFF", 4) && !memcmp(raw + 8, "WEBP", 4)) {
        uint8_t *wp = WebPDecodeRGBA(raw, len, w, h);
        if (!wp) {
            *err = "libwebp: neispravan webp";
            return NULL;
        }
        /* libwebp trazi WebPFree, a ostatak koda oslobadja sa free(). Umesto
         * da se oslanjam na to da su isti, kopiram u sopstveni bafer. */
        size_t   need = (size_t)(*w) * (size_t)(*h) * 4u;
        uint8_t *px   = malloc(need);
        if (px)
            memcpy(px, wp, need);
        else
            *err = "nema memorije";
        WebPFree(wp);
        return px;
    }
#endif
    int      ch = 0;
    uint8_t *px = stbi_load_from_memory(raw, (int)len, w, h, &ch, 4);
    if (!px)
        *err = stbi_failure_reason();
    return px;
}

static int entry_sort_cb(const void *a, const void *b)
{
    return natural_cmp(((const entry_t *)a)->name, ((const entry_t *)b)->name);
}

/* ------------------------------------------------------------------ */

static struct archive *ar_open(const char *path)
{
    struct archive *a = archive_read_new();
    if (!a)
        return NULL;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (archive_read_open_filename(a, path, BLOCK_SIZE) != ARCHIVE_OK) {
        ERR("archive_read_open_filename(%s): %s", path, archive_error_string(a));
        archive_read_free(a);
        return NULL;
    }
    return a;
}

static void ar_reset(doc_t *d)
{
    if (d->ar) {
        archive_read_free(d->ar);
        d->ar = NULL;
    }
    d->pos = 0;
}

/* Pozicionira stream tako da je `entry` zaglavlje sa rednim brojem `ord`
 * upravo procitano i telo spremno za citanje. */
static int ar_seek_to(doc_t *d, int ord, struct archive_entry **entry)
{
    if (!d->ar || d->pos > ord) {
        ar_reset(d);
        d->ar = ar_open(d->path);
        if (!d->ar)
            return -1;
    }

    while (d->pos <= ord) {
        int r = archive_read_next_header(d->ar, entry);
        if (r == ARCHIVE_EOF) {
            ERR("neocekivan EOF pri trazenju unosa %d", ord);
            ar_reset(d);
            return -1;
        }
        if (r != ARCHIVE_OK && r != ARCHIVE_WARN) {
            ERR("archive_read_next_header: %s", archive_error_string(d->ar));
            ar_reset(d);
            return -1;
        }
        d->pos++;
        if (d->pos - 1 == ord)
            return 0;
    }
    return -1;
}

/* Cita celo telo trenutnog unosa u memoriju. */
static uint8_t *ar_read_body(struct archive *a, struct archive_entry *e, size_t *out_len)
{
    la_int64_t hint = archive_entry_size_is_set(e) ? archive_entry_size(e) : 0;
    size_t cap = hint > 0 ? (size_t)hint : (1u << 20);
    size_t len = 0;

    uint8_t *buf = malloc(cap);
    if (!buf)
        return NULL;

    for (;;) {
        if (len == cap) {
            size_t ncap = cap * 2;
            uint8_t *nbuf = realloc(buf, ncap);
            if (!nbuf) {
                free(buf);
                return NULL;
            }
            buf = nbuf;
            cap = ncap;
        }
        la_ssize_t n = archive_read_data(a, buf + len, cap - len);
        if (n < 0) {
            ERR("archive_read_data: %s", archive_error_string(a));
            free(buf);
            return NULL;
        }
        if (n == 0)
            break;
        len += (size_t)n;
    }

    *out_len = len;
    return buf;
}

/* ------------------------------------------------------------------ */

static int ab_probe(const char *path)
{
    return is_archive_ext(path_ext(path));
}

static doc_t *ab_open(const char *path)
{
    struct archive       *a = ar_open(path);
    struct archive_entry *e;

    if (!a)
        return NULL;

    doc_t *d = calloc(1, sizeof(*d));
    if (!d) {
        archive_read_free(a);
        return NULL;
    }
    snprintf(d->path, sizeof(d->path), "%s", path);

    /* Prvi prolaz: popis slika i njihovih rednih brojeva. */
    int cap = 64, ord = 0;
    d->entries = malloc((size_t)cap * sizeof(entry_t));
    if (!d->entries) {
        free(d);
        archive_read_free(a);
        return NULL;
    }

    while (archive_read_next_header(a, &e) == ARCHIVE_OK) {
        const char *name = archive_entry_pathname(e);
        int this_ord = ord++;

        if (!name)
            continue;
        if (archive_entry_filetype(e) != AE_IFREG)
            continue;
        if (!is_image_ext(path_ext(name)))
            continue;
        /* macOS smece koje se cesto nadje u CBZ-ovima */
        if (strstr(name, "__MACOSX") || path_base(name)[0] == '.')
            continue;

        if (d->n_entries == cap) {
            cap *= 2;
            entry_t *ne = realloc(d->entries, (size_t)cap * sizeof(entry_t));
            if (!ne)
                break;
            d->entries = ne;
        }
        d->entries[d->n_entries].name = strdup(name);
        d->entries[d->n_entries].ord  = this_ord;
        d->n_entries++;
    }

    archive_read_free(a);

    if (d->n_entries == 0) {
        ERR("%s: nema prepoznatih slika", path);
        free(d->entries);
        free(d);
        return NULL;
    }

    qsort(d->entries, (size_t)d->n_entries, sizeof(entry_t), entry_sort_cb);
    LOG("%s: %d stranica", path_base(path), d->n_entries);

    d->ar  = NULL;
    d->pos = 0;
    return d;
}

static int ab_page_count(doc_t *d)
{
    return d->n_entries;
}

static int ab_render(doc_t *d, int index, doc_page_t *out)
{
    struct archive_entry *e;

    if (index < 0 || index >= d->n_entries)
        return -1;

    if (ar_seek_to(d, d->entries[index].ord, &e) != 0)
        return -1;

    size_t   len = 0;
    uint8_t *raw = ar_read_body(d->ar, e, &len);
    if (!raw)
        return -1;

    int         w = 0, h = 0;
    const char *derr = "nepoznata greska";
    uint8_t    *rgba = decode_image(raw, len, &w, &h, &derr);
    free(raw);

    if (!rgba) {
        ERR("dekodiranje %s: %s", d->entries[index].name, derr);
        return -1;
    }

    out->width  = w;
    out->height = h;
    out->pixels = rgba;
    return 0;
}

static void ab_close(doc_t *d)
{
    if (!d)
        return;
    ar_reset(d);
    for (int i = 0; i < d->n_entries; i++)
        free(d->entries[i].name);
    free(d->entries);
    free(d);
}

const doc_backend_t doc_backend_archive = {
    .name       = "archive",
    .probe      = ab_probe,
    .open       = ab_open,
    .page_count = ab_page_count,
    .render     = ab_render,
    .close      = ab_close,
};
