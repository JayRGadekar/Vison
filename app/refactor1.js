const fs = require('fs');
let code = fs.readFileSync('D:/Project/Vison/app/src/App.tsx', 'utf-8');

if (!code.includes('RectangleHorizontal')) {
  code = code.replace('import { Settings', 'import { Sparkles, Monitor, Maximize2, Columns, Scaling, RectangleHorizontal, RectangleVertical, Frame, Wand2, Settings');
}

code = code.replace(/const \[searchTask, setSearchTask\] = useState\([^)]+\);/, `const [mediaType, setMediaType] = useState<'image' | 'video'>('image');
  const [taskMode, setTaskMode] = useState<'generate' | 'upscale'>('generate');
  const [aspectRatio, setAspectRatio] = useState('16:9');`);

if (!code.includes('const searchTask =')) {
  code = code.replace('const filteredModels = allModels.filter(m =>', 
  `const searchTask = taskMode === 'generate' ? mediaType : mediaType + "_upscale";\n  const filteredModels = allModels.filter(m =>`);
}

fs.writeFileSync('D:/Project/Vison/app/src/App.tsx', code);
console.log("State updated");
