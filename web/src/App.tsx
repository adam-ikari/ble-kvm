import { useState, useEffect, useCallback } from 'react';
import { api } from './api';
import { useSse } from './hooks/useSse';
import { StatusCard } from './components/StatusCard';
import { DeviceList } from './components/DeviceList';
import { PairPanel } from './components/PairPanel';
import { SettingsPanel } from './components/SettingsPanel';
import { WifiPanel } from './components/WifiPanel';
import styles from './styles/App.module.css';

interface Pc { id: number; name: string; connected: boolean; type?: string }

interface Status {
  firmware_version: string;
  active_pc: number;
  pcs: Pc[];
  devices: { keyboard: boolean; mouse: boolean; input_source?: string };
  wifi: {
    mode: string;
    ap_active: boolean;
    sta_connected: boolean;
    sta_ip: string;
    ap_ip: string;
    ap_ssid: string;
    sta_ssid: string;
  };
  input_mode: number;
  usb: { mode: string; connected?: boolean; keyboard_connected?: boolean; mouse_connected?: boolean };
  voice_recording?: boolean;
}

export type { Status };

export default function App() {
  const [authed, setAuthed] = useState(false);
  const [checking, setChecking] = useState(true);
  const [status, setStatus] = useState<Status | null>(null);
  const [error, setError] = useState('');
  const [toastMsg, setToastMsg] = useState<{ text: string; type: 'success' | 'error' | 'info' } | null>(null);

  const toast = useCallback((text: string, type: 'success' | 'error' | 'info') => {
    setToastMsg({ text, type });
    setTimeout(() => setToastMsg(null), 3000);
  }, []);

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
    onAuth: (token: string) => {
      localStorage.setItem('kvm_auth_token', token);
      setAuthed(true);
    },
  });

  /* Check for existing token on mount */
  useEffect(() => {
    const existingToken = localStorage.getItem('kvm_auth_token');
    if (existingToken) {
      /* Try an authenticated request to verify the token is still valid */
      api.status().then(() => {
        setAuthed(true);
        setChecking(false);
      }).catch(() => {
        /* Token invalid, remove it */
        localStorage.removeItem('kvm_auth_token');
        setChecking(false);
      });
    } else {
      setChecking(false);
    }
  }, []);

  useEffect(() => { if (authed) refresh(); }, [authed, refresh]);

  if (checking) {
    return (
      <div className={styles.container}>
        <h1>BLE-KVM</h1>
        <p>Checking authentication...</p>
      </div>
    );
  }

  if (!authed) {
    return (
      <div className={styles.container}>
        <h1>BLE-KVM</h1>
        <p>Double-click the device button to authorize web access.</p>
        <p style={{ color: '#666', fontSize: '14px' }}>Keep this page open — authorization will arrive automatically.</p>
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
      <SettingsPanel status={status} toast={toast} refresh={refresh} />
      {toastMsg && (
        <div className={styles.toast} data-type={toastMsg.type}>
          {toastMsg.text}
        </div>
      )}
    </div>
  );
}
