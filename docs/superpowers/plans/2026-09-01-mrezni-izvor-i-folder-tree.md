# Mrežni izvor i folder tree — plan implementacije

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Dodati hijerarhijsko kretanje kroz foldere i mrežni WebDAV/HTTP izvor uz postojeći USB skener, tako da oba izvora dijele isti UI i isti navigacijski kod.

**Architecture:** Novi `source.h` vtable (po uzoru na postojeći `doc.h`) skriva izvor iza `list`/`fetch`/`close`. `library.c` postaje navigacijski stek nad tim interfejsom; korijen stabla je sintetički nivo sa spiskom izvora. Mrežni izvor lista preko WebDAV PROPFIND-a, a velike arhive čita Range zahtjevima kroz libarchive callback-ove (`vfs_http.c`), bez preuzimanja cijelog fajla.

**Tech Stack:** C99, SDL2, libarchive, libcurl 8.18 (statički, `-DCURL_STATICLIB`), libxml2, stb_image, ps5-payload-sdk (prospero toolchain).

**Spec:** `docs/superpowers/specs/2026-09-01-mrezni-izvor-i-folder-tree-design.md`

## Global Constraints

- **`cache.c` se ne dira.** Nijedan zadatak ne smije mijenjati `src/cache.c`.
- **`doc_archive.c` se dira samo u `ar_open()`** (Task 12), nigdje drugdje.
- **`doc_pdf.c` se ne dira.** PDF preko mreže uvijek ide kroz download režim.
- Font je ASCII (`ui.c`); nikakva naša slova ni Unicode u UI stringovima.
- Komentari u `src/*.c` pišu se **bez dijakritike**, kao postojeći kod.
- Lozinka iz configa ne smije završiti ni u jednom `LOG`/`ERR` ispisu, ni u URL-u, ni u `.ps5cr_state`.
- Sav mrežni pristup je read-only. Nema `PUT`, `DELETE`, `MKCOL`.
- Samo `http://`. HTTPS se ne konfiguriše ni testira.
- Svaki task završava commitom. Poruka commita na jeziku postojećih commitova (bez dijakritike).
- Testovi se grade i puštaju pod `-fsanitize=address,undefined`, kao postojeći.

## Struktura fajlova

| Fajl | Odgovornost |
|---|---|
| `src/common.c/h` | + `is_url()`, `url_decode()` |
| `src/source.h` | interfejs izvora, `lib_entry_t`, konstante putanja |
| `src/source.c` | filtriranje i sortiranje zajedničko svim izvorima |
| `src/source_usb.c` | listanje jednog nivoa `opendir`-om, `fetch` = identitet |
| `src/source_http.c` | PROPFIND/autoindex listanje, izbor stream/download režima, download s progresom |
| `src/dav_parse.c/h` | PROPFIND XML → `lib_entry_t[]` (libxml2) |
| `src/html_parse.c/h` | autoindex HTML → `lib_entry_t[]` |
| `src/vfs_http.c/h` | Range-backed libarchive callback-ovi, adaptivni chunk, keš zaglavlja, ponavljanje, tabela kredencijala |
| `src/config.c/h` | `.ps5cr.conf` → `config_t` |
| `src/library.c/h` | navigacijski stek, breadcrumb, stanje čitanja |
| `src/doc_archive.c` | **samo** `ar_open()`: URL → vfs callback-ovi |
| `src/main.c` | stablo u `draw_browser`, `SCREEN_FETCH`, oporavak od prekida |
| `tests/range_server.py` | HTTP server s Range podrškom i ubacivanjem grešaka |

**Faze.** Faza 1 (Task 1-5) je samostalno upotrebljiva: folder tree radi nad USB-om, bez ijedne mrežne zavisnosti. Faza 2 (Task 6-8) dodaje mrežno listanje. Faza 3 (Task 9-17) dodaje čitanje preko mreže.

---

## FAZA 1 — Stablo nad USB-om

### Task 0: Host zavisnosti za testove

Bez ovoga svaki `Step 2` u planu pada na `archive.h: No such file or directory`, što izgleda
kao greška u kodu a nije. Na ovom hostu nema nijednog `-dev` paketa i nema root pristupa,
pa se koristi isti obrazac koji `SETUP.md` već propisuje za LLVM: `apt-get download` +
`dpkg-deb -x` u lokalni direktorij.

**Files:**
- Create: `scripts/host_deps.sh`
- Modify: `Makefile` (dodaje `HOST_INC` / `HOST_LIBS` i koristi ih u `host` i `test`)

**Interfaces:**
- Produces: `$HOSTROOT/root/usr/{include,lib}` i simlinkove `libarchive.so`, `libxml2.so`,
  `libcurl.so` koji pokazuju na sistemske runtime biblioteke. Podrazumijevani `HOSTROOT`
  je `$HOME/.cache/ps5cr-hostdeps`.

- [ ] **Step 1: Napiši skriptu**

Kreiraj `scripts/host_deps.sh`:

```sh
#!/bin/sh
# Dovlaci -dev zaglavlja za host build i testove, bez root privilegija.
# Runtime biblioteke (libarchive.so.13 i sl.) vec postoje na sistemu; ovdje
# nedostaju samo zaglavlja i .so simlinkovi na koje linker cilja.
set -e

HOSTROOT="${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}"
# SDL2 ide i kao -dev i kao runtime: na ovom hostu nema nijednog od njih,
# a host build bez SDL2 ne postoji. libsdl2-2.0-0 je samo alternatives
# omotac - pravi .so nosi libsdl2-classic.
PKGS="libarchive-dev libxml2-dev libcurl4-openssl-dev libsdl2-dev libsdl2-2.0-0 libsdl2-classic"

mkdir -p "$HOSTROOT/debs" "$HOSTROOT/root/usr/lib"
cd "$HOSTROOT/debs"

for p in $PKGS; do
    if ! ls "${p}"_*.deb >/dev/null 2>&1; then
        echo "dovlacim $p"
        apt-get download "$p"
    fi
done

for d in *.deb; do
    dpkg-deb -x "$d" "$HOSTROOT/root"
done

# Dev paket donosi zaglavlja, ali .so simlink u njemu pokazuje na fajl koji
# u ovom rootu ne postoji. Zato se simlink pravi rucno, na sistemsku biblioteku.
for n in archive xml2 curl; do
    real=$(ldconfig -p | awk -v pat="lib$n.so." '$1 ~ ("^" pat) { print $NF; exit }')
    if [ -z "$real" ]; then
        echo "nema runtime biblioteke lib$n.so na sistemu" >&2
        exit 1
    fi
    ln -sf "$real" "$HOSTROOT/root/usr/lib/lib$n.so"
    echo "  lib$n.so -> $real"
done

echo
echo "Gotovo. Makefile ovo nalazi sam preko HOSTROOT=$HOSTROOT"
```

```bash
chmod +x scripts/host_deps.sh
```

- [ ] **Step 2: Pokreni je i provjeri da radi**

```bash
scripts/host_deps.sh
```

Očekivano: tri `lib*.so -> /usr/lib/...` linije. Zatim provjera da se sve troje kompajlira
zajedno:

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
printf '#include <archive.h>\n#include <libxml/parser.h>\n#include <curl/curl.h>\nint main(void){return 0;}\n' > /tmp/cr_dep.c
cc -I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 \
   -I$HOSTROOT/root/usr/include/x86_64-linux-gnu \
   -o /tmp/cr_dep /tmp/cr_dep.c -L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl && echo "zavisnosti OK"
```

Očekivano: `zavisnosti OK`.

- [ ] **Step 3: Uvedi zastavice u Makefile**

U `Makefile`, odmah poslije `BASE_CFLAGS`:

```make
# Host build i testovi: zaglavlja iz lokalnog roota (scripts/host_deps.sh),
# linkovanje na sistemske runtime biblioteke preko simlinkova u tom rootu.
HOSTROOT  ?= $(HOME)/.cache/ps5cr-hostdeps
HOST_INC  := -I$(HOSTROOT)/root/usr/include              -I$(HOSTROOT)/root/usr/include/libxml2              -I$(HOSTROOT)/root/usr/include/x86_64-linux-gnu
HOST_LIBS := -L$(HOSTROOT)/root/usr/lib -larchive -lxml2 -lcurl
```

Zamijeni postojeće `PKG_CFLAGS` / `PKG_LIBS` korištenje u `host` i `test` ciljevima tako da
`libarchive` više ne ide kroz `pkg-config` (SDL2 i libwebp ostaju kako jesu):

```make
PKG_CFLAGS := $(shell pkg-config --cflags $(WEBP_PKG) 2>/dev/null) $(HOST_INC)
PKG_LIBS   := $(shell pkg-config --libs $(WEBP_PKG) 2>/dev/null) $(HOST_LIBS)
```

- [ ] **Step 4: Provjeri da host build i dalje prolazi**

```bash
scripts/deps.sh && make host && echo "host build OK"
```

Očekivano: `host build OK`. Ovo je i regresiona provjera da Task 0 nije razbio zatečeni build.

- [ ] **Step 5: Commit**

```bash
git add scripts/host_deps.sh Makefile
git commit -m "build: host zavisnosti bez root-a, po obrascu iz SETUP.md"
```

---

### Task 1: `is_url()` i `url_decode()` u `common.c`

**Files:**
- Modify: `src/common.h:28-34`, `src/common.c`
- Test: `tests/test_common.c` (novi)

**Interfaces:**
- Consumes: ništa
- Produces: `int is_url(const char *path)` — 1 za `http://` prefiks, inače 0.
  `void url_decode(char *dst, size_t dstlen, const char *src)` — dekodira `%XX`;
  nepotpun escape ostaje doslovno.

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_common.c`:

```c
/* test_common.c - pomocne funkcije nad putanjama */
#include "common.h"
#include <assert.h>

int main(void)
{
    assert(is_url("http://<ip-nas>:5000/a.cbr") == 1);
    assert(is_url("HTTP://VELIKA.SLOVA/a.cbr") == 1);
    assert(is_url("/mnt/usb0/a.cbr") == 0);
    assert(is_url("") == 0);
    assert(is_url("http:/") == 0);

    char b[64];

    url_decode(b, sizeof b, "Stripoteka%2041-50.cbr");
    assert(!strcmp(b, "Stripoteka 41-50.cbr"));

    /* iz stvarnog PROPFIND odgovora: zarez i zagrada u imenu */
    url_decode(b, sizeof b, "a%2Cb%29c");
    assert(!strcmp(b, "a,b)c"));

    url_decode(b, sizeof b, "bez-escape");
    assert(!strcmp(b, "bez-escape"));

    /* nepotpun escape se ne smije progutati */
    url_decode(b, sizeof b, "krnj%2");
    assert(!strcmp(b, "krnj%2"));

    /* ne smije prepisati preko bafera */
    char small[5];
    url_decode(small, sizeof small, "abcdefgh");
    assert(strlen(small) == 4);

    printf("test_common OK\n");
    return 0;
}
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_common tests/test_common.c src/common.c && /tmp/cr_t_common
```

Očekivano: FAIL na linkovanju, `undefined reference to 'is_url'`.

- [ ] **Step 3: Implementiraj**

U `src/common.h`, poslije deklaracije `path_base`:

```c
/* Vraca 1 ako putanja pocinje sa http:// (bez obzira na velicinu slova). */
int  is_url(const char *path);

/* Dekodira %XX sekvence. Nepotpun escape se prepisuje doslovno. */
void url_decode(char *dst, size_t dstlen, const char *src);
```

U `src/common.c`, na kraj fajla:

```c
int is_url(const char *path)
{
    return path && !strncasecmp(path, "http://", 7);
}

void url_decode(char *dst, size_t dstlen, const char *src)
{
    size_t o = 0;

    if (!dstlen)
        return;

    for (const char *p = src; *p && o + 1 < dstlen; p++) {
        if (*p == '%' && isxdigit((unsigned char)p[1]) &&
                         isxdigit((unsigned char)p[2])) {
            char hex[3] = { p[1], p[2], '\0' };
            dst[o++] = (char)strtol(hex, NULL, 16);
            p += 2;
        } else {
            dst[o++] = *p;
        }
    }
    dst[o] = '\0';
}
```

- [ ] **Step 4: Pusti test da prođe**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_common tests/test_common.c src/common.c && /tmp/cr_t_common
```

Očekivano: `test_common OK`, bez prijava sanitizera.

- [ ] **Step 5: Commit**

```bash
git add src/common.c src/common.h tests/test_common.c
git commit -m "common: is_url i url_decode"
```

---

### Task 2: `source.h` interfejs i `source_usb.c`

**Files:**
- Create: `src/source.h`, `src/source.c`, `src/source_usb.c`
- Test: `tests/test_source_usb.c`

**Interfaces:**
- Consumes: `is_url` (Task 1), postojeći `doc_is_supported()`, `natural_cmp()`, `path_base()`
- Produces:
  - `lib_entry_t { char name[LIB_TITLE_MAX]; char path[LIB_PATH_MAX]; int is_dir; int last_page; }`
  - `source_t`, `source_backend_t` sa `list`/`fetch`/`close`
  - `source_t *source_usb_new(const char *root)`
  - `void source_free(source_t *s)`
  - `int source_entry_cmp(const void *a, const void *b)` — folderi prije fajlova, unutar grupe `natural_cmp` po `name`
  - `void source_filter_sort(lib_entry_t *arr, int *n)` — izbacuje skrivene (`.`/`@`) i nepodržane fajlove, pa sortira. **Zajednički za sve izvore**, da se pravilo filtriranja ne dupla i da parseri ostanu čisti parseri.

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_source_usb.c`:

```c
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
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_usb tests/test_source_usb.c src/source.c src/source_usb.c src/common.c \
   src/doc.c src/doc_archive.c src/stb_impl.c \
   $HOST_INC $HOST_LIBS -lm && /tmp/cr_t_usb
```

Očekivano: FAIL — `src/source.h: No such file or directory`.

- [ ] **Step 3: Implementiraj `src/source.h`**

```c
/* source.h - apstrakcija nad izvorom dokumenata
 *
 * Isti obrazac kao doc.h: UI i navigacija ne znaju da li citaju USB ili
 * WebDAV. Dodavanje treceg izvora znaci jos jednu implementaciju ove
 * strukture, bez diranja library.c ni main.c.
 */
#ifndef SOURCE_H
#define SOURCE_H

#include <stdint.h>
#include <stddef.h>

#define LIB_PATH_MAX  1024
#define LIB_TITLE_MAX 192
#define SRC_NAME_MAX  64

/* Jedan red u listi: folder ili dokument. */
typedef struct {
    char name[LIB_TITLE_MAX];   /* za prikaz: ime foldera ili naslov bez ekstenzije */
    char path[LIB_PATH_MAX];    /* USB: /mnt/usb0/x.cbz   HTTP: puni URL */
    int  is_dir;
    int  last_page;             /* -1 ako nije citano */
} lib_entry_t;

typedef struct source source_t;

/* Vraca !=0 da otkaze prenos. */
typedef int (*src_progress_fn)(void *ud, int64_t got, int64_t total);

typedef struct {
    const char *kind;                                  /* "usb" / "http" */

    /* Lista sadrzaj `path`. Alocira niz, pozivalac ga oslobadja sa free().
     * 0 = uspjeh, -1 = greska (opis u s->err). */
    int  (*list)(source_t *s, const char *path, lib_entry_t **out, int *n);

    /* Priprema putanju koju ce dobiti cache_open().
     * USB: kopira `path`. HTTP: URL (stream) ili lokalni fajl (download).
     * 0 = uspjeh, -1 = greska, 1 = korisnik otkazao. */
    int  (*fetch)(source_t *s, const char *path, char *local, size_t len,
                  src_progress_fn cb, void *ud);

    void (*close)(source_t *s);
} source_backend_t;

struct source {
    const source_backend_t *be;
    char  name[SRC_NAME_MAX];     /* prikaz u korijenu stabla */
    char  root[LIB_PATH_MAX];
    char  err[128];               /* posljednja greska, za prikaz u listi */
    void *priv;
};

source_t *source_usb_new(const char *root);
void      source_free(source_t *s);

/* Folderi prije fajlova, unutar grupe natural_cmp po imenu. */
int source_entry_cmp(const void *a, const void *b);

/* Izbacuje skrivene i nepodrzane unose, pa sortira. Folderi uvijek prolaze.
 * Zajednicko za USB i za mrezu - pravilo filtriranja postoji na jednom mjestu. */
void source_filter_sort(lib_entry_t *arr, int *n);

#endif /* SOURCE_H */
```

- [ ] **Step 4: Implementiraj `src/source_usb.c`**

Prvo `src/source.c` sa zajednickim pravilima:

```c
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
```

Zatim `src/source_usb.c`:

```c
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
    (void)s;

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

        snprintf(e->name, sizeof e->name, "%s", de->d_name);
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

void source_free(source_t *s)
{
    if (!s)
        return;
    if (s->be && s->be->close)
        s->be->close(s);
    free(s);
}
```

- [ ] **Step 5: Pusti test da prođe**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_usb tests/test_source_usb.c src/source.c src/source_usb.c src/common.c \
   src/doc.c src/doc_archive.c src/stb_impl.c \
   $HOST_INC $HOST_LIBS -lm && /tmp/cr_t_usb
```

Očekivano: `test_source_usb OK`.

- [ ] **Step 6: Commit**

```bash
git add src/source.h src/source.c src/source_usb.c tests/test_source_usb.c
git commit -m "source: interfejs izvora i USB backend bez rekurzije"
```

---

### Task 3: `config.c` — parser `.ps5cr.conf`

**Files:**
- Create: `src/config.h`, `src/config.c`
- Test: `tests/test_config.c`

**Interfaces:**
- Consumes: ništa iz ranijih taskova
- Produces:
  - `cfg_source_t { char name[SRC_NAME_MAX]; char url[LIB_PATH_MAX]; char type[16]; char user[64]; char pass[64]; }`
  - `config_t { int cache_mb; cfg_source_t srcs[CFG_SRC_MAX]; int n_srcs; }`
  - `int config_load(config_t *c, const char *path)` — 0 ok, -1 nema fajla
  - `int config_find(char *out, size_t len)` — traži `.ps5cr.conf` po `/mnt/usb0..7`, 0 ako našao

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_config.c`:

```c
/* test_config.c - parser konfiguracije */
#include "config.h"
#include "common.h"
#include <assert.h>

static const char *SAMPLE =
    "# komentar\n"
    "cache_mb = 2048\n"
    "\n"
    "[source]\n"
    "name = qnap\n"
    "url  = http://<ip-nas>:5000/STRIPOVI/\n"
    "user = PS5\n"
    "pass = tajna\n"
    "\n"
    "[source]\n"
    "name = drugi\n"
    "url = http://1.2.3.4:8080/x/\n"
    "type = autoindex\n"
    "nepoznat_kljuc = svejedno\n";

int main(void)
{
    char tmpl[] = "/tmp/cr_cfgXXXXXX";
    int  fd = mkstemp(tmpl);
    assert(fd >= 0);
    assert(write(fd, SAMPLE, strlen(SAMPLE)) == (ssize_t)strlen(SAMPLE));
    close(fd);

    config_t c;
    assert(config_load(&c, tmpl) == 0);

    assert(c.cache_mb == 2048);
    assert(c.n_srcs == 2);

    assert(!strcmp(c.srcs[0].name, "qnap"));
    assert(!strcmp(c.srcs[0].url,  "http://<ip-nas>:5000/STRIPOVI/"));
    assert(!strcmp(c.srcs[0].user, "PS5"));
    assert(!strcmp(c.srcs[0].pass, "tajna"));
    assert(!strcmp(c.srcs[0].type, "auto"));       /* default */

    assert(!strcmp(c.srcs[1].name, "drugi"));
    assert(!strcmp(c.srcs[1].type, "autoindex"));
    assert(c.srcs[1].user[0] == '\0');             /* nema auth */

    unlink(tmpl);

    /* Nepostojeci fajl nije fatalno - app radi samo s USB-om. */
    config_t c2;
    assert(config_load(&c2, "/nema/ovoga") == -1);
    assert(c2.n_srcs == 0);
    assert(c2.cache_mb == 4096);                   /* default ostaje */

    printf("test_config OK\n");
    return 0;
}
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_cfg tests/test_config.c src/config.c src/common.c && /tmp/cr_t_cfg
```

Očekivano: FAIL — `src/config.h: No such file or directory`.

- [ ] **Step 3: Implementiraj `src/config.h`**

```c
/* config.h - .ps5cr.conf sa USB-a */
#ifndef CONFIG_H
#define CONFIG_H

