import { useState } from 'react';
import { api } from '../api';
import styles from '../styles/App.module.css';

export function WifiPanel() {
  const [ssid, setSsid] = useState('');
  const [password, setPassword] = useState('');

  const handleConnect = async () => {
    await api.wifi(ssid, password);
  };

  return (
    <div className={styles.card}>
      <h2>Wi-Fi</h2>
      <input value={ssid} onChange={(e) => setSsid(e.target.value)} placeholder="SSID" />
      <input value={password} onChange={(e) => setPassword(e.target.value)}
             placeholder="Password" type="password" />
      <button onClick={handleConnect}>Connect</button>
    </div>
  );
}
