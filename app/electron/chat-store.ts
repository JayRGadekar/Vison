// Chat persistence, backed by SQLite.
//
// This replaces one JSON file per conversation under userData/chats/. That
// scheme had two problems that get worse with use:
//
//   - A save was a whole-file rewrite with no atomicity. A crash or a power cut
//     partway through left a truncated file, and the conversation was gone -
//     not corrupted in a recoverable way, just unparseable.
//   - Listing meant reading and JSON.parse-ing every conversation on disk, only
//     to keep three fields from each. That is linear in total history size on
//     every open of the sidebar.
//
// SQLite fixes both: a save is one transaction, and a listing touches an index
// rather than the message bodies.
//
// node:sqlite rather than better-sqlite3 deliberately. Electron 41 ships Node
// 24, where node:sqlite is available unflagged, so this adds no dependency, no
// native module to rebuild against Electron's ABI, and nothing to the
// installer. The API is synchronous, which is fine here: these are small local
// reads and writes, and the IPC handlers are already async from the renderer's
// point of view.
//
// The trade: node:sqlite is still flagged experimental in Node 24, so it prints
// a warning on first use and its API may change. The surface used here is
// deliberately tiny - DatabaseSync, exec, prepare/run/get/all, close - so if it
// does change, or if it is ever unavailable, swapping in better-sqlite3 is a
// change to this file alone. chat-store.test.mjs covers the behaviour that
// swap would have to preserve.

import { DatabaseSync } from 'node:sqlite';
import fsSync from 'fs';
import fs from 'fs/promises';
import path from 'path';

export interface ChatMessage {
  role?: string;
  content?: string;
  [key: string]: unknown;
}

export interface Chat {
  id: string;
  title?: string;
  timestamp?: number;
  messages?: ChatMessage[];
}

export interface ChatSummary {
  id: string;
  title: string;
  timestamp: number;
}

// Chat ids arrive over the IPC bridge from the renderer. They are no longer
// pasted into a filesystem path, so a traversal attempt cannot escape anywhere
// - but they are still validated, because an id outside this alphabet is a bug
// somewhere upstream and silently storing it would hide that.
const VALID_ID = /^[A-Za-z0-9_-]{1,128}$/;

export function isValidChatId(id: unknown): id is string {
  return typeof id === 'string' && VALID_ID.test(id);
}

let db: DatabaseSync | null = null;

function schema(handle: DatabaseSync) {
  // WAL keeps a reader (the sidebar listing) from blocking a writer (an
  // in-progress conversation being saved), which is exactly the access pattern
  // here. It also survives a crash without losing committed transactions.
  handle.exec('PRAGMA journal_mode = WAL');
  handle.exec('PRAGMA foreign_keys = ON');

  handle.exec(`
    CREATE TABLE IF NOT EXISTS chats (
      id        TEXT PRIMARY KEY,
      title     TEXT    NOT NULL DEFAULT '',
      timestamp INTEGER NOT NULL DEFAULT 0
    )
  `);

  // One row per message, rather than a JSON blob on the chat, so that a future
  // search can hit an index instead of parsing every conversation.
  //
  // `data` holds the complete message object and is what load() returns. role
  // and content are extracted copies for querying. Keeping the original means a
  // message field this version does not know about - a new attachment type, say
  // - survives a save/load round trip through an older build instead of being
  // silently dropped.
  handle.exec(`
    CREATE TABLE IF NOT EXISTS messages (
      chat_id TEXT    NOT NULL REFERENCES chats(id) ON DELETE CASCADE,
      seq     INTEGER NOT NULL,
      role    TEXT,
      content TEXT,
      data    TEXT    NOT NULL,
      PRIMARY KEY (chat_id, seq)
    )
  `);

  handle.exec('CREATE INDEX IF NOT EXISTS idx_chats_timestamp ON chats(timestamp DESC)');

  handle.exec(`
    CREATE TABLE IF NOT EXISTS meta (
      key   TEXT PRIMARY KEY,
      value TEXT NOT NULL
    )
  `);

  // One FTS row per conversation, not per message: the sidebar is looking for
  // the conversation to reopen, so ten hits inside one chat should be one
  // result, not ten. body is every message's text concatenated.
  //
  // Kept in step by save() and remove() inside the same transaction, rather
  // than by triggers. Triggers over an external-content table are the usual
  // approach and are more code than this earns at local scale - and a rebuild
  // on open (see ensureSearchIndex) makes a drift bug self-healing instead of
  // permanent.
  handle.exec(`
    CREATE VIRTUAL TABLE IF NOT EXISTS chat_search USING fts5(
      chat_id UNINDEXED,
      title,
      body,
      tokenize = 'unicode61'
    )
  `);
}

