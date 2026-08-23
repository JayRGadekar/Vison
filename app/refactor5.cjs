const fs = require('fs');
let code = fs.readFileSync('D:/Project/Vison/app/src/App.tsx', 'utf-8');

const effectCode = `  useEffect(() => {
    // When searchTask changes (via toggling generation/upscale or image/video),
    // Reset selectedModel to the first available model in the new category
    const available = allModels.filter(m => m.task === searchTask);
    if (available.length > 0 && selectedModel.task !== searchTask) {
        setSelectedModel({ id: available[0].id, name: available[0].name, task: available[0].task });
    }
  }, [searchTask, allModels, selectedModel.task]);

  const filteredModels = allModels.filter(m =>`;

if (!code.includes('// When searchTask changes')) {
  code = code.replace('  const filteredModels = allModels.filter(m =>', effectCode);
  fs.writeFileSync('D:/Project/Vison/app/src/App.tsx', code);
  console.log("Effect inserted!");
} else {
  console.log("Effect already there");
}
