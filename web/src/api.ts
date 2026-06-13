const API_BASE = '';

function getToken(): string | null {
  try {
    return localStorage.getItem('kvm_auth_token');
  } catch {
    return null;
  }
}

async function request(path: string, options?: RequestInit) {
  const token = getToken();
  const headers: Record<string, string> = {
    'Content-Type': 'application/json',
    ...((options?.headers as Record<string, string>) || {}),
  };
  if (token) {
    headers['Authorization'] = `Bearer ${token}`;
  }
  const res = await fetch(`${API_BASE}${path}`, { ...options, headers });
  if (!res.ok) throw new Error(`HTTP ${res.status}`);
  return res.json();
}

export const api = {
  authCheck: () => request('/api/auth-check'),
  status: () => request('/api/status'),
  switchPc: () => request('/api/switch', { method: 'POST' }),
  scan: () => request('/api/scan', { method: 'POST' }),
  scanResults: () => request('/api/scan/results'),
  pairKeyboard: (address: string, addr_type: number) => request('/api/pairings', {
    method: 'POST', body: JSON.stringify({ type: 'device', role: 'keyboard', address, addr_type }),
  }),
  pairMouse: (address: string, addr_type: number) => request('/api/pairings', {
    method: 'POST', body: JSON.stringify({ type: 'device', role: 'mouse', address, addr_type }),
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
  deletePairing: (id: number) => request(`/api/pairings/${id}`, { method: 'DELETE' }),
};
