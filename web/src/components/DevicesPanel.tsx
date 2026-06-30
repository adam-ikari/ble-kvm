import { useState } from 'react';
import { api } from '../api';
import type { Status } from '../App';
import styles from '../styles/App.module.css';

interface Props {
  status: Status | null;
  toast: (text: string, type: 'success' | 'error' | 'info') => void;
}

interface ScanDevice {
  addr: string;
  addr_type: number;
  name: string;
  has_keyboard: boolean;
  has_mouse: boolean;
}

export function DevicesPanel({ status, toast }: Props) {
  const [scanning, setScanning] = useState(false);
  const [results, setResults] = useState<ScanDevice[]>([]);
  const [pairingAddr, setPairingAddr] = useState<string | null>(null);
  const [showPcPair, setShowPcPair] = useState(false);

  const handleScan = async () => {
    setScanning(true);
    setResults([]);
    toast('Scanning for BLE devices...', 'info');
    try {
      await api.scan();
      await new Promise((r) => setTimeout(r, 5000));
      const res = await api.scanResults();
      const devices = Array.isArray(res?.results) ? res.results : [];
      setResults(devices);
      toast(`Found ${devices.length} device(s)`, 'success');
    } catch {
      toast('Scan failed', 'error');
    }
    setScanning(false);
  };

  const handlePair = async (addr: string, addrType: number, type: 'keyboard' | 'mouse') => {
    setPairingAddr(addr);
    try {
      if (type === 'keyboard') await api.pairKeyboard(addr, addrType);
      else await api.pairMouse(addr, addrType);
      toast(`Paired as ${type}`, 'success');
    } catch {
      toast('Pairing failed', 'error');
    }
    setPairingAddr(null);
  };

  const handlePairPc = async () => {
    try {
      await api.pairPc();
      toast('PC pairing started — check your PC\'s Bluetooth settings', 'success');
      setShowPcPair(false);
    } catch {
      toast('PC pairing failed', 'error');
    }
  };

  return (
    <div className={styles.devicesPanel}>
      {/* Current bindings */}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Connected Devices</h3>
        {status && (
          <div className={styles.bindingList}>
            <div className={styles.bindingItem}>
              <span className={styles.bindingIcon}>⌨</span>
              <div className={styles.bindingInfo}>
                <span className={styles.bindingName}>Keyboard</span>
                <span className={styles.bindingStatus}>
                  {status.devices.keyboard ? '● Connected' : '○ Waiting'}
                </span>
              </div>
              <span className={`${styles.bindingDot} ${status.devices.keyboard ? styles.dotGreen : styles.dotDim}`} />
            </div>
            <div className={styles.bindingItem}>
              <span className={styles.bindingIcon}>🖱</span>
              <div className={styles.bindingInfo}>
                <span className={styles.bindingName}>Mouse</span>
                <span className={styles.bindingStatus}>
                  {status.devices.mouse ? '● Connected' : '○ Waiting'}
                </span>
              </div>
              <span className={`${styles.bindingDot} ${status.devices.mouse ? styles.dotGreen : styles.dotDim}`} />
            </div>
          </div>
        )}
      </div>

      {/* Actions */}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Pair New Device</h3>
        <div className={styles.pairActions}>
          <button
            className={`${styles.btnPrimary} ${styles.btnWithIcon}`}
            onClick={handleScan}
            disabled={scanning}
          >
            <span className={styles.btnIcon}>◎</span>
            {scanning ? 'Scanning...' : 'Scan BLE Devices'}
          </button>
          <button
            className={`${styles.btnSecondary} ${styles.btnWithIcon}`}
            onClick={() => setShowPcPair(!showPcPair)}
          >
            <span className={styles.btnIcon}>＋</span>
            Pair New PC
          </button>
        </div>

        {showPcPair && (
          <div className={styles.pcPairPrompt}>
            <p>Put your PC into Bluetooth pairing mode, then click below. The BLE-KVM will appear as a keyboard/mouse.</p>
            <button className={styles.btnPrimary} onClick={handlePairPc}>
              Start PC Pairing
            </button>
          </div>
        )}

        {/* Scan progress */}
        {scanning && (
          <div className={styles.scanProgress}>
            <div className={styles.spinner} />
            <span>Scanning for nearby BLE devices (5s)...</span>
          </div>
        )}

        {/* Scan results */}
        {results.length > 0 && (
          <div className={styles.resultsList}>
            {results.map((d, i) => (
              <div key={i} className={styles.resultItem}>
                <div className={styles.resultInfo}>
                  <span className={styles.resultName}>{d.name || 'Unknown Device'}</span>
                  <span className={styles.resultAddr}>{d.addr}</span>
                </div>
                <div className={styles.resultActions}>
                  {d.has_keyboard && (
                    <button
                      className={styles.resultBtn}
                      disabled={pairingAddr === d.addr}
                      onClick={() => handlePair(d.addr, d.addr_type, 'keyboard')}
                      title="Pair as Keyboard"
                    >
                      ⌨
                    </button>
                  )}
                  {d.has_mouse && (
                    <button
                      className={styles.resultBtn}
                      disabled={pairingAddr === d.addr}
                      onClick={() => handlePair(d.addr, d.addr_type, 'mouse')}
                      title="Pair as Mouse"
                    >
                      🖱
                    </button>
                  )}
                  {!d.has_keyboard && !d.has_mouse && (
                    <span className={styles.resultNoHid}>No HID</span>
                  )}
                </div>
              </div>
            ))}
          </div>
        )}
      </div>
    </div>
  );
}
