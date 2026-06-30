import { useState } from 'react';
import { api } from '../api';
import type { Status } from '../App';
import styles from '../styles/App.module.css';

interface Props {
  status: Status | null;
  toast: (text: string, type: 'success' | 'error' | 'info') => void;
  refresh: () => void;
}

const MODE_LABELS: Record<string, string> = {
  ap: 'Access Point',
  sta: 'Station',
  apsta: 'AP + STA',
  off: 'Off',
};

const MODE_SHORT: Record<string, string> = {
  ap: 'AP',
  sta: 'STA',
  apsta: 'AP+STA',
  off: 'Off',
};

export function NetworkPanel({ status, toast, refresh }: Props) {
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');
  const [loading, setLoading] = useState('');

  if (!status) return null;

  const wifi = status.wifi;

  const handleMode = async (mode: string) => {
    setLoading(`mode-${mode}`);
    try {
      await api.wifi({ mode });
      toast(`WiFi mode: ${MODE_SHORT[mode] || mode}`, 'success');
      refresh();
    } catch { toast('Mode change failed', 'error'); }
    setLoading('');
  };

  const handleConnect = async () => {
    if (!ssid.trim()) { toast('Enter a WiFi SSID', 'error'); return; }
    setLoading('connect');
    try {
      await api.wifi({ ssid: ssid.trim(), password });
      toast(`Connecting to ${ssid}...`, 'info');
      setSsid(''); setPassword('');
      setTimeout(refresh, 4000);
    } catch { toast('Connection failed', 'error'); }
    setLoading('');
  };

  const handleDisconnect = async () => {
    setLoading('disconnect');
    try {
      await api.wifi({ disconnect_sta: true });
      toast('Disconnected from WiFi', 'info');
      refresh();
    } catch { toast('Disconnect failed', 'error'); }
    setLoading('');
  };

  return (
    <div className={styles.networkPanel}>
      {/* Current status */}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>WiFi Status</h3>
        <div className={styles.networkStatus}>
          <div className={styles.networkMain}>
            <span className={`${styles.networkMode} ${wifi.mode !== 'off' ? styles.networkActive : ''}`}>
              {MODE_LABELS[wifi.mode] || wifi.mode}
            </span>
          </div>

          {wifi.ap_active && (
            <div className={styles.networkRow}>
              <span className={styles.networkLabel}>AP</span>
              <div className={styles.networkDetail}>
                <span className={styles.networkSsid}>{wifi.ap_ssid}</span>
                <span className={styles.networkIp}>{wifi.ap_ip}</span>
              </div>
              <span className={styles.dotGreen} style={{display: 'inline-block', width: 8, height: 8, borderRadius: '50%'}} />
            </div>
          )}

          {wifi.sta_connected && (
            <div className={styles.networkRow}>
              <span className={styles.networkLabel}>STA</span>
              <div className={styles.networkDetail}>
                <span className={styles.networkSsid}>{wifi.sta_ssid}</span>
                <span className={styles.networkIp}>{wifi.sta_ip}</span>
              </div>
              <span className={styles.dotGreen} style={{display: 'inline-block', width: 8, height: 8, borderRadius: '50%'}} />
            </div>
          )}

          {!wifi.sta_connected && wifi.sta_ssid && (
            <div className={styles.networkRow}>
              <span className={styles.networkLabel}>STA</span>
              <div className={styles.networkDetail}>
                <span className={styles.networkSsid}>{wifi.sta_ssid}</span>
                <span className={styles.networkStatus}>Disconnected</span>
              </div>
              <span className={styles.dotRed} style={{display: 'inline-block', width: 8, height: 8, borderRadius: '50%'}} />
            </div>
          )}
        </div>
      </div>

      {/* Mode selector */}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Mode</h3>
        <div className={styles.modeGrid}>
          {(['ap', 'apsta', 'sta', 'off'] as const).map((m) => (
            <button
              key={m}
              className={`${styles.modeBtn} ${wifi.mode === m ? styles.modeBtnActive : ''}`}
              disabled={loading === `mode-${m}`}
              onClick={() => handleMode(m)}
            >
              <span className={styles.modeBtnIcon}>
                {m === 'ap' ? '◎' : m === 'sta' ? '◉' : m === 'apsta' ? '◈' : '○'}
              </span>
              <span>{MODE_SHORT[m]}</span>
            </button>
          ))}
        </div>
      </div>

      {/* Connect to WiFi */}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Connect to Network</h3>
        <div className={styles.connectForm}>
          <input
            className={styles.input}
            value={ssid}
            onChange={(e) => setSsid(e.target.value)}
            placeholder="WiFi SSID"
          />
          <input
            className={styles.input}
            value={password}
            onChange={(e) => setPassword(e.target.value)}
            placeholder="Password"
            type="password"
          />
          <button
            className={styles.btnPrimary}
            disabled={loading === 'connect'}
            onClick={handleConnect}
          >
            {loading === 'connect' ? 'Connecting...' : 'Connect'}
          </button>
        </div>

        {wifi.sta_connected && (
          <button
            className={styles.btnDanger}
            disabled={loading === 'disconnect'}
            onClick={handleDisconnect}
            style={{marginTop: 8, width: '100%'}}
          >
            Disconnect from {wifi.sta_ssid}
          </button>
        )}
      </div>
    </div>
  );
}
