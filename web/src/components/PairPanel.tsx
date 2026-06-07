import { useState } from 'react';
import { api } from '../api';
import styles from '../styles/App.module.css';

export function PairPanel() {
  const [scanning, setScanning] = useState(false);
  const [results, setResults] = useState<any[]>([]);

  const handleScan = async () => {
    setScanning(true);
    await api.scan();
    await new Promise((r) => setTimeout(r, 5000));
    const res = await api.scanResults();
    setResults(res);
    setScanning(false);
  };

  return (
    <div className={styles.card}>
      <h2>Pair Devices</h2>
      <button onClick={handleScan} disabled={scanning}>
        {scanning ? 'Scanning...' : 'Scan BLE Devices'}
      </button>
      <button onClick={() => api.pairPc()}>Pair New PC</button>
      {results.length > 0 && (
        <ul className={styles.scanResults}>
          {results.map((d, i) => (
            <li key={i}>
              {d.name || 'Unknown'} ({d.addr})
              <button onClick={() => d.has_keyboard && api.pairKeyboard(d.addr)}>Keyboard</button>
              <button onClick={() => d.has_mouse && api.pairMouse(d.addr)}>Mouse</button>
            </li>
          ))}
        </ul>
      )}
    </div>
  );
}
