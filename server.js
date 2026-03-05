const http = require('http');
const fs = require('fs');
const path = require('path');

// Configure the port for the server
const PORT = process.env.PORT || 3000;

// MIME types for serving static files
const MIME_TYPES = {
  '.html': 'text/html',
  '.js': 'text/javascript',
  '.css': 'text/css',
  '.json': 'application/json',
  '.png': 'image/png',
  '.jpg': 'image/jpeg',
  '.gif': 'image/gif',
  '.ico': 'image/x-icon',
  '.wasm': 'application/wasm',
  '.data': 'application/octet-stream',
  '.bin':  'application/octet-stream',
};

// Function to serve static files
function serveStaticFile(req, res, filePath) {
  const extname = path.extname(filePath);
  const contentType = MIME_TYPES[extname] || 'text/plain';

  fs.readFile(filePath, (err, content) => {
    if (err) {
      if (err.code === 'ENOENT') {
        res.writeHead(404);
        res.end('File not found');
      } else {
        res.writeHead(500);
        res.end(`Server Error: ${err.code}`);
      }
    } else {
      res.writeHead(200, { 'Content-Type': contentType });
      res.end(content, 'utf-8');
    }
  });
}

// Create a simple HTTP server
const server = http.createServer((req, res) => {
  res.setHeader('Access-Control-Allow-Origin', '*');
  res.setHeader('Access-Control-Allow-Methods', 'GET, OPTIONS');
  res.setHeader('Access-Control-Allow-Headers', 'Content-Type');

  if (req.method === 'OPTIONS') {
    res.writeHead(204);
    res.end();
    return;
  }

  // Health check endpoint
  if (req.method === 'GET' && req.url === '/health') {
    res.writeHead(200, { 'Content-Type': 'application/json' });
    res.end(JSON.stringify({ status: 'ok', timestamp: new Date().toISOString() }));
    return;
  }

  // Serve the default user.h source code for "Reset to Default"
  if (req.method === 'GET' && req.url === '/source') {
    try {
      const content = fs.readFileSync(path.join(__dirname, 'src', 'user.h'), 'utf8');
      res.writeHead(200, {
        'Content-Type': 'text/plain',
        'Cache-Control': 'no-cache, no-store, must-revalidate',
        'Pragma': 'no-cache',
        'Expires': '0'
      });
      res.end(content);
    } catch (error) {
      res.writeHead(500);
      res.end(JSON.stringify({ success: false, error: error.message }));
    }
    return;
  }

  // Serve static files from the project root
  if (req.method === 'GET') {
    const pathname = new URL(req.url, `http://${req.headers.host}`).pathname;

    let filePath;
    if (pathname === '/') {
      filePath = path.join(__dirname, 'index.html');
    } else {
      filePath = path.join(__dirname, pathname.substring(1));
    }

    if (fs.existsSync(filePath) && fs.statSync(filePath).isFile()) {
      serveStaticFile(req, res, filePath);
    } else {
      res.writeHead(404);
      res.end('File not found');
    }
    return;
  }

  res.writeHead(404, { 'Content-Type': 'application/json' });
  res.end(JSON.stringify({ success: false, error: 'Not Found' }));
});

// Start the server
server.listen(PORT, '0.0.0.0', () => {
  console.log(`\n==== Hear-C Server ====`);
  console.log(`Server running at http://0.0.0.0:${PORT}`);
  console.log(`- Access the app at http://localhost:${PORT}/`);
  console.log(`- Compilation happens entirely in the browser using wasm-clang`);
  console.log(`============================\n`);
});

// handle common server errors gracefully
server.on('error', (err) => {
  if (err.code === 'EADDRINUSE') {
    console.error(`Error: port ${PORT} is already in use.`);
    console.error('Either stop the process using that port or set a different PORT.')
    process.exit(1);
  }
  console.error('Unexpected server error:', err);
  process.exit(1);
});
