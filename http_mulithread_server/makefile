server: main.cpp  http_conn.cpp http_conn.h sql_connectionpool.cpp sql_connectionpool.h	lock.h log.cpp log.h blockqueue.h timer.h
	g++ -g -o server main.cpp http_conn.cpp log.cpp sql_connectionpool.cpp -lpthread -I/usr/include/mysql -L/usr/lib/mysql/ -lmysqlclient

clean:
	rm  -r server

