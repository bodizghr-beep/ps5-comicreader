# SETUP — PS5 Comic Reader od nule

Kako se ovaj projekat build-uje, pakuje i pokreće na jailbreak-ovanom PS5.
Pisano za nekoga ko počinje od praznog Linux računara.

Testirano na: PS5 firmware 10.01, Y2JB autoloader + etaHEN, Ubuntu host bez
root privilegija.

---

## 0. Rječnik — tri stvari koje se lako pobrkaju

Ovo je najveći izvor zabune, pa prvo:

| Mehanizam | Šta je | Gdje živi |
|---|---|---|
| **elfldr** (port 9021) | prima sirove payload-e | dio jailbreak-a, gasi se pri restartu |
| **websrv** (port 8080) | launcher za grafički homebrew | payload, čita `/data/homebrew/` |
| **Itemzflow** | launcher za instalirane PKG aplikacije | čita `/user/app/<TITLE_ID>/` + svoju SQLite bazu |

Čitač je **websrv homebrew**, ne PKG aplikacija. Zato se **neće** pojaviti u
Itemzflow meniju — to nije greška, nego druga vrsta paketa.

**Grafička aplikacija se ne može pokrenuti slanjem na elfldr (9021).**
Payload poslan tamo radi u sistemskom procesu koji ne posjeduje video izlaz,
pa `SDL_Init` padne sa `sceVideoOutOpen: Device busy`. Mora ići kroz websrv.

---

## 1. Toolchain bez root-a

Sve ide u `~/ps5-toolchain`, ništa se ne instalira sistemski.

### 1.1 SDK i biblioteke

```bash
mkdir -p ~/ps5-toolchain && cd ~/ps5-toolchain

curl -sSL -o ps5-payload-sdk.zip \
  https://github.com/ps5-payload-dev/sdk/releases/download/v0.43/ps5-payload-sdk.zip
curl -sSL -o pacbrew.tar.gz \
  https://github.com/ps5-payload-dev/pacbrew-repo/releases/download/v0.40.2/ps5-payload-dev.tar.gz

mkdir -p sdk && unzip -q ps5-payload-sdk.zip -d sdk

# pacbrew arhiva ima prefiks opt/ps5-payload-sdk/ -- strip-components=2 je
# spaja u SDK direktorij bez potrebe za /opt (koji nije upisiv bez root-a)
tar xzf pacbrew.tar.gz --strip-components=2 -C sdk/ps5-payload-sdk/
```

### 1.2 clang / lld / llvm bez root-a

SDK-ov `bin/clang` je samo omotač — traži sistemski `llvm-config-NN`
(kandidati: 21, 20, 19, 18, 17, 16, 15). `apt-get download` radi bez root-a,
a `dpkg-deb -x` raspakuje bilo gdje:

```bash
cd ~/ps5-toolchain && mkdir -p debs llvm-root && cd debs

apt-get download \
  clang-21 lld-21 llvm-21 llvm-21-dev \
  libclang-cpp21 libllvm21 llvm-21-linker-tools \
  libclang-common-21-dev

for d in *.deb; do dpkg-deb -x "$d" ~/ps5-toolchain/llvm-root; done
```

> **`libclang-common-21-dev` nije opcion.** U njemu su clang-ova ugrađena
> zaglavlja (`immintrin.h` i ostalo). Bez njega build puca na
> `SDL_cpuinfo.h:113: 'immintrin.h' file not found`, što izgleda kao
> problem sa SDL-om a nije.

### 1.3 Okruženje

```bash
export PS5_PAYLOAD_SDK=$HOME/ps5-toolchain/sdk/ps5-payload-sdk
export PATH="$HOME/ps5-toolchain/llvm-root/usr/bin:$PS5_PAYLOAD_SDK/bin:$PATH"
```

Provjera:

```bash
clang-21 --version          # Ubuntu clang version 21.x
llvm-config-21 --bindir     # .../llvm-root/usr/lib/llvm-21/bin
prospero-pkg-config --exists sdl2 && echo sdl2 ok
```

### 1.4 Test da SDK radi

```bash
cd $PS5_PAYLOAD_SDK/samples/hello_world && make
```

Ako se `hello_world.elf` napravi, toolchain je ispravan.

> Koristi **`prospero-pkg-config`**, ne `prospero-sdl2-config`.
> Ovaj drugi ima ukucane `/opt/ps5-payload-sdk` putanje iz pacbrew paketa i
> na relociranom SDK-u vraća nepostojeće direktorije.

---

## 2. Izmjena u Makefile-u i zašto

Jedina izmjena potrebna za PS5 build je u `LDADD`:

```make
LDADD  += -nodefaultlibs $(PS5_LIBS) -lSDL2main $(PDF_LIBS) -lm \
          -lc -lkernel_sys -lSceLibcInternal -lSceNet
```

