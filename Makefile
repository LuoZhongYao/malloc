zm: zalloc.c list.h
	gcc $< -o $@ -std=c99 -g

clean:
	-rm zm -f