#include "source.h"

#define CFG_SRC_MAX 8

typedef struct {
    char name[SRC_NAME_MAX];
    char url[LIB_PATH_MAX];
    char type[16];              /* auto | webdav | autoindex */
    char user[64];
    char pass[64];
} cfg_source_t;

typedef struct {
    int          cache_mb;      /* gornja granica kesa; default 4096 */
    cfg_source_t srcs[CFG_SRC_MAX];
    int          n_srcs;
} config_t;

/* 0 = procitano, -1 = nema fajla (c je i tada popunjen defaultima). */
int config_load(config_t *c, const char *path);

/* Trazi .ps5cr.conf po /mnt/usb0..7. 0 ako je nadjen, -1 inace. */
int config_find(char *out, size_t len);

#endif /* CONFIG_H */
```

- [ ] **Step 4: Implementiraj `src/config.c`**

```c
/* config.c
 *
 * Format je namjerno primitivan: kljuc = vrijednost, sekcije [source].
 * Nepoznat kljuc se preskace uz upozorenje - stariji config ne smije
 * oboriti aplikaciju.
 */
#include "config.h"
#include "common.h"

#include <sys/stat.h>

#define CONF_NAME ".ps5cr.conf"
#define USB_SLOTS 8

static char *trim(char *s)
{
    while (*s == ' ' || *s == '\t')
        s++;

    char *end = s + strlen(s);
    while (end > s && (end[-1] == ' ' || end[-1] == '\t' ||
                       end[-1] == '\r' || end[-1] == '\n'))
        *--end = '\0';
    return s;
}

static void set_field(cfg_source_t *sr, const char *key, const char *val)
{
    if      (!strcmp(key, "name")) snprintf(sr->name, sizeof sr->name, "%s", val);
    else if (!strcmp(key, "url"))  snprintf(sr->url,  sizeof sr->url,  "%s", val);
    else if (!strcmp(key, "type")) snprintf(sr->type, sizeof sr->type, "%s", val);
    else if (!strcmp(key, "user")) snprintf(sr->user, sizeof sr->user, "%s", val);
    else if (!strcmp(key, "pass")) snprintf(sr->pass, sizeof sr->pass, "%s", val);
    else LOG("config: nepoznat kljuc '%s' u [source], preskacem", key);
}

int config_load(config_t *c, const char *path)
{
    memset(c, 0, sizeof *c);
    c->cache_mb = 4096;

    FILE *f = fopen(path, "r");
    if (!f)
        return -1;

    char line[LIB_PATH_MAX + 128];
    int  cur = -1;      /* indeks tekuceg [source] bloka */

    while (fgets(line, sizeof line, f)) {
        char *s = trim(line);

        if (!*s || *s == '#' || *s == ';')
            continue;

        if (!strcmp(s, "[source]")) {
            if (c->n_srcs >= CFG_SRC_MAX) {
                ERR("config: vise od %d izvora, ostatak se ignorise", CFG_SRC_MAX);
                cur = -1;
                continue;
            }
            cur = c->n_srcs++;
            snprintf(c->srcs[cur].type, sizeof c->srcs[cur].type, "auto");
            continue;
        }

        char *eq = strchr(s, '=');
        if (!eq) {
            LOG("config: linija bez '=' preskocena: %s", s);
            continue;
        }
        *eq = '\0';

        char *key = trim(s);
        char *val = trim(eq + 1);

        if (cur < 0) {
            if (!strcmp(key, "cache_mb"))
                c->cache_mb = atoi(val);
            else
                LOG("config: nepoznat globalni kljuc '%s', preskacem", key);
        } else {
            set_field(&c->srcs[cur], key, val);
        }
    }

    fclose(f);

    /* Lozinka se namjerno ne ispisuje. */
    for (int i = 0; i < c->n_srcs; i++)
        LOG("config: izvor '%s' -> %s (type=%s, auth=%s)",
            c->srcs[i].name, c->srcs[i].url, c->srcs[i].type,
            c->srcs[i].user[0] ? "da" : "ne");

    return 0;
}

int config_find(char *out, size_t len)
{
    for (int i = 0; i < USB_SLOTS; i++) {
        char        p[LIB_PATH_MAX];
        struct stat st;

        snprintf(p, sizeof p, "/mnt/usb%d/%s", i, CONF_NAME);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, len, "%s", p);
            return 0;
        }
    }

    /* Host build: CR_ROOT zamjenjuje /mnt/usbN. */
    const char *ovr = getenv("CR_ROOT");
    if (ovr && *ovr) {
        char        p[LIB_PATH_MAX];
        struct stat st;
        snprintf(p, sizeof p, "%s/%s", ovr, CONF_NAME);
        if (stat(p, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, len, "%s", p);
            return 0;
        }
    }
    return -1;
}
```

- [ ] **Step 5: Pusti test da prođe**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_cfg tests/test_config.c src/config.c src/common.c && /tmp/cr_t_cfg
```

Očekivano: `test_config OK`. U izlazu se vidi `auth=da` za prvi izvor — **provjeri da se lozinka nigdje ne ispisuje**.

- [ ] **Step 6: Commit**

```bash
git add src/config.c src/config.h tests/test_config.c
git commit -m "config: parser .ps5cr.conf sa vise [source] blokova"
```

---

### Task 4: `library.c` — navigacijski stek

**Files:**
- Rewrite: `src/library.h`, `src/library.c`
- Test: `tests/test_nav.c`

**Interfaces:**
- Consumes: `source.h` (Task 2), `config.h` (Task 3)
- Produces:
  - `lib_level_t { source_t *src; char path[]; char title[]; lib_entry_t *entries; int count, sel, scroll; char err[128]; }`
  - `int library_init(library_t *l)` — USB slotovi + config → izvori → korijen
  - `void library_reset(library_t *l)` / `int library_add_source(library_t *l, source_t *s)` — za testove i za `library_init`
  - `int library_enter(library_t *l, int index)` — 0 ok, -1 odbijeno
  - `int library_back(library_t *l)` — 1 ako je izašao nivo, 0 ako je već korijen
  - `lib_level_t *library_cur(library_t *l)`
  - `void library_breadcrumb(const library_t *l, char *buf, size_t len)`
  - `int state_page_for(const library_t *l, const char *path)`
  - `void state_load(library_t *l)` / `void state_save(library_t *l, const char *path, int page)`

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_nav.c`:

```c
/* test_nav.c - navigacijski stek nad laznim izvorom, bez USB-a i mreze */
#include "library.h"
#include "common.h"
#include <assert.h>

/* Lazni izvor:
 *   /fake        -> [dir A, file doc1]
 *   /fake/A      -> [file doc2]
 *   /fake/deep.. -> uvijek jedan folder, za provjeru granice steka
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
    printf("test_nav OK\n");
    return 0;
}
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_nav tests/test_nav.c src/library.c src/config.c src/common.c \
   src/source.c src/source_usb.c src/doc.c src/doc_archive.c src/stb_impl.c \
   $HOST_INC $HOST_LIBS -lm && /tmp/cr_t_nav
```

Očekivano: FAIL — `library_reset` i ostalo ne postoje.

- [ ] **Step 3: Napiši `src/library.h`**

```c
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
```

- [ ] **Step 4: Napiši `src/library.c`**

Cijeli fajl (zamjenjuje postojeći):

```c
/* library.c - navigacijski stek
 *
 * Dubina 0 je sinteticki nivo sa spiskom izvora. Ulazak u izvor je isto
 * sto i ulazak u folder, pa UI ne poznaje pojam "izvora".
 */
#include "library.h"
#include "config.h"
#include "common.h"

#include <sys/stat.h>

#define STATE_FILE ".ps5cr_state"
#define USB_SLOTS  8

static void level_clear(lib_level_t *lv)
{
    free(lv->entries);
    lv->entries = NULL;
    lv->count   = 0;
    lv->err[0]  = '\0';
}

/* Korijen: po jedan red za svaki izvor. */
static void build_root(library_t *l)
{
    lib_level_t *lv = &l->stack[0];

    level_clear(lv);
    lv->src     = NULL;
    lv->path[0] = '\0';
    snprintf(lv->title, sizeof lv->title, "/");

    if (l->n_sources == 0)
        return;

    lv->entries = calloc((size_t)l->n_sources, sizeof *lv->entries);
    if (!lv->entries)
        return;

    for (int i = 0; i < l->n_sources; i++) {
        lib_entry_t *e = &lv->entries[i];
        snprintf(e->name, sizeof e->name, "%s", l->sources[i]->name);
        snprintf(e->path, sizeof e->path, "%s", l->sources[i]->root);
        e->is_dir    = 1;
        e->last_page = -1;
    }
    lv->count = l->n_sources;
}

void library_reset(library_t *l)
{
    memset(l, 0, sizeof *l);
    build_root(l);
}

int library_add_source(library_t *l, source_t *s)
{
    if (!s)
        return -1;
    if (l->n_sources >= LIB_SRC_MAX) {
        ERR("previse izvora, %s ignorisan", s->name);
        source_free(s);
        return -1;
    }
    l->sources[l->n_sources++] = s;
    build_root(l);
    return 0;
}

lib_level_t *library_cur(library_t *l)
{
    return &l->stack[l->depth];
}

/* Popunjava last_page iz ucitanog stanja. */
static void stamp_state(library_t *l, lib_level_t *lv)
{
    for (int i = 0; i < lv->count; i++)
        if (!lv->entries[i].is_dir)
            lv->entries[i].last_page = state_page_for(l, lv->entries[i].path);
}

int library_enter(library_t *l, int index)
{
    lib_level_t *cur = library_cur(l);

    if (index < 0 || index >= cur->count)
        return -1;
    if (!cur->entries[index].is_dir)
        return -1;

    if (l->depth + 1 >= LIB_DEPTH_MAX) {
        LOG("stek dubine %d je pun, ulazak odbijen", LIB_DEPTH_MAX);
        return -1;
    }

    source_t *src  = (l->depth == 0) ? l->sources[index] : cur->src;
    const char *p  = cur->entries[index].path;
    const char *nm = cur->entries[index].name;

    lib_level_t *nx = &l->stack[l->depth + 1];
    level_clear(nx);
    nx->src    = src;
    nx->sel    = 0;
    nx->scroll = 0;
    snprintf(nx->path,  sizeof nx->path,  "%s", p);
    snprintf(nx->title, sizeof nx->title, "%s", nm);

    if (src->be->list(src, p, &nx->entries, &nx->count) != 0) {
        nx->entries = NULL;
        nx->count   = 0;
        snprintf(nx->err, sizeof nx->err, "%s",
                 src->err[0] ? src->err : "listanje nije uspjelo");
    } else {
        stamp_state(l, nx);
    }

    l->depth++;
    return 0;
}

int library_back(library_t *l)
{
    if (l->depth == 0)
        return 0;

    level_clear(&l->stack[l->depth]);
    l->depth--;
    return 1;
}

void library_breadcrumb(const library_t *l, char *buf, size_t len)
{
    if (l->depth == 0) {
        snprintf(buf, len, "/");
        return;
    }

    size_t o = 0;
    buf[0] = '\0';
    for (int i = 1; i <= l->depth; i++) {
        int w = snprintf(buf + o, len - o, "%s/", l->stack[i].title);
        if (w < 0 || (size_t)w >= len - o)
            break;
        o += (size_t)w;
    }
}

int library_init(library_t *l)
{
    library_reset(l);

    /* Host build: CR_ROOT zamjenjuje /mnt/usbN. */
    const char *ovr = getenv("CR_ROOT");
    if (ovr && *ovr) {
        snprintf(l->root, sizeof l->root, "%s", ovr);
        library_add_source(l, source_usb_new(ovr));
    } else {
        for (int i = 0; i < USB_SLOTS; i++) {
            char        p[LIB_PATH_MAX];
            struct stat st;

            snprintf(p, sizeof p, "/mnt/usb%d", i);
            if (stat(p, &st) != 0 || !S_ISDIR(st.st_mode))
                continue;
            if (!l->root[0])
                snprintf(l->root, sizeof l->root, "%s", p);
            library_add_source(l, source_usb_new(p));
        }
    }

    char     cpath[LIB_PATH_MAX];
    config_t cfg;
    if (config_find(cpath, sizeof cpath) == 0 && config_load(&cfg, cpath) == 0) {
        for (int i = 0; i < cfg.n_srcs; i++) {
            source_t *s = source_http_new(cfg.srcs[i].name, cfg.srcs[i].url,
                                          cfg.srcs[i].type, cfg.srcs[i].user,
                                          cfg.srcs[i].pass, cfg.cache_mb);
            if (s)
                library_add_source(l, s);
        }
    }

    LOG("izvora: %d", l->n_sources);
    return l->n_sources;
}

void library_free(library_t *l)
{
    for (int i = 0; i <= l->depth; i++)
        level_clear(&l->stack[i]);
    level_clear(&l->stack[0]);

    for (int i = 0; i < l->n_sources; i++)
        source_free(l->sources[i]);
    l->n_sources = 0;

    free(l->state);
    l->state   = NULL;
    l->n_state = l->cap_state = 0;
}

/* ------------------------------------------------------------------ */
/* Stanje citanja. Format nepromijenjen: "putanja<TAB>stranica".       */
/* Kljuc je entry->path, dakle za mrezu puni URL.                      */

static void state_path(library_t *l, char *buf, size_t len)
{
    const char *root = l->root[0] ? l->root : "/mnt/usb0";
    int n = snprintf(buf, len, "%s/%s", root, STATE_FILE);
    if (n < 0 || (size_t)n >= len)
        snprintf(buf, len, "/mnt/usb0/%s", STATE_FILE);
}

static state_rec_t *state_find(library_t *l, const char *path)
{
    for (int i = 0; i < l->n_state; i++)
        if (!strcmp(l->state[i].path, path))
            return &l->state[i];
    return NULL;
}

int state_page_for(const library_t *l, const char *path)
{
    for (int i = 0; i < l->n_state; i++)
        if (!strcmp(l->state[i].path, path))
            return l->state[i].page;
    return -1;
}

void state_load(library_t *l)
{
    char sp[LIB_PATH_MAX + 16];
    state_path(l, sp, sizeof sp);

    FILE *f = fopen(sp, "r");
    if (!f)
        return;

    char line[LIB_PATH_MAX + 32];
    while (fgets(line, sizeof line, f)) {
        char *tab = strchr(line, '\t');
        if (!tab)
            continue;
        *tab = '\0';

        if (l->n_state == l->cap_state) {
            int          ncap = l->cap_state ? l->cap_state * 2 : 64;
            state_rec_t *ns   = realloc(l->state, (size_t)ncap * sizeof *ns);
            if (!ns)
                break;
            l->state     = ns;
            l->cap_state = ncap;
        }
        snprintf(l->state[l->n_state].path, LIB_PATH_MAX, "%s", line);
        l->state[l->n_state].page = atoi(tab + 1);
        l->n_state++;
    }
    fclose(f);
    LOG("stanje: %d zapisa", l->n_state);
}

void state_save(library_t *l, const char *path, int page)
{
    state_rec_t *r = state_find(l, path);

    if (r) {
        r->page = page;
    } else {
        if (l->n_state == l->cap_state) {
            int          ncap = l->cap_state ? l->cap_state * 2 : 64;
            state_rec_t *ns   = realloc(l->state, (size_t)ncap * sizeof *ns);
            if (!ns)
                return;
            l->state     = ns;
            l->cap_state = ncap;
        }
        snprintf(l->state[l->n_state].path, LIB_PATH_MAX, "%s", path);
        l->state[l->n_state].page = page;
        l->n_state++;
    }

    /* Osvjezi i vidljivi red, da badge "nastavi" odmah bude tacan. */
    lib_level_t *cur = library_cur(l);
    for (int i = 0; i < cur->count; i++)
        if (!strcmp(cur->entries[i].path, path))
            cur->entries[i].last_page = page;

    char sp[LIB_PATH_MAX], tmp[LIB_PATH_MAX + 16];
    state_path(l, sp, sizeof sp);
    snprintf(tmp, sizeof tmp, "%s.tmp", sp);

    /* Upis u privremeni fajl pa rename - gasenje konzole usred upisa
     * ne smije ostaviti polupraznu listu. */
    FILE *f = fopen(tmp, "w");
    if (!f) {
        ERR("ne mogu da upisem stanje u %s", tmp);
        return;
    }
    for (int i = 0; i < l->n_state; i++)
        if (l->state[i].page >= 0)
            fprintf(f, "%s\t%d\n", l->state[i].path, l->state[i].page);
    fclose(f);
    rename(tmp, sp);
}
```

> **Napomena za izvršioca:** `library_init()` poziva `source_http_new()`, koji nastaje tek u Tasku 8. Do tada u `src/source.h` stoji deklaracija, a privremenu implementaciju stavi u `src/source_http.c` kao stub istog potpisa (šest argumenata, uključujući `int cache_mb`) koji vraća `NULL` i loguje `LOG("http izvor jos nije implementiran")`. Task 8 ga zamjenjuje pravim. Test iz ovog taska ne dira tu granu.

- [ ] **Step 5: Dodaj stub i deklaraciju**

U `src/source.h`, ispod `source_usb_new`:

```c
source_t *source_http_new(const char *name, const char *url, const char *type,
                          const char *user, const char *pass, int cache_mb);
```

Kreiraj `src/source_http.c`:

```c
/* source_http.c - mrezni izvor (privremeni stub, vidi Task 8) */
#include "source.h"
#include "common.h"

source_t *source_http_new(const char *name, const char *url, const char *type,
                          const char *user, const char *pass, int cache_mb)
{
    (void)url; (void)type; (void)user; (void)pass; (void)cache_mb;
    LOG("http izvor '%s' jos nije implementiran", name);
    return NULL;
}
```

- [ ] **Step 6: Pusti test da prođe**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_nav tests/test_nav.c src/library.c src/config.c src/common.c \
   src/source.c src/source_usb.c src/source_http.c src/doc.c src/doc_archive.c src/stb_impl.c \
   $HOST_INC $HOST_LIBS -lm && /tmp/cr_t_nav
```

Očekivano: `test_nav OK`, bez prijava sanitizera.

- [ ] **Step 7: Commit**

```bash
git add src/library.c src/library.h src/source.h src/source_http.c tests/test_nav.c
git commit -m "library: navigacijski stek umjesto ravne rekurzivne liste"
```

---

### Task 5: `main.c` — stablo u UI-ju

Kraj Faze 1. Poslije ovog taska folder tree radi nad USB-om, bez ijedne mrežne zavisnosti.

**Files:**
- Modify: `src/main.c` (`app_t`, `reader_open`, `draw_browser`, `on_button`, `main`)

**Interfaces:**
- Consumes: `library_cur`, `library_enter`, `library_back`, `library_breadcrumb`, `library_init`, `state_load`, `state_save` (Task 4); `source_t::be->fetch` (Task 2)
- Produces: `static void reader_open(app_t *a, source_t *src, lib_entry_t *it, int want_page)` — `want_page < 0` znači „uzmi `it->last_page`". **Task 14 ovu funkciju zamjenjuje parom `reader_start`/`reader_finish`**; argument `want_page` preživljava zamjenu i na njemu počiva oporavak iz Taska 15.

- [ ] **Step 1: Izmijeni `app_t`**

U `src/main.c`, u `typedef struct { ... } app_t`, ukloni polja `int sel; int scroll;`
(sada žive u `lib_level_t`) i dodaj `cur_local`:

```c
typedef struct {
    ui_t      ui;
    library_t lib;
    screen_t  screen;

    /* browser */
    int rows_visible;

    /* reader */
    cache_t  *cache;
    int       page;
    int       n_pages;
    fitmode_t fit;
    float     zoom;
    float     pan_x, pan_y;
    uint32_t  hud_until;
    char      cur_path[LIB_PATH_MAX];    /* kljuc za state: putanja ili URL */
    char      cur_local[LIB_PATH_MAX];   /* ono sto je dobio cache_open */

    int running;
} app_t;
```

- [ ] **Step 2: Zamijeni `reader_open`**

