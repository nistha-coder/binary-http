# BHTP/1 server (bserve) -- build, test and verification targets.
#
#   make              build ./bserve
#   make test         unit tests + integration tests (needs gcc and python3)
#   make test-unit    C unit tests only (needs only a C compiler)
#   make asan         unit tests + server rebuilt with AddressSanitizer/UBSan
#   make clean        remove everything that was built

CC      ?= cc
CFLAGS  ?= -O2 -g
override CFLAGS += -std=c11 -D_XOPEN_SOURCE=700 -Wall -Wextra -Wpedantic -Iinclude
SAN     = -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer

PROTO_SRC  = src/protocol.c src/frame.c src/io.c
SERVER_SRC = src/bserve.c src/server.c src/network.c src/pathres.c $(PROTO_SRC)
HEADERS    = $(wildcard include/*.h)

all: bserve

bserve: $(SERVER_SRC) $(HEADERS)
	$(CC) $(CFLAGS) -o $@ $(SERVER_SRC)

build:
	mkdir -p build

build/test_protocol: tests/test_protocol.c src/protocol.c $(HEADERS) | build
	$(CC) $(CFLAGS) -o $@ tests/test_protocol.c src/protocol.c

build/test_frame: tests/test_frame.c $(PROTO_SRC) $(HEADERS) | build
	$(CC) $(CFLAGS) -o $@ tests/test_frame.c $(PROTO_SRC)

build/test_shortio: tests/test_shortio.c $(PROTO_SRC) $(HEADERS) | build
	$(CC) $(CFLAGS) -o $@ tests/test_shortio.c $(PROTO_SRC) -Wl,--wrap=read -Wl,--wrap=write

build/test_pathres: tests/test_pathres.c src/pathres.c $(HEADERS) | build
	$(CC) $(CFLAGS) -o $@ tests/test_pathres.c src/pathres.c

test-unit: build/test_protocol build/test_frame build/test_shortio build/test_pathres
	./build/test_protocol
	./build/test_frame
	./build/test_shortio
	./build/test_pathres

test-integration: bserve
	python3 tests/test_server.py
	python3 tests/test_examples.py

test: test-unit test-integration
	@echo
	@echo "==== ALL TESTS PASSED ===="

asan: | build
	$(CC) $(CFLAGS) $(SAN) -o build/test_protocol_asan tests/test_protocol.c src/protocol.c
	$(CC) $(CFLAGS) $(SAN) -o build/test_frame_asan tests/test_frame.c $(PROTO_SRC)
	$(CC) $(CFLAGS) $(SAN) -o build/test_shortio_asan tests/test_shortio.c $(PROTO_SRC) -Wl,--wrap=read -Wl,--wrap=write
	$(CC) $(CFLAGS) $(SAN) -o build/test_pathres_asan tests/test_pathres.c src/pathres.c
	$(CC) $(CFLAGS) $(SAN) -o build/bserve_asan $(SERVER_SRC)
	./build/test_protocol_asan
	./build/test_frame_asan
	./build/test_shortio_asan
	./build/test_pathres_asan
	BSERVE=./build/bserve_asan python3 tests/test_server.py
	BSERVE=./build/bserve_asan python3 tests/test_examples.py
	@echo
	@echo "==== ALL TESTS PASSED UNDER ASAN/UBSAN ===="

clean:
	rm -rf build bserve

.PHONY: all test test-unit test-integration asan clean
