/* test_source_usb.c - listanje jednog nivoa i identitet fetch-a */
#include "source.h"
#include "common.h"
#include <assert.h>
#include <sys/stat.h>
#include <unistd.h>

static void touch(const char *dir, const char *name)
{
    char p[512];
    snprintf(p, sizeof p, "%s/%s", dir, name);
    FILE *f = fopen(p, "w");
    assert(f);
    fputc('x', f);
    fclose(f);
}

int main(void)
{
    char tmpl[] = "/tmp/cr_usbXXXXXX";
    char *root = mkdtemp(tmpl);
    assert(root);

    char sub[512];
    snprintf(sub, sizeof sub, "%s/Stripovi", root);
    assert(mkdir(sub, 0755) == 0);
    snprintf(sub, sizeof sub, "%s/@Recycle", root);
    assert(mkdir(sub, 0755) == 0);

    touch(root, "b2.cbz");
    touch(root, "b10.cbz");
    touch(root, "readme.txt");
    touch(root, ".skriveno.cbz");

    source_t *s = source_usb_new(root);
    assert(s);

    lib_entry_t *e = NULL;
    int n = 0;
    assert(s->be->list(s, root, &e, &n) == 0);

    /* Prolaze: folder Stripovi, b2.cbz, b10.cbz.
     * Otpadaju: @Recycle (sistemski), readme.txt (nepodrzan),
     *           .skriveno.cbz (skriven). */
    assert(n == 3);

    /* Folder prvi, pa fajlovi prirodnim redom: b2 prije b10. */
    assert(e[0].is_dir == 1);
    assert(!strcmp(e[0].name, "Stripovi"));
    assert(e[1].is_dir == 0);
    assert(!strcmp(e[1].name, "b2"));       /* naslov bez ekstenzije */
    assert(!strcmp(e[2].name, "b10"));
    assert(e[1].last_page == -1);

    /* path je puna putanja, ne samo ime */
    char want[512];
    snprintf(want, sizeof want, "%s/b2.cbz", root);
    assert(!strcmp(e[1].path, want));

    free(e);

    /* fetch nad USB-om je identitet i ne dira disk */
    char local[LIB_PATH_MAX];
    assert(s->be->fetch(s, "/mnt/usb0/x.cbz", local, sizeof local, NULL, NULL) == 0);
    assert(!strcmp(local, "/mnt/usb0/x.cbz"));

    source_free(s);
    printf("test_source_usb OK\n");
    return 0;
}
