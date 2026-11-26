CXX ?= g++
PKG_CONFIG ?= pkg-config
APP := build/configuration_app
SOURCES := src/main.cpp
CPPFLAGS += $(shell $(PKG_CONFIG) --cflags x11 glx gl)
CXXFLAGS ?= -std=c++17 -O2 -Wall -Wextra -Wpedantic
LDLIBS += $(shell $(PKG_CONFIG) --libs x11 glx gl)

.PHONY: all run check clean

all: $(APP)

$(APP): $(SOURCES)
	mkdir -p $(dir $@)
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $< -o $@ $(LDLIBS)

run: $(APP)
	./$(APP)

check: $(APP)
	./$(APP) --check
	./$(APP) --self-test

clean:
	rm -rf build