### Zašto

`prospero-clang` po defaultu linkuje `-lkernel_web` — webkit varijantu
libkernel-a. Uz to clang driver sam dodaje `-lkernel_stub_weak`, koji i sam
povlači `libkernel_web`. Zato ni eksplicitan `-lkernel_sys` nije dovoljan:
dobiju se **obje** biblioteke.

websrv pokreće homebrew u `native_game` kontekstu. Tamo webkit libkernel daje
neispravne syscall-ove i proces gine na **SIGSYS (signal 12)**, prije ijednog
ispisa — pa izgleda kao pad u dinamičkom linkovanju.

`-nodefaultlibs` gasi SDK-ove podrazumijevane biblioteke, pa se navode ručno.
Referenca za ispravan spisak je **LakeSnes**, jedini SDL2 homebrew koji
pouzdano radi; njegova lista je bila identična ovoj osim libkernel-a.

### Provjera — obavezna poslije svakog build-a

```bash
readelf -d comicreader.elf | grep kernel
```

Mora dati **samo** `libkernel_sys.sprx`. Ako se pojavi i `libkernel_web.sprx`,
binary će pasti na SIGSYS.

---

## 3. Build

```bash
export PS5_PAYLOAD_SDK=$HOME/ps5-toolchain/sdk/ps5-payload-sdk
export PATH="$HOME/ps5-toolchain/llvm-root/usr/bin:$PS5_PAYLOAD_SDK/bin:$PATH"

cd ps5-comicreader
make clean && make
```

Rezultat je `comicreader.elf` (~5.9 MB). `scripts/deps.sh` treba pokrenuti samo
ako `src/stb_image.h` i `src/font8x8_basic.h` nedostaju.

Host build za testiranje logike (ne treba SDK):

```bash
make host
make test FILE=neki.cbz
```

---

## 4. Pakovanje u aplikaciju

websrv traži ovakvu strukturu:

```
/data/homebrew/<Ime>/
    eboot.elf              <- payload, tačno ovo ime
    sce_sys/icon0.png      <- ikona, 512x512 PNG
```

Za ovaj projekat:

```
/data/homebrew/ComicReader/eboot.elf
/data/homebrew/ComicReader/sce_sys/icon0.png
```

Opciono `homebrew.js` u istom direktoriju ako treba proslijediti argumente ili
promijeniti ime/ikonu u meniju — vidi LakeSnes paket kao primjer.

---

## 5. Instalacija na konzolu

etaHEN-ov FTP sluša na **1337**, bez lozinke.

> **Prenos MORA biti binaran.** Ovo je bio prvobitni bug u projektu: prenos u
> tekstualnom modu zamijeni svaki `0x00` bajt sa `0x20` (space). ELF se i dalje
> zove ELF, ali je neupotrebljiv — elfldr ga odbije sa
> `Error reading ELF file`, a etaHEN ga pokrene pa padne na SIGTRAP prije
> `main()`, bez ijedne poruke u logu.
>
> PowerShell tekstualni pipeline i FTP u ASCII modu rade upravo to.

Provjera bilo kog ELF-a, prije i poslije prenosa:

```bash
tr -d -c '\000' < comicreader.elf | wc -c
```

Mora biti **> 0** (za ovaj binary ~495000). Ako je **0**, fajl je korumpiran i
ne može se popraviti — `0x00 → 0x20` je nepovratno, jer se originalni space ne
razlikuje od konvertovanog NUL-a. Prenesi ponovo.

Upload skripta:

```python
#!/usr/bin/env python3
import socket, re, sys, hashlib

HOST = "<ip-konzole>"

def rd(s, t=10):
    s.settimeout(t); out = b""
    try:
        while True:
            d = s.recv(8192)
            if not d: break
            out += d
            if out.endswith(b"\r\n"): break
    except socket.timeout: pass
    return out.decode("utf-8", "replace").strip()

def cmd(s, c, t=10):
    s.sendall((c + "\r\n").encode()); return rd(s, t)

def pasv(s):
    m = re.search(r"\((\d+,\d+,\d+,\d+),(\d+),(\d+)\)", cmd(s, "PASV"))
    return socket.create_connection((HOST, int(m.group(2)) * 256 + int(m.group(3))), timeout=15)

s = socket.create_connection((HOST, 1337), timeout=10)
rd(s); cmd(s, "USER anonymous")
cmd(s, "TYPE I")                      # binarni mod -- bez ovoga se fajl kvari

for d_ in ["/data/homebrew/ComicReader", "/data/homebrew/ComicReader/sce_sys"]:
    cmd(s, f"MKD {d_}")

def put(local, remote):
    data = open(local, "rb").read()
    d = pasv(s); cmd(s, f"STOR {remote}", 5)
    d.sendall(data); d.close()
    print(remote, len(data), rd(s, 30))
    return hashlib.sha256(data).hexdigest()

print("sha256:", put("comicreader.elf", "/data/homebrew/ComicReader/eboot.elf"))
put("icon0.png", "/data/homebrew/ComicReader/sce_sys/icon0.png")
s.close()
```

