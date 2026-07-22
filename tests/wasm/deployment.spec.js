// SPDX-License-Identifier: MPL-2.0
const { createHash } = require('node:crypto');
const { readFile } = require('node:fs/promises');
const http = require('node:http');
const path = require('node:path');
const { test, expect } = require('@playwright/test');

const packageRoot = path.resolve(
  process.env.PJ_WASM_PACKAGE_DIR || path.join(__dirname, '..', '..', 'build-wasm', 'deploy'),
);
let server;
let baseUrl;
let manifest;

function rawRequest(url, { method = 'GET', headers = {}, collectBody = false } = {}) {
  return new Promise((resolve, reject) => {
    const request = http.request(url, { method, headers }, response => {
      const hash = createHash('sha256');
      const chunks = [];
      let bytes = 0;
      response.on('data', chunk => {
        hash.update(chunk);
        bytes += chunk.length;
        if (collectBody) {
          chunks.push(chunk);
        }
      });
      response.on('end', () => resolve({
        status: response.statusCode,
        headers: response.headers,
        bytes,
        sha256: hash.digest('hex'),
        body: collectBody ? Buffer.concat(chunks).toString('utf8') : undefined,
      }));
    });
    request.on('error', reject);
    request.end();
  });
}

function expectIsolationHeaders(headers) {
  expect(headers['cross-origin-opener-policy']).toBe('same-origin');
  expect(headers['cross-origin-embedder-policy']).toBe('require-corp');
  expect(headers['cross-origin-resource-policy']).toBe('same-origin');
  expect(headers['x-content-type-options']).toBe('nosniff');
}

test.beforeAll(async () => {
  manifest = JSON.parse(await readFile(path.join(packageRoot, 'manifest.json'), 'utf8'));
  const { listenWasmServer } = await import('../../scripts/serve_wasm.mjs');
  server = await listenWasmServer(packageRoot);
  const address = server.address();
  baseUrl = `http://127.0.0.1:${address.port}`;
});

test.afterAll(async () => {
  if (server) {
    await new Promise((resolve, reject) => server.close(error => (error ? reject(error) : resolve())));
  }
});

