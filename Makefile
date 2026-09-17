CC :=gcc

CFLAGS = glad/src/glad.c -Iglad/include -lglfw -lGL -ldl

fopen_test: fopen.c
	-$(CC) -o $@ $^
	-./$@
	-rm ./$@


fclose_test: fclose.c
	-$(CC) -o $@ $^
	-./$@
	-rm ./$@

fputc : fputc.c
	-$(CC) -o $@ $^
	-./$@
	-rm ./$@

fputs : fputs.c
	-$(CC) -o $@ $^
	-./$@
	-rm ./$@


system_call : system_call.c
	-$(CC) -o $@ $^
	-./$@
	-rm ./$@


gl_test:gl_test.c 
	-$(CC) $^  -o $@ $(CFLAGS)
	-./$@
	-rm ./$@

hello_window:hello_window.c 
	-$(CC) $^  -o $@ $(CFLAGS)
	-./$@
	-rm ./$@


hello_triangle : hello_triangle.c
	-$(CC) $^ -o $@ $(CFLAGS)
	-./$@
	-rm ./$@

mtdownload:mtdownload.c 
	-$(CC) $^ -o $@ -O2 -Wall -pthread