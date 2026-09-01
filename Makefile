# PS5 Comic Reader
#
#   make                    -> PS5 payload (comicreader.elf), samo arhive
#   make WITH_PDF=1         -> + PDF backend preko MuPDF-a
#   make host               -> build za PC radi testiranja logike
#   make test FILE=x.cbz    -> testovi pod ASan/UBSan
#   make send PS5_HOST=ip   -> salje payload na konzolu
#
# Za PS5 build:  export PS5_PAYLOAD_SDK=/opt/ps5-sdk

# Podrazumevani cilj zavisi od toga da li je SDK prisutan. Bez ovoga bi `make`
# gradio host build, jer je njegov target prvi u fajlu.
ifdef PS5_PAYLOAD_SDK
.DEFAULT_GOAL := all
else
.DEFAULT_GOAL := host
endif

TARGET := comicreader.elf
SRCDIR := src
BUILD  := build

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
	$(SRCDIR)/cache.c \
	$(SRCDIR)/doc.c \
	$(SRCDIR)/doc_archive.c \
	$(SRCDIR)/doc_pdf.c \
	$(SRCDIR)/stb_impl.c

WARN        := -Wall -Wextra -Wno-unused-parameter
BASE_CFLAGS := $(WARN) -O2 -I$(SRCDIR)

# Host build i testovi: zaglavlja iz lokalnog roota (scripts/host_deps.sh),
# linkovanje na sistemske runtime biblioteke preko simlinkova u tom rootu.
# Sistem nema -dev pakete ni root pristup, pa pkg-config za libarchive otpada.
HOSTROOT  ?= $(HOME)/.cache/ps5cr-hostdeps
HOST_LIBDIR := $(HOSTROOT)/root/usr/lib/x86_64-linux-gnu
HOST_INC  := -I$(HOSTROOT)/root/usr/include \
             -I$(HOSTROOT)/root/usr/include/libxml2 \
             -I$(HOSTROOT)/root/usr/include/x86_64-linux-gnu
HOST_LIBS := -L$(HOSTROOT)/root/usr/lib -L$(HOST_LIBDIR) \
             -Wl,-rpath,$(HOST_LIBDIR) \
             -larchive -lxml2 -lcurl -lSDL2

ifdef WITH_PDF
BASE_CFLAGS += -DHAVE_MUPDF
PDF_LIBS    := -lmupdf -lmupdf-third
endif

HAS_WEBP   := $(shell pkg-config --exists libwebp 2>/dev/null && echo 1)
ifeq ($(HAS_WEBP),1)
BASE_CFLAGS += -DHAVE_WEBP
WEBP_PKG   := libwebp
endif
PKG_CFLAGS := $(shell pkg-config --cflags $(WEBP_PKG) 2>/dev/null) $(HOST_INC)
PKG_LIBS   := $(shell pkg-config --libs $(WEBP_PKG) 2>/dev/null) $(HOST_LIBS)

# ======================================================================
# Testovi i host build ne diraju PS5 SDK.
# ======================================================================

host: $(SRCS)
	cc $(BASE_CFLAGS) $(PKG_CFLAGS) -o comicreader $(SRCS) $(PKG_LIBS) $(PDF_LIBS) -lm

TEST_FLAGS := $(BASE_CFLAGS) -g -fsanitize=address,undefined $(PKG_CFLAGS)

test:
	@test -n "$(FILE)" || (echo "zadaj FILE=<putanja do .cbz/.cbr>"; exit 1)
	cc $(TEST_FLAGS) -o /tmp/cr_t_archive tests/test_archive.c \
	   $(SRCDIR)/common.c $(SRCDIR)/doc.c $(SRCDIR)/doc_archive.c \
	   $(SRCDIR)/stb_impl.c $(PKG_LIBS) -lm
	cc $(TEST_FLAGS) -o /tmp/cr_t_cache tests/test_cache.c \
	   $(SRCDIR)/common.c $(SRCDIR)/ui.c $(SRCDIR)/cache.c $(SRCDIR)/doc.c \
	   $(SRCDIR)/doc_archive.c $(SRCDIR)/doc_pdf.c $(SRCDIR)/stb_impl.c $(PKG_LIBS) -lm
	/tmp/cr_t_archive "$(FILE)"
	SDL_VIDEODRIVER=dummy /tmp/cr_t_cache "$(FILE)"

clean:
	rm -rf $(BUILD) $(TARGET) comicreader

.PHONY: host test clean

# ======================================================================
# PS5 build
# ======================================================================

ifdef PS5_PAYLOAD_SDK
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

# Zastavice se ne pogadjaju - SDK-ov pkg-config zna tacan spisak, ukljucujuci
# Sce biblioteke koje SDL2 vuce za video, kontroler i zvuk.
# libwebp postoji u pacbrew paketima, pa se webp stranice podrzavaju na PS5.
# Host build ga ukljucuje samo ako je biblioteka prisutna (vidi nize).
PS5_PKGS   := sdl2 libarchive libwebp libcurl libxml-2.0
# -DCURL_STATICLIB je obavezan: libcurl.pc iz pacbrew paketa ga nosi u Cflags,
# a bez njega se simboli traze kao uvezeni iz DLL-a.
BASE_CFLAGS += -DHAVE_WEBP -DCURL_STATICLIB
PS5_CFLAGS := $(shell $(PKG_CONFIG) --cflags $(PS5_PKGS))
PS5_LIBS   := $(shell $(PKG_CONFIG) --libs $(PS5_PKGS))

CFLAGS += $(BASE_CFLAGS) $(PS5_CFLAGS)
# -nodefaultlibs: prospero-clang po defaultu linkuje -lkernel_web (webkit
# varijanta), a clang driver uz to dodaje -lkernel_stub_weak koji i sam vuce
# libkernel_web. U native_game kontekstu (kako websrv/hbldr pokrece homebrew)
# to daje SIGSYS. Zato se defaulti gase i libkernel_sys se navodi rucno --
# isti spisak koji ima LakeSnes, jedini radni SDL2 homebrew za poredjenje.
LDADD  += -nodefaultlibs $(PS5_LIBS) -lSDL2main $(PDF_LIBS) -lm \
          -lc -lkernel_sys -lSceLibcInternal -lSceNet

OBJS := $(patsubst $(SRCDIR)/%.c,$(BUILD)/%.o,$(SRCS))

all: $(TARGET)

$(BUILD):
	@mkdir -p $(BUILD)

$(BUILD)/%.o: $(SRCDIR)/%.c | $(BUILD)
	$(CC) $(CFLAGS) -c -o $@ $<

$(TARGET): $(OBJS)
	$(CC) -o $@ $(OBJS) $(LDADD)

# elfldr slusa na TCP portu 9021
send: $(TARGET)
	@test -n "$(PS5_HOST)" || (echo "zadaj PS5_HOST=<ip konzole>"; exit 1)
	nc -q0 $(PS5_HOST) 9021 < $(TARGET)

.PHONY: all send
endif
