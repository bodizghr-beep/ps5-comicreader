# Mrežni izvor (WebDAV/HTTP) i folder tree — dizajn

Datum: 2026-09-01
Status: **odobren za planiranje**. Sve odluke potkrijepljene mjerenjem (§5, §5.1, §5.2).

## 1. Cilj

Dvije funkcionalnosti koje se namjerno rade zajedno, jer bi se stablo foldera
inače pisalo dvaput:

1. **Mrežni izvor** uz postojeći USB skener — korisnik bira izvor, adresa je u
   config fajlu na USB-u.
2. **Hijerarhijsko kretanje** kroz foldere umjesto ravne liste, s istim UI-jem
   za oba izvora.

## 2. Van dosega

- Pretraga i sortiranje po datumu (postojeće ograničenje, ostaje).
- Upis na NAS. Sav mrežni pristup je read-only.
- HTTPS. Endpoint je `http://` na LAN-u; TLS se ne konfiguriše niti testira.
- Prikaz naših slova. Font je i dalje ASCII (`ui.c`).

## 3. Zatečeno stanje

- `library_scan()` rekurzivno obilazi `/mnt/usb0..7` do dubine 5 i vraća **ravnu**
  listu `lib_item_t` (`src/library.c:88`).
- `main.c` indeksira tu listu direktno: `a->lib.items[i]`, `a->lib.count`,
  `reader_open(a, a->sel)`.
- `doc.h` otvara dokument **po putanji**; `doc_archive.c` hrani libarchive s
  `archive_read_open_filename()` (`src/doc_archive.c:115`), a `ab_open()` radi pun
  prolaz kroz sva zaglavlja (`src/doc_archive.c:230`).
- `cache.c` dekodira u radnoj niti, glavna nit radi upload na GPU.

`cache.c` se ne dira. `doc_archive.c` se dira **samo** u `ar_open()`, vidi §7.

## 4. Odluke

| Odluka | Izbor | Zašto |
|---|---|---|
| Izbor izvora | Virtuelni korijen stabla | Izvor je samo folder na dubini 0; nema moda, jedan kod za navigaciju |
| Apstrakcija izvora | `source.h` vtable | Isti obrazac kao `doc.h`; parseri testabilni bez mreže |
| Preuzeti fajl | LRU keš u `/data/tmp/ps5cr/` | Ponovno otvaranje bez mreže; budžet ograničen slobodnim prostorom |
| Config | Ponovljeni `[source]` blokovi | Lista izvora ionako postoji (do 8 USB slotova) |
| Veliki CBR | Range streaming kroz libarchive | Mjereno: <1% prometa umjesto punog fajla (§5) |

## 5. Mjerenja koja opravdavaju §7

Test: 120 stranica × 600 KB = 69 MB, formati ZIP (stored), TAR, RAR5 nesolidan i
solidan. Callback-ovi broje bajtove koje libarchive stvarno traži, uz isti pun
prolaz kroz zaglavlja koji radi `ab_open()`.

**Trošak punog prolaza kroz zaglavlja, chunk 4 KB:**

| Format | `read` poziva | pročitano |
|---|---|---|
| ZIP, sa seek callback-om | 139 | 0.79% |
| ZIP, bez seek callback-a | 132 | 0.75% |
| TAR (nema central directory) | 121 | 0.69% |
| RAR5 nesolidan | 139 | 0.78% |
| RAR5 solidan | 139 | 0.78% |

Dva zaključka, oba suprotna prvobitnoj pretpostavci:

- **Central directory se ne koristi.** Seekable ZIP čitač ga nađe i pročita, pa
  svejedno prođe arhivu od početka. Posao radi `skip` callback, ne `seek`.
- **RAR nije skuplji od ZIP-a.** Bojazan da CBR ne trpi random pristup ne stoji;
  brojke su iste, i za solidnu varijantu.

**Trošak po veličini chunka (sintetički RAR5, do 60. unosa pa izvlačenje te stranice):**

| chunk | otvaranje | izvlačenje stranice (600 KB) |
|---|---|---|
| 4 KB | 76 read, 311 KB | 146 read, 598 KB |
| 64 KB | 62 read, 4014 KB | 9 read, 590 KB |
| 1 MB | 32 read, 32522 KB | 1 read, 1049 KB |

Odatle zahtjev za **adaptivnim chunkom** (§7.2). Fiksna vrijednost čini jedan od
dva puta patološkim.

