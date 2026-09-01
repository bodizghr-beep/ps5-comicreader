/* doc.c */
#include "doc.h"
#include "common.h"

static const doc_backend_t *g_backends[] = {
    &doc_backend_archive,
#ifdef HAVE_MUPDF
    &doc_backend_pdf,
#endif
    NULL
};

void doc_page_free(doc_page_t *p)
{
    if (!p)
        return;
    free(p->pixels);
    p->pixels = NULL;
    p->width = p->height = 0;
}

const doc_backend_t *doc_backend_for(const char *path)
{
    for (int i = 0; g_backends[i]; i++) {
        if (g_backends[i]->probe(path))
            return g_backends[i];
    }
    return NULL;
}

int doc_is_supported(const char *path)
{
    return doc_backend_for(path) != NULL;
}
