all : xv6_fsck

xv6_fsck : xv6_fsck.c
	gcc xv6_fsck.c -o xv6_fsck -Wall -Werror -lrt -lpthread

clean : 
	rm -f xv6_fsck
