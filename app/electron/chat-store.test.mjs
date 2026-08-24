import fs from 'fs';
import path from 'path';
import os from 'os';
import assert from 'assert';

// Behaviour the chat store has to preserve.
//
// Run with:  npm run test:chats
//
// There is no test framework in this project and this does not add one. Node 24
// imports TypeScript directly, so the module under test is loaded as-is and
// asserted against with node:assert - no build step, no dependency.
//
// The point is that a future change here, including swapping node:sqlite for
// something else, has something to fail against. What is being risked is the
// user's conversation history.

import * as store from './chat-store.ts';

const root = fs.mkdtempSync(path.join(os.tmpdir(), 'vison-store-'));
let pass = 0, fail = 0;
const t = (name, fn) => {
  try { fn(); console.log(`  PASS  ${name}`); pass++; }
  catch (e) { console.log(`  FAIL  ${name}\n        ${e.message}`); fail++; }
};

// ---- round trip, including a field the store does not model ----------------
const ud1 = path.join(root, 'ud1');
fs.mkdirSync(ud1, { recursive: true });
store.open(ud1);

const chat = {
  id: 'chat_1699999999',
  title: 'A red cube',
  timestamp: 1700000000000,
  messages: [
    { role: 'user', content: 'a red cube' },
    { role: 'assistant', content: 'done', url: 'http://x/1.png', baseImage: 'b64...' },
    { role: 'assistant', content: 'v2', futureField: { nested: [1, 2, 3] } },
  ],
};
store.save(chat);

t('load returns the chat unchanged', () => {
  const got = store.load('chat_1699999999');
  assert.strictEqual(got.title, 'A red cube');
  assert.strictEqual(got.timestamp, 1700000000000);
  assert.strictEqual(got.messages.length, 3);
  assert.deepStrictEqual(got.messages, chat.messages);
});

t('a field the schema does not model survives', () => {
  const got = store.load('chat_1699999999');
  assert.deepStrictEqual(got.messages[2].futureField, { nested: [1, 2, 3] });
});

t('message order is preserved', () => {
  const got = store.load('chat_1699999999');
  assert.deepStrictEqual(got.messages.map(m => m.content), ['a red cube', 'done', 'v2']);
});

// ---- re-save replaces rather than appends ----------------------------------
t('re-saving shorter replaces, does not append', () => {
  store.save({ ...chat, messages: [{ role: 'user', content: 'only one' }] });
  const got = store.load('chat_1699999999');
  assert.strictEqual(got.messages.length, 1, `expected 1, got ${got.messages.length}`);
});

// ---- listing ---------------------------------------------------------------
store.save({ id: 'chat_b', title: 'Newer', timestamp: 3000, messages: [] });
store.save({ id: 'chat_a', title: 'Older', timestamp: 2000, messages: [] });

t('list is newest first and carries no message bodies', () => {
  const l = store.list();
  const ids = l.map(c => c.id);
  assert.strictEqual(ids[ids.length - 2], 'chat_b');
  assert.strictEqual(ids[ids.length - 1], 'chat_a');
  assert.ok(!('messages' in l[0]), 'list rows should not include messages');
});

// ---- delete cascades -------------------------------------------------------
t('delete removes the chat and its messages', () => {
  store.remove('chat_1699999999');
  assert.strictEqual(store.load('chat_1699999999'), null);
  assert.ok(!store.list().some(c => c.id === 'chat_1699999999'));
});

t('load of an unknown id returns null, not a throw', () => {
  assert.strictEqual(store.load('chat_nonexistent'), null);
});

t('an invalid id is rejected', () => {
  assert.throws(() => store.save({ id: '../../escape', messages: [] }), /Invalid chat id/);
  assert.throws(() => store.load('../../escape'), /Invalid chat id/);
});

store.close();

