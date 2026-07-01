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
SOURCES		:=	source
DATA		:=	data
INCLUDES	:=	include
GRAPHICS	:=	gfx
GFXBUILD	:=	$(BUILD)
APP_VERSION	?=	1.3.5
CHAT_ENABLED	?=	0
TEST_MODE	?=	0
LOCAL_SERVER_HOST	?=	192.168.1.46
REMOTE_TEST_SERVER_HOST	?=	server2.rpgwo.org
LIVE_SERVER_HOST	?=	doodle.7db.pw
LIVE_SERVER_TCP_HOST	?=	tcp.doodle.7db.pw
LIVE_SERVER_HTTP_HOST	?=	server1.rpgwo.org
LOCAL_SERVER_HTTP_PORT	?=	3000
REMOTE_TEST_SERVER_HTTP_PORT	?=	3000
LIVE_SERVER_HTTP_PORT	?=	80
PUBLIC_BUILD_DIR	?=	$(CURDIR)/../Doodle-Server/public/builds
ifeq ($(origin SERVER_TCP_HOST), undefined)
ifneq ($(origin SERVER_HOST), undefined)
SERVER_TCP_HOST	:=	$(SERVER_HOST)
else
ifeq ($(TEST_MODE),1)
SERVER_TCP_HOST	:=	$(LOCAL_SERVER_HOST)
else ifeq ($(TEST_MODE),2)
SERVER_TCP_HOST	:=	$(REMOTE_TEST_SERVER_HOST)
else
SERVER_TCP_HOST	:=	$(LIVE_SERVER_TCP_HOST)
endif
endif
endif
ifeq ($(origin SERVER_HTTP_HOST), undefined)
ifneq ($(origin SERVER_HOST), undefined)
SERVER_HTTP_HOST	:=	$(SERVER_HOST)
else
ifeq ($(TEST_MODE),1)
SERVER_HTTP_HOST	:=	$(LOCAL_SERVER_HOST)
else ifeq ($(TEST_MODE),2)
SERVER_HTTP_HOST	:=	$(REMOTE_TEST_SERVER_HOST)
else
SERVER_HTTP_HOST	:=	$(LIVE_SERVER_HTTP_HOST)
endif
endif
endif
SERVER_TCP_PORT	?=	3030
ifeq ($(origin SERVER_HTTP_PORT), undefined)
ifeq ($(TEST_MODE),1)
SERVER_HTTP_PORT	:=	$(LOCAL_SERVER_HTTP_PORT)
else ifeq ($(TEST_MODE),2)
SERVER_HTTP_PORT	:=	$(REMOTE_TEST_SERVER_HTTP_PORT)
else
SERVER_HTTP_PORT	:=	$(LIVE_SERVER_HTTP_PORT)
endif
endif
DISABLE_UPDATER	?=	$(TEST_MODE)
ifneq ($(TEST_MODE),0)
BUILD_TAG	:=	TEST
APP_BUILD_LABEL	:=	$(APP_VERSION)-test$(TEST_MODE)
APP_TITLE	:=	Collab Doodle TEST
APP_DESCRIPTION	:=	Test $(TEST_MODE) build $(APP_VERSION) for $(SERVER_TCP_HOST)
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
			-DSERVER_HOST=\"$(SERVER_TCP_HOST)\" \
			-DSERVER_TCP_HOST=\"$(SERVER_TCP_HOST)\" \
			-DSERVER_HTTP_HOST=\"$(SERVER_HTTP_HOST)\" \
			-DSERVER_TCP_PORT=\"$(SERVER_TCP_PORT)\" \
			-DSERVER_HTTP_PORT=\"$(SERVER_HTTP_PORT)\" \
			-DCHAT_ENABLED=$(CHAT_ENABLED) \
			-DTEST_MODE=$(DISABLE_UPDATER)

CXXFLAGS	:= $(CFLAGS) -fno-rtti -fno-exceptions -std=gnu++11

ASFLAGS	:=	-g $(ARCH)
LDFLAGS	=	-specs=3dsx.specs -g $(ARCH) -Wl,-Map,$(notdir $*.map)

LIBS	:= -lctru -lm -lz
#---------------------------------------------------------------------------------
# list of directories containing libraries, this must be the top level containing
# include and lib
#---------------------------------------------------------------------------------
LIBDIRS := $(CTRULIB) $(DEVKITPRO)/portlibs/3ds


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

.PHONY: all clean cia

#---------------------------------------------------------------------------------
all: $(BUILD) $(GFXBUILD) $(DEPSDIR) $(ROMFS_T3XFILES) $(T3XHFILES)
	@$(MAKE) --no-print-directory -C $(BUILD) -f $(CURDIR)/Makefile
ifeq ($(TEST_MODE),2)
	@mkdir -p "$(PUBLIC_BUILD_DIR)"
	@cp "$(OUTPUT).3dsx" "$(PUBLIC_BUILD_DIR)/CollabDoodle-test-server2.3dsx"
	@echo published remote test 3dsx to "$(PUBLIC_BUILD_DIR)/CollabDoodle-test-server2.3dsx"
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

$(OUTPUT).elf	:	$(OFILES)

#---------------------------------------------------------------------------------
# you need a rule like this for each extension you use as binary data
#---------------------------------------------------------------------------------
%.bin.o	%_bin.h :	%.bin
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
