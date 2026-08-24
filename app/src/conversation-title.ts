// A sidebar label for a conversation, taken from what the user actually asked
// for.
//
// Its own module because it is the one piece of the history UI with real edge
// cases - and because a label that comes out empty or mangled makes a
// conversation effectively unfindable, which is the whole point of the sidebar.
// conversation-title.test.mjs covers them.

export const TITLE_MAX = 40;

export const DEFAULT_TITLE = 'New Conversation';

export function conversationTitle(
  messages: ReadonlyArray<{ role?: string; content?: string }>,
): string {
  const first = messages.find(m => m?.role === 'user')?.content ?? '';

  // Prompts arrive with newlines and runs of spaces from pasting. Collapsing
  // first means the label is the start of the prompt rather than a ragged
  // fragment of its first line.
  const clean = first.replace(/\s+/g, ' ').trim();
  if (!clean) return DEFAULT_TITLE;
  if (clean.length <= TITLE_MAX) return clean;

  const cut = clean.slice(0, TITLE_MAX);
  const lastSpace = cut.lastIndexOf(' ');

  // Cut on a word boundary when there is one reasonably near the limit:
  // "a cinematic portrait of a…" scans better than "a cinematic portrait of a wo".
  // The 0.6 floor stops a prompt that opens with one very long token - a URL,
  // say - from being trimmed back to almost nothing.
  const body = lastSpace > TITLE_MAX * 0.6 ? cut.slice(0, lastSpace) : cut;
  return body.trimEnd() + '…';
}
