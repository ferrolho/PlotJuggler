// SPDX-License-Identifier: MPL-2.0
const { expect } = require('@playwright/test');

async function requestScene3DFoundationState(page, consoleMessages, previousCount) {
  await page.evaluate(() => window.pjWasmReportScene3DFoundationProbe());
  await expect.poll(
    () => consoleMessages.filter(
      message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_SUMMARY'),
    ).length,
    { timeout: 10000 },
  ).toBeGreaterThan(previousCount);

  const summaryMessage = consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_SUMMARY'),
  ).at(-1) || '';
  const summary = summaryMessage.match(
    /sequence=(\d+) docks=(\d+) slider=(-?\d+),(-?\d+),(\d+)x(\d+) value=([^ ]+) range=([^,]+),([^ ]+) enabled=(\d+) window=(\d+)x(\d+)/,
  );
  expect(summary, `unparseable Scene3D summary: ${summaryMessage}`).not.toBeNull();
  const sequence = Number(summary[1]);
  const dockCount = Number(summary[2]);
  const dockMessages = () => consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE3D_FOUNDATION_DOCK')
      && message.includes(`sequence=${sequence} `),
  );
  await expect.poll(() => dockMessages().length).toBe(dockCount);
  const dataMessages = () => consoleMessages.filter(
    message => message.includes('PJ_WASM_SCENE3D_DATA')
      && message.includes(`sequence=${sequence} `),
  );
  await expect.poll(() => dataMessages().length).toBe(dockCount);
  const dataByIndex = new Map(dataMessages().map((message) => {
    const match = message.match(
      /index=(\d+) points=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+),(\d+) compressed=(\d+) depth=(\d+),(\d+),(\d+),(\d+),(\d+),(\d+) poses=(\d+),(\d+) occupancy=(\d+),(\d+),(\d+),(\d+) voxels=(\d+),(\d+),(\d+),(\d+) order=([^ ]*) submitted=([^ ]*) list=(-?\d+),(-?\d+),(\d+)x(\d+) rows=([^ ]*) toggle=(-?\d+),(-?\d+),(\d+)x(\d+) right_visible=(\d+) warnings=([^ ]*)/,
    );
    expect(match, `unparseable Scene3D data state: ${message}`).not.toBeNull();
    const ids = encoded => decodeURIComponent(encoded).split(',').filter(Boolean).map(Number);
    return [Number(match[1]), {
      points: {
        live: Number(match[2]), rendered: Number(match[3]), vertices: Number(match[4]),
        starts: Number(match[5]), completed: Number(match[6]), offMain: Number(match[7]),
        decoding: Number(match[8]), compressed: Number(match[9]),
      },
      depth: {
        live: Number(match[10]), decoding: Number(match[11]), starts: Number(match[12]),
        completed: Number(match[13]), offMain: Number(match[14]), vertices: Number(match[15]),
      },
      poses: { live: Number(match[16]), arms: Number(match[17]) },
      occupancy: {
        live: Number(match[18]), cells: Number(match[19]),
        fullUploads: Number(match[20]), partialUploads: Number(match[21]),
      },
      voxels: {
        live: Number(match[22]), count: Number(match[23]), uploads: Number(match[24]), max3d: Number(match[25]),
      },
      order: ids(match[26]),
      submitted: ids(match[27]),
      list: { x: Number(match[28]), y: Number(match[29]), width: Number(match[30]), height: Number(match[31]) },
      rows: decodeURIComponent(match[32]).split(',').filter(Boolean).map((entry) => {
        const [id, x, y] = entry.split(':').map(Number);
        return { id, x, y };
      }),
      rightPanel: {
        toggle: {
          x: Number(match[33]), y: Number(match[34]), width: Number(match[35]), height: Number(match[36]),
        },
        visible: match[37] === '1',
      },
      warnings: decodeURIComponent(match[38]).trim().split('|').filter(Boolean),
    }];
  }));

  const docks = dockMessages().map((message) => {
    const match = message.match(
      /index=(\d+) identity=(\d+) view=(-?\d+),(-?\d+),(\d+)x(\d+) visible=(\d+) frames=([^ ]*) fixed=([^ ]*) auto=(\d+) follow=([^ ]*) model=(-?\d+) camera=([^,]+),([^,]+),([^,]+),([^,]+),([^,]+),([^,]+),([^ ]+) vertices=(\d+),(\d+) resolved=(\d+) fixed_control=(-?\d+),(-?\d+),(\d+)x(\d+) camera_control=(-?\d+),(-?\d+),(\d+)x(\d+) home=(-?\d+),(-?\d+),(\d+)x(\d+) frame_hash=(\d+) rgb_sum=(\d+) framebuffer=(\d+)x(\d+)/,
    );
    expect(match, `unparseable Scene3D dock: ${message}`).not.toBeNull();
    return {
      index: Number(match[1]),
      identity: Number(match[2]),
      view: {
        x: Number(match[3]), y: Number(match[4]),
        width: Number(match[5]), height: Number(match[6]),
      },
      visible: match[7] === '1',
      frames: decodeURIComponent(match[8]).split(',').filter(Boolean),
      fixed: decodeURIComponent(match[9]),
      auto: match[10] === '1',
      follow: decodeURIComponent(match[11]),
      model: Number(match[12]),
      camera: {
        focalX: Number(match[13]), focalY: Number(match[14]), focalZ: Number(match[15]),
        radius: Number(match[16]), azimuth: Number(match[17]), elevation: Number(match[18]),
        orthoScale: Number(match[19]),
      },
      vertices: { lines: Number(match[20]), triangles: Number(match[21]) },
      resolved: Number(match[22]),
      controls: {
        fixed: {
          x: Number(match[23]), y: Number(match[24]),
          width: Number(match[25]), height: Number(match[26]),
        },
        camera: {
          x: Number(match[27]), y: Number(match[28]),
          width: Number(match[29]), height: Number(match[30]),
        },
        home: {
          x: Number(match[31]), y: Number(match[32]),
          width: Number(match[33]), height: Number(match[34]),
        },
      },
      readback: {
        frameHash: match[35],
        rgbSum: Number(match[36]),
        width: Number(match[37]),
        height: Number(match[38]),
      },
      data: dataByIndex.get(Number(match[1])),
    };
  });

  return {
    sequence,
    window: { width: Number(summary[11]), height: Number(summary[12]) },
    slider: {
      x: Number(summary[3]), y: Number(summary[4]),
      width: Number(summary[5]), height: Number(summary[6]),
      value: Number(summary[7]), min: Number(summary[8]), max: Number(summary[9]),
      enabled: summary[10] === '1',
    },
    docks,
  };
}

function qtPointToCss(screen, state, point) {
  return {
    x: screen.x + (point.x * screen.width / state.window.width),
    y: screen.y + (point.y * screen.height / state.window.height),
  };
}

module.exports = { qtPointToCss, requestScene3DFoundationState };
