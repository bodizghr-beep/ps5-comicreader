/* doc_pdf.c - PDF preko MuPDF-a
 *
 * Kompajlira se samo uz -DHAVE_MUPDF. Namerno je odvojeno: cross-kompajliranje
 * MuPDF-a za PS5 toolchain je najnepredvidiviji deo projekta, pa prvi radni
 * build ne sme da zavisi od njega.
 *
 * Napomena o nitima: MuPDF zvanicno trazi fz_clone_context() po niti. Ovde se
 * oslanjamo na to da kes serijalizuje sve pozive nad jednim dokumentom kroz
 * jedan mutex, pa se kontekst nikad ne dodiruje iz dve niti istovremeno.
 */
#ifdef HAVE_MUPDF

#include "doc.h"
#include "common.h"

#include <mupdf/fitz.h>

/* Ciljana visina renderovane stranice u pikselima. Stranice se renderuju na
 * fiksnu rezoluciju, a GPU ih skalira na ekran - jeftinije nego re-render. */
#define TARGET_HEIGHT 1800.0f

struct doc {
    fz_context  *ctx;
    fz_document *fzdoc;
    int          n_pages;
};

static int pb_probe(const char *path)
{
    return !strcasecmp(path_ext(path), "pdf");
}

static doc_t *pb_open(const char *path)
{
    doc_t *d = calloc(1, sizeof(*d));
    if (!d)
        return NULL;

    d->ctx = fz_new_context(NULL, NULL, FZ_STORE_DEFAULT);
    if (!d->ctx) {
        ERR("fz_new_context nije uspeo");
        free(d);
        return NULL;
    }

    fz_try(d->ctx) {
        fz_register_document_handlers(d->ctx);
        d->fzdoc   = fz_open_document(d->ctx, path);
        d->n_pages = fz_count_pages(d->ctx, d->fzdoc);
    }
    fz_catch(d->ctx) {
        ERR("otvaranje %s: %s", path, fz_caught_message(d->ctx));
        if (d->fzdoc)
            fz_drop_document(d->ctx, d->fzdoc);
        fz_drop_context(d->ctx);
        free(d);
        return NULL;
    }

    if (d->n_pages <= 0) {
        ERR("%s: nema stranica", path);
        fz_drop_document(d->ctx, d->fzdoc);
        fz_drop_context(d->ctx);
        free(d);
        return NULL;
    }

    LOG("%s: %d stranica (pdf)", path_base(path), d->n_pages);
    return d;
}

static int pb_page_count(doc_t *d)
{
    return d->n_pages;
}

static int pb_render(doc_t *d, int index, doc_page_t *out)
{
    fz_pixmap *pix  = NULL;
    fz_page   *page = NULL;
    int        rc   = -1;

    if (index < 0 || index >= d->n_pages)
        return -1;

    fz_try(d->ctx) {
        page = fz_load_page(d->ctx, d->fzdoc, index);

        /* Zoom se racuna iz prirodne visine stranice da bi svi PDF-ovi,
         * bez obzira na format papira, dali slicnu rezoluciju. */
        fz_rect   bounds = fz_bound_page(d->ctx, page);
        float     ph     = bounds.y1 - bounds.y0;
        float     zoom   = (ph > 1.0f) ? (TARGET_HEIGHT / ph) : 1.0f;
        fz_matrix m      = fz_scale(zoom, zoom);

        /* alpha=1 daje 4 komponente po pikselu, sto se poklapa sa RGBA8888. */
        pix = fz_new_pixmap_from_page(d->ctx, page, m, fz_device_rgb(d->ctx), 1);
    }
    fz_always(d->ctx) {
        if (page)
            fz_drop_page(d->ctx, page);
    }
    fz_catch(d->ctx) {
        ERR("render stranice %d: %s", index, fz_caught_message(d->ctx));
        return -1;
    }

    if (pix->n != 4) {
        ERR("neocekivan broj kanala: %d", pix->n);
        fz_drop_pixmap(d->ctx, pix);
        return -1;
    }

    size_t   need = (size_t)pix->w * (size_t)pix->h * 4u;
    uint8_t *rgba = malloc(need);
    if (rgba) {
        /* MuPDF-ov stride ne mora biti w*4, pa se kopira red po red. */
        for (int y = 0; y < pix->h; y++)
            memcpy(rgba + (size_t)y * pix->w * 4,
                   pix->samples + (size_t)y * pix->stride,
                   (size_t)pix->w * 4);

        out->width  = pix->w;
        out->height = pix->h;
        out->pixels = rgba;
        rc = 0;
    }

    fz_drop_pixmap(d->ctx, pix);
    return rc;
}

static void pb_close(doc_t *d)
{
    if (!d)
        return;
    if (d->fzdoc)
        fz_drop_document(d->ctx, d->fzdoc);
    if (d->ctx)
        fz_drop_context(d->ctx);
    free(d);
}

const doc_backend_t doc_backend_pdf = {
    .name       = "pdf",
    .probe      = pb_probe,
    .open       = pb_open,
    .page_count = pb_page_count,
    .render     = pb_render,
    .close      = pb_close,
};

#endif /* HAVE_MUPDF */
