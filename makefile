APP = dirsync

main: main.c
	gcc -framework CoreServices main.c -o $(APP)
