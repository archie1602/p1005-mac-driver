# p1005-native - native CUPS driver for the HP LaserJet P1005 (XQX)
#
#   make              build filter, PPD and test tools into build/
#   make test         offline test suite (no printer and no HP files needed)
#   make pkg          installer without HP files, downloads the firmware while installing
#   make firmware     get HP's firmware into firmware/HP1005.acl (HP_DMG=path to use a local DMG)
#   make pkg-offline  installer with the firmware inside, for your own Macs only
#   sudo ./install.sh / sudo ./uninstall.sh   (install straight from build/)

INSTALL_DIR ?= /Library/Printers/p1005-native
FW_SRC      ?= firmware/HP1005.acl
HP_DMG      ?=
VERSION      = 1.0.0

CC       = clang
ARCHS    = -arch arm64 -arch x86_64
CFLAGS   = -O2 -mmacosx-version-min=12.0 -Wall -Wextra -Wno-deprecated-declarations
VENDOR   = vendor/foo2zjs
JBIG_SRC = $(VENDOR)/jbig.c $(VENDOR)/jbig_ar.c
B        = build

all: $(B)/rastertoxqx $(B)/p1005-native.ppd $(B)/p1005-native-local.ppd \
     $(B)/testpage-a4.pdf tools

$(B)/testpage-a4.pdf: tools/make_testpage.py | $(B)
	python3 -I tools/make_testpage.py $@

$(B):
	mkdir -p $(B)

# Vendored foo2zjs/JBIG-KIT sources are compiled warning-free as-is (-w).
$(B)/jbig.o: $(VENDOR)/jbig.c | $(B)
	$(CC) $(ARCHS) -O2 -mmacosx-version-min=12.0 -w -c -o $@ $<
$(B)/jbig_ar.o: $(VENDOR)/jbig_ar.c | $(B)
	$(CC) $(ARCHS) -O2 -mmacosx-version-min=12.0 -w -c -o $@ $<

$(B)/rastertoxqx: src/rastertoxqx.c $(B)/jbig.o $(B)/jbig_ar.o
	$(CC) $(ARCHS) $(CFLAGS) -I$(VENDOR) -DFIRMWARE_FILE='"$(INSTALL_DIR)/P1005-firmware.acl"' \
	    -o $@ src/rastertoxqx.c $(B)/jbig.o $(B)/jbig_ar.o -lcups
	codesign --force --sign - --identifier org.p1005-native.rastertoxqx $@

# PPD for installation, and one pointing at build/ for offline tests
$(B)/p1005-native.ppd: ppd/p1005-native.ppd.in | $(B)
	sed 's#@FILTER@#$(INSTALL_DIR)/rastertoxqx#' $< > $@
$(B)/p1005-native-local.ppd: ppd/p1005-native.ppd.in | $(B)
	sed 's#@FILTER@#$(CURDIR)/$(B)/rastertoxqx#' $< > $@

# HP's firmware is not part of this project; see firmware/README.md
firmware: $(FW_SRC)
$(FW_SRC):
	sh tools/fetch-firmware.sh $@ $(HP_DMG)

tools: $(B)/xqxdecode $(B)/arm2hpdl $(B)/fakeprinter $(B)/ras2png

$(B)/xqxdecode: $(VENDOR)/xqxdecode.c $(JBIG_SRC) | $(B)
	$(CC) -O2 -w -I$(VENDOR) -o $@ $(VENDOR)/xqxdecode.c $(JBIG_SRC)
$(B)/arm2hpdl: $(VENDOR)/arm2hpdl.c | $(B)
	$(CC) -O2 -w -o $@ $<
$(B)/fakeprinter: tools/fakeprinter.c | $(B)
	$(CC) $(CFLAGS) -o $@ $< -lcups
$(B)/ras2png: tools/ras2png.c | $(B)
	$(CC) $(CFLAGS) -o $@ $< -lcups -lz

test: all
	sh tests/run_tests.sh

# Installer packages, see pkg/build.sh
PKG         = $(B)/P1005-Driver-$(VERSION).pkg
PKG_OFFLINE = $(B)/P1005-Driver-$(VERSION)-offline.pkg
PKG_DEPS    = all pkg/build.sh pkg/scrub-toc.py pkg/preinstall pkg/postinstall pkg/uninstall.sh \
              pkg/distribution.xml pkg/resources/* tools/fetch-firmware.sh

pkg: $(PKG)
pkg-offline: $(PKG_OFFLINE)

$(PKG): $(PKG_DEPS)
	sh pkg/build.sh $@ $(VERSION)
$(PKG_OFFLINE): $(PKG_DEPS) $(FW_SRC)
	sh pkg/build.sh $@ $(VERSION) $(FW_SRC)

clean:
	rm -rf $(B) tests/out

.PHONY: all tools test pkg pkg-offline firmware clean
