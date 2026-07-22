// SPDX-License-Identifier: MPL-2.0
const fs = require('fs');
const path = require('path');

function base64Fixture(name) {
  const encoded = fs.readFileSync(
    path.resolve(__dirname, '..', 'fixtures', name),
    'utf8',
  );
  return Buffer.from(encoded.trim(), 'base64');
}


module.exports = { base64Fixture };
