const dgram = require('dgram');
const client = dgram.createSocket('udp4');

const PORT = 12345;
const HOST = '192.168.1.78';

client.on('listening', () => {
  const address = client.address();
  console.log(`Listening on ${address.address}:${address.port}`);
  // client.setBroadcast(true); // принимать broadcast
});

client.on('message', (msg, rinfo) => {



  const hexStr = Array.from(msg)
    .map(b => b.toString(16).padStart(2, '0'))
    .join(' ');
  console.log(`From ${rinfo.address}:${rinfo.port} - ${hexStr} - ${msg}`);
});

client.bind(PORT, HOST);
