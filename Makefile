# libwce — Wavelet Coefficient Entropy Codec
#
# Targets:
#   make            build libwce.a
#   make test       build and run unit tests
#   make demos      build the three demo binaries
#   make bench      build and run the benchmark harness
#   make fuzz       1M random inputs under ASan + UBSan (standalone)
#   make clean      remove build artifacts

CC      ?= cc
CFLAGS  ?= -std=c99 -O2 -Wall -Wextra -Wpedantic
AR      ?= ar

# Debug override: make DEBUG=1
ifeq ($(DEBUG),1)
    CFLAGS := $(filter-out -O2,$(CFLAGS)) -O0 -g -fsanitize=address,undefined
    LDFLAGS += -fsanitize=address,undefined
endif

SRC_DIR  = src
INCLUDES = -I$(SRC_DIR)

LIBRARY  = libwce.a
LIB_OBJ  = $(SRC_DIR)/wce.o

.PHONY: FORCE
FORCE:

$(LIBRARY): $(LIB_OBJ)
	$(AR) rcs $@ $^

$(SRC_DIR)/wce.o: $(SRC_DIR)/wce.c $(SRC_DIR)/wce.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

lib: $(LIBRARY)
all: lib

# ---- Tests ---------------------------------------------------------------

TEST_DIR  = tests
TEST_OBJ  = $(TEST_DIR)/test_main.o \
            $(TEST_DIR)/test_quantize.o \
            $(TEST_DIR)/test_wce_bitio.o \
            $(TEST_DIR)/test_wce_bpc.o \
            $(TEST_DIR)/test_wce_coeffs.o \
            $(TEST_DIR)/test_wce_codec.o
TEST_BIN  = $(TEST_DIR)/test_runner

$(TEST_BIN): $(TEST_OBJ) $(LIB_OBJ) $(TEST_DIR)/test_runner.h
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(TEST_OBJ) $(LIB_OBJ) -lm

$(TEST_DIR)/test_main.o: $(TEST_DIR)/test_main.c $(TEST_DIR)/test_runner.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

$(TEST_DIR)/%.o: $(TEST_DIR)/%.c $(TEST_DIR)/test_runner.h $(SRC_DIR)/wce.h
	$(CC) $(CFLAGS) $(INCLUDES) -c -o $@ $<

test: $(TEST_BIN)
	./$(TEST_BIN)

# ---- Demos ---------------------------------------------------------------

DEMO_DIR  = demo
DEMO_BINS = $(DEMO_DIR)/mode_shootout $(DEMO_DIR)/image_compress $(DEMO_DIR)/stream_surgery

demos: $(DEMO_BINS)

$(DEMO_DIR)/mode_shootout: $(DEMO_DIR)/mode_shootout.c $(LIB_OBJ) $(SRC_DIR)/wce.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(INCLUDES) -o $@ $< $(LIB_OBJ) -lm

$(DEMO_DIR)/image_compress: $(DEMO_DIR)/image_compress.c $(LIB_OBJ) $(SRC_DIR)/wce.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(INCLUDES) -o $@ $< $(LIB_OBJ) -lm

$(DEMO_DIR)/stream_surgery: $(DEMO_DIR)/stream_surgery.c $(LIB_OBJ) $(SRC_DIR)/wce.h
	$(CC) $(CFLAGS) $(LDFLAGS) $(INCLUDES) -o $@ $< $(LIB_OBJ) -lm

demo-image: $(DEMO_DIR)/image_compress
	./$(DEMO_DIR)/image_compress $(DEMO_DIR)/Cthulhu.pgm $(DEMO_DIR)/Cthulhu_reconstructed.pgm

# ---- Benchmark harness ---------------------------------------------------

BENCH_DIR  = bench
BENCH_OBJ  = $(BENCH_DIR)/bench_runner.o \
             $(BENCH_DIR)/adapter_wce.o \
             $(BENCH_DIR)/corpus.o
BENCH_BIN  = $(BENCH_DIR)/bench_runner

$(BENCH_BIN): $(BENCH_OBJ) $(LIB_OBJ)
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $(BENCH_OBJ) $(LIB_OBJ) -lm

$(BENCH_DIR)/%.o: $(BENCH_DIR)/%.c $(BENCH_DIR)/bench.h $(SRC_DIR)/wce.h
	$(CC) $(CFLAGS) $(INCLUDES) -I$(BENCH_DIR) -c -o $@ $<

bench: $(BENCH_BIN)
	./$(BENCH_BIN)

# ---- Fuzz harness --------------------------------------------------------
#
#   make fuzz              — standalone driver, 1M random inputs under
#                            ASan + UBSan. Works on any toolchain.
#   make fuzz-libfuzzer    — libFuzzer build for users with a clang that
#                            ships libclang_rt.fuzzer (e.g., Homebrew LLVM).

FUZZ_DIR          = tests/fuzz
FUZZ_BIN          = $(FUZZ_DIR)/fuzz_decode
FUZZ_LIBFUZZ_BIN  = $(FUZZ_DIR)/fuzz_decode_lf

fuzz: $(FUZZ_BIN)
	./$(FUZZ_BIN) 1000000

$(FUZZ_BIN): $(FUZZ_DIR)/fuzz_decode.c $(SRC_DIR)/wce.c $(SRC_DIR)/wce.h | $(FUZZ_DIR)/corpus
	$(CC) -std=c99 -O2 -fsanitize=address,undefined $(INCLUDES) -o $@ $< $(SRC_DIR)/wce.c -lm

fuzz-libfuzzer: $(FUZZ_LIBFUZZ_BIN)
	./$(FUZZ_LIBFUZZ_BIN) -max_len=4096 $(FUZZ_DIR)/corpus

$(FUZZ_LIBFUZZ_BIN): $(FUZZ_DIR)/fuzz_decode.c $(SRC_DIR)/wce.c $(SRC_DIR)/wce.h | $(FUZZ_DIR)/corpus
	$(CC) -std=c99 -O2 -DWCE_FUZZ_LIBFUZZER \
	      -fsanitize=fuzzer,address,undefined $(INCLUDES) \
	      -o $@ $< $(SRC_DIR)/wce.c -lm

$(FUZZ_DIR)/corpus:
	mkdir -p $@

# ---- Clean ---------------------------------------------------------------

clean:
	rm -f $(LIBRARY) $(LIB_OBJ)
	rm -f $(TEST_OBJ) $(TEST_BIN)
	rm -f $(BENCH_OBJ) $(BENCH_BIN)
	rm -f $(FUZZ_BIN) $(FUZZ_LIBFUZZ_BIN)
	rm -rf $(FUZZ_DIR)/corpus
	rm -f $(DEMO_BINS)

.PHONY: FORCE lib all test fuzz fuzz-libfuzzer demos demo-image bench clean
