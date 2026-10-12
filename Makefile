# fm1_keyboard: the FM-1 as a QMK-style USB keyboard configured with Vial.
#   make fw        the firmware: build/fw-$(FLAVOR)/fm1kbd-$(FLAVOR).bin (JieLi toolchain, felucca-bsp in third_party/)
#   make package   the installable package: build/fw-$(FLAVOR)/fm1kbd-$(FLAVOR).fwsc (+ felucca-bsp's update loader)
#   make release   the release files: build/release/trireme-fm1-v$(VERSION).fwsc + the JieLi SDK's licence
#   FLAVOR=raw (default): keyboard + raw HID (the VIA / Vial channel); FLAVOR=console: keyboard + USB serial console
#   make test      host tests (tests/): the USB descriptors of every variant; kbcore.c (engine + Vial commands);
#                  the screen and the editor; vial-gui's own protocol code against kbcore.c (only when refs/vial-gui
#                  holds a vial-gui checkout)
#   make patch     patches/usb-hid.patch: firmware/usb.c against felucca-bsp's chip/usb.c
#   make screens   the screen test's pictures as PNGs (build/host/screens/; needs Pillow)
#   make font      firmware/font.h from Spleen's BDF files (SPLEEN=dir with spleen-5x8.bdf, spleen-6x12.bdf)
# Paths (README.md, Building): TC = the JieLi Linux toolchain, AC79_SDK = the AC79 SDK (felucca-bsp's
# tools/get_sdk.sh). Set them on the command line, in the environment, or in local.mk (not in git).

-include local.mk
TC      ?= $(HOME)/.jieli/toolchain
BSP     := third_party/felucca-bsp
CC_FW   := $(TC)/pi32v2/bin/clang -target pi32v2
GEN     := build/gen
CFLAGS_FW := -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function -I$(BSP)/chip -I$(BSP)/boards/fm1 \
             -I$(GEN)
FLAVOR  ?= raw
ifeq ($(FLAVOR),console)
CFLAGS_FW += -DKB_CONSOLE=1
else ifeq ($(FLAVOR),raw)
CFLAGS_FW += -DKB_CONSOLE=0
else
$(error FLAVOR is raw or console)
endif
FW      := build/fw-$(FLAVOR)
NAME    := fm1kbd-$(FLAVOR)
FW_OBJ  := $(FW)/crt0.o $(FW)/vec.o $(FW)/isr.o $(FW)/main.o
SRC     := firmware/main.c firmware/usb.c firmware/lcd.c firmware/kbcore.c firmware/store.c firmware/gfx.c \
           firmware/ui.c firmware/editor.c firmware/font.h firmware/keycodes.h firmware/controls.h $(GEN)/vial_def.h

.PHONY: fw package release test patch screens font clean
fw: $(FW)/$(NAME).bin

$(GEN)/vial_def.h: firmware/vial.json tools/vialdef.py
	@mkdir -p $(@D)
	python3 tools/vialdef.py $< $@

$(FW)/crt0.o: $(BSP)/chip/crt0.S
	@mkdir -p $(@D)
	$(CC_FW) -c $< -o $@
$(FW)/vec.o: $(BSP)/chip/fm1_vec.S
	@mkdir -p $(@D)
	$(CC_FW) -c $< -o $@
$(FW)/isr.o: $(BSP)/chip/fm1_isr.S
	@mkdir -p $(@D)
	$(CC_FW) -c $< -o $@
$(FW)/main.o: $(SRC) tools/check_noinit.sh
	@mkdir -p $(@D)
	tools/check_noinit.sh firmware
	$(CC_FW) $(CFLAGS_FW) -c $< -o $@
$(FW)/main.o: Makefile

$(FW)/$(NAME).elf: $(FW_OBJ) firmware/app.ld
	$(TC)/pi32v2/bin/ld -T firmware/app.ld $(FW_OBJ) -o $@          # (no --gc-sections: as Felucca and hwtest link their apps)

$(FW)/$(NAME).bin: $(FW)/$(NAME).elf tools/mkbin.py
	python3 tools/mkbin.py $(TC) $< $@
	$(TC)/common/bin/objdump -d $< > $(FW)/$(NAME).dis

# ---------------------------------------------------------------- the package
PRODUCT := FM-1_961                         # == FELUCCA_ID in firmware/main.c: the installer checks it
AC79_SDK ?= $(HOME)/.jieli/ac79-sdk
LDR     := build/loader

package: $(FW)/$(NAME).fwsc

VERSION := 0.1
REL     := build/release
release: $(FW)/$(NAME).fwsc
	@test "$(FLAVOR)" = raw || { echo "releases are FLAVOR=raw"; exit 1; }
	@mkdir -p $(REL)
	cp $< $(REL)/trireme-fm1-v$(VERSION).fwsc
	cp $(BSP)/LICENSES/Apache-2.0.txt $(REL)/LICENSE-Apache-2.0-JieLi-SDK.txt    # the package holds 3 SDK files
	cd $(REL) && sha256sum trireme-fm1-v$(VERSION).fwsc > trireme-fm1-v$(VERSION).fwsc.sha256

