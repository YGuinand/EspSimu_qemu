// Driver de mesure POC B — boot / cadence UART / teardown, QEMU wasm32 TCI.
// La composition suit EspSimu desktop : argv identiques (voir esp32backend.cpp).
// Canal de mesure : UART0 -> chardev stdio (logs Node, horodates, sans table
// cross-thread : les callbacks addFunction ne traversent pas les pthreads en
// emcc 3.1.50, cf. cb_test.) Teardown: qemu_system_shutdown_request(export),
// puis mesure de qemu_cleanup (chemin EspSimu exact).
//
// Usage : node driver.mjs [firmware]     (default firmware_merged.bin)
import fs from 'node:fs';
import path from 'node:path';

const DRIVERDIR = '/tmp/opencode/pocb-driver';
const MODDIR = '/tmp/opencode/qemu-build-407';
const FIRMWARE = process.argv[2] ?? `${DRIVERDIR}/firmware_merged.bin`;
const ROMDIR = '/home/yves/tmp/qt/opencode/EspSimu/pc-bios';
const RUN_SECONDS = 15;
const QUIESCE_MS = 3000; // attente du retour de main() du worker post-shutdown (sort en ~4 ms reellement observe)

const t0 = performance.now();
const events = [];            // {t, kind, len}  (kind: uart|msg)
let   bootedAt = null;        // t du premier octet UART via stdout
let   lastUartT = null;
let   qemuInitT = null;       // t de Module.ready (fin init runtime ~= juste avant main)
let   shutdownReqT = null;
let   mainExitedT = null;      // retour de main() du worker (apres son qemu_cleanup in-thread)
let   mainExitStatus = null;
let   cleanupT = null;
let   cleanupDoneT = null;
let   loopQuiesceT = null;
let   rawLines = 0;

const pushes = [];
for (const k of ['uart', 'msg']) { pushes.push({ kind: k, acc: [] }); }
let err = null;

const argv = [
  '-M', 'esp32-picsimlab',
  '-L', '/bios',
  '-display', 'none',
  '-serial', 'null',
  '-monitor', 'none',
  '-global', 'driver=timer.esp32.timg,property=wdt_disable,value=true',
  '-drive', `file=/fw.bin,if=mtd,format=raw`,
];

const { default: ModuleFactory } = await import(path.join(MODDIR, 'qemu-system-xtensa.js'));

let M = null;
const CFG = { closme: null, preRun: [ () => { mountMemfs(CFG); } ] };
function mountMemfs(cfg) {
      const FS = cfg.FS;
      const MODDIR = '/tmp/opencode/qemu-build-407';
      const ROMDIR = '/home/yves/tmp/qt/opencode/EspSimu/pc-bios';
      FS.mkdir('/bios');
      for (const f of ['esp32-v3-rom.bin', 'esp32-v3-rom-app.bin']) {
        const buf = fs.readFileSync(`${ROMDIR}/${f}`);
        FS.writeFile(`/bios/${f}`, buf);
      }
      const fw = fs.readFileSync(FIRMWARE);
      FS.writeFile('/fw.bin', fw);
      console.error(`[driver] MEMFS: /bios + /fw.bin (${fw.length} o) installes`);
    }
CFG.onRuntimeInitialized = () => {
  try {
    CFG._qemu_picsimlab_register_callbacks(CFG._qemu_picsimlab_ring_callbacks());
    console.error('[driver] callbacks ring enregistres (shim C), avant callMain');
  } catch (e) {
    console.error(`[driver] registration callbacks FAIL: ${e}`);
  }
};

CFG.arguments = argv;
CFG.print = (text) => {
  const t = performance.now() - t0;
  bootedAt ??= t;
  lastUartT = t;
  rawLines++;
  events.push({ t, kind: 'uart', len: String(text).length + 1 });
  console.log(`[uart +${t | 0}ms] ${text}`);
};
CFG.printErr = (text) => {
  const t = performance.now() - t0;
  events.push({ t, kind: 'msg', len: String(text).length + 1 });
  console.error(`[qemu +${t | 0}ms] ${text}`);
  const m = /program exited \(with status: (\d+)\)/.exec(String(text));
  if (m) {
    mainExitedT ??= t;
    mainExitStatus = m[1];
  }
};

const Module = await ModuleFactory(CFG);
M = Module;
qemuInitT = performance.now() - t0;

console.error(`[driver] runtime ready a +${qemuInitT|0} ms ; run de ${RUN_SECONDS}s puis shutdown_request`);

function probeBaud() {
  try {
    const ptr = M._qemu_picsimlab_get_internals(7); // QEMU_INTERNAL_UART0_BAUD
    if (!ptr) return 'null';
    return M.HEAPU32[ptr >> 2];
  } catch (e) { return `err:${e}`; }
}

let ringInfoPtr = 0;
let ringText = '';
function drainRing() {
  if (!ringInfoPtr) {
    try { ringInfoPtr = M._qemu_picsimlab_uart_ring_info(); } catch { return; }
  }
  if (!ringInfoPtr) return;
  const base = ringInfoPtr >> 2;
  const head = M.HEAPU32[base + 1];
  let tail = M.HEAPU32[base + 2];
  if (head === tail) return;
  const dataPtr = M.HEAPU32[base + 4];
  const t = performance.now() - t0;
  bootedAt ??= t;
  while (tail !== head) {
    const off = (tail * 2) & 262143;
    const id = M.HEAPU8[dataPtr + off];
    const val = M.HEAPU8[dataPtr + ((off + 1) & 262143)];
    rawLines++;
    events.push({ t, kind: 'uart', len: 1, id, val });
    if (id === 0 && ringText.length < 8000) {
      if (val === 10) { ringText += '\n'; } else if (val !== 13 && val >= 32) { ringText += String.fromCharCode(val); }
    }
    tail = (tail + 1) >>> 0;
  }
  M.HEAPU32[base + 2] = tail;
  lastUartT = t;
}

