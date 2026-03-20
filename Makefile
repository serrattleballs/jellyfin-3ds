#---------------------------------------------------------------------------------
# jellyfin-3ds - Native Jellyfin client for Nintendo 3DS
#---------------------------------------------------------------------------------
.SUFFIXES:

#---------------------------------------------------------------------------------
# Environment setup
#---------------------------------------------------------------------------------
ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# Project details
#---------------------------------------------------------------------------------
APP_TITLE	:= Jellyfin 3DS
APP_DESCRIPTION	:= Jellyfin media client for Nintendo 3DS
APP_AUTHOR	:= jellyfin-3ds team
APP_ICON	:= $(TOPDIR)/assets/icons/icon.png

TARGET		:= jellyfin-3ds
BUILD		:= build
SOURCES		:= src src/api src/audio src/video src/ui src/util
INCLUDES	:= include
ROMFS		:= romfs

#---------------------------------------------------------------------------------
# Compiler flags
#---------------------------------------------------------------------------------
ARCH	:= -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:= -g -Wall -O2 -mword-relocations \
		   -ffunction-sections \
		   $(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__ -DJFIN_VERSION=\"0.1.0\"

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++17

ASFLAGS	:= -g $(ARCH)
LDFLAGS	= -specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

#---------------------------------------------------------------------------------
# Libraries
# Order matters: most dependent first
#---------------------------------------------------------------------------------
LIBS	:= -lcitro2d -lcitro3d \
		   -lcurl -lmbedtls -lmbedx509 -lmbedcrypto \
		   -lmpg123 -lopusfile -lopus -lvorbisidec -logg \
		   -lz -lctru -lm

#---------------------------------------------------------------------------------
# Library paths
#---------------------------------------------------------------------------------
LIBDIRS	:= $(CTRULIB) $(PORTLIBS)

#---------------------------------------------------------------------------------
# Automated build rules (standard devkitARM)
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))

export OUTPUT	:= $(CURDIR)/$(TARGET)
export TOPDIR	:= $(CURDIR)

export VPATH	:= $(foreach dir,$(SOURCES),$(CURDIR)/$(dir))
export DEPSDIR	:= $(CURDIR)/$(BUILD)

CFILES		:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:= $(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))

export LD	:= $(CC)

export OFILES_SOURCES := $(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)
export OFILES := $(OFILES_SOURCES)

export INCLUDE	:= $(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
				   $(foreach dir,$(LIBDIRS),-I$(dir)/include) \
				   -I$(CURDIR)/$(BUILD)

export LIBPATHS	:= $(foreach dir,$(LIBDIRS),-L$(dir)/lib)

.PHONY: $(BUILD) clean

$(BUILD):
	@[ -d $@ ] || mkdir -p $@
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile

clean:
	@echo clean ...
	@rm -fr $(BUILD) $(OUTPUT).3dsx $(OUTPUT).elf

else

DEPENDS	:= $(OFILES:.o=.d)

#---------------------------------------------------------------------------------
# Main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx: $(OUTPUT).elf $(OUTPUT).smdh

$(OUTPUT).elf: $(OFILES)

$(OUTPUT).smdh: $(TOPDIR)/Makefile
	smdhtool --create "$(APP_TITLE)" "$(APP_DESCRIPTION)" "$(APP_AUTHOR)" $(APP_ICON) $@

#---------------------------------------------------------------------------------
# Generic build rules
#---------------------------------------------------------------------------------
%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

%.o: %.cpp
	$(CXX) $(CXXFLAGS) -c $< -o $@

-include $(DEPENDS)

endif
