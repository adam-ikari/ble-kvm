import { useEffect, useRef, useCallback } from 'react';
import { getToken } from '../api';

interface SseCallbacks {
  onSwitch?: (activePc: number) => void;
  onConnection?: (data: Record<string, string>) => void;
  onDevice?: (data: Record<string, string>) => void;
}

export function useSse(callbacks: SseCallbacks) {
  const esRef = useRef<EventSource | null>(null);
  const callbacksRef = useRef(callbacks);
  callbacksRef.current = callbacks;

  const connect = useCallback(() => {
    const token = getToken();
    if (!token) return;
    const es = new EventSource(`/api/events?token=${encodeURIComponent(token)}`);
    es.addEventListener('switch', (e) => {
      const data = JSON.parse((e as MessageEvent).data);
      callbacksRef.current.onSwitch?.(data.active_pc);
    });
    es.addEventListener('connection', (e) => {
      const data = JSON.parse((e as MessageEvent).data);
      callbacksRef.current.onConnection?.(data);
    });
    es.addEventListener('device', (e) => {
      const data = JSON.parse((e as MessageEvent).data);
      callbacksRef.current.onDevice?.(data);
    });
    es.onerror = () => {
      es.close();
      setTimeout(connect, 3000);
    };
    esRef.current = es;
  }, []);

  useEffect(() => {
    connect();
    return () => esRef.current?.close();
  }, [connect]);
}
