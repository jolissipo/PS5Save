ifndef PS5_PAYLOAD_SDK
$(error Set PS5_PAYLOAD_SDK to your installed ps5-payload-sdk directory)
endif
include $(PS5_PAYLOAD_SDK)/toolchain/prospero.mk

CFLAGS := -O2 -Wall -Wextra -Werror
LDLIBS := -lSceFsInternalForVsh -lSceNotification -lSceSystemService -lpthread

all: backup.elf

backup.elf: main.c notification_icon.h backup_progress.h title_activity.h change_index.h dashboard.h dashboard_browser.h dashboard_editor.h ps5save_i18n.h dashboard/index.html
	$(CC) $(CFLAGS) main.c $(LDLIBS) -o $@

# Optional, separate home-screen browser tile installer. Never run by default.
launcher-installer.elf: launcher_installer.c launcher/param.json notification_icon.h
	$(CC) $(CFLAGS) launcher_installer.c -lSceIpmi -lSceAppInstUtil -o $@

.PHONY: all clean test
test:
	sh tests/run-host-tests.sh

clean:
	rm -f backup.elf launcher-installer.elf
