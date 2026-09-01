/* source.c - pravila zajednicka svim izvorima
 *
 * Filtriranje i sortiranje su ovdje, a ne u pojedinacnim backend-ima, da
 * bi parseri (dirent, WebDAV XML, HTML) ostali cisti parseri i da bi
 * pravilo postojalo na jednom mjestu.
 */
#include "source.h"
#include "common.h"
#include "doc.h"

int source_entry_cmp(const void *a, const void *b)
{
    const lib_entry_t *x = a, *y = b;

    if (x->is_dir != y->is_dir)
        return y->is_dir - x->is_dir;   /* folderi prvi */
    return natural_cmp(x->name, y->name);
}

void source_filter_sort(lib_entry_t *arr, int *n)
{
    int w = 0;

    for (int r = 0; r < *n; r++) {
        /* QNAP ostavlja @Recycle i @Transcode; tacka pokriva skrivene. */
        if (arr[r].name[0] == '.' || arr[r].name[0] == '@')
            continue;
        if (!arr[r].is_dir && !doc_is_supported(arr[r].path))
            continue;
        if (w != r)
            arr[w] = arr[r];
        w++;
    }

    *n = w;
    if (w > 1)
        qsort(arr, (size_t)w, sizeof *arr, source_entry_cmp);
}

void source_free(source_t *s)
{
    if (!s)
        return;
    if (s->be && s->be->close)
        s->be->close(s);
    free(s);
}
