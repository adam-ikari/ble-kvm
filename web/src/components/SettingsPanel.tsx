import { useState, useEffect } from 'react';
import { api } from '../api';
import type { Status } from '../App';
import styles from '../styles/App.module.css';

interface Props {
  status: Status | null;
  toast: (text: string, type: 'success' | 'error' | 'info') => void;
  refresh: () => void;
}

export function SettingsPanel({ status, toast, refresh }: Props) {
  const [settings, setSettings] = useState<Record<string, any> | null>(null);
  const [loading, setLoading] = useState('');
  const [showDanger, setShowDanger] = useState(false);

  useEffect(() => {
    api.settings.get().then(setSettings).catch(() => {});
  }, []);

  const update = async (data: object, label: string) => {
    setLoading(label);
    try {
      await api.settings.update(data);
      toast(`${label} updated`, 'success');
      refresh();
      const s = await api.settings.get();
      setSettings(s);
    } catch { toast(`${label} failed`, 'error'); }
    setLoading('');
  };

  if (!settings) {
    return <div className={styles.loading}><div className={styles.spinnerLarge} /></div>;
  }

  const hasBattery = settings.screen_off_timeout_sec !== undefined;

  return (
    <div className={styles.settingsPanel}>
      {/* Device Name */}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Device Name</h3>
        <p className={styles.sectionDesc}>BLE broadcast name (default: KVM-XXXX)</p>
        <div className={styles.inlineRow}>
          <input
            type="text"
            className={styles.textInput}
            value={settings.device_name || ''}
            onChange={(e) => setSettings({ ...settings, device_name: e.target.value })}
            placeholder="KVM-XXXX"
            maxLength={31}
          />
          <button
            className={styles.btnPrimary}
            disabled={loading === 'Name'}
            onClick={() => update({ device_name: settings.device_name }, 'Name')}
          >
            Save
          </button>
        </div>
      </div>

      {/* Anti-Idle */}
      <div className={styles.section}>
        <h3 className={styles.sectionTitle}>Anti-Idle</h3>
        <p className={styles.sectionDesc}>Periodically nudge the mouse to prevent screen lock on all connected PCs</p>
        <div className={styles.toggleRow}>
          <div>
            <span className={styles.toggleLabel}>Anti-Idle</span>
            <span className={styles.toggleStatus}>{settings.anti_idle ? '● Enabled' : '○ Disabled'}</span>
          </div>
          <button
            className={`${styles.toggle} ${settings.anti_idle ? styles.toggleOn : ''}`}
            disabled={loading === 'Anti-Idle'}
            onClick={() => update({ anti_idle: !settings.anti_idle }, 'Anti-Idle')}
          >
            <span className={styles.toggleKnob} />
          </button>
        </div>
        {settings.anti_idle && (
          <div className={styles.sliderRow}>
            <span>Interval: {settings.anti_idle_interval}s</span>
            <input
              type="range"
              min={10}
              max={600}
              step={10}
              value={settings.anti_idle_interval}
              onChange={(e) => {
                const val = parseInt(e.target.value);
                setSettings({ ...settings, anti_idle_interval: val });
              }}
              onMouseUp={() => update({ anti_idle_interval: settings.anti_idle_interval }, 'Interval')}
            />
          </div>
        )}
      </div>

      {/* Sleep settings (battery only) */}
      {hasBattery && (
        <>
          <div className={styles.section}>
            <h3 className={styles.sectionTitle}>Screen Off</h3>
            <p className={styles.sectionDesc}>Turn off display after inactivity (0 = never)</p>
            <div className={styles.inlineRow}>
              <input
                type="number"
                className={styles.textInput}
                min={0}
                max={3600}
                value={settings.screen_off_timeout_sec}
                onChange={(e) => setSettings({ ...settings, screen_off_timeout_sec: parseInt(e.target.value) || 0 })}
              />
              <span className={styles.unitLabel}>seconds</span>
              <button
                className={styles.btnPrimary}
                disabled={loading === 'Screen Off'}
                onClick={() => update({ screen_off_timeout_sec: settings.screen_off_timeout_sec }, 'Screen Off')}
              >
                Save
              </button>
            </div>
          </div>

          <div className={styles.section}>
            <h3 className={styles.sectionTitle}>Sleep</h3>
            <p className={styles.sectionDesc}>Enter low-power sleep after inactivity (0 = never)</p>
            <div className={styles.inlineRow}>
              <input
                type="number"
                className={styles.textInput}
                min={0}
                max={7200}
                value={settings.sleep_timeout_sec}
                onChange={(e) => setSettings({ ...settings, sleep_timeout_sec: parseInt(e.target.value) || 0 })}
              />
              <span className={styles.unitLabel}>seconds</span>
              <button
                className={styles.btnPrimary}
                disabled={loading === 'Sleep'}
                onClick={() => update({ sleep_timeout_sec: settings.sleep_timeout_sec }, 'Sleep')}
              >
                Save
              </button>
            </div>
          </div>
        </>
      )}

      {/* Danger Zone */}
      <div className={styles.section}>
        <button
          className={styles.dangerToggle}
          onClick={() => setShowDanger(!showDanger)}
        >
          {showDanger ? '▾' : '▸'} Danger Zone
        </button>
        {showDanger && (
          <div className={styles.dangerZone}>
            <button
              className={styles.btnDanger}
              disabled={loading === 'Reset'}
              onClick={() => {
                if (window.confirm('Factory reset? This will erase ALL settings and reboot the device.')) {
                  update({ factory_reset: true }, 'Reset');
                }
              }}
            >
              Factory Reset
            </button>
          </div>
        )}
      </div>
    </div>
  );
}
