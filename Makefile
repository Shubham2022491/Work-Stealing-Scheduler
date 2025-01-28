CC = g++
CFLAGS = -std=c++11 -pthread
SRC = quill-runtime.cpp
OBJ = quill-runtime.o
EXEC = quill

$(EXEC): $(OBJ)
	$(CC) $(OBJ) -o $(EXEC)

$(OBJ): quill-runtime.cpp quill-runtime.h
	$(CC) $(CFLAGS) -c quill-runtime.cpp

clean:
	rm -f $(OBJ) $(EXEC)
