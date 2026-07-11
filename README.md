# Battleships-Server
Contains a client and a server to play battleships on using TLS sockets. This was a part of an assingment from The University of Melbourne's Computer Systems (COMP30023) subject. 

# Run
Run make clean && make to complie server.c and client.c 
The make file is designed for ARM64

To run the server: ./server.c \<port\>
To run the client: ./client.c [-a \<host-ip\>] [-p \<port\>] [-s \<game-id\>]
