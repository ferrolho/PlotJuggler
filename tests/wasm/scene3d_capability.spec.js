// SPDX-License-Identifier: MPL-2.0
const { test, expect } = require('@playwright/test');

const COMPLETE_PREFIX = 'PJ_WASM_SCENE3D_CAP_COMPLETE ';
const WEBGL_PREFIX = 'PJ_WASM_SCENE3D_CAP_WEBGL ';

function parseReport(messages, prefix) {
  const message = messages.find((entry) => entry.startsWith(prefix));
  return message ? JSON.parse(message.slice(prefix.length)) : null;
}

test('retained Scene3D QRhi subset renders in two live widgets', async ({ page, browserName }, testInfo) => {
  const messages = [];
  const pageErrors = [];
  page.on('console', (message) => messages.push(message.text()));
  page.on('pageerror', (error) => pageErrors.push(error.message));

  await page.goto(
    process.env.PJ_WASM_SCENE3D_CAP_URL ||
      'http://127.0.0.1:6931/scene3d_capability_probe.html',
  );

  await expect
    .poll(() => parseReport(messages, COMPLETE_PREFIX), {
      message: `waiting for the ${browserName} QRhi capability report`,
    })
    .not.toBeNull();
  await expect
    .poll(() => parseReport(messages, WEBGL_PREFIX), {
      message: `waiting for the ${browserName} WebGL report`,
    })
    .not.toBeNull();

  const complete = parseReport(messages, COMPLETE_PREFIX);
  const webgl = parseReport(messages, WEBGL_PREFIX);
  await testInfo.attach(`scene3d-capability-${browserName}.json`, {
    body: Buffer.from(JSON.stringify({ webgl, complete }, null, 2)),
    contentType: 'application/json',
  });
  console.log(`PJ_WASM_SCENE3D_CAP_RESULT ${browserName} ${JSON.stringify({ webgl, complete })}`);

  expect(pageErrors).toEqual([]);
  expect(webgl.webgl2).toBe(true);
  expect(webgl.version).toContain('WebGL 2.0');
  expect(webgl.shading_language).toContain('3.00');
  expect(webgl.max_3d_texture_size).toBeGreaterThanOrEqual(256);
  expect(webgl.color_buffer_float).toBe(true);
  expect(webgl.float_linear).toBe(true);
  expect(webgl.cross_origin_isolated).toBe(true);
  expect(webgl.shared_array_buffer).toBe(true);
  expect(complete.schema).toBe(1);
  expect(complete.same_rhi).toBe(true);
  expect(complete.ok).toBe(true);
  expect(complete.widgets).toHaveLength(2);
  expect(complete.evidence).toHaveLength(2);

  for (const widget of complete.widgets) {
    expect(widget.frames).toBeGreaterThanOrEqual(3);
    expect(widget.render_failed).toBe(false);
    expect(widget.resources_ready).toBe(true);
    expect(widget.resource_setup_ok).toBe(true);
    expect(widget.actual_sample_count).toBe(4);
    expect(widget.volume_texture_created).toBe(true);
    expect(widget.sample_float_target_created).toBe(true);
    expect(widget.float_sample_uses_rgba16f).toBe(true);
    expect(widget.supported_sample_counts).toEqual(expect.arrayContaining([1, 2, 4]));
    expect(widget.features.multisample_renderbuffer).toBe(true);
    expect(widget.features.multisample_texture).toBe(false);
    expect(widget.features.instancing).toBe(true);
    expect(widget.features.element_index_uint).toBe(true);
    expect(widget.features.vertex_shader_point_size).toBe(true);
    expect(widget.features.texel_fetch).toBe(true);
    expect(widget.features.three_dimensional_textures).toBe(true);
    expect(widget.features.compute).toBe(false);
    for (const format of ['rgba16f', 'rgba32f', 'r16f', 'r32f']) {
      expect(widget.float_targets[format]).toEqual({
        advertised: true,
        render_target: true,
        texture: true,
      });
    }
  }
  for (const evidence of complete.evidence) {
    expect(evidence.ok).toBe(true);
    expect(evidence.vertex_stage_3d_texel_fetch).toBe(true);
    expect(evidence.volume_pixels).toBeGreaterThan(20_000);
    expect(evidence.float_pixels).toBeGreaterThan(20_000);
    expect(evidence.left_point_pixels).toBeGreaterThan(1_000);
    expect(evidence.right_point_pixels).toBeGreaterThan(1_000);
    expect(evidence.uint32_index_yellow).toBeGreaterThan(500);
  }
});
