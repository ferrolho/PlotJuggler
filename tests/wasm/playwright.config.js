// SPDX-License-Identifier: MPL-2.0
const { defineConfig } = require('@playwright/test');

module.exports = defineConfig({
  use: {
    viewport: { width: 1280, height: 720 },
    screenshot: 'only-on-failure',
    launchOptions: process.env.PJ_CHROMIUM_EXECUTABLE
      ? { executablePath: process.env.PJ_CHROMIUM_EXECUTABLE }
      : {},
  },
});
