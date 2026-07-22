// SPDX-License-Identifier: MPL-2.0

async function dragQtCanvas(page, from, to, button = 'left') {
  await page.mouse.move(from.x, from.y);
  await page.mouse.down({ button });
  await page.waitForTimeout(150);
  await page.mouse.move(from.x + 80, from.y + 20, { steps: 6 });
  await page.mouse.move(to.x, to.y, { steps: 18 });
  await page.waitForTimeout(250);
  await page.mouse.up({ button });
}


module.exports = { dragQtCanvas };
