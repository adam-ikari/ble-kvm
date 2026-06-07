import styles from '../styles/App.module.css';

interface Props {
  devices: { keyboard: boolean; mouse: boolean };
}

export function DeviceList({ devices }: Props) {
  return (
    <div className={styles.card}>
      <h2>Input Devices</h2>
      <div className={styles.deviceItem}>
        <span>Keyboard</span>
        <span>{devices.keyboard ? '🟢 Connected' : '🔴 Disconnected'}</span>
      </div>
      <div className={styles.deviceItem}>
        <span>Mouse</span>
        <span>{devices.mouse ? '🟢 Connected' : '🔴 Disconnected'}</span>
      </div>
    </div>
  );
}
