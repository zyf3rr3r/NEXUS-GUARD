import {createServer} from 'node:http';
import {readFile} from 'node:fs/promises';

const assets = new Map(await Promise.all([
  ['/', 'index.html', 'text/html'],
  ['/style.css', 'style.css', 'text/css'],
  ['/app.js', 'app.js', 'text/javascript'],
  ['/apps.js', 'apps.js', 'text/javascript']
].map(async ([path, name, type]) => [path, {
  content: await readFile(new URL(`../web/${name}`, import.meta.url)), type
}])));

const server = createServer((request, response) => {
  const pathname = new URL(request.url, 'http://127.0.0.1').pathname;
  const asset = assets.get(pathname);
  if (!asset || request.method !== 'GET') {
    response.writeHead(404).end();
    return;
  }
  response.writeHead(200, {
    'Content-Type': `${asset.type}; charset=utf-8`,
    'Cache-Control': 'no-store',
    'X-Content-Type-Options': 'nosniff',
    'Content-Security-Policy': "default-src 'none'; script-src 'self'; style-src 'self'; base-uri 'none'; form-action 'none'; frame-ancestors 'none'"
  }).end(asset.content);
});
server.listen(8077, '127.0.0.1', () => console.log('PocketOS local apps: http://127.0.0.1:8077/?offline=1'));
