# PS5 Comic Reader

Čitač stripova i PDF-a za PS5 kao ELF payload. Čita sa USB-a i sa mreže
(WebDAV ili HTTP autoindex).

Formati: **CBZ, CBR, CB7, CBT, ZIP, RAR, 7Z, TAR** odmah, **PDF** uz `WITH_PDF=1`.

---

## Zašto payload, a ne PKG

Fake-signing PKG-a na PS5 nije zrelo. Payload se šalje na `elfldr` preko mreže i
izvrši odmah — nema potpisivanja, instalacije ni tragova u sistemskoj bazi.
Pošto exploit ionako nije trajan, ikona u meniju ne bi ništa donela.

## Arhitektura

```
main.c        petlja, unos sa kontrolera, tri ekrana (lista / čitač / preuzimanje)
ui.c          8x8 bitmap font, crtanje
library.c     navigacijski stek kroz foldere, pamćenje pozicije čitanja
config.c      .ps5cr.conf sa USB-a
source.h      interfejs izvora
  source_usb.c    listanje jednog nivoa preko opendir
  source_http.c   WebDAV/autoindex listanje, izbor stream/download režima
  dav_parse.c     PROPFIND XML (libxml2)
  html_parse.c    autoindex HTML
vfs_http.c    libarchive izvor koji čita HTTP Range zahtjevima
cache.c       radna nit dekodira, glavna nit radi upload na GPU
doc.h         interfejs backend-a
  doc_archive.c   libarchive + stb_image
  doc_pdf.c       MuPDF (opciono)
```

Ključna odluka: `doc.h` skriva format od ostatka koda. Dodavanje PDF-a ne dira
ni kes ni UI. Druga: dekodiranje ide u zasebnoj niti, jer JPEG od 3000 px zna da
traje 100+ ms i inače bi kidao frejmove.

Treća: mrežni strip se **ne preuzima** prije čitanja. `vfs_http.c` daje libarchive-u
Range callback-ove, pa se za popis stranica pročita manje od 1% fajla. Mjereno na CBR-u
od 782 MB sa 504 stranice: otvaranje 49 s i 484 zahtjeva naspram ~160 s koliko bi trajao
pun download preko WiFi-ja. Ključan je `skip` callback, ne `seek` — libarchive preskače
podatke unosa i čita samo zaglavlja.

`doc_archive.c` rešava to što libarchive ne ume nasumično da skoči na N-ti unos —
čitač pamti poziciju, skok unapred preskače zaglavlja, skok unazad ponovo otvara
arhivu. Prefetch je usmeren unapred pa se drugi slučaj retko dešava.

## Build

```sh
scripts/deps.sh                     # dovlači stb_image.h i font8x8_basic.h
export PS5_PAYLOAD_SDK=/opt/ps5-sdk
make                                # -> comicreader.elf
make send PS5_HOST=192.168.1.50     # šalje na elfldr (port 9021)
```

### Zavisnosti koje moraš sam cross-kompajlirati

| Biblioteka | Obavezna | Napomena |
|---|---|---|
| SDL2 | da | postoji kao port u `ps5-payload-dev` |
| libarchive | da | traži zlib, bzip2, xz |
| libcurl | da | mrežni izvor; traži `-DCURL_STATICLIB` |
| libxml2 | da | PROPFIND parser |
| MuPDF | ne | samo za `WITH_PDF=1` |

Za host build i testove zaglavlja se dovlače bez root privilegija:

```sh
scripts/host_deps.sh        # apt-get download + dpkg-deb -x u ~/.cache
make test                   # svi testovi bez mreže, pod ASan/UBSan
```

Ovo je najnepredvidiviji deo posla — otuda `WITH_PDF` flag, da prvi radni build
ne zavisi od MuPDF-a.

## Testiranje na PC-u

Ceo kod osim PS5 build-a radi na Linuxu, pa se logika testira bez konzole:

```sh
make host
CR_ROOT=/putanja/do/stripova ./comicreader

make test FILE=~/strip.cbz          # ASan + UBSan
```

`CR_ROOT` zamenjuje `/mnt/usbN` — na konzoli se ta grana ne aktivira.

Prošlo bez prijava pod AddressSanitizer, UndefinedBehaviorSanitizer i
ThreadSanitizer (CBZ i CBT, listanje napred/nazad i nasumični skokovi).

## Mrežni izvor

Adresa ide u `.ps5cr.conf` u korenu USB-a. Bez tog fajla aplikacija radi kao i ranije,
samo sa USB-om.

```ini
cache_mb = 4096

[source]
name = qnap
url  = http://<ip-nas>:5000/STRIPOVI/
type = auto          # auto | webdav | autoindex
user = PS5
pass = tajna
```

`auto` prvo proba WebDAV `PROPFIND`, pa na `405`/`501` pada na HTTP autoindex stranicu.
Lozinka stoji u čistom tekstu na USB-u; ne loguje se i ne ulazi u URL, pa ne završava ni
u `.ps5cr_state`.

Arhive se čitaju direktno preko mreže. PDF i serveri bez `Range` podrške idu kroz pun
download u `/data/tmp/ps5cr/`, uz progres, otkazivanje Krugom i nastavak prekinutog
preuzimanja. Keš je ograničen na manje od `cache_mb` i četvrtine slobodnog prostora.

## Kontrole

**Lista**
| | |
|---|---|
| D-pad ↑↓ | kretanje |
| L1 / R1 | stranica liste |
| Krst | uđi u folder ili otvori strip |
| Krug | nazad iz foldera; u korenu izlaz |

**Čitač**
| | |
|---|---|
| R1 / D-pad → / Krst | sledeća |
| L1 / D-pad ← | prethodna |
| D-pad ↑↓ | ±10 stranica |
| L2 / R2 | zum |
| levi stik | pomeranje kad je uvećano |
| Trougao | režim uklapanja (ekran / širina / visina) |
| Krug | nazad na listu |
| Options | izlaz |

Pozicija čitanja se pamti u `.ps5cr_state` u korenu USB-a.

## Poznata ograničenja

- WebP i AVIF stranice nisu podržane (stb_image ih ne dekodira) — traži libwebp
- Font je ASCII; naša slova se crtaju kao `?`
- PDF se renderuje na fiksnu visinu od 1800 px, bez re-rendera pri zumu, pa
  jak zum na PDF-u omekša tekst
- Nema sortiranja liste po datumu ni pretrage
- Prvo otvaranje mrežnog stripa traje oko minut (popis od 500 stranica je ~484 zahtjeva,
  a QNAP-ov DAV vhost drži KeepAlive isključen pa svaki plaća novu vezu). Svako sledeće
  otvaranje istog stripa dok aplikacija radi je trenutno, jer se zaglavlja keširaju.
- Paralelni zahtevi ne pomažu — mereno, osam veza je 2.5× sporije od jedne na TS-228
