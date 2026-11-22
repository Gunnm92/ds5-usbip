CXX ?= g++
CXXFLAGS ?= -O2 -g
CXXFLAGS += -std=c++20 -Wall -Wextra -Wpedantic -Werror -pthread -Isrc
# Static binaries: built here (Ubuntu), also run in SteamBox (CachyOS).
LDFLAGS += -static -pthread

# Library (src/usbip): copied verbatim into other projects, no tool code.
LIB_SRC := src/usbip/dualsense_device.cpp src/usbip/usbip_session.cpp src/usbip/vhci.cpp src/usbip/haptics_rtp.cpp
LIB_OBJ := $(LIB_SRC:%.cpp=build/%.o)
HDRS := $(wildcard src/usbip/*.hpp) $(wildcard src/tools/*.hpp)

# Command-line tools (src/tools): never copied.
BIN_PROTO := build/ds5-usbip-proto
BIN_ATTACH := build/ds5-usbip-attach
BIN_LOCAL := build/ds5-usbip-local
TEST_BIN := build/session_test

all: $(BIN_PROTO) $(BIN_ATTACH) $(BIN_LOCAL)

$(BIN_PROTO): build/src/tools/main.o build/src/tools/usbip_server.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

# Outil d'attache pour l'hôte (remplace « usbip attach » s'il est absent).
$(BIN_ATTACH): build/src/tools/attach_main.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(BIN_LOCAL): build/src/tools/local_main.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

$(TEST_BIN): build/tests/cpp/session_test.o $(LIB_OBJ)
	$(CXX) $^ -o $@ $(LDFLAGS)

build/%.o: %.cpp $(HDRS)
	@mkdir -p $(dir $@)
	$(CXX) $(CXXFLAGS) -c $< -o $@

# Unit tests of the library (socketpair, no framework).
test: $(TEST_BIN)
	./$(TEST_BIN)

# The library must also build as C++23.
check-cxx23:
	@mkdir -p build/c++23
	@for f in $(LIB_SRC); do \
	  echo "c++23: $$f"; \
	  $(CXX) -std=c++23 -O0 -Wall -Wextra -Wpedantic -Werror -pthread -Isrc \
	    -c $$f -o build/c++23/$$(echo $$f | tr / _).o || exit 1; \
	done

clean:
	rm -rf build

.PHONY: all test check-cxx23 clean
