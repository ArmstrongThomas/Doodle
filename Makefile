#---------------------------------------------------------------------------------
.SUFFIXES:
#---------------------------------------------------------------------------------

ifeq ($(strip $(DEVKITARM)),)
$(error "Please set DEVKITARM in your environment. export DEVKITARM=<path to>devkitARM")
endif

TOPDIR ?= $(CURDIR)
include $(DEVKITARM)/3ds_rules

#---------------------------------------------------------------------------------
# TARGET is the name of the output
# BUILD is the directory where object files & intermediate files will be placed
# SOURCES is a list of directories containing source code
# DATA is a list of directories containing data files
# INCLUDES is a list of directories containing header files
# GRAPHICS is a list of directories containing graphics files
# GFXBUILD is the directory where converted graphics files will be placed
#   If set to $(BUILD), it will statically link in the converted
#   files as if they were data files.
#
# NO_SMDH: if set to anything, no SMDH file is generated.
# ROMFS is the directory which contains the RomFS, relative to the Makefile (Optional)
# APP_TITLE is the name of the app stored in the SMDH file (Optional)
# APP_DESCRIPTION is the description of the app stored in the SMDH file (Optional)
# APP_AUTHOR is the author of the app stored in the SMDH file (Optional)
# ICON is the filename of the icon (.png), relative to the project folder.
#   If not set, it attempts to use one of the following (in this order):
#     - <Project name>.png
#     - icon.png
#     - <libctru folder>/default_icon.png
#---------------------------------------------------------------------------------
TARGET		:=	$(notdir $(CURDIR))
BUILD		:=	build
WIN_CURDIR	:=	$(shell cygpath -w "$(CURDIR)")
SOURCES		:=	source vendor/mbedtls/library
DATA		:=	data
INCLUDES	:=	include vendor/mbedtls/include
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)
APP_VERSION	?=	1.6.0
CHAT_ENABLED	?=	0
TEST_MODE	?=	0
LOCAL_SERVER_HOST	?=	192.168.1.46
REMOTE_TEST_SERVER_HOST	?=	server2.rpgwo.org
LIVE_SERVER_HOST	?=	doodle.7db.pw
LIVE_SERVER_WS_PORT	?=	443
LIVE_SERVER_WS_PATH	?=	/ws/3ds
LIVE_SERVER_HTTPS_PORT	?=	443
LOCAL_SERVER_PORT	?=	3000
REMOTE_TEST_SERVER_HTTPS_PORT	?=	443
PUBLIC_BUILD_DIR	?=	$(CURDIR)/../Doodle-Server/public/builds

ifeq ($(filter $(TEST_MODE),0 1 2),)
$(error TEST_MODE must be 0 (release), 1 (local test), or 2 (remote test))
endif

ifeq ($(origin SERVER_WS_HOST), undefined)
ifneq ($(origin SERVER_HOST), undefined)
SERVER_WS_HOST	:=	$(SERVER_HOST)
else
ifeq ($(TEST_MODE),1)
SERVER_WS_HOST	:=	$(LOCAL_SERVER_HOST)
else ifeq ($(TEST_MODE),2)
SERVER_WS_HOST	:=	$(REMOTE_TEST_SERVER_HOST)
else
SERVER_WS_HOST	:=	$(LIVE_SERVER_HOST)
endif
endif
endif
ifeq ($(origin SERVER_WS_PORT), undefined)
ifeq ($(TEST_MODE),1)
SERVER_WS_PORT	:=	$(LOCAL_SERVER_PORT)
else ifeq ($(TEST_MODE),2)
SERVER_WS_PORT	:=	$(REMOTE_TEST_SERVER_HTTPS_PORT)
else
SERVER_WS_PORT	:=	$(LIVE_SERVER_WS_PORT)
endif
endif
SERVER_WS_PATH	?=	$(LIVE_SERVER_WS_PATH)
# MSYS2 otherwise rewrites the leading slash in this -D value into a host
# filesystem path before invoking the native devkitARM compiler.
ifeq ($(findstring -DSERVER_WS_PATH=,$(MSYS2_ARG_CONV_EXCL)),)
MSYS2_ARG_CONV_EXCL	:=	$(if $(MSYS2_ARG_CONV_EXCL),$(MSYS2_ARG_CONV_EXCL);)-DSERVER_WS_PATH=
endif
export MSYS2_ARG_CONV_EXCL
ifeq ($(origin SERVER_WS_SECURE), undefined)
ifeq ($(TEST_MODE),1)
SERVER_WS_SECURE	:=	0
else
SERVER_WS_SECURE	:=	1
endif
endif
ifeq ($(origin SERVER_HTTPS_HOST), undefined)
ifneq ($(origin SERVER_HOST), undefined)
SERVER_HTTPS_HOST	:=	$(SERVER_HOST)
else
ifeq ($(TEST_MODE),1)
SERVER_HTTPS_HOST	:=	$(LOCAL_SERVER_HOST)
else ifeq ($(TEST_MODE),2)
SERVER_HTTPS_HOST	:=	$(REMOTE_TEST_SERVER_HOST)
else
SERVER_HTTPS_HOST	:=	$(LIVE_SERVER_HOST)
endif
endif
endif
ifeq ($(origin SERVER_HTTPS_PORT), undefined)
ifeq ($(TEST_MODE),1)
SERVER_HTTPS_PORT	:=	$(LOCAL_SERVER_PORT)
else ifeq ($(TEST_MODE),2)
SERVER_HTTPS_PORT	:=	$(REMOTE_TEST_SERVER_HTTPS_PORT)
else
SERVER_HTTPS_PORT	:=	$(LIVE_SERVER_HTTPS_PORT)
endif
endif

