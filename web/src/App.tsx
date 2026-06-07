import { useState, useEffect, useCallback } from 'react';
import { api, setToken, getToken } from './api';
import { useSse } from './hooks/useSse';
import { StatusCard } from './components/StatusCard';
import { DeviceList } from './components/DeviceList';
import { PairPanel } from './components/PairPanel';
import { SettingsPanel } from './components/SettingsPanel';
import { WifiPanel } from './components/WifiPanel';
import styles from './styles/App.module.css';

interface Pc { id: number; name: string; connected: boolean }

interface Status {
  firmware_version: string;
  active_pc: number;
  pcs: Pc[];
  devices: { keyboard: boolean; mouse: boolean };
  wifi: {
    mode: string;
    ap_active: boolean;
    sta_connected: boolean;
    sta_ip: string;
    ap_ip: string;
    ap_ssid: string;
    sta_ssid: string;
  };
}

export default function App() {
  const [authed, setAuthed] = useState(!!getToken());
  const [tokenInput, setTokenInput] = useState('');
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState('');

  const refresh = useCallback(async () => {
    try {
      const s = await api.status();
      setStatus(s);
      setError('');
    } catch {
      setError('Failed to fetch status');
    }
  }, []);

  useSse({
    onSwitch: (activePc) => setStatus((s) => s ? { ...s, active_pc: activePc } : s),
    onConnection: () => refresh(),
    onDevice: () => refresh(),
  });

  useEffect(() => { if (authed) refresh(); }, [authed, refresh]);

  if (!authed) {
    return (
      <div className={styles.container}>
        <h1>BLE-KVM</h1>
        <p>Enter authentication token:</p>
        <input value={tokenInput} onChange={(e) => setTokenInput(e.target.value)}
               placeholder="Token" type="password" />
        <button onClick={() => {
          setToken(tokenInput);
          api.status().then(() => { setAuthed(true); setError(''); }).catch(() => setError('Invalid token'));
        }}>Login</button>
        {error && <p className={styles.error}>{error}</p>}
      </div>
    );
  }

  return (
    <div className={styles.container}>
      <h1>BLE-KVM</h1>
      {status && <StatusCard status={status} onSwitch={() => api.switchPc().then(refresh)} />}
      {status && <DeviceList devices={status.devices} />}
      <PairPanel />
      <WifiPanel wifi={status?.wifi ?? null} onRefresh={refresh} />
      <SettingsPanel onRefresh={refresh} />
    </div>
  );
}
