# Bit-Factory-Incorporated
Client/Server communication.

Server produces data through a child process and puts it into a storage.
Clients can connect to the Server in order to receive data in batches of 13 KiB.
Server manages storage status and client queue in order to determine who is to be sent data.
Both programs print reports on their state of affairs.
Data production and receival are burdened with an artificial processing time.
Clients will connect to the server multiple times to receive a batch of data untill they fill their own storage.
Client data decays with time.
Max amount of concurrent clients is only limited to poll() max FD count - if it were to increase we could use epoll().


Usage:
Compile producent.c with buffer.c and buffer.h.

Producent(server):
-p <float> : data production rate in 2662B per second
[<addr>:]port : producent address [default value: "localhost"]
  
Konsument(client):
-c <int> : client storage capacity in blocks of 30 KiB,
-p <float> : data reading rate in 4435B per second
-d <float> : data degradation rate in 819B per second
[<addr>:]port : producent address [default value: "localhost"]
