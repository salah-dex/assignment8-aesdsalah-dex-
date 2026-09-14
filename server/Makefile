# Is compiled by the “all” and “default” target of a Makefile in the “server” 
#directory and supports cross compilation, placing the executable file in the “server” directory and named aesdsocket.

CC = gcc
CFLAGS 	= -g -Wall
TARGET 	= aesdsocket
BINARY 	= *.elf
HEADERS = *.h
MAP    	= *.map
SOURCES = $(wildcard *.c)  # Use wildcard to find all .c files
OBJECTS = $(SOURCES:.c=.o)  # Convert .c filenames to .o

ifdef CROSS_COMPILE
CC = $(CROSS_COMPILE)gcc
endif

all: $(TARGET)

default: all

# Rule to compile object files
# Pattern rule to compile each .c file into a .o file and include the header files
%.o: %.c
		$(CC) $(CFLAGS) -c $< -o $@  -I.  # Include current directory for header files

# Rule to link the target executable
$(TARGET): $(OBJECTS)
		$(CC) $(CFLAGS) -o $(TARGET) $(OBJECTS)


clean:
		rm -f $(TARGET) $(OBJECTS) $(MAP) $(BINARY) *.elf