// Markers around a match inside a snippet, for the renderer to split on and
// wrap in <mark>. Control characters rather than something like [ ] or <b>:
// a prompt can contain any printable text, so a marker made of ordinary
// characters would sometimes highlight the user's own words - and an HTML
// marker would be a way to get markup out of a prompt and into the sidebar.
export const MATCH_START = '\u0002';
export const MATCH_END = '\u0003';

function searchBody(messages: ChatMessage[]): string {
  return messages
    .map(m => (typeof m?.content === 'string' ? m.content : ''))
    .filter(Boolean)
    .join('\n');
}

function reindex(handle: DatabaseSync, chat: Chat, messages: ChatMessage[]) {
  handle.prepare('DELETE FROM chat_search WHERE chat_id = ?').run(chat.id);
  handle.prepare(
    'INSERT INTO chat_search (chat_id, title, body) VALUES (?, ?, ?)'
  ).run(chat.id, String(chat.title ?? ''), searchBody(messages));
}

// Rebuilds the index when it does not match the conversations.
//
// The index arrived after the tables did, so an existing database has chats
// and an empty index; a crash between the two writes of a save could also
// leave them out of step. Comparing counts on open and rebuilding costs
// nothing at this scale and means neither case turns into search that quietly
// misses conversations.
function ensureSearchIndex(handle: DatabaseSync) {
  const chats = (handle.prepare('SELECT count(*) AS n FROM chats').get() as { n: number }).n;
  const indexed = (handle.prepare('SELECT count(*) AS n FROM chat_search').get() as { n: number }).n;
  if (chats === indexed) return;

  handle.exec('DELETE FROM chat_search');
  const rows = handle.prepare('SELECT id, title FROM chats').all() as
    { id: string; title: string }[];
  const insert = handle.prepare(
    'INSERT INTO chat_search (chat_id, title, body) VALUES (?, ?, ?)');
  const messagesFor = handle.prepare(
    'SELECT content FROM messages WHERE chat_id = ? ORDER BY seq ASC');

  for (const row of rows) {
    const contents = (messagesFor.all(row.id) as { content: string | null }[])
      .map(r => r.content ?? '')
      .filter(Boolean)
      .join('\n');
    insert.run(row.id, row.title ?? '', contents);
  }
}

// Turns what someone typed into an FTS5 query.
//
// User input cannot go to MATCH unescaped: a stray quote, a colon, or a bare
// "AND" is FTS5 syntax, and the search box would throw instead of finding
// anything. Each word becomes a quoted phrase - which disarms every operator -
// with a trailing * so "cub" finds "cube" while the user is still typing.
// Words are ANDed, which is what people expect from a search box.
export function toMatchQuery(input: string): string | null {
  const words = input.trim().split(/\s+/).filter(Boolean);
  if (!words.length) return null;
  return words.map(w => `"${w.replace(/"/g, '""')}"*`).join(' ');
}

export function open(userDataDir: string): DatabaseSync {
  if (db) return db;
  const handle = new DatabaseSync(path.join(userDataDir, 'chats.db'));
  schema(handle);
  ensureSearchIndex(handle);
  db = handle;
  return db;
}

export function close() {
  try { db?.close(); } catch { /* already closed */ }
  db = null;
}

function requireDb(): DatabaseSync {
  if (!db) throw new Error('Chat store used before open()');
  return db;
}

