# Bit-Works

A server client relation, where server produces bits and proceeds to send them over to the client.
Server is a single-threaded program, able to handle thousands of clients at the same time.

Both client and server, combined, showcase use of:
-Pipes,
-Sockets,
-I/O Multiplexing,
-Dynamic data structures

#Details

Server:
Producer.c spawns a child process responsible for producing data, assigning it some value (single letter alphabetically in chunks of 640),
and sending it to the main server proccess via Unix Pipe (a warehouse of sorts).

Main process proceeds to listen for connections, and upon receiving one puts a client into a queue. Once enough material is available (14KB),
server begins a transfer in smaller chunks (currently 1 KB) until a full package is sent, or until a connection is broken.

Even though data is sent in small chunks, the contents are continuous, just as they were inside the warehouse upon creation.

Client:
Connects to the server and receives data. At this moment, client has no use for it and just ignores it.
Upon receiving the whole package, client may wish to get another package ( depends on available local storage ).

