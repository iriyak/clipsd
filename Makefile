CLIPS_DIR ?= $(HOME)/clips
CC ?= gcc
CFLAGS ?= -O2 -Wall

clipsd: src/clipsd.c src/cJSON.c src/cJSON.h
	$(CC) $(CFLAGS) -o $@ src/clipsd.c src/cJSON.c \
	  -I $(CLIPS_DIR)/include -L $(CLIPS_DIR)/lib -lclips -lm

clean:
	rm -f clipsd

.PHONY: clean
