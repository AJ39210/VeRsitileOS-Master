CC ?= gcc
AR ?= ar
RANLIB ?= ranlib

ROOT := ../..
BUILD := $(ROOT)/Kernel/build/graphics
SYSROOT := $(ROOT)/system
ZLIB_SRC := $(CURDIR)/zlib-1.3.2
PNG_SRC := $(CURDIR)/libpng-1.6.59
FT_SRC := $(CURDIR)/freetype-master
OS_LIBC_INCLUDE := $(abspath $(ROOT)/port/libc/include)
OS_LIBC_HEADERS := $(wildcard $(OS_LIBC_INCLUDE)/*.h \
	$(OS_LIBC_INCLUDE)/sys/*.h)

COMMON_CFLAGS := -std=gnu11 \
	-ffreestanding \
	-fPIC \
	-fno-stack-protector \
	-fno-builtin \
	-fno-asynchronous-unwind-tables \
	-fno-unwind-tables \
	-m64 \
	-mno-red-zone \
	-mgeneral-regs-only \
	-mno-sse \
	-mno-sse2 \
	-mno-mmx \
	-msoft-float \
	-Wall \
	-Wextra \
	-O2 \
	-include stddef.h \
	-I$(OS_LIBC_INCLUDE)

ZLIB_SOURCES := adler32.c compress.c crc32.c deflate.c infback.c \
	gzclose.c gzlib.c gzread.c gzwrite.c inffast.c inflate.c \
	inftrees.c trees.c uncompr.c zutil.c
ZLIB_OBJECTS := $(patsubst %.c,$(BUILD)/zlib/%.o,$(ZLIB_SOURCES))

PNG_SOURCES := png.c pngerror.c pngget.c pngmem.c pngpread.c pngread.c \
	pngrio.c pngrtran.c pngrutil.c pngset.c pngtrans.c pngwio.c pngwrite.c \
	pngwtran.c pngwutil.c
PNG_OBJECTS := $(patsubst %.c,$(BUILD)/libpng/%.o,$(PNG_SOURCES))

.PHONY: all libc-headers zlib libpng freetype clean

all: libc-headers zlib libpng freetype

libc-headers:
	mkdir -p $(SYSROOT)/include
	cp -R $(OS_LIBC_INCLUDE)/. $(SYSROOT)/include/

zlib: $(SYSROOT)/lib/libz.a $(SYSROOT)/include/zlib.h $(SYSROOT)/include/zconf.h

$(BUILD)/zlib/%.o: $(ZLIB_SRC)/%.c $(CURDIR)/Makefile $(OS_LIBC_HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) -DHAVE_UNISTD_H=1 -DHAVE_STDARG_H=1 \
		-DHAVE_VSNPRINTF=1 -DZLIB_CONST \
		-I$(ZLIB_SRC) -c $< -o $@

$(BUILD)/libz.a: $(ZLIB_OBJECTS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

$(SYSROOT)/lib/libz.a: $(BUILD)/libz.a
	mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/libpng/include/pnglibconf.h: $(PNG_SRC)/scripts/pnglibconf.h.prebuilt \
	$(CURDIR)/Makefile $(OS_LIBC_HEADERS)
	mkdir -p $(dir $@)
	sed -e '/^#define PNG_FLOATING_POINT_SUPPORTED$$/d' \
		-e '/^#define PNG_FLOATING_ARITHMETIC_SUPPORTED$$/d' \
		-e '/^#define PNG_INCH_CONVERSIONS_SUPPORTED$$/d' \
		-e '/^#define PNG_CONSOLE_IO_SUPPORTED$$/d' \
		-e '/^#define PNG_CONVERT_tIME_SUPPORTED$$/d' \
		-e '/^#define PNG_SIMPLIFIED_READ_SUPPORTED$$/d' \
		-e '/^#define PNG_SIMPLIFIED_WRITE_SUPPORTED$$/d' \
		-e '/^#define PNG_READ_tIME_SUPPORTED$$/d' \
		-e '/^#define PNG_WRITE_tIME_SUPPORTED$$/d' $< > $@

$(BUILD)/libpng/%.o: $(PNG_SRC)/%.c $(BUILD)/libpng/include/pnglibconf.h \
	$(CURDIR)/Makefile $(OS_LIBC_HEADERS)
	mkdir -p $(dir $@)
	$(CC) $(COMMON_CFLAGS) \
		-I$(BUILD)/libpng/include \
		-I$(PNG_SRC) \
		-I$(ZLIB_SRC) \
		-c $< -o $@

$(BUILD)/libpng.a: $(PNG_OBJECTS)
	$(AR) rcs $@ $^
	$(RANLIB) $@

libpng: zlib $(SYSROOT)/lib/libpng.a $(SYSROOT)/include/png.h \
	$(SYSROOT)/include/pngconf.h $(SYSROOT)/include/pnglibconf.h

$(SYSROOT)/lib/libpng.a: $(BUILD)/libpng.a
	mkdir -p $(dir $@)
	cp $< $@

$(SYSROOT)/include/png.h: $(PNG_SRC)/png.h
	mkdir -p $(dir $@)
	cp $< $@

$(SYSROOT)/include/pngconf.h: $(PNG_SRC)/pngconf.h
	mkdir -p $(dir $@)
	cp $< $@

$(SYSROOT)/include/pnglibconf.h: $(BUILD)/libpng/include/pnglibconf.h
	mkdir -p $(dir $@)
	cp $< $@

$(SYSROOT)/include/zlib.h: $(ZLIB_SRC)/zlib.h
	mkdir -p $(dir $@)
	cp $< $@

$(SYSROOT)/include/zconf.h: $(ZLIB_SRC)/zconf.h
	mkdir -p $(dir $@)
	cp $< $@

freetype: $(SYSROOT)/lib/libfreetype.a $(SYSROOT)/include/freetype2/ft2build.h

$(BUILD)/freetype/config.mk: $(CURDIR)/freetype-os/config.mk
	mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/freetype/modules.cfg: $(CURDIR)/freetype-os/modules.cfg
	mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/freetype/ftoption.h: $(FT_SRC)/include/freetype/config/ftoption.h \
	$(CURDIR)/Makefile $(OS_LIBC_HEADERS)
	mkdir -p $(dir $@)
	sed -e '/^#define FT_CONFIG_OPTION_ENVIRONMENT_PROPERTIES$$/d' \
		-e '/^#define FT_CONFIG_OPTION_USE_ZLIB$$/d' \
		-e '/^#define FT_CONFIG_OPTION_SVG$$/d' $< > $@

$(BUILD)/freetype/freetype/config/ftoption.h: $(BUILD)/freetype/ftoption.h
	mkdir -p $(dir $@)
	cp $< $@

$(BUILD)/freetype/libfreetype.a: $(BUILD)/freetype/config.mk \
	$(BUILD)/freetype/modules.cfg $(BUILD)/freetype/ftoption.h \
	$(BUILD)/freetype/freetype/config/ftoption.h \
	$(CURDIR)/Makefile
	$(MAKE) -C $(FT_SRC) \
		TOP_DIR=$(FT_SRC) \
		OBJ_DIR=$(abspath $(BUILD)/freetype) \
		CONFIG_MK=$(abspath $(BUILD)/freetype/config.mk) \
		MODULES_CFG=$(abspath $(BUILD)/freetype/modules.cfg) \
		FTMODULE_H=$(abspath $(BUILD)/freetype/freetype/config/ftmodule.h) \
		CC="$(CC)" AR="$(AR)" \
		CFLAGS="$(COMMON_CFLAGS) -c" \
		single

$(SYSROOT)/lib/libfreetype.a: $(BUILD)/freetype/libfreetype.a
	mkdir -p $(dir $@)
	cp $< $@

$(SYSROOT)/include/freetype2/ft2build.h: $(FT_SRC)/include/ft2build.h \
	$(BUILD)/freetype/libfreetype.a $(BUILD)/freetype/ftoption.h
	mkdir -p $(SYSROOT)/include/freetype2
	cp -R $(FT_SRC)/include/. $(SYSROOT)/include/freetype2/
	cp $(BUILD)/freetype/ftoption.h \
		$(SYSROOT)/include/freetype2/freetype/config/ftoption.h
	cp $(BUILD)/freetype/freetype/config/ftmodule.h \
		$(SYSROOT)/include/freetype2/freetype/config/ftmodule.h

clean:
	rm -rf $(BUILD)
