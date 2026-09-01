// Where "Support Vison" sends people, and the only URLs the app will open.
//
// One list, two consumers: the renderer renders it, and main.ts derives the
// openExternal allowlist from it. That is deliberate - a link added in one
// place and not the other would render as a button that silently does
// nothing, which is the kind of bug nobody reports.
//
// An entry with an empty `url` is "not set up yet" and is not rendered at
// all. Switch a platform on by filling in the URL once the account actually
// exists and you have loaded it in a signed-out browser: shipping a link to
// an unapproved handle is worse than shipping no link, because it sends
// people to a 404 with your name on it.

export interface SupportLink {
  id: string;
  label: string;
  detail: string;
  url: string;
}

// Ways to give money. Three rails rather than one because no single platform
// reaches everyone: Polar takes any card worldwide with no account, Razorpay
// is the only one of the three that takes UPI, and GitHub Sponsors takes no
// fee at all but asks the donor for a GitHub account.
//
// Keep .github/FUNDING.yml in step with this by hand - it is the same
// decision made again for the repository page, and GitHub does not read this
// file.
export const FUNDING_LINKS: SupportLink[] = [
  {
    id: 'polar',
    label: 'Polar',
    detail: 'Any country. Card or wallet, no account needed.',
    // Polar storefront or checkout URL, e.g. https://polar.sh/<handle>.
    url: '',
  },
  {
    id: 'razorpay',
    label: 'Razorpay',
    detail: 'India. UPI, card or netbanking.',
    // Razorpay Payment Page URL, e.g. https://pages.razorpay.com/<slug>.
    url: '',
  },
  {
    id: 'github-sponsors',
    label: 'GitHub Sponsors',
    detail: 'One-off or monthly. Needs a GitHub account, but takes no fee.',
    // Only live once the Sponsors application has been approved - the URL
    // 404s while it is pending, so this stays blank until checked signed out.
    url: '',
  },
];

// Ways to help that cost nothing, and in this project genuinely are worth as
// much: Vison has been developed against exactly one 6 GB laptop GPU, and
// three registered models have never been run because they do not fit on it.
export const CONTRIBUTION_LINKS: SupportLink[] = [
  {
    id: 'report-hardware',
    label: 'Report how it ran on your hardware',
    detail: 'Especially Qwen-Image, Wan 2.2 A14B or HunyuanVideo - none have ever been run.',
    url: 'https://github.com/JayRGadekar/Vison/issues/new',
  },
  {
    id: 'source',
    label: 'Star or fork the repository',
    detail: 'MIT licensed. Bug reports and patches equally welcome.',
    url: 'https://github.com/JayRGadekar/Vison',
  },
];

// Every URL the app is willing to hand to the OS. Exact strings rather than
// hostnames: the set is fixed and known at build time, so there is no reason
// to accept anything looser.
export const ALLOWED_EXTERNAL_URLS: string[] =
  [...FUNDING_LINKS, ...CONTRIBUTION_LINKS].map(l => l.url).filter(url => url !== '');
