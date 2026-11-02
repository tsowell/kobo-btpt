NH_QT_MAJOR ?= 5
NICKELHOOK ?= NickelHook/
include $(NICKELHOOK)NickelHook.mk

override LIBRARY  := libbtpt.so
override SOURCES  += src/btpt.cc src/eventcodes.cc
override CFLAGS   += -Wall -Wextra -Werror
override CXXFLAGS += -Wall -Wextra -Werror -Wno-missing-field-initializers
ifeq ($(NH_QT_MAJOR),5)
override LDFLAGS  += -lQt5Core
endif
override PKGCONF  += Qt$(NH_QT_MAJOR)Widgets

override MOCS += src/btpt.h

override GENERATED += src/eventcodes_init.h

src/eventcodes.o: | src/eventcodes_init.h

# Use the header the target compiler sees, not the build host's.
src/eventcodes_init.h:
	echo '#include <linux/input-event-codes.h>' | $(CC) -E -dM -x c - | \
		sed '/^#define [A-Z][A-Z0-9_]* /!d; s/^#define \([A-Z0-9_]*\).*/MAP(\1);/;' | \
		sort > $@

include $(NICKELHOOK)NickelHook.mk
