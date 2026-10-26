# File: fsck/Makefile
# Created by Andrea "Nemesi" Cocito on 23/08/2026
# Build fsck_femtofs and run its self-tests with BSD make or GNU make.

PROG = fsck_femtofs

CC ?= cc
CPPFLAGS ?=
CFLAGS ?= -O2
LDFLAGS ?=
LDLIBS ?=

CSTANDARD ?= -std=c23
CWARNINGS ?= -Wall -Wextra

PYTHON ?= python3

TEST_IMAGES = test_flat.0fs test_subdir.0fs

.PHONY: all test clean

all: $(PROG)

$(PROG): $(PROG).c
	$(CC) $(CPPFLAGS) $(CFLAGS) $(CSTANDARD) $(CWARNINGS) $(PROG).c $(LDFLAGS) $(LDLIBS) -o $@

test: $(PROG)
	$(PYTHON) make_test_image.py flat test_flat.0fs
	$(PYTHON) make_test_image.py subdir test_subdir.0fs
	./$(PROG) test_flat.0fs
	./$(PROG) test_subdir.0fs
	$(PYTHON) corrupt_tests.py

clean:
	rm -f $(PROG) $(TEST_IMAGES)

# END File: fsck/Makefile
