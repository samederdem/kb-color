CC      = gcc
CFLAGS  = -O2 -Wall -Wextra

PREFIX  = /usr/local
BINDIR  = $(PREFIX)/bin

UDEV_RULES_DIR  = /etc/udev/rules.d
UDEV_RULES_FILE = $(UDEV_RULES_DIR)/99-gigabyte-kb.rules
UDEV_RULE       = SUBSYSTEM=="hidraw", ATTRS{idVendor}=="1044", \
                  ATTRS{idProduct}=="7a3b", \
                  MODE="0660", GROUP="input"

TARGET = kb-color

.PHONY: all install uninstall clean

all: $(TARGET)

$(TARGET): $(TARGET).c
	$(CC) $(CFLAGS) -o $@ $<

install: $(TARGET)
	install -Dm755 $(TARGET) $(DESTDIR)$(BINDIR)/$(TARGET)
	echo '$(UDEV_RULE)' | sudo tee $(UDEV_RULES_FILE)
	sudo udevadm control --reload-rules
	sudo udevadm trigger

uninstall:
	rm -f $(DESTDIR)$(BINDIR)/$(TARGET)
	sudo rm -f $(UDEV_RULES_FILE)
	sudo udevadm control --reload-rules
	sudo udevadm trigger

clean:
	rm -f $(TARGET)