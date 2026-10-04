CXX ?= c++
PKG_CONFIG ?= pkg-config
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags hyprland lua5.4)
CXXFLAGS ?= -O2
CXXFLAGS += -std=c++23 -fPIC -Wall -Wextra
LDLIBS += $(shell $(PKG_CONFIG) --libs lua5.4)

.PHONY: all clean check-deps
all: check-deps build/hyprpark.so

check-deps:
	$(PKG_CONFIG) --exists hyprland lua5.4

build/hyprpark.so: hyprpark.cpp /usr/include/hyprland/src/version.h
	mkdir -p build
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) -shared $< -o $@ $(LDFLAGS) $(LDLIBS)

clean:
	$(RM) -r build
