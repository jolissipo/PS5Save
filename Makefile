ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to your installed ps5-payload-sdk directory)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS := -O2 -Wall -Wextra -Werror
LDLIBS := -lSceFsInternalForVsh -lSceNotification -lSceSystemService -lSceIpmi -lSceAppInstUtil -lpthread

all: ps5saveplus.elf

ps5saveplus.elf: main.c launcher_installer.c launcher/param.json notification_icon.h usb_storage.h usb_migrate.h backup.conf backup_progress.h title_activity.h change_index.h dashboard.h dashboard_browser.h dashboard_editor.h ps5save_i18n.h dashboard/index.html VERSION
	$(CC) $(CFLAGS) main.c $(LDLIBS) -o $@

.PHONY: all clean test
test:
	sh tests/run-host-tests.sh

clean:
	rm -f ps5saveplus.elf