Brojke iznad su sa **sintetičke** arhive (120 stranica × 600 KB). Stvarni fajl mjeren je
zasebno i daje veće brojeve — vidi §5.1; gdje se razilaze, mjerodavan je §5.1.

### 5.1 Mjereno na stvarnom NAS-u i stvarnom fajlu

`Stripoteka 41-50.cbr`, 782 MB, **504 stranice**. Sve sa žičanog PC-a; PS5 je na WiFi-ju,
pa je propusnost tamo niža, a cijena po zahtjevu slična ili gora.

| Veličina | Vrijednost |
|---|---|
| cijena zahtjeva | prosjek **115 ms** (100 stvarnih offseta zaglavlja), medijana 40-50 ms |
| progresivno usporavanje | nema — prosjek po grupama od 20 ostaje 49-136 ms |
| pun prolaz kroz zaglavlja | **496 zahtjeva → ~57 s** |
| pun download | 782 MB za **9.6 s** (81 MB/s, žičano) |
| 8 paralelnih konekcija | **2.5× sporije** od jedne (30 naspram 77 MB/s) |

Tri posljedice:

- **Paralelizam ne pomaže.** TS-228 je slab uređaj; osam konekcija ga koštaju više nego
  što donose. Ni za download režim, ni za čitanje stranice.
- **Veći chunk ne štedi zahtjeve.** Na stvarnom fajlu 4 KB daje 496 poziva, a 1 MB daje
  461 — jer su unosi veliki i veći chunk ne pokriva dva zaglavlja. Pri tome 1 MB prenosi
  61% fajla umjesto 0.26%. Adaptivni chunk iz §7.2 zato ostaje, s **malim** početnim
  chunkom; cijena zahtjeva jeste ravna po veličini, ali to ne opravdava veći chunk kad
  broj zahtjeva ostaje isti.
- **Streaming je latencijski vezan, download propusno.** Prelomna tačka je oko 13 MB/s
  na strani PS5: ispod toga streaming pobjeđuje, iznad toga download.

### 5.2 Propusnost PS5 i odluka o §7

Izmjereno FTP transferom na konzolu: **5.69 MB/s** preko PS5 WiFi-ja. Prelomna tačka je
13 MB/s, pa **streaming pobjeđuje**:

| | streaming | pun download |
|---|---|---|
| do prve stranice | **~57 s** | ~137 s |
| okretanje stranice | <1 s | trenutno |
| `/data` | netaknut | 782 MB po stripu |

Odluka je robusna: i da je stvarna propusnost dvostruka (transfer je bio kratak, 5.59 MB,
pa TCP slow start podcjenjuje), download bi bio ~71 s i streaming bi i dalje vodio.
Tek iznad 13 MB/s odnos se okreće.

### 5.3 Trošak po stranici — zašto chunk mora biti adaptivan

Stvarna stranica je 1.53 MB (782 MB / 504).

| chunk | šetnja po zaglavljima | izvlačenje jedne stranice |
|---|---|---|
| 4 KB | 496 zahtjeva, 2 MB | **386 zahtjeva** ≈ 44 s |
| 64 KB | 481 zahtjeva, 31 MB | 24 zahtjeva ≈ 2.8 s |
| 1 MB | 461 zahtjeva, 481 MB | **1 zahtjev** ≈ 0.2 s |

Nijedna fiksna vrijednost ne valja: mali chunk čini okretanje stranice neupotrebljivim,
veliki čini otvaranje neupotrebljivim. Otuda §7.2.

**Ograda:** mjereno na `-m0` (stored) arhivama. Solidan *i komprimovan* RAR imao bi
jeftinu šetnju po zaglavljima, ali skupo izvlačenje pojedinačne stranice.

## 6. Izmjereno okruženje

- WebDAV QNAP-a je na **portu 5000**, ne 80: `WWW-Authenticate: Basic realm="DAV-root"`.
  Na portu 80 PROPFIND vraća `405` na share putanjama.
- Share je `/STRIPOVI/`; nalog `PS5` ima Read Only. WebDAV pravo je na QNAP-u zasebno
  od običnih dozvola nad share-om — dok nije podešeno, **svaka** putanja vraća `404`,
  uključujući `Public/` i `Multimedia/`, a PROPFIND korijena vraća `207` s praznim
  spiskom. Taj obrazac je dijagnostika, ne kvar.
