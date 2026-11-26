# Host build: the whole IDE on a terminal, plus the test tools.
# The firmware is built with CMake + pico-sdk (see CMakeLists.txt).
CC      ?= cc
CFLAGS  ?= -std=gnu99 -O2 -g -Wall -Wextra -Wno-unused-parameter -fsigned-char
CFLAGS  += -Isrc -Icc
LDLIBS  += -lm
B        = build-host

CORE = src/console.c src/lineedit.c src/config.c src/vfs_common.c src/pf.c src/splash.c src/sound.c src/selftest.c \
       src/vm.c src/ccport.c
POSIX = platform/posix/hal_posix.c platform/posix/vfs_posix.c
APP   = src/shell.c src/editor.c src/tracker.c src/xfer.c src/main.c platform/posix/main_posix.c $(B)/embedded.c
EMBED = $(foreach f,$(wildcard examples/*.c),/calcide/examples/$(notdir $(f))=$(f)) \
        $(foreach f,$(wildcard songs/*.song),/calcide/songs/$(notdir $(f))=$(f)) \
        /calcide/cc/cc.c=cc/cc.c /calcide/cc/ops.h=cc/ops.h /calcide/cc/prelude.h=cc/prelude.h
CCOBJ = $(B)/cc.o

all: $(B)/calcide $(B)/cctool $(B)/fatfs_host

$(B):
	mkdir -p $(B)

# cc.c is written in the Tiny-C dialect; these warnings are about style it
# deliberately keeps simple enough to compile itself.
$(CCOBJ): cc/cc.c cc/ops.h cc/prelude.h cc/ccport.h | $(B)
	$(CC) $(CFLAGS) -Wno-unused-variable -Wno-unused-but-set-variable -Wno-parentheses \
	      -Wno-sign-compare -Wno-missing-field-initializers -c cc/cc.c -o $@

$(B)/embedded.c: tools/embed.py $(wildcard examples/*.c) $(wildcard songs/*.song) cc/cc.c cc/ops.h cc/prelude.h | $(B)
	python3 tools/embed.py $@ $(EMBED)

$(B)/cctool: tools/cctool.c $(CORE) $(POSIX) $(CCOBJ) src/*.h | $(B)
	$(CC) $(CFLAGS) -o $@ tools/cctool.c $(CORE) $(POSIX) $(CCOBJ) $(LDLIBS)

$(B)/calcide: $(APP) $(CORE) $(POSIX) $(CCOBJ) src/*.h | $(B)
	$(CC) $(CFLAGS) -o $@ $(APP) $(CORE) $(POSIX) $(CCOBJ) $(LDLIBS)

FATFS = lib/fatfs/ff.c lib/fatfs/option/ffunicode.c platform/picocalc/vfs_fatfs.c \
        platform/picocalc/diskio.c src/vfs_common.c

$(B)/fatfs_host: tests/fatfs_host.c $(FATFS) | $(B)
	$(CC) -std=gnu99 -O1 -g -w -Isrc -Iplatform/picocalc -Ilib/fatfs/include -o $@ tests/fatfs_host.c $(FATFS)

test: all
	python3 tests/run_tests.py

clean:
	rm -rf $(B)

.PHONY: all test clean
