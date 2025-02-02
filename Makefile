# Compiler and flags
CC = g++
CFLAGS = -std=c++11 -Wall -pthread

# List of source files
SRC = nqueens.cpp quill-runtime.cpp

# Automatically generate object files from source files
OBJ = $(SRC:.cpp=.o)

# Name of the final executable
TARGET = quill_test

# Default target: build the executable
all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ)

# Pattern rule: compile .cpp files into .o files
%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

# Clean up the build artifacts
clean:
	rm -f $(OBJ) $(TARGET)