- **KeepAlive je isključen na cijelom `:5000` vhostu**, uključujući GET na fajlu:
  20 uzastopnih Range GET-ova otvorilo je 20 TCP konekcija. Na portu 80 istog uređaja
  keep-alive radi (5 zahtjeva, 1 konekcija), pa je to konfiguracija DAV vhosta, ne
  ograničenje uređaja. Mijenjanje te konfiguracije traži admin pristup i po pravilu
  ne preživi firmware update, pa se dizajn na to ne oslanja.
- Admin UI je na 8080 (`Server: http server 1.0`), nema veze s ovim.
- `libcurl.a` 8.18.0 i `libxml2` postoje u pacbrew paketima.
- `libcurl.pc` ima u `Libs.private` ukucane `/opt/ps5-payload-sdk` putanje, a SDK je
  relociran u `~/ps5-toolchain` — ista zamka koju `SETUP.md` opisuje za
  `prospero-sdl2-config`. Provjeriti na prvom linkovanju (§12).

## 7. `vfs_http.c` — Range-backed izvor za libarchive

### 7.1 Šav prema `doc_archive.c`

Jedina izmjena u `doc_archive.c`, unutar `ar_open()`:

```c
if (is_url(path)) {
    vfs_http_t *vh = vfs_http_new(path);          /* kredencijale nadje sam, vidi nize */
    if (!vh) { archive_read_free(a); return NULL; }
    archive_read_set_callback_data(a, vh);
    archive_read_set_open_callback (a, vh_open);
    archive_read_set_read_callback (a, vh_read);
    archive_read_set_skip_callback (a, vh_skip);   /* nosi cijeli dobitak */
    archive_read_set_seek_callback (a, vh_seek);   /* jeftino, ZIP ga koristi za EOCD */
    archive_read_set_close_callback(a, vh_close);
    if (archive_read_open1(a) != ARCHIVE_OK) { ... }
} else {
    archive_read_open_filename(a, path, BLOCK_SIZE);   /* nepromijenjeno */
}
```

`archive_read_open2()` se **ne** koristi jer nema seek callback. Postojeća grana za
lokalne fajlove ostaje bajt-identična. `is_url()` je nova funkcija u `common.c`
(prefiks `http://`), uz postojeće `path_ext()` i `path_base()`.

**Kredencijali.** `doc_archive.c` vidi samo putanju i ne smije znati za izvore, a
lozinka ne smije ući u URL — jer je URL ujedno ključ u `.ps5cr_state` (§14) i završio
bi u čistom tekstu na USB-u. Zato `vfs_http.c` drži malu tabelu koju `config.c` puni
pri startu:

```c
void vfs_http_register(const char *url_prefix, const char *user, const char *pass);
```

`vfs_http_new()` traži najduži prefiks koji odgovara URL-u i uzima njegove kredencijale.
Tabela je read-only nakon starta, pa je dijeljenje među nitima bezbjedno.

### 7.2 Adaptivni chunk

```
chunk = 4 KB
na read:            posluži iz bafera; na promašaj Range GET od `off` za `chunk`,
                    zatim chunk = MIN(chunk * 4, 1 MB)
na skip ili seek:   pomjeri `off`, invalidiraj bafer, chunk = 4 KB
```

Rast je **×4**, ne ×2: sa ×4 se stranica od 1.53 MB pokrije nizom 4→16→64→256→1024 KB,
dakle ~6 zahtjeva ≈ 0.7 s, dok bi ×2 tražio ~9. Gornja granica od 1 MB je tu jer preko
nje libarchive ionako ne traži više odjednom (mjereno: 1 read po stranici pri 1 MB).

Šetnja po zaglavljima je uvijek `read`→`skip`, pa se chunk resetuje i ostaje na 4 KB —
496 zahtjeva i 2 MB prometa umjesto 481 MB.

`skip` i `seek` ne šalju nijedan zahtjev — samo pomjeraju offset. To je i razlog zašto
je `skip` callback obavezan, a `seek` samo koristan.

**Paralelizam se ne koristi.** Mjereno (§5.1): 8 paralelnih konekcija je 2.5× sporije od
jedne. Uz to je šetnja po zaglavljima kod RAR-a serijski lanac zavisnosti — offset
zaglavlja N+1 poznat je tek nakon čitanja zaglavlja N — pa `curl_multi` tu nema šta da
paralelizuje ni u teoriji.

### 7.3 Transport

