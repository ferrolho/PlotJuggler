// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

import assert from 'node:assert/strict';
import { spawnSync } from 'node:child_process';
import { mkdir, mkdtemp, readFile, rm, writeFile } from 'node:fs/promises';
import os from 'node:os';
import path from 'node:path';
import test from 'node:test';
import { fileURLToPath } from 'node:url';

import { createSizeReport } from '../../scripts/check_wasm_size.mjs';
import { representation } from '../../scripts/wasm_manifest.mjs';

const SIZE_CHECKER = fileURLToPath(new URL('../../scripts/check_wasm_size.mjs', import.meta.url));

async function fixture(t) {
  const root = await mkdtemp(path.join(os.tmpdir(), 'plotjuggler-wasm-size-'));
  t.after(() => rm(root, { recursive: true, force: true }));
  const packageRoot = path.join(root, 'package');
  await mkdir(path.join(packageRoot, 'assets'), { recursive: true });

  const wasmIdentity = Buffer.from([0x00, 0x61, 0x73, 0x6d, 1, 0, 0, 0]);
  const wasmBr = Buffer.from([1, 2, 3]);
  const wasmGzip = Buffer.from([4, 5, 6, 7]);
  const loader = Buffer.from('loader');
  const archive = Buffer.from('archive');
  const assets = [
    ['assets/app.wasm', wasmIdentity],
    ['assets/app.wasm.br', wasmBr],
    ['assets/app.wasm.gz', wasmGzip],
    ['assets/loader.js', loader],
  ];
  await Promise.all(assets.map(([name, contents]) => writeFile(path.join(packageRoot, name), contents)));

  const manifest = {
    format: 'org.plotjuggler.wasm-deployment.v1',
    bundleId: 'sha256-test',
    files: [
      {
        path: 'assets/app.wasm',
        representations: {
          identity: representation('assets/app.wasm', wasmIdentity),
          br: representation('assets/app.wasm.br', wasmBr),
          gzip: representation('assets/app.wasm.gz', wasmGzip),
        },
      },
      {
        path: 'assets/loader.js',
        representations: { identity: representation('assets/loader.js', loader) },
      },
    ],
  };
  await writeFile(path.join(packageRoot, 'manifest.json'), `${JSON.stringify(manifest)}\n`);
  const archivePath = path.join(root, 'release.tar.gz');
  await writeFile(archivePath, archive);
  return { packageRoot, archivePath };
}

function budget(overrides = {}) {
  const maxima = {
    'wasm.identity': 8,
    'wasm.br': 3,
    'wasm.gzip': 4,
    archive: 7,
    ...overrides,
  };
  return {
    format: 'org.plotjuggler.wasm-size-budget.v1',
    profile: 'test-release',
    limits: Object.fromEntries(Object.entries(maxima).map(([name, maxBytes]) => [
      name,
      { maxBytes, rationale: 'test boundary' },
    ])),
    contributorAudit: { method: 'test' },
  };
}

test('passes deterministically when every measurement is exactly at its limit', async t => {
  const paths = await fixture(t);
  const first = await createSizeReport({ ...paths, budget: budget() });
  const second = await createSizeReport({ ...paths, budget: budget() });

  assert.deepEqual(second, first);
  assert.equal(first.report.status, 'pass');
  assert.deepEqual(first.failures, []);
  assert.equal(first.report.budgets['wasm.identity'].headroomBytes, 0);
  assert.equal(first.report.measurements.servedAssets.br.bytes, 9);
});

test('passes below a limit and fails at the first byte over it', async t => {
  const paths = await fixture(t);
  const below = await createSizeReport({
    ...paths,
    budget: budget({ 'wasm.identity': 9 }),
  });
  assert.equal(below.report.status, 'pass');
  assert.equal(below.report.budgets['wasm.identity'].headroomBytes, 1);

  const over = await createSizeReport({
    ...paths,
    budget: budget({ 'wasm.identity': 7 }),
  });
  assert.equal(over.report.status, 'fail');
  assert.equal(over.report.budgets['wasm.identity'].headroomBytes, -1);
  assert.deepEqual(over.failures, ['wasm.identity: 8 > 7 bytes']);
});

