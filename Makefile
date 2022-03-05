include NickelHook/NickelHook.mk

override LIBRARY  := libbtpt.so
override SOURCES  += src/btpt.cc src/eventcodes.cc
override CFLAGS   += -Wall -Wextra -Werror
override CXXFLAGS += -Wall -Wextra -Werror -Wno-missing-field-initializers
override LDFLAGS  += -lQt5Core
override PKGCONF  += Qt5Widgets

override MOCS += src/btpt.h

override GENERATED += src/eventcodes_init.h

src/eventcodes.cc: src/eventcodes_init.h

src/eventcodes_init.h:
	sed '/^ *#define *[A-Za-z]/!d; s/^ *#define \([A-Za-z0-9_]*\).*/MAP(\1);/;' \
		< /usr/include/linux/input-event-codes.h > $@

include NickelHook/NickelHook.mk