Jedan `CURL*` easy handle po dokumentu, keep-alive. `doc.h` već garantuje jedan
dokument = jedna nit, pa nema dijeljenja. `curl_global_init()` ide u `main()` prije
pokretanja ijedne niti (nije thread-safe).

- `CURLOPT_CONNECTTIMEOUT = 5`
- `CURLOPT_LOW_SPEED_LIMIT` / `_TIME` za detekciju zastoja
- **ne** `CURLOPT_TIMEOUT` — ubijao bi legitimno spore transfere
- `CURLOPT_USERPWD` + `CURLAUTH_BASIC` kad izvor ima kredencijale

Neuspjeh Range zahtjeva vraća `ARCHIVE_FATAL`; posljedica je postojeći
`cache_failed()` put, koji već crta „Stranica N se ne može prikazati".

## 8. `source.h` — apstrakcija izvora

```c
typedef struct {
    char name[LIB_TITLE_MAX];   /* prikaz: ime foldera, ili naslov bez ekstenzije */
    char path[LIB_PATH_MAX];    /* USB: /mnt/usb0/x.cbz   HTTP: puni URL */
    int  is_dir;
    int  last_page;             /* -1 ako nije citano */
} lib_entry_t;

typedef int (*src_progress_fn)(void *ud, int64_t got, int64_t total); /* !=0 -> otkazi */

typedef struct {
    const char *kind;                                    /* "usb" / "http" */
    int  (*list) (source_t *s, const char *path, lib_entry_t **out, int *n);
    int  (*fetch)(source_t *s, const char *path, char *local, size_t len,
                  src_progress_fn cb, void *ud);         /* 0 ok, -1 greska, 1 otkazano */
    void (*close)(source_t *s);
} source_backend_t;
```

`source_usb.c`: `list` je `opendir` jednog nivoa (rekurzija nestaje), `fetch` je
`snprintf(local, len, "%s", path)` i uvijek 0. USB nikad ne dodiruje `/data/tmp`.

`source_http.c`: `list` je PROPFIND ili HTML autoindex (§9), `fetch` bira režim (§10).

## 9. Listanje mrežnog foldera

PROPFIND `Depth: 1`, parsiranje `multistatus` XML-a preko **libxml2** (`xmlReader`,
pull parser). Ručni skener bi morao ispravno rukovati namespace prefiksima
(`D:`, `d:`, `lp1:`, `ns0:`) i XML entitetima — klasa greške koju ne pišemo ručno.

Po `response` uzimamo `href`, `resourcetype/collection`, `getcontentlength`.
Self-unos (href jednak traženoj putanji) se preskače, `href` se URL-dekodira,
relativan href se razrješava u apsolutni URL.

`type = auto` (default): PROPFIND → na `405`/`501` padni na GET + HTML autoindex
parser (skenira `href="..."`, odbacuje `?sort=` linkove i `../`; završetak na `/`
znači folder). Oba parsera vraćaju isti `lib_entry_t[]`.

Iz stvarnog odgovora (`tests/fixtures/propfind_stripovi.xml`) slijede tri obaveze parsera:

- **URL-dekodiranje** — `Stripoteka%2041-50.cbr`, a ime sadrži i `,` i `)`.
- **Filtriranje QNAP-ovih sistemskih foldera** — `@Recycle`, `@Transcode`, `.@__thumb`.
  Prefiks `@` uz postojeće pravilo za `.` pokriva sve viđeno.
- **Preskakanje self-unosa** — prvi `response` je sama tražena putanja.

Fixture sadrži i ugniježđen folder istog imena kao share (`/STRIPOVI/STRIPOVI/`) i
`index.php` koji otpada na `doc_is_supported()`. Oboje ostaje u testu namjerno.

Filtriranje i sortiranje su zajednički za oba izvora: folderi prije fajlova, unutar
grupe postojeći `natural_cmp()`; fajlovi koji ne prođu `doc_is_supported()` se ne
prikazuju, folderi uvijek.

## 10. Dva režima jednog `fetch()`

Na početku `fetch()` provjeri `Accept-Ranges` (iz HEAD-a ili već dobijeno PROPFIND-om):

| Uslov | `local` sadrži | Ko čita |
|---|---|---|
| Range **i** arhivski format | **sam URL** | `vfs_http.c`, streaming (§7) |
| nema Range **ili** nije arhiva | putanju u `/data/tmp/ps5cr/` | pun download uz progress, otkazivanje, resume |

