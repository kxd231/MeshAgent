# Tests of kvm_yuv.c. Host has no compilers: run them with run-in-docker.sh (or `make` inside any image that has gcc and make).
CC ?= gcc
CFLAGS ?= -std=c99 -O2 -Wall -Wextra -Werror
SAN = -fsanitize=address,undefined -fno-sanitize-recover=undefined -O1 -g

.PHONY: all test asan clean
all: test asan

kvm_yuv_test: kvm_yuv_test.c ../kvm_yuv.c ../kvm_yuv.h
	$(CC) $(CFLAGS) -o $@ kvm_yuv_test.c ../kvm_yuv.c -lm

kvm_yuv_test_asan: kvm_yuv_test.c ../kvm_yuv.c ../kvm_yuv.h
	$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) -o $@ kvm_yuv_test.c ../kvm_yuv.c -lm

kvm_annexb_test: kvm_annexb_test.c ../kvm_annexb.c ../kvm_annexb.h
	$(CC) $(CFLAGS) -o $@ kvm_annexb_test.c ../kvm_annexb.c

kvm_annexb_test_asan: kvm_annexb_test.c ../kvm_annexb.c ../kvm_annexb.h
	$(CC) -std=c99 -Wall -Wextra -Werror $(SAN) -o $@ kvm_annexb_test.c ../kvm_annexb.c

test: kvm_yuv_test kvm_annexb_test
	./kvm_yuv_test
	./kvm_annexb_test

asan: kvm_yuv_test_asan kvm_annexb_test_asan
	./kvm_yuv_test_asan
	./kvm_annexb_test_asan

clean:
	rm -f kvm_yuv_test kvm_yuv_test_asan kvm_annexb_test kvm_annexb_test_asan