# Release builds update by default; test builds do not. DISABLE_UPDATER remains
# the compatibility override used by the self-update scripts (0 enables it).
ifeq ($(origin DISABLE_UPDATER), undefined)
ifeq ($(TEST_MODE),0)
DISABLE_UPDATER	:=	0
else
DISABLE_UPDATER	:=	1
endif
endif
ifeq ($(filter $(DISABLE_UPDATER),0 1),)
$(error DISABLE_UPDATER must be 0 or 1)
endif
ifeq ($(DISABLE_UPDATER),0)
UPDATER_ENABLED	:=	1
else
UPDATER_ENABLED	:=	0
endif

ifneq ($(TEST_MODE),0)
BUILD_TAG	:=	TEST
APP_BUILD_LABEL	:=	$(APP_VERSION)-test$(TEST_MODE)
APP_TITLE	:=	Collab Doodle TEST
APP_DESCRIPTION	:=	Test $(TEST_MODE) build $(APP_VERSION) for $(SERVER_WS_HOST)
else
BUILD_TAG	:=	RELEASE
APP_BUILD_LABEL	:=	$(APP_VERSION)
APP_TITLE	:=	Collab Doodle v$(APP_VERSION)
APP_DESCRIPTION	:=	Shared canvas drawing for Nintendo 3DS - v$(APP_VERSION)
endif
APP_AUTHOR	:=	Tommy
ICON		:=	icon.png
#ROMFS		:=	romfs
#GFXBUILD	:=	$(ROMFS)/gfx

#---------------------------------------------------------------------------------
# options for code generation
#---------------------------------------------------------------------------------
ARCH	:=	-march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft

CFLAGS	:=	-g -Wall -O2 -mword-relocations \
			-ffunction-sections \
			$(ARCH)

CFLAGS	+=	$(INCLUDE) -D__3DS__ \
			-DAPP_ID=\"collab-doodle\" \
			-DAPP_VERSION=\"$(APP_VERSION)\" \
			-DAPP_BUILD_LABEL=\"$(APP_BUILD_LABEL)\" \
			-DAPP_BUILD_TAG=\"$(BUILD_TAG)\" \
			-DSERVER_WS_HOST=\"$(SERVER_WS_HOST)\" \
			-DSERVER_WS_PORT=\"$(SERVER_WS_PORT)\" \
			-DSERVER_WS_PATH=\"$(SERVER_WS_PATH)\" \
			-DSERVER_WS_SECURE=$(SERVER_WS_SECURE) \
			-DSERVER_HTTPS_HOST=\"$(SERVER_HTTPS_HOST)\" \
			-DSERVER_HTTPS_PORT=\"$(SERVER_HTTPS_PORT)\" \
			-DCHAT_ENABLED=$(CHAT_ENABLED) \
			-DTEST_MODE=$(TEST_MODE) \
			-DUPDATER_ENABLED=$(UPDATER_ENABLED)

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lctru -lm -lz
#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS := $(CTRULIB) $(DEVKITPRO)/portlibs/3ds