Drugi uslov nije suvišan: streaming ide kroz libarchive callback-ove, koje ima samo
`doc_archive.c`. PDF zato **uvijek** ide u download režim, bez obzira na `Accept-Ranges`,
jer `doc_pdf.c` se ne dira a MuPDF otvara po putanji. Odluku donosi `fetch()` na osnovu
ekstenzije, prije provjere Range podrške.

`main.c` u oba slučaja samo prosljeđuje `local` u `cache_open()`.

Download režim ostaje potreban i za **PDF**: `doc_pdf.c` se ne dira, a MuPDF otvara
po putanji. Prekinut download se nastavlja preko `CURLOPT_RESUME_FROM_LARGE`.

## 11. Budžet na disku

`statvfs("/data")` pri startu; na host build-u, gdje `/data` ne postoji, mjeri se
filesystem direktorijuma za preuzimanje (`CR_TMP`, default `/tmp/ps5cr`). Budžet =
`min(cache_mb iz configa, 25% slobodnog)`.
Izbacivanje najstarijih po `mtime` **prije** početka preuzimanja, na osnovu
`Content-Length`. Ako jedan fajl ne stane u budžet — jasna poruka, a ne punjenje
diska do kraja. U stream režimu se `/data/tmp` uopšte ne dodiruje.

`cache_mb` je time gornja granica, a ne obećanje.

## 12. Config

`/mnt/usbN/.ps5cr.conf`, prvi nađeni pobjeđuje. Bez configa app radi kao i sad, samo
s USB izvorima.

```ini
cache_mb = 4096

[source]
name = qnap
url  = http://<ip-nas>:5000/STRIPOVI/
type = auto          # auto | webdav | autoindex
user = PS5
pass = ...
```

Nepoznat ključ → `LOG` upozorenje i preskok, nikad fatal; stariji config ne obara app.
`pass` se ne pojavljuje ni u jednom logu.

Lozinka stoji u čistom tekstu na USB-u. To je svjesna odluka korisnika; ono što je pod
našom kontrolom je da je ne logujemo i ne šaljemo nigdje osim na konfigurisani host.

## 13. `library.c` — navigacijski stek

```c
typedef struct {
    source_t    *src;                 /* NULL na korijenu */
    char         path[LIB_PATH_MAX];
    char         title[LIB_TITLE_MAX];
    lib_entry_t *entries;
    int          count, sel, scroll;  /* sel/scroll prezivljavaju izlazak i povratak */
} lib_level_t;

int library_init(library_t *l);              /* config -> izvori -> korijen */
int library_enter(library_t *l, int index);  /* u izvor ili folder */
int library_back(library_t *l);              /* 0 ako smo vec u korijenu */
const lib_level_t *library_cur(const library_t *l);
void library_breadcrumb(const library_t *l, char *buf, size_t len);
```

Dubina 0 je sintetički nivo: po jedan `is_dir` red za svaki izvor, ništa se ne lista.
Ulazak zove `be->list()`; rezultat ostaje u nivou dok se iz njega ne izađe, pa je
povratak Krugom instant i ne šalje novi zahtjev. Stek je fiksnih 16 nivoa; dublje se
ne ulazi (`LOG` i ignorisanje). Stari `MAX_DEPTH` nestaje.

Izvori pri startu: za svaki postojeći `/mnt/usbN` jedan `source_usb`, pa za svaki
`[source]` blok jedan `source_http`. `CR_ROOT` ostaje USB izvor na host build-u.

## 14. Stanje čitanja

`.ps5cr_state` ostaje na korijenu prvog USB-a, format nepromijenjen
(`putanja<TAB>stranica`). Ključ je `entry->path`, dakle za mrežu **puni URL** — ne
`/data/tmp` ime, koje nestaje pri LRU izbacivanju.

`state_load()` više ne može upisivati u ravnu listu (nivoi ne postoje pri startu), pa
se učitava jednom u niz u memoriji, a `library` popunjava `last_page` dok gradi nivo.

Posljedica za `main.c`: drži **dvije** putanje — `cur_path` (URL ili USB putanja, ključ
za `state_save`) i `cur_local` (ono što je predano `cache_open`). Za USB i za stream
režim su identične; razlikuju se samo u download režimu.

## 15. UI

`draw_browser()` crta `library_cur()` umjesto `a->lib.items`. Druga linija ekrana je
breadcrumb, skraćen s lijeva (`...Comics/Batman/`) jer je rep informativniji. Folderi
dobijaju `/` na kraju imena i `COL_ACCENT`; font je ASCII, ikone ne dolaze u obzir.
Badge `nastavi` ostaje na fajlovima.