Provjera integriteta poslije uploada — mora se poklopiti sa gornjim sha256:

```bash
curl -sS "http://<ip-konzole>:8080/fs/data/homebrew/ComicReader/eboot.elf" | sha256sum
```

---

## 6. Pokretanje

### 6.1 Sa konzole, bez PC-a

Treba **Homebrew Loader** — websrv-ov launcher, instaliran kao prava PS5
aplikacija (`FAKE00000`, `contentId: IV9999-FAKE00000_00-HOMEBREWLOADER00`).
Instalira se jednom, iz PKG-a.

1. Podigni jailbreak (Y2JB + etaHEN)
2. websrv mora raditi (vidi 6.3)
3. Home ekran → **Homebrew Loader**
4. U listi izaberi **ComicReader**

### 6.2 Sa PC-a — korisno za debug

```
http://<ip-konzole>:8080/index.html
```

Ili direktno pokretanje uz čitanje `[cr]` ispisa (stdout/stderr payload-a se
vraća kroz HTTP stream):

```bash
curl -N "http://<ip-konzole>:8080/hbldr?pipe=1&daemon=0&path=/data/homebrew/ComicReader/eboot.elf"
```

### 6.3 Podizanje websrv-a

Ručno, poslije svakog restarta:

```bash
curl -sSL https://github.com/ps5-payload-dev/websrv/releases/download/v0.34/websrv-ps5.elf \
  | python3 -c "import sys,socket;d=sys.stdin.buffer.read();s=socket.create_connection(('<ip-konzole>',9021));s.sendall(d);s.shutdown(1)"
```

Ili automatski — etaHEN autoloader. Marker `X.auto_start` govori etaHEN-u da
pri podizanju pokrene `X` iz istog direktorija:

```
/data/etaHEN/payloads/websrv-ps5.elf
/data/etaHEN/payloads/websrv-ps5.elf.auto_start     (prazan fajl)
```

Imena moraju odgovarati tačno. Ako marker pokazuje na nepostojeći fajl,
u `/data/etaHEN/etaHEN_util_daemon.log` se vidi
`payload_readuri: No such file or directory`.

Provjera da je autoloader radio:

```bash
curl -s -o /dev/null -w "%{http_code}\n" http://<ip-konzole>:8080/index.html   # 200
```

---

## 7. Šta preživljava restart

| Preživljava | Gubi se |
|---|---|
| `/data/homebrew/ComicReader/` | elfldr (9021) |
| `/data/etaHEN/payloads/` + markeri | websrv (8080) |
| Homebrew Loader (instalirana app) | klogsrv (3232), shsrv (2323) |

Sam jailbreak se mora podići ručno poslije svakog paljenja. Sve poslije toga
autoloader može odraditi sam.

---

## 8. Zamke koje su nas koštale vremena

**Prenos u tekstualnom modu uništi ELF.** Najskuplja. Simptomi izgledaju kao
greška u kodu: čist build, pad prije `main()`, nijedan `fprintf` u logu,
identičan pad i kod minimalnog test programa. Uvijek provjeri broj NUL bajtova.

**`libkernel_web` daje SIGSYS u native_game kontekstu.** Vidi sekciju 2.

**`SDL_RENDERER_ACCELERATED` ne postoji na PS5.** `SDL_CreateRenderer` vraća
`Couldn't find matching render driver`. Radi tek `SDL_RENDERER_SOFTWARE` —
kod već ima fallback, ali računaj da GPU put nije dostupan.

**`sceVideoOutOpen: Device busy`** znači da si pokušao pokrenuti grafičku
aplikaciju kroz elfldr umjesto kroz websrv. Vidi sekciju 0.

**Itemzflow neće prikazati websrv homebrew.** Različiti mehanizmi. Koristi
Homebrew Loader.

**`scp` sa Windowsa:** pokreni ga u lokalnom PowerShell prozoru, ne unutar SSH
sesije na Linux mašinu. `scp` tumači sve prije prve dvotačke kao ime hosta, pa
`C:\...` postane host `C`.

---

## 9. Provjereno / neprovjereno

Provjereno na konzoli: build, prenos, instalacija, pokretanje kroz websrv,
`SDL_Init` i softverski renderer na 1920x1080, skeniranje `/mnt/usb0..7`.

Neprovjereno: prikaz same stranice stripa (nije bilo USB-a sa CBZ fajlovima u
konzoli), i etaHEN autostart kroz stvarni restart.
