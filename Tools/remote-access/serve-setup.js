const fs = require('fs');
const http = require('http');
const path = require('path');

const bindAddress = process.argv[2] || '127.0.0.1';
const port = Number(process.argv[3] || 8765);
const scriptPath = path.join(__dirname, 'enable-eriana-ssh.ps1');

http.createServer((request, response) => {
  if (request.url !== '/enable-eriana-ssh.ps1') {
    response.writeHead(404);
    response.end();
    return;
  }
  response.writeHead(200, { 'Content-Type': 'text/plain; charset=utf-8' });
  fs.createReadStream(scriptPath).pipe(response);
}).listen(port, bindAddress, () => {
  process.stdout.write(`Serving SSH setup at http://${bindAddress}:${port}/enable-eriana-ssh.ps1\n`);
});