`on_button()`, `SCREEN_BROWSER`: Krst na folderu/izvoru → `library_enter`, na fajlu →
`reader_open`; Krug → `library_back`, a kad vrati 0 → `running = 0`. Footer se mijenja
u `Krug: nazad` osim u korijenu.

Novi `SCREEN_FETCH` za download režim: progress bar, brzina, Krug otkazuje. Stream
režim ga ne koristi — tamo se otvara odmah.

**Nit.** `fetch()` u download režimu blokira dok traje transfer, pa ga `main.c` pokreće
u zasebnoj SDL niti; glavna petlja svaki frejm čita napredak (`SDL_atomic_t` za bajtove
i status) i crta `SCREEN_FETCH`. Krug postavlja `cancel` zastavicu koju `src_progress_fn`
pročita i vrati `!=0`, što curl prekida. Ovo je jedini novi paralelizam — `vfs_http.c`
radi unutar postojeće `cache.c` radne niti i ne uvodi svoju.

**Listanje je sinhrono.** `be->list()` se zove iz glavne petlje i može je nakratko
zamrznuti. Prihvatljivo: PROPFIND jednog foldera na LAN-u je desetine milisekundi, a
gornja granica je `CONNECTTIMEOUT` od 5 s. Ako se u praksi pokaže primjetnim, prelazak
na nit je lokalna izmjena u `main.c` i ne dira `source.h`.

Greška listanja nije pad nego red u nivou:
`greska: 401 Unauthorized - provjeri user/pass u .ps5cr.conf`, Krug vraća nazad.

## 16. Testovi

Svi bez mreže, uz postojeća dva pod ASan/UBSan:

- `tests/test_dav.c` — `tests/fixtures/propfind_stripovi.xml`, snimljen sa stvarnog NAS-a
  (7 unosa: self, `@Recycle`, `index.php`, dva CBR-a, ugniježđen folder, PDF sa zarezom
  i zagradom u imenu) → `lib_entry_t[]`
- `tests/test_html.c` — autoindex fixture → `lib_entry_t[]`
- `tests/test_vfs_http.c` — callback-ovi protiv `tests/range_server.py` (Range podrška;
  `python3 -m http.server` je nema). Provjerava i da adaptivni chunk daje očekivan broj
  zahtjeva iz §5.
- `tests/test_nav.c` — ulazak/izlazak, `sel`/`scroll` pamćenje, granice steka, breadcrumb

## 17. Build

`libcurl` i `libxml2` u `PS5_PKGS` i u host `pkg-config`. `-DCURL_STATICLIB` je obavezan.
Novi fajlovi u `SRCS`: `source_usb.c`, `source_http.c`, `config.c`, `vfs_http.c`.

Rizik iz §6: `Libs.private` u `libcurl.pc` pokazuje na `/opt/ps5-payload-sdk`. Ako
statičko linkovanje povuče te putanje, `LDADD` mora dobiti ispravan `-L` na relocirani
SDK. Rješava se na prvom linkovanju, ne nagađa se unaprijed.

Moguć dodatan link zahtjev: OpenSSL koji curl vuče može tražiti `-lSceSsl` /
`-lSceRandom` uz postojeći `-lSceNet`. Utvrđuje se na prvom linkovanju.

## 18. Rizici

| Rizik | Posljedica | Odgovor |
|---|---|---|
| Otvaranje traje ~57 s | Korisnik čeka pri prvom otvaranju stripa | Prihvaćeno: mjereno je 2.4× brže od downloada na ovoj mreži. Ako zasmeta, sidecar indeks unosa u `/data/tmp` čini svako sljedeće otvaranje trenutnim — zaseban zadatak, ne dio ovog plana |
| PS5 WiFi brži nego izmjereno | Streaming gubi prednost | Prag je 13 MB/s naspram izmjerenih 5.69; download režim ionako ostaje u kodu (§10) i prebacivanje je promjena jednog uslova |
| Solidan komprimovan RAR | Skupo izvlačenje stranice | Detektovati i pasti na download režim |
| `libcurl.pc` relokacija | Ne linkuje se | §17, prvo linkovanje |
| QNAP šalje netipičan PROPFIND XML | Prazan listing | Fixture iz stvarnog odgovora, ne izmišljen |
