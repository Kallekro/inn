.PHONY: clean

inn: inn.c
	$(CC) inn.c -o inn -Wall -Wextra -pedantic -std=c99

clean:
	rm -rf inn