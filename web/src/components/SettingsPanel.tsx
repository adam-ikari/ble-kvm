import { api } from '../api';
import styles from '../styles/App.module.css';

interface Props {
  onRefresh: () => void;
}

export function SettingsPanel({ onRefresh }: Props) {
  const handleRegenToken = async () => {
    await api.settings.update({ regenerate_token: true });
    onRefresh();
  };

  return (
    <div className={styles.card}>
      <h2>Settings</h2>
      <button onClick={handleRegenToken}>Regenerate Auth Token</button>
    </div>
  );
}
