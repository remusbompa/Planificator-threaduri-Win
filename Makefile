CC = cl
CFLAGS = /nologo /W4 /c

build: libscheduler.dll

libscheduler.dll: so_scheduler.obj
	link /nologo /dll /out:$*.dll /implib:$*.lib $**

so_scheduler.obj: so_scheduler.c
	$(CC) $(CFLAGS) /Fo$@ $**

.PHONY: clean

clean:
	rm -f *.obj *~ libscheduler.dll libscheduler.lib