// watchdog de mesure
const wd = setInterval(() => {
  const elapsed = (performance.now() - t0) / 1000;
  try { drainRing(); } catch (e) { err ??= `drainRing: ${e}`; }
  const at = [];
  if (shutdownReqT === null && elapsed >= RUN_SECONDS) {
    shutdownReqT = performance.now() - t0;
    try {
      Module._qemu_system_shutdown_request(5); // SHUTDOWN_CAUSE_HOST_UI (facade EspSimu desktop)
      console.error(`[driver] shutdown_request envoye a +${shutdownReqT|0} ms`);
    } catch (e) { err ??= `shutdown_request: ${e}`; }
    return;
  }
  // V2 : le worker qui a tourne main() fait lui-meme qemu_cleanup in-thread
  // (system/main.c : qemu_main_loop(); qemu_cleanup(status);) — on attend son
  // retour ("program exited" du glue) au lieu de forcer le cleanup du loader.
  if (mainExitedT !== null && cleanupT === null) {
    cleanupT = mainExitedT;
    if (mainExitStatus !== '0') err ??= `exit status ${mainExitStatus}`;
    console.error(`[driver] main() du worker sort a +${mainExitedT|0} ms (status ${mainExitStatus}) — qemu_cleanup in-thread, pas d appel loader`);
    finish('main-exited');
    return;
  }
  if (cleanupT === null && shutdownReqT !== null &&
      (performance.now() - t0 - shutdownReqT) > QUIESCE_MS) {
    loopQuiesceT = performance.now() - t0;
    try {
      cleanupT = performance.now() - t0;
      Module._qemu_cleanup();
      cleanupDoneT = performance.now() - t0;
      console.error(`[driver] qemu_cleanup ok (+${cleanupDoneT|0} ms, duree ${(cleanupDoneT-cleanupT)|0} ms)`);
    } catch (e) { err ??= `qemu_cleanup: ${e}`; }
    finish('stop');
    return;
  }
  if (elapsed >= RUN_SECONDS + 9) {
    err ??= 'pas de quiescence apres shutdown_request (teardown v1: hang)';
    finish('timeout');
  }
}, 200);

function cadenceStats() {
  // par secondes depuis boot
  const perSecond = new Map();
  let bytes = 0;
  for (const ev of events) {
    if (ev.kind !== 'uart') continue;
    bytes += ev.len;
    const s = Math.floor((ev.t - bootedAt) / 1000);
    if (s >= 0) perSecond.set(s, (perSecond.get(s) ?? 0) + ev.len);
  }
  return { totalEvents: rawLines, totalBytes: bytes, perSecond: [...perSecond.entries()].slice(0, 20) };
}

function finish(kind) {
  clearInterval(wd);
  const report = {
    critere: { boot: '<5 s', cadence: '<4x native (115200 ~ lines/s)', teardown: 'sans crash (quiescence+qemu_cleanup)' },
    boot: {
      moduleInit_ms: qemuInitT,
      firstUart_ms: bootedAt,
      bootDelta_initTOuart_ms: bootedAt == null || qemuInitT == null ? null : bootedAt - qemuInitT,
    },
    cadence: cadenceStats(),
    teardown: {
      shutdownRequest_ms: shutdownReqT,
      quiesce_ms: loopQuiesceT,
      shutdownToQuiesce_ms: shutdownReqT != null && loopQuiesceT != null ? loopQuiesceT - shutdownReqT : null,
      cleanupStart_ms: cleanupT,
      cleanupDone_ms: cleanupDoneT,
      cleanupDuration_ms: cleanupT != null && cleanupDoneT != null ? cleanupDoneT - cleanupT : null,
    },
    heap_bytes: (Module.HEAPU32?.length ?? 0) * 4,
    totalRuntime_ms: performance.now() - t0,
    err,
    kind,
    events: events.slice(0, 40), // amortissement
  };
  // dump du texte UART decode du ring (preuve de boot + contenu)
  try {
    fs.writeFileSync(`${DRIVERDIR}/uart-text407.txt`, ringText);
    console.error(`[driver] uart-text407.txt ecrit (${ringText.length} car)`);
  } catch {}
  // dump de la trace in_asm (ecrite par qemu dans MEMFS, FS partage)
  try {
    const tr = Module.FS.readFile('/trace.log');
    fs.writeFileSync(`${DRIVERDIR}/trace-dump.txt`, tr);
    console.error(`[driver] trace.log dump : ${tr.length} o -> trace-dump.txt`);
  } catch (e) {
    console.error(`[driver] trace.log indisponible : ${e.message ?? e}`);
  }
  fs.writeFileSync(`${DRIVERDIR}/metrics407.json`, JSON.stringify(report, null, 2));
  console.error('[driver] metrics407.json ecrit ; exit');
  process.exit(0);
}

process.on('exit', (code) => {
  console.error(`[driver] exit code=${code}`);
});
process.on('uncaughtException', (e) => {
  err ??= `uncaught: ${e}`;
  finish('uncaught');
});
