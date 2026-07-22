// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

async function activeDialogButtonGeometry(page, consoleMessages) {
  const previous = consoleMessages.filter(
    message => message.includes('PJ_WASM_DIALOG_BUTTONS ok='),
  ).length;
  await page.evaluate(() => window.pjWasmReportDialogButtonsProbe());
  await expect.poll(
    () => consoleMessages.filter(message => message.includes('PJ_WASM_DIALOG_BUTTONS ok=')).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previous);
  const message = consoleMessages.filter(
    entry => entry.includes('PJ_WASM_DIALOG_BUTTONS ok='),
  ).at(-1) || '';
  const match = message.match(/ok=(-?\d+),(-?\d+) cancel=(-?\d+),(-?\d+)/);
  expect(match, `unparseable active-dialog button geometry: ${message}`).not.toBeNull();
  return {
    ok: { x: Number(match[1]), y: Number(match[2]) },
    cancel: { x: Number(match[3]), y: Number(match[4]) },
  };
}


async function clickActiveDialogButton(page, screen, consoleMessages, button) {
  // Canonical host chrome may legitimately change the dialog's size and
  // centering. Query Qt for the live canvas-global button center, then retain a
  // physical Playwright mouse click through the same production event path.
  await page.waitForTimeout(250);
  const geometry = await activeDialogButtonGeometry(page, consoleMessages);
  const point = geometry[button];
  expect(point.x, `${button} button is absent from the active dialog`).toBeGreaterThanOrEqual(0);
  expect(point.y, `${button} button is absent from the active dialog`).toBeGreaterThanOrEqual(0);
  await page.mouse.click(screen.x + point.x, screen.y + point.y);
}


module.exports = { activeDialogButtonGeometry, clickActiveDialogButton };
