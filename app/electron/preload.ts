import { contextBridge, ipcRenderer } from 'electron';

contextBridge.exposeInMainWorld('vison', {
  request: (path: string, init?: { method?: string; headers?: Record<string, string>; body?: string }) =>
    ipcRenderer.invoke('backend:request', { path, init }),
  notices: () => ipcRenderer.invoke('app:notices'),
  auth: {
    signIn: () => ipcRenderer.invoke('auth:signIn'),
    signOut: () => ipcRenderer.invoke('auth:signOut'),
    status: () => ipcRenderer.invoke('auth:status'),
  },
  chat: {
    save: (chat: any) => ipcRenderer.invoke('chat:save', chat),
    load: (id: string) => ipcRenderer.invoke('chat:load', id),
    list: () => ipcRenderer.invoke('chat:list'),
    delete: (id: string) => ipcRenderer.invoke('chat:delete', id),
    search: (query: string) => ipcRenderer.invoke('chat:search', query)
  }
});

window.addEventListener('DOMContentLoaded', () => {
    // preload bridge initialized
});