# corefw developer tasks.
#
# `make test` runs the full suite: the Go tooling tests plus the host-side C++
# wire-compatibility and kernel-lifecycle tests.

GO       ?= go
CXX      ?= c++
CC       ?= cc
CXXFLAGS  = -std=c++17 -Wall -Wextra
INCLUDE   = -I firmware/kernel/include
CRYPTODIR = firmware/drivers/crypto/ed25519
BIN       = corefw
OBJDIR    = build/obj

.PHONY: all build test go-test cpp-test crypto-objs verify-gen tools clean fmt

all: build

# Build the corefw CLI.
build:
	$(GO) build -o $(BIN) ./cmd/corefw

# Run every test.
test: go-test cpp-test

go-test:
	$(GO) test ./...

SHA256DIR = firmware/drivers/crypto/sha256
AESDIR    = firmware/drivers/crypto/aes

TESTDIR = tests/cpp

cpp-test: crypto-objs
	@$(CXX) $(CXXFLAGS) $(INCLUDE) $(TESTDIR)/protocol_test.cpp -o /tmp/corefw_ptest && /tmp/corefw_ptest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) firmware/kernel/Kernel.cpp $(TESTDIR)/kernel_test.cpp -o /tmp/corefw_ktest && /tmp/corefw_ktest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I $(CRYPTODIR) $(TESTDIR)/identity_test.cpp $(OBJDIR)/ed25519/*.o -o /tmp/corefw_idtest && /tmp/corefw_idtest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I $(SHA256DIR) $(TESTDIR)/runtime_test.cpp $(OBJDIR)/sha256.o -o /tmp/corefw_rtest && /tmp/corefw_rtest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I $(SHA256DIR) -I $(AESDIR) $(TESTDIR)/crypto_msg_test.cpp $(OBJDIR)/sha256.o $(OBJDIR)/aes128.o -o /tmp/corefw_cryptotest && /tmp/corefw_cryptotest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I $(SHA256DIR) -I $(AESDIR) -I $(CRYPTODIR) $(TESTDIR)/datagram_test.cpp $(OBJDIR)/sha256.o $(OBJDIR)/aes128.o $(OBJDIR)/ed25519/*.o -o /tmp/corefw_dgtest && /tmp/corefw_dgtest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) $(TESTDIR)/companion_test.cpp -o /tmp/corefw_ctest && /tmp/corefw_ctest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I $(SHA256DIR) -I $(AESDIR) -I $(CRYPTODIR) $(TESTDIR)/commands_test.cpp $(OBJDIR)/sha256.o $(OBJDIR)/aes128.o $(OBJDIR)/ed25519/*.o -o /tmp/corefw_cmdtest && /tmp/corefw_cmdtest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I $(SHA256DIR) -I $(AESDIR) -I $(CRYPTODIR) $(TESTDIR)/storage_test.cpp $(OBJDIR)/sha256.o $(OBJDIR)/aes128.o $(OBJDIR)/ed25519/*.o -o /tmp/corefw_storetest && /tmp/corefw_storetest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I $(SHA256DIR) -I $(AESDIR) -I $(CRYPTODIR) $(TESTDIR)/receiver_test.cpp $(OBJDIR)/sha256.o $(OBJDIR)/aes128.o $(OBJDIR)/ed25519/*.o -o /tmp/corefw_rxtest && /tmp/corefw_rxtest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) $(TESTDIR)/ui_test.cpp -o /tmp/corefw_uitest && /tmp/corefw_uitest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I firmware $(TESTDIR)/gps_test.cpp -o /tmp/corefw_gpstest && /tmp/corefw_gpstest

# Compile the vendored orlp/ed25519 and SHA-256 sources as C into build/obj.
crypto-objs:
	@mkdir -p $(OBJDIR)/ed25519
	@cd $(OBJDIR)/ed25519 && $(CC) -O2 -I $(CURDIR)/$(CRYPTODIR) -c $(CURDIR)/$(CRYPTODIR)/*.c
	@$(CC) -O2 -I $(SHA256DIR) -c $(SHA256DIR)/sha256.c -o $(OBJDIR)/sha256.o
	@$(CC) -O2 -I $(AESDIR) -c $(AESDIR)/aes128.c -o $(OBJDIR)/aes128.o

# Syntax-check the generated composition roots against the kernel headers.
verify-gen: build
	./$(BIN) build profiles/heltec-v3-repeater.yaml --no-compile >/dev/null
	./$(BIN) build profiles/wio-tracker-l1-companion.yaml --no-compile >/dev/null
	$(CXX) $(CXXFLAGS) -fsyntax-only $(INCLUDE) -I $(SHA256DIR) -I $(AESDIR) -I $(CRYPTODIR) build/heltec-v3-repeater/src/corefw_main.generated.cpp
	$(CXX) $(CXXFLAGS) -fsyntax-only $(INCLUDE) -I $(SHA256DIR) -I $(AESDIR) -I $(CRYPTODIR) build/wio-tracker-l1-companion/src/corefw_main.generated.cpp
	@echo "generated composition roots compile"

fmt:
	$(GO) fmt ./...

clean:
	rm -rf build $(BIN)
