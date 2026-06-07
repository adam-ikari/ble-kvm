import styles from '../styles/App.module.css';

interface Pc { id: number; name: string; connected: boolean }

interface Props {
  status: { firmware_version: string; active_pc: number; pcs: Pc[]; wifi: { sta_ip: string; ap_ip: string; ap_ssid: string; mode: string } };
  onSwitch: () => void;
}

export function StatusCard({ status, onSwitch }: Props) {
  return (
    <div className={styles.card}>
      <h2>Status</h2>
      <p>Firmware: {status.firmware_version}</p>
      <p>IP: {status.wifi.sta_ip !== '0.0.0.0' ? status.wifi.sta_ip : status.wifi.ap_ip}</p>
      <p>Active: PC{status.active_pc}</p>
      <div className={styles.pcList}>
        {status.pcs.map((pc) => (
          <div key={pc.id} className={`${styles.pcItem} ${pc.connected ? styles.connected : styles.disconnected}`}>
            <span>PC{pc.id}: {pc.name || 'Unnamed'}</span>
            <span>{pc.connected ? '🟢' : '🔴'}</span>
          </div>
        ))}
      </div>
      <button onClick={onSwitch}>Switch PC</button>
    </div>
  );
}
