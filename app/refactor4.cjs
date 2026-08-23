const fs = require('fs');
let code = fs.readFileSync('D:/Project/Vison/app/src/App.tsx', 'utf-8');

const disabledOld = `disabled={(!prompt.trim() && !attachedImage && taskMode !== 'upscale') || !localModels.includes(selectedModel.id)}`;
const disabledNew = `disabled={(taskMode === 'upscale' && !attachedImage) || (taskMode !== 'upscale' && !prompt.trim() && !attachedImage) || !localModels.includes(selectedModel.id)}`;

const classNameOld = `\${(!prompt.trim() && !attachedImage && taskMode !== 'upscale') || !localModels.includes(selectedModel.id) ? 'bg-[#333333] text-gray-500' : 'bg-gray-200 text-gray-900 hover:bg-white'}`;
const classNameNew = `\${(taskMode === 'upscale' && !attachedImage) || (taskMode !== 'upscale' && !prompt.trim() && !attachedImage) || !localModels.includes(selectedModel.id) ? 'bg-[#333333] text-gray-500' : 'bg-gray-200 text-gray-900 hover:bg-white'}`;

if (!code.includes(disabledOld)) {
    console.log("Could not find disabled string.");
    process.exit(1);
}

code = code.replace(disabledOld, disabledNew);
code = code.replace(classNameOld, classNameNew);

fs.writeFileSync('D:/Project/Vison/app/src/App.tsx', code);
console.log("Disabled logic fixed.");
