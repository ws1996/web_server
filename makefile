all:
	g++ -pthread main.cpp http_conn.cpp -o server -std=c++11 -g
	(cd cgi-bin; make)
clean:
	rm server
