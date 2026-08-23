const fs = require('fs');
let code = fs.readFileSync('app/src/App.tsx', 'utf-8');

const regex = /\{msg\.url && \(\s*<img src=\{msg\.url\} alt="Generated output"[\s\S]*?object-cover" \/>\s*\)\}/g;

const newCode = `{msg.url && (
                      <div className="relative group mt-4 inline-block">
                        <img src={msg.url} alt="Generated output" className="rounded-xl shadow-md border border-[#303030] max-w-md w-full object-cover" />
                        <button
                          onClick={() => handleDownloadImage(msg.url || "")}
                          className="absolute top-2 right-2 p-2 bg-black/70 rounded-lg text-white opacity-0 group-hover:opacity-100 transition-opacity hover:bg-black/90 flex items-center gap-1 shadow"
                          title="Download Image"
                        >
                          <Download size={16} />
                        </button>
                      </div>
                    )}`;

code = code.replace(regex, newCode);
fs.writeFileSync('app/src/App.tsx', code);

