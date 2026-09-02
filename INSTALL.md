# INSTALL — pokretanje na konzoli

Za nekoga ko ima jailbreak-ovan PS5 i samo hoće da čita stripove. Bez build-a,
bez razvojnog okruženja. Za build vidi [`SETUP.md`](SETUP.md).

> **Izdanje još ne postoji.** Linkovi na `releases/latest` niže neće raditi dok
> se prvo izdanje ne objavi. Do tada `eboot.elf` moraš napraviti sam — vidi
> `SETUP.md`.

**Šta je provjereno:** koraci 2, 3 i 4.2 su izvršeni i izmjereni na živoj
konzoli. Koraci 1 i 4.1 su preuzeti iz `SETUP.md` — testirani na PS5 firmware
10.01 sa Y2JB autoloader-om i etaHEN-om, ali traže fizički pristup konzoli pa
ih ovo uputstvo ne može automatski potvrditi. Na drugim firmware-ima nije
provjeravano.

---

## Šta ti treba

- PS5 sa jailbreak-om, na istoj mreži kao računar
- Računar sa `curl`-om (Windows 10/11 ga ima ugrađenog kao `curl.exe`)
- DualSense kontroler — bez njega se aplikacija pokrene, ali se ne može voditi
- IP adresa konzole (Settings → Network → View Connection Status)

Iz izdanja skini dva fajla:

```
https://github.com/bodizghr-beep/ps5-comicreader/releases/latest
```

| fajl | šta je |
|---|---|
| `eboot.elf` | sama aplikacija |
| `icon0.png` | ikona za meni, 512x512 PNG |

U uputstvu niže zamijeni `<ip-konzole>` stvarnom adresom.

---

## 1. Priprema konzole

Prije prenosa moraju raditi dvije stvari:

- **etaHEN** — daje FTP server na portu **1337**, bez lozinke
- **websrv** — launcher za grafički homebrew, port **8080**

Oba se gase pri restartu konzole i moraju se podići poslije svakog paljenja.
Postupak je u [`SETUP.md`, odjeljak 6.3](SETUP.md) — uključujući i kako se
websrv podiže automatski preko etaHEN autoloader-a.

Provjeri da websrv radi:

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://<ip-konzole>:8080/index.html
```

Mora ispisati `200`. Ako ne odgovara, aplikacija se ne može pokrenuti.

---

## 2. Prenos fajlova

> **Prenos mora biti binaran.** FTP u tekstualnom modu zamijeni svaki `0x00`
> bajt razmakom. ELF i dalje izgleda kao ELF, ali padne prije nego što se išta
> ispiše. `curl -T` je uvijek binaran — zato se koristi ovdje, a ne
> PowerShell-ov tekstualni pipeline.

**Linux / macOS:**

```bash
curl -T eboot.elf  ftp://<ip-konzole>:1337/data/homebrew/ComicReader/eboot.elf
curl -T icon0.png  ftp://<ip-konzole>:1337/data/homebrew/ComicReader/sce_sys/icon0.png
```

**Windows (PowerShell ili cmd):**

```powershell
curl.exe -T eboot.elf  ftp://<ip-konzole>:1337/data/homebrew/ComicReader/eboot.elf
curl.exe -T icon0.png  ftp://<ip-konzole>:1337/data/homebrew/ComicReader/sce_sys/icon0.png
```

Očekivana brzina je oko 5 MB/s po žičanoj mreži, dakle nekoliko sekundi za
`eboot.elf`. Struktura na konzoli poslije prenosa mora izgledati ovako:

```
/data/homebrew/ComicReader/
    eboot.elf
    sce_sys/icon0.png