```c
static void reader_open(app_t *a, source_t *src, lib_entry_t *it, int want_page)
{
    char local[LIB_PATH_MAX];

    reader_close(a);

    /* Za USB je ovo identitet; mrezni izvor ovdje vraca URL ili lokalnu kopiju. */
    if (src->be->fetch(src, it->path, local, sizeof local, NULL, NULL) != 0) {
        ERR("ne mogu da pripremim %s", it->path);
        return;
    }

    a->cache = cache_open(local, a->ui.r);
    if (!a->cache) {
        ERR("ne mogu da otvorim %s", local);
        return;
    }

    snprintf(a->cur_path,  sizeof a->cur_path,  "%s", it->path);
    snprintf(a->cur_local, sizeof a->cur_local, "%s", local);

    a->n_pages = cache_page_count(a->cache);

    int p = (want_page >= 0) ? want_page : it->last_page;
    a->page  = (p > 0 && p < a->n_pages) ? p : 0;
    a->fit   = FIT_SCREEN;
    a->zoom  = 1.0f;
    a->pan_x = a->pan_y = 0.0f;
    a->screen = SCREEN_READER;

    cache_focus(a->cache, a->page);
    hud_bump(a);
}
```

`reader_close` mijenja samo poziv `state_save`, koji sada prima `a->cur_path`:
on već koristi `a->cur_path`, pa ostaje nepromijenjen.

- [ ] **Step 3: Zamijeni `draw_browser`**

```c
static void draw_browser(app_t *a)
{
    ui_t        *ui = &a->ui;
    lib_level_t *lv = library_cur(&a->lib);
    char         buf[256];

    ui_fill_rect(ui, 0, 0, ui->screen_w, ui->screen_h, COL_BG);
    ui_text(ui, PAD, 44, 4, COL_ACCENT, "%s", APP_NAME);

    /* Breadcrumb se skracuje s LIJEVA - rep putanje je informativniji. */
    char bc[LIB_PATH_MAX];
    library_breadcrumb(&a->lib, bc, sizeof bc);

    int    max_bc = (ui->screen_w - 2 * PAD) / (8 * 2);
    size_t bclen  = strlen(bc);
    if (max_bc > 4 && bclen > (size_t)max_bc)
        ui_text(ui, PAD, 88, 2, COL_DIM, "...%s", bc + bclen - (size_t)max_bc + 3);
    else
        ui_text(ui, PAD, 88, 2, COL_DIM, "%s", bc);

    const char *footer = (a->lib.depth == 0)
        ? "Krst: otvori   Krug: izlaz   D-pad: kretanje"
        : "Krst: otvori   Krug: nazad   D-pad: kretanje";

    if (lv->err[0]) {
        ui_text(ui, PAD, LIST_TOP + 60, 3, COL_TEXT, "greska: %s", lv->err);
        ui_text(ui, PAD, ui->screen_h - 56, 2, COL_DIM, "%s", footer);
        return;
    }

    if (lv->count == 0) {
        ui_text(ui, PAD, LIST_TOP + 60, 3, COL_TEXT, "Prazno");
        ui_text(ui, PAD, LIST_TOP + 110, 2, COL_DIM,
                "Podrzano: cbz cbr cb7 cbt zip rar 7z pdf");
        ui_text(ui, PAD, ui->screen_h - 56, 2, COL_DIM, "%s", footer);
        return;
    }

    /* Skrol drzi selekciju unutar vidljivog opsega. */
    if (lv->sel < lv->scroll)
        lv->scroll = lv->sel;
    if (lv->sel >= lv->scroll + a->rows_visible)
        lv->scroll = lv->sel - a->rows_visible + 1;

    int max_chars = (ui->screen_w - 2 * PAD - 200) / (8 * 2);

    for (int i = 0; i < a->rows_visible; i++) {
        int idx = lv->scroll + i;
        if (idx >= lv->count)
            break;

        int          y  = LIST_TOP + i * ROW_H;
        lib_entry_t *it = &lv->entries[idx];
        int          on = (idx == lv->sel);

        ui_fill_rect(ui, PAD - 16, y - 6, ui->screen_w - 2 * PAD + 32, ROW_H - 4,
                     on ? COL_SEL : COL_PANEL);

        /* Folder se raspoznaje po kosoj crti i boji - font je ASCII, ikone otpadaju. */
        char label[LIB_TITLE_MAX + 2];
        snprintf(label, sizeof label, "%s%s", it->name, it->is_dir ? "/" : "");
        ui_ellipsize(buf, sizeof buf, label, max_chars);

        SDL_Color col = on ? COL_TEXT : (it->is_dir ? COL_ACCENT : COL_DIM);
        ui_text(ui, PAD, y, 2, col, "%s", buf);

        if (!it->is_dir && it->last_page > 0) {
            const char *badge = "nastavi";
            ui_text(ui, ui->screen_w - PAD - ui_text_width(2, badge), y, 2,
                    COL_ACCENT, "%s", badge);
        }
    }

    ui_text(ui, PAD, ui->screen_h - 56, 2, COL_DIM, "%s", footer);
}
```

- [ ] **Step 4: Zamijeni granu `SCREEN_BROWSER` u `on_button`**

```c
    if (a->screen == SCREEN_BROWSER) {
        lib_level_t *lv = library_cur(&a->lib);

        switch (b) {
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            if (lv->sel > 0) lv->sel--;
            break;
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            if (lv->sel < lv->count - 1) lv->sel++;
            break;
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            lv->sel = MAX(0, lv->sel - a->rows_visible);
            break;
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            lv->sel = MIN(lv->count - 1, lv->sel + a->rows_visible);
            break;
        case SDL_CONTROLLER_BUTTON_A:
            if (lv->count == 0)
                break;
            if (lv->entries[lv->sel].is_dir)
                library_enter(&a->lib, lv->sel);
            else
                reader_open(a, lv->src, &lv->entries[lv->sel], -1);
            break;
        case SDL_CONTROLLER_BUTTON_B:
            /* Krug izlazi iz nivoa; u korijenu gasi aplikaciju. */
            if (!library_back(&a->lib))
                a->running = 0;
            break;
        default:
            break;
        }
        return;
    }
```

- [ ] **Step 5: Izmijeni `main()`**

Zamijeni `library_scan(&a.lib); state_load(&a.lib);` sa:

```c
    library_init(&a.lib);
    state_load(&a.lib);
```

Redoslijed je bitan: `library_init` gradi korijen, a `state_load` puni tabelu stanja koju
`library_enter` koristi za `last_page`. Korijen nema fajlova, pa redoslijed ne pravi problem.

- [ ] **Step 6: Izgradi i ručno provjeri**

```bash
scripts/deps.sh && make host
mkdir -p /tmp/cr_demo/Serija/Sezona1
cp <bilo-koji>.cbz /tmp/cr_demo/Serija/Sezona1/
touch /tmp/cr_demo/Serija/readme.txt
CR_ROOT=/tmp/cr_demo ./comicreader
```

Provjeri redom:
1. Prvi ekran pokazuje **jedan red** — izvor `USB cr_demo` — i breadcrumb `/`.
2. Krst ulazi u izvor; breadcrumb postaje `USB cr_demo/`, footer se mijenja u `Krug: nazad`.
3. Folder `Serija/` je ispisan s kosom crtom i u plavoj boji; `readme.txt` se **ne** vidi.
4. Ulazak do `Sezona1/`, otvaranje stripa Krstom, `Krug` vraća u listu na isti red.
5. `Krug` do korijena, pa još jedan `Krug` — aplikacija se gasi.

- [ ] **Step 7: Commit**

```bash
git add src/main.c
git commit -m "main: stablo foldera u browseru umjesto ravne liste"
```

---

## FAZA 2 — Mrežno listanje

### Task 6: `dav_parse.c` — PROPFIND XML

**Files:**
- Create: `src/dav_parse.h`, `src/dav_parse.c`
- Test: `tests/test_dav.c`
- Uses: `tests/fixtures/propfind_stripovi.xml` (već u repou, snimljen sa stvarnog NAS-a)

**Interfaces:**
- Consumes: `url_decode` (Task 1), `lib_entry_t`, `source_filter_sort` (Task 2)
- Produces: `int dav_parse(const char *xml, size_t len, const char *base_url, const char *self_href, lib_entry_t **out, int *n)` — 0 ok, -1 neispravan XML. **Ne filtrira** — vraća sve osim self-unosa; filtriranje radi `source_filter_sort`.

> **Ključno:** `entry->path` mora zadržati **enkodirani** href (`Stripoteka%2041-50.cbr`), jer
> je to URL koji ide u HTTP zahtjev. Dekodira se samo `entry->name`, za prikaz.

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_dav.c`:

```c
/* test_dav.c - PROPFIND parser nad stvarnim odgovorom QNAP-a */
#include "dav_parse.h"
#include "source.h"
#include "common.h"
#include <assert.h>

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    assert(fread(b, 1, (size_t)n, f) == (size_t)n);
    b[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return b;
}

static lib_entry_t *find_by_name(lib_entry_t *e, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].name, name))
            return &e[i];
    return NULL;
}

int main(void)
{
    size_t len;
    char  *xml = slurp("tests/fixtures/propfind_stripovi.xml", &len);

    lib_entry_t *e = NULL;
    int          n = 0;

    assert(dav_parse(xml, len, "http://<ip-nas>:5000", "/STRIPOVI/", &e, &n) == 0);

    /* Fixture ima 7 <response>; self otpada, ostaje 6. Parser jos ne filtrira. */
    assert(n == 6);

    /* Folder */
    lib_entry_t *d = find_by_name(e, n, "STRIPOVI");
    assert(d && d->is_dir == 1);
    assert(!strcmp(d->path, "http://<ip-nas>:5000/STRIPOVI/STRIPOVI/"));

    /* Fajl: ime dekodirano, URL ostaje enkodiran */
    lib_entry_t *f = find_by_name(e, n, "Stripoteka 41-50");
    assert(f && f->is_dir == 0);
    assert(!strcmp(f->path, "http://<ip-nas>:5000/STRIPOVI/Stripoteka%2041-50.cbr"));
    assert(f->last_page == -1);

    /* Zarez i zagrada u imenu ne smiju razbiti parser */
    assert(find_by_name(e, n, "Stripoteka 0106 bd-3,74MB)") != NULL);

    /* Sistemski folder je jos tu - filtriranje je posao source_filter_sort */
    assert(find_by_name(e, n, "@Recycle") != NULL);

    /* Poslije filtriranja: @Recycle i index.php otpadaju, oba CBR-a ostaju,
     * folder je prvi. PDF zavisi od HAVE_MUPDF pa se broj namjerno ne tvrdi. */
    source_filter_sort(e, &n);
    assert(e[0].is_dir == 1);
    assert(!strcmp(e[0].name, "STRIPOVI"));
    assert(find_by_name(e, n, "@Recycle") == NULL);
    assert(find_by_name(e, n, "index") == NULL);
    assert(find_by_name(e, n, "Stripoteka 41-50") != NULL);
    assert(find_by_name(e, n, "Stripoteka 51-60") != NULL);

    free(e);
    free(xml);

    /* Smece na ulazu ne smije rusiti. */
    lib_entry_t *e2 = NULL;
    int          n2 = 0;
    assert(dav_parse("<nije-xml", 9, "http://x", "/", &e2, &n2) == -1);

    printf("test_dav OK\n");
    return 0;
}
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_dav tests/test_dav.c src/dav_parse.c src/source.c src/common.c \
   src/doc.c src/doc_archive.c src/stb_impl.c \
   $HOST_INC $HOST_LIBS -lm && /tmp/cr_t_dav
```

Očekivano: FAIL — `src/dav_parse.h: No such file or directory`.

- [ ] **Step 3: Implementiraj `src/dav_parse.h`**

```c
/* dav_parse.h - WebDAV PROPFIND multistatus -> lib_entry_t[] */
#ifndef DAV_PARSE_H
#define DAV_PARSE_H

#include "source.h"

/* base_url: shema i host bez zavrsne kose crte, npr. "http://<ip-nas>:5000"
 * self_href: trazena putanja, npr. "/STRIPOVI/" - taj unos se preskace
 *
 * Vraca 0 i alocira niz (pozivalac ga oslobadja sa free()), -1 na neispravan XML.
 * NE filtrira i NE sortira - to radi source_filter_sort(). */
int dav_parse(const char *xml, size_t len, const char *base_url,
              const char *self_href, lib_entry_t **out, int *n);

#endif /* DAV_PARSE_H */
```

- [ ] **Step 4: Implementiraj `src/dav_parse.c`**

```c
/* dav_parse.c
 *
 * Koristi se libxml2, a ne rucni skener, zbog jedne stvari: namespace
 * prefiksi u odgovoru nisu ni stabilni ni jednaki (vidjeno D:, lp1:, lp2:
 * u istom odgovoru QNAP-a). libxml2 u node->name drzi LOKALNO ime, pa
 * poredjenje po prefiksu uopste ne treba.
 */
#include "dav_parse.h"
#include "common.h"

#include <libxml/parser.h>
#include <libxml/tree.h>

static int is_elem(xmlNode *n, const char *local)
{
    return n->type == XML_ELEMENT_NODE && !xmlStrcmp(n->name, (const xmlChar *)local);
}

static xmlNode *child_elem(xmlNode *p, const char *local)
{
    for (xmlNode *c = p->children; c; c = c->next)
        if (is_elem(c, local))
            return c;
    return NULL;
}

/* collection je ugnijezdjen: propstat > prop > resourcetype > collection */
static int has_elem_deep(xmlNode *p, const char *local)
{
    for (xmlNode *c = p->children; c; c = c->next) {
        if (c->type != XML_ELEMENT_NODE)
            continue;
        if (!xmlStrcmp(c->name, (const xmlChar *)local))
            return 1;
        if (has_elem_deep(c, local))
            return 1;
    }
    return 0;
}

/* Uklanja zavrsnu kosu crtu osim ako je putanja samo "/". */
static void strip_slash(char *s)
{
    size_t n = strlen(s);
    if (n > 1 && s[n - 1] == '/')
        s[n - 1] = '\0';
}

int dav_parse(const char *xml, size_t len, const char *base_url,
              const char *self_href, lib_entry_t **out, int *n)
{
    *out = NULL;
    *n   = 0;

    xmlDoc *doc = xmlReadMemory(xml, (int)len, NULL, NULL,
                                XML_PARSE_NOBLANKS | XML_PARSE_NONET |
                                XML_PARSE_NOERROR  | XML_PARSE_NOWARNING);
    if (!doc)
        return -1;

    xmlNode *root = xmlDocGetRootElement(doc);
    if (!root || !is_elem(root, "multistatus")) {
        xmlFreeDoc(doc);
        return -1;
    }

    int cap = 32, cnt = 0;
    lib_entry_t *arr = calloc((size_t)cap, sizeof *arr);
    if (!arr) {
        xmlFreeDoc(doc);
        return -1;
    }

    char self[LIB_PATH_MAX];
    snprintf(self, sizeof self, "%s", self_href);
    strip_slash(self);

    for (xmlNode *r = root->children; r; r = r->next) {
        if (!is_elem(r, "response"))
            continue;

        xmlNode *h = child_elem(r, "href");
        if (!h)
            continue;

        xmlChar *raw = xmlNodeGetContent(h);
        if (!raw)
            continue;

        char href[LIB_PATH_MAX];
        snprintf(href, sizeof href, "%s", (const char *)raw);
        xmlFree(raw);

        int is_dir = has_elem_deep(r, "collection");

        /* Self-unos: ista putanja kao trazena. */
        char cmp[LIB_PATH_MAX];
        snprintf(cmp, sizeof cmp, "%s", href);
        strip_slash(cmp);
        if (!strcmp(cmp, self))
            continue;

        if (cnt == cap) {
            int          ncap = cap * 2;
            lib_entry_t *na   = realloc(arr, (size_t)ncap * sizeof *na);
            if (!na)
                break;
            arr = na;
            memset(arr + cap, 0, (size_t)(ncap - cap) * sizeof *arr);
            cap = ncap;
        }

        lib_entry_t *e = &arr[cnt];
        memset(e, 0, sizeof *e);
        e->is_dir    = is_dir;
        e->last_page = -1;

        /* URL zadrzava enkodiranje - to je ono sto ide u HTTP zahtjev. */
        if (is_url(href))
            snprintf(e->path, sizeof e->path, "%s", href);
        else
            snprintf(e->path, sizeof e->path, "%s%s", base_url, href);

        /* Ime se dekodira, samo za prikaz. */
        char dec[LIB_PATH_MAX];
        url_decode(dec, sizeof dec, cmp);
        snprintf(e->name, sizeof e->name, "%s", path_base(dec));

        if (!is_dir) {
            char *dot = strrchr(e->name, '.');
            if (dot)
                *dot = '\0';
        }

        if (e->name[0])
            cnt++;
    }

    xmlFreeDoc(doc);

    *out = arr;
    *n   = cnt;
    return 0;
}
```

- [ ] **Step 5: Pusti test da prođe**

Ista komanda kao Step 2. Očekivano: `test_dav OK`, bez prijava sanitizera.

- [ ] **Step 6: Commit**

```bash
git add src/dav_parse.c src/dav_parse.h tests/test_dav.c
git commit -m "dav: parser PROPFIND odgovora preko libxml2"
```

---

### Task 7: `html_parse.c` — autoindex fallback

**Files:**
- Create: `src/html_parse.h`, `src/html_parse.c`
- Test: `tests/test_html.c`, `tests/fixtures/autoindex_nginx.html`

**Interfaces:**
- Consumes: `url_decode` (Task 1), `lib_entry_t`
- Produces: `int html_parse(const char *html, size_t len, const char *base_url, const char *self_href, lib_entry_t **out, int *n)` — isti ugovor kao `dav_parse`: bez filtriranja i bez sortiranja.

- [ ] **Step 1: Napravi fixture**

Kreiraj `tests/fixtures/autoindex_nginx.html`:

```html
<html>
<head><title>Index of /STRIPOVI/</title></head>
<body>
<h1>Index of /STRIPOVI/</h1><hr><pre><a href="../">../</a>
<a href="?C=N;O=D">Name</a>
<a href="Serija/">Serija/</a>                      01-Sep-2026 10:00   -
<a href="Stripoteka%2041-50.cbr">Stripoteka 41-50.cbr</a>  01-Sep-2026 10:00  782458507
<a href="/STRIPOVI/">Parent Directory</a>
<a href="index.php">index.php</a>                  01-Sep-2026 10:00   12
</pre><hr></body>
</html>
```

- [ ] **Step 2: Napiši test koji pada**

Kreiraj `tests/test_html.c`:

```c
/* test_html.c - autoindex parser */
#include "html_parse.h"
#include "source.h"
#include "common.h"
#include <assert.h>

static char *slurp(const char *path, size_t *len)
{
    FILE *f = fopen(path, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *b = malloc((size_t)n + 1);
    assert(fread(b, 1, (size_t)n, f) == (size_t)n);
    b[n] = '\0';
    fclose(f);
    *len = (size_t)n;
    return b;
}

static lib_entry_t *by_name(lib_entry_t *e, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].name, name))
            return &e[i];
    return NULL;
}

int main(void)
{
    size_t len;
    char  *html = slurp("tests/fixtures/autoindex_nginx.html", &len);

    lib_entry_t *e = NULL;
    int          n = 0;

    assert(html_parse(html, len, "http://<ip-nas>:8080", "/STRIPOVI/", &e, &n) == 0);

    /* Prolaze samo relativni linkovi: Serija/, CBR, index.php.
     * Otpadaju: ../ (roditelj), ?C=N;O=D (sortiranje), /STRIPOVI/ (apsolutni). */
    assert(n == 3);

    lib_entry_t *d = by_name(e, n, "Serija");
    assert(d && d->is_dir == 1);
    assert(!strcmp(d->path, "http://<ip-nas>:8080/STRIPOVI/Serija/"));

    lib_entry_t *f = by_name(e, n, "Stripoteka 41-50");
    assert(f && f->is_dir == 0);
    assert(!strcmp(f->path, "http://<ip-nas>:8080/STRIPOVI/Stripoteka%2041-50.cbr"));

    assert(by_name(e, n, "index") != NULL);   /* filtriranje je posao source_filter_sort */

    source_filter_sort(e, &n);
    assert(by_name(e, n, "index") == NULL);
    assert(by_name(e, n, "Serija") != NULL);
    assert(e[0].is_dir == 1);

    free(e);
    free(html);

    /* Prazan ulaz nije greska, samo nula unosa. */
    lib_entry_t *e2 = NULL;
    int          n2 = 0;
    assert(html_parse("", 0, "http://x", "/", &e2, &n2) == 0);
    assert(n2 == 0);
    free(e2);

    printf("test_html OK\n");
    return 0;
}
```

- [ ] **Step 3: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_html tests/test_html.c src/html_parse.c src/source.c src/common.c \
   src/doc.c src/doc_archive.c src/stb_impl.c \
   $HOST_INC $HOST_LIBS -lm && /tmp/cr_t_html
```

