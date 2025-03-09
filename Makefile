CC = g++
CFLAGS = -std=c++11 -Wall -pthread
LDFLAGS = -lnuma  # Added NUMA library for linking
SRC = iterative_averaging.cpp quill-runtime.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = quill_test

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ) $(LDFLAGS)

%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