test('deployment server negotiates exact compressed bytes, MIME, isolation, and cache policy', async () => {
  expect(manifest.format).toBe('org.plotjuggler.wasm-deployment.v1');
  expect(manifest.bundleId).toMatch(/^sha256-[0-9a-f]{32}$/);
  expect(manifest.packagingRuntime).toEqual({
    node: process.version,
    zlib: process.versions.zlib,
    brotli: { quality: 9, window: 22 },
    gzip: { level: 9, mtime: 0, os: 255 },
  });
  const headersBytes = await readFile(path.join(packageRoot, '_headers'));
  const headersMetadata = manifest.deploymentConfiguration.headersTemplate;
  expect(headersBytes.length).toBe(headersMetadata.bytes);
  expect(createHash('sha256').update(headersBytes).digest('hex')).toBe(headersMetadata.sha256);
  const headersTemplate = headersBytes.toString('utf8');
  expect(headersTemplate).toContain('/\n  Cache-Control: no-cache');
  expect(headersTemplate).toContain(
    `/assets/${manifest.bundleId}/*\n  Cache-Control: public, max-age=31536000, immutable`,
  );

  const entrypoint = await rawRequest(`${baseUrl}/`, {
    headers: { 'Accept-Encoding': 'identity' },
    collectBody: true,
  });
  expect(entrypoint.status).toBe(200);
  expect(entrypoint.headers['content-type']).toBe('text/html; charset=utf-8');
  expect(entrypoint.headers['cache-control']).toBe('no-cache');
  expect(entrypoint.headers['content-encoding']).toBeUndefined();
  expectIsolationHeaders(entrypoint.headers);
  expect(entrypoint.body).toContain(`src="assets/${manifest.bundleId}/plotjuggler4.js"`);
  expect(entrypoint.body).toContain(`src="assets/${manifest.bundleId}/qtloader.js"`);
  expect(entrypoint.body).toContain(
    `? 'assets/${manifest.bundleId}/plotjuggler4.wasm' : filename`,
  );

  const wasm = manifest.files.find(file => file.path.endsWith('/plotjuggler4.wasm'));
  expect(wasm).toBeDefined();
  const requests = [
    ['br', 'br', 'br, identity;q=0'],
    ['gzip', 'gzip', 'gzip, identity;q=0'],
    ['identity', undefined, 'identity'],
  ];
  for (const [name, contentEncoding, acceptEncoding] of requests) {
    const response = await rawRequest(`${baseUrl}/${wasm.path}`, {
      headers: { 'Accept-Encoding': acceptEncoding },
    });
    const expected = wasm.representations[name];
    expect(response.status).toBe(200);
    expect(response.headers['content-type']).toBe('application/wasm');
    expect(response.headers['content-encoding']).toBe(contentEncoding);
    expect(response.headers['cache-control']).toBe('public, max-age=31536000, immutable');
    expect(response.headers.vary).toBe('Accept-Encoding');
    expect(response.headers.etag).toBe(`"sha256-${expected.sha256}"`);
    expect(response.bytes).toBe(expected.bytes);
    expect(response.sha256).toBe(expected.sha256);
    expectIsolationHeaders(response.headers);
  }

  const cached = await rawRequest(`${baseUrl}/${wasm.path}`, {
    headers: {
      'Accept-Encoding': 'br, identity;q=0',
      'If-None-Match': `"sha256-${wasm.representations.br.sha256}"`,
    },
  });
  expect(cached.status).toBe(304);
  expect(cached.bytes).toBe(0);
  expect(cached.headers['content-encoding']).toBe('br');
  expectIsolationHeaders(cached.headers);

  const head = await rawRequest(`${baseUrl}/${wasm.path}`, {
    method: 'HEAD',
    headers: { 'Accept-Encoding': 'gzip, identity;q=0' },
  });
  expect(head.status).toBe(200);
  expect(head.bytes).toBe(0);
  expect(head.headers['content-length']).toBe(String(wasm.representations.gzip.bytes));
  expect(head.headers['content-encoding']).toBe('gzip');
  expectIsolationHeaders(head.headers);

  const missing = await rawRequest(`${baseUrl}/${wasm.representations.br.path}`);
  expect(missing.status).toBe(404);
  expect(missing.headers['cache-control']).toBe('no-store');
  expectIsolationHeaders(missing.headers);

  const privateConfiguration = await rawRequest(`${baseUrl}/_headers`);
  expect(privateConfiguration.status).toBe(404);
  expectIsolationHeaders(privateConfiguration.headers);
});

test('cold Chromium boot runs the threaded app from the content-addressed package', async ({ page }) => {
  const consoleMessages = [];
  const pageErrors = [];
  const responses = new Map();
  page.on('console', message => consoleMessages.push(message.text()));
  page.on('pageerror', error => pageErrors.push(error.stack || error.message));
  page.on('response', response => responses.set(response.url(), response.headers()));

  await page.goto(`${baseUrl}/`, { waitUntil: 'domcontentloaded' });
  const startup = await page.waitForFunction(() => {
    const status = document.querySelector('#qtstatus')?.textContent || '';
    if (status.startsWith('Application exit')) {
      return { error: status };
    }
    const screen = document.querySelector('#screen');
    if (status === 'Loading...' && screen && getComputedStyle(screen).display !== 'none') {
      return { ready: true };
    }
    return false;
  }, undefined, { timeout: 90_000 });
  const startupState = await startup.jsonValue();
  expect(startupState.error, `Qt startup failed: ${startupState.error}`).toBeUndefined();
  await expect.poll(
    () => consoleMessages.some(message => message.includes('Scanning 0 plugin folder(s)')),
  ).toBe(true);
  await expect.poll(() => page.evaluate(() => crossOriginIsolated)).toBe(true);
  expect(await page.evaluate(() => isSecureContext)).toBe(true);
  expect(await page.evaluate(() => typeof SharedArrayBuffer)).toBe('function');

  const assetBase = `${baseUrl}/assets/${manifest.bundleId}`;
  const wasmHeaders = responses.get(`${assetBase}/plotjuggler4.wasm`);
  expect(wasmHeaders).toBeDefined();
  expect(wasmHeaders['content-type']).toBe('application/wasm');
  expect(wasmHeaders['content-encoding']).toBe('br');
  expect(wasmHeaders['cache-control']).toBe('public, max-age=31536000, immutable');
  expect(responses.has(`${assetBase}/plotjuggler4.js`)).toBe(true);
  expect(responses.has(`${assetBase}/qtloader.js`)).toBe(true);
  expect(pageErrors).toEqual([]);
});
