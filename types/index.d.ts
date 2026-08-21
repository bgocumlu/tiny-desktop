export interface TinyAPI {
  app: {
    getDataPath(): Promise<string>;
  };
  data: {
    read<T = unknown>(store: string): Promise<T | null>;
    write(store: string, value: unknown): Promise<void>;
    remove(store: string): Promise<void>;
  };
  window: {
    close(): Promise<void>;
    minimize(): Promise<void>;
    maximize(): Promise<void>;
    restore(): Promise<void>;
  };
  shell: {
    openExternal(url: string): Promise<void>;
  };
}

declare global {
  interface Window {
    tiny: TinyAPI;
  }
}
