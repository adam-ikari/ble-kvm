import { useState, useEffect, useCallback, useRef } from 'react';
import { api } from './api';
import { useSse } from './hooks/useSse';
import { Dashboard } from './components/Dashboard';
import { DevicesPanel } from './components/DevicesPanel';
import { SettingsPanel } from './components/SettingsPanel';
import { NetworkPanel } from './components/NetworkPanel';
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

type Tab = 'dashboard' | 'devices' | 'settings' | 'network';

const TABS: { key: Tab; label: string }[] = [
  { key: 'dashboard', label: 'Dashboard' },
  { key: 'devices', label: 'Devices' },
  { key: 'settings', label: 'Settings' },
  { key: 'network', label: 'Network' },
];

export default function App() {
  const [authed, setAuthed] = useState(false);
  const authedRef = useRef(false);
  const [status, setStatus] = useState<Status | null>(null);
  const [activeTab, setActiveTab] = useState<Tab>('dashboard');
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
    onConnection: () => { if (authedRef.current) refresh(); },
    onDevice: () => refresh(),
    onAuth: (token: string) => {
      localStorage.setItem('kvm_auth_token', token);
      authedRef.current = true;
      setAuthed(true);
    },
  });

  /* On mount: check if we already have a valid token */
  useEffect(() => {
    const existingToken = localStorage.getItem('kvm_auth_token');
    if (existingToken) {
      api.status().then((s) => {
        setStatus(s);
        authedRef.current = true;
        setAuthed(true);
      }).catch(() => {
        localStorage.removeItem('kvm_auth_token');
      });
    }
  }, []);

  /* When authed becomes true, fetch full status */
  useEffect(() => {
    if (authed && !status) refresh();
  }, [authed, status, refresh]);

  /* Unauthed — show prompt */
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
      {error && <p style={{ color: 'red' }}>{error}</p>}

      <nav className={styles.tabNav}>
        {TABS.map((t) => (
          <button
            key={t.key}
            className={`${styles.tab} ${activeTab === t.key ? styles.tabActive : ''}`}
            onClick={() => setActiveTab(t.key)}
          >
            {t.label}
          </button>
        ))}
      </nav>

      {activeTab === 'dashboard' && <Dashboard status={status} toast={toast} refresh={refresh} />}
      {activeTab === 'devices' && <DevicesPanel status={status} toast={toast} />}
      {activeTab === 'settings' && <SettingsPanel status={status} toast={toast} refresh={refresh} />}
      {activeTab === 'network' && <NetworkPanel status={status} toast={toast} refresh={refresh} />}

      {toastMsg && (
        <div className={styles.toast} data-type={toastMsg.type}>
          {toastMsg.text}
        </div>
      )}
    </div>
  );
}
