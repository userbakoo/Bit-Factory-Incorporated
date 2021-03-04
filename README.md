# Bit-Factory-Incorporated
Client/Server communication.
<br/>
Server produces data through a child process and puts it into a storage.<br/>
Clients can connect to the Server in order to receive data in batches of 13 KiB.<br/>
Server manages storage status and client queue in order to determine who is to be sent data.<br/>
Both programs print reports on their state of affairs.<br/>
Data production and receival are burdened with an artificial processing time.<br/>
Clients will connect to the server multiple times to receive a batch of data untill they fill their own storage.<br/>
Client data decays with time.<br/>
Max amount of concurrent clients is only limited to poll() max FD count - if it were to increase we could use epoll().<br/>


Usage:<br/>
Compile producent.c with buffer.c and buffer.h.<br/>
<br/>
Producent(server):<br/>
-p <float> : data production rate in 2662B per second<br/>
[<addr>:]port : producent address [default value: "localhost"]<br/>
  <br/>
Konsument(client):<br/>
-c <int> : client storage capacity in blocks of 30 KiB<br/>
-p <float> : data reading rate in 4435B per second<br/>
-d <float> : data degradation rate in 819B per second<br/>
[<addr>:]port : producent address [default value: "localhost"]<br/>
