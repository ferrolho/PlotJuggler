// SPDX-License-Identifier: MPL-2.0
const fs = require('fs');
const { expect } = require('@playwright/test');

async function saveLayoutActionCenter(page, screen, consoleMessages) {
  const fileMenuMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_FILE_MENU center='),
  ).length;
  await page.evaluate(() => window.pjWasmReportFileMenuProbe());
  await expect.poll(
    () => consoleMessages.filter(
      message => message.includes('PJ_WASM_FILE_MENU center='),
    ).length,
    { timeout: 10000 },
  ).toBeGreaterThan(fileMenuMessages);
  const fileMenuMessage = consoleMessages.filter(
    message => message.includes('PJ_WASM_FILE_MENU center='),
  ).at(-1) || '';
  const fileMenuCenter = fileMenuMessage.match(/center=(\d+),(\d+)/);
  expect(fileMenuCenter, `unparseable File menu geometry: ${fileMenuMessage}`).not.toBeNull();

  await page.mouse.click(
    screen.x + Number(fileMenuCenter[1]),
    screen.y + Number(fileMenuCenter[2]),
  );
  await page.waitForTimeout(250);
  const actionMessages = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=save_layout'),
  ).length;
  await page.evaluate(() => window.pjWasmReportSaveLayoutActionProbe());
  await expect.poll(
    () => consoleMessages.filter(
      message => message.includes('PJ_WASM_MENU_ACTION name=save_layout'),
    ).length,
    { timeout: 10000 },
  ).toBeGreaterThan(actionMessages);
  const actionMessage = consoleMessages.filter(
    message => message.includes('PJ_WASM_MENU_ACTION name=save_layout'),
  ).at(-1) || '';
  const actionCenter = actionMessage.match(/center=(\d+),(\d+)/);
  expect(actionCenter, `unparseable Save Layout geometry: ${actionMessage}`).not.toBeNull();

  return { x: Number(actionCenter[1]), y: Number(actionCenter[2]) };
}

async function downloadLayoutFromFileMenu(page, screen, consoleMessages) {
  const actionCenter = await saveLayoutActionCenter(page, screen, consoleMessages);
  const downloadPromise = page.waitForEvent('download', { timeout: 15000 });
  await page.mouse.click(screen.x + actionCenter.x, screen.y + actionCenter.y);
  const download = await downloadPromise;
  const downloadPath = await download.path();
  expect(downloadPath).not.toBeNull();
  return {
    fileName: download.suggestedFilename(),
    bytes: fs.readFileSync(downloadPath),
  };
}

module.exports = { downloadLayoutFromFileMenu, saveLayoutActionCenter };