Očekivano: FAIL — `src/html_parse.h: No such file or directory`.

- [ ] **Step 4: Implementiraj `src/html_parse.h`**

```c
/* html_parse.h - nginx/Apache autoindex -> lib_entry_t[] */
#ifndef HTML_PARSE_H
#define HTML_PARSE_H

#include "source.h"

/* Isti ugovor kao dav_parse: ne filtrira i ne sortira.
 * Vraca 0 uvijek osim na gresku alokacije. */
int html_parse(const char *html, size_t len, const char *base_url,
               const char *self_href, lib_entry_t **out, int *n);

#endif /* HTML_PARSE_H */
```

- [ ] **Step 5: Implementiraj `src/html_parse.c`**

```c
/* html_parse.c
 *
 * Namjerno NIJE HTML parser nego skener href atributa. Autoindex stranice
 * su generisane i predvidive, a pun parser bi bio treca zavisnost za
 * posao koji staje u sto linija.
 */
#include "html_parse.h"
#include "common.h"

#include <ctype.h>

/* Linkovi koje autoindex generise a nisu sadrzaj foldera. */
static int href_skip(const char *h)
{
    if (!*h)                      return 1;
    if (*h == '?' || *h == '#')   return 1;   /* sortiranje kolona */
    if (*h == '/')                return 1;   /* apsolutni "Parent Directory" */
    if (!strncmp(h, "..", 2))     return 1;
    if (strstr(h, "://"))         return 1;   /* link van ovog foldera */
    return 0;
}

int html_parse(const char *html, size_t len, const char *base_url,
               const char *self_href, lib_entry_t **out, int *n)
{
    *out = NULL;
    *n   = 0;

    int          cap = 32, cnt = 0;
    lib_entry_t *arr = calloc((size_t)cap, sizeof *arr);
    if (!arr)
        return -1;

    const char *p   = html;
    const char *end = html + len;

    while (p < end) {
        /* Trazi href=" ili href=' */
        const char *h = strstr(p, "href=");
        if (!h || h >= end)
            break;

        h += 5;
        char quote = *h;
        if (quote != '"' && quote != '\'') {
            p = h;
            continue;
        }
        h++;

        const char *e2 = strchr(h, quote);
        if (!e2 || e2 >= end)
            break;

        size_t hl = (size_t)(e2 - h);
        if (hl >= LIB_PATH_MAX) {
            p = e2 + 1;
            continue;
        }

        char href[LIB_PATH_MAX];
        memcpy(href, h, hl);
        href[hl] = '\0';
        p = e2 + 1;

        if (href_skip(href))
            continue;

        int is_dir = (hl > 0 && href[hl - 1] == '/');

        if (cnt == cap) {
            int          ncap = cap * 2;
            lib_entry_t *na   = realloc(arr, (size_t)ncap * sizeof *na);
            if (!na)
                break;
            arr = na;
            memset(arr + cap, 0, (size_t)(ncap - cap) * sizeof *arr);
            cap = ncap;
        }

        lib_entry_t *ent = &arr[cnt];
        memset(ent, 0, sizeof *ent);
        ent->is_dir    = is_dir;
        ent->last_page = -1;

        /* URL ostaje enkodiran; self_href je vec enkodiran put foldera. */
        snprintf(ent->path, sizeof ent->path, "%s%s%s", base_url, self_href, href);

        char dec[LIB_PATH_MAX];
        url_decode(dec, sizeof dec, href);

        size_t dl = strlen(dec);
        if (dl && dec[dl - 1] == '/')
            dec[dl - 1] = '\0';

        snprintf(ent->name, sizeof ent->name, "%s", path_base(dec));

        if (!is_dir) {
            char *dot = strrchr(ent->name, '.');
            if (dot)
                *dot = '\0';
        }

        if (ent->name[0])
            cnt++;
    }

    *out = arr;
    *n   = cnt;
    return 0;
}
```

- [ ] **Step 6: Pusti test da prođe**

Ista komanda kao Step 3. Očekivano: `test_html OK`.

- [ ] **Step 7: Commit**

```bash
git add src/html_parse.c src/html_parse.h tests/test_html.c tests/fixtures/autoindex_nginx.html
git commit -m "html: skener href atributa za autoindex listinge"
```

---

### Task 8: `source_http.c` — mrežno listanje

**Files:**
- Rewrite: `src/source_http.c` (zamjenjuje stub iz Taska 4)
- Create: `tests/http_server.py`, `tests/test_http_list.c`

**Interfaces:**
- Consumes: `dav_parse` (Task 6), `html_parse` (Task 7), `source_filter_sort` (Task 2)
- Produces:
  - `source_t *source_http_new(const char *name, const char *url, const char *type, const char *user, const char *pass, int cache_mb)` — puna implementacija
  - `fetch` je i dalje privremen: vraća `-1` uz `LOG`. Pravi dolazi u Tasku 13.

- [ ] **Step 1: Napravi test server**

Kreiraj `tests/http_server.py` (koristi ga i Task 9 i Task 11):

```python
#!/usr/bin/env python3
"""Test server za mrezni izvor.

Podrzava:
  - PROPFIND Depth:1 -> 207 multistatus (ili 405 ako SRV_NO_DAV=1)
  - GET na folder    -> nginx-stil autoindex
  - GET na fajl      -> puni sadrzaj ili 206 Partial Content za Range
  - ubacivanje gresaka: SRV_FAIL_EVERY=N obara svaki N-ti zahtjev

Okruzenje:
  SRV_ROOT      korijen koji se servira (default: tekuci direktorij)
  SRV_PORT      port (default 8099)
  SRV_NO_DAV    "1" -> PROPFIND vraca 405, tjera fallback na autoindex
  SRV_FAIL_EVERY  N -> svaki N-ti zahtjev prekida vezu bez odgovora
  SRV_NO_RANGE  "1" -> ignorise Range i vraca 200 sa cijelim fajlom
"""
import os
import sys
import urllib.parse
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT       = os.path.abspath(os.environ.get("SRV_ROOT", "."))
PORT       = int(os.environ.get("SRV_PORT", "8099"))
NO_DAV     = os.environ.get("SRV_NO_DAV") == "1"
NO_RANGE   = os.environ.get("SRV_NO_RANGE") == "1"
FAIL_EVERY = int(os.environ.get("SRV_FAIL_EVERY", "0"))

_count = [0]


def local_path(url_path):
    rel = urllib.parse.unquote(url_path).lstrip("/")
    p = os.path.abspath(os.path.join(ROOT, rel))
    if not p.startswith(ROOT):
        return None
    return p


class Handler(BaseHTTPRequestHandler):
    protocol_version = "HTTP/1.1"

    def log_message(self, *a):
        pass

    def _fail_if_due(self):
        if FAIL_EVERY <= 0:
            return False
        _count[0] += 1
        if _count[0] % FAIL_EVERY == 0:
            self.close_connection = True
            try:
                self.wfile.close()
            except Exception:
                pass
            return True
        return False

    def do_PROPFIND(self):
        if self._fail_if_due():
            return
        if NO_DAV:
            self.send_response(405)
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        p = local_path(self.path)
        if not p or not os.path.isdir(p):
            self.send_error(404)
            return

        base = self.path if self.path.endswith("/") else self.path + "/"
        parts = ['<?xml version="1.0" encoding="utf-8"?>',
                 '<D:multistatus xmlns:D="DAV:">']

        def resp(href, is_dir, size):
            rt = "<D:collection/>" if is_dir else ""
            cl = "" if is_dir else f"<D:getcontentlength>{size}</D:getcontentlength>"
            return (f"<D:response><D:href>{href}</D:href><D:propstat><D:prop>"
                    f"<D:resourcetype>{rt}</D:resourcetype>{cl}"
                    f"</D:prop><D:status>HTTP/1.1 200 OK</D:status>"
                    f"</D:propstat></D:response>")

        parts.append(resp(base, True, 0))
        for name in sorted(os.listdir(p)):
            full = os.path.join(p, name)
            quoted = urllib.parse.quote(name)
            if os.path.isdir(full):
                parts.append(resp(base + quoted + "/", True, 0))
            else:
                parts.append(resp(base + quoted, False, os.path.getsize(full)))
        parts.append("</D:multistatus>")

        body = "".join(parts).encode()
        self.send_response(207)
        self.send_header("Content-Type", 'text/xml; charset="utf-8"')
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_HEAD(self):
        self._serve(head_only=True)

    def do_GET(self):
        self._serve(head_only=False)

    def _serve(self, head_only):
        if self._fail_if_due():
            return

        p = local_path(self.path)
        if not p or not os.path.exists(p):
            self.send_error(404)
            return

        if os.path.isdir(p):
            base = self.path if self.path.endswith("/") else self.path + "/"
            rows = ['<html><body><pre><a href="../">../</a>']
            for name in sorted(os.listdir(p)):
                q = urllib.parse.quote(name)
                suffix = "/" if os.path.isdir(os.path.join(p, name)) else ""
                rows.append(f'<a href="{q}{suffix}">{name}{suffix}</a>')
            rows.append("</pre></body></html>")
            body = "\n".join(rows).encode()
            self.send_response(200)
            self.send_header("Content-Type", "text/html")
            self.send_header("Content-Length", str(len(body)))
            self.end_headers()
            if not head_only:
                self.wfile.write(body)
            return

        size = os.path.getsize(p)
        rng = self.headers.get("Range")
        start, end = 0, size - 1
        partial = False

        if rng and not NO_RANGE and rng.startswith("bytes="):
            spec = rng[6:].split(",")[0]
            a, _, b = spec.partition("-")
            if a:
                start = int(a)
                end = int(b) if b else size - 1
            else:
                start = max(0, size - int(b))
            end = min(end, size - 1)
            partial = True

        if start >= size:
            self.send_response(416)
            self.send_header("Content-Range", f"bytes */{size}")
            self.send_header("Content-Length", "0")
            self.end_headers()
            return

        length = end - start + 1
        self.send_response(206 if partial else 200)
        self.send_header("Accept-Ranges", "bytes")
        self.send_header("Content-Type", "application/octet-stream")
        self.send_header("Content-Length", str(length))
        if partial:
            self.send_header("Content-Range", f"bytes {start}-{end}/{size}")
        self.end_headers()

        if head_only:
            return

        with open(p, "rb") as f:
            f.seek(start)
            remaining = length
            while remaining > 0:
                chunk = f.read(min(65536, remaining))
                if not chunk:
                    break
                self.wfile.write(chunk)
                remaining -= len(chunk)


if __name__ == "__main__":
    srv = ThreadingHTTPServer(("127.0.0.1", PORT), Handler)
    sys.stderr.write(f"http_server na 127.0.0.1:{PORT}, root={ROOT}\n")
    sys.stderr.flush()
    srv.serve_forever()
```

```bash
chmod +x tests/http_server.py
```

- [ ] **Step 2: Napiši test koji pada**

Kreiraj `tests/test_http_list.c`:

```c
/* test_http_list.c - listanje preko WebDAV-a i preko autoindex fallbacka.
 * Server se pokrece iz test skripte, adresa dolazi kroz SRV_URL. */
#include "source.h"
#include "common.h"
#include <assert.h>

static lib_entry_t *by_name(lib_entry_t *e, int n, const char *name)
{
    for (int i = 0; i < n; i++)
        if (!strcmp(e[i].name, name))
            return &e[i];
    return NULL;
}

int main(void)
{
    const char *url = getenv("SRV_URL");
    assert(url && "SRV_URL mora biti postavljen");

    source_t *s = source_http_new("test", url, "auto", NULL, NULL, 512);
    assert(s);

    lib_entry_t *e = NULL;
    int          n = 0;
    assert(s->be->list(s, s->root, &e, &n) == 0);

    /* Server servira folder sa: Serija/ (folder) i a.cbz (fajl).
     * readme.txt otpada na filtriranju. */
    assert(n == 2);
    assert(e[0].is_dir == 1);
    assert(!strcmp(e[0].name, "Serija"));
    assert(!strcmp(e[1].name, "a"));
    assert(e[1].is_dir == 0);

    /* Ulazak u podfolder mora raditi istim pozivom. */
    lib_entry_t *sub = NULL;
    int          ns  = 0;
    assert(s->be->list(s, e[0].path, &sub, &ns) == 0);
    assert(ns == 1);
    assert(!strcmp(sub[0].name, "b"));

    free(sub);
    free(e);
    source_free(s);

    printf("test_http_list OK (%s)\n", getenv("SRV_NO_DAV") ? "autoindex" : "webdav");
    return 0;
}
```

I skriptu koja diže server i pušta test u oba režima, `tests/run_http_list.sh`:

```sh
#!/bin/sh
# Pusta test_http_list u oba rezima: WebDAV i autoindex fallback.
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

mkdir -p "$TMP/Serija"
printf 'x' > "$TMP/a.cbz"
printf 'x' > "$TMP/readme.txt"
printf 'x' > "$TMP/Serija/b.cbz"

PORT=8099

for mode in webdav autoindex; do
    if [ "$mode" = autoindex ]; then
        SRV_NO_DAV=1; export SRV_NO_DAV
    else
        unset SRV_NO_DAV
    fi

    SRV_ROOT="$TMP" SRV_PORT="$PORT" python3 tests/http_server.py &
    PID=$!
    sleep 1

    SRV_URL="http://127.0.0.1:$PORT/" "$BIN"

    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    PID=
    PORT=$((PORT + 1))
done

echo "oba rezima prosla"
```

```bash
chmod +x tests/run_http_list.sh
```

- [ ] **Step 3: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_hlist tests/test_http_list.c src/source_http.c src/dav_parse.c \
   src/html_parse.c src/source.c src/common.c src/doc.c src/doc_archive.c src/stb_impl.c \
   $HOST_INC $HOST_LIBS -lm
tests/run_http_list.sh /tmp/cr_t_hlist
```

Očekivano: FAIL — stub `source_http_new` vraća `NULL`, pa puca prvi `assert(s)`.

- [ ] **Step 4: Implementiraj `src/source_http.c`**

```c
/* source_http.c - mrezni izvor preko WebDAV-a ili autoindex stranice
 *
 * Listanje je sinhrono i zove se iz glavne petlje. Na LAN-u je PROPFIND
 * jednog foldera desetine milisekundi, a gornja granica je CONNECTTIMEOUT.
 */
#include "source.h"
#include "dav_parse.h"
#include "html_parse.h"
#include "common.h"

#include <curl/curl.h>

typedef struct {
    char url[LIB_PATH_MAX];      /* korijen izvora, uvijek sa zavrsnom / */
    char base[LIB_PATH_MAX];     /* shema+host+port, bez zavrsne / */
    char user[64];
    char pass[64];
    int  use_dav;                /* 1 = PROPFIND, 0 = autoindex */
    int  probed;                 /* 1 kad je auto-detekcija odradjena */
    int  cache_mb;               /* gornja granica kesa, iz configa */
} http_priv_t;

typedef struct {
    char  *buf;
    size_t len;
} membuf_t;

static size_t sink(void *data, size_t sz, size_t nm, void *ud)
{
    membuf_t *m = ud;
    size_t    n = sz * nm;

    char *nb = realloc(m->buf, m->len + n + 1);
    if (!nb)
        return 0;

    m->buf = nb;
    memcpy(m->buf + m->len, data, n);
    m->len += n;
    m->buf[m->len] = '\0';
    return n;
}

/* method NULL = obican GET. Vraca HTTP status ili -1 na gresku transporta. */
static long http_body(http_priv_t *p, const char *url, const char *method,
                      const char *depth, membuf_t *out)
{
    CURL *c = curl_easy_init();
    if (!c)
        return -1;

    struct curl_slist *hdr = NULL;
    if (depth)
        hdr = curl_slist_append(hdr, "Depth: 1");

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);

    if (method)
        curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (hdr)
        curl_easy_setopt(c, CURLOPT_HTTPHEADER, hdr);

    if (p->user[0]) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, p->user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, p->pass);
    }

    CURLcode rc = curl_easy_perform(c);
    long     code = -1;
    if (rc == CURLE_OK)
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    else
        LOG("http: %s -> %s", method ? method : "GET", curl_easy_strerror(rc));

    if (hdr)
        curl_slist_free_all(hdr);
    curl_easy_cleanup(c);
    return code;
}

/* Iz punog URL-a vadi putanju (dio od trece kose crte). */
static void url_path_of(const char *url, char *out, size_t len)
{
    const char *p = strstr(url, "://");
    p = p ? strchr(p + 3, '/') : NULL;
    snprintf(out, len, "%s", p ? p : "/");
}

static void url_base_of(const char *url, char *out, size_t len)
{
    const char *p = strstr(url, "://");
    const char *slash = p ? strchr(p + 3, '/') : NULL;

    if (!slash) {
        snprintf(out, len, "%s", url);
        return;
    }
    size_t n = (size_t)(slash - url);
    if (n >= len)
        n = len - 1;
    memcpy(out, url, n);
    out[n] = '\0';
}

static int http_list(source_t *s, const char *path, lib_entry_t **out, int *n)
{
    http_priv_t *p = s->priv;
    membuf_t     mb = { NULL, 0 };

    char url[LIB_PATH_MAX];
    snprintf(url, sizeof url, "%s", path);

    /* Folder mora imati zavrsnu kosu crtu, inace Apache salje redirect. */
    size_t ul = strlen(url);
    if (ul && url[ul - 1] != '/' && ul + 1 < sizeof url) {
        url[ul]     = '/';
        url[ul + 1] = '\0';
    }

    char self[LIB_PATH_MAX];
    url_path_of(url, self, sizeof self);

    s->err[0] = '\0';
    *out = NULL;
    *n   = 0;

    /* auto: prvo PROPFIND, pa fallback na autoindex. Odluka se pamti. */
    if (p->use_dav) {
        long code = http_body(p, url, "PROPFIND", "1", &mb);

        if (code == 207) {
            p->probed = 1;
            int r = dav_parse(mb.buf ? mb.buf : "", mb.len,
                              p->base, self, out, n);
            free(mb.buf);
            if (r != 0) {
                snprintf(s->err, sizeof s->err, "neispravan PROPFIND odgovor");
                return -1;
            }
            source_filter_sort(*out, n);
            return 0;
        }

        free(mb.buf);
        mb.buf = NULL;
        mb.len = 0;

        if (code == 401 || code == 403) {
            snprintf(s->err, sizeof s->err,
                     "%ld - provjeri user/pass u .ps5cr.conf", code);
            return -1;
        }
        if (code == 404) {
            snprintf(s->err, sizeof s->err, "404 - putanja ne postoji na serveru");
            return -1;
        }
        if (code == 405 || code == 501) {
            LOG("http: server ne zna PROPFIND (%ld), prelazim na autoindex", code);
            p->use_dav = 0;
            p->probed  = 1;
        } else if (code < 0) {
            snprintf(s->err, sizeof s->err, "server nije dostupan");
            return -1;
        } else {
            LOG("http: neocekivan PROPFIND status %ld, probam autoindex", code);
            p->use_dav = 0;
            p->probed  = 1;
        }
    }

    long code = http_body(p, url, NULL, NULL, &mb);
    if (code != 200) {
        free(mb.buf);
        if (code == 401 || code == 403)
            snprintf(s->err, sizeof s->err,
                     "%ld - provjeri user/pass u .ps5cr.conf", code);
        else if (code < 0)
            snprintf(s->err, sizeof s->err, "server nije dostupan");
        else
            snprintf(s->err, sizeof s->err, "HTTP %ld", code);
        return -1;
    }

    int r = html_parse(mb.buf ? mb.buf : "", mb.len, p->base, self, out, n);
    free(mb.buf);
    if (r != 0) {
        snprintf(s->err, sizeof s->err, "ne mogu da procitam listing");
        return -1;
    }

    source_filter_sort(*out, n);
    return 0;
}

