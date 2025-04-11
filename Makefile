CC = g++
CFLAGS = -std=c++11 -Wall -pthread
SRC = streams.cpp quill-runtime.cpp
OBJ = $(SRC:.cpp=.o)
TARGET = quill_test

all: $(TARGET)

$(TARGET): $(OBJ)
	$(CC) $(CFLAGS) -o $(TARGET) $(OBJ) -lpfm -fopenmp

%.o: %.cpp
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJ) $(TARGET)