export function save(chat: Chat): void {
  const handle = requireDb();
  if (!isValidChatId(chat?.id)) throw new Error(`Invalid chat id: ${String(chat?.id)}`);

  const messages = Array.isArray(chat.messages) ? chat.messages : [];

  // The renderer sends the whole conversation on every save, so the messages
  // are replaced wholesale. Inside a transaction that is atomic: a reader
  // either sees the previous conversation or the new one, never a half-written
  // mixture, which is the failure the JSON files could not rule out.
  handle.exec('BEGIN IMMEDIATE');
  try {
    handle.prepare(
      'INSERT INTO chats (id, title, timestamp) VALUES (?, ?, ?) ' +
      'ON CONFLICT(id) DO UPDATE SET title = excluded.title, timestamp = excluded.timestamp'
    ).run(chat.id, String(chat.title ?? ''), Number(chat.timestamp ?? Date.now()));

    handle.prepare('DELETE FROM messages WHERE chat_id = ?').run(chat.id);

    const insert = handle.prepare(
      'INSERT INTO messages (chat_id, seq, role, content, data) VALUES (?, ?, ?, ?, ?)'
    );
    messages.forEach((m, i) => {
      const role = typeof m?.role === 'string' ? m.role : null;
      const content = typeof m?.content === 'string' ? m.content : null;
      insert.run(chat.id, i, role, content, JSON.stringify(m ?? {}));
    });

    reindex(handle, chat, messages);

    handle.exec('COMMIT');
  } catch (err) {
    try { handle.exec('ROLLBACK'); } catch { /* transaction already gone */ }
    throw err;
  }
}

export function load(id: string): Chat | null {
  const handle = requireDb();
  if (!isValidChatId(id)) throw new Error(`Invalid chat id: ${String(id)}`);

  const row = handle.prepare('SELECT id, title, timestamp FROM chats WHERE id = ?').get(id) as
    { id: string; title: string; timestamp: number } | undefined;
  if (!row) return null;

  const rows = handle.prepare(
    'SELECT data FROM messages WHERE chat_id = ? ORDER BY seq ASC'
  ).all(id) as { data: string }[];

  const messages: ChatMessage[] = [];
  for (const r of rows) {
    // One unparseable row must not cost the user the rest of the conversation.
    try { messages.push(JSON.parse(r.data)); } catch { /* skip this message */ }
  }

  return { id: row.id, title: row.title, timestamp: row.timestamp, messages };
}

export function list(): ChatSummary[] {
  const handle = requireDb();
  return handle.prepare(
    'SELECT id, title, timestamp FROM chats ORDER BY timestamp DESC'
  ).all() as unknown as ChatSummary[];
}

export function remove(id: string): void {
  const handle = requireDb();
  if (!isValidChatId(id)) throw new Error(`Invalid chat id: ${String(id)}`);
  // ON DELETE CASCADE clears the messages, which is why foreign_keys is on.
  // The FTS table is virtual, so no foreign key reaches it - a deleted
  // conversation left in the index would keep turning up in search results
  // and fail to open.
  handle.prepare('DELETE FROM chats WHERE id = ?').run(id);
  handle.prepare('DELETE FROM chat_search WHERE chat_id = ?').run(id);
}

export interface ChatSearchHit extends ChatSummary {
  // The matching text with MATCH_START/MATCH_END around the hit.
  snippet: string;
}

// Conversations matching `input`, best first.
//
// Ranked by bm25 rather than by date: someone typing into a search box wants
// the closest match, and the date is on every row already for the times they
// wanted recency instead.
export function search(input: string, limit = 50): ChatSearchHit[] {
  const handle = requireDb();
  const match = toMatchQuery(input);
  if (!match) return [];

  // FTS5 can still raise on input that survives quoting. A search box must
  // never throw at someone mid-keystroke, so an unparseable query is simply no
  // results.
  try {
    return handle.prepare(`
      SELECT c.id, c.title, c.timestamp,
             snippet(chat_search, 2, ?, ?, '...', 12) AS snippet
      FROM chat_search s
      JOIN chats c ON c.id = s.chat_id
      WHERE chat_search MATCH ?
      ORDER BY rank
      LIMIT ?
    `).all(MATCH_START, MATCH_END, match, limit) as unknown as ChatSearchHit[];
  } catch {
    return [];
  }
}

