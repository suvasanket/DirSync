APP = dirsync

main: main.c
	gcc -framework CoreServices main.c -o $(APP)

some: some.c
	gcc some.c -o some
