const fs = require('fs');
let code = fs.readFileSync('D:/Project/Vison/app/src/App.tsx', 'utf-8');

code = code.replace(
  '<X className="w-3.5 h-3.5 text-red-500 flex-shrink-0 cursor-pointer opacity-0 group-hover:opacity-100 hover:scale-110 transition-all absolute left-0" onClick={(e) => handleModelCancel(e, model.id)} />',
  `<div 
      className="absolute left-[-20px] p-2 -my-2 cursor-pointer opacity-0 group-hover:opacity-100 transition-all z-10 flex items-center justify-center"
      onClick={(e) => handleModelCancel(e, model.id)}
   >
      <X className="w-4 h-4 text-red-500 hover:scale-125 transition-transform" />
   </div>`
);

fs.writeFileSync('D:/Project/Vison/app/src/App.tsx', code);
console.log('UI Replaced!');
