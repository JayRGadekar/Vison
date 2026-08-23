// Assembles the third-party notices that ship with Vison.
//
// Every dependency here is permissive (MIT / BSD / Apache / public domain), and
// all of those carry the same core obligation: reproduce the copyright notice
// and licence text in the distribution. That is cheap to satisfy and easy to
// forget, so it is generated from the files actually vendored in the tree
// rather than maintained by hand - a hand-written list goes stale the first
// time someone adds a dependency.
//
// ffmpeg is deliberately handled differently. It is not vendored: it is an
// optional external binary the user (or a future bundling decision) supplies.
// Its notice is emitted only when a copy is actually present in build/bin, so
// we never claim to ship something we do not.

const fs = require('fs');
const path = require('path');
const { execFileSync } = require('child_process');

const REPO = path.resolve(__dirname, '..', '..');
const OUT_DIR = path.join(REPO, 'app', 'build-resources', 'licenses');

// Vendored dependencies whose licence text lives in the tree. `file` is
// relative to the repo root; a missing file is a hard error, because silently
// dropping a notice is the failure this script exists to prevent.
const VENDORED = [
  {
    name: 'stable-diffusion.cpp',
    url: 'https://github.com/leejet/stable-diffusion.cpp',
    file: 'third_party/stable-diffusion.cpp/LICENSE',
    note: 'Diffusion inference. Vison carries local patches; see PATCHES.md.',
  },
  {
    name: 'ggml',
    url: 'https://github.com/ggml-org/ggml',
    file: 'third_party/stable-diffusion.cpp/ggml/LICENSE',
    note: 'Tensor library and Vulkan backend.',
  },
  {
    name: 'vision.cpp',
    url: 'https://github.com/Acly/vision.cpp',
    file: 'third_party/vision.cpp/LICENSE',
    note: 'Image loading, resizing, and ESRGAN upscaling.',
  },
  {
    name: 'stb',
    url: 'https://github.com/nothings/stb',
    file: 'third_party/stable-diffusion.cpp/thirdparty/stb_image.h',
    extract: 'stb',
    note: 'PNG/JPEG encoding and decoding. Dual-licensed MIT / public domain.',
  },
  {
    name: 'darts-clone',
    url: 'https://github.com/s-yata/darts-clone',
    file: 'third_party/stable-diffusion.cpp/thirdparty/LICENSE.darts_clone.txt',
    note: 'Double-array trie, used by the tokenizer.',
  },
];

// Dependencies resolved by npm, whose licence text lives in node_modules.
const NPM = ['react', 'react-dom', 'lucide-react', 'electron'];

function readVendored(entry) {
  const abs = path.join(REPO, entry.file);
  if (!fs.existsSync(abs)) {
    throw new Error(
      `Licence source missing for ${entry.name}: ${entry.file}\n` +
      `Either the dependency moved or it was removed. Fix the path in ` +
      `scripts/collect-licenses.cjs rather than dropping the notice.`);
  }
  const raw = fs.readFileSync(abs, 'utf8');

  // stb states its licence in a trailing comment block rather than a file.
  if (entry.extract === 'stb') {
    const start = raw.indexOf('ALTERNATIVE A - MIT License');
    if (start === -1) throw new Error('stb licence block not found in stb_image.h');
    return raw.slice(start).replace(/\*\/\s*$/, '').trim();
  }
  return raw.trim();
}

function readNpm(name) {
  const dir = path.join(REPO, 'app', 'node_modules', name);
  if (!fs.existsSync(dir)) return null;   // not installed; skipped, not fatal

  for (const f of ['LICENSE', 'LICENSE.md', 'LICENSE.txt', 'license', 'LICENCE']) {
    const p = path.join(dir, f);
    if (fs.existsSync(p)) {
      let url = '';
      try {
        const pkg = JSON.parse(fs.readFileSync(path.join(dir, 'package.json'), 'utf8'));
        url = typeof pkg.homepage === 'string' ? pkg.homepage : '';
      } catch { /* homepage is a nicety */ }
      return { name, url, text: fs.readFileSync(p, 'utf8').trim() };
    }
  }
  return null;
}

