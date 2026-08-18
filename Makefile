CXX      ?= g++
PREFIX   ?= /usr/local
CXXFLAGS ?= -std=c++20 -O2 -g -Wall -Wextra -Wpedantic
CXXFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64 -fstack-protector-strong
CPPFLAGS += -I src

# util-linux ships libfdisk as "fdisk.pc" on Fedora/Debian.
ifeq ($(shell pkg-config --exists fdisk && echo yes),yes)
FDISK_PKG := fdisk
else
FDISK_PKG := libfdisk
endif

PKGS     := libudev mount wimlib $(FDISK_PKG)
CXXFLAGS += $(shell pkg-config --cflags $(PKGS))
LDLIBS   += $(shell pkg-config --libs $(PKGS))

SRCS := src/main.cpp src/common.cpp src/devices.cpp src/disk.cpp src/flash.cpp
OBJS := $(SRCS:.cpp=.o)
DEPS := $(OBJS:.o=.d)

.PHONY: all clean install uninstall

all: ezwin

ezwin: $(OBJS)
	$(CXX) $(CXXFLAGS) -o $@ $^ $(LDLIBS)

src/%.o: src/%.cpp
	$(CXX) $(CXXFLAGS) $(CPPFLAGS) -MMD -MP -c -o $@ $<

clean:
	rm -f ezwin $(OBJS) $(DEPS)

install: ezwin
	install -d $(DESTDIR)$(PREFIX)/bin
	install -m 755 ezwin $(DESTDIR)$(PREFIX)/bin/ezwin

uninstall:
	rm -f $(DESTDIR)$(PREFIX)/bin/ezwin

-include $(DEPS)