// ---- legacy JSON import ----------------------------------------------------
const ud2 = path.join(root, 'ud2');
const legacy = path.join(ud2, 'chats');
fs.mkdirSync(legacy, { recursive: true });
fs.writeFileSync(path.join(legacy, 'chat_111.json'),
  JSON.stringify({ id: 'chat_111', title: 'One', timestamp: 111, messages: [{ role: 'user', content: 'hi' }] }));
fs.writeFileSync(path.join(legacy, 'chat_222.json'),
  JSON.stringify({ id: 'chat_222', title: 'Two', timestamp: 222, messages: [] }));
// the file an early build wrote with a missing id in the template string
fs.writeFileSync(path.join(legacy, '.json'),
  JSON.stringify({ id: 'chat_333', title: 'Legacy', timestamp: 333, messages: [{ role: 'user', content: 'old' }] }));
fs.writeFileSync(path.join(legacy, 'broken.json'), '{ this is not json');

store.open(ud2);
const r1 = await store.importLegacyJson(ud2);

t('import brings across the readable conversations', () => {
  assert.strictEqual(r1.imported, 3, `imported ${r1.imported}`);
  assert.strictEqual(r1.skipped, 1, `skipped ${r1.skipped}`);
});

t('the misnamed .json file is recovered by its inner id', () => {
  const got = store.load('chat_333');
  assert.ok(got, 'chat_333 missing');
  assert.strictEqual(got.messages[0].content, 'old');
});

t('one corrupt file does not stop the others', () => {
  assert.ok(store.load('chat_111'));
  assert.ok(store.load('chat_222'));
});

t('the original JSON directory is kept, not deleted', () => {
  const parked = fs.readdirSync(ud2).filter(n => n.startsWith('chats.imported-'));
  assert.strictEqual(parked.length, 1, 'expected the old dir parked aside');
  assert.ok(fs.existsSync(path.join(ud2, parked[0], 'chat_111.json')), 'original file gone');
});

const r2 = await store.importLegacyJson(ud2);
t('import does not run twice', () => {
  assert.strictEqual(r2.alreadyDone, true);
  assert.strictEqual(store.list().length, 3);
});

store.close();

// ---- fresh install ---------------------------------------------------------
const ud3 = path.join(root, 'ud3');
fs.mkdirSync(ud3, { recursive: true });
store.open(ud3);
const r3 = await store.importLegacyJson(ud3);
t('a fresh install with no legacy dir is fine', () => {
  assert.strictEqual(r3.imported, 0);
  assert.strictEqual(store.list().length, 0);
});

// ---- reopen: data survives a restart ---------------------------------------
store.save({ id: 'chat_persist', title: 'Persist', timestamp: 9, messages: [{ role: 'user', content: 'x' }] });
store.close();
store.open(ud3);
t('data survives closing and reopening the database', () => {
  assert.strictEqual(store.load('chat_persist').messages[0].content, 'x');
});
store.close();

// ---- search --------------------------------------------------------------
const ud4 = path.join(root, 'ud4');
fs.mkdirSync(ud4, { recursive: true });
store.open(ud4);

store.save({ id: 'chat_cube', title: 'A red cube', timestamp: 10, messages: [
  { role: 'user', content: 'a red cube on a wooden table' },
  { role: 'assistant', content: 'here is your cube' },
]});
store.save({ id: 'chat_cat', title: 'A cat', timestamp: 20, messages: [
  { role: 'user', content: 'a fluffy cat sitting on a windowsill' },
]});
store.save({ id: 'chat_empty', title: 'Nothing', timestamp: 30, messages: [] });

t('search finds a conversation by message text', () => {
  const hits = store.search('wooden');
  assert.strictEqual(hits.length, 1, 'got ' + hits.length);
  assert.strictEqual(hits[0].id, 'chat_cube');
});

t('search matches a prefix, so it works mid-typing', () => {
  assert.strictEqual(store.search('wind')[0]?.id, 'chat_cat');
});

t('a conversation matching many times is still one result', () => {
  const hits = store.search('cube');
  assert.strictEqual(hits.length, 1, 'expected 1 conversation, got ' + hits.length);
});