$(LDR)/loader.bin: $(BSP)/loader/* $(BSP)/chip/*
	@mkdir -p $(@D)
	$(CC_FW) -c $(BSP)/loader/crt0_ldr.S -o $(LDR)/crt0_ldr.o
	$(CC_FW) -Os -ffunction-sections -fno-builtin -Wall -Wno-unused-function -I$(BSP)/chip -I$(BSP)/boards/fm1 \
	    -c $(BSP)/loader/loader.c -o $(LDR)/loader.o
	$(TC)/pi32v2/bin/ld --gc-sections -e _start -T $(BSP)/loader/loader.ld $(LDR)/crt0_ldr.o $(LDR)/loader.o -o $(LDR)/loader.elf
	$(TC)/common/bin/objcopy -O binary -j .text $(LDR)/loader.elf $@

$(LDR)/ota.bin: $(LDR)/loader.bin tools/mkloader.py tools/lz4blk.py
	python3 tools/mkloader.py $< $@

$(FW)/$(NAME).fwsc: $(FW)/$(NAME).bin $(LDR)/ota.bin
	@grep -q '"$(strip $(PRODUCT))"' firmware/main.c || { echo "PRODUCT $(PRODUCT) is not FELUCCA_ID in firmware/main.c"; exit 1; }
	AC79_SDK=$(AC79_SDK) python3 $(BSP)/tools/fm1pkg.py $< $(LDR)/ota.bin $@ --product $(strip $(PRODUCT))

# ---------------------------------------------------------------- host tests
# usb.c is compiled from a copy in build/host: its quoted #include "fm1_usb.h" must find the stub, not the
# chip/ header next to the original
CC_HOST ?= gcc
build/host/usb_hid.c: firmware/usb.c
	@mkdir -p $(@D)
	cp $< $@
build/host/usb_bsp.c: $(BSP)/chip/usb.c
	@mkdir -p $(@D)
	cp $< $@
build/host/desc_dump: tests/desc_dump.c tests/stub/fm1_usb.h build/host/usb_hid.c
	$(CC_HOST) -O0 -g -Wall -Wno-unused-function -Itests/stub \
	    '-DUSB_C="../build/host/usb_hid.c"' -o $@ tests/desc_dump.c
build/host/desc_dump_raw: tests/desc_dump.c tests/stub/fm1_usb.h build/host/usb_hid.c
	$(CC_HOST) -O0 -g -Wall -Wno-unused-function -Itests/stub -DTEST_RAW \
	    '-DUSB_C="../build/host/usb_hid.c"' -o $@ tests/desc_dump.c
build/host/desc_dump_bsp: tests/desc_dump.c tests/stub/fm1_usb.h build/host/usb_bsp.c
	$(CC_HOST) -O0 -g -Wall -Wno-unused-function -Itests/stub -DBSP_PRISTINE \
	    '-DUSB_C="../build/host/usb_bsp.c"' -o $@ tests/desc_dump.c

build/host/core_test: tests/core_test.c firmware/kbcore.c firmware/keycodes.h firmware/controls.h $(GEN)/vial_def.h
	$(CC_HOST) -O0 -g -Wall -Wno-unused-function -I$(GEN) -o $@ tests/core_test.c
build/host/core_lib.so: tests/core_lib.c firmware/kbcore.c firmware/keycodes.h firmware/controls.h $(GEN)/vial_def.h
	$(CC_HOST) -O0 -g -shared -fPIC -Wall -Wno-unused-function -I$(GEN) -o $@ tests/core_lib.c
build/host/screen_test: tests/screen_test.c firmware/kbcore.c firmware/gfx.c firmware/ui.c firmware/editor.c firmware/font.h \
                        firmware/controls.h $(GEN)/vial_def.h
	$(CC_HOST) -O0 -g -Wall -Wno-unused-function -I$(GEN) -o $@ tests/screen_test.c
build/host/store_test: tests/store_test.c firmware/store.c firmware/kbcore.c firmware/controls.h $(GEN)/vial_def.h
	$(CC_HOST) -O0 -g -Wall -Wno-unused-function -I$(GEN) -o $@ tests/store_test.c
.venv/bin/python:                           # vial-gui's keycodes need simpleeval (tests/vialgui_test.py)
	python3 -m venv .venv
	.venv/bin/pip install -q simpleeval

VIALGUI := refs/vial-gui/src/main/python
VIALGUI_DEPS := $(if $(wildcard $(VIALGUI)),.venv/bin/python)
test: build/host/desc_dump build/host/desc_dump_raw build/host/desc_dump_bsp build/host/core_test \
      build/host/core_lib.so build/host/store_test build/host/screen_test $(VIALGUI_DEPS)
	build/host/desc_dump > build/host/descriptors.txt
	build/host/desc_dump_raw > build/host/descriptors_raw.txt
	build/host/desc_dump_bsp > build/host/descriptors_bsp.txt
	python3 tests/check_desc.py build/host/descriptors.txt build/host/descriptors_bsp.txt
	python3 tests/check_desc.py --raw build/host/descriptors_raw.txt build/host/descriptors_bsp.txt
	build/host/core_test
	build/host/store_test
	@mkdir -p build/host/screens
	build/host/screen_test
ifneq ($(VIALGUI_DEPS),)
	.venv/bin/python tests/vialgui_test.py build/host/core_lib.so $(VIALGUI)
else
	@echo "vialgui_test skipped: no vial-gui in refs/vial-gui (git clone https://github.com/vial-kb/vial-gui refs/vial-gui)"
endif

# ---------------------------------------------------------------- the patch against felucca-bsp
patch:
	@mkdir -p patches
	diff -u --label a/chip/usb.c --label b/chip/usb.c $(BSP)/chip/usb.c firmware/usb.c > patches/usb-hid.patch; \
	    test $$? -le 1
	@wc -l patches/usb-hid.patch

screens: build/host/screen_test
	@mkdir -p build/host/screens
	build/host/screen_test
	python3 tests/ppm2png.py build/host/screens

SPLEEN ?= third_party/spleen
font:
	python3 tools/mkfont.py $(SPLEEN)/spleen-5x8.bdf $(SPLEEN)/spleen-6x12.bdf firmware/font.h

clean:
	rm -rf build