// Splits a snippet into plain and matched runs, so the renderer can highlight
// without ever handling the marker characters - or having to agree with this
// file about what they are.
export function snippetParts(snippet: string): { text: string; match: boolean }[] {
  const parts: { text: string; match: boolean }[] = [];
  let rest = snippet;
  while (rest.length) {
    const start = rest.indexOf(MATCH_START);
    if (start === -1) { parts.push({ text: rest, match: false }); break; }
    if (start > 0) parts.push({ text: rest.slice(0, start), match: false });

    const end = rest.indexOf(MATCH_END, start);
    if (end === -1) {
      // Unbalanced markers should not be possible, but dropping the rest of the
      // line would be a worse answer than showing it unhighlighted.
      parts.push({ text: rest.slice(start + 1), match: false });
      break;
    }
    parts.push({ text: rest.slice(start + 1, end), match: true });
    rest = rest.slice(end + 1);
  }
  return parts.filter(p => p.text.length > 0);
}

function metaGet(key: string): string | null {
  const row = requireDb().prepare('SELECT value FROM meta WHERE key = ?').get(key) as
    { value: string } | undefined;
  return row?.value ?? null;
}

function metaSet(key: string, value: string) {
  requireDb().prepare(
    'INSERT INTO meta (key, value) VALUES (?, ?) ON CONFLICT(key) DO UPDATE SET value = excluded.value'
  ).run(key, value);
}

const MIGRATION_KEY = 'json_import_completed_at';

// Imports the old userData/chats/*.json files, once.
//
// Nothing is deleted. The directory is left exactly where it is and only marked
// as imported, because the cost of keeping a few kilobytes of superseded JSON
// is nothing next to the cost of being wrong about whether the import worked.
// A user who ends up with an empty sidebar can have their history recovered by
// hand; one whose files were deleted cannot.
//
// A file that cannot be read or parsed is skipped and reported, not fatal: one
// bad file must not stop the other conversations from coming across.
export async function importLegacyJson(userDataDir: string): Promise<{
  imported: number;
  skipped: number;
  alreadyDone: boolean;
}> {
  if (metaGet(MIGRATION_KEY)) return { imported: 0, skipped: 0, alreadyDone: true };

  const dir = path.join(userDataDir, 'chats');
  let entries: string[];
  try {
    entries = await fs.readdir(dir);
  } catch {
    // No legacy directory: a fresh install. Record it so we do not look again.
    metaSet(MIGRATION_KEY, new Date().toISOString());
    return { imported: 0, skipped: 0, alreadyDone: false };
  }

  let imported = 0;
  let skipped = 0;

  for (const entry of entries) {
    if (!entry.endsWith('.json')) continue;
    try {
      const raw = await fs.readFile(path.join(dir, entry), 'utf-8');
      const chat = JSON.parse(raw) as Chat;
      // Covers the file literally named ".json" that an early build produced:
      // its name is useless but the id inside it is not.
      if (!isValidChatId(chat?.id)) { skipped++; continue; }
      save(chat);
      imported++;
    } catch {
      skipped++;
    }
  }

  metaSet(MIGRATION_KEY, new Date().toISOString());

  // Move the directory aside so a later build cannot import it twice if the
  // meta row is ever lost, while still leaving the data recoverable by hand.
  if (imported > 0 || skipped > 0) {
    const parked = path.join(userDataDir, `chats.imported-${Date.now()}`);
    try {
      await fs.rename(dir, parked);
    } catch {
      // Renaming is a tidiness measure, not a correctness one - the meta row is
      // what prevents a second import. A locked directory is not worth failing.
    }
  }

  return { imported, skipped, alreadyDone: false };
}

// Only used by the tests, which need a store that does not touch userData.
export function openAt(file: string): DatabaseSync {
  if (db) close();
  fsSync.mkdirSync(path.dirname(file), { recursive: true });
  const handle = new DatabaseSync(file);
  schema(handle);
  ensureSearchIndex(handle);
  db = handle;
  return db;
}