/* Pravi fetch dolazi u Tasku 13. */
static int http_fetch(source_t *s, const char *path, char *local, size_t len,
                      src_progress_fn cb, void *ud)
{
    (void)path; (void)local; (void)len; (void)cb; (void)ud;
    snprintf(s->err, sizeof s->err, "citanje preko mreze jos nije implementirano");
    return -1;
}

static void http_close(source_t *s)
{
    free(s->priv);
    s->priv = NULL;
}

static const source_backend_t http_be = {
    "http", http_list, http_fetch, http_close
};

source_t *source_http_new(const char *name, const char *url, const char *type,
                          const char *user, const char *pass, int cache_mb)
{
    if (!url || !is_url(url)) {
        ERR("izvor '%s': url mora pocinjati sa http://", name ? name : "?");
        return NULL;
    }

    source_t    *s = calloc(1, sizeof *s);
    http_priv_t *p = calloc(1, sizeof *p);
    if (!s || !p) {
        free(s);
        free(p);
        return NULL;
    }

    snprintf(p->url, sizeof p->url, "%s", url);
    size_t ul = strlen(p->url);
    if (ul && p->url[ul - 1] != '/' && ul + 1 < sizeof p->url) {
        p->url[ul]     = '/';
        p->url[ul + 1] = '\0';
    }
    url_base_of(p->url, p->base, sizeof p->base);

    if (user) snprintf(p->user, sizeof p->user, "%s", user);
    if (pass) snprintf(p->pass, sizeof p->pass, "%s", pass);
    p->cache_mb = cache_mb > 0 ? cache_mb : 4096;

    /* type: auto i webdav krecu od PROPFIND-a, autoindex ga preskace. */
    p->use_dav = !(type && !strcmp(type, "autoindex"));
    p->probed  = (type && strcmp(type, "auto")) ? 1 : 0;

    s->be   = &http_be;
    s->priv = p;
    snprintf(s->name, sizeof s->name, "%s", name && *name ? name : "mreza");
    snprintf(s->root, sizeof s->root, "%s", p->url);

    return s;
}
```

- [ ] **Step 5: Inicijalizuj curl u `main.c`**

`curl_global_init` nije thread-safe i mora ići prije pokretanja ijedne niti.
U `src/main.c`, dodaj `#include <curl/curl.h>` i odmah poslije `SDL_Init(SDL_INIT_VIDEO)`:

```c
    /* Prije bilo koje niti - curl_global_init nije thread-safe. */
    curl_global_init(CURL_GLOBAL_DEFAULT);
```

a prije `SDL_Quit()` na kraju `main()`:

```c
    curl_global_cleanup();
```

- [ ] **Step 6: Pusti test da prođe**

```bash
tests/run_http_list.sh /tmp/cr_t_hlist
```

Očekivano:
```
test_http_list OK (webdav)
test_http_list OK (autoindex)
oba rezima prosla
```

- [ ] **Step 7: Commit**

```bash
git add src/source_http.c src/main.c tests/test_http_list.c tests/http_server.py tests/run_http_list.sh
git commit -m "source_http: listanje preko PROPFIND-a uz fallback na autoindex"
```

---

## FAZA 3 — Čitanje preko mreže

### Task 9: `vfs_http.c` — Range callback-ovi i adaptivni chunk

**Files:**
- Create: `src/vfs_http.h`, `src/vfs_http.c`
- Test: `tests/test_vfs_http.c`, `tests/run_vfs.sh`

**Interfaces:**
- Consumes: `tests/http_server.py` (Task 8)
- Produces:
  - `vfs_http_t *vfs_http_new(const char *url)`, `void vfs_http_free(vfs_http_t *v)`
  - libarchive callback-ovi `vh_open`, `vh_read`, `vh_skip`, `vh_seek`, `vh_close`
  - `long vfs_http_requests(const vfs_http_t *v)` — brojač zahtjeva, koriste ga testovi u Tasku 10
  - `int64_t vfs_http_size(const vfs_http_t *v)`

**Pravila iz specifikacije §7.2, mjerena:** chunk kreće od 4 KB, množi se sa 4 na svakom
promašaju do najviše 1 MB, i vraća se na 4 KB nakon svakog `skip` ili `seek`. `skip` i `seek`
**ne šalju nijedan zahtjev**.

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_vfs_http.c`:

```c
/* test_vfs_http.c - Range callback-ovi bez libarchive, direktno */
#include "vfs_http.h"
#include "common.h"
#include <assert.h>

int main(void)
{
    const char *url = getenv("SRV_FILE_URL");
    const char *loc = getenv("SRV_FILE_LOCAL");
    assert(url && loc);

    /* Lokalni original, za poredjenje bajt po bajt. */
    FILE *f = fopen(loc, "rb");
    assert(f);
    fseek(f, 0, SEEK_END);
    long fsz = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *ref = malloc((size_t)fsz);
    assert(fread(ref, 1, (size_t)fsz, f) == (size_t)fsz);
    fclose(f);

    vfs_http_t *v = vfs_http_new(url);
    assert(v);
    assert(vh_open(NULL, v) == ARCHIVE_OK);
    assert(vfs_http_size(v) == (int64_t)fsz);

    /* 1. Sekvencijalno citanje mora dati identican sadrzaj. */
    long off = 0;
    for (;;) {
        const void *b = NULL;
        la_ssize_t  n = vh_read(NULL, v, &b);
        assert(n >= 0);
        if (n == 0)
            break;
        assert(off + n <= fsz);
        assert(memcmp(ref + off, b, (size_t)n) == 0);
        off += n;
    }
    assert(off == fsz);

    /* 2. Chunk raste x4 od 4 KB do 1 MB: 4K, 16K, 64K, 256K, 1M, 1M, ...
     *    Za fajl od 2 MB to je 6 zahtjeva, ne 512. */
    long req_seq = vfs_http_requests(v);
    assert(req_seq <= 8);

    /* 3. seek vraca chunk na pocetnih 4 KB i ne salje zahtjev. */
    long before = vfs_http_requests(v);
    assert(vh_seek(NULL, v, 0, SEEK_SET) == 0);
    assert(vfs_http_requests(v) == before);      /* seek ne salje nista */

    const void *b = NULL;
    la_ssize_t  n = vh_read(NULL, v, &b);
    assert(n == 4096);                            /* resetovan na minimum */
    assert(memcmp(ref, b, 4096) == 0);

    /* 4. skip pomjera poziciju bez zahtjeva. */
    before = vfs_http_requests(v);
    assert(vh_skip(NULL, v, 1000) == 1000);
    assert(vfs_http_requests(v) == before);

    n = vh_read(NULL, v, &b);
    assert(n == 4096);                            /* skip takodje resetuje chunk */
    assert(memcmp(ref + 4096 + 1000, b, 4096) == 0);

    /* 5. Citanje iza kraja fajla je EOF, ne greska. */
    assert(vh_seek(NULL, v, 0, SEEK_END) == (la_int64_t)fsz);
    n = vh_read(NULL, v, &b);
    assert(n == 0);

    assert(vh_close(NULL, v) == ARCHIVE_OK);
    vfs_http_free(v);
    free(ref);

    printf("test_vfs_http OK (%ld zahtjeva za %ld bajtova)\n", req_seq, fsz);
    return 0;
}
```

I `tests/run_vfs.sh`:

```sh
#!/bin/sh
# Dize http_server.py nad privremenim fajlom i pusta zadati test binarij.
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
PORT="${PORT:-8110}"
TMP=$(mktemp -d)
PID=
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

# 2 MB pseudoslucajnog sadrzaja - nestisljiv, pa Range greske odmah pucaju
head -c 2097152 /dev/urandom > "$TMP/data.bin"

SRV_ROOT="$TMP" SRV_PORT="$PORT" python3 tests/http_server.py &
PID=$!
sleep 1

SRV_FILE_URL="http://127.0.0.1:$PORT/data.bin" \
SRV_FILE_LOCAL="$TMP/data.bin" \
"$BIN"
```

```bash
chmod +x tests/run_vfs.sh
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_vfs tests/test_vfs_http.c src/vfs_http.c src/common.c \
   $HOST_INC $HOST_LIBS -lm
tests/run_vfs.sh /tmp/cr_t_vfs
```

Očekivano: FAIL — `src/vfs_http.h: No such file or directory`.

- [ ] **Step 3: Implementiraj `src/vfs_http.h`**

```c
/* vfs_http.h - libarchive izvor koji cita HTTP Range zahtjevima
 *
 * Postoji zbog jedne mjerene cinjenice: puna setnja kroz zaglavlja arhive
 * od 782 MB trazi manje od 1% bajtova. Preuzimanje cijelog fajla prije prve
 * stranice je zato cista steta.
 */
#ifndef VFS_HTTP_H
#define VFS_HTTP_H

#include <archive.h>
#include <stdint.h>

typedef struct vfs_http vfs_http_t;

vfs_http_t *vfs_http_new(const char *url);
void        vfs_http_free(vfs_http_t *v);

/* libarchive callback-ovi. `a` smije biti NULL u testovima. */
int        vh_open (struct archive *a, void *cd);
la_ssize_t vh_read (struct archive *a, void *cd, const void **buf);
la_int64_t vh_skip (struct archive *a, void *cd, la_int64_t req);
la_int64_t vh_seek (struct archive *a, void *cd, la_int64_t off, int whence);
int        vh_close(struct archive *a, void *cd);

int64_t vfs_http_size(const vfs_http_t *v);

/* Broj poslatih HTTP zahtjeva - testovi na ovome tvrde da skip i seek
 * ne salju nista i da kes zaglavlja radi. */
long vfs_http_requests(const vfs_http_t *v);

#endif /* VFS_HTTP_H */
```

- [ ] **Step 4: Implementiraj `src/vfs_http.c`**

```c
/* vfs_http.c
 *
 * Adaptivni chunk je mjeren, ne pogodjen (spec 5.3):
 *   fiksnih 4 KB  -> 386 zahtjeva za jednu stranicu od 1.53 MB
 *   fiksnih 1 MB  -> 461 zahtjev za setnju kroz zaglavlja, i 61% fajla preneseno
 * Zato: kreni malo, rasti x4 dok se cita uzastopno, resetuj na svaki skok.
 */
#include "vfs_http.h"
#include "common.h"

#include <curl/curl.h>

#define VH_CHUNK_MIN (4 * 1024)
#define VH_CHUNK_MAX (1024 * 1024)
#define VH_URL_MAX   1024

struct vfs_http {
    char     url[VH_URL_MAX];
    CURL    *curl;

    int64_t  off;        /* logicka pozicija citaca */
    int64_t  size;       /* -1 ako server nije rekao */

    uint8_t *buf;        /* bafer posljednjeg chunka */
    size_t   buf_cap;
    size_t   buf_len;

    size_t   chunk;      /* trenutna velicina zahtjeva */
    long     requests;
};

typedef struct {
    vfs_http_t *v;
    size_t      len;
} sink_t;

static size_t sink_write(void *data, size_t sz, size_t nm, void *ud)
{
    sink_t *s = ud;
    size_t  n = sz * nm;

    if (s->len + n > s->v->buf_cap) {
        size_t   ncap = s->len + n;
        uint8_t *nb   = realloc(s->v->buf, ncap);
        if (!nb)
            return 0;
        s->v->buf     = nb;
        s->v->buf_cap = ncap;
    }
    memcpy(s->v->buf + s->len, data, n);
    s->len += n;
    return n;
}

/* Jedan Range GET. 0 = uspjeh, -1 = greska. */
static int fetch_range(vfs_http_t *v, int64_t off, size_t len)
{
    char range[64];
    snprintf(range, sizeof range, "%lld-%lld",
             (long long)off, (long long)(off + (int64_t)len - 1));

    sink_t s = { v, 0 };

    curl_easy_setopt(v->curl, CURLOPT_URL, v->url);
    curl_easy_setopt(v->curl, CURLOPT_RANGE, range);
    curl_easy_setopt(v->curl, CURLOPT_WRITEFUNCTION, sink_write);
    curl_easy_setopt(v->curl, CURLOPT_WRITEDATA, &s);

    CURLcode rc = curl_easy_perform(v->curl);
    v->requests++;

    if (rc != CURLE_OK) {
        ERR("vfs_http: %s (offset %lld)", curl_easy_strerror(rc), (long long)off);
        return -1;
    }

    long code = 0;
    curl_easy_getinfo(v->curl, CURLINFO_RESPONSE_CODE, &code);
    if (code != 206 && code != 200) {
        ERR("vfs_http: HTTP %ld na offsetu %lld", code, (long long)off);
        return -1;
    }

    v->buf_len = s.len;
    return 0;
}

