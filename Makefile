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

cpp-test: crypto-objs
	@$(CXX) $(CXXFLAGS) $(INCLUDE) firmware/kernel/protocol/protocol_test.cpp -o /tmp/corefw_ptest && /tmp/corefw_ptest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) firmware/kernel/Kernel.cpp firmware/kernel/kernel_test.cpp -o /tmp/corefw_ktest && /tmp/corefw_ktest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I $(CRYPTODIR) firmware/kernel/protocol/identity_test.cpp $(OBJDIR)/ed25519/*.o -o /tmp/corefw_idtest && /tmp/corefw_idtest
	@$(CXX) $(CXXFLAGS) $(INCLUDE) -I $(SHA256DIR) firmware/kernel/runtime/runtime_test.cpp $(OBJDIR)/sha256.o -o /tmp/corefw_rtest && /tmp/corefw_rtest

# Compile the vendored orlp/ed25519 and SHA-256 sources as C into build/obj.
crypto-objs:
	@mkdir -p $(OBJDIR)/ed25519
	@cd $(OBJDIR)/ed25519 && $(CC) -O2 -I $(CURDIR)/$(CRYPTODIR) -c $(CURDIR)/$(CRYPTODIR)/*.c
	@$(CC) -O2 -I $(SHA256DIR) -c $(SHA256DIR)/sha256.c -o $(OBJDIR)/sha256.o

# Syntax-check the generated composition roots against the kernel headers.
verify-gen: build
	./$(BIN) build profiles/heltec-v3-repeater.yaml >/dev/null
	./$(BIN) build profiles/wio-tracker-l1-companion.yaml >/dev/null
	$(CXX) $(CXXFLAGS) -fsyntax-only $(INCLUDE) build/heltec-v3-repeater/src/corefw_main.generated.cpp
	$(CXX) $(CXXFLAGS) -fsyntax-only $(INCLUDE) build/wio-tracker-l1-companion/src/corefw_main.generated.cpp
	@echo "generated composition roots compile"

fmt:
	$(GO) fmt ./...

clean:
	rm -rf build $(BIN)