```

---

## 3. Provjera da prenos nije pokvario fajl

Ovaj korak preskaču mnogi, a upravo on razlikuje „ne radi" od „loše prenesen".
websrv servira fajlove nazad, pa se otisak može uporediti sa originalom.

**Linux / macOS:**

```bash
sha256sum eboot.elf
curl -sS "http://<ip-konzole>:8080/fs/data/homebrew/ComicReader/eboot.elf" | sha256sum
```

**Windows:**

```powershell
Get-FileHash eboot.elf -Algorithm SHA256
curl.exe -sS "http://<ip-konzole>:8080/fs/data/homebrew/ComicReader/eboot.elf" -o check.elf
Get-FileHash check.elf -Algorithm SHA256
```

Dva otiska moraju biti ista. Ako se razlikuju, prenos je bio tekstualni —
obriši fajl sa konzole i ponovi korak 2 sa `curl -T`. Pokvaren fajl se ne može
popraviti, jer zamjena `0x00 → 0x20` ne razlikuje originalne razmake.

---

## 4. Pokretanje

### 4.1 Sa konzole

1. Home ekran → **Homebrew Loader**
2. U listi izaberi **ComicReader**

Homebrew Loader je zaseban launcher koji se instalira jednom, iz PKG-a — vidi
`SETUP.md`, odjeljak 6.1.

Čitač se **neće** pojaviti u Itemzflow meniju. To nije greška: Itemzflow
prikazuje instalirane PKG aplikacije, a ovo je websrv homebrew.

### 4.2 Sa računara — korisno kad nešto ne radi

```bash
curl -N "http://<ip-konzole>:8080/hbldr?pipe=1&daemon=0&path=/data/homebrew/ComicReader/eboot.elf"
```

Pokreće aplikaciju i vraća njen ispis u terminal. Uspješan start izgleda ovako:

```
[cr] SDL_Init VIDEO ok
[cr] SDL_Init GAMECONTROLLER ok
[cr] SDL_CreateWindow ok
[cr] nema akcelerisanog renderera (Couldn't find matching render driver), koristim software
[cr] izlaz 1920x1080
[cr] splash: nema /data/homebrew/ComicReader/splash.png, crtam ugradjeni
[cr] izvora: 1
[cr] stanje: 0 zapisa
[cr] kontroler: User1
```

Brojevi zavise od tvoje postavke — `izvora` je broj nađenih USB diskova plus
mrežnih izvora iz konfiguracije, `stanje` je broj zapamćenih pozicija čitanja.
Kontroler se prijavljuje kao `User1`.

Red `nema akcelerisanog renderera` je očekivan, ne greška — PS5 SDL2 build nema
GL kontekst, pa se crta softverski.

---

## 5. Prvi start

Pri pokretanju stoji splash ekran sa imenom i verzijom dok se skenira USB i
čita konfiguracija, najmanje 3,5 sekunde. Zatim se pojavljuje lista izvora.

**Stripovi sa USB-a:** ubaci USB disk u konzolu i stavi fajlove bilo gdje na
njega. Podržano: `cbz cbr cb7 cbt zip rar 7z tar` (i `pdf`, ako je build
napravljen sa `WITH_PDF=1`). Konzola USB montira kao `/mnt/usb0` … `/mnt/usb7`;
svaki nađeni disk je jedan red u listi.

**Stripovi sa mreže** (NAS, WebDAV ili HTTP autoindex) su opcioni — napravi
`.ps5cr.conf` u korijenu USB diska:

```ini
cache_mb = 4096

[source]
name = nas
url  = http://<ip-nas>:5000/STRIPOVI/
type = auto
user = korisnik
pass = lozinka
```

Lozinka stoji u čistom tekstu na USB-u. Ne ispisuje se u logu i ne ulazi u
zapamćene pozicije čitanja, ali ko uzme disk — pročitaće je.

**Sopstveni splash:** stavi `splash.png` u `/data/homebrew/ComicReader/` i
koristiće se umjesto nacrtanog ekrana. Najbolje 1920x1080; drugi odnos stranica
se uklapa sa crnim trakama.

Kontrole su u [`README.md`](README.md#kontrole).

---

## Kad nešto ne radi

| Simptom | Uzrok |
|---|---|
| `curl: (7) Failed to connect ... No route to host` | konzola je ugašena ili nije na mreži |
| `curl: (7) ... Connection refused` na portu 1337 | konzola radi, ali etaHEN nije podignut |
| `index.html` ne vraća 200 | websrv nije podignut — gasi se pri svakom restartu |
| aplikacije nema u Itemzflow meniju | očekivano, nije PKG — traži je u Homebrew Loader-u |
| padne odmah, bez ijedne poruke | pokvaren prenos; uradi korak 3 |
| radi, ali ne reaguje na kontroler | u logu stoji `kontroler nije nadjen`; upari DualSense pa pokreni ponovo |
| port 3232 (klogsrv) ne pokazuje `[cr]` ispis | ispis aplikacije tamo i ne ide; koristi korak 4.2 |
| lista prazna, piše `Nema izvora` | nijedan USB nije montiran i nema `.ps5cr.conf` |

Prvo otvaranje stripa sa mreže traje i do minut — čita se popis stranica. Traka
napretka pokazuje dokle je stiglo, a Krug otkazuje.
