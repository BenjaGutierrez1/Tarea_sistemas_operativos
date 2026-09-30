CC = gcc
CFLAGS = -Wall -Wextra -std=c17
TARGET = planificador
SRC = planificador.c

all: $(TARGET)

$(TARGET): $(SRC)
	$(CC) $(CFLAGS) -o $(TARGET) $(SRC) -lpthread

clean:
	rm -f $(TARGET) plan_stress.txt

.PHONY: all clean
