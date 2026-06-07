const API_BASE = '';

let token = localStorage.getItem('ble-kvm-token') || '';

export function setToken(t: string) {
  token = t;
  localStorage.setItem('ble-kvm-token', t);
}

export function getToken(): string {
  return token;
}

async function request(path: string, options?: RequestInit) {
  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    ...((options?.headers as Record<string, string>) || {}),
  };
  if (token) {
    headers['Authorization'] = `Bearer ${token}`;
  }
  const res = await fetch(`${API_BASE}${path}`, { ...options, headers });
  if (res.status === 401) throw new Error('Unauthorized');
  return res.json();
}

export const api = {
  status: () => request('/api/status'),
  switchPc: () => request('/api/switch', { method: 'POST' }),
  scan: () => request('/api/scan', { method: 'POST' }),
  scanResults: () => request('/api/scan/results'),
  pairKeyboard: (addr: string) => request('/api/pair/keyboard', {
    method: 'POST', body: JSON.stringify({ addr }),
  }),
  pairMouse: (addr: string) => request('/api/pair/mouse', {
    method: 'POST', body: JSON.stringify({ addr }),
  }),
  pairPc: () => request('/api/pair/pc', { method: 'POST' }),
  devices: () => request('/api/devices'),
  wifi: (data: { ssid?: string; password?: string; mode?: string; disconnect_sta?: boolean }) =>
    request('/api/wifi', { method: 'POST', body: JSON.stringify(data) }),
  settings: {
    get: () => request('/api/settings'),
    update: (data: object) => request('/api/settings', {
      method: 'POST', body: JSON.stringify(data),
    }),
  },
};
