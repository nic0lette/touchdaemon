
touchdaemon : src/touchdaemon.c 
	g++ -lX11 -lXi src/touchdaemon.c -o touchdaemon

clean:
	rm touchdaemon

tar:
	tar -cf touchdaemon.tar ../touchdaemon --exclude=.git

install: 
	[ -d /usr/bin ] || mkdir /usr/bin -p
	cp touchdaemon /usr/bin/touchdaemon
