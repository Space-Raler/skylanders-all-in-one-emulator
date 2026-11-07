CC      ?= cc
OBJCOPY ?= objcopy
B       := build
INC     := -Iinclude -Ihook -I$(B)
WARN    := -std=c17 -Wall -Wextra -Werror
FREE    := -DSKY_FREESTANDING -ffreestanding -fno-builtin -fPIC -fvisibility=hidden -fno-plt \
           -fno-stack-protector -fno-asynchronous-unwind-tables -fcf-protection=none -O2 -mno-red-zone

.PHONY: all test blob clean demo prebuilt
all: test

# ---- in-game blob (freestanding, position independent) ----
$(B)/hook.o: hook/usb_hook.c src/sky_portal.c include/*.h hook/sceusbd_abi.h
	@mkdir -p $(B)
	$(CC) $(WARN) $(FREE) $(INC) -c hook/usb_hook.c -o $(B)/usb_hook.o
	$(CC) $(WARN) $(FREE) $(INC) -c src/sky_portal.c -o $(B)/sky_portal_blob.o
	ld -r $(B)/usb_hook.o $(B)/sky_portal_blob.o -o $@

$(B)/hook.elf: $(B)/hook.o hook/hook.ld
	ld -nostdlib -static --no-dynamic-linker -T hook/hook.ld $< -o $@

$(B)/hook_blob.h: $(B)/hook.elf tools/gen_blob.py
	python3 tools/gen_blob.py $(B)/hook.elf $(B)/hook.o $@

blob: $(B)/hook_blob.h

# ---- host tests (blob is executed for real via mmap) ----
HOSTSRC := src/sky_portal.c src/injector.c src/api.c plugin/proc_fake.c
$(B)/test_portal: tests/test_portal.c src/sky_portal.c
	@mkdir -p $(B)
	$(CC) $(WARN) -D_DEFAULT_SOURCE -fsanitize=address,undefined $(INC) $^ -o $@
$(B)/test_hook: tests/test_hook.c $(HOSTSRC) $(B)/hook_blob.h
	$(CC) $(WARN) -D_DEFAULT_SOURCE -fsanitize=address,undefined $(INC) -Iplugin tests/test_hook.c $(HOSTSRC) -o $@
$(B)/test_daemon: tests/test_daemon.c src/daemon.c src/injector.c src/api.c src/sky_portal.c plugin/proc_fake.c $(B)/hook_blob.h
	$(CC) $(WARN) -D_DEFAULT_SOURCE -fsanitize=address,undefined $(INC) -Iplugin tests/test_daemon.c src/daemon.c src/injector.c src/api.c src/sky_portal.c plugin/proc_fake.c -o $@
$(B)/test_api: tests/test_api.c src/api.c src/sky_portal.c
	$(CC) $(WARN) -D_DEFAULT_SOURCE -fsanitize=address,undefined $(INC) -Iplugin $^ -o $@

test: $(B)/test_portal $(B)/test_hook $(B)/test_api $(B)/test_daemon
	$(B)/test_portal && $(B)/test_hook && $(B)/test_api && $(B)/test_daemon

clean:
	rm -rf $(B)

$(B)/web_ui.h: web/index.html tools/embed_ui.py
	@mkdir -p $(B)
	python3 tools/embed_ui.py web/index.html $@

demo: $(B)/web_ui.h $(B)/hook_blob.h
	$(CC) $(WARN) -D_DEFAULT_SOURCE $(INC) -Iplugin plugin/host_demo.c plugin/http_srv.c plugin/proc_fake.c \
	  src/daemon.c src/injector.c src/api.c src/sky_portal.c -lpthread -o $(B)/host_demo

# Pre-generated headers the PS5 CMake build consumes (committed, so the console build needs no host tools).
prebuilt: $(B)/hook_blob.h $(B)/web_ui.h
	@mkdir -p prebuilt
	cp $(B)/hook_blob.h $(B)/web_ui.h prebuilt/
	@echo "prebuilt/ updated"
