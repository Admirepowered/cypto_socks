
CFLAGS = -Wall -Wextra -Wno-unused -Wno-unused-parameter

ifdef DEBUG
	CFLAGS += -DDEBUG -g
endif

objs := main.o

.PHONY: all
all: a

.PHONY: a
a: ${objs}
	x86_64-pc-msys-gcc -o $@ $^

%.o : %.c
	x86_64-pc-msys-gcc -c $(CFLAGS) $(CPPFLAGS) $< -o $@

.PHONY: clean