test('command exits successfully at the limit and nonzero on the first byte over', async t => {
  const paths = await fixture(t);
  const root = path.dirname(paths.packageRoot);
  const budgetPath = path.join(root, 'budget.json');
  const reportPath = path.join(root, 'report.json');
  const argumentsFor = () => [
    SIZE_CHECKER,
    '--package', paths.packageRoot,
    '--archive', paths.archivePath,
    '--budget', budgetPath,
    '--output', reportPath,
  ];

  await writeFile(budgetPath, JSON.stringify(budget()));
  const atLimit = spawnSync(process.execPath, argumentsFor(), { encoding: 'utf8' });
  assert.equal(atLimit.status, 0, atLimit.stderr);
  assert.equal(JSON.parse(await readFile(reportPath, 'utf8')).status, 'pass');

  await writeFile(budgetPath, JSON.stringify(budget({ 'wasm.identity': 7 })));
  const overLimit = spawnSync(process.execPath, argumentsFor(), { encoding: 'utf8' });
  assert.equal(overLimit.status, 1);
  assert.match(overLimit.stderr, /wasm\.identity: 8 > 7 bytes/);
  assert.equal(JSON.parse(await readFile(reportPath, 'utf8')).status, 'fail');
});

test('skips only the archive limit when no release archive is measured', async t => {
  const { packageRoot } = await fixture(t);
  const report = await createSizeReport({ packageRoot, archivePath: '', budget: budget() });

  assert.equal(report.report.status, 'pass');
  assert.deepEqual(report.failures, []);
  assert.equal(report.report.budgets.archive.status, 'skipped');
  assert.equal(report.report.budgets.archive.maxBytes, 7);
  assert.equal(report.report.measurements.archive, undefined);
  // The delivered representations stay fully gated — skipping the archive must
  // not weaken the limits the browser build can actually measure.
  for (const name of ['wasm.identity', 'wasm.br', 'wasm.gzip']) {
    assert.equal(report.report.budgets[name].status, 'pass');
  }

  const over = await createSizeReport({
    packageRoot,
    archivePath: '',
    budget: budget({ 'wasm.br': 2 }),
  });
  assert.equal(over.report.status, 'fail');
  assert.deepEqual(over.failures, ['wasm.br: 3 > 2 bytes']);
});

test('command runs without --archive and still enforces the wasm limits', async t => {
  const { packageRoot } = await fixture(t);
  const root = path.dirname(packageRoot);
  const budgetPath = path.join(root, 'budget.json');
  const reportPath = path.join(root, 'report.json');
  const argumentsFor = () => [
    SIZE_CHECKER,
    '--package', packageRoot,
    '--budget', budgetPath,
    '--output', reportPath,
  ];

  await writeFile(budgetPath, JSON.stringify(budget()));
  const withoutArchive = spawnSync(process.execPath, argumentsFor(), { encoding: 'utf8' });
  assert.equal(withoutArchive.status, 0, withoutArchive.stderr);
  assert.match(withoutArchive.stdout, /not measured: archive/);
  assert.equal(JSON.parse(await readFile(reportPath, 'utf8')).budgets.archive.status, 'skipped');

  await writeFile(budgetPath, JSON.stringify(budget({ 'wasm.gzip': 3 })));
  const overLimit = spawnSync(process.execPath, argumentsFor(), { encoding: 'utf8' });
  assert.equal(overLimit.status, 1);
  assert.match(overLimit.stderr, /wasm\.gzip: 4 > 3 bytes/);
});

test('a budget that omits the archive limit is still rejected', async t => {
  const { packageRoot } = await fixture(t);
  const incomplete = budget();
  delete incomplete.limits.archive;
  await assert.rejects(
    createSizeReport({ packageRoot, archivePath: '', budget: incomplete }),
    /Size budget archive must define positive maxBytes and a rationale/,
  );
});

test('rejects packaged bytes that do not match the deployment manifest', async t => {
  const paths = await fixture(t);
  await writeFile(path.join(paths.packageRoot, 'assets/app.wasm.br'), 'tampered');
  await assert.rejects(
    createSizeReport({ ...paths, budget: budget() }),
    /app\.wasm\.br has 8 bytes; manifest records 3/,
  );
});
