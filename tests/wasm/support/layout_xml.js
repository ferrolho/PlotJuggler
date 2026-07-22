// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

function elementAttribute(layoutBytes, tag, attribute) {
  const xml = layoutBytes.toString('utf8');
  const match = xml.match(new RegExp(`<${tag}\\b[^>]*\\b${attribute}="([^"]*)"`));
  expect(match, `missing ${tag}.${attribute} in serialized layout`).not.toBeNull();
  return match[1];
}


module.exports = { elementAttribute };
