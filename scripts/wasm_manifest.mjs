// Copyright 2026 Davide Faconti
// SPDX-License-Identifier: MPL-2.0

import { createHash } from 'node:crypto';

export function sha256(contents) {
  return createHash('sha256').update(contents).digest('hex');
}

export function representation(pathName, contents, contentEncoding = undefined) {
  const result = {
    path: pathName,
    bytes: contents.length,
    sha256: sha256(contents),
  };
  if (contentEncoding) {
    result.contentEncoding = contentEncoding;
  }
  return result;
}
