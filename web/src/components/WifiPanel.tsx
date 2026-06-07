import { useState } from 'react';
import { api } from '../api';
import styles from '../styles/App.module.css';

interface WifiInfo {
  mode: string;
  ap_active: boolean;
  sta_connected: boolean;
  sta_ip: string;
  ap_ip: string;
  ap_ssid: string;
  sta_ssid: string;
}

interface Props {
  wifi: WifiInfo | null;
  onRefresh: () => void;
}

export function WifiPanel({ wifi, onRefresh }: Props) {
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');

  const handleConnect = async () => {
    await api.wifi({ ssid, password });
    setSsid('');
    setPassword('');
    onRefresh();
  };

  const handleDisconnect = async () => {
    await api.wifi({ disconnect_sta: true });
    onRefresh();
  };

  const handleModeChange = async (mode: string) => {
    await api.wifi({ mode });
    onRefresh();
  };

  const modeLabel: Record<string, string> = {
    ap: 'AP',
    sta: 'STA',
    apsta: 'AP+STA',
    off: 'Off',
  };

  return (
    <div className={styles.card}>
      <h2>Wi-Fi</h2>

      {wifi && (
        <>
          <div className={styles.wifiStatus}>
            <div className={styles.deviceItem}>
              <span>Mode</span>
              <span>{modeLabel[wifi.mode] || wifi.mode}</span>
            </div>
            {wifi.ap_active && (
              <div className={styles.deviceItem}>
                <span>AP: {wifi.ap_ssid}</span>
                <span>{wifi.ap_ip}</span>
              </div>
            )}
            {wifi.sta_connected && (
              <div className={styles.deviceItem}>
                <span>STA: {wifi.sta_ssid}</span>
                <span>🟢 {wifi.sta_ip}</span>
              </div>
            )}
            {!wifi.sta_connected && wifi.sta_ssid && (
              <div className={styles.deviceItem}>
                <span>STA: {wifi.sta_ssid}</span>
                <span>🔴 Disconnected</span>
              </div>
            )}
          </div>

          <div className={styles.modeButtons}>
            <button
              className={wifi.mode === 'ap' ? styles.activeMode : ''}
              onClick={() => handleModeChange('ap')}
            >AP Only</button>
            <button
              className={wifi.mode === 'apsta' ? styles.activeMode : ''}
              onClick={() => handleModeChange('apsta')}
            >AP+STA</button>
            <button
              className={wifi.mode === 'sta' ? styles.activeMode : ''}
              onClick={() => handleModeChange('sta')}
            >STA Only</button>
            <button
              className={wifi.mode === 'off' ? styles.activeMode : ''}
              onClick={() => handleModeChange('off')}
            >Off</button>
          </div>

          {wifi.sta_connected && (
            <button onClick={handleDisconnect}>Disconnect STA</button>
          )}
        </>
      )}

      <div className={styles.wifiForm}>
        <input value={ssid} onChange={(e) => setSsid(e.target.value)} placeholder="SSID" />
        <input value={password} onChange={(e) => setPassword(e.target.value)}
               placeholder="Password" type="password" />
        <button onClick={handleConnect}>Connect</button>
      </div>
    </div>
  );
}