t('several words must all match', () => {
  assert.strictEqual(store.search('red cube').length, 1);
  assert.strictEqual(store.search('red windowsill').length, 0);
});

t('the snippet marks the match', () => {
  const [hit] = store.search('wooden');
  assert.ok(hit.snippet.includes(store.MATCH_START), 'no start marker');
  assert.ok(hit.snippet.includes(store.MATCH_END), 'no end marker');
  const marked = hit.snippet.slice(
    hit.snippet.indexOf(store.MATCH_START) + 1,
    hit.snippet.indexOf(store.MATCH_END));
  assert.strictEqual(marked.toLowerCase(), 'wooden');
});

t('search is case-insensitive', () => {
  assert.strictEqual(store.search('WOODEN')[0]?.id, 'chat_cube');
});

t('an empty or blank query returns nothing rather than everything', () => {
  assert.deepStrictEqual(store.search(''), []);
  assert.deepStrictEqual(store.search('   '), []);
});

t('FTS operators in the query are treated as text, not syntax', () => {
  // Every one of these is a syntax error if handed to MATCH unescaped.
  const nasty = ['"', 'cube OR cat', 'NOT cube', 'cube:', '*', '(cube', 'a AND', '^x', 'a-b'];
  for (const q of nasty) {
    assert.doesNotThrow(() => store.search(q), 'threw on ' + JSON.stringify(q));
  }
});

t('a quoted phrase in the query still finds its conversation', () => {
  assert.strictEqual(store.search('"wooden"')[0]?.id, 'chat_cube');
});

t('editing a conversation updates what search sees', () => {
  store.save({ id: 'chat_cat', title: 'A cat', timestamp: 21, messages: [
    { role: 'user', content: 'a dog instead' },
  ]});
  assert.strictEqual(store.search('windowsill').length, 0, 'stale text still matches');
  assert.strictEqual(store.search('dog')[0]?.id, 'chat_cat');
});

t('a deleted conversation leaves the index too', () => {
  store.remove('chat_cube');
  assert.strictEqual(store.search('wooden').length, 0, 'deleted chat still in search');
});

t('reopening keeps the index usable', () => {
  store.close();
  store.open(ud4);
  assert.strictEqual(store.search('dog').length, 1);
});

store.close();

// ---- the index is rebuilt when it drifts ----------------------------------
// Stands in for a database written before search existed, where the tables are
// populated and the index is empty.
const ud6 = path.join(root, 'ud6');
fs.mkdirSync(ud6, { recursive: true });
store.open(ud6);
store.save({ id: 'chat_drift', title: 'Drift', timestamp: 5, messages: [
  { role: 'user', content: 'a lighthouse in a storm' },
]});
store.close();

const { DatabaseSync } = await import('node:sqlite');
const raw = new DatabaseSync(path.join(ud6, 'chats.db'));
raw.exec('DELETE FROM chat_search');
raw.close();

store.open(ud6);
t('an empty index is rebuilt on open', () => {
  assert.strictEqual(store.search('lighthouse')[0]?.id, 'chat_drift');
});
store.close();

// ---- search survives the legacy import ------------------------------------
const ud5 = path.join(root, 'ud5');
const legacy5 = path.join(ud5, 'chats');
fs.mkdirSync(legacy5, { recursive: true });
fs.writeFileSync(path.join(legacy5, 'chat_old.json'), JSON.stringify({
  id: 'chat_old', title: 'Imported', timestamp: 1,
  messages: [{ role: 'user', content: 'an imported conversation about mountains' }],
}));
store.open(ud5);
await store.importLegacyJson(ud5);
t('imported conversations are searchable', () => {
  assert.strictEqual(store.search('mountains')[0]?.id, 'chat_old');
});
store.close();

console.log(`\n${pass} passed, ${fail} failed`);
fs.rmSync(root, { recursive: true, force: true });
process.exit(fail ? 1 : 0);
