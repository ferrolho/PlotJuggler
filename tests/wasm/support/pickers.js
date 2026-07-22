// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

async function openFileChooser(page, screen, useProbe = false) {
  let lastError;
  for (let attempt = 0; attempt < 5; ++attempt) {
    try {
      const chooserPromise = page.waitForEvent('filechooser', { timeout: 8000 });
      if (attempt === 0 && !useProbe) {
        // Exercise the real user-facing canvas button first.
        await page.mouse.click(screen.x + 15, screen.y + 80);
      } else {
        // Probe builds expose the same MainWindow slot. This avoids turning
        // a dropped synthetic canvas gesture into a parser/ingest false failure.
        await page.evaluate(() => window.pjWasmOpenLoadProbe());
      }
      return await chooserPromise;
    } catch (error) {
      lastError = error;
      // A cold threaded-WASM page can expose its canvas before Qt has painted
      // the actionable frame. A just-destroyed Qt dialog can likewise consume
      // one click while restoring the active window. Retrying a real gesture is
      // safe in both cases and avoids relying on a fixed machine-speed delay.
      await page.waitForTimeout(750);
    }
  }
  throw lastError;
}


async function openLayoutChooser(page) {
  const hasProbe = await page.evaluate(() => typeof window.pjWasmOpenLayoutProbe === 'function');
  if (!hasProbe) {
    throw new Error('layout picker probe not installed — check EM_JS probe installers');
  }
  let lastError;
  for (let attempt = 0; attempt < 5; ++attempt) {
    try {
      const chooserPromise = page.waitForEvent('filechooser', { timeout: 8000 });
      await page.evaluate(() => window.pjWasmOpenLayoutProbe());
      return await chooserPromise;
    } catch (error) {
      lastError = error;
      await page.waitForTimeout(750);
    }
  }
  throw lastError;
}


async function openSourceReplayChooser(page, screen, geometry, consoleMessages) {
  let lastError;
  for (let attempt = 0; attempt < 5; ++attempt) {
    const previousRequests = consoleMessages.filter(
      message => message.includes('Opening browser file picker with filter:'),
    ).length;
    try {
      // A cold threaded-WASM page can emit the production request before
      // Chromium delivers its native chooser event. Keep the physical
      // click/retry path and its established delivery window; the multi-source
      // stress scenario additionally bounds each independent replay transaction
      // to a fresh Playwright Page target.
      const chooserPromise = page.waitForEvent('filechooser', { timeout: 30000 });
      // Every attempt remains a physical click on the production Reselect
      // button. The button deliberately stays open until a picker callback, so
      // a Qt/browser request that emits no chooser can be retried by a user.
      await page.mouse.click(screen.x + geometry.reselect.x, screen.y + geometry.reselect.y);
      await expect.poll(
        () => consoleMessages.filter(
          message => message.includes('Opening browser file picker with filter:'),
        ).length,
        { timeout: 5000 },
      ).toBeGreaterThan(previousRequests);
      return await chooserPromise;
    } catch (error) {
      lastError = error;
      await page.waitForTimeout(750);
    }
  }
  throw lastError;
}


module.exports = { openFileChooser, openLayoutChooser, openSourceReplayChooser };
