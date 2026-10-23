#---------------------------------------------------------------------------------
# SwitchCast - Makefile (devkitPro / libnx)
#   make            -> gera SwitchCast.nro
#   make DEBUG=1    -> habilita logs via nxlink (nxlink -s SwitchCast.nro)
#---------------------------------------------------------------------------------
.SUFFIXES:

ifeq ($(strip $(DEVKITPRO)),)
$(error "Defina DEVKITPRO no ambiente. Ex.: export DEVKITPRO=/opt/devkitpro")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITPRO)/libnx/switch_rules

TARGET      := SwitchCastCore
BUILD       := build
SOURCES     := src src/common src/player src/network src/ui src/utils src/updater third_party/civetweb
DATA        :=
INCLUDES    := src src/common include third_party third_party/civetweb

APP_TITLE   := SwitchCast
APP_AUTHOR  := Aleff959
APP_VERSION := 2.3.0

ARCH        := -march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIE

# CivetWeb: somente HTTP + WebSocket, sem TLS/CGI/arquivos/Lua.
CIVET_DEFS  := -DNO_SSL -DNO_CGI -DNO_FILES -DNO_CACHING -DUSE_WEBSOCKET

CFLAGS      := -Wall -O3 -ffunction-sections -fdata-sections $(ARCH) $(CIVET_DEFS) $(INCLUDE) -D__SWITCH__
ifeq ($(DEBUG),1)
CFLAGS      += -DSWITCHCAST_DEBUG -g
ASFLAGS     := -g $(ARCH)
LDFLAGS      = -specs=$(DEVKITPRO)/libnx/switch.specs -g $(ARCH) -Wl,--gc-sections
else
ASFLAGS     := $(ARCH)
LDFLAGS      = -specs=$(DEVKITPRO)/libnx/switch.specs $(ARCH) -Wl,--gc-sections
endif
CXXFLAGS    := $(CFLAGS) -std=gnu++20 -Wno-deprecated-declarations

# Ordem importa (linkagem estática). `aarch64-none-elf-pkg-config --static --libs mpv`
# produz uma lista equivalente caso as portlibs mudem.
LIBS        := -lmpv -lavfilter -lpostproc -lavformat -lavcodec -lswscale -lswresample -lavutil \
               -lass -lharfbuzz -lfribidi -lfreetype -lplacebo -larchive -ldav1d \
               -lSDL2 -lEGL -lglapi -ldrm_nouveau -lglad \
               -lcurl -lmbedtls -lmbedx509 -lmbedcrypto -lpng16 -ljpeg -lwebp -lbz2 -llzma -lzstd -llz4 -lz \
               -lnx -lm

LIBDIRS     := $(PORTLIBS) $(LIBNX)

#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT   := $(CURDIR)/$(TARGET)
export TOPDIR   := $(CURDIR)
export VPATH    := $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR  := $(CURDIR)/$(BUILD)

CFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES    := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES      := $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD       := $(CXX)
export OFILES   := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export INCLUDE  := $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
                   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
                   -I$(PORTLIBS)/include/freetype2 \
                   -I$(CURDIR)/$(BUILD)
export LIBPATHS := $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

ifneq ($(wildcard $(CURDIR)/icon.jpg),)
export APP_ICON := $(CURDIR)/icon.jpg
export NROFLAGS += --icon=$(CURDIR)/icon.jpg
endif

# RomFS desacoplado na v2.1.0 para reduzir o Core de ~38MB para ~25MB
# ifneq ($(wildcard $(CURDIR)/romfs),)
# export ROMFS_DIR := $(CURDIR)/romfs
# export NROFLAGS += --romfsdir=$(CURDIR)/romfs
# endif

export NROFLAGS += --nacp=$(CURDIR)/$(TARGET).nacp

.PHONY: $(BUILD) clean all

all: $(BUILD)

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).nro $(TARGET).nacp $(TARGET).elf

#---------------------------------------------------------------------------------
else
.PHONY: all

DEPENDS := $(OFILES:.o=.d)

all: $(OUTPUT).nro

$(OUTPUT).nro: $(OUTPUT).elf $(OUTPUT).nacp $(APP_ICON) $(wildcard $(ROMFS_DIR)/*)
$(OUTPUT).elf: $(OFILES)

-include $(DEPENDS)

#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------