# Every compile-affecting mode/host/flag is recorded in a content-stable stamp.
# All objects depend on it, so changing command-line or environment settings
# forces a rebuild without penalizing unchanged incremental builds.
CONFIG_STAMP	:=	$(TOPDIR)/$(BUILD)/.build-config


#---------------------------------------------------------------------------------
# no real need to edit anything past this point unless you need to add additional
# rules for different file extensions
#---------------------------------------------------------------------------------
ifneq ($(BUILD),$(notdir $(CURDIR)))
#---------------------------------------------------------------------------------

export OUTPUT	:=	$(CURDIR)/$(TARGET)
export TOPDIR	:=	$(CURDIR)

export VPATH	:=	$(foreach dir,$(SOURCES),$(CURDIR)/$(dir)) \
			$(foreach dir,$(GRAPHICS),$(CURDIR)/$(dir)) \
			$(foreach dir,$(DATA),$(CURDIR)/$(dir))

export DEPSDIR	:=	$(CURDIR)/$(BUILD)

CFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.c)))
CPPFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.cpp)))
SFILES		:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.s)))
PICAFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.v.pica)))
SHLISTFILES	:=	$(foreach dir,$(SOURCES),$(notdir $(wildcard $(dir)/*.shlist)))
GFXFILES	:=	$(foreach dir,$(GRAPHICS),$(notdir $(wildcard $(dir)/*.t3s)))
BINFILES	:=	$(foreach dir,$(DATA),$(notdir $(wildcard $(dir)/*.*)))

#---------------------------------------------------------------------------------
# use CXX for linking C++ projects, CC for standard C
#---------------------------------------------------------------------------------
ifeq ($(strip $(CPPFILES)),)
#---------------------------------------------------------------------------------
	export LD	:=	$(CC)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
	export LD	:=	$(CXX)
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

#---------------------------------------------------------------------------------
ifeq ($(GFXBUILD),$(BUILD))
#---------------------------------------------------------------------------------
export T3XFILES :=  $(GFXFILES:.t3s=.t3x)
#---------------------------------------------------------------------------------
else
#---------------------------------------------------------------------------------
export ROMFS_T3XFILES	:=	$(patsubst %.t3s, $(GFXBUILD)/%.t3x, $(GFXFILES))
export T3XHFILES		:=	$(patsubst %.t3s, $(BUILD)/%.h, $(GFXFILES))
#---------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------

export OFILES_SOURCES 	:=	$(CPPFILES:.cpp=.o) $(CFILES:.c=.o) $(SFILES:.s=.o)

export OFILES_BIN	:=	$(addsuffix .o,$(BINFILES)) \
			$(PICAFILES:.v.pica=.shbin.o) $(SHLISTFILES:.shlist=.shbin.o) \
			$(addsuffix .o,$(T3XFILES))

export OFILES := $(OFILES_BIN) $(OFILES_SOURCES)

export HFILES	:=	$(PICAFILES:.v.pica=_shbin.h) $(SHLISTFILES:.shlist=_shbin.h) \
			$(addsuffix .h,$(subst .,_,$(BINFILES))) \
			$(GFXFILES:.t3s=.h)

export INCLUDE	:=	$(foreach dir,$(INCLUDES),-I$(CURDIR)/$(dir)) \
			$(foreach dir,$(LIBDIRS),-I$(dir)/include) \
			-I$(CURDIR)/$(BUILD)

export LIBPATHS	:=	$(foreach dir,$(LIBDIRS),-L$(dir)/lib)

export _3DSXDEPS	:=	$(if $(NO_SMDH),,$(OUTPUT).smdh)

ifeq ($(strip $(ICON)),)
	icons := $(wildcard *.png)
	ifneq (,$(findstring $(TARGET).png,$(icons)))
		export APP_ICON := $(TOPDIR)/$(TARGET).png
	else
		ifneq (,$(findstring icon.png,$(icons)))
			export APP_ICON := $(TOPDIR)/icon.png
		endif
	endif
else
	export APP_ICON := $(TOPDIR)/$(ICON)
endif

ifeq ($(strip $(NO_SMDH)),)
	export _3DSXFLAGS += --smdh=$(CURDIR)/$(TARGET).smdh
endif

ifneq ($(ROMFS),)
	export _3DSXFLAGS += --romfs=$(CURDIR)/$(ROMFS)
endif

.PHONY: all clean cia host-tests verify-release-config FORCE

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(CONFIG_STAMP) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
ifeq ($(TEST_MODE),2)
	@mkdir -p "$(PUBLIC_BUILD_DIR)"
	@cp "$(OUTPUT).3dsx" "$(PUBLIC_BUILD_DIR)/CollabDoodle-test-server2.3dsx"
	@echo published remote test 3dsx to "$(PUBLIC_BUILD_DIR)/CollabDoodle-test-server2.3dsx"
endif

verify-release-config: all
	@if [ "$(TEST_MODE)" != "0" ]; then echo "verify-release-config requires TEST_MODE=0"; exit 1; fi
	@if [ "$(UPDATER_ENABLED)" != "1" ]; then echo "release updater is not enabled"; exit 1; fi
	@grep -Fqx 'TEST_MODE=0' "$(CONFIG_STAMP)"
	@grep -Fqx 'DISABLE_UPDATER=0' "$(CONFIG_STAMP)"
	@grep -Fqx 'UPDATER_ENABLED=1' "$(CONFIG_STAMP)"
	@grep -Fqx 'SERVER_WS_HOST=$(LIVE_SERVER_HOST)' "$(CONFIG_STAMP)"
	@grep -Fqx 'SERVER_WS_SECURE=1' "$(CONFIG_STAMP)"
	@grep -Fqx 'SERVER_HTTPS_HOST=$(LIVE_SERVER_HOST)' "$(CONFIG_STAMP)"
	@grep -Fqx 'SERVER_HTTPS_PORT=$(LIVE_SERVER_HTTPS_PORT)' "$(CONFIG_STAMP)"
	@$(DEVKITARM)/bin/arm-none-eabi-strings "$(OUTPUT).elf" | grep -Fq 'DoodleBuildConfig:test=0;updater=1;ws_secure=1;ws=$(LIVE_SERVER_HOST):$(LIVE_SERVER_WS_PORT)$(LIVE_SERVER_WS_PATH);https=$(LIVE_SERVER_HOST):$(LIVE_SERVER_HTTPS_PORT)'
	@echo "verified release config: updater enabled; WSS $(LIVE_SERVER_HOST):$(LIVE_SERVER_WS_PORT)$(LIVE_SERVER_WS_PATH)"

ifeq ($(OS),Windows_NT)
host-tests:
	@powershell -ExecutionPolicy Bypass -File "$(WIN_CURDIR)\scripts\run-host-tests.ps1" -ProjectRoot "$(WIN_CURDIR)"
else
HOST_CXX ?= c++
HOST_TEST_BINARY := $(BUILD)/host-tests/client_fixture_tests
HOST_TEST_SOURCES := tests/client_fixture_tests.cpp source/client_settings.cpp source/input_bindings.cpp source/protocol.cpp source/ui_canvas.cpp source/ui_route.cpp

host-tests:
	@mkdir -p "$(BUILD)/host-tests"
	@$(HOST_CXX) -std=c++11 -Wall -Wextra -pedantic \
		-Itests/stubs -Iinclude -include tests/stubs/host_compat.h \
		$(HOST_TEST_SOURCES) -o "$(HOST_TEST_BINARY)"
	@"$(HOST_TEST_BINARY)"
endif

cia:
	@powershell -ExecutionPolicy Bypass -File "$(WIN_CURDIR)\scripts\build-cia.ps1" -ProjectRoot "$(WIN_CURDIR)" -AppVersion "$(APP_VERSION)" -TestMode $(TEST_MODE) -AppTitle "$(APP_TITLE)" -AppDescription "$(APP_DESCRIPTION)" -AppAuthor "$(APP_AUTHOR)"
ifeq ($(TEST_MODE),2)
	@mkdir -p "$(PUBLIC_BUILD_DIR)"
	@cp "$(OUTPUT).cia" "$(PUBLIC_BUILD_DIR)/CollabDoodle-test-server2.cia"
	@echo published remote test cia to "$(PUBLIC_BUILD_DIR)/CollabDoodle-test-server2.cia"
endif

$(BUILD):
	@mkdir -p $@

FORCE:

$(CONFIG_STAMP): FORCE | $(BUILD)
	@{ printf '%s\n' \
		'APP_VERSION=$(APP_VERSION)' \
		'TEST_MODE=$(TEST_MODE)' \
		'DISABLE_UPDATER=$(DISABLE_UPDATER)' \
		'UPDATER_ENABLED=$(UPDATER_ENABLED)' \
		'APP_BUILD_LABEL=$(APP_BUILD_LABEL)' \
		'APP_BUILD_TAG=$(BUILD_TAG)' \
		'APP_TITLE=$(APP_TITLE)' \
		'APP_DESCRIPTION=$(APP_DESCRIPTION)' \
		'APP_AUTHOR=$(APP_AUTHOR)' \
		'SERVER_WS_HOST=$(SERVER_WS_HOST)' \
		'SERVER_WS_PORT=$(SERVER_WS_PORT)' \
		'SERVER_WS_PATH=$(SERVER_WS_PATH)' \
		'SERVER_WS_SECURE=$(SERVER_WS_SECURE)' \
		'SERVER_HTTPS_HOST=$(SERVER_HTTPS_HOST)' \
		'SERVER_HTTPS_PORT=$(SERVER_HTTPS_PORT)' \
		'CHAT_ENABLED=$(CHAT_ENABLED)' \
		'SOURCES=$(SOURCES)' \
		'DATA=$(DATA)' \
		'INCLUDES=$(INCLUDES)' \
		'CFLAGS=$(CFLAGS)' \
		'CXXFLAGS=$(CXXFLAGS)' \
		'ASFLAGS=$(ASFLAGS)' \
		'LDFLAGS=$(LDFLAGS)' \
		'LIBS=$(LIBS)' \
		'LIBDIRS=$(LIBDIRS)' \
		'NO_SMDH=$(NO_SMDH)' \
		'ROMFS=$(ROMFS)' \
		'_3DSXFLAGS=$(_3DSXFLAGS)' \
		'MSYS2_ARG_CONV_EXCL=$(MSYS2_ARG_CONV_EXCL)'; \
	} > "$@.tmp"
	@if [ -r "$@" ] && cmp -s "$@.tmp" "$@"; then \
		rm -f "$@.tmp"; \
	else \
		mv -f "$@.tmp" "$@"; \
		echo "build configuration changed; rebuilding objects"; \
	fi

ifneq ($(GFXBUILD),$(BUILD))
$(GFXBUILD):
	@mkdir -p $@
endif

ifneq ($(DEPSDIR),$(BUILD))
$(DEPSDIR):
	@mkdir -p $@
endif

#---------------------------------------------------------------------------------
clean:
	@echo clean ...
	@rm -fr $(BUILD) $(TARGET).3dsx $(TARGET).cia $(OUTPUT).smdh $(TARGET).elf $(GFXBUILD)

#---------------------------------------------------------------------------------
$(GFXBUILD)/%.t3x	$(BUILD)/%.h	:	%.t3s
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@tex3ds -i $< -H $(BUILD)/$*.h -d $(DEPSDIR)/$*.d -o $(GFXBUILD)/$*.t3x

#---------------------------------------------------------------------------------
else

#---------------------------------------------------------------------------------
# main targets
#---------------------------------------------------------------------------------
$(OUTPUT).3dsx	:	$(OUTPUT).elf $(_3DSXDEPS)

$(OFILES_SOURCES) : $(HFILES)
$(OFILES_SOURCES) : $(CONFIG_STAMP)

$(OUTPUT).smdh : $(CONFIG_STAMP)

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
%.pem.o	%_pem.h :	%.pem
#---------------------------------------------------------------------------------
	@echo $(notdir $<)
	@$(bin2o)

#---------------------------------------------------------------------------------
.PRECIOUS	:	%.t3x %.shbin
#---------------------------------------------------------------------------------
%.t3x.o	%_t3x.h :	%.t3x
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

#---------------------------------------------------------------------------------
%.shbin.o %_shbin.h : %.shbin
#---------------------------------------------------------------------------------
	$(SILENTMSG) $(notdir $<)
	$(bin2o)

-include $(DEPSDIR)/*.d

#---------------------------------------------------------------------------------------
endif
#---------------------------------------------------------------------------------------