/* HEAD, samo da se sazna velicina. Ako ne uspije, radi se i bez nje. */
static void probe_size(vfs_http_t *v)
{
    /* vh_open() se zove pri svakom reopenu (ar_seek_to -> ar_open). Velicina
     * se ne mijenja, pa bi ponovni HEAD bio ~115 ms cistog gubitka po skoku. */
    if (v->size >= 0)
        return;

    curl_easy_setopt(v->curl, CURLOPT_URL, v->url);
    curl_easy_setopt(v->curl, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(v->curl, CURLOPT_RANGE, NULL);

    v->size = -1;
    if (curl_easy_perform(v->curl) == CURLE_OK) {
        curl_off_t cl = -1;
        curl_easy_getinfo(v->curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 0)
            v->size = (int64_t)cl;
    }
    v->requests++;

    curl_easy_setopt(v->curl, CURLOPT_NOBODY, 0L);
    curl_easy_setopt(v->curl, CURLOPT_HTTPGET, 1L);
}

vfs_http_t *vfs_http_new(const char *url)
{
    if (!url || !is_url(url))
        return NULL;

    vfs_http_t *v = calloc(1, sizeof *v);
    if (!v)
        return NULL;

    snprintf(v->url, sizeof v->url, "%s", url);
    v->chunk = VH_CHUNK_MIN;
    v->size  = -1;

    v->curl = curl_easy_init();
    if (!v->curl) {
        free(v);
        return NULL;
    }

    curl_easy_setopt(v->curl, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(v->curl, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(v->curl, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(v->curl, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(v->curl, CURLOPT_FOLLOWLOCATION, 1L);
    /* Namjerno bez CURLOPT_TIMEOUT - ubijao bi legitimno spore prenose. */

    return v;
}

void vfs_http_free(vfs_http_t *v)
{
    if (!v)
        return;
    if (v->curl)
        curl_easy_cleanup(v->curl);
    free(v->buf);
    free(v);
}

int64_t vfs_http_size(const vfs_http_t *v)     { return v ? v->size : -1; }
long    vfs_http_requests(const vfs_http_t *v) { return v ? v->requests : 0; }

int vh_open(struct archive *a, void *cd)
{
    (void)a;
    vfs_http_t *v = cd;

    probe_size(v);
    v->off   = 0;
    v->chunk = VH_CHUNK_MIN;
    return ARCHIVE_OK;
}

la_ssize_t vh_read(struct archive *a, void *cd, const void **buf)
{
    vfs_http_t *v = cd;

    if (v->size >= 0 && v->off >= v->size)
        return 0;                         /* EOF */

    size_t want = v->chunk;
    if (v->size >= 0 && v->off + (int64_t)want > v->size)
        want = (size_t)(v->size - v->off);

    if (fetch_range(v, v->off, want) != 0) {
        if (a)
            archive_set_error(a, -1, "vfs_http: Range zahtjev nije uspio");
        return -1;
    }

    if (v->buf_len == 0)
        return 0;

    *buf = v->buf;
    v->off += (int64_t)v->buf_len;

    /* Uzastopno citanje: rasti x4 do gornje granice. */
    v->chunk = (v->chunk * 4 > VH_CHUNK_MAX) ? VH_CHUNK_MAX : v->chunk * 4;

    return (la_ssize_t)v->buf_len;
}

la_int64_t vh_skip(struct archive *a, void *cd, la_int64_t req)
{
    (void)a;
    vfs_http_t *v = cd;

    if (req <= 0)
        return 0;

    if (v->size >= 0 && v->off + req > v->size)
        req = v->size - v->off;

    v->off  += req;
    v->chunk = VH_CHUNK_MIN;      /* skok prekida niz uzastopnih citanja */
    return req;
}

la_int64_t vh_seek(struct archive *a, void *cd, la_int64_t off, int whence)
{
    (void)a;
    vfs_http_t *v = cd;
    int64_t     n;

    switch (whence) {
    case SEEK_SET: n = off; break;
    case SEEK_CUR: n = v->off + off; break;
    case SEEK_END:
        if (v->size < 0)
            return ARCHIVE_FATAL;
        n = v->size + off;
        break;
    default:
        return ARCHIVE_FATAL;
    }

    if (n < 0)
        n = 0;

    v->off   = n;
    v->chunk = VH_CHUNK_MIN;
    return n;
}

int vh_close(struct archive *a, void *cd)
{
    (void)a; (void)cd;
    return ARCHIVE_OK;
}
```

- [ ] **Step 5: Pusti test da prođe**

```bash
tests/run_vfs.sh /tmp/cr_t_vfs
```

Očekivano: `test_vfs_http OK (7 zahtjeva za 2097152 bajtova)` — broj može biti 6-8, test
tvrdi samo `<= 8`. Da chunk ne raste, bio bi 512.

- [ ] **Step 6: Commit**

```bash
git add src/vfs_http.c src/vfs_http.h tests/test_vfs_http.c tests/run_vfs.sh
git commit -m "vfs_http: Range callback-ovi za libarchive sa adaptivnim chunkom"
```

---

### Task 10: Keš zaglavlja — bez njega je skok unazad neupotrebljiv

**Files:**
- Modify: `src/vfs_http.c`, `src/vfs_http.h`
- Test: `tests/test_vfs_cache.c`

**Zašto (spec §7.4):** `ar_seek_to()` pri skoku unazad radi `ar_reset()` + `ar_open()` i
šeta zaglavlja **od nule**. `cache.c` traži `focus-1` pri svakom okretu stranice
(`PREFETCH_BWD 1`), pa bi svaki okret slao ~`ord` zahtjeva — na stranici 400 oko 400.
Izmjereno: šetnja je deterministična, 495 različitih offseta, ukupno 1.93 MB. Keš malih
čitanja zato pretvara ponovnu šetnju u nula zahtjeva.

**Interfaces:**
- Produces: `void vfs_http_cache_stats(const vfs_http_t *v, long *hits, long *misses)`

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_vfs_cache.c`:

```c
/* test_vfs_cache.c - drugi prolaz po istim offsetima ne smije na mrezu */
#include "vfs_http.h"
#include "common.h"
#include <assert.h>

/* Oponasa setnju po zaglavljima: mali read pa veliki skip, u krug. */
static void walk(vfs_http_t *v, int steps)
{
    assert(vh_seek(NULL, v, 0, SEEK_SET) == 0);
    for (int i = 0; i < steps; i++) {
        const void *b = NULL;
        la_ssize_t  n = vh_read(NULL, v, &b);
        if (n <= 0)
            break;
        if (vh_skip(NULL, v, 100000) <= 0)
            break;
    }
}

int main(void)
{
    const char *url = getenv("SRV_FILE_URL");
    assert(url);

    vfs_http_t *v = vfs_http_new(url);
    assert(v);
    assert(vh_open(NULL, v) == ARCHIVE_OK);

    walk(v, 15);
    long after_first = vfs_http_requests(v);
    assert(after_first > 10);          /* prvi prolaz stvarno ide na mrezu */

    /* Drugi prolaz po identicnim offsetima: nijedan novi zahtjev. */
    walk(v, 15);
    assert(vfs_http_requests(v) == after_first);

    long hits = 0, misses = 0;
    vfs_http_cache_stats(v, &hits, &misses);
    assert(hits >= 15);

    /* Veliki chunk se NE kesira - inace bi 700 MB arhiva pojela memoriju.
     * Citanje u nizu naraste preko granice i tada mora ici na mrezu. */
    assert(vh_seek(NULL, v, 0, SEEK_SET) == 0);
    for (int i = 0; i < 6; i++) {
        const void *b = NULL;
        vh_read(NULL, v, &b);          /* 4K, 16K, 64K, 256K, 1M, 1M */
    }
    long before = vfs_http_requests(v);
    assert(vh_seek(NULL, v, 0, SEEK_SET) == 0);
    for (int i = 0; i < 6; i++) {
        const void *b = NULL;
        vh_read(NULL, v, &b);
    }
    /* Prva tri (4K, 16K, 64K) su iz kesa, veci nisu. */
    assert(vfs_http_requests(v) > before);

    vh_close(NULL, v);
    vfs_http_free(v);
    printf("test_vfs_cache OK (hits %ld, misses %ld)\n", hits, misses);
    return 0;
}
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_vcache tests/test_vfs_cache.c src/vfs_http.c src/common.c \
   $HOST_INC $HOST_LIBS -lm
tests/run_vfs.sh /tmp/cr_t_vcache
```

Očekivano: FAIL — `undefined reference to 'vfs_http_cache_stats'`.

- [ ] **Step 3: Dodaj deklaracije u `src/vfs_http.h`**

```c
/* Statistika kesa malih citanja (spec 7.4). */
void vfs_http_cache_stats(const vfs_http_t *v, long *hits, long *misses);
```

- [ ] **Step 4: Dodaj keš u `src/vfs_http.c`**

Uz postojeće `#define`-ove:

```c
/* Kesiraju se samo mala citanja - to su zaglavlja. Podaci stranice su
 * veliki i pri ponovnom otvaranju se ionako ne citaju ponovo.
 * Mjereno: 495 offseta i 1.93 MB pokriva arhivu od 504 stranice, pa je
 * 8 MB cetvorostruka rezerva. */
#define VH_CACHE_MAX_CHUNK (64 * 1024)
#define VH_CACHE_BUDGET    (8 * 1024 * 1024)
```

U `struct vfs_http` dodaj:

```c
    struct vh_ent {
        int64_t  off;
        size_t   len;
        uint8_t *data;
    }       *cache;
    int      n_cache, cap_cache;
    size_t   cache_bytes;
    long     hits, misses;
```

Funkcije za keš, iznad `fetch_range`:

```c
static struct vh_ent *cache_find(vfs_http_t *v, int64_t off, size_t len)
{
    for (int i = 0; i < v->n_cache; i++)
        if (v->cache[i].off == off && v->cache[i].len >= len)
            return &v->cache[i];
    return NULL;
}

static void cache_put(vfs_http_t *v, int64_t off, const uint8_t *data, size_t len)
{
    if (len > VH_CACHE_MAX_CHUNK)
        return;
    if (cache_find(v, off, len))
        return;

    /* FIFO: izbacuju se najstariji upisi, s pocetka niza. */
    int drop = 0;
    while (v->cache_bytes + len > VH_CACHE_BUDGET && drop < v->n_cache) {
        v->cache_bytes -= v->cache[drop].len;
        free(v->cache[drop].data);
        drop++;
    }
    if (drop > 0) {
        memmove(v->cache, v->cache + drop,
                (size_t)(v->n_cache - drop) * sizeof *v->cache);
        v->n_cache -= drop;
    }

    if (v->n_cache == v->cap_cache) {
        int ncap = v->cap_cache ? v->cap_cache * 2 : 128;
        struct vh_ent *nc = realloc(v->cache, (size_t)ncap * sizeof *nc);
        if (!nc)
            return;
        v->cache     = nc;
        v->cap_cache = ncap;
    }

    uint8_t *copy = malloc(len);
    if (!copy)
        return;
    memcpy(copy, data, len);

    v->cache[v->n_cache].off  = off;
    v->cache[v->n_cache].len  = len;
    v->cache[v->n_cache].data = copy;
    v->n_cache++;
    v->cache_bytes += len;
}

static void cache_free(vfs_http_t *v)
{
    for (int i = 0; i < v->n_cache; i++)
        free(v->cache[i].data);
    free(v->cache);
    v->cache       = NULL;
    v->n_cache     = v->cap_cache = 0;
    v->cache_bytes = 0;
}

void vfs_http_cache_stats(const vfs_http_t *v, long *hits, long *misses)
{
    if (hits)   *hits   = v ? v->hits : 0;
    if (misses) *misses = v ? v->misses : 0;
}
```

U `vh_read`, prije poziva `fetch_range`, ubaci provjeru keša i upis poslije uspjeha:

```c
    struct vh_ent *ce = cache_find(v, v->off, want);
    if (ce) {
        v->hits++;
        *buf = ce->data;
        la_ssize_t n = (la_ssize_t)ce->len;
        v->off  += n;
        v->chunk = (v->chunk * 4 > VH_CHUNK_MAX) ? VH_CHUNK_MAX : v->chunk * 4;
        return n;
    }
    v->misses++;

    if (fetch_range(v, v->off, want) != 0) {
        if (a)
            archive_set_error(a, -1, "vfs_http: Range zahtjev nije uspio");
        return -1;
    }

    if (v->buf_len == 0)
        return 0;

    cache_put(v, v->off, v->buf, v->buf_len);
```

U `vfs_http_free`, prije `free(v)`, dodaj `cache_free(v);`.

> **Pažnja:** pokazivač vraćen iz keša mora ostati važeći dok ga libarchive koristi.
> Zato se unosi u kešu **nikad ne oslobađaju dok traje čitanje** osim kroz FIFO
> izbacivanje, a izbacivanje se dešava samo u `cache_put`, prije nego što se novi
> pokazivač preda. Budžet od 8 MB naspram izmjerenih 1.93 MB znači da do izbacivanja
> u praksi ne dolazi.

- [ ] **Step 5: Pusti test da prođe**

```bash
tests/run_vfs.sh /tmp/cr_t_vcache
```

Očekivano: `test_vfs_cache OK (hits ..., misses ...)`. Ponovi i `tests/run_vfs.sh /tmp/cr_t_vfs`
— Task 9 test mora i dalje prolaziti.

- [ ] **Step 6: Commit**

```bash
git add src/vfs_http.c src/vfs_http.h tests/test_vfs_cache.c
git commit -m "vfs_http: kes malih citanja, skok unazad vise ne ide na mrezu"
```

---

### Task 11: Ponavljanje zahtjeva i tabela kredencijala

**Files:**
- Modify: `src/vfs_http.c`, `src/vfs_http.h`, `src/library.c`
- Test: `tests/test_vfs_retry.c`, `tests/run_vfs_faults.sh`

**Zašto (spec §7.5):** kratak pad WiFi-ja ne smije ubiti sesiju čitanja. I: `doc_archive.c`
vidi samo putanju, a lozinka ne smije u URL jer je URL ključ u `.ps5cr_state`.

**Interfaces:**
- Produces:
  - `void vfs_http_register(const char *url_prefix, const char *user, const char *pass)` — poziva se pri startu, iz `library_init`
  - `void vfs_http_clear_creds(void)` — za testove
  - Ponavljanje: 3 pokušaja, pauze 0.5 s / 2 s / 5 s. Ponavlja se na `CURLE_COULDNT_CONNECT`, `CURLE_OPERATION_TIMEDOUT`, `CURLE_RECV_ERROR`, `CURLE_SEND_ERROR`, `CURLE_PARTIAL_FILE`, `CURLE_GOT_NOTHING` i na HTTP `5xx`. **Ne** ponavlja se na `401`, `403`, `404`.

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_vfs_retry.c`:

```c
/* test_vfs_retry.c - prekid veze se prezivljava, trajna greska ne odlaze */
#include "vfs_http.h"
#include "common.h"
#include <assert.h>

int main(void)
{
    const char *url = getenv("SRV_FILE_URL");
    assert(url);

    /* Server obara svaki 3. zahtjev (SRV_FAIL_EVERY=3 u run skripti).
     * Uz tri pokusaja citanje mora proci do kraja. */
    vfs_http_t *v = vfs_http_new(url);
    assert(v);
    assert(vh_open(NULL, v) == ARCHIVE_OK);

    long total = 0;
    for (;;) {
        const void *b = NULL;
        la_ssize_t  n = vh_read(NULL, v, &b);
        assert(n >= 0 && "prekid veze je smio biti ponovljen, ne prijavljen kao greska");
        if (n == 0)
            break;
        total += n;
    }
    assert(total == 2097152);

    vh_close(NULL, v);
    vfs_http_free(v);

    /* 404 se ne ponavlja - trajna greska mora pasti odmah. */
    char bad[512];
    snprintf(bad, sizeof bad, "%s.nema", url);
    vfs_http_t *v2 = vfs_http_new(bad);
    assert(v2);
    vh_open(NULL, v2);
    const void *b = NULL;
    assert(vh_read(NULL, v2, &b) < 0);
    long req = vfs_http_requests(v2);
    /* HEAD + jedan GET, bez tri ponavljanja. */
    assert(req <= 3);
    vfs_http_free(v2);

    printf("test_vfs_retry OK\n");
    return 0;
}
```

I `tests/run_vfs_faults.sh`:

```sh
#!/bin/sh
# Isto kao run_vfs.sh, ali server obara svaki treci zahtjev.
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
PORT="${PORT:-8120}"
TMP=$(mktemp -d)
PID=
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

head -c 2097152 /dev/urandom > "$TMP/data.bin"

SRV_ROOT="$TMP" SRV_PORT="$PORT" SRV_FAIL_EVERY=3 python3 tests/http_server.py &
PID=$!
sleep 1

SRV_FILE_URL="http://127.0.0.1:$PORT/data.bin" "$BIN"
```

```bash
chmod +x tests/run_vfs_faults.sh
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_vretry tests/test_vfs_retry.c src/vfs_http.c src/common.c \
   $HOST_INC $HOST_LIBS -lm
tests/run_vfs_faults.sh /tmp/cr_t_vretry
```

Očekivano: FAIL na prvom `assert(n >= 0)` — bez ponavljanja prvi obaran zahtjev ruši čitanje.

- [ ] **Step 3: Dodaj deklaracije u `src/vfs_http.h`**

```c
/* Kredencijali se drze ovdje, a ne u URL-u: URL je kljuc u .ps5cr_state
 * (spec 14) i lozinka bi zavrsila u citanom tekstu na USB-u.
 * Tabela se puni pri startu i poslije toga je samo za citanje, pa je
 * dijeljenje medju nitima bezbjedno. */
void vfs_http_register(const char *url_prefix, const char *user, const char *pass);
void vfs_http_clear_creds(void);
```

- [ ] **Step 4: Implementiraj u `src/vfs_http.c`**

Uz ostale `#define`-ove:

```c
#define VH_RETRIES  3
#define VH_CRED_MAX 8
```

Tabela kredencijala, iznad `fetch_range`:

```c
static struct {
    char prefix[VH_URL_MAX];
    char user[64];
    char pass[64];
} g_creds[VH_CRED_MAX];
static int g_n_creds;

void vfs_http_register(const char *url_prefix, const char *user, const char *pass)
{
    if (!url_prefix || !user || !*user)
        return;
    if (g_n_creds >= VH_CRED_MAX) {
        ERR("vfs_http: tabela kredencijala je puna");
        return;
    }
    snprintf(g_creds[g_n_creds].prefix, sizeof g_creds[0].prefix, "%s", url_prefix);
    snprintf(g_creds[g_n_creds].user,   sizeof g_creds[0].user,   "%s", user);
    snprintf(g_creds[g_n_creds].pass,   sizeof g_creds[0].pass,   "%s", pass ? pass : "");
    g_n_creds++;
    /* Lozinka se namjerno ne loguje. */
    LOG("vfs_http: kredencijali za %s", url_prefix);
}

void vfs_http_clear_creds(void)
{
    memset(g_creds, 0, sizeof g_creds);
    g_n_creds = 0;
}

/* Najduzi prefiks koji odgovara URL-u. */
static void apply_creds(vfs_http_t *v)
{
    int    best = -1;
    size_t bl   = 0;

    for (int i = 0; i < g_n_creds; i++) {
        size_t pl = strlen(g_creds[i].prefix);
        if (!strncmp(v->url, g_creds[i].prefix, pl) && pl > bl) {
            best = i;
            bl   = pl;
        }
    }
    if (best < 0)
        return;

    curl_easy_setopt(v->curl, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
    curl_easy_setopt(v->curl, CURLOPT_USERNAME, g_creds[best].user);
    curl_easy_setopt(v->curl, CURLOPT_PASSWORD, g_creds[best].pass);
}
```

U `vfs_http_new`, prije `return v;`, dodaj `apply_creds(v);`.

Zamijeni tijelo `fetch_range` verzijom koja ponavlja:

```c
static int curl_err_transient(CURLcode rc)
{
    switch (rc) {
    case CURLE_COULDNT_CONNECT:
    case CURLE_OPERATION_TIMEDOUT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_PARTIAL_FILE:
    case CURLE_GOT_NOTHING:
        return 1;
    default:
        return 0;
    }
}

static int fetch_range(vfs_http_t *v, int64_t off, size_t len)
{
    static const int backoff_ms[VH_RETRIES] = { 500, 2000, 5000 };

    char range[64];
    snprintf(range, sizeof range, "%lld-%lld",
             (long long)off, (long long)(off + (int64_t)len - 1));

    for (int try = 0; try < VH_RETRIES; try++) {
        sink_t s = { v, 0 };

        curl_easy_setopt(v->curl, CURLOPT_URL, v->url);
        curl_easy_setopt(v->curl, CURLOPT_RANGE, range);
        curl_easy_setopt(v->curl, CURLOPT_WRITEFUNCTION, sink_write);
        curl_easy_setopt(v->curl, CURLOPT_WRITEDATA, &s);

        CURLcode rc = curl_easy_perform(v->curl);
        v->requests++;

        long code = 0;
        if (rc == CURLE_OK)
            curl_easy_getinfo(v->curl, CURLINFO_RESPONSE_CODE, &code);

        if (rc == CURLE_OK && (code == 206 || code == 200)) {
            v->buf_len = s.len;
            return 0;
        }

        /* Trajne greske se ne ponavljaju - ponavljanje ih samo odlaze. */
        if (rc == CURLE_OK && (code == 401 || code == 403 || code == 404)) {
            ERR("vfs_http: HTTP %ld, ne ponavljam", code);
            return -1;
        }

        int retryable = (rc != CURLE_OK && curl_err_transient(rc)) ||
                        (rc == CURLE_OK && code >= 500);
        if (!retryable) {
            ERR("vfs_http: %s (HTTP %ld) na offsetu %lld",
                curl_easy_strerror(rc), code, (long long)off);
            return -1;
        }

        if (try + 1 < VH_RETRIES) {
            LOG("vfs_http: prekid na offsetu %lld, pokusaj %d/%d za %d ms",
                (long long)off, try + 2, VH_RETRIES, backoff_ms[try]);
            usleep((useconds_t)backoff_ms[try] * 1000);
        }
    }

    ERR("vfs_http: %d pokusaja nije uspjelo na offsetu %lld",
        VH_RETRIES, (long long)off);
    return -1;
}
```

Dodaj `#include <unistd.h>` na vrh fajla zbog `usleep`.

- [ ] **Step 5: Poveži config sa tabelom u `src/library.c`**

U `library_init`, u petlji koja pravi mrežne izvore, prije `library_add_source`:

```c
            if (cfg.srcs[i].user[0])
                vfs_http_register(cfg.srcs[i].url, cfg.srcs[i].user,
                                  cfg.srcs[i].pass);
```

i dodaj `#include "vfs_http.h"` na vrh `library.c`.

- [ ] **Step 6: Pusti testove da prođu**

```bash
tests/run_vfs_faults.sh /tmp/cr_t_vretry
tests/run_vfs.sh /tmp/cr_t_vfs
tests/run_vfs.sh /tmp/cr_t_vcache
```

Očekivano: `test_vfs_retry OK`, i oba ranija testa i dalje prolaze. U izlazu se vide
`vfs_http: prekid na offsetu ...` linije — to je dokaz da se ponavljanje stvarno dešava.

- [ ] **Step 7: Commit**

```bash
git add src/vfs_http.c src/vfs_http.h src/library.c tests/test_vfs_retry.c tests/run_vfs_faults.sh
git commit -m "vfs_http: ponavljanje uz backoff i tabela kredencijala van URL-a"
```

---

### Task 12: Šav u `doc_archive.c`

Jedina izmjena u tom fajlu u cijelom planu. Ako se mijenja bilo šta osim `ar_open()`, task je pogrešno urađen.

**Files:**
- Modify: `src/doc_archive.c` (`ar_open`, `ar_reset`, `struct doc`)
- Test: `tests/test_archive_http.c`, `tests/run_archive_http.sh`

**Interfaces:**
- Consumes: `vfs_http_*` (Task 9-11), `is_url` (Task 1)
- Produces: `ab_open()` prihvata URL kao `path`; `doc_backend_archive` radi i nad mrežom

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_archive_http.c`:

```c
/* test_archive_http.c - ista arhiva lokalno i preko HTTP-a mora dati isto */
#include "doc.h"
#include "common.h"
#include <assert.h>

/* Iz doc_archive.c, samo za testove. */
long vfs_http_requests_of_doc(doc_t *d);

int main(void)
{
    const char *url   = getenv("SRV_CBZ_URL");
    const char *local = getenv("SRV_CBZ_LOCAL");
    assert(url && local);

    const doc_backend_t *be = doc_backend_for(local);
    assert(be);

    doc_t *dl = be->open(local);
    assert(dl);
    int nl = be->page_count(dl);
    assert(nl > 0);

    doc_t *dh = be->open(url);
    assert(dh && "otvaranje preko URL-a mora raditi");
    int nh = be->page_count(dh);
    assert(nh == nl);

    /* Ista stranica mora dati identicne piksele. */
    doc_page_t pl = { 0, 0, NULL }, ph = { 0, 0, NULL };
    assert(be->render(dl, nl / 2, &pl) == 0);
    assert(be->render(dh, nl / 2, &ph) == 0);
    assert(pl.width == ph.width && pl.height == ph.height);
    assert(memcmp(pl.pixels, ph.pixels, (size_t)pl.width * pl.height * 4) == 0);

    /* Skok unazad - grana koja bez kesa zaglavlja salje ord zahtjeva. */
    doc_page_t pb = { 0, 0, NULL };
    assert(be->render(dh, 0, &pb) == 0);
    doc_page_free(&pb);

    /* Kes zaglavlja mora prezivjeti skok unazad, inace svaki okret stranice
     * ponovo seta kroz cijelu arhivu (spec 7.4). */
    long before = vfs_http_requests_of_doc(dh);
    doc_page_t pb2 = { 0, 0, NULL };
    assert(be->render(dh, 1, &pb2) == 0);      /* skok unazad -> reopen */
    doc_page_free(&pb2);
    assert(vfs_http_requests_of_doc(dh) - before < 5 &&
           "reopen je smio ici iz kesa, ne na mrezu");

    doc_page_free(&pl);
    doc_page_free(&ph);
    be->close(dl);
    be->close(dh);

    printf("test_archive_http OK (%d stranica)\n", nl);
    return 0;
}
```

I `tests/run_archive_http.sh`:

```sh
#!/bin/sh
# Pravi CBZ, servira ga, i pusta test koji poredi lokalno i mrezno citanje.
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
PORT="${PORT:-8130}"
TMP=$(mktemp -d)
PID=
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

# Male PNG stranice, dovoljno da se dobije vise unosa u arhivi.
mkdir -p "$TMP/pages"
python3 - "$TMP/pages" <<'PY'
import struct, sys, zlib, os
out = sys.argv[1]
def png(path, w, h, val):
    raw = b"".join(b"\x00" + bytes([val, val, val] * w) for _ in range(h))
    def chunk(t, d):
        c = t + d
        return struct.pack(">I", len(d)) + c + struct.pack(">I", zlib.crc32(c))
    data = (b"\x89PNG\r\n\x1a\n"
            + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw))
            + chunk(b"IEND", b""))
    open(path, "wb").write(data)
for i in range(1, 13):
    png(os.path.join(out, "page%02d.png" % i), 32, 32, i * 10)
PY

( cd "$TMP" && zip -0 -q -j strip.cbz pages/*.png )

SRV_ROOT="$TMP" SRV_PORT="$PORT" python3 tests/http_server.py &
PID=$!
sleep 1

SRV_CBZ_URL="http://127.0.0.1:$PORT/strip.cbz" \
SRV_CBZ_LOCAL="$TMP/strip.cbz" \
"$BIN"
```

```bash
chmod +x tests/run_archive_http.sh
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_arhttp tests/test_archive_http.c src/doc.c src/doc_archive.c \
   src/vfs_http.c src/common.c src/stb_impl.c \
   $HOST_INC $HOST_LIBS -lm
tests/run_archive_http.sh /tmp/cr_t_arhttp
```

Očekivano: FAIL na `assert(dh)` — `archive_read_open_filename` ne zna za URL.

- [ ] **Step 3: Izmijeni `src/doc_archive.c`**

Dodaj `#include "vfs_http.h"` uz postojeće include-ove.

U `struct doc` dodaj polje:

```c
    vfs_http_t *vh;     /* != NULL samo kad je path URL */
```

Zamijeni `ar_open()`:

```c
static struct archive *ar_open(doc_t *d, const char *path)
{
    struct archive *a = archive_read_new();
    if (!a)
        return NULL;

    archive_read_support_filter_all(a);
    archive_read_support_format_all(a);

    if (is_url(path)) {
        /* Mrezna arhiva: libarchive cita Range zahtjevima. Skip callback nosi
         * cijeli dobitak - preko njega se preskacu podaci unosa, pa se cita
         * manje od 1% fajla (spec 5). archive_read_open2 se NE koristi jer
         * nema seek callback.
         *
         * vfs_http_t se NAMJERNO ne pravi ponovo pri svakom ar_open(): u njemu
         * je kes zaglavlja (spec 7.4), a ar_seek_to() zove ar_open() na svaki
         * skok unazad. Posto cache.c trazi focus-1 pri svakom okretu stranice,
         * ponovno pravljenje bi rusilo kes bas na najcescoj putanji i vratilo
         * nas na ord zahtjeva po okretu. Oslobadja se samo u ab_close(). */
        if (!d->vh) {
            d->vh = vfs_http_new(path);
            if (!d->vh) {
                archive_read_free(a);
                return NULL;
            }
        }

        archive_read_set_callback_data(a, d->vh);
        archive_read_set_open_callback (a, vh_open);
        archive_read_set_read_callback (a, vh_read);
        archive_read_set_skip_callback (a, vh_skip);
        archive_read_set_seek_callback (a, vh_seek);
        archive_read_set_close_callback(a, vh_close);

        if (archive_read_open1(a) != ARCHIVE_OK) {
            ERR("archive_read_open1(%s): %s", path, archive_error_string(a));
            archive_read_free(a);
            return NULL;
        }
        return a;
    }

    if (archive_read_open_filename(a, path, BLOCK_SIZE) != ARCHIVE_OK) {
        ERR("archive_read_open_filename(%s): %s", path, archive_error_string(a));
        archive_read_free(a);
        return NULL;
    }
    return a;
}
```

Na sva tri mjesta gdje se `ar_open` poziva (`ar_seek_to` i `ab_open`), dodaj `d` kao prvi
argument. U `ab_open` se `doc_t` alocira poslije prvog `ar_open`, pa **preokreni redoslijed**:
prvo `calloc` za `d` i `snprintf(d->path, ...)`, pa tek onda `ar_open(d, path)`.

U `ab_close`, prije `free(d)`, dodaj:

```c
    if (d->vh)
        vfs_http_free(d->vh);
```

I na kraj fajla malu pomoćnu funkciju za test iz Step 1 (jedini dodatak izvan `ar_open`):

```c
/* Samo za testove: koliko je HTTP zahtjeva poslao ovaj dokument. */
long vfs_http_requests_of_doc(doc_t *d)
{
    return d && d->vh ? vfs_http_requests(d->vh) : 0;
}
```

> **Napomena:** `vfs_http_t` živi koliko i `doc_t`, ne koliko jedan `archive`. To je uslov da
> keš zaglavlja iz Taska 10 preživi skok unazad — a skok unazad se dešava pri **svakom**
> okretu stranice, jer `cache.c` prefetch traži `focus-1`. Da se `vh` pravi iznova u
> `ar_open`, Task 10 ne bi vrijedio ništa.

- [ ] **Step 4: Pusti test da prođe**

```bash
tests/run_archive_http.sh /tmp/cr_t_arhttp
```

Očekivano: `test_archive_http OK (12 stranica)`, bez prijava sanitizera.

- [ ] **Step 5: Provjeri da lokalni put nije dirnut**

```bash
make test FILE=<neki>.cbz
```

Očekivano: postojeći `test_archive` i `test_cache` prolaze nepromijenjeni.

- [ ] **Step 6: Commit**

```bash
git add src/doc_archive.c tests/test_archive_http.c tests/run_archive_http.sh
git commit -m "doc_archive: URL putanja ide kroz vfs_http callback-ove"
```

---

### Task 13: `fetch()` — izbor režima, LRU keš i nastavak preuzimanja

**Files:**
- Modify: `src/source_http.c`
- Test: `tests/test_http_fetch.c`, `tests/run_http_fetch.sh`

**Odluka o režimu (spec §10):**

| Uslov | `local` dobija | Ko čita |
|---|---|---|
| arhivski format **i** server podržava Range | **sam URL** | `vfs_http.c` |
| nije arhiva (PDF) **ili** nema Range | putanja u kešu | pun download |

PDF ide u download **uvijek**, bez obzira na `Accept-Ranges`, jer streaming postoji samo
kroz libarchive callback-ove, a `doc_pdf.c` se ne dira.

**Interfaces:**
- Consumes: `doc_backend_for` i `doc_backend_archive` iz `doc.h`; `src_progress_fn` (Task 2)
- Produces: puni `http_fetch`; `CR_TMP` (default `/data/tmp/ps5cr`) kao direktorij keša

- [ ] **Step 1: Napiši test koji pada**

Kreiraj `tests/test_http_fetch.c`:

```c
/* test_http_fetch.c - izbor rezima i LRU keš */
#include "source.h"
#include "common.h"
#include <assert.h>
#include <sys/stat.h>

int main(void)
{
    const char *base = getenv("SRV_URL");
    assert(base);

    source_t *s = source_http_new("test", base, "auto", NULL, NULL, 512);
    assert(s);

    char url[LIB_PATH_MAX], local[LIB_PATH_MAX];

    /* 1. Arhiva + Range -> stream, local je sam URL, nista se ne preuzima. */
    snprintf(url, sizeof url, "%sstrip.cbz", base);
    assert(s->be->fetch(s, url, local, sizeof local, NULL, NULL) == 0);
    assert(!strcmp(local, url) && "arhiva uz Range mora ostati URL");

    /* 2. Nije arhiva -> download, bez obzira na Range. */
    snprintf(url, sizeof url, "%sknjiga.pdf", base);
    assert(s->be->fetch(s, url, local, sizeof local, NULL, NULL) == 0);
    assert(is_url(local) == 0 && "PDF mora zavrsiti kao lokalni fajl");

    struct stat st;
    assert(stat(local, &st) == 0);
    assert(st.st_size > 0);

    /* 3. Drugi poziv koristi vec preuzeti fajl - ista putanja, bez ponovnog prenosa. */
    char first[LIB_PATH_MAX];
    snprintf(first, sizeof first, "%s", local);
    time_t mtime_before = st.st_mtime;

    assert(s->be->fetch(s, url, local, sizeof local, NULL, NULL) == 0);
    assert(!strcmp(local, first));
    assert(stat(local, &st) == 0);
    assert(st.st_mtime == mtime_before && "postojeci fajl se ne preuzima ponovo");

    source_free(s);
    printf("test_http_fetch OK\n");
    return 0;
}
```

I `tests/run_http_fetch.sh`:

```sh
#!/bin/sh
set -e

BIN="${1:?zadaj putanju do izgradjenog test binarija}"
PORT="${PORT:-8140}"
TMP=$(mktemp -d)
PID=
trap 'rm -rf "$TMP"; [ -n "$PID" ] && kill "$PID" 2>/dev/null || true' EXIT

mkdir -p "$TMP/srv" "$TMP/cache"
head -c 200000 /dev/urandom > "$TMP/srv/strip.cbz"
head -c 150000 /dev/urandom > "$TMP/srv/knjiga.pdf"

SRV_ROOT="$TMP/srv" SRV_PORT="$PORT" python3 tests/http_server.py &
PID=$!
sleep 1

SRV_URL="http://127.0.0.1:$PORT/" CR_TMP="$TMP/cache" "$BIN"
```

```bash
chmod +x tests/run_http_fetch.sh
```

- [ ] **Step 2: Pusti test da vidiš da pada**

```bash
HOSTROOT=${HOSTROOT:-$HOME/.cache/ps5cr-hostdeps}
HOST_INC="-I$HOSTROOT/root/usr/include -I$HOSTROOT/root/usr/include/libxml2 -I$HOSTROOT/root/usr/include/x86_64-linux-gnu"
HOST_LIBS="-L$HOSTROOT/root/usr/lib -larchive -lxml2 -lcurl"
cc -Wall -Wextra -Isrc -g -fsanitize=address,undefined \
   -o /tmp/cr_t_hfetch tests/test_http_fetch.c src/source_http.c src/dav_parse.c \
   src/html_parse.c src/source.c src/common.c src/doc.c src/doc_archive.c \
   src/vfs_http.c src/stb_impl.c \
   $HOST_INC $HOST_LIBS -lm
tests/run_http_fetch.sh /tmp/cr_t_hfetch
```

Očekivano: FAIL — `http_fetch` je još stub i vraća -1.

- [ ] **Step 3: Zamijeni `http_fetch` u `src/source_http.c`**

Dodaj include-ove i konstante na vrh fajla:

```c
#include "doc.h"

#include <sys/stat.h>
#include <sys/statvfs.h>
#include <dirent.h>
#include <unistd.h>

#define CACHE_DIR_DEFAULT "/data/tmp/ps5cr"
```

Zatim, iznad `http_fetch`:

```c
/* Streaming postoji samo kroz libarchive callback-ove, koje ima jedino
 * doc_archive.c. Sve ostalo (PDF) mora biti pravi fajl na disku. */
static int is_archive_path(const char *path)
{
    return doc_backend_for(path) == &doc_backend_archive;
}

static void cache_dir(char *out, size_t len)
{
    const char *base = getenv("CR_TMP");
    if (!base || !*base)
        base = CACHE_DIR_DEFAULT;

    snprintf(out, len, "%s", base);

    /* mkdir -p, po komponentama. */
    for (char *p = out + 1; *p; p++) {
        if (*p != '/')
            continue;
        *p = '\0';
        mkdir(out, 0755);
        *p = '/';
    }
    mkdir(out, 0755);
}

/* Ime u kesu: hash URL-a + citljivo ime, da se dva istoimena stripa iz
 * razlicitih foldera ne pobrkaju. */
static void cache_name(const char *url, char *out, size_t len)
{
    uint32_t h = 2166136261u;
    for (const unsigned char *p = (const unsigned char *)url; *p; p++) {
        h ^= *p;
        h *= 16777619u;
    }

    char dec[LIB_PATH_MAX];
    url_decode(dec, sizeof dec, url);

    char safe[96];
    snprintf(safe, sizeof safe, "%s", path_base(dec));
    for (char *p = safe; *p; p++)
        if (*p == '/' || *p == ' ' || *p == '\'' || *p == '"')
            *p = '_';

    char dir[LIB_PATH_MAX];
    cache_dir(dir, sizeof dir);
    snprintf(out, len, "%s/%08x_%s", dir, h, safe);
}

/* Budzet: manje od cache_mb iz configa i cetvrtine slobodnog prostora.
 * cache_mb je gornja granica, ne obecanje. */
static int64_t cache_budget(const char *dir, int cache_mb)
{
    struct statvfs vfs;
    int64_t        cfg = (int64_t)cache_mb * 1024 * 1024;

    if (statvfs(dir, &vfs) != 0)
        return cfg;

    int64_t freeb = (int64_t)vfs.f_bavail * (int64_t)vfs.f_frsize;
    int64_t quarter = freeb / 4;
    return (cfg < quarter) ? cfg : quarter;
}

/* Izbacuje najstarije po mtime dok se ne oslobodi `need` bajtova. */
static void cache_evict(const char *dir, int64_t budget, int64_t need)
{
    for (;;) {
        DIR *dp = opendir(dir);
        if (!dp)
            return;

        int64_t total = 0;
        time_t  oldest_t = 0;
        char    oldest[LIB_PATH_MAX] = { 0 };

        struct dirent *de;
        while ((de = readdir(dp)) != NULL) {
            if (de->d_name[0] == '.')
                continue;

            char        p[LIB_PATH_MAX];
            struct stat st;
            if (snprintf(p, sizeof p, "%s/%s", dir, de->d_name) >= (int)sizeof p)
                continue;
            if (stat(p, &st) != 0 || !S_ISREG(st.st_mode))
                continue;

            total += st.st_size;
            if (!oldest[0] || st.st_mtime < oldest_t) {
                oldest_t = st.st_mtime;
                snprintf(oldest, sizeof oldest, "%s", p);
            }
        }
        closedir(dp);

        if (total + need <= budget || !oldest[0])
            return;

        LOG("kes: izbacujem %s", path_base(oldest));
        if (unlink(oldest) != 0)
            return;
    }
}

/* Velicina fajla na serveru, ili -1. Treba je cache_evict() prije nego
 * sto preuzimanje pocne, da se ne krene pa se stane bez prostora.
 * HEAD ide kroz CURLOPT_NOBODY, ne kroz CUSTOMREQUEST - inace curl i dalje
 * ceka tijelo odgovora. */
static int64_t remote_size(http_priv_t *p, const char *url, long *code_out)
{
    CURL *c = curl_easy_init();
    if (!c)
        return -1;

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_NOBODY, 1L);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    if (p->user[0]) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, p->user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, p->pass);
    }

    int64_t sz   = -1;
    long    code = -1;
    if (curl_easy_perform(c) == CURLE_OK) {
        curl_off_t cl = -1;
        curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_getinfo(c, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &cl);
        if (cl > 0)
            sz = (int64_t)cl;
    }
    curl_easy_cleanup(c);

    if (code_out)
        *code_out = code;
    return sz;
}

typedef struct {
    src_progress_fn cb;
    void           *ud;
    int64_t         base;      /* vec preuzeto prije nastavka */
} prog_t;

static int xferinfo(void *ud, curl_off_t dltotal, curl_off_t dlnow,
                    curl_off_t ultotal, curl_off_t ulnow)
{
    (void)ultotal; (void)ulnow;
    prog_t *p = ud;
    if (!p->cb)
        return 0;
    return p->cb(p->ud, p->base + (int64_t)dlnow, p->base + (int64_t)dltotal);
}

/* 0 = spremno, -1 = greska, 1 = korisnik otkazao. */
static int http_download(source_t *s, const char *url, char *local, size_t len,
                         src_progress_fn cb, void *ud)
{
    http_priv_t *p = s->priv;

    cache_name(url, local, len);

    long    code = 0;
    int64_t want = remote_size(p, url, &code);

    if (code == 401 || code == 403) {
        snprintf(s->err, sizeof s->err,
                 "%ld - provjeri user/pass u .ps5cr.conf", code);
        return -1;
    }
    if (code == 404) {
        snprintf(s->err, sizeof s->err, "404 - fajl ne postoji");
        return -1;
    }

    /* Vec preuzet u cijelosti? Onda nema sta da se radi. */
    struct stat st;
    if (stat(local, &st) == 0 && S_ISREG(st.st_mode) &&
        want > 0 && st.st_size == want)
        return 0;

    int64_t have = 0;
    if (stat(local, &st) == 0 && S_ISREG(st.st_mode))
        have = st.st_size;

    CURL *c = curl_easy_init();
    if (!c) {
        snprintf(s->err, sizeof s->err, "curl init");
        return -1;
    }

    /* Nastavak prekinutog preuzimanja - za 780 MB preko WiFi-ja ovo je
     * razlika izmedju smetnje i pocinjanja iz pocetka. */
    FILE *f = NULL;
    if (have > 0) {
        f = fopen(local, "ab");
        curl_easy_setopt(c, CURLOPT_RESUME_FROM_LARGE, (curl_off_t)have);
        LOG("nastavljam preuzimanje od %lld B", (long long)have);
    } else {
        char dir[LIB_PATH_MAX];
        cache_dir(dir, sizeof dir);
        cache_evict(dir, cache_budget(dir, p->cache_mb), want > 0 ? want : 0);
        f = fopen(local, "wb");
    }

    if (!f) {
        curl_easy_cleanup(c);
        snprintf(s->err, sizeof s->err, "ne mogu da pisem u kes");
        return -1;
    }

    prog_t pr = { cb, ud, have };

    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, f);
    curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_LIMIT, 1L);
    curl_easy_setopt(c, CURLOPT_LOW_SPEED_TIME, 20L);
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, xferinfo);
    curl_easy_setopt(c, CURLOPT_XFERINFODATA, &pr);

    if (p->user[0]) {
        curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
        curl_easy_setopt(c, CURLOPT_USERNAME, p->user);
        curl_easy_setopt(c, CURLOPT_PASSWORD, p->pass);
    }

    CURLcode rc = curl_easy_perform(c);
    fclose(f);
    curl_easy_cleanup(c);

    if (rc == CURLE_ABORTED_BY_CALLBACK) {
        LOG("preuzimanje otkazano, djelimican fajl ostaje za nastavak");
        return 1;
    }
    if (rc != CURLE_OK) {
        snprintf(s->err, sizeof s->err, "%s", curl_easy_strerror(rc));
        return -1;
    }
    return 0;
}

