import { api } from '../api';
import type { Status } from '../App';
import styles from '../styles/App.module.css';

interface Props {
  status: Status | null;
  toast: (text: string, type: 'success' | 'error' | 'info') => void;
  refresh: () => void;
}

export function Dashboard({ status, toast, refresh }: Props) {
  if (!status) {
    return <div className={styles.loading}><div className={styles.spinnerLarge} /></div>;
  }

  const connectedCount = status.pcs.filter(p => p.connected).length;
  const activePc = status.pcs.find(p => p.id === status.active_pc);

  const handleSwitch = async () => {
    try {
      await api.switchPc();
      refresh();
      toast('Switched to next PC', 'success');
    } catch {
      toast('Switch failed', 'error');
    }
  };

  const getPcIcon = (pc: typeof status.pcs[0]) => {
    if (!pc.connected) return '○';
    if (pc.id === status.active_pc) return '●';
    return '◉';
  };

  const getPcLabel = (pc: typeof status.pcs[0]) => {
    if (pc.type === 'usb') return 'USB';
    return 'BLE';
  };

  return (
    <div className={styles.dashboard}>
      {/* Active PC card */}
      <div className={styles.activePcCard}>
        <div className={styles.activePcInfo}>
          <span className={styles.activePcLabel}>ACTIVE PC</span>
          <span className={styles.activePcName}>
            {activePc?.name || `PC${status.active_pc}`}
          </span>
          <span className={styles.activePcConn}>
            {activePc?.connected ? '● Connected' : '○ Disconnected'}
          </span>
        </div>
        <button className={styles.switchBtn} onClick={handleSwitch}>
          <span className={styles.switchIcon}>↻</span>
          Switch
        </button>
      </div>

      {/* PC grid */}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Connected PCs</h3>
        <div className={styles.pcGrid}>
          {status.pcs.map((pc) => {
            const isActive = pc.id === status.active_pc;
            return (
              <div
                key={pc.id}
                className={`${styles.pcCard} ${isActive ? styles.pcCardActive : ''} ${pc.connected ? styles.pcCardOnline : styles.pcCardOffline}`}
              >
                <span className={styles.pcCardIcon}>{getPcIcon(pc)}</span>
                <span className={styles.pcCardName}>PC {pc.id}</span>
                <span className={styles.pcCardLabel}>{pc.name || '--'}</span>
                <span className={styles.pcCardType}>{getPcLabel(pc)}</span>
                {isActive && <span className={styles.pcCardBadge}>Active</span>}
              </div>
            );
          })}
        </div>
      </div>

      {/* Status overview */}
      <div className={styles.statsGrid}>
        <div className={styles.statCard}>
          <span className={styles.statIcon}>⌨</span>
          <div className={styles.statInfo}>
            <span className={styles.statValue}>{status.devices.keyboard ? 'Connected' : 'Disconnected'}</span>
            <span className={styles.statLabel}>Keyboard</span>
          </div>
          <span className={`${styles.statDot} ${status.devices.keyboard ? styles.dotGreen : styles.dotRed}`} />
        </div>
        <div className={styles.statCard}>
          <span className={styles.statIcon}>🖱</span>
          <div className={styles.statInfo}>
            <span className={styles.statValue}>{status.devices.mouse ? 'Connected' : 'Disconnected'}</span>
            <span className={styles.statLabel}>Mouse</span>
          </div>
          <span className={`${styles.statDot} ${status.devices.mouse ? styles.dotGreen : styles.dotRed}`} />
        </div>
        <div className={styles.statCard}>
          <span className={styles.statIcon}>◈</span>
          <div className={styles.statInfo}>
            <span className={styles.statValue}>{status.wifi.mode.toUpperCase()}</span>
            <span className={styles.statLabel}>WiFi Mode</span>
          </div>
          <span className={`${styles.statDot} ${status.wifi.sta_connected || status.wifi.ap_active ? styles.dotGreen : styles.dotRed}`} />
        </div>
        <div className={styles.statCard}>
          <span className={styles.statIcon}>⊡</span>
          <div className={styles.statInfo}>
            <span className={styles.statValue}>{status.input_mode === 0 ? 'KVM' : 'PPT'}</span>
            <span className={styles.statLabel}>Input Mode</span>
          </div>
          <span className={styles.statDot} style={{background: '#1a73e8'}} />
        </div>
      </div>

      {/* Quick info */}
      <div className={styles.infoBar}>
        <div className={styles.infoItem}>
          <span className={styles.infoValue}>{status.devices.input_source?.toUpperCase() || 'BLE'}</span>
          <span className={styles.infoLabel}>Input Source</span>
        </div>
        <div className={styles.infoItem}>
          <span className={styles.infoValue}>{status.firmware_version}</span>
          <span className={styles.infoLabel}>Firmware</span>
        </div>
        <div className={styles.infoItem}>
          <span className={styles.infoValue}>{connectedCount}/3</span>
          <span className={styles.infoLabel}>PCs Online</span>
        </div>
        <div className={styles.infoItem}>
          <span className={styles.infoValue}>{status.usb.mode}</span>
          <span className={styles.infoLabel}>USB Mode</span>
        </div>
      </div>

      {/* Voice indicator */}
      {status.voice_recording && (
        <div className={styles.voiceBar}>
          <span className={styles.voiceDot} />
          <span>Voice recording in progress...</span>
        </div>
      )}
    </div>
  );
}
