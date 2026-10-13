# MPCSH 1.6 Build Script

# Compile MPCSH 1.6 with required libraries

CC = gcc
CFLAGS = -Wall -Wextra -O2 -std=c11 -D_GNU_SOURCE
LIBS = -lgio-2.0 -lgobject-2.0 -lglib-2.0

TARGET = mpcsh_1_6
SRC_DIR = src
INCLUDE_DIR = include

# Get glib include flags
GLIB_CFLAGS = $(shell pkg-config --cflags glib-2.0)

SRCS = $(wildcard $(SRC_DIR)/*.c)
OBJS = $(SRCS:.c=.o)

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LIBS)

%.o: %.c
	$(CC) $(CFLAGS) -I$(INCLUDE_DIR) $(GLIB_CFLAGS) -c $< -o $@

clean:
	rm -f $(TARGET) $(OBJS)

install: $(TARGET)
	install -m 755 $(TARGET) /usr/bin/mpcsh_1_6

.PHONY: all clean install
