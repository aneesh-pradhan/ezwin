# CMake owns the toolchain policy; Make and Ninja use the same build graph.
PREFIX ?= /usr/local

# Do not export GNU Make's built-in CXX=g++; validate explicit overrides.
ifneq ($(origin CXX),default)
export CXX
endif
export CXXFLAGS CPPFLAGS LDFLAGS

.PHONY: all configure test install uninstall clean toolchain
all: configure
	cmake --build --preset llvm
	cmake -E copy_if_different build/llvm/ezwin ezwin

configure:
	cmake --preset llvm -DCMAKE_INSTALL_PREFIX="$(PREFIX)"

test: all
	ctest --preset llvm

install:
	cmake --install build/llvm --prefix "$(PREFIX)"

uninstall:
	rm -f "$(DESTDIR)$(PREFIX)/bin/ezwin" "$(DESTDIR)$(PREFIX)/share/ezwin/uefi-ntfs.img"

clean:
	cmake --build build/llvm --target clean
	cmake -E rm -f ezwin

toolchain:
	bash scripts/install-llvm.sh
