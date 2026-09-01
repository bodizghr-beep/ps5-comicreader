# PS5 Comic Reader

Čitač stripova i PDF-a za PS5 kao ELF payload. Čita sa USB-a.

Formati: **CBZ, CBR, CB7, CBT, ZIP, RAR, 7Z, TAR** odmah, **PDF** uz `WITH_PDF=1`.

---

## Zašto payload, a ne PKG

Fake-signing PKG-a na PS5 nije zrelo. Payload se šalje na `elfldr` preko mreže i
izvrši odmah — nema potpisivanja, instalacije ni tragova u sistemskoj bazi.
Pošto exploit ionako nije trajan, ikona u meniju ne bi ništa donela.

## Arhitektura

```
main.c        petlja, unos sa kontrolera, dva ekrana (lista / čitač)
ui.c          8x8 bitmap font, crtanje
library.c     skener /mnt/usb0..7, pamćenje pozicije čitanja
cache.c       radna nit dekodira, glavna nit radi upload na GPU
doc.h         interfejs backend-a
  doc_archive.c   libarchive + stb_image
  doc_pdf.c       MuPDF (opciono)
```

Ključna odluka: `doc.h` skriva format od ostatka koda. Dodavanje PDF-a ne dira
ni kes ni UI. Druga: dekodiranje ide u zasebnoj niti, jer JPEG od 3000 px zna da
traje 100+ ms i inače bi kidao frejmove.

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
| MuPDF | ne | samo za `WITH_PDF=1` |

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

## Kontrole

**Lista**
| | |
|---|---|
| D-pad ↑↓ | kretanje |
| L1 / R1 | stranica liste |
| Krst | otvori |
| Krug | izlaz |

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
