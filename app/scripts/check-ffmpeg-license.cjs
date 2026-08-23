// Refuses to package a bundled ffmpeg whose licence we cannot ship under.
//
// Vison is distributed as a paid, closed-source product. It shells out to
// ffmpeg as a separate process, which keeps Vison's own source out of scope -
// but the ffmpeg binary we bundle carries its own licence, and that licence is
// decided at ITS build time, not by which encoders we happen to call.
//
// This is the trap worth automating away: a typical Windows ffmpeg build ships
// with --enable-gpl --enable-version3 --enable-libx264, which makes the binary
// GPLv3. Only ever invoking the VP9 encoder inside it does not change that.
// Shipping one by accident is a licensing problem discovered by a third party,
// not by us, so the build fails here instead.
//
// Not bundling ffmpeg at all is fine and is the default - the app finds a
// system copy and warns when there is none.

const { execFileSync } = require('child_process');
const fs = require('fs');
const path = require('path');

// Where the packaging config picks the backend up from.
const BACKEND_DIR = path.resolve(__dirname, '..', '..', 'build', 'bin');

const FORBIDDEN = [
  {
    flag: '--enable-nonfree',
    why: 'a non-free build cannot be redistributed at all, under any terms',
  },
  {
    flag: '--enable-gpl',
    why: 'this makes the whole binary GPL, regardless of which encoders we call',
  },
];

function findBundledFfmpeg() {
  if (!fs.existsSync(BACKEND_DIR)) return null;
  for (const name of ['ffmpeg.exe', 'ffmpeg']) {
    const p = path.join(BACKEND_DIR, name);
    if (fs.existsSync(p) && fs.statSync(p).isFile()) return p;
  }
  return null;
}

function configurationLine(ffmpegPath) {
  // ffmpeg prints its banner on stderr and exits 0.
  const out = execFileSync(ffmpegPath, ['-version'], {
    encoding: 'utf8',
    stdio: ['ignore', 'pipe', 'pipe'],
    windowsHide: true,
  });
  const line = out.split(/\r?\n/).find(l => l.startsWith('configuration:'));
  if (!line) {
    throw new Error(
      `${ffmpegPath} did not report a configuration line, so its licence cannot be ` +
      `determined. Refusing to bundle a binary we cannot vouch for.`);
  }
  return line;
}

function checkFfmpegLicense({ quiet = false } = {}) {
  const ffmpegPath = findBundledFfmpeg();
  if (!ffmpegPath) {
    if (!quiet) {
      console.log('[ffmpeg-licence] none bundled — the app will use a system copy. OK.');
    }
    return;
  }

  const config = configurationLine(ffmpegPath);
  const problems = FORBIDDEN.filter(f => config.includes(f.flag));

  if (problems.length > 0) {
    const detail = problems.map(p => `  ${p.flag}  — ${p.why}`).join('\n');
    throw new Error(
      `Refusing to package ${ffmpegPath}.\n\n` +
      `It was built with:\n${detail}\n\n` +
      `Vison needs an LGPL build with libvpx and no GPL components. BtbN's\n` +
      `FFmpeg-Builds publishes ffmpeg-master-latest-win64-lgpl.zip alongside the\n` +
      `GPL ones. Verify a candidate with:\n\n` +
      `  ffmpeg -version | grep -o -- "--enable-gpl\\|--enable-nonfree\\|--enable-libvpx"\n\n` +
      `You want --enable-libvpx and nothing else from that list.\n` +
      `See docs/BUILD.md.`);
  }

  // Not fatal: without libvpx the muxer falls back to MPEG-4 Part 2, which is
  // still licence-clean, just worse. Worth saying out loud though, because
  // silently shipping the fallback is not what anyone intended.
  if (!config.includes('--enable-libvpx')) {
    console.warn(
      `[ffmpeg-licence] WARNING: ${ffmpegPath} has no --enable-libvpx.\n` +
      `                 Video will fall back to MPEG-4 Part 2 instead of VP9/WebM.`);
  } else if (!quiet) {
    console.log(`[ffmpeg-licence] ${path.basename(ffmpegPath)}: LGPL with libvpx. OK.`);
  }
}

// electron-builder beforePack hook.
//
// Order matters: verify the licence before generating notices, so a rejected
// binary never gets a notice written for it.
module.exports = async function beforePack() {
  require('./check-auth-configured.cjs')();
  checkFfmpegLicense();
  require('./collect-licenses.cjs')();
};

module.exports.checkFfmpegLicense = checkFfmpegLicense;

// Also runnable directly: `node scripts/check-ffmpeg-license.js`
if (require.main === module) {
  try {
    checkFfmpegLicense();
  } catch (err) {
    console.error(`\n${err.message}\n`);
    process.exit(1);
  }
}
