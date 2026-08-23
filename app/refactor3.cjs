const fs = require('fs');
let code = fs.readFileSync('D:/Project/Vison/app/src/App.tsx', 'utf-8');

const mappingCode = `
      let finalWidth = settings.width;
      let finalHeight = settings.height;
      if (aspectRatio === '16:9') { finalWidth = 1024; finalHeight = 576; }
      else if (aspectRatio === '4:3') { finalWidth = 1024; finalHeight = 768; }
      else if (aspectRatio === '1:1') { finalWidth = 1024; finalHeight = 1024; }
      else if (aspectRatio === '3:4') { finalWidth = 768; finalHeight = 1024; }
      else if (aspectRatio === '9:16') { finalWidth = 576; finalHeight = 1024; }
`;

code = code.replace(
  "const userMessage = { role: \"user\",",
  mappingCode + "\n      const userMessage = { role: \"user\","
);

code = code.replace(
  "width: settings.width,",
  "width: finalWidth,"
);

code = code.replace(
  "height: settings.height,",
  "height: finalHeight,"
);

fs.writeFileSync('D:/Project/Vison/app/src/App.tsx', code);
console.log("Submit rewritten");