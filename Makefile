PREFIX        ?= $(HOME)/.local
DEST          ?= $(PREFIX)
NETCDF_PREFIX ?= $(shell brew --prefix netcdf 2>/dev/null || echo /usr)
PNG_PREFIX    ?= $(shell brew --prefix libpng 2>/dev/null || echo /usr)
X11_PREFIX    ?= /opt/X11

CC      ?= cc
CSTD    ?= -std=c99
CFLAGS  ?= -O2 -Wall -Wextra $(CSTD)
CFLAGS  += -I$(NETCDF_PREFIX)/include -I$(PNG_PREFIX)/include
LDFLAGS += -L$(NETCDF_PREFIX)/lib -L$(PNG_PREFIX)/lib
LIBS     = -lnetcdf -lpng -lm

# Where the bundled coast/borders/states overlays get installed. The binary
# uses this as the default search path when --coast-data is given.
DATADIR ?= $(DEST)/share/unsview
CFLAGS  += -DUNSVIEW_DATA_DIR=\"$(DATADIR)\"

# X11 detection / configuration.
#
# Each piece (Xaw, Xt, X11, Xmu, Xext, SM, ICE) can live in its own prefix on
# HPC nodes, so we accept per-library prefix vars. The "umbrella" X11_PREFIX
# is the default for any unset piece.
#
#   make X11_PREFIX=/opt/x11                            # everything in one tree
#   make XAW_PREFIX=/sw/xaw XT_PREFIX=/sw/xt \         # split prefixes
#        X11_PREFIX=/sw/libX11 XMU_PREFIX=/sw/xmu \
#        XEXT_PREFIX=/sw/xext
#
# X11_EXTRA_CFLAGS / X11_EXTRA_LDFLAGS let you tack on anything else.
# Set HAVE_X11=1 explicitly to bypass auto-detection (force GUI build).

XAW_PREFIX    ?= $(X11_PREFIX)
XT_PREFIX     ?= $(X11_PREFIX)
XMU_PREFIX    ?= $(X11_PREFIX)
XEXT_PREFIX   ?= $(X11_PREFIX)
SM_PREFIX     ?= $(X11_PREFIX)
ICE_PREFIX    ?= $(X11_PREFIX)
# xproto / xorgproto provides X11/X.h, X11/Xfuncproto.h, etc. Often a
# header-only spack/distro package installed under its own prefix.
XPROTO_PREFIX ?= $(X11_PREFIX)

# Build the include/lib path lists, dropping duplicates.
X11_INC_DIRS := $(sort $(XAW_PREFIX)/include $(XT_PREFIX)/include \
                       $(X11_PREFIX)/include $(XMU_PREFIX)/include \
                       $(XEXT_PREFIX)/include $(SM_PREFIX)/include \
                       $(ICE_PREFIX)/include $(XPROTO_PREFIX)/include)
X11_LIB_DIRS := $(sort $(XAW_PREFIX)/lib $(XT_PREFIX)/lib \
                       $(X11_PREFIX)/lib $(XMU_PREFIX)/lib \
                       $(XEXT_PREFIX)/lib $(SM_PREFIX)/lib \
                       $(ICE_PREFIX)/lib)

# Auto-detect Xaw header + lib unless the caller set HAVE_X11 explicitly.
# HAVE_X11=1 forces the GUI on, HAVE_X11=0 forces it off; testing `origin`
# rather than the value is what makes the "off" case work at all. The conda
# recipe passes HAVE_X11=1 so that a missing Xaw fails the build loudly instead
# of quietly producing a headless package that still installs and still runs.
ifeq ($(origin HAVE_X11),undefined)
    X11_HDR := $(firstword $(wildcard \
        $(XAW_PREFIX)/include/X11/Xaw/Form.h \
        $(X11_PREFIX)/include/X11/Xaw/Form.h \
        /usr/include/X11/Xaw/Form.h \
        /usr/include/X11/Xaw3d/Form.h))
    X11_LIB := $(firstword $(wildcard \
        $(XAW_PREFIX)/lib/libXaw.* \
        $(X11_PREFIX)/lib/libXaw.* \
        /usr/lib/libXaw.* /usr/lib64/libXaw.* \
        /usr/lib/x86_64-linux-gnu/libXaw.* \
        /usr/lib/aarch64-linux-gnu/libXaw.*))
    ifneq ($(X11_HDR),)
    ifneq ($(X11_LIB),)
        HAVE_X11 := 1
    endif
    endif
endif

ifeq ($(HAVE_X11),1)
    CFLAGS  += $(addprefix -I,$(wildcard $(X11_INC_DIRS))) $(X11_EXTRA_CFLAGS)
    LDFLAGS += $(addprefix -L,$(wildcard $(X11_LIB_DIRS))) $(X11_EXTRA_LDFLAGS)
    CFLAGS  += -DUNSVIEW_HAVE_X11
    LIBS    += -lXaw -lXmu -lXt -lXext -lX11 -lSM -lICE
    GUI_OBJ  = src/gui_x11.o
endif

CORE_OBJ = src/main.o src/nc_io.o src/mesh.o src/raster.o src/colormap.o src/png_export.o src/polylines.o
OBJS     = $(CORE_OBJ) $(GUI_OBJ)

all: unsview
	@echo "build done. X11 GUI: $(if $(filter 1,$(HAVE_X11)),enabled,disabled)"
	@$(if $(filter 1,$(HAVE_X11)),:,\
	  echo "" && \
	  echo "  The GUI is unsview's default mode, and this build does not have it." && \
	  echo "  Only headless output (-o PATH) will work. To get the GUI:" && \
	  echo "    conda install -c conda-forge unsview     # ships the GUI on Linux and macOS" && \
	  echo "    or install X11/Xaw headers and rebuild   # see INSTALL_HPC.md" && \
	  echo "")

unsview: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBS)

src/%.o: src/%.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) unsview

install: unsview
	mkdir -p $(DEST)/bin $(DATADIR) $(DATADIR)/samples
	cp unsview $(DEST)/bin/
	@if [ -d samples ]; then \
		for f in samples/coastlines_110m.txt samples/borders_50m.txt samples/states_50m.txt; do \
			[ -f $$f ] && cp $$f $(DATADIR)/ ; \
		done ; \
	fi
	@# The synthetic fixtures ship with the package (430 KB for all six). Without
	@# them an installed unsview has nothing to verify itself against, and every
	@# fixture command in README.md would need a source checkout to run.
	@if [ -d samples ]; then \
		for f in samples/synthetic*.nc; do \
			[ -f $$f ] && cp $$f $(DATADIR)/samples/ ; \
		done ; \
	fi
	@echo "installed binary  -> $(DEST)/bin/unsview"
	@echo "installed overlays-> $(DATADIR)/"
	@echo "installed samples -> $(DATADIR)/samples/"

.PHONY: all clean install
