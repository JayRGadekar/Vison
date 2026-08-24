// Run with:  npm run test:titles
//
// Node 24 imports the TypeScript directly, so there is no build step and no
// test framework. A conversation whose label comes out empty, mangled, or
// identical to its neighbours is effectively lost in the sidebar, which is the
// only way back into it.

import assert from 'assert';
import { conversationTitle, TITLE_MAX, DEFAULT_TITLE } from './conversation-title.ts';

let pass = 0, fail = 0;
const t = (name, fn) => {
  try { fn(); console.log(`  PASS  ${name}`); pass++; }
  catch (e) { console.log(`  FAIL  ${name}\n        ${e.message}`); fail++; }
};

const user = (content) => [{ role: 'user', content }];

t('uses the first user message', () => {
  assert.strictEqual(
    conversationTitle([
      { role: 'assistant', content: 'ignored' },
      { role: 'user', content: 'a red cube' },
      { role: 'user', content: 'later prompt' },
    ]),
    'a red cube');
});

t('a short prompt is used as-is', () => {
  assert.strictEqual(conversationTitle(user('a red cube')), 'a red cube');
});

t('newlines and runs of spaces collapse', () => {
  assert.strictEqual(
    conversationTitle(user('  a red\n\ncube   on   a  table  ')),
    'a red cube on a table');
});

t('no user message falls back', () => {
  assert.strictEqual(conversationTitle([{ role: 'assistant', content: 'hi' }]), DEFAULT_TITLE);
  assert.strictEqual(conversationTitle([]), DEFAULT_TITLE);
});

t('a whitespace-only prompt falls back', () => {
  assert.strictEqual(conversationTitle(user('   \n  ')), DEFAULT_TITLE);
});

t('exactly at the limit is not truncated', () => {
  const exact = 'x'.repeat(TITLE_MAX);
  assert.strictEqual(conversationTitle(user(exact)), exact);
  assert.ok(!conversationTitle(user(exact)).endsWith('…'));
});

t('one over the limit is truncated with an ellipsis', () => {
  const over = 'x'.repeat(TITLE_MAX + 1);
  const got = conversationTitle(user(over));
  assert.ok(got.endsWith('…'), got);
  assert.ok(got.length <= TITLE_MAX + 1, `too long: ${got.length}`);
});

t('a long prompt is cut on a word boundary', () => {
  const got = conversationTitle(user(
    'a cinematic portrait of a woman standing in the rain at night'));
  assert.ok(got.endsWith('…'), got);
  assert.ok(!got.includes(' …'), `trailing space before ellipsis: ${JSON.stringify(got)}`);
  // The boundary cut must not slice a word in half.
  const words = got.slice(0, -1).split(' ');
  assert.ok('a cinematic portrait of a woman standing in the rain at night'.split(' ')
    .includes(words[words.length - 1]), `cut mid-word: ${got}`);
});

t('one very long token is not trimmed away to nothing', () => {
  // No space in the last third, so the word-boundary rule must not apply.
  const got = conversationTitle(user('https://example.com/' + 'a'.repeat(80)));
  assert.ok(got.length > TITLE_MAX * 0.9, `trimmed too hard: ${got}`);
});

t('a message with no content does not throw', () => {
  assert.strictEqual(conversationTitle([{ role: 'user' }]), DEFAULT_TITLE);
});

t('titles stay distinct for prompts that differ late', () => {
  const a = conversationTitle(user('a portrait of a woman in a red dress'));
  const b = conversationTitle(user('a portrait of a woman in a blue dress'));
  assert.notStrictEqual(a, b);
});

console.log(`\n${pass} passed, ${fail} failed`);
process.exit(fail ? 1 : 0);
