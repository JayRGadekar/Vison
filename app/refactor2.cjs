const fs = require('fs');
let code = fs.readFileSync('D:/Project/Vison/app/src/App.tsx', 'utf-8');

const match = code.match(/\{\/\* Input Area \(Only show in Chat View\) \*\/\}([\s\S]*?)\{\/\* Settings Modal Layer \*\/\}/);

if (!match) {
  console.log("Could not find the Input Area block");
  process.exit(1);
}

const inputAreaReplacement = `{/* Input Area (Only show in Chat View) */}
      {currentView === 'chat' && (
        <div className="w-full absolute bottom-8 left-1/2 -translate-x-1/2 max-w-3xl px-4 flex flex-col gap-2">
          
          {/* Attachment Preview Bubble */}
          {attachedImage && (
             <div className="self-start relative group ml-2 mt-[-40px] z-10 transition-all">
                <img src={attachedImage} className="w-20 h-20 object-cover rounded-xl border-2 border-[#343434] shadow-2xl" />
                <button 
                  onClick={() => setAttachedImage(null)} 
                  className="absolute -top-2 -right-2 bg-gray-800 rounded-full p-1 border border-gray-600 hover:bg-red-500 hover:text-white"
                >
                   <X className="w-3 h-3" />
                </button>
             </div>
          )}

          <form 
            onSubmit={handleSubmit}
            className="relative flex flex-col bg-[#242424] rounded-3xl border border-[#343434] focus-within:border-[#4f4f4f] transition-all pt-2 pb-2 pl-4 pr-2 shadow-2xl"
          >
            <textarea
              ref={textareaRef}
              className="w-full bg-transparent border-none text-white focus:outline-none placeholder-gray-500 resize-none overflow-y-auto mb-2 text-sm leading-relaxed"
              style={{ minHeight: '44px', maxHeight: '160px' }}
              rows={1}
              placeholder="Send a message"
              value={prompt}
              onChange={handleInput}
              onKeyDown={(e) => {
                  if (e.key === 'Enter' && !e.shiftKey) {
                      e.preventDefault();
                      handleSubmit();
                  }
              }}
            />
            
            <div className="flex items-center justify-between mt-1">
              <div className="flex items-center gap-2">
                <input type="file" hidden accept="image/*" ref={fileInputRef} onChange={handleFileChange} />
                
                <button 
                  type="button" 
                  onClick={() => fileInputRef.current?.click()}
                  className="w-8 h-8 flex items-center justify-center text-gray-400 hover:text-white bg-[#303030]/50 hover:bg-[#404040] rounded-full transition-colors"
                  title="Attach Base Image"
                >
                  <Plus className="w-5 h-5" />
                </button>
              </div>

              <div className="flex items-center gap-2 relative">
                {/* Unified Target Configuration Pill */}
                <button 
                  type="button" 
                  onClick={() => setShowModelDropdown(!showModelDropdown)} 
                  className="h-9 px-4 flex items-center gap-2 text-sm text-gray-300 font-medium hover:text-white bg-[#333333] hover:bg-[#404040] rounded-full transition-colors"
                >
                  {mediaType === 'image' ? "Image" : "Video"}
                  <span className="text-gray-500">•</span>
                  {selectedModel.name.length > 20 ? selectedModel.name.substring(0, 20) + '...' : selectedModel.name}
                  <span className="text-gray-500 mx-1">•</span>
                  <span className="flex items-center gap-1.5 opacity-90"><RectangleHorizontal className="w-4 h-4"/> {aspectRatio}</span>
                  {taskMode === 'upscale' && (
                    <>
                       <span className="text-gray-500 mx-1">•</span>
                       <span className="flex items-center gap-1 opacity-90">x{settings.upscaleQuality.replace('x', '')}</span>
                    </>
                  )}
                </button>

                {showModelDropdown && (
                  <div className="absolute bottom-[52px] right-0 w-[420px] bg-[#1c1c1c] border border-[#2c2c2c] rounded-[24px] shadow-2xl p-4 flex flex-col gap-4 z-50">
                    
                    {/* Feature Toggles Rows */}
                    <div className="grid grid-cols-2 gap-2">
                      <div className="flex bg-[#2c2c2c] p-1 rounded-[16px]">
                          <button
                             type="button"
                             onClick={() => setMediaType('image')}
                             className={\`flex-1 flex items-center justify-center gap-2 text-sm py-2 rounded-[12px] font-medium transition-colors \${mediaType === 'image' ? 'bg-[#3c3c3c] text-white shadow-sm' : 'text-gray-400 hover:text-gray-200'}\`}
                          >
                             <ImageIcon className="w-4 h-4" /> Image
                          </button>
                          <button
                             type="button"
                             onClick={() => setMediaType('video')}
                             className={\`flex-1 flex items-center justify-center gap-2 text-sm py-2 rounded-[12px] font-medium transition-colors \${mediaType === 'video' ? 'bg-[#3c3c3c] text-white shadow-sm' : 'text-gray-400 hover:text-gray-200'}\`}
                          >
                             <Video className="w-4 h-4" /> Video
                          </button>
                      </div>

                      <div className="flex bg-[#2c2c2c] p-1 rounded-[16px]">
                          <button
                             type="button"
                             onClick={() => setTaskMode('generate')}
                             className={\`flex-1 flex items-center justify-center gap-2 text-sm py-2 rounded-[12px] font-medium transition-colors \${taskMode === 'generate' ? 'bg-[#3c3c3c] text-white shadow-sm' : 'text-gray-400 hover:text-gray-200'}\`}
                          >
                             <Sparkles className="w-4 h-4" /> Generate
                          </button>
                          <button
                             type="button"
                             onClick={() => setTaskMode('upscale')}
                             className={\`flex-1 flex items-center justify-center gap-2 text-sm py-2 rounded-[12px] font-medium transition-colors \${taskMode === 'upscale' ? 'bg-[#3c3c3c] text-white shadow-sm' : 'text-gray-400 hover:text-gray-200'}\`}
                          >
                             <Maximize2 className="w-4 h-4" /> Upscale
                          </button>
                      </div>
                    </div>

                    {/* Aspect Ratios */}
                    <div className="flex bg-[#323232] rounded-[16px] overflow-hidden divide-x divide-[#404040]">
                        {[
                          { label: '16:9', icon: <RectangleHorizontal className="w-4 h-4" /> },
                          { label: '4:3', icon: <Frame className="w-4 h-4" /> },
                          { label: '1:1', icon: <Square className="w-4 h-4" /> },
                          { label: '3:4', icon: <Columns className="w-4 h-4 rotate-90" /> },
                          { label: '9:16', icon: <RectangleVertical className="w-4 h-4" /> }
                        ].map(ratio => (
                           <button
                              key={ratio.label}
                              type="button"
                              onClick={() => setAspectRatio(ratio.label)}
                              className={\`flex-1 flex flex-col items-center justify-center gap-1.5 py-3 transition-colors \${aspectRatio === ratio.label ? 'bg-[#4a4a4a] text-white' : 'hover:bg-[#3d3d3d] text-gray-400'}\`}
                           >
                              {ratio.icon}
                              <span className="text-xs font-medium">{ratio.label}</span>
                           </button>
                        ))}
                    </div>

                    {/* Upscale Options */}
                    {taskMode === 'upscale' && (
                       <div className="flex bg-[#323232] rounded-[16px] overflow-hidden divide-x divide-[#404040]">
                           {['2x', '4x', '1080p', '1440p', '2160p'].map(factor => (
                              <button
                                 key={factor}
                                 type="button"
                                 onClick={() => setSettings({...settings, upscaleQuality: factor})}
                                 className={\`flex-1 flex items-center justify-center py-3 text-sm transition-colors \${settings.upscaleQuality === factor ? 'bg-[#4a4a4a] font-semibold text-white' : 'hover:bg-[#3d3d3d] text-gray-400'}\`}
                              >
                                 x{factor.replace('x', '')}
                              </button>
                           ))}
                       </div>
                    )}

                    {/* Quick Setting Adjustments (Hidden by default or minimalist) */}
                     <div className="flex gap-2">
                         <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] flex-1 items-center px-4">
                            <span className="text-gray-400 text-xs w-16">Steps</span>
                            <input type="number" className="flex-1 bg-transparent border-none text-white text-sm outline-none text-right" value={settings.steps} onChange={e => setSettings({...settings, steps: Number(e.target.value)})} />
                         </div>
                         <div className="flex bg-[#2c2c2c] p-1.5 rounded-[12px] flex-1 items-center px-4">
                            <span className="text-gray-400 text-xs w-16">Guidance</span>
                            <input type="number" step="0.5" className="flex-1 bg-transparent border-none text-white text-sm outline-none text-right" value={settings.guidance} onChange={e => setSettings({...settings, guidance: Number(e.target.value)})} />
                         </div>
                     </div>

                    {/* Model Dropdown */}
                    <div className="flex flex-col gap-2">
                       <div className="relative">
                          <input 
                            type="text" 
                            className="w-full bg-[#181818] border border-[#333333] text-sm text-white focus:outline-none placeholder-gray-500 rounded-[12px] px-4 py-3"
                            placeholder="Search & Select model..."
                            value={searchModel}
                            onChange={e => setSearchModel(e.target.value)}
                          />
                          <ChevronDown className="absolute right-4 top-1/2 -translate-y-1/2 w-4 h-4 text-gray-500" />
                       </div>
                       
                       <div className="max-h-48 overflow-y-auto bg-[#181818] rounded-[16px] border border-[#333333]">
                         {filteredModels.map(model => (
                           <button
                             key={model.id}
                             type="button"
                             onClick={() => {
                               setSelectedModel({ id: model.id, name: model.name, task: model.task });
                               setShowModelDropdown(false);
                               setSearchModel("");
                             }}
                             className={\`w-full flex items-center justify-between px-4 py-3 text-left hover:bg-[#2c2c2c] transition-colors \${selectedModel.id === model.id ? 'bg-[#2c2c2c] text-white' : 'text-gray-300'}\`}
                           >
                             <span className="text-sm truncate pr-2 font-medium flex items-center gap-2">
                                <Wand2 className="w-4 h-4 text-yellow-400" /> {model.name}
                             </span>
                               {downloadingModels[model.id] ? (
                                  <div className="flex flex-col items-end gap-1 w-20 relative group">
                                      <div className="flex items-center gap-1">
                                         <X className="w-3.5 h-3.5 text-red-500 flex-shrink-0 cursor-pointer opacity-0 group-hover:opacity-100 hover:scale-110 transition-all absolute left-0" onClick={(e) => handleModelCancel(e, model.id)} />
                                         <Loader2 className="w-3 h-3 text-blue-500 flex-shrink-0 animate-spin group-hover:opacity-0 transition-opacity" />
                                         <span className="text-[10px] text-blue-400 font-mono w-6 text-right">{downloadProgressStates[model.id] || 0}%</span>
                                      </div>
                                      <div className="w-full h-1 bg-[#181818] rounded-full overflow-hidden">
                                           <div className="bg-blue-500 h-full transition-all" style={{ width: \`\${downloadProgressStates[model.id] || 0}%\` }}></div>
                                      </div>
                                  </div>
                               ) : localModels.includes(model.id) ? (
                                  <div
                                     className="p-1 hover:bg-red-500/20 rounded text-green-500 hover:text-red-500 transition-colors group/trash"
                                     onClick={(e) => handleModelDelete(e, model.id)}
                                  >
                                     <CheckCircle2 className="w-4 h-4 block group-hover/trash:hidden" />
                                     <Trash2 className="w-4 h-4 hidden group-hover/trash:block" />
                                  </div>
                               ) : (
                                  <div className="p-1 hover:bg-[#404040] rounded text-gray-500 hover:text-white transition-colors" onClick={(e) => handleModelDownload(e, model.id)}>
                                     <Download className="w-4 h-4" />
                                  </div>
                               )}
                           </button>
                         ))}
                         {filteredModels.length === 0 && (
                           <div className="px-4 py-4 text-sm text-gray-500 text-center font-medium">No models found for this category</div>
                         )}
                       </div>
                    </div>
                  </div>
                )}

                {/* Generate Button inside Pill Wrapper */}
                <div className="ml-1">
                  {isGenerating ? (
                    <button
                      type="button"
                      onClick={handleCancelGeneration}
                      className="w-9 h-9 rounded-full transition-colors flex items-center justify-center bg-gray-200 text-gray-900 shadow-md"
                    >
                      <Square className="w-3.5 h-3.5 fill-current" />
                    </button>
                  ) : (
                    <button
                      type="submit"
                      disabled={(!prompt.trim() && !attachedImage && taskMode !== 'upscale') || !localModels.includes(selectedModel.id)}
                      className={\`w-9 h-9 rounded-full transition-colors flex items-center justify-center shadow-md \${(!prompt.trim() && !attachedImage && taskMode !== 'upscale') || !localModels.includes(selectedModel.id) ? 'bg-[#333333] text-gray-500' : 'bg-gray-200 text-gray-900 hover:bg-white'}\`}
                    >
                      <ArrowUp className="w-5 h-5 font-bold" />
                    </button>
                  )}
                </div>

              </div>
            </div>
          </form>
        </div>
      )}

      {/* Settings Modal Layer */}`;

code = code.replace(match[0], inputAreaReplacement);
fs.writeFileSync('D:/Project/Vison/app/src/App.tsx', code);
console.log("Input area and UI replaced");
