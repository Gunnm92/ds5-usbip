CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -pthread
# Binaire statique : compilé ici (Ubuntu), exécuté aussi dans SteamBox (CachyOS).
LDFLAGS += -static -pthread

SRC := src/dualsense_device.cpp src/usbip_server.cpp src/haptics_rtp.cpp src/main.cpp
OBJ := $(SRC:src/%.cpp=build/%.o)
BIN := build/ds5-usbip-proto
# Outil d'attache pour l'hôte (remplace « usbip attach » s'il est absent).
ATTACH := build/ds5-usbip-attach

all: $(BIN) $(ATTACH)

$(BIN): $(OBJ)
	$(CXX) $(OBJ) -o $@ $(LDFLAGS)

$(ATTACH): build/attach_main.o
	$(CXX) $< -o $@ $(LDFLAGS)

build/%.o: src/%.cpp $(wildcard src/*.hpp) | build
	$(CXX) $(CXXFLAGS) -c $< -o $@

build:
	mkdir -p build

clean:
	rm -rf build

.PHONY: all clean