static int http_fetch(source_t *s, const char *path, char *local, size_t len,
                      src_progress_fn cb, void *ud)
{
    http_priv_t *p = s->priv;

    s->err[0] = '\0';

    /* PDF i sve sto nije arhiva - uvijek download. */
    if (!is_archive_path(path))
        return http_download(s, path, local, len, cb, ud);

    /* Podrzava li server Range? Jedan zahtjev od jednog bajta je dovoljan. */
    membuf_t mb = { NULL, 0 };
    CURL    *c  = curl_easy_init();
    long     code = 0;

    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, path);
        curl_easy_setopt(c, CURLOPT_RANGE, "0-0");
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, sink);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, &mb);
        curl_easy_setopt(c, CURLOPT_CONNECTTIMEOUT, 5L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        if (p->user[0]) {
            curl_easy_setopt(c, CURLOPT_HTTPAUTH, (long)CURLAUTH_BASIC);
            curl_easy_setopt(c, CURLOPT_USERNAME, p->user);
            curl_easy_setopt(c, CURLOPT_PASSWORD, p->pass);
        }
        if (curl_easy_perform(c) == CURLE_OK)
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
        curl_easy_cleanup(c);
    }
    free(mb.buf);

    if (code == 206) {
        /* Stream: cache_open dobija sam URL, disk se ne dira. */
        snprintf(local, len, "%s", path);
        return 0;
    }

    LOG("server ne podrzava Range (status %ld), prelazim na pun download", code);
    return http_download(s, path, local, len, cb, ud);
}
```

- [ ] **Step 4: Pusti test da prođe**

```bash
tests/run_http_fetch.sh /tmp/cr_t_hfetch
```

Očekivano: `test_http_fetch OK`.

- [ ] **Step 5: Provjeri i grana bez Range podrške**

```bash
SRV_NO_RANGE=1 tests/run_http_fetch.sh /tmp/cr_t_hfetch
```

Očekivano: FAIL na prvom `assert` — jer sada i `.cbz` ide u download, pa `local` više nije
URL. To je **očekivano i tačno**; potvrđuje da odluka o režimu stvarno zavisi od servera.
Zabilježi to u commit poruci i nemoj mijenjati test.

- [ ] **Step 6: Commit**

```bash
git add src/source_http.c tests/test_http_fetch.c tests/run_http_fetch.sh
git commit -m "source_http: izbor stream/download rezima, LRU kes i nastavak preuzimanja"
```

---

### Task 14: `SCREEN_FETCH` i nit za preuzimanje

**Files:**
- Modify: `src/main.c`

**Zašto:** `fetch()` u download režimu blokira dok traje prenos — 782 MB pri izmjerenih
5.69 MB/s je oko 137 s. Iz blokirane glavne petlje se ne može crtati progress, pa
preuzimanje ide u zasebnu SDL nit. `vfs_http.c` **ne** dobija svoju nit; on radi unutar
postojeće `cache.c` radne niti. Ovo je jedini novi paralelizam u planu.

**Interfaces:**
- Consumes: `source_t::be->fetch`, `src_progress_fn` (Task 2), `library_cur` (Task 4)
- Produces: `reader_start(app_t *a, source_t *src, lib_entry_t *it, int want_page)` i `reader_finish(app_t *a)`; polja `a->cur_src` i `a->cur_entry`. Task 15 se oslanja na sve troje.

- [ ] **Step 1: Dodaj stanje i strukturu posla**

U `src/main.c`:

```c
typedef enum { SCREEN_BROWSER, SCREEN_READER, SCREEN_FETCH } screen_t;

