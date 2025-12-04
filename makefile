.PHONY : all clean linux

all: linux

linux:
	gcc -shared -fPIC -Wall -g -O2 -DLUA_PROF_TRAP -fno-omit-frame-pointer \
		-I3rd/lua-5.4.8/src \
		-o luaprofilec.so \
		imap.c smap.c profile.c icallpath.c

clean:
	rm -rf luaprofilec.so