// ffmpeg, only if one is actually staged for bundling.
function readFfmpeg() {
  for (const exe of ['ffmpeg.exe', 'ffmpeg']) {
    const p = path.join(REPO, 'build', 'bin', exe);
    if (!fs.existsSync(p)) continue;

    let config = '';
    let version = '';
    try {
      const out = execFileSync(p, ['-version'], { encoding: 'utf8', windowsHide: true });
      const lines = out.split(/\r?\n/);
      config = lines.find(l => l.startsWith('configuration:')) || '';
      version = lines.find(l => l.startsWith('ffmpeg version')) || '';
    } catch { /* the licence gate reports this properly */ }

    // ffmpeg embeds the git commit it was built from in its version banner:
    //   ffmpeg version N-126229-gf101fce22d-20260820
    //                            ^^^^^^^^^^
    // Derived rather than hardcoded so that swapping the binary updates the
    // notice by itself - a stale source link is a compliance failure that
    // nothing else in the build would catch.
    const commitMatch = version.match(/-g([0-9a-f]{7,40})\b/);
    const commit = commitMatch ? commitMatch[1] : '';
    const build = version.replace(/^ffmpeg version\s*/, '').split(' ')[0] || '(unavailable)';

    // --enable-version3 upgrades the LGPL to v3. Naming the wrong version in
    // the notice is its own compliance failure, so derive it.
    const v3 = config.includes('--enable-version3');
    return {
      version3: v3,
      config,
      text: [
        'FFmpeg is licensed under the GNU Lesser General Public License',
        `version ${v3 ? '3' : '2.1'} or later (LGPL v${v3 ? '3' : '2.1'}+).`,
        '',
        'The full licence text ships alongside this file as FFMPEG-LICENSE.txt.',
        v3
          // LGPLv3 is written as a set of additional permissions on top of
          // GPLv3, so the GPLv3 text is part of the licence and a reader has to
          // be able to reach it.
          ? 'LGPL v3 incorporates the terms of GPL v3, published at\n' +
            '  https://www.gnu.org/licenses/gpl-3.0.html'
          : 'Published at https://www.gnu.org/licenses/old-licenses/lgpl-2.1.html',
        '',
        'Vison invokes ffmpeg as a separate process; it does not link against',
        'the FFmpeg libraries. The bundled binary is unmodified.',
        '',
        // LGPL section 4 wants the corresponding source. A rolling "/latest"
        // release link stops pointing at this build the moment upstream
        // publishes again, so pin to the commit instead: it identifies exactly
        // the source that was compiled, permanently.
        'Corresponding source for this exact binary:',
        `  https://github.com/FFmpeg/FFmpeg/commit/${commit || '(unavailable)'}`,
        '',
        'Distributed build and the scripts that produced it:',
        '  https://github.com/BtbN/FFmpeg-Builds',
        `  build ${build}`,
        '',
        'The binary may be replaced with any compatible build: it is a',
        'standalone executable in the application directory.',
        '',
        'Build configuration:',
        config || '  (unavailable)',
      ].join('\n'),
    };
  }
  return null;
}

function collectLicenses({ quiet = false } = {}) {
  const sections = [];

  for (const entry of VENDORED) {
    sections.push({
      name: entry.name,
      url: entry.url,
      note: entry.note,
      text: readVendored(entry),
    });
  }

  for (const name of NPM) {
    const found = readNpm(name);
    if (found) sections.push(found);
    else if (!quiet) console.warn(`[licenses] ${name}: no licence file found, skipped`);
  }

  const ffmpeg = readFfmpeg();
  if (ffmpeg) {
    sections.push({
      name: 'FFmpeg',
      url: 'https://ffmpeg.org',
      note: `Video muxing. LGPL v${ffmpeg.version3 ? '3' : '2.1'}+, unmodified, invoked as a separate process.`,
      text: ffmpeg.text,
    });
  }

  const header = [
    'THIRD-PARTY SOFTWARE NOTICES',
    '',
    'Vison includes or uses the software listed below. Each component remains',
    'under its own licence, reproduced in full.',
    '',
    ffmpeg
      ? null
      : 'FFmpeg is NOT bundled with this build. Vison invokes a copy already\n' +
        'installed on the system; if none is found, video is written as numbered\n' +
        'PNG frames instead.',
    '',
    'Contents:',
    ...sections.map((s, i) => `  ${i + 1}. ${s.name}`),
    '',
    '='.repeat(78),
  ].filter(l => l !== null).join('\n');

  const body = sections.map((s, i) => [
    '',
    `${i + 1}. ${s.name}`,
    s.url ? `   ${s.url}` : null,
    s.note ? `   ${s.note}` : null,
    '',
    '-'.repeat(78),
    '',
    s.text,
    '',
    '='.repeat(78),
  ].filter(l => l !== null).join('\n')).join('\n');

  fs.mkdirSync(OUT_DIR, { recursive: true });
  const outPath = path.join(OUT_DIR, 'THIRD-PARTY-NOTICES.txt');
  fs.writeFileSync(outPath, header + '\n' + body + '\n', 'utf8');

  if (!quiet) {
    console.log(`[licenses] ${sections.length} components -> ${path.relative(REPO, outPath)}`);
    if (!ffmpeg) console.log('[licenses] ffmpeg not staged in build/bin; notice omitted.');
  }
  return outPath;
}

module.exports = collectLicenses;
module.exports.collectLicenses = collectLicenses;

if (require.main === module) {
  try {
    collectLicenses();
  } catch (err) {
    console.error(`\n[licenses] ${err.message}\n`);
    process.exit(1);
  }
}
