// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');
const fs = require('fs');
const path = require('path');

const { waitForQtApp } = require('./support/app');
const { dragQtCanvas } = require('./support/canvas');
const { clickActiveDialogButton } = require('./support/dialogs');
const { downloadLayoutFromFileMenu } = require('./support/layout_actions');
const { openFileChooser, openLayoutChooser, openRobotChooser } = require('./support/pickers');
const { qtPointToCss, requestScene3DFoundationState } = require('./support/scene3d_foundation');

const fixture = name => fs.readFileSync(path.resolve(__dirname, 'fixtures', name));
const reportCount = messages => messages.filter(
  message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_SUMMARY'),
).length;

async function boot(page, messages) {
  await page.addInitScript(() => { delete window.showOpenFilePicker; });
  page.on('console', message => messages.push(message.text()));
  await page.goto(process.env.PJ_WASM_URL || 'http://127.0.0.1:6931/plotjuggler4.html');
  await waitForQtApp(page, messages);
  const screen = await page.locator('#screen').boundingBox();
  expect(screen).not.toBeNull();
  return screen;
}

async function loadFixture(page, screen, messages, name) {
  const chooser = await openFileChooser(page, screen);
  await chooser.setFiles({
    name,
    mimeType: 'application/octet-stream',
    buffer: fixture(name),
  });
  await page.waitForTimeout(800);
  await clickActiveDialogButton(page, screen, messages, 'ok');
  await page.waitForTimeout(500);
}

async function dropRows(page, screen, rows) {
  await page.mouse.click(screen.x + 10, screen.y + 192);
  const target = { x: screen.x + 820, y: screen.y + 390 };
  for (const y of rows) {
    await dragQtCanvas(page, { x: screen.x + 95, y: screen.y + y }, target);
    await page.waitForTimeout(300);
  }
}

async function waitForModels(page, messages, predicate, timeout = 30000) {
  let state;
  await expect.poll(async () => {
    state = await requestScene3DFoundationState(page, messages, reportCount(messages));
    return state.docks.length === 1 && predicate(state.docks[0].models, state.docks[0], state);
  }, { timeout }).toBe(true);
  return state;
}

async function clickQtControl(page, screen, state, control) {
  const center = qtPointToCss(screen, state, {
    x: control.x + control.width / 2,
    y: control.y + control.height / 2,
  });
  await page.mouse.click(center.x, center.y);
}

test('SceneEntities submits procedural geometry and all five PBR maps', async ({ page }) => {
  test.setTimeout(90000);
  const messages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const externalModel = fs.readFileSync(path.resolve(
    __dirname,
    '../../pj_scene3D/widgets/tests/fixtures/meshes/embedded_base.glb',
  ));
  await page.route('https://models.plotjuggler.test/embedded_base.glb', route => route.fulfill({
    status: 200,
    contentType: 'model/gltf-binary',
    headers: { 'access-control-allow-origin': '*' },
    body: externalModel,
  }));

  const screen = await boot(page, messages);
  await loadFixture(page, screen, messages, 'foxglove_scene_entities_real.mcap');
  // Sorted rows: /markers, /tf/a, /tf/b, /zz_too_many.
  await dropRows(page, screen, [232, 212]);

  const active = await waitForModels(page, messages, models =>
    models.markers.live === 1 && models.markers.decoding === 0 &&
    models.markers.rendered === 1 && models.models.ready >= 1 &&
    models.models.draws >= 1);
  const models = active.docks[0].models;
  expect(models.markers.offMain).toBe(models.markers.completed);
  expect(models.markers.families).toMatchObject({
    spheres: 2, cylinders: 1, arrows: 1, lineVertices: 8, triangleVertices: 6,
  });
  expect(models.markers.skipped).toMatchObject({ text: 1, models: 1, invalid: 0 });
  expect(models.models.bytes).toBeGreaterThan(0);
  expect(models.models.maps.baseColor).toBeGreaterThanOrEqual(1);
  expect(models.models.maps).toMatchObject({
    metallicRoughness: 1, normal: 1, occlusion: 1, emissive: 1,
  });
  expect(active.docks[0].readback.rgbSum).toBeGreaterThan(0);

  expect(messages.some(message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_FAILED'))).toBe(false);
  expect(messages.some(message => message.includes('QRhiWidget: No QRhi'))).toBe(false);
  expect(errors).toEqual([]);
});

test('a local URDF renders from browser-granted bytes and replays without claiming file access', async ({ page }) => {
  test.setTimeout(90000);
  const messages = [];
  const errors = [];
  page.on('pageerror', error => errors.push(String(error)));
  const screen = await boot(page, messages);
  await loadFixture(page, screen, messages, 'ros2_robot_description_real.mcap');
  // Sorted rows: /robot_description, /tf. TF creates the dock and map frame.
  await dropRows(page, screen, [232]);

  let state = await waitForModels(page, messages, (_models, dock) => dock.fixed === 'map');
  await clickQtControl(page, screen, state, state.docks[0].data.rightPanel.toggle);
  state = await waitForModels(page, messages, models =>
    models.robots.sourceIndex >= 0);
  expect(state.docks[0].models.robots.sourceIndex).toBe(0);

  const robotChooser = await openRobotChooser(page);
  await robotChooser.setFiles({
    name: 'browser_robot.urdf',
    mimeType: 'application/xml',
    buffer: fixture('browser_robot.urdf'),
  });
  const local = await waitForModels(page, messages, models =>
    models.robots.live === 1 && models.robots.sourceType === 1 && models.robots.draws === 3 &&
    models.models.draws === 3);
  expect(local.docks[0].models.robots).toMatchObject({
    meshes: 4, visual: 3, collision: 0, placeholders: 1, bridges: 2,
    source: 'browser_robot.urdf',
  });
  expect(local.docks[0].models.robots.status.join(' '))
    .toContain('1 mesh reference(s) unavailable in the browser sandbox');
  const downloaded = await downloadLayoutFromFileMenu(page, screen, messages);
  const xml = downloaded.bytes.toString('utf8');
  expect(xml).toMatch(
    /<robot_model\b(?=[^>]*\bsource_type="file")(?=[^>]*\bsource_value="browser_robot\.urdf")[^>]*>/,
  );
  expect(xml).not.toContain('map_to_arm');
  expect(xml).not.toContain('not_granted_by_browser');
  expect(xml).not.toContain('pj-upload://');

  const layoutChooser = await openLayoutChooser(page);
  await layoutChooser.setFiles({
    name: 'scene3d-local-robot.pj4.xml',
    mimeType: 'application/xml',
    buffer: downloaded.bytes,
  });
  await expect.poll(
    () => messages.find(message => message.includes('PJ_WASM_LAYOUT_LOAD_OK name=scene3d-local-robot')) || '',
    { timeout: 15000 },
  ).toContain('scene3d-local-robot.pj4.xml');
  const restored = await waitForModels(page, messages, models =>
    models.robots.live === 1 && models.robots.sourceType === 1 && models.robots.draws === 0);
  expect(restored.docks[0].identity).not.toBe(local.docks[0].identity);
  expect(restored.docks[0].models.robots.source).toBe('browser_robot.urdf');
  expect(restored.docks[0].models.robots.status.join(' '))
    .toContain('Select the local URDF again; browser layouts cannot retain host-file access');
  expect(restored.docks[0].readback.rgbSum).toBeGreaterThan(0);
  expect(errors).toEqual([]);
});