typedef struct {
    source_t    *src;
    lib_entry_t  entry;        /* kopija - nivo steka moze nestati dok nit radi */
    char         local[LIB_PATH_MAX];
    SDL_Thread  *thread;
    int          want_page;

    /* Napredak u KB, jer SDL_atomic_t nosi int. 782 MB / 1024 = 763k,
     * daleko od granice. */
    SDL_atomic_t got_kb, total_kb;
    SDL_atomic_t done, status, cancel;
} fetch_job_t;
```

U `app_t` dodaj:

```c
    fetch_job_t  fetch;
    source_t    *cur_src;      /* izvor tekuceg dokumenta, za Task 15 */
    lib_entry_t  cur_entry;
```

- [ ] **Step 2: Nit i progress callback**

```c
static int fetch_progress(void *ud, int64_t got, int64_t total)
{
    fetch_job_t *j = ud;

    SDL_AtomicSet(&j->got_kb,   (int)(got   / 1024));
    SDL_AtomicSet(&j->total_kb, (int)(total / 1024));

    /* !=0 prekida curl. Krug postavlja cancel iz glavne niti. */
    return SDL_AtomicGet(&j->cancel);
}

static int fetch_thread(void *ud)
{
    fetch_job_t *j = ud;

    int r = j->src->be->fetch(j->src, j->entry.path, j->local, sizeof j->local,
                              fetch_progress, j);

    SDL_AtomicSet(&j->status, r);
    SDL_AtomicSet(&j->done, 1);
    return 0;
}
```

- [ ] **Step 3: Zamijeni `reader_open` iz Taska 5**

Oba izvora idu istim putem kroz nit. Za USB je `fetch` identitet, pa se `SCREEN_FETCH`
vidi jedan frejm ili nijedan — jedna grana manje umjesto dvije putanje koje se razilaze.

```c
static void reader_start(app_t *a, source_t *src, lib_entry_t *it, int want_page)
{
    fetch_job_t *j = &a->fetch;

    reader_close(a);

    memset(j, 0, sizeof *j);
    j->src       = src;
    j->entry     = *it;
    j->want_page = want_page;

    SDL_AtomicSet(&j->done, 0);
    SDL_AtomicSet(&j->cancel, 0);
    SDL_AtomicSet(&j->status, 0);
    SDL_AtomicSet(&j->got_kb, 0);
    SDL_AtomicSet(&j->total_kb, 0);

    j->thread = SDL_CreateThread(fetch_thread, "fetch", j);
    if (!j->thread) {
        ERR("SDL_CreateThread: %s", SDL_GetError());
        return;
    }

    a->screen = SCREEN_FETCH;
}

static void reader_finish(app_t *a)
{
    fetch_job_t *j = &a->fetch;

    SDL_WaitThread(j->thread, NULL);
    j->thread = NULL;

    int status = SDL_AtomicGet(&j->status);
    if (status != 0) {
        if (status == 1)
            LOG("preuzimanje otkazano");
        else
            ERR("ne mogu da pripremim %s: %s", j->entry.path,
                j->src->err[0] ? j->src->err : "nepoznata greska");
        a->screen = SCREEN_BROWSER;
        return;
    }

    a->cache = cache_open(j->local, a->ui.r);
    if (!a->cache) {
        ERR("ne mogu da otvorim %s", j->local);
        a->screen = SCREEN_BROWSER;
        return;
    }

    snprintf(a->cur_path,  sizeof a->cur_path,  "%s", j->entry.path);
    snprintf(a->cur_local, sizeof a->cur_local, "%s", j->local);
    a->cur_src   = j->src;
    a->cur_entry = j->entry;

    a->n_pages = cache_page_count(a->cache);

    int p = (j->want_page >= 0) ? j->want_page : j->entry.last_page;
    a->page   = (p > 0 && p < a->n_pages) ? p : 0;
    a->fit    = FIT_SCREEN;
    a->zoom   = 1.0f;
    a->pan_x  = a->pan_y = 0.0f;
    a->screen = SCREEN_READER;

    cache_focus(a->cache, a->page);
    hud_bump(a);
}
```

U `on_button`, grana `SCREEN_BROWSER`, zamijeni poziv iz Taska 5:

```c
                reader_start(a, lv->src, &lv->entries[lv->sel], -1);
```

U `reader_close`, na sam početak, dodaj čekanje na nit — inače bi gašenje aplikacije
ostavilo nit koja piše u `j->local`:

```c
    if (a->fetch.thread) {
        SDL_AtomicSet(&a->fetch.cancel, 1);
        SDL_WaitThread(a->fetch.thread, NULL);
        a->fetch.thread = NULL;
    }
```

- [ ] **Step 4: Ekran preuzimanja**

```c
static void draw_fetch(app_t *a)
{
    ui_t        *ui = &a->ui;
    fetch_job_t *j  = &a->fetch;

    int got   = SDL_AtomicGet(&j->got_kb);
    int total = SDL_AtomicGet(&j->total_kb);

    ui_fill_rect(ui, 0, 0, ui->screen_w, ui->screen_h, COL_BG);

    char title[LIB_TITLE_MAX + 8];
    ui_ellipsize(title, sizeof title, j->entry.name,
                 (ui->screen_w - 2 * PAD) / (8 * 3));
    ui_text(ui, PAD, ui->screen_h / 2 - 120, 3, COL_TEXT, "%s", title);

    int bar_w = ui->screen_w - 2 * PAD;
    int bar_y = ui->screen_h / 2 - 40;

    ui_fill_rect(ui, PAD, bar_y, bar_w, 28, COL_PANEL);
    if (total > 0) {
        int w = (int)((int64_t)bar_w * got / total);
        ui_fill_rect(ui, PAD, bar_y, w, 28, COL_SEL);
    }

    if (total > 0)
        ui_text(ui, PAD, bar_y + 48, 2, COL_DIM, "%d / %d MB   %d%%",
                got / 1024, total / 1024, (int)((int64_t)100 * got / total));
    else
        ui_text(ui, PAD, bar_y + 48, 2, COL_DIM, "%d MB", got / 1024);

    ui_text(ui, PAD, ui->screen_h - 56, 2, COL_DIM, "Krug: otkazi");
}
```

- [ ] **Step 5: Petlja i unos**

U `main()`, u dijelu za crtanje:

```c
        if (a.screen == SCREEN_FETCH && SDL_AtomicGet(&a.fetch.done))
            reader_finish(&a);

        if (a.screen == SCREEN_BROWSER)
            draw_browser(&a);
        else if (a.screen == SCREEN_FETCH)
            draw_fetch(&a);
        else
            draw_reader(&a);
```

U `on_button`, prije grane `SCREEN_READER`:

```c
    if (a->screen == SCREEN_FETCH) {
        if (b == SDL_CONTROLLER_BUTTON_B)
            SDL_AtomicSet(&a->fetch.cancel, 1);
        return;
    }
```

- [ ] **Step 6: Ručna provjera**

```bash
scripts/deps.sh && make host
mkdir -p /tmp/cr_srv /tmp/cr_root
cp <neki>.pdf /tmp/cr_srv/            # PDF uvijek ide u download rezim
printf 'cache_mb = 512\n\n[source]\nname = test\nurl = http://127.0.0.1:8150/\n' \
    > /tmp/cr_root/.ps5cr.conf
SRV_ROOT=/tmp/cr_srv SRV_PORT=8150 python3 tests/http_server.py &
CR_ROOT=/tmp/cr_root CR_TMP=/tmp/cr_cache ./comicreader
```

Provjeri redom:
1. Korijen pokazuje dva reda: `USB cr_root` i `test`.
2. Otvaranje PDF-a pokazuje naslov, traku i procenat koji raste.
3. `Krug` usred preuzimanja vraća u listu; aplikacija ne ostaje zaglavljena.
4. Ponovno otvaranje istog PDF-a nastavlja od prekinutog mjesta (u logu `nastavljam preuzimanje od N B`), ne od nule.
5. Otvaranje `.cbz` sa istog servera **ne** pokazuje progress — ide stream putem.

- [ ] **Step 7: Commit**

```bash
git add src/main.c
git commit -m "main: ekran preuzimanja sa progresom i otkazivanjem, u zasebnoj niti"
```

---

### Task 15: Oporavak od prekida mreže

**Files:**
- Modify: `src/main.c`

**Zašto (spec §7.5):** `vfs_http.c` ponavlja svaki zahtjev tri puta uz backoff, što pokriva
prekide kraće od ~8 s. Kad ni to ne prođe, korisnik ne smije biti izbačen na početak stripa
od 504 stranice.

**Interfaces:**
- Consumes: `cache_failed()` iz `cache.h` (nepromijenjen), `reader_start`, `a->cur_src`, `a->cur_entry` (Task 14)

- [ ] **Step 1: Razlikuj oštećenu stranicu od pale veze**

U `draw_reader`, zamijeni granu `cache_failed`:

```c
    } else if (cache_failed(a->cache, a->page)) {
        /* Kod mreznog izvora je pala veza daleko vjerovatnija od ostecene
         * slike, a za razliku od nje je i rjesiva. */
        if (a->cur_src && !strcmp(a->cur_src->be->kind, "http")) {
            ui_text(ui, PAD, ui->screen_h / 2 - 30, 3, COL_TEXT,
                    "Veza prekinuta");
            ui_text(ui, PAD, ui->screen_h / 2 + 20, 2, COL_DIM,
                    "Krst: pokusaj ponovo   Krug: nazad na listu");
        } else {
            ui_text(ui, PAD, ui->screen_h / 2, 3, COL_DIM,
                    "Stranica %d se ne moze prikazati", a->page + 1);
        }
    } else {
```

- [ ] **Step 2: Krst ponovo otvara na tekućoj stranici**

U `on_button`, grana `SCREEN_READER`, izdvoji `BUTTON_A` iz zajedničkog `case` bloka sa
`RIGHTSHOULDER` i `DPAD_RIGHT`:

```c
    case SDL_CONTROLLER_BUTTON_A:
        if (cache_failed(a->cache, a->page) && a->cur_src &&
            !strcmp(a->cur_src->be->kind, "http")) {
            /* Tekuca stranica, ne zapamcena - korisnik ne smije nazad na pocetak. */
            reader_start(a, a->cur_src, &a->cur_entry, a->page);
        } else {
            reader_goto(a, a->page + 1);
        }
        break;
```

Zahvaljujući ispravci u Tasku 12 (`vfs_http_t` živi koliko i `doc_t`), keš zaglavlja
preživljava, pa ponovno otvaranje ne plaća šetnju kroz arhivu dok proces živi. Ako je
proces ugašen, `.ps5cr_state` čuva stranicu (spec §14).

- [ ] **Step 3: Provjera sa stvarnim prekidom**

```bash
head -c 20000000 /dev/urandom > /tmp/cr_srv/veliki.cbz
SRV_ROOT=/tmp/cr_srv SRV_PORT=8150 SRV_FAIL_EVERY=2 python3 tests/http_server.py &
CR_ROOT=/tmp/cr_root ./comicreader
```

Sa `SRV_FAIL_EVERY=2` ponavljanje ne može uspjeti, pa se mora pojaviti ekran
`Veza prekinuta`. Ugasi server i pritisni Krst — mora ostati na istoj stranici. Pokreni
server bez `SRV_FAIL_EVERY` i pritisni Krst — čitanje se nastavlja s te iste stranice.

- [ ] **Step 4: Commit**

```bash
git add src/main.c
git commit -m "main: oporavak od prekida mreze na tekucoj stranici"
```

---

### Task 16: PS5 build i objedinjen `make test`

**Files:**
- Modify: `Makefile`, `README.md`

- [ ] **Step 1: Dodaj nove fajlove u `SRCS`**

```make
SRCS := \
	$(SRCDIR)/main.c \
	$(SRCDIR)/common.c \
	$(SRCDIR)/ui.c \
	$(SRCDIR)/library.c \
	$(SRCDIR)/config.c \
	$(SRCDIR)/source.c \
	$(SRCDIR)/source_usb.c \
	$(SRCDIR)/source_http.c \
	$(SRCDIR)/dav_parse.c \
	$(SRCDIR)/html_parse.c \
	$(SRCDIR)/vfs_http.c \
	$(SRCDIR)/cache.c \
	$(SRCDIR)/doc.c \
	$(SRCDIR)/doc_archive.c \
	$(SRCDIR)/doc_pdf.c \
	$(SRCDIR)/stb_impl.c
```

- [ ] **Step 2: Dodaj biblioteke u PS5 granu**

```make
PS5_PKGS   := sdl2 libarchive libwebp libcurl libxml-2.0
BASE_CFLAGS += -DHAVE_WEBP -DCURL_STATICLIB
```

`-DCURL_STATICLIB` je obavezan — `libcurl.pc` iz pacbrew paketa ga nosi u `Cflags`.

- [ ] **Step 3: Provjeri zamku relokacije SDK-a**

Spec §6: `libcurl.pc` ima u `Libs.private` ukucane `/opt/ps5-payload-sdk` putanje, a SDK je
relociran u `~/ps5-toolchain`. Prvo pogledaj šta pkg-config stvarno vrati:

```bash
prospero-pkg-config --libs libcurl libxml-2.0
prospero-pkg-config --static --libs libcurl | tr ' ' '\n' | grep '^-L'
```

Ako se pojavi `-L/opt/ps5-payload-sdk/...`, dodaj ispravan `-L` **prije** njega:

```make
LDADD  += -L$(PS5_PAYLOAD_SDK)/target/user/homebrew/lib
```

Ne dodavaj ga unaprijed — ovo se rješava na osnovu stvarnog izlaza, ne pretpostavke.

- [ ] **Step 4: Linkuj**

```bash
export PS5_PAYLOAD_SDK=$HOME/ps5-toolchain/sdk/ps5-payload-sdk
export PATH="$HOME/ps5-toolchain/llvm-root/usr/bin:$PS5_PAYLOAD_SDK/bin:$PATH"
scripts/deps.sh && make
```

OpenSSL koji curl vuče može tražiti `-lSceSsl` i `-lSceRandom` uz postojeći `-lSceNet`.
Ako linker prijavi nerazriješene simbole iz njih, dopuni `LDADD`:

```make
LDADD  += -lSceSsl -lSceRandom
```

Očekivano: `comicreader.elf` se napravi bez grešaka.

- [ ] **Step 5: Objedini `make test`**

```make
TEST_FLAGS := $(BASE_CFLAGS) -g -fsanitize=address,undefined $(PKG_CFLAGS)
TEST_CORE  := $(SRCDIR)/common.c $(SRCDIR)/source.c $(SRCDIR)/doc.c \
              $(SRCDIR)/doc_archive.c $(SRCDIR)/stb_impl.c
TEST_NET   := $(SRCDIR)/source_http.c $(SRCDIR)/dav_parse.c \
              $(SRCDIR)/html_parse.c $(SRCDIR)/vfs_http.c

hostdeps:
	@test -d "$(HOSTROOT)/root/usr/include" || scripts/host_deps.sh

test: hostdeps
	cc $(TEST_FLAGS) -o /tmp/cr_t_common tests/test_common.c $(SRCDIR)/common.c
	cc $(TEST_FLAGS) -o /tmp/cr_t_cfg    tests/test_config.c $(SRCDIR)/config.c $(SRCDIR)/common.c
	cc $(TEST_FLAGS) -o /tmp/cr_t_usb    tests/test_source_usb.c $(SRCDIR)/source_usb.c $(TEST_CORE) $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_nav    tests/test_nav.c $(SRCDIR)/library.c $(SRCDIR)/config.c $(SRCDIR)/source_usb.c $(TEST_NET) $(TEST_CORE) $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_dav    tests/test_dav.c $(SRCDIR)/dav_parse.c $(TEST_CORE) $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_html   tests/test_html.c $(SRCDIR)/html_parse.c $(TEST_CORE) $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_hlist  tests/test_http_list.c $(TEST_NET) $(TEST_CORE) $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_hfetch tests/test_http_fetch.c $(TEST_NET) $(TEST_CORE) $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_vfs    tests/test_vfs_http.c $(SRCDIR)/vfs_http.c $(SRCDIR)/common.c $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_vcache tests/test_vfs_cache.c $(SRCDIR)/vfs_http.c $(SRCDIR)/common.c $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_vretry tests/test_vfs_retry.c $(SRCDIR)/vfs_http.c $(SRCDIR)/common.c $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_arhttp tests/test_archive_http.c $(SRCDIR)/vfs_http.c $(TEST_CORE) $(PKG_LIBS) -lm
	/tmp/cr_t_common
	/tmp/cr_t_cfg
	/tmp/cr_t_usb
	/tmp/cr_t_nav
	/tmp/cr_t_dav
	/tmp/cr_t_html
	tests/run_http_list.sh    /tmp/cr_t_hlist
	tests/run_http_fetch.sh   /tmp/cr_t_hfetch
	tests/run_vfs.sh          /tmp/cr_t_vfs
	tests/run_vfs.sh          /tmp/cr_t_vcache
	tests/run_vfs_faults.sh   /tmp/cr_t_vretry
	tests/run_archive_http.sh /tmp/cr_t_arhttp
	@test -z "$(FILE)" || cc $(TEST_FLAGS) -o /tmp/cr_t_archive tests/test_archive.c $(TEST_CORE) $(PKG_LIBS) -lm
	@test -z "$(FILE)" || cc $(TEST_FLAGS) -o /tmp/cr_t_cache tests/test_cache.c $(SRCDIR)/ui.c $(SRCDIR)/cache.c $(SRCDIR)/doc_pdf.c $(SRCDIR)/vfs_http.c $(TEST_CORE) $(PKG_LIBS) -lm
	@test -z "$(FILE)" || /tmp/cr_t_archive "$(FILE)"
	@test -z "$(FILE)" || SDL_VIDEODRIVER=dummy /tmp/cr_t_cache "$(FILE)"

.PHONY: host test clean hostdeps
```

Postojeća dva testa i dalje traže `FILE=`, ali `make test` bez njega više ne pada — pušta
sve što ne traži stvarnu arhivu.

- [ ] **Step 6: Puna provjera**

```bash
make test
make test FILE=~/strip.cbz
make host
PS5_PAYLOAD_SDK=$HOME/ps5-toolchain/sdk/ps5-payload-sdk make
```

Sva četiri moraju proći.

- [ ] **Step 7: Dopuni README**

- „Arhitektura": novi fajlovi (`source.h`, `source_usb.c`, `source_http.c`, `dav_parse.c`, `html_parse.c`, `vfs_http.c`, `config.c`)
- „Kontrole": `Krug` u listi sada izlazi iz foldera, a gasi aplikaciju tek u korijenu
- Nova sekcija o `.ps5cr.conf` s primjerom iz specifikacije §12
- „Poznata ograničenja": prvo otvaranje mrežnog stripa traje oko minut (§5.2), svako sljedeće je trenutno dok proces živi

- [ ] **Step 8: Commit**

```bash
git add Makefile README.md
git commit -m "build: libcurl i libxml2 u PS5 build, objedinjen make test"
```

---

## Pokrivenost specifikacije

Provjereno svih 18 sekcija naspram 17 zadataka:

| Spec | Zadatak |
|---|---|
| §7.1 šav u `doc_archive.c` | Task 12 |
| §7.2 adaptivni chunk | Task 9 |
| §7.3 transport | Task 9 |
| §7.4 keš zaglavlja | Task 10 (+ uslov u Tasku 12: `vfs_http_t` živi koliko `doc_t`) |
| §7.5 ponavljanje / oporavak | Task 11 / Task 15 |
| §8 `source.h` | Task 2 |
| §9 listanje | Taskovi 6, 7, 8 |
| §10 dva režima | Task 13 |
| §11 budžet na disku | Task 13 |
| §12 config | Task 3 |
| §13 navigacijski stek | Task 4 |
| §14 stanje čitanja | Task 4 |
| §15 UI | Taskovi 5, 14, 15 |
| §16 testovi | raspoređeno, objedinjeno u Tasku 16 |
| §17 build | Task 16 |

Jedina stavka iz specifikacije koja **namjerno** nije u planu je sidecar indeks unosa iz
§18 — tamo je i opisana kao zaseban zadatak, ne dio ovog plana.
