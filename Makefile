utz: utz.c
	$(CC) -Wall utz.c -lasound -pthread -o $@
