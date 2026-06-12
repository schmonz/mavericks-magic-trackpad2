CC      = clang
CFLAGS  = -Wall -Wextra -Wno-missing-field-initializers -O2 -std=c99
FRAMEWORKS = -framework IOKit -framework CoreFoundation
SRC = src

.PHONY: all test clean daemon tools

all: daemon tools

daemon: mt2d
tools: dump_frames vhid_probe

mt2d: $(SRC)/mt2d.c $(SRC)/mt2_reader.c $(SRC)/mt2_decode.c $(SRC)/mt1_encode.c $(SRC)/vhid_mt1.c
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

dump_frames: tools/dump_frames.c $(SRC)/mt2_reader.c
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

vhid_probe: tools/vhid_probe.c $(SRC)/vhid_mt1.c
	$(CC) $(CFLAGS) -o $@ $^ $(FRAMEWORKS)

# Unit tests are pure C, no frameworks needed.
TESTS = test_model test_decode test_encode
test: $(TESTS)
	@fail=0; for t in $(TESTS); do echo "== $$t =="; ./$$t || fail=1; done; \
	 [ $$fail -eq 0 ] && echo "ALL TESTS PASS"

test_model: tests/test_model.c
	$(CC) $(CFLAGS) -o $@ $<
test_decode: tests/test_decode.c $(SRC)/mt2_decode.c
	$(CC) $(CFLAGS) -o $@ $^
test_encode: tests/test_encode.c $(SRC)/mt1_encode.c
	$(CC) $(CFLAGS) -o $@ $^

clean:
	rm -f mt2d dump_frames vhid_probe $(TESTS) *.o
