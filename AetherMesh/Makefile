/.PHONY: all crypto daemon client clean

CRYPTO_LIB_DIR := crypto/zig-out/lib
DAEMON_BUILD_DIR := daemon/build
BIN_DIR := bin

all: crypto daemon client

crypto:
	@echo "==> Building Zig crypto library..."
	cd crypto && zig build -Doptimize=ReleaseSafe
	@echo "==> libaethercrypto.so built"

daemon: crypto
	@echo "==> Building C++ daemon..."
	mkdir -p $(DAEMON_BUILD_DIR)
	cd $(DAEMON_BUILD_DIR) && cmake .. -DCMAKE_BUILD_TYPE=Release && make -j4
	@echo "==> aetherd built"

client:
	@echo "==> Building Go client..."
	mkdir -p $(BIN_DIR)
	cd client && go build -o ../$(BIN_DIR)/aether-cli ./...
	@echo "==> aether-cli built"

clean:
	rm -rf crypto/zig-out crypto/.zig-cache
	rm -rf $(DAEMON_BUILD_DIR)
	rm -rf $(BIN_DIR)
	@echo "==> Clean done"
