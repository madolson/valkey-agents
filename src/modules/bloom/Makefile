CARGO ?= cargo
# --frozen keeps the build offline and pinned to Cargo.lock and vendor/.
CARGO_FLAGS = --frozen --release --lib

ifeq ($(BUILD_BLOOM),module)
	CRATE_TYPE = cdylib
else
	CRATE_TYPE = staticlib
endif

all:
	$(CARGO) rustc $(CARGO_FLAGS) --crate-type $(CRATE_TYPE)

clean:
	rm -rf target

.PHONY: all clean
