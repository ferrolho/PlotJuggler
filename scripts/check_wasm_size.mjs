#!/usr/bin/env node
// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

import { mkdir, readFile, stat, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

import { sha256 } from './wasm_manifest.mjs';

const SCRIPT_DIR = path.dirname(fileURLToPath(import.meta.url));
const REPOSITORY_ROOT = path.resolve(SCRIPT_DIR, '..');
const DEPLOYMENT_FORMAT = 'org.plotjuggler.wasm-deployment.v1';
const BUDGET_FORMAT = 'org.plotjuggler.wasm-size-budget.v1';
const REPORT_FORMAT = 'org.plotjuggler.wasm-size-report.v1';
const REQUIRED_LIMITS = [
  'wasm.identity',
  'wasm.br',
  'wasm.gzip',
  'archive',
];
// A budget must DEFINE every required limit — it is the release contract, not a
// per-invocation switch. Only the archive limit may go unmeasured, because the
// deterministic release archive is a release-pipeline artifact that the ordinary
// browser CI never builds. Its evaluation is then reported as `skipped`, never
// silently dropped, so a report can never be mistaken for a full release gate.
const OPTIONAL_MEASUREMENTS = new Set(['archive']);

function usage() {
  return `Usage: node scripts/check_wasm_size.mjs [options]

Options:
  --package <dir>   Packaged deployment root (default: build-wasm/deploy-a)
  --archive <file>  Deterministic release archive; omit to skip the archive limit
  --budget <file>   Size budget definition (default: tests/wasm/size_budget.json)
  --output <file>   Structured report path (default: build-wasm/wasm-size-report.json)
  --help            Show this help
`;
}

function parseArguments(argv) {
  const options = {
    package: path.join(REPOSITORY_ROOT, 'build-wasm', 'deploy-a'),
    archive: '',
    budget: path.join(REPOSITORY_ROOT, 'tests', 'wasm', 'size_budget.json'),
    output: path.join(REPOSITORY_ROOT, 'build-wasm', 'wasm-size-report.json'),
  };

  for (let index = 0; index < argv.length; index += 1) {
    const argument = argv[index];
    if (argument === '--help') {
      process.stdout.write(usage());
      process.exit(0);
    }
    if (!['--package', '--archive', '--budget', '--output'].includes(argument)) {
      throw new Error(`Unknown argument: ${argument}\n\n${usage()}`);
    }
    const value = argv[index + 1];
    if (!value || value.startsWith('--')) {
      throw new Error(`${argument} requires a value`);
    }
    options[argument.slice(2)] = value;
    index += 1;
  }
  for (const key of Object.keys(options)) {
    if (options[key]) {
      options[key] = path.resolve(options[key]);
    }
  }
  return options;
}

async function readJson(fileName, description) {
  try {
    return JSON.parse(await readFile(fileName, 'utf8'));
  } catch (error) {
    throw new Error(`Cannot read ${description} ${fileName}: ${error.message}`, { cause: error });
  }
}

function safePackagePath(packageRoot, relativePath) {
  if (typeof relativePath !== 'string' || relativePath.length === 0) {
    throw new Error('Deployment manifest contains an empty representation path');
  }
  const candidate = path.resolve(packageRoot, relativePath);
  const relative = path.relative(packageRoot, candidate);
  if (relative.startsWith(`..${path.sep}`) || relative === '..' || path.isAbsolute(relative)) {
    throw new Error(`Deployment representation escapes package root: ${relativePath}`);
  }
  return candidate;
}

async function verifyRepresentation(packageRoot, representation) {
  if (!representation || !Number.isSafeInteger(representation.bytes)
      || representation.bytes < 0 || !/^[0-9a-f]{64}$/.test(representation.sha256 || '')) {
    throw new Error('Deployment manifest contains an invalid representation record');
  }
  const contents = await readFile(safePackagePath(packageRoot, representation.path));
  if (contents.length !== representation.bytes) {
    throw new Error(
      `${representation.path} has ${contents.length} bytes; manifest records ${representation.bytes}`,
    );
  }
  const digest = sha256(contents);
  if (digest !== representation.sha256) {
    throw new Error(`${representation.path} does not match its manifest SHA-256`);
  }
  return { ...representation };
}

function measurementFor(limitName, measurements) {
  switch (limitName) {
    case 'wasm.identity':
      return measurements.wasm.identity.bytes;
    case 'wasm.br':
      return measurements.wasm.br.bytes;
    case 'wasm.gzip':
      return measurements.wasm.gzip.bytes;
    case 'archive':
      return measurements.archive?.bytes;
    default:
      throw new Error(`Unknown size limit: ${limitName}`);
  }
}

export function evaluateBudgets(measurements, budget) {
  if (budget.format !== BUDGET_FORMAT || typeof budget.profile !== 'string'
      || !budget.profile || !budget.limits || typeof budget.limits !== 'object'
      || !budget.contributorAudit || typeof budget.contributorAudit !== 'object') {
    throw new Error(`Size budget must use ${BUDGET_FORMAT} and name a profile`);
  }
  const unknownLimits = Object.keys(budget.limits)
    .filter(name => !REQUIRED_LIMITS.includes(name));
  if (unknownLimits.length > 0) {
    throw new Error(`Unknown size limit(s): ${unknownLimits.join(', ')}`);
  }

  const evaluations = {};
  const failures = [];
  for (const name of REQUIRED_LIMITS) {
    const definition = budget.limits[name];
    if (!definition || !Number.isSafeInteger(definition.maxBytes)
        || definition.maxBytes <= 0 || typeof definition.rationale !== 'string'
        || !definition.rationale.trim()) {
      throw new Error(`Size budget ${name} must define positive maxBytes and a rationale`);
    }
    const actualBytes = measurementFor(name, measurements);
    if (actualBytes === undefined && OPTIONAL_MEASUREMENTS.has(name)) {
      evaluations[name] = {
        maxBytes: definition.maxBytes,
        status: 'skipped',
        rationale: definition.rationale,
      };
      continue;
    }
    if (!Number.isSafeInteger(actualBytes) || actualBytes <= 0) {
      throw new Error(`Size measurement ${name} must be a positive integer`);
    }
    const headroomBytes = definition.maxBytes - actualBytes;
    const passed = headroomBytes >= 0;
    evaluations[name] = {
      actualBytes,
      maxBytes: definition.maxBytes,
      headroomBytes,
      headroomPercent: Number(((headroomBytes / actualBytes) * 100).toFixed(2)),
      status: passed ? 'pass' : 'fail',
      rationale: definition.rationale,
    };
    if (!passed) {
      failures.push(`${name}: ${actualBytes} > ${definition.maxBytes} bytes`);
    }
  }
  return { evaluations, failures };
}

export async function createSizeReport({ packageRoot, archivePath, budget }) {
  const manifest = await readJson(path.join(packageRoot, 'manifest.json'), 'deployment manifest');
  if (manifest.format !== DEPLOYMENT_FORMAT || !Array.isArray(manifest.files)) {
    throw new Error(`Deployment manifest must use ${DEPLOYMENT_FORMAT}`);
  }

  const verifiedFiles = [];
  const logicalPaths = new Set();
  for (const file of manifest.files) {
    if (!file || typeof file.path !== 'string' || logicalPaths.has(file.path)
        || file.representations?.identity?.path !== file.path) {
      throw new Error('Deployment manifest contains an invalid file record');
    }
    logicalPaths.add(file.path);
    const representations = {};
    for (const encoding of ['identity', 'br', 'gzip']) {
      if (file.representations[encoding]) {
        const suffix = encoding === 'gzip' ? 'gz' : encoding;
        const expectedPath = encoding === 'identity' ? file.path : `${file.path}.${suffix}`;
        if (file.representations[encoding].path !== expectedPath) {
          throw new Error(`Deployment manifest has an invalid ${encoding} path for ${file.path}`);
        }
        representations[encoding] = await verifyRepresentation(
          packageRoot,
          file.representations[encoding],
        );
      }
    }
    verifiedFiles.push({ path: file.path, representations });
  }

  const wasmFiles = verifiedFiles.filter(file => file.path.endsWith('.wasm'));
  if (wasmFiles.length !== 1) {
    throw new Error(`Expected exactly one logical Wasm module; found ${wasmFiles.length}`);
  }
  const wasm = wasmFiles[0].representations;
  for (const encoding of ['identity', 'br', 'gzip']) {
    if (!wasm[encoding]) {
      throw new Error(`Wasm module has no ${encoding} representation`);
    }
  }

  const servedAssets = {};
  for (const encoding of ['identity', 'br', 'gzip']) {
    let bytes = 0;
    for (const file of verifiedFiles) {
      bytes += (file.representations[encoding] || file.representations.identity).bytes;
    }
    servedAssets[encoding] = { bytes };
  }

  const measurements = {
    wasm: {
      identity: { bytes: wasm.identity.bytes, sha256: wasm.identity.sha256 },
      br: { bytes: wasm.br.bytes, sha256: wasm.br.sha256 },
      gzip: { bytes: wasm.gzip.bytes, sha256: wasm.gzip.sha256 },
    },
    servedAssets,
  };
  if (archivePath) {
    const archiveStats = await stat(archivePath);
    if (!archiveStats.isFile()) {
      throw new Error(`Release archive is not a regular file: ${archivePath}`);
    }
    const archiveContents = await readFile(archivePath);
    measurements.archive = {
      bytes: archiveContents.length,
      sha256: sha256(archiveContents),
    };
  }
  const { evaluations, failures } = evaluateBudgets(measurements, budget);

  return {
    report: {
      format: REPORT_FORMAT,
      profile: budget.profile,
      bundleId: manifest.bundleId,
      measurements,
      budgets: evaluations,
      contributorAudit: budget.contributorAudit,
      status: failures.length === 0 ? 'pass' : 'fail',
    },
    failures,
  };
}

async function main() {
  const options = parseArguments(process.argv.slice(2));
  const budget = await readJson(options.budget, 'size budget');
  const { report, failures } = await createSizeReport({
    packageRoot: options.package,
    archivePath: options.archive,
    budget,
  });
  await mkdir(path.dirname(options.output), { recursive: true });
  await writeFile(options.output, `${JSON.stringify(report, null, 2)}\n`, { mode: 0o644 });
  if (failures.length > 0) {
    throw new Error(`WebAssembly size budget exceeded:\n${failures.join('\n')}`);
  }
  const skipped = Object.entries(report.budgets)
    .filter(([, evaluation]) => evaluation.status === 'skipped')
    .map(([name]) => name);
  const skippedNote = skipped.length > 0 ? ` (not measured: ${skipped.join(', ')})` : '';
  process.stdout.write(
    `WebAssembly size budget passed for ${report.profile}${skippedNote}; report: ${options.output}\n`,
  );
}

if (process.argv[1] && path.resolve(process.argv[1]) === fileURLToPath(import.meta.url)) {
  main().catch(error => {
    process.stderr.write(`${error.stack || error}\n`);
    process.exitCode = 1;
  });
}
